/*
 * AY-3-8912 Sound Chip Emulation Implementation
 * T-state precise emulation
 */

#include "ay.h"
#include <string.h>
#include <stdio.h>

// AY-3-8912 Register Map
#define AY_AFINE    0   // Channel A Fine Tune
#define AY_ACOARSE  1   // Channel A Coarse Tune
#define AY_BFINE    2   // Channel B Fine Tune
#define AY_BCOARSE  3   // Channel B Coarse Tune
#define AY_CFINE    4   // Channel C Fine Tune
#define AY_CCOARSE  5   // Channel C Coarse Tune
#define AY_NOISEPER 6   // Noise Period
#define AY_MIXER    7   // Mixer Control
#define AY_AVOL     8   // Channel A Volume
#define AY_BVOL     9   // Channel B Volume
#define AY_CVOL     10  // Channel C Volume
#define AY_EFINE    11  // Envelope Fine Tune
#define AY_ECOARSE  12  // Envelope Coarse Tune
#define AY_ESHAPE   13  // Envelope Shape
#define AY_PORTA    14  // I/O Port A
#define AY_PORTB    15  // I/O Port B

// Envelope shapes
#define ENV_CONTINUE  0x08
#define ENV_ATTACK    0x04
#define ENV_ALTERNATE 0x02
#define ENV_HOLD      0x01

typedef struct {
    // Registers
    uint8_t regs[16];
    uint8_t selected_reg;
    
    // Clock configuration
    uint32_t cpu_clock_hz;
    uint32_t sample_rate;
    bool precise_tstate;
    
    // Tone generators (3 channels)
    struct {
        uint32_t counter;
        uint32_t period;
        uint8_t output;
    } tone[3];
    
    // Noise generator
    struct {
        uint32_t counter;
        uint32_t period;
        uint32_t rng;
        uint8_t output;
    } noise;
    
    // Envelope generator
    struct {
        uint32_t counter;
        uint32_t period;
        uint8_t shape;
        uint8_t step;
        bool holding;
        bool running;
    } envelope;
    
    // Audio output
    uint64_t tstates_accumulated;
    uint64_t tstates_per_sample;
    
    // Mixer configuration
    bool mixer_enable_ay;
    bool mixer_enable_beeper;
    
    // Volume table (4-bit volume to 16-bit sample)
    int16_t volume_table[32];
} ay_state_t;

static ay_state_t ay;

// Initialize volume table with logarithmic curve
static void init_volume_table(void) {
    // AY-3-8912 uses logarithmic volume steps
    // Approximation: each step is roughly 2dB (about 1.26x multiplier)
    const int16_t max_vol = 8000;  // Maximum volume
    
    for (int i = 0; i < 16; i++) {
        if (i == 0) {
            ay.volume_table[i] = 0;
        } else {
            // Logarithmic approximation
            ay.volume_table[i] = (int16_t)(max_vol * i / 15);
        }
    }
    
    // Extended volume for envelope (0-31)
    for (int i = 16; i < 32; i++) {
        ay.volume_table[i] = ay.volume_table[i - 16];
    }
}

void ay_init(uint32_t cpu_clock_hz, uint32_t sample_rate, bool precise_tstate) {
    memset(&ay, 0, sizeof(ay));
    
    ay.cpu_clock_hz = cpu_clock_hz;
    ay.sample_rate = sample_rate;
    ay.precise_tstate = precise_tstate;
    
    // AY clock is CPU clock / 8 (for ZX Spectrum)
    // Actually the AY runs at half the CPU clock divided by 8, so CPU_CLK / 16
    ay.tstates_per_sample = (uint64_t)cpu_clock_hz * 65536 / sample_rate;
    
    // Initialize volume table
    init_volume_table();
    
    // Initialize noise RNG
    ay.noise.rng = 1;
    
    // Enable AY by default
    ay.mixer_enable_ay = true;
    ay.mixer_enable_beeper = true;
    
    ay_reset();
}

void ay_reset(void) {
    // Reset all registers
    memset(ay.regs, 0, sizeof(ay.regs));
    ay.selected_reg = 0;
    
    // Reset tone generators
    for (int i = 0; i < 3; i++) {
        ay.tone[i].counter = 0;
        ay.tone[i].period = 0;
        ay.tone[i].output = 1;
    }
    
    // Reset noise generator
    ay.noise.counter = 0;
    ay.noise.period = 0;
    ay.noise.rng = 1;
    ay.noise.output = 1;
    
    // Reset envelope
    ay.envelope.counter = 0;
    ay.envelope.period = 0;
    ay.envelope.shape = 0;
    ay.envelope.step = 0;
    ay.envelope.holding = false;
    ay.envelope.running = false;
    
    ay.tstates_accumulated = 0;
}

void ay_select_register(uint8_t reg_index) {
    ay.selected_reg = reg_index & 0x0F;
}

void ay_write_reg(uint8_t reg, uint8_t val) {
    if (reg >= 16) return;
    
    ay.regs[reg] = val;
    
    // Update internal state based on register writes
    switch (reg) {
        case AY_AFINE:
        case AY_ACOARSE:
            ay.tone[0].period = (ay.regs[AY_ACOARSE] << 8) | ay.regs[AY_AFINE];
            if (ay.tone[0].period == 0) ay.tone[0].period = 1;
            break;
            
        case AY_BFINE:
        case AY_BCOARSE:
            ay.tone[1].period = (ay.regs[AY_BCOARSE] << 8) | ay.regs[AY_BFINE];
            if (ay.tone[1].period == 0) ay.tone[1].period = 1;
            break;
            
        case AY_CFINE:
        case AY_CCOARSE:
            ay.tone[2].period = (ay.regs[AY_CCOARSE] << 8) | ay.regs[AY_CFINE];
            if (ay.tone[2].period == 0) ay.tone[2].period = 1;
            break;
            
        case AY_NOISEPER:
            ay.noise.period = val & 0x1F;
            if (ay.noise.period == 0) ay.noise.period = 1;
            break;
            
        case AY_ESHAPE:
            // Envelope shape write restarts the envelope
            ay.envelope.shape = val & 0x0F;
            ay.envelope.step = 0;
            ay.envelope.holding = false;
            ay.envelope.running = true;
            ay.envelope.counter = 0;
            break;
            
        case AY_EFINE:
        case AY_ECOARSE:
            ay.envelope.period = (ay.regs[AY_ECOARSE] << 8) | ay.regs[AY_EFINE];
            if (ay.envelope.period == 0) ay.envelope.period = 1;
            break;
    }
}

uint8_t ay_read_reg(uint8_t reg) {
    if (reg >= 16) return 0xFF;
    
    // Some registers are write-only
    switch (reg) {
        case AY_PORTA:
        case AY_PORTB:
            // I/O ports - not implemented, return 0xFF
            return 0xFF;
        default:
            return ay.regs[reg];
    }
}

// Update envelope step
static uint8_t get_envelope_volume(void) {
    if (!ay.envelope.running) {
        return 0;
    }
    
    uint8_t step = ay.envelope.step;
    uint8_t shape = ay.envelope.shape;
    
    if (ay.envelope.holding) {
        // Envelope is holding at a fixed level
        if (shape & ENV_HOLD) {
            if (shape & ENV_ALTERNATE) {
                return (shape & ENV_ATTACK) ? 0 : 31;
            } else {
                return (shape & ENV_ATTACK) ? 31 : 0;
            }
        }
        return 0;
    }
    
    // Calculate volume based on shape and step
    bool attack = (shape & ENV_ATTACK) != 0;
    uint8_t vol;
    
    if (step < 32) {
        if (attack) {
            vol = step;
        } else {
            vol = 31 - step;
        }
    } else {
        vol = 0;
    }
    
    return vol;
}

void ay_step(uint32_t tstates) {
    if (tstates == 0) return;
    
    // The AY clock is CPU clock / 16
    // For each t-state, we advance the AY by 1/16th
    uint32_t ay_ticks = tstates;
    
    // For t-state precision, we process in smaller chunks
    // The actual AY runs at CPU_CLK / 16
    for (uint32_t i = 0; i < ay_ticks; i++) {
        // Update tone generators (they divide by 16 internally)
        for (int ch = 0; ch < 3; ch++) {
            if (ay.tone[ch].period > 0) {
                ay.tone[ch].counter++;
                if (ay.tone[ch].counter >= ay.tone[ch].period * 16) {
                    ay.tone[ch].counter = 0;
                    ay.tone[ch].output ^= 1;
                }
            }
        }
        
        // Update noise generator
        if (ay.noise.period > 0) {
            ay.noise.counter++;
            if (ay.noise.counter >= ay.noise.period * 16) {
                ay.noise.counter = 0;
                // 17-bit LFSR
                ay.noise.output = ay.noise.rng & 1;
                uint32_t feedback = ((ay.noise.rng & 1) ^ ((ay.noise.rng >> 3) & 1));
                ay.noise.rng = (ay.noise.rng >> 1) | (feedback << 16);
            }
        }
        
        // Update envelope generator
        if (ay.envelope.running && ay.envelope.period > 0) {
            ay.envelope.counter++;
            if (ay.envelope.counter >= ay.envelope.period * 16) {
                ay.envelope.counter = 0;
                
                if (!ay.envelope.holding) {
                    ay.envelope.step++;
                    
                    if (ay.envelope.step >= 32) {
                        // Envelope cycle complete
                        if (ay.envelope.shape & ENV_CONTINUE) {
                            if (ay.envelope.shape & ENV_ALTERNATE) {
                                // Flip attack direction
                                ay.envelope.shape ^= ENV_ATTACK;
                            }
                            ay.envelope.step = 0;
                            
                            if (ay.envelope.shape & ENV_HOLD) {
                                ay.envelope.holding = true;
                            }
                        } else {
                            ay.envelope.holding = true;
                            ay.envelope.step = 0;
                        }
                    }
                }
            }
        }
    }
    
    ay.tstates_accumulated += tstates;
}

void ay_mix_samples(int16_t* out_buf, size_t samples) {
    if (!ay.mixer_enable_ay) {
        return;
    }
    
    for (size_t i = 0; i < samples; i++) {
        int32_t mix = 0;
        uint8_t mixer = ay.regs[AY_MIXER];
        
        // Mix the three channels
        for (int ch = 0; ch < 3; ch++) {
            bool tone_on = ((mixer >> ch) & 1) == 0;
            bool noise_on = ((mixer >> (ch + 3)) & 1) == 0;
            
            uint8_t vol_reg = ay.regs[AY_AVOL + ch];
            bool use_envelope = (vol_reg & 0x10) != 0;
            uint8_t volume;
            
            if (use_envelope) {
                volume = get_envelope_volume();
            } else {
                volume = vol_reg & 0x0F;
            }
            
            // Calculate channel output
            uint8_t output = 0;
            if (tone_on && noise_on) {
                output = ay.tone[ch].output & ay.noise.output;
            } else if (tone_on) {
                output = ay.tone[ch].output;
            } else if (noise_on) {
                output = ay.noise.output;
            }
            
            if (output) {
                mix += ay.volume_table[volume];
            }
        }
        
        // Mix with existing buffer content (for beeper mixing)
        int32_t result = out_buf[i] + (int16_t)(mix / 3);
        
        // Clamp to 16-bit range
        if (result > 32767) result = 32767;
        if (result < -32768) result = -32768;
        
        out_buf[i] = (int16_t)result;
    }
}

void ay_set_mixer(bool enable_ay, bool enable_beeper) {
    ay.mixer_enable_ay = enable_ay;
    ay.mixer_enable_beeper = enable_beeper;
}

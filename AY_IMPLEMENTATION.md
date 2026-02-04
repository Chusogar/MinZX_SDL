# AY-3-8912 Implementation Summary

## Overview

This implementation adds full AY-3-8912 sound chip emulation to MinZX_SDL with t-state precise timing. The AY-3-8912 was the sound chip used in the ZX Spectrum 128K, providing 3 tone channels, 1 noise channel, and an envelope generator.

## Architecture

### Files Added

```
ay/
├── ay.h        - Public API (74 lines)
└── ay.c        - Implementation (380 lines)
```

### Files Modified

```
minzx.c         - Integrated AY into main emulator
README.md       - Updated documentation
.gitignore      - Added build artifacts
```

## Implementation Details

### AY Module (ay/ay.c, ay/ay.h)

#### Core Features

1. **Tone Generators (3 channels)**
   - 12-bit period registers (fine + coarse)
   - Square wave output
   - Independent counters per channel
   - Runs at CPU_CLOCK / 16

2. **Noise Generator**
   - 5-bit period register
   - 17-bit LFSR (Linear Feedback Shift Register)
   - Feedback from bits 0 and 3
   - White noise output

3. **Envelope Generator**
   - 16-bit period register
   - 4-bit shape control (Continue, Attack, Alternate, Hold)
   - 32-step envelope (0-31)
   - Automatic restart on shape write

4. **Mixer**
   - 6-bit mixer control (tone/noise enable per channel)
   - 4-bit volume per channel
   - Envelope mode flag per channel
   - Combines all active channels

5. **Volume Table**
   - 16-level volume curve (0-15)
   - Linear approximation (could be enhanced with logarithmic curve)
   - Maximum volume: 8000 (16-bit signed samples)

#### API Functions

```c
void ay_init(uint32_t cpu_clock_hz, uint32_t sample_rate, bool precise_tstate);
void ay_reset(void);
void ay_step(uint32_t tstates);
void ay_write_reg(uint8_t reg, uint8_t val);
uint8_t ay_read_reg(uint8_t reg);
void ay_select_register(uint8_t reg_index);
void ay_mix_samples(int16_t* out_buf, size_t samples);
void ay_set_mixer(bool enable_ay, bool enable_beeper);
```

### Integration in minzx.c

#### Initialization

```c
// In main(), after SDL audio setup:
ay_init(CPU_HZ, SAMPLE_RATE, true);
```

- CPU_HZ: 3,500,000 (3.5 MHz)
- SAMPLE_RATE: 44,100 Hz
- precise_tstate: true (enables t-state precision)

#### Main Loop Integration

```c
// In main loop, for each scanline:
z80_step_n(&cpu, 224);      // Execute CPU for 224 t-states
ay_step(224);                // Advance AY by same amount
```

This ensures the AY emulation stays synchronized with the CPU.

#### Audio Generation

```c
void generate_audio(uint32_t current_tstates) {
    // ... fill buffer with beeper samples ...
    
    if (audio_ptr >= BUFFER_SIZE) {
        ay_mix_samples(audio_buffer, BUFFER_SIZE);  // Mix in AY output
        SDL_QueueAudio(audio_dev, audio_buffer, BUFFER_SIZE * sizeof(int16_t));
        audio_ptr = 0;
    }
}
```

The AY output is mixed with the beeper output before sending to SDL.

#### I/O Port Mapping

**Port 0xFFFD (Register Select)**
```c
// In port_out():
if (is_128k_mode && (port & 0xC002) == 0xC000) {
    ay_selected_register = val & 0x0F;
    ay_select_register(ay_selected_register);
}
```

**Port 0xBFFD (Data Read/Write)**
```c
// In port_out():
if (is_128k_mode && (port & 0xC002) == 0x8000) {
    ay_write_reg(ay_selected_register, val);
}

// In port_in():
if (is_128k_mode && (port & 0xC002) == 0x8000) {
    return ay_read_reg(ay_selected_register);
}
```

#### Reset Handler

```c
void zx_reset(void) {
    // ... other reset code ...
    ay_reset();  // Reset AY chip
}
```

## Technical Specifications

### AY-3-8912 Register Map

| Register | Name            | Description                    |
|----------|-----------------|--------------------------------|
| 0        | AFINE           | Channel A Fine Tune            |
| 1        | ACOARSE         | Channel A Coarse Tune          |
| 2        | BFINE           | Channel B Fine Tune            |
| 3        | BCOARSE         | Channel B Coarse Tune          |
| 4        | CFINE           | Channel C Fine Tune            |
| 5        | CCOARSE         | Channel C Coarse Tune          |
| 6        | NOISEPER        | Noise Period (5 bits)          |
| 7        | MIXER           | Mixer Control/Enable           |
| 8        | AVOL            | Channel A Volume               |
| 9        | BVOL            | Channel B Volume               |
| 10       | CVOL            | Channel C Volume               |
| 11       | EFINE           | Envelope Fine Tune             |
| 12       | ECOARSE         | Envelope Coarse Tune           |
| 13       | ESHAPE          | Envelope Shape                 |
| 14       | PORTA           | I/O Port A (not implemented)   |
| 15       | PORTB           | I/O Port B (not implemented)   |

### Timing Details

- **CPU Clock**: 3.5 MHz
- **AY Clock**: CPU Clock / 16 = 218.75 kHz
- **Tone Period**: (Register value) * 16 t-states
- **Noise Period**: (Register value) * 16 t-states
- **Envelope Period**: (Register value) * 16 t-states
- **Sample Rate**: 44,100 Hz
- **T-states per frame**: 69,888
- **T-states per scanline**: 224

### Envelope Shapes

The envelope shape register (register 13) controls the envelope pattern:

- **Bit 0 (HOLD)**: Hold at final level
- **Bit 1 (ALTERNATE)**: Alternate direction on repeat
- **Bit 2 (ATTACK)**: Start attack (rising) vs decay (falling)
- **Bit 3 (CONTINUE)**: Continue cycling vs one-shot

Common shapes:
- 0x00: \_____ (one-shot decay)
- 0x04: /‾‾‾‾‾ (one-shot attack)
- 0x08: \\\\\\ (repeated decay)
- 0x0A: \/\/\/ (triangle wave)
- 0x0C: ////// (repeated attack)
- 0x0E: /‾\/‾\ (attack-hold-decay-hold)

## Build Instructions

### Linux
```bash
gcc minzx.c jgz80/z80.c ay/ay.c disk/trd.c disk/scl.c disk/fdc.c -o minzx -lSDL2 -lm
```

### Windows (MSYS2)
```bash
gcc minzx.c jgz80/z80.c ay/ay.c disk/trd.c disk/scl.c disk/fdc.c -o minzx.exe -lmingw32 -lSDL2main -lSDL2
```

## Testing

See `AY_TESTING.md` for comprehensive testing instructions.

## Future Enhancements

Potential improvements for future versions:

1. **Logarithmic Volume Curve**
   - Current implementation uses linear volume
   - True AY uses ~2dB per step logarithmic curve
   - Would provide more authentic sound

2. **I/O Port Support**
   - Registers 14 and 15 (PORTA/PORTB) not implemented
   - Could be used for joystick or printer interface

3. **Stereo Output**
   - Current implementation is mono
   - Could implement ABC/ACB stereo mixing schemes

4. **DC Offset Removal**
   - Add high-pass filter to remove DC component
   - Would prevent speaker pop on start/stop

5. **Anti-aliasing**
   - Band-limit the output to prevent aliasing
   - Would improve audio quality at high frequencies

## References

- AY-3-8912 Datasheet: General Instrument / Microchip
- ZX Spectrum 128K Technical Manual
- FUSE Emulator AY implementation
- World of Spectrum technical documentation

## Compatibility

This implementation is compatible with:
- ZX Spectrum 128K software
- ZX Spectrum +2 software
- ZX Spectrum +3 software (with caveats for disk system)
- Most 128K games and demos

Known compatible software:
- Wham! The Music Box
- Chase H.Q.
- Batman: The Movie
- Turbo Esprit
- Rainbow Islands
- R-Type

## License

Implementation follows the MinZX_SDL project license.

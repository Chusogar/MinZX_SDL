/*
 * AY-3-8912 Sound Chip Emulation
 * T-state precise emulation for ZX Spectrum 128K
 * 
 * Features:
 * - 3 tone channels
 * - 1 noise channel
 * - Envelope generator
 * - Mixer for combining channels
 * - T-state based timing
 */

#ifndef AY_H
#define AY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * Initialize the AY-3-8912 emulator
 * @param cpu_clock_hz CPU clock frequency in Hz (e.g., 3500000 for ZX Spectrum)
 * @param sample_rate Audio sample rate in Hz (e.g., 44100)
 * @param precise_tstate Enable t-state precise emulation
 */
void ay_init(uint32_t cpu_clock_hz, uint32_t sample_rate, bool precise_tstate);

/**
 * Reset the AY-3-8912 to initial state
 */
void ay_reset(void);

/**
 * Advance the AY-3-8912 emulation by the specified number of t-states
 * @param tstates Number of t-states to advance
 */
void ay_step(uint32_t tstates);

/**
 * Write a value to a specific AY register
 * @param reg Register number (0-15)
 * @param val Value to write
 */
void ay_write_reg(uint8_t reg, uint8_t val);

/**
 * Read a value from a specific AY register
 * @param reg Register number (0-15)
 * @return Register value
 */
uint8_t ay_read_reg(uint8_t reg);

/**
 * Select a register for subsequent read/write operations
 * @param reg_index Register number (0-15)
 */
void ay_select_register(uint8_t reg_index);

/**
 * Mix AY output samples into the provided buffer
 * Should be called from audio callback or before SDL_QueueAudio
 * @param out_buf Output buffer for signed 16-bit samples
 * @param samples Number of samples to generate
 */
void ay_mix_samples(int16_t* out_buf, size_t samples);

/**
 * Configure mixer settings (optional)
 * @param enable_ay Enable AY output
 * @param enable_beeper Enable beeper output
 */
void ay_set_mixer(bool enable_ay, bool enable_beeper);

#endif // AY_H

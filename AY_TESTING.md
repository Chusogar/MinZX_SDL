# AY-3-8912 Emulation Testing Guide

This document describes how to test the AY-3-8912 sound chip emulation in MinZX_SDL.

## Prerequisites

1. Build the emulator with AY support:
   ```bash
   gcc minzx.c jgz80/z80.c ay/ay.c disk/trd.c disk/scl.c disk/fdc.c -o minzx -lSDL2 -lm
   ```

2. Obtain a ZX Spectrum 128K ROM file (zx128.rom)

3. Obtain test software that uses the AY chip

## Test Cases

### Basic Functionality Tests

#### 1. Test Tone Generation
- **Software**: Any 128K game with music (e.g., Turbo Esprit, Chase H.Q.)
- **Expected**: Should hear melodic music through the AY chip
- **Verification**: 
  - Sound should be distinct from beeper
  - Multiple tones should be audible simultaneously
  - Pitch should match reference emulators

#### 2. Test Noise Generator
- **Software**: Games with explosion or percussion sounds
- **Expected**: White noise effects should be audible
- **Verification**: 
  - Noise should sound random and continuous
  - Noise should mix with tone channels

#### 3. Test Envelope Generator
- **Software**: Music with envelope effects (e.g., Attack, Decay, Sustain)
- **Expected**: Volume should change over time according to envelope shape
- **Verification**:
  - Sounds should fade in/out smoothly
  - Different envelope shapes should produce different effects

#### 4. Test Channel Mixing
- **Software**: Complex music using all 3 channels + noise
- **Expected**: All channels should be audible simultaneously
- **Verification**:
  - No channel should completely drown out others
  - Volume balance should be reasonable

### I/O Port Tests

#### 5. Test Register Selection (Port 0xFFFD)
- **Method**: Use debug mode to verify register selection
- **Expected**: Writing to 0xFFFD should select correct AY register
- **Verification**: Check debug output shows correct register number

#### 6. Test Data Write/Read (Port 0xBFFD)
- **Method**: Test programs that read back AY registers
- **Expected**: Should be able to read values previously written
- **Verification**: Register reads should match writes

### Timing Tests

#### 7. Test T-state Precision
- **Software**: Music that requires precise timing (fast arpeggios, tempo)
- **Expected**: Music tempo should match reference emulators
- **Verification**:
  - No audible clicks or timing glitches
  - Tempo should be steady

#### 8. Test Synchronization with Beeper
- **Software**: Programs using both AY and beeper
- **Expected**: Both audio sources should be in sync
- **Verification**:
  - No audio/video desync
  - Beeper and AY should not interfere

## Known Good Test Software

### Music Demos
- **Wham! The Music Box** - Excellent music showcase
- **Savage** - Title music uses all AY features
- **R-Type** - Complex multi-channel music

### Games with AY Sound
- **Chase H.Q.** - Music and sound effects
- **Batman: The Movie** - Title screen music
- **Rainbow Islands** - Melodic soundtrack
- **Turbo Esprit** - Engine sounds and music

### Technical Demos
- **AY Rider** - AY register viewer and sound test
- **Beepola** - Music creation tool with AY support

## Debugging

### Enable Debug Output
Set `_debug = true` in minzx.c to see:
- Register selection messages
- Register write operations
- AY initialization info

### Volume Adjustment
If AY sound is too quiet or too loud:
- Adjust `max_vol` constant in `ay.c` (currently 8000)
- Modify mixer division in `ay_mix_samples()` (currently /3)

### Common Issues

1. **No AY Sound**
   - Verify 128K mode is active (`--128k` flag)
   - Check that AY registers are being written to
   - Ensure SDL audio device is properly initialized

2. **Distorted Sound**
   - May need to reduce volume levels
   - Check for clipping in mixer

3. **Wrong Pitch**
   - Verify CPU clock is set to 3500000 Hz
   - Check AY period calculations in `ay_write_reg()`

4. **Timing Issues**
   - Ensure `ay_step()` is called with correct t-state count
   - Verify main loop advances CPU and AY in sync

## Reference Implementation

For comparison testing, use established emulators:
- **Fuse** (Unix/Linux/Mac)
- **ZEsarUX** (Multi-platform)
- **SpecEmu** (Windows)

Compare audio output and timing against these reference implementations.

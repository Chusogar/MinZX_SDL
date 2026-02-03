# MinZX_SDL
A minimal ZX Spectrum emulator, written in C, using SDL, compiled with Visual Studio or GCC.

Using [z80cpp emulator core](https://github.com/jsanchezv/z80cpp) from [José Luis Sánchez](https://github.com/jsanchezv).

## Features

### Working features:

- Boot into Spectrum Basic (48K or 128K mode)
- Can load SNA files from command line
- Support for both 48K and 128K models
- Memory banking for 128K mode (8 RAM banks of 16KB each)
- ROM paging (2 ROM banks for 128K mode)
- AY-3-8912 sound chip emulation (placeholder)
- Tape loading support (TAP and TZX formats)
- **Floating bus emulation**: Accurate ULA fetch state tracking for undecoded port reads

### Missing features:

- No keyboard yet
- AY sound chip (placeholder only, no actual sound generation)

## Technical Details

### Floating Bus

The emulator includes accurate floating bus emulation. When reading from undecoded I/O ports (except Kempston joystick at 0x1F), the emulator returns the value currently being fetched by the ULA from video memory. This is a hardware characteristic of the ZX Spectrum that some programs rely on for:
- Pseudo-random number generation
- Timing-sensitive operations
- Detection of specific hardware

The floating bus implementation tracks:
- Current scanline position (0-311)
- ULA fetch phase (during the first 128 T-states of visible scanlines)
- Video memory address being accessed (pixel data or attributes)

During non-visible scanlines or when ULA is not actively fetching, undecoded ports return 0xFF.

As there is no input method yet, only way to test the emulator is to load some game from .SNA which has demo mode, such as Manic Miner.

## Usage

### 48K Mode (default)
```
./minzx [snapshot.sna | tape.tap | tape.tzx]
```

### 128K Mode
```
./minzx --128k [snapshot.sna | tape.tap | tape.tzx]
```

## ROM Files

- For 48K mode: Place `zx48.rom` (16KB) in the same directory as the executable
- For 128K mode: Place `zx128.rom` (32KB, containing both ROM banks) in the same directory
  - If `zx128.rom` is not found, the emulator will fallback to using `zx48.rom` in both ROM banks



# MinZX_SDL
A minimal ZX Spectrum emulator using SDL.

This repository contains two implementations:
- **C version** (`minzx.c`) - Standalone C implementation with jgz80 CPU core
- **C++ version** (`src/`) - Visual Studio project with z80cpp core

The C version now includes **both ZX Spectrum 128K support and complete TR-DOS disk support**.

Using a simple jgz80 Z80 emulator core (based on concepts from [z80cpp](https://github.com/jsanchezv/z80cpp) by José Luis Sánchez).

## Features

### Working Features:

- **ZX Spectrum 48K and 128K emulation**
- 128K memory banking (8 RAM banks of 16KB each)
- ROM banking (2 ROM banks for 128K mode)
- Boot into Spectrum Basic (48K or 128K mode)
- Load SNA files from command line
- Load TAP files (tape emulation with proper timing)
- Load TZX files (advanced tape format with multiple block types)
- **TR-DOS disk support** - Load and use .TRD and .SCL disk images
- WD1793 FDC emulation for disk operations
- Keyboard input
- Audio/beeper emulation
- Video with proper border, BRIGHT attribute support
- AY-3-8912 sound chip (placeholder)
- Floating bus emulation support

### TR-DOS Disk Support

The emulator now supports TR-DOS disk images in two formats:

- **.TRD** - Standard TR-DOS disk image format (read/write support)
- **.SCL** - Sinclair Computer Language archive format (read-only)

TR-DOS works in both 48K and 128K modes!

### 128K Emulation

- Memory banking: 8 RAM banks of 16KB each (total 128KB)
- ROM banking: 2 ROM banks of 16KB each (48K BASIC + 128K editor)
- Port 0x7FFD: Memory paging control
  - Bits 0-2: RAM bank at 0xC000-0xFFFF
  - Bit 3: Video page select (not implemented yet)
  - Bit 4: ROM select (0 = 48K, 1 = 128K)
  - Bit 5: Paging disable (locks configuration)
- AY-3-8912 sound chip emulation (placeholder)

#### FDC Emulation

- WD1793-compatible floppy disk controller
- Support for up to 4 disk drives
- Proper T-state based timing
- Sector read/write operations
- TR-DOS compatible port mapping (0x1F, 0x3F, 0x5F, 0x7F, 0xFF)

## Building

### Linux
```bash
gcc minzx.c jgz80/z80.c disk/trd.c disk/scl.c disk/fdc.c -o minzx -lSDL2 -lm
```

### Windows (MSYS2)
```bash
gcc minzx.c jgz80/z80.c disk/trd.c disk/scl.c disk/fdc.c -o minzx.exe -lmingw32 -lSDL2main -lSDL2
```

### Visual Studio
Open `MinZX_SDL.sln` and build (Note: C++ version in `src/` directory)

## Usage

### Basic Usage (48K Mode - Default)
```bash
./minzx                          # Boot to BASIC (48K)
./minzx game.sna                 # Load snapshot (48K)
./minzx tape.tap                 # Load tape (48K)
./minzx tape.tzx                 # Load TZX tape (48K)
```

### 128K Mode
```bash
./minzx --128k                   # Boot to 128K BASIC
./minzx --128k game.sna          # Load snapshot in 128K mode
./minzx --128k tape.tap          # Load tape in 128K mode
```

### TR-DOS Disk Usage (48K Mode)
```bash
# Mount a single TRD image to drive A:
./minzx disk.trd

# Mount multiple disk images to drives A: and B:
./minzx disk1.trd disk2.trd

# Mount in read-only mode
./minzx disk.trd --ro

# Mount SCL archive
./minzx archive.scl

# Specify number of drives (1-4, default 2)
./minzx disk.trd --drive-count 4

# Load TR-DOS ROM explicitly
./minzx disk.trd --trdos-rom trdos.rom

# TR-DOS ROM is loaded automatically if trdos.rom exists in the current directory
```

### TR-DOS with 128K Mode
```bash
# Combine 128K and TR-DOS
./minzx --128k disk.trd --trdos-rom trdos.rom

# 128K with multiple disks
./minzx --128k disk1.trd disk2.scl --drive-count 4
```

### Keyboard Shortcuts

- **ESC** - Exit emulator
- **F6** - Reload current tape
- **F7** - Play/pause tape
- **F8** - List mounted disks and show directory
- **F9** - Manual TR-DOS ROM toggle (optional - ROM auto-switches based on PC)
- **F12** - Reset (CPU reset)

### TR-DOS ROM

The emulator can load a TR-DOS ROM to enable full TR-DOS functionality:

1. **Automatic loading**: Place a file named `trdos.rom` in the same directory as the emulator. It will be loaded automatically when disk images are used.

2. **Manual loading**: Use the `--trdos-rom` option:
   ```bash
   ./minzx disk.trd --trdos-rom /path/to/trdos.rom
   ```

3. **Automatic ROM switching**: The emulator automatically activates TR-DOS ROM when the program counter (PC) enters the TR-DOS entry point range (0x3D00-0x3DFF). This happens automatically when:
   - TR-DOS routines are called from BASIC or machine code
   - The system boots into TR-DOS
   - Disk operations are performed

4. **Manual toggle** (optional): Press **F9** to manually override the automatic switching. This is mainly for debugging purposes.

#### How it works

- **Automatic activation**: When `PC & 0xFF00 == 0x3D00`, TR-DOS ROM is mapped
- **Automatic deactivation**: When PC leaves the 0x3D00-0x3DFF range, ZX Spectrum ROM is restored
- **Seamless switching**: The change happens transparently during execution

#### Where to get TR-DOS ROM

TR-DOS ROMs are available from various ZX Spectrum resource sites. Common versions include:
- TR-DOS 5.03 (most compatible)
- TR-DOS 5.04T
- TR-DOS 6.10

**Note**: The TR-DOS ROM file should be exactly 16384 bytes (16KB).

## Disk Image Formats

### TRD Format
- Standard TR-DOS disk image
- Typically 40 or 80 tracks, double-sided
- 16 sectors per track, 256 bytes per sector
- Contains file catalog and disk information
- Full read/write support

### SCL Format  
- Archive format containing multiple TR-DOS files
- Automatically converted to TRD on load
- **Read-only** - changes are not saved back to SCL
- Useful for distributing game collections

## Creating Test Disk Images

You can create TRD images using tools like:
- **ZXSP** - ZX Spectrum emulator with disk image creation
- **Fuse** - Free Unix Spectrum Emulator
- **TRDtool** - Command-line utility for TRD manipulation

## Known Limitations

- SCL images are read-only (write support would require re-packing to SCL format)
- Runtime disk mounting/unmounting not implemented (restart to change disks)
- No disk creation from within emulator
- TR-DOS ROM switching is automatic based on PC address (0x3D00-0x3DFF range)
- 128K video page selection not yet implemented
- AY-3-8912 sound chip is placeholder only (no actual sound generation)
- Floating bus logic added but not fully implemented

## Combining 128K and TR-DOS

The emulator supports using TR-DOS with the ZX Spectrum 128K. This allows you to:

1. **Use 128K RAM** with disk operations
2. **Run 128K programs** that also access disk files
3. **Take advantage of both systems** simultaneously

The TR-DOS ROM takes priority when accessed (PC in 0x3D00-0x3DFF range), regardless of whether you're in 48K or 128K mode.

### Example: 128K + TR-DOS workflow

```bash
# Start in 128K mode with disk support
./minzx --128k mydisk.trd --trdos-rom trdos.rom

# The system has:
# - 128KB of RAM with banking
# - TR-DOS disk access
# - Both 48K and 128K ROMs available
# - Automatic ROM switching when needed
```

## Testing

To test disk support:

1. Create or obtain a .TRD disk image
2. Obtain a TR-DOS ROM (trdos.rom) - place it in the same directory as minzx
3. Run: `./minzx test.trd`
4. The TR-DOS ROM will be loaded automatically
5. TR-DOS ROM will activate automatically when the system calls TR-DOS routines (PC in 0x3D00-0x3DFF)
6. Press F8 to see disk catalog

### Using TR-DOS

The TR-DOS ROM activates automatically when needed. Typical workflow:

1. Start the emulator with a disk image: `./minzx game.trd`
2. The system boots into ZX Spectrum BASIC
3. Use BASIC commands that access disk (e.g., `LOAD`, `CAT`, `RUN`)
4. When TR-DOS routines are called (PC enters 0x3D00-0x3DFF), the ROM automatically switches
5. After TR-DOS operations complete, the ROM switches back to ZX Spectrum

**Note**: The ROM switching is fully automatic based on the program counter. You don't need to manually toggle ROMs.

### Example Session

```bash
# Start with a disk image and TR-DOS ROM in current directory
./minzx game.trd

# System boots into ZX Spectrum BASIC
# Type in BASIC:
CAT        # TR-DOS ROM activates automatically, shows disk catalog
RUN "game" # Loads and runs game from disk

# ROM switching happens transparently
# F9 can be used for manual override if needed (debugging)
```

## License

See LICENSE file.

## Credits

- Z80 emulation based on concepts from José Luis Sánchez's z80cpp
- TR-DOS support implementation for MinZX_SDL

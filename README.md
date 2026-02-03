# MinZX_SDL
A minimal ZX Spectrum emulator using SDL.

This repository contains two implementations:
- **C version** (`minzx.c`) - Standalone C implementation with jgz80 CPU core
- **C++ version** (`src/`) - Visual Studio project with z80cpp core

The C version now includes complete TR-DOS disk support.

Using a simple jgz80 Z80 emulator core (based on concepts from [z80cpp](https://github.com/jsanchezv/z80cpp) by José Luis Sánchez).

## Features

### Working Features:

- Boot into Spectrum Basic
- Load SNA files from command line
- Load TAP files (tape emulation with proper timing)
- Load TZX files (advanced tape format with multiple block types)
- **TR-DOS disk support** - Load and use .TRD and .SCL disk images
- WD1793 FDC emulation for disk operations
- Keyboard input
- Audio/beeper emulation
- Video with proper border, BRIGHT attribute support

### TR-DOS Disk Support

The emulator now supports TR-DOS disk images in two formats:

- **.TRD** - Standard TR-DOS disk image format (read/write support)
- **.SCL** - Sinclair Computer Language archive format (read-only)

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

### Basic Usage
```bash
./minzx                          # Boot to BASIC
./minzx game.sna                 # Load snapshot
./minzx tape.tap                 # Load tape
./minzx tape.tzx                 # Load TZX tape
```

### TR-DOS Disk Usage
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

### Keyboard Shortcuts

- **ESC** - Exit emulator
- **F6** - Reload current tape
- **F7** - Play/pause tape
- **F8** - List mounted disks and show directory
- **F9** - Toggle TR-DOS ROM on/off (when loaded)
- **F12** - Reset (CPU reset)

### TR-DOS ROM

The emulator can load a TR-DOS ROM to enable full TR-DOS functionality:

1. **Automatic loading**: Place a file named `trdos.rom` in the same directory as the emulator. It will be loaded automatically when disk images are used.

2. **Manual loading**: Use the `--trdos-rom` option:
   ```bash
   ./minzx disk.trd --trdos-rom /path/to/trdos.rom
   ```

3. **Toggle ROM**: Press **F9** to switch between the ZX Spectrum ROM and TR-DOS ROM during emulation. This allows you to:
   - Boot into ZX Spectrum BASIC (ZX ROM active)
   - Switch to TR-DOS (TR-DOS ROM active) to access disk commands
   - Switch back to ZX ROM to run BASIC programs

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
- TR-DOS ROM must be toggled manually with F9 key (automatic detection not implemented)

## Testing

To test disk support:

1. Create or obtain a .TRD disk image
2. Obtain a TR-DOS ROM (trdos.rom) - place it in the same directory as minzx
3. Run: `./minzx test.trd`
4. The TR-DOS ROM will be loaded automatically
5. Press F9 to activate TR-DOS ROM
6. Press F8 to see disk catalog

### Using TR-DOS

Once TR-DOS ROM is active (press F9):

```
The system will boot into TR-DOS. Common TR-DOS commands:

LIST       - List files in the directory
RUN "file" - Run a BASIC program from disk  
LOAD "file" - Load a file
SAVE "file" - Save a file
CAT        - Show disk catalog
```

To return to ZX Spectrum BASIC, press F9 again to deactivate TR-DOS ROM and reset (F12).

### Example Session

```bash
# Start with a disk image and TR-DOS ROM in current directory
./minzx game.trd

# In console:
# - TR-DOS ROM will load automatically if trdos.rom exists
# - Press F9 to activate TR-DOS
# - System boots into TR-DOS
# - Use LIST or CAT to see files
# - Use RUN "filename" to load programs
```

## License

See LICENSE file.

## Credits

- Z80 emulation based on concepts from José Luis Sánchez's z80cpp
- TR-DOS support implementation for MinZX_SDL


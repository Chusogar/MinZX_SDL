# MinZX_SDL
A minimal ZX Spectrum emulator, written in C, using SDL.

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
```

### Keyboard Shortcuts

- **ESC** - Exit emulator
- **F6** - Reload current tape
- **F7** - Play/pause tape
- **F8** - List mounted disks and show directory
- **F12** - Reset (CPU reset)

### TR-DOS ROM

To use TR-DOS functionality, you'll need a TR-DOS ROM. The emulator currently boots with the standard ZX Spectrum 48K ROM. To use disk operations:

1. Obtain a TR-DOS ROM (trdos.rom) - these are available from various ZX Spectrum resource sites
2. (Future) Use `--trdos-rom trdos.rom` to load it (not yet implemented)

For now, disk images can be accessed via BASIC commands if a TR-DOS aware program is loaded.

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
- TR-DOS ROM loading not yet implemented (--trdos-rom option reserved)
- Runtime disk mounting/unmounting not implemented (restart to change disks)
- No disk creation from within emulator

## Testing

To test disk support:

1. Create or obtain a .TRD disk image
2. Run: `./minzx test.trd`
3. Press F8 to see disk catalog
4. If you have TR-DOS ROM loaded or a TR-DOS aware program, you can access files

Example with TR-DOS commands (if TR-DOS ROM is active):
```
RANDOMIZE USR 15616  (enters TR-DOS)
CAT                   (lists directory)
RUN "filename"        (runs BASIC program from disk)
```

## License

See LICENSE file.

## Credits

- Z80 emulation based on concepts from José Luis Sánchez's z80cpp
- TR-DOS support implementation for MinZX_SDL


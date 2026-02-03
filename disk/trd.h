/*
 * TRD - TR-DOS Disk Image Format Support
 * TRD format: 40/80 tracks, double-sided, 16 sectors per track, 256 bytes per sector
 */

#ifndef DISK_TRD_H
#define DISK_TRD_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#define TRD_SECTORS_PER_TRACK  16
#define TRD_SECTOR_SIZE        256
#define TRD_HEADS              2
#define TRD_BYTES_PER_TRACK    (TRD_SECTORS_PER_TRACK * TRD_SECTOR_SIZE)

// TRD disk information (sector 8, track 0)
typedef struct {
    uint8_t  disk_type;      // 0x16 = 80 tracks DS, 0x17 = 40 tracks DS, 0x18 = 80 tracks SS
    uint8_t  files_count;    // Number of files
    uint16_t free_sectors;   // Number of free sectors
    uint8_t  tr_dos_id;      // Usually 0x10 (TR-DOS ID)
    uint8_t  reserved[2];
    uint8_t  password[9];    // Disk password (if any)
    uint8_t  unused1;
    uint8_t  deleted_files;  // Number of deleted files
    uint8_t  disk_label[8];  // Disk label
    uint8_t  unused2[3];
} __attribute__((packed)) trd_disk_info_t;

// TRD file entry in catalog (16 bytes each, sector 0-7 of track 0)
typedef struct {
    char     filename[8];    // Filename (padded with spaces)
    char     extension[3];   // Extension: 'B' (Basic), 'C' (Code), 'D' (Data), '#' (Print)
    uint16_t start;          // Start address for CODE, line number for Basic, etc.
    uint16_t length;         // File length in bytes
    uint8_t  sectors_used;   // Number of sectors occupied
    uint8_t  start_sector;   // Starting sector number
    uint8_t  start_track;    // Starting track number
} __attribute__((packed)) trd_file_entry_t;

#define TRD_MAX_FILES 128    // 8 sectors * 16 entries per sector

// TRD disk image structure
typedef struct {
    FILE*    file;           // File handle
    char     filename[256];  // Image filename
    bool     read_only;      // Read-only flag
    bool     modified;       // Has been modified
    
    // Disk geometry
    uint8_t  tracks;         // Number of tracks (40 or 80)
    uint8_t  sides;          // Number of sides (1 or 2)
    
    // Cached data
    trd_disk_info_t disk_info;
    trd_file_entry_t files[TRD_MAX_FILES];
    uint8_t files_loaded;    // Number of valid file entries
} trd_image_t;

// TRD API
trd_image_t* trd_open(const char* filename, bool read_only);
void trd_close(trd_image_t* img);
bool trd_read_sector(trd_image_t* img, uint8_t track, uint8_t head, uint8_t sector, uint8_t* buffer);
bool trd_write_sector(trd_image_t* img, uint8_t track, uint8_t head, uint8_t sector, const uint8_t* buffer);
bool trd_flush(trd_image_t* img); // Flush changes to disk
void trd_list_files(trd_image_t* img); // Print file catalog to console

#endif // DISK_TRD_H

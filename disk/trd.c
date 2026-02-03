/*
 * TRD - TR-DOS Disk Image Format Implementation
 */

#include "trd.h"
#include <stdlib.h>
#include <string.h>

// Calculate physical offset in file for given track/head/sector
static long trd_get_offset(trd_image_t* img, uint8_t track, uint8_t head, uint8_t sector) {
    // TRD format: interleaved by head
    // Track 0 Side 0, Track 0 Side 1, Track 1 Side 0, Track 1 Side 1, ...
    // Sector numbering starts at 0
    
    if (track >= img->tracks || head >= img->sides || sector >= TRD_SECTORS_PER_TRACK) {
        return -1;
    }
    
    long offset = (long)track * img->sides * TRD_BYTES_PER_TRACK;
    offset += (long)head * TRD_BYTES_PER_TRACK;
    offset += (long)sector * TRD_SECTOR_SIZE;
    
    return offset;
}

// Open TRD image
trd_image_t* trd_open(const char* filename, bool read_only) {
    trd_image_t* img = (trd_image_t*)calloc(1, sizeof(trd_image_t));
    if (!img) return NULL;
    
    strncpy(img->filename, filename, sizeof(img->filename) - 1);
    img->read_only = read_only;
    img->modified = false;
    
    // Open file
    img->file = fopen(filename, read_only ? "rb" : "rb+");
    if (!img->file) {
        // If read-write failed, try read-only
        img->file = fopen(filename, "rb");
        if (!img->file) {
            free(img);
            return NULL;
        }
        img->read_only = true;
    }
    
    // Determine disk size from file size
    fseek(img->file, 0, SEEK_END);
    long size = ftell(img->file);
    fseek(img->file, 0, SEEK_SET);
    
    // Standard TRD sizes:
    // 655360 bytes = 80 tracks * 2 sides * 16 sectors * 256 bytes
    // 327680 bytes = 40 tracks * 2 sides * 16 sectors * 256 bytes
    // 327680 bytes = 80 tracks * 1 side  * 16 sectors * 256 bytes
    
    if (size == 655360) {
        img->tracks = 80;
        img->sides = 2;
    } else if (size == 327680) {
        // Could be 40 tracks DS or 80 tracks SS - check disk info
        img->tracks = 80; // Assume 80 tracks DS for now
        img->sides = 2;
    } else {
        fprintf(stderr, "TRD: Unknown disk size %ld bytes\n", size);
        fclose(img->file);
        free(img);
        return NULL;
    }
    
    // Read disk info from sector 8, track 0, side 0
    uint8_t info_sector[TRD_SECTOR_SIZE];
    if (!trd_read_sector(img, 0, 0, 8, info_sector)) {
        fprintf(stderr, "TRD: Could not read disk info\n");
        fclose(img->file);
        free(img);
        return NULL;
    }
    
    memcpy(&img->disk_info, info_sector, sizeof(trd_disk_info_t));
    
    // Determine actual geometry from disk info
    switch (img->disk_info.disk_type) {
        case 0x16: img->tracks = 80; img->sides = 2; break;
        case 0x17: img->tracks = 40; img->sides = 2; break;
        case 0x18: img->tracks = 80; img->sides = 1; break;
        default:
            // Use file size to determine
            break;
    }
    
    // Read file catalog (sectors 0-7, track 0, side 0)
    img->files_loaded = 0;
    for (int sec = 0; sec < 8 && img->files_loaded < TRD_MAX_FILES; sec++) {
        uint8_t sector_data[TRD_SECTOR_SIZE];
        if (!trd_read_sector(img, 0, 0, sec, sector_data)) {
            break;
        }
        
        for (int i = 0; i < 16; i++) {
            trd_file_entry_t* entry = (trd_file_entry_t*)(sector_data + i * 16);
            // Check if entry is valid (filename starts with non-zero)
            if (entry->filename[0] != 0 && entry->filename[0] != 1) {
                memcpy(&img->files[img->files_loaded], entry, sizeof(trd_file_entry_t));
                img->files_loaded++;
            }
        }
    }
    
    printf("TRD: Opened '%s' - %d tracks, %d side%s, %d files\n",
           filename, img->tracks, img->sides, img->sides > 1 ? "s" : "", img->files_loaded);
    
    return img;
}

// Close TRD image
void trd_close(trd_image_t* img) {
    if (!img) return;
    
    if (img->modified && !img->read_only) {
        fflush(img->file);
    }
    
    if (img->file) {
        fclose(img->file);
    }
    
    free(img);
}

// Read sector
bool trd_read_sector(trd_image_t* img, uint8_t track, uint8_t head, uint8_t sector, uint8_t* buffer) {
    if (!img || !img->file || !buffer) return false;
    
    long offset = trd_get_offset(img, track, head, sector);
    if (offset < 0) return false;
    
    if (fseek(img->file, offset, SEEK_SET) != 0) return false;
    
    size_t read = fread(buffer, 1, TRD_SECTOR_SIZE, img->file);
    return (read == TRD_SECTOR_SIZE);
}

// Write sector
bool trd_write_sector(trd_image_t* img, uint8_t track, uint8_t head, uint8_t sector, const uint8_t* buffer) {
    if (!img || !img->file || !buffer) return false;
    if (img->read_only) return false;
    
    long offset = trd_get_offset(img, track, head, sector);
    if (offset < 0) return false;
    
    if (fseek(img->file, offset, SEEK_SET) != 0) return false;
    
    size_t written = fwrite(buffer, 1, TRD_SECTOR_SIZE, img->file);
    if (written == TRD_SECTOR_SIZE) {
        img->modified = true;
        return true;
    }
    return false;
}

// Flush changes
bool trd_flush(trd_image_t* img) {
    if (!img || !img->file || img->read_only) return false;
    return (fflush(img->file) == 0);
}

// List files in catalog
void trd_list_files(trd_image_t* img) {
    if (!img) return;
    
    printf("\n=== TRD Disk: %s ===\n", img->filename);
    printf("Disk label: %.8s\n", img->disk_info.disk_label);
    printf("Files: %d, Free sectors: %d\n", img->disk_info.files_count, img->disk_info.free_sectors);
    printf("\nFilename        Type  Start  Length  Sectors  Track:Sector\n");
    printf("---------------------------------------------------------------\n");
    
    for (int i = 0; i < img->files_loaded; i++) {
        trd_file_entry_t* f = &img->files[i];
        
        // Format filename (trim trailing spaces)
        char name[9];
        memcpy(name, f->filename, 8);
        name[8] = '\0';
        for (int j = 7; j >= 0; j--) {
            if (name[j] == ' ') name[j] = '\0';
            else break;
        }
        
        // Extension
        char ext[4];
        memcpy(ext, f->extension, 3);
        ext[3] = '\0';
        
        printf("%-8s.%-3s   %c    %5d  %5d   %5d     %3d:%2d\n",
               name, ext,
               f->extension[0],
               f->start,
               f->length,
               f->sectors_used,
               f->start_track,
               f->start_sector);
    }
    printf("---------------------------------------------------------------\n\n");
}

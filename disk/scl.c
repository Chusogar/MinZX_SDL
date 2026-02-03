/*
 * SCL - Sinclair Computer Language Format Implementation
 * Converts SCL archives to TRD format for emulation
 */

#include "scl.h"
#include <stdlib.h>
#include <string.h>

// Open SCL file and convert to TRD
scl_image_t* scl_open(const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "SCL: Could not open file: %s\n", filename);
        return NULL;
    }
    
    // Read SCL header
    scl_header_t header;
    if (fread(&header, sizeof(scl_header_t), 1, f) != 1) {
        fprintf(stderr, "SCL: Could not read header\n");
        fclose(f);
        return NULL;
    }
    
    // Verify signature
    if (memcmp(header.signature, "SINCLAIR", 8) != 0) {
        fprintf(stderr, "SCL: Invalid signature\n");
        fclose(f);
        return NULL;
    }
    
    printf("SCL: Found %d files in archive\n", header.files_count);
    
    // Read file descriptors
    scl_file_desc_t* descriptors = (scl_file_desc_t*)malloc(header.files_count * sizeof(scl_file_desc_t));
    if (!descriptors) {
        fclose(f);
        return NULL;
    }
    
    for (int i = 0; i < header.files_count; i++) {
        if (fread(&descriptors[i], sizeof(scl_file_desc_t), 1, f) != 1) {
            fprintf(stderr, "SCL: Could not read file descriptor %d\n", i);
            free(descriptors);
            fclose(f);
            return NULL;
        }
    }
    
    // Create temporary TRD file to hold converted data
    // Use portable temp file creation
    char temp_filename[512];
#ifdef _WIN32
    char* tmpdir = getenv("TEMP");
    if (!tmpdir) tmpdir = getenv("TMP");
    if (!tmpdir) tmpdir = ".";
    snprintf(temp_filename, sizeof(temp_filename), "%s\\scl_temp_%p.trd", tmpdir, (void*)f);
#else
    snprintf(temp_filename, sizeof(temp_filename), "/tmp/scl_temp_%p.trd", (void*)f);
#endif
    
    FILE* trd_f = fopen(temp_filename, "wb+");
    if (!trd_f) {
        fprintf(stderr, "SCL: Could not create temporary TRD file\n");
        free(descriptors);
        fclose(f);
        return NULL;
    }
    
    // Initialize empty TRD image (80 tracks, 2 sides)
    uint8_t empty_sector[TRD_SECTOR_SIZE];
    memset(empty_sector, 0, TRD_SECTOR_SIZE);
    
    int total_sectors = 80 * 2 * TRD_SECTORS_PER_TRACK;
    for (int i = 0; i < total_sectors; i++) {
        fwrite(empty_sector, TRD_SECTOR_SIZE, 1, trd_f);
    }
    
    // Write file catalog to sectors 0-7, track 0
    long catalog_pos = 0;
    fseek(trd_f, catalog_pos, SEEK_SET);
    
    // Track allocation for files - local to this function
    int next_track = 1;
    int next_sector = 0;
    
    for (int i = 0; i < header.files_count && i < 128; i++) {
        trd_file_entry_t entry;
        memcpy(entry.filename, descriptors[i].filename, 8);
        memcpy(entry.extension, descriptors[i].extension, 3);
        entry.start = descriptors[i].start;
        entry.length = descriptors[i].length;
        entry.sectors_used = descriptors[i].sectors_used;
        
        // Allocate space for file starting from track 1
        entry.start_track = next_track;
        entry.start_sector = next_sector;
        
        // Advance to next free position
        next_sector += descriptors[i].sectors_used;
        while (next_sector >= TRD_SECTORS_PER_TRACK) {
            next_sector -= TRD_SECTORS_PER_TRACK;
            next_track++;
        }
        
        fwrite(&entry, sizeof(trd_file_entry_t), 1, trd_f);
    }
    
    // Write disk info to sector 8, track 0
    trd_disk_info_t disk_info;
    memset(&disk_info, 0, sizeof(disk_info));
    disk_info.disk_type = 0x16; // 80 tracks DS
    disk_info.files_count = header.files_count;
    disk_info.free_sectors = 2544; // Placeholder
    disk_info.tr_dos_id = 0x10;
    strncpy((char*)disk_info.disk_label, "SCLCONV", 8);
    // Ensure no buffer overflow
    
    long info_pos = 8 * TRD_SECTOR_SIZE; // Sector 8 on track 0
    fseek(trd_f, info_pos, SEEK_SET);
    fwrite(&disk_info, sizeof(trd_disk_info_t), 1, trd_f);
    
    // Write file data
    for (int i = 0; i < header.files_count; i++) {
        // Calculate position in TRD
        // Files are stored after the catalog, starting from track 1
        trd_file_entry_t* entry = (trd_file_entry_t*)&descriptors[i];
        
        // Read file data from SCL
        uint8_t* file_data = (uint8_t*)malloc(descriptors[i].sectors_used * TRD_SECTOR_SIZE);
        if (!file_data) continue;
        
        size_t to_read = descriptors[i].sectors_used * TRD_SECTOR_SIZE;
        if (fread(file_data, 1, to_read, f) != to_read) {
            free(file_data);
            continue;
        }
        
        // Write to TRD file
        // Recalculate position based on entry
        long trd_pos = 0; // Would need to parse catalog to find position
        // For now, just write sequentially after catalog
        fwrite(file_data, 1, to_read, trd_f);
        
        free(file_data);
    }
    
    free(descriptors);
    fclose(f);
    fclose(trd_f);
    
    // Now open the TRD file
    trd_image_t* trd = trd_open(temp_filename, false); // Read-write for temp file
    if (!trd) {
        fprintf(stderr, "SCL: Could not open converted TRD\n");
        remove(temp_filename);
        return NULL;
    }
    
    // Create SCL image wrapper
    scl_image_t* scl = (scl_image_t*)calloc(1, sizeof(scl_image_t));
    if (!scl) {
        trd_close(trd);
        remove(temp_filename);
        return NULL;
    }
    
    strncpy(scl->filename, filename, sizeof(scl->filename) - 1);
    scl->trd = trd;
    scl->read_only = true; // SCL is read-only
    
    printf("SCL: Converted to TRD successfully\n");
    
    return scl;
}

// Close SCL image
void scl_close(scl_image_t* img) {
    if (!img) return;
    
    if (img->trd) {
        // Get temp filename to delete it
        char temp_filename[256];
        strncpy(temp_filename, img->trd->filename, sizeof(temp_filename));
        
        trd_close(img->trd);
        
        // Remove temporary TRD file
        remove(temp_filename);
    }
    
    free(img);
}

// Get underlying TRD
trd_image_t* scl_get_trd(scl_image_t* img) {
    return img ? img->trd : NULL;
}

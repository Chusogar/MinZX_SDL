/*
 * SCL - Sinclair Computer Language (SCL) Disk Archive Format
 * SCL is a compressed format for TR-DOS disk files
 */

#ifndef DISK_SCL_H
#define DISK_SCL_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "trd.h"

// SCL file header
typedef struct {
    char     signature[8];   // "SINCLAIR"
    uint8_t  files_count;    // Number of files
} __attribute__((packed)) scl_header_t;

// SCL file descriptor (14 bytes)
typedef struct {
    char     filename[8];
    char     extension[3];
    uint16_t start;
    uint16_t length;
    uint8_t  sectors_used;
} __attribute__((packed)) scl_file_desc_t;

// SCL image (converted to TRD internally for simplicity)
typedef struct {
    char     filename[256];
    trd_image_t* trd;        // Converted TRD image
    bool     read_only;      // SCL is always read-only initially
} scl_image_t;

// SCL API
scl_image_t* scl_open(const char* filename);
void scl_close(scl_image_t* img);
trd_image_t* scl_get_trd(scl_image_t* img); // Get underlying TRD

// Note: For write support, would need to re-pack to SCL format
// Initially implementing as read-only

#endif // DISK_SCL_H

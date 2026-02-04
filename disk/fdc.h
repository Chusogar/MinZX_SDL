/*
 * FDC - WD1793-compatible Floppy Disk Controller Emulation
 * For TR-DOS / Beta Disk Interface
 */

#ifndef DISK_FDC_H
#define DISK_FDC_H

#include <stdint.h>
#include <stdbool.h>
#include "trd.h"

// FDC ports (typical Beta Disk Interface mapping)
#define FDC_PORT_STATUS   0x1F    // Command/Status register
#define FDC_PORT_TRACK    0x3F    // Track register
#define FDC_PORT_SECTOR   0x5F    // Sector register
#define FDC_PORT_DATA     0x7F    // Data register
#define FDC_PORT_CONTROL  0xFF    // System control register

// WD1793 Command types
#define FDC_CMD_RESTORE      0x00  // Type I
#define FDC_CMD_SEEK         0x10  // Type I
#define FDC_CMD_STEP         0x20  // Type I
#define FDC_CMD_STEP_IN      0x40  // Type I
#define FDC_CMD_STEP_OUT     0x60  // Type I
#define FDC_CMD_READ_SECTOR  0x80  // Type II
#define FDC_CMD_WRITE_SECTOR 0xA0  // Type II
#define FDC_CMD_READ_ADDRESS 0xC0  // Type III
#define FDC_CMD_READ_TRACK   0xE0  // Type III
#define FDC_CMD_WRITE_TRACK  0xF0  // Type III
#define FDC_CMD_FORCE_INT    0xD0  // Type IV

// Status register bits
#define FDC_STATUS_BUSY       0x01
#define FDC_STATUS_DRQ        0x02  // Data Request
#define FDC_STATUS_LOST_DATA  0x04
#define FDC_STATUS_CRC_ERROR  0x08
#define FDC_STATUS_RNF        0x10  // Record Not Found
#define FDC_STATUS_SEEK_ERROR 0x10  // Seek error (Type I)
#define FDC_STATUS_WRITE_PROT 0x40
#define FDC_STATUS_NOT_READY  0x80

// System control register bits (Beta Disk)
#define FDC_CONTROL_DRIVE_MASK 0x03  // Drive select (0-3)
#define FDC_CONTROL_SIDE       0x10  // Side select (0=A, 1=B)
#define FDC_CONTROL_DENSITY    0x40  // Density (0=SD, 1=DD)
#define FDC_CONTROL_HLT        0x08  // Head Load Timing

// FDC state
typedef enum {
    FDC_STATE_IDLE,
    FDC_STATE_BUSY,
    FDC_STATE_READ_DATA,
    FDC_STATE_WRITE_DATA
} fdc_state_t;

// FDC structure
typedef struct {
    // Registers
    uint8_t  status;
    uint8_t  track;
    uint8_t  sector;
    uint8_t  data;
    uint8_t  command;
    
    // Control
    uint8_t  control;       // System control register
    uint8_t  current_drive; // Currently selected drive (0-3)
    uint8_t  current_side;  // Currently selected side (0-1)
    
    // State
    fdc_state_t state;
    uint32_t    delay_tstates; // T-states remaining for current operation
    
    // Data transfer
    uint8_t     sector_buffer[256];
    int         buffer_pos;
    int         buffer_len;
    
    // Attached drives
    trd_image_t* drives[4];  // Up to 4 drives
    
    // IRQ/DRQ callbacks
    void (*irq_callback)(bool state);
    void (*drq_callback)(bool state);
    
} fdc_t;

// FDC API
void fdc_init(fdc_t* fdc);
void fdc_reset(fdc_t* fdc);
void fdc_attach_image(fdc_t* fdc, int drive, trd_image_t* img);
void fdc_detach_image(fdc_t* fdc, int drive);

// Port I/O
uint8_t fdc_port_in(fdc_t* fdc, uint16_t port);
void fdc_port_out(fdc_t* fdc, uint16_t port, uint8_t val);

// Timing - call this regularly to advance FDC state
void fdc_step(fdc_t* fdc, uint32_t tstates);

// Set IRQ/DRQ callbacks
void fdc_set_irq_callback(fdc_t* fdc, void (*callback)(bool state));
void fdc_set_drq_callback(fdc_t* fdc, void (*callback)(bool state));

#endif // DISK_FDC_H

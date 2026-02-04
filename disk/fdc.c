/*
 * FDC - WD1793-compatible Floppy Disk Controller Implementation
 */

#include "fdc.h"
#include <string.h>
#include <stdio.h>

// Initialize FDC
void fdc_init(fdc_t* fdc) {
    memset(fdc, 0, sizeof(fdc_t));
    fdc->status = FDC_STATUS_NOT_READY;
    fdc->state = FDC_STATE_IDLE;
    fdc->current_drive = 0;
    fdc->current_side = 0;
    printf("[DEBUG] FDC initialized.\n");
}

// Reset FDC
void fdc_reset(fdc_t* fdc) {
    fdc->status = FDC_STATUS_NOT_READY;
    fdc->track = 0;
    fdc->sector = 1;
    fdc->data = 0;
    fdc->command = 0;
    fdc->state = FDC_STATE_IDLE;
    fdc->delay_tstates = 0;
    fdc->buffer_pos = 0;
    fdc->buffer_len = 0;
    printf("[DEBUG] FDC reset.\n");
}

// Attach disk image to drive
void fdc_attach_image(fdc_t* fdc, int drive, trd_image_t* img) {
    if (drive < 0 || drive >= 4) return;
    fdc->drives[drive] = img;

    // Update status - drive is ready if image attached
    if (img) {
        fdc->status &= ~FDC_STATUS_NOT_READY;
        printf("[DEBUG] FDC: Attached disk to drive %d\n", drive);
    }
}

// Detach disk image from drive
void fdc_detach_image(fdc_t* fdc, int drive) {
    if (drive < 0 || drive >= 4) return;
    fdc->drives[drive] = NULL;

    // Update status
    bool any_ready = false;
    for (int i = 0; i < 4; i++) {
        if (fdc->drives[i]) {
            any_ready = true;
            break;
        }
    }
    if (!any_ready) {
        fdc->status |= FDC_STATUS_NOT_READY;
    }
    printf("[DEBUG] FDC: Detached disk from drive %d\n", drive);
}

// Get current drive image
static trd_image_t* fdc_get_current_drive(fdc_t* fdc) {
    return fdc->drives[fdc->current_drive];
}

// Execute FDC command
static void fdc_execute_command(fdc_t* fdc, uint8_t cmd) {
    printf("[DEBUG] FDC Execute Command: 0x%02X\n", cmd);
    fdc->command = cmd;
    fdc->status |= FDC_STATUS_BUSY;
    fdc->status &= ~(FDC_STATUS_DRQ | FDC_STATUS_LOST_DATA | FDC_STATUS_CRC_ERROR | FDC_STATUS_RNF);

    uint8_t cmd_type = cmd & 0xF0;

    switch (cmd_type) {
        case FDC_CMD_RESTORE: // Restore (seek to track 0)
            printf("[DEBUG] FDC Command: RESTORE\n");
            fdc->track = 0;
            fdc->delay_tstates = 3500 * 6; // ~6ms for restore
            fdc->state = FDC_STATE_BUSY;
            break;

        case FDC_CMD_SEEK: // Seek to track in data register
            printf("[DEBUG] FDC Command: SEEK to track %d\n", fdc->data);
            {
                int diff = (fdc->data > fdc->track) ? (fdc->data - fdc->track) : (fdc->track - fdc->data);
                fdc->track = fdc->data;
                fdc->delay_tstates = 3500 * (6 + diff); // ~6ms + step time
                fdc->state = FDC_STATE_BUSY;
            }
            break;

        case FDC_CMD_STEP: // Step (no update)
        case FDC_CMD_STEP_IN: // Step in
            printf("[DEBUG] FDC Command: STEP\n");
            if (cmd_type == FDC_CMD_STEP_IN && fdc->track < 79) {
                fdc->track++;
            }
            fdc->delay_tstates = 3500 * 6; // ~6ms per step
            fdc->state = FDC_STATE_BUSY;
            break;

        case FDC_CMD_STEP_OUT: // Step out
            printf("[DEBUG] FDC Command: STEP OUT\n");
            if (fdc->track > 0) {
                fdc->track--;
            }
            fdc->delay_tstates = 3500 * 6;
            fdc->state = FDC_STATE_BUSY;
            break;

        case FDC_CMD_READ_SECTOR: // Read sector
            printf("[DEBUG] FDC Command: READ SECTOR (Track=%d, Sector=%d, Drive=%d)\n",
                   fdc->track, fdc->sector, fdc->current_drive);
            trd_image_t* img = fdc_get_current_drive(fdc);
                if (!img) {
                    fdc->status |= FDC_STATUS_RNF;
                    fdc->status &= ~FDC_STATUS_BUSY;
                    fdc->state = FDC_STATE_IDLE;
                    if (fdc->irq_callback) fdc->irq_callback(true);
                    break;
                }
                
                // TR-DOS uses 1-based sector numbers, convert to 0-based for internal use
                uint8_t sector_num = (fdc->sector > 0) ? (fdc->sector - 1) : 0;
                
                if (trd_read_sector(img, fdc->track, fdc->current_side, sector_num, fdc->sector_buffer)) {
                    fdc->buffer_pos = 0;
                    fdc->buffer_len = 256;
                    fdc->state = FDC_STATE_READ_DATA;
                    fdc->delay_tstates = 3500 * 10; // ~10ms to start transfer
                    fdc->status |= FDC_STATUS_DRQ;
                    if (fdc->drq_callback) fdc->drq_callback(true);
                } else {
                    fdc->status |= FDC_STATUS_RNF;
                    fdc->status &= ~FDC_STATUS_BUSY;
                    fdc->state = FDC_STATE_IDLE;
                    if (fdc->irq_callback) fdc->irq_callback(true);
                }
            
            break;
		
		case FDC_CMD_WRITE_SECTOR: // Write sector
            {
				printf("[DEBUG] FDC Command: WRITE SECTOR (Track=%d, Sector=%d, Drive=%d)\n",
                   fdc->track, fdc->sector, fdc->current_drive);
                trd_image_t* img = fdc_get_current_drive(fdc);
                if (!img) {
                    fdc->status |= FDC_STATUS_RNF;
                    fdc->status &= ~FDC_STATUS_BUSY;
                    fdc->state = FDC_STATE_IDLE;
                    if (fdc->irq_callback) fdc->irq_callback(true);
                    break;
                }
                
                if (img->read_only) {
                    fdc->status |= FDC_STATUS_WRITE_PROT;
                    fdc->status &= ~FDC_STATUS_BUSY;
                    fdc->state = FDC_STATE_IDLE;
                    if (fdc->irq_callback) fdc->irq_callback(true);
                    break;
                }
                
                fdc->buffer_pos = 0;
                fdc->buffer_len = 256;
                fdc->state = FDC_STATE_WRITE_DATA;
                fdc->delay_tstates = 3500 * 10; // ~10ms to start transfer
                fdc->status |= FDC_STATUS_DRQ;
                if (fdc->drq_callback) fdc->drq_callback(true);
            }
            break;
            
        case FDC_CMD_READ_ADDRESS: // Read address
			printf("[DEBUG] FDC Command: READ_ADDRESS\n");
            // Return ID field of current sector
            fdc->sector_buffer[0] = fdc->track;
            fdc->sector_buffer[1] = fdc->current_side;
            fdc->sector_buffer[2] = fdc->sector;
            fdc->sector_buffer[3] = 1; // 256 bytes
            fdc->buffer_pos = 0;
            fdc->buffer_len = 6;
            fdc->state = FDC_STATE_READ_DATA;
            fdc->delay_tstates = 3500 * 10;
            fdc->status |= FDC_STATUS_DRQ;
            if (fdc->drq_callback) fdc->drq_callback(true);
            break;
            
        case FDC_CMD_FORCE_INT: // Force interrupt
			printf("[DEBUG] FDC Command: FORCE_INT\n");
            fdc->status &= ~FDC_STATUS_BUSY;
            fdc->state = FDC_STATE_IDLE;
            fdc->delay_tstates = 0;
            if (cmd & 0x0F) { // Immediate interrupt requested
                if (fdc->irq_callback) fdc->irq_callback(true);
            }
            break;

        default:
            printf("[DEBUG] FDC Command: UNKNOWN (0x%02X)\n", cmd_type);
            break;
    }
}

// Port IN
uint8_t fdc_port_in(fdc_t* fdc, uint16_t port) {
    uint8_t port_low = port & 0xFF;

    printf("[DEBUG] FDC Port IN Access: port=0x%02X\n", port_low);
    switch (port_low) {
        case FDC_PORT_STATUS: // Status register
            printf("[DEBUG] FDC_PORT_STATUS read, status=0x%02X\n", fdc->status);
            return fdc->status;

        case FDC_PORT_TRACK: // Track register
            printf("[DEBUG] FDC_PORT_TRACK read, track=0x%02X\n", fdc->track);
            return fdc->track;

        case FDC_PORT_SECTOR: // Sector register
            printf("[DEBUG] FDC_PORT_SECTOR read, sector=0x%02X\n", fdc->sector);
            return fdc->sector;

        case FDC_PORT_DATA: // Data register
            printf("[DEBUG] FDC_PORT_DATA read: data=0x%02X, buffer_pos=%d, buffer_len=%d\n",
                   fdc->data, fdc->buffer_pos, fdc->buffer_len);
            if (fdc->state == FDC_STATE_READ_DATA && fdc->buffer_pos < fdc->buffer_len) {
                fdc->data = fdc->sector_buffer[fdc->buffer_pos++];
                if (fdc->buffer_pos >= fdc->buffer_len) {
                    fdc->status &= ~(FDC_STATUS_DRQ | FDC_STATUS_BUSY);
                    fdc->state = FDC_STATE_IDLE;
                    if (fdc->drq_callback) fdc->drq_callback(false);
                    if (fdc->irq_callback) fdc->irq_callback(true);
                }
            }
            return fdc->data;

        case FDC_PORT_CONTROL: // System control
            printf("[DEBUG] FDC_PORT_CONTROL read, control=0x%02X\n", fdc->control);
            return fdc->control;

        default:
            printf("[DEBUG] Unknown FDC Port IN access: port=0x%02X\n", port_low);
            return 0xFF;
    }
}

// Port OUT
void fdc_port_out(fdc_t* fdc, uint16_t port, uint8_t val) {
    uint8_t port_low = port & 0xFF;

    printf("[DEBUG] FDC Port OUT Access: port=0x%02X, value=0x%02X\n", port_low, val);
    switch (port_low) {
        case FDC_PORT_STATUS: // Command register
            printf("[DEBUG] Writing to FDC_PORT_STATUS: command=0x%02X\n", val);
            fdc_execute_command(fdc, val);
            break;

        case FDC_PORT_TRACK: // Track register
            printf("[DEBUG] Writing to FDC_PORT_TRACK: track=0x%02X\n", val);
            fdc->track = val;
            break;

        default:
            break;
    }
}

// Advance FDC state by tstates
void fdc_step(fdc_t* fdc, uint32_t tstates) {
    if (fdc->delay_tstates > 0) {
        if (tstates >= fdc->delay_tstates) {
            fdc->delay_tstates = 0;

            // Operation complete
            if (fdc->state == FDC_STATE_BUSY) {
                fdc->status &= ~FDC_STATUS_BUSY;
                fdc->state = FDC_STATE_IDLE;
                if (fdc->irq_callback) fdc->irq_callback(true);
            }
        } else {
            fdc->delay_tstates -= tstates;
        }
    }
}

// Set callbacks
void fdc_set_irq_callback(fdc_t* fdc, void (*callback)(bool state)) {
    fdc->irq_callback = callback;
}

void fdc_set_drq_callback(fdc_t* fdc, void (*callback)(bool state)) {
    fdc->drq_callback = callback;
}
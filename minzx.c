/*
 * ZX Spectrum 48K Emulator con SDL2 + JGZ80
 * - Carga .TAP por pulsos (pilot/sync/data) compatible ROM
 * - Carga .TZX por pulsos:
 *   - Soportados: 0x00(=0x10), 0x02(=0x12), 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
 *                 0x18 (CSW raw sin compresión), 0x19 (Generalized Data),
 *                 0x20, 0x21, 0x22, 0x24/0x25, 0x2A, 0x2B,
 *                 0x30, 0x31, 0x32, 0x33, 0x35, 0x5A
 * - Carga .SNA (48K)
 * - BRIGHT aplicado a ink y paper
 * - Puerto FE: bit 6 = EAR (tape); bit 7 = espejo de bit 3 del último OUT
 * - Beeper/speaker por eventos con timestamp (T-states) → audio estable
 * - Soporte TR-DOS: imágenes .TRD y .SCL con emulación WD1793 FDC
 *
 * Compilar LINUX:     gcc minzx.c jgz80/z80.c disk/trd.c disk/scl.c disk/fdc.c -o minzx -lSDL2 -lm
 * Compilar WIN/MSYS2: gcc minzx.c jgz80/z80.c disk/trd.c disk/scl.c disk/fdc.c -o minzx.exe -lmingw32 -lSDL2main -lSDL2
 * Uso: ./minzx [archivo.tap|tzx|sna|trd|scl] [--ro] [--trdos-rom file.rom] [--drive-count N]
 */

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#ifdef _WIN32
#define strcasecmp _stricmp
#endif

#include "jgz80/z80.h"
#include "disk/trd.h"
#include "disk/scl.h"
#include "disk/fdc.h"

#define SCREEN_WIDTH   256
#define SCREEN_HEIGHT  192
#define SCALE          1

#define v_border_top    64
#define v_border_bottom 56
#define h_border        48
#define FULL_WIDTH   (SCREEN_WIDTH  + 2 * h_border)
#define FULL_HEIGHT  (v_border_top + SCREEN_HEIGHT + v_border_bottom)

#define ROM_SIZE       16384
#define RAM_START      16384
#define MEMORY_SIZE    (64*1024)
#define CYCLES_PER_FRAME 69888

// ─────────────────────────────────────────────────────────────
// Globals
// ─────────────────────────────────────────────────────────────
uint8_t memory[MEMORY_SIZE];
z80 cpu;

SDL_Window*   window   = NULL;
SDL_Renderer* renderer = NULL;
SDL_Texture*  texture  = NULL;
uint32_t pixels[FULL_HEIGHT * FULL_WIDTH];

uint8_t border_color   = 7;
uint8_t last_fe_write  = 0x00;  // espejo bit 3 → leído como bit 7 en FE

uint8_t keyboard[8]    = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

int      cycles_done   = 0;     // puede replegarse por frame
uint64_t global_cycles = 0;     // SIEMPRE crece → referencia para cinta/audio

// ─────────────────────────────────────────────────────────────
// TR-DOS / FDC
// ─────────────────────────────────────────────────────────────
fdc_t fdc;
trd_image_t* disk_images[4] = {NULL, NULL, NULL, NULL};
scl_image_t* scl_images[4] = {NULL, NULL, NULL, NULL};
bool trdos_enabled = false;

// TR-DOS ROM support
uint8_t trdos_rom[ROM_SIZE];
bool trdos_rom_loaded = false;
bool trdos_rom_active = false;  // true when TR-DOS ROM is mapped

// Configuración de audio
SDL_AudioDeviceID audio_dev;
SDL_AudioSpec want;

#define SAMPLE_RATE 44100
#define CPU_HZ 3500000
#define AUDIO_SAMPLES_PER_FRAME (SAMPLE_RATE / 50) // Aprox 882 muestras por frame
#define BUFFER_SIZE (1024)

uint32_t last_audio_tstates = 0; // T-states acumulados desde la última muestra
int16_t audio_buffer[BUFFER_SIZE];
int audio_ptr = 0;
uint8_t current_speaker_level = 0; // Estado actual del bit 4 de 0xFE


// Colores ZX con alfa (0xAARRGGBB)
uint32_t zx_colors[16] = {
    0xFF000000, 0xFF0000D8, 0xFFD80000, 0xFFD800D8,
    0xFF00D800, 0xFF00D8D8, 0xFFD8D800, 0xFFD8D8D8,
    0xFF000000, 0xFF0000FF, 0xFFFF0000, 0xFFFF00FF,
    0xFF00FF00, 0xFF00FFFF, 0xFFFFFF00, 0xFFFFFFFF
};


// ─────────────────────────────────────────────────────────────
// Utilidades lectura LE
// ─────────────────────────────────────────────────────────────
static inline uint8_t rd_u8(FILE* f) { int c = fgetc(f); return (c == EOF) ? 0 : (uint8_t)c; }
static inline uint16_t rd_u16(FILE* f) { uint16_t lo = rd_u8(f), hi = rd_u8(f); return (uint16_t)(lo | (hi << 8)); }
static inline uint32_t rd_u24(FILE* f) { uint32_t b0 = rd_u8(f), b1 = rd_u8(f), b2 = rd_u8(f); return (b0 | (b1 << 8) | (b2 << 16)); }
static inline uint32_t rd_u32(FILE* f) { uint32_t b0 = rd_u8(f), b1 = rd_u8(f), b2 = rd_u8(f), b3 = rd_u8(f); return (b0 | (b1 << 8) | (b2 << 16) | (b3 << 24)); }
#define MS_TO_TSTATES(ms) ((uint64_t)((ms) * 3500ULL)) // 3.5MHz

// ─────────────────────────────────────────────────────────────
// Motor de cinta unificado (TAP/TZX)
// ─────────────────────────────────────────────────────────────
typedef enum { TAPE_FMT_NONE=0, TAPE_FMT_TAP=1, TAPE_FMT_TZX=2 } tape_fmt_t;
typedef enum { PH_IDLE, PH_PILOT, PH_SYNC1, PH_SYNC2, PH_DATA, PH_PURE_TONE, PH_PULSE_SEQ, PH_DIRECT_REC, PH_PAUSE } pulse_phase_t;

typedef struct {
    FILE*   f;
    long    file_size;
    long    file_pos;

    // Formato
    tape_fmt_t fmt;

    // Estado de reproducción
    pulse_phase_t phase;
    int      pulses_left;            // pilot/pure tone en medias ondas
    uint32_t halfwave_ts;            // duración media onda (T-states)
    uint64_t next_edge_cycle;        // T-state del próximo flanco
    bool     level;                  // EAR actual (true=1)

    // Datos de bits
    uint8_t* blk;
    uint32_t blk_len;
    uint32_t data_pos;
    uint8_t  cur_byte;
    int      cur_bit;                // 7..0
    int      pulse_of_bit;           // 0/1

    // Parámetros activos (TZX/TAP parametrizadas)
    uint16_t t_pilot, t_sync1, t_sync2, t_bit0, t_bit1;
    uint16_t pilot_pulses;           // en ondas completas
    uint16_t used_bits_last;         // 0 => 8
    uint32_t pause_ms;

    // Secuencias (0x13 / 0x18 / 0x19)
    uint16_t* pulse_seq;
    int       pulse_seq_n;
    int       pulse_seq_i;

    // Direct recording (0x15)
    uint16_t dr_tstates_per_sample;
    uint32_t dr_total_bits;
    uint32_t dr_bit_index;

    // CSW (0x18) — como secuencia de medias ondas ya convertidas
    uint32_t csw_freq_hz;
    uint8_t  csw_compression;        // 0=raw sin compresión (soportado)
    uint32_t csw_data_len;

    // Control
    double speed;                    // solo TAP (escala)
    bool   playing;

    // Nivel inicial (0x2B)
    bool   initial_level_known;
    bool   initial_level;

    // Loop (0x24/0x25)
    struct { long file_pos_at_loop; uint16_t remaining; int active; } loop;

    // Grupos (0x21/0x22) - no afecta reproducción; útil para validar
    int group_depth;

} tape_t;

static tape_t tape = {0};
static const char* tape_filename = NULL;

bool load_tap(const char* filename);

// ─────────────────────────────────────────────────────────────
// Timings TAP por defecto (T-states) @3.5MHz
// ─────────────────────────────────────────────────────────────
static const int TS_PILOT = 2168;
static const int TS_SYNC1 = 667;
static const int TS_SYNC2 = 735;
static const int TS_BIT0  = 855;
static const int TS_BIT1  = 1710;

static inline uint32_t halfwave_for_bit(bool bit1) { return bit1 ? tape.t_bit1 : tape.t_bit0; }

// ─────────────────────────────────────────────────────────────
// Listado de bloques TAP/TZX (para mostrar al cargar)
// ─────────────────────────────────────────────────────────────
static void list_tap_blocks(const char* filename) {
    FILE* f = fopen(filename, "rb"); if (!f) { printf("No se pudo abrir TAP para listar: %s\n", filename); return; }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    printf("=== LISTA TAP: %s (%ld bytes) ===\n", filename, fsz);
    int idx=0;
    while (1) {
        uint8_t len_le[2];
        if (fread(len_le,1,2,f)!=2) break;
        uint16_t len = (uint16_t)(len_le[0] | (len_le[1]<<8));
        if (len==0 || len > 65535) { printf("Bloque %d: longitud inválida %u\n", idx, len); break; }
        uint8_t first=0xFF;
        long pos=ftell(f);
        if (len>=1) { fread(&first,1,1,f); fseek(f, pos, SEEK_SET); }
        printf("Bloque %3d: len=%5u  flag=0x%02X (%s)\n", idx, len, first, (first==0x00?"HEADER/flag=0x00":(first==0xFF?"DATA/flag=0xFF":"?")));
        fseek(f, len, SEEK_CUR);
        idx++;
    }
    fclose(f);
}

static const char* tzx_name(uint8_t id) {
    switch (id) {
        case 0x00: return "Standard Speed Data (legacy alias)";
        case 0x02: return "Pure Tone (legacy alias)";
        case 0x10: return "Standard Speed Data";
        case 0x11: return "Turbo Speed Data";
        case 0x12: return "Pure Tone";
        case 0x13: return "Pulse Sequence";
        case 0x14: return "Pure Data";
        case 0x15: return "Direct Recording";
        case 0x18: return "CSW Recording";
        case 0x19: return "Generalized Data";
        case 0x20: return "Pause";
        case 0x21: return "Group Start";
        case 0x22: return "Group End";
        case 0x24: return "Loop Start";
        case 0x25: return "Loop End";
        case 0x2A: return "Stop if 48K";
        case 0x2B: return "Set Signal Level";
        case 0x30: return "Text Description";
        case 0x31: return "Message";
        case 0x32: return "Archive Info";        // añadido
        case 0x33: return "Hardware Type";
        case 0x35: return "Custom Info";
        case 0x5A: return "Glue";
        default:   return "Desconocido/No soportado";
    }
}

// Nombres amistosos para algunos campos de Archive Info (0x32)
static const char* tzx_archive_field_name(uint8_t id) {
    switch (id) {
        case 0x00: return "Título";
        case 0x01: return "Editorial/Publisher";
        case 0x02: return "Autor";
        case 0x03: return "Año";
        case 0x04: return "Idioma";
        case 0x05: return "Tipo/Género";
        case 0x06: return "Precio";
        case 0x07: return "Protección";
        case 0x08: return "Origen";
        case 0x09: return "Comentario";
        default:   return "Campo";
    }
}

static void list_tzx_blocks(const char* filename) {
    FILE* f = fopen(filename, "rb"); if (!f) { printf("No se pudo abrir TZX para listar: %s\n", filename); return; }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    char hdr[10]={0};
    if (fread(hdr,1,10,f)<10 || memcmp(hdr,"ZXTape!\x1A",8)!=0) { printf("TZX inválido: %s\n", filename); fclose(f); return; }
    printf("=== LISTA TZX: %s (%ld bytes) v%d.%02d ===\n", filename, fsz, (unsigned char)hdr[8], (unsigned char)hdr[9]);

    int idx=0; long file_pos=10;
    while (file_pos < fsz) {
        int id = rd_u8(f); file_pos++;
        printf("Bloque %3d: 0x%02X  %-22s", idx, id, tzx_name((uint8_t)id));
        // Intento de salto según formato conocido (solo para listar; omite parse fino):
        switch (id) {
            case 0x00: // alias 0x10
            case 0x10: { uint16_t pause=rd_u16(f); uint16_t dlen=rd_u16(f); file_pos+=4; fseek(f, dlen, SEEK_CUR); file_pos+=dlen; printf("  (pause=%ums, len=%u)\n", pause, dlen); } break;
            case 0x02: // alias 0x12
            case 0x12: { uint16_t tone=rd_u16(f); uint16_t pulses=rd_u16(f); file_pos+=4; printf("  (tone=%u, pulses=%u)\n", tone,pulses); } break;
            case 0x11: { fseek(f, 2+2+2+2+2+2+1+2, SEEK_CUR); file_pos += 2+2+2+2+2+2+1+2; uint32_t dlen=rd_u24(f); file_pos+=3; fseek(f,dlen,SEEK_CUR); file_pos+=dlen; printf("  (turbo)\n"); } break;
            case 0x13: { uint8_t n=rd_u8(f); file_pos++; fseek(f, n*2, SEEK_CUR); file_pos+=n*2; printf("  (seq=%u)\n", n); } break;
            case 0x14: { fseek(f, 2+2+1+2, SEEK_CUR); file_pos += 2+2+1+2; uint32_t dlen=rd_u24(f); file_pos+=3; fseek(f,dlen,SEEK_CUR); file_pos+=dlen; printf("  (pure data len=%u)\n", dlen); } break;
            case 0x15: { fseek(f, 2+2+1, SEEK_CUR); file_pos+=2+2+1; uint32_t dlen=rd_u24(f); file_pos+=3; fseek(f,dlen,SEEK_CUR); file_pos+=dlen; printf("  (direct rec len=%u)\n", dlen); } break;
            case 0x18: { uint16_t pause=rd_u16(f); uint32_t freq=rd_u32(f); uint8_t comp=rd_u8(f); uint32_t dlen=rd_u32(f); file_pos+=2+4+1+4; fseek(f,dlen,SEEK_CUR); file_pos+=dlen; printf("  (CSW: pause=%ums, %uHz, comp=%u, data=%u)\n", pause, freq, comp, dlen); } break;
            case 0x19: { uint32_t blen=rd_u32(f); file_pos+=4; fseek(f,blen,SEEK_CUR); file_pos+=blen; printf("  (GDB len=%u)\n", blen); } break;
            case 0x20: { uint16_t ms=rd_u16(f); file_pos+=2; printf("  (pause=%u)\n", ms); } break;
            case 0x21: { uint8_t l=rd_u8(f); file_pos++; fseek(f,l,SEEK_CUR); file_pos+=l; printf("  (group)\n"); } break;
            case 0x22: { printf("\n"); } break;
            case 0x24: { uint16_t c=rd_u16(f); file_pos+=2; printf("  (loop start x%u)\n", c); } break;
            case 0x25: { printf("  (loop end)\n"); } break;
            case 0x2A: { printf("  (stop if 48K)\n"); } break;
            case 0x2B: { uint8_t lvl=rd_u8(f); file_pos++; printf("  (level=%u)\n", lvl); } break;
            case 0x30: { uint8_t l=rd_u8(f); file_pos++; fseek(f,l,SEEK_CUR); file_pos+=l; printf("  (text)\n"); } break;
            case 0x31: { uint8_t d=rd_u8(f); uint8_t l=rd_u8(f); file_pos+=2; fseek(f,l,SEEK_CUR); file_pos+=l; printf("  (message %us)\n", d); } break;

            case 0x32: { // Archive Info: listar sus campos
                uint16_t blen = rd_u16(f); file_pos += 2;
#if 1
                long end = file_pos + blen;
                if (end > fsz) end = fsz;

                if (file_pos >= end) { printf("  (archive info vacio)\n"); break; }

                uint8_t n = rd_u8(f); file_pos += 1;
                printf("  (archive info, %u campo%s)\n", n, (n==1?"":"s"));

                for (uint8_t i=0; i<n && file_pos < end; ++i) {
                    if (file_pos + 1 > end) break;
                    uint8_t tid = rd_u8(f); file_pos += 1;

                    if (file_pos + 1 > end) break;
                    uint16_t slen = rd_u8(f); file_pos += 1;

                    long remain = end - file_pos; if (remain < 0) remain = 0;
                    uint16_t toread = (slen > (uint16_t)remain) ? (uint16_t)remain : slen;

                    char* buf = (toread > 0) ? (char*)malloc((size_t)toread) : NULL;
                    if (buf && toread > 0) { size_t rd = fread(buf, 1, toread, f); (void)rd; }
                    if (toread < slen) fseek(f, slen - toread, SEEK_CUR);

                    file_pos += slen;

                    const char* fname = tzx_archive_field_name(tid);
                    if (buf && toread > 0)
                        printf("           - %s [0x%02X]: %.*s\n", fname, tid, (int)toread, buf);
                    else
                        printf("           - %s [0x%02X]: <vacío>\n", fname, tid);
                    free(buf);
                }

                if (file_pos < end) { fseek(f, end - file_pos, SEEK_CUR); file_pos = end; }
#endif
            } break;

            case 0x33: { uint8_t n=rd_u8(f); file_pos++; fseek(f, n*3, SEEK_CUR); file_pos+=n*3; printf("  (hw %u)\n", n); } break;
            case 0x35: { fseek(f, 16, SEEK_CUR); file_pos+=16; { uint32_t l=rd_u32(f); file_pos+=4; fseek(f,l,SEEK_CUR); file_pos+=l; } printf("  (custom)\n"); } break;
            case 0x5A: { uint32_t l=rd_u32(f); file_pos+=4; fseek(f,l,SEEK_CUR); file_pos+=l; printf("  (glue)\n"); } break;
            default: { printf("  (no sé saltarlo; paro listado)\n"); fclose(f); return; }
        }
        idx++;
    }
    fclose(f);
}

// ─────────────────────────────────────────────────────────────
// TAP (integrado en motor unificado) + Trazas
// ─────────────────────────────────────────────────────────────
static bool tap_read_next_block();
static void  start_block_emission(uint64_t now_cycle);
static bool  tap_ear_level_until(uint64_t now_cycle);

static bool tap_read_next_block() {
    if (!tape.f) return false;
    if (tape.file_pos >= tape.file_size) return false;

    uint8_t len_le[2];
    if (fread(len_le, 1, 2, tape.f) != 2) return false;
    tape.file_pos += 2;

    uint16_t len = (uint16_t)(len_le[0] | (len_le[1] << 8));
    if (len == 0) return false;

    free(tape.blk);
    tape.blk = (uint8_t*)malloc(len);
    if (!tape.blk) return false;

    if (fread(tape.blk, 1, len, tape.f) != len) return false;
    tape.file_pos += len;
    tape.blk_len = len;

    tape.t_pilot       = 2168;
    tape.t_sync1       = 667;
    tape.t_sync2       = 735;
    tape.t_bit0        = 855;
    tape.t_bit1        = 1710;
    tape.used_bits_last= 8;
    tape.pilot_pulses  = (tape.blk[0] == 0x00) ? 8063 : 3223;
    tape.pause_ms      = 1000;

    printf("[TAP] Nuevo bloque: len=%u flag=0x%02X pilot=%u pause=%ums\n",
           len, tape.blk[0], tape.pilot_pulses, tape.pause_ms);
    return true;
}

static void start_block_emission(uint64_t now_cycle) {
    tape.phase       = PH_PILOT;
    tape.pulses_left = tape.pilot_pulses * 2;
    tape.halfwave_ts = tape.t_pilot;
    if (tape.fmt == TAPE_FMT_TAP && tape.speed > 0.0)
        tape.halfwave_ts = (uint32_t)(tape.halfwave_ts / tape.speed);
    tape.next_edge_cycle = now_cycle + tape.halfwave_ts;
    tape.level = tape.initial_level_known ? tape.initial_level : true;
    tape.data_pos = 0;
    tape.cur_bit  = 7;
    tape.pulse_of_bit = 0;
}

static void start_pause(uint64_t now_cycle) {
    tape.phase = PH_PAUSE;
    uint32_t pause_ts = (tape.pause_ms == 0) ? MS_TO_TSTATES(0) : MS_TO_TSTATES(tape.pause_ms);
    tape.next_edge_cycle = now_cycle + pause_ts;
    tape.level = true;
}

static bool tap_ear_level_until(uint64_t now_cycle) {
    if (!tape.playing || !tape.f || tape.phase == PH_IDLE) return true;
    while (now_cycle >= tape.next_edge_cycle) {
        tape.level = !tape.level;
        switch (tape.phase) {
            case PH_PILOT:
                if (--tape.pulses_left > 0) {
                    tape.next_edge_cycle += tape.halfwave_ts;
                } else {
                    tape.phase = PH_SYNC1;
                    tape.halfwave_ts = (tape.fmt==TAPE_FMT_TAP && tape.speed>0.0) ? (uint32_t)(tape.t_sync1 / tape.speed) : tape.t_sync1;
                    tape.next_edge_cycle += tape.halfwave_ts;
                }
                break;
            case PH_SYNC1:
                tape.phase = PH_SYNC2;
                tape.halfwave_ts = (tape.fmt==TAPE_FMT_TAP && tape.speed>0.0) ? (uint32_t)(tape.t_sync2 / tape.speed) : tape.t_sync2;
                tape.next_edge_cycle += tape.halfwave_ts;
                break;
            case PH_SYNC2:
                tape.phase = PH_DATA;
                tape.data_pos = 0; tape.cur_bit = 7; tape.pulse_of_bit = 0;
                tape.cur_byte = tape.blk[tape.data_pos++];
                {
                    bool b = (tape.cur_byte & 0x80) != 0;
                    tape.halfwave_ts = halfwave_for_bit(b);
                    if (tape.fmt==TAPE_FMT_TAP && tape.speed>0.0) tape.halfwave_ts = (uint32_t)(tape.halfwave_ts / tape.speed);
                    tape.next_edge_cycle += tape.halfwave_ts;
                }
                break;
            case PH_DATA: {
                tape.pulse_of_bit ^= 1;
                if (tape.pulse_of_bit == 1) {
                    tape.next_edge_cycle += tape.halfwave_ts;
                } else {
                    if (--tape.cur_bit < 0) {
                        if (tape.data_pos >= tape.blk_len) {
                            start_pause(now_cycle);
                            break;
                        }
                        tape.cur_bit = 7;
                        tape.cur_byte = tape.blk[tape.data_pos++];
                    }
                    bool b = ((tape.cur_byte >> tape.cur_bit) & 1) != 0;
                    tape.halfwave_ts = halfwave_for_bit(b);
                    if (tape.fmt==TAPE_FMT_TAP && tape.speed>0.0) tape.halfwave_ts = (uint32_t)(tape.halfwave_ts / tape.speed);
                    tape.next_edge_cycle += tape.halfwave_ts;
                }
            } break;
            case PH_PAUSE:
                if (!tap_read_next_block()) {
                    tape.phase = PH_IDLE; tape.playing = false; tape.level = true;
                } else {
                    start_block_emission(now_cycle);
                }
                break;
            default: break;
        }
        if (tape.phase == PH_IDLE) break;
    }
    return tape.level;
}

// ─────────────────────────────────────────────────────────────
// Helpers específicos para 0x19 (Generalized Data)
// ─────────────────────────────────────────────────────────────
static inline int ceil_log2_u16(int v) {
    if (v <= 1) return 1;
    int n = 0, p = 1;
    while (p < v) { p <<= 1; n++; }
    return n;
}

typedef struct {
    uint8_t  flags;    // b0..b1: 00 edge, 01 same, 10 force low, 11 force high
    uint16_t *pulses;  // lista de medias-ondas (T-states) sin los ceros de terminación
    int      npulses;  // número de medias-ondas válidas
} tzx19_symdef_t;

// Añade o fusiona una media-onda en la secuencia de salida
static bool push_or_merge_halfwave(uint16_t **seq, int *cap, int *n, uint16_t dur, bool merge) {
    if (dur == 0) return true; // 0 en definición indica fin; no se añade
    if (merge && *n > 0) {
        uint32_t ext = (uint32_t)(*seq)[*n - 1] + (uint32_t)dur;
        (*seq)[*n - 1] = (ext > 65535) ? 65535 : (uint16_t)ext;
        return true;
    }
    if (*n >= *cap) {
        int ncap = (*cap == 0) ? 512 : (*cap * 2);
        uint16_t *tmp = (uint16_t*)realloc(*seq, sizeof(uint16_t)*ncap);
        if (!tmp) return false;
        *seq = tmp; *cap = ncap;
    }
    (*seq)[(*n)++] = dur ? dur : 1;
    return true;
}

// ─────────────────────────────────────────────────────────────
// TZX  (incluye alias 0x00→0x10, 0x02→0x12, 0x18 básico, 0x19 completo + Trazas)
// ─────────────────────────────────────────────────────────────
static bool tzx_read_and_prepare_next_block(uint64_t now);

static void tzx_prepare_standard_or_turbo(uint64_t now) {
    // Nivel base (EAR alto salvo que se especifique con 0x2B)
    tape.level = tape.initial_level_known ? tape.initial_level : true;

    // Si hay piloto, se emite; si no, saltamos a SYNC o DATA
    if (tape.pilot_pulses > 0 && tape.t_pilot > 0) {
        tape.phase       = PH_PILOT;
        tape.pulses_left = tape.pilot_pulses * 2;   // medias ondas
        tape.halfwave_ts = tape.t_pilot;
        tape.next_edge_cycle = now + tape.halfwave_ts;
    } else if (tape.t_sync1 > 0) {
        tape.phase       = PH_SYNC1;
        tape.halfwave_ts = tape.t_sync1;
        tape.next_edge_cycle = now + tape.halfwave_ts;
    } else if (tape.t_bit0 || tape.t_bit1) {
        tape.phase      = PH_DATA;
        tape.data_pos   = 0;
        tape.cur_bit    = 7;
        tape.pulse_of_bit = 0;
        tape.cur_byte   = (tape.blk_len > 0) ? tape.blk[tape.data_pos++] : 0x00;
        bool b          = (tape.cur_byte & 0x80) != 0;
        tape.halfwave_ts= halfwave_for_bit(b);
        tape.next_edge_cycle = now + tape.halfwave_ts;
    } else {
        // No hay nada que emitir: solo pausa (si existe)
        tape.phase = PH_PAUSE;
        tape.next_edge_cycle = now + MS_TO_TSTATES(tape.pause_ms);
    }
}

static bool tzx_ear_level_until(uint64_t now_cycle) {
    if (!tape.playing || !tape.f || tape.phase == PH_IDLE) return true;

    while (now_cycle >= tape.next_edge_cycle) {
        // En PAUSE no hay flancos: nivel estable
        if (tape.phase != PH_PAUSE) {
            tape.level = !tape.level;
        }

        switch (tape.phase) {
            case PH_PILOT:
            case PH_PURE_TONE:
                if (--tape.pulses_left > 0) {
                    tape.next_edge_cycle += tape.halfwave_ts;
                } else {
                    if (tape.t_sync1) {
                        tape.phase = PH_SYNC1;
                        tape.halfwave_ts = tape.t_sync1;
                        tape.next_edge_cycle += tape.halfwave_ts;
                    } else {
                        if (tape.t_bit0 || tape.t_bit1) {
                            tape.phase = PH_DATA;
                            tape.data_pos = 0; tape.cur_bit = 7; tape.pulse_of_bit = 0;
                            tape.cur_byte = (tape.blk_len > 0) ? tape.blk[tape.data_pos++] : 0x00;
                            bool b = (tape.cur_byte & 0x80) != 0;
                            tape.halfwave_ts = halfwave_for_bit(b);
                            tape.next_edge_cycle += tape.halfwave_ts;
                        } else {
                            tape.phase = PH_PAUSE;
                            tape.next_edge_cycle += MS_TO_TSTATES(tape.pause_ms);
                        }
                    }
                }
                break;

            case PH_SYNC1:
                tape.phase = tape.t_sync2 ? PH_SYNC2 : PH_DATA;
                if (tape.phase == PH_SYNC2) {
                    tape.halfwave_ts = tape.t_sync2;
                    tape.next_edge_cycle += tape.halfwave_ts;
                } else {
                    tape.data_pos = 0; tape.cur_bit = 7; tape.pulse_of_bit = 0;
                    tape.cur_byte = (tape.blk_len > 0) ? tape.blk[tape.data_pos++] : 0x00;
                    bool b = (tape.cur_byte & 0x80) != 0;
                    tape.halfwave_ts = halfwave_for_bit(b);
                    tape.next_edge_cycle += tape.halfwave_ts;
                }
                break;

            case PH_SYNC2:
                tape.phase = PH_DATA;
                tape.data_pos = 0; tape.cur_bit = 7; tape.pulse_of_bit = 0;
                tape.cur_byte = (tape.blk_len > 0) ? tape.blk[tape.data_pos++] : 0x00;
                {
                    bool b = (tape.cur_byte & 0x80) != 0;
                    tape.halfwave_ts = halfwave_for_bit(b);
                    tape.next_edge_cycle += tape.halfwave_ts;
                }
                break;

            case PH_DATA: {
                tape.pulse_of_bit ^= 1;
                if (tape.pulse_of_bit == 1) {
                    tape.next_edge_cycle += tape.halfwave_ts;
                } else {
                    if (--tape.cur_bit < 0) {
                        if (tape.data_pos >= tape.blk_len) {
                            tape.phase = PH_PAUSE;
                            tape.next_edge_cycle += MS_TO_TSTATES(tape.pause_ms);
                            break;
                        }
                        tape.cur_bit = 7;
                        tape.cur_byte = tape.blk[tape.data_pos++];
                    }

                    if (tape.data_pos == tape.blk_len && tape.used_bits_last && tape.used_bits_last != 8) {
                        int emitted_bits = 7 - tape.cur_bit;
                        if (emitted_bits >= tape.used_bits_last) {
                            tape.phase = PH_PAUSE;
                            tape.next_edge_cycle += MS_TO_TSTATES(tape.pause_ms);
                            break;
                        }
                    }

                    bool b = ((tape.cur_byte >> tape.cur_bit) & 1) != 0;
                    tape.halfwave_ts = halfwave_for_bit(b);
                    tape.next_edge_cycle += tape.halfwave_ts;
                }
            } break;

            case PH_PULSE_SEQ:
                if (tape.pulse_seq_i < tape.pulse_seq_n) {
                    tape.halfwave_ts = tape.pulse_seq[tape.pulse_seq_i++];
                    tape.next_edge_cycle += tape.halfwave_ts;
                } else {
                    tape.phase = PH_PAUSE;
                    tape.next_edge_cycle += MS_TO_TSTATES(tape.pause_ms);
                }
                break;

            case PH_DIRECT_REC: {
                if (tape.dr_bit_index >= tape.dr_total_bits) {
                    tape.phase = PH_PAUSE;
                    tape.next_edge_cycle += MS_TO_TSTATES(tape.pause_ms);
                    break;
                }
                uint32_t byte_i = tape.dr_bit_index >> 3;
                int      bit_i  = 7 - (tape.dr_bit_index & 7);
                uint8_t  b      = tape.blk[byte_i];
                bool     lvl    = ((b >> bit_i) & 1) != 0;
                tape.next_edge_cycle += tape.dr_tstates_per_sample;
                if (lvl != tape.level) { /* mantener el toggle ya aplicado */ }
                else { tape.level = !tape.level; } // revertir toggle artificial
                tape.dr_bit_index++;
            } break;

            case PH_PAUSE:
                // Pausa consumida → siguiente bloque (sin conmutar nivel)
                if (!tzx_read_and_prepare_next_block(now_cycle)) {
                    tape.phase = PH_IDLE; tape.playing = false; tape.level = true;
                }
                break;

            case PH_IDLE:
            default:
                return tape.level;
        }
        if (tape.phase == PH_IDLE) break;
    }
    return tape.level;
}

static bool tzx_read_and_prepare_next_block(uint64_t now) {
    if (tape.file_pos >= tape.file_size) return false;
    int id = rd_u8(tape.f); tape.file_pos++;

    switch (id) {
        // ── Aliases legacy: 0x00→0x10, 0x02→0x12
        case 0x00: // Standard Speed Data (legacy)
            printf("[TZX] Bloque 0x00 (alias 0x10 Standard Speed)\n");
            // fall-through a 0x10
        case 0x10: { // Standard Speed Data
            tape.pause_ms = rd_u16(tape.f);  tape.file_pos += 2;
            uint16_t dlen = rd_u16(tape.f);  tape.file_pos += 2;
            free(tape.blk); tape.blk = (uint8_t*)malloc(dlen);
            if (!tape.blk) return false;
            fread(tape.blk, 1, dlen, tape.f); tape.file_pos += dlen;
            tape.blk_len = dlen;
            tape.t_pilot = 2168; tape.t_sync1 = 667; tape.t_sync2 = 735;
            tape.t_bit0  = 855;  tape.t_bit1  = 1710;
            tape.used_bits_last = 8;
            tape.pilot_pulses = (tape.blk_len>0 && tape.blk[0]==0x00) ? 8063 : 3223;

            printf("[TZX] 0x10 std: pause=%ums len=%u pilot=%u\n", tape.pause_ms, dlen, tape.pilot_pulses);
            tzx_prepare_standard_or_turbo(now);
        } return true;

        case 0x02: // Pure Tone (legacy)
            printf("[TZX] Bloque 0x02 (alias 0x12 Pure Tone)\n");
            // fall-through a 0x12
        case 0x12: { // Pure Tone
            uint16_t tone_len = rd_u16(tape.f); tape.file_pos += 2;
            uint16_t tone_pulses = rd_u16(tape.f); tape.file_pos += 2;
            tape.t_pilot = tape.t_sync1 = tape.t_sync2 = 0;
            tape.t_bit0 = tape.t_bit1 = 0;
            tape.pulse_seq_n = 0;
            tape.pause_ms = 0;
            tape.halfwave_ts = tone_len;
            tape.pulses_left = tone_pulses * 2;
            tape.phase = PH_PURE_TONE;
            tape.next_edge_cycle = now + tape.halfwave_ts;
            tape.level = tape.initial_level_known ? tape.initial_level : true;
            printf("[TZX] 0x12 tone: halfwave=%u pulses=%u\n", tone_len, tone_pulses);
        } return true;

        case 0x11: { // Turbo
            tape.t_pilot = rd_u16(tape.f);      tape.file_pos += 2;
            tape.t_sync1 = rd_u16(tape.f);      tape.file_pos += 2;
            tape.t_sync2 = rd_u16(tape.f);      tape.file_pos += 2;
            tape.t_bit0  = rd_u16(tape.f);      tape.file_pos += 2;
            tape.t_bit1  = rd_u16(tape.f);      tape.file_pos += 2;
            tape.pilot_pulses = rd_u16(tape.f); tape.file_pos += 2;            
            { uint8_t u = rd_u8(tape.f); tape.used_bits_last = (u == 0) ? 8 : u; } tape.file_pos += 1;
            tape.pause_ms = rd_u16(tape.f);     tape.file_pos += 2;
            uint32_t dlen = rd_u24(tape.f);     tape.file_pos += 3;
            free(tape.blk); tape.blk = (uint8_t*)malloc(dlen);
            if (!tape.blk) return false;
            fread(tape.blk, 1, dlen, tape.f); tape.file_pos += dlen;
            tape.blk_len = dlen;
            printf("[TZX] 0x11 turbo: len=%u pilot=%u bit0=%u bit1=%u usedLast=%u pause=%u\n",
                   dlen, tape.pilot_pulses, tape.t_bit0, tape.t_bit1, tape.used_bits_last, tape.pause_ms);
            tzx_prepare_standard_or_turbo(now);
        } return true;

        case 0x13: { // Pulse sequence
            int n = rd_u8(tape.f); tape.file_pos += 1;
            free(tape.pulse_seq); tape.pulse_seq = (uint16_t*)malloc(sizeof(uint16_t)*n);
            if (!tape.pulse_seq) return false;
            for (int i=0;i<n;i++){ tape.pulse_seq[i]=rd_u16(tape.f); } tape.file_pos += 2*n;
            tape.pulse_seq_n = n; tape.pulse_seq_i = 0;
            tape.pause_ms = 0;
            tape.phase = PH_PULSE_SEQ;
            tape.halfwave_ts = (n>0)? tape.pulse_seq[0] : 0;
            tape.next_edge_cycle = now + (tape.halfwave_ts? tape.halfwave_ts : 1);
            tape.level = tape.initial_level_known ? tape.initial_level : true;
            printf("[TZX] 0x13 pulse-seq: n=%d\n", n);
        } return true;

        case 0x14: { // Pure Data
            tape.t_bit0  = rd_u16(tape.f);    tape.file_pos += 2;
            tape.t_bit1  = rd_u16(tape.f);    tape.file_pos += 2;
            tape.used_bits_last = rd_u8(tape.f); tape.file_pos += 1; // en 0x14 la spec también usa 0→8
            tape.pause_ms = rd_u16(tape.f);   tape.file_pos += 2;
            uint32_t dlen = rd_u24(tape.f);   tape.file_pos += 3;
            free(tape.blk); tape.blk = (uint8_t*)malloc(dlen);
            if (!tape.blk) return false;
            fread(tape.blk, 1, dlen, tape.f); tape.file_pos += dlen;
            tape.blk_len = dlen;
            tape.t_pilot = tape.t_sync1 = tape.t_sync2 = 0;
            tape.phase = PH_DATA;
            tape.data_pos = 0; tape.cur_bit = 7; tape.pulse_of_bit = 0;
            tape.cur_byte = (tape.blk_len>0)? tape.blk[tape.data_pos++] : 0x00;
            {
                bool b = (tape.cur_byte & 0x80) != 0;
                tape.halfwave_ts = halfwave_for_bit(b);
            }
            tape.level = tape.initial_level_known ? tape.initial_level : true;
            tape.next_edge_cycle = now + tape.halfwave_ts;
            printf("[TZX] 0x14 pure-data: len=%u bit0=%u bit1=%u usedLast=%u pause=%u\n",
                   dlen, tape.t_bit0, tape.t_bit1, tape.used_bits_last, tape.pause_ms);
        } return true;

        case 0x15: { // Direct Recording
            tape.dr_tstates_per_sample = rd_u16(tape.f); tape.file_pos += 2;
            tape.pause_ms = rd_u16(tape.f);  tape.file_pos += 2;
            uint8_t used_last = rd_u8(tape.f); tape.file_pos += 1;
            uint32_t dlen = rd_u24(tape.f);  tape.file_pos += 3;
            free(tape.blk); tape.blk=(uint8_t*)malloc(dlen);
            if (!tape.blk) return false;
            fread(tape.blk, 1, dlen, tape.f); tape.file_pos += dlen;
            tape.blk_len = dlen;
            tape.dr_total_bits = (dlen-1)*8 + ((used_last==0)? 8 : used_last);
            tape.dr_bit_index = 0;
            tape.phase = PH_DIRECT_REC;
            tape.level = tape.initial_level_known ? tape.initial_level : true;
            tape.next_edge_cycle = now + tape.dr_tstates_per_sample;
            printf("[TZX] 0x15 direct-rec: bitTs=%u pause=%u len=%u usedLast=%u\n",
                   tape.dr_tstates_per_sample, tape.pause_ms, dlen, used_last);
        } return true;

        case 0x18: { // CSW Recording (soporte base: compresión=0 raw)
            uint16_t pause_ms = rd_u16(tape.f); tape.file_pos += 2;
            uint32_t freq_hz  = rd_u32(tape.f); tape.file_pos += 4;
            uint8_t  comp     = rd_u8(tape.f);  tape.file_pos += 1;  // 0=raw sin compresión (asumido)
            uint32_t data_len = rd_u32(tape.f); tape.file_pos += 4;

            free(tape.blk); tape.blk = (uint8_t*)malloc(data_len);
            if (!tape.blk) return false;
            fread(tape.blk,1,data_len,tape.f); tape.file_pos += data_len;

            tape.pause_ms = pause_ms;
            tape.csw_freq_hz = freq_hz;
            tape.csw_compression = comp;
            tape.csw_data_len = data_len;

            // Convertimos CSW raw a secuencia de medias ondas si compresión=0:
            if (comp == 0 && data_len >= 2) {
                uint32_t pairs = data_len / 2;
                free(tape.pulse_seq); tape.pulse_seq = (uint16_t*)malloc(sizeof(uint16_t)*pairs*4);
                if (!tape.pulse_seq) return false;

                int n=0;
                for (uint32_t i=0;i<pairs;i++) {
                    uint16_t samples = (uint16_t)(tape.blk[2*i] | (tape.blk[2*i+1]<<8));
                    if (samples==0) continue; // ignora silencios nulos
                    uint32_t ts = (uint32_t)(((uint64_t)samples * 3500000ULL) / (freq_hz ? freq_hz : 1));
                    if (ts==0) ts=1;
                    while (ts > 0) {
                        uint16_t chunk = (ts > 65535) ? 65535 : (uint16_t)ts;
                        tape.pulse_seq[n++] = chunk;
                        ts -= chunk;
                    }
                }
                tape.pulse_seq_n = n;
                tape.pulse_seq_i = 0;
                tape.phase = PH_PULSE_SEQ;
                tape.halfwave_ts = (n>0)? tape.pulse_seq[0] : 1;
                tape.level = tape.initial_level_known ? tape.initial_level : true;
                tape.next_edge_cycle = now + tape.halfwave_ts;

                printf("[TZX] 0x18 CSW(raw): pause=%ums freq=%uHz pulses=%d (from %u bytes)\n",
                       pause_ms, freq_hz, n, data_len);
            } else {
                printf("[TZX] 0x18 CSW comp=%u NO soportado; se salta (pause=%ums, data=%u)\n",
                       comp, pause_ms, data_len);
                tape.phase = PH_PAUSE;
                tape.next_edge_cycle = now + MS_TO_TSTATES(pause_ms);
                tape.level = true;
            }
        } return true;

        case 0x19: { // Generalized Data Block (implementación completa)
            uint32_t blen = rd_u32(tape.f); tape.file_pos += 4;
            long block_end = tape.file_pos + blen;

            tape.pause_ms = rd_u16(tape.f);        tape.file_pos += 2;
            uint32_t TOTP  = rd_u32(tape.f);        tape.file_pos += 4; // pilot/sync total symbols
            uint8_t  NPP   = rd_u8(tape.f);         tape.file_pos += 1; // max pulses per pilot/sync symbol
            uint8_t  ASPx  = rd_u8(tape.f);         tape.file_pos += 1; // alphabet size (0=256)
            uint32_t TOTD  = rd_u32(tape.f);        tape.file_pos += 4; // data total symbols
            uint8_t  NPD   = rd_u8(tape.f);         tape.file_pos += 1; // max pulses per data symbol
            uint8_t  ASDx  = rd_u8(tape.f);         tape.file_pos += 1; // alphabet size (0=256)

            int ASP = (ASPx == 0) ? 256 : ASPx;
            int ASD = (ASDx == 0) ? 256 : ASDx;

            // Salida: secuencia de medias ondas
            uint16_t *seq = NULL; int seq_cap = 0, seq_n = 0;

            // Nivel inicial
            bool init_level = tape.initial_level_known ? tape.initial_level : true;

            // Tablas de símbolos para Pilot/Sync
            tzx19_symdef_t *pilot = NULL;
            if (TOTP > 0) {
                pilot = (tzx19_symdef_t*)calloc((size_t)ASP, sizeof(tzx19_symdef_t));
                if (!pilot) goto tzx19_fail;

                for (int i = 0; i < ASP; ++i) {
                    pilot[i].flags = rd_u8(tape.f); tape.file_pos += 1;
                    pilot[i].pulses = (uint16_t*)malloc(sizeof(uint16_t) * NPP);
                    if (!pilot[i].pulses) goto tzx19_fail;
                    pilot[i].npulses = 0;
                    for (int j = 0; j < NPP; ++j) {
                        uint16_t d = rd_u16(tape.f); tape.file_pos += 2;
                        if (d) pilot[i].pulses[pilot[i].npulses++] = d;
                    }
                }

                // PRLE (TOTP entradas): símbolo + repeticiones
                for (uint32_t k = 0; k < TOTP; ++k) {
                    uint8_t sym = rd_u8(tape.f);     tape.file_pos += 1;
                    uint16_t rep = rd_u16(tape.f);   tape.file_pos += 2;
                    if (sym >= ASP) sym %= ASP;

                    for (uint16_t r = 0; r < rep; ++r) {
                        tzx19_symdef_t *S = &pilot[sym];

                        uint8_t pol = (S->flags & 0x03);

                        // nivel actual según sec_n
                        bool current_level = (seq_n % 2 == 0) ? init_level : !init_level;

                        bool merge_first = false;
                        if      (pol == 0x01) merge_first = (seq_n > 0);                     // same level → sin flanco
                        else if (pol == 0x02) merge_first = (seq_n > 0) && (current_level == false); // force low
                        else if (pol == 0x03) merge_first = (seq_n > 0) && (current_level == true);  // force high

                        for (int p = 0; p < S->npulses; ++p) {
                            if (!push_or_merge_halfwave(&seq, &seq_cap, &seq_n, S->pulses[p], (p==0) && merge_first))
                                goto tzx19_fail;
                        }
                    }
                }
            }

            // Tabla de símbolos de datos
            tzx19_symdef_t *data = NULL;
            uint32_t bytes_consumed = 0;
            if (TOTD > 0) {
                data = (tzx19_symdef_t*)calloc((size_t)ASD, sizeof(tzx19_symdef_t));
                if (!data) goto tzx19_fail;

                for (int i = 0; i < ASD; ++i) {
                    data[i].flags = rd_u8(tape.f); tape.file_pos += 1;
                    data[i].pulses = (uint16_t*)malloc(sizeof(uint16_t) * NPD);
                    if (!data[i].pulses) goto tzx19_fail;
                    data[i].npulses = 0;
                    for (int j = 0; j < NPD; ++j) {
                        uint16_t d = rd_u16(tape.f); tape.file_pos += 2;
                        if (d) data[i].pulses[data[i].npulses++] = d;
                    }
                }

                int NB = ceil_log2_u16(ASD);
                uint32_t DS = (uint32_t)((NB * (uint64_t)TOTD + 7) / 8);

                uint8_t cur = 0; int rem_bits = 0;

                for (uint32_t k = 0; k < TOTD; ++k) {
                    uint32_t sym = 0;
                    for (int i = 0; i < NB; ++i) {
                        if (rem_bits == 0) {
                            cur = rd_u8(tape.f); tape.file_pos += 1;
                            rem_bits = 8;
                            bytes_consumed++;
                        }
                        sym = (sym << 1) | ((cur >> (rem_bits - 1)) & 1u);
                        rem_bits--;
                    }

                    if (sym >= (uint32_t)ASD) sym %= ASD; // defensa
                    tzx19_symdef_t *S = &data[sym];

                    uint8_t pol = (S->flags & 0x03);
                    bool current_level = (seq_n % 2 == 0) ? init_level : !init_level;

                    bool merge_first = false;
                    if      (pol == 0x01) merge_first = (seq_n > 0);
                    else if (pol == 0x02) merge_first = (seq_n > 0) && (current_level == false);
                    else if (pol == 0x03) merge_first = (seq_n > 0) && (current_level == true);

                    for (int p = 0; p < S->npulses; ++p) {
                        if (!push_or_merge_halfwave(&seq, &seq_cap, &seq_n, S->pulses[p], (p==0) && merge_first))
                            goto tzx19_fail;
                    }
                }

                // Saltar bytes de padding (si los hubiera) hasta consumir DS
                if (bytes_consumed < DS) {
                    uint32_t skip = DS - bytes_consumed;
                    fseek(tape.f, skip, SEEK_CUR);
                    tape.file_pos += skip;
                }
            } else {
                // Si no hay TOTD, saltar a block_end si queda
                if (tape.file_pos < block_end) {
                    fseek(tape.f, block_end - tape.file_pos, SEEK_CUR);
                    tape.file_pos = block_end;
                }
            }

            // Liberar tablas auxiliares
            if (pilot) {
                for (int i = 0; i < ((TOTP>0)? ((ASPx==0)?256:ASPx) : 0); ++i) free(pilot[i].pulses);
                free(pilot);
            }
            if (data) {
                for (int i = 0; i < ((TOTD>0)? ((ASDx==0)?256:ASDx) : 0); ++i) free(data[i].pulses);
                free(data);
            }

            // Activar emisión como secuencia de medias ondas
            free(tape.pulse_seq);
            tape.pulse_seq   = seq;
            tape.pulse_seq_n = seq_n;
            tape.pulse_seq_i = 0;

            tape.phase = PH_PULSE_SEQ;
            tape.halfwave_ts = (seq_n > 0) ? tape.pulse_seq[0] : 1;
            tape.level = init_level;
            tape.next_edge_cycle = now + tape.halfwave_ts;

            printf("[TZX] 0x19 GDB: pulses=%d pause=%ums (ASP=%d,NPP=%d; ASD=%d,NPD=%d; TOTP=%u; TOTD=%u)\n",
                   seq_n, tape.pause_ms, (ASPx==0)?256:ASPx, NPP, (ASDx==0)?256:ASDx, NPD, TOTP, TOTD);

            return true;

tzx19_fail:
            fprintf(stderr, "[TZX] 0x19: error de memoria o lectura; se intenta continuar.\n");
            if (pilot) { for (int i = 0; i < ((ASPx==0)?256:ASPx); ++i) free(pilot[i].pulses); free(pilot); }
            if (data)  { for (int i = 0; i < ((ASDx==0)?256:ASDx); ++i) free(data[i].pulses);  free(data);  }
            free(seq);
            // Saltar al final del bloque e intentar seguir
            fseek(tape.f, block_end, SEEK_SET); tape.file_pos = block_end;
            return tzx_read_and_prepare_next_block(now);
        }

        case 0x20: { // Pause
            uint16_t ms = rd_u16(tape.f); tape.file_pos += 2;
            if (ms == 0) { tape.phase = PH_IDLE; tape.playing = false; tape.level = true; printf("[TZX] 0x20 pause=0 (stop)\n"); return false; }
            tape.pause_ms = ms;
            tape.phase = PH_PAUSE; tape.next_edge_cycle = now + MS_TO_TSTATES(ms); tape.level = true;
            printf("[TZX] 0x20 pause=%u\n", ms);
        } return true;

        case 0x21: { // Group start (informativo)
            uint8_t ln = rd_u8(tape.f); tape.file_pos += 1;
            char name[ln];
            int rd = ln;//(ln < 255) ? ln : 255;
            if (rd > 0) fread(name, 1, rd, tape.f);
            tape.file_pos += rd;
            name[ (rd>0)? rd : 0 ] = 0;
            //if (ln > rd) { fseek(tape.f, ln - rd, SEEK_CUR); tape.file_pos += (ln - rd); }

            if (tape.group_depth == 0) tape.group_depth = 1;
            else fprintf(stderr, "[TZX] 0x21: grupo anidado no permitido por la spec.\n");
            printf("[TZX] 0x21 group-start: \"%s\"\n", name);
            return tzx_read_and_prepare_next_block(now);
        }

        case 0x22: { // Group end (informativo)
            if (tape.group_depth > 0) tape.group_depth = 0;
            printf("[TZX] 0x22 group-end\n");
            return tzx_read_and_prepare_next_block(now);
        }

        case 0x24: { uint16_t count = rd_u16(tape.f); tape.file_pos += 2; tape.loop.file_pos_at_loop = ftell(tape.f); tape.loop.remaining = count; tape.loop.active = 1; printf("[TZX] 0x24 loop-start x%u\n", count); return tzx_read_and_prepare_next_block(now); }
        case 0x25: { printf("[TZX] 0x25 loop-end (remain=%u)\n", tape.loop.remaining); if (tape.loop.active && tape.loop.remaining > 1) { tape.loop.remaining--; fseek(tape.f, tape.loop.file_pos_at_loop, SEEK_SET); tape.file_pos = tape.loop.file_pos_at_loop; return tzx_read_and_prepare_next_block(now); } else { tape.loop.active = 0; return tzx_read_and_prepare_next_block(now);} }

        case 0x2A: { tape.phase = PH_IDLE; tape.playing = false; tape.level = true; printf("[TZX] 0x2A stop-if-48K → STOP\n"); return false; }
        case 0x2B: { uint8_t lvl = rd_u8(tape.f); tape.file_pos += 1; tape.initial_level_known = true; tape.initial_level = (lvl != 0); printf("[TZX] 0x2B set-level=%u\n", lvl); return tzx_read_and_prepare_next_block(now); }

        case 0x30: { uint8_t ln = rd_u8(tape.f); tape.file_pos += 1; fseek(tape.f, ln, SEEK_CUR); tape.file_pos += ln; printf("[TZX] 0x30 text\n"); return tzx_read_and_prepare_next_block(now); }
        case 0x31: { uint8_t dur = rd_u8(tape.f); uint8_t ln = rd_u8(tape.f); tape.file_pos += 2; fseek(tape.f, ln, SEEK_CUR); tape.file_pos += ln; printf("[TZX] 0x31 message %us\n", dur); return tzx_read_and_prepare_next_block(now); }

        case 0x32: { // Archive Info (metadatos; se muestra y se continúa)
            uint16_t blen = rd_u16(tape.f); tape.file_pos += 2;
            printf("Longitud bloque completo: %d\n", blen);
            long end = tape.file_pos + blen;
            printf("Posicion final: %ld\n", end);
            if (end > tape.file_size) end = tape.file_size;

            printf("[TZX] 0x32 archive-info:\n");
#if 1
            if (tape.file_pos >= end) { printf("       (vacío)\n"); return tzx_read_and_prepare_next_block(now); }

            uint8_t n = rd_u8(tape.f); tape.file_pos += 1;
            printf("       %u campo%s\n", n, (n==1?"":"s"));

            for (uint8_t i = 0; (i < n) && (tape.file_pos < end); ++i) {
                if (tape.file_pos + 1 > end) break;
                uint8_t tid = rd_u8(tape.f); tape.file_pos += 1;

                if (tape.file_pos + 1 > end) break;
                uint8_t slen = rd_u8(tape.f); tape.file_pos += 1;

                long remain = end - tape.file_pos; if (remain < 0) remain = 0;
                uint16_t toread = (slen > (uint16_t)remain) ? (uint16_t)remain : slen;

                char* buf = (toread > 0) ? (char*)malloc((size_t)toread) : NULL;
                if (buf && toread > 0) { size_t rd = fread(buf, 1, toread, tape.f); (void)rd; }
                if (toread < slen) fseek(tape.f, slen - toread, SEEK_CUR);

                tape.file_pos += slen;

                const char* fname = tzx_archive_field_name(tid);
                if (buf && toread > 0)
                    printf("       - %s [0x%02X]: %.*s\n", fname, tid, (int)toread, buf);
                else
                    printf("       - %s [0x%02X]: <vacío>\n", fname, tid);

                free(buf);
            }

            if (tape.file_pos < end) { fseek(tape.f, end - tape.file_pos, SEEK_CUR); tape.file_pos = end; }
#endif
            // (NO sumar tape.file_pos += blen; ya hemos posicionado a end)
            return tzx_read_and_prepare_next_block(now);
        }

        case 0x33: { uint8_t n = rd_u8(tape.f); tape.file_pos += 1; fseek(tape.f, n*3, SEEK_CUR); tape.file_pos += n*3; printf("[TZX] 0x33 hardware x%u\n", n); return tzx_read_and_prepare_next_block(now); }
        case 0x35: { fseek(tape.f, 16, SEEK_CUR); tape.file_pos += 16; { uint32_t ln = rd_u32(tape.f); tape.file_pos += 4; fseek(tape.f, ln, SEEK_CUR); tape.file_pos += ln; } printf("[TZX] 0x35 custom\n"); return tzx_read_and_prepare_next_block(now); }
        case 0x5A: { uint32_t ln = rd_u32(tape.f); tape.file_pos += 4; fseek(tape.f, ln, SEEK_CUR); tape.file_pos += ln; printf("[TZX] 0x5A glue\n"); return tzx_read_and_prepare_next_block(now); }

        default:
            fprintf(stderr, "[TZX] Bloque 0x%02X no soportado.\n", id);
            return false;
    }
}

bool load_tzx(const char* filename) {
    list_tzx_blocks(filename); // listado completo al cargar

    if (tape.f) { fclose(tape.f); tape.f = NULL; }
    tape.f = fopen(filename, "rb");
    if (!tape.f) { fprintf(stderr, "No se pudo abrir %s\n", filename); return false; }

    fseek(tape.f, 0, SEEK_END); tape.file_size = ftell(tape.f);
    fseek(tape.f, 0, SEEK_SET); tape.file_pos = 0;

    char hdr[10]={0};
    if (fread(hdr,1,10,tape.f) < 10 || memcmp(hdr,"ZXTape!\x1A",8) != 0) {
        fprintf(stderr, "TZX: cabecera inválida.\n");
        fclose(tape.f); tape.f = NULL; return false;
    }
    tape.file_pos += 10;

    free(tape.blk); tape.blk=NULL;
    free(tape.pulse_seq); tape.pulse_seq=NULL;
    tape.blk_len = 0;
    tape.fmt = TAPE_FMT_TZX;
    tape.playing = false;
    tape.initial_level_known = false;
    tape.loop.active = 0;
    tape.group_depth = 0;

    if (!tzx_read_and_prepare_next_block(global_cycles)) { tape.playing=false; return false; }
    border_color = 7;

    printf("TZX cargado: %s (%ld bytes) v%d.%02d\n", filename, tape.file_size, (unsigned char)hdr[8], (unsigned char)hdr[9]);
    tape_filename = filename;
    return true;
}

// ─────────────────────────────────────────────────────────────
// Selector unificado EAR
// ─────────────────────────────────────────────────────────────
static inline bool get_current_ear_level_from_tape(void) {
    if (tape.fmt == TAPE_FMT_TZX) return tzx_ear_level_until(global_cycles);
    if (tape.fmt == TAPE_FMT_TAP) return tap_ear_level_until(global_cycles);
    return true; // sin cinta → EAR alto
}

void generate_audio(uint32_t current_tstates) {
    uint32_t delta_t = current_tstates - last_audio_tstates;
    
    // Convertimos t-states a número de muestras
    // Muestras = (T-states * SampleRate) / CPU_Freq
    int samples_to_render = (delta_t * SAMPLE_RATE) / CPU_HZ;

    for (int i = 0; i < samples_to_render; i++) {
        if (audio_ptr < BUFFER_SIZE) 
        {
            // Amplificamos el bit (0 o 1) a un valor de 16 bits
            audio_buffer[audio_ptr++] = current_speaker_level ? 8000 : -8000;
        }

        // Si el buffer se llena, lo enviamos a SDL
        if (audio_ptr >= BUFFER_SIZE) 
        {
            SDL_QueueAudio(audio_dev, audio_buffer, BUFFER_SIZE * sizeof(int16_t));
            audio_ptr = 0;
        }
    }

    // Actualizamos el contador de tiempo procesado
    if (samples_to_render > 0) {
        last_audio_tstates = current_tstates;
    }
}

// ─────────────────────────────────────────────────────────────
// Contended RAM (placeholder; desactivado)
// ─────────────────────────────────────────────────────────────
int contended_delay(uint16_t addr, int tstates_in_line) { (void)addr; (void)tstates_in_line; return 0; }

// ─────────────────────────────────────────────────────────────
// Memoria y puertos
// ─────────────────────────────────────────────────────────────
uint8_t read_byte(void* ud, uint16_t addr) {
#if 0
    int line_t = cycles_done % 224;
    int delay = contended_delay(addr, line_t);
    if (delay > 0) {
        z80_add_cycles(&cpu, delay);
        cycles_done   += delay;
        global_cycles += (uint64_t)delay;
    }
#endif
    (void)ud;
    
    // If TR-DOS ROM is active and address is in ROM area, read from TR-DOS ROM
    if (trdos_rom_active && addr < ROM_SIZE && trdos_rom_loaded) {
        return trdos_rom[addr];
    }
    
    return memory[addr];
}

void write_byte(void* ud, uint16_t addr, uint8_t val) {
#if 0
    int line_t = cycles_done % 224;
    int delay = contended_delay(addr, line_t);
    if (delay > 0) {
        z80_add_cycles(&cpu, delay);
        cycles_done   += delay;
        global_cycles += (uint64_t)delay;
    }
#endif
    (void)ud;
    if (addr >= RAM_START) memory[addr] = val;
}

uint8_t port_in(z80* z, uint16_t port) {
    (void)z;
    uint8_t res = 0xFF;

	/*if ((port&0xff) != 254)
	{
		printf("puerto %d\n", (port&0xff));
	}*/

	// Check for FDC ports first (if TR-DOS enabled)
    if (trdos_enabled) {
        uint8_t port_low = port & 0xFF;
        if (port_low == 0x1F || port_low == 0x3F || port_low == 0x5F || 
            port_low == 0x7F || port_low == 0xFF) {
            return fdc_port_in(&fdc, port);
        }
    }

    if ((port & 1) == 0) { // FE
		res = 0xbf;
        uint8_t hi = port >> 8;

        // Teclado
        for (int r = 0; r < 8; r++)
            if ((hi & (1 << r)) == 0)
                res &= keyboard[r];

        // Bit 5 = 1 en 48K (sin MIC)
        //
		//res |= 0x20;

        // Bit 7 = espejo del bit 3 del último OUT a FE
        //if (last_fe_write & 0x08) res &= ~0x80; else res |= 0x80;

		if (tape.playing) {
            //Tape::Read();
            //bitWrite(data,6,Tape::tapeEarBit);
			bool ear = get_current_ear_level_from_tape();
			if (ear) res |= 0x40; else res &= ~0x40;
			current_speaker_level = (ear) ? 1 : 0;
        } else { // now Abu Simbel runs!
    		//if ((Z80Ops::is48) && (Config::Issue2)) // Issue 2 behaviour only on Spectrum 48K
				if (last_fe_write & 0x18) res |= 0x40;
			//else
			//	if (port254 & 0x10) data |= 0x40;
		}
#if 0
		if (tape.playing)
		{
			// Bit 6 = EAR desde cinta
			bool ear = get_current_ear_level_from_tape();
			if (ear) res |= 0x40; else res &= 0xbf;
			current_speaker_level = (ear) ? 1 : 0;

		} else {
			if (last_fe_write & 0x18) 
				res = res | 0x40; 
			else 
				res &= 0xbf;
		}

  #endif
        
    } else if ((port&0xff) == 0x1f ) // Joystick Kempston
	{
		return 0xff;
	}

    return res;
}



void port_out(z80* z, uint16_t port, uint8_t val) {
    (void)z;
	//printf("out %d=%d", port, val);
	
	// Check for FDC ports first (if TR-DOS enabled)
    if (trdos_enabled) {
        uint8_t port_low = port & 0xFF;
        if (port_low == 0x1F || port_low == 0x3F || port_low == 0x5F || 
            port_low == 0x7F || port_low == 0xFF) {
            fdc_port_out(&fdc, port, val);
            return;
        }
    }
	
    if ((port & 1) == 0) {
        border_color   = val & 0x07;
        last_fe_write  = val;
        
        // Antes de cambiar el valor, generamos el audio con el nivel anterior
        // hasta el momento exacto del cambio (t_states actuales)
        generate_audio(cycles_done); 
        
        // El bit 4 controla el altavoz
        current_speaker_level = (val & 0x10) ? 1 : 0;
    }
}

// ─────────────────────────────────────────────────────────────
// ROM
// ─────────────────────────────────────────────────────────────
bool load_rom(const char* fn) {
    FILE* f = fopen(fn, "rb");
    if (!f) return false;
    size_t rd = fread(memory, 1, ROM_SIZE, f);
    fclose(f);
    return rd == ROM_SIZE;
}

bool load_trdos_rom(const char* fn) {
    FILE* f = fopen(fn, "rb");
    if (!f) return false;
    size_t rd = fread(trdos_rom, 1, ROM_SIZE, f);
    fclose(f);
    if (rd == ROM_SIZE) {
        trdos_rom_loaded = true;
        printf("TR-DOS ROM loaded: %s\n", fn);
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────
// Vídeo
// ─────────────────────────────────────────────────────────────
void displayscanline(int y, int flash_phase) {
    int col = y * FULL_WIDTH;

    // Borde izquierdo
    for (int x = 0; x < h_border; x++)
        pixels[col++] = zx_colors[border_color];

    if (y >= v_border_top && y < v_border_top + SCREEN_HEIGHT) {
        int vy = y - v_border_top;

        int addr_pix = 0x4000
            + ((vy & 0xC0) << 5)
            + ((vy & 0x07) << 8)
            + ((vy & 0x38) << 2);

        int addr_att = 0x5800 + (32 * (vy >> 3));

        for (int bx = 0; bx < 32; bx++) {
            uint8_t pix = memory[addr_pix++];
            uint8_t att = memory[addr_att++];

            int bright = (att & 0x40) ? 8 : 0;
            int ink    = (att & 0x07) + bright;
            int paper  = ((att >> 3) & 0x07) + bright;

            if (att & 0x80) {
                if (flash_phase) { int t = ink; ink = paper; paper = t; }
            }

            pixels[col++] = zx_colors[(pix & 0x80) ? ink : paper];
            pixels[col++] = zx_colors[(pix & 0x40) ? ink : paper];
            pixels[col++] = zx_colors[(pix & 0x20) ? ink : paper];
            pixels[col++] = zx_colors[(pix & 0x10) ? ink : paper];
            pixels[col++] = zx_colors[(pix & 0x08) ? ink : paper];
            pixels[col++] = zx_colors[(pix & 0x04) ? ink : paper];
            pixels[col++] = zx_colors[(pix & 0x02) ? ink : paper];
            pixels[col++] = zx_colors[(pix & 0x01) ? ink : paper];
        }
    } else {
        // Borde superior/inferior
        for (int x = 0; x < SCREEN_WIDTH; x++)
            pixels[col++] = zx_colors[border_color];
    }

    // Borde derecho
    for (int x = 0; x < h_border; x++)
        pixels[col++] = zx_colors[border_color];
}

void update_texture() {
    SDL_UpdateTexture(texture, NULL, pixels, FULL_WIDTH * sizeof(uint32_t));
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}


// ─────────────────────────────────────────────────────────────
// Teclado (eventos)
// ─────────────────────────────────────────────────────────────
void handle_input() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE))
            exit(0);

        if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F12)
            z80_reset(&cpu);

        if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F6) {
            if (tape_filename) {
                const char* ext = strrchr(tape_filename, '.');
                if (ext && strcasecmp(ext, ".tap") == 0) load_tap(tape_filename);
                else if (ext && strcasecmp(ext, ".tzx") == 0) load_tzx(tape_filename);
            }
        }

        if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F7) {
            if (tape_filename) {
                tape.playing=!tape.playing;
            }
        }
        
        // F8: List disk images and files
        if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F8) {
            if (trdos_enabled) {
                printf("\n=== Disk Status ===\n");
                for (int i = 0; i < 4; i++) {
                    printf("Drive %d: ", i);
                    if (disk_images[i]) {
                        printf("%s\n", disk_images[i]->filename);
                        trd_list_files(disk_images[i]);
                    } else if (scl_images[i]) {
                        printf("%s (SCL)\n", scl_images[i]->filename);
                        if (scl_images[i]->trd) {
                            trd_list_files(scl_images[i]->trd);
                        }
                    } else {
                        printf("(empty)\n");
                    }
                }
            } else {
                printf("TR-DOS not enabled\n");
            }
        }
        
        // F9: Toggle TR-DOS ROM
        if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F9) {
            if (trdos_rom_loaded) {
                trdos_rom_active = !trdos_rom_active;
                printf("TR-DOS ROM: %s\n", trdos_rom_active ? "ACTIVE" : "INACTIVE");
            } else {
                printf("TR-DOS ROM not loaded\n");
            }
        }

        if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F12)
            z80_reset(&cpu);

        if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
            bool press = (e.type == SDL_KEYDOWN);
            int row = -1, bit = -1;

            switch (e.key.keysym.sym) {
                case SDLK_a:     row=1; bit=0; break;
                case SDLK_b:     row=7; bit=4; break;
                case SDLK_c:     row=0; bit=3; break;
                case SDLK_d:     row=1; bit=2; break;
                case SDLK_e:     row=2; bit=2; break;
                case SDLK_f:     row=1; bit=3; break;
                case SDLK_g:     row=1; bit=4; break;
                case SDLK_h:     row=6; bit=4; break;
                case SDLK_i:     row=5; bit=2; break;
                case SDLK_j:     row=6; bit=3; break;
                case SDLK_k:     row=6; bit=2; break;
                case SDLK_l:     row=6; bit=1; break;
                case SDLK_m:     row=7; bit=2; break;
                case SDLK_n:     row=7; bit=3; break;
                case SDLK_o:     row=5; bit=1; break;
                case SDLK_p:     row=5; bit=0; break;
                case SDLK_q:     row=2; bit=0; break;
                case SDLK_r:     row=2; bit=3; break;
                case SDLK_s:     row=1; bit=1; break;
                case SDLK_t:     row=2; bit=4; break;
                case SDLK_u:     row=5; bit=3; break;
                case SDLK_v:     row=0; bit=4; break;
                case SDLK_w:     row=2; bit=1; break;
                case SDLK_x:     row=0; bit=2; break;
                case SDLK_y:     row=5; bit=4; break;
                case SDLK_z:     row=0; bit=1; break;
                case SDLK_0:     row=4; bit=0; break;
                case SDLK_1:     row=3; bit=0; break;
                case SDLK_2:     row=3; bit=1; break;
                case SDLK_3:     row=3; bit=2; break;
                case SDLK_4:     row=3; bit=3; break;
                case SDLK_5:     row=3; bit=4; break;
                case SDLK_6:     row=4; bit=4; break;
                case SDLK_7:     row=4; bit=3; break;
                case SDLK_8:     row=4; bit=2; break;
                case SDLK_9:     row=4; bit=1; break;
                case SDLK_SPACE: row=7; bit=0; break;
                case SDLK_RETURN:row=6; bit=0; break;
                case SDLK_LSHIFT:
                case SDLK_RSHIFT: row=0; bit=0; break; // Caps Shift
                case SDLK_LCTRL:
                case SDLK_RCTRL:  row=7; bit=1; break; // Symbol Shift
            }

            if (row >= 0 && bit >= 0) {
                if (press) keyboard[row] &= ~(1 << bit);
                else       keyboard[row] |=  (1 << bit);
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────
// Carga de snapshot .sna (48K)
// ─────────────────────────────────────────────────────────────
bool load_sna(const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) { fprintf(stderr, "No se pudo abrir .sna: %s\n", filename); return false; }

    uint8_t header[27];
    if (fread(header, 1, 27, f) != 27) { fclose(f); fprintf(stderr, "Archivo .sna incompleto (header)\n"); return false; }

    // Restaurar registros Z80 (formato .sna 48K)
    cpu.i      = header[0];
    cpu.h_l_   = (header[2] << 8) | header[1];
    cpu.d_e_   = (header[4] << 8) | header[3];
    cpu.b_c_   = (header[6] << 8) | header[5];
    cpu.a_f_   = (header[8] << 8) | header[7];
    cpu.hl     = (header[10] << 8) | header[9];
    cpu.de     = (header[12] << 8) | header[11];
    cpu.bc     = (header[14] << 8) | header[13];
    cpu.iy     = (header[16] << 8) | header[15];
    cpu.ix     = (header[18] << 8) | header[17];
    cpu.iff2   = header[19] ? 1 : 0;
    cpu.r      = header[20];
    cpu.af     = (header[22] << 8) | header[21];
    cpu.sp     = (header[24] << 8) | header[23];
    cpu.interrupt_mode = header[25];
    border_color = header[26] & 0x07;

    if (fread(&memory[RAM_START], 1, 49152, f) != 49152) { fclose(f); fprintf(stderr, "Archivo .sna incompleto (RAM)\n"); return false; }
    fclose(f);

    uint16_t sp = cpu.sp;
    cpu.pc = (memory[sp+1] << 8) | memory[sp];
    cpu.sp += 2;

    cpu.iff1 = cpu.iff2;

    printf("Snapshot .sna cargado: %s\n", filename);
    printf("PC=0x%04X  SP=0x%04X  Border=%d  IM=%d\n", cpu.pc, cpu.sp, border_color, cpu.interrupt_mode);

    return true;
}

// ─────────────────────────────────────────────────────────────
// Carga TAP
// ─────────────────────────────────────────────────────────────
bool load_tap(const char* filename) {
    list_tap_blocks(filename); // listado completo al cargar

    if (tape.f) { fclose(tape.f); tape.f = NULL; }
    tape.f = fopen(filename, "rb");
    if (!tape.f) { printf("No se pudo abrir %s\n", filename); tape.playing = false; return false; }

    fseek(tape.f, 0, SEEK_END); tape.file_size = ftell(tape.f);
    fseek(tape.f, 0, SEEK_SET); tape.file_pos = 0;

    free(tape.blk); tape.blk = NULL;
    free(tape.pulse_seq); tape.pulse_seq = NULL;
    tape.blk_len = 0;
    tape.fmt = TAPE_FMT_TAP;
    tape.speed   = 1.0;
    tape.playing = true;
    tape.initial_level_known = false;

    if (!tap_read_next_block()) { printf("TAP vacío.\n"); tape.playing = false; return false; }
    start_block_emission(global_cycles);
    border_color = 7;

    printf("TAP cargado: %s (%ld bytes)\n", filename, tape.file_size);
    tape_filename = filename;
    return true;
}

// ─────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    bool read_only_disks = false;
    int drive_count = 2; // Default 2 drives
    int next_drive = 0;
    const char* trdos_rom_file = NULL;
    
    // Parse command-line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ro") == 0) {
            read_only_disks = true;
        } else if (strcmp(argv[i], "--drive-count") == 0 && i + 1 < argc) {
            drive_count = atoi(argv[++i]);
            if (drive_count < 1 || drive_count > 4) drive_count = 2;
        } else if (strcmp(argv[i], "--trdos-rom") == 0 && i + 1 < argc) {
            trdos_rom_file = argv[++i];
        }
    }
    
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_memset(&want, 0, sizeof(want));
    want.freq = SAMPLE_RATE;
    want.format = AUDIO_S16SYS; // 16 bits con signo (endian nativo)
    want.channels = 1;          // Mono
    want.samples = 1024;        // Tamaño del buffer interno de SDL
    want.callback = NULL;       // Usaremos SDL_QueueAudio en su lugar

    audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);

    if (audio_dev == 0) {
        printf("No se pudo abrir el audio: %s\n", SDL_GetError());
    } else {
        SDL_PauseAudioDevice(audio_dev, 0); // Empieza a reproducir (silencio inicial)
    }

    window = SDL_CreateWindow(trdos_enabled ? "Minimal ZX 48K + TR-DOS" : "Minimal ZX 48K",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              FULL_WIDTH * SCALE, FULL_HEIGHT * SCALE, 0);

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(renderer, FULL_WIDTH, FULL_HEIGHT);

    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STATIC, FULL_WIDTH, FULL_HEIGHT);

    if (!load_rom("zx48.rom")) { fprintf(stderr, "No se encuentra zx48.rom\n"); return 1; }

    // Try to load TR-DOS ROM if specified or if trdos.rom exists
    if (trdos_rom_file) {
        if (!load_trdos_rom(trdos_rom_file)) {
            fprintf(stderr, "Warning: Could not load TR-DOS ROM: %s\n", trdos_rom_file);
        }
    } else {
        // Try to load trdos.rom from current directory by default
        load_trdos_rom("trdos.rom");
    }

    z80_init(&cpu);
    cpu.read_byte  = read_byte;
    cpu.write_byte = write_byte;
    cpu.port_in    = port_in;
    cpu.port_out   = port_out;
    cpu.pc = 0x0000;
    cpu.sp = 0x0000;
    cpu.interrupt_mode = 1;
    
    // Initialize FDC
    fdc_init(&fdc);


    int frame_counter = 0;
    int flash_phase = 0;

    // Load files from command line
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            // Skip options
            if (strcmp(argv[i], "--drive-count") == 0 || strcmp(argv[i], "--trdos-rom") == 0) {
                i++; // Skip option argument
            }
            continue;
        }
        
        const char* ext = strrchr(argv[i], '.');
        if (!ext) continue;
        
        if (strcasecmp(ext, ".tap") == 0) {
            load_tap(argv[i]);
        } else if (strcasecmp(ext, ".sna") == 0) {
            load_sna(argv[i]);
        } else if (strcasecmp(ext, ".tzx") == 0) {
            load_tzx(argv[i]);
        } else if (strcasecmp(ext, ".trd") == 0) {
            // Mount TRD image
            if (next_drive < drive_count) {
                trd_image_t* img = trd_open(argv[i], read_only_disks);
                if (img) {
                    disk_images[next_drive] = img;
                    fdc_attach_image(&fdc, next_drive, img);
                    printf("Mounted TRD to drive %d\n", next_drive);
                    next_drive++;
                    trdos_enabled = true;
                }
            }
        } else if (strcasecmp(ext, ".scl") == 0) {
            // Mount SCL image
            if (next_drive < drive_count) {
                scl_image_t* img = scl_open(argv[i]);
                if (img) {
                    scl_images[next_drive] = img;
                    fdc_attach_image(&fdc, next_drive, scl_get_trd(img));
                    printf("Mounted SCL to drive %d\n", next_drive);
                    next_drive++;
                    trdos_enabled = true;
                }
            }
        }
    }
    
    if (trdos_enabled) {
        printf("\nTR-DOS enabled. Keys: F8=List disks, F12=Reset\n");
    }

    while (true) {
        handle_input();
        
        for (int line = 0; line < FULL_HEIGHT; line++) {
            z80_step_n(&cpu, 224);
            
            // Step FDC if TR-DOS enabled
            if (trdos_enabled) {
                fdc_step(&fdc, 224);
            }
            
            displayscanline(line, flash_phase);
            
            cycles_done   += (224);
            global_cycles += (uint64_t)224;
            //generate_audio(cycles_done);

            if (line == (FULL_HEIGHT -1)) {z80_pulse_irq(&cpu, 1); /*generate_audio(69888);*/}
        }

        cycles_done -= CYCLES_PER_FRAME;

        frame_counter++;

        if (frame_counter >= 16) { frame_counter = 0; flash_phase = !flash_phase; }

        update_texture();

        generate_audio(69888);
        cycles_done = 0; 
        last_audio_tstates = 0;

        SDL_Delay(10);
    }

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
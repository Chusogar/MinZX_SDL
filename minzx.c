
/*
 * ZX Spectrum 48K Emulator con SDL2 + JGZ80
 * - Carga .TAP por pulsos (pilot/sync/data) compatible ROM
 * - Carga .SNA (48K)
 * - BRIGHT aplicado a ink y paper
 * - Puerto FE: bit 6 = EAR (tape); bit 7 = espejo de bit 3 del último OUT
 * - Beeper/speaker por eventos con timestamp (T-states) → audio estable
 *
 * Compilar LINUX:     gcc minzx.c jgz80/z80.c -o minzx -lSDL2 -lm
 * Compilar WIN/MSYS2: gcc minzx.c jgz80/z80.c -o minzx.exe -lmingw32 -lSDL2main -lSDL2
 * Uso: ./minzx [fichero.tap | snapshot.sna]
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

// Si no tienes este header, puedes quitarlo sin problema
// #include "minzx.h"

#include "jgz80/z80.h"

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

// Audio (beeper)
#define AUDIO_SAMPLE_RATE 44100
#define AUDIO_BUFFER_SIZE 2048
static SDL_AudioDeviceID audio_dev = 0;
static bool speaker_state = false;   // estado lógico del bit 4 (para detectar flancos)

// Colores ZX con alfa (0xAARRGGBB)
uint32_t zx_colors[16] = {
    0xFF000000, 0xFF0000D8, 0xFFD80000, 0xFFD800D8,
    0xFF00D800, 0xFF00D8D8, 0xFFD8D800, 0xFFD8D8D8,
    0xFF000000, 0xFF0000FF, 0xFFFF0000, 0xFFFF00FF,
    0xFF00FF00, 0xFF00FFFF, 0xFFFFFF00, 0xFFFFFFFF
};

// ─────────────────────────────────────────────────────────────
// Beeper por eventos (edge-timestamped) en T-states → audio 44.1kHz
// ─────────────────────────────────────────────────────────────
#define BEEPER_EDGE_QUEUE_CAP 4096

typedef struct {
    // Audio/time
    double sample_rate;                 // p.ej., 44100
    double tstates_per_sec;             // 3500000.0
    double tstate_to_sample;            // sample_rate / tstates_per_sec

    // Estado del generador
    uint64_t last_cycle_processed;      // T-state del último punto sintetizado
    bool     level;                     // nivel actual (false=-amp, true=+amp)
    int16_t  amp_pos;                   // amplitud positiva
    int16_t  amp_neg;                   // amplitud negativa

    // Cola de flancos (T-states absolutos)
    uint64_t edges[BEEPER_EDGE_QUEUE_CAP];
    int head;                           // escribe main thread (port_out)
    int tail;                           // consume audio thread (callback)
} beeper_t;

static beeper_t beeper;

static void beeper_init(double sample_rate) {
    beeper.sample_rate       = sample_rate;
    beeper.tstates_per_sec   = 3500000.0;
    beeper.tstate_to_sample  = beeper.sample_rate / beeper.tstates_per_sec;
    beeper.last_cycle_processed = global_cycles;  // referencia inicial
    beeper.level             = false;
    beeper.amp_pos           = 11000;
    beeper.amp_neg           = -11000;
    beeper.head = beeper.tail = 0;
}

static inline void beeper_push_edge_ts(uint64_t edge_cycle) {
    int next_head = (beeper.head + 1) % BEEPER_EDGE_QUEUE_CAP;
    if (next_head == beeper.tail) {
        // cola llena → descarta el más antiguo
        beeper.tail = (beeper.tail + 1) % BEEPER_EDGE_QUEUE_CAP;
    }
    beeper.edges[beeper.head] = edge_cycle;
    beeper.head = next_head;
}

// ─────────────────────────────────────────────────────────────
// Cinta .TAP por pulsos compatible ROM
// ─────────────────────────────────────────────────────────────
typedef enum { TAPE_IDLE, TAPE_PILOT, TAPE_SYNC1, TAPE_SYNC2, TAPE_DATA, TAPE_PAUSE } tape_phase_t;

typedef struct {
    FILE* f;
    long  file_size;
    long  file_pos;

    // Bloque actual (flag + payload + checksum)
    uint8_t* blk;
    uint16_t blk_len;
    uint16_t blk_idx;     // no usado directamente (data_pos manda)
    uint8_t  flag;        // 0x00 header / 0xFF data típicamente

    // Emisión de pulsos
    tape_phase_t phase;
    int      pulses_left;            // pilot (medias ondas)
    uint32_t halfwave_ts;            // duración media onda en T-states
    uint64_t next_edge_cycle;        // ciclo (global_cycles) del próximo flanco
    bool     level;                  // EAR actual (true=1)

    // Datos
    uint16_t data_pos;               // índice dentro de blk[]
    uint8_t  cur_byte;
    int      cur_bit;                // 7..0
    int      pulse_of_bit;           // 0/1 (dos medias ondas por bit)

    // Config
    double speed;                    // 1.0 real, >1.0 más rápido
    bool   playing;
} tape_t;

static tape_t tape = {0};

// Timings (T-states) @3.5MHz
static const int TS_PILOT = 2168;
static const int TS_SYNC1 = 667;
static const int TS_SYNC2 = 735;
static const int TS_BIT0  = 855;
static const int TS_BIT1  = 1710;

// Fichero recordado (para F6 recarga)
static const char* tap_filename = NULL;

// Lee siguiente bloque del .TAP a memoria
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
    tape.blk_idx = 0;
    tape.flag    = tape.blk[0];
    return true;
}

static void tap_start_block_emission(uint64_t now_cycle) {
    tape.phase = TAPE_PILOT;
    int pilot_pulses = (tape.flag == 0x00) ? 8063 : 3223; // ondas completas
    tape.pulses_left = pilot_pulses * 2;                  // contamos medias ondas
    tape.halfwave_ts = (uint32_t)(TS_PILOT / tape.speed);
    tape.next_edge_cycle = now_cycle + tape.halfwave_ts;
    tape.level = true;

    tape.data_pos = 0;
    tape.cur_bit = 7;
    tape.pulse_of_bit = 0;
}

static void tap_start_pause(uint64_t now_cycle) {
    tape.phase = TAPE_PAUSE;
    uint32_t pause_ts = (uint32_t)(3500000 / tape.speed); // ~1s
    tape.next_edge_cycle = now_cycle + pause_ts;
    tape.level = true;
}

static inline uint32_t tap_halwave_for_bit(bool bit1) {
    return (uint32_t)((bit1 ? TS_BIT1 : TS_BIT0) / tape.speed);
}

static bool tap_ear_level_until(uint64_t now_cycle) {
    if (!tape.playing || !tape.f || tape.phase == TAPE_IDLE) return true; // EAR alto
    while (now_cycle >= tape.next_edge_cycle) {
        // flanco → invertimos nivel
        tape.level = !tape.level;
        switch (tape.phase) {
            case TAPE_PILOT:
                if (--tape.pulses_left > 0) {
                    tape.next_edge_cycle += tape.halfwave_ts;
                } else {
                    tape.phase = TAPE_SYNC1;
                    tape.halfwave_ts = (uint32_t)(TS_SYNC1 / tape.speed);
                    tape.next_edge_cycle += tape.halfwave_ts;
                }
                break;
            case TAPE_SYNC1:
                tape.phase = TAPE_SYNC2;
                tape.halfwave_ts = (uint32_t)(TS_SYNC2 / tape.speed);
                tape.next_edge_cycle += tape.halfwave_ts;
                break;
            case TAPE_SYNC2:
                tape.phase = TAPE_DATA;
                tape.data_pos = 0;
                tape.cur_bit = 7;
                tape.pulse_of_bit = 0;
                tape.cur_byte = tape.blk[tape.data_pos++];
                {
                    bool b = (tape.cur_byte & 0x80) != 0;
                    tape.halfwave_ts = tap_halwave_for_bit(b);
                    tape.next_edge_cycle += tape.halfwave_ts;
                }
                break;
            case TAPE_DATA: {
                tape.pulse_of_bit ^= 1;
                if (tape.pulse_of_bit == 1) {
                    tape.next_edge_cycle += tape.halfwave_ts; // segunda media onda
                } else {
                    if (--tape.cur_bit < 0) {
                        if (tape.data_pos >= tape.blk_len) {
                            tap_start_pause(now_cycle); // fin de bloque
                            break;
                        }
                        tape.cur_bit = 7;
                        tape.cur_byte = tape.blk[tape.data_pos++];
                    }
                    bool b = ((tape.cur_byte >> tape.cur_bit) & 1) != 0;
                    tape.halfwave_ts = tap_halwave_for_bit(b);
                    tape.next_edge_cycle += tape.halfwave_ts;
                }
            } break;
            case TAPE_PAUSE:
                if (!tap_read_next_block()) {
                    tape.phase = TAPE_IDLE;
                    tape.playing = false;
                    tape.level = true; // EAR alto
                } else {
                    tap_start_block_emission(now_cycle);
                }
                break;
            default: break;
        }
        if (tape.phase == TAPE_IDLE) break;
    }
    return tape.level;
}

bool load_tap(const char* filename) {
    if (tape.f) { fclose(tape.f); tape.f = NULL; }
    tape.f = fopen(filename, "rb");
    if (!tape.f) {
        printf("No se pudo abrir %s\n", filename);
        tape.playing = false;
        return false;
    }

    fseek(tape.f, 0, SEEK_END);
    tape.file_size = ftell(tape.f);
    fseek(tape.f, 0, SEEK_SET);
    tape.file_pos = 0;

    free(tape.blk); tape.blk = NULL;
    tape.blk_len = tape.blk_idx = 0;

    tape.speed   = 1.0;   // 2x (ajusta: 1.0..8.0)
    tape.playing = true;

    if (!tap_read_next_block()) {
        printf("TAP vacío.\n");
        tape.playing = false;
        return false;
    }
    tap_start_block_emission(global_cycles);
    border_color = 7;

    printf("TAP cargado: %s (%ld bytes)\n", filename, tape.file_size);
    tap_filename = filename; // recordar para F6 recarga
    return true;
}

static inline bool get_current_ear_level_from_tape(void) {
    return tap_ear_level_until(global_cycles);
}

// ─────────────────────────────────────────────────────────────
// Contended RAM (placeholder; desactivado)
// ─────────────────────────────────────────────────────────────
int contended_delay(uint16_t addr, int tstates_in_line) {
    (void)addr; (void)tstates_in_line;
    return 0;
}

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

    if ((port & 1) == 0) { // FE
        uint8_t hi = port >> 8;

        // Teclado (fila activa cuando bit de 'hi' = 0)
        for (int r = 0; r < 8; r++)
            if ((hi & (1 << r)) == 0)
                res &= keyboard[r];

        // Bit 5 = 1 en 48K (sin MIC)
        res |= 0x20;

        // Bit 7 = espejo del bit 3 del último OUT a FE
        if (last_fe_write & 0x08) res &= ~0x80; else res |= 0x80;

        // Bit 6 = EAR desde la cinta por pulsos
        bool ear = get_current_ear_level_from_tape();
        if (ear) res |= 0x40; else res &= ~0x40;
    }

    return res;
}

void port_out(z80* z, uint16_t port, uint8_t val) {
    (void)z;
    if ((port & 1) == 0) {
        border_color   = val & 0x07;
        last_fe_write  = val;

        // Beeper: bit 4
        bool new_state = (val & 0x10) != 0;
        if (new_state != speaker_state) {
            speaker_state = new_state;

            // Registrar flanco en beeper con timestamp (T-states)
            if (audio_dev) SDL_LockAudioDevice(audio_dev);
            beeper_push_edge_ts(global_cycles);
            if (audio_dev) SDL_UnlockAudioDevice(audio_dev);
        }
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
            + ((vy & 0xC0) << 5)   // filas 0..2
            + ((vy & 0x07) << 8)   // línea dentro del bloque de 8
            + ((vy & 0x38) << 2);  // bloque de 8 líneas

        int addr_att = 0x5800 + (32 * (vy >> 3));

        for (int bx = 0; bx < 32; bx++) {
            uint8_t pix = memory[addr_pix++];
            uint8_t att = memory[addr_att++];

            int bright = (att & 0x40) ? 8 : 0;
            int ink    = (att & 0x07) + bright;
            int paper  = ((att >> 3) & 0x07) + bright;

            if (att & 0x80) { // FLASH
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
// Audio callback (beeper por eventos)
// ─────────────────────────────────────────────────────────────
void audio_callback(void* userdata, uint8_t* stream, int len) {
    (void)userdata;
    int16_t* out = (int16_t*)stream;
    int samples_to_write = len / sizeof(int16_t);

    int produced = 0;
    while (produced < samples_to_write) {
        // Próximo flanco (si existe)
        uint64_t next_edge = (beeper.tail != beeper.head)
                           ? beeper.edges[beeper.tail]
                           : UINT64_MAX;

        // ¿Cuántas muestras hasta el flanco?
        double samples_until_edge_d;
        if (next_edge == UINT64_MAX) {
            samples_until_edge_d = (double)(samples_to_write - produced);
        } else {
            uint64_t delta_tstates = (next_edge > beeper.last_cycle_processed)
                ? (next_edge - beeper.last_cycle_processed)
                : 0;
            samples_until_edge_d = delta_tstates * beeper.tstate_to_sample;
            if (samples_until_edge_d < 0.0) samples_until_edge_d = 0.0;
        }

        int chunk = (int)samples_until_edge_d;
        if (chunk <= 0) {
            // Estamos en/justo sobre el flanco → cambiamos nivel y consumimos el evento
            beeper.level = !beeper.level;
            beeper.last_cycle_processed = next_edge;
            if (beeper.tail != beeper.head) {
                beeper.tail = (beeper.tail + 1) % BEEPER_EDGE_QUEUE_CAP;
            }
            continue;
        }

        if (chunk > (samples_to_write - produced))
            chunk = samples_to_write - produced;

        // Escribir 'chunk' muestras al nivel actual
        int16_t val = beeper.level ? beeper.amp_pos : beeper.amp_neg;
        for (int i = 0; i < chunk; ++i) {
            out[produced++] = val;
        }

        // Avanza el “tiempo” sintetizado (en T-states equivalentes)
        double tstates_advanced_d = (double)chunk / beeper.tstate_to_sample;
        uint64_t tstates_advanced = (uint64_t)(tstates_advanced_d + 0.5);
        beeper.last_cycle_processed += tstates_advanced;
    }
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
            if (tap_filename) load_tap(tap_filename);
        }

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
                if (press)
                    keyboard[row] &= ~(1 << bit);
                else
                    keyboard[row] |= (1 << bit);
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────
// Carga de snapshot .sna (48K)
// ─────────────────────────────────────────────────────────────
bool load_sna(const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "No se pudo abrir .sna: %s\n", filename);
        return false;
    }

    uint8_t header[27];
    if (fread(header, 1, 27, f) != 27) {
        fclose(f);
        fprintf(stderr, "Archivo .sna incompleto (header)\n");
        return false;
    }

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
    cpu.r      = header[20];  // conserva bit 7
    cpu.af     = (header[22] << 8) | header[21];
    cpu.sp     = (header[24] << 8) | header[23];
    cpu.interrupt_mode = header[25];
    border_color = header[26] & 0x07;

    if (fread(&memory[RAM_START], 1, 49152, f) != 49152) {
        fclose(f);
        fprintf(stderr, "Archivo .sna incompleto (RAM)\n");
        return false;
    }
    fclose(f);

    // PC desde la pila (pop)
    uint16_t sp = cpu.sp;
    cpu.pc = (memory[sp+1] << 8) | memory[sp];
    cpu.sp += 2;

    cpu.iff1 = cpu.iff2;

    printf("Snapshot .sna cargado: %s\n", filename);
    printf("PC=0x%04X  SP=0x%04X  Border=%d  IM=%d\n", cpu.pc, cpu.sp, border_color, cpu.interrupt_mode);

    return true;
}

// ─────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    window = SDL_CreateWindow("Minimal ZX 48K",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              FULL_WIDTH * SCALE, FULL_HEIGHT * SCALE, 0);

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(renderer, FULL_WIDTH, FULL_HEIGHT);

    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STATIC, FULL_WIDTH, FULL_HEIGHT);

    if (!load_rom("zx48.rom")) {
        fprintf(stderr, "No se encuentra zx48.rom\n");
        return 1;
    }

    // Inicializar CPU
    z80_init(&cpu);
    cpu.read_byte  = read_byte;
    cpu.write_byte = write_byte;
    cpu.port_in    = port_in;
    cpu.port_out   = port_out;

    cpu.pc = 0x0000;
    cpu.sp = 0x0000;
    cpu.interrupt_mode = 1;

    // Audio
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq     = AUDIO_SAMPLE_RATE;
    want.format   = AUDIO_S16SYS;
    want.channels = 1;
    want.samples  = AUDIO_BUFFER_SIZE;
    want.callback = audio_callback;

    audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio_dev == 0) {
        fprintf(stderr, "Audio init falló: %s\n", SDL_GetError());
    } else {
        // Inicializa el beeper con la frecuencia REAL del dispositivo
        beeper_init((double)have.freq);
        SDL_PauseAudioDevice(audio_dev, 0);  // empieza reproduciendo
    }

    int frame_counter = 0;
    int flash_phase = 0;

    if (argc > 1) {
        const char* ext = strrchr(argv[1], '.');
        if (ext && strcasecmp(ext, ".tap") == 0) {
            load_tap(argv[1]);
        } else if (ext && strcasecmp(ext, ".sna") == 0) {
            load_sna(argv[1]);
        }
    }

    while (true) {
        handle_input();

        for (int line = 0; line < FULL_HEIGHT; line++) {
            z80_step_n(&cpu, 224);
            displayscanline(line, flash_phase);

            cycles_done   += 224;           // puede replegarse
            global_cycles += (uint64_t)224; // referencia estable para cinta y audio

            if (line == (FULL_HEIGHT -1)) {
                // RST 38h por frame (IM1). Alternativamente en line==0.
                z80_pulse_irq(&cpu, 1);
            }
        }

        // Mantén cycles_done estable por frame si quieres, pero NO toques global_cycles
        cycles_done -= CYCLES_PER_FRAME;

        frame_counter++;
        if (frame_counter >= 16) { // flash ≈ 1.56 Hz
            frame_counter = 0;
            flash_phase = !flash_phase;
        }

        update_texture();

        // Con PRESENTVSYNC no es necesario delay
        //SDL_Delay(1);
    }

    if (audio_dev) SDL_CloseAudioDevice(audio_dev);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

/*
 * ZX Spectrum 48K/128K Emulator con SDL2 + JGZ80
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
 *
 * 128K Support:
 * - Memory banking: 8 RAM banks of 16KB each (total 128KB)
 * - ROM banking: 2 ROM banks of 16KB each (48K BASIC + 128K editor)
 * - Port 0x7FFD: Memory paging control
 *   - Bits 0-2: RAM bank at 0xC000-0xFFFF
 *   - Bit 3: Video page select (not implemented)
 *   - Bit 4: ROM select (0 = 48K, 1 = 128K)
 *   - Bit 5: Paging disable (locks configuration)
 * - AY-3-8912 sound chip (placeholder):
 *   - Port 0xFFFD: Register select
 *   - Port 0xBFFD: Data write/read
 *
 * Compilar LINUX:     gcc minzx.c jgz80/z80.c -o minzx -lSDL2 -lm
 * Compilar WIN/MSYS2: gcc minzx.c jgz80/z80.c -o minzx.exe -lmingw32 -lSDL2main -lSDL2
 * Uso: ./minzx [--128k] [cinta.tap | cinta.tzx | snapshot.sna]
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

// ─────────────────────────────────────────────────────────────
// Globals
// ─────────────────────────────────────────────────────────────
#define MEM_SIZE	0x10000
#define ROM_SIZE	16384
#define RAM_START	16384

z80 cpu;
uint8_t mem[MEM_SIZE];
uint32_t tstates;
uint32_t cycleTstates;

SDL_Window*   window   = NULL;
SDL_Renderer* renderer = NULL;
SDL_Texture*  texture  = NULL;

#define FULL_WIDTH	320
#define FULL_HEIGHT 240
#define SCALE		2

uint32_t pixels[FULL_HEIGHT * FULL_WIDTH];
static const int TOTAL_SCANLINES = 312;
static const int TOP_BORDER_LINES = 64;
static const int VISIBLE_LINES = 192;
static const int TSTATES_PER_SCANLINE = 224;
static const int FETCH_SLOTS_PER_LINE = 16;
static const int TSTATES_ACTIVE_FETCH = 128;

uint8_t border_color   = 7;
int _num_frames = 0;
bool _flash_act = false;
uint8_t last_fe_write = 0x00;
// Scanline-based rendering
int currentScanline;          // 0..311
uint32_t tstatesThisLine;
// Floating bus
int ulaFetchPhase;            // -1 = idle, 0..15 = slot activo
bool isInVisibleArea;
uint16_t currentVideoAddress;
bool intPending;

uint8_t keyboard[8]    = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

// Audio (beeper)
bool speakerLevel;
uint32_t lastTstate;
double fractional;

const double CLOCK_FREQ = 3500000.0;
const int    AUDIO_SAMPLE_RATE = 44100;
const double TSTATES_PER_SAMPLE = CLOCK_FREQ / AUDIO_SAMPLE_RATE;
const int16_t HIGH_LEVEL = 8000;
const int16_t LOW_LEVEL = -8000;
const double FILTER_ALPHA = 0.5;

void load_bios(void);
bool is_128k_mode = false;


// Colores ZX con alfa (0xAARRGGBB)
uint32_t zx_colors[16] = {
    0xFF000000, 0xFF0000D8, 0xFFD80000, 0xFFD800D8,
    0xFF00D800, 0xFF00D8D8, 0xFFD8D800, 0xFFD8D8D8,
    0xFF000000, 0xFF0000FF, 0xFFFF0000, 0xFFFF00FF,
    0xFF00FF00, 0xFF00FFFF, 0xFFFFFF00, 0xFFFFFFFF
};

// Incrementa tstates y notifica al TzxPlayer si está reproduciendo
void addTstates(uint32_t delta)
{
    if (delta == 0) return;
    tstates += delta;
    /*if (tapePlayer && tapePlaying) {
        tapePlayer->advanceByTstates(delta);
    }*/
}



// ─────────────────────────────────────────────────────────────
// Memoria y puertos
// ─────────────────────────────────────────────────────────────
/*bool MinZX::isActiveINT(void)
{
    return intPending;
}

void MinZX::interruptHandlingTime(int32_t wstates)
{
    addTstates(wstates);
    intPending = false;
}*/

unsigned char delay_contention(uint16_t address, unsigned int tstates)
{
    tstates += 1;
    int line = tstates / 224;
    if (line < 64 || line >= 256) return 0;
    int halfpix = tstates % 224;
    if (halfpix >= 128) return 0;
    int modulo = halfpix % 8;
    static unsigned char wait_states[8] = { 6,5,4,3,2,1,0,0 };
    return wait_states[modulo];
}

/*uint8_t MinZX::fetchOpcode(uint16_t address)
{
    if ((address >> 14) == 1)
        addTstates(delay_contention(address, tstates));
    addTstates(4);
    return mem[address];
}*/

uint8_t read_byte(void* ud, uint16_t address) {
	if ((address >> 14) == 1)
        addTstates(delay_contention(address, tstates));
    addTstates(3);
    return mem[address];
}

void write_byte(void* ud, uint16_t address, uint8_t value) {
	if ((address >> 14) == 1)
        addTstates(delay_contention(address, tstates));
    addTstates(3);
    mem[address] = value;
}

/*uint16_t MinZX::peek16(uint16_t address)
{
    uint8_t lo = peek8(address);
    uint8_t hi = peek8(address + 1);
    return (hi << 8) | lo;
}

void MinZX::poke16(uint16_t address, RegisterPair word)
{
    poke8(address, word.byte8.lo);
    poke8(address + 1, word.byte8.hi);
}*/



void addressOnBus(uint16_t address, int32_t wstates)
{
    if ((address >> 14) == 1)
    {
        for (int i = 0; i < wstates; i++)
            addTstates(delay_contention(address, tstates) + 1);
    }
    else
        addTstates(wstates);
}

void updateULAFetchState()
{
    uint32_t tInLine = tstates % TSTATES_PER_SCANLINE;

    if (currentScanline < TOP_BORDER_LINES || currentScanline >= TOP_BORDER_LINES + VISIBLE_LINES)
    {
        isInVisibleArea = false;
        ulaFetchPhase = -1;
        currentVideoAddress = 0;
        return;
    }

    isInVisibleArea = true;

    if (tInLine >= TSTATES_ACTIVE_FETCH)
    {
        ulaFetchPhase = -1;
        currentVideoAddress = 0;
        return;
    }

    int slot = tInLine / 8;
    int subT = tInLine % 8;

    if (subT >= 4)
    {
        ulaFetchPhase = -1;
        currentVideoAddress = 0;
        return;
    }

    ulaFetchPhase = slot;

    int charX = slot * 2 + (subT / 2);
    bool isAttr = (subT % 2) == 1;

    int speY = currentScanline - TOP_BORDER_LINES;
    int ulaY = ((speY & 0xC0) | ((speY & 0x38) >> 3) | ((speY & 0x07) << 3));

    if (isAttr)
        currentVideoAddress = 0x5800 + ((speY >> 3) << 5) + charX;
    else
        currentVideoAddress = 0x4000 + (ulaY << 5) + charX;
}

void flushAudioBuffer(uint32_t upToTstate)
{

    if (upToTstate <= lastTstate) return;
#if 0
    uint32_t delta_t = upToTstate - lastTstate;
    double delta_samples = static_cast<double>(delta_t) / TSTATES_PER_SAMPLE;
    int num_samples = static_cast<int>(delta_samples + fractional);
    fractional = delta_samples + fractional - static_cast<double>(num_samples);

    int16_t beeperLevel = speakerLevel ? HIGH_LEVEL : LOW_LEVEL;

    // Prepare tape buffer only when playing
    std::vector<int16_t> tapeBuf;
    tapeBuf.resize(num_samples);
    /*if (tapePlayer && tapePlaying) {
        tapePlayer->generateSamples(static_cast<size_t>(num_samples), tapeBuf.data());
    }
    else {*/
        for (int i = 0; i < num_samples; ++i) tapeBuf[i] = 0;
    //}

    for (int i = 0; i < num_samples; ++i) {
        int mixed = static_cast<int>(beeperLevel) + static_cast<int>(tapeBuf[i]);
        if (mixed > INT16_MAX) mixed = INT16_MAX;
        if (mixed < INT16_MIN) mixed = INT16_MIN;
        audioBuffer.push_back(static_cast<int16_t>(mixed));
    }
#endif
}


uint8_t processInputPort(uint16_t port)
{
    uint8_t hi = port >> 8;
    uint8_t lo = port & 0xFF;

    // Teclado + ULA (puertos pares)
    if ((lo & 0x01) == 0)
    {
        uint8_t result = 0xBF;

        // Teclado
        for (int r = 0; r < 8; r++)
            if ((hi & (1 << r)) == 0)
                result &= keyboard[r];

        // EAR - mapear desde tapePlayer si existe y está reproduciendo
        //if (tapePlayer && tapePlaying) {
            // tapePlayer->earLevelHigh() == true -> no pulse (line HIGH)
            // cuando hay pulso queremos que el bit 6 sea 0 (activo)
            //if (!tapePlayer->earLevelHigh())
            //    result &= static_cast<uint8_t>(~0x40);
        //}
        //uint8_t ear_bit = tape.get_ear() ? 0x40 : 0x00;  // bit6 = 1 si HIGH
        // EAR (bit 6)
        //if (tape.motor)
        //    result = (result & 0xBF) | (tape.get_ear() ? 0x40 : 0x00);  // 0xBF = ~0x40
        //if(!tape.get_ear())
        //    result &= (~0x40);


        //if (Tape_GetEAR()) result &= ~(1 << 6);
        //else               
        //result |= (1 << 6);

        if (last_fe_write & 0x18) result |= 0x40;


        return result;
    }

    // Floating bus para puertos no decodificados (excepto Kempston)
    if (lo != 0x1F)
    {
        updateULAFetchState();

        if (!isInVisibleArea || ulaFetchPhase < 0)
            return 0xFF;

        return mem[currentVideoAddress];
    }

    return 0xFF; // Kempston o default
}

uint8_t port_in(z80* z, uint16_t port) {
	addTstates(3);
    return processInputPort(port);    
}

void processOutputPort(uint16_t port, uint8_t value) {
	
	uint8_t lo = port & 0xFF;

    if (lo == 0xFE)
    {
        flushAudioBuffer(tstates);
        speakerLevel = (value & 0x10) != 0;
        lastTstate = tstates;

        border_color = value & 0x07;

        //tape.motor = !!(value & 0x08);

        last_fe_write = value;

    }
}

void port_out(z80* z, uint16_t port, uint8_t value) {
	addTstates(4);
    processOutputPort(port, value);    
}

void zx_reset(void) {
	printf("RESET!\n");
	z80_init(&cpu);
    cpu.read_byte  = read_byte;
    cpu.write_byte = write_byte;
    cpu.port_in    = port_in;
    cpu.port_out   = port_out;
    cpu.pc = 0x0000;
    cpu.sp = 0x0000;
    cpu.interrupt_mode = 1;

}

void init(void)
{
    z80_init(&cpu);
    
    memset(mem, 0x00, MEM_SIZE);
    memset(keyboard, 0xFF, sizeof(keyboard));

    cycleTstates = 69888;
    load_bios();

    //createSpectrumColors();

    intPending = false;
    speakerLevel = false;
    lastTstate = 0;
    fractional = 0.0;
    currentScanline = 0;
    tstatesThisLine = 0;
    ulaFetchPhase = -1;
    isInVisibleArea = false;
    currentVideoAddress = 0;

    // Inicializa el reproductor de cinta a nullptr (se puede asignar con FileMgr)
    //tapePlayer = nullptr;
    //tapePlaying = false;

    zx_reset();
}

// ─────────────────────────────────────────────────────────────
// ROM
// ─────────────────────────────────────────────────────────────
bool load_rom(const char* fn) {
    FILE* f = fopen(fn, "rb");
    if (!f) return false;
    size_t rd = fread(mem, 1, ROM_SIZE, f);
    fclose(f);
    return rd == ROM_SIZE;
}


// ─────────────────────────────────────────────────────────────
// Vídeo
// ─────────────────────────────────────────────────────────────
void renderScanline()
{
	//printf("Render!\n");
    if (currentScanline < 0 || currentScanline >= TOTAL_SCANLINES)
        return;

    uint32_t borderColor = zx_colors[border_color];

    int displayY = currentScanline - (TOP_BORDER_LINES - 24);
    if (displayY < 0 || displayY >= 240)
        return;

    uint32_t* linePtr = (uint32_t*)(pixels + displayY * 320 /* *4 */);

    if (currentScanline < TOP_BORDER_LINES || currentScanline >= TOP_BORDER_LINES + VISIBLE_LINES)
    {
        for (int x = 0; x < 320; x++)
            linePtr[x] = borderColor;
    }
    else
    {
        int speY = currentScanline - TOP_BORDER_LINES;
        int ulaY = ((speY & 0xC0) | ((speY & 0x38) >> 3) | ((speY & 0x07) << 3));

        int bmpBase = 0x4000 + (ulaY << 5);
        int attBase = 0x5800 + ((speY >> 3) << 5);

        uint8_t* bmpPtr = mem + bmpBase;
        uint8_t* attPtr = mem + attBase;

        for (int x = 0; x < 32; x++)
            linePtr[x] = borderColor;

        for (int charX = 0; charX < 32; charX++)
        {
            uint8_t bmp = bmpPtr[charX];
            uint8_t att = attPtr[charX];

            int ink = att & 7;
            int pap = (att >> 3) & 7;
            bool br = (att & 0x40) != 0;

            uint32_t fore = 0;
            uint32_t back = 0;

            if (((att & 0x80) == 0) || (_flash_act == 0)) {
                fore = zx_colors[ink/*, br*/];
                back = zx_colors[pap/*, br*/];
            }
            else {
                fore = zx_colors[pap/*, br*/];
                back = zx_colors[ink/*, br*/];
            }


            int px = 32 + charX * 8;
            for (int bit = 7; bit >= 0; bit--)
                linePtr[px++] = (bmp & (1 << bit)) ? fore : back;
        }

        for (int x = 32 + 256; x < 320; x++)
            linePtr[x] = borderColor;
    }

}

void update_texture() {
    SDL_UpdateTexture(texture, NULL, pixels, FULL_WIDTH * sizeof(uint32_t));
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}


void load_bios(void) {
	if (!load_rom("zx48.rom")) { 
		fprintf(stderr, "No se encuentra zx48.rom\n"); 
		return; 
	}
	printf("Ejecutando en modo 48K\n");
}


// ─────────────────────────────────────────────────────────────
// Teclado (eventos)
// ─────────────────────────────────────────────────────────────
void handle_input() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE))
            exit(0);

        if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F12){
            //z80_reset(&cpu);
			//port_7ffd = 0x00;                      // Memory paging register
			//paging_disabled = 0;
			zx_reset();
			load_bios();
		}


        /*if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F12)
            z80_reset(&cpu);
		*/

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

    if (!is_128k_mode) {
        // 48K snapshot: load directly into memory
        if (fread(&mem[RAM_START], 1, 49152, f) != 49152) { 
            fclose(f); 
            fprintf(stderr, "Archivo .sna incompleto (RAM)\n"); 
            return false; 
        }
        
        uint16_t sp = cpu.sp;
        cpu.pc = (mem[sp+1] << 8) | mem[sp];
        cpu.sp += 2;
    }
    
    fclose(f);

    cpu.iff1 = cpu.iff2;

    printf("Snapshot .sna cargado: %s\n", filename);
    printf("PC=0x%04X  SP=0x%04X  Border=%d  IM=%d\n", cpu.pc, cpu.sp, border_color, cpu.interrupt_mode);

    return true;
}

void zx_update(void) {
	tstates = 0;
    currentScanline = 0;
    tstatesThisLine = 0;
    ulaFetchPhase = -1;
    isInVisibleArea = false;
    currentVideoAddress = 0;

    lastTstate = 0;

    while (tstates < cycleTstates)
    {
		for (int _ci=0 ; _ci<224 ; _ci++ )
		{
			//tstates += 4;
			z80_step_n(&cpu, 1);
			//tape.advance(10);
		}
		

		tstates += 224;

        while (tstates >= (currentScanline + 1) * TSTATES_PER_SCANLINE)
        {
            renderScanline();
            currentScanline++;

            //tape.advance(224);
            flushAudioBuffer(224);
            //applyLowPassFilter();
        }
    }

    if (_num_frames == 16) {   // FLASH ~ 1.56 Hz (50/32 ≈ 1.56)
        _num_frames = 0;
        _flash_act = !_flash_act;
    }

    _num_frames++;

    //flushAudioBuffer(69888/4);
    //tape.advance(6998);
    //applyLowPassFilter();

    //tape.advance(tstates);

    /*while (currentScanline < TOTAL_SCANLINES)
    {
        renderScanline();
        currentScanline++;
    }*/

    intPending = true;
	z80_pulse_irq(&cpu, 1);
	intPending = false;
	

    tstates -= cycleTstates;
}

// ─────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
	bool read_only_disks = false;
    int drive_count = 2; // Default 2 drives
    int next_drive = 0;

    // Parse command-line arguments
    const char* tape_file = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--128k") == 0) {
            is_128k_mode = 1;
        } 
    }
    
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }


    const char* window_title = "Minimal ZX 48K";
    window = SDL_CreateWindow(window_title,
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              FULL_WIDTH * SCALE, FULL_HEIGHT * SCALE, 0);

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(renderer, FULL_WIDTH, FULL_HEIGHT);

    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STATIC, FULL_WIDTH, FULL_HEIGHT);

    init();
	
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
			//tape_file = argv[i];
            //load_tap(argv[i]);
        } else if (strcasecmp(ext, ".sna") == 0) {
            load_sna(argv[i]);
        }
    }

    while (true) {
        handle_input();

		zx_update();

		update_texture();

		

        //SDL_Delay(16);
    }

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
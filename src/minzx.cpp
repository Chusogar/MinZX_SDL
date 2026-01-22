#include "minzx.h"
#include "z80.h"
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <vector>

#include "tape/tape_stream.h"
#include "tape/tap_loader.h"

#define TRACE   printf
#define DEBUG   printf
#define LOG     printf
#define INFO    printf
#define NOTICE  printf
#define WARN    printf
#define ERROR   printf
#define FATAL   printf

#define VAL_BRIGHT    255
#define VAL_NO_BRIGHT 176

uint32_t speColors[16];

static void createSpectrumColors()
{
    uint32_t A = 0xFF000000;
    uint32_t r = VAL_NO_BRIGHT << 16;
    uint32_t g = VAL_NO_BRIGHT << 8;
    uint32_t b = VAL_NO_BRIGHT;
    uint32_t R = VAL_BRIGHT << 16;
    uint32_t G = VAL_BRIGHT << 8;
    uint32_t B = VAL_BRIGHT;

    speColors[0] = A;
    speColors[1] = A | b;
    speColors[2] = A | r;
    speColors[3] = A | r | b;
    speColors[4] = A | g;
    speColors[5] = A | g | b;
    speColors[6] = A | r | g;
    speColors[7] = A | r | g | b;
    speColors[8] = A;
    speColors[9] = A | B;
    speColors[10] = A | R;
    speColors[11] = A | R | B;
    speColors[12] = A | G;
    speColors[13] = A | G | B;
    speColors[14] = A | R | G;
    speColors[15] = A | R | G | B;
}

uint32_t MinZX::zxColor(int c, bool bright)
{
    c &= 7;
    if (bright) c += 8;
    return speColors[c];
}

const double CLOCK_FREQ = 3500000.0;
const int    AUDIO_SAMPLE_RATE = 44100;
const double TSTATES_PER_SAMPLE = CLOCK_FREQ / AUDIO_SAMPLE_RATE;
const int16_t HIGH_LEVEL = 8000;
const int16_t LOW_LEVEL = -8000;
const double FILTER_ALPHA = 0.5;

void MinZX::init()
{
    z80 = new Z80(this);
    mem = new uint8_t[0x10000];
    ports = new uint8_t[0x10000];

    memset(mem, 0x00, 0x10000);
    memset(ports, 0xFF, 0x10000);
    memset(keymatrix, 0xFF, sizeof(keymatrix));

    cycleTstates = 69888;
    loadROM();

    createSpectrumColors();

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
    tapePlaying = false;

    reset();
}

// Incrementa tstates y notifica al TzxPlayer si está reproduciendo
void MinZX::addTstates(uint32_t delta)
{
    if (delta == 0) return;
    tstates += delta;
    /*if (tapePlayer && tapePlaying) {
        tapePlayer->advanceByTstates(delta);
    }*/
}

void MinZX::reset()
{
    border = 7;
    z80->reset();

    ports[0x001F] = 0;
    ports[0x011F] = 0;

    memset(keymatrix, 0xFF, sizeof(keymatrix));
    intPending = false;

    speakerLevel = false;
    lastTstate = 0;
    fractional = 0.0;
    audioBuffer.clear();

    currentScanline = 0;
    tstatesThisLine = 0;
    ulaFetchPhase = -1;
    isInVisibleArea = false;
    currentVideoAddress = 0;

    //if (tapePlayer) tapePlayer->rewind();
    tapePlaying = false;
}


// --- Tape driver simple ---
static TapeStream* sTape = nullptr;
static size_t sTapeIdx = 0;         // índice del pulso actual
static uint32_t sTapeUsLeft = 0;    // microsegundos restantes del pulso actual
static bool sEarLevel = true;       // alterna con cada semionda

void Tape_Attach(TapeStream* t) {
    sTape = t;
    sTapeIdx = 0;
    sTapeUsLeft = 0;
    sEarLevel = true;
}

inline void Tape_StepUS(uint32_t us_step) {
    if (!sTape || sTape->pulses.empty()) return;
    while (us_step > 0) {
        if (sTapeIdx >= sTape->pulses.size()) return;

        if (sTapeUsLeft == 0) {
            sTapeUsLeft = sTape->pulses[sTapeIdx].us;
            if (sTapeUsLeft == 0) {
                // Pausa: consume y avanza
                sTapeIdx++;
                continue;
            }
        }

        if (us_step >= sTapeUsLeft) {
            us_step -= sTapeUsLeft;
            sTapeUsLeft = 0;
            sTapeIdx++;
            // Borde: alterna nivel
            sEarLevel = !sEarLevel;
        }
        else {
            sTapeUsLeft -= us_step;
            us_step = 0;
        }
    }
}

inline bool Tape_GetEAR() {
    // Convención: true = tono presente → bit 6 = 0 (línea baja en EAR)
    return sEarLevel;
}


int _num_frames = 0;
bool _flash_act = false;

void MinZX::update(uint8_t* screen)
{
    screenPtr = screen;

    tstates = 0;
    currentScanline = 0;
    tstatesThisLine = 0;
    ulaFetchPhase = -1;
    isInVisibleArea = false;
    currentVideoAddress = 0;

    lastTstate = 0;

    while (tstates < cycleTstates)
    {
        z80->execute();
        //tape.advance(10);

        while (tstates >= (currentScanline + 1) * TSTATES_PER_SCANLINE)
        {
            renderScanline();
            currentScanline++;

            tape.advance(224);
            //flushAudioBuffer(224);
            //applyLowPassFilter();
        }
    }

    if (_num_frames == 16) {   // FLASH ~ 1.56 Hz (50/32 ≈ 1.56)
        _num_frames = 0;
        _flash_act = !_flash_act;
    }

    _num_frames++;

    //flushAudioBuffer(cycleTstates);
    //tape.advance(6998);
    applyLowPassFilter();

    //tape.advance(tstates);

    /*while (currentScanline < TOTAL_SCANLINES)
    {
        renderScanline();
        currentScanline++;
    }*/

    intPending = true;


    tstates -= cycleTstates;
}

void MinZX::renderScanline()
{
    if (currentScanline < 0 || currentScanline >= TOTAL_SCANLINES)
        return;

    uint32_t borderColor = zxColor(border, false);

    int displayY = currentScanline - (TOP_BORDER_LINES - 24);
    if (displayY < 0 || displayY >= 240)
        return;

    uint32_t* linePtr = (uint32_t*)(screenPtr + displayY * 320 * 4);

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
                fore = zxColor(ink, br);
                back = zxColor(pap, br);
            }
            else {
                fore = zxColor(pap, br);
                back = zxColor(ink, br);
            }


            int px = 32 + charX * 8;
            for (int bit = 7; bit >= 0; bit--)
                linePtr[px++] = (bmp & (1 << bit)) ? fore : back;
        }

        for (int x = 32 + 256; x < 320; x++)
            linePtr[x] = borderColor;
    }
}

void MinZX::flushAudioBuffer(uint32_t upToTstate)
{
    if (upToTstate <= lastTstate) return;

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
}

void MinZX::applyLowPassFilter()
{
    if (audioBuffer.empty()) return;

    int16_t prev = 0;
    for (auto& sample : audioBuffer)
    {
        sample = static_cast<int16_t>(FILTER_ALPHA * sample + (1.0 - FILTER_ALPHA) * prev);
        prev = sample;
    }
}

void MinZX::updateULAFetchState()
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

uint8_t MinZX::processInputPort(uint16_t port)
{
    uint8_t hi = port >> 8;
    uint8_t lo = port & 0xFF;

    // Teclado + ULA (puertos pares)
    if ((lo & 0x01) == 0)
    {
        uint8_t result = 0xFF;

        for (int row = 0; row < 8; row++)
            if ((hi & (1 << row)) == 0)
                result &= keymatrix[row];

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


        if (Tape_GetEAR()) result &= ~(1 << 6);
        else               result |= (1 << 6);


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

void MinZX::processOutputPort(uint16_t port, uint8_t value)
{
    uint8_t lo = port & 0xFF;

    if (lo == 0xFE)
    {
        //flushAudioBuffer(tstates);
        speakerLevel = (value & 0x10) != 0;
        lastTstate = tstates;

        border = value & 0x07;

        tape.motor = !!(value & 0x08);

    }
}

void MinZX::keyPress(int row, int bit, bool press)
{
    if (press)
        keymatrix[row] &= ~(1 << bit);
    else
        keymatrix[row] |= (1 << bit);
}

bool MinZX::isActiveINT(void)
{
    return intPending;
}

void MinZX::interruptHandlingTime(int32_t wstates)
{
    addTstates(wstates);
    intPending = false;
}

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

uint8_t MinZX::fetchOpcode(uint16_t address)
{
    if ((address >> 14) == 1)
        addTstates(delay_contention(address, tstates));
    addTstates(4);
    return mem[address];
}

uint8_t MinZX::peek8(uint16_t address)
{
    if ((address >> 14) == 1)
        addTstates(delay_contention(address, tstates));
    addTstates(3);
    return mem[address];
}

void MinZX::poke8(uint16_t address, uint8_t value)
{
    if ((address >> 14) == 1)
        addTstates(delay_contention(address, tstates));
    addTstates(3);
    mem[address] = value;
}

uint16_t MinZX::peek16(uint16_t address)
{
    uint8_t lo = peek8(address);
    uint8_t hi = peek8(address + 1);
    return (hi << 8) | lo;
}

void MinZX::poke16(uint16_t address, RegisterPair word)
{
    poke8(address, word.byte8.lo);
    poke8(address + 1, word.byte8.hi);
}

uint8_t MinZX::inPort(uint16_t port)
{
    addTstates(3);
    return processInputPort(port);
}

void MinZX::outPort(uint16_t port, uint8_t value)
{
    addTstates(4);
    processOutputPort(port, value);
}

void MinZX::addressOnBus(uint16_t address, int32_t wstates)
{
    if ((address >> 14) == 1)
    {
        for (int i = 0; i < wstates; i++)
            addTstates(delay_contention(address, tstates) + 1);
    }
    else
        addTstates(wstates);
}



void MinZX::loadROM()
{
    FILE* pf = fopen("zx48.rom", "rb");
    if (!pf)
    {
        ERROR("Cannot load zx48.rom\n");
        return;
    }
    fread(mem, 1, 0x4000, pf);
    fclose(pf);
}

void MinZX::loadDump()
{
    // opcional - implementación vacía por ahora
}

void MinZX::destroy()
{
    delete z80;
    delete[] mem;
    delete[] ports;
    //if (tapePlayer) { delete tapePlayer; tapePlayer = nullptr; }
}
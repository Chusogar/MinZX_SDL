#ifndef _MINZX_H_
#define _MINZX_H_

#include <inttypes.h>
#include <vector>
#include "z80.h"

class MinZX : public Z80operations
{
public:
    void init();
    void update(uint8_t* screen);
    void destroy();
    void reset();

    void setBorderColor(uint8_t bcol) { border = bcol; }
    void keyPress(int row, int bit, bool press);

    Z80*     getCPU()    { return z80; }
    uint8_t* getMemory() { return mem; }

    const std::vector<int16_t>& getAudioBuffer() const { return audioBuffer; }
    void clearAudioBuffer() { audioBuffer.clear(); }

public:
    virtual uint8_t  fetchOpcode (uint16_t address);
    virtual uint8_t  peek8       (uint16_t address);
    virtual void     poke8       (uint16_t address, uint8_t value);
    virtual uint16_t peek16      (uint16_t address);
    virtual void     poke16      (uint16_t address, RegisterPair word);
    virtual uint8_t  inPort      (uint16_t port);
    virtual void     outPort     (uint16_t port, uint8_t value);
    virtual void     addressOnBus(uint16_t address, int32_t wstates);
    virtual void     interruptHandlingTime(int32_t wstates);
    virtual bool     isActiveINT (void);
#ifdef WITH_BREAKPOINT_SUPPORT
    virtual uint8_t  breakpoint  (uint16_t address, uint8_t opcode);
#endif
#ifdef WITH_EXEC_DONE
    virtual void     execDone(void);
#endif

private:
    Z80* z80;
    uint8_t* mem;
    uint8_t* ports;
    uint32_t tstates;

    uint32_t cycleTstates;

    void loadROM();
    void loadDump();

    uint8_t processInputPort(uint16_t port);
    void processOutputPort(uint16_t port, uint8_t value);

    uint8_t border;
    uint8_t keymatrix[8];
    bool intPending;

    // Audio (beeper)
    bool speakerLevel;
    uint32_t lastTstate;
    double fractional;
    std::vector<int16_t> audioBuffer;

    void flushAudioBuffer(uint32_t upToTstate);
    void applyLowPassFilter();

    // Scanline-based rendering
    int currentScanline;          // 0..311
    uint32_t tstatesThisLine;
    uint8_t* screenPtr;           // buffer ARGB8888 320×240

    void renderScanline();
    uint32_t zxColor(int c, bool bright);

    // Floating bus
    int ulaFetchPhase;            // -1 = idle, 0..15 = slot activo
    bool isInVisibleArea;
    uint16_t currentVideoAddress;

    void updateULAFetchState();

    static const int TOTAL_SCANLINES       = 312;
    static const int TOP_BORDER_LINES      = 64;
    static const int VISIBLE_LINES         = 192;
    static const int TSTATES_PER_SCANLINE  = 224;
    static const int FETCH_SLOTS_PER_LINE  = 16;
    static const int TSTATES_ACTIVE_FETCH  = 128;
};

#endif // _MINZX_H_
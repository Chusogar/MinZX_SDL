// Simple Z80 CPU emulator structure
// Compatible with minzx.c usage

#ifndef JGZ80_H
#define JGZ80_H

#include <stdint.h>

typedef struct z80_s {
    // 16-bit registers
    uint16_t af, bc, de, hl;
    uint16_t ix, iy, sp, pc;
    uint16_t a_f_, b_c_, d_e_, h_l_;  // shadow registers
    
    // 8-bit registers
    uint8_t i, r;
    uint8_t iff1, iff2;
    uint8_t interrupt_mode;
    
    // Memory and I/O callbacks
    uint8_t (*read_byte)(uint16_t addr);
    void (*write_byte)(uint16_t addr, uint8_t val);
    uint8_t (*port_in)(uint16_t port);
    void (*port_out)(uint16_t port, uint8_t val);
} z80;

// Function prototypes
void z80_init(z80* cpu);
void z80_step_n(z80* cpu, int cycles);
void z80_pulse_irq(z80* cpu, int active);

#endif // JGZ80_H

/*
 * JGZ80 - Simple Z80 CPU Emulator in C
 * Basic Z80 emulation core for MinZX
 */

#ifndef JGZ80_Z80_H
#define JGZ80_Z80_H

#include <stdint.h>
#include <stdbool.h>

typedef struct z80_s z80;

// Memory and I/O callbacks
typedef uint8_t (*z80_read_func)(z80* cpu, uint16_t addr);
typedef void (*z80_write_func)(z80* cpu, uint16_t addr, uint8_t val);
typedef uint8_t (*z80_port_in_func)(z80* cpu, uint16_t port);
typedef void (*z80_port_out_func)(z80* cpu, uint16_t port, uint8_t val);

// Z80 CPU structure
struct z80_s {
    // Main registers
    uint16_t pc;
    uint16_t sp;
    uint16_t af, bc, de, hl;
    uint16_t ix, iy;
    
    // Alternate registers
    uint16_t af_, bc_, de_, hl_;
    
    // Special registers
    uint8_t i;  // Interrupt vector
    uint8_t r;  // Refresh counter
    uint8_t r7; // Bit 7 of R
    
    // Interrupt state
    bool iff1, iff2;
    uint8_t interrupt_mode; // 0, 1, or 2
    bool halted;
    
    // Callbacks
    z80_read_func read_byte;
    z80_write_func write_byte;
    z80_port_in_func port_in;
    z80_port_out_func port_out;
    
    // Timing
    uint32_t cycles; // Total cycles executed
};

// Core API
void z80_init(z80* cpu);
void z80_reset(z80* cpu);
uint32_t z80_step(z80* cpu);        // Execute one instruction, return cycles
void z80_step_n(z80* cpu, uint32_t n); // Execute n cycles worth of instructions
void z80_pulse_irq(z80* cpu, uint8_t mode);  // Trigger interrupt
void z80_pulse_nmi(z80* cpu);       // Trigger NMI

// Helper macros for register access
#define Z80_A(cpu)  ((uint8_t)((cpu)->af >> 8))
#define Z80_F(cpu)  ((uint8_t)((cpu)->af))
#define Z80_B(cpu)  ((uint8_t)((cpu)->bc >> 8))
#define Z80_C(cpu)  ((uint8_t)((cpu)->bc))
#define Z80_D(cpu)  ((uint8_t)((cpu)->de >> 8))
#define Z80_E(cpu)  ((uint8_t)((cpu)->de))
#define Z80_H(cpu)  ((uint8_t)((cpu)->hl >> 8))
#define Z80_L(cpu)  ((uint8_t)((cpu)->hl))

#define Z80_SET_A(cpu, v) ((cpu)->af = ((cpu)->af & 0x00FF) | ((v) << 8))
#define Z80_SET_F(cpu, v) ((cpu)->af = ((cpu)->af & 0xFF00) | (v))
#define Z80_SET_B(cpu, v) ((cpu)->bc = ((cpu)->bc & 0x00FF) | ((v) << 8))
#define Z80_SET_C(cpu, v) ((cpu)->bc = ((cpu)->bc & 0xFF00) | (v))
#define Z80_SET_D(cpu, v) ((cpu)->de = ((cpu)->de & 0x00FF) | ((v) << 8))
#define Z80_SET_E(cpu, v) ((cpu)->de = ((cpu)->de & 0xFF00) | (v))
#define Z80_SET_H(cpu, v) ((cpu)->hl = ((cpu)->hl & 0x00FF) | ((v) << 8))
#define Z80_SET_L(cpu, v) ((cpu)->hl = ((cpu)->hl & 0xFF00) | (v))

// Flag bits
#define Z80_FLAG_C  0x01
#define Z80_FLAG_N  0x02
#define Z80_FLAG_PV 0x04
#define Z80_FLAG_H  0x10
#define Z80_FLAG_Z  0x40
#define Z80_FLAG_S  0x80

#endif // JGZ80_Z80_H

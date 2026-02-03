/*
 * JGZ80 - Simple Z80 CPU Emulator in C
 * Minimal Z80 core for ZX Spectrum emulation
 * 
 * This is a simplified implementation focusing on common Z80 instructions.
 * For a complete emulation, a full instruction decoder would be needed.
 */

#include "z80.h"
#include <string.h>
#include <stdio.h>

// Parity lookup table - not thread-safe, initialize before multi-threaded use
static uint8_t parity_table[256];
static bool parity_initialized = false;

static void init_parity_table(void) {
    if (parity_initialized) return;
    for (int i = 0; i < 256; i++) {
        int count = 0;
        int val = i;
        while (val) {
            count += val & 1;
            val >>= 1;
        }
        parity_table[i] = (count & 1) ? 0 : Z80_FLAG_PV;
    }
    parity_initialized = true;
}

// Initialize Z80 CPU
void z80_init(z80* cpu) {
    init_parity_table();
    memset(cpu, 0, sizeof(z80));
    cpu->af = cpu->bc = cpu->de = cpu->hl = 0xFFFF;
    cpu->af_ = cpu->bc_ = cpu->de_ = cpu->hl_ = 0xFFFF;
    cpu->ix = cpu->iy = 0xFFFF;
    cpu->sp = 0xFFFF;
    cpu->pc = 0x0000;
    cpu->i = 0x00;
    cpu->r = 0x00;
    cpu->r7 = 0x00;
    cpu->iff1 = cpu->iff2 = false;
    cpu->interrupt_mode = 1;
    cpu->halted = false;
    cpu->cycles = 0;
}

// Reset Z80 CPU
void z80_reset(z80* cpu) {
    cpu->pc = 0x0000;
    cpu->iff1 = cpu->iff2 = false;
    cpu->halted = false;
    cpu->r = 0x00;
}

// Read a byte from memory via callback
static inline uint8_t read_mem(z80* cpu, uint16_t addr) {
    return cpu->read_byte ? cpu->read_byte(cpu, addr) : 0xFF;
}

// Write a byte to memory via callback
static inline void write_mem(z80* cpu, uint16_t addr, uint8_t val) {
    if (cpu->write_byte) cpu->write_byte(cpu, addr, val);
}

// Read from port via callback
static inline uint8_t read_port(z80* cpu, uint16_t port) {
    return cpu->port_in ? cpu->port_in(cpu, port) : 0xFF;
}

// Write to port via callback
static inline void write_port(z80* cpu, uint16_t port, uint8_t val) {
    if (cpu->port_out) cpu->port_out(cpu, port, val);
}

// Fetch next instruction byte
static inline uint8_t fetch(z80* cpu) {
    uint8_t val = read_mem(cpu, cpu->pc++);
    cpu->r = (cpu->r & 0x80) | ((cpu->r + 1) & 0x7F);
    return val;
}

// Push word to stack
static inline void push(z80* cpu, uint16_t val) {
    cpu->sp -= 2;
    write_mem(cpu, cpu->sp, val & 0xFF);
    write_mem(cpu, cpu->sp + 1, val >> 8);
}

// Pop word from stack
static inline uint16_t pop(z80* cpu) {
    uint16_t val = read_mem(cpu, cpu->sp) | (read_mem(cpu, cpu->sp + 1) << 8);
    cpu->sp += 2;
    return val;
}

// Update flags after arithmetic/logic operations
static inline void update_flags_szp(z80* cpu, uint8_t result) {
    uint8_t f = Z80_F(cpu) & Z80_FLAG_C; // Preserve carry
    if (result == 0) f |= Z80_FLAG_Z;
    if (result & 0x80) f |= Z80_FLAG_S;
    f |= parity_table[result];
    Z80_SET_F(cpu, f);
}

// Execute one Z80 instruction - SIMPLIFIED VERSION
// This implements only the most common instructions needed for basic emulation
// A complete Z80 emulator would need all ~1500 instruction variants
uint32_t z80_step(z80* cpu) {
    uint32_t cycles = 4; // Base cycle count
    
    if (cpu->halted) {
        return 4; // Just burn cycles while halted
    }
    
    uint8_t opcode = fetch(cpu);
    
    switch (opcode) {
        case 0x00: // NOP
            cycles = 4;
            break;
            
        case 0x01: // LD BC,nn
            cpu->bc = fetch(cpu) | (fetch(cpu) << 8);
            cycles = 10;
            break;
            
        case 0x06: // LD B,n
            Z80_SET_B(cpu, fetch(cpu));
            cycles = 7;
            break;
            
        case 0x0E: // LD C,n
            Z80_SET_C(cpu, fetch(cpu));
            cycles = 7;
            break;
            
        case 0x11: // LD DE,nn
            cpu->de = fetch(cpu) | (fetch(cpu) << 8);
            cycles = 10;
            break;
            
        case 0x16: // LD D,n
            Z80_SET_D(cpu, fetch(cpu));
            cycles = 7;
            break;
            
        case 0x1E: // LD E,n
            Z80_SET_E(cpu, fetch(cpu));
            cycles = 7;
            break;
            
        case 0x21: // LD HL,nn
            cpu->hl = fetch(cpu) | (fetch(cpu) << 8);
            cycles = 10;
            break;
            
        case 0x26: // LD H,n
            Z80_SET_H(cpu, fetch(cpu));
            cycles = 7;
            break;
            
        case 0x2E: // LD L,n
            Z80_SET_L(cpu, fetch(cpu));
            cycles = 7;
            break;
            
        case 0x31: // LD SP,nn
            cpu->sp = fetch(cpu) | (fetch(cpu) << 8);
            cycles = 10;
            break;
            
        case 0x3E: // LD A,n
            Z80_SET_A(cpu, fetch(cpu));
            cycles = 7;
            break;
            
        case 0x76: // HALT
            cpu->halted = true;
            cycles = 4;
            break;
            
        case 0xC3: // JP nn
            cpu->pc = fetch(cpu) | (fetch(cpu) << 8);
            cycles = 10;
            break;
            
        case 0xC9: // RET
            cpu->pc = pop(cpu);
            cycles = 10;
            break;
            
        case 0xCD: // CALL nn
            {
                uint16_t addr = fetch(cpu) | (fetch(cpu) << 8);
                push(cpu, cpu->pc);
                cpu->pc = addr;
                cycles = 17;
            }
            break;
            
        case 0xD3: // OUT (n),A
            {
                uint8_t port = fetch(cpu);
                write_port(cpu, port, Z80_A(cpu));
                cycles = 11;
            }
            break;
            
        case 0xDB: // IN A,(n)
            {
                uint8_t port = fetch(cpu);
                Z80_SET_A(cpu, read_port(cpu, port));
                cycles = 11;
            }
            break;
            
        case 0xED: // Extended instructions
            {
                uint8_t ed_op = fetch(cpu);
                switch (ed_op) {
                    case 0x40: // IN B,(C)
                        Z80_SET_B(cpu, read_port(cpu, cpu->bc));
                        update_flags_szp(cpu, Z80_B(cpu));
                        cycles = 12;
                        break;
                    case 0x41: // OUT (C),B
                        write_port(cpu, cpu->bc, Z80_B(cpu));
                        cycles = 12;
                        break;
                    case 0x48: // IN C,(C)
                        Z80_SET_C(cpu, read_port(cpu, cpu->bc));
                        update_flags_szp(cpu, Z80_C(cpu));
                        cycles = 12;
                        break;
                    case 0x49: // OUT (C),C
                        write_port(cpu, cpu->bc, Z80_C(cpu));
                        cycles = 12;
                        break;
                    case 0x50: // IN D,(C)
                        Z80_SET_D(cpu, read_port(cpu, cpu->bc));
                        update_flags_szp(cpu, Z80_D(cpu));
                        cycles = 12;
                        break;
                    case 0x51: // OUT (C),D
                        write_port(cpu, cpu->bc, Z80_D(cpu));
                        cycles = 12;
                        break;
                    case 0x56: // IM 1
                        cpu->interrupt_mode = 1;
                        cycles = 8;
                        break;
                    case 0x5E: // IM 2
                        cpu->interrupt_mode = 2;
                        cycles = 8;
                        break;
                    case 0x78: // IN A,(C)
                        Z80_SET_A(cpu, read_port(cpu, cpu->bc));
                        update_flags_szp(cpu, Z80_A(cpu));
                        cycles = 12;
                        break;
                    case 0x79: // OUT (C),A
                        write_port(cpu, cpu->bc, Z80_A(cpu));
                        cycles = 12;
                        break;
                    case 0xB0: // LDIR
                        {
                            write_mem(cpu, cpu->de, read_mem(cpu, cpu->hl));
                            cpu->hl++;
                            cpu->de++;
                            cpu->bc--;
                            if (cpu->bc != 0) {
                                cpu->pc -= 2; // Repeat
                                cycles = 21;
                            } else {
                                cycles = 16;
                            }
                        }
                        break;
                    default:
                        // Unknown ED instruction - treat as NOP
                        cycles = 8;
                        break;
                }
            }
            break;
            
        case 0xF3: // DI
            cpu->iff1 = cpu->iff2 = false;
            cycles = 4;
            break;
            
        case 0xFB: // EI
            cpu->iff1 = cpu->iff2 = true;
            cycles = 4;
            break;
            
        default:
            // Unknown instruction - NOP it
            // In a real emulator, we'd decode all 1500+ instruction variants
            cycles = 4;
            break;
    }
    
    cpu->cycles += cycles;
    return cycles;
}

// Execute n cycles worth of instructions
void z80_step_n(z80* cpu, uint32_t n) {
    uint32_t target = cpu->cycles + n;
    while (cpu->cycles < target) {
        z80_step(cpu);
    }
}

// Trigger maskable interrupt (IRQ)
void z80_pulse_irq(z80* cpu, uint8_t mode) {
    (void)mode; // Mode parameter for compatibility
    
    if (!cpu->iff1) return; // Interrupts disabled
    
    cpu->halted = false;
    cpu->iff1 = cpu->iff2 = false; // Disable interrupts
    
    // For ZX Spectrum, we mainly use IM 1
    if (cpu->interrupt_mode == 1) {
        // RST 38h
        push(cpu, cpu->pc);
        cpu->pc = 0x0038;
    } else if (cpu->interrupt_mode == 2) {
        // IM 2 mode - vector interrupt
        push(cpu, cpu->pc);
        uint16_t vector = (cpu->i << 8) | 0xFF;
        uint16_t addr = read_mem(cpu, vector) | (read_mem(cpu, vector + 1) << 8);
        cpu->pc = addr;
    } else {
        // IM 0 - not commonly used in ZX Spectrum
        push(cpu, cpu->pc);
        cpu->pc = 0x0038;
    }
}

// Trigger non-maskable interrupt (NMI)
void z80_pulse_nmi(z80* cpu) {
    cpu->halted = false;
    cpu->iff2 = cpu->iff1;
    cpu->iff1 = false;
    push(cpu, cpu->pc);
    cpu->pc = 0x0066;
}

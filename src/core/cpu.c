#include <stdlib.h> 
#include <string.h>

#include "core/emulator.h"
#include "core/cart.h"
#include "core/mmu.h"
#include "core/cpu.h"

#include "util/common.h"
#include "util/disassembler.h"

typedef bool (*OpcodeHandler)(CPU*);

// Various Helpers

static uint16_t form_address(CPU *cpu)
{ 
    uint16_t   upper = cpu->ins.high;
    uint16_t   lower = cpu->ins.low;
    uint16_t address = (upper << BYTE) |  lower;
    return address;
}

static void write_flag_reg(CPU *cpu, uint8_t value)
{
    cpu->reg.F = (value & 0xF0); // Only the upper nibble.
}

static void set_flag(CPU *cpu, bool is_set, Flag flag_mask)
{
    uint8_t value = is_set ? (cpu->reg.F | flag_mask) : (cpu->reg.F & ~flag_mask);
    write_flag_reg(cpu, value);
}

static bool is_flag_set(CPU *cpu, Flag flag)
{
    return ((cpu->reg.F & flag) != 0);
}

static uint8_t fetch(CPU *cpu)
{
    uint8_t rom_byte = 0x00; // Jusssssst in case...

    if (cpu->halt_bug_active)
    {
        cpu->halt_bug_active = false;
        rom_byte = read_memory(cpu->mem, cpu->reg.PC);
    }
    else
    {
        rom_byte = read_memory(cpu->mem, cpu->reg.PC++);
    }

    return rom_byte;
}

static uint16_t getDR(CPU *cpu, DualRegister dr)
{
    switch(dr)
    {
        case AF_REG: return (uint16_t)((cpu->reg.A << BYTE) | cpu->reg.F);
        case BC_REG: return (uint16_t)((cpu->reg.B << BYTE) | cpu->reg.C);
        case DE_REG: return (uint16_t)((cpu->reg.D << BYTE) | cpu->reg.E);
        case HL_REG: return (uint16_t)((cpu->reg.H << BYTE) | cpu->reg.L);
        case SP_REG: return cpu->reg.SP;
        default: 
            return 0;
    }
}

static void setDR(CPU *cpu, DualRegister dr, uint16_t source)
{ // Optimization? Unions for Dual Registers!
    switch(dr)
    {
        case AF_REG:
            cpu->reg.A = ((uint8_t) (source >> BYTE));
            cpu->reg.F = ((uint8_t) (source & 0xF0));            
            break;
        case BC_REG:
            cpu->reg.B = ((uint8_t) (source >> BYTE));
            cpu->reg.C = ((uint8_t) source & 0xFF);
            break;
        case DE_REG:
            cpu->reg.D = ((uint8_t) (source >> BYTE));
            cpu->reg.E = ((uint8_t) source & 0xFF);
            break;
        case HL_REG:
            cpu->reg.H = ((uint8_t) (source >> BYTE));
            cpu->reg.L = ((uint8_t) source & 0xFF);
            break;
        case SP_REG:
            cpu->reg.SP = source;
            break;
        default:
            break;
    }
}

static uint8_t pop_stack(CPU *cpu)
{
    uint8_t result = read_memory(cpu->mem, cpu->reg.SP);
    cpu->reg.SP += 1; 
    return result;
}

static void push_stack(CPU *cpu, uint8_t value)
{
    cpu->reg.SP -= 1;
    write_memory(cpu->mem, cpu->reg.SP, value);
}

static void encode_interrupt(CPU *cpu, uint8_t pending)
{
    if (pending & VBLANK_INTERRUPT_CODE)
    {
        cpu->ins.address = VBLANK_VECTOR;
        cpu->ins.  label = "VBLANK INTTERRUPT";
        cpu->ins.    low = VBLANK_INTERRUPT_CODE;
        return;
    }
    
    if (pending & LCD_STAT_INTERRUPT_CODE)
    {
        cpu->ins.address = LCD_VECTOR;
        cpu->ins.  label = "LCD INTTERRUPT";
        cpu->ins.    low = LCD_STAT_INTERRUPT_CODE;
        return;
    }

    if (pending & TIMER_INTERRUPT_CODE)
    {
        cpu->ins.address = TIMER_VECTOR;
        cpu->ins.  label = "TIMER INTTERRUPT";
        cpu->ins.    low = TIMER_INTERRUPT_CODE;
        return;
    }

    if (pending & SERIAL_INTERRUPT_CODE)
    {
        cpu->ins.address = SERIAL_VECTOR;
        cpu->ins.  label = "SERIAL INTTERRUPT";
        cpu->ins.    low = SERIAL_INTERRUPT_CODE;
        return;
    }

    if (pending & JOYPAD_INTERRUPT_CODE)
    {
        cpu->ins.address = JOYPAD_VECTOR;
        cpu->ins.  label = "JOYPAD INTTERRUPT";
        cpu->ins.    low = JOYPAD_INTERRUPT_CODE;
        return;
    }
}

static uint8_t get_pending_interrupts(CPU *cpu)
{
    uint8_t ifr = *cpu->reg.IFR & LOWER_5_MASK;
    uint8_t ier = *cpu->reg.IER & LOWER_5_MASK;
    return ifr & ier;
}

char *get_cpu_state(CPU *cpu, char *buffer, size_t size)
{
    snprintf(
        buffer, 
        size,
        "IME-%d | PC-$%04X | SP-$%04X | INT-($%02X & $%02X : $%02X) ||$%02X|| - %-17s",
        cpu->ime, 
        cpu->reg.PC, 
        cpu->reg.SP,
        (*cpu->reg.IER),
        (*cpu->reg.IFR),
        get_pending_interrupts(cpu), 
        cpu->ins.opcode, 
        cpu->ins.label
    );
    
    return buffer;
}

char *get_reg_state(CPU *cpu, char *buffer, size_t size)
{
    snprintf(
        buffer,
        size,
        "[%d] A-$%02X%02X-F || B-$%02X%02X-C || D-$%02X%02X-E || H-$%02X%02X-L [PC=%04X] [SP=%04X] $%02X- %-17s",
        cpu->ime, cpu->reg.A, cpu->reg.F, cpu->reg.B, cpu->reg.C, cpu->reg.D, cpu->reg.E, cpu->reg.H, cpu->reg.L, cpu->reg.PC, cpu->reg.SP, cpu->ins.opcode, cpu->ins.label
    );

    return buffer;
}

// CPU Opcode Implementations

static bool nop(CPU *cpu)        // 0x00 (- - - -) 1M
{
    return true;
}
static bool halt(CPU *cpu)       // 0x76 (- - - -) 1M
{
    uint8_t pending = get_pending_interrupts(cpu);

    if ((cpu->ime == 0) && (pending != 0))
    {
        cpu->halt_bug_active = true;
        cpu->halted = false;
    }
    else
    {
        cpu->halted = (pending == 0);
    }

    return true;
}
static bool stop(CPU *cpu)       // 0x10
{
    fetch(cpu); // STOP is a 2-byte instruction; second byte is unused

    uint8_t key1 = read_memory(cpu->mem, KEY1);

    if (cpu->cart->is_gbc && ((key1 & BIT_0_MASK) != cpu->speed_enabled)) // Prepare Speed Switch set
    {
        cpu->speed_enabled = !cpu->speed_enabled;

        // Set bit 7 = speed, clear bit 0 = handshake complete
        uint8_t new_key1 = (cpu->speed_enabled << 7);
        write_memory(cpu->mem, KEY1, new_key1);

        return true;
    }

    return true;
}

static bool ld_bc_nn(CPU *cpu)   // 0x01 (- - - -) 3M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->reg.C = fetch(cpu);
            return false;

        case 3:
            cpu->reg.B = fetch(cpu);
            return true; // Instruction complete
    }

    return true;
}
static bool ld_de_nn(CPU *cpu)   // 0x11 (- - - -) 3M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->reg.E = fetch(cpu);
            return false;
        
        case 3:
            cpu->reg.D = fetch(cpu);
            return true;
    }

    return true;
}
static bool ld_hl_nn(CPU *cpu)   // 0x21 (- - - -) 3M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->reg.L = fetch(cpu);
            return false;
        
        case 3:
            cpu->reg.H = fetch(cpu);
            return true;
    }

    return true;
}
static bool ld_sp_nn(CPU *cpu)   // 0x31 (- - - -) 3M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.low = fetch(cpu);
            return false;
        
        case 3:
            cpu->ins.high = fetch(cpu);
            cpu->reg.  SP = form_address(cpu);
            return true;
    }

    return true;
}

static void reg_inc_16(CPU *cpu, DualRegister dr)
{
    switch(dr)
    {
        case BC_REG: 
            cpu->reg.C += 1;
            if (cpu->reg.C == 0) cpu->reg.B += 1; 
            break;
        case DE_REG: 
            cpu->reg.E += 1;
            if (cpu->reg.E == 0) cpu->reg.D += 1; 
            break;
        case HL_REG:
            cpu->reg.L += 1;
            if (cpu->reg.L == 0) cpu->reg.H += 1; 
            break;
        case SP_REG:
            cpu->reg.SP += 1;
            break;
    }
}
static bool reg_inc_16_handler(CPU *cpu, DualRegister dr)
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            reg_inc_16(cpu, dr);
            return true;
    }

    return true;
}
static bool inc_bc(CPU *cpu)     // 0x03 (- - - -) 2M
{
    return reg_inc_16_handler(cpu, BC_REG);
}
static bool inc_de(CPU *cpu)     // 0x13 (- - - -) 2M
{ 
    return reg_inc_16_handler(cpu, DE_REG);
}
static bool inc_hl(CPU *cpu)     // 0x23 (- - - -) 2M
{
    return reg_inc_16_handler(cpu, HL_REG);
}
static bool inc_sp(CPU *cpu)     // 0x33 (- - - -) 2M
{ 
    return reg_inc_16_handler(cpu, SP_REG);
}

static void reg_dec_16(CPU *cpu, DualRegister dr)
{
    switch(dr)
    {
        case BC_REG:
            cpu->reg.C -= 1;
            if (cpu->reg.C == BYTE_UNDERFLOW) cpu->reg.B -= 1; 
            break;
        case DE_REG: 
            cpu->reg.E -= 1;
            if (cpu->reg.E == BYTE_UNDERFLOW) cpu->reg.D -= 1;
            break;
        case HL_REG:
            cpu->reg.L -= 1;
            if (cpu->reg.L == BYTE_UNDERFLOW) cpu->reg.H -= 1;
            break;
        case SP_REG:
            cpu->reg.SP -= 1;
            break;
    }
}
static bool reg_dec_16_handler(CPU *cpu, DualRegister dr)
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            reg_dec_16(cpu, dr);
            return true;
    }

    return false;
}
static bool dec_bc(CPU *cpu)     // 0x0B (- - - -) 2M
{
    return reg_dec_16_handler(cpu, BC_REG);
}
static bool dec_de(CPU *cpu)     // 0x1B (- - - -) 2M
{
    return reg_dec_16_handler(cpu, DE_REG);
}
static bool dec_hl(CPU *cpu)     // 0x2B (- - - -) 2M
{
    return reg_dec_16_handler(cpu, HL_REG);
}
static bool dec_sp(CPU *cpu)     // 0x3B (- - - -) 2M
{ 
    return reg_dec_16_handler(cpu, SP_REG);
}

static bool pop_bc(CPU *cpu)     // 0xC1 (- - - -) 3M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->reg.C = pop_stack(cpu);
            return false;
        
        case 3:
            cpu->reg.B = pop_stack(cpu);
            return true;
    }

    return true;
}
static bool pop_de(CPU *cpu)     // 0xD1 (- - - -) 3M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->reg.E = pop_stack(cpu);
            return false;
        
        case 3:
            cpu->reg.D = pop_stack(cpu);
            return true;
    }

    return true;
}
static bool pop_hl(CPU *cpu)     // 0xE1 (- - - -) 3M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->reg.L = pop_stack(cpu);
            return false;
        
        case 3:
            cpu->reg.H = pop_stack(cpu);
            return true;
    }

    return true;
}
static bool pop_af(CPU *cpu)     // 0xF1 (Z N H C) 3M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            write_flag_reg(cpu, pop_stack(cpu));
            return false;
        
        case 3:
            cpu->reg.A = pop_stack(cpu);
            return true;
    }

    return true;
}

static bool push_bc(CPU *cpu)    // 0xC5 (- - - -) 4M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            return false;
        
        case 3:
            push_stack(cpu, cpu->reg.B);
            return false;
        case 4:
            push_stack(cpu, cpu->reg.C);
            return true; // Instruction Complete
    }

    return true;
}
static bool push_de(CPU *cpu)    // 0xD5 (- - - -) 4M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            return false;
        
        case 3:
            push_stack(cpu, cpu->reg.D);
            return false;
        case 4:
            push_stack(cpu, cpu->reg.E);
            return true; // Instruction Complete
    }
    
    return true;
}
static bool push_hl(CPU *cpu)    // 0xE5 (- - - -) 4M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            return false;
        
        case 3:
            push_stack(cpu, cpu->reg.H);
            return false;
        case 4:
            push_stack(cpu, cpu->reg.L);
            return true; // Instruction Complete
    }
    
    return true;
}
static bool push_af(CPU *cpu)    // 0xF5 (- - - -) 4M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            return false;
        
        case 3:
            push_stack(cpu, cpu->reg.A);
            return false;
        case 4:
            push_stack(cpu, cpu->reg.F);
            return true; // Instruction Complete
    }
    
    return true;
}

static bool ld_hli_a(CPU *cpu)   // 0x22 (- - - -) 2M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            uint16_t hl = getDR(cpu, HL_REG);
            write_memory(cpu->mem, hl, cpu->reg.A);
            setDR(cpu, HL_REG, (hl + 1));
            return true;
    }

    return true;
}
static bool ld_a_hli(CPU *cpu)   // 0x2A (- - - -) 2M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            uint16_t hl = getDR(cpu, HL_REG);
            cpu->reg.A = read_memory(cpu->mem, hl);
            setDR(cpu, HL_REG, (hl + 1));
            return true;
    }

    return true;
}
static bool ld_hld_a(CPU *cpu)   // 0x32 (- - - -) 2M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            uint16_t hl = getDR(cpu, HL_REG);
            write_memory(cpu->mem, hl, cpu->reg.A);
            setDR(cpu, HL_REG, (hl - 1));
            return true;
    }
    
    return true;
}
static bool ld_a_hld(CPU *cpu)   // 0x3A (- - - -) 2M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            uint16_t hl = getDR(cpu, HL_REG);
            cpu->reg.A = read_memory(cpu->mem, hl);
            setDR(cpu, HL_REG, (hl - 1));
            return true;
    }

    return true;
}

static bool ld_bc_a(CPU *cpu)    // 0x02 (- - - -) 2M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            uint16_t bc = getDR(cpu, BC_REG);
            write_memory(cpu->mem, bc, cpu->reg.A);
            return true;
    }

    return true;
}
static bool ld_de_a(CPU *cpu)    // 0x12 (- - - -) 2M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            uint16_t de = getDR(cpu, DE_REG);
            write_memory(cpu->mem, de, cpu->reg.A);
            return true;
    }

    return true;
}
static bool ld_nn_sp(CPU *cpu)   // 0x08 (- - - -) 5M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.low = fetch(cpu);
            return false;
        
        case 3:
            cpu->ins.   high = fetch(cpu);
            cpu->ins.address = form_address(cpu);
            return false;

        case 4:
            uint8_t sp_low = cpu->reg.SP & LOWER_BYTE_MASK;
            write_memory(cpu->mem, cpu->ins.address, sp_low);
            return false;
        
        case 5:
            uint8_t sp_high = (cpu->reg.SP >> BYTE) & LOWER_BYTE_MASK;
            write_memory(cpu->mem, cpu->ins.address + 1, sp_high);
            return true;
    }

    return true;
}

static bool ld_hl_reg(CPU *cpu, uint8_t reg)
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            uint16_t hl = getDR(cpu, HL_REG);
            write_memory(cpu->mem, hl, reg);
            return true;
    }

    return true;
}
static bool ld_hl_b(CPU *cpu)    // 0x70 (- - - -) 2M
{
    return ld_hl_reg(cpu, cpu->reg.B);
}
static bool ld_hl_c(CPU *cpu)    // 0x71 (- - - -) 2M
{
    return ld_hl_reg(cpu, cpu->reg.C);
}
static bool ld_hl_d(CPU *cpu)    // 0x72 (- - - -) 2M
{
    return ld_hl_reg(cpu, cpu->reg.D);
}
static bool ld_hl_e(CPU *cpu)    // 0x63 (- - - -) 2M
{
    return ld_hl_reg(cpu, cpu->reg.E);
}
static bool ld_hl_h(CPU *cpu)    // 0x64 (- - - -) 2M
{
    return ld_hl_reg(cpu, cpu->reg.H);
}
static bool ld_hl_l(CPU *cpu)    // 0x65 (- - - -) 2M
{
    return ld_hl_reg(cpu, cpu->reg.L);
}
static bool ld_hl_a(CPU *cpu)    // 0x77 (- - - -) 2M
{
    return ld_hl_reg(cpu, cpu->reg.A);
}

static bool ld_b_c(CPU *cpu)     // 0x41 (- - - -) 1M
{
    cpu->reg.B = cpu->reg.C;
    return true;
}
static bool ld_b_d(CPU *cpu)     // 0x42 (- - - -) 1M
{
    cpu->reg.B = cpu->reg.D;
    return true;
}
static bool ld_b_e(CPU *cpu)     // 0x43 (- - - -) 1M
{
    cpu->reg.B = cpu->reg.E;
    return true;
}
static bool ld_b_h(CPU *cpu)     // 0x44 (- - - -) 1M
{
    cpu->reg.B = cpu->reg.H;
    return true;
}
static bool ld_b_l(CPU *cpu)     // 0x45 (- - - -) 1M
{
    cpu->reg.B = cpu->reg.L;
    return true;
}
static bool ld_b_hl(CPU *cpu)    // 0x46 (- - - -) 2M
{
    if (cpu->ins.duration == 1) // First Cycle
    {
        return false;
    }

    if (cpu->ins.duration == 2) // Second Cycle
    {
        uint16_t hl = getDR(cpu, HL_REG);
        cpu->reg.B = read_memory(cpu->mem, hl);
        return true; // Instruction Complete
    }
    
    return true;
}
static bool ld_b_a(CPU *cpu)     // 0x47 (- - - -) 1M
{
    cpu->reg.B = cpu->reg.A;
    return true;
}
static bool ld_c_b(CPU *cpu)     // 0x48 (- - - -) 1M
{
    cpu->reg.C = cpu->reg.B;
    return true;
}
static bool ld_c_d(CPU *cpu)     // 0x4A (- - - -) 1M
{
    cpu->reg.C = cpu->reg.D;
    return true;
}
static bool ld_c_e(CPU *cpu)     // 0x4B (- - - -) 1M
{
    cpu->reg.C = cpu->reg.E;
    return true;
}
static bool ld_c_h(CPU *cpu)     // 0x4C (- - - -) 1M
{
    cpu->reg.C = cpu->reg.H;
    return true;
}
static bool ld_c_l(CPU *cpu)     // 0x4D (- - - -) 1M
{
    cpu->reg.C = cpu->reg.L;
    return true;
}
static bool ld_c_hl(CPU *cpu)    // 0x4E (- - - -) 2M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            uint16_t hl = getDR(cpu, HL_REG);
            cpu->reg.C = read_memory(cpu->mem, hl);
            return true;
    }

    return true;
}
static bool ld_c_a(CPU *cpu)     // 0x4F (- - - -) 1M
{
    cpu->reg.C = cpu->reg.A;
    return true;
}

static bool ld_d_b(CPU *cpu)     // 0x50 (- - - -) 1M
{
    cpu->reg.D = cpu->reg.B;
    return true;
}
static bool ld_d_c(CPU *cpu)     // 0x51 (- - - -) 1M
{
    cpu->reg.D = cpu->reg.C;
    return true;
}
static bool ld_d_e(CPU *cpu)     // 0x53 (- - - -) 1M
{
    cpu->reg.D = cpu->reg.E;
    return true;
}
static bool ld_d_h(CPU *cpu)     // 0x54 (- - - -) 1M
{
    cpu->reg.D = cpu->reg.H;
    return true;
}
static bool ld_d_l(CPU *cpu)     // 0x55 (- - - -) 1M
{
    cpu->reg.D = cpu->reg.L;
    return true;
}
static bool ld_d_hl(CPU *cpu)    // 0x56 (- - - -) 2M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            uint16_t hl = getDR(cpu, HL_REG);
            cpu->reg.D = read_memory(cpu->mem, hl);
            return true;
    }

    return true;
}
static bool ld_d_a(CPU *cpu)     // 0x57 (- - - -) 1M
{
    cpu->reg.D = cpu->reg.A;
    return true;
}
static bool ld_e_b(CPU *cpu)     // 0x58 (- - - -) 1M
{
    cpu->reg.E = cpu->reg.B;
    return true;
}
static bool ld_e_c(CPU *cpu)     // 0x59 (- - - -) 1M
{
    cpu->reg.E = cpu->reg.C;
    return true;
}
static bool ld_e_d(CPU *cpu)     // 0x5A (- - - -) 1M
{
    cpu->reg.E = cpu->reg.D;
    return true;
}
static bool ld_e_h(CPU *cpu)     // 0x5C (- - - -) 1M
{
    cpu->reg.E = cpu->reg.H;
    return true;
}
static bool ld_e_l(CPU *cpu)     // 0x5D (- - - -) 1M
{
    cpu->reg.E = cpu->reg.L;
    return true;
}
static bool ld_e_hl(CPU *cpu)    // 0x5E (- - - -) 2M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            uint16_t hl = getDR(cpu, HL_REG);
            cpu->reg.E = read_memory(cpu->mem, hl);
            return true;
    }

    return true;
}
static bool ld_e_a(CPU *cpu)     // 0x5F (- - - -) 1M
{
    cpu->reg.E = cpu->reg.A;
    return true;
}

static bool ld_h_b(CPU *cpu)     // 0x60 (- - - -) 1M
{
    cpu->reg.H = cpu->reg.B;
    return true;
}
static bool ld_h_c(CPU *cpu)     // 0x61 (- - - -) 1M
{
    cpu->reg.H = cpu->reg.C;
    return true;
}
static bool ld_h_d(CPU *cpu)     // 0x62 (- - - -) 1M
{
    cpu->reg.H = cpu->reg.D;
    return true;
}
static bool ld_h_e(CPU *cpu)     // 0x63 (- - - -) 1M
{
    cpu->reg.H = cpu->reg.E;
    return true;
}
static bool ld_h_l(CPU *cpu)     // 0x65 (- - - -) 1M
{
    cpu->reg.H = cpu->reg.L;
    return true;
}
static bool ld_h_hl(CPU *cpu)    // 0x66 (- - - -) 2M
{   
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            uint16_t hl = getDR(cpu, HL_REG);
            cpu->reg.H = read_memory(cpu->mem, hl);
            return true;
    }

    return true;
}
static bool ld_h_a(CPU *cpu)     // 0x67 (- - - -) 1M
{
    cpu->reg.H = cpu->reg.A;
    return true;
}
static bool ld_l_b(CPU *cpu)     // 0x68 (- - - -) 1M
{
    cpu->reg.L = cpu->reg.B;
    return true;
}
static bool ld_l_c(CPU *cpu)     // 0x69 (- - - -) 1M
{
    cpu->reg.L = cpu->reg.C;
    return true;
}
static bool ld_l_d(CPU *cpu)     // 0x6A (- - - -) 1M
{
    cpu->reg.L = cpu->reg.D;
    return true;
}
static bool ld_l_e(CPU *cpu)     // 0x6B (- - - -) 1M
{
    cpu->reg.L = cpu->reg.E;
    return true;
}
static bool ld_l_h(CPU *cpu)     // 0x6C (- - - -) 1M
{
    cpu->reg.L = cpu->reg.H;
    return true;
}
static bool ld_l_hl(CPU *cpu)    // 0x6E (- - - -) 2M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            uint16_t hl = getDR(cpu, HL_REG);
            cpu->reg.L = read_memory(cpu->mem, hl);
            return true;
    }

    return true;
}
static bool ld_l_a(CPU *cpu)     // 0x6F (- - - -) 1M
{ 
    cpu->reg.L = cpu->reg.A;
    return true;
}

static bool ld_a_b(CPU *cpu)     // 0x78 (- - - -) 1M
{
    cpu->reg.A = cpu->reg.B;
    return true;
}
static bool ld_a_c(CPU *cpu)     // 0x79 (- - - -) 1M
{ 
    cpu->reg.A = cpu->reg.C;
    return true;
}
static bool ld_a_d(CPU *cpu)     // 0x7A (- - - -) 1M
{
    cpu->reg.A = cpu->reg.D;
    return true;
}
static bool ld_a_e(CPU *cpu)     // 0x7B (- - - -) 1M
{
    cpu->reg.A = cpu->reg.E;
    return true;
}
static bool ld_a_h(CPU *cpu)     // 0x7C (- - - -) 1M
{
    cpu->reg.A = cpu->reg.H;
    return true;
}
static bool ld_a_l(CPU *cpu)     // 0x7D (- - - -) 1M
{
    cpu->reg.A = cpu->reg.L;
    return true;
}
static bool ld_a_hl(CPU *cpu)    // 0x7E (- - - -) 2M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            uint16_t hl = getDR(cpu, HL_REG);
            cpu->reg.A = read_memory(cpu->mem, hl);
            return true;
    }

    return true;
}
static bool ld_a_bc(CPU *cpu)    // 0x0A (- - - -) 2M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            uint16_t bc = getDR(cpu, BC_REG);
            cpu->reg.A = read_memory(cpu->mem, bc);
            return true;
    }

    return true;
}
static bool ld_a_de(CPU *cpu)    // 0x1A (- - - -) 2M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            uint16_t de = getDR(cpu, DE_REG);
            cpu->reg.A = read_memory(cpu->mem, de);
            return true;
    }

    return true;
}

static bool reg_ld_n_8(CPU *cpu, uint8_t *reg)
{ 
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            *reg = fetch(cpu);
            return true;
    }

    return true;
}
static bool ld_b_n(CPU *cpu)     // 0x06 (- - - -) 2M
{
    return reg_ld_n_8(cpu, &(cpu->reg.B));
}
static bool ld_c_n(CPU *cpu)     // 0x0E (- - - -) 2M
{
    return reg_ld_n_8(cpu, &(cpu->reg.C));
}
static bool ld_d_n(CPU *cpu)     // 0x16 (- - - -) 2M
{
    return reg_ld_n_8(cpu, &(cpu->reg.D));
}
static bool ld_e_n(CPU *cpu)     // 0x1E (- - - -) 2M
{
    return reg_ld_n_8(cpu, &(cpu->reg.E));
}
static bool ld_h_n(CPU *cpu)     // 0x26 (- - - -) 2M
{
    return reg_ld_n_8(cpu, &(cpu->reg.H));
}
static bool ld_l_n(CPU *cpu)     // 0x2E (- - - -) 2M
{
    return reg_ld_n_8(cpu, &(cpu->reg.L));
}
static bool ld_hl_n(CPU *cpu)    // 0x36 (- - - -) 3M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.low = fetch(cpu);
            return false;
        
        case 3:
            uint16_t hl = getDR(cpu, HL_REG);
            write_memory(cpu->mem, hl, cpu->ins.low);
            return true;
    }

    return true;
}
static bool ld_a_n(CPU *cpu)     // 0x3E (- - - -) 2M
{
    return reg_ld_n_8(cpu, &(cpu->reg.A));
}

static uint8_t reg_inc_8(CPU *cpu, uint8_t r)
{
    uint8_t result = r + 1;
    bool   is_zero = (result == 0);
    bool hc_exists = ((r & LOWER_4_MASK) == LOWER_4_MASK);
    set_flag(cpu, is_zero, ZERO_FLAG);
    set_flag(cpu, false, SUBTRACT_FLAG);
    set_flag(cpu, hc_exists, HALF_CARRY_FLAG);
    return result;
}
static bool inc_b(CPU *cpu)      // 0x04 (Z 0 H -) 1M
{
    cpu->reg.B = reg_inc_8(cpu, cpu->reg.B);
    return true;
}
static bool inc_d(CPU *cpu)      // 0x14 (Z 0 H -) 1M
{
    cpu->reg.D = reg_inc_8(cpu, cpu->reg.D);
    return true;
}
static bool inc_h(CPU *cpu)      // 0x24 (Z 0 H -) 1M
{
    cpu->reg.H = reg_inc_8(cpu, cpu->reg.H);
    return true;
}
static bool inc_hl_mem(CPU *cpu) // 0x34 (Z 0 H -) 3M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            uint16_t hl = getDR(cpu, HL_REG);
            cpu->ins.low = read_memory(cpu->mem, hl);
            cpu->ins.address = hl;
            return false;
        
        case 3:
            uint8_t result = reg_inc_8(cpu, cpu->ins.low);
            write_memory(cpu->mem, cpu->ins.address, result);
            return true;
    }

    return true;
}
static bool inc_c(CPU *cpu)      // 0x0C (Z 0 H -) 1M
{
    cpu->reg.C = reg_inc_8(cpu, cpu->reg.C);
    return true;
}
static bool inc_e(CPU *cpu)      // 0x1C (Z 0 H -) 1M
{
    cpu->reg.E = reg_inc_8(cpu, cpu->reg.E);
    return true;
}
static bool inc_l(CPU *cpu)      // 0x2C (Z 0 H -) 1M
{
    cpu->reg.L = reg_inc_8(cpu, cpu->reg.L);
    return true;
}
static bool inc_a(CPU *cpu)      // 0x3C (Z 0 H -) 1M
{
    cpu->reg.A = reg_inc_8(cpu, cpu->reg.A);
    return true;
}

static uint8_t reg_dec_8(CPU *cpu, uint8_t r)
{
    uint8_t result = r - 1;
    bool   is_zero = (result == 0);
    bool hc_exists = ((r & LOWER_4_MASK) == 0);
    set_flag(cpu, is_zero, ZERO_FLAG);
    set_flag(cpu, true, SUBTRACT_FLAG);
    set_flag(cpu, hc_exists, HALF_CARRY_FLAG);
    return result;
}
static bool dec_b(CPU *cpu)      // 0x05 (Z 1 H -) 1M
{
    cpu->reg.B = reg_dec_8(cpu, cpu->reg.B);
    return true;
}
static bool dec_d(CPU *cpu)      // 0x15 (Z 1 H -) 1M
{
    cpu->reg.D = reg_dec_8(cpu, cpu->reg.D);
    return true;
}
static bool dec_h(CPU *cpu)      // 0x25 (Z 1 H -) 1M
{
    cpu->reg.H = reg_dec_8(cpu, cpu->reg.H);
    return true;
}
static bool dec_hl_mem(CPU *cpu) // 0x35 (Z 1 H -) 3M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            uint16_t  hl = getDR(cpu, HL_REG);
            cpu->ins.    low = read_memory(cpu->mem, hl);
            cpu->ins.address = hl;
            return false;
        
        case 3:
            uint8_t result = reg_dec_8(cpu, cpu->ins.low);
            write_memory(cpu->mem, cpu->ins.address, result);
            return true;
    }

    return true;
}
static bool dec_c(CPU *cpu)      // 0x0D (Z 1 H -) 1M
{
    cpu->reg.C = reg_dec_8(cpu, cpu->reg.C);
    return true;
}
static bool dec_e(CPU *cpu)      // 0x1D (Z 1 H -) 1M
{
    cpu->reg.E = reg_dec_8(cpu, cpu->reg.E);
    return true;
}
static bool dec_l(CPU *cpu)      // 0x2D (Z 1 H -) 1M
{
    cpu->reg.L = reg_dec_8(cpu, cpu->reg.L);
    return true;
}
static bool dec_a(CPU *cpu)      // 0x3D (Z 1 H -) 1M
{
    cpu->reg.A = reg_dec_8(cpu, cpu->reg.A);
    return true;
}

static bool rlca(CPU *cpu)       // 0x07 (0 0 0 C) 1M
{
    bool c_exists = (cpu->reg.A & BIT_7_MASK) != 0;
    cpu->reg.A = ((cpu->reg.A >> 7) | (cpu->reg.A << 1));
    set_flag(cpu, false, ZERO_FLAG);
    set_flag(cpu, false, SUBTRACT_FLAG);
    set_flag(cpu, false, HALF_CARRY_FLAG);
    set_flag(cpu, c_exists, CARRY_FLAG);
    return true;
}
static bool rla(CPU *cpu)        // 0x17 (0 0 0 C) 1M
{
    bool c_exists = (cpu->reg.A & BIT_7_MASK) != 0;
    uint8_t carry_in = is_flag_set(cpu, CARRY_FLAG) ? 1 : 0;
    cpu->reg.A = ((cpu->reg.A << 1) | carry_in);
    set_flag(cpu, false, ZERO_FLAG);
    set_flag(cpu, false, SUBTRACT_FLAG);
    set_flag(cpu, false, HALF_CARRY_FLAG);
    set_flag(cpu, c_exists, CARRY_FLAG);
    return true;
}
static bool rrca(CPU *cpu)       // 0x0F (0 0 0 C) 1M
{
    bool c_exists = (cpu->reg.A & BIT_0_MASK) != 0;
    cpu->reg.A = ((cpu->reg.A << 7) | (cpu->reg.A >> 1));
    set_flag(cpu, false, ZERO_FLAG);
    set_flag(cpu, false, SUBTRACT_FLAG);
    set_flag(cpu, false, HALF_CARRY_FLAG);
    set_flag(cpu, c_exists, CARRY_FLAG);
    return true;
}
static bool rra(CPU *cpu)        // 0x1F (0 0 0 C) 1M
{
    bool c_exists = (cpu->reg.A & BIT_0_MASK) != 0;
    uint8_t carry_in = is_flag_set(cpu, CARRY_FLAG) ? 1 : 0;
    cpu->reg.A = ((carry_in << 7) | (cpu->reg.A >> 1));
    set_flag(cpu, false, ZERO_FLAG);
    set_flag(cpu, false, SUBTRACT_FLAG);
    set_flag(cpu, false, HALF_CARRY_FLAG);
    set_flag(cpu, c_exists, CARRY_FLAG);
    return true;
}

static uint16_t reg_add_16(CPU *cpu, uint16_t dest, uint16_t source)
{
    bool hc_exists = ((dest & LOWER_12_MASK) + (source & LOWER_12_MASK)) > LOWER_12_MASK;
    bool  c_exists = (dest + source) > MAX_INT_16;
    set_flag(cpu, false, SUBTRACT_FLAG);
    set_flag(cpu, hc_exists, HALF_CARRY_FLAG);
    set_flag(cpu, c_exists, CARRY_FLAG);
    uint16_t result = dest + source;
    return result;
}
static bool reg_add_16_handler(CPU *cpu, DualRegister source)
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            uint16_t       hl = getDR(cpu, HL_REG);
            uint16_t  operand = getDR(cpu, source);
            uint16_t   result = reg_add_16(cpu, hl, operand);
            setDR(cpu, HL_REG, result);
            return true;
    }

    return true;
}
static bool add_hl_bc(CPU *cpu)  // 0x09 (- 0 H C) 2M
{
    return reg_add_16_handler(cpu, BC_REG);
}
static bool add_hl_de(CPU *cpu)  // 0x19 (- 0 H C) 2M
{
    return reg_add_16_handler(cpu, DE_REG);
}
static bool add_hl_hl(CPU *cpu)  // 0x29 (- 0 H C) 2M
{
    return reg_add_16_handler(cpu, HL_REG);
}
static bool add_hl_sp(CPU *cpu)  // 0x39 (- 0 H C) 2M
{
    return reg_add_16_handler(cpu, SP_REG);
}

static uint8_t reg_add_8(CPU *cpu, uint8_t dest, uint8_t source)
{ // 0x8X
    uint8_t  result = dest + source;
    bool    is_zero = (result == 0);
    bool  hc_exists = ((dest & LOWER_4_MASK) + (source & LOWER_4_MASK)) > LOWER_4_MASK;
    bool   c_exists = (((uint16_t) dest) + ((uint16_t) source)) > LOWER_BYTE_MASK;
    set_flag(cpu, is_zero, ZERO_FLAG);
    set_flag(cpu, false, SUBTRACT_FLAG);
    set_flag(cpu, hc_exists, HALF_CARRY_FLAG);
    set_flag(cpu, c_exists, CARRY_FLAG);
    return (uint8_t) result;
}
static bool add_a_b(CPU *cpu)    // 0x80 (Z 0 H C) 1M
{
    cpu->reg.A = reg_add_8(cpu, cpu->reg.A, cpu->reg.B);
    return true;
}
static bool add_a_c(CPU *cpu)    // 0x81 (Z 0 H C) 1M
{
    cpu->reg.A = reg_add_8(cpu, cpu->reg.A, cpu->reg.C);
    return true;
}
static bool add_a_d(CPU *cpu)    // 0x82 (Z 0 H C) 1M
{
    cpu->reg.A = reg_add_8(cpu, cpu->reg.A, cpu->reg.D);
    return true;
}
static bool add_a_e(CPU *cpu)    // 0x83 (Z 0 H C) 1M
{
    cpu->reg.A = reg_add_8(cpu, cpu->reg.A, cpu->reg.E);
    return true;
}
static bool add_a_h(CPU *cpu)    // 0x84 (Z 0 H C) 1M
{
    cpu->reg.A = reg_add_8(cpu, cpu->reg.A, cpu->reg.H);
    return true;
}
static bool add_a_l(CPU *cpu)    // 0x85 (Z 0 H C) 1M
{
    cpu->reg.A = reg_add_8(cpu, cpu->reg.A, cpu->reg.L);
    return true;
}
static bool add_a_hl(CPU *cpu)   // 0x86 (Z 0 H C) 2M
{ 
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            uint16_t hl = getDR(cpu, HL_REG);
            cpu->reg.A = reg_add_8(cpu, cpu->reg.A, read_memory(cpu->mem, hl));
            return true;
    }

    return true;
}
static bool add_a_a(CPU *cpu)    // 0x87 (Z 0 H C) 1M
{
    cpu->reg.A = reg_add_8(cpu, cpu->reg.A, cpu->reg.A);
    return true;
}
static bool add_a_n(CPU *cpu)    // 0xC6 (Z 0 H C) 2M
{
    switch(cpu->ins.duration)
    {
        case 1:
            cpu->ins.low = fetch(cpu);
            return false;

        case 2:
            cpu->reg.A = reg_add_8(cpu, cpu->reg.A, cpu->ins.low); 
            return true;
    }

    return true;
}

static uint8_t reg_adc_8(CPU *cpu, uint8_t dest, uint8_t source)
{ // 0x8X
    uint8_t  carry = is_flag_set(cpu, CARRY_FLAG) ? 1 : 0;
    uint8_t result = dest + source + carry;
    bool is_zero   = (result  == 0);
    bool hc_exists = ((dest & LOWER_4_MASK) + (source & LOWER_4_MASK) + carry) > LOWER_4_MASK;
    bool c_exists  = (((uint16_t) dest) + ((uint16_t) source) + ((uint16_t) carry)) > LOWER_BYTE_MASK;
    set_flag(cpu, is_zero, ZERO_FLAG);
    set_flag(cpu, false, SUBTRACT_FLAG);
    set_flag(cpu, hc_exists, HALF_CARRY_FLAG);
    set_flag(cpu, c_exists, CARRY_FLAG);
    return (uint8_t) result;
}
static bool adc_a_b(CPU *cpu)    // 0x88 (Z 0 H C) 1M
{
    cpu->reg.A = reg_adc_8(cpu, cpu->reg.A, cpu->reg.B);
    return true;
}
static bool adc_a_c(CPU *cpu)    // 0x89 (Z 0 H C) 1M
{
    cpu->reg.A = reg_adc_8(cpu, cpu->reg.A, cpu->reg.C);
    return true;
}
static bool adc_a_d(CPU *cpu)    // 0x8A (Z 0 H C) 1M
{
    cpu->reg.A = reg_adc_8(cpu, cpu->reg.A, cpu->reg.D);
    return true;
}
static bool adc_a_e(CPU *cpu)    // 0x8B (Z 0 H C) 1M
{
    cpu->reg.A = reg_adc_8(cpu, cpu->reg.A, cpu->reg.E);
    return true;
}
static bool adc_a_h(CPU *cpu)    // 0x8C (Z 0 H C) 1M
{
    cpu->reg.A = reg_adc_8(cpu, cpu->reg.A, cpu->reg.H);
    return true;
}
static bool adc_a_l(CPU *cpu)    // 0x8D (Z 0 H C) 1M
{
    cpu->reg.A = reg_adc_8(cpu, cpu->reg.A, cpu->reg.L);
    return true;
}
static bool adc_a_hl(CPU *cpu)   // 0x8E (Z 0 H C) 2M
{   
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            uint16_t hl = getDR(cpu, HL_REG);
            cpu->reg.A = reg_adc_8(cpu, cpu->reg.A, read_memory(cpu->mem, hl));
            return true;
    }

    return true;
}
static bool adc_a_a(CPU *cpu)    // 0x8F (Z 0 H C) 1M
{
    cpu->reg.A = reg_adc_8(cpu, cpu->reg.A, cpu->reg.A);
    return true;
}
static bool adc_a_n(CPU *cpu)    // 0xCE (Z 0 H C) 2M
{
    switch(cpu->ins.duration)
    {
        case 1:
            cpu->ins.low = fetch(cpu);
            return false;

        case 2:
            cpu->reg.A = reg_adc_8(cpu, cpu->reg.A, cpu->ins.low);
            return true;
    }

    return true;
}

static uint8_t reg_sub_8(CPU *cpu, uint8_t dest, uint8_t source)
{
    uint8_t result = (dest - source);
    bool    is_zero = (result == 0);
    bool  hc_exists = ((dest & LOWER_4_MASK) < (source & LOWER_4_MASK));
    bool   c_exists = dest < source;
    set_flag(cpu, is_zero, ZERO_FLAG);
    set_flag(cpu, true, SUBTRACT_FLAG);
    set_flag(cpu, hc_exists, HALF_CARRY_FLAG);
    set_flag(cpu, c_exists, CARRY_FLAG);
    return (uint8_t) result;
}
static bool sub_a_b(CPU *cpu)    // 0x90 (Z 1 H C) 1M
{
    cpu->reg.A = reg_sub_8(cpu, cpu->reg.A, cpu->reg.B);
    return true;
}
static bool sub_a_c(CPU *cpu)    // 0x91 (Z 1 H C) 1M
{
    cpu->reg.A = reg_sub_8(cpu, cpu->reg.A, cpu->reg.C);
    return true;
}
static bool sub_a_d(CPU *cpu)    // 0x92 (Z 1 H C) 1M
{
    cpu->reg.A = reg_sub_8(cpu, cpu->reg.A, cpu->reg.D);
    return true;
}
static bool sub_a_e(CPU *cpu)    // 0x93 (Z 1 H C) 1M
{
    cpu->reg.A = reg_sub_8(cpu, cpu->reg.A, cpu->reg.E);
    return true;
}
static bool sub_a_h(CPU *cpu)    // 0x94 (Z 1 H C) 1M
{
    cpu->reg.A = reg_sub_8(cpu, cpu->reg.A, cpu->reg.H);
    return true;
}
static bool sub_a_l(CPU *cpu)    // 0x95 (Z 1 H C) 1M
{
    cpu->reg.A = reg_sub_8(cpu, cpu->reg.A, cpu->reg.L);
    return true;
}
static bool sub_a_hl(CPU *cpu)   // 0x96 (Z 1 H C) 2M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            uint16_t hl = getDR(cpu, HL_REG);
            cpu->reg.A = reg_sub_8(cpu, cpu->reg.A, read_memory(cpu->mem, hl));
            return true;
    }
    
    return true;
}
static bool sub_a_a(CPU *cpu)    // 0x97 (Z 1 H C) 1M
{
    cpu->reg.A = reg_sub_8(cpu, cpu->reg.A, cpu->reg.A);
    return true;
}
static bool sub_a_n(CPU *cpu)    // 0xD6 (Z 1 H C) 2M
{
    switch(cpu->ins.duration)
    {
        case 1:
            cpu->ins.low = fetch(cpu);
            return false;

        case 2:
            cpu->reg.A = reg_sub_8(cpu, cpu->reg.A, cpu->ins.low);
            return true;
    }
    
    return true;
}

static uint8_t reg_sbc_8(CPU *cpu, uint8_t dest, uint8_t source)
{
    uint8_t   carry = is_flag_set(cpu, CARRY_FLAG) ? 1 : 0;
    uint8_t  result = (dest - source - carry);
    bool    is_zero = (result == 0);
    bool  hc_exists = ((dest & LOWER_4_MASK) < ((source & LOWER_4_MASK) + carry));
    bool   c_exists = dest < (source + carry);
    set_flag(cpu, is_zero, ZERO_FLAG);
    set_flag(cpu, true, SUBTRACT_FLAG);
    set_flag(cpu, hc_exists, HALF_CARRY_FLAG);
    set_flag(cpu, c_exists, CARRY_FLAG);
    return (uint8_t) result;
}
static bool sbc_a_b(CPU *cpu)    // 0x98 (Z 1 H C) 1M
{
    cpu->reg.A = reg_sbc_8(cpu, cpu->reg.A, cpu->reg.B);
    return true;
}
static bool sbc_a_c(CPU *cpu)    // 0x99 (Z 1 H C) 1M
{
    cpu->reg.A = reg_sbc_8(cpu, cpu->reg.A, cpu->reg.C);
    return true;
}
static bool sbc_a_d(CPU *cpu)    // 0x9A (Z 1 H C) 1M
{
    cpu->reg.A = reg_sbc_8(cpu, cpu->reg.A, cpu->reg.D);
    return true;
}
static bool sbc_a_e(CPU *cpu)    // 0x9B (Z 1 H C) 1M
{
    cpu->reg.A = reg_sbc_8(cpu, cpu->reg.A, cpu->reg.E);
    return true;
}
static bool sbc_a_h(CPU *cpu)    // 0x9C (Z 1 H C) 1M
{
    cpu->reg.A = reg_sbc_8(cpu, cpu->reg.A, cpu->reg.H);
    return true;
}
static bool sbc_a_l(CPU *cpu)    // 0x9D (Z 1 H C) 1M
{
    cpu->reg.A = reg_sbc_8(cpu, cpu->reg.A, cpu->reg.L);
    return true;
}
static bool sbc_a_hl(CPU *cpu)   // 0x9E (Z 1 H C) 2M
{ 
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            uint16_t hl = getDR(cpu, HL_REG);
            cpu->reg.A = reg_sbc_8(cpu, cpu->reg.A, read_memory(cpu->mem, hl));
            return true;
    }

    return true;
}
static bool sbc_a_a(CPU *cpu)    // 0x9F (Z 1 H C) 1M
{
    cpu->reg.A = reg_sbc_8(cpu, cpu->reg.A, cpu->reg.A);
    return true;
}
static bool sbc_a_n(CPU *cpu)    // 0xDE (Z 1 H C) 2M
{
    switch(cpu->ins.duration)
    {
        case 1:
            cpu->ins.low = fetch(cpu);
            return false;

        case 2:
            cpu->reg.A = reg_sbc_8(cpu, cpu->reg.A, cpu->ins.low);
            return true;
    }
    
    return true;
}

static uint8_t reg_and_8(CPU *cpu, uint8_t dest, uint8_t source)
{
    uint8_t result = dest & source;
    bool   is_zero = (result == 0);
    set_flag(cpu, is_zero, ZERO_FLAG);
    set_flag(cpu, false, SUBTRACT_FLAG);
    set_flag(cpu, true, HALF_CARRY_FLAG);
    set_flag(cpu, false, CARRY_FLAG);
    return result;
}
static bool and_a_b(CPU *cpu)    // 0xA0 (Z 0 1 0) 1M
{
    cpu->reg.A = reg_and_8(cpu, cpu->reg.A, cpu->reg.B);
    return true;
}
static bool and_a_c(CPU *cpu)    // 0xA1 (Z 0 1 0) 1M
{
    cpu->reg.A = reg_and_8(cpu, cpu->reg.A, cpu->reg.C);
    return true;
}
static bool and_a_d(CPU *cpu)    // 0xA2 (Z 0 1 0) 1M
{
    cpu->reg.A = reg_and_8(cpu, cpu->reg.A, cpu->reg.D);
    return true;
}
static bool and_a_e(CPU *cpu)    // 0xA3 (Z 0 1 0) 1M
{
    cpu->reg.A = reg_and_8(cpu, cpu->reg.A, cpu->reg.E);
    return true; 
}
static bool and_a_h(CPU *cpu)    // 0xA4 (Z 0 1 0) 1M
{
    cpu->reg.A = reg_and_8(cpu, cpu->reg.A, cpu->reg.H);
    return true; 
}
static bool and_a_l(CPU *cpu)    // 0xA5 (Z 0 1 0) 1M
{
    cpu->reg.A = reg_and_8(cpu, cpu->reg.A, cpu->reg.L);
    return true;
}
static bool and_a_hl(CPU *cpu)   // 0xA6 (Z 0 1 0) 2M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            uint16_t hl = getDR(cpu, HL_REG);
            cpu->reg.A = reg_and_8(cpu, cpu->reg.A, read_memory(cpu->mem, hl));
            return true;
    }
    
    return true;
}
static bool and_a_a(CPU *cpu)    // 0xA7 (Z 0 1 0) 1M
{
    cpu->reg.A = reg_and_8(cpu, cpu->reg.A, cpu->reg.A);
    return true; 
}
static bool and_a_n(CPU *cpu)    // 0xE6 (Z 0 1 0) 2M
{
    switch(cpu->ins.duration)
    {
        case 1:
            cpu->ins.low = fetch(cpu);
            return false;

        case 2:
            cpu->reg.A = reg_and_8(cpu, cpu->reg.A, cpu->ins.low);
            return true;
    }

    return true;
}

static uint8_t reg_xor_8(CPU *cpu, uint8_t dest, uint8_t source)
{
    uint8_t result = dest ^ source;
    bool   is_zero = (result == 0);
    set_flag(cpu, is_zero, ZERO_FLAG);
    set_flag(cpu, false, SUBTRACT_FLAG);
    set_flag(cpu, false, HALF_CARRY_FLAG);
    set_flag(cpu, false, CARRY_FLAG);
    return result;
}
static bool xor_a_b(CPU *cpu)    // 0xA8 (Z 0 0 0) 1M
{
    cpu->reg.A = reg_xor_8(cpu, cpu->reg.A, cpu->reg.B);
    return true;
}
static bool xor_a_c(CPU *cpu)    // 0xA9 (Z 0 0 0) 1M
{
    cpu->reg.A = reg_xor_8(cpu, cpu->reg.A, cpu->reg.C);
    return true;
}
static bool xor_a_d(CPU *cpu)    // 0xAA (Z 0 0 0) 1M
{
    cpu->reg.A = reg_xor_8(cpu, cpu->reg.A, cpu->reg.D);
    return true;
}
static bool xor_a_e(CPU *cpu)    // 0xAB (Z 0 0 0) 1M
{
    cpu->reg.A = reg_xor_8(cpu, cpu->reg.A, cpu->reg.E);
    return true;
}
static bool xor_a_h(CPU *cpu)    // 0xAC (Z 0 0 0) 1M
{
    cpu->reg.A = reg_xor_8(cpu, cpu->reg.A, cpu->reg.H);
    return true;
}
static bool xor_a_l(CPU *cpu)    // 0xAD (Z 0 0 0) 1M
{
    cpu->reg.A = reg_xor_8(cpu, cpu->reg.A, cpu->reg.L);
    return true;
}
static bool xor_a_hl(CPU *cpu)   // 0xAE (Z 0 0 0) 2M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            uint16_t hl = getDR(cpu, HL_REG);
            cpu->reg.A = reg_xor_8(cpu, cpu->reg.A, read_memory(cpu->mem, hl));
            return true;
    }
    
    return true;
}
static bool xor_a_a(CPU *cpu)    // 0xAF (Z 0 0 0) 1M
{
    cpu->reg.A = reg_xor_8(cpu, cpu->reg.A, cpu->reg.A);
    return true;
}
static bool xor_a_n(CPU *cpu)    // 0xEE (Z 0 0 0) 2M
{  
    switch(cpu->ins.duration)
    {
        case 1:
            cpu->ins.low = fetch(cpu);
            return false;

        case 2:
            cpu->reg.A = reg_xor_8(cpu, cpu->reg.A, cpu->ins.low);
            return true;
    }

    return true;
}

static uint8_t reg_or_8(CPU *cpu, uint8_t dest, uint8_t source)
{
    uint8_t result = dest | source;
    bool   is_zero = (result == 0);
    set_flag(cpu, is_zero, ZERO_FLAG);
    set_flag(cpu, false, SUBTRACT_FLAG);
    set_flag(cpu, false, HALF_CARRY_FLAG);
    set_flag(cpu, false, CARRY_FLAG);
    return result;
}
static bool or_a_b(CPU *cpu)     // 0xB0 (Z 0 0 0) 1M
{
    cpu->reg.A = reg_or_8(cpu, cpu->reg.A, cpu->reg.B);
    return true;
}
static bool or_a_c(CPU *cpu)     // 0xB1 (Z 0 0 0) 1M
{
    cpu->reg.A = reg_or_8(cpu, cpu->reg.A, cpu->reg.C);
    return true;
}
static bool or_a_d(CPU *cpu)     // 0xB2 (Z 0 0 0) 1M
{
    cpu->reg.A = reg_or_8(cpu, cpu->reg.A, cpu->reg.D);
    return true;
}
static bool or_a_e(CPU *cpu)     // 0xB3 (Z 0 0 0) 1M
{
    cpu->reg.A = reg_or_8(cpu, cpu->reg.A, cpu->reg.E);
    return true;
}
static bool or_a_h(CPU *cpu)     // 0xB4 (Z 0 0 0) 1M
{
    cpu->reg.A = reg_or_8(cpu, cpu->reg.A, cpu->reg.H);
    return true;
}
static bool or_a_l(CPU *cpu)     // 0xB5 (Z 0 0 0) 1M
{
    cpu->reg.A = reg_or_8(cpu, cpu->reg.A, cpu->reg.L);
    return true;
}
static bool or_a_hl(CPU *cpu)    // 0xB6 (Z 0 0 0) 2M
{   
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            uint16_t hl = getDR(cpu, HL_REG);
            cpu->reg.A = reg_or_8(cpu, cpu->reg.A, read_memory(cpu->mem, hl));
            return true;
    }

    return true;
}
static bool or_a_a(CPU *cpu)     // 0xB7 (Z 0 0 0) 1M
{
    cpu->reg.A = reg_or_8(cpu, cpu->reg.A, cpu->reg.A);
    return true;
}
static bool or_a_n(CPU *cpu)     // 0xF6 (Z 0 0 0) 2M
{
    switch(cpu->ins.duration)
    {
        case 1:
            cpu->ins.low = fetch(cpu);
            return false;

        case 2:
            cpu->reg.A = reg_or_8(cpu, cpu->reg.A, cpu->ins.low);
            return true;
    }

    return true;
}

static void reg_cp_8(CPU *cpu, uint8_t dest, uint8_t source)
{
    bool    is_zero = (dest == source); 
    bool  hc_exists = ((dest & LOWER_4_MASK) < (source & LOWER_4_MASK));
    bool   c_exists = dest < source;
    set_flag(cpu, is_zero, ZERO_FLAG);
    set_flag(cpu, true, SUBTRACT_FLAG);
    set_flag(cpu, hc_exists, HALF_CARRY_FLAG);
    set_flag(cpu, c_exists, CARRY_FLAG);
}
static bool cp_a_b(CPU *cpu)     // 0xB8 (Z 1 H C) 1M
{
    reg_cp_8(cpu, cpu->reg.A, cpu->reg.B);
    return true;
}
static bool cp_a_c(CPU *cpu)     // 0xB9 (Z 1 H C) 1M
{
    reg_cp_8(cpu, cpu->reg.A, cpu->reg.C);
    return true;
}
static bool cp_a_d(CPU *cpu)     // 0xBA (Z 1 H C) 1M
{
    reg_cp_8(cpu, cpu->reg.A, cpu->reg.D);
    return true;
}
static bool cp_a_e(CPU *cpu)     // 0xBB (Z 1 H C) 1M
{
    reg_cp_8(cpu, cpu->reg.A, cpu->reg.E);
    return true;
}
static bool cp_a_h(CPU *cpu)     // 0xBC (Z 1 H C) 1M
{
    reg_cp_8(cpu, cpu->reg.A, cpu->reg.H);
    return true;
}
static bool cp_a_l(CPU *cpu)     // 0xBD (Z 1 H C) 1M
{
    reg_cp_8(cpu, cpu->reg.A, cpu->reg.L);
    return true;
}
static bool cp_a_hl(CPU *cpu)    // 0xBE (Z 1 H C) 2M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            uint16_t hl = getDR(cpu, HL_REG);
            reg_cp_8(cpu, cpu->reg.A, read_memory(cpu->mem, hl));
            return true;
    }
    
    return true;
}
static bool cp_a_a(CPU *cpu)     // 0xBF (Z 1 H C) 1M
{
    reg_cp_8(cpu, cpu->reg.A, cpu->reg.A);
    return true;
}
static bool cp_a_n(CPU *cpu)     // 0xFE (Z 1 H C) 2M
{  
    switch(cpu->ins.duration)
    {
        case 1:
            cpu->ins.low = fetch(cpu);
            return false;

        case 2:
            reg_cp_8(cpu, cpu->reg.A, cpu->ins.low);
            return true;
    }

    return true;
}

static bool return_handler(CPU *cpu, bool returning)
{
    switch(cpu->ins.duration)
    {
        case 1:

            return false;

        case 2:
            if (!returning)
            {
                return true;
            }
            return false;
        
        case 3:
            cpu->ins.low = pop_stack(cpu);
            return false;

        case 4:
            cpu->ins.high = pop_stack(cpu);
            return false;
        
        case 5:
            cpu->reg.PC = form_address(cpu);
            return true; // Instruction Complete
    }

    return true;
}
static bool ret_nz(CPU *cpu)     // 0xC0 (- - - -) 5M
{
    return return_handler(cpu, !is_flag_set(cpu, ZERO_FLAG));
}
static bool ret_nc(CPU *cpu)     // 0xD0 (- - - -) 5M
{
    return return_handler(cpu, !is_flag_set(cpu, CARRY_FLAG));
}
static bool ret_c(CPU *cpu)      // 0xD8 (- - - -) 5M
{
    return return_handler(cpu, is_flag_set(cpu, CARRY_FLAG));
}
static bool ret_z(CPU *cpu)      // 0xC8 (- - - -) 5M
{
    return return_handler(cpu, is_flag_set(cpu, ZERO_FLAG));
}
static bool ret(CPU *cpu)        // 0xC9 (- - - -) 4M
{
    if (cpu->ins.duration == 2) cpu->ins.duration += 1; // Skips check cycle
    return return_handler(cpu, true);
}
static bool reti(CPU *cpu)       // 0xD9 (- - - -) 4M
{
    if (cpu->ins.duration == 2) cpu->ins.duration += 1; // Skips check cycle
    bool result = return_handler(cpu, true);

    if (cpu->ins.duration == 4)
    {
        cpu->ime = true;
    }

    return result;
}

static bool rst_handler(CPU *cpu, uint16_t rst_vector)
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            return false;
        
        case 3:
            push_stack(cpu, ((cpu->reg.PC >> BYTE) & LOWER_BYTE_MASK));
            return false;

        case 4:
            push_stack(cpu, (cpu->reg.PC & LOWER_BYTE_MASK));
            cpu->reg.PC = rst_vector;
            return true;
    }
    
    return true;
}
static bool rst_00(CPU *cpu)     // 0xC7 (- - - -) 4M
{
    return rst_handler(cpu, 0x00);
}
static bool rst_10(CPU *cpu)     // 0xD7 (- - - -) 4M
{
    return rst_handler(cpu, 0x10);
}
static bool rst_20(CPU *cpu)     // 0xE7 (- - - -) 4M
{
    return rst_handler(cpu, 0x20);
}
static bool rst_30(CPU *cpu)     // 0xF7 (- - - -) 4M
{
    return rst_handler(cpu, 0x30);
}
static bool rst_08(CPU *cpu)     // 0xCF (- - - -) 4M
{
    return rst_handler(cpu, 0x08);
}
static bool rst_18(CPU *cpu)     // 0xDF (- - - -) 4M
{ 
    return rst_handler(cpu, 0x18);
}
static bool rst_28(CPU *cpu)     // 0xEF (- - - -) 4M
{
    return rst_handler(cpu, 0x28);
}
static bool rst_38(CPU *cpu)     // 0xFF (- - - -) 4M
{
    return rst_handler(cpu, 0x38);
}

static bool call_handler(CPU *cpu, bool calling)
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.low = fetch(cpu);
            return false;
        
        case 3:
            cpu->ins.high = fetch(cpu);
            if (!calling)
            {
                return true;
            }
            return false;

        case 4:
            return false;
        
        case 5:
            push_stack(cpu, ((cpu->reg.PC >> BYTE) & LOWER_BYTE_MASK));
            return false;

        case 6:
            push_stack(cpu, (cpu->reg.PC & LOWER_BYTE_MASK));
            cpu->reg.PC = form_address(cpu);
            return true;
    }

    return true;
}
static bool call_nz_nn(CPU *cpu) // 0xC4 (- - - -) 6M
{
    return call_handler(cpu, !is_flag_set(cpu, ZERO_FLAG));
}
static bool call_nc_nn(CPU *cpu) // 0xD4 (- - - -) 6M
{
    return call_handler(cpu, !is_flag_set(cpu, CARRY_FLAG));
}
static bool call_z_nn(CPU *cpu)  // 0xCC (- - - -) 6M
{
    return call_handler(cpu, is_flag_set(cpu, ZERO_FLAG));
}
static bool call_c_nn(CPU *cpu)  // 0xDC (- - - -) 6M
{
    return call_handler(cpu, is_flag_set(cpu, CARRY_FLAG));
}
static bool call_nn(CPU *cpu)    // 0xCD (- - - -) 6M
{
    return call_handler(cpu, true);
}

static bool jump_relative_handler(CPU *cpu, bool jumping)
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.low = fetch(cpu);
            if (!jumping)
            {
                return true;
            }
            return false;
        
        case 3:
            int8_t offset = (int8_t) cpu->ins.low;
            cpu->reg.PC += offset;
            return true;
    }
    
    return true;
}
static bool jr_n(CPU *cpu)       // 0x18 (- - - -) 3M
{
    return jump_relative_handler(cpu, true);
}
static bool jr_z_n(CPU *cpu)     // 0x28 (- - - -) 3M
{
    return jump_relative_handler(cpu, is_flag_set(cpu, ZERO_FLAG));
}
static bool jr_c_n(CPU *cpu)     // 0x38 (- - - -) 3M
{
    return jump_relative_handler(cpu, is_flag_set(cpu, CARRY_FLAG));
}
static bool jr_nz_n(CPU *cpu)    // 0x20 (- - - -) 3M
{
    return jump_relative_handler(cpu, !is_flag_set(cpu, ZERO_FLAG));
}
static bool jr_nc_n(CPU *cpu)    // 0x30 (- - - -) 3M
{
    return jump_relative_handler(cpu, !is_flag_set(cpu, CARRY_FLAG));
}

static bool jump_position_handler(CPU *cpu, bool jumping)
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.low = fetch(cpu);
            return false;
        
        case 3:
            cpu->ins.high = fetch(cpu);
            if (!jumping)
            {
                return true;
            }
            return false;

        case 4:
            cpu->reg.PC = form_address(cpu);
            return true;
    }
    
    return true;
}
static bool jp_nz_nn(CPU *cpu)   // 0xC2 (- - - -) 4M
{
    return jump_position_handler(cpu, !is_flag_set(cpu, ZERO_FLAG));
}
static bool jp_nc_nn(CPU *cpu)   // 0xD2 (- - - -) 4M
{
    return jump_position_handler(cpu, !is_flag_set(cpu, CARRY_FLAG));
}
static bool jp_nn(CPU *cpu)      // 0xC3 (- - - -) 4M
{
    return jump_position_handler(cpu, true);
}
static bool jp_z_nn(CPU *cpu)    // 0xCA (- - - -) 4M
{
    return jump_position_handler(cpu, is_flag_set(cpu, ZERO_FLAG));
}
static bool jp_c_nn(CPU *cpu)    // 0xDA (- - - -) 4M
{
    return jump_position_handler(cpu, is_flag_set(cpu, CARRY_FLAG));
}
static bool jp_hl(CPU *cpu)      // 0xE9 (- - - -) 1M
{
    cpu->reg.PC = getDR(cpu, HL_REG);
    return true;
}

static bool daa(CPU *cpu)        // 0x27 (Z - 0 C) 1M
{
    uint8_t correction = 0;
    bool carry = is_flag_set(cpu, CARRY_FLAG);

    if (!is_flag_set(cpu, SUBTRACT_FLAG)) 
    {
        if (is_flag_set(cpu, HALF_CARRY_FLAG) || ((cpu->reg.A & 0x0F) > 9)) correction |= 0x06;
        if (carry || cpu->reg.A > 0x99) 
        {
            correction |= 0x60;
            set_flag(cpu, true, CARRY_FLAG);
        } 
        else 
        {
            set_flag(cpu, false, CARRY_FLAG);
        }
        cpu->reg.A += correction;
    } 
    else 
    {
        if (is_flag_set(cpu, HALF_CARRY_FLAG)) correction |= 0x06;
        if (carry)                        correction |= 0x60;
        cpu->reg.A -= correction;
    }

    set_flag(cpu, (cpu->reg.A == 0), ZERO_FLAG);
    set_flag(cpu, false, HALF_CARRY_FLAG);
    return true;
}

static bool cpl(CPU *cpu)        // 0x2F (- 1 1 -) 1M
{
    cpu->reg.A = ~cpu->reg.A;
    set_flag(cpu, true, SUBTRACT_FLAG);
    set_flag(cpu, true, HALF_CARRY_FLAG);
    return true;
}
static bool scf(CPU *cpu)        // 0x07 (- 0 0 1) 1M
{
    set_flag(cpu, false, SUBTRACT_FLAG);
    set_flag(cpu, false, HALF_CARRY_FLAG);
    set_flag(cpu, true, CARRY_FLAG);
    return true;
}
static bool ccf(CPU *cpu)        // 0x3F (- 0 0 C) 1M
{
    set_flag(cpu, false, SUBTRACT_FLAG);
    set_flag(cpu, false, HALF_CARRY_FLAG);
    set_flag(cpu, !is_flag_set(cpu, CARRY_FLAG), CARRY_FLAG);
    return true;
}

static bool ldh_n_a(CPU *cpu)    // 0xE0 (- - - -) 3M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.address = (0xFF00 | fetch(cpu));
            return false;
        
        case 3:
            write_memory(cpu->mem, cpu->ins.address, cpu->reg.A);
            return true;
    }
    
    return true;
}
static bool ldh_a_n(CPU *cpu)    // 0xF0 (- - - -) 3M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.address = (0xFF00 | fetch(cpu));
            return false;
        
        case 3:
            cpu->reg.A = read_memory(cpu->mem, cpu->ins.address);
            return true;
    }
    
    return true;
}
static bool ldh_c_a(CPU *cpu)    // 0xE1 (- - - -) 2M
{
    switch(cpu->ins.duration)
    {
        case 1:
            cpu->ins.address = (0xFF00 | cpu->reg.C);
            return false;

        case 2:
            write_memory(cpu->mem, cpu->ins.address, cpu->reg.A);
            return true;
    }

    return true;
}
static bool ldh_a_c(CPU *cpu)    // 0xF1 (- - - -) 2M
{ 
    switch(cpu->ins.duration)
    {
        case 1:
            cpu->ins.address = (0xFF00 | cpu->reg.C);
            return false;

        case 2:
            cpu->reg.A = read_memory(cpu->mem, cpu->ins.address);
            return true;
    }

    return true;
}

static bool ld_nn_a(CPU *cpu)    // 0xEA (- - - -) 4M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.low = fetch(cpu);
            return false;
        
        case 3:
            cpu->ins.high = fetch(cpu);
            return false;

        case 4:
            cpu->ins.address = form_address(cpu);
            write_memory(cpu->mem, cpu->ins.address, cpu->reg.A);
            return true;
    }
    
    return true;
}
static bool ld_a_nn(CPU *cpu)    // 0xFA (- - - -) 4M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.low = fetch(cpu);
            return false;
        
        case 3:
            cpu->ins.high = fetch(cpu);
            return false;

        case 4:
            cpu->ins.address = form_address(cpu);
            cpu->reg.A = read_memory(cpu->mem, cpu->ins.address);
            return true;
    }
    
    return true;
}
static bool ld_hl_sp_n(CPU *cpu) // 0xF8 (0 0 H C) 3M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.low = fetch(cpu);
            return false;
        
        case 3:
            int8_t        n = (int8_t) cpu->ins.low;
            uint16_t result = cpu->reg.SP + n;
            bool hc_exists = ((cpu->reg.SP & LOWER_4_MASK) + (((uint8_t) n) & LOWER_4_MASK)) > LOWER_4_MASK;
            bool c_exists  = ((cpu->reg.SP & 0xFF) + (((uint8_t) n) & 0xFF)) > 0xFF;
            set_flag(cpu, false, ZERO_FLAG);
            set_flag(cpu, false, SUBTRACT_FLAG);
            set_flag(cpu, hc_exists, HALF_CARRY_FLAG);
            set_flag(cpu, c_exists, CARRY_FLAG);
            setDR(cpu, HL_REG, result);
            return true;
    }
    
    return true;
}
static bool ld_sp_hl(CPU *cpu)   // 0xF9 (- - - -) 2M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->reg.SP = getDR(cpu, HL_REG);
            return true;
    }

    return true;
}
static bool add_sp_n(CPU *cpu)   // 0xE8 (0 0 H C) 4M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.low = fetch(cpu);
            return false;
        
        case 3:
            return false;

        case 4:
            int8_t        n = (int8_t) cpu->ins.low;
            uint16_t    sum = (cpu->reg.SP + n);
            bool  hc_exists = ((cpu->reg.SP & LOWER_4_MASK) + (((uint8_t) n) & LOWER_4_MASK)) > LOWER_4_MASK;
            bool  c_exists  = ((cpu->reg.SP & 0xFF) + (((uint8_t) n) & 0xFF)) > 0xFF;
            set_flag(cpu, false, ZERO_FLAG);
            set_flag(cpu, false, SUBTRACT_FLAG);
            set_flag(cpu, hc_exists, HALF_CARRY_FLAG);
            set_flag(cpu, c_exists, CARRY_FLAG);
            cpu->reg.SP = sum;
            return true;
    }
    
    return true;
}

static bool di(CPU *cpu)         // 0xF3 (- - - -) 1M
{
    cpu->          ime = false;
    cpu->ime_scheduled = false;
    return true;
}
static bool ei(CPU *cpu)         // 0xFB (- - - -) 1M
{
    if (cpu->ime || cpu->ime_scheduled) return true;

    cpu->ime_scheduled = true;
    cpu->    ime_delay = 2;
    return true;
}
static bool cb_prefix(CPU *cpu)  // 0xCB (- - - -) 1M
{
    cpu->ins.cb_prefixed = true;
    return true;
}

static bool int_exec(CPU *cpu)   // 0xXX (- - - -) 5M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            return false;
        
        case 3:
            uint8_t high = ((cpu->reg.PC >> BYTE) & LOWER_BYTE_MASK);
            push_stack(cpu, high);

            uint8_t pending = get_pending_interrupts(cpu);

            if (pending == 0) // Cancel Interrupt
            {
                cpu->reg.PC = 0x0000;
                return true;
            }

            if ((pending & cpu->ins.low) == 0) // Shift Interrupt
            {
                encode_interrupt(cpu, pending);
                cpu->ins.duration = 3;
            }

            return false;

        case 4:
            uint8_t low = (cpu->reg.PC & LOWER_BYTE_MASK);
            push_stack(cpu, low);
            return false;
        
        case 5:
            cpu->reg.PC = cpu->ins.address;    // Set when servicing
            uint8_t byte = (*cpu->reg.IFR & ~cpu->ins.low);
            write_memory(cpu->mem, IFR, byte);
            return true;
    }
    
    return true;
}

const OpcodeHandler opcode_table[256] = 
{
    /*              ROW 1               */
    [0x00] = nop,   
    [0x01] = ld_bc_nn,
    [0x02] = ld_bc_a,
    [0x03] = inc_bc,
    [0x04] = inc_b,
    [0x05] = dec_b,
    [0x06] = ld_b_n,
    [0x07] = rlca,
    [0x08] = ld_nn_sp,
    [0x09] = add_hl_bc,
    [0x0A] = ld_a_bc,
    [0x0B] = dec_bc,
    [0x0C] = inc_c,
    [0x0D] = dec_c,
    [0x0E] = ld_c_n,
    [0x0F] = rrca,
    /*              ROW 2               */
    [0x10] = stop,
    [0x11] = ld_de_nn,
    [0x12] = ld_de_a,
    [0x13] = inc_de,
    [0x14] = inc_d,
    [0x15] = dec_d,
    [0x16] = ld_d_n,
    [0x17] = rla,
    [0x18] = jr_n,
    [0x19] = add_hl_de,
    [0x1A] = ld_a_de,
    [0x1B] = dec_de,
    [0x1C] = inc_e,
    [0x1D] = dec_e,
    [0x1E] = ld_e_n,
    [0x1F] = rra,
    /*              ROW 3               */
    [0x20] = jr_nz_n,
    [0x21] = ld_hl_nn,
    [0x22] = ld_hli_a,
    [0x23] = inc_hl,
    [0x24] = inc_h,
    [0x25] = dec_h,
    [0x26] = ld_h_n,
    [0x27] = daa,
    [0x28] = jr_z_n,
    [0x29] = add_hl_hl,
    [0x2A] = ld_a_hli,
    [0x2B] = dec_hl,
    [0x2C] = inc_l,
    [0x2D] = dec_l,
    [0x2E] = ld_l_n,
    [0x2F] = cpl,
    /*              ROW 4               */
    [0x30] = jr_nc_n,
    [0x31] = ld_sp_nn,
    [0x32] = ld_hld_a,
    [0x33] = inc_sp,
    [0x34] = inc_hl_mem,
    [0x35] = dec_hl_mem,
    [0x36] = ld_hl_n,
    [0x37] = scf,
    [0x38] = jr_c_n,
    [0x39] = add_hl_sp,
    [0x3A] = ld_a_hld,
    [0x3B] = dec_sp,
    [0x3C] = inc_a,
    [0x3D] = dec_a,
    [0x3E] = ld_a_n,
    [0x3F] = ccf,
    /*              ROW 5               */
    [0x40] = nop,   
    [0x41] = ld_b_c,
    [0x42] = ld_b_d,
    [0x43] = ld_b_e,
    [0x44] = ld_b_h,
    [0x45] = ld_b_l,
    [0x46] = ld_b_hl,
    [0x47] = ld_b_a,
    [0x48] = ld_c_b,
    [0x49] = nop,
    [0x4A] = ld_c_d,
    [0x4B] = ld_c_e,
    [0x4C] = ld_c_h,
    [0x4D] = ld_c_l,
    [0x4E] = ld_c_hl,
    [0x4F] = ld_c_a,
    /*              ROW 6               */
    [0x50] = ld_d_b,
    [0x51] = ld_d_c,
    [0x52] = nop,
    [0x53] = ld_d_e,
    [0x54] = ld_d_h,
    [0x55] = ld_d_l,
    [0x56] = ld_d_hl,
    [0x57] = ld_d_a,
    [0x58] = ld_e_b,
    [0x59] = ld_e_c,
    [0x5A] = ld_e_d,
    [0x5B] = nop,
    [0x5C] = ld_e_h,
    [0x5D] = ld_e_l,
    [0x5E] = ld_e_hl,
    [0x5F] = ld_e_a,
    /*              ROW 7               */
    [0x60] = ld_h_b,
    [0x61] = ld_h_c,
    [0x62] = ld_h_d,
    [0x63] = ld_h_e,
    [0x64] = nop,
    [0x65] = ld_h_l,
    [0x66] = ld_h_hl,
    [0x67] = ld_h_a,
    [0x68] = ld_l_b,
    [0x69] = ld_l_c,
    [0x6A] = ld_l_d,
    [0x6B] = ld_l_e,
    [0x6C] = ld_l_h,
    [0x6D] = nop,
    [0x6E] = ld_l_hl,
    [0x6F] = ld_l_a,
    /*              ROW 8               */
    [0x70] = ld_hl_b,
    [0x71] = ld_hl_c,
    [0x72] = ld_hl_d,
    [0x73] = ld_hl_e,
    [0x74] = ld_hl_h,
    [0x75] = ld_hl_l,
    [0x76] = halt,
    [0x77] = ld_hl_a,
    [0x78] = ld_a_b,
    [0x79] = ld_a_c,
    [0x7A] = ld_a_d,
    [0x7B] = ld_a_e,
    [0x7C] = ld_a_h,
    [0x7D] = ld_a_l,
    [0x7E] = ld_a_hl,
    [0x7F] = nop,
    /*              ROW 9               */
    [0x80] = add_a_b,
    [0x81] = add_a_c,
    [0x82] = add_a_d,
    [0x83] = add_a_e,
    [0x84] = add_a_h,
    [0x85] = add_a_l,
    [0x86] = add_a_hl,
    [0x87] = add_a_a,
    [0x88] = adc_a_b,
    [0x89] = adc_a_c,
    [0x8A] = adc_a_d,
    [0x8B] = adc_a_e,
    [0x8C] = adc_a_h,
    [0x8D] = adc_a_l,
    [0x8E] = adc_a_hl,
    [0x8F] = adc_a_a,
    /*              ROW 10              */
    [0x90] = sub_a_b,
    [0x91] = sub_a_c,
    [0x92] = sub_a_d,
    [0x93] = sub_a_e,
    [0x94] = sub_a_h,
    [0x95] = sub_a_l,
    [0x96] = sub_a_hl,
    [0x97] = sub_a_a,
    [0x98] = sbc_a_b,
    [0x99] = sbc_a_c,
    [0x9A] = sbc_a_d,
    [0x9B] = sbc_a_e,
    [0x9C] = sbc_a_h,
    [0x9D] = sbc_a_l,
    [0x9E] = sbc_a_hl,
    [0x9F] = sbc_a_a,
    /*              ROW 11              */
    [0xA0] = and_a_b,
    [0xA1] = and_a_c,
    [0xA2] = and_a_d,
    [0xA3] = and_a_e,
    [0xA4] = and_a_h,
    [0xA5] = and_a_l,
    [0xA6] = and_a_hl,
    [0xA7] = and_a_a,
    [0xA8] = xor_a_b,
    [0xA9] = xor_a_c,
    [0xAA] = xor_a_d,
    [0xAB] = xor_a_e,
    [0xAC] = xor_a_h,
    [0xAD] = xor_a_l,
    [0xAE] = xor_a_hl,
    [0xAF] = xor_a_a,
    /*              ROW 12              */
    [0xB0] = or_a_b,
    [0xB1] = or_a_c,
    [0xB2] = or_a_d,
    [0xB3] = or_a_e,
    [0xB4] = or_a_h,
    [0xB5] = or_a_l,
    [0xB6] = or_a_hl,
    [0xB7] = or_a_a,
    [0xB8] = cp_a_b,
    [0xB9] = cp_a_c,
    [0xBA] = cp_a_d,
    [0xBB] = cp_a_e,
    [0xBC] = cp_a_h,
    [0xBD] = cp_a_l,
    [0xBE] = cp_a_hl,
    [0xBF] = cp_a_a,
    /*              ROW 13              */
    [0xC0] = ret_nz,
    [0xC1] = pop_bc,
    [0xC2] = jp_nz_nn,
    [0xC3] = jp_nn,
    [0xC4] = call_nz_nn,
    [0xC5] = push_bc,
    [0xC6] = add_a_n,
    [0xC7] = rst_00,
    [0xC8] = ret_z,
    [0xC9] = ret,
    [0xCA] = jp_z_nn,
    [0xCB] = cb_prefix,
    [0xCC] = call_z_nn,
    [0xCD] = call_nn,
    [0xCE] = adc_a_n,
    [0xCF] = rst_08,
    /*              ROW 14              */
    [0xD0] = ret_nc,
    [0xD1] = pop_de,
    [0xD2] = jp_nc_nn,
    [0xD3] = nop,
    [0xD4] = call_nc_nn,
    [0xD5] = push_de,
    [0xD6] = sub_a_n,
    [0xD7] = rst_10,
    [0xD8] = ret_c,
    [0xD9] = reti,
    [0xDA] = jp_c_nn,
    [0xDB] = nop,
    [0xDC] = call_c_nn,
    [0xDD] = nop,
    [0xDE] = sbc_a_n,
    [0xDF] = rst_18,
    /*              ROW 15              */
    [0xE0] = ldh_n_a,
    [0xE1] = pop_hl,
    [0xE2] = ldh_c_a,
    [0xE3] = nop,
    [0xE4] = nop,
    [0xE5] = push_hl,
    [0xE6] = and_a_n,
    [0xE7] = rst_20,
    [0xE8] = add_sp_n,
    [0xE9] = jp_hl,
    [0xEA] = ld_nn_a,
    [0xEB] = nop,
    [0xEC] = nop,
    [0xED] = nop,
    [0xEE] = xor_a_n,
    [0xEF] = rst_28,
    /*              ROW 16              */
    [0xF0] = ldh_a_n,
    [0xF1] = pop_af,
    [0xF2] = ldh_a_c,
    [0xF3] = di,
    [0xF4] = nop,
    [0xF5] = push_af,
    [0xF6] = or_a_n,
    [0xF7] = rst_30,
    [0xF8] = ld_hl_sp_n,
    [0xF9] = ld_sp_hl,
    [0xFA] = ld_a_nn,
    [0xFB] = ei,
    [0xFC] = nop,
    [0xFD] = nop,
    [0xFE] = cp_a_n,
    [0xFF] = rst_38,
};

static uint8_t reg_rlc_8(CPU *cpu, uint8_t r)
{
    bool  c_exists = ((r & BIT_7_MASK) != 0);
    uint8_t carry_in = c_exists ? 1 : 0;
    uint8_t   result = ((r << 1) | carry_in);
    bool   is_zero = (result == 0);
    set_flag(cpu, is_zero, ZERO_FLAG);
    set_flag(cpu, false, SUBTRACT_FLAG);
    set_flag(cpu, false, HALF_CARRY_FLAG);
    set_flag(cpu, c_exists, CARRY_FLAG);
    return result;
}
static bool rlc_b(CPU *cpu)     // 0x00 (Z 0 0 C) 2M
{
    cpu->reg.B = reg_rlc_8(cpu, cpu->reg.B);
    return true;
}
static bool rlc_c(CPU *cpu)     // 0x01 (Z 0 0 C) 2M
{
    cpu->reg.C = reg_rlc_8(cpu, cpu->reg.C);
    return true;
}  
static bool rlc_d(CPU *cpu)     // 0x02 (Z 0 0 C) 2M
{
    cpu->reg.D = reg_rlc_8(cpu, cpu->reg.D);
    return true;
}  
static bool rlc_e(CPU *cpu)     // 0x03 (Z 0 0 C) 2M
{
    cpu->reg.E = reg_rlc_8(cpu, cpu->reg.E);
    return true;
}  
static bool rlc_h(CPU *cpu)     // 0x04 (Z 0 0 C) 2M
{
    cpu->reg.H = reg_rlc_8(cpu, cpu->reg.H);
    return true;
}  
static bool rlc_l(CPU *cpu)     // 0x05 (Z 0 0 C) 2M
{
    cpu->reg.L = reg_rlc_8(cpu, cpu->reg.L);
    return true;
}  
static bool rlc_hl(CPU *cpu)    // 0x06 (Z 0 0 C) 4M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.address = getDR(cpu, HL_REG);
            cpu->ins.low = read_memory(cpu->mem, cpu->ins.address);
            return false;
        
        case 3:
            uint8_t result = reg_rlc_8(cpu, cpu->ins.low);
            write_memory(cpu->mem, cpu->ins.address, result);
            return true;
    }

    return true;
}  
static bool rlc_a(CPU *cpu)     // 0x07 (Z 0 0 C) 2M
{
    cpu->reg.A = reg_rlc_8(cpu, cpu->reg.A);
    return true;
}     

static uint8_t reg_rrc_8(CPU *cpu, uint8_t r)
{
    bool  c_exists = ((r & BIT_0_MASK) != 0);
    uint8_t carry_in = c_exists ? 1 : 0;
    uint8_t result = ((carry_in << 7) | (r >> 1));
    bool   is_zero = (result == 0);
    set_flag(cpu, is_zero, ZERO_FLAG);
    set_flag(cpu, false, SUBTRACT_FLAG);
    set_flag(cpu, false, HALF_CARRY_FLAG);
    set_flag(cpu, c_exists, CARRY_FLAG);
    return result;
}
static bool rrc_b(CPU *cpu)     // 0x08 (Z 0 0 C) 2M
{
    cpu->reg.B = reg_rrc_8(cpu, cpu->reg.B);
    return true;
}
static bool rrc_c(CPU *cpu)     // 0x09 (Z 0 0 C) 2M
{
    cpu->reg.C = reg_rrc_8(cpu, cpu->reg.C);
    return true;
}
static bool rrc_d(CPU *cpu)     // 0x0A (Z 0 0 C) 2M
{
    cpu->reg.D = reg_rrc_8(cpu, cpu->reg.D);
    return true;
}
static bool rrc_e(CPU *cpu)     // 0x0B (Z 0 0 C) 2M
{
    cpu->reg.E = reg_rrc_8(cpu, cpu->reg.E);
    return true;
}
static bool rrc_h(CPU *cpu)     // 0x0C (Z 0 0 C) 2M
{
    cpu->reg.H = reg_rrc_8(cpu, cpu->reg.H);
    return true;
}
static bool rrc_l(CPU *cpu)     // 0x0D (Z 0 0 C) 2M
{
    cpu->reg.L = reg_rrc_8(cpu, cpu->reg.L);
    return true;
}
static bool rrc_hl(CPU *cpu)    // 0x0E (Z 0 0 C) 4M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.address = getDR(cpu, HL_REG);
            cpu->ins.low = read_memory(cpu->mem, cpu->ins.address);
            return false;
        
        case 3:
            uint8_t result = reg_rrc_8(cpu, cpu->ins.low);
            write_memory(cpu->mem, cpu->ins.address, result);
            return true;
    }

    return true;
}
static bool rrc_a(CPU *cpu)     // 0x0F (Z 0 0 C) 2M
{
    cpu->reg.A = reg_rrc_8(cpu, cpu->reg.A);
    return true;
}

static uint8_t reg_rl_8(CPU *cpu, uint8_t r)
{
    bool    c_exists = ((r & BIT_7_MASK) != 0);
    uint8_t carry_in = is_flag_set(cpu, CARRY_FLAG) ? 1 : 0;
    uint8_t   result = ((r << 1) | carry_in);
    bool     is_zero = (result == 0);
    set_flag(cpu, is_zero, ZERO_FLAG);
    set_flag(cpu, false, SUBTRACT_FLAG);
    set_flag(cpu, false, HALF_CARRY_FLAG);
    set_flag(cpu, c_exists, CARRY_FLAG);
    return result;
}
static bool rl_b(CPU *cpu)      // 0x10 (Z 0 0 C) 2M
{
    cpu->reg.B = reg_rl_8(cpu, cpu->reg.B);
    return true;
}
static bool rl_c(CPU *cpu)      // 0x11 (Z 0 0 C) 2M
{
    cpu->reg.C = reg_rl_8(cpu, cpu->reg.C);
    return true;
}
static bool rl_d(CPU *cpu)      // 0x12 (Z 0 0 C) 2M
{
    cpu->reg.D = reg_rl_8(cpu, cpu->reg.D);
    return true;
}
static bool rl_e(CPU *cpu)      // 0x13 (Z 0 0 C) 2M
{
    cpu->reg.E = reg_rl_8(cpu, cpu->reg.E);
    return true;
}
static bool rl_h(CPU *cpu)      // 0x14 (Z 0 0 C) 2M
{
    cpu->reg.H = reg_rl_8(cpu, cpu->reg.H);
    return true;
}
static bool rl_l(CPU *cpu)      // 0x15 (Z 0 0 C) 2M
{
    cpu->reg.L = reg_rl_8(cpu, cpu->reg.L);
    return true;
}
static bool rl_hl(CPU *cpu)     // 0x16 (Z 0 0 C) 4M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.address = getDR(cpu, HL_REG);
            cpu->ins.low = read_memory(cpu->mem, cpu->ins.address);
            return false;
        
        case 3:
            uint8_t result = reg_rl_8(cpu, cpu->ins.low);
            write_memory(cpu->mem, cpu->ins.address, result);
            return true;
    }
    
    return true;
}
static bool rl_a(CPU *cpu)      // 0x17 (Z 0 0 C) 2M
{
    cpu->reg.A = reg_rl_8(cpu, cpu->reg.A);
    return true;
}

static uint8_t reg_rr_8(CPU *cpu, uint8_t r)
{
    bool    c_exists = ((r & BIT_0_MASK) != 0);
    uint8_t carry_in = is_flag_set(cpu, CARRY_FLAG) ? 1 : 0;
    uint8_t   result = ((carry_in << 7) | (r >> 1));
    bool     is_zero = (result == 0);
    set_flag(cpu, is_zero, ZERO_FLAG);
    set_flag(cpu, false, SUBTRACT_FLAG);
    set_flag(cpu, false, HALF_CARRY_FLAG);
    set_flag(cpu, c_exists, CARRY_FLAG);
    return result;
}
static bool rr_b(CPU *cpu)      // 0x18 (Z 0 0 C) 2M
{
    cpu->reg.B = reg_rr_8(cpu, cpu->reg.B);
    return true;
}
static bool rr_c(CPU *cpu)      // 0x19 (Z 0 0 C) 2M
{
    cpu->reg.C = reg_rr_8(cpu, cpu->reg.C);
    return true;
}
static bool rr_d(CPU *cpu)      // 0x1A (Z 0 0 C) 2M
{
    cpu->reg.D = reg_rr_8(cpu, cpu->reg.D);
    return true;
}
static bool rr_e(CPU *cpu)      // 0x1B (Z 0 0 C) 2M
{
    cpu->reg.E = reg_rr_8(cpu, cpu->reg.E);
    return true;
}
static bool rr_h(CPU *cpu)      // 0x1C (Z 0 0 C) 2M
{
    cpu->reg.H = reg_rr_8(cpu, cpu->reg.H);
    return true;
}
static bool rr_l(CPU *cpu)      // 0x1D (Z 0 0 C) 2M
{
    cpu->reg.L = reg_rr_8(cpu, cpu->reg.L);
    return true;
}
static bool rr_hl(CPU *cpu)     // 0x1E (Z 0 0 C) 4M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.address = getDR(cpu, HL_REG);
            cpu->ins.low = read_memory(cpu->mem, cpu->ins.address);
            return false;
        
        case 3:
            uint8_t result = reg_rr_8(cpu, cpu->ins.low);
            write_memory(cpu->mem, cpu->ins.address, result);
            return true;
    }
    
    return true;
}
static bool rr_a(CPU *cpu)      // 0x1F (Z 0 0 C) 2M
{
    cpu->reg.A = reg_rr_8(cpu, cpu->reg.A);
    return true;
}

static uint8_t reg_sla_8(CPU *cpu, uint8_t r)
{
    bool  c_exists = ((r & BIT_7_MASK) != 0);
    uint8_t result = (r << 1);
    bool   is_zero = (result == 0);
    set_flag(cpu, is_zero, ZERO_FLAG);
    set_flag(cpu, false, SUBTRACT_FLAG);
    set_flag(cpu, false, HALF_CARRY_FLAG);
    set_flag(cpu, c_exists, CARRY_FLAG);
    return result;
}
static bool sla_b(CPU *cpu)     // 0x20 (Z 0 0 C) 2M
{
    cpu->reg.B = reg_sla_8(cpu, cpu->reg.B);
    return true;
}
static bool sla_c(CPU *cpu)     // 0x21 (Z 0 0 C) 2M
{
    cpu->reg.C = reg_sla_8(cpu, cpu->reg.C);
    return true;
}
static bool sla_d(CPU *cpu)     // 0x22 (Z 0 0 C) 2M
{
    cpu->reg.D = reg_sla_8(cpu, cpu->reg.D);
    return true;
}
static bool sla_e(CPU *cpu)     // 0x23 (Z 0 0 C) 2M
{
    cpu->reg.E = reg_sla_8(cpu, cpu->reg.E);
    return true;
}
static bool sla_h(CPU *cpu)     // 0x24 (Z 0 0 C) 2M
{
    cpu->reg.H = reg_sla_8(cpu, cpu->reg.H);
    return true;
}
static bool sla_l(CPU *cpu)     // 0x25 (Z 0 0 C) 2M
{
    cpu->reg.L = reg_sla_8(cpu, cpu->reg.L);
    return true;
}
static bool sla_hl(CPU *cpu)    // 0x26 (Z 0 0 C) 4M
{ 
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.address = getDR(cpu, HL_REG);
            cpu->ins.low = read_memory(cpu->mem, cpu->ins.address);
            return false;
        
        case 3:
            uint8_t result = reg_sla_8(cpu, cpu->ins.low);
            write_memory(cpu->mem, cpu->ins.address, result);
            return true;
    }

    return true;
}
static bool sla_a(CPU *cpu)     // 0x27 (Z 0 0 C) 2M
{
    cpu->reg.A = reg_sla_8(cpu, cpu->reg.A);
    return true;
}

static uint8_t reg_sra_8(CPU *cpu, uint8_t r)
{
    bool  c_exists = (r & BIT_0_MASK) != 0;
    uint8_t result = (r & BIT_7_MASK) | (r >> 1);
    bool   is_zero = (result == 0);
    set_flag(cpu, is_zero, ZERO_FLAG);
    set_flag(cpu, false, SUBTRACT_FLAG);
    set_flag(cpu, false, HALF_CARRY_FLAG);
    set_flag(cpu, c_exists, CARRY_FLAG);
    return result;
}
static bool sra_b(CPU *cpu)     // 0x28 (Z 0 0 C) 2M
{
    cpu->reg.B = reg_sra_8(cpu, cpu->reg.B);
    return true;
}
static bool sra_c(CPU *cpu)     // 0x29 (Z 0 0 C) 2M
{
    cpu->reg.C = reg_sra_8(cpu, cpu->reg.C);
    return true;
}
static bool sra_d(CPU *cpu)     // 0x2A (Z 0 0 C) 2M
{
    cpu->reg.D = reg_sra_8(cpu, cpu->reg.D);
    return true;
}
static bool sra_e(CPU *cpu)     // 0x2B (Z 0 0 C) 2M
{
    cpu->reg.E = reg_sra_8(cpu, cpu->reg.E);
    return true;
}
static bool sra_h(CPU *cpu)     // 0x2C (Z 0 0 C) 2M
{
    cpu->reg.H = reg_sra_8(cpu, cpu->reg.H);
    return true;
}
static bool sra_l(CPU *cpu)     // 0x2D (Z 0 0 C) 2M
{
    cpu->reg.L = reg_sra_8(cpu, cpu->reg.L);
    return true;
}
static bool sra_hl(CPU *cpu)    // 0x2E (Z 0 0 C) 4M
{ 
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.address = getDR(cpu, HL_REG);
            cpu->ins.low = read_memory(cpu->mem, cpu->ins.address);
            return false;
        
        case 3:
            uint8_t result = reg_sra_8(cpu, cpu->ins.low);
            write_memory(cpu->mem, cpu->ins.address, result);
            return true;
    }

    return true;
}
static bool sra_a(CPU *cpu)     // 0x2F (Z 0 0 C) 2M
{
    cpu->reg.A = reg_sra_8(cpu, cpu->reg.A);
    return true;
}

static uint8_t reg_swap_8(CPU *cpu, uint8_t r)
{
    uint8_t result  = ((r >> NIBBLE) | (r << NIBBLE));
    bool    is_zero = (result == 0);
    set_flag(cpu, is_zero, ZERO_FLAG);
    set_flag(cpu, false, SUBTRACT_FLAG);
    set_flag(cpu, false, HALF_CARRY_FLAG);
    set_flag(cpu, false, CARRY_FLAG);
    return result;
}
static bool swap_b(CPU *cpu)    // 0x30 (Z 0 0 0) 2M
{
    cpu->reg.B = reg_swap_8(cpu, cpu->reg.B);
    return true;
}
static bool swap_c(CPU *cpu)    // 0x31 (Z 0 0 0) 2M
{
    cpu->reg.C = reg_swap_8(cpu, cpu->reg.C);
    return true;  
}
static bool swap_d(CPU *cpu)    // 0x32 (Z 0 0 0) 2M
{
    cpu->reg.D = reg_swap_8(cpu, cpu->reg.D);
    return true;
}
static bool swap_e(CPU *cpu)    // 0x33 (Z 0 0 0) 2M
{
    cpu->reg.E = reg_swap_8(cpu, cpu->reg.E);
    return true;
}
static bool swap_h(CPU *cpu)    // 0x34 (Z 0 0 0) 2M
{
    cpu->reg.H = reg_swap_8(cpu, cpu->reg.H);
    return true;
}
static bool swap_l(CPU *cpu)    // 0x35 (Z 0 0 0) 2M
{
    cpu->reg.L = reg_swap_8(cpu, cpu->reg.L);
    return true;
}
static bool swap_hl(CPU *cpu)   // 0x36 (Z 0 0 0) 4M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.address = getDR(cpu, HL_REG);
            cpu->ins.low = read_memory(cpu->mem, cpu->ins.address);
            return false;
        
        case 3:
            uint8_t result = reg_swap_8(cpu, cpu->ins.low);
            write_memory(cpu->mem, cpu->ins.address, result);
            return true;
    }

    return true;
}
static bool swap_a(CPU *cpu)    // 0x37 (Z 0 0 0) 2M
{
    cpu->reg.A = reg_swap_8(cpu, cpu->reg.A);
    return true;
}

static uint8_t reg_srl_8(CPU *cpu, uint8_t r)
{
    bool  c_exists = ((r & BIT_0_MASK) != 0);
    uint8_t result = (r >> 1);
    bool   is_zero = (result == 0);
    set_flag(cpu, is_zero, ZERO_FLAG);
    set_flag(cpu, false, SUBTRACT_FLAG);
    set_flag(cpu, false, HALF_CARRY_FLAG);
    set_flag(cpu, c_exists, CARRY_FLAG);
    return result;
}
static bool srl_b(CPU *cpu)     // 0x38 (Z 0 0 C) 2M
{
    cpu->reg.B = reg_srl_8(cpu, cpu->reg.B);
    return true;
}
static bool srl_c(CPU *cpu)     // 0x39 (Z 0 0 C) 2M
{
    cpu->reg.C = reg_srl_8(cpu, cpu->reg.C);
    return true;
}
static bool srl_d(CPU *cpu)     // 0x3A (Z 0 0 C) 2M
{
    cpu->reg.D = reg_srl_8(cpu, cpu->reg.D);
    return true;
}
static bool srl_e(CPU *cpu)     // 0x3B (Z 0 0 C) 2M
{
    cpu->reg.E = reg_srl_8(cpu, cpu->reg.E);
    return true;
}
static bool srl_h(CPU *cpu)     // 0x3C (Z 0 0 C) 2M
{
    cpu->reg.H = reg_srl_8(cpu, cpu->reg.H);
    return true;
}
static bool srl_l(CPU *cpu)     // 0x3D (Z 0 0 C) 2M
{
    cpu->reg.L = reg_srl_8(cpu, cpu->reg.L);
    return true;
}
static bool srl_hl(CPU *cpu)    // 0x3E (Z 0 0 C) 4M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.address = getDR(cpu, HL_REG);
            cpu->ins.low = read_memory(cpu->mem, cpu->ins.address);
            return false;
        
        case 3:
            uint8_t result = reg_srl_8(cpu, cpu->ins.low);
            write_memory(cpu->mem, cpu->ins.address, result);
            return true;
    }

    return true;
}
static bool srl_a(CPU *cpu)     // 0x3F (Z 0 0 C) 2M
{
    cpu->reg.A = reg_srl_8(cpu, cpu->reg.A);
    return true;
}

static void reg_bit_x(CPU *cpu, BitMask mask, uint8_t r)
{
    bool is_zero = ((r & mask) == 0);
    set_flag(cpu, is_zero, ZERO_FLAG);
    set_flag(cpu, false, SUBTRACT_FLAG);
    set_flag(cpu, true, HALF_CARRY_FLAG);
}
static bool bit_0_b(CPU *cpu)   // 0x40 (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_0_MASK, cpu->reg.B);
    return true;
}
static bool bit_0_c(CPU *cpu)   // 0x41 (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_0_MASK, cpu->reg.C);
    return true;
}
static bool bit_0_d(CPU *cpu)   // 0x42 (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_0_MASK, cpu->reg.D);
    return true;
}
static bool bit_0_e(CPU *cpu)   // 0x43 (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_0_MASK, cpu->reg.E);
    return true;
}
static bool bit_0_h(CPU *cpu)   // 0x44 (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_0_MASK, cpu->reg.H);
    return true;
}
static bool bit_0_l(CPU *cpu)   // 0x45 (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_0_MASK, cpu->reg.L);
    return true;
}
static bool bit_0_hl(CPU *cpu)  // 0x46 (Z 0 1 -) 3M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.address = getDR(cpu, HL_REG);
            cpu->ins.    low = read_memory(cpu->mem, cpu->ins.address);
            reg_bit_x(cpu, BIT_0_MASK, cpu->ins.low);
            return true;
    }

    return true;
}
static bool bit_0_a(CPU *cpu)   // 0x47 (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_0_MASK, cpu->reg.A);
    return true;
}
static bool bit_1_b(CPU *cpu)   // 0x48 (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_1_MASK, cpu->reg.B);
    return true;
}
static bool bit_1_c(CPU *cpu)   // 0x49 (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_1_MASK, cpu->reg.C);
    return true;
}
static bool bit_1_d(CPU *cpu)   // 0x4A (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_1_MASK, cpu->reg.D);
    return true;
}
static bool bit_1_e(CPU *cpu)   // 0x4B (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_1_MASK, cpu->reg.E);
    return true;
}
static bool bit_1_h(CPU *cpu)   // 0x4C (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_1_MASK, cpu->reg.H);
    return true;
}
static bool bit_1_l(CPU *cpu)   // 0x4D (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_1_MASK, cpu->reg.L);
    return true;
}
static bool bit_1_hl(CPU *cpu)  // 0x4E (Z 0 1 -) 3M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.address = getDR(cpu, HL_REG);
            cpu->ins.    low = read_memory(cpu->mem, cpu->ins.address);
            reg_bit_x(cpu, BIT_1_MASK, cpu->ins.low);
            return true;
    }
    
    return true;
}
static bool bit_1_a(CPU *cpu)   // 0x4F (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_1_MASK, cpu->reg.A);
    return true;
}
static bool bit_2_b(CPU *cpu)   // 0x50 (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_2_MASK, cpu->reg.B);
    return true;
}
static bool bit_2_c(CPU *cpu)   // 0x51 (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_2_MASK, cpu->reg.C);
    return true;
}
static bool bit_2_d(CPU *cpu)   // 0x52 (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_2_MASK, cpu->reg.D);
    return true;
}
static bool bit_2_e(CPU *cpu)   // 0x53 (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_2_MASK, cpu->reg.E);
    return true;
}
static bool bit_2_h(CPU *cpu)   // 0x54 (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_2_MASK, cpu->reg.H);
    return true;
}
static bool bit_2_l(CPU *cpu)   // 0x55 (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_2_MASK, cpu->reg.L);
    return true;
}
static bool bit_2_hl(CPU *cpu)  // 0x56 (Z 0 1 -) 3M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.address = getDR(cpu, HL_REG);
            cpu->ins.    low = read_memory(cpu->mem, cpu->ins.address);
            reg_bit_x(cpu, BIT_2_MASK, cpu->ins.low);
            return true;
    }
    
    return true;
}
static bool bit_2_a(CPU *cpu)   // 0x57 (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_2_MASK, cpu->reg.A);
    return true;
}
static bool bit_3_b(CPU *cpu)   // 0x58 (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_3_MASK, cpu->reg.B);
    return true;
}
static bool bit_3_c(CPU *cpu)   // 0x59 (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_3_MASK, cpu->reg.C);
    return true;
}
static bool bit_3_d(CPU *cpu)   // 0x5A (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_3_MASK, cpu->reg.D);
    return true;
}
static bool bit_3_e(CPU *cpu)   // 0x5B (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_3_MASK, cpu->reg.E);
    return true;
}
static bool bit_3_h(CPU *cpu)   // 0x5C (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_3_MASK, cpu->reg.H);
    return true;
}
static bool bit_3_l(CPU *cpu)   // 0x5D (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_3_MASK, cpu->reg.L);
    return true;
}
static bool bit_3_hl(CPU *cpu)  // 0x5E (Z 0 1 -) 3M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.address = getDR(cpu, HL_REG);
            cpu->ins.    low = read_memory(cpu->mem, cpu->ins.address);
            reg_bit_x(cpu, BIT_3_MASK, cpu->ins.low);
            return true;
    }

    return true;
}
static bool bit_3_a(CPU *cpu)   // 0x5F (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_3_MASK, cpu->reg.A);
    return true;
}
static bool bit_4_b(CPU *cpu)   // 0x60 (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_4_MASK, cpu->reg.B);
    return true;
}
static bool bit_4_c(CPU *cpu)   // 0x61 (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_4_MASK, cpu->reg.C);
    return true;
}
static bool bit_4_d(CPU *cpu)   // 0x62 (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_4_MASK, cpu->reg.D);
    return true;
}
static bool bit_4_e(CPU *cpu)   // 0x63 (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_4_MASK, cpu->reg.E);
    return true;
}
static bool bit_4_h(CPU *cpu)   // 0x64 (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_4_MASK, cpu->reg.H);
    return true;
}
static bool bit_4_l(CPU *cpu)   // 0x65 (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_4_MASK, cpu->reg.L);
    return true;
}
static bool bit_4_hl(CPU *cpu)  // 0x66 (Z 0 1 -) 3M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.address = getDR(cpu, HL_REG);
            cpu->ins.    low = read_memory(cpu->mem, cpu->ins.address);
            reg_bit_x(cpu, BIT_4_MASK, cpu->ins.low);
            return true;
    }
    
    return true;
}
static bool bit_4_a(CPU *cpu)   // 0x67 (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_4_MASK, cpu->reg.A);
    return true;
}
static bool bit_5_b(CPU *cpu)   // 0x68 (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_5_MASK, cpu->reg.B);
    return true;
}
static bool bit_5_c(CPU *cpu)   // 0x69 (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_5_MASK, cpu->reg.C);
    return true;
}
static bool bit_5_d(CPU *cpu)   // 0x6A (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_5_MASK, cpu->reg.D);
    return true;
}
static bool bit_5_e(CPU *cpu)   // 0x6B (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_5_MASK, cpu->reg.E);
    return true;
}
static bool bit_5_h(CPU *cpu)   // 0x6C (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_5_MASK, cpu->reg.H);
    return true;
}
static bool bit_5_l(CPU *cpu)   // 0x6D (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_5_MASK, cpu->reg.L);
    return true;
}
static bool bit_5_hl(CPU *cpu)  // 0x6E (Z 0 1 -) 3M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.address = getDR(cpu, HL_REG);
            cpu->ins.    low = read_memory(cpu->mem, cpu->ins.address);
            reg_bit_x(cpu, BIT_5_MASK, cpu->ins.low);
            return true;
    }
    
    return true;
}
static bool bit_5_a(CPU *cpu)   // 0x6F (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_5_MASK, cpu->reg.A);
    return true;
}
static bool bit_6_b(CPU *cpu)   // 0x70 (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_6_MASK, cpu->reg.B);
    return true;
}
static bool bit_6_c(CPU *cpu)   // 0x71 (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_6_MASK, cpu->reg.C);
    return true;
}
static bool bit_6_d(CPU *cpu)   // 0x72 (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_6_MASK, cpu->reg.D);
    return true;
}
static bool bit_6_e(CPU *cpu)   // 0x73 (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_6_MASK, cpu->reg.E);
    return true;
}
static bool bit_6_h(CPU *cpu)   // 0x74 (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_6_MASK, cpu->reg.H);
    return true;
}
static bool bit_6_l(CPU *cpu)   // 0x75 (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_6_MASK, cpu->reg.L);
    return true;
}
static bool bit_6_hl(CPU *cpu)  // 0x76 (Z 0 1 -) 3M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.address = getDR(cpu, HL_REG);
            cpu->ins.    low = read_memory(cpu->mem, cpu->ins.address);
            reg_bit_x(cpu, BIT_6_MASK, cpu->ins.low);
            return true;
    }

    return true;
}
static bool bit_6_a(CPU *cpu)   // 0x77 (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_6_MASK, cpu->reg.A);
    return true;
}
static bool bit_7_b(CPU *cpu)   // 0x78 (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_7_MASK, cpu->reg.B);
    return true;
}
static bool bit_7_c(CPU *cpu)   // 0x79 (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_7_MASK, cpu->reg.C);
    return true;
}
static bool bit_7_d(CPU *cpu)   // 0x7A (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_7_MASK, cpu->reg.D);
    return true;
}
static bool bit_7_e(CPU *cpu)   // 0x7B (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_7_MASK, cpu->reg.E);
    return true;
}
static bool bit_7_h(CPU *cpu)   // 0x7C (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_7_MASK, cpu->reg.H);
    return true;
}
static bool bit_7_l(CPU *cpu)   // 0x7D (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_7_MASK, cpu->reg.L);
    return true;;
}
static bool bit_7_hl(CPU *cpu)  // 0x7E (Z 0 1 -) 3M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.address = getDR(cpu, HL_REG);
            cpu->ins.    low = read_memory(cpu->mem, cpu->ins.address);
            reg_bit_x(cpu, BIT_7_MASK, cpu->ins.low);
            return true;
    }

    return true;
}
static bool bit_7_a(CPU *cpu)   // 0x7F (Z 0 1 -) 2M
{
    reg_bit_x(cpu, BIT_7_MASK, cpu->reg.A);
    return true;
}

static bool res_0_b(CPU *cpu)   // 0x80 (- - - -) 2M
{
    cpu->reg.B = (cpu->reg.B & ~BIT_0_MASK);
    return true;
}
static bool res_0_c(CPU *cpu)   // 0x81 (- - - -) 2M
{
    cpu->reg.C = (cpu->reg.C & ~BIT_0_MASK);
    return true;
}
static bool res_0_d(CPU *cpu)   // 0x82 (- - - -) 2M
{
    cpu->reg.D = (cpu->reg.D & ~BIT_0_MASK);
    return true;
}
static bool res_0_e(CPU *cpu)   // 0x83 (- - - -) 2M
{
    cpu->reg.E = (cpu->reg.E & ~BIT_0_MASK);
    return true;
}
static bool res_0_h(CPU *cpu)   // 0x84 (- - - -) 2M
{
    cpu->reg.H = (cpu->reg.H & ~BIT_0_MASK);
    return true;
}
static bool res_0_l(CPU *cpu)   // 0x85 (- - - -) 2M
{
    cpu->reg.L = (cpu->reg.L & ~BIT_0_MASK);
    return true;
}
static bool res_0_hl(CPU *cpu)  // 0x86 (- - - -) 4M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.address = getDR(cpu, HL_REG);
            cpu->ins.low = read_memory(cpu->mem, cpu->ins.address);
            return false;
        
        case 3:
            uint8_t result = (cpu->ins.low & (~BIT_0_MASK)); 
            write_memory(cpu->mem, cpu->ins.address, result);
            return true;
    }

    return true;
}
static bool res_0_a(CPU *cpu)   // 0x87 (- - - -) 2M
{
    cpu->reg.A = (cpu->reg.A & ~BIT_0_MASK);
    return true;
}
static bool res_1_b(CPU *cpu)   // 0x88 (- - - -) 2M
{
    cpu->reg.B = (cpu->reg.B & ~BIT_1_MASK);
    return true;
}
static bool res_1_c(CPU *cpu)   // 0x89 (- - - -) 2M
{
    cpu->reg.C = (cpu->reg.C & ~BIT_1_MASK);
    return true;
}
static bool res_1_d(CPU *cpu)   // 0x8A (- - - -) 2M
{
    cpu->reg.D = (cpu->reg.D & ~BIT_1_MASK);
    return true;
}
static bool res_1_e(CPU *cpu)   // 0x8B (- - - -) 2M
{
    cpu->reg.E = (cpu->reg.E & ~BIT_1_MASK);
    return true;
}
static bool res_1_h(CPU *cpu)   // 0x8C (- - - -) 2M
{
    cpu->reg.H = (cpu->reg.H & ~BIT_1_MASK);
    return true;
}
static bool res_1_l(CPU *cpu)   // 0x8D (- - - -) 2M
{
    cpu->reg.L = (cpu->reg.L & ~BIT_1_MASK);
    return true;
}
static bool res_1_hl(CPU *cpu)  // 0x8E (- - - -) 4M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.address = getDR(cpu, HL_REG);
            cpu->ins.low = read_memory(cpu->mem, cpu->ins.address);
            return false;
        
        case 3:
            uint8_t result = (cpu->ins.low & (~BIT_1_MASK)); 
            write_memory(cpu->mem, cpu->ins.address, result);
            return true;
    }
    
    return true;
}
static bool res_1_a(CPU *cpu)   // 0x8F (- - - -) 2M
{
    cpu->reg.A = (cpu->reg.A & ~BIT_1_MASK);
    return true;
}
static bool res_2_b(CPU *cpu)   // 0x90 (- - - -) 2M
{
    cpu->reg.B = (cpu->reg.B & ~BIT_2_MASK);
    return true;
}
static bool res_2_c(CPU *cpu)   // 0x91 (- - - -) 2M
{
    cpu->reg.C = (cpu->reg.C & ~BIT_2_MASK);
    return true;
}
static bool res_2_d(CPU *cpu)   // 0x92 (- - - -) 2M
{
    cpu->reg.D = (cpu->reg.D & ~BIT_2_MASK);
    return true;
}
static bool res_2_e(CPU *cpu)   // 0x93 (- - - -) 2M
{
    cpu->reg.E = (cpu->reg.E & ~BIT_2_MASK);
    return true;
}
static bool res_2_h(CPU *cpu)   // 0x94 (- - - -) 2M
{
    cpu->reg.H = (cpu->reg.H & ~BIT_2_MASK);
    return true;
}
static bool res_2_l(CPU *cpu)   // 0x95 (- - - -) 2M
{
    cpu->reg.L = (cpu->reg.L & ~BIT_2_MASK);
    return true;
}
static bool res_2_hl(CPU *cpu)  // 0x96 (- - - -) 4M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.address = getDR(cpu, HL_REG);
            cpu->ins.low = read_memory(cpu->mem, cpu->ins.address);
            return false;
        
        case 3:
            uint8_t result = (cpu->ins.low & (~BIT_2_MASK)); 
            write_memory(cpu->mem, cpu->ins.address, result);
            return true;
    }
    
    return true;
}
static bool res_2_a(CPU *cpu)   // 0x97 (- - - -) 2M
{
    cpu->reg.A = (cpu->reg.A & ~BIT_2_MASK);
    return true;
}
static bool res_3_b(CPU *cpu)   // 0x98 (- - - -) 2M
{
    cpu->reg.B = (cpu->reg.B & ~BIT_3_MASK);
    return true;
}
static bool res_3_c(CPU *cpu)   // 0x99 (- - - -) 2M
{
    cpu->reg.C = (cpu->reg.C & ~BIT_3_MASK);
    return true;
}
static bool res_3_d(CPU *cpu)   // 0x9A (- - - -) 2M
{
    cpu->reg.D = (cpu->reg.D & ~BIT_3_MASK);
    return true;
}
static bool res_3_e(CPU *cpu)   // 0x9B (- - - -) 2M
{
    cpu->reg.E = (cpu->reg.E & ~BIT_3_MASK);
    return true;
}
static bool res_3_h(CPU *cpu)   // 0x9C (- - - -) 2M
{
    cpu->reg.H = (cpu->reg.H & ~BIT_3_MASK);
    return true;
}
static bool res_3_l(CPU *cpu)   // 0x9D (- - - -) 2M
{
    cpu->reg.L = (cpu->reg.L & ~BIT_3_MASK);
    return true;
}
static bool res_3_hl(CPU *cpu)  // 0x9E (- - - -) 4M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.address = getDR(cpu, HL_REG);
            cpu->ins.low = read_memory(cpu->mem, cpu->ins.address);
            return false;
        
        case 3:
            uint8_t result = (cpu->ins.low & (~BIT_3_MASK)); 
            write_memory(cpu->mem, cpu->ins.address, result);
            return true;
    }
    
    return true;
}
static bool res_3_a(CPU *cpu)   // 0x9F (- - - -) 2M
{
    cpu->reg.A = (cpu->reg.A & ~BIT_3_MASK);
    return true;
}
static bool res_4_b(CPU *cpu)   // 0xA0 (- - - -) 2M
{
    cpu->reg.B = (cpu->reg.B & ~BIT_4_MASK);
    return true;
}
static bool res_4_c(CPU *cpu)   // 0xA1 (- - - -) 2M
{
    cpu->reg.C = (cpu->reg.C & ~BIT_4_MASK);
    return true;
}
static bool res_4_d(CPU *cpu)   // 0xA2 (- - - -) 2M
{
    cpu->reg.D = (cpu->reg.D & ~BIT_4_MASK);
    return true;
}
static bool res_4_e(CPU *cpu)   // 0xA3 (- - - -) 2M
{
    cpu->reg.E = (cpu->reg.E & ~BIT_4_MASK);
    return true;
}
static bool res_4_h(CPU *cpu)   // 0xA4 (- - - -) 2M
{
    cpu->reg.H = (cpu->reg.H & ~BIT_4_MASK);
    return true;
}
static bool res_4_l(CPU *cpu)   // 0xA5 (- - - -) 2M
{
    cpu->reg.L = (cpu->reg.L & ~BIT_4_MASK);
    return true;
}
static bool res_4_hl(CPU *cpu)  // 0xA6 (- - - -) 4M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.address = getDR(cpu, HL_REG);
            cpu->ins.low = read_memory(cpu->mem, cpu->ins.address);
            return false;
        
        case 3:
            uint8_t result = (cpu->ins.low & (~BIT_4_MASK)); 
            write_memory(cpu->mem, cpu->ins.address, result);
            return true;
    }
    
    return true;
}
static bool res_4_a(CPU *cpu)   // 0xA7 (- - - -) 2M
{
    cpu->reg.A = (cpu->reg.A & ~BIT_4_MASK);
    return true;
}
static bool res_5_b(CPU *cpu)   // 0xA8 (- - - -) 2M
{
    cpu->reg.B = (cpu->reg.B & ~BIT_5_MASK);
    return true;
}
static bool res_5_c(CPU *cpu)   // 0xA9 (- - - -) 2M
{
    cpu->reg.C = (cpu->reg.C & ~BIT_5_MASK);
    return true;
}
static bool res_5_d(CPU *cpu)   // 0xAA (- - - -) 2M
{
    cpu->reg.D = (cpu->reg.D & ~BIT_5_MASK);
    return true;
}
static bool res_5_e(CPU *cpu)   // 0xAB (- - - -) 2M
{
    cpu->reg.E = (cpu->reg.E & ~BIT_5_MASK);
    return true;
}
static bool res_5_h(CPU *cpu)   // 0xAC (- - - -) 2M
{
    cpu->reg.H = (cpu->reg.H & ~BIT_5_MASK);
    return true;
}
static bool res_5_l(CPU *cpu)   // 0xAD (- - - -) 2M
{
    cpu->reg.L = (cpu->reg.L & ~BIT_5_MASK);
    return true;
}
static bool res_5_hl(CPU *cpu)  // 0xAE (- - - -) 4M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.address = getDR(cpu, HL_REG);
            cpu->ins.low = read_memory(cpu->mem, cpu->ins.address);
            return false;
        
        case 3:
            uint8_t result = (cpu->ins.low & (~BIT_5_MASK)); 
            write_memory(cpu->mem, cpu->ins.address, result);
            return true;
    }
    
    return true;
}
static bool res_5_a(CPU *cpu)   // 0xAF (- - - -) 2M
{
    cpu->reg.A = (cpu->reg.A & ~BIT_5_MASK);
    return true;
}
static bool res_6_b(CPU *cpu)   // 0xB0 (- - - -) 2M
{
    cpu->reg.B = (cpu->reg.B & ~BIT_6_MASK);
    return true;
}
static bool res_6_c(CPU *cpu)   // 0xB1 (- - - -) 2M
{
    cpu->reg.C = (cpu->reg.C & ~BIT_6_MASK);
    return true;
}
static bool res_6_d(CPU *cpu)   // 0xB2 (- - - -) 2M
{
    cpu->reg.D = (cpu->reg.D & ~BIT_6_MASK);
    return true;
}
static bool res_6_e(CPU *cpu)   // 0xB3 (- - - -) 2M
{
    cpu->reg.E = (cpu->reg.E & ~BIT_6_MASK);
    return true;
}
static bool res_6_h(CPU *cpu)   // 0xB4 (- - - -) 2M
{
    cpu->reg.H = (cpu->reg.H & ~BIT_6_MASK);
    return true;
}
static bool res_6_l(CPU *cpu)   // 0xB5 (- - - -) 2M
{
    cpu->reg.L = (cpu->reg.L & ~BIT_6_MASK);
    return true;
}
static bool res_6_hl(CPU *cpu)  // 0xB6 (- - - -) 4M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.address = getDR(cpu, HL_REG);
            cpu->ins.low = read_memory(cpu->mem, cpu->ins.address);
            return false;
        
        case 3:
            uint8_t result = (cpu->ins.low & (~BIT_6_MASK)); 
            write_memory(cpu->mem, cpu->ins.address, result);
            return true;
    }
    
    return true;
}
static bool res_6_a(CPU *cpu)   // 0xB7 (- - - -) 2M
{
    cpu->reg.A = (cpu->reg.A & ~BIT_6_MASK);
    return true;
}
static bool res_7_b(CPU *cpu)   // 0xB8 (- - - -) 2M
{
    cpu->reg.B = (cpu->reg.B & ~BIT_7_MASK);
    return true;
}
static bool res_7_c(CPU *cpu)   // 0xB9 (- - - -) 2M
{
    cpu->reg.C = (cpu->reg.C & ~BIT_7_MASK);
    return true;
}
static bool res_7_d(CPU *cpu)   // 0xBA (- - - -) 2M
{
    cpu->reg.D = (cpu->reg.D & ~BIT_7_MASK);
    return true;
}
static bool res_7_e(CPU *cpu)   // 0xBB (- - - -) 2M
{
    cpu->reg.E = (cpu->reg.E & ~BIT_7_MASK);
    return true;
}
static bool res_7_h(CPU *cpu)   // 0xBC (- - - -) 2M
{
    cpu->reg.H = (cpu->reg.H & ~BIT_7_MASK);
    return true;
}
static bool res_7_l(CPU *cpu)   // 0xBD (- - - -) 2M
{
    cpu->reg.L = (cpu->reg.L & ~BIT_7_MASK);
    return true;
}
static bool res_7_hl(CPU *cpu)  // 0xBE (- - - -) 4M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.address = getDR(cpu, HL_REG);
            cpu->ins.low = read_memory(cpu->mem, cpu->ins.address);
            return false;
        
        case 3:
            uint8_t result = (cpu->ins.low & (~BIT_7_MASK)); 
            write_memory(cpu->mem, cpu->ins.address, result);
            return true;
    }
    
    return true;
}
static bool res_7_a(CPU *cpu)   // 0xBF (- - - -) 2M
{
    cpu->reg.A = (cpu->reg.A & ~BIT_7_MASK);
    return true;
}

static bool set_0_b(CPU *cpu)   // 0xC0 (- - - -) 2M
{
    cpu->reg.B = (cpu->reg.B | BIT_0_MASK);
    return true;
}
static bool set_0_c(CPU *cpu)   // 0xC1 (- - - -) 2M
{
    cpu->reg.C = (cpu->reg.C | BIT_0_MASK);
    return true;
}
static bool set_0_d(CPU *cpu)   // 0xC2 (- - - -) 2M
{
    cpu->reg.D = (cpu->reg.D | BIT_0_MASK);
    return true;
}
static bool set_0_e(CPU *cpu)   // 0xC3 (- - - -) 2M
{
    cpu->reg.E = (cpu->reg.E | BIT_0_MASK);
    return true;
}
static bool set_0_h(CPU *cpu)   // 0xC4 (- - - -) 2M
{
    cpu->reg.H = (cpu->reg.H | BIT_0_MASK);
    return true;
}
static bool set_0_l(CPU *cpu)   // 0xC5 (- - - -) 2M
{
    cpu->reg.L = (cpu->reg.L | BIT_0_MASK);
    return true;
}
static bool set_0_hl(CPU *cpu)  // 0xC6 (- - - -) 4M
{ 
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.address = getDR(cpu, HL_REG);
            cpu->ins.low = read_memory(cpu->mem, cpu->ins.address);
            return false;
        
        case 3:
            uint8_t result = (cpu->ins.low | BIT_0_MASK); 
            write_memory(cpu->mem, cpu->ins.address, result);
            return true;
    }

    return true;
}
static bool set_0_a(CPU *cpu)   // 0xC7 (- - - -) 2M
{
    cpu->reg.A = (cpu->reg.A | BIT_0_MASK);
    return true;
}
static bool set_1_b(CPU *cpu)   // 0xC8 (- - - -) 2M
{
    cpu->reg.B = (cpu->reg.B | BIT_1_MASK);
    return true;
}
static bool set_1_c(CPU *cpu)   // 0xC9 (- - - -) 2M
{
    cpu->reg.C = (cpu->reg.C | BIT_1_MASK);
    return true;
}
static bool set_1_d(CPU *cpu)   // 0xCA (- - - -) 2M
{
    cpu->reg.D = (cpu->reg.D | BIT_1_MASK);
    return true;
}
static bool set_1_e(CPU *cpu)   // 0xCB (- - - -) 2M
{
    cpu->reg.E = (cpu->reg.E | BIT_1_MASK);
    return true;
}
static bool set_1_h(CPU *cpu)   // 0xCC (- - - -) 2M
{
    cpu->reg.H = (cpu->reg.H | BIT_1_MASK);
    return true;
}
static bool set_1_l(CPU *cpu)   // 0xCD (- - - -) 2M
{
    cpu->reg.L = (cpu->reg.L | BIT_1_MASK);
    return true;
}
static bool set_1_hl(CPU *cpu)  // 0xCE (- - - -) 4M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.address = getDR(cpu, HL_REG);
            cpu->ins.low = read_memory(cpu->mem, cpu->ins.address);
            return false;
        
        case 3:
            uint8_t result = (cpu->ins.low | BIT_1_MASK); 
            write_memory(cpu->mem, cpu->ins.address, result);
            return true;
    }
    
    return true;
}
static bool set_1_a(CPU *cpu)   // 0xCF (- - - -) 2M
{
    cpu->reg.A = (cpu->reg.A | BIT_1_MASK);
    return true;
}
static bool set_2_b(CPU *cpu)   // 0xD0 (- - - -) 2M
{
    cpu->reg.B = (cpu->reg.B | BIT_2_MASK);
    return true;
}
static bool set_2_c(CPU *cpu)   // 0xD1 (- - - -) 2M
{
    cpu->reg.C = (cpu->reg.C | BIT_2_MASK);
    return true;
}
static bool set_2_d(CPU *cpu)   // 0xD2 (- - - -) 2M
{
    cpu->reg.D = (cpu->reg.D | BIT_2_MASK);
    return true;
}
static bool set_2_e(CPU *cpu)   // 0xD3 (- - - -) 2M
{
    cpu->reg.E = (cpu->reg.E | BIT_2_MASK);
    return true;
}
static bool set_2_h(CPU *cpu)   // 0xD4 (- - - -) 2M
{
    cpu->reg.H = (cpu->reg.H | BIT_2_MASK);
    return true;
}
static bool set_2_l(CPU *cpu)   // 0xD5 (- - - -) 2M
{
    cpu->reg.L = (cpu->reg.L | BIT_2_MASK);
    return true;
}
static bool set_2_hl(CPU *cpu)  // 0xD6 (- - - -) 4M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.address = getDR(cpu, HL_REG);
            cpu->ins.low = read_memory(cpu->mem, cpu->ins.address);
            return false;
        
        case 3:
            uint8_t result = (cpu->ins.low | BIT_2_MASK); 
            write_memory(cpu->mem, cpu->ins.address, result);
            return true;
    }
    
    return true;
}
static bool set_2_a(CPU *cpu)   // 0xD7 (- - - -) 2M
{
    cpu->reg.A = (cpu->reg.A | BIT_2_MASK);
    return true;
}
static bool set_3_b(CPU *cpu)   // 0xD8 (- - - -) 2M
{
    cpu->reg.B = (cpu->reg.B | BIT_3_MASK);
    return true;
}
static bool set_3_c(CPU *cpu)   // 0xD9 (- - - -) 2M
{
    cpu->reg.C = (cpu->reg.C | BIT_3_MASK);
    return true;
}
static bool set_3_d(CPU *cpu)   // 0xDA (- - - -) 2M
{
    cpu->reg.D = (cpu->reg.D | BIT_3_MASK);
    return true;
}
static bool set_3_e(CPU *cpu)   // 0xDB (- - - -) 2M
{
    cpu->reg.E = (cpu->reg.E | BIT_3_MASK);
    return true;
}
static bool set_3_h(CPU *cpu)   // 0xDC (- - - -) 2M
{
    cpu->reg.H = (cpu->reg.H | BIT_3_MASK);
    return true;
}
static bool set_3_l(CPU *cpu)   // 0xDD (- - - -) 2M
{
    cpu->reg.L = (cpu->reg.L | BIT_3_MASK);
    return true;
}
static bool set_3_hl(CPU *cpu)  // 0xDE (- - - -) 4M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.address = getDR(cpu, HL_REG);
            cpu->ins.low = read_memory(cpu->mem, cpu->ins.address);
            return false;
        
        case 3:
            uint8_t result = (cpu->ins.low | BIT_3_MASK); 
            write_memory(cpu->mem, cpu->ins.address, result);
            return true;
    }

    return true;
}
static bool set_3_a(CPU *cpu)   // 0xDF (- - - -) 2M
{
    cpu->reg.A = (cpu->reg.A | BIT_3_MASK);
    return true;
}
static bool set_4_b(CPU *cpu)   // 0xE0 (- - - -) 2M
{
    cpu->reg.B = (cpu->reg.B | BIT_4_MASK);
    return true;
}
static bool set_4_c(CPU *cpu)   // 0xE1 (- - - -) 2M
{
    cpu->reg.C = (cpu->reg.C | BIT_4_MASK);
    return true;
}
static bool set_4_d(CPU *cpu)   // 0xE2 (- - - -) 2M
{
    cpu->reg.D = (cpu->reg.D | BIT_4_MASK);
    return true;
}
static bool set_4_e(CPU *cpu)   // 0xE3 (- - - -) 2M
{
    cpu->reg.E = (cpu->reg.E | BIT_4_MASK);
    return true;
}
static bool set_4_h(CPU *cpu)   // 0xE4 (- - - -) 2M
{
    cpu->reg.H = (cpu->reg.H | BIT_4_MASK);
    return true;
}
static bool set_4_l(CPU *cpu)   // 0xE5 (- - - -) 2M
{
    cpu->reg.L = (cpu->reg.L | BIT_4_MASK);
    return true;
}
static bool set_4_hl(CPU *cpu)  // 0xE6 (- - - -) 4M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.address = getDR(cpu, HL_REG);
            cpu->ins.low = read_memory(cpu->mem, cpu->ins.address);
            return false;
        
        case 3:
            uint8_t result = (cpu->ins.low | BIT_4_MASK); 
            write_memory(cpu->mem, cpu->ins.address, result);
            return true;
    }
    
    return true;
}
static bool set_4_a(CPU *cpu)   // 0xE7 (- - - -) 2M
{
    cpu->reg.A = (cpu->reg.A | BIT_4_MASK);
    return true;
}
static bool set_5_b(CPU *cpu)   // 0xE8 (- - - -) 2M
{
    cpu->reg.B = (cpu->reg.B | BIT_5_MASK);
    return true;
}
static bool set_5_c(CPU *cpu)   // 0xE9 (- - - -) 2M
{
    cpu->reg.C = (cpu->reg.C | BIT_5_MASK);
    return true;
}
static bool set_5_d(CPU *cpu)   // 0xEA (- - - -) 2M
{
    cpu->reg.D = (cpu->reg.D | BIT_5_MASK);
    return true;
}
static bool set_5_e(CPU *cpu)   // 0xEB (- - - -) 2M
{
    cpu->reg.E = (cpu->reg.E | BIT_5_MASK);
    return true;
}
static bool set_5_h(CPU *cpu)   // 0xEC (- - - -) 2M
{
    cpu->reg.H = (cpu->reg.H | BIT_5_MASK);
    return true;
}
static bool set_5_l(CPU *cpu)   // 0xED (- - - -) 2M
{
    cpu->reg.L = (cpu->reg.L | BIT_5_MASK);
    return true;
}
static bool set_5_hl(CPU *cpu)  // 0xEE (- - - -) 4M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.address = getDR(cpu, HL_REG);
            cpu->ins.low = read_memory(cpu->mem, cpu->ins.address);
            return false;
        
        case 3:
            uint8_t result = (cpu->ins.low | BIT_5_MASK); 
            write_memory(cpu->mem, cpu->ins.address, result);
            return true;
    }
    
    return true;
}
static bool set_5_a(CPU *cpu)   // 0xEF (- - - -) 2M
{
    cpu->reg.A = (cpu->reg.A | BIT_5_MASK);
    return true;
}
static bool set_6_b(CPU *cpu)   // 0xF0 (- - - -) 2M
{
    cpu->reg.B = (cpu->reg.B | BIT_6_MASK);
    return true;
}
static bool set_6_c(CPU *cpu)   // 0xF1 (- - - -) 2M
{
    cpu->reg.C = (cpu->reg.C | BIT_6_MASK);
    return true;
}
static bool set_6_d(CPU *cpu)   // 0xF2 (- - - -) 2M
{
    cpu->reg.D = (cpu->reg.D | BIT_6_MASK);
    return true;
}
static bool set_6_e(CPU *cpu)   // 0xF3 (- - - -) 2M
{
    cpu->reg.E = (cpu->reg.E | BIT_6_MASK);
    return true;
}
static bool set_6_h(CPU *cpu)   // 0xF4 (- - - -) 2M
{
    cpu->reg.H = (cpu->reg.H | BIT_6_MASK);
    return true;
}
static bool set_6_l(CPU *cpu)   // 0xF5 (- - - -) 2M
{
    cpu->reg.L = (cpu->reg.L | BIT_6_MASK);
    return true;
}
static bool set_6_hl(CPU *cpu)  // 0xF6 (- - - -) 4M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.address = getDR(cpu, HL_REG);
            cpu->ins.low = read_memory(cpu->mem, cpu->ins.address);
            return false;
        
        case 3:
            uint8_t result = (cpu->ins.low | BIT_6_MASK); 
            write_memory(cpu->mem, cpu->ins.address, result);
            return true;
    }
    
    return true;
}
static bool set_6_a(CPU *cpu)   // 0xF7 (- - - -) 2M
{
    cpu->reg.A = (cpu->reg.A | BIT_6_MASK);
    return true;
}
static bool set_7_b(CPU *cpu)   // 0xF8 (- - - -) 2M
{
    cpu->reg.B = (cpu->reg.B | BIT_7_MASK);
    return true;
}
static bool set_7_c(CPU *cpu)   // 0xF9 (- - - -) 2M
{
    cpu->reg.C = (cpu->reg.C | BIT_7_MASK);
    return true;
}
static bool set_7_d(CPU *cpu)   // 0xFA (- - - -) 2M
{
    cpu->reg.D = (cpu->reg.D | BIT_7_MASK);
    return true;
}
static bool set_7_e(CPU *cpu)   // 0xFB (- - - -) 2M
{
    cpu->reg.E = (cpu->reg.E | BIT_7_MASK);
    return true;
}
static bool set_7_h(CPU *cpu)   // 0xFC (- - - -) 2M
{
    cpu->reg.H = (cpu->reg.H | BIT_7_MASK);
    return true;
}
static bool set_7_l(CPU *cpu)   // 0xFD (- - - -) 2M
{
    cpu->reg.L = (cpu->reg.L | BIT_7_MASK);
    return true;
}
static bool set_7_hl(CPU *cpu)  // 0xFE (- - - -) 4M
{
    switch(cpu->ins.duration)
    {
        case 1:
            return false;

        case 2:
            cpu->ins.address = getDR(cpu, HL_REG);
            cpu->ins.low = read_memory(cpu->mem, cpu->ins.address);
            return false;
        
        case 3:
            uint8_t result = (cpu->ins.low | BIT_7_MASK); 
            write_memory(cpu->mem, cpu->ins.address, result);
            return true;
    }
    
    return true;
}
static bool set_7_a(CPU *cpu)   // 0xFF (- - - -) 2M
{
    cpu->reg.A = (cpu->reg.A | BIT_7_MASK);
    return true;
}

const OpcodeHandler prefix_opcode_table[256] = 
{
    /*              ROW 1               */
    [0x00] = rlc_b,   
    [0x01] = rlc_c,
    [0x02] = rlc_d,
    [0x03] = rlc_e,
    [0x04] = rlc_h,
    [0x05] = rlc_l,
    [0x06] = rlc_hl,
    [0x07] = rlc_a,
    [0x08] = rrc_b,
    [0x09] = rrc_c,
    [0x0A] = rrc_d,
    [0x0B] = rrc_e,
    [0x0C] = rrc_h,
    [0x0D] = rrc_l,
    [0x0E] = rrc_hl,
    [0x0F] = rrc_a,
    /*              ROW 2                */
    [0x10] = rl_b,   
    [0x11] = rl_c,
    [0x12] = rl_d,
    [0x13] = rl_e,
    [0x14] = rl_h,
    [0x15] = rl_l,
    [0x16] = rl_hl,
    [0x17] = rl_a,
    [0x18] = rr_b,
    [0x19] = rr_c,
    [0x1A] = rr_d,
    [0x1B] = rr_e,
    [0x1C] = rr_h,
    [0x1D] = rr_l,
    [0x1E] = rr_hl,
    [0x1F] = rr_a,
    /*              ROW 3                */
    [0x20] = sla_b,   
    [0x21] = sla_c,
    [0x22] = sla_d,
    [0x23] = sla_e,
    [0x24] = sla_h,
    [0x25] = sla_l,
    [0x26] = sla_hl,
    [0x27] = sla_a,
    [0x28] = sra_b,
    [0x29] = sra_c,
    [0x2A] = sra_d,
    [0x2B] = sra_e,
    [0x2C] = sra_h,
    [0x2D] = sra_l,
    [0x2E] = sra_hl,
    [0x2F] = sra_a,
    /*              ROW 4                */
    [0x30] = swap_b,   
    [0x31] = swap_c,
    [0x32] = swap_d,
    [0x33] = swap_e,
    [0x34] = swap_h,
    [0x35] = swap_l,
    [0x36] = swap_hl,
    [0x37] = swap_a,
    [0x38] = srl_b,
    [0x39] = srl_c,
    [0x3A] = srl_d,
    [0x3B] = srl_e,
    [0x3C] = srl_h,
    [0x3D] = srl_l,
    [0x3E] = srl_hl,
    [0x3F] = srl_a,
    /*              ROW 5                */
    [0x40] = bit_0_b,   
    [0x41] = bit_0_c,
    [0x42] = bit_0_d,
    [0x43] = bit_0_e,
    [0x44] = bit_0_h,
    [0x45] = bit_0_l,
    [0x46] = bit_0_hl,
    [0x47] = bit_0_a,
    [0x48] = bit_1_b,
    [0x49] = bit_1_c,
    [0x4A] = bit_1_d,
    [0x4B] = bit_1_e,
    [0x4C] = bit_1_h,
    [0x4D] = bit_1_l,
    [0x4E] = bit_1_hl,
    [0x4F] = bit_1_a,
    /*              ROW 6                */
    [0x50] = bit_2_b,   
    [0x51] = bit_2_c,
    [0x52] = bit_2_d,
    [0x53] = bit_2_e,
    [0x54] = bit_2_h,
    [0x55] = bit_2_l,
    [0x56] = bit_2_hl,
    [0x57] = bit_2_a,
    [0x58] = bit_3_b,
    [0x59] = bit_3_c,
    [0x5A] = bit_3_d,
    [0x5B] = bit_3_e,
    [0x5C] = bit_3_h,
    [0x5D] = bit_3_l,
    [0x5E] = bit_3_hl,
    [0x5F] = bit_3_a,
    /*              ROW 7                */
    [0x60] = bit_4_b,   
    [0x61] = bit_4_c,
    [0x62] = bit_4_d,
    [0x63] = bit_4_e,
    [0x64] = bit_4_h,
    [0x65] = bit_4_l,
    [0x66] = bit_4_hl,
    [0x67] = bit_4_a,
    [0x68] = bit_5_b,
    [0x69] = bit_5_c,
    [0x6A] = bit_5_d,
    [0x6B] = bit_5_e,
    [0x6C] = bit_5_h,
    [0x6D] = bit_5_l,
    [0x6E] = bit_5_hl,
    [0x6F] = bit_5_a,
    /*              ROW 8                */
    [0x70] = bit_6_b,   
    [0x71] = bit_6_c,
    [0x72] = bit_6_d,
    [0x73] = bit_6_e,
    [0x74] = bit_6_h,
    [0x75] = bit_6_l,
    [0x76] = bit_6_hl,
    [0x77] = bit_6_a,
    [0x78] = bit_7_b,
    [0x79] = bit_7_c,
    [0x7A] = bit_7_d,
    [0x7B] = bit_7_e,
    [0x7C] = bit_7_h,
    [0x7D] = bit_7_l,
    [0x7E] = bit_7_hl,
    [0x7F] = bit_7_a,
    /*              ROW 9                */
    [0x80] = res_0_b,   
    [0x81] = res_0_c,
    [0x82] = res_0_d,
    [0x83] = res_0_e,
    [0x84] = res_0_h,
    [0x85] = res_0_l,
    [0x86] = res_0_hl,
    [0x87] = res_0_a,
    [0x88] = res_1_b,
    [0x89] = res_1_c,
    [0x8A] = res_1_d,
    [0x8B] = res_1_e,
    [0x8C] = res_1_h,
    [0x8D] = res_1_l,
    [0x8E] = res_1_hl,
    [0x8F] = res_1_a,
    /*              ROW 10                */
    [0x90] = res_2_b,   
    [0x91] = res_2_c,
    [0x92] = res_2_d,
    [0x93] = res_2_e,
    [0x94] = res_2_h,
    [0x95] = res_2_l,
    [0x96] = res_2_hl,
    [0x97] = res_2_a,
    [0x98] = res_3_b,
    [0x99] = res_3_c,
    [0x9A] = res_3_d,
    [0x9B] = res_3_e,
    [0x9C] = res_3_h,
    [0x9D] = res_3_l,
    [0x9E] = res_3_hl,
    [0x9F] = res_3_a,
    /*              ROW 11                */
    [0xA0] = res_4_b,   
    [0xA1] = res_4_c,
    [0xA2] = res_4_d,
    [0xA3] = res_4_e,
    [0xA4] = res_4_h,
    [0xA5] = res_4_l,
    [0xA6] = res_4_hl,
    [0xA7] = res_4_a,
    [0xA8] = res_5_b,
    [0xA9] = res_5_c,
    [0xAA] = res_5_d,
    [0xAB] = res_5_e,
    [0xAC] = res_5_h,
    [0xAD] = res_5_l,
    [0xAE] = res_5_hl,
    [0xAF] = res_5_a,
    /*              ROW 12                */
    [0xB0] = res_6_b,   
    [0xB1] = res_6_c,
    [0xB2] = res_6_d,
    [0xB3] = res_6_e,
    [0xB4] = res_6_h,
    [0xB5] = res_6_l,
    [0xB6] = res_6_hl,
    [0xB7] = res_6_a,
    [0xB8] = res_7_b,
    [0xB9] = res_7_c,
    [0xBA] = res_7_d,
    [0xBB] = res_7_e,
    [0xBC] = res_7_h,
    [0xBD] = res_7_l,
    [0xBE] = res_7_hl,
    [0xBF] = res_7_a,
    /*              ROW 13                */
    [0xC0] = set_0_b,   
    [0xC1] = set_0_c,
    [0xC2] = set_0_d,
    [0xC3] = set_0_e,
    [0xC4] = set_0_h,
    [0xC5] = set_0_l,
    [0xC6] = set_0_hl,
    [0xC7] = set_0_a,
    [0xC8] = set_1_b,
    [0xC9] = set_1_c,
    [0xCA] = set_1_d,
    [0xCB] = set_1_e,
    [0xCC] = set_1_h,
    [0xCD] = set_1_l,
    [0xCE] = set_1_hl,
    [0xCF] = set_1_a,
    /*              ROW 14                */
    [0xD0] = set_2_b,   
    [0xD1] = set_2_c,
    [0xD2] = set_2_d,
    [0xD3] = set_2_e,
    [0xD4] = set_2_h,
    [0xD5] = set_2_l,
    [0xD6] = set_2_hl,
    [0xD7] = set_2_a,
    [0xD8] = set_3_b,
    [0xD9] = set_3_c,
    [0xDA] = set_3_d,
    [0xDB] = set_3_e,
    [0xDC] = set_3_h,
    [0xDD] = set_3_l,
    [0xDE] = set_3_hl,
    [0xDF] = set_3_a,
    /*              ROW 15                */
    [0xE0] = set_4_b,   
    [0xE1] = set_4_c,
    [0xE2] = set_4_d,
    [0xE3] = set_4_e,
    [0xE4] = set_4_h,
    [0xE5] = set_4_l,
    [0xE6] = set_4_hl,
    [0xE7] = set_4_a,
    [0xE8] = set_5_b,
    [0xE9] = set_5_c,
    [0xEA] = set_5_d,
    [0xEB] = set_5_e,
    [0xEC] = set_5_h,
    [0xED] = set_5_l,
    [0xEE] = set_5_hl,
    [0xEF] = set_5_a,
    /*              ROW 16                */
    [0xF0] = set_6_b,   
    [0xF1] = set_6_c,
    [0xF2] = set_6_d,
    [0xF3] = set_6_e,
    [0xF4] = set_6_h,
    [0xF5] = set_6_l,
    [0xF6] = set_6_hl,
    [0xF7] = set_6_a,
    [0xF8] = set_7_b,
    [0xF9] = set_7_c,
    [0xFA] = set_7_d,
    [0xFB] = set_7_e,
    [0xFC] = set_7_h,
    [0xFD] = set_7_l,
    [0xFE] = set_7_hl,
    [0xFF] = set_7_a,
};

static void reset_ins(CPU *cpu)
{
    cpu->ins. address = cpu->reg.PC;
    cpu->ins.duration =           0;
    cpu->ins.  length =           1;
    cpu->ins.     low =           0;
    cpu->ins.    high =           0;
    cpu->ins.  opcode =           0;
    cpu->ins.   label =       "N/A";
    cpu->ins.executed =       false;
    cpu->ins. handler =         nop;
}

// Interrupt Handling

void request_interrupt(CPU *cpu, InterruptCode interrupt)
{
    uint8_t byte = (*cpu->reg.IFR | interrupt);
    write_memory(cpu->mem, IFR, byte);
}

static bool service_interrupts(CPU *cpu)
{
    uint8_t pending = get_pending_interrupts(cpu);
    if ((!cpu->ime) || (pending == 0)) return false;

    cpu->ime = false;
    reset_ins(cpu); // Normalize
    cpu->ins.handler = int_exec;
    encode_interrupt(cpu, pending);

    return true;
}

// Instruction Execution

static void check_pending_interrupts(CPU *cpu)
{
    if (cpu->halted) // Need to unhalt if interrupt pending.
    {
        uint8_t pending = get_pending_interrupts(cpu);
        cpu->halted = (pending == 0); // No reason to unhalt.
    }
}

static void next_ins(CPU *cpu)
{
    reset_ins(cpu);
    cpu->ins. opcode = fetch(cpu);
    if (cpu->ins.cb_prefixed)
    {
        cpu->      ins.label = cb_opcode_word[cpu->ins.opcode];
        cpu->    ins.handler = prefix_opcode_table[cpu->ins.opcode];
        cpu->ins.cb_prefixed = false;
        return;
    }
    cpu->ins.  label = opcode_word[cpu->ins.opcode];
    cpu->ins.handler = opcode_table[cpu->ins.opcode];
}

static void check_ins(CPU *cpu)
{
    if (cpu->ins.executed)
    {
        if (cpu->ins.cb_prefixed)
        {
            next_ins(cpu);
            return;
        }

        if (service_interrupts(cpu)) return;

        next_ins(cpu);
    }
}

static void execute_ins(CPU *cpu)
{
    cpu->ins.duration += 1; // Steps through micro-instructions
    cpu->ins.executed = cpu->ins.handler(cpu);

    if (cpu->ime_scheduled && cpu->ins.executed)
    {
        cpu->ime_delay--;

        if (cpu->ime_delay == 0)
        {
            cpu->          ime =  true;
            cpu->ime_scheduled = false;
        }
    }
}

void machine_cycle(CPU *cpu)
{
    check_pending_interrupts(cpu); // Unhalts If Interrupt Pending
    
    if (!cpu->running || cpu->halted) return;

    check_ins(cpu);
    execute_ins(cpu); // Continue Execution. 
}

void reset_cpu(CPU *cpu)
{
    cpu->         halted = false;
    cpu->halt_bug_active = false;
    cpu->            ime = false;
    cpu->  ime_scheduled = false;
    cpu->        running = false;
    cpu->  speed_enabled = false;

    cpu->reg.PC =       0x0100;
    cpu->reg.SP = HIGH_RAM_END;
}

void start_cpu(CPU *cpu)
{
    cpu->running = true;
}

void stop_cpu(CPU *cpu)
{
    cpu->running = false;
}

void link_cpu(CPU *cpu, GbcEmu *emu)
{
    cpu-> cart = emu->cart;
    cpu->  mem = emu->mem;

    cpu->reg.IER = &(emu->mem->memory[IER]);
    cpu->reg.IFR = &(emu->mem->memory[IFR]);

    cpu->ins.cb_prefixed = false;
    next_ins(cpu); // Start feeding the tape here...
}

CPU *init_cpu()
{
    CPU *cpu = (CPU*) malloc(sizeof(CPU));

    reset_cpu(cpu);

    return cpu;
}

void tidy_cpu(CPU **cpu)
{
    free(*cpu); 
    *cpu = NULL;
}




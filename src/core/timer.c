#include <stdio.h>
#include <stdlib.h>

#include "core/emulator.h"
#include "core/cpu.h"
#include "core/mmu.h"
#include "core/apu.h"
#include "core/ppu.h"
#include "core/timer.h"

#include "util/common.h"

static inline bool get_current_sys_bit(EmuTimer *timer)
{
    uint8_t  select = (*timer->tac) & LOWER_2_MASK;
    uint8_t sys_bit = (timer->sys >> sys_shift_table[select]) & BIT_0_MASK;
    return (sys_bit != 0);
}

static inline bool get_current_apu_bit(EmuTimer *timer)
{
    bool apu_bit = timer->cpu->speed_enabled ?
    ((timer->sys & BIT_11_MASK) != 0) : ((timer->sys & BIT_10_MASK) != 0);
    return apu_bit;
}

static inline bool tac_enabled(EmuTimer *timer)
{
    return (((*timer->tac) & BIT_2_MASK) != 0);
}

static inline void sync_div(EmuTimer *timer)
{
    *timer->div_ = (timer->sys >> 6) & LOWER_BYTE_MASK;
}

static inline void sync_sys(EmuTimer *timer)
{
    timer->sys = (*timer->div_ << 6) | (timer->sys * LOWER_6_MASK);
}

static inline bool inc_tima(EmuTimer *timer)
{
    (*timer->tima) += 1;
    return (*timer->tima == 0);
}

static inline void check_tima_overflow(EmuTimer *timer)
{
    switch(timer->tofs)
    {
        case PRE_CYCLE_A:
            timer->tofs = CYCLE_A;
            return;

        case CYCLE_A:
            // Defines cycle in which [TIMA]  = $00
            timer->tofs = CYCLE_B;
            return;

        case CYCLE_B:
            // Defines cycle in which [TIMA] <- [TMA]
            (*timer->tima) = (*timer->tma);
            request_interrupt(timer->cpu, TIMER_INTERRUPT_CODE);
            timer->tofs = NOT_OVERFLOWING;
            break;

        case NOT_OVERFLOWING: 
            // Nothing to do...
            return;
    }
}

static inline void check_apu_event(EmuTimer *timer, bool curr_sys_bit)
{
    timer->prev_sys_bit = curr_sys_bit;

    bool curr_apu_bit = get_current_apu_bit(timer);

    if (timer->prev_apu_bit && !curr_apu_bit)
       div_apu_event(timer->apu);

    timer->prev_apu_bit = curr_apu_bit;
}

static bool write_sys(EmuTimer *timer, uint16_t value)
{
    bool tima_overflow = false;

    timer->sys = value & LOWER_14_MASK;
    sync_div(timer);
    bool curr_sys_bit = get_current_sys_bit(timer);

    if (tac_enabled(timer) && timer->prev_sys_bit && !curr_sys_bit)
        tima_overflow  = inc_tima(timer);

    check_apu_event(timer, curr_sys_bit);

    timer->prev_sys_bit = curr_sys_bit; 

    return tima_overflow;
}

static inline bool inc_sys(EmuTimer *timer)
{
    return write_sys(timer, (timer->sys + 1));
}

// Register Writing

static void clear_sys(EmuTimer *timer)
{
   bool tima_overflow = write_sys(timer, 0);
   if (tima_overflow) timer->tofs = CYCLE_B;
}

static void write_tac(EmuTimer *timer, uint8_t value)
{
    bool prev_enable = tac_enabled(timer);

    (*timer->tac) = value;

    bool  curr_enable = tac_enabled(timer); // Did the enable bit cause a pulse?
    bool curr_sys_bit = get_current_sys_bit(timer); // Did switching sys-bit-sel cause a pulse?

    if (prev_enable && timer->prev_sys_bit) // Circuit was primed for pulse.
    {
        if (!curr_enable || !curr_sys_bit)
        {
            bool tima_overflow = inc_tima(timer);
            if (tima_overflow) timer->tofs = CYCLE_B;    
        }
    }
}

static void write_tima(EmuTimer *timer, uint8_t value)
{
    if (timer->tofs == CYCLE_A)
    {
        (*timer->tima) = value;
        timer->tofs = NOT_OVERFLOWING;
        return;
    }

    if (timer->tofs == CYCLE_B) return;

    (*timer->tima) = value;
}  

static void write_tma(EmuTimer *timer, uint8_t value)
{
    (*timer->tma) = value;
}

void write_timer_register(EmuTimer *timer, uint16_t address, uint8_t value)
{
    switch(address)
    {
        case DIV:  clear_sys(timer);         break;
        case TIMA: write_tima(timer, value); break;
        case TMA:  write_tma(timer, value);  break;
        case TAC:  write_tac(timer, value);  break;
    }
}

// Drivers

bool system_clock_pulse(EmuTimer *timer)
{
    static uint8_t dots = 4;

    bool frame_ready = false;

    check_hdma_transfer(timer->mem);
    apu_dot(timer->apu);
    frame_ready = ppu_dot(timer->ppu);

    dots--; // Cycle Divider
    if (dots != 0) return frame_ready;
    dots = timer->cpu->speed_enabled ? 2 : 4;

    // Machine Cycle Occurs

    check_dma_transfer(timer->mem);
    check_tima_overflow(timer);

    if (timer->tofs == CYCLE_B) 
        (*timer->tima) = (*timer->tma);

    if (!timer->mem->hdma.bytes_transferring)
        machine_cycle(timer->cpu);

    if (inc_sys(timer)) 
        timer->tofs = PRE_CYCLE_A;

    return frame_ready;
}

bool machine_clock_pulse(EmuTimer *timer)
{
    static uint8_t dots = 0; 

    bool frame_ready = false;

    dots = timer->cpu->speed_enabled ? 2 : 4;

    check_dma_transfer(timer->mem); // DMA Transfer
    check_tima_overflow(timer);

    for (int i = 0; i < dots; i++)
    {
        check_hdma_transfer(timer->mem);
        apu_dot(timer->apu);
        frame_ready |= ppu_dot(timer->ppu);
    }

    if (timer->tofs == CYCLE_B) 
        (*timer->tima) = (*timer->tma);

    if (!timer->mem->hdma.bytes_transferring)
        machine_cycle(timer->cpu);

    if (inc_sys(timer)) 
        timer->tofs = PRE_CYCLE_A;

    return frame_ready;
}

// Linking and Initialization

void link_timer(EmuTimer *timer, GbcEmu *emu)
{
    timer->div_ = &(emu->mem->memory[DIV]);
    timer-> tac = &(emu->mem->memory[TAC]);
    timer->tima = &(emu->mem->memory[TIMA]);
    timer-> tma = &(emu->mem->memory[TMA]);
    
    timer->cart = emu->cart;
    timer-> cpu = emu->cpu;
    timer-> mem = emu->mem;
    timer-> apu = emu->apu;
    timer-> ppu = emu->ppu;

    sync_sys(timer);
}

EmuTimer *init_timer()
{
    EmuTimer *timer = (EmuTimer*) malloc(sizeof(EmuTimer));

    timer->        tofs = NOT_OVERFLOWING;
    timer->prev_apu_bit =               0;
    timer->prev_sys_bit =               0;
    timer->         sys =               0;
    
    return timer;
}

void tidy_timer(EmuTimer **timer)
{
    free(*timer); 
    *timer = NULL;
}
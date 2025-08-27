#ifndef TIMER_H
#define TIMER_H

#include <stdbool.h>
#include <stdint.h>

#define SYSTEM_CLOCK_FREQUENCY (uint64_t) 4194304

typedef struct Cartridge Cartridge;
typedef struct CPU CPU;
typedef struct EmuMemory EmuMemory;
typedef struct APU APU;
typedef struct PPU PPU;

static const uint8_t sys_shift_table[4] = 
{
    7, // TAC = 0b00 → 4096 Hz
    1, // TAC = 0b01 → 262144 Hz
    3, // TAC = 0b10 → 65536 Hz
    5  // TAC = 0b11 → 16384 Hz
};

typedef enum 
{
    PRE_CYCLE_A,
    CYCLE_A,
    CYCLE_B,
    NOT_OVERFLOWING,

} TimaOverflowState; 

typedef struct EmuTimer
{
    uint16_t          sys;
    uint8_t         *div_;
    uint8_t          *tac;
    uint8_t          *tma;
    uint8_t         *tima;

    TimaOverflowState tofs;

    bool     prev_sys_bit;
    bool     prev_apu_bit;

    uint32_t          dot;

    Cartridge       *cart;
    CPU              *cpu;
    EmuMemory        *mem;
    APU              *apu;
    PPU              *ppu;
    
} EmuTimer;

char *get_emu_time(EmuTimer *timer, char *buffer, size_t size);

void write_timer_register(EmuTimer *timer, uint16_t address, uint8_t value);

bool system_clock_pulse(EmuTimer *timer);

bool machine_clock_pulse(EmuTimer *timer);

void link_timer(EmuTimer *timer, GbcEmu *emu);

char *get_emu_time(EmuTimer *timer, char *buffer, size_t size);

EmuTimer *init_timer();

void tidy_timer(EmuTimer **timer);

#endif
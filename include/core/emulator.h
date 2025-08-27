#ifndef EMULATOR_H
#define EMULATOR_H

#include <stdbool.h>
#include <stdint.h>

typedef struct Cartridge Cartridge;
typedef struct CPU CPU;
typedef struct EmuMemory EmuMemory;
typedef struct EmuTimer EmuTimer;
typedef struct APU APU;
typedef struct PPU PPU;

typedef enum
{
    DMG,
    CGB

} EmulationType;

typedef enum
{
    A_BUTTON_MASK      = 0b00000001,
    B_BUTTON_MASK      = 0b00000010,
    SELECT_BUTTON_MASK = 0b00000100,
    START_BUTTON_MASK  = 0b00001000,
    DOWN_BUTTON_MASK   = 0b00001000,
    UP_BUTTON_MASK     = 0b00000100,
    LEFT_BUTTON_MASK   = 0b00000010,
    RIGHT_BUTTON_MASK  = 0b00000001

} JoypadMask;

typedef struct Joypad
{
    bool             A;
    bool             B; 
    bool        SELECT; 
    bool         START;

    bool         RIGHT; 
    bool          LEFT; 
    bool            UP; 
    bool          DOWN;

    bool turbo_enabled;
    
} Joypad;

typedef struct GbcEmu
{
    Joypad joypad;

    Cartridge *cart;
    CPU        *cpu;
    EmuMemory  *mem;
    EmuTimer *timer;
    APU        *apu;
    PPU        *ppu;

    volatile bool running;

} GbcEmu;

void load_cartridge(GbcEmu *emu, const char *file_path, const char *file_name);

void swap_cartridge(GbcEmu *emu, const char *file_path, const char *file_name);

GbcEmu *init_emulator();

void tidy_emulator(GbcEmu **emu);

#endif
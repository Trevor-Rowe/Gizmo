#include <stdlib.h>

#include "core/cart.h"
#include "core/cpu.h"
#include "core/mmu.h"
#include "core/timer.h"
#include "core/apu.h"
#include "core/ppu.h"
#include "core/emulator.h"

static void dmg_bios(GbcEmu *emu)
{
    // PPU Values
    write_memory(emu->mem, LCDC, 0x91);
    write_memory(emu->mem,  SCX, 0x00);
    write_memory(emu->mem,  SCY, 0x00);
    write_memory(emu->mem,   WX, 0x00);
    write_memory(emu->mem,   WY, 0x00);
    write_memory(emu->mem,  BGP, 0xFC);
    write_memory(emu->mem, STAT, 0x00);
    write_memory(emu->mem, OBP0, 0x00);
    write_memory(emu->mem, OBP1, 0x00);

    // APU Values
    write_memory(emu->mem, NR52, 0xF1);
    write_memory(emu->mem, NR51, 0xF3);
    write_memory(emu->mem, NR50, 0x77);

    // CPU Values
    write_memory(emu->mem, IER, 0x00);
    write_memory(emu->mem, IFR, 0x00);
    emu->cpu->reg.PC = 0x0100;
    emu->cpu->reg.SP = 0xFFFE;
    emu->cpu->reg.A = 0x01;
    emu->cpu->reg.F = 0xB0;
    emu->cpu->reg.B = 0x00;
    emu->cpu->reg.C = 0x13;
    emu->cpu->reg.D = 0x00;
    emu->cpu->reg.E = 0xD8;
    emu->cpu->reg.H = 0x01;
    emu->cpu->reg.L = 0x4D;
    
    // BIOS
    write_memory(emu->mem, BIOS, 0x01);
}

static void cgb_bios(GbcEmu *emu)
{
    dmg_bios(emu);

    write_memory(emu->mem, KEY1, 0x00);
    write_memory(emu->mem,  VBK, 0x00);
    write_memory(emu->mem, SVBK, 0x01);

    emu->cpu->reg.A = 0x11;
}

static void link_emulator(GbcEmu *emu)
{
    link_mmu(emu->mem, emu);
    link_timer(emu->timer, emu);
    link_cpu(emu->cpu, emu);
    link_apu(emu->apu, emu);
    link_ppu(emu->ppu, emu);
}

static void empty_cartridge(GbcEmu *emu)
{
    if (emu->cart != NULL)
    {
        tidy_cartridge(&emu->cart);
        emu->cart = NULL;
    }

    if (emu->cpu != NULL)
    {
        tidy_cpu(&emu->cpu);
        emu->cpu = NULL;
    }

    if (emu->mem != NULL)
    {
        tidy_memory(&emu->mem);
        emu->mem = NULL;
    }

    if (emu->timer != NULL)
    {
        tidy_timer(&emu->timer);
        emu->timer = NULL;
    }

    if (emu->apu != NULL)
    {
        tidy_apu(&emu->apu);
        emu->apu = NULL;
    }

    if (emu->ppu != NULL)
    {
        tidy_ppu(&emu->ppu);
        emu->ppu = NULL;
    }
}

void load_cartridge(GbcEmu *emu, const char *file_path, const char *file_name)
{
    emu-> cart = init_cartridge(file_path, file_name);
    emu->  cpu = init_cpu();
    emu->  mem = init_memory();
    emu->timer = init_timer();
    emu->  apu = init_apu();
    emu->  ppu = init_ppu();

    link_emulator(emu);
    load_cartridge_save(emu->cart);

    emu->cart->is_gbc ? cgb_bios(emu) : dmg_bios(emu);
}

void swap_cartridge(GbcEmu *emu, const char *file_path, const char *file_name)
{
    empty_cartridge(emu);
    load_cartridge(emu, file_path, file_name);
}

GbcEmu *init_emulator()
{
    GbcEmu *emu = (GbcEmu*) malloc(sizeof(GbcEmu));

    return emu; 
}

void tidy_emulator(GbcEmu **emu)
{
    GbcEmu *temp = *emu;

    tidy_cartridge(&temp->cart);
    tidy_cpu(&temp->cpu);
    tidy_memory(&temp->mem);
    tidy_timer(&temp->timer);
    tidy_apu(&temp->apu);
    tidy_ppu(&temp->ppu);

    free(*emu); 
    *emu = NULL;
}
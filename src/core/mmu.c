#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "core/emulator.h"
#include "core/cart.h"
#include "core/cpu.h"
#include "core/timer.h"
#include "core/apu.h"
#include "core/ppu.h"
#include "core/mmu.h"

#include "util/common.h"

typedef uint8_t (*MemoryReadHandler)(EmuMemory*, uint16_t);
typedef void (*MemoryWriteHandler)(EmuMemory*, uint16_t, uint8_t);

MemoryReadHandler   memory_read_table[MEMORY_SIZE];
MemoryWriteHandler memory_write_table[MEMORY_SIZE];

static uint8_t memory_mask_table[MEMORY_SIZE] = {0};


// I/O API


uint8_t read_vram_bank(EmuMemory *mem, uint8_t bank, uint16_t address)
{
    address -= VRAM_START;

    if (bank == 0) return mem->vram[0][address];

    return mem->vram[1][address];
}

uint8_t read_cram(EmuMemory *mem, bool is_obj, uint8_t palette_index, uint8_t color_id, uint8_t index)
{
    uint8_t base   = is_obj ? 0x40 : 0x00;
    uint8_t offset = (palette_index << 3) | (color_id << 1);
    return mem->cram[base + offset + index];
}

uint8_t read_memory(EmuMemory *mem, uint16_t address)
{
    return (memory_read_table[address](mem, address) | memory_mask_table[address]);
}

void write_memory(EmuMemory *mem, uint16_t address, uint8_t value)
{ 
    memory_write_table[address](mem, address, value); 
}

void check_dma_transfer(EmuMemory *mem)
{
    if (!mem->dma.active)
        return;

    if (mem->dma.length == (DMA_DURATION - 1))
    {
        mem-> oam_read_blocked = true;
        mem->oam_write_blocked = true;
    }

    if (mem->dma.length > 160)
    {
        mem->dma.length--;
        return;
    }

    uint8_t byte = read_memory(mem, mem->dma.src);
    mem->dma.src++;
    mem->oam[mem->dma.dst - OAM_START] = byte;
    mem->dma.dst++; 

    mem->dma.length--;

    if (mem->dma.length == 0)
    {
        mem-> oam_read_blocked = false;
        mem->oam_write_blocked = false;
        mem->       dma.active = false;
    }
}

void check_hdma_trigger(EmuMemory *mem)
{
    if (!mem->hdma.active || (mem->hdma.mode != HBLANK_HDMA) || !mem->cart->is_gbc)
        return;
    
    mem->hdma.bytes_transferring = true;
    mem->hdma.bytes_transferred  =    0;
}

void check_hdma_transfer(EmuMemory *mem)
{
    if (!mem->hdma.active || !mem->hdma.bytes_transferring)
        return;

    mem->hdma.counter++;
    if (mem->hdma.counter < 2) return;
    mem->hdma.counter = 0;

    uint8_t byte = read_memory(mem, mem->hdma.src++);
    write_memory(mem, mem->hdma.dst++, byte);
    mem->hdma.bytes_transferred++;
    mem->hdma.length--;

    if ((mem->hdma.length == 0) || (mem->hdma.dst > VRAM_END))
    {
        mem->hdma.active             = false;
        mem->hdma.bytes_transferring = false;
        mem->memory[HDMA5]           =  0xFF;
        return;
    }

    if ((mem->hdma.mode == HBLANK_HDMA) && (mem->hdma.bytes_transferred >= 16))
    {
        mem->hdma.bytes_transferring = false;
        mem->hdma.bytes_transferred  =     0;
    }

    mem->memory[HDMA5] &= BIT_7_MASK;
    mem->memory[HDMA5] |= ((mem->hdma.length / 0x10) - 1) & LOWER_7_MASK;
}

// HIGH-LEVEL MEMORY


static uint8_t default_read(EmuMemory *mem, uint16_t address)
{
    return mem->memory[address];
}

static void default_write(EmuMemory *mem, uint16_t address, uint8_t value)
{
    mem->memory[address] = value;
}

// [$0000 - $7FFF] ROM, [$A000 - $BFFF] Cartridge RAM

static uint8_t read_cart_memory(EmuMemory *mem, uint16_t address)
{
    return read_cartridge(mem->cart, address);
}

static void write_cart_memory(EmuMemory *mem, uint16_t address, uint8_t value)
{
    write_cartridge(mem->cart, address, value);
}

// [$8000 - $9FFF] VRAM

static uint8_t read_vram(EmuMemory *mem, uint16_t address)
{
   if (mem->vram_read_blocked) return OPEN_BUS;

    address -= VRAM_START;
    uint8_t bank = mem->memory[VBK] & BIT_0_MASK;
    return mem->vram[bank][address];
}

static void write_vram(EmuMemory *mem, uint16_t address, uint8_t value)
{   
    if (mem->vram_write_blocked) return;

    address -= VRAM_START;
    uint8_t bank = mem->memory[VBK] & BIT_0_MASK;
    mem->vram[bank][address] = value;
}

// [$C000 - $CFFF] Static WRAM

static uint8_t read_static_wram(EmuMemory *mem, uint16_t address)
{
    address -= WRAM_STATIC_START;
    return mem->wram[0][address];
}

static void write_static_wram(EmuMemory *mem, uint16_t address, uint8_t value)
{
    address -= WRAM_STATIC_START;
    mem->wram[0][address] = value;
}

// [$D000 - $DFFF] Dynamic WRAM

static uint8_t read_dynamic_wram(EmuMemory *mem, uint16_t address)
{
    address -= WRAM_DYNAMIC_START;
    uint8_t svbk = mem->memory[SVBK] & LOWER_3_MASK;
    if (svbk == 0) svbk = 1;
    return mem->wram[svbk][address];
}

static void write_dynamic_wram(EmuMemory *mem, uint16_t address, uint8_t value)
{
    address -= WRAM_DYNAMIC_START;
    uint8_t svbk = mem->memory[SVBK] & LOWER_3_MASK;
    if (svbk == 0) svbk = 1;
    mem->wram[svbk][address] = value;
} 

// [$E000 - $FDFF] Echo of Static WRAM

static uint8_t read_echo_ram(EmuMemory *mem, uint16_t address)
{
    return read_memory(mem, (address - 0x2000));
}

static void write_echo_ram(EmuMemory *mem, uint16_t address, uint8_t value)
{
    write_memory(mem, (address - 0x2000), value);
}

// [$FE00 - $FE9F] OAM

static uint8_t read_oam(EmuMemory *mem, uint16_t address)
{
    if (mem->oam_read_blocked) return OPEN_BUS;

    address -= OAM_START;

    return mem->oam[address];
}

static void write_oam(EmuMemory *mem, uint16_t address, uint8_t value)
{
    if (mem->oam_write_blocked) return;

    address -= OAM_START;

    mem->oam[address] = value;
}


// I/O HANDLING


// Cartridge

static void write_bios(EmuMemory *mem, uint16_t address, uint8_t value)
{
    if (mem->cart->bios_locked) return;

    mem->memory[BIOS] = 1;

    mem->cart->bios_locked = true;
}

// PPU

static void write_ppu(EmuMemory *mem, uint16_t address, uint8_t value)
{
    write_ppu_register(mem->ppu, address, value);
}

static uint8_t read_bcpd(EmuMemory *mem, uint16_t address)
{
    return mem->cram[(mem->memory[BCPS] & LOWER_6_MASK)];
}

static void write_bcpd(EmuMemory *mem, uint16_t address, uint8_t value)
{
    uint8_t index = mem->memory[BCPS] & LOWER_6_MASK;
    mem->cram[index] = value;
    uint8_t inc_index = (index + 1) & LOWER_6_MASK;
    
    if(mem->memory[BCPS] & BIT_7_MASK)
    {
        mem->memory[BCPS] = (mem->memory[BCPS] & BIT_7_MASK) | (inc_index);
    }
}

static uint8_t read_ocpd(EmuMemory *mem, uint16_t address)
{
    return mem->cram[(mem->memory[OCPS] & LOWER_6_MASK) + 0x40];
}

static void write_ocpd(EmuMemory *mem, uint16_t address, uint8_t value)
{
    uint8_t index = mem->memory[OCPS] & LOWER_6_MASK;
    mem->cram[index + 0x40] = value;
    uint8_t inc_index = (index + 1) & LOWER_6_MASK;

    if (mem->memory[OCPS] & BIT_7_MASK)
    {
        mem->memory[OCPS] = (mem->memory[OCPS] & BIT_7_MASK) | inc_index;
    }
}

// Timer

static void write_timer(EmuMemory *mem, uint16_t address, uint8_t value)
{
    write_timer_register(mem->timer, address, value);
}

// CPU

static void write_interrupt_flag(EmuMemory *mem, uint16_t address, uint8_t value)
{
    mem->memory[IFR] = (0xE0 | (value & LOWER_5_MASK));
}

// APU

static void write_audio(EmuMemory *mem, uint16_t address, uint8_t value)
{
    write_audio_register(mem->apu, address, value);
}

static uint8_t read_wave_ram(EmuMemory *mem, uint16_t address)
{
    address -= WAVE_RAM_START;
    return mem->wave_ram[address];
}

static void write_wave_ram(EmuMemory *mem, uint16_t address, uint8_t value)
{
    address -= WAVE_RAM_START;
    mem->wave_ram[address] = value;
}

// DMA and HDMA Transfers

static void dma_handler(EmuMemory *mem, uint16_t address, uint8_t value)
{
    value = (value == 0xFF) ? 0xDF : value;
    value = (value == 0xFE) ? 0xE0 : value;
    uint16_t source = value * 0x0100;

    mem->memory[DMA]       = value;
    mem->dma.src           = source;
    mem->dma.dst           = OAM_START;
    mem->dma.length        = DMA_DURATION;
    mem->oam_read_blocked  = mem->dma.active;
    mem->oam_write_blocked = mem->dma.active;
    mem->dma.active        = true;
}


/* 
    (HDMA5 & 0x7F) = (L / $10) - 1
    L = ((HDMA5 & 0x7F) + 1) / $10 
*/
static void hdma_handler(EmuMemory *mem, uint16_t address, uint8_t value)
{  
    if (!mem->cart->is_gbc)
        return;

    HdmaMode mode = (value >> 7) & BIT_0_MASK;

    if ((mode == 0) && mem->hdma.active) // This can only occur with mode 1 since cpu is blocked during general transfer.
    {
        mem->hdma.active = false;
        return;
    }

    // Source, bits 15-4
    uint16_t hdma1 = mem->memory[HDMA1];
    uint16_t hdma2 = mem->memory[HDMA2] & UPPER_4_MASK;
    uint16_t   src = (hdma1 << BYTE) | hdma2;

    // Destination, 12-4
    uint16_t hdma3 = mem->memory[HDMA3] & LOWER_5_MASK;
    uint16_t hdma4 = mem->memory[HDMA4] & UPPER_4_MASK;
    uint16_t   dst = VRAM_START | (hdma3 << BYTE) | hdma4;

    mem->hdma.active             = true;
    mem->hdma.src                = src; 
    mem->hdma.dst                = dst;
    mem->hdma.length             = ((value & LOWER_7_MASK) + 1) * 0x10; // or << 4
    mem->hdma.bytes_transferring = (mode == GENERAL_HDMA);
    mem->hdma.mode               = mode;
    mem->hdma.counter            = 0;
    mem->hdma.bytes_transferred  = 0;

    mem->memory[HDMA5] = value;
}

// Emulation

static uint8_t read_joypad(EmuMemory *mem,  uint16_t address)
{
    Joypad *joypad = mem->joypad;
    uint8_t select = mem->memory[JOYP] & 0x30;
    uint8_t result = select | 0xCF;

    if ((select & BIT_5_MASK) == 0) // Action buttons selected
    {
        if (joypad->A)      result &= ~A_BUTTON_MASK;
        if (joypad->B)      result &= ~B_BUTTON_MASK;
        if (joypad->SELECT) result &= ~SELECT_BUTTON_MASK;
        if (joypad->START)  result &= ~START_BUTTON_MASK;
    }

    if ((select & BIT_4_MASK) == 0) // Direction buttons selected 
    {
        if (joypad->RIGHT) result &= ~RIGHT_BUTTON_MASK;
        if (joypad->LEFT)  result &= ~LEFT_BUTTON_MASK;
        if (joypad->UP)    result &= ~UP_BUTTON_MASK;
        if (joypad->DOWN)  result &= ~DOWN_BUTTON_MASK;
    }

    return result;
}

static void write_joypad(EmuMemory *mem, uint16_t address, uint8_t value)
{
    mem->memory[address] = (value & 0xF0);
}


// LINKING AND INITIALIZATION


void link_mmu(EmuMemory *mem, GbcEmu *emu)
{
    mem->joypad = &emu->joypad;
    mem->  cart = emu->cart;
    mem->   cpu = emu->cpu;
    mem-> timer = emu->timer;
    mem->   apu = emu->apu;
    mem->   ppu = emu->ppu;
}

static void init_tables()
{
    for (int index = ROM_STATIC_START; index < VRAM_START; index++)
    {
        memory_read_table[index]  = read_cart_memory;
        memory_write_table[index] = write_cart_memory;
    }

    for (int index = VRAM_START; index < EXT_RAM_START; index++)
    {
        memory_read_table[index]  = read_vram;
        memory_write_table[index] = write_vram;
    }

    for (int index = EXT_RAM_START; index < WRAM_STATIC_START; index++)
    {
        memory_read_table[index]  =  read_cart_memory;
        memory_write_table[index] = write_cart_memory;
    }

    for (int index = WRAM_STATIC_START; index < WRAM_DYNAMIC_START; index++)
    {
        memory_read_table[index]  = read_static_wram;
        memory_write_table[index] = write_static_wram;
    }

    for (int index = WRAM_DYNAMIC_START; index < ECHO_RAM_START; index++)
    {
        memory_read_table[index]  = read_dynamic_wram;
        memory_write_table[index] = write_dynamic_wram;
    }
    
    for (int index = ECHO_RAM_START; index < OAM_START; index++)
    {
        memory_read_table[index]  = read_echo_ram;
        memory_write_table[index] = write_echo_ram;
    }

    for (int index = OAM_START; index < NOT_USABLE_START; index++)
    {
        memory_read_table[index]  =  read_oam;
        memory_write_table[index] = write_oam;
    }

    for (int index = NOT_USABLE_START; index < IO_REGISTERS_START; index++)
    {
        memory_read_table[index]  =  default_read;
        memory_write_table[index] = default_write;
    }

    for (int index = IO_REGISTERS_START; index < HIGH_RAM_START; index++)
    {
        memory_read_table[index]  =  default_read;
        memory_write_table[index] = default_write;
    }

    for (int index = HIGH_RAM_START; index <= INTERRUPT_ENABLE; index++)
    {
        memory_read_table[index]  =  default_read;
        memory_write_table[index] = default_write;
    }

    // IO Registers (Overrides defaults set earlier)

    // PPU
    memory_write_table[LCDC]  = write_ppu;
    memory_write_table[STAT]  = write_ppu;
    memory_write_table[LY]    = write_ppu;
    memory_write_table[LYC]   = write_ppu;

    // Timer
    memory_write_table[DIV]   = write_timer;
    memory_write_table[TIMA]  = write_timer;
    memory_write_table[TMA]   = write_timer; 
    memory_write_table[TAC]   = write_timer;

    // CPU
    memory_write_table[IFR]   = write_interrupt_flag;

    // Channel 1
    memory_write_table[NR10]  = write_audio;
    memory_write_table[NR11]  = write_audio;
    memory_write_table[NR12]  = write_audio;
    memory_write_table[NR13]  = write_audio;
    memory_write_table[NR14]  = write_audio;
    // Channel 2
    memory_write_table[NR20]  = write_audio;
    memory_write_table[NR21]  = write_audio;
    memory_write_table[NR22]  = write_audio;
    memory_write_table[NR23]  = write_audio;
    memory_write_table[NR24]  = write_audio;
    // Channel 3
    memory_write_table[NR30]  = write_audio;
    memory_write_table[NR31]  = write_audio;
    memory_write_table[NR32]  = write_audio;
    memory_write_table[NR33]  = write_audio;
    memory_write_table[NR34]  = write_audio;
    // Channel 4
    memory_write_table[NR40]  = write_audio;
    memory_write_table[NR41]  = write_audio;
    memory_write_table[NR42]  = write_audio;
    memory_write_table[NR43]  = write_audio;
    memory_write_table[NR44]  = write_audio;
    // Global audio
    memory_write_table[NR50]  = write_audio;
    memory_write_table[NR51]  = write_audio;
    memory_write_table[NR52]  = write_audio;
    
    // DMA
    memory_write_table[DMA]   = dma_handler;

    // BIOS Latch
    memory_write_table[BIOS]  = write_bios;
    
    // HDMA
    memory_write_table[HDMA5] = hdma_handler;

    // Background Palette
    memory_read_table[BCPD]   = read_bcpd;
    memory_write_table[BCPD]  = write_bcpd;
    
    // Object Palette
    memory_read_table[OCPD]   = read_ocpd;
    memory_write_table[OCPD]  = write_ocpd;

    // Joypad
    memory_read_table[JOYP]   = read_joypad;
    memory_write_table[JOYP]  = write_joypad;

    // IO Ranges
    for (int i = WAVE_RAM_START; i <= WAVE_RAM_END; i++)
    {
        memory_read_table[i]  = read_wave_ram;
        memory_write_table[i] = write_wave_ram;
    }
}

static void init_masks()
{
    // Channel 1
    memory_mask_table[NR10] = 0x80;
    memory_mask_table[NR11] = 0x3F;
    memory_mask_table[NR12] = 0x00;
    memory_mask_table[NR13] = 0xFF;
    memory_mask_table[NR14] = 0xBF;
    // Channel 2
    memory_mask_table[NR20] = 0xFF;
    memory_mask_table[NR21] = 0x3F;
    memory_mask_table[NR22] = 0x00;
    memory_mask_table[NR23] = 0xFF;
    memory_mask_table[NR24] = 0xBF;
    // Channel 3
    memory_mask_table[NR30] = 0x7F;
    memory_mask_table[NR31] = 0xFF;
    memory_mask_table[NR32] = 0x9F;
    memory_mask_table[NR33] = 0xFF;
    memory_mask_table[NR34] = 0xBF;
    // Channel 4
    memory_mask_table[NR40] = 0xFF;
    memory_mask_table[NR41] = 0xFF;
    memory_mask_table[NR42] = 0x00;
    memory_mask_table[NR43] = 0x00;
    memory_mask_table[NR44] = 0xBF;
    // Global Audio Registers
    memory_mask_table[NR50] = 0x00;
    memory_mask_table[NR51] = 0x00;
    memory_mask_table[NR52] = 0x70;

    for (int index = (NR52 + 1); index < WAVE_RAM_START; index++)
        memory_mask_table[index] = 0xFF;
}

EmuMemory *init_memory()
{
    EmuMemory *mem = (EmuMemory*) malloc(sizeof(EmuMemory));
    memset(mem, 0, sizeof(EmuMemory));

    // (65,536 Bytes) General Memory with some 'extra' room for lazy addressing.
    mem->memory = (uint8_t*) malloc(MEMORY_SIZE * sizeof(uint8_t));
    memset(mem->memory, 0, MEMORY_SIZE); 

    // CRAM Memory 
    mem->cram = (uint8_t*) malloc(CRAM_BANK_SIZE * sizeof(uint8_t));
    memset(mem->cram, 0, CRAM_BANK_SIZE);

    // VRAM 
    mem->vram    = (uint8_t**) malloc(VRAM_BANK_QUANTITY * sizeof(uint8_t*));
    mem->vram[0] = (uint8_t*) malloc(VRAM_BANK_SIZE     * sizeof(uint8_t));
    memset(mem->vram[0], 0, VRAM_BANK_SIZE);
    mem->vram[1] = (uint8_t*) malloc(VRAM_BANK_SIZE     * sizeof(uint8_t));
    memset(mem->vram[1], 0, VRAM_BANK_SIZE);

    // WRAM
    mem->wram = (uint8_t**) malloc(WRAM_BANK_QUANTITY * sizeof(uint8_t*));
    for (uint8_t i = 0; i < WRAM_BANK_QUANTITY; i++)
    {
        mem->wram[i] = (uint8_t*) malloc(WRAM_BANK_SIZE * sizeof(uint8_t));
        memset(mem->wram[i], 0, WRAM_BANK_SIZE);
    }

    // Wave RAM
    mem->wave_ram = (uint8_t*) malloc(WAVE_RAM_SIZE * sizeof(uint8_t));
    memset(mem->wave_ram, 0, WAVE_RAM_SIZE);

    // OAM
    mem->oam = (uint8_t*) malloc(OAM_SIZE * sizeof(uint8_t));
    memset(mem->oam, 0, WAVE_RAM_SIZE);
 
    init_tables();
    init_masks();

    return mem;
}

void tidy_memory(EmuMemory **mem)
{
    free((*mem)->memory); 
    (*mem)->memory = NULL;

    free((*mem)->cram); 
    (*mem)->cram = NULL;

    free((*mem)->vram[0]); 
    (*mem)->vram[0] = NULL;
    free((*mem)->vram[1]); 
    (*mem)->vram[1] = NULL;
    free((*mem)->vram); (*mem)->vram = NULL;

    for (int i = 0; i < WRAM_BANK_QUANTITY; i++)
    {
        free((*mem)->wram[i]);
        (*mem)->wram[i] = NULL;
    }
    free((*mem)->wram); (*mem)->wram = NULL;

    free((*mem)->wave_ram); (*mem)->wave_ram = NULL;
}
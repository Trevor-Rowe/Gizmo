#ifndef PPU_H
#define PPU_H

#include <stdbool.h>
#include <stdint.h>

#define VISIBLE_TILES_PER_ROW  21

typedef struct Cartridge Cartridge;
typedef struct GbcEmu GbcEmu;
typedef struct EmuMemory EmuMemory;
typedef struct CPU CPU;

typedef enum
{
    DOT_PER_FRAME      = 70224,
    DOTS_PER_SCANLINE  =   456,
    OBJS_PER_SCANLINE  =    10,
    GBC_WIDTH          =   160,
    GBC_HEIGHT         =   144,
    GRID_SIZE          =    32,
    OAM_SCAN_DELAY     =    80,
    OAM_ENTRY_SIZE     =     4,
    OBJ_PER_LINE       =    10,
    SCAN_LINE_QUANTITY =   154,
    TILE_SIZE          =     8,

} GraphicDefaults;

typedef enum
{
    WHITE      = 0xFFE0F8D0,
    LIGHT_GRAY = 0xFF88C070,
    DARK_GRAY  = 0xFF346856,
    BLACK      = 0xFF081820

} DmgColors;

typedef enum
{
    HBLANK      = (uint8_t) 0x00, // (276 - Len(Mode 3))
    VBLANK      = (uint8_t) 0x01, // (4560 dots)
    OAM_SCAN    = (uint8_t) 0x02, // (80 dots)
    DRAWING     = (uint8_t) 0x03, // (172-289 dots)
    COINCIDENCE = (uint8_t) 0x04  // Coincidence

} PpuMode;

typedef enum
{
    B0_ADDRESS_START  = (uint16_t) 0x8000, // $8000
    B0_ADDRESS_END    = (uint16_t) 0x87FF, // $87FF
    B1_ADDRESS_START  = (uint16_t) 0x8800, // $8800
    B1_ADDRESS_END    = (uint16_t) 0x8FFF, // $8FFF
    B2_ADDRESS_START  = (uint16_t) 0x9000, // $9000
    B2_ADDRESS_END    = (uint16_t) 0x97FF, // $97FF
    TM0_ADDRESS_START = (uint16_t) 0x9800, // $9800
    TM0_ADDRESS_END   = (uint16_t) 0x9BFF, // $9BFF
    TM1_ADDRESS_START = (uint16_t) 0x9C00, // $9C00
    TM1_ADDRESS_END   = (uint16_t) 0x9FFF, // $9FFF
    TM_OFFEST         = (uint16_t) 0x0400  // $0400

} VRAMAddresses;

typedef struct
{
    uint8_t     x;
    uint8_t     y;
    uint8_t   row;

    uint8_t  attr;
    uint8_t   lsb;
    uint8_t   msb;

} Tile;

typedef struct PPU
{
    bool tile_considered[VISIBLE_TILES_PER_ROW];
    
    PpuMode           mode;
    uint16_t        sc_dot;
    uint8_t        penalty;

    uint8_t          *lcdc;
    uint8_t          *stat;
    uint8_t           *lyc;

    uint8_t        sc_tile;
    uint8_t             lx;
    uint8_t            *ly;
    uint8_t           *scx;
    uint8_t           *scy;
    uint8_t            *wx;
    uint8_t            *wy;

    uint8_t           *bgp;
    uint8_t          *opd0;
    uint8_t          *opd1;

    Cartridge        *cart;
    GbcEmu            *emu;
    EmuMemory         *mem;
    CPU               *cpu;

    bool           init_sc;
    bool         init_tile;
    bool     win_rendering;
    bool      sc_rendering;
    bool       frame_delay;
    bool           running;

    bool     stat_irq_line;
    bool           lyc_irq;

} PPU;

bool ppu_dot(PPU *ppu);

char *get_ppu_state(PPU *ppu, char *buffer, size_t size);

void write_ppu_register(PPU *ppu, uint16_t address, uint8_t value);

void *render_frame(PPU *ppu);

void link_ppu(PPU *ppu, GbcEmu *emu);

PPU *init_ppu();

void tidy_ppu(PPU **ppu);

#endif
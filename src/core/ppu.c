#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "core/emulator.h"
#include "core/cart.h"
#include "core/mmu.h"
#include "core/cpu.h"
#include "core/ppu.h"

#include "util/common.h"
#include "util/circular_queue.h"

#define FRAME_SIZE GBC_WIDTH * GBC_HEIGHT * sizeof(uint32_t)

static uint32_t        *gbc_lcd;
static uint32_t *disabled_frame;

static Queue          *oam_fifo;
static Queue          *bgw_fifo;
static Queue          *obj_fifo;

static uint8_t scx_penalty(PPU *ppu)
{
    switch(*ppu->scx % TILE_SIZE)
    {
        case 0:
            return 0;

        case 1:
        case 2:
        case 3:
        case 4:
            return 4;

        case 5:
        case 6:
        case 7:
            return 8;
    }
}

static uint8_t obj_penalty(PPU *ppu, OamObject *obj)
{
    if (obj->x >= 168) 
        return 0;

    uint8_t tile = obj->x / TILE_SIZE;

    if (ppu->tile_considered[tile]) 
        return 6;

    ppu->tile_considered[tile] = true;

    if (obj->x == 0) 
        return 11;

    int8_t  penalty = TILE_SIZE - (obj->x % TILE_SIZE) - 3;
    penalty = (penalty < 0) ? 0 : penalty;
    penalty += 6;

    return (uint8_t) penalty;
}

static inline void unlock_oam(EmuMemory *mem)
{
    mem-> oam_read_blocked = false;
    mem->oam_write_blocked = false;
}

static inline void lock_oam(EmuMemory *mem)
{
    mem-> oam_read_blocked = true;
    mem->oam_write_blocked = true;
}

static inline void unlock_vram(EmuMemory *mem)
{
    mem-> vram_read_blocked = false;
    mem->vram_write_blocked = false;
}

static inline void lock_vram(EmuMemory *mem)
{
    mem-> vram_read_blocked = true;
    mem->vram_write_blocked = true;
}

// Drawing

static uint32_t get_argb(uint8_t lsb, uint8_t msb)
{
    uint16_t color = (msb << BYTE) | lsb;
    uint8_t   red  = ((color)       & LOWER_5_MASK) << 3;
    uint8_t green  = ((color >>  5) & LOWER_5_MASK) << 3;
    uint8_t  blue  = ((color >> 10) & LOWER_5_MASK) << 3;
    uint32_t argb  = 
    (0xFF << (BYTE * 3)) | (red << (BYTE * 2)) | (green << (BYTE * 1)) | blue;
    return argb;
}

static uint32_t get_dmg_shade(uint8_t id)
{
    uint32_t result = WHITE;
    switch(id)
    {
        case 0: result =      WHITE; break;
        case 1: result = LIGHT_GRAY; break;
        case 2: result =  DARK_GRAY; break;
        case 3: result =      BLACK; break;
    }
    return result;
}

static uint8_t get_color_id(uint8_t lsb, uint8_t msb, bool x_flip, uint8_t bit_index)
{
    uint8_t shift = x_flip ? bit_index : (7 - bit_index);

    uint8_t lo = (lsb >> shift) & 0x1;
    uint8_t hi = (msb >> shift) & 0x1;

    return (hi << 1) | lo;
}

static bool drawing_window(PPU *ppu)
{
    bool win_enabled = (((*ppu->lcdc) & BIT_5_MASK) != 0);
    return (((ppu->lx + 7) >= (*ppu->wx)) && (*ppu->ly >= *ppu->wy) && win_enabled);
}

static bool obj_rendering_triggered(PPU *ppu)
{
    bool obj_enabled = (((*ppu->lcdc) & BIT_1_MASK) != 0);

    OamObject *obj = (OamObject*) peek(oam_fifo);

    if (obj == NULL || !obj_enabled) return false;

    uint8_t lx = ppu->lx + TILE_SIZE; // Object domain. WRT

    return (lx >= obj->x);
}

static uint32_t get_obj_pixel_color(PPU *ppu, GbcPixel *pixel)
{
    if(ppu->cart->is_gbc)
    {
        uint8_t lsb = read_cram(ppu->mem, true, pixel->cgb_palette, pixel->color, 0);
        uint8_t msb = read_cram(ppu->mem, true, pixel->cgb_palette, pixel->color, 1);
        return get_argb(lsb, msb); 
    }
    else
    {
        uint8_t opd = (pixel->dmg_palette) ? *ppu->opd1 : *ppu->opd0;
        uint8_t cid = (opd >> (2 * pixel->color)) & LOWER_2_MASK;
        return get_dmg_shade(cid);
    }
}

static uint32_t get_bgw_pixel_color(PPU *ppu, GbcPixel *pixel)
{
    if(ppu->cart->is_gbc)
    {
        uint8_t lsb = read_cram(ppu->mem, false, pixel->cgb_palette, pixel->color, 0);
        uint8_t msb = read_cram(ppu->mem, false, pixel->cgb_palette, pixel->color, 1);
        return get_argb(lsb, msb); 
    }
    else
    {
        uint8_t cid = ((*ppu->bgp) >> (2 * pixel->color)) & LOWER_2_MASK;
        return get_dmg_shade(cid);
    }
}

static uint32_t merge_obj_bgw(PPU *ppu, GbcPixel *bgw, GbcPixel *obj)
{   
    if (obj->color == 0) return get_bgw_pixel_color(ppu, bgw);
    if (bgw->color == 0) return get_obj_pixel_color(ppu, obj);

    bool master_prio = ppu->cart->is_gbc ? (((*ppu->lcdc) & BIT_0_MASK) != 0) : false;
    uint8_t code = (master_prio << 2) | (obj->priority << 1) | bgw->priority;

    switch(code) // Truth table from Pandocs. 
    {
        case 0:
        case 1:
        case 2:
        case 3:
        case 4:
            return get_obj_pixel_color(ppu, obj);
        
        case 5:
        case 6:
        case 7:
            return get_bgw_pixel_color(ppu, bgw);
    }
    
}

static void draw_pixel_lcd(PPU *ppu)
{
    if (!is_empty(bgw_fifo) && !is_empty(obj_fifo))
    {
        GbcPixel *bgw = dequeue(bgw_fifo); 
        GbcPixel *obj = dequeue(obj_fifo);
        gbc_lcd[((*ppu->ly) * GBC_WIDTH) + ppu->lx] = merge_obj_bgw(ppu, bgw, obj);
        ppu->lx++;
    }
    else if (!is_empty(bgw_fifo))
    {
        GbcPixel *bgw = dequeue(bgw_fifo);
        gbc_lcd[((*ppu->ly) * GBC_WIDTH) + ppu->lx] = get_bgw_pixel_color(ppu, bgw);
        ppu->lx++;
    }

    if (ppu->lx >= GBC_WIDTH)
        ppu->sc_rendering = false;
}

// VRAM Access

static uint16_t bgw_tile_data_address(uint8_t index, uint8_t lcdc, uint8_t row)
{
    uint16_t base;

    if ((lcdc & BIT_4_MASK) != 0)  // Tile Data Select (0x8000 mode)
    {
        base = 0x8000 + (index * 16);
    }
    else  // 0x8800 mode (signed indices)
    {
        int8_t signed_index = (int8_t) index;
        base = 0x9000 + (signed_index * 16);
    }

    return base + (row * 2);  // Two bytes per row
}

static void encode_tile(PPU *ppu, Tile *tile, uint8_t mask)
{
    uint16_t map_base = ((*ppu->lcdc & mask) != 0) ? TM1_ADDRESS_START : TM0_ADDRESS_START;
    uint16_t  address = map_base + (tile->y * GRID_SIZE) + tile->x;

    uint8_t      bank = 0;

    if (ppu->cart->is_gbc)
    {
        // Tilemap Attribute Bank 1
        tile->attr = read_vram_bank(ppu->mem, 1, address);
        // Y-Flip Handling
        tile-> row = (tile->attr & BIT_6_MASK) ? (TILE_SIZE - 1 - tile->row) : tile->row;
        // Update Bank
        bank = (tile->attr & BIT_3_MASK) >> 3;
    }

    // Get MSB and LSB data from VRAM.
    uint8_t index = read_vram_bank(ppu->mem, 0, address);
    address   = bgw_tile_data_address(index, *ppu->lcdc, tile->row);
    tile->lsb = read_vram_bank(ppu->mem, bank, address);
    tile->msb = read_vram_bank(ppu->mem, bank, address + 1);
}

static Tile get_win_tile(PPU *ppu)
{
    Tile tile = {0};
    // Column Calc
    tile.x = ((ppu->lx + 7) - (*ppu->wx)) / TILE_SIZE;
    // Row Calc
    uint8_t y = (*ppu->ly) - (*ppu->wy); 
    tile.y = y / TILE_SIZE;
    tile.row = y % TILE_SIZE;
    // VRAM fetch
    encode_tile(ppu, &tile, BIT_6_MASK);

    return tile;
}

static Tile get_bg_tile(PPU *ppu)
{
    Tile tile = {0};
    // Column Calc
    tile.x = (((*ppu->scx) / TILE_SIZE) + ppu->sc_tile) % GRID_SIZE;
    // Row Calc
    uint8_t y = (*ppu->scy) + (*ppu->ly);
    tile.y = (y / TILE_SIZE) % GRID_SIZE;
    tile.row = y % TILE_SIZE;
    // VRAM fetch
    encode_tile(ppu, &tile, BIT_3_MASK);

    return tile;
}

static Tile get_obj_tile(PPU *ppu)
{
    Tile tile = {0};

    OamObject *obj =  (OamObject*) peek(oam_fifo);

    uint8_t    row = ((*ppu->ly + 16) - obj->y);
    bool   stacked = (((*ppu->lcdc) & BIT_2_MASK) != 0);
    uint8_t height = stacked ? 16 : 8;
    uint8_t  index = obj->tile_index;

    row = (obj->y_flip) ? (height - 1 - row) : row;

    uint16_t address = B0_ADDRESS_START + (index * 16) + (row * 2);
    tile.lsb = read_vram_bank(ppu->mem, obj->bank, address);
    tile.msb = read_vram_bank(ppu->mem, obj->bank, address + 1);

    return tile;
}

static void oam_scan(PPU *ppu)
{
    reset_queue(oam_fifo);
    
    uint16_t address = OAM_START;
    bool stacked = (((*ppu->lcdc) & BIT_2_MASK) != 0);

    while((address <= OAM_END) && (oam_fifo->size < 10))
    {
        uint8_t       y_pos = read_memory(ppu->mem, address); // y_screen + 16
        uint8_t      height = (stacked) ? 16 : 8;
        uint8_t          ly = (*ppu->ly) + 16; // Object domain
        bool    on_scanline = (ly >= y_pos) && ((ly - y_pos) < height);

        if (on_scanline)
        {
            OamObject obj = (OamObject) {0}; 

            uint8_t       x_pos = read_memory(ppu->mem, address + 1); // x_screen + 8
            uint8_t  tile_index = read_memory(ppu->mem, address + 2);
            uint8_t  attributes = read_memory(ppu->mem, address + 3);

            obj.    oam_address = address;
            obj.              x = x_pos; 
            obj.              y = y_pos;
            obj.     tile_index = tile_index;

            obj.       priority = (attributes & BIT_7_MASK) != 0;
            obj.         y_flip = (attributes & BIT_6_MASK) != 0;
            obj.         x_flip = (attributes & BIT_5_MASK) != 0;
            obj.    dmg_palette = (attributes & BIT_4_MASK) != 0;
            obj.           bank = ((attributes & BIT_3_MASK) != 0) ?  1 : 0;
            obj.    cgb_palette = (uint8_t) (attributes & LOWER_3_MASK);

            enqueue_object(oam_fifo, &obj);
        }

        address += OAM_ENTRY_SIZE;
    }

    sort_oam_by_xpos(oam_fifo);
}

// Pixel Pipeline

static uint8_t get_tile_pixel_color(Tile tile, uint8_t shift, bool x_flip)
{
    uint8_t lsb, msb;

    if (x_flip)
    {
        lsb = (tile.lsb >> shift) & BIT_0_MASK;
        msb = (tile.msb >> shift) & BIT_0_MASK;
    }
    else
    {
        lsb = (tile.lsb >> (TILE_SIZE - 1 - shift)) & BIT_0_MASK;
        msb = (tile.msb >> (TILE_SIZE - 1 - shift)) & BIT_0_MASK;
    }

    return ((msb << 1) | lsb);
}

static void push_bgw_row(Tile tile, Queue *fifo, uint8_t offset)
{
    bool   x_flip = ((tile.attr & BIT_5_MASK) != 0);

    for (uint8_t i = offset; i < TILE_SIZE; i++)
    {
        GbcPixel    pixel = {0};
        pixel.      color = get_tile_pixel_color(tile, i, x_flip);
        pixel.   priority = ((tile.attr & BIT_7_MASK) != 0);
        pixel.cgb_palette = tile.attr & LOWER_3_MASK;
        enqueue_pixel(fifo, &pixel);
    }
}

static uint8_t push_obj_row(PPU *ppu, Tile tile, Queue *fifo)
{
    OamObject *obj = dequeue(oam_fifo);     // Consume object.

    ppu->penalty += obj_penalty(ppu, obj);  // Calculate drawing penalty.

    GbcPixel pixel = {0};                   // Transparent pixel.

    while(fifo->size < TILE_SIZE)           // Pad up to 8.
    {
        enqueue_pixel(fifo, &pixel);
    }

    for (uint8_t i = 0; i < TILE_SIZE; i++) // Replace transparent pixels.
    {
        GbcPixel *current = dequeue(fifo);

        pixel.      color = get_tile_pixel_color(tile, i, obj->x_flip);
        pixel.   priority = obj->priority;
        pixel.dmg_palette = obj->dmg_palette;
        pixel.cgb_palette = obj->cgb_palette;
        
        current = (current->color == 0) ? &pixel : current;
        enqueue_pixel(fifo, current);
    }

    for (uint8_t i = 0; i < ((ppu->lx + 8) - obj->x); i++)    // Chop a little off the top.
    {
        dequeue(fifo);
    }
}

static void pixel_pipeline_step(PPU *ppu)
{
    while (obj_rendering_triggered(ppu))
    {
        Tile tile = get_obj_tile(ppu);
        push_obj_row(ppu, tile, obj_fifo);
    }

    if (drawing_window(ppu) && !ppu->win_rendering)
    {
        reset_queue(bgw_fifo);
        ppu->penalty += 6;
        ppu->win_rendering = true;
    }
    
    if (is_empty(bgw_fifo))
    {
        Tile tile = ppu->win_rendering ? get_win_tile(ppu) : get_bg_tile(ppu);
        uint8_t offset = ((ppu->sc_tile == 0) && !ppu->win_rendering) ? ((*ppu->scx) % 8) : 0; 
        push_bgw_row(tile, bgw_fifo, offset);
        ppu->sc_tile++;
    }

    draw_pixel_lcd(ppu);
}

// Mode Handling

static void check_stat_irq(PPU *ppu, PpuMode mode)
{
    bool triggered = false;
    uint8_t stat = (*ppu->stat);

    switch(mode)
    {
        case HBLANK:
            triggered = ((stat & BIT_3_MASK) != 0);
            break;

        case VBLANK:
            triggered = ((stat & BIT_4_MASK) != 0);
            break;

        case OAM_SCAN:
            triggered = ((stat & BIT_5_MASK) != 0);
            break;

        case DRAWING:
            ppu->stat_irq_line = ppu->lyc_irq; 
            break;

        case COINCIDENCE:
            triggered = ((stat & BIT_6_MASK) != 0);
            bool intersecting = ((*ppu->ly) == (*ppu->lyc));
            stat &= ~BIT_2_MASK;
            stat |= (intersecting << 2);
            triggered &= intersecting;
            triggered &= !ppu->lyc_irq;
            ppu->lyc_irq = intersecting;
            break;
    }

    (*ppu->stat) = stat;

    if (triggered && !ppu->stat_irq_line)
    {
        ppu->stat_irq_line = true;
        request_interrupt(ppu->cpu, LCD_STAT_INTERRUPT_CODE);
    }
}

static inline void set_ppu_mode(PPU *ppu, PpuMode mode)
{
    (*ppu->stat) = ((*ppu->stat) & ~LOWER_2_MASK) | mode;
    ppu->mode = mode;
}

static void enter_oam_mode(PPU *ppu)
{
    // OAM Scan
    unlock_oam(ppu->mem);
    oam_scan(ppu);
    lock_oam(ppu->mem);
    // Check for STAT interrupt. 
    check_stat_irq(ppu, OAM_SCAN);
    // Update STAT
    set_ppu_mode(ppu, OAM_SCAN);
}

static void enter_drawing_mode(PPU *ppu)
{
    reset_queue(bgw_fifo);
    reset_queue(obj_fifo);
    // Normalize scanline variables.
    ppu->      penalty = scx_penalty(ppu);
    ppu->      sc_tile =     0;
    ppu->           lx =     0;
    ppu->win_rendering = false;
    ppu-> sc_rendering =  true;
    // Lock memory.
    lock_oam(ppu->mem);
    lock_vram(ppu->mem);
    // Reset object penalty tiles.
    memset(ppu->tile_considered, 0, sizeof(ppu->tile_considered));
    // Check for STAT interrupt.
    check_stat_irq(ppu, DRAWING);
    // Update STAT
    set_ppu_mode(ppu, DRAWING);
}

static void enter_hblank_mode(PPU *ppu)
{
    // Unlock memory.
    unlock_oam(ppu->mem);
    unlock_vram(ppu->mem);
    check_hdma_trigger(ppu->mem);
    // Check for STAT interrupt.
    check_stat_irq(ppu, HBLANK);
    // Update Stat
    set_ppu_mode(ppu, HBLANK);
}

static void enter_vblank_mode(PPU *ppu)
{
    request_interrupt(ppu->cpu, VBLANK_INTERRUPT_CODE);
    // Check for STAT interrupt.
    check_stat_irq(ppu, VBLANK);
    check_stat_irq(ppu, OAM_SCAN); // Hardware Quirk
    // Update Stat
    set_ppu_mode(ppu, VBLANK);
}

static bool next_scanline(PPU *ppu)
{
    ppu->sc_dot = 0;
    uint8_t next_ly = ((*ppu->ly) + 1) % SCAN_LINE_QUANTITY;
    *ppu->ly = next_ly;

    if ((*ppu->stat & BIT_2_MASK) != 0)
        check_stat_irq(ppu, COINCIDENCE);

    if (*ppu->ly == 0)
        return true;

    return false;
}

static void check_mode(PPU *ppu)
{
    uint16_t dot = ppu->sc_dot;
    uint8_t   ly = *ppu->ly;

    if (ly < GBC_HEIGHT)
    {  
        switch(dot)
        {
            case 0:
                enter_oam_mode(ppu);
                check_stat_irq(ppu, COINCIDENCE);
                break;

            case 79: // Hardware quirk
                ppu->mem->oam_write_blocked = false;
                ppu->mem->vram_read_blocked = !ppu->init_sc;
                break;

            case 80:
                enter_drawing_mode(ppu);
                break;

            case 455:
                ppu->init_sc = false;
                ppu->mem->oam_read_blocked = true;
        }

        if (dot == (252 + ppu->penalty))
        {
            enter_hblank_mode(ppu);
        }
        
        return;
    }

    if ((ly == GBC_HEIGHT) && (dot == 0))
    {
        enter_vblank_mode(ppu);
        return;
    }

}

// Driver

bool ppu_dot(PPU *ppu) // Once per dot. 
{
    bool frame_ready = false;

    if (!ppu->running) return false;

    check_mode(ppu);

    if (++ppu->sc_dot == DOTS_PER_SCANLINE)
        frame_ready = next_scanline(ppu);
    
    if ((ppu->mode == DRAWING) && ppu->sc_rendering)
        pixel_pipeline_step(ppu); 

    return frame_ready;
}

// Info

char *get_ppu_state(PPU *ppu, char *buffer, size_t size)
{
    snprintf(
        buffer, 
        size, 
        "[LCDC] = %02X, [LY] = %02X, [LYC] = %02X, [STAT] = %02X, [SC] = %d",
        (*ppu->lcdc),
        (*ppu->ly),
        (*ppu->lyc),
        (*ppu->stat),
        (ppu->sc_dot)
    );

    return buffer;
}

// Register Writes 

static void write_lcdc(PPU *ppu, uint8_t value)
{
    (*ppu->lcdc) = value;
    
    bool enabled = (((*ppu->lcdc) & BIT_7_MASK) != 0);

    if (ppu->running && !enabled)
    {
        ppu->running  = false;
        ppu->  sc_dot =     0;
        *ppu->     ly =     0;

        unlock_oam(ppu->mem);
        unlock_vram(ppu->mem);

        set_ppu_mode(ppu, HBLANK);
    }

    if (!ppu->running && enabled)
    {
        ppu->    running = true;
        ppu->frame_delay = true;
        ppu->    init_sc = true;
        ppu->    penalty =    0;
        ppu->     sc_dot =    4;
        *ppu->        ly =    0;

        unlock_oam(ppu->mem);
        unlock_vram(ppu->mem);

        check_stat_irq(ppu, COINCIDENCE);

        set_ppu_mode(ppu, HBLANK);
    }
}

static void write_stat(PPU *ppu, uint8_t value)
{
    value        &= ~0x87; // [0 X X X X 0 0 0]
    (*ppu->stat) &=  0x87; // [X 0 0 0 0 X X X]    
    (*ppu->stat) |= value;

    check_stat_irq(ppu, ppu->mode);
}

static void write_lyc(PPU *ppu, uint8_t value)
{
    *ppu->lyc = value;

    if (ppu->running)
        check_stat_irq(ppu, COINCIDENCE);
}

void write_ppu_register(PPU *ppu, uint16_t address, uint8_t value)
{
    switch(address)
    {
        case LCDC: write_lcdc(ppu, value); break;
        case STAT: write_stat(ppu, value); break;
        case LYC:  write_lyc(ppu, value);  break;
    }
}

// Obtaining Frame

void *render_frame(PPU *ppu)
{   
    if (ppu->frame_delay)
    {
        ppu->frame_delay = false;
        return disabled_frame;
    }

    return gbc_lcd;
}
 
// Linking and Initialization

void link_ppu(PPU *ppu, GbcEmu *emu)
{
    // Context
    ppu->cart = emu->cart;
    ppu-> emu = emu;
    ppu-> mem = emu->mem;
    ppu-> cpu = emu->cpu;

    // Hardware Registers
    ppu->ly   = &(emu->mem->memory[LY]);   // Current Scanline
    ppu->lyc  = &(emu->mem->memory[LYC]);  // LY == LYC Coincidence
    ppu->lcdc = &(emu->mem->memory[LCDC]); // LCD Control
    ppu->stat = &(emu->mem->memory[STAT]); // STAT Interrupt Control
    ppu->scx  = &(emu->mem->memory[SCX]);  // Viewport X Position
    ppu->scy  = &(emu->mem->memory[SCY]);  // Viewport Y Position
    ppu->wx   = &(emu->mem->memory[WX]);   // Window Screen Position + 7
    ppu->wy   = &(emu->mem->memory[WY]);   // Window Screen Position 
    ppu->bgp  = &(emu->mem->memory[BGP]);  // DMG - Background Palette
    ppu->opd0 = &(emu->mem->memory[OBP0]); // DMG - Object Palette 0
    ppu->opd1 = &(emu->mem->memory[OBP1]); // DMG - Object Palette 1
}

static void init_pipeline()
{
    gbc_lcd = (uint32_t*) malloc(FRAME_SIZE);

    disabled_frame = (uint32_t*) malloc(FRAME_SIZE);
    memset(disabled_frame, WHITE, FRAME_SIZE);

    oam_fifo = init_queue(OBJS_PER_SCANLINE, OBJECT);
    bgw_fifo = init_queue(2 * TILE_SIZE, PIXEL);
    obj_fifo = init_queue(TILE_SIZE, PIXEL);
}

static void tidy_pipeline()
{
    free(gbc_lcd);               gbc_lcd = NULL;
    free(disabled_frame); disabled_frame = NULL;

    tidy_queue(oam_fifo);
    tidy_queue(bgw_fifo);
    tidy_queue(obj_fifo);
}

PPU *init_ppu()
{
    PPU *ppu = (PPU*) malloc(sizeof(PPU));

    ppu->      penalty =     0;
    ppu->       sc_dot =     0;
    ppu->      running = false;
    ppu->win_rendering = false;
    ppu-> sc_rendering = false;
    ppu->stat_irq_line = false;
    ppu->      lyc_irq = false;

    init_pipeline(); 

    return ppu;
}

void tidy_ppu(PPU **ppu)
{
    free(*ppu);
    *ppu = NULL;

    tidy_pipeline();
}


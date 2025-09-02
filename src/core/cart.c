#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h> 
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h> // Windows
#define mkdir_if_needed(dir) _mkdir(dir)
#else
#include <unistd.h> // Linux
#define mkdir_if_needed(dir) mkdir((dir), 0755)
#endif

#include "core/cart.h"
#include "core/emulator.h"
#include "core/mmu.h"

#include "util/common.h"

// File and Header Helpers

static inline void strip_extension(char *file_name)
{
    char *dot = strrchr(file_name, '.');

    if (dot != file_name) // not at null char
        *dot = '\0';
}

static bool save_file_needed(Cartidge *cart)
{
    const char *file_name = cart->file_name;
    char  *file_name_copy = (char*) malloc(strlen(file_name) + 1);

    if (file_name_copy)
        strcpy(file_name_copy, file_name);
    
    strip_extension(file_name_copy);
    char save_path[MAX_FILE_PATH];
    snprintf(save_path, sizeof(save_path), "./%s/%s.sav", SAVE_DIR, file_name_copy);
    free(file_name_copy);
    
    FILE *file = fopen(save_path, "rb");

    if (!file) 
        return true;

    fclose(file);
    
    return false;
}

static bool save_game(Cartidge *cart)
{
    const char *file_name = cart->file_name;
    const uint8_t    *ram = cart->      ram;
    size_t           size = cart-> ram_size;

    mkdir_if_needed(SAVE_DIR);

    char *file_name_copy = (char*) malloc(strlen(file_name) + 1);

    if (file_name_copy)
        strcpy(file_name_copy, file_name);
    
    strip_extension(file_name_copy);
    char save_path[MAX_FILE_PATH];
    snprintf(save_path, sizeof(save_path), "./%s/%s.sav", SAVE_DIR, file_name_copy);
    free(file_name_copy);

    FILE *file = fopen(save_path, "wb");

    if (!file)
        return false;

    fwrite(ram, 1, size, file);
    fwrite(&cart->clock, 1, sizeof(RTCC), file);
    fclose(file);

    return true;
}

static bool load_game(Cartridge *cart)
{
    const char *file_name = cart->file_name;
    uint8_t          *ram = cart->      ram;
    size_t           size = cart-> ram_size;

    char *file_name_copy = (char*) malloc(strlen(file_name) + 1);

    if (file_name_copy)
        strcpy(file_name_copy, file_name);

    strip_extension(file_name_copy);
    char save_path[MAX_FILE_PATH];
    snprintf(save_path, sizeof(save_path), "./%s/%s.sav", SAVE_DIR, file_name_copy);
    free(file_name_copy);

    FILE *file = fopen(save_path, "rb");

    if (!file) 
        return false;

    fread(ram, 1, size, file);
    fread(&cart->clock, 1, sizeof(RTCC), file);
    fclose(file);

    return true;
}

static uint8_t *get_rom_content(Cartridge *cart, const char *file_path) // Reads file and returns pointer to loaded content.
{  
    FILE *file = fopen(file_path, "rb");

    if (!file) 
    {
        perror("Unable to find file!");
        return NULL;
    }

    // Move to end of file to find its size.
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    rewind(file);

    // Allocate memory to a buffer.
    uint8_t *buffer = (uint8_t*) malloc(file_size);

    if (!buffer) 
    {
        fclose(file);
        return NULL;
    }

    // Read value into buffer.
    long bytesRead = fread(buffer, 1, file_size, file);

    if (bytesRead != file_size) 
    {
        fclose(file);
        return NULL;
    }

    cart->file_size = file_size;

    // Tidy up.
    fclose(file);

    return buffer;
}

static void encode_rom_title(Header *header, uint8_t *rom)   // Loads cartridge title from ROM.
{
    int size = 15; 
    for (int i = 0; i < size - 1; i++) 
    {
        header->title[i] =  rom[TITLE_ADDRESS + i];
    }
    header->title[size - 1] = '\0';
}

static void encode_header(Header *header, uint8_t *rom)      // Load header data into struct.
{
    header->cart_code = rom[MBC_SCHEMA_ADDRESS];
    header-> cgb_code = rom[COLOR_MODE_ENABLE_ADDRESS];
    header-> checksum = rom[CHECKSUM_ADDRESS];
    header->dest_code = rom[DESTINATION_ADDRESS];
    header->  nl_code = rom[NEW_PUBLISHER_ADDRESS];
    header->  ol_code = rom[OLD_PUBLISHER_ADDRESS];
    header->  version = rom[VERSION_ADDRESS];
    header-> rom_code = rom[ROM_SETTINGS_ADDRESS];
    header-> ram_code = rom[RAM_SETTINGS_ADDRESS];
    encode_rom_title(header, rom);
}

// RAM Read/Write

static uint8_t get_address_code(uint16_t address)
{
    return (address >> 13);
}

static uint8_t read_rom(Cartridge *cart, uint16_t bank, uint16_t address)
{
    bank &= cart->rom_bank_mask;
    uint32_t offset = bank * ROM_BANK_SIZE;
    return cart->rom[offset + address];
}

static uint8_t read_ram(Cartridge *cart, uint8_t bank, uint16_t address)
{
    if (!cart->ram_enabled) 
        return OPEN_BUS;

    uint32_t  index = address - EXT_RAM_START;
    uint32_t offset = bank * RAM_BANK_SIZE;
    index += offset;

    return 
        cart->ram[index % cart->ram_size];
}

static void write_ram(Cartridge *cart, uint16_t bank, uint16_t address, uint8_t value)
{
    if (!cart->ram_enabled) return;

    uint32_t  index = address - EXT_RAM_START;
    uint32_t offset = bank * RAM_BANK_SIZE;
    index += offset;
    cart->ram[index % cart->ram_size] = value;
}


// Memory Bank Controllers


/* READ ONLY */

static uint8_t read_rom_only(Cartridge *cart, uint16_t address)
{
    switch(get_address_code(address))
    {
        case 0: // $0000-$1FFF
        case 1: // $2000-$3FFF
            return read_rom(cart, 0, address);

        case 2: // $4000-$5FFF
        case 3: // $6000-$7FFF
            return read_rom(cart, 1, (address - ROM_BANK_SIZE));   
    }

    return OPEN_BUS;
}

static void write_rom_only(Cartridge *cart, uint16_t address, uint8_t value)
{
    return;
}

/* MBC1 */

static uint8_t rom_bank_sel_mbc1(Cartridge *cart)
{
    uint8_t bank = cart->lower; 
    bank &= LOWER_5_MASK;
    bank  = (bank == 0) ? 1 : bank;
    bank |= (cart->upper << 5);
    bank  = (bank >= cart->rom_bank_quantity) ? (bank & cart->rom_bank_mask) : bank; 
    return bank;
}

static uint8_t read_mbc1(Cartridge *cart, uint16_t address)
{
    uint8_t rom_bank = 0;

    switch(get_address_code(address))
    {
        case 0: // $0000 - $1FFF
        case 1: // $2000 - $3FFF
            rom_bank = (cart->upper_bank_enabled && (cart->mode == RAM_MODE)) ?
            (cart->upper << 5) : 0;
            return read_rom(cart, rom_bank, address);

        case 2: // $4000 - $5FFF
        case 3: // $6000 - $7FFF
            rom_bank = rom_bank_sel_mbc1(cart);
            return read_rom(cart, rom_bank, (address - ROM_BANK_SIZE));

        case 5: // $A000 - $BFFF
            uint8_t ram_bank = (cart->mode == RAM_MODE) ? cart->upper : 0;
            return read_ram(cart, ram_bank, address);
    }

    return OPEN_BUS;
}

static void write_mbc1(Cartridge *cart, uint16_t address, uint8_t value)
{
    switch(get_address_code(address))
    {
        case 0: // $0000 - $1FFF - RAM Enable
            cart->ram_enabled = ((value & LOWER_4_MASK) == 0x0A);
            return;
            
        case 1: // $2000 - $3FFF - ROM Bank, Lower 5
            cart->lower = value & LOWER_5_MASK;
            return;

        case 2: // $4000 - $5FFF - RAM Bank, Upper 2
            cart->upper = value & LOWER_2_MASK;
            return;

        case 3: // $6000 - $7FFF - Mode Select, (0 - ROM) (1 - RAM)
            cart->mode = value & BIT_0_MASK;
            return;

        case 5: // $A000 - $BFFF
            uint8_t ram_bank = (cart->mode == RAM_MODE) ? cart->upper : 0; 
            write_ram(cart, ram_bank, address, value);
            return;
    }
}

/* MBC2 */

static uint8_t read_ram_mbc2(Cartridge *cart, uint16_t address)
{
    if (!cart->ram_enabled) return OPEN_BUS;
    
    address -= EXT_RAM_START;
    address %= 0x0200;
    return ((cart->ram[address] & 0x0F) | 0xF0);
}

static void write_ram_mbc2(Cartridge *cart, uint16_t address, uint8_t value)
{
    if (!cart->ram_enabled) return;
    
    address -= EXT_RAM_START;
    address %= 0x0200;
    cart->ram[address] = value;
}

static uint8_t read_mbc2(Cartridge *cart, uint16_t address)
{
    switch(get_address_code(address))
    {
        case 0: // $0000 - $1FFF
        case 1: // $2000 - $3FFF
            return read_rom(cart, 0, address);
        
        case 2: // $4000 - $5FFF
        case 3: // $6000 - $7FFF
            uint8_t rom_bank = cart->lower;
            rom_bank = (rom_bank == 0) ? 1 : rom_bank;
            return read_rom(cart, rom_bank, (address - ROM_BANK_SIZE));

        case 4: 
            return OPEN_BUS;
    
        case 5: // $A000 - $BFFF
            return read_ram_mbc2(cart, address);  
    }

    return OPEN_BUS;
}

static void write_mbc2(Cartridge *cart, uint16_t address, uint8_t value)
{
    switch(get_address_code(address))
    {
        case 0: // $0000 - $1FFF
        case 1: // $2000 - $3FFF

            if ((address & 0x0100) == 0)
            {
                cart->ram_enabled = ((value & LOWER_4_MASK) == 0x0A);
                return;
            }
            
            value &= LOWER_4_MASK;
            cart->lower = value;
            return;
       
        case 2:
        case 3:
        case 4:
            return;

        case 5: // $A000 - $BFFF
            write_ram_mbc2(cart, address, value);
            break;
    }
}

/* MBC3 */

static uint8_t read_rtc(Cartridge *cart, uint8_t code)
{
    switch(code)
    {
        case 0x08: return cart->clock.rtc_s;
        case 0x09: return cart->clock.rtc_m;
        case 0x0A: return cart->clock.rtc_h;
        case 0x0B: return cart->clock.rtc_dl;
        case 0x0C: return cart->clock.rtc_dh;
    }

    return OPEN_BUS;
}

static void write_rtc(Cartridge *cart, uint8_t code, uint8_t value)
{
    switch(code)
    {
        case 0x08: cart->clock.rtc_s  =   value % 60; break;
        case 0x09: cart->clock.rtc_m  =   value % 60; break;
        case 0x0A: cart->clock.rtc_h  =   value % 24; break;
        case 0x0B: cart->clock.rtc_dl =        value; break;
        case 0x0C: cart->clock.rtc_dh = value & 0xC1; break;
    }
}

static void latch_clock(Cartridge *cart, uint8_t value)
{
    static uint8_t prev = 0;
    bool triggered = ((value == 0x01) && (prev == 0x00));

    if (triggered)
    {
        cart->clock. rtc_s = cart->clock. live_s;
        cart->clock. rtc_m = cart->clock. live_m;
        cart->clock. rtc_h = cart->clock. live_h;
        cart->clock.rtc_dl = cart->clock.live_dl; 
    }

    prev = value;
}

static uint8_t read_mbc3(Cartridge *cart, uint16_t address)
{
    uint8_t rom_bank = 0;

    switch(get_address_code(address))
    {
        case 0: // $0000 - $1FFF
        case 1: // $2000 - $3FFF
            return read_rom(cart, rom_bank, address);
        
        case 2: // $4000 - $5FFF
        case 3: // $6000 - $7FFF    
            rom_bank = (cart->lower == 0) ? 1 : cart->lower;
            return read_rom(cart, rom_bank, (address - ROM_BANK_SIZE));

        case 5: // $A000 - $BFFF

            if (cart->mode == RAM_MODE)
            {
                uint8_t bank = cart->upper;
                return read_ram(cart, bank, address);
            }
            // RTC_MODE
            uint8_t code = cart->upper;
            return read_rtc(cart, code);
    }

    return OPEN_BUS; 
}

static void write_mbc3(Cartridge *cart, uint16_t address, uint8_t value)
{
    switch(get_address_code(address))
    {
        case 0: // $0000 - $1FFF
            cart->ram_enabled = ((value & LOWER_4_MASK) == 0x0A);
            break;

        case 1: // $2000 - $3FFF
            cart->lower = value & LOWER_7_MASK;
            break;

        case 2: // $4000 - $5FFF
            cart->upper = value & LOWER_4_MASK;
            cart->mode = (value > 0x07) ? RTC_MODE : RAM_MODE;
            break;
        
        case 3: // $6000 - $7FFF - Latch.
            latch_clock(cart, value);
            break;

        case 5: // $A000 - $BFFF

            if (!cart->ram_enabled) return; 

            if (cart->mode == RAM_MODE)
            {
                uint8_t bank = cart->upper;
                write_ram(cart, bank, address, value);
                return;
            }

            // RTC_MODE
            uint8_t code = cart->upper;
            write_rtc(cart, code, value);
            break;
    }
}

/* MBC5 */

static uint8_t read_mbc5(Cartridge *cart, uint16_t address)
{
    switch(get_address_code(address))
    {
        case 0: // $0000 - $1FFF
        case 1: // $2000 - $3FFF
            return read_rom(cart, 0, address);
        
        case 2: // $4000 - $5FFF
        case 3: // $6000 - $7FFF
            uint16_t rom_bank = (cart->mbc5_upper << 8) | cart->lower;
            return read_rom(cart, rom_bank, (address - ROM_BANK_SIZE));
        
        case 4: // $8000 - $9FFF
            return OPEN_BUS;
    
        case 5: // $A000 - $BFFF
            uint8_t ram_bank = cart->upper;
            return read_ram(cart, ram_bank, address);
    }

    return OPEN_BUS;
}

static void write_mbc5(Cartridge *cart, uint16_t address, uint8_t value)
{
    switch(get_address_code(address))
    {
        case 0: // $0000 - $1FFF (RAM ENABLE)

            cart->ram_enabled = ((value & LOWER_4_MASK) == 0x0A);
            return;

        case 1: // $2000 - $3FFF (ROM BANK SEL)

            if (address <= 0x2FFF)
            {
                cart->lower = value;
                return;
            }

            cart->mbc5_upper = value & BIT_0_MASK;
            return;

        case 2: // $4000 - $5FFF (RAM BANK SEL)

            cart->upper = value & LOWER_4_MASK;
            return;

        case 5: // $A000 - $BFFF
            uint8_t ram_bank = cart->upper;
            write_ram(cart, ram_bank, address, value);
    }
}

// Cart API

char *get_cart_info(Cartridge *cart, char *buffer, size_t size)
{
    snprintf(
        buffer,
        size,
        "ROM [%d]-[%02X] RAM [%d]-[%d] [%d]-[%02X|%02X]",
        cart->rom_bank_quantity,
        cart->rom_bank_mask,
        cart->ram_enabled,
        cart->ram_bank_quantity,
        cart->mode,
        cart->upper,
        cart->lower
    );

    return buffer;
}

void load_cartridge_save(Cartridge *cart)
{
    if (save_file_needed(cart))
        save_game(cart);
    
    load_game(cart);
}

uint8_t read_cartridge(Cartridge *cart, uint16_t address)
{
    return cart->cartridge_reader(cart, address);
}

void rtc_tick_day(Cartridge *cart)
{
    cart->clock.live_dl++;

    if (cart->clock.live_dl != 0) 
        return;

    bool upper_bit = ((cart->clock.live_dh & BIT_0_MASK) != 0);

    cart->clock.live_dh = upper_bit ? (cart->clock.live_dh | BIT_7_MASK) : (cart->clock.live_dh | BIT_0_MASK);
}

void rtc_tick_hour(Cartridge *cart)
{
    cart->clock.live_h = (cart->clock.live_h + 1) % 24;

    if (cart->clock.live_h == 0) 
        rtc_tick_day(cart);
}

void rtc_tick_minute(Cartridge *cart)
{
    cart->clock.live_m = (cart->clock.live_m + 1) % 60;

    if (cart->clock.live_m == 0) 
        rtc_tick_hour(cart);
}

void rtc_tick_second(Cartridge *cart)
{
    bool halted = ((cart->clock.live_dh & BIT_6_MASK) != 0);

    if (halted) 
        return;

    cart->clock.live_s = (cart->clock.live_s + 1) % 60; 
    
    if (cart->clock.live_s == 0) 
        rtc_tick_minute(cart);
}

void save_cartridge(Cartridge *cart)
{
    save_game(cart); // Call overhead not needed but not called often enough to matter.
}

void set_bios(Cartridge *cart, uint8_t value)
{
    cart->bios_locked = (value != 0);
}

void write_cartridge(Cartridge *cart, uint16_t address, uint8_t value)
{
    cart->cartridge_writer(cart, address, value);
}

// Cart Initialization

static uint8_t compute_bank_mask(uint8_t quantity)
{
    uint8_t mask = 0x00;
    quantity -= 1;

    while ((quantity & mask) != quantity)
        mask = (mask << 1) + 1;

    return mask;
}

static void encode_rom_settings(Cartridge *cart)
{
    switch(cart->header.rom_code)
    {
        case 0x00: cart->rom_bank_quantity = (uint16_t)   2; break;
        case 0x01: cart->rom_bank_quantity = (uint16_t)   4; break;
        case 0x02: cart->rom_bank_quantity = (uint16_t)   8; break;
        case 0x03: cart->rom_bank_quantity = (uint16_t)  16; break;
        case 0x04: cart->rom_bank_quantity = (uint16_t)  32; break;
        case 0x05: cart->rom_bank_quantity = (uint16_t)  64; break;
        case 0x06: cart->rom_bank_quantity = (uint16_t) 128; break;
        case 0x07: cart->rom_bank_quantity = (uint16_t) 256; break;
        case 0x08: cart->rom_bank_quantity = (uint16_t) 512; break;
        default:   cart->rom_bank_quantity = (uint16_t)   2; break; 
    }
}

static void encode_ram_settings(Cartridge *cart)
{
    switch (cart->header.ram_code)
    {
        case 0x00: cart->ram_bank_quantity = (uint8_t)  0; break;
        case 0x01: cart->ram_bank_quantity = (uint8_t)  0; break;
        case 0x02: cart->ram_bank_quantity = (uint8_t)  1; break;
        case 0x03: cart->ram_bank_quantity = (uint8_t)  4; break; 
        case 0x04: cart->ram_bank_quantity = (uint8_t) 16; break;
        case 0x05: cart->ram_bank_quantity = (uint8_t)  8; break;
        default:   cart->ram_bank_quantity = (uint8_t)  1; break;
    }
}

static void encode_cartridge(Cartridge *cart)
{
    encode_header(&(cart->header), cart->rom);

    encode_rom_settings(cart);
    encode_ram_settings(cart);
    cart->rom_bank_mask = compute_bank_mask(cart->rom_bank_quantity);
    cart->         mode = ROM_MODE;
    cart->  ram_enabled =    false;
    cart->  bios_locked =    false;
    cart->       is_gbc = ((cart->header.cgb_code == 0x80) || (cart->header.cgb_code == 0xC0)); 
    cart->        lower =        1;
    cart->        upper =        0;
    cart->   mbc5_upper =        0;

    cart->upper_bank_enabled = (cart->file_size >= 0x100000);

    switch (cart->header.cart_code)
    {
        case ROM_ONLY:
            cart->cartridge_reader = read_rom_only;
            cart->cartridge_writer = write_rom_only;
            break;

        case MBC1:
        case MBC1_RAM:
        case MBC1_RAM_BATTERY:
            cart->cartridge_reader = read_mbc1;
            cart->cartridge_writer = write_mbc1;
            break;

        case MBC2:
        case MBC2_BATTERY:
            cart->cartridge_reader = read_mbc2;
            cart->cartridge_writer = write_mbc2;
            break;

        case MBC3:
        case MBC3_RAM:
        case MBC3_RAM_BATTERY:
        case MBC3_TIMER_BATTERY:
        case MBC3_TIMER_RAM_BATTERY:
            cart->cartridge_reader = read_mbc3;
            cart->cartridge_writer = write_mbc3;
            break;

        case MBC5:
        case MBC5_RAM:
        case MBC5_RAM_BATTERY:
        case MBC5_RUMBLE:
        case MBC5_RUMBLE_RAM:
        case MBC5_RUMBLE_RAM_BATTERY:
            cart->cartridge_reader = read_mbc5;
            cart->cartridge_writer = write_mbc5;
            break;
            
        default:
            return;
    }
}

static void init_ram(Cartridge *cart)
{
    cart->ram_bank_quantity = (cart->ram_bank_quantity == 0) ? 1 : cart->ram_bank_quantity;
    cart->         ram_size = cart->ram_bank_quantity * RAM_BANK_SIZE;

    cart->ram = (uint8_t*) malloc(cart->ram_size * sizeof(uint8_t));

    if (!cart->ram)
    {
        fprintf(stderr, "Failed to allocate RAM\n");
        exit(EXIT_FAILURE);
    }

    memset(cart->ram, 0, cart->ram_size);
}
 
static void init_rtcc(Cartridge *cart)
{
    cart-> clock.rtc_s = 0; 
    cart-> clock.rtc_m = 0;
    cart-> clock.rtc_h = 0;
    cart->clock.rtc_dl = 0;
    cart->clock.rtc_dh = 0;
}

Cartridge *init_cartridge(const char *file_path, const char *file_name)
{
    Cartridge *cart = (Cartridge*) malloc(sizeof(Cartridge));

    cart->file_name = (char*) malloc(strlen(file_name) + 1);
    strcpy(cart->file_name, file_name);

    cart->file_path = (char*) malloc(strlen(file_path) + 1);
    strcpy(cart->file_path, file_path);

    cart->      rom = get_rom_content(cart, file_path);

    encode_cartridge(cart);
    init_ram(cart);
    init_rtcc(cart);

    return cart;
}

void tidy_cartridge(Cartridge **cart)
{
    free((*cart)->file_name); 
    (*cart)->file_name = NULL;
    
    free((*cart)->file_path); 
    (*cart)->file_path = NULL;
    
    free((*cart)->ram); (*cart)->ram = NULL;
    free((*cart)->rom); (*cart)->rom = NULL;
    free(*cart);               *cart = NULL;
}
#ifndef CART_H
#define CART_H

#include <stdbool.h>
#include <stdint.h>

#define SAVE_DIR    "saves"
#define MAX_FILE_PATH  256

#define DEFAULT_RAM_BANK 0
#define DEFAULT_ROM_BANK 1

typedef enum
{
    TITLE_ADDRESS             = (uint16_t) 0x0134,
    COLOR_MODE_ENABLE_ADDRESS = (uint16_t) 0x0143,
    NEW_PUBLISHER_ADDRESS     = (uint16_t) 0x0144,
    MBC_SCHEMA_ADDRESS        = (uint16_t) 0x0147,
    DESTINATION_ADDRESS       = (uint16_t) 0x014A,
    OLD_PUBLISHER_ADDRESS     = (uint16_t) 0x014B,
    VERSION_ADDRESS           = (uint16_t) 0x014C,
    CHECKSUM_ADDRESS          = (uint16_t) 0x014D,
    ROM_SETTINGS_ADDRESS      = (uint16_t) 0x0148,
    RAM_SETTINGS_ADDRESS      = (uint16_t) 0x0149

} HeaderAddress;

typedef enum
{
    ROM_BANK_SIZE = (uint16_t) 0x4000,
    RAM_BANK_SIZE = (uint16_t) 0x2000,

    ROM_MODE = (uint8_t) 0x00,
    RAM_MODE = (uint8_t) 0x01,
    RTC_MODE = (uint8_t) 0x02,

} MbcConstant;

typedef enum
{
    ROM_ONLY                       = (uint8_t) 0x00,
    MBC1                           = (uint8_t) 0x01,
    MBC1_RAM                       = (uint8_t) 0x02,
    MBC1_RAM_BATTERY               = (uint8_t) 0x03,
    MBC2                           = (uint8_t) 0x05,
    MBC2_BATTERY                   = (uint8_t) 0x06,
    MMM01                          = (uint8_t) 0x0B,
    MMM01_RAM                      = (uint8_t) 0x0C,
    MMM01_RAM_BATTERY              = (uint8_t) 0x0D, 
    MBC3_TIMER_BATTERY             = (uint8_t) 0x0F,
    MBC3_TIMER_RAM_BATTERY         = (uint8_t) 0x10,
    MBC3                           = (uint8_t) 0x11,
    MBC3_RAM                       = (uint8_t) 0x12,
    MBC3_RAM_BATTERY               = (uint8_t) 0x13,
    MBC5                           = (uint8_t) 0x19,
    MBC5_RAM                       = (uint8_t) 0x1A,
    MBC5_RAM_BATTERY               = (uint8_t) 0x1B,
    MBC5_RUMBLE                    = (uint8_t) 0x1C,
    MBC5_RUMBLE_RAM                = (uint8_t) 0x1D,
    MBC5_RUMBLE_RAM_BATTERY        = (uint8_t) 0x1E,
    MBC6                           = (uint8_t) 0x20,
    MBC7_SENSOR_RUMBLE_RAM_BATTERY = (uint8_t) 0x22

} CartridgeCode;

typedef enum
{
    RTC_S  = 0x08,
    RTC_M  = 0x09,
    RTC_H  = 0x0A,
    RTC_DL = 0x0B,
    RTC_DH = 0x0C

} ClockRegisterCodes;

typedef struct
{ 
    char     title[15]; // Game Title        | 0x0134 - 0x0143
    uint8_t   cgb_code; // Enable Color Mode | 0x0143 - 0x0144
    uint16_t   nl_code; // Game's publisher  | 0x0144 - 0x0146
    uint8_t  cart_code; // Mapping schema    | 0x0147 - 0x0148
    uint8_t   rom_code; // ROM Settings      | 0x0148 - 0x0149
    uint8_t   ram_code; // RAM Settings      | 0x0149 - 0x014A
    uint8_t  dest_code; // Destination code  | 0x014A - 0x014B
    uint8_t    ol_code; // Old license code  | 0x014B - 0x014C
    uint8_t    version; // Version number    | 0x014C - 0x014D
    uint8_t   checksum; // Header checksum   | 0x014D - 0x014E

} Header;

typedef struct
{
    uint8_t  live_s;
    uint8_t  live_m;
    uint8_t  live_h;
    uint8_t live_dl;
    uint8_t live_dh;

    uint8_t   rtc_s;
    uint8_t   rtc_m;
    uint8_t   rtc_h;
    uint8_t  rtc_dl;
    uint8_t  rtc_dh;

    uint8_t prev_latch_value;

} RTCC; // Real-Time Cartridge-Clock

typedef struct Cartridge
{
    // Handlers
    uint8_t (*cartridge_reader)(struct Cartridge*, uint16_t);
    void    (*cartridge_writer)(struct Cartridge*, uint16_t, uint8_t);

    // Mutable State
    bool upper_bank_enabled;
    bool        ram_enabled;
    bool        bios_locked;
    bool             is_gbc;
    MbcConstant        mode;
    uint8_t           lower;
    uint8_t           upper;
    uint8_t      mbc5_upper;

    // 'Immutable' State
    uint8_t  ram_bank_quantity;
    uint16_t rom_bank_quantity;
    uint8_t      rom_bank_mask;
    uint32_t          ram_size;

    // Accessories
    RTCC    clock;
    Header header;
    
    // Memory
    uint8_t *rom;
    uint8_t *ram;
    
    // Meta Data
    char *file_path;
    char *file_name;
    long  file_size;

} Cartridge;

char *get_cart_info(Cartridge *cart, char *buffer, size_t size);

void load_cartridge_save(Cartridge *cart);

Cartridge *init_cartridge(const char *file_path, const char *file_name);

uint8_t read_cartridge(Cartridge *cart, uint16_t address);

void rtc_tick_day(Cartridge *cart);

void rtc_tick_hour(Cartridge *cart);

void rtc_tick_minute(Cartridge *cart);

void rtc_tick_second(Cartridge *cart);

void save_cartridge(Cartridge *cart);

void tidy_cartridge(Cartridge **cart);

void write_cartridge(Cartridge *cart, uint16_t address, uint8_t value);

#endif
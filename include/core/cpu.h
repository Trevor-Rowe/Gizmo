#ifndef CPU_H
#define CPU_H

#include <stdint.h>
#include <stdbool.h>

typedef struct Cartridge Cartridge;
typedef struct EmuMemory EmuMemory;
typedef struct EmuTimer EmuTimer;
typedef struct GbcEmu GbcEmu;
typedef struct CPU CPU;

typedef enum
{
    BASE_CLOCK_SPEED     = 4194304,
    DEFAULT_REG_VAL      =       0,
    M2S_BASE_SPEED       =       4,
    M2S_DOUBLE_SPEED     =       2,
    DIV_INC_PERIOD       =      64,

} CpuDefaults;

typedef enum
{
    ZERO_FLAG       = 0b10000000, // Z
    SUBTRACT_FLAG   = 0b01000000, // N
    HALF_CARRY_FLAG = 0b00100000, // H
    CARRY_FLAG      = 0b00010000  // C
      
} Flag;

typedef enum
{
    VBLANK_INTERRUPT_CODE   = 0x01,
    LCD_STAT_INTERRUPT_CODE = 0x02,
    TIMER_INTERRUPT_CODE    = 0x04,
    SERIAL_INTERRUPT_CODE   = 0x08,
    JOYPAD_INTERRUPT_CODE   = 0x10
    
} InterruptCode;

typedef enum
{
    AF_REG = 0x00,
    BC_REG = 0x01,
    DE_REG = 0x02,
    HL_REG = 0x03,
    SP_REG = 0x04

} DualRegister;

typedef enum
{
    RISING  = 0, 
    FALLING = 1
    
} EdgeState;

/* STATE MANGEMENT */

typedef struct
{
    uint8_t delay;
    bool   active;

} InterruptEnableEvent;

typedef struct
{
    // CPU Registers
    uint8_t     A; uint8_t      F; // Accumulator          | Flags
    uint8_t     B; uint8_t      C; // General Purpose      | -
    uint8_t     D; uint8_t      E; // General Purpose      | -
    uint8_t     H; uint8_t      L; // Memory Addressing    | -
    uint16_t   PC; uint16_t    SP; // Program Counter      | Stack Pointer
    // Hardware Registers
    volatile uint8_t  *IER; volatile uint8_t   *IFR;

} Register;

typedef struct
{
    uint16_t  address;
    uint8_t  duration;
    uint8_t    length;
    uint8_t       low;
    uint8_t      high;
    uint8_t    opcode;
    char       *label;
    bool     executed;
    bool  cb_prefixed;

    bool (*handler)(CPU*);

} InstructionEntity;

typedef struct CPU
{
    uint8_t              ime_delay;
    
    volatile bool              ime;
    volatile bool    ime_scheduled;
    volatile bool    speed_enabled;
    volatile bool          running;
    volatile bool           halted;
    volatile bool  halt_bug_active;

    Register             reg;
    InstructionEntity    ins;

    Cartridge *cart;
    EmuMemory  *mem;
    EmuTimer *timer;

} CPU;

char *get_cpu_state(CPU *cpu, char *buffer, size_t size);

char *get_reg_state(CPU *cpu, char *buffer, size_t size);

void request_interrupt(CPU *cpu, InterruptCode interrupt);

void machine_cycle(CPU *cpu);

void reset_cpu(CPU *cpu);

void start_cpu(CPU *cpu);

void stop_cpu(CPU *cpu);

void link_cpu(CPU *cpu, GbcEmu *emu);

CPU *init_cpu();

void tidy_cpu(CPU **cpu);

#endif
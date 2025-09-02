#ifndef COMMON_H
#define COMMON_H

#include <stdbool.h>
#include <stdint.h>

#define OPEN_BUS 0xFF

typedef enum
{
    BIT_0_MASK      =  0b00000001,
    BIT_1_MASK      =  0b00000010,
    BIT_2_MASK      =  0b00000100,
    BIT_3_MASK      =  0b00001000,
    BIT_4_MASK      =  0b00010000,
    BIT_5_MASK      =  0b00100000,
    BIT_6_MASK      =  0b01000000,
    BIT_7_MASK      =  0b10000000,
    BIT_10_MASK     =      0x0400,
    BIT_11_MASK     =      0x0800,
    LOWER_2_MASK    =  0b00000011,
    LOWER_3_MASK    =  0b00000111,
    LOWER_4_MASK    =  0b00001111,
    LOWER_5_MASK    =  0b00011111, 
    LOWER_6_MASK    =  0b00111111,
    LOWER_7_MASK    =  0b01111111,
    LOWER_11_MASK   =      0x07FF,
    LOWER_12_MASK   =      0x0FFF,
    LOWER_14_MASK   =      0x3FFF,
    LOWER_BYTE_MASK =      0x00FF,
    UPPER_4_MASK    =        0xF0,
    UPPER_5_MASK    =        0xF8, 
    UPPER_BYTE_MASK =      0xFF00,

} BitMask;

typedef enum
{
    BYTE           =       8,
    BYTE_OVERFLOW  =    0x00,
    BYTE_UNDERFLOW =    0xFF,
    IE_NEXT_INS    =       2,
    MAX_INT_16     =  0xFFFF,
    MAX_INT_8      =    0xFF,
    NIBBLE         =       4,

} CommonConstants;

#endif
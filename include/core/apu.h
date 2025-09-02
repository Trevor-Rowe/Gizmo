#ifndef APU_H
#define APU_H

#include <stdint.h>
#include <stdbool.h>

#define PERIOD_OVERFLOW (uint16_t) 0x07FF

typedef struct Joypad Joypad;
typedef struct GbcEmu GbcEmu;
typedef struct EmuMemory EmuMemory;

typedef enum
{
    PULSE_ONE = 1,
    PULSE_TWO = 2,
    WAVE      = 3,
    NOISE     = 4

} ChannelName;

typedef struct
{
    bool calc_occured_negate_mode;
    bool              negate_mode;
    bool       freq_sweep_enabled;
    uint8_t     freq_sweep_thresh;
    uint8_t      freq_sweep_timer;
    uint16_t               shadow;

} FrequencySweepUnit;

typedef struct
{
    ChannelName name;

    bool dac_enabled;
    bool     enabled;
    uint8_t   output;

    uint8_t    phase;

    // Volume Envelope
    bool    volume_envelope_enabled;
    uint8_t   volume_envelope_timer;

    // Length
    bool     length_timer_enabled;
    uint16_t         length_timer;

    // Waveform Handling
    uint8_t   volume;
    uint8_t     step;
    uint8_t    timer;
    uint16_t divider;
    uint16_t    lfsr;

    // Hardware Registers
    uint8_t    *nrx0;
    uint8_t    *nrx1;
    uint8_t    *nrx2;
    uint8_t    *nrx3;
    uint8_t    *nrx4;

} Channel;

typedef struct APU
{
    // State
    bool             powered;
    uint8_t            frame;
    
    // Architecture
    Channel ch1; Channel ch2;
    Channel ch3; Channel ch4;

    FrequencySweepUnit fsu;

    // Hardware
    uint8_t  *nr50;
    uint8_t  *nr51;
    uint8_t  *nr52;
    uint8_t *pcm12;
    uint8_t *pcm34;

    uint8_t *wave_ram;

    // Emulation
    Joypad *joypad;
    EmuMemory *mem;

} APU;

int16_t sample_left_channel(APU *apu);

int16_t sample_right_channel(APU *apu);

void div_apu_event(APU *apu);

void apu_dot(APU *apu);

void write_audio_register(APU *apu, uint16_t address, uint8_t value);

void link_apu(APU *apu, GbcEmu *emu);

APU *init_apu();

void tidy_apu(APU **apu);

#endif
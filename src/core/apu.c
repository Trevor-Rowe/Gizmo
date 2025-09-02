#include <stdlib.h>
#include <string.h>

#include "core/emulator.h"
#include "core/apu.h"
#include "core/mmu.h"
#include "core/timer.h"

#include "util/common.h"

static uint16_t noise_period = 8; 

static const uint8_t wave_forms[4][8] = 
{
    [0] = { 1, 1, 1, 1, 1, 1, 1, 0 },
    [1] = { 0, 1, 1, 1, 1, 1, 1, 0 },
    [2] = { 0, 1, 1, 1, 1, 0, 0, 0 },
    [3] = { 1, 0, 0, 0, 0, 0, 0, 1 } 
};

static const int16_t dac_table[16] = 
{
     32767,  28377,  23987,  19597,
     15207,  10817,   6427,   2037,
     -2353,  -6743, -11133, -15523,
    -19913, -24303, -28693, -32768
};

static const float volume_table[8] = 
{
    0.125, 0.250, 0.375, 0.500, 0.625, 0.750, 0.875, 1.000 
};

static void sync_nr52(APU *apu)
{
    uint8_t nr52 = *apu->nr52 & BIT_7_MASK; // Preserve APU Power flag

    if (apu->ch1.enabled) 
        nr52 |= BIT_0_MASK;
    
    if (apu->ch2.enabled) 
        nr52 |= BIT_1_MASK;
    
    if (apu->ch3.enabled) 
        nr52 |= BIT_2_MASK;

    if (apu->ch4.enabled) 
        nr52 |= BIT_3_MASK;

    *apu->nr52 = nr52;
}

static inline void enable_fsu(FrequencySweepUnit *fsu) // Purely Idiomatic
{
    fsu->freq_sweep_enabled = true;
}

static inline void disable_fsu(FrequencySweepUnit *fsu) // Purely Idiomatic
{
    fsu->freq_sweep_enabled = false;
}

static void disable_channel(APU *apu, Channel *ch)
{
    ch->                 output =     0;
    ch->                enabled = false;
    ch->volume_envelope_enabled = false;

    if (ch->name == PULSE_ONE)
        disable_fsu(&apu->fsu);

    sync_nr52(apu);
}

static inline uint16_t get_period(Channel *ch)
{
    uint16_t lower = *ch->nrx3;
    uint16_t upper = (*ch->nrx4) & LOWER_3_MASK;
    return ((upper << 8) | lower);
}

static inline void set_period(Channel *ch, uint16_t period)
{
    *ch->nrx3 = period & LOWER_BYTE_MASK;
    *ch->nrx4 = ((*ch->nrx4) & ~LOWER_3_MASK) | ((period >> 8) & LOWER_3_MASK);
}

// Length Functionality

static inline uint16_t init_length(Channel *ch)
{
    return (ch->name == WAVE) ? (256 - (*ch->nrx1)) : (64 - ((*ch->nrx1) & LOWER_6_MASK));
}

static inline bool length_timer_enabled(Channel *ch)
{
    return (((*ch->nrx4) & BIT_6_MASK) != 0);
}

static inline void length_timer(APU *apu, Channel *ch)
{
    if (!ch->length_timer_enabled) return;

    if (ch->length_timer > 0)
        ch->length_timer--;

    if (ch->length_timer == 0)
        disable_channel(apu, ch);
}

// Volume Envelope Functionality, NRX2 -> [INIT | DIR |  PACE]

static inline uint8_t init_volume(Channel *ch)
{
    if (ch->name == WAVE)
        return ((*ch->nrx2) >> 5) & LOWER_2_MASK;
    
    return ((*ch->nrx2) >> 4) & LOWER_4_MASK;
}

static inline bool volume_dir(Channel *ch)
{
    return (((*ch->nrx2) & BIT_3_MASK) != 0);
}

static inline uint8_t volume_pace(Channel *ch)
{
    return ((*ch->nrx2) & LOWER_3_MASK);
}

static void volume_envelope(Channel *ch)
{
    if (!ch->volume_envelope_enabled)
        return; 

    uint8_t pace = volume_pace(ch);
    bool     dir =  volume_dir(ch);

    if (pace == 0)
        return;

    ch->volume_envelope_timer++;
    if (ch->volume_envelope_timer < pace) return;
    ch->volume_envelope_timer = 0;

    if (dir && (ch->volume < 0x0F)) // increasing over time 
    {
        ch->volume++;
        ch->volume_envelope_enabled = (ch->volume != 0x0F);
        return;
    }

    if (!dir && (ch->volume > 0)) // decreasing over time
    {
        ch->volume--;
        ch->volume_envelope_enabled = (ch->volume != 0);
        return;
    }
}

// Frequency Sweep Unit, NR10 -> [PACE | DIR | STEP]

static inline uint8_t sweep_pace(APU *apu)
{
    return ((*apu->ch1.nrx0) >> 4) & LOWER_3_MASK;
}

static inline bool sweep_dir(APU *apu)
{
    return (((*apu->ch1.nrx0) & BIT_3_MASK) != 0);
}

static inline uint8_t sweep_step(APU *apu)
{
    return ((*apu->ch1.nrx0) & LOWER_3_MASK);
}

static inline void check_negate_latch_set(FrequencySweepUnit *fsu)
{
    if (fsu->negate_mode && !fsu->calc_occured_negate_mode)
        fsu->calc_occured_negate_mode = true; // Latch set
}

static inline void check_negate_transition(APU *apu)
{
    FrequencySweepUnit *fsu = &apu->fsu; // Want the actual pointer for side effects.

    bool prev_negate_mode = fsu->negate_mode;
    fsu->     negate_mode = sweep_dir(apu);

    if (sweep_pace(apu) == 0) 
    {
        fsu->calc_occured_negate_mode = false;
        return;
    }

    if (!fsu->negate_mode && prev_negate_mode && fsu->calc_occured_negate_mode)
    {
        disable_fsu(fsu);
        disable_channel(apu, &apu->ch1);
    }

    if (fsu->negate_mode)
        fsu->calc_occured_negate_mode = false; // Latch reset.
}

static inline uint16_t freq_sweep_calc(FrequencySweepUnit fsu, uint8_t step)
{
    uint16_t period = fsu.shadow;

    if (step == 0)
        return period;

    uint16_t delta = period >> step;

    // 1 = Negative, 0 = Positive
    return fsu.negate_mode ? (period - delta) : (period + delta);
}

static void calc_overflow_check(APU *apu, FrequencySweepUnit *fsu, uint8_t step)
{
    uint16_t period = freq_sweep_calc(*fsu, step);

    check_negate_latch_set(fsu);

    if (period > PERIOD_OVERFLOW)
    {
        disable_fsu(fsu);
        disable_channel(apu, &apu->ch1);
        return;
    }

    fsu->shadow = period;
    set_period(&apu->ch1, period);

    // 2nd Check

    period = freq_sweep_calc(*fsu, step);

    if (period > PERIOD_OVERFLOW)
    {
        disable_fsu(fsu);
        disable_channel(apu, &apu->ch1);
        return;
    }
}

static void freq_sweep(APU *apu)
{
    FrequencySweepUnit *fsu = &apu->fsu;

    if (!fsu->freq_sweep_enabled) 
        return;

    fsu->freq_sweep_timer++;
    if (fsu->freq_sweep_timer < fsu->freq_sweep_thresh) return;
    fsu->freq_sweep_timer = 0;

    // Register decoding. 
    uint8_t pace = sweep_pace(apu);
    uint8_t step = sweep_step(apu);
    
    fsu->freq_sweep_thresh = (pace == 0) ? 8 : pace; // Hardware quirk

    if (pace == 0) // Do not calculate if 0.
        return;

    if ((step == 0) && (fsu->shadow == PERIOD_OVERFLOW)) // Edge case
    {
        disable_fsu(fsu);
        disable_channel(apu, &apu->ch1);
    }

    calc_overflow_check(apu, fsu, step);
}

// Envelope Driving

static void clock_volume_envelopes(APU *apu)
{
    volume_envelope(&apu->ch1);
    volume_envelope(&apu->ch2);
    volume_envelope(&apu->ch4);
}

static void clock_length_timers(APU *apu)
{
    length_timer(apu, &apu->ch1);
    length_timer(apu, &apu->ch2);
    length_timer(apu, &apu->ch3);
    length_timer(apu, &apu->ch4);
}

void div_apu_event(APU *apu)
{
    if (!apu->powered) return;

    switch(apu->frame) // Frame Sequencer
    {
        case 0:
            clock_length_timers(apu);
            break;

        case 1: 
            break;

        case 2:
            freq_sweep(apu);
            clock_length_timers(apu);  
            break;
        
        case 3: 
            break;

        case 4:
            clock_length_timers(apu);
            break;

        case 5:
            break;

        case 6:
            freq_sweep(apu);
            clock_length_timers(apu);
            break;

        case 7:
            clock_volume_envelopes(apu);
            break;
    }

    apu->frame = (apu->frame + 1) % 8;
}

// Pulse Channel(s) Waveform Handling

static inline uint8_t get_duty_cycle(Channel *ch)
{
    return (*ch->nrx1 >> 6) & LOWER_2_MASK;
}

static void clock_pulse_divider(Channel *ch)
{
    ch->divider++;

    if (ch->divider > PERIOD_OVERFLOW)
    {
        ch->   divider = get_period(ch);
        ch->      step = (ch->step + 1) % 8;
        bool wave_high = (wave_forms[get_duty_cycle(ch)][ch->step] != 0);
        ch->    output = wave_high ? ch->volume : 0;
    }
}

static void clock_pulse_timer(Channel *ch)
{
    if (!ch->enabled)
        return;
    
    ch->timer++;
    if (ch->timer < 4) return;
    ch->timer = 0;

    clock_pulse_divider(ch);
}

// Wave Channel Waveform Handling

static void apply_coarse_wave_volume(Channel *ch)
{
    switch(init_volume(ch))
    {
        case 0: ch->output  *= 0; break;
        case 1: ch->output  *= 1; break;
        case 2: ch->output >>= 1; break;
        case 3: ch->output >>= 2; break;
    }
}

static void advance_general_waveform(APU *apu, Channel *ch)
{
    bool left_nibble = ((ch->step & BIT_0_MASK) == 0);

    uint8_t index = ch->step >> 1;
    uint8_t  byte = apu->wave_ram[index];

    ch->output = left_nibble ? (byte >> 4) : byte;
    ch->output &= LOWER_4_MASK;
    apply_coarse_wave_volume(ch);

    ch->step = (ch->step + 1) % 32;
}

static void clock_wave_divider(APU *apu, Channel *ch)
{
    ch->divider++;

    if (ch->divider > PERIOD_OVERFLOW)
    {
        ch->divider = get_period(ch);
        advance_general_waveform(apu, ch);
    }
}

static void clock_wave_timer(APU *apu)
{
    Channel *ch3 = &(apu->ch3);

    if (!ch3->enabled)
        return;

    if (ch3->phase != 0)
    {
        ch3->phase--;
        return;
    }

    ch3->timer++;
    if (ch3->timer < 2) return;
    ch3->timer = 0;

    clock_wave_divider(apu, ch3);
}

// Noise Channel Waveform Handling

static void clock_lfsr(Channel *ch)
{
    uint8_t      nr43 = *ch->nrx4;

    uint16_t     lfsr = ch->lfsr;
    bool        bit_0 = lfsr & BIT_0_MASK;
    bool        bit_1 = (lfsr >> 1) & BIT_0_MASK;
    uint16_t feedback = (~(bit_0 ^ bit_1)) & BIT_0_MASK;

    lfsr = (lfsr & 0x7FFF) | (feedback << 15);

    if ((nr43 & BIT_3_MASK) != 0) // Copy to bit 7 as well?
        lfsr = (lfsr & 0xFF7F) | (feedback << 7);
        
    lfsr >>= 1;

    ch->  lfsr = lfsr;
    ch->output = (feedback == 0) ? 0 : ch->volume; 
 }

static void clock_noise_timer(APU *apu)
{
    Channel *ch = &(apu->ch4); 

    if (!ch->enabled)
        return;
    
    ch->timer++;
    if (ch->timer < noise_period) return;
    ch->timer = 0;

    clock_lfsr(ch);
}

// Waveform Driver

void apu_dot(APU *apu)
{
    clock_pulse_timer(&apu->ch1);
    clock_pulse_timer(&apu->ch2);
    clock_wave_timer(apu);
    clock_noise_timer(apu);
}

// Channel Triggering

static inline bool length_enabled(Channel *ch)
{
    return (((*ch->nrx4) & BIT_6_MASK) != 0);
}

static void check_length_trigger(APU *apu, Channel *ch)
{
    bool        prev_enabled = ch->length_timer_enabled;
    ch->length_timer_enabled = length_enabled(ch);
    bool   first_period_half = ((apu->frame % 2) == 1);

    if (ch->length_timer_enabled && !prev_enabled && first_period_half)
        length_timer(apu, ch);
}

static inline void reset_length_timer(APU *apu, Channel *ch)
{
    ch->length_timer = (ch->name == WAVE) ? 256 : 64;
    ch->length_timer_enabled = false;
    check_length_trigger(apu, ch);
}

static inline bool channel_triggered(Channel *ch)
{
    return (((*ch->nrx4) & BIT_7_MASK) != 0);
}

static void check_channel_trigger(APU *apu, Channel *ch)
{
    if (!channel_triggered(ch)) 
        return;

    ch->enabled = ch->dac_enabled;

    // Reset length timer.
    if (ch->length_timer == 0)
        reset_length_timer(apu, ch);

    // Reset period divider.
    ch->divider = get_period(ch);

    // Reset envelope timer.
    ch->volume_envelope_enabled = true;
    ch->volume_envelope_timer = 0;

    // Reset volume.
    ch->volume = init_volume(ch);

    // Reset Waveform
    ch->step = 0;

    // Sync Status
    sync_nr52(apu);
}

// Left and Right Output

static inline bool ch_out_active(Channel *ch, uint8_t nr51, uint8_t mask)
{
    bool ch_out = ((nr51 & mask) != 0);
    ch_out &= ch->dac_enabled;
    return ch_out;
}

int16_t sample_left_channel(APU *apu)
{
    uint8_t panning = *apu->nr51;
    int32_t  output =          0;
    uint8_t  active =          0;

    uint8_t  volume = ((*apu->nr50) >> 4) & LOWER_3_MASK;

    if (ch_out_active(&apu->ch1, panning, BIT_4_MASK))
    {
        output += dac_table[apu->ch1.output];
        active++;
    }   

    if (ch_out_active(&apu->ch2, panning, BIT_5_MASK))
    {
        output += dac_table[apu->ch2.output];
        active++;
    }

    if (ch_out_active(&apu->ch3, panning, BIT_6_MASK))
    {
        output += dac_table[apu->ch3.output];
        active++;
    }

    if (ch_out_active(&apu->ch4, panning, BIT_7_MASK))
    {
        output += dac_table[apu->ch4.output];
        active++;
    }

    if (active == 0)
        return 0;

    int32_t mixed = (output / active) * volume_table[volume];

    return (int16_t) mixed;
} 

int16_t sample_right_channel(APU *apu)
{
    uint8_t panning = *apu->nr51;
    int32_t  output =          0;
    uint8_t  active =          0;

    uint8_t  volume = *apu->nr50 & LOWER_3_MASK;

    if (ch_out_active(&apu->ch1, panning, BIT_0_MASK))
    {
        output += dac_table[apu->ch1.output];
        active++;
    }   

    if (ch_out_active(&apu->ch2, panning, BIT_1_MASK))
    {
        output += dac_table[apu->ch2.output];
        active++;
    }

    if (ch_out_active(&apu->ch3, panning, BIT_2_MASK))
    {
        output += dac_table[apu->ch3.output];
        active++;
    }

    if (ch_out_active(&apu->ch4, panning, BIT_3_MASK))
    {
        output += dac_table[apu->ch4.output];
        active++;
    }

    if (active == 0)
        return 0;

    int32_t mixed = (output / active) * volume_table[volume];

    return (int16_t) mixed;
 }

// Channel 1 Writes

static void write_nr10(APU *apu, uint8_t value) // Channel 1 Sweep
{ 
    static uint8_t iteration = 0;

    Channel *ch1 = &apu->ch1;

    *ch1->nrx0 = value;

    check_negate_transition(apu); 

    iteration++;

   // cpu_log(INFO, " [NR10] <- %02X, iter: %d", value, iteration);
}
static void write_nr11(APU *apu, uint8_t value) // Channel 1 Length
{
    Channel *ch1 = &apu->ch1;

    *ch1->nrx1 = value;

    ch1->length_timer = init_length(ch1);
}
static void write_nr12(APU *apu, uint8_t value) // Channel 1 DAC & Volume
{
    Channel *ch1 = &apu->ch1;

    *ch1->nrx2 = value;

    ch1->dac_enabled = ((value & UPPER_5_MASK) != 0); // Slight optimization to calc here.

    if (!ch1->dac_enabled && ch1->enabled)
        disable_channel(apu, ch1);
}
static void write_nr13(APU *apu, uint8_t value) // Channel 1 Low-Byte of Period
{
    Channel *ch1 = &apu->ch1;

    *ch1->nrx3 = value;
}
static void write_nr14(APU *apu, uint8_t value) // Channel 1 Trigger, Length Enable, Upper-3 Period Bits
{
    FrequencySweepUnit *fsu = &apu->fsu;
    Channel            *ch1 = &apu->ch1;

    *ch1->nrx4 = value;

    check_length_trigger(apu, ch1);
    check_channel_trigger(apu, ch1);

    uint8_t pace = sweep_pace(apu);
    uint8_t step = sweep_step(apu);

    if ((pace == 0) && (step == 0))
    {
        disable_fsu(fsu);
        return;
    }

    enable_fsu(fsu);
    fsu->           shadow = get_period(ch1);
    fsu->freq_sweep_thresh = (pace == 0) ? 8 : pace; // Hardware quirk
    
    if (step == 0)
        return;

    check_negate_latch_set(fsu);

    uint16_t period = freq_sweep_calc(*fsu, step);
    
    if (period > PERIOD_OVERFLOW)
    {
        disable_fsu(fsu);
        disable_channel(apu, ch1);
    }
}

// Channel 2 Writes

static void write_nr20(APU *apu, uint8_t value) // Channel 2 Empty
{
    Channel *ch2 = &apu->ch2;

    *ch2->nrx0 = value;
}
static void write_nr21(APU *apu, uint8_t value) // Channel 2 Length
{
    Channel *ch2 = &apu->ch2;

    *ch2->nrx1 = value;

    ch2->length_timer = init_length(ch2);
}
static void write_nr22(APU *apu, uint8_t value) // Channel 2 DAC & Volume
{
    Channel *ch2 = &apu->ch2;

    *ch2->nrx2 = value;

    ch2->dac_enabled = ((value & UPPER_5_MASK) != 0);

    if (!ch2->dac_enabled && ch2->enabled)
        disable_channel(apu, ch2);
}
static void write_nr23(APU *apu, uint8_t value) // Channel 2 Low-Byte of Period
{
    Channel *ch2 = &apu->ch2;

    *ch2->nrx3 = value;
}
static void write_nr24(APU *apu, uint8_t value) // Channel 2 Trigger, Length Enable, Upper-3 Period Bits
{
    Channel *ch2 = &apu->ch2;

    *ch2->nrx4 = value;

    check_length_trigger(apu, ch2);
    check_channel_trigger(apu, ch2);
}

// Channel 3 Writes

static void write_nr30(APU *apu, uint8_t value) // Channel 3 DAC Enable
{
    Channel *ch3 = &apu->ch3;

    *ch3->nrx0 = value;

    ch3->dac_enabled = ((value & BIT_7_MASK) != 0);

    if (!ch3->dac_enabled && ch3->enabled)
        disable_channel(apu, ch3);
}
static void write_nr31(APU *apu, uint8_t value) // Channel 3 Length
{
    Channel *ch3 = &apu->ch3;

    *ch3->nrx1 = value;

    ch3->length_timer = init_length(ch3);
}
static void write_nr32(APU *apu, uint8_t value) // Channel 3 Volume
{
    Channel *ch3 = &apu->ch3;

    *ch3->nrx2 = value;
}
static void write_nr33(APU *apu, uint8_t value) // Channel 3 Low-Byte of Period
{
    Channel *ch3 = &apu->ch3;

    *ch3->nrx3 = value;
}
static void write_nr34(APU *apu, uint8_t value) // Channel 3 Trigger, Length Enable, Upper-3 Period Bits
{
    Channel *ch3 = &apu->ch3;

    *ch3->nrx4 = value;

    check_length_trigger(apu, ch3);
    check_channel_trigger(apu, ch3);
}

// Channel 4 Writes

static void write_nr40(APU *apu, uint8_t value) // Channel 4 Empty
{
    Channel *ch4 = &apu->ch4;

    *ch4->nrx0 = value;
}
static void write_nr41(APU *apu, uint8_t value) // Channel 4 Length
{
    Channel *ch4 = &apu->ch4;

    *ch4->nrx1 = value;

    ch4->length_timer = init_length(ch4);
}
static void write_nr42(APU *apu, uint8_t value) // Channel 4 DAC & Volume
{
    Channel *ch4 = &apu->ch4;

    *ch4->nrx2 = value;

    ch4->dac_enabled = ((value & UPPER_5_MASK) != 0);

    if (!ch4->dac_enabled && ch4->enabled)
        disable_channel(apu, ch4);
}
static void write_nr43(APU *apu, uint8_t value) // Channel 4 Random Control
{
    static const uint8_t divisor_table[8] = { 8, 16, 32, 48, 64, 80, 96, 112 };
    
    Channel *ch4 = &apu->ch4;

    *ch4->nrx3 = value;

    uint8_t step = (value >> 4) & LOWER_4_MASK; // s: shift clock frequency (0–15)
    uint8_t  div = value & LOWER_3_MASK;        // r: divisor code (0–7)

    noise_period = divisor_table[div] << step; // Period = divisor × 2^shift
}

static void write_nr44(APU *apu, uint8_t value) // Channel 4 Trigger & Length Enable
{
    Channel *ch4 = &apu->ch4;

    *ch4->nrx4 = value;

    check_length_trigger(apu, ch4);
    check_channel_trigger(apu, ch4);

    ch4->lfsr = 0;
}

// Global Control writes

static void clear_channel(Channel *ch)
{
    *ch->nrx0 = 0;
    *ch->nrx1 = 0;
    *ch->nrx2 = 0;
    *ch->nrx3 = 0;
    *ch->nrx4 = 0;

    ch->volume_envelope_enabled = false;
    ch->   length_timer_enabled = false;
    ch->            dac_enabled = false;
}
static void power_apu_on(APU *apu)
{
    apu->     powered = true;
    apu->       frame =    0;
    apu->   ch1.timer =    0;
    apu->   ch2.timer =    0;
    apu->   ch3.timer =    0;
    apu->   ch4.timer =    0;
}
static void power_apu_off(APU *apu)
{
    clear_channel(&apu->ch1);
    disable_channel(apu, &apu->ch1);

    clear_channel(&apu->ch2);
    disable_channel(apu, &apu->ch2);

    clear_channel(&apu->ch3);
    disable_channel(apu, &apu->ch3);
    
    clear_channel(&apu->ch4);
    disable_channel(apu, &apu->ch4);

    *apu->nr50 = 0;
    *apu->nr51 = 0;
}

static void write_nr50(APU *apu, uint8_t value) // Master Volume
{
    *apu->nr50 = value;
}
static void write_nr51(APU *apu, uint8_t value) // Panning
{
    *apu->nr51 = value;
}
static void write_nr52(APU *apu, uint8_t value) // Powered & Status
{
    *apu->nr52 = value;
    
    bool prev_powered = apu->powered;
    apu->powered = ((value & BIT_7_MASK) != 0);
    
    if (apu->powered && !prev_powered)
        power_apu_on(apu);

    if (!apu->powered && prev_powered) // Powering Off 
        power_apu_off(apu);

    sync_nr52(apu);
}

void write_audio_register(APU *apu, uint16_t address, uint8_t value)
{
    if (!apu->powered && (address != NR52)) 
        return;

    switch(address)
    {
        case NR10: write_nr10(apu, value); break;
        case NR11: write_nr11(apu, value); break;
        case NR12: write_nr12(apu, value); break;
        case NR13: write_nr13(apu, value); break;
        case NR14: write_nr14(apu, value); break;

        case NR20: write_nr20(apu, value); break;
        case NR21: write_nr21(apu, value); break;
        case NR22: write_nr22(apu, value); break;
        case NR23: write_nr23(apu, value); break;
        case NR24: write_nr24(apu, value); break;
        
        case NR30: write_nr30(apu, value); break; 
        case NR31: write_nr31(apu, value); break;
        case NR32: write_nr32(apu, value); break;
        case NR33: write_nr33(apu, value); break;
        case NR34: write_nr34(apu, value); break;
       
        case NR40: write_nr40(apu, value); break;
        case NR41: write_nr41(apu, value); break;
        case NR42: write_nr42(apu, value); break;
        case NR43: write_nr43(apu, value); break;
        case NR44: write_nr44(apu, value); break;
        
        case NR50: write_nr50(apu, value); break;
        case NR51: write_nr51(apu, value); break;
        case NR52: write_nr52(apu, value); break;
    }
}

// Linking and Initialization

void link_apu(APU *apu, GbcEmu *emu)
{
    EmuMemory *mem = emu->mem;

    Channel *ch1 = &(apu->ch1); // Pulse
    ch1->nrx0 = &(mem->memory[NR10]);
    ch1->nrx1 = &(mem->memory[NR11]);
    ch1->nrx2 = &(mem->memory[NR12]);
    ch1->nrx3 = &(mem->memory[NR13]);
    ch1->nrx4 = &(mem->memory[NR14]);

    Channel *ch2 = &(apu->ch2); // Pulse
    ch2->nrx0 = &(mem->memory[NR20]);
    ch2->nrx1 = &(mem->memory[NR21]);
    ch2->nrx2 = &(mem->memory[NR22]);
    ch2->nrx3 = &(mem->memory[NR23]);
    ch2->nrx4 = &(mem->memory[NR24]);

    Channel *ch3 = &(apu->ch3); // Wave
    ch3->nrx0 = &(mem->memory[NR30]);
    ch3->nrx1 = &(mem->memory[NR31]);
    ch3->nrx2 = &(mem->memory[NR32]);
    ch3->nrx3 = &(mem->memory[NR33]);
    ch3->nrx4 = &(mem->memory[NR34]);

    Channel *ch4 = &(apu->ch4); // Noise
    ch4->nrx0 = &(mem->memory[NR40]);
    ch4->nrx1 = &(mem->memory[NR41]);
    ch4->nrx2 = &(mem->memory[NR42]);
    ch4->nrx3 = &(mem->memory[NR43]);
    ch4->nrx4 = &(mem->memory[NR44]);

    // Global Rgisters
    apu->nr50 = &(mem->memory[NR50]);
    apu->nr51 = &(mem->memory[NR51]);
    apu->nr52 = &(mem->memory[NR52]);

    // PCM
    apu->pcm12 = &(mem->memory[PCM12]);
    apu->pcm34 = &(mem->memory[PCM34]);

    // Wave Ram (16 Bytes)
    apu->wave_ram = mem->wave_ram;

    // Emulation References
    apu->joypad = &emu->joypad;
    apu->   mem = mem;
}

APU *init_apu()
{
    APU *apu = (APU*) malloc(sizeof(APU));
    memset(apu, 0, sizeof(APU));

    apu->ch1.name = PULSE_ONE;
    apu->ch2.name = PULSE_TWO;
    apu->ch3.name =      WAVE;
    apu->ch4.name =     NOISE;

    return apu;
}

void tidy_apu(APU **apu)
{
    free(*apu);
    *apu = NULL; 
}


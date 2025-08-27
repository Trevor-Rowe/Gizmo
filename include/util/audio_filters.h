#ifndef AUDIO_FILTERS_H
#define AUDIO_FILTERS_H

#include <stdint.h>
#include <limits.h> // INT16_MAX/MIN

typedef struct 
{
    float  prev_input;
    float prev_output;
    float       alpha;

} HighPassFilter;

typedef struct 
{
    float prev_output;
    float       alpha;

} LowPassFilter;


static inline int16_t hpf_process(HighPassFilter *hpf, int16_t input) 
{
    // y[n] = a*( y[n-1] + x[n] - x[n-1] )
    float x = (float)input;
    float y = hpf->alpha * (hpf->prev_output + x - hpf->prev_input);

    hpf->prev_input  = x;
    hpf->prev_output = y;

    if (y > (float)INT16_MAX) 
        y = (float)INT16_MAX;
    
    if (y < (float)INT16_MIN) 
        y = (float)INT16_MIN;

    return (int16_t)y;
}

static inline int16_t lpf_process(LowPassFilter *lpf, int16_t input) 
{
    // y[n] = a*x[n] + (1-a)*y[n-1]
    float y = lpf->alpha * (float)input + (1.0f - lpf->alpha) * lpf->prev_output;
    lpf->prev_output = y;

    if (y > (float)INT16_MAX) 
        y = (float)INT16_MAX;
    
    if (y < (float)INT16_MIN) 
        y = (float)INT16_MIN;
    
    return (int16_t)y;
}

#endif // AUDIO_FILTERS_H

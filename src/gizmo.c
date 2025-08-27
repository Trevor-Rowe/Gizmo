#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>

#include "core/apu.h"
#include "core/cart.h"
#include "core/ppu.h"
#include "core/cpu.h"
#include "core/timer.h"
#include "core/emulator.h"

#include "gizmo.h"

#include "util/ring_buffer.h"
#include "util/audio_filters.h"
#include "util/common.h"

// Video Constants

#define FRAME_PERIOD 16.74 // 1 / 60 seconds per frame 
#define LCD_BUFFER_SIZE  GBC_HEIGHT * GBC_WIDTH * sizeof(uint32_t)

// Audio Constants

#define HP_ALPHA    0.998f
#define LP_ALPHA    0.500f
#define SAMPLE_RATE  44100
#define CHANNELS         2
#define BUFFER_SIZE    128

// Dynamic Thresholding 

static const int          FP_SHIFT = 16;
static const int64_t        FP_ONE = (1LL << FP_SHIFT);
static const double       BASE_RAW = (((double) SYSTEM_CLOCK_FREQUENCY) / ((double) SAMPLE_RATE));          
static const int64_t    BASE_FIXED = (int64_t) (BASE_RAW * ((double) FP_ONE));
static const int64_t MAX_THRESHOLD = (int64_t) (BASE_FIXED * 1.01);
static const int64_t MIN_THRESHOLD = (int64_t) (BASE_FIXED * 0.99);

// SDL Components

static SDL_Window             *window;
static SDL_Renderer         *renderer;
static SDL_Texture       *framebuffer;
static SDL_AudioDeviceID audio_device;

// Thread Handling

static bool      frame_available;
static SDL_mutex          *mutex;
static SDL_cond     *frame_ready;

// Audio Buffer and Filters

static RingBuffer        ring_buffer = {0};
static HighPassFilter hpl = {0}, hpr = {0};
static LowPassFilter  lpl = {0}, lpr = {0};

static GbcEmu *current_emulator;

// Variable Control

static uint8_t    volume = 5;
static uint8_t win_scale = 5;

// SDL2 Handling

static void render_sdl_frame(GbcEmu *emu)
{
    static uint32_t double_buffer[LCD_BUFFER_SIZE] = {0};

    if (!emu->running)
        return;

    memcpy(double_buffer, render_frame(emu->ppu), LCD_BUFFER_SIZE);

    SDL_UpdateTexture(framebuffer, NULL, double_buffer, GBC_WIDTH * sizeof(uint32_t));
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, framebuffer, NULL, NULL);
    SDL_RenderPresent(renderer);
}

static int64_t dynamic_sample_threshold()
{
    const int64_t target = RING_BUFFER_CAPACITY / 2;

    int64_t delta = ring_buffer.size - target;
    int64_t   adj = (delta / 4) * (FP_ONE >> 8);
    int64_t threshold = BASE_FIXED + adj;

    if (threshold > MAX_THRESHOLD)
        threshold = MAX_THRESHOLD;

    if (threshold < MIN_THRESHOLD)
        threshold = MIN_THRESHOLD;

    return threshold;
}

static void audio_callback(void *userdata, Uint8 *stream, int len)
{
    static int16_t  left_sample = 0;
    static int16_t right_sample = 0;

    int16_t *buffer = (int16_t*) stream;
    int     samples = len / sizeof(int16_t);

    bool read_completed = false;

    for (int i = 0; i < samples; i += 2) 
    {
        read_completed = ring_buffer_read(&ring_buffer, &left_sample);

        if (read_completed) // Avoid signal attenuation 
        {
            hpf_process(&hpl, left_sample);
            left_sample >>= volume;
        }

        read_completed = ring_buffer_read(&ring_buffer, &right_sample);

        if (read_completed) // Avoid signal attenuation
        {
            hpf_process(&hpr, right_sample);
            right_sample >>= volume;
        }

        buffer[i + 0] = left_sample;
        buffer[i + 1] = right_sample;
    }
}

static void init_audio()
{
    SDL_AudioSpec 
    want = {0}, have = {0};

    want.freq     =    SAMPLE_RATE;
    want.format   =   AUDIO_S16SYS;
    want.channels =       CHANNELS;
    want.samples  =    BUFFER_SIZE;
    want.callback = audio_callback;

    audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);

    if (audio_device == 0)
    {
        perror("Failed to initialize audio!");
        exit(EXIT_FAILURE);
    }

    hpl.alpha = HP_ALPHA;
    hpr.alpha = HP_ALPHA;

    lpl.alpha = LP_ALPHA;
    lpr.alpha = LP_ALPHA;

    SDL_PauseAudioDevice(audio_device, false);
    reset_ring_buffer(&ring_buffer);
}

static bool init_peripherals()
{
    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0)
        return false;

    // Create SDL Window
    window = SDL_CreateWindow
    (
        "Gizmo!",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        GBC_WIDTH  * win_scale,
        GBC_HEIGHT * win_scale,
        SDL_WINDOW_SHOWN 
    );
    if(!window)
    {
        SDL_Quit();
        return false;
    }

    // Create SDL Renderer
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);

    if (!renderer)
    {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return false;
    }

    // Frame buffer Texture
    framebuffer = SDL_CreateTexture
    (
        renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        GBC_WIDTH,
        GBC_HEIGHT
    );

    if (!framebuffer)
    {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return false;
    }

    init_audio();

    return true;
}

static void tidy_peripherals()
{
    SDL_PauseAudioDevice(audio_device, true);
    SDL_CloseAudioDevice(audio_device);

    SDL_DestroyMutex(mutex);
    SDL_DestroyTexture(framebuffer);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);

    SDL_Quit();
}

// Emulation Drivers

static void audio_sample_pulse(GbcEmu *emu)
{
    static uint64_t counter = 0;
    static uint64_t  thresh = BASE_FIXED;

    counter += FP_ONE;
    if (counter < thresh) return;
    counter -= thresh;
    
    thresh = dynamic_sample_threshold();

    int16_t  left_sample = sample_left_channel(emu->apu);
    int16_t right_sample = sample_right_channel(emu->apu);

    if (emu->joypad.turbo_enabled)
        return;
        
    bool buffer_write_occurred = ring_buffer_write(&ring_buffer,  left_sample);

    if (buffer_write_occurred)
        buffer_write_occurred &= ring_buffer_write(&ring_buffer, right_sample);
}

static int emu_thread(void *data)
{
    GbcEmu *emu = (GbcEmu*) data;

    while(emu->running)
    {
        bool emu_frame_complete = system_clock_pulse(emu->timer);
        audio_sample_pulse(emu);

        if (emu_frame_complete) // Emulation Frame Complete? 
        {
            SDL_LockMutex(mutex);

            while(frame_available)
                SDL_CondWait(frame_ready, mutex);

            frame_available = true;
            SDL_CondSignal(frame_ready);
            SDL_UnlockMutex(mutex);
        }
    }
    
    return 0;
}

static void check_rtc_clock(GbcEmu *emu)
{
    static uint8_t frames = 0;

    frames++;
    if (frames < 60) return;
    frames = 0;

    rtc_tick_second(emu->cart);
}

static void emulate_frame(GbcEmu *emu)
{
    Uint64 start_time = SDL_GetPerformanceCounter(); // Start Timer
    
    check_rtc_clock(emu); // Real Time Clock

    handle_events(emu);   // Record Input

    SDL_LockMutex(mutex); // Frame Sync

    while (!frame_available) 
        SDL_CondWait(frame_ready, mutex);

    frame_available = false;
    SDL_CondSignal(frame_ready);
    SDL_UnlockMutex(mutex);

    render_sdl_frame(emu);

    Uint64 perf_freq = SDL_GetPerformanceFrequency(); // Stop Timer
    
    // Calculate and Implement Delay
    Uint64    elapsed = SDL_GetPerformanceCounter() - start_time;
    double elapsed_ms = ((double) elapsed / perf_freq) * 1e3;

    if (elapsed_ms < FRAME_PERIOD && !emu->joypad.turbo_enabled)
    {
        uint32_t delay = (Uint32)(FRAME_PERIOD - elapsed_ms);
        SDL_Delay(delay);        
    } 
}

void start_emulator(GbcEmu *emu)
{
    emu->running = true;
    start_cpu(emu->cpu);

    // Threading setup
    mutex           = SDL_CreateMutex();
    frame_ready     =  SDL_CreateCond();
    frame_available =             false;

    SDL_Thread *emulation_thread = SDL_CreateThread(emu_thread, "Emu Thread", emu);

    while(emu->running) // 1 Loop = 1 Frame
        emulate_frame(emu);

    SDL_WaitThread(emulation_thread, NULL);
}

// GUI Elements

static const char *file_name_from_path(const char *file_path)
{
    const char *slash_fwd = strrchr(file_path, '/');  // Mac Linux
    const char *slash_bwd = strrchr(file_path, '\\'); // Windows
    const char *sep = (slash_fwd > slash_bwd) ? slash_fwd : slash_bwd;

    return sep ? (sep + 1) : file_path; // Points to first character of file name
}

static void choose_file(GbcEmu *emu, bool swapping)
{
    const char *filter_patterns[] = { "*.gb", "*.gbc" };

    char *file_path = tinyfd_openFileDialog("Open ROM", "", 2, filter_patterns, "Game Boy ROMs", 0);

    if (!file_path)
        return;

    const char *file_name = file_name_from_path(file_path);

    if (file_path && swapping)
    {
        swap_cartridge(emu, file_path, file_name);
        start_emulator(emu);
        return;  
    }

    if (file_path && !swapping)
    {
        load_cartridge(emu, file_path, file_name);
        start_emulator(emu);
        return;
    }
}

static void ask_to_save(GbcEmu *emu)
{
    if (tinyfd_messageBox("Save Game?", "Do you want to save before exiting?", "yesno", "question", 1)) 
    {
        save_cartridge(emu->cart);
    }
}

// Joypad Control

static void handle_button_press(GbcEmu *emu, SDL_Event *event)
{
    if (event->key.repeat) 
        return;

    Joypad  *joypad = &emu->joypad; 
    SDL_Keycode key = event->key.keysym.sym;

    switch (key)
    {
        case SDLK_x:         joypad->            A = true; break;
        case SDLK_z:         joypad->            B = true; break;
        case SDLK_RETURN:    joypad->        START = true; break;
        case SDLK_BACKSPACE: joypad->       SELECT = true; break;
        case SDLK_UP:        joypad->           UP = true; break;
        case SDLK_DOWN:      joypad->         DOWN = true; break;
        case SDLK_RIGHT:     joypad->        RIGHT = true; break;
        case SDLK_LEFT:      joypad->         LEFT = true; break;
        case SDLK_SPACE:     joypad->turbo_enabled = true; break;

        case SDLK_q: // Volume Down
            volume += 1;
            if (volume > 10)
                volume = 10;
            printf("[Volume] = %d\n", volume);
            break;

        case SDLK_w: // Volume Up
            if (volume != 0)
                volume -= 1;
            printf("[Volume] = %d\n", volume);
            break;

        case SDLK_e: // Screen Size Down
            win_scale--;
            if (win_scale == 0)
                win_scale = 1;
            
            SDL_SetWindowSize(window, GBC_WIDTH * win_scale, GBC_HEIGHT * win_scale);
            break;

        case SDLK_r: // Screen Size Up
            win_scale++;
            if (win_scale > 10)
                win_scale = 10;

            SDL_SetWindowSize(window, GBC_WIDTH * win_scale, GBC_HEIGHT * win_scale);
            break;

        case SDLK_o: 
            choose_file(emu, true); 
            break;

        case SDLK_s:
            save_cartridge(emu->cart);
            printf("Saving game!\n");
            break;

        case SDLK_t:
            rtc_tick_hour(emu->cart);
            printf("Advancing clock by one hour...");
            break;
    }
} 

static void handle_button_release(GbcEmu *emu, SDL_Event *event)
{
    Joypad  *joypad = &emu->joypad; 
    SDL_Keycode key = event->key.keysym.sym;

    switch (key)
    {
        // case SDLK_r:         reset_emulator();              break;
        case SDLK_x:         joypad->            A = false; break;
        case SDLK_z:         joypad->            B = false; break;
        case SDLK_RETURN:    joypad->        START = false; break;
        case SDLK_BACKSPACE: joypad->       SELECT = false; break;
        case SDLK_UP:        joypad->           UP = false; break;
        case SDLK_DOWN:      joypad->         DOWN = false; break;
        case SDLK_RIGHT:     joypad->        RIGHT = false; break;
        case SDLK_LEFT:      joypad->         LEFT = false; break;
        case SDLK_SPACE:     joypad->turbo_enabled = false; break;
    }
}

bool handle_events(GbcEmu *emu)
{
    SDL_Event event;
    bool jirn = false; // Joypad Interrupt Request Needed

    while (SDL_PollEvent(&event)) // Keep going until event queue is empty.
    {
        switch(event.type)
        {
            case SDL_QUIT:
                emu->running = false;
                ask_to_save(emu);
                tidy_emulator(&emu);
                tidy_peripherals();
                return false;
            
            case SDL_KEYDOWN:
                handle_button_press(emu, &event);
                jirn = true;
                break;
            
            case SDL_KEYUP:
                handle_button_release(emu, &event);
                jirn = true;
                break;
        }
    }
    
    if (jirn) 
        request_interrupt(emu->cpu, JOYPAD_INTERRUPT_CODE);
        
    return true;
}

// Entry Point

int main(int argc, char *argv[]) 
{ 
    GbcEmu *emu = init_emulator();
    init_peripherals();
    current_emulator = emu;
    memset(&emu->joypad, 0, sizeof(Joypad));
    choose_file(emu, false);

    return 0;
}


#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "psx_spu.h"
#include "xport.h"

enum
{
    XPORT_AUDIO_BUFFER_COUNT = 2,
    XPORT_AUDIO_BUFFER_FRAMES = (SPU_SAMPLE_RATE / 30) * 2
};

static HWND platform_window;
static sint32 platform_window_width = WND_WIDTH;
static sint32 platform_window_height = WND_HEIGHT;
static sint32 platform_headless;
static sint32 platform_quit;
static uint32 platform_pad_buttons;
static sint32 platform_pad_override;
static uint32 platform_override_buttons;
static LARGE_INTEGER platform_timer_frequency;
static LARGE_INTEGER platform_timer_start;
static uint64 platform_frame_deadline;
static sint32 platform_timer_initialized;

static void xport_timer_init(void)
{
    if (platform_timer_initialized)
        return;
    QueryPerformanceFrequency(&platform_timer_frequency);
    QueryPerformanceCounter(&platform_timer_start);
    timeBeginPeriod(1);
    platform_timer_initialized = 1;
}

uint64 xport_timer_get(void)
{
    LARGE_INTEGER now;
    uint64 ticks;
    uint64 frequency;
    xport_timer_init();
    QueryPerformanceCounter(&now);
    ticks = (uint64)(now.QuadPart - platform_timer_start.QuadPart);
    frequency = (uint64)platform_timer_frequency.QuadPart;
    return ticks / frequency * 1000000u + ticks % frequency * 1000000u / frequency;
}

void xport_timer_wait_frame(uint32 refresh_rate)
{
    uint64 now;
    uint64 period;
    uint64 remaining;
    xport_timer_init();
    if (!refresh_rate)
        refresh_rate = FIELD_RATE;
    now = xport_timer_get();
    period = 1000000u / refresh_rate;
    if (!platform_frame_deadline || now > platform_frame_deadline + period * 4)
        platform_frame_deadline = now + period;
    while (now < platform_frame_deadline)
    {
        remaining = platform_frame_deadline - now;
        Sleep(remaining > 2000 ? (DWORD)(remaining / 1000 - 1) : 0);
        now = xport_timer_get();
    }
    platform_frame_deadline += period;
}

static uint32 key_to_pad(WPARAM key)
{
    switch (key)
    {
        case VK_UP:
        case 'W':
            return PADLup;
        case VK_DOWN:
        case 'S':
            return PADLdown;
        case VK_LEFT:
        case 'A':
            return PADLleft;
        case VK_RIGHT:
        case 'D':
            return PADLright;
        case 'I':
            return PADRup;
        case 'K':
            return PADRdown;
        case 'J':
            return PADRleft;
        case 'L':
            return PADRright;
        case 'Q':
            return PADL1;
        case 'E':
            return PADR1;
        case '1':
            return PADL2;
        case '3':
            return PADR2;
        case VK_RETURN:
            return PADstart;
        case VK_BACK:
            return PADselect;
        default:
            return 0;
    }
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    uint32 button;
    switch (message)
    {
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            platform_quit = 1;
            platform_window = NULL;
            PostQuitMessage(0);
            return 0;
        case WM_SIZE:
            platform_window_width = LOWORD(lparam);
            platform_window_height = HIWORD(lparam);
            return 0;
        case WM_KILLFOCUS:
            platform_pad_buttons = 0;
            return 0;
        case WM_KEYDOWN:
        case WM_KEYUP:
            button = key_to_pad(wparam);
            if (message == WM_KEYDOWN)
                platform_pad_buttons |= button;
            else
                platform_pad_buttons &= ~button;
            return 0;
        default:
            return DefWindowProc(window, message, wparam, lparam);
    }
}

sint32 xport_input_init(void)
{
    platform_pad_buttons = 0;
    platform_pad_override = 0;
    platform_override_buttons = 0;
    return 1;
}

sint32 xport_poll(void)
{
    MSG message;
    while (PeekMessage(&message, NULL, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessage(&message);
    }
    return !platform_quit;
}

uint32 xport_input_read(sint32 controller)
{
    if (controller != 0)
        return 0;
    return platform_pad_override ? platform_override_buttons : platform_pad_buttons;
}

void xport_input_override(sint32 enabled, uint32 buttons)
{
    platform_pad_override = enabled != 0;
    platform_override_buttons = buttons;
}

sint32 xport_window_init(void)
{
    WNDCLASSEX window_class;
    RECT rectangle;
    HINSTANCE instance;
    sint32 height;
    sint32 width;
    sint32 x;
    sint32 y;
    if (platform_window != NULL || platform_headless)
        return 1;
    instance = GetModuleHandle(NULL);
    memset(&window_class, 0, sizeof(window_class));
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursor(NULL, IDC_ARROW);
    window_class.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    window_class.lpszClassName = "XportWindow";
    window_class.hIcon = LoadIcon(instance, IDI_APPLICATION);
    window_class.hIconSm = window_class.hIcon;
    if (!RegisterClassEx(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return 0;
    rectangle.left = 0;
    rectangle.top = 0;
    rectangle.right = platform_window_width;
    rectangle.bottom = platform_window_height;
    AdjustWindowRect(&rectangle, WS_OVERLAPPEDWINDOW, 0);
    width = rectangle.right - rectangle.left;
    height = rectangle.bottom - rectangle.top;
    x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
    platform_window = CreateWindow(window_class.lpszClassName, WND_TITLE, WS_OVERLAPPEDWINDOW, x, y, width, height, NULL, NULL, instance, NULL);
    if (platform_window == NULL)
        return 0;
    ShowWindow(platform_window, SW_SHOW);
    UpdateWindow(platform_window);
    return 1;
}

sint32 xport_present(const uint32 *pixels, sint32 bitmap_width, sint32 bitmap_height, sint32 source_x, sint32 source_y, sint32 source_width, sint32 source_height, const char *title)
{
    RECT client;
    HDC device;
    BITMAPINFO bitmap;
    sint32 client_height;
    sint32 client_width;
    sint32 result;
    xport_poll();
    if (platform_headless)
        return 1;
    if (pixels == NULL || bitmap_width <= 0 || bitmap_height <= 0 || source_x < 0 || source_y < 0 || source_width <= 0 || source_height <= 0 || source_x + source_width > bitmap_width || source_y + source_height > bitmap_height || platform_window == NULL || platform_quit)
        return 0;
    if (!GetClientRect(platform_window, &client))
        return 0;
    client_width = client.right - client.left;
    client_height = client.bottom - client.top;
    if (client_width <= 0 || client_height <= 0)
        return 0;
    memset(&bitmap, 0, sizeof(bitmap));
    bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth = bitmap_width;
    bitmap.bmiHeader.biHeight = -bitmap_height;
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    bitmap.bmiHeader.biCompression = BI_RGB;
    device = GetDC(platform_window);
    if (device == NULL)
        return 0;
    SetStretchBltMode(device, COLORONCOLOR);
    result = StretchDIBits(device, 0, 0, client_width, client_height, source_x, source_y, source_width, source_height, pixels, &bitmap, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(platform_window, device);
    if (title != NULL)
        SetWindowText(platform_window, title);
    return result != 0 && result != GDI_ERROR;
}

sint32 xport_is_headless(void)
{
    return platform_headless;
}

void xport_set_headless(sint32 headless)
{
    platform_headless = headless != 0;
}

sint32 xport_isquit(void)
{
    return platform_quit;
}

/* Types. */
typedef struct WAVEOUT_state
{
    HWAVEOUT device;
    WAVEHDR headers[XPORT_AUDIO_BUFFER_COUNT];
    sint16 samples[XPORT_AUDIO_BUFFER_COUNT][XPORT_AUDIO_BUFFER_FRAMES * 2];
    HANDLE semaphore;
    HANDLE thread;
    volatile LONG running;
    uint8 prepared[XPORT_AUDIO_BUFFER_COUNT];
} WAVEOUT_state;

/* Variables. */
static WAVEOUT_state output;
static sint32 null_active;
static uint64 null_frames;
static CRITICAL_SECTION audio_lock;
static sint32 audio_lock_initialized;

volatile uint32 g_xport_audio_submitted_buffers;

volatile uint32 g_xport_audio_nonzero_buffers;

volatile uint32 g_xport_audio_peak;

volatile uint32 g_xport_audio_backend_active;

/* Host output only: keep SPU processing and evidence counters active while muted */
volatile uint32 g_xport_audio_output_muted = 0;

/* Functions. */
volatile uint32 g_xport_audio_callback_overruns;

static void CALLBACK wave_callback(HWAVEOUT device, UINT message, DWORD_PTR instance, DWORD_PTR parameter1, DWORD_PTR parameter2)
{
    WAVEOUT_state *state = (WAVEOUT_state *)instance;
    if (message == WOM_DONE && state != 0 && state->semaphore != 0 && InterlockedCompareExchange(&state->running, 1, 1) != 0)
        if (!ReleaseSemaphore(state->semaphore, 1, 0))
            InterlockedIncrement((volatile LONG *)&g_xport_audio_callback_overruns);
}

static DWORD WINAPI wave_thread(void *argument)
{
    WAVEOUT_state *state = (WAVEOUT_state *)argument;
    while (InterlockedCompareExchange(&state->running, 1, 1) != 0)
    {
        sint32 index;
        WaitForSingleObject(state->semaphore, INFINITE);
        if (InterlockedCompareExchange(&state->running, 1, 1) == 0)
            break;
        for (index = 0; index < XPORT_AUDIO_BUFFER_COUNT; ++index)
        {
            WAVEHDR *header = state->headers + index;
            if (!(header->dwFlags & WHDR_DONE))
                continue;
            if (state->prepared[index])
            {
                if (waveOutUnprepareHeader(state->device, header, sizeof(*header)) != MMSYSERR_NOERROR)
                    continue;
                state->prepared[index] = 0;
            }
            spu_render(state->samples[index], XPORT_AUDIO_BUFFER_FRAMES);
            {
                sint32 sample_index;
                uint32 peak = 0;
                for (sample_index = 0; sample_index < XPORT_AUDIO_BUFFER_FRAMES * 2; ++sample_index)
                {
                    sint32 value = state->samples[index][sample_index];
                    uint32 magnitude = (uint32)(value < 0 ? -value : value);
                    if (magnitude > peak)
                        peak = magnitude;
                }
                if (peak != 0)
                    ++g_xport_audio_nonzero_buffers;
                if (peak > g_xport_audio_peak)
                    g_xport_audio_peak = peak;
            }
            if (g_xport_audio_output_muted)
                memset(state->samples[index], 0, sizeof(state->samples[index]));
            header->dwFlags = 0;
            header->dwLoops = 0;
            if (waveOutPrepareHeader(state->device, header, sizeof(*header)) != MMSYSERR_NOERROR)
                continue;
            state->prepared[index] = 1;
            if (waveOutWrite(state->device, header, sizeof(*header)) != MMSYSERR_NOERROR)
            {
                waveOutUnprepareHeader(state->device, header, sizeof(*header));
                state->prepared[index] = 0;
            }
            else
                ++g_xport_audio_submitted_buffers;
        }
    }
    return 0;
}

void xport_audio_lock(void)
{
    if (audio_lock_initialized)
        EnterCriticalSection(&audio_lock);
}

void xport_audio_unlock(void)
{
    if (audio_lock_initialized)
        LeaveCriticalSection(&audio_lock);
}

void xport_audio_shutdown(void)
{
    sint32 index;
    HANDLE thread;
    if (null_active)
    {
        printf("headless_audio backend=null frames=%llu submitted=0 nonzero=%u peak=%u\n", (unsigned long long)null_frames, (unsigned)g_xport_audio_nonzero_buffers, (unsigned)g_xport_audio_peak);
        null_active = 0;
        InterlockedExchange(&output.running, 0);
        return;
    }
    if (output.device == 0 && output.semaphore == 0)
        return;
    InterlockedExchange(&output.running, 0);
    g_xport_audio_backend_active = 0;
    if (output.device != 0)
        waveOutReset(output.device);
    if (output.semaphore != 0)
        ReleaseSemaphore(output.semaphore, 1, 0);
    thread = output.thread;
    if (thread != 0)
    {
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
        output.thread = 0;
    }
    if (output.device != 0)
    {
        for (index = 0; index < XPORT_AUDIO_BUFFER_COUNT; ++index)
        {
            if (!output.prepared[index])
                continue;
            waveOutUnprepareHeader(output.device, output.headers + index, sizeof(WAVEHDR));
            output.prepared[index] = 0;
        }
        waveOutClose(output.device);
        output.device = 0;
    }
    if (output.semaphore != 0)
    {
        CloseHandle(output.semaphore);
        output.semaphore = 0;
    }
}

sint32 xport_audio_init(void)
{
    WAVEFORMATEX format;
    MMRESULT result;
    sint32 index;
    const char *audible = getenv("XPORT_AUDIO_OUTPUT");
    const char *backend = getenv("XPORT_AUDIO_BACKEND");
    if (InterlockedCompareExchange(&output.running, 1, 1) != 0)
        return 1;
    g_xport_audio_output_muted = audible != 0 && strcmp(audible, "0") == 0;
    memset(&output, 0, sizeof(output));
    g_xport_audio_submitted_buffers = 0;
    g_xport_audio_nonzero_buffers = 0;
    g_xport_audio_peak = 0;
    g_xport_audio_callback_overruns = 0;
    if (xport_is_headless() && g_xport_audio_output_muted && (backend == NULL || strcmp(backend, "waveout") != 0))
    {
        null_active = 1;
        null_frames = 0;
        g_xport_audio_backend_active = 0;
        InterlockedExchange(&output.running, 1);
        printf("headless_audio backend=null\n");
        return 1;
    }
    memset(&format, 0, sizeof(format));
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = SPU_SAMPLE_RATE;
    format.wBitsPerSample = 16;
    format.nBlockAlign = (WORD)(format.nChannels * sizeof(sint16));
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    format.cbSize = 0;
    output.semaphore = CreateSemaphore(0, 0, XPORT_AUDIO_BUFFER_COUNT, 0);
    if (output.semaphore == 0)
        return 0;
    result = waveOutOpen(&output.device, WAVE_MAPPER, &format, (DWORD_PTR)wave_callback, (DWORD_PTR)&output, CALLBACK_FUNCTION);
    if (result != MMSYSERR_NOERROR)
    {
        CloseHandle(output.semaphore);
        memset(&output, 0, sizeof(output));
        return 0;
    }
    InterlockedExchange(&output.running, 1);
    g_xport_audio_backend_active = 1;
    output.thread = CreateThread(0, 0, wave_thread, &output, 0, 0);
    if (output.thread == 0)
    {
        xport_audio_shutdown();
        return 0;
    }
    for (index = 0; index < XPORT_AUDIO_BUFFER_COUNT; ++index)
    {
        WAVEHDR *header = output.headers + index;
        memset(header, 0, sizeof(*header));
        memset(output.samples[index], 0, sizeof(output.samples[index]));
        header->lpData = (LPSTR)output.samples[index];
        header->dwBufferLength = sizeof(output.samples[index]);
        if (waveOutPrepareHeader(output.device, header, sizeof(*header)) != MMSYSERR_NOERROR)
            break;
        output.prepared[index] = 1;
        if (waveOutWrite(output.device, header, sizeof(*header)) != MMSYSERR_NOERROR)
            break;
    }
    if (index != XPORT_AUDIO_BUFFER_COUNT)
    {
        xport_audio_shutdown();
        return 0;
    }
    return 1;
}

sint32 xport_audio_is_running(void)
{
    return InterlockedCompareExchange(&output.running, 1, 1) != 0;
}

/* Advance the null sink on guest VBlank time without a host worker */
void xport_audio_vblank(uint32 before, uint32 rate)
{
    sint16 samples[2048];
    uint32 remaining, count;
    if (!null_active || !rate)
        return;
    remaining = (uint32)(((uint64)(before + 1u) * SPU_SAMPLE_RATE) / rate - ((uint64)before * SPU_SAMPLE_RATE) / rate);
    while (remaining)
    {
        uint32 peak = 0;
        uint32 sample_index;
        count = remaining > 1024 ? 1024 : remaining;
        spu_render(samples, count);
        for (sample_index = 0; sample_index < count * 2; ++sample_index)
        {
            sint32 value = samples[sample_index];
            uint32 magnitude = (uint32)(value < 0 ? -value : value);
            if (magnitude > peak)
                peak = magnitude;
        }
        if (peak != 0)
            ++g_xport_audio_nonzero_buffers;
        if (peak > g_xport_audio_peak)
            g_xport_audio_peak = peak;
        null_frames += count;
        remaining -= count;
    }
}

sint32 xport_file_read(const char *path, void *data, size_t capacity, size_t *size)
{
    FILE *file;
    size_t read_size;
    if (size != NULL)
        *size = 0;
    if (path == NULL || data == NULL)
        return 0;
    file = fopen(path, "rb");
    if (file == NULL)
        return 0;
    read_size = fread(data, 1, capacity, file);
    if (size != NULL)
        *size = read_size;
    if (ferror(file))
    {
        fclose(file);
        return 0;
    }
    fclose(file);
    return 1;
}

sint32 xport_memory_readable(const void *address, size_t size)
{
    MEMORY_BASIC_INFORMATION information;
    uintptr_t begin;
    uintptr_t end;
    uintptr_t region_end;
    if (address == NULL || size == 0 || VirtualQuery(address, &information, sizeof(information)) == 0)
        return 0;
    begin = (uintptr_t)address;
    end = begin + size;
    region_end = (uintptr_t)information.BaseAddress + information.RegionSize;
    return end >= begin && end <= region_end && information.State == MEM_COMMIT && (information.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
}

void xport_message_error(const char *title, const char *message)
{
    MessageBoxA(platform_window, message, title, MB_OK | MB_ICONERROR);
}

void xport_shutdown(void)
{
    xport_audio_shutdown();
    if (platform_window != NULL)
    {
        DestroyWindow(platform_window);
        platform_window = NULL;
    }
    if (platform_timer_initialized)
    {
        timeEndPeriod(1);
        platform_timer_initialized = 0;
    }
    if (audio_lock_initialized)
    {
        DeleteCriticalSection(&audio_lock);
        audio_lock_initialized = 0;
    }
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous_instance, LPSTR command_line, int show_command)
{
    int result;
    InitializeCriticalSection(&audio_lock);
    audio_lock_initialized = 1;
    xport_timer_init();
    xport_input_init();
    result = xport_main(__argc, __argv);
    xport_shutdown();
    return result;
}

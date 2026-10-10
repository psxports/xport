#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
// This backend uses the Windows critical-section API
#define XPORT_NO_PSYQ_CRITICAL_SECTION
#include "psx_spu.h"
#include "xport.h"
#include "xport_trace.h"
void psx_root_counter_poll(void);
#if defined(LOCKSTEP_DEBUG)
#endif

enum
{
    XPORT_AUDIO_BUFFER_COUNT = 2,
    XPORT_AUDIO_BUFFER_FRAMES = (SPU_SAMPLE_RATE / 30) * 2
};

typedef struct XINPUT_GAMEPAD
{
    WORD wButtons;
    BYTE bLeftTrigger;
    BYTE bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
} XINPUT_GAMEPAD;

typedef struct XINPUT_STATE
{
    DWORD dwPacketNumber;
    XINPUT_GAMEPAD Gamepad;
} XINPUT_STATE;

typedef DWORD(WINAPI *XInputGetStateProc)(DWORD dwUserIndex, XINPUT_STATE *pState);

#define XUSER_MAX_COUNT 4
#define XINPUT_GAMEPAD_DPAD_UP 0x0001
#define XINPUT_GAMEPAD_DPAD_DOWN 0x0002
#define XINPUT_GAMEPAD_DPAD_LEFT 0x0004
#define XINPUT_GAMEPAD_DPAD_RIGHT 0x0008
#define XINPUT_GAMEPAD_START 0x0010
#define XINPUT_GAMEPAD_BACK 0x0020
#define XINPUT_GAMEPAD_LEFT_THUMB 0x0040
#define XINPUT_GAMEPAD_RIGHT_THUMB 0x0080
#define XINPUT_GAMEPAD_LEFT_SHOULDER 0x0100
#define XINPUT_GAMEPAD_RIGHT_SHOULDER 0x0200
#define XINPUT_GAMEPAD_A 0x1000
#define XINPUT_GAMEPAD_B 0x2000
#define XINPUT_GAMEPAD_X 0x4000
#define XINPUT_GAMEPAD_Y 0x8000
#define XINPUT_GAMEPAD_TRIGGER_THRESHOLD 30

static HWND platform_window;
static sint32 platform_window_width = WND_WIDTH;
static sint32 platform_window_height = WND_HEIGHT;
static sint32 platform_headless;
static sint32 platform_quit;
static jmp_buf platform_game_exit;
static sint32 platform_game_exit_active;
static uint32 platform_pad_buttons;
static sint32 platform_pad_override;
static uint32 platform_override_buttons;
static LARGE_INTEGER platform_timer_frequency;
static LARGE_INTEGER platform_timer_start;
static uint64 platform_frame_deadline;
static sint32 platform_timer_initialized;
static RUN_MODE platform_run_mode;
static sint32 platform_exit_code;
static sint32 platform_argc;
static char *platform_argv[64];
static HMODULE platform_xinput_module;
static XInputGetStateProc _XInputGetState;

static void xport_frame_profile_record(uint64 begin, uint64 end);
static void xport_frame_profile_shutdown(void);
static void xport_frame_profile_wait(uint64 begin, uint64 end);

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
    uint64 wait_begin;
    xport_timer_init();
    if (!refresh_rate)
        refresh_rate = FIELD_RATE;
    now = xport_timer_get();
    period = 1000000u / refresh_rate;
    if (!platform_frame_deadline || now > platform_frame_deadline + period * 4)
        platform_frame_deadline = now + period;
    wait_begin = now;
    while (now < platform_frame_deadline)
    {
        remaining = platform_frame_deadline - now;
        Sleep(remaining > 2000 ? (DWORD)(remaining / 1000 - 1) : 0);
        now = xport_timer_get();
    }
    xport_frame_profile_wait(wait_begin, now);
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

static sint32 xinput_load(const char *name)
{
    HMODULE module = LoadLibraryA(name);
    XInputGetStateProc get_state;
    if (module == NULL)
        return 0;
    get_state = (XInputGetStateProc)GetProcAddress(module, "XInputGetState");
    if (get_state == NULL)
    {
        FreeLibrary(module);
        return 0;
    }
    platform_xinput_module = module;
    _XInputGetState = get_state;
    return 1;
}

static uint32 xinput_to_pad(const XINPUT_GAMEPAD *gamepad)
{
    uint32 buttons = 0;
    if (gamepad->wButtons & XINPUT_GAMEPAD_DPAD_UP)
        buttons |= PADLup;
    if (gamepad->wButtons & XINPUT_GAMEPAD_DPAD_DOWN)
        buttons |= PADLdown;
    if (gamepad->wButtons & XINPUT_GAMEPAD_DPAD_LEFT)
        buttons |= PADLleft;
    if (gamepad->wButtons & XINPUT_GAMEPAD_DPAD_RIGHT)
        buttons |= PADLright;
    if (gamepad->wButtons & XINPUT_GAMEPAD_START)
        buttons |= PADstart;
    if (gamepad->wButtons & XINPUT_GAMEPAD_BACK)
        buttons |= PADselect;
    if (gamepad->wButtons & XINPUT_GAMEPAD_LEFT_THUMB)
        buttons |= PADi;
    if (gamepad->wButtons & XINPUT_GAMEPAD_RIGHT_THUMB)
        buttons |= PADj;
    if (gamepad->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)
        buttons |= PADL1;
    if (gamepad->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER)
        buttons |= PADR1;
    if (gamepad->wButtons & XINPUT_GAMEPAD_A)
        buttons |= PADRdown;
    if (gamepad->wButtons & XINPUT_GAMEPAD_B)
        buttons |= PADRright;
    if (gamepad->wButtons & XINPUT_GAMEPAD_X)
        buttons |= PADRleft;
    if (gamepad->wButtons & XINPUT_GAMEPAD_Y)
        buttons |= PADRup;
    if (gamepad->bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
        buttons |= PADL2;
    if (gamepad->bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
        buttons |= PADR2;
    return buttons;
}

static uint32 xinput_read(void)
{
    DWORD controller;
    XINPUT_STATE state;
    if (_XInputGetState == NULL)
        return 0;
    for (controller = 0; controller < XUSER_MAX_COUNT; ++controller)
    {
        memset(&state, 0, sizeof(state));
        if (_XInputGetState(controller, &state) == ERROR_SUCCESS)
        {
            /* TODO: Publish left and right sticks when live PsyQ analog packets are supported */
            return xinput_to_pad(&state.Gamepad);
        }
    }
    return 0;
}

static void switch_window_mode(void)
{
    static WINDOWPLACEMENT windowed_placement;
    DWORD style;
    if (platform_window == NULL)
        return;
    style = (DWORD)GetWindowLongA(platform_window, GWL_STYLE);
    if (style & WS_OVERLAPPEDWINDOW)
    {
        MONITORINFO monitor;
        windowed_placement.length = sizeof(windowed_placement);
        monitor.cbSize = sizeof(monitor);
        if (GetWindowPlacement(platform_window, &windowed_placement) && GetMonitorInfoA(MonitorFromWindow(platform_window, MONITOR_DEFAULTTOPRIMARY), &monitor))
        {
            const RECT *rectangle = &monitor.rcMonitor;
            SetWindowLongA(platform_window, GWL_STYLE, (LONG)(style & ~WS_OVERLAPPEDWINDOW));
            MoveWindow(platform_window, rectangle->left, rectangle->top, rectangle->right - rectangle->left, rectangle->bottom - rectangle->top, FALSE);
        }
    }
    else if (windowed_placement.length == sizeof(windowed_placement))
    {
        SetWindowLongA(platform_window, GWL_STYLE, (LONG)(style | WS_OVERLAPPEDWINDOW));
        SetWindowPlacement(platform_window, &windowed_placement);
        SetWindowPos(platform_window, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
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
        case WM_SYSKEYDOWN:
            if (wparam == VK_RETURN && (lparam & (1L << 29)))
            {
                if (!(lparam & (1L << 30)))
                    switch_window_mode();
                return 0;
            }
            return DefWindowProc(window, message, wparam, lparam);
        case WM_SYSKEYUP:
            if (wparam == VK_RETURN)
                return 0;
            return DefWindowProc(window, message, wparam, lparam);
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
    if (_XInputGetState == NULL)
        if (!xinput_load("xinput1_3.dll"))
            xinput_load("xinput9_1_0.dll");
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
    /* Return to native cleanup after the window message has unwound */
    if (platform_quit && platform_game_exit_active)
        longjmp(platform_game_exit, 1);
    psx_root_counter_poll();
    return !platform_quit;
}

uint32 xport_input_read(sint32 controller)
{
    if (controller != 0)
        return 0;
    return platform_pad_override ? platform_override_buttons : platform_pad_buttons | xinput_read();
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

/* Save bounded producer pixels without desktop readback */
static void xport_capture_present(const uint32 *pixels, sint32 bw, sint32 bh, sint32 sx, sint32 sy, sint32 sw, sint32 sh, sint32 cw, sint32 ch, sint32 result)
{
    static uint32 attempted, saved, seen, configured, start, interval = 1u;
    const char *directory = getenv("XPORT_FRAME_CAPTURE_DIR");
    const char *setting;
    char *end;
    unsigned long parsed;
    uint32 present_index;
    char path[1024];
    uint8 row[4096 * 3];
    sint32 x, y, nonblack = 0, complete = 1;
    FILE *file;
    if (directory == NULL || directory[0] == 0 || saved >= 4u || attempted >= 256u)
        return;
    if (configured == 0u)
    {
        configured = 1u;
        setting = getenv("XPORT_FRAME_CAPTURE_START");
        if (setting != NULL && setting[0] >= '0' && setting[0] <= '9')
        {
            parsed = strtoul(setting, &end, 10);
            if (*end == 0 && parsed <= 1000000ul)
                start = (uint32)parsed;
        }
        setting = getenv("XPORT_FRAME_CAPTURE_INTERVAL");
        if (setting != NULL && setting[0] >= '0' && setting[0] <= '9')
        {
            parsed = strtoul(setting, &end, 10);
            if (*end == 0 && parsed >= 1ul && parsed <= 1000000ul)
                interval = (uint32)parsed;
        }
    }
    if (seen >= 1000000u)
        return;
    present_index = seen++;
    if (present_index < start || (present_index - start) % interval != 0u)
        return;
    ++attempted;
    if (bw > 4096 || bh > 4096 || sw > 4096 || sh > 4096)
        return;
    for (y = 0; y < sh && !nonblack; ++y)
        for (x = 0; x < sw; ++x)
            if (pixels[(size_t)(sy + y) * bw + sx + x] & 0xFFFFFFu)
            {
                nonblack = 1;
                break;
            }
    if (!nonblack)
        return;
    if (snprintf(path, sizeof(path), "%s/present-%03u.ppm", directory, saved) < 0 || strlen(path) >= sizeof(path) - 1)
        return;
    file = fopen(path, "wb");
    if (file == NULL)
        return;
    if (fprintf(file, "P6\n%d %d\n255\n", sw, sh) < 0)
        complete = 0;
    for (y = 0; y < sh && complete; ++y)
    {
        for (x = 0; x < sw; ++x)
        {
            uint32 pixel = pixels[(size_t)(sy + y) * bw + sx + x];
            row[3 * x] = (uint8)(pixel >> 16);
            row[3 * x + 1] = (uint8)(pixel >> 8);
            row[3 * x + 2] = (uint8)pixel;
        }
        if (fwrite(row, 3u, (size_t)sw, file) != (size_t)sw)
            complete = 0;
    }
    if (fclose(file) != 0)
        complete = 0;
    if (!complete)
        return;
    if (snprintf(path, sizeof(path), "%s/present-%03u.json", directory, saved) < 0 || strlen(path) >= sizeof(path) - 1)
        return;
    file = fopen(path, "wb");
    if (file == NULL)
        return;
    complete = fprintf(file,
                       "{\"present_index\":%u,\"sample_start\":%u,\"sample_interval\":%u,\"bitmap_width\":%d,\"bitmap_height\":%d,\"source_crop\":[%d,%d,%d,%d],"
                       "\"client_width\":%d,\"client_height\":%d,\"stretch_dibits_result\":%d,"
                       "\"presentation_api_success\":%s,\"visible_desktop_claim\":false,"
                       "\"producer_format\":\"BI_RGB32 BGRX little endian\",\"nonblack\":true}\n",
                       present_index, start, interval, bw, bh, sx, sy, sw, sh, cw, ch, result, result != 0 && result != GDI_ERROR ? "true" : "false") >= 0;
    if (fclose(file) != 0)
        complete = 0;
    if (complete)
        ++saved;
}

sint32 xport_present(const uint32 *pixels, sint32 bitmap_width, sint32 bitmap_height, sint32 source_x, sint32 source_y, sint32 source_width, sint32 source_height, const char *title)
{
    RECT client;
    HDC device;
    BITMAPINFO bitmap;
    sint32 client_height;
    sint32 client_width;
    sint32 result;
    uint64 begin;
    uint64 end;
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
    begin = xport_timer_get();
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
    end = xport_timer_get();
    xport_frame_profile_record(begin, end);
    xport_capture_present(pixels, bitmap_width, bitmap_height, source_x, source_y, source_width, source_height, client_width, client_height, result);
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

RUN_MODE xport_run_mode(void)
{
    return platform_run_mode;
}

sint32 xport_arg_count(void)
{
    return platform_argc;
}

char **xport_arg_values(void)
{
    return platform_argv;
}

void xport_set_exit_code(sint32 code)
{
    platform_exit_code = code;
}

sint32 xport_isquit(void)
{
    return platform_quit;
}

/* Types. */
typedef struct WAVEOUT_state
{
    HWAVEOUT device;
    FILE *capture;
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

static volatile uint32 audio_profile_output_samples;
static volatile uint32 audio_profile_rail_samples;

typedef struct XPORT_FRAME_PROFILE
{
    FILE *stream;
    uint64 frame;
    uint64 last_begin;
    uint64 last_kernel;
    uint64 last_user;
    uint64 last_process_kernel;
    uint64 last_process_user;
    uint64 wait_us;
    uint32 last_submitted;
    uint32 last_overruns;
    uint32 last_output_samples;
    uint32 last_rail_samples;
    sint32 initialized;
} XPORT_FRAME_PROFILE;

static XPORT_FRAME_PROFILE frame_profile;

static void xport_frame_profile_initialize(void)
{
    const char *path;
    if (frame_profile.initialized)
        return;
    frame_profile.initialized = 1;
    path = getenv("XPORT_FRAME_PROFILE");
    if (!path || !*path)
        return;
    frame_profile.stream = fopen(path, "wb");
    if (!frame_profile.stream)
    {
        fprintf(stderr, "Cannot open frame profile: %s\n", path);
        return;
    }
    setvbuf(frame_profile.stream, NULL, _IOFBF, 1024u * 1024u);
    fprintf(frame_profile.stream, "frame,begin_us,interval_us,wait_us,useful_wall_us,present_us,process_cpu_us,main_cpu_us,main_user_cpu_us,main_kernel_cpu_us,audio_submitted,audio_submitted_delta,audio_overruns,audio_overruns_delta,audio_output_samples,audio_output_samples_delta,audio_rail_samples,audio_rail_samples_delta,audio_peak\n");
}

static void xport_frame_profile_wait(uint64 begin, uint64 end)
{
    xport_frame_profile_initialize();
    if (frame_profile.stream && end >= begin)
        frame_profile.wait_us += end - begin;
}

static uint64 xport_filetime_value(FILETIME value)
{
    ULARGE_INTEGER ticks;
    ticks.LowPart = value.dwLowDateTime;
    ticks.HighPart = value.dwHighDateTime;
    return ticks.QuadPart;
}

static void xport_frame_profile_record(uint64 begin, uint64 end)
{
    FILETIME created;
    FILETIME exited;
    FILETIME kernel_time;
    FILETIME user_time;
    uint64 kernel;
    uint64 user;
    uint64 interval = 0u;
    uint64 kernel_delta = 0u;
    uint64 user_delta = 0u;
    uint64 process_kernel;
    uint64 process_user;
    uint64 process_cpu = 0u;
    uint64 useful_wall;
    uint32 submitted;
    uint32 overruns;
    uint32 output_samples;
    uint32 rail_samples;

    xport_frame_profile_initialize();
    if (!frame_profile.stream)
        return;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel_time, &user_time))
        return;
    kernel = xport_filetime_value(kernel_time);
    user = xport_filetime_value(user_time);
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel_time, &user_time))
        return;
    process_kernel = xport_filetime_value(kernel_time);
    process_user = xport_filetime_value(user_time);
    submitted = g_xport_audio_submitted_buffers;
    overruns = g_xport_audio_callback_overruns;
    output_samples = audio_profile_output_samples;
    rail_samples = audio_profile_rail_samples;
    if (frame_profile.last_begin)
    {
        interval = begin - frame_profile.last_begin;
        kernel_delta = kernel - frame_profile.last_kernel;
        user_delta = user - frame_profile.last_user;
        process_cpu = process_kernel + process_user - frame_profile.last_process_kernel - frame_profile.last_process_user;
    }
    useful_wall = interval > frame_profile.wait_us ? interval - frame_profile.wait_us : 0u;
    fprintf(frame_profile.stream, "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%u,%u,%u,%u,%u,%u,%u,%u\n", (unsigned long long)frame_profile.frame, (unsigned long long)begin, (unsigned long long)interval, (unsigned long long)frame_profile.wait_us, (unsigned long long)useful_wall, (unsigned long long)(end - begin), (unsigned long long)(process_cpu / 10u), (unsigned long long)((kernel_delta + user_delta) / 10u), (unsigned long long)(user_delta / 10u), (unsigned long long)(kernel_delta / 10u), (unsigned)submitted, (unsigned)(submitted - frame_profile.last_submitted), (unsigned)overruns, (unsigned)(overruns - frame_profile.last_overruns), (unsigned)output_samples, (unsigned)(output_samples - frame_profile.last_output_samples), (unsigned)rail_samples, (unsigned)(rail_samples - frame_profile.last_rail_samples), (unsigned)g_xport_audio_peak);
    frame_profile.last_begin = begin;
    frame_profile.last_kernel = kernel;
    frame_profile.last_user = user;
    frame_profile.last_process_kernel = process_kernel;
    frame_profile.last_process_user = process_user;
    frame_profile.wait_us = 0u;
    frame_profile.last_submitted = submitted;
    frame_profile.last_overruns = overruns;
    frame_profile.last_output_samples = output_samples;
    frame_profile.last_rail_samples = rail_samples;
    ++frame_profile.frame;
}

static void xport_frame_profile_shutdown(void)
{
    if (frame_profile.stream)
    {
        fclose(frame_profile.stream);
        frame_profile.stream = NULL;
    }
}

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
            if (state->capture)
                fwrite(state->samples[index], sizeof(state->samples[index]), 1, state->capture);
            {
                sint32 sample_index;
                uint32 peak = 0;
                for (sample_index = 0; sample_index < XPORT_AUDIO_BUFFER_FRAMES * 2; ++sample_index)
                {
                    sint32 value = state->samples[index][sample_index];
                    uint32 magnitude = (uint32)(value < 0 ? -value : value);
                    ++audio_profile_output_samples;
                    if (value == 32767 || value == -32768)
                        ++audio_profile_rail_samples;
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
    if (output.capture)
    {
        fclose(output.capture);
        output.capture = NULL;
    }
}

sint32 xport_audio_init(void)
{
    WAVEFORMATEX format;
    MMRESULT result;
    sint32 index;
    const char *audible = getenv("XPORT_AUDIO_OUTPUT");
    const char *backend = getenv("XPORT_AUDIO_BACKEND");
    const char *capture = getenv("XPORT_AUDIO_CAPTURE");
    if (InterlockedCompareExchange(&output.running, 1, 1) != 0)
        return 1;
    g_xport_audio_output_muted = audible != 0 && strcmp(audible, "0") == 0;
    memset(&output, 0, sizeof(output));
    if (capture && *capture)
    {
        output.capture = fopen(capture, "wb");
        if (!output.capture)
            fprintf(stderr, "Cannot open audio capture: %s\n", capture);
        else
            setvbuf(output.capture, NULL, _IOFBF, 1024u * 1024u);
    }
    g_xport_audio_submitted_buffers = 0;
    g_xport_audio_nonzero_buffers = 0;
    g_xport_audio_peak = 0;
    g_xport_audio_callback_overruns = 0;
    audio_profile_output_samples = 0;
    audio_profile_rail_samples = 0;
    if (xport_is_headless() && (backend == NULL || strcmp(backend, "waveout") != 0))
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
    {
        if (output.capture)
        {
            fclose(output.capture);
            output.capture = NULL;
        }
        return 0;
    }
    result = waveOutOpen(&output.device, WAVE_MAPPER, &format, (DWORD_PTR)wave_callback, (DWORD_PTR)&output, CALLBACK_FUNCTION);
    if (result != MMSYSERR_NOERROR)
    {
        CloseHandle(output.semaphore);
        if (output.capture)
            fclose(output.capture);
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
        spu_render_vblank(samples, count);
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

static FILE *collect_log;

FILE *xport_fopen(const char *path, const char *mode)
{
    FILE *file = fopen(path, mode);
    if (file && collect_log && mode[0] == 'r')
    {
        const char *resource = NULL;
        const char *cursor;
        for (cursor = path; *cursor; ++cursor)
            if ((cursor == path || cursor[-1] == '/' || cursor[-1] == '\\') && (!_strnicmp(cursor, "DATA/", 5) || !_strnicmp(cursor, "DATA\\", 5) || !_strnicmp(cursor, "MUSIC/", 6) || !_strnicmp(cursor, "MUSIC\\", 6)))
            {
                resource = cursor;
                break;
            }
        if (resource)
        {
            for (; *resource && *resource != ';'; ++resource)
                fputc(*resource == '\\' ? '/' : *resource, collect_log);
            fputc('\n', collect_log);
            fflush(collect_log);
        }
    }
    return file;
}

sint32 xport_file_size(const char *path, size_t *size)
{
    FILE *file;
    long length;
    if (size != NULL)
        *size = 0;
    if (path == NULL || size == NULL)
        return 0;
    file = xport_fopen(path, "rb");
    if (file == NULL)
        return 0;
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0)
    {
        fclose(file);
        return 0;
    }
    fclose(file);
    *size = (size_t)length;
    return 1;
}

sint32 xport_file_read(const char *path, void *data, size_t capacity, size_t *size)
{
    FILE *file;
    size_t read_size;
    if (size != NULL)
        *size = 0;
    if (path == NULL || data == NULL)
        return 0;
    file = xport_fopen(path, "rb");
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
    xport_frame_profile_shutdown();
    xport_audio_shutdown();
    _XInputGetState = NULL;
    if (platform_xinput_module != NULL)
    {
        FreeLibrary(platform_xinput_module);
        platform_xinput_module = NULL;
    }
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
    int index;
#if defined(LOCKSTEP_DEBUG)
    if (!xport_lockstep_memory_init())
    {
        fprintf(stderr, "lockstep_memory_error=%s\n", xport_lockstep_memory_error_name(xport_lockstep_memory_error()));
        return 2;
    }
    xport_lockstep_memory_ready();
#endif
    xport_memory_init(DRAM, sizeof(DRAM), (uint8 *)SCRATCHPAD, sizeof(SCRATCHPAD), NULL, 0u);
    for (index = 1; index < __argc; ++index)
    {
        if (!strcmp(__argv[index], "--collect"))
        {
            char path[MAX_PATH];
            char *slash;
            DWORD length = GetModuleFileNameA(NULL, path, sizeof(path));
            if (!length || length >= sizeof(path))
                return 1;
            slash = strrchr(path, '\\');
            if (!slash)
                return 1;
            *slash = 0;
            if (strlen(path) + sizeof("/../src/platform/web/collect.log") > sizeof(path))
                return 1;
            strcat(path, "/../src/platform/web/collect.log");
            collect_log = fopen(path, "a");
            if (!collect_log)
            {
                MessageBoxA(NULL, path, "Cannot open collect.log", MB_OK | MB_ICONERROR);
                return 1;
            }
            break;
        }
    }
    platform_run_mode = RUN_MODE_GAME;
    platform_argc = 1;
    platform_argv[0] = __argv[0];
    for (index = 1; index < __argc; ++index)
    {
        if (!strcmp(__argv[index], "--collect"))
            continue;
#if defined(LOCKSTEP_DEBUG)
        if (!strcmp(__argv[index], "--lockstep-replay") && platform_run_mode == RUN_MODE_GAME)
            platform_run_mode = RUN_MODE_LOCKSTEP_REPLAY;
        else if (!strcmp(__argv[index], "--lockstep-boundary-selftest") && platform_run_mode == RUN_MODE_GAME)
            platform_run_mode = RUN_MODE_LOCKSTEP_SELFTEST;
        else
        {
            if (platform_argc + 1 >= (sint32)(sizeof(platform_argv) / sizeof(platform_argv[0])))
                return 64;
            platform_argv[platform_argc++] = __argv[index];
        }
#else
        if (!strcmp(__argv[index], "--lockstep-replay") || !strcmp(__argv[index], "--lockstep-boundary-selftest"))
            return 64;
        if (platform_argc + 1 >= (sint32)(sizeof(platform_argv) / sizeof(platform_argv[0])))
            return 64;
        platform_argv[platform_argc++] = __argv[index];
#endif
    }
    platform_argv[platform_argc] = NULL;
    if (platform_run_mode != RUN_MODE_GAME)
        platform_headless = 1;
    InitializeCriticalSection(&audio_lock);
    audio_lock_initialized = 1;
    xport_timer_init();
    xport_input_init();
    platform_exit_code = 0;
    platform_game_exit_active = 1;
    if (setjmp(platform_game_exit) == 0)
        xport_main();
    platform_game_exit_active = 0;
    result = platform_exit_code;
    xport_shutdown();
#if defined(LOCKSTEP_DEBUG)
    xport_lockstep_memory_shutdown();
#endif
    if (collect_log)
        fclose(collect_log);
    return result;
}

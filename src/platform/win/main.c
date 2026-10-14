#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
// This backend uses the Windows critical-section API
#define XPORT_NO_PSYQ_CRITICAL_SECTION
#include "psx_spu.h"
#include "xport.h"
#include "vid.h"

int xport_hd = 1;

#ifndef SHORT_NAME
    #error SHORT_NAME must be defined by the consuming project
#endif

static sint32 save_path(char path[MAX_PATH], const char *suffix)
{
    DWORD length = GetEnvironmentVariableA("APPDATA", path, MAX_PATH);
    if (!length || length >= MAX_PATH || length + sizeof("\\xport\\" SHORT_NAME ".sav.tmp") > MAX_PATH)
        return 0;
    strcat(path, "\\xport");
    CreateDirectoryA(path, NULL);
    strcat(path, "\\" SHORT_NAME ".sav");
    strcat(path, suffix);
    return 1;
}

sint32 xport_save(const void *data, sint32 size)
{
    char path[MAX_PATH], temporary[MAX_PATH];
    HANDLE file;
    DWORD written;
    sint32 success;
    if (!data || size <= 0 || !save_path(path, "") || !save_path(temporary, ".tmp"))
        return -1;
    file = CreateFileA(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return -1;
    success = WriteFile(file, data, (DWORD)size, &written, NULL) && written == (DWORD)size;
    if (!CloseHandle(file))
        success = 0;
    if (success && MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return 1;
    DeleteFileA(temporary);
    return -1;
}

sint32 xport_load(void *data, sint32 capacity)
{
    char path[MAX_PATH];
    HANDLE file;
    LARGE_INTEGER size;
    DWORD read;
    sint32 result = -1;
    if (!data || capacity <= 0 || !save_path(path, ""))
        return -1;
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
        return 0;
    file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return -1;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= capacity && ReadFile(file, data, (DWORD)size.QuadPart, &read, NULL) && read == (DWORD)size.QuadPart)
        result = (sint32)read;
    CloseHandle(file);
    return result;
}

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
#define XINPUT_STICK_DPAD_THRESHOLD 13107

static HWND platform_window;
static sint32 platform_window_width = WND_WIDTH;
static sint32 platform_window_height = WND_HEIGHT;
static sint32 platform_headless;
static sint32 platform_quit;
static jmp_buf platform_game_exit;
static sint32 platform_game_exit_active;
static void (*platform_cleanup)(void);
static uint32 platform_pad_buttons[2];
static uint8 platform_hd_pressed[2];
/* Four PS1 axes followed by the analog-controller availability flag */
static uint8 platform_pad_analog[2][5];
static sint32 platform_pad_override;
static uint32 platform_override_buttons;
static LARGE_INTEGER platform_timer_frequency;
static LARGE_INTEGER platform_timer_start;
static uint64 platform_frame_deadline;
static sint32 platform_timer_initialized;
static RUN_MODE platform_run_mode;
static sint32 platform_exit_code;
static HMODULE platform_xinput_module;
static XInputGetStateProc _XInputGetState;

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

void xport_render_wait(void)
{
    static uint64 deadline;
    static HMONITOR monitor;
    static uint32 rate = 60;
    HMONITOR current = MonitorFromWindow(platform_window, MONITOR_DEFAULTTOPRIMARY);
    if (monitor != current)
    {
        MONITORINFOEXW info = {0};
        DEVMODEW settings = {0};
        info.cbSize = sizeof(info);
        settings.dmSize = sizeof(settings);
        rate = 60;
        if (GetMonitorInfoW(current, (MONITORINFO *)&info) && EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &settings) && settings.dmDisplayFrequency > 1)
            rate = settings.dmDisplayFrequency;
        monitor = current;
        deadline = 0;
    }
    uint64 now = xport_timer_get();
    uint64 period = 1000000u / rate;
    if (!deadline || now > deadline + period)
        deadline = now + period;
    while (now < deadline)
    {
        uint64 remaining = deadline - now;
        Sleep(remaining > 2000 ? (DWORD)(remaining / 1000 - 1) : 0);
        now = xport_timer_get();
    }
    deadline += period;
}

static uint32 key_to_pad(sint32 controller, WPARAM key, LPARAM flags)
{
    if (controller == 1)
    {
        // Distinguish keypad navigation with Num Lock off from the arrow cluster
        if (!(flags & (1L << 24)))
        {
            switch (key)
            {
                case VK_HOME:
                    key = VK_NUMPAD7;
                    break;
                case VK_UP:
                    key = VK_NUMPAD8;
                    break;
                case VK_PRIOR:
                    key = VK_NUMPAD9;
                    break;
                case VK_LEFT:
                    key = VK_NUMPAD4;
                    break;
                case VK_CLEAR:
                    key = VK_NUMPAD5;
                    break;
                case VK_RIGHT:
                    key = VK_NUMPAD6;
                    break;
                case VK_END:
                    key = VK_NUMPAD1;
                    break;
                case VK_DOWN:
                    key = VK_NUMPAD2;
                    break;
                case VK_NEXT:
                    key = VK_NUMPAD3;
                    break;
            }
        }
        switch (key)
        {
            case VK_UP:
                return PADLup;
            case VK_DOWN:
                return PADLdown;
            case VK_LEFT:
                return PADLleft;
            case VK_RIGHT:
                return PADLright;
            case VK_NUMPAD5:
                return PADRup;
            case VK_NUMPAD1:
                return PADRleft;
            case VK_NUMPAD2:
                return PADRdown;
            case VK_NUMPAD3:
                return PADRright;
            case VK_NUMPAD7:
                return PADL2;
            case VK_NUMPAD4:
                return PADL1;
            case VK_NUMPAD9:
                return PADR2;
            case VK_NUMPAD6:
                return PADR1;
            case VK_ADD:
                return PADselect;
            case VK_RETURN:
                return flags & (1L << 24) ? PADstart : 0;
            default:
                return 0;
        }
    }
    if (controller != 0)
        return 0;
    switch (key)
    {
        case 'W':
            return PADLup;
        case 'S':
            return PADLdown;
        case 'A':
            return PADLleft;
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
            return flags & (1L << 24) ? 0 : PADstart;
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
    if (gamepad->sThumbLX < -XINPUT_STICK_DPAD_THRESHOLD)
        buttons |= PADLleft;
    if (gamepad->sThumbLX > XINPUT_STICK_DPAD_THRESHOLD)
        buttons |= PADLright;
    if (gamepad->sThumbLY < -XINPUT_STICK_DPAD_THRESHOLD)
        buttons |= PADLdown;
    if (gamepad->sThumbLY > XINPUT_STICK_DPAD_THRESHOLD)
        buttons |= PADLup;
    return buttons;
}

static uint8 xinput_axis(SHORT value, sint32 invert)
{
    sint32 scaled = invert ? 32768 - (sint32)value : 32768 + (sint32)value;
    return (uint8)(scaled > 65535 ? 255 : scaled >> 8);
}

static uint32 xinput_read(sint32 player, uint8 axes[5])
{
    DWORD controller;
    XINPUT_STATE state;
    memset(axes, 128, 4);
    axes[4] = 0;
    if (_XInputGetState == NULL)
        return 0;
    for (controller = 0; controller < XUSER_MAX_COUNT; ++controller)
    {
        memset(&state, 0, sizeof(state));
        if (_XInputGetState(controller, &state) == ERROR_SUCCESS)
        {
            if (player-- != 0)
                continue;
            axes[0] = xinput_axis(state.Gamepad.sThumbRX, 0);
            axes[1] = xinput_axis(state.Gamepad.sThumbRY, 1);
            axes[2] = xinput_axis(state.Gamepad.sThumbLX, 0);
            axes[3] = xinput_axis(state.Gamepad.sThumbLY, 1);
            axes[4] = 1;
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
            platform_quit = 1;
            PostQuitMessage(0);
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
        case WM_SETCURSOR:
            if (!((DWORD)GetWindowLongA(window, GWL_STYLE) & WS_OVERLAPPEDWINDOW))
            {
                SetCursor(NULL);
                return TRUE;
            }
            return DefWindowProc(window, message, wparam, lparam);
        case WM_KILLFOCUS:
            memset(platform_pad_buttons, 0, sizeof(platform_pad_buttons));
            return 0;
        case WM_SYSCOMMAND:
            if ((wparam & 0xfff0) == SC_KEYMENU)
                return 0;
            return DefWindowProc(window, message, wparam, lparam);
        case WM_SYSKEYDOWN:
            if (wparam == VK_MENU)
                return 0;
            if (wparam == VK_RETURN && (lparam & (1L << 29)))
            {
                if (!(lparam & (1L << 30)))
                    switch_window_mode();
                return 0;
            }
            return DefWindowProc(window, message, wparam, lparam);
        case WM_SYSKEYUP:
            if (wparam == VK_RETURN || wparam == VK_MENU)
                return 0;
            return DefWindowProc(window, message, wparam, lparam);
        case WM_KEYDOWN:
        case WM_KEYUP:
            if (wparam == VK_TAB)
            {
                if (message == WM_KEYDOWN && !(lparam & (1L << 30)))
                    xport_hd = !xport_hd;
                return 0;
            }
            for (sint32 controller = 0; controller < 2; ++controller)
            {
                button = key_to_pad(controller, wparam, lparam);
                if (message == WM_KEYDOWN)
                    platform_pad_buttons[controller] |= button;
                else
                    platform_pad_buttons[controller] &= ~button;
            }
            return 0;
        default:
            return DefWindowProc(window, message, wparam, lparam);
    }
}

sint32 xport_input_init(void)
{
    memset(platform_pad_buttons, 0, sizeof(platform_pad_buttons));
    memset(platform_hd_pressed, 0, sizeof(platform_hd_pressed));
    memset(platform_pad_analog, 128, sizeof(platform_pad_analog));
    platform_pad_analog[0][4] = platform_pad_analog[1][4] = 0;
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
    if (controller < 0 || controller >= 2)
        return 0;
    if (controller == 0 && platform_pad_override)
        return platform_override_buttons;
    uint32 buttons = xinput_read(controller, platform_pad_analog[controller]);
    uint8 pressed = (buttons & PADi) != 0;
    if (pressed && !platform_hd_pressed[controller])
        xport_hd = !xport_hd;
    platform_hd_pressed[controller] = pressed;
    return platform_pad_buttons[controller] | (buttons & ~PADi);
}

sint32 xport_input_analog(sint32 controller, uint8 axes[4])
{
    memset(axes, 128, 4);
    if (controller < 0 || controller >= 2 || (controller == 0 && platform_pad_override))
        return 0;
    memcpy(axes, platform_pad_analog[controller], 4);
    return platform_pad_analog[controller][4];
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
    window_class.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursor(NULL, IDC_ARROW);
    window_class.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    window_class.lpszClassName = "XportWindow";
    window_class.hIcon = LoadIcon(instance, "MAINICON");
    if (window_class.hIcon == NULL)
        window_class.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    window_class.hIconSm = window_class.hIcon;
    if (!RegisterClassEx(&window_class))
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
    if (!vid_init(platform_window))
    {
        xport_message_error("VID", "OpenGL 3.3 initialization failed");
        DestroyWindow(platform_window);
        return 0;
    }
    ShowWindow(platform_window, SW_SHOW);
    return 1;
}

sint32 xport_present(const uint32 *pixels, sint32 bitmap_width, sint32 bitmap_height, sint32 source_x, sint32 source_y, sint32 source_width, sint32 source_height, const char *title)
{
    VID_RECT src = {source_x, source_y, source_width, source_height};
    xport_poll();
    if (platform_headless)
        return 1;
    if (platform_window == NULL || platform_quit)
        return 0;
    if (title != NULL)
        SetWindowText(platform_window, title);
    return vid_blit(pixels, bitmap_width, bitmap_height, src);
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
#ifndef NDEBUG
        printf("headless_audio backend=null frames=%llu submitted=0 nonzero=%u peak=%u\n", (unsigned long long)null_frames, (unsigned)g_xport_audio_nonzero_buffers, (unsigned)g_xport_audio_peak);
#endif
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
#ifdef _DEBUG
    const char *audible = getenv("XPORT_AUDIO_OUTPUT");
    const char *backend = getenv("XPORT_AUDIO_BACKEND");
#endif
    if (InterlockedCompareExchange(&output.running, 1, 1) != 0)
        return 1;
#ifdef _DEBUG
    g_xport_audio_output_muted = audible != 0 && strcmp(audible, "0") == 0;
#else
    g_xport_audio_output_muted = 0;
#endif
    memset(&output, 0, sizeof(output));
    g_xport_audio_submitted_buffers = 0;
    g_xport_audio_nonzero_buffers = 0;
    g_xport_audio_peak = 0;
    g_xport_audio_callback_overruns = 0;
    if (xport_is_headless()
#ifdef _DEBUG
        && (backend == NULL || strcmp(backend, "waveout") != 0)
#endif
    )
    {
        null_active = 1;
        null_frames = 0;
        g_xport_audio_backend_active = 0;
        InterlockedExchange(&output.running, 1);
#ifndef NDEBUG
        printf("headless_audio backend=null\n");
#endif
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
        return 0;
    }
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
    {
        spu_dispatch_pending_irq();
        return;
    }
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
    spu_dispatch_pending_irq();
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
            char buffer[MAX_PATH + 1];
            size_t length = 0;
            for (; *resource && *resource != ';' && length < MAX_PATH; ++resource)
                buffer[length++] = *resource == '\\' ? '/' : *resource;
            buffer[length++] = '\n';
            fwrite(buffer, 1, length, collect_log);
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
#ifndef NDEBUG
    if (ferror(file))
    {
        fclose(file);
        return 0;
    }
#endif
    fclose(file);
    return 1;
}

sint32 xport_memory_readable(const void *address, size_t size)
{
    uintptr_t begin = (uintptr_t)address;
    return begin >= 0x00200000u && size && size <= UINTPTR_MAX - begin;
}

void xport_message_error(const char *title, const char *message)
{
    MessageBoxA(platform_window, message, title, MB_OK | MB_ICONERROR);
}

void xport_set_cleanup(void (*cleanup)(void))
{
    platform_cleanup = cleanup;
}

void xport_shutdown(void)
{
    void (*cleanup)(void) = platform_cleanup;
    platform_cleanup = NULL;
    if (cleanup)
        cleanup();
    xport_audio_shutdown();
    vid_free();
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
    int collect = strstr(command_line, "--collect") != NULL;
#if defined(LOCKSTEP_DEBUG)
    if (!xport_lockstep_memory_init())
    {
    #ifndef NDEBUG
        fprintf(stderr, "lockstep_memory_error=%s\n", xport_lockstep_memory_error_name(xport_lockstep_memory_error()));
    #endif
        return 2;
    }
    xport_lockstep_memory_ready();
#endif
    xport_memory_init(DRAM, sizeof(DRAM), (uint8 *)SCRATCHPAD, sizeof(SCRATCHPAD), NULL, 0u);
    if (collect)
    {
        const char *path = "../src/platform/web/collect.log";
        collect_log = fopen(path, "a");
        if (!collect_log)
        {
            MessageBoxA(NULL, path, "Cannot open collect.log", MB_OK | MB_ICONERROR);
            return 1;
        }
    }
    platform_run_mode = RUN_MODE_GAME;
#if defined(LOCKSTEP_DEBUG)
    if (strstr(command_line, "--lockstep-replay"))
        platform_run_mode = RUN_MODE_LOCKSTEP_REPLAY;
    else if (strstr(command_line, "--lockstep-boundary-selftest"))
        platform_run_mode = RUN_MODE_LOCKSTEP_SELFTEST;
#endif
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

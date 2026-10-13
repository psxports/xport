#include <emscripten.h>
#include <emscripten/heap.h>
#include <emscripten/html5.h>
#include <stdio.h>
#include <string.h>
#include "psx_spu.h"
#include "xport.h"
#include "vid.h"

int xport_hd = 1;
#include "xport_trace.h"

enum
{
    XPORT_AUDIO_INPUT_FRAMES = 2048,
    XPORT_AUDIO_OUTPUT_FRAMES = 4096
};

static sint32 platform_headless;
static sint32 platform_quit;
static void (*platform_cleanup)(void);
static uint32 platform_pad_buttons[2];
static uint8 platform_hd_pressed[2];
static uint32 platform_touch_buttons;
static sint32 platform_pad_override;
static uint32 platform_override_buttons;
static uint64 platform_frame_deadline;
static uint64 platform_poll_deadline;
static sint32 audio_active;
static RUN_MODE platform_run_mode;
static sint32 platform_exit_code;
static uint32 audio_output_accumulator;
static sint16 audio_input[XPORT_AUDIO_INPUT_FRAMES * 2];
static float audio_output[XPORT_AUDIO_OUTPUT_FRAMES * 2];

volatile uint32 g_xport_audio_submitted_buffers;
volatile uint32 g_xport_audio_nonzero_buffers;
volatile uint32 g_xport_audio_peak;
volatile uint32 g_xport_audio_backend_active;
volatile uint32 g_xport_audio_output_muted;
volatile uint32 g_xport_audio_callback_overruns;

#if defined(__cplusplus)
extern "C"
#endif
    EMSCRIPTEN_KEEPALIVE void
    xport_touch_set(uint32 buttons)
{
    platform_touch_buttons = buttons & 0xffffu;
}

// clang-format off
EM_JS(uint32, xport_gamepad_buttons, (sint32 player), {
    const pads = navigator.getGamepads ? navigator.getGamepads() : [];
    const pressed = (pad, index) => index < pad.buttons.length && (pad.buttons[index].pressed || pad.buttons[index].value > 0.5);
    let buttons = 0;
    let controller = 0;
    for (const pad of pads)
    {
        if (!pad || !pad.connected)
            continue;
        if (controller++ !== player)
            continue;
        if (pad.axes.length > 1)
        {
            if (pad.axes[0] < -0.4)
                buttons |= 0x8000;
            if (pad.axes[0] > 0.4)
                buttons |= 0x2000;
            if (pad.axes[1] < -0.4)
                buttons |= 0x1000;
            if (pad.axes[1] > 0.4)
                buttons |= 0x4000;
        }
        if (pressed(pad, 0))
            buttons |= 0x0040;
        if (pressed(pad, 1))
            buttons |= 0x0020;
        if (pressed(pad, 2))
            buttons |= 0x0080;
        if (pressed(pad, 3))
            buttons |= 0x0010;
        if (pressed(pad, 4))
            buttons |= 0x0004;
        if (pressed(pad, 5))
            buttons |= 0x0008;
        if (pressed(pad, 6))
            buttons |= 0x0001;
        if (pressed(pad, 7))
            buttons |= 0x0002;
        if (pressed(pad, 8))
            buttons |= 0x0100;
        if (pressed(pad, 9))
            buttons |= 0x0800;
        if (pressed(pad, 10))
            buttons |= 0x0200;
        if (pressed(pad, 12))
            buttons |= 0x1000;
        if (pressed(pad, 13))
            buttons |= 0x4000;
        if (pressed(pad, 14))
            buttons |= 0x8000;
        if (pressed(pad, 15))
            buttons |= 0x2000;
    }
    return buttons;
});
// clang-format on

EM_JS(uint32, xport_audio_rate, (), { return Module.xportAudioRate || 44100; });

EM_JS(void, xport_audio_push, (const float *samples, uint32 frames), {
    if (Module.xportAudioPush)
        Module.xportAudioPush(HEAPF32.slice(samples >> 2, (samples >> 2) + frames * 2));
});

EM_JS(void, xport_canvas_title, (const char *title), { document.title = UTF8ToString(title); });

EM_JS(void, xport_error, (const char *title, const char *message), {
    const text = UTF8ToString(title) + ': ' + UTF8ToString(message);
    const status = document.getElementById('status');
    if (status)
        status.textContent = text;
    console.error(text);
});

static uint32 key_to_pad(sint32 controller, const char *code)
{
    if (controller == 1)
    {
        if (!strcmp(code, "ArrowUp"))
            return PADLup;
        if (!strcmp(code, "ArrowDown"))
            return PADLdown;
        if (!strcmp(code, "ArrowLeft"))
            return PADLleft;
        if (!strcmp(code, "ArrowRight"))
            return PADLright;
        if (!strcmp(code, "Numpad5"))
            return PADRup;
        if (!strcmp(code, "Numpad1"))
            return PADRleft;
        if (!strcmp(code, "Numpad2"))
            return PADRdown;
        if (!strcmp(code, "Numpad3"))
            return PADRright;
        if (!strcmp(code, "Numpad7"))
            return PADL2;
        if (!strcmp(code, "Numpad4"))
            return PADL1;
        if (!strcmp(code, "Numpad9"))
            return PADR2;
        if (!strcmp(code, "Numpad6"))
            return PADR1;
        if (!strcmp(code, "NumpadEnter"))
            return PADstart;
        if (!strcmp(code, "NumpadAdd"))
            return PADselect;
        return 0;
    }
    if (controller != 0)
        return 0;
    if (!strcmp(code, "KeyW"))
        return PADLup;
    if (!strcmp(code, "KeyS"))
        return PADLdown;
    if (!strcmp(code, "KeyA"))
        return PADLleft;
    if (!strcmp(code, "KeyD"))
        return PADLright;
    if (!strcmp(code, "KeyI"))
        return PADRup;
    if (!strcmp(code, "KeyK"))
        return PADRdown;
    if (!strcmp(code, "KeyJ"))
        return PADRleft;
    if (!strcmp(code, "KeyL"))
        return PADRright;
    if (!strcmp(code, "KeyQ"))
        return PADL1;
    if (!strcmp(code, "KeyE"))
        return PADR1;
    if (!strcmp(code, "Digit1"))
        return PADL2;
    if (!strcmp(code, "Digit3"))
        return PADR2;
    if (!strcmp(code, "Enter"))
        return PADstart;
    if (!strcmp(code, "Backspace"))
        return PADselect;
    return 0;
}

static EM_BOOL key_event(sint32 type, const EmscriptenKeyboardEvent *event, void *user)
{
    if (!strcmp(event->code, "Tab"))
    {
        if (type == EMSCRIPTEN_EVENT_KEYDOWN && !event->repeat)
            xport_hd = !xport_hd;
        return EM_TRUE;
    }

    EM_BOOL handled = EM_FALSE;
    (void)user;
    for (sint32 controller = 0; controller < 2; ++controller)
    {
        uint32 button = key_to_pad(controller, event->code);
        if (!button)
            continue;
        handled = EM_TRUE;
        if (type == EMSCRIPTEN_EVENT_KEYDOWN)
            platform_pad_buttons[controller] |= button;
        else
            platform_pad_buttons[controller] &= ~button;
    }
    return handled;
}

static EM_BOOL blur_event(sint32 type, const EmscriptenFocusEvent *event, void *user)
{
    (void)type;
    (void)event;
    (void)user;
    memset(platform_pad_buttons, 0, sizeof(platform_pad_buttons));
    return EM_FALSE;
}

uint64 xport_timer_get(void)
{
    return (uint64)(emscripten_get_now() * 1000.0);
}

void xport_timer_wait_frame(uint32 refresh_rate)
{
    uint64 now = xport_timer_get();
    uint64 period;
    if (!refresh_rate)
        refresh_rate = FIELD_RATE;
    period = 1000000u / refresh_rate;
    if (!platform_frame_deadline || now > platform_frame_deadline + period * 4)
        platform_frame_deadline = now + period;
    while (now < platform_frame_deadline)
    {
        emscripten_sleep((unsigned)((platform_frame_deadline - now + 999u) / 1000u));
        now = xport_timer_get();
        platform_poll_deadline = now + period;
    }
    platform_frame_deadline += period;
}

sint32 xport_input_init(void)
{
    memset(platform_pad_buttons, 0, sizeof(platform_pad_buttons));
    memset(platform_hd_pressed, 0, sizeof(platform_hd_pressed));
    platform_touch_buttons = 0;
    platform_pad_override = 0;
    platform_override_buttons = 0;
    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, 0, 1, key_event);
    emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, 0, 1, key_event);
    emscripten_set_blur_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, 0, 1, blur_event);
    return 1;
}

sint32 xport_poll(void)
{
    // Frame waits already dispatch browser events without an extra clamped timer
    if (xport_timer_get() >= platform_poll_deadline)
    {
        emscripten_sleep(0);
        platform_poll_deadline = xport_timer_get() + 1000000u / FIELD_RATE;
    }
    return !platform_quit;
}

EMSCRIPTEN_KEEPALIVE uint32 xport_input_read(sint32 controller)
{
    if (controller < 0 || controller >= 2)
        return 0;
    if (controller == 0 && platform_pad_override)
        return platform_override_buttons;
    uint32 buttons = xport_gamepad_buttons(controller);
    uint8 pressed = (buttons & PADi) != 0;
    if (pressed && !platform_hd_pressed[controller])
        xport_hd = !xport_hd;
    platform_hd_pressed[controller] = pressed;
    return platform_pad_buttons[controller] | (controller == 0 ? platform_touch_buttons : 0) | (buttons & ~PADi);
}

void xport_input_override(sint32 enabled, uint32 buttons)
{
    platform_pad_override = enabled != 0;
    platform_override_buttons = buttons;
}

sint32 xport_window_init(void)
{
    xport_canvas_title(WND_TITLE);
    if (platform_headless)
        return 1;
    if (!vid_init(NULL))
    {
        xport_message_error("VID", "WebGL 2.0 initialization failed");
        return 0;
    }
    return 1;
}

sint32 xport_present(const uint32 *pixels, sint32 bitmap_width, sint32 bitmap_height, sint32 source_x, sint32 source_y, sint32 source_width, sint32 source_height, const char *title)
{
    VID_RECT src = {source_x, source_y, source_width, source_height};
    if (title)
        xport_canvas_title(title);
    if (platform_headless)
        return 1;
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

sint32 xport_audio_init(void)
{
    audio_active = 1;
    audio_output_accumulator = 0;
    g_xport_audio_backend_active = 1;
    return 1;
}

void xport_audio_lock(void)
{
}

void xport_audio_unlock(void)
{
}

void xport_audio_shutdown(void)
{
    audio_active = 0;
    g_xport_audio_backend_active = 0;
}

sint32 xport_audio_is_running(void)
{
    return audio_active;
}

void xport_audio_vblank(uint32 before, uint32 rate)
{
    uint32 input_frames;
    uint32 output_frames;
    uint32 output_rate;
    uint32 index;
    uint32 peak = 0;
    spu_dispatch_pending_irq();
    if (!audio_active || !rate)
        return;
    input_frames = (uint32)(((uint64)(before + 1u) * SPU_SAMPLE_RATE) / rate - ((uint64)before * SPU_SAMPLE_RATE) / rate);
    if (!input_frames || input_frames > XPORT_AUDIO_INPUT_FRAMES)
        return;
    spu_render(audio_input, input_frames);
    spu_dispatch_pending_irq();
    output_rate = xport_audio_rate();
    audio_output_accumulator += output_rate;
    output_frames = audio_output_accumulator / rate;
    audio_output_accumulator %= rate;
    if (!output_frames || output_frames > XPORT_AUDIO_OUTPUT_FRAMES)
        return;
    for (index = 0; index < input_frames * 2; ++index)
    {
        sint32 value = audio_input[index];
        uint32 magnitude = (uint32)(value < 0 ? -value : value);
        if (magnitude > peak)
            peak = magnitude;
    }
    for (index = 0; index < output_frames; ++index)
    {
        uint64 position = (uint64)index * input_frames * 65536u / output_frames;
        uint32 source = (uint32)(position >> 16);
        uint32 fraction = (uint32)position & 0xffffu;
        uint32 next = source + 1 < input_frames ? source + 1 : source;
        sint32 left = audio_input[source * 2] + (sint32)(((sint64)(audio_input[next * 2] - audio_input[source * 2]) * fraction) >> 16);
        sint32 right = audio_input[source * 2 + 1] + (sint32)(((sint64)(audio_input[next * 2 + 1] - audio_input[source * 2 + 1]) * fraction) >> 16);
        audio_output[index * 2] = g_xport_audio_output_muted ? 0.0f : left / 32768.0f;
        audio_output[index * 2 + 1] = g_xport_audio_output_muted ? 0.0f : right / 32768.0f;
    }
    if (peak)
        ++g_xport_audio_nonzero_buffers;
    if (peak > g_xport_audio_peak)
        g_xport_audio_peak = peak;
    ++g_xport_audio_submitted_buffers;
    xport_audio_push(audio_output, output_frames);
}

EM_ASYNC_JS(void, xport_file_prepare, (const char *path), { await Module.xportEnsureFile(UTF8ToString(path)); });

FILE *xport_fopen(const char *path, const char *mode)
{
    if (mode[0] == 'r')
        xport_file_prepare(path);
    return fopen(path, mode);
}

sint32 xport_file_size(const char *path, size_t *size)
{
    FILE *file;
    long length;
    if (size)
        *size = 0;
    if (!path || !size || !(file = xport_fopen(path, "rb")))
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
    if (size)
        *size = 0;
    if (!path || !data || !(file = xport_fopen(path, "rb")))
        return 0;
    read_size = fread(data, 1, capacity, file);
    if (size)
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
    uintptr_t begin = (uintptr_t)address;
    uintptr_t end = begin + size;
    return address && size && end >= begin && end <= emscripten_get_heap_size();
}

void xport_message_error(const char *title, const char *message)
{
    xport_error(title, message);
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
    vid_free();
    xport_audio_shutdown();
}

int main(int argc, char **argv)
{
    int result;
    (void)argc;
    (void)argv;

    platform_run_mode = RUN_MODE_GAME;
    platform_exit_code = 0;
    xport_memory_init(DRAM, sizeof(DRAM), (uint8 *)SCRATCHPAD, sizeof(SCRATCHPAD), NULL, 0u);
    xport_input_init();
    xport_main();
    result = platform_exit_code;
    xport_shutdown();
    return result;
}

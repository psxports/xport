#include <emscripten.h>
#include <emscripten/heap.h>
#include <emscripten/html5.h>
#include <stdio.h>
#include <string.h>
#include "psx_spu.h"
#include "xport.h"
#include "vid.h"

int xport_hd = 1;

#ifndef SHORT_NAME
    #error SHORT_NAME must be defined by the consuming project
#endif

// clang-format off
EM_ASYNC_JS(sint32, save_transfer, (const char *name, void *data, sint32 size, sint32 writing), {
    if (!data || size <= 0)
        return -1;
    let db;
    try
    {
        const key = UTF8ToString(name);
        // Copy before yielding so a growing WASM heap cannot invalidate the view
        const bytes = writing ? HEAPU8.slice(data, data + size) : null;
        db = await new Promise((resolve, reject) => {
            let blocked = false;
            const request = indexedDB.open('XPORT_SAVES', 1);
            request.onupgradeneeded = () => request.result.createObjectStore('saves');
            request.onsuccess = () =>
            {
                if (blocked)
                    request.result.close();
                else
                    resolve(request.result);
            };
            request.onerror = () => reject(request.error);
            request.onblocked = () =>
            {
                blocked = true;
                reject(new Error('Save database blocked'));
            };
        });
        return await new Promise((resolve, reject) => {
            const transaction = db.transaction('saves', writing ? 'readwrite' : 'readonly');
            const store = transaction.objectStore('saves');
            let loaded;
            if (writing)
                store.put(bytes, key);
            else
            {
                const request = store.get(key);
                request.onsuccess = () =>
                {
                    loaded = request.result;
                };
            }
            transaction.onabort = transaction.onerror = () => reject(transaction.error);
            transaction.oncomplete = () =>
            {
                if (writing)
                {
                    resolve(1);
                    return;
                }
                if (loaded === undefined)
                {
                    resolve(0);
                    return;
                }
                if (!(loaded instanceof Uint8Array) || !loaded.length || loaded.length > size)
                {
                    resolve(-1);
                    return;
                }
                HEAPU8.set(loaded, data);
                resolve(loaded.length);
            };
        });
    }
    catch (error)
    {
        return -1;
    }
    finally
    {
        if (db)
            db.close();
    }
});
// clang-format on

sint32 xport_save(const void *data, sint32 size)
{
    return save_transfer(SHORT_NAME, (void *)data, size, 1);
}

sint32 xport_load(void *data, sint32 capacity)
{
    return save_transfer(SHORT_NAME, data, capacity, 0);
}

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
static uint32 platform_gamepad_buttons[2];
// Zero selects keyboard or touch, one selects gamepad
static uint8 platform_input_type[2];
static uint8 platform_hd_pressed[2];
/* Four PS1 axes, analog availability and physical D-pad bits */
static uint8 platform_pad_analog[2][6];
static uint32 platform_touch_buttons;
static sint32 platform_pad_override;
static uint32 platform_override_buttons;
static uint64 platform_poll_deadline;
static sint32 audio_active;
static RUN_MODE platform_run_mode;
static sint32 platform_exit_code;
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
    if (buttons & ~platform_touch_buttons & 0xffffu)
    {
        platform_input_type[0] = 0;
        xport_input_rumble(0, 0, 0);
    }
    platform_touch_buttons = buttons & 0xffffu;
}

// clang-format off
EM_JS(uint32, xport_gamepad_buttons, (sint32 player, uint8 *axes, sint32 pad_hack), {
    HEAPU8.fill(128, axes, axes + 4);
    HEAPU8[axes + 4] = 0;
    HEAPU8[axes + 5] = 0;
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
        const axis = index =>
        {
            const value = pad.axes[index];
            return Number.isFinite(value) ? Math.max(0, Math.min(255, Math.floor((value + 1) * 128))) : 128;
        };
        HEAPU8[axes] = axis(2);
        HEAPU8[axes + 1] = axis(3);
        HEAPU8[axes + 2] = axis(0);
        HEAPU8[axes + 3] = axis(1);
        HEAPU8[axes + 4] = pad.axes.length >= 4 ? 1 : 0;
        HEAPU8[axes + 5] = (pressed(pad, 12) ? 1 : 0) | (pressed(pad, 13) ? 2 : 0) | (pressed(pad, 14) ? 4 : 0) | (pressed(pad, 15) ? 8 : 0);
        if (pad_hack && pad.axes.length > 1)
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

// clang-format off
EM_JS(sint32, xport_gamepad_rumble, (sint32 player, uint16 low, uint16 high, sint32 query), {
    if (player < 0 || player >= 2)
        return 0;
    const pads = navigator.getGamepads ? navigator.getGamepads() : [];
    let index = 0;
    for (const pad of pads)
    {
        if (!pad || !pad.connected || index++ !== player)
            continue;
        const actuator = pad.vibrationActuator;
        if (!actuator || typeof actuator.playEffect !== 'function' || (actuator.effects && !actuator.effects.includes('dual-rumble')))
            return 0;
        if (query)
            return 1;
        if (!document.hasFocus())
            low = high = 0;
        const states = Module.xportRumble || (Module.xportRumble = new Map());
        const prev = states.get(pad.index);
        const now = performance.now();
        if (prev &&prev.actuator === actuator &&prev.low === low &&prev.high === high && (!low && !high || now - prev.time < 100))
            return 1;
        states.set(pad.index, {actuator, low, high, time : now});
        try
        {
            const effect = !low &&!high &&typeof actuator.reset === 'function' ? actuator.reset() : actuator.playEffect('dual-rumble', {duration : 250, startDelay : 0, strongMagnitude : low / 65535, weakMagnitude : high / 65535});
            Promise.resolve(effect).catch(() => {
                if (states.get(pad.index)?.time === now)
                    states.delete(pad.index);
            });
        }
        catch (error)
        {
            states.delete(pad.index);
        }
        return 1;
    }
    return 0;
});
// clang-format on

sint32 xport_input_rumble_supported(sint32 controller)
{
    return !(controller == 0 && platform_pad_override) && xport_gamepad_rumble(controller, 0, 0, 1);
}

void xport_input_rumble(sint32 controller, uint16 low, uint16 high)
{
    if (controller < 0 || controller >= 2)
        return;
    if (!platform_input_type[controller] || (controller == 0 && platform_pad_override))
        low = high = 0;
    xport_gamepad_rumble(controller, low, high, 0);
}

EM_JS(uint32, xport_audio_rate, (), { return Module.xportAudioRate || 44100; });

EM_JS(void, xport_audio_push, (const float *samples, uint32 frames), {
    if (Module.xportAudioPush)
        Module.xportAudioPush(HEAPF32.slice(samples >> 2, (samples >> 2) + frames * 2));
});

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
        {
            if (!(platform_pad_buttons[controller] & button))
            {
                platform_input_type[controller] = 0;
                xport_input_rumble(controller, 0, 0);
            }
            platform_pad_buttons[controller] |= button;
        }
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
    xport_input_rumble(0, 0, 0);
    xport_input_rumble(1, 0, 0);
    return EM_FALSE;
}

uint64 xport_timer_get(void)
{
    static uint64 start;
    static sint32 initialized;
    uint64 now = (uint64)(emscripten_get_now() * 1000.0);
    if (!initialized)
    {
        start = now;
        initialized = 1;
    }
    return now - start;
}

EM_ASYNC_JS(void, xport_render_wait, (void), { await new Promise(function(wake) { requestAnimationFrame(wake); }); });

void xport_timer_wait_frame(uint32 refresh_rate)
{
    uint64 idx;
    uint64 now = xport_timer_get();
    uint64 deadline;
    if (!refresh_rate || refresh_rate == FIELD_RATE)
    {
        xport_frame_fract(&idx);
        deadline = xport_frame_time(idx + 1);
    }
    else
    {
        idx = now / 1000000 * refresh_rate + now % 1000000 * refresh_rate / 1000000;
        deadline = ((idx + 1) * 1000000 + refresh_rate - 1) / refresh_rate;
    }
    while (now < deadline)
    {
        emscripten_sleep((unsigned)((deadline - now + 999u) / 1000u));
        now = xport_timer_get();
        platform_poll_deadline = now + 1000000u / FIELD_RATE;
    }
}

sint32 xport_input_init(void)
{
    memset(platform_pad_buttons, 0, sizeof(platform_pad_buttons));
    memset(platform_gamepad_buttons, 0, sizeof(platform_gamepad_buttons));
    memset(platform_input_type, 0, sizeof(platform_input_type));
    memset(platform_hd_pressed, 0, sizeof(platform_hd_pressed));
    memset(platform_pad_analog, 128, sizeof(platform_pad_analog));
    platform_pad_analog[0][4] = platform_pad_analog[1][4] = 0;
    platform_pad_analog[0][5] = platform_pad_analog[1][5] = 0;
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
    uint32 buttons = xport_gamepad_buttons(controller, platform_pad_analog[controller], XPORT_PAD_HACK);
    if (buttons & ~platform_gamepad_buttons[controller])
        platform_input_type[controller] = 1;
    platform_gamepad_buttons[controller] = buttons;
#if XPORT_PAD_HACK
    uint8 *axes = platform_pad_analog[controller];
    uint32 directions = platform_pad_buttons[controller] | (controller == 0 ? platform_touch_buttons : 0);
    if (axes[5] & 1u)
        directions |= PADLup;
    if (axes[5] & 2u)
        directions |= PADLdown;
    if (axes[5] & 4u)
        directions |= PADLleft;
    if (axes[5] & 8u)
        directions |= PADLright;
    if (directions & (PADLleft | PADLright))
        axes[2] = (directions & PADLleft) ? ((directions & PADLright) ? 128 : 0) : 255;
    if (directions & (PADLup | PADLdown))
        axes[3] = (directions & PADLup) ? ((directions & PADLdown) ? 128 : 0) : 255;
#endif
    uint8 pressed = (buttons & PADi) != 0;
    if (pressed && !platform_hd_pressed[controller])
        xport_hd = !xport_hd;
    platform_hd_pressed[controller] = pressed;
    return platform_pad_buttons[controller] | (controller == 0 ? platform_touch_buttons : 0) | (buttons & ~PADi);
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
    (void)title;
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
    input_frames = (rate == FIELD_RATE ? xport_field_sample_count(before, SPU_SAMPLE_RATE) : (uint32)(((uint64)(before + 1u) * SPU_SAMPLE_RATE) / rate - ((uint64)before * SPU_SAMPLE_RATE) / rate));
    if (!input_frames || input_frames > XPORT_AUDIO_INPUT_FRAMES)
        return;
    spu_render(audio_input, input_frames);
    spu_dispatch_pending_irq();
    output_rate = xport_audio_rate();
    output_frames = rate == FIELD_RATE ? xport_field_sample_count(before, output_rate) : (uint32)(((uint64)(before + 1u) * output_rate) / rate - ((uint64)before * output_rate) / rate);
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
    xport_input_rumble(0, 0, 0);
    xport_input_rumble(1, 0, 0);
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
#if !defined(LOCKSTEP_DEBUG) && defined(XPORT_NATIVE_VM) && XPORT_NATIVE_VM
    xport_memory_bind_mmio(0x1F801C00, 0x200, spu_mmio_read, spu_mmio_write, NULL);
#endif
    xport_input_init();
    xport_main();
    result = platform_exit_code;
    xport_shutdown();
    return result;
}

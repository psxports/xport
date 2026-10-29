#ifndef XPORT_H
#define XPORT_H

#include "ap.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(_DEBUG)
    #define XASSERT(x) \
        do \
        { \
            if (!(x)) \
            { \
                fprintf(stderr, "%s:%d: Assertion failed: %s\n", __FILE__, __LINE__, #x); \
                abort(); \
            } \
        } while (0)
#else
    #define XASSERT(x) ((void)0)
#endif

typedef uint8_t uint8;
typedef int8_t sint8;
typedef uint16_t uint16;
typedef int16_t sint16;
typedef uint32_t uint32;
typedef int32_t sint32;
typedef uint64_t uint64;
typedef int64_t sint64;
typedef intptr_t intptr;

typedef union
{
    struct
    {
        float x, y, z;
    };

    float v[3];
} VEC3F;

typedef union
{
    struct
    {
        float x, y, z, w;
    };

    float v[4];
} QUATF;

#define PADLup (1 << 12)
#define PADLdown (1 << 14)
#define PADLleft (1 << 15)
#define PADLright (1 << 13)
#define PADRup (1 << 4)
#define PADRdown (1 << 6)
#define PADRleft (1 << 7)
#define PADRright (1 << 5)
#define PADi (1 << 9)
#define PADj (1 << 10)
#define PADk (1 << 8)
#define PADl (1 << 3)
#define PADm (1 << 1)
#define PADn (1 << 2)
#define PADo (1 << 0)
#define PADh (1 << 11)
#define PADL1 PADn
#define PADL2 PADo
#define PADR1 PADl
#define PADR2 PADm
#define PADstart PADh
#define PADselect PADk
#define MOUSEleft (1 << 3)
#define MOUSEright (1 << 2)
#define _PAD(controller, buttons) ((buttons) << ((controller) << 4))

#if INTPTR_MAX == INT32_MAX
    #define AP_32BIT 1
#endif

#if !defined(WND_TITLE)
    #error WND_TITLE must be defined by the consuming project
#endif
#if !defined(WND_WIDTH)
    #error WND_WIDTH must be defined by the consuming project
#endif
#if !defined(WND_HEIGHT)
    #error WND_HEIGHT must be defined by the consuming project
#endif
#if !defined(FIELD_RATE)
    #error FIELD_RATE must be defined by the consuming project
#endif
#ifndef FIELD_RATE_NUMERATOR
    #if FIELD_RATE == 50
        #define FIELD_RATE_NUMERATOR 53203425
    #elif FIELD_RATE == 60
        #define FIELD_RATE_NUMERATOR 53693175
    #else
        #define FIELD_RATE_NUMERATOR FIELD_RATE
    #endif
#endif
#ifndef FIELD_RATE_DENOMINATOR
    #if FIELD_RATE == 50
        #define FIELD_RATE_DENOMINATOR (3406 * 314)
    #elif FIELD_RATE == 60
        #define FIELD_RATE_DENOMINATOR (3413 * 263)
    #else
        #define FIELD_RATE_DENOMINATOR 1
    #endif
#endif
#ifndef XPORT_PAD_HACK
    #define XPORT_PAD_HACK 0
#endif
#ifndef XPORT_AUDIO_VBLANK_QUEUE
    #define XPORT_AUDIO_VBLANK_QUEUE 0
#endif
#if FIELD_RATE_NUMERATOR <= 0 || FIELD_RATE_DENOMINATOR <= 0
    #error Physical field rate must be positive
#endif

/* Keep nominal SDK rates separate from the physical video clock */
static __inline uint32 xport_field_sample_count(uint32 before, uint32 sample_rate)
{
    uint64 scaled = (uint64)sample_rate * FIELD_RATE_DENOMINATOR;
    uint64 remainder = scaled % FIELD_RATE_NUMERATOR;
    return (uint32)(scaled / FIELD_RATE_NUMERATOR + (((uint64)before * remainder % FIELD_RATE_NUMERATOR) + remainder) / FIELD_RATE_NUMERATOR);
}

#if defined(_DEBUG) && defined(_MSC_VER)
    #define GDB_DATA __declspec(dllexport)
#else
    #define GDB_DATA
#endif

typedef struct PSX_RECT PSX_RECT;

typedef enum RUN_MODE
{
    RUN_MODE_GAME = 0
} RUN_MODE;

#if defined(__cplusplus)
extern "C"
{
#endif

    /* Map a complete guest address into shared PSX memory */
    void *psx_addr(uint32 address, size_t count);

    /* Platform services implemented by the selected backend */
    extern int xport_hd;
    typedef sint32 (*XportFunctionKeyHandler)(uint32 key);
    // Deliver non-repeating F1..F12 presses; return one to consume the key
    void xport_set_function_key_handler(XportFunctionKeyHandler handler);
    void xport_set_cleanup(void (*cleanup)(void));
    typedef void (*XportCleanup)(void);
    XportCleanup xport_get_cleanup(void);
    // Fullscreen playback returns 1 at EOF, 2 when interrupted and -1 on failure
    sint32 xport_fmv_play(const char *path);
    // Include the final source frame; zero selects physical EOF
    sint32 xport_fmv_play_range(const char *path, uint32 last_frame, sint32 repeat);

    typedef struct XportFmvPresentation
    {
        uint32 width, height, bottom_rows;
        sint32 video_x, video_y;
        sint32 screen_x, screen_y;
        uint32 screen_width, screen_height;
        sint32 interlace;
        uint32 controllers, skip_buttons;
        sint32 skip_held;
        void *context;
        sint32 (*frame)(void *context, uint32 number);
        sint32 (*strip)(void *context, uint32 word_x, uint8 *rgb, uint32 rows);
        sint32 (*tick)(void *context);
        sint32 (*finish)(void *context, uint32 step);
    } XportFmvPresentation;

    sint32 xport_fmv_play_presented(const char *path, uint32 last_frame, sint32 repeat, const XportFmvPresentation *presentation);
    sint32 xport_fmv_ready(void);
    typedef struct XportFmvAudioRemainder XportFmvAudioRemainder;
    // Take native XA audio once after successful synchronous playback
    sint32 xport_fmv_take_audio(XportFmvAudioRemainder *audio);
    void xport_fmv_discard_audio(void);

    typedef struct XportFmvFrame
    {
        uint32 width, height, number;
    } XportFmvFrame;

    typedef struct XportFmvBackend
    {
        sint32 (*open)(void *context, const char *path);
        sint32 (*next)(void *context, uint8 *rgb, uint32 capacity, XportFmvFrame *frame);
        void (*pause)(void *context, sint32 paused);
        void (*close)(void *context);
        uint32 (*audio)(void *context, sint16 *output, uint32 frames);
        sint32 (*has_audio)(void *context);
    } XportFmvBackend;

    // Copy the backend callbacks while no source session is open; NULL restores the platform source
    sint32 xport_fmv_set_backend(const XportFmvBackend *backend, void *context);
    // Open a source session without an owned game loop
    sint32 xport_fmv_open(const char *path);
    // Return 0 while waiting, 1 for RGB24, 2 at EOF and -1 on error
    sint32 xport_fmv_next(uint8 *rgb, uint32 capacity, XportFmvFrame *frame);
    void xport_fmv_pause(sint32 paused);
    void xport_fmv_close(void);
    void xport_shutdown(void);
    uint64 xport_timer_get(void);

    static __inline float xport_frame_fract(uint64 *idx)
    {
        uint64 elapsed = xport_timer_get();
        uint64 seconds = elapsed / 1000000 * FIELD_RATE_NUMERATOR;
        uint64 fraction = seconds % FIELD_RATE_DENOMINATOR * 1000000 + elapsed % 1000000 * FIELD_RATE_NUMERATOR;
        uint64 period = (uint64)1000000 * FIELD_RATE_DENOMINATOR;
        *idx = seconds / FIELD_RATE_DENOMINATOR + fraction / period;
        return (float)(fraction % period) / (float)period;
    }

    static __inline uint64 xport_frame(void)
    {
        uint64 idx;
        xport_frame_fract(&idx);
        return idx;
    }

    void xport_timer_wait_frame(uint64 target_field);
    /* Web yields to the next animation frame; Windows presentation already waits for VSync */
    sint32 xport_render_wait_until(uint64 deadline);
    // Yield after a completed screen presentation on browser hosts
    void xport_present_complete(void);
    sint32 xport_input_init(void);
    sint32 xport_poll(void);
    uint32 xport_input_read(sint32 controller);
    sint32 xport_input_analog(sint32 controller, uint8 axes[4]);
    sint32 xport_input_rumble_supported(sint32 controller);
    void xport_input_rumble(sint32 controller, uint16 low, uint16 high);
    void xport_input_override(sint32 enabled, uint32 buttons);
    sint32 xport_window_init(void);
    sint32 xport_present(const uint32 *pixels, sint32 bitmap_width, sint32 bitmap_height, sint32 source_x, sint32 source_y, sint32 source_width, sint32 source_height, const char *title);
    sint32 xport_is_headless(void);
    void xport_set_headless(sint32 headless);
    sint32 xport_isquit(void);
    sint32 xport_audio_init(void);
    void xport_audio_lock(void);
    void xport_audio_unlock(void);
    void xport_audio_shutdown(void);
    void xport_audio_vblank(uint32 before, uint32 rate);
    sint32 xport_audio_is_running(void);
    sint32 xport_file_size(const char *path, size_t *size);
    FILE *xport_fopen(const char *path, const char *mode);
    size_t xport_fread(void *data, size_t size, size_t count, FILE *stream);
    sint32 xport_file_read(const char *path, void *data, size_t capacity, size_t *size);
    sint32 xport_save(const void *data, sint32 size);
    sint32 xport_load(void *data, sint32 capacity);
    sint32 xport_memory_readable(const void *address, size_t size);
    uint32 xport_memory_read(uint32 address, uint32 width);
    uint32 xport_memory_read_masked_u32(uint32 address, uint32 byte_mask);
    sint32 xport_memory_write(uint32 address, uint32 width, uint32 value, uint32 byte_mask);
    void xport_message_error(const char *title, const char *message);
    RUN_MODE xport_run_mode(void);

    void xport_set_exit_code(sint32 code);

    extern GDB_DATA volatile uint32 g_xport_audio_submitted_buffers;
    extern GDB_DATA volatile uint32 g_xport_audio_nonzero_buffers;
    extern GDB_DATA volatile uint32 g_xport_audio_peak;
    extern GDB_DATA volatile uint32 g_xport_audio_backend_active;
    extern GDB_DATA volatile uint32 g_xport_audio_output_muted;
    extern GDB_DATA volatile uint32 g_xport_audio_callback_overruns;

    /* Required link-time services implemented by every game */
    void xport_main(void);
    // Native consumers select GPU and CD ownership independently
#if defined(XPORT_NATIVE_GPU_CD)
    #define xport_gpu_graph_type_address 0u
#else
// Zero selects the native standard GPU, nonzero selects a guest PsyQ byte
extern const uint32 xport_gpu_graph_type_address;
#endif
#if defined(XPORT_NATIVE_GPU_CD) || (defined(XPORT_NATIVE_CD) && XPORT_NATIVE_CD)
    #define xport_cd_sync_callback_address 0u
    #define xport_cd_ready_callback_address 0u
    #define xport_cd_status_address 0u
    #define xport_cd_setloc_table_address 0u
#else
// Native CD consumers use typed callbacks without guest PsyQ state
extern const uint32 xport_cd_sync_callback_address;
extern const uint32 xport_cd_ready_callback_address;
extern const uint32 xport_cd_status_address;
extern const uint32 xport_cd_setloc_table_address;
#endif
    uint32 xport_libsnd_sequence_call(uint32 target, uint32 argc, const uint32 *arguments);
    void xport_function_marker(const char *image, uint32 address, const char *function, const char *file, uint32 line, uintptr_t host_return_address, uint32 channels);

#if defined(__cplusplus)
}
#endif
/* Raw guest-memory lvalues for direct IDA pseudocode translation */
#define byte_(address) (*(uint8 *)psx_addr((uint32)(address), sizeof(uint8)))
#define word_(address) (*(uint16 *)psx_addr((uint32)(address), sizeof(uint16)))
#define dword_(address) (*(uint32 *)psx_addr((uint32)(address), sizeof(uint32)))

#if defined(_MSC_VER)
    #include <intrin.h>
    #define XPORT_FUNCTION_RETURN_ADDRESS() ((uintptr_t)_ReturnAddress())
#elif defined(__GNUC__) || defined(__clang__)
    #define XPORT_FUNCTION_RETURN_ADDRESS() ((uintptr_t)__builtin_return_address(0))
#else
    #define XPORT_FUNCTION_RETURN_ADDRESS() ((uintptr_t)0)
#endif
#if defined(XPORT_ENABLE_AUDIT)
    #define XPORT_FUNCTION_AUDIT_CHANNEL 1u
#else
    #define XPORT_FUNCTION_AUDIT_CHANNEL 0u
#endif
#if defined(XPORT_DISABLE_FUNCTION_MARKERS)
    #define FUNCTION_MARKER(address, image) ((void)0)
#else
    #define FUNCTION_MARKER(address, image) xport_function_marker((image), (uint32)(address), __func__, __FILE__, __LINE__, XPORT_FUNCTION_RETURN_ADDRESS(), XPORT_FUNCTION_AUDIT_CHANNEL)
#endif

#define XPORT_TODO abort()

#endif

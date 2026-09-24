#ifndef XPORT_H
#define XPORT_H

#include <stddef.h>
#include <stdint.h>

typedef uint8_t uint8;
typedef int8_t sint8;
typedef uint16_t uint16;
typedef int16_t sint16;
typedef uint32_t uint32;
typedef int32_t sint32;
typedef uint64_t uint64;
typedef int64_t sint64;
typedef intptr_t intptr;

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

#define FUNCTION_MARKER(address, image) ((void)0)

#if defined(_DEBUG) && defined(_MSC_VER)
    #define GDB_CALL __declspec(dllexport) __declspec(noinline)
    #define GDB_DATA __declspec(dllexport)
#else
    #define GDB_CALL
    #define GDB_DATA
#endif

typedef struct PSX_RECT PSX_RECT;

#if defined(__cplusplus)
extern "C"
{
#endif

    /* Map a complete guest address into shared PSX memory */
    void *psx_addr(uint32 address, size_t count);

    /* Platform services implemented by the selected backend */
    void xport_shutdown(void);
    uint64 xport_timer_get(void);
    void xport_timer_wait_frame(uint32 refresh_rate);
    sint32 xport_input_init(void);
    sint32 xport_poll(void);
    uint32 xport_input_read(sint32 controller);
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
    sint32 xport_file_read(const char *path, void *data, size_t capacity, size_t *size);
    sint32 xport_memory_readable(const void *address, size_t size);
    void xport_message_error(const char *title, const char *message);

    extern GDB_DATA volatile uint32 g_xport_audio_submitted_buffers;
    extern GDB_DATA volatile uint32 g_xport_audio_nonzero_buffers;
    extern GDB_DATA volatile uint32 g_xport_audio_peak;
    extern GDB_DATA volatile uint32 g_xport_audio_backend_active;
    extern GDB_DATA volatile uint32 g_xport_audio_output_muted;
    extern GDB_DATA volatile uint32 g_xport_audio_callback_overruns;

    /* Required link-time services implemented by every game */
    int xport_main(int argc, char **argv);

#if defined(__cplusplus)
}
#endif

/* Raw guest-memory lvalues for direct IDA pseudocode translation */
#define byte_(address) (*(uint8 *)psx_addr((uint32)(address), sizeof(uint8)))
#define word_(address) (*(uint16 *)psx_addr((uint32)(address), sizeof(uint16)))
#define dword_(address) (*(uint32 *)psx_addr((uint32)(address), sizeof(uint32)))

#endif

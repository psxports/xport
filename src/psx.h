#ifndef PSX_H
#define PSX_H

/* Portable PsyQ compatibility surface. The selected xport platform backend
 * supplies the window, input, timer, audio output and file services. */

#include "xport.h"
#include "psx_press.h"

/* Represent recovered MIPS BREAK instructions without a callable SDK symbol */
#if defined(_MSC_VER)
    #define trap(code) ((void)(code), __debugbreak())
#else
    #define trap(code) ((void)(code), __builtin_trap())
#endif

#if defined(__cplusplus)
extern "C"
{
#endif

    /* Access shared guest memory through stable debugger symbols */
    GDB_CALL uint8 r_u8(uint32 address);
    GDB_CALL uint16 r_u16(uint32 address);
    GDB_CALL uint32 r_u32(uint32 address);
    GDB_CALL sint8 r_s8(uint32 address);
    GDB_CALL sint16 r_s16(uint32 address);
    GDB_CALL sint32 r_s32(uint32 address);
    GDB_CALL sint32 w_u8(uint32 address, uint32 value);
    GDB_CALL sint32 w_u16(uint32 address, uint32 value);
    GDB_CALL sint32 w_u32(uint32 address, uint32 value);
    GDB_CALL sint32 w_masked_u32(uint32 address, uint32 value, uint32 byte_mask);

/* Use the PsyQ name instead of the Windows API macro */
#ifdef OpenEvent
    #undef OpenEvent
#endif
    /* Synchronous digital PAD serial transport */
    sint32 psx_pad_serial_transfer(uint32 owner, sint32 value, uint32 port, uint32 skip_initial, sint32 timeout_error);
    uint32 psx_pad_serial_read(uint32 port);
    uint32 psx_pad_serial_ready(uint32 port);
    uint32 psx_pad_serial_release_ack(uint32 port);
    typedef uint32 (*PsxPadOwnerCallback)(uint32 owner);

    typedef struct
    {
        uint32 target;
        PsxPadOwnerCallback callback;
    } PsxPadCallbackBinding;

    void psx_pad_bind_callbacks(const PsxPadCallbackBinding *bindings, uint32 count);
    uint32 psx_pad_owner_cleanup(uint32 owner);
    uint32 psx_pad_owner_begin(uint32 owner, uint32 cleanup_target);
    void psx_pad_bind_owner_packets(uint32 owner0, uint32 owner1);
    void psx_pad_bind_main_mode_commands(uint32 send_target, uint32 receive_target);
    sint32 openPSX(const char *path, uint32 mode);
    sint32 readPSX(sint32 descriptor, void *destination, uint32 length);
    sint32 writePSX(sint32 fd, const void *src, uint32 length);
    sint32 closePSX(sint32 descriptor);
    sint32 erase(const char *path);

    void gte_write_h(uint16 h);
    uint16 gte_read_h(void);
    void xport_missing_pad_cleanup_callback(uint32 target, uint32 owner);
    void xport_missing_guest_sprintf_format(char *destination, uint32 format_address);
    uint32 xport_missing_pad_owner_callback(uint32 target, uint32 owner);
    uint32 bzeroPSX(uint32 destination, uint32 length);
    uint32 SysEnqIntRP(uint32 priority, uint32 node);
    uint32 ChangeClearRCnt(uint32 counter, uint32 mode);
    uint32 xport_missing_cd_sync_guest_callback(uint32 guest_callback);
    uint32 _SpuDataCallback(uint32 guest_callback);
#if defined(__cplusplus)
    extern "C"
    {
#endif
        uint32 xport_guest_buffer_address(void *host_buffer, size_t bytes);
#if defined(__cplusplus)
    }
#endif
    uint32 ChangeClearPAD(uint32 mode);
    sint32 CdReset(sint32 mode);
    sint32 CdControlBPSX(uint8 command, uint8 *parameter, uint8 *result);
    sint32 StopRCnt(uint32 counter, uint32 control_slot, uint32 mask_table);
    sint32 StopCallback(void);
    sint32 Exec(uint32 guest_header, uint32 argc, uint32 argv);
    sint32 randPSX(void);
    sint32 psyq_rand_next(uint32 *seed);
    sint32 __ltdf2(double left, double right);
    extern sint32 math_errno, math_err_point;
    sint32 _err_math(sint32 error, sint32 point);
    float __floatsisf(sint32 value);
    sint32 __fixsfsi(float value);
    float __divsf3(float numerator, float denominator);
    double __extendsfdf2(float value);
    sint32 __fixdfsi(double value);
    double __floatsidf(sint32 value);
    /* Explicit soft-float carriers avoid x87 quieting of signaling NaNs */
    uint32 psx_float_bits(float value);
    float psx_float_value(uint32 bits);
    uint32 __mulsf3PSX(uint32 left, uint32 right);
    uint64 psx_double_bits(double value);
    double psx_double_value(uint64 bits);
    uint64 __adddf3PSX(uint64 left, uint64 right);
    uint64 __muldf3PSX(uint64 left, uint64 right);
    uint64 __divdf3PSX(uint64 left, uint64 right);
    float __truncdfsf2(double value);
    void qsortPSX(void *base, size_t count, size_t size, int (*compare)(const void *, const void *));
    sint32 OpenEventPSX(uint32 event_class, uint32 specification, uint32 mode, uint32 guest_callback);
    sint32 EnableEventPSX(uint32 handle);
    sint32 DisableEventPSX(uint32 handle);
    sint32 CloseEventPSX(uint32 handle);
    uint32 psx_bios_init_kernel_heap(uint32 base, uint32 bytes);
    uint32 psx_bios_kernel_malloc(uint32 bytes);
    uint32 psx_bios_init_rcnt(uint32 priority);
    uint32 psx_bios_init_default_interrupts(uint32 priority);
    uint32 psx_bios_enqueue_timer_vblank(uint32 priority);
    uint32 psx_bios_reset_entry_interrupt(void);
    uint32 psx_bios_init_user_heap(uint32 base, uint32 bytes);
    uint32 InitPAD(uint32 buffer0, uint32 size0, uint32 buffer1, uint32 size1);
    uint32 StartPAD(void);
    uint32 ChangeClearPAD(uint32 mode);
    uint32 ReadInitPadFlag(void);
    uint32 xport_bios_init_card(uint32 mode);
    void psx_bios_bind_card_irq_initializer(void (*initializer)(void));
    uint32 xport_bios_start_card(void);
    uint32 VSyncCallbackPSX(uint32 guest_callback);
    uint32 ResetCallbackPSX(void);
    sint32 xport_bios_enter_critical(void);
    void xport_bios_exit_critical(void);
    void psx_bios_bind_card_patches(uint32 copy_source, uint32 first_source, uint32 second_source, uint32 first_destination, uint32 second_destination);
    void *firstfile(const char *pattern, void *record);
    uint32 psx_bios_card_write_guest(uint32 channel, uint32 sector, uint32 buffer);
    uint32 psx_bios_card_load_guest(uint32 channel);
    uint32 psx_bios_card_read_guest(uint32 channel, uint32 sector, uint32 buffer);
    void psx_bios_card_poll(void);
    void UnDeliverEvent(uint32 event_class, uint32 spec);

    typedef uint16 u_short;
    typedef uint8 u_char;
    typedef uint16 ushort;

    typedef struct PsxGteSnapshot PsxGteSnapshot;

#define PSX_DRAM_SIZE 0x200000u
#define PSX_CLOCK_EVENTS 32u

    typedef sint32 (*PsxBiosEventService)(void *context);

    typedef struct PsxBiosCardInfoLayout
    {
        uint32 requested, status, step, retry;
        uint32 channel, buffer, handler, sector, info_handler;
    } PsxBiosCardInfoLayout;

    typedef sint32 (*PsxClockEvent)(void *context, uint64 now);
    typedef sint32 (*PsxClockAdvance)(void *context, uint64 from, uint64 to);

    typedef struct PsxClockSlot
    {
        uint64 due;
        uint64 order;
        PsxClockEvent callback;
        void *context;
        uint32 active;
    } PsxClockSlot;

    typedef struct PsxClock
    {
        uint64 now;
        uint64 sequence;
        PsxClockSlot events[PSX_CLOCK_EVENTS];
        PsxClockAdvance advance;
        void *context;
        uint32 failed;
        uint32 running;
        uint32 advancing;
    } PsxClock;

    typedef sint32 (*PsxVBlankWait)(void *context);
    typedef sint32 (*PsxGuestCallback)(void *context, uint32 address);
    void psx_bios_bind_guest_callback_service(PsxGuestCallback dispatch, void *context);
    typedef sint32 (*PsxNativeExec)(void *context, uint32 entry, uint32 argc, uint32 argv);
    void psx_bios_bind_native_exec(PsxNativeExec dispatch, void *context);
    typedef sint32 (*PsxVsyncSample)(void *context, uint32 *counter, uint32 *gpu_status);

    typedef struct PsxIrqState
    {
        uint32 status;
        uint32 mask;
        uint32 lines;
    } PsxIrqState;

    typedef struct PsxDmaChannel
    {
        uint32 base, block, control, request;
    } PsxDmaChannel;

    typedef struct PsxDmaState
    {
        PsxDmaChannel channels[7];
        uint32 priority, interrupt, halt_ticks;
    } PsxDmaState;

    typedef sint32 (*PsxDmaTransfer)(void *context, uint32 channel);
    typedef sint32 (*PsxDmaHalt)(void *context, uint32 ticks);

    typedef struct PsxDmaScheduler
    {
        PsxClock *clock;
        PsxDmaState *dma;
        PsxIrqState *irq;
        PsxDmaTransfer transfer;
        void *context;
        uint64 scheduled_at;
        uint32 event_id, active, failed;
    } PsxDmaScheduler;

    typedef struct PsxCrtcState
    {
        uint32 pal;
        uint32 horizontal_total, horizontal_start, horizontal_end;
        uint32 vertical_total, vertical_start, vertical_end;
        uint32 dot_divider, dot_external;
        uint32 fractional_ticks, fractional_dot_ticks;
        uint32 scanline, scanline_tick;
    } PsxCrtcState;

    typedef struct PsxCrtcStep
    {
        uint32 dots, hblanks, vblanks;
        uint32 in_hblank, in_vblank, line_lsb;
    } PsxCrtcStep;

    typedef struct PsxVideoTiming
    {
        PsxCrtcState crtc;
        PsxIrqState irq;
        uint64 frame;
    } PsxVideoTiming;

    typedef struct PsxTimerState
    {
        uint32 counter[3];
        uint32 target[3];
        uint32 mode[3];
        uint32 div8_carry;
    } PsxTimerState;

    typedef struct
    {
        sint16 m[3][3];
        sint32 t[3];
    } MATRIX;

    struct PsxGteSnapshot
    {
        MATRIX rotation;
        MATRIX light;
        MATRIX color;
        sint32 translation[3];
        sint32 back_color[3];
        sint32 h;
        sint32 ofx;
        sint32 ofy;
        sint32 flag;
        sint32 sxy[3];
        uint32 sz[4];
        sint16 v0[3];
        sint32 mac[3];
        sint32 ir[3];
        sint32 far_color[3];
        sint32 mac0;
        sint32 ir0;
        sint16 v1[3];
        sint16 v2[3];
        uint32 rgbc;
        uint32 rgb[3];
        uint32 otz;
        sint32 zsf3;
        sint32 zsf4;
        sint32 dqa;
        sint32 dqb;
    };

#include "xport_trace.h"

#if defined(__cplusplus)
    extern "C"
    {
#endif
#if defined(LOCKSTEP_DEBUG)
        extern uint8 (*xport_lockstep_dram)[PSX_DRAM_SIZE];
        extern uint32 (*xport_lockstep_scratchpad)[256];
    #if defined(_MSC_VER)
        extern __declspec(dllexport) const volatile PsxGteSnapshot *xport_lockstep_gte_state;
    #else
        extern const volatile PsxGteSnapshot *xport_lockstep_gte_state;
    #endif
    #define DRAM (*xport_lockstep_dram)
    #define SCRATCHPAD (*xport_lockstep_scratchpad)
#else
extern uint32 SCRATCHPAD[256];
extern uint8 DRAM[PSX_DRAM_SIZE];
#endif

/* PsyQ LIBETC offsets are 32-bit words in the single shared scratchpad */
#define getScratchAddr(offset) (&SCRATCHPAD[(uint32)(offset)])
        void psx_gte_snapshot(PsxGteSnapshot *snapshot);
        sint32 psx_gte_import_control_registers(const uint32 registers[32]);
        sint32 psx_bios_bind_events(uint32 address, uint32 bytes);
        sint32 psx_bios_events_failed(void);
        uint32 SysDeqIntRP(uint32 priority, uint32 node);
        sint32 OpenEventGuest(uint32 event_class, uint32 specification, uint32 mode, uint32 guest_callback);
        typedef sint32 (*PsxEventCallback)(void);
        sint32 OpenEvent(uint32 event_class, sint32 specification, sint32 mode, PsxEventCallback callback);
#if !defined(XPORT_NO_PSYQ_CRITICAL_SECTION)
        sint32 EnterCriticalSectionPSX(void);
        void ExitCriticalSection(void);
#endif
        sint32 EnableEvent(sint32 event_handle);
        sint32 DisableEvent(sint32 event_handle);
        sint32 CloseEvent(sint32 event_handle);
        sint32 CdStatus(void);
        void *nextfile(void *record);
        sint32 TestEvent(sint32 event_handle);
        void DeliverEvent(uint32 event_class, uint32 spec);
        void psx_bios_bind_event_service(PsxBiosEventService service, void *context);
        sint32 psx_bios_bind_card_info(const PsxBiosCardInfoLayout *layout);
        sint32 psx_bios_card_info_failed(void);
        sint32 _card_info(sint32 channel);
        sint32 psx_bios_bind_card_read(uint32 read_handler, uint32 new_card_flag);
        sint32 _card_read(sint32 channel, sint32 sector, uint8 *buffer);
        void _new_card(void);
        void psx_clock_init(PsxClock *clock, uint64 now, PsxClockAdvance advance, void *context);
        sint32 psx_clock_schedule(PsxClock *clock, uint32 id, uint64 due, PsxClockEvent callback, void *context);
        sint32 psx_clock_cancel(PsxClock *clock, uint32 id);
        sint32 psx_clock_run(PsxClock *clock, uint64 until, uint32 event_budget);
        void psx_vblank_bind(PsxVBlankWait wait, void *context, uint32 initial_count);
        void psx_vblank_signal(void);
        void psx_native_poll_events(void);
        sint32 psx_vblank_failed(void);
        sint32 psx_vblank_bind_native_counter(uint32 counter);
        sint32 psx_vblank_bind_guest(uint32 counter, uint32 callbacks, PsxGuestCallback dispatch, void *context);
        sint32 psx_vsync_bind_sdk(uint32 last_counter, uint32 last_vblank, PsxVsyncSample sample, void *context, uint32 wait_budget);
        sint32 psx_vsync_resume_second_wait(uint32 target, uint32 entry_elapsed, uint32 saved_gpu_status);
        sint32 psx_irq_validate(const PsxIrqState *state);
        sint32 psx_irq_pending(const PsxIrqState *state);
        sint32 psx_irq_line(PsxIrqState *state, uint32 irq, uint32 high);
        sint32 psx_irq_write(PsxIrqState *state, uint32 offset, uint32 value);
        uint16 psx_irq_native_mask_exchange(uint16 mask);
        sint32 psx_irq_native_acknowledge(uint32 sources);
        sint32 psx_dma_validate(const PsxDmaState *state, const PsxIrqState *irq);
        sint32 psx_dma_write_interrupt(PsxDmaState *state, PsxIrqState *irq, uint32 value);
        sint32 psx_dma_complete(PsxDmaState *state, PsxIrqState *irq, uint32 channel);
        sint32 psx_dma_bus_error(PsxDmaState *state, PsxIrqState *irq, uint32 channel);
        sint32 psx_dma_trace_write(const PsxDmaState *state, uint32 channel, XportDeviceRegion source_region, uint32 source_offset, XportDeviceRegion destination_region, uint32 destination_offset, uint32 byte_count, uint32 byte_mask);
        sint32 psx_dma_ready(const PsxDmaState *state, uint32 channel, uint32 halted, uint32 ignore_halt);
        sint32 psx_dma_request(PsxDmaState *state, PsxIrqState *irq, uint32 channel, uint32 request, uint32 halted, PsxDmaTransfer transfer, void *context);
        sint32 psx_dma_write(PsxDmaState *state, PsxIrqState *irq, uint32 offset, uint32 value, uint32 halted, PsxDmaTransfer transfer, PsxDmaHalt halt, void *context);
        sint32 psx_dma_scheduler_init(PsxDmaScheduler *scheduler, PsxClock *clock, PsxDmaState *dma, PsxIrqState *irq, uint32 event_id, PsxDmaTransfer transfer, void *context);
        sint32 psx_dma_halt(PsxDmaScheduler *scheduler, uint32 ticks);
        sint32 psx_crtc_validate(const PsxCrtcState *state);
        sint32 psx_crtc_until_vblank(const PsxCrtcState *state, uint32 *cpu_ticks);
        sint32 psx_crtc_advance(PsxCrtcState *state, uint32 cpu_ticks, PsxCrtcStep *result);
        sint32 psx_video_timing_validate(const PsxVideoTiming *state);
        sint32 psx_video_timing_advance(PsxVideoTiming *state, uint32 cpu_ticks, PsxCrtcStep *result);
        sint32 psx_timers_import(const PsxTimerState *state);
        void psx_timers_export(PsxTimerState *state);
        sint32 psx_timers_advance(uint32 cpu_ticks, uint32 dot_ticks, uint32 hblank_ticks);
        sint32 SetRCnt(uint32 counter, uint16 target, sint32 mode);
        sint32 ResetRCnt(uint32 counter);
#if defined(__cplusplus)
    }
#endif

/* Public PsyQ-compatible ABI. Game code includes only this file; it has no
 * dependency on an installed PsyQ SDK or on this project's game headers. */
#define ONE 4096

#define MODE_NTSC 0
#define MODE_PAL 1

#define SPU_OFF 0
#define SPU_ON 1
#define SPU_OFF_ENV_ON 2
#define SPU_ON_ENV_OFF 3

#define SPU_VOICE_VOLL (0x01u << 0)
#define SPU_VOICE_VOLR (0x01u << 1)
#define SPU_VOICE_VOLMODEL (0x01u << 2)
#define SPU_VOICE_VOLMODER (0x01u << 3)
#define SPU_VOICE_PITCH (0x01u << 4)
#define SPU_VOICE_NOTE (0x01u << 5)
#define SPU_VOICE_SAMPLE_NOTE (0x01u << 6)
#define SPU_VOICE_WDSA (0x01u << 7)
#define SPU_VOICE_ADSR_AMODE (0x01u << 8)
#define SPU_VOICE_ADSR_SMODE (0x01u << 9)
#define SPU_VOICE_ADSR_RMODE (0x01u << 10)
#define SPU_VOICE_ADSR_AR (0x01u << 11)
#define SPU_VOICE_ADSR_DR (0x01u << 12)
#define SPU_VOICE_ADSR_SR (0x01u << 13)
#define SPU_VOICE_ADSR_RR (0x01u << 14)
#define SPU_VOICE_ADSR_SL (0x01u << 15)
#define SPU_VOICE_LSAX (0x01u << 16)
#define SPU_VOICE_ADSR_ADSR1 (0x01u << 17)
#define SPU_VOICE_ADSR_ADSR2 (0x01u << 18)

#define SPU_COMMON_MVOLL (0x01u << 0)
#define SPU_COMMON_MVOLR (0x01u << 1)
#define SPU_COMMON_MVOLMODEL (0x01u << 2)
#define SPU_COMMON_MVOLMODER (0x01u << 3)
#define SPU_COMMON_CDVOLL (0x01u << 6)
#define SPU_COMMON_CDVOLR (0x01u << 7)
#define SPU_COMMON_CDMIX (0x01u << 9)

#define SPU_REV_MODE (0x01u << 0)
#define SPU_REV_DEPTHL (0x01u << 1)
#define SPU_REV_DEPTHR (0x01u << 2)
#define SPU_REV_DELAYTIME (0x01u << 3)
#define SPU_REV_FEEDBACK (0x01u << 4)

#define SPU_ALLCH 0x00ffffffu
#define SPU_KEYCH(voice) (1u << (voice))

#define SS_TICK60 1
#define SS_SERIAL_A 0
#define SS_SERIAL_B 1
#define SS_WAIT_COMPLETED 1

#define CdlModeRept 0x04
#define CdlModeDA 0x01
#define CdlPlay 0x03
#define CdlStop 0x08
#define CdlPause 0x09
#define CdlMute 0x0b
#define CdlDemute 0x0c
#define CdlSetmode 0x0e
#define CdlSeekL 0x15

#define CdlNoIntr 0x00
#define CdlDataReady 0x01
#define CdlComplete 0x02
#define CdlAcknowledge 0x03
#define CdlDataEnd 0x04
#define CdlDiskError 0x05

    typedef void (*CdlCB)(uint8 status, uint8 *result);

    typedef struct
    {
        uint8 minute;
        uint8 second;
        uint8 sector;
        uint8 track;
    } CdlLOC;

    typedef struct CdlFILE
    {
        CdlLOC pos;
        uint32 size;
        char name[16];
    } CdlFILE;

    CdlFILE *CdSearchFile(CdlFILE *result, const char *path);

    typedef struct
    {
        uint8 val0;
        uint8 val1;
        uint8 val2;
        uint8 val3;
    } CdlATV;

    typedef struct
    {
        sint32 vx;
        sint32 vy;
        sint32 vz;
        sint32 pad;
    } VECTOR;

    typedef struct
    {
        sint16 vx;
        sint16 vy;
        sint16 vz;
        sint16 pad;
    } SVECTOR;

    typedef struct
    {
        uint8 r;
        uint8 g;
        uint8 b;
        uint8 cd;
    } CVECTOR;

    typedef struct
    {
        sint16 left;
        sint16 right;
    } SpuVolume;

    typedef struct
    {
        uint32 voice;
        uint32 mask;
        SpuVolume volume;
        SpuVolume volmode;
        SpuVolume volumex;
        uint16 pitch;
        uint16 note;
        uint16 sample_note;
        sint16 envx;
        uint32 addr;
        uint32 loop_addr;
        sint32 a_mode;
        sint32 s_mode;
        sint32 r_mode;
        uint16 ar;
        uint16 dr;
        uint16 sr;
        uint16 rr;
        uint16 sl;
        uint16 adsr1;
        uint16 adsr2;
    } SpuVoiceAttr;

    typedef struct
    {
        SpuVolume volume;
        sint32 reverb;
        sint32 mix;
    } SpuExtAttr;

    typedef struct
    {
        uint32 mask;
        SpuVolume mvol;
        SpuVolume mvolmode;
        SpuVolume mvolx;
        SpuExtAttr cd;
        SpuExtAttr ext;
    } SpuCommonAttr;

    typedef struct
    {
        uint32 mask;
        sint32 mode;
        SpuVolume depth;
        sint32 delay;
        sint32 feedback;
    } SpuReverbAttr;

    typedef struct
    {
        sint16 vx;
        sint16 vy;
    } DVECTOR;

    struct PSX_RECT
    {
        sint16 x;
        sint16 y;
        sint16 w;
        sint16 h;
    };

    typedef struct
    {
        SVECTOR v;
        uint8 uv[2];
        uint16 pad;
        CVECTOR c;
        DVECTOR sxy;
        uint32 sz;
    } RVECTOR;

    typedef struct
    {
        RVECTOR r01;
        RVECTOR r12;
        RVECTOR r20;
        RVECTOR *r0;
        RVECTOR *r1;
        RVECTOR *r2;
        uint32 *rtn;
    } CRVECTOR3;

    typedef struct
    {
        RVECTOR r01;
        RVECTOR r02;
        RVECTOR r31;
        RVECTOR r32;
        RVECTOR rc;
        RVECTOR *r0;
        RVECTOR *r1;
        RVECTOR *r2;
        RVECTOR *r3;
        uint32 *rtn;
    } CRVECTOR4;

    typedef struct
    {
        uint32 tag;
        uint32 code[15];
    } DR_ENV;

    typedef struct
    {
        PSX_RECT clip;
        sint16 ofs[2];
        PSX_RECT tw;
        uint16 tpage;
        uint8 dtd;
        uint8 dfe;
        uint8 isbg;
        uint8 r0;
        uint8 g0;
        uint8 b0;
        DR_ENV dr_env;
    } DRAWENV;

    typedef struct
    {
        PSX_RECT disp;
        PSX_RECT screen;
        uint8 isinter;
        uint8 isrgb24;
        uint8 pad0;
        uint8 pad1;
    } DISPENV;

    typedef union
    {
        uint32 rgbc;

        struct
        {
            uint8 r, g, b, code;
        };
    } RGBC;

    typedef struct
    {
        uint32 addr : 24;
        uint32 len : 8;
        RGBC rgbc;
    } P_TAG;

    // Packed GPU coordinate word
    typedef union
    {
        uint32 xy;

        struct
        {
            sint16 x, y;
        };
    } XY;

    typedef struct
    {
        RGBC rgbc;
        XY xy;
    } POINT_C;

    typedef union
    {
        uint16 uv;

        struct
        {
            uint8 u, v;
        };
    } UV;

    typedef struct
    {
        XY xy;
        UV uv;
        uint16 attr;
    } POINT_T;

    typedef struct
    {
        RGBC rgbc;
        XY xy;
        UV uv;
        uint16 attr;
    } POINT_CT;

    typedef struct
    {
        uint32 tag;
        RGBC rgbc;
        XY p[3];
    } POLY_F3;

    typedef struct
    {
        uint32 tag;
        RGBC rgbc;
        XY p[4];
    } POLY_F4;

    typedef struct
    {
        uint32 tag;
        RGBC rgbc;
        POINT_T p[3];
    } POLY_FT3;

    typedef struct
    {
        uint32 tag;
        RGBC rgbc;
        POINT_T p[4];
    } POLY_FT4;

    typedef struct
    {
        uint32 tag;
        POINT_C p[3];
    } POLY_G3;

    typedef struct
    {
        uint32 tag;
        POINT_C p[4];
    } POLY_G4;

    typedef struct
    {
        uint32 tag;
        RGBC rgbc;
        XY p[2];
    } LINE_F2;

    typedef struct
    {
        uint32 tag;
        POINT_C p[2];
    } LINE_G2;

    typedef struct
    {
        uint32 tag;
        RGBC rgbc;
        XY p[3];
        uint32 pad;
    } LINE_F3;

    typedef struct
    {
        uint32 tag;
        POINT_C p[3];
        uint32 pad;
    } LINE_G3;

    typedef struct
    {
        uint32 tag;
        RGBC rgbc;
        XY p[4];
        uint32 pad;
    } LINE_F4;

    typedef struct
    {
        uint32 tag;
        POINT_C p[4];
        uint32 pad;
    } LINE_G4;

    typedef struct
    {
        uint32 tag;
        RGBC rgbc;
        XY p[5];
        uint32 pad;
    } LINE_F5;

    typedef struct
    {
        uint32 tag;
        POINT_C p[5];
        uint32 pad;
    } LINE_G5;

    typedef struct
    {
        uint32 tag;
        POINT_CT p[3];
    } POLY_GT3;

    typedef struct
    {
        uint32 tag;
        POINT_CT p[4];
    } POLY_GT4;

    typedef struct
    {
        uint32 tag;
        RGBC rgbc;
        POINT_T p[1];
        sint16 w, h;
    } SPRT;

    typedef struct
    {
        uint32 tag;
        RGBC rgbc;
        POINT_T p[1];
    } SPRT_16;

    typedef SPRT_16 SPRT_8;

    typedef struct
    {
        uint32 tag;
        RGBC rgbc;
        XY p[1];
        sint16 w, h;
    } TILE;

    typedef struct
    {
        uint32 tag;
        RGBC rgbc;
        XY p[1];
    } TILE_16;

    typedef TILE_16 TILE_8;
    typedef TILE_16 TILE_1;

    typedef struct
    {
        uint32 tag;
        uint32 code[2];
    } DR_MODE;

    typedef DR_MODE DR_TWIN;
    typedef DR_MODE DR_AREA;
    typedef DR_MODE DR_OFFSET;

    typedef struct
    {
        uint32 tag;
        uint32 code[5];
    } DR_MOVE;

    typedef struct
    {
        uint32 tag;
        uint32 code[3];
        uint32 p[13];
    } DR_LOAD;

    typedef struct
    {
        uint32 tag;
        uint32 code[1];
    } DR_TPAGE;

    typedef DR_MODE DR_STP;

#define setVector(vector, _x, _y, _z) (xport_store_le16(&(vector)->vx, (uint16)(_x)), xport_store_le16(&(vector)->vy, (uint16)(_y)), xport_store_le16(&(vector)->vz, (uint16)(_z)))
#define setRECT(rect, _x, _y, width, height) (xport_store_le16(&(rect)->x, (uint16)(_x)), xport_store_le16(&(rect)->y, (uint16)(_y)), xport_store_le16(&(rect)->w, (uint16)(width)), xport_store_le16(&(rect)->h, (uint16)(height)))
#define setRGB0(packet, red, green, blue) (xport_store_u8((uint8 *)(packet) + 4u, (red)), xport_store_u8((uint8 *)(packet) + 4u + 1u, (green)), xport_store_u8((uint8 *)(packet) + 4u + 2u, (blue)))
#define setRGB1(packet, red, green, blue) (xport_store_u8(&(packet)->p[1].rgbc.r, (red)), xport_store_u8(&(packet)->p[1].rgbc.g, (green)), xport_store_u8(&(packet)->p[1].rgbc.b, (blue)))
#define setRGB2(packet, red, green, blue) (xport_store_u8(&(packet)->p[2].rgbc.r, (red)), xport_store_u8(&(packet)->p[2].rgbc.g, (green)), xport_store_u8(&(packet)->p[2].rgbc.b, (blue)))
#define setRGB3(packet, red, green, blue) (xport_store_u8(&(packet)->p[3].rgbc.r, (red)), xport_store_u8(&(packet)->p[3].rgbc.g, (green)), xport_store_u8(&(packet)->p[3].rgbc.b, (blue)))
#define setXY0(packet, _x0, _y0) (xport_store_le16((uint8 *)(packet) + 8u, (uint16)(_x0)), xport_store_le16((uint8 *)(packet) + 8u + 2u, (uint16)(_y0)))
#define setXY2(packet, _x0, _y0, _x1, _y1) (xport_store_le16((uint8 *)(packet) + 8u, (uint16)(_x0)), xport_store_le16((uint8 *)(packet) + 8u + 2u, (uint16)(_y0)), xport_store_le16((uint8 *)(packet) + (8u + 1u * sizeof((packet)->p[0])), (uint16)(_x1)), xport_store_le16((uint8 *)(packet) + (8u + 1u * sizeof((packet)->p[0])) + 2u, (uint16)(_y1)))
#define setXY3(packet, _x0, _y0, _x1, _y1, _x2, _y2) (xport_store_le16((uint8 *)(packet) + 8u, (uint16)(_x0)), xport_store_le16((uint8 *)(packet) + 8u + 2u, (uint16)(_y0)), xport_store_le16((uint8 *)(packet) + (8u + 1u * sizeof((packet)->p[0])), (uint16)(_x1)), xport_store_le16((uint8 *)(packet) + (8u + 1u * sizeof((packet)->p[0])) + 2u, (uint16)(_y1)), xport_store_le16((uint8 *)(packet) + (8u + 2u * sizeof((packet)->p[0])), (uint16)(_x2)), xport_store_le16((uint8 *)(packet) + (8u + 2u * sizeof((packet)->p[0])) + 2u, (uint16)(_y2)))
#define setXY4(packet, _x0, _y0, _x1, _y1, _x2, _y2, _x3, _y3) (xport_store_le16((uint8 *)(packet) + 8u, (uint16)(_x0)), xport_store_le16((uint8 *)(packet) + 8u + 2u, (uint16)(_y0)), xport_store_le16((uint8 *)(packet) + (8u + 1u * sizeof((packet)->p[0])), (uint16)(_x1)), xport_store_le16((uint8 *)(packet) + (8u + 1u * sizeof((packet)->p[0])) + 2u, (uint16)(_y1)), xport_store_le16((uint8 *)(packet) + (8u + 2u * sizeof((packet)->p[0])), (uint16)(_x2)), xport_store_le16((uint8 *)(packet) + (8u + 2u * sizeof((packet)->p[0])) + 2u, (uint16)(_y2)), xport_store_le16((uint8 *)(packet) + (8u + 3u * sizeof((packet)->p[0])), (uint16)(_x3)), xport_store_le16((uint8 *)(packet) + (8u + 3u * sizeof((packet)->p[0])) + 2u, (uint16)(_y3)))
#define setWH(packet, width, height) (xport_store_le16(&(packet)->w, (uint16)(width)), xport_store_le16(&(packet)->h, (uint16)(height)))
#define setUV0(packet, _u0, _v0) (xport_store_u8(&(packet)->p[0].uv.u, (_u0)), xport_store_u8(&(packet)->p[0].uv.v, (_v0)))
#define setUV3(packet, _u0, _v0, _u1, _v1, _u2, _v2) (xport_store_u8(&(packet)->p[0].uv.u, (_u0)), xport_store_u8(&(packet)->p[0].uv.v, (_v0)), xport_store_u8(&(packet)->p[1].uv.u, (_u1)), xport_store_u8(&(packet)->p[1].uv.v, (_v1)), xport_store_u8(&(packet)->p[2].uv.u, (_u2)), xport_store_u8(&(packet)->p[2].uv.v, (_v2)))
#define setUV4(packet, _u0, _v0, _u1, _v1, _u2, _v2, _u3, _v3) (xport_store_u8(&(packet)->p[0].uv.u, (_u0)), xport_store_u8(&(packet)->p[0].uv.v, (_v0)), xport_store_u8(&(packet)->p[1].uv.u, (_u1)), xport_store_u8(&(packet)->p[1].uv.v, (_v1)), xport_store_u8(&(packet)->p[2].uv.u, (_u2)), xport_store_u8(&(packet)->p[2].uv.v, (_v2)), xport_store_u8(&(packet)->p[3].uv.u, (_u3)), xport_store_u8(&(packet)->p[3].uv.v, (_v3)))
#define setlen(packet, length) xport_store_u8((uint8 *)(packet) + 3u, (length))
#define setaddr(packet, address) xport_store_le32((packet), (xport_load_le32(packet) & 0xff000000u) | ((uint32)(intptr)(address) & 0x00ffffffu))
#define setcode(packet, command) xport_store_u8(&((P_TAG *)(packet))->rgbc.code, (command))
#define getlen(packet) xport_load_u8((const uint8 *)(packet) + 3u)
#define getaddr(packet) (xport_load_le32(packet) & 0x00ffffffu)
#define getcode(packet) xport_load_u8(&((P_TAG *)(packet))->rgbc.code)
#define nextPrim(packet) ((void *)(intptr)(getaddr(packet) | 0x80000000UL))
#define isendprim(packet) (getaddr(packet) == 0xffffffUL)
#define addPrim(ot, packet) (setaddr(packet, getaddr(ot)), setaddr(ot, packet))
#define addPrims(ot, first, last) (setaddr(last, getaddr(ot)), setaddr(ot, first))
#define termPrim(packet) setaddr(packet, 0xffffffffUL)
#define setSemiTrans(packet, enabled) ((enabled) ? setcode(packet, getcode(packet) | 2) : setcode(packet, getcode(packet) & ~2))
#define setShadeTex(packet, raw_texture) ((raw_texture) ? setcode(packet, getcode(packet) | 1) : setcode(packet, getcode(packet) & ~1))
#define getTPage(depth, abr, x, y) ((((depth) & 3) << 7) | (((abr) & 3) << 5) | (((y) & 0x100) >> 4) | (((x) & 0x3ff) >> 6) | (((y) & 0x200) << 2))
#define getClut(x, y) (((y) << 6) | (((x) >> 4) & 0x3f))
#define setTPage(packet, depth, abr, x, y) xport_store_le16(&(packet)->p[1].attr, getTPage(depth, abr, x, y))
#define setClut(packet, x, y) xport_store_le16(&(packet)->p[0].attr, getClut(x, y))
#define psx_draw_mode(dfe, dtd, tpage) (0xe1000000UL | ((dtd) ? 0x0200UL : 0) | ((dfe) ? 0x0400UL : 0) | ((tpage) & 0x9ff))
#define psx_texture_window(texture_window) ((texture_window) ? (0xe2000000UL | (((texture_window)->y & 0xff) >> 3 << 15) | (((texture_window)->x & 0xff) >> 3 << 10) | ((~((texture_window)->h - 1) & 0xff) >> 3 << 5) | ((~((texture_window)->w - 1) & 0xff) >> 3)) : 0)
#define setDrawMode1(packet, dfe, dtd, tpage) (setlen(packet, 1), xport_store_le32((uint32 *)(packet) + 1, psx_draw_mode(dfe, dtd, tpage)))
#define setDrawMode(packet, dfe, dtd, tpage, texture_window) (setlen(packet, 2), xport_store_le32((uint32 *)(packet) + 1, psx_draw_mode(dfe, dtd, tpage)), xport_store_le32((uint32 *)(packet) + 2, psx_texture_window((PSX_RECT *)(texture_window))))
#define setTexWindow(packet, texture_window) (setlen(packet, 2), xport_store_le32((uint32 *)(packet) + 1, psx_texture_window((PSX_RECT *)(texture_window))), xport_store_le32((uint32 *)(packet) + 2, 0))
#define setLineF2(line) (setlen(line, 3), setcode(line, 0x40))
#define setLineG2(line) (setlen(line, 4), setcode(line, 0x50))
#define setLineF3(line) (setlen(line, 5), setcode(line, 0x48), (line)->pad = 0x55555555u)
#define setLineG3(line) (setlen(line, 7), setcode(line, 0x58), (line)->pad = 0x55555555u, (line)->p[2].rgbc.code = 0)
#define setLineF4(line) (setlen(line, 6), setcode(line, 0x4c), (line)->pad = 0x55555555u)
#define setLineG4(line) (setlen(line, 9), setcode(line, 0x5c), (line)->pad = 0x55555555u, (line)->p[2].rgbc.code = 0, (line)->p[3].rgbc.code = 0)
#define setLineF5(line) (setlen(line, 7), setcode(line, 0x4c), (line)->pad = 0x55555555u)
#define setLineG5(line) (setlen(line, 11), setcode(line, 0x5c), (line)->pad = 0x55555555u, (line)->p[2].rgbc.code = 0, (line)->p[3].rgbc.code = 0, (line)->p[4].rgbc.code = 0)

#define setPolyF3(packet) (setlen(packet, 4), setcode(packet, 0x20))
#define setPolyFT3(packet) (setlen(packet, 7), setcode(packet, 0x24))
#define setPolyG3(packet) (setlen(packet, 6), setcode(packet, 0x30))
#define setPolyGT3(packet) (setlen(packet, 9), setcode(packet, 0x34))
#define setPolyF4(packet) (setlen(packet, 5), setcode(packet, 0x28))
#define setPolyFT4(packet) (setlen(packet, 9), setcode(packet, 0x2c))
#define setPolyG4(packet) (setlen(packet, 8), setcode(packet, 0x38))
#define setPolyGT4(packet) (setlen(packet, 12), setcode(packet, 0x3c))
#define setSprt8(packet) (setlen(packet, 3), setcode(packet, 0x74))
#define setSprt16(packet) (setlen(packet, 3), setcode(packet, 0x7c))
#define setSprt(packet) (setlen(packet, 4), setcode(packet, 0x64))
#define setTile1(packet) (setlen(packet, 2), setcode(packet, 0x68))
#define setTile8(packet) (setlen(packet, 2), setcode(packet, 0x70))
#define setTile16(packet) (setlen(packet, 2), setcode(packet, 0x78))
#define setTile(packet) (setlen(packet, 3), setcode(packet, 0x60))

/* PsyQ declares these primitive initializers as macros. Keep the familiar
 * uppercase spellings used by reconstructed game code without adding wrapper
 * functions or another packet-initialization semantic. */
#define SetDrawMode(packet, dfe, dtd, tpage, texture_window) setDrawMode(packet, dfe, dtd, tpage, texture_window)
#define SetDrawMode1(packet, dfe, dtd, tpage) setDrawMode1(packet, dfe, dtd, tpage)
#define SetPolyF3(packet) setPolyF3(packet)
#define SetPolyF4(packet) setPolyF4(packet)
#define SetPolyFT3(packet) setPolyFT3(packet)
#define SetPolyFT4(packet) setPolyFT4(packet)
#define SetPolyG3(packet) setPolyG3(packet)
#define SetPolyG4(packet) setPolyG4(packet)
#define SetPolyGT3(packet) setPolyGT3(packet)
#define SetPolyGT4(packet) setPolyGT4(packet)
#define SetSemiTrans(packet, enabled) setSemiTrans(packet, enabled)
#define SetShadeTex(packet, raw_texture) setShadeTex(packet, raw_texture)
#define SetSprt(packet) setSprt(packet)
#define SetSprt16(packet) setSprt16(packet)
#define SetSprt8(packet) setSprt8(packet)
#define SetTexWindow(packet, texture_window) setTexWindow(packet, texture_window)
#define SetTile(packet) setTile(packet)
#define SetTile1(packet) setTile1(packet)
#define SetTile16(packet) setTile16(packet)
#define SetTile8(packet) setTile8(packet)

#if defined(__cplusplus)
    extern "C"
    {
#endif

        void AddPrim(void *ot, void *prim);
        void AddPrims(void *ot, void *first, void *last);
        VECTOR *ApplyMatrix(MATRIX *matrix, SVECTOR *input, VECTOR *output);
        VECTOR *ApplyMatrixLV(MATRIX *matrix, VECTOR *input, VECTOR *output);
        VECTOR *ApplyRotMatrix(SVECTOR *input, VECTOR *output);
        SVECTOR *ApplyMatrixSV(MATRIX *matrix, SVECTOR *input, SVECTOR *output);
        sint32 AverageZ3(sint32 z0, sint32 z1, sint32 z2);
        sint32 AverageZ4(sint32 z0, sint32 z1, sint32 z2, sint32 z3);
        sint32 ClearImage(PSX_RECT *rectangle, uint8 red, uint8 green, uint8 blue);
        uint32 *ClearOTag(uint32 *ot, sint32 count);
        uint32 ClearOTagPSX(uint32 ot, uint32 count, uint32 header_address, uint32 header_target, uint32 debug_flag_address, uint32 debug_callback_slot, uint32 debug_format_address);
        uint32 SetDrawMovePSX(uint32 destination, uint32 rectangle, uint32 x, uint32 y);
        void SetDrawMove(DR_MOVE *packet, PSX_RECT *rectangle, sint32 x, sint32 y);
        uint32 xport_guest_call3(uint32 target, uint32 argument0, uint32 argument1, uint32 argument2);
        uint32 xport_guest_call_known_registers(uint32 target, uint32 *registers, uint32 known_register_mask);
        uint32 *ClearOTagR(uint32 *ot, sint32 count);
        sint32 ccos(sint32 a);
        sint32 csin(sint32 a);
        sint32 rcos(sint32 a);
        sint32 rsin(sint32 a);
        uint32 SquareRoot0(sint32 input);
        uint32 SquareRoot0PSX(uint32 input, uint32 table_address);

        typedef struct
        {
            sint32 vx, vy, vz;
            uint8 r, g, b, pad;
        } GsF_LIGHT;

        void gte_bind_gs_light_matrices(MATRIX *directions, MATRIX *colors);
        sint32 GsSetFlatLight(sint32 index, const GsF_LIGHT *light);
        uint32 GsSortOtPSX(uint32 source_ot, uint32 destination_ot);
        void rsincos(sint32 angle, sint32 *sine, sint32 *cosine);
        sint32 ratan2(sint32 y, sint32 x);
        sint32 DrawSync(sint32 mode);
        void DrawPrim(void *primitive);
        void DrawOTag(uint32 *ot);
        uint32 *ClearOTagRNativePSX(uint32 *ot, sint32 count, uint32 header_address, uint32 header_target);
        uint32 *ClearOTagRPSX(uint32 *ot, sint32 count, uint32 header_address, uint32 header_target, uint32 guest_ot);
        uint32 DrawOTagPSX(uint32 guest_ot);
        uint32 DrawPrimPSX(uint32 primitive, uint32 driver_slot, uint32 guest_gp, uint32 guest_entry);
        uint32 xport_gpu_guest_dispatch(uint32 target, uint32 *registers, uint32 known_register_mask);
        uint32 SYS_OBJ_172CPSX(uint32 x, uint32 y, uint32 extent);
        uint32 SYS_OBJ_17C4PSX(uint32 x, uint32 y, uint32 extent);
        uint32 SYS_OBJ_185CPSX(uint32 x, uint32 y);
        uint32 SYS_OBJ_170CPSX(uint32 dfe, uint32 dtd, uint32 tpage);
        uint32 SYS_OBJ_1878PSX(uint32 rectangle);
        uint32 SYS_OBJ_149CPSX(uint32 packet, uint32 environment, uint32 extent);
        uint32 PutDrawEnvPSX(uint32 environment, uint32 state, uint32 debug_format, uint32 guest_gp, uint32 guest_entry, uint32 (*copy_guest)(uint32 destination, uint32 source, uint32 bytes));
        uint32 xport_sdk_texture_frame_acquire(size_t bytes);
        void xport_sdk_texture_frame_release(uint32 address, size_t bytes);
        uint32 xport_sdk_draw_env_frame_acquire(size_t bytes);
        void xport_sdk_draw_env_frame_release(uint32 address, size_t bytes);

        MATRIX *CompMatrixLV(MATRIX *left, MATRIX *right, MATRIX *destination);
        uint32 xport_gpu_draw_otag_guest_result(uint32 guest_ot);
        void xport_gpu_clear_otag_r_guest_driver(uint32 guest_ot, uint32 count);
        uint32 xport_guest_frame_acquire(size_t bytes);
        void xport_guest_frame_release(uint32 address, size_t bytes);
        void FlushCache(void);
        uint16 GetClut(sint32 x, sint32 y);
        sint32 GetRCnt(uint32 counter);
        uint16 GetTPage(sint32 depth, sint32 abr, sint32 x, sint32 y);
        sint32 GetGraphType(void);
        void FntLoad(sint32 x, sint32 y);
        sint32 FntOpen(sint32 x, sint32 y, sint32 w, sint32 h, sint32 bg, sint32 n);
        void SetDumpFnt(sint32 id);
        sint32 FntPrint(const char *fmt, ...);
        uint32 *FntFlush(sint32 id);
        sint32 GetODE(void);
        void InitGeom(void);
        sint32 Lzc(sint32 value);
        void xport_gte_write_data(uint32 register_index, uint32 value);
        void xport_gte_mvmva(uint32 command);
        void xport_gte_write_control(uint32 index, uint32 value);
        uint32 xport_gte_read_data(uint32 register_index);
        uint32 xport_gte_read_flag(void);
        uint32 xport_gte_read_screen_offset(uint32 axis);
        uint32 VectorNormalSS(SVECTOR *source, SVECTOR *destination);
        void xport_gte_execute(uint32 command);
        void xport_gte_write_unavailable_data(uint32 register_index, uint32 value);
        void xport_gte_complete_rtpt(void);
        uint32 xport_guest_call_known_registers_post_sdk(uint32 target, uint32 *registers, uint32 known_register_mask);
        uint32 xport_guest_nonlocal_transfer(uint32 owner_entry, uint32 source_pc, uint32 target, uint32 known_gpr_mask, const uint32 *known_gpr_values);
        sint32 LoadImagePSX(PSX_RECT *rectangle, uint32 *pixels);
        uint16 LoadClut(uint32 *pixels, sint32 x, sint32 y);
        uint16 LoadTPage(uint32 *pixels, sint32 depth, sint32 abr, sint32 x, sint32 y, sint32 width, sint32 height);
        sint32 MoveImage(PSX_RECT *rectangle, sint32 x, sint32 y);
        sint32 NormalClip(sint32 point0, sint32 point1, sint32 point2);
        VECTOR *OuterProduct0(VECTOR *left, VECTOR *right, VECTOR *output);
        VECTOR *OuterProduct12(VECTOR *left, VECTOR *right, VECTOR *output);
        VECTOR *Square0(VECTOR *input, VECTOR *output);
        VECTOR *Square12(VECTOR *input, VECTOR *output);
        uint32 PadRead(sint32 controller);
        void PadInit(sint32 mode);
        sint32 PadGetState(sint32 port);
        uint32 PadGetStatePSX(uint32 port);
        void PadSetActPSX(uint32 port, uint32 actuator, uint32 length);
        uint32 PadSetActAlignPSX(uint32 port, uint32 alignment);
        sint32 PadInfoMode(sint32 port, sint32 term, sint32 offset);
        void PadInitDirect(uint8 *pad1, uint8 *pad2);
        uint32 PadInitDirectPSX(uint32 first_packet, uint32 second_packet);
        uint32 PadStartComPSX(void);
        void PadSetAct(sint32 port, uint8 *actuator, sint32 length);
        sint32 PadSetActAlign(sint32 port, uint8 *alignment);
        sint32 PadSetMainMode(sint32 port, sint32 mode, sint32 lock);
        void PadStartCom(void);
        void PadStopCom(void);
        uint32 setRC2wait(uint32 wait);
        void PopMatrix(void);
        void PushMatrix(void);
        sint32 psx_gte_bind_matrix_stack(uint32 depth_address, uint32 base_address, uint32 capacity);
        void ReadGeomOffset(sint32 *x, sint32 *y);
        sint32 ReadGeomScreen(void);
        void ReadRotMatrix(MATRIX *matrix);
        sint32 ResetGraph(sint32 mode);
        sint32 psx_pc_set_root(const char *root, sint32 uppercase);
        sint32 PCinit(void);
        sint32 PCopen(char *name, sint32 flags, sint32 permissions);
        sint32 PCcreat(char *name, sint32 permissions);
        sint32 PCclose(sint32 handle);
        sint32 PClseek(sint32 handle, sint32 offset, sint32 origin);
        sint32 PCread(sint32 handle, char *buffer, sint32 count);
        sint32 PCwrite(sint32 handle, char *buffer, sint32 count);

        typedef struct DIRENTRY
        {
            char name[20];
            sint32 attr, size;
            struct DIRENTRY *next;
            sint32 head;
            char system[4];
        } DIRENTRY;

        typedef void (*MemCB)(sint32 command, sint32 result);
        sint32 psx_memcard_mount(sint32 channel, uint8 *image, size_t bytes);
        void MemCardInit(sint32 mode);
        void MemCardStart(void);
        void MemCardStop(void);
        void MemCardEnd(void);
        sint32 MemCardExist(sint32 channel);
        sint32 MemCardAccept(sint32 channel);
        sint32 MemCardSync(sint32 mode, sint32 *command, sint32 *result);
        MemCB MemCardCallback(MemCB callback);
        sint32 MemCardReadFile(sint32 channel, char *name, uint32 *buffer, sint32 offset, sint32 bytes);
        sint32 MemCardWriteFile(sint32 channel, char *name, uint32 *buffer, sint32 offset, sint32 bytes);
        sint32 MemCardFormat(sint32 channel);
        sint32 MemCardCreateFile(sint32 channel, char *name, sint32 blocks);
        sint32 MemCardDeleteFile(sint32 channel, char *name);
        sint32 MemCardGetDirentry(sint32 channel, char *pattern, DIRENTRY *directory, sint32 *files, sint32 offset, sint32 maximum);
        sint32 ResetCallback(void);
        typedef void (*PsxInterruptCallback)(void);
        PsxInterruptCallback InterruptCallback(sint32 interrupt, PsxInterruptCallback callback);
        void psx_interrupt_raise(uint32 interrupt);
        MATRIX *RotMatrix(SVECTOR *rotation, MATRIX *matrix);
        MATRIX *RotMatrixX(sint32 angle, MATRIX *matrix);
        MATRIX *RotMatrixY(sint32 angle, MATRIX *matrix);
        MATRIX *RotMatrixZ(sint32 rotation, MATRIX *matrix);
        MATRIX *RotMatrixYXZ(SVECTOR *rotation, MATRIX *matrix);
        MATRIX *MulMatrix(MATRIX *left, MATRIX *right);
        MATRIX *MulMatrix0(MATRIX *left, MATRIX *right, MATRIX *destination);
        MATRIX *MulMatrix2(MATRIX *source, MATRIX *destination);
        void RotTrans(SVECTOR *input, VECTOR *output, sint32 *flags);
        sint32 RotTransPers(SVECTOR *input, sint32 *screen_xy, sint32 *projection, sint32 *flags);
        void RotTransSV(SVECTOR *input, SVECTOR *output, sint32 *flags);
        MATRIX *ScaleMatrix(MATRIX *matrix, VECTOR *scale);
        void SetDispMask(sint32 enabled);
        DISPENV *SetDefDispEnv(DISPENV *environment, sint32 x, sint32 y, sint32 width, sint32 height);
        DRAWENV *SetDefDrawEnv(DRAWENV *environment, sint32 x, sint32 y, sint32 width, sint32 height);
        void SetDrawArea(DR_AREA *packet, PSX_RECT *rectangle);
        void SetDrawOffset(void *packet, const sint16 *offset);
        void SetDrawEnv(DR_ENV *packet, DRAWENV *environment);
        void SetDrawStp(DR_STP *packet, sint32 enabled);
        void SetFarColor(sint32 red, sint32 green, sint32 blue);
        void SetFogNear(sint32 distance, sint32 projection);
        uint32 SetFogNearFar(sint32 near_distance, sint32 far_distance, sint32 projection);
        void SetGeomOffset(sint32 x, sint32 y);
        void SetGeomScreen(sint32 distance);
        sint32 SetGraphDebug(sint32 level);
        void SetBackColor(sint32 red, sint32 green, sint32 blue);
        void gte_set_back_color_raw(sint32 red, sint32 green, sint32 blue);
        void gte_set_far_color_raw(sint32 red, sint32 green, sint32 blue);
        void gte_write_ir0(sint32 value);
        void gte_intpl12(const SVECTOR *input, const SVECTOR *far_color, sint32 factor, VECTOR *output);
        void gte_gpf12(const SVECTOR *input, sint32 scale, VECTOR *output);
        void gte_gpf0(const SVECTOR *input, sint32 scale, VECTOR *output);
        void gte_gpl12(const SVECTOR *input, sint32 scale, VECTOR *output);
        void gte_gpl0(const SVECTOR *input, sint32 scale, VECTOR *output);
        uint32 LoadAverageShort12(SVECTOR *first, SVECTOR *second, sint32 first_weight, sint32 second_weight, SVECTOR *output);
        uint32 LoadAverageByte(uint8 *first, uint8 *second, sint32 first_weight, sint32 second_weight, uint8 *output);
        uint32 LoadAverageCol(uint8 *first, uint8 *second, sint32 first_weight, sint32 second_weight, uint8 *output);
        void SetColorMatrix(MATRIX *matrix);
        void SetLightMatrix(MATRIX *matrix);
        void SetRotMatrix(MATRIX *matrix);
        void SetTransMatrix(MATRIX *matrix);
        uint32 gte_normal_color_col(const SVECTOR *normal, uint32 color);
        uint32 gte_normal_color_col_depth(const SVECTOR *normal, uint32 color);
        void gte_normal_color_col3(const SVECTOR normals[3], uint32 color, uint32 output[3]);
        void gte_transform_light(const SVECTOR *input, VECTOR *output, sint32 *flags);
        sint32 StoreImage(PSX_RECT *rectangle, uint32 *pixels);
        MATRIX *TransposeMatrix(MATRIX *source, MATRIX *destination);
        DISPENV *PutDispEnv(DISPENV *environment);
        uint32 PutDispEnvPSX(uint32 environment, uint32 debug_level_address, uint32 debug_callback_address, uint32 debug_format_address, uint32 driver_table_address, uint32 cache_address, uint32 reverse_flag_address, uint32 divisors_address, uint32 ranges_address, uint32 guest_gp, uint32 guest_entry, sint32 (*get_video_mode)(void), uint32 (*copy_bytes)(uint32 destination, uint32 source, uint32 bytes));
        PsxInterruptCallback DrawSyncCallback(PsxInterruptCallback callback);
        DRAWENV *PutDrawEnv(DRAWENV *environment);
        void SsEnd(void);

        typedef struct SndVolume2
        {
            sint16 left;
            sint16 right;
        } SndVolume2;

        typedef struct SndRegisterAttr
        {
            SndVolume2 volume;
            sint16 pitch;
            sint16 mask;
            sint16 addr;
            sint16 adsr1;
            sint16 adsr2;
        } SndRegisterAttr;

        void SsQueueRegisters(sint32 voice, SndRegisterAttr *attributes);
        void SsInit(void);
        void SsQuit(void);
        void SsSetMVol(sint16 left, sint16 right);
        void SsSetSerialVol(sint8 serial, sint16 left, sint16 right);
        void SsSetTickMode(sint32 mode);
        void SsStart(void);
        sint16 SsUtKeyOn(sint16 bank, sint16 program, sint16 tone, sint16 note, sint16 fine, sint16 left, sint16 right);
        void xport_libsnd_bind_voice_limits(uint32 protection_address, uint32 count_address);
        void xport_libsnd_bind_voice_volume(uint32 volume_address, uint32 flags_address);
        void xport_libsnd_bind_sequence_table(uint32 table_address);
        uint32 _SsSeqPlay(sint16 sequence, sint16 separation);
        uint32 SsChannelMute(sint16 sequence, sint16 separation, uint16 mask);
        void xport_libsnd_bind_sequence_handlers(uint32 table_address);
        void spu_sequence_tables(uint32 *sequences, uint32 *handlers);
        uint32 _SsGetSeqData(sint16 sequence, sint16 separation);
        uint32 _SsSetControlChange(sint16 sequence, sint16 separation, uint8 control);
        uint32 _SsContNrpn2(sint16 sequence, sint16 separation, uint8 value);
        uint32 _SsContNrpn1(sint16 sequence, sint16 separation, uint8 value);
        uint32 _SsSetProgramChange(sint16 sequence, sint16 separation, uint8 program);
        uint32 _SsNoteOn(sint16 sequence, sint16 separation, uint8 note, uint8 velocity);
        uint32 _SsVmKeyOn(sint16 identity, sint16 bank, sint16 program, uint16 note, uint16 velocity, uint16 volume);
        uint32 _SsVmKeyOff(sint16 identity, sint16 bank, sint16 program, uint16 note);
        uint32 SsVoKeyOff(uint32 voice_program, uint32 note_volume);
        sint32 _SsVmVSetUp(uint16 bank, sint16 program);
        uint32 _SsVmSelectToneAndVag(uint8 *tones, uint8 *vags);
        uint32 _SsVmAlloc(void);
        uint32 _SsVmDoAllocate(void);
        uint32 _SsVmKeyOnNow(uint16 pitch);
        uint32 _SsVmKeyOffNow(void);
        uint32 vmNoiseOff(uint8 voice);
        uint32 note2pitch(void);
        uint16 SsPitchFromNote(sint32 note, sint16 fine, uint8 center, uint8 shift);
        void xport_libsnd_bind_sequence_mark_table(uint32 table_address);
        uint32 _SsReadDeltaValue(sint16 sequence, sint16 separation);
        uint32 _SsSeqGetEof(sint16 sequence, sint16 separation, uint32 event);
        sint32 SsUtSetDetVVol(uint16 voice, sint16 left, sint16 right);
        sint32 SsVoiceCheck(uint32 voice, sint32 bank_program, sint16 note);
        uint32 SsVoKeyOn(uint32 bank_program, uint32 note_fine, uint16 left, uint16 right);
        sint16 SsUtKeyOnV(sint16 voice, sint16 bank, sint16 program, sint16 tone, sint16 note, sint16 fine, sint16 left, sint16 right);
        sint16 SsVabOpenHead(uint8 *header, sint16 requested_bank);
        sint16 SsVabTransBody(uint8 *body, sint16 bank);
        sint16 SsVabTransCompleted(sint16 mode);
        sint32 CdControl(uint8 command, uint8 *parameter, uint8 *result);
        sint32 CdControlB(uint8 command, uint8 *parameter, uint8 *result);
        sint32 CdControlF(uint8 command, uint8 *parameter);
        sint32 CdRead2(sint32 mode);
        sint32 CdMix(CdlATV *volume);
        sint32 CdPlay(sint32 mode, sint32 *track, sint32 offset);
        sint32 CdDiskReady(sint32 mode);
        sint32 CdGetToc(CdlLOC *toc);
        sint32 CdSetToc(const CdlLOC *toc, sint32 track_count);
        PsxInterruptCallback CdDataCallback(PsxInterruptCallback callback);
        CdlCB CdReadyCallback(CdlCB callback);
        CdlCB CdSyncCallback(CdlCB callback);
        sint32 CdSetDebug(sint32 level);
        void srandPSX(uint32 seed);
        sint32 CdGetToc2(sint32 mode, CdlLOC *toc);
        PsxInterruptCallback DMACallback(uint32 channel, PsxInterruptCallback callback);
        PsxInterruptCallback psx_dma_callback_native(uint32 channel);
        sint32 CD_cw(uint8 command, const uint8 *parameter, uint8 *result, uint32 asynchronous);
        sint32 CD_sync(sint32 mode, uint8 *result);
        sint32 CdSync(sint32 mode, uint8 *result);
        void CdFlush(void);
        sint32 CdDataSync(sint32 mode);
        sint32 CdGetDiskType(void);
        sint32 CdGetSector(void *destination, sint32 words);
        sint32 CdPosToInt(CdlLOC *position);
        uint32 CdReadyCallbackPSX(uint32 callback);
        uint32 CdSyncCallbackPSX(uint32 callback);
        void SpuGetCommonAttr(SpuCommonAttr *attr);
        void SpuGetVoiceAttr(SpuVoiceAttr *attr);
        sint32 SpuGetKeyStatus(uint32 voice_bit);
        void SpuInit(void);
        void SpuInitHot(void);
        void SpuQuit(void);
        void SpuSetCommonAttr(SpuCommonAttr *attr);
        void SpuSetKey(sint32 on_off, uint32 voice_bit);
        sint32 SpuSetReverb(sint32 enabled);
        sint32 SpuSetReverbModeParam(SpuReverbAttr *attr);
        uint32 SpuSetReverbVoice(sint32 on_off, uint32 voice_bit);
        sint32 SpuMalloc(sint32 bytes);
        void xport_mips_break(uint32 code);
        sint32 CD_vol(CdlATV *volume);
        sint32 MargePrim(uint32 first, uint32 second);
        void SpuFree(uint32 address);
        sint32 SpuSetTransferMode(sint32 mode);
        uint32 SpuSetTransferStartAddr(uint32 address);
        uint32 SpuWrite(const void *source, uint32 byte_count);
        sint32 SpuIsTransferCompleted(sint32 mode);
        void SpuSetVoiceAttr(SpuVoiceAttr *attr);
        void SpuSetVoiceLoopStartAddr(sint32 voice, uint32 loop_start_address);
        void SpuSetVoicePitch(sint32 voice, uint16 pitch);
        void SpuSetVoiceStartAddr(sint32 voice, uint32 start_address);
        void SpuSetVoiceVolume(sint32 voice, sint16 left, sint16 right);
        MATRIX *TransMatrix(MATRIX *matrix, VECTOR *translation);
        sint32 VSync(sint32 mode);
        sint32 VSyncCallback(PsxInterruptCallback callback);
        sint32 VectorNormal(VECTOR *input, VECTOR *output);
        sint32 VectorNormalS(VECTOR *input, SVECTOR *output);
        sint32 SetVideoMode(sint32 mode);
        sint32 GetVideoMode(void);
        sint32 gte_project(const SVECTOR *input, sint32 *screen_xy, sint32 *flags);
        sint32 gte_project_full_depth(const SVECTOR *input, sint32 *screen_xy, sint32 *flags);
        void gte_project3_full_depth(const SVECTOR input[3], sint32 screen_xy[3], sint32 depth[3], sint32 *flags);
        void gte_interpolate_project3(const SVECTOR first[3], const SVECTOR second[3], sint32 screen_xy[3], sint32 depth[3], sint32 *flags);
        void gte_rotate_packed(const uint32 registers[5], uint32 xy, uint32 z, sint32 output[3]);
        void gte_transform_raw(const SVECTOR *input, VECTOR *output);
        void gte_transform(const SVECTOR *input, VECTOR *output, sint32 *flags);

#if defined(__cplusplus)
    }
#endif

/* Pad services */
#if defined(__cplusplus)
    extern "C"
    {
#endif

        void pad_publish(sint32 controller, sint32 connected, uint16 buttons);

        typedef struct PsxPadReplayBinding
        {
            uint32 buffers[2];
            uint32 active;
            sint32 main_modes[2];
        } PsxPadReplayBinding;

        sint32 pad_bind_replay(const PsxPadReplayBinding *binding);
        sint32 pad_publish_replay(sint32 controller, const uint8 *report, uint32 length);
        sint32 pad_replay_failed(void);
        sint32 pad_lockstep_apply(sint32 controller, uint32 type, uint32 buttons, uint32 analog);
        int pad_state_io(FILE *file, int load);

#if defined(__cplusplus)
    }
#endif

    /* PS-X executable loading */
    typedef struct PSX_EXE
    {
        const char *path;
        uint32 load_address;
        uint32 bss_start;
        uint32 bss_end;
        uint32 heap_metadata;
        uint32 heap_start;
        uint32 heap_size;
    } PSX_EXE;

#if defined(__cplusplus)
    extern "C"
    {
#endif
        sint32 xport_psx_exe_load(const PSX_EXE *executable);
        sint32 xport_cd_file_size(uint32 guest_path, const char *root, size_t *size);
        sint32 xport_cd_file_size_native(const char *src, const char *root, size_t *size);
        sint32 xport_cd_file_read_native(const char *src, const char *root, void *dst, size_t cap, size_t *size);
        sint32 xport_cd_file_read(uint32 guest_path, const char *root, uint32 destination, size_t capacity, size_t *size);
#if defined(__cplusplus)
    }
#endif

    sint32 SetConf(sint32 events, sint32 threads, uint32 stack_top);
    sint32 _96_init(void);
    sint32 InitTAP(uint32 first_buffer, uint32 second_buffer, sint32 first_mode, sint32 second_mode);
    sint32 StartTAP(void);
    sint32 MemCardInitPSX(void);
    uint32 chkRC2wait(uint32 *registers);
    sint32 MemCardStartPSX(void);

    uint32 MemCardExistPSX(uint32 channel);
    uint32 MemCardSyncPSX(uint32 mode, uint32 command, uint32 result);
    uint32 MemCardOpenPSX(uint32 channel, uint32 filename, uint32 mode);
    uint32 MemCardReadFilePSX(uint32 channel, uint32 filename, uint32 destination, uint32 offset, uint32 length);
    uint32 MemCardClosePSX(void);

    uint32 CdSearchFileGuest(uint32 destination, uint32 filename);

    uint32 _spu_Fw(uint32 source, uint32 length);
    void SpuSetReverbDepth(SpuReverbAttr *attr);
    sint32 SpuReserveReverbWorkArea(sint32 mode);
    sint32 SpuClearReverbWorkArea(sint32 mode);
    sint32 SpuIsReverbWorkAreaReserved(sint32 mode);

    sint32 StartRCnt(uint32 counter);
    void psx_root_counter_bind(uint32 control_slot, uint32 mask_table);
    void psx_root_counter_poll(void);

    void psx_bios_bind_tap_interrupt(uint32 node);
    void psx_bios_bind_card_end_patch(uint32 table, uint32 saved_words);
    uint32 StopTAP(void);
    sint32 MemCardEndPSX(void);

    uint32 xport_gpu_status_read_missing(uint32 address);
    uint32 xport_cd_controller_read_missing(uint32 address, uint32 width);
    void xport_cd_controller_write_missing(uint32 address, uint32 width, uint32 value);
    void xport_memory_control_write32_missing(uint32 address, uint32 value);
    uint32 CDREAD_OBJ_32CPSX(uint32 retry);

#if defined(__cplusplus)
}
#endif

#endif

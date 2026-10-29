#ifndef XPORT_PSX_GPU_H
#define XPORT_PSX_GPU_H
#include "psx.h"
#define GPU_VRAM_WIDTH 1024
#define GPU_VRAM_HEIGHT 512

#if defined(__cplusplus)
extern "C"
{
#endif
    extern const uint32 gpu_dma_nop_terminator[5];
    extern uint16 VRAM[GPU_VRAM_WIDTH * GPU_VRAM_HEIGHT];
    uint32 gpu_vram_version(void);
    sint32 gpu_port_read(uint32 offset, uint32 *value);
    sint32 gpu_port_write(uint32 offset, uint32 value);
    extern sint32 graph_debug_level;

    typedef struct GpuGraphControl
    {
        uint8 type;
        uint8 queue_enabled;
        uint8 reverse;
        uint8 command_cache[256];
    } GpuGraphControl;

    extern GpuGraphControl gpu_graph_control;

    void _ctl(uint32 command);
    uint32 _getctl(uint32 command);
    uint32 _status(void);

    typedef struct GpuTimImage
    {
        uint32 mode;
        PSX_RECT pixels;
        const uint8 *pixel_data;
        PSX_RECT clut;
        const uint8 *clut_data;
    } GpuTimImage;

    // Decode the TIM body starting at its mode word
    sint32 gpu_decode_tim_body(const void *src, size_t size, GpuTimImage *image);

    typedef void (*GpuTransferObserver)(void *context, uint32 operation, uint32 guest_address, const uint32 *words, uint32 count);
    void gpu_set_transfer_observer(GpuTransferObserver observer, void *context);
    sint32 gpu_transfer_observation_failed(void);
    typedef sint32 (*GpuLinkedSubmit)(void *context, uint32 physical_address, uint32 copy_bytes);
    void gpu_bind_linked_submit(GpuLinkedSubmit submit, void *context);
    /* Return 0 idle, 1 paused, -1 wrong mode or -2 unsupported/error */
    typedef sint32 (*GpuLinkedPause)(void *context, uint32 *continuation);
    /* Bind after the submit provider, using that provider's existing context */
    void gpu_bind_linked_pause(GpuLinkedPause pause);
    /* Resume a BreakDraw continuation without a legacy guest V0 adapter */
    void gpu_resume_linked_draw(uint32 continuation);
    sint32 gpu_linked_submit_failed(void);
    sint32 gpu_execute_linked_dma(uint32 physical_address);
    void gpu_set_raster(sint32 enabled);
    sint32 gpu_get_raster(void);
    typedef struct GpuRenderList GpuRenderList;
    GpuRenderList *gpu_render_list_create(void);
    void gpu_render_list_free(GpuRenderList *list);
    sint32 gpu_render_list_begin(GpuRenderList *list, const DRAWENV *environment);
    sint32 gpu_render_list_end(GpuRenderList *list);
    sint32 gpu_render_list_replay(const GpuRenderList *list);
    sint32 gpu_get_display_env(DISPENV *environment);
    // Preserve an embedded polygon texture page without emitting geometry
    void gpu_set_polygon_tpage(uint16 page);

    typedef struct GpuLinkedDmaState
    {
        uint32 address;
        uint32 busy;
    } GpuLinkedDmaState;

    typedef sint32 (*GpuDmaRequest)(void *context);
    typedef sint32 (*GpuDmaWrite)(void *context, uint32 address, const uint32 *words, uint32 count);
    typedef sint32 (*GpuDmaTicks)(void *context, uint32 ticks);
    sint32 gpu_dma_linked_slice(GpuLinkedDmaState *state, uint32 budget, GpuDmaRequest request, GpuDmaWrite write, GpuDmaTicks add_ticks, void *context);
    typedef sint32 (*GpuSyncSample)(void *context, uint32 *dma_control, uint32 *gpu_status);
    typedef sint32 (*GpuSyncStep)(void *context);
    sint32 gpu_bind_sync(uint32 queue_head, uint32 queue_tail, GpuSyncSample sample, GpuSyncStep pump, GpuSyncStep wait, void *context, uint32 budget);
    sint32 gpu_sync_failed(void);

    typedef struct GpuScanoutState
    {
        uint32 mode, disp_y, start_y, end_y;
        uint32 line, tick, field, disp_field;
    } GpuScanoutState;

    void gpu_scanout_reset(GpuScanoutState *state);
    void gpu_scanout_command(GpuScanoutState *state, uint32 command);
    sint32 gpu_scanout_validate(const GpuScanoutState *state);
    sint32 gpu_scanout_advance(GpuScanoutState *state, uint64 ticks);
    sint32 gpu_scanout_ode(const GpuScanoutState *state);

    typedef struct GpuPsyqTimeout
    {
        uint32 vblank_counter;
        uint32 deadline;
        uint32 polls;
    } GpuPsyqTimeout;

    sint32 gpu_psyq_timeout_begin(const GpuPsyqTimeout *binding);
    sint32 gpu_psyq_timeout_check(const GpuPsyqTimeout *binding);
    sint32 gpu_bind_sync_timeout(const GpuPsyqTimeout *binding);

    typedef struct GpuPsyqQueue
    {
        uint32 entries;
        uint32 head;
        uint32 tail;
        uint32 last_command;
    } GpuPsyqQueue;

    typedef struct GpuPsyqCommand
    {
        uint32 function;
        uint32 argument;
        uint32 extra;
    } GpuPsyqCommand;

    sint32 gpu_psyq_queue_store(const GpuPsyqQueue *queue, const GpuPsyqCommand *command, sint32 copy_bytes);
    sint32 gpu_psyq_queue_peek(const GpuPsyqQueue *queue, GpuPsyqCommand *command);
    sint32 gpu_psyq_queue_retire(const GpuPsyqQueue *queue);

    typedef sint32 (*GpuQueueCritical)(void *context, uint32 enabled, uint32 *previous);
    typedef sint32 (*GpuQueueDmaCallback)(void *context, uint32 function);
    typedef sint32 (*GpuQueueDispatch)(void *context, const GpuPsyqCommand *command);
    typedef sint32 (*GpuQueueComplete)(void *context, uint32 function);

    typedef struct GpuPsyqQueueDriver
    {
        GpuPsyqQueue queue;
        uint32 active;
        uint32 completion;
        uint32 saved_interrupt_mask;
        GpuSyncSample sample;
        GpuSyncStep wait;
        GpuQueueCritical critical;
        GpuQueueDmaCallback dma_callback;
        GpuQueueDispatch dispatch;
        GpuQueueComplete complete;
        void *context;
        uint32 budget;
    } GpuPsyqQueueDriver;

    sint32 gpu_psyq_queue_pump(const GpuPsyqQueueDriver *driver);

    typedef struct GpuPsyqSubmit
    {
        const GpuPsyqQueueDriver *driver;
        uint32 enabled;
        uint32 saved_interrupt_mask;
        uint32 pump_function;
        GpuSyncStep timeout_begin;
        GpuSyncStep timeout_check;
    } GpuPsyqSubmit;

    sint32 gpu_psyq_queue_submit(const GpuPsyqSubmit *submit, const GpuPsyqCommand *command, sint32 copy_bytes);

    typedef struct GpuRasterState
    {
        uint32 draw_mode;
        /* GPU flags: dither bit9, draw-display bit10, mask force/check bits11/12 */
        uint32 draw_flags;
        uint32 texture_window;
        uint32 area[4];
        sint32 offset[2];
    } GpuRasterState;

    sint32 gpu_import_raster(const GpuRasterState *state, const void *vram, size_t bytes);
    void gpu_export_raster(GpuRasterState *state);

    typedef struct GpuPsyqStateBinding
    {
        uint32 display_cache;
        uint32 draw_cache;
        uint32 graph_type;
        uint32 reverse_flag;
        uint32 video_mode;
        uint32 gp1_shadow;
    } GpuPsyqStateBinding;

    typedef struct GpuPsyqQueueBinding
    {
        uint32 last_callback;
        uint32 last_argument;
        uint32 last_aux;
        uint32 load_image_callback;
        uint32 move_image_callback;
        uint32 store_image_callback;
        uint32 pointer_segment;
        uint32 alarm_deadline;
        uint32 alarm_active;
        uint32 frame_counter;
        uint32 alarm_delay;
    } GpuPsyqQueueBinding;

    sint32 gpu_bind_psyq_state(const GpuPsyqStateBinding *binding);
    sint32 gpu_bind_psyq_queue(const GpuPsyqQueueBinding *binding);
    sint32 gpu_bind_clear_image(uint32 packet, uint32 dimensions);
    sint32 gpu_bind_move_image(uint32 packet);
    sint32 gpu_bind_soft_reset(uint32 queue_head, uint32 queue_tail);
    sint32 gpu_load_rect(uint32 source, sint32 x, sint32 y, sint32 w, sint32 h);
    sint32 gpu_load_clut(uint32 source, sint32 x, sint32 y);
    void gpu_clear_menu_surfaces(void);
    void gpu_reset_graph_state(void);
    uint32 gpu_set_def_draw_env(uint32 env, sint32 x, sint32 y, sint32 w, sint32 h);
    uint32 gpu_set_def_disp_env(uint32 env, sint32 x, sint32 y, sint32 w, sint32 h);
    void gpu_begin(void);
    void gpu_draw_env(uint32 env, sint32 surface_x, sint32 surface_y);
    void gpu_put_draw_env(uint32 env, sint32 surface_x, sint32 surface_y);
    void gpu_packet(void *packet);
    void gpu_register_dma_range(void *begin, uint32 size);
    sint32 gpu_register_packet_range(void *begin, uint32 size);
    sint32 gpu_present(void);
    // Suppress presentation until all passes of a host frame are complete
    void gpu_frame_begin(void);
    void gpu_frame_end(void);
    void gpu_display_offset(sint32 x, sint32 y);
    void gpu_set_display(sint32 x, sint32 y, sint32 width, sint32 height);
    sint32 gpu_load_vram(const char *path);
    void gpu_init_empty(void);
    sint32 gpu_save_frame(const char *path);
#if defined(__cplusplus)
}
#endif
#endif

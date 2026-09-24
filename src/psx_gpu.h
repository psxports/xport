#ifndef XPORT_PSX_GPU_H
#define XPORT_PSX_GPU_H
#include "xport.h"
#define GPU_VRAM_WIDTH 1024
#define GPU_VRAM_HEIGHT 512

#if defined(__cplusplus)
extern "C"
{
#endif
    extern uint16 VRAM[GPU_VRAM_WIDTH * GPU_VRAM_HEIGHT];
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
    sint32 gpu_present(void);
    void gpu_display_offset(sint32 x, sint32 y);
    void gpu_set_display(sint32 x, sint32 y, sint32 width, sint32 height);
    sint32 gpu_load_vram(const char *path);
    void gpu_init_empty(void);
    sint32 gpu_save_frame(const char *path);
#if defined(__cplusplus)
}
#endif
#endif

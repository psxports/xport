#ifndef VID_H
#define VID_H

#include "xport.h"

typedef struct VID_GEOMETRY VID_GEOMETRY;
typedef struct VID_TEXTURE VID_TEXTURE;
typedef struct VID_INDEX VID_INDEX;
typedef struct VID_MATERIAL VID_MATERIAL;
typedef struct VID_SHADER VID_SHADER;
typedef struct VID_SAMPLE VID_SAMPLE;
typedef struct VID_TARGET VID_TARGET;

enum
{
    VID_RGBA8,
    VID_RGBA5551,
    VID_R8UI,
    VID_RGBA16UI
};

typedef struct VID_VERTEX
{
    float pos[4];
    float uv[2];
    float color[4];
} VID_VERTEX;

typedef struct VID_VERTEX3D
{
    float pos[4];
    float uv[2];
    float color[4];
    float n[4];
    float extra[4];
} VID_VERTEX3D;

typedef struct VID_VERTEX2D
{
    float pos[4], uv[2], alpha;
    uint8 color[4];
    uint16 tex[4], rect[2];
} VID_VERTEX2D;

typedef struct VID_VERTEX16
{
    sint16 pos[4], uv[2];
    uint8 color[4];
    sint16 n[4];
    float extra[4];
    uint16 tex[4], rect[2];
} VID_VERTEX16;

enum
{
    VID_UNIFORM_VECTORS = 32
};

typedef struct VID_RECT
{
    sint32 x, y, w, h;
} VID_RECT;

typedef struct VID_FOG
{
    float near_dist, far_dist, horizon_dist;
    float color[3];
    float focal[2];
} VID_FOG;

enum
{
    VID_BLEND_NONE,
    VID_BLEND_ALPHA,
    VID_BLEND_ADD,
    VID_BLEND_SUBTRACT
};

typedef struct VID_MATERIAL_DATA
{
    VID_TEXTURE *texture;
    VID_TEXTURE *aux[2];
    VID_SHADER *shader;
    float params[8];
    uint32 blend;
    uint32 depth_test;
    uint32 depth_write;
    uint32 filter;
    uint32 repeat;
    uint32 cull;
} VID_MATERIAL_DATA;

#ifdef __cplusplus
extern "C"
{
#endif

    sint32 vid_init(void *window);
    void vid_free(void);
    sint32 vid_blit(const uint32 *pixels, sint32 width, sint32 height, VID_RECT src);

    // Data pointers remain stable until free or vid_index_static
    VID_GEOMETRY *vid_mesh_create(uint32 vertices);
    VID_VERTEX *vid_mesh_data(VID_GEOMETRY *geometry);
    VID_GEOMETRY *vid_mesh_create_3d(uint32 vertices);
    VID_VERTEX3D *vid_mesh_data_3d(VID_GEOMETRY *geometry);
    VID_GEOMETRY *vid_mesh_create_2d(uint32 vertices);
    VID_VERTEX2D *vid_mesh_data_2d(VID_GEOMETRY *geometry);
    VID_GEOMETRY *vid_mesh_create_16(uint32 vertices);
    VID_VERTEX16 *vid_mesh_data_16(VID_GEOMETRY *geometry);
    VID_INDEX *vid_index_create(uint32 count);
    uint32 *vid_index_data(VID_INDEX *indices);
    void vid_index_commit(VID_INDEX *indices, uint32 count);
    void vid_index_static(VID_INDEX *indices);
    void vid_index_free(VID_INDEX *indices);
    void vid_mesh_commit(VID_GEOMETRY *geometry);
    void vid_mesh_free(VID_GEOMETRY *geometry);
    VID_TEXTURE *vid_tex_create(uint32 width, uint32 height);
    VID_TEXTURE *vid_tex_create_fmt(uint32 width, uint32 height, uint32 format);
    uint8 *vid_tex_data(VID_TEXTURE *texture);
    void vid_tex_commit(VID_TEXTURE *texture);
    void vid_tex_free(VID_TEXTURE *texture);
    VID_MATERIAL *vid_mat_create(void);
    VID_MATERIAL_DATA *vid_mat_data(VID_MATERIAL *material);
    void vid_mat_commit(VID_MATERIAL *material);
    void vid_mat_free(VID_MATERIAL *material);
    VID_SHADER *vid_shader_create(const char *vertex, const char *fragment);
    void vid_shader_free(VID_SHADER *shader);
    VID_SAMPLE *vid_sample_create(void);
    void vid_sample_free(VID_SAMPLE *sample);
    uint32 vid_sample_color(const VID_SAMPLE *sample);
    VID_TARGET *vid_target_create(void);
    // Borrowed GPU image, with bottom-left UV origin and no CPU data
    VID_TEXTURE *vid_target_texture(VID_TARGET *target);
    sint32 vid_target_copy(VID_TARGET *destination, const VID_TARGET *source);
    void vid_target_free(VID_TARGET *target);

    // Frame references retain resources until end or cancellation
    sint32 vid_begin(float r, float g, float b);
    // Select before recording commands; end presents and retains the completed image
    sint32 vid_target_select(VID_TARGET *target);
    sint32 vid_target_select_offscreen(VID_TARGET *target);
    void vid_viewport(VID_RECT viewport);
    // Draw commands copy these vectors before the caller changes them
    void vid_uniforms(const float *values, uint32 vectors);
    sint32 vid_draw(VID_GEOMETRY *geometry, uint32 first, uint32 count, VID_MATERIAL *material);
    void vid_pass(VID_MATERIAL *material);
    sint32 vid_submit(VID_GEOMETRY *geometry, VID_INDEX *indices, uint32 first, uint32 count);
    // Queue one color probe at normalized coordinates within the current viewport
    sint32 vid_sample(VID_SAMPLE *sample, float x, float y);
    // Fade subsequent custom-shader draws through a fog color into transparency
    void vid_fog(const VID_FOG *fog);
    sint32 vid_end(void);
    void vid_cancel(void);
    void vid_resize(sint32 *width, sint32 *height);
    VID_RECT vid_best_fit(sint32 width, sint32 height);

#ifdef __cplusplus
}
#endif

#endif

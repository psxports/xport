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

// world-to-camera rows: positive z forward and positive y down
// face order is +x, -x, +y, -y, +z, -z
// size is the square face viewport; use a 90-degree projection
typedef struct VID_CUBE_FACE
{
    uint32 index, size;
    float rotation[3][3];
} VID_CUBE_FACE;

typedef sint32 (*VID_CUBE_RENDER)(void *context, const VID_CUBE_FACE *face);

enum
{
    VID_RGBA8,
    VID_RGBA5551,
    VID_R8UI,
    VID_RGBA16UI,
    VID_DEPTH24,
    VID_RGBA32F,
    VID_RGBA16F,
    VID_DEPTH24_RAW,
    VID_R8
};

typedef struct VID_STATS
{
    uint64 draws, triangles, program_changes, texture_binds, state_changes;
    uint64 uniform_uploads, uniform_bytes, vertex_bytes, index_bytes;
} VID_STATS;

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
    VID_AUX_TEXTURES = 8,
    VID_UNIFORM_VECTORS = 72
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
    VID_BLEND_SUBTRACT,
    VID_BLEND_PREMULTIPLIED
};

enum
{
    VID_FILTER_NEAREST,
    VID_FILTER_LINEAR,
    VID_FILTER_ANISOTROPIC,
    VID_FILTER_TRILINEAR
};

typedef struct VID_MATERIAL_DATA
{
    VID_TEXTURE *texture;
    VID_TEXTURE *aux[VID_AUX_TEXTURES];
    uint32 aux_filter[VID_AUX_TEXTURES];
    VID_SHADER *shader;
    float params[8];
    uint32 blend;
    uint32 depth_test;
    uint32 depth_write;
    uint32 surface_write;
    uint32 filter;
    uint32 repeat;
    uint32 cull;
    float depth_bias_slope;
    float depth_bias_units;
} VID_MATERIAL_DATA;

#ifdef __cplusplus
extern "C"
{
#endif

    sint32 vid_init(void *window);
    void vid_free(void);
    sint32 vid_blit(const uint32 *pixels, sint32 width, sint32 height, VID_RECT src);

    // Data pointers remain stable until free or vid_index_static
    void vid_stats_read(VID_STATS *stats);
    uint64 vid_shader_draw_count(const VID_SHADER *shader);
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
    void vid_mesh_commit_range(VID_GEOMETRY *geometry, uint32 first, uint32 count);
    void vid_mesh_static(VID_GEOMETRY *geometry);
    void vid_mesh_free(VID_GEOMETRY *geometry);
    VID_TEXTURE *vid_tex_create(uint32 width, uint32 height);
    VID_TEXTURE *vid_tex_create_array(uint32 width, uint32 height, uint32 layers);
    VID_TEXTURE *vid_tex_create_array_fmt(uint32 width, uint32 height, uint32 layers, uint32 format);
    VID_TEXTURE *vid_tex_create_fmt(uint32 width, uint32 height, uint32 format);
    uint8 *vid_tex_data(VID_TEXTURE *texture);
    void vid_tex_commit(VID_TEXTURE *texture);
    void vid_tex_static(VID_TEXTURE *texture);
    void vid_tex_set_mip_levels(VID_TEXTURE *texture, uint32 levels);
    void vid_tex_commit_rows(VID_TEXTURE *texture, uint32 first, uint32 count);
    void vid_tex_free(VID_TEXTURE *texture);
    VID_MATERIAL *vid_mat_create(void);
    VID_MATERIAL_DATA *vid_mat_data(VID_MATERIAL *material);
    const VID_MATERIAL_DATA *vid_mat_read(const VID_MATERIAL *material);
    uint64 vid_mat_revision(const VID_MATERIAL *material);
    void vid_mat_commit(VID_MATERIAL *material);
    void vid_mat_free(VID_MATERIAL *material);
    VID_SHADER *vid_shader_create(const char *vertex, const char *fragment);
    VID_SHADER *vid_shader_create_array(const char *vertex, const char *fragment);
    VID_SHADER *vid_shader_create_cube(const char *vertex, const char *fragment);
    void vid_shader_free(VID_SHADER *shader);
    VID_SAMPLE *vid_sample_create(void);
    void vid_sample_free(VID_SAMPLE *sample);
    uint32 vid_sample_color(const VID_SAMPLE *sample);
    VID_TARGET *vid_target_create(void);
    VID_TARGET *vid_target_create_hdr(void);
    VID_TARGET *vid_target_create_hdr_depth(void);
    VID_TARGET *vid_target_create_scene(void);
    VID_TARGET *vid_target_create_scene_ldr(void);
    VID_TARGET *vid_target_create_depth(void);
    VID_TARGET *vid_target_create_depth_array(uint32 size, uint32 layers);
    VID_TARGET *vid_target_create_cube(uint32 size);
    VID_TARGET *vid_target_create_cube_depth(uint32 size);
    // Borrowed GPU image, with bottom-left UV origin and no CPU data
    VID_TEXTURE *vid_target_texture(VID_TARGET *target);
    VID_TEXTURE *vid_target_depth_texture(VID_TARGET *target);
    VID_TEXTURE *vid_target_surface_texture(VID_TARGET *target);
    sint32 vid_target_copy(VID_TARGET *destination, const VID_TARGET *source);
    sint32 vid_target_copy_mip(VID_TARGET *destination, uint32 level, const VID_TARGET *source);
    void vid_target_free(VID_TARGET *target);

    // Frame references retain resources until end or cancellation
    sint32 vid_begin(float r, float g, float b);
    // Select before recording commands; end presents and retains the completed image
    sint32 vid_target_select(VID_TARGET *target);
    sint32 vid_target_select_offset(VID_TARGET *target, float y);
    sint32 vid_target_select_offscreen(VID_TARGET *target);
    sint32 vid_target_select_offscreen_size(VID_TARGET *target, uint32 width, uint32 height);
    sint32 vid_target_select_mip(VID_TARGET *target, uint32 level);
    sint32 vid_target_select_cube_face(VID_TARGET *target, uint32 face);
    sint32 vid_target_select_depth_layer(VID_TARGET *target, uint32 layer);
    sint32 vid_target_next_part(uint32 part);
    // begin, record six offscreen faces and end without presenting
    sint32 vid_cube_capture(VID_TARGET *target, const float clear[3], VID_CUBE_RENDER render, void *context);
    void vid_viewport(VID_RECT viewport);
    // Draw commands copy these vectors before the caller changes them
    void vid_uniforms(const float *values, uint32 vectors);
    sint32 vid_draw(VID_GEOMETRY *geometry, uint32 first, uint32 count, VID_MATERIAL *material);
    void vid_pass(VID_MATERIAL *material);
    sint32 vid_submit(VID_GEOMETRY *geometry, VID_INDEX *indices, uint32 first, uint32 count);
    sint32 vid_clear_depth(void);
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

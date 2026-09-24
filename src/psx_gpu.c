#include <stdlib.h>
/* Software GPU raster core */
#include "psx.h"
#include "psx_gpu.h"
#include "xport.h"
#include <string.h>
#include <stdio.h>
#define VRAM_W GPU_VRAM_WIDTH
#define VRAM_H GPU_VRAM_HEIGHT

enum
{
    GPU_DISPLAY_MAX_WIDTH = 640,
    GPU_DISPLAY_MAX_HEIGHT = 512
};

uint16 VRAM[GPU_VRAM_WIDTH * GPU_VRAM_HEIGHT];

enum
{
    GPU_NATIVE_DMA_RANGE_COUNT = 32
};

typedef struct GpuNativeDmaRange
{
    uintptr_t begin;
    uintptr_t end;
} GpuNativeDmaRange;

static GpuNativeDmaRange native_dma_ranges[GPU_NATIVE_DMA_RANGE_COUNT];
static sint32 native_dma_range_count;

void gpu_register_dma_range(void *begin, uint32 size)
{
    uintptr_t first = (uintptr_t)begin;
    uintptr_t end = first + size;
    sint32 index;
    if (!begin || !size || end < first)
        return;
    for (index = 0; index < native_dma_range_count; ++index)
        if (native_dma_ranges[index].begin == first && native_dma_ranges[index].end == end)
            return;
    if (native_dma_range_count >= GPU_NATIVE_DMA_RANGE_COUNT)
        return;
    native_dma_ranges[native_dma_range_count].begin = first;
    native_dma_ranges[native_dma_range_count].end = end;
    ++native_dma_range_count;
}

static sint32 gpu_native_dma_pointer(uint32 link, uint32 **packet)
{
    sint32 index;
    for (index = 0; index < native_dma_range_count; ++index)
    {
        uintptr_t segment = native_dma_ranges[index].begin & ~(uintptr_t)0x00ffffffu;
        sint32 adjustment;
        for (adjustment = -1; adjustment <= 1; ++adjustment)
        {
            uintptr_t candidate = segment + (intptr_t)adjustment * 0x01000000 + (link & 0x00fffffcu);
            if (candidate >= native_dma_ranges[index].begin && candidate + sizeof(**packet) <= native_dma_ranges[index].end)
            {
                *packet = (uint32 *)candidate;
                return 1;
            }
        }
    }
    return 0;
}

static sint32 gpu_wrap_x(sint32 x)
{
    return x & (GPU_VRAM_WIDTH - 1);
}

static sint32 gpu_wrap_y(sint32 y)
{
    return y & (GPU_VRAM_HEIGHT - 1);
}

sint32 gpu_load_rect(uint32 source, sint32 x, sint32 y, sint32 width, sint32 height)
{
    sint32 row;
    sint32 column;
    if (!source || width < 0 || height < 0)
        return 0;
    for (row = 0; row < height; ++row)
    {
        const uint16 *pixels = (const uint16 *)psx_addr(source + (uint32)(row * width * 2), (size_t)width * 2);
        for (column = 0; column < width; ++column)
            VRAM[gpu_wrap_y(y + row) * GPU_VRAM_WIDTH + gpu_wrap_x(x + column)] = pixels[column];
    }
    return 1;
}

sint32 LoadImagePSX(PSX_RECT *rectangle, uint32 *pixels)
{
    const uint16 *source = (const uint16 *)pixels;
    sint32 row;
    sint32 column;
    if (!rectangle || !source || rectangle->w < 0 || rectangle->h < 0)
        return -1;
    for (row = 0; row < rectangle->h; ++row)
        for (column = 0; column < rectangle->w; ++column)
            VRAM[gpu_wrap_y(rectangle->y + row) * GPU_VRAM_WIDTH + gpu_wrap_x(rectangle->x + column)] = source[row * rectangle->w + column];
    return 0;
}

sint32 StoreImage(PSX_RECT *rectangle, uint32 *pixels)
{
    sint32 row;
    sint32 column;
    uint16 *destination = (uint16 *)pixels;
    if (!rectangle || !destination || rectangle->w < 0 || rectangle->h < 0)
        return -1;
    for (row = 0; row < rectangle->h; ++row)
        for (column = 0; column < rectangle->w; ++column)
            destination[row * rectangle->w + column] = VRAM[gpu_wrap_y(rectangle->y + row) * GPU_VRAM_WIDTH + gpu_wrap_x(rectangle->x + column)];
    return 0;
}

sint32 MoveImage(PSX_RECT *rectangle, sint32 x, sint32 y)
{
    uint16 row_pixels[GPU_VRAM_WIDTH];
    sint32 first;
    sint32 last;
    sint32 step;
    sint32 row;
    sint32 column;
    if (!rectangle || rectangle->w < 0 || rectangle->w > GPU_VRAM_WIDTH || rectangle->h < 0)
        return -1;
    first = y > rectangle->y ? rectangle->h - 1 : 0;
    last = y > rectangle->y ? -1 : rectangle->h;
    step = y > rectangle->y ? -1 : 1;
    for (row = first; row != last; row += step)
    {
        for (column = 0; column < rectangle->w; ++column)
            row_pixels[column] = VRAM[gpu_wrap_y(rectangle->y + row) * GPU_VRAM_WIDTH + gpu_wrap_x(rectangle->x + column)];
        for (column = 0; column < rectangle->w; ++column)
            VRAM[gpu_wrap_y(y + row) * GPU_VRAM_WIDTH + gpu_wrap_x(x + column)] = row_pixels[column];
    }
    return 0;
}

sint32 ClearImage(PSX_RECT *rectangle, uint8 red, uint8 green, uint8 blue)
{
    sint32 row;
    sint32 column;
    uint16 color = (uint16)((red >> 3) | ((green >> 3) << 5) | ((blue >> 3) << 10));
    if (!rectangle || rectangle->w < 0 || rectangle->h < 0)
        return -1;
    for (row = 0; row < rectangle->h; ++row)
        for (column = 0; column < rectangle->w; ++column)
            VRAM[gpu_wrap_y(rectangle->y + row) * GPU_VRAM_WIDTH + gpu_wrap_x(rectangle->x + column)] = color;
    return 0;
}

static sint32 gpu_guest_address(const void *pointer, uint32 *address);

static sint32 gpu_native_low24_to_guest(uint32 link, uint32 *address)
{
    uintptr_t bases[2] = {(uintptr_t)DRAM, (uintptr_t)SCRATCHPAD};
    size_t sizes[2] = {sizeof(DRAM), sizeof(SCRATCHPAD)};
    uint32 guest_bases[2] = {0, 0x1f800000u};
    sint32 i;
    for (i = 0; i < 2; ++i)
    {
        uintptr_t segment = bases[i] & ~(uintptr_t)0x00ffffffu;
        sint32 adjustment;
        for (adjustment = -1; adjustment <= 1; ++adjustment)
        {
            uintptr_t candidate = segment + (intptr_t)adjustment * 0x01000000 + (link & 0x00fffffcu);
            if (candidate >= bases[i] && candidate - bases[i] < sizes[i])
            {
                *address = guest_bases[i] + (uint32)(candidate - bases[i]);
                return 1;
            }
        }
    }
    return 0;
}

uint32 *ClearOTagR(uint32 *ot, sint32 count)
{
    sint32 index;
    uint32 address;
    if (!gpu_guest_address(ot, &address))
        address = (uint32)(intptr)ot;
    if (count <= 0)
        return ot;
    ot[0] = 0x00ffffffu;
    for (index = 1; index < count; ++index)
        ot[index] = (address + (uint32)(index - 1) * 4u) & 0x00ffffffu;
    return ot;
}

uint32 *ClearOTag(uint32 *ot, sint32 count)
{
    sint32 index;
    uint32 address;
    if (!gpu_guest_address(ot, &address))
        address = (uint32)(intptr)ot;
    if (count <= 0)
        return ot;
    for (index = 0; index + 1 < count; ++index)
        ot[index] = (address + (uint32)(index + 1) * 4u) & 0x00ffffffu;
    ot[count - 1] = 0x00ffffffu;
    return ot;
}

void AddPrim(void *ot, void *prim)
{
    uint32 *ot_tag = (uint32 *)ot;
    uint32 *prim_tag = (uint32 *)prim;
    uint32 address;
    if (!gpu_guest_address(prim, &address))
        address = (uint32)(intptr)prim;
    *prim_tag = (*prim_tag & 0xff000000u) | (*ot_tag & 0x00ffffffu);
    *ot_tag = address & 0x00ffffffu;
}

void AddPrims(void *ot, void *first, void *last)
{
    uint32 *ot_tag = (uint32 *)ot;
    uint32 *last_tag = (uint32 *)last;
    uint32 address;
    if (!gpu_guest_address(first, &address))
        address = (uint32)(intptr)first;
    *last_tag = (*last_tag & 0xff000000u) | (*ot_tag & 0x00ffffffu);
    *ot_tag = address & 0x00ffffffu;
}

uint16 GetTPage(sint32 depth, sint32 abr, sint32 x, sint32 y)
{
    return (uint16)(((depth & 3) << 7) | ((abr & 3) << 5) | ((y & 0x100) >> 4) | ((x & 0x3ff) >> 6) | ((y & 0x200) << 2));
}

uint16 GetClut(sint32 x, sint32 y)
{
    return (uint16)(((y & 0x1ff) << 6) | ((x >> 4) & 0x3f));
}

uint16 LoadTPage(uint32 *pixels, sint32 depth, sint32 abr, sint32 x, sint32 y, sint32 width, sint32 height)
{
    PSX_RECT rectangle;
    rectangle.x = (sint16)x;
    rectangle.y = (sint16)y;
    rectangle.w = (sint16)(depth == 0 ? width / 4 : depth == 1 ? width / 2 : width);
    rectangle.h = (sint16)height;
    LoadImagePSX(&rectangle, pixels);
    return GetTPage(depth, abr, x, y);
}

uint16 LoadClut(uint32 *pixels, sint32 x, sint32 y)
{
    PSX_RECT rectangle;
    rectangle.x = (sint16)x;
    rectangle.y = (sint16)y;
    rectangle.w = 16;
    rectangle.h = 1;
    if (LoadImagePSX(&rectangle, pixels) < 0)
        return 0;
    return GetClut(x, y);
}

sint32 gpu_load_clut(uint32 source, sint32 x, sint32 y)
{
    return gpu_load_rect(source, (sint16)x, (sint16)y, 256, 1);
}

typedef struct
{
    sint32 x, y, u, v;
} RV;

typedef struct
{
    sint32 x, y, u, v, r, g, b;
} ScanVertex;

static uint32 present_pixels[GPU_DISPLAY_MAX_WIDTH * GPU_DISPLAY_MAX_HEIGHT];
static uint16 active_tpage;
static uint32 texture_window;
static uint32 display_state;
static sint32 graph_debug_level;
static sint32 display_x;
static sint32 display_y;
static sint32 display_width = 320;
static sint32 display_height = 240;
static sint32 display_offset_x;
static sint32 display_offset_y;
static uint32 gpu_raster_pixel(sint32 x, sint32 y);

static sint32 gpu_guest_address(const void *pointer, uint32 *address)
{
    uintptr_t value = (uintptr_t)pointer;
    uintptr_t base = (uintptr_t)DRAM;
    if (value >= base && value - base < sizeof(DRAM))
    {
        *address = (uint32)(value - base);
        return 1;
    }
    base = (uintptr_t)SCRATCHPAD;
    if (value >= base && value - base < sizeof(SCRATCHPAD))
    {
        *address = 0x1f800000u + (uint32)(value - base);
        return 1;
    }
    return 0;
}

static uint32 gpu_u32(uint32 address)
{
    return *(uint32 *)psx_addr(address, 4);
}

static sint16 gpu_s16(uint32 address)
{
    return *(sint16 *)psx_addr(address, 2);
}

static void gpu_w32(uint32 address, uint32 value)
{
    *(uint32 *)psx_addr(address, 4) = value;
}

static void gpu_w16(uint32 address, uint16 value)
{
    *(uint16 *)psx_addr(address, 2) = value;
}

static void gpu_w8(uint32 address, uint8 value)
{
    *(uint8 *)psx_addr(address, 1) = value;
}

/* Diagnostic policy only: keep GPU commands and VRAM transfers, omit host pixels.
 * VRAM/pixel verification must explicitly set the game-provided environment variables */
static sint32 raster_enabled(void)
{
    static sint32 force = -1;
    if (!xport_is_headless())
        return 1;
    if (force < 0)
    {
        const char *r = getenv("XPORT_RASTERIZE"), *v = getenv("XPORT_VERIFY_VRAM");
        force = (r && !strcmp(r, "1")) || (v && !strcmp(v, "1"));
        printf("headless_rasterization %s\n", force ? "enabled" : "disabled");
    }
    return force;
}

static sint32 raster_clip_x0, raster_clip_y0, raster_clip_x1 = VRAM_W, raster_clip_y1 = VRAM_H, raster_offset_x, raster_offset_y;
static sint32 raster_surface_x, raster_surface_y;

static sint16 read_s16_le_at(const uint8 *p, sint32 o)
{
    return (sint16)((uint16)p[o] | (uint16)p[o + 1] << 8);
}

static void pxc(sint32 x, sint32 y, uint32 color)
{
    sint32 vram_x = x + raster_surface_x;
    sint32 vram_y = y + raster_surface_y;
    if (x >= raster_clip_x0 && x < raster_clip_x1 && y >= raster_clip_y0 && y < raster_clip_y1 && (uint32)vram_x < VRAM_W && (uint32)vram_y < VRAM_H)
        VRAM[vram_y * VRAM_W + vram_x] = (uint16)(((color >> 19) & 31u) | ((color >> 6) & 0x3e0u) | ((color << 7) & 0x7c00u));
}

static uint32 rgb555(uint16 c)
{
    uint32 r = (c & 31) << 3, g = ((c >> 5) & 31) << 3, b = ((c >> 10) & 31) << 3;
    return (r | (r >> 5)) << 16 | (g | (g >> 5)) << 8 | (b | (b >> 5));
}

static uint16 texel_indexed(sint32 u, sint32 v, uint16 tpage, uint16 clut, sint32 *transparent)
{
    sint32 tp = (tpage >> 7) & 3, tx = (tpage & 15) * 64, ty = ((tpage >> 4) & 1) * 256, cx = (clut & 63) * 16, cy = (clut >> 6) & 0x1ff, index;
    uint16 w, color;
    u = ((u & ~(((sint32)texture_window & 31) << 3)) | ((((sint32)texture_window >> 10) & 31) & ((sint32)texture_window & 31)) << 3) & 255;
    v = ((v & ~((((sint32)texture_window >> 5) & 31) << 3)) | ((((sint32)texture_window >> 15) & 31) & (((sint32)texture_window >> 5) & 31)) << 3) & 255;
    *transparent = 1;
    if ((uint32)(ty + v) >= VRAM_H || (uint32)cy >= VRAM_H)
        return 0;
    if (tp == 0)
    {
        w = VRAM[(ty + v) * VRAM_W + tx + (u >> 2)];
        index = (w >> ((u & 3) * 4)) & 15;
    }
    else if (tp == 1)
    {
        w = VRAM[(ty + v) * VRAM_W + tx + (u >> 1)];
        index = (w >> ((u & 1) * 8)) & 255;
    }
    else
    {
        if ((uint32)(tx + u) >= VRAM_W)
            return 0;
        color = VRAM[(ty + v) * VRAM_W + tx + u];
        *transparent = color == 0;
        return color;
    }
    if ((uint32)(cx + index) >= VRAM_W)
        return 0;
    color = VRAM[cy * VRAM_W + cx + index];
    *transparent = color == 0;
    return color;
}

static uint16 texel(sint32 u, sint32 v, uint16 tpage, uint16 clut)
{
    sint32 transparent;
    return texel_indexed(u, v, tpage, clut, &transparent);
}

static uint32 modulate(uint16 c, sint32 r, sint32 g, sint32 b, sint32 raw)
{
    uint32 q = rgb555(c), tr = (q >> 16) & 255, tg = (q >> 8) & 255, tb = q & 255;
    if (raw)
        return q;
    tr = tr * r / 128;
    tg = tg * g / 128;
    tb = tb * b / 128;
    if (tr > 255)
        tr = 255;
    if (tg > 255)
        tg = 255;
    if (tb > 255)
        tb = 255;
    return tr << 16 | tg << 8 | tb;
}

static uint32 semi_blend(uint32 back, uint32 front, sint32 abr)
{
    sint32 br = (back >> 19) & 31, bg = (back >> 11) & 31, bb = (back >> 3) & 31, fr = (front >> 19) & 31, fg = (front >> 11) & 31, fbv = (front >> 3) & 31, r, g, b;
    if (abr == 0)
    {
        r = (br + fr) >> 1;
        g = (bg + fg) >> 1;
        b = (bb + fbv) >> 1;
    }
    else if (abr == 1)
    {
        r = br + fr;
        g = bg + fg;
        b = bb + fbv;
    }
    else if (abr == 2)
    {
        r = br - fr;
        g = bg - fg;
        b = bb - fbv;
    }
    else
    {
        r = br + (fr >> 2);
        g = bg + (fg >> 2);
        b = bb + (fbv >> 2);
    }
    if (r < 0)
        r = 0;
    if (g < 0)
        g = 0;
    if (b < 0)
        b = 0;
    if (r > 31)
        r = 31;
    if (g > 31)
        g = 31;
    if (b > 31)
        b = 31;
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return (uint32)(r << 16 | g << 8 | b);
}

static uint32 packet_rgb(uint8 *p, sint32 o)
{
    return (uint32)p[o] << 16 | (uint32)p[o + 1] << 8 | p[o + 2];
}

static void scan_swap(ScanVertex *a, ScanVertex *b)
{
    ScanVertex value = *a;
    *a = *b;
    *b = value;
}

static sint32 scan_edge_x(const ScanVertex *a, const ScanVertex *b, sint32 y)
{
    sint64 sample_y = ((sint64)y << 16) + 0x8000;
    if (a->y == b->y)
        return a->x << 16;
    return (sint32)(((sint64)a->x << 16) + (sample_y - ((sint64)a->y << 16)) * (b->x - a->x) / (b->y - a->y));
}

static sint32 scan_ceil_center(sint32 value)
{
    return (value - 0x8000 + 0xffff) >> 16;
}

static sint32 scan_gradient(sint32 av, sint32 bv, sint32 cv, const ScanVertex *a, const ScanVertex *b, const ScanVertex *c, sint64 denominator, sint32 horizontal)
{
    sint64 value;
    if (horizontal)
        value = (sint64)(bv - av) * (c->y - a->y) - (sint64)(cv - av) * (b->y - a->y);
    else
        value = (sint64)(b->x - a->x) * (cv - av) - (sint64)(c->x - a->x) * (bv - av);
    return (sint32)(value * 65536 / denominator);
}

static sint32 scan_clamp_channel(sint32 value)
{
    value >>= 16;
    if (value < 0)
        return 0;
    return value > 255 ? 255 : value;
}

static void scan_triangle(RV va, RV vb, RV vc, uint16 tp, uint16 cl, uint32 ca, uint32 cb, uint32 cc, sint32 textured, sint32 raw, sint32 semi)
{
    ScanVertex a = {va.x, va.y, va.u, va.v, (sint32)((ca >> 16) & 255), (sint32)((ca >> 8) & 255), (sint32)(ca & 255)};
    ScanVertex b = {vb.x, vb.y, vb.u, vb.v, (sint32)((cb >> 16) & 255), (sint32)((cb >> 8) & 255), (sint32)(cb & 255)};
    ScanVertex c = {vc.x, vc.y, vc.u, vc.v, (sint32)((cc >> 16) & 255), (sint32)((cc >> 8) & 255), (sint32)(cc & 255)};
    sint64 denominator;
    sint32 dudx, dvdx, drdx, dgdx, dbdx;
    sint32 dudy, dvdy, drdy, dgdy, dbdy;
    sint64 ubase, vbase, rbase, gbase, bbase;
    sint32 min_y, max_y, y;
    if (a.y > b.y)
        scan_swap(&a, &b);
    if (b.y > c.y)
        scan_swap(&b, &c);
    if (a.y > b.y)
        scan_swap(&a, &b);
    denominator = (sint64)(b.x - a.x) * (c.y - a.y) - (sint64)(c.x - a.x) * (b.y - a.y);
    if (!denominator || a.y == c.y)
        return;
    dudx = scan_gradient(a.u, b.u, c.u, &a, &b, &c, denominator, 1);
    dvdx = scan_gradient(a.v, b.v, c.v, &a, &b, &c, denominator, 1);
    drdx = scan_gradient(a.r, b.r, c.r, &a, &b, &c, denominator, 1);
    dgdx = scan_gradient(a.g, b.g, c.g, &a, &b, &c, denominator, 1);
    dbdx = scan_gradient(a.b, b.b, c.b, &a, &b, &c, denominator, 1);
    dudy = scan_gradient(a.u, b.u, c.u, &a, &b, &c, denominator, 0);
    dvdy = scan_gradient(a.v, b.v, c.v, &a, &b, &c, denominator, 0);
    drdy = scan_gradient(a.r, b.r, c.r, &a, &b, &c, denominator, 0);
    dgdy = scan_gradient(a.g, b.g, c.g, &a, &b, &c, denominator, 0);
    dbdy = scan_gradient(a.b, b.b, c.b, &a, &b, &c, denominator, 0);
    ubase = ((sint64)a.u << 16) - (sint64)dudx * a.x - (sint64)dudy * a.y + (dudx + dudy) / 2;
    vbase = ((sint64)a.v << 16) - (sint64)dvdx * a.x - (sint64)dvdy * a.y + (dvdx + dvdy) / 2;
    rbase = ((sint64)a.r << 16) - (sint64)drdx * a.x - (sint64)drdy * a.y + (drdx + drdy) / 2;
    gbase = ((sint64)a.g << 16) - (sint64)dgdx * a.x - (sint64)dgdy * a.y + (dgdx + dgdy) / 2;
    bbase = ((sint64)a.b << 16) - (sint64)dbdx * a.x - (sint64)dbdy * a.y + (dbdx + dbdy) / 2;
    min_y = a.y;
    max_y = c.y;
    if (min_y < raster_clip_y0)
        min_y = raster_clip_y0;
    if (min_y < -raster_surface_y)
        min_y = -raster_surface_y;
    if (max_y > raster_clip_y1)
        max_y = raster_clip_y1;
    if (max_y > VRAM_H - raster_surface_y)
        max_y = VRAM_H - raster_surface_y;
    for (y = min_y; y < max_y; ++y)
    {
        const ScanVertex *short_a = y < b.y ? &a : &b;
        const ScanVertex *short_b = y < b.y ? &b : &c;
        sint32 left = scan_edge_x(&a, &c, y);
        sint32 right = scan_edge_x(short_a, short_b, y);
        sint32 x, end, u, v, r, g, blue;
        if (left > right)
        {
            sint32 swap = left;
            left = right;
            right = swap;
        }
        x = scan_ceil_center(left);
        end = scan_ceil_center(right);
        if (x < raster_clip_x0)
            x = raster_clip_x0;
        if (x < -raster_surface_x)
            x = -raster_surface_x;
        if (end > raster_clip_x1)
            end = raster_clip_x1;
        if (end > VRAM_W - raster_surface_x)
            end = VRAM_W - raster_surface_x;
        u = (sint32)(ubase + (sint64)dudx * x + (sint64)dudy * y);
        v = (sint32)(vbase + (sint64)dvdx * x + (sint64)dvdy * y);
        r = (sint32)(rbase + (sint64)drdx * x + (sint64)drdy * y);
        g = (sint32)(gbase + (sint64)dgdx * x + (sint64)dgdy * y);
        blue = (sint32)(bbase + (sint64)dbdx * x + (sint64)dbdy * y);
        for (; x < end; ++x)
        {
            uint32 color;
            if (textured)
            {
                sint32 transparent;
                uint16 texel = texel_indexed(u >> 16, v >> 16, tp, cl, &transparent);
                if (!transparent)
                {
                    color = modulate(texel, raw ? 128 : scan_clamp_channel(r), raw ? 128 : scan_clamp_channel(g), raw ? 128 : scan_clamp_channel(blue), 0);
                    if (semi && (texel & 0x8000))
                        color = semi_blend(gpu_raster_pixel(x, y), color, (tp >> 5) & 3);
                    pxc(x, y, color);
                }
            }
            else
            {
                color = (uint32)scan_clamp_channel(r) << 16 | (uint32)scan_clamp_channel(g) << 8 | (uint32)scan_clamp_channel(blue);
                if (semi)
                    color = semi_blend(gpu_raster_pixel(x, y), color, (active_tpage >> 5) & 3);
                pxc(x, y, color);
            }
            u += dudx;
            v += dvdx;
            r += drdx;
            g += dgdx;
            blue += dbdx;
        }
    }
}

static void textured_triangle(RV a, RV b, RV c, uint16 tp, uint16 cl, uint32 ca, uint32 cb, uint32 cc, sint32 raw, sint32 semi)
{
    scan_triangle(a, b, c, tp, cl, ca, cb, cc, 1, raw, semi);
}

static void colored_triangle(RV a, RV b, RV c, uint32 ca, uint32 cb, uint32 cc, sint32 semi)
{
    scan_triangle(a, b, c, active_tpage, 0, ca, cb, cc, 0, 0, semi);
}

static RV rv(uint8 *p, sint32 xy, sint32 uv)
{
    RV v;
    v.x = read_s16_le_at(p, xy) + raster_offset_x;
    v.y = read_s16_le_at(p, xy + 2) + raster_offset_y;
    v.u = p[uv];
    v.v = p[uv + 1];
    return v;
}

static void magenta_bbox(uint8 *p, sint32 n, const sint32 *ofs)
{
    sint32 i, minx = 32767, miny = 32767, maxx = -32768, maxy = -32768, x, y;
    for (i = 0; i < n; i++)
    {
        x = read_s16_le_at(p, ofs[i]);
        y = read_s16_le_at(p, ofs[i] + 2);
        if (x < minx)
            minx = x;
        if (x > maxx)
            maxx = x;
        if (y < miny)
            miny = y;
        if (y > maxy)
            maxy = y;
    }
    if (minx < 0)
        minx = 0;
    if (miny < 0)
        miny = 0;
    if (maxx >= VRAM_W)
        maxx = VRAM_W - 1;
    if (maxy >= VRAM_H)
        maxy = VRAM_H - 1;
    for (y = miny; y <= maxy; y++)
        for (x = minx; x <= maxx; x++)
            pxc(x, y, 0xff00ff);
}

static sint32 gpu_sign_extend_11(uint32 value)
{
    value &= 0x7ffu;
    return (value & 0x400u) ? (sint32)(value | ~0x7ffu) : (sint32)value;
}

static sint32 gpu_draw_state_word(uint32 word)
{
    switch (word >> 24)
    {
        case 0xe1:
            active_tpage = (uint16)(word & 0x3fffu);
            return 1;
        case 0xe2:
            texture_window = word & 0xfffffu;
            return 1;
        case 0xe3:
            raster_clip_x0 = (sint32)(word & 0x3ffu) - raster_surface_x;
            raster_clip_y0 = (sint32)((word >> 10) & 0x1ffu) - raster_surface_y;
            return 1;
        case 0xe4:
            raster_clip_x1 = (sint32)(word & 0x3ffu) + 1 - raster_surface_x;
            raster_clip_y1 = (sint32)((word >> 10) & 0x1ffu) + 1 - raster_surface_y;
            return 1;
        case 0xe5:
            raster_offset_x = gpu_sign_extend_11(word) - raster_surface_x;
            raster_offset_y = gpu_sign_extend_11(word >> 11) - raster_surface_y;
            return 1;
        case 0xe6:
            return 1;
        default:
            return 0;
    }
}

static void draw_prim(void *raw)
{
    uint8 *p = (uint8 *)raw;
    sint32 code = p[7] & 0xfc;
    uint32 *w = (uint32 *)p;
    uint32 ca, cb, cc, cd;
    if (gpu_draw_state_word(w[1]))
    {
        uint32 count = w[0] >> 24;
        uint32 index;
        for (index = 2; index <= count && gpu_draw_state_word(w[index]); ++index)
        {
        }
        if (index + 2 <= count && (w[index] & 0xfc000000u) == 0x60000000u)
        {
            uint32 tile[4] = {0x03000000u, w[index], w[index + 1], w[index + 2]};
            draw_prim(tile);
        }
        return;
    }
    if (!raster_enabled())
        return;
    ca = packet_rgb(p, 4);
    /* Host approximation of flat two-point PSX lines. Visual fidelity is
     * secondary; retain clipping, draw offset and semitransparency. */
    if (code == 0x40)
    {
        RV a = rv(p, 8, 0), b = rv(p, 12, 0);
        sint32 dx = abs(b.x - a.x), dy = -abs(b.y - a.y);
        sint32 sx = a.x < b.x ? 1 : -1, sy = a.y < b.y ? 1 : -1;
        sint32 err = dx + dy, e;
        for (;;)
        {
            if ((uint32)(a.x + raster_surface_x) < VRAM_W && (uint32)(a.y + raster_surface_y) < VRAM_H)
            {
                uint32 color = ca;
                if (p[7] & 2)
                    color = semi_blend(gpu_raster_pixel(a.x, a.y), color, (active_tpage >> 5) & 3);
                pxc(a.x, a.y, color);
            }
            if (a.x == b.x && a.y == b.y)
                break;
            e = 2 * err;
            if (e >= dy)
            {
                err += dy;
                a.x += sx;
            }
            if (e <= dx)
            {
                err += dx;
                a.y += sy;
            }
        }
        return;
    }
    if (code == 0x20)
    {
        RV a = rv(p, 8, 0), b = rv(p, 12, 0), c = rv(p, 16, 0);
        colored_triangle(a, b, c, ca, ca, ca, (p[7] & 2) != 0);
        return;
    }
    if (code == 0x28)
    {
        RV a = rv(p, 8, 0), b = rv(p, 12, 0), c = rv(p, 16, 0), d = rv(p, 20, 0);
        colored_triangle(a, b, c, ca, ca, ca, (p[7] & 2) != 0);
        colored_triangle(b, c, d, ca, ca, ca, (p[7] & 2) != 0);
        return;
    }
    if (code == 0x30)
    {
        RV a = rv(p, 8, 0), b = rv(p, 16, 0), c = rv(p, 24, 0);
        cb = packet_rgb(p, 12);
        cc = packet_rgb(p, 20);
        colored_triangle(a, b, c, ca, cb, cc, (p[7] & 2) != 0);
        return;
    }
    if (code == 0x38)
    {
        RV a = rv(p, 8, 0), b = rv(p, 16, 0), c = rv(p, 24, 0), d = rv(p, 32, 0);
        cb = packet_rgb(p, 12);
        cc = packet_rgb(p, 20);
        cd = packet_rgb(p, 28);
        colored_triangle(a, b, c, ca, cb, cc, (p[7] & 2) != 0);
        colored_triangle(b, c, d, cb, cc, cd, (p[7] & 2) != 0);
        return;
    }
    if (code == 0x24)
    {
        RV a = rv(p, 8, 12), b = rv(p, 16, 20), c = rv(p, 24, 28);
        textured_triangle(a, b, c, *(uint16 *)(p + 22), *(uint16 *)(p + 14), ca, ca, ca, p[7] & 1, (p[7] & 2) != 0);
        return;
    }
    if (code == 0x2c)
    {
        RV a = rv(p, 8, 12), b = rv(p, 16, 20), c = rv(p, 24, 28), d = rv(p, 32, 36);
        uint16 tp = *(uint16 *)(p + 22), cl = *(uint16 *)(p + 14);
        textured_triangle(a, b, c, tp, cl, ca, ca, ca, p[7] & 1, (p[7] & 2) != 0);
        textured_triangle(b, c, d, tp, cl, ca, ca, ca, p[7] & 1, (p[7] & 2) != 0);
        return;
    }
    if (code == 0x34)
    {
        RV a = rv(p, 8, 12), b = rv(p, 20, 24), c = rv(p, 32, 36);
        uint16 tp = *(uint16 *)(p + 26), cl = *(uint16 *)(p + 14);
        cb = packet_rgb(p, 16);
        cc = packet_rgb(p, 28);
        textured_triangle(a, b, c, tp, cl, ca, cb, cc, p[7] & 1, (p[7] & 2) != 0);
        return;
    }
    if (code == 0x3c)
    {
        RV a = rv(p, 8, 12), b = rv(p, 20, 24), c = rv(p, 32, 36), d = rv(p, 44, 48);
        uint16 tp = *(uint16 *)(p + 26), cl = *(uint16 *)(p + 14);
        cb = packet_rgb(p, 16);
        cc = packet_rgb(p, 28);
        cd = packet_rgb(p, 40);
        textured_triangle(a, b, c, tp, cl, ca, cb, cc, p[7] & 1, (p[7] & 2) != 0);
        textured_triangle(b, c, d, tp, cl, cb, cc, cd, p[7] & 1, (p[7] & 2) != 0);
        return;
    }
    if (code == 0x64 || code == 0x74 || code == 0x7c)
    {
        sint32 x, y, x0 = read_s16_le_at(p, 8) + raster_offset_x, y0 = read_s16_le_at(p, 10) + raster_offset_y, ww = code == 0x64 ? read_s16_le_at(p, 16) : (code == 0x74 ? 8 : 16), hh = code == 0x64 ? read_s16_le_at(p, 18) : (code == 0x74 ? 8 : 16);
        for (y = 0; y < hh; y++)
            for (x = 0; x < ww; x++)
            {
                sint32 transparent;
                uint16 t = texel_indexed(p[12] + x, p[13] + y, active_tpage, *(uint16 *)(p + 14), &transparent);
                if (!transparent && x0 + x >= raster_clip_x0 && x0 + x < raster_clip_x1 && y0 + y >= raster_clip_y0 && y0 + y < raster_clip_y1 && (uint32)(x0 + x + raster_surface_x) < VRAM_W && (uint32)(y0 + y + raster_surface_y) < VRAM_H)
                {
                    uint32 color = modulate(t, p[4], p[5], p[6], p[7] & 1);
                    if ((p[7] & 2) && (t & 0x8000))
                        color = semi_blend(gpu_raster_pixel(x0 + x, y0 + y), color, (active_tpage >> 5) & 3);
                    pxc(x0 + x, y0 + y, color);
                }
            }
        return;
    }
    if (code == 0x60 || code == 0x68 || code == 0x70 || code == 0x78)
    {
        sint32 x, y, x0 = read_s16_le_at(p, 8) + raster_offset_x, y0 = read_s16_le_at(p, 10) + raster_offset_y, ww = code == 0x60 ? read_s16_le_at(p, 12) : (code == 0x68 ? 1 : code == 0x70 ? 8 : 16), hh = code == 0x60 ? read_s16_le_at(p, 14) : (code == 0x68 ? 1 : code == 0x70 ? 8 : 16);
        for (y = 0; y < hh; y++)
            for (x = 0; x < ww; x++)
            {
                uint32 color = ca;
                if ((uint32)(x0 + x + raster_surface_x) >= VRAM_W || (uint32)(y0 + y + raster_surface_y) >= VRAM_H)
                    continue;
                if (p[7] & 2)
                    color = semi_blend(gpu_raster_pixel(x0 + x, y0 + y), color, (active_tpage >> 5) & 3);
                pxc(x0 + x, y0 + y, color);
            }
        return;
    }
    pxc(read_s16_le_at(p, 8) + raster_offset_x, read_s16_le_at(p, 10) + raster_offset_y, 0xff00ff);
}

void gpu_begin(void)
{
}

sint32 ResetGraph(sint32 mode)
{
    if (mode != 0)
        return -1;
    gpu_reset_graph_state();
    if (xport_is_headless())
        return 0;
    return xport_window_init() ? 0 : -1;
}

sint32 SetGraphDebug(sint32 level)
{
    sint32 previous = graph_debug_level;
    graph_debug_level = level;
    return previous;
}

sint32 DrawSync(sint32 mode)
{
    return mode == 0 || mode == 1 ? 0 : -1;
}

void SetDispMask(sint32 enabled)
{
    display_state = enabled ? 0u : 2u;
}

static uint32 gpu_raster_pixel(sint32 x, sint32 y)
{
    sint32 vram_x = x + raster_surface_x;
    sint32 vram_y = y + raster_surface_y;
    if ((uint32)vram_x >= VRAM_W || (uint32)vram_y >= VRAM_H)
        return 0;
    return rgb555(VRAM[vram_y * VRAM_W + vram_x]);
}

/* Host PsyQ DrawOTagEnv adaptation, not a replacement decompilation of the SDK.
 * The caller supplies the VRAM origin of the draw surface (not the displayed
 * surface). This preserves relative clip/offset changes in the 320x240 window.
 * Dithering, draw-to-display inhibition and GPU mask bits remain WIP. */
void gpu_draw_env(uint32 env, sint32 surface_x, sint32 surface_y)
{
    const uint8 *e = (const uint8 *)psx_addr(env, 28);
    sint32 x, y;
    raster_surface_x = surface_x;
    raster_surface_y = surface_y;
    raster_clip_x0 = gpu_s16(env) - surface_x;
    raster_clip_y0 = gpu_s16(env + 2) - surface_y;
    raster_clip_x1 = raster_clip_x0 + gpu_s16(env + 4);
    raster_clip_y1 = raster_clip_y0 + gpu_s16(env + 6);
    raster_offset_x = gpu_s16(env + 8) - surface_x;
    raster_offset_y = gpu_s16(env + 10) - surface_y;
    active_tpage = (uint16)gpu_s16(env + 20);
    /* Pack the PsyQ texture-window fields into a GP0 command */
    texture_window = (((uint32)(-gpu_s16(env + 16)) & 255u) >> 3) | ((((uint32)(-gpu_s16(env + 18)) & 255u) >> 3) << 5) | ((uint32)(e[12] >> 3) << 10) | ((uint32)(e[14] >> 3) << 15);
    if (e[24] && raster_enabled())
    {
        uint32 color = (uint32)e[25] << 16 | (uint32)e[26] << 8 | e[27];
        for (y = raster_clip_y0; y < raster_clip_y1; y++)
            for (x = raster_clip_x0; x < raster_clip_x1; x++)
                pxc(x, y, color);
    }
}

static uint8 gpu_env_u8(uint32 address)
{
    return *(uint8 *)psx_addr(address, 1);
}

static sint32 gpu_env_clamp(sint32 value, sint32 limit)
{
    if (value < 0)
        return 0;
    return value > limit - 1 ? limit - 1 : value;
}

static uint32 gpu_env_clip(uint32 command, sint32 x, sint32 y, uint32 wide)
{
    uint32 mask = wide ? 4095u : 1023u, shift = wide ? 12u : 10u;
    x = gpu_env_clamp((sint16)x, VRAM_W);
    y = gpu_env_clamp((sint16)y, VRAM_H);
    return command | ((uint32)x & mask) | (((uint32)y & mask) << shift);
}

/* 700A0 packet writes and 6F828 cache copy, audited from actual MIPS.
 * DMA/interrupt queue bookkeeping remains a separate host SDK adaptation. */
void gpu_put_draw_env(uint32 env, sint32 surface_x, sint32 surface_y)
{
    uint32 packet = env + 28, wide = 0;
    uint32 mask = wide ? 4095u : 2047u, shift = wide ? 12u : 11u, mode, count = 6;
    sint32 x, y, w, h;
    gpu_w32(packet + 4, gpu_env_clip(0xe3000000u, gpu_s16(env), gpu_s16(env + 2), wide));
    gpu_w32(packet + 8, gpu_env_clip(0xe4000000u, (sint16)((uint16)gpu_s16(env) + (uint16)gpu_s16(env + 4) - 1), (sint16)((uint16)gpu_s16(env + 2) + (uint16)gpu_s16(env + 6) - 1), wide));
    gpu_w32(packet + 12, 0xe5000000u | ((uint32)gpu_s16(env + 8) & mask) | (((uint32)gpu_s16(env + 10) & mask) << shift));
    mode = 0xe1000000u | ((uint16)gpu_s16(env + 20) & (wide ? 0x27ffu : 0x9ffu));
    if (gpu_env_u8(env + 22))
        mode |= wide ? 0x800u : 0x200u;
    if (gpu_env_u8(env + 23))
        mode |= wide ? 0x1000u : 0x400u;
    gpu_w32(packet + 16, mode);
    gpu_w32(packet + 20, 0xe2000000u | (((uint32)-gpu_s16(env + 16) & 255u) >> 3) | ((((uint32)-gpu_s16(env + 18) & 255u) >> 3) << 5) | ((uint32)(gpu_env_u8(env + 12) >> 3) << 10) | ((uint32)(gpu_env_u8(env + 14) >> 3) << 15));
    gpu_w32(packet + 24, 0xe6000000u);
    if (gpu_env_u8(env + 24))
    {
        x = gpu_s16(env);
        y = gpu_s16(env + 2);
        w = (sint16)gpu_env_clamp(gpu_s16(env + 4), VRAM_W);
        h = (sint16)gpu_env_clamp(gpu_s16(env + 6), VRAM_H);
        mode = 0x02000000u;
        if (((uint32)x & 63u) || ((uint32)w & 63u))
        {
            mode = 0x60000000u;
            x = (sint16)(x - gpu_s16(env + 8));
            y = (sint16)(y - gpu_s16(env + 10));
        }
        gpu_w32(packet + 28, mode | gpu_env_u8(env + 25) | ((uint32)gpu_env_u8(env + 26) << 8) | ((uint32)gpu_env_u8(env + 27) << 16));
        gpu_w32(packet + 32, (uint16)x | ((uint32)(uint16)y << 16));
        gpu_w32(packet + 36, (uint16)w | ((uint32)(uint16)h << 16));
        count = 9;
    }
    gpu_w8(packet + 3, (uint8)count);
    gpu_w32(packet, gpu_u32(packet) | 0xffffffu);
    gpu_draw_env(env, surface_x, surface_y);
}

void gpu_packet(void *packet)
{
    draw_prim(packet);
}

/* Host DMA adapter: consume PSX address tags, never native pointer tags. */
void DrawOTag(uint32 *ot)
{
    uint32 head;
    uint32 count = 0;
    sint32 native = !gpu_guest_address(ot, &head);
    uint32 native_segment = 0;
    if (native)
    {
        head = (uint32)(intptr)ot;
        native_segment = head & 0xff000000u;
    }
    while ((head & 0xffffffu) != 0xffffffu)
    {
        uint32 link = head & 0xffffffu;
        uint32 address = native ? native_segment | link : link;
        uint32 *packet = 0;
        uint32 tag;
        sint32 packet_native = native;
#if !defined(NDEBUG)
        size_t packet_size;
#endif
        if ((address & 3u) || count++ >= 65536)
            return;
        if (native && !xport_memory_readable((const void *)(intptr)address, sizeof(*packet)))
        {
            /* Translated callers pass a native OT pointer whose links remain guest addresses */
            native = 0;
            packet_native = 0;
            address = link;
        }
        if (!native && address >= PSX_DRAM_SIZE && (address < 0x1f800000u || address >= 0x1f800400u))
        {
            if (!gpu_native_low24_to_guest(link, &address))
            {
                if (!gpu_native_dma_pointer(link, &packet))
                    return;
                packet_native = 1;
            }
        }
        if (!packet_native)
            packet = (uint32 *)psx_addr(address, sizeof(*packet));
        else if (native)
            packet = (uint32 *)(intptr)address;
#if !defined(NDEBUG)
        if (packet_native && !xport_memory_readable(packet, sizeof(*packet)))
            return;
#endif
        tag = *packet;
        /* Preserve the original terminal packet in RAM. The host still omits this
   * exact degenerate black-line sentinel, as it did before original OT setup
   * was translated. Other flat line packets use the host rasterizer. */
        if (link == 0x10018u && tag == 0x03ffffffu && packet[1] == 0x40000000u && !packet[2] && !packet[3])
        {
            head = tag;
            continue;
        }
        if (tag >> 24)
        {
#if !defined(NDEBUG)
            packet_size = 4u + 4u * (tag >> 24);
            if (packet_native && !xport_memory_readable(packet, packet_size))
                return;
#endif
            draw_prim(packet_native ? packet : psx_addr(address, 4u + 4u * (tag >> 24)));
        }
        head = tag;
    }
}

static void gpu_convert_display(void)
{
    sint32 x;
    sint32 y;
    for (y = 0; y < display_height; ++y)
        for (x = 0; x < display_width; ++x)
        {
            sint32 source_x = x - display_offset_x;
            sint32 source_y = y - display_offset_y;
            present_pixels[y * display_width + x] = (uint32)source_x < (uint32)display_width && (uint32)source_y < (uint32)display_height ? rgb555(VRAM[(display_y + source_y) * VRAM_W + display_x + source_x]) : 0;
        }
}

sint32 gpu_present(void)
{
    if (display_state & 2u)
        return 1;
    if (!raster_enabled())
        return 1;
    gpu_convert_display();
    return xport_present(present_pixels, display_width, display_height, 0, 0, display_width, display_height, WND_TITLE);
}

void gpu_display_offset(sint32 x, sint32 y)
{
    display_offset_x = x;
    display_offset_y = y;
}

void gpu_set_display(sint32 x, sint32 y, sint32 width, sint32 height)
{
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x >= VRAM_W)
        x = VRAM_W - 1;
    if (y >= VRAM_H)
        y = VRAM_H - 1;
    if (width < 1)
        width = 1;
    if (height < 1)
        height = 1;
    if (width > GPU_DISPLAY_MAX_WIDTH)
        width = GPU_DISPLAY_MAX_WIDTH;
    if (height > GPU_DISPLAY_MAX_HEIGHT)
        height = GPU_DISPLAY_MAX_HEIGHT;
    if (x + width > VRAM_W)
        width = VRAM_W - x;
    if (y + height > VRAM_H)
        height = VRAM_H - y;
    display_x = x;
    display_y = y;
    display_width = width;
    display_height = height;
}

void gpu_init_empty(void)
{
    display_state = 0;
    memset(VRAM, 0, sizeof(VRAM));
    active_tpage = 0;
    texture_window = 0;
    raster_clip_x0 = raster_clip_y0 = raster_offset_x = raster_offset_y = 0;
    raster_clip_x1 = VRAM_W;
    raster_clip_y1 = VRAM_H;
    raster_surface_x = raster_surface_y = 0;
    display_offset_x = display_offset_y = 0;
    gpu_set_display(0, 0, 320, 240);
}

sint32 gpu_load_vram(const char *path)
{
    size_t n;
    return xport_file_read(path, VRAM, sizeof(VRAM), &n) && n == sizeof(VRAM);
}

sint32 gpu_save_frame(const char *path)
{
    FILE *f;
    size_t n;
    if (!raster_enabled())
        return 0; /* No valid pixels to export in fast mode. */
    gpu_convert_display();
    f = fopen(path, "wb");
    if (!f)
        return 0;
    n = fwrite(present_pixels, sizeof(*present_pixels), (size_t)display_width * display_height, f);
    fclose(f);
    return n == (size_t)display_width * display_height;
}

/* Reset only host GPU state; PsyQ environment calls provide game dimensions */
void gpu_reset_graph_state(void)
{
    display_state = 0;
    active_tpage = 0;
    texture_window = 0;
    raster_clip_x0 = 0;
    raster_clip_y0 = 0;
    raster_clip_x1 = VRAM_W;
    raster_clip_y1 = VRAM_H;
    raster_offset_x = 0;
    raster_offset_y = 0;
    raster_surface_x = 0;
    raster_surface_y = 0;
    display_offset_x = 0;
    display_offset_y = 0;
}

/* DRAWENV packet storage at +28 is deliberately untouched */
uint32 gpu_set_def_draw_env(uint32 env, sint32 x, sint32 y, sint32 w, sint32 h)
{
    uint32 i;
    gpu_w16(env, (uint16)x);
    gpu_w16(env + 2, (uint16)y);
    gpu_w16(env + 4, (uint16)w);
    gpu_w16(env + 6, (uint16)h);
    for (i = 12; i <= 18; i += 2)
        gpu_w16(env + i, 0);
    gpu_w8(env + 25, 0);
    gpu_w8(env + 26, 0);
    gpu_w8(env + 27, 0);
    gpu_w8(env + 22, 1);
    gpu_w8(env + 23, (uint8)(h < 289));
    gpu_w16(env + 8, (uint16)x);
    gpu_w16(env + 10, (uint16)y);
    gpu_w16(env + 20, 10);
    gpu_w8(env + 24, 0);
    return env;
}

uint32 gpu_set_def_disp_env(uint32 env, sint32 x, sint32 y, sint32 w, sint32 h)
{
    gpu_w16(env, (uint16)x);
    gpu_w16(env + 2, (uint16)y);
    gpu_w16(env + 4, (uint16)w);
    memset(psx_addr(env + 8, 12), 0, 12);
    gpu_w16(env + 6, (uint16)h);
    return env;
}

DRAWENV *SetDefDrawEnv(DRAWENV *environment, sint32 x, sint32 y, sint32 width, sint32 height)
{
    if (!environment)
        return 0;
    gpu_set_def_draw_env((uint32)(intptr)environment, x, y, width, height);
    return environment;
}

DISPENV *SetDefDispEnv(DISPENV *environment, sint32 x, sint32 y, sint32 width, sint32 height)
{
    if (!environment)
        return 0;
    gpu_set_def_disp_env((uint32)(intptr)environment, x, y, width, height);
    return environment;
}

void SetDrawArea(void *packet, PSX_RECT *rectangle)
{
    uint32 *words = (uint32 *)packet;
    if (!words || !rectangle)
        return;
    words[0] = (words[0] & 0x00ffffffu) | 0x02000000u;
    words[1] = gpu_env_clip(0xe3000000u, rectangle->x, rectangle->y, 0);
    words[2] = gpu_env_clip(0xe4000000u, rectangle->x + rectangle->w - 1, rectangle->y + rectangle->h - 1, 0);
}

void SetDrawEnv(void *packet, DRAWENV *environment)
{
    uint32 *words = (uint32 *)packet;
    sint32 x;
    sint32 y;
    sint32 count = 6;
    if (!words || !environment)
        return;
    words[1] = gpu_env_clip(0xe3000000u, environment->clip.x, environment->clip.y, 0);
    words[2] = gpu_env_clip(0xe4000000u, environment->clip.x + environment->clip.w - 1, environment->clip.y + environment->clip.h - 1, 0);
    words[3] = 0xe5000000u | ((uint32)environment->ofs[0] & 0x7ffu) | (((uint32)environment->ofs[1] & 0x7ffu) << 11);
    words[4] = 0xe1000000u | (environment->tpage & 0x9ffu) | (environment->dtd ? 0x200u : 0u) | (environment->dfe ? 0x400u : 0u);
    words[5] = psx_texture_window(&environment->tw);
    words[6] = 0xe6000000u;
    if (environment->isbg)
    {
        x = environment->clip.x - environment->ofs[0];
        y = environment->clip.y - environment->ofs[1];
        words[7] = 0x60000000u | (uint32)environment->r0 | ((uint32)environment->g0 << 8) | ((uint32)environment->b0 << 16);
        words[8] = (uint16)x | ((uint32)(uint16)y << 16);
        words[9] = (uint16)environment->clip.w | ((uint32)(uint16)environment->clip.h << 16);
        count = 9;
    }
    words[0] = (words[0] & 0x00ffffffu) | ((uint32)count << 24);
}

void SetDrawStp(void *packet, sint32 enabled)
{
    uint32 *words = (uint32 *)packet;
    if (!words)
        return;
    words[0] = (words[0] & 0x00ffffffu) | 0x02000000u;
    words[1] = 0xe6000000u | (enabled ? 1u : 0u);
    words[2] = 0;
}

DRAWENV *PutDrawEnv(DRAWENV *environment)
{
    if (!environment)
        return 0;
    gpu_put_draw_env((uint32)(intptr)environment, 0, 0);
    return environment;
}

DISPENV *PutDispEnv(DISPENV *environment)
{
    if (!environment)
        return 0;
    gpu_set_display(environment->disp.x, environment->disp.y, environment->disp.w, environment->disp.h);
    return environment;
}

void gpu_clear_menu_surfaces(void)
{
    uint32 y;
    for (y = 0; y < 256; y++)
        memset(VRAM + y * VRAM_W, 0, 640 * sizeof(uint16));
}

static int gpu_state_block(FILE *file, void *data, size_t size, int load)
{
    uint32 stored = (uint32)size;
    if (load)
        return fread(&stored, 4, 1, file) == 1 && stored == size && fread(data, 1, size, file) == size;
    return fwrite(&stored, 4, 1, file) == 1 && fwrite(data, 1, size, file) == size;
}

int gpu_state_io(FILE *f, int load)
{
    return gpu_state_block(f, &VRAM, sizeof(VRAM), load) && gpu_state_block(f, &active_tpage, sizeof(active_tpage), load) && gpu_state_block(f, &texture_window, sizeof(texture_window), load) && gpu_state_block(f, &display_state, sizeof(display_state), load) && gpu_state_block(f, &display_x, sizeof(display_x), load) && gpu_state_block(f, &display_y, sizeof(display_y), load) && gpu_state_block(f, &display_width, sizeof(display_width), load) && gpu_state_block(f, &display_height, sizeof(display_height), load) && gpu_state_block(f, &display_offset_x, sizeof(display_offset_x), load) && gpu_state_block(f, &display_offset_y, sizeof(display_offset_y), load) && gpu_state_block(f, &raster_clip_x0, sizeof(raster_clip_x0), load) && gpu_state_block(f, &raster_clip_y0, sizeof(raster_clip_y0), load) && gpu_state_block(f, &raster_clip_x1, sizeof(raster_clip_x1), load) && gpu_state_block(f, &raster_clip_y1, sizeof(raster_clip_y1), load) &&
           gpu_state_block(f, &raster_offset_x, sizeof(raster_offset_x), load) && gpu_state_block(f, &raster_offset_y, sizeof(raster_offset_y), load) && gpu_state_block(f, &raster_surface_x, sizeof(raster_surface_x), load) && gpu_state_block(f, &raster_surface_y, sizeof(raster_surface_y), load);
}

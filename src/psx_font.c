#include "psx.h"
#include "psx_gpu.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FONT_COUNT 8
#define FONT_CHARS 1024

// PsyQ LIBGPU font atlas verified against SDK FONT.OBJ data
static const uint32 font_clut[8] = {
    0xFFFF0000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
};

static const uint32 font_pixels[512] = {
    0x00000000u, 0x00010000u, 0x00101000u, 0x00000000u, 0x00111000u, 0x01111100u, 0x00001000u, 0x00011000u, 0x01000000u, 0x00000100u, 0x00010000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00010000u, 0x00101000u, 0x00101000u, 0x01010100u, 0x01001100u, 0x00010100u, 0x00001100u, 0x00100000u, 0x00001000u, 0x01010100u, 0x00010000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x01000000u, 0x00000000u, 0x00010000u, 0x00000000u, 0x01111100u, 0x00010100u, 0x00100000u, 0x00010100u, 0x00000000u, 0x00010000u, 0x00010000u, 0x00111000u, 0x00010000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00100000u, 0x00000000u, 0x00010000u, 0x00000000u, 0x00101000u, 0x00111000u, 0x00010000u, 0x00001000u, 0x00000000u, 0x00010000u, 0x00010000u, 0x00010000u, 0x01111100u, 0x00000000u, 0x01111100u, 0x00000000u, 0x00010000u, 0x00000000u, 0x00010000u, 0x00000000u, 0x00101000u, 0x01010000u, 0x00001000u, 0x01010100u, 0x00000000u, 0x00010000u, 0x00010000u,
    0x00111000u, 0x00010000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00001000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x01111100u, 0x01010100u, 0x01100100u, 0x00100100u, 0x00000000u, 0x00100000u, 0x00001000u, 0x01010100u, 0x00010000u, 0x00011000u, 0x00000000u, 0x00011000u, 0x00000100u, 0x00000000u, 0x00010000u, 0x00000000u, 0x00101000u, 0x00111000u, 0x01100100u, 0x01011000u, 0x00000000u, 0x01000000u, 0x00000100u, 0x00010000u, 0x00000000u, 0x00011000u, 0x00000000u, 0x00011000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00010000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00001100u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00111000u, 0x00010000u, 0x00111000u, 0x00111000u, 0x00100000u, 0x01111100u, 0x00110000u, 0x01111100u, 0x00111000u, 0x00111000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00111000u, 0x01000100u, 0x00011000u, 0x01000100u, 0x01000100u,
    0x00110000u, 0x00000100u, 0x00001000u, 0x01000000u, 0x01000100u, 0x01000100u, 0x00000000u, 0x00000000u, 0x00100000u, 0x00000000u, 0x00001000u, 0x01000100u, 0x01100100u, 0x00010000u, 0x01000000u, 0x01000000u, 0x00101000u, 0x00111100u, 0x00000100u, 0x00100000u, 0x01000100u, 0x01000100u, 0x00011000u, 0x00011000u, 0x00010000u, 0x01111100u, 0x00010000u, 0x00100000u, 0x01010100u, 0x00010000u, 0x00100000u, 0x00110000u, 0x00100100u, 0x01000000u, 0x00111100u, 0x00100000u, 0x00111000u, 0x01111000u, 0x00011000u, 0x00011000u, 0x00001000u, 0x00000000u, 0x00100000u, 0x00010000u, 0x01001100u, 0x00010000u, 0x00010000u, 0x01000000u, 0x01111100u, 0x01000000u, 0x01000100u, 0x00010000u, 0x01000100u, 0x01000000u, 0x00000000u, 0x00000000u, 0x00010000u, 0x01111100u, 0x00010000u, 0x00010000u, 0x01000100u, 0x00010000u, 0x00001000u, 0x01000100u, 0x00100000u, 0x01000100u, 0x01000100u, 0x00010000u, 0x01000100u, 0x00100000u, 0x00011000u, 0x00011000u, 0x00100000u, 0x00000000u,
    0x00001000u, 0x00000000u, 0x00111000u, 0x00111000u, 0x01111100u, 0x00111000u, 0x00100000u, 0x00111000u, 0x00111000u, 0x00010000u, 0x00111000u, 0x00011000u, 0x00011000u, 0x00011000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00010000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00001100u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00111000u, 0x00111000u, 0x00111100u, 0x00111000u, 0x00111100u, 0x01111100u, 0x01111100u, 0x00111000u, 0x01000100u, 0x00111000u, 0x01000000u, 0x01000100u, 0x00000100u, 0x01000100u, 0x01000100u, 0x00111000u, 0x01000100u, 0x01000100u, 0x01000100u, 0x01000100u, 0x01001000u, 0x00000100u, 0x00000100u, 0x01000100u, 0x01000100u, 0x00010000u, 0x01000000u, 0x00100100u, 0x00000100u, 0x01101100u, 0x01000100u, 0x01000100u, 0x01110100u, 0x01000100u, 0x01000100u, 0x00000100u, 0x01001000u, 0x00000100u, 0x00000100u, 0x00000100u,
    0x01000100u, 0x00010000u, 0x01000000u, 0x00010100u, 0x00000100u, 0x01010100u, 0x01001100u, 0x01000100u, 0x01010100u, 0x01111100u, 0x00111100u, 0x00000100u, 0x01001000u, 0x00111100u, 0x00111100u, 0x00000100u, 0x01111100u, 0x00010000u, 0x01000000u, 0x00001100u, 0x00000100u, 0x01000100u, 0x01010100u, 0x01000100u, 0x01110100u, 0x01000100u, 0x01000100u, 0x00000100u, 0x01001000u, 0x00000100u, 0x00000100u, 0x01100100u, 0x01000100u, 0x00010000u, 0x01000000u, 0x00010100u, 0x00000100u, 0x01000100u, 0x01100100u, 0x01000100u, 0x00000100u, 0x01000100u, 0x01000100u, 0x01000100u, 0x01001000u, 0x00000100u, 0x00000100u, 0x01000100u, 0x01000100u, 0x00010000u, 0x01000100u, 0x00100100u, 0x00000100u, 0x01000100u, 0x01000100u, 0x01000100u, 0x00111000u, 0x01000100u, 0x00111100u, 0x00111000u, 0x00111100u, 0x01111100u, 0x00000100u, 0x00111000u, 0x01000100u, 0x00111000u, 0x00111000u, 0x01000100u, 0x01111100u, 0x01000100u, 0x01000100u, 0x00111000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00111100u, 0x00111000u, 0x00111100u, 0x00111000u, 0x01111100u, 0x01000100u, 0x01000100u, 0x01000100u, 0x01000100u, 0x01000100u, 0x01111100u, 0x00010000u, 0x00000000u, 0x00010000u, 0x00101000u, 0x00000000u, 0x01000100u, 0x01000100u, 0x01000100u, 0x01000100u, 0x00010000u, 0x01000100u, 0x01000100u, 0x01000100u, 0x01000100u, 0x01000100u, 0x01000000u, 0x00010000u, 0x00000100u, 0x00010000u, 0x01000100u, 0x00000000u, 0x01000100u, 0x01000100u, 0x01000100u, 0x00000100u, 0x00010000u, 0x01000100u, 0x01000100u, 0x01000100u, 0x00101000u, 0x00101000u, 0x00100000u, 0x00010000u, 0x00001000u, 0x00010000u, 0x00000000u, 0x00000000u, 0x00111100u, 0x01000100u, 0x00111100u, 0x00111000u, 0x00010000u, 0x01000100u, 0x01000100u, 0x01000100u, 0x00010000u, 0x00010000u, 0x01111100u, 0x00010000u,
    0x00010000u, 0x00010000u, 0x00000000u, 0x00000000u, 0x00000100u, 0x01010100u, 0x00010100u, 0x01000000u, 0x00010000u, 0x01000100u, 0x00101000u, 0x01010100u, 0x00101000u, 0x00010000u, 0x00001000u, 0x00010000u, 0x00100000u, 0x00010000u, 0x00000000u, 0x00000000u, 0x00000100u, 0x00100100u, 0x00100100u, 0x01000100u, 0x00010000u, 0x01000100u, 0x00101000u, 0x01101100u, 0x01000100u, 0x00010000u, 0x00000100u, 0x00010000u, 0x01000000u, 0x00010000u, 0x00000000u, 0x00000000u, 0x00000100u, 0x01011000u, 0x01000100u, 0x00111000u, 0x00010000u, 0x00111000u, 0x00010000u, 0x01000100u, 0x01000100u, 0x00010000u, 0x01111100u, 0x00010000u, 0x00000000u, 0x00010000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x01110000u, 0x00000000u, 0x00011100u, 0x00000000u, 0x00000000u,
};

typedef struct
{
    TILE bg;
    DR_MODE mode;
    SPRT_8 glyphs[FONT_CHARS];
    char text[FONT_CHARS + 2];
    sint32 cap;
    sint32 used;
    uint32 auto_size;
} FONT_STATE;

static FONT_STATE fonts[FONT_COUNT];
static uint32 font_count, font_chars;
static sint32 dump_font;
static uint16 font_tpage, font_clut_id;

void FntLoad(sint32 x, sint32 y)
{
    font_clut_id = LoadClut((uint32 *)(void *)font_clut, x, y + 128);
    font_tpage = LoadTPage((uint32 *)(void *)font_pixels, 0, 0, x, y, 128, 32);
    font_count = 0;
    memset(fonts, 0, sizeof(fonts));
    if (!gpu_register_packet_range(fonts, sizeof(fonts)))
        abort();
}

sint32 FntOpen(sint32 x, sint32 y, sint32 w, sint32 h, sint32 bg, sint32 n)
{
    FONT_STATE *font;
    PSX_RECT tw = {0, 0, 256, 256};
    sint32 idx;

    if (font_count >= FONT_COUNT || n < 0)
        return -1;
    if (font_count == 0)
        font_chars = 0;
    if ((uint32)n > FONT_CHARS - font_chars)
        n = (sint32)(FONT_CHARS - font_chars);
    font = &fonts[font_count];
    font->auto_size = w == 0;
    SetDrawMode(&font->mode, 0, 0, font_tpage, &tw);
    if (bg)
    {
        SetTile(&font->bg);
        setRGB0(&font->bg, 0, 0, 0);
        SetSemiTrans(&font->bg, bg == 2);
    }
    setXY0(&font->bg, x, y);
    setWH(&font->bg, w, h);
    font->cap = n;
    font->used = 0;
    font->text[0] = 0;
    for (idx = 0; idx < n; ++idx)
    {
        SetSprt8(&font->glyphs[idx]);
        font->glyphs[idx].p[0].attr = font_clut_id;
    }
    font_chars += (uint32)n;
    return (sint32)font_count++;
}

void SetDumpFnt(sint32 id)
{
    if (id >= 0 && (uint32)id <= font_count)
        dump_font = id;
}

// Keep the original overflow count within a separate byte and NUL slot
static sint32 font_put(FONT_STATE *font, char ch)
{
    if (font->used > font->cap)
        return 0;
    font->text[font->used++] = ch;
    font->text[font->used] = 0;
    return font->used <= font->cap;
}

sint32 FntPrint(const char *fmt, ...)
{
    static const char hex[] = "0123456789ABCDEF";
    FONT_STATE *font;
    va_list args;
    sint32 id = dump_font;

    va_start(args, fmt);
    if ((uintptr_t)fmt < font_count)
    {
        id = (sint32)(uintptr_t)fmt;
        fmt = va_arg(args, const char *);
    }
    if (id < 0 || (uint32)id >= font_count || !fmt)
        goto fail;
    font = &fonts[id];
    if (font->used > font->cap)
        goto fail;
    while (*fmt)
    {
        char num[12];
        const char *src;
        size_t len;
        uint32 width = 0;
        sint32 zero;
        if (*fmt != '%' || *++fmt == '%')
        {
            if (!font_put(font, *fmt++))
                goto fail;
            continue;
        }
        zero = *fmt == '0';
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10u + (uint32)(*fmt++ - '0');
        if ((sint32)width <= 0)
            width = 1;
        if (*fmt == 'd' || *fmt == 'x' || *fmt == 'X')
        {
            uint32 value;
            uint32 radix = *fmt == 'd' ? 10u : 16u;
            sint32 negative = 0;
            char *dst = num + sizeof(num);
            if (radix == 10)
            {
                sint32 signed_value = va_arg(args, sint32);
                negative = signed_value < 0;
                value = negative ? 0u - (uint32)signed_value : (uint32)signed_value;
                zero = 0;
            }
            else
                value = va_arg(args, uint32);
            do
            {
                *--dst = hex[value % radix];
                value /= radix;
            } while (value);
            if (negative)
                *--dst = '-';
            src = dst;
            len = (size_t)(num + sizeof(num) - dst);
        }
        else if (*fmt == 'c')
        {
            num[0] = (char)va_arg(args, int);
            src = num;
            len = 1;
            zero = 0;
        }
        else if (*fmt == 's')
        {
            src = va_arg(args, const char *);
            if (!src)
                goto fail;
            len = strlen(src);
            zero = 0;
        }
        else
            goto fail;
        while (width > len)
        {
            if (!font_put(font, zero ? '0' : ' '))
                goto fail;
            --width;
        }
        while (len--)
            if (!font_put(font, *src++))
                goto fail;
        ++fmt;
    }
    va_end(args);
    return font->used;
fail:
    va_end(args);
    return -1;
}

uint32 *FntFlush(sint32 id)
{
    FONT_STATE *font;
    const uint8 *src;
    sint32 x, y, end_x, end_y, max_x = 0;
    sint32 left, idx = 0;
    uint8 r = 128, g = 128, b = 128;

    if (id < 0 || (uint32)id >= font_count)
        id = dump_font;
    if (id < 0 || (uint32)id >= font_count)
        return NULL;
    font = &fonts[id];
    src = (const uint8 *)font->text;
    x = font->bg.p[0].x;
    y = font->bg.p[0].y;
    end_x = x + font->bg.w;
    end_y = y + font->bg.h;
    left = font->cap;
    font->mode.tag |= 0x00FFFFFFu;
    while (*src && left > 0)
    {
        sint32 newline = 0;
        if (*src == '~')
        {
            ++src;
            if (*src == 'c' && src[1] && src[2] && src[3])
            {
                r = (uint8)(16 * ((sint32)src[1] - '0'));
                g = (uint8)(16 * ((sint32)src[2] - '0'));
                b = (uint8)(16 * ((sint32)src[3] - '0'));
                src += 3;
            }
            if (!*src)
                break;
        }
        else if (*src == '\n')
            newline = 1;
        else
        {
            if (*src == '\t')
                x += 32;
            else
            {
                if (*src != ' ')
                {
                    sint32 glyph = *src >= 'a' && *src <= 'z' ? *src - 64 : *src - 32;
                    SPRT_8 *prim = &font->glyphs[idx++];
                    setUV0(prim, 8 * (glyph % 16), 8 * (glyph / 16));
                    setXY0(prim, x, y);
                    setRGB0(prim, r, g, b);
                    AddPrim(&font->mode, prim);
                }
                x += 8;
            }
            if (x >= end_x && !font->auto_size)
                newline = 1;
        }
        if (newline)
        {
            y += 8;
            if (max_x < x)
                max_x = x;
            x = font->bg.p[0].x;
            if (y >= end_y)
                break;
        }
        ++src;
        --left;
    }
    if (font->bg.rgbc.code)
    {
        AddPrim(&font->mode, &font->bg);
        if (font->auto_size)
            setWH(&font->bg, max_x - font->bg.p[0].x, y - (font->bg.p[0].y - 8));
    }
    DrawOTag(&font->mode.tag);
    font->used = 0;
    font->text[0] = 0;
    return &font->mode.tag;
}

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

typedef struct KROM_RANGE
{
    uint16 first;
    uint16 last;
    uint16 index;
} KROM_RANGE;

static const KROM_RANGE krom_normal_ranges[19] = {
    {0x8140, 0x817E, 0x0}, {0x8180, 0x81AC, 0x3F}, {0x81B8, 0x81BF, 0x6C}, {0x81C8, 0x81CE, 0x74}, {0x81DA, 0x81E8, 0x7B}, {0x81F0, 0x81F7, 0x8A}, {0x81FC, 0x81FC, 0x92}, {0x824F, 0x8258, 0x93}, {0x8260, 0x8279, 0x9D}, {0x8281, 0x829A, 0xB7}, {0x829F, 0x82F1, 0xD1}, {0x8340, 0x837E, 0x124}, {0x8380, 0x8396, 0x163}, {0x839F, 0x83B6, 0x17A}, {0x83BF, 0x83D6, 0x192}, {0x8440, 0x8460, 0x1AA}, {0x8470, 0x847E, 0x1CB}, {0x8480, 0x8491, 0x1DA}, {0x849F, 0x84BE, 0x1EC},
};

static const KROM_RANGE krom_extra_ranges[19] = {
    {0x8540, 0x8544, 0x0}, {0x8546, 0x8546, 0x5}, {0x8548, 0x8549, 0x6}, {0x854B, 0x854B, 0x8}, {0x8550, 0x855B, 0x9}, {0x85A0, 0x85AC, 0x15}, {0x85B0, 0x85BC, 0x22}, {0x8640, 0x8641, 0x2F}, {0x8643, 0x8643, 0x31}, {0x8645, 0x8645, 0x32}, {0x8647, 0x8648, 0x33}, {0x864A, 0x864C, 0x35}, {0x864E, 0x864F, 0x38}, {0x8660, 0x866F, 0x3A}, {0x869F, 0x86B6, 0x4A}, {0x86B8, 0x86F5, 0x62}, {0x8740, 0x875A, 0xA0}, {0x875C, 0x8762, 0xBB}, {0x8764, 0x8796, 0xC2},
};

static const uint16 krom_kanji_rows[32][2] = {
    {0x889F, 0x0}, {0x8940, 0x5E}, {0x899F, 0xBC}, {0x8A40, 0x11A}, {0x8A9F, 0x178}, {0x8B40, 0x1D6}, {0x8B9F, 0x234}, {0x8C40, 0x292}, {0x8C9F, 0x2F0}, {0x8D40, 0x34E}, {0x8D9F, 0x3AC}, {0x8E40, 0x40A}, {0x8E9F, 0x468}, {0x8F40, 0x4C6}, {0x8F9F, 0x524}, {0x9040, 0x582}, {0x909F, 0x5E0}, {0x9140, 0x63E}, {0x919F, 0x69C}, {0x9240, 0x6FA}, {0x929F, 0x758}, {0x9340, 0x7B6}, {0x939F, 0x814}, {0x9440, 0x872}, {0x949F, 0x8D0}, {0x9540, 0x92E}, {0x959F, 0x98C}, {0x9640, 0x9EA}, {0x969F, 0xA48}, {0x9740, 0xAA6}, {0x979F, 0xB04}, {0x9840, 0xB62},
};

static const uint8 *krom_pixels;
static uint32 krom_pixel_bytes, krom_first_offset, krom_kanji_offset;

sint32 krom_bind_font(const uint8 *pixels, uint32 bytes, uint32 first_offset, uint32 kanji_offset)
{
    if (!pixels || first_offset < 0x2000 || first_offset > bytes || 524 * 30 > bytes - first_offset || kanji_offset > bytes || 2965 * 30 > bytes - kanji_offset)
        return 0;
    krom_pixels = pixels;
    krom_pixel_bytes = bytes;
    krom_first_offset = first_offset;
    krom_kanji_offset = kanji_offset;
    return 1;
}

static sint32 krom_range_index(uint32 character, const KROM_RANGE *ranges, uint32 count)
{
    uint32 index;
    for (index = 0; index < count; ++index)
        if (character >= ranges[index].first && character <= ranges[index].last)
            return (sint32)(character - ranges[index].first + ranges[index].index);
    return -1;
}

static sint32 krom_character_index(uint32 character)
{
    uint32 high = character >> 8;
    uint32 low = character & 0xFF;
    uint32 row;
    if (high >= 0x81 && high <= 0x84)
        return krom_range_index(character, krom_normal_ranges, 19);
    if (high >= 0x85 && high <= 0x87)
        return krom_range_index(character, krom_extra_ranges, 19);
    if (high >= 0x88 && high <= 0x98 && (high != 0x98 || low < 0x73) && (high != 0x88 || low >= 0x9F) && ((low - 0x40) & 0xFF) < 0xBD && low != 0x7F)
    {
        row = ((high - 0x88) * 2 - (low < 0x7F)) & 0xFF;
        return (sint32)(character - krom_kanji_rows[row][0] + krom_kanji_rows[row][1]);
    }
    return -1;
}

static const uint8 *krom_lookup_glyph(uint32 character, int fallback)
{
    uint32 code = character & 0xFFFF;
    uint32 base, offset;
    sint32 index = -1;
    if (!krom_pixels)
        abort();
    base = krom_first_offset;
    if (((code + 0x7EC0) & 0xFFFF) < 0x37F)
        index = krom_character_index(code);
    else if (((code + 0x7AC0) & 0xFFFF) < 0x257)
    {
        base -= 0x2000;
        index = krom_character_index(code);
    }
    else if (((code + 0x7761) & 0xFFFF) < 0xFD4)
    {
        base = krom_kanji_offset;
        index = krom_character_index(code);
    }
    if (index < 0 && !fallback)
        return NULL;
    offset = index < 0 ? krom_first_offset : base + (uint32)index * 30;
    if (offset > krom_pixel_bytes || 30 > krom_pixel_bytes - offset)
        abort();
    return krom_pixels + offset;
}

const uint8 *Krom2RawAdd2(uint32 character)
{
    return krom_lookup_glyph(character, 1);
}

const uint8 *Krom2RawAdd(uint32 character)
{
    return krom_lookup_glyph(character, 0);
}

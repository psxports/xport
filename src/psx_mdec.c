#include "psx_mdec.h"
#include "psx_press.h"
#include <string.h>
#include <stdlib.h>

/* Default quantization and scale matrices from PsyQ 4.6 LIBPRESS */
const DECDCTENV mdec_default_environment = {{2, 16, 16, 19, 16, 19, 22, 22, 22, 22, 22, 22, 26, 24, 26, 27, 27, 27, 26, 26, 26, 26, 27, 27, 27, 29, 29, 29, 34, 34, 34, 29, 29, 29, 27, 27, 29, 29, 32, 32, 34, 34, 37, 38, 37, 35, 35, 34, 35, 38, 38, 40, 40, 40, 48, 48, 46, 46, 56, 56, 58, 69, 69, 83}, {2, 16, 16, 19, 16, 19, 22, 22, 22, 22, 22, 22, 26, 24, 26, 27, 27, 27, 26, 26, 26, 26, 27, 27, 27, 29, 29, 29, 34, 34, 34, 29, 29, 29, 27, 27, 29, 29, 32, 32, 34, 34, 37, 38, 37, 35, 35, 34, 35, 38, 38, 40, 40, 40, 48, 48, 46, 46, 56, 56, 58, 69, 69, 83}, {23170, 23170,  23170,  23170, 23170, 23170,  23170,  23170, 32138, 27245,  18204, 6392,  -6393,  -18205, -27246, -32139, 30273, 12539,  -12540, -30274, -30274, -12540, 12539,  30273, 27245, -6393,  -32139, -18205, 18204, 32138,  6392,  -27246,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               23170, -23171, -23171, 23170, 23170, -23171, -23171, 23170, 18204, -32139, 6392,  27245, -27246, -6393,  32138,  -18205, 12539, -30274, 30273,  -12540, -12540, 30273,  -30274, 12539, 6392,  -18205, 27245,  -32139, 32138, -27246, 18204, -6393}};

/* Coefficient placement follows the diagonal traversal of an 8 by 8 matrix */
static uint32 mdec_coefficient_address(uint32 ordinal)
{
    uint32 diagonal, preceding = 0u;
    for (diagonal = 0u; diagonal < 15u; ++diagonal)
    {
        uint32 first = diagonal > 7u ? diagonal - 7u : 0u;
        uint32 last = diagonal < 7u ? diagonal : 7u;
        uint32 length = last - first + 1u;
        if (ordinal < preceding + length)
        {
            uint32 within = ordinal - preceding;
            uint32 row = (diagonal & 1u) ? first + within : last - within;
            return (diagonal - row) * 8u + row;
        }
        preceding += length;
    }
    return 63u;
}

static sint32 mdec_signed_ten(uint16 code)
{
    sint32 value = code & 1023u;
    return value >= 512 ? value - 1024 : value;
}

static sint16 mdec_coefficient_limit(sint32 value)
{
    if (value < -16384)
        return -16384;
    if (value > 16383)
        return 16383;
    return (sint16)value;
}

/* Arithmetic division by eight rounds negative values down */
static sint32 mdec_quantized_ac(sint32 value, uint8 factor, uint16 scale)
{
    sint32 numerator = value * (sint32)factor * (sint32)scale;
    sint32 rounded = numerator >= 0 ? numerator / 8 : -((-numerator + 7) / 8);
    return factor && scale ? rounded * 16 + (value > 0 ? -8 : value < 0 ? 8 : 0) : value * 32;
}

void mdec_rle_begin(MdecRleBlock *block)
{
    if (!block)
        return;
    memset(block->coefficients, 0, sizeof(block->coefficients));
    block->position = 64u;
    block->quantization_scale = 0u;
}

sint32 mdec_rle_decode(MdecRleBlock *block, const uint16 *input, uint32 count, uint32 *consumed, const uint8 quantization[64])
{
    uint32 cursor;
    if (!consumed)
        return -1;
    *consumed = 0u;
    if (!block || (!input && count) || !quantization || block->position > 64u || block->quantization_scale > 63u)
        return -1;
    if (block->position == 64u)
        memset(block->coefficients, 0, sizeof(block->coefficients));
    for (cursor = 0u; cursor < count; ++cursor)
    {
        uint16 code = input[cursor];
        sint32 value = mdec_signed_ten(code);
        uint32 destination;
        *consumed = cursor + 1u;
        if (block->position == 64u)
        {
            if (code == 0xFE00u)
                continue;
            block->position = 0u;
            block->quantization_scale = code >> 10;
            value = block->quantization_scale ? value * (sint32)quantization[0] * 16 + (value > 0 ? -8 : value < 0 ? 8 : 0) : value * 32;
        }
        else
        {
            block->position += (code >> 10) + 1u;
            if (block->position >= 64u)
            {
                block->position = 64u;
                return 1;
            }
            value = mdec_quantized_ac(value, quantization[block->position], block->quantization_scale);
        }
        destination = mdec_coefficient_address(block->position);
        block->coefficients[destination] = mdec_coefficient_limit(value);
        if (block->position == 63u)
        {
            block->position = 64u;
            return 1;
        }
    }
    return 0;
}

static sint64 mdec_arithmetic_shift(sint64 value, uint32 bits)
{
    return value >> bits;
}

static sint32 mdec_signed_word(uint32 word)
{
    return word <= 0x7FFFFFFFu ? (sint32)word : -1 - (sint32)~word;
}

static sint16 mdec_idct_value(const sint16 *values, const sint16 *scale, uint32 scale_stride)
{
    uint32 first = 0u, second = 0u, frequency;
    sint64 rounded;
    for (frequency = 0u; frequency < 8u; ++frequency)
    {
        sint32 product = (sint32)values[frequency] * (sint32)scale[frequency * scale_stride];
        if (frequency < 4u)
            first += (uint32)product;
        else
            second += (uint32)product;
    }
    rounded = mdec_arithmetic_shift((sint64)mdec_signed_word(first) + mdec_signed_word(second) + 131072, 18u);
    return (sint16)rounded;
}

const sint16 mdec_standard_scale[64] = {23170, 23170, 23170, 23170, 23170, 23170, 23170, 23170, 32138, 27245, 18204, 6392, -6393, -18205, -27246, -32139, 30273, 12539, -12540, -30274, -30274, -12540, 12539, 30273, 27245, -6393, -32139, -18205, 18204, 32138, 6392, -27246, 23170, -23171, -23171, 23170, 23170, -23171, -23171, 23170, 18204, -32139, 6392, 27245, -27246, -6393, 32138, -18205, 12539, -30274, 30273, -12540, -12540, 30273, -30274, 12539, 6392, -18205, 27245, -32139, 32138, -27246, 18204, -6393};

static void mdec_idct_matrix_1d(const sint16 values[8], sint16 output[8], const sint16 scale[64])
{
    uint32 coordinate;
    for (coordinate = 0u; coordinate < 8u; ++coordinate)
        output[coordinate] = mdec_idct_value(values, scale + coordinate, 8u);
}

static sint16 mdec_idct_accumulated(uint32 first, uint32 second)
{
    return (sint16)mdec_arithmetic_shift((sint64)mdec_signed_word(first) + mdec_signed_word(second) + 131072, 18u);
}

static void mdec_idct_standard_1d(const sint16 values[8], sint16 output[8])
{
    uint32 coordinate, frequency;
    for (coordinate = 0u; coordinate < 4u; ++coordinate)
    {
        uint32 first = 0u, second = 0u, mirror_first = 0u, mirror_second = 0u;
        for (frequency = 0u; frequency < 8u; ++frequency)
        {
            sint32 source = values[frequency];
            sint32 scale = mdec_standard_scale[frequency * 8u + coordinate];
            sint32 mirror_scale = mdec_standard_scale[frequency * 8u + 7u - coordinate];
            sint32 product = source * scale;
            sint32 mirror_product = frequency & 1u ? -product : product;
            sint32 correction = mirror_scale - (frequency & 1u ? -scale : scale);
            if (correction > 0)
                mirror_product += source;
            else if (correction < 0)
                mirror_product -= source;
            if (frequency < 4u)
            {
                first += (uint32)product;
                mirror_first += (uint32)mirror_product;
            }
            else
            {
                second += (uint32)product;
                mirror_second += (uint32)mirror_product;
            }
        }
        output[coordinate] = mdec_idct_accumulated(first, second);
        output[7u - coordinate] = mdec_idct_accumulated(mirror_first, mirror_second);
    }
}

static void mdec_idct_aan_1d(const sint16 values[8], sint16 output[8])
{
    static const sint32 prescale[8] = {1482880, 2056848, 1937500, 1743712, 1482912, 1165111, 802539, 409133};
    sint64 value[8], result[8], z10, z11, z12, z13, z5, tmp0, tmp1, tmp2, tmp3, tmp4, tmp5, tmp6, tmp7;
    sint16 exact[8];
    uint8 uncertain[8];
    uint32 coordinate, frequency, nonzero = 0u, nonzero_frequency = 0u, absolute_sum = 0u, uncertain_count = 0u;
    sint64 error;
    for (frequency = 0u; frequency < 8u; ++frequency)
    {
        sint32 source = values[frequency];
        if (source)
        {
            ++nonzero;
            nonzero_frequency = frequency;
        }
        absolute_sum += (uint32)(source < 0 ? -source : source);
    }
    if (!nonzero)
    {
        memset(output, 0, 8u * sizeof(*output));
        return;
    }
    if (nonzero == 1u)
    {
        for (coordinate = 0u; coordinate < 8u; ++coordinate)
        {
            sint64 product = (sint64)values[nonzero_frequency] * mdec_standard_scale[nonzero_frequency * 8u + coordinate];
            output[coordinate] = (sint16)mdec_arithmetic_shift(product + 131072, 18u);
        }
        return;
    }
    error = ((sint64)absolute_sum << 6) + 256;
    if (error >= 8388608)
    {
        mdec_idct_standard_1d(values, output);
        return;
    }
    for (frequency = 0u; frequency < 8u; ++frequency)
        value[frequency] = (sint64)values[frequency] * prescale[frequency];
    z10 = value[0] + value[4];
    z11 = value[0] - value[4];
    z13 = value[2] + value[6];
    z12 = mdec_arithmetic_shift((value[2] - value[6]) * 5931642, 22u) - z13;
    tmp0 = z10 + z13;
    tmp3 = z10 - z13;
    tmp1 = z11 + z12;
    tmp2 = z11 - z12;
    z13 = value[3] + value[5];
    z10 = value[3] - value[5];
    z11 = value[1] + value[7];
    z12 = value[1] - value[7];
    tmp7 = z11 + z13;
    z5 = (z12 - z10) * 7750063;
    tmp6 = mdec_arithmetic_shift(z10 * 10960245 + z5, 22u) - tmp7;
    tmp5 = mdec_arithmetic_shift((z11 - z13) * 5931642, 22u) - tmp6;
    tmp4 = mdec_arithmetic_shift(z12 * 4539882 - z5, 22u) + tmp5;
    result[0] = tmp0 + tmp7;
    result[1] = tmp1 + tmp6;
    result[2] = tmp2 + tmp5;
    result[3] = tmp3 - tmp4;
    result[4] = tmp3 + tmp4;
    result[5] = tmp2 - tmp5;
    result[6] = tmp1 - tmp6;
    result[7] = tmp0 - tmp7;
    for (coordinate = 0u; coordinate < 8u; ++coordinate)
    {
        sint64 rounded = mdec_arithmetic_shift(result[coordinate] + 8388608, 24u);
        sint64 center = rounded * 16777216;
        output[coordinate] = (sint16)rounded;
        uncertain[coordinate] = (uint8)(result[coordinate] - error < center - 8388608 || result[coordinate] + error >= center + 8388608);
        uncertain_count += uncertain[coordinate];
    }
    if (uncertain_count > 4u)
    {
        mdec_idct_standard_1d(values, exact);
        for (coordinate = 0u; coordinate < 8u; ++coordinate)
            if (uncertain[coordinate])
                output[coordinate] = exact[coordinate];
    }
    else
    {
        for (coordinate = 0u; coordinate < 8u; ++coordinate)
            if (uncertain[coordinate])
                output[coordinate] = mdec_idct_value(values, mdec_standard_scale + coordinate, 8u);
    }
}

static sint32 mdec_signed_nine(sint32 value)
{
    uint32 bits = (uint32)value & 511u;
    return bits < 256u ? (sint32)bits : (sint32)bits - 512;
}

static sint32 mdec_pixel_limit(sint32 value)
{
    return value < -128 ? -128 : value > 127 ? 127 : value;
}

sint32 mdec_idct(sint16 coefficients[64], const sint16 scale[64])
{
    sint16 intermediate[64], input[8], output[8];
    uint32 x, y, frequency, standard;
    if (!coefficients || !scale)
        return -1;
    standard = !memcmp(scale, mdec_standard_scale, sizeof(mdec_standard_scale));
    for (x = 0u; x < 8u; ++x)
    {
        for (frequency = 0u; frequency < 8u; ++frequency)
            input[frequency] = coefficients[x * 8u + frequency];
        if (standard)
            mdec_idct_aan_1d(input, output);
        else
            mdec_idct_matrix_1d(input, output, scale);
        for (y = 0u; y < 8u; ++y)
            intermediate[y * 8u + x] = output[y];
    }
    for (y = 0u; y < 8u; ++y)
    {
        for (frequency = 0u; frequency < 8u; ++frequency)
            input[frequency] = intermediate[y * 8u + frequency];
        if (standard)
            mdec_idct_aan_1d(input, output);
        else
            mdec_idct_matrix_1d(input, output, scale);
        for (x = 0u; x < 8u; ++x)
            coefficients[y * 8u + x] = (sint16)mdec_pixel_limit(mdec_signed_nine(output[x]));
    }
    return 0;
}

#if defined(XPORT_MDEC_TESTING)
void mdec_idct_test_1d(const sint16 values[8], sint16 optimized[8], sint16 reference[8])
{
    mdec_idct_aan_1d(values, optimized);
    mdec_idct_matrix_1d(values, reference, mdec_standard_scale);
}

void mdec_idct_test_reference(sint16 coefficients[64], const sint16 scale[64])
{
    sint16 intermediate[64], input[8], output[8];
    uint32 x, y, frequency;
    for (x = 0u; x < 8u; ++x)
    {
        for (frequency = 0u; frequency < 8u; ++frequency)
            input[frequency] = coefficients[x * 8u + frequency];
        mdec_idct_matrix_1d(input, output, scale);
        for (y = 0u; y < 8u; ++y)
            intermediate[y * 8u + x] = output[y];
    }
    for (y = 0u; y < 8u; ++y)
    {
        mdec_idct_matrix_1d(intermediate + y * 8u, output, scale);
        for (x = 0u; x < 8u; ++x)
            coefficients[y * 8u + x] = (sint16)mdec_pixel_limit(mdec_signed_nine(output[x]));
    }
}
#endif

sint32 mdec_color(uint32 pixels[256], const sint16 blocks[384], uint32 signed_output)
{
    uint32 x, y;
    if (!pixels || !blocks)
        return -1;
    for (x = 0u; x < 384u; ++x)
        if (blocks[x] < -128 || blocks[x] > 127)
            return -1;
    for (y = 0u; y < 16u; ++y)
        for (x = 0u; x < 16u; ++x)
        {
            uint32 chroma = (y / 2u) * 8u + x / 2u;
            uint32 luminance = (2u + (y / 8u) * 2u + x / 8u) * 64u + (y & 7u) * 8u + (x & 7u);
            sint32 cr = blocks[chroma], cb = blocks[64u + chroma], brightness = blocks[luminance];
            sint32 red = (sint32)mdec_arithmetic_shift(359 * cr + 128, 8u);
            sint32 blue = (sint32)mdec_arithmetic_shift(454 * cb + 128, 8u);
            sint32 green = (sint32)mdec_arithmetic_shift(mdec_arithmetic_shift(-88 * cb, 5u) * 32 + mdec_arithmetic_shift(-183 * cr, 3u) * 8 + 128, 8u);
            uint32 bias = signed_output ? 0u : 128u;
            uint32 r = ((uint32)mdec_pixel_limit(mdec_signed_nine(brightness + red)) + bias) & 255u;
            uint32 g = ((uint32)mdec_pixel_limit(mdec_signed_nine(brightness + green)) + bias) & 255u;
            uint32 b = ((uint32)mdec_pixel_limit(mdec_signed_nine(brightness + blue)) + bias) & 255u;
            pixels[y * 16u + x] = r | (g << 8) | (b << 16);
        }
    return 0;
}

static void mdec_status(MdecState *s)
{
    uint32 flags = s->status & 0x07800000u;
    flags |= ((s->remaining_halfwords / 2u - 1u) & 65535u);
    flags |= ((s->current_block + 4u) % 6u) << 16;
    if (!s->output_count)
        flags |= 0x80000000u;
    if (s->input_count == 512u)
        flags |= 0x40000000u;
    if (s->phase)
        flags |= 0x20000000u;
    if (s->dma_in && s->input_count <= 448u)
        flags |= 0x10000000u;
    if (s->dma_out && s->output_count)
        flags |= 0x08000000u;
    s->status = flags;
}

static void mdec_decoder_reset(MdecState *s)
{
    s->current_block = 0u;
    s->current_coefficient = 64u;
    s->quantization_scale = 0u;
}

static uint16 mdec_input_pop(MdecState *s)
{
    uint16 value = s->input[s->input_head];
    s->input_head = (s->input_head + 1u) % 512u;
    --s->input_count;
    return value;
}

static sint32 mdec_decode_block(MdecState *s, uint32 block)
{
    MdecRleBlock rle;
    uint16 input[512];
    uint32 i, consumed, available = s->input_count;
    sint32 complete;
    if (available > s->remaining_halfwords)
        available = s->remaining_halfwords;
    for (i = 0u; i < available; ++i)
        input[i] = s->input[(s->input_head + i) % 512u];
    memcpy(rle.coefficients, s->blocks + block * 64u, sizeof(rle.coefficients));
    rle.position = s->current_coefficient;
    rle.quantization_scale = (uint16)s->quantization_scale;
    complete = mdec_rle_decode(&rle, input, available, &consumed, block >= 2u || ((s->status >> 25) & 3u) < 2u ? s->quantization_y : s->quantization_uv);
    if (complete < 0)
        abort();
    s->input_head = (s->input_head + consumed) % 512u;
    s->input_count -= consumed;
    s->remaining_halfwords -= consumed;
    s->current_coefficient = rle.position;
    s->quantization_scale = rle.quantization_scale;
    memcpy(s->blocks + block * 64u, rle.coefficients, sizeof(rle.coefficients));
    if (complete && mdec_idct(s->blocks + block * 64u, s->scale))
        abort();
    return complete;
}

static sint32 mdec_macroblock(MdecState *s)
{
    uint32 depth = (s->status >> 25) & 3u;
    uint32 signed_output = (s->status >> 24) & 1u, i;
    if (depth < 2u)
    {
        if (s->output_count || !mdec_decode_block(s, 0u))
            return 0;
        for (i = 0u; i < 64u; ++i)
            s->pixels[i] = (uint32)(mdec_pixel_limit(mdec_signed_nine(s->blocks[i])) + (signed_output ? 0 : 128));
    }
    else
    {
        while (s->current_block < 6u)
        {
            if (!mdec_decode_block(s, s->current_block))
                return 0;
            ++s->current_block;
        }
        if (s->output_count)
            return 0;
        if (mdec_color(s->pixels, s->blocks, signed_output))
            abort();
    }
    mdec_decoder_reset(s);
    s->phase = 2u;
    s->copy_ticks = 2688u;
    return 1;
}

static void mdec_execute(MdecState *s)
{
    for (;;)
    {
        uint32 i, command, kind;
        if (!s->phase)
        {
            if (s->input_count < 2u)
                break;
            command = mdec_input_pop(s);
            command |= (uint32)mdec_input_pop(s) << 16;
            s->status = (command >> 2) & 0x07800000u;
            s->output_head = s->output_count = 0u;
            kind = command >> 29;
            s->remaining_halfwords = (command & 65535u) * 2u;
            s->phase = kind == 1u ? 1u : kind == 2u ? 3u : kind == 3u ? 4u : 5u;
            if (kind == 2u)
                s->remaining_halfwords = command & 1u ? 64u : 32u;
            if (kind == 3u)
                s->remaining_halfwords = 64u;
            continue;
        }
        if (s->phase == 1u)
        {
            if (mdec_macroblock(s))
                break;
            if (s->remaining_halfwords || s->current_block == 6u)
                break;
            mdec_decoder_reset(s);
            s->phase = 0u;
            continue;
        }
        if (s->phase == 2u)
            break;
        if (s->phase == 3u || s->phase == 4u)
        {
            uint32 needed = s->remaining_halfwords;
            if (s->input_count < needed)
                break;
            for (i = 0u; i < needed; ++i)
            {
                uint16 value = mdec_input_pop(s);
                if (s->phase == 4u)
                    s->scale[i] = value < 32768u ? (sint16)value : (sint16)((sint32)value - 65536);
                else
                {
                    uint8 *table = i < 32u ? s->quantization_y : s->quantization_uv;
                    table[(i % 32u) * 2u] = (uint8)value;
                    table[(i % 32u) * 2u + 1u] = (uint8)(value >> 8);
                }
            }
            s->remaining_halfwords = s->phase = 0u;
            continue;
        }
        if (s->phase != 5u)
            abort();
        i = s->input_count < s->remaining_halfwords ? s->input_count : s->remaining_halfwords;
        s->input_head = (s->input_head + i) % 512u;
        s->input_count -= i;
        s->remaining_halfwords -= i;
        if (s->remaining_halfwords)
            break;
        s->phase = 0u;
    }
    mdec_status(s);
}

static uint32 mdec_rgb_fifteen(uint32 rgb, uint32 high_bit)
{
    uint32 channel, value = high_bit << 15;
    for (channel = 0u; channel < 3u; ++channel)
    {
        uint32 component = (((rgb >> (channel * 8u)) & 255u) + 4u) / 8u;
        if (component > 31u)
            component = 31u;
        value |= component << (channel * 5u);
    }
    return value;
}

static void mdec_copy_output(MdecState *s)
{
    uint32 depth = (s->status >> 25) & 3u, i;
    if (s->output_count || s->phase != 2u)
        abort();
    s->output_head = 0u;
    s->copy_ticks = 0u;
    if (depth < 2u)
    {
        uint32 per_word = depth ? 4u : 8u;
        uint32 bits = depth ? 8u : 4u;
        for (i = 0u; i < 64u; ++i)
        {
            uint32 word = i / per_word;
            uint32 value = depth ? s->pixels[i] : s->pixels[i] >> 4;
            if (!(i % per_word))
                s->output[word] = 0u;
            s->output[word] |= value << ((i % per_word) * bits);
        }
        s->output_count = 64u / per_word;
    }
    else if (depth == 2u)
    {
        memset(s->output, 0, sizeof(s->output));
        for (i = 0u; i < 768u; ++i)
        {
            uint32 value = (s->pixels[i / 3u] >> ((i % 3u) * 8u)) & 255u;
            s->output[i / 4u] |= value << ((i % 4u) * 8u);
        }
        s->output_count = 192u;
    }
    else
    {
        for (i = 0u; i < 256u; i += 2u)
        {
            uint32 a = mdec_rgb_fifteen(s->pixels[i], (s->status >> 23) & 1u);
            uint32 b = mdec_rgb_fifteen(s->pixels[i + 1u], (s->status >> 23) & 1u);
            s->output[i / 2u] = a | (b << 16);
        }
        s->output_count = 128u;
    }
    s->phase = s->remaining_halfwords ? 1u : 0u;
    mdec_execute(s);
}

void mdec_control(MdecState *s, uint32 control)
{
    if (!s)
        return;
    if (control & 0x80000000u)
    {
        s->status = s->phase = s->remaining_halfwords = s->copy_ticks = 0u;
        s->input_head = s->input_count = s->output_head = s->output_count = 0u;
        mdec_decoder_reset(s);
    }
    s->dma_in = (control >> 30) & 1u;
    s->dma_out = (control >> 29) & 1u;
    mdec_execute(s);
}

void mdec_initialize(MdecState *s)
{
    if (!s)
        return;
    memset(s, 0, sizeof(*s));
    mdec_control(s, 0x80000000u);
}

uint32 mdec_write_data(MdecState *s, uint32 value)
{
    uint32 tail;
    if (!s || s->input_count > 510u)
        return 0u;
    tail = (s->input_head + s->input_count) % 512u;
    s->input[tail] = (uint16)value;
    s->input[(tail + 1u) % 512u] = (uint16)(value >> 16);
    s->input_count += 2u;
    mdec_execute(s);
    return 2u;
}

uint32 mdec_dma_write(MdecState *s, const uint32 *words, uint32 count)
{
    uint32 available, accepted, i;
    if (!s || (!words && count))
        return 0u;
    available = (512u - s->input_count) & ~2u;
    accepted = count > 256u ? available : count * 2u;
    if (accepted > available)
        accepted = available;
    for (i = 0u; i < accepted; ++i)
        s->input[(s->input_head + s->input_count + i) % 512u] = (uint16)(words[i / 2u] >> ((i % 2u) * 16u));
    s->input_count += accepted;
    mdec_execute(s);
    return accepted;
}

uint32 mdec_dma_read(MdecState *s, uint32 *words, uint32 count)
{
    uint32 i, available;
    if (!s || (!words && count))
        return 0u;
    available = count < s->output_count ? count : s->output_count;
    for (i = 0u; i < available; ++i)
        words[i] = s->output[(s->output_head + i) % 192u];
    s->output_head = (s->output_head + available) % 192u;
    s->output_count -= available;
    if (!s->output_count)
        mdec_execute(s);
    return available;
}

sint32 mdec_read_data(MdecState *s, uint32 *value, uint32 *stall_ticks)
{
    if (!s || !value || !stall_ticks)
        return -1;
    *stall_ticks = 0u;
    if (!s->output_count)
    {
        if (s->copy_ticks)
        {
            *stall_ticks = s->copy_ticks;
            return 0;
        }
        *value = 0xFFFFFFFFu;
        return 1;
    }
    *value = s->output[s->output_head];
    s->output_head = (s->output_head + 1u) % 192u;
    --s->output_count;
    if (!s->output_count)
        mdec_execute(s);
    else
        mdec_status(s);
    return 1;
}

void mdec_advance(MdecState *s, uint32 ticks)
{
    if (!s)
        return;
    while (s->copy_ticks && ticks >= s->copy_ticks)
    {
        ticks -= s->copy_ticks;
        mdec_copy_output(s);
    }
    if (s->copy_ticks)
        s->copy_ticks -= ticks;
}

sint32 mdec_state_valid(const MdecState *s)
{
    return s && s->input_count <= 512u && s->input_head < 512u && s->output_count <= 192u && s->output_head < 192u && s->phase <= 5u && s->remaining_halfwords <= 131070u && s->current_block <= 6u && s->current_coefficient <= 64u && s->quantization_scale <= 63u && s->dma_in <= 1u && s->dma_out <= 1u && s->copy_ticks <= 2688u && ((s->phase == 2u) == (s->copy_ticks != 0u)) && (s->phase != 3u || s->remaining_halfwords == 32u || s->remaining_halfwords == 64u) && (s->phase != 4u || s->remaining_halfwords == 64u);
}

static sint32 mdec_io_integer(FILE *file, uint32 *value, uint32 width, sint32 load)
{
    uint8 bytes[4];
    uint32 i;
    if (load)
    {
        if (fread(bytes, 1, width, file) != width)
            return 0;
        *value = 0u;
        for (i = 0u; i < width; ++i)
            *value |= (uint32)bytes[i] << (i * 8u);
    }
    else
    {
        for (i = 0u; i < width; ++i)
            bytes[i] = (uint8)(*value >> (i * 8u));
        if (fwrite(bytes, 1, width, file) != width)
            return 0;
    }
    return 1;
}

static sint32 mdec_io_signed(FILE *file, sint16 *values, uint32 count, sint32 load)
{
    uint32 i;
    for (i = 0u; i < count; ++i)
    {
        uint32 value = (uint16)values[i];
        if (!mdec_io_integer(file, &value, 2u, load))
            return 0;
        if (load)
            values[i] = value < 32768u ? (sint16)value : (sint16)((sint32)value - 65536);
    }
    return 1;
}

sint32 mdec_state_io(FILE *file, MdecState *state, sint32 load)
{
    MdecState candidate;
    uint32 magic = 0x3143444Du, version = 1u, i;
    if (!file || !state || (!load && !mdec_state_valid(state)))
        return 0;
    if (load)
        memset(&candidate, 0, sizeof(candidate));
    else
        candidate = *state;
    if (!mdec_io_integer(file, &magic, 4u, load) || !mdec_io_integer(file, &version, 4u, load) || magic != 0x3143444Du || version != 1u || !mdec_io_integer(file, &candidate.status, 4u, load) || !mdec_io_integer(file, &candidate.dma_in, 4u, load) || !mdec_io_integer(file, &candidate.dma_out, 4u, load) || !mdec_io_integer(file, &candidate.input_count, 4u, load) || candidate.input_count > 512u)
        return 0;
    for (i = 0u; i < candidate.input_count; ++i)
    {
        uint32 value = candidate.input[(candidate.input_head + i) % 512u];
        if (!mdec_io_integer(file, &value, 2u, load))
            return 0;
        if (load)
            candidate.input[i] = (uint16)value;
    }
    if (!mdec_io_integer(file, &candidate.output_count, 4u, load) || candidate.output_count > 192u)
        return 0;
    for (i = 0u; i < candidate.output_count; ++i)
    {
        uint32 value = candidate.output[(candidate.output_head + i) % 192u];
        if (!mdec_io_integer(file, &value, 4u, load))
            return 0;
        if (load)
            candidate.output[i] = value;
    }
    if (!mdec_io_integer(file, &candidate.phase, 4u, load) || !mdec_io_integer(file, &candidate.remaining_halfwords, 4u, load))
        return 0;
    if (load)
    {
        if (fread(candidate.quantization_y, 1, 64, file) != 64 || fread(candidate.quantization_uv, 1, 64, file) != 64)
            return 0;
    }
    else if (fwrite(candidate.quantization_y, 1, 64, file) != 64 || fwrite(candidate.quantization_uv, 1, 64, file) != 64)
        return 0;
    if (!mdec_io_signed(file, candidate.scale, 64u, load) || !mdec_io_signed(file, candidate.blocks, 384u, load) || !mdec_io_integer(file, &candidate.current_block, 4u, load) || !mdec_io_integer(file, &candidate.current_coefficient, 4u, load) || !mdec_io_integer(file, &candidate.quantization_scale, 4u, load))
        return 0;
    for (i = 0u; i < 256u; ++i)
        if (!mdec_io_integer(file, candidate.pixels + i, 4u, load))
            return 0;
    if (!mdec_io_integer(file, &candidate.copy_ticks, 4u, load) || !mdec_state_valid(&candidate))
        return 0;
    if (load)
        *state = candidate;
    return 1;
}

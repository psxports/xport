#include "psx_press.h"
#include "psx_vlc_table.inc"

typedef struct VlcState
{
    const uint16 *source;
    uint16 *output;
    uint32 bits, consumed, quantization, block, predictor[3];
} VlcState;

typedef struct VlcDecoder
{
    VlcState state;
    uint16 *end;
    uint32 limit;
} VlcDecoder;

static VlcDecoder default_decoder = {{NULL, NULL, 0, 0, 0, 0, {0, 0, 0}}, NULL, 0xFFFFFFFFu};
#if MDEC_CUSTOM
static VlcDecoder table_decoder = {{NULL, NULL, 0, 0, 0, 0, {0, 0, 0}}, NULL, 0xFFFFFFFFu};
#endif

static uint32 vlc_ac_token(uint32 bits)
{
    uint32 zeros = 0, shift, index, coefficient, length;
    if (bits & 0x80000000u)
        return bits & 0x40000000u ? ((bits & 0x20000000u ? 0x03FFu : 1u) << 16) | 3u : 0xFE000002u;
    while (zeros < 17u && !(bits & (0x80000000u >> zeros)))
        ++zeros;
    if (zeros == 5u)
        return 0x7C1F0006u;
    if (zeros > 11u)
        return 0u;
    shift = zeros < 6u ? 1u : zeros < 9u ? 6u : 9u;
    index = vlc_ac_lookup[(shift == 1u ? 0u : shift == 6u ? 256u : 512u) + ((bits >> (24u - shift)) & 255u)];
    if (index == 255u)
        return 0u;
    coefficient = vlc_ac[index].coefficient;
    length = vlc_ac[index].length;
    if (bits & (1u << (32u - length)))
        coefficient = (coefficient & 0xFC00u) | ((0u - coefficient) & 0x03FFu);
    return (coefficient << 16) | length;
}

/* Preserve the SDK batching and bit consumption at continuation boundaries */
static uint32 vlc_ac_entry(uint32 bits, uint32 *additional)
{
    uint32 first = 0, used = 0, token, coefficient, i;
    *additional = 0;
    for (i = 0; i < 3u; ++i)
    {
        token = vlc_ac_token(bits << used);
        if (!token || used + (token & 255u) > 13u)
            break;
        used += token & 255u;
        coefficient = token >> 16;
        if (!i)
            first = token & 0xFFFF0000u;
        else
            *additional |= coefficient << ((i - 1u) * 16u);
        if (coefficient == 0xFE00u || coefficient == 0x7C1Fu)
            break;
    }
    return first | used;
}

static uint16 vlc_read16(const void *pointer)
{
    const uint8 *p = (const uint8 *)pointer;
    return (uint16)((uint16)p[0] | (uint16)p[1] << 8);
}

static uint32 vlc_read32(const void *pointer)
{
    const uint8 *p = (const uint8 *)pointer;
    return (uint32)vlc_read16(p) | (uint32)vlc_read16(p + 2) << 16;
}

static void vlc_write16(void *pointer, uint16 value)
{
    uint8 *p = (uint8 *)pointer;
    p[0] = (uint8)value;
    p[1] = (uint8)(value >> 8);
}

static void vlc_write32(void *pointer, uint32 value)
{
    vlc_write16(pointer, (uint16)value);
    vlc_write16((uint8 *)pointer + 2, (uint16)(value >> 16));
}

static void vlc_consume(VlcState *state, uint32 count)
{
    uint32 total = state->consumed + count;
    state->bits <<= count & 31u;
    state->consumed = total & 15u;
    if (total & 16u)
    {
        state->bits |= (uint32)vlc_read16(state->source++) << state->consumed;
    }
}

static void vlc_escape(VlcState *state)
{
    vlc_write16(state->output++, (uint16)(state->bits >> 16));
    state->bits = (state->bits << 16) | (uint32)vlc_read16(state->source++) << state->consumed;
}

static int vlc_decode(VlcDecoder *decoder, const uint32 *source, uint32 *destination, const uint16 *table)
{
    VlcState state;
    uint16 *chunk_limit;
#if MDEC_CUSTOM
    const uint16 *table_base = table ? table + 1024u : NULL;
    const uint16 *dc_table;
#else
    (void)table;
#endif
    uint32 first_code, additional, coefficient, count, index;
    uint32 dc_prefix, dc_width, delta, leading, header;
    if (source)
    {
        state.block = 0u;
        state.predictor[0] = state.predictor[1] = state.predictor[2] = 0u;
        chunk_limit = decoder->limit == 0xFFFFFFFFu ? NULL : (uint16 *)destination + decoder->limit;
        header = vlc_read32(source);
        state.quantization = (uint32)vlc_read16((const uint8 *)source + 4u) << 10;
        if (vlc_read16((const uint8 *)source + 6u) >= 3u)
            state.block = 1u;
        state.bits = ((uint32)vlc_read16((const uint8 *)source + 8u) << 16) | vlc_read16((const uint8 *)source + 10u);
        state.source = (const uint16 *)((const uint8 *)source + 12u);
        state.consumed = 0u;
        vlc_write32(destination, header);
        decoder->end = (uint16 *)destination + 2u * (header & 65535u) + 2u;
        state.output = (uint16 *)destination + 1u;
        goto decode_dc;
    }
    state = decoder->state;
    chunk_limit = decoder->limit == 0xFFFFFFFFu ? NULL : state.output + decoder->limit;

decode_ac:
#if MDEC_CUSTOM
    if (table)
    {
        index = 4u * (state.bits >> 19);
        first_code = vlc_read32(table_base + index);
        if (first_code)
            additional = vlc_read32(table_base + index + 2u);
        else
        {
            vlc_consume(&state, 8u);
            first_code = vlc_read32(table_base + 32768u + 2u * (state.bits >> 23));
            additional = 0u;
        }
    }
    else
#endif
    {
        first_code = vlc_ac_entry(state.bits, &additional);
        if (!first_code)
        {
            first_code = vlc_ac_token(state.bits);
            vlc_consume(&state, 8u);
            if (first_code)
                first_code -= 8u;
            additional = 0u;
        }
    }
    vlc_consume(&state, first_code & 255u);
    coefficient = first_code >> 16;
    if (coefficient == 0x7C1Fu)
    {
        vlc_escape(&state);
        goto decode_ac;
    }
    vlc_write16(state.output, (uint16)coefficient);
    if (coefficient == 0xFE00u)
        goto decode_dc;
    state.output += 1u;
    if (!additional)
        goto decode_ac;
    coefficient = additional & 65535u;
    if (coefficient == 0x7C1Fu)
    {
        vlc_escape(&state);
        goto decode_ac;
    }
    vlc_write16(state.output, (uint16)coefficient);
    if (coefficient == 0xFE00u)
        goto decode_dc;
    state.output += 1u;
    coefficient = additional >> 16;
    if (!coefficient)
        goto decode_ac;
    if (coefficient == 0x7C1Fu)
    {
        vlc_escape(&state);
        goto decode_ac;
    }
    vlc_write16(state.output, (uint16)coefficient);
    if (coefficient == 0xFE00u)
        goto decode_dc;
    state.output += 1u;
    goto decode_ac;

decode_dc:
    coefficient = state.bits >> 22;
    state.output += 1u;
    if (coefficient == (state.block ? 0x3FFu : 0x1FFu))
        goto finished;
    if (state.block)
    {
#if MDEC_CUSTOM
        if (table)
        {
            dc_table = table_base - (state.block >= 3u ? 1024u : 512u);
            index = 2u * (state.bits >> 24);
            dc_prefix = vlc_read16(dc_table + index);
            dc_width = vlc_read16(dc_table + index + 1u);
        }
        else
#endif
        {
            index = (state.block >= 3u ? 0u : 256u) + (state.bits >> 24);
            dc_prefix = vlc_dc[index] >> 4;
            dc_width = vlc_dc[index] & 15u;
        }
        state.bits <<= dc_prefix & 31u;
        delta = 0u;
        count = state.consumed;
        if (dc_width)
        {
            delta = state.bits >> ((32u - dc_width) & 31u);
            leading = state.bits & 0x80000000u;
            state.bits <<= dc_width & 31u;
            if (!leading)
                delta -= 0xFFFFFFFFu >> ((32u - dc_width) & 31u);
            count += dc_width;
        }
        count += dc_prefix;
        state.consumed = count & 15u;
        if (count & 16u)
        {
            state.bits |= (uint32)vlc_read16(state.source) << state.consumed;
            state.source += 1u;
        }
        index = state.block > 2u ? 2u : state.block - 1u;
        state.predictor[index] += delta;
        coefficient = state.quantization | ((state.predictor[index] << 2) & 1023u);
        ++state.block;
        if (state.block == 7u)
            state.block = 1u;
    }
    else
    {
        vlc_consume(&state, 10u);
        coefficient |= state.quantization;
    }
    vlc_write16(state.output, (uint16)coefficient);
    if (chunk_limit && state.output >= chunk_limit)
    {
        state.output += 1u;
        decoder->state = state;
        return 1;
    }
    state.output += 1u;
    goto decode_ac;

finished:
    chunk_limit = decoder->end;
    while (state.output < chunk_limit)
    {
        vlc_write16(state.output, 0xFE00u);
        state.output += 1u;
    }
    /* Native C does not require the original CP0 cache-control write */
    return 0u;
}

int DecDCTvlc(uint32 *bs, uint32 *buf)
{
    return vlc_decode(&default_decoder, bs, buf, NULL);
}

#if MDEC_CUSTOM
int DecDCTvlc2(uint32 *bs, uint32 *buf, DECDCTTAB table)
{
    return vlc_decode(&table_decoder, bs, buf, table);
}
#endif

static int vlc_set_size(VlcDecoder *decoder, int size)
{
    uint32 previous = decoder->limit;
    decoder->limit = size < 2 ? 0xFFFFFFFFu : (uint32)size << 1;
    return (int)previous;
}

int DecDCTvlcSize(int size)
{
    return vlc_set_size(&default_decoder, size);
}

#if MDEC_CUSTOM
int DecDCTvlcSize2(int size)
{
    return vlc_set_size(&table_decoder, size);
}

void DecDCTvlcBuild(uint16 *table)
{
    uint32 index, first, additional;
    for (index = 0; index < 512u; ++index)
    {
        vlc_write16(table + index * 2u, vlc_dc[index] >> 4);
        vlc_write16(table + index * 2u + 1u, vlc_dc[index] & 15u);
    }
    for (index = 0; index < 8192u; ++index)
    {
        first = vlc_ac_entry(index << 19, &additional);
        vlc_write32(table + 1024u + index * 4u, first);
        vlc_write32(table + 1026u + index * 4u, additional);
    }
    for (index = 0; index < 512u; ++index)
    {
        first = vlc_ac_token(index << 15);
        if (first)
            first -= 8u;
        vlc_write32(table + 33792u + index * 2u, first);
    }
}
#endif

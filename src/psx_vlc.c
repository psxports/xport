#include "psx_press.h"
#include <string.h>
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

static VlcDecoder default_decoder = {{0}, NULL, 0xFFFFFFFFu};
static VlcDecoder table_decoder = {{0}, NULL, 0xFFFFFFFFu};

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
    const uint16 *table_base = table + 1024u;
    uint32 first_code, additional, coefficient, count, index;
    const uint16 *dc_table;
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
        dc_table = table_base - (state.block >= 3u ? 1024u : 512u);
        index = 2u * (state.bits >> 24);
        dc_prefix = vlc_read16(dc_table + index);
        dc_width = vlc_read16(dc_table + index + 1u);
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
    return vlc_decode(&default_decoder, bs, buf, psyq_vlc_table);
}

int DecDCTvlc2(uint32 *bs, uint32 *buf, DECDCTTAB table)
{
    return vlc_decode(&table_decoder, bs, buf, table);
}

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

int DecDCTvlcSize2(int size)
{
    return vlc_set_size(&table_decoder, size);
}

void DecDCTvlcBuild(uint16 *table)
{
    memcpy(table, psyq_vlc_table, sizeof(psyq_vlc_table));
}

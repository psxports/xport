#include "psx_xa.h"
#include <string.h>
#include <stdint.h>
#include <stdint.h>
#include <stdint.h>
#include <stdint.h>

/* Independently implemented from documented XA sector layout and FIR facts
   Native arithmetic follows PSX-SPX, without hardware-equivalence claims */
static const sint16 xa_filter[7][29] = {{0, 0, 0, 0, 0, -2, 10, -34, 65, -84, 52, 9, -266, 1024, -2680, 9036, 26516, -6016, 3021, -1571, 848, -365, 107, 10, -16, 17, -8, 3, -1}, {0, 0, 0, -2, 0, 3, -19, 60, -75, 162, -227, 306, -67, -615, 3229, 29883, -4532, 2488, -1471, 882, -424, 166, -27, 5, 6, -8, 3, -1, 0},     {0, 0, -1, 3, -2, -5, 31, -74, 179, -402, 689, -926, 1272, -1446, 31033, -1446, 1272, -926, 689, -402, 179, -74, 31, -5, -2, 3, -1, 0, 0}, {0, -1, 3, -8, 6, 5, -27, 166, -424, 882, -1471, 2488, -4532, 29883, 3229, -615, -67, 306, -227, 162, -75, 60, -19, 3, 0, -2, 0, 0, 0},
                                        {-1, 3, -8, 17, -16, 10, 107, -365, 848, -1571, 3021, -6016, 26516, 9036, -2680, 1024, -266, 9, 52, -84, 65, -34, 10, -1, 0, 1, 0, 0, 0}, {2, -8, 16, -35, 43, 26, -235, 635, -1352, 2810, -5882, 21472, 15367, -4681, 2062, -839, 347, -68, -23, 70, -35, 17, -5, 0, 0, 0, 0, 0, 0}, {-5, 17, -35, 70, -23, -68, 347, -839, 2062, -4681, 15367, 21472, -5882, 2810, -1352, 635, -235, 26, 43, -35, 16, -8, 2, 0, 0, 0, 0, 0, 0}};
static const sint16 xa_half_filter[7][63] = {{0, 0, 0, -2, 5, -10, 16, -22, 26, -26, 18, 0, -31, 77, -133, 195, -254, 297, -309, 272, -173, 0, 253, -583, 978, -1419, 1876, -2318, 2708, -3014, 3210, 29494, 3210, -3014, 2708, -2318, 1876, -1419, 978, -583, 253, 0, -173, 272, -309, 297, -254, 195, -133, 77, -31, 0, 18, -26, 26, -22, 16, -10, 5, -2, 0, 0, 0},   {0, 0, 0, -1, 3, -8, 14, -23, 32, -38, 40, -31, 9, 29, -85, 156, -236, 314, -376, 402, -374, 270, -75, -225, 633, -1151, 1776, -2517, 3427, -4741, 7973, 28699, -815, -1065, 1639, -1772, 1678, -1451, 1155, -836, 530, -262, 50, 102, -194, 233, -232, 203, -159, 111, -67, 31, -5, -10, 17, -17, 14, -10, 6, -3, 1, 0, 0},
                                             {0, 0, -1, 1, 1, -4, 11, -20, 32, -45, 55, -58, 49, -24, -22, 91, -179, 281, -384, 470, -519, 504, -401, 184, 169, -680, 1376, -2312, 3641, -5931, 13106, 26396, -3829, 801, 410, -987, 1227, -1254, 1143, -949, 715, -476, 259, -81, -50, 133, -173, 178, -159, 127, -90, 56, -28, 8, 4, -10, 11, -9, 6, -3, 1, 0, 0},    {0, 0, -1, 2, -2, 0, 6, -15, 28, -44, 61, -75, 82, -74, 45, 9, -92, 201, -329, 463, -583, 663, -672, 578, -343, -72, 722, -1703, 3262, -6296, 18172, 22802, -5689, 2328, -780, -99, 608, -869, 952, -908, 781, -608, 422, -247, 98, 14, -87, 125, -134, 123, -100, 72, -46, 24, -8, -1, 5, -6, 5, -3, 1, 0, 0},
                                             {0, 0, -1, 3, -4, 4, -1, -6, 18, -36, 58, -81, 101, -112, 106, -75, 12, 87, -220, 381, -554, 718, -843, 892, -822, 580, -95, -757, 2281, -5621, 22721, 18261, -6380, 3333, -1755, 750, -76, -363, 617, -724, 721, -641, 514, -370, 228, -106, 11, 53, -88, 99, -93, 77, -56, 36, -20, 8, -1, -3, 3, -2, 1, 0, 0},          {0, 0, -1, 3, -5, 7, -7, 3, 6, -22, 45, -74, 105, -134, 151, -149, 116, -44, -72, 234, -434, 658, -883, 1073, -1188, 1173, -952, 399, 786, -3788, 26332, 13186, -6017, 3726, -2386, 1432, -714, 179, 196, -432, 549, -571, 523, -431, 319, -206, 106, -26, -28, 59, -71, 69, -57, 43, -28, 15, -7, 1, 1, -1, 1, 0, 0},
                                             {0, 0, -1, 3, -6, 10, -12, 12, -8, -4, 25, -54, 92, -134, 173, -200, 204, -171, 91, 45, -240, 489, -779, 1086, -1377, 1605, -1711, 1596, -1046, -807, 28664, 8029, -4816, 3510, -2601, 1851, -1210, 672, -240, -81, 295, -412, 448, -423, 357, -272, 182, -100, 35, 11, -38, 50, -49, 42, -31, 20, -11, 5, -1, 0, 1, 0, 0}};
static const sint32 xa_positive[4] = {0, 60, 115, 98};
static const sint32 xa_negative[4] = {0, 0, -52, -55};

static sint32 xa_floor_shift(sint32 value, uint32 bits)
{
    sint32 divisor = (sint32)(1u << bits);
    sint32 quotient = value / divisor;
    return quotient - (value < 0 && value % divisor != 0);
}

static sint16 xa_clamp(sint32 sample)
{
    if (sample < -32768)
        return -32768;
    if (sample > 32767)
        return 32767;
    return (sint16)sample;
}

void xa_reset(XaState *state)
{
    if (!state)
        return;
    memset(state, 0, sizeof(*state));
    state->pending = 6;
}

static void xa_resample_half(XaState *state, sint16 left, sint16 right, sint16 *output, uint32 *frames)
{
    uint32 channel, tap;
    state->ring[0][state->position] = left;
    state->ring[1][state->position] = right;
    while (state->half_phase < 7u)
    {
        for (channel = 0; channel < 2u; ++channel)
        {
            int64_t sum = 0;
            for (tap = 0; tap < 63u; ++tap)
                sum += (int64_t)state->ring[channel][(state->position - tap) & 63u] * xa_half_filter[state->half_phase][tap];
            output[*frames * 2u + channel] = xa_clamp((sint32)(sum / 32768 - (sum < 0 && sum % 32768 != 0)));
        }
        ++*frames;
        state->half_phase += 3u;
    }
    state->half_phase -= 7u;
    state->position = (state->position + 1u) & 63u;
}

static void xa_resample(XaState *state, sint16 left, sint16 right, sint16 *output, uint32 *frames)
{
    uint32 phase, channel, tap;
    state->ring[0][state->position] = left;
    state->ring[1][state->position] = right;
    state->position = (state->position + 1u) & 63u;
    if (--state->pending)
        return;
    state->pending = 6;
    for (phase = 0; phase < 7; ++phase)
    {
        for (channel = 0; channel < 2; ++channel)
        {
            sint32 sum = 0;
            for (tap = 0; tap < 29; ++tap)
            {
                sint32 product = (sint32)state->ring[channel][(state->position - 1u - tap) & 63u] * xa_filter[phase][tap];
                sum += xa_floor_shift(product, 15);
            }
            output[*frames * 2u + channel] = xa_clamp(sum);
        }
        ++*frames;
    }
}

sint32 xa_decode_sector(XaState *state, const uint8 raw[2352], sint16 *stereo, uint32 capacity_frames, uint32 *produced_frames)
{
    XaState next;
    uint32 stereo_mode, eight_bit, blocks, frames_per_group, required;
    uint32 group, block, sample, frames = 0;
    uint8 coding;
    sint16 decoded[224];
    if (produced_frames)
        *produced_frames = 0;
    if (!state || !raw || !stereo || !produced_frames)
        return XA_BAD_SECTOR;
    if (raw[15] != 2 || !(raw[18] & 4u) || memcmp(raw + 16, raw + 20, 4) != 0)
        return XA_BAD_SECTOR;
    coding = raw[19];
    /* Emphasis requires a reviewed native contract */
    if ((coding & 0xaau) || (coding & 0x40u))
        return XA_UNSUPPORTED;
    stereo_mode = coding & 1u;
    eight_bit = (coding >> 4) & 1u;
    blocks = eight_bit ? 4u : 8u;
    frames_per_group = blocks * 28u / (stereo_mode ? 2u : 1u);
    required = frames_per_group * 18u / ((coding & 4u) ? 3u : 6u) * 7u;
    if (capacity_frames < required)
        return XA_NO_SPACE;
    /* Validate before committing decoder state or output */
    for (group = 0; group < 18; ++group)
    {
        const uint8 *source = raw + 24u + group * 128u;
        for (block = 0; block < blocks; ++block)
            if (source[4u + block] & 0xc0u)
                return XA_BAD_SECTOR;
    }
    next = *state;
    if (next.position > 63u || next.half_phase > 2u || next.pending < 1u || next.pending > 6u)
        return XA_BAD_SECTOR;
    for (group = 0; group < 18; ++group)
    {
        const uint8 *source = raw + 24u + group * 128u;
        for (block = 0; block < blocks; ++block)
        {
            uint32 channel = stereo_mode ? (block & 1u) : 0u;
            uint32 shift = source[4u + block] & 15u;
            uint32 filter = (source[4u + block] >> 4) & 3u;
            if (shift > 12u)
                shift = 9u;
            for (sample = 0; sample < 28u; ++sample)
            {
                uint32 bit = block * (eight_bit ? 8u : 4u);
                uint32 packed = (uint32)source[16u + sample * 4u] | (uint32)source[17u + sample * 4u] << 8 | (uint32)source[18u + sample * 4u] << 16 | (uint32)source[19u + sample * 4u] << 24;
                uint32 mask = eight_bit ? 255u : 15u;
                sint32 value = (sint32)((packed >> bit) & mask);
                sint32 sign = eight_bit ? 128 : 8;
                sint32 prediction, expanded;
                uint32 destination = stereo_mode ? ((block / 2u) * 28u + sample) * 2u + channel : block * 28u + sample;
                if (value & sign)
                    value -= sign * 2;
                expanded = xa_floor_shift(value * (eight_bit ? 256 : 4096), shift);
                prediction = xa_floor_shift(next.previous[channel][0] * xa_positive[filter] + next.previous[channel][1] * xa_negative[filter] + 32, 6);
                decoded[destination] = xa_clamp(expanded + prediction);
                next.previous[channel][1] = next.previous[channel][0];
                next.previous[channel][0] = decoded[destination];
            }
        }
        for (sample = 0; sample < frames_per_group; ++sample)
        {
            sint16 left = decoded[sample * (stereo_mode ? 2u : 1u)];
            sint16 right = stereo_mode ? decoded[sample * 2u + 1u] : left;
            if (coding & 4u)
                xa_resample_half(&next, left, right, stereo, &frames);
            else
                xa_resample(&next, left, right, stereo, &frames);
        }
    }
    *state = next;
    *produced_frames = frames;
    return XA_OK;
}

#include "psx_press.h"
#include "psx.h"
#include "psx_mdec.h"
#include <string.h>
#include <stdlib.h>

/* Default quantization and scale matrices from PsyQ 4.6 LIBPRESS */
static const DECDCTENV default_env = {{2, 16, 16, 19, 16, 19, 22, 22, 22, 22, 22, 22, 26, 24, 26, 27, 27, 27, 26, 26, 26, 26, 27, 27, 27, 29, 29, 29, 34, 34, 34, 29, 29, 29, 27, 27, 29, 29, 32, 32, 34, 34, 37, 38, 37, 35, 35, 34, 35, 38, 38, 40, 40, 40, 48, 48, 46, 46, 56, 56, 58, 69, 69, 83}, {2, 16, 16, 19, 16, 19, 22, 22, 22, 22, 22, 22, 26, 24, 26, 27, 27, 27, 26, 26, 26, 26, 27, 27, 27, 29, 29, 29, 34, 34, 34, 29, 29, 29, 27, 27, 29, 29, 32, 32, 34, 34, 37, 38, 37, 35, 35, 34, 35, 38, 38, 40, 40, 40, 48, 48, 46, 46, 56, 56, 58, 69, 69, 83}, {23170, 23170,  23170,  23170, 23170, 23170,  23170,  23170, 32138, 27245,  18204, 6392,  -6393,  -18205, -27246, -32139, 30273, 12539,  -12540, -30274, -30274, -12540, 12539,  30273, 27245, -6393,  -32139, -18205, 18204, 32138,  6392,  -27246,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         23170, -23171, -23171, 23170, 23170, -23171, -23171, 23170, 18204, -32139, 6392,  27245, -27246, -6393,  32138,  -18205, 12539, -30274, 30273,  -12540, -12540, 30273,  -30274, 12539, 6392,  -18205, 27245,  -32139, 32138, -27246, 18204, -6393}};

static MdecState decoder;
static const uint32 *input;
static uint32 input_snapshot[65535];
static uint32 input_pos, input_left;
static uint32 *output;
static uint32 output_left;
static uint32 initialized, input_busy, output_busy, input_pending, output_pending;
static uint32 input_masked, output_masked;

static void mdec_wait_service(void)
{
    mdec_psyq_pump(0);
}

typedef struct MDEC_CHECKPOINT
{
    MdecState decoder;
    uint32 input_snapshot[65535];
    uint32 input_pos, input_left, output_address, output_left;
    uint32 initialized, input_busy, output_busy, input_pending, output_pending;
} MDEC_CHECKPOINT;

sint32 mdec_psyq_checkpoint_io(FILE *file, sint32 load)
{
    MDEC_CHECKPOINT *saved;
    uint32 *restored_output = NULL;
    sint32 ok;
    if (!file)
        return 0;
    saved = (MDEC_CHECKPOINT *)calloc(1, sizeof(*saved));
    if (!saved)
        return 0;
    if (load)
    {
        ok = fread(saved, sizeof(*saved), 1, file) == 1;
        if (ok && saved->output_busy)
        {
            restored_output = (uint32 *)xport_guest_ptr(saved->output_address, (size_t)saved->output_left * 4);
            ok = restored_output != NULL;
        }
        if (ok)
        {
            decoder = saved->decoder;
            memcpy(input_snapshot, saved->input_snapshot, sizeof(input_snapshot));
            input = saved->initialized ? input_snapshot : NULL;
            input_pos = saved->input_pos;
            input_left = saved->input_left;
            output = restored_output;
            output_left = saved->output_left;
            initialized = saved->initialized;
            input_busy = saved->input_busy;
            output_busy = saved->output_busy;
            input_pending = saved->input_pending;
            output_pending = saved->output_pending;
            input_masked = output_masked = 0;
            psx_wait_bind_service(0, initialized ? mdec_wait_service : NULL);
        }
    }
    else
    {
        saved->decoder = decoder;
        memcpy(saved->input_snapshot, input_snapshot, sizeof(input_snapshot));
        saved->input_pos = input_pos;
        saved->input_left = input_left;
        saved->output_left = output_left;
        saved->initialized = initialized;
        saved->input_busy = input_busy;
        saved->output_busy = output_busy;
        saved->input_pending = input_pending;
        saved->output_pending = output_pending;
        if (output_busy)
            saved->output_address = xport_guest_buffer_address(output, (size_t)output_left * 4);
        ok = (!output_busy || saved->output_address) && fwrite(saved, sizeof(*saved), 1, file) == 1;
    }
    free(saved);
    return ok;
}

static void ensure_initialized(void)
{
    if (!initialized)
        DecDCTReset(0);
}

void DecDCTReset(sint32 mode)
{
    psx_wait_bind_service(0, mdec_wait_service);
    if (!initialized)
        mdec_initialize(&decoder);
    mdec_control(&decoder, 0xE0000000u);
    initialized = 1;
    input_busy = output_busy = input_pending = output_pending = 0;
    input_left = output_left = 0;
    input = NULL;
    output = NULL;
    if (!mode)
    {
        memcpy(decoder.quantization_y, default_env.iq_y, 64);
        memcpy(decoder.quantization_uv, default_env.iq_c, 64);
        memcpy(decoder.scale, mdec_standard_scale, sizeof(decoder.scale));
    }
}

DECDCTENV *DecDCTGetEnv(DECDCTENV *env)
{
    ensure_initialized();
    if (!env)
        return NULL;
    memcpy(env->iq_y, decoder.quantization_y, 64);
    memcpy(env->iq_c, decoder.quantization_uv, 64);
    memcpy(env->dct, decoder.scale, 128);
    return env;
}

DECDCTENV *DecDCTPutEnv(DECDCTENV *env)
{
    ensure_initialized();
    if (!env)
        return NULL;
    memcpy(decoder.quantization_y, env->iq_y, 64);
    memcpy(decoder.quantization_uv, env->iq_c, 64);
    memcpy(decoder.scale, env->dct, 128);
    return env;
}

sint32 DecDCTBufSize(uint32 *bs)
{
    return bs ? (sint32)(bs[0] & 0xFFFFu) : 0;
}

void mdec_psyq_pump(uint32 ticks)
{
    uint32 loops;
    ensure_initialized();
    for (loops = 0; loops < 65536u; ++loops)
    {
        uint32 progress = 0;
        if (input_busy && input_left)
        {
            uint32 packed[256], n = input_left > 512u ? 512u : input_left, i, accepted;
            /* Repack after an odd FIFO acceptance without losing a halfword */
            for (i = 0; i < n; i += 2)
            {
                uint32 p = input_pos + i;
                uint32 lo = (input[p / 2] >> ((p & 1u) * 16u)) & 0xFFFFu;
                uint32 hi = 0;
                if (i + 1 < n)
                {
                    ++p;
                    hi = (input[p / 2] >> ((p & 1u) * 16u)) & 0xFFFFu;
                }
                packed[i / 2] = lo | (hi << 16);
            }
            if (n == 1u && decoder.input_count < 512u)
            {
                decoder.input[(decoder.input_head + decoder.input_count) % 512u] = (uint16)packed[0];
                ++decoder.input_count;
                mdec_control(&decoder, 0x60000000u);
                accepted = 1u;
            }
            else
            {
                /* Leave an odd final halfword for the next iteration */
                if (n & 1u)
                    --n;
                accepted = n ? mdec_dma_write(&decoder, packed, n / 2u) : 0;
            }
            if (accepted > input_left)
                abort();
            input_pos += accepted;
            input_left -= accepted;
            progress |= accepted;
        }
        if (input_busy && !input_left)
        {
            input_busy = 0;
            input_pending = 1;
            progress = 1;
        }
        if (decoder.copy_ticks)
        {
            ticks = decoder.copy_ticks;
            mdec_advance(&decoder, ticks);
            progress = 1;
        }
        /* A completion callback still owns its completed buffer until it returns */
        if (output_busy && output_left && !output_masked)
        {
            uint32 copied = mdec_dma_read(&decoder, output, output_left);
            output += copied;
            output_left -= copied;
            progress |= copied;
        }
        if (output_busy && !output_left && !output_masked)
        {
            output_busy = 0;
            output_pending = 1;
            progress = 1;
        }
        /* Mask only the matching completion callback while allowing the opposite DMA to progress */
        if (input_pending && !input_masked && psx_callbacks_allowed())
        {
            DecDCTCallback callback = psx_dma_callback_native(0u);
            input_pending = 0;
            if (callback)
            {
                input_masked = 1;
                callback();
                input_masked = 0;
            }
            progress = 1;
        }
        if (output_pending && !output_masked && psx_callbacks_allowed())
        {
            DecDCTCallback callback = psx_dma_callback_native(1u);
            output_pending = 0;
            if (callback)
            {
                output_masked = 1;
                callback();
                output_masked = 0;
            }
            progress = 1;
        }
        if (!progress)
            break;
    }
}

static sint32 dma_sync(sint32 mode, uint32 *busy)
{
    uint32 attempts;
    ensure_initialized();
    if (mode)
    {
        mdec_psyq_pump(0);
        return *busy ? 1 : 0;
    }
    for (attempts = 0; *busy && attempts < 1024u; ++attempts)
        mdec_psyq_pump(564480u);
    if (*busy)
        DecDCTReset(1);
    return 0;
}

sint32 DecDCTinSync(sint32 mode)
{
    return dma_sync(mode, &input_busy);
}

sint32 DecDCToutSync(sint32 mode)
{
    return dma_sync(mode, &output_busy);
}

void DecDCTin(uint32 *buf, sint32 mode)
{
    uint32 command;
    ensure_initialized();
    if (!buf)
        abort();
    DecDCTinSync(0);
    command = buf[0] & ~0x0A000000u;
    if (!(mode & 1))
        command |= 0x08000000u;
    if (mode & 2)
        command |= 0x02000000u;
    buf[0] = command;
    if (!mdec_write_data(&decoder, command))
        abort();
    /* Own the submitted payload while the caller reuses its VLC buffer */
    memcpy(input_snapshot, buf + 1, (command & 0xFFFFu) * sizeof(uint32));
    input = input_snapshot;
    input_pos = 0;
    input_left = (command & 0xFFFFu) * 2u;
    input_busy = 1;
    mdec_psyq_pump(0);
}

void DecDCTout(uint32 *buf, sint32 size)
{
    ensure_initialized();
    if (!buf || size <= 0)
        abort();
    DecDCToutSync(0);
    /* PsyQ programs 32-word DMA blocks and discards the remainder */
    output = buf;
    output_left = (uint32)size & ~31u;
    output_busy = 1;
    /* Complete native output synchronously after queued callbacks release their buffers */
    if (!output_masked)
        mdec_psyq_pump(0xFFFFFFFFu);
}

DecDCTCallback DecDCTinCallback(DecDCTCallback callback)
{
    return DMACallback(0u, callback);
}

DecDCTCallback DecDCToutCallback(DecDCTCallback callback)
{
    return DMACallback(1u, callback);
}

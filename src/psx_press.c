#include "psx_press.h"
#include "psx.h"
#include "psx_mdec.h"
#include <string.h>
#include <stdlib.h>

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
        memcpy(decoder.quantization_y, mdec_default_environment.iq_y, 64);
        memcpy(decoder.quantization_uv, mdec_default_environment.iq_c, 64);
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
            psx_wait_signal_service(0);
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
            psx_wait_signal_service(0);
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
    psx_sync_callbacks();
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
    psx_sync_callbacks();
}

DecDCTCallback DecDCTinCallback(DecDCTCallback callback)
{
    return DMACallback(0u, callback);
}

DecDCTCallback DecDCToutCallback(DecDCTCallback callback)
{
    return DMACallback(1u, callback);
}

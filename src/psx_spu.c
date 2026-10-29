#include "ap.h"
#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef AP_WEB
    #include <emscripten.h>
#endif
#include "psx_spu.h"
#include "psx_xa.h"
#include "xport_trace.h"

const uint8 spu_silent_sample[16] = {7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7};

SpuVoiceModulationCallbacks spu_voice_modulation_callbacks;
SpuDmaEventState spu_dma_event_state;

static SpuReverbControl reverb_control;
static XportFmvAudioRemainder xa_remainder;
static sint32 xa_remainder_ready;
static SpuReverbWorkCheck reverb_work_check;
static void *reverb_work_context;
static sint32 reverb_incomplete;
// Original PsyQ LIBSPU reverb tables
const uint32 spu_reverb_default_work[10] = {0x0000FFFEu, 0x0000FB28u, 0x0000FC18u, 0x0000F6F8u, 0x0000F204u, 0x0000EA44u, 0x0000E128u, 0x0000CFF8u, 0x0000CFF8u, 0x0000F880u};

static const uint8 spu_reverb_default_presets[680] = {
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x7Du, 0x00u, 0x5Bu, 0x00u, 0x80u, 0x6Du, 0xB8u, 0x54u, 0xD0u, 0xBEu, 0x00u, 0x00u, 0x00u, 0x00u, 0x80u, 0xBAu, 0x00u, 0x58u, 0x00u, 0x53u, 0xD6u, 0x04u, 0x33u, 0x03u, 0xF0u, 0x03u, 0x27u, 0x02u, 0x74u, 0x03u, 0xEFu, 0x01u, 0x34u, 0x03u, 0xB5u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xB4u, 0x01u, 0x36u, 0x01u, 0xB8u, 0x00u, 0x5Cu, 0x00u, 0x00u, 0x80u, 0x00u, 0x80u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x33u, 0x00u, 0x25u, 0x00u, 0xF0u, 0x70u, 0xA8u, 0x4Fu, 0xE0u, 0xBCu, 0x10u, 0x44u, 0xF0u, 0xC0u, 0x00u, 0x9Cu, 0x80u, 0x52u, 0xC0u, 0x4Eu, 0xE4u, 0x03u, 0x1Bu, 0x03u, 0xA4u, 0x03u, 0xAFu, 0x02u, 0x72u, 0x03u, 0x66u, 0x02u, 0x1Cu, 0x03u, 0x5Du, 0x02u, 0x5Cu, 0x02u, 0x8Eu, 0x01u, 0x2Fu, 0x02u, 0x35u, 0x01u, 0xD2u, 0x01u, 0xB7u, 0x00u, 0x8Fu, 0x01u, 0xB5u, 0x00u, 0xB4u, 0x00u, 0x80u, 0x00u, 0x4Cu, 0x00u, 0x26u, 0x00u, 0x00u, 0x80u, 0x00u, 0x80u, 0x00u, 0x00u, 0x00u, 0x00u, 0xB1u, 0x00u, 0x7Fu, 0x00u, 0xF0u, 0x70u, 0xA8u, 0x4Fu, 0xE0u, 0xBCu, 0x10u, 0x45u, 0xF0u, 0xBEu, 0xC0u, 0xB4u, 0x80u, 0x52u, 0xC0u, 0x4Eu, 0x04u, 0x09u, 0x6Bu, 0x07u, 0x24u, 0x08u, 0x5Fu, 0x06u, 0xA2u, 0x07u, 0x16u, 0x06u, 0x6Cu, 0x07u, 0xEDu, 0x05u, 0xECu, 0x05u, 0x2Eu, 0x04u, 0x0Fu, 0x05u, 0x05u, 0x03u, 0x62u, 0x04u, 0xB7u, 0x02u, 0x2Fu, 0x04u, 0x65u, 0x02u, 0x64u, 0x02u, 0xB2u, 0x01u, 0x00u, 0x01u, 0x80u, 0x00u, 0x00u, 0x80u, 0x00u, 0x80u,
    0x00u, 0x00u, 0x00u, 0x00u, 0xE3u, 0x00u, 0xA9u, 0x00u, 0x60u, 0x6Fu, 0xA8u, 0x4Fu, 0xE0u, 0xBCu, 0x10u, 0x45u, 0xF0u, 0xBEu, 0x80u, 0xA6u, 0x80u, 0x56u, 0xC0u, 0x52u, 0xFBu, 0x0Du, 0x58u, 0x0Bu, 0x09u, 0x0Du, 0x3Cu, 0x0Au, 0xD9u, 0x0Bu, 0x73u, 0x09u, 0x59u, 0x0Bu, 0xDAu, 0x08u, 0xD9u, 0x08u, 0xE9u, 0x05u, 0xECu, 0x07u, 0xB0u, 0x04u, 0xEFu, 0x06u, 0xD2u, 0x03u, 0xEAu, 0x05u, 0x1Du, 0x03u, 0x1Cu, 0x03u, 0x38u, 0x02u, 0x54u, 0x01u, 0xAAu, 0x00u, 0x00u, 0x80u, 0x00u, 0x80u, 0x00u, 0x00u, 0x00u, 0x00u, 0xA5u, 0x01u, 0x39u, 0x01u, 0x00u, 0x60u, 0x00u, 0x50u, 0x00u, 0x4Cu, 0x00u, 0xB8u, 0x00u, 0xBCu, 0x00u, 0xC0u, 0x00u, 0x60u, 0x00u, 0x5Cu, 0xBAu, 0x15u, 0xBBu, 0x11u, 0xC2u, 0x14u, 0xBDu, 0x10u, 0xBCu, 0x11u, 0xC1u, 0x0Du, 0xC0u, 0x11u, 0xC3u, 0x0Du, 0xC0u, 0x0Du, 0xC1u, 0x09u, 0xC4u, 0x0Bu, 0xC1u, 0x07u, 0x00u, 0x0Au, 0xCDu, 0x06u, 0xC2u, 0x09u, 0xC1u, 0x05u, 0xC0u, 0x05u, 0x1Au, 0x04u, 0x74u, 0x02u, 0x3Au, 0x01u, 0x00u, 0x80u, 0x00u, 0x80u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x3Du, 0x03u, 0x31u, 0x02u, 0x00u, 0x7Eu, 0x00u, 0x50u, 0x00u, 0xB4u, 0x00u, 0xB0u, 0x00u, 0x4Cu, 0x00u, 0xB0u, 0x00u, 0x60u, 0x00u, 0x54u, 0xD6u, 0x1Eu, 0x31u, 0x1Au, 0x14u, 0x1Du, 0x3Bu, 0x18u, 0xC2u, 0x1Bu, 0xB2u, 0x16u, 0x32u, 0x1Au, 0xEFu, 0x15u, 0xEEu, 0x15u, 0x55u, 0x10u, 0x34u, 0x13u, 0x2Du, 0x0Fu, 0xF6u, 0x11u, 0x5Du, 0x0Cu, 0x56u, 0x10u, 0xE1u, 0x0Au, 0xE0u, 0x0Au, 0xA2u, 0x07u, 0x64u, 0x04u, 0x32u, 0x02u, 0x00u, 0x80u, 0x00u, 0x80u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0x00u, 0x01u, 0x00u, 0xFFu, 0x7Fu, 0xFFu, 0x7Fu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x81u, 0x00u, 0x00u, 0x00u, 0x00u, 0xFFu, 0x1Fu, 0xFFu, 0x0Fu, 0x05u, 0x10u, 0x05u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x05u, 0x10u, 0x05u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x04u, 0x10u, 0x02u, 0x10u, 0x04u, 0x00u, 0x02u, 0x00u, 0x00u, 0x80u, 0x00u, 0x80u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0x00u, 0x01u, 0x00u, 0xFFu, 0x7Fu, 0xFFu, 0x7Fu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xFFu, 0x1Fu, 0xFFu, 0x0Fu, 0x05u, 0x10u, 0x05u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x05u, 0x10u, 0x05u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x04u, 0x10u, 0x02u, 0x10u, 0x04u, 0x00u, 0x02u, 0x00u, 0x00u, 0x80u, 0x00u, 0x80u, 0x00u, 0x00u, 0x00u, 0x00u, 0x17u, 0x00u, 0x13u, 0x00u, 0xF0u, 0x70u, 0xA8u, 0x4Fu, 0xE0u, 0xBCu, 0x10u, 0x45u, 0xF0u, 0xBEu, 0x00u, 0x85u, 0x80u, 0x5Fu, 0xC0u, 0x54u, 0x71u, 0x03u, 0xAFu, 0x02u, 0xE5u, 0x02u, 0xDFu, 0x01u, 0xB0u, 0x02u, 0xD7u, 0x01u, 0x58u, 0x03u, 0x6Au, 0x02u, 0xD6u, 0x01u, 0x1Eu, 0x01u, 0x2Du, 0x01u, 0xB1u, 0x00u, 0x1Fu, 0x01u, 0x59u, 0x00u, 0xA0u, 0x01u, 0xE3u, 0x00u, 0x58u, 0x00u, 0x40u, 0x00u, 0x28u, 0x00u, 0x14u, 0x00u, 0x00u, 0x80u, 0x00u, 0x80u,
};

static uint32 reverb_work_override[10];
static uint8 reverb_presets_override[680];
static const uint32 *reverb_preset_work = spu_reverb_default_work;
static const uint8 *reverb_presets = spu_reverb_default_presets;
static uint32 reverb_address_shift = 3u;
static void spu_reverb_reset_cursor(void);

void spu_bind_reverb_presets(const uint32 *work_units, const uint8 *presets, uint32 address_shift)
{
    if (!work_units || !presets || address_shift > 15u)
        abort();
    xport_audio_lock();
    memcpy(reverb_work_override, work_units, sizeof(reverb_work_override));
    reverb_preset_work = reverb_work_override;
    memcpy(reverb_presets_override, presets, sizeof(reverb_presets_override));
    reverb_presets = reverb_presets_override;
    reverb_address_shift = address_shift;
    xport_audio_unlock();
}

static SpuHeapBinding heap;
static sint32 heap_failed;

typedef enum SPU_TRANSFER_OWNER
{
    SPU_TRANSFER_CUSTOM,
    SPU_TRANSFER_GUEST,
    SPU_TRANSFER_NATIVE
} SPU_TRANSFER_OWNER;

static SpuTransferBinding transfer;
static SPU_TRANSFER_OWNER native_transfer_binding;
static sint32 transfer_failed;
static uint32 transfer_byte_address;
static uint64 transfer_deadline;
static sint32 transfer_pending;
static XportDeviceOperation transfer_operation;
static void spu_irq_check_range(uint32 address, uint32 bytes);




static void cd_notify_command(uint8 command);
static void cd_record_stream_command_locked(uint8 command);

static void spu_trace_internal_gap(XportDevicePrimitive primitive, uint32 reason)
{
    XportDeviceOperation operation = {0};
    if (xport_device_trace_enabled() && xport_device_trace_begin(&operation, 0u, XPORT_DEVICE_SPU, primitive, XPORT_DEVICE_DIRECTION_INTERNAL, 4u, 0u))
    {
        xport_device_trace_gap(&operation, reason);
        xport_device_trace_complete(&operation, 0);
    }
}

static XportDeviceRegion spu_device_region(XportMemoryRegion region)
{
    if (region == XPORT_MEMORY_DRAM)
        return XPORT_DEVICE_REGION_RAM;
    if (region == XPORT_MEMORY_SCRATCHPAD)
        return XPORT_DEVICE_REGION_SCRATCHPAD;
    return XPORT_DEVICE_REGION_NONE;
}

sint32 spu_bind_transfer(const SpuTransferBinding *binding)
{
    native_transfer_binding = SPU_TRANSFER_CUSTOM;
    memset(&transfer, 0, sizeof(transfer));
    transfer_failed = 0;
    transfer_byte_address = 0;
    transfer_deadline = 0;
    transfer_pending = 0;
    memset(&transfer_operation, 0, sizeof(transfer_operation));
    if (!binding)
        return 1;
    if (!binding->requested_mode || !binding->normalized_mode || !binding->address_units || !binding->completion || ((binding->completion_callback != NULL) == (binding->native_completion_callback != NULL)) || !binding->source_address || !binding->dma_blocks || binding->address_shift > 31u || (binding->address_rounding_enabled && !binding->address_rounding_unit) || !binding->block_bytes || !binding->ticks_per_block || !binding->clock || !binding->wait_until || !binding->complete_event || !binding->guest_address)
    {
        transfer_failed = 1;
        return 0;
    }
    transfer = *binding;
    return 1;
}

static uint64 spu_native_transfer_clock(void *context)
{
    (void)context;
    return 0u;
}

static sint32 spu_native_transfer_wait(void *context, uint64 deadline)
{
    (void)context;
    (void)deadline;
    return transfer_failed ? 0 : 1;
}

static sint32 spu_native_transfer_complete(void *context)
{
    (void)context;
    return transfer_failed ? 0 : 1;
}

static uint32 spu_native_transfer_address(void *context, const void *source)
{
    (void)context;
    return xport_guest_buffer_address((void *)source, 1u);
}

sint32 spu_bind_native_transfer_guest(const SpuNativeTransferGuestBinding *guest)
{
    SpuTransferBinding binding;
    if (!guest)
        return 0;
    memset(&binding, 0, sizeof(binding));
    binding.requested_mode = (uint32 *)psx_addr(guest->requested_mode, 4u);
    binding.normalized_mode = (uint32 *)psx_addr(guest->normalized_mode, 4u);
    binding.address_units = (uint16 *)psx_addr(guest->address_units, 2u);
    binding.completion = (uint32 *)psx_addr(guest->completion, 4u);
    binding.completion_callback = (uint32 *)psx_addr(guest->completion_callback, 4u);
    binding.source_address = (uint32 *)psx_addr(guest->source_address, 4u);
    binding.dma_blocks = (uint32 *)psx_addr(guest->dma_blocks, 4u);
    binding.address_shift = guest->address_shift;
    binding.address_rounding_enabled = 1u;
    binding.address_rounding_unit = 1u << (guest->address_shift & 31u);
    binding.address_alignment_mask = binding.address_rounding_unit - 1u;
    binding.block_bytes = 64u;
    binding.ticks_per_block = 1u;
    binding.clock = spu_native_transfer_clock;
    binding.wait_until = spu_native_transfer_wait;
    binding.complete_event = spu_native_transfer_complete;
    binding.guest_address = spu_native_transfer_address;
    if (!spu_bind_transfer(&binding))
        return 0;
    native_transfer_binding = SPU_TRANSFER_GUEST;
    return 1;
}

sint32 spu_transfer_failed(void)
{
    return transfer_failed;
}

sint32 SpuSetTransferMode(sint32 mode)
{
    sint32 normalized = mode == 1 ? 1 : 0;
    if (!transfer.requested_mode || transfer_failed)
    {
        transfer_failed = 1;
        return normalized;
    }
    *transfer.requested_mode = (uint32)mode;
    *transfer.normalized_mode = (uint32)normalized;
    return normalized;
}

uint32 _spu_FsetRXXa(sint32 register_index, uint32 address)
{
    uint32 units;
    if (address % SPU_RAM_ADDRESS_UNIT != 0)
        address = (address + SPU_RAM_ADDRESS_UNIT) & ~(uint32)SPU_RAM_ADDRESS_MASK;
    units = address >> SPU_RAM_ADDRESS_SHIFT;
    if (register_index == -1)
        return (uint16)units;
    if (register_index != -2)
        w_u16(SPU_REGISTER_BASE + ((uint32)register_index << 1), units);
    return address;
}

uint32 SpuSetTransferStartAddr(uint32 address)
{
    uint32 units, aligned = address;
    if (!transfer.requested_mode || transfer_failed)
    {
        transfer_failed = 1;
        return 0;
    }
    if (address - 0x1010u > 0x7efe8u)
        return 0;
    if (transfer.address_rounding_enabled && address % transfer.address_rounding_unit)
        aligned = (address + transfer.address_rounding_unit) & ~transfer.address_alignment_mask;
    units = aligned >> transfer.address_shift;
    xport_audio_lock();
    *transfer.address_units = (uint16)units;
    transfer_byte_address = (uint32)(uint16)units << transfer.address_shift;
    spu_irq_check_range(transfer_byte_address, 2);
    xport_audio_unlock();
    return (uint32)(uint16)units << transfer.address_shift;
}

static sint32 spu_upload_wrapped(uint32 addr, const void *src, uint32 size)
{
    uint32 first = size > SPU_RAM_SIZE - addr ? SPU_RAM_SIZE - addr : size;

    return spu_upload(addr, src, first) && (first == size || spu_upload(0u, (const uint8 *)src + first, size - first));
}

static sint32 spu_upload_native(uint32 addr, const void *src, uint32 size, uint32 rounded)
{
    uint8 padding[64] = {0};
    uint32 tail = rounded - size;

    if (!xport_memory_readable(src, size) || tail > sizeof(padding) || !spu_upload_wrapped(addr, src, size))
        return 0;
    return !tail || spu_upload_wrapped((addr + size) & (SPU_RAM_SIZE - 1u), padding, tail);
}

/* PsyQ transfer caps differ between the early and later SDK families */
#ifndef XPORT_SPU_WRITE_MAX_BYTES
    #define XPORT_SPU_WRITE_MAX_BYTES 0x7f000u
#endif
#if XPORT_SPU_WRITE_MAX_BYTES != 0x7f000u && XPORT_SPU_WRITE_MAX_BYTES != 0x7eff0u
    #error Unsupported PsyQ SPU write limit
#endif

static uint32 spu_write_source(const void *source, uint32 byte_count, sint32 allow_host)
{
    uint32 blocks, rounded, guest;
    sint32 host_source;
    if (byte_count > XPORT_SPU_WRITE_MAX_BYTES)
        byte_count = XPORT_SPU_WRITE_MAX_BYTES;
    if (!transfer.requested_mode || transfer_failed || !source)
    {
        transfer_failed = 1;
        return byte_count;
    }
    blocks = (byte_count + transfer.block_bytes - 1u) / transfer.block_bytes;
    if (blocks > UINT32_MAX / transfer.block_bytes || blocks > UINT64_MAX / transfer.ticks_per_block)
    {
        transfer_failed = 1;
        return byte_count;
    }
    rounded = *transfer.normalized_mode == 1u ? (byte_count + 1u) & ~1u : blocks * transfer.block_bytes;
    if (native_transfer_binding)
        transfer_byte_address = (uint32)*transfer.address_units << transfer.address_shift;
    guest = transfer.guest_address(transfer.context, source);
    host_source = allow_host && native_transfer_binding && !guest;
    if (xport_device_trace_enabled())
    {
        if (transfer_operation.active || !xport_device_trace_begin(&transfer_operation, 0u, XPORT_DEVICE_SPU, XPORT_DEVICE_SPU_DMA, XPORT_DEVICE_DIRECTION_TO_DEVICE, 4u, *transfer.normalized_mode))
        {
            transfer_failed = 1;
            return byte_count;
        }
        if (host_source)
        {
            /* Native source has no guest DMA address */
            xport_device_trace_gap(&transfer_operation, 1u);
            xport_device_trace_stage(&transfer_operation, XPORT_DEVICE_SUBMIT, XPORT_DEVICE_EVIDENCE_UNCOVERED, XPORT_DEVICE_REGION_NONE, 0u, XPORT_DEVICE_REGION_SPU_RAM, transfer_byte_address, rounded, 0xffffffffu);
        }
        else
        {
            xport_device_trace_stage(&transfer_operation, XPORT_DEVICE_SUBMIT, XPORT_DEVICE_EVIDENCE_EXACT, XPORT_DEVICE_REGION_RAM, guest & 0x1fffffu, XPORT_DEVICE_REGION_SPU_RAM, transfer_byte_address, rounded, 0xffffffffu);
            xport_device_trace_stage(&transfer_operation, XPORT_DEVICE_FETCH, XPORT_DEVICE_EVIDENCE_EXACT, XPORT_DEVICE_REGION_RAM, guest & 0x1fffffu, XPORT_DEVICE_REGION_NONE, 0u, rounded, 0xffffffffu);
        }
    }
    if ((!guest && !host_source) || (host_source && native_transfer_binding != SPU_TRANSFER_NATIVE ? !xport_memory_readable(source, rounded) : (!host_source && native_transfer_binding && !xport_guest_buffer_address((void *)source, rounded))) || rounded > SPU_RAM_SIZE || (host_source && native_transfer_binding == SPU_TRANSFER_NATIVE ? !spu_upload_native(transfer_byte_address, source, byte_count, rounded) : !spu_upload_wrapped(transfer_byte_address, source, rounded)))
    {
        if (transfer_operation.active)
            xport_device_trace_complete(&transfer_operation, 0);
        transfer_failed = 1;
        return byte_count;
    }
    if (*transfer.normalized_mode == 0u)
    {
        if (!host_source)
            *transfer.source_address = guest;
        *transfer.dma_blocks = blocks;
    }
    if (transfer.native_completion_callback ? *transfer.native_completion_callback == NULL : *transfer.completion_callback == 0)
        *transfer.completion = 0;
    transfer_deadline = transfer.clock(transfer.context) + (uint64)blocks * transfer.ticks_per_block;
    transfer_pending = blocks != 0;
    if (native_transfer_binding)
    {
        transfer_pending = 0;
        *transfer.completion = 1u;
    }
    if (*transfer.requested_mode == 1u && transfer_operation.active)
        xport_device_trace_complete(&transfer_operation, 1);
    return byte_count;
}

uint32 SpuWrite(const void *source, uint32 byte_count)
{
    return spu_write_source(source, byte_count, native_transfer_binding == SPU_TRANSFER_NATIVE);
}

uint32 spu_write_host(const void *source, uint32 byte_count)
{
    return spu_write_source(source, byte_count, 1);
}

sint32 SpuIsTransferCompleted(sint32 mode)
{
    sint32 result;
    if (!transfer.requested_mode || transfer_failed)
    {
        transfer_failed = 1;
        return 0;
    }
    if (*transfer.requested_mode == 1u || *transfer.completion == 1u)
    {
        if (transfer_operation.active)
            xport_device_trace_complete(&transfer_operation, 1);
        return 1;
    }
    if (transfer_pending)
    {
        if (mode != 1)
            return 0;
        if (!transfer.wait_until(transfer.context, transfer_deadline))
        {
            transfer_failed = 1;
            return 0;
        }
        result = transfer.complete_event(transfer.context);
        if (result != 1)
        {
            transfer_failed = 1;
            return result;
        }
        transfer_pending = 0;
        *transfer.completion = 1u;
        if (transfer_operation.active)
            xport_device_trace_complete(&transfer_operation, 1);
        return 1;
    }
    result = transfer.complete_event(transfer.context);
    if (result == 1)
    {
        *transfer.completion = 1u;
        if (transfer_operation.active)
            xport_device_trace_complete(&transfer_operation, 1);
    }
    return result;
}

static SpuMallocGuestBinding malloc_guest;

sint32 spu_bind_malloc_guest(const SpuMallocGuestBinding *binding)
{
    if (!binding)
    {
        memset(&malloc_guest, 0, sizeof(malloc_guest));
        return 1;
    }
    if (!psx_addr(binding->shift_address, 4u) || !psx_addr(binding->alignment_address, 4u) || !psx_addr(binding->count_address, 4u) || !psx_addr(binding->last_index_address, 4u) || !psx_addr(binding->table_address, 4u) || !psx_addr(binding->reserved_enabled_address, 4u) || !psx_addr(binding->reserved_units_address, 4u))
        return 0;
    malloc_guest = *binding;
    return 1;
}

uint32 spu_init_malloc_guest(uint32 count, uint32 table)
{
    SpuHeapBinding binding;
    uint32 shift, reserved;
    if ((sint32)count <= 0)
        return 0u;
    if (!malloc_guest.shift_address || count > 0x10000u)
        return 0u;
    shift = r_u32(malloc_guest.shift_address) & 31u;
    if (shift > 15u)
        return 0u;
    memset(&binding, 0, sizeof(binding));
    binding.entries = (uint32 *)psx_addr(table, (size_t)(count + 1u) * 8u);
    binding.last_index = (uint32 *)psx_addr(malloc_guest.last_index_address, 4u);
    if (!binding.entries || !binding.last_index)
        return 0u;
    w_u32(table, 0x40001010u);
    w_u32(malloc_guest.table_address, table);
    w_u32(malloc_guest.last_index_address, 0u);
    w_u32(malloc_guest.count_address, count);
    w_u32(table + 4u, (0x10000u << shift) - 0x1010u);
    binding.capacity = count + 1u;
    binding.limit = count;
    binding.address_shift = shift;
    binding.alignment_mask = r_u32(malloc_guest.alignment_address);
    reserved = r_u32(malloc_guest.reserved_enabled_address);
    binding.reserved_bytes = reserved ? (0x10000u - r_u32(malloc_guest.reserved_units_address)) << shift : 0u;
    return spu_bind_heap(&binding) ? count : 0u;
}

sint32 spu_bind_heap(const SpuHeapBinding *binding)
{
    memset(&heap, 0, sizeof(heap));
    heap_failed = 0;
    if (!binding)
        return 1;
    if (!binding->entries || !binding->last_index || !binding->capacity || binding->limit >= binding->capacity || *binding->last_index > binding->limit || binding->address_shift > 31u)
    {
        heap_failed = 1;
        return 0;
    }
    heap = *binding;
    return 1;
}

sint32 spu_heap_failed(void)
{
    return heap_failed;
}

static sint32 spu_heap_collect(void)
{
    uint32 i, j, last = *heap.last_index;
    uint32 *entry, *next;
    // Preserve the SDK's merge, zero-mark, sort, compact and tail-collapse order
    for (i = 0; i <= last;)
    {
        entry = heap.entries + 2u * i;
        if (entry[0] & 0x80000000u)
        {
            j = i + 1u;
            while (j < heap.capacity && heap.entries[2u * j] == 0x2fffffffu)
                ++j;
            if (j >= heap.capacity)
            {
                heap_failed = 1;
                return 0;
            }
            next = heap.entries + 2u * j;
            if ((next[0] & 0x80000000u) && (next[0] & 0x0fffffffu) == (entry[0] & 0x0fffffffu) + entry[1])
            {
                next[0] = 0x2fffffffu;
                entry[1] += next[1];
                continue;
            }
        }
        ++i;
    }
    for (i = 0; i <= last; ++i)
        if (!heap.entries[2u * i + 1u])
            heap.entries[2u * i] = 0x2fffffffu;
    for (i = 0; i <= last && !(heap.entries[2u * i] & 0x40000000u); ++i)
        for (j = i + 1u; j <= last && !(heap.entries[2u * j] & 0x40000000u); ++j)
            if ((heap.entries[2u * j] & 0x0fffffffu) < (heap.entries[2u * i] & 0x0fffffffu))
            {
                uint32 address = heap.entries[2u * i], size = heap.entries[2u * i + 1u];
                heap.entries[2u * i] = heap.entries[2u * j];
                heap.entries[2u * i + 1u] = heap.entries[2u * j + 1u];
                heap.entries[2u * j] = address;
                heap.entries[2u * j + 1u] = size;
            }
    for (i = 0; i <= last && !(heap.entries[2u * i] & 0x40000000u); ++i)
        if (heap.entries[2u * i] == 0x2fffffffu)
        {
            heap.entries[2u * i] = heap.entries[2u * last];
            heap.entries[2u * i + 1u] = heap.entries[2u * last + 1u];
            *heap.last_index = last = i;
            break;
        }
    while (last && (heap.entries[2u * (last - 1u)] & 0x80000000u))
    {
        entry = heap.entries + 2u * (last - 1u);
        entry[0] = (entry[0] & 0x0fffffffu) | 0x40000000u;
        entry[1] += heap.entries[2u * last + 1u];
        *heap.last_index = --last;
    }
    return 1;
}

void SpuFree(uint32 address)
{
    uint32 i;
    if (!heap.entries || heap_failed || *heap.last_index > heap.limit)
    {
        heap_failed = 1;
        return;
    }
    for (i = 0; i < heap.limit; ++i)
    {
        uint32 *entry = heap.entries + 2u * i;
        if (entry[0] & 0x40000000u)
            break;
        if (entry[0] == address)
        {
            entry[0] = address | 0x80000000u;
            break;
        }
    }
    spu_heap_collect();
}

sint32 SpuMalloc(sint32 bytes)
{
    uint32 size = (uint32)bytes, i, *entry, *tail, last;
    if (!heap.entries || heap_failed || *heap.last_index > heap.limit)
    {
        heap_failed = 1;
        return -1;
    }
    // The reviewed SDK tests the complement of the mask, not the low bits
    if (size & ~heap.alignment_mask)
        size += heap.alignment_mask;
    size = (uint32)((sint32)size >> heap.address_shift) << heap.address_shift;
    i = 0;
    if (!(heap.entries[0] & 0x40000000u))
    {
        if (!spu_heap_collect())
            return -1;
        for (; i < heap.limit; ++i)
        {
            entry = heap.entries + 2u * i;
            if ((entry[0] & 0x40000000u) || ((entry[0] & 0x80000000u) && entry[1] >= size))
                break;
        }
    }
    if (i >= heap.limit)
        return -1;
    entry = heap.entries + 2u * i;
    if (entry[0] & 0x40000000u)
    {
        if (entry[1] - heap.reserved_bytes < size)
            return -1;
        tail = entry + 2;
        tail[0] = ((entry[0] & 0x0fffffffu) + size) | 0x40000000u;
        tail[1] = entry[1] - size;
        *heap.last_index = i + 1u;
    }
    else if (size < entry[1] && (last = *heap.last_index) < heap.limit)
    {
        tail = heap.entries + 2u * last;
        tail[2] = tail[0];
        tail[3] = tail[1];
        tail[0] = (entry[0] + size) | 0x80000000u;
        tail[1] = entry[1] - size;
        *heap.last_index = last + 1u;
    }
    entry[0] &= 0x0fffffffu;
    entry[1] = size;
    if (!spu_heap_collect())
        return -1;
    return (sint32)heap.entries[2u * i];
}

sint32 spu_reverb_import(const SpuReverbControl *state, SpuReverbWorkCheck check, void *context)
{
    if (!state || state->control > 0xffffu || state->enabled > 1u || ((state->control >> 7) & 1u) != state->enabled)
        return 0;
    xport_audio_lock();
    reverb_control = *state;
    spu_reverb_reset_cursor();
    reverb_work_check = check;
    reverb_work_context = context;
    reverb_incomplete = 0;
    xport_audio_unlock();
    return 1;
}

void spu_reverb_export(SpuReverbControl *state)
{
    if (!state)
        return;
    xport_audio_lock();
    *state = reverb_control;
    xport_audio_unlock();
}

sint32 spu_reverb_incomplete(void)
{
    sint32 result;
    xport_audio_lock();
    result = reverb_incomplete;
    xport_audio_unlock();
    return result;
}

static sint32 spu_reverb_work_occupied(uint32 units)
{
    uint32 i, boundary;
    if (!heap.entries)
        return 0;
    boundary = units << heap.address_shift;
    for (i = 0u; i < heap.capacity; ++i)
    {
        uint32 address = heap.entries[2u * i];
        uint32 base = address & 0x0FFFFFFFu;
        if (address & 0x80000000u)
            continue;
        if (address & 0x40000000u)
            return 0;
        if (base >= boundary || boundary < base + heap.entries[2u * i + 1u])
            return 1;
    }
#ifndef NDEBUG
    fprintf(stderr, "Invalid SPU heap while checking reverb work area\n");
#endif
    abort();
}

sint32 SpuSetReverb(sint32 enabled)
{
    sint32 occupied, result;
    xport_audio_lock();
    if (enabled == 1 && reverb_control.reserved != 1u)
    {
        occupied = reverb_work_check ? reverb_work_check(reverb_work_context, reverb_control.work_address) : spu_reverb_work_occupied(reverb_control.work_address);
        if (occupied < 0)
        {
            reverb_incomplete = 1;
            xport_audio_unlock();
            return -1;
        }
        if (occupied)
            enabled = 0;
    }
    if (enabled == 0 || enabled == 1)
    {
        reverb_control.enabled = (uint32)enabled;
        reverb_control.control = (reverb_control.control & ~0x80u) | ((uint32)enabled << 7);
        reverb_incomplete = 0;
    }
    result = (sint32)reverb_control.enabled;
    xport_audio_unlock();
    /* Native DSP support does not assert device-provenance equivalence */
    if (enabled == 1)
        spu_trace_internal_gap(XPORT_DEVICE_SPU_REVERB, 2u);
    return result;
}

static inline sint16 audio_sample_clamp(sint32 value)
{
    if (value < -32768)
        return -32768;
    if (value > 32767)
        return 32767;
    return (sint16)value;
}

static const sint16 spu_gaussian[512] = {
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    0,     0,     0,     0,     0,     0,     0,     1,     1,     1,     1,     2,     2,     2,     3,     3,     3,     4,     4,     5,     5,     6,     7,     7,     8,     9,     9,     10,    11,    12,    13,    14,    15,    16,    17,    18,    19,    21,    22,    24,    25,    27,    28,    30,    32,    33,    35,    37,    39,    41,    44,    46,    48,    51,    53,    56,    58,    61,    64,    67,    70,    73,    77,    80,    84,    87,    91,    95,    99,    103,   107,   111,   116,   120,   125,   130,   135,   140,   145,   150,   156,   161,   167,   173,   179,   186,   192,   199,   205,   212,   219,   227,   234,   242,   250,   257,   266,   274,   283,   291,   300,   309,   319,   328,   338,   348,   358,   369,   379,   390,   401,   412,
    424,   436,   448,   460,   473,   485,   498,   512,   525,   539,   553,   567,   582,   597,   612,   627,   643,   659,   675,   692,   708,   726,   743,   761,   779,   797,   816,   835,   854,   874,   894,   914,   935,   956,   977,   999,   1020,  1043,  1066,  1089,  1112,  1136,  1160,  1184,  1209,  1234,  1260,  1286,  1312,  1339,  1366,  1394,  1422,  1450,  1479,  1508,  1537,  1567,  1598,  1628,  1660,  1691,  1723,  1756,  1789,  1822,  1856,  1890,  1924,  1959,  1995,  2031,  2067,  2104,  2141,  2179,  2217,  2256,  2295,  2334,  2374,  2415,  2456,  2497,  2539,  2582,  2624,  2668,  2712,  2756,  2801,  2846,  2892,  2938,  2985,  3032,  3079,  3128,  3176,  3225,  3275,  3325,  3376,  3427,  3479,  3531,  3584,  3637,  3691,  3745,  3799,  3855,  3910,  3967,  4023,  4081,  4138,  4197,  4255,  4315,  4374,  4435,  4495,  4557,  4619,  4681,  4744,  4807,
    4871,  4935,  5000,  5065,  5131,  5197,  5264,  5332,  5399,  5468,  5536,  5606,  5676,  5746,  5817,  5888,  5959,  6032,  6104,  6177,  6251,  6325,  6400,  6475,  6550,  6626,  6702,  6779,  6856,  6934,  7012,  7091,  7170,  7249,  7329,  7409,  7490,  7571,  7653,  7735,  7817,  7900,  7983,  8066,  8150,  8234,  8319,  8404,  8489,  8575,  8661,  8748,  8834,  8922,  9009,  9097,  9185,  9273,  9362,  9451,  9541,  9630,  9720,  9811,  9901,  9992,  10083, 10174, 10266, 10358, 10450, 10542, 10635, 10727, 10820, 10913, 11007, 11100, 11194, 11288, 11382, 11476, 11571, 11665, 11760, 11855, 11950, 12045, 12140, 12236, 12331, 12427, 12522, 12618, 12714, 12809, 12905, 13001, 13097, 13193, 13289, 13385, 13481, 13577, 13673, 13769, 13865, 13961, 14056, 14152, 14248, 14343, 14439, 14534, 14630, 14725, 14820, 14915, 15010, 15104, 15199, 15293, 15387, 15481, 15575, 15669, 15762, 15855,
    15948, 16041, 16133, 16226, 16317, 16409, 16500, 16592, 16682, 16773, 16863, 16953, 17042, 17131, 17220, 17308, 17396, 17484, 17571, 17658, 17744, 17830, 17916, 18001, 18086, 18170, 18254, 18337, 18420, 18502, 18584, 18665, 18746, 18826, 18905, 18985, 19063, 19141, 19219, 19295, 19372, 19447, 19522, 19597, 19671, 19744, 19816, 19888, 19959, 20030, 20100, 20169, 20238, 20306, 20373, 20439, 20505, 20570, 20634, 20698, 20760, 20822, 20884, 20944, 21004, 21063, 21121, 21178, 21235, 21290, 21345, 21399, 21452, 21505, 21556, 21607, 21657, 21706, 21754, 21801, 21848, 21893, 21938, 21982, 22025, 22066, 22107, 22148, 22187, 22225, 22262, 22299, 22334, 22369, 22402, 22435, 22467, 22498, 22527, 22556, 22584, 22611, 22637, 22662, 22686, 22709, 22731, 22752, 22772, 22791, 22809, 22826, 22842, 22857, 22872, 22885, 22897, 22908, 22918, 22927, 22935, 22942, 22948, 22953, 22957, 22960, 22962, 22963,
};

/* Types. */
typedef struct SPU_VOICE_REGISTERS
{
    sint16 volume_left;
    sint16 volume_right;
    uint16 pitch;
    uint16 start_address;
    uint16 adsr1;
    uint16 adsr2;
    uint16 repeat_address;
} SPU_VOICE_REGISTERS;

typedef struct SPU_VOICE
{
    SPU_VOICE_REGISTERS registers;
    uint32 current_address;
    uint32 repeat_address;
    uint32 phase;
    uint64 sample_position;
    sint16 history1;
    sint16 history2;
    sint16 decoded[31];
    uint8 decoded_index;
    uint8 decoded_valid;
    uint8 block_flags;
    uint8 first_block;
    uint8 ignore_loop_address;
    uint16 envelope;
    SPU_ADSR_PHASE adsr_phase;
    uint32 envelope_counter;
    uint16 envelope_increment;
    sint16 envelope_step;
    uint8 envelope_rate;
    uint8 envelope_decreasing;
    uint8 envelope_exponential;
} SPU_VOICE;

typedef struct SPU_STATE
{
    uint8 ram[SPU_RAM_SIZE];
    SPU_VOICE voices[SPU_VOICE_COUNT];
    uint32 end_flags;
    uint32 key_status;
    uint32 key_on_pending;
    uint32 key_off_pending;
    uint16 mmio_transfer_control;
    uint16 mmio_transfer_address;
    uint16 mmio_fifo[32];
    uint32 mmio_fifo_count;
    uint32 pitch_modulation;
    uint32 noise_mask, noise_count, noise_level;
    uint32 decoded_capture_cursor;
    uint16 irq_address;
    uint8 irq_flag;
    uint8 irq_pending;
    uint32 reverb_voice_mask;
    uint32 reverb_base;
    uint32 reverb_cursor;
    uint32 reverb_phase;
    uint32 reverb_history_index;
    sint16 reverb_input[2][39];
    sint16 reverb_output[2][39];
    sint16 master_left;
    sint16 master_right;
    sint16 cd_left;
    sint16 cd_right;
    sint16 ext_left;
    sint16 ext_right;
    uint8 cd_mix_controlled;
    uint8 native_muted;
    SpuNativeTransferState transfer;
    SpuKeyControl key_control;
    uint16 sample_notes[SPU_VOICE_COUNT];
    uint32 heap_last_idx;
    uint8 initialized;
    uint8 sample_clock_from_wait;
} SPU_STATE;

static SPU_STATE spu;
SpuNativeTransferState *const spu_native_transfer_state = &spu.transfer;
SpuKeyControl *const spu_key_control = &spu.key_control;
uint16 *const spu_sample_notes = spu.sample_notes;

static void update_adsr(SPU_VOICE *voice);

sint32 spu_load_handoff_file(const char *path)
{
    uint32 words[51 + 24 * 54], index, cursor = 51;
    FILE *file = path ? xport_fopen(path, "rb") : NULL;
    uint64 sample_counter = 0;
    uint8 trailing;
    sint32 ok;
    if (!file)
        return 0;
    ok = fread(words, sizeof(words), 1, file) == 1 && fread(&sample_counter, sizeof(sample_counter), 1, file) == 1 && fread(&trailing, 1, 1, file) == 0;
    if (fclose(file) || !ok)
        return 0;
    xport_audio_lock();
    reverb_control.control = words[0];
    reverb_control.enabled = (words[0] >> 7) & 1;
    reverb_control.work_address = words[9] >> 2;
    reverb_control.depth_left = (sint16)words[17];
    reverb_control.depth_right = (sint16)words[18];
    for (index = 0; index < 32; ++index)
        reverb_control.parameters[index] = (uint16)words[19 + index];
    spu.transfer.address_units = (uint16)(words[2] >> 3);
    transfer_byte_address = words[2];
    spu.irq_address = (uint16)words[3];
    spu.irq_flag = (words[1] >> 6) & 1;
    spu.irq_pending = 0;
    spu.end_flags = words[4];
    spu.key_on_pending = words[5];
    spu.key_off_pending = words[6];
    spu.pitch_modulation = words[7];
    spu.reverb_voice_mask = words[8];
    spu.reverb_base = words[9] << 1;
    spu.reverb_cursor = (words[10] - words[9]) << 1;
    spu.master_left = (sint16)words[11];
    spu.master_right = (sint16)words[12];
    spu.cd_left = (sint16)words[13];
    spu.cd_right = (sint16)words[14];
    spu.ext_left = (sint16)words[15];
    spu.ext_right = (sint16)words[16];
    spu.cd_mix_controlled = 1;
    spu.key_status = 0;
    spu.sample_clock_from_wait = 1;
    for (index = 0; index < 24; ++index)
    {
        SPU_VOICE *voice = spu.voices + index;
        uint32 sample;
        voice->registers.volume_left = (sint16)words[cursor];
        voice->registers.volume_right = (sint16)words[cursor + 1];
        voice->registers.pitch = (uint16)words[cursor + 2];
        voice->registers.start_address = (uint16)words[cursor + 3];
        voice->registers.adsr1 = (uint16)words[cursor + 4];
        voice->registers.adsr2 = (uint16)words[cursor + 5];
        voice->envelope = (uint16)words[cursor + 6];
        voice->registers.repeat_address = (uint16)words[cursor + 7];
        voice->repeat_address = words[cursor + 7] << 3;
        voice->current_address = words[cursor + 8] << 3;
        voice->phase = words[cursor + 9];
        voice->sample_position = words[cursor + 9];
        voice->block_flags = (uint8)words[cursor + 10];
        voice->first_block = (uint8)words[cursor + 11];
        voice->decoded_valid = (uint8)words[cursor + 12];
        voice->ignore_loop_address = (uint8)words[cursor + 13];
        voice->adsr_phase = (SPU_ADSR_PHASE)words[cursor + 14];
        voice->envelope_counter = words[cursor + 15];
        voice->envelope_increment = (uint16)words[cursor + 16];
        voice->envelope_step = (sint16)words[cursor + 17];
        voice->envelope_rate = (uint8)words[cursor + 18];
        voice->envelope_decreasing = (uint8)words[cursor + 19];
        voice->envelope_exponential = (uint8)words[cursor + 20];
        voice->history1 = (sint16)words[cursor + 21];
        voice->history2 = (sint16)words[cursor + 22];
        for (sample = 0; sample < 31; ++sample)
            voice->decoded[sample] = (sint16)words[cursor + 23 + sample];
        if (voice->adsr_phase != SPU_ADSR_OFF)
            spu.key_status |= 1u << index;
        cursor += 54;
    }
    xport_audio_unlock();
    return 1;
}

/* Latch RAM IRQs without entering game code on the mixer thread */
static void spu_irq_check_range(uint32 address, uint32 bytes)
{
    uint32 target = (uint32)spu.irq_address << 3;
    if ((reverb_control.control & 0x40) && !spu.irq_flag && ((target - address) & (SPU_RAM_SIZE - 1)) < bytes)
    {
        spu.irq_flag = 1;
        spu.irq_pending = 1;
    }
}

static void spu_irq_check_late(void)
{
    uint32 index;
    spu_irq_check_range(transfer_byte_address, 2);
    for (index = 0; index < SPU_VOICE_COUNT && !spu.irq_flag; ++index)
        if (spu.voices[index].decoded_valid)
            spu_irq_check_range(spu.voices[index].current_address, 16);
}

/* Call only from the game thread after releasing the mixer lock */
void spu_dispatch_pending_irq(void)
{
    uint32 pending;
    xport_audio_lock();
    pending = spu.irq_pending && spu.irq_flag && (reverb_control.control & 0x40);
    spu.irq_pending = 0;
    xport_audio_unlock();
    if (pending)
        psx_interrupt_raise(9);
}

static void spu_reverb_reset_cursor(void)
{
    spu.reverb_base = (reverb_control.work_address << 3) & (SPU_RAM_SIZE - 2u);
    spu.reverb_cursor = 0u;
}


enum
{
    VAB_BANK_COUNT = 16,
    VAB_PROGRAM_COUNT = 128,
    VAB_TONES_PER_PROGRAM = 16,
    SPU_MIX_CHUNK_FRAMES = 256
};

typedef struct VAB_HEADER_RECORD
{
    uint32 magic;
    uint8 reserved_04[0x0e];
    uint16 program_count;
    uint16 tone_count;
    uint16 sample_count;
    uint8 master_volume;
    uint8 master_pan;
    uint8 reserved_1a[6];
} VAB_HEADER_RECORD;

typedef struct VAB_PROGRAM_RECORD
{
    uint8 tone_count;
    uint8 volume;
    uint8 priority;
    uint8 mode;
    uint8 pan;
    uint8 reserved_05[0x0b];
} VAB_PROGRAM_RECORD;

typedef struct VAB_TONE_RECORD
{
    uint8 priority;
    uint8 mode;
    uint8 volume;
    uint8 pan;
    uint8 center;
    uint8 shift;
    uint8 note_min;
    uint8 note_max;
    uint8 reserved_08[4];
    uint8 pitch_bend_min;
    uint8 pitch_bend_max;
    uint8 reserved_0e[2];
    uint16 adsr1;
    uint16 adsr2;
    sint16 program;
    sint16 sample;
    uint8 reserved_18[8];
} VAB_TONE_RECORD;

typedef VAB_PROGRAM_RECORD VAB_PROGRAM;

typedef VAB_TONE_RECORD VAB_TONE;

typedef struct VAB_BANK
{
    VAB_PROGRAM programs[VAB_PROGRAM_COUNT];
    VAB_TONE tones[VAB_PROGRAM_COUNT][VAB_TONES_PER_PROGRAM];
    uint32 sample_addresses[256];
    uint32 body_address;
    uint32 body_size;
    VAB_HEADER_RECORD header;
    uint8 valid;
    uint8 transferred;
} VAB_BANK;

// Separate staging slots for the native and sticky parsers under the audio lock
static VAB_BANK vab_parse_banks[2];

typedef struct LIBSND_VOICE
{
    sint16 bank;
    sint16 program;
    sint16 tone;
    uint16 note;
    uint16 age;
    uint8 direct_keyon;
    uint8 priority;
    uint8 keyed;
} LIBSND_VOICE;

typedef struct IMA_ADPCM_STREAM
{
    const uint8 *file_data;
    uint32 file_size;
    const uint8 *audio_data;
    uint32 audio_size;
    uint32 total_frames;
    uint32 decoded_frames;
    uint32 block_offset;
    uint32 sample_in_block;
    uint16 block_align;
    uint16 samples_per_block;
    uint16 channels;
    uint32 sample_rate;
    sint32 predictor[2];
    sint32 step_index[2];
    sint32 block_loaded;
} IMA_ADPCM_STREAM;

typedef struct CD_AUDIO_STATE
{
    uint8 *data;
    IMA_ADPCM_STREAM stream;
    XaState file_xa_state;
    sint16 file_xa_samples[XA_MAX_SECTOR_FRAMES * 2];
    uint32 file_xa_frames;
    uint32 file_xa_position;
    uint32 file_xa_start_sector;
    uint8 file_xa;
    uint8 file_pending;
    FILE *file;
    uint32 file_received;
    CdlATV mix;
    sint32 track;
    uint8 active;
    uint8 paused;
    uint8 muted;
    uint8 repeat;
} CD_AUDIO_STATE;

typedef struct SPU_STREAM_STATE
{
    uint32 state;
    PsxInterruptCallback callbacks[5];
    uint32 pending[9];
    uint32 current_voice[5];
    SpuStEnv environment;
    uint32 completion[4];
} SPU_STREAM_STATE;

typedef struct LIBSND_STATE
{
    VAB_BANK banks[VAB_BANK_COUNT];
    LIBSND_VOICE voices[SPU_VOICE_COUNT];
    SPU_STREAM_STATE streaming;
    uint32 voice_protection_address;
    uint32 voice_count_address;
    uint32 voice_volume_address;
    uint32 voice_update_flags_address;
    uint32 sequence_table_address;
    uint32 sequence_handlers_address;
    uint32 sequence_mark_table_address;
    SpuVmGuestBinding vm_guest;
    CD_AUDIO_STATE cd;
    PsxInterruptCallback data_callback;
    CdlCB ready_callback;
    CdlCB cd_end_callback;
    CdlCB sync_callback;
    CdlCB read_callback;

    struct
    {
        uint8 kind, status, has_response;
        uint8 response[8];
    } pending_callbacks[256];

    uint32 pending_callback_read, pending_callback_count;

    struct
    {
        uint8 interrupt;
        uint8 length;
        uint8 response[8];
    } pending_sdk_callbacks[256];

    uint32 pending_sdk_read;
    uint32 pending_sdk_count;
    uint32 cd_bus_delay;
    uint32 next_sound_ram_address;
    uint32 tick_accumulator;
    uint32 tick_count;
    sint32 tick_mode;
    sint32 ticks_running;
    sint32 vab_transfer_complete;
    uint8 cd_mode;
    uint8 cd_filter_file;
    uint8 cd_filter_channel;
    uint8 initialized;
} LIBSND_STATE;

/* Variables. */
static LIBSND_STATE libsnd;

SpuStEnv *SpuStInit(void)
{
    uint32 index;
    SPU_STREAM_STATE *stream = &libsnd.streaming;
    xport_audio_lock();
    stream->state = 0x10;
    for (index = 0; index < 5; ++index)
        stream->callbacks[index] = NULL;
    for (index = 0; index < 9; ++index)
        stream->pending[index] = 0;
    for (index = 0; index < 5; ++index)
        stream->current_voice[index] = SPU_VOICE_COUNT;
    for (index = 0; index < SPU_VOICE_COUNT; ++index)
    {
        stream->environment.voice[index].status = 6;
        stream->environment.voice[index].last_size = 0;
        stream->environment.voice[index].buf_addr = 0;
        stream->environment.voice[index].data_addr = 0;
    }
    stream->environment.size = 0;
    for (index = 0; index < 4; ++index)
        stream->completion[index] = 0;
    xport_audio_unlock();
    return &stream->environment;
}

enum
{
    CD_CALLBACK_SYNC,
    CD_CALLBACK_READY,
    CD_CALLBACK_READ
};

static void cd_queue_callback(uint32 kind, uint8 status, const uint8 *response)
{
    uint32 index;
    xport_audio_lock();
    if (libsnd.pending_callback_count == 256)
        abort();
    index = (libsnd.pending_callback_read + libsnd.pending_callback_count) & 255;
    libsnd.pending_callbacks[index].kind = (uint8)kind;
    libsnd.pending_callbacks[index].status = status;
    memset(libsnd.pending_callbacks[index].response, 0, sizeof(libsnd.pending_callbacks[index].response));
    if (response != NULL)
        memcpy(libsnd.pending_callbacks[index].response, response, kind == CD_CALLBACK_READ ? 8u : 1u);
    libsnd.pending_callbacks[index].has_response = response != NULL;
    ++libsnd.pending_callback_count;
    xport_audio_unlock();
    psx_wait_signal_service(6);
    psx_interrupt_raise(2);
    psx_sync_callbacks();
    if (psx_callbacks_allowed())
        cd_dispatch_ready_event();
}

static CdlLOC cd_toc[100];

static sint32 cd_toc_track_count;


const uint16 spu_sequence_voice_defaults[8] = {0x0, 0x0, 0x1000, 0x3000, 0xbf, 0x0, 0x0, 0x0};
const uint16 spu_sequence_common_defaults[16] = {0x3fff, 0x3fff, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};

const uint16 spu_note_pitch_table[192] = {
    0x1000, 0x100e, 0x101d, 0x102c, 0x103b, 0x104a, 0x1059, 0x1068, 0x1078, 0x1087, 0x1096, 0x10a5, 0x10b5, 0x10c4, 0x10d4, 0x10e3, 0x10f3, 0x1103, 0x1113, 0x1122, 0x1132, 0x1142, 0x1152, 0x1162, 0x1172, 0x1182, 0x1193, 0x11a3, 0x11b3, 0x11c4, 0x11d4, 0x11e5, 0x11f5, 0x1206, 0x1216, 0x1227, 0x1238, 0x1249, 0x125a, 0x126b, 0x127c, 0x128d, 0x129e, 0x12af, 0x12c1, 0x12d2, 0x12e3, 0x12f5, 0x1306, 0x1318, 0x132a, 0x133c, 0x134d, 0x135f, 0x1371, 0x1383, 0x1395, 0x13a7, 0x13ba, 0x13cc, 0x13de, 0x13f1, 0x1403, 0x1416, 0x1428, 0x143b, 0x144e, 0x1460, 0x1473, 0x1486, 0x1499, 0x14ac, 0x14bf, 0x14d3, 0x14e6, 0x14f9, 0x150d, 0x1520, 0x1534, 0x1547, 0x155b, 0x156f, 0x1583, 0x1597, 0x15ab, 0x15bf, 0x15d3, 0x15e7, 0x15fb, 0x1610, 0x1624, 0x1638, 0x164d, 0x1662, 0x1676, 0x168b,
    0x16a0, 0x16b5, 0x16ca, 0x16df, 0x16f4, 0x170a, 0x171f, 0x1734, 0x174a, 0x175f, 0x1775, 0x178b, 0x17a1, 0x17b6, 0x17cc, 0x17e2, 0x17f9, 0x180f, 0x1825, 0x183b, 0x1852, 0x1868, 0x187f, 0x1896, 0x18ac, 0x18c3, 0x18da, 0x18f1, 0x1908, 0x191f, 0x1937, 0x194e, 0x1965, 0x197d, 0x1995, 0x19ac, 0x19c4, 0x19dc, 0x19f4, 0x1a0c, 0x1a24, 0x1a3c, 0x1a55, 0x1a6d, 0x1a85, 0x1a9e, 0x1ab7, 0x1acf, 0x1ae8, 0x1b01, 0x1b1a, 0x1b33, 0x1b4c, 0x1b66, 0x1b7f, 0x1b98, 0x1bb2, 0x1bcc, 0x1be5, 0x1bff, 0x1c19, 0x1c33, 0x1c4d, 0x1c67, 0x1c82, 0x1c9c, 0x1cb7, 0x1cd1, 0x1cec, 0x1d07, 0x1d22, 0x1d3d, 0x1d58, 0x1d73, 0x1d8e, 0x1da9, 0x1dc5, 0x1de0, 0x1dfc, 0x1e18, 0x1e34, 0x1e50, 0x1e6c, 0x1e88, 0x1ea4, 0x1ec1, 0x1edd, 0x1efa, 0x1f16, 0x1f33, 0x1f50, 0x1f6d, 0x1f8a, 0x1fa7, 0x1fc5, 0x1fe2,
};

uint16 _spu_note2pitch(uint16 center_note, uint16 center_fine, uint16 note, uint16 note_fine)
{
    sint32 fine = ((sint32)center_fine + note_fine) / 8;
    sint32 semitone = (sint16)(fine / 16 + note + 60 - center_note);
    sint32 octave = semitone / 12;
    sint32 remainder = semitone - octave * 12;
    sint32 shift = (sint16)(octave - 5);
    uint32 index = (uint32)(remainder * 16 + fine % 16);
    uint32 pitch;
    if (index >= 192)
    {
#ifndef NDEBUG
        fprintf(stderr, "_spu_note2pitch: Pitch table index out of range: %u\n", index);
#else
        (void)0;
#endif
        abort();
    }
    pitch = spu_note_pitch_table[index];
    if (shift > 0)
        pitch <<= (uint32)shift & 31;
    else if (shift < 0)
        pitch >>= (uint32)(0u - (uint32)shift) & 31;
    return (uint16)pitch;
}

uint32 _SsUtResolveADSR(uint16 first, uint16 second, uint16 *fields)
{
    uint32 sustain = (second >> 6) & 0x7F;
    fields[5] = first & 0x8000;
    fields[6] = second & 0x8000;
    fields[8] = second & 0x4000;
    fields[7] = second & 0x20;
    fields[0] = (first >> 8) & 0x7F;
    fields[1] = (first >> 4) & 15;
    fields[2] = first & 15;
    fields[3] = sustain;
    fields[4] = second & 31;
    return sustain;
}

uint32 _SsUtBuildADSR(const uint16 *fields, uint16 *first, uint16 *second)
{
    uint32 mode = fields[6] != 0 ? 0xFFFF8000u : 0;
    uint32 attack_mode;
    uint32 attack, decay, level, sustain, release;
    uint32 result;
    if (fields[8] != 0)
        mode |= 0x4000;
    attack_mode = fields[5] != 0 ? 0xFFFF8000u : 0;
    attack = (fields[0] << 8) & 0x7F00;
    decay = (fields[1] << 4) & 0xF0;
    level = fields[2] & 15;
    sustain = (fields[3] << 6) & 0x1FC0;
    release = fields[4] & 31;
    *first = attack_mode | attack | decay | level;
    result = mode | sustain;
    *second = result | release;
    return result;
}

/* Functions. */
static void spu_key_off(uint32 voice_mask);
static void spu_key_on(uint32 voice_mask);
static void spu_init(void);
static void spu_init_native_transfer(void);
static void spu_shutdown(void);
static void libsnd_reset(void);
static void cd_audio_stop_locked(void);

#if defined(XPORT_NATIVE_VM) && XPORT_NATIVE_VM
static void libsnd_initialize_native(void);
#endif

static void libsnd_initialize_runtime(void)
{
    xport_audio_lock();
    if (libsnd.initialized)
    {
        xport_audio_unlock();
        return;
    }
    spu_init();
    libsnd_reset();
    libsnd.initialized = 1;
    xport_audio_unlock();
    xport_audio_init();
}

void SsInitHot(void)
{
#if defined(XPORT_NATIVE_VM) && XPORT_NATIVE_VM
    ResetCallback();
    libsnd_initialize_runtime();
    SpuInit();
    spu_sequence_scheduler_initialize(&spu_sequence_scheduler, spu_sequence_vm_initialize);
#else
    libsnd_initialize_runtime();
#endif
}

void SsInit(void)
{
#if defined(XPORT_NATIVE_VM) && XPORT_NATIVE_VM
    ResetCallback();
    libsnd_initialize_runtime();
    SpuInit();
    SpuClearReverbWorkArea(7);
    libsnd_initialize_native();
#else
    libsnd_initialize_runtime();
#endif
}

void SsEnd(void)
{
    xport_audio_lock();
    spu_key_off(SPU_ALLCH);
    cd_audio_stop_locked();
    xport_audio_unlock();
}

void SsQuit(void)
{
    xport_audio_shutdown();
#if defined(XPORT_NATIVE_SEQUENCE) && XPORT_NATIVE_SEQUENCE
    spu_sequence_reset();
#endif
    xport_audio_lock();
    cd_audio_stop_locked();
    memset(&libsnd, 0, sizeof(libsnd));
    spu_shutdown();
    xport_audio_unlock();
}

void SsSetMVol(sint16 left, sint16 right)
{
    SpuCommonAttr attr;
    memset(&attr, 0, sizeof(attr));
    attr.mask = SPU_COMMON_MVOLL | SPU_COMMON_MVOLR;
    attr.mvol.left = (sint16)(left * 129);
    attr.mvol.right = (sint16)(right * 129);
    SpuSetCommonAttr(&attr);
}

void SsSetSerialVol(sint8 serial, sint16 left, sint16 right)
{
    sint32 channel = (sint8)serial;
    sint16 l = (sint16)left, r = (sint16)right;
    SpuCommonAttr attr;
    if ((uint32)channel > 1)
        abort();
    if (l >= 128)
        l = 127;
    if (r >= 128)
        r = 127;
    memset(&attr, 0, sizeof(attr));
    attr.mask = channel == 0 ? 0xC0 : 0xC00;
    if (channel == 0)
    {
        attr.cd.volume.left = (sint16)((sint32)l * 258);
        attr.cd.volume.right = (sint16)((sint32)r * 258);
    }
    else
    {
        attr.ext.volume.left = (sint16)((sint32)l * 258);
        attr.ext.volume.right = (sint16)((sint32)r * 258);
    }
    SpuSetCommonAttr(&attr);
}

void SsSetTickMode(sint32 mode)
{
#if defined(XPORT_NATIVE_SEQUENCE) && XPORT_NATIVE_SEQUENCE
    spu_sequence_set_tick_mode(mode);
#endif
    xport_audio_lock();
    libsnd.tick_mode = mode;
    libsnd.tick_accumulator = 0;
    xport_audio_unlock();
}

void SsStart(void)
{
    xport_audio_lock();
    libsnd.ticks_running = libsnd.tick_mode == SS_TICK60;
    xport_audio_unlock();
#if defined(XPORT_NATIVE_SEQUENCE) && XPORT_NATIVE_SEQUENCE
    spu_sequence_start_timer(0);
#endif
}

static void spu_init(void)
{
    uint32 voice;
    xa_remainder_ready = 0;
    memset(&spu, 0, sizeof(spu));
    for (voice = 0; voice < SPU_VOICE_COUNT; ++voice)
        spu.sample_notes[voice] = SPU_INITIAL_SAMPLE_NOTE;
    spu.master_left = 0x3fff;
    spu.master_right = 0x3fff;
    spu.cd_left = 0;
    spu.cd_right = 0;
    spu.initialized = 1;
    spu.noise_level = 1;
    reverb_control.control = 0xC000;
    reverb_control.enabled = 0;
    libsnd.cd.mix.val0 = 255;
    libsnd.cd.mix.val1 = 0;
    libsnd.cd.mix.val2 = 255;
    libsnd.cd.mix.val3 = 0;
    spu_init_native_transfer();
}

static void spu_shutdown(void)
{
    xa_remainder_ready = 0;
    memset(&spu, 0, sizeof(spu));
}

sint32 spu_upload(uint32 byte_address, const void *source, uint32 byte_count)
{
    XportDeviceOperation operation = {0};
    XportMemoryRegion memory_region = XPORT_MEMORY_NONE;
    XportDeviceRegion source_region = XPORT_DEVICE_REGION_NONE;
    uint32 source_offset = 0u;
    if (!spu.initialized || source == 0 || byte_address >= SPU_RAM_SIZE || byte_count > SPU_RAM_SIZE - byte_address)
        return 0;
    if (xport_device_trace_enabled())
    {
        xport_memory_pointer_identity(source, byte_count, &memory_region, &source_offset);
        source_region = spu_device_region(memory_region);
        if (!xport_device_trace_begin(&operation, transfer_operation.active ? transfer_operation.id : 0u, XPORT_DEVICE_SPU, XPORT_DEVICE_SPU_UPLOAD, XPORT_DEVICE_DIRECTION_TO_DEVICE, 4u, 0u))
            return 0;
        xport_device_trace_stage(&operation, XPORT_DEVICE_SUBMIT, XPORT_DEVICE_EVIDENCE_EXACT, source_region, source_offset, XPORT_DEVICE_REGION_SPU_RAM, byte_address, byte_count, 0xffffffffu);
    }
    xport_audio_lock();
    memcpy(spu.ram + byte_address, source, byte_count);
    spu_irq_check_range(byte_address, byte_count);
    xport_audio_unlock();
    if (operation.active)
    {
        xport_device_trace_stage(&operation, XPORT_DEVICE_WRITE, XPORT_DEVICE_EVIDENCE_EXACT, source_region, source_offset, XPORT_DEVICE_REGION_SPU_RAM, byte_address, byte_count, 0xffffffffu);
        xport_device_trace_complete(&operation, 1);
    }
    return 1;
}

sint32 spu_download(uint32 byte_address, void *destination, uint32 byte_count)
{
    if (!spu.initialized || destination == 0 || byte_address >= SPU_RAM_SIZE || byte_count > SPU_RAM_SIZE - byte_address)
        return 0;
    xport_audio_lock();
    memcpy(destination, spu.ram + byte_address, byte_count);
    spu_irq_check_range(byte_address, byte_count);
    xport_audio_unlock();
    return 1;
}

uint32 spu_read_blocks(void *destination, uint16 address_units, uint16 blocks)
{
    uint8 *output = (uint8 *)destination;
    uint32 address = (uint32)address_units << 3;
    uint32 remaining = (uint32)blocks << 6;
    uint32 bytes;
    if (native_transfer_binding != SPU_TRANSFER_NATIVE || transfer_failed || transfer_pending || transfer_operation.active || !destination || blocks == 0 || !spu.initialized)
        abort();
    xport_audio_lock();
    while (remaining != 0)
    {
        bytes = SPU_RAM_SIZE - address;
        if (bytes > remaining)
            bytes = remaining;
        if (!spu_download(address, output, bytes))
        {
            xport_audio_unlock();
            abort();
        }
        output += bytes;
        remaining -= bytes;
        address = 0;
    }
    *transfer.completion = 1;
    xport_audio_unlock();
    spu_sequence_vm_dma();
    return 1;
}

sint32 spu_stop_transfer(void)
{
    uint16 control;
    xport_audio_lock();
    control = r_u16(SPU_REGISTER_BASE + 426);
    w_u16(SPU_REGISTER_BASE + 426, control & 0x7FCF);
    w_u16(SPU_REGISTER_BASE + 426, control & 0xFFCF);
    xport_audio_unlock();
    return 0;
}

sint32 SpuReadDecodedData(void *destination, sint32 flags)
{
    uint32 source = flags == 6 ? 2048 : 0;
    uint32 bytes = flags == 5 || flags == 6 ? 2048 : 4096;
    sint32 bank;
    if (!destination || !spu.initialized || SpuIsTransferCompleted(1) != 1)
        return -1;
    xport_audio_lock();
    if (!spu_download(source, (uint8 *)destination + source, bytes))
    {
        xport_audio_unlock();
        return -1;
    }
    *transfer.completion = 1;
    bank = spu.decoded_capture_cursor >= 256;
    xport_audio_unlock();
    return bank;
}

sint32 spu_load_ram_file(const char *path)
{
    FILE *file;
    size_t size;

    if (!path || !*path || !spu.initialized)
        return 0;
    file = xport_fopen(path, "rb");
    if (!file)
        return 0;
    xport_audio_lock();
    size = fread(spu.ram, 1, sizeof(spu.ram), file);
    xport_audio_unlock();
    if (size != sizeof(spu.ram)
#ifndef NDEBUG
        || fgetc(file) != EOF
#endif
    )
    {
        fclose(file);
        return 0;
    }
    return fclose(file) == 0;
}

static sint32 spu_mmio_read16(uint32 physical_address, uint16 *value)
{
    uint32 offset;
    uint32 voice;
    uint32 index;
    SPU_VOICE *state;

    if (!value || physical_address < 0x1F801C00u || physical_address >= 0x1F801E00u || (physical_address & 1u))
        return 0;
    offset = physical_address - 0x1F801C00u;
    if (offset < 24u * 16u)
    {
        voice = offset / 16u;
        index = (offset & 15u) / 2u;
        state = spu.voices + voice;
        switch (index)
        {
            case 0:
                *value = (uint16)state->registers.volume_left;
                break;
            case 1:
                *value = (uint16)state->registers.volume_right;
                break;
            case 2:
                *value = state->registers.pitch;
                break;
            case 3:
                *value = state->registers.start_address;
                break;
            case 4:
                *value = state->registers.adsr1;
                break;
            case 5:
                *value = state->registers.adsr2;
                break;
            case 6:
                *value = state->envelope;
                break;
            case 7:
                *value = state->registers.repeat_address;
                break;
            default:
                return 0;
        }
        return 1;
    }
    switch (physical_address)
    {
        case 0x1F801D84u:
            *value = (uint16)reverb_control.depth_left;
            return 1;
        case 0x1F801D86u:
            *value = (uint16)reverb_control.depth_right;
            return 1;
        case 0x1F801D88u:
            *value = (uint16)spu.key_on_pending;
            return 1;
        case 0x1F801D8Au:
            *value = (uint16)(spu.key_on_pending >> 16);
            return 1;
        case 0x1F801D8Cu:
            *value = (uint16)spu.key_off_pending;
            return 1;
        case 0x1F801D8Eu:
            *value = (uint16)(spu.key_off_pending >> 16);
            return 1;
        case 0x1F801D98u:
            *value = (uint16)spu.reverb_voice_mask;
            return 1;
        case 0x1F801D9Au:
            *value = (uint16)(spu.reverb_voice_mask >> 16);
            return 1;
        case 0x1F801DA2u:
            *value = (uint16)reverb_control.work_address;
            return 1;
        case 0x1F801DA6u:
            *value = spu.mmio_transfer_address;
            return 1;
        case 0x1F801DACu:
            *value = spu.mmio_transfer_control;
            return 1;
        case 0x1F801DAEu:
            *value = (uint16)((reverb_control.control & 0x3F) | (spu.irq_flag ? 0x40 : 0));
            return 1;
        case 0x1F801D80u:
            *value = (uint16)spu.master_left;
            return 1;
        case 0x1F801D82u:
            *value = (uint16)spu.master_right;
            return 1;
        case 0x1F801DAAu:
            *value = (uint16)reverb_control.control;
            return 1;
        case 0x1F801DA4u:
            *value = spu.irq_address;
            return 1;
        case 0x1F801DB0u:
            *value = (uint16)spu.cd_left;
            return 1;
        case 0x1F801DB2u:
            *value = (uint16)spu.cd_right;
            return 1;
        case 0x1F801DB4u:
            *value = (uint16)spu.ext_left;
            return 1;
        case 0x1F801DB6u:
            *value = (uint16)spu.ext_right;
            return 1;
        case 0x1F801D9Cu:
            *value = (uint16)spu.end_flags;
            return 1;
        case 0x1F801D90:
            *value = (uint16)spu.pitch_modulation;
            return 1;
        case 0x1F801D92:
            *value = (uint16)(spu.pitch_modulation >> 16);
            return 1;
        case 0x1F801D94:
            *value = (uint16)spu.noise_mask;
            return 1;
        case 0x1F801D96:
            *value = (uint16)(spu.noise_mask >> 16);
            return 1;
        case 0x1F801D9Eu:
            *value = (uint16)(spu.end_flags >> 16);
            return 1;
        default:
            if (physical_address >= 0x1F801DC0u)
            {
                *value = reverb_control.parameters[(physical_address - 0x1F801DC0u) / 2u];
                return 1;
            }
            return 0;
    }
}

static sint32 spu_synchronize_to_ticks(uint64 target)
{
    (void)target;
    return 1;
}

static void spu_synchronize_device_clock(void)
{
}


static sint32 spu_mmio_timed_read16(uint32 physical_address, uint16 *value)
{
    spu_synchronize_device_clock();
    if (!spu_mmio_read16(physical_address, value))
        return 0;
    return 1;
}

void spu_mmio_initialize(void)
{
    xport_audio_lock();
    if (!spu.initialized)
        spu_init();
    xport_audio_unlock();
}

sint32 spu_mmio_read(void *user, uint32 physical_address, uint32 width, uint32 *value)
{
    uint16 low;
    uint16 high;

    (void)user;
    if (!value || (width != 1u && width != 2u && width != 4u))
        return 0;
    xport_audio_lock();
    if (width == 1u)
    {
        if (!spu_mmio_timed_read16(physical_address & ~1u, &low))
        {
            xport_audio_unlock();
            return 0;
        }
        *value = (low >> ((physical_address & 1u) * 8u)) & 0xFFu;
    }
    else if (width == 2u)
    {
        if (!spu_mmio_timed_read16(physical_address, &low))
        {
            xport_audio_unlock();
            return 0;
        }
        *value = low;
    }
    else
    {
        if (!spu_mmio_timed_read16(physical_address, &low) || !spu_mmio_timed_read16(physical_address + 2u, &high))
        {
            xport_audio_unlock();
            return 0;
        }
        *value = (uint32)low | ((uint32)high << 16);
    }
    xport_audio_unlock();
    return 1;
}

static sint32 spu_mmio_common_writable(uint32 address)
{
    if (address >= 0x1F801DC0u && address < 0x1F801E00u && !(address & 1))
        return 1;
    if (address >= 0x1F801C00 && address < 0x1F801D80 && !(address & 1))
        return 1;
    switch (address)
    {
        case 0x1F801D84u:
        case 0x1F801D86u:
        case 0x1F801D88u:
        case 0x1F801D8Au:
        case 0x1F801D8Cu:
        case 0x1F801D8Eu:
        case 0x1F801D98u:
        case 0x1F801D9Au:
        case 0x1F801DA2u:
        case 0x1F801DA6u:
        case 0x1F801DA8u:
        case 0x1F801DACu:
        case 0x1F801D80u:
        case 0x1F801D82u:
        case 0x1F801D90:
        case 0x1F801D92:
        case 0x1F801D94:
        case 0x1F801D96:
        case 0x1F801DAAu:
        case 0x1F801DA4u:
        case 0x1F801DB0u:
        case 0x1F801DB2u:
        case 0x1F801DB4u:
        case 0x1F801DB6u:
            return 1;
        default:
            return 0;
    }
}

sint32 spu_mmio_write(void *user, uint32 physical_address, uint32 width, uint32 value, uint32 byte_mask)
{
    uint32 addresses[3], count = 0u, index, byte;
    uint16 values[3], previous[3];
    sint32 result = 0;
    (void)user;
    if ((width != 1u && width != 2u && width != 4u) || physical_address < 0x1F801C00u || physical_address > 0x1F801E00u - width)
        return 0;
    byte_mask &= (1u << width) - 1u;
    if (!byte_mask)
        return 1;
    xport_audio_lock();
    spu_synchronize_device_clock();
    if (!spu.initialized)
        goto finish;
    /* Stage every selected halfword before committing any register */
    for (byte = 0u; byte < width; ++byte)
    {
        uint32 address, shift;
        if (!(byte_mask & (1u << byte)))
            continue;
        address = (physical_address + byte) & ~1u;
        for (index = 0u; index < count && addresses[index] != address; ++index)
        {
        }
        if (index == count)
        {
            if (!spu_mmio_common_writable(address))
                goto finish;
            if (address == 0x1F801DA8u)
                values[index] = 0;
            else if (!spu_mmio_read16(address, &values[index]))
                goto finish;
            addresses[index] = address;
            previous[index] = values[index];
            ++count;
        }
        shift = ((physical_address + byte) & 1u) * 8u;
        values[index] = (uint16)((values[index] & ~(0xFFu << shift)) | (((value >> (8u * byte)) & 0xFFu) << shift));
    }
    for (index = 0u; index < count; ++index)
    {
        if ((addresses[index] == 0x1F801D80u || addresses[index] == 0x1F801D82u || (addresses[index] < 0x1F801D80 && (addresses[index] & 15) < 4)) && (values[index] & 0x8000u))
        {
#ifndef NDEBUG
            fprintf(stderr, "xport_spu_write unsupported volume sweep address=%08X value=%04X\n", addresses[index], values[index]);
#endif
            goto finish;
        }
        if (addresses[index] == 0x1F801DA8u && spu.mmio_fifo_count == 32u)
            goto finish;
        if (addresses[index] == 0x1F801DACu && values[index] != 4u)
            goto finish;
    }
    for (index = 0u; index < count; ++index)
    {
        if (addresses[index] >= 0x1F801DC0u)
        {
            reverb_control.parameters[(addresses[index] - 0x1F801DC0u) / 2u] = values[index];
            continue;
        }
        if (addresses[index] < 0x1F801D80)
        {
            SPU_VOICE *voice = spu.voices + (addresses[index] - 0x1F801C00) / 16;
            switch ((addresses[index] & 15) >> 1)
            {
                case 0:
                    voice->registers.volume_left = (sint16)values[index];
                    break;
                case 1:
                    voice->registers.volume_right = (sint16)values[index];
                    break;
                case 2:
                    voice->registers.pitch = values[index];
                    break;
                case 3:
                    voice->registers.start_address = values[index];
                    break;
                case 4:
                    voice->registers.adsr1 = values[index];
                    update_adsr(voice);
                    break;
                case 5:
                    voice->registers.adsr2 = values[index];
                    update_adsr(voice);
                    break;
                case 6:
                    voice->envelope = values[index] & 0x7FFF;
                    break;
                case 7:
                    voice->registers.repeat_address = values[index];
                    voice->repeat_address = (uint32)values[index] << 3;
                    voice->ignore_loop_address |= voice->adsr_phase == SPU_ADSR_OFF || !voice->first_block;
                    break;
            }
            continue;
        }
        switch (addresses[index])
        {
            case 0x1F801D84u:
                reverb_control.depth_left = (sint16)values[index];
                break;
            case 0x1F801D86u:
                reverb_control.depth_right = (sint16)values[index];
                break;
            case 0x1F801D88u:
            case 0x1F801D8Au:
                spu_key_on(((uint32)values[index] << (addresses[index] == 0x1F801D88u ? 0 : 16)) & SPU_ALLCH);
                break;
            case 0x1F801D8Cu:
            case 0x1F801D8Eu:
                spu_key_off(((uint32)values[index] << (addresses[index] == 0x1F801D8Cu ? 0 : 16)) & SPU_ALLCH);
                break;
            case 0x1F801D98u:
                spu.reverb_voice_mask = (spu.reverb_voice_mask & 0xFFFF0000u) | values[index];
                break;
            case 0x1F801D9Au:
                spu.reverb_voice_mask = (spu.reverb_voice_mask & 0xFFFFu) | ((uint32)values[index] << 16);
                break;
            case 0x1F801DA2u:
                reverb_control.work_address = values[index];
                spu_reverb_reset_cursor();
                break;
            case 0x1F801DA6u:
                spu.mmio_transfer_address = values[index];
                transfer_byte_address = (uint32)values[index] << 3;
                spu_irq_check_range(transfer_byte_address, 2);
                break;
            case 0x1F801DA8u:
                spu.mmio_fifo[spu.mmio_fifo_count++] = values[index];
                break;
            case 0x1F801DACu:
                spu.mmio_transfer_control = values[index];
                break;
            case 0x1F801D90:
                spu.pitch_modulation = (spu.pitch_modulation & 0xFFFF0000) | values[index];
                break;
            case 0x1F801D92:
                spu.pitch_modulation = (spu.pitch_modulation & 0xFFFF) | ((uint32)values[index] << 16);
                break;
            case 0x1F801D94:
                spu.noise_mask = (spu.noise_mask & 0xFFFF0000) | values[index];
                break;
            case 0x1F801D96:
                spu.noise_mask = (spu.noise_mask & 0xFFFF) | ((uint32)values[index] << 16);
                break;
            case 0x1F801D80u:
                spu.master_left = (sint16)values[index];
                break;
            case 0x1F801D82u:
                spu.master_right = (sint16)values[index];
                break;
            case 0x1F801DB0u:
                spu.cd_left = (sint16)values[index];
                break;
            case 0x1F801DB2u:
                spu.cd_right = (sint16)values[index];
                break;
            case 0x1F801DB4u:
                spu.ext_left = (sint16)values[index];
                break;
            case 0x1F801DB6u:
                spu.ext_right = (sint16)values[index];
                break;
            case 0x1F801DAAu:
                if ((previous[index] & 0x8000) && !(values[index] & 0x8000))
                {
                    uint32 voice_index;
                    for (voice_index = 0; voice_index < SPU_VOICE_COUNT; ++voice_index)
                    {
                        SPU_VOICE *voice = spu.voices + voice_index;
                        if (voice->adsr_phase != SPU_ADSR_OFF)
                        {
                            voice->adsr_phase = SPU_ADSR_OFF;
                            voice->envelope = 0;
                        }
                    }
                }
                reverb_control.control = values[index];
                if ((values[index] & 0x30u) == 0x10u && spu.mmio_fifo_count)
                {
                    uint32 word;
                    for (word = 0; word < spu.mmio_fifo_count; ++word)
                    {
                        spu_irq_check_range(transfer_byte_address, 2);
                        spu.ram[transfer_byte_address] = (uint8)spu.mmio_fifo[word];
                        spu.ram[(transfer_byte_address + 1u) & (SPU_RAM_SIZE - 1u)] = (uint8)(spu.mmio_fifo[word] >> 8);
                        transfer_byte_address = (transfer_byte_address + 2u) & (SPU_RAM_SIZE - 1u);
                    }
                    spu.mmio_fifo_count = 0;
                }
                reverb_control.enabled = (values[index] >> 7) & 1u;
                spu.cd_mix_controlled = 1u;
                if (!(values[index] & 0x40))
                    spu.irq_flag = spu.irq_pending = 0;
                else
                    spu_irq_check_late();
                break;
            case 0x1F801DA4u:
                spu.irq_address = values[index];
                spu_irq_check_late();
                break;
        }
    }
    result = 1;
finish:
    xport_audio_unlock();
    return result;
}

const uint8 *spu_ram_data(void)
{
    return spu.ram;
}

uint64 spu_voice_sample_position(sint32 voice)
{
    uint64 position;
    if (voice < 0 || voice >= SPU_VOICE_COUNT)
        return ~(uint64)0;
    xport_audio_lock();
    position = spu.voices[voice].sample_position >> 12;
    xport_audio_unlock();
    return position;
}

sint32 spu_decode_adpcm_block(const uint8 block[16], sint16 *history1, sint16 *history2, sint16 output[28])
{
    static const sint8 filter_pos[16] = {0, 60, 115, 98, 122, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    static const sint8 filter_neg[16] = {0, 0, -52, -55, -60, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    sint32 filter;
    sint32 shift;
    sint32 previous1;
    sint32 previous2;
    sint32 index;

    if (block == 0 || history1 == 0 || history2 == 0 || output == 0)
        return 0;
    filter = block[0] >> 4;
    shift = block[0] & 15;
    if (shift > 12)
        shift = 9;
    previous1 = *history1;
    previous2 = *history2;

    for (index = 0; index < 28; ++index)
    {
        uint8 packed = block[2 + index / 2];
        sint32 nibble = (index & 1) ? (packed >> 4) : (packed & 15);
        sint32 sample;
        if (nibble & 8)
            nibble -= 16;
        sample = (nibble << 12) >> shift;
        sample += (previous1 * filter_pos[filter]) >> 6;
        sample += (previous2 * filter_neg[filter]) >> 6;
        sample = audio_sample_clamp(sample);
        previous2 = previous1;
        previous1 = sample;
        output[index] = (sint16)sample;
    }

    *history1 = (sint16)previous1;
    *history2 = (sint16)previous2;
    return 28;
}

static void spu_set_voice_registers(sint32 voice, const SPU_VOICE_REGISTERS *registers)
{
    if (!spu.initialized || registers == 0 || voice < 0 || voice >= SPU_VOICE_COUNT)
        return;
    spu.voices[voice].registers = *registers;
    spu.voices[voice].repeat_address = (uint32)registers->repeat_address * 8u;
}

static void spu_set_voice_volume(sint32 voice, sint16 left, sint16 right)
{
    if (!spu.initialized || voice < 0 || voice >= SPU_VOICE_COUNT)
        return;
    spu.voices[voice].registers.volume_left = left;
    spu.voices[voice].registers.volume_right = right;
}

static void spu_set_voice_pitch(sint32 voice, uint16 pitch)
{
    if (!spu.initialized || voice < 0 || voice >= SPU_VOICE_COUNT)
        return;
    spu.voices[voice].registers.pitch = pitch;
}

static sint32 spu_get_voice_registers(sint32 voice, SPU_VOICE_REGISTERS *registers)
{
    if (!spu.initialized || !registers || voice < 0 || voice >= SPU_VOICE_COUNT)
        return 0;
    *registers = spu.voices[voice].registers;
    return 1;
}

static uint16 read_u16_le(const uint8 *data)
{
    return (uint16)(data[0] | ((uint16)data[1] << 8));
}

static uint32 read_u32_le(const uint8 *data)
{
    return (uint32)data[0] | ((uint32)data[1] << 8) | ((uint32)data[2] << 16) | ((uint32)data[3] << 24);
}

static SpuVmState native_vm;

SpuVmState *spu_vm_state(void)
{
    return &native_vm;
}

SpuVoiceState *spu_vm_voice(uint32 voice)
{
    if (voice >= SPU_VOICE_COUNT)
    {
#ifndef NDEBUG
        fprintf(stderr, "Invalid native SPU voice %u\n", voice);
#endif
        abort();
    }
    return native_vm.voices + voice;
}

void spu_vm_reset(void)
{
    memset(&native_vm, 0, sizeof(native_vm));
}

static void libsnd_reset(void)
{
    sint32 voice;
    SPU_STREAM_STATE streaming = libsnd.streaming;
#if defined(XPORT_NATIVE_SEQUENCE) && XPORT_NATIVE_SEQUENCE
    spu_sequence_reset();
#endif
    memset(&libsnd, 0, sizeof(libsnd));
    libsnd.streaming = streaming;
    libsnd.next_sound_ram_address = 0x1010;
    libsnd.vab_transfer_complete = 1;
    libsnd.cd.track = -1;
    libsnd.cd.mix.val0 = 255;
    libsnd.cd.mix.val2 = 255;
    for (voice = 0; voice < SPU_VOICE_COUNT; ++voice)
        libsnd.voices[voice].age = SPU_VOICE_COUNT;
}

static sint32 vab_is_valid(sint16 bank)
{
    return bank >= 0 && bank < VAB_BANK_COUNT && libsnd.banks[bank].valid && libsnd.banks[bank].transferred;
}

static const VAB_PROGRAM *vab_get_program(sint16 bank, sint16 program)
{
    if (!vab_is_valid(bank) || program < 0 || program >= VAB_PROGRAM_COUNT || libsnd.banks[bank].programs[program].tone_count == 0)
        return 0;
    return &libsnd.banks[bank].programs[program];
}

static const VAB_TONE *vab_get_tone(sint16 bank, sint16 program, sint16 tone)
{
    const VAB_PROGRAM *program_entry = vab_get_program(bank, program);
    if (program_entry == 0 || tone < 0 || tone >= program_entry->tone_count)
        return 0;
    return &libsnd.banks[bank].tones[xport_load_le32((const uint8 *)program_entry + 8)][tone];
}

static sint16 vab_open_head(const uint8 *header, sint16 requested_bank)
{
    const VAB_HEADER_RECORD *header_record = (const VAB_HEADER_RECORD *)header;
    VAB_BANK *parsed = &vab_parse_banks[0];
    uint16 program_count;
    uint16 tone_count;
    uint16 sample_count;
    uint32 tone_group = 0;
    uint32 program;
    uint32 size_table_offset;
    uint32 sample_offset = 0;
    sint16 bank;

    if (header == 0 || header_record->magic != 0x56414270u)
        return -1;
    program_count = header_record->program_count;
    tone_count = header_record->tone_count;
    sample_count = header_record->sample_count;
    if (program_count > VAB_PROGRAM_COUNT || tone_count > 2048 || sample_count > 255)
        return -1;
    if (requested_bank < 0)
    {
        for (bank = 0; bank < VAB_BANK_COUNT; ++bank)
            if (!libsnd.banks[bank].valid)
                break;
        if (bank == VAB_BANK_COUNT)
            return -1;
    }
    else
    {
        if (requested_bank >= VAB_BANK_COUNT || libsnd.banks[requested_bank].valid)
            return -1;
        bank = requested_bank;
    }

    memset(parsed, 0, sizeof(*parsed));
    memcpy(&parsed->header, header_record, sizeof(parsed->header));
    memcpy(parsed->tones, (const uint8 *)header + 32 + VAB_PROGRAM_COUNT * 16, (size_t)program_count * sizeof(parsed->tones[0]));
    for (program = 0; program < VAB_PROGRAM_COUNT; ++program)
    {
        const VAB_PROGRAM_RECORD *source = (const VAB_PROGRAM_RECORD *)(header_record + 1) + program;
        VAB_PROGRAM *destination = parsed->programs + program;
        uint32 tone;
        memcpy(destination, source, sizeof(*destination));
        // PsyQ reserves the program word at8 for its compact tone group
        xport_store_le32((uint8 *)destination + 8, tone_group);
        if (source->tone_count == 0)
            continue;
        if (source->tone_count > VAB_TONES_PER_PROGRAM || tone_group >= program_count)
            return -1;
        for (tone = 0; tone < VAB_TONES_PER_PROGRAM; ++tone)
        {
            VAB_TONE *tone_destination = &parsed->tones[tone_group][tone];
            // Only declared tones participate in playback sample validation
            if (tone < source->tone_count && (tone_destination->sample < 0 || tone_destination->sample > (sint16)sample_count))
                return -1;
        }
        ++tone_group;
    }
    // The header can reserve unused tone groups before the sample size table

    size_table_offset = 32 + 128 * 16 + (uint32)program_count * 16 * 32;
    for (program = 0; program < 256; ++program)
    {
        uint32 size = (uint32)read_u16_le(header + size_table_offset + program * 2) * 8u;
        if (program >= 1 && program <= sample_count)
            parsed->sample_addresses[program] = libsnd.next_sound_ram_address + sample_offset;
        sample_offset += size;
        // Program tail halfwords hold SPU sample addresses in eight-byte units
        if (program <= sample_count)
            xport_store_le16((uint8 *)parsed->programs + (program / 2) * 16 + ((program & 1) ? 14 : 12), (libsnd.next_sound_ram_address + sample_offset) >> 3);
    }
    parsed->body_size = sample_offset;
    if (parsed->body_size == 0 || libsnd.next_sound_ram_address > SPU_RAM_SIZE || parsed->body_size > SPU_RAM_SIZE - libsnd.next_sound_ram_address)
        return -1;
    parsed->body_address = libsnd.next_sound_ram_address;
    parsed->valid = 1;
    libsnd.banks[bank] = *parsed;
    libsnd.next_sound_ram_address = (libsnd.next_sound_ram_address + parsed->body_size + 7u) & ~7u;
    return bank;
}

sint16 spu_vab_open_head_sticky(const uint8 *header, uint32 header_bytes, sint16 requested_bank, uint32 body_address)
{
    VAB_BANK *parsed = &vab_parse_banks[1];
    uint32 magic, capacity, program_count, sample_count, size_offset;
    uint32 program, tone_group, sample, total, length, scale;
    sint32 version;
    sint16 bank;
    if (header == NULL || header_bytes < sizeof(VAB_HEADER_RECORD) || body_address > SPU_RAM_SIZE)
        return -1;
    magic = xport_load_le32(header);
    version = (sint32)xport_load_le32(header + 4);
    if ((magic >> 8) != 0x564142)
        return -1;
    capacity = (uint8)magic == 112 && version >= 5 ? 128 : 64;
    program_count = xport_load_le16(header + 18);
    sample_count = header[22];
    if (program_count > capacity)
        return -1;
    size_offset = 32 + capacity * 16 + program_count * 512;
    if (size_offset > header_bytes || 512 > header_bytes - size_offset)
        return -1;
    xport_audio_lock();
    memset(parsed, 0, sizeof(*parsed));
    memcpy(&parsed->header, header, sizeof(parsed->header));
    memcpy(parsed->programs, header + 32, capacity * sizeof(parsed->programs[0]));
    memcpy(parsed->tones, header + 32 + capacity * 16, program_count * sizeof(parsed->tones[0]));
    tone_group = 0;
    for (program = 0; program < capacity; ++program)
    {
        VAB_PROGRAM *entry = parsed->programs + program;
        if (entry->tone_count > VAB_TONES_PER_PROGRAM)
            goto failed;
        xport_store_le32((uint8 *)entry + 8, tone_group);
        if (entry->tone_count != 0)
        {
            if (tone_group >= program_count)
                goto failed;
            ++tone_group;
        }
    }
    total = 0;
    scale = version >= 5 ? 3 : 2;
    for (sample = 0; sample <= sample_count; ++sample)
    {
        length = (uint32)xport_load_le16(header + size_offset + sample * 2) << scale;
        if (length > SPU_RAM_SIZE - body_address - total)
            goto failed;
        if (sample != 0)
            parsed->sample_addresses[sample] = body_address + total;
        total += length;
        xport_store_le16((uint8 *)parsed->programs + (sample / 2) * 16 + ((sample & 1) ? 14 : 12), (body_address + total) >> 3);
    }
    parsed->body_address = body_address;
    parsed->body_size = total;
    parsed->valid = 2;
    if (!libsnd.initialized || !libsnd.vab_transfer_complete)
    {
        xport_audio_unlock();
        return -1;
    }
    if (requested_bank == -1)
    {
        for (bank = 0; bank < VAB_BANK_COUNT; ++bank)
            if (!libsnd.banks[bank].valid)
                break;
        if (bank == VAB_BANK_COUNT)
        {
            xport_audio_unlock();
            return -1;
        }
    }
    else
    {
        if (requested_bank < 0 || requested_bank >= VAB_BANK_COUNT || libsnd.banks[requested_bank].valid)
        {
            xport_audio_unlock();
            return -1;
        }
        bank = requested_bank;
    }
    libsnd.banks[bank] = *parsed;
    libsnd.vab_transfer_complete = 0;
    xport_audio_unlock();
    return bank;
failed:
    xport_audio_unlock();
    return -1;
}

sint16 spu_vab_trans_body_native(const uint8 *body, uint32 body_bytes, sint16 bank)
{
    VAB_BANK *entry;
    uint32 count, address, rounded;
    sint16 result = -1;
    xport_audio_lock();
    if (!libsnd.initialized || native_transfer_binding != SPU_TRANSFER_NATIVE || body == NULL || bank < 0 || bank >= VAB_BANK_COUNT)
        goto failed;
    entry = libsnd.banks + bank;
    if (!entry->valid || entry->transferred)
        goto failed;
    count = entry->body_size > 0x7EFF0 ? 0x7EFF0 : entry->body_size;
    rounded = (count + 63) & ~63u;
    if (count > body_bytes)
        goto failed;
    libsnd.vab_transfer_complete = 0;
    SpuSetTransferMode(0);
    address = SpuSetTransferStartAddr(entry->body_address);
    if (address == 0 || address >= SPU_RAM_SIZE || rounded > SPU_RAM_SIZE - address)
        goto failed;
    SpuWrite(body, count);
    if (transfer_failed)
        goto failed;
    if (*transfer.native_completion_callback == NULL)
        *transfer.completion = 0;
    entry->transferred = 1;
    // Native SpuWrite copies all bytes before returning and retains no host source
    libsnd.vab_transfer_complete = 1;
    result = bank;
    xport_audio_unlock();
    return result;
failed:
    if (native_transfer_binding == SPU_TRANSFER_NATIVE && transfer.completion != NULL)
        *transfer.completion = 1;
    libsnd.vab_transfer_complete = !transfer_failed;
    xport_audio_unlock();
    return result;
}

void spu_vab_reset_native(void)
{
    xport_audio_lock();
    memset(libsnd.banks, 0, sizeof(libsnd.banks));
    libsnd.next_sound_ram_address = 0x1010;
    libsnd.vab_transfer_complete = 1;
    xport_audio_unlock();
}

uint32 spu_vab_close_sticky(sint16 bank)
{
    VAB_BANK *entry;
    uint32 result, index;
    if ((uint16)bank >= VAB_BANK_COUNT)
        return 0;
    result = (uint32)bank * 4;
    xport_audio_lock();
    entry = libsnd.banks + bank;
    if (!entry->valid)
    {
        xport_audio_unlock();
        return result;
    }
    // Legacy automatic banks have validity1; only sticky banks have validity2
    if (entry->valid != 2)
    {
        xport_audio_unlock();
        return 0xFFFFFFFF;
    }
    if (!entry->transferred)
    {
        for (index = 0; index < VAB_BANK_COUNT; ++index)
        {
            if (index != (uint32)bank && libsnd.banks[index].valid && !libsnd.banks[index].transferred)
            {
                xport_audio_unlock();
                return 0xFFFFFFFF;
            }
        }
        if (native_transfer_binding != SPU_TRANSFER_NATIVE || transfer_pending || transfer_operation.active)
        {
            xport_audio_unlock();
            return 0xFFFFFFFF;
        }
        *transfer.completion = 1;
        libsnd.vab_transfer_complete = 1;
    }
    SpuFree(entry->body_address);
    memset(entry, 0, sizeof(*entry));
    result = 0;
    for (index = 0; index < VAB_BANK_COUNT; ++index)
        if (libsnd.banks[index].valid)
            ++result;
    xport_audio_unlock();
    return result;
}

void SsVabClose(sint16 bank)
{
    uint32 next = 0x1010;
    sint32 index;
    xport_audio_lock();
    if (bank >= 0 && bank < VAB_BANK_COUNT && libsnd.banks[bank].valid)
    {
        memset(libsnd.banks + bank, 0, sizeof(libsnd.banks[bank]));
        // Reclaim the free tail without moving any live sample data
        for (index = 0; index < VAB_BANK_COUNT; ++index)
        {
            const VAB_BANK *entry = libsnd.banks + index;
            uint32 end = (entry->body_address + entry->body_size + 7) & ~(uint32)7;
            if (entry->valid && end > next)
                next = end;
        }
        libsnd.next_sound_ram_address = next;
    }
    xport_audio_unlock();
}

void spu_native_set_mute(sint32 muted)
{
    xport_audio_lock();
    spu.native_muted = muted != 0;
    reverb_control.control = muted ? reverb_control.control & ~0x4000u : reverb_control.control | 0x4000u;
    xport_audio_unlock();
}

sint32 spu_vab_native_view(sint16 bank, SpuVabNativeView *view)
{
    const VAB_BANK *entry;
    if (!view)
        return 0;
    xport_audio_lock();
    if (bank < 0 || bank >= VAB_BANK_COUNT || !libsnd.banks[bank].valid)
    {
        xport_audio_unlock();
        return 0;
    }
    entry = libsnd.banks + bank;
    view->header = (const uint8 *)&entry->header;
    view->programs = (const uint8 *)entry->programs;
    view->tones = (const uint8 *)entry->tones;
    view->header_bytes = sizeof(entry->header);
    view->program_bytes = sizeof(entry->programs);
    view->tone_bytes = (uint32)entry->header.program_count * VAB_TONES_PER_PROGRAM * sizeof(entry->tones[0][0]);
    view->body_address = entry->body_address;
    view->body_bytes = entry->body_size;
    xport_audio_unlock();
    return 1;
}

SpuVabNativeView spu_vab_native_current(void)
{
    SpuVabNativeView view;
    if (!spu_vab_native_view((sint16)spu_vm_state()->note.bank, &view))
    {
#ifndef NDEBUG
        fprintf(stderr, "PsyQ native VAB context has no open bank\n");
#endif
        abort();
    }
    return view;
}

SpuVabNativeView spu_vm_native_bank(void)
{
#if defined(XPORT_NATIVE_VM) && XPORT_NATIVE_VM
    return spu_vab_native_current();
#else
    SpuVabNativeView view;
    SpuVmGuestBinding binding;
    sint16 bank;
    spu_vm_guest_binding(&binding);
    if (!binding.context)
        abort();
    bank = (sint16)r_u8(binding.context + 1);
    if (!spu_vab_native_view(bank, &view))
    {
    #ifndef NDEBUG
        fprintf(stderr, "PsyQ native VAB context has no open bank\n");
    #endif
        abort();
    }
    return view;
#endif
}

sint32 spu_native_vab_address(sint16 bank)
{
    sint32 address = -1;
    xport_audio_lock();
    if (bank >= 0 && bank < VAB_BANK_COUNT && libsnd.banks[bank].valid)
        address = (sint32)libsnd.banks[bank].body_address;
    xport_audio_unlock();
    return address;
}

sint16 SsUtGetVabHdr(sint16 bank, VabHdr *attributes)
{
    sint16 result = -1;
    if (!attributes)
        return result;
    xport_audio_lock();
    if (bank >= 0 && bank < VAB_BANK_COUNT && libsnd.banks[bank].valid)
    {
        memcpy(attributes, &libsnd.banks[bank].header, sizeof(*attributes));
        result = 0;
    }
    xport_audio_unlock();
    return result;
}

sint32 spu_vab_select_native(uint16 bank, sint16 program)
{
    const VAB_BANK *entry;
    uint32 magic, capacity;
    sint32 version;
    xport_audio_lock();
    if (bank >= VAB_BANK_COUNT || !vab_is_valid((sint16)bank))
    {
        xport_audio_unlock();
        return -1;
    }
    entry = libsnd.banks + bank;
    magic = xport_load_le32(&entry->header);
    version = (sint32)xport_load_le32((const uint8 *)&entry->header + 4);
    capacity = (uint8)magic == 112 && version >= 5 ? 128 : 64;
    if (program < 0 || (uint32)program >= capacity)
    {
        xport_audio_unlock();
        return -1;
    }
    spu_vm_state()->note.bank = bank;
    spu_vm_state()->note.program = program;
    spu_vm_state()->note.program_tone_index = ((const uint8 *)(entry->programs + program))[8];
    xport_audio_unlock();
    return 0;
}

uint32 spu_vab_set_program_volume(sint16 bank, sint16 program, uint8 value)
{
    uint32 result;
    xport_audio_lock();
    if (spu_vab_select_native((uint16)bank, program) != 0)
    {
        xport_audio_unlock();
        return (uint32)-1;
    }
    libsnd.banks[bank].programs[program].volume = value;
    result = libsnd.banks[bank].programs[program].volume;
    xport_audio_unlock();
    return result;
}

uint32 spu_vab_set_program_pan(sint16 bank, sint16 program, uint8 value)
{
    uint32 result;
    xport_audio_lock();
    if (spu_vab_select_native((uint16)bank, program) != 0)
    {
        xport_audio_unlock();
        return (uint32)-1;
    }
    libsnd.banks[bank].programs[program].pan = value;
    result = libsnd.banks[bank].programs[program].pan;
    xport_audio_unlock();
    return result;
}

sint16 SsUtGetProgAtr(sint16 bank, sint16 program, ProgAtr *attributes)
{
    uint8 *destination = (uint8 *)attributes;
    const uint8 *source;
    uint32 index;
    uint16 value;
    if (attributes == NULL)
        return -1;
    xport_audio_lock();
    if (spu_vab_select_native((uint16)bank, program) != 0)
    {
        xport_audio_unlock();
        return -1;
    }
    for (index = 0; index < 5; ++index)
    {
        source = (const uint8 *)(libsnd.banks[bank].programs + program);
        destination[index] = source[index];
    }
    source = (const uint8 *)(libsnd.banks[bank].programs + program);
    value = xport_load_le16(source + 6);
    memcpy(destination + 6, &value, sizeof(value));
    xport_audio_unlock();
    return 0;
}

sint16 spu_vab_get_vag_atr_native(sint16 bank, sint16 program, sint16 tone, VagAtr *output)
{
    static const uint8 byte_offsets[14] = {0, 1, 2, 3, 4, 5, 7, 6, 8, 9, 10, 11, 12, 13};
    uint8 *destination = (uint8 *)output;
    const uint8 *source;
    SpuVabNativeView view;
    uint32 index;
    sint32 displacement;
    uint16 value;
    if (output == NULL)
        return -1;
    xport_audio_lock();
#if defined(XPORT_NATIVE_VM) && XPORT_NATIVE_VM
    if (_SsVmVSetUp((uint16)bank, program) != 0)
#else
    if (spu_vab_select_native((uint16)bank, program) != 0)
#endif
    {
        xport_audio_unlock();
        return -1;
    }
    displacement = (sint32)(sint16)((uint32)(uint16)tone + spu_vm_state()->note.program_tone_index * 16) * 32;
    if (!spu_vab_native_view(bank, &view) || (uint32)displacement > view.tone_bytes || 24 > view.tone_bytes - (uint32)displacement)
    {
        xport_audio_unlock();
        return -1;
    }
    for (index = 0; index < 14; ++index)
    {
        uint32 offset = byte_offsets[index];
#if defined(XPORT_NATIVE_VM) && XPORT_NATIVE_VM
        source = spu_vm_native_bank().tones + displacement;
#else
        source = (const uint8 *)libsnd.banks[spu_vm_state()->note.bank].tones + displacement;
#endif
        destination[offset] = source[offset];
    }
#if defined(XPORT_NATIVE_VM) && XPORT_NATIVE_VM
    source = spu_vm_native_bank().tones + displacement;
#else
    source = (const uint8 *)libsnd.banks[spu_vm_state()->note.bank].tones + displacement;
#endif
    for (index = 16; index < 24; index += 2)
    {
        value = xport_load_le16(source + index);
        memcpy(destination + index, &value, sizeof(value));
    }
    xport_audio_unlock();
    return 0;
}

sint16 SsUtSetVagAtr(sint16 bank, sint16 program, sint16 tone, const VagAtr *attributes)
{
    static const uint8 byte_offsets[14] = {0, 1, 2, 3, 4, 5, 7, 6, 8, 9, 10, 11, 12, 13};
    const uint8 *source = (const uint8 *)attributes;
    uint8 *destination;
    uint32 index, tone_bytes;
    sint32 displacement;
    uint16 value;
    if (attributes == NULL)
        return -1;
    xport_audio_lock();
    if (spu_vab_select_native((uint16)bank, program) != 0)
    {
        xport_audio_unlock();
        return -1;
    }
    displacement = (sint32)(sint16)((uint32)(uint16)tone + spu_vm_state()->note.program_tone_index * 16) * 32;
    tone_bytes = (uint32)libsnd.banks[bank].header.program_count * sizeof(libsnd.banks[bank].tones[0]);
    if ((uint32)displacement > tone_bytes || 24 > tone_bytes - (uint32)displacement)
    {
        xport_audio_unlock();
        return -1;
    }
    for (index = 0; index < 14; ++index)
    {
        uint32 offset = byte_offsets[index];
        destination = (uint8 *)libsnd.banks[spu_vm_state()->note.bank].tones + displacement;
        destination[offset] = source[offset];
    }
    destination = (uint8 *)libsnd.banks[spu_vm_state()->note.bank].tones + displacement;
    for (index = 16; index < 24; index += 2)
    {
        value = xport_load_le16(source + index);
        memcpy(destination + index, &value, sizeof(value));
    }
    xport_audio_unlock();
    return 0;
}

void spu_sequence_stream_bind(SpuSequenceStream *stream, const uint8 *data, uint32 size)
{
    if (!stream || !data || !size)
    {
#ifndef NDEBUG
        fprintf(stderr, "PsyQ native SEQ: Missing sequence data span\n");
#endif
        abort();
    }
    memset(stream, 0, sizeof(*stream));
    stream->data = data;
    stream->size = size;
}

void spu_sequence_stream_clear(SpuSequenceStream *stream)
{
    if (!stream)
    {
#ifndef NDEBUG
        fprintf(stderr, "PsyQ native SEQ: Missing sequence stream\n");
#endif
        abort();
    }
    memset(stream, 0, sizeof(*stream));
}

uint8 spu_sequence_stream_byte(const SpuSequenceStream *stream, uint32 offset)
{
    if (!stream || !stream->data || offset >= stream->size)
    {
#ifndef NDEBUG
        fprintf(stderr, "PsyQ native SEQ: Read outside sequence data span\n");
#endif
        abort();
    }
    return stream->data[offset];
}

uint32 spu_sequence_stream_delta(SpuSequenceStream *stream, uint32 *elapsed)
{
    uint32 cursor, value, byte, result;
    cursor = stream->cursor;
    stream->cursor = cursor + 1;
    value = spu_sequence_stream_byte(stream, cursor);
    if (value == 0)
        return 0;
    if ((value & 0x80) != 0)
    {
        value &= 0x7F;
        do
        {
            cursor = stream->cursor;
            stream->cursor = cursor + 1;
            byte = spu_sequence_stream_byte(stream, cursor);
            value = (value << 7) + (byte & 0x7F);
        } while ((byte & 0x80) != 0);
    }
    result = value * 10;
    *elapsed = *elapsed + result;
    return result;
}

uint32 spu_sequence_scheduler_data_entry(SpuSequenceSchedulerTrack *record, sint16 sequence, sint16 separation, uint8 value, const SpuToneCommand *commands, uint32 command_count, SpuSequenceHandler read_delta)
{
    uint32 channel = record->channel;
    const SpuToneCommand *target;
    ProgAtr program = {0};
    VagAtr attributes = {0};
    uint32 index, kind, tone, result;
    SsUtGetProgAtr((sint16)record->bank, record->channel_program[channel], &program);
    if (record->loop_count_pending == 1 && record->loop_count_set == 0)
    {
        record->loop_remaining = (uint8)(value);
        record->loop_count_set = (uint8)(1);
    }
    else if (record->rpn_phase == 2)
    {
        if (record->rpn_selector == 0 && record->rpn_parameter <= 2 && program.tones != 0)
        {
            index = 0;
            do
            {
                spu_vab_get_vag_atr_native((sint16)record->bank, record->channel_program[channel], (sint16)index, &attributes);
                kind = record->rpn_parameter;
                if (kind == 0)
                {
                    attributes.pbmax = value & 127;
                    attributes.pbmin = value & 127;
                }
                SsUtSetVagAtr((sint16)record->bank, record->channel_program[channel], (sint16)index, &attributes);
                ++index;
            } while ((sint32)index < program.tones);
        }
        result = read_delta(sequence, separation);
        record->delta = (uint32)(result);
        record->rpn_phase = (uint8)(0);
        return result;
    }
    else if (record->nrpn_phase == 2)
    {
        tone = record->nrpn_selector;
        if (tone == 16)
        {
            index = 0;
            if (program.tones != 0)
            {
                do
                {
                    kind = record->nrpn_parameter;
                    if (kind >= command_count)
                    {
#ifndef NDEBUG
                        fprintf(stderr, "PsyQ native SEQ: Tone command outside table: %u\n", kind);
#else
                        (void)0;
#endif
                        abort();
                    }
                    target = &commands[kind];
                    if (target->parameter != NULL)
                        target->parameter((sint16)record->bank, record->channel_program[channel], (sint16)index, attributes, value);
                    else
                        target->value(value);
                    ++index;
                } while ((sint32)index < program.tones);
            }
        }
        else
        {
            kind = record->nrpn_parameter;
            if (kind >= command_count)
            {
#ifndef NDEBUG
                fprintf(stderr, "PsyQ native SEQ: Tone command outside table: %u\n", kind);
#else
                (void)0;
#endif
                abort();
            }
            target = &commands[kind];
            if (target->parameter != NULL)
                target->parameter((sint16)record->bank, record->channel_program[channel], (sint16)tone, attributes, value);
            else
                target->value(value);
        }
        result = read_delta(sequence, separation);
        record->delta = (uint32)(result);
        record->nrpn_phase = (uint8)(0);
        return result;
    }
    result = read_delta(sequence, separation);
    record->delta = (uint32)(result);
    return result;
}

uint32 spu_vab_nrpn_attr_by_value(sint16 attribute, sint16 bank, sint16 program, sint16 tone, VagAtr attributes, uint8 value)
{
    uint16 fields[9];
    uint32 result;
    spu_vab_get_vag_atr_native(bank, program, tone, &attributes);
    if ((uint16)attribute > 14)
    {
        switch (attribute)
        {
            case 15:
                return spu_sequence_vm_reverb_type(value);
            case 16:
                return spu_sequence_vm_reverb_depth(value, value);
            case 17:
                return spu_sequence_vm_reverb_feedback(value);
            case 18:
            case 19:
                return spu_sequence_vm_reverb_delay(value);
            default:
                return 0;
        }
    }
    switch (attribute)
    {
        case 0:
            attributes.prior = value;
            break;
        case 1:
            attributes.mode = value;
            break;
        case 2:
            attributes.min = value;
            break;
        case 3:
            attributes.max = value;
            break;
        default:
            // Define the four original leftover-stack ADSR operations
            _SsUtResolveADSR(attributes.adsr1, attributes.adsr2, fields);
            switch (attribute)
            {
                case 4:
                    fields[5] = 0;
                    fields[0] = value;
                    break;
                case 5:
                    fields[5] = 1;
                    fields[0] = value;
                    break;
                case 6:
                    fields[1] = value;
                    break;
                case 7:
                    fields[2] = value;
                    break;
                case 8:
                    fields[6] = 0;
                    fields[3] = value;
                    break;
                case 9:
                    fields[6] = 1;
                    fields[3] = value;
                    break;
                case 10:
                    fields[7] = 0;
                    fields[4] = value;
                    break;
                case 11:
                    fields[7] = 1;
                    fields[4] = value;
                    break;
                case 12:
                    if (value != 0 && value < 64)
                        fields[8] = 0;
                    else if ((uint32)value - 64 < 64)
                        fields[8] = 1;
                    break;
                case 13:
                    attributes.vibT = value;
                    break;
                case 14:
                    attributes.porW = value;
                    break;
            }
            _SsUtBuildADSR(fields, &attributes.adsr1, &attributes.adsr2);
            break;
    }
    result = (uint32)(sint32)SsUtSetVagAtr(bank, program, tone, &attributes);
    if (attribute == 1)
    {
        if (value == 0)
            return (uint32)SpuSetReverb(0);
        if (value == 4)
            return (uint32)SpuSetReverb(1);
        return value < 4 ? value + 1 : 4;
    }
    return result;
}

#if defined(XPORT_NATIVE_VM) && XPORT_NATIVE_VM
sint16 SsUtGetVagAtr(sint16 bank, sint16 program, sint16 tone, VagAtr *output)
{
    return spu_vab_get_vag_atr_native(bank, program, tone, output);
}
#else
sint16 SsUtGetVagAtr(sint16 bank, sint16 program, sint16 tone, VagAtr *attributes)
{
    sint16 result = -1;
    if (!attributes)
        return result;
    xport_audio_lock();
    if (bank >= 0 && bank < VAB_BANK_COUNT && libsnd.banks[bank].valid && program >= 0 && program < VAB_PROGRAM_COUNT && libsnd.banks[bank].programs[program].tone_count && tone >= 0 && tone < VAB_TONES_PER_PROGRAM)
    {
        memcpy(attributes, &libsnd.banks[bank].tones[xport_load_le32((const uint8 *)&libsnd.banks[bank].programs[program] + 8)][tone], sizeof(*attributes));
        result = 0;
    }
    xport_audio_unlock();
    return result;
}
#endif

sint16 SsVabOpenHead(uint8 *header, sint16 requested_bank)
{
    sint16 bank;
    xport_audio_lock();
    bank = libsnd.initialized ? vab_open_head(header, requested_bank) : -1;
    if (bank >= 0)
        libsnd.vab_transfer_complete = 0;
    xport_audio_unlock();
    return bank;
}

sint16 SsVabTransBody(uint8 *body, sint16 bank)
{
    VAB_BANK *entry;
    sint16 result = -1;
    xport_audio_lock();
    if (libsnd.initialized && bank >= 0 && bank < VAB_BANK_COUNT && body != 0)
    {
        entry = libsnd.banks + bank;
        if (entry->valid && spu_upload(entry->body_address, body, entry->body_size))
        {
            entry->transferred = 1;
            result = bank;
        }
    }
    libsnd.vab_transfer_complete = result >= 0;
    xport_audio_unlock();
    return result;
}

sint16 SsVabTransCompleted(sint16 mode)
{
    sint16 complete;
    xport_audio_lock();
    complete = (sint16)libsnd.vab_transfer_complete;
    xport_audio_unlock();
    return complete;
}

static const uint16 libsnd_semitone_pitch[12] = {32768, 34716, 36780, 38967, 41285, 43740, 46340, 49096, 52015, 55108, 58385, 61857};

static const uint16 libsnd_fine_pitch[128] = {32768, 32782, 32797, 32812, 32827, 32842, 32856, 32871, 32886, 32901, 32916, 32931, 32945, 32960, 32975, 32990, 33005, 33020, 33035, 33050, 33065, 33080, 33094, 33109, 33124, 33139, 33154, 33169, 33184, 33199, 33214, 33229, 33244, 33259, 33274, 33289, 33304, 33319, 33334, 33349, 33364, 33379, 33394, 33410, 33425, 33440, 33455, 33470, 33485, 33500, 33515, 33530, 33546, 33561, 33576, 33591, 33606, 33621, 33636, 33652, 33667, 33682, 33697, 33712, 33728, 33743, 33758, 33773, 33789, 33804, 33819, 33834, 33850, 33865, 33880, 33896, 33911, 33926, 33941, 33957, 33972, 33987, 34003, 34018, 34033, 34049, 34064, 34080, 34095, 34110, 34126, 34141, 34157, 34172, 34187, 34203, 34218, 34234, 34249, 34265, 34280, 34296, 34311, 34327, 34342, 34358, 34373, 34389, 34404, 34420, 34435, 34451, 34466, 34482, 34497, 34513, 34528, 34544, 34560, 34575, 34591, 34606, 34622, 34638, 34653, 34669, 34685, 34700};

uint16 SsPitchFromNote(sint32 note, sint16 fine, uint8 center, uint8 shift)
{
    sint32 combined = (sint16)(uint16)((uint16)fine + (uint32)shift);
    sint32 coarse = combined / 128;
    sint32 remainder = combined - coarse * 128;
    uint32 adjusted = (uint32)note + (uint32)coarse - center;
    sint32 fine_index = remainder;
    sint32 semitone, quotient, octave, index;
    uint32 pitch, shift_count;
    if ((sint16)remainder < 0)
    {
        fine_index = remainder + 128;
        adjusted = adjusted - 1u + (uint32)((sint16)fine_index / 128);
    }
    semitone = (sint16)(uint16)adjusted;
    quotient = semitone / 12;
    octave = quotient - 2;
    index = semitone - quotient * 12;
    if ((sint16)index < 0)
    {
        index += 12;
        octave = quotient - 3;
    }
    pitch = ((uint32)libsnd_semitone_pitch[(sint16)index] * libsnd_fine_pitch[(sint16)fine_index]) >> 16;
    if ((sint16)octave >= 0)
        return 0x3FFFu;
    shift_count = (uint32)(-(sint16)octave);
    pitch += 1u << ((shift_count - 1u) & 31u);
    return (uint16)(pitch >> (shift_count & 31u));
}

static uint16 note_to_pitch(sint16 note, uint16 fine, const VAB_TONE *tone)
{
    sint32 fine_index = ((sint32)fine + tone->shift) / 8;
    sint32 carry = 0;
    sint32 semitone;
    sint32 octave;
    sint32 remainder;
    uint32 pitch;
    if (fine_index >= 16)
    {
        carry = 1;
        fine_index -= 16;
    }
    semitone = carry + note + 60 - tone->center;
    octave = semitone / 12;
    if (semitone < 0 && semitone % 12)
        --octave;
    remainder = semitone - octave * 12;
    pitch = spu_note_pitch_table[remainder * 16 + fine_index];
    if (octave > 5)
        pitch <<= octave - 5;
    else if (octave < 5)
        pitch >>= 5 - octave;
    return (uint16)pitch;
}

static sint16 spu_volume_attr(sint16 volume, sint16 mode, sint32 sweep)
{
    uint16 control = 0u;

    if (!sweep || mode < 1 || mode > 7)
        return (sint16)((uint16)volume & 0x7fffu);
    if (volume < 0)
        volume = 0;
    if (volume > 127)
        volume = 127;
    control = (uint16)(0x7000u + ((uint16)mode << 12));
    return (sint16)(control | (uint16)volume);
}

static uint16 spu_adsr_attack(uint16 adsr1, const SpuVoiceAttr *attr, uint32 mask)
{
    uint16 rate = attr->ar < 128u ? attr->ar : 127u;
    uint16 mode = 0u;

    if (!mask || (mask & SPU_VOICE_ADSR_AMODE))
        mode = attr->a_mode == 5 ? 0x80u : 0u;
    return (uint16)((adsr1 & 0x00ffu) | ((rate | mode) << 8));
}

static uint16 spu_adsr_sustain(uint16 adsr2, const SpuVoiceAttr *attr, uint32 mask)
{
    uint16 rate = attr->sr < 128u ? attr->sr : 127u;
    uint16 mode = 0u;

    if (!mask || (mask & SPU_VOICE_ADSR_SMODE))
    {
        if (attr->s_mode == 3)
            mode = 0x100u;
        else if (attr->s_mode == 5)
            mode = 0x200u;
        else if (attr->s_mode == 7)
            mode = 0x300u;
        else if (attr->s_mode != 1)
            mode = 0x100u;
    }
    return (uint16)((adsr2 & 0x003fu) | ((rate | mode) << 6));
}

static sint16 compose_volume(sint16 primary, sint16 secondary, uint8 bank_volume, const VAB_PROGRAM *program, const VAB_TONE *tone, sint32 left)
{
    uint32 maximum;
    uint32 pan;
    uint32 volume;
    uint32 channel;
    if (primary < 0)
        primary = 0;
    if (secondary < 0)
        secondary = 0;
    if (primary == secondary)
    {
        maximum = (uint32)primary;
        pan = 64;
    }
    else if (secondary >= primary)
    {
        maximum = (uint32)secondary;
        pan = secondary ? 127u - ((uint32)primary << 6) / (uint32)secondary : 64;
    }
    else
    {
        maximum = (uint32)primary;
        pan = primary ? ((uint32)secondary << 6) / (uint32)primary : 64;
    }
    volume = maximum * 0x3fffu * bank_volume / 16129u;
    volume = volume * program->volume * tone->volume / 0x3f01u;
    channel = volume;
    if (tone->pan >= 64)
    {
        if (left)
            channel = channel * (127 - tone->pan) / 63;
    }
    else if (!left)
        channel = channel * tone->pan / 63;
    if (program->pan >= 64)
    {
        if (left)
            channel = channel * (127 - program->pan) / 63;
    }
    else if (!left)
        channel = channel * program->pan / 63;
    if (pan >= 64)
    {
        if (left)
            channel = channel * (127 - pan) / 63;
    }
    else if (!left)
        channel = channel * pan / 63;
    return (sint16)(channel * channel / 0x3fff);
}

static sint16 start_voice(sint16 voice, sint16 bank, sint16 program, sint16 tone_index, sint16 note, sint16 fine, sint16 left, sint16 right)
{
    const VAB_PROGRAM *program_entry = vab_get_program(bank, program);
    const VAB_TONE *tone = vab_get_tone(bank, program, tone_index);
    SPU_VOICE_REGISTERS registers;
    uint32 sample_address;
    if (voice < 0 || voice >= SPU_VOICE_COUNT || program_entry == 0 || tone == 0)
        return -1;
    sample_address = libsnd.banks[bank].sample_addresses[tone->sample];
    if (sample_address == 0)
        return -1;
    memset(&registers, 0, sizeof(registers));
    registers.volume_left = compose_volume(left, right, libsnd.banks[bank].header.master_volume, program_entry, tone, 1);
    registers.volume_right = compose_volume(left, right, libsnd.banks[bank].header.master_volume, program_entry, tone, 0);
    registers.pitch = note_to_pitch(note, (uint16)fine, tone);
    registers.start_address = (uint16)(sample_address >> 3);
    registers.repeat_address = registers.start_address;
    registers.adsr1 = tone->adsr1;
    registers.adsr2 = tone->adsr2;
    spu_set_voice_registers(voice, &registers);
    spu.sample_notes[voice] = (uint16)(((uint16)tone->center << 8) | tone->shift);
    spu_key_on(1u << voice);
    libsnd.voices[voice].bank = bank;
    libsnd.voices[voice].program = program;
    libsnd.voices[voice].tone = tone_index;
    libsnd.voices[voice].note = (uint16)note;
    libsnd.voices[voice].direct_keyon = 0u;
    libsnd.voices[voice].priority = tone->priority;
    libsnd.voices[voice].age = 0;
    libsnd.voices[voice].keyed = 1;
    return voice;
}

#if defined(XPORT_NATIVE_VM) && XPORT_NATIVE_VM
static sint16 spu_sequence_vm_key_on_tone(uint16 voice, sint16 bank, sint16 program, sint16 tone, sint16 note, sint16 fine, sint16 left, sint16 right, sint32 allocate);
#endif

sint16 SsUtKeyOn(sint16 bank, sint16 program, sint16 tone, sint16 note, sint16 fine, sint16 left, sint16 right)
{
#if defined(XPORT_NATIVE_VM) && XPORT_NATIVE_VM
    return spu_sequence_vm_key_on_tone(0, bank, program, tone, note, fine, left, right, 1);
#else
    const VAB_TONE *requested_tone;
    sint16 voice;
    sint16 selected = -1;
    sint16 best_priority;
    uint16 best_envelope = 0xffffu;
    sint16 best_age = 0;
    xport_audio_lock();
    requested_tone = vab_get_tone(bank, program, tone);
    if (requested_tone == 0)
    {
        xport_audio_unlock();
        return -1;
    }
    best_priority = requested_tone->priority;
    for (voice = 0; voice < SPU_VOICE_COUNT; ++voice)
    {
        sint16 priority = libsnd.voices[voice].priority;
        if (!(spu.key_on_pending & SPU_KEYCH(voice)) && spu.voices[voice].adsr_phase == SPU_ADSR_OFF && spu.voices[voice].envelope == 0)
        {
            selected = voice;
            break;
        }
        if (priority < best_priority)
        {
            best_priority = priority;
            best_envelope = spu.voices[voice].envelope;
            best_age = (sint16)libsnd.voices[voice].age;
            selected = voice;
        }
        else if (priority == best_priority && (spu.voices[voice].envelope < best_envelope || (spu.voices[voice].envelope == best_envelope && (sint16)libsnd.voices[voice].age > best_age)))
        {
            best_envelope = spu.voices[voice].envelope;
            best_age = (sint16)libsnd.voices[voice].age;
            selected = voice;
        }
    }
    if (selected >= 0)
    {
        for (voice = 0; voice < SPU_VOICE_COUNT; ++voice)
            ++libsnd.voices[voice].age;
        selected = start_voice(selected, bank, program, tone, note, fine, left, right);
    }
    xport_audio_unlock();
    return selected;
#endif
}

void xport_libsnd_bind_sequence_table(uint32 table_address)
{
    if (!table_address || !psx_addr(table_address, 4u))
    {
#ifndef NDEBUG
        fprintf(stderr, "Invalid libsnd sequence table binding %08X\n", table_address);
#endif
        abort();
    }
    xport_audio_lock();
    libsnd.sequence_table_address = table_address;
    xport_audio_unlock();
}

void xport_libsnd_bind_sequence_handlers(uint32 table_address)
{
    if (!table_address || !psx_addr(table_address, 68u))
    {
#ifndef NDEBUG
        fprintf(stderr, "Invalid libsnd sequence handler binding %08X\n", table_address);
#endif
        abort();
    }
    xport_audio_lock();
    libsnd.sequence_handlers_address = table_address;
    xport_audio_unlock();
}

void xport_libsnd_bind_sequence_mark_table(uint32 table_address)
{
    if (!table_address || !psx_addr(table_address, 4u))
    {
#ifndef NDEBUG
        fprintf(stderr, "Invalid libsnd sequence mark table binding %08X\n", table_address);
#endif
        abort();
    }
    xport_audio_lock();
    libsnd.sequence_mark_table_address = table_address;
    xport_audio_unlock();
}

uint32 spu_sequence_mark_table(void)
{
    uint32 address;
    xport_audio_lock();
    address = libsnd.sequence_mark_table_address;
    xport_audio_unlock();
    return address;
}

void spu_sequence_tables(uint32 *sequences, uint32 *handlers)
{
    xport_audio_lock();
    if (sequences)
        *sequences = libsnd.sequence_table_address;
    if (handlers)
        *handlers = libsnd.sequence_handlers_address;
    xport_audio_unlock();
}

static sint32 spu_vm_guest_valid(const SpuVmGuestBinding *binding)
{
    if (!binding || !binding->protection || !psx_addr(binding->protection, 4u))
        return 0;
    if (!binding->channel_masks || !binding->release_offset || !psx_addr(binding->channel_masks, 64u) || !psx_addr(binding->release_offset, 2u))
        return 0;
    if (!binding->mono_mode || !binding->voice_masks || !binding->pending_keyoff || !psx_addr(binding->mono_mode, 2u) || !psx_addr(binding->voice_masks, 50u) || !psx_addr(binding->pending_keyoff, 4u))
        return 0;
#if defined(XPORT_NATIVE_VAB) && XPORT_NATIVE_VAB
    return binding->context && binding->voices && binding->voice_count && psx_addr(binding->context, 32) && psx_addr(binding->voices, 54) && psx_addr(binding->voice_count, 1);
#else
    return binding && binding->context && binding->voices && binding->program_pointer && binding->header_pointer && binding->tone_pointer && binding->bank_flags && binding->program_limit && binding->header_table && binding->program_table && binding->tone_table && binding->voice_count && psx_addr(binding->context, 32u) && psx_addr(binding->voices, 54u) && psx_addr(binding->program_pointer, 4u) && psx_addr(binding->header_pointer, 4u) && psx_addr(binding->tone_pointer, 4u) && psx_addr(binding->bank_flags, 16u) && psx_addr(binding->program_limit, 2u) && psx_addr(binding->header_table, 64u) && psx_addr(binding->program_table, 64u) && psx_addr(binding->tone_table, 64u) && psx_addr(binding->voice_count, 1u);
#endif
}

sint32 spu_bind_vm_guest(const SpuVmGuestBinding *binding)
{
    if (!spu_vm_guest_valid(binding))
        return 0;
    xport_audio_lock();
    libsnd.vm_guest = *binding;
    xport_audio_unlock();
    return 1;
}

void spu_vm_guest_binding(SpuVmGuestBinding *binding)
{
    xport_audio_lock();
    *binding = libsnd.vm_guest;
    xport_audio_unlock();
}

void xport_libsnd_bind_voice_volume(uint32 volume_address, uint32 flags_address)
{
    if (!volume_address || !flags_address || !psx_addr(volume_address, 16u * (SPU_VOICE_COUNT - 1u) + 4u) || !psx_addr(flags_address, SPU_VOICE_COUNT))
    {
#ifndef NDEBUG
        fprintf(stderr, "Invalid libsnd voice volume binding %08X/%08X\n", volume_address, flags_address);
#endif
        abort();
    }
    xport_audio_lock();
    libsnd.voice_volume_address = volume_address;
    libsnd.voice_update_flags_address = flags_address;
    xport_audio_unlock();
}

void spu_voice_cache_binding(uint32 *values, uint32 *flags)
{
    xport_audio_lock();
    if (values)
        *values = libsnd.voice_volume_address;
    if (flags)
        *flags = libsnd.voice_update_flags_address;
    xport_audio_unlock();
}

static uint16 spu_register_half(const SndRegisterAttr *attributes, uint32 index)
{
    uint16 value;
    memcpy(&value, (const uint8 *)attributes + 2u * index, sizeof(value));
    return value;
}

#if defined(XPORT_NATIVE_VM) && XPORT_NATIVE_VM
void SsQueueRegisters(sint32 voice, SndRegisterAttr *attributes)
{
    uint16 *registers;
    uint8 *dirty;
    uint16 value, flags;
    uint32 all;
    if (voice < 0 || voice >= SPU_VOICE_COUNT || !attributes)
        abort();
    xport_audio_lock();
    registers = native_vm.registers + voice * 8;
    dirty = native_vm.dirty + voice;
    all = spu_register_half(attributes, 3) == 0;
    if ((spu_register_half(attributes, 3) & 1) || all)
    {
        value = spu_register_half(attributes, 0) & 0x7FFF;
        flags = *dirty | 1;
        registers[0] = value;
        *dirty = flags;
    }
    if ((spu_register_half(attributes, 3) & 2) || all)
    {
        registers[1] = spu_register_half(attributes, 1) & 0x7FFF;
        *dirty |= 2;
    }
    if ((spu_register_half(attributes, 3) & 4) || all)
    {
        registers[4] = spu_register_half(attributes, 5);
        *dirty |= 0x10;
    }
    if ((spu_register_half(attributes, 3) & 8) || all)
    {
        registers[5] = spu_register_half(attributes, 6);
        *dirty |= 0x20;
    }
    if ((spu_register_half(attributes, 3) & 0x20) || all)
    {
        registers[2] = spu_register_half(attributes, 2);
        *dirty |= 4;
    }
    if ((spu_register_half(attributes, 3) & 0x10) || all)
    {
        registers[3] = spu_register_half(attributes, 4);
        *dirty |= 8;
    }
    xport_audio_unlock();
}
#else
void SsQueueRegisters(sint32 voice, SndRegisterAttr *attributes)
{
    uint32 base, dirty_address;
    uint16 value, dirty;
    uint32 all;
    if ((uint32)voice >= SPU_VOICE_COUNT || !attributes)
    {
    #ifndef NDEBUG
        fprintf(stderr, "Invalid SsQueueRegisters voice/attributes\n");
    #endif
        abort();
    }
    xport_audio_lock();
    if (!libsnd.voice_volume_address || !libsnd.voice_update_flags_address || !psx_addr(libsnd.voice_volume_address, 16u * (SPU_VOICE_COUNT - 1u) + 12u))
    {
        xport_audio_unlock();
    #ifndef NDEBUG
        fprintf(stderr, "SsQueueRegisters requires the deferred voice cache binding\n");
    #endif
        abort();
    }
    base = libsnd.voice_volume_address + 16u * (uint32)voice;
    dirty_address = libsnd.voice_update_flags_address + (uint32)voice;
    all = spu_register_half(attributes, 3u) == 0u;
    if ((spu_register_half(attributes, 3u) & 1u) || all)
    {
        value = spu_register_half(attributes, 0u) & 0x7FFFu;
        dirty = r_u8(dirty_address) | 1u;
        w_u16(base, value);
        w_u8(dirty_address, dirty);
    }
    if ((spu_register_half(attributes, 3u) & 2u) || all)
    {
        w_u16(base + 2u, spu_register_half(attributes, 1u) & 0x7FFFu);
        w_u8(dirty_address, r_u8(dirty_address) | 2u);
    }
    if ((spu_register_half(attributes, 3u) & 4u) || all)
    {
        w_u16(base + 8u, spu_register_half(attributes, 5u));
        w_u8(dirty_address, r_u8(dirty_address) | 0x10u);
    }
    if ((spu_register_half(attributes, 3u) & 8u) || all)
    {
        w_u16(base + 10u, spu_register_half(attributes, 6u));
        w_u8(dirty_address, r_u8(dirty_address) | 0x20u);
    }
    if ((spu_register_half(attributes, 3u) & 0x20u) || all)
    {
        w_u16(base + 4u, spu_register_half(attributes, 2u));
        w_u8(dirty_address, r_u8(dirty_address) | 4u);
    }
    if ((spu_register_half(attributes, 3u) & 0x10u) || all)
    {
        w_u16(base + 6u, spu_register_half(attributes, 4u));
        w_u8(dirty_address, r_u8(dirty_address) | 8u);
    }
    xport_audio_unlock();
}
#endif

#if defined(XPORT_NATIVE_VM) && XPORT_NATIVE_VM
sint32 SsVoiceCheck(uint32 voice, sint32 bank_program, sint16 note)
{
    SpuVoiceState *record;
    sint32 result = -1;
    if (voice >= SPU_VOICE_COUNT)
        return -1;
    xport_audio_lock();
    record = native_vm.voices + voice;
    if ((sint16)record->bank == (bank_program >> 8) && (sint16)record->program == (uint8)bank_program && (sint16)record->note == note)
        result = 0;
    xport_audio_unlock();
    return result;
}
#else
sint32 SsVoiceCheck(uint32 voice, sint32 bank_program, sint16 note)
{
    uint32 record;
    sint32 result = -1;
    if (voice >= SPU_VOICE_COUNT)
        return -1;
    xport_audio_lock();
    if (!libsnd.vm_guest.voices)
    {
        xport_audio_unlock();
    #ifndef NDEBUG
        fprintf(stderr, "SsVoiceCheck requires the voice record binding\n");
    #endif
        abort();
    }
    record = libsnd.vm_guest.voices + 54u * voice;
    if (r_s16(record + 24u) == (bank_program >> 8) && r_s16(record + 20u) == (uint8)bank_program && r_s16(record + 14u) == note)
        result = 0;
    xport_audio_unlock();
    return result;
}
#endif

#if defined(XPORT_NATIVE_VM) && XPORT_NATIVE_VM
sint32 SsUtSetDetVVol(uint16 voice, sint16 left, sint16 right)
{
    uint16 *registers;
    uint8 flags;
    if (voice >= SPU_VOICE_COUNT)
        return -1;
    xport_audio_lock();
    registers = native_vm.registers + voice * 8;
    registers[1] = right;
    flags = native_vm.dirty[voice];
    registers[0] = left;
    native_vm.dirty[voice] = flags | 3;
    xport_audio_unlock();
    return 0;
}
#else
sint32 SsUtSetDetVVol(uint16 voice, sint16 left, sint16 right)
{
    uint32 values, flags_address;
    uint8 flags;
    if (voice >= SPU_VOICE_COUNT)
        return -1;
    xport_audio_lock();
    if (!libsnd.voice_volume_address || !libsnd.voice_update_flags_address)
    {
        xport_audio_unlock();
    #ifndef NDEBUG
        fprintf(stderr, "SsUtSetDetVVol requires the deferred voice volume binding\n");
    #endif
        abort();
    }
    values = libsnd.voice_volume_address + 16u * voice;
    flags_address = libsnd.voice_update_flags_address + voice;
    w_u16(values + 2u, (uint16)right);
    flags = r_u8(flags_address);
    w_u16(values, (uint16)left);
    w_u8(flags_address, (uint8)(flags | 3u));
    xport_audio_unlock();
    return 0;
}
#endif

void xport_libsnd_bind_voice_limits(uint32 protection_address, uint32 count_address)
{
    if ((protection_address && !psx_addr(protection_address, 4u)) || (count_address && !psx_addr(count_address, 1u)))
    {
#ifndef NDEBUG
        fprintf(stderr, "Invalid libsnd voice limit binding %08X/%08X\n", protection_address, count_address);
#endif
        abort();
    }
    xport_audio_lock();
    libsnd.voice_protection_address = protection_address;
    libsnd.voice_count_address = count_address;
    xport_audio_unlock();
}

#if defined(XPORT_NATIVE_VM) && XPORT_NATIVE_VM
static uint32 libsnd_voice_protection(void)
{
    return native_vm.protection;
}
#else
static uint32 libsnd_voice_protection(void)
{
    return libsnd.voice_protection_address ? r_u32(libsnd.voice_protection_address) : 0u;
}
#endif

static sint16 libsnd_voice_count(void)
{
#if defined(XPORT_NATIVE_VM) && XPORT_NATIVE_VM
    sint16 count = (sint8)native_vm.events.voice_count;
#else
    sint16 count = libsnd.voice_count_address ? (sint8)r_u8(libsnd.voice_count_address) : SPU_VOICE_COUNT;
#endif
    if (count > SPU_VOICE_COUNT)
    {
#ifndef NDEBUG
        fprintf(stderr, "Unsupported libsnd voice count %d\n", count);
#endif
        abort();
    }
    return count > 0 ? count : 0;
}

uint32 SsVoKeyOn(uint32 bank_program, uint32 note_fine, uint16 left, uint16 right)
{
    sint16 bank = (sint16)((bank_program >> 8) & 0xffffu);
    sint16 program = (sint16)(bank_program & 0xffu);
    uint16 note = (uint16)(note_fine >> 8);
    sint32 selection_note = (sint8)(uint8)note;
    const VAB_PROGRAM *program_entry;
    uint32 mask = 0u;
    sint16 tone_index;
    xport_audio_lock();
    program_entry = vab_get_program(bank, program);
    if (!program_entry)
    {
        xport_audio_unlock();
        return 0xffffffffu;
    }
    if (!left && !right)
    {
        sint16 voice;
        uint32 stopped = 0u;
        for (voice = 0; voice < libsnd_voice_count(); ++voice)
        {
            if (libsnd_voice_protection() & (1u << voice))
                continue;
            if (libsnd.voices[voice].direct_keyon && libsnd.voices[voice].bank == bank && libsnd.voices[voice].program == program && (sint32)(sint16)libsnd.voices[voice].note == (sint32)note)
            {
                spu_key_off(1u << voice);
                libsnd.voices[voice].keyed = 0;
                ++stopped;
            }
        }
        xport_audio_unlock();
        return stopped;
    }
    /* Unsupported volume inputs remain explicit until their native contract is reviewed */
    if (left > 127u || right > 127u)
    {
        xport_audio_unlock();
#ifndef NDEBUG
        fprintf(stderr, "SsVoKeyOn volume above127 is not implemented\n");
#endif
        abort();
    }
    for (tone_index = 0; tone_index < program_entry->tone_count; ++tone_index)
    {
        const VAB_TONE *tone = vab_get_tone(bank, program, tone_index);
        sint16 voice, selected = -1, voice_count;
        uint16 best_priority = (uint16)(sint16)(sint8)tone->priority;
        uint16 best_envelope = 0xffffu;
        uint16 best_age = 0u;
        uint32 protection;
        if (selection_note < tone->note_min || selection_note > tone->note_max)
            continue;
        if (tone->sample == 255)
        {
            xport_audio_unlock();
#ifndef NDEBUG
            fprintf(stderr, "SsVoKeyOn noise tone is not implemented\n");
#endif
            abort();
        }
        protection = libsnd_voice_protection();
        voice_count = libsnd_voice_count();
        for (voice = 0; voice < voice_count; ++voice)
        {
            sint16 priority = (sint8)libsnd.voices[voice].priority;
            if (protection & (1u << voice))
                continue;
            if (!libsnd.voices[voice].keyed && spu.voices[voice].envelope == 0)
            {
                selected = voice;
                break;
            }
            if (priority < best_priority)
            {
                best_priority = (uint16)priority;
                best_envelope = spu.voices[voice].envelope;
                best_age = libsnd.voices[voice].age;
                selected = voice;
            }
            else if (priority == best_priority && (spu.voices[voice].envelope < best_envelope || (spu.voices[voice].envelope == best_envelope && (sint16)libsnd.voices[voice].age > (sint32)best_age)))
            {
                best_envelope = spu.voices[voice].envelope;
                best_age = libsnd.voices[voice].age;
                selected = voice;
            }
        }
        if (selected < 0)
            mask = 0xffffffffu;
        else
        {
            protection = libsnd_voice_protection();
            voice_count = libsnd_voice_count();
            for (voice = 0; voice < voice_count; ++voice)
                if (!(protection & (1u << voice)))
                    ++libsnd.voices[voice].age;
            /* The reviewed key-on chain discards fine and publishes zero */
            selected = start_voice(selected, bank, program, tone_index, note, 0, left, right);
            if (selected < 0)
                mask = 0xffffffffu;
            else
            {
                libsnd.voices[selected].direct_keyon = 1u;
                mask |= 1u << selected;
            }
        }
    }
    xport_audio_unlock();
    return mask;
}

sint16 SsUtKeyOnV(sint16 voice, sint16 bank, sint16 program, sint16 tone, sint16 note, sint16 fine, sint16 left, sint16 right)
{
#if defined(XPORT_NATIVE_VM) && XPORT_NATIVE_VM
    return spu_sequence_vm_key_on_tone((uint16)voice, bank, program, tone, note, fine, left, right, 0);
#else
    sint16 result;
    xport_audio_lock();
    result = start_voice(voice, bank, program, tone, note, fine, left, right);
    xport_audio_unlock();
    return result;
#endif
}

static void spu_init_native_transfer(void)
{
    SpuTransferBinding binding = {0};
    SpuNativeTransferState *state = &spu.transfer;

    if (transfer.requested_mode && native_transfer_binding != SPU_TRANSFER_NATIVE)
        return;
    state->completion = 1u;
    state->address_units = 0x200u;
    binding.requested_mode = &state->requested_mode;
    binding.normalized_mode = &state->normalized_mode;
    binding.address_units = &state->address_units;
    binding.completion = &state->completion;
    binding.native_completion_callback = &state->completion_callback;
    binding.source_address = &state->source_address;
    binding.dma_blocks = &state->dma_blocks;
    binding.address_shift = SPU_RAM_ADDRESS_SHIFT;
    binding.address_rounding_enabled = 1u;
    binding.address_rounding_unit = SPU_RAM_ADDRESS_UNIT;
    binding.address_alignment_mask = SPU_RAM_ADDRESS_MASK;
    binding.block_bytes = 64u;
    binding.ticks_per_block = 1u;
    binding.clock = spu_native_transfer_clock;
    binding.wait_until = spu_native_transfer_wait;
    binding.complete_event = spu_native_transfer_complete;
    binding.guest_address = spu_native_transfer_address;
    if (!spu_bind_transfer(&binding))
        abort();
    native_transfer_binding = SPU_TRANSFER_NATIVE;
}

static uint16 spu_reset_read(uint32 address, sint32 native)
{
    uint32 value;
    if (!native)
        return r_u16(address);
    if (!spu_mmio_read(NULL, address, 2u, &value))
        abort();
    return value;
}

static void spu_reset_write(uint32 address, uint16 value, sint32 native)
{
    if (!native)
        w_u16(address, value);
    else if (!spu_mmio_write(NULL, address, 2u, value, 3u))
        abort();
}

static void spu_reset_write_by_io(const uint8 *source, uint32 bytes, sint32 native)
{
    uint32 port = SPU_REGISTER_BASE;
    uint32 original_status = spu_reset_read(port + 430, native) & 0x7FF;
    uint32 chunk, written, count, status;
    spu_reset_write(port + 422, spu.transfer.address_units, native);
    while (bytes != 0)
    {
        chunk = bytes < 65 ? bytes : 64;
        port = SPU_REGISTER_BASE;
        for (written = 0; (sint32)written < (sint32)chunk; written += 2)
        {
            spu_reset_write(port + 424, xport_load_le16(source), native);
            source += 2;
        }
        port = SPU_REGISTER_BASE;
        spu_reset_write(port + 426, (spu_reset_read(port + 426, native) & 0xFFCF) | 0x10, native);
        status = spu_reset_read(SPU_REGISTER_BASE + 430, native) & 0x400;
        count = 1;
        while (status != 0 && count < 0xF01)
        {
            status = spu_reset_read(SPU_REGISTER_BASE + 430, native) & 0x400;
            ++count;
        }
        if (status != 0)
#ifndef NDEBUG
            printf("SPU:T/O [%s]\n", "wait (wrdy H -> L)");
#else
            (void)0;
#endif
        bytes -= chunk;
    }
    port = SPU_REGISTER_BASE;
    spu_reset_write(port + 426, spu_reset_read(port + 426, native) & 0xFFCF, native);
    status = spu_reset_read(port + 430, native) & 0x7FF;
    count = 1;
    while (status != original_status && count < 0xF01)
    {
        status = spu_reset_read(SPU_REGISTER_BASE + 430, native) & 0x7FF;
        ++count;
    }
    if (status != original_status)
#ifndef NDEBUG
        printf("SPU:T/O [%s]\n", "wait (dmaf clear/W)");
#else
        (void)0;
#endif
}

void _spu_writeByIO(const uint8 *source, uint32 bytes)
{
    spu_reset_write_by_io(source, bytes, 0);
}

#if defined(XPORT_NATIVE_VM) && XPORT_NATIVE_VM
static void spu_init_psyq_native(void)
{
    uint32 index;
    uint32 base = 0x1F801C00;
    memset(&spu.key_control, 0, sizeof(spu.key_control));
    w_u32(0x1F8010F0, r_u32(0x1F8010F0) | 0xB0000);
    spu.transfer.normalized_mode = 0;
    spu.transfer.address_units = 0;
    w_u16(base + 384, 0);
    w_u16(base + 386, 0);
    w_u16(base + 426, 0);
    w_u16(base + 428, 4);
    w_u16(base + 388, 0);
    w_u16(base + 390, 0);
    w_u16(base + 396, 65535);
    w_u16(base + 398, 65535);
    w_u16(base + 408, 0);
    w_u16(base + 410, 0);
    spu.transfer.address_units = 512;
    for (index = 400; index <= 406; index += 2)
        w_u16(base + index, 0);
    for (index = 432; index <= 438; index += 2)
        w_u16(base + index, 0);
    if (!spu_upload(4096, spu_silent_sample, sizeof(spu_silent_sample)))
        abort();
    for (index = 0; index < 24; ++index)
    {
        uint32 voice = base + index * 16;
        w_u16(voice, 0);
        w_u16(voice + 2, 0);
        w_u16(voice + 4, 0x3FFF);
        w_u16(voice + 6, 512);
        w_u16(voice + 8, 0);
        w_u16(voice + 10, 0);
    }
    w_u16(base + 392, 65535);
    w_u16(base + 394, 255);
    w_u16(base + 396, 65535);
    w_u16(base + 398, 255);
    spu.transfer.completion = 1;
    w_u16(base + 426, 0xC000);
    spu.transfer.completion_callback = 0;
    spu.transfer.requested_mode = 0;
    reverb_control.reserved = 0;
    reverb_control.mode = 0;
    reverb_control.depth_left = 0;
    reverb_control.depth_right = 0;
    reverb_control.delay = 0;
    reverb_control.feedback = 0;
    w_u16(base + 418, spu_reverb_default_work[0]);
}
#endif

void SpuInit(void)
{
#if defined(XPORT_NATIVE_VM) && XPORT_NATIVE_VM
    ResetCallback();
#endif
    xport_audio_lock();
    spu_init();
    memset(&heap, 0, sizeof(heap));
    heap_failed = 0;
    reverb_control.reserved = 0u;
    reverb_control.work_address = 0xF204u;
    xport_audio_unlock();
#if defined(XPORT_NATIVE_VM) && XPORT_NATIVE_VM
    spu_init_psyq_native();
#endif
    xport_audio_init();
}

sint32 SpuInitMalloc(sint32 count, char *records)
{
    SpuHeapBinding binding = {0};
    uint32 first[2] = {0x40001010u, SPU_RAM_SIZE - 0x1010u};

    if (count <= 0 || !records || ((uintptr_t)records & 3u) != 0u)
        return 0;
    xport_audio_lock();
    memcpy(records, first, sizeof(first));
    spu.heap_last_idx = 0u;
    binding.entries = (uint32 *)records;
    binding.last_index = &spu.heap_last_idx;
    binding.capacity = (uint32)count + 1u;
    binding.limit = (uint32)count;
    binding.address_shift = SPU_RAM_ADDRESS_SHIFT;
    binding.alignment_mask = SPU_RAM_ADDRESS_MASK;
    binding.reserved_bytes = reverb_control.reserved ? (0x10000u - reverb_control.work_address) << 3 : 0u;
    spu_bind_heap(&binding);
    xport_audio_unlock();
    return count;
}

void SpuInitHot(void)
{
    SpuInit();
}

void SpuQuit(void)
{
    xport_audio_lock();
    spu_shutdown();
    xport_audio_unlock();
}

void SpuSetVoiceAttr(const SpuVoiceAttr *attr)
{
    sint32 voice;
    if (!attr)
        return;
    xport_audio_lock();
    spu_synchronize_device_clock();
    for (voice = 0; voice < SPU_VOICE_COUNT; ++voice)
    {
        SPU_VOICE_REGISTERS registers;
        if (!(attr->voice & SPU_KEYCH(voice)) || !spu_get_voice_registers(voice, &registers))
            continue;
        if (!attr->mask || (attr->mask & SPU_VOICE_VOLL))
            registers.volume_left = spu_volume_attr(attr->volume.left, attr->volmode.left, !attr->mask || (attr->mask & SPU_VOICE_VOLMODEL));
        if (!attr->mask || (attr->mask & SPU_VOICE_VOLR))
            registers.volume_right = spu_volume_attr(attr->volume.right, attr->volmode.right, !attr->mask || (attr->mask & SPU_VOICE_VOLMODER));
        if (!attr->mask || (attr->mask & SPU_VOICE_PITCH))
            registers.pitch = attr->pitch;
        if (!attr->mask || (attr->mask & SPU_VOICE_SAMPLE_NOTE))
            spu.sample_notes[voice] = attr->sample_note;
        if (!attr->mask || (attr->mask & SPU_VOICE_NOTE))
            registers.pitch = _spu_note2pitch(spu.sample_notes[voice] >> 8, spu.sample_notes[voice] & 255, attr->note >> 8, attr->note & 255);
        if (!attr->mask || (attr->mask & SPU_VOICE_WDSA))
            registers.start_address = _spu_FsetRXXa(-1, attr->addr);
        if (!attr->mask || (attr->mask & SPU_VOICE_LSAX))
            registers.repeat_address = _spu_FsetRXXa(-1, attr->loop_addr);
        if (!attr->mask || (attr->mask & SPU_VOICE_ADSR_ADSR1))
            registers.adsr1 = attr->adsr1;
        if (!attr->mask || (attr->mask & SPU_VOICE_ADSR_ADSR2))
            registers.adsr2 = attr->adsr2;
        if (!attr->mask || (attr->mask & SPU_VOICE_ADSR_AR))
            registers.adsr1 = spu_adsr_attack(registers.adsr1, attr, attr->mask);
        if (!attr->mask || (attr->mask & SPU_VOICE_ADSR_DR))
        {
            uint16 rate = attr->dr < 16u ? attr->dr : 15u;
            registers.adsr1 = (uint16)((registers.adsr1 & 0xff0fu) | (rate << 4));
        }
        if (!attr->mask || (attr->mask & SPU_VOICE_ADSR_SL))
        {
            uint16 level = attr->sl < 16u ? attr->sl : 15u;
            registers.adsr1 = (uint16)((registers.adsr1 & 0xfff0u) | level);
        }
        if (!attr->mask || (attr->mask & SPU_VOICE_ADSR_SR))
            registers.adsr2 = spu_adsr_sustain(registers.adsr2, attr, attr->mask);
        if (!attr->mask || (attr->mask & SPU_VOICE_ADSR_RR))
        {
            uint16 rate = attr->rr < 32u ? attr->rr : 31u;
            uint16 mode = (!attr->mask || (attr->mask & SPU_VOICE_ADSR_RMODE)) && attr->r_mode == 7 ? 0x20u : 0u;
            registers.adsr2 = (uint16)((registers.adsr2 & 0xffc0u) | mode | rate);
        }
        spu_set_voice_registers(voice, &registers);
    }
    xport_audio_unlock();
}

void SpuGetVoiceAttr(SpuVoiceAttr *attr)
{
    SpuVoiceAttr result;
    SPU_VOICE_REGISTERS registers;
    uint32 requested;
    sint32 voice;
    if (!attr)
        return;
    requested = attr->voice;
    for (voice = 0; voice < SPU_VOICE_COUNT && !(requested & SPU_KEYCH(voice)); ++voice)
        ;
    if (voice == SPU_VOICE_COUNT)
        return;
    xport_audio_lock();
    spu_synchronize_device_clock();
    if (!spu_get_voice_registers(voice, &registers))
    {
        xport_audio_unlock();
        return;
    }
    result = *attr;
    result.volume.left = registers.volume_left;
    result.volume.right = registers.volume_right;
    result.volumex = result.volume;
    result.pitch = registers.pitch;
    result.sample_note = spu.sample_notes[voice];
    result.envx = (sint16)spu.voices[voice].envelope;
    result.addr = (uint32)registers.start_address * 8u;
    result.loop_addr = (uint32)registers.repeat_address * 8u;
    result.adsr1 = registers.adsr1;
    result.adsr2 = registers.adsr2;
    *attr = result;
    xport_audio_unlock();
}

static uint32 spu_key_guest_addresses[5];
static uint32 key_status_state_address, key_status_registers_pointer_address;

void spu_bind_key_guest(uint32 state_address, uint32 event_mode_address, uint32 event_flags_address, uint32 pending_mask_address, uint32 registers_address)
{
    spu_key_guest_addresses[0] = state_address;
    spu_key_guest_addresses[1] = event_mode_address;
    spu_key_guest_addresses[2] = event_flags_address;
    spu_key_guest_addresses[3] = pending_mask_address;
    spu_key_guest_addresses[4] = registers_address;
}

static uint32 spu_set_key_bound(sint32 on_off, uint32 voice_bit)
{
    uint32 pending, opposite, cached, result, registers;
    uint32 low = voice_bit & 0xFFFF;
    uint32 high = voice_bit >> 16;
    if (r_u32(spu_key_guest_addresses[1]) & 1)
    {
        pending = spu_key_guest_addresses[4] + (on_off == SPU_ON ? 0 : 4);
        opposite = spu_key_guest_addresses[4] + (on_off == SPU_ON ? 4 : 0);
        w_u16(pending, low);
        w_u16(pending + 2, high);
        w_u32(spu_key_guest_addresses[2], r_u32(spu_key_guest_addresses[2]) | 1);
        cached = r_u32(spu_key_guest_addresses[3]);
        w_u32(spu_key_guest_addresses[3], on_off == SPU_ON ? cached | voice_bit : cached & ~voice_bit);
        if (r_u16(opposite) & low)
            w_u16(opposite, r_u16(opposite) & ~low);
        result = r_u16(opposite + 2) & high;
        if (result)
        {
            result = r_u16(opposite + 2) & ~high;
            w_u16(opposite + 2, result);
        }
        return result;
    }
    if (on_off == SPU_ON)
    {
        cached = r_u32(spu_key_guest_addresses[0]);
        registers = r_u32(key_status_registers_pointer_address);
        result = cached | voice_bit;
        w_u16(registers + 0x188, low);
        w_u16(registers + 0x18A, high);
    }
    else
    {
        registers = r_u32(key_status_registers_pointer_address);
        w_u16(registers + 0x18C, low);
        w_u16(registers + 0x18E, high);
        cached = r_u32(spu_key_guest_addresses[0]);
        result = cached & ~voice_bit;
    }
    w_u32(spu_key_guest_addresses[0], result);
    return result;
}

uint32 spu_set_key(sint32 on_off, uint32 voice_bit)
{
    uint32 cached;
    uint32 result;
    uint32 pending;
    uint32 opposite;
    uint32 low;
    uint32 high;
    uint16 *pending_registers;
    uint16 *opposite_registers;

    voice_bit &= SPU_ALLCH;
    if (on_off != SPU_ON && on_off != SPU_OFF)
        return 1;
    if (spu_key_guest_addresses[0] && key_status_state_address == spu_key_guest_addresses[0] && key_status_registers_pointer_address)
        return spu_set_key_bound(on_off, voice_bit);
    xport_audio_lock();
    if (spu_key_guest_addresses[0] && (r_u32(spu_key_guest_addresses[1]) & 1u))
    {
        pending = spu_key_guest_addresses[4] + (on_off == SPU_ON ? 0u : 4u);
        opposite = spu_key_guest_addresses[4] + (on_off == SPU_ON ? 4u : 0u);
        low = voice_bit & 0xffffu;
        high = voice_bit >> 16;
        w_u16(pending, (uint16)low);
        w_u16(pending + 2u, (uint16)high);
        w_u32(spu_key_guest_addresses[2], r_u32(spu_key_guest_addresses[2]) | 1u);
        cached = r_u32(spu_key_guest_addresses[3]);
        w_u32(spu_key_guest_addresses[3], on_off == SPU_ON ? cached | voice_bit : cached & ~voice_bit);
        if (r_u16(opposite) & low)
            w_u16(opposite, (uint16)(r_u16(opposite) & ~low));
        result = r_u16(opposite + 2u) & high;
        if (result != 0)
        {
            result = r_u16(opposite + 2u) & ~high;
            w_u16(opposite + 2u, (uint16)result);
        }
    }
    else if (!spu_key_guest_addresses[0] && (spu.key_control.event_mode & 1u))
    {
        pending_registers = on_off == SPU_ON ? spu.key_control.key_on : spu.key_control.key_off;
        opposite_registers = on_off == SPU_ON ? spu.key_control.key_off : spu.key_control.key_on;
        low = voice_bit & 0xffffu;
        high = voice_bit >> 16;
        pending_registers[0] = (uint16)low;
        pending_registers[1] = (uint16)high;
        spu.key_control.event_flags |= 1u;
        cached = spu.key_control.pending_state;
        spu.key_control.pending_state = on_off == SPU_ON ? cached | voice_bit : cached & ~voice_bit;
        if (opposite_registers[0] & low)
            opposite_registers[0] = (uint16)(opposite_registers[0] & ~low);
        result = opposite_registers[1] & high;
        if (result != 0)
        {
            result = opposite_registers[1] & ~high;
            opposite_registers[1] = (uint16)result;
        }
    }
    else if (on_off == SPU_ON)
    {
        cached = spu_key_guest_addresses[0] ? r_u32(spu_key_guest_addresses[0]) : spu.key_control.key_state;
        spu_key_on(voice_bit);
        result = cached | voice_bit;
        if (spu_key_guest_addresses[0])
            w_u32(spu_key_guest_addresses[0], result);
        else
            spu.key_control.key_state = result;
    }
    else
    {
        spu_key_off(voice_bit);
        cached = spu_key_guest_addresses[0] ? r_u32(spu_key_guest_addresses[0]) : spu.key_control.key_state;
        result = cached & ~voice_bit;
        if (spu_key_guest_addresses[0])
            w_u32(spu_key_guest_addresses[0], result);
        else
            spu.key_control.key_state = result;
        /* A later SDK stop cancels an unrendered SDK start */
        spu.key_on_pending &= ~voice_bit;
    }
    xport_audio_unlock();
    return result;
}

void SpuSetKey(sint32 on_off, uint32 voice_bit)
{
    spu_set_key(on_off, voice_bit);
}

/* Caller holds the audio lock; pending masks never publish MMIO */
static uint32 spu_set_pending_mask_locked(sint32 mode, uint32 mask, uint16 pending[2], uint32 event_flag)
{
    uint16 low = pending[0], high = pending[1];
    uint32 result = low | ((uint32)(high & 255) << 16);
    if (mode == 1)
    {
        low |= mask;
        high |= (mask >> 16) & 255;
        result |= mask;
    }
    else if (mode == 0)
    {
        low &= ~mask;
        high &= ~((mask >> 16) & 255);
        result &= ~mask;
    }
    else
        return result & SPU_ALLCH;
    pending[0] = low;
    pending[1] = high;
    spu_key_control->event_flags |= event_flag;
    return result & SPU_ALLCH;
}

static uint32 spu_set_register_mask(sint32 mode, uint32 mask, uint32 low_address)
{
    uint16 low = r_u16(low_address);
    uint16 high = r_u16(low_address + 2);
    uint32 result = low | ((uint32)(high & 255) << 16);
    if (mode != 0 && mode != 1 && mode != 8)
        return result;
    if (mode == 8)
    {
        low = mask;
        high = (mask >> 16) & 255;
        result = mask;
    }
    else if (mode == 1)
    {
        low |= mask;
        high |= (mask >> 16) & 255;
        result |= mask;
    }
    else
    {
        low &= ~mask;
        high &= ~((mask >> 16) & 255);
        result &= ~mask;
    }
    w_u16(low_address, low);
    w_u16(low_address + 2, high);
    return result & SPU_ALLCH;
}

uint32 SpuSetNoiseVoice(sint32 mode, uint32 mask)
{
    return spu_set_register_mask(mode, mask, 0x1F801D94);
}

#if defined(XPORT_NATIVE_VM) && XPORT_NATIVE_VM
uint32 SpuSetReverbVoice(sint32 on_off, uint32 voice_bit)
{
    uint32 result;
    if (on_off == 8)
        return spu_set_register_mask(on_off, voice_bit, 0x1F801D98);
    xport_audio_lock();
    if (spu_key_guest_addresses[0] == 0u && (spu_key_control->event_mode & 1u) != 0u)
    {
        result = spu_set_pending_mask_locked(on_off, voice_bit, spu_key_control->reverb_mask, 8u);
        xport_audio_unlock();
        return result;
    }
    xport_audio_unlock();
    return spu_set_register_mask(on_off, voice_bit, 0x1F801D98);
}
#else
uint32 SpuSetReverbVoice(sint32 on_off, uint32 voice_bit)
{
    uint32 result;
    xport_audio_lock();
    if (on_off != 8 && spu_key_guest_addresses[0] == 0u && (spu_key_control->event_mode & 1u) != 0u)
        result = spu_set_pending_mask_locked(on_off, voice_bit, spu_key_control->reverb_mask, 8u);
    else
    {
        if (on_off == SPU_ON)
            spu.reverb_voice_mask |= voice_bit & SPU_ALLCH;
        else if (on_off == SPU_OFF)
            spu.reverb_voice_mask &= ~(voice_bit & SPU_ALLCH);
        result = spu.reverb_voice_mask;
    }
    xport_audio_unlock();
    return result;
}
#endif

sint32 SpuReserveReverbWorkArea(sint32 mode)
{
    sint32 result;
    xport_audio_lock();
    result = mode != 0 && !spu_reverb_work_occupied(reverb_control.work_address);
    reverb_control.reserved = (uint32)result;
    if (heap.entries)
        heap.reserved_bytes = result ? (0x10000u - reverb_control.work_address) << heap.address_shift : 0u;
    xport_audio_unlock();
    return result;
}

sint32 SpuIsReverbWorkAreaReserved(sint32 mode)
{
    sint32 result;
    xport_audio_lock();
    result = mode == -1 ? (sint32)reverb_control.reserved : !spu_reverb_work_occupied(reverb_control.work_address);
    xport_audio_unlock();
    return result;
}

static sint32 spu_clear_reverb_region(uint32 begin, uint32 length, const void *source, const SpuTransferBinding *binding, uint32 *direction)
{
    uint32 normalized, callback, guest;
    PsxInterruptCallback native_callback;
    sint32 result = 0;
    if (!source || !binding || !binding->normalized_mode || ((binding->completion_callback != NULL) == (binding->native_completion_callback != NULL)) || !binding->address_units || !binding->source_address || !binding->dma_blocks || binding->address_shift > 3u || binding->block_bytes != 64u || ((binding->clock != NULL) != (binding->wait_until != NULL)) || (binding->clock && !binding->ticks_per_block) || !binding->complete_event)
        return -1;
    guest = binding->guest_address ? binding->guest_address(binding->context, source) : xport_guest_buffer_address((void *)source, 1024);
    normalized = *binding->normalized_mode;
    callback = binding->completion_callback != NULL ? *binding->completion_callback : 0;
    native_callback = binding->native_completion_callback != NULL ? *binding->native_completion_callback : NULL;
    if (normalized == 1u)
        *binding->normalized_mode = 0u;
    if (callback)
        *binding->completion_callback = 0u;
    if (native_callback != NULL)
        *binding->native_completion_callback = NULL;
    do
    {
        uint32 bytes = length > 1024u ? 1024u : length;
        uint32 blocks = (bytes + 63u) >> 6;
        uint32 rounded = blocks << 6;
        uint64 deadline;
        XportDeviceOperation operation = {0};
        *binding->address_units = (uint16)(begin >> binding->address_shift);
        if (direction)
            *direction = 0u;
        *binding->source_address = guest;
        *binding->dma_blocks = blocks;
        if (xport_device_trace_enabled())
        {
            if (!xport_device_trace_begin(&operation, 0u, XPORT_DEVICE_SPU, XPORT_DEVICE_SPU_DMA, XPORT_DEVICE_DIRECTION_TO_DEVICE, 4u, 0u))
            {
                result = -1;
                break;
            }
            if (!guest)
                xport_device_trace_gap(&operation, 1u);
            xport_device_trace_stage(&operation, XPORT_DEVICE_SUBMIT, guest ? XPORT_DEVICE_EVIDENCE_EXACT : XPORT_DEVICE_EVIDENCE_UNCOVERED, guest ? XPORT_DEVICE_REGION_RAM : XPORT_DEVICE_REGION_NONE, guest & 0x1fffffu, XPORT_DEVICE_REGION_SPU_RAM, begin, rounded, 0xffffffffu);
            if (guest)
                xport_device_trace_stage(&operation, XPORT_DEVICE_FETCH, XPORT_DEVICE_EVIDENCE_EXACT, XPORT_DEVICE_REGION_RAM, guest & 0x1fffffu, XPORT_DEVICE_REGION_NONE, 0u, rounded, 0xffffffffu);
        }
        xport_audio_lock();
        reverb_control.control = (reverb_control.control & ~0x30u) | 0x20u;
        xport_audio_unlock();
        result = spu_upload_wrapped(begin, source, rounded) ? 0 : -1;
        if (!result && binding->clock)
        {
            deadline = binding->clock(binding->context) + (uint64)blocks * binding->ticks_per_block;
            if (!binding->wait_until(binding->context, deadline))
                result = -1;
        }
        xport_audio_lock();
        reverb_control.control &= ~0x30u;
        xport_audio_unlock();
        if (!result && binding->complete_event(binding->context) != 1)
            result = -1;
        if (operation.active)
            xport_device_trace_complete(&operation, !result);
        if (result || length <= 1024u)
            break;
        length -= 1024u;
        begin += 1024u;
    } while (1);
    if (normalized == 1u)
        *binding->normalized_mode = normalized;
    if (callback)
        *binding->completion_callback = callback;
    if (native_callback != NULL)
        *binding->native_completion_callback = native_callback;
    return result;
}

sint32 spu_clear_reverb_transfer(sint32 mode, const void *source, const SpuTransferBinding *binding, uint32 *direction)
{
    uint32 units;
    if ((uint32)mode >= 10u || !source || !binding || !binding->normalized_mode || ((binding->completion_callback != NULL) == (binding->native_completion_callback != NULL)) || !binding->address_units || !binding->source_address || !binding->dma_blocks || binding->address_shift > 3u || binding->block_bytes != 64u || ((binding->clock != NULL) != (binding->wait_until != NULL)) || (binding->clock && !binding->ticks_per_block) || !binding->complete_event)
        return -1;
    xport_audio_lock();
    units = reverb_preset_work[mode];
    if (spu_reverb_work_occupied(units))
    {
        xport_audio_unlock();
        return -1;
    }
    if (!mode)
        units = 65520u;
    xport_audio_unlock();
    if (units > 0x10000u)
        return -1;
    return spu_clear_reverb_region(units << binding->address_shift, (0x10000u - units) << binding->address_shift, source, binding, direction);
}

sint32 spu_clear_reverb_guest(const SpuNativeTransferGuestBinding *guest, uint32 begin, uint32 bytes, uint32 source, uint32 direction)
{
    const void *buffer;
    uint32 *direction_state;
    if (!guest || begin >= SPU_RAM_SIZE || bytes > SPU_RAM_SIZE - begin || guest->address_shift > 3u)
        return -1;
    buffer = psx_addr(source, 1024u);
    direction_state = (uint32 *)psx_addr(direction, 4u);
    if (!buffer || !direction_state || transfer_pending || transfer_operation.active || transfer_failed)
        return -1;
    if (native_transfer_binding != SPU_TRANSFER_GUEST)
    {
        if (!spu_bind_native_transfer_guest(guest))
            return -1;
    }
    else if (transfer.requested_mode != (uint32 *)psx_addr(guest->requested_mode, 4u) || transfer.normalized_mode != (uint32 *)psx_addr(guest->normalized_mode, 4u) || transfer.address_units != (uint16 *)psx_addr(guest->address_units, 2u) || transfer.completion != (uint32 *)psx_addr(guest->completion, 4u) || transfer.completion_callback != (uint32 *)psx_addr(guest->completion_callback, 4u) || transfer.source_address != (uint32 *)psx_addr(guest->source_address, 4u) || transfer.dma_blocks != (uint32 *)psx_addr(guest->dma_blocks, 4u) || transfer.address_shift != guest->address_shift)
        return -1;
    return spu_clear_reverb_region(begin, bytes, buffer, &transfer, direction_state);
}

static const uint8 spu_reverb_silence[1024] = {0};

/* Clear with the caller's transfer lifecycle and the private zero source */
sint32 spu_clear_reverb_work_area(sint32 mode, const SpuTransferBinding *binding, uint32 *direction)
{
    return spu_clear_reverb_transfer(mode, spu_reverb_silence, binding, direction);
}

sint32 spu_clear_reverb_native(uint32 begin, uint32 bytes)
{
    if (native_transfer_binding != SPU_TRANSFER_NATIVE || begin >= SPU_RAM_SIZE || bytes > SPU_RAM_SIZE - begin || transfer.address_shift > 3u || transfer_pending || transfer_operation.active || transfer_failed)
        return -1;
    return spu_clear_reverb_region(begin, bytes, spu_reverb_silence, &transfer, NULL);
}

sint32 SpuClearReverbWorkArea(sint32 mode)
{
    if (transfer_pending || transfer_operation.active || transfer_failed)
        return -1;
    return spu_clear_reverb_transfer(mode, spu_reverb_silence, &transfer, NULL);
}

#if defined(XPORT_NATIVE_VM) && XPORT_NATIVE_VM
static void libsnd_native_fail(const char *operation)
{
    #ifndef NDEBUG
    fprintf(stderr, "Unimplemented native PsyQ operation: %s\n", operation);
    #else
    (void)operation;
    #endif
    abort();
}

void SsFlush(void)
{
    uint32 index = (spu_vm_state()->events.history_index + 1) & 15;
    uint32 i, inactive = 0xFFFFFFFF, mask, low, high, key_off_low, key_on_low, key_off_high, key_on_high;
    spu_vm_state()->events.history_index = (uint32)(index);
    spu_vm_state()->events.silence_history[index] = (uint32)(0);
    for (i = 0; (sint32)i < (sint32)(sint8)spu_vm_state()->events.voice_count; ++i)
    {
        SpuVoiceAttr attributes = {0};
        attributes.voice = 1u << (i & 31);
        SpuGetVoiceAttr(&attributes);
        uint16 envelope = (uint16)attributes.envx;
        spu_vm_voice(i)->envelope = (uint16)(envelope);
        if (spu_vm_voice(i)->envelope == 0)
        {
            index = spu_vm_state()->events.history_index;
            mask = spu_vm_state()->events.silence_history[index];
            spu_vm_state()->events.silence_history[index] = (uint32)(mask | (1u << (i & 31)));
        }
    }
    if (spu_vm_state()->events.silence_cleanup_disabled == 0)
    {
        for (i = 0; i < 15; ++i)
            inactive &= spu_vm_state()->events.silence_history[i];
        for (i = 0; (sint32)i < (sint32)(sint8)spu_vm_state()->events.voice_count; ++i)
            if ((inactive & (1u << (i & 31))) != 0)
            {
                if (spu_vm_voice(i)->status == 2)
                {
                    // Original SRA16 extends the low voice mask through the high byte
                    if ((sint32)i < 16)
                        mask = (uint32)(sint32)(sint16)(1 << i);
                    else
                        mask = ((1u << ((i - 16) & 31)) & 255) << 16;
                    SpuSetNoiseVoice(0, mask);
                }
                spu_vm_voice(i)->status = (uint8)(0);
            }
    }
    key_off_low = spu_vm_state()->events.key_off[0];
    key_on_low = spu_vm_state()->events.key_on[0];
    key_off_high = spu_vm_state()->events.key_off[1];
    spu_vm_state()->events.key_on[0] = (uint16)(key_on_low & ~key_off_low);
    key_on_high = spu_vm_state()->events.key_on[1];
    spu_vm_state()->events.key_on[1] = (uint16)(key_on_high & ~key_off_high);
    for (i = 0; i < 24; ++i)
    {
        if (spu_vm_voice(i)->volume_modulation[0] != 0)
        {
            if (spu_voice_modulation_callbacks.volume == NULL)
                libsnd_native_fail("Unbound automatic voice volume callback");
            spu_voice_modulation_callbacks.volume(i);
        }
        if (spu_vm_voice(i)->pitch_modulation[0] != 0)
        {
            if (spu_voice_modulation_callbacks.pitch == NULL)
                libsnd_native_fail("Unbound automatic voice pitch callback");
            spu_voice_modulation_callbacks.pitch(i);
        }
    }
    for (i = 0; i < 24; ++i)
    {
        uint16 *shadow = spu_vm_state()->registers + i * 8;
        SpuVoiceAttr attr;
        memset(&attr, 0, sizeof(attr));
        attr.voice = 1 << i;
        if ((spu_vm_state()->dirty[i] & 1) != 0)
        {
            attr.mask |= 3;
            attr.volume.left = (sint16)shadow[0];
            attr.volume.right = (sint16)shadow[1];
        }
        if ((spu_vm_state()->dirty[i] & 4) != 0)
        {
            attr.mask |= 0x10;
            attr.pitch = shadow[2];
        }
        if ((spu_vm_state()->dirty[i] & 8) != 0)
        {
            attr.mask |= 0x80;
            attr.addr = (uint32)shadow[3] << 3;
        }
        if ((spu_vm_state()->dirty[i] & 0x10) != 0)
        {
            attr.mask |= 0x60000;
            attr.adsr1 = shadow[4];
            attr.adsr2 = shadow[5];
        }
        if (attr.mask != 0)
            SpuSetVoiceAttr(&attr);
        spu_vm_state()->dirty[i] = (uint8)(0);
    }
    high = (uint8)spu_vm_state()->events.key_off[1];
    low = spu_vm_state()->events.key_off[0];
    SpuSetKey(0, low | (high << 16));
    high = (uint8)spu_vm_state()->events.key_on[1];
    low = spu_vm_state()->events.key_on[0];
    SpuSetKey(1, low | (high << 16));
    // Mode 8 replaces the complete original voice masks
    high = (uint8)spu_vm_state()->events.reverb[1];
    low = spu_vm_state()->events.reverb[0];
    SpuSetReverbVoice(8, low | (high << 16));
    high = (uint8)spu_vm_state()->events.noise[1];
    low = spu_vm_state()->events.noise[0];
    SpuSetNoiseVoice(8, low | (high << 16));
    spu_vm_state()->events.key_off[0] = (uint16)(0);
    spu_vm_state()->events.key_off[1] = (uint16)(0);
    spu_vm_state()->events.key_on[0] = (uint16)(0);
    spu_vm_state()->events.key_on[1] = (uint16)(0);
    spu_vm_state()->events.noise[0] = (uint16)(0);
    spu_vm_state()->events.noise[1] = (uint16)(0);
}

static void libsnd_initialize_voices(uint32 requested_count)
{
    uint32 count = (uint32)(sint32)(sint8)requested_count < 24 ? (uint32)(sint32)(sint8)requested_count : 24;
    uint32 i, j;
    spu_native_transfer_state->completion = 1;
    spu_vm_state()->release_offset = (uint16)(0);
    if (SpuInitMalloc(32, (char *)spu_vm_state()->heap_records) != 32)
        libsnd_native_fail("Initialize native sequencer SPU heap");
    for (i = 0; i < 192; ++i)
        spu_vm_state()->registers[i] = (uint16)(0);
    for (i = 0; i < 24; ++i)
        spu_vm_state()->dirty[i] = (uint8)(0);
    spu_vm_state()->events.voice_count = (uint8)(count);
    for (i = 0; i < count; ++i)
    {
        SpuVoiceState *record = spu_vm_voice(i);
        uint32 bit = 1 << i;
        SpuVoiceAttr attr;
        record->sample = (uint16)(255);
        record->age = (uint16)(24);
        uint16 *fields[] = {&record->pitch, &record->envelope, &record->velocity, &record->channel, &record->program_tone_index, &record->program, &record->volume_modulation[0], &record->volume_modulation[1], &record->volume_modulation[2], &record->volume_modulation[3], &record->volume_modulation[4], &record->pitch_modulation[0], &record->pitch_modulation[1], &record->pitch_modulation[2], &record->pitch_modulation[3], &record->pitch_modulation[4]};
        for (j = 0; j < sizeof(fields) / sizeof(fields[0]); ++j)
            *fields[j] = 0;
        record->sequence = (uint16)(0xFFFF);
        record->tone = (uint16)(255);
        record->pan = (uint8)(64);
        record->status = (uint8)(0);
        memset(&attr, 0, sizeof(attr));
        attr.voice = bit;
        attr.mask = 0x60093;
        attr.pitch = 0x1000;
        attr.addr = 0x1000;
        attr.adsr1 = 0x80FF;
        attr.adsr2 = 0x4000;
        SpuSetVoiceAttr(&attr);
        spu_vm_state()->note.voice = (uint16)(i);
        record->status = (uint8)(0);
        record->pitch = (uint16)(0);
        record->sample = (uint16)(0);
        spu_vm_state()->events.key_off[0] = (uint16)(spu_vm_state()->events.key_off[0] | bit);
        spu_vm_state()->events.key_off[1] = (uint16)(spu_vm_state()->events.key_off[1] | (bit >> 16));
        spu_vm_state()->events.key_on[0] = (uint16)(spu_vm_state()->events.key_on[0] & ~bit);
        spu_vm_state()->events.key_on[1] = (uint16)(spu_vm_state()->events.key_on[1] & ~(bit >> 16));
    }
    spu_vm_state()->events.key_on[0] = (uint16)(0);
    spu_vm_state()->events.key_on[1] = (uint16)(0);
    spu_vm_state()->events.key_off[0] = (uint16)(0);
    spu_vm_state()->events.reverb[0] = (uint16)(0);
    spu_vm_state()->events.reverb[1] = (uint16)(0);
    spu_vm_state()->events.noise[0] = (uint16)(0);
    spu_vm_state()->events.noise[1] = (uint16)(0);
    spu_vm_state()->events.silence_cleanup_disabled = (uint8)(0);
    spu_vm_state()->mono_mode = (uint16)(0);
    SsFlush();
}

static void libsnd_initialize_native(void)
{
    uint32 voice, field;
    for (voice = 0; voice < 24; ++voice)
        for (field = 0; field < 8; ++field)
            w_u16(0x1F801C00 + voice * 16 + field * 2, spu_sequence_voice_defaults[field]);
    for (field = 0; field < 16; ++field)
        w_u16(0x1F801D80 + field * 2, spu_sequence_common_defaults[field]);
    libsnd_initialize_voices(24);
    spu_sequence_clear_marks();
    spu_sequence_manager()->tick_rate = (uint32)(60);
    spu_sequence_manager()->opened = (uint32)(0);
    spu_sequence_manager()->locked = (uint32)(0);
}
#else
void SsFlush(void)
{
    abort();
}
#endif

#if defined(XPORT_NATIVE_VM) && XPORT_NATIVE_VM
sint32 SsAllocateVoices(uint8 count, uint8 priority)
{
    uint32 voice_mask = 0;
    uint32 selected_count, voice, bit;
    uint16 best_priority, best_envelope;
    sint32 best_age, current_priority;
    uint8 busy_voice, free_voice;
    uint32 remaining;
    SpuVoiceState *record;
    for (selected_count = 0; selected_count < (uint8)count; ++selected_count)
    {
        best_priority = (uint8)priority;
        best_envelope = 0xFFFF;
        best_age = 0;
        busy_voice = 99;
        free_voice = 99;
        for (voice = 0; voice < spu_vm_state()->events.voice_count; ++voice)
        {
            if (voice_mask & (1 << voice))
                continue;
            record = spu_vm_voice(voice);
            if (!record->status && !record->envelope)
            {
                free_voice = voice;
                break;
            }
            current_priority = (sint16)record->priority;
            if (current_priority < best_priority)
            {
                best_priority = record->priority;
                best_envelope = record->envelope;
                best_age = record->age;
                busy_voice = voice;
            }
            else if (current_priority == best_priority)
            {
                if (record->envelope < best_envelope)
                {
                    best_age = record->age;
                    best_envelope = record->envelope;
                    busy_voice = voice;
                }
                else if (record->envelope == best_envelope && best_age < (sint16)record->age)
                {
                    best_age = record->age;
                    busy_voice = voice;
                }
            }
        }
        if (free_voice == 99)
        {
            if (busy_voice == 99)
                return -1;
            bit = 1 << busy_voice;
        }
        else
            bit = 1 << free_voice;
        voice_mask |= bit;
    }
    remaining = count;
    for (voice = 0; voice < spu_vm_state()->events.voice_count; ++voice)
    {
        bit = 1 << voice;
        record = spu_vm_voice(voice);
        if (voice_mask & bit)
        {
            uint8 status = record->status;
            --remaining;
            record->age = (uint8)remaining;
            record->priority = (uint8)priority;
            record->volume_modulation[0] = 0;
            record->pitch_modulation[0] = 0;
            if (status == 2)
                SpuSetNoiseVoice(0, bit);
            record->status = 1;
        }
        else
            record->age += (uint8)count;
    }
    for (voice = 0; voice < 16; ++voice)
        spu_vm_state()->events.silence_history[voice] &= ~voice_mask;
    return voice_mask;
}

void SsSetVoiceSettings(sint32 voice, SndVoiceStats *settings)
{
    SpuVoiceState *record = spu_vm_voice(voice);
    record->sample = settings->vagId;
    record->pitch = settings->pitch;
    record->velocity = settings->vol;
    record->pan = (uint8)settings->pan;
    record->sequence = 33;
    record->note = settings->note;
    record->program_tone_index = settings->prog_actual;
    record->program = settings->prog_num;
    record->tone = settings->tone;
    record->bank = settings->vabId;
}

sint8 SsSetReservedVoice(sint8 count)
{
    if ((uint8)(count - 1) >= 24)
        return -1;
    native_vm.events.voice_count = count;
    return count;
}

sint8 SsBlockVoiceAllocation(void)
{
    if (spu_sequence_manager()->locked == 1)
        return -1;
    spu_sequence_manager()->locked = 1;
    return 1;
}

sint8 SsUnBlockVoiceAllocation(void)
{
    if (!spu_sequence_manager()->locked)
        return -1;
    spu_sequence_manager()->locked = 0;
    return 1;
}
#else
sint32 SsAllocateVoices(uint8 count, uint8 priority)
{
    (void)count;
    (void)priority;
    abort();
}

void SsSetVoiceSettings(sint32 voice, SndVoiceStats *settings)
{
    (void)voice;
    (void)settings;
    abort();
}

sint8 SsSetReservedVoice(sint8 count)
{
    (void)count;
    abort();
}

sint8 SsBlockVoiceAllocation(void)
{
    abort();
}

sint8 SsUnBlockVoiceAllocation(void)
{
    abort();
}
#endif

void SsSetSerialAttr(sint8 serial, sint8 attribute, sint8 value)
{
    sint32 channel = (sint8)serial, selector = (sint8)attribute;
    SpuCommonAttr attr;
    memset(&attr, 0, sizeof(attr));
    if ((uint32)channel > 1 || (uint32)selector > 1)
        abort();
    if (channel == 0)
    {
        attr.mask = selector == 0 ? 0x200 : 0x100;
        if (selector == 0)
            attr.cd.mix = (sint8)value;
        else
            attr.cd.reverb = (sint8)value;
    }
    else
    {
        attr.mask = selector == 0 ? 0x2000 : 0x1000;
        if (selector == 0)
            attr.ext.mix = (sint8)value;
        else
            attr.ext.reverb = (sint8)value;
    }
    SpuSetCommonAttr(&attr);
}

sint16 SsGetActualProgFromProg(sint16 bank, sint16 program)
{
    sint32 signed_program = (sint16)program;
    SpuVabNativeView view;
    if (signed_program < 0 || !spu_vab_native_view((sint16)bank, &view) || (uint32)signed_program >= view.program_bytes / 16)
        return -1;
    return (sint16)xport_load_le16(view.programs + signed_program * 16 + 8);
}

#if defined(XPORT_NATIVE_VM) && XPORT_NATIVE_VM
sint32 SsUtGetVagAddr(sint16 bank, sint16 vag)
{
    sint32 index;
    const uint8 *table;
    uint32 offset;
    if (_SsVmVSetUp((uint16)bank, 0) == -1)
        return -1;
    index = ((sint32)(sint16)vag - 1) / 2;
    table = spu_vm_native_bank().programs;
    offset = (vag & 1) ? 12 : 14;
    return (sint32)((uint32)xport_load_le16(table + ((uint32)index << 4) + offset) << 3);
}
#else
sint32 SsUtGetVagAddr(sint16 bank, sint16 vag)
{
    (void)bank;
    (void)vag;
    abort();
}
#endif

sint16 SsUtSetReverbType(sint16 type)
{
    uint32 magnitude = type < 0 ? 0 - (uint32)(sint32)type : (uint32)type;
    SpuReverbAttr attribute = {0};
    if (magnitude >= 10)
        return -1;
    attribute.mask = 1;
    attribute.mode = magnitude | (type < 0 ? 0x100 : 0);
    if (!magnitude)
        SpuSetReverb(0);
    SpuSetReverbModeParam(&attribute);
    return magnitude;
}

void SsUtSetReverbDepth(sint16 left, sint16 right)
{
    SpuReverbAttr attribute = {0};
    attribute.mask = 6;
    attribute.depth.left = 32767 * left / 127;
    attribute.depth.right = 32767 * right / 127;
    SpuSetReverbDepth(&attribute);
}

void SsSetRVol(sint16 left, sint16 right)
{
    SsUtSetReverbDepth(left, right);
}

void SpuSetReverbDepth(SpuReverbAttr *attr)
{
    if (!attr)
        abort();
    xport_audio_lock();
    if (!attr->mask || (attr->mask & 2u))
        reverb_control.depth_left = attr->depth.left;
    if (!attr->mask || (attr->mask & 4u))
        reverb_control.depth_right = attr->depth.right;
    reverb_incomplete = 0;
    xport_audio_unlock();
}

sint32 SpuSetReverbModeParam(SpuReverbAttr *attr)
{
    uint8 block[68];
    uint32 mask, register_mask, mode, i, clear, change, delay_change, feedback_change;
    if (!attr)
        return -1;
    mask = attr->mask;
    clear = change = delay_change = feedback_change = 0u;
    memset(block, 0, sizeof(block));
    xport_audio_lock();
    if (!mask || (mask & 1u))
    {
        mode = (uint32)attr->mode;
        clear = (mode & 0x100u) != 0u;
        mode &= ~0x100u;
        if (mode >= 10u || spu_reverb_work_occupied(reverb_preset_work[mode]))
        {
            xport_audio_unlock();
            return -1;
        }
        change = 1u;
        reverb_control.mode = (sint32)mode;
        reverb_control.work_address = reverb_preset_work[mode];
        spu_reverb_reset_cursor();
        if (reverb_control.reserved && heap.entries)
            heap.reserved_bytes = (0x10000u - reverb_control.work_address) << heap.address_shift;
        memcpy(block, reverb_presets + 68u * mode, sizeof(block));
        reverb_control.delay = mode == 7u || mode == 8u ? 127 : 0;
        reverb_control.feedback = mode == 7u ? 127 : 0;
    }
    mode = (uint32)reverb_control.mode;
    if ((!mask || (mask & 8u)) && mode >= 7u && mode < 9u)
    {
        sint32 d13 = (sint32)((uint32)attr->delay << 13) / 127;
        sint32 d12 = (sint32)((uint32)attr->delay << 12) / 127;
        uint16 values[32];
        delay_change = 1u;
        if (!change)
        {
            memcpy(block, reverb_presets + 68u * mode, sizeof(block));
            block[0] = 0u;
            block[1] = 0x1Cu;
            block[2] = 1u;
            block[3] = 0x0Cu;
        }
        for (i = 0u; i < 32u; ++i)
            values[i] = read_u16_le(block + 4u + 2u * i);
        reverb_control.delay = attr->delay;
        values[10] = (uint16)(d13 - values[0]);
        values[11] = (uint16)(d12 - values[1]);
        values[16] = (uint16)(values[17] + d12);
        values[12] = (uint16)(values[13] + d12);
        values[26] = (uint16)(values[28] + d12);
        values[27] = (uint16)(values[29] + d12);
        for (i = 0u; i < 32u; ++i)
        {
            block[4u + 2u * i] = (uint8)values[i];
            block[5u + 2u * i] = (uint8)(values[i] >> 8);
        }
    }
    if ((!mask || (mask & 16u)) && mode >= 7u && mode < 9u)
    {
        uint16 feedback = (uint16)((sint32)((uint32)attr->feedback * 33024u) / 127);
        feedback_change = 1u;
        if (!change && !delay_change)
        {
            memcpy(block, reverb_presets + 68u * mode, sizeof(block));
            memset(block, 0, 4u);
        }
        if (!change)
            block[0] |= 0x80u;
        reverb_control.feedback = attr->feedback;
        block[18] = (uint8)feedback;
        block[19] = (uint8)(feedback >> 8);
    }
    if (change)
        reverb_control.depth_left = reverb_control.depth_right = 0;
    else
    {
        if (!mask || (mask & 2u))
            reverb_control.depth_left = attr->depth.left;
        if (!mask || (mask & 4u))
            reverb_control.depth_right = attr->depth.right;
    }
    if (change || delay_change || feedback_change)
    {
        register_mask = read_u32_le(block);
        for (i = 0u; i < 32u; ++i)
            if (!register_mask || (register_mask & (1u << i)))
                reverb_control.parameters[i] = read_u16_le(block + 4u + 2u * i);
    }
    if (clear)
    {
        uint32 enabled = reverb_control.control & 0x80u;
        sint32 result;
        reverb_control.control &= ~0x80u;
        reverb_control.enabled = 0;
        xport_audio_unlock();
        result = SpuClearReverbWorkArea((sint32)mode);
        xport_audio_lock();
        reverb_control.control |= enabled;
        reverb_control.enabled = (reverb_control.control >> 7) & 1;
        xport_audio_unlock();
        return result;
    }
    xport_audio_unlock();
    return 0;
}

void spu_bind_key_status_guest(uint32 state_address, uint32 registers_pointer_address)
{
    key_status_state_address = state_address;
    key_status_registers_pointer_address = registers_pointer_address;
}

void SpuGetAllKeysStatus(uint8 *status)
{
    uint32 voice, base, keys, envelope, result;
    if (!status)
        return;
    if (key_status_state_address && key_status_registers_pointer_address)
    {
        for (voice = 0; voice < SPU_VOICE_COUNT; ++voice)
        {
            base = r_u32(key_status_registers_pointer_address);
            keys = r_u32(key_status_state_address);
            envelope = r_u16(base + voice * 16 + 12);
            result = keys & (1 << voice) ? (envelope ? SPU_ON : SPU_ON_ENV_OFF) : (envelope ? SPU_OFF_ENV_ON : SPU_OFF);
            xport_store_u8(status + voice, result);
        }
        return;
    }
    xport_audio_lock();
    spu_synchronize_device_clock();
    for (voice = 0; voice < SPU_VOICE_COUNT; ++voice)
    {
        keys = spu.key_status;
        envelope = spu.voices[voice].envelope;
        result = keys & (1 << voice) ? (envelope || (spu.key_on_pending & (1 << voice)) ? SPU_ON : SPU_ON_ENV_OFF) : (envelope ? SPU_OFF_ENV_ON : SPU_OFF);
        xport_store_u8(status + voice, result);
    }
    xport_audio_unlock();
}

sint32 SpuGetKeyStatus(uint32 voice_bit)
{
    sint32 voice;
    sint32 status;

    for (voice = 0; voice < SPU_VOICE_COUNT; ++voice)
        if (voice_bit & SPU_KEYCH(voice))
            break;
    if (voice == SPU_VOICE_COUNT)
        return -1;
    xport_audio_lock();
    spu_synchronize_device_clock();
    if (spu.key_status & SPU_KEYCH(voice))
    {
        // Pending starts are active before the mixer updates their envelope
        status = (spu.key_on_pending & SPU_KEYCH(voice)) || spu.voices[voice].envelope != 0 ? SPU_ON : SPU_ON_ENV_OFF;
    }
    else
        status = spu.voices[voice].envelope != 0 ? SPU_OFF_ENV_ON : SPU_OFF;
    xport_audio_unlock();
    return status;
}

// Native common attributes target the mixer through the SPU device boundary
static uint32 spu_common_base(void)
{
    return 0x1F801C00u;
}

static uint16 spu_common_read(uint32 address)
{
    uint16 value;
    sint32 valid;
    xport_audio_lock();
    valid = spu_mmio_read16(address, &value);
    xport_audio_unlock();
    if (!valid)
        abort();
    return value;
}

static void spu_common_write(uint32 address, uint16 value)
{
    if (!spu_mmio_write(NULL, address, 2u, value, 3u))
        abort();
}

void SpuSetCommonAttr(SpuCommonAttr *attr)
{
    const volatile SpuCommonAttr *source;
    uint32 mask;
    uint32 mode;
    uint32 base;
    uint32 value;
    sint32 signed_volume;
    source = attr;
    mask = source->mask;
    if (mask == 0u || (mask & 1u) != 0u)
    {
        mode = 0u;
        if (mask == 0u || (mask & 4u) != 0u)
        {
            switch (source->mvolmode.left)
            {
                case 1:
                    mode = 0x8000u;
                    break;
                case 2:
                    mode = 0x9000u;
                    break;
                case 3:
                    mode = 0xA000u;
                    break;
                case 4:
                    mode = 0xB000u;
                    break;
                case 5:
                    mode = 0xC000u;
                    break;
                case 6:
                    mode = 0xD000u;
                    break;
                case 7:
                    mode = 0xE000u;
                    break;
                default:
                    break;
            }
        }
        if (mode == 0u)
            value = (uint16)source->mvol.left & 0x7FFFu;
        else
        {
            signed_volume = source->mvol.left;
            value = (uint16)source->mvol.left;
            if (signed_volume >= 128)
                value = 127u;
            else if (signed_volume < 0)
                value = 0u;
            value &= 0x7FFFu;
        }
        base = spu_common_base();
        spu_common_write(base + 0x180u, (uint16)(value | mode));
    }
    if (mask == 0u || (mask & 2u) != 0u)
    {
        mode = 0u;
        if (mask == 0u || (mask & 8u) != 0u)
        {
            switch (source->mvolmode.right)
            {
                case 1:
                    mode = 0x8000u;
                    break;
                case 2:
                    mode = 0x9000u;
                    break;
                case 3:
                    mode = 0xA000u;
                    break;
                case 4:
                    mode = 0xB000u;
                    break;
                case 5:
                    mode = 0xC000u;
                    break;
                case 6:
                    mode = 0xD000u;
                    break;
                case 7:
                    mode = 0xE000u;
                    break;
                default:
                    break;
            }
        }
        if (mode == 0u)
            value = (uint16)source->mvol.right & 0x7FFFu;
        else
        {
            signed_volume = source->mvol.right;
            value = (uint16)source->mvol.right;
            if (signed_volume >= 128)
                value = 127u;
            else if (signed_volume < 0)
                value = 0u;
            value &= 0x7FFFu;
        }
        base = spu_common_base();
        spu_common_write(base + 0x182u, (uint16)(value | mode));
    }
    if (mask == 0u || (mask & 0x40u) != 0u)
    {
        base = spu_common_base();
        value = (uint16)source->cd.volume.left;
        spu_common_write(base + 0x1B0u, (uint16)value);
    }
    if (mask == 0u || (mask & 0x80u) != 0u)
    {
        base = spu_common_base();
        value = (uint16)source->cd.volume.right;
        spu_common_write(base + 0x1B2u, (uint16)value);
    }
    if (mask == 0u || (mask & 0x400u) != 0u)
    {
        base = spu_common_base();
        value = (uint16)source->ext.volume.left;
        spu_common_write(base + 0x1B4u, (uint16)value);
    }
    if (mask == 0u || (mask & 0x800u) != 0u)
    {
        base = spu_common_base();
        value = (uint16)source->ext.volume.right;
        spu_common_write(base + 0x1B6u, (uint16)value);
    }
    if (mask == 0u || (mask & 0x100u) != 0u)
    {
        if (source->cd.reverb != 0)
        {
            base = spu_common_base();
            value = spu_common_read(base + 0x1AAu) | 4u;
        }
        else
        {
            base = spu_common_base();
            value = spu_common_read(base + 0x1AAu) & 0xFFFBu;
        }
        spu_common_write(base + 0x1AAu, (uint16)value);
    }
    if (mask == 0u || (mask & 0x200u) != 0u)
    {
        if (source->cd.mix != 0)
        {
            base = spu_common_base();
            value = spu_common_read(base + 0x1AAu) | 1u;
        }
        else
        {
            base = spu_common_base();
            value = spu_common_read(base + 0x1AAu) & 0xFFFEu;
        }
        spu_common_write(base + 0x1AAu, (uint16)value);
    }
    if (mask == 0u || (mask & 0x1000u) != 0u)
    {
        if (source->ext.reverb != 0)
        {
            base = spu_common_base();
            value = spu_common_read(base + 0x1AAu) | 8u;
        }
        else
        {
            base = spu_common_base();
            value = spu_common_read(base + 0x1AAu) & 0xFFF7u;
        }
        spu_common_write(base + 0x1AAu, (uint16)value);
    }
    if (mask == 0u || (mask & 0x2000u) != 0u)
    {
        if (source->ext.mix != 0)
        {
            base = spu_common_base();
            value = spu_common_read(base + 0x1AAu) | 2u;
        }
        else
        {
            base = spu_common_base();
            value = spu_common_read(base + 0x1AAu) & 0xFFFDu;
        }
        spu_common_write(base + 0x1AAu, (uint16)value);
    }
    return;
}

void SpuGetCommonAttr(SpuCommonAttr *attr)
{
    uint32 mask;

    if (!attr)
        return;
    xport_audio_lock();
    mask = attr->mask;
    memset(attr, 0, sizeof(*attr));
    attr->mask = mask;
    attr->mvol.left = spu.master_left;
    attr->mvol.right = spu.master_right;
    attr->mvolx = attr->mvol;
    attr->cd.volume.left = spu.cd_left;
    attr->cd.volume.right = spu.cd_right;
    attr->cd.mix = (reverb_control.control & 1u) != 0u;
    attr->cd.reverb = (reverb_control.control & 4u) != 0u;
    attr->ext.volume.left = spu.ext_left;
    attr->ext.volume.right = spu.ext_right;
    xport_audio_unlock();
}

void SsGetSerialVol(uint8 serial, uint16 *volume)
{
    SpuCommonAttr attribute = {0};
    SpuVolume selected;
    SpuGetCommonAttr(&attribute);
    if (serial == 0)
        selected = attribute.cd.volume;
    else if (serial == 1)
        selected = attribute.ext.volume;
    else
        return;
    volume[0] = (uint16)(selected.left / 258);
    volume[1] = (uint16)(selected.right / 258);
}

void SpuSetVoiceVolume(sint32 voice, sint16 left, sint16 right)
{
    xport_audio_lock();
    spu_set_voice_volume(voice, left, right);
    xport_audio_unlock();
}

void SpuSetVoicePitch(sint32 voice, uint16 pitch)
{
    xport_audio_lock();
    spu_set_voice_pitch(voice, pitch);
    xport_audio_unlock();
}

void SpuSetVoiceStartAddr(sint32 voice, uint32 start_address)
{
    xport_audio_lock();
    if (spu.initialized && voice >= 0 && voice < SPU_VOICE_COUNT)
        spu.voices[voice].registers.start_address = (uint16)(start_address >> 3);
    xport_audio_unlock();
}

void SpuSetVoiceLoopStartAddr(sint32 voice, uint32 loop_start_address)
{
    xport_audio_lock();
    if (spu.initialized && voice >= 0 && voice < SPU_VOICE_COUNT)
    {
        spu.voices[voice].registers.repeat_address = (uint16)(loop_start_address >> 3);
        spu.voices[voice].repeat_address = loop_start_address;
    }
    xport_audio_unlock();
}

static void envelope_reset(SPU_VOICE *voice, uint8 rate, uint8 rate_mask, sint32 decreasing, sint32 exponential)
{
    sint32 base_step = 7 - (rate & 3);
    voice->envelope_rate = rate;
    voice->envelope_decreasing = (uint8)decreasing;
    voice->envelope_exponential = (uint8)exponential;
    voice->envelope_counter = 0;
    voice->envelope_increment = 0x8000;
    voice->envelope_step = (sint16)(decreasing ? ~base_step : base_step);
    if (rate < 44)
    {
        voice->envelope_step = (sint16)(voice->envelope_step << (11 - (rate >> 2)));
    }
    else if (rate >= 48)
    {
        voice->envelope_increment = (uint16)(voice->envelope_increment >> ((rate >> 2) - 11));
        if ((rate & rate_mask) == rate_mask)
            voice->envelope_increment = 0;
        else if (voice->envelope_increment == 0)
            voice->envelope_increment = 1;
    }
}

static void update_adsr(SPU_VOICE *voice)
{
    uint16 adsr1 = voice->registers.adsr1;
    uint16 adsr2 = voice->registers.adsr2;
    switch (voice->adsr_phase)
    {
        case SPU_ADSR_ATTACK:
            envelope_reset(voice, (uint8)((adsr1 >> 8) & 0x7f), 0x7f, 0, (adsr1 & 0x8000) != 0);
            break;
        case SPU_ADSR_DECAY:
            envelope_reset(voice, (uint8)(((adsr1 >> 4) & 0x0f) << 2), 0x7c, 1, 1);
            break;
        case SPU_ADSR_SUSTAIN:
            envelope_reset(voice, (uint8)((adsr2 >> 6) & 0x7f), 0x7f, (adsr2 & 0x4000) != 0, (adsr2 & 0x8000) != 0);
            break;
        case SPU_ADSR_RELEASE:
            envelope_reset(voice, (uint8)((adsr2 & 0x1f) << 2), 0x7c, 1, (adsr2 & 0x20) != 0);
            break;
        default:
            voice->envelope_counter = 0;
            voice->envelope_increment = 0;
            voice->envelope_step = 0;
            break;
    }
}

static void tick_adsr(SPU_VOICE *voice)
{
    sint32 step;
    sint32 level;
    uint32 increment;
    sint32 target;
    if (voice->adsr_phase == SPU_ADSR_OFF || voice->envelope_increment == 0)
        return;
    step = voice->envelope_step;
    level = voice->envelope;
    increment = voice->envelope_increment;
    if (voice->envelope_exponential)
    {
        if (voice->envelope_decreasing)
        {
            step = (step * level) >> 15;
        }
        else if (level >= 0x6000)
        {
            if (voice->envelope_rate < 40)
                step >>= 2;
            else if (voice->envelope_rate >= 44)
                increment >>= 2;
            else
            {
                step >>= 1;
                increment >>= 1;
            }
        }
    }
    voice->envelope_counter += increment;
    if (!(voice->envelope_counter & 0x8000))
        return;
    voice->envelope_counter = 0;
    level += step;
    if (level < 0)
        level = 0;
    if (level > 32767)
        level = 32767;
    voice->envelope = (uint16)level;

    if (voice->adsr_phase == SPU_ADSR_ATTACK)
        target = 32767;
    else if (voice->adsr_phase == SPU_ADSR_DECAY)
    {
        target = ((voice->registers.adsr1 & 15) + 1) * 0x800;
        if (target > 32767)
            target = 32767;
    }
    else if (voice->adsr_phase == SPU_ADSR_RELEASE)
        target = 0;
    else
        return;

    if ((!voice->envelope_decreasing && level >= target) || (voice->envelope_decreasing && level <= target))
    {
        if (voice->adsr_phase == SPU_ADSR_ATTACK)
            voice->adsr_phase = SPU_ADSR_DECAY;
        else if (voice->adsr_phase == SPU_ADSR_DECAY)
            voice->adsr_phase = SPU_ADSR_SUSTAIN;
        else
        {
            voice->adsr_phase = SPU_ADSR_OFF;
            voice->envelope = 0;
        }
        update_adsr(voice);
    }
}

static void decode_voice_block(SPU_VOICE *voice)
{
    uint8 block[16];
    uint32 index;
    spu_irq_check_range(voice->current_address, 16);
    for (index = 0; index < 16; ++index)
        block[index] = spu.ram[(voice->current_address + index) & (SPU_RAM_SIZE - 1)];
    voice->decoded[0] = voice->decoded[28];
    voice->decoded[1] = voice->decoded[29];
    voice->decoded[2] = voice->decoded[30];
    spu_decode_adpcm_block(block, &voice->history1, &voice->history2, voice->decoded + 3);
    voice->block_flags = block[1];
    if ((block[1] & 4) && !voice->ignore_loop_address)
    {
        voice->registers.repeat_address = (uint16)(voice->current_address >> 3);
        voice->repeat_address = voice->current_address;
    }
    voice->decoded_valid = 1;
}


static sint32 interpolate_voice(const SPU_VOICE *voice)
{
    uint32 fraction = (voice->phase >> 4) & 0xff;
    uint32 sample = 3 + (voice->phase >> 12);
    sint32 result = spu_gaussian[0x0ff - fraction] * voice->decoded[sample - 3];
    result += spu_gaussian[0x1ff - fraction] * voice->decoded[sample - 2];
    result += spu_gaussian[0x100 + fraction] * voice->decoded[sample - 1];
    result += spu_gaussian[0x000 + fraction] * voice->decoded[sample];
    return result >> 15;
}

static sint32 fixed_volume(uint16 bits)
{
    if (bits & 0x8000)
        return 0; /* Volume sweeps are outside AA's proven use. */
    return (sint16)(bits << 1);
}

static sint32 apply_volume(sint32 sample, sint32 volume)
{
    return (sample * volume) >> 15;
}

static void spu_key_on(uint32 voice_mask)
{
    spu_synchronize_device_clock();
    if (!spu.initialized)
        return;
    spu.key_status |= voice_mask;
    spu.key_on_pending |= voice_mask;
}

static void spu_key_off(uint32 voice_mask)
{
    spu_synchronize_device_clock();
    if (!spu.initialized)
        return;
    spu.key_status &= ~voice_mask;
    spu.key_off_pending |= voice_mask;
}

uint32 spu_end_flags(void)
{
    return spu.end_flags;
}

SPU_ADSR_PHASE spu_voice_phase(sint32 voice)
{
    if (voice < 0 || voice >= SPU_VOICE_COUNT)
        return SPU_ADSR_OFF;
    return spu.voices[voice].adsr_phase;
}

uint16 spu_voice_envelope(sint32 voice)
{
    if (voice < 0 || voice >= SPU_VOICE_COUNT)
        return 0;
    return spu.voices[voice].envelope;
}

static const sint32 ima_index_table[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

static const sint32 ima_step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

static sint16 ima_decode_nibble(IMA_ADPCM_STREAM *stream, sint32 channel, uint8 nibble)
{
    sint32 step = ima_step_table[stream->step_index[channel]];
    sint32 difference = step >> 3;
    if (nibble & 1)
        difference += step >> 2;
    if (nibble & 2)
        difference += step >> 1;
    if (nibble & 4)
        difference += step;
    if (nibble & 8)
        stream->predictor[channel] -= difference;
    else
        stream->predictor[channel] += difference;
    stream->predictor[channel] = audio_sample_clamp(stream->predictor[channel]);
    stream->step_index[channel] += ima_index_table[nibble & 7];
    if (stream->step_index[channel] < 0)
        stream->step_index[channel] = 0;
    if (stream->step_index[channel] > 88)
        stream->step_index[channel] = 88;
    return (sint16)stream->predictor[channel];
}

#ifdef AP_WEB
EM_JS(uint32, web_music_size, (const char *key), { return Module.xportManifest.music[UTF8ToString(key)] ?.size || 0; });

EM_JS(sint32, web_music_open, (const char *key, uint8 *data), {
    const state = Module.xportMusicOpen(UTF8ToString(key));
    if (!state)
        return 0;
    Module.xportMusic.set(data, state);
    return 1;
});

EM_JS(sint32, web_music_fill, (const uint8 *data), {
    const state = Module.xportMusic.get(data);
    if (!state)
        return -1;
    const copied = state.copied || 0;
    HEAPU8.set(state.data.subarray(copied, state.received), data + copied);
    state.copied = state.received;
    return state.failed ? -1 : state.received;
});

EM_JS(void, web_music_close, (const uint8 *data), { Module.xportMusic.delete(data); });
#endif

static sint32 cd_file_available(const uint8 *data, uint32 needed)
{
#ifdef AP_WEB
    if (!libsnd.cd.file)
        return web_music_fill(data);
#endif
    CD_AUDIO_STATE *cd = &libsnd.cd;
    if (cd->data != data || cd->file == 0)
        return 0x7FFFFFFF;
    if (cd->file_received < needed)
    {
        uint32 remaining = cd->stream.file_size - cd->file_received;
        uint32 count = remaining < 128 * 1024 ? remaining : 128 * 1024;
        if (xport_fread(cd->data + cd->file_received, 1, count, cd->file) != count)
            return -1;
        cd->file_received += count;
    }
    return (sint32)cd->file_received;
}

static sint32 ima_load_block(IMA_ADPCM_STREAM *stream)
{
    const uint8 *block;
    uint32 header_size = stream->channels * 4u;
    sint32 channel;
    sint32 available;
    uint32 needed = (uint32)(stream->audio_data - stream->file_data) + stream->block_offset;
    uint32 remaining = stream->audio_size - stream->block_offset;
    needed += remaining < stream->block_align ? remaining : stream->block_align;
    available = cd_file_available(stream->file_data, needed);
    if (available < 0)
    {
        stream->total_frames = stream->decoded_frames;
        return 0;
    }
    if ((uint32)available < needed)
        return 0;
    if (stream->block_offset >= stream->audio_size || stream->audio_size - stream->block_offset < header_size)
        return 0;
    block = stream->audio_data + stream->block_offset;
    for (channel = 0; channel < stream->channels; ++channel)
    {
        stream->predictor[channel] = (sint16)read_u16_le(block + channel * 4);
        stream->step_index[channel] = block[channel * 4 + 2];
        if (stream->step_index[channel] > 88)
            return 0;
    }
    stream->sample_in_block = 0;
    stream->block_loaded = 1;
    return 1;
}

static sint32 ima_open(IMA_ADPCM_STREAM *stream, const uint8 *data, uint32 size)
{
    uint32 offset = 12;
    uint32 fact_frames = 0;
    sint32 have_format = 0;
    memset(stream, 0, sizeof(*stream));
    if (data == 0 || size < 12 || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0)
        return 0;
    while (offset + 8 <= size)
    {
        const uint8 *chunk = data + offset;
        uint32 chunk_size = read_u32_le(chunk + 4);
        uint32 payload = offset + 8;
        if (chunk_size > size - payload)
            return 0;
        if (memcmp(chunk, "fmt ", 4) == 0)
        {
            if (chunk_size < 20 || read_u16_le(data + payload) != 0x11)
                return 0;
            stream->channels = read_u16_le(data + payload + 2);
            stream->sample_rate = read_u32_le(data + payload + 4);
            stream->block_align = read_u16_le(data + payload + 12);
            if (read_u16_le(data + payload + 14) != 4 || read_u16_le(data + payload + 16) < 2)
                return 0;
            stream->samples_per_block = read_u16_le(data + payload + 18);
            have_format = 1;
        }
        else if (memcmp(chunk, "fact", 4) == 0 && chunk_size >= 4)
            fact_frames = read_u32_le(data + payload);
        else if (memcmp(chunk, "data", 4) == 0)
        {
            stream->audio_data = data + payload;
            stream->audio_size = chunk_size;
        }
        offset = payload + chunk_size + (chunk_size & 1u);
    }
    if (!have_format || stream->audio_data == 0 || stream->channels == 0 || stream->channels > 2 || stream->sample_rate != SPU_SAMPLE_RATE || stream->block_align < stream->channels * 4u || stream->samples_per_block == 0)
        return 0;
    stream->file_data = data;
    stream->file_size = size;
    stream->total_frames = fact_frames;
    if (stream->total_frames == 0)
        stream->total_frames = (stream->audio_size / stream->block_align) * stream->samples_per_block;
    return 1;
}

static uint32 ima_decode(IMA_ADPCM_STREAM *stream, sint16 *stereo, uint32 frames)
{
    uint32 written = 0;
    while (written < frames && stream->decoded_frames < stream->total_frames)
    {
        const uint8 *block;
        uint32 encoded_sample;
        uint32 group;
        uint32 within;
        sint16 sample[2];
        sint32 channel;
        if (!stream->block_loaded && !ima_load_block(stream))
            break;
        block = stream->audio_data + stream->block_offset;
        if (stream->sample_in_block == 0)
        {
            sample[0] = (sint16)stream->predictor[0];
            sample[1] = stream->channels == 2 ? (sint16)stream->predictor[1] : sample[0];
        }
        else
        {
            encoded_sample = stream->sample_in_block - 1;
            group = encoded_sample / 8u;
            within = encoded_sample & 7u;
            for (channel = 0; channel < stream->channels; ++channel)
            {
                uint32 byte_offset = stream->channels * 4u + group * stream->channels * 4u + channel * 4u + within / 2u;
                uint8 byte;
                uint8 nibble;
                if (byte_offset >= stream->block_align || stream->block_offset + byte_offset >= stream->audio_size)
                    return written;
                byte = block[byte_offset];
                nibble = (within & 1u) ? byte >> 4 : byte & 15;
                sample[channel] = ima_decode_nibble(stream, channel, nibble);
            }
            if (stream->channels == 1)
                sample[1] = sample[0];
        }
        stereo[written * 2] = sample[0];
        stereo[written * 2 + 1] = sample[1];
        ++written;
        ++stream->decoded_frames;
        ++stream->sample_in_block;
        if (stream->sample_in_block >= stream->samples_per_block)
        {
            uint32 remaining = stream->audio_size - stream->block_offset;
            stream->block_offset += remaining < stream->block_align ? remaining : stream->block_align;
            stream->block_loaded = 0;
        }
    }
    return written;
}

static uint8 *cd_track_load(const char *path, uint32 *size, FILE **opened_file)
{
    *opened_file = 0;
#ifdef AP_WEB
    if (strncmp(path, "MUSIC/", 6) == 0)
    {
        char key[256];
        size_t path_length = strlen(path);
        const char *extension = strrchr(path, '.');
        size_t key_length;
        uint32 length;
        if (path_length < 10 || strncmp(path, "MUSIC/", 6) != 0 || extension == 0 || (strcmp(extension, ".WAV") != 0 && strcmp(extension, ".XA") != 0))
            return 0;
        key_length = (size_t)(extension - (path + 6));
        if (key_length == 0 || key_length >= sizeof(key))
            return 0;
        memcpy(key, path + 6, key_length);
        key[key_length] = 0;
        length = web_music_size(key);
        uint8 *data = length ? (uint8 *)calloc(1, length) : 0;
        if (!data || !web_music_open(key, data) || web_music_fill(data) < 0)
        {
            web_music_close(data);
            free(data);
            return 0;
        }
        *size = (uint32)length;
        return (uint8 *)data;
    }
#endif
    FILE *file;
    long length;
    uint8 *data;
    file = xport_fopen(path, "rb");
    if (file == 0 || fseek(file, 0, SEEK_END) != 0)
    {
        if (file != 0)
            fclose(file);
        return 0;
    }
    length = ftell(file);
    if (length <= 0 || (uint64)length > 0xffffffffu || fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        return 0;
    }
    data = (uint8 *)calloc(1, (size_t)length);
    if (data == 0
    )
    {
        free(data);
        fclose(file);
        return 0;
    }
    *opened_file = file;
    *size = (uint32)length;
    return data;
}

static void cd_audio_stop_locked(void)
{
#ifdef AP_WEB
    web_music_close(libsnd.cd.data);
#endif
    free(libsnd.cd.data);
    if (libsnd.cd.file != 0)
        fclose(libsnd.cd.file);
    libsnd.cd.file = 0;
    libsnd.cd.file_received = 0;
    libsnd.cd.data = 0;
    memset(&libsnd.cd.stream, 0, sizeof(libsnd.cd.stream));
    libsnd.cd.track = -1;
    libsnd.cd_end_callback = NULL;
    libsnd.cd.active = 0;
    libsnd.cd.paused = 0;
    libsnd.cd.file_xa = 0;
    libsnd.cd.file_pending = 0;
    libsnd.cd.file_xa_frames = 0;
    libsnd.cd.file_xa_position = 0;
    libsnd.cd.file_xa_start_sector = 0;
    xa_reset(&libsnd.cd.file_xa_state);
}

static sint32 cd_file_stream_open(IMA_ADPCM_STREAM *stream, const uint8 *data, uint32 size, sint32 xa)
{
    uint8 coding;
    uint32 samples, index;
    uint64 total;
    if (!xa)
        return ima_open(stream, data, size);
    if (data == 0 || size < 2352 || size % 2352 != 0 || data[0] != 0 || data[11] != 0 || data[15] != 2 || !(data[18] & 4) || memcmp(data + 16, data + 20, 4) != 0)
        return 0;
    for (index = 1; index < 11; ++index)
        if (data[index] != 255)
            return 0;
    coding = data[19];
    if (coding & 0xEA)
        return 0;
    samples = ((coding & 16) ? 4 : 8) * 28 * 18 / ((coding & 1) ? 2 : 1);
    samples = samples / ((coding & 4) ? 3 : 6) * 7;
    total = (uint64)(size / 2352) * samples;
    if (total > 0xFFFFFFFF)
        return 0;
    memset(stream, 0, sizeof(*stream));
    stream->file_data = data;
    stream->audio_data = data;
    stream->file_size = size;
    stream->audio_size = size;
    stream->total_frames = (uint32)total;
    stream->channels = 2;
    stream->sample_rate = 44100;
    stream->block_align = 2352;
    stream->samples_per_block = (uint16)samples;
    return 1;
}

static uint32 cd_file_decode(CD_AUDIO_STATE *cd, sint16 *output, uint32 count)
{
    IMA_ADPCM_STREAM *stream = &cd->stream;
    uint32 written = 0;
    if (cd->file_pending)
    {
        uint32 needed = stream->file_size < 128 * 1024 ? stream->file_size : 128 * 1024;
        sint32 available = cd_file_available(cd->data, needed);
        if (available >= 0 && (uint32)available < needed)
            return 0;
        cd->file_pending = 0;
        if (available < 0 || !cd_file_stream_open(stream, cd->data, stream->file_size, cd->file_xa))
        {
#if defined(_DEBUG) || defined(AP_WEB)
            fprintf(stderr, "Music stream could not start\n");
#endif
            stream->total_frames = stream->decoded_frames = 0;
            cd->repeat = 0;
            return 0;
        }
    }
    if (!cd->file_xa)
        return ima_decode(stream, output, count);
    while (written < count && stream->decoded_frames < stream->total_frames)
    {
        if (cd->file_xa_position == cd->file_xa_frames)
        {
            const uint8 *raw;
            sint32 available = cd_file_available(stream->file_data, stream->block_offset + 2352);
            if (available < 0)
            {
                stream->total_frames = stream->decoded_frames;
                break;
            }
            if ((uint32)available < stream->block_offset + 2352)
                break;
            if (stream->block_offset >= stream->file_size)
                break;
            raw = stream->file_data + stream->block_offset;
            if (cd->file_xa_start_sector)
            {
                CdlLOC position;
                memset(&position, 0, sizeof(position));
                memcpy(&position, raw + 12, 3);
                if (CdPosToInt(&position) < (sint32)cd->file_xa_start_sector)
                {
                    stream->block_offset += 2352;
                    stream->decoded_frames += stream->samples_per_block;
                    continue;
                }
                cd->file_xa_start_sector = 0;
            }
            if (raw[16] != stream->file_data[16] || raw[17] != stream->file_data[17] || raw[19] != stream->file_data[19] || xa_decode_sector(&cd->file_xa_state, raw, cd->file_xa_samples, XA_MAX_SECTOR_FRAMES, &cd->file_xa_frames) != XA_OK || cd->file_xa_frames != stream->samples_per_block)
            {
                stream->total_frames = stream->decoded_frames;
                break;
            }
            stream->block_offset += 2352;
            cd->file_xa_position = 0;
        }
        {
            uint32 frames = cd->file_xa_frames - cd->file_xa_position;
            if (frames > count - written)
                frames = count - written;
            memcpy(output + written * 2, cd->file_xa_samples + cd->file_xa_position * 2, frames * sizeof(sint16) * 2);
            cd->file_xa_position += frames;
            stream->decoded_frames += frames;
            written += frames;
        }
    }
    return written;
}

static sint32 cd_play_file_at(const char *path, sint32 mode, sint32 selected, sint32 xa, sint32 sector)
{
    IMA_ADPCM_STREAM stream;
    uint8 *data;
    uint32 size;
    FILE *file;
    CdlCB callback;
    if (path == 0 || mode < 0 || sector < 0)
        return 0;
    data = cd_track_load(path, &size, &file);
    if (data == 0
    )
    {
#ifdef AP_WEB
        web_music_close(data);
#endif
        free(data);
        if (file != 0)
            fclose(file);
        return 0;
    }
    /* Pending music must not suspend the game while the initial buffer arrives */
    memset(&stream, 0, sizeof(stream));
    stream.file_data = data;
    stream.file_size = size;
    stream.total_frames = 0xFFFFFFFF;
    xport_audio_lock();
    cd_audio_stop_locked();
    libsnd.cd.data = data;
    libsnd.cd.file = file;
    libsnd.cd.stream = stream;
    libsnd.cd.file_xa = (uint8)xa;
    libsnd.cd.file_xa_start_sector = (uint32)sector;
    libsnd.cd.file_pending = 1;
    xa_reset(&libsnd.cd.file_xa_state);
    libsnd.cd.track = selected;
    libsnd.cd.active = 1;
    libsnd.cd.repeat = mode != 0;
    cd_record_stream_command_locked(CdlPlay);
    callback = libsnd.sync_callback;
    xport_audio_unlock();
    if (callback != 0)
        cd_queue_callback(0, CdlComplete, NULL);
    return 1;
}

static sint32 cd_play_file(const char *path, sint32 mode, sint32 selected, sint32 xa)
{
    return cd_play_file_at(path, mode, selected, xa, 0);
}

sint32 xport_xa_play_file_at(const char *path, sint32 repeat, sint32 sector)
{
    return cd_play_file_at(path, repeat, -1, 1, sector);
}

sint32 xport_xa_play_file(const char *path, sint32 repeat)
{
    return cd_play_file(path, repeat, -1, 1);
}

// Report the existing file mixer clock and original sector identity
sint32 xport_xa_file_position(CdlLOC *position, uint32 *frames)
{
    sint32 ready = 0;
    xport_audio_lock();
    if (libsnd.cd.file_xa && libsnd.cd.data && !libsnd.cd.file_pending && libsnd.cd.stream.block_offset >= 2352 && (libsnd.cd.file_xa_frames || libsnd.cd.stream.decoded_frames == libsnd.cd.stream.total_frames))
    {
        if (position)
        {
            memset(position, 0, sizeof(*position));
            memcpy(position, libsnd.cd.data + libsnd.cd.stream.block_offset - 2352 + 12, 3);
        }
        if (frames)
            *frames = libsnd.cd.stream.decoded_frames;
        ready = 1;
    }
    xport_audio_unlock();
    return ready;
}

sint32 CdPlay(sint32 mode, sint32 *track, sint32 offset)
{
    char path[64];
    if (track == 0 || mode < 0 || offset < 0)
        return 0;
    snprintf(path, sizeof(path), "MUSIC/%d.WAV", *track);
    return cd_play_file(path, mode, *track, 0);
}

sint32 CdSetToc(const CdlLOC *toc, sint32 track_count)
{
    if (toc == 0 || track_count <= 0 || track_count >= (sint32)(sizeof(cd_toc) / sizeof(cd_toc[0])))
        return 0;
    xport_audio_lock();
    memcpy(cd_toc, toc, (size_t)(track_count + 1) * sizeof(cd_toc[0]));
    cd_toc_track_count = track_count;
    xport_audio_unlock();
    return track_count;
}

sint32 CdDiskReady(sint32 mode)
{
    (void)mode;
    return CdlComplete;
}

sint32 CdGetToc2(sint32 mode, CdlLOC *toc)
{
    sint32 tracks;

    if (!toc)
        return 0;
    xport_audio_lock();
    tracks = cd_toc_track_count;
    if (tracks > 0)
        memcpy(toc, cd_toc, (size_t)(tracks + 1) * sizeof(cd_toc[0]));
    xport_audio_unlock();
    return tracks;
}

sint32 CdGetToc(CdlLOC *toc)
{
    sint32 result;
    result = CdGetToc2(1, toc);
    return result;
}

static void cd_xa_stop_locked(void);
static void cd_xa_set_reading_locked(sint32 reading);
static sint32 cd_xa_active_locked(void);
static uint32 cd_xa_render_locked(sint16 *output, uint32 count);

sint32 CdControlBPSX(uint8 command, uint8 *parameter, uint8 *result);

static sint32 cd_control_dispatch(uint8 command, uint8 *parameter, uint8 *result)
{
    CdlCB callback;
    uint8 status = CdlComplete;
    if (command == 1u || command == 16u || command == 17 || command == 2u || command == 6u || command == 14u || command == 21u || command == 22u || command == 27u)
        return CdControlBPSX(command, parameter, result);
    if (command == CdlPlay && parameter != 0)
    {
        sint32 track;

        xport_audio_lock();
        for (track = 1; track <= cd_toc_track_count; ++track)
            if (memcmp(parameter, cd_toc + track, 3u) == 0)
                break;
        xport_audio_unlock();
        if (track > cd_toc_track_count || !CdPlay(1, &track, 0))
        {
            if (result != 0)
                result[0] = CdlDiskError;
            return 0;
        }
        if (result != 0)
            result[0] = CdlComplete;
        return 1;
    }
    xport_audio_lock();
    switch (command)
    {
        case 1:
            break;
        case CdlPlay:
            if (libsnd.cd.data != 0)
            {
                libsnd.cd_end_callback = NULL;
                libsnd.cd.active = 1;
                libsnd.cd.paused = 0;
            }
            cd_record_stream_command_locked(command);
            break;
        case CdlPause:
            libsnd.cd.paused = 1;
            cd_xa_stop_locked();
            break;
        case CdlStop:
            cd_xa_stop_locked();
            cd_audio_stop_locked();
            break;
        case CdlMute:
            libsnd.cd.muted = 1;
            break;
        case CdlDemute:
            libsnd.cd.muted = 0;
            break;
        case 13u:
            if (parameter == NULL)
                status = CdlDiskError;
            else
            {
                if (libsnd.cd_filter_file != parameter[0] || libsnd.cd_filter_channel != parameter[1])
                {
                    sint32 reading = cd_xa_active_locked();
                    cd_xa_stop_locked();
                    cd_xa_set_reading_locked(reading);
                }
                libsnd.cd_filter_file = parameter[0];
                libsnd.cd_filter_channel = parameter[1];
            }
            break;
        case CdlSetmode:
            libsnd.cd_mode = parameter != 0 ? parameter[0] : 0;
            break;
        default:
            status = CdlDiskError;
            break;
    }
    callback = libsnd.sync_callback;
    xport_audio_unlock();
    if (result != 0)
        result[0] = status;
    if (callback != 0)
        cd_queue_callback(0, status, result);
    if (status != CdlDiskError)
        cd_notify_command(command);
    return status != CdlDiskError;
}

typedef struct CdCommandState
{
    sint32 completion;
    uint8 response[8];
    uint8 last_command;
    sint32 debug_level;
    uint8 last_stream_command;
} CdCommandState;

static CdCommandState cd_command = {CdlNoIntr, {0}, 0, 0, 0};

sint32 CdSetDebug(sint32 level)
{
    sint32 previous = cd_command.debug_level;
    cd_command.debug_level = level;
    return previous;
}

sint32 CdLastCom(void)
{
    sint32 command;
    xport_audio_lock();
    command = cd_command.last_command;
    xport_audio_unlock();
    return command;
}

sint32 CdLastStreamCom(void)
{
    sint32 command;
    xport_audio_lock();
    command = cd_command.last_stream_command;
    xport_audio_unlock();
    return command;
}

sint32 CdMode(void)
{
    sint32 mode;
    xport_audio_lock();
    mode = libsnd.cd_mode;
    xport_audio_unlock();
    return mode;
}

static uint8 cd_native_status_locked(void);

sint32 CdStatus(void)
{
    uint8 status;

    if (xport_cd_status_address != 0u)
        return r_u8(xport_cd_status_address);
    xport_audio_lock();
    status = cd_native_status_locked();
    xport_audio_unlock();
    return status;
}

static sint32 cd_control_native(uint8 command, uint8 *parameter, uint8 *result)
{
    uint8 response[8] = {0};
    sint32 accepted = cd_control_dispatch(command, parameter, response);
    xport_audio_lock();
    cd_command.completion = accepted ? CdlComplete : CdlDiskError;
    memcpy(cd_command.response, response, sizeof(cd_command.response));
    xport_audio_unlock();
    if (result)
        memcpy(result, response, command == 16 || command == 17 ? 8 : 1);
    return accepted;
}

sint32 CdControl(uint8 command, uint8 *parameter, uint8 *result)
{
    uint32 previous, table, attempts = 3;
    if (!xport_cd_setloc_table_address)
        return cd_control_native(command, parameter, result);
    previous = r_u32(xport_cd_sync_callback_address);
    table = xport_cd_setloc_table_address + 4 * command;
    for (;;)
    {
        w_u32(xport_cd_sync_callback_address, 0);
        if (command != 1 && (r_u8(xport_cd_status_address) & 0x10))
            CD_cw(1, NULL, NULL, 0);
        if (!parameter || !r_u32(table) || !CD_cw(2, parameter, result, 0))
        {
            w_u32(xport_cd_sync_callback_address, previous);
            if (!CD_cw(command, parameter, result, 0))
                return 1;
        }
        --attempts;
        if (attempts == 0xFFFFFFFF)
        {
            w_u32(xport_cd_sync_callback_address, previous);
            return 0;
        }
    }
}

static CD_GUEST_BINDING cd_guest;

void cd_bind_guest(const CD_GUEST_BINDING *binding)
{
    if (binding)
        cd_guest = *binding;
    else
        memset(&cd_guest, 0, sizeof(cd_guest));
}

static void cd_sdk_copy(uint8 *destination, uint32 source)
{
    uint32 index;
    if (destination)
        for (index = 0; index < 8; ++index)
            destination[index] = r_u8(source + index);
}

static void cd_sdk_store(uint32 destination, const uint8 response[8])
{
    uint32 index;
    for (index = 0; index < 8; ++index)
        w_u8(destination + index, response[index]);
}

void cd_publish_guest_irq(uint32 interrupt, const uint8 *response, uint32 length)
{
    uint8 packet[8] = {0};
    uint32 index, error = 0, callbacks = 0, target;
    if (!cd_guest.context || !interrupt)
        return;
    if (length > 8)
        length = 8;
    for (index = 0; index < length; ++index)
        packet[index] = response[index];
    if (interrupt != 3 || r_u32(cd_guest.command_flags + 256 + 4 * r_u8(cd_guest.context + 29)))
    {
        if (!(r_u32(cd_guest.context + 12) & 16) && (packet[0] & 16))
            w_u32(cd_guest.context + 20, r_u32(cd_guest.context + 20) + 1);
        error = packet[0] & 0x1D;
        w_u32(cd_guest.context + 12, packet[0]);
        w_u32(cd_guest.context + 16, packet[1]);
    }
#ifdef _DEBUG
    if (interrupt == 5 && r_s32(cd_guest.context + 8) >= 3)
    {
        printf("%s", (const char *)xport_guest_cptr(cd_guest.strings + 44, 1));
        if (r_s32(cd_guest.context + 8) >= 3)
            printf((const char *)xport_guest_cptr(cd_guest.strings + 56, 1), (const char *)xport_guest_cptr(r_u32(cd_guest.command_names + 4 * r_u8(cd_guest.context + 29)), 1), (unsigned int)r_u32(cd_guest.context + 12), (unsigned int)r_u32(cd_guest.context + 16));
    }
#endif
    switch (interrupt)
    {
        case 1:
            if (error && length == 1)
                error = 0;
            w_u8(cd_guest.events + 1, error ? 5 : 1);
            cd_sdk_store(cd_guest.responses + 8, packet);
            callbacks = 4;
            break;
        case 2:
            w_u8(cd_guest.events, error ? 5 : 2);
            cd_sdk_store(cd_guest.responses, packet);
            callbacks = 2;
            break;
        case 3:
            if (!error && r_u32(cd_guest.command_flags + 4 * r_u8(cd_guest.context + 29)))
                w_u8(cd_guest.events, 3);
            else
            {
                w_u8(cd_guest.events, error ? 5 : 2);
                callbacks = 2;
            }
            cd_sdk_store(cd_guest.responses, packet);
            break;
        case 4:
            w_u8(cd_guest.events + 2, 4);
            w_u8(cd_guest.events + 1, r_u8(cd_guest.events + 2));
            cd_sdk_store(cd_guest.responses + 16, packet);
            cd_sdk_store(cd_guest.responses + 8, packet);
            callbacks = 4;
            break;
        case 5:
            w_u8(cd_guest.events + 1, 5);
            w_u8(cd_guest.events, r_u8(cd_guest.events + 1));
            cd_sdk_store(cd_guest.responses, packet);
            cd_sdk_store(cd_guest.responses + 8, packet);
            callbacks = 6;
            break;
        default:
#ifdef _DEBUG
            printf("%s", (const char *)xport_guest_cptr(cd_guest.strings + 84, 1));
            printf((const char *)xport_guest_cptr(cd_guest.strings + 104, 1), (unsigned int)interrupt);
#endif
            break;
    }
    if ((callbacks & 4) && (target = r_u32(xport_cd_ready_callback_address)) != 0)
    {
        if (cd_guest.dispatch)
            cd_guest.dispatch(target, r_u8(cd_guest.events + 1), cd_guest.responses + 8);
        else
            (void)cd_guest.resolve(target)(r_u8(cd_guest.events + 1), cd_guest.responses + 8);
    }
    if ((callbacks & 2) && (target = r_u32(xport_cd_sync_callback_address)) != 0)
    {
        if (cd_guest.dispatch)
            cd_guest.dispatch(target, r_u8(cd_guest.events), cd_guest.responses);
        else
            (void)cd_guest.resolve(target)(r_u8(cd_guest.events), cd_guest.responses);
    }
}

static void cd_sdk_service(void)
{
    for (;;)
    {
        uint8 interrupt, length, response[8];
        uint32 index;
        xport_audio_lock();
        if (!libsnd.pending_sdk_count)
        {
            xport_audio_unlock();
            return;
        }
        index = libsnd.pending_sdk_read;
        interrupt = libsnd.pending_sdk_callbacks[index].interrupt;
        length = libsnd.pending_sdk_callbacks[index].length;
        memcpy(response, libsnd.pending_sdk_callbacks[index].response, 8);
        libsnd.pending_sdk_read = (index + 1) & 255;
        --libsnd.pending_sdk_count;
        xport_audio_unlock();
        cd_publish_guest_irq(interrupt, response, length);
    }
}

static void cd_sdk_queue(uint8 interrupt, const uint8 response[8], uint8 length)
{
    uint32 index;
    xport_audio_lock();
    if (libsnd.pending_sdk_count == 256)
        abort();
    index = (libsnd.pending_sdk_read + libsnd.pending_sdk_count) & 255;
    libsnd.pending_sdk_callbacks[index].interrupt = interrupt;
    libsnd.pending_sdk_callbacks[index].length = length;
    memcpy(libsnd.pending_sdk_callbacks[index].response, response, 8);
    ++libsnd.pending_sdk_count;
    xport_audio_unlock();
    psx_wait_signal_service(6);
    psx_sync_callbacks();
}

static void cd_sdk_timeout_begin(uint32 label)
{
    uint32 now = (uint32)VSync(-1);
    w_u32(cd_guest.responses + 40, label);
    w_u32(cd_guest.responses + 32, now + 960);
    w_u32(cd_guest.responses + 36, 0);
}

static sint32 cd_sdk_timeout_poll(void)
{
    uint32 previous;
    if ((sint32)r_u32(cd_guest.responses + 32) >= VSync(-1))
    {
        previous = r_u32(cd_guest.responses + 36);
        w_u32(cd_guest.responses + 36, previous + 1);
        if ((sint32)previous <= 0x3C0000)
            return 0;
    }
#ifdef _DEBUG
    printf("%s", (const char *)xport_guest_cptr(cd_guest.strings, 1));
    printf((const char *)xport_guest_cptr(cd_guest.strings + 16, 1), (const char *)xport_guest_cptr(r_u32(cd_guest.responses + 40), 1), (const char *)xport_guest_cptr(r_u32(cd_guest.command_names + 4 * r_u8(cd_guest.context + 29)), 1), (const char *)xport_guest_cptr(r_u32(cd_guest.event_names + 4 * r_u8(cd_guest.events)), 1), (const char *)xport_guest_cptr(r_u32(cd_guest.event_names + 4 * r_u8(cd_guest.events + 1)), 1));
#endif
    xport_audio_lock();
    libsnd.pending_sdk_read = libsnd.pending_sdk_count = 0;
    libsnd.cd_bus_delay = 0x1325;
    xport_audio_unlock();
    w_u8(cd_guest.events + 2, 0);
    w_u8(cd_guest.events + 1, r_u8(cd_guest.events + 2));
    w_u8(cd_guest.events, 2);
    return -1;
}

static void cd_sdk_poll(void)
{
    psx_sync_callbacks();
    if (r_u16(cd_guest.poll_enabled))
        cd_sdk_service();
}

sint32 CD_cw(uint8 command, const uint8 *parameter, uint8 *result, uint32 asynchronous)
{
    uint8 response[8] = {0};
    uint8 complete_response[8] = {0};
    uint8 status_before, status_after;
    uint32 table, index;
    sint32 dispatched;
    if (!cd_guest.context)
    {
        dispatched = cd_control_native(command, (uint8 *)parameter, result ? result : response);
        xport_audio_lock();
        cd_command.completion = dispatched ? CdlComplete : CdlDiskError;
        xport_audio_unlock();
        return dispatched ? 0 : -1;
    }
#ifdef _DEBUG
    if (r_s32(cd_guest.context + 8) >= 2)
        printf((const char *)xport_guest_cptr(cd_guest.strings + 132, 1), (const char *)xport_guest_cptr(r_u32(cd_guest.command_names + 4 * command), 1));
#endif
    table = cd_guest.command_flags + 4 * command;
    if (r_u32(table + 384) && !parameter)
    {
#ifdef _DEBUG
        if (r_s32(cd_guest.context + 8) > 0)
            printf((const char *)xport_guest_cptr(cd_guest.strings + 140, 1), (const char *)xport_guest_cptr(r_u32(cd_guest.command_names + 4 * command), 1));
#endif
        return -2;
    }
    (void)CD_sync(0, NULL);
    if (command == 2)
        for (index = 0; index < 4; ++index)
            w_u8(cd_guest.context + 24 + index, parameter[index]);
    if (command == 14)
        w_u8(cd_guest.context + 28, parameter[0]);
    w_u8(cd_guest.events, 0);
    if (r_u32(table + 128))
        w_u8(cd_guest.events + 1, 0);
    w_u8(cd_guest.context + 29, command);
    xport_audio_lock();
    status_before = cd_native_status_locked();
    xport_audio_unlock();
    dispatched = cd_control_dispatch(command, (uint8 *)parameter, response);
    xport_audio_lock();
    status_after = cd_native_status_locked();
    cd_command.completion = dispatched ? CdlComplete : CdlDiskError;
    memcpy(cd_command.response, response, sizeof(cd_command.response));
    xport_audio_unlock();
    if (dispatched)
    {
        if (r_u32(table + 256))
            response[0] = status_before;
        cd_sdk_queue(3, response, command == 16 || command == 17 ? 8 : 1);
        if (r_u32(table))
        {
            complete_response[0] = status_after;
            cd_sdk_queue(2, complete_response, 1);
        }
    }
    else
        cd_sdk_queue(5, response, 2);
    if (asynchronous)
        return 0;
    cd_sdk_timeout_begin(cd_guest.strings + 156);
    while (!r_u8(cd_guest.events))
    {
        if (cd_sdk_timeout_poll())
            return -1;
        cd_sdk_poll();
    }
    cd_sdk_copy(result, cd_guest.responses);
    return r_u8(cd_guest.events) == 5 ? -1 : 0;
}

sint32 CD_sync(sint32 mode, uint8 *result)
{
    sint32 completion;
    if (!cd_guest.context)
    {
        xport_audio_lock();
        completion = cd_command.completion;
        if (result)
            memcpy(result, cd_command.response, sizeof(cd_command.response));
        xport_audio_unlock();
        return completion;
    }
    cd_sdk_timeout_begin(cd_guest.strings + 112);
    do
    {
        if (cd_sdk_timeout_poll())
            return -1;
        cd_sdk_poll();
        completion = r_u8(cd_guest.events);
        if (completion == 2 || completion == 5)
        {
            w_u8(cd_guest.events, 2);
            cd_sdk_copy(result, cd_guest.responses);
            return completion;
        }
    } while (!mode);
    return 0;
}

// PsyQ LIBCD SYS command flags for implicit Setloc
static const uint8 cd_setloc_commands[32] = {0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0};

static sint32 cd_control_b_native(uint8 command, uint8 *parameter, uint8 *result)
{
    CdlCB prev;
    uint32 attempts = 3u;

    if (command >= sizeof(cd_setloc_commands) / sizeof(cd_setloc_commands[0]))
        return 0;
    prev = CdSyncCallback(NULL);
    for (;;)
    {
        CdSyncCallback(NULL);
        if (command != 1u && (CdStatus() & 0x10u) != 0u)
            CD_cw(1u, NULL, NULL, 0u);
        if (!parameter || !cd_setloc_commands[command] || CD_cw(2u, parameter, result, 0u) == 0)
        {
            CdSyncCallback(prev);
            if (CD_cw(command, parameter, result, 0u) == 0)
                break;
        }
        if (attempts-- == 0u)
        {
            CdSyncCallback(prev);
            return 0;
        }
    }
    return CD_sync(0, result) == CdlComplete;
}

sint32 CdControlB(uint8 command, uint8 *parameter, uint8 *result)
{
    uint32 attempts;
    uint32 previous_callback;
    uint32 setloc_entry;
    sint32 dispatch_result;
    if (xport_cd_setloc_table_address == 0u)
        return cd_control_b_native(command, parameter, result);
    attempts = 3u;
    previous_callback = r_u32(xport_cd_sync_callback_address);
    setloc_entry = xport_cd_setloc_table_address + 4u * command;
    for (;;)
    {
        w_u32(xport_cd_sync_callback_address, 0u);
        if (command != 1u && (r_u8(xport_cd_status_address) & 0x10u) != 0u)
            CD_cw(1u, NULL, NULL, 0u);
        if (parameter == NULL || r_u32(setloc_entry) == 0u || CD_cw(2u, parameter, result, 0u) == 0)
        {
            w_u32(xport_cd_sync_callback_address, previous_callback);
            dispatch_result = CD_cw(command, parameter, result, 0u);
            if (dispatch_result == 0)
            {
                dispatch_result = 0;
                break;
            }
        }
        --attempts;
        dispatch_result = -1;
        if (attempts == 0xFFFFFFFFu)
        {
            w_u32(xport_cd_sync_callback_address, previous_callback);
            break;
        }
    }
    if (dispatch_result != 0)
        return 0;
    return CD_sync(0, result) == 2;
}

sint32 CdControlF(uint8 command, uint8 *parameter)
{
    uint32 previous_callback, attempts = 3;
    CdlCB previous_native_callback = NULL;
    if (xport_cd_setloc_table_address)
        previous_callback = r_u32(xport_cd_sync_callback_address);
    else
    {
        previous_callback = 0;
        previous_native_callback = CdSyncCallback(NULL);
    }
    for (;;)
    {
        if (xport_cd_setloc_table_address)
            w_u32(xport_cd_sync_callback_address, 0);
        else
            CdSyncCallback(NULL);
        if (command != 1 && (CdStatus() & 16))
            (void)CD_cw(1, NULL, NULL, 0);
        if (!parameter || !(xport_cd_setloc_table_address ? r_u32(xport_cd_setloc_table_address + 4 * command) : (command < sizeof(cd_setloc_commands) ? cd_setloc_commands[command] : 0)) || CD_cw(2, parameter, NULL, 0) == 0)
        {
            if (xport_cd_setloc_table_address)
                w_u32(xport_cd_sync_callback_address, previous_callback);
            else
                CdSyncCallback(previous_native_callback);
            if (CD_cw(command, parameter, NULL, 1) == 0)
                return 1;
        }
        if (attempts-- == 0)
        {
            if (xport_cd_setloc_table_address)
                w_u32(xport_cd_sync_callback_address, previous_callback);
            else
                CdSyncCallback(previous_native_callback);
            return 0;
        }
    }
}

sint32 CdRead2(sint32 mode)
{
    uint8 parameter = (uint8)mode;

    CdControl(CdlSetmode, &parameter, 0);
    return CdControl(0x1Bu, 0, 0);
}

sint32 CdMix(CdlATV *volume)
{
    if (volume == 0)
        return 0;
    xport_audio_lock();
    libsnd.cd.mix = *volume;
    xport_audio_unlock();
    return 1;
}

void cd_dispatch_ready_event(void)
{
    CdlCB callback;

    if (!psx_callbacks_allowed())
        return;

    cd_sdk_service();

    for (;;)
    {
        uint8 status, response[8], has_response;
        uint32 index;
        xport_audio_lock();
        if (!libsnd.pending_callback_count)
        {
            xport_audio_unlock();
            break;
        }
        index = libsnd.pending_callback_read;
        if (libsnd.pending_callbacks[index].kind == CD_CALLBACK_READ)
            callback = libsnd.read_callback;
        else if (libsnd.pending_callbacks[index].kind == CD_CALLBACK_READY)
            callback = libsnd.ready_callback;
        else
            callback = libsnd.sync_callback;
        status = libsnd.pending_callbacks[index].status;
        memcpy(response, libsnd.pending_callbacks[index].response, sizeof(response));
        has_response = libsnd.pending_callbacks[index].has_response;
        libsnd.pending_callback_read = (index + 1) & 255;
        --libsnd.pending_callback_count;
        xport_audio_unlock();
        if (callback)
            callback(status, has_response ? response : NULL);
        if (!psx_callbacks_allowed())
            return;
    }

    xport_audio_lock();
    callback = libsnd.cd_end_callback;
    libsnd.cd_end_callback = NULL;
    xport_audio_unlock();
    if (callback)
        callback(CdlDataEnd, NULL);
}

CdlCB CdReadyCallback(CdlCB callback)
{
    CdlCB previous;
    xport_audio_lock();
    previous = libsnd.ready_callback;
    if (previous != callback)
        libsnd.cd_end_callback = NULL;
    libsnd.ready_callback = callback;
    xport_audio_unlock();
    return previous;
}

PsxInterruptCallback CdDataCallback(PsxInterruptCallback callback)
{
    PsxInterruptCallback previous;
    previous = DMACallback(3u, callback);
    return previous;
}

CdlCB CdSyncCallback(CdlCB callback)
{
    CdlCB previous;
    xport_audio_lock();
    previous = libsnd.sync_callback;
    libsnd.sync_callback = callback;
    xport_audio_unlock();
    return previous;
}

CdlCB CdReadCallback(CdlCB callback)
{
    CdlCB previous;
    xport_audio_lock();
    previous = libsnd.read_callback;
    libsnd.read_callback = callback;
    xport_audio_unlock();
    return previous;
}

/* Independent fixed-point implementation of the documented SPU reverb network */
static const sint16 spu_reverb_fir[39] = {-1, 0, 2, 0, -10, 0, 35, 0, -103, 0, 266, 0, -616, 0, 1332, 0, -2960, 0, 10246, 16384, 10246, 0, -2960, 0, 1332, 0, -616, 0, 266, 0, -103, 0, 35, 0, -10, 0, 2, 0, -1};

static uint32 spu_reverb_address(sint32 offset)
{
    uint32 base = (reverb_control.work_address << 3) & (SPU_RAM_SIZE - 2u);
    uint32 length = SPU_RAM_SIZE - base;
    sint64 relative = (sint64)spu.reverb_cursor + offset;
    relative %= length;
    if (relative < 0)
        relative += length;
    return base + (uint32)relative;
}

static sint32 spu_reverb_read(sint32 offset)
{
    uint32 address = spu_reverb_address(offset);
    spu_irq_check_range(address, 2);
    return (sint16)((uint16)spu.ram[address] | ((uint16)spu.ram[address + 1u] << 8));
}

static void spu_reverb_write(sint32 offset, sint32 value)
{
    uint32 address;
    uint16 sample;
    if (!reverb_control.enabled)
        return;
    address = spu_reverb_address(offset);
    sample = (uint16)audio_sample_clamp(value);
    spu.ram[address] = (uint8)sample;
    spu.ram[address + 1u] = (uint8)(sample >> 8);
    spu_irq_check_range(address, 2);
}

static sint32 spu_reverb_q15(sint64 value)
{
    /* Division rounds toward negative infinity without signed shift assumptions */
    return (sint32)(value >= 0 ? value / 32768 : -((-value + 32767) / 32768));
}

static sint32 spu_reverb_tap(uint32 parameter, sint32 displacement)
{
    return spu_reverb_read((sint32)((uint32)reverb_control.parameters[parameter] << 3) + displacement);
}

static sint32 spu_reverb_shift(sint64 value, uint32 bits)
{
    sint64 divisor = (sint64)1 << bits;
    return (sint32)(value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor));
}

static sint32 spu_reverb_negative(sint32 value)
{
    return value == -32768 ? 32767 : -value;
}

static sint32 spu_reverb_reflection(sint32 input, sint32 previous, sint32 alpha)
{
    sint64 history = (sint64)previous * (32768 - alpha);
    if (alpha == -32768)
        history = previous == -32768 ? 0 : (sint64)previous * -65536;
    return audio_sample_clamp(spu_reverb_shift(spu_reverb_shift((sint64)input * alpha, 14u) + spu_reverb_shift(history, 14u), 1u));
}

static void spu_reverb_network(const sint32 input[2], sint32 output[2])
{
    const uint16 *p = reverb_control.parameters;
    uint32 channel;
    for (channel = 0u; channel < 2u; ++channel)
    {
        sint32 same = (sint32)((uint32)p[10u + channel] << 3);
        sint32 different = (sint32)((uint32)p[18u + channel] << 3);
        sint32 alpha = (sint16)p[2];
        sint32 wall = (sint16)p[7];
        sint32 in = spu_reverb_shift((sint64)input[channel] * (sint16)p[30u + channel], 14u);
        if (reverb_control.enabled)
        {
            sint32 reflected = audio_sample_clamp(spu_reverb_shift(in + spu_reverb_shift((sint64)spu_reverb_tap(16u + channel, 0) * wall, 14u), 1u));
            spu_reverb_write(same, spu_reverb_reflection(reflected, spu_reverb_read(same - 2), alpha));
            reflected = audio_sample_clamp(spu_reverb_shift(in + spu_reverb_shift((sint64)spu_reverb_tap(24u + (channel ^ 1u), 0) * wall, 14u), 1u));
            spu_reverb_write(different, spu_reverb_reflection(reflected, spu_reverb_read(different - 2), alpha));
        }
        {
            sint32 sum = spu_reverb_shift((sint64)spu_reverb_tap(12u + channel, 0) * (sint16)p[3], 14u) + spu_reverb_shift((sint64)spu_reverb_tap(14u + channel, 0) * (sint16)p[4], 14u) + spu_reverb_shift((sint64)spu_reverb_tap(20u + channel, 0) * (sint16)p[5], 14u) + spu_reverb_shift((sint64)spu_reverb_tap(22u + channel, 0) * (sint16)p[6], 14u);
            sint32 address1 = (sint32)((uint32)p[26u + channel] << 3);
            sint32 address2 = (sint32)((uint32)p[28u + channel] << 3);
            sint32 delayed1 = spu_reverb_read(address1 - (sint32)((uint32)p[0] << 3));
            sint32 delayed2 = spu_reverb_read(address2 - (sint32)((uint32)p[1] << 3));
            sint32 coefficient1 = (sint16)p[8];
            sint32 coefficient2 = (sint16)p[9];
            sint32 stored1 = audio_sample_clamp(spu_reverb_shift(sum + spu_reverb_shift((sint64)delayed1 * spu_reverb_negative(coefficient1), 14u), 1u));
            sint32 stored2 = audio_sample_clamp(delayed1 + spu_reverb_shift(spu_reverb_shift((sint64)stored1 * coefficient1, 14u) + spu_reverb_shift((sint64)delayed2 * spu_reverb_negative(coefficient2), 14u), 1u));
            output[channel] = audio_sample_clamp(delayed2 + spu_reverb_q15((sint64)stored2 * coefficient2));
            spu_reverb_write(address1, stored1);
            spu_reverb_write(address2, stored2);
        }
    }
    spu.reverb_cursor += 2u;
    if (spu.reverb_cursor >= SPU_RAM_SIZE - spu.reverb_base)
        spu.reverb_cursor = 0u;
}

static void spu_reverb_frame(sint32 left, sint32 right, sint32 *wet_left, sint32 *wet_right)
{
    uint32 base = (reverb_control.work_address << 3) & (SPU_RAM_SIZE - 2u);
    uint32 channel;
    uint32 index;
    sint32 input[2];
    sint32 output[2] = {0, 0};
    if (spu.reverb_base != base)
    {
        spu.reverb_base = base;
        spu.reverb_cursor = 0u;
    }
    index = spu.reverb_history_index;
    spu.reverb_input[0][index] = audio_sample_clamp(left);
    spu.reverb_input[1][index] = audio_sample_clamp(right);
    spu.reverb_phase ^= 1u;
    if (!spu.reverb_phase)
    {
        for (channel = 0u; channel < 2u; ++channel)
        {
            sint64 sum = 0;
            uint32 tap;
            for (tap = 0u; tap < 39u; ++tap)
                sum += (sint64)spu.reverb_input[channel][(index + 39u - tap) % 39u] * spu_reverb_fir[tap];
            input[channel] = audio_sample_clamp(spu_reverb_q15(sum));
        }
        spu_reverb_network(input, output);
    }
    for (channel = 0u; channel < 2u; ++channel)
    {
        sint64 sum = 0;
        uint32 tap;
        spu.reverb_output[channel][index] = (sint16)output[channel];
        for (tap = 0u; tap < 39u; ++tap)
            sum += (sint64)spu.reverb_output[channel][(index + 39u - tap) % 39u] * spu_reverb_fir[tap];
        output[channel] = audio_sample_clamp(spu_reverb_q15(sum * 2));
    }
    *wet_left = spu_reverb_q15((sint64)output[0] * reverb_control.depth_left);
    *wet_right = spu_reverb_q15((sint64)output[1] * reverb_control.depth_right);
    spu.reverb_history_index = (index + 1u) % 39u;
}

/* Optional raw PCM capture, 24 stereo voices in sample-major order */
static FILE *spu_voice_capture_file;
#ifdef _DEBUG
static sint32 spu_voice_capture_checked;
static FILE *spu_mix_capture_file;
#endif
static sint16 spu_voice_capture_buffer[256][24][2];
static uint32 spu_voice_capture_frames;
uint64 spu_voice_capture_sample_count;

#ifdef _DEBUG
static void spu_voice_capture_close(void)
{
    if (spu_mix_capture_file)
    {
        fclose(spu_mix_capture_file);
        spu_mix_capture_file = NULL;
    }
    if (spu_voice_capture_file)
    {
        fwrite(spu_voice_capture_buffer, sizeof(spu_voice_capture_buffer[0]), spu_voice_capture_frames, spu_voice_capture_file);
        fclose(spu_voice_capture_file);
        spu_voice_capture_file = NULL;
        spu_voice_capture_frames = 0;
    }
}

#endif

static void spu_noise_advance(void)
{
    static const uint32 fractional_steps[4] = {0, 84, 140, 180};
    uint32 clock = (reverb_control.control >> 8) & 63;
    uint32 period = ((uint32)0x8000 >> (clock >> 2)) << 16;
    uint32 feedback;
    spu.noise_count += 0x10000 + fractional_steps[clock & 3];
    if ((spu.noise_count & 0xFFFF) >= 210)
    {
        spu.noise_count += 0x10000;
        spu.noise_count -= fractional_steps[clock & 3];
    }
    if (spu.noise_count < period)
        return;
    spu.noise_count %= period;
    feedback = ((spu.noise_level >> 15) ^ (spu.noise_level >> 12) ^ (spu.noise_level >> 11) ^ (spu.noise_level >> 10) ^ 1) & 1;
    spu.noise_level = (spu.noise_level << 1) | feedback;
}

static void spu_capture_sample(uint32 bank, uint32 frame, sint32 value)
{
    uint32 address = bank * 1024 + (frame & 511) * 2;
    uint16 sample = (uint16)audio_sample_clamp(value);
    spu.ram[address] = (uint8)sample;
    spu.ram[address + 1] = (uint8)(sample >> 8);
}

static void spu_mix_voices(sint32 *interleaved_stereo, sint32 *sends, uint32 frame_count)
{
    uint32 frame;
#ifdef _DEBUG
    if (!spu_voice_capture_checked)
    {
        const char *path = getenv("XPORT_SPU_VOICE_CAPTURE");
        const char *mix_path = getenv("XPORT_SPU_MIX_CAPTURE");
        spu_voice_capture_checked = 1;
        if (path && *path)
        {
            spu_voice_capture_file = xport_fopen(path, "wb");
            if (!spu_voice_capture_file)
                abort();
        }
        if (mix_path && *mix_path)
            spu_mix_capture_file = xport_fopen(mix_path, "wb");
        if (spu_voice_capture_file || spu_mix_capture_file)
            atexit(spu_voice_capture_close);
    }
#endif
    if (interleaved_stereo == 0)
        return;
    for (frame = 0; frame < frame_count; ++frame)
    {
        sint32 mix_left = 0;
        sint32 mix_right = 0;
        sint32 send_left = 0;
        sint32 send_right = 0;
        sint32 previous_amplitude = 0;
        sint32 index;
        uint32 capture = spu.decoded_capture_cursor;
        spu_capture_sample(2, capture, 0);
        spu_capture_sample(3, capture, 0);
        if (spu_voice_capture_file)
            memset(spu_voice_capture_buffer[spu_voice_capture_frames], 0, sizeof(spu_voice_capture_buffer[0]));
        for (index = 0; index < SPU_VOICE_COUNT; ++index)
        {
            SPU_VOICE *voice = spu.voices + index;
            sint32 sample;
            sint32 amplitude;
            uint32 step;
            if (voice->adsr_phase == SPU_ADSR_OFF && !(reverb_control.control & 0x40))
            {
                previous_amplitude = 0;
                continue;
            }
            if (!voice->decoded_valid)
                decode_voice_block(voice);
            sample = spu.noise_mask & (1u << index) ? (sint16)spu.noise_level : interpolate_voice(voice);
            amplitude = apply_volume(sample, voice->envelope);
            if (index == 1 || index == 3)
                spu_capture_sample(index == 1 ? 2 : 3, capture, amplitude);
            {
                sint32 left = apply_volume(amplitude, fixed_volume((uint16)voice->registers.volume_left));
                sint32 right = apply_volume(amplitude, fixed_volume((uint16)voice->registers.volume_right));
                if (spu_voice_capture_file)
                {
                    spu_voice_capture_buffer[spu_voice_capture_frames][index][0] = (sint16)audio_sample_clamp(left);
                    spu_voice_capture_buffer[spu_voice_capture_frames][index][1] = (sint16)audio_sample_clamp(right);
                }
                mix_left += left;
                mix_right += right;
                if (spu.reverb_voice_mask & (1u << index))
                {
                    send_left += left;
                    send_right += right;
                }
            }
            tick_adsr(voice);
            step = voice->registers.pitch;
            if (index && (spu.pitch_modulation & (1u << index)))
                step = (uint16)(((sint32)(sint16)step * (audio_sample_clamp(previous_amplitude) + 32768)) >> 15);
            previous_amplitude = amplitude;
            if (step > 0x3fff)
                step = 0x3fff;
            voice->phase += step;
            voice->sample_position += step;
            if ((voice->phase >> 12) >= 28)
            {
                voice->phase -= 28u << 12;
                voice->decoded_valid = 0;
                voice->first_block = 0;
                voice->current_address = (voice->current_address + 16) & (SPU_RAM_SIZE - 1);
                if (voice->block_flags & 1)
                {
                    spu.end_flags |= 1u << index;
                    voice->current_address = voice->repeat_address & (SPU_RAM_SIZE - 1);
                    if (!(voice->block_flags & 2) && !(spu.noise_mask & (1u << index)))
                    {
                        voice->adsr_phase = SPU_ADSR_OFF;
                        voice->envelope = 0;
                    }
                }
            }
        }
        spu.decoded_capture_cursor = (capture + 1) & 511;
        spu_noise_advance();
        if (spu_voice_capture_file && ++spu_voice_capture_frames == 256u)
        {
            if (fwrite(spu_voice_capture_buffer, sizeof(spu_voice_capture_buffer[0]), 256u, spu_voice_capture_file) != 256u)
                abort();
            spu_voice_capture_frames = 0;
        }
        if (spu_voice_capture_file)
            ++spu_voice_capture_sample_count;
        /* Hardware mute precedes CD mixing and the reverb return */
        if (!(reverb_control.control & 0x4000))
        {
            mix_left = mix_right = 0;
            send_left = send_right = 0;
        }
        interleaved_stereo[frame * 2] += mix_left;
        interleaved_stereo[frame * 2 + 1] += mix_right;
        if (sends)
        {
            sends[frame * 2] = send_left;
            sends[frame * 2 + 1] = send_right;
        }
        else
        {
            sint32 wet_left, wet_right;
            spu_reverb_frame(send_left, send_right, &wet_left, &wet_right);
            interleaved_stereo[frame * 2] += wet_left;
            interleaved_stereo[frame * 2 + 1] += wet_right;
        }
        if (frame == 0 && (spu.key_off_pending != 0u || spu.key_on_pending != 0u))
        {
            uint32 key_off = spu.key_off_pending;
            uint32 key_on = spu.key_on_pending;

            spu.key_off_pending = 0u;
            spu.key_on_pending = 0u;
            for (index = 0; index < SPU_VOICE_COUNT; ++index)
            {
                SPU_VOICE *voice = spu.voices + index;
                uint32 bit = 1u << index;

                if ((key_off & bit) != 0u && voice->adsr_phase != SPU_ADSR_OFF)
                {
                    voice->adsr_phase = SPU_ADSR_RELEASE;
                    update_adsr(voice);
                }
                if ((key_on & bit) == 0u)
                    continue;
                voice->current_address = (uint32)(voice->registers.start_address & 0xfffeu) * 8u;
                voice->repeat_address = (uint32)voice->registers.repeat_address * 8u;
                voice->phase = 0;
                voice->sample_position = 0;
                voice->history1 = 0;
                voice->history2 = 0;
                memset(voice->decoded, 0, sizeof(voice->decoded));
                voice->decoded_index = 0;
                voice->decoded_valid = 0;
                voice->block_flags = 0;
                voice->first_block = 1;
                voice->ignore_loop_address = 0;
                voice->envelope = 0;
                voice->adsr_phase = SPU_ADSR_ATTACK;
                update_adsr(voice);
                spu.end_flags &= ~bit;
            }
        }
    }
}

void spu_mix(sint32 *interleaved_stereo, uint32 frame_count)
{
    spu_mix_voices(interleaved_stereo, NULL, frame_count);
}

static SpuCdPcmSource cd_pcm_source;
static void *cd_pcm_context;

void spu_set_cd_pcm_source(SpuCdPcmSource source, void *context)
{
    xport_audio_lock();
    cd_pcm_source = source;
    cd_pcm_context = context;
    xport_audio_unlock();
}

void spu_render(sint16 *interleaved_stereo, uint32 frame_count)
{
    uint32 offset = 0;
    if (interleaved_stereo == 0 || frame_count == 0)
        return;
    memset(interleaved_stereo, 0, (size_t)frame_count * 2u * sizeof(*interleaved_stereo));
    xport_audio_lock();
    if (!spu.initialized)
    {
        xport_audio_unlock();
        return;
    }
    spu_synchronize_device_clock();
    while (offset < frame_count)
    {
        /* Transient workspaces are protected by the audio lock */
        static sint32 mixed[SPU_MIX_CHUNK_FRAMES * 2];
        static sint32 sends[SPU_MIX_CHUNK_FRAMES * 2];
        static sint16 decoded[SPU_MIX_CHUNK_FRAMES * 2];
        uint32 count = frame_count - offset;
        uint32 synchronized = 0u;
        uint32 produced = 0;
        uint32 capture;
        uint32 sample;
        if (count > SPU_MIX_CHUNK_FRAMES)
            count = SPU_MIX_CHUNK_FRAMES;
        memset(mixed, 0, count * 2u * sizeof(*mixed));
        memset(sends, 0, count * 2u * sizeof(*sends));
        if (synchronized < count)
            spu_mix_voices(mixed + synchronized * 2u, sends + synchronized * 2u, count - synchronized);
        /* Synchronized voices already advanced the capture ring */
        capture = (spu.decoded_capture_cursor - count) & 511;
        if (cd_pcm_source || cd_xa_active_locked() || (libsnd.cd.active && !libsnd.cd.paused && !libsnd.cd.muted))
        {
            sint32 xa_source = cd_xa_active_locked();
            produced = cd_pcm_source ? cd_pcm_source(cd_pcm_context, decoded, count) : xa_source ? cd_xa_render_locked(decoded, count) : cd_file_decode(&libsnd.cd, decoded, count);
            if (produced > count)
                produced = count;
            while (!cd_pcm_source && !xa_source && produced < count && libsnd.cd.repeat && libsnd.cd.stream.decoded_frames >= libsnd.cd.stream.total_frames)
            {
                IMA_ADPCM_STREAM stream;
                uint32 restarted;
                if (!cd_file_stream_open(&stream, libsnd.cd.data, libsnd.cd.stream.file_size, libsnd.cd.file_xa))
                    break;
                libsnd.cd.stream = stream;
                xa_reset(&libsnd.cd.file_xa_state);
                libsnd.cd.file_xa_frames = 0;
                libsnd.cd.file_xa_position = 0;
                libsnd.cd.file_xa_start_sector = 0;
                restarted = cd_file_decode(&libsnd.cd, decoded + produced * 2u, count - produced);
                produced += restarted;
                if (!restarted)
                    break;
            }
            for (sample = 0; sample < produced; ++sample)
            {
                sint32 input_left = decoded[sample * 2];
                sint32 input_right = decoded[sample * 2 + 1];
                sint32 routed_left = audio_sample_clamp((input_left * libsnd.cd.mix.val0 + input_right * libsnd.cd.mix.val3) >> 8);
                sint32 routed_right = audio_sample_clamp((input_left * libsnd.cd.mix.val1 + input_right * libsnd.cd.mix.val2) >> 8);
                sint32 cd_left = routed_left;
                sint32 cd_right = routed_right;
                spu_capture_sample(0, capture + sample, cd_left);
                spu_capture_sample(1, capture + sample, cd_right);
                /* CD sends remain independent of the dry mixing switch */
                if (reverb_control.control & 4u)
                {
                    sends[sample * 2] += apply_volume(cd_left, (sint16)spu.cd_left);
                    sends[sample * 2 + 1] += apply_volume(cd_right, (sint16)spu.cd_right);
                }
                if (!spu.cd_mix_controlled || (reverb_control.control & 1u))
                {
                    mixed[sample * 2] += apply_volume(cd_left, (sint16)spu.cd_left);
                    mixed[sample * 2 + 1] += apply_volume(cd_right, (sint16)spu.cd_right);
                }
            }
            if (!cd_pcm_source && !xa_source && produced < count && libsnd.cd.stream.decoded_frames >= libsnd.cd.stream.total_frames)
            {
                libsnd.cd.active = 0;
                libsnd.cd_end_callback = libsnd.ready_callback;
            }
        }
        for (sample = produced; sample < count; ++sample)
        {
            spu_capture_sample(0, capture + sample, 0);
            spu_capture_sample(1, capture + sample, 0);
        }
        for (sample = 0; sample < count; ++sample)
        {
            if (sample >= synchronized)
            {
                sint32 wet_left, wet_right;
                spu_reverb_frame(sends[sample * 2], sends[sample * 2 + 1], &wet_left, &wet_right);
                mixed[sample * 2] += wet_left;
                mixed[sample * 2 + 1] += wet_right;
            }
            sint16 left = audio_sample_clamp(mixed[sample * 2]);
            sint16 right = audio_sample_clamp(mixed[sample * 2 + 1]);
            if (spu.native_muted)
                left = right = 0;
            interleaved_stereo[(offset + sample) * 2] = audio_sample_clamp(apply_volume(left, fixed_volume((uint16)spu.master_left)));
            interleaved_stereo[(offset + sample) * 2 + 1] = audio_sample_clamp(apply_volume(right, fixed_volume((uint16)spu.master_right)));
        }
        offset += count;
    }
#ifdef _DEBUG
    if (spu_mix_capture_file)
        fwrite(interleaved_stereo, sizeof(*interleaved_stereo) * 2u, frame_count, spu_mix_capture_file);
#endif
    if (libsnd.ticks_running)
    {
        libsnd.tick_accumulator += frame_count;
        while (libsnd.tick_accumulator >= SPU_SAMPLE_RATE / 60u)
        {
            libsnd.tick_accumulator -= SPU_SAMPLE_RATE / 60u;
            ++libsnd.tick_count;
        }
    }
    xport_audio_unlock();
}

void spu_render_vblank(sint16 *interleaved_stereo, uint32 frame_count)
{
    spu_render(interleaved_stereo, frame_count);
}





static FILE *cd_image;
static uint32 cd_image_sectors;
static uint32 cd_sector_cursor;
static uint32 cd_sector_offset;
static uint32 cd_image_byte_offset;
static uint32 cd_image_sector_bytes = 2352u;
static uint8 cd_data_mode;
static sint32 cd_read_error;
static uint8 cd_sector_buffer[2352];
static XPORT_CD_SOURCE cd_source;
static uint8 cd_data_fifo[2340];
static uint32 cd_data_offset;
static uint32 cd_data_length;
static void (*cd_command_service)(uint8, uint32, uint32);

static sint32 cd_media_present(void)
{
    return cd_image != NULL || cd_source.read_sector != NULL;
}

void cd_bind_command_native(void (*command)(uint8, uint32, uint32))
{
    cd_command_service = command;
}

static void cd_record_stream_command_locked(uint8 command)
{
    if (command == 3 || command == 6 || command == 27)
        cd_command.last_stream_command = command;
}

static void cd_notify_command(uint8 command)
{
    xport_audio_lock();
    cd_command.last_command = command;
    xport_audio_unlock();
    if (cd_command_service)
        cd_command_service(command, cd_sector_cursor, cd_data_mode);
}

static void cd_xa_stop_locked(void);

static void cd_unmount_image_locked(void)
{
    cd_xa_stop_locked();
    if (cd_image)
        fclose(cd_image);
    cd_image = NULL;
    if (cd_source.close)
        cd_source.close(cd_source.context);
    memset(&cd_source, 0, sizeof(cd_source));
    cd_image_sectors = cd_sector_cursor = cd_sector_offset = cd_image_byte_offset = 0u;
    cd_image_sector_bytes = 2352u;
    cd_read_error = 0;
    cd_data_offset = cd_data_length = 0u;
}

void cd_unmount_image(void)
{
    xport_audio_lock();
    cd_unmount_image_locked();
    xport_audio_unlock();
}

static char *cd_cue_field(char *line, const char *field)
{
    size_t size = strlen(field);
    while (*line == ' ' || *line == '\t')
        ++line;
    if (strncmp(line, field, size) || (line[size] != ' ' && line[size] != '\t'))
        return NULL;
    line += size;
    while (*line == ' ' || *line == '\t')
        ++line;
    return line;
}

static int cd_cue_number(char **cursor, uint32 *value)
{
    char *end;
    *value = strtoul(*cursor, &end, 10);
    if (end == *cursor)
        return 0;
    *cursor = end;
    return 1;
}

static sint32 cd_mount_cue_locked(const char *cue_path)
{
    FILE *image;
    char filename[768] = {0}, current_file[768] = {0}, image_path[1024];
    char *data, *line, *next, *field;
    const char *slash, *backslash;
    size_t directory, size, read_size;
    sint32 tracks = 0, mode2 = 0;
    uint32 minute = 0, second = 0, frame = 0, current_track = 0;
    sint64 bytes;
    if (!cue_path || !xport_file_size(cue_path, &size) || size == SIZE_MAX)
        return 0;
    data = (char *)malloc(size + 1u);
    if (!data)
        return 0;
    if (!xport_file_read(cue_path, data, size, &read_size) || read_size != size)
    {
        free(data);
        return 0;
    }
    data[size] = 0;
    for (line = data; line; line = next)
    {
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        field = strchr(line, '\r');
        if (field)
            *field = 0;
        if ((field = cd_cue_field(line, "FILE")) != NULL && *field == '"')
        {
            char *end = strchr(++field, '"');
            size_t length = end ? (size_t)(end - field) : 0;
            if (!end || length >= sizeof(current_file))
            {
                free(data);
                return 0;
            }
            memcpy(current_file, field, length);
            current_file[length] = 0;
        }
        if ((field = cd_cue_field(line, "TRACK")) != NULL)
        {
            uint32 track;
            char *end;
            if (!cd_cue_number(&field, &track))
            {
                free(data);
                return 0;
            }
            while (*field == ' ' || *field == '\t')
                ++field;
            end = field;
            while (*end && *end != ' ' && *end != '\t')
                ++end;
            *end = 0;
            current_track = track;
            if (track == 1 && !strcmp(field, "MODE2/2352"))
            {
                ++tracks;
                mode2 = 1;
                strcpy(filename, current_file);
            }
            else if (strcmp(field, "AUDIO"))
            {
                free(data);
                return 0;
            }
        }
        if (current_track == 1 && (field = cd_cue_field(line, "INDEX")) != NULL)
        {
            uint32 index;
            if (!cd_cue_number(&field, &index))
            {
                free(data);
                return 0;
            }
            if (index == 1)
            {
                if (!cd_cue_number(&field, &minute) || *field++ != ':' || !cd_cue_number(&field, &second) || *field++ != ':' || !cd_cue_number(&field, &frame) || minute >= 100 || second >= 60 || frame >= 75)
                {
                    free(data);
                    return 0;
                }
            }
        }
    }
    free(data);
    if (!filename[0] || tracks != 1 || !mode2)
        return 0;
    slash = strrchr(cue_path, '/');
    backslash = strrchr(cue_path, '\\');
    if (!slash || (backslash && backslash > slash))
        slash = backslash;
    directory = slash ? (size_t)(slash - cue_path + 1) : 0u;
    if (directory + strlen(filename) >= sizeof(image_path))
        return 0;
    memcpy(image_path, cue_path, directory);
    strcpy(image_path + directory, filename);
    image = xport_fopen(image_path, "rb");
    if (!image)
        return 0;
    if (fseek(image, 0, SEEK_END) != 0)
    {
        fclose(image);
        return 0;
    }
    bytes = ftell(image);

    if (bytes <= 0 || bytes % 2352 != 0 || (uint64)bytes / 2352u > UINT32_MAX)
    {
        fclose(image);
        return 0;
    }
    cd_unmount_image_locked();
    cd_image = image;
    cd_image_sectors = (uint32)((uint64)bytes / 2352u);
    cd_sector_offset = (uint32)(minute * 4500 + second * 75 + frame);
    if (cd_sector_offset >= cd_image_sectors)
    {
        cd_unmount_image_locked();
        return 0;
    }
    return 1;
}

sint32 cd_mount_cue(const char *cue_path)
{
    sint32 value;
    xport_audio_lock();
    value = cd_mount_cue_locked(cue_path);
    xport_audio_unlock();
    return value;
}

// Mount extracted CDXA or STR sectors through the same native transport
sint32 cd_mount_stream_file(const char *path)
{
    FILE *file;
    uint8 header[12], chunk[8];
    uint32 offset = 0u, size = 0u, sector_bytes = 2352u;
    sint64 bytes;
    sint32 valid = 0;

    if (!path || !(file = xport_fopen(path, "rb")))
        return 0;
    if (fseek(file, 0, SEEK_END) != 0)
        goto fail;
    bytes = ftell(file);

    if (bytes <= 0 || bytes > UINT32_MAX || fseek(file, 0, SEEK_SET) != 0 || xport_fread(header, 1u, sizeof(header), file) != sizeof(header))
        goto fail;
    if (memcmp(header, "RIFF", 4u) == 0 && memcmp(header + 8, "CDXA", 4u) == 0)
    {
        uint32 end = read_u32_le(header + 4);
        uint32 pos = 12u;
        if (end > UINT32_MAX - 8u || end + 8u > (uint32)bytes)
            goto fail;
        end += 8u;
        while (pos <= end && end - pos >= 8u)
        {
            if (fseek(file, (long)pos, SEEK_SET) != 0 || xport_fread(chunk, 1u, 8u, file) != 8u)
                goto fail;
            size = read_u32_le(chunk + 4);
            if (size > end - pos - 8u)
                goto fail;
            if (memcmp(chunk, "data", 4u) == 0)
            {
                offset = pos + 8u;
                valid = 1;
                break;
            }
            pos += 8u + size;
            if ((size & 1u) && pos == UINT32_MAX)
                goto fail;
            pos += size & 1u;
        }
    }
    else
    {
        size = (uint32)bytes;
        valid = header[0] == 0u && header[11] == 0u;
        if (header[0] == 0x60u && header[1] == 1u)
        {
            sector_bytes = 2048u;
            valid = 1;
        }
    }
    if (!valid || !size || size % sector_bytes != 0u)
        goto fail;
    xport_audio_lock();
    cd_unmount_image_locked();
    cd_image = file;
    cd_image_sectors = size / sector_bytes;
    cd_image_sector_bytes = sector_bytes;
    cd_image_byte_offset = offset;
    xport_audio_unlock();
    return 1;
fail:
    fclose(file);
    return 0;
}

sint32 xport_cd_mount_source(const XPORT_CD_SOURCE *source)
{
    XPORT_CD_SOURCE next;
    if (!source || !source->read_sector || !source->sectors)
        return 0;
    next = *source;
    xport_audio_lock();
    cd_unmount_image_locked();
    cd_source = next;
    cd_image_sectors = next.sectors;
    xport_audio_unlock();
    return 1;
}

static void cd_mode2_sector_header(uint32 sector)
{
    CdlLOC position;
    memset(cd_sector_buffer + 1, 0xFF, 10);
    CdIntToPos((sint32)sector, &position);
    memcpy(cd_sector_buffer + 12, &position, 3);
    cd_sector_buffer[15] = 2;
    cd_sector_buffer[18] = cd_sector_buffer[22] = 8;
}

static sint32 cd_fetch_sector(uint32 sector)
{
    uint64 offset = cd_image_byte_offset + ((uint64)sector + cd_sector_offset) * cd_image_sector_bytes;
    if (cd_source.read_sector)
    {
        uint32 sector_bytes = 0;
        if (sector >= cd_source.sectors)
            return 0;
        memset(cd_sector_buffer, 0, sizeof(cd_sector_buffer));
        if (!cd_source.read_sector(cd_source.context, sector, cd_sector_buffer, &sector_bytes))
            return 0;
        if (sector_bytes == 2048)
            cd_mode2_sector_header(sector);
        else if (sector_bytes != 2352)
            return 0;
        return cd_sector_buffer[0] == 0 && cd_sector_buffer[11] == 0 && cd_sector_buffer[15] == 2;
    }
    if (!cd_image || (uint64)sector + cd_sector_offset >= cd_image_sectors)
        return 0;
    if (offset > 0x7fffffffu || fseek(cd_image, (long)offset, SEEK_SET) != 0)
        return 0;

    if (cd_image_sector_bytes == 2048u)
    {
        memset(cd_sector_buffer, 0, sizeof(cd_sector_buffer));
        cd_mode2_sector_header(sector);
        if (xport_fread(cd_sector_buffer + 24, 1u, 2048u, cd_image) != 2048u)
            return 0;
    }
    else if (xport_fread(cd_sector_buffer, 1u, sizeof(cd_sector_buffer), cd_image) != sizeof(cd_sector_buffer))
        return 0;
    return cd_sector_buffer[0] == 0 && cd_sector_buffer[11] == 0 && cd_sector_buffer[15] == 2;
}

static uint32 cd_le32(const uint8 *bytes)
{
    return bytes[0] | ((uint32)bytes[1] << 8) | ((uint32)bytes[2] << 16) | ((uint32)bytes[3] << 24);
}

static sint32 cd_name_matches(const uint8 *name, uint32 length, const char *part, size_t part_length)
{
    uint32 i;
    if (!memchr(part, ';', part_length))
        for (i = 0u; i < length; ++i)
            if (name[i] == ';')
            {
                length = i;
                break;
            }
    if (length != part_length)
        return 0;
    for (i = 0u; i < length; ++i)
    {
        uint8 left = name[i], right = (uint8)part[i];
        if (left >= 'a' && left <= 'z')
            left -= 'a' - 'A';
        if (right >= 'a' && right <= 'z')
            right -= 'a' - 'A';
        if (left != right)
            return 0;
    }
    return 1;
}

static void *cd_search_file_native_locked(void *result, const char *path)
{
    uint8 record[256], *output = (uint8 *)result;
    uint32 extent, directory_size, sector, offset, remaining, length, found;
    uint32 file_size = 0u;
    const char *part, *end, *last = NULL;
    size_t part_length = 0u;
    CdlLOC position = {0, 0, 0, 0};
    if (cd_source.read_sector)
        return result && path && cd_source.search_file ? cd_source.search_file(cd_source.context, result, path) : NULL;
    if (!result || !path || !cd_fetch_sector(16u))
        return NULL;
    if (cd_sector_buffer[24] != 1u || memcmp(cd_sector_buffer + 25u, "CD001", 5u) != 0)
        return NULL;
    memcpy(record, cd_sector_buffer + 24u + 156u, 34u);
    extent = cd_le32(record + 2u);
    directory_size = cd_le32(record + 10u);
    part = path;
    while (*part == '/' || *part == '\\')
        ++part;
    if (!*part)
        return NULL;
    for (;;)
    {
        end = part;
        while (*end && *end != '/' && *end != '\\')
            ++end;
        part_length = (size_t)(end - part);
        if (!part_length || part_length > 255u)
            return NULL;
        found = 0u;
        for (sector = 0u; (uint64)sector * 2048u < directory_size && !found; ++sector)
        {
            if (!cd_fetch_sector(extent + sector))
                return NULL;
            remaining = directory_size - sector * 2048u;
            if (remaining > 2048u)
                remaining = 2048u;
            for (offset = 0u; offset < remaining; offset += length)
            {
                const uint8 *entry = cd_sector_buffer + 24u + offset;
                length = entry[0];
                if (!length)
                    break;
                if (length < 34u || length > remaining - offset || entry[32] > length - 33u)
                    return NULL;
                if (cd_name_matches(entry + 33u, entry[32], part, part_length))
                {
                    memcpy(record, entry, length);
                    found = 1u;
                    break;
                }
            }
        }
        if (!found)
            return NULL;
        extent = cd_le32(record + 2u);
        file_size = cd_le32(record + 10u);
        last = part;
        while (*end == '/' || *end == '\\')
            ++end;
        if (!*end)
            break;
        if ((record[25] & 2u) == 0u)
            return NULL;
        directory_size = file_size;
        part = end;
    }
    if ((record[25] & 2u) || extent > 0x7fffffffu || !CdIntToPos((sint32)extent, &position))
        return NULL;
    memcpy(output, &position, 4u);
    output[4] = (uint8)file_size;
    output[5] = (uint8)(file_size >> 8);
    output[6] = (uint8)(file_size >> 16);
    output[7] = (uint8)(file_size >> 24);
    memset(output + 8u, 0, 16u);
    if (part_length > 15u)
        part_length = 15u;
    memcpy(output + 8u, last, part_length);
    return result;
}

void *cd_search_file_native(void *result, const char *path)
{
    void *value;
    xport_audio_lock();
    value = cd_search_file_native_locked(result, path);
    xport_audio_unlock();
    return value;
}

static sint32 cd_bcd(uint8 value)
{
    if ((value & 15u) > 9u || (value >> 4) > 9u)
        return -1;
    return (value >> 4) * 10 + (value & 15u);
}

static sint32 cd_set_location(const uint8 *position)
{
    sint32 m, s, f, sector;
    if (!position)
        return 0;
    m = cd_bcd(position[0]);
    s = cd_bcd(position[1]);
    f = cd_bcd(position[2]);
    if (m < 0 || s < 0 || s >= 60 || f < 0 || f >= 75)
        return 0;
    sector = m * 4500 + s * 75 + f - 150;
    if (sector < 0 || !cd_media_present() || (uint64)(uint32)sector + cd_sector_offset >= cd_image_sectors)
        return 0;
    cd_sector_cursor = (uint32)sector;
    cd_data_offset = cd_data_length = 0u;
    return 1;
}

/* Included after cd_fetch_sector and cd_set_location definitions */
#define CD_XA_PCM_FRAMES 32768u
#define CD_XA_DATA_SECTORS 64u

typedef struct CD_XA_STATE
{
    XaState decoder;
    sint16 pcm[CD_XA_PCM_FRAMES * 2u];
    uint8 data[CD_XA_DATA_SECTORS][2352];
    uint8 header[8];
    uint32 pcm_read, pcm_write, pcm_count;
    uint32 data_read, data_write, data_count;
    uint32 credit;
    uint32 underruns;
    sint32 reading, consumer, header_valid, error, identity_valid;
    uint8 identity_file, identity_channel;
} CD_XA_STATE;

static CD_XA_STATE cd_xa;











static void cd_xa_stop_locked(void)
{
    sint32 consumer = cd_xa.consumer;
    memset(&cd_xa, 0, sizeof(cd_xa));
    cd_xa.consumer = consumer;
    xa_reset(&cd_xa.decoder);
}

sint32 spu_take_xa_pcm(XportFmvAudioRemainder *audio)
{
    sint32 taken = 0;

    if (!audio)
        return 0;
    xport_audio_lock();
    if (xa_remainder_ready)
    {
        *audio = xa_remainder;
        xa_remainder_ready = 0;
        taken = 1;
    }
    xport_audio_unlock();
    return taken;
}

void spu_discard_xa_pcm(void)
{
    xport_audio_lock();
    xa_remainder_ready = 0;
    xport_audio_unlock();
}

sint32 spu_save_xa_pcm(void)
{
    XportFmvAudioRemainder *audio = &xa_remainder;
    uint32 first;
    sint32 taken = 0;

    xport_audio_lock();
    xa_remainder_ready = 0;
    if (!cd_pcm_source && cd_image && !cd_source.read_sector && !cd_command_service && cd_image_sector_bytes == 2352 && cd_image_byte_offset == 0 && cd_sector_offset == 0 && cd_sector_cursor <= cd_image_sectors && !cd_read_error && (!cd_xa.error || cd_sector_cursor == cd_image_sectors) && !libsnd.cd.paused && cd_xa.identity_valid && cd_xa.pcm_read < CD_XA_PCM_FRAMES && cd_xa.pcm_count <= CD_XA_PCM_FRAMES && cd_xa.pcm_count <= XPORT_FMV_AUDIO_FRAMES)
    {
        memset(audio, 0, sizeof(*audio));
        audio->decoder = cd_xa.decoder;
        audio->pcm_frames = cd_xa.pcm_count;
        audio->next_sector = cd_sector_cursor;
        audio->file = cd_xa.identity_file;
        audio->channel = cd_xa.identity_channel;
        first = CD_XA_PCM_FRAMES - cd_xa.pcm_read;
        if (first > cd_xa.pcm_count)
            first = cd_xa.pcm_count;
        memcpy(audio->pcm, cd_xa.pcm + cd_xa.pcm_read * 2, first * 2 * sizeof(sint16));
        memcpy(audio->pcm + first * 2, cd_xa.pcm, (cd_xa.pcm_count - first) * 2 * sizeof(sint16));
        cd_xa_stop_locked();
        xa_remainder_ready = 1;
        taken = 1;
    }
    xport_audio_unlock();
    return taken;
}

static void cd_xa_set_reading_locked(sint32 reading)
{
    cd_xa.reading = reading;
}

static sint32 cd_xa_active_locked(void)
{
    return (cd_xa.reading && (!cd_command_service || (libsnd.cd_mode & 0x40))) || cd_xa.pcm_count != 0u;
}

static sint32 cd_xa_accept_sector_locked(void)
{
    /* Transient workspace is protected by the audio lock */
    static sint16 decoded[XA_MAX_SECTOR_FRAMES * 2u];
    uint32 frames, i;
    sint32 audio;
    if (CD_XA_PCM_FRAMES - cd_xa.pcm_count < XA_MAX_SECTOR_FRAMES)
        return 0;
    if (cd_xa.consumer && cd_xa.data_count == CD_XA_DATA_SECTORS)
        return 0;
    memcpy(cd_xa.header, cd_sector_buffer + 12u, 8u);
    cd_xa.header_valid = 1;
    audio = cd_sector_buffer[15] == 2u && (cd_sector_buffer[18] & 4u) && (libsnd.cd_mode & 0x40u);
    if (audio)
    {
        if ((libsnd.cd_mode & 8u) && (cd_sector_buffer[16] != libsnd.cd_filter_file || cd_sector_buffer[17] != libsnd.cd_filter_channel))
            return 1;
        if (cd_xa.identity_valid && (cd_xa.identity_file != cd_sector_buffer[16] || cd_xa.identity_channel != cd_sector_buffer[17]))
            return 1;
        cd_xa.identity_valid = 1;
        cd_xa.identity_file = cd_sector_buffer[16];
        cd_xa.identity_channel = cd_sector_buffer[17];
        if (cd_xa.identity_valid && (cd_xa.identity_file != cd_sector_buffer[16] || cd_xa.identity_channel != cd_sector_buffer[17]))
            return 1;
        cd_xa.identity_valid = 1;
        cd_xa.identity_file = cd_sector_buffer[16];
        cd_xa.identity_channel = cd_sector_buffer[17];
        if (cd_xa.identity_valid && (cd_xa.identity_file != cd_sector_buffer[16] || cd_xa.identity_channel != cd_sector_buffer[17]))
            return 1;
        cd_xa.identity_valid = 1;
        cd_xa.identity_file = cd_sector_buffer[16];
        cd_xa.identity_channel = cd_sector_buffer[17];
        if (cd_xa.identity_valid && (cd_xa.identity_file != cd_sector_buffer[16] || cd_xa.identity_channel != cd_sector_buffer[17]))
            return 1;
        cd_xa.identity_valid = 1;
        cd_xa.identity_file = cd_sector_buffer[16];
        cd_xa.identity_channel = cd_sector_buffer[17];
        if (xa_decode_sector(&cd_xa.decoder, cd_sector_buffer, decoded, XA_MAX_SECTOR_FRAMES, &frames) != XA_OK)
        {
#ifndef NDEBUG
            fprintf(stderr, "Unsupported or malformed XA sector at LBA %u, coding %02X\n", cd_sector_cursor - 1u, cd_sector_buffer[19]);
#endif
            abort();
        }
        for (i = 0; i < frames; ++i)
        {
            cd_xa.pcm[cd_xa.pcm_write * 2u] = decoded[i * 2u];
            cd_xa.pcm[cd_xa.pcm_write * 2u + 1u] = decoded[i * 2u + 1u];
            cd_xa.pcm_write = (cd_xa.pcm_write + 1u) % CD_XA_PCM_FRAMES;
        }
        cd_xa.pcm_count += frames;
    }
    else if (cd_xa.consumer)
    {
        memcpy(cd_xa.data[cd_xa.data_write], cd_sector_buffer, 2352u);
        cd_xa.data_write = (cd_xa.data_write + 1u) % CD_XA_DATA_SECTORS;
        ++cd_xa.data_count;
    }
    return 1;
}

static sint32 cd_xa_sector_locked(void)
{
    if (CD_XA_PCM_FRAMES - cd_xa.pcm_count < XA_MAX_SECTOR_FRAMES || (cd_xa.consumer && cd_xa.data_count == CD_XA_DATA_SECTORS))
        return 0;
    if (!cd_fetch_sector(cd_sector_cursor))
    {
        cd_xa.reading = 0;
        cd_xa.error = 1;
        return -1;
    }
    ++cd_sector_cursor;
    return cd_xa_accept_sector_locked();
}

void cd_set_reading_native(sint32 reading)
{
    xport_audio_lock();
    cd_xa.reading = reading != 0;
    libsnd.cd.paused = !reading;
#if defined(XPORT_NATIVE_CD) && XPORT_NATIVE_CD
    cd_command.last_command = reading ? 27 : CdlPause;
#endif
    xport_audio_unlock();
}

static uint32 cd_xa_render_locked(sint16 *output, uint32 count)
{
    uint32 i;
    for (i = 0; i < count; ++i)
    {
        if (cd_xa.reading && !libsnd.cd.paused && !cd_command_service)
        {
            if (cd_xa.credit < 44100u)
                if (cd_xa.credit < 44100u)
                    if (cd_xa.credit < 44100u)
                        if (cd_xa.credit < 44100u)
                            cd_xa.credit += (libsnd.cd_mode & 0x80u) ? 150u : 75u;
            while (cd_xa.credit >= 44100u)
            {
                if (cd_xa_sector_locked() <= 0)
                    break;
                cd_xa.credit -= 44100u;
            }
        }
        output[i * 2u] = output[i * 2u + 1u] = 0;
        if (cd_xa.pcm_count && !libsnd.cd.paused)
        {
            if (!libsnd.cd.muted)
            {
                output[i * 2u] = cd_xa.pcm[cd_xa.pcm_read * 2u];
                output[i * 2u + 1u] = cd_xa.pcm[cd_xa.pcm_read * 2u + 1u];
            }
            cd_xa.pcm_read = (cd_xa.pcm_read + 1u) % CD_XA_PCM_FRAMES;
            --cd_xa.pcm_count;
        }
        else if (cd_xa.reading && !libsnd.cd.paused)
            ++cd_xa.underruns;
    }
    return count;
}

void cd_stream_consumer_native(sint32 enabled)
{
    xport_audio_lock();
    cd_xa.consumer = enabled != 0;
    if (!enabled)
        cd_xa.data_read = cd_xa.data_write = cd_xa.data_count = 0;
    xport_audio_unlock();
}

sint32 cd_stream_pop_sector_native(uint8 output[2352])
{
    sint32 result = 0;
    if (!output)
        return -1;
    xport_audio_lock();
    if (cd_xa.data_count)
    {
        memcpy(output, cd_xa.data[cd_xa.data_read], 2352u);
        cd_xa.data_read = (cd_xa.data_read + 1u) % CD_XA_DATA_SECTORS;
        --cd_xa.data_count;
        result = 1;
    }
    else if (cd_xa.error)
        result = -1;
    xport_audio_unlock();
    return result;
}

sint32 cd_restore_fifo_native(const uint8 *bytes, uint32 count, uint32 mode, uint32 next_sector)
{
    if (count > sizeof(cd_data_fifo) || (count && !bytes))
        return 0;
    xport_audio_lock();
    if (count)
        memcpy(cd_data_fifo, bytes, count);
    cd_data_offset = 0;
    cd_data_length = count;
    cd_data_mode = (uint8)mode;
    libsnd.cd_mode = (uint8)mode;
    cd_sector_cursor = next_sector;
    xport_audio_unlock();
    return 1;
}

sint32 cd_read_sector_fifo_native(uint32 sector, uint32 mode)
{
    sint32 ok;
    xport_audio_lock();
    ok = cd_fetch_sector(sector);
    if (ok)
    {
        cd_data_mode = (uint8)mode;
        cd_data_length = (mode & 0x20u) ? 2340u : 2048u;
        memcpy(cd_data_fifo, cd_sector_buffer + ((mode & 0x20u) ? 12u : 24u), cd_data_length);
        cd_data_offset = 0;
        cd_sector_cursor = sector + 1u;
        if (cd_command_service)
        {
            libsnd.cd_mode = (uint8)mode;
            ok = cd_xa_accept_sector_locked();
            if (ok && cd_sector_buffer[15] == 2 && (cd_sector_buffer[18] & 4) && (mode & 0x40))
            {
                cd_data_offset = cd_data_length = 0;
                ok = 2;
            }
        }
    }
    xport_audio_unlock();
    return ok;
}

static sint32 CdGetSector_locked(void *destination, sint32 words)
{
    uint32 bytes;
    if (!destination || words < 0 || words > 585)
        return 0;
    bytes = (uint32)words * 4u;
    if (cd_data_offset == cd_data_length)
    {
        if (cd_command_service)
            return 0;
        if (!cd_fetch_sector(cd_sector_cursor))
            return 0;
        cd_data_length = (cd_data_mode & 0x20u) ? 2340u : 2048u;
        memcpy(cd_data_fifo, cd_sector_buffer + ((cd_data_mode & 0x20u) ? 12u : 24u), cd_data_length);
        cd_data_offset = 0u;
        ++cd_sector_cursor;
    }
    if (bytes > cd_data_length - cd_data_offset)
        return 0;
    memcpy(destination, cd_data_fifo + cd_data_offset, bytes);
    cd_data_offset += bytes;
    return 1;
}

sint32 CdGetSector(void *destination, sint32 words)
{
    sint32 value;
    xport_audio_lock();
    value = CdGetSector_locked(destination, words);
    xport_audio_unlock();
    return value;
}

sint32 CD_getsector(uint32 guest_destination, uint32 words)
{
    // Preserve the established guest entry's boolean result
    if (words > 585u)
        return 0;
    return CdGetSector(psx_addr(guest_destination, words * 4u), (sint32)words);
}

static sint32 CdRead_locked(sint32 count, uint32 *destination, sint32 mode)
{
    sint32 index;
    uint32 bytes;

    cd_xa_stop_locked();
    cd_read_error = 0;
    if (count <= 0 || !destination || !cd_media_present())
    {
        cd_read_error = 1;
        return 0;
    }
    cd_data_mode = (uint8)mode;
    cd_data_offset = cd_data_length = 0u;
    bytes = (cd_data_mode & 0x20u) ? 2340u : 2048u;
    if ((uint32)count > UINT32_MAX / bytes)
    {
        cd_read_error = 1;
        return 0;
    }
    for (index = 0; index < count; ++index)
    {
        if (!CdGetSector_locked((uint8 *)destination + (uint32)index * bytes, (sint32)(bytes / 4u)))
        {
            cd_read_error = 1;
            return 0;
        }
    }
    return 1;
}

sint32 CdRead(sint32 count, uint32 *destination, sint32 mode)
{
    sint32 value;
    uint32 bytes;
    int started, notify;
    uint8 response[8];
    xport_audio_lock();
    bytes = (mode & 0x20) != 0 ? 2340u : 2048u;
    started = count > 0 && destination != NULL && cd_media_present() && (uint32)count <= UINT32_MAX / bytes;
    if (started)
        cd_record_stream_command_locked(6);
    value = CdRead_locked(count, destination, mode);
    notify = started && libsnd.read_callback != NULL;
    memcpy(response, cd_command.response, sizeof(response));
    response[0] = cd_read_error ? 1u : 2u;
    xport_audio_unlock();
    if (notify)
        cd_queue_callback(CD_CALLBACK_READ, value != 0 ? CdlComplete : CdlDiskError, response);
    return value;
}

sint32 CdSync(sint32 mode, uint8 *result)
{
    return CD_sync(mode, result);
}

void CdFlush(void)
{
    xport_audio_lock();
    cd_data_offset = cd_data_length = 0u;
    cd_command.completion = CdlComplete;
    memset(cd_command.response, 0, sizeof(cd_command.response));
    xport_audio_unlock();
}

sint32 CdPosToInt(CdlLOC *position)
{
    const uint8 *bytes = (const uint8 *)position;
    sint32 minute = (bytes[0] >> 4) * 10 + (bytes[0] & 15);
    sint32 second = (bytes[1] >> 4) * 10 + (bytes[1] & 15);
    sint32 frame = (bytes[2] >> 4) * 10 + (bytes[2] & 15);
    return (minute * 60 + second) * 75 + frame - 150;
}

static sint32 CdGetDiskType_locked(void)
{
    sint32 result;

    cd_xa_stop_locked();
    if (!cd_media_present())
        return 0x10;
    if (cd_source.read_sector)
        return 2;
    if (!cd_fetch_sector(16u))
        return 1;
    result = memcmp(cd_sector_buffer + 25u, "CD001", 5u) == 0 ? 2 : 1;
    cd_sector_cursor = 17u;
    cd_data_offset = cd_data_length = 0u;
    libsnd.cd.paused = 1;
    return result;
}

sint32 CdGetDiskType(void)
{
    sint32 value;
    xport_audio_lock();
    value = CdGetDiskType_locked();
    xport_audio_unlock();
    return value;
}

sint32 CdReadSync(sint32 mode, uint8 *result)
{
    (void)mode;
    if (result)
        result[0] = cd_read_error ? 1u : 2u;
    return cd_read_error ? -1 : 0;
}

static void cd_initialize_audio_device(void)
{
    xport_audio_lock();
    if (!spu.initialized)
        spu_init();
    spu_synchronize_device_clock();
    /* Fixed master volumes determine the current volume registers in this backend */
    if (!fixed_volume((uint16)spu.master_left) && !fixed_volume((uint16)spu.master_right))
    {
        spu.master_left = spu.master_right = 0x3fff;
    }
    spu.cd_left = spu.cd_right = 0x3fff;
    reverb_control.control = 0xc001u;
    reverb_control.enabled = 0;
    spu.cd_mix_controlled = 1;
    /* MMIO reads and the mixer share these device fields */
    xport_audio_unlock();
    xport_audio_init();
}

static sint32 CdInit_locked(void)
{
    cd_xa_stop_locked();
    cd_command.last_stream_command = 0;

    if (!cd_media_present())
        return 0;
    cd_sector_cursor = 0u;
    cd_data_offset = cd_data_length = 0u;
    cd_data_mode = 0u;
    cd_read_error = 0;
    return 1;
}

sint32 CdInit(void)
{
    sint32 value;
#if defined(XPORT_NATIVE_CD) && XPORT_NATIVE_CD
    sint32 attempt;
    for (attempt = 4; attempt >= 0; --attempt)
    {
        xport_audio_lock();
        value = CdInit_locked();
        xport_audio_unlock();
        if (value == 1)
            break;
    }
    if (!value)
    #ifndef NDEBUG
        fprintf(stderr, "CdInit: Init failed\n");
    #else
        (void)0;
    #endif
#else
    xport_audio_lock();
    value = CdInit_locked();
    xport_audio_unlock();
#endif
    if (value)
    {
        cd_initialize_audio_device();
    }
    return value;
}

static sint32 cd_reset_native_locked(sint32 mode)
{
    cd_xa_stop_locked();
    cd_command.last_stream_command = 0;

    (void)mode;
    cd_read_error = 0;
    return cd_media_present();
}

sint32 cd_reset_native(sint32 mode)
{
    sint32 value;
    xport_audio_lock();
    value = cd_reset_native_locked(mode);
    xport_audio_unlock();
    if (value && mode == 1)
        cd_initialize_audio_device();
    return value;
}

CdlLOC *CdIntToPos(sint32 sector, CdlLOC *position)
{
    uint32 bits = (uint32)sector + 150u;
    sint32 value = bits <= 0x7FFFFFFFu ? (sint32)bits : -1 - (sint32)~bits;
    sint32 seconds = value / 75;
    sint32 minute = seconds / 60;
    sint32 second = seconds % 60;
    sint32 frame = value % 75;
    uint8 *bytes = (uint8 *)position;
    if (!position)
        return NULL;
    bytes[2] = (uint8)((frame / 10) * 16 + frame % 10);
    bytes[1] = (uint8)((second / 10) * 16 + second % 10);
    bytes[0] = (uint8)((minute / 10) * 16 + minute % 10);
    return position;
}

sint32 CdDataSync(sint32 mode)
{
    (void)mode;
    if (cd_guest.context)
    {
        cd_sdk_timeout_begin(cd_guest.strings + 264);
        if (cd_sdk_timeout_poll())
            return -1;
    }
    // Native CD DMA finishes before CdGetSector returns
    return 0;
}

uint32 xport_guest_buffer_address(void *host_buffer, size_t bytes)
{
    uintptr_t base = (uintptr_t)psx_addr(0x80000000u, 0x200000u);
    uintptr_t pointer = (uintptr_t)host_buffer;
    if (pointer < base || pointer - base > 0x200000u || bytes > 0x200000u - (pointer - base))
        return 0u;
    return 0x80000000u + (uint32)(pointer - base);
}

static void (*cd_native_guest_dispatch)(uint32, uint32, uint32);

void cd_bind_guest_dispatch_native(void (*dispatch)(uint32, uint32, uint32))
{
    cd_native_guest_dispatch = dispatch;
}

sint32 xport_guest_call2(uint32 guest_target, uint32 argument0, uint32 argument1)
{
    if (!cd_native_guest_dispatch)
        return (sint32)psx_dispatch_guest_call(guest_target, 2, argument0, argument1, 0);
    cd_native_guest_dispatch(guest_target, argument0, argument1);
    return 0;
}

uint32 xport_cd_data_guest_callback(uint32 guest_callback)
{
    (void)guest_callback;
    abort();
}

uint32 xport_cd_sync_guest_callback(uint32 guest_callback)
{
    (void)guest_callback;
    abort();
}

uint32 xport_cd_ready_guest_callback(uint32 guest_callback)
{
    (void)guest_callback;
    abort();
}

sint32 CD_vol(CdlATV *volume)
{
    return CdMix(volume);
}

// Status byte already published by the native controller Nop response
static uint8 cd_native_status_locked(void)
{
    return (uint8)((cd_media_present() ? 2u : 0u) | (cd_xa.reading ? 0x20u : 0u) | (cd_xa.error ? 1u : 0u));
}

sint32 CdControlBPSX(uint8 command, uint8 *parameter, uint8 *result)
{
    sint32 ok = 0;
    if (command != 1u && command != 2u && command != 6u && command != 14u && command != 16u && command != 17 && command != 21u && command != 22u && command != 27u)
        return CdControl(command, parameter, result);
    xport_audio_lock();
    if (command == 1u)
    {
        ok = cd_media_present();
        if (result)
            result[0] = cd_native_status_locked();
    }
    else if (command == 16u)
    {
        ok = cd_xa.header_valid;
        if (result)
        {
            if (ok)
                memcpy(result, cd_xa.header, 8u);
            else
            {
                result[0] = 1u;
                result[1] = 0x80u;
            }
        }
    }
    else if (command == 17)
    {
        // The mounted image is the data track at index 1
        ok = cd_media_present();
        if (ok && result)
        {
            CdlLOC absolute, relative;
            sint32 sector = (sint32)cd_sector_cursor - (cd_xa.header_valid && cd_sector_cursor != 0);
            CdIntToPos(sector, &absolute);
            CdIntToPos(sector - 150, &relative);
            result[0] = 1;
            result[1] = 1;
            memcpy(result + 2, &relative, 3);
            memcpy(result + 5, &absolute, 3);
        }
        else if (result)
        {
            result[0] = 1;
            result[1] = 0x80;
        }
    }
    else if (command == 14u)
    {
        if (parameter && cd_media_present())
        {
            cd_data_mode = libsnd.cd_mode = parameter[0];
            ok = 1;
        }
    }
    else
    {
        if (command == 2u || parameter)
        {
            if (command != 2)
                cd_xa_stop_locked();
            ok = cd_set_location(parameter);
        }
        else
            ok = cd_media_present();
        if (ok && (command == 6u || command == 27u))
        {
            cd_xa_stop_locked();
            cd_xa.reading = 1;
            libsnd.cd.paused = 0;
        }
    }
    cd_read_error = !ok;
    if (result && command != 1u && command != 16u && command != 17)
        result[0] = ok ? 2u : 1u;
    if (ok)
        cd_record_stream_command_locked(command);
    xport_audio_unlock();
    if (ok)
        cd_notify_command(command);
    return ok;
}

/* Native streaming consumes exactly one physical Mode 2 sector */
sint32 cd_read_sector_native(uint8 output[2352])
{
    return cd_stream_pop_sector_native(output);
}

uint32 xport_cd_controller_read_missing(uint32 address, uint32 width)
{
    (void)address;
    (void)width;
    abort();
}

void xport_cd_controller_write_missing(uint32 address, uint32 width, uint32 value)
{
    (void)address;
    (void)width;
    (void)value;
    abort();
}

uint32 CDREAD_OBJ_32CPSX(uint32 retry)
{
    (void)retry;
    abort();
}

uint32 spu_sequence_scheduler_set_table(SpuSequenceScheduler *scheduler, SpuSequenceSchedulerTrack *table, sint16 sequences, sint16 separations)
{
    sint32 sequence;
    uint32 record_index = 0;
    scheduler->sequence_count = (uint16)(sequences);
    scheduler->separation_count = (uint16)(separations);
    for (sequence = 0; sequence < sequences; ++sequence)
    {
        scheduler->tracks[(uint32)sequence] = table + record_index;
        record_index += (uint32)(sint32)separations;
    }
    if ((sint16)scheduler->sequence_count <= 0)
        return (uint32)(sint32)(sint16)scheduler->sequence_count;
    sequence = 0;
    do
    {
        sint32 track = 0;
        uint32 track_index = 0;
        SpuSequenceSchedulerTrack **slot = &scheduler->tracks[(uint32)sequence];
        if ((sint16)scheduler->separation_count > 0)
        {
            sint32 more;
            do
            {
                SpuSequenceSchedulerTrack *entry;
                ((*slot) + track_index)->flags = (uint32)(0);
                ((*slot) + track_index)->next_sequence = (uint8)(255);
                ((*slot) + track_index)->next_separation = (uint8)(0);
                ((*slot) + track_index)->volume_delta = (uint16)(0);
                ((*slot) + track_index)->volume_remaining = (uint16)(0);
                ((*slot) + track_index)->volume_duration = (uint32)(0);
                ((*slot) + track_index)->volume_countdown = (uint32)(0);
                ((*slot) + track_index)->volume_period = (uint16)(0);
                ((*slot) + track_index)->target_tempo = (uint32)(0);
                ((*slot) + track_index)->tempo_countdown = (uint32)(0);
                ((*slot) + track_index)->tempo_duration = (uint32)(0);
                ((*slot) + track_index)->tempo_period = (uint16)(0);
                ((*slot) + track_index)->left_volume = (uint16)(127);
                ((*slot) + track_index)->right_volume = (uint16)(127);
                ++track;
                ((*slot) + track_index)->saved_left_volume = (uint16)(127);
                entry = (*slot) + track_index;
                ++track_index;
                more = track < (sint16)scheduler->separation_count;
                entry->saved_right_volume = (uint16)(127);
            } while (more);
        }
        ++sequence;
    } while (sequence < (sint16)scheduler->sequence_count);
    return 0;
}

uint32 spu_sequence_scheduler_play_tick(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, SpuSequenceHandler read_event)
{
    SpuSequenceSchedulerTrack *record;
    uint32 ticks, result, accumulated;
    sint32 duration, countdown, delta;
    record = scheduler->tracks[(uint32)(sint32)sequence] + (uint32)(sint32)separation;
    duration = (sint16)record->tick_period;
    ticks = record->delta;
    delta = (sint32)(ticks - (uint32)duration);
    if (delta > 0)
    {
        countdown = (sint16)record->tick_countdown;
        result = (uint32)countdown;
        if (countdown > 0)
        {
            --result;
            record->tick_countdown = (uint16)(result);
            return result;
        }
        if (countdown != 0)
        {
            record->delta = (uint32)((uint32)delta);
            return result;
        }
        result = record->delta - 1;
        record->tick_countdown = (uint16)(duration);
    }
    else
    {
        result = duration < (sint32)ticks;
        accumulated = ticks;
        if (result != 0)
            return result;
        do
        {
            do
            {
                read_event(sequence, separation);
                ticks = record->delta;
            } while (ticks == 0);
            duration = (sint16)record->tick_period;
            accumulated += ticks;
            result = accumulated - (uint32)duration;
        } while ((sint32)accumulated < duration);
    }
    record->delta = (uint32)(result);
    return result;
}

uint32 spu_sequence_scheduler_crescendo(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, SpuSequenceVolumeGet get_volume, SpuSequenceVolumeSet set_volume)
{
    SpuSequenceSchedulerTrack **slot = &scheduler->tracks[(uint32)(sint32)sequence];
    uint32 track_index = (uint32)(sint32)separation;
    SpuSequenceSchedulerTrack *record = (*slot) + track_index;
    uint32 countdown = record->volume_countdown - 1;
    sint32 period = (sint16)record->volume_period;
    sint16 packed = (sint16)((uint32)(sint32)sequence | ((uint32)(sint32)separation << 8));
    SpuSequenceSchedulerTrack *flag_record;
    uint16 local[2];
    uint32 left, right, value, elapsed, result;
    sint32 remaining;
    record->volume_countdown = (uint32)(countdown);
    if (period > 0)
    {
        if (countdown % (uint32)period != 0)
            goto volume_up_return;
        value = record->volume_remaining - 1;
        record->volume_remaining = (uint16)(value);
        if ((sint16)value < 0)
            goto volume_up_reset;
        get_volume(packed, &local[0], &local[1]);
        left = local[0];
        remaining = (sint16)record->volume_remaining;
        if ((sint32)left + remaining >= (sint32)left + 1)
        {
            left = local[0];
            right = local[1];
            set_volume(packed, (sint16)(left + 1), (sint16)(right + 1), 1);
        }
    }
    else if (period < 0)
    {
        value = record->volume_remaining + (uint32)period;
        record->volume_remaining = (uint16)(value);
        if ((sint16)value < 0)
            goto volume_up_reset;
        get_volume(packed, &local[0], &local[1]);
        left = local[0];
        period = (sint16)record->volume_period;
        if ((sint32)left - period >= 127)
        {
            right = local[1];
            if ((sint32)right - period >= 127)
                set_volume(packed, 127, 127, 1);
        }
        elapsed = record->volume_duration - record->volume_countdown;
        period = (sint16)record->volume_period;
        value = (uint32)(sint32)(sint16)record->volume_delta;
        if (elapsed * (0u - (uint32)period) < value)
        {
            left = local[0];
            right = local[1];
            set_volume(packed, (sint16)(left - (uint32)period), (sint16)(right - (uint32)period), 1);
        }
    }
    else
        goto volume_up_return;
    goto volume_up_finish;
volume_up_reset:
    set_volume(packed, 127, 127, 1);
    flag_record = (*slot) + track_index;
    flag_record->flags = (uint32)(flag_record->flags & ~0x10);
volume_up_finish:
    if (record->volume_countdown == 0 || (sint16)record->volume_remaining == 0)
    {
        flag_record = (*slot) + track_index;
        flag_record->flags = (uint32)(flag_record->flags & ~0x10);
    }
volume_up_return:
    result = get_volume(packed, &record->saved_left_volume, &record->saved_right_volume);
    return result;
}

uint32 spu_sequence_scheduler_decrescendo(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, SpuSequenceVolumeGet get_volume, SpuSequenceVolumeSet set_volume)
{
    SpuSequenceSchedulerTrack **slot = &scheduler->tracks[(uint32)(sint32)sequence];
    uint32 track_index = (uint32)(sint32)separation;
    SpuSequenceSchedulerTrack *record = (*slot) + track_index;
    uint32 countdown = record->volume_countdown - 1;
    sint32 period = (sint16)record->volume_period;
    sint16 packed = (sint16)((uint32)(sint32)sequence | ((uint32)(sint32)separation << 8));
    SpuSequenceSchedulerTrack *flag_record;
    uint16 local[2];
    uint32 left, right, value, elapsed, result;
    sint32 remaining, magnitude;
    record->volume_countdown = (uint32)(countdown);
    if (period > 0)
    {
        if (countdown % (uint32)period != 0)
            goto volume_down_return;
        value = record->volume_remaining - 1;
        record->volume_remaining = (uint16)(value);
        if ((sint16)value <= 0)
            goto volume_down_clear;
        get_volume(packed, &local[0], &local[1]);
        left = local[0];
        remaining = (sint16)record->volume_remaining;
        if ((sint32)left - remaining <= 0)
            goto volume_down_minimum;
        right = local[1];
        if ((sint32)right - remaining <= 0 || left == 1)
            goto volume_down_minimum;
        left = local[0];
        right = local[1];
        set_volume(packed, (sint16)(left - 1), (sint16)(right - 1), 1);
    }
    else
    {
        value = record->volume_remaining + (uint32)period;
        record->volume_remaining = (uint16)(value);
        if ((sint16)value <= 0)
            goto volume_down_clear;
        get_volume(packed, &local[0], &local[1]);
        elapsed = record->volume_duration - record->volume_countdown;
        period = (sint16)record->volume_period;
        magnitude = -period;
        value = (uint32)(sint32)(sint16)record->volume_delta;
        if (value < elapsed * (uint32)magnitude)
            goto volume_down_minimum;
        left = local[0];
        if (magnitude >= (sint32)left)
            goto volume_down_minimum;
        left = local[0];
        right = local[1];
        set_volume(packed, (sint16)(left + (uint32)period), (sint16)(right + (uint32)period), 1);
    }
    goto volume_down_finish;
volume_down_minimum:
    set_volume(packed, 1, 1, 1);
    goto volume_down_finish;
volume_down_clear:
    flag_record = (*slot) + track_index;
    flag_record->flags = (uint32)(flag_record->flags & ~0x20);
volume_down_finish:
    if (record->volume_countdown == 0 || (sint16)record->volume_remaining == 0)
    {
        flag_record = (*slot) + track_index;
        flag_record->flags = (uint32)(flag_record->flags & ~0x20);
    }
volume_down_return:
    result = get_volume(packed, &record->saved_left_volume, &record->saved_right_volume);
    return result;
}

void spu_sequence_scheduler_tempo_ramp(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation)
{
    SpuSequenceSchedulerTrack **slot = &scheduler->tracks[(uint32)(sint32)sequence];
    uint32 track_index = (uint32)(sint32)separation;
    SpuSequenceSchedulerTrack *record = (*slot) + track_index;
    uint32 countdown = record->tempo_countdown - 1;
    sint32 period = (sint16)record->tempo_period;
    uint32 current, target, value, remainder, numerator, denominator;
    record->tempo_countdown = (uint32)(countdown);
    if (period > 0)
    {
        remainder = countdown % (uint32)period;
        if (remainder != 0)
            return;
        current = record->tempo;
        target = record->target_tempo;
        if (target < current)
            record->tempo = (uint32)(current - 1);
        else if (current < target)
            record->tempo = (uint32)(current + 1);
    }
    else
    {
        current = record->tempo;
        target = record->target_tempo;
        if (target < current)
        {
            value = current + (uint32)period;
            record->tempo = (uint32)(value);
            if (value < target)
                record->tempo = (uint32)(target);
        }
        else if (current < target)
        {
            value = current - (uint32)period;
            target = record->target_tempo;
            record->tempo = (uint32)(value);
            if (target < value)
                record->tempo = (uint32)(target);
        }
    }
    numerator = (uint32)(sint32)(sint16)record->ticks_per_quarter * record->tempo;
    denominator = scheduler->rate * 60;
    numerator *= 10;
    if (denominator == 0)
    {
#ifndef NDEBUG
        fprintf(stderr, "P2: SEQ tempo ramp division by zero at 80075F1C\n");
#else
        (void)0;
#endif
        abort();
    }
    value = numerator / denominator;
    record->tick_period = (uint16)(value);
    if ((sint16)value <= 0)
        record->tick_period = (uint16)(1);
    if (record->tempo_countdown != 0)
    {
        current = record->tempo;
        target = record->target_tempo;
        if (current != target)
            return;
    }
    record = (*slot) + track_index;
    record->flags = (uint32)(record->flags & ~0x40);
    record = (*slot) + track_index;
    record->flags = (uint32)(record->flags & ~0x80);
    return;
}

uint32 spu_sequence_scheduler_tick_mode(SpuSequenceScheduler *scheduler, SpuSequenceClock *clock, uint32 mode, uint32 video)
{
    sint32 selected;
    uint32 result;
    if ((mode & 0x1000) != 0)
    {
        clock->manual = 1;
        clock->mode = mode & 0xFFF;
    }
    else
    {
        clock->manual = 0;
        clock->mode = (sint32)mode;
    }
    selected = clock->mode;
    if (selected >= 6)
    {
        scheduler->rate = (uint32)selected;
        return 0;
    }
    switch (selected)
    {
        case 4:
            scheduler->rate = 50;
            clock->mode = video == 1 ? 5 : 50;
            return 5;
        case 1:
            scheduler->rate = 60;
            result = video == 0 ? 5 : 60;
            clock->mode = result;
            return result;
        case 3:
            scheduler->rate = 120;
            return 120;
        case 2:
            scheduler->rate = 240;
            return 240;
        case 0:
        case 5:
            result = video == 1 ? 50 : 60;
            scheduler->rate = result;
            return result;
        default:
            scheduler->rate = 60;
            return 60;
    }
}

uint32 spu_sequence_scheduler_volume(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, uint8 value, SpuSequenceVoiceVolumeSet set_voice_volume, SpuSequenceHandler read_delta)
{
    SpuSequenceSchedulerTrack *record;
    uint32 channel, program, pan, result;
    sint16 bank, packed;
    record = scheduler->tracks[(uint32)(sint32)sequence] + (uint32)(sint32)separation;
    channel = record->channel;
    bank = (sint16)record->bank;
    program = record->channel_program[channel];
    pan = record->channel_pan[channel];
    packed = (sint16)((uint32)(sint32)sequence | ((uint32)(sint32)separation << 8));
    set_voice_volume(packed, bank, (sint16)program, (uint16)value, (uint16)pan);
    record->channel_volume[channel] = (uint16)(value);
    result = read_delta(sequence, separation);
    record->delta = (uint32)(result);
    return result;
}

uint32 spu_sequence_scheduler_pan(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, uint8 value, SpuSequenceVoiceVolumeSet set_voice_volume, SpuSequenceHandler read_delta)
{
    SpuSequenceSchedulerTrack *record;
    uint8 *channel_program;
    uint8 *channel_pan;
    uint32 channel, program, volume, result;
    sint16 bank, packed;
    record = scheduler->tracks[(uint32)(sint32)sequence] + (uint32)(sint32)separation;
    channel = record->channel;
    bank = (sint16)record->bank;
    channel_program = &record->channel_program[channel];
    channel_pan = &record->channel_pan[channel];
    program = *channel_program;
    volume = record->channel_volume[channel];
    packed = (sint16)((uint32)(sint32)sequence | ((uint32)(sint32)separation << 8));
    set_voice_volume(packed, bank, (sint16)program, (uint16)volume, (uint16)value);
    *channel_pan = (uint8)(value);
    result = read_delta(sequence, separation);
    record->delta = (uint32)(result);
    return result;
}

uint32 spu_sequence_scheduler_expression(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, uint8 value, SpuSequenceVoiceVolumeSet set_voice_volume, SpuSequenceHandler read_delta)
{
    SpuSequenceSchedulerTrack *record;
    uint32 channel, result;
    sint16 packed;
    record = scheduler->tracks[(uint32)(sint32)sequence] + (uint32)(sint32)separation;
    channel = record->channel;
    spu_vab_set_program_volume((sint16)record->bank, record->channel_program[channel], value);
    packed = (sint16)((uint32)(sint32)sequence | ((uint32)(sint32)separation << 8));
    set_voice_volume(packed, (sint16)record->bank, record->channel_program[channel], record->channel_volume[channel], record->channel_pan[channel]);
    result = read_delta(sequence, separation);
    record->delta = result;
    return result;
}

uint32 spu_sequence_scheduler_reset_controllers(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, SpuSequenceReverbEnable set_reverb, SpuSequenceHandler read_delta)
{
    SpuSequenceSchedulerTrack *record;
    uint32 channel, program, result;
    record = scheduler->tracks[(uint32)(sint32)sequence] + (uint32)(sint32)separation;
    /* Inline the original reverb-off and damper-off helpers */
    set_reverb(0);
    spu_vm_state()->release_offset = 0;
    channel = record->channel;
    program = record->channel;
    record->channel_program[channel] = (uint8)(program);
    channel = record->channel;
    record->rpn_parameter = (uint8)(0);
    record->rpn_selector = (uint8)(0);
    record->channel_volume[channel] = (uint16)(127);
    channel = record->channel;
    record->channel_pan[channel] = (uint8)(64);
    result = read_delta(sequence, separation);
    record->delta = (uint32)(result);
    return result;
}

uint32 spu_sequence_scheduler_nrpn_lsb(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, uint8 value, SpuSequenceMarkHandler mark, SpuSequenceHandler read_delta)
{
    SpuSequenceSchedulerTrack *record;
    uint32 mode, count, result;
    record = scheduler->tracks[(uint32)(sint32)sequence] + (uint32)(sint32)separation;
    if (record->loop_count_pending == 1 && record->loop_count_set == 0)
    {
        record->loop_remaining = (uint8)(value);
        record->loop_count_set = (uint8)(1);
        mode = record->nrpn_selector;
    }
    else
    {
        mode = record->nrpn_selector;
        if (mode != 30 && mode != 20)
        {
            count = record->nrpn_phase;
            record->nrpn_parameter = (uint8)(value);
            record->nrpn_phase = (uint8)(count + 1);
            mode = record->nrpn_selector;
        }
    }
    if (mode == 40)
        mark(sequence, separation, value);
    result = read_delta(sequence, separation);
    record->delta = (uint32)(result);
    return result;
}

uint32 spu_sequence_scheduler_damper(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, uint8 value, SpuSequenceHandler read_delta)
{
    SpuSequenceSchedulerTrack *record;
    uint32 result;
    record = scheduler->tracks[(uint32)(sint32)sequence] + (uint32)(sint32)separation;
    /* Inline the original single-store damper helpers */
    spu_vm_state()->release_offset = value >= 0x40 ? 2 : 0;
    result = read_delta(sequence, separation);
    record->delta = (uint32)(result);
    return result;
}

uint32 spu_sequence_scheduler_reverb_depth(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, uint8 value, SpuSequenceReverbDepthSet set_reverb_depth, SpuSequenceHandler read_delta)
{
    SpuSequenceSchedulerTrack *record;
    uint32 result;
    record = scheduler->tracks[(uint32)(sint32)sequence] + (uint32)(sint32)separation;
    set_reverb_depth((sint16)value, (sint16)value);
    result = read_delta(sequence, separation);
    record->delta = (uint32)(result);
    return result;
}

uint32 spu_sequence_scheduler_nrpn_msb(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, uint8 value, SpuSequenceHandler read_delta)
{
    SpuSequenceSchedulerTrack *record;
    uint32 count, result, cursor;
    record = scheduler->tracks[(uint32)(sint32)sequence] + (uint32)(sint32)separation;
    if (value == 20)
    {
        record->nrpn_selector = (uint8)(value);
        record->loop_count_pending = (uint8)(1);
        result = read_delta(sequence, separation);
        cursor = record->stream.cursor;
        record->delta = (uint32)(result);
        record->stream.loop_start = (uint32)(cursor);
        return result;
    }
    if (value == 30)
    {
        count = record->loop_remaining;
        record->nrpn_selector = (uint8)(value);
        if (count != 0)
        {
            if (count >= 0x7F)
            {
                read_delta(sequence, separation);
                result = record->stream.loop_start;
                record->delta = (uint32)(0);
                record->stream.cursor = (uint32)(result);
                return result;
            }
            count = record->loop_remaining;
            record->loop_remaining = (uint8)(count - 1);
            result = read_delta(sequence, separation);
            count = record->loop_remaining;
            record->delta = (uint32)(result);
            if (count != 0)
            {
                result = record->stream.loop_start;
                record->stream.cursor = (uint32)(result);
            }
            else
                record->loop_count_set = (uint8)(0);
            return result;
        }
        record->loop_count_set = (uint8)(0);
    }
    else
    {
        count = record->nrpn_phase;
        record->nrpn_selector = (uint8)(value);
        record->nrpn_phase = (uint8)(count + 1);
    }
    result = read_delta(sequence, separation);
    record->delta = (uint32)(result);
    return result;
}

uint32 spu_sequence_scheduler_rpn_lsb(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, uint8 value, SpuSequenceHandler read_delta)
{
    SpuSequenceSchedulerTrack *record;
    uint32 count, result;
    record = scheduler->tracks[(uint32)(sint32)sequence] + (uint32)(sint32)separation;
    count = record->rpn_phase;
    record->rpn_parameter = (uint8)(value);
    record->rpn_phase = (uint8)(count + 1);
    result = read_delta(sequence, separation);
    record->delta = (uint32)(result);
    return result;
}

uint32 spu_sequence_scheduler_rpn_msb(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, uint8 value, SpuSequenceHandler read_delta)
{
    SpuSequenceSchedulerTrack *record;
    uint32 count, result;
    record = scheduler->tracks[(uint32)(sint32)sequence] + (uint32)(sint32)separation;
    count = record->rpn_phase;
    record->rpn_selector = (uint8)(value);
    record->rpn_phase = (uint8)(count + 1);
    result = read_delta(sequence, separation);
    record->delta = (uint32)(result);
    return result;
}

uint32 spu_sequence_scheduler_control_change(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, uint8 controller, SpuSequenceHandlers *handlers, SpuSequenceHandler read_delta)
{
    SpuSequenceSchedulerTrack *record;
    uint32 cursor, value, result;
    record = scheduler->tracks[(uint32)(sint32)sequence] + (uint32)(sint32)separation;
    cursor = record->stream.cursor;
    record->stream.cursor = (uint32)(cursor + 1);
    if (record->stream.data == NULL || cursor >= record->stream.size || 1u > record->stream.size - cursor)
    {
#ifndef NDEBUG
        fprintf(stderr, "P2: SEQ stream read outside buffer at %u, span 1, size %u\n", cursor, record->stream.size);
#else
        (void)0;
#endif
        abort();
    }
    value = record->stream.data[cursor];
    switch (controller)
    {
        case 0:
            record->bank = (uint16)(value);
            break;
        case 6:
            return handlers->data_entry(sequence, separation, (uint8)value);
        case 7:
            return handlers->volume(sequence, separation, (uint8)value);
        case 10:
            return handlers->pan(sequence, separation, (uint8)value);
        case 11:
            return handlers->expression(sequence, separation, (uint8)value);
        case 64:
            if (handlers->damper != NULL)
                return handlers->damper(sequence, separation, (uint8)value);
            return handlers->expression(sequence, separation, (uint8)value);
        case 65:
            if (handlers->portamento != NULL)
                return handlers->portamento(sequence, separation, (uint8)value);
            break;
        case 91:
            return handlers->reverb_depth(sequence, separation, (uint8)value);
        case 98:
            return handlers->parameter_lsb(sequence, separation, (uint8)value);
        case 99:
            return handlers->parameter_msb(sequence, separation, (uint8)value);
        case 100:
            return handlers->registered_parameter_lsb(sequence, separation, (uint8)value);
        case 101:
            return handlers->registered_parameter_msb(sequence, separation, (uint8)value);
        case 121:
            return handlers->reset_controllers(sequence, separation);
    }
    result = read_delta(sequence, separation);
    record->delta = (uint32)(result);
    return result;
}

uint32 spu_sequence_scheduler_meta_event(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, SpuSequenceHandler read_delta)
{
    SpuSequenceSchedulerTrack *record;
    uint32 cursor, high, middle, microseconds, tempo, product;
    uint32 rate, quarter_rate, denominator, numerator, quotient, remainder, result;
    record = scheduler->tracks[(uint32)(sint32)sequence] + (uint32)(sint32)separation;
    cursor = record->stream.cursor;
    record->stream.cursor = (uint32)(cursor + 1);
    if (record->stream.data == NULL || cursor >= record->stream.size || 1u > record->stream.size - cursor)
    {
#ifndef NDEBUG
        fprintf(stderr, "P2: SEQ stream read outside buffer at %u, span 1, size %u\n", cursor, record->stream.size);
#else
        (void)0;
#endif
        abort();
    }
    high = record->stream.data[cursor];
    record->stream.cursor = (uint32)(cursor + 2);
    if (record->stream.data == NULL || cursor >= record->stream.size || 2u > record->stream.size - cursor)
    {
#ifndef NDEBUG
        fprintf(stderr, "P2: SEQ stream read outside buffer at %u, span 2, size %u\n", cursor, record->stream.size);
#else
        (void)0;
#endif
        abort();
    }
    middle = record->stream.data[cursor + 1];
    record->stream.cursor = (uint32)(cursor + 3);
    if (record->stream.data == NULL || cursor >= record->stream.size || 3u > record->stream.size - cursor)
    {
#ifndef NDEBUG
        fprintf(stderr, "P2: SEQ stream read outside buffer at %u, span 3, size %u\n", cursor, record->stream.size);
#else
        (void)0;
#endif
        abort();
    }
    microseconds = (high << 16) | (middle << 8) | record->stream.data[cursor + 2];
    if (microseconds == 0)
    {
#ifndef NDEBUG
        fprintf(stderr, "P2: Sequence tempo division by zero at 80073AA4\n");
#else
        (void)0;
#endif
        abort();
    }
    tempo = 60000000 / microseconds;
    product = (uint32)(sint32)(sint16)record->ticks_per_quarter * tempo;
    rate = scheduler->rate;
    quarter_rate = 15 * rate;
    denominator = 4 * quarter_rate;
    record->tempo = (uint32)(tempo);
    if (10 * product < denominator)
    {
        if (product == 0)
        {
#ifndef NDEBUG
            fprintf(stderr, "P2: Sequence tempo division by zero at 80073B24\n");
#else
            (void)0;
#endif
            abort();
        }
        quotient = (600 * rate) / product;
        record->tick_countdown = (uint16)(quotient);
        record->tick_period = (uint16)(quotient);
    }
    else
    {
        product = (uint32)(sint32)(sint16)record->ticks_per_quarter * record->tempo;
        numerator = 10 * product;
        if (denominator == 0)
        {
#ifndef NDEBUG
            fprintf(stderr, "P2: Sequence tempo division by zero at 80073B68\n");
#else
            (void)0;
#endif
            abort();
        }
        quotient = numerator / denominator;
        product = (uint32)(sint32)(sint16)record->ticks_per_quarter * record->tempo;
        remainder = (10 * product) % denominator;
        record->tick_countdown = (uint16)(0xFFFF);
        record->tick_period = (uint16)(quotient);
        if (2 * quarter_rate < remainder)
            record->tick_period = (uint16)(quotient + 1);
    }
    result = read_delta(sequence, separation);
    record->delta = (uint32)(result);
    return result;
}

static uint32 spu_sequence_read_event(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, SpuSequenceHandlers *handlers, SpuSequenceHandler read_delta, SpuSequenceHandler end_of_track, SpuSequenceValueHandler meta_dispatch)
{
    SpuSequenceSchedulerTrack *record;
    uint32 cursor, event, status, velocity, delta;
    record = scheduler->tracks[(uint32)(sint32)sequence] + (uint32)(sint32)separation;
    cursor = record->stream.cursor;
    record->stream.cursor = (uint32)(cursor + 1);
    if (record->stream.data == NULL || cursor >= record->stream.size || 1u > record->stream.size - cursor)
    {
#ifndef NDEBUG
        fprintf(stderr, "P2: SEQ stream read outside buffer at %u, span 1, size %u\n", cursor, record->stream.size);
#else
        (void)0;
#endif
        abort();
    }
    event = record->stream.data[cursor];
    if ((event & 0x80) != 0)
    {
        status = event & 0xF0;
        record->channel = (uint8)(event & 0x0F);
        switch (status)
        {
            case 0x90:
                cursor = record->stream.cursor;
                record->running_status = (uint8)(status);
                record->stream.cursor = (uint32)(cursor + 1);
                if (record->stream.data == NULL || cursor >= record->stream.size || 1u > record->stream.size - cursor)
                {
#ifndef NDEBUG
                    fprintf(stderr, "P2: SEQ stream read outside buffer at %u, span 1, size %u\n", cursor, record->stream.size);
#else
                    (void)0;
#endif
                    abort();
                }
                event = record->stream.data[cursor];
                record->stream.cursor = (uint32)(cursor + 2);
                if (record->stream.data == NULL || cursor >= record->stream.size || 2u > record->stream.size - cursor)
                {
#ifndef NDEBUG
                    fprintf(stderr, "P2: SEQ stream read outside buffer at %u, span 2, size %u\n", cursor, record->stream.size);
#else
                    (void)0;
#endif
                    abort();
                }
                velocity = record->stream.data[cursor + 1];
                delta = read_delta(sequence, separation);
                record->delta = (uint32)(delta);
                return handlers->note_on(sequence, separation, (uint8)event, (uint8)velocity);
            case 0xB0:
            case 0xC0:
                cursor = record->stream.cursor;
                record->running_status = (uint8)(status);
                record->stream.cursor = (uint32)(cursor + 1);
                if (record->stream.data == NULL || cursor >= record->stream.size || 1u > record->stream.size - cursor)
                {
#ifndef NDEBUG
                    fprintf(stderr, "P2: SEQ stream read outside buffer at %u, span 1, size %u\n", cursor, record->stream.size);
#else
                    (void)0;
#endif
                    abort();
                }
                event = record->stream.data[cursor];
                break;
            case 0xE0:
                cursor = record->stream.cursor;
                record->running_status = (uint8)(status);
                record->stream.cursor = (uint32)(cursor + 1);
                break;
            case 0xF0:
                cursor = record->stream.cursor;
                record->running_status = (uint8)(0xFF);
                record->channel = (uint8)(event & 0x0F);
                record->stream.cursor = (uint32)(cursor + 1);
                if (record->stream.data == NULL || cursor >= record->stream.size || 1u > record->stream.size - cursor)
                {
#ifndef NDEBUG
                    fprintf(stderr, "P2: SEQ stream read outside buffer at %u, span 1, size %u\n", cursor, record->stream.size);
#else
                    (void)0;
#endif
                    abort();
                }
                event = record->stream.data[cursor];
                status = 0xFF;
                break;
            default:
                return status < 0xC1 ? 0xB0 : 0xFF;
        }
    }
    else
    {
        status = record->running_status;
        if (status == 0x90)
        {
            record->stream.cursor = (uint32)(cursor + 2);
            if (record->stream.data == NULL || cursor >= record->stream.size || 2u > record->stream.size - cursor)
            {
#ifndef NDEBUG
                fprintf(stderr, "P2: SEQ stream read outside buffer at %u, span 2, size %u\n", cursor, record->stream.size);
#else
                (void)0;
#endif
                abort();
            }
            velocity = record->stream.data[cursor + 1];
            delta = read_delta(sequence, separation);
            record->delta = (uint32)(delta);
            return handlers->note_on(sequence, separation, (uint8)event, (uint8)velocity);
        }
    }
    switch (status)
    {
        case 0xB0:
            return handlers->control_change(sequence, separation, (uint8)event);
        case 0xC0:
            return handlers->program_change(sequence, separation, (uint8)event);
        case 0xE0:
            return handlers->pitch_bend(sequence, separation);
        case 0xFF:
            if (meta_dispatch != NULL)
                return meta_dispatch(sequence, separation, (uint8)event);
            if (event == 0x2F)
                return end_of_track(sequence, separation);
            return handlers->meta_event(sequence, separation);
        default:
            return status < 0xC1 ? 0xB0 : 0xFF;
    }
}

uint32 spu_sequence_scheduler_read_event(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, SpuSequenceHandlers *handlers, SpuSequenceHandler read_delta, SpuSequenceHandler end_of_track)
{
    return spu_sequence_read_event(scheduler, sequence, separation, handlers, read_delta, end_of_track, NULL);
}

uint32 spu_sequence_scheduler_dispatch_event(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, SpuSequenceHandlers *handlers, SpuSequenceHandler read_delta, SpuSequenceValueHandler meta_dispatch)
{
    if (meta_dispatch == NULL)
        abort();
    return spu_sequence_read_event(scheduler, sequence, separation, handlers, read_delta, NULL, meta_dispatch);
}

uint32 spu_sequence_scheduler_end_of_track(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, SpuSequenceHandler next_separation, SpuSequencePackedHandler key_off)
{
    SpuSequenceSchedulerTrack **slot;
    SpuSequenceSchedulerTrack *record;
    uint32 *flags;
    uint32 track_index, count, result, copy, value, bank;
    sint32 repeats;
    sint16 packed;
    slot = &scheduler->tracks[(uint32)(sint32)sequence];
    track_index = (uint32)(sint32)separation;
    record = (*slot) + track_index;
    count = record->repeat_count + 1;
    repeats = (sint16)record->repeat_limit;
    record->repeat_count = (uint16)(count);
    if (repeats == 0)
    {
        result = record->stream.start;
        record->elapsed_ticks = (uint32)(0);
        record->loop_count_pending = (uint8)(0);
        record->delta = (uint32)(0);
        record->stream.cursor = (uint32)(result);
        return result;
    }
    if ((sint16)count < repeats)
    {
        result = record->stream.start;
        copy = record->stream.start;
        record->elapsed_ticks = (uint32)(0);
        record->loop_count_pending = (uint8)(0);
        record->delta = (uint32)(0);
        record->stream.cursor = (uint32)(result);
        record->stream.loop_start = (uint32)(copy);
        return result;
    }
    flags = &((*slot) + track_index)->flags;
    *flags = (uint32)(*flags & 0xFFFFFFFE);
    flags = &((*slot) + track_index)->flags;
    *flags = (uint32)(*flags & 0xFFFFFFF7);
    flags = &((*slot) + track_index)->flags;
    *flags = (uint32)(*flags & 0xFFFFFFFD);
    flags = &((*slot) + track_index)->flags;
    *flags = (uint32)(*flags | 0x200);
    flags = &((*slot) + track_index)->flags;
    *flags = (uint32)(*flags | 4);
    result = record->stream.start;
    value = record->next_sequence;
    record->playback_latch = (uint8)(0);
    record->stream.loop_start = (uint32)(result);
    packed = (sint16)((uint32)(sint32)sequence | ((uint32)(sint32)separation << 8));
    if (value != 0xFF)
    {
        value = record->next_sequence;
        bank = record->next_separation;
        record->playback_latch = (uint8)(0);
        next_separation((sint16)value, (sint16)bank);
        key_off(packed);
    }
    key_off(packed);
    result = (uint32)(sint32)(sint16)record->tick_period;
    record->delta = (uint32)(result);
    return result;
}

uint32 spu_sequence_scheduler_program_change(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, uint8 program, SpuSequenceHandler read_delta)
{
    SpuSequenceSchedulerTrack *record;
    uint32 result;
    record = scheduler->tracks[(uint32)(sint32)sequence] + (uint32)(sint32)separation;
    record->channel_program[record->channel] = (uint8)(program);
    result = read_delta(sequence, separation);
    record->delta = (uint32)(result);
    return result;
}

uint32 spu_sequence_scheduler_pitch_bend(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, SpuSequencePitchSet set_pitch, SpuSequenceHandler read_delta)
{
    SpuSequenceSchedulerTrack *record = scheduler->tracks[(uint32)(sint32)sequence] + (uint32)(sint32)separation;
    uint32 cursor = record->stream.cursor;
    sint16 bank = (sint16)record->bank;
    sint16 packed = (sint16)((uint32)(sint32)sequence | ((uint32)(sint32)separation << 8));
    uint32 channel, bend, program, result;
    record->stream.cursor = (uint32)(cursor + 1);
    channel = record->channel;
    if (record->stream.data == NULL || cursor >= record->stream.size || 1u > record->stream.size - cursor)
    {
#ifndef NDEBUG
        fprintf(stderr, "P2: SEQ stream read outside buffer at %u, span 1, size %u\n", cursor, record->stream.size);
#else
        (void)0;
#endif
        abort();
    }
    bend = record->stream.data[cursor];
    program = record->channel_program[channel];
    set_pitch(packed, bank, (sint16)program, (uint16)bend);
    result = read_delta(sequence, separation);
    record->delta = (uint32)(result);
    return result;
}

uint32 spu_sequence_scheduler_note_on(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, uint8 note, uint8 velocity, SpuSequenceVoiceKeyOn key_on, SpuSequenceVoiceKeyOff key_off)
{
    SpuSequenceSchedulerTrack *record = scheduler->tracks[(uint32)(sint32)sequence] + (uint32)(sint32)separation;
    uint32 channel = record->channel;
    uint32 result = (uint32)(((sint16)record->channel_mute_mask >> (channel & 31)) & 1);
    uint32 pan = record->channel_pan[channel];
    sint16 packed;
    if (result != 0)
        return result;
    result = record->left_volume;
    if (result == 0)
        return velocity;
    packed = (sint16)((uint32)(sint32)sequence | ((uint32)(sint32)separation << 8));
    if (velocity != 0)
    {
        result = key_on(packed, (sint16)record->bank, (sint16)record->channel_program[channel], note, velocity, (uint16)pan);
        record->event_velocity = (uint16)(velocity);
        return result;
    }
    return key_off(packed, (sint16)record->bank, (sint16)record->channel_program[channel], note);
}

uint32 spu_sequence_scheduler_next_separation(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation)
{
    SpuSequenceSchedulerTrack **slot;
    SpuSequenceSchedulerTrack *record;
    uint32 *flags;
    uint32 track_index, cursor, result;
    slot = &scheduler->tracks[(uint32)(sint32)sequence];
    track_index = (uint32)(sint32)separation;
    record = (*slot) + track_index;
    record->repeat_limit = (uint16)(1);
    record->repeat_count = (uint16)(0);
    flags = &((*slot) + track_index)->flags;
    *flags = (uint32)(*flags & 0xFFFFFEFF);
    flags = &((*slot) + track_index)->flags;
    *flags = (uint32)(*flags & 0xFFFFFFF7);
    flags = &((*slot) + track_index)->flags;
    *flags = (uint32)(*flags & 0xFFFFFFFD);
    flags = &((*slot) + track_index)->flags;
    *flags = (uint32)(*flags & 0xFFFFFFFB);
    flags = &((*slot) + track_index)->flags;
    *flags = (uint32)(*flags & 0xFFFFFDFF);
    cursor = record->stream.start;
    record->playback_latch = (uint8)(1);
    record->stream.cursor = (uint32)(cursor);
    flags = &((*slot) + track_index)->flags;
    result = *flags | 1;
    *flags = (uint32)(result);
    return result;
}

uint32 spu_sequence_scheduler_read_delta(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation)
{
    SpuSequenceSchedulerTrack *record = scheduler->tracks[(uint32)(sint32)sequence] + (uint32)(sint32)separation;
    return spu_sequence_stream_delta(&record->stream, &record->elapsed_ticks);
}

uint32 spu_sequence_scheduler_replay_mode(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 track)
{
    SpuSequenceSchedulerTrack **table = &scheduler->tracks[(uint32)(sint32)sequence];
    uint32 track_index = (uint32)(sint32)track;
    SpuSequenceSchedulerTrack *entry = (*table) + track_index;
    uint32 flags = entry->flags;
    SpuSequenceSchedulerTrack *current;
    if ((flags & 0x204) != 0)
        return flags & 0x100;
    if ((flags & 0x100) != 0)
        return (uint32)-3;
    entry->flags = (uint32)(flags & ~2u);
    current = (*table) + track_index;
    current->flags = (uint32)(current->flags | 8);
    current = (*table) + track_index;
    flags = current->flags | 1;
    current->flags = (uint32)(flags);
    return flags;
}

uint32 spu_sequence_scheduler_play_mode(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 track, uint8 mode, sint16 repeat, SpuSequenceVolumeSet set_volume)
{
    SpuSequenceSchedulerTrack **table = &scheduler->tracks[(uint32)(sint32)sequence];
    uint32 track_index = (uint32)(sint32)track;
    SpuSequenceSchedulerTrack *entry = (*table) + track_index;
    SpuSequenceSchedulerTrack *current;
    uint32 result = 1;
    entry->flags = (uint32)(entry->flags & ~0x200u);
    current = (*table) + track_index;
    current->flags = (uint32)(current->flags & ~4u);
    entry->repeat_limit = (uint16)(repeat);
    if (mode == 1)
    {
        sint16 handle = (sint16)((uint32)(sint32)sequence | ((uint32)(sint32)track << 8));
        uint16 left;
        uint16 right;
        current = (*table) + track_index;
        current->flags = (uint32)(current->flags | 1);
        left = entry->left_volume;
        right = entry->right_volume;
        entry->repeat_count = (uint16)(0);
        entry->playback_latch = (uint8)(1);
        return set_volume(handle, (sint16)left, (sint16)right, 0);
    }
    if (mode == 0)
    {
        current = (*table) + track_index;
        result = current->flags | 2;
        current->flags = (uint32)(result);
    }
    return result;
}

uint32 spu_sequence_scheduler_stop(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 track, SpuSequencePackedHandler key_off)
{
    SpuSequenceSchedulerTrack **table = &scheduler->tracks[(uint32)(sint32)sequence];
    uint32 track_index = (uint32)(sint32)track;
    SpuSequenceSchedulerTrack *entry = (*table) + track_index;
    SpuSequenceSchedulerTrack *current;
    uint32 saved_ticks;
    uint32 saved_tempo;
    uint16 saved_duration;
    uint32 saved_position;
    uint32 saved_loop;
    uint32 channel;
    entry->flags = (uint32)(entry->flags & ~1u);
    current = (*table) + track_index;
    current->flags = (uint32)(current->flags & ~2u);
    current = (*table) + track_index;
    current->flags = (uint32)(current->flags & ~8u);
    current = (*table) + track_index;
    current->flags = (uint32)(current->flags & ~0x400u);
    current = (*table) + track_index;
    current->flags = (uint32)(current->flags | 4);
    key_off((sint16)((uint32)(sint32)sequence | ((uint32)(sint32)track << 8)));
    spu_vm_state()->release_offset = 0;
    saved_ticks = entry->initial_delta;
    saved_tempo = entry->initial_tempo;
    saved_duration = entry->initial_tick_period;
    saved_position = entry->stream.start;
    saved_loop = entry->stream.start;
    entry->playback_latch = (uint8)(0);
    entry->elapsed_ticks = (uint32)(0);
    entry->loop_count_pending = (uint8)(0);
    entry->rpn_parameter = (uint8)(0);
    entry->rpn_selector = (uint8)(0);
    entry->rpn_phase = (uint8)(0);
    entry->nrpn_parameter = (uint8)(0);
    entry->nrpn_selector = (uint8)(0);
    entry->nrpn_phase = (uint8)(0);
    entry->channel = (uint8)(0);
    entry->repeat_count = (uint16)(0);
    entry->loop_count_pending = (uint8)(0);
    entry->loop_remaining = (uint8)(0);
    entry->loop_count_set = (uint8)(0);
    entry->running_status = (uint8)(0);
    entry->delta = (uint32)(saved_ticks);
    entry->tempo = (uint32)(saved_tempo);
    entry->tick_period = (uint16)(saved_duration);
    entry->stream.cursor = (uint32)(saved_position);
    entry->stream.loop_start = (uint32)(saved_loop);
    for (channel = 0; channel < 16; ++channel)
    {
        entry->channel_program[channel] = (uint8)(channel);
        entry->channel_pan[channel] = (uint8)(64);
        entry->channel_volume[channel] = (uint16)(127);
    }
    entry->saved_left_volume = (uint16)(127);
    entry->saved_right_volume = (uint16)(127);
    return 127;
}

uint32 spu_sequence_scheduler_pause(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, SpuSequencePackedHandler key_off)
{
    SpuSequenceSchedulerTrack **slot = &scheduler->tracks[(uint32)(sint32)sequence];
    uint32 track_index = (uint32)(sint32)separation;
    SpuSequenceSchedulerTrack *record = (*slot) + track_index;
    uint32 result;
    sint16 packed = (sint16)((uint32)(sint32)sequence | ((uint32)(sint32)separation << 8));
    key_off(packed);
    record->playback_latch = (uint8)(0);
    record = (*slot) + track_index;
    result = record->flags & ~2;
    record->flags = (uint32)(result);
    return result;
}

void spu_sequence_scheduler_resume(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation)
{
    SpuSequenceSchedulerTrack **slot = &scheduler->tracks[(uint32)(sint32)sequence];
    uint32 track_index = (uint32)(sint32)separation;
    SpuSequenceSchedulerTrack *record = (*slot) + track_index;
    record->playback_latch = (uint8)(1);
    record = (*slot) + track_index;
    record->flags = (uint32)(record->flags & ~8);
    return;
}

uint32 spu_sequence_scheduler_request_pause(SpuSequenceScheduler *scheduler, uint32 sequence, uint32 track, SpuSequenceVolumeGet get_volume)
{
    SpuSequenceSchedulerTrack **table;
    SpuSequenceSchedulerTrack *entry;
    uint32 track_index, result;
    table = &scheduler->tracks[(uint32)(sint32)(sint16)sequence];
    track_index = (uint32)(sint32)(sint16)track;
    entry = (*table) + track_index;
    get_volume((sint16)(sequence | (track << 8)), &entry->saved_left_volume, &entry->saved_right_volume);
    entry = (*table) + track_index;
    result = entry->flags & 0xFFFFFFFE;
    entry->flags = (uint32)(result);
    entry = (*table) + track_index;
    result = entry->flags & 0xFFFFFFF7;
    entry->flags = (uint32)(result);
    entry = (*table) + track_index;
    result = entry->flags | 2;
    entry->flags = (uint32)(result);
    return result;
}

uint32 spu_sequence_scheduler_close(SpuSequenceScheduler *scheduler, sint16 sequence, SpuSequenceVolumeSet set_volume, SpuSequencePackedHandler key_off)
{
    uint32 result;
    SpuSequenceSchedulerTrack **table;
    uint32 track_index = 0;
    uint32 index = 0;
    SpuSequenceSchedulerTrack *pointer;
    sint32 count;
    set_volume(sequence, 0, 0, 1);
    key_off(sequence);
    result = scheduler->used_mask;
    count = (sint16)scheduler->separation_count;
    result &= ~((uint32)1 << ((uint32)(sint32)sequence & 31));
    scheduler->used_mask = result;
    if (count > 0)
    {
        table = &scheduler->tracks[(uint32)(sint32)sequence];
        do
        {
            ((SpuSequenceSchedulerTrack *)(track_index + (*table)))->flags = (uint32)(0);
            ((SpuSequenceSchedulerTrack *)(track_index + (*table)))->next_sequence = (uint8)(255);
            ((SpuSequenceSchedulerTrack *)(track_index + (*table)))->next_separation = (uint8)(0);
            ((SpuSequenceSchedulerTrack *)(track_index + (*table)))->volume_delta = (uint16)(0);
            ((SpuSequenceSchedulerTrack *)(track_index + (*table)))->volume_remaining = (uint16)(0);
            ((SpuSequenceSchedulerTrack *)(track_index + (*table)))->volume_duration = (uint32)(0);
            ((SpuSequenceSchedulerTrack *)(track_index + (*table)))->volume_countdown = (uint32)(0);
            ((SpuSequenceSchedulerTrack *)(track_index + (*table)))->volume_period = (uint16)(0);
            ((SpuSequenceSchedulerTrack *)(track_index + (*table)))->target_tempo = (uint32)(0);
            ((SpuSequenceSchedulerTrack *)(track_index + (*table)))->tempo_countdown = (uint32)(0);
            ((SpuSequenceSchedulerTrack *)(track_index + (*table)))->tempo_duration = (uint32)(0);
            ((SpuSequenceSchedulerTrack *)(track_index + (*table)))->tempo_period = (uint16)(0);
            ++index;
            ((SpuSequenceSchedulerTrack *)(track_index + (*table)))->left_volume = (uint16)(127);
            pointer = track_index + (*table);
            count = (sint16)scheduler->separation_count;
            ++track_index;
            result = (sint32)index < count;
            pointer->right_volume = (uint16)(127);
        } while (result != 0);
        free((uint8 *)(*table)->stream.data);
        (*table)->stream.data = NULL;
        (*table)->stream.size = 0;
    }
    return result;
}

void spu_sequence_scheduler_pump(SpuSequenceScheduler *scheduler, const SpuSequencePumpOps *ops)
{
    uint32 result = scheduler->processing;
    SpuSequenceSchedulerTrack **slot;
    uint32 sequence = 0, separation;
    SpuSequenceSchedulerTrack *record;
    if (result == 1)
        return;
    scheduler->processing = 1;
    ops->flush();
    result = (uint32)(sint32)(sint16)scheduler->sequence_count;
    if ((sint32)result > 0)
    {
        do
        {
            if ((scheduler->used_mask & (1u << (sequence & 31))) != 0 && (sint16)scheduler->separation_count > 0)
            {
                slot = &scheduler->tracks[sequence];
                separation = 0;
                do
                {
                    record = (*slot) + separation;
                    if ((record->flags & 1) != 0)
                    {
                        ops->play((sint16)sequence, (sint16)separation);
                        if (((*slot)[separation].flags & 16) != 0)
                            ops->crescendo((sint16)sequence, (sint16)separation);
                        if (((*slot)[separation].flags & 32) != 0)
                            ops->decrescendo((sint16)sequence, (sint16)separation);
                        if (((*slot)[separation].flags & 64) != 0)
                            ops->tempo((sint16)sequence, (sint16)separation);
                        if (((*slot)[separation].flags & 128) != 0)
                            ops->tempo((sint16)sequence, (sint16)separation);
                    }
                    if (((*slot)[separation].flags & 2) != 0)
                        ops->pause((sint16)sequence, (sint16)separation);
                    if (((*slot)[separation].flags & 8) != 0)
                        ops->resume((sint16)sequence, (sint16)separation);
                    if (((*slot)[separation].flags & 4) != 0)
                    {
                        ops->stop((sint16)sequence, (sint16)separation);
                        (*slot)[separation].flags = 0;
                    }
                    ++separation;
                } while ((sint32)separation < (sint16)scheduler->separation_count);
            }
            ++sequence;
            result = (sint32)sequence < (sint16)scheduler->sequence_count;
        } while (result != 0);
    }
    scheduler->processing = 0;
}

static SpuSequencePumpOps sequence_pump_ops;

void spu_sequence_bind_pump(const SpuSequencePumpOps *ops)
{
    if (ops == NULL || ops->flush == NULL || ops->play == NULL || ops->crescendo == NULL || ops->decrescendo == NULL || ops->tempo == NULL || ops->pause == NULL || ops->resume == NULL || ops->stop == NULL)
        abort();
    sequence_pump_ops = *ops;
}

void SsSeqCalledTbyT(void)
{
    if (sequence_pump_ops.flush == NULL)
        abort();
    spu_sequence_scheduler_pump(&spu_sequence_scheduler, &sequence_pump_ops);
}

static SpuSequenceCallbacks sequence_callbacks;

static SpuSequenceClock sequence_clock = {60, 1, 0, 0, 0, 0, 127, 0};

SpuSequenceCallbacks *spu_sequence_callbacks(void)
{
    return &sequence_callbacks;
}

void spu_sequence_clear_marks(void)
{
    memset(sequence_callbacks.marks, 0, sizeof(sequence_callbacks.marks));
}

void spu_sequence_dispatch_mark(sint16 sequence, sint16 track, sint16 value)
{
    SpuSequenceMarkCallback callback;
    if (sequence < 0 || sequence >= 32 || track < 0 || track >= 16)
    {
#ifndef NDEBUG
        fprintf(stderr, "PsyQ native SEQ: Invalid mark callback index\n");
#endif
        abort();
    }
    callback = sequence_callbacks.marks[sequence][track];
    if (callback)
        callback(sequence, track, value);
}

SpuSequenceClock *spu_sequence_clock(void)
{
    return &sequence_clock;
}

uint32 spu_sequence_scheduler_initialize_track(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 bank, const uint8 *data, SpuSequenceHandler read_delta)
{
    SpuSequenceSchedulerTrack *record = scheduler->tracks[(uint32)(sint32)sequence];
    uint32 index, cursor, high, middle, tempo, quotient, remainder, delta;
    uint32 product, ticks, threshold, numerator;
    record->bank = (uint16)(bank);
    record->ticks_per_quarter = (uint16)(0);
    record->rpn_parameter = (uint8)(0);
    record->rpn_selector = (uint8)(0);
    record->rpn_phase = (uint8)(0);
    record->nrpn_parameter = (uint8)(0);
    record->nrpn_selector = (uint8)(0);
    record->nrpn_phase = (uint8)(0);
    record->channel = (uint8)(0);
    record->initial_delta = (uint32)(0);
    record->elapsed_ticks = (uint32)(0);
    record->initial_tempo = (uint32)(0);
    record->initial_tick_period = (uint16)(0);
    record->repeat_count = (uint16)(0);
    record->playback_latch = (uint8)(0);
    record->delta = (uint32)(0);
    record->loop_count_pending = (uint8)(0);
    record->loop_remaining = (uint8)(0);
    record->loop_count_set = (uint8)(0);
    record->running_status = (uint8)(0);
    record->event_velocity = (uint16)(127);
    record->channel_mute_mask = (uint16)(0);
    for (index = 0; index < 16; ++index)
    {
        record->channel_program[index] = (uint8)(index);
        record->channel_pan[index] = (uint8)(64);
        record->channel_volume[index] = (uint16)(127);
    }
    record->tick_countdown = (uint16)(1);
    record->stream.cursor = 0;
    high = data[0];
    if (high != 83 && high != 112)
    {
#ifndef NDEBUG
        printf("This is an old SEQ data\n");
#else
        (void)0;
#endif
        return 0;
    }
    record->stream.cursor = 8;
    if (data[7] != 1)
    {
#ifndef NDEBUG
        printf("This is not SEQ data\n");
#else
        (void)0;
#endif
        return (uint32)-1;
    }
    record->stream.cursor = 9;
    high = data[8];
    record->stream.cursor = 10;
    middle = data[9];
    cursor = record->stream.cursor;
    record->ticks_per_quarter = (uint16)((high << 8) | middle);
    record->stream.cursor = (uint32)(cursor + 1);
    if (record->stream.data == NULL || cursor >= record->stream.size || 1u > record->stream.size - cursor)
    {
#ifndef NDEBUG
        fprintf(stderr, "P2: SEQ stream read outside buffer at %u, span 1, size %u\n", cursor, record->stream.size);
#else
        (void)0;
#endif
        abort();
    }
    high = record->stream.data[cursor];
    record->stream.cursor = (uint32)(cursor + 2);
    if (record->stream.data == NULL || cursor >= record->stream.size || 2u > record->stream.size - cursor)
    {
#ifndef NDEBUG
        fprintf(stderr, "P2: SEQ stream read outside buffer at %u, span 2, size %u\n", cursor, record->stream.size);
#else
        (void)0;
#endif
        abort();
    }
    middle = record->stream.data[cursor + 1];
    record->stream.cursor = (uint32)(cursor + 3);
    if (record->stream.data == NULL || cursor >= record->stream.size || 3u > record->stream.size - cursor)
    {
#ifndef NDEBUG
        fprintf(stderr, "P2: SEQ stream read outside buffer at %u, span 3, size %u\n", cursor, record->stream.size);
#else
        (void)0;
#endif
        abort();
    }
    tempo = (high << 16) | (middle << 8) | record->stream.data[cursor + 2];
    if (tempo == 0)
    {
#ifndef NDEBUG
        fprintf(stderr, "P2: SEQ tempo division by zero at 80073F9C\n");
#else
        (void)0;
#endif
        abort();
    }
    quotient = 60000000 / tempo;
    remainder = 60000000 % tempo;
    record->initial_tempo = (uint32)(tempo);
    if ((tempo >> 1) < remainder)
        ++quotient;
    record->initial_tempo = (uint32)(quotient);
    cursor = record->stream.cursor;
    record->tempo = (uint32)(record->initial_tempo);
    record->stream.cursor = (uint32)(cursor + 2);
    delta = read_delta(sequence, 0);
    product = (uint32)(sint32)(sint16)record->ticks_per_quarter * record->initial_tempo;
    cursor = record->stream.cursor;
    high = record->stream.cursor;
    record->stream.loop_start = (uint32)(cursor);
    ticks = scheduler->rate;
    record->initial_delta = (uint32)(delta);
    record->delta = (uint32)(delta);
    record->stream.start = (uint32)(high);
    threshold = ticks * 60;
    if (product * 10 < threshold)
    {
        if (product == 0)
        {
#ifndef NDEBUG
            fprintf(stderr, "P2: SEQ tick division by zero at 80074078\n");
#else
            (void)0;
#endif
            abort();
        }
        quotient = (ticks * 600) / product;
        record->tick_countdown = (uint16)(quotient);
        record->tick_period = (uint16)(quotient);
    }
    else
    {
        if (threshold == 0)
        {
#ifndef NDEBUG
            fprintf(stderr, "P2: SEQ tick division by zero at 800740BC\n");
#else
            (void)0;
#endif
            abort();
        }
        numerator = (uint32)(sint32)(sint16)record->ticks_per_quarter * record->initial_tempo * 10;
        quotient = numerator / threshold;
        numerator = (uint32)(sint32)(sint16)record->ticks_per_quarter * record->initial_tempo * 10;
        remainder = numerator % threshold;
        record->tick_countdown = (uint16)(0xFFFF);
        record->tick_period = (uint16)(quotient);
        if (ticks * 30 < remainder)
            record->tick_period = (uint16)(quotient + 1);
    }
    record->initial_tick_period = (uint16)(record->tick_period);
    return 0;
}

uint32 spu_sequence_scheduler_open(SpuSequenceScheduler *scheduler, const uint8 *sequence, uint32 bytes, sint16 bank, SpuSequenceInitialize initialize, SpuSequenceHandlers *handlers, const SpuSequenceHandlers *default_handlers, SpuToneCommand *commands, const SpuToneCommand *default_commands)
{
    uint32 used = scheduler->used_mask;
    uint32 slot = 0;
    uint8 *stream;
    SpuSequenceSchedulerTrack *record;
    sint16 result;
    if (used == 0xFFFFFFFF || bytes < 16)
    {
#ifndef NDEBUG
        printf("Can't open sequence\n");
#else
        (void)0;
#endif
        return (uint32)-1;
    }
    while (slot < 32 && (used & (1u << slot)) != 0)
        ++slot;
    if (slot >= 32 || (sint16)scheduler->sequence_count <= 0 || slot >= scheduler->sequence_count || scheduler->tracks[slot] == NULL)
    {
#ifndef NDEBUG
        printf("Can't open sequence\n");
#else
        (void)0;
#endif
        return (uint32)-1;
    }
    stream = (uint8 *)malloc(bytes);
    if (stream == NULL)
    {
#ifndef NDEBUG
        printf("Can't open sequence\n");
#else
        (void)0;
#endif
        return (uint32)-1;
    }
    memcpy(stream, sequence, bytes);
    record = scheduler->tracks[slot];
    record->stream.data = stream;
    record->stream.size = bytes;
    scheduler->used_mask = scheduler->used_mask | (1u << slot);
    result = (sint16)initialize((sint16)slot, bank, stream);
    handlers->note_on = default_handlers->note_on;
    handlers->program_change = default_handlers->program_change;
    handlers->meta_event = default_handlers->meta_event;
    handlers->pitch_bend = default_handlers->pitch_bend;
    handlers->control_change = default_handlers->control_change;
    handlers->volume = default_handlers->volume;
    handlers->pan = default_handlers->pan;
    handlers->expression = default_handlers->expression;
    handlers->parameter_lsb = default_handlers->parameter_lsb;
    handlers->parameter_msb = default_handlers->parameter_msb;
    handlers->registered_parameter_lsb = default_handlers->registered_parameter_lsb;
    handlers->registered_parameter_msb = default_handlers->registered_parameter_msb;
    handlers->reverb_depth = default_handlers->reverb_depth;
    handlers->reset_controllers = default_handlers->reset_controllers;
    handlers->data_entry = default_handlers->data_entry;
    handlers->damper = default_handlers->damper;
    handlers->portamento = default_handlers->portamento;
    commands[0].parameter = default_commands[0].parameter;
    commands[1].parameter = default_commands[1].parameter;
    commands[2].parameter = default_commands[2].parameter;
    commands[3].parameter = default_commands[3].parameter;
    commands[4].parameter = default_commands[4].parameter;
    commands[5].parameter = default_commands[5].parameter;
    commands[6].parameter = default_commands[6].parameter;
    commands[7].parameter = default_commands[7].parameter;
    commands[8].parameter = default_commands[8].parameter;
    commands[9].parameter = default_commands[9].parameter;
    commands[10].parameter = default_commands[10].parameter;
    commands[11].parameter = default_commands[11].parameter;
    commands[12].parameter = default_commands[12].parameter;
    commands[13].parameter = default_commands[13].parameter;
    commands[14].parameter = default_commands[14].parameter;
    commands[15].value = default_commands[15].value;
    commands[16].value = default_commands[16].value;
    commands[17].value = default_commands[17].value;
    commands[18].value = default_commands[18].value;
    commands[19].value = default_commands[19].value;
    return result == -1 ? (uint32)-1 : slot;
}

uint32 spu_sequence_scheduler_initialize(SpuSequenceScheduler *scheduler, SpuSequenceVmInitialize initialize_voices)
{
    uint32 voice, field;
    for (voice = 0; voice < 24; ++voice)
    {
        for (field = 0; field < 8; ++field)
            w_u16(0x1F801C00 + voice * 16 + field * 2, spu_sequence_voice_defaults[field]);
    }
    for (field = 0; field < 16; ++field)
        w_u16(0x1F801D80 + field * 2, spu_sequence_common_defaults[field]);
    initialize_voices(24);
    spu_sequence_clear_marks();
    scheduler->rate = 60;
    scheduler->used_mask = 0;
    for (voice = 0; voice < 32; ++voice)
    {
        SpuSequenceSchedulerTrack *track = scheduler->tracks[voice];
        if (track != NULL)
        {
            free((uint8 *)track->stream.data);
            track->stream.data = NULL;
            track->stream.size = 0;
        }
    }
    scheduler->processing = 0;
    return 60;
}

void spu_sequence_scheduler_irq0(SpuSequenceClock *clock)
{
    PsxInterruptCallback previous = clock->previous_vblank;
    PsxInterruptCallback tick;
    if (previous != NULL)
        previous();
    tick = clock->tick;
    if (tick == NULL)
    {
#ifndef NDEBUG
        fprintf(stderr, "P2: Unbound sequence tick callback\n");
#else
        (void)0;
#endif
        abort();
    }
    tick();
}

void spu_sequence_scheduler_half_rate(SpuSequenceClock *clock)
{
    if (clock->phase == 0)
        clock->phase = 1;
    else
    {
        PsxInterruptCallback tick = clock->tick;
        clock->phase = 0;
        if (tick == NULL)
        {
#ifndef NDEBUG
            fprintf(stderr, "P2: Unbound half-rate sequence callback\n");
#else
            (void)0;
#endif
            abort();
        }
        tick();
    }
}

void spu_sequence_scheduler_stop_timer(SpuSequenceClock *clock)
{
    if (clock->manual != 0)
        return;
    clock->half_rate = (uint8)(0);
    if (clock->interrupt == 127)
        return;
    EnterCriticalSectionPSX();
    if (clock->direct_vblank != 0)
    {
        VSyncCallback(NULL);
        clock->direct_vblank = (uint8)(0);
    }
    else if (clock->interrupt == 0)
    {
        InterruptCallback(0, clock->previous_vblank);
        clock->previous_vblank = NULL;
    }
    else
        InterruptCallback(6, NULL);
    ExitCriticalSection();
    clock->interrupt = (uint8)(127);
}

void spu_sequence_scheduler_start_timer(SpuSequenceClock *clock, uint32 mode, PsxInterruptCallback irq0, PsxInterruptCallback half_rate)
{
    uint32 counter = 0xF2000002;
    uint32 target = 0x44E8;
    uint32 interrupt, use_vsync;
    PsxInterruptCallback callback, previous;
    sint32 tick_mode;
    clock->direct_vblank = (uint8)(0);
    tick_mode = clock->mode;
    clock->interrupt = (uint8)(6);
    clock->half_rate = (uint8)(0);
    clock->previous_vblank = NULL;
    if (tick_mode == 0)
    {
        clock->interrupt = (uint8)(127);
        return;
    }
    if (tick_mode == 3)
        target = 0x89D0;
    else if (tick_mode == 5)
    {
        clock->interrupt = (uint8)(0);
        if (mode == 0)
            clock->direct_vblank = (uint8)(1);
        else
        {
            counter = 0xF2000003;
            target = 1;
        }
    }
    else if (tick_mode != 2)
    {
        if (clock->manual != 0)
            return;
        tick_mode = clock->mode;
        if (tick_mode == 0)
        {
#ifndef NDEBUG
            fprintf(stderr, "P2: Sound timer division by zero at 80074604\n");
#else
            (void)0;
#endif
            abort();
        }
        if (tick_mode < 70)
        {
            target = (uint32)(2116800 / tick_mode);
            clock->half_rate = (uint8)(clock->half_rate + 1);
        }
        else
            target = (uint32)(4233600 / tick_mode);
    }
    use_vsync = clock->direct_vblank;
    EnterCriticalSectionPSX();
    if (use_vsync != 0)
    {
        callback = clock->tick;
        VSyncCallback(callback);
    }
    else
    {
        ResetRCnt(counter);
        SetRCnt(counter, (uint16)target, 0x1000);
        interrupt = clock->interrupt;
        if (interrupt == 0)
        {
            previous = InterruptCallback(0, NULL);
            interrupt = clock->interrupt;
            clock->previous_vblank = previous;
            callback = irq0;
        }
        else
        {
            callback = half_rate;
            if (clock->half_rate == 0)
                callback = clock->tick;
        }
        InterruptCallback((sint32)interrupt, callback);
    }
    ExitCriticalSection();
}

SpuSequenceScheduler spu_sequence_scheduler;
SpuToneCommand spu_tone_commands[20];
SpuSequenceHandlers spu_sequence_handlers;

static uint32 spu_sequence_set_volume(sint16 sequence, sint16 left, sint16 right, sint16 mode)
{
    return spu_sequence_vm_set_sequence_volume(&spu_sequence_scheduler, sequence, left, right, mode);
}

sint16 SsIsEos(sint16 sequence, sint16 separation)
{
    return spu_sequence_scheduler.tracks[sequence][separation].playback_latch;
}

void SsSeqSetVol(sint16 sequence, sint16 left, sint16 right)
{
    spu_sequence_vm_set_sequence_volume(&spu_sequence_scheduler, sequence, left, right, 1);
}

uint32 SsSeqPlay(sint16 sequence, uint8 mode, sint16 repetitions)
{
    return spu_sequence_scheduler_play_mode(&spu_sequence_scheduler, sequence, 0, mode, repetitions, spu_sequence_set_volume);
}

void SsSeqStop(sint16 sequence)
{
    spu_sequence_scheduler_stop(&spu_sequence_scheduler, sequence, 0, spu_sequence_vm_sequence_key_off);
}

uint32 spu_sequence_vm_set_sequence_volume(SpuSequenceScheduler *scheduler, sint16 handle, sint16 left, sint16 right, sint16 mode)
{
    SpuSequenceSchedulerTrack *entry = scheduler->tracks[((uint32)handle & 255)];
    sint32 voice = 0;
    spu_vm_state()->note.sequence = (uint16)(handle);
    entry += (((uint32)handle & 0xFF00) >> 8);
    entry->left_volume = (uint16)(left);
    {
        uint32 valid_left = entry->left_volume < 128;
        entry->right_volume = (uint16)(right);
        if (!valid_left)
            entry->left_volume = (uint16)(127);
    }
    if (entry->right_volume >= 128)
        entry->right_volume = (uint16)(127);
    if (mode == 1 && spu_vm_state()->events.voice_count != 0)
    {
        do
        {
            const SpuVoiceState *attributes = &spu_vm_state()->voices[(sint16)voice];
            if (attributes->sequence == (uint16)handle)
            {
                sint32 scaled = (sint16)attributes->velocity * (sint16)entry->channel_volume[entry->channel];
                uint32 bank_volume = *(spu_vab_native_current().header + 24);
                sint32 sequence_volume = scaled / 127;
                const uint8 *program = spu_vab_native_current().programs + (uint32)(sint32)(sint16)attributes->program * 16;
                const uint8 *tone;
                uint32 level;
                uint32 left_volume;
                uint32 right_volume;
                uint32 pan;
                uint32 square;
                sint32 high;
                scaled = (sint32)(bank_volume * ((uint32)sequence_volume * 16383));
                level = (uint32)(scaled / 16129) * *(program + 1);
                tone = spu_vab_native_current().tones + ((uint32)(sint32)(sint16)attributes->program_tone_index * 16 + (uint32)(sint32)(sint16)attributes->tone) * 32;
                level = (level * *(tone + 2)) / 16129;
                left_volume = (level * entry->left_volume) / 127;
                right_volume = (level * entry->right_volume) / 127;
                pan = *(tone + 3);
                if (pan < 64)
                    right_volume = (right_volume * pan) / 63;
                else
                    left_volume = (left_volume * (127 - pan)) / 63;
                program = spu_vab_native_current().programs + (uint32)(sint32)(sint16)attributes->program * 16;
                pan = *(program + 4);
                if (pan < 64)
                    right_volume = ((uint16)right_volume * pan) / 63;
                else
                    left_volume = (uint32)(((sint32)(uint16)left_volume * (127 - (sint32)pan)) / 63);
                pan = attributes->pan;
                if (pan < 64)
                    right_volume = ((uint16)right_volume * pan) / 63;
                else
                    left_volume = (uint32)(((sint32)(uint16)left_volume * (127 - (sint32)pan)) / 63);
                if (spu_vm_state()->mono_mode == 1)
                {
                    if ((uint16)left_volume < (uint16)right_volume)
                        left_volume = right_volume;
                    else
                        right_volume = left_volume;
                }
                square = (uint32)(uint16)left_volume * (uint16)left_volume;
                high = (sint32)(((sint64)(sint32)square * (sint32)0x80020009u) >> 32);
                left_volume = (uint32)(((sint32)((uint32)high + square) >> 13) - ((sint32)square >> 31));
                spu_vm_state()->registers[(uint32)(sint32)(sint16)voice * 8] = (uint16)(left_volume);
                square = (uint32)(uint16)right_volume * (uint16)right_volume;
                high = (sint32)(((sint64)(sint32)square * (sint32)0x80020009u) >> 32);
                right_volume = (uint32)(((sint32)((uint32)high + square) >> 13) - ((sint32)square >> 31));
                spu_vm_state()->registers[(uint32)(sint32)(sint16)voice * 8 + 1] = (uint16)(right_volume);
                spu_vm_state()->dirty[(uint32)(sint32)(sint16)voice] = (uint8)(spu_vm_state()->dirty[(uint32)(sint32)(sint16)voice] | 3);
            }
            ++voice;
        } while ((sint16)voice < spu_vm_state()->events.voice_count);
    }
    return (uint32)(sint32)(sint16)spu_vm_state()->note.sequence;
}

uint32 spu_sequence_vm_get_sequence_volume(SpuSequenceScheduler *scheduler, sint16 handle, uint16 *left, uint16 *right)
{
    SpuSequenceSchedulerTrack *entry = scheduler->tracks[((uint32)handle & 0xFF)];
    spu_vm_state()->note.sequence = (uint16)(handle);
    entry += (((uint32)handle & 0xFF00) >> 8);
    *left = entry->left_volume;
    *right = entry->right_volume;
    return (uint32)(sint32)(sint16)spu_vm_state()->note.sequence;
}

uint32 spu_sequence_vm_sequence_key_off(sint16 sequence)
{
    uint32 voice = 0;
    if (spu_vm_state()->events.voice_count != 0)
    {
        do
        {
            if ((sint16)spu_vm_state()->voices[voice].sequence == sequence)
            {
                spu_vm_state()->note.voice = (uint16)(voice);
                spu_sequence_vm_key_off_now();
            }
            voice = (uint8)(voice + 1);
        } while (voice < spu_vm_state()->events.voice_count);
    }
    return voice;
}

uint32 spu_sequence_vm_set_volume(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 bank, sint16 program, uint16 volume, uint16 pan)
{
    SpuSequenceSchedulerTrack *entry;
    uint32 packed = (uint16)sequence;
    uint32 updated = 0;
    sint32 voice = 0;
    if ((packed & 255) >= 32 || scheduler->tracks[packed & 255] == NULL || (packed >> 8) >= scheduler->separation_count)
    {
#ifndef NDEBUG
        fprintf(stderr, "PsyQ native SEQ: Invalid volume sequence track\n");
#endif
        abort();
    }
    entry = scheduler->tracks[packed & 255] + (packed >> 8);
    spu_vab_select_native((uint16)bank, program);
    spu_vm_state()->note.sequence = (uint16)(sequence);
    if (spu_vm_state()->events.voice_count != 0)
    {
        do
        {
            const SpuVoiceState *attributes = &spu_vm_state()->voices[(sint16)voice];
            if ((sint16)attributes->sequence == sequence && (sint16)attributes->program == program && (sint16)attributes->bank == bank)
            {
                uint16 *channel = &entry->channel_volume[entry->channel];
                sint32 channel_volume = (sint16)*channel;
                sint32 scaled;
                uint32 bank_volume;
                const uint8 *program_offset;
                const uint8 *tone;
                uint32 level;
                uint32 left;
                uint32 right;
                uint32 balance;
                if (channel_volume != volume && channel_volume == 0)
                    *channel = (uint16)(1);
                scaled = (sint16)attributes->velocity * (sint32)volume;
                bank_volume = *(spu_vab_native_current().header + 24);
                scaled = (sint32)((uint32)(scaled / 127) * (bank_volume * 16383));
                program_offset = spu_vab_native_current().programs + (uint32)(sint32)program * 16;
                level = (uint32)(scaled / 16129) * *(program_offset + 1);
                tone = spu_vab_native_current().tones + ((uint32)(sint32)(sint16)attributes->program_tone_index * 16 + (uint32)(sint32)(sint16)attributes->tone) * 32;
                level = (level * *(tone + 2)) / 16129;
                left = (level * entry->left_volume) / 127;
                right = (level * entry->right_volume) / 127;
                balance = *(tone + 3);
                if (balance < 64)
                    right = (right * balance) / 63;
                else
                    left = (left * (127 - balance)) / 63;
                program_offset = spu_vab_native_current().programs + (uint32)(sint32)(sint16)attributes->program * 16;
                balance = *(program_offset + 4);
                if (balance < 64)
                    right = (right * balance) / 63;
                else
                    left = (left * (127 - balance)) / 63;
                balance = (uint8)pan;
                if (balance < 64)
                    right = (right * balance) / 63;
                else
                    left = (left * (127 - balance)) / 63;
                if (spu_vm_state()->mono_mode == 1)
                {
                    if (left < right)
                        left = right;
                    else
                        right = left;
                }
                spu_vm_state()->registers[(uint32)(sint32)(sint16)voice * 8] = (uint16)((left * left) / 16383);
                spu_vm_state()->registers[(uint32)(sint32)(sint16)voice * 8 + 1] = (uint16)((right * right) / 16383);
                ++updated;
                spu_vm_state()->dirty[(uint32)(sint32)(sint16)voice] = (uint8)(spu_vm_state()->dirty[(uint32)(sint32)(sint16)voice] | 3);
            }
            ++voice;
        } while ((sint16)voice < spu_vm_state()->events.voice_count);
    }
    return updated;
}

uint32 spu_sequence_vm_key_off_now(void)
{
    uint32 voice = spu_vm_state()->note.voice;
    uint32 low = voice < 16 ? 1u << (voice & 31) : 0;
    uint32 high = voice < 16 ? 0 : 1u << ((voice - 16) & 31);
    uint32 low_pending;
    uint32 high_pending;
    uint32 result;
    spu_vm_state()->voices[voice].status = (uint8)(0);
    low_pending = spu_vm_state()->events.key_off[0];
    high_pending = spu_vm_state()->events.key_off[1];
    spu_vm_state()->voices[voice].pitch = (uint16)(0);
    spu_vm_state()->voices[voice].sample = (uint16)(0);
    result = spu_vm_state()->events.key_on[0];
    low_pending |= low;
    spu_vm_state()->events.key_off[0] = (uint16)(low_pending);
    spu_vm_state()->events.key_on[0] = (uint16)(result & ~low_pending);
    result = spu_vm_state()->events.key_on[1];
    high_pending |= high;
    spu_vm_state()->events.key_off[1] = (uint16)(high_pending);
    result &= ~high_pending;
    spu_vm_state()->events.key_on[1] = (uint16)(result);
    return result;
}

uint32 spu_sequence_vm_noise_voice(uint32 mode, uint32 voices)
{
    uint32 high, low, previous, queued, result;
    queued = spu_key_control->event_mode & 1;
    high = queued != 0 ? spu_key_control->noise_mask[1] : r_u16(SPU_REGISTER_BASE + 406);
    low = queued != 0 ? spu_key_control->noise_mask[0] : r_u16(SPU_REGISTER_BASE + 404);
    previous = low | ((high & 255) << 16);
    if (mode != 0 && mode != 1)
        return previous & 0xFFFFFF;
    queued = spu_key_control->event_mode & 1;
    low = queued != 0 ? spu_key_control->noise_mask[0] : r_u16(SPU_REGISTER_BASE + 404);
    low = mode == 1 ? low | voices : low & ~voices;
    if (queued != 0)
        spu_key_control->noise_mask[0] = (uint16)low;
    else
        w_u16(SPU_REGISTER_BASE + 404, low);
    high = queued != 0 ? spu_key_control->noise_mask[1] : r_u16(SPU_REGISTER_BASE + 406);
    high = mode == 1 ? high | ((voices >> 16) & 255) : high & ~((voices >> 16) & 255);
    if (queued != 0)
        spu_key_control->noise_mask[1] = (uint16)high;
    else
        w_u16(SPU_REGISTER_BASE + 406, high);
    if (queued != 0)
        spu_key_control->event_flags |= 4;
    result = mode == 1 ? previous | (voices & 0xFFFFFF) : previous & ~(voices & 0xFFFFFF);
    return result & 0xFFFFFF;
}

uint32 spu_sequence_vm_allocate(void)
{
    uint32 selected = 99, minimum_envelope = 0xFFFF, matches = 0;
    SpuVoiceState *record;
    uint32 candidate = 99, minimum_priority, voice = 0, envelope, capacity;
    sint32 age = 0, priority, current_age;
    capacity = spu_vm_state()->events.voice_count;
    minimum_priority = spu_vm_state()->note.priority;
    if (capacity != 0)
    {
        do
        {
            record = &spu_vm_state()->voices[(voice & 255)];
            if (record->status == 0 && record->envelope == 0)
            {
                selected = voice;
                break;
            }
            priority = (sint16)record->priority;
            if (priority < (sint32)(uint16)minimum_priority)
            {
                minimum_priority = (uint32)priority;
                candidate = voice;
                minimum_envelope = record->envelope;
                age = record->age;
                matches = 1;
            }
            else if (priority == (sint32)(uint16)minimum_priority)
            {
                envelope = record->envelope;
                ++matches;
                if (envelope < (uint16)minimum_envelope)
                {
                    age = record->age;
                    minimum_envelope = envelope;
                    candidate = voice;
                }
                else if (envelope == (uint16)minimum_envelope)
                {
                    current_age = (sint16)record->age;
                    if (age < current_age)
                    {
                        age = current_age;
                        candidate = voice;
                    }
                }
            }
            ++voice;
        } while ((voice & 255) < spu_vm_state()->events.voice_count);
    }
    if ((selected & 255) == 99)
    {
        selected = candidate;
        if ((matches & 255) == 0)
            selected = spu_vm_state()->events.voice_count;
    }
    capacity = spu_vm_state()->events.voice_count;
    selected &= 255;
    if (selected < capacity)
    {
        for (voice = 0; (voice & 255) < capacity; ++voice)
        {
            record = &spu_vm_state()->voices[voice & 255];
            record->age = (uint16)(record->age + 1);
        }
        record = &spu_vm_state()->voices[selected];
        record->age = (uint16)(0);
        record->priority = (uint16)(spu_vm_state()->note.priority);
        if (record->status == 2)
            spu_sequence_vm_noise_voice(0, 0xFFFFFF);
    }
    return selected;
}

uint32 spu_sequence_vm_prepare_voice(void)
{
    uint32 voice = (uint32)(sint32)(sint16)spu_vm_state()->note.voice;
    const uint8 *data;
    const uint8 *table;
    uint32 tone, index, value;
    sint32 sample;
    spu_vm_state()->note.register_index = (uint16)(voice * 8);
    tone = 16 * spu_vm_state()->note.program_tone_index + spu_vm_state()->note.tone;
    spu_vm_state()->note.tone_index = (uint16)(tone);
    spu_vm_state()->voices[voice].envelope = (uint16)(0x7FFF);
    for (index = 0; index < 16; ++index)
    {
        voice = (uint32)(sint32)(sint16)spu_vm_state()->note.voice;
        value = spu_vm_state()->events.silence_history[index];
        spu_vm_state()->events.silence_history[index] = value & ~(1u << (voice & 31));
    }
    value = spu_vm_state()->note.sample;
    sample = ((sint32)(sint16)value - 1) / 2;
    data = spu_vab_native_current().programs + (uint32)sample * 16;
    index = (uint32)(sint32)(sint16)spu_vm_state()->note.register_index;
    value = xport_load_le16(data + ((value & 1) != 0 ? 12 : 14));
    spu_vm_state()->registers[index + 3] = (uint16)(value);
    voice = (uint32)(sint32)(sint16)spu_vm_state()->note.voice;
    spu_vm_state()->dirty[voice] = (uint8)(spu_vm_state()->dirty[voice] | 8);
    tone = 16 * spu_vm_state()->note.program_tone_index + spu_vm_state()->note.tone;
    table = spu_vab_native_current().tones;
    index = (uint32)(sint32)(sint16)spu_vm_state()->note.register_index;
    value = xport_load_le16(table + tone * 32 + 16);
    spu_vm_state()->registers[index + 4] = (uint16)(value);
    tone = 16 * spu_vm_state()->note.program_tone_index + spu_vm_state()->note.tone;
    index = (uint32)(sint32)(sint16)spu_vm_state()->note.register_index;
    value = xport_load_le16(table + tone * 32 + 18);
    value += spu_vm_state()->release_offset;
    spu_vm_state()->registers[index + 5] = (uint16)(value);
    voice = (uint32)(sint32)(sint16)spu_vm_state()->note.voice;
    value = spu_vm_state()->dirty[voice] | 0x30;
    spu_vm_state()->dirty[voice] = (uint8)(value);
    return value;
}

uint16 spu_sequence_vm_note_pitch(void)
{
    sint32 semitone = (sint16)(spu_vm_state()->note.note + 60 - spu_vm_state()->note.center_note);
    sint32 octave;
    sint32 remainder, shift;
    uint32 fine = spu_vm_state()->note.fine_tune >> 3;
    uint32 index, pitch;
    octave = semitone / 12;
    remainder = (sint16)(semitone - octave * 12);
    if (remainder < 0)
    {
        remainder += 12;
        --octave;
    }
    if (fine >= 16)
        fine = 15;
    index = (uint32)(remainder * 16) + fine;
    if (index >= 192)
    {
#ifndef NDEBUG
        fprintf(stderr, "P2: Pitch table index out of range: %u\n", index);
#else
        (void)0;
#endif
        abort();
    }
    pitch = spu_note_pitch_table[index];
    shift = (sint16)(octave - 5);
    if (shift > 0)
        pitch <<= (uint32)shift & 31;
    else if (shift < 0)
        pitch >>= (uint32)(0u - (uint32)shift) & 31;
    return (uint16)pitch;
}

uint32 spu_sequence_vm_noise_off(uint8 voice)
{
    uint32 offset = (uint32)voice * 52;
    uint32 registers;
    spu_vm_state()->voices[voice].status = 0;
    registers = SPU_REGISTER_BASE;
    spu_vm_state()->voices[voice].pitch = 0;
    w_u16(registers + 404, 0);
    w_u16(registers + 406, 0);
    return offset;
}

uint32 spu_sequence_vm_select_tones(uint8 *tones, uint8 *samples)
{
    const uint8 *data;
    uint32 tone = 0, count = 0;
    uint8 note, sample;
    if (spu_vm_state()->note.tone_count != 0)
    {
        do
        {
            data = spu_vab_native_current().tones + (16 * spu_vm_state()->note.program_tone_index + (tone & 255)) * 32;
            sample = *(data + 6);
            note = spu_vm_state()->note.note;
            if (note >= sample)
            {
                sample = *(data + 7);
                if (sample >= note)
                {
                    sample = *(data + 22);
                    samples[count & 255] = (uint8)sample;
                    tones[count & 255] = (uint8)tone;
                    ++count;
                }
            }
            ++tone;
        } while ((tone & 255) < spu_vm_state()->note.tone_count);
    }
    return count & 255;
}

uint16 spu_sequence_vm_tone_pitch(uint16 note, uint16 fraction)
{
    uint32 tone = spu_vm_state()->note.program_tone_index * 16 + spu_vm_state()->note.tone;
    const uint8 *data = spu_vab_native_current().tones + tone * 32;
    sint32 fine = (fraction + *(data + 5)) / 8;
    sint32 carry = fine >= 16;
    sint32 semitone, octave, remainder, shift;
    uint32 index, pitch;
    if (carry != 0)
        fine -= 16;
    semitone = (sint16)(carry + note + 60 - *(data + 4));
    octave = semitone / 12;
    remainder = (sint16)(semitone - octave * 12);
    if (remainder < 0)
    {
        remainder += 12;
        --octave;
    }
    index = (uint32)(remainder * 16 + (sint16)fine);
    if (index >= 192)
    {
#ifndef NDEBUG
        fprintf(stderr, "P2: Pitch table index out of range: %u\n", index);
#else
        (void)0;
#endif
        abort();
    }
    pitch = spu_note_pitch_table[index];
    shift = (sint16)(octave - 5);
    if (shift > 0)
        pitch <<= (uint32)shift & 31;
    else if (shift < 0)
        pitch >>= (uint32)(0u - (uint32)shift) & 31;
    return (uint16)pitch;
}

void spu_sequence_vm_voice_bend_offset(uint8 voice, sint16 bend)
{
    SpuVoiceState *record;
    const uint8 *tone;
    sint32 scaled, quotient, coarse, fraction;
    uint32 tone_index;
    uint16 pitch;
    if (voice >= SPU_VOICE_COUNT)
        return;
    record = spu_vm_state()->voices + voice;
    spu_vm_state()->note.program_tone_index = (uint8)record->program_tone_index;
    spu_vm_state()->note.voice = voice;
    spu_vm_state()->note.tone = (uint8)record->tone;
    tone_index = spu_vm_state()->note.tone + 16 * spu_vm_state()->note.program_tone_index;
    tone = spu_vab_native_current().tones + tone_index * 32;
    scaled = bend * tone[bend < 0 ? 12 : 13];
    quotient = scaled / 127;
    coarse = record->note + quotient;
    if (bend < 0)
    {
        --coarse;
        fraction = quotient + 127;
    }
    else
        fraction = scaled - quotient * 127;
    pitch = spu_sequence_vm_tone_pitch((uint16)coarse, (uint16)fraction);
    spu_vm_state()->registers[(uint32)voice * 8 + 2] = pitch;
    spu_vm_state()->dirty[voice] |= 4;
}

static SpuVoiceState *spu_sequence_vm_matching_voice(uint16 voice, sint16 bank, sint16 program, sint16 note)
{
    SpuVoiceState *record;
    if (voice >= SPU_VOICE_COUNT)
        return NULL;
    record = spu_vm_state()->voices + voice;
    if ((sint16)record->bank != bank)
        return NULL;
    if ((sint16)record->program != program)
        return NULL;
    if ((sint16)record->note != note)
        return NULL;
    return record;
}

sint32 spu_sequence_vm_voice_set_pitch(uint16 voice, sint16 bank, sint16 program, sint16 note, uint16 new_note, uint16 fine)
{
    SpuVoiceState *record = spu_sequence_vm_matching_voice(voice, bank, program, note);
    uint16 pitch;
    if (record == NULL)
        return -1;
    spu_vab_select_native((uint16)bank, program);
    spu_vm_state()->note.sequence = 33;
    spu_vm_state()->note.voice = voice;
    spu_vm_state()->note.tone = (uint8)record->tone;
    pitch = spu_sequence_vm_tone_pitch(new_note, fine);
    spu_vm_state()->registers[(uint32)voice * 8 + 2] = pitch;
    spu_vm_state()->dirty[voice] |= 4;
    return 0;
}

sint32 spu_sequence_vm_voice_set_adsr(uint16 voice, sint16 bank, sint16 program, sint16 note, uint16 adsr1, uint16 adsr2)
{
    if (spu_sequence_vm_matching_voice(voice, bank, program, note) == NULL)
        return -1;
    spu_vm_state()->registers[(uint32)voice * 8 + 4] = adsr1;
    spu_vm_state()->registers[(uint32)voice * 8 + 5] = adsr2;
    spu_vm_state()->dirty[voice] |= 0x30;
    return 0;
}

uint32 spu_sequence_vm_voice_bend(sint16 voice, sint16 sequence, sint16 bank, sint16 program, uint16 bend)
{
    uint32 index = (uint32)(sint32)voice;
    SpuVoiceState *record = &spu_vm_state()->voices[index];
    uint32 tone, note, pitch;
    sint32 amount = (sint16)(bend - 64);
    sint32 scaled, coarse, fraction;
    if ((sint16)record->sequence != sequence)
        return 0;
    if ((sint16)record->bank != bank)
        return 0;
    if ((sint16)record->program != program)
        return 0;
    tone = (uint16)(record->tone + 16 * spu_vm_state()->note.program_tone_index);
    note = record->note;
    if (amount > 0)
    {
        scaled = amount * *(spu_vab_native_current().tones + tone * 32 + 13);
        coarse = (sint32)note + scaled / 63;
        fraction = (scaled % 63) * 2;
    }
    else if (amount < 0)
    {
        scaled = amount * *(spu_vab_native_current().tones + tone * 32 + 12);
        coarse = (sint32)note + scaled / 64 - 1;
        fraction = (scaled % 64) * 2 + 127;
    }
    else
    {
        coarse = (sint32)note;
        fraction = 0;
    }
    tone = (uint8)record->tone;
    spu_vm_state()->note.voice = (uint16)((uint16)voice);
    spu_vm_state()->note.tone = (uint8)(tone);
    pitch = spu_sequence_vm_tone_pitch((uint16)coarse, (uint16)fraction);
    spu_vm_state()->registers[index * 8 + 2] = (uint16)(pitch);
    spu_vm_state()->dirty[index] = (uint8)(spu_vm_state()->dirty[index] | 4);
    return 1;
}

uint32 spu_sequence_vm_sequence_bend(sint16 sequence, sint16 bank, sint16 program, uint16 bend)
{
    uint32 voice = 0, result = 0;
    spu_vab_select_native((uint16)bank, program);
    spu_vm_state()->note.sequence = (uint16)((uint16)sequence);
    if (spu_vm_state()->events.voice_count != 0)
    {
        do
        {
            result += (uint32)(sint32)(sint16)spu_sequence_vm_voice_bend((sint16)voice, sequence, bank, program, bend);
            ++voice;
        } while ((sint16)voice < spu_vm_state()->events.voice_count);
    }
    return result;
}

uint32 spu_sequence_vm_key_off(sint16 sequence, sint16 bank, sint16 program, uint16 note)
{
    SpuVoiceState *record;
    uint32 voice = 0, result = 0;
    if (spu_vm_state()->events.voice_count != 0)
    {
        do
        {
            record = &spu_vm_state()->voices[(voice & 255)];
            if ((sint16)record->note == (sint32)note && (sint16)record->program == program && (sint16)record->sequence == sequence && (sint16)record->bank == bank)
            {
                if ((sint16)record->sample == 255)
                {
                    ++result;
                    spu_sequence_vm_noise_off((uint8)voice);
                }
                else
                {
                    spu_vm_state()->note.voice = (uint16)(voice & 255);
                    spu_sequence_vm_key_off_now();
                    ++result;
                }
            }
            ++voice;
        } while ((voice & 255) < spu_vm_state()->events.voice_count);
    }
    return result;
}

SpuReverbAttr spu_reverb_attributes;

static void spu_vm_modulation_start(uint16 *fields, sint16 from, sint16 to, sint16 ticks)
{
    sint32 difference, distance, quotient;
    if (from == to)
        return;
    fields[0] = 1;
    fields[4] = (uint16)from;
    fields[5] = (uint16)to;
    difference = (sint32)from - to;
    distance = difference < 0 ? -difference : difference;
    if (distance < ticks)
    {
        quotient = ticks / difference;
        fields[1] = 1;
        fields[2] = (uint16)quotient;
        fields[3] = (uint16)quotient;
    }
    else
    {
        if (ticks == 0)
        {
            trap(7);
            abort();
        }
        quotient = difference / ticks;
        fields[2] = 0;
        fields[1] = (uint16)quotient;
    }
}

static sint32 spu_vm_modulation_step(uint16 *fields)
{
    uint16 previous, current;
    sint16 step, target;
    if ((sint16)fields[2] != 0)
    {
        previous = fields[3];
        fields[3] = (uint16)(previous - 1);
        if ((sint16)previous > 0)
            return 0;
        fields[3] = fields[2];
    }
    current = (uint16)(fields[4] + fields[1]);
    step = (sint16)fields[1];
    fields[4] = current;
    if (step > 0)
    {
        target = (sint16)fields[5];
        if ((sint16)current >= target)
        {
            fields[4] = (uint16)target;
            fields[0] = 0;
        }
    }
    else if (step < 0)
    {
        target = (sint16)fields[5];
        if ((sint16)current <= target)
        {
            fields[4] = (uint16)target;
            fields[0] = 0;
        }
    }
    return 1;
}

static uint32 spu_vm_modulation_pan(uint32 volume, uint32 factor)
{
    uint32 product = (uint16)volume * factor;
    if ((sint32)product < 0)
        product += 63;
    return product >> 6;
}

static void spu_vm_modulation_volume(uint32 voice, sint32 velocity)
{
    uint32 master, product, left, right, pan, value;
    sint32 scaled;
    const uint8 *header = spu_vab_native_current().header;
    master = header[24];
    product = (uint32)velocity * (master * 16383);
    scaled = (sint32)product / 16129;
    product = (uint32)scaled * spu_vm_state()->note.program_volume;
    product *= spu_vm_state()->note.tone_volume;
    value = product / 16129;
    pan = spu_vm_state()->note.tone_pan;
    if (pan < 64)
    {
        left = value;
        right = (value * pan) >> 6;
    }
    else
    {
        right = value;
        left = (value * (127 - pan)) >> 6;
    }
    pan = spu_vm_state()->note.program_pan;
    if (pan < 64)
        right = spu_vm_modulation_pan(right, pan);
    else
        left = spu_vm_modulation_pan(left, 127 - pan);
    pan = spu_vm_state()->note.pan;
    if (pan < 64)
        right = spu_vm_modulation_pan(right, pan);
    else
        left = spu_vm_modulation_pan(left, 127 - pan);
    if ((sint16)spu_vm_state()->mono_mode == 1)
    {
        if ((uint16)left < (uint16)right)
            left = right;
        else
            right = left;
    }
    spu_vm_state()->registers[voice * 8 + 1] = (uint16)right;
    value = spu_vm_state()->dirty[voice];
    spu_vm_state()->registers[voice * 8] = (uint16)left;
    spu_vm_state()->dirty[voice] = (uint8)(value | 3);
}

void spu_sequence_vm_volume_modulation_start(sint16 voice, sint16 from, sint16 to, sint16 ticks)
{
    spu_vm_modulation_start(spu_vm_state()->voices[voice].volume_modulation, from, to, ticks);
}

void spu_sequence_vm_pan_modulation_start(sint16 voice, sint16 from, sint16 to, sint16 ticks)
{
    spu_vm_modulation_start(spu_vm_state()->voices[voice].pitch_modulation, from, to, ticks);
}

void spu_sequence_vm_volume_modulation_tick(uint32 voice)
{
    uint16 *fields = spu_vm_state()->voices[voice].volume_modulation;
    sint16 velocity;
    if (!spu_vm_modulation_step(fields))
        return;
    velocity = (sint16)fields[4];
    spu_vm_state()->note.velocity = (uint8)velocity;
    spu_vm_modulation_volume(voice, velocity);
}

void spu_sequence_vm_pan_modulation_tick(uint32 voice)
{
    uint16 *fields = spu_vm_state()->voices[voice].pitch_modulation;
    if (!spu_vm_modulation_step(fields))
        return;
    spu_vm_state()->note.pan = (uint8)fields[4];
    spu_vm_modulation_volume(voice, spu_vm_state()->note.velocity);
}

uint32 spu_sequence_vm_flush(void)
{
    uint32 ring = spu_vm_state()->events.history_index;
    uint32 capacity = spu_vm_state()->events.voice_count;
    uint32 *history;
    SpuVoiceState *record;
    uint32 voice, registers, value, quiet = 0xFFFFFFFF;
    const uint16 *buffered;
    uint32 off_low, off_high, on_low, on_high, reverb_low, reverb_high;
    ring = (ring + 1) & 15;
    spu_vm_state()->events.history_index = ring;
    history = &spu_vm_state()->events.silence_history[ring];
    *history = 0;
    if (capacity != 0)
    {
        registers = SPU_REGISTER_BASE;
        for (voice = 0; voice < capacity; ++voice)
        {
            record = &spu_vm_state()->voices[voice];
            record->envelope = r_u16(registers + voice * 16 + 12);
            if (record->envelope == 0)
                *history = *history | (1u << (voice & 31));
        }
    }
    if (spu_vm_state()->events.silence_cleanup_disabled == 0)
    {
        for (voice = 0; voice < 15; ++voice)
            quiet &= spu_vm_state()->events.silence_history[voice];
        voice = 0;
        if (spu_vm_state()->events.voice_count != 0)
        {
            do
            {
                if ((quiet & (1u << (voice & 31))) != 0)
                {
                    record = &spu_vm_state()->voices[voice];
                    if (record->status == 2)
                        spu_sequence_vm_noise_voice(0, 0xFFFFFF);
                    record->status = 0;
                }
                ++voice;
            } while ((sint32)voice < spu_vm_state()->events.voice_count);
        }
    }
    value = spu_vm_state()->events.key_off[0];
    on_low = spu_vm_state()->events.key_on[0];
    off_high = spu_vm_state()->events.key_off[1];
    spu_vm_state()->events.key_on[0] = (uint16)(on_low & ~value);
    spu_vm_state()->events.key_on[1] = (uint16)(spu_vm_state()->events.key_on[1] & ~off_high);
    for (voice = 0; voice < 24; ++voice)
    {
        record = &spu_vm_state()->voices[voice];
        if ((sint16)record->volume_modulation[0] != 0)
        {
            if (spu_voice_modulation_callbacks.volume == NULL)
            {
#ifndef NDEBUG
                fprintf(stderr, "P2: Unbound SPU volume modulation callback for voice %u\n", voice);
#else
                (void)0;
#endif
                abort();
            }
            spu_voice_modulation_callbacks.volume(voice);
        }
        if ((sint16)record->pitch_modulation[0] != 0)
        {
            if (spu_voice_modulation_callbacks.pitch == NULL)
            {
#ifndef NDEBUG
                fprintf(stderr, "P2: Unbound SPU pitch modulation callback for voice %u\n", voice);
#else
                (void)0;
#endif
                abort();
            }
            spu_voice_modulation_callbacks.pitch(voice);
        }
    }
    for (voice = 0; voice < 24; ++voice)
    {
        buffered = spu_vm_state()->registers + voice * 8;
        if ((spu_vm_state()->dirty[voice] & 1) != 0)
        {
            registers = SPU_REGISTER_BASE + voice * 16;
            w_u16(registers, buffered[0]);
            w_u16(registers + 2, buffered[1]);
        }
        if ((spu_vm_state()->dirty[voice] & 4) != 0)
        {
            registers = SPU_REGISTER_BASE + voice * 16;
            w_u16(registers + 4, buffered[2]);
        }
        if ((spu_vm_state()->dirty[voice] & 8) != 0)
        {
            registers = SPU_REGISTER_BASE + voice * 16;
            w_u16(registers + 6, buffered[3]);
        }
        if ((spu_vm_state()->dirty[voice] & 16) != 0)
        {
            registers = SPU_REGISTER_BASE + voice * 16;
            w_u16(registers + 8, buffered[4]);
            w_u16(registers + 10, buffered[5]);
        }
        spu_vm_state()->dirty[voice] = (uint8)(0);
    }
    registers = SPU_REGISTER_BASE;
    off_low = spu_vm_state()->events.key_off[0];
    off_high = spu_vm_state()->events.key_off[1];
    on_low = spu_vm_state()->events.key_on[0];
    on_high = spu_vm_state()->events.key_on[1];
    reverb_low = spu_vm_state()->events.reverb[0];
    reverb_high = spu_vm_state()->events.reverb[1];
    spu_vm_state()->events.key_off[0] = (uint16)(0);
    spu_vm_state()->events.key_off[1] = (uint16)(0);
    spu_vm_state()->events.key_on[0] = (uint16)(0);
    spu_vm_state()->events.key_on[1] = (uint16)(0);
    w_u16(registers + 396, off_low);
    w_u16(registers + 398, off_high);
    w_u16(registers + 392, on_low);
    w_u16(registers + 394, on_high);
    w_u16(registers + 408, reverb_low);
    w_u16(registers + 410, reverb_high);
    return registers;
}

uint32 spu_sequence_vm_initialize(uint8 voices)
{
    SpuVoiceState *record;
    uint32 index, registers;
    spu_sequence_vm_transfer_mode(0);
    spu_vm_state()->release_offset = 0;
    SpuInitMalloc(32, (char *)spu_vm_state()->heap_records);
    for (index = 0; index < 192; ++index)
        spu_vm_state()->registers[index] = 0;
    for (index = 0; index < 24; ++index)
        spu_vm_state()->dirty[index] = (uint8)(0);
    spu_vab_reset_native();
    spu_vm_state()->events.voice_count = (uint8)(voices < 24 ? voices : 24);
    index = 0;
    if (spu_vm_state()->events.voice_count != 0)
    {
        do
        {
            record = &spu_vm_state()->voices[(index & 0xFFFF)];
            record->age = (uint16)(24);
            record->sequence = (uint16)(0xFFFF);
            record->sample = (uint16)(255);
            record->status = (uint8)(0);
            record->pitch = (uint16)(0);
            record->envelope = (uint16)(0);
            record->program_tone_index = (uint16)(0);
            record->program = (uint16)(0);
            record->tone = (uint16)(255);
            record->velocity = (uint16)(0);
            record->pan = (uint8)(64);
            record->volume_modulation[0] = 0;
            record->volume_modulation[1] = 0;
            record->volume_modulation[2] = 0;
            record->volume_modulation[3] = 0;
            record->pitch_modulation[0] = 0;
            record->pitch_modulation[1] = 0;
            record->pitch_modulation[2] = 0;
            record->pitch_modulation[3] = 0;
            record->pitch_modulation[4] = 0;
            record->volume_modulation[4] = 0;
            registers = SPU_REGISTER_BASE + (uint16)(index * 8) * 2;
            w_u16(registers + 6, 512);
            w_u16(registers + 4, 4096);
            w_u16(registers + 8, 0x80FF);
            w_u16(registers, 0);
            w_u16(registers + 2, 0);
            w_u16(registers + 10, 0x4000);
            spu_vm_state()->note.voice = (uint16)(index);
            spu_sequence_vm_key_off_now();
            ++index;
        } while ((index & 0xFFFF) < spu_vm_state()->events.voice_count);
    }
    spu_reverb_attributes.depth.left = 0x3FFF;
    spu_reverb_attributes.depth.right = 0x3FFF;
    spu_vm_state()->events.key_on[0] = (uint16)(0);
    spu_vm_state()->events.key_on[1] = (uint16)(0);
    spu_vm_state()->events.key_off[0] = (uint16)(0);
    spu_vm_state()->events.reverb[0] = (uint16)(0);
    spu_vm_state()->events.reverb[1] = (uint16)(0);
    spu_reverb_attributes.mask = 0;
    spu_reverb_attributes.mode = 0;
    spu_vm_state()->events.silence_cleanup_disabled = (uint8)(0);
    spu_vm_state()->mono_mode = 0;
    return spu_sequence_vm_flush();
}

uint32 spu_sequence_vm_key_on(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 bank, sint16 program, uint16 note, uint16 velocity, uint16 pan)
{
    uint32 packed = (uint16)sequence;
    SpuSequenceSchedulerTrack *record;
    uint8 workspace[256];
    uint8 *samples = workspace;
    uint8 *tones = workspace + 128;
    const uint8 *data;
    uint32 count, index, voice, result = 0;
    const uint8 *program_data;
    uint32 value;
    sint32 scaled;
    if (spu_vab_select_native((uint16)bank, program) != 0)
        return (uint32)-1;
    spu_vm_state()->note.sequence = (uint16)(packed);
    spu_vm_state()->note.note = (uint8)(note);
    spu_vm_state()->note.note_fraction = (uint8)(0);
    if (sequence == 33)
        spu_vm_state()->note.velocity = (uint8)(velocity);
    else
    {
        record = scheduler->tracks[(packed & 255)] + (packed >> 8);
        value = record->channel;
        scaled = (sint32)velocity * (sint16)record->channel_volume[value];
        spu_vm_state()->note.velocity = (uint8)(scaled / 127);
    }
    spu_vm_state()->note.pan = (uint8)(pan);
    program_data = spu_vab_native_current().programs + (uint32)(sint32)program * 16;
    spu_vm_state()->note.program_volume = (uint8)(*(program_data + 1));
    spu_vm_state()->note.program_pan = (uint8)(*(program_data + 4));
    value = *(program_data);
    data = spu_vab_native_current().header;
    spu_vm_state()->note.tone_count = (uint8)(value);
    value = spu_vm_state()->note.program_tone_index;
    if (value >= xport_load_le16(data + 18))
        return (uint32)-1;
    if (velocity == 0)
        return spu_sequence_vm_key_off(sequence, bank, program, note);
    count = spu_sequence_vm_select_tones(tones, samples) & 255;
    index = 0;
    if (count != 0)
    {
        do
        {
            spu_vm_state()->note.sample = (uint16)(samples[index & 255]);
            spu_vm_state()->note.tone = (uint8)(tones[index & 255]);
            value = 16 * spu_vm_state()->note.program_tone_index + spu_vm_state()->note.tone;
            data = spu_vab_native_current().tones + value * 32;
            spu_vm_state()->note.priority = (uint8)(*(data));
            spu_vm_state()->note.tone_volume = (uint8)(*(data + 2));
            spu_vm_state()->note.tone_pan = (uint8)(*(data + 3));
            spu_vm_state()->note.center_note = (uint8)(*(data + 4));
            spu_vm_state()->note.fine_tune = (uint8)(*(data + 5));
            spu_vm_state()->note.tone_mode = (uint8)(*(data + 1));
            spu_vm_state()->note.min_key = (uint8)(*(data + 6));
            spu_vm_state()->note.max_key = (uint8)(*(data + 7));
            voice = spu_sequence_vm_allocate() & 255;
            value = spu_vm_state()->events.voice_count;
            spu_vm_state()->note.voice = (uint16)(voice);
            if (voice >= value)
                result = (uint32)-1;
            else
            {
                spu_vm_state()->voices[voice].status = (uint8)(1);
                voice = (uint32)(sint32)(sint16)spu_vm_state()->note.voice;
                spu_vm_state()->voices[voice].age = (uint16)(0);
                voice = (uint32)(sint32)(sint16)spu_vm_state()->note.voice;
                spu_vm_state()->voices[voice].sequence = (uint16)(packed);
                voice = (uint32)(sint32)(sint16)spu_vm_state()->note.voice;
                spu_vm_state()->voices[voice].bank = (uint16)(spu_vm_state()->note.bank);
                voice = (uint32)(sint32)(sint16)spu_vm_state()->note.voice;
                spu_vm_state()->voices[voice].program_tone_index = (uint16)(spu_vm_state()->note.program_tone_index);
                voice = (uint32)(sint32)(sint16)spu_vm_state()->note.voice;
                spu_vm_state()->voices[voice].program = (uint16)((uint16)program);
                if (sequence != 33)
                {
                    voice = (uint32)(sint32)(sint16)spu_vm_state()->note.voice;
                    spu_vm_state()->voices[voice].velocity = (uint16)(velocity);
                }
                voice = (uint32)(sint32)(sint16)spu_vm_state()->note.voice;
                spu_vm_state()->voices[voice].pan = (uint8)(pan);
                voice = (uint32)(sint32)(sint16)spu_vm_state()->note.voice;
                spu_vm_state()->voices[voice].tone = (uint16)(spu_vm_state()->note.tone);
                voice = (uint32)(sint32)(sint16)spu_vm_state()->note.voice;
                spu_vm_state()->voices[voice].note = (uint16)(note);
                voice = (uint32)(sint32)(sint16)spu_vm_state()->note.voice;
                spu_vm_state()->voices[voice].priority = (uint16)(spu_vm_state()->note.priority);
                voice = (uint32)(sint32)(sint16)spu_vm_state()->note.voice;
                spu_vm_state()->voices[voice].sample = (uint16)(spu_vm_state()->note.sample);
                spu_sequence_vm_prepare_voice();
                if ((sint16)spu_vm_state()->note.sample == 255)
                    spu_sequence_vm_noise_on(scheduler, (uint8)spu_vm_state()->note.voice);
                else
                {
                    value = spu_sequence_vm_note_pitch();
                    spu_sequence_vm_key_on_now(scheduler, count, (sint16)value);
                }
                voice = (uint32)(sint32)(sint16)spu_vm_state()->note.voice;
                result |= 1u << (voice & 31);
            }
            ++index;
        } while ((index & 255) < count);
    }
    return result;
}

static uint32 spu_sequence_vm_noise_key_events(uint8 voice, uint32 low, uint32 high, sint32 reset_age)
{
    uint32 value, index, upper, pending;
    value = spu_vm_state()->events.voice_count;
    spu_vm_state()->voices[(uint32)voice].pitch = (uint16)(10);
    if (value != 0)
    {
        index = 0;
        do
        {
            spu_vm_state()->voices[(sint16)index].status = (uint8)(spu_vm_state()->voices[(sint16)index].status & 1);
            ++index;
        } while ((sint16)index < spu_vm_state()->events.voice_count);
    }
    spu_vm_state()->voices[(uint32)voice].status = (uint8)(2);
    value = spu_vm_state()->events.key_on[0];
    upper = spu_vm_state()->events.key_on[1];
    if (reset_age)
        spu_vm_state()->voices[(uint32)voice].age = 0;
    pending = spu_vm_state()->events.key_off[0];
    value |= low;
    upper |= high;
    spu_vm_state()->events.key_on[0] = (uint16)(value);
    pending &= ~value;
    spu_vm_state()->events.key_on[1] = (uint16)(upper);
    spu_vm_state()->events.key_off[0] = (uint16)(pending);
    pending = spu_vm_state()->events.key_off[1];
    return pending & ~upper;
}

void spu_sequence_vm_noise_key_on_direct(uint8 voice, sint16 left, sint16 right)
{
    uint32 low, high, pending;
    spu_vm_voice(voice);
    SsUtSetDetVVol(voice, left, right);
    low = voice < 16 ? 1u << (voice & 31) : 0;
    high = voice < 16 ? 0 : 1u << ((voice - 16) & 31);
    pending = spu_sequence_vm_noise_key_events(voice, low, high, 1);
    spu_vm_state()->events.key_off[1] = (uint16)pending;
    w_u16(SPU_REGISTER_BASE + 404, low);
    w_u16(SPU_REGISTER_BASE + 406, high);
}

void spu_sequence_vm_noise_key_on_allocated(sint16 left, sint16 right)
{
    uint32 voice;
    spu_vm_state()->note.priority = 127;
    voice = spu_sequence_vm_allocate() & 255;
    spu_vm_state()->note.voice = (uint16)voice;
    if (voice < spu_vm_state()->events.voice_count)
        spu_sequence_vm_noise_key_on_direct((uint8)spu_vm_state()->note.voice, left, right);
}

#if defined(XPORT_NATIVE_VM) && XPORT_NATIVE_VM
static sint16 spu_sequence_vm_key_on_tone(uint16 voice, sint16 bank, sint16 program, sint16 tone, sint16 note, sint16 fine, sint16 left, sint16 right, sint32 allocate)
{
    SpuVabNativeView view;
    SpuVoiceState *record;
    const uint8 *data;
    uint32 tone_index, selected;
    uint16 pitch;
    sint16 result = -1;
    xport_audio_lock();
    if (spu_vm_state()->immediate_key_on_busy == 1)
    {
        xport_audio_unlock();
        return -1;
    }
    spu_vm_state()->immediate_key_on_busy = 1;
    if ((!allocate && voice >= SPU_VOICE_COUNT) || spu_vab_select_native((uint16)bank, program) != 0)
        goto finished;
    spu_vm_state()->note.sequence = 33;
    spu_vm_state()->note.note = (uint8)note;
    spu_vm_state()->note.note_fraction = (uint8)fine;
    spu_vm_state()->note.tone = (uint8)tone;
    if (left == right)
    {
        spu_vm_state()->note.pan = 64;
        spu_vm_state()->note.velocity = (uint8)left;
    }
    else if (right < left)
    {
        if (left == 0)
        {
            trap(7);
            abort();
        }
        spu_vm_state()->note.velocity = (uint8)left;
        spu_vm_state()->note.pan = (uint8)(((sint32)right * 64) / left);
    }
    else
    {
        if (right == 0)
        {
            trap(7);
            abort();
        }
        spu_vm_state()->note.velocity = (uint8)right;
        spu_vm_state()->note.pan = (uint8)(127 - ((sint32)left * 64) / right);
    }
    view = spu_vab_native_current();
    data = view.programs + (uint32)(sint32)program * 16;
    spu_vm_state()->note.program_volume = data[1];
    spu_vm_state()->note.program_pan = data[4];
    spu_vm_state()->note.tone_count = data[0];
    tone_index = 16 * spu_vm_state()->note.program_tone_index + spu_vm_state()->note.tone;
    if (tone_index >= view.tone_bytes / 32)
    {
    #ifndef NDEBUG
        fprintf(stderr, "PsyQ native VAB: Immediate tone outside bank\n");
    #endif
        abort();
    }
    data = view.tones + tone_index * 32;
    spu_vm_state()->note.priority = data[0];
    spu_vm_state()->note.sample = xport_load_le16(data + 22);
    spu_vm_state()->note.tone_volume = data[2];
    spu_vm_state()->note.tone_pan = data[3];
    spu_vm_state()->note.center_note = data[4];
    spu_vm_state()->note.fine_tune = data[5];
    spu_vm_state()->note.tone_mode = data[1];
    spu_vm_state()->note.min_key = data[6];
    spu_vm_state()->note.max_key = data[7];
    if ((sint16)spu_vm_state()->note.sample == 0)
        goto finished;
    selected = allocate ? spu_sequence_vm_allocate() & 255 : voice;
    if (allocate && selected == spu_vm_state()->events.voice_count)
        goto finished;
    spu_vm_state()->note.voice = (uint16)selected;
    record = spu_vm_voice(selected);
    record->sequence = 33;
    record->bank = (uint16)bank;
    record->program = (uint16)program;
    record->program_tone_index = spu_vm_state()->note.program_tone_index;
    record->sample = spu_vm_state()->note.sample;
    record->note = (uint16)note;
    record->status = 1;
    record->age = 0;
    record->tone = spu_vm_state()->note.tone;
    spu_sequence_vm_prepare_voice();
    if ((sint16)spu_vm_state()->note.sample == 255)
        spu_sequence_vm_noise_on(&spu_sequence_scheduler, (uint8)selected);
    else
    {
        pitch = spu_sequence_vm_tone_pitch((uint16)note, (uint16)fine);
        spu_sequence_vm_key_on_now(&spu_sequence_scheduler, 1, (sint16)pitch);
    }
    result = (sint16)selected;
finished:
    spu_vm_state()->immediate_key_on_busy = 0;
    xport_audio_unlock();
    return result;
}
#endif

uint32 spu_sequence_vm_noise_on(SpuSequenceScheduler *scheduler, uint8 voice)
{
    uint32 packed = spu_vm_state()->note.sequence;
    SpuSequenceSchedulerTrack *record;
    if (voice >= SPU_VOICE_COUNT || (packed & 255) >= 32 || scheduler->tracks[packed & 255] == NULL || (packed >> 8) >= scheduler->separation_count)
    {
#ifndef NDEBUG
        fprintf(stderr, "PsyQ native SEQ: Invalid noise voice or sequence track\n");
#endif
        abort();
    }
    record = scheduler->tracks[packed & 255] + (packed >> 8);
    uint32 track_left = record->left_volume;
    uint32 program_volume = spu_vm_state()->note.program_volume;
    uint32 left = track_left * 129 * program_volume;
    uint32 right = record->right_volume * 129 * program_volume;
    uint32 tone_volume = spu_vm_state()->note.tone_volume;
    uint32 control, pan, registers, low, high, value, upper, pending;
    left = (left / 127) * tone_volume;
    right = (right / 127) * tone_volume;
    control = r_u16(SPU_REGISTER_BASE + 426);
    left /= 127;
    right /= 127;
    pan = spu_vm_state()->note.tone_pan;
    if (pan < 64)
        right = (right * pan) / 63;
    else
        left = (left * (127 - pan)) / 63;
    pan = spu_vm_state()->note.program_pan;
    if (pan < 64)
        right = (right * pan) / 63;
    else
        left = (left * (127 - pan)) / 63;
    pan = spu_vm_state()->note.pan;
    if (pan < 64)
        right = (right * pan) / 63;
    else
        left = (left * (127 - pan)) / 63;
    if (spu_vm_state()->mono_mode == 1)
    {
        if (left < right)
            left = right;
        else
            right = left;
    }
    value = spu_vm_state()->note.note - spu_vm_state()->note.center_note;
    control = (control & 0xC0FF) | ((value & 63) << 8);
    registers = SPU_REGISTER_BASE;
    w_u16(registers + 426, control);
    spu_vm_state()->registers[(uint32)voice * 8 + 1] = (uint16)(right);
    value = spu_vm_state()->dirty[voice];
    spu_vm_state()->registers[(uint32)voice * 8] = (uint16)(left);
    spu_vm_state()->dirty[voice] = (uint8)(value | 3);
    low = voice < 16 ? 1u << (voice & 31) : 0;
    high = voice < 16 ? 0 : 1u << ((voice - 16) & 31);
    pending = spu_sequence_vm_noise_key_events(voice, low, high, 0);
    value = spu_vm_state()->note.tone_mode & 4;
    spu_vm_state()->events.key_off[1] = (uint16)pending;
    if (value != 0)
    {
        value = spu_vm_state()->events.reverb[0];
        upper = spu_vm_state()->events.reverb[1];
        spu_vm_state()->events.reverb[0] = (uint16)(value | low);
        spu_vm_state()->events.reverb[1] = (uint16)(upper | high);
    }
    else
    {
        spu_vm_state()->events.reverb[0] = (uint16)(spu_vm_state()->events.reverb[0] & ~low);
        spu_vm_state()->events.reverb[1] = (uint16)(spu_vm_state()->events.reverb[1] & ~high);
    }
    registers = SPU_REGISTER_BASE;
    w_u16(registers + 404, low);
    w_u16(registers + 406, high);
    return registers;
}

uint32 spu_sequence_vm_key_on_now(SpuSequenceScheduler *scheduler, uint32 layers, sint16 pitch)
{
    uint32 header_volume = *(spu_vab_native_current().header + 24);
    uint32 gain = spu_vm_state()->note.velocity * (header_volume * 16383);
    SpuSequenceSchedulerTrack *record;
    uint32 packed, left, right, pan, index, voice, low, high, value, upper;
    sint32 signed_voice;
    gain /= 16129;
    gain *= spu_vm_state()->note.program_volume;
    gain *= spu_vm_state()->note.tone_volume;
    signed_voice = (sint16)spu_vm_state()->note.voice;
    index = (uint16)((uint32)signed_voice * 8);
    gain /= 16129;
    packed = spu_vm_state()->note.sequence;
    left = gain;
    right = gain;
    if ((sint16)packed != 33)
    {
        record = scheduler->tracks[(packed & 255)] + (packed >> 8);
        left = (gain * record->left_volume) / 127;
        right = (gain * record->right_volume) / 127;
    }
    pan = spu_vm_state()->note.tone_pan;
    if (pan < 64)
        right = (right * pan) / 63;
    else
        left = (left * (127 - pan)) / 63;
    pan = spu_vm_state()->note.program_pan;
    if (pan < 64)
        right = (right * pan) / 63;
    else
        left = (left * (127 - pan)) / 63;
    pan = spu_vm_state()->note.pan;
    if (pan < 64)
        right = (right * pan) / 63;
    else
        left = (left * (127 - pan)) / 63;
    if (spu_vm_state()->mono_mode == 1)
    {
        if (left < right)
            left = right;
        else
            right = left;
    }
    left = (left * left) / 16383;
    right = (right * right) / 16383;
    spu_vm_state()->registers[index + 2] = (uint16)((uint16)pitch);
    spu_vm_state()->registers[index] = (uint16)(left);
    spu_vm_state()->registers[index + 1] = (uint16)(right);
    voice = (uint32)(sint32)(sint16)spu_vm_state()->note.voice;
    spu_vm_state()->dirty[voice] = (uint8)(spu_vm_state()->dirty[voice] | 7);
    voice = (uint32)(sint32)(sint16)spu_vm_state()->note.voice;
    spu_vm_state()->voices[voice].pitch = (uint16)((uint16)pitch);
    voice = (uint32)(sint32)(sint16)spu_vm_state()->note.voice;
    spu_vm_state()->voices[voice].status = (uint8)(1);
    signed_voice = (sint16)spu_vm_state()->note.voice;
    low = signed_voice < 16 ? 1u << ((uint32)signed_voice & 31) : 0;
    high = signed_voice < 16 ? 0 : 1u << (((uint32)signed_voice - 16) & 31);
    if ((spu_vm_state()->note.tone_mode & 4) != 0)
    {
        value = spu_vm_state()->events.reverb[0];
        upper = spu_vm_state()->events.reverb[1];
        spu_vm_state()->events.reverb[0] = (uint16)(value | low);
        spu_vm_state()->events.reverb[1] = (uint16)(upper | high);
    }
    else
    {
        spu_vm_state()->events.reverb[0] = (uint16)(spu_vm_state()->events.reverb[0] & ~low);
        spu_vm_state()->events.reverb[1] = (uint16)(spu_vm_state()->events.reverb[1] & ~high);
    }
    value = spu_vm_state()->events.key_on[0];
    upper = spu_vm_state()->events.key_on[1];
    left = spu_vm_state()->events.key_off[0];
    value |= low;
    spu_vm_state()->events.key_on[0] = (uint16)(value);
    spu_vm_state()->events.key_off[0] = (uint16)(left & ~value);
    right = spu_vm_state()->events.key_off[1];
    upper |= high;
    spu_vm_state()->events.key_on[1] = (uint16)(upper);
    value = right & ~upper;
    spu_vm_state()->events.key_off[1] = (uint16)(value);
    return (uint16)value;
}

PsxInterruptCallback SpuSetTransferCallback(PsxInterruptCallback callback)
{
    PsxInterruptCallback previous;
    xport_audio_lock();
    previous = spu_native_transfer_state->completion_callback;
    if (previous != callback)
        spu_native_transfer_state->completion_callback = callback;
    xport_audio_unlock();
    return previous;
}

uint32 spu_sequence_vm_transfer_mode(uint32 mode)
{
    spu_native_transfer_state->completion = mode != 1;
    return 1;
}

uint32 spu_sequence_vm_reverb_depth(sint16 left, sint16 right)
{
    spu_reverb_attributes.mask = 6;
    spu_reverb_attributes.depth.left = (sint16)(((sint32)left * 32767) / 127);
    spu_reverb_attributes.depth.right = (sint16)(((sint32)right * 32767) / 127);
    return (uint32)SpuSetReverbModeParam(&spu_reverb_attributes);
}

uint32 spu_sequence_vm_reverb_type(uint32 mode)
{
    uint32 magnitude = (mode & 0x8000) != 0 ? 0u - mode : mode;
    sint16 type;
    if ((uint16)magnitude >= 10)
        return (uint32)-1;
    spu_reverb_attributes.mask = 1;
    spu_reverb_attributes.mode = (sint16)(magnitude | ((mode & 0x8000) != 0 ? 0x100 : 0));
    type = (sint16)magnitude;
    if (type == 0)
        SpuSetReverb(0);
    SpuSetReverbModeParam(&spu_reverb_attributes);
    return (uint32)(sint32)type;
}

uint32 spu_sequence_vm_reverb_feedback(uint8 value)
{
    spu_reverb_attributes.mask = 16;
    spu_reverb_attributes.feedback = value;
    return (uint32)SpuSetReverbModeParam(&spu_reverb_attributes);
}

uint32 spu_sequence_vm_reverb_delay(uint8 value)
{
    spu_reverb_attributes.mask = 8;
    spu_reverb_attributes.delay = value;
    return (uint32)SpuSetReverbModeParam(&spu_reverb_attributes);
}

static void spu_reset_device(uint32 mode, sint32 native)
{
    uint32 control = SPU_DMA_PRIORITY_REGISTER;
    uint32 port, status, count, index;
    /* Native transfers have no hardware DMA-priority scheduler */
    if (!native)
        w_u32(control, r_u32(control) | 0xB0000);
    port = SPU_REGISTER_BASE;
    spu_native_transfer_state->normalized_mode = 0;
    spu_native_transfer_state->address_units = 0;
    spu_reset_write(port + 384, 0, native);
    spu_reset_write(port + 386, 0, native);
    spu_reset_write(port + 426, 0, native);
    port = SPU_REGISTER_BASE;
    spu_reset_write(port + 384, 0, native);
    spu_reset_write(port + 386, 0, native);
    status = spu_reset_read(port + 430, native) & 0x7FF;
    count = 1;
    while (status != 0 && count < 0xF01)
    {
        status = spu_reset_read(SPU_REGISTER_BASE + 430, native) & 0x7FF;
        ++count;
    }
    if (status != 0)
#ifndef NDEBUG
        printf("SPU:T/O [%s]\n", "wait (reset)");
#else
        (void)0;
#endif
    port = SPU_REGISTER_BASE;
    spu_reset_write(port + 428, 4, native);
    spu_reset_write(port + 388, 0, native);
    spu_reset_write(port + 390, 0, native);
    spu_reset_write(port + 396, 0xFFFF, native);
    spu_reset_write(port + 398, 0xFFFF, native);
    spu_reset_write(port + 408, 0, native);
    spu_reset_write(port + 410, 0, native);
    spu_key_control->key_on[0] = 0;
    spu_key_control->key_on[1] = 0;
    spu_key_control->key_off[0] = 0;
    spu_key_control->key_off[1] = 0;
    spu_key_control->pitch_mask[0] = 0;
    spu_key_control->pitch_mask[1] = 0;
    spu_key_control->noise_mask[0] = 0;
    spu_key_control->noise_mask[1] = 0;
    spu_key_control->reverb_mask[0] = 0;
    spu_key_control->reverb_mask[1] = 0;
    if (mode == 0)
    {
        port = SPU_REGISTER_BASE;
        spu_native_transfer_state->address_units = 512;
        spu_reset_write(port + 400, 0, native);
        spu_reset_write(port + 402, 0, native);
        spu_reset_write(port + 404, 0, native);
        spu_reset_write(port + 406, 0, native);
        spu_reset_write(port + 432, 0, native);
        spu_reset_write(port + 434, 0, native);
        spu_reset_write(port + 436, 0, native);
        spu_reset_write(port + 438, 0, native);
        spu_reset_write_by_io(spu_silent_sample, sizeof(spu_silent_sample), native);
        port = SPU_REGISTER_BASE;
        for (index = 0; index < 24; ++index)
        {
            spu_reset_write(port, 0, native);
            spu_reset_write(port + 2, 0, native);
            spu_reset_write(port + 4, 0x3FFF, native);
            spu_reset_write(port + 6, 512, native);
            spu_reset_write(port + 8, 0, native);
            spu_reset_write(port + 10, 0, native);
            port += 16;
        }
        port = SPU_REGISTER_BASE;
        spu_reset_write(port + 392, 0xFFFF, native);
        spu_reset_write(port + 394, 255, native);
        port = SPU_REGISTER_BASE;
        spu_reset_write(port + 396, 0xFFFF, native);
        spu_reset_write(port + 398, 255, native);
    }
    port = SPU_REGISTER_BASE;
    spu_native_transfer_state->completion = 1;
    spu_reset_write(port + 426, 0xC000, native);
    spu_native_transfer_state->completion_callback = 0;
}

void spu_sequence_vm_reset_device(uint32 mode)
{
    spu_reset_device(mode, 0);
}

/* Original PsyQ reset over the native SPU device, without rebinding the bus */
sint32 _spu_init(uint32 mode)
{
    xport_audio_lock();
    /* TODO: Investigate reset with in-flight transfers or queued PIO data */
    if (!spu.initialized || spu.mmio_fifo_count || transfer_failed || transfer_pending || transfer_operation.active || (transfer.requested_mode && native_transfer_binding != SPU_TRANSFER_NATIVE))
    {
#ifndef NDEBUG
        fprintf(stderr, "Native SPU reset rejected: mode=%u initialized=%u fifo=%u failed=%d pending=%d active=%d owner=%d\n", mode, spu.initialized, spu.mmio_fifo_count, transfer_failed, transfer_pending, transfer_operation.active, native_transfer_binding);
#endif
        xport_audio_unlock();
        abort();
    }
    spu_reset_device(mode, 1);
    xport_audio_unlock();
    return 0;
}

void spu_sequence_vm_dma(void)
{
    uint32 port = SPU_REGISTER_BASE;
    uint32 count = 0;
    PsxInterruptCallback callback;
    w_u16(port + 426, r_u16(port + 426) & 0xFFCF);
    if ((r_u16(port + 426) & 0x30) != 0)
    {
        count = 1;
        while (count < 0xF01)
        {
            if ((r_u16(port + 426) & 0x30) == 0)
                break;
            ++count;
        }
    }
    callback = spu_native_transfer_state->completion_callback;
    if (callback != NULL)
        callback();
    else
        DeliverEvent(0xF0000009, 0x20);
}

PsxInterruptCallback spu_sequence_vm_register_dma(PsxInterruptCallback callback)
{
    return setIntrDMA(4, callback);
}

void spu_sequence_vm_quit(void)
{
    if (spu_dma_event_state.initialized != 1)
        return;
    spu_dma_event_state.initialized = 0;
    EnterCriticalSectionPSX();
    spu_native_transfer_state->completion_callback = 0;
    spu_sequence_vm_register_dma(NULL);
    CloseEvent((sint32)spu_dma_event_state.event_handle);
    DisableEvent((sint32)spu_dma_event_state.event_handle);
    ExitCriticalSection();
}

static uint32 spu_start_dma(PsxInterruptCallback dma_callback)
{
    uint32 result;
    if (spu_dma_event_state.initialized)
        return spu_dma_event_state.initialized;
    spu_dma_event_state.initialized = 1;
    EnterCriticalSectionPSX();
    spu_sequence_vm_register_dma(dma_callback);
    spu_dma_event_state.event_handle = (uint32)OpenEvent(0xF0000009, 0x20, 0x2000, NULL);
    result = EnableEvent((sint32)spu_dma_event_state.event_handle);
    ExitCriticalSection();
    return result;
}

uint32 SpuStart(void)
{
    return spu_start_dma(spu_sequence_vm_dma);
}

uint32 spu_sequence_vm_initialize_device(uint32 mode, PsxInterruptCallback dma_callback)
{
    static uint32 device_initialized;
    SpuReverbControl reverb;
    uint32 index, table, result;
    if (device_initialized == 0)
    {
        SpuInit();
        xport_memory_bind_mmio(0x1F801C00, 512, spu_mmio_read, spu_mmio_write, NULL);
        device_initialized = 1;
    }
    psx_callback_reset_driver();
    spu_sequence_vm_reset_device(mode);
    if (mode == 0)
        for (index = 0; index < 24; ++index)
            spu_sample_notes[23 - index] = SPU_INITIAL_SAMPLE_NOTE;
    spu_start_dma(dma_callback);
    table = spu_reverb_default_work[0];
    spu_reverb_export(&reverb);
    reverb.enabled = 0;
    reverb.control &= ~0x80u;
    reverb.reserved = 0;
    reverb.mode = 0;
    reverb.depth_left = 0;
    reverb.depth_right = 0;
    reverb.delay = 0;
    reverb.feedback = 0;
    reverb.work_address = table;
    spu_reverb_import(&reverb, NULL, NULL);
    result = SPU_REGISTER_BASE + 418;
    w_u16(result, table);
    spu_bind_heap(NULL);
    spu_native_transfer_state->requested_mode = 0;
    spu_native_transfer_state->normalized_mode = 0;
    spu_key_control->key_state = 0;
    spu_key_control->event_flags = 0;
    spu_key_control->pending_state = 0;
    spu_key_control->event_mode = 0;
    return result;
}

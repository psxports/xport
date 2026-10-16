#include "ap.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef AP_WEB
    #include <emscripten.h>
#endif
#include "psx_spu.h"
#include "psx_xa.h"
#include "xport_trace.h"
#if defined(LOCKSTEP_DEBUG)
#endif

static SpuReverbControl reverb_control;
static SpuReverbWorkCheck reverb_work_check;
static void *reverb_work_context;
static sint32 reverb_incomplete;
// Original PsyQ LIBSPU reverb tables
static const uint32 spu_reverb_default_work[10] = {0x0000FFFEu, 0x0000FB28u, 0x0000FC18u, 0x0000F6F8u, 0x0000F204u, 0x0000EA44u, 0x0000E128u, 0x0000CFF8u, 0x0000CFF8u, 0x0000F880u};

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

typedef struct SPU_TRANSFER_STATE
{
    uint32 req_mode, mode, complete, callback, src_addr, blocks;
    uint16 addr_units;
} SPU_TRANSFER_STATE;

static SpuTransferBinding transfer;
static SPU_TRANSFER_OWNER native_transfer_binding;
static sint32 transfer_failed;
static uint32 transfer_byte_address;
static uint64 transfer_deadline;
static sint32 transfer_pending;
static XportDeviceOperation transfer_operation;
static void spu_irq_check_range(uint32 address, uint32 bytes);
static size_t cd_checkpoint_size(void);
static void cd_checkpoint_capture(void *data);
static void cd_checkpoint_restore(const void *data);
static int cd_checkpoint_io(FILE *file, int load);
static void cd_notify_command(uint8 command);
#if defined(LOCKSTEP_DEBUG)
static int cd_checkpoint_selftest(void);
static int cd_callback_selftest(void);
#endif

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
    if (!binding->requested_mode || !binding->normalized_mode || !binding->address_units || !binding->completion || !binding->completion_callback || !binding->source_address || !binding->dma_blocks || binding->address_shift > 31u || (binding->address_rounding_enabled && !binding->address_rounding_unit) || !binding->block_bytes || !binding->ticks_per_block || !binding->clock || !binding->wait_until || !binding->complete_event || !binding->guest_address)
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

static uint32 spu_write_source(const void *source, uint32 byte_count, sint32 allow_host)
{
    uint32 blocks, rounded, guest;
    sint32 host_source;
    if (byte_count > 0x7eff0u)
        byte_count = 0x7eff0u;
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
    if (!*transfer.completion_callback)
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
    uint16 sample_note;
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
    uint32 pitch_modulation;
    uint16 irq_address;
    uint8 irq_flag;
    uint8 irq_pending;
    uint64 lockstep_ticks;
    uint32 lockstep_clock_valid;
    uint32 lockstep_vblank_render;
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
    SPU_TRANSFER_STATE transfer;
    uint32 heap_last_idx;
    uint8 initialized;
    uint8 sample_clock_from_wait;
} SPU_STATE;

static SPU_STATE spu;

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
    spu.transfer.addr_units = (uint16)(words[2] >> 3);
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
    spu.lockstep_ticks = sample_counter;
    spu.lockstep_clock_valid = 1;
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

#if defined(LOCKSTEP_DEBUG)
extern sint32 xport_lockstep_device_ticks(uint64 *ticks);
#endif

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

typedef struct VAB_PROGRAM
{
    uint8 tone_count;
    uint8 volume;
    uint8 priority;
    uint8 mode;
    uint8 pan;
} VAB_PROGRAM;

typedef struct VAB_TONE
{
    uint8 priority;
    uint8 mode;
    uint8 volume;
    uint8 pan;
    uint8 center;
    uint8 shift;
    uint8 note_min;
    uint8 note_max;
    uint8 pitch_bend_min;
    uint8 pitch_bend_max;
    uint16 adsr1;
    uint16 adsr2;
    sint16 program;
    sint16 sample;
} VAB_TONE;

typedef struct VAB_BANK
{
    VAB_PROGRAM programs[VAB_PROGRAM_COUNT];
    VAB_TONE tones[VAB_PROGRAM_COUNT][VAB_TONES_PER_PROGRAM];
    uint32 sample_addresses[256];
    uint32 body_address;
    uint32 body_size;
    uint8 master_volume;
    uint8 master_pan;
    uint8 program_count;
    uint8 valid;
    uint8 transferred;
} VAB_BANK;

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
    CdlATV mix;
    sint16 volume_left;
    sint16 volume_right;
    sint32 track;
    uint8 active;
    uint8 paused;
    uint8 muted;
    uint8 repeat;
} CD_AUDIO_STATE;

typedef struct LIBSND_STATE
{
    VAB_BANK banks[VAB_BANK_COUNT];
    LIBSND_VOICE voices[SPU_VOICE_COUNT];
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

    struct
    {
        uint8 ready, status, response, has_response;
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

static void cd_queue_callback(uint32 ready, uint8 status, const uint8 *response)
{
    uint32 index;
    xport_audio_lock();
    if (libsnd.pending_callback_count == 256)
        abort();
    index = (libsnd.pending_callback_read + libsnd.pending_callback_count) & 255;
    libsnd.pending_callbacks[index].ready = (uint8)ready;
    libsnd.pending_callbacks[index].status = status;
    libsnd.pending_callbacks[index].response = response ? response[0] : 0;
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

#if defined(LOCKSTEP_DEBUG)
typedef struct SPU_LOCKSTEP_CHECKPOINT
{
    SPU_STATE spu;
    LIBSND_STATE libsnd;
    SpuReverbControl reverb_control;
    SpuReverbWorkCheck reverb_work_check;
    void *reverb_work_context;
    sint32 reverb_incomplete;
    SpuHeapBinding heap;
    sint32 heap_failed;
    SpuTransferBinding transfer;
    SPU_TRANSFER_OWNER native_transfer_binding;
    sint32 transfer_failed;
    uint32 transfer_byte_address;
    uint64 transfer_deadline;
    sint32 transfer_pending;
    XportDeviceOperation transfer_operation;
    CdlLOC cd_toc[100];
    sint32 cd_toc_track_count;
} SPU_LOCKSTEP_CHECKPOINT;

size_t spu_lockstep_checkpoint_size(void)
{
    return sizeof(SPU_LOCKSTEP_CHECKPOINT) + cd_checkpoint_size();
}

int spu_lockstep_checkpoint_capture(void *data, size_t size)
{
    SPU_LOCKSTEP_CHECKPOINT *saved = (SPU_LOCKSTEP_CHECKPOINT *)data;

    if (!saved || size != spu_lockstep_checkpoint_size())
        return 0;
    xport_audio_lock();
    saved->spu = spu;
    saved->libsnd = libsnd;
    saved->reverb_control = reverb_control;
    saved->reverb_work_check = reverb_work_check;
    saved->reverb_work_context = reverb_work_context;
    saved->reverb_incomplete = reverb_incomplete;
    saved->heap = heap;
    saved->heap_failed = heap_failed;
    saved->transfer = transfer;
    saved->native_transfer_binding = native_transfer_binding;
    saved->transfer_failed = transfer_failed;
    saved->transfer_byte_address = transfer_byte_address;
    saved->transfer_deadline = transfer_deadline;
    saved->transfer_pending = transfer_pending;
    saved->transfer_operation = transfer_operation;
    memcpy(saved->cd_toc, cd_toc, sizeof(cd_toc));
    saved->cd_toc_track_count = cd_toc_track_count;
    cd_checkpoint_capture(saved + 1);
    xport_audio_unlock();
    return 1;
}

int spu_lockstep_checkpoint_restore(const void *data, size_t size)
{
    const SPU_LOCKSTEP_CHECKPOINT *saved = (const SPU_LOCKSTEP_CHECKPOINT *)data;

    if (!saved || size != spu_lockstep_checkpoint_size())
        return 0;
    xport_audio_lock();
    spu = saved->spu;
    libsnd = saved->libsnd;
    if (libsnd.pending_callback_count || libsnd.pending_sdk_count || libsnd.cd_end_callback)
        psx_wait_signal_service(6);
    reverb_control = saved->reverb_control;
    reverb_work_check = saved->reverb_work_check;
    reverb_work_context = saved->reverb_work_context;
    reverb_incomplete = saved->reverb_incomplete;
    heap = saved->heap;
    heap_failed = saved->heap_failed;
    transfer = saved->transfer;
    native_transfer_binding = saved->native_transfer_binding;
    transfer_failed = saved->transfer_failed;
    transfer_byte_address = saved->transfer_byte_address;
    transfer_deadline = saved->transfer_deadline;
    transfer_pending = saved->transfer_pending;
    transfer_operation = saved->transfer_operation;
    memcpy(cd_toc, saved->cd_toc, sizeof(cd_toc));
    cd_toc_track_count = saved->cd_toc_track_count;
    cd_checkpoint_restore(saved + 1);
    xport_audio_unlock();
    return 1;
}

int spu_lockstep_checkpoint_synchronize(void)
{
    sint16 discarded[2];
    uint64 target;
    sint32 synchronized;
    uint8 clock_from_wait = spu.sample_clock_from_wait;

    if (!xport_lockstep_device_ticks(&target))
        return !xport_lockstep_spu_timeline_active();
    spu.sample_clock_from_wait = 0;
    spu_render_vblank(discarded, 1u);
    spu.sample_clock_from_wait = clock_from_wait;
    xport_audio_lock();
    synchronized = spu.lockstep_clock_valid && spu.lockstep_ticks == target;
    xport_audio_unlock();
    return synchronized;
}
#endif

static const uint16 libsnd_pitch_table[192] = {
    0x1000, 0x100e, 0x101d, 0x102c, 0x103b, 0x104a, 0x1059, 0x1068, 0x1078, 0x1087, 0x1096, 0x10a5, 0x10b5, 0x10c4, 0x10d4, 0x10e3, 0x10f3, 0x1103, 0x1113, 0x1122, 0x1132, 0x1142, 0x1152, 0x1162, 0x1172, 0x1182, 0x1193, 0x11a3, 0x11b3, 0x11c4, 0x11d4, 0x11e5, 0x11f5, 0x1206, 0x1216, 0x1227, 0x1238, 0x1249, 0x125a, 0x126b, 0x127c, 0x128d, 0x129e, 0x12af, 0x12c1, 0x12d2, 0x12e3, 0x12f5, 0x1306, 0x1318, 0x132a, 0x133c, 0x134d, 0x135f, 0x1371, 0x1383, 0x1395, 0x13a7, 0x13ba, 0x13cc, 0x13de, 0x13f1, 0x1403, 0x1416, 0x1428, 0x143b, 0x144e, 0x1460, 0x1473, 0x1486, 0x1499, 0x14ac, 0x14bf, 0x14d3, 0x14e6, 0x14f9, 0x150d, 0x1520, 0x1534, 0x1547, 0x155b, 0x156f, 0x1583, 0x1597, 0x15ab, 0x15bf, 0x15d3, 0x15e7, 0x15fb, 0x1610, 0x1624, 0x1638, 0x164d, 0x1662, 0x1676, 0x168b,
    0x16a0, 0x16b5, 0x16ca, 0x16df, 0x16f4, 0x170a, 0x171f, 0x1734, 0x174a, 0x175f, 0x1775, 0x178b, 0x17a1, 0x17b6, 0x17cc, 0x17e2, 0x17f9, 0x180f, 0x1825, 0x183b, 0x1852, 0x1868, 0x187f, 0x1896, 0x18ac, 0x18c3, 0x18da, 0x18f1, 0x1908, 0x191f, 0x1937, 0x194e, 0x1965, 0x197d, 0x1995, 0x19ac, 0x19c4, 0x19dc, 0x19f4, 0x1a0c, 0x1a24, 0x1a3c, 0x1a55, 0x1a6d, 0x1a85, 0x1a9e, 0x1ab7, 0x1acf, 0x1ae8, 0x1b01, 0x1b1a, 0x1b33, 0x1b4c, 0x1b66, 0x1b7f, 0x1b98, 0x1bb2, 0x1bcc, 0x1be5, 0x1bff, 0x1c19, 0x1c33, 0x1c4d, 0x1c67, 0x1c82, 0x1c9c, 0x1cb7, 0x1cd1, 0x1cec, 0x1d07, 0x1d22, 0x1d3d, 0x1d58, 0x1d73, 0x1d8e, 0x1da9, 0x1dc5, 0x1de0, 0x1dfc, 0x1e18, 0x1e34, 0x1e50, 0x1e6c, 0x1e88, 0x1ea4, 0x1ec1, 0x1edd, 0x1efa, 0x1f16, 0x1f33, 0x1f50, 0x1f6d, 0x1f8a, 0x1fa7, 0x1fc5, 0x1fe2,
};

/* Functions. */
static void spu_key_off(uint32 voice_mask);
static void spu_key_on(uint32 voice_mask);
static void spu_init(void);
static void spu_init_native_transfer(void);
static void spu_shutdown(void);
static void libsnd_reset(void);
static void cd_audio_stop_locked(void);

void SsInit(void)
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
    if (serial != SS_SERIAL_A)
        return;
    if (left < 0)
        left = 0;
    if (left > 127)
        left = 127;
    if (right < 0)
        right = 0;
    if (right > 127)
        right = 127;
    xport_audio_lock();
    libsnd.cd.volume_left = left;
    libsnd.cd.volume_right = right;
    xport_audio_unlock();
}

void SsSetTickMode(sint32 mode)
{
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
}

static void spu_init(void)
{
    memset(&spu, 0, sizeof(spu));
    spu.master_left = 0x3fff;
    spu.master_right = 0x3fff;
    spu.cd_left = 0;
    spu.cd_right = 0;
    spu.initialized = 1;
    reverb_control.control = 0xC000;
    reverb_control.enabled = 0;
    libsnd.cd.volume_left = 127;
    libsnd.cd.volume_right = 127;
    libsnd.cd.mix.val0 = 255;
    libsnd.cd.mix.val1 = 0;
    libsnd.cd.mix.val2 = 255;
    libsnd.cd.mix.val3 = 0;
    spu_init_native_transfer();
}

static void spu_shutdown(void)
{
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
        case 0x1F801D9Eu:
            *value = (uint16)(spu.end_flags >> 16);
            return 1;
        default:
            return 0;
    }
}

static sint32 spu_synchronize_to_ticks(uint64 target)
{
#if defined(LOCKSTEP_DEBUG)
    uint64 frames;

    if (!spu.lockstep_clock_valid)
    {
        spu.lockstep_ticks = target;
        spu.lockstep_clock_valid = 1u;
        return 1;
    }
    if (target < spu.lockstep_ticks)
    {
    #ifndef NDEBUG
        fprintf(stderr, "xport_spu_clock backward current=%llu target=%llu\n", (unsigned long long)spu.lockstep_ticks, (unsigned long long)target);
    #endif
        return 0;
    }
    frames = target - spu.lockstep_ticks;
    spu.lockstep_ticks = target;
    while (frames != 0u)
    {
        sint32 synchronized[SPU_MIX_CHUNK_FRAMES * 2] = {0};
        uint32 count = frames > SPU_MIX_CHUNK_FRAMES ? SPU_MIX_CHUNK_FRAMES : (uint32)frames;

        spu_mix(synchronized, count);
        frames -= count;
    }
    return 1;
#else
    (void)target;
    return 1;
#endif
}

static void spu_synchronize_device_clock(void)
{
#if defined(LOCKSTEP_DEBUG)
    uint64 target;

    if (spu.sample_clock_from_wait || spu.lockstep_vblank_render)
        return;
    if (xport_lockstep_spu_timeline_active())
        return;
    if (xport_lockstep_mode == LOCKSTEP_MODE_PREFIX_GUARD)
        return;
    if (xport_lockstep_device_ticks(&target) && !spu_synchronize_to_ticks(target))
        exit(81);
#endif
}

#if defined(LOCKSTEP_DEBUG)
static sint32 spu_synchronize_event(XportLockstepSpuEventKind kind, uint32 address, uint32 *value, sint32 validate_value)
{
    uint64 target;
    sint32 result = xport_lockstep_spu_event_consume(kind, address, value, validate_value, &target);

    if (result < 0)
        exit(82);
    if (result > 0 && !spu_synchronize_to_ticks(target))
        exit(83);
    return result;
}
#endif

static sint32 spu_mmio_timed_read16(uint32 physical_address, uint16 *value)
{
#if defined(LOCKSTEP_DEBUG)
    uint32 expected = 0u;
    sint32 synchronized = 0;
    if (physical_address >= 0x1F801C00 && physical_address < 0x1F801D80 && (physical_address & 15) == 12)
        synchronized = spu_synchronize_event(XPORT_LOCKSTEP_SPU_VOICE_READ, physical_address, &expected, 0);

    if (!synchronized)
        spu_synchronize_device_clock();
#else
    spu_synchronize_device_clock();
#endif
    if (!spu_mmio_read16(physical_address, value))
        return 0;
#if defined(LOCKSTEP_DEBUG)
    if (synchronized && *value != (uint16)expected)
    {
    #ifndef NDEBUG
        fprintf(stderr, "xport_spu_read mismatch address=%08X expected=%04X actual=%04X\n", physical_address, (uint16)expected, *value);
    #endif
        exit(84);
    }
#endif
    return 1;
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
    if (address >= 0x1F801C00 && address < 0x1F801D80 && !(address & 1))
        return 1;
    switch (address)
    {
        case 0x1F801D80u:
        case 0x1F801D82u:
        case 0x1F801D90:
        case 0x1F801D92:
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
            if (!spu_mmio_common_writable(address) || !spu_mmio_read16(address, &values[index]))
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
        uint32 changed = values[index] ^ previous[index];
        if ((addresses[index] == 0x1F801D80u || addresses[index] == 0x1F801D82u || (addresses[index] < 0x1F801D80 && (addresses[index] & 15) < 4)) && (values[index] & 0x8000u))
        {
#ifndef NDEBUG
            fprintf(stderr, "xport_spu_write unsupported volume sweep address=%08X value=%04X\n", addresses[index], values[index]);
#endif
            goto finish;
        }
        if (addresses[index] == 0x1F801DAAu)
        {
            /* Bits 0 and 1 enable dry CD and external input mixing
               Bits 2 and 3 route those inputs to the reverb processor
               This host has no external analog input, so that bus is silent */
            if (changed & ~0xC0CFu)
            {
#ifndef NDEBUG
                fprintf(stderr, "xport_spu_write unsupported control routing value=%04X changed=%04X\n", values[index], changed);
#endif
                goto finish;
            }
        }
    }
    for (index = 0u; index < count; ++index)
    {
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
            case 0x1F801D90:
                spu.pitch_modulation = (spu.pitch_modulation & 0xFFFF0000) | values[index];
                break;
            case 0x1F801D92:
                spu.pitch_modulation = (spu.pitch_modulation & 0xFFFF) | ((uint32)values[index] << 16);
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

static void libsnd_reset(void)
{
    sint32 voice;
    memset(&libsnd, 0, sizeof(libsnd));
    libsnd.next_sound_ram_address = 0x1010;
    libsnd.vab_transfer_complete = 1;
    libsnd.cd.track = -1;
    libsnd.cd.volume_left = 127;
    libsnd.cd.volume_right = 127;
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
    return &libsnd.banks[bank].tones[program][tone];
}

static sint16 vab_open_head(const uint8 *header, sint16 requested_bank)
{
    const VAB_HEADER_RECORD *header_record = (const VAB_HEADER_RECORD *)header;
    VAB_BANK parsed;
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

    memset(&parsed, 0, sizeof(parsed));
    parsed.master_volume = header_record->master_volume;
    parsed.master_pan = header_record->master_pan;
    parsed.program_count = (uint8)program_count;
    for (program = 0; program < VAB_PROGRAM_COUNT; ++program)
    {
        const VAB_PROGRAM_RECORD *source = (const VAB_PROGRAM_RECORD *)(header_record + 1) + program;
        VAB_PROGRAM *destination = parsed.programs + program;
        uint32 tone;
        if (source->tone_count == 0)
            continue;
        if (source->tone_count > VAB_TONES_PER_PROGRAM || tone_group >= program_count)
            return -1;
        destination->tone_count = source->tone_count;
        destination->volume = source->volume;
        destination->priority = source->priority;
        destination->mode = source->mode;
        destination->pan = source->pan;
        for (tone = 0; tone < source->tone_count; ++tone)
        {
            const VAB_TONE_RECORD *tone_source = (const VAB_TONE_RECORD *)((const VAB_PROGRAM_RECORD *)(header_record + 1) + VAB_PROGRAM_COUNT) + tone_group * VAB_TONES_PER_PROGRAM + tone;
            VAB_TONE *tone_destination = &parsed.tones[program][tone];
            tone_destination->priority = tone_source->priority;
            tone_destination->mode = tone_source->mode;
            tone_destination->volume = tone_source->volume;
            tone_destination->pan = tone_source->pan;
            tone_destination->center = tone_source->center;
            tone_destination->shift = tone_source->shift;
            tone_destination->note_min = tone_source->note_min;
            tone_destination->note_max = tone_source->note_max;
            tone_destination->pitch_bend_min = tone_source->pitch_bend_min;
            tone_destination->pitch_bend_max = tone_source->pitch_bend_max;
            tone_destination->adsr1 = tone_source->adsr1;
            tone_destination->adsr2 = tone_source->adsr2;
            tone_destination->program = tone_source->program;
            tone_destination->sample = tone_source->sample;
            // Zero denotes an unused tone with no sample address
            if (tone_destination->sample < 0 || tone_destination->sample > (sint16)sample_count)
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
            parsed.sample_addresses[program] = libsnd.next_sound_ram_address + sample_offset;
        sample_offset += size;
    }
    parsed.body_size = sample_offset;
    if (parsed.body_size == 0 || libsnd.next_sound_ram_address > SPU_RAM_SIZE || parsed.body_size > SPU_RAM_SIZE - libsnd.next_sound_ram_address)
        return -1;
    parsed.body_address = libsnd.next_sound_ram_address;
    parsed.valid = 1;
    libsnd.banks[bank] = parsed;
    libsnd.next_sound_ram_address = (libsnd.next_sound_ram_address + parsed.body_size + 7u) & ~7u;
    return bank;
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

sint32 spu_native_vab_address(sint16 bank)
{
    sint32 address = -1;
    xport_audio_lock();
    if (bank >= 0 && bank < VAB_BANK_COUNT && libsnd.banks[bank].valid)
        address = (sint32)libsnd.banks[bank].body_address;
    xport_audio_unlock();
    return address;
}

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
    pitch = libsnd_pitch_table[remainder * 16 + fine_index];
    if (octave > 5)
        pitch <<= octave - 5;
    else if (octave < 5)
        pitch >>= 5 - octave;
    return (uint16)pitch;
}

static uint16 spu_note_to_pitch(uint16 sample_note, uint16 note)
{
    sint32 fine = ((sint32)(note & 0xffu) + (sint32)(sample_note & 0xffu)) / 8;
    sint32 carry = fine / 16;
    sint32 semitone;
    sint32 octave;
    sint32 remainder;
    uint32 pitch;

    fine %= 16;
    semitone = carry + (sint32)(note >> 8) + 60 - (sint32)(sample_note >> 8);
    octave = semitone / 12;
    if (semitone < 0 && semitone % 12)
        --octave;
    remainder = semitone - octave * 12;
    pitch = libsnd_pitch_table[remainder * 16 + fine];
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
    registers.volume_left = compose_volume(left, right, libsnd.banks[bank].master_volume, program_entry, tone, 1);
    registers.volume_right = compose_volume(left, right, libsnd.banks[bank].master_volume, program_entry, tone, 0);
    registers.pitch = note_to_pitch(note, (uint16)fine, tone);
    registers.start_address = (uint16)(sample_address >> 3);
    registers.repeat_address = registers.start_address;
    registers.adsr1 = tone->adsr1;
    registers.adsr2 = tone->adsr2;
    spu_set_voice_registers(voice, &registers);
    spu.voices[voice].sample_note = (uint16)(((uint16)tone->center << 8) | tone->shift);
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

sint16 SsUtKeyOn(sint16 bank, sint16 program, sint16 tone, sint16 note, sint16 fine, sint16 left, sint16 right)
{
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
    return binding && binding->context && binding->voices && binding->program_pointer && binding->header_pointer && binding->tone_pointer && binding->bank_flags && binding->program_limit && binding->header_table && binding->program_table && binding->tone_table && binding->voice_count && psx_addr(binding->context, 32u) && psx_addr(binding->voices, 54u) && psx_addr(binding->program_pointer, 4u) && psx_addr(binding->header_pointer, 4u) && psx_addr(binding->tone_pointer, 4u) && psx_addr(binding->bank_flags, 16u) && psx_addr(binding->program_limit, 2u) && psx_addr(binding->header_table, 64u) && psx_addr(binding->program_table, 64u) && psx_addr(binding->tone_table, 64u) && psx_addr(binding->voice_count, 1u);
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

static uint32 libsnd_voice_protection(void)
{
    return libsnd.voice_protection_address ? r_u32(libsnd.voice_protection_address) : 0u;
}

static sint16 libsnd_voice_count(void)
{
    sint16 count = libsnd.voice_count_address ? (sint8)r_u8(libsnd.voice_count_address) : SPU_VOICE_COUNT;
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
    sint16 result;
    xport_audio_lock();
    result = start_voice(voice, bank, program, tone, note, fine, left, right);
    xport_audio_unlock();
    return result;
}

static void spu_init_native_transfer(void)
{
    SpuTransferBinding binding = {0};
    SPU_TRANSFER_STATE *state = &spu.transfer;

    if (transfer.requested_mode && native_transfer_binding != SPU_TRANSFER_NATIVE)
        return;
    state->complete = 1u;
    state->addr_units = 0x200u;
    binding.requested_mode = &state->req_mode;
    binding.normalized_mode = &state->mode;
    binding.address_units = &state->addr_units;
    binding.completion = &state->complete;
    binding.completion_callback = &state->callback;
    binding.source_address = &state->src_addr;
    binding.dma_blocks = &state->blocks;
    binding.address_shift = 3u;
    binding.address_rounding_enabled = 1u;
    binding.address_rounding_unit = 8u;
    binding.address_alignment_mask = 7u;
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

void SpuInit(void)
{
    xport_audio_lock();
    spu_init();
    memset(&heap, 0, sizeof(heap));
    heap_failed = 0;
    reverb_control.reserved = 0u;
    reverb_control.work_address = 0xF204u;
    xport_audio_unlock();
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
    binding.address_shift = 3u;
    binding.alignment_mask = 7u;
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

void SpuSetVoiceAttr(SpuVoiceAttr *attr)
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
            spu.voices[voice].sample_note = attr->sample_note;
        if (!attr->mask || (attr->mask & SPU_VOICE_NOTE))
            registers.pitch = spu_note_to_pitch(spu.voices[voice].sample_note, attr->note);
        if (!attr->mask || (attr->mask & SPU_VOICE_WDSA))
            registers.start_address = (uint16)(attr->addr >> 3);
        if (!attr->mask || (attr->mask & SPU_VOICE_LSAX))
            registers.repeat_address = (uint16)(attr->loop_addr >> 3);
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
#if defined(LOCKSTEP_DEBUG)
    uint32 expected_envelope = 0u;
    sint32 synchronized;
#endif
    if (!attr)
        return;
    requested = attr->voice;
    for (voice = 0; voice < SPU_VOICE_COUNT && !(requested & SPU_KEYCH(voice)); ++voice)
        ;
    if (voice == SPU_VOICE_COUNT)
        return;
    xport_audio_lock();
#if defined(LOCKSTEP_DEBUG)
    synchronized = spu_synchronize_event(XPORT_LOCKSTEP_SPU_VOICE_READ, 0x1F801C00u + (uint32)voice * 16u + 12u, &expected_envelope, 0);
    if (!synchronized)
        spu_synchronize_device_clock();
#else
    spu_synchronize_device_clock();
#endif
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
    result.sample_note = spu.voices[voice].sample_note;
    result.envx = (sint16)spu.voices[voice].envelope;
    result.addr = (uint32)registers.start_address * 8u;
    result.loop_addr = (uint32)registers.repeat_address * 8u;
    result.adsr1 = registers.adsr1;
    result.adsr2 = registers.adsr2;
#if defined(LOCKSTEP_DEBUG)
    if (synchronized && (uint16)expected_envelope != (uint16)result.envx)
    {
    #ifndef NDEBUG
        fprintf(stderr, "xport_spu_env mismatch voice=%d expected=%04X actual=%04X\n", voice, (uint16)expected_envelope, (uint16)result.envx);
    #endif
        exit(85);
    }
#endif
    *attr = result;
    xport_audio_unlock();
}

static uint32 spu_key_guest_addresses[5];

void spu_bind_key_guest(uint32 state_address, uint32 event_mode_address, uint32 event_flags_address, uint32 pending_mask_address, uint32 registers_address)
{
    spu_key_guest_addresses[0] = state_address;
    spu_key_guest_addresses[1] = event_mode_address;
    spu_key_guest_addresses[2] = event_flags_address;
    spu_key_guest_addresses[3] = pending_mask_address;
    spu_key_guest_addresses[4] = registers_address;
}

void SpuSetKey(sint32 on_off, uint32 voice_bit)
{
    uint32 cached;
    uint32 pending;
    uint32 opposite;
    uint32 low;
    uint32 high;

    voice_bit &= SPU_ALLCH;
    if (on_off != SPU_ON && on_off != SPU_OFF)
        return;
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
        if (r_u16(opposite + 2u) & high)
            w_u16(opposite + 2u, (uint16)(r_u16(opposite + 2u) & ~high));
    }
    else if (on_off == SPU_ON)
    {
        cached = spu_key_guest_addresses[0] ? r_u32(spu_key_guest_addresses[0]) : 0u;
        spu_key_on(voice_bit);
        if (spu_key_guest_addresses[0])
            w_u32(spu_key_guest_addresses[0], cached | voice_bit);
    }
    else
    {
        spu_key_off(voice_bit);
        if (spu_key_guest_addresses[0])
            w_u32(spu_key_guest_addresses[0], r_u32(spu_key_guest_addresses[0]) & ~voice_bit);
        /* A later SDK stop cancels an unrendered SDK start */
        spu.key_on_pending &= ~voice_bit;
    }
    xport_audio_unlock();
}

uint32 SpuSetReverbVoice(sint32 on_off, uint32 voice_bit)
{
    uint32 result;
    xport_audio_lock();
    if (on_off == SPU_ON)
        spu.reverb_voice_mask |= voice_bit & SPU_ALLCH;
    else if (on_off == SPU_OFF)
        spu.reverb_voice_mask &= ~(voice_bit & SPU_ALLCH);
    result = spu.reverb_voice_mask;
    xport_audio_unlock();
    return result;
}

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

sint32 spu_clear_reverb_transfer(sint32 mode, const void *source, const SpuTransferBinding *binding, uint32 *direction)
{
    uint32 units, begin, length, normalized, callback, guest;
    sint32 result = 0;
    if ((uint32)mode >= 10u || !source || !binding || !binding->normalized_mode || !binding->completion_callback || !binding->address_units || !binding->source_address || !binding->dma_blocks || binding->address_shift > 3u || binding->block_bytes != 64u || ((binding->clock != NULL) != (binding->wait_until != NULL)) || (binding->clock && !binding->ticks_per_block) || !binding->complete_event)
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
    if (units > 0x10000u)
    {
        xport_audio_unlock();
        return -1;
    }
    begin = units << binding->address_shift;
    length = (0x10000u - units) << binding->address_shift;
    xport_audio_unlock();
    guest = binding->guest_address ? binding->guest_address(binding->context, source) : xport_guest_buffer_address((void *)source, 1024);
    normalized = *binding->normalized_mode;
    callback = *binding->completion_callback;
    if (normalized == 1u)
        *binding->normalized_mode = 0u;
    if (callback)
        *binding->completion_callback = 0u;
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
    return result;
}

sint32 SpuClearReverbWorkArea(sint32 mode)
{
    static const uint8 zero[1024] = {0};
    if (transfer_pending || transfer_operation.active || transfer_failed)
        return -1;
    return spu_clear_reverb_transfer(mode, zero, &transfer, NULL);
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
#if !defined(LOCKSTEP_DEBUG)
        // Pending starts are active before the mixer updates their envelope
        status = (spu.key_on_pending & SPU_KEYCH(voice)) || spu.voices[voice].envelope != 0 ? SPU_ON : SPU_ON_ENV_OFF;
#else
        status = spu.voices[voice].envelope != 0 ? SPU_ON : SPU_ON_ENV_OFF;
#endif
    }
    else
        status = spu.voices[voice].envelope != 0 ? SPU_OFF_ENV_ON : SPU_OFF;
    xport_audio_unlock();
    return status;
}

// Native common attributes target the mixer through the SPU device boundary
static uint32 spu_common_base(void)
{
#if defined(LOCKSTEP_DEBUG)
    extern const uint32 xport_spu_register_pointer_address;
    return r_u32(xport_spu_register_pointer_address);
#else
    return 0x1F801C00u;
#endif
}

static uint16 spu_common_read(uint32 address)
{
#if defined(LOCKSTEP_DEBUG)
    return r_u16(address);
#else
    uint16 value;
    sint32 valid;
    xport_audio_lock();
    valid = spu_mmio_read16(address, &value);
    xport_audio_unlock();
    if (!valid)
        abort();
    return value;
#endif
}

static void spu_common_write(uint32 address, uint16 value)
{
#if defined(LOCKSTEP_DEBUG)
    w_u16(address, value);
#else
    if (!spu_mmio_write(NULL, address, 2u, value, 3u))
        abort();
#endif
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
    xport_audio_unlock();
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

#if defined(LOCKSTEP_DEBUG)
int spu_lockstep_checkpoint_rebase_clock(void)
{
    xport_audio_lock();
    spu.lockstep_ticks = 0u;
    spu.lockstep_clock_valid = 1u;
    xport_audio_unlock();
    return 1;
}
#endif

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
#if defined(LOCKSTEP_DEBUG)
    uint32 low = voice_mask & 0xffffu;
    uint32 high = voice_mask >> 16;
    sint32 synchronized = spu_synchronize_event(XPORT_LOCKSTEP_SPU_KEY_ON_LOW, 0x1F801D88u, &low, 1);

    synchronized |= spu_synchronize_event(XPORT_LOCKSTEP_SPU_KEY_ON_HIGH, 0x1F801D8Au, &high, 1);
    if (!synchronized)
        spu_synchronize_device_clock();
#else
    spu_synchronize_device_clock();
#endif
    if (!spu.initialized)
        return;
    spu.key_status |= voice_mask;
    spu.key_on_pending |= voice_mask;
}

static void spu_key_off(uint32 voice_mask)
{
#if defined(LOCKSTEP_DEBUG)
    uint32 low = voice_mask & 0xffffu;
    uint32 high = voice_mask >> 16;
    sint32 synchronized = spu_synchronize_event(XPORT_LOCKSTEP_SPU_KEY_OFF_LOW, 0x1F801D8Cu, &low, 1);

    synchronized |= spu_synchronize_event(XPORT_LOCKSTEP_SPU_KEY_OFF_HIGH, 0x1F801D8Eu, &high, 1);
    if (!synchronized)
        spu_synchronize_device_clock();
#else
    spu_synchronize_device_clock();
#endif
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
EM_JS(uint32, web_music_size, (sint32 track), { return Module.xportManifest.music[track] ?.size || 0; });

EM_ASYNC_JS(sint32, web_music_open, (sint32 track, uint8 *data), {
    const state = await Module.xportMusicOpen(track);
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

static sint32 ima_load_block(IMA_ADPCM_STREAM *stream)
{
    const uint8 *block;
    uint32 header_size = stream->channels * 4u;
    sint32 channel;
#ifdef AP_WEB
    sint32 available = web_music_fill(stream->file_data);
    uint32 needed = (uint32)(stream->audio_data - stream->file_data) + stream->block_offset;
    uint32 remaining = stream->audio_size - stream->block_offset;
    needed += remaining < stream->block_align ? remaining : stream->block_align;
    if (available < 0)
    {
        stream->total_frames = stream->decoded_frames;
        return 0;
    }
    if ((uint32)available < needed)
        return 0;
#endif
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

static uint8 *cd_track_load(sint32 track, uint32 *size)
{
    char path[64];
#ifdef AP_WEB
    uint32 length = web_music_size(track);
    uint8 *data = length ? (uint8 *)calloc(1, length) : 0;
    if (!data || !web_music_open(track, data) || web_music_fill(data) < 0)
    {
        web_music_close(data);
        free(data);
        return 0;
    }
    *size = (uint32)length;
    return (uint8 *)data;
#else
    FILE *file;
    long length;
    uint8 *data;
    snprintf(path, sizeof(path), "MUSIC/%d.WAV", track);
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
    data = (uint8 *)malloc((size_t)length);
    if (data == 0 || fread(data, 1, (size_t)length, file) != (size_t)length)
    {
        free(data);
        fclose(file);
        return 0;
    }
    fclose(file);
    *size = (uint32)length;
    return data;
#endif
}

static void cd_audio_stop_locked(void)
{
#if defined(LOCKSTEP_DEBUG)
    if (libsnd.cd.data)
        xport_lockstep_checkpoint_invalidate();
#endif
#ifdef AP_WEB
    web_music_close(libsnd.cd.data);
#endif
    free(libsnd.cd.data);
    libsnd.cd.data = 0;
    memset(&libsnd.cd.stream, 0, sizeof(libsnd.cd.stream));
    libsnd.cd.track = -1;
    libsnd.cd_end_callback = NULL;
    libsnd.cd.active = 0;
    libsnd.cd.paused = 0;
}

sint32 CdPlay(sint32 mode, sint32 *track, sint32 offset)
{
    IMA_ADPCM_STREAM stream;
    uint8 *data;
    uint32 size;
    sint32 selected;
    CdlCB callback;
    if (track == 0 || mode < 0 || offset < 0)
        return 0;
    selected = *track;
    data = cd_track_load(selected, &size);
    if (data == 0 || !ima_open(&stream, data, size))
    {
#ifdef AP_WEB
        web_music_close(data);
#endif
        free(data);
        return 0;
    }
    xport_audio_lock();
#if defined(LOCKSTEP_DEBUG)
    xport_lockstep_checkpoint_invalidate();
#endif
    cd_audio_stop_locked();
    libsnd.cd.data = data;
    libsnd.cd.stream = stream;
    libsnd.cd.track = selected;
    libsnd.cd.active = 1;
    libsnd.cd.repeat = mode != 0;
    callback = libsnd.sync_callback;
    xport_audio_unlock();
    if (callback != 0)
        cd_queue_callback(0, CdlComplete, NULL);
    return 1;
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

static sint32 cd_command_completion = CdlNoIntr;
static uint8 cd_command_response[8];

sint32 CdSetDebug(sint32 level)
{
    static sint32 debug_level;
    sint32 previous = debug_level;
    debug_level = level;
    return previous;
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

sint32 CdControl(uint8 command, uint8 *parameter, uint8 *result)
{
    uint8 response[8] = {0};
    sint32 accepted = cd_control_dispatch(command, parameter, response);
    xport_audio_lock();
    cd_command_completion = accepted ? CdlComplete : CdlDiskError;
    memcpy(cd_command_response, response, sizeof(cd_command_response));
    xport_audio_unlock();
    if (result)
        memcpy(result, response, command == 16 || command == 17 ? 8 : 1);
    return accepted;
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
        (void)cd_guest.resolve(target)(r_u8(cd_guest.events + 1), cd_guest.responses + 8);
    if ((callbacks & 2) && (target = r_u32(xport_cd_sync_callback_address)) != 0)
        (void)cd_guest.resolve(target)(r_u8(cd_guest.events), cd_guest.responses);
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
        dispatched = CdControl(command, (uint8 *)parameter, result ? result : response);
        xport_audio_lock();
        cd_command_completion = dispatched ? CdlComplete : CdlDiskError;
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
    cd_command_completion = dispatched ? CdlComplete : CdlDiskError;
    memcpy(cd_command_response, response, sizeof(cd_command_response));
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
        completion = cd_command_completion;
        if (result)
            memcpy(result, cd_command_response, sizeof(cd_command_response));
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
        uint8 status, response, has_response;
        uint32 index;
        xport_audio_lock();
        if (!libsnd.pending_callback_count)
        {
            xport_audio_unlock();
            break;
        }
        index = libsnd.pending_callback_read;
        callback = libsnd.pending_callbacks[index].ready ? libsnd.ready_callback : libsnd.sync_callback;
        status = libsnd.pending_callbacks[index].status;
        response = libsnd.pending_callbacks[index].response;
        has_response = libsnd.pending_callbacks[index].has_response;
        libsnd.pending_callback_read = (index + 1) & 255;
        --libsnd.pending_callback_count;
        xport_audio_unlock();
        if (callback)
            callback(status, has_response ? &response : NULL);
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

static void spu_mix_voices(sint32 *interleaved_stereo, sint32 *sends, uint32 frame_count)
{
    uint32 frame;
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
            sample = interpolate_voice(voice);
            amplitude = apply_volume(sample, voice->envelope);
            {
                sint32 left = apply_volume(amplitude, fixed_volume((uint16)voice->registers.volume_left));
                sint32 right = apply_volume(amplitude, fixed_volume((uint16)voice->registers.volume_right));
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
                    if (!(voice->block_flags & 2))
                    {
                        voice->adsr_phase = SPU_ADSR_OFF;
                        voice->envelope = 0;
                    }
                }
            }
        }
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

void spu_render(sint16 *interleaved_stereo, uint32 frame_count)
{
    uint32 offset = 0;
#if defined(LOCKSTEP_DEBUG)
    CdlCB end_callback = NULL;
#endif
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
        uint32 sample;
        if (count > SPU_MIX_CHUNK_FRAMES)
            count = SPU_MIX_CHUNK_FRAMES;
        memset(mixed, 0, count * 2u * sizeof(*mixed));
        memset(sends, 0, count * 2u * sizeof(*sends));
#if defined(LOCKSTEP_DEBUG)
        if (spu.lockstep_clock_valid && !spu.lockstep_vblank_render)
            synchronized = count;
#endif
        if (synchronized < count)
            spu_mix_voices(mixed + synchronized * 2u, sends + synchronized * 2u, count - synchronized);
        if (cd_xa_active_locked() || (libsnd.cd.active && !libsnd.cd.paused && !libsnd.cd.muted))
        {
            sint32 xa_source = cd_xa_active_locked();
            uint32 produced = xa_source ? cd_xa_render_locked(decoded, count) : ima_decode(&libsnd.cd.stream, decoded, count);
            while (!xa_source && produced < count && libsnd.cd.repeat && libsnd.cd.stream.decoded_frames >= libsnd.cd.stream.total_frames)
            {
                IMA_ADPCM_STREAM stream;
                uint32 restarted;
                if (!ima_open(&stream, libsnd.cd.data, libsnd.cd.stream.file_size))
                    break;
                libsnd.cd.stream = stream;
                restarted = ima_decode(&libsnd.cd.stream, decoded + produced * 2u, count - produced);
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
                sint32 cd_left = routed_left * libsnd.cd.volume_left / 127;
                sint32 cd_right = routed_right * libsnd.cd.volume_right / 127;
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
            if (!xa_source && produced < count
#ifdef AP_WEB
                && libsnd.cd.stream.decoded_frames >= libsnd.cd.stream.total_frames
#endif
            )
            {
                libsnd.cd.active = 0;
#if defined(LOCKSTEP_DEBUG)
                end_callback = libsnd.ready_callback;
#else
                libsnd.cd_end_callback = libsnd.ready_callback;
#endif
            }
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
#if defined(LOCKSTEP_DEBUG)
    if (end_callback)
        cd_queue_callback(1, CdlDataEnd, NULL);
#endif
}

void spu_render_vblank(sint16 *interleaved_stereo, uint32 frame_count)
{
#if defined(LOCKSTEP_DEBUG)
    uint64 target;
    uint64 frames;
    uint32 output_frames;

    if (spu.sample_clock_from_wait)
    {
        if (interleaved_stereo && frame_count)
            memset(interleaved_stereo, 0, (size_t)frame_count * 2 * sizeof(*interleaved_stereo));
        return;
    }

    if (!interleaved_stereo || !frame_count)
        return;
    if (!xport_lockstep_device_ticks(&target))
    {
        spu_render(interleaved_stereo, frame_count);
        return;
    }
    xport_audio_lock();
    if (!spu.lockstep_clock_valid)
        frames = frame_count;
    else if (target >= spu.lockstep_ticks)
        frames = target - spu.lockstep_ticks;
    else
    {
        xport_audio_unlock();
        exit(86);
    }
    spu.lockstep_ticks = target;
    spu.lockstep_clock_valid = 1u;
    spu.lockstep_vblank_render = 1u;
    xport_audio_unlock();

    memset(interleaved_stereo, 0, (size_t)frame_count * 2u * sizeof(*interleaved_stereo));
    output_frames = frames > frame_count ? frame_count : (uint32)frames;
    if (output_frames)
        spu_render(interleaved_stereo, output_frames);
    frames -= output_frames;
    while (frames)
    {
        sint16 discarded[SPU_MIX_CHUNK_FRAMES * 2];
        uint32 count = frames > SPU_MIX_CHUNK_FRAMES ? SPU_MIX_CHUNK_FRAMES : (uint32)frames;

        spu_render(discarded, count);
        frames -= count;
    }
    xport_audio_lock();
    spu.lockstep_vblank_render = 0u;
    xport_audio_unlock();
#else
    spu_render(interleaved_stereo, frame_count);
#endif
}

static int spu_state_block(FILE *file, void *data, size_t size, int load)
{
    uint32 stored = (uint32)size;
    if (load)
        return fread(&stored, 4, 1, file) == 1 && stored == size && fread(data, 1, size, file) == size;
    return fwrite(&stored, 4, 1, file) == 1 && fwrite(data, 1, size, file) == size;
}

int spu_state_io(FILE *f, int load)
{
    return spu_state_block(f, &spu, sizeof(spu), load);
}

typedef struct SPU_PORTABLE_BINDINGS
{
    uint32 transfer_owner;
    SpuNativeTransferGuestBinding transfer_guest;
    uint32 heap_bound;
    uint32 heap_entries_offset;
    uint32 heap_last_index_offset;
    uint32 heap_capacity;
    uint32 heap_limit;
    uint32 heap_address_shift;
    uint32 heap_alignment_mask;
    uint32 heap_reserved_bytes;
    sint32 heap_failed;
    SpuReverbControl reverb;
    sint32 reverb_incomplete;
    sint32 transfer_failed;
    uint32 transfer_byte_address;
    uint64 transfer_deadline;
    sint32 transfer_pending;
} SPU_PORTABLE_BINDINGS;

static int spu_portable_bindings_pack(SPU_PORTABLE_BINDINGS *saved)
{
    XportMemoryRegion region;
    if (!saved || (transfer.requested_mode && native_transfer_binding == SPU_TRANSFER_CUSTOM) || transfer_pending || transfer_operation.active || reverb_work_check)
        return 0;
    memset(saved, 0, sizeof(*saved));
    saved->transfer_owner = native_transfer_binding;
    if (native_transfer_binding == SPU_TRANSFER_GUEST)
    {
        SpuNativeTransferGuestBinding *guest = &saved->transfer_guest;
        guest->requested_mode = xport_guest_buffer_address(transfer.requested_mode, 4);
        guest->normalized_mode = xport_guest_buffer_address(transfer.normalized_mode, 4);
        guest->address_units = xport_guest_buffer_address(transfer.address_units, 2);
        guest->completion = xport_guest_buffer_address(transfer.completion, 4);
        guest->completion_callback = xport_guest_buffer_address(transfer.completion_callback, 4);
        guest->source_address = xport_guest_buffer_address(transfer.source_address, 4);
        guest->dma_blocks = xport_guest_buffer_address(transfer.dma_blocks, 4);
        guest->address_shift = transfer.address_shift;
        if (!guest->requested_mode || !guest->normalized_mode || !guest->address_units || !guest->completion || !guest->completion_callback || !guest->source_address || !guest->dma_blocks)
            return 0;
    }
    saved->heap_bound = heap.entries != NULL;
    if (saved->heap_bound)
    {
        if (!heap.last_index || !heap.capacity || heap.capacity > PSX_DRAM_SIZE / 8u || !xport_memory_pointer_identity(heap.entries, (size_t)heap.capacity * 8u, &region, &saved->heap_entries_offset) || region != XPORT_MEMORY_DRAM || !xport_memory_pointer_identity(heap.last_index, sizeof(uint32), &region, &saved->heap_last_index_offset) || region != XPORT_MEMORY_DRAM)
            return 0;
        saved->heap_capacity = heap.capacity;
        saved->heap_limit = heap.limit;
        saved->heap_address_shift = heap.address_shift;
        saved->heap_alignment_mask = heap.alignment_mask;
        saved->heap_reserved_bytes = heap.reserved_bytes;
    }
    saved->heap_failed = heap_failed;
    saved->reverb = reverb_control;
    saved->reverb_incomplete = reverb_incomplete;
    saved->transfer_failed = transfer_failed;
    saved->transfer_byte_address = transfer_byte_address;
    saved->transfer_deadline = transfer_deadline;
    saved->transfer_pending = transfer_pending;
    return 1;
}

static int spu_portable_bindings_unpack(const SPU_PORTABLE_BINDINGS *saved, SpuHeapBinding *restored_heap)
{
    if (!saved || !restored_heap || saved->heap_bound > 1u || saved->transfer_pending || saved->transfer_owner > SPU_TRANSFER_NATIVE || saved->reverb.control > 0xffffu || saved->reverb.enabled > 1u || ((saved->reverb.control >> 7) & 1u) != saved->reverb.enabled)
        return 0;
    memset(restored_heap, 0, sizeof(*restored_heap));
    if (!saved->heap_bound)
        return !saved->heap_capacity && !saved->heap_entries_offset && !saved->heap_last_index_offset;
    if (!saved->heap_capacity || saved->heap_capacity > PSX_DRAM_SIZE / 8u || saved->heap_limit >= saved->heap_capacity || saved->heap_address_shift > 31u)
        return 0;
    restored_heap->entries = (uint32 *)xport_guest_ptr(0x80000000u + saved->heap_entries_offset, (size_t)saved->heap_capacity * 8u);
    restored_heap->last_index = (uint32 *)xport_guest_ptr(0x80000000u + saved->heap_last_index_offset, sizeof(uint32));
    if (!restored_heap->entries || !restored_heap->last_index)
        return 0;
    restored_heap->capacity = saved->heap_capacity;
    restored_heap->limit = saved->heap_limit;
    restored_heap->address_shift = saved->heap_address_shift;
    restored_heap->alignment_mask = saved->heap_alignment_mask;
    restored_heap->reserved_bytes = saved->heap_reserved_bytes;
    return 1;
}

static int spu_portable_cd_header(const LIBSND_STATE *state, uint32 *size, uint32 *audio_offset)
{
    uintptr_t base;
    uintptr_t audio;
    if (!state || !size || !audio_offset || state->data_callback || state->ready_callback || state->cd_end_callback || state->sync_callback)
        return 0;
    *size = 0u;
    *audio_offset = 0u;
    if (!state->cd.data)
        return !state->cd.active && !state->cd.stream.file_data && !state->cd.stream.audio_data && !state->cd.stream.file_size && !state->cd.stream.audio_size;
    base = (uintptr_t)state->cd.data;
    audio = (uintptr_t)state->cd.stream.audio_data;
    if (state->cd.stream.file_data != state->cd.data || !state->cd.stream.file_size || state->cd.stream.file_size > 256u * 1024u * 1024u || audio < base || audio - base >= state->cd.stream.file_size || state->cd.stream.audio_size > state->cd.stream.file_size - (uint32)(audio - base))
        return 0;
    *size = state->cd.stream.file_size;
    *audio_offset = (uint32)(audio - base);
    return 1;
}

int spu_lockstep_portable_io(FILE *file, int load)
{
    LIBSND_STATE saved = {0};
    SPU_PORTABLE_BINDINGS bindings = {0};
    SpuHeapBinding restored_heap = {0};
    IMA_ADPCM_STREAM parsed;
    uint8 *data = NULL;
    uint32 data_size = 0u;
    uint32 audio_offset = 0u;
    int ok;
    if (!file)
        return 0;
    xport_audio_lock();
    if (load)
    {
        ok = spu_state_io(file, 1) && spu_state_block(file, &saved, sizeof(saved), 1) && fread(&data_size, sizeof(data_size), 1, file) == 1 && fread(&audio_offset, sizeof(audio_offset), 1, file) == 1 && !saved.cd.data && !saved.cd.stream.file_data && !saved.cd.stream.audio_data && !saved.data_callback && !saved.ready_callback && !saved.cd_end_callback && !saved.sync_callback && (!saved.voice_protection_address || psx_addr(saved.voice_protection_address, 4u)) && (!saved.voice_count_address || psx_addr(saved.voice_count_address, 1u)) && ((!saved.voice_volume_address && !saved.voice_update_flags_address) || (saved.voice_volume_address && saved.voice_update_flags_address && psx_addr(saved.voice_volume_address, 16u * (SPU_VOICE_COUNT - 1u) + 4u) && psx_addr(saved.voice_update_flags_address, SPU_VOICE_COUNT))) && (!saved.sequence_table_address || psx_addr(saved.sequence_table_address, 4u)) && (!saved.sequence_handlers_address || psx_addr(saved.sequence_handlers_address, 68u)) &&
             (!saved.sequence_mark_table_address || psx_addr(saved.sequence_mark_table_address, 4u)) && (!saved.vm_guest.context || spu_vm_guest_valid(&saved.vm_guest)) && data_size <= 256u * 1024u * 1024u;
        if (ok && data_size)
        {
            data = (uint8 *)malloc(data_size);
            ok = data && fread(data, 1, data_size, file) == data_size && ima_open(&parsed, data, data_size) && audio_offset == (uint32)(parsed.audio_data - data) && saved.cd.stream.file_size == data_size && saved.cd.stream.audio_size == parsed.audio_size && saved.cd.stream.channels == parsed.channels && saved.cd.stream.sample_rate == parsed.sample_rate && saved.cd.stream.block_align == parsed.block_align && saved.cd.stream.samples_per_block == parsed.samples_per_block && saved.cd.stream.total_frames <= parsed.total_frames && saved.cd.stream.decoded_frames <= saved.cd.stream.total_frames && saved.cd.stream.block_offset <= saved.cd.stream.audio_size && saved.cd.stream.sample_in_block <= saved.cd.stream.samples_per_block && saved.cd.stream.step_index[0] <= 88 && saved.cd.stream.step_index[1] <= 88;
            if (ok)
            {
                saved.cd.data = data;
                saved.cd.stream.file_data = data;
                saved.cd.stream.audio_data = data + audio_offset;
            }
        }
        else if (ok)
            ok = !audio_offset && !saved.cd.active && !saved.cd.stream.file_size && !saved.cd.stream.audio_size;
        if (ok)
            ok = spu_state_block(file, &bindings, sizeof(bindings), 1) && spu_portable_bindings_unpack(&bindings, &restored_heap);
        if (ok)
        {
            SPU_TRANSFER_STATE state = spu.transfer;
            spu_bind_transfer(NULL);
            if (bindings.transfer_owner == SPU_TRANSFER_NATIVE)
            {
                spu_init_native_transfer();
                spu.transfer = state;
            }
            else if (bindings.transfer_owner == SPU_TRANSFER_GUEST)
                ok = spu_bind_native_transfer_guest(&bindings.transfer_guest);
        }
        if (ok)
        {
            free(libsnd.cd.data);
            libsnd = saved;
            if (libsnd.pending_callback_count || libsnd.cd_end_callback)
                psx_wait_signal_service(6);
            heap = restored_heap;
            heap_failed = bindings.heap_failed;
            reverb_control = bindings.reverb;
            reverb_incomplete = bindings.reverb_incomplete;
            reverb_work_check = NULL;
            reverb_work_context = NULL;
            transfer_failed = bindings.transfer_failed;
            transfer_byte_address = bindings.transfer_byte_address;
            transfer_deadline = bindings.transfer_deadline;
            transfer_pending = bindings.transfer_pending;
            memset(&transfer_operation, 0, sizeof(transfer_operation));
            data = NULL;
        }
        free(data);
    }
    else
    {
        ok = spu_portable_cd_header(&libsnd, &data_size, &audio_offset) && spu_portable_bindings_pack(&bindings);
        if (ok)
        {
            saved = libsnd;
            saved.cd.data = NULL;
            saved.cd.stream.file_data = NULL;
            saved.cd.stream.audio_data = NULL;
            ok = spu_state_io(file, 0) && spu_state_block(file, &saved, sizeof(saved), 0) && fwrite(&data_size, sizeof(data_size), 1, file) == 1 && fwrite(&audio_offset, sizeof(audio_offset), 1, file) == 1 && (!data_size || fwrite(libsnd.cd.data, 1, data_size, file) == data_size) && spu_state_block(file, &bindings, sizeof(bindings), 0);
        }
    }
    if (ok)
        ok = cd_checkpoint_io(file, load);
    xport_audio_unlock();
    return ok;
}

#if defined(LOCKSTEP_DEBUG)
int spu_lockstep_portable_selftest(void)
{
    uint8 wav[68] = {0};
    uint8 *data;
    IMA_ADPCM_STREAM expected_stream;
    SpuHeapBinding heap_binding = {0};
    sint16 first[6];
    sint16 expected[12];
    sint16 actual[12];
    FILE *file;
    const char *export_path = getenv("XPORT_LOCKSTEP_CD_EXPORT_PATH");
    const char *import_path = getenv("XPORT_LOCKSTEP_CD_IMPORT_PATH");
    int ok = 0;
    if (!cd_checkpoint_selftest() || !cd_callback_selftest())
        return 0;
    if (import_path && *import_path)
    {
        file = xport_fopen(import_path, "rb");
        if (!file)
            return 0;
        ok = spu_lockstep_portable_io(file, 1) && fread(expected, 1, sizeof(expected), file) == sizeof(expected) && fgetc(file) == EOF && libsnd.cd.active && libsnd.cd.data && heap.entries == xport_guest_ptr(0x80002000u, 16u) && heap.last_index == xport_guest_ptr(0x80002020u, 4u) && heap.capacity == 2u && heap.limit == 1u && libsnd.cd.stream.decoded_frames == 3u && ima_decode(&libsnd.cd.stream, actual, 6u) == 6u && memcmp(expected, actual, sizeof(expected)) == 0;
        xport_audio_lock();
        free(libsnd.cd.data);
        memset(&libsnd.cd, 0, sizeof(libsnd.cd));
        xport_audio_unlock();
        fclose(file);
        return ok;
    }
    memcpy(wav, "RIFF", 4);
    wav[4] = 60;
    memcpy(wav + 8, "WAVEfmt ", 8);
    wav[16] = 20;
    wav[20] = 0x11;
    wav[22] = 1;
    wav[24] = 0x44;
    wav[25] = 0xac;
    wav[32] = 8;
    wav[34] = 4;
    wav[36] = 2;
    wav[38] = 9;
    memcpy(wav + 40, "fact", 4);
    wav[44] = 4;
    wav[48] = 9;
    memcpy(wav + 52, "data", 4);
    wav[56] = 8;
    wav[64] = 0x11;
    wav[65] = 0x22;
    wav[66] = 0x33;
    wav[67] = 0x44;
    data = (uint8 *)malloc(sizeof(wav));
    file = export_path && *export_path ? xport_fopen(export_path, "wb") : tmpfile();
    if (!data || !file)
    {
        free(data);
        if (file)
            fclose(file);
        return 0;
    }
    memcpy(data, wav, sizeof(wav));
    heap_binding.entries = (uint32 *)xport_guest_ptr(0x80002000u, 16u);
    heap_binding.last_index = (uint32 *)xport_guest_ptr(0x80002020u, 4u);
    heap_binding.capacity = 2u;
    heap_binding.limit = 1u;
    if (!heap_binding.entries || !heap_binding.last_index)
    {
        free(data);
        fclose(file);
        return 0;
    }
    *heap_binding.last_index = 0u;
    if (!spu_bind_heap(&heap_binding))
    {
        free(data);
        fclose(file);
        return 0;
    }
    xport_audio_lock();
    free(libsnd.cd.data);
    memset(&libsnd.cd, 0, sizeof(libsnd.cd));
    libsnd.cd.data = data;
    libsnd.cd.active = 1;
    ok = ima_open(&libsnd.cd.stream, data, sizeof(wav)) && ima_decode(&libsnd.cd.stream, first, 3u) == 3u;
    xport_audio_unlock();
    if (ok)
        ok = spu_lockstep_portable_io(file, 0);
    if (ok)
    {
        expected_stream = libsnd.cd.stream;
        ok = ima_decode(&expected_stream, expected, 6u) == 6u;
    }
    if (ok && export_path && *export_path)
        ok = fwrite(expected, 1, sizeof(expected), file) == sizeof(expected);
    xport_audio_lock();
    free(libsnd.cd.data);
    memset(&libsnd.cd, 0, sizeof(libsnd.cd));
    xport_audio_unlock();
    spu_bind_heap(NULL);
    if (export_path && *export_path)
    {
        if (fclose(file))
            ok = 0;
        return ok;
    }
    if (ok)
        ok = fseek(file, 0, SEEK_SET) == 0 && spu_lockstep_portable_io(file, 1) && libsnd.cd.active && libsnd.cd.data && heap.entries == heap_binding.entries && heap.last_index == heap_binding.last_index && heap.capacity == 2u && heap.limit == 1u && libsnd.cd.stream.decoded_frames == 3u && ima_decode(&libsnd.cd.stream, actual, 6u) == 6u && memcmp(expected, actual, sizeof(expected)) == 0;
    xport_audio_lock();
    free(libsnd.cd.data);
    memset(&libsnd.cd, 0, sizeof(libsnd.cd));
    xport_audio_unlock();
    spu_bind_heap(NULL);
    fclose(file);
    return ok;
}
#endif

static FILE *cd_image;
static uint32 cd_image_sectors;
static uint32 cd_sector_cursor;
static uint32 cd_sector_offset;
static uint32 cd_image_byte_offset;
static uint32 cd_image_sector_bytes = 2352u;
static uint8 cd_data_mode;
static sint32 cd_read_error;
static uint8 cd_sector_buffer[2352];
static uint8 cd_data_fifo[2340];
static uint32 cd_data_offset;
static uint32 cd_data_length;
static void (*cd_command_service)(uint8, uint32, uint32);

void cd_bind_command_native(void (*command)(uint8, uint32, uint32))
{
    cd_command_service = command;
}

static void cd_notify_command(uint8 command)
{
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

    if (bytes <= 0 || bytes > UINT32_MAX || fseek(file, 0, SEEK_SET) != 0 || fread(header, 1u, sizeof(header), file) != sizeof(header))
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
            if (fseek(file, (long)pos, SEEK_SET) != 0 || fread(chunk, 1u, 8u, file) != 8u)
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

static sint32 cd_fetch_sector(uint32 sector)
{
    uint64 offset = cd_image_byte_offset + ((uint64)sector + cd_sector_offset) * cd_image_sector_bytes;
    if (!cd_image || (uint64)sector + cd_sector_offset >= cd_image_sectors)
        return 0;
    if (offset > 0x7fffffffu || fseek(cd_image, (long)offset, SEEK_SET) != 0)
        return 0;

    if (cd_image_sector_bytes == 2048u)
    {
        CdlLOC pos;
        memset(cd_sector_buffer, 0, sizeof(cd_sector_buffer));
        memset(cd_sector_buffer + 1, 0xFF, 10u);
        CdIntToPos((sint32)sector, &pos);
        memcpy(cd_sector_buffer + 12, &pos, 3u);
        cd_sector_buffer[15] = 2u;
        cd_sector_buffer[18] = cd_sector_buffer[22] = 8u;
        if (fread(cd_sector_buffer + 24, 1u, 2048u, cd_image) != 2048u)
            return 0;
    }
    else if (fread(cd_sector_buffer, 1u, sizeof(cd_sector_buffer), cd_image) != sizeof(cd_sector_buffer))
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
    if (sector < 0 || !cd_image || (uint64)(uint32)sector + cd_sector_offset >= cd_image_sectors)
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

typedef struct CD_CHECKPOINT
{
    CD_XA_STATE xa;
    uint32 sector_cursor, data_offset, data_length;
    sint32 read_error, command_completion;
    uint8 data_mode, command_response[8];
    uint8 sector_buffer[2352], data_fifo[2340];
} CD_CHECKPOINT;

static size_t cd_checkpoint_size(void)
{
    return sizeof(CD_CHECKPOINT);
}

static void cd_checkpoint_capture(void *data)
{
    CD_CHECKPOINT *saved = (CD_CHECKPOINT *)data;
    memset(saved, 0, sizeof(*saved));
    saved->xa = cd_xa;
    saved->sector_cursor = cd_sector_cursor;
    saved->data_offset = cd_data_offset;
    saved->data_length = cd_data_length;
    saved->read_error = cd_read_error;
    saved->command_completion = cd_command_completion;
    saved->data_mode = cd_data_mode;
    memcpy(saved->command_response, cd_command_response, sizeof(cd_command_response));
    memcpy(saved->sector_buffer, cd_sector_buffer, sizeof(cd_sector_buffer));
    memcpy(saved->data_fifo, cd_data_fifo, sizeof(cd_data_fifo));
}

static void cd_checkpoint_restore(const void *data)
{
    const CD_CHECKPOINT *saved = (const CD_CHECKPOINT *)data;
    cd_xa = saved->xa;
    cd_sector_cursor = saved->sector_cursor;
    cd_data_offset = saved->data_offset;
    cd_data_length = saved->data_length;
    cd_read_error = saved->read_error;
    cd_command_completion = saved->command_completion;
    cd_data_mode = saved->data_mode;
    memcpy(cd_command_response, saved->command_response, sizeof(cd_command_response));
    memcpy(cd_sector_buffer, saved->sector_buffer, sizeof(cd_sector_buffer));
    memcpy(cd_data_fifo, saved->data_fifo, sizeof(cd_data_fifo));
}

static int cd_checkpoint_io(FILE *file, int load)
{
    CD_CHECKPOINT *saved = (CD_CHECKPOINT *)malloc(sizeof(*saved));
    int ok;
    if (!saved)
        return 0;
    if (load)
    {
        ok = fread(saved, sizeof(*saved), 1, file) == 1;
        if (ok)
            cd_checkpoint_restore(saved);
    }
    else
    {
        cd_checkpoint_capture(saved);
        ok = fwrite(saved, sizeof(*saved), 1, file) == 1;
    }
    free(saved);
    return ok;
}

#if defined(LOCKSTEP_DEBUG)
static uint32 cd_callback_test_count;
static uint32 cd_callback_test_failed;

static void cd_callback_test(uint8 status, uint8 *response)
{
    if (cd_callback_test_count < 2)
    {
        if (status != CdlComplete || !response || response[0] != CdlComplete)
            cd_callback_test_failed = 1;
    }
    else if (cd_callback_test_count != 2 || status != CdlDataEnd || response)
        cd_callback_test_failed = 1;
    ++cd_callback_test_count;
}

static sint32 cd_callback_test_wait(void *context)
{
    return context != NULL;
}

static int cd_callback_selftest(void)
{
    size_t size = spu_lockstep_checkpoint_size();
    void *previous = malloc(size);
    void *queued = malloc(size);
    uint8 response;
    int ok = 0;
    if (!previous || !queued || !spu_lockstep_checkpoint_capture(previous, size))
        goto cleanup;
    cd_callback_test_count = cd_callback_test_failed = 0;
    psx_vblank_bind(cd_callback_test_wait, &response, 0);
    CdSyncCallback(cd_callback_test);
    CdReadyCallback(cd_callback_test);
    EnterCriticalSectionPSX();
    ok = CdControl(CdlMute, NULL, &response) && CdControl(CdlDemute, NULL, &response);
    response = 0xFF;
    cd_queue_callback(1, CdlDataEnd, NULL);
    cd_dispatch_ready_event();
    ok = ok && !cd_callback_test_count && VSync(-1) == 0 && !cd_callback_test_count && spu_lockstep_checkpoint_capture(queued, size);
    ExitCriticalSection();
    ok = ok && cd_callback_test_count == 3 && !cd_callback_test_failed;
    if (ok)
    {
        cd_callback_test_count = 0;
        ok = spu_lockstep_checkpoint_restore(queued, size) && !cd_callback_test_count;
        psx_sync_callbacks();
        ok = ok && cd_callback_test_count == 3 && !cd_callback_test_failed;
    }
    if (ok)
    {
        cd_callback_test_count = 0;
        ok = CdControl(CdlMute, NULL, &response) && cd_callback_test_count == 1 && CdControl(CdlDemute, NULL, &response) && cd_callback_test_count == 2;
        cd_queue_callback(1, CdlDataEnd, NULL);
        ok = ok && cd_callback_test_count == 3 && !cd_callback_test_failed;
    }
    psx_vblank_bind(NULL, NULL, 0);
    if (!spu_lockstep_checkpoint_restore(previous, size))
        ok = 0;
cleanup:
    free(queued);
    free(previous);
    return ok;
}

static int cd_checkpoint_selftest(void)
{
    CD_CHECKPOINT *previous = (CD_CHECKPOINT *)malloc(cd_checkpoint_size());
    size_t size = spu_lockstep_checkpoint_size();
    void *saved = malloc(size);
    uint8 bytes[64], changed[64] = {0};
    uint32 actual[2], index;
    FILE *file = tmpfile();
    int ok = 0;
    if (!previous || !saved || !file)
        goto cleanup;
    xport_audio_lock();
    cd_checkpoint_capture(previous);
    xport_audio_unlock();
    for (index = 0; index < sizeof(bytes); ++index)
        bytes[index] = (uint8)index;
    ok = cd_restore_fifo_native(bytes, sizeof(bytes), 0x20, 1234) && CdGetSector(actual, 2);
    if (ok)
    {
        cd_xa.credit = 37;
        ok = spu_lockstep_checkpoint_capture(saved, size) && cd_checkpoint_io(file, 0);
    }
    if (ok)
    {
        cd_restore_fifo_native(changed, sizeof(changed), 0, 9999);
        cd_xa.credit = 0;
        ok = spu_lockstep_checkpoint_restore(saved, size) && CdGetSector(actual, 2) && !memcmp(actual, bytes + 8, sizeof(actual)) && cd_sector_cursor == 1234 && cd_data_mode == 0x20 && cd_xa.credit == 37;
    }
    if (ok)
    {
        cd_restore_fifo_native(changed, sizeof(changed), 0, 9999);
        cd_xa.credit = 0;
        ok = fseek(file, 0, SEEK_SET) == 0 && cd_checkpoint_io(file, 1) && CdGetSector(actual, 2) && !memcmp(actual, bytes + 8, sizeof(actual)) && cd_sector_cursor == 1234 && cd_data_mode == 0x20 && cd_xa.credit == 37;
    }
    xport_audio_lock();
    cd_checkpoint_restore(previous);
    xport_audio_unlock();
cleanup:
    if (file)
        fclose(file);
    free(saved);
    free(previous);
    return ok;
}
#endif

static void cd_xa_stop_locked(void)
{
    sint32 consumer = cd_xa.consumer;
    memset(&cd_xa, 0, sizeof(cd_xa));
    cd_xa.consumer = consumer;
    xa_reset(&cd_xa.decoder);
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
    if (count <= 0 || !destination || !cd_image)
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
        if (!CdGetSector_locked((uint8 *)destination + (uint32)index * bytes, (sint32)(bytes / 4u)))
        {
            cd_read_error = 1;
            return 0;
        }
    return 1;
}

sint32 CdRead(sint32 count, uint32 *destination, sint32 mode)
{
    sint32 value;
    xport_audio_lock();
    value = CdRead_locked(count, destination, mode);
    xport_audio_unlock();
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
    cd_command_completion = CdlComplete;
    memset(cd_command_response, 0, sizeof(cd_command_response));
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
    if (!cd_image)
        return 0x10;
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

    if (!cd_image)
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
    xport_audio_lock();
    value = CdInit_locked();
    xport_audio_unlock();
    if (value)
        cd_initialize_audio_device();
    return value;
}

static sint32 cd_reset_native_locked(sint32 mode)
{
    cd_xa_stop_locked();

    (void)mode;
    cd_read_error = 0;
    return cd_image != NULL;
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
        abort();
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
    return (uint8)((cd_image ? 2u : 0u) | (cd_xa.reading ? 0x20u : 0u) | (cd_xa.error ? 1u : 0u));
}

sint32 CdControlBPSX(uint8 command, uint8 *parameter, uint8 *result)
{
    sint32 ok = 0;
    if (command != 1u && command != 2u && command != 6u && command != 14u && command != 16u && command != 17 && command != 21u && command != 22u && command != 27u)
        return CdControl(command, parameter, result);
    xport_audio_lock();
    if (command == 1u)
    {
        ok = cd_image != NULL;
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
        ok = cd_image != NULL;
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
        if (parameter && cd_image)
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
            ok = cd_image != NULL;
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

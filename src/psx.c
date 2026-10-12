#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "psx.h"

#if defined(XPORT_MEMORY_DIAGNOSTICS)
    #undef w_u8
    #undef w_u16
    #undef w_u32
    #undef w_masked_u32
#endif

uint32 GsSortOtPSX(uint32 source_ot, uint32 destination_ot)
{
    uint32 current = r_u32(source_ot + 4u);
    uint32 source_offset = r_u32(source_ot + 12u);
    uint32 destination_offset = r_u32(destination_ot + 8u);
    uint32 destination_base = r_u32(destination_ot + 4u);
    uint32 previous = current;
    uint32 tail_link = current;
    uint32 link = r_u32(tail_link) & 0xFFFFFFu;
    uint32 slot = destination_base + 4u * (source_offset - destination_offset);
    while (link != 0xFFFFFFu)
    {
        tail_link = previous;
        link = r_u32(current);
        previous = current;
        current = link & 0xFFFFFFu;
        link = r_u32(current) & 0xFFFFFFu;
    }
    /* Preserve packet lengths and reload aliases after each write */
    link = r_u32(tail_link) & 0xFF000000u;
    link |= r_u32(slot) & 0xFFFFFFu;
    w_u32(tail_link, link);
    link = r_u32(slot) & 0xFF000000u;
    link |= r_u32(source_ot + 16u) & 0xFFFFFFu;
    w_u32(slot, link);
    return destination_ot;
}

GDB_CALL uint8 r_u8(uint32 address)
{
    return (uint8)xport_memory_read(address, 1u);
}

GDB_CALL uint16 r_u16(uint32 address)
{
    return (uint16)xport_memory_read(address, 2u);
}

GDB_CALL uint32 r_u32(uint32 address)
{
    return xport_memory_read(address, 4u);
}

GDB_CALL sint8 r_s8(uint32 address)
{
    uint8 value = r_u8(address);
    return value <= INT8_MAX ? (sint8)value : (sint8)(-1 - (sint32)(uint8)~value);
}

GDB_CALL sint16 r_s16(uint32 address)
{
    uint16 value = r_u16(address);
    return value <= INT16_MAX ? (sint16)value : (sint16)(-1 - (sint32)(uint16)~value);
}

GDB_CALL sint32 r_s32(uint32 address)
{
    uint32 value = r_u32(address);
    return value <= INT32_MAX ? (sint32)value : -1 - (sint32)~value;
}

GDB_CALL sint32 w_u8(uint32 address, uint32 value)
{
    return xport_memory_write(address, 1u, value, 1u);
}

GDB_CALL sint32 w_u16(uint32 address, uint32 value)
{
    return xport_memory_write(address, 2u, value, 3u);
}

GDB_CALL sint32 w_u32(uint32 address, uint32 value)
{
    return xport_memory_write(address, 4u, value, 15u);
}

GDB_CALL sint32 w_masked_u32(uint32 address, uint32 value, uint32 byte_mask)
{
    return xport_memory_write(address, 4u, value, byte_mask);
}

#if defined(XPORT_MEMORY_DIAGNOSTICS)
    #define w_u8(address, value) w_u8_at((address), (value), __FILE__, __LINE__, __func__, NULL)
    #define w_u16(address, value) w_u16_at((address), (value), __FILE__, __LINE__, __func__, NULL)
    #define w_u32(address, value) w_u32_at((address), (value), __FILE__, __LINE__, __func__, NULL)
    #define w_masked_u32(address, value, mask) w_masked_u32_at((address), (value), (mask), __FILE__, __LINE__, __func__, NULL)
#endif

#include "xport_trace.h"
#include "psx_gpu.h"
#include "psx_spu.h"
#include "xport.h"

/* Missing guest GPU driver remains a fail-fast boundary */

/* Minimal native event registry */
typedef struct
{
    uint32 event_class;
    uint32 specification;
    uint32 mode;
    uint32 callback;
    uint8 enabled, allocated, pending;
    PsxEventCallback native_callback;
} BiosNativeEvent;

static BiosNativeEvent bios_native_events[32];
static PsxIrqState native_root_irq;
static uint32 native_root_control_slot, native_root_mask_table;
static uint64 native_root_last_us, native_root_tick_fraction;
static uint32 native_root_polling, native_root_clock_started;
static sint32 native_video_mode;
void psx_root_counter_poll(void);
static void mc_poll(void);
static void bios_native_irq_dispatch(void);

static uint32 bios_interrupts_enabled = 1u;
static uint32 bios_vblank_pending;
static uint32 bios_vsync_callback;
static uint32 bios_callbacks_running = 1u;
static PsxGuestCallback bios_native_dispatch;
static void *bios_native_dispatch_context;
static PsxNativeExec bios_native_exec;
static void *bios_native_exec_context;
static void bios_native_pad_poll(void);
static uint32 bios_card_copy_source, bios_card_first_source, bios_card_second_source;
static uint32 bios_card_first_destination, bios_card_second_destination;
static uint32 bios_card_patches_bound;
static uint32 bios_card_pending_ports;
static void (*bios_card_irq_initializer)(void);
static void bios_native_card_complete(void);
static sint32 bios_native_card_medium_present(uint32 port);

void psx_bios_bind_card_irq_initializer(void (*initializer)(void))
{
    bios_card_irq_initializer = initializer;
}

void psx_bios_bind_card_patches(uint32 copy_source, uint32 first_source, uint32 second_source, uint32 first_destination, uint32 second_destination)
{
    bios_card_copy_source = copy_source;
    bios_card_first_source = first_source;
    bios_card_second_source = second_source;
    bios_card_first_destination = first_destination;
    bios_card_second_destination = second_destination;
    bios_card_patches_bound = 1u;
}

void psx_bios_bind_guest_callback_service(PsxGuestCallback dispatch, void *context)
{
    bios_native_dispatch = dispatch;
    bios_native_dispatch_context = context;
}

void psx_bios_bind_native_exec(PsxNativeExec dispatch, void *context)
{
    bios_native_exec = dispatch;
    bios_native_exec_context = context;
}

static uint32 bios_events_base, bios_events_count;
static sint32 bios_events_failure;
static PsxBiosEventService bios_event_service;
static void *bios_event_context;
static sint32 bios_event_servicing;

void psx_bios_bind_event_service(PsxBiosEventService service, void *context)
{
    bios_event_service = service;
    bios_event_context = context;
    bios_event_servicing = 0;
}

static PsxBiosCardInfoLayout bios_card;
static sint32 bios_card_bound, bios_card_failure;
static uint32 bios_card_read_handler, bios_card_new_flag;
static sint32 bios_card_read_bound;

static sint32 bios_card_address(uint32 address, uint32 size, uint32 alignment)
{
    uint32 segment = address & 0xe0000000u;
    uint32 physical = address & 0x1fffffffu;
    return (!segment || segment == 0x80000000u || segment == 0xa0000000u) && !(physical & (alignment - 1u)) && physical <= PSX_DRAM_SIZE - size;
}

sint32 psx_bios_bind_card_info(const PsxBiosCardInfoLayout *layout)
{
    bios_card_bound = 0;
    bios_card_read_bound = 0;
    if (!layout || !bios_card_address(layout->requested, 8u, 4u) || !bios_card_address(layout->status, 2u, 1u) || !bios_card_address(layout->step, 4u, 4u) || !bios_card_address(layout->retry, 4u, 4u) || !bios_card_address(layout->channel, 8u, 4u) || !bios_card_address(layout->buffer, 8u, 4u) || !bios_card_address(layout->handler, 8u, 4u) || !bios_card_address(layout->sector, 8u, 4u) || !bios_card_address(layout->info_handler, 4u, 4u))
    {
        bios_card_failure = 1;
        return 0;
    }
    bios_card = *layout;
    bios_card_bound = 1;
    bios_card_failure = 0;
    return 1;
}

sint32 psx_bios_card_info_failed(void)
{
    return bios_card_failure;
}

static void bios_card_store(uint32 address, uint32 value)
{
    memcpy(DRAM + (address & 0x1fffffffu), &value, sizeof(value));
}

sint32 _card_info(sint32 channel)
{
    sint32 port = channel / 16;
    uint32 offset;
    uint8 *status;
    if (port < 0 || port > 1)
    {
        bios_card_failure = 1;
        return 0;
    }
    offset = (uint32)port * 4u;
    w_u32(0xA0009F20u + offset, 1u);
    if (!bios_card_bound)
    {
        uint32 flag = 0x7568u + (uint32)port;
        if (!(r_u8(flag) & 1u))
        {
            w_u32(0xA0009F20u + offset, 0u);
            return 0;
        }
        w_u32(0x7514u, 0u);
        w_u32(0x74A4u, 0u);
        w_u32(0x7500u + offset, (uint32)channel);
        w_u32(0x7550u + offset, 0u);
        w_u32(0x7528u + offset, 0x5B64u);
        w_u32(0x7508u + offset, 0u);
        w_u8(flag, 8u);
        bios_card_pending_ports |= 1u << port;
        return 1;
    }
    status = DRAM + (bios_card.status & 0x1fffffffu) + port;
    bios_card_store(bios_card.requested + offset, 1u);
    if (!(*status & 1u))
    {
        bios_card_store(bios_card.requested + offset, 0u);
        return 0;
    }
    bios_card_store(bios_card.step, 0u);
    bios_card_store(bios_card.retry, 0u);
    bios_card_store(bios_card.channel + offset, (uint32)channel);
    bios_card_store(bios_card.buffer + offset, 0u);
    bios_card_store(bios_card.handler + offset, bios_card.info_handler);
    bios_card_store(bios_card.sector + offset, 0u);
    *status = 8u;
    bios_card_pending_ports |= 1u << port;
    return 1;
}

sint32 psx_bios_bind_card_read(uint32 read_handler, uint32 new_card_flag)
{
    bios_card_read_bound = 0;
    if (!bios_card_bound || !bios_card_address(read_handler, 4u, 4u) || !bios_card_address(new_card_flag, 4u, 4u))
    {
        bios_card_failure = 1;
        return 0;
    }
    bios_card_read_handler = read_handler;
    bios_card_new_flag = new_card_flag;
    bios_card_read_bound = 1;
    return 1;
}

void _new_card(void)
{
    w_u32(0x74C0u, 1u);
    if (!bios_card_read_bound)
        return;
    bios_card_store(bios_card_new_flag, 1u);
}

sint32 _card_read(sint32 channel, sint32 sector, uint8 *buffer)
{
    sint32 port = channel / 16;
    uint32 offset, address;
    uintptr_t pointer = (uintptr_t)buffer, ram = (uintptr_t)DRAM;
    uint8 *status;
    if (!bios_card_read_bound || port < 0 || port > 1)
    {
        bios_card_failure = 1;
        return 0;
    }
    offset = (uint32)port * 4u;
    status = DRAM + (bios_card.status & 0x1fffffffu) + port;
    if (!(*status & 1u) || sector < 0 || sector > 0x400)
        return 0;
    if (pointer >= ram && pointer - ram <= PSX_DRAM_SIZE - 128u)
        address = 0x80000000u | (uint32)(pointer - ram);
    else if (pointer >= 0x00200000u && pointer < 0x80000000u && xport_memory_readable(buffer, 128u))
        address = (uint32)pointer;
    else
    {
        bios_card_failure = 1;
        return 0;
    }
    bios_card_store(bios_card.step, 0u);
    bios_card_store(bios_card.retry, 0u);
    bios_card_store(bios_card.channel + offset, (uint32)channel);
    bios_card_store(bios_card.buffer + offset, address);
    bios_card_store(bios_card.handler + offset, bios_card_read_handler);
    bios_card_store(bios_card.sector + offset, (uint32)sector);
    *status = 2u;
    return 1;
}

sint32 psx_bios_bind_events(uint32 address, uint32 bytes)
{
    uint32 physical = address & 0x1fffffffu;
    uint32 segment = address & 0xe0000000u;
    if ((segment && segment != 0x80000000u && segment != 0xa0000000u) || (physical & 3u) || !bytes || bytes % 28u || bytes > PSX_DRAM_SIZE || physical > PSX_DRAM_SIZE - bytes)
        return 0;
    bios_events_base = physical;
    bios_events_count = bytes / 28u;
    bios_events_failure = 0;
    psx_bios_bind_event_service(NULL, NULL);
    return 1;
}

sint32 psx_bios_events_failed(void)
{
    return bios_events_failure;
}

uint32 SysDeqIntRP(uint32 priority, uint32 node)
{
    uint32 link = r_u32(0xA0000100u) + (priority << 3);
    uint32 current = r_u32(link);
    uint32 next;
    while (current)
    {
        if (current == node)
        {
            next = r_u32(current);
            w_u32(link, next);
            return current;
        }
        next = r_u32(current);
        if (!next)
            return 0u;
        link = current;
        current = next;
    }
    return 0u;
}

sint32 CloseEvent(sint32 event_handle)
{
    uint32 handle = (uint32)event_handle;
    uint32 index = handle & 0xffffu;
    uint32 base;

    /* Native handles are untagged; BIOS handles retain their guest table path */
    if ((handle & 0xffff0000u) == 0u)
    {
        if (handle < 1u || handle > 32u || !bios_native_events[handle - 1u].allocated)
            return 0;
        memset(&bios_native_events[handle - 1u], 0, sizeof(BiosNativeEvent));
        return 1;
    }
    base = r_u32(0xA0000120u);
    w_u32(base + index * 28u + 4u, 0u);
    return 1;
}

sint32 TestEvent(sint32 event_handle)
{
    uint32 handle = (uint32)event_handle;
    uint32 index = handle & 0xffffu;
    uint32 status, enabled = 0x2000u;
    uint8 *record;
    bios_native_card_complete();
    if ((handle & 0xFFFF0000u) == 0u)
    {
        BiosNativeEvent *event;
        if (handle < 1u || handle > 32u)
            return 0;
        event = &bios_native_events[handle - 1u];
        if (!event->allocated || !event->enabled || !event->pending)
            return 0;
        event->pending = 0u;
        return 1;
    }
    if (!bios_events_count || index >= bios_events_count)
    {
        bios_events_failure = 1;
        return 0;
    }
    if (bios_event_service)
    {
        if (bios_event_servicing)
        {
            bios_events_failure = 1;
            return 0;
        }
        bios_event_servicing = 1;
        if (!bios_event_service(bios_event_context))
            bios_events_failure = 1;
        bios_event_servicing = 0;
        if (bios_events_failure)
            return 0;
    }
    record = DRAM + bios_events_base + index * 28u;
    memcpy(&status, record + 4u, sizeof(status));
    if (status != 0x4000u)
        return 0;
    memcpy(record + 4u, &enabled, sizeof(enabled));
    return 1;
}

static void bios_native_card_complete(void)
{
    uint32 port, pending = bios_card_pending_ports;
    bios_card_pending_ports = 0u;
    for (port = 0u; port < 2u; ++port)
    {
        uint32 requested;
        if (!(pending & (1u << port)))
            continue;
        if (bios_native_card_medium_present(port))
        {
#ifndef NDEBUG
            fprintf(stderr, "PSX: pending mounted-card BIOS sector completion and event transport\n");
#endif
            abort();
        }
        requested = r_u32(0xA0009F20u + 4u * port);
        w_u8(0x7568u + port, 0x21u);
        w_u32(0x751Cu, port);
        w_u32(0x74C0u, 0u);
        if (bios_card_bound)
        {
            w_u8(bios_card.status + port, 0x21u);
            w_u32(bios_card.requested + port * 4u, 0u);
        }
        /* BIOS A0:A8 forwards an I/O error to the pending filesystem request */
        w_u32(0xA000B9D4u, 1u);
        if (requested)
        {
            w_u32(0xA0009F20u + 4u * port, 0u);
            w_u32(0xA0009F28u + 4u * port, 0u);
            w_u32(0xA0009F30u + 4u * port, 0u);
            DeliverEvent(0xF4000001u, 0x8000u);
        }
        /* No native card medium is mounted; complete with the BIOS I/O error */
        DeliverEvent(0xF0000011u, 0x8000u);
    }
}

uint32 psx_bios_card_read_guest(uint32 channel, uint32 sector, uint32 buffer)
{
    sint32 port = (sint32)channel / 16;
    uint32 offset;
    if (port < 0 || port > 1 || (sint32)sector < 0 || (sint32)sector > 0x400)
        return 0u;
    if (!(r_u8(0x7568u + (uint32)port) & 1u))
        return 0u;
    offset = (uint32)port * 4u;
    w_u32(0x7514u, 0u);
    w_u32(0x74A4u, 0u);
    w_u32(0x7500u + offset, channel);
    w_u32(0x7550u + offset, buffer);
    w_u32(0x7528u + offset, 0x5688u);
    w_u32(0x7508u + offset, sector);
    w_u8(0x7568u + (uint32)port, 2u);
    bios_card_pending_ports |= 1u << port;
    return 1u;
}

void psx_bios_card_poll(void)
{
    bios_native_card_complete();
}

void UnDeliverEvent(uint32 event_class, uint32 spec)
{
    uint32 index;
    for (index = 0u; index < 32u; ++index)
    {
        BiosNativeEvent *event = &bios_native_events[index];
        if (event->allocated && event->enabled && event->pending && event->event_class == event_class && event->specification == spec && event->mode == 0x2000u)
            event->pending = 0u;
    }
    for (index = 0u; index < bios_events_count; ++index)
    {
        uint32 record = bios_events_base + index * 28u;
        if (r_u32(record + 4u) == 0x4000u && r_u32(record) == event_class && r_u32(record + 8u) == spec && r_u32(record + 12u) == 0x2000u)
            w_u32(record + 4u, 0x2000u);
    }
}

uint32 psx_bios_card_write_guest(uint32 channel, uint32 sector, uint32 buffer)
{
    sint32 port = (sint32)channel / 16;
    uint32 offset;
    if (port < 0 || port > 1 || sector > 0x400u || !(r_u8(0x7568u + (uint32)port) & 1u))
        return 0u;
    offset = (uint32)port * 4u;
    w_u32(0x7514u, 0u);
    w_u32(0x74A4u, 0u);
    w_u32(0x7500u + offset, channel);
    w_u32(0x7550u + offset, buffer);
    w_u32(0x7528u + offset, 0x51F4u);
    w_u32(0x7508u + offset, sector);
    w_u8(0x7568u + (uint32)port, 4u);
    bios_card_pending_ports |= 1u << port;
    return 1u;
}

uint32 psx_bios_card_load_guest(uint32 channel)
{
    sint32 port = (sint32)channel / 16;
    uint32 offset;
    if (port < 0 || port > 1)
        return 0u;
    offset = (uint32)port * 4u;
    w_u32(0xA0009F20u + offset, 4u);
    if (!(r_u8(0x7568u + (uint32)port) & 1u))
    {
        w_u32(0xA0009F20u + offset, 0u);
        return 0u;
    }
    w_u32(0x7514u, 0u);
    w_u32(0x74A4u, 0u);
    w_u32(0x7500u + offset, channel);
    w_u32(0x7550u + offset, 0xA000BE48u + 128u * (uint32)port);
    w_u32(0x7528u + offset, 0x4F90u);
    w_u32(0x7508u + offset, 0u);
    w_u8(0x7568u + (uint32)port, 2u);
    w_u32(0xA0009F28u + offset, 1u);
    bios_card_pending_ports |= 1u << port;
    return 1u;
}

void DeliverEvent(uint32 event_class, uint32 spec)
{
    uint32 index;
    for (index = 0; index < 32u; ++index)
    {
        BiosNativeEvent *event = &bios_native_events[index];
        if (!event->allocated || !event->enabled || event->event_class != event_class || event->specification != spec)
            continue;
        if (event->mode == 0x2000u)
            event->pending = 1u;
        else if (event->mode == 0x1000u)
        {
            if (event->native_callback)
                (void)event->native_callback();
            else if (event->callback && (!bios_native_dispatch || !bios_native_dispatch(bios_native_dispatch_context, event->callback)))
            {
                bios_events_failure = 1;
                return;
            }
        }
    }
    if (!bios_events_count)
    {
        return;
    }
    for (index = 0; index < bios_events_count; ++index)
    {
        uint32 fields[5];
        uint8 *record = DRAM + bios_events_base + index * 28u;
        memcpy(fields, record, sizeof(fields));
        if (fields[1] != 0x2000u || fields[0] != event_class || fields[2] != spec)
            continue;
        if (fields[3] == 0x2000u)
        {
            uint32 ready = 0x4000u;
            memcpy(record + 4u, &ready, sizeof(ready));
        }
        else if (fields[3] == 0x1000u && fields[4])
        {
            if (!bios_native_dispatch || !bios_native_dispatch(bios_native_dispatch_context, fields[4]))
            {
                bios_events_failure = 1;
                return;
            }
        }
    }
}

static uint32 dma_master(uint32 value)
{
    return (value & 0x8000u) || ((value & 0x800000u) && (value & 0x7f000000u));
}

static const PsxDmaState *dma_trace_owner;
static XportDeviceOperation dma_trace_operations[7];

static void dma_trace_start(const PsxDmaState *state, uint32 channel)
{
    XportDeviceOperation *operation;
    uint32 control;
    uint32 byte_count;
    uint64 words;
    if (!xport_device_trace_enabled() || !state || channel >= 7u)
        return;
    if (dma_trace_owner != state)
    {
        memset(dma_trace_operations, 0, sizeof(dma_trace_operations));
        dma_trace_owner = state;
    }
    operation = &dma_trace_operations[channel];
    if (operation->active)
        return;
    control = state->channels[channel].control;
    words = (uint64)(state->channels[channel].block & 0xffffu) * (uint64)((state->channels[channel].block >> 16) & 0xffffu);
    if (!words)
        words = state->channels[channel].block & 0xffffu;
    byte_count = words > 0x3fffffffu ? 0xffffffffu : (uint32)words * 4u;
    if (xport_device_trace_begin(operation, 0u, XPORT_DEVICE_DMA, XPORT_DEVICE_RAM_DMA, control & 1u ? XPORT_DEVICE_DIRECTION_TO_DEVICE : XPORT_DEVICE_DIRECTION_FROM_DEVICE, channel, control))
        xport_device_trace_stage(operation, XPORT_DEVICE_SUBMIT, XPORT_DEVICE_EVIDENCE_CANDIDATE, control & 1u ? XPORT_DEVICE_REGION_RAM : XPORT_DEVICE_REGION_NONE, state->channels[channel].base & 0x1fffffu, control & 1u ? XPORT_DEVICE_REGION_NONE : XPORT_DEVICE_REGION_RAM, state->channels[channel].base & 0x1fffffu, byte_count, 0xffffffffu);
}

sint32 psx_dma_trace_write(const PsxDmaState *state, uint32 channel, XportDeviceRegion source_region, uint32 source_offset, XportDeviceRegion destination_region, uint32 destination_offset, uint32 byte_count, uint32 byte_mask)
{
    if (state != dma_trace_owner || channel >= 7u || !dma_trace_operations[channel].active)
        return 0;
    return xport_device_trace_stage(&dma_trace_operations[channel], XPORT_DEVICE_WRITE, XPORT_DEVICE_EVIDENCE_EXACT, source_region, source_offset, destination_region, destination_offset, byte_count, byte_mask);
}

sint32 psx_dma_validate(const PsxDmaState *state, const PsxIrqState *irq)
{
    uint32 channel, master;
    if (!state || !psx_irq_validate(irq) || state->halt_ticks > 0x7fffffffu)
        return 0;
    master = dma_master(state->interrupt);
    if ((state->interrupt >> 31) != master || ((irq->lines >> 3) & 1u) != master)
        return 0;
    for (channel = 0; channel < 7u; ++channel)
        if (state->channels[channel].base > 0xffffffu || state->channels[channel].request > 1u)
            return 0;
    return 1;
}

static void dma_update_irq(PsxDmaState *state, PsxIrqState *irq)
{
    uint32 master = dma_master(state->interrupt);
    state->interrupt = (state->interrupt & 0x7fffffffu) | (master << 31);
    psx_irq_line(irq, 3u, master);
}

sint32 psx_dma_write_interrupt(PsxDmaState *state, PsxIrqState *irq, uint32 value)
{
    if (!psx_dma_validate(state, irq))
        return 0;
    state->interrupt = (state->interrupt & ~0x00ff807fu) | (value & 0x00ff807fu);
    state->interrupt &= ~(value & 0x7f000000u);
    dma_update_irq(state, irq);
    return 1;
}

sint32 psx_dma_complete(PsxDmaState *state, PsxIrqState *irq, uint32 channel)
{
    if (!psx_dma_validate(state, irq) || channel >= 7u || !(state->channels[channel].control & 0x01000000u))
        return 0;
    state->channels[channel].control &= ~0x01000000u;
    if ((state->interrupt & 0x800000u) && (state->interrupt & (1u << (channel + 16u))))
    {
        state->interrupt |= 1u << (channel + 24u);
        dma_update_irq(state, irq);
    }
    if (state == dma_trace_owner && dma_trace_operations[channel].active)
        xport_device_trace_complete(&dma_trace_operations[channel], 1);
    return 1;
}

sint32 psx_dma_bus_error(PsxDmaState *state, PsxIrqState *irq, uint32 channel)
{
    if (!psx_dma_validate(state, irq) || channel >= 7u)
        return 0;
    state->channels[channel].control &= ~0x01000000u;
    state->interrupt |= 0x8000u | (1u << (channel + 24u));
    dma_update_irq(state, irq);
    if (state == dma_trace_owner && dma_trace_operations[channel].active)
    {
        xport_device_trace_gap(&dma_trace_operations[channel], 1u);
        xport_device_trace_complete(&dma_trace_operations[channel], 0);
    }
    return 1;
}

static uint32 vblank_count;
static uint64 native_vblank_last_us;
static uint64 native_vblank_fraction;
static uint32 native_vblank_clock_started;
static uint32 native_vblank_polling;

sint32 psx_dma_ready(const PsxDmaState *state, uint32 channel, uint32 halted, uint32 ignore_halt)
{
    uint32 control;
    if (!state || channel >= 7u || halted > 1u || ignore_halt > 1u || state->channels[channel].request > 1u)
        return -1;
    control = state->channels[channel].control;
    return (state->priority & (8u << (channel * 4u))) && (control & 0x01000000u) && (!((control >> 9) & 3u) || !halted || ignore_halt) && state->channels[channel].request;
}

sint32 psx_dma_request(PsxDmaState *state, PsxIrqState *irq, uint32 channel, uint32 request, uint32 halted, PsxDmaTransfer transfer, void *context)
{
    if (!psx_dma_validate(state, irq) || channel >= 7u || request > 1u || halted > 1u || !transfer)
        return 0;
    if (state->channels[channel].request == request)
        return 1;
    state->channels[channel].request = request;
    if (psx_dma_ready(state, channel, halted, 0) == 1)
    {
        dma_trace_start(state, channel);
        sint32 result = transfer(context, channel);
        if (result < 0 || result > 1)
            return -1;
    }
    return 1;
}

sint32 psx_dma_write(PsxDmaState *state, PsxIrqState *irq, uint32 offset, uint32 value, uint32 halted, PsxDmaTransfer transfer, PsxDmaHalt halt, void *context)
{
    uint32 channel = offset >> 4, index;
    if (!psx_dma_validate(state, irq) || halted > 1u || !transfer || !halt)
        return 0;
    if (offset == 0x74u)
        return psx_dma_write_interrupt(state, irq, value);
    if (offset == 0x70u)
    {
        state->priority = value;
        for (index = 0; index < 7u; ++index)
            if (psx_dma_ready(state, index, halted, 0) == 1)
            {
                dma_trace_start(state, index);
                sint32 result = transfer(context, index);
                if (result < 0 || result > 1)
                    return -1;
                if (!result)
                    break;
            }
        return 1;
    }
    if (channel >= 7u)
        return 0;
    if ((offset & 15u) == 0u)
        state->channels[channel].base = value & 0xffffffu;
    else if ((offset & 15u) == 4u)
        state->channels[channel].block = value;
    else if ((offset & 15u) == 8u)
    {
        uint32 old = state->channels[channel].control;
        uint32 mask = channel == 6u ? 0x51000000u : 0x71770703u;
        uint32 ignore_halt = !(old & 0x01000000u) && (value & 0x01000000u);
        uint32 control;
        state->channels[channel].control = (old & ~mask) | (value & mask);
        if (channel == 6u && psx_dma_request(state, irq, channel, (state->channels[channel].control >> 28) & 1u, halted, transfer, context) != 1)
            return -1;
        if (psx_dma_ready(state, channel, halted, ignore_halt) != 1)
            return 1;
        control = state->channels[channel].control;
        if (channel != 6u && !(control & 0x600u) && (control & 0x100u))
        {
            uint32 words = state->channels[channel].block & 0xffffu;
            uint32 cycles = (1u << ((control >> 20) & 7u)) * (words >> ((control >> 16) & 7u));
            if (cycles > 500u)
                cycles = 500u;
            if (words > 4u && cycles > 1u)
                return halt(context, cycles) == 1 ? 1 : -1;
        }
        {
            dma_trace_start(state, channel);
            sint32 result = transfer(context, channel);
            if (result < 0 || result > 1)
                return -1;
        }
    }
    else
        return 0;
    return 1;
}

static PsxInterruptCallback vsync_callback;

static sint32 dma_unhalt(void *context, uint64 now)
{
    PsxDmaScheduler *scheduler = (PsxDmaScheduler *)context;
    uint32 channel;
    uint64 elapsed = now - scheduler->scheduled_at;
    if (scheduler->failed || !scheduler->active || elapsed > scheduler->dma->halt_ticks || !psx_dma_validate(scheduler->dma, scheduler->irq))
        goto unhalt_failed;
    scheduler->dma->halt_ticks -= (uint32)elapsed;
    scheduler->active = 0;
    for (channel = 0; channel < 7u; ++channel)
        if (psx_dma_ready(scheduler->dma, channel, scheduler->active, 0) == 1)
        {
            sint32 result = scheduler->transfer(scheduler->context, channel);
            if (result < 0 || result > 1 || scheduler->failed)
                goto unhalt_failed;
            if (!result)
            {
                if (!scheduler->active)
                    goto unhalt_failed;
                return 1;
            }
        }
    if (scheduler->active)
        goto unhalt_failed;
    scheduler->dma->halt_ticks = 0;
    return 1;
unhalt_failed:
    scheduler->failed = 1;
    return 0;
}

sint32 psx_dma_scheduler_init(PsxDmaScheduler *scheduler, PsxClock *clock, PsxDmaState *dma, PsxIrqState *irq, uint32 event_id, PsxDmaTransfer transfer, void *context)
{
    if (!scheduler || !clock || clock->failed || clock->running || clock->advancing || event_id >= PSX_CLOCK_EVENTS || clock->events[event_id].active || !transfer || !psx_dma_validate(dma, irq) || dma->halt_ticks)
        return 0;
    memset(scheduler, 0, sizeof(*scheduler));
    scheduler->clock = clock;
    scheduler->dma = dma;
    scheduler->irq = irq;
    scheduler->event_id = event_id;
    scheduler->transfer = transfer;
    scheduler->context = context;
    return 1;
}

sint32 psx_dma_halt(PsxDmaScheduler *scheduler, uint32 ticks)
{
    uint32 total;
    if (!scheduler || scheduler->failed || !scheduler->clock || !scheduler->dma || scheduler->clock->failed || scheduler->event_id >= PSX_CLOCK_EVENTS || !ticks || ticks > 0x7fffffffu || scheduler->dma->halt_ticks > 0x7fffffffu - ticks)
        return 0;
    if (scheduler->active && (!scheduler->clock->events[scheduler->event_id].active || scheduler->clock->events[scheduler->event_id].callback != dma_unhalt || scheduler->clock->events[scheduler->event_id].context != scheduler))
    {
        scheduler->failed = 1;
        return 0;
    }
    total = scheduler->dma->halt_ticks + ticks;
    if (!scheduler->active)
    {
        uint64 now = scheduler->clock->now;
        if (now > ~(uint64)0 - total || scheduler->clock->events[scheduler->event_id].active || !psx_clock_schedule(scheduler->clock, scheduler->event_id, now + total, dma_unhalt, scheduler))
        {
            scheduler->failed = 1;
            return 0;
        }
        scheduler->scheduled_at = now;
        scheduler->active = 1;
    }
    scheduler->dma->halt_ticks = total;
    return 1;
}

static PsxVBlankWait vblank_wait;
static void *vblank_wait_context;
static sint32 vblank_wait_failed;
static uint32 *vblank_guest_counter;
static uint32 *vblank_native_guest_counter;
static uint32 *vblank_guest_callbacks;
static PsxGuestCallback vblank_guest_dispatch;
static void *vblank_guest_context;
static sint32 vblank_dispatching;
static uint32 *vsync_last_counter;
static uint32 *vsync_last_vblank;
static PsxVsyncSample vsync_sample;
static void *vsync_sample_context;
static uint32 vsync_wait_budget;
static PsxTimerState timers;

sint32 psx_video_timing_validate(const PsxVideoTiming *state)
{
    uint32 blank;
    if (!state || !psx_crtc_validate(&state->crtc) || !psx_irq_validate(&state->irq))
        return 0;
    blank = state->crtc.scanline < state->crtc.vertical_start || state->crtc.scanline >= state->crtc.vertical_end;
    return (state->irq.lines & 1u) == blank;
}

sint32 psx_video_timing_advance(PsxVideoTiming *state, uint32 cpu_ticks, PsxCrtcStep *result)
{
    PsxVideoTiming next;
    PsxCrtcStep step;
    uint64 period, before, after, end_edge, gpu_ticks, exits;
    if (!result || !psx_video_timing_validate(state))
        return 0;
    next = *state;
    if (!psx_crtc_advance(&next.crtc, cpu_ticks, &step) || step.vblanks > 1u || (step.vblanks && state->frame == ~(uint64)0))
        return 0;
    period = (uint64)state->crtc.horizontal_total * state->crtc.vertical_total;
    before = (uint64)state->crtc.scanline * state->crtc.horizontal_total + state->crtc.scanline_tick;
    gpu_ticks = ((uint64)cpu_ticks * (state->crtc.pal ? 709379u : 715909u) + state->crtc.fractional_ticks) / 451584u;
    after = before + gpu_ticks;
    end_edge = (uint64)state->crtc.vertical_start * state->crtc.horizontal_total;
    exits = (after + period - end_edge) / period - (before + period - end_edge) / period;
    if (exits)
        psx_irq_line(&next.irq, 0u, 0u);
    if (step.vblanks)
        psx_irq_line(&next.irq, 0u, 1u);
    psx_irq_line(&next.irq, 0u, step.in_vblank);
    next.frame += step.vblanks;
    *state = next;
    *result = step;
    return 1;
}

sint32 psx_irq_validate(const PsxIrqState *state)
{
    return state && !((state->status | state->mask | state->lines) & ~0x7ffu);
}

sint32 psx_irq_pending(const PsxIrqState *state)
{
    if (!psx_irq_validate(state))
        return -1;
    return (state->status & state->mask) != 0u;
}

sint32 psx_irq_line(PsxIrqState *state, uint32 irq, uint32 high)
{
    uint32 bit;
    if (!psx_irq_validate(state) || irq >= 11u || high > 1u)
        return 0;
    bit = 1u << irq;
    if (high)
    {
        if (!(state->lines & bit))
            state->status |= bit;
        state->lines |= bit;
    }
    else
        state->lines &= ~bit;
    return 1;
}

sint32 psx_irq_write(PsxIrqState *state, uint32 offset, uint32 value)
{
    if (!psx_irq_validate(state))
        return 0;
    if (offset == 0u)
        state->status &= value & 0x7ffu;
    else if (offset == 4u)
        state->mask = value & 0x7ffu;
    else
        return 0;
    return 1;
}

sint32 psx_crtc_validate(const PsxCrtcState *state)
{
    return state && state->pal <= 1u && state->dot_external <= 1u && state->horizontal_total && state->horizontal_total <= 65535u && state->vertical_total && state->vertical_total <= 65535u && state->horizontal_start < state->horizontal_end && state->horizontal_end <= state->horizontal_total && state->vertical_start < state->vertical_end && state->vertical_end < state->vertical_total && state->dot_divider && state->dot_divider <= 65535u && state->fractional_ticks < 451584u && state->fractional_dot_ticks < state->dot_divider && state->scanline < state->vertical_total && state->scanline_tick < state->horizontal_total;
}

sint32 psx_crtc_until_vblank(const PsxCrtcState *state, uint32 *cpu_ticks)
{
    uint64 lines, ticks, frequency;
    if (!cpu_ticks || !psx_crtc_validate(state))
        return 0;
    lines = state->scanline >= state->vertical_end ? state->vertical_total - state->scanline + state->vertical_end : state->vertical_end - state->scanline;
    ticks = lines * state->horizontal_total - state->scanline_tick;
    frequency = state->pal ? 709379u : 715909u;
    ticks = (ticks * 451584u + state->fractional_ticks + frequency - 1u) / frequency;
    if (ticks > 0x7fffffffu)
        return 0;
    *cpu_ticks = (uint32)ticks;
    return 1;
}

sint32 psx_crtc_advance(PsxCrtcState *state, uint32 cpu_ticks, PsxCrtcStep *result)
{
    uint64 converted, gpu_ticks, line_ticks, lines, period, old_position, new_position, edge;
    PsxCrtcState next;
    PsxCrtcStep step;
    if (!result || !psx_crtc_validate(state) || cpu_ticks > 0x7fffffffu)
        return 0;
    next = *state;
    converted = (uint64)cpu_ticks * (state->pal ? 709379u : 715909u) + state->fractional_ticks;
    gpu_ticks = converted / 451584u;
    next.fractional_ticks = (uint32)(converted % 451584u);
    line_ticks = state->scanline_tick + gpu_ticks;
    lines = line_ticks / state->horizontal_total;
    next.scanline_tick = (uint32)(line_ticks % state->horizontal_total);
    next.scanline = (uint32)((state->scanline + lines) % state->vertical_total);
    step.hblanks = (uint32)(lines + (next.scanline_tick >= state->horizontal_end) - (state->scanline_tick >= state->horizontal_end));
    period = (uint64)state->horizontal_total * state->vertical_total;
    old_position = (uint64)state->scanline * state->horizontal_total + state->scanline_tick;
    new_position = old_position + gpu_ticks;
    edge = (uint64)state->vertical_end * state->horizontal_total;
    step.vblanks = (uint32)((new_position + period - edge) / period - (old_position + period - edge) / period);
    step.dots = 0u;
    if (state->dot_external)
    {
        converted = gpu_ticks + state->fractional_dot_ticks;
        step.dots = (uint32)(converted / state->dot_divider);
        next.fractional_dot_ticks = (uint32)(converted % state->dot_divider);
    }
    step.in_hblank = next.scanline_tick < state->horizontal_start || next.scanline_tick >= state->horizontal_end;
    step.in_vblank = next.scanline < state->vertical_start || next.scanline >= state->vertical_end;
    step.line_lsb = next.scanline & 1u;
    *state = next;
    *result = step;
    return 1;
}

void psx_vblank_bind(PsxVBlankWait wait, void *context, uint32 initial_count)
{
    vblank_wait = wait;
    vblank_wait_context = context;
    vblank_count = initial_count;
    vblank_wait_failed = 0;
    native_vblank_clock_started = 0u;
    native_vblank_fraction = 0u;
}

void psx_vblank_signal(void)
{
    uint32 index;
    psx_root_counter_poll();
    if (!bios_interrupts_enabled)
    {
        bios_vblank_pending = 1u;
        return;
    }
    if (vblank_wait_failed || vblank_dispatching)
    {
        vblank_wait_failed = 1;
        return;
    }
    xport_audio_vblank(vblank_count, FIELD_RATE);
    bios_native_pad_poll();
    psx_interrupt_raise(0u);
    if (!bios_callbacks_running)
        return;
    if (vblank_guest_counter)
    {
        vblank_dispatching = 1;
        vblank_count = ++*vblank_guest_counter;
        for (index = 0; index < 8u; ++index)
        {
            uint32 address = vblank_guest_callbacks[index];
            if ((address && !vblank_guest_dispatch(vblank_guest_context, address)) || vblank_wait_failed)
            {
                vblank_wait_failed = 1;
                break;
            }
        }
        vblank_dispatching = 0;
        return;
    }
    if (vblank_native_guest_counter)
        vblank_count = ++*vblank_native_guest_counter;
    else
        ++vblank_count;
    if (bios_callbacks_running && bios_vsync_callback)
    {
        if (!bios_native_dispatch || !bios_native_dispatch(bios_native_dispatch_context, bios_vsync_callback))
        {
#ifndef NDEBUG
            fprintf(stderr, "Unsupported native VBlank callback %08X\n", bios_vsync_callback);
#endif
            abort();
        }
    }
    if (bios_callbacks_running && vsync_callback)
        vsync_callback();
    mc_poll();
}

static uint32 *psx_vblank_guest_words(uint32 address, uint32 count)
{
    uint32 segment = address & 0xe0000000u;
    uint32 physical = address & 0x1fffffffu;
    if ((segment && segment != 0x80000000u && segment != 0xa0000000u) || (physical & 3u) || physical > PSX_DRAM_SIZE - count * 4u)
        return NULL;
    return (uint32 *)(DRAM + physical);
}

sint32 psx_vblank_bind_native_counter(uint32 counter)
{
    uint32 *words = counter ? psx_vblank_guest_words(counter, 1u) : NULL;
    if (vblank_dispatching || (counter && !words))
        return 0;
    vblank_native_guest_counter = words;
    if (words)
        vblank_count = *words;
    return 1;
}

sint32 psx_vblank_bind_guest(uint32 counter, uint32 callbacks, PsxGuestCallback dispatch, void *context)
{
    uint32 *count_words, *callback_words;
    if (vblank_dispatching)
    {
        vblank_wait_failed = 1;
        return 0;
    }
    if (!dispatch)
    {
        if (counter || callbacks)
            return 0;
        vblank_guest_counter = vblank_guest_callbacks = NULL;
        vblank_guest_dispatch = NULL;
        vblank_guest_context = NULL;
        return 1;
    }
    count_words = psx_vblank_guest_words(counter, 1u);
    callback_words = psx_vblank_guest_words(callbacks, 8u);
    if (!count_words || !callback_words || ((counter & 0x1fffffffu) >= (callbacks & 0x1fffffffu) && (counter & 0x1fffffffu) < (callbacks & 0x1fffffffu) + 32u))
        return 0;
    vblank_guest_counter = count_words;
    vblank_guest_callbacks = callback_words;
    vblank_guest_dispatch = dispatch;
    vblank_guest_context = context;
    vblank_count = *count_words;
    return 1;
}

sint32 psx_vblank_failed(void)
{
    return vblank_wait_failed;
}

sint32 psx_vsync_bind_sdk(uint32 last_counter, uint32 last_vblank, PsxVsyncSample sample, void *context, uint32 wait_budget)
{
    uint32 *counter, *count;
    if (vblank_dispatching)
        return 0;
    if (!sample)
    {
        if (last_counter || last_vblank || wait_budget)
            return 0;
        vsync_last_counter = vsync_last_vblank = NULL;
        vsync_sample = NULL;
        vsync_sample_context = NULL;
        vsync_wait_budget = 0u;
        return 1;
    }
    counter = psx_vblank_guest_words(last_counter, 1u);
    count = psx_vblank_guest_words(last_vblank, 1u);
    if (!counter || !count || counter == count || !wait_budget)
        return 0;
    vsync_last_counter = counter;
    vsync_last_vblank = count;
    vsync_sample = sample;
    vsync_sample_context = context;
    vsync_wait_budget = wait_budget;
    return 1;
}

static sint32 psx_vsync_wait_target(uint32 target, uint32 *waited)
{
    while ((sint32)*vblank_guest_counter < (sint32)target)
    {
        uint32 before = *vblank_guest_counter;
        if (*waited >= vsync_wait_budget)
            return 0;
        ++*waited;
        if (!vblank_wait(vblank_wait_context) || vblank_wait_failed || *vblank_guest_counter - before != 1u)
            return 0;
    }
    return 1;
}

static sint32 psx_vsync_wait_field(uint32 initial_status, uint32 *waited)
{
    uint32 before, counter, status;
    if (!(initial_status & 0x80000u))
        return 1;
    for (;;)
    {
        if (!vsync_sample(vsync_sample_context, &counter, &status))
            return 0;
        if ((status ^ initial_status) & 0x80000000u)
            return 1;
        if (*waited >= vsync_wait_budget)
            return 0;
        before = *vblank_guest_counter;
        ++*waited;
        if (!vblank_wait(vblank_wait_context) || vblank_wait_failed || *vblank_guest_counter - before != 1u)
            return 0;
    }
}

static sint32 psx_vsync_sdk(sint32 mode)
{
    uint32 counter, status, elapsed, target, waited = 0u;
    if (!vblank_guest_counter || !vblank_wait || !vsync_sample(vsync_sample_context, &counter, &status))
        goto failed;
    elapsed = (counter - *vsync_last_counter) & 0xffffu;
    if (mode < 0)
        return (sint32)*vblank_guest_counter;
    if (mode == 1)
        return (sint32)elapsed;
    target = *vsync_last_vblank + (mode > 0 ? (uint32)mode - 1u : 0u);
    if (!psx_vsync_wait_target(target, &waited) || !vsync_sample(vsync_sample_context, &counter, &status))
        goto failed;
    target = *vblank_guest_counter + 1u;
    if (!psx_vsync_wait_target(target, &waited) || !psx_vsync_wait_field(status, &waited) || !vsync_sample(vsync_sample_context, &counter, &status))
        goto failed;
    *vsync_last_vblank = *vblank_guest_counter;
    *vsync_last_counter = counter;
    return (sint32)elapsed;
failed:
    vblank_wait_failed = 1;
    return -1;
}

sint32 psx_vsync_resume_second_wait(uint32 target, uint32 entry_elapsed, uint32 saved_gpu_status)
{
    uint32 waited = 0u, counter, status;
    if (vblank_wait_failed || vblank_dispatching || !vsync_sample || !vblank_wait || !vblank_guest_counter || entry_elapsed > 0xffffu || !psx_vsync_wait_target(target, &waited) || !psx_vsync_wait_field(saved_gpu_status, &waited) || !vsync_sample(vsync_sample_context, &counter, &status))
    {
        vblank_wait_failed = 1;
        return -1;
    }
    *vsync_last_vblank = *vblank_guest_counter;
    *vsync_last_counter = counter;
    return (sint32)entry_elapsed;
}

void psx_clock_init(PsxClock *clock, uint64 now, PsxClockAdvance advance, void *context)
{
    memset(clock, 0, sizeof(*clock));
    clock->now = now;
    clock->advance = advance;
    clock->context = context;
}

sint32 psx_clock_schedule(PsxClock *clock, uint32 id, uint64 due, PsxClockEvent callback, void *context)
{
    PsxClockSlot *event;
    if (clock->failed || clock->advancing || id >= PSX_CLOCK_EVENTS || due < clock->now || !callback || clock->sequence == ~(uint64)0)
    {
        clock->failed = 1u;
        return 0;
    }
    event = &clock->events[id];
    event->due = due;
    event->order = ++clock->sequence;
    event->callback = callback;
    event->context = context;
    event->active = 1u;
    return 1;
}

sint32 psx_clock_cancel(PsxClock *clock, uint32 id)
{
    if (clock->failed || clock->advancing || id >= PSX_CLOCK_EVENTS)
    {
        clock->failed = 1u;
        return 0;
    }
    clock->events[id].active = 0u;
    return 1;
}

static sint32 psx_clock_step(PsxClock *clock, uint64 until)
{
    sint32 ok = 1;
    if (until < clock->now)
        return 0;
    if (clock->advance && until != clock->now)
    {
        clock->advancing = 1u;
        ok = clock->advance(clock->context, clock->now, until);
        clock->advancing = 0u;
    }
    if (!ok || clock->failed)
        return 0;
    clock->now = until;
    return 1;
}

sint32 psx_clock_run(PsxClock *clock, uint64 until, uint32 event_budget)
{
    uint32 executed = 0u;
    if (clock->failed || clock->running || until < clock->now)
    {
        clock->failed = 1u;
        return 0;
    }
    clock->running = 1u;
    for (;;)
    {
        uint32 index;
        uint32 selected = PSX_CLOCK_EVENTS;
        PsxClockSlot event;
        for (index = 0u; index < PSX_CLOCK_EVENTS; ++index)
        {
            PsxClockSlot *candidate = &clock->events[index];
            if (!candidate->active || candidate->due > until)
                continue;
            if (selected == PSX_CLOCK_EVENTS || candidate->due < clock->events[selected].due || (candidate->due == clock->events[selected].due && candidate->order < clock->events[selected].order))
                selected = index;
        }
        if (selected == PSX_CLOCK_EVENTS)
        {
            if (!psx_clock_step(clock, until))
                clock->failed = 1u;
            break;
        }
        if (executed++ >= event_budget || !psx_clock_step(clock, clock->events[selected].due))
        {
            clock->failed = 1u;
            break;
        }
        event = clock->events[selected];
        clock->events[selected].active = 0u;
        if (!event.callback(event.context, clock->now) || clock->failed)
        {
            clock->failed = 1u;
            break;
        }
    }
    clock->running = 0u;
    return !clock->failed;
}

sint32 psx_timers_import(const PsxTimerState *state)
{
    uint32 index;
    if (!state || state->div8_carry >= 8u)
        return 0;
    for (index = 0; index < 3u; ++index)
        if (state->counter[index] > 0xffffu || state->target[index] > 0xffffu || state->mode[index] > 0x1fffu)
            return 0;
    timers = *state;
    return 1;
}

void psx_timers_export(PsxTimerState *state)
{
    if (state)
        *state = timers;
}

static uint8 native_timer_irq_fired[3];

static void psx_timer_irq(uint32 index)
{
    uint32 mode = timers.mode[index];
    if (!(mode & 0x40u) && native_timer_irq_fired[index])
        return;
    native_timer_irq_fired[index] = 1u;
    psx_interrupt_raise(index + 4u);
}

static void psx_timer_add(uint32 index, uint32 ticks)
{
    while (ticks)
    {
        uint32 old = timers.counter[index];
        uint32 target = timers.target[index];
        uint32 to_wrap = 0x10000u - old;
        uint32 to_target = target > old ? target - old : 0x10000u - old + target;
        uint32 step = to_wrap < to_target ? to_wrap : to_target;
        uint32 mode = timers.mode[index];
        if (ticks < step)
        {
            timers.counter[index] = old + ticks;
            return;
        }
        ticks -= step;
        timers.counter[index] = (old + step) & 0xFFFFu;
        if (step == to_target)
        {
            mode |= 0x800u;
            if (mode & 8u)
                timers.counter[index] = 0u;
        }
        if (step == to_wrap)
            mode |= 0x1000u;
        timers.mode[index] = mode;
        if ((step == to_target && (mode & 0x10u)) || (step == to_wrap && (mode & 0x20u)))
            psx_timer_irq(index);
    }
}

sint32 psx_timers_advance(uint32 cpu_ticks, uint32 dot_ticks, uint32 hblank_ticks)
{
    uint32 index;
    uint32 divided;
    if (cpu_ticks > 0x7fffffffu || dot_ticks > 0x7fffffffu || hblank_ticks > 0x7fffffffu)
        return 0;
    // Gate synchronization and toggle IRQ require the GPU scheduler
    for (index = 0; index < 3u; ++index)
        if (timers.mode[index] & 0x81u)
            return 0;
    psx_timer_add(0u, timers.mode[0] & 0x100u ? dot_ticks : cpu_ticks);
    psx_timer_add(1u, timers.mode[1] & 0x100u ? hblank_ticks : cpu_ticks);
    if (timers.mode[2] & 0x200u)
    {
        divided = cpu_ticks + timers.div8_carry;
        timers.div8_carry = divided % 8u;
        psx_timer_add(2u, divided / 8u);
    }
    else
        psx_timer_add(2u, cpu_ticks);
    return 1;
}

sint32 SetRCnt(uint32 counter, uint16 target, sint32 mode)
{
    uint32 bits = 0x48u;
    psx_root_counter_poll();
    counter &= 0xffffu;
    if (counter >= 3u)
        return 0;
    if (counter < 2u)
    {
        if (mode & 0x10u)
            bits = 0x49u;
        if (!(mode & 1u))
            bits |= 0x100u;
    }
    else if (!(mode & 1u))
        bits = 0x248u;
    if (mode & 0x1000u)
        bits |= 0x10u;
    native_timer_irq_fired[counter] = 0u;
    timers.counter[counter] = 0u;
    timers.target[counter] = target;
    timers.mode[counter] = bits | 0x400u | (timers.mode[counter] & 0x1800u);
    return 1;
}

sint32 ResetRCnt(uint32 counter)
{
    counter &= 0xffffu;
    if (counter >= 3u)
        return 0;
    timers.counter[counter] = 0u;
    return 1;
}

#if !defined(LOCKSTEP_DEBUG)
uint32 SCRATCHPAD[256];
uint8 DRAM[PSX_DRAM_SIZE];
#endif

void *psx_addr(uint32 address, size_t count)
{
    uint32 physical = address & 0x1fffffffu;
    uint32 segment = address & 0xe0000000u;
    uint8 *base;
    size_t capacity;

    if (physical >= 0x1f800000u && physical < 0x1f800400u)
    {
        physical -= 0x1f800000u;
        base = (uint8 *)SCRATCHPAD;
        capacity = sizeof(SCRATCHPAD);
    }
    else if (physical < PSX_DRAM_SIZE || ((segment == 0x80000000u || segment == 0xa0000000u) && physical < 0x00800000u))
    {
        physical &= PSX_DRAM_SIZE - 1u;
        base = DRAM;
        capacity = sizeof(DRAM);
    }
    else if (address >= 0x00200000u && address < 0x80000000u && xport_memory_readable((const void *)(intptr)address, count))
    {
        return (void *)(intptr)address;
    }
    else
    {
#ifndef NDEBUG
        fprintf(stderr, "Unmapped PSX address %08X\n", address);
#endif
        abort();
    }
    if (count > capacity - physical)
    {
#ifndef NDEBUG
        fprintf(stderr, "PSX address range exceeds memory: %08X + %zu\n", address, count);
#endif
        abort();
    }
    return base + physical;
}

static PsxInterruptCallback native_irq_callbacks[11];

uint16 psx_irq_native_mask_exchange(uint16 mask)
{
    uint16 previous = (uint16)native_root_irq.mask;
    if (!psx_irq_write(&native_root_irq, 4u, mask))
    {
#ifndef NDEBUG
        fprintf(stderr, "Invalid native IRQ mask state\n");
#endif
        abort();
    }
    return previous;
}

sint32 psx_irq_native_acknowledge(uint32 sources)
{
    if (sources & ~0x7FFu)
        return 0;
    return psx_irq_write(&native_root_irq, 0u, ~sources);
}

PsxInterruptCallback InterruptCallback(sint32 interrupt, PsxInterruptCallback callback)
{
    PsxInterruptCallback previous;
    uint32 bit;
    if (interrupt < 0 || interrupt >= 11)
        return NULL;
    previous = native_irq_callbacks[interrupt];
    if (previous == callback)
        return previous;
    bit = 1u << interrupt;
    native_irq_callbacks[interrupt] = callback;
    if (callback)
        native_root_irq.mask |= bit;
    else
        native_root_irq.mask &= ~bit;
    if (interrupt == 0)
    {
        ChangeClearPAD(callback == NULL);
        ChangeClearRCnt(3u, callback == NULL);
    }
    if (interrupt >= 4 && interrupt <= 6)
        ChangeClearRCnt((uint32)interrupt - 4u, callback == NULL);
    return previous;
}

static uint32 native_irq_dispatching;

static void bios_native_irq_dispatch(void)
{
    uint32 interrupt;
    if (!bios_interrupts_enabled || native_irq_dispatching)
        return;
    native_irq_dispatching = 1u;
    for (interrupt = 0u; interrupt < 11u && bios_interrupts_enabled; ++interrupt)
    {
        uint32 bit = 1u << interrupt;
        if (!(native_root_irq.status & native_root_irq.mask & bit))
            continue;
        if (native_irq_callbacks[interrupt])
            native_irq_callbacks[interrupt]();
        else if (interrupt == 0u)
            DeliverEvent(0xF2000003u, 2u);
        else if (interrupt >= 4u && interrupt <= 6u)
            DeliverEvent(0xF2000000u + interrupt - 4u, 2u);
        else
            continue;
        psx_irq_write(&native_root_irq, 0u, ~bit);
    }
    if (bios_interrupts_enabled)
        cd_dispatch_ready_event();
    native_irq_dispatching = 0u;
}

void psx_interrupt_raise(uint32 interrupt)
{
    if (interrupt >= 11u)
        return;
    psx_irq_line(&native_root_irq, interrupt, 1u);
    bios_native_irq_dispatch();
    psx_irq_line(&native_root_irq, interrupt, 0u);
}

sint32 ResetCallback(void)
{
    static uint32 initialized;
    if (initialized)
        return 0;
    vsync_callback = NULL;
    memset(native_irq_callbacks, 0, sizeof(native_irq_callbacks));
    initialized = 1u;
    return 0;
}

sint32 VSyncCallback(PsxInterruptCallback callback)
{
    PsxInterruptCallback previous = vsync_callback;
    vsync_callback = callback;
    return (sint32)(intptr)previous;
}

/* Fixed-point trigonometry and PsyQ GTE implementation. */
/* Each entry is {s16 sin,s16 cos} in 4.12 format */

/* Variables. */
/* Exact PsyQ quarter-wave table */
static const sint16 psyq_sine_quarter[1025] = {0,    6,    13,   19,   25,   31,   38,   44,   50,   57,   63,   69,   75,   82,   88,   94,   101,  107,  113,  119,  126,  132,  138,  144,  151,  157,  163,  170,  176,  182,  188,  195,  201,  207,  214,  220,  226,  232,  239,  245,  251,  257,  264,  270,  276,  283,  289,  295,  301,  308,  314,  320,  326,  333,  339,  345,  351,  358,  364,  370,  376,  383,  389,  395,  401,  408,  414,  420,  426,  433,  439,  445,  451,  458,  464,  470,  476,  483,  489,  495,  501,  508,  514,  520,  526,  533,  539,  545,  551,  557,  564,  570,  576,  582,  589,  595,  601,  607,  613,  620,  626,  632,  638,  644,  651,  657,  663,  669,  675,  682,  688,  694,  700,  706,  713,  719,  725,  731,  737,  744,  750,  756,  762,  768,  774,  781,  787,  793,  799,  805,  811,  818,  824,  830,  836,  842,  848,  854,  861,  867,  873,  879,  885,  891,  897,  904,  910,
                                               916,  922,  928,  934,  940,  946,  953,  959,  965,  971,  977,  983,  989,  995,  1001, 1007, 1014, 1020, 1026, 1032, 1038, 1044, 1050, 1056, 1062, 1068, 1074, 1080, 1086, 1092, 1099, 1105, 1111, 1117, 1123, 1129, 1135, 1141, 1147, 1153, 1159, 1165, 1171, 1177, 1183, 1189, 1195, 1201, 1207, 1213, 1219, 1225, 1231, 1237, 1243, 1249, 1255, 1261, 1267, 1273, 1279, 1285, 1291, 1297, 1303, 1309, 1315, 1321, 1327, 1332, 1338, 1344, 1350, 1356, 1362, 1368, 1374, 1380, 1386, 1392, 1398, 1404, 1409, 1415, 1421, 1427, 1433, 1439, 1445, 1451, 1457, 1462, 1468, 1474, 1480, 1486, 1492, 1498, 1503, 1509, 1515, 1521, 1527, 1533, 1538, 1544, 1550, 1556, 1562, 1567, 1573, 1579, 1585, 1591, 1596, 1602, 1608, 1614, 1620, 1625, 1631, 1637, 1643, 1648, 1654, 1660, 1666, 1671, 1677, 1683, 1689, 1694, 1700, 1706, 1711, 1717, 1723, 1729, 1734, 1740, 1746, 1751, 1757, 1763, 1768, 1774, 1780,
                                               1785, 1791, 1797, 1802, 1808, 1813, 1819, 1825, 1830, 1836, 1842, 1847, 1853, 1858, 1864, 1870, 1875, 1881, 1886, 1892, 1898, 1903, 1909, 1914, 1920, 1925, 1931, 1936, 1942, 1947, 1953, 1958, 1964, 1970, 1975, 1981, 1986, 1992, 1997, 2002, 2008, 2013, 2019, 2024, 2030, 2035, 2041, 2046, 2052, 2057, 2062, 2068, 2073, 2079, 2084, 2090, 2095, 2100, 2106, 2111, 2117, 2122, 2127, 2133, 2138, 2143, 2149, 2154, 2159, 2165, 2170, 2175, 2181, 2186, 2191, 2197, 2202, 2207, 2213, 2218, 2223, 2228, 2234, 2239, 2244, 2249, 2255, 2260, 2265, 2270, 2276, 2281, 2286, 2291, 2296, 2302, 2307, 2312, 2317, 2322, 2328, 2333, 2338, 2343, 2348, 2353, 2359, 2364, 2369, 2374, 2379, 2384, 2389, 2394, 2399, 2405, 2410, 2415, 2420, 2425, 2430, 2435, 2440, 2445, 2450, 2455, 2460, 2465, 2470, 2475, 2480, 2485, 2490, 2495, 2500, 2505, 2510, 2515, 2520, 2525, 2530, 2535, 2540, 2545, 2550, 2555, 2559,
                                               2564, 2569, 2574, 2579, 2584, 2589, 2594, 2598, 2603, 2608, 2613, 2618, 2623, 2628, 2632, 2637, 2642, 2647, 2652, 2656, 2661, 2666, 2671, 2675, 2680, 2685, 2690, 2694, 2699, 2704, 2709, 2713, 2718, 2723, 2727, 2732, 2737, 2741, 2746, 2751, 2755, 2760, 2765, 2769, 2774, 2779, 2783, 2788, 2792, 2797, 2802, 2806, 2811, 2815, 2820, 2824, 2829, 2833, 2838, 2843, 2847, 2852, 2856, 2861, 2865, 2870, 2874, 2878, 2883, 2887, 2892, 2896, 2901, 2905, 2910, 2914, 2918, 2923, 2927, 2932, 2936, 2940, 2945, 2949, 2953, 2958, 2962, 2967, 2971, 2975, 2979, 2984, 2988, 2992, 2997, 3001, 3005, 3009, 3014, 3018, 3022, 3026, 3031, 3035, 3039, 3043, 3048, 3052, 3056, 3060, 3064, 3068, 3073, 3077, 3081, 3085, 3089, 3093, 3097, 3102, 3106, 3110, 3114, 3118, 3122, 3126, 3130, 3134, 3138, 3142, 3146, 3150, 3154, 3158, 3162, 3166, 3170, 3174, 3178, 3182, 3186, 3190, 3194, 3198, 3202, 3206, 3210,
                                               3214, 3217, 3221, 3225, 3229, 3233, 3237, 3241, 3244, 3248, 3252, 3256, 3260, 3264, 3267, 3271, 3275, 3279, 3282, 3286, 3290, 3294, 3297, 3301, 3305, 3309, 3312, 3316, 3320, 3323, 3327, 3331, 3334, 3338, 3342, 3345, 3349, 3352, 3356, 3360, 3363, 3367, 3370, 3374, 3378, 3381, 3385, 3388, 3392, 3395, 3399, 3402, 3406, 3409, 3413, 3416, 3420, 3423, 3426, 3430, 3433, 3437, 3440, 3444, 3447, 3450, 3454, 3457, 3461, 3464, 3467, 3471, 3474, 3477, 3481, 3484, 3487, 3490, 3494, 3497, 3500, 3504, 3507, 3510, 3513, 3516, 3520, 3523, 3526, 3529, 3532, 3536, 3539, 3542, 3545, 3548, 3551, 3555, 3558, 3561, 3564, 3567, 3570, 3573, 3576, 3579, 3582, 3585, 3588, 3591, 3594, 3597, 3600, 3603, 3606, 3609, 3612, 3615, 3618, 3621, 3624, 3627, 3630, 3633, 3636, 3639, 3642, 3644, 3647, 3650, 3653, 3656, 3659, 3661, 3664, 3667, 3670, 3673, 3675, 3678, 3681, 3684, 3686, 3689, 3692, 3695, 3697,
                                               3700, 3703, 3705, 3708, 3711, 3713, 3716, 3719, 3721, 3724, 3727, 3729, 3732, 3734, 3737, 3739, 3742, 3745, 3747, 3750, 3752, 3755, 3757, 3760, 3762, 3765, 3767, 3770, 3772, 3775, 3777, 3779, 3782, 3784, 3787, 3789, 3791, 3794, 3796, 3798, 3801, 3803, 3805, 3808, 3810, 3812, 3815, 3817, 3819, 3822, 3824, 3826, 3828, 3831, 3833, 3835, 3837, 3839, 3842, 3844, 3846, 3848, 3850, 3852, 3854, 3857, 3859, 3861, 3863, 3865, 3867, 3869, 3871, 3873, 3875, 3877, 3879, 3881, 3883, 3885, 3887, 3889, 3891, 3893, 3895, 3897, 3899, 3901, 3903, 3905, 3907, 3909, 3910, 3912, 3914, 3916, 3918, 3920, 3921, 3923, 3925, 3927, 3929, 3930, 3932, 3934, 3936, 3937, 3939, 3941, 3943, 3944, 3946, 3948, 3949, 3951, 3953, 3954, 3956, 3958, 3959, 3961, 3962, 3964, 3965, 3967, 3969, 3970, 3972, 3973, 3975, 3976, 3978, 3979, 3981, 3982, 3984, 3985, 3987, 3988, 3989, 3991, 3992, 3994, 3995, 3996, 3998,
                                               3999, 4001, 4002, 4003, 4005, 4006, 4007, 4008, 4010, 4011, 4012, 4014, 4015, 4016, 4017, 4019, 4020, 4021, 4022, 4023, 4024, 4026, 4027, 4028, 4029, 4030, 4031, 4032, 4034, 4035, 4036, 4037, 4038, 4039, 4040, 4041, 4042, 4043, 4044, 4045, 4046, 4047, 4048, 4049, 4050, 4051, 4052, 4053, 4053, 4054, 4055, 4056, 4057, 4058, 4059, 4060, 4060, 4061, 4062, 4063, 4064, 4064, 4065, 4066, 4067, 4067, 4068, 4069, 4070, 4070, 4071, 4072, 4072, 4073, 4074, 4074, 4075, 4076, 4076, 4077, 4077, 4078, 4079, 4079, 4080, 4080, 4081, 4081, 4082, 4082, 4083, 4083, 4084, 4084, 4085, 4085, 4086, 4086, 4087, 4087, 4088, 4088, 4088, 4089, 4089, 4089, 4090, 4090, 4090, 4091, 4091, 4091, 4092, 4092, 4092, 4092, 4093, 4093, 4093, 4093, 4094, 4094, 4094, 4094, 4094, 4095, 4095, 4095, 4095, 4095, 4095, 4095, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096};

/* Exact PsyQ square-root table */
static const sint16 psyq_sqrt_table[192] = {
    4096, 4127, 4159, 4190, 4222, 4252, 4283, 4314, 4344, 4374, 4404, 4434, 4463, 4492, 4521, 4550, 4579, 4608, 4636, 4664, 4692, 4720, 4748, 4775, 4802, 4830, 4857, 4884, 4910, 4937, 4964, 4990, 5016, 5042, 5068, 5094, 5120, 5145, 5170, 5196, 5221, 5246, 5271, 5296, 5320, 5345, 5369, 5394, 5418, 5442, 5466, 5490, 5514, 5538, 5561, 5585, 5608, 5632, 5655, 5678, 5701, 5724, 5747, 5769, 5792, 5815, 5837, 5860, 5882, 5904, 5926, 5948, 5970, 5992, 6014, 6036, 6058, 6079, 6101, 6122, 6144, 6165, 6186, 6207, 6228, 6249, 6270, 6291, 6312, 6333, 6353, 6374, 6394, 6415, 6435, 6456,
    6476, 6496, 6516, 6536, 6556, 6576, 6596, 6616, 6636, 6656, 6675, 6695, 6714, 6734, 6753, 6773, 6792, 6811, 6830, 6850, 6869, 6888, 6907, 6926, 6945, 6963, 6982, 7001, 7020, 7038, 7057, 7075, 7094, 7112, 7131, 7149, 7168, 7186, 7204, 7222, 7240, 7258, 7276, 7294, 7312, 7330, 7348, 7366, 7384, 7401, 7419, 7437, 7454, 7472, 7489, 7507, 7524, 7542, 7559, 7576, 7594, 7611, 7628, 7645, 7662, 7680, 7697, 7714, 7731, 7747, 7764, 7781, 7798, 7815, 7832, 7848, 7865, 7882, 7898, 7915, 7931, 7948, 7964, 7981, 7997, 8014, 8030, 8046, 8062, 8079, 8095, 8111, 8127, 8143, 8159, 8175,
};

// Exact PsyQ reciprocal square-root table
static const sint16 psyq_rsqrt_table[192] = {
    4096, 4064, 4033, 4003, 3973, 3944, 3916, 3888, 3861, 3835, 3809, 3783, 3758, 3734, 3710, 3686, 3663, 3640, 3618, 3596, 3575, 3554, 3533, 3513, 3493, 3473, 3454, 3435, 3416, 3397, 3379, 3361, 3344, 3327, 3310, 3293, 3276, 3260, 3244, 3228, 3213, 3197, 3182, 3167, 3153, 3138, 3124, 3110, 3096, 3082, 3069, 3055, 3042, 3029, 3016, 3003, 2991, 2978, 2966, 2954, 2942, 2930, 2919, 2907, 2896, 2885, 2873, 2862, 2852, 2841, 2830, 2820, 2809, 2799, 2789, 2779, 2769, 2759, 2749, 2740, 2730, 2721, 2711, 2702, 2693, 2684, 2675, 2666, 2657, 2649, 2640, 2631, 2623, 2615, 2606, 2598,
    2590, 2582, 2574, 2566, 2558, 2550, 2543, 2535, 2528, 2520, 2513, 2505, 2498, 2491, 2484, 2477, 2469, 2462, 2456, 2449, 2442, 2435, 2428, 2422, 2415, 2409, 2402, 2396, 2389, 2383, 2377, 2371, 2364, 2358, 2352, 2346, 2340, 2334, 2328, 2322, 2317, 2311, 2305, 2299, 2294, 2288, 2283, 2277, 2272, 2266, 2261, 2255, 2250, 2245, 2239, 2234, 2229, 2224, 2219, 2214, 2209, 2204, 2199, 2194, 2189, 2184, 2179, 2174, 2170, 2165, 2160, 2155, 2151, 2146, 2142, 2137, 2133, 2128, 2124, 2119, 2115, 2110, 2106, 2102, 2097, 2093, 2089, 2084, 2080, 2076, 2072, 2068, 2064, 2060, 2056, 2052,
};

/* Original tables have nine halfwords between the sine and square-root arrays */
enum
{
    PSYQ_SQRT_SINE_OFFSET = 1025 + 9
};

/* Functions. */
uint32 SquareRoot0(sint32 input)
{
    uint32 bits = (uint32)input;
    uint32 leading = 0;
    uint32 even;
    sint32 table_index;
    sint32 value;
    uint32 shift;

    while (leading < 32u && ((bits >> (31u - leading)) & 1u) == (input < 0 ? 1u : 0u))
        ++leading;
    if (leading == 32u)
        return 0;
    even = leading & ~1u;
    if (even < 24u)
        table_index = input >> (24u - even);
    else
        table_index = (sint32)(bits << (even - 24u));
    /* Signed normalization selects preceding sine entries for negative inputs */
    if (table_index < 0)
        value = psyq_sine_quarter[PSYQ_SQRT_SINE_OFFSET + table_index - 64];
    else
        value = psyq_sqrt_table[table_index - 64];
    shift = (31u - even) >> 1;
    return ((uint32)value << shift) >> 12;
}

static sint32 psyq_sine(sint32 a)
{
    uint32 angle = (uint32)a & 0xfffu;

    if (angle <= 0x400u)
        return psyq_sine_quarter[angle];
    if (angle <= 0x800u)
        return psyq_sine_quarter[0x800u - angle];
    if (angle <= 0xc00u)
        return -psyq_sine_quarter[angle - 0x800u];
    return -psyq_sine_quarter[0x1000u - angle];
}

// PsyQ packed sin/cos quarter with duplicated quadrant endpoints
static const sint16 gte_sincos_quarter[1024][2] = {
    {0, 4096},    {6, 4096},    {13, 4096},   {19, 4096},   {25, 4096},   {31, 4096},   {38, 4096},   {44, 4096},   {50, 4096},   {57, 4096},   {63, 4095},   {69, 4095},   {75, 4095},   {82, 4095},   {88, 4095},   {94, 4095},   {101, 4095},  {107, 4094},  {113, 4094},  {119, 4094},  {126, 4094},  {132, 4094},  {138, 4093},  {144, 4093},  {151, 4093},  {157, 4093},  {163, 4092},  {170, 4092},  {176, 4092},  {182, 4092},  {188, 4091},  {195, 4091},  {201, 4091},  {207, 4090},  {214, 4090},  {220, 4090},  {226, 4089},  {232, 4089},  {239, 4089},  {245, 4088},  {251, 4088},  {257, 4088},  {264, 4087},  {270, 4087},  {276, 4086},  {283, 4086},  {289, 4085},  {295, 4085},  {301, 4084},  {308, 4084},  {314, 4083},  {320, 4083},  {326, 4082},  {333, 4082},  {339, 4081},  {345, 4081},  {351, 4080},  {358, 4080},  {364, 4079},  {370, 4079},  {376, 4078},  {383, 4077},  {389, 4077},  {395, 4076},  {401, 4076},  {408, 4075},  {414, 4074},  {420, 4074},  {426, 4073},
    {433, 4072},  {439, 4072},  {445, 4071},  {451, 4070},  {458, 4070},  {464, 4069},  {470, 4068},  {476, 4067},  {483, 4067},  {489, 4066},  {495, 4065},  {501, 4064},  {508, 4064},  {514, 4063},  {520, 4062},  {526, 4061},  {533, 4060},  {539, 4060},  {545, 4059},  {551, 4058},  {557, 4057},  {564, 4056},  {570, 4055},  {576, 4054},  {582, 4053},  {589, 4053},  {595, 4052},  {601, 4051},  {607, 4050},  {613, 4049},  {620, 4048},  {626, 4047},  {632, 4046},  {638, 4045},  {644, 4044},  {651, 4043},  {657, 4042},  {663, 4041},  {669, 4040},  {675, 4039},  {682, 4038},  {688, 4037},  {694, 4036},  {700, 4035},  {706, 4034},  {713, 4032},  {719, 4031},  {725, 4030},  {731, 4029},  {737, 4028},  {744, 4027},  {750, 4026},  {756, 4024},  {762, 4023},  {768, 4022},  {774, 4021},  {781, 4020},  {787, 4019},  {793, 4017},  {799, 4016},  {805, 4015},  {811, 4014},  {818, 4012},  {824, 4011},  {830, 4010},  {836, 4008},  {842, 4007},  {848, 4006},  {854, 4005},
    {861, 4003},  {867, 4002},  {873, 4001},  {879, 3999},  {885, 3998},  {891, 3996},  {897, 3995},  {904, 3994},  {910, 3992},  {916, 3991},  {922, 3989},  {928, 3988},  {934, 3987},  {940, 3985},  {946, 3984},  {953, 3982},  {959, 3981},  {965, 3979},  {971, 3978},  {977, 3976},  {983, 3975},  {989, 3973},  {995, 3972},  {1001, 3970}, {1007, 3969}, {1014, 3967}, {1020, 3965}, {1026, 3964}, {1032, 3962}, {1038, 3961}, {1044, 3959}, {1050, 3958}, {1056, 3956}, {1062, 3954}, {1068, 3953}, {1074, 3951}, {1080, 3949}, {1086, 3948}, {1092, 3946}, {1099, 3944}, {1105, 3943}, {1111, 3941}, {1117, 3939}, {1123, 3937}, {1129, 3936}, {1135, 3934}, {1141, 3932}, {1147, 3930}, {1153, 3929}, {1159, 3927}, {1165, 3925}, {1171, 3923}, {1177, 3921}, {1183, 3920}, {1189, 3918}, {1195, 3916}, {1201, 3914}, {1207, 3912}, {1213, 3910}, {1219, 3909}, {1225, 3907}, {1231, 3905}, {1237, 3903}, {1243, 3901}, {1249, 3899}, {1255, 3897}, {1261, 3895}, {1267, 3893}, {1273, 3891},
    {1279, 3889}, {1285, 3887}, {1291, 3885}, {1297, 3883}, {1303, 3881}, {1309, 3879}, {1315, 3877}, {1321, 3875}, {1327, 3873}, {1332, 3871}, {1338, 3869}, {1344, 3867}, {1350, 3865}, {1356, 3863}, {1362, 3861}, {1368, 3859}, {1374, 3857}, {1380, 3854}, {1386, 3852}, {1392, 3850}, {1398, 3848}, {1404, 3846}, {1409, 3844}, {1415, 3842}, {1421, 3839}, {1427, 3837}, {1433, 3835}, {1439, 3833}, {1445, 3831}, {1451, 3828}, {1457, 3826}, {1462, 3824}, {1468, 3822}, {1474, 3819}, {1480, 3817}, {1486, 3815}, {1492, 3812}, {1498, 3810}, {1503, 3808}, {1509, 3805}, {1515, 3803}, {1521, 3801}, {1527, 3798}, {1533, 3796}, {1538, 3794}, {1544, 3791}, {1550, 3789}, {1556, 3787}, {1562, 3784}, {1567, 3782}, {1573, 3779}, {1579, 3777}, {1585, 3775}, {1591, 3772}, {1596, 3770}, {1602, 3767}, {1608, 3765}, {1614, 3762}, {1620, 3760}, {1625, 3757}, {1631, 3755}, {1637, 3752}, {1643, 3750}, {1648, 3747}, {1654, 3745}, {1660, 3742}, {1666, 3739}, {1671, 3737}, {1677, 3734},
    {1683, 3732}, {1689, 3729}, {1694, 3727}, {1700, 3724}, {1706, 3721}, {1711, 3719}, {1717, 3716}, {1723, 3713}, {1729, 3711}, {1734, 3708}, {1740, 3705}, {1746, 3703}, {1751, 3700}, {1757, 3697}, {1763, 3695}, {1768, 3692}, {1774, 3689}, {1780, 3686}, {1785, 3684}, {1791, 3681}, {1797, 3678}, {1802, 3675}, {1808, 3673}, {1813, 3670}, {1819, 3667}, {1825, 3664}, {1830, 3661}, {1836, 3659}, {1842, 3656}, {1847, 3653}, {1853, 3650}, {1858, 3647}, {1864, 3644}, {1870, 3642}, {1875, 3639}, {1881, 3636}, {1886, 3633}, {1892, 3630}, {1898, 3627}, {1903, 3624}, {1909, 3621}, {1914, 3618}, {1920, 3615}, {1925, 3612}, {1931, 3609}, {1936, 3606}, {1942, 3603}, {1947, 3600}, {1953, 3597}, {1958, 3594}, {1964, 3591}, {1970, 3588}, {1975, 3585}, {1981, 3582}, {1986, 3579}, {1992, 3576}, {1997, 3573}, {2002, 3570}, {2008, 3567}, {2013, 3564}, {2019, 3561}, {2024, 3558}, {2030, 3555}, {2035, 3551}, {2041, 3548}, {2046, 3545}, {2052, 3542}, {2057, 3539}, {2062, 3536},
    {2068, 3532}, {2073, 3529}, {2079, 3526}, {2084, 3523}, {2090, 3520}, {2095, 3516}, {2100, 3513}, {2106, 3510}, {2111, 3507}, {2117, 3504}, {2122, 3500}, {2127, 3497}, {2133, 3494}, {2138, 3490}, {2143, 3487}, {2149, 3484}, {2154, 3481}, {2159, 3477}, {2165, 3474}, {2170, 3471}, {2175, 3467}, {2181, 3464}, {2186, 3461}, {2191, 3457}, {2197, 3454}, {2202, 3450}, {2207, 3447}, {2213, 3444}, {2218, 3440}, {2223, 3437}, {2228, 3433}, {2234, 3430}, {2239, 3426}, {2244, 3423}, {2249, 3420}, {2255, 3416}, {2260, 3413}, {2265, 3409}, {2270, 3406}, {2276, 3402}, {2281, 3399}, {2286, 3395}, {2291, 3392}, {2296, 3388}, {2302, 3385}, {2307, 3381}, {2312, 3378}, {2317, 3374}, {2322, 3370}, {2328, 3367}, {2333, 3363}, {2338, 3360}, {2343, 3356}, {2348, 3352}, {2353, 3349}, {2359, 3345}, {2364, 3342}, {2369, 3338}, {2374, 3334}, {2379, 3331}, {2384, 3327}, {2389, 3323}, {2394, 3320}, {2399, 3316}, {2405, 3312}, {2410, 3309}, {2415, 3305}, {2420, 3301}, {2425, 3297},
    {2430, 3294}, {2435, 3290}, {2440, 3286}, {2445, 3282}, {2450, 3279}, {2455, 3275}, {2460, 3271}, {2465, 3267}, {2470, 3264}, {2475, 3260}, {2480, 3256}, {2485, 3252}, {2490, 3248}, {2495, 3244}, {2500, 3241}, {2505, 3237}, {2510, 3233}, {2515, 3229}, {2520, 3225}, {2525, 3221}, {2530, 3217}, {2535, 3214}, {2540, 3210}, {2545, 3206}, {2550, 3202}, {2555, 3198}, {2559, 3194}, {2564, 3190}, {2569, 3186}, {2574, 3182}, {2579, 3178}, {2584, 3174}, {2589, 3170}, {2594, 3166}, {2598, 3162}, {2603, 3158}, {2608, 3154}, {2613, 3150}, {2618, 3146}, {2623, 3142}, {2628, 3138}, {2632, 3134}, {2637, 3130}, {2642, 3126}, {2647, 3122}, {2652, 3118}, {2656, 3114}, {2661, 3110}, {2666, 3106}, {2671, 3102}, {2675, 3097}, {2680, 3093}, {2685, 3089}, {2690, 3085}, {2694, 3081}, {2699, 3077}, {2704, 3073}, {2709, 3068}, {2713, 3064}, {2718, 3060}, {2723, 3056}, {2727, 3052}, {2732, 3048}, {2737, 3043}, {2741, 3039}, {2746, 3035}, {2751, 3031}, {2755, 3026}, {2760, 3022},
    {2765, 3018}, {2769, 3014}, {2774, 3009}, {2779, 3005}, {2783, 3001}, {2788, 2997}, {2792, 2992}, {2797, 2988}, {2802, 2984}, {2806, 2979}, {2811, 2975}, {2815, 2971}, {2820, 2967}, {2824, 2962}, {2829, 2958}, {2833, 2953}, {2838, 2949}, {2843, 2945}, {2847, 2940}, {2852, 2936}, {2856, 2932}, {2861, 2927}, {2865, 2923}, {2870, 2918}, {2874, 2914}, {2878, 2910}, {2883, 2905}, {2887, 2901}, {2892, 2896}, {2896, 2892}, {2901, 2887}, {2905, 2883}, {2910, 2878}, {2914, 2874}, {2918, 2870}, {2923, 2865}, {2927, 2861}, {2932, 2856}, {2936, 2852}, {2940, 2847}, {2945, 2843}, {2949, 2838}, {2953, 2833}, {2958, 2829}, {2962, 2824}, {2967, 2820}, {2971, 2815}, {2975, 2811}, {2979, 2806}, {2984, 2802}, {2988, 2797}, {2992, 2792}, {2997, 2788}, {3001, 2783}, {3005, 2779}, {3009, 2774}, {3014, 2769}, {3018, 2765}, {3022, 2760}, {3026, 2755}, {3031, 2751}, {3035, 2746}, {3039, 2741}, {3043, 2737}, {3048, 2732}, {3052, 2727}, {3056, 2723}, {3060, 2718}, {3064, 2713},
    {3068, 2709}, {3073, 2704}, {3077, 2699}, {3081, 2694}, {3085, 2690}, {3089, 2685}, {3093, 2680}, {3097, 2675}, {3102, 2671}, {3106, 2666}, {3110, 2661}, {3114, 2656}, {3118, 2652}, {3122, 2647}, {3126, 2642}, {3130, 2637}, {3134, 2632}, {3138, 2628}, {3142, 2623}, {3146, 2618}, {3150, 2613}, {3154, 2608}, {3158, 2603}, {3162, 2598}, {3166, 2594}, {3170, 2589}, {3174, 2584}, {3178, 2579}, {3182, 2574}, {3186, 2569}, {3190, 2564}, {3194, 2559}, {3198, 2555}, {3202, 2550}, {3206, 2545}, {3210, 2540}, {3214, 2535}, {3217, 2530}, {3221, 2525}, {3225, 2520}, {3229, 2515}, {3233, 2510}, {3237, 2505}, {3241, 2500}, {3244, 2495}, {3248, 2490}, {3252, 2485}, {3256, 2480}, {3260, 2475}, {3264, 2470}, {3267, 2465}, {3271, 2460}, {3275, 2455}, {3279, 2450}, {3282, 2445}, {3286, 2440}, {3290, 2435}, {3294, 2430}, {3297, 2425}, {3301, 2420}, {3305, 2415}, {3309, 2410}, {3312, 2405}, {3316, 2399}, {3320, 2394}, {3323, 2389}, {3327, 2384}, {3331, 2379}, {3334, 2374},
    {3338, 2369}, {3342, 2364}, {3345, 2359}, {3349, 2353}, {3352, 2348}, {3356, 2343}, {3360, 2338}, {3363, 2333}, {3367, 2328}, {3370, 2322}, {3374, 2317}, {3378, 2312}, {3381, 2307}, {3385, 2302}, {3388, 2296}, {3392, 2291}, {3395, 2286}, {3399, 2281}, {3402, 2276}, {3406, 2270}, {3409, 2265}, {3413, 2260}, {3416, 2255}, {3420, 2249}, {3423, 2244}, {3426, 2239}, {3430, 2234}, {3433, 2228}, {3437, 2223}, {3440, 2218}, {3444, 2213}, {3447, 2207}, {3450, 2202}, {3454, 2197}, {3457, 2191}, {3461, 2186}, {3464, 2181}, {3467, 2175}, {3471, 2170}, {3474, 2165}, {3477, 2159}, {3481, 2154}, {3484, 2149}, {3487, 2143}, {3490, 2138}, {3494, 2133}, {3497, 2127}, {3500, 2122}, {3504, 2117}, {3507, 2111}, {3510, 2106}, {3513, 2100}, {3516, 2095}, {3520, 2090}, {3523, 2084}, {3526, 2079}, {3529, 2073}, {3532, 2068}, {3536, 2062}, {3539, 2057}, {3542, 2052}, {3545, 2046}, {3548, 2041}, {3551, 2035}, {3555, 2030}, {3558, 2024}, {3561, 2019}, {3564, 2013}, {3567, 2008},
    {3570, 2002}, {3573, 1997}, {3576, 1992}, {3579, 1986}, {3582, 1981}, {3585, 1975}, {3588, 1970}, {3591, 1964}, {3594, 1958}, {3597, 1953}, {3600, 1947}, {3603, 1942}, {3606, 1936}, {3609, 1931}, {3612, 1925}, {3615, 1920}, {3618, 1914}, {3621, 1909}, {3624, 1903}, {3627, 1898}, {3630, 1892}, {3633, 1886}, {3636, 1881}, {3639, 1875}, {3642, 1870}, {3644, 1864}, {3647, 1858}, {3650, 1853}, {3653, 1847}, {3656, 1842}, {3659, 1836}, {3661, 1830}, {3664, 1825}, {3667, 1819}, {3670, 1813}, {3673, 1808}, {3675, 1802}, {3678, 1797}, {3681, 1791}, {3684, 1785}, {3686, 1780}, {3689, 1774}, {3692, 1768}, {3695, 1763}, {3697, 1757}, {3700, 1751}, {3703, 1746}, {3705, 1740}, {3708, 1734}, {3711, 1729}, {3713, 1723}, {3716, 1717}, {3719, 1711}, {3721, 1706}, {3724, 1700}, {3727, 1694}, {3729, 1689}, {3732, 1683}, {3734, 1677}, {3737, 1671}, {3739, 1666}, {3742, 1660}, {3745, 1654}, {3747, 1648}, {3750, 1643}, {3752, 1637}, {3755, 1631}, {3757, 1625}, {3760, 1620},
    {3762, 1614}, {3765, 1608}, {3767, 1602}, {3770, 1596}, {3772, 1591}, {3775, 1585}, {3777, 1579}, {3779, 1573}, {3782, 1567}, {3784, 1562}, {3787, 1556}, {3789, 1550}, {3791, 1544}, {3794, 1538}, {3796, 1533}, {3798, 1527}, {3801, 1521}, {3803, 1515}, {3805, 1509}, {3808, 1503}, {3810, 1498}, {3812, 1492}, {3815, 1486}, {3817, 1480}, {3819, 1474}, {3822, 1468}, {3824, 1462}, {3826, 1457}, {3828, 1451}, {3831, 1445}, {3833, 1439}, {3835, 1433}, {3837, 1427}, {3839, 1421}, {3842, 1415}, {3844, 1409}, {3846, 1404}, {3848, 1398}, {3850, 1392}, {3852, 1386}, {3854, 1380}, {3857, 1374}, {3859, 1368}, {3861, 1362}, {3863, 1356}, {3865, 1350}, {3867, 1344}, {3869, 1338}, {3871, 1332}, {3873, 1327}, {3875, 1321}, {3877, 1315}, {3879, 1309}, {3881, 1303}, {3883, 1297}, {3885, 1291}, {3887, 1285}, {3889, 1279}, {3891, 1273}, {3893, 1267}, {3895, 1261}, {3897, 1255}, {3899, 1249}, {3901, 1243}, {3903, 1237}, {3905, 1231}, {3907, 1225}, {3909, 1219}, {3910, 1213},
    {3912, 1207}, {3914, 1201}, {3916, 1195}, {3918, 1189}, {3920, 1183}, {3921, 1177}, {3923, 1171}, {3925, 1165}, {3927, 1159}, {3929, 1153}, {3930, 1147}, {3932, 1141}, {3934, 1135}, {3936, 1129}, {3937, 1123}, {3939, 1117}, {3941, 1111}, {3943, 1105}, {3944, 1099}, {3946, 1092}, {3948, 1086}, {3949, 1080}, {3951, 1074}, {3953, 1068}, {3954, 1062}, {3956, 1056}, {3958, 1050}, {3959, 1044}, {3961, 1038}, {3962, 1032}, {3964, 1026}, {3965, 1020}, {3967, 1014}, {3969, 1007}, {3970, 1001}, {3972, 995},  {3973, 989},  {3975, 983},  {3976, 977},  {3978, 971},  {3979, 965},  {3981, 959},  {3982, 953},  {3984, 946},  {3985, 940},  {3987, 934},  {3988, 928},  {3989, 922},  {3991, 916},  {3992, 910},  {3994, 904},  {3995, 897},  {3996, 891},  {3998, 885},  {3999, 879},  {4001, 873},  {4002, 867},  {4003, 861},  {4005, 854},  {4006, 848},  {4007, 842},  {4008, 836},  {4010, 830},  {4011, 824},  {4012, 818},  {4014, 811},  {4015, 805},  {4016, 799},  {4017, 793},
    {4019, 787},  {4020, 781},  {4021, 774},  {4022, 768},  {4023, 762},  {4024, 756},  {4026, 750},  {4027, 744},  {4028, 737},  {4029, 731},  {4030, 725},  {4031, 719},  {4032, 713},  {4034, 706},  {4035, 700},  {4036, 694},  {4037, 688},  {4038, 682},  {4039, 675},  {4040, 669},  {4041, 663},  {4042, 657},  {4043, 651},  {4044, 644},  {4045, 638},  {4046, 632},  {4047, 626},  {4048, 620},  {4049, 613},  {4050, 607},  {4051, 601},  {4052, 595},  {4053, 589},  {4053, 582},  {4054, 576},  {4055, 570},  {4056, 564},  {4057, 557},  {4058, 551},  {4059, 545},  {4060, 539},  {4060, 533},  {4061, 526},  {4062, 520},  {4063, 514},  {4064, 508},  {4064, 501},  {4065, 495},  {4066, 489},  {4067, 483},  {4067, 476},  {4068, 470},  {4069, 464},  {4070, 458},  {4070, 451},  {4071, 445},  {4072, 439},  {4072, 433},  {4073, 426},  {4074, 420},  {4074, 414},  {4075, 408},  {4076, 401},  {4076, 395},  {4077, 389},  {4077, 383},  {4078, 376},  {4079, 370},  {4079, 364},
    {4080, 358},  {4080, 351},  {4081, 345},  {4081, 339},  {4082, 333},  {4082, 326},  {4083, 320},  {4083, 314},  {4084, 308},  {4084, 301},  {4085, 295},  {4085, 289},  {4086, 283},  {4086, 276},  {4087, 270},  {4087, 264},  {4088, 257},  {4088, 251},  {4088, 245},  {4089, 239},  {4089, 232},  {4089, 226},  {4090, 220},  {4090, 214},  {4090, 207},  {4091, 201},  {4091, 195},  {4091, 188},  {4092, 182},  {4092, 176},  {4092, 170},  {4092, 163},  {4093, 157},  {4093, 151},  {4093, 144},  {4093, 138},  {4094, 132},  {4094, 126},  {4094, 119},  {4094, 113},  {4094, 107},  {4095, 101},  {4095, 94},   {4095, 88},   {4095, 82},   {4095, 75},   {4095, 69},   {4095, 63},   {4096, 57},   {4096, 50},   {4096, 44},   {4096, 38},   {4096, 31},   {4096, 25},   {4096, 19},   {4096, 13},   {4096, 6},    {4096, 0},
};

void rsincos(sint32 angle, sint32 *sine, sint32 *cosine)
{
    uint32 phase = angle < 0 ? (0u - (uint32)angle) & 0xfffu : (uint32)angle & 0xfffu;
    const sint16 *pair = gte_sincos_quarter[phase & 0x3ffu];
    sint32 s = pair[0];
    sint32 c = pair[1];

    switch (phase >> 10)
    {
        case 0:
            *sine = s;
            *cosine = c;
            break;
        case 1:
            *sine = c;
            *cosine = -s;
            break;
        case 2:
            *sine = -s;
            *cosine = -c;
            break;
        default:
            *sine = -c;
            *cosine = s;
            break;
    }
    if (angle < 0)
        *sine = -*sine;
}

sint32 csin(sint32 a)
{
    return psyq_sine(a);
}

sint32 ccos(sint32 a)
{
    return psyq_sine((sint32)((uint32)a + 0x400u));
}

sint32 rsin(sint32 a)
{
    return csin(a);
}

sint32 rcos(sint32 a)
{
    return ccos(a);
}

static const sint16 psyq_atan_table[1025] = {0,   1,   2,   2,   3,   3,   4,   5,   5,   6,   7,   7,   8,   9,   9,   10,  10,  11,  12,  12,  13,  14,  14,  15,  16,  16,  17,  17,  18,  19,  19,  20,  21,  21,  22,  23,  23,  24,  24,  25,  26,  26,  27,  28,  28,  29,  30,  30,  31,  31,  32,  33,  33,  34,  35,  35,  36,  36,  37,  38,  38,  39,  40,  40,  41,  42,  42,  43,  43,  44,  45,  45,  46,  47,  47,  48,  49,  49,  50,  50,  51,  52,  52,  53,  54,  54,  55,  55,  56,  57,  57,  58,  59,  59,  60,  61,  61,  62,  62,  63,  64,  64,  65,  66,  66,  67,  67,  68,  69,  69,  70,  71,  71,  72,  73,  73,  74,  74,  75,  76,  76,  77,  78,  78,  79,  79,  80,  81,  81,  82,  83,  83,  84,  84,  85,  86,  86,  87,  88,  88,  89,  89,  90,  91,  91,  92,  93,  93,  94,  94,  95,  96,  96,  97,  98,  98,  99,  99,  100, 101, 101, 102, 103, 103, 104, 104, 105, 106, 106, 107, 107,
                                             108, 109, 109, 110, 111, 111, 112, 112, 113, 114, 114, 115, 116, 116, 117, 117, 118, 119, 119, 120, 120, 121, 122, 122, 123, 124, 124, 125, 125, 126, 127, 127, 128, 128, 129, 130, 130, 131, 131, 132, 133, 133, 134, 135, 135, 136, 136, 137, 138, 138, 139, 139, 140, 141, 141, 142, 142, 143, 144, 144, 145, 145, 146, 147, 147, 148, 148, 149, 150, 150, 151, 152, 152, 153, 153, 154, 155, 155, 156, 156, 157, 158, 158, 159, 159, 160, 161, 161, 162, 162, 163, 164, 164, 165, 165, 166, 166, 167, 168, 168, 169, 169, 170, 171, 171, 172, 172, 173, 174, 174, 175, 175, 176, 177, 177, 178, 178, 179, 180, 180, 181, 181, 182, 182, 183, 184, 184, 185, 185, 186, 187, 187, 188, 188, 189, 190, 190, 191, 191, 192, 192, 193, 194, 194, 195, 195, 196, 197, 197, 198, 198, 199, 199, 200, 201, 201, 202, 202, 203, 203, 204, 205, 205, 206, 206, 207, 207, 208, 209, 209, 210,
                                             210, 211, 211, 212, 213, 213, 214, 214, 215, 215, 216, 217, 217, 218, 218, 219, 219, 220, 221, 221, 222, 222, 223, 223, 224, 225, 225, 226, 226, 227, 227, 228, 228, 229, 230, 230, 231, 231, 232, 232, 233, 234, 234, 235, 235, 236, 236, 237, 237, 238, 239, 239, 240, 240, 241, 241, 242, 242, 243, 244, 244, 245, 245, 246, 246, 247, 247, 248, 248, 249, 250, 250, 251, 251, 252, 252, 253, 253, 254, 254, 255, 256, 256, 257, 257, 258, 258, 259, 259, 260, 260, 261, 262, 262, 263, 263, 264, 264, 265, 265, 266, 266, 267, 267, 268, 269, 269, 270, 270, 271, 271, 272, 272, 273, 273, 274, 274, 275, 275, 276, 276, 277, 278, 278, 279, 279, 280, 280, 281, 281, 282, 282, 283, 283, 284, 284, 285, 285, 286, 286, 287, 288, 288, 289, 289, 290, 290, 291, 291, 292, 292, 293, 293, 294, 294, 295, 295, 296, 296, 297, 297, 298, 298, 299, 299, 300, 300, 301, 301, 302, 302,
                                             303, 303, 304, 304, 305, 305, 306, 307, 307, 308, 308, 309, 309, 310, 310, 311, 311, 312, 312, 313, 313, 314, 314, 315, 315, 316, 316, 317, 317, 318, 318, 319, 319, 320, 320, 321, 321, 322, 322, 322, 323, 323, 324, 324, 325, 325, 326, 326, 327, 327, 328, 328, 329, 329, 330, 330, 331, 331, 332, 332, 333, 333, 334, 334, 335, 335, 336, 336, 337, 337, 338, 338, 339, 339, 340, 340, 340, 341, 341, 342, 342, 343, 343, 344, 344, 345, 345, 346, 346, 347, 347, 348, 348, 349, 349, 349, 350, 350, 351, 351, 352, 352, 353, 353, 354, 354, 355, 355, 356, 356, 356, 357, 357, 358, 358, 359, 359, 360, 360, 361, 361, 362, 362, 362, 363, 363, 364, 364, 365, 365, 366, 366, 367, 367, 368, 368, 368, 369, 369, 370, 370, 371, 371, 372, 372, 372, 373, 373, 374, 374, 375, 375, 376, 376, 377, 377, 377, 378, 378, 379, 379, 380, 380, 381, 381, 381, 382, 382, 383, 383, 384,
                                             384, 385, 385, 385, 386, 386, 387, 387, 388, 388, 388, 389, 389, 390, 390, 391, 391, 391, 392, 392, 393, 393, 394, 394, 395, 395, 395, 396, 396, 397, 397, 398, 398, 398, 399, 399, 400, 400, 401, 401, 401, 402, 402, 403, 403, 403, 404, 404, 405, 405, 406, 406, 406, 407, 407, 408, 408, 409, 409, 409, 410, 410, 411, 411, 411, 412, 412, 413, 413, 413, 414, 414, 415, 415, 416, 416, 416, 417, 417, 418, 418, 418, 419, 419, 420, 420, 420, 421, 421, 422, 422, 422, 423, 423, 424, 424, 425, 425, 425, 426, 426, 427, 427, 427, 428, 428, 429, 429, 429, 430, 430, 431, 431, 431, 432, 432, 432, 433, 433, 434, 434, 434, 435, 435, 436, 436, 436, 437, 437, 438, 438, 438, 439, 439, 440, 440, 440, 441, 441, 441, 442, 442, 443, 443, 443, 444, 444, 445, 445, 445, 446, 446, 446, 447, 447, 448, 448, 448, 449, 449, 450, 450, 450, 451, 451, 451, 452, 452, 453, 453, 453,
                                             454, 454, 454, 455, 455, 456, 456, 456, 457, 457, 457, 458, 458, 459, 459, 459, 460, 460, 460, 461, 461, 461, 462, 462, 463, 463, 463, 464, 464, 464, 465, 465, 465, 466, 466, 467, 467, 467, 468, 468, 468, 469, 469, 469, 470, 470, 471, 471, 471, 472, 472, 472, 473, 473, 473, 474, 474, 474, 475, 475, 476, 476, 476, 477, 477, 477, 478, 478, 478, 479, 479, 479, 480, 480, 480, 481, 481, 481, 482, 482, 483, 483, 483, 484, 484, 484, 485, 485, 485, 486, 486, 486, 487, 487, 487, 488, 488, 488, 489, 489, 489, 490, 490, 490, 491, 491, 491, 492, 492, 492, 493, 493, 493, 494, 494, 494, 495, 495, 495, 496, 496, 496, 497, 497, 497, 498, 498, 498, 499, 499, 499, 500, 500, 500, 501, 501, 501, 502, 502, 502, 503, 503, 503, 504, 504, 504, 505, 505, 505, 506, 506, 506, 507, 507, 507, 508, 508, 508, 509, 509, 509, 510, 510, 510, 511, 511, 511, 511, 512, 512};

sint32 ratan2(sint32 y, sint32 x)
{
    sint32 negative_x = x < 0;
    sint32 negative_y = y < 0;
    sint32 angle;
    sint32 ratio;

    if (negative_x)
        x = (sint32)(0u - (uint32)x);
    if (negative_y)
        y = (sint32)(0u - (uint32)y);
    if (x == 0 && y == 0)
        return 0;
    if (y < x)
    {
        ratio = ((uint32)y & 0x7FE00000u) != 0u ? y / (x >> 10) : (y << 10) / x;
        angle = psyq_atan_table[ratio];
    }
    else
    {
        ratio = ((uint32)x & 0x7FE00000u) != 0u ? x / (y >> 10) : (x << 10) / y;
        angle = 1024 - psyq_atan_table[ratio];
    }
    if (negative_x)
        angle = 2048 - angle;
    if (negative_y)
        angle = -angle;
    return angle;
}

MATRIX *RotMatrixZ(sint32 rotation, MATRIX *m)
{
    MATRIX temporary = {{{0, 0, 0}, {0, 0, 0}, {0, 0, 0}}, {0, 0, 0}};
    sint32 sine;
    sint32 cosine;

    if (!m)
        return m;
    sine = csin(rotation);
    cosine = ccos(rotation);
    temporary.m[0][0] = (sint16)cosine;
    temporary.m[0][1] = (sint16)-sine;
    temporary.m[1][0] = (sint16)sine;
    temporary.m[1][1] = (sint16)cosine;
    temporary.m[2][2] = ONE;
    return MulMatrix2(&temporary, m);
}

static uint32 vector_leading_zero_count(uint32 value)
{
    uint32 count = 0;

    if (value == 0u)
        return 32u;
    while ((value & 0x80000000u) == 0u)
    {
        value <<= 1;
        ++count;
    }
    return count;
}

static sint32 vector_normal_components(VECTOR *input, sint32 *x, sint32 *y, sint32 *z)
{
    SVECTOR source;
    VECTOR product;
    uint32 sum;
    uint32 leading;
    uint32 normalized;
    sint16 coefficient;
    sint32 shift;

    if (!input || !x || !y || !z)
        return 0;
    source.vx = (sint16)input->vx;
    source.vy = (sint16)input->vy;
    source.vz = (sint16)input->vz;
    source.pad = 0;
    sum = (uint32)((sint32)source.vx * source.vx + (sint32)source.vy * source.vy + (sint32)source.vz * source.vz);
    leading = vector_leading_zero_count(sum) & ~1u;
    normalized = leading < 24u ? sum >> (24u - leading) : sum << (leading - 24u);
    // Zero vectors use a defined coefficient instead of reading before the table
    coefficient = sum != 0u ? psyq_rsqrt_table[normalized - 64u] : 0;
    shift = (31 - (sint32)leading) >> 1;
    gte_gpf0(&source, coefficient, &product);
    product.vx = (sint32)source.vx * coefficient;
    product.vy = (sint32)source.vy * coefficient;
    product.vz = (sint32)source.vz * coefficient;
    if (sum == 0u)
    {
        *x = 0;
        *y = 0;
        *z = 0;
    }
    else
    {
        *x = product.vx >> shift;
        *y = product.vy >> shift;
        *z = product.vz >> shift;
    }
    return (sint32)sum;
}

sint32 VectorNormal(VECTOR *input, VECTOR *output)
{
    sint32 x;
    sint32 y;
    sint32 z;
    sint32 result;

    if (!output)
        return 0;
    result = vector_normal_components(input, &x, &y, &z);
    output->vx = x;
    output->vy = y;
    output->vz = z;
    return result;
}

sint32 VectorNormalS(VECTOR *input, SVECTOR *output)
{
    sint32 x;
    sint32 y;
    sint32 z;
    sint32 result;

    if (!output)
        return 0;
    result = vector_normal_components(input, &x, &y, &z);
    output->vx = (sint16)x;
    output->vy = (sint16)y;
    output->vz = (sint16)z;
    return result;
}

VECTOR *OuterProduct0(VECTOR *left, VECTOR *right, VECTOR *output)
{
    sint16 left_x;
    sint16 left_y;
    sint16 left_z;
    sint16 right_x;
    sint16 right_y;
    sint16 right_z;
    sint32 x;
    sint32 y;
    sint32 z;

    if (!left || !right || !output)
        return output;
    left_x = (sint16)left->vx;
    left_y = (sint16)left->vy;
    left_z = (sint16)left->vz;
    right_x = (sint16)right->vx;
    right_y = (sint16)right->vy;
    right_z = (sint16)right->vz;
    x = (sint32)((sint64)left_y * right_z - (sint64)left_z * right_y);
    y = (sint32)((sint64)left_z * right_x - (sint64)left_x * right_z);
    z = (sint32)((sint64)left_x * right_y - (sint64)left_y * right_x);
    output->vx = x;
    output->vy = y;
    output->vz = z;
    return output;
}

sint32 Lzc(sint32 value)
{
    uint32 bits = (uint32)value;
    sint32 count = 0;

    if (value < 0)
        bits = ~bits;
    while (count < 32 && (bits & 0x80000000u) == 0u)
    {
        bits <<= 1;
        ++count;
    }
    return count;
}

/* PsyQ-compatible GTE implementation. */
/* Types. */
/* Variables. */
static PsxGteSnapshot gte;
static uint32 gte_lzcs, gte_lzcr;
static uint16 native_gte_ofx_fraction, native_gte_ofy_fraction;
#if defined(LOCKSTEP_DEBUG) && defined(_MSC_VER)
extern __declspec(dllexport) const volatile PsxGteSnapshot *xport_lockstep_gte_state = &gte;
#endif

static MATRIX matrix_stack[20];

static sint32 matrix_stack_depth;
static uint32 matrix_stack_depth_address;
static uint32 matrix_stack_base_address;
static uint32 matrix_stack_capacity;

/* Functions. */
static sint32 mac_shift12(sint64 value)
{
    if (value >= 0)
        return (sint32)(value >> 12);
    return -(sint32)(((-value) + 0xfff) >> 12);
}

static sint32 clamp_ir(sint32 value, uint32 bit)
{
    if (value < -32768L)
    {
        gte.flag |= (sint32)bit;
        return -32768L;
    }
    if (value > 32767L)
    {
        gte.flag |= (sint32)bit;
        return 32767L;
    }
    return value;
}

static sint32 clamp_ir_unsigned(sint32 value, uint32 bit)
{
    if (value < 0)
    {
        gte.flag |= (sint32)bit;
        return 0;
    }
    if (value > 32767L)
    {
        gte.flag |= (sint32)bit;
        return 32767L;
    }
    return value;
}

static sint32 clamp_sxy(sint32 value, uint32 bit)
{
    if (value < -1024L)
    {
        gte.flag |= (sint32)bit;
        return -1024L;
    }
    if (value > 1023L)
    {
        gte.flag |= (sint32)bit;
        return 1023L;
    }
    return value;
}

static uint32 clamp_sz(sint32 value)
{
    if (value < 0)
    {
        gte.flag |= 1L << 18;
        return 0;
    }
    if (value > 65535L)
    {
        gte.flag |= 1L << 18;
        return 65535UL;
    }
    return (uint32)value;
}

static uint32 clamp_rgb(sint32 value, uint32 bit)
{
    if (value < 0)
    {
        gte.flag |= (sint32)bit;
        return 0;
    }
    if (value > 255)
    {
        gte.flag |= (sint32)bit;
        return 255u;
    }
    return (uint32)value;
}

static sint32 gte_sra4(sint32 value)
{
    if (value >= 0)
        return value >> 4;
    return -(sint32)(((sint64)0 - value + 15) >> 4);
}

static sint32 truncate_mac0(sint64 value)
{
    if (value < (-2147483647LL - 1LL))
        gte.flag |= 1L << 15;
    else if (value > 2147483647LL)
        gte.flag |= 1L << 16;
    return (sint32)(uint32)value;
}

static void push_rgb_from_mac(void)
{
    uint32 red = clamp_rgb(gte_sra4(gte.mac[0]), 1UL << 21);
    uint32 green = clamp_rgb(gte_sra4(gte.mac[1]), 1UL << 20);
    uint32 blue = clamp_rgb(gte_sra4(gte.mac[2]), 1UL << 19);

    gte.rgb[0] = gte.rgb[1];
    gte.rgb[1] = gte.rgb[2];
    gte.rgb[2] = red | (green << 8) | (blue << 16) | (gte.rgbc & 0xff000000u);
}

static void finish_flag(void)
{
    if ((uint32)gte.flag & 0x7f87e000UL)
        gte.flag |= (sint32)0x80000000UL;
}

VECTOR *OuterProduct12(VECTOR *left, VECTOR *right, VECTOR *output)
{
    sint16 lx;
    sint16 ly;
    sint16 lz;
    sint16 rx;
    sint16 ry;
    sint16 rz;

    if (!left || !right || !output)
        return output;
    lx = (sint16)left->vx;
    ly = (sint16)left->vy;
    lz = (sint16)left->vz;
    rx = (sint16)right->vx;
    ry = (sint16)right->vy;
    rz = (sint16)right->vz;
    gte.flag = 0;
    gte.ir[0] = rx;
    gte.ir[1] = ry;
    gte.ir[2] = rz;
    gte.mac[0] = mac_shift12((sint64)ly * rz - (sint64)lz * ry);
    gte.mac[1] = mac_shift12((sint64)lz * rx - (sint64)lx * rz);
    gte.mac[2] = mac_shift12((sint64)lx * ry - (sint64)ly * rx);
    gte.ir[0] = clamp_ir(gte.mac[0], 1UL << 24);
    gte.ir[1] = clamp_ir(gte.mac[1], 1UL << 23);
    gte.ir[2] = clamp_ir(gte.mac[2], 1UL << 22);
    output->vx = gte.mac[0];
    output->vy = gte.mac[1];
    output->vz = gte.mac[2];
    finish_flag();
    return output;
}

VECTOR *Square0(VECTOR *input, VECTOR *output)
{
    const sint32 components[3] = {input ? input->vx : 0, input ? input->vy : 0, input ? input->vz : 0};
    sint32 index;

    if (!input || !output)
        return output;
    gte.flag = 0;
    for (index = 0; index < 3; ++index)
    {
        gte.ir[index] = (sint16)components[index];
        gte.mac[index] = gte.ir[index] * gte.ir[index];
        gte.ir[index] = clamp_ir_unsigned(gte.mac[index], 1UL << (24 - index));
    }
    output->vx = gte.mac[0];
    output->vy = gte.mac[1];
    output->vz = gte.mac[2];
    finish_flag();
    return output;
}

VECTOR *Square12(VECTOR *input, VECTOR *output)
{
    const sint32 components[3] = {input ? input->vx : 0, input ? input->vy : 0, input ? input->vz : 0};
    sint32 index;

    if (!input || !output)
        return output;
    gte.flag = 0;
    for (index = 0; index < 3; ++index)
    {
        gte.ir[index] = (sint16)components[index];
        gte.mac[index] = gte.ir[index] * gte.ir[index];
        gte.ir[index] = clamp_ir_unsigned(gte.mac[index] >> 12, 1UL << (24 - index));
    }
    output->vx = gte.mac[0];
    output->vy = gte.mac[1];
    output->vz = gte.mac[2];
    finish_flag();
    return output;
}

void gte_gpf12(const SVECTOR *input, sint32 scale, VECTOR *output)
{
    const sint32 components[3] = {input ? input->vx : 0, input ? input->vy : 0, input ? input->vz : 0};
    sint32 index;

    if (!input || !output)
        return;
    gte.flag = 0;
    gte.ir0 = (sint16)scale;
    for (index = 0; index < 3; ++index)
    {
        gte.ir[index] = (sint16)components[index];
        gte.mac[index] = (sint32)(((sint64)gte.ir[index] * gte.ir0) >> 12);
        gte.ir[index] = clamp_ir(gte.mac[index], 1UL << (24 - index));
    }
    push_rgb_from_mac();
    output->vx = gte.ir[0];
    output->vy = gte.ir[1];
    output->vz = gte.ir[2];
    finish_flag();
}

void gte_gpf0(const SVECTOR *input, sint32 scale, VECTOR *output)
{
    const sint32 components[3] = {input ? input->vx : 0, input ? input->vy : 0, input ? input->vz : 0};
    sint32 index;

    if (!input || !output)
        return;
    gte.flag = 0;
    gte.ir0 = (sint16)scale;
    for (index = 0; index < 3; ++index)
    {
        gte.ir[index] = (sint16)components[index];
        gte.mac[index] = (sint32)((sint64)gte.ir[index] * gte.ir0);
        gte.ir[index] = clamp_ir(gte.mac[index], 1UL << (24 - index));
    }
    push_rgb_from_mac();
    output->vx = gte.mac[0];
    output->vy = gte.mac[1];
    output->vz = gte.mac[2];
    finish_flag();
}

void gte_gpl12(const SVECTOR *input, sint32 scale, VECTOR *output)
{
    const sint32 components[3] = {input ? input->vx : 0, input ? input->vy : 0, input ? input->vz : 0};
    const sint32 accumulated[3] = {gte.mac[0], gte.mac[1], gte.mac[2]};
    sint32 index;

    if (!input || !output)
        return;
    gte.flag = 0;
    gte.ir0 = (sint16)scale;
    for (index = 0; index < 3; ++index)
    {
        sint64 value;

        gte.ir[index] = (sint16)components[index];
        value = (sint64)accumulated[index] * 4096 + (sint64)gte.ir[index] * gte.ir0;
        gte.mac[index] = mac_shift12(value);
        gte.ir[index] = clamp_ir(gte.mac[index], 1UL << (24 - index));
    }
    push_rgb_from_mac();
    output->vx = gte.ir[0];
    output->vy = gte.ir[1];
    output->vz = gte.ir[2];
    finish_flag();
}

void gte_gpl0(const SVECTOR *input, sint32 scale, VECTOR *output)
{
    const sint32 components[3] = {input ? input->vx : 0, input ? input->vy : 0, input ? input->vz : 0};
    const sint32 accumulated[3] = {gte.mac[0], gte.mac[1], gte.mac[2]};
    sint32 index;

    if (!input || !output)
        return;
    gte.flag = 0;
    gte.ir0 = (sint16)scale;
    for (index = 0; index < 3; ++index)
    {
        gte.ir[index] = (sint16)components[index];
        gte.mac[index] = (sint32)((uint32)accumulated[index] + (uint32)((sint32)gte.ir[index] * gte.ir0));
        gte.ir[index] = clamp_ir(gte.mac[index], 1UL << (24 - index));
    }
    push_rgb_from_mac();
    output->vx = gte.mac[0];
    output->vy = gte.mac[1];
    output->vz = gte.mac[2];
    finish_flag();
}

uint32 LoadAverageShort12(SVECTOR *first, SVECTOR *second, sint32 first_weight, sint32 second_weight, SVECTOR *output)
{
    SVECTOR input;
    VECTOR result;
    uint32 returned;
    input.vx = first->vx;
    input.vy = first->vy;
    input.vz = first->vz;
    gte_gpf12(&input, first_weight, &result);
    returned = xport_gte_read_data(31u);
    input.vx = second->vx;
    input.vy = second->vy;
    input.vz = second->vz;
    gte_gpl12(&input, second_weight, &result);
    xport_store_le16((uint8 *)output, (uint16)result.vx);
    xport_store_le16((uint8 *)output + 2u, (uint16)result.vy);
    xport_store_le32((uint8 *)output + 4u, (uint32)result.vz);
    return returned;
}

static uint32 load_average_bytes(uint8 *first, uint8 *second, sint32 first_weight, sint32 second_weight, uint8 *output, uint32 count)
{
    SVECTOR input;
    VECTOR result;
    uint32 returned;
    input.vx = first[0];
    input.vy = first[1];
    input.vz = count == 3u ? first[2] : (sint16)xport_gte_read_data(11u);
    gte_gpf0(&input, first_weight, &result);
    returned = xport_gte_read_data(31u);
    input.vx = second[0];
    input.vy = second[1];
    input.vz = count == 3u ? second[2] : (sint16)xport_gte_read_data(11u);
    gte_gpl0(&input, second_weight, &result);
    output[0] = (uint8)(result.vx >> 12);
    output[1] = (uint8)(result.vy >> 12);
    if (count == 3u)
        output[2] = (uint8)(result.vz >> 12);
    return returned;
}

uint32 LoadAverageByte(uint8 *first, uint8 *second, sint32 first_weight, sint32 second_weight, uint8 *output)
{
    return load_average_bytes(first, second, first_weight, second_weight, output, 2u);
}

uint32 LoadAverageCol(uint8 *first, uint8 *second, sint32 first_weight, sint32 second_weight, uint8 *output)
{
    return load_average_bytes(first, second, first_weight, second_weight, output, 3u);
}

static MATRIX *multiply_matrix_rotation(MATRIX *source, MATRIX *input_matrix, MATRIX *destination)
{
    sint16 input[3][3];
    sint16 output[3][3];
    sint32 row;
    sint32 column;
    sint32 inner;
    uint32 first_word;
    uint32 fourth_word;
    uint32 second_word;
    uint32 third_word;
    sint32 fifth_word;

    memcpy(&gte.rotation, source, 16);
    {
        sint32 rt33 = source->m[2][2];
        memcpy((uint8 *)&gte.rotation + 16, &rt33, 4);
    }
    memcpy(input, input_matrix->m, sizeof(input));
    for (column = 0; column < 3; ++column)
    {
        gte.flag = 0;
        for (row = 0; row < 3; ++row)
            gte.v0[row] = input[row][column];
        for (row = 0; row < 3; ++row)
        {
            sint64 sum = 0;
            for (inner = 0; inner < 3; ++inner)
                sum += (sint64)gte.rotation.m[row][inner] * gte.v0[inner];
            gte.mac[row] = (sint32)(sum >> 12);
            gte.ir[row] = clamp_ir(gte.mac[row], 1UL << (24 - row));
            output[row][column] = (sint16)gte.ir[row];
        }
        finish_flag();
    }

    first_word = (uint16)output[0][0] | ((uint32)(uint16)output[0][1] << 16);
    fourth_word = (uint16)output[2][0] | ((uint32)(uint16)output[2][1] << 16);
    second_word = (uint16)output[0][2] | ((uint32)(uint16)output[1][0] << 16);
    third_word = (uint16)output[1][1] | ((uint32)(uint16)output[1][2] << 16);
    fifth_word = gte.ir[2];
    memcpy((uint8 *)destination, &first_word, 4);
    memcpy((uint8 *)destination + 12, &fourth_word, 4);
    memcpy((uint8 *)destination + 4, &second_word, 4);
    memcpy((uint8 *)destination + 8, &third_word, 4);
    memcpy((uint8 *)destination + 16, &fifth_word, 4);
    return destination;
}

MATRIX *MulMatrix0(MATRIX *left, MATRIX *right, MATRIX *destination)
{
    const uint8 *left_bytes = (const uint8 *)left;
    const uint8 *right_bytes = (const uint8 *)right;
    uint8 *output = (uint8 *)destination;
    uint32 words[5], column1[3], column2[3];
    uint32 first, second, third;
    words[0] = xport_load_le32(left_bytes + 0u);
    words[1] = xport_load_le32(left_bytes + 4u);
    words[2] = xport_load_le32(left_bytes + 8u);
    words[3] = xport_load_le32(left_bytes + 12u);
    words[4] = xport_load_le32(left_bytes + 16u);
    xport_gte_write_control(0u, words[0]);
    xport_gte_write_control(1u, words[1]);
    xport_gte_write_control(2u, words[2]);
    xport_gte_write_control(3u, words[3]);
    xport_gte_write_control(4u, words[4]);
    first = xport_load_le16(right_bytes);
    second = xport_load_le32(right_bytes + 4u);
    third = xport_load_le32(right_bytes + 12u);
    first |= second & 0xFFFF0000u;
    xport_gte_write_data(0u, first);
    xport_gte_write_data(1u, third);
    xport_gte_execute(0x486012u);
    first = xport_load_le16(right_bytes + 2u);
    second = xport_load_le32(right_bytes + 8u);
    third = (uint32)(sint32)(sint16)xport_load_le16(right_bytes + 14u);
    first |= second << 16;
    column1[0] = xport_gte_read_data(9u);
    column1[1] = xport_gte_read_data(10u);
    column1[2] = xport_gte_read_data(11u);
    xport_gte_write_data(0u, first);
    xport_gte_write_data(1u, third);
    xport_gte_execute(0x486012u);
    first = xport_load_le16(right_bytes + 4u);
    second = xport_load_le32(right_bytes + 8u);
    third = xport_load_le32(right_bytes + 16u);
    first |= second & 0xFFFF0000u;
    column2[0] = xport_gte_read_data(9u);
    column2[1] = xport_gte_read_data(10u);
    column2[2] = xport_gte_read_data(11u);
    xport_gte_write_data(0u, first);
    xport_gte_write_data(1u, third);
    xport_gte_execute(0x486012u);
    xport_store_le32(output, (column1[0] & 0xFFFFu) | (column2[0] << 16));
    xport_store_le32(output + 12u, (column1[2] & 0xFFFFu) | (column2[2] << 16));
    first = xport_gte_read_data(9u);
    second = xport_gte_read_data(10u);
    xport_store_le32(output + 4u, (first & 0xFFFFu) | (column1[1] << 16));
    xport_store_le32(output + 8u, (column2[1] & 0xFFFFu) | (second << 16));
    xport_store_le32(output + 16u, xport_gte_read_data(11u));
    return destination;
}

MATRIX *MulMatrix(MATRIX *left, MATRIX *right)
{
    if (!left || !right)
        return left;
    return multiply_matrix_rotation(left, right, left);
}

MATRIX *MulMatrix2(MATRIX *source, MATRIX *destination)
{
    if (!source || !destination)
        return destination;
    return multiply_matrix_rotation(source, destination, destination);
}

static void transform(const MATRIX *m, const SVECTOR *v, VECTOR *out, sint32 translate)
{
    sint64 x, y, z;
    x = (sint64)m->m[0][0] * v->vx + (sint64)m->m[0][1] * v->vy + (sint64)m->m[0][2] * v->vz;
    y = (sint64)m->m[1][0] * v->vx + (sint64)m->m[1][1] * v->vy + (sint64)m->m[1][2] * v->vz;
    z = (sint64)m->m[2][0] * v->vx + (sint64)m->m[2][1] * v->vy + (sint64)m->m[2][2] * v->vz;
    out->vx = mac_shift12(x) + (translate ? gte.translation[0] : 0);
    out->vy = mac_shift12(y) + (translate ? gte.translation[1] : 0);
    out->vz = mac_shift12(z) + (translate ? gte.translation[2] : 0);
    out->pad = 0;
}

void InitGeom(void)
{
    gte.zsf3 = 0x155;
    gte.zsf4 = 0x100;
    gte.h = 1000;
    gte.dqa = -4194;
    gte.dqb = 0x01400000;
    gte.ofx = 0;
    gte.ofy = 0;
    native_gte_ofx_fraction = 0u;
    native_gte_ofy_fraction = 0u;
}

void SetGeomOffset(sint32 x, sint32 y)
{
    gte.ofx = (sint16)(uint16)x;
    gte.ofy = (sint16)(uint16)y;
    native_gte_ofx_fraction = 0u;
    native_gte_ofy_fraction = 0u;
}

void SetGeomScreen(sint32 h)
{
    gte.h = (uint16)h;
}

/* Raw CTC2 H write, including zero, used by translated SDK entries. */
void gte_write_h(uint16 h)
{
    gte.h = h;
}

uint16 gte_read_h(void)
{
    return (uint16)gte.h;
}

void psx_gte_snapshot(PsxGteSnapshot *snapshot)
{
    if (snapshot)
        *snapshot = gte;
}

static void gte_import_matrix(MATRIX *matrix, const uint32 *registers)
{
    matrix->m[0][0] = (sint16)(uint16)registers[0];
    matrix->m[0][1] = (sint16)(uint16)(registers[0] >> 16);
    matrix->m[0][2] = (sint16)(uint16)registers[1];
    matrix->m[1][0] = (sint16)(uint16)(registers[1] >> 16);
    matrix->m[1][1] = (sint16)(uint16)registers[2];
    matrix->m[1][2] = (sint16)(uint16)(registers[2] >> 16);
    matrix->m[2][0] = (sint16)(uint16)registers[3];
    matrix->m[2][1] = (sint16)(uint16)(registers[3] >> 16);
    matrix->m[2][2] = (sint16)(uint16)registers[4];
    matrix->t[0] = 0;
    matrix->t[1] = 0;
    matrix->t[2] = 0;
}

sint32 psx_gte_import_control_registers(const uint32 registers[32])
{
    if (!registers)
        return 0;
    gte_import_matrix(&gte.rotation, registers);
    gte.translation[0] = (sint32)registers[5];
    gte.translation[1] = (sint32)registers[6];
    gte.translation[2] = (sint32)registers[7];
    gte_import_matrix(&gte.light, registers + 8);
    gte.back_color[0] = (sint32)registers[13];
    gte.back_color[1] = (sint32)registers[14];
    gte.back_color[2] = (sint32)registers[15];
    gte_import_matrix(&gte.color, registers + 16);
    gte.far_color[0] = (sint32)registers[21];
    gte.far_color[1] = (sint32)registers[22];
    gte.far_color[2] = (sint32)registers[23];
    gte.ofx = (sint32)registers[24] >> 16;
    gte.ofy = (sint32)registers[25] >> 16;
    native_gte_ofx_fraction = (uint16)registers[24];
    native_gte_ofy_fraction = (uint16)registers[25];
    gte.h = (sint32)(registers[26] & 0xffffu);
    gte.dqa = (sint16)(uint16)registers[27];
    gte.dqb = (sint32)registers[28];
    gte.zsf3 = (sint16)(uint16)registers[29];
    gte.zsf4 = (sint16)(uint16)registers[30];
    gte.flag = (sint32)registers[31];
    return 1;
}

void ReadGeomOffset(sint32 *x, sint32 *y)
{
    if (x)
        *x = gte.ofx;
    if (y)
        *y = gte.ofy;
}

sint32 ReadGeomScreen(void)
{
    return gte.h;
}

void SetRotMatrix(MATRIX *m)
{
    const uint8 *input = (const uint8 *)m;
    uint32 words[5];

    words[0] = xport_load_le32(input);
    words[1] = xport_load_le32(input + 4u);
    words[2] = xport_load_le32(input + 8u);
    words[3] = xport_load_le32(input + 12u);
    words[4] = xport_load_le32(input + 16u);
    xport_gte_write_control(0u, words[0]);
    xport_gte_write_control(1u, words[1]);
    xport_gte_write_control(2u, words[2]);
    xport_gte_write_control(3u, words[3]);
    xport_gte_write_control(4u, words[4]);
}

void SetLightMatrix(MATRIX *m)
{
    if (m)
        memcpy(gte.light.m, m->m, sizeof(gte.light.m));
}

void SetColorMatrix(MATRIX *m)
{
    const uint8 *input = (const uint8 *)m;
    uint32 words[5];

    words[0] = xport_load_le32(input);
    words[1] = xport_load_le32(input + 4u);
    words[2] = xport_load_le32(input + 8u);
    words[3] = xport_load_le32(input + 12u);
    words[4] = xport_load_le32(input + 16u);
    xport_gte_write_control(16u, words[0]);
    xport_gte_write_control(17u, words[1]);
    xport_gte_write_control(18u, words[2]);
    xport_gte_write_control(19u, words[3]);
    xport_gte_write_control(20u, words[4]);
}

void SetBackColor(sint32 red, sint32 green, sint32 blue)
{
    gte.back_color[0] = (sint32)((uint32)red << 4);
    gte.back_color[1] = (sint32)((uint32)green << 4);
    gte.back_color[2] = (sint32)((uint32)blue << 4);
}

void SetFarColor(sint32 red, sint32 green, sint32 blue)
{
    gte.far_color[0] = (sint32)((uint32)red << 4);
    gte.far_color[1] = (sint32)((uint32)green << 4);
    gte.far_color[2] = (sint32)((uint32)blue << 4);
}

void gte_set_back_color_raw(sint32 red, sint32 green, sint32 blue)
{
    gte.back_color[0] = red;
    gte.back_color[1] = green;
    gte.back_color[2] = blue;
}

void gte_set_far_color_raw(sint32 red, sint32 green, sint32 blue)
{
    gte.far_color[0] = red;
    gte.far_color[1] = green;
    gte.far_color[2] = blue;
}

void gte_write_ir0(sint32 value)
{
    gte.ir0 = value;
}

void gte_intpl12(const SVECTOR *input, const SVECTOR *far_color, sint32 factor, VECTOR *output)
{
    const sint32 components[3] = {input ? input->vx : 0, input ? input->vy : 0, input ? input->vz : 0};
    const sint32 far_components[3] = {far_color ? far_color->vx : 0, far_color ? far_color->vy : 0, far_color ? far_color->vz : 0};
    sint32 index;

    if (!input || !far_color || !output)
        return;
    gte.flag = 0;
    gte.ir0 = (sint16)factor;
    for (index = 0; index < 3; ++index)
    {
        sint64 value;

        gte.far_color[index] = far_components[index];
        gte.ir[index] = (sint16)components[index];
        value = (sint64)gte.ir[index] * 4096 + (sint64)gte.ir0 * (far_components[index] - gte.ir[index]);
        gte.mac[index] = mac_shift12(value);
        gte.ir[index] = clamp_ir(gte.mac[index], 1UL << (24 - index));
    }
    push_rgb_from_mac();
    output->vx = gte.ir[0];
    output->vy = gte.ir[1];
    output->vz = gte.ir[2];
    finish_flag();
}

void SetTransMatrix(MATRIX *m)
{
    const uint8 *input = (const uint8 *)m;
    uint32 x = xport_load_le32(input + 20u);
    uint32 y = xport_load_le32(input + 24u);
    uint32 z = xport_load_le32(input + 28u);

    xport_gte_write_control(5u, x);
    xport_gte_write_control(6u, y);
    xport_gte_write_control(7u, z);
}

void ReadRotMatrix(MATRIX *m)
{
    if (m)
    {
        memcpy(m, &gte.rotation, sizeof(MATRIX));
        m->t[0] = gte.translation[0];
        m->t[1] = gte.translation[1];
        m->t[2] = gte.translation[2];
    }
}

MATRIX *TransposeMatrix(MATRIX *source, MATRIX *destination)
{
    const uint8 *input = (const uint8 *)source;
    uint8 *output = (uint8 *)destination;
    uint32 first, second, third;
    first = xport_load_le32(input);
    second = xport_load_le32(input + 4u);
    xport_store_le32(output + 4u, first);
    xport_store_le32(output, second);
    xport_store_le16(output, (uint16)first);
    third = xport_load_le32(input + 8u);
    first = xport_load_le32(input + 12u);
    xport_store_le32(output + 12u, third);
    xport_store_le32(output + 8u, first);
    xport_store_le16(output + 12u, (uint16)second);
    xport_store_le16(output + 8u, (uint16)third);
    second = xport_load_le16(input + 16u);
    xport_store_le16(output + 4u, (uint16)first);
    xport_store_le16(output + 16u, (uint16)second);
    return destination;
}

void PushMatrix(void)
{
    if (matrix_stack_capacity)
    {
        uint32 offset = r_u32(matrix_stack_depth_address);
        MATRIX current = gte.rotation;
        uint32 packed_last_rotation = (uint32)(sint32)current.m[2][2];

        if (offset > matrix_stack_capacity * (uint32)sizeof(MATRIX) - (uint32)sizeof(MATRIX))
            return;
        memcpy((uint8 *)&current + 16u, &packed_last_rotation, sizeof(packed_last_rotation));
        current.t[0] = gte.translation[0];
        current.t[1] = gte.translation[1];
        current.t[2] = gte.translation[2];
        if (!xport_guest_copy(xport_guest_ref(matrix_stack_base_address + offset), xport_host_ref(&current), sizeof(current)) || !w_u32(matrix_stack_depth_address, offset + (uint32)sizeof(MATRIX)))
            return;
    }
    if (matrix_stack_depth < 20)
    {
        matrix_stack[matrix_stack_depth] = gte.rotation;
        matrix_stack[matrix_stack_depth].t[0] = gte.translation[0];
        matrix_stack[matrix_stack_depth].t[1] = gte.translation[1];
        matrix_stack[matrix_stack_depth].t[2] = gte.translation[2];
        matrix_stack_depth++;
    }
}

void PopMatrix(void)
{
    MATRIX *m;
    MATRIX guest;
    if (matrix_stack_capacity)
    {
        uint32 offset = r_u32(matrix_stack_depth_address);

        if (offset < sizeof(MATRIX) || offset > matrix_stack_capacity * (uint32)sizeof(MATRIX))
            return;
        offset -= (uint32)sizeof(MATRIX);
        if (!xport_guest_copy(xport_host_ref(&guest), xport_guest_ref(matrix_stack_base_address + offset), sizeof(guest)) || !w_u32(matrix_stack_depth_address, offset))
            return;
        SetRotMatrix(&guest);
        SetTransMatrix(&guest);
    }
    if (matrix_stack_depth <= 0)
        return;
    m = &matrix_stack[--matrix_stack_depth];
    SetRotMatrix(m);
    SetTransMatrix(m);
}

sint32 psx_gte_bind_matrix_stack(uint32 depth_address, uint32 base_address, uint32 capacity)
{
    if (!depth_address && !base_address && !capacity)
    {
        matrix_stack_depth_address = matrix_stack_base_address = matrix_stack_capacity = 0u;
        return 1;
    }
    if (!capacity || capacity > 1024u || !xport_guest_ptr(depth_address, 4u) || !xport_guest_ptr(base_address, (size_t)capacity * sizeof(MATRIX)))
        return 0;
    matrix_stack_depth_address = depth_address;
    matrix_stack_base_address = base_address;
    matrix_stack_capacity = capacity;
    matrix_stack_depth = 0;
    return 1;
}

VECTOR *ApplyMatrix(MATRIX *m, SVECTOR *v, VECTOR *out)
{
    if (m && v && out)
    {
        VECTOR result;

        gte.flag = 0;
        SetRotMatrix(m);
        gte.v0[0] = v->vx;
        gte.v0[1] = v->vy;
        gte.v0[2] = v->vz;
        transform(m, v, &result, 0);
        gte.mac[0] = result.vx;
        gte.mac[1] = result.vy;
        gte.mac[2] = result.vz;
        gte.ir[0] = clamp_ir(result.vx, 1UL << 24);
        gte.ir[1] = clamp_ir(result.vy, 1UL << 23);
        gte.ir[2] = clamp_ir(result.vz, 1UL << 22);
        out->vx = result.vx;
        out->vy = result.vy;
        out->vz = result.vz;
        finish_flag();
    }
    return out;
}

VECTOR *ApplyRotMatrix(SVECTOR *v, VECTOR *out)
{
    if (v && out)
    {
        VECTOR result;

        gte.flag = 0;
        gte.v0[0] = v->vx;
        gte.v0[1] = v->vy;
        gte.v0[2] = v->vz;
        transform(&gte.rotation, v, &result, 0);
        gte.mac[0] = result.vx;
        gte.mac[1] = result.vy;
        gte.mac[2] = result.vz;
        gte.ir[0] = clamp_ir(result.vx, 1UL << 24);
        gte.ir[1] = clamp_ir(result.vy, 1UL << 23);
        gte.ir[2] = clamp_ir(result.vz, 1UL << 22);
        out->vx = result.vx;
        out->vy = result.vy;
        out->vz = result.vz;
        out->pad = 0;
        finish_flag();
    }
    return out;
}

VECTOR *ApplyMatrixLV(MATRIX *m, VECTOR *v, VECTOR *out)
{
    sint32 high[3];
    sint32 low[3];
    sint32 input[3];
    sint32 result[3];
    sint32 row;
    sint32 column;
    if (!m || !v || !out)
        return out;

    input[0] = v->vx;
    input[1] = v->vy;
    input[2] = v->vz;
    for (column = 0; column < 3; ++column)
    {
        sint64 magnitude = input[column];
        sint32 sign = 1;
        if (magnitude < 0)
        {
            magnitude = -magnitude;
            sign = -1;
        }
        high[column] = sign * (sint32)(magnitude >> 15);
        low[column] = sign * (sint32)(magnitude & 0x7fff);
    }

    for (row = 0; row < 3; ++row)
    {
        sint64 high_mac = 0;
        sint64 low_mac = 0;
        uint32 combined;
        for (column = 0; column < 3; ++column)
        {
            high_mac += (sint64)m->m[row][column] * high[column];
            low_mac += (sint64)m->m[row][column] * low[column];
        }
        combined = ((uint32)high_mac << 3) + (uint32)mac_shift12(low_mac);
        result[row] = (sint32)combined;
    }

    out->vx = result[0];
    out->vy = result[1];
    out->vz = result[2];
    out->pad = 0;
    return out;
}

SVECTOR *ApplyMatrixSV(MATRIX *m, SVECTOR *v, SVECTOR *out)
{
    VECTOR q;
    if (!m || !v || !out)
        return out;
    transform(m, v, &q, 0);
    out->vx = (sint16)q.vx;
    out->vy = (sint16)q.vy;
    out->vz = (sint16)q.vz;
    out->pad = 0;
    return out;
}

static void matrix_identity(MATRIX *m)
{
    memset(m, 0, sizeof(*m));
    m->m[0][0] = m->m[1][1] = m->m[2][2] = ONE;
}

static void matrix_left_multiply(MATRIX *m, const MATRIX *left)
{
    MATRIX out;
    sint32 r, c;
    memset(&out, 0, sizeof(out));
    for (r = 0; r < 3; r++)
        for (c = 0; c < 3; c++)
            out.m[r][c] = (sint16)mac_shift12((sint64)left->m[r][0] * m->m[0][c] + (sint64)left->m[r][1] * m->m[1][c] + (sint64)left->m[r][2] * m->m[2][c]);
    out.t[0] = m->t[0];
    out.t[1] = m->t[1];
    out.t[2] = m->t[2];
    *m = out;
}

MATRIX *RotMatrixYXZ(SVECTOR *rotation, MATRIX *matrix)
{
    uint32 xy, product, a0, a1, a2, a3, v1, t0, t1, t2, t3, t4, t5, t6, t7, at;
    SVECTOR gpf_input;
    VECTOR gpf_output;
    sint32 sine, cosine, z;
    uint8 *output = (uint8 *)matrix;
    z = (sint16)xport_load_le16((uint8 *)rotation + 4u);
    xy = xport_load_le32(rotation);
    rsincos(z, &sine, &cosine);
    a2 = ((uint32)(uint16)cosine << 16) | (uint16)sine;
    rsincos((sint16)(xy >> 16), &sine, &cosine);
    a1 = ((uint32)(uint16)cosine << 16) | (uint16)sine;
    rsincos((sint16)xy, &sine, &cosine);
    a0 = ((uint32)(uint16)cosine << 16) | (uint16)sine;
    t0 = (uint32)(sint32)(sint16)(a1 >> 16);
    gte_write_ir0((sint32)t0);
    a3 = (uint32)(sint32)(sint16)a0;
    xport_gte_write_data(9u, a3);
    v1 = (uint32)(sint32)(sint16)a2;
    xport_gte_write_data(10u, v1);
    at = (uint32)(sint32)(sint16)(a2 >> 16);
    xport_gte_write_data(11u, at);
    gpf_input.vx = (sint16)a3;
    gpf_input.vy = (sint16)v1;
    gpf_input.vz = (sint16)at;
    gte_gpf12(&gpf_input, (sint32)t0, &gpf_output);
    at = (uint32)(sint32)(sint16)(a0 >> 16);
    product = at * t0;
    t0 = xport_gte_read_data(9u);
    t1 = xport_gte_read_data(10u);
    t2 = xport_gte_read_data(11u);
    t6 = (uint32)(sint32)(sint16)a1;
    gte_write_ir0((sint32)t6);
    xport_gte_write_data(9u, a3);
    xport_gte_write_data(10u, v1);
    at = (uint32)(sint32)(sint16)(a2 >> 16);
    xport_gte_write_data(11u, at);
    gpf_input.vx = (sint16)a3;
    gpf_input.vy = (sint16)v1;
    gpf_input.vz = (sint16)at;
    gte_gpf12(&gpf_input, (sint32)t6, &gpf_output);
    at = (product >> 12) | ((product & 0x80000000u) != 0u ? 0xFFF00000u : 0u);
    xport_store_le16(output + 16u, (uint16)at);
    t3 = xport_gte_read_data(9u);
    t4 = xport_gte_read_data(10u);
    t5 = xport_gte_read_data(11u);
    at = (uint32)(sint32)(sint16)(a2 >> 16);
    gte_write_ir0((sint32)at);
    t7 = at;
    at = (uint32)(sint32)(sint16)(a0 >> 16);
    xport_gte_write_data(9u, at);
    product = at * t6;
    xport_gte_write_data(10u, t3);
    xport_gte_write_data(11u, t0);
    gpf_input.vx = (sint16)at;
    gpf_input.vy = (sint16)t3;
    gpf_input.vz = (sint16)t0;
    gte_gpf12(&gpf_input, (sint32)t7, &gpf_output);
    a0 = xport_gte_read_data(9u);
    a1 = xport_gte_read_data(10u);
    a2 = xport_gte_read_data(11u);
    gte_write_ir0((sint32)v1);
    xport_gte_write_data(9u, at);
    xport_gte_write_data(10u, t3);
    xport_gte_write_data(11u, t0);
    gpf_input.vx = (sint16)at;
    gpf_input.vy = (sint16)t3;
    gpf_input.vz = (sint16)t0;
    gte_gpf12(&gpf_input, (sint32)v1, &gpf_output);
    a0 = (a0 & 0xFFFFu) | ((0u - a3) << 16);
    xport_store_le32(output + 8u, a0);
    at = xport_gte_read_data(9u);
    t1 = a1 - t1;
    t6 = xport_gte_read_data(10u);
    t7 = xport_gte_read_data(11u);
    t1 <<= 16;
    t2 = (t2 + t6) & 0xFFFFu;
    xport_store_le32(output, t1 | t2);
    t1 = ((product >> 12) | ((product & 0x80000000u) != 0u ? 0xFFF00000u : 0u)) & 0xFFFFu;
    xport_store_le32(output + 4u, (at << 16) | t1);
    t5 = (t7 - t5) & 0xFFFFu;
    t4 = (a2 + t4) << 16;
    xport_store_le32(output + 12u, t4 | t5);
    return matrix;
}

/* PsyQ RotMatrix order, carried over from de-NFS4's host runtime. */
MATRIX *RotMatrix(SVECTOR *r, MATRIX *m)
{
    sint32 sx, cx, sy, cy, sz, cz, nsy, czn, szn;
    if (!r || !m)
        return m;
    rsincos(r->vx, &sx, &cx);
    rsincos(r->vy, &sy, &cy);
    rsincos(r->vz, &sz, &cz);
    nsy = -sy;
    m->m[0][2] = (sint16)sy;
    m->m[1][2] = (sint16)((-(cy * sx)) >> 12);
    m->m[2][2] = (sint16)((cy * cx) >> 12);
    m->m[0][0] = (sint16)((cz * cy) >> 12);
    m->m[0][1] = (sint16)((-(sz * cy)) >> 12);
    czn = (cz * nsy) >> 12;
    m->m[1][0] = (sint16)((sz * cx) >> 12) - (sint16)((czn * sx) >> 12);
    m->m[2][0] = (sint16)((sz * sx) >> 12) + (sint16)((czn * cx) >> 12);
    szn = (sz * nsy) >> 12;
    m->m[1][1] = (sint16)((cz * cx) >> 12) + (sint16)((szn * sx) >> 12);
    m->m[2][1] = (sint16)((cz * sx) >> 12) - (sint16)((szn * cx) >> 12);
    return m;
}

static sint32 matrix_sra12(uint32 value)
{
    return (sint32)((value >> 12) | ((value & 0x80000000u) != 0u ? 0xFFF00000u : 0u));
}

MATRIX *RotMatrixX(sint32 angle, MATRIX *m)
{
    sint32 sine;
    sint32 cosine;
    sint16 row10;
    sint16 row20;
    sint16 row11;
    sint16 row21;
    sint16 row12;
    sint16 row22;

    if (!m)
        return m;
    rsincos(angle, &sine, &cosine);
    row10 = m->m[1][0];
    row20 = m->m[2][0];
    row11 = m->m[1][1];
    row21 = m->m[2][1];
    row12 = m->m[1][2];
    row22 = m->m[2][2];
    m->m[1][0] = (sint16)matrix_sra12((uint32)(cosine * row10) - (uint32)(sine * row20));
    m->m[1][1] = (sint16)matrix_sra12((uint32)(cosine * row11) - (uint32)(sine * row21));
    m->m[1][2] = (sint16)matrix_sra12((uint32)(cosine * row12) - (uint32)(sine * row22));
    m->m[2][0] = (sint16)matrix_sra12((uint32)(sine * row10) + (uint32)(cosine * row20));
    m->m[2][1] = (sint16)matrix_sra12((uint32)(sine * row11) + (uint32)(cosine * row21));
    m->m[2][2] = (sint16)matrix_sra12((uint32)(sine * row12) + (uint32)(cosine * row22));
    return m;
}

MATRIX *RotMatrixY(sint32 angle, MATRIX *m)
{
    sint32 sine;
    sint32 cosine;
    sint16 row00;
    sint16 row20;
    sint16 row01;
    sint16 row21;
    sint16 row02;
    sint16 row22;

    if (!m)
        return m;
    rsincos(angle, &sine, &cosine);
    row00 = m->m[0][0];
    row20 = m->m[2][0];
    row01 = m->m[0][1];
    row21 = m->m[2][1];
    row02 = m->m[0][2];
    row22 = m->m[2][2];
    m->m[0][0] = (sint16)matrix_sra12((uint32)(cosine * row00) + (uint32)(sine * row20));
    m->m[0][1] = (sint16)matrix_sra12((uint32)(cosine * row01) + (uint32)(sine * row21));
    m->m[0][2] = (sint16)matrix_sra12((uint32)(cosine * row02) + (uint32)(sine * row22));
    m->m[2][0] = (sint16)matrix_sra12((uint32)(-sine * row00) + (uint32)(cosine * row20));
    m->m[2][1] = (sint16)matrix_sra12((uint32)(-sine * row01) + (uint32)(cosine * row21));
    m->m[2][2] = (sint16)matrix_sra12((uint32)(-sine * row02) + (uint32)(cosine * row22));
    return m;
}

MATRIX *TransMatrix(MATRIX *m, VECTOR *v)
{
    if (m && v)
    {
        m->t[0] = v->vx;
        m->t[1] = v->vy;
        m->t[2] = v->vz;
    }
    return m;
}

MATRIX *ScaleMatrix(MATRIX *m, VECTOR *v)
{
    sint32 r;
    if (m && v)
        for (r = 0; r < 3; r++)
        {
            m->m[r][0] = (sint16)mac_shift12((sint64)m->m[r][0] * v->vx);
            m->m[r][1] = (sint16)mac_shift12((sint64)m->m[r][1] * v->vy);
            m->m[r][2] = (sint16)mac_shift12((sint64)m->m[r][2] * v->vz);
        }
    return m;
}

void gte_transform(const SVECTOR *v, VECTOR *out, sint32 *flag)
{
    VECTOR q;
    gte.flag = 0;
    gte.v0[0] = v->vx;
    gte.v0[1] = v->vy;
    gte.v0[2] = v->vz;
    transform(&gte.rotation, v, &q, 1);
    gte.mac[0] = q.vx;
    gte.mac[1] = q.vy;
    gte.mac[2] = q.vz;
    gte.ir[0] = clamp_ir(q.vx, 1UL << 24);
    gte.ir[1] = clamp_ir(q.vy, 1UL << 23);
    gte.ir[2] = clamp_ir(q.vz, 1UL << 22);
    out->vx = gte.ir[0];
    out->vy = gte.ir[1];
    out->vz = gte.ir[2];
    out->pad = 0;
    finish_flag();
    if (flag)
        *flag = gte.flag;
}

void gte_transform_light(const SVECTOR *v, VECTOR *out, sint32 *flag)
{
    sint32 component[3];
    sint32 row;

    gte.flag = 0;
    component[0] = v->vx;
    component[1] = v->vy;
    component[2] = v->vz;
    gte.v0[0] = v->vx;
    gte.v0[1] = v->vy;
    gte.v0[2] = v->vz;
    for (row = 0; row < 3; ++row)
    {
        sint64 value = (sint64)gte.light.m[row][0] * component[0] + (sint64)gte.light.m[row][1] * component[1] + (sint64)gte.light.m[row][2] * component[2];

        gte.mac[row] = (sint32)((uint32)mac_shift12(value) + (uint32)gte.back_color[row]);
        gte.ir[row] = clamp_ir(gte.mac[row], 1UL << (24 - row));
    }
    out->vx = gte.ir[0];
    out->vy = gte.ir[1];
    out->vz = gte.ir[2];
    out->pad = 0;
    finish_flag();
    if (flag)
        *flag = gte.flag;
}

void gte_rotate_packed(const uint32 registers[5], uint32 xy, uint32 z, sint32 output[3])
{
    sint32 row;
    gte.rotation.m[0][0] = (sint16)registers[0];
    gte.rotation.m[0][1] = (sint16)(registers[0] >> 16u);
    gte.rotation.m[0][2] = (sint16)registers[1];
    gte.rotation.m[1][0] = (sint16)(registers[1] >> 16u);
    gte.rotation.m[1][1] = (sint16)registers[2];
    gte.rotation.m[1][2] = (sint16)(registers[2] >> 16u);
    gte.rotation.m[2][0] = (sint16)registers[3];
    gte.rotation.m[2][1] = (sint16)(registers[3] >> 16u);
    gte.rotation.m[2][2] = (sint16)registers[4];
    gte.v0[0] = (sint16)xy;
    gte.v0[1] = (sint16)(xy >> 16u);
    gte.v0[2] = (sint16)z;
    gte.flag = 0;
    for (row = 0; row < 3; ++row)
    {
        sint64 value = (sint64)gte.rotation.m[row][0] * gte.v0[0] + (sint64)gte.rotation.m[row][1] * gte.v0[1] + (sint64)gte.rotation.m[row][2] * gte.v0[2];
        gte.mac[row] = mac_shift12(value);
        gte.ir[row] = clamp_ir(gte.mac[row], 1u << (24 - row));
    }
    finish_flag();
    output[0] = gte.ir[0];
    output[1] = gte.ir[1];
    output[2] = gte.ir[2];
}

void xport_gte_mvmva(uint32 command)
{
    sint32 input[3];
    sint32 row;
    uint32 translated;
    if (command != 0x480012u && command != 0x49E012u && command != 0x486012u)
        abort();
    translated = command == 0x480012u;
    for (row = 0; row < 3; ++row)
        input[row] = command == 0x49E012u ? (sint16)gte.ir[row] : (sint16)gte.v0[row];
    gte.flag = 0;
    for (row = 0; row < 3; ++row)
    {
        sint64 value = translated ? (sint64)gte.translation[row] * 4096 : 0;
        sint32 component;
        for (component = 0; component < 3; ++component)
        {
            value += (sint64)gte.rotation.m[row][component] * input[component];
            if (value > 0x7FFFFFFFFFFLL)
                gte.flag |= (sint32)(1u << (30 - row));
            if (value < -0x80000000000LL)
                gte.flag |= (sint32)(1u << (27 - row));
            uint64 wrapped = (uint64)value & 0xFFFFFFFFFFFULL;
            value = (sint64)(wrapped & 0x7FFFFFFFFFFULL);
            if ((wrapped & 0x80000000000ULL) != 0u)
                value -= 0x80000000000LL;
        }
        gte.mac[row] = mac_shift12(value);
        gte.ir[row] = clamp_ir(gte.mac[row], 1u << (24 - row));
    }
    finish_flag();
}

void xport_gte_write_control(uint32 index, uint32 value)
{
    if ((index >= 8u && index <= 12u) || (index >= 16u && index <= 20u))
    {
        MATRIX *matrix = index < 16u ? &gte.light : &gte.color;
        uint32 component = (index - (index < 16u ? 8u : 16u)) * 2u;
        matrix->m[component / 3u][component % 3u] = (sint16)value;
        if (component < 8u)
        {
            ++component;
            matrix->m[component / 3u][component % 3u] = (sint16)(value >> 16);
        }
        return;
    }
    switch (index)
    {
        case 0:
            gte.rotation.m[0][0] = (sint16)value;
            gte.rotation.m[0][1] = (sint16)(value >> 16);
            break;
        case 1:
            gte.rotation.m[0][2] = (sint16)value;
            gte.rotation.m[1][0] = (sint16)(value >> 16);
            break;
        case 2:
            gte.rotation.m[1][1] = (sint16)value;
            gte.rotation.m[1][2] = (sint16)(value >> 16);
            break;
        case 3:
            gte.rotation.m[2][0] = (sint16)value;
            gte.rotation.m[2][1] = (sint16)(value >> 16);
            break;
        case 4:
            gte.rotation.m[2][2] = (sint16)value;
            break;
        case 5:
        case 6:
        case 7:
            gte.translation[index - 5u] = (sint32)value;
            break;
        case 13:
        case 14:
        case 15:
            gte.back_color[index - 13u] = (sint32)value;
            break;
        case 21:
        case 22:
        case 23:
            gte.far_color[index - 21u] = (sint32)value;
            break;
        case 24:
            gte.ofx = (sint32)value >> 16;
            native_gte_ofx_fraction = (uint16)value;
            break;
        case 25:
            gte.ofy = (sint32)value >> 16;
            native_gte_ofy_fraction = (uint16)value;
            break;
        case 26:
            gte.h = (uint16)value;
            break;
        case 27:
            gte.dqa = (sint16)value;
            break;
        case 28:
            gte.dqb = (sint32)value;
            break;
        case 29:
            gte.zsf3 = (sint16)value;
            break;
        case 30:
            gte.zsf4 = (sint16)value;
            break;
        case 31:
            gte.flag = (sint32)(value & 0x7FFFF000u);
            if (value & 0x7F87E000u)
                gte.flag |= (sint32)0x80000000u;
            break;
        default:
            abort();
    }
}

void gte_transform_raw(const SVECTOR *v, VECTOR *out)
{
    transform(&gte.rotation, v, out, 1);
}

static uint32 gte_unr_divide(uint32 numerator, uint32 denominator)
{
    uint32 shift = 0;
    uint32 divisor;
    uint32 index;
    sint32 table_value;
    sint32 x;
    sint32 d;
    uint32 reciprocal;
    uint32 result;

    while ((denominator & 0x8000u) == 0u)
    {
        numerator <<= 1;
        denominator <<= 1;
        ++shift;
    }
    (void)shift;
    divisor = denominator | 0x8000u;
    index = ((divisor & 0x7fffu) + 0x40u) >> 7;
    table_value = (sint32)((0x40000u / (index + 0x100u) + 1u) / 2u) - 0x101;
    if (table_value < 0)
        table_value = 0;
    x = 0x101 + table_value;
    d = ((sint32)divisor * -x + 0x80) >> 8;
    reciprocal = (uint32)((x * (0x20000 + d) + 0x80) >> 8);
    result = (uint32)(((uint64)numerator * reciprocal + 0x8000u) >> 16);
    return result > 0x1ffffu ? 0x1ffffu : result;
}

static sint32 gte_project_depth_step(const SVECTOR *v, sint32 *sxy)
{
    VECTOR q;
    uint32 depth, quotient;
    sint32 ix, iy, sx, sy;

    transform(&gte.rotation, v, &q, 1);
    gte.mac[0] = q.vx;
    gte.mac[1] = q.vy;
    gte.mac[2] = q.vz;
    gte.ir[0] = clamp_ir(q.vx, 1UL << 24);
    gte.ir[1] = clamp_ir(q.vy, 1UL << 23);
    gte.ir[2] = clamp_ir(q.vz, 1UL << 22);
    ix = gte.ir[0];
    iy = gte.ir[1];
    depth = clamp_sz(q.vz);
    if (!depth || (uint32)gte.h >= depth * 2UL)
    {
        quotient = 0x1ffffUL;
        gte.flag |= 1L << 17;
    }
    else
        quotient = gte_unr_divide((uint32)gte.h, depth);
    sx = clamp_sxy(gte.ofx + (sint32)(((sint64)ix * quotient) >> 16), 1UL << 14);
    sy = clamp_sxy(gte.ofy + (sint32)(((sint64)iy * quotient) >> 16), 1UL << 13);
    gte.sxy[0] = gte.sxy[1];
    gte.sxy[1] = gte.sxy[2];
    gte.sxy[2] = (sint32)((uint16)sx | ((uint32)(uint16)sy << 16));
    gte.sz[0] = gte.sz[1];
    gte.sz[1] = gte.sz[2];
    gte.sz[2] = gte.sz[3];
    gte.sz[3] = depth;
    if (sxy)
        *sxy = gte.sxy[2];
    return (sint32)depth;
}

sint32 gte_project_full_depth(const SVECTOR *v, sint32 *sxy, sint32 *flag)
{
    sint32 depth;

    gte.flag = 0;
    gte.v0[0] = v->vx;
    gte.v0[1] = v->vy;
    gte.v0[2] = v->vz;
    depth = gte_project_depth_step(v, sxy);
    finish_flag();
    if (flag)
        *flag = gte.flag;
    return depth;
}

void gte_project3_full_depth(const SVECTOR input[3], sint32 screen_xy[3], sint32 depth[3], sint32 *flag)
{
    sint32 index;

    gte.flag = 0;
    gte.v0[0] = input[0].vx;
    gte.v0[1] = input[0].vy;
    gte.v0[2] = input[0].vz;
    gte.v1[0] = input[1].vx;
    gte.v1[1] = input[1].vy;
    gte.v1[2] = input[1].vz;
    gte.v2[0] = input[2].vx;
    gte.v2[1] = input[2].vy;
    gte.v2[2] = input[2].vz;
    for (index = 0; index < 3; ++index)
        depth[index] = gte_project_depth_step(&input[index], &screen_xy[index]);
    finish_flag();
    if (flag)
        *flag = gte.flag;
}

static sint32 gte_interpolate_component(sint32 first, sint32 second, uint32 flag_bit)
{
    sint32 difference = clamp_ir(second - first, flag_bit);
    sint64 value = ((sint64)first << 12) + (sint64)difference * gte.ir0;

    return (sint32)(value >> 12);
}

void gte_interpolate_project3(const SVECTOR first[3], const SVECTOR second[3], sint32 screen_xy[3], sint32 depth[3], sint32 *flag)
{
    SVECTOR interpolated[3];
    sint32 vertex;
    sint32 component;

    for (vertex = 0; vertex < 3; ++vertex)
    {
        const sint16 *first_component = &first[vertex].vx;
        const sint16 *second_component = &second[vertex].vx;
        sint16 *output_component = &interpolated[vertex].vx;

        gte.flag = 0;
        for (component = 0; component < 3; ++component)
        {
            gte.ir[component] = first_component[component];
            gte.mac[component] = gte_interpolate_component(gte.ir[component], second_component[component], 1UL << (24 - component));
            gte.ir[component] = clamp_ir(gte.mac[component], 1UL << (24 - component));
            output_component[component] = (sint16)gte.ir[component];
        }
        interpolated[vertex].pad = 0;
        finish_flag();
    }
    gte_project3_full_depth(interpolated, screen_xy, depth, flag);
}

sint32 gte_project(const SVECTOR *v, sint32 *sxy, sint32 *flag)
{
    return gte_project_full_depth(v, sxy, flag) >> 2;
}

static uint32 gte_normal_color_col_step(const SVECTOR *normal, uint32 color)
{
    sint32 component[3];
    sint32 light[3];
    sint32 lit[3];
    sint32 row;
    sint32 column;
    uint32 output;

    component[0] = normal->vx;
    component[1] = normal->vy;
    component[2] = normal->vz;
    for (row = 0; row < 3; ++row)
    {
        sint64 value = 0;
        for (column = 0; column < 3; ++column)
            value += (sint64)gte.light.m[row][column] * component[column];
        light[row] = mac_shift12(value);
        light[row] = clamp_ir_unsigned(light[row], 1UL << (24 - row));
    }
    for (row = 0; row < 3; ++row)
    {
        sint64 value = (sint64)gte.back_color[row] << 12;
        for (column = 0; column < 3; ++column)
            value += (sint64)gte.color.m[row][column] * light[column];
        lit[row] = mac_shift12(value);
        lit[row] = clamp_ir_unsigned(lit[row], 1UL << (24 - row));
    }
    for (row = 0; row < 3; ++row)
    {
        sint32 source = (sint32)((color >> (row * 8)) & 0xffu);
        sint32 value = mac_shift12((sint64)source * lit[row] * 16);
        gte.mac[row] = value;
        gte.ir[row] = clamp_ir_unsigned(value, 1UL << (24 - row));
    }
    push_rgb_from_mac();
    output = gte.rgb[2];
    return output;
}

uint32 gte_normal_color_col(const SVECTOR *normal, uint32 color)
{
    uint32 output;

    gte.flag = 0;
    gte.v0[0] = normal->vx;
    gte.v0[1] = normal->vy;
    gte.v0[2] = normal->vz;
    gte.rgbc = color;
    output = gte_normal_color_col_step(normal, color);
    finish_flag();
    return output;
}

uint32 gte_normal_color_col_depth(const SVECTOR *normal, uint32 color)
{
    sint32 component[3];
    sint32 lit[3];
    sint32 row;
    sint32 column;

    (void)gte_normal_color_col(normal, color);
    component[0] = gte.ir[0];
    component[1] = gte.ir[1];
    component[2] = gte.ir[2];
    gte.flag = 0;
    for (row = 0; row < 3; ++row)
    {
        sint64 value = (sint64)gte.back_color[row] << 12;

        for (column = 0; column < 3; ++column)
            value += (sint64)gte.color.m[row][column] * component[column];
        lit[row] = mac_shift12(value);
        lit[row] = clamp_ir_unsigned(lit[row], 1UL << (24 - row));
    }
    for (row = 0; row < 3; ++row)
    {
        sint64 base = (sint64)((color >> (row * 8)) & 0xffu) * lit[row] * 16;
        sint32 difference = mac_shift12(((sint64)gte.far_color[row] << 12) - base);
        sint64 value;

        difference = clamp_ir(difference, 1UL << (24 - row));
        value = base + (sint64)difference * gte.ir0;
        gte.mac[row] = mac_shift12(value);
        gte.ir[row] = clamp_ir_unsigned(gte.mac[row], 1UL << (24 - row));
    }
    push_rgb_from_mac();
    finish_flag();
    return gte.rgb[2];
}

void gte_normal_color_col3(const SVECTOR normals[3], uint32 color, uint32 output[3])
{
    sint32 index;

    gte.flag = 0;
    gte.v0[0] = normals[0].vx;
    gte.v0[1] = normals[0].vy;
    gte.v0[2] = normals[0].vz;
    gte.v1[0] = normals[1].vx;
    gte.v1[1] = normals[1].vy;
    gte.v1[2] = normals[1].vz;
    gte.v2[0] = normals[2].vx;
    gte.v2[1] = normals[2].vy;
    gte.v2[2] = normals[2].vz;
    gte.rgbc = color;
    for (index = 0; index < 3; ++index)
        output[index] = gte_normal_color_col_step(&normals[index], color);
    finish_flag();
}

void RotTrans(SVECTOR *v, VECTOR *out, sint32 *flag)
{
    gte_transform(v, out, flag);
}

void RotTransSV(SVECTOR *v, SVECTOR *out, sint32 *flag)
{
    VECTOR q;
    gte_transform(v, &q, flag);
    if (out)
    {
        out->vx = (sint16)q.vx;
        out->vy = (sint16)q.vy;
        out->vz = (sint16)q.vz;
        out->pad = 0;
    }
}

sint32 RotTransPers(SVECTOR *v, sint32 *sxy, sint32 *p, sint32 *flag)
{
    sint32 z = gte_project(v, sxy, flag);
    if (p)
        *p = 0;
    return z;
}

sint32 NormalClip(sint32 a, sint32 b, sint32 c)
{
    sint32 x0 = (sint16)a, y0 = (sint16)((uint32)a >> 16), x1 = (sint16)b, y1 = (sint16)((uint32)b >> 16), x2 = (sint16)c, y2 = (sint16)((uint32)c >> 16);
    sint64 value;

    gte.flag = 0;
    gte.sxy[0] = a;
    gte.sxy[1] = b;
    gte.sxy[2] = c;
    value = (sint64)x0 * y1 + (sint64)x1 * y2 + (sint64)x2 * y0 - (sint64)x0 * y2 - (sint64)x1 * y0 - (sint64)x2 * y1;
    gte.mac0 = truncate_mac0(value);
    finish_flag();
    return gte.mac0;
}

sint32 AverageZ3(sint32 a, sint32 b, sint32 c)
{
    sint64 value;

    gte.flag = 0;
    gte.sz[1] = (uint32)a & 0xffffu;
    gte.sz[2] = (uint32)b & 0xffffu;
    gte.sz[3] = (uint32)c & 0xffffu;
    value = (sint64)gte.zsf3 * (sint32)(gte.sz[1] + gte.sz[2] + gte.sz[3]);
    gte.mac0 = truncate_mac0(value);
    gte.otz = clamp_sz((sint32)(value >> 12));
    finish_flag();
    return (sint32)gte.otz;
}

sint32 AverageZ4(sint32 a, sint32 b, sint32 c, sint32 d)
{
    sint64 value;

    gte.flag = 0;
    gte.sz[0] = (uint32)a & 0xffffu;
    gte.sz[1] = (uint32)b & 0xffffu;
    gte.sz[2] = (uint32)c & 0xffffu;
    gte.sz[3] = (uint32)d & 0xffffu;
    value = (sint64)gte.zsf4 * (sint32)(gte.sz[0] + gte.sz[1] + gte.sz[2] + gte.sz[3]);
    gte.mac0 = truncate_mac0(value);
    gte.otz = clamp_sz((sint32)(value >> 12));
    finish_flag();
    return (sint32)gte.otz;
}

static void psx_native_vblank_poll(void)
{
    uint64 now, elapsed, fields;
    uint32 present_field;
    if (native_vblank_polling || vsync_sample || vblank_wait)
        return;
    native_vblank_polling = 1u;
    xport_poll();
    now = xport_timer_get();
    if (!native_vblank_clock_started)
    {
        native_vblank_last_us = now;
        native_vblank_clock_started = 1u;
        native_vblank_polling = 0u;
        return;
    }
    elapsed = now >= native_vblank_last_us ? now - native_vblank_last_us : 0u;
    native_vblank_last_us = now;
    fields = elapsed / 1000000u * FIELD_RATE;
    native_vblank_fraction += elapsed % 1000000u * FIELD_RATE;
    fields += native_vblank_fraction / 1000000u;
    native_vblank_fraction %= 1000000u;
    present_field = fields != 0u;
    while (fields && !vblank_wait_failed)
    {
        --fields;
        psx_vblank_signal();
    }
    if (present_field && !vblank_wait_failed)
        gpu_present();
    native_vblank_polling = 0u;
}

void psx_native_poll_events(void)
{
    bios_native_irq_dispatch();
    psx_root_counter_poll();
    psx_native_vblank_poll();
}

sint32 VSync(sint32 mode)
{
    uint32 frame;
    uint32 frames;
    bios_native_irq_dispatch();
    psx_root_counter_poll();
    if (vblank_wait_failed)
        return -1;
    if (vsync_sample)
        return psx_vsync_sdk(mode);
    psx_native_vblank_poll();
    if (vblank_wait_failed)
        return -1;
    if (mode < 0)
        return (sint32)vblank_count;
    if (mode == 1)
        return 0;
    frames = mode > 1 ? (uint32)mode : 1;
    for (frame = 0; frame < frames; ++frame)
    {
        if (vblank_wait)
        {
            uint32 before = vblank_count;
            if (!vblank_wait(vblank_wait_context) || vblank_wait_failed || vblank_count - before != 1u)
            {
                vblank_wait_failed = 1;
                return -1;
            }
            continue;
        }
        {
            uint32 before = vblank_count;
            do
            {
                xport_timer_wait_frame(FIELD_RATE);
                psx_native_vblank_poll();
            } while (vblank_count == before && !vblank_wait_failed);
        }
        if (vblank_wait_failed)
            return -1;
    }
    return (sint32)frames;
}

static int psx_state_block(FILE *file, void *data, size_t size, int load)
{
    uint32 stored = (uint32)size;
    if (load)
        return fread(&stored, 4, 1, file) == 1 && stored == size && fread(data, 1, size, file) == size;
    return fwrite(&stored, 4, 1, file) == 1 && fwrite(data, 1, size, file) == size;
}

int psx_state_io(FILE *f, int load)
{
    PsxTimerState restored_timers;
    if (!psx_state_block(f, &gte, sizeof(gte), load) || !psx_state_block(f, &matrix_stack, sizeof(matrix_stack), load) || !psx_state_block(f, &matrix_stack_depth, sizeof(matrix_stack_depth), load) || !psx_state_block(f, &vblank_count, sizeof(vblank_count), load))
        return 0;
    if (load)
    {
        if (!psx_state_block(f, &restored_timers, sizeof(restored_timers), 1) || !psx_timers_import(&restored_timers))
            return 0;
    }
    else if (!psx_state_block(f, &timers, sizeof(timers), 0))
        return 0;
    return pad_state_io(f, load);
}

/* Pad services */
#include <string.h>
static uint8 *pad_packets[2];
static uint8 *pad_actuators[2];
static uint8 pad_actuator_lengths[2];
static uint8 pad_alignment[2][6];
static sint32 pad_active;
static sint32 pad_main_mode[2];
static uint8 pad_current_id[2];
static uint32 native_pad_packet_owners[2];
static uint32 pad_buttons;
static sint32 pad_replay_bound;
static sint32 pad_replay_failure;
static uint32 bios_pad_initialized;
static uint32 bios_pad_clear_mode;

sint32 pad_bind_replay(const PsxPadReplayBinding *binding)
{
    uint32 port;
    uint8 *buffers[2];
    if (!binding)
    {
        pad_replay_bound = pad_replay_failure = 0;
        return 1;
    }
    if (binding->active > 1u)
        return 0;
    for (port = 0u; port < 2u; ++port)
    {
        uint32 address = binding->buffers[port];
        uint32 segment = address & 0xe0000000u;
        uint32 physical = address & 0x1fffffffu;
        if ((segment && segment != 0x80000000u && segment != 0xa0000000u) || physical > PSX_DRAM_SIZE - 34u || binding->main_modes[port] < 0 || binding->main_modes[port] > 1)
            return 0;
        buffers[port] = DRAM + physical;
    }
    if ((binding->buffers[0] & 0x1fffffffu) < (binding->buffers[1] & 0x1fffffffu) + 34u && (binding->buffers[1] & 0x1fffffffu) < (binding->buffers[0] & 0x1fffffffu) + 34u)
        return 0;
    for (port = 0u; port < 2u; ++port)
    {
        pad_packets[port] = buffers[port];
        pad_main_mode[port] = binding->main_modes[port];
    }
    pad_active = (sint32)binding->active;
    pad_replay_bound = 1;
    pad_replay_failure = 0;
    return 1;
}

sint32 pad_publish_replay(sint32 controller, const uint8 *report, uint32 length)
{
    if (pad_replay_failure || !pad_replay_bound || !pad_active || controller < 0 || controller >= 2 || !report || length < 2u || !pad_packets[controller])
        goto failed;
    if (report[0] != 0u || !((report[1] == 0x41u && length == 4u) || (report[1] == 0x73u && length == 8u)))
        goto failed;
    memmove(pad_packets[controller], report, length);
    pad_current_id[controller] = report[1] >> 4;
    return 1;
failed:
    pad_replay_failure = 1;
    return 0;
}

sint32 pad_replay_failed(void)
{
    return pad_replay_failure;
}

sint32 pad_lockstep_apply(sint32 controller, uint32 type, uint32 buttons, uint32 analog)
{
    uint8 report[8];
    if (controller < 0 || controller >= 2 || type > 2u || buttons > 0xffffu)
        return 0;
    if (controller == 0)
        xport_input_override(1, buttons);
    if (!pad_packets[controller])
        return type != 2u;
    if (!type)
    {
        memset(pad_packets[controller], 0, 8);
        pad_packets[controller][0] = 0xffu;
        return 1;
    }
    report[0] = 0u;
    report[1] = type == 2u ? 0x73u : 0x41u;
    report[2] = (uint8)~buttons;
    report[3] = (uint8)(~buttons >> 8);
    if (type == 2u)
    {
        report[4] = (uint8)analog;
        report[5] = (uint8)(analog >> 8);
        report[6] = (uint8)(analog >> 16);
        report[7] = (uint8)(analog >> 24);
    }
    memmove(pad_packets[controller], report, type == 2u ? 8u : 4u);
    pad_current_id[controller] = report[1] >> 4;
    return 1;
}

static sint32 pad_index(sint32 port)
{
    return (port >> 4) & 1;
}

void pad_publish(sint32 controller, sint32 connected, uint16 buttons)
{
    uint8 *packet;
    if (pad_replay_bound)
        return;
    if (controller < 0 || controller >= 2)
        return;
    packet = pad_packets[controller];
    if (packet == 0)
        return;
    memset(packet, 0xff, 8);
    if (!connected)
        return;
    packet[0] = 0;
    packet[1] = 0x41;
    pad_current_id[controller] = 4u;
    packet[2] = (uint8)buttons;
    packet[3] = (uint8)(buttons >> 8);
}

void PadInit(sint32 mode)
{
    if (pad_replay_bound)
    {
        pad_replay_failure = 1;
        return;
    }
    if (mode == 0)
    {
        pad_buttons = 0;
        xport_input_init();
    }
}

uint32 PadRead(sint32 controller)
{
    if (pad_replay_bound)
    {
        pad_replay_failure = 1;
        return 0;
    }
    pad_buttons = xport_input_read(controller);
    return pad_buttons;
}

void PadInitDirect(uint8 *pad1, uint8 *pad2)
{
    if (pad_replay_bound)
    {
        pad_replay_failure = 1;
        return;
    }
    pad_packets[0] = pad1;
    pad_packets[1] = pad2;
    memset(pad_current_id, 0, sizeof(pad_current_id));
    bios_pad_initialized = 1u;
    if (pad1)
    {
        pad1[0] = 0xFFu;
        pad1[1] = 0u;
    }
    if (pad2)
    {
        pad2[0] = 0xFFu;
        pad2[1] = 0u;
    }
}

typedef struct
{
    uint8 phase;
    uint8 received;
    uint8 acknowledged;
    uint8 ready;
} NativePadSerial;

static NativePadSerial native_pad_serial[2];

uint32 psx_pad_serial_read(uint32 port)
{
    if (port >= 2u)
        abort();
    return native_pad_serial[port].ready ? native_pad_serial[port].received : 0xFFu;
}

uint32 psx_pad_serial_ready(uint32 port)
{
    if (port >= 2u)
        abort();
    return native_pad_serial[port].ready ? 2u : 0u;
}

uint32 psx_pad_serial_release_ack(uint32 port)
{
    if (port >= 2u)
        abort();
    /* Synchronous transfer has already completed the finite ACK pulse */
    return 1u;
}

static void native_pad_serial_send(uint32 port, uint8 command)
{
    NativePadSerial *serial = &native_pad_serial[port];
    uint16 buttons = (uint16)~xport_input_read((sint32)port);
    serial->received = 0xFFu;
    serial->ready = 1u;
    serial->acknowledged = 0u;
    switch (serial->phase)
    {
        case 0:
            if (command == 1u)
            {
                serial->phase = 1u;
                serial->acknowledged = 1u;
            }
            break;
        case 1:
            if (command == 0x42u)
            {
                serial->received = 0x41u;
                serial->phase = 2u;
                serial->acknowledged = 1u;
            }
            break;
        case 2:
            serial->received = 0x5Au;
            serial->phase = 3u;
            serial->acknowledged = 1u;
            break;
        case 3:
            serial->received = (uint8)(buttons >> 8);
            serial->phase = 4u;
            serial->acknowledged = 1u;
            break;
        case 4:
            serial->received = (uint8)buttons;
            serial->phase = 0u;
            break;
        default:
            abort();
    }
}

sint32 psx_pad_serial_transfer(uint32 owner, sint32 value, uint32 port, uint32 skip_initial, sint32 timeout_error)
{
    NativePadSerial *serial;
    uint32 previous, offset;
    if (port >= 2u || (timeout_error != -2 && timeout_error != -20))
        abort();
    serial = &native_pad_serial[port];
    previous = psx_pad_serial_read(port);
    if (value < 0)
    {
        uint8 command = (uint8) ~(uint32)value;
        w_u8(owner + 68u, 0xFFu);
        w_u8(owner + 69u, 1u);
        w_u8(r_u32(owner + 64u), command);
        serial->phase = 0u;
        native_pad_serial_send(port, command);
        return (sint32)previous;
    }
    if (!serial->ready || !serial->acknowledged)
        return timeout_error;
    native_pad_serial_send(port, (uint8)value);
    offset = r_u8(owner + 68u);
    w_u8(owner + 69u, (uint8)(r_u8(owner + 69u) + 1u));
    if (!skip_initial || offset != 0xFFu)
        w_u8(r_u32(owner + 60u) + offset, (uint8)previous);
    w_u8(owner + 68u, (uint8)(offset + 1u));
    return (sint32)previous;
}

void psx_pad_bind_owner_packets(uint32 owner0, uint32 owner1)
{
    if ((owner0 && !psx_addr(owner0, 240u)) || (owner1 && !psx_addr(owner1, 240u)))
        abort();
    native_pad_packet_owners[0] = owner0;
    native_pad_packet_owners[1] = owner1;
}

static void bios_native_pad_poll(void)
{
    if (!pad_replay_bound && pad_active)
    {
        uint32 port;
        for (port = 0u; port < 2u; ++port)
        {
            uint32 owner = native_pad_packet_owners[port];
            uint32 packet;
            uint16 owner_buttons = (uint16)~xport_input_read((sint32)port);
            // Both keyboard layouts are connected digital controllers
            pad_publish((sint32)port, 1, (uint16)((owner_buttons >> 8) | (owner_buttons << 8)));
            if (!owner)
                continue;
            packet = r_u32(owner + 48u);
            if (!packet)
                continue;
            w_u8(packet, 0u);
            w_u8(packet + 1u, 0x41u);
            w_u8(packet + 2u, (uint8)(owner_buttons >> 8));
            w_u8(packet + 3u, (uint8)owner_buttons);
        }
    }
}

void PadStartCom(void)
{
    pad_active = 1;
}

void PadStopCom(void)
{
    pad_active = 0;
}

sint32 PadGetState(sint32 port)
{
    uint32 owner = native_pad_packet_owners[((uint32)port & 0xF0u) != 0u];
    sint32 index;
    if (owner)
    {
        uint32 error = r_u32(owner + 0x34u) & 0xFFFF0000u;
        uint32 state;
        if (!error && r_u32(owner + 0x10u) != owner)
            error = r_u8(owner + 0x38u);
        if (!error)
            error = r_u8(r_u32(owner + 0x30u));
        state = r_u8(owner + 0x49u);
        if (error)
        {
            if (state == 2u || state == 3u)
                return 1;
            if (state == 6u)
                return 4;
        }
        return (sint32)state;
    }
    if (port != 0 && port != 0x10)
        return 0;
    index = pad_index(port);
    return pad_active && pad_packets[index] && pad_packets[index][0] == 0u ? 6 : 0;
}

sint32 PadInfoMode(sint32 port, sint32 term, sint32 offset)
{
    uint32 index = ((uint32)port & 0xF0u) != 0u;
    uint32 owner = native_pad_packet_owners[index];
    if (owner)
    {
        switch (term)
        {
            case 1:
                return (sint32)r_u8(owner + 0xE8u);
            case 2:
                return (sint32)r_u16(owner + 0xE6u);
            case 3:
                return (sint32)r_u8(owner + 0xE4u);
            case 4:
            {
                uint32 count = r_u8(owner + 0xE3u);
                if (offset < 0)
                    return (sint32)count;
                if ((uint32)offset >= count)
                    return 0;
                return (sint32)r_u16(r_u32(owner) + (uint32)offset * 2u);
            }
            case 100:
                return (sint32)r_u32(owner + 0x4Cu);
            default:
                return 0;
        }
    }
    switch (term)
    {
        case 1:
            return (sint32)pad_current_id[index];
        case 3:
            return pad_main_mode[index];
        default:
            return 0;
    }
}

void PadSetAct(sint32 port, uint8 *actuator, sint32 length)
{
    uint32 index = ((uint32)port & 0xF0u) != 0u;
    pad_actuators[index] = actuator;
    pad_actuator_lengths[index] = (uint8)length;
}

sint32 PadSetActAlign(sint32 port, uint8 *alignment)
{
    sint32 index = pad_index(port);
    if (PadGetState(port) == 0 || (pad_packets[index][1] >> 4) == 4u)
        return 0;
    if (alignment == 0)
        return 0;
    memcpy(pad_alignment[index], alignment, sizeof(pad_alignment[index]));
    return 1;
}

static uint32 pad_main_mode_send_target;
static uint32 pad_main_mode_receive_target;

void psx_pad_bind_main_mode_commands(uint32 send_target, uint32 receive_target)
{
    if ((send_target & 3u) || (receive_target & 3u) || (!send_target != !receive_target))
        abort();
    pad_main_mode_send_target = send_target;
    pad_main_mode_receive_target = receive_target;
}

static uint32 native_pad_main_mode_send(uint32 owner)
{
    uint8 phase = r_u8(owner + 0x46u);
    if (phase == 2u)
    {
        w_u8(owner + 0x36u, 0x44u);
        w_u32(owner + 0x2Cu, owner + 0x51u);
        w_u8(owner + 0x35u, phase);
        return owner + 0x51u;
    }
    if (phase == 3u)
    {
        w_u8(owner + 0x36u, 0x4Du);
        w_u32(owner + 0x2Cu, owner + 0x5Du);
        w_u8(owner + 0x35u, 6u);
        return 6u;
    }
    return 0x4Du;
}

static uint32 native_pad_main_mode_receive(uint32 owner)
{
    if (r_u8(owner + 0x53u))
    {
        if (r_u8(owner + 0x46u) == 2u)
            return 1u;
        w_u8(owner + 0x46u, 0xFEu);
    }
    else
        (void)psx_pad_owner_cleanup(owner);
    return 0u;
}

sint32 PadSetMainMode(sint32 port, sint32 mode, sint32 lock)
{
    uint32 index = ((uint32)port & 0xF0u) != 0u;
    uint32 owner = native_pad_packet_owners[index];
    uint8 current_offset;
    uint8 requested_mode = (uint8)mode;
    uint8 requested_lock = (uint8)lock;
    /* Native digital input has no extended-mode command engine */
    if (!owner)
        return 0;
    if (!r_u16(owner + 0xE6u) || r_u8(owner + 0x46u) != 0xFFu)
        return 0;
    if (!pad_main_mode_send_target || !pad_main_mode_receive_target)
    {
#ifndef NDEBUG
        fprintf(stderr, "Unbound native PAD main-mode commands owner=%08X\n", owner);
#endif
        abort();
    }
    current_offset = r_u8(owner + 0xE4u);
    w_u8(owner + 0x46u, 1u);
    w_u32(owner + 0x14u, pad_main_mode_send_target);
    w_u32(owner + 0x18u, pad_main_mode_receive_target);
    w_u8(owner + 0x51u, requested_mode);
    w_u8(owner + 0x52u, requested_lock);
    w_u8(owner + 0x53u, requested_mode == current_offset);
    return 1;
}

int pad_state_io(FILE *file, int load)
{
    uint32 size = (uint32)sizeof(pad_buttons);
    if (load)
        return fread(&size, 4, 1, file) == 1 && size == sizeof(pad_buttons) && fread(&pad_buttons, 1, sizeof(pad_buttons), file) == sizeof(pad_buttons);
    return fwrite(&size, 4, 1, file) == 1 && fwrite(&pad_buttons, 1, sizeof(pad_buttons), file) == sizeof(pad_buttons);
}

/* PS-X executable loading */
#include <stdlib.h>
#include <string.h>

static sint32 resolve_cd_path(uint32 guest_path, const char *root, char *host_path, size_t capacity)
{
    size_t cursor;
    size_t index;

    if (!root || !host_path || !capacity)
        return 0;
    cursor = strlen(root);
    if (!cursor || cursor + 2u > capacity)
        return 0;
    memcpy(host_path, root, cursor);
    if (host_path[cursor - 1u] != '/' && host_path[cursor - 1u] != '\\')
        host_path[cursor++] = '/';
    while (r_u8(guest_path) == '\\' || r_u8(guest_path) == '/')
        ++guest_path;
    while (r_u8(guest_path) != 0u && r_u8(guest_path) != ';')
    {
        uint8 character = r_u8(guest_path++);

        if (cursor + 1u >= capacity || character == ':')
            return 0;
        host_path[cursor++] = character == '\\' ? '/' : (char)character;
    }
    host_path[cursor] = '\0';
    for (index = 0u; index + 1u < cursor; ++index)
        if (host_path[index] == '.' && host_path[index + 1u] == '.')
            return 0;
    return 1;
}

static sint32 resolve_cd_path_native(const char *src, const char *root, char *host_path, size_t capacity)
{
    size_t cursor;
    size_t index;

    if (!src || !root || !host_path || !capacity)
        return 0;
    cursor = strlen(root);
    if (!cursor || cursor + 2u > capacity)
        return 0;
    memcpy(host_path, root, cursor);
    if (host_path[cursor - 1u] != '/' && host_path[cursor - 1u] != '\\')
        host_path[cursor++] = '/';
    while ((uint8)*src == '\\' || (uint8)*src == '/')
        ++src;
    while ((uint8)*src != 0u && (uint8)*src != ';')
    {
        uint8 character = (uint8)*src++;

        if (cursor + 1u >= capacity || character == ':')
            return 0;
        host_path[cursor++] = character == '\\' ? '/' : (char)character;
    }
    host_path[cursor] = '\0';
    for (index = 0u; index + 1u < cursor; ++index)
        if (host_path[index] == '.' && host_path[index + 1u] == '.')
            return 0;
    return 1;
}

sint32 xport_psx_exe_load(const PSX_EXE *executable)
{
    uint8 *image;
    size_t size;
    uint32 load_address;
    uint32 payload_size;
    sint32 ok = 0;

    if (!executable || !executable->path || executable->bss_end < executable->bss_start)
        return 0;
    image = (uint8 *)malloc(PSX_DRAM_SIZE);
    if (!image)
        return 0;
    if (!xport_file_read(executable->path, image, PSX_DRAM_SIZE, &size) || size < 2048u)
        goto finish;
    if (memcmp(image, "PS-X EXE", 8u) != 0)
        goto finish;
    load_address = xport_load_le32(image + 0x18u);
    payload_size = xport_load_le32(image + 0x1cu);
    if (load_address != executable->load_address || payload_size > size - 2048u || (load_address & 0x1fffffffu) + payload_size > PSX_DRAM_SIZE)
        goto finish;
    if (!xport_guest_copy(xport_guest_ref(0u), xport_host_ref(image), size) || !xport_guest_copy(xport_guest_ref(load_address), xport_guest_ref(2048u), payload_size) || !xport_guest_fill(0u, 0u, load_address & 0x1fffffffu) || !xport_guest_fill(executable->bss_start, 0u, executable->bss_end - executable->bss_start) || !w_u32(executable->heap_metadata, executable->heap_start) || !w_u32(executable->heap_metadata + 4u, executable->heap_size))
        goto finish;
    ok = 1;
finish:
    free(image);
    return ok;
}

sint32 xport_cd_file_size(uint32 guest_path, const char *root, size_t *size)
{
    char path[260];

    if (!resolve_cd_path(guest_path, root, path, sizeof(path)))
        return 0;
    return xport_file_size(path, size);
}

sint32 xport_cd_file_read(uint32 guest_path, const char *root, uint32 destination, size_t capacity, size_t *size)
{
    char path[260];
    uint8 *buffer;
    sint32 ok;

    if (!size || !resolve_cd_path(guest_path, root, path, sizeof(path)))
        return 0;
    buffer = (uint8 *)malloc(capacity ? capacity : 1u);
    if (!buffer)
        return 0;
    ok = xport_file_read(path, buffer, capacity, size) && (!*size || xport_guest_copy(xport_guest_ref(destination), xport_host_ref(buffer), *size));
    free(buffer);
    return ok;
}

sint32 xport_cd_file_size_native(const char *src, const char *root, size_t *size)
{
    char path[260];

    if (!size || !resolve_cd_path_native(src, root, path, sizeof(path)))
        return 0;
    return xport_file_size(path, size);
}

sint32 xport_cd_file_read_native(const char *src, const char *root, void *dst, size_t cap, size_t *size)
{
    char path[260];

    if (!size || (!dst && cap) || !resolve_cd_path_native(src, root, path, sizeof(path)))
        return 0;
    return xport_file_read(path, dst, cap, size);
}

sint32 GetRCnt(uint32 counter)
{
    uint32 index = (uint32)counter & 0xFFFFu;
    if (index >= 3u)
        return 0;
    psx_root_counter_poll();
    return (sint32)timers.counter[index];
}

sint32 OpenEventGuest(uint32 event_class, uint32 specification, uint32 mode, uint32 guest_callback)
{
    uint32 index;
    for (index = 0; index < 32u; ++index)
    {
        BiosNativeEvent *event = &bios_native_events[index];
        if (event->allocated)
            continue;
        memset(event, 0, sizeof(*event));
        event->allocated = 1u;
        event->event_class = event_class;
        event->specification = specification;
        event->mode = mode;
        event->callback = guest_callback;
        return (sint32)(index + 1u);
    }
    return -1;
}

sint32 OpenEvent(uint32 event_class, sint32 specification, sint32 mode, PsxEventCallback callback)
{
    sint32 handle = OpenEventGuest(event_class, (uint32)specification, (uint32)mode, 0u);
    if (handle > 0)
        bios_native_events[handle - 1].native_callback = callback;
    return handle;
}

sint32 EnableEvent(sint32 event_handle)
{
    uint32 handle = (uint32)event_handle;
    if (handle >= 1u && handle <= 32u && bios_native_events[handle - 1u].allocated)
    {
        bios_native_events[handle - 1u].enabled = 1u;
        return 1;
    }
    return 0;
}

sint32 DisableEvent(sint32 event_handle)
{
    uint32 handle = (uint32)event_handle;
    if (handle >= 1u && handle <= 32u && bios_native_events[handle - 1u].allocated)
    {
        bios_native_events[handle - 1u].enabled = 0u;
        return 1;
    }
    return 0;
}

/* Native instruction caches need no guest flush */
void FlushCache(void)
{
}

/* No native memory-card medium is mounted */
void *firstfile(const char *pattern, void *record)
{
    (void)record;
    if (!pattern)
        return NULL;
    if (strncmp(pattern, "bu00:", 5u) == 0 || strncmp(pattern, "bu10:", 5u) == 0)
        return NULL;
#ifndef NDEBUG
    fprintf(stderr, "Unsupported BIOS directory device: %s\n", pattern);
#endif
    abort();
}

/* A missing native card has no further directory entries */
void *nextfile(void *record)
{
    (void)record;
    return NULL;
}

void _boot(void)
{
    abort();
}

sint32 SetConf(sint32 events, sint32 threads, uint32 stack_top)
{
    uint32 descriptor, event_table, thread_slot, thread_table, index;
    if (events <= 0 || threads <= 0 || (uint32)events > 256u || (uint32)threads > 32u)
        return 0;
    w_u32(0xA001B944u, (uint32)events);
    w_u32(0xA001B940u, (uint32)threads);
    w_u32(0xA001B948u, stack_top);
    psx_bios_init_kernel_heap(0xA000E000u, 0x2000u);
    descriptor = psx_bios_kernel_malloc(32u);
    event_table = psx_bios_kernel_malloc(28u * (uint32)events);
    thread_slot = psx_bios_kernel_malloc(4u);
    thread_table = psx_bios_kernel_malloc(192u * (uint32)threads);
    if (!descriptor || !event_table || !thread_slot || !thread_table || descriptor == ~0u || event_table == ~0u || thread_slot == ~0u || thread_table == ~0u)
        return 0;
    memset(psx_addr(descriptor, 32u), 0, 32u);
    memset(psx_addr(event_table, 28u * (uint32)events), 0, 28u * (uint32)events);
    memset(psx_addr(thread_table, 192u * (uint32)threads), 0, 192u * (uint32)threads);
    w_u32(0xA0000100u, descriptor);
    w_u32(0xA0000104u, 32u);
    w_u32(0xA0000120u, event_table);
    w_u32(0xA0000124u, 28u * (uint32)events);
    psx_bios_bind_events(event_table, 28u * (uint32)events);
    w_u32(0xA0000108u, thread_slot);
    w_u32(0xA000010Cu, 4u);
    w_u32(0xA0000110u, thread_table);
    w_u32(0xA0000114u, 192u * (uint32)threads);
    w_u32(thread_slot, thread_table);
    for (index = 0u; index < (uint32)threads; ++index)
        w_u32(thread_table + index * 192u, index ? 0x1000u : 0x4000u);
    psx_bios_init_rcnt(0u);
    psx_bios_init_default_interrupts(3u);
    psx_bios_enqueue_timer_vblank(1u);
    psx_bios_reset_entry_interrupt();
    return 1;
}

/* SCPH-5502 kernel heap metadata and allocation algorithm */
uint32 psx_bios_init_kernel_heap(uint32 base, uint32 bytes)
{
    w_u32(0x7460u, base);
    w_u32(base, 0u);
    w_u32(0x7468u, base + (bytes & ~3u) + 4u);
    w_u32(0x7464u, bytes);
    w_u32(0x6D30u, 0u);
    return bytes & ~3u;
}

static uint32 bios_kernel_heap_extend(uint32 bytes)
{
    uint32 cursor = r_u32(0x7460u), end = r_u32(0x7468u);
    uint32 amount = 0u;
    if (bytes)
    {
        uint32 available = (uint32)(((sint32)(end - cursor) >> 2) << 2) - 4u;
        uint32 configured = r_u32(0x7464u);
        uint32 minimum = configured < bytes ? configured : bytes;
        uint32 maximum = configured < bytes ? bytes : configured;
        if (available >= maximum)
            amount = maximum;
        else if (available >= minimum)
            amount = minimum;
        else
            return ~0u;
    }
    else if (cursor >= end)
        return ~0u;
    w_u32(cursor + (amount & ~3u), ~1u);
    w_u32(cursor - 4u, amount | 1u);
    w_u32(0x7460u, cursor + ((amount + 4u) & ~3u));
    return 0u;
}

uint32 psx_bios_kernel_malloc(uint32 bytes)
{
    uint32 size = (bytes + 3u) & ~3u;
    sint32 scans = 2;
    uint32 header, block;
    if (!r_u32(0x6D30u))
    {
        header = r_u32(0x7460u);
        if (header >= r_u32(0x7468u))
            return 0u;
        w_u32(header, ~1u);
        w_u32(0x85FCu, header);
        w_u32(0x7460u, header + 4u);
        if (bios_kernel_heap_extend(size))
            return 0u;
        w_u32(0x85F8u, header);
        w_u32(0x6D30u, 1u);
    }
    for (;;)
    {
        header = r_u32(0x85FCu);
        block = r_u32(header);
        if (block & 1u)
        {
            if (block == (size | 1u))
            {
                w_u32(header, block & ~1u);
                return header + 4u;
            }
            if ((size | 1u) < block)
            {
                w_u32(header + size + 4u, block - size - 4u);
                w_u32(header, size);
                return header + 4u;
            }
            if (block < size)
            {
                uint32 next_header = header + (block & ~3u) + 4u;
                uint32 next = r_u32(next_header);
                if (next == ~1u)
                {
                    if (--scans > 0)
                        w_u32(0x85FCu, r_u32(0x85F8u));
                    else if (bios_kernel_heap_extend(size))
                        return 0u;
                }
                else if (next & 1u)
                    w_u32(header, block + (next & ~1u) + 4u);
                else
                    w_u32(0x85FCu, next_header);
            }
        }
        else if (block == ~1u)
        {
            if (--scans > 0)
                w_u32(0x85FCu, r_u32(0x85F8u));
            else
            {
                uint32 result = bios_kernel_heap_extend(size);
                if (result)
                    return result;
            }
        }
        else
            w_u32(0x85FCu, header + (block & ~3u) + 4u);
    }
}

uint32 psx_bios_init_rcnt(uint32 priority)
{
    return SysEnqIntRP(priority, 0x6DA8u);
}

uint32 psx_bios_init_default_interrupts(uint32 priority)
{
    uint32 node;
    w_u32(0x8610u, 0u);
    w_u32(0x8614u, 0u);
    w_u32(0x8618u, 0u);
    for (node = 0x861Cu; node < 0x863Cu; node += 4u)
        w_u32(node, 0u);
    return SysEnqIntRP(priority, 0x6D98u);
}

uint32 psx_bios_enqueue_timer_vblank(uint32 priority)
{
    uint32 node, counter;
    for (node = 0x6D58u, counter = 0u; node < 0x6D98u; node += 16u, ++counter)
    {
        w_u32(0x8600u + counter * 4u, 1u);
        SysEnqIntRP(priority, node);
    }
    for (counter = 0u; counter < 3u; ++counter)
    {
        timers.mode[counter] = 0u;
        timers.target[counter] = 0u;
        timers.counter[counter] = 0u;
    }
    return 0x863Cu;
}

uint32 psx_bios_reset_entry_interrupt(void)
{
    w_u32(0x75D0u, 0x6CF4u);
    return 0x6CF4u;
}

uint32 psx_bios_init_user_heap(uint32 base, uint32 bytes)
{
    if (!r_u32(0xA0000100u))
        SetConf(16, 4, 0x801FFF00u);
    w_u32(0xA0009000u, base);
    w_u32(base, 0u);
    w_u32(0xA0009008u, base + (bytes & ~3u) + 4u);
    w_u32(0xA0009004u, bytes);
    w_u32(0xA000900Cu, 0u);
    return bytes & ~3u;
}

sint32 _96_init(void)
{
    return CdInit();
}

sint32 InitTAP(uint32 first_buffer, uint32 second_buffer, sint32 first_mode, sint32 second_mode)
{
    (void)first_buffer;
    (void)second_buffer;
    (void)first_mode;
    (void)second_mode;
    abort();
}

sint32 StartTAP(void)
{
    abort();
}

sint32 MemCardInitPSX(void)
{
    abort();
}

sint32 MemCardStartPSX(void)
{
    abort();
}

uint32 InitPAD(uint32 buffer0, uint32 size0, uint32 buffer1, uint32 size1)
{
    if (size0 < 8u || size1 < 8u || size0 > PSX_DRAM_SIZE || size1 > PSX_DRAM_SIZE || !bios_card_address(buffer0, size0, 1u) || !bios_card_address(buffer1, size1, 1u))
        return 0u;
    pad_packets[0] = (uint8 *)psx_addr(buffer0, size0);
    pad_packets[1] = (uint8 *)psx_addr(buffer1, size1);
    w_u32(0x74C8u, buffer0);
    w_u32(0x74CCu, buffer1);
    w_u32(0x74D8u, size0);
    w_u32(0x74DCu, size1);
    w_u32(0x74D0u, 0u);
    w_u32(0x74D4u, 0u);
    w_u32(0x74E0u, 0u);
    w_u32(0x74E4u, 0u);
    pad_active = 0;
    bios_pad_initialized = 1u;
    w_u32(0x74B8u, 1u);
    pad_publish(0, 0, 0xFFFFu);
    pad_publish(1, 0, 0xFFFFu);
    xport_input_init();
    return 1u;
}

uint32 StartPAD(void)
{
    if (!bios_pad_initialized)
        return 0u;
    pad_active = 1;
    ChangeClearPAD(1u);
    ChangeClearRCnt(3u, 0u);
    return 1u;
}

uint32 ChangeClearPAD(uint32 mode)
{
    uint32 previous = bios_pad_clear_mode;
    bios_pad_clear_mode = mode;
    w_u32(0x8914u, mode);
    return previous;
}

uint32 _SpuInit(uint32 mode)
{
    if (mode != 0u)
    {
#ifndef NDEBUG
        fprintf(stderr, "Unsupported native SPU warm initialization mode %u\n", mode);
#endif
        abort();
    }
    SpuInit();
    spu_bind_heap(NULL);
    /* Preserve the last SDK register-write address carried in v0 */
    return 0x1F801DA2u;
}

void xport_missing_sn_constructors(void)
{
    /* User-authorized missing SN compiler runtime service at 0x8005B1A0 */
    abort();
}

sint32 ClearImage2(PSX_RECT *rectangle, uint8 red, uint8 green, uint8 blue)
{
    (void)rectangle;
    (void)red;
    (void)green;
    (void)blue;
    abort();
}

uint32 _bu_init(void)
{
    return xport_bios_start_card();
}

uint32 GsInitGraph(uint32 arg0, uint32 arg1, uint32 arg2, uint32 arg3, uint32 arg4)
{
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    abort();
}

uint32 GsDefDispBuff(uint32 arg0, uint32 arg1, uint32 arg2, uint32 arg3)
{
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    abort();
}

uint32 PadInitMtap(uint32 arg0, uint32 arg1)
{
    (void)arg0;
    (void)arg1;
    abort();
}

uint32 ReadInitPadFlag(void)
{
    return bios_pad_initialized;
}

uint32 _patch_card(void)
{
    if (!bios_card_patches_bound)
    {
#ifndef NDEBUG
        fprintf(stderr, "Unbound BIOS card patch sources\n");
#endif
        abort();
    }
    memmove(psx_addr(bios_card_first_destination, 20u), psx_addr(bios_card_first_source, 20u), 20u);
    w_u32(0xDFFCu, bios_card_first_destination + 20u);
    FlushCache();
    return bios_card_first_destination + 20u;
}

uint32 _patch_card2(void)
{
    if (!bios_card_patches_bound)
    {
#ifndef NDEBUG
        fprintf(stderr, "Unbound BIOS card patch sources\n");
#endif
        abort();
    }
    uint32 original = r_u32(bios_card_second_destination);
    memmove(psx_addr(bios_card_second_destination, 20u), psx_addr(bios_card_second_source, 20u), 20u);
    FlushCache();
    return original;
}

uint32 _copy_memcard_patch(void)
{
    if (!bios_card_patches_bound)
    {
#ifndef NDEBUG
        fprintf(stderr, "Unbound BIOS card patch sources\n");
#endif
        abort();
    }
    memmove(psx_addr(0xDF80u, 112u), psx_addr(bios_card_copy_source, 112u), 112u);
    return 0xDFF0u;
}

uint32 VSyncCallbackPSX(uint32 guest_callback)
{
    uint32 previous = bios_vsync_callback;
    bios_vsync_callback = guest_callback;
    return previous;
}

uint32 ResetCallbackPSX(void)
{
    uint32 previous = bios_callbacks_running;
    ResetCallback();
    bios_vsync_callback = 0u;
    bios_callbacks_running = 1u;
    return previous;
}

uint32 xport_bios_init_card(uint32 mode)
{
    uint32 previous = r_u32(0x756Cu);
    w_u8(0x755Au, 0u);
    w_u8(0x7264u, 0u);
    w_u8(0x7568u, 1u);
    w_u8(0x7569u, 1u);
    w_u32(0x7544u, 0x4D6Cu);
    w_u32(0x7548u, 0x4F90u);
    w_u32(0x7540u, 0u);
    w_u32(0x754Cu, 0u);
    if (bios_card_irq_initializer != NULL)
        bios_card_irq_initializer();
    w_u32(0x756Cu, 1u);
    w_u32(0x74B8u, mode);
    return previous;
}

uint32 xport_bios_start_card(void)
{
    SysDeqIntRP(2u, 0x74A8u);
    SysEnqIntRP(2u, 0x74A8u);
    ChangeClearPAD(1u);
    ChangeClearRCnt(3u, 0u);
    w_u32(0x74BCu, 1u);
    return 1u;
}

sint32 xport_bios_enter_critical(void)
{
    sint32 previous = (sint32)bios_interrupts_enabled;
    bios_interrupts_enabled = 0u;
    return previous;
}

void xport_bios_exit_critical(void)
{
    bios_interrupts_enabled = 1u;
    bios_native_irq_dispatch();
    if (bios_vblank_pending)
    {
        bios_vblank_pending = 0u;
        psx_vblank_signal();
    }
}

uint32 xport_psyq_crt_initialize(void)
{
    abort();
}

static MATRIX *gte_gs_light_directions;
static MATRIX *gte_gs_light_colors;

void gte_bind_gs_light_matrices(MATRIX *directions, MATRIX *colors)
{
    gte_gs_light_directions = directions;
    gte_gs_light_colors = colors;
}

sint32 GsSetFlatLight(sint32 index, const GsF_LIGHT *light)
{
    MATRIX directions, colors;
    uint32 squared, length, component, channel;
    sint32 vector[3];
    uint8 rgb[3];
    if (!light || !gte_gs_light_directions || !gte_gs_light_colors)
    {
#ifndef NDEBUG
        fprintf(stderr, "GsSetFlatLight requires a descriptor and bound LIBGS matrices\n");
#endif
        abort();
    }
    rgb[0] = light->r;
    rgb[1] = light->g;
    rgb[2] = light->b;
    directions = *gte_gs_light_directions;
    colors = *gte_gs_light_colors;
    vector[0] = light->vx;
    vector[1] = light->vy;
    vector[2] = light->vz;
    squared = (uint32)vector[0] * (uint32)vector[0];
    squared += (uint32)vector[1] * (uint32)vector[1];
    squared += (uint32)vector[2] * (uint32)vector[2];
    xport_gte_write_data(30u, squared);
    length = SquareRoot0((sint32)squared);
    if (!length)
        return -1;
    if (index >= 0 && index < 3)
    {
        for (component = 0; component < 3u; ++component)
        {
            sint32 numerator = (sint32)((0u - (uint32)vector[component]) << 12u);
            sint32 denominator = (sint32)length;
            if (denominator == -1 && numerator == (-2147483647 - 1))
            {
#ifndef NDEBUG
                fprintf(stderr, "GsSetFlatLight signed division overflow\n");
#endif
                abort();
            }
            directions.m[index][component] = (sint16)(numerator / denominator);
        }
        for (channel = 0; channel < 3u; ++channel)
        {
            sint32 scaled = (sint32)((uint32)rgb[channel] << 12u);
            sint32 high = (sint32)(((sint64)scaled * (sint32)0x80808081u) >> 32u);
            colors.m[channel][index] = (sint16)((sint32)((uint32)high + (uint32)scaled) >> 7u);
        }
    }
    *gte_gs_light_directions = directions;
    *gte_gs_light_colors = colors;
    SetColorMatrix(&colors);
    return 0;
}

sint32 GsInit3D(void)
{
    abort();
}

void xport_missing_guest_vsync_callback(uint32 callback_address)
{
    (void)callback_address;
    abort();
}

void xport_missing_guest_cdcontrol(uint32 command, uint32 parameter_address, uint32 result_address)
{
    (void)command;
    (void)parameter_address;
    (void)result_address;
    abort();
}

void xport_missing_guest_cd_puts(uint32 text_address)
{
    (void)text_address;
    abort();
}

void xport_missing_guest_cd_printf_text(uint32 format_address)
{
    (void)format_address;
    abort();
}

void xport_missing_guest_cd_printf_error(uint32 format_address, uint32 command_name_address, uint32 status, uint32 code)
{
    (void)format_address;
    (void)command_name_address;
    (void)status;
    (void)code;
    abort();
}

void xport_missing_guest_cd_printf_interrupt(uint32 format_address, uint32 interrupt)
{
    (void)format_address;
    (void)interrupt;
    abort();
}

void xport_missing_guest_main_sprintf(char *destination, const char *format, const char *level_name)
{
    (void)destination;
    (void)format;
    (void)level_name;
    abort();
}

uint32 xport_guest_call0(uint32 target)
{
    (void)target;
    abort();
}

void xport_mips_overflow_exception(uint32 pc)
{
    (void)pc;
    abort();
}

uint32 PadInitDirectPSX(uint32 first_packet, uint32 second_packet)
{
    if (pad_replay_bound)
    {
        pad_replay_failure = 1;
        return 0u;
    }
    if (!bios_card_address(first_packet, 0x22u, 1u) || !bios_card_address(second_packet, 0x22u, 1u))
        return 0u;
    PadInitDirect((uint8 *)psx_addr(first_packet, 0x22u), (uint8 *)psx_addr(second_packet, 0x22u));
    if (!xport_input_init())
        return 0u;
    return 1u;
}

uint32 PadStartComPSX(void)
{
    if (pad_replay_bound)
    {
        pad_replay_failure = 1;
        return 0u;
    }
    if (!bios_pad_initialized)
        return 0u;
    PadStartCom();
    return 1u;
}

uint32 MemCardExistPSX(uint32 channel)
{
    (void)channel;
    abort();
}

uint32 MemCardSyncPSX(uint32 mode, uint32 command, uint32 result)
{
    (void)mode;
    (void)command;
    (void)result;
    abort();
}

uint32 MemCardOpenPSX(uint32 channel, uint32 filename, uint32 mode)
{
    (void)channel;
    (void)filename;
    (void)mode;
    abort();
}

uint32 MemCardReadFilePSX(uint32 channel, uint32 filename, uint32 destination, uint32 offset, uint32 length)
{
    (void)channel;
    (void)filename;
    (void)destination;
    (void)offset;
    (void)length;
    abort();
}

uint32 MemCardClosePSX(void)
{
    abort();
}

uint32 chkRC2wait(uint32 *registers)
{
    uint32 current;
    uint32 start;
    uint32 target;
    uint32 mode;
    uint32 limit;
    uint32 elapsed;
    current = r_u16(0x1F801120u);
    start = r_u32(0x800A4F08u);
    registers[0] = current;
    if (current < start)
    {
        target = r_u16(0x1F801128u);
        if (target != 0u)
            registers[0] += r_u16(0x1F801128u);
        else
            registers[0] += 0x10000u;
    }
    mode = r_u16(0x1F801124u);
    start = r_u32(0x800A4F08u);
    limit = r_u32(0x800A4F0Cu);
    elapsed = registers[0] - start;
    if ((mode & 0x200u) == 0u)
        elapsed >>= 3;
    return (uint32)(elapsed >= limit);
}

uint32 PadGetStatePSX(uint32 port)
{
    return (uint32)PadGetState((sint32)port);
}

void PadSetActPSX(uint32 port, uint32 actuator, uint32 length)
{
    uint32 owner = native_pad_packet_owners[(port & 0xF0u) != 0u];
    PadSetAct((sint32)port, actuator ? (uint8 *)psx_addr(actuator, 0u) : NULL, (sint32)length);
    if (owner)
    {
        w_u32(owner + 0x28u, actuator);
        w_u8(owner + 0x34u, (uint8)length);
    }
}

uint32 PadSetActAlignPSX(uint32 port, uint32 alignment)
{
    if (PadGetState((sint32)port) == 0 || (pad_packets[pad_index((sint32)port)][1] >> 4) == 4u)
        return 0u;
    return (uint32)PadSetActAlign((sint32)port, alignment ? (uint8 *)psx_addr(alignment, 6u) : NULL);
}

uint32 setRC2wait(uint32 wait)
{
    uint32 counter = r_u16(0x1F801120u);
    w_u32(0x800A4F0Cu, wait);
    w_u32(0x800A4F08u, counter);
    return counter;
}

uint32 xport_guest_call4_with_registers(uint32 target, uint32 *registers)
{
    (void)target;
    (void)registers;
    abort();
}

void xport_mips_break(uint32 code)
{
    /* Terminate on an explicit guest MIPS break */
    (void)code;
    abort();
}

uint32 CdSearchFileGuest(uint32 destination, uint32 filename)
{
    char path[256];
    uint32 index;
    if (!destination || !filename)
        return 0;
    for (index = 0; index < sizeof(path); ++index)
    {
        path[index] = (char)r_u8(filename + index);
        if (!path[index])
            return CdSearchFile((CdlFILE *)psx_addr(destination, sizeof(CdlFILE)), path) ? destination : 0;
    }
    return 0;
}

void xport_gte_write_data(uint32 register_index, uint32 value)
{
    if (register_index <= 5u)
    {
        sint16 *vertex = register_index < 2u ? gte.v0 : register_index < 4u ? gte.v1 : gte.v2;
        if (register_index & 1u)
            vertex[2] = (sint16)value;
        else
        {
            vertex[0] = (sint16)value;
            vertex[1] = (sint16)(value >> 16);
        }
    }
    else if (register_index == 6u)
        gte.rgbc = value;
    else if (register_index == 7u)
        gte.otz = (uint16)value;
    else if (register_index == 8u)
        gte.ir0 = (sint16)value;
    else if (register_index <= 11u)
        gte.ir[register_index - 9u] = (sint16)value;
    else if (register_index <= 14u)
        gte.sxy[register_index - 12u] = (sint32)value;
    else if (register_index == 15u)
    {
        /* SXYP writes advance the screen coordinate FIFO */
        gte.sxy[0] = gte.sxy[1];
        gte.sxy[1] = gte.sxy[2];
        gte.sxy[2] = (sint32)value;
    }
    else if (register_index <= 19u)
        gte.sz[register_index - 16u] = (uint16)value;
    else if (register_index <= 22u)
        gte.rgb[register_index - 20u] = value;
    else if (register_index == 24u)
        gte.mac0 = (sint32)value;
    else if (register_index >= 25u && register_index <= 27u)
        gte.mac[register_index - 25u] = (sint32)value;
    else if (register_index == 30u)
    {
        uint32 bits = (value & 0x80000000u) != 0u ? ~value : value;
        uint32 count = 0u;
        gte_lzcs = value;
        while (count < 32u && (bits & 0x80000000u) == 0u)
        {
            ++count;
            bits <<= 1;
        }
        gte_lzcr = count;
    }
    else if (register_index == 31u)
    {
        /* LZCR is read only */
    }
    else
        abort();
}

uint32 xport_gte_read_data(uint32 register_index)
{
    if (register_index <= 5u)
    {
        const sint16 *vertex = register_index < 2u ? gte.v0 : register_index < 4u ? gte.v1 : gte.v2;
        if (register_index & 1u)
            return (uint32)(sint32)vertex[2];
        return (uint16)vertex[0] | ((uint32)(uint16)vertex[1] << 16);
    }
    if (register_index == 6u)
        return gte.rgbc;
    if (register_index == 7u)
        return (uint16)gte.otz;
    if (register_index == 8u)
        return (uint32)(sint32)(sint16)gte.ir0;
    if (register_index <= 11u)
        return (uint32)(sint32)(sint16)gte.ir[register_index - 9u];
    if (register_index <= 14u)
        return (uint32)gte.sxy[register_index - 12u];
    if (register_index == 15u)
        return (uint32)gte.sxy[2];
    if (register_index <= 19u)
        return (uint16)gte.sz[register_index - 16u];
    if (register_index <= 22u)
        return gte.rgb[register_index - 20u];
    if (register_index == 24u)
        return (uint32)gte.mac0;
    if (register_index >= 25u && register_index <= 27u)
        return (uint32)gte.mac[register_index - 25u];
    if (register_index == 30u)
        return gte_lzcs;
    if (register_index == 31u)
        return gte_lzcr;
    abort();
}

uint32 _SpuDataCallback(uint32 guest_callback)
{
    (void)guest_callback;
    abort();
}

CdlFILE *CdSearchFile(CdlFILE *result, const char *path)
{
    return (CdlFILE *)cd_search_file_native(result, path);
}

sint32 CdReset(sint32 mode)
{
    return cd_reset_native(mode);
}

sint32 StopRCnt(uint32 counter, uint32 control_slot, uint32 mask_table)
{
    uint32 control = r_u32(control_slot) + 4u;
    uint32 mask = r_u32(mask_table + ((counter & 0xFFFFu) << 2));
    uint32 value = r_u32(control);
    w_u32(control, value & ~mask);
    return 1;
}

sint32 StopCallback(void)
{
    sint32 previous = (sint32)bios_callbacks_running;
    bios_callbacks_running = 0u;
    return previous;
}

sint32 Exec(uint32 guest_header, uint32 argc, uint32 argv)
{
    uint32 entry, destination, bytes, bss, bss_bytes;
    if (!bios_card_address(guest_header, 0x30u, 4u))
        return 0;
    entry = r_u32(guest_header);
    destination = r_u32(guest_header + 8u);
    bytes = r_u32(guest_header + 12u);
    bss = r_u32(guest_header + 24u);
    bss_bytes = r_u32(guest_header + 28u);
    if (!bytes || bytes > PSX_DRAM_SIZE || !bios_card_address(destination, bytes, 4u) || !bios_card_address(entry, 4u, 4u) || (entry & 0x1FFFFFFFu) < (destination & 0x1FFFFFFFu) || (entry & 0x1FFFFFFFu) - (destination & 0x1FFFFFFFu) >= bytes || bss_bytes > PSX_DRAM_SIZE || (bss_bytes && !bios_card_address(bss, bss_bytes, 1u)))
        return 0;
    /* BIOS Load has already placed the text at its destination */
    if (bss_bytes)
        memset(psx_addr(bss, bss_bytes), 0, bss_bytes);
    if (!bios_native_exec)
    {
#ifndef NDEBUG
        fprintf(stderr, "Unsupported native PS-X EXE entry %08X (header %08X)\n", entry, guest_header);
#endif
        abort();
    }
    /* The dispatcher must reject unregistered entries; zero is a valid program result */
    return bios_native_exec(bios_native_exec_context, entry, argc, argv);
}

// Shared recurrence for PsyQ LIBC2 and BIOS, with separate seed owners
sint32 psyq_rand_next(uint32 *seed)
{
    *seed = *seed * 0x41C64E6Du + 0x3039u;
    return (sint32)((*seed >> 16) & 0x7FFFu);
}

sint32 randPSX(void)
{
    uint32 seed = r_u32(0xA0009010u);
    sint32 value = psyq_rand_next(&seed);

    w_u32(0xA0009010u, seed);
    return value;
}

sint32 OpenEventPSX(uint32 event_class, uint32 specification, uint32 mode, uint32 guest_callback)
{
    uint32 saved_base = r_u32(0xA0000120u);
    uint32 bytes = r_u32(0xA0000124u);
    uint32 base = r_u32(0xA0000120u);
    uint32 current = base;
    uint32 end = base + (bytes / 28u) * 28u;
    sint32 slot = -1;
    while (current < end)
    {
        if (r_u32(current + 4u) == 0u)
        {
            uint32 refreshed_base = r_u32(0xA0000120u);
            slot = (sint32)(current - refreshed_base) / 28;
            break;
        }
        current += 28u;
    }
    if (slot == -1)
        return -1;
    uint32 event = saved_base + (uint32)slot * 28u;
    w_u32(event, event_class);
    w_u32(event + 8u, specification);
    w_u32(event + 12u, mode);
    w_u32(event + 4u, 0x1000u);
    w_u32(event + 16u, guest_callback);
    return (sint32)((uint32)slot | 0xF1000000u);
}

sint32 EnableEventPSX(uint32 handle)
{
    uint32 base = r_u32(0xA0000120u);
    uint32 event = base + (handle & 0xFFFFu) * 28u;
    if (r_u32(event + 4u) != 0u)
        w_u32(event + 4u, 0x2000u);
    return 1;
}

sint32 DisableEventPSX(uint32 handle)
{
    uint32 base = r_u32(0xA0000120u);
    uint32 event = base + (handle & 0xFFFFu) * 28u;
    if (r_u32(event + 4u) != 0u)
        w_u32(event + 4u, 0x1000u);
    return 1;
}

sint32 CloseEventPSX(uint32 handle)
{
    uint32 base = r_u32(0xA0000120u);
    w_u32(base + (handle & 0xFFFFu) * 28u + 4u, 0u);
    return 1;
}

uint32 xport_missing_cd_sync_guest_callback(uint32 guest_callback)
{
    return CdSyncCallbackPSX(guest_callback);
}

uint32 _spu_Fw(uint32 source, uint32 length)
{
    uint32 transferred;
    transferred = SpuWrite(psx_addr(source, length), length);
    if (spu_transfer_failed())
    {
#ifndef NDEBUG
        fprintf(stderr, "SPU upload failed: source %08X bytes %u\n", source, length);
#endif
        abort();
    }
    return transferred;
}

static sint32 native_root_irq_read(void *user, uint32 address, uint32 width, uint32 *value)
{
    uint32 offset = address - 0x1F801070u;
    (void)user;
    if ((width != 2u && width != 4u) || (offset != 0u && offset != 4u))
        return 0;
    *value = offset == 0u ? native_root_irq.status : native_root_irq.mask;
    return 1;
}

static sint32 native_root_irq_write(void *user, uint32 address, uint32 width, uint32 value, uint32 byte_mask)
{
    uint32 offset = address - 0x1F801070u;
    (void)user;
    (void)byte_mask;
    if ((width != 2u && width != 4u) || (offset != 0u && offset != 4u))
        return 0;
    return psx_irq_write(&native_root_irq, offset, value);
}

void psx_root_counter_bind(uint32 control_slot, uint32 mask_table)
{
    native_root_control_slot = control_slot;
    native_root_mask_table = mask_table;
    native_root_last_us = xport_timer_get();
    native_root_clock_started = 1u;
    native_root_tick_fraction = 0u;
    xport_memory_bind_mmio(0x1F801070u, 8u, native_root_irq_read, native_root_irq_write, NULL);
}

void psx_root_counter_poll(void)
{
    uint64 now, elapsed, ticks;
    if (native_root_polling)
        return;
    if (!native_root_clock_started)
    {
        native_root_last_us = xport_timer_get();
        native_root_clock_started = 1u;
    }
    native_root_polling = 1u;
    now = xport_timer_get();
    elapsed = now - native_root_last_us;
    native_root_last_us = now;
    ticks = elapsed / 1000000u * 33868800u;
    native_root_tick_fraction += elapsed % 1000000u * 33868800u;
    ticks += native_root_tick_fraction / 1000000u;
    native_root_tick_fraction %= 1000000u;
    {
        uint64 remaining = ticks;
        while (remaining)
        {
            uint32 step = remaining > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32)remaining;
            if (!(timers.mode[0] & 0x100u))
                psx_timer_add(0u, step);
            if (!(timers.mode[1] & 0x100u))
                psx_timer_add(1u, step);
            remaining -= step;
        }
    }
    if (timers.mode[2] & 0x81u)
    {
#ifndef NDEBUG
        fprintf(stderr, "Unsupported native timer2 gate/toggle mode %04X\n", timers.mode[2]);
#endif
        abort();
    }
    if (timers.mode[2] & 0x200u)
    {
        ticks += timers.div8_carry;
        timers.div8_carry = (uint32)(ticks % 8u);
        ticks /= 8u;
    }
    while (ticks)
    {
        uint32 step = ticks > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32)ticks;
        psx_timer_add(2u, step);
        ticks -= step;
    }
    native_root_polling = 0u;
}

sint32 StartRCnt(uint32 counter)
{
    uint32 control, mask;
    psx_root_counter_poll();
    counter &= 0xFFFFu;
    if (!native_root_control_slot || !native_root_mask_table)
    {
        if (counter > 3u)
            return 0;
        native_root_irq.mask |= counter == 3u ? 1u : 1u << (counter + 4u);
        return counter < 3u;
    }
    control = r_u32(native_root_control_slot) + 4u;
    mask = r_u32(native_root_mask_table + counter * 4u);
    w_u32(control, r_u32(control) | mask);
    return counter < 3u;
}

void srandPSX(uint32 seed)
{
    w_u32(0xA0009010u, seed);
}

static PsxInterruptCallback native_dma_callbacks[7];

PsxInterruptCallback psx_dma_callback_native(uint32 channel)
{
    PsxInterruptCallback callback;
    if (channel >= sizeof(native_dma_callbacks) / sizeof(native_dma_callbacks[0]))
        return NULL;
    xport_audio_lock();
    callback = native_dma_callbacks[channel];
    xport_audio_unlock();
    return callback;
}

PsxInterruptCallback DMACallback(uint32 channel, PsxInterruptCallback callback)
{
    PsxInterruptCallback previous;

    if (channel >= sizeof(native_dma_callbacks) / sizeof(native_dma_callbacks[0]))
        return NULL;
    xport_audio_lock();
    previous = native_dma_callbacks[channel];
    native_dma_callbacks[channel] = callback;
    xport_audio_unlock();
    return previous;
}

uint32 CdReadyCallbackPSX(uint32 callback)
{
    uint32 previous;
    previous = r_u32(xport_cd_ready_callback_address);
    w_u32(xport_cd_ready_callback_address, callback);
    return previous;
}

uint32 CdSyncCallbackPSX(uint32 callback)
{
    uint32 previous;
    previous = r_u32(xport_cd_sync_callback_address);
    w_u32(xport_cd_sync_callback_address, callback);
    return previous;
}

uint32 xport_guest_nonlocal_transfer(uint32 owner_entry, uint32 source_pc, uint32 target, uint32 known_gpr_mask, const uint32 *known_gpr_values)
{
    (void)owner_entry;
    (void)source_pc;
    (void)target;
    (void)known_gpr_mask;
    (void)known_gpr_values;
    abort();
}

uint32 bzeroPSX(uint32 destination, uint32 length)
{
    if (destination == 0u || (sint32)length <= 0)
        return 0u;
    memset(psx_addr(destination, length), 0, length);
    return destination;
}

sint32 openPSX(const char *path, uint32 mode)
{
    (void)mode;
    if (!path)
        return -1;
    if (strncmp(path, "bu00:", 5u) == 0 || strncmp(path, "bu10:", 5u) == 0)
        return -1;
#ifndef NDEBUG
    fprintf(stderr, "Unsupported native BIOS file device: %s\n", path);
#endif
    abort();
}

sint32 readPSX(sint32 descriptor, void *destination, uint32 length)
{
    (void)descriptor;
    (void)destination;
    (void)length;
    /* No native BIOS file descriptor can be opened without a card medium */
    return -1;
}

sint32 writePSX(sint32 fd, const void *src, uint32 length)
{
    (void)fd;
    (void)src;
    (void)length;
    // No BIOS file descriptor is available without a card medium
    return -1;
}

sint32 erase(const char *path)
{
    if (!path)
        return 0;
    if (strncmp(path, "bu00:", 5u) == 0 || strncmp(path, "bu10:", 5u) == 0)
        return 0;
#ifndef NDEBUG
    fprintf(stderr, "Unsupported native BIOS file device: %s\n", path);
#endif
    abort();
}

sint32 closePSX(sint32 descriptor)
{
    (void)descriptor;
    return -1;
}

uint32 SysEnqIntRP(uint32 priority, uint32 node)
{
    uint32 queue = r_u32(0xA0000100u) + 8u * priority;
    uint32 previous = r_u32(queue);
    w_u32(queue, node);
    w_u32(node, previous);
    return 0u;
}

uint32 ChangeClearRCnt(uint32 counter, uint32 mode)
{
    uint32 slot = 0x8600u + counter * 4u;
    uint32 previous = r_u32(slot);
    w_u32(slot, mode);
    return previous;
}

static PsxPadCallbackBinding native_pad_callbacks[32];
static uint32 native_pad_callback_count;

void psx_pad_bind_callbacks(const PsxPadCallbackBinding *bindings, uint32 count)
{
    uint32 i, j;
    if (count > 32u || (count && !bindings))
        abort();
    for (i = 0u; i < count; ++i)
    {
        if (!bindings[i].target || !bindings[i].callback)
            abort();
        for (j = 0u; j < i; ++j)
            if (bindings[j].target == bindings[i].target)
                abort();
    }
    if (count)
        memcpy(native_pad_callbacks, bindings, count * sizeof(*bindings));
    native_pad_callback_count = count;
}

uint32 psx_pad_owner_cleanup(uint32 owner)
{
    uint32 i;
    if (!r_u8(owner + 73u))
        return 0u;
    w_u8(owner + 73u, 0u);
    w_u8(owner + 70u, 0u);
    w_u16(owner + 230u, 0u);
    w_u32(owner + 20u, 0u);
    w_u32(owner + 24u, 0u);
    w_u8(owner + 227u, 0u);
    w_u8(owner + 228u, 0u);
    w_u16(owner + 230u, 0u);
    w_u8(owner + 233u, 0u);
    w_u8(owner + 234u, 0u);
    w_u32(owner, 0u);
    w_u32(owner + 4u, 0u);
    w_u32(owner + 8u, 0u);
    for (i = 0u; i < 6u; ++i)
        w_u8(owner + 93u + i, 0xFFu);
    return 0xFFFFFFFFu;
}

static void native_pad_owner_command(uint32 owner, uint32 command, uint32 parameter)
{
    w_u8(owner + 54u, (uint8)command);
    w_u32(owner + 44u, owner + 36u);
    w_u8(owner + 36u, (uint8)parameter);
    w_u8(owner + 53u, 1u);
}

uint32 psx_pad_owner_begin(uint32 owner, uint32 cleanup_target)
{
    uint32 state, callback;
    if (r_u8(r_u32(owner + 60u)) == 0xF3u)
    {
        if (!r_u8(owner + 232u))
        {
            native_pad_owner_command(owner, 0x43u, 0u);
            return 0u;
        }
        if (r_u8(owner + 73u) == 2u)
            xport_missing_pad_cleanup_callback(cleanup_target, owner);
    }
    state = r_u8(owner + 70u);
    if (state == 1u || state == 0xFEu)
    {
        native_pad_owner_command(owner, 0x43u, state == 1u);
        return 0u;
    }
    if (state == 0u || state == 0xFFu)
        return 0u;
    callback = r_u32(owner + 20u);
    if (callback)
        xport_missing_pad_owner_callback(callback, owner);
    else if (state == 2u)
    {
        w_u8(owner + 54u, 0x45u);
        w_u32(owner + 44u, 0u);
        w_u8(owner + 53u, 0u);
    }
    else if (state == 3u)
        native_pad_owner_command(owner, 0x4Cu, r_u8(owner + 228u));
    else if (state == 4u)
        native_pad_owner_command(owner, 0x47u, r_u8(owner + 71u));
    return 0u;
}

void xport_missing_pad_cleanup_callback(uint32 target, uint32 owner)
{
    (void)xport_missing_pad_owner_callback(target, owner);
}

void xport_missing_guest_sprintf_format(char *destination, uint32 format_address)
{
    (void)destination;
    (void)format_address;
    abort();
}

uint32 xport_missing_pad_owner_callback(uint32 target, uint32 owner)
{
    uint32 i;
    if (target && target == pad_main_mode_send_target)
        return native_pad_main_mode_send(owner);
    if (target && target == pad_main_mode_receive_target)
        return native_pad_main_mode_receive(owner);
    for (i = 0u; i < native_pad_callback_count; ++i)
        if (native_pad_callbacks[i].target == target)
            return native_pad_callbacks[i].callback(owner);
#ifndef NDEBUG
    fprintf(stderr, "Unbound native PAD callback target=%08X owner=%08X\n", target, owner);
#endif
    abort();
}

uint32 VectorNormalSS(SVECTOR *source, SVECTOR *destination)
{
    VECTOR input;
    VECTOR squares;
    VECTOR product;
    SVECTOR loaded;
    sint64 sum_wide;
    uint32 sum;
    uint32 leading;
    uint32 normalized;
    uint32 shift;
    sint16 coefficient;

    if (!source || !destination)
        abort();
    loaded.vx = source->vx;
    loaded.vy = source->vy;
    loaded.vz = source->vz;
    loaded.pad = 0;
    input.vx = loaded.vx;
    input.vy = loaded.vy;
    input.vz = loaded.vz;
    input.pad = 0;
    Square0(&input, &squares);
    sum_wide = (sint64)squares.vx + squares.vy + squares.vz;
    // Original signed ADD traps on squared-norm overflow
    if (sum_wide > 2147483647)
    {
#ifndef NDEBUG
        fprintf(stderr, "VectorNormalSS squared-norm overflow\n");
#endif
        abort();
    }
    sum = (uint32)sum_wide;
    leading = (uint32)Lzc((sint32)sum) & ~1u;
    normalized = leading < 24u ? sum >> (24u - leading) : sum << (leading - 24u);
    // Original zero lookup is the halfword 128 bytes before its table
    coefficient = sum ? psyq_rsqrt_table[normalized - 64u] : 7276;
    shift = (uint32)((31 - (sint32)leading) >> 1) & 31u;
    gte_gpf0(&loaded, coefficient, &product);
    destination->vx = (sint16)(product.vx >> shift);
    destination->vy = (sint16)(product.vy >> shift);
    destination->vz = (sint16)(product.vz >> shift);
    return sum;
}

uint32 SetFogNearFar(sint32 near_distance, sint32 far_distance, sint32 projection)
{
    sint32 separation = (sint32)((uint32)far_distance - (uint32)near_distance);
    sint32 product;
    sint32 near_quotient;
    sint32 far_quotient;
    sint32 numerator;
    sint32 dqa;
    uint32 result;

    if (separation < 100)
        return 1u;
    product = (sint32)((0u - (uint32)near_distance) * (uint32)far_distance);
    near_quotient = product / separation;
    far_quotient = (sint32)((uint32)far_distance << 12u) / separation;
    numerator = (sint32)((uint32)near_quotient << 8u);
    if (!projection || (numerator == INT32_MIN && projection == -1))
    {
#ifndef NDEBUG
        fprintf(stderr, "SetFogNearFar original DIV exception\n");
#endif
        abort();
    }
    dqa = numerator / projection;
    if (dqa < -32768)
        dqa = -32768;
    result = dqa > 32767 ? 1u : 0u;
    if (result)
        dqa = 32767;
    xport_gte_write_control(27u, (uint32)dqa);
    xport_gte_write_control(28u, (uint32)far_quotient << 12u);
    return result;
}

static sint64 native_gte_wrap44(sint64 value, uint32 row)
{
    uint64 bits;
    if (value > 0x7FFFFFFFFFFLL)
        gte.flag |= (sint32)(1u << (30u - row));
    if (value < -0x80000000000LL)
        gte.flag |= (sint32)(1u << (27u - row));
    bits = (uint64)value & 0xFFFFFFFFFFFULL;
    return (sint64)(bits & 0x7FFFFFFFFFFULL) - ((bits & 0x80000000000ULL) ? 0x80000000000LL : 0);
}

static void native_gte_mac_ir(uint32 row, sint64 value, uint32 shift, uint32 lm)
{
    value = native_gte_wrap44(value, row);
    gte.mac[row] = shift ? mac_shift12(value) : (sint32)(uint32)value;
    gte.ir[row] = lm ? clamp_ir_unsigned(gte.mac[row], 1u << (24u - row)) : clamp_ir(gte.mac[row], 1u << (24u - row));
}

static SVECTOR native_gte_vertex(uint32 index)
{
    SVECTOR v;
    if (index < 3u)
    {
        const sint16 *input = index == 0u ? gte.v0 : index == 1u ? gte.v1 : gte.v2;
        v.vx = input[0];
        v.vy = input[1];
        v.vz = input[2];
    }
    else
    {
        v.vx = (sint16)gte.ir[0];
        v.vy = (sint16)gte.ir[1];
        v.vz = (sint16)gte.ir[2];
    }
    v.pad = 0;
    return v;
}

static void native_gte_matrix(const MATRIX *matrix, const sint32 input[3], const sint32 translation[3], uint32 shift, uint32 lm)
{
    uint32 row, column;
    for (row = 0; row < 3u; ++row)
    {
        sint64 value = translation ? (sint64)translation[row] * 4096 : 0;
        for (column = 0; column < 3u; ++column)
            value = native_gte_wrap44(value + (sint64)matrix->m[row][column] * input[column], row);
        native_gte_mac_ir(row, value, shift, lm);
    }
}

static void native_gte_normal_color(const SVECTOR *normal, uint32 depth)
{
    sint32 input[3] = {normal->vx, normal->vy, normal->vz};
    uint32 row;
    native_gte_matrix(&gte.light, input, NULL, 1u, 1u);
    for (row = 0; row < 3u; ++row)
        input[row] = gte.ir[row];
    native_gte_matrix(&gte.color, input, gte.back_color, 1u, 1u);
    for (row = 0; row < 3u; ++row)
    {
        sint64 base = (sint64)((gte.rgbc >> (row * 8u)) & 255u) * gte.ir[row] * 16;
        sint64 value = base;
        if (depth)
        {
            sint64 difference = native_gte_wrap44((sint64)gte.far_color[row] * 4096 - base, row);
            sint32 delta = clamp_ir(mac_shift12(difference), 1u << (24u - row));
            value = base + (sint64)gte.ir0 * delta;
        }
        native_gte_mac_ir(row, value, 1u, 1u);
    }
    push_rgb_from_mac();
}

static uint32 native_gte_screen_offset_raw(uint32 axis)
{
    if (axis == 0u)
        return ((uint32)gte.ofx << 16) | native_gte_ofx_fraction;
    if (axis == 1u)
        return ((uint32)gte.ofy << 16) | native_gte_ofy_fraction;
    abort();
}

static sint64 native_gte_floor_shift(sint64 value, uint32 shift)
{
    sint64 divisor = (sint64)(1ULL << shift);
    if (value >= 0)
        return value / divisor;
    return -1 - ((-1 - value) / divisor);
}

static void native_gte_project(const SVECTOR *v, uint32 last)
{
    sint32 input[3] = {v->vx, v->vy, v->vz};
    sint32 sx, sy, depth_cue;
    uint32 row, column, depth, quotient;
    sint64 accumulated, screen_x, screen_y, cue;

    for (row = 0u; row < 3u; ++row)
    {
        accumulated = (sint64)gte.translation[row] * 4096;
        for (column = 0u; column < 3u; ++column)
            accumulated = native_gte_wrap44(accumulated + (sint64)gte.rotation.m[row][column] * input[column], row);
        gte.mac[row] = (sint32)native_gte_floor_shift(accumulated, 12u);
        gte.ir[row] = clamp_ir(gte.mac[row], 1u << (24u - row));
    }
    depth = clamp_sz(gte.mac[2]);
    gte.sz[0] = gte.sz[1];
    gte.sz[1] = gte.sz[2];
    gte.sz[2] = gte.sz[3];
    gte.sz[3] = depth;
    if (depth == 0u || (uint32)gte.h >= depth * 2u)
    {
        quotient = 0x1FFFFu;
        gte.flag |= 1u << 17;
    }
    else
        quotient = gte_unr_divide((uint32)gte.h, depth);
    screen_x = (sint64)(sint32)native_gte_screen_offset_raw(0u) + (sint64)gte.ir[0] * quotient;
    screen_y = (sint64)(sint32)native_gte_screen_offset_raw(1u) + (sint64)gte.ir[1] * quotient;
    (void)truncate_mac0(screen_x);
    (void)truncate_mac0(screen_y);
    sx = clamp_sxy((sint32)native_gte_floor_shift(screen_x, 16u), 1u << 14);
    sy = clamp_sxy((sint32)native_gte_floor_shift(screen_y, 16u), 1u << 13);
    gte.sxy[0] = gte.sxy[1];
    gte.sxy[1] = gte.sxy[2];
    gte.sxy[2] = (sint32)((uint16)sx | ((uint32)(uint16)sy << 16));
    if (last != 0u)
    {
        cue = (sint64)quotient * gte.dqa + gte.dqb;
        gte.mac0 = truncate_mac0(cue);
        depth_cue = (sint32)native_gte_floor_shift(cue, 12u);
        if (depth_cue < 0)
        {
            depth_cue = 0;
            gte.flag |= 1u << 12;
        }
        else if (depth_cue > 4096)
        {
            depth_cue = 4096;
            gte.flag |= 1u << 12;
        }
        gte.ir0 = depth_cue;
    }
}

static void native_gte_depth_color(uint32 color, uint32 shift, uint32 lm)
{
    uint32 row;
    for (row = 0; row < 3u; ++row)
    {
        sint64 base = (sint64)((color >> (row * 8u)) & 255u) * 65536;
        sint32 delta;
        native_gte_mac_ir(row, (sint64)gte.far_color[row] * 4096 - base, shift, 0u);
        delta = gte.ir[row];
        native_gte_mac_ir(row, base + (sint64)gte.ir0 * delta, shift, lm);
    }
    push_rgb_from_mac();
}

void xport_gte_execute(uint32 command)
{
    uint32 opcode = command & 63u;
    uint32 shift = (command >> 19u) & 1u;
    uint32 lm = (command >> 10u) & 1u;
    uint32 row, index;
    SVECTOR v;
    gte.flag = 0;
    switch (opcode)
    {
        case 0x01u:
            if (!shift || lm)
                break;
            v = native_gte_vertex(0u);
            native_gte_project(&v, 1u);
            finish_flag();
            return;
        case 0x30u:
            if (!shift || lm)
                break;
            for (index = 0; index < 3u; ++index)
            {
                v = native_gte_vertex(index);
                native_gte_project(&v, index == 2u);
            }
            finish_flag();
            return;
        case 0x06u:
            (void)NormalClip(gte.sxy[0], gte.sxy[1], gte.sxy[2]);
            return;
        case 0x2Du:
            (void)AverageZ3((sint32)gte.sz[1], (sint32)gte.sz[2], (sint32)gte.sz[3]);
            return;
        case 0x2Eu:
            (void)AverageZ4((sint32)gte.sz[0], (sint32)gte.sz[1], (sint32)gte.sz[2], (sint32)gte.sz[3]);
            return;
        case 0x12u:
        {
            uint32 mx = (command >> 17u) & 3u, vector = (command >> 15u) & 3u, cv = (command >> 13u) & 3u;
            sint32 input[3];
            const MATRIX *matrix;
            const sint32 *translation;
            if (mx == 3u || cv == 2u)
                break;
            matrix = mx == 0u ? &gte.rotation : mx == 1u ? &gte.light : &gte.color;
            translation = cv == 0u ? gte.translation : cv == 1u ? gte.back_color : NULL;
            v = native_gte_vertex(vector);
            input[0] = v.vx;
            input[1] = v.vy;
            input[2] = v.vz;
            native_gte_matrix(matrix, input, translation, shift, lm);
            finish_flag();
            return;
        }
        case 0x14u:
        {
            sint32 input[3] = {gte.ir[0], gte.ir[1], gte.ir[2]};
            sint32 base[3];
            native_gte_matrix(&gte.color, input, gte.back_color, shift, lm);
            for (row = 0u; row < 3u; ++row)
                base[row] = (sint32)((gte.rgbc >> (row * 8u)) & 255u) * gte.ir[row] * 16;
            for (row = 0u; row < 3u; ++row)
            {
                sint32 delta;
                native_gte_mac_ir(row, (sint64)gte.far_color[row] * 4096 - base[row], shift, 0u);
                delta = gte.ir[row];
                native_gte_mac_ir(row, (sint64)base[row] + (sint64)gte.ir0 * delta, shift, lm);
            }
            push_rgb_from_mac();
            finish_flag();
            return;
        }
        case 0x10u:
            native_gte_depth_color(gte.rgbc, shift, lm);
            finish_flag();
            return;
        case 0x2Au:
            for (index = 0; index < 3u; ++index)
                native_gte_depth_color(gte.rgb[0], shift, lm);
            finish_flag();
            return;
        case 0x13u:
        case 0x1Bu:
        case 0x16u:
        case 0x3Fu:
            if (!shift || !lm)
                break;
            for (index = 0; index < ((opcode == 0x16u || opcode == 0x3Fu) ? 3u : 1u); ++index)
            {
                v = native_gte_vertex(index);
                native_gte_normal_color(&v, opcode == 0x13u || opcode == 0x16u);
            }
            finish_flag();
            return;
        case 0x28u:
            for (row = 0u; row < 3u; ++row)
                native_gte_mac_ir(row, (sint64)gte.ir[row] * gte.ir[row], shift, lm);
            finish_flag();
            return;
        case 0x0Cu:
        {
            sint32 ir[3] = {gte.ir[0], gte.ir[1], gte.ir[2]};
            sint64 output[3];
            output[0] = (sint64)gte.rotation.m[1][1] * ir[2] - (sint64)gte.rotation.m[2][2] * ir[1];
            output[1] = (sint64)gte.rotation.m[2][2] * ir[0] - (sint64)gte.rotation.m[0][0] * ir[2];
            output[2] = (sint64)gte.rotation.m[0][0] * ir[1] - (sint64)gte.rotation.m[1][1] * ir[0];
            for (row = 0; row < 3u; ++row)
                native_gte_mac_ir(row, output[row], shift, lm);
            finish_flag();
            return;
        }
        default:
            break;
    }
#ifndef NDEBUG
    fprintf(stderr, "Unsupported GTE command %08X\n", command);
#endif
    abort();
}

uint32 xport_gte_read_screen_offset(uint32 axis)
{
    if (axis == 0u)
        return ((uint32)gte.ofx << 16u) | native_gte_ofx_fraction;
    if (axis == 1u)
        return ((uint32)gte.ofy << 16u) | native_gte_ofy_fraction;
    abort();
}

uint32 xport_gte_read_flag(void)
{
    return (uint32)gte.flag;
}

static uint32 bios_tap_interrupt_node;
static uint32 bios_card_end_table, bios_card_end_saved_words;

void psx_bios_bind_tap_interrupt(uint32 node)
{
    bios_tap_interrupt_node = node;
}

void psx_bios_bind_card_end_patch(uint32 table, uint32 saved_words)
{
    bios_card_end_table = table;
    bios_card_end_saved_words = saved_words;
}

uint32 StopTAP(void)
{
    if (!bios_tap_interrupt_node || !bios_card_address(bios_tap_interrupt_node, 12u, 4u))
    {
#ifndef NDEBUG
        fprintf(stderr, "Unbound StopTAP interrupt node\n");
#endif
        abort();
    }
    xport_bios_enter_critical();
    ChangeClearRCnt(3u, 1u);
    SysDeqIntRP(1u, bios_tap_interrupt_node);
    pad_active = 0;
    xport_bios_exit_critical();
    return bios_interrupts_enabled;
}

sint32 MemCardEndPSX(void)
{
    uint32 destination;
    if (!bios_card_end_table || !bios_card_end_saved_words || !bios_card_address(bios_card_end_table, 28u, 4u) || !bios_card_address(bios_card_end_saved_words, 12u, 4u))
    {
#ifndef NDEBUG
        fprintf(stderr, "Unbound MemCardEndPSX kernel patch\n");
#endif
        abort();
    }
    /* BIOS _stop_card removes the card interrupt routine */
    SysDeqIntRP(2u, 0x74A8u);
    w_u32(0x74BCu, 0u);
    bios_card_pending_ports = 0u;
    xport_bios_enter_critical();
    destination = r_u32(bios_card_end_table + 24u) + 112u;
    if (!bios_card_address(destination, 12u, 4u))
    {
#ifndef NDEBUG
        fprintf(stderr, "Invalid MemCardEndPSX kernel patch destination\n");
#endif
        abort();
    }
    for (uint32 i = 0; i < 3u; ++i)
        w_u32(destination + 4u * i, r_u32(bios_card_end_saved_words + 4u * i));
    FlushCache();
    xport_bios_exit_critical();
    return (sint32)bios_interrupts_enabled;
}

MATRIX *CompMatrixLV(MATRIX *left, MATRIX *right, MATRIX *destination)
{
    const uint8 *left_bytes = (const uint8 *)left;
    const uint8 *right_bytes = (const uint8 *)right;
    uint8 *output = (uint8 *)destination;
    uint32 words[5], column1[3], column2[3];
    uint32 first, second, third;
    uint32 low[3], high[3], high_mac[3], result[3];
    uint32 component;
    words[0] = xport_load_le32(left_bytes + 0u);
    words[1] = xport_load_le32(left_bytes + 4u);
    words[2] = xport_load_le32(left_bytes + 8u);
    words[3] = xport_load_le32(left_bytes + 12u);
    words[4] = xport_load_le32(left_bytes + 16u);
    xport_gte_write_control(0u, words[0]);
    xport_gte_write_control(1u, words[1]);
    xport_gte_write_control(2u, words[2]);
    xport_gte_write_control(3u, words[3]);
    xport_gte_write_control(4u, words[4]);
    first = xport_load_le16(right_bytes);
    second = xport_load_le32(right_bytes + 4u);
    third = xport_load_le32(right_bytes + 12u);
    first |= second & 0xFFFF0000u;
    xport_gte_write_data(0u, first);
    xport_gte_write_data(1u, third);
    xport_gte_execute(0x486012u);
    first = xport_load_le16(right_bytes + 2u);
    second = xport_load_le32(right_bytes + 8u);
    third = (uint32)(sint32)(sint16)xport_load_le16(right_bytes + 14u);
    first |= second << 16;
    column1[0] = xport_gte_read_data(9u);
    column1[1] = xport_gte_read_data(10u);
    column1[2] = xport_gte_read_data(11u);
    xport_gte_write_data(0u, first);
    xport_gte_write_data(1u, third);
    xport_gte_execute(0x486012u);
    first = xport_load_le16(right_bytes + 4u);
    second = xport_load_le32(right_bytes + 8u);
    third = xport_load_le32(right_bytes + 16u);
    first |= second & 0xFFFF0000u;
    column2[0] = xport_gte_read_data(9u);
    column2[1] = xport_gte_read_data(10u);
    column2[2] = xport_gte_read_data(11u);
    xport_gte_write_data(0u, first);
    xport_gte_write_data(1u, third);
    xport_gte_execute(0x486012u);
    xport_store_le32(output, (column1[0] & 0xFFFFu) | (column2[0] << 16));
    xport_store_le32(output + 12u, (column1[2] & 0xFFFFu) | (column2[2] << 16));
    first = xport_gte_read_data(9u);
    second = xport_gte_read_data(10u);
    xport_store_le32(output + 16u, xport_gte_read_data(11u));
    xport_store_le32(output + 4u, (first & 0xFFFFu) | (column1[1] << 16));
    xport_store_le32(output + 8u, (column2[1] & 0xFFFFu) | (second << 16));
    for (component = 0u; component < 3u; ++component)
    {
        uint32 value = xport_load_le32(right_bytes + 20u + component * 4u);
        uint32 magnitude = (sint32)value < 0 ? 0u - value : value;
        uint32 shifted = (magnitude >> 15u) | ((magnitude & 0x80000000u) ? 0xFFFF0000u : 0u);
        high[component] = (sint32)value < 0 ? 0u - shifted : shifted;
        low[component] = (sint32)value < 0 ? 0u - (magnitude & 0x7FFFu) : magnitude & 0x7FFFu;
    }
    for (component = 0u; component < 3u; ++component)
        xport_gte_write_data(9u + component, high[component]);
    xport_gte_execute(0x41E012u);
    for (component = 0u; component < 3u; ++component)
        high_mac[component] = xport_gte_read_data(25u + component);
    for (component = 0u; component < 3u; ++component)
        xport_gte_write_data(9u + component, low[component]);
    xport_gte_execute(0x49E012u);
    for (component = 0u; component < 3u; ++component)
        result[component] = xport_gte_read_data(25u + component) + (high_mac[component] << 3u);
    for (component = 0u; component < 3u; ++component)
        result[component] += xport_load_le32(left_bytes + 20u + component * 4u);
    for (component = 0u; component < 3u; ++component)
        xport_store_le32(output + 20u + component * 4u, result[component]);
    return destination;
}

void xport_gte_write_unavailable_data(uint32 register_index, uint32 value)
{
    (void)register_index;
    (void)value;
    abort();
}

void xport_gte_complete_rtpt(void)
{
    abort();
}

uint32 xport_guest_frame_acquire(size_t bytes)
{
    (void)bytes;
    abort();
}

void xport_guest_frame_release(uint32 address, size_t bytes)
{
    (void)address;
    (void)bytes;
    abort();
}

uint32 xport_guest_call3(uint32 target, uint32 argument0, uint32 argument1, uint32 argument2)
{
    (void)target;
    (void)argument0;
    (void)argument1;
    (void)argument2;
    abort();
}

void xport_gpu_clear_otag_r_guest_driver(uint32 guest_ot, uint32 count)
{
    abort();
}

void SetFogNear(sint32 distance, sint32 projection)
{
    uint32 product = (uint32)distance * 320u;
    sint32 numerator = (sint32)(0u - product);
    if (!projection || (numerator == INT32_MIN && projection == -1))
        abort();
    xport_gte_write_control(27u, (uint32)(numerator / projection));
    xport_gte_write_control(28u, 0x01400000u);
}

sint32 EnterCriticalSectionPSX(void)
{
    return xport_bios_enter_critical();
}

void ExitCriticalSection(void)
{
    xport_bios_exit_critical();
}

sint32 SetVideoMode(sint32 mode)
{
    sint32 previous = native_video_mode;
    native_video_mode = mode;
    return previous;
}

sint32 GetVideoMode(void)
{
    return native_video_mode;
}

/* Native PsyQ PC file-server handles */
static FILE *pc_files[64];
static uint8 pc_modes[64];
static char pc_root[512];
static sint32 pc_uppercase;

sint32 psx_pc_set_root(const char *root, sint32 uppercase)
{
    size_t length = root ? strlen(root) : 0u;
    if (length >= sizeof(pc_root))
        return 0;
    if (length)
        memcpy(pc_root, root, length);
    pc_root[length] = 0;
    pc_uppercase = uppercase != 0;
    return 1;
}

static sint32 pc_path(const char *name, char path[1024])
{
    size_t root_length = strlen(pc_root), length = 0u;
    if (!name)
        return 0;
    if (root_length)
    {
        memcpy(path, pc_root, root_length);
        length = root_length;
        path[length++] = '/';
        while (*name == '/' || *name == '\\')
            ++name;
    }
    while (*name && *name != ';')
    {
        char value = *name++;
        if (length >= 1023u)
            return 0;
        if (value == '\\')
            value = '/';
        if (pc_uppercase && value >= 'a' && value <= 'z')
            value -= 'a' - 'A';
        path[length++] = value;
    }
    path[length] = 0;
    return length != 0u;
}

sint32 PCinit(void)
{
    sint32 index, result = 0;
    for (index = 0; index < 64; ++index)
        if (pc_files[index])
        {
            if (fclose(pc_files[index]) != 0)
                result = -1;
            pc_files[index] = NULL;
        }
    return result;
}

static sint32 pc_open(char *name, const char *mode, uint8 access)
{
    char path[1024];
    sint32 index;
    if (!pc_path(name, path))
        return -1;
    for (index = 0; index < 64; ++index)
        if (!pc_files[index])
        {
            pc_files[index] = xport_fopen(path, mode);
            if (!pc_files[index])
                return -1;
            pc_modes[index] = access;
            return index;
        }
    return -1;
}

sint32 PCopen(char *name, sint32 flags, sint32 permissions)
{
    if (flags < 0 || flags > 2)
        return -1;
    return pc_open(name, flags ? "r+b" : "rb", (uint8)flags);
}

sint32 PCcreat(char *name, sint32 permissions)
{
    return pc_open(name, "w+b", 2u);
}

sint32 PCclose(sint32 handle)
{
    sint32 result;
    if (handle < 0 || handle >= 64 || !pc_files[handle])
        return -1;
    result = fclose(pc_files[handle]);
    pc_files[handle] = NULL;
    return result;
}

sint32 PClseek(sint32 handle, sint32 offset, sint32 origin)
{
    long position;
    if (handle < 0 || handle >= 64 || !pc_files[handle] || origin < 0 || origin > 2)
        return -1;
    if (fseek(pc_files[handle], (long)offset, origin) != 0)
        return -1;
    position = ftell(pc_files[handle]);
    return position < 0 || (uint64)position > 0x7FFFFFFFu ? -1 : (sint32)position;
}

sint32 PCread(sint32 handle, char *buffer, sint32 count)
{
    size_t bytes;
    if (handle < 0 || handle >= 64 || !pc_files[handle] || pc_modes[handle] == 1u || count < 0 || (!buffer && count))
        return -1;
    bytes = fread(buffer, 1u, (size_t)count, pc_files[handle]);
#ifndef NDEBUG
    return ferror(pc_files[handle]) ? -1 : (sint32)bytes;
#else
    return (sint32)bytes;
#endif
}

sint32 PCwrite(sint32 handle, char *buffer, sint32 count)
{
    size_t bytes;
    if (handle < 0 || handle >= 64 || !pc_files[handle] || pc_modes[handle] == 0u || count < 0 || (!buffer && count))
        return -1;
    bytes = fwrite(buffer, 1u, (size_t)count, pc_files[handle]);
#ifndef NDEBUG
    if (fflush(pc_files[handle]) != 0 || ferror(pc_files[handle]))
        return -1;
#endif
    return (sint32)bytes;
}

/* Native libmcrd uses the raw 128 KiB card filesystem */
static uint8 *mc_media[2];

static sint32 bios_native_card_medium_present(uint32 port)
{
    return port < 2u && mc_media[port] != NULL;
}

static uint8 mc_new[2];
static sint32 mc_running;
static sint32 mc_command, mc_channel, mc_completed_command, mc_result, mc_ready;
static sint32 mc_offset, mc_bytes;
static uint32 *mc_buffer;
static char mc_name[21];
static MemCB mc_callback;

static sint32 mc_socket(sint32 channel)
{
    return channel == 0 ? 0 : channel == 0x10 ? 1 : -1;
}

sint32 psx_memcard_mount(sint32 channel, uint8 *image, size_t bytes)
{
    sint32 socket = mc_socket(channel);
    if (socket < 0 || (image && bytes != 0x20000u) || mc_command)
        return 0;
    mc_media[socket] = image;
    mc_new[socket] = image != NULL;
    return 1;
}

static uint8 mc_checksum(const uint8 *frame)
{
    uint8 value = 0;
    uint32 index;
    for (index = 0u; index < 127u; ++index)
        value ^= frame[index];
    return value;
}

static sint32 mc_status(sint32 channel, uint8 **image)
{
    sint32 socket = mc_socket(channel);
    if (socket < 0 || !mc_media[socket])
        return 1;
    *image = mc_media[socket];
    if ((*image)[0] != 'M' || (*image)[1] != 'C')
        return 4;
    if ((*image)[127] != mc_checksum(*image))
        return 2;
    return 0;
}

static sint32 mc_find(const uint8 *image, const char *name)
{
    uint32 block;
    if (!name || !*name || strlen(name) > 20u)
        return -1;
    for (block = 1u; block <= 15u; ++block)
    {
        const uint8 *entry = image + block * 128u;
        if (xport_load_le32(entry) == 0x51u && memchr(entry + 10u, 0, 21u) && strcmp((const char *)entry + 10u, name) == 0)
            return (sint32)block;
    }
    return -1;
}

static sint32 mc_chain(const uint8 *image, sint32 first, uint8 blocks[15])
{
    uint32 visited = 0u, next, count = 0u;
    while (first >= 1 && first <= 15 && count < 15u)
    {
        const uint8 *entry = image + (uint32)first * 128u;
        uint32 state = xport_load_le32(entry);
        if (visited & (1u << first) || entry[127] != mc_checksum(entry) || (!count && state != 0x51u) || (count && state != 0x52u && state != 0x53u))
            return -1;
        blocks[count++] = (uint8)first;
        visited |= 1u << first;
        next = xport_load_le16(entry + 8u);
        if (next == 0xFFFFu)
            return (sint32)count;
        if (state == 0x53u || next >= 15u)
            return -1;
        first = (sint32)next + 1;
    }
    return -1;
}

static sint32 mc_transfer(uint8 *image)
{
    uint8 blocks[15];
    sint32 first = mc_find(image, mc_name), count;
    uint32 length, offset, remaining;
    uint8 *buffer = (uint8 *)mc_buffer;
    if (first < 0)
        return 5;
    count = mc_chain(image, first, blocks);
    length = xport_load_le32(image + (uint32)first * 128u + 4u);
    if (count < 1 || length != (uint32)count * 8192u)
        return 2;
    if (mc_offset < 0 || mc_bytes < 0 || (uint32)mc_offset > length || (uint32)mc_bytes > length - (uint32)mc_offset)
        return 2;
    offset = (uint32)mc_offset;
    remaining = (uint32)mc_bytes;
    while (remaining)
    {
        uint32 part = 8192u - (offset & 8191u);
        uint8 *data = image + (uint32)blocks[offset / 8192u] * 8192u + (offset & 8191u);
        if (part > remaining)
            part = remaining;
        if (mc_command == 4)
            memcpy(data, buffer, part);
        else
            memcpy(buffer, data, part);
        offset += part;
        buffer += part;
        remaining -= part;
    }
    return 0;
}

static void mc_poll(void)
{
    uint8 *image = NULL;
    sint32 socket, result;
    MemCB callback;
    if (!mc_command || !mc_running)
        return;
    socket = mc_socket(mc_channel);
    if (socket < 0 || !mc_media[socket])
        result = 1;
    else if (mc_command == 1)
        result = mc_new[socket] ? 3 : 0;
    else
    {
        result = mc_status(mc_channel, &image);
        if (mc_command == 2)
        {
            if (!result && mc_new[socket])
                result = 3;
            mc_new[socket] = 0u;
        }
        else if (mc_new[socket])
            result = 3;
        else if (!result)
            result = mc_transfer(image);
    }
    mc_completed_command = mc_command;
    mc_result = result;
    mc_command = 0;
    mc_ready = 1;
    callback = mc_callback;
    if (callback)
        callback(mc_completed_command, result);
}

void MemCardInit(sint32 mode)
{
    mc_running = 0;
    mc_command = mc_completed_command = mc_result = mc_ready = 0;
    mc_callback = NULL;
}

void MemCardStart(void)
{
    mc_command = mc_completed_command = mc_result = mc_ready = 0;
    mc_running = 1;
}

void MemCardStop(void)
{
    mc_poll();
    mc_running = 0;
}

void MemCardEnd(void)
{
    mc_running = 0;
}

static sint32 mc_queue(sint32 command, sint32 channel)
{
    if (mc_command || !mc_running)
        return 0;
    mc_command = command;
    mc_channel = channel;
    mc_ready = 0;
    mc_result = 0;
    return 1;
}

sint32 MemCardExist(sint32 channel)
{
    return mc_queue(1, channel);
}

sint32 MemCardAccept(sint32 channel)
{
    return mc_queue(2, channel);
}

static sint32 mc_queue_file(sint32 command, sint32 channel, char *name, uint32 *buffer, sint32 offset, sint32 bytes)
{
    size_t length;
    if (!name || !buffer || ((uint32)offset & 127u) || ((uint32)bytes & 127u))
        return 0;
    length = strlen(name);
    if (!length || length > 20u || !mc_queue(command, channel))
        return 0;
    memcpy(mc_name, name, length + 1u);
    mc_buffer = buffer;
    mc_offset = offset;
    mc_bytes = bytes;
    return 1;
}

sint32 MemCardReadFile(sint32 channel, char *name, uint32 *buffer, sint32 offset, sint32 bytes)
{
    return mc_queue_file(3, channel, name, buffer, offset, bytes);
}

sint32 MemCardWriteFile(sint32 channel, char *name, uint32 *buffer, sint32 offset, sint32 bytes)
{
    return mc_queue_file(4, channel, name, buffer, offset, bytes);
}

sint32 MemCardSync(sint32 mode, sint32 *command, sint32 *result)
{
    if (!mc_command && !mc_ready)
        return -1;
    if (!mode)
        mc_poll();
    if (result)
        *result = mc_ready ? mc_result : 0;
    if (command)
        *command = mc_ready ? mc_completed_command : mc_command;
    if (!mc_ready)
        return 0;
    mc_ready = 0;
    return 1;
}

MemCB MemCardCallback(MemCB callback)
{
    MemCB previous = mc_callback;
    mc_callback = callback;
    return previous;
}

sint32 MemCardFormat(sint32 channel)
{
    sint32 socket = mc_socket(channel);
    uint8 *image;
    uint32 frame;
    if (mc_command)
        return -1;
    if (socket < 0 || !(image = mc_media[socket]))
        return 1;
    memset(image, 0xFF, 0x20000u);
    memset(image, 0, 8192u);
    image[0] = 'M';
    image[1] = 'C';
    image[127] = mc_checksum(image);
    for (frame = 1u; frame < 36u; ++frame)
    {
        uint8 *entry = image + frame * 128u;
        if (frame < 16u)
            entry[0] = 0xA0u;
        else
            xport_store_le32(entry, 0xFFFFFFFFu);
        xport_store_le16(entry + 8u, 0xFFFFu);
        entry[127] = mc_checksum(entry);
    }
    memcpy(image + 63u * 128u, image, 128u);
    mc_new[socket] = 0u;
    return 0;
}

sint32 MemCardCreateFile(sint32 channel, char *name, sint32 count)
{
    uint8 *image = NULL, free_blocks[15];
    sint32 status, found = 0;
    uint32 block;
    size_t length;
    if (mc_command)
        return -1;
    status = mc_status(channel, &image);
    if (status)
        return status;
    if (!name || !(length = strlen(name)) || length > 20u || count < 1 || count > 15)
        return 5;
    if (mc_find(image, name) >= 0)
        return 6;
    for (block = 1u; block <= 15u && found < count; ++block)
    {
        uint8 *entry = image + block * 128u;
        uint32 state = xport_load_le32(entry);
        if (entry[127] != mc_checksum(entry))
            return 2;
        if (state >= 0xA0u && state <= 0xA3u)
            free_blocks[found++] = (uint8)block;
    }
    if (found < count)
        return 7;
    for (found = 0; found < count; ++found)
    {
        uint8 *entry = image + (uint32)free_blocks[found] * 128u;
        memset(entry, 0, 128u);
        entry[0] = found == 0 ? 0x51u : found + 1 == count ? 0x53u : 0x52u;
        xport_store_le16(entry + 8u, found + 1 == count ? 0xFFFFu : (uint16)(free_blocks[found + 1] - 1u));
        if (!found)
        {
            xport_store_le32(entry + 4u, (uint32)count * 8192u);
            memcpy(entry + 10u, name, length + 1u);
        }
        entry[127] = mc_checksum(entry);
    }
    return 0;
}

sint32 MemCardDeleteFile(sint32 channel, char *name)
{
    uint8 *image = NULL, blocks[15];
    sint32 status, first, count, index;
    if (mc_command)
        return -1;
    status = mc_status(channel, &image);
    if (status)
        return status;
    first = mc_find(image, name);
    if (first < 0)
        return 5;
    count = mc_chain(image, first, blocks);
    if (count < 1)
        return 2;
    for (index = 0; index < count; ++index)
    {
        uint8 *entry = image + (uint32)blocks[index] * 128u;
        entry[0] = (uint8)(entry[0] + 0x50u);
        entry[127] = mc_checksum(entry);
    }
    return 0;
}

static sint32 mc_matches(const char *pattern, const char *name)
{
    const char *star = NULL, *retry = NULL;
    while (*name)
    {
        if (*pattern == '?' || *pattern == *name)
        {
            ++pattern;
            ++name;
        }
        else if (*pattern == '*')
        {
            star = ++pattern;
            retry = name;
        }
        else if (star)
        {
            pattern = star;
            name = ++retry;
        }
        else
            return 0;
    }
    while (*pattern == '*')
        ++pattern;
    return *pattern == 0;
}

sint32 MemCardGetDirentry(sint32 channel, char *pattern, DIRENTRY *directory, sint32 *files, sint32 offset, sint32 maximum)
{
    uint8 *image = NULL;
    sint32 status, skipped = 0, copied = 0;
    uint32 block;
    if (mc_command)
        return -1;
    status = mc_status(channel, &image);
    if (status)
        return status;
    if (!pattern || offset < 0 || maximum < 0)
        return 2;
    for (block = 1u; block <= 15u && copied < maximum; ++block)
    {
        uint8 *entry = image + block * 128u;
        if (xport_load_le32(entry) != 0x51u)
            continue;
        if (entry[127] != mc_checksum(entry) || !memchr(entry + 10u, 0, 21u))
            return 2;
        if (!mc_matches(pattern, (char *)entry + 10u))
            continue;
        if (skipped++ < offset || !directory)
            continue;
        memset(&directory[copied], 0, sizeof(DIRENTRY));
        memcpy(directory[copied].name, entry + 10u, 20u);
        directory[copied].attr = 0x51;
        directory[copied].size = (sint32)xport_load_le32(entry + 4u);
        directory[copied].head = (sint32)block;
        ++copied;
    }
    if (files)
        *files = copied;
    return 0;
}

static void libc_qsort_swap(unsigned char *left, unsigned char *right, size_t size)
{
    for (size_t i = 0; i < size; ++i)
    {
        unsigned char value = left[i];
        left[i] = right[i];
        right[i] = value;
    }
}

/* PsyQ libc qsortPSX Pivot selection and strict-less-than
 * partitioning are observable when the comparator reports equal keys. */
void qsortPSX(void *base, size_t count, size_t size, int (*compare)(const void *, const void *))
{
    unsigned char *bytes = (unsigned char *)base;
    if (count < 2)
        return;
    if (count == 2)
    {
        if (compare(bytes, bytes + size) > 0)
            libc_qsort_swap(bytes, bytes + size, size);
        return;
    }

    libc_qsort_swap(bytes, bytes + (count >> 1) * size, size);
    unsigned char *boundary = bytes;
    unsigned char *element = bytes + size;
    size_t lower_count = 0;
    for (size_t i = 1; i < count; ++i, element += size)
    {
        if (compare(element, bytes) < 0)
        {
            boundary += size;
            ++lower_count;
            if (element != boundary)
                libc_qsort_swap(element, boundary, size);
        }
    }
    if (boundary != bytes)
        libc_qsort_swap(bytes, boundary, size);

    qsortPSX(bytes, lower_count, size, compare);
    qsortPSX(boundary + size, count - lower_count - 1, size, compare);
}

/* PsyQ soft-float comparison orders encoded magnitudes, including NaNs */
sint32 __ltdf2(double left, double right)
{
    uint64 left_bits, right_bits;
    memcpy(&left_bits, &left, sizeof(left_bits));
    memcpy(&right_bits, &right, sizeof(right_bits));
    const uint64 sign = (uint64)1 << 63;
    const uint64 left_magnitude = left_bits & ~sign;
    const uint64 right_magnitude = right_bits & ~sign;
    if (left_bits == right_bits || !(left_magnitude | right_magnitude))
        return 0;
    if ((left_bits ^ right_bits) & sign)
        return (left_bits & sign) ? -1 : 0;
    const int magnitude_less = left_magnitude < right_magnitude;
    return (left_bits & sign) ? (magnitude_less ? 0 : -1) : (magnitude_less ? -1 : 0);
}

sint32 math_errno, math_err_point;

sint32 _err_math(sint32 error, sint32 point)
{
    math_errno = error;
    math_err_point = point;
    if (error == 0x21 || error == 0x22)
        DeliverEvent(0xF4000002u, error == 0x21 ? 0x301u : 0x302u);
    return 0;
}

uint32 psx_float_bits(float value)
{
    uint32 bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float psx_float_value(uint32 bits)
{
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint32 math_signed_shift(uint32 value, uint32 shift)
{
    shift &= 31u;
    if (!shift)
        return value;
    return (value >> shift) | ((value & 0x80000000u) ? (~(uint32)0 << (32u - shift)) : 0u);
}

/* PsyQ rounds integer conversion halfway values away from zero */
float __floatsisf(sint32 value)
{
    uint32 magnitude = (uint32)value;
    uint32 sign = value < 0 ? 0x80000000u : 0u;
    uint32 exponent = 157u;
    if (!magnitude)
        return psx_float_value(0u);
    if (sign)
        magnitude = 0u - magnitude;
    if ((sint32)magnitude <= 0x00FFFFFF)
    {
        do
        {
            magnitude <<= 4;
            exponent -= 4u;
        } while ((sint32)magnitude <= 0x00FFFFFF);
    }
    if ((sint32)magnitude <= 0x3FFFFFFF)
    {
        do
        {
            magnitude <<= 1;
            --exponent;
        } while ((sint32)magnitude <= 0x3FFFFFFF);
    }
    magnitude += 0x40u;
    if (magnitude & 0x80000000u)
    {
        ++exponent;
        magnitude >>= 8;
    }
    else
        magnitude >>= 7;
    return psx_float_value(sign | (exponent << 23) | (magnitude & 0xFF7FFFFFu));
}

sint32 __fixsfsi(float value)
{
    uint32 bits = psx_float_bits(value);
    uint32 sign = bits & 0x80000000u;
    uint32 exponent = (bits >> 23) & 0xFFu;
    if (!(bits & 0x7FFFFFFFu))
        return (sint32)sign;
    if (exponent > 157u)
    {
        if (sign)
            return (sint32)0x80000000u;
        _err_math(0x22, 0x12);
        return 0x7FFFFFFF;
    }
    if (exponent < 126u)
        return 0;
    uint32 magnitude = ((bits & 0x7FFFFFu) | 0x800000u) << 7;
    magnitude >>= 157u - exponent;
    return (sint32)(sign ? 0u - magnitude : magnitude);
}

float __divsf3(float numerator, float denominator)
{
    uint32 left = psx_float_bits(numerator);
    uint32 right = psx_float_bits(denominator);
    uint32 sign = (left ^ right) & 0x80000000u;
    sint32 exponent = (sint32)((left >> 23) & 0xFFu) - (sint32)((right >> 23) & 0xFFu) + 126;
    if (!(right & 0x7FFFFFFFu))
        return psx_float_value(sign ? 0x4F800000u : 0x4F000000u);
    if (!(left & 0x7FFFFFFFu))
        return psx_float_value(sign);
    uint32 dividend = (left & 0x7FFFFFu) | 0x800000u;
    uint32 divisor = (right & 0x7FFFFFu) | 0x800000u;
    if (dividend < divisor)
    {
        dividend <<= 1;
        --exponent;
    }
    uint32 quotient = (uint32)(((uint64)dividend << 24) / divisor);
    if (exponent >= 0)
    {
        quotient = (quotient + 1u) >> 1;
        ++exponent;
    }
    else
    {
        uint32 shift = (0u - (uint32)exponent) & 31u;
        quotient += (uint32)1 << shift;
        quotient = math_signed_shift(quotient, shift + 1u);
        exponent = 0;
    }
    quotient &= 0xFF7FFFFFu;
    if (exponent >= 255)
    {
        _err_math(0x22, 0x0E);
        return psx_float_value(sign | 0x7F800000u);
    }
    return psx_float_value(sign | ((uint32)exponent << 23) | quotient);
}

double psx_double_value(uint64 bits)
{
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

uint64 psx_double_bits(double value)
{
    uint64 bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static uint64 math_mantissa64(uint64 bits)
{
    return (bits & (((uint64)1 << 52) - 1u)) | ((uint64)1 << 52);
}

static uint64 math_arithmetic_right64(uint64 value, uint32 count)
{
    if (!count)
        return value;
    uint64 fill = (value & ((uint64)1 << 63)) ? ~(uint64)0 : 0;
    if (count >= 64u)
        return fill;
    return (value >> count) | (fill << (64u - count));
}

static uint64 math_pack64(uint64 sign, sint32 exponent, uint64 mantissa, sint32 point)
{
    mantissa &= ~((uint64)1 << 52);
    if (exponent >= 2047)
    {
        _err_math(0x22, point);
        return sign | ((uint64)0x7FF00000u << 32);
    }
    /* Preserve the original 32-bit exponent shift even below zero */
    return sign | ((uint64)((uint32)exponent << 20) << 32) | mantissa;
}

uint64 __adddf3PSX(uint64 left, uint64 right)
{
    uint64 sign_mask = (uint64)1 << 63;
    if (!(left & ~sign_mask))
        return right;
    if (!(right & ~sign_mask))
        return left;
    sint32 left_exp = (sint32)((left >> 52) & 0x7FFu);
    sint32 right_exp = (sint32)((right >> 52) & 0x7FFu);
    if (left_exp > right_exp + 54)
        return left;
    if (right_exp > left_exp + 54)
        return right;
    uint64 a = math_mantissa64(left);
    uint64 b = math_mantissa64(right);
    if (left & sign_mask)
        a = 0u - a;
    if (right & sign_mask)
        b = 0u - b;
    a <<= 9;
    b <<= 9;
    sint32 exponent;
    if (left_exp > right_exp)
    {
        b = math_arithmetic_right64(b, (uint32)(left_exp - right_exp));
        exponent = left_exp;
    }
    else
    {
        a = math_arithmetic_right64(a, (uint32)(right_exp - left_exp));
        exponent = right_exp;
    }
    uint64 sum = a + b;
    if (!sum)
        return 0;
    uint64 sign = sum & sign_mask;
    if (sign)
        sum = 0u - sum;
    while (!(sum & ((uint64)0xE0000000u << 32)))
    {
        sum <<= 1;
        --exponent;
    }
    if (sum & ((uint64)1 << 62))
    {
        sum >>= 1;
        ++exponent;
    }
    sum += (sum & 0x200u) ? 0x100u : 0xFFu;
    if (sum & ((uint64)1 << 62))
    {
        sum >>= 1;
        ++exponent;
    }
    return math_pack64(sign, exponent, sum >> 9, 0x0B);
}

uint64 __muldf3PSX(uint64 left, uint64 right)
{
    uint64 sign_mask = (uint64)1 << 63;
    uint64 sign = (left ^ right) & sign_mask;
    if (!(left & ~sign_mask) || !(right & ~sign_mask))
        return sign;
    uint64 a = math_mantissa64(left);
    uint64 b = math_mantissa64(right);
    uint64 high_a = a >> 21;
    uint64 high_b = b >> 21;
    uint64 product = high_a * high_b;
    product += ((a & 0x1FFFFFu) * high_b) >> 21;
    product += ((b & 0x1FFFFFu) * high_a) >> 21;
    sint32 exponent = (sint32)((left >> 52) & 0x7FFu) + (sint32)((right >> 52) & 0x7FFu) - 1022;
    if (product & sign_mask)
        product = (product + 0x400u) >> 11;
    else
    {
        product += 0x200u;
        if (product & sign_mask)
            product >>= 11;
        else
        {
            product >>= 10;
            --exponent;
        }
    }
    return math_pack64(sign, exponent, product, 0x0D);
}

uint64 __divdf3PSX(uint64 left, uint64 right)
{
    uint64 sign_mask = (uint64)1 << 63;
    uint64 sign = (left ^ right) & sign_mask;
    if (!(right & ~sign_mask))
        return sign | ~sign_mask;
    if (!(left & ~sign_mask))
        return sign;
    uint64 dividend = math_mantissa64(left);
    uint64 divisor = math_mantissa64(right);
    sint32 exponent = (sint32)((left >> 52) & 0x7FFu) - (sint32)((right >> 52) & 0x7FFu) + 1022;
    if (dividend < divisor)
    {
        dividend <<= 1;
        --exponent;
    }
    uint64 quotient = 0;
    uint64 bit;
    for (bit = (uint64)1 << 53; bit; bit >>= 1)
    {
        if (dividend >= divisor)
        {
            quotient |= bit;
            dividend -= divisor;
        }
        dividend <<= 1;
    }
    uint32 shift;
    if (exponent >= 0)
    {
        quotient += 1u;
        ++exponent;
        shift = 1;
    }
    else
    {
        shift = 0u - (uint32)exponent;
        uint32 increment = (uint32)1 << (shift & 31u);
        quotient += increment;
        ++shift;
        exponent = 0;
    }
    quotient = shift >= 64u ? 0 : quotient >> shift;
    return math_pack64(sign, exponent, quotient, 0x0F);
}

double __extendsfdf2(float value)
{
    uint32 bits = psx_float_bits(value);
    uint64 sign = (uint64)(bits & 0x80000000u) << 32;
    uint32 fraction = bits & 0x7FFFFFu;
    uint32 exponent = (bits >> 23) & 0xFFu;
    if (!(bits & 0x7FFFFFFFu))
        return psx_double_value(sign);
    uint64 mantissa;
    if (exponent)
    {
        exponent += 0x380u;
        mantissa = (uint64)fraction << 29;
    }
    else
    {
        exponent = 0x37Eu;
        mantissa = (uint64)fraction << 32;
        while (mantissa >= ((uint64)1 << 53))
        {
            mantissa >>= 1;
            ++exponent;
        }
        while (!(mantissa & ((uint64)1 << 52)))
        {
            mantissa <<= 1;
            --exponent;
        }
        mantissa &= ~((uint64)1 << 52);
    }
    return psx_double_value(sign | ((uint64)exponent << 52) | mantissa);
}

sint32 __fixdfsi(double value)
{
    uint64 bits;
    memcpy(&bits, &value, sizeof(bits));
    uint64 sign = bits & ((uint64)1 << 63);
    uint32 exponent = (uint32)(bits >> 52) & 0x7FFu;
    if (!(bits & ~((uint64)1 << 63)))
        return sign ? (sint32)0x80000000u : 0;
    if (exponent > 1053u)
    {
        _err_math(0x22, 0x11);
        return sign ? (sint32)0x80000000u : 0x7FFFFFFF;
    }
    if (exponent < 1022u)
        return 0;
    uint64 mantissa = (bits & (((uint64)1 << 52) - 1u)) | ((uint64)1 << 52);
    uint32 magnitude = (uint32)(mantissa >> 22);
    magnitude >>= 1053u - exponent;
    return (sint32)(sign ? 0u - magnitude : magnitude);
}

double __floatsidf(sint32 value)
{
    uint32 magnitude = (uint32)value;
    uint64 sign = value < 0 ? (uint64)1 << 63 : 0u;
    uint32 exponent = 1053u;
    if (!magnitude)
        return psx_double_value(0u);
    if (sign)
        magnitude = 0u - magnitude;
    if ((sint32)magnitude <= 0x00FFFFFF)
    {
        do
        {
            magnitude <<= 4;
            exponent -= 4u;
        } while ((sint32)magnitude <= 0x00FFFFFF);
    }
    if ((sint32)magnitude <= 0x3FFFFFFF)
    {
        do
        {
            magnitude <<= 1;
            --exponent;
        } while ((sint32)magnitude <= 0x3FFFFFFF);
    }
    uint64 mantissa = ((uint64)magnitude << 22) & ~((uint64)1 << 52);
    return psx_double_value(sign | ((uint64)exponent << 52) | mantissa);
}

/* PsyQ omits the low-byte product and truncates each cross product separately */
uint32 __mulsf3PSX(uint32 left, uint32 right)
{
    uint32 sign = (left ^ right) & 0x80000000u;
    if (!(left & 0x7FFFFFFFu) || !(right & 0x7FFFFFFFu))
        return sign;
    uint32 left_mantissa = (left & 0x7FFFFFu) | 0x800000u;
    uint32 right_mantissa = (right & 0x7FFFFFu) | 0x800000u;
    uint32 left_high = left_mantissa >> 8;
    uint32 right_high = right_mantissa >> 8;
    uint32 product = left_high * right_high;
    product += ((left_mantissa & 0xFFu) * right_high) >> 8;
    product += ((right_mantissa & 0xFFu) * left_high) >> 8;
    sint32 exponent = (sint32)((left >> 23) & 0xFFu) + (sint32)((right >> 23) & 0xFFu) - 126;
    if (product & 0x80000000u)
        product = (product + 0x80u) >> 8;
    else
    {
        product += 0x40u;
        if (product & 0x80000000u)
            product >>= 8;
        else
        {
            product >>= 7;
            --exponent;
        }
    }
    product &= 0xFF7FFFFFu;
    if (exponent >= 255)
    {
        _err_math(0x22, 0x0C);
        return sign | 0x7F800000u;
    }
    return sign | ((uint32)exponent << 23) | product;
}

float __truncdfsf2(double value)
{
    uint64 bits;
    memcpy(&bits, &value, sizeof(bits));
    uint32 sign = (uint32)(bits >> 32) & 0x80000000u;
    if (!(bits & ~((uint64)1 << 63)))
        return psx_float_value(sign);
    sint32 exponent = (sint32)((bits >> 52) & 0x7FFu) - 0x380;
    uint64 mantissa = (bits & (((uint64)1 << 52) - 1u)) | ((uint64)1 << 52);
    uint32 rounded = (uint32)(mantissa >> 28);
    if (exponent > 0)
    {
        rounded = (rounded + 1u) >> 1;
        if (rounded & 0xFF000000u)
        {
            rounded >>= 1;
            ++exponent;
        }
    }
    else
    {
        uint32 shift = 0u - (uint32)exponent;
        rounded += (uint32)1 << ((shift + 1u) & 31u);
        rounded >>= (shift + 2u) & 31u;
        exponent = 0;
    }
    rounded &= 0xFF7FFFFFu;
    if (exponent >= 255)
    {
        _err_math(0x22, 0x10);
        return psx_float_value(sign | 0x7F800000u);
    }
    return psx_float_value(sign | ((uint32)exponent << 23) | rounded);
}

void xport_memory_control_write32_missing(uint32 address, uint32 value)
{
    (void)address;
    (void)value;
    abort();
}

uint32 SquareRoot0PSX(uint32 input, uint32 table_address)
{
    uint32 leading, even, normalized, value, shift;
    xport_gte_write_data(30u, input);
    leading = xport_gte_read_data(31u);
    uint32 result = 0u;
    if (leading != 32u)
    {
        even = leading & 0xFFFFFFFEu;
        shift = (uint32)((sint32)(31u - even) >> 1);
        if ((sint32)(even - 24u) < 0)
            normalized = (uint32)((sint32)input >> ((24u - even) & 31u));
        else
            normalized = input << ((even - 24u) & 31u);
        value = r_s16(table_address + 2u * (normalized - 64u));
        result = (value << (shift & 31u)) >> 12;
    }
    return result;
}

uint32 xport_sdk_texture_frame_acquire(size_t bytes)
{
    (void)bytes;
    abort();
}

void xport_sdk_texture_frame_release(uint32 address, size_t bytes)
{
    (void)address;
    (void)bytes;
    abort();
}

uint32 xport_sdk_draw_env_frame_acquire(size_t bytes)
{
    (void)bytes;
    abort();
}

void xport_sdk_draw_env_frame_release(uint32 address, size_t bytes)
{
    (void)address;
    (void)bytes;
    abort();
}

uint32 xport_guest_call_known_registers_post_sdk(uint32 target, uint32 *registers, uint32 known_register_mask)
{
    (void)target;
    (void)registers;
    (void)known_register_mask;
    abort();
}

uint32 xport_guest_call_known_registers(uint32 target, uint32 *registers, uint32 known_register_mask)
{
    (void)target;
    (void)registers;
    (void)known_register_mask;
    abort();
}

uint32 sprintfPSX(uint32 destination, uint32 format, uint32 value)
{
    (void)destination;
    (void)format;
    (void)value;
    abort();
}

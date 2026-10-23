#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#if defined(_WIN32) && !defined(XPORT_XCRT)
    #include <io.h>
    #include <fcntl.h>
#endif
#include "psx.h"
#include "psx_gpu.h"
#include "psx_mdec.h"

const MATRIX psx_identity_matrix = {{{4096, 0, 0}, {0, 4096, 0}, {0, 0, 4096}}, {0, 0, 0}};

const uint32 psx_identity_rotation_words[5] = {4096, 0, 4096, 0, 4096};

sint32 gte_matrix_near_identity(const MATRIX *matrix)
{
    sint32 row, column, difference, distance = 0;
    for (row = 0; row < 3; ++row)
    {
        for (column = 0; column < 3; ++column)
        {
            difference = matrix->m[row][column] - psx_identity_matrix.m[row][column];
            distance += difference < 0 ? -difference : difference;
        }
    }
    return distance < 20;
}

uint32 PAD_dr(void);

#if defined(XPORT_MEMORY_DIAGNOSTICS)
    #undef w_u8
    #undef w_u16
    #undef w_u32
    #undef w_masked_u32
#endif

size_t xport_fread(void *data, size_t size, size_t count, FILE *stream)
{
    return fread(data, size, count, stream);
}

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
        tail_link = current;
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

GDB_CALL uint32 r_masked_u32(uint32 address, uint32 byte_mask)
{
    return xport_memory_read_masked_u32(address, byte_mask);
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
static PsxRootClockSample native_root_clock_sample;
static void *native_root_clock_context;
static uint64 native_root_last_tick;
static sint32 native_video_mode;
void psx_root_counter_poll(void);
static void native_mdec_advance(uint64 ticks);
static void native_gpu_service(void);
static void mc_poll(void);
static void bios_native_irq_dispatch(void);
static sint32 bios_native_irq_ready(void);

static uint32 bios_interrupts_enabled = 1u;
static uint32 bios_vblank_pending;
static uint32 bios_vsync_callback;
static PsxInterruptCallback bios_vsync_native_callback;
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

uint32 _card_clear(uint32 channel)
{
    _new_card();
    return psx_bios_card_write_guest(channel, 0x3F, 0);
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

static PsxCardFormatGuestBinding card_format_guest;

void psx_card_bind_format_guest(const PsxCardFormatGuestBinding *binding)
{
    memset(&card_format_guest, 0, sizeof(card_format_guest));
    if (binding)
        card_format_guest = *binding;
}

static void card_format_clear(uint32 address, uint32 bytes)
{
    uint32 index;
    for (index = 0; index < bytes; ++index)
        w_u8(address + index, 0);
}

static void card_format_checksum(uint32 buffer)
{
    uint32 index, checksum = 0;
    for (index = 0; index < 127; ++index)
        checksum ^= r_u8(buffer + index);
    w_u8(buffer + 127, checksum);
}

static void card_format_wait(sint32 channel)
{
    uint32 status;
    do
    {
        psx_bios_card_poll();
        status = r_u8(0x7568 + (uint32)(channel >> 4));
    } while ((status & 1) == 0);
}

sint32 _card_format(sint32 channel)
{
    uint32 buffer, record, index, group, word, values[4], result;

    if (!card_format_guest.records || !card_format_guest.defects || !card_format_guest.sector_buffer)
        abort();
    buffer = card_format_guest.sector_buffer;
    record = card_format_guest.records;
    for (index = 0; index < 15; ++index)
    {
        card_format_clear(buffer, 128);
        card_format_clear(record, 32);
        w_u32(card_format_guest.records + index * 32, 0xA0);
        w_u32(card_format_guest.records + index * 32 + 4, 0);
        w_u16(card_format_guest.records + index * 32 + 8, 0xFFFF);
        for (group = 0; group < 2; ++group)
        {
            for (word = 0; word < 4; ++word)
                values[word] = r_u32(record + group * 16 + word * 4);
            for (word = 0; word < 4; ++word)
                w_u32(buffer + group * 16 + word * 4, values[word]);
        }
        card_format_checksum(buffer);
        _new_card();
        result = psx_bios_card_write_guest((uint32)channel, index + 1, buffer);
        if (result != 1)
            return 0;
        card_format_wait(channel);
        record += 32;
    }
    record = card_format_guest.defects;
    for (index = 0; index < 20; ++index)
    {
        w_u32(record, 0xFFFFFFFF);
        card_format_clear(buffer, 128);
        result = r_u32(record);
        w_u32(buffer, result);
        card_format_checksum(buffer);
        _new_card();
        result = psx_bios_card_write_guest((uint32)channel, index + 16, buffer);
        if (result != 1)
            return 0;
        card_format_wait(channel);
        record += 4;
    }
    card_format_clear(buffer, 128);
    w_u8(buffer, 77);
    w_u8(buffer + 1, 67);
    card_format_checksum(buffer);
    _new_card();
    result = psx_bios_card_write_guest((uint32)channel, 0, buffer);
    if (result != 1)
        return 0;
    card_format_wait(channel);
    card_format_clear(buffer, 128);
    _new_card();
    result = psx_bios_card_read_guest((uint32)channel, 0, buffer);
    if (result != 1)
        return 0;
    card_format_wait(channel);
    _new_card();
    result = psx_bios_card_load_guest((uint32)channel);
    if (result != 1)
        return 0;
    card_format_wait(channel);
    return 1;
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
static uint32 native_vsync_count;

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
static uint32 cooperative_wait_depth;
static uint32 cooperative_sync_depth;
static PsxInterruptCallback cooperative_services[7];
static uint32 cooperative_pending_services;
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
    cooperative_wait_depth = 0u;
    cooperative_sync_depth = 0u;
    vblank_wait_context = context;
    vblank_count = native_vsync_count = initial_count;
    vblank_wait_failed = 0;
    native_vblank_clock_started = 0u;
    native_vblank_fraction = 0u;
}

static sint32 psx_wait_events_internal(sint32 *provider_called)
{
    sint32 ok;
    *provider_called = 0;
    if (!vblank_wait || vblank_wait_failed)
        return 0;
    ++cooperative_wait_depth;
    if (bios_interrupts_enabled && (cooperative_pending_services || bios_native_irq_ready()))
        ok = 1;
    else if (bios_vblank_pending && bios_interrupts_enabled)
    {
        bios_vblank_pending = 0u;
        psx_vblank_signal();
        ok = !vblank_wait_failed;
    }
    else
    {
        *provider_called = 1;
        ok = vblank_wait(vblank_wait_context);
    }
    if (ok && !vblank_wait_failed)
        bios_native_irq_dispatch();
    --cooperative_wait_depth;
    return ok && !vblank_wait_failed;
}

sint32 psx_wait_events(void)
{
    sint32 provider_called;
    return psx_wait_events_internal(&provider_called);
}

#if defined(LOCKSTEP_DEBUG)
sint32 psx_wait_events_resume(void)
{
    sint32 ok;
    if (!vblank_wait || vblank_wait_failed)
        return 0;
    ++cooperative_wait_depth;
    ok = vblank_wait(vblank_wait_context);
    if (ok && !vblank_wait_failed)
        bios_native_irq_dispatch();
    --cooperative_wait_depth;
    return ok && !vblank_wait_failed;
}
#endif

sint32 psx_callbacks_allowed(void)
{
    return bios_interrupts_enabled && (!vblank_wait || cooperative_wait_depth || cooperative_sync_depth);
}

static sint32 psx_wait_one_vblank(uint32 *counter)
{
    uint32 before = *counter;
    do
    {
        sint32 provider_called;
        if (!psx_wait_events_internal(&provider_called) || vblank_wait_failed || (provider_called && *counter == before))
            return 0;
    } while (*counter == before);
    return *counter - before == 1;
}

void psx_wait_bind_service(uint32 slot, PsxInterruptCallback service)
{
    if (slot >= 7)
        abort();
    cooperative_services[slot] = service;
    cooperative_pending_services &= ~(1u << slot);
}

void psx_wait_signal_service(uint32 slot)
{
    if (slot >= 7)
        abort();
    cooperative_pending_services |= 1u << slot;
}

void psx_vblank_signal(void)
{
    uint32 index;
    if (vblank_wait && !cooperative_wait_depth)
    {
        bios_vblank_pending = 1u;
        return;
    }
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
    if (bios_callbacks_running && bios_vsync_native_callback)
        bios_vsync_native_callback();
    else if (bios_callbacks_running && bios_vsync_callback)
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
        if (!psx_wait_one_vblank(vblank_guest_counter) || *vblank_guest_counter - before != 1u)
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
        if (!psx_wait_one_vblank(vblank_guest_counter) || *vblank_guest_counter - before != 1u)
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

sint32 psx_root_counter_import_irq(const PsxIrqState *state, const uint8 fired[3])
{
    uint32 index;
    if (!psx_irq_validate(state) || !fired)
        return 0;
    for (index = 0; index < 3; ++index)
        if (fired[index] > 1)
            return 0;
    native_root_irq = *state;
    memcpy(native_timer_irq_fired, fired, sizeof(native_timer_irq_fired));
    return 1;
}

static void psx_timer_irq(uint32 index)
{
    uint32 mode = timers.mode[index];
    if (!(mode & 0x40u) && native_timer_irq_fired[index])
        return;
    native_timer_irq_fired[index] = 1u;
    psx_irq_line(&native_root_irq, index + 4u, 0);
    psx_irq_line(&native_root_irq, index + 4u, 1);
    bios_native_irq_dispatch();
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
            if ((mode & 8u) && target != 0xFFFFu)
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

PsxCallbackDriver psx_callback_driver;
static PsxInterruptCallback native_irq_callbacks[11];
static PsxInterruptCallback native_vsync_callbacks[8];

PsxInterruptCallback setIntrVSync(uint32 slot, PsxInterruptCallback callback)
{
    PsxInterruptCallback previous;
    if (slot >= sizeof(native_vsync_callbacks) / sizeof(native_vsync_callbacks[0]))
        return NULL;
    previous = native_vsync_callbacks[slot];
    if (callback != previous)
        native_vsync_callbacks[slot] = callback;
    return previous;
}

void trapIntrVSync(void)
{
    uint32 slot;
    PsxInterruptCallback callback;
    for (slot = 0; slot < sizeof(native_vsync_callbacks) / sizeof(native_vsync_callbacks[0]); ++slot)
    {
        callback = native_vsync_callbacks[slot];
        if (callback != NULL)
            callback();
    }
}

static sint32 bios_native_irq_ready(void)
{
    uint32 interrupt, pending = native_root_irq.status & native_root_irq.mask;
    for (interrupt = 0; interrupt < 11; ++interrupt)
        if ((pending & (1u << interrupt)) && (native_irq_callbacks[interrupt] || interrupt == 0 || (interrupt >= 4 && interrupt <= 6)))
            return 1;
    return 0;
}

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
    if (!bios_interrupts_enabled || native_irq_dispatching || (vblank_wait && !cooperative_wait_depth && !cooperative_sync_depth))
        return;
    native_irq_dispatching = 1u;
    for (interrupt = 0u; interrupt < 11u && bios_interrupts_enabled; ++interrupt)
    {
        uint32 bit = 1u << interrupt;
        if (cooperative_sync_depth && !cooperative_wait_depth && !(bit & 0x20eu))
            continue;
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
    {
        uint32 services = 0x7f;
        while (services && psx_callbacks_allowed())
        {
            for (interrupt = 0; interrupt < 7 && psx_callbacks_allowed(); ++interrupt)
            {
                if (!(services & (1u << interrupt)))
                    continue;
                cooperative_pending_services &= ~(1u << interrupt);
                if (cooperative_services[interrupt])
                    cooperative_services[interrupt]();
            }
            /* A completion handler can submit another synchronous transfer */
            services = cooperative_pending_services;
        }
        cd_dispatch_ready_event();
    }
    native_irq_dispatching = 0u;
}

void psx_sync_callbacks(void)
{
    if (!bios_interrupts_enabled || cooperative_sync_depth)
        return;
    ++cooperative_sync_depth;
    bios_native_irq_dispatch();
    --cooperative_sync_depth;
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
    if (psx_callback_driver.initialized != 0)
        return 0;
    vsync_callback = NULL;
    memset(native_irq_callbacks, 0, sizeof(native_irq_callbacks));
    memset(native_vsync_callbacks, 0, sizeof(native_vsync_callbacks));
    psx_callback_driver.initialized = 1u;
    psx_callback_driver.saved_interrupt_mask = 0;
    psx_callback_driver.saved_dma_control = 0;
#if defined(XPORT_NATIVE_CALLBACK_BOOTSTRAP) && XPORT_NATIVE_CALLBACK_BOOTSTRAP
    bios_vsync_callback = 0;
    bios_vsync_native_callback = NULL;
    bios_callbacks_running = 1;
    psx_irq_native_mask_exchange(0);
    psx_irq_native_acknowledge(0x7FF);
    // Retain device setup without the guest exception context or dispatch vectors
    w_u32(0x1F8010F0, 0x33333333);
    w_u32(0x1F801114, 0x107);
    ChangeClearPAD(0);
    ChangeClearRCnt(3, 0);
    w_u32(0x1F8010F4, 0);
    psx_irq_native_mask_exchange(9);
    xport_bios_exit_critical();
#endif
    return 0;
}

PsxCallbackDriver *stopIntr(void)
{
    if (psx_callback_driver.initialized == 0)
        return NULL;
    xport_bios_enter_critical();
    psx_callback_driver.saved_interrupt_mask = r_u16(0x1F801074u);
    psx_callback_driver.saved_dma_control = r_u32(0x1F8010F0u);
    w_u16(0x1F801074u, 0);
    w_u16(0x1F801070u, r_u16(0x1F801074u));
    w_u32(0x1F8010F0u, r_u32(0x1F8010F0u) & 0x77777777);
    psx_bios_reset_entry_interrupt();
    psx_callback_driver.initialized = 0;
    return &psx_callback_driver;
}

PsxCallbackDriver *restartIntr(void)
{
    if (psx_callback_driver.initialized != 0)
        return NULL;
    psx_callback_driver.initialized = 1;
    w_u16(0x1F801074u, psx_callback_driver.saved_interrupt_mask);
    w_u32(0x1F8010F0u, psx_callback_driver.saved_dma_control);
    xport_bios_exit_critical();
    return &psx_callback_driver;
}

intptr VSyncCallback(PsxInterruptCallback callback)
{
    PsxInterruptCallback previous = vsync_callback;
    vsync_callback = callback;
    return (intptr)previous;
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

// PsyQ COR_06 retains all twelve steps, including zero increments
static const uint32 psyq_catan_steps[12] = {511, 302, 159, 81, 41, 20, 10, 5, 3, 1, 0, 0};

sint32 catan(sint32 a)
{
    uint32 x = 4096, y = (uint32)a, angle = 0, step;
    for (step = 0; step < 12; ++step)
    {
        uint32 x_shift = (uint32)((sint32)x >> step);
        uint32 y_shift = (uint32)((sint32)y >> step);
        if ((sint32)y >= 0)
        {
            x += y_shift;
            y -= x_shift;
            angle += psyq_catan_steps[step];
        }
        else
        {
            x -= y_shift;
            y += x_shift;
            angle -= psyq_catan_steps[step];
        }
    }
    return (sint32)angle;
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

static sint32 psyq_atan_ratio(uint32 numerator, uint32 denominator)
{
    if (denominator == 0)
        xport_mips_break(7);
    if (denominator == 0xFFFFFFFF && numerator == 0x80000000)
        xport_mips_break(6);
    return (sint32)((sint64)(sint32)numerator / (sint32)denominator);
}

uint64 __divdi3PSX(uint64 dividend, uint64 divisor)
{
    uint32 negative = (uint32)((dividend ^ divisor) >> 63);
    uint64 quotient;
    if ((dividend >> 63) != 0)
        dividend = (uint64)0 - dividend;
    if ((divisor >> 63) != 0)
        divisor = (uint64)0 - divisor;
    if (divisor == 0)
        xport_mips_break(7);
    quotient = dividend / divisor;
    return negative ? (uint64)0 - quotient : quotient;
}

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
        ratio = ((uint32)y & 0x7FE00000) != 0 ? psyq_atan_ratio((uint32)y, (uint32)(x >> 10)) : psyq_atan_ratio((uint32)y << 10, (uint32)x);
        angle = psyq_atan_table[ratio];
    }
    else
    {
        ratio = ((uint32)x & 0x7FE00000) != 0 ? psyq_atan_ratio((uint32)x, (uint32)(y >> 10)) : psyq_atan_ratio((uint32)x << 10, (uint32)y);
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
    uint32 saved[3], words[3];
    if (!left || !right || !output)
        return output;
    saved[0] = xport_gte_read_control(0);
    saved[1] = xport_gte_read_control(2);
    saved[2] = xport_gte_read_control(4);
    words[0] = xport_load_le32((const uint8 *)left);
    words[1] = xport_load_le32((const uint8 *)left + 4);
    words[2] = xport_load_le32((const uint8 *)left + 8);
    xport_gte_write_control(0, words[0]);
    xport_gte_write_control(2, words[1]);
    xport_gte_write_control(4, words[2]);
    xport_gte_write_data(11, xport_load_le32((const uint8 *)right + 8));
    xport_gte_write_data(9, xport_load_le32((const uint8 *)right));
    xport_gte_write_data(10, xport_load_le32((const uint8 *)right + 4));
    xport_gte_execute(0x170000C);
    xport_store_le32((uint8 *)output, xport_gte_read_data(25));
    xport_store_le32((uint8 *)output + 4, xport_gte_read_data(26));
    xport_store_le32((uint8 *)output + 8, xport_gte_read_data(27));
    xport_gte_write_control(0, saved[0]);
    xport_gte_write_control(2, saved[1]);
    xport_gte_write_control(4, saved[2]);
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

VECTOR *ApplyRotMatrixLV(const VECTOR *source, VECTOR *destination)
{
    uint32 input[3], low[3], high[3], axis;
    input[0] = source->vx;
    input[1] = source->vy;
    input[2] = source->vz;
    for (axis = 0; axis < 3; ++axis)
    {
        uint32 magnitude = input[axis];
        if ((sint32)magnitude < 0)
        {
            magnitude = 0 - magnitude;
            high[axis] = 0 - (uint32)((sint32)magnitude >> 15);
            low[axis] = 0 - (magnitude & 0x7FFF);
        }
        else
        {
            high[axis] = magnitude >> 15;
            low[axis] = magnitude & 0x7FFF;
        }
    }
    for (axis = 0; axis < 3; ++axis)
        xport_gte_write_data(9 + axis, high[axis]);
    xport_gte_execute(0x41E012);
    for (axis = 0; axis < 3; ++axis)
        high[axis] = xport_gte_read_data(25 + axis);
    for (axis = 0; axis < 3; ++axis)
        xport_gte_write_data(9 + axis, low[axis]);
    xport_gte_execute(0x49E012);
    for (axis = 0; axis < 3; ++axis)
        input[axis] = xport_gte_read_data(25 + axis) + (high[axis] << 3);
    destination->vx = input[0];
    destination->vy = input[1];
    destination->vz = input[2];
    return destination;
}

uint32 ApplyRotMatrixLVPSX(uint32 source, uint32 destination)
{
    VECTOR input, output;
    input.vx = r_u32(source);
    input.vy = r_u32(source + 4);
    input.vz = r_u32(source + 8);
    ApplyRotMatrixLV(&input, &output);
    w_u32(destination, output.vx);
    w_u32(destination + 4, output.vy);
    w_u32(destination + 8, output.vz);
    return destination;
}

static uint32 mul_rot_matrix_psx(uint32 source, uint32 destination, uint32 first)
{
    uint32 second, third, column1[3], column2[3];
    second = r_u32(source + 4);
    third = r_u32(source + 12);
    first = (first & 0xFFFF) | (second & 0xFFFF0000);
    xport_gte_write_data(0, first);
    xport_gte_write_data(1, third);
    xport_gte_execute(0x486012);
    first = r_u16(source + 2);
    second = r_u32(source + 8);
    third = (uint32)(sint32)r_s16(source + 14);
    first |= second << 16;
    column1[0] = xport_gte_read_data(9);
    column1[1] = xport_gte_read_data(10);
    column1[2] = xport_gte_read_data(11);
    xport_gte_write_data(0, first);
    xport_gte_write_data(1, third);
    xport_gte_execute(0x486012);
    first = r_u16(source + 4);
    second = r_u32(source + 8);
    third = r_u32(source + 16);
    first |= second & 0xFFFF0000;
    column2[0] = xport_gte_read_data(9);
    column2[1] = xport_gte_read_data(10);
    column2[2] = xport_gte_read_data(11);
    xport_gte_write_data(0, first);
    xport_gte_write_data(1, third);
    xport_gte_execute(0x486012);
    w_u32(destination, (column1[0] & 0xFFFF) | (column2[0] << 16));
    w_u32(destination + 12, (column1[2] & 0xFFFF) | (column2[2] << 16));
    first = xport_gte_read_data(9);
    second = xport_gte_read_data(10);
    w_u32(destination + 4, (first & 0xFFFF) | (column1[1] << 16));
    w_u32(destination + 8, (column2[1] & 0xFFFF) | (second << 16));
    w_u32(destination + 16, xport_gte_read_data(11));
    return destination;
}

static MATRIX *mul_rot_matrix_native(const MATRIX *source, MATRIX *destination, uint32 first)
{
    const uint8 *input = (const uint8 *)source;
    uint8 *output = (uint8 *)destination;
    uint32 second, third, column1[3], column2[3];
    second = xport_load_le32(input + 4);
    third = xport_load_le32(input + 12);
    first = (first & 0xFFFF) | (second & 0xFFFF0000);
    xport_gte_write_data(0, first);
    xport_gte_write_data(1, third);
    xport_gte_execute(0x486012);
    first = xport_load_le16(input + 2);
    second = xport_load_le32(input + 8);
    third = (uint32)(sint32)(sint16)xport_load_le16(input + 14);
    first |= second << 16;
    column1[0] = xport_gte_read_data(9);
    column1[1] = xport_gte_read_data(10);
    column1[2] = xport_gte_read_data(11);
    xport_gte_write_data(0, first);
    xport_gte_write_data(1, third);
    xport_gte_execute(0x486012);
    first = xport_load_le16(input + 4);
    second = xport_load_le32(input + 8);
    third = xport_load_le32(input + 16);
    first |= second & 0xFFFF0000;
    column2[0] = xport_gte_read_data(9);
    column2[1] = xport_gte_read_data(10);
    column2[2] = xport_gte_read_data(11);
    xport_gte_write_data(0, first);
    xport_gte_write_data(1, third);
    xport_gte_execute(0x486012);
    xport_store_le32(output, (column1[0] & 0xFFFF) | (column2[0] << 16));
    xport_store_le32(output + 12, (column1[2] & 0xFFFF) | (column2[2] << 16));
    first = xport_gte_read_data(9);
    second = xport_gte_read_data(10);
    xport_store_le32(output + 4, (first & 0xFFFF) | (column1[1] << 16));
    xport_store_le32(output + 8, (column2[1] & 0xFFFF) | (second << 16));
    xport_store_le32(output + 16, xport_gte_read_data(11));
    return destination;
}

MATRIX *MulRotMatrix(MATRIX *matrix)
{
    return mul_rot_matrix_native(matrix, matrix, xport_load_le32((const uint8 *)matrix));
}

MATRIX *MulRotMatrix0(const MATRIX *source, MATRIX *destination)
{
    return mul_rot_matrix_native(source, destination, xport_load_le16((const uint8 *)source));
}

uint32 MulRotMatrixPSX(uint32 matrix)
{
    return mul_rot_matrix_psx(matrix, matrix, r_u32(matrix));
}

uint32 MulRotMatrix0PSX(uint32 source, uint32 destination)
{
    return mul_rot_matrix_psx(source, destination, r_u16(source));
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
    return MulMatrix0(source, destination, destination);
}

MATRIX *SetMulMatrix(MATRIX *left, MATRIX *right)
{
    MATRIX result;
    MulMatrix0(left, right, &result);
    SetRotMatrix(&result);
    return left;
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

void psx_gte_save_context(PsxGteContext *context)
{
    if (!context)
        return;
    context->registers = gte;
    context->lzcs = gte_lzcs;
    context->lzcr = gte_lzcr;
    context->ofx_frac = native_gte_ofx_fraction;
    context->ofy_frac = native_gte_ofy_fraction;
}

void psx_gte_restore_context(const PsxGteContext *context)
{
    if (!context)
        return;
    gte = context->registers;
    gte_lzcs = context->lzcs;
    gte_lzcr = context->lzcr;
    native_gte_ofx_fraction = context->ofx_frac;
    native_gte_ofy_fraction = context->ofy_frac;
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
    uint32 words[5], x, y, z;
    uint32 index;
    if (!m || !v || !out)
        return out;
    for (index = 0; index < 5; ++index)
        words[index] = xport_load_le32((const uint8 *)m + index * 4);
    for (index = 0; index < 5; ++index)
        xport_gte_write_control(index, words[index]);
    xport_gte_write_data(0, xport_load_le32((const uint8 *)v));
    xport_gte_write_data(1, xport_load_le32((const uint8 *)v + 4));
    xport_gte_execute(0x486012);
    x = xport_gte_read_data(9);
    y = xport_gte_read_data(10);
    z = xport_gte_read_data(11);
    xport_store_le16((uint8 *)out, (uint16)x);
    xport_store_le16((uint8 *)out + 2, (uint16)y);
    xport_store_le16((uint8 *)out + 4, (uint16)z);
    return out;
}

MATRIXF *IdentityMatrixf(MATRIXF *matrix)
{
    memset(matrix, 0, sizeof(*matrix));
    matrix->m[0][0] = matrix->m[1][1] = matrix->m[2][2] = 1.0f;
    return matrix;
}

VECTORF *ApplyMatrixf(const MATRIXF *matrix, const VECTORF *src, VECTORF *dst)
{
    VECTORF result;
    result.vx = matrix->m[0][0] * src->vx + matrix->m[0][1] * src->vy + matrix->m[0][2] * src->vz;
    result.vy = matrix->m[1][0] * src->vx + matrix->m[1][1] * src->vy + matrix->m[1][2] * src->vz;
    result.vz = matrix->m[2][0] * src->vx + matrix->m[2][1] * src->vy + matrix->m[2][2] * src->vz;
    *dst = result;
    return dst;
}

VECTORF *RotTransf(const MATRIXF *matrix, const VECTORF *src, VECTORF *dst)
{
    ApplyMatrixf(matrix, src, dst);
    dst->vx += matrix->t[0];
    dst->vy += matrix->t[1];
    dst->vz += matrix->t[2];
    return dst;
}

MATRIXF *MulMatrix0f(const MATRIXF *left, const MATRIXF *right, MATRIXF *dst)
{
    float result[3][3];
    for (uint32 row = 0; row < 3; ++row)
        for (uint32 column = 0; column < 3; ++column)
            result[row][column] = left->m[row][0] * right->m[0][column] + left->m[row][1] * right->m[1][column] + left->m[row][2] * right->m[2][column];
    memcpy(dst->m, result, sizeof(result));
    return dst;
}

MATRIXF *MulMatrixf(MATRIXF *left, const MATRIXF *right)
{
    return MulMatrix0f(left, right, left);
}

MATRIXF *MulMatrix2f(const MATRIXF *left, MATRIXF *right)
{
    return MulMatrix0f(left, right, right);
}

MATRIXF *CompMatrixf(const MATRIXF *left, const MATRIXF *right, MATRIXF *dst)
{
    MATRIXF result;
    VECTORF pos = {right->t[0], right->t[1], right->t[2]};
    MulMatrix0f(left, right, &result);
    RotTransf(left, &pos, &pos);
    result.t[0] = pos.vx;
    result.t[1] = pos.vy;
    result.t[2] = pos.vz;
    *dst = result;
    return dst;
}

MATRIXF *TransposeMatrixf(const MATRIXF *src, MATRIXF *dst)
{
    float result[3][3];
    for (uint32 row = 0; row < 3; ++row)
        for (uint32 column = 0; column < 3; ++column)
            result[row][column] = src->m[column][row];
    memcpy(dst->m, result, sizeof(result));
    return dst;
}

MATRIXF *RotMatrixXf(float angle, MATRIXF *matrix)
{
    float s = sinf(angle), c = cosf(angle);
    for (uint32 column = 0; column < 3; ++column)
    {
        float y = matrix->m[1][column], z = matrix->m[2][column];
        matrix->m[1][column] = c * y - s * z;
        matrix->m[2][column] = s * y + c * z;
    }
    return matrix;
}

MATRIXF *RotMatrixYf(float angle, MATRIXF *matrix)
{
    float s = sinf(angle), c = cosf(angle);
    for (uint32 column = 0; column < 3; ++column)
    {
        float x = matrix->m[0][column], z = matrix->m[2][column];
        matrix->m[0][column] = c * x + s * z;
        matrix->m[2][column] = -s * x + c * z;
    }
    return matrix;
}

MATRIXF *RotMatrixZf(float angle, MATRIXF *matrix)
{
    float s = sinf(angle), c = cosf(angle);
    for (uint32 column = 0; column < 3; ++column)
    {
        float x = matrix->m[0][column], y = matrix->m[1][column];
        matrix->m[0][column] = c * x - s * y;
        matrix->m[1][column] = s * x + c * y;
    }
    return matrix;
}

MATRIXF *RotMatrixf(const VECTORF *angles, MATRIXF *matrix)
{
    MATRIXF result;
    IdentityMatrixf(&result);
    RotMatrixZf(angles->vz, &result);
    RotMatrixYf(angles->vy, &result);
    RotMatrixXf(angles->vx, &result);
    memcpy(matrix->m, result.m, sizeof(result.m));
    return matrix;
}

MATRIXF *RotMatrixYXZf(const VECTORF *angles, MATRIXF *matrix)
{
    MATRIXF result;
    IdentityMatrixf(&result);
    RotMatrixZf(angles->vz, &result);
    RotMatrixXf(angles->vx, &result);
    RotMatrixYf(angles->vy, &result);
    memcpy(matrix->m, result.m, sizeof(result.m));
    return matrix;
}

MATRIXF *ScaleMatrixf(MATRIXF *matrix, const VECTORF *scale)
{
    for (uint32 row = 0; row < 3; ++row)
    {
        matrix->m[row][0] *= scale->vx;
        matrix->m[row][1] *= scale->vy;
        matrix->m[row][2] *= scale->vz;
    }
    return matrix;
}

MATRIXF *TransMatrixf(MATRIXF *matrix, const VECTORF *pos)
{
    matrix->t[0] = pos->vx;
    matrix->t[1] = pos->vy;
    matrix->t[2] = pos->vz;
    return matrix;
}

VECTORF *OuterProductf(const VECTORF *left, const VECTORF *right, VECTORF *dst)
{
    VECTORF result;
    result.vx = left->vy * right->vz - left->vz * right->vy;
    result.vy = left->vz * right->vx - left->vx * right->vz;
    result.vz = left->vx * right->vy - left->vy * right->vx;
    *dst = result;
    return dst;
}

float NormalizeVectorf(const VECTORF *src, VECTORF *dst)
{
    float length = sqrtf(src->vx * src->vx + src->vy * src->vy + src->vz * src->vz);
    VECTORF result = {0};
    if (length > 0)
    {
        result.vx = src->vx / length;
        result.vy = src->vy / length;
        result.vz = src->vz / length;
    }
    *dst = result;
    return length;
}

float NormalizeQuatf(const QUATF *src, QUATF *dst)
{
    float length = sqrtf(src->x * src->x + src->y * src->y + src->z * src->z + src->w * src->w);
    QUATF result = {{0, 0, 0, 1}};
    if (length > 0)
        for (uint32 idx = 0; idx < 4; ++idx)
            result.v[idx] = src->v[idx] / length;
    *dst = result;
    return length;
}

QUATF *SlerpQuatf(const QUATF *previous, const QUATF *current, float fract, QUATF *dst)
{
    QUATF a, b, result;
    NormalizeQuatf(previous, &a);
    NormalizeQuatf(current, &b);
    fract = fminf(1, fmaxf(0, fract));
    float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (dot < 0)
    {
        dot = -dot;
        for (uint32 idx = 0; idx < 4; ++idx)
            b.v[idx] = -b.v[idx];
    }
    float weight0 = 1 - fract;
    float weight1 = fract;
    if (dot < 0.9995f)
    {
        float angle = atan2f(sqrtf(fmaxf(0, 1 - dot * dot)), dot);
        float denominator = sinf(angle);
        weight0 = sinf((1 - fract) * angle) / denominator;
        weight1 = sinf(fract * angle) / denominator;
    }
    for (uint32 idx = 0; idx < 4; ++idx)
        result.v[idx] = a.v[idx] * weight0 + b.v[idx] * weight1;
    NormalizeQuatf(&result, dst);
    return dst;
}

QUATF *MulQuatf(const QUATF *left, const QUATF *right, QUATF *dst)
{
    QUATF result;
    result.x = left->w * right->x + right->w * left->x + left->y * right->z - left->z * right->y;
    result.y = left->w * right->y + right->w * left->y + left->z * right->x - left->x * right->z;
    result.z = left->w * right->z + right->w * left->z + left->x * right->y - left->y * right->x;
    result.w = left->w * right->w - left->x * right->x - left->y * right->y - left->z * right->z;
    *dst = result;
    return dst;
}

MATRIXF *QuatMatrixf(const QUATF *quat, MATRIXF *matrix)
{
    QUATF q;
    NormalizeQuatf(quat, &q);
    matrix->m[0][0] = 1 - 2 * (q.y * q.y + q.z * q.z);
    matrix->m[1][1] = 1 - 2 * (q.x * q.x + q.z * q.z);
    matrix->m[2][2] = 1 - 2 * (q.x * q.x + q.y * q.y);
    matrix->m[1][0] = 2 * (q.x * q.y + q.w * q.z);
    matrix->m[0][1] = 2 * (q.x * q.y - q.w * q.z);
    matrix->m[2][0] = 2 * (q.x * q.z - q.w * q.y);
    matrix->m[0][2] = 2 * (q.x * q.z + q.w * q.y);
    matrix->m[2][1] = 2 * (q.y * q.z + q.w * q.x);
    matrix->m[1][2] = 2 * (q.y * q.z - q.w * q.x);
    return matrix;
}

QUATF *MatrixQuatf(const MATRIXF *matrix, QUATF *quat)
{
    QUATF result;
    float trace = matrix->m[0][0] + matrix->m[1][1] + matrix->m[2][2];
    if (trace > 0)
    {
        float scale = 2 * sqrtf(1 + trace);
        result.w = scale * 0.25f;
        result.x = (matrix->m[2][1] - matrix->m[1][2]) / scale;
        result.y = (matrix->m[0][2] - matrix->m[2][0]) / scale;
        result.z = (matrix->m[1][0] - matrix->m[0][1]) / scale;
    }
    else
    {
        uint32 i = matrix->m[1][1] > matrix->m[0][0] ? 1 : 0;
        if (matrix->m[2][2] > matrix->m[i][i])
            i = 2;
        uint32 j = (i + 1) % 3, k = (i + 2) % 3;
        float scale = 2 * sqrtf(1 + matrix->m[i][i] - matrix->m[j][j] - matrix->m[k][k]);
        result.v[i] = scale * 0.25f;
        result.v[j] = (matrix->m[j][i] + matrix->m[i][j]) / scale;
        result.v[k] = (matrix->m[k][i] + matrix->m[i][k]) / scale;
        result.w = (matrix->m[k][j] - matrix->m[j][k]) / scale;
    }
    NormalizeQuatf(&result, quat);
    return quat;
}

void ProjectVertexf(const VECTORF *src, float focal_x, float focal_y, float near_z, float far_z, float clip[4])
{
    clip[0] = src->vx * focal_x;
    clip[1] = -src->vy * focal_y;
    clip[2] = src->vz * (far_z + near_z) / (far_z - near_z) - 2.0f * far_z * near_z / (far_z - near_z);
    clip[3] = src->vz;
}

void RotTransPersf(const MATRIXF *matrix, const VECTORF *src, float focal_x, float focal_y, float near_z, float far_z, float clip[4])
{
    VECTORF point;
    RotTransf(matrix, src, &point);
    ProjectVertexf(&point, focal_x, focal_y, near_z, far_z, clip);
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
static MATRIX *gte_rot_matrix_components(sint32 sx, sint32 cx, sint32 sy, sint32 cy, sint32 sz, sint32 cz, MATRIX *m)
{
    sint32 nsy, czn, szn;
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

MATRIX *RotMatrix(SVECTOR *r, MATRIX *m)
{
    sint32 sx, cx, sy, cy, sz, cz;
    if (!r || !m)
        return m;
    rsincos(r->vx, &sx, &cx);
    rsincos(r->vy, &sy, &cy);
    rsincos(r->vz, &sz, &cz);
    return gte_rot_matrix_components(sx, cx, sy, cy, sz, cz, m);
}

MATRIX *gte_rot_matrix_scalar(SVECTOR *rotation, MATRIX *matrix)
{
    sint32 sx, cx, sy, cy, sz, cz;
    if (!rotation || !matrix)
        return matrix;
    cx = rcos(rotation->vx);
    cy = rcos(rotation->vy);
    cz = rcos(rotation->vz);
    sx = rsin(rotation->vx);
    sy = rsin(rotation->vy);
    sz = rsin(rotation->vz);
    return gte_rot_matrix_components(sx, cx, sy, cy, sz, cz, matrix);
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

uint32 xport_gte_read_control(uint32 index)
{
    const MATRIX *matrix;
    uint32 component, value;
    if (index < 5 || (index >= 8 && index <= 12) || (index >= 16 && index <= 20))
    {
        matrix = index < 5 ? &gte.rotation : index < 16 ? &gte.light : &gte.color;
        component = (index - (index < 5 ? 0 : index < 16 ? 8 : 16)) * 2;
        value = (uint16)matrix->m[component / 3][component % 3];
        if (component == 8)
            return (uint32)(sint32)(sint16)value;
        ++component;
        return value | ((uint32)(uint16)matrix->m[component / 3][component % 3] << 16);
    }
    if (index >= 5 && index <= 7)
        return (uint32)gte.translation[index - 5];
    if (index >= 13 && index <= 15)
        return (uint32)gte.back_color[index - 13];
    if (index >= 21 && index <= 23)
        return (uint32)gte.far_color[index - 21];
    switch (index)
    {
        case 24:
            return ((uint32)gte.ofx << 16) | native_gte_ofx_fraction;
        case 25:
            return ((uint32)gte.ofy << 16) | native_gte_ofy_fraction;
        case 26:
            return (uint32)(sint32)(sint16)gte.h;
        case 27:
            return (uint32)(sint32)(sint16)gte.dqa;
        case 28:
            return (uint32)gte.dqb;
        case 29:
            return (uint32)(sint32)(sint16)gte.zsf3;
        case 30:
            return (uint32)(sint32)(sint16)gte.zsf4;
        case 31:
            return (uint32)gte.flag;
        default:
            abort();
    }
}

uint32 xport_gte_read_translation(uint32 axis)
{
    if (axis >= 3u)
        abort();
    return (uint32)gte.translation[axis];
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

sint32 RotTransPers4(const SVECTOR *a, const SVECTOR *b, const SVECTOR *c, const SVECTOR *d, sint32 *xy0, sint32 *xy1, sint32 *xy2, sint32 *xy3, sint32 *projection, sint32 *flags)
{
    SVECTOR input[3];
    sint32 screen[3], depth[3], first_flags, last_flags, result;
    input[0] = *a;
    input[1] = *b;
    input[2] = *c;
    gte_project3_full_depth(input, screen, depth, &first_flags);
    *xy0 = screen[0];
    *xy1 = screen[1];
    *xy2 = screen[2];
    result = gte_project(d, xy3, &last_flags);
    *projection = (sint32)xport_gte_read_data(8);
    *flags = first_flags | last_flags;
    return result;
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
    fields = (elapsed / 1000000u) * FIELD_RATE_NUMERATOR / FIELD_RATE_DENOMINATOR;
    native_vblank_fraction += ((elapsed / 1000000u) * FIELD_RATE_NUMERATOR % FIELD_RATE_DENOMINATOR) * 1000000u + (elapsed % 1000000u) * FIELD_RATE_NUMERATOR;
    fields += native_vblank_fraction / ((uint64)1000000u * FIELD_RATE_DENOMINATOR);
    native_vblank_fraction %= (uint64)1000000u * FIELD_RATE_DENOMINATOR;
    present_field = fields != 0u;
    while (fields && !vblank_wait_failed)
    {
        --fields;
        psx_vblank_signal();
        // Keep the host responsive while servicing elapsed fields after a stall
        if (fields && (fields & 31u) == 0u)
            xport_poll();
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
    if (!vblank_wait && gpu_frame_sync(frames))
        return (sint32)frames;
    // Count fields elapsed since the preceding blocking native VSync
    for (frame = vblank_wait ? 0 : vblank_count - native_vsync_count; frame < frames; ++frame)
    {
        if (vblank_wait)
        {
            uint32 before = vblank_count;
            if (!psx_wait_one_vblank(&vblank_count) || vblank_count - before != 1u)
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
                xport_timer_wait_frame(xport_frame() + 1);
                psx_native_vblank_poll();
            } while (vblank_count == before && !vblank_wait_failed);
        }
        if (vblank_wait_failed)
            return -1;
    }
    if (!vblank_wait)
        native_vsync_count = vblank_count;
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
    if (!psx_state_block(f, &cooperative_pending_services, sizeof(cooperative_pending_services), load))
        return 0;
    if (!psx_state_block(f, &gte, sizeof(gte), load) || !psx_state_block(f, &matrix_stack, sizeof(matrix_stack), load) || !psx_state_block(f, &matrix_stack_depth, sizeof(matrix_stack_depth), load) || !psx_state_block(f, &vblank_count, sizeof(vblank_count), load))
        return 0;
    if (load)
    {
        if (!psx_state_block(f, &restored_timers, sizeof(restored_timers), 1) || !psx_timers_import(&restored_timers))
            return 0;
    }
    else if (!psx_state_block(f, &timers, sizeof(timers), 0))
        return 0;
    if (!psx_state_block(f, &native_root_irq, sizeof(native_root_irq), load) || !psx_state_block(f, native_timer_irq_fired, sizeof(native_timer_irq_fired), load) || !psx_state_block(f, &native_root_last_tick, sizeof(native_root_last_tick), load) || !psx_state_block(f, &bios_vblank_pending, sizeof(bios_vblank_pending), load) || !psx_state_block(f, &bios_interrupts_enabled, sizeof(bios_interrupts_enabled), load) || !psx_state_block(f, &bios_callbacks_running, sizeof(bios_callbacks_running), load) || !psx_state_block(f, &psx_callback_driver, sizeof(psx_callback_driver), load))
        return 0;
    if (load)
    {
        cooperative_wait_depth = 0u;
        cooperative_sync_depth = 0u;
        native_irq_dispatching = 0u;
        native_root_polling = 0u;
        vblank_dispatching = 0;
        native_vsync_count = vblank_count;
    }
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
static uint8 pad_main_mode_lock[2];
static uint8 pad_current_id[2];
static uint32 native_pad_packet_owners[2];
static uint32 native_pad_cleanup_callback;
static uint32 native_pad_delay_cache;
static uint32 native_pad_multitap_enable;
static uint32 native_pad_actuator_budget;
static uint32 pad_buttons;
static sint32 pad_replay_bound;
static sint32 pad_replay_failure;
static uint32 pad_replay_inputs[2][3];
static uint32 bios_pad_initialized;
static uint32 bios_pad_clear_mode;
static uint32 bios_legacy_pad_buffers;
static uint32 bios_legacy_pad_destination;
static uint32 bios_legacy_pad_enabled;

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

sint32 pad_replay_is_bound(void)
{
    return pad_replay_bound;
}

sint32 pad_lockstep_apply(sint32 controller, uint32 type, uint32 buttons, uint32 analog)
{
    if (controller < 0 || controller >= 2 || type > 2u || buttons > 0xffffu)
        return 0;
    if (controller == 0)
        xport_input_override(1, buttons);
    if (!pad_packets[controller] && type == 2u)
        return 0;
    pad_replay_inputs[controller][0] = type;
    pad_replay_inputs[controller][1] = buttons;
    pad_replay_inputs[controller][2] = analog;
    return 1;
}

static sint32 pad_index(sint32 port)
{
    return (port >> 4) & 1;
}

void pad_publish(sint32 controller, sint32 connected, uint16 buttons)
{
    uint8 *packet;
    uint8 axes[4];
    sint32 analog;
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
    analog = xport_input_analog(controller, axes) && pad_main_mode[controller];
    packet[1] = analog ? 0x73u : 0x41u;
    pad_current_id[controller] = analog ? 7u : 4u;
    memcpy(packet + 4, axes, 4);
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
#if !defined(LOCKSTEP_DEBUG)
    pad_main_mode[0] = pad_main_mode[1] = 0;
    memset(pad_main_mode_lock, 0, sizeof(pad_main_mode_lock));
#endif
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
    uint8 analog;
    uint8 axes[4];
    uint16 buttons;
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
                serial->buttons = (uint16)~xport_input_read((sint32)port);
                serial->analog = (uint8)(xport_input_analog((sint32)port, serial->axes) && pad_main_mode[port]);
                serial->received = serial->analog ? 0x73u : 0x41u;
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
            serial->received = (uint8)(serial->buttons >> 8);
            serial->phase = 4u;
            serial->acknowledged = 1u;
            break;
        case 4:
            serial->received = (uint8)serial->buttons;
            serial->phase = serial->analog ? 5u : 0u;
            serial->acknowledged = serial->analog;
            break;
        case 5:
        case 6:
        case 7:
        case 8:
            serial->received = serial->axes[serial->phase - 5u];
            serial->acknowledged = serial->phase < 8u;
            serial->phase = serial->phase < 8u ? serial->phase + 1u : 0u;
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

void psx_pad_bind_owner_packets(uint32 owner0, uint32 owner1, uint32 cleanup_callback, uint32 delay_cache, uint32 multitap_enable, uint32 actuator_budget)
{
    if ((owner0 && !psx_addr(owner0, 240u)) || (owner1 && !psx_addr(owner1, 240u)))
        abort();
    native_pad_packet_owners[0] = owner0;
    native_pad_packet_owners[1] = owner1;
    native_pad_cleanup_callback = cleanup_callback;
    native_pad_delay_cache = delay_cache;
    native_pad_multitap_enable = multitap_enable;
    native_pad_actuator_budget = actuator_budget;
}

static void pad_update_rumble(uint32 port)
{
    uint16 low = 0, high = 0;
    if (pad_active && pad_actuators[port])
        for (uint32 idx = 0; idx < sizeof(pad_alignment[port]) && idx < pad_actuator_lengths[port]; ++idx)
        {
            uint8 value = pad_actuators[port][idx];
            if (pad_alignment[port][idx] == 0 && value)
                high = 65535;
            else if (pad_alignment[port][idx] == 1)
                low = value * 257u;
        }
    xport_input_rumble((sint32)port, low, high);
}

static void bios_native_pad_poll(void)
{
    if (pad_replay_bound && pad_active)
    {
        if (native_pad_delay_cache)
        {
            w_u32(native_pad_delay_cache, r_u16(0x1F801120));
            w_u32(native_pad_delay_cache + 4, 0);
        }
        uint32 port;
        uint32 budget_pending = native_pad_actuator_budget;
        for (port = 0; port < 2; ++port)
        {
            typedef struct PAD_ERROR_FRAME
            {
                uint32 owner, id, children, next_child, entered;
                struct PAD_ERROR_FRAME *previous;
            } PAD_ERROR_FRAME;

            PAD_ERROR_FRAME root = {0}, *frame;
            uint32 packet_owner = native_pad_packet_owners[port];
            if (!packet_owner || !pad_packets[port])
                continue;
            if (pad_replay_inputs[port][0])
            {
                uint32 received = r_u32(packet_owner + 60u);
                uint32 output = r_u32(packet_owner + 48u);
                uint32 parent = r_u32(packet_owner + 16u);
                uint32 id = r_u8(received) >> 4;
                uint32 previous_id = r_u8(packet_owner + 232u);
                uint32 length;
                uint32 index;

                /* Publish the completed exchange before starting the next */
                if (!(r_u8(received) & 0xf0u))
                {
                    w_u8(output, 0xffu);
                    w_u8(output + 1u, 0u);
                    w_u8(packet_owner + 232u, 0u);
                    w_u8(packet_owner + 53u, 0u);
                    xport_missing_pad_cleanup_callback(native_pad_cleanup_callback, packet_owner);
                }
                else if (packet_owner != parent && ((r_u8(received + 1u) && r_u8(received + 1u) != 0x5au) || id == 8u))
                {
                    xport_missing_pad_owner_callback(0u, packet_owner);
                }
                else
                {
                    if (r_u8(received + 1u) == 0x5au)
                    {
                        if (previous_id == 8u)
                            w_u8(output, 0u);
                        else if (id != 15u)
                        {
                            w_u8(packet_owner + 232u, id);
                            w_u8(output, 0u);
                            w_u8(output + 1u, r_u8(received));
                            length = packet_owner != parent ? 8u : id == 8u ? 2u : r_u8(packet_owner + 68u);
                            w_u8(packet_owner + 53u, length);
                            for (index = 2u; index < length; ++index)
                                w_u8(output + index, r_u8(received + index));
                        }
                    }
                    if ((!r_u8(received + 1u) && (r_u8(packet_owner + 70u) != 1u || r_u32(packet_owner + 20u)) && !r_u8(packet_owner + 80u)) || r_u8(packet_owner + 232u) != previous_id)
                        xport_missing_pad_cleanup_callback(native_pad_cleanup_callback, packet_owner);
                    if (r_u32(packet_owner + 12u) || !r_u8(packet_owner + 55u))
                        w_u8(packet_owner + 74u, 0u);
                    if (r_u8(packet_owner + 70u) != 0xffu)
                    {
                        uint32 state = r_u8(packet_owner + 70u);
                        if ((uint8)(state - 2u) < 0xfcu && r_u8(received) != 0xf3u)
                            xport_missing_pad_cleanup_callback(native_pad_cleanup_callback, packet_owner);
                        else if (r_u8(packet_owner + 232u) == 8u && state)
                            w_u8(packet_owner + 70u, state + 1u);
                        else if (r_u8(packet_owner + 232u) == 8u || !state || r_u8(packet_owner + (packet_owner == parent ? 55u : 56u)))
                        {
                            if (!state)
                            {
                                uint32 children = r_u32(packet_owner + 12u);
                                if (children || !r_u8(packet_owner + 55u))
                                {
                                    if (r_u8(packet_owner + 232u) == 8u && children && r_u8(r_u32(children + 60u)) == 0xffu)
                                        w_u8(packet_owner + 73u, 2u);
                                    else
                                    {
                                        w_u8(packet_owner + 73u, 1u);
                                        w_u8(packet_owner + 70u, state + 1u);
                                    }
                                }
                            }
                            else if (state == 1u)
                            {
                                w_u8(packet_owner + 71u, 0u);
                                w_u8(packet_owner + 70u, state + 1u);
                            }
                            else if (state == 0xfeu)
                                w_u8(packet_owner + 70u, 0xffu);
                            else
                            {
                                uint32 result = xport_missing_pad_owner_callback(r_u32(packet_owner + 24u), packet_owner);
                                w_u8(packet_owner + 70u, r_u8(packet_owner + 70u) + result);
                            }
                        }
                    }
                }
                pad_current_id[port] = r_u8(output + 1u) >> 4;

                if (budget_pending)
                {
                    w_u32(budget_pending, 0);
                    budget_pending = 0;
                }
                if (native_pad_actuator_budget && !(native_pad_multitap_enable && r_u32(native_pad_multitap_enable) && (r_u8(received) >> 4) == 8 && !r_u8(packet_owner + 55)))
                {
                    uint32 owner = packet_owner;
                    uint32 budget = native_pad_actuator_budget;
                    uint32 slot, motor, count, mode, packet;

                    for (slot = 0; slot < 6; ++slot)
                        w_u8(owner + 87 + slot, 0);
                    mode = r_u16(owner + 230);
                    packet = r_u32(owner + 40);
                    if (mode && packet)
                    {
                        count = r_u8(owner + 52);
                        if (count > 6)
                            count = 6;
                        for (motor = 0; motor < r_u8(owner + 233); ++motor)
                        {
                            uint32 descriptor = r_u32(owner + 4) + motor * 5;
                            uint32 mask = r_u8(descriptor + 2) ? 255 : 1;
                            uint32 active = 0;
                            for (slot = 0; slot < count; ++slot)
                                if (r_u8(owner + 93 + slot) == motor && (r_u8(packet + slot) & mask))
                                {
                                    active = 1;
                                    break;
                                }
                            if (active)
                            {
                                uint32 total = r_u32(budget) + r_u8(descriptor + 3);
                                if ((sint32)total < 61)
                                {
                                    w_u32(budget, total);
                                    for (slot = 0; slot < count; ++slot)
                                        if (r_u8(owner + 93 + slot) == motor)
                                            w_u8(owner + 87 + slot, 1);
                                }
                            }
                        }
                    }
                    else
                    {
                        uint32 type = r_u8(owner + 232);
                        if ((type == 4 || type == 5 || type == 7) && !mode && r_u8(owner + 52) >= 2)
                        {
                            if ((r_u8(packet) & 0xc0) == 0x40 && (r_u8(packet + 1) & 1) && (sint32)(r_u32(budget) + 10) < 61)
                            {
                                w_u8(owner + 88, 1);
                                w_u8(owner + 87, 1);
                                w_u32(budget, r_u32(budget) + 10);
                            }
                        }
                        else if (type == 3)
                            w_u8(owner + 87, 1);
                        else if (!mode)
                            for (slot = 6; slot != 0; --slot)
                                w_u8(owner + 86 + slot, 1);
                    }
                }

                /* The next synchronous exchange samples physical input */
                w_u8(received, pad_replay_inputs[port][0] == 2u ? 0x73u : 0x41u);
                w_u8(received + 1u, 0x5au);
                w_u8(received + 2u, (uint8)~pad_replay_inputs[port][1]);
                w_u8(received + 3u, (uint8)(~pad_replay_inputs[port][1] >> 8));
                if (pad_replay_inputs[port][0] == 2u)
                    for (index = 0u; index < 4u; ++index)
                        w_u8(received + 4u + index, (uint8)(pad_replay_inputs[port][2] >> (8u * index)));
                continue;
            }
            pad_packets[port][0] = 0xffu;
            root.owner = native_pad_packet_owners[port];
            frame = &root;
            while (frame)
            {
                uint32 owner = frame->owner, parent, children, state, retry, previous;
                sint32 aggregate_error = 0;
                if (!frame->entered)
                {
                    frame->id = r_u8(r_u32(owner + 60)) >> 4;
                    if (r_u8(owner + 232) != 8 && frame->id == 8 && r_u32(owner + 16) == owner)
                    {
                        w_u8(owner + 232, 8);
                        w_u8(r_u32(owner + 48), 0xff);
                        w_u8(r_u32(owner + 48) + 1, 0x80);
                        w_u8(owner + 53, 2);
                    }
                    if (r_u8(owner + 232) == 8)
                        frame->children = r_u32(owner + 12);
                    frame->entered = 1;
                }
                if (frame->children && frame->next_child < 4)
                {
                    PAD_ERROR_FRAME *child = (PAD_ERROR_FRAME *)calloc(1, sizeof(*child));
                    if (!child)
                        abort();
                    child->owner = frame->children + 240 * frame->next_child++;
                    child->previous = frame;
                    frame = child;
                    continue;
                }
                parent = r_u32(owner + 16);
                children = r_u32(parent + 12);
                if ((r_u32(owner + 12) && !r_u8(owner + 55)) || (!r_u32(owner + 12) && !r_u8(owner + 56)))
                {
                    uint32 index;
                    if (!r_u32(owner + 12))
                        aggregate_error = r_u8(parent + 55) != 0;
                    for (index = 0; index < 4 && !aggregate_error; ++index)
                        aggregate_error = r_u8(children + 240 * index + 56) != 0;
                    if (aggregate_error)
                    {
                        if (!r_u8(owner + 74))
                            w_u8(owner + 74, 1);
                        goto pad_error_return;
                    }
                }
                previous = r_u8(owner + 55);
                state = r_u8(owner + 70);
                w_u8(owner + 55, 0);
                w_u32(owner + 76, r_u32(owner + 76) + 1);
                w_u8(owner + 57, r_u8(owner + 56));
                w_u8(owner + 56, previous);
                if (state)
                {
                    retry = r_u8(owner + 74);
                    if (state == 1)
                    {
                        if (retry < 11 && r_u8(owner + 232) != 8)
                        {
                            w_u8(owner + 74, retry + 1);
                            goto pad_error_return;
                        }
                        if (r_u16(owner + 230))
                        {
                            retry = r_u8(owner + 74);
                            if (retry < 21)
                            {
                                w_u8(owner + 74, retry + 1);
                                goto pad_error_return;
                            }
                            xport_missing_pad_cleanup_callback(native_pad_cleanup_callback, owner);
                        }
                        w_u8(owner + 73, 2);
                        if (r_u8(owner + 232) == 8)
                            w_u8(owner + 70, 0xfe);
                        else
                        {
                            if (r_u32(owner + 12))
                            {
                                w_u8(owner + 56, 0);
                                w_u8(owner + 55, 0);
                            }
                            w_u8(owner + 70, 0xff);
                        }
                        goto pad_error_return;
                    }
                    if (retry < 11)
                    {
                        w_u8(owner + 74, retry + 1);
                        goto pad_error_return;
                    }
                }
                if (r_u8(owner + 73))
                {
                    w_u8(r_u32(owner + 48), 0xff);
                    w_u8(r_u32(owner + 48) + 1, 0);
                    w_u8(owner + 232, 0);
                    w_u8(owner + 53, 0);
                    xport_missing_pad_cleanup_callback(native_pad_cleanup_callback, owner);
                }
                if (frame->id == 8)
                {
                    if (r_u32(owner + 12))
                    {
                        w_u8(owner + 232, 8);
                        w_u8(r_u32(owner + 48) + 1, 0x80);
                    }
                }
                else if (r_u8(owner + 232) == 8)
                {
                    w_u8(owner + 232, 0);
                    w_u8(r_u32(owner + 48) + 1, 0);
                }
            pad_error_return:
            {
                PAD_ERROR_FRAME *previous_frame = frame->previous;
                if (frame != &root)
                    free(frame);
                frame = previous_frame;
            }
            }
            if (budget_pending)
            {
                w_u32(budget_pending, 0);
                budget_pending = 0;
            }
        }
    }
    if (!pad_replay_bound && pad_active)
    {
        uint32 port;
        for (port = 0u; port < 2u; ++port)
        {
            uint32 owner = native_pad_packet_owners[port];
            uint32 packet;
            uint16 owner_buttons = (uint16)~xport_input_read((sint32)port);
            pad_update_rumble(port);
            uint8 axes[4];
            sint32 analog = xport_input_analog((sint32)port, axes) && pad_main_mode[port];
            pad_publish((sint32)port, 1, (uint16)((owner_buttons >> 8) | (owner_buttons << 8)));
            if (!owner)
                continue;
            packet = r_u32(owner + 48u);
            if (!packet)
                continue;
            w_u8(packet, 0u);
            w_u8(packet + 1u, analog ? 0x73u : 0x41u);
            w_u8(packet + 2u, (uint8)(owner_buttons >> 8));
            w_u8(packet + 3u, (uint8)owner_buttons);
            w_u8(packet + 4u, axes[0]);
            w_u8(packet + 5u, axes[1]);
            w_u8(packet + 6u, axes[2]);
            w_u8(packet + 7u, axes[3]);
        }
    }
    if (pad_active && bios_legacy_pad_enabled && bios_legacy_pad_destination)
        PAD_dr();
}

void PadStartCom(void)
{
    pad_active = 1;
}

void PadStopCom(void)
{
    pad_active = 0;
    if (!pad_replay_bound)
        for (uint32 port = 0; port < 2; ++port)
            xport_input_rumble((sint32)port, 0, 0);
}

sint32 PadGetState(sint32 port)
{
    uint32 owner = native_pad_packet_owners[((uint32)port & 0xF0u) != 0u];
    sint32 index;
    if (owner)
    {
        uint32 error;
        if (native_pad_multitap_enable && r_u32(native_pad_multitap_enable) && ((r_u8(owner + 0xE8) == 8 && !((uint32)port & 0xF)) || ((uint32)port & 3)))
            owner = r_u32(owner + 0xC) + 240 * ((uint32)port & 3);
        uint32 state;
#if defined(XPORT_PAD_STATE_PSYQ43)
        error = r_u32(owner + 0x34u) & 0xFFFF0000;
        if (!error && r_u32(owner + 0x10u) != owner)
            error = r_u8(owner + 0x38u);
#else
        error = r_u8(owner + 0x37u);
        if (!error)
            error = r_u8(owner + 0x38u);
        if (!error && r_u32(owner + 0x10u) != owner)
            error = r_u8(owner + 0x39u);
#endif
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
#if !defined(LOCKSTEP_DEBUG)
    if (!pad_replay_bound && !owner)
    {
        uint8 axes[4];
        sint32 capable;
        if ((port != 0 && port != 0x10) || !pad_active || (!pad_packets[index] && !owner))
            return 0;
        (void)xport_input_read((sint32)index);
        capable = xport_input_analog((sint32)index, axes);
        switch (term)
        {
            case 1:
                return capable && pad_main_mode[index] ? 7 : 4;
            case 2:
                return capable ? 7 : 0;
            case 3:
                return capable ? pad_main_mode[index] : 0;
            case 4:
                if (offset == -1)
                    return capable ? 2 : 1;
                if (offset == 0)
                    return 4;
                return capable && offset == 1 ? 7 : 0;
            default:
                return 0;
        }
    }
#endif
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

sint32 PadInfoAct(sint32 port, sint32 actuator, sint32 term)
{
    uint32 index = ((uint32)port & 0xF0u) != 0u;
    uint32 owner = native_pad_packet_owners[index];
    if (owner)
    {
        uint32 count = r_u8(owner + 0xE9u);
        if (actuator < 0)
            return (sint32)count;
        if ((uint32)actuator >= count || term < 1 || term > 5)
            return 0;
        return r_u8(r_u32(owner + 4u) + 5u * (uint32)actuator + (uint32)term - 1u);
    }
    if ((port != 0 && port != 0x10) || pad_replay_bound || !pad_active || !xport_input_rumble_supported((sint32)index))
        return 0;
    if (actuator < 0)
        return 2;
    return actuator < 2 && term == 3 ? actuator : 0;
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
#if !defined(LOCKSTEP_DEBUG)
    if (!pad_replay_bound && !owner)
    {
        uint8 axes[4];
        if ((port != 0 && port != 0x10) || (mode != 0 && mode != 1) || lock < 0 || lock > 3 || !pad_active || (!pad_packets[index] && !owner))
            return 0;
        (void)xport_input_read((sint32)index);
        if (!xport_input_analog((sint32)index, axes))
            return 0;
        pad_main_mode[index] = mode;
        if (lock == 2 || lock == 3)
            pad_main_mode_lock[index] = (uint8)(lock == 3);
        bios_native_pad_poll();
        return 1;
    }
#endif
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
    if (!psx_state_block(file, pad_replay_inputs, sizeof(pad_replay_inputs), load))
        return 0;
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

void InitHeap(uint32 address, uint32 bytes)
{
    uint32 end;
    w_u32(0xA0009000, address);
    end = address + ((bytes >> 2) << 2);
    w_u32(address, 0);
    w_u32(0xA0009008, end + 4);
    w_u32(0xA0009004, bytes);
    w_u32(0xA000900C, 0);
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
    bios_legacy_pad_buffers = 0;
    bios_legacy_pad_destination = 0;
    bios_legacy_pad_enabled = 0;
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

static uint32 bios_user_heap_extend(uint32 bytes)
{
    uint32 current = r_u32(0xA0009000u);
    uint32 end = r_u32(0xA0009008u);
    uint32 quantum, larger, smaller, available, count;
    if (bytes != 0u)
    {
        quantum = r_u32(0xA0009004u);
        larger = quantum < bytes ? bytes : quantum;
        smaller = quantum < bytes ? quantum : bytes;
        available = ((end - current) & ~3u) - 4u;
        if (available >= larger)
            count = larger;
        else if (available >= smaller)
            count = smaller;
        else
            return 0xFFFFFFFFu;
    }
    else
    {
        if (current >= end)
            return 0xFFFFFFFFu;
        count = 0u;
    }
    w_u32(current + (uint32)((sint32)count / 4) * 4u, 0xFFFFFFFEu);
    w_u32(current - 4u, count | 1u);
    w_u32(0xA0009000u, current + (uint32)((sint32)(count + 4u) / 4) * 4u);
    return 0u;
}

uint32 mallocPSX(uint32 bytes)
{
    uint32 size = (bytes + 3u) & ~3u;
    uint32 cursor, header, following, next, result;
    sint32 passes = 2;
    if (r_u32(0xA000900Cu) == 0u)
    {
        cursor = r_u32(0xA0009000u);
        if (cursor >= r_u32(0xA0009008u))
            return 0u;
        w_u32(cursor, 0xFFFFFFFEu);
        w_u32(0xA000B064u, cursor);
        w_u32(0xA0009000u, cursor + 4u);
        if (bios_user_heap_extend(size) != 0u)
            return 0u;
        w_u32(0xA000B060u, r_u32(0xA000B064u));
        w_u32(0xA000900Cu, 1u);
    }
    for (;;)
    {
        cursor = r_u32(0xA000B064u);
        header = r_u32(cursor);
        if ((header & 1u) != 0u)
        {
            if ((size | 1u) == header)
            {
                w_u32(cursor, header & ~1u);
                return r_u32(0xA000B064u) + 4u;
            }
            if ((size | 1u) < header)
            {
                w_u32(cursor + (uint32)((sint32)size / 4) * 4u + 4u, header - size - 4u);
                w_u32(r_u32(0xA000B064u), size);
                return r_u32(0xA000B064u) + 4u;
            }
            if (header >= size)
                continue;
            following = cursor + (header & ~3u) + 4u;
            next = r_u32(following);
            if (next == 0xFFFFFFFEu)
            {
                --passes;
                if (passes > 0)
                    w_u32(0xA000B064u, r_u32(0xA000B060u));
                else if (bios_user_heap_extend(size) != 0u)
                    return 0u;
            }
            else if ((next & 1u) != 0u)
                w_u32(r_u32(0xA000B064u), header + (next & ~1u) + 4u);
            else
                w_u32(0xA000B064u, following);
        }
        else if (header != 0xFFFFFFFEu)
            w_u32(0xA000B064u, cursor + (header & ~3u) + 4u);
        else
        {
            --passes;
            if (passes > 0)
                w_u32(0xA000B064u, r_u32(0xA000B060u));
            else
            {
                result = bios_user_heap_extend(size);
                if (result != 0u)
                    return result;
            }
        }
    }
}

void freePSX(uint32 address)
{
    w_u32(address - 4u, r_u32(address - 4u) | 1u);
}

sint32 _96_init(void)
{
    return CdInit();
}

uint32 _96_remove(void)
{
    EnterCriticalSectionPSX();
    CloseEventPSX(r_u32(0xA000B9B8u));
    CloseEventPSX(r_u32(0xA000B9BCu));
    CloseEventPSX(r_u32(0xA000B9C0u));
    CloseEventPSX(r_u32(0xA000B9C4u));
    CloseEventPSX(r_u32(0xA000B9C8u));
    SysDeqIntRP(0u, 0xA00091D0u);
    return SysDeqIntRP(0u, 0xA00091E0u);
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

uint32 InitPADNative(uint8 *buffer0, uint32 size0, uint8 *buffer1, uint32 size1)
{
    if (!buffer0 || size0 < 8u || size0 > PSX_DRAM_SIZE)
        return 0u;
    if (size1 == 0u ? buffer1 != NULL : !buffer1 || size1 < 8u || size1 > PSX_DRAM_SIZE)
        return 0u;
    bios_legacy_pad_enabled = 0;
    pad_packets[0] = buffer0;
    pad_packets[1] = buffer1;
    pad_active = 0;
    bios_pad_initialized = 1u;
    w_u32(0x74B8u, 1u);
    pad_publish(0, 0, 0xFFFFu);
    pad_publish(1, 0, 0xFFFFu);
    xport_input_init();
    return 1u;
}

uint32 InitPAD(uint32 buffer0, uint32 size0, uint32 buffer1, uint32 size1)
{
    if (size0 < 8u || size0 > PSX_DRAM_SIZE || !bios_card_address(buffer0, size0, 1u))
        return 0u;
    if (size1 == 0u ? buffer1 != 0u : size1 < 8u || size1 > PSX_DRAM_SIZE || !bios_card_address(buffer1, size1, 1u))
        return 0u;
    bios_legacy_pad_enabled = 0;
    pad_packets[0] = (uint8 *)psx_addr(buffer0, size0);
    pad_packets[1] = size1 != 0u ? (uint8 *)psx_addr(buffer1, size1) : NULL;
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

uint32 PAD_dr(void)
{
    uint32 port, packed = 0;
    for (port = 0; port < 2; ++port)
    {
        const uint8 *packet = pad_packets[port];
        uint32 buttons = 0xFFFF;
        if (packet && packet[0] == 0 && (packet[1] == 0x41 || packet[1] == 0x23))
        {
            buttons = ((uint32)packet[2] << 8) | packet[3];
            if (packet[1] == 0x23)
            {
                buttons |= 0x7C7;
                if (packet[5] > 0x10)
                    buttons &= ~0x40u;
                if (packet[6] > 0x10)
                    buttons &= ~0x80u;
            }
        }
        packed |= buttons << (port * 16);
    }
    w_u32(bios_legacy_pad_destination, packed);
    return packed;
}

uint32 PAD_init2(uint32 type, uint32 destination)
{
    if (type != 0x20000000u && type != 0x20000001u)
        return 0;
    if (!bios_legacy_pad_buffers)
    {
        bios_legacy_pad_buffers = psx_bios_kernel_malloc(0x44);
        if (!bios_legacy_pad_buffers || bios_legacy_pad_buffers == ~0u)
        {
            bios_legacy_pad_buffers = 0;
            return 0;
        }
    }
    memset(psx_addr(bios_legacy_pad_buffers, 0x44), 0, 0x44);
    if (!InitPAD(bios_legacy_pad_buffers, 0x22, bios_legacy_pad_buffers + 0x22, 0x22))
        return 0;
    StartPAD();
    bios_legacy_pad_destination = destination;
    bios_legacy_pad_enabled = 1;
    return 2;
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

sint32 ClearImage2(PSX_RECT *rect, uint8 r, uint8 g, uint8 b)
{
    static uint32 packet[14];
    GpuRasterState state;
    sint32 width_limit = GPU_VRAM_WIDTH - 1;
    sint32 height_limit = GPU_VRAM_HEIGHT - 1;
    uint32 color = r | ((uint32)g << 8) | ((uint32)b << 16);
    uint32 xy, size, mode;
    if (!rect || !gpu_register_packet_range(packet, sizeof(packet)))
        abort();
    rect->w = rect->w < 0 ? 0 : rect->w > width_limit ? (sint16)width_limit : rect->w;
    rect->h = rect->h < 0 ? 0 : rect->h > height_limit ? (sint16)height_limit : rect->h;
    xy = (uint16)rect->x | ((uint32)(uint16)rect->y << 16);
    size = (uint16)rect->w | ((uint32)(uint16)rect->h << 16);
    gpu_export_raster(&state);
    mode = 0xE1000400 | ((state.draw_mode | state.draw_flags) & 0x7FF);
    if (((rect->x | rect->w) & 63) != 0)
    {
        packet[0] = 0x08000000 | gpu_packet_link((uintptr_t)(packet + 10));
        packet[1] = 0xE3000000;
        packet[2] = 0xE4FFFFFF;
        packet[3] = 0xE5000000;
        packet[4] = 0xE6000000;
        packet[5] = mode;
        packet[6] = 0x60000000 | color;
        packet[7] = xy;
        packet[8] = size;
        packet[10] = 0x03FFFFFF;
        packet[11] = 0xE3000000 | state.area[0] | (state.area[1] << 10);
        packet[12] = 0xE4000000 | state.area[2] | (state.area[3] << 10);
        packet[13] = 0xE5000000 | ((uint32)state.offset[0] & 0x7FF) | (((uint32)state.offset[1] & 0x7FF) << 11);
    }
    else
    {
        packet[0] = 0x05FFFFFF;
        packet[1] = 0xE6000000;
        packet[2] = mode;
        packet[3] = 0x02000000 | color;
        packet[4] = xy;
        packet[5] = size;
    }
    DrawOTag(packet);
    return 0;
}

uint32 _bu_init(void)
{
    return xport_bios_start_card();
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
    uint32 index, value;
    if (!bios_card_patches_bound)
    {
#ifndef NDEBUG
        fprintf(stderr, "Unbound BIOS card patch sources\n");
#endif
        abort();
    }
    for (index = 0; index < 5; ++index)
    {
        value = r_u32(bios_card_first_source + index * 4);
        w_u32(bios_card_first_destination + index * 4, value);
    }
    w_u32(0xDFFCu, bios_card_first_destination + 20u);
    FlushCache();
    return bios_card_first_destination + 20u;
}

uint32 _patch_card2(void)
{
    uint32 index, value;
    if (!bios_card_patches_bound)
    {
#ifndef NDEBUG
        fprintf(stderr, "Unbound BIOS card patch sources\n");
#endif
        abort();
    }
    uint32 original = r_u32(bios_card_second_destination);
    for (index = 0; index < 5; ++index)
    {
        value = r_u32(bios_card_second_source + index * 4);
        w_u32(bios_card_second_destination + index * 4, value);
    }
    FlushCache();
    return original;
}

uint32 _copy_memcard_patch(void)
{
    uint32 index, value;
    if (!bios_card_patches_bound)
    {
#ifndef NDEBUG
        fprintf(stderr, "Unbound BIOS card patch sources\n");
#endif
        abort();
    }
    for (index = 0; index < 28; ++index)
    {
        value = r_u32(bios_card_copy_source + index * 4);
        w_u32(0xDF80 + index * 4, value);
    }
    return 0xDFF0u;
}

static PsxCardSdkBinding bios_card_sdk;
static void (*bios_card_stop_restore)(uint32);
static uint32 bios_card_stop_return;
static uint32 bios_card_sdk_bound;

void psx_bios_bind_card_sdk(const PsxCardSdkBinding *binding)
{
    if (!binding || !binding->init_pad_flag || !binding->saved_return || !binding->copy_source || !binding->first_source || !binding->second_source || !binding->first_return || !binding->second_return || !binding->clear_return || !binding->clear_add_pc)
        abort();
    bios_card_sdk = *binding;
    bios_card_copy_source = binding->copy_source;
    bios_card_first_source = binding->first_source;
    bios_card_second_source = binding->second_source;
    bios_card_patches_bound = 1;
    bios_card_sdk_bound = 1;
}

/* Native returns retain the original guest continuation storage */
static int bios_card_sdk_returns_to(uint32 expected)
{
    uint32 target = r_u32(bios_card_sdk.saved_return);
    if (target == expected)
        return 1;
    xport_guest_call0(target);
    return 0;
}

void psx_bios_bind_card_stop(void (*restore)(uint32), uint32 return_continuation)
{
    bios_card_stop_restore = restore;
    bios_card_stop_return = return_continuation;
}

uint32 StopCARD(void)
{
    if (!bios_card_stop_restore)
    {
#ifndef NDEBUG
        fprintf(stderr, "Unbound StopCARD kernel restore\n");
#else
        (void)0;
#endif
        abort();
    }
    xport_bios_stop_card();
    bios_card_stop_restore(bios_card_stop_return);
    return 0;
}

void InitCARD(sint32 mode)
{
    uint32 previous, table, base, high, low;
    sint64 destination;
    if (!bios_card_sdk_bound)
    {
#ifndef NDEBUG
        fprintf(stderr, "Unbound InitCARD SDK image layout\n");
#endif
        abort();
    }
    ChangeClearPAD(0);
    VSync(0);
    previous = (uint32)xport_bios_enter_critical();
    if (r_u32(bios_card_sdk.init_pad_flag) == 0)
        mode = 0;
    xport_bios_init_card((uint32)mode);
    _copy_memcard_patch();

    w_u32(bios_card_sdk.saved_return, bios_card_sdk.first_return);
    xport_bios_enter_critical();
    table = GetC0Table();
    base = r_u32(table + 24);
    high = r_u32(base + 112);
    low = r_u32(base + 116);
    bios_card_first_destination = (((high & 0xFFFF) << 16) + (low & 0xFFFF)) + 40;
    _patch_card();
    if (!bios_card_sdk_returns_to(bios_card_sdk.first_return))
        return;

    w_u32(bios_card_sdk.saved_return, bios_card_sdk.second_return);
    xport_bios_enter_critical();
    table = GetB0Table();
    base = r_u32(table + 364);
    bios_card_second_destination = base + 2504;
    _patch_card2();
    if (!bios_card_sdk_returns_to(bios_card_sdk.second_return))
        return;

    w_u32(bios_card_sdk.saved_return, bios_card_sdk.clear_return);
    table = GetB0Table();
    base = r_u32(table + 364);
    destination = (sint64)(sint32)base + 6536;
    if (destination < INT32_MIN || destination > INT32_MAX)
        xport_mips_overflow_exception(bios_card_sdk.clear_add_pc);
    w_u32((uint32)destination, 0);
    FlushCache();
    if (!bios_card_sdk_returns_to(bios_card_sdk.clear_return))
        return;
    if (previous == 1)
        xport_bios_exit_critical();
}

void StartCARD(void)
{
    uint32 previous = (uint32)xport_bios_enter_critical();
    xport_bios_start_card();
    ChangeClearPAD(0);
    if (previous == 1)
        xport_bios_exit_critical();
}

PsxInterruptCallback psx_bios_vsync_callback(PsxInterruptCallback callback)
{
    PsxInterruptCallback previous = bios_vsync_native_callback;
    bios_vsync_callback = 0;
    bios_vsync_native_callback = callback;
    return previous;
}

uint32 VSyncCallbackPSX(uint32 guest_callback)
{
    uint32 previous = bios_vsync_callback;
    bios_vsync_native_callback = NULL;
    bios_vsync_callback = guest_callback;
    return previous;
}

uint32 ResetCallbackPSX(void)
{
    uint32 previous;
    previous = bios_callbacks_running;
    ResetCallback();
    bios_vsync_callback = 0u;
    bios_vsync_native_callback = NULL;
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

uint32 GetC0Table(void)
{
    return 0x674;
}

uint32 GetB0Table(void)
{
    return 0x874;
}

uint32 xport_bios_stop_pad(void)
{
    uint32 result;
    xport_bios_enter_critical();
    ChangeClearRCnt(3, 1);
    result = SysDeqIntRP(2, 0x74A8);
    pad_active = 0;
    xport_bios_exit_critical();
    return result;
}

uint32 xport_bios_stop_card(void)
{
    xport_bios_enter_critical();
    ChangeClearRCnt(3, 1);
    SysDeqIntRP(2, 0x74A8);
    w_u32(0x74BC, 0);
    bios_card_pending_ports = 0;
    xport_bios_exit_critical();
    return 1;
}

void xport_bios_exit_critical(void)
{
    bios_interrupts_enabled = 1u;
    psx_sync_callbacks();
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

static uint32 *gs_work_cursor;
static sint32 gs_clip_distance;

void GsSetWorkBase(uint32 *work)
{
    gs_work_cursor = work;
}

uint32 *GsGetWorkBase(void)
{
    return gs_work_cursor;
}

void gs_set_clip_distance(sint32 distance)
{
    gs_clip_distance = distance;
}

sint32 gs_get_clip_distance(void)
{
    return gs_clip_distance;
}

void GsLinkObject4(uint32 model, uint32 object, uint32 index)
{
    uint32 data = model + index * 28;
    uint32 primitive, count, head, processed, group_count, previous, code;
    w_u32(object + 8, data);
    primitive = r_u32(data + 16);
    count = r_u32(data + 20);
    head = primitive;
    group_count = 0;
    previous = 0;
    for (processed = 0; processed < count; ++processed)
    {
        code = r_u8(primitive + 3);
        if (previous != 0 && code != previous)
        {
            w_u16(head, (uint16)group_count);
            group_count = 0;
            head = primitive;
        }
        switch (code & 0xFD)
        {
            case 0x20:
                primitive += (r_u16(primitive + 2) & 4) != 0 ? 24 : 16;
                break;
            case 0x21:
            case 0x29:
                primitive += 16;
                break;
            case 0x24:
            case 0x31:
            case 0x38:
                primitive += 24;
                break;
            case 0x28:
                primitive += 20;
                break;
            case 0x2C:
            case 0x2D:
                primitive += 32;
                break;
            case 0x30:
                primitive += (r_u16(primitive + 2) & 4) != 0 ? 28 : 20;
                break;
            case 0x25:
            case 0x34:
            case 0x39:
                primitive += 28;
                break;
            case 0x35:
            case 0x3C:
                primitive += 36;
                break;
            case 0x3D:
                primitive += 44;
                break;
            default:
#ifndef NDEBUG
                printf("GPU_CODE %02xH not assigned\n", code);
#endif
                break;
        }
        ++group_count;
        previous = code;
    }
    w_u16(head, (uint16)group_count);
}

void GsMapModelingData(uint32 model)
{
    uint32 flags = r_u32(model);
    uint32 base, object, vertex, normal, primitive;
    sint32 count, index;
    if ((flags & 1) != 0)
        return;
    w_u32(model, flags | 1);
    count = r_s32(model + 4);
    base = model + 8;
    object = base;
    for (index = 0; index < count; ++index)
    {
        vertex = r_u32(object);
        primitive = r_u32(object + 16);
        w_u32(object, base + vertex);
        normal = r_u32(object + 8);
        w_u32(object + 16, base + primitive);
        w_u32(object + 8, base + normal);
        object += 28;
    }
}

static sint32 gs_screen_width;
static sint32 gs_screen_height;
static sint32 gs_light_mode;
static GsSortAttributes gs_sort_attributes;
static sint32 gs_subdivision_distance;
static sint16 gs_subdivision_width = 1023;
static sint16 gs_subdivision_height = 511;

void gs_set_subdivision_controls(sint32 distance, sint16 width, sint16 height)
{
    gs_subdivision_distance = distance;
    gs_subdivision_width = width;
    gs_subdivision_height = height;
}

static uint32 *gs_tmd_work_buffer(uint32 *work, uint32 words)
{
    uintptr_t begin = (uintptr_t)&DRAM[0];
    uintptr_t cursor = (uintptr_t)work;
    if (work == NULL || words > UINT32_MAX / 4)
        abort();
    if (cursor >= begin && cursor - begin < PSX_DRAM_SIZE)
        return (uint32 *)psx_addr((uint32)(cursor - begin), words * 4);
    return work;
}

static void gs_tmd_load_vector(uint32 address, uint32 vertex)
{
    xport_gte_write_data(vertex * 2, r_u32(address));
    xport_gte_write_data(vertex * 2 + 1, r_u32(address + 4));
}

static uint32 *gs_tmd_fast_f3(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 lighting)
{
    uint32 base = r_u32(ordering_table + 4);
    uint32 bias, indices0, indices1, normal, next0, next1, slot, depth, color;
    if (count == 0)
        return work;
    bias = r_u32(ordering_table + 8);
    if (lighting == 0)
    {
        indices1 = r_u32(primitive + 12);
        indices0 = (uint32)r_u16(primitive + 10) << 16;
    }
    else
    {
        indices0 = r_u32(primitive + 8);
        indices1 = r_u32(primitive + 12);
    }
    do
    {
        gs_tmd_load_vector(vertices + (indices0 >> 16) * 8, 0);
        gs_tmd_load_vector(vertices + (indices1 & 0xFFFF) * 8, 1);
        gs_tmd_load_vector(vertices + (indices1 >> 16) * 8, 2);
        xport_gte_execute(0x280030);
        normal = normals + (indices0 & 0xFFFF) * 8;
        color = r_u32(primitive + 4) | (gs_sort_attributes.bit30 << 25);
        xport_gte_write_data(lighting == 0 ? 22 : 6, color);
        primitive += 16;
        if (lighting == 0)
        {
            next1 = r_u32(primitive + 12);
            next0 = (uint32)r_u16(primitive + 10) << 16;
        }
        else
        {
            next0 = r_u32(primitive + 8);
            next1 = r_u32(primitive + 12);
        }
        if ((sint32)xport_gte_read_flag() >= 0)
        {
            xport_gte_execute(0x1400006);
            if ((sint32)xport_gte_read_data(24) > 0)
            {
                xport_gte_execute(0x158002D);
                if (lighting != 0)
                    gs_tmd_load_vector(normal, 0);
                depth = xport_gte_read_data(7);
                if (lighting != 0)
                    xport_gte_execute(lighting == 1 ? 0x108041B : 0xE80413);
                slot = base + ((((depth - bias) >> (shift & 31)) & 0xFFFF) * 4);
                work = gs_tmd_work_buffer(work, 5);
                xport_store_le32(work, 0x04000000);
                AddPrim(psx_addr(slot, 4), work);
                if (lighting == 0)
                    xport_store_le32(work + 1, xport_gte_read_data(22));
                xport_store_le32(work + 2, xport_gte_read_data(12));
                xport_store_le32(work + 3, xport_gte_read_data(13));
                xport_store_le32(work + 4, xport_gte_read_data(14));
                if (lighting != 0)
                    xport_store_le32(work + 1, xport_gte_read_data(22));
                work += 5;
            }
        }
        indices0 = next0;
        indices1 = next1;
    } while (--count != 0);
    return work;
}

uint32 *GsTMDfastF3L(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table)
{
    return gs_tmd_fast_f3(primitive, vertices, normals, work, count, shift, ordering_table, 1);
}

uint32 *GsTMDfastF3LFG(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table)
{
    return gs_tmd_fast_f3(primitive, vertices, normals, work, count, shift, ordering_table, 2);
}

uint32 *GsTMDfastF3NL(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table)
{
    return gs_tmd_fast_f3(primitive, vertices, normals, work, count, shift, ordering_table, 0);
}

static uint32 *gs_tmd_fast_g3(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 lighting)
{
    uint32 base = r_u32(ordering_table + 4);
    uint32 bias, indices[3], index, depth, slot, color, prefetched = 0;
    if (count == 0)
        return work;
    bias = r_u32(ordering_table + 8);
    if (lighting == 0)
    {
        for (index = 0; index < 3; ++index)
            indices[index] = (uint32)r_u16(primitive + 10 + index * 4) << 16;
        prefetched = 1;
    }
    do
    {
        if (!prefetched)
            for (index = 0; index < 3; ++index)
                indices[index] = r_u32(primitive + 8 + index * 4);
        prefetched = 0;
        for (index = 0; index < 3; ++index)
            gs_tmd_load_vector(vertices + (indices[index] >> 16) * 8, index);
        xport_gte_execute(0x280030);
        if (lighting == 0)
        {
            color = r_u32(primitive + 4) | (gs_sort_attributes.bit30 << 25);
            xport_gte_write_data(20, color);
            primitive += 20;
            for (index = 0; index < 3; ++index)
                indices[index] = (uint32)r_u16(primitive + 10 + index * 4) << 16;
            prefetched = 1;
        }
        if ((sint32)xport_gte_read_flag() >= 0)
        {
            xport_gte_execute(0x1400006);
            if (lighting != 0)
            {
                color = r_u32(primitive + 4) | (gs_sort_attributes.bit30 << 25);
                xport_gte_write_data(6, color);
            }
            if ((sint32)xport_gte_read_data(24) > 0)
            {
                xport_gte_execute(0x158002D);
                if (lighting != 0)
                    for (index = 0; index < 3; ++index)
                        gs_tmd_load_vector(normals + (indices[index] & 0xFFFF) * 8, index);
                depth = xport_gte_read_data(7);
                if (lighting != 0)
                    xport_gte_execute(lighting == 1 ? 0x118043F : 0xF80416);
                slot = base + ((((depth - bias) >> (shift & 31)) & 0xFFFF) * 4);
                work = gs_tmd_work_buffer(work, 7);
                xport_store_le32(work, 0x06000000);
                AddPrim(psx_addr(slot, 4), work);
                if (lighting != 0)
                {
                    primitive += 20;
                    for (index = 0; index < 3; ++index)
                        indices[index] = r_u32(primitive + 8 + index * 4);
                    prefetched = 1;
                    for (index = 0; index < 3; ++index)
                        xport_store_le32(work + 2 + index * 2, xport_gte_read_data(12 + index));
                    for (index = 0; index < 3; ++index)
                        xport_store_le32(work + 1 + index * 2, xport_gte_read_data(20 + index));
                }
                else
                {
                    for (index = 0; index < 3; ++index)
                        xport_store_le32(work + 1 + index * 2, xport_gte_read_data(20));
                    for (index = 0; index < 3; ++index)
                        xport_store_le32(work + 2 + index * 2, xport_gte_read_data(12 + index));
                }
                work += 7;
            }
        }
        if (!prefetched)
            primitive += 20;
    } while (--count != 0);
    return work;
}

uint32 *GsTMDfastG3L(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table)
{
    return gs_tmd_fast_g3(primitive, vertices, normals, work, count, shift, ordering_table, 1);
}

uint32 *GsTMDfastG3LFG(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table)
{
    return gs_tmd_fast_g3(primitive, vertices, normals, work, count, shift, ordering_table, 2);
}

uint32 *GsTMDfastG3NL(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table)
{
    return gs_tmd_fast_g3(primitive, vertices, normals, work, count, shift, ordering_table, 0);
}

static uint32 *gs_tmd_fast_f4(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 lighting)
{
    uint32 base = r_u32(ordering_table + 4);
    uint32 bias, indices0, indices1, next0, next1, normal, fourth, color, depth, slot;
    if (count == 0)
        return work;
    bias = r_u32(ordering_table + 8);
    if (lighting == 0)
    {
        indices1 = r_u32(primitive + 12);
        indices0 = (uint32)r_u16(primitive + 10) << 16;
    }
    else
    {
        indices0 = r_u32(primitive + 8);
        indices1 = r_u32(primitive + 12);
    }
    do
    {
        gs_tmd_load_vector(vertices + (indices0 >> 16) * 8, 0);
        gs_tmd_load_vector(vertices + (indices1 & 0xFFFF) * 8, 1);
        gs_tmd_load_vector(vertices + (indices1 >> 16) * 8, 2);
        xport_gte_execute(0x280030);
        normal = normals + (indices0 & 0xFFFF) * 8;
        if (lighting != 0)
            fourth = r_u16(primitive + 16);
        color = r_u32(primitive + 4) | (gs_sort_attributes.bit30 << 25);
        xport_gte_write_data(lighting == 0 ? 22 : 6, color);
        if (lighting == 0)
            fourth = r_u16(primitive + 16);
        fourth = vertices + fourth * 8;
        primitive += 20;
        if (lighting == 0)
        {
            next1 = r_u32(primitive + 12);
            next0 = (uint32)r_u16(primitive + 10) << 16;
        }
        else
        {
            next0 = r_u32(primitive + 8);
            next1 = r_u32(primitive + 12);
        }
        if ((sint32)xport_gte_read_flag() >= 0)
        {
            xport_gte_execute(0x1400006);
            gs_tmd_load_vector(fourth, 0);
            if ((sint32)xport_gte_read_data(24) > 0)
            {
                work = gs_tmd_work_buffer(work, 6);
                if (lighting == 0)
                    xport_store_le32(work + 1, xport_gte_read_data(22));
                xport_store_le32(work + 2, xport_gte_read_data(12));
                xport_store_le32(work + 3, xport_gte_read_data(13));
                xport_store_le32(work + 4, xport_gte_read_data(14));
                xport_gte_execute(0x180001);
                if ((sint32)xport_gte_read_flag() >= 0)
                {
                    xport_gte_execute(0x168002E);
                    if (lighting != 0)
                        gs_tmd_load_vector(normal, 0);
                    depth = xport_gte_read_data(7);
                    if (lighting != 0)
                        xport_gte_execute(lighting == 1 ? 0x108041B : 0xE80413);
                    if (lighting == 0)
                        xport_store_le32(work + 5, xport_gte_read_data(14));
                    slot = base + ((((depth - bias) >> (shift & 31)) & 0xFFFF) * 4);
                    xport_store_le32(work, 0x05000000);
                    AddPrim(psx_addr(slot, 4), work);
                    if (lighting != 0)
                    {
                        xport_store_le32(work + 5, xport_gte_read_data(14));
                        xport_store_le32(work + 1, xport_gte_read_data(22));
                    }
                    work += 6;
                }
            }
        }
        indices0 = next0;
        indices1 = next1;
    } while (--count != 0);
    return work;
}

uint32 *GsTMDfastF4L(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table)
{
    return gs_tmd_fast_f4(primitive, vertices, normals, work, count, shift, ordering_table, 1);
}

uint32 *GsTMDfastF4LFG(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table)
{
    return gs_tmd_fast_f4(primitive, vertices, normals, work, count, shift, ordering_table, 2);
}

uint32 *GsTMDfastF4NL(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table)
{
    return gs_tmd_fast_f4(primitive, vertices, normals, work, count, shift, ordering_table, 0);
}

static uint32 *gs_tmd_fast_tf3(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 lighting)
{
    uint32 base = r_u32(ordering_table + 4);
    uint32 bias, indices0, indices1, next0, next1, normal, color, uv0, uv1, uv2, depth, slot;
    if (count == 0)
        return work;
    bias = r_u32(ordering_table + 8);
    if (lighting == 0)
    {
        indices1 = r_u32(primitive + 20);
        indices0 = (uint32)r_u16(primitive + 18) << 16;
    }
    else
    {
        indices0 = r_u32(primitive + 16);
        indices1 = r_u32(primitive + 20);
    }
    do
    {
        gs_tmd_load_vector(vertices + (indices0 >> 16) * 8, 0);
        gs_tmd_load_vector(vertices + (indices1 & 0xFFFF) * 8, 1);
        gs_tmd_load_vector(vertices + (indices1 >> 16) * 8, 2);
        xport_gte_execute(0x280030);
        normal = normals + (indices0 & 0xFFFF) * 8;
        color = 0x00808080 | (((uint32)(sint32)r_s8(primitive + 3) | (gs_sort_attributes.bit30 << 1)) << 24);
        if (lighting != 0)
            xport_gte_write_data(6, color);
        uv0 = r_u32(primitive + 4);
        uv1 = r_u32(primitive + 8);
        uv2 = r_u32(primitive + 12);
        work = gs_tmd_work_buffer(work, 8);
        xport_store_le32(work + 3, uv0);
        xport_store_le32(work + 5, uv1);
        xport_store_le32(work + 7, uv2);
        if (lighting == 0)
            xport_store_le32(work + 1, color);
        primitive += 24;
        if (lighting == 0)
        {
            next1 = r_u32(primitive + 20);
            next0 = (uint32)r_u16(primitive + 18) << 16;
        }
        else
        {
            next0 = r_u32(primitive + 16);
            next1 = r_u32(primitive + 20);
        }
        if ((sint32)xport_gte_read_flag() >= 0)
        {
            xport_gte_execute(0x1400006);
            if ((sint32)xport_gte_read_data(24) > 0)
            {
                xport_gte_execute(0x158002D);
                if (lighting != 0)
                    gs_tmd_load_vector(normal, 0);
                depth = xport_gte_read_data(7);
                if (lighting != 0)
                    xport_gte_execute(lighting == 1 ? 0x108041B : 0xE80413);
                slot = base + ((((depth - bias) >> (shift & 31)) & 0xFFFF) * 4);
                xport_store_le32(work, 0x07000000);
                AddPrim(psx_addr(slot, 4), work);
                xport_store_le32(work + 2, xport_gte_read_data(12));
                xport_store_le32(work + 4, xport_gte_read_data(13));
                xport_store_le32(work + 6, xport_gte_read_data(14));
                if (lighting != 0)
                    xport_store_le32(work + 1, xport_gte_read_data(22));
                work += 8;
            }
        }
        indices0 = next0;
        indices1 = next1;
    } while (--count != 0);
    return work;
}

uint32 *GsTMDfastTF3L(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table)
{
    return gs_tmd_fast_tf3(primitive, vertices, normals, work, count, shift, ordering_table, 1);
}

uint32 *GsTMDfastTF3LFG(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table)
{
    return gs_tmd_fast_tf3(primitive, vertices, normals, work, count, shift, ordering_table, 2);
}

uint32 *GsTMDfastTF3NL(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table)
{
    return gs_tmd_fast_tf3(primitive, vertices, normals, work, count, shift, ordering_table, 0);
}

static uint32 *gs_tmd_fast_g4(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 lighting)
{
    uint32 base = r_u32(ordering_table + 4);
    uint32 bias, indices[3], fourth, fourth_normal, index, depth, slot, color, prefetched = 0;
    if (count == 0)
        return work;
    bias = r_u32(ordering_table + 8);
    if (lighting == 0)
    {
        for (index = 0; index < 3; ++index)
            indices[index] = (uint32)r_u16(primitive + 10 + index * 4) << 16;
        prefetched = 1;
    }
    do
    {
        if (!prefetched)
            for (index = 0; index < 3; ++index)
                indices[index] = r_u32(primitive + 8 + index * 4);
        prefetched = 0;
        for (index = 0; index < 3; ++index)
            gs_tmd_load_vector(vertices + (indices[index] >> 16) * 8, index);
        xport_gte_execute(0x280030);
        if (lighting == 0)
        {
            color = r_u32(primitive + 4) | (gs_sort_attributes.bit30 << 25);
            xport_gte_write_data(20, color);
            fourth = vertices + (uint32)r_u16(primitive + 22) * 8;
            primitive += 24;
            for (index = 0; index < 3; ++index)
                indices[index] = (uint32)r_u16(primitive + 10 + index * 4) << 16;
            prefetched = 1;
        }
        else
        {
            fourth = r_u32(primitive + 20);
            fourth_normal = normals + (fourth & 0xFFFF) * 8;
            fourth = vertices + (fourth >> 16) * 8;
        }
        if ((sint32)xport_gte_read_flag() >= 0)
        {
            xport_gte_execute(0x1400006);
            gs_tmd_load_vector(fourth, 0);
            if ((sint32)xport_gte_read_data(24) > 0)
            {
                work = gs_tmd_work_buffer(work, 9);
                for (index = 0; index < 3; ++index)
                    xport_store_le32(work + 2 + index * 2, xport_gte_read_data(12 + index));
                xport_gte_execute(0x180001);
                if (lighting != 0)
                {
                    color = r_u32(primitive + 4) | (gs_sort_attributes.bit30 << 25);
                    xport_gte_write_data(6, color);
                    gs_tmd_load_vector(normals + (indices[1] & 0xFFFF) * 8, 1);
                    gs_tmd_load_vector(normals + (indices[2] & 0xFFFF) * 8, 2);
                }
                if ((sint32)xport_gte_read_flag() >= 0)
                {
                    xport_gte_execute(0x168002E);
                    if (lighting != 0)
                        gs_tmd_load_vector(fourth_normal, 0);
                    depth = xport_gte_read_data(7);
                    if (lighting != 0)
                    {
                        xport_gte_execute(lighting == 1 ? 0x108041B : 0xE80413);
                        xport_store_le32(work + 7, xport_gte_read_data(22));
                        gs_tmd_load_vector(normals + (indices[0] & 0xFFFF) * 8, 0);
                        xport_gte_execute(lighting == 1 ? 0x118043F : 0xF80416);
                        primitive += 24;
                    }
                    slot = base + ((((depth - bias) >> (shift & 31)) & 0xFFFF) * 4);
                    xport_store_le32(work, 0x08000000);
                    AddPrim(psx_addr(slot, 4), work);
                    if (lighting != 0)
                    {
                        for (index = 0; index < 3; ++index)
                            indices[index] = r_u32(primitive + 8 + index * 4);
                        prefetched = 1;
                        xport_store_le32(work + 8, xport_gte_read_data(14));
                        for (index = 0; index < 3; ++index)
                            xport_store_le32(work + 1 + index * 2, xport_gte_read_data(20 + index));
                    }
                    else
                    {
                        for (index = 0; index < 4; ++index)
                            xport_store_le32(work + 1 + index * 2, xport_gte_read_data(20));
                        xport_store_le32(work + 8, xport_gte_read_data(14));
                    }
                    work += 9;
                }
            }
        }
        if (!prefetched)
            primitive += 24;
    } while (--count != 0);
    return work;
}

uint32 *GsTMDfastG4L(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table)
{
    return gs_tmd_fast_g4(primitive, vertices, normals, work, count, shift, ordering_table, 1);
}

uint32 *GsTMDfastG4LFG(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table)
{
    return gs_tmd_fast_g4(primitive, vertices, normals, work, count, shift, ordering_table, 2);
}

uint32 *GsTMDfastG4NL(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table)
{
    return gs_tmd_fast_g4(primitive, vertices, normals, work, count, shift, ordering_table, 0);
}

static uint32 *gs_tmd_fast_tf4(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 lighting)
{
    uint32 base = r_u32(ordering_table + 4);
    uint32 bias, indices0, indices1, next0, next1, normal, fourth, color, current, uv0, uv1, depth, slot;
    if (count == 0)
        return work;
    bias = r_u32(ordering_table + 8);
    if (lighting == 0)
    {
        indices1 = r_u32(primitive + 24);
        indices0 = (uint32)r_u16(primitive + 22) << 16;
    }
    else
    {
        indices0 = r_u32(primitive + 20);
        indices1 = r_u32(primitive + 24);
    }
    do
    {
        gs_tmd_load_vector(vertices + (indices0 >> 16) * 8, 0);
        gs_tmd_load_vector(vertices + (indices1 & 0xFFFF) * 8, 1);
        gs_tmd_load_vector(vertices + (indices1 >> 16) * 8, 2);
        xport_gte_execute(0x280030);
        if (lighting != 0)
        {
            color = 0x00808080 | (((uint32)(sint32)r_s8(primitive + 3) | (gs_sort_attributes.bit30 << 1)) << 24);
            xport_gte_write_data(6, color);
        }
        normal = normals + (indices0 & 0xFFFF) * 8;
        fourth = vertices + (uint32)r_u16(primitive + 28) * 8;
        current = primitive;
        primitive += 32;
        if (lighting == 0)
        {
            next1 = r_u32(primitive + 24);
            next0 = (uint32)r_u16(primitive + 22) << 16;
        }
        else
        {
            next0 = r_u32(primitive + 20);
            next1 = r_u32(primitive + 24);
        }
        if ((sint32)xport_gte_read_flag() >= 0)
        {
            xport_gte_execute(0x1400006);
            gs_tmd_load_vector(fourth, 0);
            if ((sint32)xport_gte_read_data(24) > 0)
            {
                work = gs_tmd_work_buffer(work, 10);
                xport_store_le32(work + 2, xport_gte_read_data(12));
                xport_store_le32(work + 4, xport_gte_read_data(13));
                xport_store_le32(work + 6, xport_gte_read_data(14));
                xport_gte_execute(0x180001);
                uv0 = r_u32(current + 4);
                uv1 = r_u32(current + 8);
                xport_store_le32(work + 3, uv0);
                xport_store_le32(work + 5, uv1);
                uv0 = r_u32(current + 12);
                uv1 = r_u32(current + 16);
                xport_store_le32(work + 7, uv0);
                xport_store_le32(work + 9, uv1);
                if (lighting == 0)
                {
                    color = 0x00808080 | (((uint32)(sint32)r_s8(current + 3) | (gs_sort_attributes.bit30 << 1)) << 24);
                    xport_store_le32(work + 1, color);
                }
                if ((sint32)xport_gte_read_flag() >= 0)
                {
                    xport_gte_execute(0x168002E);
                    if (lighting != 0)
                        gs_tmd_load_vector(normal, 0);
                    depth = xport_gte_read_data(7);
                    if (lighting != 0)
                        xport_gte_execute(lighting == 1 ? 0x108041B : 0xE80413);
                    if (lighting == 0)
                        xport_store_le32(work + 8, xport_gte_read_data(14));
                    slot = base + ((((depth - bias) >> (shift & 31)) & 0xFFFF) * 4);
                    xport_store_le32(work, 0x09000000);
                    AddPrim(psx_addr(slot, 4), work);
                    if (lighting != 0)
                    {
                        xport_store_le32(work + 8, xport_gte_read_data(14));
                        xport_store_le32(work + 1, xport_gte_read_data(22));
                    }
                    work += 10;
                }
            }
        }
        indices0 = next0;
        indices1 = next1;
    } while (--count != 0);
    return work;
}

uint32 *GsTMDfastTF4L(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table)
{
    return gs_tmd_fast_tf4(primitive, vertices, normals, work, count, shift, ordering_table, 1);
}

uint32 *GsTMDfastTF4LFG(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table)
{
    return gs_tmd_fast_tf4(primitive, vertices, normals, work, count, shift, ordering_table, 2);
}

uint32 *GsTMDfastTF4NL(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table)
{
    return gs_tmd_fast_tf4(primitive, vertices, normals, work, count, shift, ordering_table, 0);
}

static uint32 *gs_tmd_fast_tg3_lit(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 fog)
{
    uint32 base = r_u32(ordering_table + 4);
    uint32 bias, indices[3], uv[3], index, color, depth, slot, prefetched = 0;
    if (count == 0)
        return work;
    bias = r_u32(ordering_table + 8);
    do
    {
        if (!prefetched)
            for (index = 0; index < 3; ++index)
                indices[index] = r_u32(primitive + 16 + index * 4);
        prefetched = 0;
        for (index = 0; index < 3; ++index)
            gs_tmd_load_vector(vertices + (indices[index] >> 16) * 8, index);
        xport_gte_execute(0x280030);
        color = 0x00808080 | (((uint32)(sint32)r_s8(primitive + 3) | (gs_sort_attributes.bit30 << 1)) << 24);
        xport_gte_write_data(6, color);
        if ((sint32)xport_gte_read_flag() >= 0)
        {
            xport_gte_execute(0x1400006);
            if ((sint32)xport_gte_read_data(24) > 0)
            {
                xport_gte_execute(0x158002D);
                for (index = 0; index < 3; ++index)
                    gs_tmd_load_vector(normals + (indices[index] & 0xFFFF) * 8, index);
                depth = xport_gte_read_data(7);
                xport_gte_execute(fog ? 0xF80416 : 0x118043F);
                slot = base + ((((depth - bias) >> (shift & 31)) & 0xFFFF) * 4);
                work = gs_tmd_work_buffer(work, 10);
                xport_store_le32(work, 0x09000000);
                AddPrim(psx_addr(slot, 4), work);
                for (index = 0; index < 3; ++index)
                    uv[index] = r_u32(primitive + 4 + index * 4);
                for (index = 0; index < 3; ++index)
                    xport_store_le32(work + 3 + index * 3, uv[index]);
                for (index = 0; index < 3; ++index)
                    xport_store_le32(work + 2 + index * 3, xport_gte_read_data(12 + index));
                for (index = 0; index < 3; ++index)
                    xport_store_le32(work + 1 + index * 3, xport_gte_read_data(20 + index));
                primitive += 28;
                for (index = 0; index < 3; ++index)
                    indices[index] = r_u32(primitive + 16 + index * 4);
                prefetched = 1;
                work += 10;
            }
        }
        if (!prefetched)
            primitive += 28;
    } while (--count != 0);
    return work;
}

uint32 *GsTMDfastTG3L(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table)
{
    return gs_tmd_fast_tg3_lit(primitive, vertices, normals, work, count, shift, ordering_table, 0);
}

uint32 *GsTMDfastTG3LFG(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table)
{
    return gs_tmd_fast_tg3_lit(primitive, vertices, normals, work, count, shift, ordering_table, 1);
}

uint32 *GsTMDfastTG3NL(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table)
{
    uint32 base = r_u32(ordering_table + 4);
    uint32 bias, indices[3], index, color, depth, slot;
    if (count == 0)
        return work;
    bias = r_u32(ordering_table + 8);
    for (index = 0; index < 3; ++index)
        indices[index] = r_u16(primitive + 18 + index * 4);
    do
    {
        for (index = 0; index < 3; ++index)
            gs_tmd_load_vector(vertices + indices[index] * 8, index);
        xport_gte_execute(0x280030);
        for (index = 0; index < 3; ++index)
            xport_gte_write_data(20 + index, r_u32(primitive + 4 + index * 4));
        color = 0x25808080 | ((((uint32)(sint32)r_s8(primitive + 3) & 2) | (gs_sort_attributes.bit30 << 1)) << 24);
        work = gs_tmd_work_buffer(work, 8);
        xport_store_le32(work + 1, color);
        primitive += 28;
        for (index = 0; index < 3; ++index)
            indices[index] = r_u16(primitive + 18 + index * 4);
        if ((sint32)xport_gte_read_flag() >= 0)
        {
            xport_gte_execute(0x1400006);
            if ((sint32)xport_gte_read_data(24) > 0)
            {
                xport_gte_execute(0x158002D);
                depth = xport_gte_read_data(7);
                for (index = 0; index < 3; ++index)
                    xport_store_le32(work + 3 + index * 2, xport_gte_read_data(20 + index));
                for (index = 0; index < 3; ++index)
                    xport_store_le32(work + 2 + index * 2, xport_gte_read_data(12 + index));
                slot = base + ((((depth - bias) >> (shift & 31)) & 0xFFFF) * 4);
                xport_store_le32(work, 0x07000000);
                AddPrim(psx_addr(slot, 4), work);
                work += 8;
            }
        }
    } while (--count != 0);
    return work;
}

static uint32 *gs_tmd_fast_tg4_lit(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 fog)
{
    uint32 base = r_u32(ordering_table + 4);
    uint32 bias, indices[3], fourth, fourth_normal, uv[4], index, color, depth, slot, prefetched = 0;
    if (count == 0)
        return work;
    bias = r_u32(ordering_table + 8);
    do
    {
        if (!prefetched)
            for (index = 0; index < 3; ++index)
                indices[index] = r_u32(primitive + 20 + index * 4);
        prefetched = 0;
        for (index = 0; index < 3; ++index)
            gs_tmd_load_vector(vertices + (indices[index] >> 16) * 8, index);
        xport_gte_execute(0x280030);
        color = 0x00808080 | (((uint32)(sint32)r_s8(primitive + 3) | (gs_sort_attributes.bit30 << 1)) << 24);
        xport_gte_write_data(6, color);
        fourth = r_u32(primitive + 32);
        fourth_normal = normals + (fourth & 0xFFFF) * 8;
        fourth = vertices + (fourth >> 16) * 8;
        if ((sint32)xport_gte_read_flag() >= 0)
        {
            xport_gte_execute(0x1400006);
            gs_tmd_load_vector(fourth, 0);
            if ((sint32)xport_gte_read_data(24) > 0)
            {
                work = gs_tmd_work_buffer(work, 13);
                for (index = 0; index < 3; ++index)
                    xport_store_le32(work + 2 + index * 3, xport_gte_read_data(12 + index));
                xport_gte_execute(0x180001);
                gs_tmd_load_vector(normals + (indices[1] & 0xFFFF) * 8, 1);
                gs_tmd_load_vector(normals + (indices[2] & 0xFFFF) * 8, 2);
                if ((sint32)xport_gte_read_flag() >= 0)
                {
                    xport_gte_execute(0x168002E);
                    gs_tmd_load_vector(fourth_normal, 0);
                    depth = xport_gte_read_data(7);
                    xport_gte_execute(fog ? 0xE80413 : 0x108041B);
                    for (index = 0; index < 4; ++index)
                        uv[index] = r_u32(primitive + 4 + index * 4);
                    for (index = 0; index < 4; ++index)
                        xport_store_le32(work + 3 + index * 3, uv[index]);
                    xport_store_le32(work + 10, xport_gte_read_data(22));
                    gs_tmd_load_vector(normals + (indices[0] & 0xFFFF) * 8, 0);
                    xport_gte_execute(fog ? 0xF80416 : 0x118043F);
                    slot = base + ((((depth - bias) >> (shift & 31)) & 0xFFFF) * 4);
                    xport_store_le32(work, 0x0C000000);
                    AddPrim(psx_addr(slot, 4), work);
                    primitive += 36;
                    for (index = 0; index < 3; ++index)
                        indices[index] = r_u32(primitive + 20 + index * 4);
                    prefetched = 1;
                    xport_store_le32(work + 11, xport_gte_read_data(14));
                    for (index = 0; index < 3; ++index)
                        xport_store_le32(work + 1 + index * 3, xport_gte_read_data(20 + index));
                    work += 13;
                }
            }
        }
        if (!prefetched)
            primitive += 36;
    } while (--count != 0);
    return work;
}

uint32 *GsTMDfastTG4L(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table)
{
    return gs_tmd_fast_tg4_lit(primitive, vertices, normals, work, count, shift, ordering_table, 0);
}

uint32 *GsTMDfastTG4LFG(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table)
{
    return gs_tmd_fast_tg4_lit(primitive, vertices, normals, work, count, shift, ordering_table, 1);
}

uint32 *GsTMDfastTG4NL(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table)
{
    uint32 base = r_u32(ordering_table + 4);
    uint32 bias, indices[3], fourth, current, index, color, uv0, uv1, uv2, depth, slot;
    if (count == 0)
        return work;
    bias = r_u32(ordering_table + 8);
    for (index = 0; index < 3; ++index)
        indices[index] = r_u16(primitive + 22 + index * 4);
    do
    {
        for (index = 0; index < 3; ++index)
            gs_tmd_load_vector(vertices + indices[index] * 8, index);
        xport_gte_execute(0x280030);
        fourth = vertices + (uint32)r_u16(primitive + 34) * 8;
        current = primitive;
        primitive += 36;
        for (index = 0; index < 3; ++index)
            indices[index] = r_u16(primitive + 22 + index * 4);
        if ((sint32)xport_gte_read_flag() >= 0)
        {
            xport_gte_execute(0x1400006);
            gs_tmd_load_vector(fourth, 0);
            if ((sint32)xport_gte_read_data(24) > 0)
            {
                work = gs_tmd_work_buffer(work, 10);
                xport_store_le32(work + 2, xport_gte_read_data(12));
                xport_store_le32(work + 4, xport_gte_read_data(13));
                xport_store_le32(work + 6, xport_gte_read_data(14));
                xport_gte_execute(0x180001);
                color = 0x2D808080 | ((((uint32)(sint32)r_s8(current + 3) & 2) | (gs_sort_attributes.bit30 << 1)) << 24);
                xport_store_le32(work + 1, color);
                uv0 = r_u32(current + 4);
                uv1 = r_u32(current + 8);
                if ((sint32)xport_gte_read_flag() >= 0)
                {
                    xport_gte_execute(0x168002E);
                    uv2 = r_u32(current + 12);
                    xport_store_le32(work + 3, uv0);
                    xport_store_le32(work + 5, uv1);
                    xport_store_le32(work + 7, uv2);
                    uv0 = r_u32(current + 16);
                    depth = xport_gte_read_data(7);
                    xport_store_le32(work + 9, uv0);
                    xport_store_le32(work + 8, xport_gte_read_data(14));
                    slot = base + ((((depth - bias) >> (shift & 31)) & 0xFFFF) * 4);
                    xport_store_le32(work, 0x09000000);
                    AddPrim(psx_addr(slot, 4), work);
                    work += 10;
                }
            }
        }
    } while (--count != 0);
    return work;
}

static uint32 gs_tmd_unlit_texture_color(uint32 primitive, uint32 rgb)
{
    uint32 code = (uint32)(sint32)r_s8(primitive + 3) | (gs_sort_attributes.bit30 << 1);
    return (rgb & 0xFFFFFF) | ((code & 0xFE) << 24);
}

static void gs_tmd_link_unlit(uint32 *work, uint32 base, uint32 bias, uint32 depth, uint32 shift, uint32 words)
{
    uint32 slot = base + ((((depth - bias) >> (shift & 31)) & 0xFFFF) * 4);
    xport_store_le32(work, words << 24);
    AddPrim(psx_addr(slot, 4), work);
}

static uint32 *gs_tmd_fast_unlit_triangle(uint32 primitive, uint32 vertices, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 kind)
{
    static const uint32 index_offsets[4] = {8, 16, 20, 28};
    static const uint32 packet_words[4] = {5, 7, 8, 10};
    uint32 base = r_u32(ordering_table + 4);
    uint32 bias, offset, stride, packed, third, next_packed, next_third, color, uv[3], index, depth, step;
    if (count == 0)
        return work;
    bias = r_u32(ordering_table + 8);
    offset = index_offsets[kind];
    stride = offset + 8;
    packed = r_u32(primitive + offset);
    third = r_u16(primitive + offset + 4);
    step = kind == 0 ? 1 : kind == 3 ? 3 : 2;
    do
    {
        gs_tmd_load_vector(vertices + (packed & 0xFFFF) * 8, 0);
        gs_tmd_load_vector(vertices + (packed >> 16) * 8, 1);
        gs_tmd_load_vector(vertices + third * 8, 2);
        xport_gte_execute(0x280030);
        if (kind <= 1)
        {
            color = r_u32(primitive + 4) | (gs_sort_attributes.bit30 << 25);
            xport_gte_write_data(kind == 0 ? 22 : 20, color);
        }
        else if (kind == 2)
        {
            color = r_u32(primitive + 16);
            xport_gte_write_data(22, gs_tmd_unlit_texture_color(primitive, color));
        }
        else
        {
            color = (uint32)(sint32)r_s8(primitive + 3) | (gs_sort_attributes.bit30 << 1);
            color = ((color & 0xFE) << 24) | (r_u32(primitive + 16) & 0xFFFFFF);
            xport_gte_write_data(20, color);
        }
        if (kind == 1 || kind == 3)
        {
            xport_gte_write_data(21, r_u32(primitive + (kind == 1 ? 8 : 20)));
            xport_gte_write_data(22, r_u32(primitive + (kind == 1 ? 12 : 24)));
        }
        if (kind >= 2)
        {
            for (index = 0; index < 3; ++index)
                uv[index] = r_u32(primitive + 4 + index * 4);
            work = gs_tmd_work_buffer(work, packet_words[kind]);
            for (index = 0; index < 3; ++index)
                xport_store_le32(work + 3 + index * step, uv[index]);
        }
        primitive += stride;
        next_packed = r_u32(primitive + offset);
        next_third = r_u16(primitive + offset + 4);
        if ((sint32)xport_gte_read_flag() >= 0)
        {
            xport_gte_execute(0x1400006);
            if ((sint32)xport_gte_read_data(24) > 0)
            {
                xport_gte_execute(0x158002D);
                depth = xport_gte_read_data(7);
                work = gs_tmd_work_buffer(work, packet_words[kind]);
                if (kind == 0 || kind == 2)
                {
                    gs_tmd_link_unlit(work, base, bias, depth, shift, packet_words[kind] - 1);
                    xport_store_le32(work + 1, xport_gte_read_data(22));
                    for (index = 0; index < 3; ++index)
                        xport_store_le32(work + 2 + index * step, xport_gte_read_data(12 + index));
                }
                else
                {
                    if (kind == 1)
                        for (index = 0; index < 3; ++index)
                            xport_store_le32(work + 1 + index * step, xport_gte_read_data(20 + index));
                    for (index = 0; index < 3; ++index)
                        xport_store_le32(work + 2 + index * step, xport_gte_read_data(12 + index));
                    if (kind == 3)
                        for (index = 0; index < 3; ++index)
                            xport_store_le32(work + 1 + index * step, xport_gte_read_data(20 + index));
                    gs_tmd_link_unlit(work, base, bias, depth, shift, packet_words[kind] - 1);
                }
                work += packet_words[kind];
            }
        }
        packed = next_packed;
        third = next_third;
    } while (--count != 0);
    return work;
}

static uint32 *gs_tmd_fast_unlit_quad(uint32 primitive, uint32 vertices, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 kind)
{
    static const uint32 index_offsets[4] = {8, 20, 24, 36};
    static const uint32 packet_words[4] = {6, 9, 10, 13};
    uint32 base = r_u32(ordering_table + 4);
    uint32 bias, offset, stride, packed, third, next_packed, next_third, fourth, current, color, uv0, uv1, uv2, index, depth, step;
    if (count == 0)
        return work;
    bias = r_u32(ordering_table + 8);
    offset = index_offsets[kind];
    stride = offset + 8;
    packed = r_u32(primitive + offset);
    third = r_u16(primitive + offset + 4);
    step = kind == 0 ? 1 : kind == 3 ? 3 : 2;
    do
    {
        gs_tmd_load_vector(vertices + (packed & 0xFFFF) * 8, 0);
        gs_tmd_load_vector(vertices + (packed >> 16) * 8, 1);
        gs_tmd_load_vector(vertices + third * 8, 2);
        xport_gte_execute(0x280030);
        if (kind == 0)
            xport_gte_write_data(22, r_u32(primitive + 4) | (gs_sort_attributes.bit30 << 25));
        else if (kind == 2)
        {
            color = r_u32(primitive + 20);
            fourth = r_u16(primitive + offset + 6);
            color = gs_tmd_unlit_texture_color(primitive, color);
            work = gs_tmd_work_buffer(work, packet_words[kind]);
            xport_store_le32(work + 1, color);
        }
        else if (kind == 3)
        {
            color = (uint32)(sint32)r_s8(primitive + 3) | (gs_sort_attributes.bit30 << 1);
            color = ((color & 0xFE) << 24) | (r_u32(primitive + 20) & 0xFFFFFF);
            xport_gte_write_data(20, color);
            xport_gte_write_data(21, r_u32(primitive + 24));
            xport_gte_write_data(22, r_u32(primitive + 28));
            xport_gte_write_data(6, r_u32(primitive + 32));
        }
        if (kind != 2)
            fourth = r_u16(primitive + offset + 6);
        fourth = vertices + fourth * 8;
        current = primitive;
        primitive += stride;
        next_packed = r_u32(primitive + offset);
        next_third = r_u16(primitive + offset + 4);
        if ((sint32)xport_gte_read_flag() >= 0)
        {
            xport_gte_execute(0x1400006);
            gs_tmd_load_vector(fourth, 0);
            if ((sint32)xport_gte_read_data(24) > 0)
            {
                work = gs_tmd_work_buffer(work, packet_words[kind]);
                if (kind == 0)
                    xport_store_le32(work + 1, xport_gte_read_data(22));
                for (index = 0; index < 3; ++index)
                    xport_store_le32(work + 2 + index * step, xport_gte_read_data(12 + index));
                xport_gte_execute(0x180001);
                if (kind == 1)
                {
                    color = r_u32(current + 4) | (gs_sort_attributes.bit30 << 25);
                    uv0 = r_u32(current + 8);
                    uv1 = r_u32(current + 12);
                    xport_store_le32(work + 1, color);
                    xport_store_le32(work + 3, uv0);
                    xport_store_le32(work + 5, uv1);
                }
                else if (kind >= 2)
                {
                    uv0 = r_u32(current + 4);
                    uv1 = r_u32(current + 8);
                    if (kind == 2)
                    {
                        xport_store_le32(work + 3, uv0);
                        xport_store_le32(work + 5, uv1);
                        uv0 = r_u32(current + 12);
                        uv1 = r_u32(current + 16);
                        xport_store_le32(work + 7, uv0);
                        xport_store_le32(work + 9, uv1);
                    }
                }
                if ((sint32)xport_gte_read_flag() >= 0)
                {
                    xport_gte_execute(0x168002E);
                    if (kind == 1)
                    {
                        color = r_u32(current + 16);
                        depth = xport_gte_read_data(7);
                        xport_store_le32(work + 7, color);
                    }
                    else if (kind == 3)
                    {
                        uv2 = r_u32(current + 12);
                        xport_store_le32(work + 3, uv0);
                        xport_store_le32(work + 6, uv1);
                        xport_store_le32(work + 9, uv2);
                        uv0 = r_u32(current + 16);
                        depth = xport_gte_read_data(7);
                        xport_store_le32(work + 12, uv0);
                    }
                    else
                        depth = xport_gte_read_data(7);
                    xport_store_le32(work + 2 + 3 * step, xport_gte_read_data(14));
                    if (kind == 3)
                    {
                        for (index = 0; index < 3; ++index)
                            xport_store_le32(work + 1 + index * 3, xport_gte_read_data(20 + index));
                        xport_store_le32(work + 10, xport_gte_read_data(6));
                    }
                    gs_tmd_link_unlit(work, base, bias, depth, shift, packet_words[kind] - 1);
                    work += packet_words[kind];
                }
            }
        }
        packed = next_packed;
        third = next_third;
    } while (--count != 0);
    return work;
}

static uint32 *gs_tmd_fast_gradient_nl(uint32 primitive, uint32 vertices, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch, uint32 gouraud)
{
    uint32 base = r_u32(ordering_table + 4);
    uint32 index, addresses[3], flag, slot, color, depth, depth_shift;
    w_u32(scratch + 12, shift);
    w_u32(scratch + 16, base);
    while ((sint32)count > 0)
    {
        for (index = 0; index < 3; ++index)
            addresses[index] = vertices + (uint32)r_u16(primitive + 18 + index * (gouraud ? 4 : 2)) * 8;
        for (index = 0; index < 3; ++index)
            gs_tmd_load_vector(addresses[index], index);
        xport_gte_execute(0x280030);
        color = r_u32(primitive + 4);
        if (!gouraud)
            color = (color & 0xFFFFFF) | 0x30000000;
        work = gs_tmd_work_buffer(work, 7);
        xport_store_le32(work + 1, color);
        flag = xport_gte_read_flag();
        w_u32(scratch + 36, flag);
        if (r_s32(scratch + 36) >= 0)
        {
            xport_gte_execute(0x1400006);
            xport_store_le32(work + 3, r_u32(primitive + 8));
            w_u32(scratch + 24, xport_gte_read_data(24));
            if (r_s32(scratch + 24) > 0)
            {
                for (index = 0; index < 3; ++index)
                    xport_store_le32(work + 2 + index * 2, xport_gte_read_data(12 + index));
                xport_gte_execute(0x158002D);
                xport_store_le32(work + 5, r_u32(primitive + 12));
                w_u32(scratch + 24, xport_gte_read_data(7));
                depth = r_u32(scratch + 24);
                depth_shift = r_u32(scratch + 12);
                slot = (uint32)((sint32)depth >> (depth_shift & 31));
                slot = (slot << 2) + r_u32(scratch + 16);
                w_u32(scratch + 52, slot);
                xport_store_le32(work, (r_u32(slot) & 0xFFFFFF) | 0x06000000);
                w_u32(r_u32(scratch + 52), gpu_packet_link((uintptr_t)work));
                work += 7;
            }
        }
        primitive += gouraud ? 28 : 24;
        --count;
    }
    return work;
}

uint32 *GsTMDfastF3GNL(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    (void)normals;
    return gs_tmd_fast_gradient_nl(primitive, vertices, work, count, shift, ordering_table, scratch, 0);
}

void gs_subdivision_midpoint(uint32 destination, uint32 left, uint32 right)
{
    uint32 offset;
    sint32 first, second;
    for (offset = 0; offset < 6; offset += 2)
    {
        first = r_s16(left + offset);
        second = r_s16(right + offset);
        w_u16(destination + offset, (uint32)((first + second) / 2));
    }
}

uint32 gs_subdivision_triangle_bounds(uint32 packet, uint32 scratch)
{
    uint32 offset;
    sint32 first, second, third, bound, extent;
    for (offset = 0; offset < 4; offset += 2)
    {
        first = r_s16(packet + 8 + offset);
        second = r_s16(packet + 12 + offset);
        if (second < first)
        {
            w_u16(scratch + 44 + offset, (uint32)first);
            w_u16(scratch + 40 + offset, r_u16(packet + 12 + offset));
        }
        else
        {
            w_u16(scratch + 40 + offset, (uint32)first);
            w_u16(scratch + 44 + offset, r_u16(packet + 12 + offset));
        }
        third = r_s16(packet + 16 + offset);
        bound = r_s16(scratch + 44 + offset);
        if (bound < third)
            w_u16(scratch + 44 + offset, (uint32)third);
        else
        {
            bound = r_s16(scratch + 40 + offset);
            if (third < bound)
                w_u16(scratch + 40 + offset, (uint32)third);
        }
        extent = r_s16(scratch + 48 + offset);
        bound = r_s16(scratch + 44 + offset);
        if (bound < -extent)
            return 1;
        bound = r_s16(scratch + 40 + offset);
        if (extent < bound)
            return 1;
    }
    return 0;
}

static void gs_subdivision_copy_vector(uint32 destination, uint32 source)
{
    uint32 first = r_u32(source);
    uint32 second = r_u32(source + 4);
    w_u32(destination, first);
    w_u32(destination + 4, second);
}

static uint32 gs_subdivision_slot(uint32 scratch)
{
    uint32 depth = r_u32(scratch + 24);
    uint32 shift = r_u32(scratch + 12);
    uint32 slot = (uint32)((sint32)depth >> (shift & 31));
    slot = (slot << 2) + r_u32(scratch + 16);
    w_u32(scratch + 52, slot);
    return slot;
}

static void gs_subdivision_link_f3(uint32 **cursor, uint32 scratch, uint32 slot)
{
    uint32 *work = gs_tmd_work_buffer(*cursor, 5);
    xport_store_le32(work, (r_u32(slot) & 0xFFFFFF) | 0x04000000);
    w_u32(r_u32(scratch + 52), gpu_packet_link((uintptr_t)work));
    *cursor = work + 5;
}

static void gs_subdivision_emit_f3(uint32 first, uint32 second, uint32 third, uint32 scratch, uint32 **cursor)
{
    uint32 *work, index, slot;
    gs_tmd_load_vector(first, 0);
    gs_tmd_load_vector(second, 1);
    gs_tmd_load_vector(third, 2);
    xport_gte_execute(0x280030);
    work = gs_tmd_work_buffer(*cursor, 5);
    xport_store_le32(work + 1, r_u32(scratch + 60));
    ((uint8 *)work)[3] = 4;
    ((uint8 *)work)[7] = 0x20;
    w_u32(scratch + 36, xport_gte_read_flag());
    if (r_s32(scratch + 36) >= 0)
    {
        xport_gte_execute(0x158002D);
        for (index = 0; index < 3; ++index)
            xport_store_le32(work + 2 + index, xport_gte_read_data(12 + index));
        w_u32(scratch + 24, xport_gte_read_data(7));
        slot = gs_subdivision_slot(scratch);
        gs_subdivision_link_f3(cursor, scratch, slot);
        w_u32(scratch + 20, gpu_packet_link((uintptr_t)*cursor));
    }
}

static void gs_subdivision_recurse_f3(uint32 vertices, uint32 scratch, uint32 level, uint32 **cursor)
{
    uint32 next = vertices + 24, index, emit = 0, slot;
    sint32 maximum, minimum, limit;
    for (index = 0; index < 3; ++index)
        gs_tmd_load_vector(vertices + index * 8, index);
    xport_gte_execute(0x280030);
    w_u32(scratch + 36, xport_gte_read_flag());
    for (index = 0; index < 3; ++index)
        w_u32(scratch + 64 + index * 4, xport_gte_read_data(12 + index));
    xport_gte_execute(0x158002D);
    w_u32(scratch + 24, (uint32)((sint32)xport_gte_read_data(19) >> 2));
    if (gs_subdivision_triangle_bounds(scratch + 56, scratch) != 0)
        return;
    if (r_u32(scratch) == level)
    {
        if (r_s32(scratch + 36) < 0)
            return;
        emit = 1;
    }
    else if (r_s32(scratch + 36) >= 0)
    {
        maximum = r_s16(scratch + 44);
        minimum = r_s16(scratch + 40);
        limit = r_s16(scratch + 32);
        if (maximum - minimum < limit)
        {
            maximum = r_s16(scratch + 46);
            minimum = r_s16(scratch + 42);
            limit = r_s16(scratch + 34);
            if (maximum - minimum < limit)
                emit = 1;
        }
    }
    if (emit)
    {
        uint32 *work = gs_tmd_work_buffer(*cursor, 5);
        for (index = 0; index < 5; ++index)
            xport_store_le32(work + index, r_u32(scratch + 56 + index * 4));
        slot = gs_subdivision_slot(scratch);
        gs_subdivision_link_f3(cursor, scratch, slot);
        w_u32(scratch + 20, gpu_packet_link((uintptr_t)*cursor));
        return;
    }
    ++level;
    gs_subdivision_copy_vector(next, vertices);
    gs_subdivision_midpoint(next + 8, vertices, vertices + 8);
    gs_subdivision_midpoint(next + 16, vertices, vertices + 16);
    gs_subdivision_emit_f3(vertices, vertices + 8, next + 8, scratch, cursor);
    gs_subdivision_recurse_f3(next, scratch, level, cursor);
    gs_subdivision_midpoint(next, vertices + 8, vertices + 16);
    gs_subdivision_recurse_f3(next, scratch, level, cursor);
    gs_subdivision_copy_vector(next + 16, vertices + 8);
    gs_subdivision_emit_f3(vertices + 8, vertices + 16, next, scratch, cursor);
    gs_subdivision_recurse_f3(next, scratch, level, cursor);
    gs_subdivision_copy_vector(next + 8, vertices + 16);
    gs_subdivision_midpoint(next + 16, vertices, vertices + 16);
    gs_subdivision_emit_f3(vertices + 16, vertices, next + 16, scratch, cursor);
    gs_subdivision_recurse_f3(next, scratch, level, cursor);
}

static uint32 *gs_a4_div_f3(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch, uint32 mode)
{
    uint32 base, index, addresses[3], normal, slot, depth, flags, threshold, subdivide;
    uint32 index_offset = mode == 3 ? 8 : 10;
    w_u16(scratch + 48, (uint32)(gs_screen_width / 2));
    w_u16(scratch + 50, (uint32)(gs_screen_height / 2));
    base = r_u32(ordering_table + 4);
    w_u32(scratch + 20, gpu_packet_link((uintptr_t)work));
    w_u32(scratch + 28, (uint32)gs_subdivision_distance);
    w_u16(scratch + 32, (uint32)gs_subdivision_width);
    if (mode != 3)
        w_u16(scratch + 34, (uint32)gs_subdivision_height);
    w_u32(scratch + 12, shift);
    w_u8(scratch + 59, 4);
    if (mode == 3)
        w_u16(scratch + 34, (uint32)gs_subdivision_height);
    w_u8(scratch + 63, 0x20);
    w_u32(scratch + 16, base);
    while ((sint32)count > 0)
    {
        if (mode == 1 || mode == 2)
            xport_gte_write_data(6, r_u32(primitive + 4));
        for (index = 0; index < 3; ++index)
            addresses[index] = vertices + (uint32)r_u16(primitive + index_offset + index * 2) * 8;
        for (index = 0; index < 3; ++index)
            gs_tmd_load_vector(addresses[index], index);
        xport_gte_execute(0x280030);
        w_u32(scratch + 36, xport_gte_read_flag());
        xport_gte_execute(0x1400006);
        w_u32(scratch + 24, xport_gte_read_data(24));
        if (r_s32(scratch + 24) > 0)
        {
            flags = r_u32(scratch + 36);
            if ((flags & 0x40000) == 0)
            {
                work = gs_tmd_work_buffer(work, 5);
                for (index = 0; index < 3; ++index)
                    xport_store_le32(work + 2 + index, xport_gte_read_data(12 + index));
                xport_gte_execute(0x158002D);
                if (mode == 0 || mode == 3)
                    xport_store_le32(work + 1, r_u32(primitive + 4));
                w_u32(scratch + 24, xport_gte_read_data(7));
                if (mode == 1 || mode == 2)
                {
                    normal = normals + (uint32)r_u16(primitive + 8) * 8;
                    gs_tmd_load_vector(normal, 0);
                    xport_gte_execute(mode == 2 ? 0xE80413 : 0x108041B);
                    slot = gs_subdivision_slot(scratch);
                    xport_store_le32(work + 1, xport_gte_read_data(22));
                    depth = r_u32(scratch + 24);
                    threshold = r_u32(scratch + 28);
                    subdivide = (sint32)depth < (sint32)threshold;
                }
                else
                {
                    depth = r_u32(scratch + 24);
                    flags = r_u32(scratch + 12);
                    base = r_u32(scratch + 16);
                    threshold = r_u32(scratch + 28);
                    slot = (uint32)((sint32)depth >> (flags & 31)) << 2;
                    depth = r_u32(scratch + 24);
                    slot += base;
                    subdivide = (sint32)depth < (sint32)threshold;
                    w_u32(scratch + 52, slot);
                }
                if (subdivide || r_s32(scratch + 36) < 0)
                {
                    if (mode == 1 || mode == 2)
                        w_u32(scratch + 60, xport_gte_read_data(22));
                    else
                        w_u32(scratch + 60, r_u32(primitive + 4));
                    for (index = 0; index < 3; ++index)
                    {
                        normal = vertices + (uint32)r_u16(primitive + index_offset + index * 2) * 8;
                        gs_subdivision_copy_vector(scratch + 76 + index * 8, normal);
                    }
                    w_u32(scratch + 20, gpu_packet_link((uintptr_t)work));
                    gs_subdivision_recurse_f3(scratch + 76, scratch, 0, &work);
                }
                else
                    gs_subdivision_link_f3(&work, scratch, slot);
            }
        }
        primitive += 16;
        --count;
    }
    w_u32(scratch + 20, gpu_packet_link((uintptr_t)work));
    return work;
}

uint32 *GsA4divF3L(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    return gs_a4_div_f3(primitive, vertices, normals, work, count, shift, ordering_table, scratch, 1);
}

uint32 *GsA4divF3LFG(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    return gs_a4_div_f3(primitive, vertices, normals, work, count, shift, ordering_table, scratch, 2);
}

uint32 *GsA4divF3NL(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    (void)normals;
    return gs_a4_div_f3(primitive, vertices, 0, work, count, shift, ordering_table, scratch, 0);
}

uint32 *GsA4divNF3(uint32 primitive, uint32 vertices, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    return gs_a4_div_f3(primitive, vertices, 0, work, count, shift, ordering_table, scratch, 3);
}

static void gs_subdivision_midpoint_color(uint32 destination, uint32 left, uint32 right)
{
    uint32 offset, first, second;
    gs_subdivision_midpoint(destination, left, right);
    for (offset = 8; offset < 11; ++offset)
    {
        first = r_u8(left + offset);
        second = r_u8(right + offset);
        w_u8(destination + offset, (first + second) >> 1);
    }
    w_u8(destination + 11, r_u8(left + 11));
}

static void gs_subdivision_copy_color_vertex(uint32 destination, uint32 source)
{
    uint32 first = r_u32(source);
    uint32 second = r_u32(source + 4);
    uint32 color = r_u32(source + 8);
    w_u32(destination, first);
    w_u32(destination + 4, second);
    w_u32(destination + 8, color);
}

static uint32 gs_subdivision_g3_bounds(uint32 packet, uint32 scratch)
{
    uint32 offset;
    sint32 first, second, third, bound, extent;
    for (offset = 0; offset < 4; offset += 2)
    {
        first = r_s16(packet + 8 + offset);
        second = r_s16(packet + 16 + offset);
        if (second < first)
        {
            w_u16(scratch + 44 + offset, (uint32)first);
            w_u16(scratch + 40 + offset, r_u16(packet + 16 + offset));
        }
        else
        {
            w_u16(scratch + 40 + offset, (uint32)first);
            w_u16(scratch + 44 + offset, r_u16(packet + 16 + offset));
        }
        third = r_s16(packet + 24 + offset);
        bound = r_s16(scratch + 44 + offset);
        if (bound < third)
            w_u16(scratch + 44 + offset, (uint32)third);
        else
        {
            bound = r_s16(scratch + 40 + offset);
            if (third < bound)
                w_u16(scratch + 40 + offset, (uint32)third);
        }
        extent = r_s16(scratch + 48 + offset);
        bound = r_s16(scratch + 44 + offset);
        if (bound < -extent)
            return 1;
        bound = r_s16(scratch + 40 + offset);
        if (extent < bound)
            return 1;
    }
    return 0;
}

static void gs_subdivision_link_g3(uint32 **cursor, uint32 scratch, uint32 slot)
{
    uint32 *work = gs_tmd_work_buffer(*cursor, 7);
    xport_store_le32(work, (r_u32(slot) & 0xFFFFFF) | 0x06000000);
    w_u32(r_u32(scratch + 52), gpu_packet_link((uintptr_t)work));
    *cursor = work + 7;
}

static void gs_subdivision_emit_g3(uint32 first, uint32 second, uint32 third, uint32 scratch, uint32 **cursor)
{
    uint32 *work, index, slot;
    gs_tmd_load_vector(first, 0);
    gs_tmd_load_vector(second, 1);
    gs_tmd_load_vector(third, 2);
    xport_gte_execute(0x280030);
    work = gs_tmd_work_buffer(*cursor, 7);
    xport_store_le32(work + 1, r_u32(first + 8));
    ((uint8 *)work)[3] = 6;
    ((uint8 *)work)[7] = 0x30;
    w_u32(scratch + 36, xport_gte_read_flag());
    if (r_s32(scratch + 36) >= 0)
    {
        xport_gte_execute(0x158002D);
        for (index = 0; index < 3; ++index)
            xport_store_le32(work + 2 + index * 2, xport_gte_read_data(12 + index));
        w_u32(scratch + 24, xport_gte_read_data(7));
        xport_store_le32(work + 3, r_u32(second + 8));
        xport_store_le32(work + 5, r_u32(third + 8));
        slot = gs_subdivision_slot(scratch);
        gs_subdivision_link_g3(cursor, scratch, slot);
        w_u32(scratch + 20, gpu_packet_link((uintptr_t)*cursor));
    }
}

static void gs_subdivision_recurse_g3(uint32 vertices, uint32 scratch, uint32 level, uint32 **cursor)
{
    uint32 next = vertices + 36, index, emit = 0, slot;
    sint32 maximum, minimum, limit;
    for (index = 0; index < 3; ++index)
        gs_tmd_load_vector(vertices + index * 12, index);
    xport_gte_execute(0x280030);
    w_u32(scratch + 36, xport_gte_read_flag());
    for (index = 0; index < 3; ++index)
        w_u32(scratch + 64 + index * 8, xport_gte_read_data(12 + index));
    xport_gte_execute(0x158002D);
    w_u32(scratch + 24, (uint32)((sint32)xport_gte_read_data(19) >> 2));
    if (gs_subdivision_g3_bounds(scratch + 56, scratch) != 0)
        return;
    if (r_u32(scratch) == level)
    {
        if (r_s32(scratch + 36) < 0)
            return;
        emit = 1;
    }
    else if (r_s32(scratch + 36) >= 0)
    {
        maximum = r_s16(scratch + 44);
        minimum = r_s16(scratch + 40);
        limit = r_s16(scratch + 32);
        if (maximum - minimum < limit)
        {
            maximum = r_s16(scratch + 46);
            minimum = r_s16(scratch + 42);
            limit = r_s16(scratch + 34);
            if (maximum - minimum < limit)
                emit = 1;
        }
    }
    if (emit)
    {
        uint32 *work = gs_tmd_work_buffer(*cursor, 7);
        xport_store_le32(work, r_u32(scratch + 56));
        for (index = 0; index < 3; ++index)
        {
            xport_store_le32(work + 1 + index * 2, r_u32(vertices + 8 + index * 12));
            xport_store_le32(work + 2 + index * 2, r_u32(scratch + 64 + index * 8));
        }
        slot = gs_subdivision_slot(scratch);
        gs_subdivision_link_g3(cursor, scratch, slot);
        w_u32(scratch + 20, gpu_packet_link((uintptr_t)*cursor));
        return;
    }
    ++level;
    gs_subdivision_copy_color_vertex(next, vertices);
    gs_subdivision_midpoint_color(next + 12, vertices, vertices + 12);
    gs_subdivision_midpoint_color(next + 24, vertices, vertices + 24);
    gs_subdivision_emit_g3(vertices, vertices + 12, next + 12, scratch, cursor);
    gs_subdivision_recurse_g3(next, scratch, level, cursor);
    gs_subdivision_midpoint_color(next, vertices + 12, vertices + 24);
    gs_subdivision_recurse_g3(next, scratch, level, cursor);
    gs_subdivision_copy_color_vertex(next + 24, vertices + 12);
    gs_subdivision_emit_g3(vertices + 12, vertices + 24, next, scratch, cursor);
    gs_subdivision_recurse_g3(next, scratch, level, cursor);
    gs_subdivision_copy_color_vertex(next + 12, vertices + 24);
    gs_subdivision_midpoint_color(next + 24, vertices, vertices + 24);
    gs_subdivision_emit_g3(vertices + 24, vertices, next + 24, scratch, cursor);
    gs_subdivision_recurse_g3(next, scratch, level, cursor);
}

static uint32 *gs_a4_div_g3_unlit(uint32 primitive, uint32 vertices, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch, uint32 distinct_colors)
{
    uint32 base, index, addresses[3], slot, depth, flags, threshold, subdivide, color;
    w_u16(scratch + 48, (uint32)(gs_screen_width / 2));
    w_u16(scratch + 50, (uint32)(gs_screen_height / 2));
    base = r_u32(ordering_table + 4);
    w_u32(scratch + 20, gpu_packet_link((uintptr_t)work));
    w_u32(scratch + 28, (uint32)gs_subdivision_distance);
    w_u16(scratch + 32, (uint32)gs_subdivision_width);
    if (!distinct_colors)
        w_u16(scratch + 34, (uint32)gs_subdivision_height);
    w_u32(scratch + 12, shift);
    w_u8(scratch + 59, 6);
    if (distinct_colors)
        w_u16(scratch + 34, (uint32)gs_subdivision_height);
    w_u8(scratch + 63, 0x30);
    w_u32(scratch + 16, base);
    while ((sint32)count > 0)
    {
        for (index = 0; index < 3; ++index)
            addresses[index] = vertices + (uint32)r_u16(primitive + (distinct_colors ? 16 + index * 2 : 10 + index * 4)) * 8;
        for (index = 0; index < 3; ++index)
            gs_tmd_load_vector(addresses[index], index);
        xport_gte_execute(0x280030);
        work = gs_tmd_work_buffer(work, 7);
        color = r_u32(primitive + 4);
        if (distinct_colors)
            color |= 0x30000000;
        xport_store_le32(work + 1, color);
        w_u32(scratch + 36, xport_gte_read_flag());
        xport_gte_execute(0x1400006);
        xport_store_le32(work + 3, r_u32(primitive + (distinct_colors ? 8 : 4)));
        w_u32(scratch + 24, xport_gte_read_data(24));
        if (r_s32(scratch + 24) > 0)
        {
            flags = r_u32(scratch + 36);
            if ((flags & 0x40000) == 0)
            {
                for (index = 0; index < 3; ++index)
                    xport_store_le32(work + 2 + index * 2, xport_gte_read_data(12 + index));
                xport_gte_execute(0x158002D);
                if (!distinct_colors)
                    xport_store_le32(work + 5, r_u32(primitive + 4));
                w_u32(scratch + 24, xport_gte_read_data(7));
                if (distinct_colors)
                    xport_store_le32(work + 5, r_u32(primitive + 12));
                depth = r_u32(scratch + 24);
                flags = r_u32(scratch + 12);
                base = r_u32(scratch + 16);
                threshold = r_u32(scratch + 28);
                slot = (uint32)((sint32)depth >> (flags & 31)) << 2;
                depth = r_u32(scratch + 24);
                slot += base;
                subdivide = (sint32)depth < (sint32)threshold;
                w_u32(scratch + 52, slot);
                if (subdivide || r_s32(scratch + 36) < 0)
                {
                    gs_subdivision_copy_vector(scratch + 84, vertices + (uint32)r_u16(primitive + (distinct_colors ? 16 : 10)) * 8);
                    if (distinct_colors)
                        for (index = 0; index < 3; ++index)
                            w_u32(scratch + 92 + index * 12, xport_load_le32(work + 1 + index * 2) | 0x30000000);
                    else
                    {
                        color = r_u32(primitive + 4);
                        w_u32(scratch + 92, color);
                        w_u32(scratch + 104, color);
                        w_u32(scratch + 116, color);
                    }
                    for (index = 1; index < 3; ++index)
                        gs_subdivision_copy_vector(scratch + 84 + index * 12, vertices + (uint32)r_u16(primitive + (distinct_colors ? 16 + index * 2 : 10 + index * 4)) * 8);
                    w_u32(scratch + 20, gpu_packet_link((uintptr_t)work));
                    gs_subdivision_recurse_g3(scratch + 84, scratch, 0, &work);
                }
                else
                    gs_subdivision_link_g3(&work, scratch, slot);
            }
        }
        primitive += distinct_colors ? 24 : 20;
        --count;
    }
    w_u32(scratch + 20, gpu_packet_link((uintptr_t)work));
    return work;
}

uint32 *GsA4divG3NL(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    (void)normals;
    return gs_a4_div_g3_unlit(primitive, vertices, work, count, shift, ordering_table, scratch, 0);
}

uint32 *GsA4divNG3(uint32 primitive, uint32 vertices, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    return gs_a4_div_g3_unlit(primitive, vertices, work, count, shift, ordering_table, scratch, 1);
}

static uint32 *gs_a4_div_g3_lit(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch, uint32 fog)
{
    uint32 base, index, addresses[3], slot, depth, threshold, vertex;
    w_u16(scratch + 48, (uint32)(gs_screen_width / 2));
    w_u16(scratch + 50, (uint32)(gs_screen_height / 2));
    base = r_u32(ordering_table + 4);
    w_u32(scratch + 20, gpu_packet_link((uintptr_t)work));
    w_u32(scratch + 28, (uint32)gs_subdivision_distance);
    w_u16(scratch + 32, (uint32)gs_subdivision_width);
    w_u16(scratch + 34, (uint32)gs_subdivision_height);
    w_u32(scratch + 12, shift);
    w_u8(scratch + 59, 6);
    w_u8(scratch + 63, 0x30);
    w_u32(scratch + 16, base);
    while ((sint32)count > 0)
    {
        xport_gte_write_data(6, r_u32(primitive + 4));
        for (index = 0; index < 3; ++index)
            addresses[index] = vertices + (uint32)r_u16(primitive + 10 + index * 4) * 8;
        for (index = 0; index < 3; ++index)
            gs_tmd_load_vector(addresses[index], index);
        xport_gte_execute(0x280030);
        w_u32(scratch + 36, xport_gte_read_flag());
        xport_gte_execute(0x1400006);
        w_u32(scratch + 24, xport_gte_read_data(24));
        if (r_s32(scratch + 24) > 0 && (r_u32(scratch + 36) & 0x40000) == 0)
        {
            work = gs_tmd_work_buffer(work, 7);
            for (index = 0; index < 3; ++index)
                xport_store_le32(work + 2 + index * 2, xport_gte_read_data(12 + index));
            xport_gte_execute(0x158002D);
            w_u32(scratch + 24, xport_gte_read_data(7));
            for (index = 0; index < 3; ++index)
                addresses[index] = normals + (uint32)r_u16(primitive + 8 + index * 4) * 8;
            for (index = 0; index < 3; ++index)
                gs_tmd_load_vector(addresses[index], index);
            xport_gte_execute(fog ? 0xF80416 : 0x118043F);
            slot = gs_subdivision_slot(scratch);
            for (index = 0; index < 3; ++index)
                xport_store_le32(work + 1 + index * 2, xport_gte_read_data(20 + index));
            depth = r_u32(scratch + 24);
            threshold = r_u32(scratch + 28);
            if ((sint32)depth < (sint32)threshold || r_s32(scratch + 36) < 0)
            {
                for (index = 0; index < 3; ++index)
                {
                    vertex = vertices + (uint32)r_u16(primitive + 10 + index * 4) * 8;
                    gs_subdivision_copy_vector(scratch + 84 + index * 12, vertex);
                    w_u32(scratch + 92 + index * 12, xport_load_le32(work + 1 + index * 2));
                }
                w_u32(scratch + 20, gpu_packet_link((uintptr_t)work));
                gs_subdivision_recurse_g3(scratch + 84, scratch, 0, &work);
            }
            else
                gs_subdivision_link_g3(&work, scratch, slot);
        }
        primitive += 20;
        --count;
    }
    w_u32(scratch + 20, gpu_packet_link((uintptr_t)work));
    return work;
}

uint32 *GsA4divG3L(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    return gs_a4_div_g3_lit(primitive, vertices, normals, work, count, shift, ordering_table, scratch, 0);
}

uint32 *GsA4divG3LFG(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    return gs_a4_div_g3_lit(primitive, vertices, normals, work, count, shift, ordering_table, scratch, 1);
}

static uint32 gs_subdivision_f4_bounds(uint32 packet, uint32 scratch)
{
    uint32 offset, index;
    sint32 first, second, coordinate, bound, extent;
    for (offset = 0; offset < 4; offset += 2)
    {
        first = r_s16(packet + 8 + offset);
        second = r_s16(packet + 12 + offset);
        if (second < first)
        {
            w_u16(scratch + 48 + offset, (uint32)first);
            w_u16(scratch + 44 + offset, r_u16(packet + 12 + offset));
        }
        else
        {
            w_u16(scratch + 44 + offset, (uint32)first);
            w_u16(scratch + 48 + offset, r_u16(packet + 12 + offset));
        }
        for (index = 2; index < 4; ++index)
        {
            coordinate = r_s16(packet + 8 + index * 4 + offset);
            bound = r_s16(scratch + 48 + offset);
            if (bound < coordinate)
                w_u16(scratch + 48 + offset, (uint32)coordinate);
            else
            {
                bound = r_s16(scratch + 44 + offset);
                if (coordinate < bound)
                    w_u16(scratch + 44 + offset, (uint32)coordinate);
            }
        }
        extent = r_s16(scratch + 52 + offset);
        bound = r_s16(scratch + 48 + offset);
        if (bound < -extent)
            return 1;
        bound = r_s16(scratch + 44 + offset);
        if (extent < bound)
            return 1;
    }
    return 0;
}

static uint32 gs_subdivision_f4_slot(uint32 scratch)
{
    uint32 depth = r_u32(scratch + 24);
    uint32 shift = r_u32(scratch + 12);
    uint32 slot = (uint32)((sint32)depth >> (shift & 31));
    slot = (slot << 2) + r_u32(scratch + 16);
    w_u32(scratch + 56, slot);
    return slot;
}

static void gs_subdivision_f4_link(uint32 **cursor, uint32 scratch, uint32 slot, uint32 words)
{
    uint32 *work = gs_tmd_work_buffer(*cursor, words);
    xport_store_le32(work, (r_u32(slot) & 0xFFFFFF) | ((words - 1) << 24));
    w_u32(r_u32(scratch + 56), gpu_packet_link((uintptr_t)work));
    *cursor = work + words;
}

static void gs_subdivision_f4_emit_triangle(uint32 first, uint32 second, uint32 third, uint32 scratch, uint32 **cursor)
{
    uint32 *work, index, slot;
    gs_tmd_load_vector(first, 0);
    gs_tmd_load_vector(second, 1);
    gs_tmd_load_vector(third, 2);
    xport_gte_execute(0x280030);
    work = gs_tmd_work_buffer(*cursor, 5);
    xport_store_le32(work + 1, r_u32(scratch + 64));
    ((uint8 *)work)[3] = 4;
    ((uint8 *)work)[7] = 0x20;
    w_u32(scratch + 40, xport_gte_read_flag());
    if (r_s32(scratch + 40) >= 0)
    {
        xport_gte_execute(0x158002D);
        for (index = 0; index < 3; ++index)
            xport_store_le32(work + 2 + index, xport_gte_read_data(12 + index));
        w_u32(scratch + 24, xport_gte_read_data(7));
        slot = gs_subdivision_f4_slot(scratch);
        gs_subdivision_f4_link(cursor, scratch, slot, 5);
        w_u32(scratch + 20, gpu_packet_link((uintptr_t)*cursor));
    }
}

static void gs_subdivision_recurse_f4(uint32 vertices, uint32 scratch, uint32 level, uint32 **cursor)
{
    uint32 next = vertices + 32, index, flags, last_flags, emit = 0, slot;
    sint32 maximum, minimum, limit;
    for (index = 0; index < 3; ++index)
        gs_tmd_load_vector(vertices + index * 8, index);
    xport_gte_execute(0x280030);
    w_u32(scratch + 40, xport_gte_read_flag());
    for (index = 0; index < 3; ++index)
        w_u32(scratch + 68 + index * 4, xport_gte_read_data(12 + index));
    gs_tmd_load_vector(vertices + 24, 0);
    xport_gte_execute(0x180001);
    w_u32(scratch + 36, xport_gte_read_flag());
    w_u32(scratch + 80, xport_gte_read_data(14));
    xport_gte_execute(0x168002E);
    flags = r_u32(scratch + 40);
    last_flags = r_u32(scratch + 36);
    w_u32(scratch + 40, flags | last_flags);
    w_u32(scratch + 24, (uint32)((sint32)xport_gte_read_data(19) >> 2));
    if (gs_subdivision_f4_bounds(scratch + 60, scratch) != 0)
        return;
    if (r_u32(scratch) == level)
    {
        if (r_s32(scratch + 40) < 0)
            return;
        emit = 1;
    }
    else if (r_s32(scratch + 40) >= 0)
    {
        maximum = r_s16(scratch + 48);
        minimum = r_s16(scratch + 44);
        limit = r_s16(scratch + 32);
        if (maximum - minimum < limit)
        {
            maximum = r_s16(scratch + 50);
            minimum = r_s16(scratch + 46);
            limit = r_s16(scratch + 34);
            if (maximum - minimum < limit)
                emit = 1;
        }
    }
    if (emit)
    {
        uint32 *work = gs_tmd_work_buffer(*cursor, 6);
        for (index = 0; index < 6; ++index)
            xport_store_le32(work + index, r_u32(scratch + 60 + index * 4));
        slot = gs_subdivision_f4_slot(scratch);
        gs_subdivision_f4_link(cursor, scratch, slot, 6);
        w_u32(scratch + 20, gpu_packet_link((uintptr_t)*cursor));
        return;
    }
    ++level;
    gs_subdivision_copy_vector(next, vertices);
    gs_subdivision_midpoint(next + 8, vertices, vertices + 8);
    gs_subdivision_midpoint(next + 16, vertices, vertices + 16);
    gs_subdivision_midpoint(next + 24, vertices, vertices + 24);
    gs_subdivision_f4_emit_triangle(vertices, vertices + 8, next + 8, scratch, cursor);
    gs_subdivision_recurse_f4(next, scratch, level, cursor);
    gs_subdivision_copy_vector(next, vertices + 8);
    gs_subdivision_midpoint(next + 16, vertices + 8, vertices + 24);
    gs_subdivision_f4_emit_triangle(vertices + 8, vertices + 24, next + 16, scratch, cursor);
    gs_subdivision_recurse_f4(next, scratch, level, cursor);
    gs_subdivision_copy_vector(next, vertices + 24);
    gs_subdivision_midpoint(next + 8, vertices + 24, vertices + 16);
    gs_subdivision_f4_emit_triangle(vertices + 24, vertices + 16, next + 8, scratch, cursor);
    gs_subdivision_recurse_f4(next, scratch, level, cursor);
    gs_subdivision_copy_vector(next, vertices + 16);
    gs_subdivision_midpoint(next + 16, vertices, vertices + 16);
    gs_subdivision_f4_emit_triangle(vertices + 16, vertices, next + 16, scratch, cursor);
    gs_subdivision_recurse_f4(next, scratch, level, cursor);
}

static uint32 *gs_a4_div_f4(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch, uint32 mode)
{
    uint32 base, index, addresses[3], slot, depth, flags, threshold, subdivide, vertex;
    uint32 index_offset = mode == 3 ? 8 : 10;
    uint32 *packet;
    base = r_u32(ordering_table + 4);
    if (mode == 2)
    {
        w_u32(scratch + 12, shift);
        w_u16(scratch + 32, (uint32)gs_subdivision_width);
        w_u16(scratch + 34, (uint32)gs_subdivision_height);
        w_u32(scratch + 20, gpu_packet_link((uintptr_t)work));
        w_u32(scratch + 28, (uint32)gs_subdivision_distance);
    }
    else
    {
        w_u32(scratch + 20, gpu_packet_link((uintptr_t)work));
        w_u32(scratch + 28, (uint32)gs_subdivision_distance);
        if (mode == 3)
        {
            w_u16(scratch + 34, (uint32)gs_subdivision_height);
            w_u32(scratch + 12, shift);
            w_u16(scratch + 32, (uint32)gs_subdivision_width);
        }
        else
        {
            w_u16(scratch + 32, (uint32)gs_subdivision_width);
            w_u32(scratch + 12, shift);
            w_u16(scratch + 34, (uint32)gs_subdivision_height);
        }
    }
    w_u16(scratch + 52, (uint32)(gs_screen_width / 2));
    if (mode == 3)
        w_u32(scratch + 16, base);
    w_u16(scratch + 54, (uint32)(gs_screen_height / 2));
    w_u8(scratch + 63, 5);
    w_u8(scratch + 67, 0x28);
    if (mode != 3)
        w_u32(scratch + 16, base);
    if (mode == 2)
        xport_gte_write_data(6, r_u32(primitive + 4));
    while ((sint32)count > 0)
    {
        if (mode == 1)
            xport_gte_write_data(6, r_u32(primitive + 4));
        for (index = 0; index < 3; ++index)
            addresses[index] = vertices + (uint32)r_u16(primitive + index_offset + index * 2) * 8;
        for (index = 0; index < 3; ++index)
            gs_tmd_load_vector(addresses[index], index);
        xport_gte_execute(0x280030);
        w_u32(scratch + 36, xport_gte_read_flag());
        xport_gte_execute(0x1400006);
        w_u32(scratch + 24, xport_gte_read_data(24));
        if (r_s32(scratch + 24) > 0)
        {
            if (mode == 1 || mode == 2)
            {
                vertex = normals + (uint32)r_u16(primitive + 8) * 8;
                gs_tmd_load_vector(vertex, 0);
                xport_gte_execute(mode == 2 ? 0xE80413 : 0x108041B);
            }
            work = gs_tmd_work_buffer(work, 6);
            for (index = 0; index < 3; ++index)
                xport_store_le32(work + 2 + index, xport_gte_read_data(12 + index));
            vertex = vertices + (uint32)r_u16(primitive + index_offset + 6) * 8;
            gs_tmd_load_vector(vertex, 0);
            xport_gte_execute(0x180001);
            w_u32(scratch + 40, xport_gte_read_flag());
            flags = r_u32(scratch + 40) & 0x40000;
            if (flags == 0 || (r_u32(scratch + 36) & 0x40000) == 0)
            {
                xport_store_le32(work + 5, xport_gte_read_data(14));
                xport_gte_execute(0x168002E);
                if (mode == 0 || mode == 3)
                    xport_store_le32(work + 1, r_u32(primitive + 4));
                w_u32(scratch + 24, xport_gte_read_data(7));
                depth = r_u32(scratch + 24);
                flags = r_u32(scratch + 12);
                base = r_u32(scratch + 16);
                threshold = r_u32(scratch + 28);
                slot = (uint32)((sint32)depth >> (flags & 31)) << 2;
                depth = r_u32(scratch + 24);
                slot += base;
                subdivide = (sint32)depth < (sint32)threshold;
                w_u32(scratch + 56, slot);
                if (subdivide || r_s32(scratch + 36) < 0 || (r_u32(scratch + 40) & 0x80000000) != 0)
                {
                    if (mode == 1 || mode == 2)
                        w_u32(scratch + 64, xport_gte_read_data(22));
                    else
                        w_u32(scratch + 64, r_u32(primitive + 4));
                    for (index = 0; index < 4; ++index)
                    {
                        vertex = vertices + (uint32)r_u16(primitive + index_offset + index * 2) * 8;
                        gs_subdivision_copy_vector(scratch + 84 + index * 8, vertex);
                    }
                    w_u32(scratch + 20, gpu_packet_link((uintptr_t)work));
                    gs_subdivision_recurse_f4(scratch + 84, scratch, 0, &work);
                }
                else
                {
                    packet = work;
                    gs_subdivision_f4_link(&work, scratch, slot, 6);
                    if (mode == 1 || mode == 2)
                        xport_store_le32(packet + 1, xport_gte_read_data(22));
                }
            }
        }
        primitive += mode == 3 ? 16 : 20;
        --count;
    }
    w_u32(scratch + 20, gpu_packet_link((uintptr_t)work));
    return work;
}

uint32 *GsA4divF4NL(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    (void)normals;
    return gs_a4_div_f4(primitive, vertices, 0, work, count, shift, ordering_table, scratch, 0);
}

uint32 *GsA4divF4L(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    return gs_a4_div_f4(primitive, vertices, normals, work, count, shift, ordering_table, scratch, 1);
}

uint32 *GsA4divF4LFG(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    return gs_a4_div_f4(primitive, vertices, normals, work, count, shift, ordering_table, scratch, 2);
}

uint32 *GsA4divNF4(uint32 primitive, uint32 vertices, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    return gs_a4_div_f4(primitive, vertices, 0, work, count, shift, ordering_table, scratch, 3);
}

static uint32 gs_subdivision_g4_bounds(uint32 packet, uint32 scratch)
{
    uint32 offset, index;
    sint32 first, second, coordinate, bound, extent;
    for (offset = 0; offset < 4; offset += 2)
    {
        first = r_s16(packet + 8 + offset);
        second = r_s16(packet + 16 + offset);
        if (second < first)
        {
            w_u16(scratch + 48 + offset, (uint32)first);
            w_u16(scratch + 44 + offset, r_u16(packet + 16 + offset));
        }
        else
        {
            w_u16(scratch + 44 + offset, (uint32)first);
            w_u16(scratch + 48 + offset, r_u16(packet + 16 + offset));
        }
        for (index = 2; index < 4; ++index)
        {
            coordinate = r_s16(packet + 8 + index * 8 + offset);
            bound = r_s16(scratch + 48 + offset);
            if (bound < coordinate)
                w_u16(scratch + 48 + offset, (uint32)coordinate);
            else
            {
                bound = r_s16(scratch + 44 + offset);
                if (coordinate < bound)
                    w_u16(scratch + 44 + offset, (uint32)coordinate);
            }
        }
        extent = r_s16(scratch + 52 + offset);
        bound = r_s16(scratch + 48 + offset);
        if (bound < -extent)
            return 1;
        bound = r_s16(scratch + 44 + offset);
        if (extent < bound)
            return 1;
    }
    return 0;
}

static void gs_subdivision_g4_emit_triangle(uint32 first, uint32 second, uint32 third, uint32 scratch, uint32 **cursor)
{
    uint32 *work, index, slot;
    gs_tmd_load_vector(first, 0);
    gs_tmd_load_vector(second, 1);
    gs_tmd_load_vector(third, 2);
    xport_gte_execute(0x280030);
    work = gs_tmd_work_buffer(*cursor, 7);
    xport_store_le32(work + 1, r_u32(first + 8));
    ((uint8 *)work)[3] = 6;
    ((uint8 *)work)[7] = 0x30;
    w_u32(scratch + 40, xport_gte_read_flag());
    if ((r_u32(scratch + 40) & 0x60000) == 0)
    {
        xport_gte_execute(0x158002D);
        for (index = 0; index < 3; ++index)
            xport_store_le32(work + 2 + index * 2, xport_gte_read_data(12 + index));
        w_u32(scratch + 24, xport_gte_read_data(7));
        xport_store_le32(work + 3, r_u32(second + 8));
        xport_store_le32(work + 5, r_u32(third + 8));
        slot = gs_subdivision_f4_slot(scratch);
        gs_subdivision_f4_link(cursor, scratch, slot, 7);
        w_u32(scratch + 20, gpu_packet_link((uintptr_t)*cursor));
    }
}

static void gs_subdivision_recurse_g4(uint32 vertices, uint32 scratch, uint32 level, uint32 **cursor)
{
    uint32 next = vertices + 48, index, flags, last_flags, emit = 0, slot;
    sint32 maximum, minimum, limit;
    for (index = 0; index < 3; ++index)
        gs_tmd_load_vector(vertices + index * 12, index);
    xport_gte_execute(0x280030);
    w_u32(scratch + 40, xport_gte_read_flag());
    for (index = 0; index < 3; ++index)
        w_u32(scratch + 68 + index * 8, xport_gte_read_data(12 + index));
    gs_tmd_load_vector(vertices + 36, 0);
    xport_gte_execute(0x180001);
    w_u32(scratch + 36, xport_gte_read_flag());
    w_u32(scratch + 92, xport_gte_read_data(14));
    xport_gte_execute(0x168002E);
    flags = r_u32(scratch + 40);
    last_flags = r_u32(scratch + 36);
    w_u32(scratch + 40, flags | last_flags);
    w_u32(scratch + 24, (uint32)((sint32)xport_gte_read_data(19) >> 2));
    if (gs_subdivision_g4_bounds(scratch + 60, scratch) != 0)
        return;
    if (r_u32(scratch) == level)
    {
        if (r_s32(scratch + 40) < 0)
            return;
        emit = 1;
    }
    else if (r_s32(scratch + 40) >= 0)
    {
        maximum = r_s16(scratch + 48);
        minimum = r_s16(scratch + 44);
        limit = r_s16(scratch + 32);
        if (maximum - minimum < limit)
        {
            maximum = r_s16(scratch + 50);
            minimum = r_s16(scratch + 46);
            limit = r_s16(scratch + 34);
            if (maximum - minimum < limit)
                emit = 1;
        }
    }
    if (emit)
    {
        uint32 *work = gs_tmd_work_buffer(*cursor, 9);
        xport_store_le32(work, r_u32(scratch + 60));
        for (index = 0; index < 4; ++index)
        {
            xport_store_le32(work + 1 + index * 2, r_u32(vertices + 8 + index * 12));
            xport_store_le32(work + 2 + index * 2, r_u32(scratch + 68 + index * 8));
        }
        slot = gs_subdivision_f4_slot(scratch);
        gs_subdivision_f4_link(cursor, scratch, slot, 9);
        w_u32(scratch + 20, gpu_packet_link((uintptr_t)*cursor));
        return;
    }
    ++level;
    gs_subdivision_copy_color_vertex(next, vertices);
    gs_subdivision_midpoint_color(next + 12, vertices, vertices + 12);
    gs_subdivision_midpoint_color(next + 24, vertices, vertices + 24);
    gs_subdivision_midpoint_color(next + 36, vertices, vertices + 36);
    gs_subdivision_g4_emit_triangle(vertices, vertices + 12, next + 12, scratch, cursor);
    gs_subdivision_recurse_g4(next, scratch, level, cursor);
    gs_subdivision_copy_color_vertex(next, vertices + 12);
    gs_subdivision_midpoint_color(next + 24, vertices + 12, vertices + 36);
    gs_subdivision_g4_emit_triangle(vertices + 12, vertices + 36, next + 24, scratch, cursor);
    gs_subdivision_recurse_g4(next, scratch, level, cursor);
    gs_subdivision_copy_color_vertex(next, vertices + 36);
    gs_subdivision_midpoint_color(next + 12, vertices + 36, vertices + 24);
    gs_subdivision_g4_emit_triangle(vertices + 36, vertices + 24, next + 12, scratch, cursor);
    gs_subdivision_recurse_g4(next, scratch, level, cursor);
    gs_subdivision_copy_color_vertex(next, vertices + 24);
    gs_subdivision_midpoint_color(next + 24, vertices, vertices + 24);
    gs_subdivision_g4_emit_triangle(vertices + 24, vertices, next + 24, scratch, cursor);
    gs_subdivision_recurse_g4(next, scratch, level, cursor);
}

static uint32 *gs_a4_div_g4_unlit(uint32 primitive, uint32 vertices, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch, uint32 multicolor)
{
    uint32 base, index, addresses[3], vertex, slot, depth, flags, threshold, color;
    uint32 index_offset = multicolor ? 20 : 10;
    uint32 index_stride = multicolor ? 2 : 4;
    w_u16(scratch + 52, (uint32)(gs_screen_width / 2));
    w_u16(scratch + 54, (uint32)(gs_screen_height / 2));
    base = r_u32(ordering_table + 4);
    w_u32(scratch + 20, gpu_packet_link((uintptr_t)work));
    w_u32(scratch + 28, (uint32)gs_subdivision_distance);
    w_u16(scratch + 32, (uint32)gs_subdivision_width);
    if (!multicolor)
        w_u16(scratch + 34, (uint32)gs_subdivision_height);
    w_u32(scratch + 12, shift);
    w_u8(scratch + 63, 8);
    if (multicolor)
        w_u16(scratch + 34, (uint32)gs_subdivision_height);
    w_u8(scratch + 67, 0x38);
    w_u32(scratch + 16, base);
    while ((sint32)count > 0)
    {
        for (index = 0; index < 3; ++index)
            addresses[index] = vertices + (uint32)r_u16(primitive + index_offset + index * index_stride) * 8;
        for (index = 0; index < 3; ++index)
            gs_tmd_load_vector(addresses[index], index);
        xport_gte_execute(0x280030);
        if (!multicolor)
        {
            work = gs_tmd_work_buffer(work, 9);
            xport_store_le32(work + 1, r_u32(primitive + 4));
            xport_store_le32(work + 3, r_u32(primitive + 4));
        }
        w_u32(scratch + 36, xport_gte_read_flag());
        xport_gte_execute(0x1400006);
        if (!multicolor)
            xport_store_le32(work + 5, r_u32(primitive + 4));
        w_u32(scratch + 24, xport_gte_read_data(24));
        if (r_s32(scratch + 24) > 0)
        {
            work = gs_tmd_work_buffer(work, 9);
            for (index = 0; index < 3; ++index)
                xport_store_le32(work + 2 + index * 2, xport_gte_read_data(12 + index));
            vertex = vertices + (uint32)r_u16(primitive + index_offset + 3 * index_stride) * 8;
            gs_tmd_load_vector(vertex, 0);
            xport_gte_execute(0x180001);
            if (multicolor)
                xport_store_le32(work + 1, r_u32(primitive + 4) | 0x38000000);
            w_u32(scratch + 40, xport_gte_read_flag());
            flags = r_u32(scratch + 40) & 0x40000;
            if (flags == 0 || (r_u32(scratch + 36) & 0x40000) == 0)
            {
                xport_store_le32(work + 8, xport_gte_read_data(14));
                xport_gte_execute(0x168002E);
                if (multicolor)
                {
                    for (index = 1; index < 4; ++index)
                        xport_store_le32(work + 1 + index * 2, r_u32(primitive + 4 + index * 4));
                }
                else
                    xport_store_le32(work + 7, r_u32(primitive + 4));
                w_u32(scratch + 24, xport_gte_read_data(7));
                depth = r_u32(scratch + 24);
                flags = r_u32(scratch + 12);
                base = r_u32(scratch + 16);
                threshold = r_u32(scratch + 28);
                slot = (uint32)((sint32)depth >> (flags & 31)) << 2;
                depth = r_u32(scratch + 24);
                slot += base;
                w_u32(scratch + 56, slot);
                if ((sint32)depth < (sint32)threshold || r_s32(scratch + 36) < 0 || (r_u32(scratch + 40) & 0x80000000) != 0)
                {
                    if (multicolor)
                    {
                        for (index = 0; index < 4; ++index)
                            w_u32(scratch + 104 + index * 12, r_u32(primitive + 4 + index * 4) | 0x38000000);
                    }
                    else
                    {
                        vertex = vertices + (uint32)r_u16(primitive + 10) * 8;
                        gs_subdivision_copy_vector(scratch + 96, vertex);
                        color = r_u32(primitive + 4);
                        for (index = 0; index < 4; ++index)
                            w_u32(scratch + 104 + index * 12, color);
                    }
                    for (index = multicolor ? 0 : 1; index < 4; ++index)
                    {
                        vertex = vertices + (uint32)r_u16(primitive + index_offset + index * index_stride) * 8;
                        gs_subdivision_copy_vector(scratch + 96 + index * 12, vertex);
                    }
                    w_u32(scratch + 20, gpu_packet_link((uintptr_t)work));
                    gs_subdivision_recurse_g4(scratch + 96, scratch, 0, &work);
                }
                else
                    gs_subdivision_f4_link(&work, scratch, slot, 9);
            }
        }
        primitive += multicolor ? 28 : 24;
        --count;
    }
    w_u32(scratch + 20, gpu_packet_link((uintptr_t)work));
    return work;
}

uint32 *GsA4divG4NL(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    (void)normals;
    return gs_a4_div_g4_unlit(primitive, vertices, work, count, shift, ordering_table, scratch, 0);
}

uint32 *GsA4divNG4(uint32 primitive, uint32 vertices, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    return gs_a4_div_g4_unlit(primitive, vertices, work, count, shift, ordering_table, scratch, 1);
}

static uint32 *gs_a4_div_g4_lit(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch, uint32 fog)
{
    uint32 base, index, addresses[3], vertex, slot, depth, threshold;
    w_u16(scratch + 52, (uint32)(gs_screen_width / 2));
    w_u16(scratch + 54, (uint32)(gs_screen_height / 2));
    base = r_u32(ordering_table + 4);
    if (fog)
    {
        w_u32(scratch + 12, shift);
        w_u16(scratch + 32, (uint32)gs_subdivision_width);
        w_u16(scratch + 34, (uint32)gs_subdivision_height);
        w_u32(scratch + 20, gpu_packet_link((uintptr_t)work));
        w_u32(scratch + 28, (uint32)gs_subdivision_distance);
    }
    else
    {
        w_u32(scratch + 20, gpu_packet_link((uintptr_t)work));
        w_u32(scratch + 28, (uint32)gs_subdivision_distance);
        w_u16(scratch + 32, (uint32)gs_subdivision_width);
        w_u16(scratch + 34, (uint32)gs_subdivision_height);
        w_u32(scratch + 12, shift);
    }
    w_u8(scratch + 63, 8);
    w_u8(scratch + 67, 0x38);
    w_u32(scratch + 16, base);
    if (fog)
        xport_gte_write_data(6, r_u32(primitive + 4));
    while ((sint32)count > 0)
    {
        if (!fog)
            xport_gte_write_data(6, r_u32(primitive + 4));
        for (index = 0; index < 3; ++index)
            addresses[index] = vertices + (uint32)r_u16(primitive + 10 + index * 4) * 8;
        for (index = 0; index < 3; ++index)
            gs_tmd_load_vector(addresses[index], index);
        xport_gte_execute(0x280030);
        w_u32(scratch + 36, xport_gte_read_flag());
        xport_gte_execute(0x1400006);
        w_u32(scratch + 24, xport_gte_read_data(24));
        if (r_s32(scratch + 24) > 0)
        {
            work = gs_tmd_work_buffer(work, 9);
            for (index = 0; index < 3; ++index)
                xport_store_le32(work + 2 + index * 2, xport_gte_read_data(12 + index));
            vertex = vertices + (uint32)r_u16(primitive + 22) * 8;
            gs_tmd_load_vector(vertex, 0);
            xport_gte_execute(0x180001);
            w_u32(scratch + 40, xport_gte_read_flag());
            if ((r_u32(scratch + 40) & 0x40000) == 0 || (r_u32(scratch + 36) & 0x40000) == 0)
            {
                xport_store_le32(work + 8, xport_gte_read_data(14));
                xport_gte_execute(0x168002E);
                w_u32(scratch + 24, xport_gte_read_data(7));
                for (index = 0; index < 3; ++index)
                    addresses[index] = normals + (uint32)r_u16(primitive + 8 + index * 4) * 8;
                for (index = 0; index < 3; ++index)
                    gs_tmd_load_vector(addresses[index], index);
                xport_gte_execute(fog ? 0xF80416 : 0x118043F);
                slot = gs_subdivision_f4_slot(scratch);
                for (index = 0; index < 3; ++index)
                    xport_store_le32(work + 1 + index * 2, xport_gte_read_data(20 + index));
                vertex = normals + (uint32)r_u16(primitive + 20) * 8;
                gs_tmd_load_vector(vertex, 0);
                xport_gte_execute(fog ? 0xE80413 : 0x108041B);
                xport_store_le32(work + 7, xport_gte_read_data(22));
                depth = r_u32(scratch + 24);
                threshold = r_u32(scratch + 28);
                if ((sint32)depth < (sint32)threshold || r_s32(scratch + 36) < 0 || (r_u32(scratch + 40) & 0x80000000) != 0)
                {
                    for (index = 0; index < 4; ++index)
                    {
                        vertex = vertices + (uint32)r_u16(primitive + 10 + index * 4) * 8;
                        gs_subdivision_copy_vector(scratch + 96 + index * 12, vertex);
                        w_u32(scratch + 104 + index * 12, xport_load_le32(work + 1 + index * 2));
                    }
                    w_u32(scratch + 20, gpu_packet_link((uintptr_t)work));
                    gs_subdivision_recurse_g4(scratch + 96, scratch, 0, &work);
                }
                else
                    gs_subdivision_f4_link(&work, scratch, slot, 9);
            }
        }
        primitive += 24;
        --count;
    }
    w_u32(scratch + 20, gpu_packet_link((uintptr_t)work));
    return work;
}

uint32 *GsA4divG4L(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    return gs_a4_div_g4_lit(primitive, vertices, normals, work, count, shift, ordering_table, scratch, 0);
}

uint32 *GsA4divG4LFG(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    return gs_a4_div_g4_lit(primitive, vertices, normals, work, count, shift, ordering_table, scratch, 1);
}

static void gs_subdivision_link_tf3(uint32 **cursor, uint32 scratch, uint32 slot)
{
    uint32 *work = gs_tmd_work_buffer(*cursor, 8);
    xport_store_le32(work, (r_u32(slot) & 0xFFFFFF) | 0x07000000);
    w_u32(r_u32(scratch + 52), gpu_packet_link((uintptr_t)work));
    *cursor = work + 8;
}

static void gs_subdivision_midpoint_uv(uint32 destination, uint32 left, uint32 right)
{
    uint32 offset, first, second;
    gs_subdivision_midpoint(destination, left, right);
    for (offset = 6; offset < 8; ++offset)
    {
        first = r_u8(left + offset);
        second = r_u8(right + offset);
        w_u8(destination + offset, (first + second) >> 1);
    }
}

static void gs_subdivision_emit_tf3(uint32 first, uint32 second, uint32 third, uint32 scratch, uint32 **cursor)
{
    uint32 *work, index, slot;
    gs_tmd_load_vector(first, 0);
    gs_tmd_load_vector(second, 1);
    gs_tmd_load_vector(third, 2);
    xport_gte_execute(0x280030);
    work = gs_tmd_work_buffer(*cursor, 8);
    xport_store_le32(work + 1, r_u32(scratch + 60));
    ((uint8 *)work)[3] = 7;
    ((uint8 *)work)[7] = 0x24;
    w_u32(scratch + 36, xport_gte_read_flag());
    if (r_s32(scratch + 36) >= 0)
    {
        xport_gte_execute(0x158002D);
        for (index = 0; index < 3; ++index)
            xport_store_le32(work + 2 + index * 2, xport_gte_read_data(12 + index));
        w_u32(scratch + 24, xport_gte_read_data(7));
        xport_store_le32(work + 3, r_u16(first + 6));
        xport_store_le32(work + 5, r_u16(second + 6));
        xport_store_le32(work + 7, r_u16(third + 6));
        xport_store_le16((uint8 *)work + 14, r_u16(scratch + 70));
        xport_store_le16((uint8 *)work + 22, r_u16(scratch + 78));
        slot = gs_subdivision_slot(scratch);
        gs_subdivision_link_tf3(cursor, scratch, slot);
        w_u32(scratch + 20, gpu_packet_link((uintptr_t)*cursor));
    }
}

static void gs_subdivision_recurse_tf3(uint32 vertices, uint32 scratch, uint32 level, uint32 **cursor)
{
    uint32 next = vertices + 24, index, emit = 0, slot;
    sint32 maximum, minimum, limit;
    for (index = 0; index < 3; ++index)
        gs_tmd_load_vector(vertices + index * 8, index);
    xport_gte_execute(0x280030);
    for (index = 0; index < 3; ++index)
        w_u16(scratch + 68 + index * 8, r_u16(vertices + 6 + index * 8));
    w_u32(scratch + 36, xport_gte_read_flag());
    for (index = 0; index < 3; ++index)
        w_u32(scratch + 64 + index * 8, xport_gte_read_data(12 + index));
    xport_gte_execute(0x158002D);
    w_u32(scratch + 24, (uint32)((sint32)xport_gte_read_data(19) >> 2));
    if (gs_subdivision_g3_bounds(scratch + 56, scratch) != 0)
        return;
    if (r_u32(scratch) == level)
    {
        if (r_s32(scratch + 36) < 0)
            return;
        emit = 1;
    }
    else if (r_s32(scratch + 36) >= 0)
    {
        maximum = r_s16(scratch + 44);
        minimum = r_s16(scratch + 40);
        limit = r_s16(scratch + 32);
        if (maximum - minimum < limit)
        {
            maximum = r_s16(scratch + 46);
            minimum = r_s16(scratch + 42);
            limit = r_s16(scratch + 34);
            if (maximum - minimum < limit)
                emit = 1;
        }
    }
    if (emit)
    {
        uint32 *work = gs_tmd_work_buffer(*cursor, 8);
        for (index = 0; index < 8; ++index)
            xport_store_le32(work + index, r_u32(scratch + 56 + index * 4));
        slot = gs_subdivision_slot(scratch);
        gs_subdivision_link_tf3(cursor, scratch, slot);
        w_u32(scratch + 20, gpu_packet_link((uintptr_t)*cursor));
        return;
    }
    ++level;
    gs_subdivision_copy_vector(next, vertices);
    gs_subdivision_midpoint_uv(next + 8, vertices, vertices + 8);
    gs_subdivision_midpoint_uv(next + 16, vertices, vertices + 16);
    gs_subdivision_recurse_tf3(next, scratch, level, cursor);
    gs_subdivision_emit_tf3(vertices, vertices + 8, next + 8, scratch, cursor);
    gs_subdivision_midpoint_uv(next, vertices + 8, vertices + 16);
    gs_subdivision_recurse_tf3(next, scratch, level, cursor);
    gs_subdivision_copy_vector(next + 16, vertices + 8);
    gs_subdivision_recurse_tf3(next, scratch, level, cursor);
    gs_subdivision_emit_tf3(vertices + 8, vertices + 16, next, scratch, cursor);
    gs_subdivision_copy_vector(next + 8, vertices + 16);
    gs_subdivision_midpoint_uv(next + 16, vertices, vertices + 16);
    gs_subdivision_recurse_tf3(next, scratch, level, cursor);
    gs_subdivision_emit_tf3(vertices + 16, vertices, next + 16, scratch, cursor);
}

static uint32 *gs_a4_div_tf3(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch, uint32 mode)
{
    uint32 base, index, addresses[3], vertex, slot, depth, threshold;
    uint32 colored = mode == 1;
    uint32 lit = mode >= 2;
    uint32 index_offset = colored ? 20 : 18;
    w_u16(scratch + 48, (uint32)(gs_screen_width / 2));
    w_u16(scratch + 50, (uint32)(gs_screen_height / 2));
    base = r_u32(ordering_table + 4);
    w_u32(scratch + 12, mode == 3 ? shift + 2 : shift);
    w_u16(scratch + 32, (uint32)gs_subdivision_width);
    w_u16(scratch + 34, (uint32)gs_subdivision_height);
    w_u32(scratch + 20, gpu_packet_link((uintptr_t)work));
    w_u32(scratch + 28, (uint32)gs_subdivision_distance);
    w_u8(scratch + 59, 7);
    w_u8(scratch + 63, 0x24);
    if (!colored)
    {
        w_u8(scratch + 60, 0x80);
        w_u8(scratch + 61, 0x80);
        w_u8(scratch + 62, 0x80);
    }
    w_u32(scratch + 16, base);
    xport_gte_write_data(6, r_u32(scratch + 60));
    while ((sint32)count > 0)
    {
        for (index = 0; index < 3; ++index)
            addresses[index] = vertices + (uint32)r_u16(primitive + index_offset + index * 2) * 8;
        for (index = 0; index < 3; ++index)
            gs_tmd_load_vector(addresses[index], index);
        xport_gte_execute(0x280030);
        work = gs_tmd_work_buffer(work, 8);
        xport_store_le32(work + 3, r_u32(primitive + 4));
        xport_store_le32(work + 7, r_u32(primitive + 12));
        w_u32(scratch + 36, xport_gte_read_flag());
        xport_gte_execute(0x1400006);
        w_u32(scratch + 24, xport_gte_read_data(24));
        if (r_s32(scratch + 24) > 0 && (r_u32(scratch + 36) & 0x40000) == 0)
        {
            for (index = 0; index < 3; ++index)
                xport_store_le32(work + 2 + index * 2, xport_gte_read_data(12 + index));
            xport_gte_execute(0x158002D);
            if (colored)
                xport_store_le32(work + 1, r_u32(primitive + 16) | 0x24000000);
            w_u32(scratch + 24, xport_gte_read_data(7));
            if (lit)
            {
                vertex = normals + (uint32)r_u16(primitive + 16) * 8;
                gs_tmd_load_vector(vertex, 0);
                xport_gte_execute(mode == 3 ? 0xE80413 : 0x108041B);
            }
            xport_store_le32(work + 5, r_u32(primitive + 8));
            slot = gs_subdivision_slot(scratch);
            if (!colored)
                xport_store_le32(work + 1, lit ? xport_gte_read_data(22) : 0x24808080);
            depth = r_u32(scratch + 24);
            threshold = r_u32(scratch + 28);
            if ((sint32)depth < (sint32)threshold || r_s32(scratch + 36) < 0)
            {
                if (lit)
                    w_u32(scratch + 60, xport_gte_read_data(22));
                else if (colored)
                    w_u32(scratch + 60, xport_load_le32(work + 1));
                for (index = 0; index < 3; ++index)
                {
                    vertex = vertices + (uint32)r_u16(primitive + index_offset + index * 2) * 8;
                    gs_subdivision_copy_vector(scratch + 88 + index * 8, vertex);
                }
                for (index = 0; index < 3; ++index)
                    w_u16(scratch + 94 + index * 8, r_u16(primitive + 4 + index * 4));
                w_u16(scratch + 70, r_u16(primitive + 6));
                w_u32(scratch + 20, gpu_packet_link((uintptr_t)work));
                w_u16(scratch + 78, r_u16(primitive + 10));
                gs_subdivision_recurse_tf3(scratch + 88, scratch, 0, &work);
            }
            else
                gs_subdivision_link_tf3(&work, scratch, slot);
        }
        primitive += colored ? 28 : 24;
        --count;
    }
    w_u32(scratch + 20, gpu_packet_link((uintptr_t)work));
    return work;
}

uint32 *GsA4divTF3NL(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    (void)normals;
    return gs_a4_div_tf3(primitive, vertices, 0, work, count, shift, ordering_table, scratch, 0);
}

uint32 *GsA4divTNF3(uint32 primitive, uint32 vertices, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    return gs_a4_div_tf3(primitive, vertices, 0, work, count, shift, ordering_table, scratch, 1);
}

uint32 *GsA4divTF3L(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    return gs_a4_div_tf3(primitive, vertices, normals, work, count, shift, ordering_table, scratch, 2);
}

uint32 *GsA4divTF3LFG(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    return gs_a4_div_tf3(primitive, vertices, normals, work, count, shift, ordering_table, scratch, 3);
}

static void gs_subdivision_tf4_emit_triangle(uint32 first, uint32 second, uint32 third, uint32 scratch, uint32 **cursor)
{
    uint32 *work, index, slot;
    gs_tmd_load_vector(first, 0);
    gs_tmd_load_vector(second, 1);
    gs_tmd_load_vector(third, 2);
    xport_gte_execute(0x280030);
    work = gs_tmd_work_buffer(*cursor, 8);
    xport_store_le32(work + 1, r_u32(scratch + 64));
    ((uint8 *)work)[3] = 7;
    ((uint8 *)work)[7] = 0x24;
    w_u32(scratch + 40, xport_gte_read_flag());
    if (r_s32(scratch + 40) >= 0)
    {
        xport_gte_execute(0x158002D);
        for (index = 0; index < 3; ++index)
            xport_store_le32(work + 2 + index * 2, xport_gte_read_data(12 + index));
        w_u32(scratch + 24, xport_gte_read_data(7));
        xport_store_le32(work + 3, r_u16(first + 6));
        xport_store_le32(work + 5, r_u16(second + 6));
        xport_store_le32(work + 7, r_u16(third + 6));
        xport_store_le16((uint8 *)work + 14, r_u16(scratch + 74));
        xport_store_le16((uint8 *)work + 22, r_u16(scratch + 82));
        slot = gs_subdivision_f4_slot(scratch);
        gs_subdivision_f4_link(cursor, scratch, slot, 8);
        w_u32(scratch + 20, gpu_packet_link((uintptr_t)*cursor));
    }
}

static void gs_subdivision_recurse_tf4(uint32 vertices, uint32 scratch, uint32 level, uint32 **cursor)
{
    uint32 next = vertices + 32, index, flags, last_flags, emit = 0, slot;
    sint32 maximum, minimum, limit;
    for (index = 0; index < 3; ++index)
        gs_tmd_load_vector(vertices + index * 8, index);
    xport_gte_execute(0x280030);
    for (index = 0; index < 3; ++index)
        w_u16(scratch + 72 + index * 8, r_u16(vertices + 6 + index * 8));
    w_u32(scratch + 40, xport_gte_read_flag());
    for (index = 0; index < 3; ++index)
        w_u32(scratch + 68 + index * 8, xport_gte_read_data(12 + index));
    gs_tmd_load_vector(vertices + 24, 0);
    xport_gte_execute(0x180001);
    w_u16(scratch + 96, r_u16(vertices + 30));
    w_u32(scratch + 36, xport_gte_read_flag());
    flags = r_u32(scratch + 40);
    last_flags = r_u32(scratch + 36);
    w_u32(scratch + 40, flags | last_flags);
    w_u32(scratch + 92, xport_gte_read_data(14));
    xport_gte_execute(0x168002E);
    w_u32(scratch + 24, (uint32)((sint32)xport_gte_read_data(19) >> 2));
    if (gs_subdivision_g4_bounds(scratch + 60, scratch) != 0)
        return;
    if (r_u32(scratch) == level)
    {
        if (r_s32(scratch + 40) < 0)
            return;
        emit = 1;
    }
    else if (r_s32(scratch + 40) >= 0)
    {
        maximum = r_s16(scratch + 48);
        minimum = r_s16(scratch + 44);
        limit = r_s16(scratch + 32);
        if (maximum - minimum < limit)
        {
            maximum = r_s16(scratch + 50);
            minimum = r_s16(scratch + 46);
            limit = r_s16(scratch + 34);
            if (maximum - minimum < limit)
                emit = 1;
        }
    }
    if (emit)
    {
        uint32 *work = gs_tmd_work_buffer(*cursor, 10);
        for (index = 0; index < 10; ++index)
            xport_store_le32(work + index, r_u32(scratch + 60 + index * 4));
        slot = gs_subdivision_f4_slot(scratch);
        gs_subdivision_f4_link(cursor, scratch, slot, 10);
        w_u32(scratch + 20, gpu_packet_link((uintptr_t)*cursor));
        return;
    }
    ++level;
    gs_subdivision_copy_vector(next, vertices);
    gs_subdivision_midpoint_uv(next + 8, vertices, vertices + 8);
    gs_subdivision_midpoint_uv(next + 16, vertices, vertices + 16);
    gs_subdivision_midpoint_uv(next + 24, vertices, vertices + 24);
    gs_subdivision_tf4_emit_triangle(vertices, vertices + 8, next + 8, scratch, cursor);
    gs_subdivision_recurse_tf4(next, scratch, level, cursor);
    gs_subdivision_copy_vector(next, vertices + 8);
    gs_subdivision_midpoint_uv(next + 16, vertices + 8, vertices + 24);
    gs_subdivision_tf4_emit_triangle(vertices + 8, vertices + 24, next + 16, scratch, cursor);
    gs_subdivision_recurse_tf4(next, scratch, level, cursor);
    gs_subdivision_copy_vector(next, vertices + 24);
    gs_subdivision_midpoint_uv(next + 8, vertices + 24, vertices + 16);
    gs_subdivision_tf4_emit_triangle(vertices + 24, vertices + 16, next + 8, scratch, cursor);
    gs_subdivision_recurse_tf4(next, scratch, level, cursor);
    gs_subdivision_copy_vector(next, vertices + 16);
    gs_subdivision_midpoint_uv(next + 16, vertices, vertices + 16);
    gs_subdivision_tf4_emit_triangle(vertices + 16, vertices, next + 16, scratch, cursor);
    gs_subdivision_recurse_tf4(next, scratch, level, cursor);
}

static uint32 *gs_a4_div_tf4(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch, uint32 mode)
{
    uint32 base, index, addresses[3], vertex, slot, depth, threshold;
    uint32 *packet;
    uint32 colored = mode == 1;
    uint32 lit = mode >= 2;
    uint32 index_offset = colored ? 24 : 22;
    base = r_u32(ordering_table + 4);
    if (lit)
    {
        w_u32(scratch + 12, shift);
        w_u16(scratch + 32, (uint32)gs_subdivision_width);
        w_u16(scratch + 34, (uint32)gs_subdivision_height);
        w_u32(scratch + 20, gpu_packet_link((uintptr_t)work));
        w_u32(scratch + 28, (uint32)gs_subdivision_distance);
        w_u16(scratch + 52, (uint32)(gs_screen_width / 2));
        w_u16(scratch + 54, (uint32)(gs_screen_height / 2));
        w_u8(scratch + 63, 9);
        w_u8(scratch + 67, 0x2C);
        w_u8(scratch + 64, 0x80);
        w_u8(scratch + 65, 0x80);
        w_u8(scratch + 66, 0x80);
        w_u32(scratch + 16, base);
        xport_gte_write_data(6, r_u32(scratch + 64));
    }
    else
    {
        w_u32(scratch + 20, gpu_packet_link((uintptr_t)work));
        w_u32(scratch + 28, (uint32)gs_subdivision_distance);
        if (colored)
            w_u16(scratch + 34, (uint32)gs_subdivision_height);
        else
            w_u16(scratch + 32, (uint32)gs_subdivision_width);
        w_u32(scratch + 12, shift);
        if (colored)
            w_u16(scratch + 32, (uint32)gs_subdivision_width);
        else
            w_u16(scratch + 34, (uint32)gs_subdivision_height);
        w_u16(scratch + 52, (uint32)(gs_screen_width / 2));
        if (colored)
            w_u32(scratch + 16, base);
        w_u16(scratch + 54, (uint32)(gs_screen_height / 2));
        w_u8(scratch + 63, 9);
        w_u8(scratch + 67, 0x2C);
        if (!colored)
        {
            w_u8(scratch + 64, 0x80);
            w_u8(scratch + 65, 0x80);
            w_u8(scratch + 66, 0x80);
            w_u32(scratch + 16, base);
        }
    }
    while ((sint32)count > 0)
    {
        for (index = 0; index < 3; ++index)
            addresses[index] = vertices + (uint32)r_u16(primitive + index_offset + index * 2) * 8;
        for (index = 0; index < 3; ++index)
            gs_tmd_load_vector(addresses[index], index);
        xport_gte_execute(0x280030);
        work = gs_tmd_work_buffer(work, 10);
        xport_store_le32(work + 3, r_u32(primitive + 4));
        if (lit)
            xport_store_le32(work + 7, r_u32(primitive + 12));
        if (colored)
            xport_store_le32(work + 5, r_u32(primitive + 8));
        w_u32(scratch + 36, xport_gte_read_flag());
        xport_gte_execute(0x1400006);
        if (colored)
            xport_store_le32(work + 7, r_u32(primitive + 12));
        else
            xport_store_le32(work + 9, r_u32(primitive + 16));
        w_u32(scratch + 24, xport_gte_read_data(24));
        if (r_s32(scratch + 24) > 0)
        {
            if (lit)
            {
                vertex = normals + (uint32)r_u16(primitive + 20) * 8;
                gs_tmd_load_vector(vertex, 0);
                xport_gte_execute(mode == 3 ? 0xE80413 : 0x108041B);
                xport_store_le32(work + 5, r_u32(primitive + 8));
            }
            for (index = 0; index < 3; ++index)
                xport_store_le32(work + 2 + index * 2, xport_gte_read_data(12 + index));
            vertex = vertices + (uint32)r_u16(primitive + index_offset + 6) * 8;
            gs_tmd_load_vector(vertex, 0);
            xport_gte_execute(0x180001);
            if (colored)
                xport_store_le32(work + 9, r_u32(primitive + 16));
            else if (!lit)
                xport_store_le32(work + 5, r_u32(primitive + 8));
            w_u32(scratch + 40, xport_gte_read_flag());
            if ((r_u32(scratch + 40) & 0x40000) == 0 || (r_u32(scratch + 36) & 0x40000) == 0)
            {
                xport_store_le32(work + 8, xport_gte_read_data(14));
                xport_gte_execute(0x168002E);
                if (colored)
                    xport_store_le32(work + 1, r_u32(primitive + 20) | 0x2C000000);
                else if (!lit)
                    xport_store_le32(work + 7, r_u32(primitive + 12));
                w_u32(scratch + 24, xport_gte_read_data(7));
                slot = gs_subdivision_f4_slot(scratch);
                if (!colored && !lit)
                    xport_store_le32(work + 1, 0x2C808080);
                depth = r_u32(scratch + 24);
                threshold = r_u32(scratch + 28);
                if ((sint32)depth < (sint32)threshold || r_s32(scratch + 36) < 0 || (r_u32(scratch + 40) & 0x80000000) != 0)
                {
                    if (lit)
                        w_u32(scratch + 64, xport_gte_read_data(22));
                    else if (colored)
                        w_u32(scratch + 64, xport_load_le32(work + 1));
                    for (index = 0; index < 4; ++index)
                    {
                        vertex = vertices + (uint32)r_u16(primitive + index_offset + index * 2) * 8;
                        gs_subdivision_copy_vector(scratch + 100 + index * 8, vertex);
                    }
                    for (index = 0; index < 4; ++index)
                        w_u16(scratch + 106 + index * 8, r_u16(primitive + 4 + index * 4));
                    w_u16(scratch + 74, r_u16(primitive + 6));
                    w_u32(scratch + 20, gpu_packet_link((uintptr_t)work));
                    w_u16(scratch + 82, r_u16(primitive + 10));
                    gs_subdivision_recurse_tf4(scratch + 100, scratch, 0, &work);
                }
                else
                {
                    packet = work;
                    gs_subdivision_f4_link(&work, scratch, slot, 10);
                    if (lit)
                        xport_store_le32(packet + 1, xport_gte_read_data(22));
                }
            }
        }
        primitive += 32;
        --count;
    }
    w_u32(scratch + 20, gpu_packet_link((uintptr_t)work));
    return work;
}

uint32 *GsA4divTF4NL(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    (void)normals;
    return gs_a4_div_tf4(primitive, vertices, 0, work, count, shift, ordering_table, scratch, 0);
}

uint32 *GsA4divTNF4(uint32 primitive, uint32 vertices, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    return gs_a4_div_tf4(primitive, vertices, 0, work, count, shift, ordering_table, scratch, 1);
}

uint32 *GsA4divTF4L(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    return gs_a4_div_tf4(primitive, vertices, normals, work, count, shift, ordering_table, scratch, 2);
}

uint32 *GsA4divTF4LFG(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    return gs_a4_div_tf4(primitive, vertices, normals, work, count, shift, ordering_table, scratch, 3);
}

static uint32 gs_subdivision_tg3_bounds(uint32 packet, uint32 scratch)
{
    uint32 offset;
    sint32 first, second, third, bound, extent;
    for (offset = 0; offset < 4; offset += 2)
    {
        first = r_s16(packet + 8 + offset);
        second = r_s16(packet + 20 + offset);
        if (second < first)
        {
            w_u16(scratch + 44 + offset, (uint32)first);
            w_u16(scratch + 40 + offset, r_u16(packet + 20 + offset));
        }
        else
        {
            w_u16(scratch + 40 + offset, (uint32)first);
            w_u16(scratch + 44 + offset, r_u16(packet + 20 + offset));
        }
        third = r_s16(packet + 32 + offset);
        bound = r_s16(scratch + 44 + offset);
        if (bound < third)
            w_u16(scratch + 44 + offset, (uint32)third);
        else
        {
            bound = r_s16(scratch + 40 + offset);
            if (third < bound)
                w_u16(scratch + 40 + offset, (uint32)third);
        }
        extent = r_s16(scratch + 48 + offset);
        bound = r_s16(scratch + 44 + offset);
        if (bound < -extent)
            return 1;
        bound = r_s16(scratch + 40 + offset);
        if (extent < bound)
            return 1;
    }
    return 0;
}

static void gs_subdivision_link_tg3(uint32 **cursor, uint32 scratch, uint32 slot)
{
    uint32 *work = gs_tmd_work_buffer(*cursor, 10);
    xport_store_le32(work, (r_u32(slot) & 0xFFFFFF) | 0x09000000);
    w_u32(r_u32(scratch + 52), gpu_packet_link((uintptr_t)work));
    *cursor = work + 10;
}

static void gs_subdivision_midpoint_uv_color(uint32 destination, uint32 left, uint32 right)
{
    uint32 offset, first, second;
    gs_subdivision_midpoint_color(destination, left, right);
    for (offset = 6; offset < 8; ++offset)
    {
        first = r_u8(left + offset);
        second = r_u8(right + offset);
        w_u8(destination + offset, (first + second) >> 1);
    }
}

static void gs_subdivision_emit_tg3(uint32 first, uint32 second, uint32 third, uint32 scratch, uint32 **cursor)
{
    uint32 *work, index, slot;
    gs_tmd_load_vector(first, 0);
    gs_tmd_load_vector(second, 1);
    gs_tmd_load_vector(third, 2);
    xport_gte_execute(0x280030);
    work = gs_tmd_work_buffer(*cursor, 10);
    xport_store_le32(work + 1, r_u32(first + 8));
    ((uint8 *)work)[3] = 9;
    ((uint8 *)work)[7] = 0x34;
    w_u32(scratch + 36, xport_gte_read_flag());
    if (r_s32(scratch + 36) >= 0)
    {
        xport_gte_execute(0x158002D);
        for (index = 0; index < 3; ++index)
            xport_store_le32(work + 2 + index * 3, xport_gte_read_data(12 + index));
        w_u32(scratch + 24, xport_gte_read_data(7));
        xport_store_le32(work + 3, r_u16(first + 6));
        xport_store_le32(work + 6, r_u16(second + 6));
        xport_store_le32(work + 9, r_u16(third + 6));
        xport_store_le32(work + 4, r_u32(second + 8));
        xport_store_le32(work + 7, r_u32(third + 8));
        xport_store_le16((uint8 *)work + 14, r_u16(scratch + 70));
        xport_store_le16((uint8 *)work + 26, r_u16(scratch + 82));
        slot = gs_subdivision_slot(scratch);
        gs_subdivision_link_tg3(cursor, scratch, slot);
        w_u32(scratch + 20, gpu_packet_link((uintptr_t)*cursor));
    }
}

static void gs_subdivision_recurse_tg3(uint32 vertices, uint32 scratch, uint32 level, uint32 **cursor)
{
    uint32 next = vertices + 36, index, emit = 0, slot;
    sint32 maximum, minimum, limit;
    for (index = 0; index < 3; ++index)
        gs_tmd_load_vector(vertices + index * 12, index);
    xport_gte_execute(0x280030);
    for (index = 0; index < 3; ++index)
        w_u16(scratch + 68 + index * 12, r_u16(vertices + 6 + index * 12));
    w_u32(scratch + 36, xport_gte_read_flag());
    for (index = 0; index < 3; ++index)
        w_u32(scratch + 64 + index * 12, xport_gte_read_data(12 + index));
    xport_gte_execute(0x158002D);
    w_u32(scratch + 24, (uint32)((sint32)xport_gte_read_data(19) >> 2));
    if (gs_subdivision_tg3_bounds(scratch + 56, scratch) != 0)
        return;
    if (r_u32(scratch) == level)
    {
        if (r_s32(scratch + 36) < 0)
            return;
        emit = 1;
    }
    else if (r_s32(scratch + 36) >= 0)
    {
        maximum = r_s16(scratch + 44);
        minimum = r_s16(scratch + 40);
        limit = r_s16(scratch + 32);
        if (maximum - minimum < limit)
        {
            maximum = r_s16(scratch + 46);
            minimum = r_s16(scratch + 42);
            limit = r_s16(scratch + 34);
            if (maximum - minimum < limit)
                emit = 1;
        }
    }
    if (emit)
    {
        uint32 *work = gs_tmd_work_buffer(*cursor, 10);
        xport_store_le32(work, r_u32(scratch + 56));
        for (index = 0; index < 3; ++index)
        {
            xport_store_le32(work + 1 + index * 3, r_u32(vertices + 8 + index * 12));
            xport_store_le32(work + 2 + index * 3, r_u32(scratch + 64 + index * 12));
            xport_store_le32(work + 3 + index * 3, r_u32(scratch + 68 + index * 12));
        }
        slot = gs_subdivision_slot(scratch);
        gs_subdivision_link_tg3(cursor, scratch, slot);
        w_u32(scratch + 20, gpu_packet_link((uintptr_t)*cursor));
        return;
    }
    ++level;
    gs_subdivision_copy_color_vertex(next, vertices);
    gs_subdivision_midpoint_uv_color(next + 12, vertices, vertices + 12);
    gs_subdivision_midpoint_uv_color(next + 24, vertices, vertices + 24);
    gs_subdivision_emit_tg3(vertices, vertices + 12, next + 12, scratch, cursor);
    gs_subdivision_recurse_tg3(next, scratch, level, cursor);
    gs_subdivision_midpoint_uv_color(next, vertices + 12, vertices + 24);
    gs_subdivision_recurse_tg3(next, scratch, level, cursor);
    gs_subdivision_copy_color_vertex(next + 24, vertices + 12);
    gs_subdivision_emit_tg3(vertices + 12, vertices + 24, next, scratch, cursor);
    gs_subdivision_recurse_tg3(next, scratch, level, cursor);
    gs_subdivision_copy_color_vertex(next + 12, vertices + 24);
    gs_subdivision_midpoint_uv_color(next + 24, vertices, vertices + 24);
    gs_subdivision_emit_tg3(vertices + 24, vertices, next + 24, scratch, cursor);
    gs_subdivision_recurse_tg3(next, scratch, level, cursor);
}

static uint32 *gs_a4_div_tg3(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch, uint32 mode)
{
    uint32 base, index, addresses[3], vertex, slot, depth, threshold;
    uint32 colored = mode == 1;
    uint32 lit = mode >= 2;
    uint32 index_offset = colored ? 28 : 18;
    uint32 index_stride = colored ? 2 : 4;
    w_u16(scratch + 48, (uint32)(gs_screen_width / 2));
    w_u16(scratch + 50, (uint32)(gs_screen_height / 2));
    base = r_u32(ordering_table + 4);
    if (lit)
    {
        w_u32(scratch + 12, shift);
        w_u16(scratch + 32, (uint32)gs_subdivision_width);
        w_u16(scratch + 34, (uint32)gs_subdivision_height);
        w_u32(scratch + 20, gpu_packet_link((uintptr_t)work));
        w_u32(scratch + 28, (uint32)gs_subdivision_distance);
        w_u8(scratch + 59, 9);
        w_u8(scratch + 63, 0x34);
        for (index = 0; index < 3; ++index)
        {
            w_u8(scratch + 60 + index * 12, 0x80);
            w_u8(scratch + 61 + index * 12, 0x80);
            w_u8(scratch + 62 + index * 12, 0x80);
        }
        w_u32(scratch + 16, base);
        xport_gte_write_data(6, r_u32(scratch + 60));
    }
    else
    {
        w_u32(scratch + 20, gpu_packet_link((uintptr_t)work));
        w_u32(scratch + 28, (uint32)gs_subdivision_distance);
        w_u16(scratch + 32, (uint32)gs_subdivision_width);
        if (!colored)
            w_u16(scratch + 34, (uint32)gs_subdivision_height);
        w_u32(scratch + 12, shift);
        w_u8(scratch + 59, 9);
        if (colored)
            w_u16(scratch + 34, (uint32)gs_subdivision_height);
        w_u8(scratch + 63, 0x34);
        w_u32(scratch + 16, base);
    }
    while ((sint32)count > 0)
    {
        for (index = 0; index < 3; ++index)
            addresses[index] = vertices + (uint32)r_u16(primitive + index_offset + index * index_stride) * 8;
        for (index = 0; index < 3; ++index)
            gs_tmd_load_vector(addresses[index], index);
        xport_gte_execute(0x280030);
        work = gs_tmd_work_buffer(work, 10);
        xport_store_le32(work + 3, r_u32(primitive + 4));
        if (colored)
            xport_store_le32(work + 6, r_u32(primitive + 8));
        else
            xport_store_le32(work + 9, r_u32(primitive + 12));
        w_u32(scratch + 36, xport_gte_read_flag());
        xport_gte_execute(0x1400006);
        if (colored)
            xport_store_le32(work + 9, r_u32(primitive + 12));
        else if (!lit)
            xport_store_le32(work + 4, 0x808080);
        w_u32(scratch + 24, xport_gte_read_data(24));
        if (r_s32(scratch + 24) > 0 && (r_u32(scratch + 36) & 0x40000) == 0)
        {
            for (index = 0; index < 3; ++index)
                xport_store_le32(work + 2 + index * 3, xport_gte_read_data(12 + index));
            xport_gte_execute(0x158002D);
            if (colored)
                xport_store_le32(work + 1, r_u32(primitive + 16) | 0x34000000);
            else if (!lit)
                xport_store_le32(work + 7, 0x808080);
            w_u32(scratch + 24, xport_gte_read_data(7));
            if (lit)
            {
                for (index = 0; index < 3; ++index)
                    addresses[index] = normals + (uint32)r_u16(primitive + 16 + index * 4) * 8;
                for (index = 0; index < 3; ++index)
                    gs_tmd_load_vector(addresses[index], index);
                xport_gte_execute(mode == 3 ? 0xF80416 : 0x118043F);
            }
            if (colored)
            {
                xport_store_le32(work + 4, r_u32(primitive + 20));
                xport_store_le32(work + 7, r_u32(primitive + 24));
            }
            else
            {
                vertex = r_u32(primitive + 8);
                if (!lit)
                    xport_store_le32(work + 1, 0x34808080);
                xport_store_le32(work + 6, vertex);
            }
            slot = gs_subdivision_slot(scratch);
            if (lit)
            {
                for (index = 0; index < 3; ++index)
                    xport_store_le32(work + 1 + index * 3, xport_gte_read_data(20 + index));
            }
            if (colored)
            {
                index = r_u32(scratch + 36);
                if ((sint32)index < 0)
                    goto subdivide;
            }
            depth = r_u32(scratch + 24);
            threshold = r_u32(scratch + 28);
            if ((sint32)depth < (sint32)threshold || (!colored && r_s32(scratch + 36) < 0))
            {
            subdivide:
                for (index = 0; index < 3; ++index)
                {
                    vertex = vertices + (uint32)r_u16(primitive + index_offset + index * index_stride) * 8;
                    gs_subdivision_copy_vector(scratch + 96 + index * 12, vertex);
                    w_u32(scratch + 104 + index * 12, lit ? xport_load_le32(work + 1 + index * 3) : colored ? xport_load_le32(work + 1 + index * 3) | 0x34000000 : 0x34808080);
                }
                for (index = 0; index < 3; ++index)
                    w_u16(scratch + 102 + index * 12, r_u16(primitive + 4 + index * 4));
                w_u16(scratch + 70, r_u16(primitive + 6));
                w_u32(scratch + 20, gpu_packet_link((uintptr_t)work));
                w_u16(scratch + 82, r_u16(primitive + 10));
                gs_subdivision_recurse_tg3(scratch + 96, scratch, 0, &work);
            }
            else
                gs_subdivision_link_tg3(&work, scratch, slot);
        }
        primitive += colored ? 36 : 28;
        --count;
    }
    w_u32(scratch + 20, gpu_packet_link((uintptr_t)work));
    return work;
}

uint32 *GsA4divTG3NL(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    (void)normals;
    return gs_a4_div_tg3(primitive, vertices, 0, work, count, shift, ordering_table, scratch, 0);
}

uint32 *GsA4divTNG3(uint32 primitive, uint32 vertices, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    return gs_a4_div_tg3(primitive, vertices, 0, work, count, shift, ordering_table, scratch, 1);
}

uint32 *GsA4divTG3L(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    return gs_a4_div_tg3(primitive, vertices, normals, work, count, shift, ordering_table, scratch, 2);
}

uint32 *GsA4divTG3LFG(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    return gs_a4_div_tg3(primitive, vertices, normals, work, count, shift, ordering_table, scratch, 3);
}

static uint32 gs_subdivision_tg4_bounds(uint32 packet, uint32 scratch)
{
    uint32 offset, index;
    sint32 first, second, coordinate, bound, extent;
    for (offset = 0; offset < 4; offset += 2)
    {
        first = r_s16(packet + 8 + offset);
        second = r_s16(packet + 20 + offset);
        if (second < first)
        {
            w_u16(scratch + 48 + offset, (uint32)first);
            w_u16(scratch + 44 + offset, r_u16(packet + 20 + offset));
        }
        else
        {
            w_u16(scratch + 44 + offset, (uint32)first);
            w_u16(scratch + 48 + offset, r_u16(packet + 20 + offset));
        }
        for (index = 2; index < 4; ++index)
        {
            coordinate = r_s16(packet + 8 + index * 12 + offset);
            bound = r_s16(scratch + 48 + offset);
            if (bound < coordinate)
                w_u16(scratch + 48 + offset, (uint32)coordinate);
            else
            {
                bound = r_s16(scratch + 44 + offset);
                if (coordinate < bound)
                    w_u16(scratch + 44 + offset, (uint32)coordinate);
            }
        }
        extent = r_s16(scratch + 52 + offset);
        bound = r_s16(scratch + 48 + offset);
        if (bound < -extent)
            return 1;
        bound = r_s16(scratch + 44 + offset);
        if (extent < bound)
            return 1;
    }
    return 0;
}

static void gs_subdivision_tg4_emit_triangle(uint32 first, uint32 second, uint32 third, uint32 scratch, uint32 **cursor)
{
    uint32 *work, index, slot;
    gs_tmd_load_vector(first, 0);
    gs_tmd_load_vector(second, 1);
    gs_tmd_load_vector(third, 2);
    xport_gte_execute(0x280030);
    work = gs_tmd_work_buffer(*cursor, 10);
    xport_store_le32(work + 1, r_u32(first + 8));
    ((uint8 *)work)[3] = 9;
    ((uint8 *)work)[7] = 0x34;
    w_u32(scratch + 40, xport_gte_read_flag());
    if (r_s32(scratch + 40) >= 0)
    {
        xport_gte_execute(0x158002D);
        for (index = 0; index < 3; ++index)
            xport_store_le32(work + 2 + index * 3, xport_gte_read_data(12 + index));
        w_u32(scratch + 24, xport_gte_read_data(7));
        xport_store_le32(work + 3, r_u16(first + 6));
        xport_store_le32(work + 6, r_u16(second + 6));
        xport_store_le32(work + 9, r_u16(third + 6));
        xport_store_le32(work + 4, r_u32(second + 8));
        xport_store_le32(work + 7, r_u32(third + 8));
        xport_store_le16((uint8 *)work + 14, r_u16(scratch + 74));
        xport_store_le16((uint8 *)work + 26, r_u16(scratch + 86));
        slot = gs_subdivision_f4_slot(scratch);
        gs_subdivision_f4_link(cursor, scratch, slot, 10);
        w_u32(scratch + 20, gpu_packet_link((uintptr_t)*cursor));
    }
}

static void gs_subdivision_recurse_tg4(uint32 vertices, uint32 scratch, uint32 level, uint32 **cursor)
{
    uint32 next = vertices + 48, index, flags, last_flags, emit = 0, slot;
    sint32 maximum, minimum, limit;
    for (index = 0; index < 3; ++index)
        gs_tmd_load_vector(vertices + index * 12, index);
    xport_gte_execute(0x280030);
    for (index = 0; index < 3; ++index)
        w_u16(scratch + 72 + index * 12, r_u16(vertices + 6 + index * 12));
    w_u32(scratch + 40, xport_gte_read_flag());
    for (index = 0; index < 3; ++index)
        w_u32(scratch + 68 + index * 12, xport_gte_read_data(12 + index));
    gs_tmd_load_vector(vertices + 36, 0);
    xport_gte_execute(0x180001);
    w_u16(scratch + 108, r_u16(vertices + 42));
    w_u32(scratch + 36, xport_gte_read_flag());
    flags = r_u32(scratch + 40);
    last_flags = r_u32(scratch + 36);
    w_u32(scratch + 40, flags | last_flags);
    w_u32(scratch + 104, xport_gte_read_data(14));
    xport_gte_execute(0x168002E);
    w_u32(scratch + 24, (uint32)((sint32)xport_gte_read_data(19) >> 2));
    if (gs_subdivision_tg4_bounds(scratch + 60, scratch) != 0)
        return;
    if (r_u32(scratch) == level)
    {
        if (r_s32(scratch + 40) < 0)
            return;
        emit = 1;
    }
    else if (r_s32(scratch + 40) >= 0)
    {
        maximum = r_s16(scratch + 48);
        minimum = r_s16(scratch + 44);
        limit = r_s16(scratch + 32);
        if (maximum - minimum < limit)
        {
            maximum = r_s16(scratch + 50);
            minimum = r_s16(scratch + 46);
            limit = r_s16(scratch + 34);
            if (maximum - minimum < limit)
                emit = 1;
        }
    }
    if (emit)
    {
        uint32 *work = gs_tmd_work_buffer(*cursor, 13);
        xport_store_le32(work, r_u32(scratch + 60));
        for (index = 0; index < 4; ++index)
        {
            xport_store_le32(work + 1 + index * 3, r_u32(vertices + 8 + index * 12));
            xport_store_le32(work + 2 + index * 3, r_u32(scratch + 68 + index * 12));
            xport_store_le32(work + 3 + index * 3, r_u32(scratch + 72 + index * 12));
        }
        slot = gs_subdivision_f4_slot(scratch);
        gs_subdivision_f4_link(cursor, scratch, slot, 13);
        w_u32(scratch + 20, gpu_packet_link((uintptr_t)*cursor));
        return;
    }
    ++level;
    gs_subdivision_copy_color_vertex(next, vertices);
    gs_subdivision_midpoint_uv_color(next + 12, vertices, vertices + 12);
    gs_subdivision_midpoint_uv_color(next + 24, vertices, vertices + 24);
    gs_subdivision_midpoint_uv_color(next + 36, vertices, vertices + 36);
    gs_subdivision_tg4_emit_triangle(vertices, vertices + 12, next + 12, scratch, cursor);
    gs_subdivision_recurse_tg4(next, scratch, level, cursor);
    gs_subdivision_copy_color_vertex(next, vertices + 12);
    gs_subdivision_midpoint_uv_color(next + 24, vertices + 12, vertices + 36);
    gs_subdivision_tg4_emit_triangle(vertices + 12, vertices + 36, next + 24, scratch, cursor);
    gs_subdivision_recurse_tg4(next, scratch, level, cursor);
    gs_subdivision_copy_color_vertex(next, vertices + 36);
    gs_subdivision_midpoint_uv_color(next + 12, vertices + 36, vertices + 24);
    gs_subdivision_tg4_emit_triangle(vertices + 36, vertices + 24, next + 12, scratch, cursor);
    gs_subdivision_recurse_tg4(next, scratch, level, cursor);
    gs_subdivision_copy_color_vertex(next, vertices + 24);
    gs_subdivision_midpoint_uv_color(next + 24, vertices, vertices + 24);
    gs_subdivision_tg4_emit_triangle(vertices + 24, vertices, next + 24, scratch, cursor);
    gs_subdivision_recurse_tg4(next, scratch, level, cursor);
}

static uint32 *gs_a4_div_tg4(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch, uint32 mode)
{
    uint32 base, index, addresses[3], vertex, slot, depth, threshold;
    uint32 colored = mode == 1;
    uint32 lit = mode >= 2;
    uint32 index_offset = colored ? 36 : 22;
    uint32 index_stride = colored ? 2 : 4;
    w_u16(scratch + 52, (uint32)(gs_screen_width / 2));
    w_u16(scratch + 54, (uint32)(gs_screen_height / 2));
    base = r_u32(ordering_table + 4);
    if (lit)
    {
        w_u32(scratch + 12, shift);
        w_u16(scratch + 32, (uint32)gs_subdivision_width);
        w_u16(scratch + 34, (uint32)gs_subdivision_height);
        w_u32(scratch + 20, gpu_packet_link((uintptr_t)work));
        w_u32(scratch + 28, (uint32)gs_subdivision_distance);
        w_u8(scratch + 63, 12);
        w_u8(scratch + 67, 0x3C);
        for (index = 0; index < 4; ++index)
        {
            w_u8(scratch + 64 + index * 12, 0x80);
            w_u8(scratch + 65 + index * 12, 0x80);
            w_u8(scratch + 66 + index * 12, 0x80);
        }
        w_u32(scratch + 16, base);
        xport_gte_write_data(6, r_u32(scratch + 64));
    }
    else
    {
        w_u32(scratch + 20, gpu_packet_link((uintptr_t)work));
        w_u32(scratch + 28, (uint32)gs_subdivision_distance);
        w_u16(scratch + 32, (uint32)gs_subdivision_width);
        if (!colored)
            w_u16(scratch + 34, (uint32)gs_subdivision_height);
        w_u32(scratch + 12, shift);
        w_u8(scratch + 63, 12);
        if (colored)
            w_u16(scratch + 34, (uint32)gs_subdivision_height);
        w_u8(scratch + 67, 0x3C);
        w_u32(scratch + 16, base);
    }
    while ((sint32)count > 0)
    {
        for (index = 0; index < 3; ++index)
            addresses[index] = vertices + (uint32)r_u16(primitive + index_offset + index * index_stride) * 8;
        for (index = 0; index < 3; ++index)
            gs_tmd_load_vector(addresses[index], index);
        xport_gte_execute(0x280030);
        work = gs_tmd_work_buffer(work, 13);
        xport_store_le32(work + 3, r_u32(primitive + 4));
        xport_store_le32(work + (lit ? 9 : 6), r_u32(primitive + (lit ? 12 : 8)));
        w_u32(scratch + 36, xport_gte_read_flag());
        xport_gte_execute(0x1400006);
        xport_store_le32(work + (lit ? 12 : 9), r_u32(primitive + (lit ? 16 : 12)));
        w_u32(scratch + 24, xport_gte_read_data(24));
        if (r_s32(scratch + 24) > 0)
        {
            for (index = 0; index < 3; ++index)
                xport_store_le32(work + 2 + index * 3, xport_gte_read_data(12 + index));
            vertex = vertices + (uint32)r_u16(primitive + index_offset + 3 * index_stride) * 8;
            gs_tmd_load_vector(vertex, 0);
            xport_gte_execute(0x180001);
            if (!lit && colored)
            {
                xport_store_le32(work + 12, r_u32(primitive + 16));
                xport_store_le32(work + 1, r_u32(primitive + 20) | 0x3C000000);
            }
            else if (!lit)
            {
                vertex = r_u32(primitive + 16);
                xport_store_le32(work + 1, 0x3C808080);
                xport_store_le32(work + 12, vertex);
            }
            w_u32(scratch + 40, xport_gte_read_flag());
            if ((r_u32(scratch + 40) & 0x40000) == 0 || (r_u32(scratch + 36) & 0x40000) == 0)
            {
                xport_store_le32(work + 11, xport_gte_read_data(14));
                xport_gte_execute(0x168002E);
                if (!lit)
                {
                    for (index = 1; index < 4; ++index)
                        xport_store_le32(work + 1 + index * 3, colored ? r_u32(primitive + 20 + index * 4) : 0x808080);
                }
                w_u32(scratch + 24, xport_gte_read_data(7));
                if (lit)
                {
                    for (index = 0; index < 3; ++index)
                        addresses[index] = normals + (uint32)r_u16(primitive + 20 + index * 4) * 8;
                    for (index = 0; index < 3; ++index)
                        gs_tmd_load_vector(addresses[index], index);
                    xport_gte_execute(mode == 3 ? 0xF80416 : 0x118043F);
                    xport_store_le32(work + 6, r_u32(primitive + 8));
                }
                slot = gs_subdivision_f4_slot(scratch);
                if (lit)
                {
                    for (index = 0; index < 3; ++index)
                        xport_store_le32(work + 1 + index * 3, xport_gte_read_data(20 + index));
                    vertex = normals + (uint32)r_u16(primitive + 32) * 8;
                    gs_tmd_load_vector(vertex, 0);
                    xport_gte_execute(mode == 3 ? 0xE80413 : 0x108041B);
                    xport_store_le32(work + 10, xport_gte_read_data(22));
                }
                depth = r_u32(scratch + 24);
                threshold = r_u32(scratch + 28);
                if ((sint32)depth < (sint32)threshold || r_s32(scratch + 36) < 0 || (r_u32(scratch + 40) & 0x80000000) != 0)
                {
                    for (index = 0; index < 4; ++index)
                    {
                        vertex = vertices + (uint32)r_u16(primitive + index_offset + index * index_stride) * 8;
                        gs_subdivision_copy_vector(scratch + 112 + index * 12, vertex);
                        w_u32(scratch + 120 + index * 12, lit ? xport_load_le32(work + 1 + index * 3) : colored ? xport_load_le32(work + 1 + index * 3) | 0x3C000000 : 0x3C808080);
                    }
                    for (index = 0; index < 4; ++index)
                        w_u16(scratch + 118 + index * 12, r_u16(primitive + 4 + index * 4));
                    w_u16(scratch + 74, r_u16(primitive + 6));
                    w_u32(scratch + 20, gpu_packet_link((uintptr_t)work));
                    w_u16(scratch + 86, r_u16(primitive + 10));
                    gs_subdivision_recurse_tg4(scratch + 112, scratch, 0, &work);
                }
                else
                    gs_subdivision_f4_link(&work, scratch, slot, 13);
            }
        }
        primitive += colored ? 44 : 36;
        --count;
    }
    w_u32(scratch + 20, gpu_packet_link((uintptr_t)work));
    return work;
}

uint32 *GsA4divTG4NL(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    (void)normals;
    return gs_a4_div_tg4(primitive, vertices, 0, work, count, shift, ordering_table, scratch, 0);
}

uint32 *GsA4divTNG4(uint32 primitive, uint32 vertices, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    return gs_a4_div_tg4(primitive, vertices, 0, work, count, shift, ordering_table, scratch, 1);
}

uint32 *GsA4divTG4L(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    return gs_a4_div_tg4(primitive, vertices, normals, work, count, shift, ordering_table, scratch, 2);
}

uint32 *GsA4divTG4LFG(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    return gs_a4_div_tg4(primitive, vertices, normals, work, count, shift, ordering_table, scratch, 3);
}

static uint32 *gs_tmd_fast_gradient_lit(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch, uint32 gouraud, uint32 fog)
{
    uint32 base = r_u32(ordering_table + 4);
    uint32 index, addresses[3], normal, slot, depth, depth_shift;
    w_u32(scratch + 12, shift);
    w_u32(scratch + 16, base);
    while ((sint32)count > 0)
    {
        for (index = 0; index < 3; ++index)
            addresses[index] = vertices + (uint32)r_u16(primitive + 18 + index * (gouraud ? 4 : 2)) * 8;
        for (index = 0; index < 3; ++index)
            gs_tmd_load_vector(addresses[index], index);
        xport_gte_execute(0x280030);
        if (!gouraud)
        {
            w_u32(scratch, r_u32(primitive + 4));
            w_u8(scratch + 3, 0x30);
        }
        w_u32(scratch + 36, xport_gte_read_flag());
        if (r_s32(scratch + 36) >= 0)
        {
            xport_gte_execute(0x1400006);
            w_u32(scratch + 24, xport_gte_read_data(24));
            if (r_s32(scratch + 24) > 0)
            {
                work = gs_tmd_work_buffer(work, 7);
                for (index = 0; index < 3; ++index)
                    xport_store_le32(work + 2 + index * 2, xport_gte_read_data(12 + index));
                xport_gte_execute(0x158002D);
                w_u32(scratch + 24, xport_gte_read_data(7));
                xport_gte_write_data(6, r_u32(gouraud ? primitive + 4 : scratch));
                normal = (uint32)r_u16(primitive + 16) * 8 + normals;
                gs_tmd_load_vector(normal, 0);
                xport_gte_execute(fog ? 0xE80413 : 0x108041B);
                depth = r_u32(scratch + 24);
                depth_shift = r_u32(scratch + 12);
                slot = (uint32)((sint32)depth >> (depth_shift & 31));
                slot = (slot << 2) + r_u32(scratch + 16);
                w_u32(scratch + 52, slot);
                xport_store_le32(work + 1, xport_gte_read_data(22));
                xport_gte_write_data(6, r_u32(primitive + 8));
                normal = (uint32)r_u16(primitive + (gouraud ? 20 : 16)) * 8 + normals;
                gs_tmd_load_vector(normal, 0);
                xport_gte_execute(fog ? 0xE80413 : 0x108041B);
                slot = r_u32(scratch + 52);
                xport_store_le32(work, (r_u32(slot) & 0xFFFFFF) | 0x06000000);
                xport_store_le32(work + 3, xport_gte_read_data(22));
                xport_gte_write_data(6, r_u32(primitive + 12));
                normal = (uint32)r_u16(primitive + (gouraud ? 24 : 16)) * 8 + normals;
                gs_tmd_load_vector(normal, 0);
                xport_gte_execute(fog ? 0xE80413 : 0x108041B);
                w_u32(r_u32(scratch + 52), gpu_packet_link((uintptr_t)work));
                xport_store_le32(work + 5, xport_gte_read_data(22));
                work += 7;
            }
        }
        primitive += gouraud ? 28 : 24;
        --count;
    }
    return work;
}

uint32 *GsTMDfastF3GL(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    return gs_tmd_fast_gradient_lit(primitive, vertices, normals, work, count, shift, ordering_table, scratch, 0, 0);
}

uint32 *GsTMDfastF3GLFG(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    return gs_tmd_fast_gradient_lit(primitive, vertices, normals, work, count, shift, ordering_table, scratch, 0, 1);
}

uint32 *GsTMDfastG3GL(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    return gs_tmd_fast_gradient_lit(primitive, vertices, normals, work, count, shift, ordering_table, scratch, 1, 0);
}

uint32 *GsTMDfastG3GLFG(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    return gs_tmd_fast_gradient_lit(primitive, vertices, normals, work, count, shift, ordering_table, scratch, 1, 1);
}

uint32 *GsTMDfastG3GNL(uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch)
{
    (void)normals;
    return gs_tmd_fast_gradient_nl(primitive, vertices, work, count, shift, ordering_table, scratch, 1);
}

uint32 *GsTMDfastNF3(uint32 primitive, uint32 vertices, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table)
{
    return gs_tmd_fast_unlit_triangle(primitive, vertices, work, count, shift, ordering_table, 0);
}

uint32 *GsTMDfastNG3(uint32 primitive, uint32 vertices, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table)
{
    return gs_tmd_fast_unlit_triangle(primitive, vertices, work, count, shift, ordering_table, 1);
}

uint32 *GsTMDfastTNF3(uint32 primitive, uint32 vertices, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table)
{
    return gs_tmd_fast_unlit_triangle(primitive, vertices, work, count, shift, ordering_table, 2);
}

uint32 *GsTMDfastTNG3(uint32 primitive, uint32 vertices, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table)
{
    return gs_tmd_fast_unlit_triangle(primitive, vertices, work, count, shift, ordering_table, 3);
}

uint32 *GsTMDfastNF4(uint32 primitive, uint32 vertices, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table)
{
    return gs_tmd_fast_unlit_quad(primitive, vertices, work, count, shift, ordering_table, 0);
}

uint32 *GsTMDfastNG4(uint32 primitive, uint32 vertices, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table)
{
    return gs_tmd_fast_unlit_quad(primitive, vertices, work, count, shift, ordering_table, 1);
}

uint32 *GsTMDfastTNF4(uint32 primitive, uint32 vertices, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table)
{
    return gs_tmd_fast_unlit_quad(primitive, vertices, work, count, shift, ordering_table, 2);
}

uint32 *GsTMDfastTNG4(uint32 primitive, uint32 vertices, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table)
{
    return gs_tmd_fast_unlit_quad(primitive, vertices, work, count, shift, ordering_table, 3);
}

#define GS_BIND_PRIMITIVE(adapter, target, arguments) \
    static uint32 *adapter(GsSortAttributes *attributes, uint32 primitive, uint32 vertices, uint32 normals, uint32 *work, uint32 count, uint32 shift, uint32 ordering_table, uint32 scratch) \
    { \
        (void)attributes; \
        (void)normals; \
        (void)scratch; \
        return target arguments; \
    }

GS_BIND_PRIMITIVE(gs_dispatch_tmdfastf3l, GsTMDfastF3L, (primitive, vertices, normals, work, count, shift, ordering_table))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfastf3lfg, GsTMDfastF3LFG, (primitive, vertices, normals, work, count, shift, ordering_table))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfastf3nl, GsTMDfastF3NL, (primitive, vertices, normals, work, count, shift, ordering_table))
GS_BIND_PRIMITIVE(gs_dispatch_a4divf3l, GsA4divF3L, (primitive, vertices, normals, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_a4divf3lfg, GsA4divF3LFG, (primitive, vertices, normals, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_a4divf3nl, GsA4divF3NL, (primitive, vertices, normals, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfastnf3, GsTMDfastNF3, (primitive, vertices, work, count, shift, ordering_table))
GS_BIND_PRIMITIVE(gs_dispatch_a4divnf3, GsA4divNF3, (primitive, vertices, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfastg3l, GsTMDfastG3L, (primitive, vertices, normals, work, count, shift, ordering_table))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfastg3lfg, GsTMDfastG3LFG, (primitive, vertices, normals, work, count, shift, ordering_table))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfastg3nl, GsTMDfastG3NL, (primitive, vertices, normals, work, count, shift, ordering_table))
GS_BIND_PRIMITIVE(gs_dispatch_a4divg3l, GsA4divG3L, (primitive, vertices, normals, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_a4divg3lfg, GsA4divG3LFG, (primitive, vertices, normals, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_a4divg3nl, GsA4divG3NL, (primitive, vertices, normals, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfastng3, GsTMDfastNG3, (primitive, vertices, work, count, shift, ordering_table))
GS_BIND_PRIMITIVE(gs_dispatch_a4divng3, GsA4divNG3, (primitive, vertices, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfasttf3l, GsTMDfastTF3L, (primitive, vertices, normals, work, count, shift, ordering_table))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfasttf3lfg, GsTMDfastTF3LFG, (primitive, vertices, normals, work, count, shift, ordering_table))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfasttf3nl, GsTMDfastTF3NL, (primitive, vertices, normals, work, count, shift, ordering_table))
GS_BIND_PRIMITIVE(gs_dispatch_a4divtf3l, GsA4divTF3L, (primitive, vertices, normals, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_a4divtf3lfg, GsA4divTF3LFG, (primitive, vertices, normals, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_a4divtf3nl, GsA4divTF3NL, (primitive, vertices, normals, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfasttnf3, GsTMDfastTNF3, (primitive, vertices, work, count, shift, ordering_table))
GS_BIND_PRIMITIVE(gs_dispatch_a4divtnf3, GsA4divTNF3, (primitive, vertices, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfasttg3l, GsTMDfastTG3L, (primitive, vertices, normals, work, count, shift, ordering_table))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfasttg3lfg, GsTMDfastTG3LFG, (primitive, vertices, normals, work, count, shift, ordering_table))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfasttg3nl, GsTMDfastTG3NL, (primitive, vertices, normals, work, count, shift, ordering_table))
GS_BIND_PRIMITIVE(gs_dispatch_a4divtg3l, GsA4divTG3L, (primitive, vertices, normals, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_a4divtg3lfg, GsA4divTG3LFG, (primitive, vertices, normals, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_a4divtg3nl, GsA4divTG3NL, (primitive, vertices, normals, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfasttng3, GsTMDfastTNG3, (primitive, vertices, work, count, shift, ordering_table))
GS_BIND_PRIMITIVE(gs_dispatch_a4divtng3, GsA4divTNG3, (primitive, vertices, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfastf4l, GsTMDfastF4L, (primitive, vertices, normals, work, count, shift, ordering_table))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfastf4lfg, GsTMDfastF4LFG, (primitive, vertices, normals, work, count, shift, ordering_table))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfastf4nl, GsTMDfastF4NL, (primitive, vertices, normals, work, count, shift, ordering_table))
GS_BIND_PRIMITIVE(gs_dispatch_a4divf4l, GsA4divF4L, (primitive, vertices, normals, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_a4divf4lfg, GsA4divF4LFG, (primitive, vertices, normals, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_a4divf4nl, GsA4divF4NL, (primitive, vertices, normals, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfastnf4, GsTMDfastNF4, (primitive, vertices, work, count, shift, ordering_table))
GS_BIND_PRIMITIVE(gs_dispatch_a4divnf4, GsA4divNF4, (primitive, vertices, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfastg4l, GsTMDfastG4L, (primitive, vertices, normals, work, count, shift, ordering_table))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfastg4lfg, GsTMDfastG4LFG, (primitive, vertices, normals, work, count, shift, ordering_table))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfastg4nl, GsTMDfastG4NL, (primitive, vertices, normals, work, count, shift, ordering_table))
GS_BIND_PRIMITIVE(gs_dispatch_a4divg4l, GsA4divG4L, (primitive, vertices, normals, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_a4divg4lfg, GsA4divG4LFG, (primitive, vertices, normals, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_a4divg4nl, GsA4divG4NL, (primitive, vertices, normals, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfastng4, GsTMDfastNG4, (primitive, vertices, work, count, shift, ordering_table))
GS_BIND_PRIMITIVE(gs_dispatch_a4divng4, GsA4divNG4, (primitive, vertices, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfasttf4l, GsTMDfastTF4L, (primitive, vertices, normals, work, count, shift, ordering_table))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfasttf4lfg, GsTMDfastTF4LFG, (primitive, vertices, normals, work, count, shift, ordering_table))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfasttf4nl, GsTMDfastTF4NL, (primitive, vertices, normals, work, count, shift, ordering_table))
GS_BIND_PRIMITIVE(gs_dispatch_a4divtf4l, GsA4divTF4L, (primitive, vertices, normals, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_a4divtf4lfg, GsA4divTF4LFG, (primitive, vertices, normals, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_a4divtf4nl, GsA4divTF4NL, (primitive, vertices, normals, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfasttnf4, GsTMDfastTNF4, (primitive, vertices, work, count, shift, ordering_table))
GS_BIND_PRIMITIVE(gs_dispatch_a4divtnf4, GsA4divTNF4, (primitive, vertices, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfasttg4l, GsTMDfastTG4L, (primitive, vertices, normals, work, count, shift, ordering_table))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfasttg4lfg, GsTMDfastTG4LFG, (primitive, vertices, normals, work, count, shift, ordering_table))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfasttg4nl, GsTMDfastTG4NL, (primitive, vertices, normals, work, count, shift, ordering_table))
GS_BIND_PRIMITIVE(gs_dispatch_a4divtg4l, GsA4divTG4L, (primitive, vertices, normals, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_a4divtg4lfg, GsA4divTG4LFG, (primitive, vertices, normals, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_a4divtg4nl, GsA4divTG4NL, (primitive, vertices, normals, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfasttng4, GsTMDfastTNG4, (primitive, vertices, work, count, shift, ordering_table))
GS_BIND_PRIMITIVE(gs_dispatch_a4divtng4, GsA4divTNG4, (primitive, vertices, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfastf3gl, GsTMDfastF3GL, (primitive, vertices, normals, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfastf3glfg, GsTMDfastF3GLFG, (primitive, vertices, normals, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfastf3gnl, GsTMDfastF3GNL, (primitive, vertices, normals, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfastg3gl, GsTMDfastG3GL, (primitive, vertices, normals, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfastg3glfg, GsTMDfastG3GLFG, (primitive, vertices, normals, work, count, shift, ordering_table, scratch))
GS_BIND_PRIMITIVE(gs_dispatch_tmdfastg3gnl, GsTMDfastG3GNL, (primitive, vertices, normals, work, count, shift, ordering_table, scratch))

#undef GS_BIND_PRIMITIVE

const GsPrimitiveHandler gs_primitive_handlers[70] = {
    gs_dispatch_tmdfastf3l, gs_dispatch_tmdfastf3lfg, gs_dispatch_tmdfastf3nl, gs_dispatch_a4divf3l,   gs_dispatch_a4divf3lfg, gs_dispatch_a4divf3nl,  gs_dispatch_tmdfastnf3,   gs_dispatch_a4divnf3,    gs_dispatch_tmdfastg3l, gs_dispatch_tmdfastg3lfg, gs_dispatch_tmdfastg3nl, gs_dispatch_a4divg3l,   gs_dispatch_a4divg3lfg, gs_dispatch_a4divg3nl,   gs_dispatch_tmdfastng3,    gs_dispatch_a4divng3,     gs_dispatch_tmdfasttf3l, gs_dispatch_tmdfasttf3lfg, gs_dispatch_tmdfasttf3nl, gs_dispatch_a4divtf3l,   gs_dispatch_a4divtf3lfg, gs_dispatch_a4divtf3nl,  gs_dispatch_tmdfasttnf3,   gs_dispatch_a4divtnf3,    gs_dispatch_tmdfasttg3l, gs_dispatch_tmdfasttg3lfg, gs_dispatch_tmdfasttg3nl, gs_dispatch_a4divtg3l,   gs_dispatch_a4divtg3lfg, gs_dispatch_a4divtg3nl,  gs_dispatch_tmdfasttng3,   gs_dispatch_a4divtng3,    gs_dispatch_tmdfastf4l,  gs_dispatch_tmdfastf4lfg,  gs_dispatch_tmdfastf4nl,
    gs_dispatch_a4divf4l,   gs_dispatch_a4divf4lfg,   gs_dispatch_a4divf4nl,   gs_dispatch_tmdfastnf4, gs_dispatch_a4divnf4,   gs_dispatch_tmdfastg4l, gs_dispatch_tmdfastg4lfg, gs_dispatch_tmdfastg4nl, gs_dispatch_a4divg4l,   gs_dispatch_a4divg4lfg,   gs_dispatch_a4divg4nl,   gs_dispatch_tmdfastng4, gs_dispatch_a4divng4,   gs_dispatch_tmdfasttf4l, gs_dispatch_tmdfasttf4lfg, gs_dispatch_tmdfasttf4nl, gs_dispatch_a4divtf4l,   gs_dispatch_a4divtf4lfg,   gs_dispatch_a4divtf4nl,   gs_dispatch_tmdfasttnf4, gs_dispatch_a4divtnf4,   gs_dispatch_tmdfasttg4l, gs_dispatch_tmdfasttg4lfg, gs_dispatch_tmdfasttg4nl, gs_dispatch_a4divtg4l,   gs_dispatch_a4divtg4lfg,   gs_dispatch_a4divtg4nl,   gs_dispatch_tmdfasttng4, gs_dispatch_a4divtng4,   gs_dispatch_tmdfastf3gl, gs_dispatch_tmdfastf3glfg, gs_dispatch_tmdfastf3gnl, gs_dispatch_tmdfastg3gl, gs_dispatch_tmdfastg3glfg, gs_dispatch_tmdfastg3gnl,
};

typedef struct GsNativeState
{
    DRAWENV draw;
    DISPENV display;
    MATRIX world;
    MATRIX screen;
    MATRIX view;
    MATRIX saved_view;
    MATRIX light_directions;
    MATRIX light_colors;
    PSX_RECT clip;
    sint16 buffer_x[2];
    sint16 buffer_y[2];
    sint16 sprite_x[2];
    sint16 sprite_y[2];
    sint16 center_x;
    sint16 center_y;
    sint16 screen_offset_x;
    sint16 screen_offset_y;
    uint32 mode;
    uint32 active;
    uint32 frame;
    uint32 depth_limit;
    uint32 clear_packets[2][4];
} GsNativeState;

static GsNativeState gs_state;

static void gs_apply_clip(void)
{
    gs_state.draw.clip.x = (sint16)(gs_state.clip.x + gs_state.buffer_x[gs_state.active]);
    gs_state.draw.clip.y = (sint16)(gs_state.clip.y + gs_state.buffer_y[gs_state.active]);
    gs_state.draw.clip.w = gs_state.clip.w;
    gs_state.draw.clip.h = gs_state.clip.h;
    PutDrawEnv(&gs_state.draw);
}

static void gs_apply_center(void)
{
    sint32 x, y;
    if ((gs_state.mode & 4) != 0)
    {
        gs_state.screen_offset_x = 0;
        gs_state.screen_offset_y = 0;
        gs_state.draw.ofs[0] = (sint16)(gs_state.center_x + gs_state.buffer_x[gs_state.active]);
        gs_state.draw.ofs[1] = (sint16)(gs_state.center_y + gs_state.buffer_y[gs_state.active]);
        PutDrawEnv(&gs_state.draw);
    }
    else
    {
        x = gs_state.center_x + gs_state.buffer_x[gs_state.active == 0 ? 1 : 0];
        y = gs_state.center_y + gs_state.buffer_y[gs_state.active == 0 ? 1 : 0];
        SetGeomOffset(x, y);
        gs_state.screen_offset_x = (sint16)x;
        gs_state.screen_offset_y = (sint16)y;
    }
}

void gs_set_screen_dimensions(sint32 width, sint32 height)
{
    gs_screen_width = width;
    gs_screen_height = height;
}

void GsSetLightMode(sint32 mode)
{
    if (mode >= 0 && mode <= 3)
        gs_light_mode = mode;
    else
    {
#ifndef NDEBUG
        printf("not supported light mode %d\n", mode);
#endif
    }
}

void gs_sort_object4(uint32 object, uint32 ordering_table, uint32 shift, uint32 scratch)
{
    uint32 value, fog, lighting, model, primitive, remaining, vertices, normals;
    uint32 code, slot, stride, count, flags;
    uint32 *next_work;
    value = r_u32(object);
    if ((sint32)value < 0)
        return;
    flags = r_u32(object);
    gs_sort_attributes.bits3_4 = (value >> 3) & 3;
    value = r_u32(object);
    gs_sort_attributes.bit5 = (flags >> 5) & 1;
    flags = r_u32(object);
    gs_sort_attributes.bit6 = (value >> 6) & 1;
    value = r_u32(object);
    gs_sort_attributes.bits9_11 = (flags >> 9) & 7;
    gs_sort_attributes.bit30 = (value >> 30) & 1;
    fog = gs_sort_attributes.bits9_11 != 0;
    if (fog != 0)
    {
        value = (uint32)gs_screen_width;
        flags = (uint32)gs_screen_height;
        w_u32(scratch, gs_sort_attributes.bits9_11);
        w_u32(scratch + 4, value);
        w_u32(scratch + 8, flags);
    }
    if (gs_sort_attributes.bit6 == 1)
        lighting = 2;
    else if ((gs_sort_attributes.bit5 == 0 && gs_light_mode == 1) || (gs_sort_attributes.bit5 == 1 && gs_sort_attributes.bits3_4 == 1))
        lighting = 1;
    else
        lighting = 0;
    model = r_u32(object + 8);
    primitive = r_u32(model + 16);
    remaining = r_u32(model + 20);
    vertices = r_u32(model);
    normals = r_u32(model + 8);
    while (remaining != 0)
    {
        code = r_u8(primitive + 3) & 0xFD;
        switch (code)
        {
            case 0x20:
                flags = r_u16(primitive + 2);
                if ((flags & 4) != 0)
                {
                    slot = 64 + lighting;
                    stride = 24;
                }
                else
                {
                    slot = lighting + fog * 3;
                    stride = 16;
                }
                break;
            case 0x21:
                slot = 6 + fog;
                stride = 16;
                break;
            case 0x24:
                slot = 16 + lighting + fog * 3;
                stride = 24;
                break;
            case 0x25:
                slot = 22 + fog;
                stride = 28;
                break;
            case 0x28:
                slot = 32 + lighting + fog * 3;
                stride = 20;
                break;
            case 0x29:
                slot = 38 + fog;
                stride = 16;
                break;
            case 0x2C:
                slot = 48 + lighting + fog * 3;
                stride = 32;
                break;
            case 0x2D:
                slot = 54 + fog;
                stride = 32;
                break;
            case 0x30:
                flags = r_u16(primitive + 2);
                if ((flags & 4) != 0)
                {
                    slot = 67 + lighting;
                    stride = 28;
                }
                else
                {
                    slot = 8 + lighting + fog * 3;
                    stride = 20;
                }
                break;
            case 0x31:
                slot = 14 + fog;
                stride = 24;
                break;
            case 0x34:
                slot = 24 + lighting + fog * 3;
                stride = 28;
                break;
            case 0x35:
                slot = 30 + fog;
                stride = 36;
                break;
            case 0x38:
                slot = 40 + lighting + fog * 3;
                stride = 24;
                break;
            case 0x39:
                slot = 46 + fog;
                stride = 28;
                break;
            case 0x3C:
                slot = 56 + lighting + fog * 3;
                stride = 36;
                break;
            case 0x3D:
                slot = 62 + fog;
                stride = 44;
                break;
            default:
#ifndef NDEBUG
                printf("non supported code %x %x\n", code, primitive);
#endif
                continue;
        }
        count = r_u16(primitive);
        next_work = gs_primitive_handlers[slot](&gs_sort_attributes, primitive, vertices, normals, GsGetWorkBase(), count, shift, ordering_table, scratch);
        GsSetWorkBase(next_work);
        count = r_u16(primitive);
        remaining -= count;
        primitive += count * stride;
    }
}

static MATRIX *gte_gs_light_directions;
static MATRIX *gte_gs_light_colors;

void gte_bind_gs_light_matrices(MATRIX *directions, MATRIX *colors)
{
    gte_gs_light_directions = directions;
    gte_gs_light_colors = colors;
}

void GsSetLightMatrix(const MATRIX *matrix)
{
    MATRIX light;
    if (matrix == NULL || gte_gs_light_directions == NULL)
        abort();
    light = *gte_gs_light_directions;
    PushMatrix();
    MulMatrix0(&light, (MATRIX *)matrix, &light);
    PopMatrix();
    SetLightMatrix(&light);
}

void GsSetAmbient(sint32 red, sint32 green, sint32 blue)
{
    SetBackColor(red >> 4, green >> 4, blue >> 4);
}

void GsSetFogParam(const void *parameters)
{
    const uint8 *fog = (const uint8 *)parameters;
    xport_gte_write_control(27, (uint32)(sint32)(sint16)xport_load_le16(fog));
    xport_gte_write_control(28, xport_load_le32(fog + 4));
    SetFarColor(fog[8], fog[9], fog[10]);
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

static void gs_compose_matrices(MATRIX *left, MATRIX *right, MATRIX *destination)
{
    VECTOR input, translation;
    input.vx = right->t[0];
    input.vy = right->t[1];
    input.vz = right->t[2];
    ApplyMatrixLV(left, &input, &translation);
    MulMatrix0(left, right, destination);
    destination->t[0] = (sint32)((uint32)translation.vx + (uint32)left->t[0]);
    destination->t[1] = (sint32)((uint32)translation.vy + (uint32)left->t[1]);
    destination->t[2] = (sint32)((uint32)translation.vz + (uint32)left->t[2]);
}

static void gs_get_local_world(uint32 coordinate, MATRIX *world)
{
    uint32 path[100];
    uint32 depth = 0, last_dirty = 100, parent, flag, unwind, node;
    MATRIX local;
    for (;;)
    {
        if (depth >= sizeof(path) / sizeof(path[0]))
            abort();
        path[depth] = coordinate;
        parent = r_u32(coordinate + 72);
        flag = r_u32(coordinate);
        if (parent == 0)
        {
            if (flag == gs_state.frame || flag == 0)
            {
                memcpy(world, psx_addr(coordinate + 4, sizeof(*world)), sizeof(*world));
                if (!xport_guest_copy(xport_guest_ref(coordinate + 36), xport_host_ref(world), sizeof(*world)))
                    abort();
                w_u32(coordinate, gs_state.frame);
                unwind = depth;
            }
            else if (last_dirty == 100)
            {
                memcpy(world, psx_addr(path[0] + 36, sizeof(*world)), sizeof(*world));
                unwind = 0;
            }
            else
            {
                unwind = last_dirty + 1;
                memcpy(world, psx_addr(path[unwind] + 36, sizeof(*world)), sizeof(*world));
            }
            break;
        }
        if (flag == gs_state.frame)
        {
            memcpy(world, psx_addr(coordinate + 36, sizeof(*world)), sizeof(*world));
            unwind = depth;
            break;
        }
        if (flag == 0)
            last_dirty = depth;
        coordinate = parent;
        ++depth;
    }
    while (unwind != 0)
    {
        node = path[--unwind];
        memcpy(&local, psx_addr(node + 4, sizeof(local)), sizeof(local));
        gs_compose_matrices(world, &local, world);
        if (!xport_guest_copy(xport_guest_ref(node + 36), xport_host_ref(world), sizeof(*world)))
            abort();
        w_u32(node, gs_state.frame);
    }
}

static void gs_view_coordinates(uint32 view, sint32 coordinates[6])
{
    sint32 maximum, magnitude, value;
    uint32 index, bits = 0, shift;
    maximum = r_s32(view);
    if (maximum < 0)
        maximum = (sint32)(0 - (uint32)maximum);
    for (index = 1; index < 6; ++index)
    {
        magnitude = r_s32(view + index * 4);
        if (magnitude < 0)
            magnitude = (sint32)(0 - (uint32)magnitude);
        if (magnitude > maximum)
            maximum = magnitude;
    }
    for (value = maximum; value > 0; value >>= 1)
        ++bits;
    shift = bits >= 16 ? bits - 15 : 0;
    for (index = 0; index < 6; ++index)
        coordinates[index] = r_s32(view + index * 4) >> shift;
}

sint32 GsSetRefView2(uint32 view)
{
    MATRIX rotation = {0}, parent_world, inverse = {0};
    VECTOR input, output;
    sint32 coordinates[6], roll, angle, sine, cosine;
    uint32 dx, dy, dz, horizontal, length, parent;
    sint16 sine_axis, cosine_axis;
    gs_state.view = gs_state.screen;
    roll = (sint32)(0 - r_u32(view + 24));
    angle = roll / 360;
    cosine = rcos(angle);
    sine = rsin(angle);
    if (roll != 0)
    {
        rotation.m[0][0] = (sint16)cosine;
        rotation.m[0][1] = (sint16)(0 - (uint32)sine);
        rotation.m[1][0] = (sint16)sine;
        rotation.m[1][1] = (sint16)cosine;
        rotation.m[2][2] = 4096;
        MulMatrix0(&gs_state.view, &rotation, &gs_state.view);
    }
    gs_view_coordinates(view, coordinates);
    dx = (uint32)coordinates[3] - (uint32)coordinates[0];
    dy = (uint32)coordinates[4] - (uint32)coordinates[1];
    dz = (uint32)coordinates[5] - (uint32)coordinates[2];
    length = SquareRoot0((sint32)(dx * dx + dy * dy + dz * dz));
    if (length == 0)
        return 1;
    sine_axis = (sint16)(0 - (uint32)((sint32)((0 - dy) << 12) / (sint32)length));
    horizontal = SquareRoot0((sint32)(dx * dx + dz * dz));
    cosine_axis = (sint16)((sint32)(horizontal << 12) / (sint32)length);
    rotation = gs_state.world;
    rotation.m[1][1] = cosine_axis;
    rotation.m[2][2] = cosine_axis;
    rotation.m[1][2] = (sint16)(0 - (uint32)sine_axis);
    rotation.m[2][1] = sine_axis;
    MulMatrix0(&gs_state.view, &rotation, &gs_state.view);
    if (horizontal != 0)
    {
        sine_axis = (sint16)(0 - (uint32)((sint32)(dx << 12) / (sint32)horizontal));
        cosine_axis = (sint16)((sint32)(dz << 12) / (sint32)horizontal);
        rotation = gs_state.world;
        rotation.m[0][0] = cosine_axis;
        rotation.m[2][2] = cosine_axis;
        rotation.m[0][2] = sine_axis;
        rotation.m[2][0] = (sint16)(0 - (uint32)sine_axis);
        MulMatrix0(&gs_state.view, &rotation, &gs_state.view);
    }
    input.vx = (sint32)(0 - r_u32(view));
    input.vy = (sint32)(0 - r_u32(view + 4));
    input.vz = (sint32)(0 - r_u32(view + 8));
    ApplyMatrixLV(&gs_state.view, &input, &output);
    gs_state.view.t[0] = output.vx;
    gs_state.view.t[1] = output.vy;
    gs_state.view.t[2] = output.vz;
    parent = r_u32(view + 28);
    if (parent != 0)
    {
        gs_get_local_world(parent, &parent_world);
        TransposeMatrix(&parent_world, &inverse);
        input.vx = parent_world.t[0];
        input.vy = parent_world.t[1];
        input.vz = parent_world.t[2];
        ApplyMatrixLV(&inverse, &input, &output);
        inverse.t[0] = (sint32)(0 - (uint32)output.vx);
        inverse.t[1] = (sint32)(0 - (uint32)output.vy);
        inverse.t[2] = (sint32)(0 - (uint32)output.vz);
        gs_compose_matrices(&gs_state.view, &inverse, &inverse);
        gs_state.view = inverse;
    }
    gs_state.saved_view = gs_state.view;
    return 0;
}

void GsGetLs(uint32 coordinate, MATRIX *screen)
{
    gs_get_local_world(coordinate, screen);
    gs_compose_matrices(&gs_state.view, screen, screen);
}

void GsGetLws(uint32 coordinate, MATRIX *world, MATRIX *screen)
{
    gs_get_local_world(coordinate, world);
    if (screen != world)
        memcpy(screen, world, sizeof(*screen));
    gs_compose_matrices(&gs_state.view, screen, screen);
}

void GsInitGraph(uint32 width, uint32 height, uint32 mode, uint32 dither, uint32 rgb24)
{
    uint32 index;
    width = (uint16)width;
    height = (uint16)height;
    mode = (uint16)mode;
    SetDispMask(0);
    gs_state.draw.ofs[0] = 0;
    gs_state.draw.ofs[1] = 0;
    memset(&gs_state.draw.tw, 0, sizeof(gs_state.draw.tw));
    gs_state.draw.tpage = 0;
    gs_state.draw.dtd = (uint8)dither;
    gs_state.draw.dfe = 0;
    gs_state.draw.isbg = 0;
    PutDrawEnv(&gs_state.draw);
    gs_state.display.disp.x = 0;
    gs_state.display.disp.y = 0;
    gs_state.display.disp.w = (sint16)width;
    gs_state.display.disp.h = (sint16)height;
    memset(&gs_state.display.screen, 0, sizeof(gs_state.display.screen));
    if (GetVideoMode() == 1)
    {
        gs_state.display.screen.y = 24;
        gs_state.display.pad0 = 1;
    }
    gs_state.display.isinter = (uint8)(mode & 1);
    gs_state.display.isrgb24 = (uint8)rgb24;
    gs_state.mode = mode & 4;
    PutDispEnv(&gs_state.display);
    InitGeom();
    SetFarColor(0, 0, 0);
    SetGeomOffset(0, 0);
    gs_state.screen_offset_x = 0;
    gs_state.screen_offset_y = 0;
    gs_state.active = 0;
    gs_set_screen_dimensions((sint32)width, (sint32)height);
    if (width == 0)
        abort();
    gs_state.world = psx_identity_matrix;
    gs_state.screen = gs_state.world;
    gs_state.screen.m[1][1] = (sint16)((height << 14) / width / 3);
    memset(&gs_state.light_directions, 0, sizeof(gs_state.light_directions));
    memset(&gs_state.light_colors, 0, sizeof(gs_state.light_colors));
    if (gte_gs_light_directions == NULL || gte_gs_light_colors == NULL)
        gte_bind_gs_light_matrices(&gs_state.light_directions, &gs_state.light_colors);
    else
    {
        *gte_gs_light_directions = gs_state.light_directions;
        *gte_gs_light_colors = gs_state.light_colors;
    }
    memset(gs_state.sprite_x, 0, sizeof(gs_state.sprite_x));
    memset(gs_state.sprite_y, 0, sizeof(gs_state.sprite_y));
    gs_state.center_x = 0;
    gs_state.center_y = 0;
    gs_state.clip.x = 0;
    gs_state.clip.y = 0;
    gs_state.clip.w = (sint16)width;
    gs_state.clip.h = (sint16)height;
    for (index = 0; index < 2; ++index)
    {
        gs_state.clear_packets[index][0] = (gs_state.clear_packets[index][0] & 0xFFFFFF) | 0x03000000;
        gs_state.clear_packets[index][1] = (gs_state.clear_packets[index][1] & 0xFFFFFF) | 0x02000000;
    }
    gs_state.frame = 1;
    gs_apply_clip();
    gs_apply_center();
}

void GsDefDispBuff(uint32 x0, uint32 y0, uint32 x1, uint32 y1)
{
    gs_state.buffer_x[0] = (sint16)x0;
    gs_state.buffer_x[1] = (sint16)x1;
    gs_state.buffer_y[0] = (sint16)y0;
    gs_state.buffer_y[1] = (sint16)y1;
    if ((gs_state.mode & 4) != 0)
    {
        gs_state.sprite_x[0] = 0;
        gs_state.sprite_x[1] = 0;
        gs_state.sprite_y[0] = 0;
        gs_state.sprite_y[1] = 0;
    }
    else
    {
        gs_state.sprite_x[0] = (sint16)x0;
        gs_state.sprite_x[1] = (sint16)x1;
        gs_state.sprite_y[0] = (sint16)y0;
        gs_state.sprite_y[1] = (sint16)y1;
    }
    gs_apply_clip();
    gs_apply_center();
}

sint32 GsInit3D(void)
{
    gs_state.center_x = (sint16)(gs_screen_width / 2);
    gs_state.center_y = (sint16)(gs_screen_height / 2);
    gs_apply_center();
    gs_set_clip_distance(10);
    gs_light_mode = 0;
    gs_state.depth_limit = 0x3FFF;
    return 0x3FFF;
}

uint32 GsGetActiveBuff(void)
{
    return gs_state.active;
}

void GsSortClear(uint8 red, uint8 green, uint8 blue, void *ordering_table)
{
    const uint8 *descriptor = (const uint8 *)ordering_table;
    uint32 *packet = gs_state.clear_packets[gs_state.active];
    uint32 width = (uint32)gs_screen_width;
    if (!gpu_register_packet_range(gs_state.clear_packets, sizeof(gs_state.clear_packets)))
        abort();
    xport_store_u8((uint8 *)packet + 4, red);
    xport_store_u8((uint8 *)packet + 5, green);
    xport_store_u8((uint8 *)packet + 6, blue);
    xport_store_le16((uint8 *)packet + 8, (uint16)gs_state.buffer_x[gs_state.active]);
    xport_store_le16((uint8 *)packet + 14, (uint16)gs_screen_height);
    xport_store_le16((uint8 *)packet + 10, (uint16)gs_state.buffer_y[gs_state.active]);
    if (gs_state.display.isrgb24 != 0)
        width = width * 3 / 2;
    xport_store_le16((uint8 *)packet + 12, (uint16)width);
    AddPrim(psx_addr(xport_load_le32(descriptor + 16), 4), packet);
}

static uint32 *gs_work_packet(uint32 words)
{
    uintptr_t begin = (uintptr_t)&DRAM[0];
    uintptr_t cursor = (uintptr_t)gs_work_cursor;
    if (gs_work_cursor == NULL)
        abort();
    if (cursor >= begin && cursor - begin < PSX_DRAM_SIZE)
        return (uint32 *)psx_addr((uint32)(cursor - begin), words * 4);
    return gs_work_cursor;
}

static void gs_sort_packet(uint32 *packet, const void *ordering_table, uint16 priority, uint8 words, sint32 add_length)
{
    const uint8 *descriptor = (const uint8 *)ordering_table;
    uint32 depth = (uint32)priority - xport_load_le32(descriptor + 8);
    uint32 slot = xport_load_le32(descriptor + 4) + depth * 4;
#ifndef NDEBUG
    if (!add_length && (sint32)depth < 0)
        printf("ps_sort_sprite,bg: z resolution overflow\n");
#endif
    xport_store_le32(packet, ((uint32)words + (add_length ? r_u32(slot) >> 24 : 0)) << 24);
    AddPrim(psx_addr(slot, 4), packet);
    w_u8(slot + 3, 0);
    gs_work_cursor = packet + (uint32)words + 1;
}

void GsSortBoxFill(const void *box, void *ordering_table, uint32 priority)
{
    const uint8 *descriptor = (const uint8 *)box;
    uint32 attribute = xport_load_le32(descriptor);
    uint32 *packet;
    uint8 *output;
    if ((sint32)attribute < 0)
        return;
    packet = gs_work_packet(5);
    output = (uint8 *)packet;
    xport_store_le32(output + 4, 0xE1000200 | ((attribute >> 17) & 0x180) | ((attribute >> 23) & 0x60));
    xport_store_u8(output + 8, descriptor[12]);
    xport_store_u8(output + 9, descriptor[13]);
    xport_store_u8(output + 11, (uint8)(0x60 | ((attribute >> 29) & 2)));
    xport_store_u8(output + 10, descriptor[14]);
    xport_store_le16(output + 12, (uint16)(xport_load_le16(descriptor + 4) + gs_state.screen_offset_x));
    xport_store_le16(output + 14, (uint16)(xport_load_le16(descriptor + 6) + gs_state.screen_offset_y));
    xport_store_le16(output + 16, xport_load_le16(descriptor + 8));
    xport_store_le16(output + 18, xport_load_le16(descriptor + 10));
    gs_sort_packet(packet, ordering_table, (uint16)priority, 4, 0);
}

void GsSortFastSprite(const void *sprite, void *ordering_table, uint32 priority)
{
    const uint8 *descriptor = (const uint8 *)sprite;
    uint32 attribute = xport_load_le32(descriptor);
    uint32 *packet;
    uint32 mode, x, y, red, green, blue, uv, clut_y, clut_x, width, height;
    if ((sint32)attribute < 0 || xport_load_le16(descriptor + 8) == 0 || xport_load_le16(descriptor + 10) == 0)
        return;
    mode = (xport_load_le16(descriptor + 12) & 0x1F) | ((attribute >> 17) & 0x180) | ((attribute >> 23) & 0x60) | 0xE1000200;
    y = xport_load_le16(descriptor + 6);
    x = xport_load_le16(descriptor + 4);
    packet = gs_work_packet(6);
    xport_store_le32(packet + 1, mode);
    blue = descriptor[22];
    green = descriptor[21];
    red = descriptor[20];
    x += (uint16)gs_state.screen_offset_x;
    y += (uint16)gs_state.screen_offset_y;
    xport_store_le32(packet + 3, (x & 0xFFFF) | (y << 16));
    xport_store_le32(packet + 2, 0x64000000 | ((attribute >> 5) & 0x02000000) | ((attribute << 18) & 0x01000000) | (blue << 16) | (green << 8) | red);
    uv = descriptor[14] | ((uint32)descriptor[15] << 8);
    clut_y = (uint32)(sint32)(sint16)xport_load_le16(descriptor + 18) << 22;
    clut_x = ((uint32)(sint32)(sint16)xport_load_le16(descriptor + 16) << 12) & 0x003F0000;
    xport_store_le32(packet + 4, uv | clut_y | clut_x);
    height = xport_load_le16(descriptor + 10);
    width = xport_load_le16(descriptor + 8);
    xport_store_le32(packet + 5, width | (height << 16));
    gs_sort_packet(packet, ordering_table, (uint16)priority, 5, 1);
}

void GsSortSprite(const void *sprite, void *ordering_table, uint32 priority)
{
    const uint8 *descriptor = (const uint8 *)sprite;
    uint32 attribute = xport_load_le32(descriptor);
    uint32 scale_word, rotation_word, simple, *packet, color, clut;
    uint32 u0, u1, v0, v1, tpage;
    sint32 x, y, origin_x, origin_y, angle, high, projection, flags;
    uint16 width, height;
    MATRIX matrix = {0};
    VECTOR vector;
    SVECTOR angles = {0}, vertices[4] = {{0}};
    sint32 screen[4];
    if ((sint32)attribute < 0 || xport_load_le16(descriptor + 8) == 0 || xport_load_le16(descriptor + 10) == 0)
        return;
    scale_word = xport_load_le32(descriptor + 28);
    rotation_word = xport_load_le32(descriptor + 32);
    simple = (attribute >> 27) & 1;
    if (scale_word == 0x10001000 && rotation_word == 0 && (attribute & 0x00C00000) == 0)
        simple = 1;
    if (simple != 0)
    {
        packet = gs_work_packet(6);
        x = (sint16)xport_load_le16(descriptor + 4);
        y = (sint16)xport_load_le16(descriptor + 6);
        xport_store_le32(packet + 1, 0xE1000200 | (xport_load_le16(descriptor + 12) & 0x1F) | ((attribute >> 17) & 0x180) | ((attribute >> 23) & 0x60));
        color = 0x64000000 | ((attribute >> 5) & 0x02000000) | ((attribute << 18) & 0x01000000) | ((uint32)descriptor[22] << 16) | ((uint32)descriptor[21] << 8) | descriptor[20];
        xport_store_le32(packet + 2, color);
        origin_x = (sint16)xport_load_le16(descriptor + 24);
        origin_y = (sint16)xport_load_le16(descriptor + 26);
        x = x + gs_state.screen_offset_x - origin_x;
        y = y + gs_state.screen_offset_y - origin_y;
        xport_store_le32(packet + 3, ((uint32)x & 0xFFFF) | ((uint32)y << 16));
        clut = ((uint32)(sint32)(sint16)xport_load_le16(descriptor + 18) << 22) | (((uint32)(sint32)(sint16)xport_load_le16(descriptor + 16) << 12) & 0x003F0000);
        xport_store_le32(packet + 4, xport_load_le16(descriptor + 14) | clut);
        xport_store_le32(packet + 5, xport_load_le16(descriptor + 8) | ((uint32)xport_load_le16(descriptor + 10) << 16));
        gs_sort_packet(packet, ordering_table, (uint16)priority, 5, 0);
        return;
    }
    packet = gs_work_packet(10);
    if (rotation_word == 0)
        matrix = gs_state.world;
    else
    {
        angle = (sint32)rotation_word;
        high = (sint32)(((sint64)angle * (sint32)0xB60B60B7) >> 32);
        angles.vz = (sint16)(((sint32)((uint32)high + rotation_word) >> 8) - (angle >> 31));
        RotMatrix(&angles, &matrix);
    }
    if (xport_load_le32(descriptor + 28) != 0x10001000)
    {
        vector.vx = (sint16)xport_load_le16(descriptor + 28);
        vector.vy = (sint16)xport_load_le16(descriptor + 30);
        vector.vz = 0;
        ScaleMatrix(&matrix, &vector);
    }
    vector.vx = (sint16)xport_load_le16(descriptor + 4);
    vector.vy = (sint16)xport_load_le16(descriptor + 6);
    vector.vz = (sint32)xport_gte_read_control(26);
    TransMatrix(&matrix, &vector);
    SetRotMatrix(&matrix);
    SetTransMatrix(&matrix);
    origin_x = (sint16)xport_load_le16(descriptor + 24);
    origin_y = (sint16)xport_load_le16(descriptor + 26);
    width = xport_load_le16(descriptor + 8);
    height = xport_load_le16(descriptor + 10);
    vertices[0].vx = (sint16)-origin_x;
    vertices[0].vy = (sint16)-origin_y;
    vertices[1].vx = (sint16)(width - origin_x);
    vertices[1].vy = (sint16)-origin_y;
    vertices[2].vx = (sint16)-origin_x;
    vertices[2].vy = (sint16)(height - origin_y);
    vertices[3].vx = (sint16)(width - origin_x);
    vertices[3].vy = (sint16)(height - origin_y);
    RotTransPers4(&vertices[0], &vertices[1], &vertices[2], &vertices[3], &screen[0], &screen[1], &screen[2], &screen[3], &projection, &flags);
    u0 = descriptor[14];
    u1 = u0 + descriptor[8] - 1;
    if ((attribute & 0x00800000) != 0)
    {
        u1 = descriptor[14];
        u0 = u1 + descriptor[8] - 1;
    }
    v0 = descriptor[15];
    v1 = v0 + descriptor[10] - 1;
    if ((attribute & 0x00400000) != 0)
    {
        v1 = descriptor[15];
        v0 = v1 + descriptor[10] - 1;
    }
    u0 &= 255;
    u1 &= 255;
    v0 = (v0 & 255) << 8;
    v1 = (v1 & 255) << 8;
    color = 0x2C000000 | ((attribute >> 5) & 0x02000000) | ((attribute << 18) & 0x01000000) | ((uint32)descriptor[22] << 16) | ((uint32)descriptor[21] << 8) | descriptor[20];
    xport_store_le32(packet + 2, (uint32)screen[0]);
    xport_store_le32(packet + 1, color);
    clut = ((uint32)(sint32)(sint16)xport_load_le16(descriptor + 18) << 22) | (((uint32)(sint32)(sint16)xport_load_le16(descriptor + 16) << 12) & 0x003F0000);
    xport_store_le32(packet + 4, (uint32)screen[1]);
    xport_store_le32(packet + 3, u0 | v0 | clut);
    tpage = (xport_load_le16(descriptor + 12) & 0x1F) << 16;
    xport_store_le32(packet + 6, (uint32)screen[2]);
    xport_store_le32(packet + 7, u0 | v1);
    xport_store_le32(packet + 8, (uint32)screen[3]);
    xport_store_le32(packet + 9, u1 | v1);
    xport_store_le32(packet + 5, u1 | v0 | tpage | ((attribute >> 1) & 0x01800000) | ((attribute >> 7) & 0x00600000));
    gs_sort_packet(packet, ordering_table, (uint16)priority, 9, 0);
}

uint32 GsSwapDispBuff(void)
{
    gs_state.display.disp.x = gs_state.buffer_x[gs_state.active];
    gs_state.display.disp.y = gs_state.buffer_y[gs_state.active];
    PutDispEnv(&gs_state.display);
    DrawSync(1);
    gs_state.active = gs_state.active == 0 ? 1 : 0;
    gs_apply_clip();
    gs_apply_center();
    ++gs_state.frame;
    return (uint32)-40;
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

static PsxGuestDispatch native_guest_dispatch;

void psx_bind_guest_dispatch(PsxGuestDispatch dispatch)
{
    native_guest_dispatch = dispatch;
}

uint32 psx_dispatch_guest_call(uint32 target, uint32 argument_count, uint32 argument0, uint32 argument1, uint32 argument2)
{
    if (!native_guest_dispatch)
    {
#ifndef NDEBUG
        fprintf(stderr, "Unbound guest dispatch %08X/%u\n", target, argument_count);
#else
        (void)0;
#endif
        abort();
    }
    return native_guest_dispatch(target, argument_count, argument0, argument1, argument2);
}

uint32 xport_guest_call0(uint32 target)
{
    return psx_dispatch_guest_call(target, 0, 0, 0, 0);
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
    else if (register_index == 28)
    {
        gte.ir[0] = (value & 31) * 128;
        gte.ir[1] = ((value >> 5) & 31) * 128;
        gte.ir[2] = ((value >> 10) & 31) * 128;
    }
    else if (register_index == 29)
    {
        /* ORGB is read only */
    }
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
    if (register_index == 28 || register_index == 29)
    {
        uint32 color = 0, index;
        for (index = 0; index < 3; ++index)
        {
            sint32 component = gte.ir[index] / 128;
            if (component < 0)
                component = 0;
            else if (component > 31)
                component = 31;
            color |= (uint32)component << (index * 5);
        }
        return color;
    }
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

sint32 psx_root_counter_stop(uint32 counter)
{
    static const uint32 interrupt_masks[4] = {0x10, 0x20, 0x40, 1};
    uint32 index = counter & 0xFFFFu;
    uint32 value;
    if (index >= sizeof(interrupt_masks) / sizeof(interrupt_masks[0]))
    {
#ifndef NDEBUG
        fprintf(stderr, "xport: Invalid native root counter selector %u\n", index);
#else
        (void)0;
#endif
        abort();
    }
    value = r_u32(0x1F801074u);
    w_u32(0x1F801074u, value & ~interrupt_masks[index]);
    return 1;
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

uint32 WaitEventPSX(uint32 handle)
{
    uint32 base = r_u32(0xA0000120);
    uint32 slot = base + (handle & 0xFFFF) * 28 + 4;
    uint32 status = r_u32(slot);
    if (status == 0x4000)
    {
        w_u32(slot, 0x2000);
        return 1;
    }
    status = r_u32(slot);
    if (status != 0x2000)
        return 0;
    while (r_u32(slot) != 0x4000)
    {
        if (!psx_wait_events())
        {
#ifndef NDEBUG
            fprintf(stderr, "WaitEventPSX cannot advance event %08X\n", handle);
#else
            (void)0;
#endif
            abort();
        }
    }
    w_u32(slot, 0x2000);
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

static PsxDmaState native_bus_dma = {{{0}, {0}, {0}, {0}, {0}, {0}, {0, 0, 2, 0}}, 0x07654321, 0, 0};

static MdecState native_mdec;
static uint32 native_mdec_servicing;
static uint32 native_mdec_words[65536];
static uint32 native_gpu_servicing;

static void native_gpu_request(void)
{
    uint32 status;
    if (!gpu_port_read(4, &status))
        abort();
    native_bus_dma.channels[2].request = (status >> 25) & 1;
    if (!psx_irq_line(&native_root_irq, 1, (status >> 24) & 1))
        abort();
}

static sint32 native_gpu_transfer(void)
{
    PsxDmaChannel *dma = &native_bus_dma.channels[2];
    uint32 address = dma->base & 0xFFFFFC;
    uint32 mode = (dma->control >> 9) & 3;
    uint32 reverse = dma->control & 2, output = dma->control & 1;
    uint32 count = dma->block & 0xFFFF, blocks = dma->block >> 16;
    uint32 tag = 0, index, value, physical;
    if (mode == 3 || (mode == 2 && !output))
        return -1;
    dma->control &= ~0x10000000;
    if (mode == 2)
    {
        if (address >= 0x800000)
            return psx_dma_bus_error(&native_bus_dma, &native_root_irq, 2);
        tag = r_u32(0x80000000 | (address & (PSX_DRAM_SIZE - 1)));
        count = tag >> 24;
        address += 4;
        reverse = 0;
    }
    else if (!count)
        count = 65536;
    if (count && (address >= 0x800000 || (reverse ? (count - 1) * 4 > address : (count - 1) * 4 >= 0x800000 - address)))
        return psx_dma_bus_error(&native_bus_dma, &native_root_irq, 2);
    for (index = 0; index < count; ++index)
    {
        physical = (address + (reverse ? 0 - index * 4 : index * 4)) & (PSX_DRAM_SIZE - 1);
        if (output)
        {
            value = r_u32(0x80000000 | physical);
            if (!gpu_port_write(0, value))
                return -1;
        }
        else
        {
            if (!gpu_port_read(0, &value))
                return -1;
            if (!w_u32(0x80000000 | physical, value))
                return psx_dma_bus_error(&native_bus_dma, &native_root_irq, 2);
            if (xport_device_trace_enabled() && !psx_dma_trace_write(&native_bus_dma, 2, XPORT_DEVICE_REGION_NONE, 0, XPORT_DEVICE_REGION_RAM, physical, 4, 15))
                return psx_dma_bus_error(&native_bus_dma, &native_root_irq, 2);
        }
    }
    native_gpu_request();
    if (mode == 2)
    {
        dma->base = tag & 0xFFFFFF;
        if (dma->base != 0xFFFFFF)
            return 1;
    }
    else if (mode == 1)
    {
        dma->base = (address + (reverse ? 0 - count * 4 : count * 4)) & 0xFFFFFF;
        if (!blocks)
            blocks = 65536;
        --blocks;
        dma->block = (dma->block & 0xFFFF) | (blocks << 16);
        if (blocks)
            return 1;
    }
    return psx_dma_complete(&native_bus_dma, &native_root_irq, 2);
}

static void native_gpu_service(void)
{
    uint32 nodes = 0;
    if (native_gpu_servicing)
        return;
    native_gpu_servicing = 1;
    native_gpu_request();
    while (psx_dma_ready(&native_bus_dma, 2, 0, 0) == 1)
    {
        if (++nodes > PSX_DRAM_SIZE / 4)
        {
#ifndef NDEBUG
            fputs("Native GPU DMA did not terminate within RAM tag capacity\n", stderr);
#else
            (void)0;
#endif
            abort();
        }
        dma_trace_start(&native_bus_dma, 2);
        if (native_gpu_transfer() != 1)
        {
#ifndef NDEBUG
            fputs("Unsupported native GPU DMA transfer\n", stderr);
#else
            (void)0;
#endif
            abort();
        }
    }
    native_gpu_servicing = 0;
}

static void native_mdec_requests(void)
{
    native_bus_dma.channels[0].request = (native_mdec.status >> 28) & 1;
    native_bus_dma.channels[1].request = (native_mdec.status >> 27) & 1;
}

static sint32 native_mdec_transfer(uint32 channel)
{
    PsxDmaChannel *dma = &native_bus_dma.channels[channel];
    uint32 mode = (dma->control >> 9) & 3;
    uint32 address = dma->base & 0xFFFFFC;
    uint32 reverse = dma->control & 2;
    uint32 count = dma->block & 0xFFFF;
    uint32 blocks = dma->block >> 16;
    uint32 index, copied;
    if (mode > 1 || ((dma->control & 1) != (channel == 0)))
    {
#ifndef NDEBUG
        fprintf(stderr, "Unsupported native MDEC DMA channel%u control%08X\n", channel, dma->control);
#else
        (void)0;
#endif
        return -1;
    }
    if (!count)
        count = 65536;
    if (!blocks)
        blocks = 65536;
    dma->control &= ~0x10000000;
    if (address >= 0x800000 || (reverse ? (count - 1) * 4 > address : (count - 1) * 4 >= 0x800000 - address))
        return psx_dma_bus_error(&native_bus_dma, &native_root_irq, channel);
    if (!channel)
    {
        for (index = 0; index < count; ++index)
            native_mdec_words[index] = r_u32(0x80000000 | ((address + (reverse ? 0 - index * 4 : index * 4)) & (PSX_DRAM_SIZE - 1)));
        copied = mdec_dma_write(&native_mdec, native_mdec_words, count);
        if (copied > count * 2)
            abort();
    }
    else
    {
        copied = mdec_dma_read(&native_mdec, native_mdec_words, count);
        for (index = 0; index < copied; ++index)
        {
            uint32 destination = (address + (reverse ? 0 - index * 4 : index * 4)) & (PSX_DRAM_SIZE - 1);
            if (!w_u32(0x80000000 | destination, native_mdec_words[index]))
                return psx_dma_bus_error(&native_bus_dma, &native_root_irq, channel);
            if (xport_device_trace_enabled() && !psx_dma_trace_write(&native_bus_dma, channel, XPORT_DEVICE_REGION_NONE, 0, XPORT_DEVICE_REGION_RAM, destination, 4, 15))
                return psx_dma_bus_error(&native_bus_dma, &native_root_irq, channel);
        }
    }
    native_mdec_requests();
    if (mode == 1)
    {
        dma->base = (address + (reverse ? 0 - count * 4 : count * 4)) & 0xFFFFFF;
        --blocks;
        dma->block = (dma->block & 0xFFFF) | (blocks << 16);
        if (blocks)
            return 1;
    }
    return psx_dma_complete(&native_bus_dma, &native_root_irq, channel);
}

static void native_mdec_advance(uint64 ticks)
{
    uint32 channel, progress, step;
    if (native_mdec_servicing)
        return;
    native_mdec_servicing = 1;
    for (;;)
    {
        do
        {
            progress = 0;
            native_mdec_requests();
            for (channel = 0; channel < 2; ++channel)
            {
                if (psx_dma_ready(&native_bus_dma, channel, 0, 0) == 1)
                {
                    dma_trace_start(&native_bus_dma, channel);
                    if (native_mdec_transfer(channel) != 1)
                        abort();
                    progress = 1;
                }
            }
        } while (progress);
        if (!ticks || !native_mdec.copy_ticks)
            break;
        step = ticks < native_mdec.copy_ticks ? (uint32)ticks : native_mdec.copy_ticks;
        mdec_advance(&native_mdec, step);
        ticks -= step;
    }
    native_mdec_servicing = 0;
}

static sint32 native_bus_dma_transfer(void *context, uint32 channel)
{
    uint32 address, count, index, next, value;
    (void)context;
    if (channel < 2)
        return native_mdec_transfer(channel);
    if (channel == 2)
        return native_gpu_transfer();
    if (channel == 6)
    {
        address = native_bus_dma.channels[6].base & 0xFFFFFC;
        count = native_bus_dma.channels[6].block & 0xFFFF;
        if (!count)
            count = 0x10000;
        native_bus_dma.channels[6].control &= ~0x10000000;
        if (address >= 0x800000 || count - 1 > address / 4)
            return psx_dma_bus_error(&native_bus_dma, &native_root_irq, 6);
        address &= PSX_DRAM_SIZE - 1;
        for (index = 0; index < count; ++index)
        {
            next = (address - 4) & (PSX_DRAM_SIZE - 1);
            value = index + 1 == count ? 0xFFFFFF : next;
            if (!w_u32(0x80000000 | address, value))
                return psx_dma_bus_error(&native_bus_dma, &native_root_irq, 6);
            if (xport_device_trace_enabled() && !psx_dma_trace_write(&native_bus_dma, 6, XPORT_DEVICE_REGION_NONE, 0, XPORT_DEVICE_REGION_RAM, address, 4, 15))
                return psx_dma_bus_error(&native_bus_dma, &native_root_irq, 6);
            address = next;
        }
        return psx_dma_complete(&native_bus_dma, &native_root_irq, 6);
    }
#ifndef NDEBUG
    fprintf(stderr, "Unbound native DMA transfer channel %u\n", channel);
#else
    (void)0;
#endif
    return -1;
}

static sint32 native_bus_dma_halt(void *context, uint32 ticks)
{
    (void)context;
#ifndef NDEBUG
    fprintf(stderr, "Unbound native DMA halt for %u ticks\n", ticks);
#else
    (void)0;
#endif
    return 0;
}

static sint32 native_bus_read(void *context, uint32 address, uint32 width, uint32 *value)
{
    uint32 offset, channel, index;
    if ((address == 0x1F801820 || address == 0x1F801824) && width == 4)
    {
        uint32 stall;
        sint32 result;
        psx_root_counter_poll();
        if (address == 0x1F801824)
            *value = native_mdec.status;
        else
        {
            result = mdec_read_data(&native_mdec, value, &stall);
            if (!result)
            {
                native_mdec_advance(stall);
                result = mdec_read_data(&native_mdec, value, &stall);
            }
            if (result != 1)
                return 0;
            native_mdec_advance(0);
        }
        return 1;
    }
    if (address >= 0x1F801C00 && address < 0x1F801E00)
        return spu_mmio_read(context, address, width, value);
    if ((address == 0x1F801810 || address == 0x1F801814) && width == 4)
    {
        psx_native_poll_events();
        return gpu_port_read(address - 0x1F801810, value);
    }
    if (address >= 0x1F801070 && address < 0x1F801078)
        return native_root_irq_read(context, address, width, value);
    if (address >= 0x1F801080 && address < 0x1F8010F8 && width == 4)
    {
        psx_root_counter_poll();
        offset = address - 0x1F801080;
        channel = offset >> 4;
        if (offset == 0x70)
            *value = native_bus_dma.priority;
        else if (offset == 0x74)
            *value = native_bus_dma.interrupt;
        else if (channel < 7 && (offset & 15) == 0)
            *value = native_bus_dma.channels[channel].base;
        else if (channel < 7 && (offset & 15) == 4)
            *value = native_bus_dma.channels[channel].block;
        else if (channel < 7 && (offset & 15) == 8)
            *value = native_bus_dma.channels[channel].control;
        else
            return 0;
        return 1;
    }
    if (address >= 0x1F801100 && address < 0x1F80112C && (width == 2 || width == 4))
    {
        offset = address - 0x1F801100;
        index = offset >> 4;
        if (index >= 3 || ((offset & 15) != 0 && (offset & 15) != 4 && (offset & 15) != 8))
            return 0;
        psx_root_counter_poll();
        if ((offset & 15) == 0)
            *value = timers.counter[index];
        else if ((offset & 15) == 4)
        {
            *value = timers.mode[index];
            timers.mode[index] &= ~0x1800;
        }
        else
            *value = timers.target[index];
        return 1;
    }
    return 0;
}

static sint32 native_bus_write(void *context, uint32 address, uint32 width, uint32 value, uint32 byte_mask)
{
    uint32 offset, index;
    if (address >= 0x1F801C00 && address < 0x1F801E00)
        return spu_mmio_write(context, address, width, value, byte_mask);
    if ((width != 2 && width != 4) || byte_mask != ((1 << width) - 1))
        return 0;
    if ((address == 0x1F801820 || address == 0x1F801824) && width == 4)
    {
        psx_root_counter_poll();
        if (address == 0x1F801824)
            mdec_control(&native_mdec, value);
        else if (mdec_write_data(&native_mdec, value) != 2)
            return 0;
        native_mdec_advance(0);
        return 1;
    }
    if ((address == 0x1F801810 || address == 0x1F801814) && width == 4)
    {
        uint32 status;
        if (!gpu_port_write(address - 0x1F801810, value) || !gpu_port_read(4, &status))
            return 0;
        if (!psx_irq_line(&native_root_irq, 1, (status >> 24) & 1))
            return 0;
        native_gpu_service();
        return 1;
    }
    if (address >= 0x1F801070 && address < 0x1F801078)
        return native_root_irq_write(context, address, width, value, byte_mask);
    if (address >= 0x1F801080 && address < 0x1F8010F8 && width == 4)
    {
        sint32 result;
        psx_root_counter_poll();
        result = psx_dma_write(&native_bus_dma, &native_root_irq, address - 0x1F801080, value, 0, native_bus_dma_transfer, native_bus_dma_halt, NULL);
        if (result != 1)
            return 0;
        native_mdec_advance(0);
        native_gpu_service();
        return 1;
    }
    if (address >= 0x1F801100 && address < 0x1F80112C)
    {
        offset = address - 0x1F801100;
        index = offset >> 4;
        if (index >= 3 || ((offset & 15) != 0 && (offset & 15) != 4 && (offset & 15) != 8))
            return 0;
        psx_root_counter_poll();
        if ((offset & 15) == 0)
            timers.counter[index] = value & 0xFFFF;
        else if ((offset & 15) == 4)
        {
            timers.mode[index] = (timers.mode[index] & 0x1800) | (value & 0x3FF) | 0x400;
            timers.counter[index] = 0;
            native_timer_irq_fired[index] = 0;
            psx_irq_line(&native_root_irq, index + 4, 0);
        }
        else
            timers.target[index] = value & 0xFFFF;
        return 1;
    }
    return 0;
}

void psx_native_bus_bind(void)
{
    spu_mmio_initialize();
    memset(native_pad_serial, 0, sizeof(native_pad_serial));
    mdec_initialize(&native_mdec);
    native_mdec_requests();
    native_gpu_request();
    native_root_last_us = xport_timer_get();
    native_root_clock_started = 1;
    native_root_tick_fraction = 0;
    xport_memory_bind_mmio(0x1F801040, 0xDC0, native_bus_read, native_bus_write, NULL);
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

void psx_root_counter_bind_clock(PsxRootClockSample sample, void *context)
{
    native_root_clock_sample = sample;
    native_root_clock_context = context;
    native_root_last_tick = sample ? sample(context) : 0;
    native_root_last_us = xport_timer_get();
    native_root_tick_fraction = 0;
    native_root_clock_started = 1;
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
    if (native_root_clock_sample)
    {
        now = native_root_clock_sample(native_root_clock_context);
        if (now < native_root_last_tick)
        {
#ifdef _DEBUG
            fputs("PSX: emulated root clock moved backwards\n", stderr);
#endif
            abort();
        }
        ticks = now - native_root_last_tick;
        native_root_last_tick = now;
    }
    else
    {
        now = xport_timer_get();
        elapsed = now - native_root_last_us;
        native_root_last_us = now;
        ticks = elapsed / 1000000u * 33868800u;
        native_root_tick_fraction += elapsed % 1000000u * 33868800u;
        ticks += native_root_tick_fraction / 1000000u;
        native_root_tick_fraction %= 1000000u;
    }
    native_mdec_advance(ticks);
    native_gpu_service();
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

PsxInterruptCallback setIntrDMA(uint32 channel, PsxInterruptCallback callback)
{
    PsxInterruptCallback previous;
    uint32 value, bit;
    if (channel >= sizeof(native_dma_callbacks) / sizeof(native_dma_callbacks[0]))
        return NULL;
    previous = DMACallback(channel, callback);
    if (previous != callback)
    {
        value = (r_u32(0x1F8010F4) & 0x00FFFFFFu) | 0x00800000u;
        bit = 1u << (channel + 16);
        w_u32(0x1F8010F4, callback != NULL ? value | bit : value & ~bit);
    }
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
    uint32 initial = destination;
    if (!destination || (sint32)length <= 0)
        return 0;
    do
    {
        w_u8(destination, 0);
        --length;
        ++destination;
    } while ((sint32)length > 0);
    return initial;
}

static PSX_BIOS_FILE_GUEST_BINDING bios_file_guest;

sint32 psx_bios_bind_file_guest(const PSX_BIOS_FILE_GUEST_BINDING *binding)
{
    memset(&bios_file_guest, 0, sizeof(bios_file_guest));
    if (!binding)
        return 1;
    if ((binding->descriptors & 3) || (binding->error & 3) || !psx_addr(binding->descriptors, 16 * 44) || !psx_addr(binding->error, 4) || !psx_addr(binding->invalid_seek_text, 1))
        return 0;
    bios_file_guest = *binding;
    return 1;
}

sint32 lseekPSX(sint32 descriptor, sint32 offset, uint32 origin)
{
    uint32 file;
    if (!bios_file_guest.descriptors)
        abort();
    if (descriptor < 0 || descriptor >= 16)
    {
        w_u32(bios_file_guest.error, 9);
        return -1;
    }
    file = bios_file_guest.descriptors + (uint32)descriptor * 44;
    if (r_u32(file) == 0)
    {
        w_u32(bios_file_guest.error, 9);
        return -1;
    }
    if (origin == 0)
        w_u32(file + 16, (uint32)offset);
    else if (origin == 1)
    {
        uint32 position = r_u32(file + 16);
        w_u32(file + 16, position + (uint32)offset);
    }
    else if (origin != 2)
    {
        const char *message = psx_addr(bios_file_guest.invalid_seek_text, 1);
        const char *end;
        uintptr_t offset;
        if (!message || (uintptr_t)message < (uintptr_t)DRAM || (uintptr_t)message - (uintptr_t)DRAM >= sizeof(DRAM))
            abort();
        offset = (uintptr_t)message - (uintptr_t)DRAM;
        end = memchr(message, 0, sizeof(DRAM) - offset);
        if (!end)
            abort();
#ifndef NDEBUG
        fwrite(psx_addr(bios_file_guest.invalid_seek_text, end - message), 1, end - message, stdout);
#endif
        w_u32(bios_file_guest.error, 0x16);
        w_u32(file + 24, 0x16);
        return -1;
    }
    /* Original origin2 leaves the position unchanged */
    return r_s32(file + 16);
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
#if defined(NDEBUG)
    if ((fd != 1 && fd != 2) || (!src && length))
        return -1;
    return (sint32)length;
#else
    FILE *stream;
    size_t written;
    #if defined(_WIN32) && !defined(XPORT_XCRT)
    int previous_mode;
    #endif
    if ((fd != 1 && fd != 2) || (!src && length))
        return -1;
    stream = fd == 1 ? stdout : stderr;
    fflush(stream);
    #if defined(_WIN32) && !defined(XPORT_XCRT)
    previous_mode = _setmode(_fileno(stream), _O_BINARY);
    #endif
    written = length ? fwrite(src, 1, length, stream) : 0;
    fflush(stream);
    #if defined(_WIN32) && !defined(XPORT_XCRT)
    if (previous_mode >= 0)
        _setmode(_fileno(stream), previous_mode);
    #endif
    return (sint32)(uint32)written;
#endif
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

sint32 format(const char *path)
{
    if (!path)
        return 0;
    /* The native BIOS filesystem has no mounted card medium */
    if (strncmp(path, "bu00:", 5) == 0 || strncmp(path, "bu10:", 5) == 0)
        return 0;
#ifndef NDEBUG
    fprintf(stderr, "Unsupported native BIOS format device: %s\n", path);
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
        case 0x11:
            for (row = 0; row < 3; ++row)
            {
                sint64 base = (sint64)gte.ir[row] * 4096;
                sint32 difference;
                native_gte_mac_ir(row, (sint64)gte.far_color[row] * 4096 - base, shift, 0);
                difference = gte.ir[row];
                native_gte_mac_ir(row, base + (sint64)gte.ir0 * difference, shift, lm);
            }
            push_rgb_from_mac();
            finish_flag();
            return;
        case 0x1E:
        case 0x20:
            for (index = 0; index < (uint32)(opcode == 0x20 ? 3 : 1); ++index)
            {
                sint32 input[3];
                v = native_gte_vertex(index);
                input[0] = v.vx;
                input[1] = v.vy;
                input[2] = v.vz;
                native_gte_matrix(&gte.light, input, NULL, shift, lm);
                for (row = 0; row < 3; ++row)
                    input[row] = gte.ir[row];
                native_gte_matrix(&gte.color, input, gte.back_color, shift, lm);
                push_rgb_from_mac();
            }
            finish_flag();
            return;
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
        case 0x29:
            if (command != 0x680029)
                break;
            // DCPL multiplies each input IR channel before depth interpolation
            for (row = 0; row < 3; ++row)
            {
                sint64 base = (sint64)((gte.rgbc >> (row * 8)) & 255) * gte.ir[row] * 16;
                sint64 difference = native_gte_wrap44((sint64)gte.far_color[row] * 4096 - base, row);
                sint32 delta = clamp_ir((sint32)native_gte_floor_shift(difference, 12), 1u << (24 - row));
                sint64 value = native_gte_wrap44(base + (sint64)gte.ir0 * delta, row);
                gte.mac[row] = (sint32)native_gte_floor_shift(value, 12);
                gte.ir[row] = clamp_ir(gte.mac[row], 1u << (24 - row));
            }
            push_rgb_from_mac();
            finish_flag();
            return;
        case 0x3Du:
        case 0x3Eu:
            for (row = 0u; row < 3u; ++row)
            {
                sint64 value = (sint64)gte.ir0 * gte.ir[row];
                if (opcode == 0x3Eu)
                    value += (sint64)gte.mac[row] * (shift ? 4096 : 1);
                native_gte_mac_ir(row, value, shift, lm);
            }
            push_rgb_from_mac();
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
    xport_bios_stop_card();
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
    return psx_dispatch_guest_call(target, 3, argument0, argument1, argument2);
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

uint32 memmovePSX(uint32 destination, uint32 source, sint32 bytes)
{
    uint32 remaining;
    if (bytes <= 0)
        return destination;
    remaining = (uint32)bytes;
    if (destination < source)
    {
        while (remaining != 0)
        {
            w_u8(destination, r_u8(source));
            ++destination;
            ++source;
            --remaining;
        }
    }
    else
    {
        while (remaining != 0)
        {
            --remaining;
            w_u8(destination + remaining, r_u8(source + remaining));
        }
    }
    return destination;
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

uint32 SquareRoot12(sint32 input)
{
    uint32 leading, even, normalized, value;
    sint32 shift;
    xport_gte_write_data(30, (uint32)input);
    leading = xport_gte_read_data(31);
    if (leading == 32)
        return 0;
    even = leading & 0xFFFFFFFE;
    shift = (19 - (sint32)even) >> 1;
    if (even < 24)
        normalized = (uint32)(input >> ((24 - even) & 31));
    else
        normalized = (uint32)input << ((even - 24) & 31);
    value = (uint32)(sint32)psyq_sqrt_table[normalized - 64];
    if (shift < 0)
        return value >> ((uint32)-shift & 31);
    return value << ((uint32)shift & 31);
}

uint32 SquareRoot12PSX(uint32 input, uint32 table_address)
{
    uint32 leading, even, normalized, value;
    sint32 shift;
    xport_gte_write_data(30, input);
    leading = xport_gte_read_data(31);
    if (leading == 32)
        return 0;
    even = leading & 0xFFFFFFFE;
    shift = (19 - (sint32)even) >> 1;
    if (even < 24)
        normalized = (uint32)((sint32)input >> ((24 - even) & 31));
    else
        normalized = input << ((even - 24) & 31);
    value = (uint32)(sint32)r_s16(table_address + 2 * (normalized - 64));
    if (shift < 0)
        return value >> ((uint32)-shift & 31);
    return value << ((uint32)shift & 31);
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
    uint32 count = 0u, consumed = 0u;
    uint32 character, digits, magnitude;
    uint8 decimal[10];
    while ((character = r_u8(format++)) != 0u)
    {
        if (character != '%')
        {
            w_u8(destination + count++, character);
            continue;
        }
        character = r_u8(format++);
        if (character == '%')
        {
            w_u8(destination + count++, '%');
            continue;
        }
        if (character != 'd' || consumed != 0u)
        {
#ifdef _DEBUG
            fprintf(stderr, "sprintfPSX: Unsupported one-argument format conversion %02X\n", character);
#endif
            abort();
        }
        consumed = 1u;
        magnitude = value;
        if ((sint32)value < 0)
        {
            w_u8(destination + count++, '-');
            magnitude = 0u - value;
        }
        digits = 0u;
        do
        {
            decimal[digits++] = (uint8)('0' + magnitude % 10u);
            magnitude /= 10u;
        } while (magnitude != 0u);
        while (digits != 0u)
            w_u8(destination + count++, decimal[--digits]);
    }
    w_u8(destination + count, 0u);
    return count;
}

PsxCallbackDriver *psx_callback_reset_driver(void)
{
    uint32 address;
    if (psx_callback_driver.initialized != 0)
        return NULL;
    ResetCallbackPSX();
    psx_root_counter_bind(0, 0);
    w_u16(0x1F801074u, 0);
    w_u16(0x1F801070u, r_u16(0x1F801074u));
    w_u32(0x1F8010F0u, 0x33333333);
    psx_callback_driver.saved_interrupt_mask = 0;
    psx_callback_driver.saved_dma_control = 0;
    w_u32(0x1F801114u, 0x107);
    for (address = 0; address < 8; ++address)
        setIntrVSync(address, NULL);
    InterruptCallback(0, trapIntrVSync);
    for (address = 0; address < 7; ++address)
        DMACallback(address, NULL);
    w_u32(0x1F8010F4, 0);
    _96_remove();
    ExitCriticalSection();
    return &psx_callback_driver;
}

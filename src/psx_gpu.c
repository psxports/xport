#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Software GPU raster core */
#include "psx.h"
#include "psx_gpu.h"
#include "xport.h"
#include "xport_trace.h"
#define VRAM_W GPU_VRAM_WIDTH
#define VRAM_H GPU_VRAM_HEIGHT

// PsyQ DMA terminator followed by four NOP commands
const uint32 gpu_dma_nop_terminator[5] = {0x04FFFFFFu, 0u, 0u, 0u, 0u};

enum
{
    GPU_DISPLAY_MAX_WIDTH = 640,
    GPU_DISPLAY_MAX_HEIGHT = 512
};

#if !defined(LOCKSTEP_DEBUG)
uint16 VRAM[GPU_VRAM_WIDTH * GPU_VRAM_HEIGHT];
#endif

enum
{
    GPU_NATIVE_DMA_RANGE_COUNT = 32
};

typedef struct GpuNativeDmaRange
{
    uintptr_t begin;
    uintptr_t end;
} GpuNativeDmaRange;

static GpuNativeDmaRange native_dma_ranges[GPU_NATIVE_DMA_RANGE_COUNT];
static sint32 native_dma_range_count;

typedef struct
{
    uintptr_t begin, end;
    uint32 link;
} GpuPacketRange;

static GpuPacketRange packet_ranges[GPU_NATIVE_DMA_RANGE_COUNT];
static sint32 packet_range_count;
static uint32 packet_range_next = 0x00200000u;

static sint32 gpu_guest_address(const void *pointer, uint32 *address);

// Stable native links occupy the unused gap between RAM and scratchpad tags
sint32 gpu_register_packet_range(void *begin, uint32 size)
{
    uintptr_t first = (uintptr_t)begin, end = first + size;
    uint32 ignored;
    sint32 index;
    if (!begin || !size || ((first | size) & 3u) || end < first || gpu_guest_address(begin, &ignored))
        return 0;
    for (index = 0; index < packet_range_count; ++index)
    {
        if (first == packet_ranges[index].begin && end == packet_ranges[index].end)
            return 1;
        if (first < packet_ranges[index].end && end > packet_ranges[index].begin)
            return 0;
    }
    if (packet_range_count >= GPU_NATIVE_DMA_RANGE_COUNT || size > 0x00800000u - packet_range_next)
        return 0;
    packet_ranges[packet_range_count].begin = first;
    packet_ranges[packet_range_count].end = end;
    packet_ranges[packet_range_count].link = packet_range_next;
    packet_range_next += size;
    ++packet_range_count;
    return 1;
}

static sint32 gpu_packet_range_address(const void *pointer, uint32 *link)
{
    uintptr_t address = (uintptr_t)pointer;
    sint32 index;
    for (index = 0; index < packet_range_count; ++index)
        if (address >= packet_ranges[index].begin && address < packet_ranges[index].end)
        {
            *link = packet_ranges[index].link + (uint32)(address - packet_ranges[index].begin);
            return 1;
        }
    return 0;
}

static uint32 gpu_packet_range_pointer(uint32 link, uint32 **pointer)
{
    sint32 index;
    for (index = 0; index < packet_range_count; ++index)
    {
        uint32 size = (uint32)(packet_ranges[index].end - packet_ranges[index].begin);
        if (link >= packet_ranges[index].link && link - packet_ranges[index].link < size)
        {
            uint32 offset = link - packet_ranges[index].link;
            *pointer = (uint32 *)(packet_ranges[index].begin + offset);
            return size - offset;
        }
    }
    return 0;
}

static GpuTransferObserver transfer_observer;
static void *transfer_observer_context;
static sint32 transfer_observation_failed;
static GpuPsyqStateBinding psyq_binding;
static sint32 psyq_bound;
static GpuPsyqQueueBinding psyq_queue_binding;
static sint32 psyq_queue_bound;
static GpuLinkedSubmit linked_submit;
static void *linked_submit_context;
static sint32 linked_submit_failed;
static void gpu_draw_linked_dma(uint32 *ot);
static XportDeviceOperation *linked_trace_operation;
static GpuSyncSample sync_sample;

static struct
{
    uint64 deadline, last_time;
    uint32 fraction, started, skipped, skipped_count;
    sint32 enabled;
} frame_skip = {0, 0, 0, 0, 0, 0,
#if defined(LOCKSTEP_DEBUG)
                0
#else
                1
#endif
};

static sint32 frame_raster = 1;
static uint32 frame_present_depth;
static uint32 vram_version = 1;

static void gpu_vram_invalidate(void)
{
    if (!++vram_version)
        vram_version = 1;
}

uint32 gpu_vram_version(void)
{
    return vram_version;
}

void gpu_set_raster(sint32 enabled)
{
#if defined(LOCKSTEP_DEBUG)
    (void)enabled;
#else
    enabled = enabled != 0;
    if (!enabled && frame_raster)
        gpu_vram_invalidate();
    if (enabled && !frame_raster)
        frame_skip.started = frame_skip.skipped = 0;
    frame_raster = enabled;
#endif
}

sint32 gpu_get_raster(void)
{
    return frame_raster;
}

void gpu_set_frame_skip(sint32 enabled)
{
    memset(&frame_skip, 0, sizeof(frame_skip));
#if !defined(LOCKSTEP_DEBUG)
    frame_skip.enabled = enabled != 0;
#else
    (void)enabled;
#endif
}

sint32 gpu_frame_skipped(void)
{
    return frame_skip.skipped && !transfer_observer && !linked_submit && !sync_sample && !xport_device_trace_enabled();
}

uint32 gpu_skipped_frames(void)
{
    return frame_skip.skipped_count;
}

sint32 gpu_frame_sync(uint32 fields)
{
    uint64 now, span, scaled;
    if (!frame_skip.enabled || transfer_observer || linked_submit || sync_sample || xport_device_trace_enabled())
    {
        frame_skip.started = frame_skip.skipped = 0;
        return 0;
    }
    now = xport_timer_get();
    scaled = (uint64)fields * 1000000u * FIELD_RATE_DENOMINATOR;
    span = scaled / FIELD_RATE_NUMERATOR;
    // Rebase after a pause instead of replaying an unbounded backlog
    if (!frame_skip.started || now < frame_skip.last_time || now - frame_skip.last_time > 1000000)
    {
        // The first requested field ends after the initial VSync wait
        frame_skip.deadline = now + span;
        frame_skip.fraction = 0;
        frame_skip.started = 1;
    }
    else
    {
        scaled += frame_skip.fraction;
        frame_skip.deadline += scaled / FIELD_RATE_NUMERATOR;
        frame_skip.fraction = scaled % FIELD_RATE_NUMERATOR;
    }
    frame_skip.last_time = now;
    // Blocking VSync latches one decision for every pass of the following frame
    frame_skip.skipped = now >= frame_skip.deadline + span;
    if (frame_skip.skipped)
        ++frame_skip.skipped_count;
    return frame_skip.skipped;
}

void gpu_bind_linked_submit(GpuLinkedSubmit submit, void *context)
{
    linked_submit = submit;
    linked_submit_context = context;
    linked_submit_failed = 0;
}

sint32 gpu_linked_submit_failed(void)
{
    return linked_submit_failed;
}

static void gpu_psyq_gp1(uint32 command);
static uint32 sync_head, sync_tail, sync_budget;
static GpuSyncStep sync_pump, sync_wait;
static void *sync_context;
static sint32 sync_failed, sync_running;
static GpuPsyqTimeout sync_timeout;
static sint32 sync_timeout_bound;

static GpuScanoutState native_scanout = {0, 0, 16, 256, 0, 0, 0, 0};
static uint64 scanout_last_us;
static uint32 scanout_fraction, scanout_clock_started;
static DISPENV native_disp_cache;
static uint32 native_disp_cached;
static DRAWENV native_draw_cache;
static uint32 native_draw_cached;

void gpu_scanout_reset(GpuScanoutState *state)
{
    memset(state, 0, sizeof(*state));
    state->start_y = 16u;
    state->end_y = 256u;
}

sint32 gpu_scanout_validate(const GpuScanoutState *state)
{
    uint32 pal;
    if (!state)
        return 0;
    pal = (state->mode >> 3) & 1u;
    return state->mode <= 255u && state->disp_y <= 1023u && state->start_y <= 1023u && state->end_y <= 1023u && state->line < (pal ? 314u : 263u) && state->tick < (pal ? 3406u : 3413u) && state->field <= 1u && state->disp_field <= 1u;
}

void gpu_scanout_command(GpuScanoutState *state, uint32 command)
{
    switch (command >> 24)
    {
        case 0u:
            gpu_scanout_reset(state);
            break;
        case 5u:
            state->disp_y = (command >> 10) & 1023u;
            break;
        case 7u:
            state->start_y = command & 1023u;
            state->end_y = (command >> 10) & 1023u;
            break;
        case 8u:
            state->mode = command & 255u;
            state->line %= (state->mode & 8u) ? 314u : 263u;
            state->tick %= (state->mode & 8u) ? 3406u : 3413u;
            break;
        default:
            break;
    }
}

sint32 gpu_scanout_advance(GpuScanoutState *state, uint64 ticks)
{
    uint32 total_lines, line_ticks, end_y, start_y, count, edge;
    uint64 lines;
    if (!gpu_scanout_validate(state))
        return 0;
    total_lines = (state->mode & 8u) ? 314u : 263u;
    line_ticks = (state->mode & 8u) ? 3406u : 3413u;
    // Visit two fields before skipping complete parity cycles
    if (ticks >= (uint64)2u * total_lines * line_ticks)
        ticks = ticks % ((uint64)2u * total_lines * line_ticks) + (uint64)2u * total_lines * line_ticks;
    ticks += state->tick;
    lines = ticks / line_ticks;
    state->tick = (uint32)(ticks % line_ticks);
    start_y = state->start_y < total_lines ? state->start_y : total_lines;
    end_y = state->end_y < total_lines ? state->end_y : total_lines;
    while (lines)
    {
        edge = total_lines;
        if (start_y < end_y && state->line < end_y)
            edge = end_y;
        count = edge - state->line;
        if (lines < count)
            count = (uint32)lines;
        state->line += count;
        lines -= count;
        if (state->line == end_y && start_y < end_y)
            state->disp_field = (state->mode & 0x24u) == 0x24u ? state->field ^ 1u : 0u;
        if (state->line == total_lines)
        {
            state->line = 0u;
            state->field = (state->mode & 32u) ? state->field ^ 1u : 0u;
        }
    }
    return 1;
}

sint32 gpu_scanout_ode(const GpuScanoutState *state)
{
    uint32 active;
    if (!gpu_scanout_validate(state))
        return -1;
    if ((state->mode & 0x24u) != 0x24u)
        return (sint32)((state->disp_y + state->line) & 1u);
    active = state->line >= state->start_y && state->line < state->end_y;
    return (sint32)((state->disp_y + (active ? state->disp_field : 0u)) & 1u);
}

static void gpu_scanout_poll(void)
{
    uint64 now = xport_timer_get(), elapsed, ticks, scaled;
    uint32 freq = (native_scanout.mode & 8u) ? 53203425u : 53693175u;
    uint32 period = (native_scanout.mode & 8u) ? 2u * 314u * 3406u : 2u * 263u * 3413u;
    if (!scanout_clock_started)
    {
        scanout_last_us = now;
        scanout_clock_started = 1u;
        return;
    }
    elapsed = now >= scanout_last_us ? now - scanout_last_us : 0u;
    scanout_last_us = now;
    scaled = elapsed % 1000000u * freq + scanout_fraction;
    ticks = (elapsed / 1000000u % period) * freq + scaled / 1000000u;
    if (elapsed / 1000000u >= period)
        ticks += period;
    scanout_fraction = (uint32)(scaled % 1000000u);
    if (!gpu_scanout_advance(&native_scanout, ticks))
        abort();
}

sint32 GetODE(void)
{
    uint32 dma, status;
    if (sync_sample)
    {
        if (sync_sample(sync_context, &dma, &status) != 1)
            abort();
        return (sint32)(status >> 31);
    }
    gpu_scanout_poll();
    return gpu_scanout_ode(&native_scanout);
}

sint32 gpu_bind_sync(uint32 queue_head, uint32 queue_tail, GpuSyncSample sample, GpuSyncStep pump, GpuSyncStep wait, void *context, uint32 budget)
{
    uint32 addresses[2] = {queue_head, queue_tail}, i;
    if (sync_running)
    {
        sync_failed = 1;
        return 0;
    }
    if (sample)
    {
        if (!pump || !wait || !budget)
            return 0;
        for (i = 0; i < 2u; ++i)
        {
            uint32 physical = addresses[i] & 0x1fffffffu;
            uint32 segment = addresses[i] & 0xe0000000u;
            if ((segment != 0u && segment != 0x80000000u && segment != 0xa0000000u) || physical > PSX_DRAM_SIZE - 4u || (physical & 3u))
                return 0;
        }
        if ((queue_head & 0x1fffffffu) == (queue_tail & 0x1fffffffu))
            return 0;
    }
    else if (queue_head || queue_tail || pump || wait || context || budget)
        return 0;
    sync_head = queue_head;
    sync_tail = queue_tail;
    sync_sample = sample;
    sync_pump = pump;
    sync_wait = wait;
    sync_context = context;
    sync_budget = budget;
    sync_timeout_bound = 0;
    sync_failed = 0;
    return 1;
}

sint32 gpu_sync_failed(void)
{
    return sync_failed;
}

sint32 gpu_bind_psyq_state(const GpuPsyqStateBinding *binding)
{
    const uint32 sizes[6] = {20u, 92u, 1u, 1u, 4u, 256u};
    uint32 addresses[6];
    uint32 index;
    if (!binding)
    {
        psyq_bound = 0;
        memset(&psyq_binding, 0, sizeof(psyq_binding));
        return 1;
    }
    addresses[0] = binding->display_cache;
    addresses[1] = binding->draw_cache;
    addresses[2] = binding->graph_type;
    addresses[3] = binding->reverse_flag;
    addresses[4] = binding->video_mode;
    addresses[5] = binding->gp1_shadow;
    for (index = 0; index < 6u; ++index)
    {
        uint32 physical = addresses[index] & 0x1fffffffu;
        uint32 segment = addresses[index] & 0xe0000000u;
        if (index == 5u && addresses[index] == 0u)
            continue;
        if ((segment != 0u && segment != 0x80000000u && segment != 0xa0000000u) || physical > PSX_DRAM_SIZE - sizes[index] || ((index == 0u || index == 1u || index == 4u) && (physical & 3u)))
            return 0;
    }
    psyq_binding = *binding;
    psyq_bound = 1;
    return 1;
}

void gpu_set_transfer_observer(GpuTransferObserver observer, void *context)
{
    transfer_observer = observer;
    transfer_observer_context = context;
    transfer_observation_failed = 0;
}

sint32 gpu_transfer_observation_failed(void)
{
    return transfer_observation_failed;
}

static void gpu_unobserved_transfer(void)
{
    if (transfer_observer)
        transfer_observation_failed = 1;
}

static void gpu_observe_ot(uint32 address, const uint32 *packet, sint32 native)
{
    uint32 words = packet[0] >> 24;
    if (!transfer_observer)
        return;
    if (native || address >= PSX_DRAM_SIZE || words > (PSX_DRAM_SIZE - address - 4u) / 4u)
    {
        transfer_observation_failed = 1;
        return;
    }
    transfer_observer(transfer_observer_context, 3u, address, packet, 1u);
    if (words)
        transfer_observer(transfer_observer_context, 2u, address + 4u, packet + 1, words);
}

static void gpu_observe_dma_direction(void)
{
    uint32 command = 0x04000002u;
    if (transfer_observer)
        transfer_observer(transfer_observer_context, 1u, 4u, &command, 1u);
}

void gpu_register_dma_range(void *begin, uint32 size)
{
    uintptr_t first = (uintptr_t)begin;
    uintptr_t end = first + size;
    sint32 index;
    if (!begin || !size || end < first)
        return;
    for (index = 0; index < native_dma_range_count; ++index)
        if (native_dma_ranges[index].begin == first && native_dma_ranges[index].end == end)
            return;
    if (native_dma_range_count >= GPU_NATIVE_DMA_RANGE_COUNT)
        return;
    native_dma_ranges[native_dma_range_count].begin = first;
    native_dma_ranges[native_dma_range_count].end = end;
    ++native_dma_range_count;
}

static sint32 gpu_native_dma_pointer(uint32 link, uint32 **packet)
{
    sint32 index;
    for (index = 0; index < native_dma_range_count; ++index)
    {
        uintptr_t segment = native_dma_ranges[index].begin & ~(uintptr_t)0x00ffffffu;
        sint32 adjustment;
        for (adjustment = -1; adjustment <= 1; ++adjustment)
        {
            uintptr_t candidate = segment + (intptr_t)adjustment * 0x01000000 + (link & 0x00fffffcu);
            if (candidate >= native_dma_ranges[index].begin && candidate + sizeof(**packet) <= native_dma_ranges[index].end)
            {
                *packet = (uint32 *)candidate;
                return 1;
            }
        }
    }
    return 0;
}

static sint32 gpu_wrap_x(sint32 x)
{
    return x & (GPU_VRAM_WIDTH - 1);
}

static sint32 gpu_wrap_y(sint32 y)
{
    return y & (GPU_VRAM_HEIGHT - 1);
}

static XportDeviceRegion gpu_trace_pointer(const void *pointer, size_t bytes, uint32 *offset)
{
    XportMemoryRegion region = XPORT_MEMORY_NONE;
    if (xport_memory_pointer_identity(pointer, bytes, &region, offset))
    {
        if (region == XPORT_MEMORY_DRAM)
            return XPORT_DEVICE_REGION_RAM;
        if (region == XPORT_MEMORY_SCRATCHPAD)
            return XPORT_DEVICE_REGION_SCRATCHPAD;
    }
    *offset = 0u;
    return XPORT_DEVICE_REGION_NONE;
}

static void gpu_trace_vram_row(XportDeviceOperation *operation, XportDeviceRegion source_region, uint32 source_offset, sint32 x, sint32 y, sint32 width)
{
    sint32 column;
    if (!operation->active || width <= 0)
        return;
    if (x >= 0 && x + width <= GPU_VRAM_WIDTH)
    {
        uint32 destination = (uint32)(gpu_wrap_y(y) * GPU_VRAM_WIDTH + x) * 2u;
        xport_device_trace_stage(operation, XPORT_DEVICE_WRITE, XPORT_DEVICE_EVIDENCE_EXACT, source_region, source_offset, XPORT_DEVICE_REGION_VRAM, destination, (uint32)width * 2u, 0xffffffffu);
        return;
    }
    for (column = 0; column < width; ++column)
    {
        uint32 destination = (uint32)(gpu_wrap_y(y) * GPU_VRAM_WIDTH + gpu_wrap_x(x + column)) * 2u;
        xport_device_trace_stage(operation, XPORT_DEVICE_WRITE, XPORT_DEVICE_EVIDENCE_EXACT, source_region, source_offset + (uint32)column * 2u, XPORT_DEVICE_REGION_VRAM, destination, 2u, 3u);
    }
}

sint32 gpu_load_rect(uint32 source, sint32 x, sint32 y, sint32 width, sint32 height)
{
    XportDeviceOperation operation = {0};
    XportDeviceRegion source_region = XPORT_DEVICE_REGION_NONE;
    uint32 source_offset = 0u;
    gpu_unobserved_transfer();
    sint32 row;
    sint32 column;
    if (!source || width < 0 || height < 0)
        return 0;
    if (xport_device_trace_enabled())
    {
        uint32 physical = source & 0x1fffffffu;
        source_region = physical < PSX_DRAM_SIZE ? XPORT_DEVICE_REGION_RAM : physical >= 0x1f800000u && physical < 0x1f800400u ? XPORT_DEVICE_REGION_SCRATCHPAD : XPORT_DEVICE_REGION_NONE;
        source_offset = source_region == XPORT_DEVICE_REGION_RAM ? physical : source_region == XPORT_DEVICE_REGION_SCRATCHPAD ? physical - 0x1f800000u : 0u;
        xport_device_trace_begin(&operation, 0u, XPORT_DEVICE_GPU, XPORT_DEVICE_VRAM_UPLOAD, XPORT_DEVICE_DIRECTION_TO_DEVICE, 2u, 0xa0u);
        xport_device_trace_stage(&operation, XPORT_DEVICE_SUBMIT, XPORT_DEVICE_EVIDENCE_EXACT, source_region, source_offset, XPORT_DEVICE_REGION_VRAM, (uint32)(gpu_wrap_y(y) * GPU_VRAM_WIDTH + gpu_wrap_x(x)) * 2u, (uint32)(width * height) * 2u, 0xffffffffu);
    }
    gpu_vram_invalidate();
    for (row = 0; row < height; ++row)
    {
        const uint16 *pixels = (const uint16 *)psx_addr(source + (uint32)(row * width * 2), (size_t)width * 2);
        for (column = 0; column < width; ++column)
            VRAM[gpu_wrap_y(y + row) * GPU_VRAM_WIDTH + gpu_wrap_x(x + column)] = pixels[column];
        gpu_trace_vram_row(&operation, source_region, source_offset + (uint32)(row * width * 2), x, y + row, width);
    }
    if (operation.active)
    {
        xport_device_trace_stage(&operation, XPORT_DEVICE_RASTER_COMPLETE, XPORT_DEVICE_EVIDENCE_EXACT, XPORT_DEVICE_REGION_NONE, 0u, XPORT_DEVICE_REGION_VRAM, 0u, 0u, 0u);
        xport_device_trace_complete(&operation, 1);
    }
    return 1;
}

static sint32 gpu_guest_address(const void *pointer, uint32 *address);

static void gpu_publish_psyq_queue(uint32 callback, uint32 argument, uint32 aux)
{
    if (!psyq_queue_bound || callback == 0u)
        return;
    if (argument < PSX_DRAM_SIZE)
        argument |= psyq_queue_binding.pointer_segment;
    if (aux < PSX_DRAM_SIZE && aux != 0u)
        aux |= psyq_queue_binding.pointer_segment;
    w_u32(psyq_queue_binding.last_callback, callback);
    w_u32(psyq_queue_binding.last_argument, argument);
    w_u32(psyq_queue_binding.last_aux, aux);
    if (psyq_queue_binding.alarm_deadline != 0u)
    {
        w_u32(psyq_queue_binding.alarm_deadline, r_u32(psyq_queue_binding.frame_counter) + psyq_queue_binding.alarm_delay);
        w_u32(psyq_queue_binding.alarm_active, 0u);
    }
}

sint32 gpu_bind_psyq_queue(const GpuPsyqQueueBinding *binding)
{
    uint32 addresses[6];
    uint32 index;

    if (!binding)
        return 0;
    if (binding->pointer_segment != 0u && binding->pointer_segment != 0x80000000u && binding->pointer_segment != 0xA0000000u)
        return 0;
    addresses[0] = binding->last_callback;
    addresses[1] = binding->last_argument;
    addresses[2] = binding->last_aux;
    addresses[3] = binding->alarm_deadline;
    addresses[4] = binding->alarm_active;
    addresses[5] = binding->frame_counter;
    for (index = 0u; index < 6u; ++index)
    {
        uint32 physical = addresses[index] & 0x1FFFFFFFu;

        if (addresses[index] == 0u && index >= 3u)
            continue;
        if ((addresses[index] >> 29 != 0u && addresses[index] >> 29 != 4u && addresses[index] >> 29 != 5u) || (physical & 3u) != 0u || physical > PSX_DRAM_SIZE - 4u)
            return 0;
    }
    if ((binding->alarm_deadline == 0u) != (binding->alarm_active == 0u) || (binding->alarm_deadline == 0u) != (binding->frame_counter == 0u))
        return 0;
    psyq_queue_binding = *binding;
    psyq_queue_bound = 1;
    return 1;
}

static void gpu_observe_image(const PSX_RECT *rectangle, const uint32 *pixels, sint32 load)
{
    uint32 command, index, count, words, remainder, address = 0u;
    if (!transfer_observer)
        return;
    if (!psyq_bound || !rectangle || !pixels || rectangle->w <= 0 || rectangle->h <= 0)
        goto unsupported;
    count = (uint32)rectangle->w * (uint32)rectangle->h;
    words = (count + 1u) / 2u;
    remainder = words & 15u;
    if (rectangle->w > GPU_VRAM_WIDTH || rectangle->h > GPU_VRAM_HEIGHT || (!load && ((count & 1u) || count >= 32u)))
        goto unsupported;
    // Audit the payload boundary before exposing any part of a block upload
    if (load && words >= 16u && (!gpu_guest_address(pixels, &address) || (address & 3u) || words > PSX_DRAM_SIZE / 4u || address > PSX_DRAM_SIZE - words * 4u))
        goto unsupported;
    command = 0x04000000u;
    transfer_observer(transfer_observer_context, 1u, 4u, &command, 1u);
    command = 0x01000000u;
    transfer_observer(transfer_observer_context, 0u, 0u, &command, 1u);
    command = load ? 0xa0000000u : 0xc0000000u;
    transfer_observer(transfer_observer_context, 0u, 0u, &command, 1u);
    command = (uint16)rectangle->x | ((uint32)(uint16)rectangle->y << 16);
    transfer_observer(transfer_observer_context, 0u, 0u, &command, 1u);
    command = (uint16)rectangle->w | ((uint32)(uint16)rectangle->h << 16);
    transfer_observer(transfer_observer_context, 0u, 0u, &command, 1u);
    if (load)
    {
        for (index = 0u; index < remainder; ++index)
            transfer_observer(transfer_observer_context, 0u, 0u, pixels + index, 1u);
        if (words >= 16u)
        {
            command = 0x04000002u;
            transfer_observer(transfer_observer_context, 1u, 4u, &command, 1u);
            for (index = remainder; index < words; index += 16u)
                transfer_observer(transfer_observer_context, 2u, address + index * 4u, pixels + index, 16u);
        }
    }
    return;
unsupported:
    gpu_unobserved_transfer();
}

static sint32 gpu_decode_tim_block(const uint8 *src, size_t size, PSX_RECT *rect, const uint8 **pixels, size_t *block_size)
{
    size_t payload_size;

    if (size < 12u)
        return 0;
    *block_size = xport_load_le32(src);
    setRECT(rect, (sint16)xport_load_le16(src + 4u), (sint16)xport_load_le16(src + 6u), (sint16)xport_load_le16(src + 8u), (sint16)xport_load_le16(src + 10u));
    if (rect->w < 0 || rect->h < 0)
        return 0;
    payload_size = (size_t)rect->w * (size_t)rect->h * 2u;
    if (payload_size > size - 12u)
        return 0;
    *pixels = src + 12u;
    return 1;
}

sint32 gpu_decode_tim_body(const void *src, size_t size, GpuTimImage *image)
{
    const uint8 *bytes = (const uint8 *)src;
    GpuTimImage decoded = {0};
    size_t off = 4u;
    size_t block_size;

    if (!bytes || !image || size < 4u)
        return 0;
    decoded.mode = xport_load_le32(bytes);
    if (decoded.mode & ~0x0Bu)
        return 0;
    if (decoded.mode & 8u)
    {
        size_t clut_size;

        if (!gpu_decode_tim_block(bytes + off, size - off, &decoded.clut, &decoded.clut_data, &block_size))
            return 0;
        clut_size = (size_t)decoded.clut.w * (size_t)decoded.clut.h * 2u;
        if (block_size < 12u || block_size > size - off || clut_size > block_size - 12u || (block_size & 3u))
            return 0;
        off += block_size;
    }
    if (!gpu_decode_tim_block(bytes + off, size - off, &decoded.pixels, &decoded.pixel_data, &block_size))
        return 0;
    *image = decoded;
    return 1;
}

sint32 LoadImagePSX(PSX_RECT *rectangle, uint32 *pixels)
{
    XportDeviceOperation operation = {0};
    XportDeviceRegion source_region = XPORT_DEVICE_REGION_NONE;
    uint32 source_offset = 0u;
    gpu_observe_image(rectangle, pixels, 1);
    const uint16 *source = (const uint16 *)pixels;
    sint32 row;
    sint32 column;
    if (!rectangle || !source || rectangle->w < 0 || rectangle->h < 0)
        return -1;
    if (psyq_queue_bound)
    {
        uint32 rectangle_address;
        uint32 pixel_address;

        if (gpu_guest_address(rectangle, &rectangle_address) && gpu_guest_address(pixels, &pixel_address))
            gpu_publish_psyq_queue(psyq_queue_binding.load_image_callback, rectangle_address, pixel_address);
    }
    if (xport_device_trace_enabled())
    {
        source_region = gpu_trace_pointer(source, (size_t)rectangle->w * (size_t)rectangle->h * 2u, &source_offset);
        xport_device_trace_begin(&operation, 0u, XPORT_DEVICE_GPU, XPORT_DEVICE_VRAM_UPLOAD, XPORT_DEVICE_DIRECTION_TO_DEVICE, 2u, 0xa0u);
        xport_device_trace_stage(&operation, XPORT_DEVICE_SUBMIT, XPORT_DEVICE_EVIDENCE_EXACT, source_region, source_offset, XPORT_DEVICE_REGION_VRAM, (uint32)(gpu_wrap_y(rectangle->y) * GPU_VRAM_WIDTH + gpu_wrap_x(rectangle->x)) * 2u, (uint32)rectangle->w * (uint32)rectangle->h * 2u, 0xffffffffu);
    }
    gpu_vram_invalidate();
    for (row = 0; row < rectangle->h; ++row)
    {
        for (column = 0; column < rectangle->w; ++column)
            VRAM[gpu_wrap_y(rectangle->y + row) * GPU_VRAM_WIDTH + gpu_wrap_x(rectangle->x + column)] = source[row * rectangle->w + column];
        gpu_trace_vram_row(&operation, source_region, source_offset + (uint32)(row * rectangle->w * 2), rectangle->x, rectangle->y + row, rectangle->w);
    }
    if (operation.active)
    {
        xport_device_trace_stage(&operation, XPORT_DEVICE_RASTER_COMPLETE, XPORT_DEVICE_EVIDENCE_EXACT, XPORT_DEVICE_REGION_NONE, 0u, XPORT_DEVICE_REGION_VRAM, 0u, 0u, 0u);
        xport_device_trace_complete(&operation, 1);
    }
    return 0;
}

sint32 StoreImage(PSX_RECT *rectangle, uint32 *pixels)
{
    gpu_observe_image(rectangle, pixels, 0);
    sint32 row;
    sint32 column;
    uint16 *destination = (uint16 *)pixels;
    if (!rectangle || !destination || rectangle->w < 0 || rectangle->h < 0)
        return -1;
    gpu_vram_invalidate();
    for (row = 0; row < rectangle->h; ++row)
        for (column = 0; column < rectangle->w; ++column)
            destination[row * rectangle->w + column] = VRAM[gpu_wrap_y(rectangle->y + row) * GPU_VRAM_WIDTH + gpu_wrap_x(rectangle->x + column)];
    return 0;
}

static uint32 move_image_packet;

static void gpu_vram_copy(PSX_RECT *rectangle, sint32 x, sint32 y, XportDeviceOperation *trace)
{
    uint16 row_pixels[GPU_VRAM_WIDTH];
    sint32 first, last, step, row, column;
    gpu_vram_invalidate();
    first = y > rectangle->y ? rectangle->h - 1 : 0;
    last = y > rectangle->y ? -1 : rectangle->h;
    step = y > rectangle->y ? -1 : 1;
    for (row = first; row != last; row += step)
    {
        for (column = 0; column < rectangle->w; ++column)
            row_pixels[column] = VRAM[gpu_wrap_y(rectangle->y + row) * GPU_VRAM_WIDTH + gpu_wrap_x(rectangle->x + column)];
        for (column = 0; column < rectangle->w; ++column)
            VRAM[gpu_wrap_y(y + row) * GPU_VRAM_WIDTH + gpu_wrap_x(x + column)] = row_pixels[column];
        gpu_trace_vram_row(trace, XPORT_DEVICE_REGION_VRAM, (uint32)(gpu_wrap_y(rectangle->y + row) * GPU_VRAM_WIDTH + gpu_wrap_x(rectangle->x)) * 2u, x, y + row, rectangle->w);
    }
    if (trace->active)
    {
        xport_device_trace_stage(trace, XPORT_DEVICE_RASTER_COMPLETE, XPORT_DEVICE_EVIDENCE_EXACT, XPORT_DEVICE_REGION_NONE, 0u, XPORT_DEVICE_REGION_VRAM, 0u, 0u, 0u);
        xport_device_trace_complete(trace, 1);
    }
}

sint32 MoveImage(PSX_RECT *rectangle, sint32 x, sint32 y)
{
    XportDeviceOperation operation = {0};
    if (!rectangle || rectangle->w < 0 || rectangle->w > GPU_VRAM_WIDTH || rectangle->h < 0)
        return -1;
    gpu_publish_psyq_queue(psyq_queue_binding.move_image_callback, move_image_packet, 0u);
    if (move_image_packet)
    {
        uint32 *packet = (uint32 *)psx_addr(move_image_packet, 20u);

        packet[0] = 0x04FFFFFFu;
        packet[1] = 0x80000000u;
        packet[2] = (uint16)rectangle->x | ((uint32)(uint16)rectangle->y << 16);
        packet[3] = (uint16)x | ((uint32)(uint16)y << 16);
        packet[4] = (uint16)rectangle->w | ((uint32)(uint16)rectangle->h << 16);
        if (transfer_observer)
        {
            gpu_observe_dma_direction();
            gpu_observe_ot(move_image_packet, packet, 0);
        }
        else
            gpu_unobserved_transfer();
    }
    else
        gpu_unobserved_transfer();
    if (xport_device_trace_enabled())
    {
        xport_device_trace_begin(&operation, 0u, XPORT_DEVICE_GPU, XPORT_DEVICE_VRAM_COPY, XPORT_DEVICE_DIRECTION_INTERNAL, 2u, 0x80u);
        xport_device_trace_stage(&operation, XPORT_DEVICE_SUBMIT, XPORT_DEVICE_EVIDENCE_EXACT, XPORT_DEVICE_REGION_VRAM, (uint32)(gpu_wrap_y(rectangle->y) * GPU_VRAM_WIDTH + gpu_wrap_x(rectangle->x)) * 2u, XPORT_DEVICE_REGION_VRAM, (uint32)(gpu_wrap_y(y) * GPU_VRAM_WIDTH + gpu_wrap_x(x)) * 2u, (uint32)rectangle->w * (uint32)rectangle->h * 2u, 0xffffffffu);
    }
    gpu_vram_copy(rectangle, x, y, &operation);
    return 0;
}

sint32 gpu_bind_move_image(uint32 packet)
{
    uint32 physical = packet & 0x1FFFFFFFu;

    if ((packet >> 29 != 0u && packet >> 29 != 4u && packet >> 29 != 5u) || (physical & 3u) || physical > PSX_DRAM_SIZE - 20u)
        return 0;
    move_image_packet = physical;
    return 1;
}

static uint32 clear_image_packet;
static uint32 clear_image_dimensions;
static uint16 active_draw_flags;

sint32 gpu_bind_clear_image(uint32 packet, uint32 dimensions)
{
    uint32 physical = packet & 0x1fffffffu;
    uint32 limits = dimensions & 0x1fffffffu;
    if ((packet >> 29 != 0u && packet >> 29 != 4u && packet >> 29 != 5u) || (dimensions >> 29 != 0u && dimensions >> 29 != 4u && dimensions >> 29 != 5u) || (physical & 3u) || physical > PSX_DRAM_SIZE - 52u || (limits & 1u) || limits > PSX_DRAM_SIZE - 4u)
        return 0;
    clear_image_packet = physical;
    clear_image_dimensions = limits;
    return 1;
}

sint32 ClearImage(PSX_RECT *rectangle, uint8 red, uint8 green, uint8 blue)
{
    XportDeviceOperation operation = {0};
    sint32 row;
    sint32 column;
    uint16 color = (uint16)((red >> 3) | ((green >> 3) << 5) | ((blue >> 3) << 10));
    if (clear_image_packet && rectangle)
    {
        GpuRasterState state;
        uint32 *packet = (uint32 *)psx_addr(clear_image_packet, 52u);
        const sint16 *limits = (const sint16 *)psx_addr(clear_image_dimensions, 4u);
        uint32 rgb = red | ((uint32)green << 8) | ((uint32)blue << 16);
        uint32 xy, size, mode;
        if (limits[0] <= 0 || limits[1] <= 0)
        {
            gpu_unobserved_transfer();
            return -1;
        }
        rectangle->w = rectangle->w < 0 ? 0 : rectangle->w >= limits[0] ? limits[0] - 1 : rectangle->w;
        rectangle->h = rectangle->h < 0 ? 0 : rectangle->h >= limits[1] ? limits[1] - 1 : rectangle->h;
        xy = (uint16)rectangle->x | ((uint32)(uint16)rectangle->y << 16);
        size = (uint16)rectangle->w | ((uint32)(uint16)rectangle->h << 16);
        gpu_export_raster(&state);
        mode = 0xe1000000u | ((state.draw_mode | active_draw_flags) & 0x7ffu);
        if (((rectangle->x | rectangle->w) & 63) == 0)
        {
            packet[0] = 0x05ffffffu;
            packet[1] = 0xe6000000u;
            packet[2] = mode;
            packet[3] = 0x02000000u | rgb;
            packet[4] = xy;
            packet[5] = size;
        }
        else
        {
            uint32 command;
            packet[0] = 0x08000000u | (clear_image_packet + 36u);
            packet[1] = 0xe3000000u;
            packet[2] = 0xe4ffffffu;
            packet[3] = 0xe5000000u;
            packet[4] = 0xe6000000u;
            packet[5] = mode;
            packet[6] = 0x60000000u | rgb;
            packet[7] = xy;
            packet[8] = size;
            packet[9] = 0x03ffffffu;
            packet[10] = 0xe3000000u | state.area[0] | (state.area[1] << 10);
            packet[11] = 0xe4000000u | state.area[2] | (state.area[3] << 10);
            packet[12] = 0xe5000000u | ((uint32)state.offset[0] & 0x7ffu) | (((uint32)state.offset[1] & 0x7ffu) << 11);
            if (transfer_observer)
                for (command = 0x10000003u; command <= 0x10000005u; ++command)
                    transfer_observer(transfer_observer_context, 1u, 4u, &command, 1u);
        }
        gpu_observe_dma_direction();
        gpu_observe_ot(clear_image_packet, packet, 0);
        if (packet[0] >> 24 == 8u)
            gpu_observe_ot(clear_image_packet + 36u, packet + 9, 0);
    }
    else
        gpu_unobserved_transfer();
    if (!rectangle || rectangle->w < 0 || rectangle->h < 0)
        return -1;
    if (gpu_frame_skipped())
        return 0;
    if (xport_device_trace_enabled())
    {
        xport_device_trace_begin(&operation, 0u, XPORT_DEVICE_GPU, XPORT_DEVICE_VRAM_FILL, XPORT_DEVICE_DIRECTION_INTERNAL, 2u, 0x02u);
        xport_device_trace_stage(&operation, XPORT_DEVICE_SUBMIT, XPORT_DEVICE_EVIDENCE_EXACT, XPORT_DEVICE_REGION_NONE, 0u, XPORT_DEVICE_REGION_VRAM, (uint32)(gpu_wrap_y(rectangle->y) * GPU_VRAM_WIDTH + gpu_wrap_x(rectangle->x)) * 2u, (uint32)rectangle->w * (uint32)rectangle->h * 2u, 0xffffffffu);
    }
    gpu_vram_invalidate();
    for (row = 0; row < rectangle->h; ++row)
    {
        for (column = 0; column < rectangle->w; ++column)
            VRAM[gpu_wrap_y(rectangle->y + row) * GPU_VRAM_WIDTH + gpu_wrap_x(rectangle->x + column)] = color;
        gpu_trace_vram_row(&operation, XPORT_DEVICE_REGION_NONE, 0u, rectangle->x, rectangle->y + row, rectangle->w);
    }
    if (operation.active)
    {
        xport_device_trace_stage(&operation, XPORT_DEVICE_RASTER_COMPLETE, XPORT_DEVICE_EVIDENCE_EXACT, XPORT_DEVICE_REGION_NONE, 0u, XPORT_DEVICE_REGION_VRAM, 0u, 0u, 0u);
        xport_device_trace_complete(&operation, 1);
    }
    return 0;
}

static sint32 gpu_guest_address(const void *pointer, uint32 *address);

static sint32 gpu_native_low24_to_guest(uint32 link, uint32 *address)
{
    uintptr_t bases[2] = {(uintptr_t)DRAM, (uintptr_t)SCRATCHPAD};
    size_t sizes[2] = {sizeof(DRAM), sizeof(SCRATCHPAD)};
    uint32 guest_bases[2] = {0, 0x1f800000u};
    sint32 i;
    for (i = 0; i < 2; ++i)
    {
        uintptr_t segment = bases[i] & ~(uintptr_t)0x00ffffffu;
        sint32 adjustment;
        for (adjustment = -1; adjustment <= 1; ++adjustment)
        {
            uintptr_t candidate = segment + (intptr_t)adjustment * 0x01000000 + (link & 0x00fffffcu);
            if (candidate >= bases[i] && candidate - bases[i] < sizes[i])
            {
                *address = guest_bases[i] + (uint32)(candidate - bases[i]);
                return 1;
            }
        }
    }
    return 0;
}

void GsClearOt(uint32 offset, uint32 point, void *ordering_table)
{
    uint8 *descriptor = (uint8 *)ordering_table;
    uint32 rank = xport_load_le32(descriptor);
    uint32 address, count, bytes;
    xport_store_le32(descriptor + 8, offset & 0xFFFF);
    address = xport_load_le32(descriptor + 4);
    xport_store_le32(descriptor + 12, point & 0xFFFF);
    bytes = 4u << (rank & 31);
    count = 1u << (xport_load_le32(descriptor) & 31);
    xport_store_le32(descriptor + 16, address + bytes - 4);
    address = xport_load_le32(descriptor + 4);
    if (count > PSX_DRAM_SIZE / 4)
        abort();
    ClearOTagR((uint32 *)psx_addr(address, count * 4), (sint32)count);
}

uint32 *ClearOTagR(uint32 *ot, sint32 count)
{
    sint32 index;
    uint32 address;
    if (!gpu_packet_range_address(ot, &address) && !gpu_guest_address(ot, &address))
#if defined(AP_X64)
        abort();
#else
        address = (uint32)(intptr)ot;
#endif
    if (count <= 0)
        return ot;
    xport_store_le32(ot, 0x00ffffffu);
    for (index = 1; index < count; ++index)
        xport_store_le32(ot + index, (address + (uint32)(index - 1) * 4u) & 0x00ffffffu);
    return ot;
}

uint32 *ClearOTag(uint32 *ot, sint32 count)
{
    sint32 index;
    uint32 address;
    if (!gpu_packet_range_address(ot, &address) && !gpu_guest_address(ot, &address))
#if defined(AP_X64)
        abort();
#else
        address = (uint32)(intptr)ot;
#endif
    if (count <= 0)
        return ot;
    for (index = 0; index + 1 < count; ++index)
        xport_store_le32(ot + index, (address + (uint32)(index + 1) * 4u) & 0x00ffffffu);
    xport_store_le32(ot + count - 1, 0x00ffffffu);
    return ot;
}

void AddPrim(void *ot, void *prim)
{
    uint32 *ot_tag = (uint32 *)ot;
    uint32 *prim_tag = (uint32 *)prim;
    uint32 address;
    uint32 prim_value;
    uint32 ot_value;

    if (!gpu_packet_range_address(prim, &address) && !gpu_guest_address(prim, &address))
#if defined(AP_X64)
        abort();
#else
        address = (uint32)(intptr)prim;
#endif
    prim_value = *prim_tag;
    ot_value = *ot_tag;
    *prim_tag = (prim_value & 0xff000000u) | (ot_value & 0x00ffffffu);
    ot_value = *ot_tag;
    *ot_tag = (ot_value & 0xff000000u) | (address & 0x00ffffffu);
}

void AddPrims(void *ot, void *first, void *last)
{
    uint32 *ot_tag = (uint32 *)ot;
    uint32 *last_tag = (uint32 *)last;
    uint32 address;
    if (!gpu_packet_range_address(first, &address) && !gpu_guest_address(first, &address))
#if defined(AP_X64)
        abort();
#else
        address = (uint32)(intptr)first;
#endif
    *last_tag = (*last_tag & 0xff000000u) | (*ot_tag & 0x00ffffffu);
    *ot_tag = address & 0x00ffffffu;
}

uint16 GetTPage(sint32 depth, sint32 abr, sint32 x, sint32 y)
{
    return (uint16)(((depth & 3) << 7) | ((abr & 3) << 5) | ((y & 0x100) >> 4) | ((x & 0x3ff) >> 6) | ((y & 0x200) << 2));
}

uint16 GetClut(sint32 x, sint32 y)
{
    return (uint16)(((uint32)y << 6u) | (((uint32)x >> 4u) & 0x3Fu));
}

uint16 LoadTPage(uint32 *pixels, sint32 depth, sint32 abr, sint32 x, sint32 y, sint32 width, sint32 height)
{
    PSX_RECT rectangle;
    rectangle.x = (sint16)x;
    rectangle.y = (sint16)y;
    rectangle.w = (sint16)(depth == 0 ? width / 4 : depth == 1 ? width / 2 : width);
    rectangle.h = (sint16)height;
    LoadImagePSX(&rectangle, pixels);
    return GetTPage(depth, abr, x, y);
}

uint16 LoadClut(uint32 *pixels, sint32 x, sint32 y)
{
    PSX_RECT rectangle;
    rectangle.x = (sint16)x;
    rectangle.y = (sint16)y;
    rectangle.w = 16;
    rectangle.h = 1;
    if (LoadImagePSX(&rectangle, pixels) < 0)
        return 0;
    return GetClut(x, y);
}

sint32 gpu_load_clut(uint32 source, sint32 x, sint32 y)
{
    return gpu_load_rect(source, (sint16)x, (sint16)y, 256, 1);
}

typedef struct
{
    sint32 x, y, u, v;
} RV;

typedef struct
{
    sint32 x, y, u, v, r, g, b;
} ScanVertex;

static uint32 present_pixels[GPU_DISPLAY_MAX_WIDTH * GPU_DISPLAY_MAX_HEIGHT];
static uint16 active_tpage;

void gpu_set_polygon_tpage(uint16 page)
{
    active_tpage = (uint16)((active_tpage & ~0x09ffu) | (page & 0x09ffu));
}

static uint32 texture_window;
static uint32 display_state;
sint32 graph_debug_level;
GpuGraphControl gpu_graph_control;
static sint32 display_x;
static sint32 display_y;
static sint32 display_width = 320;
static sint32 display_height = 240;
static sint32 display_offset_x;
static sint32 display_offset_y;
static uint32 gpu_raster_pixel(sint32 x, sint32 y);
#if !defined(NDEBUG)
static uint32 gpu_debug_packet_address;
static uint32 gpu_debug_packet_tag;
#endif

static sint32 gpu_guest_address(const void *pointer, uint32 *address)
{
    uintptr_t value = (uintptr_t)pointer;
    uintptr_t base = (uintptr_t)DRAM;
    if (value >= base && value - base < sizeof(DRAM))
    {
        *address = (uint32)(value - base);
        return 1;
    }
    base = (uintptr_t)SCRATCHPAD;
    if (value >= base && value - base < sizeof(SCRATCHPAD))
    {
        *address = 0x1f800000u + (uint32)(value - base);
        return 1;
    }
    return 0;
}

uint32 gpu_packet_link(uintptr_t value)
{
    uint32 address;
    if (gpu_packet_range_address((const void *)value, &address) || gpu_guest_address((const void *)value, &address))
        return address & 0x00ffffffu;
#if defined(AP_X64)
    // Numeric guest links and the chain terminator do not contain host pointers
    if (value > UINT32_MAX || (value > 0x00ffffffu && value != UINT32_MAX && (value & 0xff000000u) != 0x80000000u && (value & 0xff000000u) != 0xa0000000u && (value & 0xfffffc00u) != 0x1f800000u))
        abort();
#endif
    return (uint32)value & 0x00ffffffu;
}

static uint32 gpu_u32(uint32 address)
{
    return *(uint32 *)psx_addr(address, 4);
}

static sint32 gpu_queue_range(uint32 address, uint32 bytes)
{
    uint32 segment = address & 0xe0000000u;
    uint32 physical = address & 0x1fffffffu;
    return (segment == 0 || segment == 0x80000000u || segment == 0xa0000000u) && !(physical & 3u) && bytes <= PSX_DRAM_SIZE && physical <= PSX_DRAM_SIZE - bytes;
}

static sint32 gpu_timeout_valid(const GpuPsyqTimeout *binding)
{
    if (!binding || !gpu_queue_range(binding->vblank_counter, 4u) || !gpu_queue_range(binding->deadline, 4u) || !gpu_queue_range(binding->polls, 4u))
        return 0;
    return ((binding->vblank_counter ^ binding->deadline) & 0x1fffffffu) && ((binding->vblank_counter ^ binding->polls) & 0x1fffffffu) && ((binding->deadline ^ binding->polls) & 0x1fffffffu);
}

sint32 gpu_psyq_timeout_begin(const GpuPsyqTimeout *binding)
{
    if (!gpu_timeout_valid(binding))
        return -1;
    *(uint32 *)psx_addr(binding->deadline, 4) = gpu_u32(binding->vblank_counter) + 240u;
    *(uint32 *)psx_addr(binding->polls, 4) = 0;
    return 1;
}

sint32 gpu_psyq_timeout_check(const GpuPsyqTimeout *binding)
{
    uint32 polls;
    if (!gpu_timeout_valid(binding))
        return -1;
    if ((gpu_u32(binding->deadline) ^ 0x80000000u) < (gpu_u32(binding->vblank_counter) ^ 0x80000000u))
        return 0;
    polls = gpu_u32(binding->polls);
    *(uint32 *)psx_addr(binding->polls, 4) = polls + 1u;
    return (polls ^ 0x80000000u) <= (0xf0000u ^ 0x80000000u);
}

sint32 gpu_bind_sync_timeout(const GpuPsyqTimeout *binding)
{
    uint32 addresses[3], index;
    if (sync_running)
    {
        sync_failed = 1;
        return 0;
    }
    if (binding && (!sync_sample || !gpu_timeout_valid(binding)))
        return 0;
    if (binding)
    {
        addresses[0] = binding->vblank_counter;
        addresses[1] = binding->deadline;
        addresses[2] = binding->polls;
        for (index = 0; index < 3u; ++index)
            if (!((addresses[index] ^ sync_head) & 0x1fffffffu) || !((addresses[index] ^ sync_tail) & 0x1fffffffu))
                return 0;
        sync_timeout = *binding;
    }
    sync_timeout_bound = binding != NULL;
    return 1;
}

static sint32 gpu_queue_valid(const GpuPsyqQueue *queue)
{
    uint32 addresses[4], lengths[4] = {64u * 96u, 4u, 4u, 12u}, i, j;
    if (!queue)
        return 0;
    addresses[0] = queue->entries;
    addresses[1] = queue->head;
    addresses[2] = queue->tail;
    addresses[3] = queue->last_command;
    for (i = 0; i < 4; ++i)
    {
        if (!gpu_queue_range(addresses[i], lengths[i]))
            return 0;
        addresses[i] &= 0x1fffffffu;
        for (j = 0; j < i; ++j)
            if (addresses[i] < addresses[j] + lengths[j] && addresses[j] < addresses[i] + lengths[i])
                return 0;
    }
    return gpu_u32(queue->head) < 64u && gpu_u32(queue->tail) < 64u;
}

sint32 gpu_psyq_queue_store(const GpuPsyqQueue *queue, const GpuPsyqCommand *command, sint32 copy_bytes)
{
    GpuPsyqCommand saved;
    uint32 head, entry, argument, index;
    if (!command || !gpu_queue_valid(queue) || copy_bytes < 0 || copy_bytes > 84)
        return -1;
    saved = *command;
    command = &saved;
    if (copy_bytes && !gpu_queue_range(command->argument, (uint32)copy_bytes & ~3u))
        return -1;
    head = gpu_u32(queue->head);
    if (((head + 1u) & 63u) == gpu_u32(queue->tail))
        return 0;
    entry = queue->entries + head * 96u;
    argument = command->argument;
    if (copy_bytes)
    {
        argument = entry + 12u;
        for (index = 0; index < (uint32)copy_bytes / 4u; ++index)
            *(uint32 *)psx_addr(argument + index * 4u, 4) = gpu_u32(command->argument + index * 4u);
    }
    *(uint32 *)psx_addr(entry + 4u, 4) = argument;
    *(uint32 *)psx_addr(entry + 8u, 4) = command->extra;
    *(uint32 *)psx_addr(entry, 4) = command->function;
    *(uint32 *)psx_addr(queue->head, 4) = (head + 1u) & 63u;
    return 1;
}

sint32 gpu_psyq_queue_peek(const GpuPsyqQueue *queue, GpuPsyqCommand *command)
{
    uint32 entry;
    if (!command || !gpu_queue_valid(queue))
        return -1;
    if (gpu_u32(queue->head) == gpu_u32(queue->tail))
        return 0;
    entry = queue->entries + gpu_u32(queue->tail) * 96u;
    command->function = gpu_u32(entry);
    command->argument = gpu_u32(entry + 4u);
    command->extra = gpu_u32(entry + 8u);
    return 1;
}

sint32 gpu_psyq_queue_retire(const GpuPsyqQueue *queue)
{
    GpuPsyqCommand command;
    sint32 result = gpu_psyq_queue_peek(queue, &command);
    if (result != 1)
        return result;
    *(uint32 *)psx_addr(queue->last_command, 4) = command.function;
    *(uint32 *)psx_addr(queue->last_command + 4u, 4) = command.argument;
    *(uint32 *)psx_addr(queue->last_command + 8u, 4) = command.extra;
    *(uint32 *)psx_addr(queue->tail, 4) = (gpu_u32(queue->tail) + 1u) & 63u;
    return 1;
}

static sint32 gpu_queue_driver_valid(const GpuPsyqQueueDriver *driver)
{
    uint32 addresses[7], sizes[7] = {6144u, 4u, 4u, 12u, 4u, 4u, 4u}, i, j;
    if (!driver || !gpu_queue_valid(&driver->queue) || !driver->sample || !driver->wait || !driver->critical || !driver->dma_callback || !driver->dispatch || !driver->complete || !driver->budget || !gpu_queue_range(driver->active, 4u) || !gpu_queue_range(driver->completion, 4u) || !gpu_queue_range(driver->saved_interrupt_mask, 4u))
        return 0;
    addresses[0] = driver->queue.entries & 0x1fffffffu;
    addresses[1] = driver->queue.head & 0x1fffffffu;
    addresses[2] = driver->queue.tail & 0x1fffffffu;
    addresses[3] = driver->queue.last_command & 0x1fffffffu;
    addresses[4] = driver->active & 0x1fffffffu;
    addresses[5] = driver->completion & 0x1fffffffu;
    addresses[6] = driver->saved_interrupt_mask & 0x1fffffffu;
    for (i = 4; i < 7; ++i)
        for (j = 0; j < i; ++j)
            if (addresses[i] < addresses[j] + sizes[j] && addresses[j] < addresses[i] + sizes[i])
                return 0;
    return 1;
}

sint32 gpu_psyq_queue_pump(const GpuPsyqQueueDriver *driver)
{
    static sint32 running, failed;
    uint32 dma, status, previous, ignored, remaining;
    sint32 locked = 0, result = -1;
    GpuPsyqCommand command;
    if (running)
    {
        failed = 1;
        return -1;
    }
    if (!gpu_queue_driver_valid(driver))
        return -1;
    running = 1;
    failed = 0;
    remaining = driver->budget;
    if (driver->sample(driver->context, &dma, &status) != 1 || failed)
        goto queue_done;
    if (dma & 0x01000000u)
    {
        result = 1;
        goto queue_done;
    }
    if (driver->critical(driver->context, 0, &previous) != 1)
        goto queue_done;
    locked = 1;
    *(uint32 *)psx_addr(driver->saved_interrupt_mask, 4) = previous;
    if (failed)
        goto queue_done;
    while (gpu_u32(driver->queue.head) != gpu_u32(driver->queue.tail))
    {
        if (!remaining-- || !gpu_queue_valid(&driver->queue) || driver->sample(driver->context, &dma, &status) != 1 || failed)
            goto queue_done;
        if (dma & 0x01000000u)
            break;
        if (((gpu_u32(driver->queue.tail) + 1u) & 63u) == gpu_u32(driver->queue.head) && !gpu_u32(driver->completion) && (driver->dma_callback(driver->context, 0) != 1 || failed))
            goto queue_done;
        for (;;)
        {
            if (driver->sample(driver->context, &dma, &status) != 1 || failed)
                goto queue_done;
            if (status & 0x04000000u)
                break;
            if (!remaining-- || driver->wait(driver->context) != 1 || failed)
                goto queue_done;
        }
        if (gpu_psyq_queue_peek(&driver->queue, &command) != 1 || driver->dispatch(driver->context, &command) != 1 || failed || gpu_psyq_queue_retire(&driver->queue) != 1)
            goto queue_done;
    }
    if (driver->critical(driver->context, gpu_u32(driver->saved_interrupt_mask), &ignored) != 1 || failed)
        goto queue_done;
    locked = 0;
    if (gpu_u32(driver->queue.head) == gpu_u32(driver->queue.tail))
    {
        if (driver->sample(driver->context, &dma, &status) != 1 || failed)
            goto queue_done;
        if (!(dma & 0x01000000u) && gpu_u32(driver->active) && gpu_u32(driver->completion))
        {
            uint32 callback = gpu_u32(driver->completion);
            *(uint32 *)psx_addr(driver->active, 4) = 0;
            if (driver->complete(driver->context, callback) != 1 || failed)
                goto queue_done;
        }
    }
    if (gpu_queue_valid(&driver->queue))
        result = (sint32)((gpu_u32(driver->queue.head) - gpu_u32(driver->queue.tail)) & 63u);
queue_done:
    if (locked && driver->critical(driver->context, gpu_u32(driver->saved_interrupt_mask), &ignored) != 1)
        failed = 1;
    running = 0;
    return failed ? -1 : result;
}

sint32 gpu_psyq_queue_submit(const GpuPsyqSubmit *submit, const GpuPsyqCommand *command, sint32 copy_bytes)
{
    static sint32 running, failed;
    const GpuPsyqQueueDriver *driver;
    GpuPsyqCommand saved;
    uint32 dma, status, previous, ignored, remaining;
    uint32 addresses[9], sizes[9] = {6144u, 4u, 4u, 12u, 4u, 4u, 4u, 1u, 4u}, i, j;
    sint32 locked = 0, result = -1, direct;
    if (running)
    {
        failed = 1;
        return -1;
    }
    if (!submit || !command || !gpu_queue_driver_valid(submit->driver) || !submit->timeout_begin || !submit->timeout_check || !submit->pump_function || !gpu_queue_range(submit->enabled & ~3u, 4u) || !gpu_queue_range(submit->saved_interrupt_mask, 4u) || copy_bytes < 0 || copy_bytes > 84 || (copy_bytes && !gpu_queue_range(command->argument, (uint32)copy_bytes & ~3u)))
        return -1;
    driver = submit->driver;
    addresses[0] = driver->queue.entries & 0x1fffffffu;
    addresses[1] = driver->queue.head & 0x1fffffffu;
    addresses[2] = driver->queue.tail & 0x1fffffffu;
    addresses[3] = driver->queue.last_command & 0x1fffffffu;
    addresses[4] = driver->active & 0x1fffffffu;
    addresses[5] = driver->completion & 0x1fffffffu;
    addresses[6] = driver->saved_interrupt_mask & 0x1fffffffu;
    addresses[7] = submit->enabled & 0x1fffffffu;
    addresses[8] = submit->saved_interrupt_mask & 0x1fffffffu;
    for (i = 7; i < 9; ++i)
        for (j = 0; j < i; ++j)
            if (addresses[i] < addresses[j] + sizes[j] && addresses[j] < addresses[i] + sizes[i])
                return -1;
    saved = *command;
    running = 1;
    failed = 0;
    remaining = driver->budget;
    if (submit->timeout_begin(driver->context) != 1 || failed)
        goto submit_done;
    while (((gpu_u32(driver->queue.head) + 1u) & 63u) == gpu_u32(driver->queue.tail))
    {
        if (!remaining-- || submit->timeout_check(driver->context) != 1 || failed || gpu_psyq_queue_pump(driver) < 0)
            goto submit_done;
        if (((gpu_u32(driver->queue.head) + 1u) & 63u) == gpu_u32(driver->queue.tail) && (driver->wait(driver->context) != 1 || failed))
            goto submit_done;
    }
    if (driver->critical(driver->context, 0, &previous) != 1)
        goto submit_done;
    locked = 1;
    *(uint32 *)psx_addr(driver->active, 4) = 1;
    *(uint32 *)psx_addr(submit->saved_interrupt_mask, 4) = previous;
    if (failed)
        goto submit_done;
    direct = !*(uint8 *)psx_addr(submit->enabled, 1);
    if (!direct && gpu_u32(driver->queue.head) == gpu_u32(driver->queue.tail))
    {
        if (driver->sample(driver->context, &dma, &status) != 1 || failed)
            goto submit_done;
        direct = !(dma & 0x01000000u) && !gpu_u32(driver->completion);
    }
    if (direct)
    {
        for (;;)
        {
            if (driver->sample(driver->context, &dma, &status) != 1 || failed)
                goto submit_done;
            if (status & 0x04000000u)
                break;
            if (!remaining-- || driver->wait(driver->context) != 1 || failed)
                goto submit_done;
        }
        if (driver->dispatch(driver->context, &saved) != 1 || failed)
            goto submit_done;
        memcpy(psx_addr(driver->queue.last_command, 12), &saved, 12);
        result = 0;
    }
    else
    {
        if (driver->dma_callback(driver->context, submit->pump_function) != 1 || failed || gpu_psyq_queue_store(&driver->queue, &saved, copy_bytes) != 1)
            goto submit_done;
        result = 1;
    }
    if (driver->critical(driver->context, gpu_u32(submit->saved_interrupt_mask), &ignored) != 1 || failed)
    {
        result = -1;
        goto submit_done;
    }
    locked = 0;
    if (!direct)
    {
        result = gpu_psyq_queue_pump(driver);
        if (result >= 0 && gpu_queue_valid(&driver->queue))
            result = (sint32)((gpu_u32(driver->queue.head) - gpu_u32(driver->queue.tail)) & 63u);
        else
            result = -1;
    }
submit_done:
    if (locked && driver->critical(driver->context, gpu_u32(submit->saved_interrupt_mask), &ignored) != 1)
        failed = 1;
    running = 0;
    return failed ? -1 : result;
}

static sint16 gpu_s16(uint32 address)
{
    return *(sint16 *)psx_addr(address, 2);
}

static void gpu_w32(uint32 address, uint32 value)
{
    *(uint32 *)psx_addr(address, 4) = value;
}

static void gpu_w16(uint32 address, uint16 value)
{
    *(uint16 *)psx_addr(address, 2) = value;
}

static void gpu_w8(uint32 address, uint8 value)
{
    *(uint8 *)psx_addr(address, 1) = value;
}

/* Diagnostic policy only: keep GPU commands and VRAM transfers, omit host pixels.
 * VRAM/pixel verification must explicitly set the game-provided environment variables */
static sint32 raster_enabled(void)
{
    if (!frame_raster)
        return 0;
#if defined(LOCKSTEP_DEBUG)
    // Lockstep must preserve GPU readback semantics
    return 1;
#elif defined(_DEBUG)
    static sint32 force = -1;
    if (!xport_is_headless())
        return 1;
    if (force < 0)
    {
        const char *r = getenv("XPORT_RASTERIZE"), *v = getenv("XPORT_VERIFY_VRAM");
        force = (r && !strcmp(r, "1")) || (v && !strcmp(v, "1"));
    #ifndef NDEBUG
        printf("headless_rasterization %s\n", force ? "enabled" : "disabled");
    #endif
    }
    return force;
#else
    return !xport_is_headless();
#endif
}

static sint32 raster_clip_x0, raster_clip_y0, raster_clip_x1 = VRAM_W, raster_clip_y1 = VRAM_H, raster_offset_x, raster_offset_y;
static sint32 raster_surface_x, raster_surface_y;

sint32 gpu_import_raster(const GpuRasterState *state, const void *vram, size_t bytes)
{
    if (!state || !vram || bytes != sizeof(VRAM) || (state->draw_mode & ~0x1ffu) || (state->draw_flags & ~0x600u) || state->texture_window > 0xfffffu || state->area[0] >= VRAM_W || state->area[2] >= VRAM_W || state->area[1] >= VRAM_H || state->area[3] >= VRAM_H || state->offset[0] < -1024 || state->offset[0] > 1023 || state->offset[1] < -1024 || state->offset[1] > 1023)
        return 0;
    gpu_vram_invalidate();
    memmove(VRAM, vram, sizeof(VRAM));
    active_tpage = (uint16)state->draw_mode;
    active_draw_flags = (uint16)state->draw_flags;
    texture_window = state->texture_window;
    raster_surface_x = raster_surface_y = 0;
    raster_clip_x0 = (sint32)state->area[0];
    raster_clip_y0 = (sint32)state->area[1];
    raster_clip_x1 = (sint32)state->area[2] + 1;
    raster_clip_y1 = (sint32)state->area[3] + 1;
    raster_offset_x = state->offset[0];
    raster_offset_y = state->offset[1];
    return 1;
}

void gpu_export_raster(GpuRasterState *state)
{
    if (!state)
        return;
    state->draw_mode = active_tpage & 0x1ffu;
    state->draw_flags = active_draw_flags;
    state->texture_window = texture_window;
    state->area[0] = (uint32)(raster_clip_x0 + raster_surface_x);
    state->area[1] = (uint32)(raster_clip_y0 + raster_surface_y);
    state->area[2] = (uint32)(raster_clip_x1 + raster_surface_x - 1);
    state->area[3] = (uint32)(raster_clip_y1 + raster_surface_y - 1);
    state->offset[0] = raster_offset_x + raster_surface_x;
    state->offset[1] = raster_offset_y + raster_surface_y;
}

static sint16 read_s16_le_at(const uint8 *p, sint32 o)
{
    return (sint16)((uint16)p[o] | (uint16)p[o + 1] << 8);
}

static void pxc(sint32 x, sint32 y, uint32 color)
{
    sint32 vram_x = x + raster_surface_x;
    sint32 vram_y = y + raster_surface_y;
    if (x >= raster_clip_x0 && x < raster_clip_x1 && y >= raster_clip_y0 && y < raster_clip_y1 && (uint32)vram_x < VRAM_W && (uint32)vram_y < VRAM_H)
        VRAM[vram_y * VRAM_W + vram_x] = (uint16)(((color >> 19) & 31u) | ((color >> 6) & 0x3e0u) | ((color << 7) & 0x7c00u) | ((color >> 16) & 0x8000u));
}

static uint32 rgb555(uint16 c)
{
    uint32 r = (c & 31) << 3, g = ((c >> 5) & 31) << 3, b = ((c >> 10) & 31) << 3;
    return (r | (r >> 5)) << 16 | (g | (g >> 5)) << 8 | (b | (b >> 5));
}

static uint16 texel_indexed(sint32 u, sint32 v, uint16 tpage, uint16 clut, sint32 *transparent)
{
    uint32 mode = (tpage >> 7) & 3u;
    uint32 tx = (tpage & 15u) * 64u, ty = ((tpage >> 4) & 1u) * 256u;
    uint32 cx = (clut & 63u) * 16u, cy = (clut >> 6) & 511u;
    uint32 mask_x = (texture_window & 31u) * 8u, mask_y = ((texture_window >> 5) & 31u) * 8u;
    uint32 offset_x = ((texture_window >> 10) & 31u) * 8u, offset_y = ((texture_window >> 15) & 31u) * 8u;
    uint32 x = (((uint32)u & ~mask_x) | (offset_x & mask_x)) & 255u;
    uint32 y = (((uint32)v & ~mask_y) | (offset_y & mask_y)) & 255u;
    uint16 word, color;
    if (mode < 2u)
    {
        uint32 shift = mode ? 1u : 2u;
        uint32 bits = mode ? 8u : 4u;
        uint32 index;
        word = VRAM[((ty + y) & 511u) * VRAM_W + ((tx + (x >> shift)) & 1023u)];
        index = (word >> ((x & ((1u << shift) - 1u)) * bits)) & ((1u << bits) - 1u);
        color = VRAM[cy * VRAM_W + ((cx + index) & 1023u)];
    }
    else
        color = VRAM[((ty + y) & 511u) * VRAM_W + ((tx + x) & 1023u)];
    *transparent = color == 0u;
    return color;
}

static uint16 texel(sint32 u, sint32 v, uint16 tpage, uint16 clut)
{
    sint32 transparent;
    return texel_indexed(u, v, tpage, clut, &transparent);
}

static uint32 modulate(uint16 c, sint32 r, sint32 g, sint32 b, sint32 raw)
{
    uint32 tr = c & 31u, tg = (c >> 5) & 31u, tb = (c >> 10) & 31u;
    uint32 mask = (uint32)(c & 0x8000u) << 16;
    if (raw)
        return rgb555(c) | mask;
    tr = (tr * (uint32)r) >> 4;
    tg = (tg * (uint32)g) >> 4;
    tb = (tb * (uint32)b) >> 4;
    if (tr > 255u)
        tr = 255u;
    if (tg > 255u)
        tg = 255u;
    if (tb > 255u)
        tb = 255u;
    /* Carry the texture STP bit beside the RGB channels */
    return tr << 16 | tg << 8 | tb | mask;
}

static uint32 semi_blend(uint32 back, uint32 front, sint32 abr)
{
    sint32 br = (back >> 19) & 31, bg = (back >> 11) & 31, bb = (back >> 3) & 31, fr = (front >> 19) & 31, fg = (front >> 11) & 31, fbv = (front >> 3) & 31, r, g, b;
    if (abr == 0)
    {
        r = (br + fr) >> 1;
        g = (bg + fg) >> 1;
        b = (bb + fbv) >> 1;
    }
    else if (abr == 1)
    {
        r = br + fr;
        g = bg + fg;
        b = bb + fbv;
    }
    else if (abr == 2)
    {
        r = br - fr;
        g = bg - fg;
        b = bb - fbv;
    }
    else
    {
        r = br + (fr >> 2);
        g = bg + (fg >> 2);
        b = bb + (fbv >> 2);
    }
    if (r < 0)
        r = 0;
    if (g < 0)
        g = 0;
    if (b < 0)
        b = 0;
    if (r > 31)
        r = 31;
    if (g > 31)
        g = 31;
    if (b > 31)
        b = 31;
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return (uint32)(r << 16 | g << 8 | b) | (front & 0x80000000u);
}

static uint32 packet_rgb(uint8 *p, sint32 o)
{
    return (uint32)p[o] << 16 | (uint32)p[o + 1] << 8 | p[o + 2];
}

static void scan_swap(ScanVertex *a, ScanVertex *b)
{
    ScanVertex value = *a;
    *a = *b;
    *b = value;
}

static sint32 scan_edge_x(const ScanVertex *a, const ScanVertex *b, sint32 y)
{
    sint64 sample_y = ((sint64)y << 16) + 0x8000;
    if (a->y == b->y)
        return a->x << 16;
    return (sint32)(((sint64)a->x << 16) + (sample_y - ((sint64)a->y << 16)) * (b->x - a->x) / (b->y - a->y));
}

static sint32 scan_ceil_center(sint32 value)
{
    return (value - 0x8000 + 0xffff) >> 16;
}

static sint32 scan_gradient(sint32 av, sint32 bv, sint32 cv, const ScanVertex *a, const ScanVertex *b, const ScanVertex *c, sint64 denominator, sint32 horizontal)
{
    sint64 value;
    if (horizontal)
        value = (sint64)(bv - av) * (c->y - a->y) - (sint64)(cv - av) * (b->y - a->y);
    else
        value = (sint64)(b->x - a->x) * (cv - av) - (sint64)(c->x - a->x) * (bv - av);
    return (sint32)(value * 65536 / denominator);
}

static sint32 scan_clamp_channel(sint32 value)
{
    value >>= 16;
    if (value < 0)
        return 0;
    return value > 255 ? 255 : value;
}

static void scan_triangle(RV va, RV vb, RV vc, uint16 tp, uint16 cl, uint32 ca, uint32 cb, uint32 cc, sint32 textured, sint32 raw, sint32 semi, sint32 shaded)
{
    static const sint32 dither[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};
    RV vertex[3] = {va, vb, vc}, sorted[3] = {va, vb, vc};
    uint32 colors[3] = {ca, cb, cc};
    uint32 start[5], step_x[5], step_y[5];
    sint32 attribute[3][5];
    sint32 anchor, i, j, y, min_x, max_x, min_y, max_y;
    sint64 determinant, long_step;
    min_x = max_x = va.x;
    min_y = max_y = va.y;
    for (i = 0; i < 3; ++i)
    {
        if (vertex[i].x < min_x)
            min_x = vertex[i].x;
        if (vertex[i].x > max_x)
            max_x = vertex[i].x;
        if (vertex[i].y < min_y)
            min_y = vertex[i].y;
        if (vertex[i].y > max_y)
            max_y = vertex[i].y;
        attribute[i][0] = vertex[i].u;
        attribute[i][1] = vertex[i].v;
        attribute[i][2] = (colors[i] >> 16) & 255u;
        attribute[i][3] = (colors[i] >> 8) & 255u;
        attribute[i][4] = colors[i] & 255u;
    }
    if (max_x - min_x > 1023 || max_y - min_y > 511)
        return;
    determinant = (sint64)(vb.x - va.x) * (vc.y - va.y) - (sint64)(vc.x - va.x) * (vb.y - va.y);
    if (!determinant || min_y == max_y)
        return;
    anchor = vb.x <= va.x ? (vc.x <= vb.x ? 2 : 1) : (vc.x < va.x ? 2 : 0);
    for (i = 0; i < 5; ++i)
    {
        sint64 numerator_x = (sint64)(attribute[1][i] - attribute[0][i]) * (vc.y - va.y) - (sint64)(attribute[2][i] - attribute[0][i]) * (vb.y - va.y);
        sint64 numerator_y = (sint64)(vb.x - va.x) * (attribute[2][i] - attribute[0][i]) - (sint64)(vc.x - va.x) * (attribute[1][i] - attribute[0][i]);
        /* Quantize gradients before placing them in the wrapping accumulator */
        step_x[i] = (uint32)(numerator_x * 4096 / determinant) * 4096u;
        step_y[i] = (uint32)(numerator_y * 4096 / determinant) * 4096u;
        start[i] = (uint32)attribute[anchor][i] * 0x1000000u + 0x800000u - step_x[i] * (uint32)vertex[anchor].x - step_y[i] * (uint32)vertex[anchor].y;
    }
    for (i = 1; i < 3; ++i)
        for (j = i; j > 0 && sorted[j].y < sorted[j - 1].y; --j)
        {
            RV temporary = sorted[j];
            sorted[j] = sorted[j - 1];
            sorted[j - 1] = temporary;
        }
    {
        sint64 dx = sorted[2].x - sorted[0].x;
        sint64 dy = sorted[2].y - sorted[0].y;
        long_step = (dx * 0x100000000LL + (dx < 0 ? -(dy - 1) : dx > 0 ? dy - 1 : 0)) / dy;
    }
    if (min_y < raster_clip_y0)
        min_y = raster_clip_y0;
    if (min_y < -raster_surface_y)
        min_y = -raster_surface_y;
    if (max_y > raster_clip_y1)
        max_y = raster_clip_y1;
    if (max_y > VRAM_H - raster_surface_y)
        max_y = VRAM_H - raster_surface_y;
    for (y = min_y; y < max_y; ++y)
    {
        sint32 lower = y < sorted[1].y ? 0 : 1;
        sint64 dx = sorted[lower + 1].x - sorted[lower].x;
        sint64 dy = sorted[lower + 1].y - sorted[lower].y;
        sint64 short_step = (dx * 0x100000000LL + (dx < 0 ? -(dy - 1) : dx > 0 ? dy - 1 : 0)) / dy;
        sint64 edge0 = (sint64)sorted[0].x * 0x100000000LL + 0xfffff800LL + long_step * (y - sorted[0].y);
        sint64 edge1 = (sint64)sorted[lower].x * 0x100000000LL + 0xfffff800LL + short_step * (y - sorted[lower].y);
        sint32 left = (sint32)(edge0 >> 32), right = (sint32)(edge1 >> 32), x;
        uint32 value[5];
        if (left > right)
        {
            sint32 temporary = left;
            left = right;
            right = temporary;
        }
        if (left < raster_clip_x0)
            left = raster_clip_x0;
        if (left < -raster_surface_x)
            left = -raster_surface_x;
        if (right > raster_clip_x1)
            right = raster_clip_x1;
        if (right > VRAM_W - raster_surface_x)
            right = VRAM_W - raster_surface_x;
        for (i = 0; i < 5; ++i)
            value[i] = start[i] + step_x[i] * (uint32)left + step_y[i] * (uint32)y;
        for (x = left; x < right; ++x)
        {
            sint32 channel[3], shade, transparent = 0;
            sint32 physical_x = x + raster_surface_x, physical_y = y + raster_surface_y;
            uint16 texture = 0, foreground = 0, background = VRAM[physical_y * VRAM_W + physical_x];
            uint16 mask = 0;
            shade = ((active_draw_flags & 0x200u) && (shaded || (textured && !raw))) ? dither[physical_y & 3][physical_x & 3] : 0;
            if (textured)
            {
                texture = texel_indexed((uint8)(value[0] >> 24), (uint8)(value[1] >> 24), tp, cl, &transparent);
                mask = texture & 0x8000u;
            }
            if (!transparent)
            {
                for (i = 0; i < 3; ++i)
                {
                    sint32 color = (uint8)(value[i + 2] >> 24);
                    if (textured)
                    {
                        sint32 sample = (texture >> (i * 5)) & 31u;
                        color = raw ? sample * 8 : ((sample * color) >> 4) + shade;
                    }
                    else
                        color += shade;
                    color >>= 3;
                    channel[i] = color < 0 ? 0 : color > 31 ? 31 : color;
                }
                if (semi && (!textured || mask))
                {
                    sint32 mode = ((textured ? tp : active_tpage) >> 5) & 3;
                    for (i = 0; i < 3; ++i)
                    {
                        sint32 back = (background >> (i * 5)) & 31u, color;
                        if (!mode)
                            color = (back + channel[i]) >> 1;
                        else if (mode == 1)
                            color = back + channel[i];
                        else if (mode == 2)
                            color = back - channel[i];
                        else
                            color = back + (channel[i] >> 2);
                        channel[i] = color < 0 ? 0 : color > 31 ? 31 : color;
                    }
                }
                foreground = mask | (uint16)(channel[0] | channel[1] << 5 | channel[2] << 10);
                VRAM[physical_y * VRAM_W + physical_x] = foreground;
            }
            for (i = 0; i < 5; ++i)
                value[i] += step_x[i];
        }
    }
}

static void textured_triangle(RV a, RV b, RV c, uint16 tp, uint16 cl, uint32 ca, uint32 cb, uint32 cc, sint32 raw, sint32 semi)
{
    scan_triangle(a, b, c, tp, cl, ca, cb, cc, 1, raw, semi, 1);
}

static void colored_triangle(RV a, RV b, RV c, uint32 ca, uint32 cb, uint32 cc, sint32 semi, sint32 shaded)
{
    scan_triangle(a, b, c, active_tpage, 0, ca, cb, cc, 0, 0, semi, shaded);
}

static sint32 gpu_sign_extend_11(uint32 value)
{
    value &= 0x7ffu;
    return (value & 0x400u) ? (sint32)(value | ~0x7ffu) : (sint32)value;
}

static sint32 gpu_rectangle_coordinate(uint16 value, sint32 offset, sint32 surface)
{
    return gpu_sign_extend_11((uint32)((sint32)value + offset + surface)) - surface;
}

static RV rv(uint8 *p, sint32 xy, sint32 uv)
{
    RV v;
    v.x = gpu_sign_extend_11((uint16)read_s16_le_at(p, xy)) + raster_offset_x;
    v.y = gpu_sign_extend_11((uint16)read_s16_le_at(p, xy + 2)) + raster_offset_y;
    v.u = p[uv];
    v.v = p[uv + 1];
    return v;
}

static void magenta_bbox(uint8 *p, sint32 n, const sint32 *ofs)
{
    sint32 i, minx = 32767, miny = 32767, maxx = -32768, maxy = -32768, x, y;
    for (i = 0; i < n; i++)
    {
        x = read_s16_le_at(p, ofs[i]);
        y = read_s16_le_at(p, ofs[i] + 2);
        if (x < minx)
            minx = x;
        if (x > maxx)
            maxx = x;
        if (y < miny)
            miny = y;
        if (y > maxy)
            maxy = y;
    }
    if (minx < 0)
        minx = 0;
    if (miny < 0)
        miny = 0;
    if (maxx >= VRAM_W)
        maxx = VRAM_W - 1;
    if (maxy >= VRAM_H)
        maxy = VRAM_H - 1;
    for (y = miny; y <= maxy; y++)
        for (x = minx; x <= maxx; x++)
            pxc(x, y, 0xff00ff);
}

static sint32 gpu_draw_state_word(uint32 word)
{
    switch (word >> 24)
    {
        case 0xe1:
            active_tpage = (uint16)(word & 0x3fffu);
            active_draw_flags = (uint16)(word & 0x600u);
            return 1;
        case 0xe2:
            texture_window = word & 0xfffffu;
            return 1;
        case 0xe3:
            raster_clip_x0 = (sint32)(word & 0x3ffu) - raster_surface_x;
            raster_clip_y0 = (sint32)((word >> 10) & 0x1ffu) - raster_surface_y;
            return 1;
        case 0xe4:
            raster_clip_x1 = (sint32)(word & 0x3ffu) + 1 - raster_surface_x;
            raster_clip_y1 = (sint32)((word >> 10) & 0x1ffu) + 1 - raster_surface_y;
            return 1;
        case 0xe5:
            raster_offset_x = gpu_sign_extend_11(word) - raster_surface_x;
            raster_offset_y = gpu_sign_extend_11(word >> 11) - raster_surface_y;
            return 1;
        case 0xe6:
            return 1;
        default:
            return 0;
    }
}

static void gpu_draw_command(void *raw);

static void draw_prim(void *raw)
{
    const uint32 *words = (const uint32 *)raw;
    uint32 remaining = words[0] >> 24;
    uint32 cursor = 1u;
    while (remaining)
    {
        uint32 command = words[cursor] >> 24;
        uint32 length = 1u;
        uint32 packet[256];
        if ((command & 0xE0u) == 0x20u)
        {
            uint32 vertices = command & 8u ? 4u : 3u;
            length = 1u + vertices * (1u + ((command & 4u) != 0u)) + (command & 16u ? vertices - 1u : 0u);
        }
        else if ((command & 0xE0u) == 0x40u)
        {
            length = command & 16u ? 4u : 3u;
            if (command & 8u)
            {
                uint32 stride = command & 16u ? 2u : 1u;
                length = command & 16u ? 4u : 3u;
                while (length < remaining && (words[cursor + length] & 0xF000F000u) != 0x50005000u)
                    length += stride;
                if (length < remaining)
                    ++length;
                else
                    length = remaining;
            }
        }
        else if ((command & 0xE0u) == 0x60u)
            length = 2u + ((command & 4u) != 0u) + ((command & 0x18u) == 0u);
        else if ((command & 0xE0u) == 0x80u)
            length = 4u;
        else if (command == 2u)
            length = 3u;
        else if ((command & 0xE0u) == 0xA0u)
        {
            if (remaining < 3u)
                return;
            uint32 dimensions = words[cursor + 2u];
            uint32 width = ((dimensions - 1u) & 0x3FFu) + 1u;
            uint32 height = (((dimensions >> 16) - 1u) & 0x1FFu) + 1u;
            length = 3u + (width * height + 1u) / 2u;
        }
        else if ((command & 0xE0u) == 0xC0u)
            length = 3u;
        if (length > remaining)
        {
#ifndef NDEBUG
            fprintf(stderr, "Truncated GPU command %02X: %u of %u words\n", command, remaining, length);
#endif
            abort();
        }
        packet[0] = length << 24;
        memcpy(packet + 1u, words + cursor, length * sizeof(uint32));
        gpu_draw_command(packet);
        cursor += length;
        remaining -= length;
    }
}

static void gpu_draw_command(void *raw)
{
    uint8 *p = (uint8 *)raw;
    sint32 code = p[7] & 0xfc;
    uint32 *w = (uint32 *)p;
    uint32 ca, cb, cc, cd;
    /* Direct VRAM sampling has no texture cache to invalidate */
    if (p[7] == 0x01u)
    {
        uint32 count = w[0] >> 24;
        if (count > 1u)
        {
            uint32 remainder[256];
            remainder[0] = ((count - 1u) << 24) | (w[0] & 0x00ffffffu);
            memcpy(remainder + 1u, w + 2u, (count - 1u) * sizeof(uint32));
            draw_prim(remainder);
        }
        return;
    }
    /* GP0 NOP has no drawing or state effect */
    if (p[7] == 0x00)
        return;
    if (p[7] == 2)
    {
        XportDeviceOperation operation = {0};
        uint32 x = w[2] & 0x3f0;
        uint32 y = (w[2] >> 16) & 511;
        uint32 width = ((w[3] & 1023) + 15) & ~15;
        uint32 height = (w[3] >> 16) & 511;
        uint16 color = (uint16)(((w[1] >> 3) & 31) | ((w[1] >> 6) & 0x3e0) | ((w[1] >> 9) & 0x7c00));
        uint32 row, column, interlaced, active_field;
        if (!width || !height || gpu_frame_skipped())
            return;
        gpu_scanout_poll();
        interlaced = (native_scanout.mode & 0x24) == 0x24 && !(active_draw_flags & 0x400);
        active_field = (native_scanout.disp_y + native_scanout.disp_field) & 1;
        if (xport_device_trace_enabled())
        {
            xport_device_trace_begin(&operation, 0, XPORT_DEVICE_GPU, XPORT_DEVICE_VRAM_FILL, XPORT_DEVICE_DIRECTION_INTERNAL, 2, 2);
            xport_device_trace_stage(&operation, XPORT_DEVICE_SUBMIT, XPORT_DEVICE_EVIDENCE_EXACT, XPORT_DEVICE_REGION_NONE, 0, XPORT_DEVICE_REGION_VRAM, (y * GPU_VRAM_WIDTH + x) * 2, width * height * 2, 0xffffffff);
        }
        gpu_vram_invalidate();
        for (row = 0; row < height; ++row)
        {
            uint32 destination_y = (y + row) & 511;
            if (interlaced && (destination_y & 1) == active_field)
                continue;
            for (column = 0; column < width; ++column)
                VRAM[destination_y * GPU_VRAM_WIDTH + ((x + column) & 1023)] = color;
            gpu_trace_vram_row(&operation, XPORT_DEVICE_REGION_NONE, 0, (sint32)x, (sint32)destination_y, (sint32)width);
        }
        if (operation.active)
        {
            xport_device_trace_stage(&operation, XPORT_DEVICE_RASTER_COMPLETE, XPORT_DEVICE_EVIDENCE_EXACT, XPORT_DEVICE_REGION_NONE, 0, XPORT_DEVICE_REGION_VRAM, 0, 0, 0);
            xport_device_trace_complete(&operation, 1);
        }
        return;
    }
    if ((p[7] & 0xe0) == 0xa0)
    {
        PSX_RECT rectangle;
        rectangle.x = (sint16)(w[2] & 1023);
        rectangle.y = (sint16)((w[2] >> 16) & 511);
        rectangle.w = (sint16)(((w[3] - 1) & 1023) + 1);
        rectangle.h = (sint16)((((w[3] >> 16) - 1) & 511) + 1);
        LoadImagePSX(&rectangle, w + 4);
        return;
    }
    if ((p[7] & 0xe0u) == 0x80u)
    {
        PSX_RECT source;
        XportDeviceOperation operation = {0};
        sint32 destination_x = (sint32)(w[3] & 0x3ffu);
        sint32 destination_y = (sint32)((w[3] >> 16) & 0x1ffu);
        if ((w[0] >> 24) != 4u)
            abort();
        source.x = (sint16)(w[2] & 0x3ffu);
        source.y = (sint16)((w[2] >> 16) & 0x1ffu);
        source.w = (sint16)(((w[4] - 1u) & 0x3ffu) + 1u);
        source.h = (sint16)((((w[4] >> 16) - 1u) & 0x1ffu) + 1u);
        gpu_vram_copy(&source, destination_x, destination_y, &operation);
        return;
    }
    if (gpu_draw_state_word(w[1]))
    {
        uint32 count = w[0] >> 24;
        uint32 index = 2u;
        uint32 remainder[256];
        // Consume state commands and GP0 NOPs before the following primitive
        while (index <= count)
        {
            if ((w[index] >> 24u) == 0u || (w[index] >> 24u) == 1u)
                ++index;
            else if (gpu_draw_state_word(w[index]))
                ++index;
            else
                break;
        }
        if (index <= count)
        {
            uint32 remaining = count - index + 1u;
            remainder[0] = (remaining << 24u) | (w[0] & 0x00ffffffu);
            memcpy(remainder + 1u, w + index, remaining * sizeof(uint32));
            draw_prim(remainder);
        }
        return;
    }
    if (code >= 0x20 && code <= 0x3c && (code & 4))
    {
        uint16 mode = (uint16)read_s16_le_at(p, (code & 0x10) ? 26 : 22);
        /* Polygon texture attributes also select the page for following sprites */
        gpu_set_polygon_tpage(mode);
    }
    if (gpu_frame_skipped() || !raster_enabled())
        return;
    ca = packet_rgb(p, 4);
    // Flat lines and polylines share clipping, draw offset and blending
    if ((code & 0xf8) == 0x40 || (code & 0xf8) == 0x48)
    {
        uint32 count = code & 8 ? w[0] >> 24 : 3u;
        uint32 index;
        for (index = 3u; index <= count; ++index)
        {
            // A terminator ends the polyline without becoming a vertex
            if ((code & 8) && (w[index] & 0xf000f000u) == 0x50005000u)
                break;
            RV a = rv(p, (sint32)(4u * (index - 1u)), 0), b = rv(p, (sint32)(4u * index), 0);
            sint32 dx = abs(b.x - a.x), dy = -abs(b.y - a.y);
            sint32 sx = a.x < b.x ? 1 : -1, sy = a.y < b.y ? 1 : -1;
            sint32 err = dx + dy, e;
            if (dx >= 1024 || dy <= -512)
                continue;
            for (;;)
            {
                if ((uint32)(a.x + raster_surface_x) < VRAM_W && (uint32)(a.y + raster_surface_y) < VRAM_H)
                {
                    uint32 color = ca;
                    if (p[7] & 2)
                        color = semi_blend(gpu_raster_pixel(a.x, a.y), color, (active_tpage >> 5) & 3);
                    pxc(a.x, a.y, color);
                }
                if (a.x == b.x && a.y == b.y)
                    break;
                e = 2 * err;
                if (e >= dy)
                {
                    err += dy;
                    a.x += sx;
                }
                if (e <= dx)
                {
                    err += dx;
                    a.y += sy;
                }
            }
        }
        return;
    }
    if (code == 0x50)
    {
        RV a = rv(p, 8, 0), b = rv(p, 16, 0);
        sint32 dx = abs(b.x - a.x), dy = -abs(b.y - a.y);
        sint32 sx = a.x < b.x ? 1 : -1, sy = a.y < b.y ? 1 : -1;
        sint32 steps = dx > -dy ? dx : -dy, step = 0;
        sint32 err = dx + dy, e;
        cb = packet_rgb(p, 12);
        if (dx >= 1024 || dy <= -512)
            return;
        for (;;)
        {
            uint32 color = 0u, channel;
            for (channel = 0u; channel < 3u; ++channel)
            {
                uint32 shift = channel * 8u;
                sint32 first = (sint32)((ca >> shift) & 255u);
                sint32 last = (sint32)((cb >> shift) & 255u);
                sint32 value = steps ? (first * (steps - step) + last * step) / steps : first;
                color |= (uint32)value << shift;
            }
            if ((uint32)(a.x + raster_surface_x) < VRAM_W && (uint32)(a.y + raster_surface_y) < VRAM_H)
            {
                if (p[7] & 2u)
                    color = semi_blend(gpu_raster_pixel(a.x, a.y), color, (active_tpage >> 5) & 3);
                pxc(a.x, a.y, color);
            }
            if (a.x == b.x && a.y == b.y)
                break;
            e = 2 * err;
            if (e >= dy)
            {
                err += dy;
                a.x += sx;
            }
            if (e <= dx)
            {
                err += dx;
                a.y += sy;
            }
            ++step;
        }
        return;
    }
    if (code == 0x20)
    {
        RV a = rv(p, 8, 0), b = rv(p, 12, 0), c = rv(p, 16, 0);
        colored_triangle(a, b, c, ca, ca, ca, (p[7] & 2) != 0, 0);
        return;
    }
    if (code == 0x28)
    {
        RV a = rv(p, 8, 0), b = rv(p, 12, 0), c = rv(p, 16, 0), d = rv(p, 20, 0);
        colored_triangle(a, b, c, ca, ca, ca, (p[7] & 2) != 0, 0);
        colored_triangle(b, c, d, ca, ca, ca, (p[7] & 2) != 0, 0);
        return;
    }
    if (code == 0x30)
    {
        RV a = rv(p, 8, 0), b = rv(p, 16, 0), c = rv(p, 24, 0);
        cb = packet_rgb(p, 12);
        cc = packet_rgb(p, 20);
        colored_triangle(a, b, c, ca, cb, cc, (p[7] & 2) != 0, 1);
        return;
    }
    if (code == 0x38)
    {
        RV a = rv(p, 8, 0), b = rv(p, 16, 0), c = rv(p, 24, 0), d = rv(p, 32, 0);
        cb = packet_rgb(p, 12);
        cc = packet_rgb(p, 20);
        cd = packet_rgb(p, 28);
        colored_triangle(a, b, c, ca, cb, cc, (p[7] & 2) != 0, 1);
        colored_triangle(b, c, d, cb, cc, cd, (p[7] & 2) != 0, 1);
        return;
    }
    if (code == 0x24)
    {
        RV a = rv(p, 8, 12), b = rv(p, 16, 20), c = rv(p, 24, 28);
        textured_triangle(a, b, c, *(uint16 *)(p + 22), *(uint16 *)(p + 14), ca, ca, ca, p[7] & 1, (p[7] & 2) != 0);
        return;
    }
    if (code == 0x2c)
    {
        RV a = rv(p, 8, 12), b = rv(p, 16, 20), c = rv(p, 24, 28), d = rv(p, 32, 36);
        uint16 tp = *(uint16 *)(p + 22), cl = *(uint16 *)(p + 14);
        textured_triangle(a, b, c, tp, cl, ca, ca, ca, p[7] & 1, (p[7] & 2) != 0);
        textured_triangle(b, c, d, tp, cl, ca, ca, ca, p[7] & 1, (p[7] & 2) != 0);
        return;
    }
    if (code == 0x34)
    {
        RV a = rv(p, 8, 12), b = rv(p, 20, 24), c = rv(p, 32, 36);
        uint16 tp = *(uint16 *)(p + 26), cl = *(uint16 *)(p + 14);
        cb = packet_rgb(p, 16);
        cc = packet_rgb(p, 28);
        textured_triangle(a, b, c, tp, cl, ca, cb, cc, p[7] & 1, (p[7] & 2) != 0);
        return;
    }
    if (code == 0x3c)
    {
        RV a = rv(p, 8, 12), b = rv(p, 20, 24), c = rv(p, 32, 36), d = rv(p, 44, 48);
        uint16 tp = *(uint16 *)(p + 26), cl = *(uint16 *)(p + 14);
        cb = packet_rgb(p, 16);
        cc = packet_rgb(p, 28);
        cd = packet_rgb(p, 40);
        textured_triangle(a, b, c, tp, cl, ca, cb, cc, p[7] & 1, (p[7] & 2) != 0);
        textured_triangle(b, c, d, tp, cl, cb, cc, cd, p[7] & 1, (p[7] & 2) != 0);
        return;
    }
    if (code == 0x64 || code == 0x6c || code == 0x74 || code == 0x7c)
    {
        sint32 x, y, x0 = gpu_rectangle_coordinate((uint16)read_s16_le_at(p, 8), raster_offset_x, raster_surface_x), y0 = gpu_rectangle_coordinate((uint16)read_s16_le_at(p, 10), raster_offset_y, raster_surface_y), ww = code == 0x64 ? (sint32)((uint16)read_s16_le_at(p, 16) & 0x3ffu) : (code == 0x6c ? 1 : code == 0x74 ? 8 : 16), hh = code == 0x64 ? (sint32)((uint16)read_s16_le_at(p, 18) & 0x1ffu) : (code == 0x6c ? 1 : code == 0x74 ? 8 : 16);
        for (y = 0; y < hh; y++)
            for (x = 0; x < ww; x++)
            {
                sint32 transparent;
                uint16 t = texel_indexed(p[12] + x, p[13] + y, active_tpage, *(uint16 *)(p + 14), &transparent);
                if (!transparent && x0 + x >= raster_clip_x0 && x0 + x < raster_clip_x1 && y0 + y >= raster_clip_y0 && y0 + y < raster_clip_y1 && (uint32)(x0 + x + raster_surface_x) < VRAM_W && (uint32)(y0 + y + raster_surface_y) < VRAM_H)
                {
                    uint32 color = modulate(t, p[4], p[5], p[6], p[7] & 1);
                    if ((p[7] & 2) && (t & 0x8000))
                        color = semi_blend(gpu_raster_pixel(x0 + x, y0 + y), color, (active_tpage >> 5) & 3);
                    pxc(x0 + x, y0 + y, color);
                }
            }
        return;
    }
    if (code == 0x60 || code == 0x68 || code == 0x70 || code == 0x78)
    {
        sint32 x, y, x0 = gpu_rectangle_coordinate((uint16)read_s16_le_at(p, 8), raster_offset_x, raster_surface_x), y0 = gpu_rectangle_coordinate((uint16)read_s16_le_at(p, 10), raster_offset_y, raster_surface_y), ww = code == 0x60 ? (sint32)((uint16)read_s16_le_at(p, 12) & 0x3ffu) : (code == 0x68 ? 1 : code == 0x70 ? 8 : 16), hh = code == 0x60 ? (sint32)((uint16)read_s16_le_at(p, 14) & 0x1ffu) : (code == 0x68 ? 1 : code == 0x70 ? 8 : 16);
        for (y = 0; y < hh; y++)
            for (x = 0; x < ww; x++)
            {
                uint32 color = ca;
                if ((uint32)(x0 + x + raster_surface_x) >= VRAM_W || (uint32)(y0 + y + raster_surface_y) >= VRAM_H)
                    continue;
                if (p[7] & 2)
                    color = semi_blend(gpu_raster_pixel(x0 + x, y0 + y), color, (active_tpage >> 5) & 3);
                pxc(x0 + x, y0 + y, color);
            }
        return;
    }
#if !defined(NDEBUG)
    {
        static uint8 reported[256];
        if (!reported[p[7]])
        {
            reported[p[7]] = 1u;
    #ifndef NDEBUG
            fprintf(stderr, "gpu_unsupported_primitive address=%08x tag=%08x code=%02x words=%u\n", gpu_debug_packet_address, gpu_debug_packet_tag, p[7], gpu_debug_packet_tag >> 24);
    #endif
        }
    }
#endif
}

typedef struct GpuPortState
{
    uint32 packet[256], count, needed, polyline;
    uint32 direction, disabled, irq, mask, latch;
    uint32 x, y, width, height, cursor, transfer;
    uint32 horizontal_range;
    XportDeviceOperation upload;
} GpuPortState;

static GpuPortState gpu_port = {{0}, 0, 0, 0, 0, 1};

static void gpu_port_clear_fifo(void)
{
    if (gpu_port.upload.active)
        xport_device_trace_complete(&gpu_port.upload, 0);
    gpu_port.count = gpu_port.needed = gpu_port.polyline = 0;
    gpu_port.cursor = gpu_port.transfer = 0;
}

static uint32 gpu_port_pixel_address(void)
{
    uint32 x = (gpu_port.x + gpu_port.cursor % gpu_port.width) & 1023;
    uint32 y = (gpu_port.y + gpu_port.cursor / gpu_port.width) & 511;
    return y * 1024 + x;
}

static sint32 gpu_port_gp0(uint32 value)
{
    uint32 command, vertices, index;
    if (gpu_port.transfer == 2)
        return 0;
    if (gpu_port.transfer == 1)
    {
        for (index = 0; index < 2; ++index)
        {
            uint32 address = gpu_port_pixel_address();
            uint16 pixel = (uint16)(value >> (index * 16));
            if (!(gpu_port.mask & 2) || !(VRAM[address] & 0x8000))
            {
                VRAM[address] = pixel | ((gpu_port.mask & 1) << 15);
                if (gpu_port.upload.active)
                    xport_device_trace_stage(&gpu_port.upload, XPORT_DEVICE_WRITE, XPORT_DEVICE_EVIDENCE_EXACT, XPORT_DEVICE_REGION_NONE, 0, XPORT_DEVICE_REGION_VRAM, address * 2, 2, 3);
            }
            if (++gpu_port.cursor == gpu_port.width * gpu_port.height)
            {
                gpu_port.transfer = 0;
                if (gpu_port.upload.active)
                    xport_device_trace_complete(&gpu_port.upload, 1);
                break;
            }
        }
        gpu_vram_invalidate();
        return 1;
    }
    if (!gpu_port.count)
    {
        command = value >> 24;
        /* Gouraud polylines and this line alias are not implemented by the raster core */
        if ((command & 0xF8) == 0x58 || (command & 0xFC) == 0x54)
            return 0;
        gpu_port.needed = 1;
        if ((command & 0xE0) == 0x20)
        {
            vertices = command & 8 ? 4 : 3;
            gpu_port.needed = 1 + vertices * (1 + ((command & 4) != 0)) + (command & 16 ? vertices - 1 : 0);
        }
        else if ((command & 0xE0) == 0x40)
        {
            gpu_port.needed = command & 16 ? 4 : 3;
            gpu_port.polyline = (command & 8) != 0;
        }
        else if ((command & 0xE0) == 0x60)
            gpu_port.needed = 2 + ((command & 4) != 0) + ((command & 0x18) == 0);
        else if ((command & 0xE0) == 0x80)
            gpu_port.needed = 4;
        else if ((command & 0xE0) == 0xA0 || (command & 0xE0) == 0xC0 || command == 2)
            gpu_port.needed = 3;
        else if (command > 0x1F && (command < 0xE0 || command > 0xEF))
            return 0;
    }
    if (gpu_port.count == 255)
        return 0;
    gpu_port.packet[++gpu_port.count] = value;
    command = gpu_port.packet[1] >> 24;
    if (gpu_port.count < gpu_port.needed)
        return 1;
    if (gpu_port.polyline)
    {
        uint32 stride = command & 16 ? 2 : 1;
        if (gpu_port.count == gpu_port.needed || ((gpu_port.count - gpu_port.needed - 1) % stride) || (value & 0xF000F000) != 0x50005000)
            return 1;
    }
    if ((command & 0xE0) == 0xA0 || (command & 0xE0) == 0xC0)
    {
        gpu_port.x = gpu_port.packet[2] & 1023;
        gpu_port.y = (gpu_port.packet[2] >> 16) & 511;
        gpu_port.width = ((gpu_port.packet[3] - 1) & 1023) + 1;
        gpu_port.height = (((gpu_port.packet[3] >> 16) - 1) & 511) + 1;
        gpu_port.cursor = 0;
        gpu_port.transfer = (command & 0xE0) == 0xA0 ? 1 : 2;
        if (gpu_port.transfer == 1 && xport_device_trace_enabled())
        {
            xport_device_trace_begin(&gpu_port.upload, 0, XPORT_DEVICE_GPU, XPORT_DEVICE_VRAM_UPLOAD, XPORT_DEVICE_DIRECTION_TO_DEVICE, 2, command);
            xport_device_trace_stage(&gpu_port.upload, XPORT_DEVICE_SUBMIT, XPORT_DEVICE_EVIDENCE_CANDIDATE, XPORT_DEVICE_REGION_NONE, 0, XPORT_DEVICE_REGION_VRAM, (gpu_port.y * 1024 + gpu_port.x) * 2, gpu_port.width * gpu_port.height * 2, 3);
        }
    }
    else if (command == 0x1F)
    {
        gpu_port.irq = 1;
    }
    else if (command == 0xE6)
        gpu_port.mask = value & 3;
    else if (command == 0 || command == 1 || (command >= 3 && command <= 0x1E) || command == 0xE0 || (command >= 0xE7 && command <= 0xEF))
    {
        /* NOP mirrors and texture-cache invalidation need no raster work */
    }
    else
    {
        /* The raster core does not yet implement masked primitive/copy writes */
        if (gpu_port.mask && command < 0xA0 && command != 2)
            return 0;
        gpu_port.packet[0] = gpu_port.count << 24;
        gpu_draw_command(gpu_port.packet);
    }
    gpu_port.count = gpu_port.needed = gpu_port.polyline = 0;
    return 1;
}

static sint32 gpu_port_gp1(uint32 value)
{
    uint32 command = (value >> 24) & 63;
    uint32 index = value & 15;
    if (command == 0)
    {
        gpu_port_clear_fifo();
        gpu_reset_graph_state();
        memset(&gpu_port, 0, sizeof(gpu_port));
        gpu_port.disabled = 1;
        gpu_port.horizontal_range = 0xC60260;
        raster_clip_x1 = raster_clip_y1 = 1;
        display_state = 2;
    }
    else if (command == 1)
        gpu_port_clear_fifo();
    else if (command == 2)
        gpu_port.irq = 0;
    else if (command == 3)
    {
        gpu_port.disabled = value & 1;
        display_state = (display_state & ~2) | (gpu_port.disabled << 1);
    }
    else if (command == 4)
        gpu_port.direction = value & 3;
    else if (command >= 5 && command <= 8)
    {
        gpu_scanout_poll();
        gpu_scanout_command(&native_scanout, (command << 24) | (value & 0xFFFFFF));
        if (command == 5)
        {
            display_x = value & 1023;
            display_y = (value >> 10) & 511;
        }
        else if (command == 6)
            gpu_port.horizontal_range = value & 0xFFFFFF;
        else if (command == 8)
        {
            static const uint32 widths[4] = {256, 320, 512, 640};
            display_width = value & 64 ? 368 : widths[value & 3];
            display_height = (value & 0x24) == 0x24 ? 480 : 240;
            display_state = (display_state & ~4) | ((value & 16) >> 2);
        }
    }
    else if (command >= 0x10 && command <= 0x1F)
    {
        if (index == 2)
            gpu_port.latch = texture_window;
        else if (index == 3)
            gpu_port.latch = (raster_clip_x0 + raster_surface_x) | ((raster_clip_y0 + raster_surface_y) << 10);
        else if (index == 4)
            gpu_port.latch = (raster_clip_x1 - 1 + raster_surface_x) | ((raster_clip_y1 - 1 + raster_surface_y) << 10);
        else if (index == 5)
            gpu_port.latch = ((raster_offset_x + raster_surface_x) & 2047) | (((raster_offset_y + raster_surface_y) & 2047) << 11);
        else if (index == 7)
            gpu_port.latch = 2;
        else if (index == 8)
            gpu_port.latch = 0;
    }
    return 1;
}

sint32 gpu_port_read(uint32 offset, uint32 *value)
{
    uint32 index, status, ready, mode;
    if (!value || (offset != 0 && offset != 4))
        return 0;
    if (!offset)
    {
        if (gpu_port.transfer == 2)
        {
            gpu_port.latch = 0;
            for (index = 0; index < 2; ++index)
            {
                gpu_port.latch |= (uint32)VRAM[gpu_port_pixel_address()] << (index * 16);
                if (++gpu_port.cursor == gpu_port.width * gpu_port.height)
                {
                    gpu_port.transfer = 0;
                    break;
                }
            }
        }
        *value = gpu_port.latch;
        return 1;
    }
    gpu_scanout_poll();
    mode = native_scanout.mode;
    ready = gpu_port.transfer != 2 && gpu_port.count < 255;
    status = (active_tpage & 0x7FF) | (gpu_port.mask << 11) | ((mode & 32 ? native_scanout.field : 1) << 13) | ((mode & 128) << 7) | ((mode & 64) << 10) | ((mode & 63) << 17) | (gpu_port.disabled << 23) | (gpu_port.irq << 24) | (gpu_port.direction << 29);
    status |= (gpu_port.transfer == 2 ? 0x08000000 : 0) | (ready << 28);
    if (!gpu_port.count && !gpu_port.transfer)
        status |= 0x04000000;
    if ((gpu_port.direction == 3 && gpu_port.transfer == 2) || ((gpu_port.direction == 1 || gpu_port.direction == 2) && ready))
        status |= 0x02000000;
    if (native_scanout.line >= native_scanout.start_y && native_scanout.line < native_scanout.end_y)
        status |= (uint32)gpu_scanout_ode(&native_scanout) << 31;
    *value = status;
    return 1;
}

sint32 gpu_port_write(uint32 offset, uint32 value)
{
    if (offset == 0)
        return gpu_port_gp0(value);
    if (offset == 4)
        return gpu_port_gp1(value);
    return 0;
}

void gpu_begin(void)
{
}

static uint32 soft_reset_head, soft_reset_tail;

sint32 gpu_bind_soft_reset(uint32 queue_head, uint32 queue_tail)
{
    uint32 addresses[2] = {queue_head, queue_tail}, index;
    for (index = 0u; index < 2u; ++index)
    {
        uint32 segment = addresses[index] >> 29;
        uint32 physical = addresses[index] & 0x1fffffffu;
        if ((segment != 0u && segment != 4u && segment != 5u) || (physical & 3u) || physical > PSX_DRAM_SIZE - 4u)
            return 0;
    }
    if ((queue_head & 0x1fffffffu) == (queue_tail & 0x1fffffffu))
        return 0;
    soft_reset_head = queue_head;
    soft_reset_tail = queue_tail;
    return 1;
}

sint32 ResetGraph(sint32 mode)
{
    if (mode == 1 && transfer_observer && soft_reset_head && soft_reset_tail && !linked_submit)
    {
        uint32 command = 0x02000000u;
        // Synchronous observation has no active DMA transfer to cancel
        *(uint32 *)psx_addr(soft_reset_tail, 4u) = 0u;
        *(uint32 *)psx_addr(soft_reset_head, 4u) = 0u;
        transfer_observer(transfer_observer_context, 1u, 4u, &command, 1u);
        command = 0x01000000u;
        transfer_observer(transfer_observer_context, 1u, 4u, &command, 1u);
        return 0;
    }
    if (mode == 1 && !transfer_observer && !linked_submit && !sync_sample && soft_reset_head && soft_reset_tail)
    {
        /* Native packets complete synchronously, with no active DMA or partial FIFO */
        *(uint32 *)psx_addr(soft_reset_tail, 4u) = 0u;
        *(uint32 *)psx_addr(soft_reset_head, 4u) = 0u;
        if (!psx_irq_native_acknowledge(1u << 3))
            return -1;
        gpu_psyq_gp1(0x02000000u);
        gpu_psyq_gp1(0x01000000u);
        return 0;
    }
    gpu_unobserved_transfer();
    if (mode != 0)
        return -1;
    gpu_reset_graph_state();
    if (xport_is_headless())
        return 0;
    return xport_window_init() ? 0 : -1;
}

sint32 SetGraphDebug(sint32 level)
{
    sint32 previous = graph_debug_level;
    graph_debug_level = level;
    return previous;
}

sint32 SetGraphReverse(sint32 reverse)
{
    uint8 previous = gpu_graph_control.reverse;
    uint32 mode;
    gpu_graph_control.reverse = (uint8)reverse;
    mode = _getctl(8);
    _ctl(mode | (gpu_graph_control.reverse != 0 ? 0x08000080 : 0x08000000));
    if (GetGraphType() == 2)
        _ctl(gpu_graph_control.reverse != 0 ? 0x20000501 : 0x20000504);
    return previous;
}

sint32 SetGraphQue(sint32 enabled)
{
    uint8 previous = gpu_graph_control.queue_enabled;
    if (enabled != gpu_graph_control.queue_enabled)
    {
        ResetGraph(1);
        gpu_graph_control.queue_enabled = (uint8)enabled;
        DMACallback(2, NULL);
    }
    return previous;
}

uint32 _status(void)
{
    return r_u32(0x1F801814u);
}

void _ctl(uint32 command)
{
    w_u32(0x1F801814u, command);
    gpu_graph_control.command_cache[command >> 24] = (uint8)command;
}

uint32 _getctl(uint32 command)
{
    if (command >= sizeof(gpu_graph_control.command_cache))
    {
#ifndef NDEBUG
        fprintf(stderr, "xport: Invalid GPU control cache index %u\n", command);
#else
        (void)0;
#endif
        abort();
    }
    return gpu_graph_control.command_cache[command];
}

sint32 GetGraphType(void)
{
    // Native rasterizer models the standard PS1 GPU
    if (xport_gpu_graph_type_address == 0u)
        return 0;
    return (sint32)r_u8(xport_gpu_graph_type_address);
}

sint32 DrawSync(sint32 mode)
{
    if (sync_sample)
    {
        uint32 dma, status, pending, remaining = sync_budget;
        sint32 result = -1;
        if (sync_failed || sync_running)
        {
            sync_failed = 1;
            return -1;
        }
        sync_running = 1;
        if (mode)
        {
            pending = (gpu_u32(sync_head) - gpu_u32(sync_tail)) & 63u;
            if (pending && (sync_pump(sync_context) != 1 || sync_failed))
                goto sync_done;
            if (sync_sample(sync_context, &dma, &status) != 1 || sync_failed)
                goto sync_done;
            result = (sint32)pending;
            if (!pending && ((dma & 0x01000000u) || !(status & 0x04000000u)))
                result = 1;
        }
        else
        {
            if (sync_timeout_bound && gpu_psyq_timeout_begin(&sync_timeout) != 1)
                goto sync_done;
            while (gpu_u32(sync_head) != gpu_u32(sync_tail))
            {
                if (!remaining-- || sync_pump(sync_context) != 1 || sync_failed)
                    goto sync_done;
                if (sync_timeout_bound && gpu_psyq_timeout_check(&sync_timeout) != 1)
                    goto sync_done;
                if (gpu_u32(sync_head) != gpu_u32(sync_tail) && (sync_wait(sync_context) != 1 || sync_failed))
                    goto sync_done;
            }
            for (;;)
            {
                if (sync_sample(sync_context, &dma, &status) != 1 || sync_failed)
                    goto sync_done;
                if (!(dma & 0x01000000u) && (status & 0x04000000u))
                    break;
                if (sync_timeout_bound && gpu_psyq_timeout_check(&sync_timeout) != 1)
                    goto sync_done;
                if (!remaining-- || sync_wait(sync_context) != 1 || sync_failed)
                    goto sync_done;
            }
            result = 0;
        }
    sync_done:
        sync_running = 0;
        if (result < 0)
            sync_failed = 1;
        return result;
    }
    return mode == 0 || mode == 1 ? 0 : -1;
}

void SetDispMask(sint32 enabled)
{
    if (psyq_bound)
    {
        if (!enabled)
            memset(psx_addr(psyq_binding.display_cache, 20u), 0xff, 20u);
        gpu_psyq_gp1(enabled ? 0x03000000u : 0x03000001u);
    }
    else
        gpu_unobserved_transfer();
    display_state = (display_state & ~2u) | (enabled ? 0u : 2u);
}

static uint32 gpu_raster_pixel(sint32 x, sint32 y)
{
    sint32 vram_x = x + raster_surface_x;
    sint32 vram_y = y + raster_surface_y;
    if ((uint32)vram_x >= VRAM_W || (uint32)vram_y >= VRAM_H)
        return 0;
    return rgb555(VRAM[vram_y * VRAM_W + vram_x]);
}

/* Host PsyQ DrawOTagEnv adaptation, not a replacement decompilation of the SDK.
 * The caller supplies the VRAM origin of the draw surface (not the displayed
 * surface). This preserves relative clip/offset changes in the 320x240 window.
 * Dithering, draw-to-display inhibition and GPU mask bits remain WIP. */
static void gpu_draw_env_native(DRAWENV *environment, sint32 surface_x, sint32 surface_y)
{
    const uint8 *e = (const uint8 *)environment;
    sint32 x, y;
    raster_surface_x = surface_x;
    raster_surface_y = surface_y;
    raster_clip_x0 = read_s16_le_at(e, 0) - surface_x;
    raster_clip_y0 = read_s16_le_at(e, 2) - surface_y;
    raster_clip_x1 = raster_clip_x0 + read_s16_le_at(e, 4);
    raster_clip_y1 = raster_clip_y0 + read_s16_le_at(e, 6);
    raster_offset_x = read_s16_le_at(e, 8) - surface_x;
    raster_offset_y = read_s16_le_at(e, 10) - surface_y;
    active_tpage = (uint16)read_s16_le_at(e, 20);
    active_draw_flags = (e[22] ? 0x200u : 0u) | (e[23] ? 0x400u : 0u);
    /* Pack the PsyQ texture-window fields into a GP0 command */
    texture_window = (((uint32)(-read_s16_le_at(e, 16)) & 255u) >> 3) | ((((uint32)(-read_s16_le_at(e, 18)) & 255u) >> 3) << 5) | ((uint32)(e[12] >> 3) << 10) | ((uint32)(e[14] >> 3) << 15);
    if (e[24] && !gpu_frame_skipped() && raster_enabled())
    {
        uint32 color = (uint32)e[25] << 16 | (uint32)e[26] << 8 | e[27];
        for (y = raster_clip_y0; y < raster_clip_y1; y++)
            for (x = raster_clip_x0; x < raster_clip_x1; x++)
                pxc(x, y, color);
    }
}

void gpu_draw_env(uint32 env, sint32 surface_x, sint32 surface_y)
{
    gpu_draw_env_native((DRAWENV *)psx_addr(env, sizeof(DRAWENV)), surface_x, surface_y);
}

static sint32 gpu_env_clamp(sint32 value, sint32 limit)
{
    if (value < 0)
        return 0;
    return value > limit - 1 ? limit - 1 : value;
}

static uint32 gpu_env_clip(uint32 command, sint32 x, sint32 y, uint32 wide)
{
    uint32 mask = wide ? 4095u : 1023u, shift = wide ? 12u : 10u;
    x = gpu_env_clamp((sint16)x, VRAM_W);
    y = gpu_env_clamp((sint16)y, VRAM_H);
    return command | ((uint32)x & mask) | (((uint32)y & mask) << shift);
}

/* 700A0 packet writes and 6F828 cache copy, audited from actual MIPS.
 * DMA/interrupt queue bookkeeping remains a separate host SDK adaptation. */
static void gpu_put_draw_env_native(DRAWENV *environment, sint32 surface_x, sint32 surface_y)
{
    uint8 *e = (uint8 *)environment;
    uint8 *packet = e + 28;
    uint32 wide = 0;
    uint32 mask = wide ? 4095u : 2047u, shift = wide ? 12u : 11u, mode, count = 6;
    sint32 x, y, w, h;
    xport_store_le32(packet + 4, gpu_env_clip(0xe3000000u, read_s16_le_at(e, 0), read_s16_le_at(e, 2), wide));
    xport_store_le32(packet + 8, gpu_env_clip(0xe4000000u, (sint16)((uint16)read_s16_le_at(e, 0) + (uint16)read_s16_le_at(e, 4) - 1), (sint16)((uint16)read_s16_le_at(e, 2) + (uint16)read_s16_le_at(e, 6) - 1), wide));
    xport_store_le32(packet + 12, 0xe5000000u | ((uint32)read_s16_le_at(e, 8) & mask) | (((uint32)read_s16_le_at(e, 10) & mask) << shift));
    mode = 0xe1000000u | ((uint16)read_s16_le_at(e, 20) & (wide ? 0x27ffu : 0x9ffu));
    if (e[22])
        mode |= wide ? 0x800u : 0x200u;
    if (e[23])
        mode |= wide ? 0x1000u : 0x400u;
    xport_store_le32(packet + 16, mode);
    xport_store_le32(packet + 20, 0xe2000000u | (((uint32)-read_s16_le_at(e, 16) & 255u) >> 3) | ((((uint32)-read_s16_le_at(e, 18) & 255u) >> 3) << 5) | ((uint32)(e[12] >> 3) << 10) | ((uint32)(e[14] >> 3) << 15));
    xport_store_le32(packet + 24, 0xe6000000u);
    if (e[24])
    {
        x = read_s16_le_at(e, 0);
        y = read_s16_le_at(e, 2);
        w = (sint16)gpu_env_clamp(read_s16_le_at(e, 4), VRAM_W);
        h = (sint16)gpu_env_clamp(read_s16_le_at(e, 6), VRAM_H);
        mode = 0x02000000u;
        if (((uint32)x & 63u) || ((uint32)w & 63u))
        {
            mode = 0x60000000u;
            x = (sint16)(x - read_s16_le_at(e, 8));
            y = (sint16)(y - read_s16_le_at(e, 10));
        }
        xport_store_le32(packet + 28, mode | e[25] | ((uint32)e[26] << 8) | ((uint32)e[27] << 16));
        xport_store_le32(packet + 32, (uint16)x | ((uint32)(uint16)y << 16));
        xport_store_le32(packet + 36, (uint16)w | ((uint32)(uint16)h << 16));
        count = 9;
    }
    xport_store_u8(packet + 3, (uint8)count);
    xport_store_le32(packet, xport_load_le32(packet) | 0xffffffu);
    if (linked_submit)
    {
        uint32 address;
        const void *words = packet;
        if (!gpu_guest_address(words, &address) || address > PSX_DRAM_SIZE - 64u || (address & 3u) || linked_submit(linked_submit_context, address, 64u) != 1)
            linked_submit_failed = 1;
    }
    else if (transfer_observer)
    {
        uint32 address = 0;
        const uint32 *words = (const uint32 *)packet;
        sint32 native = !gpu_guest_address(words, &address);
        gpu_observe_dma_direction();
        gpu_observe_ot(address, words, native);
    }
    if (!linked_submit)
        gpu_draw_env_native(environment, surface_x, surface_y);
    if (psyq_bound)
        memcpy(psx_addr(psyq_binding.draw_cache, 92u), environment, 92u);
    else
    {
        memcpy(&native_draw_cache, environment, sizeof(native_draw_cache));
        native_draw_cached = 1u;
    }
}

void gpu_put_draw_env(uint32 env, sint32 surface_x, sint32 surface_y)
{
    gpu_put_draw_env_native((DRAWENV *)psx_addr(env, sizeof(DRAWENV)), surface_x, surface_y);
}

void gpu_packet(void *packet)
{
    XportDeviceOperation operation = {0};
    uint32 source_offset = 0u;
    XportDeviceRegion source_region = XPORT_DEVICE_REGION_NONE;
    uint32 command = packet ? ((const uint32 *)packet)[0] >> 24 : 0u;
    gpu_unobserved_transfer();
    if (xport_device_trace_enabled() && packet)
    {
        source_region = gpu_trace_pointer(packet, 4u, &source_offset);
        xport_device_trace_begin(&operation, 0u, XPORT_DEVICE_GPU, XPORT_DEVICE_GPU_PACKET, XPORT_DEVICE_DIRECTION_TO_DEVICE, 2u, command);
        xport_device_trace_stage(&operation, XPORT_DEVICE_SUBMIT, XPORT_DEVICE_EVIDENCE_EXACT, source_region, source_offset, XPORT_DEVICE_REGION_NONE, 0u, 0u, 0u);
        xport_device_trace_stage(&operation, XPORT_DEVICE_FETCH, XPORT_DEVICE_EVIDENCE_EXACT, source_region, source_offset, XPORT_DEVICE_REGION_NONE, 0u, 4u, 0xffffffffu);
    }
    draw_prim(packet);
    if (operation.active)
    {
        xport_device_trace_stage(&operation, XPORT_DEVICE_RASTER_COMPLETE, XPORT_DEVICE_EVIDENCE_CANDIDATE, source_region, source_offset, XPORT_DEVICE_REGION_VRAM, 0u, 0u, 0u);
        xport_device_trace_complete(&operation, 1);
    }
}

void DrawPrim(void *primitive)
{
    uint32 count = 0;
    if (transfer_observer)
    {
        uint32 address, index, command = 0x04000000u;
        const uint32 *words = (const uint32 *)primitive;
        if (!gpu_guest_address(primitive, &address) || (address & 3u) || address > PSX_DRAM_SIZE - 4u)
        {
            gpu_unobserved_transfer();
            return;
        }
        count = words[0] >> 24;
        if (count > (PSX_DRAM_SIZE - address - 4u) / 4u)
        {
            gpu_unobserved_transfer();
            return;
        }
        DrawSync(0);
        transfer_observer(transfer_observer_context, 1u, 4u, &command, 1u);
        for (index = 1; index <= count; ++index)
            transfer_observer(transfer_observer_context, 0u, 0u, words + index, 1u);
        if (!count)
            return;
    }
    else
        DrawSync(0);
    draw_prim(primitive);
}

/* Host DMA adapter: consume PSX address tags, never native pointer tags. */
static void gpu_draw_linked_dma(uint32 *ot)
{
    uint32 head;
    uint32 count = 0;
    sint32 native = !gpu_guest_address(ot, &head);
    uint32 native_segment = 0;
    gpu_observe_dma_direction();
    if (native)
    {
        if (gpu_packet_range_address(ot, &head))
            native = 0;
        else
        {
#if defined(AP_X64)
            // Native chains must use explicit registered ranges on x64
            abort();
#else
            head = (uint32)(intptr)ot;
#endif
            native_segment = head & 0xff000000u;
        }
    }
    while ((head & 0xffffffu) != 0xffffffu)
    {
        uint32 link = head & 0xffffffu;
        uint32 address = native ? native_segment | link : link;
        uint32 *packet = 0;
        uint32 tag;
        sint32 packet_native = native;
        uint32 registered_bytes = 0;
#if !defined(NDEBUG)
        size_t packet_size;
#endif
        if ((address & 3u) || count++ >= 65536)
        {
#if !defined(NDEBUG)
    #ifndef NDEBUG
            fprintf(stderr, "gpu_link_failure reason=head address=%08x head=%08x count=%u native=%d\n", address, head, count, native);
    #endif
#endif
            if (transfer_observer)
                transfer_observation_failed = 1;
            return;
        }
        if (!native || count > 1u)
            registered_bytes = gpu_packet_range_pointer(link, &packet);
        if (registered_bytes)
        {
            native = 0;
            packet_native = 1;
        }
        if (!registered_bytes && native && !xport_memory_readable((const void *)(intptr)address, sizeof(*packet)))
        {
            /* Translated callers pass a native OT pointer whose links remain guest addresses */
            native = 0;
            packet_native = 0;
            address = link;
        }
        if (!registered_bytes && !native && address >= PSX_DRAM_SIZE && (address < 0x1f800000u || address >= 0x1f800400u))
        {
            if (!gpu_native_low24_to_guest(link, &address))
            {
                if (!gpu_native_dma_pointer(link, &packet))
                {
#if !defined(NDEBUG)
    #ifndef NDEBUG
                    fprintf(stderr, "gpu_link_failure reason=resolve address=%08x head=%08x link=%08x count=%u native=%d\n", address, head, link, count, native);
    #endif
#endif
                    if (transfer_observer)
                        transfer_observation_failed = 1;
                    return;
                }
                packet_native = 1;
            }
        }
        if (!packet_native)
            packet = (uint32 *)psx_addr(address, sizeof(*packet));
        else if (native)
            packet = (uint32 *)(intptr)address;
#if !defined(NDEBUG)
        if (packet_native && !xport_memory_readable(packet, sizeof(*packet)))
        {
            if (transfer_observer)
                transfer_observation_failed = 1;
            return;
        }
#endif
        if (registered_bytes && registered_bytes < sizeof(*packet))
            return;
        tag = *packet;
        if (registered_bytes && registered_bytes < 4u + 4u * (tag >> 24))
            return;
#if !defined(NDEBUG)
    #ifndef NDEBUG
        if ((tag & 0x00ffffffu) == 0u)
            fprintf(stderr, "gpu_link_failure zero_link predecessor=%08x tag=%08x words=%u count=%u\n", address, tag, tag >> 24, count);
    #endif
    #ifndef NDEBUG
        if ((tag & 0x00ffffffu) != 0x00ffffffu && (tag & 3u))
            fprintf(stderr, "gpu_link_failure predecessor=%08x tag=%08x words=%u count=%u\n", address, tag, tag >> 24, count);
    #endif
#endif
        if (linked_trace_operation && linked_trace_operation->active)
            xport_device_trace_stage(linked_trace_operation, XPORT_DEVICE_FETCH, XPORT_DEVICE_EVIDENCE_EXACT, packet_native ? XPORT_DEVICE_REGION_NONE : address < PSX_DRAM_SIZE ? XPORT_DEVICE_REGION_RAM : XPORT_DEVICE_REGION_SCRATCHPAD, packet_native ? 0u : address < PSX_DRAM_SIZE ? address : address - 0x1f800000u, XPORT_DEVICE_REGION_NONE, 0u, 4u + 4u * (tag >> 24), 0xffffffffu);
        gpu_observe_ot(address, packet, packet_native);
        if (transfer_observer && transfer_observation_failed)
            return;
        /* Preserve the original terminal packet in RAM. The host still omits this
   * exact degenerate black-line sentinel, as it did before original OT setup
   * was translated. Other flat line packets use the host rasterizer. */
        if (link == 0x10018u && tag == 0x03ffffffu && packet[1] == 0x40000000u && !packet[2] && !packet[3])
        {
            head = tag;
            continue;
        }
        if (tag >> 24)
        {
#if !defined(NDEBUG)
            packet_size = 4u + 4u * (tag >> 24);
            if (packet_native && !xport_memory_readable(packet, packet_size))
            {
                if (transfer_observer)
                    transfer_observation_failed = 1;
                return;
            }
#endif
#if !defined(NDEBUG)
            gpu_debug_packet_address = address;
            gpu_debug_packet_tag = tag;
#endif
            draw_prim(packet_native ? packet : psx_addr(address, 4u + 4u * (tag >> 24)));
        }
        head = tag;
    }
}

static PsxInterruptCallback native_draw_sync_callback;
static uint32 native_draw_sync_pending;

static void gpu_wait_service(void)
{
    while (native_draw_sync_pending && psx_callbacks_allowed())
    {
        --native_draw_sync_pending;
        if (native_draw_sync_callback)
            native_draw_sync_callback();
    }
}

PsxInterruptCallback DrawSyncCallback(PsxInterruptCallback callback)
{
    PsxInterruptCallback previous = native_draw_sync_callback;
    native_draw_sync_callback = callback;
    psx_wait_bind_service(2, gpu_wait_service);
    if (native_draw_sync_pending)
        psx_wait_signal_service(2);
    return previous;
}

void DrawOTag(uint32 *ot)
{
    XportDeviceOperation operation = {0};
    uint32 source_offset = 0u;
    XportDeviceRegion source_region = gpu_trace_pointer(ot, 4u, &source_offset);
    if (xport_device_trace_enabled())
    {
        xport_device_trace_begin(&operation, 0u, XPORT_DEVICE_GPU, XPORT_DEVICE_GPU_PACKET, XPORT_DEVICE_DIRECTION_TO_DEVICE, 2u, 0u);
        xport_device_trace_stage(&operation, XPORT_DEVICE_SUBMIT, XPORT_DEVICE_EVIDENCE_EXACT, source_region, source_offset, XPORT_DEVICE_REGION_NONE, 0u, 0u, 0u);
    }
    if (linked_submit)
    {
        uint32 address;
        if (!gpu_guest_address(ot, &address) || address > PSX_DRAM_SIZE - 4u || (address & 3u) || linked_submit(linked_submit_context, address, 0u) != 1)
            linked_submit_failed = 1;
        if (operation.active)
        {
            xport_device_trace_gap(&operation, 2u);
            xport_device_trace_complete(&operation, linked_submit_failed ? 0 : 1);
        }
        return;
    }
    linked_trace_operation = operation.active ? &operation : NULL;
    gpu_draw_linked_dma(ot);
    linked_trace_operation = NULL;
    if (!transfer_observation_failed && !linked_submit_failed && native_draw_sync_callback)
    {
        ++native_draw_sync_pending;
        psx_wait_signal_service(2);
        if (psx_callbacks_allowed())
            gpu_wait_service();
    }
    if (operation.active)
    {
        xport_device_trace_stage(&operation, XPORT_DEVICE_RASTER_COMPLETE, XPORT_DEVICE_EVIDENCE_CANDIDATE, source_region, source_offset, XPORT_DEVICE_REGION_VRAM, 0u, 0u, 0u);
        xport_device_trace_complete(&operation, transfer_observation_failed ? 0 : 1);
    }
    psx_sync_callbacks();
}

sint32 gpu_execute_linked_dma(uint32 physical_address)
{
    XportDeviceOperation operation = {0};
    if (!transfer_observer || transfer_observation_failed || physical_address > PSX_DRAM_SIZE - 4u || (physical_address & 3u))
    {
        linked_submit_failed = 1;
        return 0;
    }
    if (xport_device_trace_enabled())
    {
        xport_device_trace_begin(&operation, 0u, XPORT_DEVICE_GPU, XPORT_DEVICE_GPU_PACKET, XPORT_DEVICE_DIRECTION_TO_DEVICE, 2u, 0u);
        xport_device_trace_stage(&operation, XPORT_DEVICE_SUBMIT, XPORT_DEVICE_EVIDENCE_EXACT, XPORT_DEVICE_REGION_RAM, physical_address, XPORT_DEVICE_REGION_NONE, 0u, 0u, 0u);
    }
    linked_trace_operation = operation.active ? &operation : NULL;
    gpu_draw_linked_dma((uint32 *)psx_addr(physical_address, 4u));
    linked_trace_operation = NULL;
    if (transfer_observation_failed)
        linked_submit_failed = 1;
    if (operation.active)
    {
        xport_device_trace_stage(&operation, XPORT_DEVICE_RASTER_COMPLETE, XPORT_DEVICE_EVIDENCE_CANDIDATE, XPORT_DEVICE_REGION_RAM, physical_address, XPORT_DEVICE_REGION_VRAM, 0u, 0u, 0u);
        xport_device_trace_complete(&operation, linked_submit_failed ? 0 : 1);
    }
    return !linked_submit_failed;
}

sint32 gpu_dma_linked_slice(GpuLinkedDmaState *state, uint32 budget, GpuDmaRequest request, GpuDmaWrite write, GpuDmaTicks add_ticks, void *context)
{
    sint32 remaining, requested;
    if (!state || state->busy != 1u || state->address > 0xffffffu || !budget || budget > 0x7fffffffu || !request || !write || !add_ticks || !transfer_observer || transfer_observation_failed)
        return -1;
    remaining = (sint32)budget;
    for (;;)
    {
        uint32 address, header, words, setup, cost;
        const uint32 *packet;
        requested = request(context);
        if (requested != 0 && requested != 1)
            return -1;
        if (!requested)
            return 0;
        if (remaining <= 0)
            return 2;
        address = state->address & 0xfffffcu;
        if (address >= PSX_DRAM_SIZE - 4u)
            return -1;
        packet = (const uint32 *)psx_addr(address, 4u);
        header = packet[0];
        words = header >> 24;
        if (words > (PSX_DRAM_SIZE - address - 4u) / 4u)
            return -1;
        transfer_observer(transfer_observer_context, 3u, address, &header, 1u);
        setup = words ? 13u : 8u;
        if (add_ticks(context, setup) != 1)
            return -1;
        remaining -= (sint32)setup;
        if (words)
        {
            transfer_observer(transfer_observer_context, 2u, address + 4u, packet + 1, words);
            if (write(context, address + 4u, packet + 1, words) != 1)
                return -1;
            cost = words + (words + 15u) / 16u;
            if (add_ticks(context, cost) != 1)
                return -1;
            remaining -= (sint32)cost;
        }
        state->address = header & 0xffffffu;
        if (state->address == 0xffffffu)
        {
            state->busy = 0;
            return 1;
        }
    }
}

static void gpu_convert_display(void)
{
    sint32 x;
    sint32 y;
    for (y = 0; y < display_height; ++y)
        for (x = 0; x < display_width; ++x)
        {
            sint32 source_x = x - display_offset_x;
            sint32 source_y = y - display_offset_y;
            uint32 pixel = 0u;
            if ((uint32)source_x < (uint32)display_width && (uint32)source_y < (uint32)display_height)
            {
                if (display_state & 4u)
                {
                    sint32 byte_x = display_x * 2 + source_x * 3;
                    if (byte_x >= 0 && byte_x + 2 < VRAM_W * 2)
                    {
                        const uint8 *rgb = (const uint8 *)(VRAM + (display_y + source_y) * VRAM_W) + byte_x;
                        pixel = ((uint32)rgb[0] << 16) | ((uint32)rgb[1] << 8) | rgb[2];
                    }
                }
                else
                    pixel = rgb555(VRAM[(display_y + source_y) * VRAM_W + display_x + source_x]);
            }
            present_pixels[y * display_width + x] = pixel;
        }
}

void gpu_frame_begin(void)
{
    ++frame_present_depth;
}

void gpu_frame_end(void)
{
    if (frame_present_depth)
        --frame_present_depth;
}

sint32 gpu_present(void)
{
    if (frame_present_depth || gpu_frame_skipped() || (display_state & 2u))
        return 1;
    if (!raster_enabled())
        return 1;
    gpu_convert_display();
    return xport_present(present_pixels, display_width, display_height, 0, 0, display_width, display_height, WND_TITLE);
}

void gpu_display_offset(sint32 x, sint32 y)
{
    display_offset_x = x;
    display_offset_y = y;
}

void gpu_set_display(sint32 x, sint32 y, sint32 width, sint32 height)
{
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x >= VRAM_W)
        x = VRAM_W - 1;
    if (y >= VRAM_H)
        y = VRAM_H - 1;
    if (width < 1)
        width = 1;
    if (height < 1)
        height = 1;
    if (width > GPU_DISPLAY_MAX_WIDTH)
        width = GPU_DISPLAY_MAX_WIDTH;
    if (height > GPU_DISPLAY_MAX_HEIGHT)
        height = GPU_DISPLAY_MAX_HEIGHT;
    if (x + width > VRAM_W)
        width = VRAM_W - x;
    if (y + height > VRAM_H)
        height = VRAM_H - y;
    display_x = x;
    display_y = y;
    display_width = width;
    display_height = height;
}

void gpu_init_empty(void)
{
    frame_present_depth = 0;
    frame_skip.started = frame_skip.skipped = 0;
    gpu_scanout_reset(&native_scanout);
    scanout_last_us = xport_timer_get();
    scanout_clock_started = 1u;
    scanout_fraction = native_disp_cached = native_draw_cached = 0u;
    display_state = 0;
    gpu_vram_invalidate();
    memset(VRAM, 0, sizeof(VRAM));
    active_tpage = 0;
    active_draw_flags = 0;
    texture_window = 0;
    raster_clip_x0 = raster_clip_y0 = raster_offset_x = raster_offset_y = 0;
    raster_clip_x1 = VRAM_W;
    raster_clip_y1 = VRAM_H;
    raster_surface_x = raster_surface_y = 0;
    display_offset_x = display_offset_y = 0;
    gpu_set_display(0, 0, 320, 240);
}

sint32 gpu_load_vram(const char *path)
{
    size_t n;
    gpu_vram_invalidate();
    return xport_file_read(path, VRAM, sizeof(VRAM), &n) && n == sizeof(VRAM);
}

sint32 gpu_save_frame(const char *path)
{
    FILE *f;
    size_t n;
    if (!raster_enabled())
        return 0; /* No valid pixels to export in fast mode. */
    gpu_convert_display();
    f = xport_fopen(path, "wb");
    if (!f)
        return 0;
    n = fwrite(present_pixels, sizeof(*present_pixels), (size_t)display_width * display_height, f);
    fclose(f);
    return n == (size_t)display_width * display_height;
}

/* Reset only host GPU state; PsyQ environment calls provide game dimensions */
void gpu_reset_graph_state(void)
{
    frame_present_depth = 0;
    frame_skip.started = frame_skip.skipped = 0;
    gpu_scanout_reset(&native_scanout);
    scanout_last_us = xport_timer_get();
    scanout_clock_started = 1u;
    scanout_fraction = native_disp_cached = native_draw_cached = 0u;
    display_state = 0;
    active_tpage = 0;
    active_draw_flags = 0;
    texture_window = 0;
    raster_clip_x0 = 0;
    raster_clip_y0 = 0;
    raster_clip_x1 = VRAM_W;
    raster_clip_y1 = VRAM_H;
    raster_offset_x = 0;
    raster_offset_y = 0;
    raster_surface_x = 0;
    raster_surface_y = 0;
    display_offset_x = 0;
    display_offset_y = 0;
}

/* DRAWENV packet storage at +28 is deliberately untouched */
uint32 gpu_set_def_draw_env(uint32 env, sint32 x, sint32 y, sint32 w, sint32 h)
{
    uint32 i;
    gpu_w16(env, (uint16)x);
    gpu_w16(env + 2, (uint16)y);
    gpu_w16(env + 4, (uint16)w);
    gpu_w16(env + 6, (uint16)h);
    for (i = 12; i <= 18; i += 2)
        gpu_w16(env + i, 0);
    gpu_w8(env + 25, 0);
    gpu_w8(env + 26, 0);
    gpu_w8(env + 27, 0);
    gpu_w8(env + 22, 1);
    gpu_w8(env + 23, (uint8)(h < 289));
    gpu_w16(env + 8, (uint16)x);
    gpu_w16(env + 10, (uint16)y);
    gpu_w16(env + 20, 10);
    gpu_w8(env + 24, 0);
    return env;
}

uint32 gpu_set_def_disp_env(uint32 env, sint32 x, sint32 y, sint32 w, sint32 h)
{
    gpu_w16(env, (uint16)x);
    gpu_w16(env + 2, (uint16)y);
    gpu_w16(env + 4, (uint16)w);
    memset(psx_addr(env + 8, 12), 0, 12);
    gpu_w16(env + 6, (uint16)h);
    return env;
}

DRAWENV *SetDefDrawEnv(DRAWENV *environment, sint32 x, sint32 y, sint32 width, sint32 height)
{
    sint32 video_mode;

    if (!environment)
        return 0;
    video_mode = GetVideoMode();
    xport_store_le16(&environment->clip.x, (uint16)x);
    xport_store_le16(&environment->clip.y, (uint16)y);
    xport_store_le16(&environment->clip.w, (uint16)width);
    xport_store_le16(&environment->tw.x, 0u);
    xport_store_le16(&environment->tw.y, 0u);
    xport_store_le16(&environment->tw.w, 0u);
    xport_store_le16(&environment->tw.h, 0u);
    xport_store_u8(&environment->r0, 0u);
    xport_store_u8(&environment->g0, 0u);
    xport_store_u8(&environment->b0, 0u);
    xport_store_u8(&environment->dtd, 1u);
    xport_store_le16(&environment->clip.h, (uint16)height);
    xport_store_u8(&environment->dfe, (uint8)(height < (video_mode ? 289 : 257)));
    xport_store_le16(&environment->ofs[0], (uint16)x);
    xport_store_le16(&environment->ofs[1], (uint16)y);
    xport_store_le16(&environment->tpage, 10u);
    xport_store_u8(&environment->isbg, 0u);
    return environment;
}

DISPENV *SetDefDispEnv(DISPENV *environment, sint32 x, sint32 y, sint32 width, sint32 height)
{
    if (!environment)
        return 0;
    xport_store_le16(&environment->disp.x, (uint16)x);
    xport_store_le16(&environment->disp.y, (uint16)y);
    xport_store_le16(&environment->disp.w, (uint16)width);
    xport_store_le16(&environment->screen.x, 0u);
    xport_store_le16(&environment->screen.y, 0u);
    xport_store_le16(&environment->screen.w, 0u);
    xport_store_le16(&environment->screen.h, 0u);
    xport_store_u8(&environment->isrgb24, 0u);
    xport_store_u8(&environment->isinter, 0u);
    xport_store_u8(&environment->pad1, 0u);
    xport_store_u8(&environment->pad0, 0u);
    xport_store_le16(&environment->disp.h, (uint16)height);
    return environment;
}

static uint32 draw_area_width_address, draw_area_height_address;

void gpu_bind_draw_area_bounds(uint32 width_address, uint32 height_address)
{
    draw_area_width_address = width_address;
    draw_area_height_address = height_address;
}

static uint32 gpu_draw_area_coordinate(sint16 coordinate, uint32 address, sint32 extent)
{
    sint32 maximum;
    uint32 result;
    if (coordinate < 0)
        return 0;
    maximum = (address ? r_s16(address) : extent) - 1;
    result = (address ? r_u16(address) : (uint32)extent) - 1;
    return coordinate > maximum ? result : (uint32)coordinate;
}

static uint32 gpu_draw_area_word(uint32 command, sint16 x, sint16 y)
{
    uint32 column = gpu_draw_area_coordinate(x, draw_area_width_address, VRAM_W);
    uint32 row = gpu_draw_area_coordinate(y, draw_area_height_address, VRAM_H);
    return command | (column & 1023) | ((row & 1023) << 10);
}

void SetDrawLoad(DR_LOAD *packet, const PSX_RECT *rectangle)
{
    uint8 *bytes = (uint8 *)packet;
    uint32 words, length, value;
    sint32 width, height;
    if (!packet || !rectangle)
        return;
    width = (sint16)xport_load_le16(&rectangle->w);
    height = (sint16)xport_load_le16(&rectangle->h);
    words = (uint32)((width * height + 1) / 2);
    length = words < 13 ? words + 4 : 0;
    bytes[3] = (uint8)length;
    xport_store_le32(bytes + 4, 0xA0000000);
    value = xport_load_le32(rectangle);
    xport_store_le32(bytes + 8, value);
    value = xport_load_le32((const uint8 *)rectangle + 4);
    xport_store_le32(bytes + 12, value);
    xport_store_le32(bytes + length * 4, 0x01000000);
}

uint32 SetDrawArea(DR_AREA *packet, const PSX_RECT *rectangle)
{
    uint32 x, y, width, height, word;
    const uint8 *input = (const uint8 *)rectangle;
    uint8 *output = (uint8 *)packet;
    if (!packet || !rectangle)
        return 0;
    xport_store_u8(output + 3, 2);
    x = xport_load_le16(input);
    y = xport_load_le16(input + 2);
    word = gpu_draw_area_word(0xE3000000, (sint16)x, (sint16)y);
    xport_store_le32(output + 4, word);
    x = xport_load_le16(input);
    width = xport_load_le16(input + 4);
    y = xport_load_le16(input + 2);
    height = xport_load_le16(input + 6);
    word = gpu_draw_area_word(0xE4000000, (sint16)(x + width - 1), (sint16)(y + height - 1));
    xport_store_le32(output + 8, word);
    return word;
}

void SetDrawOffset(void *packet, const sint16 *offset)
{
    uint32 *words = (uint32 *)packet;

    if (!words || !offset)
        return;
    words[0] = (words[0] & 0x00ffffffu) | 0x02000000u;
    words[1] = 0xe5000000u | ((uint32)offset[0] & 0x7ffu) | (((uint32)offset[1] & 0x7ffu) << 11);
    words[2] = 0u;
}

void SetDrawEnv(DR_ENV *packet, DRAWENV *environment)
{
    uint32 *words = (uint32 *)packet;
    sint32 x;
    sint32 y;
    sint32 count = 6;
    if (!words || !environment)
        return;
    words[1] = gpu_env_clip(0xe3000000u, environment->clip.x, environment->clip.y, 0);
    words[2] = gpu_env_clip(0xe4000000u, environment->clip.x + environment->clip.w - 1, environment->clip.y + environment->clip.h - 1, 0);
    words[3] = 0xe5000000u | ((uint32)environment->ofs[0] & 0x7ffu) | (((uint32)environment->ofs[1] & 0x7ffu) << 11);
    words[4] = 0xe1000000u | (environment->tpage & 0x9ffu) | (environment->dtd ? 0x200u : 0u) | (environment->dfe ? 0x400u : 0u);
    words[5] = psx_texture_window(&environment->tw);
    words[6] = 0xe6000000u;
    if (environment->isbg)
    {
        x = environment->clip.x - environment->ofs[0];
        y = environment->clip.y - environment->ofs[1];
        words[7] = 0x60000000u | (uint32)environment->r0 | ((uint32)environment->g0 << 8) | ((uint32)environment->b0 << 16);
        words[8] = (uint16)x | ((uint32)(uint16)y << 16);
        words[9] = (uint32)gpu_env_clamp((sint16)environment->clip.w, VRAM_W) | ((uint32)gpu_env_clamp((sint16)environment->clip.h, VRAM_H) << 16);
        count = 9;
    }
    words[0] = (words[0] & 0x00ffffffu) | ((uint32)count << 24);
}

void SetDrawStp(DR_STP *packet, sint32 enabled)
{
    uint32 *words = (uint32 *)packet;
    if (!words)
        return;
    words[0] = (words[0] & 0x00ffffffu) | 0x02000000u;
    words[1] = 0xe6000000u | (enabled ? 1u : 0u);
    words[2] = 0;
}

DRAWENV *GetDrawEnv(DRAWENV *environment)
{
    if (!environment || (!psyq_bound && !native_draw_cached))
        return NULL;
    memcpy(environment, psyq_bound ? psx_addr(psyq_binding.draw_cache, sizeof(DRAWENV)) : &native_draw_cache, sizeof(DRAWENV));
    return environment;
}

DRAWENV *PutDrawEnv(DRAWENV *environment)
{
    if (!environment)
        return 0;
    gpu_put_draw_env_native(environment, 0, 0);
    return environment;
}

static void gpu_psyq_gp1(uint32 command)
{
    gpu_scanout_poll();
    gpu_scanout_command(&native_scanout, command);
    if (!(command >> 24))
        scanout_fraction = 0u;
    if (transfer_observer)
        transfer_observer(transfer_observer_context, 1u, 4u, &command, 1u);
    if (psyq_binding.gp1_shadow)
        ((uint8 *)psx_addr(psyq_binding.gp1_shadow, 256u))[command >> 24] = (uint8)command;
}

static sint32 gpu_psyq_clamp(sint32 value, sint32 low, sint32 high)
{
    return value < low ? low : (value > high ? high : value);
}

static void gpu_psyq_disp_env(DISPENV *environment)
{
    DISPENV *cache;
    uint32 mode, video;
    sint32 x0, x1, y0, y1, width, height;
    if (!psyq_bound)
    {
        if (transfer_observer)
        {
            transfer_observation_failed = 1;
            return;
        }
        video = (uint32)GetVideoMode();
        cache = &native_disp_cache;
    }
    else
    {
        video = *(uint32 *)psx_addr(psyq_binding.video_mode, 4u);
        cache = (DISPENV *)psx_addr(psyq_binding.display_cache, 20u);
    }
    if ((psyq_bound && *(uint8 *)psx_addr(psyq_binding.graph_type, 1u) != 0u) || video > 1u)
    {
        transfer_observation_failed = 1;
        return;
    }
    gpu_psyq_gp1(0x05000000u | ((uint32)environment->disp.x & 0x3ffu) | (((uint32)environment->disp.y & 0x3ffu) << 10));
    if ((!psyq_bound && !native_disp_cached) || memcmp(&cache->screen, &environment->screen, 8u))
    {
        environment->pad0 = (uint8)video;
        x0 = 10 * environment->screen.x + 608;
        x1 = x0 + (environment->screen.w ? 10 * environment->screen.w : 2560);
        y0 = environment->screen.y + (video ? 19 : 16);
        y1 = y0 + (environment->screen.h ? environment->screen.h : 240);
        x0 = gpu_psyq_clamp(x0, 500, 3290);
        x1 = gpu_psyq_clamp(x1, x0 + 80, 3290);
        y0 = gpu_psyq_clamp(y0, 16, video ? 310 : 256);
        y1 = gpu_psyq_clamp(y1, y0 + 2, video ? 312 : 258);
        gpu_psyq_gp1(0x06000000u | ((uint32)x0 & 0xfffu) | (((uint32)x1 & 0xfffu) << 12));
        gpu_psyq_gp1(0x07000000u | ((uint32)y0 & 0x3ffu) | (((uint32)y1 & 0x3ffu) << 10));
    }
    if ((!psyq_bound && !native_disp_cached) || memcmp(&cache->disp, &environment->disp, 8u) || memcmp(&cache->isinter, &environment->isinter, 4u))
    {
        environment->pad0 = (uint8)video;
        mode = 0x08000000u | (video ? 8u : 0u) | (environment->isrgb24 ? 16u : 0u) | (environment->isinter ? 32u : 0u);
        if (psyq_bound && *(uint8 *)psx_addr(psyq_binding.reverse_flag, 1u))
            mode |= 128u;
        width = environment->disp.w;
        height = environment->disp.h;
        mode |= width < 281 ? 0u : width < 353 ? 1u : width < 401 ? 64u : width < 561 ? 2u : 3u;
        if (height >= (video ? 289 : 257))
            mode |= 0x24u;
        gpu_psyq_gp1(mode);
    }
    memcpy(cache, environment, 20u);
    if (!psyq_bound)
        native_disp_cached = 1u;
}

DISPENV *PutDispEnv(DISPENV *environment)
{
    if (!environment)
        return 0;
    gpu_psyq_disp_env(environment);
    gpu_set_display(environment->disp.x, environment->disp.y, environment->disp.w, environment->disp.h);
    display_state = (display_state & ~4u) | (environment->isrgb24 ? 4u : 0u);
    return environment;
}

sint32 gpu_get_display_env(DISPENV *environment)
{
    if (!environment || (!psyq_bound && !native_disp_cached))
        return 0;
    memcpy(environment, psyq_bound ? psx_addr(psyq_binding.display_cache, sizeof(DISPENV)) : &native_disp_cache, sizeof(DISPENV));
    return 1;
}

void gpu_clear_menu_surfaces(void)
{
    uint32 y;
    if (gpu_frame_skipped())
        return;
    gpu_vram_invalidate();
    for (y = 0; y < 256; y++)
        memset(VRAM + y * VRAM_W, 0, 640 * sizeof(uint16));
}

static int gpu_state_block(FILE *file, void *data, size_t size, int load)
{
    uint32 stored = (uint32)size;
    if (load)
        return fread(&stored, 4, 1, file) == 1 && stored == size && fread(data, 1, size, file) == size;
    return fwrite(&stored, 4, 1, file) == 1 && fwrite(data, 1, size, file) == size;
}

typedef struct GPU_PORTABLE_SCALARS
{
    uint32 draw_sync_pending;
    GpuScanoutState scanout;
    uint32 scanout_fraction;
    DISPENV native_disp_cache;
    uint32 native_disp_cached;
    uint32 move_image_packet;
    uint32 clear_image_packet;
    uint32 clear_image_dimensions;
    uint16 active_draw_flags;
    sint32 graph_debug_level;
    GpuGraphControl graph_control;
    uint32 sync_head;
    uint32 sync_tail;
    uint32 sync_budget;
    sint32 sync_failed;
    sint32 sync_running;
    uint32 soft_reset_head;
    uint32 soft_reset_tail;
    sint32 transfer_observation_failed;
    sint32 linked_submit_failed;
    sint32 psyq_bound;
    sint32 psyq_queue_bound;
    GpuPsyqStateBinding psyq_binding;
    GpuPsyqQueueBinding psyq_queue_binding;
} GPU_PORTABLE_SCALARS;

int gpu_state_io(FILE *f, int load)
{
    if (load)
        gpu_vram_invalidate();
    GPU_PORTABLE_SCALARS scalars = {0};
    if (!load)
        gpu_scanout_poll();
    if (!(gpu_state_block(f, &VRAM, sizeof(VRAM), load) && gpu_state_block(f, &active_tpage, sizeof(active_tpage), load) && gpu_state_block(f, &texture_window, sizeof(texture_window), load) && gpu_state_block(f, &display_state, sizeof(display_state), load) && gpu_state_block(f, &display_x, sizeof(display_x), load) && gpu_state_block(f, &display_y, sizeof(display_y), load) && gpu_state_block(f, &display_width, sizeof(display_width), load) && gpu_state_block(f, &display_height, sizeof(display_height), load) && gpu_state_block(f, &display_offset_x, sizeof(display_offset_x), load) && gpu_state_block(f, &display_offset_y, sizeof(display_offset_y), load) && gpu_state_block(f, &raster_clip_x0, sizeof(raster_clip_x0), load) && gpu_state_block(f, &raster_clip_y0, sizeof(raster_clip_y0), load) && gpu_state_block(f, &raster_clip_x1, sizeof(raster_clip_x1), load) && gpu_state_block(f, &raster_clip_y1, sizeof(raster_clip_y1), load) &&
          gpu_state_block(f, &raster_offset_x, sizeof(raster_offset_x), load) && gpu_state_block(f, &raster_offset_y, sizeof(raster_offset_y), load) && gpu_state_block(f, &raster_surface_x, sizeof(raster_surface_x), load) && gpu_state_block(f, &raster_surface_y, sizeof(raster_surface_y), load)))
        return 0;
    if (load)
    {
        if (!gpu_state_block(f, &scalars, sizeof(scalars), 1) || !gpu_scanout_validate(&scalars.scanout) || scalars.scanout_fraction >= 1000000u || scalars.native_disp_cached > 1u || scalars.sync_running || scalars.psyq_bound != psyq_bound || scalars.psyq_queue_bound != psyq_queue_bound || (psyq_bound && memcmp(&scalars.psyq_binding, &psyq_binding, sizeof(psyq_binding))) || (psyq_queue_bound && memcmp(&scalars.psyq_queue_binding, &psyq_queue_binding, sizeof(psyq_queue_binding))))
            return 0;
        native_scanout = scalars.scanout;
        native_draw_sync_pending = scalars.draw_sync_pending;
        if (native_draw_sync_pending)
            psx_wait_signal_service(2);
        scanout_fraction = scalars.scanout_fraction;
        native_disp_cache = scalars.native_disp_cache;
        native_disp_cached = scalars.native_disp_cached;
        scanout_clock_started = 0u;
        move_image_packet = scalars.move_image_packet;
        clear_image_packet = scalars.clear_image_packet;
        clear_image_dimensions = scalars.clear_image_dimensions;
        active_draw_flags = scalars.active_draw_flags;
        graph_debug_level = scalars.graph_debug_level;
        gpu_graph_control = scalars.graph_control;
        sync_head = scalars.sync_head;
        sync_tail = scalars.sync_tail;
        sync_budget = scalars.sync_budget;
        sync_failed = scalars.sync_failed;
        soft_reset_head = scalars.soft_reset_head;
        soft_reset_tail = scalars.soft_reset_tail;
        transfer_observation_failed = scalars.transfer_observation_failed;
        linked_submit_failed = scalars.linked_submit_failed;
        frame_present_depth = 0;
        frame_skip.started = frame_skip.skipped = 0;
        return 1;
    }
    /* The full guest DRAM binding is recreated by platform initialization */
    if ((native_dma_range_count && (native_dma_range_count != 1 || native_dma_ranges[0].begin != (uintptr_t)DRAM || native_dma_ranges[0].end != (uintptr_t)DRAM + PSX_DRAM_SIZE)) || transfer_observer || linked_submit || linked_trace_operation || sync_sample || sync_pump || sync_wait || sync_running || sync_timeout_bound)
        return 0;
    scalars.scanout = native_scanout;
    scalars.draw_sync_pending = native_draw_sync_pending;
    scalars.scanout_fraction = scanout_fraction;
    scalars.native_disp_cache = native_disp_cache;
    scalars.native_disp_cached = native_disp_cached;
    scalars.move_image_packet = move_image_packet;
    scalars.clear_image_packet = clear_image_packet;
    scalars.clear_image_dimensions = clear_image_dimensions;
    scalars.active_draw_flags = active_draw_flags;
    scalars.graph_debug_level = graph_debug_level;
    scalars.graph_control = gpu_graph_control;
    scalars.sync_head = sync_head;
    scalars.sync_tail = sync_tail;
    scalars.sync_budget = sync_budget;
    scalars.sync_failed = sync_failed;
    scalars.sync_running = sync_running;
    scalars.soft_reset_head = soft_reset_head;
    scalars.soft_reset_tail = soft_reset_tail;
    scalars.transfer_observation_failed = transfer_observation_failed;
    scalars.linked_submit_failed = linked_submit_failed;
    scalars.psyq_bound = psyq_bound;
    scalars.psyq_queue_bound = psyq_queue_bound;
    if (psyq_bound)
        scalars.psyq_binding = psyq_binding;
    if (psyq_queue_bound)
        scalars.psyq_queue_binding = psyq_queue_binding;
    return gpu_state_block(f, &scalars, sizeof(scalars), 0);
}

#if defined(LOCKSTEP_DEBUG)
int gpu_lockstep_portable_selftest(void)
{
    const GpuPsyqStateBinding state_binding = {0x80005000u, 0x80005100u, 0x80005200u, 0x80005300u, 0x80005400u, 0x80005500u};
    GpuPsyqQueueBinding queue_binding = {0};
    const char *export_path = getenv("XPORT_LOCKSTEP_GPU_EXPORT_PATH");
    const char *import_path = getenv("XPORT_LOCKSTEP_GPU_IMPORT_PATH");
    FILE *file;
    int ok;
    gpu_register_dma_range(DRAM, PSX_DRAM_SIZE);
    queue_binding.last_callback = 0x80006000u;
    queue_binding.last_argument = 0x80006004u;
    queue_binding.last_aux = 0x80006008u;
    queue_binding.pointer_segment = 0x80000000u;
    if (!gpu_bind_psyq_state(&state_binding) || !gpu_bind_psyq_queue(&queue_binding))
        return 0;
    if (import_path && *import_path)
    {
        file = fopen(import_path, "rb");
        if (!file)
            return 0;
        ok = gpu_state_io(file, 1) && fgetc(file) == EOF && VRAM[9] == 0x1357u && move_image_packet == 0x80001000u && clear_image_packet == 0x80002000u && clear_image_dimensions == 0x01400100u && active_draw_flags == 0x1234u && graph_debug_level == 7 && sync_head == 0x80003000u && sync_tail == 0x80003004u && sync_budget == 33u && sync_failed == 1 && soft_reset_head == 0x80004000u && soft_reset_tail == 0x80004004u && transfer_observation_failed == 1 && linked_submit_failed == 1;
        fclose(file);
        if (!ok)
            return 0;
        queue_binding.last_callback = 0x80006100u;
        if (!gpu_bind_psyq_queue(&queue_binding))
            return 0;
        file = fopen(import_path, "rb");
        if (!file)
            return 0;
        ok = !gpu_state_io(file, 1);
        fclose(file);
        if (!ok)
            return 0;
        file = tmpfile();
        if (!file)
            return 0;
        gpu_register_dma_range(&queue_binding, sizeof(queue_binding));
        ok = !gpu_state_io(file, 0);
        native_dma_range_count = 1;
        fclose(file);
        return ok;
    }
    file = export_path && *export_path ? fopen(export_path, "wb") : tmpfile();
    if (!file)
        return 0;
    VRAM[9] = 0x1357u;
    move_image_packet = 0x80001000u;
    clear_image_packet = 0x80002000u;
    clear_image_dimensions = 0x01400100u;
    active_draw_flags = 0x1234u;
    graph_debug_level = 7;
    sync_head = 0x80003000u;
    sync_tail = 0x80003004u;
    sync_budget = 33u;
    sync_failed = 1;
    soft_reset_head = 0x80004000u;
    soft_reset_tail = 0x80004004u;
    transfer_observation_failed = 1;
    linked_submit_failed = 1;
    ok = gpu_state_io(file, 0);
    if (fclose(file))
        ok = 0;
    return ok;
}
#endif

sint32 MargePrim(uint32 first, uint32 second)
{
    uint32 first_length, second_length, length;
    first_length = r_u8(first + 3u);
    second_length = r_u8(second + 3u);
    length = first_length + second_length + 1u;
    if (length >= 17u)
        return -1;
    w_u8(first + 3u, length);
    w_u32(second, 0u);
    return 0;
}

uint32 *ClearOTagRPSX(uint32 *ot, sint32 count, uint32 header_address, uint32 header_target, uint32 guest_ot)
{
    (void)ClearOTagR(ot, count);
    xport_gpu_clear_otag_r_guest_driver(guest_ot, (uint32)count);
    w_u32(header_address, (header_target & 0xFFFFFFu) | 0x04000000u);
    xport_store_le32(ot, header_address & 0xFFFFFFu);
    return ot;
}

uint32 *ClearOTagRNativePSX(uint32 *ot, sint32 count, uint32 header_address, uint32 header_target)
{
    (void)ClearOTagR(ot, count);
    w_u32(header_address, (header_target & 0xFFFFFFu) | 0x04000000u);
    xport_store_le32(ot, header_address & 0xFFFFFFu);
    return ot;
}

uint32 DrawOTagPSX(uint32 guest_ot)
{
    DrawOTag((uint32 *)psx_addr(guest_ot, 4u));
    return xport_gpu_draw_otag_guest_result(guest_ot);
}

void SetDrawMove(DR_MOVE *packet, PSX_RECT *rectangle, sint32 x, sint32 y)
{
    uint8 *bytes = (uint8 *)packet;
    uint32 source = xport_load_le32(rectangle);
    uint32 dimensions = xport_load_le32((uint8 *)rectangle + 4u);
    bytes[3] = rectangle->w && rectangle->h ? 5u : 0u;
    xport_store_le32(bytes + 4u, 0x01000000u);
    xport_store_le32(bytes + 8u, 0x80000000u);
    xport_store_le32(bytes + 12u, source);
    xport_store_le32(bytes + 16u, ((uint32)y << 16u) | ((uint32)x & 0xFFFFu));
    xport_store_le32(bytes + 20u, dimensions);
}

uint32 SetDrawMovePSX(uint32 destination, uint32 rectangle, uint32 x, uint32 y)
{
    DR_MOVE *packet = (DR_MOVE *)psx_addr(destination, sizeof(DR_MOVE));
    PSX_RECT *source = (PSX_RECT *)psx_addr(rectangle, sizeof(PSX_RECT));
    SetDrawMove(packet, source, (sint32)x, (sint32)y);
    return xport_load_le32((uint8 *)source + 4u);
}

uint32 ClearOTagPSX(uint32 ot, uint32 count, uint32 header_address, uint32 header_target, uint32 debug_flag_address, uint32 debug_callback_slot, uint32 debug_format_address)
{
    uint32 cursor = ot, remaining = count, value, callback;
    value = r_u8(debug_flag_address);
    if (value >= 2u)
    {
        callback = r_u32(debug_callback_slot);
        (void)xport_guest_call3(callback, debug_format_address, ot, count);
    }
    remaining -= 1u;
    while (remaining != 0u)
    {
        remaining -= 1u;
        w_u8(cursor + 3u, 0u);
        value = r_u32(cursor);
        w_u32(cursor, (value & 0xFF000000u) | ((cursor + 4u) & 0xFFFFFFu));
        cursor += 4u;
        if (remaining == 0u)
            break;
    }
    w_u32(header_address, (header_target & 0xFFFFFFu) | 0x04000000u);
    w_u32(cursor, header_address & 0xFFFFFFu);
    return cursor;
}

uint32 xport_gpu_draw_otag_guest_result(uint32 guest_ot)
{
    (void)guest_ot;
    abort();
}

uint32 xport_gpu_status_read_missing(uint32 address)
{
    (void)address;
    abort();
}

uint32 DrawPrimPSX(uint32 primitive, uint32 driver_slot, uint32 guest_gp, uint32 guest_entry)
{
    uint32 driver, target, count, registers[32];
    driver = r_u32(driver_slot);
    target = r_u32(driver + 60u);
    count = r_u8(primitive + 3u);
    registers[0] = 0u;
    registers[2] = target;
    registers[4] = 0u;
    registers[16] = primitive;
    registers[17] = count;
    registers[28] = guest_gp;
    registers[31] = guest_entry + 0x2Cu;
    (void)xport_gpu_guest_dispatch(target, registers, 0x90030015u);
    driver = r_u32(driver_slot);
    target = r_u32(driver + 20u);
    registers[2] = target;
    registers[4] = primitive + 4u;
    registers[5] = count;
    registers[31] = guest_entry + 0x48u;
    return xport_gpu_guest_dispatch(target, registers, 0x90030035u);
}

uint32 SYS_OBJ_172CPSX(uint32 x, uint32 y, uint32 extent)
{
    uint32 value, maximum;
    if ((sint16)x < 0)
        x = 0u;
    else
    {
        value = (uint32)(sint32)r_s16(extent) - 1u;
        maximum = r_u16(extent) - 1u;
        if ((sint32)value < (sint16)x)
            x = maximum;
    }
    if ((sint16)y < 0)
        y = 0u;
    else
    {
        value = (uint32)(sint32)r_s16(extent + 2u) - 1u;
        maximum = r_u16(extent + 2u) - 1u;
        if ((sint32)value < (sint16)y)
            y = maximum;
    }
    return 0xE3000000u | (x & 1023u) | ((y & 1023u) << 10u);
}

uint32 SYS_OBJ_17C4PSX(uint32 x, uint32 y, uint32 extent)
{
    uint32 value, maximum;
    if ((sint16)x < 0)
        x = 0u;
    else
    {
        value = (uint32)(sint32)r_s16(extent) - 1u;
        maximum = r_u16(extent) - 1u;
        if ((sint32)value < (sint16)x)
            x = maximum;
    }
    if ((sint16)y < 0)
        y = 0u;
    else
    {
        value = (uint32)(sint32)r_s16(extent + 2u) - 1u;
        maximum = r_u16(extent + 2u) - 1u;
        if ((sint32)value < (sint16)y)
            y = maximum;
    }
    return 0xE4000000u | (x & 1023u) | ((y & 1023u) << 10u);
}

uint32 SYS_OBJ_185CPSX(uint32 x, uint32 y)
{
    return 0xE5000000u | (x & 2047u) | ((y & 2047u) << 11u);
}

uint32 SYS_OBJ_170CPSX(uint32 dfe, uint32 dtd, uint32 tpage)
{
    uint32 command = 0xE1000000u;
    if (dtd != 0u)
        command |= 0x200u;
    tpage &= 0x9FFu;
    if (dfe != 0u)
        tpage |= 0x400u;
    return command | tpage;
}

uint32 SYS_OBJ_1878PSX(uint32 rectangle)
{
    uint32 frame = xport_sdk_texture_frame_acquire(16u);
    uint32 width, offset_x, height, offset_y, result = 0u;
    if (rectangle != 0u)
    {
        width = r_u8(rectangle) >> 3u;
        w_u32(frame, width);
        offset_x = ((0u - (uint32)(sint32)r_s16(rectangle + 4u)) & 255u) >> 3u;
        w_u32(frame + 8u, offset_x);
        height = r_u8(rectangle + 2u) >> 3u;
        w_u32(frame + 4u, height);
        offset_y = ((0u - (uint32)(sint32)r_s16(rectangle + 6u)) & 255u) >> 3u;
        result = 0xE2000000u | (width << 10u) | (height << 15u) | offset_x | (offset_y << 5u);
        w_u32(frame + 12u, offset_y);
    }
    xport_sdk_texture_frame_release(frame, 16u);
    return result;
}

uint32 SYS_OBJ_149CPSX(uint32 packet, uint32 environment, uint32 extent)
{
    uint32 frame = xport_sdk_draw_env_frame_acquire(40u);
    uint32 x, y, width, height, value, count = 7u;
    sint32 height_signed;
    x = (uint32)(sint32)r_s16(environment);
    y = (uint32)(sint32)r_s16(environment + 2u);
    w_u32(packet + 4u, SYS_OBJ_172CPSX(x, y, extent));
    width = r_u16(environment + 4u);
    x = r_u16(environment);
    y = r_u16(environment + 2u);
    x = (uint32)(sint32)(sint16)(width + x - 1u);
    height = r_u16(environment + 6u);
    y = (uint32)(sint32)(sint16)(y + height - 1u);
    w_u32(packet + 8u, SYS_OBJ_17C4PSX(x, y, extent));
    x = (uint32)(sint32)r_s16(environment + 8u);
    y = (uint32)(sint32)r_s16(environment + 10u);
    w_u32(packet + 12u, SYS_OBJ_185CPSX(x, y));
    x = r_u8(environment + 23u);
    y = r_u8(environment + 22u);
    value = r_u16(environment + 20u);
    w_u32(packet + 16u, SYS_OBJ_170CPSX(x, y, value));
    value = SYS_OBJ_1878PSX(environment + 12u);
    w_u32(packet + 20u, value);
    w_u32(packet + 24u, 0xE6000000u);
    if (r_u8(environment + 24u) != 0u)
    {
        x = r_u16(environment);
        w_u16(frame + 16u, x);
        y = r_u16(environment + 2u);
        w_u16(frame + 18u, y);
        width = r_u16(environment + 4u);
        w_u16(frame + 20u, width);
        height = r_u16(environment + 6u);
        w_u16(frame + 22u, height);
        if ((sint16)width < 0)
            width = 0u;
        else
        {
            value = (uint32)(sint32)r_s16(extent) - 1u;
            if ((sint32)value < (sint16)width)
                width = r_u16(extent) - 1u;
            else
            {
                (void)r_u16(extent);
            }
        }
        height_signed = r_s16(frame + 22u);
        height = r_u16(frame + 22u);
        w_u16(frame + 20u, width);
        if (height_signed < 0)
            height = 0u;
        else
        {
            value = (uint32)(sint32)r_s16(extent + 2u) - 1u;
            if ((sint32)value < height_signed)
                height = r_u16(extent + 2u) - 1u;
            else
            {
                (void)r_u16(extent + 2u);
            }
        }
        x = r_u16(frame + 16u);
        w_u16(frame + 22u, height);
        if ((x & 63u) != 0u || (r_u16(frame + 20u) & 63u) != 0u)
        {
            value = r_u16(environment + 8u);
            w_u16(frame + 16u, x - value);
            y = r_u16(frame + 18u);
            value = r_u16(environment + 10u);
            w_u16(frame + 18u, y - value);
            value = r_u8(environment + 27u) << 16u;
            value |= r_u8(environment + 26u) << 8u;
            value |= 0x60000000u | r_u8(environment + 25u);
        }
        else
        {
            value = r_u8(environment + 27u) << 16u;
            value |= r_u8(environment + 26u) << 8u;
            value |= 0x02000000u | r_u8(environment + 25u);
        }
        w_u32(packet + 28u, value);
        w_u32(packet + 32u, r_u32(frame + 16u));
        w_u32(packet + 36u, r_u32(frame + 20u));
        count = 10u;
    }
    w_u8(packet + 3u, (uint8)(count - 1u));
    xport_sdk_draw_env_frame_release(frame, 40u);
    return count - 1u;
}

uint32 PutDrawEnvPSX(uint32 environment, uint32 state, uint32 debug_format, uint32 guest_gp, uint32 guest_entry, uint32 (*copy_guest)(uint32 destination, uint32 source, uint32 bytes))
{
    uint32 driver, target, value, registers[32];
    if (r_u8(state) >= 2u)
    {
        target = r_u32(state - 6u);
        registers[0] = 0u;
        registers[2] = target;
        registers[4] = debug_format;
        registers[5] = environment;
        registers[17] = environment;
        registers[18] = state;
        registers[28] = guest_gp;
        registers[31] = guest_entry + 0x4Cu;
        (void)xport_gpu_guest_dispatch(target, registers, 0x90060035u);
    }
    (void)SYS_OBJ_149CPSX(environment + 28u, environment, state + 2u);
    value = r_u32(environment + 28u);
    driver = r_u32(state - 10u);
    w_u32(environment + 28u, value | 0xFFFFFFu);
    registers[0] = 0u;
    registers[4] = r_u32(driver + 24u);
    target = r_u32(driver + 8u);
    registers[2] = target;
    registers[3] = driver;
    registers[5] = environment + 28u;
    registers[6] = 64u;
    registers[7] = 0u;
    registers[16] = environment + 28u;
    registers[17] = environment;
    registers[18] = state;
    registers[28] = guest_gp;
    registers[31] = guest_entry + 0x94u;
    (void)xport_gpu_guest_dispatch(target, registers, 0x900700FDu);
    (void)copy_guest(state + 14u, environment, 92u);
    return environment;
}

uint32 PutDispEnvPSX(uint32 environment, uint32 debug_level_address, uint32 debug_callback_address, uint32 debug_format_address, uint32 driver_table_address, uint32 cache_address, uint32 reverse_flag_address, uint32 divisors_address, uint32 ranges_address, uint32 guest_gp, uint32 guest_entry, sint32 (*get_video_mode)(void), uint32 (*copy_bytes)(uint32 destination, uint32 source, uint32 bytes))
{
    uint32 mode = 0x08000000u, registers[32], target, driver, packed, command, value, cached, current, index, video, divisor, selector, start, end, delta, x0, x1, y0, y1, minimum;
    sint32 width, height, screen_x, screen_y, screen_w;
    sint32 changed;
    value = r_u8(debug_level_address);
    if (value >= 2u)
    {
        target = r_u32(debug_callback_address);
        registers[0] = 0u;
        registers[2] = target;
        registers[4] = debug_format_address;
        registers[5] = environment;
        registers[16] = mode;
        registers[17] = environment;
        registers[18] = debug_level_address;
        registers[28] = guest_gp;
        registers[31] = guest_entry + 0x50u;
        xport_gpu_guest_dispatch(target, registers, 0x90070035u);
    }
    packed = (r_u16(environment + 2u) & 0x3FFu) << 10;
    command = r_u16(environment) & 0x3FFu;
    driver = r_u32(driver_table_address);
    command |= 0x05000000u;
    target = r_u32(driver + 16u);
    command |= packed;
    registers[0] = 0u;
    registers[2] = target;
    registers[3] = packed;
    registers[4] = command;
    registers[5] = driver;
    registers[16] = mode;
    registers[17] = environment;
    registers[18] = debug_level_address;
    registers[28] = guest_gp;
    registers[31] = guest_entry + 0x84u;
    xport_gpu_guest_dispatch(target, registers, 0x9007003Du);
    cached = r_u32(cache_address + 16u);
    current = r_u32(environment + 16u);
    changed = cached != current;
    if (!changed)
    {
        cached = (uint32)(sint32)(sint16)r_u16(cache_address + 0u);
        current = (uint32)(sint32)r_s16(environment + 0u);
        changed = cached != current;
    }
    if (!changed)
    {
        cached = (uint32)(sint32)(sint16)r_u16(cache_address + 2u);
        current = (uint32)(sint32)r_s16(environment + 2u);
        changed = cached != current;
    }
    if (!changed)
    {
        cached = (uint32)(sint32)(sint16)r_u16(cache_address + 4u);
        current = (uint32)(sint32)r_s16(environment + 4u);
        changed = cached != current;
    }
    if (!changed)
    {
        cached = (uint32)(sint32)(sint16)r_u16(cache_address + 6u);
        current = (uint32)(sint32)r_s16(environment + 6u);
        changed = cached != current;
    }
    if (changed)
    {
        value = (uint32)get_video_mode();
        w_u8(environment + 18u, (uint8)value);
        if ((value & 0xFFu) == 1u)
            mode |= 8u;
        if (r_u8(environment + 17u))
            mode |= 16u;
        if (r_u8(environment + 16u))
            mode |= 32u;
        if (r_u8(reverse_flag_address))
            mode |= 128u;
        width = r_s16(environment + 4u);
        if (width >= 281 && width < 353)
            mode |= 1u;
        else if (width >= 353 && width < 401)
            mode |= 64u;
        else if (width >= 401 && width < 561)
            mode |= 2u;
        else if (width >= 561)
            mode |= 3u;
        video = r_u8(environment + 18u);
        height = r_s16(environment + 6u);
        if (height >= (video ? 289 : 257))
            mode |= 36u;
        driver = r_u32(driver_table_address);
        target = r_u32(driver + 16u);
        registers[0] = 0u;
        registers[2] = target;
        registers[3] = (uint32)height;
        registers[4] = mode;
        registers[16] = mode;
        registers[17] = environment;
        registers[18] = debug_level_address;
        registers[28] = guest_gp;
        registers[31] = guest_entry + 0x1DCu;
        xport_gpu_guest_dispatch(target, registers, 0x9007001Du);
        w_u8(environment + 18u, 8u);
    }
    changed = 0;
    if (!changed)
    {
        cached = (uint32)(sint32)(sint16)r_u16(cache_address + 8u);
        current = (uint32)(sint32)r_s16(environment + 8u);
        changed = cached != current;
    }
    if (!changed)
    {
        cached = (uint32)(sint32)(sint16)r_u16(cache_address + 10u);
        current = (uint32)(sint32)r_s16(environment + 10u);
        changed = cached != current;
    }
    if (!changed)
    {
        cached = (uint32)(sint32)(sint16)r_u16(cache_address + 12u);
        current = (uint32)(sint32)r_s16(environment + 12u);
        changed = cached != current;
    }
    if (!changed)
    {
        cached = (uint32)(sint32)(sint16)r_u16(cache_address + 14u);
        current = (uint32)(sint32)r_s16(environment + 14u);
        changed = cached != current;
    }
    if (!changed)
        changed = r_u8(environment + 18u) == 8u;
    if (changed)
    {
        value = (uint32)get_video_mode();
        w_u8(environment + 18u, (uint8)value);
        screen_y = r_s16(environment + 10u);
        y0 = (uint32)screen_y + ((value & 0xFFu) ? 19u : 16u);
        height = r_s16(environment + 14u);
        y1 = y0 + (height ? (uint32)height : 240u);
        width = r_s16(environment + 4u);
        index = width < 281 ? 0u : width < 353 ? 1u : width < 401 ? 2u : width < 561 ? 3u : 4u;
        screen_x = r_s16(environment + 8u);
        divisor = r_u8(divisors_address + index);
        video = r_u8(environment + 18u);
        selector = (5u * video + index) * 4u;
        start = r_u16(ranges_address + selector);
        end = r_u16(ranges_address + selector + 2u);
        screen_w = r_s16(environment + 12u);
        delta = end - start;
        x0 = start + (uint32)((sint64)screen_x * (sint64)divisor);
        if (screen_w)
        {
            delta = (uint32)((sint32)(uint32)((sint64)(sint32)delta * (sint64)screen_w) >> 8);
        }
        x1 = x0 + delta;
        video = r_u8(environment + 18u);
        if (video)
        {
            x0 = (sint32)x0 < 540 ? 540u : (sint32)x0 < 3221 ? x0 : 3220u;
            divisor = r_u8(divisors_address + index);
            minimum = x0 + 4u * divisor;
            x1 = (sint32)x1 < (sint32)minimum ? minimum : (sint32)x1 < 3261 ? x1 : 3260u;
            y0 = (sint32)y0 < 19 ? 19u : (sint32)y0 < 304 ? y0 : 303u;
            minimum = y0 + 2u;
            value = 305u;
        }
        else
        {
            x0 = (sint32)x0 < 500 ? 500u : (sint32)x0 < 3251 ? x0 : 3250u;
            divisor = r_u8(divisors_address + index);
            minimum = x0 + 4u * divisor;
            x1 = (sint32)x1 < (sint32)minimum ? minimum : (sint32)x1 < 3291 ? x1 : 3290u;
            y0 = (sint32)y0 < 16 ? 16u : (sint32)y0 < 258 ? y0 : 257u;
            minimum = y0 + 2u;
            value = 258u;
        }
        y1 = (sint32)y1 < (sint32)minimum ? minimum : (sint32)y1 < (sint32)(value + 1u) ? y1 : value;
        packed = (x1 & 0xFFFu) << 12;
        command = 0x06000000u | (x0 & 0xFFFu);
        driver = r_u32(driver_table_address);
        target = r_u32(driver + 16u);
        command |= packed;
        registers[0] = 0u;
        registers[2] = target;
        registers[3] = packed;
        registers[4] = command;
        registers[5] = driver;
        registers[16] = y0;
        registers[17] = environment;
        registers[18] = y1;
        registers[28] = guest_gp;
        registers[31] = guest_entry + 0x49Cu;
        xport_gpu_guest_dispatch(target, registers, 0x9007003Du);
        packed = (y1 & 0x3FFu) << 10;
        command = 0x07000000u | (y0 & 0x3FFu);
        driver = r_u32(driver_table_address);
        target = r_u32(driver + 16u);
        command |= packed;
        registers[0] = 0u;
        registers[2] = target;
        registers[3] = packed;
        registers[4] = command;
        registers[5] = driver;
        registers[16] = y0;
        registers[17] = environment;
        registers[18] = y1;
        registers[28] = guest_gp;
        registers[31] = guest_entry + 0x4C8u;
        xport_gpu_guest_dispatch(target, registers, 0x9007003Du);
    }
    copy_bytes(cache_address, environment, 20u);
    return environment;
}

#if !defined(XPORT_GAME_GPU_GUEST_DISPATCH) || !XPORT_GAME_GPU_GUEST_DISPATCH
uint32 xport_gpu_guest_dispatch(uint32 target, uint32 *registers, uint32 known_register_mask)
{
    (void)target;
    (void)registers;
    (void)known_register_mask;
    abort();
}
#endif

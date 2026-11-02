#include <windows.h>
#define XPORT_NO_PSYQ_CRITICAL_SECTION
#include "psx.h"
#include "psx_gpu.h"
#include "xport_trace.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>


// PsyQ packet sizes include the four-byte DMA tag
typedef char gpu_line3_abi_size[(sizeof(LINE_F3) == 24) ? 1 : -1];
typedef char gpu_ft4_abi_size[(sizeof(POLY_FT4) == 40) ? 1 : -1];
typedef char gpu_gt3_abi_size[(sizeof(POLY_GT3) == 40) ? 1 : -1];
typedef char gpu_sprite_abi_size[(sizeof(SPRT) == 20) ? 1 : -1];
typedef char gpu_draw_mode_abi_size[(sizeof(DR_MODE) == 12) ? 1 : -1];

uint8 DRAM[PSX_DRAM_SIZE];
uint32 SCRATCHPAD[256];

// Standalone fixture provides deterministic host services and RAM accesses
static uint64 fixture_now;
static uint32 fixture_present_count;
static uint32 fixture_present_left, fixture_present_right;

uint64 xport_timer_get(void)
{
    return fixture_now;
}

sint32 GetVideoMode(void)
{
    return MODE_NTSC;
}

uint8 r_u8(uint32 addr)
{
    return *(uint8 *)psx_addr(addr, 1);
}

uint16 r_u16(uint32 addr)
{
    const uint8 *p = psx_addr(addr, 2);
    return p[0] | (uint16)p[1] << 8;
}

sint16 r_s16(uint32 addr)
{
    return (sint16)r_u16(addr);
}

// Legacy SDK scratch frames and IRQ execution are outside this GPU fixture
PsxInterruptCallback DMACallback(uint32 channel, PsxInterruptCallback callback)
{
    fprintf(stderr, "Unexpected DMA callback registration in isolated GPU fixture\n");
    exit(90);
    return NULL;
}

sint32 psx_irq_native_acknowledge(uint32 mask)
{
    abort();
    return 0;
}

static sint32 fixture_gpu_callbacks;
static uint32 fixture_gpu_completions;
static void (*fixture_gpu_service)(void);

/* Only the explicit GPU callback regression enables cooperative delivery */
sint32 psx_callbacks_allowed(void)
{
    if (!fixture_gpu_callbacks)
        abort();
    return 1;
}

void psx_wait_bind_service(uint32 slot, void (*service)(void))
{
    if (!fixture_gpu_callbacks || slot != 2u)
        abort();
    fixture_gpu_service = service;
}

void psx_wait_signal_service(uint32 slot)
{
    if (!fixture_gpu_callbacks || slot != 2u || !fixture_gpu_service)
        abort();
}

void psx_sync_callbacks(void)
{
}

uint32 xport_sdk_texture_frame_acquire(size_t bytes)
{
    abort();
    return 0;
}

void xport_sdk_texture_frame_release(uint32 address, size_t bytes)
{
    abort();
}

uint32 xport_sdk_draw_env_frame_acquire(size_t bytes)
{
    abort();
    return 0;
}

void xport_sdk_draw_env_frame_release(uint32 address, size_t bytes)
{
    abort();
}

uint32 r_u32(uint32 addr)
{
    const uint8 *p = psx_addr(addr, 4);
    return (uint32)p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16) | ((uint32)p[3] << 24);
}

#undef w_u8
#undef w_u16
#undef w_u32
#undef w_masked_u32

sint32 w_u8(uint32 addr, uint32 value)
{
    *(uint8 *)psx_addr(addr, 1) = (uint8)value;
    return 1;
}

sint32 w_u16(uint32 addr, uint32 value)
{
    uint8 *p = psx_addr(addr, 2);
    p[0] = (uint8)value;
    p[1] = (uint8)(value >> 8);
    return 1;
}

sint32 w_u32(uint32 addr, uint32 value)
{
    uint8 *p = psx_addr(addr, 4);
    uint32 i;
    for (i = 0; i < 4; ++i)
        p[i] = (uint8)(value >> (8 * i));
    return 1;
}

sint32 w_masked_u32(uint32 addr, uint32 value, uint32 mask)
{
    uint8 *p = psx_addr(addr, 4);
    uint32 i;
    for (i = 0; i < 4; ++i)
        if (mask & (1u << i))
            p[i] = (uint8)(value >> (8 * i));
    return 1;
}

const uint32 xport_gpu_graph_type_address = 0x80010000u;
static uint8 fixture_spu_ram[512 * 1024];

const uint8 *spu_ram_data(void)
{
    return fixture_spu_ram;
}

uint32 xport_guest_call3(uint32 target, uint32 argument0, uint32 argument1, uint32 argument2)
{
    (void)target;
    (void)argument0;
    (void)argument1;
    (void)argument2;
    abort();
    return 0;
}

void xport_gpu_clear_otag_r_guest_driver(uint32 guest_ot, uint32 count)
{
    (void)guest_ot;
    (void)count;
    abort();
}

static uint32 seen;
static uint32 operations[64], addresses[64], sizes[64], payloads[64][16];
static uint32 event_ticks[64], dma_ticks, dma_words;
static sint32 dma_request;
static uint16 initial_vram[GPU_VRAM_WIDTH * GPU_VRAM_HEIGHT];
static uint32 sync_dma, sync_status, pumps, waits;
static sint32 drain_queue, make_ready, reenter;
static uint32 device_events, device_fetches, device_candidates;

static void observe_device(void *context, const XportDeviceEvent *event)
{
    if (context != &device_events)
        abort();
    ++device_events;
    if (event->phase == XPORT_DEVICE_FETCH)
        ++device_fetches;
    if (event->evidence == XPORT_DEVICE_EVIDENCE_CANDIDATE)
        ++device_candidates;
}

static sint32 sample_sync(void *context, uint32 *dma, uint32 *status)
{
    if (context != &pumps)
        abort();
    *dma = sync_dma;
    *status = sync_status;
    return 1;
}

static sint32 pump_sync(void *context)
{
    ++pumps;
    if (drain_queue)
        *(uint32 *)(DRAM + 0x14004) = *(uint32 *)(DRAM + 0x14000);
    if (reenter)
        DrawSync(1);
    return 1;
}

static sint32 wait_sync(void *context)
{
    ++waits;
    if (make_ready)
    {
        sync_dma = 0;
        sync_status = 0x04000000u;
    }
    return 1;
}

static sint32 test_sync(void)
{
    uint32 *head = (uint32 *)(DRAM + 0x14000), *tail = head + 1;
    if (!gpu_bind_sync(0x80014000u, 0xa0014004u, sample_sync, pump_sync, wait_sync, &pumps, 4))
        return 0;
    *head = 1;
    *tail = 62;
    drain_queue = 1;
    sync_status = 0x04000000u;
    if (DrawSync(-1) != 3 || pumps != 1 || waits || gpu_sync_failed())
        return 0;
    sync_dma = 0x01000000u;
    if (DrawSync(2) != 1 || pumps != 1 || waits)
        return 0;
    make_ready = 1;
    if (DrawSync(0) != 0 || waits != 1)
        return 0;
    *head = 2;
    sync_dma = 0x01000000u;
    if (DrawSync(0) != 0 || pumps != 2 || waits != 2)
        return 0;
    make_ready = 0;
    sync_status = 0;
    if (DrawSync(0) != -1 || !gpu_sync_failed() || waits != 6 || DrawSync(1) != -1)
        return 0;
    if (!gpu_bind_sync(0x14000u, 0x14004u, sample_sync, pump_sync, wait_sync, &pumps, 4))
        return 0;
    *head = 3;
    reenter = 1;
    if (DrawSync(1) != -1 || !gpu_sync_failed())
        return 0;
    if (gpu_bind_sync(0x14000u, 0x80014000u, sample_sync, pump_sync, wait_sync, &pumps, 4))
        return 0;
    if (!gpu_bind_sync(0, 0, NULL, NULL, NULL, NULL, 0) || gpu_sync_failed() || DrawSync(0))
        return 0;
    return 1;
}

static sint32 test_timeout(void)
{
    GpuPsyqTimeout binding = {0x80024000u, 0x80024004u, 0xa0024008u}, invalid;
    uint32 *counter = (uint32 *)(DRAM + 0x24000), *deadline = counter + 1, *polls = counter + 2;
    *counter = 1799;
    if (gpu_psyq_timeout_begin(&binding) != 1 || *deadline != 2039 || *polls)
        return 0;
    *counter = 2039;
    if (gpu_psyq_timeout_check(&binding) != 1 || *polls != 1)
        return 0;
    *counter = 2040;
    if (gpu_psyq_timeout_check(&binding) != 0 || *polls != 1)
        return 0;
    *counter = 2039;
    *polls = 0xf0000u;
    if (gpu_psyq_timeout_check(&binding) != 1 || *polls != 0xf0001u || gpu_psyq_timeout_check(&binding) != 0 || *polls != 0xf0002u)
        return 0;
    *polls = 0xffffffffu;
    if (gpu_psyq_timeout_check(&binding) != 1 || *polls != 0)
        return 0;
    *counter = 0x7ffffff0u;
    if (gpu_psyq_timeout_begin(&binding) != 1 || *deadline != 0x800000e0u || gpu_psyq_timeout_check(&binding) != 0 || *polls)
        return 0;
    invalid = binding;
    invalid.polls = binding.deadline;
    if (gpu_psyq_timeout_begin(&invalid) != -1 || *deadline != 0x800000e0u)
        return 0;
    if (!gpu_bind_sync(0x14000u, 0x14004u, sample_sync, pump_sync, wait_sync, &pumps, 4) || !gpu_bind_sync_timeout(&binding))
        return 0;
    invalid = binding;
    invalid.deadline = 0x80014000u;
    if (gpu_bind_sync_timeout(&invalid))
        return 0;
    *(uint32 *)(DRAM + 0x14000) = 1;
    *(uint32 *)(DRAM + 0x14004) = 0;
    reenter = 0;
    drain_queue = make_ready = 1;
    sync_dma = 0x01000000u;
    sync_status = 0x04000000u;
    *counter = 100;
    if (DrawSync(0) != 0 || *deadline != 340 || *polls != 2)
        return 0;
    *deadline = 123;
    *polls = 456;
    if (DrawSync(1) != 0 || *deadline != 123 || *polls != 456)
        return 0;
    gpu_bind_sync(0, 0, NULL, NULL, NULL, NULL, 0);
    return 1;
}

static sint32 test_queue(void)
{
    GpuPsyqQueue queue = {0x80020000u, 0x80022000u, 0xa0022004u, 0x80022008u};
    GpuPsyqCommand input = {0x8006dc2cu, 0x80023000u, 0x12345678u}, actual;
    uint32 *head = (uint32 *)(DRAM + 0x22000), *tail = head + 1;
    uint32 *source = (uint32 *)(DRAM + 0x23000);
    uint32 i;
    *head = *tail = 63;
    memset(DRAM + 0x20000, 0x5a, 6144);
    for (i = 0; i < 21u; ++i)
        source[i] = 0xe1000000u + i;
    if (gpu_psyq_queue_peek(&queue, &actual) != 0 || gpu_psyq_queue_retire(&queue) != 0)
        return 0;
    if (gpu_psyq_queue_store(&queue, &input, 84) != 1 || *head != 0 || *tail != 63 || gpu_psyq_queue_peek(&queue, &actual) != 1 || actual.function != input.function || actual.argument != queue.entries + 63u * 96u + 12u || actual.extra != input.extra || memcmp(psx_addr(actual.argument, 84), source, 84))
        return 0;
    if (gpu_psyq_queue_retire(&queue) != 1 || *tail != 0 || memcmp(psx_addr(queue.last_command, 12), &actual, 12))
        return 0;
    if (gpu_psyq_queue_store(&queue, &input, 7) != 1 || *(uint32 *)(DRAM + 0x2000c) != source[0] || *(uint32 *)(DRAM + 0x20010) != 0x5a5a5a5au)
        return 0;
    if (gpu_psyq_queue_retire(&queue) != 1 || gpu_psyq_queue_store(&queue, &input, 0) != 1 || gpu_psyq_queue_peek(&queue, &actual) != 1 || actual.argument != input.argument)
        return 0;
    *tail = (*head + 1u) & 63u;
    if (gpu_psyq_queue_store(&queue, &input, 0) != 0 || *head != 2)
        return 0;
    if (gpu_psyq_queue_store(&queue, &input, 85) != -1 || gpu_psyq_queue_store(&queue, &input, -4) != -1 || *head != 2)
        return 0;
    queue.tail = queue.head;
    if (gpu_psyq_queue_peek(&queue, &actual) != -1)
        return 0;
    return 1;
}

static uint32 irq_enabled, dispatched, completed, dma_callbacks;
static uint32 registered_dma, dispatched_argument, timeout_starts;
static sint32 dispatch_dma, dispatch_error;
static sint32 dispatch_execute;

static sint32 queue_critical(void *context, uint32 enabled, uint32 *previous)
{
    *previous = irq_enabled;
    irq_enabled = enabled;
    return 1;
}

static sint32 queue_dma_callback(void *context, uint32 function)
{
    if ((function && function != 0x8006dfb0u) || irq_enabled)
        abort();
    registered_dma = function;
    ++dma_callbacks;
    return 1;
}

static sint32 queue_dispatch(void *context, const GpuPsyqCommand *command)
{
    if (irq_enabled || command->function != 0x8006dc2cu)
        abort();
    if (dispatch_error)
        return 0;
    ++dispatched;
    dispatched_argument = command->argument;
    if (dispatch_dma)
        sync_dma = 0x01000000u;
    if (dispatch_execute)
        return gpu_execute_linked_dma(command->argument & 0x1fffffffu);
    return 1;
}

static sint32 queue_complete(void *context, uint32 function)
{
    if (!irq_enabled || function != 0x80010000u || *(uint32 *)(DRAM + 0x22014))
        abort();
    ++completed;
    return 1;
}

static sint32 test_queue_pump(void)
{
    GpuPsyqQueueDriver driver = {{0x80020000u, 0x80022000u, 0x80022004u, 0x80022008u}, 0x80022014u, 0x80022018u, 0x8002201cu, sample_sync, wait_sync, queue_critical, queue_dma_callback, queue_dispatch, queue_complete, &pumps, 16u};
    GpuPsyqCommand command = {0x8006dc2cu, 0x80023000u, 0};
    uint32 *head = (uint32 *)(DRAM + 0x22000), *tail = head + 1;
    memset(head, 0, 32);
    irq_enabled = 1;
    sync_dma = 0;
    sync_status = 0x04000000u;
    if (gpu_psyq_queue_store(&driver.queue, &command, 0) != 1 || gpu_psyq_queue_store(&driver.queue, &command, 0) != 1)
        return 0;
    dispatch_dma = 1;
    if (gpu_psyq_queue_pump(&driver) != 1 || *tail != 1 || dispatched != 1 || !irq_enabled || dma_callbacks || completed)
        return 0;
    if (gpu_psyq_queue_pump(&driver) != 1 || dispatched != 1 || *tail != 1)
        return 0;
    sync_dma = 0;
    dispatch_dma = 0;
    sync_status = 0;
    make_ready = 1;
    if (gpu_psyq_queue_pump(&driver) != 0 || *tail != 2 || dispatched != 2 || dma_callbacks != 1 || !irq_enabled || completed)
        return 0;
    *(uint32 *)psx_addr(driver.active, 4) = 1;
    *(uint32 *)psx_addr(driver.completion, 4) = 0x80010000u;
    sync_dma = 0x01000000u;
    if (gpu_psyq_queue_pump(&driver) != 1 || completed)
        return 0;
    sync_dma = 0;
    if (gpu_psyq_queue_pump(&driver) != 0 || completed != 1 || gpu_psyq_queue_pump(&driver) != 0 || completed != 1)
        return 0;
    dispatch_error = 1;
    if (gpu_psyq_queue_store(&driver.queue, &command, 0) != 1 || gpu_psyq_queue_pump(&driver) != -1 || *tail != 2 || !irq_enabled)
        return 0;
    driver.active = driver.queue.head;
    if (gpu_psyq_queue_pump(&driver) != -1 || *head != 3 || *tail != 2)
        return 0;
    return 1;
}

static sint32 queue_timeout_begin(void *context)
{
    ++timeout_starts;
    return 1;
}

static sint32 queue_timeout_check(void *context)
{
    return 1;
}

static sint32 test_queue_submit(void)
{
    GpuPsyqQueueDriver driver = {{0x80020000u, 0x80022000u, 0x80022004u, 0x80022008u}, 0x80022014u, 0x80022018u, 0x8002201cu, sample_sync, wait_sync, queue_critical, queue_dma_callback, queue_dispatch, queue_complete, &pumps, 16u};
    GpuPsyqSubmit submit = {&driver, 0x80022021u, 0x80022024u, 0x8006dfb0u, queue_timeout_begin, queue_timeout_check};
    GpuPsyqCommand command = {0x8006dc2cu, 0x80023000u, 0x12345678u}, actual;
    uint32 *head = (uint32 *)(DRAM + 0x22000), *tail = head + 1;
    memset(head, 0, 40);
    DRAM[0x22021] = 1;
    irq_enabled = 1;
    sync_dma = 0;
    sync_status = 0x04000000u;
    dispatched = completed = dma_callbacks = registered_dma = 0;
    dispatch_error = dispatch_dma = 0;
    if (gpu_psyq_queue_submit(&submit, &command, 12) != 0 || *head || *tail || dispatched != 1 || dispatched_argument != command.argument || dma_callbacks || !irq_enabled || memcmp(psx_addr(driver.queue.last_command, 12), &command, 12))
        return 0;
    sync_dma = 0x01000000u;
    if (gpu_psyq_queue_submit(&submit, &command, 12) != 1 || dispatched != 1 || *head != 1 || *tail || registered_dma != submit.pump_function || !irq_enabled || gpu_psyq_queue_peek(&driver.queue, &actual) != 1 || actual.argument != 0x8002000cu)
        return 0;
    sync_dma = 0;
    if (gpu_psyq_queue_pump(&driver) != 0 || dispatched != 2 || dispatched_argument != actual.argument)
        return 0;
    *(uint32 *)psx_addr(driver.completion, 4) = 0x80010000u;
    if (gpu_psyq_queue_submit(&submit, &command, 0) != 0 || *head != 2 || *tail != 2 || dispatched != 3 || completed != 1 || dispatched_argument != command.argument)
        return 0;
    DRAM[0x22021] = 0;
    sync_dma = 0x01000000u;
    if (gpu_psyq_queue_submit(&submit, &command, 12) != 0 || *head != 2 || *tail != 2 || dispatched != 4 || completed != 1 || dispatched_argument != command.argument || timeout_starts != 4)
        return 0;
    submit.saved_interrupt_mask = driver.active;
    if (gpu_psyq_queue_submit(&submit, &command, 0) != -1 || timeout_starts != 4)
        return 0;
    return 1;
}

static const GpuPsyqSubmit *linked_driver;
static uint32 linked_copy;

static sint32 submit_linked(void *context, uint32 physical, uint32 copy_bytes)
{
    GpuPsyqCommand command = {0x8006dc2cu, 0x80000000u | physical, 0};
    linked_copy = copy_bytes;
    return gpu_psyq_queue_submit(linked_driver, &command, (sint32)copy_bytes) >= 0;
}

static sint32 test_linked_routing(void)
{
    GpuPsyqQueueDriver driver = {{0x80020000u, 0x80022000u, 0x80022004u, 0x80022008u}, 0x80022014u, 0x80022018u, 0x8002201cu, sample_sync, wait_sync, queue_critical, queue_dma_callback, queue_dispatch, queue_complete, &pumps, 16u};
    GpuPsyqSubmit submit = {&driver, 0x80022021u, 0x80022024u, 0x8006dfb0u, queue_timeout_begin, queue_timeout_check};
    GpuRasterState before, after;
    memset(DRAM + 0x22000, 0, 40);
    DRAM[0x22021] = 1;
    linked_driver = &submit;
    gpu_bind_linked_submit(submit_linked, NULL);
    irq_enabled = 1;
    dispatch_error = dispatch_dma = 0;
    dispatch_execute = 1;
    sync_dma = 0x01000000u;
    sync_status = 0x04000000u;
    seen = 0;
    gpu_export_raster(&before);
    gpu_set_def_draw_env(0x12000u, 0, 0, 320, 240);
    gpu_put_draw_env(0x12000u, 0, 0);
    gpu_export_raster(&after);
    if (seen || linked_copy != 64u || gpu_linked_submit_failed() || memcmp(&before, &after, sizeof(before)) || memcmp(DRAM + 0x2000c, DRAM + 0x1201c, 64u))
        return 0;
    *(uint32 *)(DRAM + 0x12020) = 0xe30000ffu;
    sync_dma = 0;
    if (gpu_psyq_queue_pump(&driver) != 0 || seen != 3 || addresses[1] != 0x2000cu || addresses[2] != 0x20010u || payloads[2][0] != 0xe3000000u || gpu_linked_submit_failed())
        return 0;
    *(uint32 *)(DRAM + 0x10000) = 0x00ffffffu;
    sync_dma = 0x01000000u;
    DrawOTag((uint32 *)(DRAM + 0x10000));
    if (seen != 3 || linked_copy != 0 || gpu_linked_submit_failed())
        return 0;
    sync_dma = 0;
    if (gpu_psyq_queue_pump(&driver) != 0 || seen != 5 || addresses[4] != 0x10000u)
        return 0;
    gpu_bind_linked_submit(NULL, NULL);
    dispatch_execute = 0;
    return 1;
}

static sint32 test_raster_import(void)
{
    GpuRasterState initial = {0x1fu, 0x400u, 0u, {0u, 256u, 511u, 511u}, {0, 256}};
    GpuRasterState actual, invalid;
    uint32 i;
    for (i = 0; i < GPU_VRAM_WIDTH * GPU_VRAM_HEIGHT; ++i)
        initial_vram[i] = (uint16)(i ^ (i >> 16));
    if (!gpu_import_raster(&initial, initial_vram, sizeof(initial_vram)))
        return 0;
    gpu_export_raster(&actual);
    if (memcmp(&initial, &actual, sizeof(initial)) || memcmp(VRAM, initial_vram, sizeof(initial_vram)))
        return 0;
    invalid = initial;
    invalid.draw_flags |= 0x2000u;
    if (gpu_import_raster(&invalid, initial_vram, sizeof(initial_vram)))
        return 0;
    invalid = initial;
    invalid.area[2] = GPU_VRAM_WIDTH;
    if (gpu_import_raster(&invalid, initial_vram, sizeof(initial_vram)) || gpu_import_raster(&initial, initial_vram, sizeof(initial_vram) - 1u))
        return 0;
    gpu_export_raster(&actual);
    if (memcmp(&initial, &actual, sizeof(initial)) || memcmp(VRAM, initial_vram, sizeof(initial_vram)))
        return 0;
    initial.offset[0] = -1024;
    initial.offset[1] = 1023;
    initial.area[0] = 100u;
    initial.area[2] = 0u;
    if (!gpu_import_raster(&initial, initial_vram, sizeof(initial_vram)))
        return 0;
    gpu_export_raster(&actual);
    return !memcmp(&initial, &actual, sizeof(initial));
}

sint32 xport_window_init(void)
{
    abort();
    return 0;
}

FILE *xport_fopen(const char *path, const char *mode)
{
    abort();
    return NULL;
}

sint32 xport_is_headless(void)
{
    return 0;
}

sint32 xport_file_read(const char *path, void *data, size_t capacity, size_t *size)
{
    abort();
    return 0;
}

sint32 xport_present(const uint32 *pixels, sint32 w, sint32 h, sint32 x, sint32 y, sint32 sw, sint32 sh, const char *title)
{
    ++fixture_present_count;
    if (pixels && w > 0 && h > 0)
    {
        fixture_present_left = pixels[0];
        fixture_present_right = pixels[w - 1];
        return 1;
    }
    return 0;
}

void *psx_addr(uint32 address, size_t size)
{
    address &= 0x1fffffffu;
    if (address >= sizeof(DRAM) || size > sizeof(DRAM) - address)
        abort();
    return DRAM + address;
}

sint32 xport_memory_readable(const void *address, size_t size)
{
    uintptr_t begin = (uintptr_t)address, end;
    if (!address || !size || size > UINTPTR_MAX - begin)
        return 0;
    end = begin + size;
    while (begin < end)
    {
        MEMORY_BASIC_INFORMATION region;
        uintptr_t next;
        if (!VirtualQuery((const void *)begin, &region, sizeof(region)) || region.State != MEM_COMMIT || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || (region.Protect & 255u) == PAGE_EXECUTE)
            return 0;
        next = (uintptr_t)region.BaseAddress + region.RegionSize;
        if (next <= begin)
            return 0;
        begin = next < end ? next : end;
    }
    return 1;
}

static void observe(void *context, uint32 operation, uint32 address, const uint32 *words, uint32 count)
{
    if (context != &seen || seen >= 64 || count > 16)
        abort();
    operations[seen] = operation;
    addresses[seen] = address;
    sizes[seen] = count;
    event_ticks[seen] = dma_ticks;
    memcpy(payloads[seen], words, count * 4u);
    ++seen;
}

static sint32 slice_request(void *context)
{
    return dma_request;
}

static sint32 slice_ticks(void *context, uint32 ticks)
{
    dma_ticks += ticks;
    return 1;
}

static sint32 slice_write(void *context, uint32 address, const uint32 *words, uint32 count)
{
    dma_words += count;
    return 1;
}

static sint32 test_dma_slice(void)
{
    GpuLinkedDmaState state = {0x25000u, 1u};
    uint32 *packet = (uint32 *)(DRAM + 0x25000u);
    packet[0] = 0x03025010u;
    packet[1] = 1;
    packet[2] = 2;
    packet[3] = 3;
    packet[4] = 0x00025014u;
    packet[5] = 0x00ffffffu;
    seen = dma_ticks = dma_words = 0;
    dma_request = 1;
    if (gpu_dma_linked_slice(&state, 10, slice_request, slice_write, slice_ticks, NULL) != 2 || state.address != 0x25010u || state.busy != 1 || dma_ticks != 17 || dma_words != 3 || seen != 2 || operations[0] != 3 || event_ticks[0] != 0 || operations[1] != 2 || event_ticks[1] != 13)
        return 0;
    dma_request = 0;
    if (gpu_dma_linked_slice(&state, 10, slice_request, slice_write, slice_ticks, NULL) != 0 || seen != 2 || state.address != 0x25010u)
        return 0;
    dma_request = 1;
    if (gpu_dma_linked_slice(&state, 10, slice_request, slice_write, slice_ticks, NULL) != 1 || state.address != 0xffffffu || state.busy || dma_ticks != 33 || seen != 4 || event_ticks[2] != 17 || event_ticks[3] != 25 || dma_words != 3)
        return 0;
    state.address = PSX_DRAM_SIZE - 4u;
    state.busy = 1;
    if (gpu_dma_linked_slice(&state, 10, slice_request, slice_write, slice_ticks, NULL) != -1 || seen != 4)
        return 0;
    return 1;
}

static sint32 test_flat_polylines(void)
{
    static uint16 expected[GPU_VRAM_WIDTH * GPU_VRAM_HEIGHT];
    uint32 *packet = (uint32 *)(DRAM + 0x1a000u);
    uint32 *line = packet + 16;
    GpuRasterState state = {0u, 0u, 0u, {0u, 0u, 1023u, 511u}, {0, 0}};
    uint32 variant, index;
    gpu_set_transfer_observer(NULL, NULL);
    for (variant = 0; variant < 128u; ++variant)
    {
        uint32 vertices = variant & 64u ? 5u : 3u + (variant & 1u);
        uint32 color = 0x4800ffffu | ((variant & 2u) << 24) | ((variant & 32u) << 21);
        uint32 terminator = variant & 4u ? 0x55555555u : 0x50005000u;
        state.offset[0] = variant & 8u ? -7 : 3;
        state.offset[1] = variant & 8u ? 5 : -2;
        state.area[0] = variant & 16u ? 24u : 0u;
        memset(initial_vram, 0, sizeof(initial_vram));
        if (!gpu_import_raster(&state, initial_vram, sizeof(initial_vram)))
            return 0;
        packet[0] = ((vertices + 2u) << 24) | 0xffffffu;
        packet[1] = color;
        packet[2] = 0x00100010u;
        packet[3] = 0x00100030u;
        packet[4] = 0x00300030u;
        packet[5] = vertices >= 4u ? 0x00300010u : terminator;
        packet[6] = packet[2];
        packet[vertices + 2u] = terminator;
        packet[vertices + 3u] = 0x00600060u;
        line[0] = 0x03ffffffu;
        line[1] = (color & 0x00ffffffu) | ((color & 0x02000000u) | 0x40000000u);
        for (index = 0; index + 1u < vertices; ++index)
        {
            line[2] = packet[2u + index];
            line[3] = packet[3u + index];
            DrawPrim(line);
        }
        memcpy(expected, VRAM, sizeof(expected));
        if (!expected[(32 + state.offset[1]) * GPU_VRAM_WIDTH + 48 + state.offset[0]])
            return 0;
        if (!gpu_import_raster(&state, initial_vram, sizeof(initial_vram)))
            return 0;
        // Flat line opcode bit two does not change the rasterized segments
        line[1] |= 0x04000000u;
        for (index = 0; index + 1u < vertices; ++index)
        {
            line[2] = packet[2u + index];
            line[3] = packet[3u + index];
            DrawPrim(line);
        }
        if (memcmp(VRAM, expected, sizeof(expected)))
            return 0;
        if (!gpu_import_raster(&state, initial_vram, sizeof(initial_vram)))
            return 0;
        DrawPrim(packet);
        if (memcmp(VRAM, expected, sizeof(expected)))
            return 0;
        // A missing terminator stays bounded by the packet length
        packet[0] = ((vertices + 1u) << 24) | 0xffffffu;
        if (!gpu_import_raster(&state, initial_vram, sizeof(initial_vram)))
            return 0;
        DrawPrim(packet);
        if (memcmp(VRAM, expected, sizeof(expected)))
            return 0;
    }
    return 1;
}

static sint32 test_line_size_limits(void)
{
    const uint32 commands[6] = {0x40, 0x42, 0x50, 0x51, 0x52, 0x53};
    const uint32 endpoints[5][2] = {
        {0xFC00FC00, 0x00640064},
        {0x0064FFFF, 0x006403FF},
        {0xFFFF0064, 0x01FF0064},
        {0x00640000, 0x006403FF},
        {0x00000064, 0x01FF0064}
    };
    GpuRasterState state = {0, 0, 0, {0, 0, 1023, 511}, {0, 0}};
    uint32 command, test, i, pixels, packet[5];
    gpu_set_transfer_observer(NULL, NULL);
    gpu_set_raster(1);
    memset(initial_vram, 0, sizeof(initial_vram));
    for (command = 0; command < 6; ++command)
    {
        for (test = 0; test < 5; ++test)
        {
            if (!gpu_import_raster(&state, initial_vram, sizeof(initial_vram)))
                return 0;
            packet[0] = (commands[command] & 0x10 ? 0x04000000 : 0x03000000) | 0xFFFFFF;
            packet[1] = (commands[command] << 24) | 0xFFFFFF;
            packet[2] = endpoints[test][0];
            packet[3] = commands[command] & 0x10 ? 0xFFFFFF : endpoints[test][1];
            packet[4] = endpoints[test][1];
            DrawPrim(packet);
            pixels = 0;
            for (i = 0; i < GPU_VRAM_WIDTH * GPU_VRAM_HEIGHT; ++i)
                if (VRAM[i])
                    ++pixels;
            if (pixels != (test < 3 ? 0 : test == 3 ? 1024 : 512))
                return 0;
        }
    }
    // Reject one oversized polyline segment and retain the following valid segment
    {
        uint32 polyline[] = {0x05FFFFFF, 0x48FFFFFF, 0xFC00FC00, 0x00640064, 0x00640065, 0x55555555};
        if (!gpu_import_raster(&state, initial_vram, sizeof(initial_vram)))
            return 0;
        DrawPrim(polyline);
        pixels = 0;
        for (i = 0; i < GPU_VRAM_WIDTH * GPU_VRAM_HEIGHT; ++i)
            if (VRAM[i])
                ++pixels;
        if (pixels != 2)
            return 0;
    }
    return 1;
}

static sint32 test_gouraud_line(void)
{
    uint32 line[] = {0x04FFFFFFu, 0x500000FFu, 0x000A000Au, 0x00FF0000u, 0x000A000Eu};
    GpuRasterState state = {0u, 0u, 0u, {0u, 0u, 1023u, 511u}, {0, 0}};
    gpu_set_transfer_observer(NULL, NULL);
    memset(initial_vram, 0, sizeof(initial_vram));
    if (!gpu_import_raster(&state, initial_vram, sizeof(initial_vram)))
        return 0;
    DrawPrim(line);
    if (VRAM[10u * GPU_VRAM_WIDTH + 10u] != 31u || VRAM[10u * GPU_VRAM_WIDTH + 12u] != 0x3C0Fu || VRAM[10u * GPU_VRAM_WIDTH + 14u] != 0x7C00u || VRAM[11u * GPU_VRAM_WIDTH + 12u] != 0u)
        return 0;
    state.area[2] = 12u;
    if (!gpu_import_raster(&state, initial_vram, sizeof(initial_vram)))
        return 0;
    DrawPrim(line);
    if (VRAM[10u * GPU_VRAM_WIDTH + 12u] != 0x3C0Fu || VRAM[10u * GPU_VRAM_WIDTH + 13u] != 0u)
        return 0;
    state.area[2] = 1023u;
    state.offset[0] = 3;
    state.offset[1] = 2;
    if (!gpu_import_raster(&state, initial_vram, sizeof(initial_vram)))
        return 0;
    DrawPrim(line);
    if (VRAM[12u * GPU_VRAM_WIDTH + 13u] != 31u || VRAM[12u * GPU_VRAM_WIDTH + 17u] != 0x7C00u)
        return 0;
    state.offset[0] = state.offset[1] = 0;
    line[1] = 0x520000FFu;
    if (!gpu_import_raster(&state, initial_vram, sizeof(initial_vram)))
        return 0;
    DrawPrim(line);
    if (VRAM[10u * GPU_VRAM_WIDTH + 10u] != 15u || VRAM[10u * GPU_VRAM_WIDTH + 14u] != 0x3C00u)
        return 0;
    return 1;
}

static sint32 test_native_draw_environment(void)
{
    static uint16 expected_pixels[GPU_VRAM_WIDTH * GPU_VRAM_HEIGHT];
    DRAWENV expected, native;
    DRAWENV *guest = psx_addr(0x1c000u, sizeof(DRAWENV));
    GpuRasterState before = {0u, 0u, 0u, {0u, 0u, 1023u, 511u}, {0, 0}}, expected_state, actual_state;
    uint32 variant;
    gpu_set_transfer_observer(NULL, NULL);
    memset(initial_vram, 0x39, sizeof(initial_vram));
    for (variant = 0; variant < 256u; ++variant)
    {
        memset(&native, 0x55, sizeof(native));
        native.clip.x = variant & 1u ? -9 : 0;
        native.clip.y = (sint16)(variant % 13u);
        native.clip.w = variant & 1u ? 29 : 64;
        native.clip.h = (sint16)(1u + variant % 31u);
        native.ofs[0] = (sint16)(variant % 17u);
        native.ofs[1] = (sint16)(variant % 19u);
        native.tw.x = (sint16)(variant * 3u);
        native.tw.y = (sint16)(variant * 7u);
        native.tw.w = (sint16)(variant * 11u);
        native.tw.h = (sint16)(variant * 13u);
        native.tpage = (uint16)(variant * 37u);
        native.dtd = (uint8)(variant & 2u);
        native.dfe = (uint8)(variant & 4u);
        native.isbg = (uint8)(variant & 8u);
        native.r0 = (uint8)variant;
        native.g0 = (uint8)(variant * 11u);
        native.b0 = (uint8)(variant * 19u);
        memcpy(guest, &native, sizeof(native));
        if (!gpu_import_raster(&before, initial_vram, sizeof(initial_vram)))
            return 0;
        gpu_put_draw_env(0x1c000u, 0, 0);
        memcpy(&expected, guest, sizeof(expected));
        memcpy(expected_pixels, VRAM, sizeof(expected_pixels));
        gpu_export_raster(&expected_state);
        memset(guest, 0xcc, sizeof(*guest));
        if (!gpu_import_raster(&before, initial_vram, sizeof(initial_vram)))
            return 0;
        if (PutDrawEnv(&native) != &native || memcmp(&expected, &native, sizeof(native)) || memcmp(VRAM, expected_pixels, sizeof(expected_pixels)) || memcmp(psx_addr(0x13100u, sizeof(DRAWENV)), &native, sizeof(native)))
            return 0;
        gpu_export_raster(&actual_state);
        if (memcmp(&expected_state, &actual_state, sizeof(actual_state)))
            return 0;
    }
    return 1;
}

static sint32 test_full_frame_render(void)
{
    PSX_RECT screen = {0, 0, 64, 32};
    uint32 tile[] = {0x03FFFFFF, 0x600000FF, 0x00100020, 0x00020002};
    GpuRasterState state = {0, 0, 0, {0, 0, 1023, 511}, {0, 0}};
    uint32 i, presented;
    gpu_set_transfer_observer(NULL, NULL);
    gpu_bind_linked_submit(NULL, NULL);
    gpu_bind_sync(0, 0, NULL, NULL, NULL, NULL, 0);
    xport_device_trace_bind(NULL, NULL);
    memset(initial_vram, 0, sizeof(initial_vram));
    if (!gpu_import_raster(&state, initial_vram, sizeof(initial_vram)))
        return 0;
    gpu_set_raster(1);
    gpu_set_display(0, 0, 64, 32);
    SetDispMask(1);
    presented = fixture_present_count;
    for (i = 0; i < 8; ++i)
    {
        // Long host delays must not suppress fills, primitives or presentation
        fixture_now += 200000;
        if (ClearImage(&screen, 0, 255, 0) || VRAM[16 * GPU_VRAM_WIDTH + 32] != 0x03E0)
            return 0;
        DrawPrim(tile);
        if (VRAM[16 * GPU_VRAM_WIDTH + 32] != 0x001F)
            return 0;
        if (!gpu_present() || fixture_present_count != presented + i + 1)
            return 0;
    }
    fixture_now = 0;
    puts("PASS: every submitted frame fills, rasterizes and presents after long host delays");
    return 1;
}

static sint32 test_frame_present_boundary(void)
{
    PSX_RECT page0 = {0, 0, 64, 32}, page1 = {64, 0, 64, 32};
    DRAWENV draw;
    uint32 presented;
    gpu_reset_graph_state();
    gpu_set_raster(1);
    fixture_now = 0;
    SetDispMask(1);
    ClearImage(&page0, 0, 255, 0);
    ClearImage(&page1, 255, 255, 255);
    gpu_set_display(0, 0, 64, 32);
    if (!gpu_present())
        return 0;
    if (fixture_present_left != 0x00ff00 || fixture_present_right != 0x00ff00)
        return 0;
    presented = fixture_present_count;
    fixture_now += 200000;
    gpu_frame_begin();
    gpu_set_display(64, 0, 64, 32);
    if (!gpu_present() || fixture_present_count != presented)
        return 0;
    SetDefDrawEnv(&draw, 64, 0, 32, 32);
    draw.isbg = 1;
    draw.r0 = 255;
    PutDrawEnv(&draw);
    // A finished first viewport is not a complete split-screen frame
    if (!gpu_present() || fixture_present_count != presented)
        return 0;
    SetDefDrawEnv(&draw, 96, 0, 32, 32);
    draw.isbg = 1;
    draw.b0 = 255;
    PutDrawEnv(&draw);
    if (!gpu_present() || fixture_present_count != presented)
        return 0;
    gpu_frame_end();
    if (!gpu_present() || fixture_present_count != presented + 1)
        return 0;
    if (fixture_present_left != 0xff0000 || fixture_present_right != 0x0000ff)
        return 0;
    gpu_frame_begin();
    gpu_frame_begin();
    gpu_frame_end();
    if (!gpu_present() || fixture_present_count != presented + 1)
        return 0;
    gpu_frame_end();
    if (!gpu_present() || fixture_present_count != presented + 2)
        return 0;
    fixture_now = 0;
    puts("PASS: Partial split viewports suppressed until complete frame, nested frame boundaries");
    return 1;
}

static sint32 test_hd_raster_policy(void)
{
    uint32 gpu_status;
    uint32 tile[] = {0x03FFFFFF, 0x60FF0000, 0x00100020, 0x00020002};
    uint32 mode[] = {0x01FFFFFF, 0xE1000178};
    uint32 uploaded = 0x56784567, copied = 0;
    PSX_RECT texture = {900, 490, 2, 1}, copy = {904, 490, 2, 1};
    GpuRasterState state = {0, 0, 0, {0, 0, 1023, 511}, {0, 0}}, actual;
    sint32 presented = fixture_present_count;
    memset(initial_vram, 0, sizeof(initial_vram));
    if (!gpu_import_raster(&state, initial_vram, sizeof(initial_vram)))
        return 0;
    gpu_set_raster(0);
    DrawPrim(tile);
    DrawPrim(mode);
    gpu_export_raster(&actual);
    if (actual.draw_mode != 0x178 || VRAM[16 * GPU_VRAM_WIDTH + 32] != 0)
        return 0;
    state.draw_mode = 0x178;
    state.draw_flags = 0x600;
    if (!gpu_import_raster(&state, initial_vram, sizeof(initial_vram)))
        return 0;
    gpu_set_polygon_tpage(0xFFFF);
    gpu_export_raster(&actual);
    if (!gpu_port_read(4u, &gpu_status) || actual.draw_mode != 0x1FF || actual.draw_flags != 0x600 || (gpu_status & 0x7FF) != 0x1FF || memcmp(VRAM, initial_vram, sizeof(initial_vram)))
        return 0;
    if (!gpu_present() || fixture_present_count != presented)
        return 0;
    if (LoadImagePSX(&texture, &uploaded) || MoveImage(&texture, copy.x, copy.y) || StoreImage(&copy, &copied) || copied != uploaded)
        return 0;
    gpu_set_raster(1);
    DrawPrim(tile);
    if (VRAM[16 * GPU_VRAM_WIDTH + 32] != 0x7C00)
        return 0;
    if (!gpu_present() || fixture_present_count != presented + 1)
        return 0;
    puts("PASS: HD suppresses software pixels/presentation, preserves draw state and VRAM transfers, restores complete OG frame");
    return 1;
}

static int test_vram_invalidation(void)
{
    PSX_RECT rect = {32, 499, 2, 1};
    uint32 pixels = 0x03e0001f, result = 0;
    uint32 version;
    gpu_set_raster(0);
    gpu_set_transfer_observer(NULL, NULL);
    version = gpu_vram_version();
    LoadImagePSX(&rect, &pixels);
    if (gpu_vram_version() == version)
        return 0;
    version = gpu_vram_version();
    StoreImage(&rect, &result);
    if (gpu_vram_version() == version || result != pixels)
        return 0;
    version = gpu_vram_version();
    MoveImage(&rect, 40, 499);
    if (gpu_vram_version() == version || VRAM[499 * GPU_VRAM_WIDTH + 40] != 31)
        return 0;
    version = gpu_vram_version();
    ClearImage(&rect, 0, 0, 0);
    if (gpu_vram_version() == version)
        return 0;
    memcpy(DRAM + 0x1000, &pixels, sizeof(pixels));
    version = gpu_vram_version();
    if (!gpu_load_rect(0x1000, 50, 499, 2, 1) || gpu_vram_version() == version)
        return 0;
    gpu_set_raster(1);
    version = gpu_vram_version();
    gpu_set_raster(0);
    if (gpu_vram_version() == version)
        return 0;
    gpu_set_raster(1);
    return 1;
}

static sint32 test_gp0_fill(void)
{
    static uint16 expected[GPU_VRAM_WIDTH * GPU_VRAM_HEIGHT];
    const uint32 rectangles[][4] = {{0, 0, 640, 256}, {1, 1, 1, 1}, {15, 511, 17, 2}, {31, 1023, 15, 3}, {1023, 511, 17, 2}, {1008, 510, 1023, 3}, {0, 0, 0, 4}, {0, 0, 16, 0}, {0, 0, 1024, 4}, {0, 0, 16, 512}, {65535, 65535, 65535, 65535}};
    const uint32 colors[] = {0, 0xffffff, 0x87a3d9};
    uint32 index, color_index, interlace, raster, version, row, column;
    uint32 packet[4];
    DISPENV display;
    GpuRasterState state = {0, 0x400, 0, {900, 400, 901, 401}, {300, 200}};
    for (interlace = 0; interlace < 4; ++interlace)
    {
        gpu_reset_graph_state();
        fixture_now = 0;
        gpu_set_transfer_observer(NULL, NULL);
        xport_device_trace_bind(NULL, NULL);
        SetDefDispEnv(&display, 0, interlace == 3 ? 1 : 0, 320, interlace ? 480 : 240);
        display.isinter = interlace != 0;
        PutDispEnv(&display);
        state.draw_flags = interlace == 1 || interlace == 3 ? 0 : 0x400;
        for (raster = 0; raster < 2; ++raster)
            for (index = 0; index < sizeof(rectangles) / sizeof(rectangles[0]); ++index)
                for (color_index = 0; color_index < sizeof(colors) / sizeof(colors[0]); ++color_index)
                {
                    uint32 x = rectangles[index][0] & 0x3f0;
                    uint32 y = rectangles[index][1] & 511;
                    uint32 width = ((rectangles[index][2] & 1023) + 15) & ~15;
                    uint32 height = rectangles[index][3] & 511;
                    uint32 rgb = colors[color_index];
                    uint16 color = (uint16)((rgb & 0xf8) >> 3 | (rgb & 0xf800) >> 6 | (rgb & 0xf80000) >> 9);
                    for (row = 0; row < GPU_VRAM_WIDTH * GPU_VRAM_HEIGHT; ++row)
                        initial_vram[row] = expected[row] = (uint16)(0x8000 | row);
                    if (!gpu_import_raster(&state, initial_vram, sizeof(initial_vram)))
                        return 0;
                    gpu_set_raster((sint32)raster);
                    for (row = 0; row < height; ++row)
                        if ((interlace != 1 && interlace != 3) || ((y + row) & 1) != (interlace == 3))
                            for (column = 0; column < width; ++column)
                                expected[((y + row) % 512) * GPU_VRAM_WIDTH + ((x + column) % 1024)] = color;
                    packet[0] = 0x03000000;
                    packet[1] = 0x02000000 | rgb;
                    packet[2] = rectangles[index][0] | (rectangles[index][1] << 16);
                    packet[3] = rectangles[index][2] | (rectangles[index][3] << 16);
                    version = gpu_vram_version();
                    DrawPrim(packet);
                    if (memcmp(VRAM, expected, sizeof(expected)) || (width && height && gpu_vram_version() == version))
                        return 0;
                }
    }
    gpu_reset_graph_state();
    {
        uint32 *guest_packet = (uint32 *)(DRAM + 0x1000);
        gpu_bind_sync(0, 0, NULL, NULL, NULL, NULL, 0);
        guest_packet[0] = 0x03000000;
        guest_packet[1] = 0x020000ff;
        guest_packet[2] = 0;
        guest_packet[3] = 0x00020010;
        seen = device_events = device_candidates = 0;
        gpu_set_transfer_observer(observe, &seen);
        xport_device_trace_bind(observe_device, &device_events);
        gpu_set_raster(0);
        DrawPrim(guest_packet);
        if (seen != 4 || operations[0] != 1 || payloads[0][0] != 0x04000000 || payloads[1][0] != guest_packet[1] || payloads[2][0] != guest_packet[2] || payloads[3][0] != guest_packet[3] || !device_events || device_candidates || gpu_transfer_observation_failed() || VRAM[0] != 31 || VRAM[1024] != 31)
            return 0;
        gpu_set_transfer_observer(NULL, NULL);
        xport_device_trace_bind(NULL, NULL);
    }
    gpu_set_raster(1);
    return 1;
}

static sint32 test_high_packet_addresses(void)
{
#if defined(AP_X64)
    uint32 *memory = VirtualAlloc((void *)(uintptr_t)0x200000000ull, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!memory || (uintptr_t)memory <= UINT32_MAX)
        return 0;
    uint32 *ot = memory, *packet = memory + 4;
    GpuRasterState state = {0, 0, 0, {0, 0, 1023, 511}, {0, 0}};
    memset(initial_vram, 0, sizeof(initial_vram));
    if (!gpu_register_packet_range(memory, 65536) || !gpu_import_raster(&state, initial_vram, sizeof(initial_vram)))
        return 0;
    gpu_set_transfer_observer(NULL, NULL);
    packet[0] = 0x03ffffff;
    packet[1] = 0x600000ff;
    packet[2] = 0x00100010;
    packet[3] = 0x00020002;
    ClearOTagR(ot, 2);
    AddPrim(ot + 1, packet);
    DrawOTag(ot + 1);
    if (VRAM[16 * GPU_VRAM_WIDTH + 16] != 31)
        return 0;
    packet[1] = 0x6000ff00;
    ClearOTag(ot, 2);
    addPrim(ot, packet);
    DrawOTag(ot);
    if (VRAM[16 * GPU_VRAM_WIDTH + 16] != 0x3e0)
        return 0;
    printf("PASS: Registered OT, AddPrim and setaddr macro at host address %p above 4 GiB\n", memory);
    VirtualFree(memory, 0, MEM_RELEASE);
#endif
    return 1;
}

static sint32 test_draw_load(void)
{
    static DR_LOAD first, second;
    PSX_RECT rectangle = {1020, 511, 16, 1};
    static uint32 ordering;
    uint32 index;
    uint16 first_colors[16], second_colors[16];
    gpu_set_transfer_observer(NULL, NULL);
    if (!gpu_register_packet_range(&first, sizeof(first))
        || !gpu_register_packet_range(&second, sizeof(second))
        || !gpu_register_packet_range(&ordering, sizeof(ordering)))
        return 0;
    memset(&first, 0xA5, sizeof(first));
    memset(&second, 0x5A, sizeof(second));
    for (index = 0; index < 16; ++index)
    {
        first_colors[index] = (uint16)(0x8000 | index * 1031);
        second_colors[index] = (uint16)(0x1000 | index * 67);
    }
    SetDrawLoad(&first, &rectangle);
    if (first.tag != 0x0CA5A5A5 || first.code[0] != 0xA0000000 || first.p[8] != 0x01000000)
        return 0;
    memcpy((uint8 *)&first + 16, first_colors, sizeof(first_colors));
    rectangle.x = 1019;
    SetDrawLoad(&second, &rectangle);
    memcpy((uint8 *)&second + 16, second_colors, sizeof(second_colors));
    for (index = 0; index < 1024; ++index)
        VRAM[511 * GPU_VRAM_WIDTH + index] = 0x5A5A;
    ClearOTagR(&ordering, 1);
    AddPrim(&ordering, &first);
    AddPrim(&ordering, &second);
    for (index = 0; index < 1024; ++index)
        if (VRAM[511 * GPU_VRAM_WIDTH + index] != 0x5A5A)
            return 0;
    DrawOTag(&ordering);
    for (index = 0; index < 16; ++index)
        if (VRAM[511 * GPU_VRAM_WIDTH + ((1020 + index) & 1023)] != first_colors[index])
            return 0;
    if (VRAM[511 * GPU_VRAM_WIDTH + 1019] != second_colors[0] || VRAM[511 * GPU_VRAM_WIDTH + 12] != 0x5A5A)
        return 0;
    return 1;
}

static sint32 test_native_mask_modes(void)
{
    uint32 kind, mode, protected_pixel, stp, index, status;
    uint32 pixel = 11u * GPU_VRAM_WIDTH + 11u;
    uint32 state_packet[] = {0x02000000u, 0xe1000126u, 0xe6000000u};
    GpuRasterState state;
    gpu_set_transfer_observer(NULL, NULL);
    gpu_bind_linked_submit(NULL, NULL);
    for (kind = 0; kind < 5; ++kind)
        for (mode = 0; mode < 4; ++mode)
            for (protected_pixel = 0; protected_pixel < 2; ++protected_pixel)
                for (stp = 0; stp < 2; ++stp)
                {
                    uint32 tile[] = {0x03000000u, 0x600000ffu, 0x000a000au, 0x00080008u};
                    uint32 line[] = {0x03000000u, 0x400000ffu, 0x000b000au, 0x000b0012u};
                    uint32 triangle[] = {0x04000000u, 0x200000ffu, 0x000a000au, 0x000a0012u, 0x0012000au};
                    uint32 sprite[] = {0x04000000u, 0x67000000u, 0x000a000au, 0xffff6464u, 0x00080008u};
                    uint32 quad[] = {0x09000000u, 0x2f000000u, 0x000a000au, 0xffff6464u, 0x000a0012u, 0x0126646cu, 0x0012000au, 0x00006c64u, 0x00120012u, 0x00006c6cu};
                    uint32 *primitives[] = {tile, line, triangle, sprite, quad};
                    uint16 background = (uint16)(0x03e0u | (protected_pixel << 15));
                    uint16 expected = kind >= 3 && stp ? 0x83ffu : 0x001fu;
                    gpu_reset_graph_state();
                    memset(VRAM, 0, sizeof(VRAM));
                    for (index = 0; index < 16; ++index)
                    {
                        uint32 column;
                        for (column = 0; column < 16; ++column)
                            VRAM[(100u + index) * GPU_VRAM_WIDTH + 484u + column] = (uint16)(31u | (stp << 15));
                    }
                    VRAM[pixel] = background;
                    VRAM[400u * GPU_VRAM_WIDTH + 700u] = 0x55aau;
                    state_packet[2] = 0xe6000000u | mode;
                    DrawPrim(state_packet);
                    DrawPrim(primitives[kind]);
                    expected |= (uint16)((mode & 1u) << 15);
                    if ((mode & 2u) && protected_pixel)
                        expected = background;
                    if (VRAM[pixel] != expected || VRAM[400u * GPU_VRAM_WIDTH + 700u] != 0x55aau)
                        return 0;
                }
    for (stp = 0; stp < 2; ++stp)
    {
        uint32 sprite[] = {0x04000000u, 0x67000000u, 0x000a000au, 0xffff6464u, 0x00080008u};
        uint32 row, column;
        gpu_reset_graph_state();
        memset(VRAM, 0, sizeof(VRAM));
        for (row = 100; row < 116; ++row)
            for (column = 484; column < 500; ++column)
                VRAM[row * GPU_VRAM_WIDTH + column] = (uint16)(stp << 15);
        VRAM[pixel] = 0x03e0u;
        state_packet[2] = 0xe6000001u;
        DrawPrim(state_packet);
        DrawPrim(sprite);
        if (VRAM[pixel] != (stp ? 0x83e0u : 0x03e0u))
            return 0;
    }
    gpu_reset_graph_state();
    memset(VRAM, 0, sizeof(VRAM));
    state_packet[2] = 0xe6000001u;
    DrawPrim(state_packet);
    if (!gpu_port_read(4, &status) || ((status >> 11) & 3u) != 1u)
        return 0;
    gpu_export_raster(&state);
    if ((state.draw_flags & 0x1800u) != 0x800u)
        return 0;
    if (!gpu_port_write(0, 0x600000ffu) || !gpu_port_write(0, 0x000a000au) || !gpu_port_write(0, 0x00080008u) || VRAM[pixel] != 0x801fu)
        return 0;
    state.draw_flags = 0x1800u;
    if (!gpu_import_raster(&state, VRAM, sizeof(VRAM)))
        return 0;
    gpu_export_raster(&state);
    if (state.draw_flags != 0x1800u)
        return 0;
    gpu_reset_graph_state();
    return 1;
}

typedef struct GPU_BREAK_CASE
{
    uint32 entered, headers, payloads, error;
    uint32 continuation;
    sint32 provider_result;
    uint32 provider_calls, submitted;
} GPU_BREAK_CASE;

static void count_gpu_completion(void)
{
    ++fixture_gpu_completions;
}

static void observe_gpu_break(void *context, uint32 operation, uint32 address, const uint32 *words, uint32 count)
{
    GPU_BREAK_CASE *test = (GPU_BREAK_CASE *)context;
    (void)words;
    (void)count;
    if (operation == 2u && address == 0x1D0014u)
    {
        ++test->payloads;
        if (BreakDraw() != 0xFFFFFFFFu)
            test->error = 4;
    }
    if (operation == 3u && address == 0x1D0010u)
    {
        ++test->headers;
        if (!test->entered)
        {
            PSX_RECT *rectangle = (PSX_RECT *)psx_addr(0x1D0220u, 8u);
            test->entered = 1;
            if (VRAM[16u * GPU_VRAM_WIDTH + 16u] != 31u)
            {
                test->error = 1;
                return;
            }
            test->continuation = BreakDraw();
            if (test->continuation != 0x1D0010u)
            {
                test->error = 2;
                return;
            }
            rectangle->x = rectangle->y = 16;
            rectangle->w = rectangle->h = 1;
            *(uint32 *)psx_addr(0x1D0200u, 4u) = 0x7C00u;
            if (LoadImagePSX(rectangle, (uint32 *)psx_addr(0x1D0200u, 4u)) < 0 || VRAM[16u * GPU_VRAM_WIDTH + 16u] != 0x7C00u)
            {
                test->error = 3;
                return;
            }
            gpu_resume_linked_draw(test->continuation);
        }
    }
}

static sint32 submit_gpu_break(void *context, uint32 address, uint32 copy_bytes)
{
    GPU_BREAK_CASE *test = (GPU_BREAK_CASE *)context;
    if (copy_bytes)
        return 0;
    ++test->provider_calls;
    test->submitted = address;
    return 1;
}

static sint32 pause_gpu_break(void *context, uint32 *continuation)
{
    GPU_BREAK_CASE *test = (GPU_BREAK_CASE *)context;
    *continuation = test->continuation;
    return test->provider_result;
}

static sint32 test_break_draw(void)
{
    GPU_BREAK_CASE test = {0};
    GpuRasterState state = {0u, 0u, 0x310u, {0u, 0u, 1023u, 511u}, {0, 0}};
    uint32 *first = (uint32 *)psx_addr(0x1D0000u, 16u);
    uint32 *second = (uint32 *)psx_addr(0x1D0010u, 16u);
    memset(initial_vram, 0, sizeof(initial_vram));
    gpu_bind_linked_submit(NULL, NULL);
    gpu_set_transfer_observer(NULL, NULL);
    if (BreakDraw() || !gpu_import_raster(&state, initial_vram, sizeof(initial_vram)))
        return 0;
    first[0] = 0x031D0010u;
    first[1] = 0x600000FFu;
    first[2] = 0x00100010u;
    first[3] = 0x00020002u;
    second[0] = 0x03FFFFFFu;
    second[1] = 0x6000FF00u;
    second[2] = 0x00100010u;
    second[3] = 0x00020002u;
    fixture_gpu_callbacks = 1;
    fixture_gpu_completions = 0;
    if (DrawSyncCallback(count_gpu_completion) != NULL)
        return 0;
    gpu_set_transfer_observer(observe_gpu_break, &test);
    DrawOTag(first);
    if (test.error || !test.entered || test.headers != 2u || test.payloads != 1u || fixture_gpu_completions != 1u || VRAM[16u * GPU_VRAM_WIDTH + 16u] != 0x03E0u || gpu_transfer_observation_failed() || BreakDraw())
        return 0;
    DrawSyncCallback(NULL);
    fixture_gpu_callbacks = 0;
    gpu_set_transfer_observer(NULL, NULL);
    gpu_bind_linked_submit(submit_gpu_break, &test);
    if (BreakDraw() != 0xFFFFFFFFu || !gpu_linked_submit_failed())
        return 0;
    gpu_bind_linked_submit(submit_gpu_break, &test);
    gpu_bind_linked_pause(pause_gpu_break);
    test.provider_result = 0;
    if (BreakDraw() || gpu_linked_submit_failed())
        return 0;
    test.provider_result = -1;
    if (BreakDraw() != 0xFFFFFFFFu || gpu_linked_submit_failed())
        return 0;
    test.provider_result = 1;
    test.continuation = 0xFFFFFFu;
    if (BreakDraw() || gpu_linked_submit_failed())
        return 0;
    test.continuation = 0x1D0010u;
    if (BreakDraw() != test.continuation)
        return 0;
    gpu_resume_linked_draw(test.continuation);
    if (test.provider_calls != 1u || test.submitted != test.continuation || gpu_linked_submit_failed())
        return 0;
    gpu_bind_linked_submit(submit_gpu_break, &test);
    if (BreakDraw() != 0xFFFFFFFFu || !gpu_linked_submit_failed())
        return 0;
    gpu_bind_linked_submit(NULL, NULL);
    gpu_reset_graph_state();
    return BreakDraw() == 0;
}

int main(void)
{
    GpuPsyqStateBinding binding = {0x13000u, 0x13100u, 0x13200u, 0x13201u, 0x13204u, 0x13300u};
    DISPENV display;
    DISPENV *guest_display = psx_addr(0x14000u, sizeof(DISPENV));
    DRAWENV *guest_draw = psx_addr(0x14100u, sizeof(DRAWENV));
    uint32 *root = psx_addr(0x10000u, 4);
    uint32 *terminal = psx_addr(0x10018u, 16);
    if (!test_break_draw())
        return 64;
    xport_device_trace_bind(observe_device, &device_events);
    memset(guest_display, 0, sizeof(*guest_display));
    memset(guest_draw, 0, sizeof(*guest_draw));
    SetDefDispEnv(guest_display, 3, 4, 368, 512);
    SetDefDrawEnv(guest_draw, 5, 6, 320, 256);
    if (guest_display->disp.x != 3 || guest_display->disp.y != 4 || guest_display->disp.w != 368 || guest_display->disp.h != 512 || guest_draw->clip.x != 5 || guest_draw->clip.y != 6 || guest_draw->clip.w != 320 || guest_draw->clip.h != 256)
        return 33;
    if (!test_raster_import())
        return 13;
    if (!test_sync())
        return 23;
    if (!test_timeout())
        return 28;
    if (!test_queue())
        return 24;
    if (!test_queue_pump())
        return 25;
    if (!test_queue_submit())
        return 26;
    *root = 0x10018u;
    terminal[0] = 0x03ffffffu;
    terminal[1] = 0x40000000u;
    terminal[2] = terminal[3] = 0;
    gpu_set_transfer_observer(observe, &seen);
    DrawOTag(root);
    if (seen != 4 || gpu_transfer_observation_failed() || operations[0] != 1 || addresses[0] != 4 || payloads[0][0] != 0x04000002u || operations[1] != 3 || operations[2] != 3 || operations[3] != 2 || addresses[3] != 0x1001cu || sizes[3] != 3 || payloads[3][0] != 0x40000000u)
        return 1;
    if (!device_events || device_fetches < 2u || !device_candidates)
        return 29;
    *root = 0x10001u;
    DrawOTag(root);
    if (!gpu_transfer_observation_failed())
        return 2;
    gpu_set_transfer_observer(NULL, NULL);
    if (gpu_transfer_observation_failed())
        return 3;
    *root = 0x10018u;
    DrawOTag(root);
    if (seen != 6)
        return 4;
    gpu_set_transfer_observer(observe, &seen);
    gpu_set_def_draw_env(0x12000u, 0, 0, 320, 240);
    gpu_put_draw_env(0x12000u, 0, 0);
    if (seen != 9 || gpu_transfer_observation_failed() || operations[6] != 1 || operations[7] != 3 || addresses[7] != 0x1201cu || payloads[7][0] != 0x06ffffffu || operations[8] != 2 || addresses[8] != 0x12020u || sizes[8] != 6 || payloads[8][0] != 0xe3000000u || payloads[8][3] != 0xe100060au || payloads[8][5] != 0xe6000000u)
        return 5;
    if (!gpu_bind_psyq_state(&binding))
        return 6;
    *(uint32 *)psx_addr(binding.video_mode, 4u) = 1u;
    memset(&display, 0, sizeof(display));
    display.disp.w = 512;
    display.disp.h = 256;
    display.screen.x = 2;
    display.screen.y = 20;
    display.screen.h = 256;
    PutDispEnv(&display);
    if (seen != 13 || payloads[9][0] != 0x05000000u || payloads[10][0] != (0x06000000u | 628u | (3188u << 12)) || payloads[11][0] != (0x07000000u | 39u | (295u << 10)) || payloads[12][0] != 0x0800000au || display.pad0 != 1u || memcmp(psx_addr(binding.display_cache, 20u), &display, 20u) || ((uint8 *)psx_addr(binding.gp1_shadow, 256u))[8] != 10u)
        return 7;
    PutDispEnv(&display);
    if (seen != 14 || payloads[13][0] != 0x05000000u)
        return 8;
    display.screen.x = display.screen.y = -100;
    display.screen.w = display.screen.h = 1;
    PutDispEnv(&display);
    if (seen != 17 || payloads[15][0] != (0x06000000u | 500u | (580u << 12)) || payloads[16][0] != (0x07000000u | 16u | (18u << 10)))
        return 9;
    gpu_bind_psyq_state(NULL);
    PutDispEnv(&display);
    if (!gpu_transfer_observation_failed() || seen != 17)
        return 10;
    gpu_set_transfer_observer(observe, &seen);
    ClearImage(NULL, 0, 0, 0);
    if (!gpu_transfer_observation_failed())
        return 11;
    gpu_set_transfer_observer(observe, &seen);
    SetDispMask(1);
    if (!gpu_transfer_observation_failed())
        return 12;
    gpu_bind_psyq_state(&binding);
    gpu_set_transfer_observer(observe, &seen);
    seen = 0;
    memset(psx_addr(binding.display_cache - 1u, 22u), 0x5a, 22u);
    SetDispMask(0);
    if (seen != 1 || operations[0] != 1 || addresses[0] != 4 || sizes[0] != 1 || payloads[0][0] != 0x03000001u || gpu_transfer_observation_failed() || DRAM[binding.gp1_shadow + 3u] != 1u || DRAM[binding.display_cache - 1u] != 0x5a || DRAM[binding.display_cache + 20u] != 0x5a)
        return 14;
    {
        uint32 i;
        for (i = 0; i < 20u; ++i)
            if (DRAM[binding.display_cache + i] != 0xff)
                return 15;
    }
    memset(psx_addr(binding.display_cache, 20u), 0x5a, 20u);
    SetDispMask(-1);
    SetDispMask(1);
    if (seen != 3 || payloads[1][0] != 0x03000000u || payloads[2][0] != 0x03000000u || DRAM[binding.gp1_shadow + 3u] != 0u || gpu_transfer_observation_failed())
        return 16;
    {
        uint32 i;
        for (i = 0; i < 20u; ++i)
            if (DRAM[binding.display_cache + i] != 0x5a)
                return 17;
    }
    seen = 0;
    DRAM[binding.gp1_shadow + 4u] = 0x5a;
    terminal[0] = 0x03000000u;
    terminal[1] = 0xe1000000u;
    terminal[2] = 0xe2000000u;
    terminal[3] = 0xe6000000u;
    DrawPrim(terminal);
    if (seen != 4 || operations[0] != 1 || addresses[0] != 4 || payloads[0][0] != 0x04000000u || gpu_transfer_observation_failed() || DRAM[binding.gp1_shadow + 4u] != 0x5a)
        return 18;
    {
        uint32 i;
        for (i = 1; i < 4u; ++i)
            if (operations[i] != 0 || addresses[i] != 0 || sizes[i] != 1 || payloads[i][0] != terminal[i])
                return 19;
    }
    *root = 0u;
    DrawPrim(root);
    if (seen != 5 || payloads[4][0] != 0x04000000u)
        return 20;
    *(uint32 *)(DRAM + PSX_DRAM_SIZE - 4u) = 0x01000000u;
    DrawPrim(DRAM + PSX_DRAM_SIZE - 4u);
    if (!gpu_transfer_observation_failed() || seen != 5)
        return 21;
    gpu_set_transfer_observer(observe, &seen);
    DrawPrim(&display);
    if (!gpu_transfer_observation_failed() || seen != 5)
        return 22;
    gpu_set_transfer_observer(observe, &seen);
    if (!test_linked_routing())
        return 27;
    gpu_set_transfer_observer(observe, &seen);
    if (!test_dma_slice())
        return 29;
    {
        PSX_RECT area = {688, 317, 16, 1};
        uint32 pixels[16] = {0};
        uint32 i;
        seen = 0;
        gpu_set_transfer_observer(observe, &seen);
        for (i = 0; i < 16u; ++i)
            VRAM[317u * 1024u + 688u + i] = (uint16)(0x8000u + i);
        if (StoreImage(&area, pixels) || LoadImagePSX(&area, pixels) || seen != 18u || gpu_transfer_observation_failed())
            return 30;
        if (operations[0] != 1u || payloads[0][0] != 0x04000000u || payloads[1][0] != 0x01000000u || payloads[2][0] != 0xc0000000u || payloads[3][0] != 0x013d02b0u || payloads[4][0] != 0x00010010u || payloads[5][0] != 0x04000000u || payloads[6][0] != 0x01000000u || payloads[7][0] != 0xa0000000u)
            return 31;
        for (i = 0u; i < 8u; ++i)
            if (operations[10u + i] != 0u || sizes[10u + i] != 1u || payloads[10u + i][0] != pixels[i])
                return 32;
        area.w = 32;
        LoadImagePSX(&area, pixels);
        if (!gpu_transfer_observation_failed() || seen != 18u)
            return 33;
    }
    {
        PSX_RECT area = {902, 360, 7, 20};
        uint32 *pixels = (uint32 *)(DRAM + 0x8d240u);
        uint32 i, block;
        for (i = 0u; i < 70u; ++i)
            pixels[i] = 0x12340000u + i;
        seen = 0u;
        gpu_set_transfer_observer(observe, &seen);
        if (LoadImagePSX(&area, pixels) || gpu_transfer_observation_failed() || seen != 16u || payloads[3][0] != 0x01680386u || payloads[4][0] != 0x00140007u)
            return 34;
        for (i = 0u; i < 6u; ++i)
            if (operations[5u + i] != 0u || sizes[5u + i] != 1u || payloads[5u + i][0] != pixels[i])
                return 35;
        if (operations[11] != 1u || payloads[11][0] != 0x04000002u)
            return 36;
        for (block = 0u; block < 4u; ++block)
        {
            if (operations[12u + block] != 2u || sizes[12u + block] != 16u || addresses[12u + block] != 0x8d258u + block * 64u || memcmp(payloads[12u + block], pixels + 6u + block * 16u, 64u))
                return 37;
        }
        seen = 0u;
        area.w = 32;
        area.h = 1;
        if (LoadImagePSX(&area, pixels) || gpu_transfer_observation_failed() || seen != 7u || operations[6] != 2u || sizes[6] != 16u || addresses[6] != 0x8d240u)
            return 38;
        seen = 0u;
        area.w = 1;
        if (LoadImagePSX(&area, pixels) || gpu_transfer_observation_failed() || seen != 6u || payloads[5][0] != pixels[0])
            return 39;
    }
    {
        PSX_RECT area = {64, 20, 64, 2};
        GpuRasterState state;
        uint16 *limits = (uint16 *)(DRAM + 0x18000);
        limits[0] = 1024;
        limits[1] = 512;
        if (gpu_bind_clear_image(0x1f801810u, 0x18000u) || !gpu_bind_clear_image(0x80018100u, 0x80018000u))
            return 40;
        gpu_export_raster(&state);
        seen = 0u;
        gpu_set_transfer_observer(observe, &seen);
        if (ClearImage(&area, 248, 16, 8) || gpu_transfer_observation_failed() || seen != 3u || operations[1] != 3u || addresses[1] != 0x18100u || payloads[1][0] != 0x05ffffffu || operations[2] != 2u || sizes[2] != 5u || payloads[2][0] != 0xe6000000u || payloads[2][1] != 0xe100060au || state.draw_mode != 0x0au || payloads[2][2] != 0x020810f8u || payloads[2][3] != 0x00140040u || payloads[2][4] != 0x00020040u || VRAM[20 * GPU_VRAM_WIDTH + 64] != 0x045fu)
            return 41;
        seen = 0u;
        area.x = 65;
        if (ClearImage(&area, 0, 0, 0) || seen != 8u || payloads[0][0] != 0x10000003u || payloads[2][0] != 0x10000005u || payloads[4][0] != 0x08018124u || sizes[5] != 8u || payloads[6][0] != 0x03ffffffu || sizes[7] != 3u || payloads[7][0] != (0xe3000000u | state.area[0] | (state.area[1] << 10)))
            return 42;
        seen = 0u;
        area.w = -1;
        area.h = 1024;
        if (ClearImage(&area, 0, 0, 0) || area.w != 0 || area.h != 511 || gpu_transfer_observation_failed())
            return 43;
    }
    {
        GpuRasterState before, after;
        uint32 *head = (uint32 *)(DRAM + 0x18200), *tail = head + 1;
        if (gpu_bind_soft_reset(0x18200u, 0x18200u) || gpu_bind_soft_reset(0x1f801810u, 0x18204u) || !gpu_bind_soft_reset(0x80018200u, 0xa0018204u))
            return 44;
        *head = 4u;
        *tail = 3u;
        gpu_export_raster(&before);
        seen = 0u;
        gpu_set_transfer_observer(observe, &seen);
        if (ResetGraph(1) || gpu_transfer_observation_failed() || seen != 2u || *head || *tail || operations[0] != 1u || addresses[0] != 4u || payloads[0][0] != 0x02000000u || operations[1] != 1u || payloads[1][0] != 0x01000000u)
            return 45;
        gpu_export_raster(&after);
        if (memcmp(&before, &after, sizeof(before)))
            return 46;
    }
    {
        GpuRasterState state = {0u, 0u, 0u, {0u, 0u, 1023u, 511u}, {0, 0}};
        uint32 *packet = (uint32 *)(DRAM + 0x18300u);
        memset(initial_vram, 0, sizeof(initial_vram));
        if (!gpu_import_raster(&state, initial_vram, sizeof(initial_vram)))
            return 47;
        gpu_set_transfer_observer(NULL, NULL);
        packet[0] = 0x04ffffffu;
        packet[1] = 0x20ffffffu;
        packet[2] = 0x0000fe0cu;
        packet[3] = 0x00000258u;
        packet[4] = 0x0064fe0cu;
        DrawPrim(packet);
        if (VRAM[20u * GPU_VRAM_WIDTH] != 0u)
            return 48;
        packet[3] = 0x000001f3u;
        DrawPrim(packet);
        if (VRAM[20u * GPU_VRAM_WIDTH] == 0u)
            return 49;
    }
    {
        static uint32 native_packets[8];
        static uint32 native_ot[2];
        uint32 *guest_ot = (uint32 *)(DRAM + 0x19000u);
        uint32 *guest_packet = (uint32 *)(DRAM + 0x19010u);
        GpuRasterState state = {0u, 0u, 0u, {0u, 0u, 1023u, 511u}, {0, 0}};
        static uint32 invalid[1] = {0x03ffffffu};
        memset(initial_vram, 0, sizeof(initial_vram));
        if (!gpu_register_packet_range(native_packets, sizeof(native_packets)) || !gpu_register_packet_range(native_packets, sizeof(native_packets)) || !gpu_register_packet_range(native_ot, sizeof(native_ot)) || gpu_register_packet_range(native_packets + 1, 4u) || gpu_register_packet_range((uint8 *)native_packets + 1, 4u) || gpu_register_packet_range(native_packets, 3u) || gpu_register_packet_range(DRAM, 4u) || gpu_register_packet_range(NULL, 4u) || !gpu_register_packet_range(invalid, sizeof(invalid)))
            return 50;
        native_packets[0] = 0x03ffffffu;
        native_packets[1] = 0x600000ffu;
        native_packets[2] = 0x00100010u;
        native_packets[3] = 0x00020002u;
        native_packets[4] = 0x03ffffffu;
        native_packets[5] = 0x6000ff00u;
        native_packets[6] = 0x00100010u;
        native_packets[7] = 0x00020002u;
        guest_packet[0] = 0x03ffffffu;
        guest_packet[1] = 0x60ff0000u;
        guest_packet[2] = 0x00100010u;
        guest_packet[3] = 0x00020002u;
        gpu_set_transfer_observer(NULL, NULL);
        if (!gpu_import_raster(&state, initial_vram, sizeof(initial_vram)))
            return 51;
        ClearOTagR(guest_ot, 1);
        AddPrim(guest_ot, native_packets);
        if ((*guest_ot & 0xffffffu) != 0x200000u)
            return 52;
        AddPrim(guest_ot, guest_packet);
        DrawOTag(guest_ot);
        if (VRAM[16u * GPU_VRAM_WIDTH + 16u] != 31u)
            return 53;
        if (!gpu_import_raster(&state, initial_vram, sizeof(initial_vram)))
            return 54;
        ClearOTagR(native_ot, 2);
        AddPrim(native_ot, guest_packet);
        AddPrim(native_ot + 1, native_packets + 4);
        DrawOTag(native_ot + 1);
        if (VRAM[16u * GPU_VRAM_WIDTH + 16u] != 0x7c00u)
            return 55;
        if (!gpu_import_raster(&state, initial_vram, sizeof(initial_vram)))
            return 56;
        ClearOTag(guest_ot, 1);
        AddPrim(guest_ot, invalid);
        DrawOTag(guest_ot);
        if (memcmp(VRAM, initial_vram, sizeof(initial_vram)))
            return 57;
        native_packets[0] = (native_packets[0] & 0xff000000u) | 0x200010u;
        ClearOTag(guest_ot, 1);
        AddPrims(guest_ot, native_packets, native_packets + 4);
        DrawOTag(guest_ot);
        if (VRAM[16u * GPU_VRAM_WIDTH + 16u] != 0x03e0u)
            return 58;
    }
    {
        uint32 compound[] = {0x09FFFFFFu, 0x600000FFu, 0x00100010u, 0x00020002u, 0u, 0xE2000000u, 0u, 0x6000FF00u, 0x00100020u, 0x00020002u};
        GpuRasterState state = {0u, 0u, 0x310u, {0u, 0u, 1023u, 511u}, {0, 0}}, actual;
        memset(initial_vram, 0, sizeof(initial_vram));
        gpu_set_transfer_observer(NULL, NULL);
        if (!gpu_import_raster(&state, initial_vram, sizeof(initial_vram)))
            return 61;
        DrawPrim(compound);
        gpu_export_raster(&actual);
        if (actual.texture_window != 0u || VRAM[16u * GPU_VRAM_WIDTH + 16u] != 31u || VRAM[16u * GPU_VRAM_WIDTH + 32u] != 0x03E0u)
            return 62;
    }
    if (!test_native_draw_environment())
        return 60;
    if (!test_line_size_limits())
        return 71;
    if (!test_gouraud_line())
        return 68;
    if (!test_flat_polylines())
        return 59;
    if (!test_full_frame_render())
        return 63;
    if (!test_frame_present_boundary())
        return 66;
    if (!test_hd_raster_policy())
        return 64;
    if (!test_vram_invalidation())
        return 65;
    if (!test_gp0_fill())
        return 67;
    if (!test_high_packet_addresses())
        return 69;
    if (!test_draw_load())
        return 70;
    if (!test_native_mask_modes())
        return 72;
    puts("PASS: native E6 mask force/check, source STP blending, primitive paths, port and reset");
    puts("PASS: SetDrawLoad native packet construction, deferred OT execution, upload order and VRAM wrapping");
    puts("PASS: GP0(02) RGB555, alignment, rounding, zero dimensions, XY wrap, full VRAM, disabled raster, clipping/mask independence, interlace and draw-to-display override");
    puts("PASS: VRAM invalidation; DMA/OT, queued SDK routing, draw environment, PAL GP1 display/cache/shadow, display mask, direct GP0, clamping, image upload remainder/blocks, ClearImage packets, synchronous soft reset, polygon size limit, unbound failure, stable registered native links/mixed guest chains/range validation, bent flat polylines/terminators/bounds/clipping/offset/blending, native DRAWENV/full packet/cache/raster/VRAM");
    return 0;
}

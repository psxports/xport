#define XPORT_MEMORY_IMPLEMENTATION
#include "xport_trace.h"
#include "psx.h"

/* Memory access */
#include <string.h>

void xport_function_marker(const char *image, uint32 address, const char *function, const char *file, uint32 line, uintptr_t host_return_address, uint32 channels)
{
#if defined(LOCKSTEP_DEBUG)
    if (channels & 2u)
        xport_lockstep_function_marker(image, address, function, file, line, host_return_address);
#endif
    if (channels & 1u)
        xport_audit_emit(XPORT_AUDIT_MARKER, image, address, "function", function, file, (int)line);
}

typedef struct XportMemoryContext
{
    uint8 *dram;
    size_t dram_size;
    uint8 *scratchpad;
    size_t scratchpad_size;
    const uint8 *bios;
    size_t bios_size;
    uint32 mmio_first;
    uint32 mmio_size;
    XportMemoryMmioRead mmio_read;
    XportMemoryMmioWrite mmio_write;
    void *mmio_user;
    XportMemoryObserver observer;
    void *observer_user;
    XportMemoryError error;
} XportMemoryContext;

typedef struct XportMappedRange
{
    uint8 *pointer;
    XportMemoryRegion region;
    uint32 offset;
    sint32 readonly;
} XportMappedRange;

static XportMemoryContext memory_context;
static sint32 xport_host_range(const XportMemoryContext *context, const void *pointer, size_t width, XportMemoryRegion *region, uint32 *offset, sint32 *readonly);

static uint32 xport_physical(uint32 address)
{
    return address & 0x1fffffffu;
}

static sint32 xport_segment_valid(uint32 address)
{
    uint32 segment = address & 0xe0000000u;
    return segment == 0u || segment == 0x80000000u || segment == 0xa0000000u;
}

static void xport_set_error(XportMemoryContext *context, XportMemoryError error)
{
    if (context)
        context->error = error;
}

static sint32 xport_map(XportMemoryContext *context, uint32 address, size_t length, sint32 writing, XportMappedRange *mapped)
{
    uint32 physical;
    if (!context || !mapped || !length || !xport_segment_valid(address))
    {
        xport_set_error(context, XPORT_MEMORY_ERROR_ARGUMENT);
        return 0;
    }
    physical = xport_physical(address);
    if (physical < 0x00800000u && context->dram && context->dram_size)
    {
        uint32 offset = physical % (uint32)context->dram_size;
        if (length <= context->dram_size - offset)
        {
            mapped->pointer = context->dram == (uint8 *)DRAM && context->dram_size == sizeof(DRAM) && (physical < PSX_DRAM_SIZE || (address & 0xe0000000u) != 0u) ? (uint8 *)psx_addr(address, length) : context->dram + offset;
            mapped->region = XPORT_MEMORY_DRAM;
            mapped->offset = offset;
            mapped->readonly = 0;
            return 1;
        }
    }
    if (physical >= 0x1f800000u && context->scratchpad)
    {
        uint32 offset = physical - 0x1f800000u;
        if (offset <= context->scratchpad_size && length <= context->scratchpad_size - offset)
        {
            mapped->pointer = context->scratchpad == (uint8 *)SCRATCHPAD && context->scratchpad_size == sizeof(SCRATCHPAD) ? (uint8 *)psx_addr(address, length) : context->scratchpad + offset;
            mapped->region = XPORT_MEMORY_SCRATCHPAD;
            mapped->offset = offset;
            mapped->readonly = 0;
            return 1;
        }
    }
    if (physical >= 0x1fc00000u && context->bios)
    {
        uint32 offset = physical - 0x1fc00000u;
        if (offset <= context->bios_size && length <= context->bios_size - offset)
        {
            if (writing)
            {
                xport_set_error(context, XPORT_MEMORY_ERROR_READONLY);
                return 0;
            }
            mapped->pointer = (uint8 *)context->bios + offset;
            mapped->region = XPORT_MEMORY_BIOS;
            mapped->offset = offset;
            mapped->readonly = 1;
            return 1;
        }
    }
    xport_set_error(context, XPORT_MEMORY_ERROR_RANGE);
    return 0;
}

static sint32 xport_mmio_contains(const XportMemoryContext *context, uint32 address, uint32 width)
{
    uint32 physical = xport_physical(address);
    return context && context->mmio_size && physical >= context->mmio_first && width <= context->mmio_size && physical - context->mmio_first <= context->mmio_size - width;
}

static void xport_observe(XportMemoryContext *context, XportMemoryOperation operation, XportMemoryRegion region, uint32 offset, size_t width, uint32 before, uint32 after, uint32 byte_mask)
{
    if (context && context->observer)
        context->observer(context->observer_user, operation, region, offset, width, before, after, byte_mask);
}

static uint32 xport_bytes_load(const uint8 *bytes, uint32 width)
{
    uint32 value = bytes[0];
    if (width > 1u)
        value |= (uint32)bytes[1] << 8;
    if (width > 2u)
        value |= (uint32)bytes[2] << 16 | (uint32)bytes[3] << 24;
    return value;
}

static void xport_bytes_store(uint8 *bytes, uint32 width, uint32 value)
{
    bytes[0] = (uint8)value;
    if (width > 1u)
        bytes[1] = (uint8)(value >> 8);
    if (width > 2u)
    {
        bytes[2] = (uint8)(value >> 16);
        bytes[3] = (uint8)(value >> 24);
    }
}

uint32 xport_memory_read(uint32 address, uint32 width)
{
    XportMappedRange mapped;
    uint32 value = 0;
    if (xport_mmio_contains(&memory_context, address, width))
    {
        if (!memory_context.mmio_read || !memory_context.mmio_read(memory_context.mmio_user, xport_physical(address), width, &value))
        {
            xport_set_error(&memory_context, XPORT_MEMORY_ERROR_UNBOUND_MMIO);
            return 0;
        }
        xport_observe(&memory_context, XPORT_MEMORY_READ, XPORT_MEMORY_MMIO, xport_physical(address) - memory_context.mmio_first, width, value, value, (1u << width) - 1u);
        return value;
    }
    if (!xport_map(&memory_context, address, width, 0, &mapped))
        return 0;
    value = xport_bytes_load(mapped.pointer, width);
    xport_observe(&memory_context, XPORT_MEMORY_READ, mapped.region, mapped.offset, width, value, value, (1u << width) - 1u);
    return value;
}

sint32 xport_memory_write(uint32 address, uint32 width, uint32 value, uint32 byte_mask)
{
    XportMappedRange mapped;
    uint32 before;
    uint32 index;
    byte_mask &= (1u << width) - 1u;
    if (!byte_mask)
    {
        xport_set_error(&memory_context, XPORT_MEMORY_ERROR_ARGUMENT);
        return 0;
    }
    if (xport_mmio_contains(&memory_context, address, width))
    {
        if (!memory_context.mmio_write || !memory_context.mmio_write(memory_context.mmio_user, xport_physical(address), width, value, byte_mask))
        {
            xport_set_error(&memory_context, XPORT_MEMORY_ERROR_UNBOUND_MMIO);
            return 0;
        }
        xport_observe(&memory_context, XPORT_MEMORY_WRITE, XPORT_MEMORY_MMIO, xport_physical(address) - memory_context.mmio_first, width, 0u, value, byte_mask);
        return 1;
    }
    if (!xport_map(&memory_context, address, width, 1, &mapped))
        return 0;
    before = xport_bytes_load(mapped.pointer, width);
#if defined(LOCKSTEP_DEBUG) && defined(_M_IX86)
    if (width == 4u && byte_mask == 15u && ((uintptr_t)mapped.pointer & 3u) == 0u)
        *(volatile uint32 *)mapped.pointer = value;
    else
#endif
        for (index = 0; index < width; ++index)
            if (byte_mask & (1u << index))
                mapped.pointer[index] = (uint8)(value >> (index * 8u));
    xport_observe(&memory_context, XPORT_MEMORY_WRITE, mapped.region, mapped.offset, width, before, xport_bytes_load(mapped.pointer, width), byte_mask);
    return 1;
}

void xport_memory_init(uint8 *dram, size_t dram_size, uint8 *scratchpad, size_t scratchpad_size, const uint8 *bios, size_t bios_size)
{
    memset(&memory_context, 0, sizeof(memory_context));
    memory_context.dram = dram;
    memory_context.dram_size = dram_size;
    memory_context.scratchpad = scratchpad;
    memory_context.scratchpad_size = scratchpad_size;
    memory_context.bios = bios;
    memory_context.bios_size = bios_size;
}

void xport_memory_bind_mmio(uint32 first, uint32 size, XportMemoryMmioRead read, XportMemoryMmioWrite write, void *user)
{
    memory_context.mmio_first = first;
    memory_context.mmio_size = size;
    memory_context.mmio_read = read;
    memory_context.mmio_write = write;
    memory_context.mmio_user = user;
}

void xport_memory_bind_bios(const uint8 *bios, size_t bios_size)
{
    memory_context.bios = bios;
    memory_context.bios_size = bios_size;
}

void xport_memory_bind_observer(XportMemoryObserver observer, void *user)
{
    memory_context.observer = observer;
    memory_context.observer_user = user;
}

XportMemoryError xport_memory_error()
{
    return memory_context.error;
}

void xport_memory_clear_error()
{
    memory_context.error = XPORT_MEMORY_ERROR_NONE;
}

sint32 xport_memory_pointer_identity(const void *pointer, size_t length, XportMemoryRegion *region, uint32 *offset)
{
    sint32 readonly;
    if (!pointer || !region || !offset || !length)
        return 0;
    return xport_host_range(&memory_context, pointer, length, region, offset, &readonly);
}

void *xport_guest_ptr(uint32 address, size_t length)
{
    XportMemoryContext *context = &memory_context;
    XportMappedRange mapped;
    return xport_map(context, address, length, 1, &mapped) ? mapped.pointer : NULL;
}

const void *xport_guest_cptr(uint32 address, size_t length)
{
    XportMemoryContext *context = &memory_context;
    XportMappedRange mapped;
    return xport_map(context, address, length, 0, &mapped) ? mapped.pointer : NULL;
}

static sint32 xport_update_value(uint32 address, uint32 width, XportMemoryUpdate operation, uint32 operand)
{
    XportMemoryContext *context = &memory_context;
    XportMemoryError prior_error;
    uint32 value;
    prior_error = context->error;
    context->error = XPORT_MEMORY_ERROR_NONE;
    value = xport_memory_read(address, width);
    if (context->error != XPORT_MEMORY_ERROR_NONE)
        return 0;
    context->error = prior_error;
    switch (operation)
    {
        case XPORT_MEMORY_UPDATE_ADD:
            value += operand;
            break;
        case XPORT_MEMORY_UPDATE_SUBTRACT:
            value -= operand;
            break;
        case XPORT_MEMORY_UPDATE_MULTIPLY:
            value *= operand;
            break;
        case XPORT_MEMORY_UPDATE_DIVIDE:
            if (!operand)
            {
                xport_set_error(context, XPORT_MEMORY_ERROR_ARGUMENT);
                return 0;
            }
            value /= operand;
            break;
        case XPORT_MEMORY_UPDATE_REMAINDER:
            if (!operand)
            {
                xport_set_error(context, XPORT_MEMORY_ERROR_ARGUMENT);
                return 0;
            }
            value %= operand;
            break;
        case XPORT_MEMORY_UPDATE_AND:
            value &= operand;
            break;
        case XPORT_MEMORY_UPDATE_OR:
            value |= operand;
            break;
        case XPORT_MEMORY_UPDATE_XOR:
            value ^= operand;
            break;
        case XPORT_MEMORY_UPDATE_SHIFT_LEFT:
            if (operand >= 32u)
            {
                xport_set_error(context, XPORT_MEMORY_ERROR_ARGUMENT);
                return 0;
            }
            value <<= operand;
            break;
        case XPORT_MEMORY_UPDATE_SHIFT_RIGHT:
            if (operand >= 32u)
            {
                xport_set_error(context, XPORT_MEMORY_ERROR_ARGUMENT);
                return 0;
            }
            value >>= operand;
            break;
        default:
            xport_set_error(context, XPORT_MEMORY_ERROR_ARGUMENT);
            return 0;
    }
    return xport_memory_write(address, width, value, (1u << width) - 1u);
}

sint32 xport_update_u8(uint32 address, XportMemoryUpdate operation, uint32 value)
{
    return xport_update_value(address, 1u, operation, value);
}

sint32 xport_update_u16(uint32 address, XportMemoryUpdate operation, uint32 value)
{
    return xport_update_value(address, 2u, operation, value);
}

sint32 xport_update_u32(uint32 address, XportMemoryUpdate operation, uint32 value)
{
    return xport_update_value(address, 4u, operation, value);
}

static sint32 xport_host_range(const XportMemoryContext *context, const void *pointer, size_t width, XportMemoryRegion *region, uint32 *offset, sint32 *readonly)
{
    uintptr_t value = (uintptr_t)pointer;
    const uint8 *bases[3] = {context ? context->dram : NULL, context ? context->scratchpad : NULL, context ? context->bios : NULL};
    size_t sizes[3] = {context ? context->dram_size : 0u, context ? context->scratchpad_size : 0u, context ? context->bios_size : 0u};
    uint32 index;
    for (index = 0; index < 3u; ++index)
    {
        uintptr_t base = (uintptr_t)bases[index];
        if (bases[index] && value >= base && value - base <= sizes[index] && width <= sizes[index] - (value - base))
        {
            *region = (XportMemoryRegion)(index + 1u);
            *offset = (uint32)(value - base);
            *readonly = index == 2u;
            return 1;
        }
    }
    return 0;
}

uint8 xport_load_u8(const void *pointer)
{
    XportMemoryContext *context = &memory_context;
    XportMemoryRegion region;
    uint32 offset;
    sint32 readonly;
    uint8 value;
    if (!pointer)
    {
        xport_set_error(context, XPORT_MEMORY_ERROR_ARGUMENT);
        return 0;
    }
    value = *(const uint8 *)pointer;
    if (xport_host_range(context, pointer, 1u, &region, &offset, &readonly))
        xport_observe(context, XPORT_MEMORY_READ, region, offset, 1u, value, value, 1u);
    return value;
}

uint16 xport_load_le16(const void *pointer)
{
    XportMemoryContext *context = &memory_context;
    XportMemoryRegion region;
    uint32 offset;
    sint32 readonly;
    uint16 value;
    if (!pointer)
    {
        xport_set_error(context, XPORT_MEMORY_ERROR_ARGUMENT);
        return 0;
    }
    value = (uint16)xport_bytes_load((const uint8 *)pointer, 2u);
    if (xport_host_range(context, pointer, 2u, &region, &offset, &readonly))
        xport_observe(context, XPORT_MEMORY_READ, region, offset, 2u, value, value, 3u);
    return value;
}

uint32 xport_load_le32(const void *pointer)
{
    XportMemoryContext *context = &memory_context;
    XportMemoryRegion region;
    uint32 offset;
    sint32 readonly;
    uint32 value;
    if (!pointer)
    {
        xport_set_error(context, XPORT_MEMORY_ERROR_ARGUMENT);
        return 0;
    }
    value = xport_bytes_load((const uint8 *)pointer, 4u);
    if (xport_host_range(context, pointer, 4u, &region, &offset, &readonly))
        xport_observe(context, XPORT_MEMORY_READ, region, offset, 4u, value, value, 15u);
    return value;
}

static sint32 xport_store_host(void *pointer, uint32 width, uint32 value)
{
    XportMemoryContext *context = &memory_context;
    XportMemoryRegion region;
    uint32 offset;
    uint32 before;
    sint32 readonly;
    if (!pointer)
    {
        xport_set_error(context, XPORT_MEMORY_ERROR_ARGUMENT);
        return 0;
    }
    if (xport_host_range(context, pointer, width, &region, &offset, &readonly) && readonly)
    {
        xport_set_error(context, XPORT_MEMORY_ERROR_READONLY);
        return 0;
    }
    before = xport_bytes_load((const uint8 *)pointer, width);
    xport_bytes_store((uint8 *)pointer, width, value);
    if (xport_host_range(context, pointer, width, &region, &offset, &readonly))
        xport_observe(context, XPORT_MEMORY_WRITE, region, offset, width, before, value, (1u << width) - 1u);
    return 1;
}

sint32 xport_store_le16(void *pointer, uint32 value)
{
    return xport_store_host(pointer, 2u, value);
}

sint32 xport_store_u8(void *pointer, uint32 value)
{
    return xport_store_host(pointer, 1u, value);
}

sint32 xport_store_le32(void *pointer, uint32 value)
{
    return xport_store_host(pointer, 4u, value);
}

XportMemoryRef xport_host_ref(void *pointer)
{
    XportMemoryRef reference = {XPORT_MEMORY_REF_HOST, pointer, 0u};
    return reference;
}

XportMemoryRef xport_guest_ref(uint32 address)
{
    XportMemoryRef reference = {XPORT_MEMORY_REF_GUEST, NULL, address};
    return reference;
}

sint32 xport_guest_copy(XportMemoryRef destination, XportMemoryRef source, size_t length)
{
    XportMemoryContext *context = &memory_context;
    void *destination_pointer;
    const void *source_pointer;
    XportMappedRange destination_range;
    XportMappedRange source_range;
    XportMemoryRegion observed_region = XPORT_MEMORY_NONE;
    uint32 observed_offset = 0u;
    sint32 readonly;
    if (!context || !length || (destination.kind != XPORT_MEMORY_REF_HOST && destination.kind != XPORT_MEMORY_REF_GUEST) || (source.kind != XPORT_MEMORY_REF_HOST && source.kind != XPORT_MEMORY_REF_GUEST) || (destination.kind == XPORT_MEMORY_REF_HOST && source.kind == XPORT_MEMORY_REF_HOST))
    {
        xport_set_error(context, XPORT_MEMORY_ERROR_ARGUMENT);
        return 0;
    }
    if (destination.kind == XPORT_MEMORY_REF_GUEST)
    {
        if (!xport_map(context, destination.guest_address, length, 1, &destination_range))
            return 0;
        destination_pointer = destination_range.pointer;
        observed_region = destination_range.region;
        observed_offset = destination_range.offset;
    }
    else
    {
        destination_pointer = destination.host_pointer;
        if (destination_pointer && xport_host_range(context, destination_pointer, length, &observed_region, &observed_offset, &readonly) && readonly)
        {
            xport_set_error(context, XPORT_MEMORY_ERROR_READONLY);
            return 0;
        }
    }
    if (source.kind == XPORT_MEMORY_REF_GUEST)
    {
        if (!xport_map(context, source.guest_address, length, 0, &source_range))
            return 0;
        source_pointer = source_range.pointer;
        if (observed_region == XPORT_MEMORY_NONE)
        {
            observed_region = source_range.region;
            observed_offset = source_range.offset;
        }
    }
    else
        source_pointer = source.host_pointer;
    if (!destination_pointer || !source_pointer)
    {
        xport_set_error(context, XPORT_MEMORY_ERROR_ARGUMENT);
        return 0;
    }
    memmove(destination_pointer, source_pointer, length);
    xport_observe(context, XPORT_MEMORY_COPY, observed_region, observed_offset, length, 0u, 0u, 0u);
    return 1;
}

sint32 xport_guest_fill(uint32 destination, uint8 value, size_t length)
{
    XportMemoryContext *context = &memory_context;
    XportMappedRange mapped;
    if (!xport_map(context, destination, length, 1, &mapped))
        return 0;
    memset(mapped.pointer, value, length);
    xport_observe(context, XPORT_MEMORY_FILL, mapped.region, mapped.offset, length, 0u, value, 0u);
    return 1;
}

const MEMORY_EXCEPTION xport_memory_exceptions[] = {{MEMORY_REGION_DRAM, 0x00000000u, 0x00010000u, MEMORY_EXCEPTION_FIRMWARE_RUNTIME}, {MEMORY_REGION_SCRATCHPAD, 0x00000000u, 0x000003c0u, MEMORY_EXCEPTION_GUEST_STACK}};

const uint32 xport_memory_exception_count = (uint32)(sizeof(xport_memory_exceptions) / sizeof(xport_memory_exceptions[0]));

/* Memory observation */
#include <string.h>

static sint32 xport_ranges_overlap(uint32 left_offset, size_t left_length, uint32 right_offset, size_t right_length)
{
    uint64 left_end = (uint64)left_offset + left_length;
    uint64 right_end = (uint64)right_offset + right_length;
    return left_length && right_length && (uint64)left_offset < right_end && (uint64)right_offset < left_end;
}

static sint32 xport_memory_observer_interested(const XportMemoryObserverState *state, XportMemoryRegion region, uint32 offset, size_t width)
{
    uint32 index;
    for (index = 0; state && index < state->watch_count; ++index)
        if (state->watches[index].region == region && xport_ranges_overlap(offset, width, state->watches[index].offset, state->watches[index].length))
            return 1;
    return 0;
}

static void xport_memory_observer_capture(void *user, XportMemoryOperation operation, XportMemoryRegion region, uint32 offset, size_t width, uint32 before, uint32 after, uint32 byte_mask)
{
    XportMemoryObserverState *state = (XportMemoryObserverState *)user;
    XportMemoryEvent event;
    if (!state || !xport_memory_observer_interested(state, region, offset, width))
    {
        if (state)
            ++state->filtered;
        return;
    }
    memset(&event, 0, sizeof(event));
    event.sequence = state->next_sequence++;
    event.operation = operation;
    event.region = region;
    event.offset = offset;
    event.width = width;
    event.before = before;
    event.after = after;
    event.byte_mask = byte_mask;
    if (operation == XPORT_MEMORY_READ || operation == XPORT_MEMORY_WRITE)
        event.flags |= XPORT_MEMORY_EVENT_BEFORE_VALID | XPORT_MEMORY_EVENT_AFTER_VALID;
    if (operation == XPORT_MEMORY_WRITE && before == after)
        event.flags |= XPORT_MEMORY_EVENT_SAME_VALUE;
    if (state->source_depth)
    {
        event.flags |= XPORT_MEMORY_EVENT_SOURCE_VALID;
        event.source = state->source;
    }
    ++state->emitted;
    if (state->sink)
        state->sink(state->sink_user, &event);
}

void xport_memory_observer_init(XportMemoryObserverState *state, XportMemoryEventSink sink, void *user)
{
    if (!state)
        return;
    memset(state, 0, sizeof(*state));
    state->sink = sink;
    state->sink_user = user;
}

sint32 xport_memory_observer_watch(XportMemoryObserverState *state, XportMemoryRegion region, uint32 offset, size_t length)
{
    uint32 index;
    uint64 end = (uint64)offset + length;
    if (!state || region == XPORT_MEMORY_NONE || region > XPORT_MEMORY_MMIO || !length || end > 0x100000000ull || state->watch_count >= XPORT_MEMORY_OBSERVER_MAX_WATCHES)
        return 0;
    for (index = 0; index < state->watch_count; ++index)
        if (state->watches[index].region == region && xport_ranges_overlap(offset, length, state->watches[index].offset, state->watches[index].length))
            return 0;
    state->watches[state->watch_count].region = region;
    state->watches[state->watch_count].offset = offset;
    state->watches[state->watch_count].length = length;
    ++state->watch_count;
    return 1;
}

void xport_memory_observer_clear_watches(XportMemoryObserverState *state)
{
    if (state)
        state->watch_count = 0u;
}

static XportMemoryObserverState *attached_observer;

void xport_memory_observer_attach(XportMemoryObserverState *state)
{
    if (state)
    {
        attached_observer = state;
        xport_memory_bind_observer(xport_memory_observer_capture, state);
    }
}

void xport_memory_observer_detach(XportMemoryObserverState *state)
{
    if (attached_observer == state)
    {
        xport_memory_bind_observer(NULL, NULL);
        attached_observer = NULL;
    }
}

void xport_memory_write_ring_init(MEMORY_WRITE_RING *ring)
{
    if (ring)
        memset(ring, 0, sizeof(*ring));
}

void xport_memory_write_ring_capture(void *user, const XportMemoryEvent *event)
{
    MEMORY_WRITE_RING *ring = (MEMORY_WRITE_RING *)user;
    if (!ring || !event || event->operation == XPORT_MEMORY_READ)
        return;
    ring->entries[ring->next % XPORT_MEMORY_WRITE_RING_CAPACITY] = *event;
    ++ring->next;
    if (ring->count < XPORT_MEMORY_WRITE_RING_CAPACITY)
        ++ring->count;
    else
        ++ring->dropped;
}

const XportMemoryEvent *xport_memory_write_ring_at(const MEMORY_WRITE_RING *ring, uint32 index)
{
    if (!ring || index >= ring->count)
        return NULL;
    return &ring->entries[(ring->next - ring->count + index) % XPORT_MEMORY_WRITE_RING_CAPACITY];
}

static XportMemoryObserverState *xport_memory_observer_state(void)
{
    return attached_observer;
}

static XportMemorySourceLocation xport_memory_observer_begin(const char *file, uint32 line, const char *function, const char *semantic_context, uint32 *prior_depth)
{
    XportMemoryObserverState *state = xport_memory_observer_state();
    XportMemorySourceLocation prior = {0};
    *prior_depth = 0u;
    if (state)
    {
        prior = state->source;
        *prior_depth = state->source_depth;
        state->source.file = file;
        state->source.function = function;
        state->source.context = semantic_context;
        state->source.line = line;
        ++state->source_depth;
    }
    return prior;
}

static void xport_memory_observer_end(XportMemorySourceLocation prior, uint32 prior_depth)
{
    XportMemoryObserverState *state = xport_memory_observer_state();
    if (state)
    {
        state->source = prior;
        state->source_depth = prior_depth;
    }
}

#define XPORT_OBSERVED_CALL(expression) \
    do \
    { \
        uint32 prior_depth; \
        XportMemorySourceLocation prior = xport_memory_observer_begin(file, line, function, semantic_context, &prior_depth); \
        sint32 result = (expression); \
        xport_memory_observer_end(prior, prior_depth); \
        return result; \
    } while (0)

sint32 w_u8_at(uint32 address, uint32 value, const char *file, uint32 line, const char *function, const char *semantic_context)
{
    XPORT_OBSERVED_CALL(w_u8(address, value));
}

sint32 w_u16_at(uint32 address, uint32 value, const char *file, uint32 line, const char *function, const char *semantic_context)
{
    XPORT_OBSERVED_CALL(w_u16(address, value));
}

sint32 w_u32_at(uint32 address, uint32 value, const char *file, uint32 line, const char *function, const char *semantic_context)
{
    XPORT_OBSERVED_CALL(w_u32(address, value));
}

sint32 w_masked_u32_at(uint32 address, uint32 value, uint32 byte_mask, const char *file, uint32 line, const char *function, const char *semantic_context)
{
    XPORT_OBSERVED_CALL(w_masked_u32(address, value, byte_mask));
}

sint32 xport_update_u8_at(uint32 address, XportMemoryUpdate operation, uint32 value, const char *file, uint32 line, const char *function, const char *semantic_context)
{
    XPORT_OBSERVED_CALL(xport_update_u8(address, operation, value));
}

sint32 xport_update_u16_at(uint32 address, XportMemoryUpdate operation, uint32 value, const char *file, uint32 line, const char *function, const char *semantic_context)
{
    XPORT_OBSERVED_CALL(xport_update_u16(address, operation, value));
}

sint32 xport_update_u32_at(uint32 address, XportMemoryUpdate operation, uint32 value, const char *file, uint32 line, const char *function, const char *semantic_context)
{
    XPORT_OBSERVED_CALL(xport_update_u32(address, operation, value));
}

sint32 xport_store_le16_at(void *pointer, uint32 value, const char *file, uint32 line, const char *function, const char *semantic_context)
{
    XPORT_OBSERVED_CALL(xport_store_le16(pointer, value));
}

sint32 xport_store_le32_at(void *pointer, uint32 value, const char *file, uint32 line, const char *function, const char *semantic_context)
{
    XPORT_OBSERVED_CALL(xport_store_le32(pointer, value));
}

sint32 xport_guest_copy_at(XportMemoryRef destination, XportMemoryRef source, size_t length, const char *file, uint32 line, const char *function, const char *semantic_context)
{
    XPORT_OBSERVED_CALL(xport_guest_copy(destination, source, length));
}

sint32 xport_guest_fill_at(uint32 destination, uint8 value, size_t length, const char *file, uint32 line, const char *function, const char *semantic_context)
{
    XPORT_OBSERVED_CALL(xport_guest_fill(destination, value, length));
}

/* Audit events */
static XportAuditSink audit_sink;
static void *audit_user;

void xport_audit_bind(XportAuditSink sink, void *user)
{
    audit_sink = sink;
    audit_user = user;
}

void xport_audit_emit(XportAuditKind kind, const char *image, uint32_t address, const char *name, const char *function, const char *file, int line)
{
    XportAuditRecord record;
    if (!audit_sink)
        return;
    record.kind = kind;
    record.image = image;
    record.address = address;
    record.name = name;
    record.function = function;
    record.file = file;
    record.line = line;
    audit_sink(audit_user, &record);
}

/* Call trace */
#include <stdarg.h>
#include <string.h>

#define XPORT_CALL_MAX_DEPTH 64u
#define XPORT_CALL_MAX_VALUES 16u

typedef struct XportCallFrame
{
    uint64 invocation;
    uint32 callsite;
    uint32 relation;
    const char *function_id;
} XportCallFrame;

static XportCallTraceSink call_sink;
static void *call_user;
static XportCallFrame call_stack[XPORT_CALL_MAX_DEPTH];
static uint32 call_depth;
static uint64 call_sequence;
static uint64 next_invocation = 1u;
static uint32 pending_callsite;
static uint32 pending_relation;

void xport_call_trace_bind(XportCallTraceSink sink, void *user)
{
    call_sink = sink;
    call_user = user;
    call_depth = 0u;
    call_sequence = 0u;
    next_invocation = 1u;
    pending_callsite = 0u;
    pending_relation = 0u;
}

void xport_call_trace_prepare(uint32 callsite, uint32 relation)
{
    pending_callsite = callsite;
    pending_relation = relation;
}

XportCallValue xport_call_scalar(uint64 value)
{
    XportCallValue result = {XPORT_CALL_VALUE_SCALAR, value, XPORT_MEMORY_NONE, 0u, 0u};
    return result;
}

XportCallValue xport_call_void(void)
{
    XportCallValue result = {XPORT_CALL_VALUE_VOID, 0u, XPORT_MEMORY_NONE, 0u, 0u};
    return result;
}

XportCallValue xport_call_guest_address(uint32 address, size_t bytes)
{
    XportCallValue result = {XPORT_CALL_VALUE_GUEST_POINTER, address, XPORT_MEMORY_NONE, 0u, bytes};
    uint32 physical = address & 0x1fffffffu;
    if (physical <= 0x00200000u && bytes <= 0x00200000u - physical)
    {
        result.region = XPORT_MEMORY_DRAM;
        result.offset = physical & 0x001fffffu;
    }
    else if (physical >= 0x1f800000u && physical <= 0x1f800400u && bytes <= 0x1f800400u - physical)
    {
        result.region = XPORT_MEMORY_SCRATCHPAD;
        result.offset = physical - 0x1f800000u;
    }
    else
        result.kind = XPORT_CALL_VALUE_UNSUPPORTED_POINTER;
    return result;
}

XportCallValue xport_call_pointer(const void *pointer, size_t bytes, XportCallValueKind requested)
{
    XportCallValue result = {requested, (uint64)(uintptr_t)pointer, XPORT_MEMORY_NONE, 0u, bytes};
    if (requested == XPORT_CALL_VALUE_GUEST_POINTER && (!bytes || !xport_memory_pointer_identity(pointer, bytes, &result.region, &result.offset)))
        result.kind = XPORT_CALL_VALUE_UNSUPPORTED_POINTER;
    if (requested == XPORT_CALL_VALUE_HOST_POINTER || requested == XPORT_CALL_VALUE_OPAQUE_POINTER)
        result.kind = XPORT_CALL_VALUE_UNSUPPORTED_POINTER;
    return result;
}

void xport_call_trace_enter(const char *function_id, uint32 value_count, ...)
{
    XportCallValue values[XPORT_CALL_MAX_VALUES];
    XportCallEvent event;
    va_list arguments;
    uint32 index;
    if (!function_id || value_count > XPORT_CALL_MAX_VALUES || call_depth >= XPORT_CALL_MAX_DEPTH)
        return;
    va_start(arguments, value_count);
    for (index = 0; index < value_count; ++index)
        values[index] = va_arg(arguments, XportCallValue);
    va_end(arguments);
    event.sequence = call_sequence++;
    event.invocation = next_invocation++;
    event.parent_invocation = call_depth ? call_stack[call_depth - 1u].invocation : 0u;
    event.callsite = pending_callsite;
    event.relation = pending_relation;
    event.edge = XPORT_CALL_ENTER;
    event.function_id = function_id;
    event.values = values;
    event.value_count = value_count;
    call_stack[call_depth].invocation = event.invocation;
    call_stack[call_depth].callsite = event.callsite;
    call_stack[call_depth].relation = event.relation;
    call_stack[call_depth].function_id = function_id;
    ++call_depth;
    pending_callsite = 0u;
    pending_relation = 0u;
    if (call_sink)
        call_sink(call_user, &event);
}

void xport_call_trace_exit(const char *function_id, XportCallValue value)
{
    XportCallEvent event;
    if (!function_id || !call_depth || strcmp(call_stack[call_depth - 1u].function_id, function_id))
        return;
    --call_depth;
    event.sequence = call_sequence++;
    event.invocation = call_stack[call_depth].invocation;
    event.parent_invocation = call_depth ? call_stack[call_depth - 1u].invocation : 0u;
    event.callsite = call_stack[call_depth].callsite;
    event.relation = call_stack[call_depth].relation;
    event.edge = XPORT_CALL_EXIT;
    event.function_id = function_id;
    event.values = &value;
    event.value_count = value.kind == XPORT_CALL_VALUE_VOID ? 0u : 1u;
    if (call_sink)
        call_sink(call_user, &event);
}

#include <string.h>

static XportDeviceEventSink device_sink;
static void *device_user;
static uint64 device_sequence;
static uint64 next_operation_id = 1u;
static uint64 device_frame;
static uint64 device_clock;

static sint32 xport_device_valid(const XportDeviceOperation *operation)
{
    return operation && operation->active && operation->id && operation->device >= XPORT_DEVICE_DMA && operation->device <= XPORT_DEVICE_SPU && operation->primitive >= XPORT_DEVICE_RAM_DMA && operation->primitive <= XPORT_DEVICE_SPU_REVERB;
}

static void xport_device_emit(const XportDeviceOperation *operation, XportDevicePhase phase, XportDeviceEvidence evidence, XportDeviceRegion source_region, uint32 source_offset, XportDeviceRegion destination_region, uint32 destination_offset, uint32 byte_count, uint32 byte_mask, sint32 status)
{
    XportDeviceEvent event;
    if (!device_sink)
        return;
    memset(&event, 0, sizeof(event));
    event.sequence = device_sequence++;
    event.operation_id = operation->id;
    event.parent_operation_id = operation->parent_id;
    event.frame = device_frame;
    event.guest_clock = device_clock;
    event.device = operation->device;
    event.primitive = operation->primitive;
    event.phase = phase;
    event.direction = operation->direction;
    event.evidence = evidence;
    event.source_region = source_region;
    event.destination_region = destination_region;
    event.source_offset = source_offset;
    event.destination_offset = destination_offset;
    event.byte_count = byte_count;
    event.byte_mask = byte_mask;
    event.channel = operation->channel;
    event.command = operation->command;
    event.status = status;
    device_sink(device_user, &event);
}

void xport_device_trace_bind(XportDeviceEventSink sink, void *user)
{
    device_sink = sink;
    device_user = user;
    device_sequence = 0u;
    next_operation_id = 1u;
    device_frame = 0u;
    device_clock = 0u;
}

void xport_device_trace_set_time(uint64 frame, uint64 guest_clock)
{
    device_frame = frame;
    device_clock = guest_clock;
}

sint32 xport_device_trace_begin(XportDeviceOperation *operation, uint64 parent_id, XportDevice device, XportDevicePrimitive primitive, XportDeviceDirection direction, uint32 channel, uint32 command)
{
    if (!operation || operation->active || device < XPORT_DEVICE_DMA || device > XPORT_DEVICE_SPU || primitive < XPORT_DEVICE_RAM_DMA || primitive > XPORT_DEVICE_SPU_REVERB || direction > XPORT_DEVICE_DIRECTION_INTERNAL)
        return 0;
    memset(operation, 0, sizeof(*operation));
    operation->id = next_operation_id++;
    operation->parent_id = parent_id;
    operation->device = device;
    operation->primitive = primitive;
    operation->direction = direction;
    operation->channel = channel;
    operation->command = command;
    operation->active = 1u;
    xport_device_emit(operation, XPORT_DEVICE_CREATE, XPORT_DEVICE_EVIDENCE_EXACT, XPORT_DEVICE_REGION_NONE, 0u, XPORT_DEVICE_REGION_NONE, 0u, 0u, 0u, 0);
    return 1;
}

sint32 xport_device_trace_stage(XportDeviceOperation *operation, XportDevicePhase phase, XportDeviceEvidence evidence, XportDeviceRegion source_region, uint32 source_offset, XportDeviceRegion destination_region, uint32 destination_offset, uint32 byte_count, uint32 byte_mask)
{
    if (!xport_device_valid(operation) || phase < XPORT_DEVICE_SUBMIT || phase > XPORT_DEVICE_RASTER_COMPLETE || evidence < XPORT_DEVICE_EVIDENCE_EXACT || evidence > XPORT_DEVICE_EVIDENCE_UNCOVERED || source_region > XPORT_DEVICE_REGION_SPU_RAM || destination_region > XPORT_DEVICE_REGION_SPU_RAM || (phase == XPORT_DEVICE_WRITE && (!byte_count || destination_region == XPORT_DEVICE_REGION_NONE)))
        return 0;
    xport_device_emit(operation, phase, evidence, source_region, source_offset, destination_region, destination_offset, byte_count, byte_mask, 0);
    return 1;
}

sint32 xport_device_trace_complete(XportDeviceOperation *operation, sint32 status)
{
    if (!xport_device_valid(operation))
        return 0;
    xport_device_emit(operation, XPORT_DEVICE_COMPLETE, XPORT_DEVICE_EVIDENCE_EXACT, XPORT_DEVICE_REGION_NONE, 0u, XPORT_DEVICE_REGION_NONE, 0u, 0u, 0u, status);
    operation->active = 0u;
    return 1;
}

sint32 xport_device_trace_gap(XportDeviceOperation *operation, uint32 reason)
{
    if (!xport_device_valid(operation))
        return 0;
    xport_device_emit(operation, XPORT_DEVICE_GAP, XPORT_DEVICE_EVIDENCE_UNCOVERED, XPORT_DEVICE_REGION_NONE, 0u, XPORT_DEVICE_REGION_NONE, 0u, 0u, 0u, (sint32)reason);
    return 1;
}

sint32 xport_device_trace_enabled(void)
{
    return device_sink != NULL;
}

#if defined(LOCKSTEP_DEBUG)
    /* Lockstep shared memory */
    #if !defined(_WIN32) || !defined(_M_IX86)
        #error LOCKSTEP_DEBUG requires Windows x86
    #endif

    #include <windows.h>
    #include <intrin.h>
    #include <stdio.h>
    #include <stdlib.h>
    #include <string.h>
    #include "psx_gpu.h"

uint8 (*xport_lockstep_dram)[PSX_DRAM_SIZE];
uint32 (*xport_lockstep_scratchpad)[256];
uint16 (*xport_lockstep_vram)[GPU_VRAM_WIDTH * GPU_VRAM_HEIGHT];

static HANDLE dram_handle;
static HANDLE scratchpad_handle;
static HANDLE vram_handle;
static HANDLE deferred_handle;
static XportLockstepMemoryError memory_error;
static XportLockstepMemoryIdentity memory_identity;
static XportLockstepDeferredState *deferred_state;
static XportLockstepDeferredRange deferred_ranges[XPORT_LOCKSTEP_DEFERRED_MAX_RANGES];
static XportLockstepDeferredRange deferred_ready;
static XportLockstepDeferredRange deferred_canonical[XPORT_LOCKSTEP_DEFERRED_MAX_CANONICAL_RANGES];
static uint32 deferred_range_count;
static uint32 deferred_canonical_count;
static uint32 deferred_boundary_count;
static sint32 deferred_open;
static uint32 deferred_catchup_generation;

    #if defined(_MSC_VER)
__declspec(dllexport) __declspec(noinline)
    #endif
void xport_lockstep_memory_ready(void)
{
    #if defined(_MSC_VER)
    _ReadWriteBarrier();
    #endif
}

static sint32 read_environment(const char *name, char *value, DWORD capacity)
{
    DWORD length = GetEnvironmentVariableA(name, value, capacity);
    return length > 0u && length < capacity;
}

static sint32 valid_run_id(const char *value)
{
    size_t index;
    if (strlen(value) != XPORT_LOCKSTEP_RUN_ID_LENGTH)
        return 0;
    for (index = 0; index < XPORT_LOCKSTEP_RUN_ID_LENGTH; ++index)
    {
        char character = value[index];
        if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f')))
            return 0;
    }
    return 1;
}

static sint32 expected_mapping_name(char *value, size_t capacity, const char *run_id, const char *region)
{
    int length = snprintf(value, capacity, "Local\\xport-lockstep-%s-%s", run_id, region);
    return length > 0 && (size_t)length < capacity;
}

static void release_mapping(HANDLE *handle, void **view)
{
    if (*view)
    {
        UnmapViewOfFile(*view);
        *view = NULL;
    }
    if (*handle)
    {
        CloseHandle(*handle);
        *handle = NULL;
    }
}

static sint32 publish_mapping(const char *name, DWORD size, sint32 reuse_existing, HANDLE *handle, void **view)
{
    *handle = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (*handle && !reuse_existing)
    {
        CloseHandle(*handle);
        *handle = NULL;
        memory_error = XPORT_LOCKSTEP_MEMORY_MAPPING_COLLISION;
        return 0;
    }
    if (!*handle)
        *handle = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0u, size, name);
    if (!*handle)
    {
        memory_error = XPORT_LOCKSTEP_MEMORY_CREATE_FAILED;
        return 0;
    }
    *view = MapViewOfFile(*handle, FILE_MAP_ALL_ACCESS, 0u, 0u, size);
    if (!*view)
    {
        CloseHandle(*handle);
        *handle = NULL;
        memory_error = XPORT_LOCKSTEP_MEMORY_MAP_FAILED;
        return 0;
    }
    return 1;
}

sint32 xport_lockstep_memory_init(void)
{
    char expected_dram[XPORT_LOCKSTEP_MAPPING_NAME_CAPACITY];
    char expected_scratchpad[XPORT_LOCKSTEP_MAPPING_NAME_CAPACITY];
    char expected_vram[XPORT_LOCKSTEP_MAPPING_NAME_CAPACITY];
    char expected_deferred[XPORT_LOCKSTEP_MAPPING_NAME_CAPACITY];
    void *dram_view = NULL;
    void *scratchpad_view = NULL;
    void *vram_view = NULL;
    void *deferred_view = NULL;
    char reuse_value[2];
    sint32 reuse_existing = read_environment("XPORT_LOCKSTEP_REUSE_MAPPING", reuse_value, sizeof(reuse_value)) && reuse_value[0] == '1' && getenv("XPORT_LOCKSTEP_PORTABLE_LOAD_PATH") != NULL;

    if (xport_lockstep_dram || xport_lockstep_scratchpad || xport_lockstep_vram || deferred_state)
    {
        memory_error = XPORT_LOCKSTEP_MEMORY_ALREADY_INITIALIZED;
        return 0;
    }
    memset(&memory_identity, 0, sizeof(memory_identity));
    if (!read_environment("XPORT_LOCKSTEP_RUN_ID", memory_identity.run_id, sizeof(memory_identity.run_id)) || !read_environment("XPORT_LOCKSTEP_DRAM_MAPPING", memory_identity.dram_mapping, sizeof(memory_identity.dram_mapping)) || !read_environment("XPORT_LOCKSTEP_SCRATCHPAD_MAPPING", memory_identity.scratchpad_mapping, sizeof(memory_identity.scratchpad_mapping)) || !read_environment("XPORT_LOCKSTEP_DEFERRED_MAPPING", memory_identity.deferred_mapping, sizeof(memory_identity.deferred_mapping)))
    {
        memory_error = XPORT_LOCKSTEP_MEMORY_MISSING_ENVIRONMENT;
        return 0;
    }
    if (!valid_run_id(memory_identity.run_id))
    {
        memory_error = XPORT_LOCKSTEP_MEMORY_INVALID_RUN_ID;
        return 0;
    }
    if (!expected_mapping_name(expected_dram, sizeof(expected_dram), memory_identity.run_id, "dram") || !expected_mapping_name(expected_scratchpad, sizeof(expected_scratchpad), memory_identity.run_id, "scratchpad") || !expected_mapping_name(expected_vram, sizeof(expected_vram), memory_identity.run_id, "vram") || !expected_mapping_name(expected_deferred, sizeof(expected_deferred), memory_identity.run_id, "deferred") || strcmp(memory_identity.dram_mapping, expected_dram) != 0 || strcmp(memory_identity.scratchpad_mapping, expected_scratchpad) != 0 || strcmp(memory_identity.deferred_mapping, expected_deferred) != 0)
    {
        memory_error = XPORT_LOCKSTEP_MEMORY_INVALID_MAPPING_NAME;
        return 0;
    }
    if (!publish_mapping(memory_identity.dram_mapping, PSX_DRAM_SIZE, reuse_existing, &dram_handle, &dram_view))
        return 0;
    if (!publish_mapping(memory_identity.scratchpad_mapping, sizeof(uint32) * 256u, reuse_existing, &scratchpad_handle, &scratchpad_view))
    {
        release_mapping(&dram_handle, &dram_view);
        return 0;
    }
    if (!publish_mapping(expected_vram, GPU_VRAM_WIDTH * GPU_VRAM_HEIGHT * sizeof(uint16), reuse_existing, &vram_handle, &vram_view))
    {
        release_mapping(&scratchpad_handle, &scratchpad_view);
        release_mapping(&dram_handle, &dram_view);
        return 0;
    }
    if (!publish_mapping(memory_identity.deferred_mapping, sizeof(XportLockstepDeferredState), reuse_existing, &deferred_handle, &deferred_view))
    {
        release_mapping(&vram_handle, &vram_view);
        release_mapping(&scratchpad_handle, &scratchpad_view);
        release_mapping(&dram_handle, &dram_view);
        return 0;
    }
    xport_lockstep_dram = (uint8(*)[PSX_DRAM_SIZE])dram_view;
    xport_lockstep_scratchpad = (uint32(*)[256])scratchpad_view;
    xport_lockstep_vram = (uint16(*)[GPU_VRAM_WIDTH * GPU_VRAM_HEIGHT]) vram_view;
    deferred_state = (XportLockstepDeferredState *)deferred_view;
    if (!reuse_existing)
    {
        memset(deferred_state, 0, sizeof(*deferred_state));
        deferred_state->magic = XPORT_LOCKSTEP_DEFERRED_MAGIC;
        deferred_state->version = XPORT_LOCKSTEP_DEFERRED_VERSION;
    }
    else if (deferred_state->magic != XPORT_LOCKSTEP_DEFERRED_MAGIC || deferred_state->version != XPORT_LOCKSTEP_DEFERRED_VERSION)
    {
        release_mapping(&deferred_handle, &deferred_view);
        release_mapping(&vram_handle, &vram_view);
        release_mapping(&scratchpad_handle, &scratchpad_view);
        release_mapping(&dram_handle, &dram_view);
        xport_lockstep_dram = NULL;
        xport_lockstep_scratchpad = NULL;
        xport_lockstep_vram = NULL;
        deferred_state = NULL;
        memory_error = XPORT_LOCKSTEP_MEMORY_MAP_FAILED;
        return 0;
    }
    memory_identity.dram_size = PSX_DRAM_SIZE;
    memory_identity.scratchpad_size = sizeof(uint32) * 256u;
    memory_identity.deferred_size = sizeof(XportLockstepDeferredState);
    deferred_range_count = 0u;
    memset(&deferred_ready, 0, sizeof(deferred_ready));
    memset(deferred_canonical, 0, sizeof(deferred_canonical));
    deferred_canonical_count = 0u;
    deferred_boundary_count = 0u;
    deferred_catchup_generation = deferred_state->generation;
    deferred_open = 0;
    memory_error = XPORT_LOCKSTEP_MEMORY_OK;
    return 1;
}

void xport_lockstep_memory_shutdown(void)
{
    void *dram_view = xport_lockstep_dram;
    void *scratchpad_view = xport_lockstep_scratchpad;
    void *vram_view = xport_lockstep_vram;
    void *deferred_view = deferred_state;
    release_mapping(&deferred_handle, &deferred_view);
    release_mapping(&vram_handle, &vram_view);
    release_mapping(&scratchpad_handle, &scratchpad_view);
    release_mapping(&dram_handle, &dram_view);
    xport_lockstep_dram = NULL;
    xport_lockstep_scratchpad = NULL;
    xport_lockstep_vram = NULL;
    deferred_state = NULL;
    deferred_range_count = 0u;
    deferred_canonical_count = 0u;
    deferred_boundary_count = 0u;
    deferred_catchup_generation = 0u;
    deferred_open = 0;
    memset(&memory_identity, 0, sizeof(memory_identity));
}

XportLockstepMemoryError xport_lockstep_memory_error(void)
{
    return memory_error;
}

const char *xport_lockstep_memory_error_name(XportLockstepMemoryError error)
{
    static const char *names[] = {"ok", "missing_environment", "invalid_run_id", "invalid_mapping_name", "mapping_collision", "create_failed", "map_failed", "already_initialized"};
    if ((uint32)error >= sizeof(names) / sizeof(names[0]))
        return "unknown";
    return names[error];
}

const XportLockstepMemoryIdentity *xport_lockstep_memory_identity(void)
{
    if (!xport_lockstep_dram || !xport_lockstep_scratchpad || !deferred_state)
        return NULL;
    return &memory_identity;
}

size_t xport_lockstep_device_checkpoint_size(void)
{
    return GPU_VRAM_WIDTH * GPU_VRAM_HEIGHT * sizeof(uint16) + sizeof(XportLockstepDeferredState);
}

sint32 xport_lockstep_device_checkpoint_capture(void *data, size_t size)
{
    uint8 *bytes = (uint8 *)data;
    size_t vram_size = GPU_VRAM_WIDTH * GPU_VRAM_HEIGHT * sizeof(uint16);

    if (!bytes || size != xport_lockstep_device_checkpoint_size() || !xport_lockstep_vram || !deferred_state)
        return 0;
    memcpy(bytes, *xport_lockstep_vram, vram_size);
    _ReadWriteBarrier();
    memcpy(bytes + vram_size, deferred_state, sizeof(*deferred_state));
    _ReadWriteBarrier();
    return 1;
}

sint32 xport_lockstep_device_checkpoint_restore(const void *data, size_t size)
{
    const uint8 *bytes = (const uint8 *)data;
    const XportLockstepDeferredState *saved;
    size_t vram_size = GPU_VRAM_WIDTH * GPU_VRAM_HEIGHT * sizeof(uint16);

    if (!bytes || size != xport_lockstep_device_checkpoint_size() || !xport_lockstep_vram || !deferred_state)
        return 0;
    saved = (const XportLockstepDeferredState *)(bytes + vram_size);
    if (saved->magic != XPORT_LOCKSTEP_DEFERRED_MAGIC || saved->version != XPORT_LOCKSTEP_DEFERRED_VERSION)
        return 0;
    memcpy(*xport_lockstep_vram, bytes, vram_size);
    _ReadWriteBarrier();
    memcpy(deferred_state, saved, sizeof(*deferred_state));
    _ReadWriteBarrier();
    return 1;
}

sint32 xport_lockstep_device_ticks(uint64 *ticks)
{
    uint32 attempt;

    if (!deferred_state || !ticks)
        return 0;
    for (attempt = 0u; attempt < 32u; ++attempt)
    {
        uint32 before = deferred_state->device_sequence;
        uint32 low;
        uint32 high;

        if (!before || (before & 1u))
            continue;
        _ReadWriteBarrier();
        low = deferred_state->device_ticks_low;
        high = deferred_state->device_ticks_high;
        _ReadWriteBarrier();
        if (before == deferred_state->device_sequence && !(before & 1u))
        {
            *ticks = ((uint64)high << 32) | low;
            return 1;
        }
    }
    return 0;
}

sint32 xport_lockstep_spu_timeline_active(void)
{
    return deferred_state && deferred_state->spu_timeline_active;
}

sint32 xport_lockstep_spu_event_consume(XportLockstepSpuEventKind kind, uint32 address, uint32 *value, sint32 validate_value, uint64 *ticks)
{
    DWORD started;

    if (!deferred_state || !value || !ticks || !deferred_state->spu_timeline_active)
        return 0;
    started = GetTickCount();
    for (;;)
    {
        uint32 read = deferred_state->spu_timeline_read;
        uint32 write = deferred_state->spu_timeline_write;
        XportLockstepSpuEvent event;

        if (deferred_state->spu_timeline_error)
            return -1;
        if (read != write)
        {
            _ReadWriteBarrier();
            event = deferred_state->spu_events[read % XPORT_LOCKSTEP_SPU_EVENT_CAPACITY];
            _ReadWriteBarrier();
            if (event.kind != (uint32)kind || event.address != address || (validate_value && event.value != *value))
            {
    #ifndef NDEBUG
                fprintf(stderr, "xport_spu_timeline mismatch read=%u write=%u expected=%u:%08X:%08X validate=%d actual=%u:%08X:%08X ticks=%08X%08X\n", read, write, (uint32)kind, address, *value, validate_value, event.kind, event.address, event.value, event.ticks_high, event.ticks_low);
    #endif
                InterlockedCompareExchange((volatile LONG *)&deferred_state->spu_timeline_error, 2, 0);
                return -1;
            }
            *value = event.value;
            *ticks = ((uint64)event.ticks_high << 32) | event.ticks_low;
            InterlockedExchange((volatile LONG *)&deferred_state->spu_timeline_read, (LONG)(read + 1u));
            return 1;
        }
        if (GetTickCount() - started >= 10000u)
        {
    #ifndef NDEBUG
            fprintf(stderr, "xport_spu_timeline timeout read=%u write=%u expected=%u:%08X:%08X validate=%d\n", deferred_state->spu_timeline_read, deferred_state->spu_timeline_write, (uint32)kind, address, *value, validate_value);
    #endif
            InterlockedCompareExchange((volatile LONG *)&deferred_state->spu_timeline_error, 3, 0);
            return -1;
        }
        Sleep(0u);
    }
}

sint32 xport_lockstep_deferred_begin(void)
{
    uint32 generation;

    if (!deferred_state || deferred_open)
        return 0;
    deferred_range_count = 0u;
    deferred_canonical_count = 0u;
    memset(&deferred_ready, 0, sizeof(deferred_ready));
    memset(deferred_canonical, 0, sizeof(deferred_canonical));
    deferred_boundary_count = 0u;
    InterlockedIncrement((volatile LONG *)&deferred_state->sequence);
    generation = deferred_state->generation + 1u;
    if (!generation)
        generation = 1u;
    deferred_state->generation = generation;
    deferred_state->range_count = 0u;
    deferred_state->boundary_count = 0u;
    deferred_state->catchup_count = 0u;
    deferred_state->open = 1u;
    deferred_state->resolved = 0u;
    deferred_state->canonical_count = 0u;
    memset(&deferred_state->ready, 0, sizeof(deferred_state->ready));
    memset(deferred_state->canonical, 0, sizeof(deferred_state->canonical));
    _ReadWriteBarrier();
    InterlockedIncrement((volatile LONG *)&deferred_state->sequence);
    deferred_open = 1;
    return 1;
}

sint32 xport_lockstep_deferred_active(void)
{
    return deferred_open;
}

sint32 xport_lockstep_deferred_add(XportMemoryRegion region, uint32 offset, uint32 size)
{
    XportLockstepDeferredRange *range;
    uint32 index;
    uint32 limit = region == XPORT_MEMORY_DRAM ? PSX_DRAM_SIZE : region == XPORT_MEMORY_SCRATCHPAD ? 0x400u : 0u;

    if (!deferred_open || !limit || !size || offset > limit - size)
        return 0;
    for (index = 0u; index < deferred_range_count; ++index)
    {
        uint32 begin;
        uint32 end;

        range = &deferred_ranges[index];
        if (range->region != (uint32)region || offset > range->offset + range->size || range->offset > offset + size)
            continue;
        begin = offset < range->offset ? offset : range->offset;
        end = offset + size > range->offset + range->size ? offset + size : range->offset + range->size;
        range->offset = begin;
        range->size = end - begin;
        break;
    }
    if (index == deferred_range_count)
    {
        if (deferred_range_count >= XPORT_LOCKSTEP_DEFERRED_MAX_RANGES)
            return 0;
        range = &deferred_ranges[deferred_range_count++];
        range->region = (uint32)region;
        range->offset = offset;
        range->size = size;
    }
    InterlockedIncrement((volatile LONG *)&deferred_state->sequence);
    memcpy(deferred_state->ranges, deferred_ranges, deferred_range_count * sizeof(deferred_ranges[0]));
    deferred_state->range_count = deferred_range_count;
    _ReadWriteBarrier();
    InterlockedIncrement((volatile LONG *)&deferred_state->sequence);
    return 1;
}

sint32 xport_lockstep_deferred_ready(XportMemoryRegion region, uint32 offset, uint32 size)
{
    uint32 limit = region == XPORT_MEMORY_DRAM ? PSX_DRAM_SIZE : region == XPORT_MEMORY_SCRATCHPAD ? 0x400u : 0u;

    if (!deferred_open || !limit || !size || deferred_ready.size || offset > limit - size)
        return 0;
    deferred_ready.region = (uint32)region;
    deferred_ready.offset = offset;
    deferred_ready.size = size;
    return 1;
}

sint32 xport_lockstep_deferred_canonical(XportMemoryRegion region, uint32 offset, uint32 size)
{
    XportLockstepDeferredRange *range;
    uint32 index;
    uint32 limit = region == XPORT_MEMORY_DRAM ? PSX_DRAM_SIZE : region == XPORT_MEMORY_SCRATCHPAD ? 0x400u : 0u;

    if (!deferred_open || !limit || !size || offset > limit - size)
        return 0;
    for (index = 0u; index < deferred_canonical_count; ++index)
    {
        range = &deferred_canonical[index];
        if (range->region == (uint32)region && range->offset == offset && range->size == size)
            return 1;
    }
    if (deferred_canonical_count >= XPORT_LOCKSTEP_DEFERRED_MAX_CANONICAL_RANGES)
        return 0;
    range = &deferred_canonical[deferred_canonical_count++];
    range->region = (uint32)region;
    range->offset = offset;
    range->size = size;
    return 1;
}

void xport_lockstep_deferred_boundary(void)
{
    if (deferred_open && deferred_boundary_count != 0xffffffffu)
    {
        ++deferred_boundary_count;
        InterlockedIncrement((volatile LONG *)&deferred_state->sequence);
        deferred_state->boundary_count = deferred_boundary_count;
        _ReadWriteBarrier();
        InterlockedIncrement((volatile LONG *)&deferred_state->sequence);
    }
}

uint32 xport_lockstep_deferred_boundary_count(void)
{
    return deferred_open ? deferred_boundary_count : 0u;
}

uint32 xport_lockstep_deferred_range_count(void)
{
    return deferred_range_count;
}

uint32 xport_lockstep_deferred_take_catchup(void)
{
    uint32 generation;
    uint32 count;

    if (!deferred_state || deferred_state->open)
        return 0u;
    generation = deferred_state->generation;
    count = deferred_state->catchup_count;
    if (!generation || generation == deferred_catchup_generation || !count)
        return 0u;
    deferred_catchup_generation = generation;
    return count;
}

sint32 xport_lockstep_deferred_resolved(void)
{
    return deferred_state && !deferred_state->open && deferred_state->resolved == 1u;
}

sint32 xport_lockstep_deferred_commit(void)
{
    if (!deferred_state || !deferred_open || !deferred_range_count || !deferred_ready.size || !deferred_canonical_count)
        return 0;
    InterlockedIncrement((volatile LONG *)&deferred_state->sequence);
    memcpy(deferred_state->ranges, deferred_ranges, deferred_range_count * sizeof(deferred_ranges[0]));
    deferred_state->ready = deferred_ready;
    memcpy(deferred_state->canonical, deferred_canonical, deferred_canonical_count * sizeof(deferred_canonical[0]));
    deferred_state->range_count = deferred_range_count;
    deferred_state->canonical_count = deferred_canonical_count;
    deferred_state->boundary_count = deferred_boundary_count;
    deferred_state->open = 0u;
    deferred_state->resolved = 0u;
    _ReadWriteBarrier();
    InterlockedIncrement((volatile LONG *)&deferred_state->sequence);
    deferred_open = 0;
    deferred_range_count = 0u;
    memset(&deferred_ready, 0, sizeof(deferred_ready));
    memset(deferred_canonical, 0, sizeof(deferred_canonical));
    deferred_canonical_count = 0u;
    deferred_boundary_count = 0u;
    return 1;
}

    /* Lockstep boundaries */
    #include <windows.h>
    #include <stdarg.h>
    #include <string.h>

    #if defined(_MSC_VER)
        #include <intrin.h>
    #endif

XPORT_LOCKSTEP_EXPORT const char xport_lockstep_boundary_contract[] = XPORT_LOCKSTEP_BOUNDARY_CONTRACT;
XPORT_LOCKSTEP_EXPORT const volatile LOCKSTEP_BOUNDARY_HIT *xport_lockstep_last_hit;
XPORT_LOCKSTEP_EXPORT volatile uint32 xport_lockstep_mode = LOCKSTEP_MODE_DETAILED;
XPORT_LOCKSTEP_EXPORT volatile uint32 xport_lockstep_epoch;
XPORT_LOCKSTEP_EXPORT volatile uint32 xport_lockstep_target_frame;

static uint64 boundary_sequence;
static char frame_image[32];
static uint32 boundary_frame;
static LOCKSTEP_BOUNDARY_HIT last_hit;

static void publish_hit(const LOCKSTEP_BOUNDARY_HIT *hit)
{
    last_hit = *hit;
    xport_lockstep_last_hit = &last_hit;
    #if defined(_MSC_VER)
    _ReadWriteBarrier();
    #endif
}

XPORT_LOCKSTEP_EXPORT XPORT_LOCKSTEP_NOINLINE void xport_lockstep_boundary_break(const LOCKSTEP_BOUNDARY_HIT *hit)
{
    publish_hit(hit);
}

XPORT_LOCKSTEP_EXPORT XPORT_LOCKSTEP_NOINLINE void xport_lockstep_vblank_break(const LOCKSTEP_BOUNDARY_HIT *hit)
{
    publish_hit(hit);
}

XPORT_LOCKSTEP_EXPORT XPORT_LOCKSTEP_NOINLINE void xport_lockstep_transition_break(const LOCKSTEP_BOUNDARY_HIT *hit)
{
    publish_hit(hit);
}

void xport_lockstep_boundary_emit(const char *image, uint32 address, const char *function, const char *file, uint32 line, const XportCallValue *arguments, uint32 argument_count, XportCallValueKind return_kind, uintptr_t host_return_address)
{
    LOCKSTEP_BOUNDARY_HIT hit;
    if (xport_lockstep_mode != LOCKSTEP_MODE_DETAILED)
        return;
    if (!image || !function || !file || argument_count > XPORT_LOCKSTEP_BOUNDARY_MAX_VALUES || (argument_count && !arguments))
        return;
    memset(&hit, 0, sizeof(hit));
    hit.schema = 1u;
    hit.kind = XPORT_LOCKSTEP_PRE_CALL;
    hit.sequence = boundary_sequence++;
    hit.contract = xport_lockstep_boundary_contract;
    hit.image = image;
    hit.address = address;
    hit.function = function;
    hit.file = file;
    hit.line = line;
    hit.arguments = arguments;
    hit.argument_count = argument_count;
    hit.return_kind = return_kind;
    hit.host_return_address = host_return_address;
    hit.frame = boundary_frame;
    psx_gte_snapshot(&hit.gte);
    xport_lockstep_boundary_break(&hit);
}

void xport_lockstep_boundary_set_frame(uint32 frame)
{
    boundary_frame = frame;
}

void xport_lockstep_boundary_configure(uint32 target_frame)
{
    boundary_sequence = 0u;
    boundary_frame = 0u;
    xport_lockstep_epoch = 0u;
    xport_lockstep_target_frame = target_frame;
    xport_lockstep_mode = target_frame ? LOCKSTEP_MODE_PREFIX_GUARD : LOCKSTEP_MODE_DETAILED;
}

void xport_lockstep_boundary_resume_from_checkpoint(uint32 next_frame)
{
    LOCKSTEP_BOUNDARY_HIT hit;
    memset(&hit, 0, sizeof(hit));
    boundary_sequence = 0u;
    boundary_frame = next_frame;
    if (GetEnvironmentVariableA("XPORT_LOCKSTEP_PREFIX_RESUME", NULL, 0u))
    {
        if (!SetEnvironmentVariableA("XPORT_LOCKSTEP_PREFIX_RESUME", NULL))
            ExitProcess(75u);
        if (xport_lockstep_target_frame > next_frame)
        {
            xport_lockstep_mode = LOCKSTEP_MODE_PREFIX_GUARD;
            xport_lockstep_epoch = 0u;
            return;
        }
    }
    xport_lockstep_mode = LOCKSTEP_MODE_DETAILED;
    xport_lockstep_epoch = 1u;
    xport_lockstep_target_frame = next_frame;
    hit.schema = 1u;
    hit.kind = XPORT_LOCKSTEP_MODE_TRANSITION;
    hit.sequence = boundary_sequence++;
    hit.contract = xport_lockstep_boundary_contract;
    hit.image = frame_image;
    hit.function = "FrameDone";
    hit.file = "xport_lockstep_checkpoint";
    hit.return_kind = XPORT_CALL_VALUE_VOID;
    hit.frame = next_frame;
    xport_lockstep_transition_break(&hit);
}

void xport_lockstep_boundary_frame(const char *image, uint32 frame, const uint32 type[2], const uint32 buttons[2], const uint32 analog[2])
{
    LOCKSTEP_BOUNDARY_HIT hit;
    if (!image || !type || !buttons || !analog)
        return;
    memset(&hit, 0, sizeof(hit));
    hit.schema = 1u;
    hit.kind = XPORT_LOCKSTEP_VBLANK;
    hit.sequence = boundary_sequence++;
    hit.contract = xport_lockstep_boundary_contract;
    hit.image = image;
    hit.function = "FrameDone";
    hit.file = "xport_lockstep_frame";
    hit.return_kind = XPORT_CALL_VALUE_VOID;
    hit.frame = frame;
    memcpy(hit.controller_type, type, sizeof(hit.controller_type));
    memcpy(hit.controller_buttons, buttons, sizeof(hit.controller_buttons));
    memcpy(hit.controller_analog, analog, sizeof(hit.controller_analog));
    psx_gte_snapshot(&hit.gte);
    xport_lockstep_deferred_boundary();
    xport_lockstep_vblank_break(&hit);
    if (xport_lockstep_mode == LOCKSTEP_MODE_PREFIX_GUARD && frame >= xport_lockstep_target_frame)
    {
        memset(&hit, 0, sizeof(hit));
        xport_lockstep_mode = LOCKSTEP_MODE_DETAILED;
        ++xport_lockstep_epoch;
        hit.schema = 1u;
        hit.kind = XPORT_LOCKSTEP_MODE_TRANSITION;
        hit.sequence = boundary_sequence++;
        hit.contract = xport_lockstep_boundary_contract;
        hit.image = image;
        hit.function = "FrameDone";
        hit.file = "xport_lockstep_frame";
        hit.return_kind = XPORT_CALL_VALUE_VOID;
        hit.frame = frame;
        xport_lockstep_transition_break(&hit);
    }
}

void xport_lockstep_function_marker(const char *image, uint32 address, const char *function, const char *file, uint32 line, uintptr_t host_return_address)
{
    xport_lockstep_boundary_emit(image, address, function, file, line, NULL, 0u, XPORT_CALL_VALUE_UNSPECIFIED, host_return_address);
}

void xport_lockstep_function_marker_args(const char *image, uint32 address, const char *function, const char *file, uint32 line, uint32 return_kind, uintptr_t host_return_address, uint32 argument_count, ...)
{
    XportCallValue arguments[XPORT_LOCKSTEP_BOUNDARY_MAX_VALUES];
    va_list values;
    uint32 index;
    if (argument_count > XPORT_LOCKSTEP_BOUNDARY_MAX_VALUES)
        return;
    va_start(values, argument_count);
    for (index = 0u; index < argument_count; ++index)
        arguments[index] = va_arg(values, XportCallValue);
    va_end(values);
    xport_lockstep_boundary_emit(image, address, function, file, line, arguments, argument_count, (XportCallValueKind)return_kind, host_return_address);
}

    /* Lockstep frames */
    #if defined(XPORT_MEMORY_DIAGNOSTICS)
    #endif
    #include "psx.h"
    #include <stdio.h>
    #include <stdlib.h>
    #include <string.h>

typedef struct LOCKSTEP_INPUT_ENTRY
{
    uint32 frame;
    uint32 type[2];
    uint32 buttons[2];
    uint32 analog[2];
} LOCKSTEP_INPUT_ENTRY;

static LOCKSTEP_INPUT_ENTRY *frame_entries;
static uint32 frame_entry_count;
static uint32 frame_entry_index;
static const char *frame_error;
    #if defined(XPORT_MEMORY_DIAGNOSTICS)
GDB_DATA const uint32 xport_lockstep_write_ring_available = 1u;
GDB_DATA const uint32 xport_lockstep_write_event_size = sizeof(XportMemoryEvent);
GDB_DATA volatile uint32 xport_lockstep_write_ring_armed;
GDB_DATA volatile uint32 xport_lockstep_write_request_region;
GDB_DATA volatile uint32 xport_lockstep_write_request_offset;
GDB_DATA volatile uint32 xport_lockstep_write_request_length;
GDB_DATA MEMORY_WRITE_RING xport_lockstep_write_ring;
static XportMemoryObserverState frame_write_observer;

static sint32 frame_watch_apply(uint32 region, uint32 offset, uint32 length)
{
    uint32 limit = region == XPORT_MEMORY_DRAM ? 0x200000u : region == XPORT_MEMORY_SCRATCHPAD ? 0x400u : 0u;
    if (!limit || !length || length > 4u || offset >= limit || length > limit - offset)
        return 0;
    if (xport_lockstep_write_ring_armed)
        xport_memory_observer_detach(&frame_write_observer);
    xport_lockstep_write_ring_armed = 0u;
    xport_memory_write_ring_init(&xport_lockstep_write_ring);
    xport_memory_observer_init(&frame_write_observer, xport_memory_write_ring_capture, &xport_lockstep_write_ring);
    if (!xport_memory_observer_watch(&frame_write_observer, (XportMemoryRegion)region, offset, length))
        return 0;
    xport_memory_observer_attach(&frame_write_observer);
    xport_lockstep_write_ring_armed = 1u;
    return 1;
}

static sint32 frame_watch_configure(void)
{
    const char *region_text = getenv("XPORT_LOCKSTEP_WATCH_REGION");
    const char *offset_text = getenv("XPORT_LOCKSTEP_WATCH_OFFSET");
    const char *length_text = getenv("XPORT_LOCKSTEP_WATCH_LENGTH");
    XportMemoryRegion region;
    unsigned long offset;
    unsigned long length;
    char *end;
    if (!region_text && !offset_text && !length_text)
        return 1;
    if (!region_text || !offset_text || !length_text)
        return 0;
    if (!strcmp(region_text, "dram"))
    {
        region = XPORT_MEMORY_DRAM;
    }
    else if (!strcmp(region_text, "scratchpad"))
    {
        region = XPORT_MEMORY_SCRATCHPAD;
    }
    else
        return 0;
    if (!*offset_text || !*length_text)
        return 0;
    offset = strtoul(offset_text, &end, 10);
    if (*end)
        return 0;
    length = strtoul(length_text, &end, 10);
    if (*end)
        return 0;
    return frame_watch_apply(region, (uint32)offset, (uint32)length);
}
    #else
GDB_DATA const uint32 xport_lockstep_write_ring_available = 0u;
GDB_DATA volatile uint32 xport_lockstep_write_ring_armed;
    #endif

sint32 xport_lockstep_frame_apply_watch_request(void)
{
    #if defined(XPORT_MEMORY_DIAGNOSTICS)
    uint32 region = xport_lockstep_write_request_region;
    uint32 offset = xport_lockstep_write_request_offset;
    uint32 length = xport_lockstep_write_request_length;
    xport_lockstep_write_request_region = 0u;
    xport_lockstep_write_request_offset = 0u;
    xport_lockstep_write_request_length = 0u;
    if (!region)
        return !offset && !length;
    return frame_watch_apply(region, offset, length);
    #else
    return 1;
    #endif
}

static uint8 frame_hex(char value)
{
    if (value >= '0' && value <= '9')
        return (uint8)(value - '0');
    if (value >= 'a' && value <= 'f')
        return (uint8)(value - 'a' + 10);
    return 0xffu;
}

static sint32 frame_read_identity(const char *text, uint8 identity[32])
{
    uint32 index;
    if (!text || strlen(text) != 64u)
        return 0;
    for (index = 0; index < 32u; ++index)
    {
        uint8 high = frame_hex(text[index * 2u]);
        uint8 low = frame_hex(text[index * 2u + 1u]);
        if (high > 15u || low > 15u)
            return 0;
        identity[index] = (uint8)((high << 4) | low);
    }
    return 1;
}

static sint32 frame_validate_entries(void)
{
    uint32 index;
    for (index = 0; index < frame_entry_count; ++index)
    {
        const LOCKSTEP_INPUT_ENTRY *entry = frame_entries + index;
        uint32 port;
        if (index && entry->frame != frame_entries[index - 1u].frame + 1u)
            return 0;
        for (port = 0; port < 2u; ++port)
            if (entry->type[port] > 2u || entry->buttons[port] > 0xffffu)
                return 0;
    }
    return 1;
}

sint32 xport_lockstep_frame_init(const char *image)
{
    const char *path = getenv("XPORT_LOCKSTEP_INPUT_PATH");
    const char *identity_text = getenv("XPORT_LOCKSTEP_RECORD_ID");
    uint8 header[XPORT_LOCKSTEP_INPUT_HEADER_SIZE];
    uint8 expected_identity[32];
    uint32 version;
    uint32 header_size;
    uint32 entry_size;
    uint32 field_rate;
    uint32 count;
    uint32 target_frame = 0u;
    const char *target_text = getenv("XPORT_LOCKSTEP_TARGET_FRAME");
    FILE *file = NULL;
    long size;
    xport_lockstep_frame_shutdown();
    if (!image || !*image || strlen(image) >= sizeof(frame_image) || !path || !*path || !frame_read_identity(identity_text, expected_identity))
        goto invalid_environment;
    if (fopen_s(&file, path, "rb") || !file)
        goto open_failed;
    if (fseek(file, 0, SEEK_END) || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) || fread(header, 1, sizeof(header), file) != sizeof(header))
        goto invalid_file;
    memcpy(&version, header + 8, 4);
    memcpy(&header_size, header + 12, 4);
    memcpy(&entry_size, header + 16, 4);
    memcpy(&field_rate, header + 20, 4);
    memcpy(&count, header + 24, 4);
    if (target_text && *target_text)
    {
        char *end;
        unsigned long parsed = strtoul(target_text, &end, 10);
        if (*end || parsed > 0xfffffffful)
            goto invalid_environment;
        target_frame = (uint32)parsed;
    }
    if (memcmp(header, XPORT_LOCKSTEP_INPUT_MAGIC, 8) || version != XPORT_LOCKSTEP_INPUT_VERSION || header_size != XPORT_LOCKSTEP_INPUT_HEADER_SIZE || entry_size != XPORT_LOCKSTEP_INPUT_ENTRY_SIZE || (field_rate != 50u && field_rate != 60u) || !count || target_frame >= count || memcmp(header + 32, expected_identity, 32) || (uint64)size != (uint64)sizeof(header) + (uint64)count * sizeof(LOCKSTEP_INPUT_ENTRY))
        goto invalid_file;
    frame_entries = (LOCKSTEP_INPUT_ENTRY *)malloc((size_t)count * sizeof(*frame_entries));
    if (!frame_entries)
        goto allocation_failed;
    if (fread(frame_entries, sizeof(*frame_entries), count, file) != count || fgetc(file) != EOF)
        goto invalid_file;
    fclose(file);
    file = NULL;
    frame_entry_count = count;
    frame_entry_index = 0u;
    memcpy(frame_image, image, strlen(image) + 1u);
    if (!frame_validate_entries())
        goto invalid_entries;
    if (frame_entries[0].frame != 0u)
        goto invalid_entries;
    xport_lockstep_boundary_configure(target_frame);
    xport_lockstep_boundary_set_frame(0u);
    #if defined(XPORT_MEMORY_DIAGNOSTICS)
    if (!frame_watch_configure())
        goto invalid_environment;
    #endif
    frame_error = NULL;
    return 1;
invalid_entries:
    frame_error = "invalid_frame_sequence";
    goto failed;
allocation_failed:
    frame_error = "input_allocation_failed";
    goto failed;
invalid_file:
    frame_error = "invalid_input_file";
    goto failed;
open_failed:
    frame_error = "input_open_failed";
    goto failed;
invalid_environment:
    frame_error = "invalid_input_environment";
failed:
    if (file)
        fclose(file);
    free(frame_entries);
    frame_entries = NULL;
    frame_entry_count = frame_entry_index = 0u;
    frame_image[0] = 0;
    return 0;
}

void xport_lockstep_frame_shutdown(void)
{
    #if defined(XPORT_MEMORY_DIAGNOSTICS)
    if (xport_lockstep_write_ring_armed)
    {
        xport_memory_observer_detach(&frame_write_observer);
        xport_lockstep_write_ring_armed = 0u;
    }
    #endif
    free(frame_entries);
    frame_entries = NULL;
    frame_entry_count = frame_entry_index = 0u;
    frame_image[0] = 0;
    frame_error = NULL;
}

sint32 xport_lockstep_frame_done(void)
{
    const LOCKSTEP_INPUT_ENTRY *entry;
    uint32 frame;
    uint32 port;
    if (!frame_entries || frame_entry_index >= frame_entry_count)
    {
        frame_error = "frame_input_exhausted";
        if (getenv("XPORT_LOCKSTEP_RUN_ID"))
            exit(EXIT_SUCCESS);
        return 0;
    }
    entry = frame_entries + frame_entry_index;
    frame = frame_entry_index;
    if (entry->frame != frame)
    {
        frame_error = "frame_input_mismatch";
        return 0;
    }
    frame_entry_index++;
    xport_lockstep_boundary_set_frame(frame + 1u);
    xport_lockstep_boundary_frame(frame_image, frame, entry->type, entry->buttons, entry->analog);
    #if defined(XPORT_MEMORY_DIAGNOSTICS)
    if (xport_lockstep_write_ring_armed)
        xport_memory_write_ring_init(&xport_lockstep_write_ring);
    #endif
    for (port = 0; port < 2u; ++port)
        if (!pad_lockstep_apply((sint32)port, entry->type[port], entry->buttons[port], entry->analog[port]))
        {
            frame_error = "controller_apply_failed";
            return 0;
        }
    return 1;
}

sint32 xport_lockstep_frame_begin_at(uint32 next_frame)
{
    if (!frame_entries || next_frame == 0u || next_frame >= frame_entry_count || frame_entries[next_frame].frame != next_frame)
    {
        frame_error = "handoff_input_cursor_invalid";
        return 0;
    }
    frame_entry_index = next_frame;
    frame_error = NULL;
    xport_lockstep_boundary_set_frame(next_frame);
    return xport_lockstep_frame_apply_watch_request();
}

const char *xport_lockstep_frame_error(void)
{
    return frame_error ? frame_error : "ok";
}

uint32 xport_lockstep_frame_next(void)
{
    return frame_entry_index;
}

sint32 xport_lockstep_frame_resume(uint32 next_frame)
{
    if (!frame_entries || next_frame >= frame_entry_count || frame_entries[next_frame].frame != next_frame)
    {
        frame_error = "checkpoint_input_cursor_invalid";
        return 0;
    }
    frame_entry_index = next_frame;
    frame_error = NULL;
    xport_lockstep_boundary_resume_from_checkpoint(next_frame);
    // The checkpoint already contains the input active after the preceding boundary
    return xport_lockstep_frame_apply_watch_request();
}

    /* Lockstep checkpoints */
    #include "psx_gpu.h"
    #include "psx_spu.h"
    #include <windows.h>
    #include <string.h>
    #include <stdio.h>
    #include <stdlib.h>

extern uint8 (*xport_lockstep_dram)[PSX_DRAM_SIZE];
extern uint32 (*xport_lockstep_scratchpad)[256];
extern int psx_state_io(FILE *file, int load);
extern int gpu_state_io(FILE *file, int load);
extern sint32 xport_game_checkpoint_io(FILE *file, sint32 load);

typedef struct LOCKSTEP_PORTABLE_HEADER
{
    char magic[4];
    uint32 version;
    uint32 next_frame;
    uint32 gte_size;
    char record_id[64];
} LOCKSTEP_PORTABLE_HEADER;

static sint32 portable_registers_io(FILE *file, sint32 load, PsxGteSnapshot *expected)
{
    uint8 area[1024] = {0};
    PsxGteSnapshot snapshot;
    size_t index;
    if (!file || !expected || sizeof(snapshot) > sizeof(area) - 12u)
        return 0;
    if (load)
    {
        if (fread(area, 1, sizeof(area), file) != sizeof(area) || memcmp(area, "XPRG", 4) || area[4] != 1u || area[5] != 0u || area[6] != 1u || area[7] != 0u || area[8] != 5u || area[9] != 0u || area[10] != (uint8)(sizeof(snapshot) & 0xffu) || area[11] != (uint8)(sizeof(snapshot) >> 8))
            return 0;
        for (index = 12u + sizeof(snapshot); index < sizeof(area); ++index)
            if (area[index] != 0u)
                return 0;
        memcpy(&snapshot, area + 12, sizeof(snapshot));
        *expected = snapshot;
        return 1;
    }
    memcpy(area, "XPRG", 4);
    area[4] = 1u;
    area[6] = 1u;
    area[8] = 5u;
    area[10] = (uint8)(sizeof(snapshot) & 0xffu);
    area[11] = (uint8)(sizeof(snapshot) >> 8);
    psx_gte_snapshot(&snapshot);
    memcpy(area + 12, &snapshot, sizeof(snapshot));
    return fwrite(area, 1, sizeof(area), file) == sizeof(area);
}

static sint32 portable_body_io(FILE *file, sint32 load)
{
    PsxGteSnapshot expected;
    PsxGteSnapshot actual;
    if (!(file && xport_lockstep_dram && xport_lockstep_scratchpad && (load ? fread(*xport_lockstep_dram, 1, PSX_DRAM_SIZE, file) == PSX_DRAM_SIZE : fwrite(*xport_lockstep_dram, 1, PSX_DRAM_SIZE, file) == PSX_DRAM_SIZE) && (load ? fread(*xport_lockstep_scratchpad, 1, 1024u, file) == 1024u : fwrite(*xport_lockstep_scratchpad, 1, 1024u, file) == 1024u) && portable_registers_io(file, load, &expected) && psx_state_io(file, load)))
        return 0;
    if (load)
    {
        psx_gte_snapshot(&actual);
        if (memcmp(&expected, &actual, sizeof(actual)))
            return 0;
    }
    return gpu_state_io(file, load) && spu_lockstep_portable_io(file, load) && xport_game_checkpoint_io(file, load);
}

sint32 xport_lockstep_checkpoint_save_portable(const char *path)
{
    LOCKSTEP_PORTABLE_HEADER header;
    const char *record_id = getenv("XPORT_LOCKSTEP_RECORD_ID");
    char temporary[MAX_PATH];
    FILE *file = NULL;
    uint32 next_frame = xport_lockstep_frame_next();
    sint32 ok;
    if (!path || !record_id || strlen(record_id) != sizeof(header.record_id) || !next_frame || strlen(path) + 5u >= sizeof(temporary) || snprintf(temporary, sizeof(temporary), "%s.tmp", path) < 0 || fopen_s(&file, temporary, "wb") || !file)
        return 0;
    memcpy(header.magic, "XPCS", 4);
    header.version = 6u;
    header.next_frame = next_frame;
    header.gte_size = (uint32)sizeof(PsxGteSnapshot);
    memcpy(header.record_id, record_id, sizeof(header.record_id));
    ok = fwrite(&header, 1, sizeof(header), file) == sizeof(header) && portable_body_io(file, 0) && fflush(file) == 0;
    if (fclose(file))
        ok = 0;
    if (!ok || !MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileA(temporary);
        return 0;
    }
    return 1;
}

sint32 xport_lockstep_checkpoint_load_portable(const char *path, uint32 *next_frame)
{
    LOCKSTEP_PORTABLE_HEADER header;
    const char *record_id = getenv("XPORT_LOCKSTEP_RECORD_ID");
    FILE *file = NULL;
    sint32 ok;
    if (!path || !next_frame || !record_id || strlen(record_id) != sizeof(header.record_id) || fopen_s(&file, path, "rb") || !file)
        return 0;
    ok = fread(&header, 1, sizeof(header), file) == sizeof(header) && !memcmp(header.magic, "XPCS", 4) && header.version == 6u && header.next_frame > 0u && header.gte_size == sizeof(PsxGteSnapshot) && !memcmp(header.record_id, record_id, sizeof(header.record_id)) && portable_body_io(file, 1) && fgetc(file) == EOF;
    if (fclose(file))
        ok = 0;
    if (ok && !xport_audio_init())
        ok = 0;
    if (ok)
        *next_frame = header.next_frame;
    return ok;
}

typedef struct LOCKSTEP_CHECKPOINT
{
    uint8 *section;
    uint32 section_size;
    size_t spu_size;
    size_t device_size;
    uint8 *bytes;
} LOCKSTEP_CHECKPOINT;

XPORT_LOCKSTEP_EXPORT jmp_buf xport_lockstep_checkpoint_context;
XPORT_LOCKSTEP_EXPORT volatile uint32 xport_lockstep_checkpoint_valid;
static LOCKSTEP_CHECKPOINT *checkpoint;

static sint32 writable_section(uint8 **base, uint32 *size)
{
    uint8 *image = (uint8 *)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)image;
    IMAGE_NT_HEADERS32 *nt;
    IMAGE_SECTION_HEADER *section;
    uint32 index;
    if (!image || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
        return 0;
    nt = (IMAGE_NT_HEADERS32 *)(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        return 0;
    section = IMAGE_FIRST_SECTION(nt);
    for (index = 0; index < nt->FileHeader.NumberOfSections; ++index)
    {
        if (memcmp(section[index].Name, ".data", 5u) == 0 && (section[index].Characteristics & IMAGE_SCN_MEM_WRITE) != 0 && section[index].Misc.VirtualSize > 0u && section[index].Misc.VirtualSize <= 16u * 1024u * 1024u)
        {
            *base = image + section[index].VirtualAddress;
            *size = section[index].Misc.VirtualSize;
            return 1;
        }
    }
    return 0;
}

XPORT_LOCKSTEP_EXPORT XPORT_LOCKSTEP_NOINLINE sint32 xport_lockstep_checkpoint_capture(void)
{
    LOCKSTEP_CHECKPOINT *candidate;
    uint8 *section;
    uint32 size;
    size_t spu_size = spu_lockstep_checkpoint_size();
    size_t device_size = xport_lockstep_device_checkpoint_size();
    if (!xport_lockstep_dram || !xport_lockstep_scratchpad || !writable_section(&section, &size))
        return 0;
    if (!spu_lockstep_checkpoint_synchronize())
    {
        xport_lockstep_checkpoint_valid = 0u;
        return 0;
    }
    candidate = checkpoint;
    if (candidate && (candidate->section != section || candidate->section_size != size || candidate->spu_size != spu_size || candidate->device_size != device_size))
        return 0;
    if (!candidate)
    {
        candidate = (LOCKSTEP_CHECKPOINT *)VirtualAlloc(NULL, sizeof(*candidate), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!candidate)
            return 0;
        candidate->bytes = (uint8 *)VirtualAlloc(NULL, size + PSX_DRAM_SIZE + 1024u + spu_size + device_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!candidate->bytes)
        {
            VirtualFree(candidate, 0, MEM_RELEASE);
            return 0;
        }
        candidate->section = section;
        candidate->section_size = size;
        candidate->spu_size = spu_size;
        candidate->device_size = device_size;
        checkpoint = candidate;
    }
    xport_lockstep_checkpoint_valid = 1u;
    memcpy(candidate->bytes, section, size);
    memcpy(candidate->bytes + size, *xport_lockstep_dram, PSX_DRAM_SIZE);
    memcpy(candidate->bytes + size + PSX_DRAM_SIZE, *xport_lockstep_scratchpad, 1024u);
    if (!xport_lockstep_device_checkpoint_capture(candidate->bytes + size + PSX_DRAM_SIZE + 1024u + spu_size, device_size))
    {
        xport_lockstep_checkpoint_valid = 0u;
        return 0;
    }
    if (!spu_lockstep_checkpoint_capture(candidate->bytes + size + PSX_DRAM_SIZE + 1024u, spu_size))
    {
        xport_lockstep_checkpoint_valid = 0u;
        return 0;
    }
    return 1;
}

XPORT_LOCKSTEP_EXPORT XPORT_LOCKSTEP_NOINLINE void xport_lockstep_checkpoint_jump(void)
{
    LOCKSTEP_CHECKPOINT *saved = checkpoint;
    if (!xport_lockstep_checkpoint_valid || !saved || !xport_lockstep_dram || !xport_lockstep_scratchpad)
        ExitProcess(74u);
    memcpy(*xport_lockstep_dram, saved->bytes + saved->section_size, PSX_DRAM_SIZE);
    memcpy(*xport_lockstep_scratchpad, saved->bytes + saved->section_size + PSX_DRAM_SIZE, 1024u);
    memcpy(saved->section, saved->bytes, saved->section_size);
    if (!xport_lockstep_device_checkpoint_restore(saved->bytes + saved->section_size + PSX_DRAM_SIZE + 1024u + saved->spu_size, saved->device_size))
        ExitProcess(76u);
    if (!spu_lockstep_checkpoint_restore(saved->bytes + saved->section_size + PSX_DRAM_SIZE + 1024u, saved->spu_size))
        ExitProcess(76u);
    if (!spu_lockstep_checkpoint_rebase_clock())
        ExitProcess(77u);
    longjmp(xport_lockstep_checkpoint_context, 1);
}

void xport_lockstep_checkpoint_invalidate(void)
{
    xport_lockstep_checkpoint_valid = 0u;
}

void xport_lockstep_checkpoint_resumed(void)
{
    const char *export_path = getenv("XPORT_LOCKSTEP_PORTABLE_EXPORT_PATH");
    if (export_path && *export_path && !xport_lockstep_checkpoint_save_portable(export_path))
        DeleteFileA(export_path);
    if (!xport_lockstep_frame_resume(xport_lockstep_frame_next()))
        ExitProcess(75u);
}

#endif

/* Diagnostic stream persistence */
#include <stdlib.h>
#include <time.h>
#include "psx.h"
#include "psx_gpu.h"
#include "psx_spu.h"

#define XPORT_SNAPSHOT_REGION_COUNT 4u
#define XPORT_SNAPSHOT_BLOCK_SIZE 1024u
#define XPORT_SNAPSHOT_MAX_MANIFEST (1024u * 1024u)
#define XPORT_SNAPSHOT_MAX_PAYLOAD (8u * 1024u * 1024u)

typedef struct XportSha256
{
    uint32 state[8];
    uint64 bits;
    uint8 buffer[64];
    uint32 used;
} XportSha256;

typedef struct XportSnapshotRegion
{
    uint32 id;
    uint32 size;
    uint32 blocks;
    uint32 mask_words;
    const uint8 *base;
    uint8 *current;
    uint8 *previous;
    sint32 captured;
} XportSnapshotRegion;

typedef struct XportSnapshotImplementation
{
    FILE *stream;
    char *path;
    char *manifest;
    uint32 manifest_length;
    uint32 keyframe_interval;
    XportSnapshotRegion regions[XPORT_SNAPSHOT_REGION_COUNT];
    XportMemorySnapshotBoundary boundary;
    XportSha256 file_hash;
    uint64 token;
    uint64 snapshots;
    uint64 last_frame;
    uint64 last_clock;
    uint32 phase_id;
    sint32 header_written;
    sint32 active;
    sint32 failed;
} XportSnapshotImplementation;

static const uint32 xport_sha256_constants[64] = {0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

static uint32 xport_rotr32(uint32 value, uint32 count)
{
    return value >> count | value << (32u - count);
}

static void xport_sha256_transform(XportSha256 *hash, const uint8 *block)
{
    uint32 words[64];
    uint32 a;
    uint32 b;
    uint32 c;
    uint32 d;
    uint32 e;
    uint32 f;
    uint32 g;
    uint32 h;
    uint32 index;
    for (index = 0u; index < 16u; ++index)
        words[index] = (uint32)block[index * 4u] << 24 | (uint32)block[index * 4u + 1u] << 16 | (uint32)block[index * 4u + 2u] << 8 | block[index * 4u + 3u];
    for (; index < 64u; ++index)
    {
        uint32 s0 = xport_rotr32(words[index - 15u], 7u) ^ xport_rotr32(words[index - 15u], 18u) ^ (words[index - 15u] >> 3u);
        uint32 s1 = xport_rotr32(words[index - 2u], 17u) ^ xport_rotr32(words[index - 2u], 19u) ^ (words[index - 2u] >> 10u);
        words[index] = words[index - 16u] + s0 + words[index - 7u] + s1;
    }
    a = hash->state[0];
    b = hash->state[1];
    c = hash->state[2];
    d = hash->state[3];
    e = hash->state[4];
    f = hash->state[5];
    g = hash->state[6];
    h = hash->state[7];
    for (index = 0u; index < 64u; ++index)
    {
        uint32 sum1 = xport_rotr32(e, 6u) ^ xport_rotr32(e, 11u) ^ xport_rotr32(e, 25u);
        uint32 choice = (e & f) ^ (~e & g);
        uint32 temp1 = h + sum1 + choice + xport_sha256_constants[index] + words[index];
        uint32 sum0 = xport_rotr32(a, 2u) ^ xport_rotr32(a, 13u) ^ xport_rotr32(a, 22u);
        uint32 majority = (a & b) ^ (a & c) ^ (b & c);
        uint32 temp2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    hash->state[0] += a;
    hash->state[1] += b;
    hash->state[2] += c;
    hash->state[3] += d;
    hash->state[4] += e;
    hash->state[5] += f;
    hash->state[6] += g;
    hash->state[7] += h;
}

static void xport_sha256_init(XportSha256 *hash)
{
    static const uint32 initial[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    memset(hash, 0, sizeof(*hash));
    memcpy(hash->state, initial, sizeof(initial));
}

static void xport_sha256_update(XportSha256 *hash, const void *data, size_t length)
{
    const uint8 *bytes = (const uint8 *)data;
    hash->bits += (uint64)length * 8u;
    while (length)
    {
        size_t available = 64u - hash->used;
        size_t take = length < available ? length : available;
        memcpy(hash->buffer + hash->used, bytes, take);
        hash->used += (uint32)take;
        bytes += take;
        length -= take;
        if (hash->used == 64u)
        {
            xport_sha256_transform(hash, hash->buffer);
            hash->used = 0u;
        }
    }
}

static void xport_sha256_finish(const XportSha256 *source, uint8 digest[32])
{
    XportSha256 hash = *source;
    uint32 index;
    hash.buffer[hash.used++] = 0x80u;
    if (hash.used > 56u)
    {
        memset(hash.buffer + hash.used, 0, 64u - hash.used);
        xport_sha256_transform(&hash, hash.buffer);
        hash.used = 0u;
    }
    memset(hash.buffer + hash.used, 0, 56u - hash.used);
    for (index = 0u; index < 8u; ++index)
        hash.buffer[63u - index] = (uint8)(hash.bits >> (index * 8u));
    xport_sha256_transform(&hash, hash.buffer);
    for (index = 0u; index < 8u; ++index)
    {
        digest[index * 4u] = (uint8)(hash.state[index] >> 24);
        digest[index * 4u + 1u] = (uint8)(hash.state[index] >> 16);
        digest[index * 4u + 2u] = (uint8)(hash.state[index] >> 8);
        digest[index * 4u + 3u] = (uint8)hash.state[index];
    }
}

static void xport_snapshot_u32(uint8 *output, uint32 value)
{
    output[0] = (uint8)value;
    output[1] = (uint8)(value >> 8);
    output[2] = (uint8)(value >> 16);
    output[3] = (uint8)(value >> 24);
}

static void xport_snapshot_u64(uint8 *output, uint64 value)
{
    uint32 index;
    for (index = 0u; index < 8u; ++index)
        output[index] = (uint8)(value >> (index * 8u));
}

static sint32 xport_snapshot_write(XportSnapshotImplementation *implementation, const void *data, size_t length, sint32 hash)
{
    if (!implementation || implementation->failed || !implementation->stream || fwrite(data, 1u, length, implementation->stream) != length)
    {
        if (implementation)
            implementation->failed = 1;
        return 0;
    }
    if (hash)
        xport_sha256_update(&implementation->file_hash, data, length);
    return 1;
}

static sint32 xport_snapshot_record(XportSnapshotImplementation *implementation, uint32 kind, const uint8 *payload, uint32 length, sint32 hash)
{
    uint8 envelope[8];
    uint8 digest[32];
    XportSha256 payload_hash;
    xport_snapshot_u32(envelope, kind);
    xport_snapshot_u32(envelope + 4u, length);
    xport_sha256_init(&payload_hash);
    xport_sha256_update(&payload_hash, payload, length);
    xport_sha256_finish(&payload_hash, digest);
    return xport_snapshot_write(implementation, envelope, sizeof(envelope), hash) && xport_snapshot_write(implementation, payload, length, hash) && xport_snapshot_write(implementation, digest, sizeof(digest), hash);
}

static XportSnapshotImplementation *xport_snapshot_implementation(XportMemorySnapshotWriter *writer)
{
    return writer ? (XportSnapshotImplementation *)writer->implementation : NULL;
}

static XportSnapshotRegion *xport_snapshot_region_by_id(XportSnapshotImplementation *implementation, uint32 region_id)
{
    return implementation && region_id >= 1u && region_id <= XPORT_SNAPSHOT_REGION_COUNT ? &implementation->regions[region_id - 1u] : NULL;
}

static void xport_snapshot_release(XportMemorySnapshotWriter *writer, sint32 close_stream)
{
    XportSnapshotImplementation *implementation = xport_snapshot_implementation(writer);
    uint32 index;
    if (!implementation)
        return;
    if (close_stream && implementation->stream)
        fclose(implementation->stream);
    for (index = 0u; index < XPORT_SNAPSHOT_REGION_COUNT; ++index)
    {
        free(implementation->regions[index].current);
        free(implementation->regions[index].previous);
    }
    free(implementation->path);
    free(implementation->manifest);
    free(implementation);
    writer->implementation = NULL;
}

sint32 xport_memory_snapshot_open(XportMemorySnapshotWriter *writer, const char *path, const char *canonical_manifest, uint32 keyframe_interval)
{
    static const uint32 sizes[XPORT_SNAPSHOT_REGION_COUNT] = {2097152u, 1048576u, 1024u, 524288u};
    static const uint32 mask_words[XPORT_SNAPSHOT_REGION_COUNT] = {32u, 16u, 1u, 8u};
    XportSnapshotImplementation *implementation;
    size_t path_length;
    size_t manifest_length;
    char keyframe_field[64];
    uint32 index;
    if (!writer || writer->implementation || !path || !canonical_manifest || !keyframe_interval)
        return 0;
    path_length = strlen(path);
    manifest_length = strlen(canonical_manifest);
    if (snprintf(keyframe_field, sizeof(keyframe_field), "\"keyframe_interval\":%u", keyframe_interval) < 0 || !path_length || !manifest_length || manifest_length > XPORT_SNAPSHOT_MAX_MANIFEST || canonical_manifest[0] != '{' || canonical_manifest[manifest_length - 1u] != '}' || !strstr(canonical_manifest, keyframe_field))
        return 0;
    implementation = (XportSnapshotImplementation *)calloc(1u, sizeof(*implementation));
    if (!implementation)
        return 0;
    implementation->path = (char *)malloc(path_length + 1u);
    implementation->manifest = (char *)malloc(manifest_length + 1u);
    if (!implementation->path || !implementation->manifest)
    {
        writer->implementation = implementation;
        xport_snapshot_release(writer, 0);
        return 0;
    }
    memcpy(implementation->path, path, path_length + 1u);
    memcpy(implementation->manifest, canonical_manifest, manifest_length + 1u);
    implementation->manifest_length = (uint32)manifest_length;
    implementation->keyframe_interval = keyframe_interval;
    for (index = 0u; index < XPORT_SNAPSHOT_REGION_COUNT; ++index)
    {
        implementation->regions[index].id = index + 1u;
        implementation->regions[index].size = sizes[index];
        implementation->regions[index].blocks = sizes[index] / XPORT_SNAPSHOT_BLOCK_SIZE;
        implementation->regions[index].mask_words = mask_words[index];
    }
    implementation->stream = xport_fopen(path, "wb");
    if (!implementation->stream)
    {
        writer->implementation = implementation;
        xport_snapshot_release(writer, 0);
        return 0;
    }
    xport_sha256_init(&implementation->file_hash);
    writer->implementation = implementation;
    return 1;
}

sint32 xport_memory_snapshot_register_region(XportMemorySnapshotWriter *writer, uint32 region_id, const void *base, size_t size)
{
    XportSnapshotImplementation *implementation = xport_snapshot_implementation(writer);
    XportSnapshotRegion *region = xport_snapshot_region_by_id(implementation, region_id);
    uintptr_t begin = (uintptr_t)base;
    uintptr_t end;
    uint32 index;
    if (!implementation || implementation->header_written || !region || !base || size != region->size || region->base || begin > UINTPTR_MAX - size)
        return 0;
    end = begin + size;
    for (index = 0u; index < XPORT_SNAPSHOT_REGION_COUNT; ++index)
    {
        const XportSnapshotRegion *other = &implementation->regions[index];
        uintptr_t other_begin = (uintptr_t)other->base;
        uintptr_t other_end;
        if (!other->base)
            continue;
        other_end = other_begin + other->size;
        if (begin < other_end && other_begin < end)
            return 0;
    }
    region->current = (uint8 *)malloc(size);
    region->previous = (uint8 *)malloc(size);
    if (!region->current || !region->previous)
    {
        free(region->current);
        free(region->previous);
        region->current = NULL;
        region->previous = NULL;
        return 0;
    }
    region->base = (const uint8 *)base;
    return 1;
}

static sint32 xport_snapshot_header(XportSnapshotImplementation *implementation)
{
    static const uint8 magic[8] = {'X', 'P', 'M', 'E', 'M', '0', '0', '1'};
    uint8 header[16];
    uint32 index;
    if (implementation->header_written)
        return 1;
    for (index = 0u; index < XPORT_SNAPSHOT_REGION_COUNT; ++index)
        if (!implementation->regions[index].base)
            return 0;
    memcpy(header, magic, sizeof(magic));
    xport_snapshot_u32(header + 8u, sizeof(header));
    xport_snapshot_u32(header + 12u, implementation->manifest_length);
    if (!xport_snapshot_write(implementation, header, sizeof(header), 1) || !xport_snapshot_write(implementation, implementation->manifest, implementation->manifest_length, 1))
        return 0;
    implementation->header_written = 1;
    return 1;
}

uint64 xport_memory_snapshot_begin(XportMemorySnapshotWriter *writer, XportMemorySnapshotBoundary boundary)
{
    XportSnapshotImplementation *implementation = xport_snapshot_implementation(writer);
    uint32 index;
    if (!implementation || implementation->failed || implementation->active || !xport_snapshot_header(implementation))
        return 0u;
    if (implementation->snapshots)
    {
        if (boundary.frame_id != implementation->last_frame + 1u || boundary.guest_clock <= implementation->last_clock || !boundary.phase_id)
            return 0u;
        if (implementation->phase_id && boundary.phase_id != implementation->phase_id)
            return 0u;
    }
    if (boundary.phase_id)
    {
        if (implementation->phase_id && boundary.phase_id != implementation->phase_id)
            return 0u;
        implementation->phase_id = boundary.phase_id;
    }
    for (index = 0u; index < XPORT_SNAPSHOT_REGION_COUNT; ++index)
        implementation->regions[index].captured = 0;
    implementation->boundary = boundary;
    implementation->active = 1;
    ++implementation->token;
    if (!implementation->token)
        ++implementation->token;
    return implementation->token;
}

sint32 xport_memory_snapshot_region(XportMemorySnapshotWriter *writer, uint64 token, uint32 region_id)
{
    XportSnapshotImplementation *implementation = xport_snapshot_implementation(writer);
    XportSnapshotRegion *region = xport_snapshot_region_by_id(implementation, region_id);
    if (!implementation || !implementation->active || token != implementation->token || !region || region->captured)
        return 0;
    memcpy(region->current, region->base, region->size);
    region->captured = 1;
    return 1;
}

static uint8 *xport_snapshot_append_u32(uint8 *output, uint32 value)
{
    xport_snapshot_u32(output, value);
    return output + 4u;
}

static uint8 *xport_snapshot_append_u64(uint8 *output, uint64 value)
{
    xport_snapshot_u64(output, value);
    return output + 8u;
}

sint32 xport_memory_snapshot_commit(XportMemorySnapshotWriter *writer, uint64 token)
{
    XportSnapshotImplementation *implementation = xport_snapshot_implementation(writer);
    uint8 *payload;
    uint8 *output;
    uint32 index;
    uint32 kind;
    sint32 full;
    if (!implementation || !implementation->active || token != implementation->token || implementation->failed)
        return 0;
    for (index = 0u; index < XPORT_SNAPSHOT_REGION_COUNT; ++index)
        if (!implementation->regions[index].captured)
            return 0;
    payload = (uint8 *)malloc(XPORT_SNAPSHOT_MAX_PAYLOAD);
    if (!payload)
        return 0;
    output = payload;
    output = xport_snapshot_append_u64(output, implementation->boundary.frame_id);
    output = xport_snapshot_append_u64(output, implementation->boundary.guest_clock);
    output = xport_snapshot_append_u64(output, implementation->boundary.event_cursor);
    output = xport_snapshot_append_u32(output, implementation->boundary.phase_id);
    output = xport_snapshot_append_u32(output, XPORT_SNAPSHOT_REGION_COUNT);
    full = !implementation->snapshots || implementation->snapshots % implementation->keyframe_interval == 0u;
    kind = full ? 1u : 2u;
    for (index = 0u; index < XPORT_SNAPSHOT_REGION_COUNT; ++index)
    {
        XportSnapshotRegion *region = &implementation->regions[index];
        uint64 masks[32] = {0u};
        uint32 block;
        uint32 changed = 0u;
        uint32 word;
        for (block = 0u; block < region->blocks; ++block)
            if (full || memcmp(region->current + (size_t)block * XPORT_SNAPSHOT_BLOCK_SIZE, region->previous + (size_t)block * XPORT_SNAPSHOT_BLOCK_SIZE, XPORT_SNAPSHOT_BLOCK_SIZE))
            {
                masks[block / 64u] |= (uint64)1u << (block % 64u);
                ++changed;
            }
        output = xport_snapshot_append_u32(output, region->id);
        output = xport_snapshot_append_u32(output, region->size);
        output = xport_snapshot_append_u32(output, XPORT_SNAPSHOT_BLOCK_SIZE);
        output = xport_snapshot_append_u32(output, region->mask_words);
        output = xport_snapshot_append_u32(output, changed);
        output = xport_snapshot_append_u32(output, changed * XPORT_SNAPSHOT_BLOCK_SIZE);
        for (word = 0u; word < region->mask_words; ++word)
            output = xport_snapshot_append_u64(output, masks[word]);
        for (block = 0u; block < region->blocks; ++block)
            if (masks[block / 64u] & ((uint64)1u << (block % 64u)))
            {
                memcpy(output, region->current + (size_t)block * XPORT_SNAPSHOT_BLOCK_SIZE, XPORT_SNAPSHOT_BLOCK_SIZE);
                output += XPORT_SNAPSHOT_BLOCK_SIZE;
            }
    }
    if ((size_t)(output - payload) > XPORT_SNAPSHOT_MAX_PAYLOAD || !xport_snapshot_record(implementation, kind, payload, (uint32)(output - payload), 1))
    {
        free(payload);
        return 0;
    }
    free(payload);
    for (index = 0u; index < XPORT_SNAPSHOT_REGION_COUNT; ++index)
        memcpy(implementation->regions[index].previous, implementation->regions[index].current, implementation->regions[index].size);
    implementation->last_frame = implementation->boundary.frame_id;
    implementation->last_clock = implementation->boundary.guest_clock;
    ++implementation->snapshots;
    implementation->active = 0;
    return 1;
}

sint32 xport_memory_snapshot_finish(XportMemorySnapshotWriter *writer)
{
    XportSnapshotImplementation *implementation = xport_snapshot_implementation(writer);
    uint8 payload[56];
    uint8 file_digest[32];
    sint32 result;
    if (!implementation || implementation->active || !implementation->snapshots || implementation->failed)
        return 0;
    xport_sha256_finish(&implementation->file_hash, file_digest);
    xport_snapshot_u64(payload, implementation->snapshots);
    xport_snapshot_u64(payload + 8u, implementation->last_frame);
    xport_snapshot_u64(payload + 16u, implementation->last_clock);
    memcpy(payload + 24u, file_digest, sizeof(file_digest));
    result = xport_snapshot_record(implementation, 3u, payload, sizeof(payload), 0);
#ifndef NDEBUG
    result = result && fflush(implementation->stream) == 0;
#endif
    if (fclose(implementation->stream) != 0)
        result = 0;
    implementation->stream = NULL;
    xport_snapshot_release(writer, 0);
    return result;
}

void xport_memory_snapshot_abort(XportMemorySnapshotWriter *writer)
{
    xport_snapshot_release(writer, 1);
}

sint32 xport_memory_snapshot_register_psx_regions(XportMemorySnapshotWriter *writer)
{
    return xport_memory_snapshot_register_region(writer, 1u, DRAM, sizeof(DRAM)) && xport_memory_snapshot_register_region(writer, 2u, VRAM, sizeof(VRAM)) && xport_memory_snapshot_register_region(writer, 3u, SCRATCHPAD, sizeof(SCRATCHPAD)) && xport_memory_snapshot_register_region(writer, 4u, spu_ram_data(), SPU_RAM_SIZE);
}

sint32 xport_memory_snapshot_capture_psx(XportMemorySnapshotWriter *writer, XportMemorySnapshotBoundary boundary)
{
    uint64 token = xport_memory_snapshot_begin(writer, boundary);
    return token && xport_memory_snapshot_region(writer, token, 1u) && xport_memory_snapshot_region(writer, token, 2u) && xport_memory_snapshot_region(writer, token, 3u) && xport_memory_snapshot_region(writer, token, 4u) && xport_memory_snapshot_commit(writer, token);
}

#ifndef NDEBUG
int xport_journal_file_sha256(const char *path, char output[65])
{
    XportSha256 hash;
    uint8 data[16384], digest[32];
    size_t count;
    uint32 index;
    int failed;
    FILE *stream;
    if (!path || !output)
        return 0;
    stream = xport_fopen(path, "rb");
    if (!stream)
        return 0;
    xport_sha256_init(&hash);
    while ((count = fread(data, 1, sizeof(data), stream)) != 0)
        xport_sha256_update(&hash, data, count);
    #ifndef NDEBUG
    failed = ferror(stream);
    #else
    failed = 0;
    #endif
    if (fclose(stream))
        failed = 1;
    if (failed)
        return 0;
    xport_sha256_finish(&hash, digest);
    for (index = 0; index < 32; ++index)
        sprintf(output + 2 * index, "%02x", digest[index]);
    output[64] = 0;
    return 1;
}

static int quoted(FILE *stream, const char *value)
{
    const unsigned char *cursor = (const unsigned char *)(value ? value : "");
    if (fputc('"', stream) == EOF)
        return 0;
    for (; *cursor; ++cursor)
    {
        if ((*cursor == '"' || *cursor == '\\') && fputc('\\', stream) == EOF)
            return 0;
        if (*cursor < 32u)
        {
            if (fprintf(stream, "\\u%04x", *cursor) < 0)
                return 0;
        }
        else if (fputc(*cursor, stream) == EOF)
            return 0;
    }
    return fputc('"', stream) != EOF;
}

static int field(XportJournal *journal, const char *name)
{
    if (!journal || !journal->stream || journal->failed)
        return 0;
    if ((journal->fields && fputc(',', journal->stream) == EOF) || !quoted(journal->stream, name) || fputc(':', journal->stream) == EOF)
    {
        journal->failed = 1;
        return 0;
    }
    ++journal->fields;
    return 1;
}

int xport_journal_begin(XportJournal *journal, const char *event)
{
    if (!journal || !journal->stream || journal->failed || fputc('{', journal->stream) == EOF)
        return 0;
    journal->fields = 0;
    return xport_journal_string(journal, "event", event);
}

int xport_journal_string(XportJournal *journal, const char *name, const char *value)
{
    return field(journal, name) && quoted(journal->stream, value);
}

int xport_journal_u64(XportJournal *journal, const char *name, uint64_t value)
{
    return field(journal, name) && fprintf(journal->stream, "%llu", (unsigned long long)value) >= 0;
}

int xport_journal_u32_hex(XportJournal *journal, const char *name, uint32_t value)
{
    return field(journal, name) && fprintf(journal->stream, "\"%08X\"", value) >= 0;
}

int xport_journal_boolean(XportJournal *journal, const char *name, int value)
{
    return field(journal, name) && fputs(value ? "true" : "false", journal->stream) >= 0;
}

int xport_journal_u32_hex_array(XportJournal *journal, const char *name, const uint32_t *values, size_t count)
{
    size_t index;
    if (!field(journal, name) || fputc('[', journal->stream) == EOF)
        return 0;
    for (index = 0; index < count; ++index)
        if (fprintf(journal->stream, "%s\"%08X\"", index ? "," : "", values[index]) < 0)
            return 0;
    return fputc(']', journal->stream) != EOF;
}

int xport_journal_hex_bytes(XportJournal *journal, const char *name, uint32_t address, size_t count, XportJournalReadByte read_byte, void *user)
{
    size_t index;
    if (!read_byte || !field(journal, name) || fputc('"', journal->stream) == EOF)
        return 0;
    for (index = 0; index < count; ++index)
        if (fprintf(journal->stream, "%02X", read_byte(user, address + (uint32_t)index)) < 0)
            return 0;
    return fputc('"', journal->stream) != EOF;
}

int xport_journal_end(XportJournal *journal)
{
    if (!journal || !journal->stream || journal->failed || fputs("}\n", journal->stream) < 0
    #ifndef NDEBUG
        || fflush(journal->stream) || ferror(journal->stream)
    #endif
    )
    {
        if (journal)
            journal->failed = 1;
        return 0;
    }
    return 1;
}

int xport_journal_open(XportJournal *journal, const char *path, const char *build, const char *executable, int continued)
{
    char digest[65];
    if (!journal || !xport_journal_file_sha256(executable, digest))
        return 0;
    memset(journal, 0, sizeof(*journal));
    journal->stream = xport_fopen(path, "ab");
    if (!journal->stream)
        return 0;
    return xport_journal_begin(journal, "session") && xport_journal_u64(journal, "time", (uint64_t)time(NULL)) && xport_journal_boolean(journal, "continue", continued) && xport_journal_string(journal, "build", build) && xport_journal_string(journal, "exe_sha256", digest) && xport_journal_end(journal);
}

int xport_journal_close(XportJournal *journal)
{
    int ok;
    if (!journal)
        return 0;
    ok = !journal->failed;
    if (journal->stream && fclose(journal->stream))
        ok = 0;
    memset(journal, 0, sizeof(*journal));
    return ok;
}

int xport_trace_env_u32(const char *name, uint32_t maximum, uint32_t *value)
{
    const char *text = getenv(name);
    char *end;
    unsigned long parsed;

    if (!text || !*text || !value)
        return 0;
    parsed = strtoul(text, &end, 10);
    if (*end || parsed > maximum)
        return 0;
    *value = (uint32_t)parsed;
    return 1;
}

int xport_trace_open_suffix(FILE **stream, const char *base, const char *suffix, const char *mode)
{
    char path[1024];
    int length;

    if (!stream || !base || !suffix || !mode)
        return 0;
    length = snprintf(path, sizeof(path), "%s%s", base, suffix);
    if (length < 0 || (size_t)length >= sizeof(path))
        return 0;
    *stream = xport_fopen(path, mode);
    return *stream != NULL;
}

int xport_trace_open_streams(XportTraceStreams *streams, const char *phase_path, const char *calls_path, const char *output_base, const char *phase_mode, const char *calls_mode)
{
    if (!streams || !phase_path || !calls_path || !output_base)
        return 0;
    memset(streams, 0, sizeof(*streams));
    streams->phases = xport_fopen(phase_path, phase_mode);
    streams->calls = xport_fopen(calls_path, calls_mode);
    if (!streams->phases || !streams->calls || !xport_trace_open_suffix(&streams->actors, output_base, ".actors", "wb") || !xport_trace_open_suffix(&streams->world, output_base, ".world", "wb") || !xport_trace_open_suffix(&streams->inputs, output_base, ".inputs", "wb"))
    {
        xport_trace_close_streams(streams);
        return 0;
    }
    return 1;
}

int xport_trace_close_streams(XportTraceStreams *streams)
{
    int ok = 1;

    if (!streams)
        return 0;
    if (streams->phases && fclose(streams->phases))
        ok = 0;
    if (streams->calls && fclose(streams->calls))
        ok = 0;
    if (streams->actors && fclose(streams->actors))
        ok = 0;
    if (streams->world && fclose(streams->world))
        ok = 0;
    if (streams->inputs && fclose(streams->inputs))
        ok = 0;
    memset(streams, 0, sizeof(*streams));
    return ok;
}

int xport_trace_read_context(const char *checkpoint_path, uint32_t magic, uint32_t pc, uint32_t phase, uint32_t values[12])
{
    char path[1040];
    FILE *stream;
    int length;
    int ok;

    if (!checkpoint_path || !values)
        return 0;
    length = snprintf(path, sizeof(path), "%s.ctx", checkpoint_path);
    if (length < 0 || (size_t)length >= sizeof(path))
        return 0;
    stream = xport_fopen(path, "rb");
    if (!stream)
        return 0;
    ok = fread(values, sizeof(uint32_t), 12, stream) == 12 && fgetc(stream) == EOF;
    if (fclose(stream))
        ok = 0;
    return ok && values[0] == magic && values[1] == pc && values[2] == phase;
}

int xport_trace_write_progress(const char *path, uint32_t sequence, const uint32_t *values, size_t count)
{
    char temporary[1060];
    FILE *stream;
    int length;
    int ok;

    if (!path || !*path)
        return 1;
    length = snprintf(temporary, sizeof(temporary), "%s.%u.tmp", path, sequence);
    if (length < 0 || (size_t)length >= sizeof(temporary))
        return 0;
    stream = xport_fopen(temporary, "wb");
    if (!stream)
        return 0;
    ok = fwrite(values, sizeof(uint32_t), count, stream) == count;
    if (fclose(stream))
        ok = 0;
    if (ok)
    {
        remove(path);
        ok = rename(temporary, path) == 0;
    }
    if (!ok)
        remove(temporary);
    return ok;
}

#endif

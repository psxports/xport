#if defined(OBSERVER_INSTRUMENTED)
#define XPORT_MEMORY_DIAGNOSTICS
#endif
#include "xport_trace.h"
#include "psx.h"

#include <stdio.h>
#include <string.h>

uint8 DRAM[PSX_DRAM_SIZE];
uint32 SCRATCHPAD[256];

FILE *xport_fopen(const char *path, const char *mode)
{
    FILE *stream = NULL;
    return fopen_s(&stream, path, mode) ? NULL : stream;
}

typedef struct FixtureEvents
{
    MEMORY_WRITE_RING ring;
    uint32 count;
    uint32 sourced;
    uint32 same_value;
    uint32 copies;
    uint32 fills;
    uint32 last_offset;
} FixtureEvents;

static void capture(void *user, const XportMemoryEvent *event)
{
    FixtureEvents *events = (FixtureEvents *)user;
    xport_memory_write_ring_capture(&events->ring, event);
    ++events->count;
    events->last_offset = event->offset;
    if (event->flags & XPORT_MEMORY_EVENT_SOURCE_VALID)
        ++events->sourced;
    if (event->flags & XPORT_MEMORY_EVENT_SAME_VALUE)
        ++events->same_value;
    if (event->operation == XPORT_MEMORY_COPY)
        ++events->copies;
    if (event->operation == XPORT_MEMORY_FILL)
        ++events->fills;
}

static uint64 checksum(const uint8 *bytes, size_t size)
{
    uint64 hash = 1469598103934665603ull;
    size_t index;
    for (index = 0; index < size; ++index)
    {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

int main(void)
{
    XportMemoryObserverState observer;
    FixtureEvents events;
    uint8 host[16];
    uint64 state_hash;

    memset(DRAM, 0, sizeof(DRAM));
    memset(SCRATCHPAD, 0, sizeof(SCRATCHPAD));
    memset(&events, 0, sizeof(events));
    xport_memory_write_ring_init(&events.ring);
    memset(host, 0, sizeof(host));
    xport_memory_init(DRAM, sizeof(DRAM), (uint8 *)SCRATCHPAD, sizeof(SCRATCHPAD), NULL, 0u);
    xport_memory_observer_init(&observer, capture, &events);
    if (!xport_memory_observer_watch(&observer, XPORT_MEMORY_DRAM, 100u, 32u))
        return 1;
    if (!xport_memory_observer_watch(&observer, XPORT_MEMORY_SCRATCHPAD, 0u, 8u))
        return 1;
    if (xport_memory_observer_watch(&observer, XPORT_MEMORY_DRAM, 110u, 4u))
        return 1;
    xport_memory_observer_attach(&observer);

    if (!w_u32(100u, 0x11223344u))
        return 1;
    if (!w_masked_u32(102u, 0xaabbccddu, 5u))
        return 1;
    if (!w_u32(0x80000064u, r_u32(100u)))
        return 1;
    if (!xport_guest_copy(xport_guest_ref(108u), xport_guest_ref(100u), 8u))
        return 1;
    if (!xport_guest_copy(xport_guest_ref(110u), xport_guest_ref(108u), 8u))
        return 1;
    if (!xport_guest_fill(120u, 0x5au, 6u))
        return 1;
    if (!w_u16(127u, 0x7788u))
        return 1;
    if (!w_u32(200u, 0xdeadc0deu))
        return 1;
    {
        uint32 value = 0xabcdef01u;
        memcpy(host + 1u, &value, sizeof(value));
    }
    if (!w_u32(0x1f800000u, 0x01020304u))
        return 1;

    state_hash = checksum(DRAM, sizeof(DRAM)) ^ checksum((const uint8 *)SCRATCHPAD, sizeof(SCRATCHPAD));
    if (events.ring.count != 8u || events.ring.dropped ||
        !xport_memory_write_ring_at(&events.ring, 0u) ||
        xport_memory_write_ring_at(&events.ring, 8u))
        return 1;
    {
        XportMemoryEvent synthetic = {0};
        uint32 index;
        synthetic.operation = XPORT_MEMORY_WRITE;
        xport_memory_write_ring_init(&events.ring);
        for (index = 0u; index < XPORT_MEMORY_WRITE_RING_CAPACITY + 3u; ++index)
        {
            synthetic.sequence = index;
            xport_memory_write_ring_capture(&events.ring, &synthetic);
        }
        if (events.ring.count != XPORT_MEMORY_WRITE_RING_CAPACITY ||
            events.ring.dropped != 3u ||
            xport_memory_write_ring_at(&events.ring, 0u)->sequence != 3u ||
            xport_memory_write_ring_at(&events.ring, XPORT_MEMORY_WRITE_RING_CAPACITY - 1u)->sequence != XPORT_MEMORY_WRITE_RING_CAPACITY + 2u)
            return 1;
    }
    printf("{\"state_hash\":\"%016llx\",\"events\":%u,\"sourced\":%u,\"same_value\":%u,\"copies\":%u,\"fills\":%u,\"filtered\":%llu}\n",
           (unsigned long long)state_hash, events.count, events.sourced, events.same_value,
           events.copies, events.fills, (unsigned long long)observer.filtered);
    return events.count == 9u && events.same_value == 1u && events.copies == 2u && events.fills == 1u && events.last_offset == 0u ? 0 : 1;
}

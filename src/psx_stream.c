#include "psx_stream.h"
#include "psx_spu.h"
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
extern sint32 cd_stream_pop_sector_native(uint8 output[2352]);
extern void cd_stream_consumer_native(sint32 enabled);
#define STREAM_PAYLOAD 2016u
#define STREAM_HEADER 32u

typedef struct
{
    uint8 *ring, *payload;
    const uint8 *emulate;
    uint32 count, read, write, assembly, part, frame, chunks;
    uint32 start, end, mask, channel, next_channel, mode, read_mode;
    uint32 generation;
    sint32 configured, pumping, hooked, have_back, ended;
    CdlLOC back;
    uint32 back_frame;
    void (*complete)(void), (*limit)(void);
    CdlCB previous_ready;
    uint8 callbacks[256];
    uint32 callback_read, callback_count;
} StreamState;

static StreamState stream;
sint32 StCdIntrFlag;

static void stream_wait_service(void)
{
    while (stream.callback_count && psx_callbacks_allowed())
    {
        void (*callback)(void) = stream.callbacks[stream.callback_read] ? stream.limit : stream.complete;
        stream.callback_read = (stream.callback_read + 1) & 255;
        --stream.callback_count;
        if (callback)
            callback();
    }
}

static void stream_completion(uint32 limit)
{
    void (*callback)(void) = limit ? stream.limit : stream.complete;
    if (!callback)
        return;
    if (psx_callbacks_allowed())
    {
        callback();
        return;
    }
    if (stream.callback_count == 256)
        abort();
    stream.callbacks[(stream.callback_read + stream.callback_count) & 255] = (uint8)limit;
    ++stream.callback_count;
    psx_wait_signal_service(3);
    psx_sync_callbacks();
}

static uint16 stream_u16(const uint8 *p)
{
    return (uint16)(p[0] | ((uint16)p[1] << 8));
}

static uint32 stream_u32(const uint8 *p)
{
    return (uint32)p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16) | ((uint32)p[3] << 24);
}

static void stream_put16(uint8 *p, uint16 n)
{
    p[0] = (uint8)n;
    p[1] = (uint8)(n >> 8);
}

static uint8 *stream_header(uint32 slot)
{
    return stream.ring + STREAM_HEADER * slot;
}

static uint16 stream_status(uint32 slot)
{
    return slot < stream.count ? stream_u16(stream_header(slot)) : StFREE;
}

static uint32 stream_read_slot(void)
{
    uint32 slot = stream.read;
    if (slot >= stream.count || stream_status(slot) == StREWIND)
        slot = 0;
    return slot;
}

static void stream_drop_partial(void)
{
    uint32 i;
    for (i = 0; i < stream.part; ++i)
        stream_put16(stream_header(stream.assembly + i), StFREE);
    if (stream.part)
        stream.write = stream.assembly;
    stream.part = stream.frame = stream.chunks = 0;
}

void StClearRing(void)
{
    uint32 i;
    if (stream.ring)
        for (i = 0; i < stream.count; ++i)
            stream_put16(stream_header(i), StFREE);
    stream.read = stream.write = stream.assembly = stream.part = stream.frame = stream.chunks = 0;
    StCdIntrFlag = 0;
    stream.callback_read = stream.callback_count = 0;
    ++stream.generation;
}

void StSetRing(uint32 *ring_addr, uint32 ring_size)
{
    psx_wait_bind_service(3, stream_wait_service);
    stream.ring = (uint8 *)ring_addr;
    stream.count = ring_addr && ring_size <= UINT32_MAX / 2048u ? ring_size : 0;
    stream.payload = stream.count ? stream.ring + STREAM_HEADER * stream.count : 0;
    stream.have_back = 0;
    StClearRing();
}

static void stream_ready(uint8 status, uint8 *result)
{
    (void)result;
    if (status == 1u)
        StCdInterrupt();
}

void StSetStream(uint32 mode, uint32 start_frame, uint32 end_frame, void (*func1)(void), void (*func2)(void))
{
    psx_wait_bind_service(3, stream_wait_service);
    stream_drop_partial();
    stream.mode = mode & 1u;
    stream.start = start_frame;
    stream.end = end_frame;
    stream.mask = 1;
    stream.channel = stream.next_channel = 0;
    stream.complete = func1;
    stream.limit = func2;
    stream.callback_read = stream.callback_count = 0;
    stream.emulate = 0;
    stream.configured = 1;
    cd_stream_consumer_native(1);
    stream.have_back = 0;
    stream.ended = 0;
    if (!stream.hooked)
    {
        stream.previous_ready = CdReadyCallback(stream_ready);
        stream.hooked = 1;
    }
    ++stream.generation;
}

void StSetEmulate(uint32 *addr, uint32 mode, uint32 start_frame, uint32 end_frame, void (*func1)(void), void (*func2)(void))
{
    StSetStream(mode, start_frame, end_frame, func1, func2);
    stream.emulate = (const uint8 *)addr;
    cd_stream_consumer_native(0);
}

void StUnSetRing(void)
{
    cd_stream_consumer_native(0);
    if (stream.hooked)
        CdReadyCallback(stream.previous_ready);
    stream.hooked = 0;
    stream.configured = 0;
    stream.callback_read = stream.callback_count = 0;
    stream.emulate = 0;
    StCdIntrFlag = 0;
    ++stream.generation;
}

void StSetMask(uint32 mask, uint32 start, uint32 end)
{
    stream_drop_partial();
    stream.mask = mask;
    stream.start = start;
    stream.end = end;
    stream.ended = 0;
    ++stream.generation;
}

sint32 StSetChannel(uint32 channel)
{
    if (channel > 31u)
        return 1;
    stream.next_channel = channel;
    if (!stream.part)
        stream.channel = channel;
    return 0;
}

void stream_set_read_mode(uint32 mode)
{
    stream.read_mode = mode;
}

sint32 stream_feed_sector(const uint8 sector[2048], const CdlLOC *location)
{
    uint32 part, chunks, frame, index, i;
    uint8 *header;
    void (*callback)(void);
    if (!sector || !stream.configured || !stream.count || stream.ended)
        return 0;
    if (stream_u16(sector) != 0x160u || ((stream_u16(sector + 2) >> 10) & 31u) != stream.channel)
        return 0;
    part = stream_u16(sector + 4);
    chunks = stream_u16(sector + 6);
    frame = stream_u32(sector + 8);
    if (!chunks || part >= chunks || chunks >= stream.count)
        return -1;
    if (stream.mask == 1u && stream.start)
    {
        if (stream_u16(sector + 8) != stream.start)
            return 0;
        stream.mask = 0;
    }
    if (!part)
    {
        stream_drop_partial();
        if (stream.end && frame >= stream.end)
        {
            stream.mask = 1;
            stream.ended = 1;
            callback = stream.limit;
            if (callback)
                stream_completion(1);
            return 0;
        }
        index = stream.write;
        if (index >= stream.count || stream.count - index - 1u < chunks)
        {
            if (!stream.end)
            {
                stream.mask = 1;
                stream.ended = 1;
                callback = stream.limit;
                if (callback)
                    stream_completion(1);
                return 0;
            }
            for (i = 0; i < chunks; ++i)
                if (stream_status(i) != StFREE)
                    return -2;
            if (index < stream.count)
            {
                if (stream_status(index) != StFREE)
                    return -2;
                stream_put16(stream_header(index), StREWIND);
            }
            index = 0;
            stream.write = 0;
        }
        for (i = 0; i < chunks; ++i)
            if (stream_status(index + i) != StFREE)
                return -2;
        stream.assembly = index;
        stream.frame = frame;
        stream.chunks = chunks;
    }
    else if (!stream.part || stream.frame != frame || stream.chunks != chunks || stream.part != part)
    {
        stream_drop_partial();
        return -1;
    }
    index = stream.write;
    if (index >= stream.count || stream_status(index) != StFREE)
    {
        stream_drop_partial();
        return -2;
    }
    header = stream_header(index);
    memcpy(header, sector, STREAM_HEADER);
    if (location)
        memcpy(header + 28, location, sizeof(CdlLOC));
    else
        memset(header + 28, 0, sizeof(CdlLOC));
    memcpy(stream.payload + STREAM_PAYLOAD * index, sector + STREAM_HEADER, STREAM_PAYLOAD);
    stream_put16(header, StBUSY);
    ++stream.write;
    ++stream.part;
    if (stream.part == chunks)
    {
        header = stream_header(stream.assembly);
        stream_put16(header, StCOMPLETE);
        stream.back_frame = frame;
        if (location)
        {
            memcpy(&stream.back, header + 28, sizeof(CdlLOC));
            stream.have_back = 1;
        }
        stream.assembly = stream.write;
        stream.part = stream.frame = stream.chunks = 0;
        stream.channel = stream.next_channel;
        callback = stream.complete;
        if (callback)
            stream_completion(0);
    }
    return 1;
}

sint32 stream_pump(void)
{
    uint8 raw[2352];
    CdlLOC location;
    sint32 result;
    if (stream.pumping || !stream.configured || !stream.count || stream.ended)
        return 0;
    stream.pumping = 1;
    if (stream.emulate)
    {
        const uint8 *sector = stream.emulate;
        stream.emulate += 2048;
        result = stream_feed_sector(sector, 0);
    }
    else
    {
        sint32 available = cd_stream_pop_sector_native(raw);
        if (available > 0)
        {
            memcpy(&location, raw + 12, sizeof(CdlLOC));
            result = stream_feed_sector(raw + 24, &location);
        }
        else
            result = available < 0 ? -3 : 0;
    }
    stream.pumping = 0;
    StCdIntrFlag = 0;
    return result;
}

void StCdInterrupt(void)
{
    stream_pump();
}

uint16 StNextStatus(uint32 **addr, uint32 **header)
{
    uint32 index;
    if (!stream.count)
        return StFREE;
    index = stream_read_slot();
    if (addr)
        *addr = (uint32 *)(stream.payload + STREAM_PAYLOAD * index);
    if (header)
        *header = (uint32 *)stream_header(index);
    return stream_status(index);
}

uint32 StGetNext(uint32 **addr, uint32 **header)
{
    uint32 index;
    if (!stream.count || !addr || !header)
        return 1;
    index = stream_read_slot();
    if (stream_status(index) == StLOCK)
        return 1;
    if (stream_status(index) != StCOMPLETE)
    {
        stream_pump();
        return 1;
    }
    if (index != stream.read)
    {
        if (stream.read < stream.count)
            stream_put16(stream_header(stream.read), StFREE);
        stream.read = index;
    }
    stream_put16(stream_header(index), StLOCK);
    *addr = (uint32 *)(stream.payload + STREAM_PAYLOAD * index);
    *header = (uint32 *)stream_header(index);
    return 0;
}

uint32 StGetNextS(uint32 **addr, uint32 **header)
{
    if (!stream.emulate)
        return StGetNext(addr, header);
    if (StNextStatus(0, 0) == StLOCK)
        return 1;
    while (StNextStatus(0, 0) != StCOMPLETE)
    {
        if (stream_pump() <= 0)
            return 1;
    }
    return StGetNext(addr, header);
}

uint32 StFreeRing(uint32 *base)
{
    uintptr_t value = (uintptr_t)base, first = (uintptr_t)stream.payload;
    uint32 index, count, i;
    if (!stream.count || value < first || value - first >= (uintptr_t)STREAM_PAYLOAD * stream.count || (value - first) % STREAM_PAYLOAD)
        return 1;
    index = (uint32)((value - first) / STREAM_PAYLOAD);
    count = stream_u16(stream_header(index) + 6);
    if (stream_status(index) != StLOCK || !count || count > stream.count - index)
        return 1;
    for (i = 0; i < count; ++i)
        stream_put16(stream_header(index + i), StFREE);
    stream.read = index + count;
    return 0;
}

void StRingStatus(sint16 *free_sectors, sint16 *over_sectors)
{
    uint32 i, free_count = 0, over = 0;
    for (i = 0; i < stream.count; ++i)
    {
        uint16 status = stream_status(i);
        if (status == StFREE)
            ++free_count;
        else if (status != StREWIND)
            ++over;
    }
    if (free_sectors)
        *free_sectors = (sint16)free_count;
    if (over_sectors)
        *over_sectors = (sint16)over;
}

sint32 StGetBackloc(CdlLOC *loc)
{
    if (!(stream.read_mode & 0x20u) || !stream.have_back || !loc)
        return -1;
    CdIntToPos(CdPosToInt(&stream.back) + 1, loc);
    return (sint32)stream.back_frame;
}

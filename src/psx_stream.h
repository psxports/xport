#ifndef XPORT_PSX_STREAM_H
#define XPORT_PSX_STREAM_H
#include "psx.h"
#ifdef __cplusplus
extern "C"
{
#endif
#define StFREE 0
#define StREWIND 1
#define StCOMPLETE 2
#define StBUSY 3
#define StLOCK 4

    typedef struct
    {
        uint16 id, type, secCount, nSectors;
        uint32 frameCount, frameSize;
        uint16 width, height;
        uint32 dummy1, dummy2;
        CdlLOC loc;
    } StHEADER;

    extern sint32 StCdIntrFlag;
    void StSetRing(uint32 *ring_addr, uint32 ring_size);
    void StClearRing(void);
    void StUnSetRing(void);
    void StSetStream(uint32 mode, uint32 start_frame, uint32 end_frame, void (*func1)(void), void (*func2)(void));
    void StSetEmulate(uint32 *addr, uint32 mode, uint32 start_frame, uint32 end_frame, void (*func1)(void), void (*func2)(void));
    uint32 StFreeRing(uint32 *base);
    uint32 StGetNext(uint32 **addr, uint32 **header);
    uint32 StGetNextS(uint32 **addr, uint32 **header);
    uint16 StNextStatus(uint32 **addr, uint32 **header);
    void StRingStatus(sint16 *free_sectors, sint16 *over_sectors);
    void StSetMask(uint32 mask, uint32 start, uint32 end);
    void StCdInterrupt(void);
    sint32 StGetBackloc(CdlLOC *loc);
    sint32 StSetChannel(uint32 channel);
    /* The transport passes its actual CdRead2 mode before delivering sectors */
    void stream_set_read_mode(uint32 mode);
    /* Accept one complete 2048-byte STR user sector, with optional physical location */
    sint32 stream_feed_sector(const uint8 sector[2048], const CdlLOC *location);
    /* Consume one native physical sector or one emulated STR sector */
    sint32 stream_pump(void);
#ifdef __cplusplus
}
#endif
#endif

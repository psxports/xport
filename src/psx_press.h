#ifndef XPORT_PSX_PRESS_H
#define XPORT_PSX_PRESS_H
#include "xport.h"
#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct DECDCTENV
    {
        uint8 iq_y[64], iq_c[64];
        sint16 dct[64];
    } DECDCTENV;

    typedef uint16 DECDCTTAB[34816];
    typedef void (*DecDCTCallback)(void);
    void DecDCTReset(sint32 mode);
    DECDCTENV *DecDCTGetEnv(DECDCTENV *env);
    DECDCTENV *DecDCTPutEnv(DECDCTENV *env);
    sint32 DecDCTBufSize(uint32 *bs);
    void DecDCTin(uint32 *buf, sint32 mode);
    void DecDCTout(uint32 *buf, sint32 size);
    sint32 DecDCTinSync(sint32 mode);
    sint32 DecDCToutSync(sint32 mode);
    DecDCTCallback DecDCTinCallback(DecDCTCallback callback);
    DecDCTCallback DecDCToutCallback(DecDCTCallback callback);
    /* Advance native decoder clocks and deliver completed DMA callbacks */
    void mdec_psyq_pump(uint32 ticks);
    sint32 DecDCTvlc(uint32 *bs, uint32 *buf);
    sint32 DecDCTvlc2(uint32 *bs, uint32 *buf, DECDCTTAB table);
    sint32 DecDCTvlcSize(sint32 size);
    sint32 DecDCTvlcSize2(sint32 size);
    void DecDCTvlcBuild(uint16 *table);
#ifdef __cplusplus
}
#endif
#endif

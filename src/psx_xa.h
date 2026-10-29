#ifndef PSX_XA_H
#define PSX_XA_H
#include "xport.h"

#define XA_MAX_SECTOR_FRAMES 9408u

typedef struct XaState
{
    sint32 previous[2][2];
    sint16 ring[2][64];
    uint32 position;
    uint32 pending;
    uint32 half_phase;
} XaState;

enum
{
    XPORT_FMV_AUDIO_FRAMES = 32768
};

struct XportFmvAudioRemainder
{
    XaState decoder;
    sint16 pcm[XPORT_FMV_AUDIO_FRAMES * 2];
    uint32 pcm_frames;
    uint32 next_sector;
    uint8 file;
    uint8 channel;
};

enum
{
    XA_OK = 1,
    XA_BAD_SECTOR = -1,
    XA_UNSUPPORTED = -2,
    XA_NO_SPACE = -3
};

void xa_reset(XaState *state);
sint32 xa_decode_sector(XaState *state, const uint8 raw[2352], sint16 *stereo, uint32 capacity_frames, uint32 *produced_frames);
#endif

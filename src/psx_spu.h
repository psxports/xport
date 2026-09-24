#ifndef XPORT_PSX_SPU_H
#define XPORT_PSX_SPU_H

#include <stdio.h>
#include "psx.h"

#define SPU_RAM_SIZE 0x80000u
#define SPU_VOICE_COUNT 24
#define SPU_SAMPLE_RATE 44100

typedef enum SPU_ADSR_PHASE
{
    SPU_ADSR_OFF = 0,
    SPU_ADSR_ATTACK,
    SPU_ADSR_DECAY,
    SPU_ADSR_SUSTAIN,
    SPU_ADSR_RELEASE
} SPU_ADSR_PHASE;

#if defined(__cplusplus)
extern "C"
{
#endif

    /* Exposed for deterministic block vectors. `history1` is the immediately
 * preceding decoded sample; `history2` is the sample before it. */

    /* BEGIN GENERATED MODULE API */
    SPU_ADSR_PHASE spu_voice_phase(sint32 voice);
    sint32 spu_decode_adpcm_block(const uint8 block[16], sint16 *history1, sint16 *history2, sint16 output[28]);
    sint32 spu_upload(uint32 byte_address, const void *source, uint32 byte_count);
    sint32 spu_download(uint32 byte_address, void *destination, uint32 byte_count);
    uint64 spu_voice_sample_position(sint32 voice);
    uint32 spu_end_flags(void);
    uint16 spu_voice_envelope(sint32 voice);
    void spu_mix(sint32 *interleaved_stereo, uint32 frame_count);
    void spu_render(sint16 *interleaved_stereo, uint32 frame_count);
    int spu_state_io(FILE *file, int load);
    /* END GENERATED MODULE API */

#if defined(__cplusplus)
}
#endif

#endif

#include "psx.h"
#include "psx_spu.h"

sint32 _SpuRSetVoiceAttr(const SpuVoiceAttr *attributes, sint32 first, sint32 last, sint32 deferred)
{
    sint32 voice;
    sint32 single;
    if (!attributes)
        return -3;
    single = first == last;
    if (first < 0)
        first = 0;
    if (last >= 24)
        last = 23;
    if (first >= 24 || last < 0 || last < first)
        return -3;
    for (voice = first; voice <= last; ++voice)
    {
        SpuVoiceAttr current = *attributes;
        if (!single && !(current.voice & (1u << voice)))
            continue;
        current.voice = 1u << voice;
        SpuSetVoiceAttr(&current);
    }
    (void)deferred;
    return 0;
}

static uint32 ss_get_sequence_volume(sint16 handle, uint16 *left, uint16 *right)
{
    return spu_sequence_vm_get_sequence_volume(&spu_sequence_scheduler, handle, left, right);
}

static uint32 ss_set_sequence_volume(sint16 handle, sint16 left, sint16 right, sint16 mode)
{
    return spu_sequence_vm_set_sequence_volume(&spu_sequence_scheduler, handle, left, right, mode);
}

void _SsSndCrescendo(sint16 sequence, sint16 separation)
{
    spu_sequence_scheduler_crescendo(&spu_sequence_scheduler, sequence, separation, ss_get_sequence_volume, ss_set_sequence_volume);
}

void _SsSndDecrescendo(sint16 sequence, sint16 separation)
{
    spu_sequence_scheduler_decrescendo(&spu_sequence_scheduler, sequence, separation, ss_get_sequence_volume, ss_set_sequence_volume);
}

void _SsSndTempo(sint16 sequence, sint16 separation)
{
    spu_sequence_scheduler_tempo_ramp(&spu_sequence_scheduler, sequence, separation);
}

void _SsSndPause(sint16 sequence, sint16 separation)
{
    spu_sequence_scheduler_pause(&spu_sequence_scheduler, sequence, separation, spu_sequence_vm_sequence_key_off);
}

void _SsSndReplay(sint16 sequence, sint16 separation)
{
    spu_sequence_scheduler_replay_mode(&spu_sequence_scheduler, sequence, separation);
}

void SsSetTableSize(SpuSequenceSchedulerTrack *table, sint16 sequences, sint16 separations)
{
    spu_sequence_scheduler_set_table(&spu_sequence_scheduler, table, sequences, separations);
}

void _SsVmSeqKeyOff(sint16 sequence)
{
    spu_sequence_vm_sequence_key_off(sequence);
}

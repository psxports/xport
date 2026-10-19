#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psx_spu.h"
#include "xport_trace.h"

#if defined(XPORT_NATIVE_VM) && XPORT_NATIVE_VM
    #if !defined(XPORT_NATIVE_VAB) || !XPORT_NATIVE_VAB || !defined(XPORT_NATIVE_SEQUENCE) || !XPORT_NATIVE_SEQUENCE
        #error Native VM requires native VAB and SEQ owners
    #endif
#endif

static SpuSequenceTrack *sequence_tracks;
static SpuSequenceManager sequence_manager;
static uint32 sequence_stream_count;
static uint32 sequence_stream_tracks;

static void sequence_fail(const char *reason)
{
#ifndef NDEBUG
    fprintf(stderr, "PsyQ native SEQ: %s\n", reason);
#else
    (void)reason;
#endif
    abort();
}

uint32 spu_sequence_dispatch_note(uint32 sequence, uint32 track, uint32 note, uint32 velocity)
{
    SpuSequenceNoteCallback callback = spu_sequence_callbacks()->note;
    if (!callback)
        sequence_fail("Unbound note callback");
    return callback(sequence, track, note, velocity);
}

uint32 spu_sequence_dispatch_program(uint32 sequence, uint32 track, uint32 program)
{
    SpuSequenceEventCallback callback = spu_sequence_callbacks()->program;
    if (!callback)
        sequence_fail("Unbound program callback");
    return callback(sequence, track, program);
}

uint32 spu_sequence_dispatch_pitch(uint32 sequence, uint32 track)
{
    SpuSequencePairCallback callback = spu_sequence_callbacks()->pitch;
    if (!callback)
        sequence_fail("Unbound pitch callback");
    return callback(sequence, track);
}

uint32 spu_sequence_dispatch_meta(uint32 sequence, uint32 track, uint32 event)
{
    SpuSequenceEventCallback callback = spu_sequence_callbacks()->meta;
    if (!callback)
        sequence_fail("Unbound meta-event callback");
    return callback(sequence, track, event);
}

uint32 spu_sequence_dispatch_control(uint32 sequence, uint32 track, uint32 control)
{
    SpuSequenceEventCallback callback = spu_sequence_callbacks()->control;
    if (!callback)
        sequence_fail("Unbound control callback");
    return callback(sequence, track, control);
}

uint32 spu_sequence_dispatch_controller(uint32 sequence, uint32 track, uint8 control, uint8 value)
{
    SpuSequenceEventCallback callback;
    if (control == 121)
    {
        SpuSequencePairCallback reset = spu_sequence_callbacks()->reset_all;
        if (!reset)
            sequence_fail("Unbound reset-all callback");
        return reset(sequence, track);
    }
    switch (control)
    {
        case 6:
            callback = spu_sequence_callbacks()->data_entry;
            break;
        case 7:
            callback = spu_sequence_callbacks()->volume;
            break;
        case 10:
            callback = spu_sequence_callbacks()->pan;
            break;
        case 11:
            callback = spu_sequence_callbacks()->expression;
            break;
        case 64:
            callback = spu_sequence_callbacks()->damper;
            break;
        case 98:
            callback = spu_sequence_callbacks()->nrpn_lsb;
            break;
        case 99:
            callback = spu_sequence_callbacks()->nrpn_msb;
            break;
        case 100:
            callback = spu_sequence_callbacks()->rpn_lsb;
            break;
        case 101:
            callback = spu_sequence_callbacks()->rpn_msb;
            break;
        case 91:
            callback = spu_sequence_callbacks()->reverb;
            break;
        default:
            sequence_fail("Unsupported controller callback");
            return 0;
    }
    if (!callback)
        sequence_fail("Unbound controller callback");
    return callback(sequence, track, value);
}

// Registration tokens borrow platform callbacks and are never serialized
static struct
{
    PsxInterruptCallback irq_previous[11];
    PsxInterruptCallback irq_installed[11];
    PsxInterruptCallback vblank_previous;
    PsxInterruptCallback vblank_installed;
    uint32 vblank_owned;
} sequence_timer_bindings;

static PsxInterruptCallback sequence_timer_vblank(PsxInterruptCallback callback)
{
    // VSyncCallback returns a native intptr, not a PS1 address
    PsxInterruptCallback previous = (PsxInterruptCallback)VSyncCallback(callback);
    if (!sequence_timer_bindings.vblank_owned || previous != sequence_timer_bindings.vblank_installed)
        sequence_timer_bindings.vblank_previous = previous;
    sequence_timer_bindings.vblank_installed = callback;
    sequence_timer_bindings.vblank_owned = 1;
    return previous;
}

static void sequence_timer_interrupt(uint8 interrupt, PsxInterruptCallback callback)
{
    PsxInterruptCallback previous;
    if (interrupt >= 11 || !callback)
        sequence_fail("Invalid timer interrupt callback");
    previous = InterruptCallback(interrupt, callback);
    if (previous != sequence_timer_bindings.irq_installed[interrupt])
        sequence_timer_bindings.irq_previous[interrupt] = previous;
    sequence_timer_bindings.irq_installed[interrupt] = callback;
}

void spu_sequence_stop_timer(void)
{
    uint32 index, owned = sequence_timer_bindings.vblank_owned;
    sint32 critical;
    for (index = 0; index < 11; ++index)
        owned |= sequence_timer_bindings.irq_installed[index] != 0;
    if (!owned)
        return;
    critical = xport_bios_enter_critical();
    for (index = 0; index < 11; ++index)
    {
        PsxInterruptCallback installed = sequence_timer_bindings.irq_installed[index];
        if (installed)
        {
            PsxInterruptCallback current = InterruptCallback((sint32)index, 0);
            InterruptCallback((sint32)index, current == installed ? sequence_timer_bindings.irq_previous[index] : current);
        }
    }
    if (sequence_timer_bindings.vblank_owned)
    {
        PsxInterruptCallback current = (PsxInterruptCallback)VSyncCallback(0);
        VSyncCallback(current == sequence_timer_bindings.vblank_installed ? sequence_timer_bindings.vblank_previous : current);
    }
    memset(&sequence_timer_bindings, 0, sizeof(sequence_timer_bindings));
    if (critical)
        xport_bios_exit_critical();
}

static void sequence_timer_chain(void)
{
    PsxInterruptCallback previous = spu_sequence_clock()->previous_vblank;
    PsxInterruptCallback tick;
    if (previous)
        previous();
    tick = spu_sequence_clock()->tick;
    if (!tick)
        sequence_fail("Unbound sequence tick callback");
    tick();
}

static void sequence_timer_half(void)
{
    if (spu_sequence_clock()->phase)
    {
        PsxInterruptCallback tick = spu_sequence_clock()->tick;
        spu_sequence_clock()->phase = 0;
        if (!tick)
            sequence_fail("Unbound sequence tick callback");
        tick();
    }
    else
        spu_sequence_clock()->phase = 1;
}

void spu_sequence_set_tick_mode(sint32 mode)
{
    sint32 video = GetVideoMode();
    spu_sequence_clock()->manual = ((uint32)mode & 0x1000) != 0;
    spu_sequence_clock()->mode = spu_sequence_clock()->manual ? mode & 0xFFF : mode;
    switch (spu_sequence_clock()->mode)
    {
        case 4:
            sequence_manager.tick_rate = 50;
            spu_sequence_clock()->mode = video == 1 ? 5 : 50;
            break;
        case 1:
            sequence_manager.tick_rate = 60;
            spu_sequence_clock()->mode = video == 0 ? 5 : 60;
            break;
        case 3:
            sequence_manager.tick_rate = 120;
            break;
        case 2:
            sequence_manager.tick_rate = 240;
            break;
        case 0:
        case 5:
            sequence_manager.tick_rate = video == 1 ? 50 : 60;
            break;
        default:
            sequence_manager.tick_rate = spu_sequence_clock()->mode >= 6 ? (uint32)spu_sequence_clock()->mode : 60;
            break;
    }
}

void spu_sequence_start_timer(uint32 vblank_counter)
{
    uint32 source = 0xF2000002, rate = 0x44E8;
    sint32 setting;
    PsxInterruptCallback callback;
    spu_sequence_clock()->interrupt = 6;
    spu_sequence_clock()->direct_vblank = 0;
    spu_sequence_clock()->half_rate = 0;
    spu_sequence_clock()->previous_vblank = 0;
    setting = spu_sequence_clock()->mode;
    if (!setting)
    {
        spu_sequence_clock()->interrupt = 127;
        return;
    }
    if (setting == 3)
        rate = 0x89D0;
    else if (setting == 5)
    {
        spu_sequence_clock()->interrupt = 0;
        if (vblank_counter)
        {
            source = 0xF2000003;
            rate = 1;
        }
        else
            spu_sequence_clock()->direct_vblank = 1;
    }
    else if (setting != 2)
    {
        if (spu_sequence_clock()->manual)
            return;
        setting = spu_sequence_clock()->mode;
        if (!setting)
            sequence_fail("Sequence timer division by zero");
        if (setting < 70)
        {
            rate = (uint32)(0x204CC0 / setting);
            ++spu_sequence_clock()->half_rate;
        }
        else
            rate = (uint32)(0x409980 / setting);
    }
    xport_bios_enter_critical();
    if (spu_sequence_clock()->direct_vblank)
    {
        if (!spu_sequence_clock()->tick)
            sequence_fail("Unbound sequence tick callback");
        sequence_timer_vblank(spu_sequence_clock()->tick);
    }
    else
    {
        ResetRCnt(source);
        SetRCnt(source, (uint16)rate, 4096);
        if (!spu_sequence_clock()->interrupt)
        {
            spu_sequence_clock()->previous_vblank = sequence_timer_vblank(0);
            callback = sequence_timer_chain;
        }
        else
            callback = spu_sequence_clock()->half_rate ? sequence_timer_half : spu_sequence_clock()->tick;
        sequence_timer_interrupt(spu_sequence_clock()->interrupt, callback);
    }
    xport_bios_exit_critical();
}

void spu_sequence_stream_init(uint32 sequences, uint32 tracks)
{
    SpuSequenceTrack *tracks_new = 0;
    if (sequences || tracks)
    {
        if (!sequences || sequences > 32 || !tracks || tracks > 32767)
            sequence_fail("Invalid sequence/track counts");
        tracks_new = (SpuSequenceTrack *)calloc((size_t)sequences * tracks, sizeof(*tracks_new));
        if (!tracks_new)
            sequence_fail("Cannot allocate sequence stream table");
    }
    free(sequence_tracks);
    sequence_tracks = tracks_new;
    sequence_stream_count = sequences;
    sequence_stream_tracks = tracks;
    sequence_manager.sequence_count = (sint16)sequences;
    sequence_manager.track_count = (sint16)tracks;
}

SpuSequenceManager *spu_sequence_manager(void)
{
    return &sequence_manager;
}

void spu_sequence_reset(void)
{
    SpuSequenceManager initial = {0, 0, 60, 0, 0};
    SpuSequenceClock clock_initial = {60, 1, 0, 0, 0, 0, 127, 0};
    spu_sequence_stop_timer();
    spu_sequence_stream_init(0, 0);
    sequence_manager = initial;
    *spu_sequence_clock() = clock_initial;
    memset(spu_sequence_callbacks(), 0, sizeof(SpuSequenceCallbacks));
}

SpuSequenceTrack *spu_sequence_track(sint16 sequence, sint16 track)
{
    if (sequence < 0 || (uint32)sequence >= sequence_stream_count || track < 0 || (uint32)track >= sequence_stream_tracks)
        sequence_fail("Sequence/track outside configured table");
    return sequence_tracks + (uint32)sequence * sequence_stream_tracks + (uint32)track;
}

SpuSequenceStream *spu_sequence_stream(sint16 sequence, sint16 track)
{
    return &spu_sequence_track(sequence, track)->stream;
}

static SpuVmGuestBinding spu_vm_binding(void)
{
    SpuVmGuestBinding binding;
    spu_vm_guest_binding(&binding);
    if (!binding.context)
    {
#ifndef NDEBUG
        fprintf(stderr, "PsyQ voice manager requires the guest VM binding\n");
#endif
        abort();
    }
    return binding;
}

#if defined(XPORT_NATIVE_VAB) && XPORT_NATIVE_VAB
typedef const uint8 *VM_VAB_REF;

static SpuVabNativeView vm_vab_view(const SpuVmGuestBinding *binding)
{
    SpuVabNativeView view;
    if (!spu_vab_native_view((sint16)r_u8(binding->context + 1), &view))
    {
    #ifndef NDEBUG
        fprintf(stderr, "PsyQ native VAB selection has no open bank\n");
    #endif
        abort();
    }
    return view;
}
#else
typedef uint32 VM_VAB_REF;
#endif

static VM_VAB_REF vm_vab_header(const SpuVmGuestBinding *binding)
{
#if defined(XPORT_NATIVE_VAB) && XPORT_NATIVE_VAB
    return vm_vab_view(binding).header;
#else
    return r_u32(binding->header_pointer);
#endif
}

static VM_VAB_REF vm_vab_programs(const SpuVmGuestBinding *binding)
{
#if defined(XPORT_NATIVE_VAB) && XPORT_NATIVE_VAB
    return vm_vab_view(binding).programs;
#else
    return r_u32(binding->program_pointer);
#endif
}

static VM_VAB_REF vm_vab_tones(const SpuVmGuestBinding *binding)
{
#if defined(XPORT_NATIVE_VAB) && XPORT_NATIVE_VAB
    return vm_vab_view(binding).tones;
#else
    return r_u32(binding->tone_pointer);
#endif
}

static uint8 vm_vab_read8(VM_VAB_REF source, uint32 offset)
{
#if defined(XPORT_NATIVE_VAB) && XPORT_NATIVE_VAB
    return source[offset];
#else
    return r_u8(source + offset);
#endif
}

static uint16 vm_vab_read16(VM_VAB_REF source, uint32 offset)
{
#if defined(XPORT_NATIVE_VAB) && XPORT_NATIVE_VAB
    return xport_load_le16(source + offset);
#else
    return r_u16(source + offset);
#endif
}

#if defined(XPORT_NATIVE_VM) && XPORT_NATIVE_VM
sint32 _SsVmVSetUp(uint16 bank, sint16 program)
{
    return spu_vab_select_native(bank, program);
}
#else
sint32 _SsVmVSetUp(uint16 bank, sint16 program)
{
    SpuVmGuestBinding binding = spu_vm_binding();
    #if defined(XPORT_NATIVE_VAB) && XPORT_NATIVE_VAB
    SpuVabNativeView view;
    uint32 group;
    if (!spu_vab_native_view((sint16)bank, &view) || program < 0 || program >= view.program_bytes / 16)
        return -1;
    group = xport_load_le32(view.programs + program * 16 + 8);
    w_u8(binding.context + 1, bank);
    w_u8(binding.context + 6, program);
    w_u8(binding.context + 7, group);
    return 0;
    #else
    uint32 header, programs, tones, row;
    if (bank >= 16u || r_u8(binding.bank_flags + bank) != 1u)
        return -1;
    if (program >= r_s16(binding.program_limit))
        return -1;
    row = 4u * bank;
    header = r_u32(binding.header_table + row);
    programs = r_u32(binding.program_table + row);
    tones = r_u32(binding.tone_table + row);
    w_u8(binding.context + 1u, (uint8)bank);
    w_u8(binding.context + 6u, (uint8)program);
    w_u32(binding.tone_pointer, tones);
    row = programs + 16u * (uint32)(sint32)program;
    w_u32(binding.header_pointer, header);
    w_u32(binding.program_pointer, programs);
    w_u8(binding.context + 7u, r_u8(row + 8u));
    return 0;
    #endif
}
#endif

#if defined(XPORT_NATIVE_VM) && XPORT_NATIVE_VM
uint32 _SsVmSelectToneAndVag(uint8 *tones, uint8 *vags)
{
    uint32 count = 0u, index = 0u, row, minimum;
    VM_VAB_REF source;
    sint32 note;
    if ((sint8)spu_vm_state()->note.tone_count <= 0)
        return 0u;
    do
    {
        row = ((uint32)(sint32)(sint8)spu_vm_state()->note.program_tone_index * 16u + (uint32)(sint32)(sint8)index) * 32u;
        source = spu_vm_native_bank().tones + row;
        minimum = vm_vab_read8(source, 6u);
        note = (sint8)spu_vm_state()->note.note;
        if (note >= (sint32)minimum && (sint32)vm_vab_read8(source, 7u) >= note)
        {
            vags[count] = vm_vab_read8(source, 22u);
            tones[count] = (uint8)index;
            ++count;
        }
        ++index;
    } while ((sint32)(sint8)index < (sint8)spu_vm_state()->note.tone_count);
    return (uint8)count;
}
#else
uint32 _SsVmSelectToneAndVag(uint8 *tones, uint8 *vags)
{
    SpuVmGuestBinding binding = spu_vm_binding();
    uint32 count = 0u, index = 0u, row, minimum;
    VM_VAB_REF source;
    sint32 note;
    if (r_s8(binding.context) <= 0)
        return 0u;
    do
    {
        row = ((uint32)(sint32)r_s8(binding.context + 7u) * 16u + (uint32)(sint32)(sint8)index) * 32u;
        source = vm_vab_tones(&binding) + row;
        minimum = vm_vab_read8(source, 6u);
        note = r_s8(binding.context + 2u);
        if (note >= (sint32)minimum && (sint32)vm_vab_read8(source, 7u) >= note)
        {
            vags[count] = vm_vab_read8(source, 22u);
            tones[count] = (uint8)index;
            ++count;
        }
        ++index;
    } while ((sint32)(sint8)index < r_s8(binding.context));
    return (uint8)count;
}
#endif

#if defined(XPORT_NATIVE_VM) && XPORT_NATIVE_VM
uint32 _SsVmAlloc(void)
{
    uint32 selected = 99u, best = 99u, envelope = 0xFFFFu, ties = 0u, age = 0u;
    uint32 threshold, protection, index, raw_priority, current_envelope, current_age, priority_byte;
    SpuVoiceState *row;
    sint32 count, priority, signed_age;
    threshold = (uint32)(sint32)(sint8)spu_vm_state()->note.priority;
    count = (sint8)spu_vm_state()->events.voice_count;
    if (count > 0)
    {
        protection = spu_vm_state()->protection;
        for (index = 0u; (sint32)(uint8)index < count; ++index)
        {
            if (protection & (1u << ((uint8)index & 31u)))
                continue;
            row = spu_vm_voice((uint8)index);
            if ((sint8)row->status == 0 && row->envelope == 0u)
            {
                selected = index;
                break;
            }
            priority = (sint16)row->priority;
            raw_priority = row->priority;
            if (priority < (sint32)(uint16)threshold)
            {
                threshold = raw_priority;
                best = index;
                envelope = row->envelope;
                age = row->age;
                ties = 1u;
            }
            else if (priority == (sint32)(uint16)threshold)
            {
                current_envelope = row->envelope;
                ++ties;
                if (current_envelope < (uint16)envelope)
                {
                    age = row->age;
                    envelope = current_envelope;
                    best = index;
                }
                else if (current_envelope == (uint16)envelope)
                {
                    signed_age = (sint16)row->age;
                    current_age = row->age;
                    if ((sint32)age < signed_age)
                    {
                        age = current_age;
                        best = index;
                    }
                }
            }
        }
    }
    if ((uint8)selected == 99u)
    {
        selected = best;
        if ((uint8)ties == 0u)
            selected = spu_vm_state()->events.voice_count;
    }
    count = (sint8)spu_vm_state()->events.voice_count;
    if ((sint32)(uint8)selected < count)
    {
        if (count > 0)
        {
            protection = spu_vm_state()->protection;
            for (index = 0u; (sint32)(uint8)index < count; ++index)
            {
                if (protection & (1u << ((uint8)index & 31u)))
                    continue;
                row = spu_vm_voice((uint8)index);
                current_age = row->age;
                row->age = (uint16)((uint16)(current_age + 1u));
            }
        }
        row = spu_vm_voice((uint8)selected);
        row->age = (uint16)(0u);
        priority_byte = spu_vm_state()->note.priority;
        row->pitch_modulation[0] = (uint16)(0u);
        row->volume_modulation[0] = (uint16)(0u);
        row->priority = (uint16)((uint16)(sint16)(sint8)priority_byte);
    }
    return (uint8)selected;
}
#else
uint32 _SsVmAlloc(void)
{
    SpuVmGuestBinding binding = spu_vm_binding();
    uint32 selected = 99u, best = 99u, envelope = 0xFFFFu, ties = 0u, age = 0u;
    uint32 threshold, protection, index, row, raw_priority, current_envelope, current_age, priority_byte;
    sint32 count, priority, signed_age;
    threshold = (uint32)(sint32)r_s8(binding.context + 15u);
    count = r_s8(binding.voice_count);
    if (count > 0)
    {
        protection = r_u32(binding.protection);
        for (index = 0u; (sint32)(uint8)index < count; ++index)
        {
            if (protection & (1u << ((uint8)index & 31u)))
                continue;
            row = binding.voices + 54u * (uint8)index;
            if (r_s8(row + 29u) == 0 && r_u16(row + 6u) == 0u)
            {
                selected = index;
                break;
            }
            priority = r_s16(row + 26u);
            raw_priority = r_u16(row + 26u);
            if (priority < (sint32)(uint16)threshold)
            {
                threshold = raw_priority;
                best = index;
                envelope = r_u16(row + 6u);
                age = r_u16(row + 2u);
                ties = 1u;
            }
            else if (priority == (sint32)(uint16)threshold)
            {
                current_envelope = r_u16(row + 6u);
                ++ties;
                if (current_envelope < (uint16)envelope)
                {
                    age = r_u16(row + 2u);
                    envelope = current_envelope;
                    best = index;
                }
                else if (current_envelope == (uint16)envelope)
                {
                    signed_age = r_s16(row + 2u);
                    current_age = r_u16(row + 2u);
                    if ((sint32)age < signed_age)
                    {
                        age = current_age;
                        best = index;
                    }
                }
            }
        }
    }
    if ((uint8)selected == 99u)
    {
        selected = best;
        if ((uint8)ties == 0u)
            selected = r_u8(binding.voice_count);
    }
    count = r_s8(binding.voice_count);
    if ((sint32)(uint8)selected < count)
    {
        if (count > 0)
        {
            protection = r_u32(binding.protection);
            for (index = 0u; (sint32)(uint8)index < count; ++index)
            {
                if (protection & (1u << ((uint8)index & 31u)))
                    continue;
                row = binding.voices + 54u * (uint8)index;
                current_age = r_u16(row + 2u);
                w_u16(row + 2u, (uint16)(current_age + 1u));
            }
        }
        row = binding.voices + 54u * (uint8)selected;
        w_u16(row + 2u, 0u);
        priority_byte = r_u8(binding.context + 15u);
        w_u16(row + 42u, 0u);
        w_u16(row + 30u, 0u);
        w_u16(row + 26u, (uint16)(sint16)(sint8)priority_byte);
    }
    return (uint8)selected;
}
#endif

#if defined(XPORT_NATIVE_VM) && XPORT_NATIVE_VM
uint32 _SsVmDoAllocate(void)
{
    uint32 voice, offset, index, mask, value, vag, adsr, release, result;
    SpuVoiceState *row;
    sint32 fake_program, tone;
    VM_VAB_REF source;
    voice = spu_vm_state()->note.voice;
    offset = (uint32)((sint32)(sint16)(uint16)(voice * 8u) * 2);
    row = spu_vm_voice((uint32)(sint32)(sint16)voice);
    row->envelope = (uint16)(0x7FFFu);
    for (index = 0u; index < 16u; ++index)
    {
        voice = (uint32)(sint32)(sint16)spu_vm_state()->note.voice;
        mask = spu_vm_state()->events.silence_history[index];
        spu_vm_state()->events.silence_history[index] = (uint32)(mask & ~(1u << (voice & 31u)));
    }
    vag = spu_vm_state()->note.sample;
    source = (uint32)(((sint32)(sint16)vag - 1) / 2) * 16u + spu_vm_native_bank().programs;
    value = vm_vab_read16(source, (vag & 1u) ? 12u : 14u);
    spu_vm_state()->registers[offset / 2 + 3] = (uint16)((uint16)value);
    voice = (uint32)(sint32)(sint16)spu_vm_state()->note.voice;
    value = spu_vm_state()->dirty[voice];
    spu_vm_state()->dirty[voice] = (uint8)((uint8)(value | 8u));
    fake_program = (sint8)spu_vm_state()->note.program_tone_index;
    tone = (sint8)spu_vm_state()->note.tone;
    source = (uint32)(fake_program * 16 + tone) * 32u + spu_vm_native_bank().tones;
    value = vm_vab_read16(source, 16u);
    spu_vm_state()->registers[offset / 2 + 4] = (uint16)((uint16)value);
    adsr = vm_vab_read16(source, 18u);
    release = spu_vm_state()->release_offset + (adsr & 31u);
    if ((sint16)(uint16)release >= 32)
        release = 31u;
    spu_vm_state()->registers[offset / 2 + 5] = (uint16)((uint16)(release | (adsr & 0xFFE0u)));
    voice = (uint32)(sint32)(sint16)spu_vm_state()->note.voice;
    result = spu_vm_state()->dirty[voice] | 0x30u;
    spu_vm_state()->dirty[voice] = (uint8)((uint8)result);
    return result;
}
#else
uint32 _SsVmDoAllocate(void)
{
    SpuVmGuestBinding binding = spu_vm_binding();
    uint32 values, flags, voice, offset, row, index, mask, value, vag, adsr, release, result;
    sint32 fake_program, tone;
    VM_VAB_REF source;
    spu_voice_cache_binding(&values, &flags);
    if (!values || !flags)
    {
    #ifndef NDEBUG
        fprintf(stderr, "_SsVmDoAllocate requires the libsnd voice cache binding\n");
    #endif
        abort();
    }
    voice = r_u16(binding.context + 24u);
    offset = (uint32)((sint32)(sint16)(uint16)(voice * 8u) * 2);
    row = binding.voices + 54u * (uint32)(sint32)(sint16)voice;
    w_u16(row + 6u, 0x7FFFu);
    for (index = 0u; index < 16u; ++index)
    {
        voice = (uint32)(sint32)r_s16(binding.context + 24u);
        mask = r_u32(binding.channel_masks + 4u * index);
        w_u32(binding.channel_masks + 4u * index, mask & ~(1u << (voice & 31u)));
    }
    vag = r_u16(binding.context + 22u);
    source = (uint32)(((sint32)(sint16)vag - 1) / 2) * 16u + vm_vab_programs(&binding);
    value = vm_vab_read16(source, (vag & 1u) ? 12u : 14u);
    w_u16(values + offset + 6u, (uint16)value);
    voice = (uint32)(sint32)r_s16(binding.context + 24u);
    value = r_u8(flags + voice);
    w_u8(flags + voice, (uint8)(value | 8u));
    fake_program = r_s8(binding.context + 7u);
    tone = r_s8(binding.context + 12u);
    source = (uint32)(fake_program * 16 + tone) * 32u + vm_vab_tones(&binding);
    value = vm_vab_read16(source, 16u);
    w_u16(values + offset + 8u, (uint16)value);
    adsr = vm_vab_read16(source, 18u);
    release = r_u16(binding.release_offset) + (adsr & 31u);
    if ((sint16)(uint16)release >= 32)
        release = 31u;
    w_u16(values + offset + 10u, (uint16)(release | (adsr & 0xFFE0u)));
    voice = (uint32)(sint32)r_s16(binding.context + 24u);
    result = r_u8(flags + voice) | 0x30u;
    w_u8(flags + voice, (uint8)result);
    return result;
}
#endif

static void vmNoiseOn(uint8 voice)
{
#ifndef NDEBUG
    fprintf(stderr, "Unimplemented PsyQ vmNoiseOn voice %u\n", voice);
#endif
    abort();
}

#if defined(XPORT_NATIVE_VM) && XPORT_NATIVE_VM
uint32 note2pitch(void)
{
    sint32 note = (sint8)spu_vm_state()->note.note;
    uint8 center = spu_vm_state()->note.center_note;
    uint8 shift = spu_vm_state()->note.fine_tune;
    return SsPitchFromNote(note, 0, center, shift);
}
#else
uint32 note2pitch(void)
{
    SpuVmGuestBinding binding = spu_vm_binding();
    sint32 note = r_s8(binding.context + 2u);
    uint8 center = r_u8(binding.context + 16u);
    uint8 shift = r_u8(binding.context + 17u);
    return SsPitchFromNote(note, 0, center, shift);
}
#endif

#if defined(XPORT_NATIVE_SEQUENCE) && XPORT_NATIVE_SEQUENCE
    #if defined(XPORT_NATIVE_VM) && XPORT_NATIVE_VM
uint32 _SsVmKeyOnNow(uint16 pitch)
{
    uint32 volume, left, right, voice, offset;
    SpuSequenceTrack *row;
    uint32 pan, low_mask, high_mask, low, high, key_high, off_high, key_low, result;
    sint32 velocity, program_volume, tone_volume;
    sint16 identity, initial_voice;
    VM_VAB_REF header;
    header = spu_vm_native_bank().header;
    volume = vm_vab_read8(header, 24u);
    velocity = (sint8)spu_vm_state()->note.velocity;
    volume = (uint32)(velocity * (sint32)(volume * 0x3FFFu));
    program_volume = (sint8)spu_vm_state()->note.program_volume;
    volume = (uint32)((sint32)volume / 16129);
    volume *= (uint32)program_volume;
    tone_volume = (sint8)spu_vm_state()->note.tone_volume;
    volume *= (uint32)tone_volume;
    initial_voice = (sint16)spu_vm_state()->note.voice;
    offset = 2u * (uint16)((uint32)(sint32)initial_voice * 8u);
    volume /= 16129u;
    identity = (sint16)spu_vm_state()->note.sequence;
    row = identity == 33 ? 0 : spu_sequence_track((sint16)((uint16)identity & 255), (sint16)(((uint16)identity & 0xFF00) >> 8));
    left = volume;
    right = volume;
    if (identity != 33)
    {
        low = row->volume_left;
        left *= low;
        high = row->volume_right;
        right *= high;
        left /= 127u;
        right /= 127u;
    }
    pan = spu_vm_state()->note.tone_pan;
    if (pan < 64u)
        right = (right * pan) / 63u;
    else
        left = (left * (127u - pan)) / 63u;
    pan = spu_vm_state()->note.program_pan;
    if (pan < 64u)
        right = (right * pan) / 63u;
    else
        left = (left * (127u - pan)) / 63u;
    pan = spu_vm_state()->note.pan;
    if (pan < 64u)
        right = (right * pan) / 63u;
    else
        left = (left * (127u - pan)) / 63u;
    if ((sint16)spu_vm_state()->mono_mode == 1)
    {
        if (left < right)
            left = right;
        else
            right = left;
    }
    if ((sint16)spu_vm_state()->note.sequence != 33)
    {
        left = (left * left) / 0x3FFFu;
        right = (right * right) / 0x3FFFu;
    }
    spu_vm_state()->registers[offset / 2 + 2] = (uint16)(pitch);
    spu_vm_state()->registers[offset / 2] = (uint16)((uint16)left);
    spu_vm_state()->registers[offset / 2 + 1] = (uint16)((uint16)right);
    voice = (uint32)(sint32)(sint16)spu_vm_state()->note.voice;
    spu_vm_state()->dirty[voice] = (uint8)(spu_vm_state()->dirty[voice] | 7u);
    voice = (uint32)(sint32)(sint16)spu_vm_state()->note.voice;
    spu_vm_voice(voice)->pitch = (uint16)(pitch);
    voice = (uint32)(sint32)(sint16)spu_vm_state()->note.voice;
    if ((sint32)voice < 16)
    {
        low_mask = 1u << (voice & 31u);
        high_mask = 0u;
    }
    else
    {
        low_mask = 0u;
        high_mask = 1u << ((voice - 16u) & 31u);
    }
    if (spu_vm_state()->note.tone_mode & 4u)
    {
        low = spu_vm_state()->events.reverb[0];
        high = spu_vm_state()->events.reverb[1];
        spu_vm_state()->events.reverb[0] = (uint16)((uint16)(low | low_mask));
        spu_vm_state()->events.reverb[1] = (uint16)((uint16)(high | high_mask));
    }
    else
    {
        low = spu_vm_state()->events.reverb[0];
        spu_vm_state()->events.reverb[0] = (uint16)((uint16)(low & ~low_mask));
        high = spu_vm_state()->events.reverb[1];
        spu_vm_state()->events.reverb[1] = (uint16)((uint16)(high & ~high_mask));
    }
    low = spu_vm_state()->events.noise[0];
    key_high = spu_vm_state()->events.key_on[1];
    spu_vm_state()->events.noise[0] = (uint16)((uint16)(low & ~low_mask));
    off_high = spu_vm_state()->events.noise[1] & ~high_mask;
    key_low = spu_vm_state()->events.key_on[0];
    key_high |= high_mask;
    spu_vm_state()->events.key_on[1] = (uint16)((uint16)key_high);
    spu_vm_state()->events.noise[1] = (uint16)((uint16)off_high);
    low = spu_vm_state()->events.key_off[0];
    key_low |= low_mask;
    spu_vm_state()->events.key_on[0] = (uint16)((uint16)key_low);
    spu_vm_state()->events.key_off[0] = (uint16)((uint16)(low & ~key_low));
    result = spu_vm_state()->events.key_off[1] & ~key_high;
    spu_vm_state()->events.key_off[1] = (uint16)((uint16)result);
    return result;
}
    #else
uint32 _SsVmKeyOnNow(uint16 pitch)
{
    SpuVmGuestBinding binding = spu_vm_binding();
    uint32 values, flags_address, volume, left, right, voice, offset;
    SpuSequenceTrack *row;
    uint32 pan, low_mask, high_mask, low, high, key_high, off_high, key_low, result;
    sint32 velocity, program_volume, tone_volume;
    sint16 identity, initial_voice;
    VM_VAB_REF header;
    spu_voice_cache_binding(&values, &flags_address);
    if (!values || !flags_address)
    {
        #ifndef NDEBUG
        fprintf(stderr, "_SsVmKeyOnNow requires libsnd cache and sequence bindings\n");
        #endif
        abort();
    }
    header = vm_vab_header(&binding);
    volume = vm_vab_read8(header, 24u);
    velocity = r_s8(binding.context + 4u);
    volume = (uint32)(velocity * (sint32)(volume * 0x3FFFu));
    program_volume = r_s8(binding.context + 10u);
    volume = (uint32)((sint32)volume / 16129);
    volume *= (uint32)program_volume;
    tone_volume = r_s8(binding.context + 13u);
    volume *= (uint32)tone_volume;
    initial_voice = r_s16(binding.context + 24u);
    offset = 2u * (uint16)((uint32)(sint32)initial_voice * 8u);
    volume /= 16129u;
    identity = (sint16)r_u16(binding.context + 20u);
    row = identity == 33 ? 0 : spu_sequence_track((sint16)((uint16)identity & 255), (sint16)(((uint16)identity & 0xFF00) >> 8));
    left = volume;
    right = volume;
    if (identity != 33)
    {
        low = row->volume_left;
        left *= low;
        high = row->volume_right;
        right *= high;
        left /= 127u;
        right /= 127u;
    }
    pan = r_u8(binding.context + 14u);
    if (pan < 64u)
        right = (right * pan) / 63u;
    else
        left = (left * (127u - pan)) / 63u;
    pan = r_u8(binding.context + 11u);
    if (pan < 64u)
        right = (right * pan) / 63u;
    else
        left = (left * (127u - pan)) / 63u;
    pan = r_u8(binding.context + 5u);
    if (pan < 64u)
        right = (right * pan) / 63u;
    else
        left = (left * (127u - pan)) / 63u;
    if (r_s16(binding.mono_mode) == 1)
    {
        if (left < right)
            left = right;
        else
            right = left;
    }
    if (r_s16(binding.context + 20u) != 33)
    {
        left = (left * left) / 0x3FFFu;
        right = (right * right) / 0x3FFFu;
    }
    w_u16(values + offset + 4u, pitch);
    w_u16(values + offset, (uint16)left);
    w_u16(values + offset + 2u, (uint16)right);
    voice = (uint32)(sint32)r_s16(binding.context + 24u);
    w_u8(flags_address + voice, r_u8(flags_address + voice) | 7u);
    voice = (uint32)(sint32)r_s16(binding.context + 24u);
    w_u16(binding.voices + 54u * voice + 4u, pitch);
    voice = (uint32)(sint32)r_s16(binding.context + 24u);
    if ((sint32)voice < 16)
    {
        low_mask = 1u << (voice & 31u);
        high_mask = 0u;
    }
    else
    {
        low_mask = 0u;
        high_mask = 1u << ((voice - 16u) & 31u);
    }
    if (r_u8(binding.context + 18u) & 4u)
    {
        low = r_u16(binding.voice_masks + 4u);
        high = r_u16(binding.voice_masks + 44u);
        w_u16(binding.voice_masks + 4u, (uint16)(low | low_mask));
        w_u16(binding.voice_masks + 44u, (uint16)(high | high_mask));
    }
    else
    {
        low = r_u16(binding.voice_masks + 4u);
        w_u16(binding.voice_masks + 4u, (uint16)(low & ~low_mask));
        high = r_u16(binding.voice_masks + 44u);
        w_u16(binding.voice_masks + 44u, (uint16)(high & ~high_mask));
    }
    low = r_u16(binding.voice_masks + 46u);
    key_high = r_u16(binding.voice_masks + 2u);
    w_u16(binding.voice_masks + 46u, (uint16)(low & ~low_mask));
    off_high = r_u16(binding.voice_masks + 48u) & ~high_mask;
    key_low = r_u16(binding.voice_masks);
    key_high |= high_mask;
    w_u16(binding.voice_masks + 2u, (uint16)key_high);
    w_u16(binding.voice_masks + 48u, (uint16)off_high);
    low = r_u16(binding.pending_keyoff);
    key_low |= low_mask;
    w_u16(binding.voice_masks, (uint16)key_low);
    w_u16(binding.pending_keyoff, (uint16)(low & ~key_low));
    result = r_u16(binding.pending_keyoff + 2u) & ~key_high;
    w_u16(binding.pending_keyoff + 2u, (uint16)result);
    return result;
}
    #endif
#else
uint32 _SsVmKeyOnNow(uint16 pitch)
{
    SpuVmGuestBinding binding = spu_vm_binding();
    uint32 values, flags_address, table, volume, left, right, row, voice, offset;
    uint32 pan, low_mask, high_mask, low, high, key_high, off_high, key_low, result;
    sint32 velocity, program_volume, tone_volume;
    sint16 identity, initial_voice;
    VM_VAB_REF header;
    spu_voice_cache_binding(&values, &flags_address);
    spu_sequence_tables(&table, NULL);
    if (!values || !flags_address || !table)
    {
    #ifndef NDEBUG
        fprintf(stderr, "_SsVmKeyOnNow requires libsnd cache and sequence bindings\n");
    #endif
        abort();
    }
    header = vm_vab_header(&binding);
    volume = vm_vab_read8(header, 24u);
    velocity = r_s8(binding.context + 4u);
    volume = (uint32)(velocity * (sint32)(volume * 0x3FFFu));
    program_volume = r_s8(binding.context + 10u);
    volume = (uint32)((sint32)volume / 16129);
    volume *= (uint32)program_volume;
    tone_volume = r_s8(binding.context + 13u);
    volume *= (uint32)tone_volume;
    initial_voice = r_s16(binding.context + 24u);
    offset = 2u * (uint16)((uint32)(sint32)initial_voice * 8u);
    volume /= 16129u;
    identity = (sint16)r_u16(binding.context + 20u);
    row = r_u32(table + 4u * (uint8)identity) + 176u * (((uint16)identity & 0xFF00u) >> 8);
    left = volume;
    right = volume;
    if (identity != 33)
    {
        low = r_u16(row + 88u);
        left *= low;
        high = r_u16(row + 90u);
        right *= high;
        left /= 127u;
        right /= 127u;
    }
    pan = r_u8(binding.context + 14u);
    if (pan < 64u)
        right = (right * pan) / 63u;
    else
        left = (left * (127u - pan)) / 63u;
    pan = r_u8(binding.context + 11u);
    if (pan < 64u)
        right = (right * pan) / 63u;
    else
        left = (left * (127u - pan)) / 63u;
    pan = r_u8(binding.context + 5u);
    if (pan < 64u)
        right = (right * pan) / 63u;
    else
        left = (left * (127u - pan)) / 63u;
    if (r_s16(binding.mono_mode) == 1)
    {
        if (left < right)
            left = right;
        else
            right = left;
    }
    if (r_s16(binding.context + 20u) != 33)
    {
        left = (left * left) / 0x3FFFu;
        right = (right * right) / 0x3FFFu;
    }
    w_u16(values + offset + 4u, pitch);
    w_u16(values + offset, (uint16)left);
    w_u16(values + offset + 2u, (uint16)right);
    voice = (uint32)(sint32)r_s16(binding.context + 24u);
    w_u8(flags_address + voice, r_u8(flags_address + voice) | 7u);
    voice = (uint32)(sint32)r_s16(binding.context + 24u);
    w_u16(binding.voices + 54u * voice + 4u, pitch);
    voice = (uint32)(sint32)r_s16(binding.context + 24u);
    if ((sint32)voice < 16)
    {
        low_mask = 1u << (voice & 31u);
        high_mask = 0u;
    }
    else
    {
        low_mask = 0u;
        high_mask = 1u << ((voice - 16u) & 31u);
    }
    if (r_u8(binding.context + 18u) & 4u)
    {
        low = r_u16(binding.voice_masks + 4u);
        high = r_u16(binding.voice_masks + 44u);
        w_u16(binding.voice_masks + 4u, (uint16)(low | low_mask));
        w_u16(binding.voice_masks + 44u, (uint16)(high | high_mask));
    }
    else
    {
        low = r_u16(binding.voice_masks + 4u);
        w_u16(binding.voice_masks + 4u, (uint16)(low & ~low_mask));
        high = r_u16(binding.voice_masks + 44u);
        w_u16(binding.voice_masks + 44u, (uint16)(high & ~high_mask));
    }
    low = r_u16(binding.voice_masks + 46u);
    key_high = r_u16(binding.voice_masks + 2u);
    w_u16(binding.voice_masks + 46u, (uint16)(low & ~low_mask));
    off_high = r_u16(binding.voice_masks + 48u) & ~high_mask;
    key_low = r_u16(binding.voice_masks);
    key_high |= high_mask;
    w_u16(binding.voice_masks + 2u, (uint16)key_high);
    w_u16(binding.voice_masks + 48u, (uint16)off_high);
    low = r_u16(binding.pending_keyoff);
    key_low |= low_mask;
    w_u16(binding.voice_masks, (uint16)key_low);
    w_u16(binding.pending_keyoff, (uint16)(low & ~key_low));
    result = r_u16(binding.pending_keyoff + 2u) & ~key_high;
    w_u16(binding.pending_keyoff + 2u, (uint16)result);
    return result;
}
#endif

static uint32 spu_vm_voice_row(const SpuVmGuestBinding *binding)
{
    return binding->voices + 54u * (uint32)(sint32)r_s16(binding->context + 24u);
}

#if defined(XPORT_NATIVE_SEQUENCE) && XPORT_NATIVE_SEQUENCE
    #if defined(XPORT_NATIVE_VM) && XPORT_NATIVE_VM
uint32 _SsVmKeyOn(sint16 identity, sint16 bank, sint16 program, uint16 note, uint16 velocity, uint16 volume)
{
    uint8 tones[128], vags[128], tone_count;
    uint32 row, value, candidate, index, pitch, result = 0u;
    SpuSequenceTrack *record;
    sint32 limit;
    VM_VAB_REF source;
    SpuNoteContext *context = &spu_vm_state()->note;
    SpuVoiceState *voice_row;
    record = identity == 33 ? 0 : spu_sequence_track((sint16)((uint16)identity & 255), (sint16)(((uint16)identity & 0xFF00) >> 8));
    if (_SsVmVSetUp((uint16)bank, program) != 0)
        return 0xFFFFFFFFu;
    context->sequence = (uint16)((uint16)identity);
    context->note = (uint8)((uint8)note);
    context->note_fraction = (uint8)(0u);
    if (identity == 33)
        value = velocity;
    else
        value = (uint32)((sint32)velocity * (sint32)(sint16)record->channel_volume[record->channel] / 127);
    context->velocity = (uint8)((uint8)value);
    context->pan = (uint8)((uint8)volume);
    source = spu_vm_native_bank().programs + 16u * (uint32)(sint32)program;
    context->program_volume = (uint8)(vm_vab_read8(source, 1u));
    context->program_pan = (uint8)(vm_vab_read8(source, 4u));
    context->tone_count = (uint8)(vm_vab_read8(source, 0u));
    source = spu_vm_native_bank().header;
    value = (uint32)(sint32)(sint8)context->program_tone_index;
    if ((sint32)value >= (sint32)vm_vab_read16(source, 18u))
        return 0xFFFFFFFFu;
    if (velocity == 0u)
        return _SsVmKeyOff(identity, bank, program, note);
    tone_count = (uint8)_SsVmSelectToneAndVag(tones, vags);
    for (index = 0u; (uint8)index < tone_count; ++index)
    {
        context->sample = (uint16)(vags[(uint8)index]);
        value = tones[(uint8)index];
        row = (uint16)((sint32)(sint8)value + 16 * (sint32)(sint8)context->program_tone_index);
        context->tone = (uint8)((uint8)value);
        source = row * 32u + spu_vm_native_bank().tones;
        context->priority = (uint8)(vm_vab_read8(source, 0u));
        context->tone_volume = (uint8)(vm_vab_read8(source, 2u));
        context->tone_pan = (uint8)(vm_vab_read8(source, 3u));
        context->center_note = (uint8)(vm_vab_read8(source, 4u));
        context->fine_tune = (uint8)(vm_vab_read8(source, 5u));
        context->tone_mode = (uint8)(vm_vab_read8(source, 1u));
        candidate = (uint8)_SsVmAlloc();
        limit = (sint8)spu_vm_state()->events.voice_count;
        context->voice = (uint16)((uint16)candidate);
        if ((sint32)candidate >= limit)
        {
            result = 0xFFFFFFFFu;
            continue;
        }
        voice_row = spu_vm_voice(spu_vm_state()->note.voice);
        voice_row->status = 1;
        voice_row->age = (uint16)(0u);
        voice_row->sequence = (uint16)((uint16)identity);
        value = (uint32)(sint32)(sint8)context->bank;
        voice_row->bank = (uint16)((uint16)value);
        value = (uint32)(sint32)(sint8)context->program_tone_index;
        voice_row->program_tone_index = (uint16)((uint16)value);
        voice_row->program = (uint16)((uint16)program);
        if (identity != 33)
        {
            voice_row->velocity = (uint16)(velocity);
            value = record->channel;
            voice_row->channel = (uint16)((uint16)value);
        }
        voice_row->pan = (uint8)((uint8)volume);
        value = (uint32)(sint32)(sint8)context->tone;
        voice_row->tone = (uint16)((uint16)value);
        voice_row->note = (uint16)(note);
        value = (uint32)(sint32)(sint8)context->priority;
        voice_row->priority = (uint16)((uint16)value);
        value = context->sample;
        voice_row->sample = (uint16)((uint16)value);
        _SsVmDoAllocate();
        if ((sint16)context->sample == 255)
            vmNoiseOn((uint8)context->voice);
        else
        {
            pitch = note2pitch();
            _SsVmKeyOnNow((uint16)pitch);
        }
        result |= 1u << ((uint32)(sint32)(sint16)context->voice & 31u);
    }
    return result;
}
    #else
uint32 _SsVmKeyOn(sint16 identity, sint16 bank, sint16 program, uint16 note, uint16 velocity, uint16 volume)
{
    SpuVmGuestBinding binding = spu_vm_binding();
    uint8 tones[128], vags[128], tone_count;
    uint32 context = binding.context, row, value, candidate, index, pitch, result = 0u;
    SpuSequenceTrack *record;
    sint32 limit;
    VM_VAB_REF source;
    record = identity == 33 ? 0 : spu_sequence_track((sint16)((uint16)identity & 255), (sint16)(((uint16)identity & 0xFF00) >> 8));
    if (_SsVmVSetUp((uint16)bank, program) != 0)
        return 0xFFFFFFFFu;
    w_u16(context + 20u, (uint16)identity);
    w_u8(context + 2u, (uint8)note);
    w_u8(context + 3u, 0u);
    if (identity == 33)
        value = velocity;
    else
        value = (uint32)((sint32)velocity * (sint32)(sint16)record->channel_volume[record->channel] / 127);
    w_u8(context + 4u, (uint8)value);
    w_u8(context + 5u, (uint8)volume);
    source = vm_vab_programs(&binding) + 16u * (uint32)(sint32)program;
    w_u8(context + 10u, vm_vab_read8(source, 1u));
    w_u8(context + 11u, vm_vab_read8(source, 4u));
    w_u8(context, vm_vab_read8(source, 0u));
    source = vm_vab_header(&binding);
    value = (uint32)(sint32)r_s8(context + 7u);
    if ((sint32)value >= (sint32)vm_vab_read16(source, 18u))
        return 0xFFFFFFFFu;
    if (velocity == 0u)
        return _SsVmKeyOff(identity, bank, program, note);
    tone_count = (uint8)_SsVmSelectToneAndVag(tones, vags);
    for (index = 0u; (uint8)index < tone_count; ++index)
    {
        w_u16(context + 22u, vags[(uint8)index]);
        value = tones[(uint8)index];
        row = (uint16)((sint32)(sint8)value + 16 * (sint32)r_s8(context + 7u));
        w_u8(context + 12u, (uint8)value);
        source = row * 32u + vm_vab_tones(&binding);
        w_u8(context + 15u, vm_vab_read8(source, 0u));
        w_u8(context + 13u, vm_vab_read8(source, 2u));
        w_u8(context + 14u, vm_vab_read8(source, 3u));
        w_u8(context + 16u, vm_vab_read8(source, 4u));
        w_u8(context + 17u, vm_vab_read8(source, 5u));
        w_u8(context + 18u, vm_vab_read8(source, 1u));
        candidate = (uint8)_SsVmAlloc();
        limit = r_s8(binding.voice_count);
        w_u16(context + 24u, (uint16)candidate);
        if ((sint32)candidate >= limit)
        {
            result = 0xFFFFFFFFu;
            continue;
        }
        w_u8(binding.voices + 54u * candidate + 29u, 1u);
        row = spu_vm_voice_row(&binding);
        w_u16(row + 2u, 0u);
        row = spu_vm_voice_row(&binding);
        w_u16(row + 16u, (uint16)identity);
        value = (uint32)(sint32)r_s8(context + 1u);
        row = spu_vm_voice_row(&binding);
        w_u16(row + 24u, (uint16)value);
        value = (uint32)(sint32)r_s8(context + 7u);
        row = spu_vm_voice_row(&binding);
        w_u16(row + 18u, (uint16)value);
        row = spu_vm_voice_row(&binding);
        w_u16(row + 20u, (uint16)program);
        if (identity != 33)
        {
            row = spu_vm_voice_row(&binding);
            w_u16(row + 8u, velocity);
            row = spu_vm_voice_row(&binding);
            value = record->channel;
            w_u16(row + 12u, (uint16)value);
        }
        row = spu_vm_voice_row(&binding);
        w_u8(row + 10u, (uint8)volume);
        value = (uint32)(sint32)r_s8(context + 12u);
        row = spu_vm_voice_row(&binding);
        w_u16(row + 22u, (uint16)value);
        row = spu_vm_voice_row(&binding);
        w_u16(row + 14u, note);
        value = (uint32)(sint32)r_s8(context + 15u);
        row = spu_vm_voice_row(&binding);
        w_u16(row + 26u, (uint16)value);
        row = spu_vm_voice_row(&binding);
        value = r_u16(context + 22u);
        w_u16(row, (uint16)value);
        _SsVmDoAllocate();
        if (r_s16(context + 22u) == 255)
            vmNoiseOn(r_u8(context + 24u));
        else
        {
            pitch = note2pitch();
            _SsVmKeyOnNow((uint16)pitch);
        }
        result |= 1u << ((uint32)(sint32)r_s16(context + 24u) & 31u);
    }
    return result;
}
    #endif
#else
uint32 _SsVmKeyOn(sint16 identity, sint16 bank, sint16 program, uint16 note, uint16 velocity, uint16 volume)
{
    SpuVmGuestBinding binding = spu_vm_binding();
    uint8 tones[128], vags[128], tone_count;
    uint32 table, record, context = binding.context, row, value, candidate, index, pitch, result = 0u;
    sint32 limit;
    VM_VAB_REF source;
    spu_sequence_tables(&table, NULL);
    if (!table)
    {
    #ifndef NDEBUG
        fprintf(stderr, "_SsVmKeyOn requires the libsnd sequence table binding\n");
    #endif
        abort();
    }
    record = r_u32(table + 4u * ((uint16)identity & 0xFFu)) + 176u * (((uint16)identity & 0xFF00u) >> 8);
    if (_SsVmVSetUp((uint16)bank, program) != 0)
        return 0xFFFFFFFFu;
    w_u16(context + 20u, (uint16)identity);
    w_u8(context + 2u, (uint8)note);
    w_u8(context + 3u, 0u);
    if (identity == 33)
        value = velocity;
    else
        value = (uint32)((sint32)velocity * (sint32)r_s16(record + 96u + 2u * r_u8(record + 23u)) / 127);
    w_u8(context + 4u, (uint8)value);
    w_u8(context + 5u, (uint8)volume);
    source = vm_vab_programs(&binding) + 16u * (uint32)(sint32)program;
    w_u8(context + 10u, vm_vab_read8(source, 1u));
    w_u8(context + 11u, vm_vab_read8(source, 4u));
    w_u8(context, vm_vab_read8(source, 0u));
    source = vm_vab_header(&binding);
    value = (uint32)(sint32)r_s8(context + 7u);
    if ((sint32)value >= (sint32)vm_vab_read16(source, 18u))
        return 0xFFFFFFFFu;
    if (velocity == 0u)
        return _SsVmKeyOff(identity, bank, program, note);
    tone_count = (uint8)_SsVmSelectToneAndVag(tones, vags);
    for (index = 0u; (uint8)index < tone_count; ++index)
    {
        w_u16(context + 22u, vags[(uint8)index]);
        value = tones[(uint8)index];
        row = (uint16)((sint32)(sint8)value + 16 * (sint32)r_s8(context + 7u));
        w_u8(context + 12u, (uint8)value);
        source = row * 32u + vm_vab_tones(&binding);
        w_u8(context + 15u, vm_vab_read8(source, 0u));
        w_u8(context + 13u, vm_vab_read8(source, 2u));
        w_u8(context + 14u, vm_vab_read8(source, 3u));
        w_u8(context + 16u, vm_vab_read8(source, 4u));
        w_u8(context + 17u, vm_vab_read8(source, 5u));
        w_u8(context + 18u, vm_vab_read8(source, 1u));
        candidate = (uint8)_SsVmAlloc();
        limit = r_s8(binding.voice_count);
        w_u16(context + 24u, (uint16)candidate);
        if ((sint32)candidate >= limit)
        {
            result = 0xFFFFFFFFu;
            continue;
        }
        w_u8(binding.voices + 54u * candidate + 29u, 1u);
        row = spu_vm_voice_row(&binding);
        w_u16(row + 2u, 0u);
        row = spu_vm_voice_row(&binding);
        w_u16(row + 16u, (uint16)identity);
        value = (uint32)(sint32)r_s8(context + 1u);
        row = spu_vm_voice_row(&binding);
        w_u16(row + 24u, (uint16)value);
        value = (uint32)(sint32)r_s8(context + 7u);
        row = spu_vm_voice_row(&binding);
        w_u16(row + 18u, (uint16)value);
        row = spu_vm_voice_row(&binding);
        w_u16(row + 20u, (uint16)program);
        if (identity != 33)
        {
            row = spu_vm_voice_row(&binding);
            w_u16(row + 8u, velocity);
            row = spu_vm_voice_row(&binding);
            value = r_u8(record + 23u);
            w_u16(row + 12u, (uint16)value);
        }
        row = spu_vm_voice_row(&binding);
        w_u8(row + 10u, (uint8)volume);
        value = (uint32)(sint32)r_s8(context + 12u);
        row = spu_vm_voice_row(&binding);
        w_u16(row + 22u, (uint16)value);
        row = spu_vm_voice_row(&binding);
        w_u16(row + 14u, note);
        value = (uint32)(sint32)r_s8(context + 15u);
        row = spu_vm_voice_row(&binding);
        w_u16(row + 26u, (uint16)value);
        row = spu_vm_voice_row(&binding);
        value = r_u16(context + 22u);
        w_u16(row, (uint16)value);
        _SsVmDoAllocate();
        if (r_s16(context + 22u) == 255)
            vmNoiseOn(r_u8(context + 24u));
        else
        {
            pitch = note2pitch();
            _SsVmKeyOnNow((uint16)pitch);
        }
        result |= 1u << ((uint32)(sint32)r_s16(context + 24u) & 31u);
    }
    return result;
}
#endif

uint32 SsVoKeyOff(uint32 voice_program, uint32 note_volume)
{
    sint16 bank = (sint16)(voice_program >> 8);
    sint16 program = (uint8)voice_program;
    uint16 note = (uint16)(note_volume >> 8);
    return _SsVmKeyOff(33, bank, program, note);
}

#if defined(XPORT_NATIVE_VM) && XPORT_NATIVE_VM
uint32 _SsVmKeyOff(sint16 identity, sint16 bank, sint16 program, uint16 note)
{
    uint32 index = 0u, matched = 0u;
    SpuVoiceState *row;
    if ((sint8)spu_vm_state()->events.voice_count <= 0)
        return 0u;
    do
    {
        if (!(spu_vm_state()->protection & (1u << ((uint8)index & 31u))))
        {
            row = spu_vm_voice((uint8)index);
            if ((sint16)row->note == (sint32)note && (sint16)row->program == program && (sint16)row->sequence == identity && (sint16)row->bank == bank)
            {
                if ((sint16)row->sample == 255)
                {
                    ++matched;
                    vmNoiseOff((uint8)index);
                }
                else
                {
                    spu_vm_state()->note.voice = (uint16)((uint8)index);
                    _SsVmKeyOffNow();
                    ++matched;
                }
            }
        }
        ++index;
    } while ((sint32)(uint8)index < (sint8)spu_vm_state()->events.voice_count);
    return matched;
}
#else
uint32 _SsVmKeyOff(sint16 identity, sint16 bank, sint16 program, uint16 note)
{
    SpuVmGuestBinding binding = spu_vm_binding();
    uint32 index = 0u, matched = 0u, row;
    if (r_s8(binding.voice_count) <= 0)
        return 0u;
    do
    {
        if (!(r_u32(binding.protection) & (1u << ((uint8)index & 31u))))
        {
            row = binding.voices + 54u * (uint8)index;
            if (r_s16(row + 14u) == (sint32)note && r_s16(row + 20u) == program && r_s16(row + 16u) == identity && r_s16(row + 24u) == bank)
            {
                if (r_s16(row) == 255)
                {
                    ++matched;
                    vmNoiseOff((uint8)index);
                }
                else
                {
                    w_u16(binding.context + 24u, (uint8)index);
                    _SsVmKeyOffNow();
                    ++matched;
                }
            }
        }
        ++index;
    } while ((sint32)(uint8)index < r_s8(binding.voice_count));
    return matched;
}
#endif

#if defined(XPORT_NATIVE_VM) && XPORT_NATIVE_VM
uint32 vmNoiseOff(uint8 voice)
{
    SpuVoiceState *record = spu_vm_voice(voice);
    record->status = (uint8)(0u);
    record->sample = (uint16)(0u);
    record->pitch = (uint16)(0u);
    return 54 * voice;
}
#else
uint32 vmNoiseOff(uint8 voice)
{
    SpuVmGuestBinding binding = spu_vm_binding();
    uint32 offset = 54u * voice;
    w_u8(binding.voices + offset + 29u, 0u);
    w_u16(binding.voices + offset, 0u);
    w_u16(binding.voices + offset + 4u, 0u);
    return offset;
}
#endif

#if defined(XPORT_NATIVE_VM) && XPORT_NATIVE_VM
uint32 _SsVmKeyOffNow(void)
{
    uint32 voice = spu_vm_state()->note.voice;
    uint32 low_mask, high_mask, pending_low, pending_high, key_low, key_high, result;
    SpuVoiceState *row;
    if (voice < 16u)
    {
        low_mask = 1u << (voice & 31u);
        high_mask = 0u;
    }
    else
    {
        low_mask = 0u;
        high_mask = 1u << ((voice - 16u) & 31u);
    }
    row = spu_vm_voice(voice);
    row->status = (uint8)(0u);
    pending_low = spu_vm_state()->events.key_off[0];
    pending_high = spu_vm_state()->events.key_off[1];
    row->pitch = (uint16)(0u);
    row->sample = (uint16)(0u);
    key_low = spu_vm_state()->events.key_on[0];
    pending_low |= low_mask;
    spu_vm_state()->events.key_off[0] = (uint16)((uint16)pending_low);
    spu_vm_state()->events.key_on[0] = (uint16)((uint16)(key_low & ~pending_low));
    key_high = spu_vm_state()->events.key_on[1];
    pending_high |= high_mask;
    spu_vm_state()->events.key_off[1] = (uint16)((uint16)pending_high);
    result = key_high & ~pending_high;
    spu_vm_state()->events.key_on[1] = (uint16)((uint16)result);
    return result;
}
#else
uint32 _SsVmKeyOffNow(void)
{
    SpuVmGuestBinding binding = spu_vm_binding();
    uint32 voice = r_u16(binding.context + 24u);
    uint32 low_mask, high_mask, row, pending_low, pending_high, key_low, key_high, result;
    if (voice < 16u)
    {
        low_mask = 1u << (voice & 31u);
        high_mask = 0u;
    }
    else
    {
        low_mask = 0u;
        high_mask = 1u << ((voice - 16u) & 31u);
    }
    row = binding.voices + 54u * voice;
    w_u8(row + 29u, 0u);
    pending_low = r_u16(binding.pending_keyoff);
    pending_high = r_u16(binding.pending_keyoff + 2u);
    w_u16(row + 4u, 0u);
    w_u16(row, 0u);
    key_low = r_u16(binding.voice_masks);
    pending_low |= low_mask;
    w_u16(binding.pending_keyoff, (uint16)pending_low);
    w_u16(binding.voice_masks, (uint16)(key_low & ~pending_low));
    key_high = r_u16(binding.voice_masks + 2u);
    pending_high |= high_mask;
    w_u16(binding.pending_keyoff + 2u, (uint16)pending_high);
    result = key_high & ~pending_high;
    w_u16(binding.voice_masks + 2u, (uint16)result);
    return result;
}
#endif

#if defined(XPORT_NATIVE_SEQUENCE) && XPORT_NATIVE_SEQUENCE
uint32 SsChannelMute(sint16 sequence, sint16 separation, uint16 mask)
{
    uint32 offset;
    SpuSequenceTrack *record;
    offset = 176u * (uint32)(sint32)separation;
    record = spu_sequence_track(sequence, separation);
    record->muted_channels = mask;
    return offset;
}
#else
uint32 SsChannelMute(sint16 sequence, sint16 separation, uint16 mask)
{
    uint32 table, offset, record;
    spu_sequence_tables(&table, NULL);
    if (!table)
    {
    #ifndef NDEBUG
        fprintf(stderr, "SsChannelMute requires the libsnd sequence table binding\n");
    #endif
        abort();
    }
    offset = 176u * (uint32)(sint32)separation;
    record = r_u32(table + 4u * (uint32)(sint32)sequence);
    w_u16(record + offset + 128u, mask);
    return offset;
}
#endif

#if defined(XPORT_NATIVE_SEQUENCE) && XPORT_NATIVE_SEQUENCE
uint32 _SsNoteOn(sint16 sequence, sint16 separation, uint8 note, uint8 velocity)
{
    uint32 channel, volume, muted;
    SpuSequenceTrack *record;
    sint16 identity, bank;
    uint8 program;
    record = spu_sequence_track(sequence, separation);
    channel = record->channel;
    volume = record->channel_pan[channel];
    if (velocity != 0u)
    {
        muted = ((uint32)(sint32)(sint16)record->muted_channels >> (channel & 31u)) & 1u;
        if (muted != 0u)
            return muted;
    }
    identity = (sint16)((uint32)(sint32)sequence | ((uint32)(sint32)separation << 8));
    bank = (sint8)record->bank;
    program = record->channel_program[channel];
    if (velocity == 0u)
        return _SsVmKeyOff(identity, bank, program, note);
    return _SsVmKeyOn(identity, bank, program, note, velocity, (uint16)volume);
}
#else
uint32 _SsNoteOn(sint16 sequence, sint16 separation, uint8 note, uint8 velocity)
{
    uint32 table, record, channel, volume, muted;
    sint16 identity, bank;
    uint8 program;
    spu_sequence_tables(&table, NULL);
    if (!table)
    {
    #ifndef NDEBUG
        fprintf(stderr, "_SsNoteOn requires the libsnd sequence table binding\n");
    #endif
        abort();
    }
    record = r_u32(table + 4u * (uint32)(sint32)sequence) + 176u * (uint32)(sint32)separation;
    channel = r_u8(record + 23u);
    volume = r_u8(record + channel + 39u);
    if (velocity != 0u)
    {
        muted = ((uint32)(sint32)r_s16(record + 128u) >> (channel & 31u)) & 1u;
        if (muted != 0u)
            return muted;
    }
    identity = (sint16)((uint32)(sint32)sequence | ((uint32)(sint32)separation << 8));
    bank = r_s8(record + 38u);
    program = r_u8(record + channel + 55u);
    if (velocity == 0u)
        return _SsVmKeyOff(identity, bank, program, note);
    return _SsVmKeyOn(identity, bank, program, note, velocity, (uint16)volume);
}
#endif

#if defined(XPORT_NATIVE_SEQUENCE) && XPORT_NATIVE_SEQUENCE
uint32 _SsSetProgramChange(sint16 sequence, sint16 separation, uint8 program)
{
    uint32 result, channel;
    SpuSequenceTrack *record;
    record = spu_sequence_track(sequence, separation);
    channel = record->channel;
    record->channel_program[channel] = (uint8)(program);
    result = _SsReadDeltaValue(sequence, separation);
    record->delta = (uint32)(result);
    return result;
}
#else
uint32 _SsSetProgramChange(sint16 sequence, sint16 separation, uint8 program)
{
    uint32 table, record, result, channel;
    spu_sequence_tables(&table, NULL);
    if (!table)
    {
    #ifndef NDEBUG
        fprintf(stderr, "_SsSetProgramChange requires the libsnd sequence table binding\n");
    #endif
        abort();
    }
    record = r_u32(table + 4u * (uint32)(sint32)sequence) + 176u * (uint32)(sint32)separation;
    channel = r_u8(record + 23u);
    w_u8(record + channel + 55u, program);
    result = _SsReadDeltaValue(sequence, separation);
    w_u32(record + 144u, result);
    return result;
}
#endif

#if defined(XPORT_NATIVE_SEQUENCE) && XPORT_NATIVE_SEQUENCE
uint32 _SsContNrpn1(sint16 sequence, sint16 separation, uint8 value)
{
    uint32 kind, count, result;
    SpuSequenceTrack *record;
    record = spu_sequence_track(sequence, separation);
    kind = record->control_data[3];
    if (kind == 40u)
    {
        spu_sequence_dispatch_mark(sequence, separation, value);
        kind = record->control_data[3];
    }
    if (kind != 30u && kind != 20u && kind != 40u)
    {
        count = record->control_data[7];
        record->control_data[2] = (uint8)(value);
        record->control_data[4] = (uint8)(0u);
        record->control_data[7] = (uint8)((uint8)(count + 1u));
    }
    result = _SsReadDeltaValue(sequence, separation);
    record->delta = (uint32)(result);
    return result;
}
#else
uint32 _SsContNrpn1(sint16 sequence, sint16 separation, uint8 value)
{
    uint32 table, record, kind, marks, target, count, result;
    uint32 arguments[3];
    spu_sequence_tables(&table, NULL);
    if (!table)
    {
    #ifndef NDEBUG
        fprintf(stderr, "_SsContNrpn1 requires the libsnd sequence table binding\n");
    #endif
        abort();
    }
    record = r_u32(table + 4u * (uint32)(sint32)sequence) + 176u * (uint32)(sint32)separation;
    kind = r_u8(record + 27u);
    if (kind == 40u)
    {
        marks = spu_sequence_mark_table();
        if (!marks)
        {
    #ifndef NDEBUG
            fprintf(stderr, "_SsContNrpn1 requires the libsnd mark table binding\n");
    #endif
            abort();
        }
        target = r_u32(marks + 64u * (uint32)(sint32)sequence + 4u * (uint32)(sint32)separation);
        if (target != 0u)
        {
            arguments[0] = (uint32)(sint32)sequence;
            arguments[1] = (uint32)(sint32)separation;
            arguments[2] = value;
            xport_libsnd_sequence_call(target, 3u, arguments);
        }
        kind = r_u8(record + 27u);
    }
    if (kind != 30u && kind != 20u && kind != 40u)
    {
        count = r_u8(record + 31u);
        w_u8(record + 26u, value);
        w_u8(record + 28u, 0u);
        w_u8(record + 31u, (uint8)(count + 1u));
    }
    result = _SsReadDeltaValue(sequence, separation);
    w_u32(record + 144u, result);
    return result;
}
#endif

#if defined(XPORT_NATIVE_SEQUENCE) && XPORT_NATIVE_SEQUENCE
uint32 _SsContNrpn2(sint16 sequence, sint16 separation, uint8 value)
{
    uint32 result, cursor, count;
    SpuSequenceTrack *record;
    record = spu_sequence_track(sequence, separation);
    if (value == 20u)
    {
        record->control_data[3] = (uint8)(value);
        record->control_data[4] = (uint8)(1u);
        result = _SsReadDeltaValue(sequence, separation);
        cursor = record->stream.cursor;
        record->delta = (uint32)(result);
        record->stream.loop_start = cursor;
        return result;
    }
    if (value == 30u)
    {
        count = record->control_data[5];
        record->control_data[3] = (uint8)(value);
        if (count == 0u)
            record->loop_active = (uint8)(0u);
        else if (count >= 127u)
        {
            _SsReadDeltaValue(sequence, separation);
            result = record->stream.loop_start;
            record->delta = (uint32)(0u);
            record->stream.cursor = result;
            return result;
        }
        else
        {
            record->control_data[5] = (uint8)((uint8)(count - 1u));
            result = _SsReadDeltaValue(sequence, separation);
            count = record->control_data[5];
            record->delta = (uint32)(result);
            if (count != 0u)
            {
                result = record->stream.loop_start;
                record->stream.cursor = result;
            }
            else
                record->loop_active = (uint8)(0u);
            return result;
        }
    }
    else
    {
        count = record->control_data[7];
        record->control_data[3] = (uint8)(value);
        record->control_data[7] = (uint8)((uint8)(count + 1u));
    }
    result = _SsReadDeltaValue(sequence, separation);
    record->delta = (uint32)(result);
    return result;
}
#else
uint32 _SsContNrpn2(sint16 sequence, sint16 separation, uint8 value)
{
    uint32 table, record, result, cursor, count;
    spu_sequence_tables(&table, NULL);
    if (!table)
    {
    #ifndef NDEBUG
        fprintf(stderr, "_SsContNrpn2 requires the libsnd sequence table binding\n");
    #endif
        abort();
    }
    record = r_u32(table + 4u * (uint32)(sint32)sequence) + 176u * (uint32)(sint32)separation;
    if (value == 20u)
    {
        w_u8(record + 27u, value);
        w_u8(record + 28u, 1u);
        result = _SsReadDeltaValue(sequence, separation);
        cursor = r_u32(record);
        w_u32(record + 144u, result);
        w_u32(record + 8, cursor);
        return result;
    }
    if (value == 30u)
    {
        count = r_u8(record + 29u);
        w_u8(record + 27u, value);
        if (count == 0u)
            w_u8(record + 21u, 0u);
        else if (count >= 127u)
        {
            _SsReadDeltaValue(sequence, separation);
            result = r_u32(record + 8);
            w_u32(record + 144u, 0u);
            w_u32(record, result);
            return result;
        }
        else
        {
            w_u8(record + 29u, (uint8)(count - 1u));
            result = _SsReadDeltaValue(sequence, separation);
            count = r_u8(record + 29u);
            w_u32(record + 144u, result);
            if (count != 0u)
            {
                result = r_u32(record + 8);
                w_u32(record, result);
            }
            else
                w_u8(record + 21u, 0u);
            return result;
        }
    }
    else
    {
        count = r_u8(record + 31u);
        w_u8(record + 27u, value);
        w_u8(record + 31u, (uint8)(count + 1u));
    }
    result = _SsReadDeltaValue(sequence, separation);
    w_u32(record + 144u, result);
    return result;
}
#endif

#if defined(XPORT_NATIVE_SEQUENCE) && XPORT_NATIVE_SEQUENCE
uint32 _SsSetControlChange(sint16 sequence, sint16 separation, uint8 control)
{
    SpuSequenceTrack *record = spu_sequence_track(sequence, separation);
    uint32 cursor = record->stream.cursor;
    uint8 value = spu_sequence_stream_byte(&record->stream, cursor);
    record->stream.cursor = cursor + 1;
    switch (control)
    {
        case 6:
        case 7:
        case 10:
        case 11:
        case 64:
        case 98:
        case 99:
        case 100:
        case 101:
        case 91:
        case 121:
            return spu_sequence_dispatch_controller((uint32)(sint32)sequence, (uint32)(sint32)separation, control, value);
        default:
            if (control == 0)
                record->bank = value;
            uint32 result = _SsReadDeltaValue(sequence, separation);
            record->delta = result;
            return result;
    }
}
#else
uint32 _SsSetControlChange(sint16 sequence, sint16 separation, uint8 control)
{
    uint32 table, handlers, record, cursor, value, result, offset;
    uint32 arguments[3];
    spu_sequence_tables(&table, &handlers);
    if (!table || !handlers)
    {
    #ifndef NDEBUG
        fprintf(stderr, "_SsSetControlChange requires sequence and handler table bindings\n");
    #endif
        abort();
    }
    record = r_u32(table + 4u * (uint32)(sint32)sequence) + 176u * (uint32)(sint32)separation;
    cursor = r_u32(record);
    value = r_u8(cursor);
    w_u32(record, cursor + 1u);
    switch (control)
    {
        case 6:
            offset = 24u;
            break;
        case 7:
            offset = 28u;
            break;
        case 10:
            offset = 32u;
            break;
        case 11:
            offset = 36u;
            break;
        case 64:
            offset = 40u;
            break;
        case 98:
            offset = 44u;
            break;
        case 99:
            offset = 48u;
            break;
        case 100:
            offset = 52u;
            break;
        case 101:
            offset = 56u;
            break;
        case 91:
            offset = 60u;
            break;
        case 121:
            offset = 64u;
            break;
        default:
            if (control == 0u)
                w_u8(record + 38u, value);
            result = _SsReadDeltaValue(sequence, separation);
            w_u32(record + 144u, result);
            return result;
    }
    arguments[0] = (uint32)(sint32)sequence;
    arguments[1] = (uint32)(sint32)separation;
    arguments[2] = control == 121u ? 121u : value;
    return xport_libsnd_sequence_call(r_u32(handlers + offset), 3u, arguments);
}
#endif

uint32 _SsSeqGetEof(sint16 sequence, sint16 separation, uint32 event)
{
#ifndef NDEBUG
    fprintf(stderr, "Unimplemented PsyQ _SsSeqGetEof sequence %d separation %d event %u\n", sequence, separation, event);
#endif
    abort();
}

#if defined(XPORT_NATIVE_SEQUENCE) && XPORT_NATIVE_SEQUENCE
uint32 _SsReadDeltaValue(sint16 sequence, sint16 separation)
{
    uint32 cursor, delta, byte, result;
    SpuSequenceTrack *record;
    record = spu_sequence_track(sequence, separation);
    cursor = record->stream.cursor;
    delta = spu_sequence_stream_byte(&record->stream, cursor);
    record->stream.cursor = cursor + 1u;
    if (delta == 0u)
        return 0u;
    if ((delta & 0x80u) != 0u)
    {
        delta &= 0x7Fu;
        do
        {
            cursor = record->stream.cursor;
            delta <<= 7;
            byte = spu_sequence_stream_byte(&record->stream, cursor);
            record->stream.cursor = cursor + 1u;
            delta += byte & 0x7Fu;
        } while ((byte & 0x80u) != 0u);
    }
    result = delta * 10u;
    record->elapsed_delta = (uint32)(record->elapsed_delta + result);
    return result;
}
#else
uint32 _SsReadDeltaValue(sint16 sequence, sint16 separation)
{
    uint32 table, record, cursor, delta, byte, result;
    spu_sequence_tables(&table, NULL);
    if (!table)
    {
    #ifndef NDEBUG
        fprintf(stderr, "_SsReadDeltaValue requires the libsnd sequence table binding\n");
    #endif
        abort();
    }
    record = r_u32(table + 4u * (uint32)(sint32)sequence) + 176u * (uint32)(sint32)separation;
    cursor = r_u32(record);
    delta = r_u8(cursor);
    w_u32(record, cursor + 1u);
    if (delta == 0u)
        return 0u;
    if ((delta & 0x80u) != 0u)
    {
        delta &= 0x7Fu;
        do
        {
            cursor = r_u32(record);
            delta <<= 7;
            byte = r_u8(cursor);
            w_u32(record, cursor + 1u);
            delta += byte & 0x7Fu;
        } while ((byte & 0x80u) != 0u);
    }
    result = delta * 10u;
    w_u32(record + 136u, r_u32(record + 136u) + result);
    return result;
}
#endif

#if defined(XPORT_NATIVE_SEQUENCE) && XPORT_NATIVE_SEQUENCE
uint32 _SsGetSeqData(sint16 sequence, sint16 separation)
{
    uint32 cursor, event, status, note, velocity, marker;
    SpuSequenceTrack *record;
    int explicit_status;
    record = spu_sequence_track(sequence, separation);
    cursor = record->stream.cursor;
    event = spu_sequence_stream_byte(&record->stream, cursor);
    cursor += 1u;
    record->stream.cursor = cursor;
    if ((record->flags & 0x401u) == 0x401u)
    {
        marker = record->stream.loop_end;
        if (record->stream.loop_end_set && cursor == marker + 1u)
        {
            _SsSeqGetEof(sequence, separation, spu_sequence_stream_byte(&record->stream, marker + 1u));
            return 0xFFFFFFFFu;
        }
    }
    explicit_status = (event & 0x80u) != 0u;
    if (explicit_status)
    {
        record->channel = (uint8)((uint8)(event & 15u));
        status = event & 0xF0u;
    }
    else
        status = record->running_status;
    switch (status)
    {
        case 0x90:
            if (explicit_status)
            {
                cursor = record->stream.cursor;
                record->running_status = (uint8)(0x90u);
                note = spu_sequence_stream_byte(&record->stream, cursor);
                record->stream.cursor = cursor + 1u;
                velocity = spu_sequence_stream_byte(&record->stream, cursor + 1u);
                record->stream.cursor = cursor + 2u;
            }
            else
            {
                cursor = record->stream.cursor;
                note = event;
                velocity = spu_sequence_stream_byte(&record->stream, cursor);
                record->stream.cursor = cursor + 1u;
            }
            record->delta = (uint32)(_SsReadDeltaValue(sequence, separation));
            spu_sequence_dispatch_note((uint32)(sint32)sequence, (uint32)(sint32)separation, note, velocity);
            return 0u;
        case 0xB0:
        case 0xC0:
            if (explicit_status)
            {
                cursor = record->stream.cursor;
                record->running_status = (uint8)((uint8)status);
                event = spu_sequence_stream_byte(&record->stream, cursor);
                record->stream.cursor = cursor + 1u;
            }
            if (status == 0xC0u)
                spu_sequence_dispatch_program((uint32)(sint32)sequence, (uint32)(sint32)separation, event);
            else
                spu_sequence_dispatch_control((uint32)(sint32)sequence, (uint32)(sint32)separation, event);
            return 0u;
        case 0xE0:
            if (explicit_status)
            {
                cursor = record->stream.cursor;
                record->running_status = (uint8)(0xE0u);
                record->stream.cursor = cursor + 1u;
            }
            spu_sequence_dispatch_pitch((uint32)(sint32)sequence, (uint32)(sint32)separation);
            return 0u;
        case 0xF0:
            if (!explicit_status)
                return 0u;
            cursor = record->stream.cursor;
            record->running_status = (uint8)(0xFFu);
            event = spu_sequence_stream_byte(&record->stream, cursor);
            record->stream.cursor = cursor + 1u;
            break;
        case 0xFF:
            if (explicit_status)
                return 0u;
            break;
        default:
            return 0u;
    }
    if (event == 0x2Fu)
    {
        _SsSeqGetEof(sequence, separation, 0x2Fu);
        return 1u;
    }
    spu_sequence_dispatch_meta((uint32)(sint32)sequence, (uint32)(sint32)separation, event);
    return 0u;
}
#else
uint32 _SsGetSeqData(sint16 sequence, sint16 separation)
{
    uint32 table, handlers, row, stride, record, cursor, event, status, note, velocity, marker, target;
    uint32 arguments[4];
    int explicit_status;
    spu_sequence_tables(&table, &handlers);
    if (!table || !handlers)
    {
    #ifndef NDEBUG
        fprintf(stderr, "_SsGetSeqData requires sequence and handler table bindings\n");
    #endif
        abort();
    }
    row = table + 4u * (uint32)(sint32)sequence;
    stride = 176u * (uint32)(sint32)separation;
    record = r_u32(row) + stride;
    cursor = r_u32(record);
    event = r_u8(cursor);
    cursor += 1u;
    w_u32(record, cursor);
    if ((r_u32(r_u32(row) + stride + 152u) & 0x401u) == 0x401u)
    {
        marker = r_u32(record + 16);
        if (cursor == marker + 1u)
        {
            _SsSeqGetEof(sequence, separation, r_u8(marker + 1u));
            return 0xFFFFFFFFu;
        }
    }
    explicit_status = (event & 0x80u) != 0u;
    arguments[0] = (uint32)(sint32)sequence;
    arguments[1] = (uint32)(sint32)separation;
    if (explicit_status)
    {
        w_u8(record + 23u, (uint8)(event & 15u));
        status = event & 0xF0u;
    }
    else
        status = r_u8(record + 22u);
    switch (status)
    {
        case 0x90:
            if (explicit_status)
            {
                cursor = r_u32(record);
                w_u8(record + 22u, 0x90u);
                note = r_u8(cursor);
                w_u32(record, cursor + 1u);
                velocity = r_u8(cursor + 1u);
                w_u32(record, cursor + 2u);
            }
            else
            {
                cursor = r_u32(record);
                note = event;
                velocity = r_u8(cursor);
                w_u32(record, cursor + 1u);
            }
            w_u32(record + 144u, _SsReadDeltaValue(sequence, separation));
            arguments[2] = note;
            arguments[3] = velocity;
            target = r_u32(handlers);
            xport_libsnd_sequence_call(target, 4u, arguments);
            return 0u;
        case 0xB0:
        case 0xC0:
            if (explicit_status)
            {
                cursor = r_u32(record);
                w_u8(record + 22u, (uint8)status);
                event = r_u8(cursor);
                w_u32(record, cursor + 1u);
            }
            arguments[2] = event;
            target = r_u32(handlers + (status == 0xC0u ? 4u : 16u));
            xport_libsnd_sequence_call(target, 3u, arguments);
            return 0u;
        case 0xE0:
            if (explicit_status)
            {
                cursor = r_u32(record);
                w_u8(record + 22u, 0xE0u);
                w_u32(record, cursor + 1u);
            }
            /* Preserve the original carried a2 even though pseudocode omits it */
            arguments[2] = explicit_status ? 0xE0u : row;
            target = r_u32(handlers + 8u);
            xport_libsnd_sequence_call(target, 3u, arguments);
            return 0u;
        case 0xF0:
            if (!explicit_status)
                return 0u;
            cursor = r_u32(record);
            w_u8(record + 22u, 0xFFu);
            event = r_u8(cursor);
            w_u32(record, cursor + 1u);
            break;
        case 0xFF:
            if (explicit_status)
                return 0u;
            break;
        default:
            return 0u;
    }
    if (event == 0x2Fu)
    {
        _SsSeqGetEof(sequence, separation, 0x2Fu);
        return 1u;
    }
    arguments[2] = event;
    target = r_u32(handlers + 12u);
    xport_libsnd_sequence_call(target, 3u, arguments);
    return 0u;
}
#endif

#if defined(XPORT_NATIVE_SEQUENCE) && XPORT_NATIVE_SEQUENCE
uint32 _SsSeqPlay(sint16 sequence, sint16 separation)
{
    uint32 ticks, duration_word, result, accumulated;
    SpuSequenceTrack *record;
    sint32 duration, countdown, delta;
    record = spu_sequence_track(sequence, separation);
    duration = (sint16)record->tick_step;
    ticks = record->delta;
    duration_word = record->tick_step;
    delta = (sint32)(ticks - (uint32)duration);
    result = duration < (sint32)ticks;
    if (delta > 0)
    {
        countdown = (sint16)record->countdown;
        result = (uint32)record->countdown - 1u;
        if (countdown > 0)
        {
            record->countdown = (uint16)((uint16)result);
            return result;
        }
        if (countdown == 0)
        {
            result = record->delta;
            record->countdown = (uint16)((uint16)duration_word);
            result -= 1u;
            record->delta = (uint32)(result);
        }
        else
            record->delta = (uint32)((uint32)delta);
        return result;
    }
    accumulated = ticks;
    if (result)
        return result;
    do
    {
        do
        {
            _SsGetSeqData(sequence, separation);
            ticks = record->delta;
        } while (ticks == 0u);
        duration = (sint16)record->tick_step;
        accumulated += ticks;
        result = accumulated - (uint32)duration;
    } while ((sint32)accumulated < duration);
    record->delta = (uint32)(result);
    return result;
}
#else
uint32 _SsSeqPlay(sint16 sequence, sint16 separation)
{
    uint32 table, record, ticks, duration_word, result, accumulated;
    sint32 duration, countdown, delta;
    spu_sequence_tables(&table, NULL);
    if (!table)
    {
    #ifndef NDEBUG
        fprintf(stderr, "_SsSeqPlay requires the libsnd sequence table binding\n");
    #endif
        abort();
    }
    record = r_u32(table + 4u * (uint32)(sint32)sequence) + 176u * (uint32)(sint32)separation;
    duration = r_s16(record + 84u);
    ticks = r_u32(record + 144u);
    duration_word = r_u16(record + 84u);
    delta = (sint32)(ticks - (uint32)duration);
    result = duration < (sint32)ticks;
    if (delta > 0)
    {
        countdown = r_s16(record + 82u);
        result = (uint32)r_u16(record + 82u) - 1u;
        if (countdown > 0)
        {
            w_u16(record + 82u, (uint16)result);
            return result;
        }
        if (countdown == 0)
        {
            result = r_u32(record + 144u);
            w_u16(record + 82u, (uint16)duration_word);
            result -= 1u;
            w_u32(record + 144u, result);
        }
        else
            w_u32(record + 144u, (uint32)delta);
        return result;
    }
    accumulated = ticks;
    if (result)
        return result;
    do
    {
        do
        {
            _SsGetSeqData(sequence, separation);
            ticks = r_u32(record + 144u);
        } while (ticks == 0u);
        duration = r_s16(record + 84u);
        accumulated += ticks;
        result = accumulated - (uint32)duration;
    } while ((sint32)accumulated < duration);
    w_u32(record + 144u, result);
    return result;
}
#endif

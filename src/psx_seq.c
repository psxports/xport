#include "psx_spu.h"
#include "xport_trace.h"
#include <stdio.h>
#include <stdlib.h>

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

sint32 _SsVmVSetUp(uint16 bank, sint16 program)
{
    SpuVmGuestBinding binding = spu_vm_binding();
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
}

uint32 _SsVmSelectToneAndVag(uint8 *tones, uint8 *vags)
{
    SpuVmGuestBinding binding = spu_vm_binding();
    uint32 count = 0u, index = 0u, row, minimum;
    sint32 note;
    if (r_s8(binding.context) <= 0)
        return 0u;
    do
    {
        row = ((uint32)(sint32)r_s8(binding.context + 7u) * 16u + (uint32)(sint32)(sint8)index) * 32u;
        row += r_u32(binding.tone_pointer);
        minimum = r_u8(row + 6u);
        note = r_s8(binding.context + 2u);
        if (note >= (sint32)minimum && (sint32)r_u8(row + 7u) >= note)
        {
            vags[count] = r_u8(row + 22u);
            tones[count] = (uint8)index;
            ++count;
        }
        ++index;
    } while ((sint32)(sint8)index < r_s8(binding.context));
    return (uint8)count;
}

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

uint32 _SsVmDoAllocate(void)
{
    SpuVmGuestBinding binding = spu_vm_binding();
    uint32 values, flags, voice, offset, row, index, mask, value, vag, adsr, release, result;
    sint32 fake_program, tone;
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
    row = (uint32)(((sint32)(sint16)vag - 1) / 2) * 16u + r_u32(binding.program_pointer);
    value = r_u16(row + ((vag & 1u) ? 12u : 14u));
    w_u16(values + offset + 6u, (uint16)value);
    voice = (uint32)(sint32)r_s16(binding.context + 24u);
    value = r_u8(flags + voice);
    w_u8(flags + voice, (uint8)(value | 8u));
    fake_program = r_s8(binding.context + 7u);
    tone = r_s8(binding.context + 12u);
    row = (uint32)(fake_program * 16 + tone) * 32u + r_u32(binding.tone_pointer);
    value = r_u16(row + 16u);
    w_u16(values + offset + 8u, (uint16)value);
    adsr = r_u16(row + 18u);
    release = r_u16(binding.release_offset) + (adsr & 31u);
    if ((sint16)(uint16)release >= 32)
        release = 31u;
    w_u16(values + offset + 10u, (uint16)(release | (adsr & 0xFFE0u)));
    voice = (uint32)(sint32)r_s16(binding.context + 24u);
    result = r_u8(flags + voice) | 0x30u;
    w_u8(flags + voice, (uint8)result);
    return result;
}

static void vmNoiseOn(uint8 voice)
{
#ifndef NDEBUG
    fprintf(stderr, "Unimplemented PsyQ vmNoiseOn voice %u\n", voice);
#endif
    abort();
}

uint32 note2pitch(void)
{
    SpuVmGuestBinding binding = spu_vm_binding();
    sint32 note = r_s8(binding.context + 2u);
    uint8 center = r_u8(binding.context + 16u);
    uint8 shift = r_u8(binding.context + 17u);
    return SsPitchFromNote(note, 0, center, shift);
}

uint32 _SsVmKeyOnNow(uint16 pitch)
{
    SpuVmGuestBinding binding = spu_vm_binding();
    uint32 values, flags_address, table, header, volume, left, right, row, voice, offset;
    uint32 pan, low_mask, high_mask, low, high, key_high, off_high, key_low, result;
    sint32 velocity, program_volume, tone_volume;
    sint16 identity, initial_voice;
    spu_voice_cache_binding(&values, &flags_address);
    spu_sequence_tables(&table, NULL);
    if (!values || !flags_address || !table)
    {
#ifndef NDEBUG
        fprintf(stderr, "_SsVmKeyOnNow requires libsnd cache and sequence bindings\n");
#endif
        abort();
    }
    header = r_u32(binding.header_pointer);
    volume = r_u8(header + 24u);
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

static uint32 spu_vm_voice_row(const SpuVmGuestBinding *binding)
{
    return binding->voices + 54u * (uint32)(sint32)r_s16(binding->context + 24u);
}

uint32 _SsVmKeyOn(sint16 identity, sint16 bank, sint16 program, uint16 note, uint16 velocity, uint16 volume)
{
    SpuVmGuestBinding binding = spu_vm_binding();
    uint8 tones[128], vags[128], tone_count;
    uint32 table, record, context = binding.context, row, value, candidate, index, pitch, result = 0u;
    sint32 limit;
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
    row = r_u32(binding.program_pointer) + 16u * (uint32)(sint32)program;
    w_u8(context + 10u, r_u8(row + 1u));
    w_u8(context + 11u, r_u8(row + 4u));
    w_u8(context, r_u8(row));
    row = r_u32(binding.header_pointer);
    value = (uint32)(sint32)r_s8(context + 7u);
    if ((sint32)value >= (sint32)r_u16(row + 18u))
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
        row = row * 32u + r_u32(binding.tone_pointer);
        w_u8(context + 15u, r_u8(row));
        w_u8(context + 13u, r_u8(row + 2u));
        w_u8(context + 14u, r_u8(row + 3u));
        w_u8(context + 16u, r_u8(row + 4u));
        w_u8(context + 17u, r_u8(row + 5u));
        w_u8(context + 18u, r_u8(row + 1u));
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

uint32 SsVoKeyOff(uint32 voice_program, uint32 note_volume)
{
    sint16 bank = (sint16)(voice_program >> 8);
    sint16 program = (uint8)voice_program;
    uint16 note = (uint16)(note_volume >> 8);
    return _SsVmKeyOff(33, bank, program, note);
}

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

uint32 vmNoiseOff(uint8 voice)
{
    SpuVmGuestBinding binding = spu_vm_binding();
    uint32 offset = 54u * voice;
    w_u8(binding.voices + offset + 29u, 0u);
    w_u16(binding.voices + offset, 0u);
    w_u16(binding.voices + offset + 4u, 0u);
    return offset;
}

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
        w_u32(record + 8u, cursor);
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
            result = r_u32(record + 8u);
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
                result = r_u32(record + 8u);
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

uint32 _SsSeqGetEof(sint16 sequence, sint16 separation, uint32 event)
{
#ifndef NDEBUG
    fprintf(stderr, "Unimplemented PsyQ _SsSeqGetEof sequence %d separation %d event %u\n", sequence, separation, event);
#endif
    abort();
}

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
        marker = r_u32(record + 16u);
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

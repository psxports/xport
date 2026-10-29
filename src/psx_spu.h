#ifndef XPORT_PSX_SPU_H
#define XPORT_PSX_SPU_H

#include <stddef.h>
#include <stdio.h>
#include "psx.h"
#include "psx_xa.h"

#define SPU_RAM_SIZE 0x80000u
#define SPU_VOICE_COUNT 24
#define SPU_SAMPLE_RATE 44100
#define SPU_INITIAL_SAMPLE_NOTE 0xC000u

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

    void cd_dispatch_ready_event(void);
    sint32 xport_xa_play_file(const char *path, sint32 repeat);
    sint32 xport_xa_play_file_at(const char *path, sint32 repeat, sint32 sector);
    sint32 xport_xa_file_position(CdlLOC *position, uint32 *frames);
    sint32 CdInit(void);
    sint32 CdLastCom(void);
    sint32 cd_mount_cue(const char *cue_path);
    sint32 cd_mount_stream_file(const char *path);

    typedef struct XPORT_CD_SOURCE
    {
        void *context;
        uint32 sectors;
        /* Fill raw bytes at offset 0 for 2352 or payload at offset 24 for 2048 */
        sint32 (*read_sector)(void *context, uint32 sector, uint8 buffer[2352], uint32 *sector_bytes);
        CdlFILE *(*search_file)(void *context, CdlFILE *result, const char *path);
        void (*close)(void *context);
    } XPORT_CD_SOURCE;

    /* Source callbacks run under the CD owner's lock and must not reenter it */
    sint32 xport_cd_mount_source(const XPORT_CD_SOURCE *source);

    void *cd_search_file_native(void *result, const char *path);
    CdlLOC *CdIntToPos(sint32 sector, CdlLOC *position);
    void cd_unmount_image(void);
    sint32 cd_reset_native(sint32 mode);

    typedef struct SpuReverbControl
    {
        uint32 control;
        uint32 enabled;
        uint32 reserved;
        uint32 work_address;
        sint16 depth_left;
        sint16 depth_right;
        sint32 mode;
        sint32 delay;
        sint32 feedback;
        uint16 parameters[32];
    } SpuReverbControl;

    typedef sint32 (*SpuReverbWorkCheck)(void *context, uint32 work_address);
    sint32 spu_reverb_import(const SpuReverbControl *state, SpuReverbWorkCheck check, void *context);
    void spu_reverb_export(SpuReverbControl *state);
    sint32 spu_reverb_incomplete(void);
    void spu_bind_reverb_presets(const uint32 *work_units, const uint8 *presets, uint32 address_shift);

    enum
    {
        SPU_RAM_ADDRESS_SHIFT = 3,
        SPU_RAM_ADDRESS_UNIT = 1 << SPU_RAM_ADDRESS_SHIFT,
        SPU_RAM_ADDRESS_MASK = SPU_RAM_ADDRESS_UNIT - 1,
        SPU_REGISTER_BASE = 0x1F801C00,
        SPU_DMA_ADDRESS_REGISTER = 0x1F8010C0,
        SPU_DMA_BLOCK_REGISTER = 0x1F8010C4,
        SPU_DMA_CONTROL_REGISTER = 0x1F8010C8,
        SPU_DMA_PRIORITY_REGISTER = 0x1F8010F0,
        SPU_DMA_DELAY_REGISTER = 0x1F801014
    };

    extern const uint32 spu_reverb_default_work[10];

    typedef struct SpuMallocRecord
    {
        uint32 address;
        uint32 size;
    } SpuMallocRecord;

    typedef char SpuMallocRecordWordBinding[(sizeof(SpuMallocRecord) == 8 && offsetof(SpuMallocRecord, size) == 4) ? 1 : -1];

    typedef struct SpuMallocState
    {
        uint32 count;
        uint32 last_index;
        SpuMallocRecord *records;
    } SpuMallocState;

    /* PsyQ 3.3/3.4 streaming environment, before low_priority was added */
    typedef struct SpuStVoiceAttr
    {
        uint8 status;
        uint8 pad[3];
        sint32 last_size;
        uint32 buf_addr;
        uint32 data_addr;
    } SpuStVoiceAttr;

    typedef struct SpuStEnv
    {
        sint32 size;
        SpuStVoiceAttr voice[SPU_VOICE_COUNT];
    } SpuStEnv;

    SpuStEnv *SpuStInit(void);

    typedef struct SpuSequenceStream
    {
        const uint8 *data;
        uint32 size;
        uint32 cursor;
        uint32 start;
        uint32 loop_start;
        uint32 replay;
        uint32 loop_end;
        uint32 loop_end_set;
    } SpuSequenceStream;

    typedef struct SpuSequenceSchedulerTrack
    {
        SpuSequenceStream stream;
        uint8 next_separation;
        uint8 loop_count_set;
        uint8 running_status;
        uint8 channel;
        uint8 rpn_parameter;
        uint8 rpn_selector;
        uint8 nrpn_parameter;
        uint8 nrpn_selector;
        uint8 channel_pan[16];
        uint8 loop_count_pending;
        uint8 loop_remaining;
        uint8 rpn_phase;
        uint8 nrpn_phase;
        uint8 playback_latch;
        uint8 channel_program[16];
        uint8 next_sequence;
        uint16 volume_delta;
        uint16 volume_remaining;
        uint16 volume_period;
        uint16 tempo_period;
        uint16 repeat_limit;
        uint16 repeat_count;
        uint16 ticks_per_quarter;
        uint16 bank;
        uint16 channel_volume[16];
        uint16 tick_countdown;
        uint16 tick_period;
        uint16 initial_tick_period;
        uint16 left_volume;
        uint16 right_volume;
        uint16 saved_left_volume;
        uint16 saved_right_volume;
        uint32 initial_delta;
        uint32 elapsed_ticks;
        uint32 initial_tempo;
        uint32 delta;
        uint32 tempo;
        uint32 flags;
        uint32 volume_duration;
        uint32 volume_countdown;
        uint32 tempo_duration;
        uint32 tempo_countdown;
        uint32 target_tempo;
        uint16 event_velocity;
        uint16 channel_mute_mask;
    } SpuSequenceSchedulerTrack;

    typedef struct SpuSequenceScheduler
    {
        uint32 rate;
        uint32 used_mask;
        uint32 processing;
        uint16 sequence_count;
        uint16 separation_count;
        SpuSequenceSchedulerTrack *tracks[32];
    } SpuSequenceScheduler;

    typedef uint32 (*SpuToneParameterHandler)(sint16 bank, sint16 program, sint16 tone, VagAtr attributes, uint8 value);
    typedef uint32 (*SpuToneValueHandler)(uint8 value);

    typedef struct SpuToneCommand
    {
        SpuToneParameterHandler parameter;
        SpuToneValueHandler value;
    } SpuToneCommand;

    typedef uint32 (*SpuSequenceNoteHandler)(sint16 sequence, sint16 separation, uint8 note, uint8 velocity);
    typedef uint32 (*SpuSequenceValueHandler)(sint16 sequence, sint16 separation, uint8 value);
    typedef uint32 (*SpuSequenceHandler)(sint16 sequence, sint16 separation);
    typedef uint32 (*SpuSequenceVolumeGet)(sint16 handle, uint16 *left, uint16 *right);
    typedef uint32 (*SpuSequenceVolumeSet)(sint16 handle, sint16 left, sint16 right, sint16 mode);

    typedef uint32 (*SpuSequenceVoiceVolumeSet)(sint16 handle, sint16 bank, sint16 program, uint16 volume, uint16 pan);
    typedef uint32 (*SpuSequenceReverbEnable)(uint32 mode);
    typedef uint32 (*SpuSequenceReverbDepthSet)(sint16 left, sint16 right);
    typedef void (*SpuSequenceMarkHandler)(sint16 sequence, sint16 separation, sint16 value);
    typedef uint32 (*SpuSequenceVoiceKeyOn)(sint16 handle, sint16 bank, sint16 program, uint16 note, uint16 velocity, uint16 pan);
    typedef uint32 (*SpuSequenceVoiceKeyOff)(sint16 handle, sint16 bank, sint16 program, uint16 note);
    typedef uint32 (*SpuSequencePitchSet)(sint16 handle, sint16 bank, sint16 program, uint16 bend);
    typedef uint32 (*SpuSequencePackedHandler)(sint16 handle);

    typedef void (*SpuSequenceStep)(sint16 sequence, sint16 separation);

    typedef struct SpuSequencePumpOps
    {
        uint32 (*flush)(void);
        SpuSequenceHandler play;
        SpuSequenceHandler crescendo;
        SpuSequenceHandler decrescendo;
        SpuSequenceStep tempo;
        SpuSequenceHandler pause;
        SpuSequenceStep resume;
        SpuSequenceHandler stop;
    } SpuSequencePumpOps;

    typedef uint32 (*SpuSequenceInitialize)(sint16 sequence, sint16 bank, const uint8 *data);
    typedef uint32 (*SpuSequenceVmInitialize)(uint8 voices);

    typedef struct SpuSequenceHandlers
    {
        SpuSequenceNoteHandler note_on;
        SpuSequenceValueHandler program_change;
        SpuSequenceHandler pitch_bend;
        SpuSequenceHandler meta_event;
        SpuSequenceValueHandler control_change;
        SpuSequenceValueHandler data_entry;
        SpuSequenceValueHandler volume;
        SpuSequenceValueHandler pan;
        SpuSequenceValueHandler expression;
        SpuSequenceValueHandler parameter_lsb;
        SpuSequenceValueHandler parameter_msb;
        SpuSequenceValueHandler registered_parameter_lsb;
        SpuSequenceValueHandler registered_parameter_msb;
        SpuSequenceValueHandler reverb_depth;
        SpuSequenceHandler reset_controllers;
        SpuSequenceValueHandler damper;
        SpuSequenceValueHandler portamento;
    } SpuSequenceHandlers;

    extern SpuSequenceScheduler spu_sequence_scheduler;
    extern SpuToneCommand spu_tone_commands[20];
    extern SpuSequenceHandlers spu_sequence_handlers;

    uint32 spu_sequence_scheduler_data_entry(SpuSequenceSchedulerTrack *record, sint16 sequence, sint16 separation, uint8 value, const SpuToneCommand *commands, uint32 command_count, SpuSequenceHandler read_delta);
    uint32 spu_sequence_scheduler_replay_mode(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 track);
    uint32 spu_sequence_scheduler_play_mode(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 track, uint8 mode, sint16 repeat, SpuSequenceVolumeSet set_volume);
    uint32 spu_sequence_scheduler_stop(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 track, SpuSequencePackedHandler key_off);
    uint32 spu_sequence_scheduler_pause(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, SpuSequencePackedHandler key_off);
    void spu_sequence_scheduler_resume(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation);
    uint32 spu_sequence_scheduler_request_pause(SpuSequenceScheduler *scheduler, uint32 sequence, uint32 track, SpuSequenceVolumeGet get_volume);
    uint32 spu_sequence_scheduler_close(SpuSequenceScheduler *scheduler, sint16 sequence, SpuSequenceVolumeSet set_volume, SpuSequencePackedHandler key_off);
    void spu_sequence_scheduler_pump(SpuSequenceScheduler *scheduler, const SpuSequencePumpOps *ops);
    void spu_sequence_bind_pump(const SpuSequencePumpOps *ops);
    void SsSeqCalledTbyT(void);
    uint32 spu_sequence_scheduler_dispatch_event(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, SpuSequenceHandlers *handlers, SpuSequenceHandler read_delta, SpuSequenceValueHandler meta_dispatch);
    uint32 spu_sequence_scheduler_volume(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, uint8 value, SpuSequenceVoiceVolumeSet set_voice_volume, SpuSequenceHandler read_delta);
    uint32 spu_sequence_scheduler_pan(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, uint8 value, SpuSequenceVoiceVolumeSet set_voice_volume, SpuSequenceHandler read_delta);
    uint32 spu_sequence_scheduler_reset_controllers(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, SpuSequenceReverbEnable set_reverb, SpuSequenceHandler read_delta);
    uint32 spu_sequence_scheduler_nrpn_lsb(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, uint8 value, SpuSequenceMarkHandler mark, SpuSequenceHandler read_delta);
    uint32 spu_sequence_scheduler_damper(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, uint8 value, SpuSequenceHandler read_delta);
    uint32 spu_sequence_scheduler_reverb_depth(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, uint8 value, SpuSequenceReverbDepthSet set_reverb_depth, SpuSequenceHandler read_delta);
    uint32 spu_sequence_scheduler_nrpn_msb(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, uint8 value, SpuSequenceHandler read_delta);
    uint32 spu_sequence_scheduler_rpn_lsb(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, uint8 value, SpuSequenceHandler read_delta);
    uint32 spu_sequence_scheduler_rpn_msb(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, uint8 value, SpuSequenceHandler read_delta);
    uint32 spu_sequence_scheduler_expression(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, uint8 value, SpuSequenceVoiceVolumeSet set_voice_volume, SpuSequenceHandler read_delta);
    uint32 spu_sequence_scheduler_damper(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, uint8 value, SpuSequenceHandler read_delta);
    uint32 spu_sequence_scheduler_control_change(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, uint8 controller, SpuSequenceHandlers *handlers, SpuSequenceHandler read_delta);
    uint32 spu_sequence_scheduler_meta_event(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, SpuSequenceHandler read_delta);
    uint32 spu_sequence_scheduler_read_event(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, SpuSequenceHandlers *handlers, SpuSequenceHandler read_delta, SpuSequenceHandler end_of_track);
    uint32 spu_sequence_scheduler_end_of_track(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, SpuSequenceHandler next_separation, SpuSequencePackedHandler key_off);
    uint32 spu_sequence_scheduler_program_change(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, uint8 program, SpuSequenceHandler read_delta);
    uint32 spu_sequence_scheduler_pitch_bend(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, SpuSequencePitchSet set_pitch, SpuSequenceHandler read_delta);
    uint32 spu_sequence_scheduler_note_on(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, uint8 note, uint8 velocity, SpuSequenceVoiceKeyOn key_on, SpuSequenceVoiceKeyOff key_off);
    uint32 spu_sequence_scheduler_next_separation(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation);
    uint32 spu_sequence_scheduler_read_delta(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation);

    typedef struct SpuVoiceState
    {
        uint16 sample, age, pitch, envelope, velocity;
        uint8 pan;
        uint16 channel, note, sequence, program_tone_index, program, tone, bank, priority;
        uint8 status;
        uint16 volume_modulation[6];
        uint16 pitch_modulation[6];
    } SpuVoiceState;

    typedef void (*SpuVoiceModulationCallback)(uint32 voice);

    typedef struct SpuVoiceModulationCallbacks
    {
        SpuVoiceModulationCallback volume;
        SpuVoiceModulationCallback pitch;
    } SpuVoiceModulationCallbacks;

    extern SpuVoiceModulationCallbacks spu_voice_modulation_callbacks;

    void spu_sequence_vm_volume_modulation_start(sint16 voice, sint16 from, sint16 to, sint16 ticks);
    void spu_sequence_vm_pan_modulation_start(sint16 voice, sint16 from, sint16 to, sint16 ticks);
    void spu_sequence_vm_volume_modulation_tick(uint32 voice);
    void spu_sequence_vm_pan_modulation_tick(uint32 voice);

    typedef struct SpuNoteContext
    {
        uint8 tone_count;
        uint8 bank;
        uint8 note;
        uint8 note_fraction;
        uint8 velocity;
        uint8 pan;
        uint8 program;
        uint8 program_tone_index;
        uint8 program_volume;
        uint8 program_pan;
        uint8 tone;
        uint8 tone_volume;
        uint8 tone_pan;
        uint8 priority;
        uint8 center_note;
        uint8 fine_tune;
        uint8 min_key;
        uint8 max_key;
        uint8 tone_mode;
        uint16 sequence;
        uint16 sample;
        uint16 voice;
        uint16 register_index;
        uint16 tone_index;
    } SpuNoteContext;

    typedef struct SpuVoiceEventState
    {
        uint16 key_on[2];
        uint16 key_off[2];
        uint16 reverb[2];
        uint16 noise[2];
        uint32 silence_history[16];
        uint32 history_index;
        uint8 voice_count;
        uint8 silence_cleanup_disabled;
    } SpuVoiceEventState;

    typedef struct SpuVmState
    {
        SpuVoiceState voices[24];
        uint32 heap_records[2 * 33];
        SpuNoteContext note;
        SpuVoiceEventState events;
        uint16 registers[24 * 8];
        uint8 dirty[24];
        uint32 protection;
        uint16 release_offset;
        uint16 mono_mode;
        uint32 immediate_key_on_busy;
    } SpuVmState;

    SpuVmState *spu_vm_state(void);
    SpuVoiceState *spu_vm_voice(uint32 voice);
    void spu_vm_reset(void);

    typedef struct SpuVoiceRegisterCache
    {
        uint8 bank_flags[16];
        uint16 registers[24 * 8];
        uint8 dirty[24];
    } SpuVoiceRegisterCache;

    typedef struct SpuVabBank
    {
        uint8 *header;
        uint32 bytes;
        uint8 *programs;
        uint32 program_bytes;
        uint8 *tones;
        uint32 tone_bytes;
        uint8 *sample_end;
    } SpuVabBank;

    typedef struct SpuVabState
    {
        SpuVabBank banks[16];
        SpuVabBank *current;
        uint16 program_capacity;
    } SpuVabState;

    typedef struct SpuVabTransferState
    {
        uint32 sample_bytes[16];
        uint32 spu_addresses[16];
        uint16 bank_count;
    } SpuVabTransferState;

    typedef struct SpuMallocGuestBinding
    {
        uint32 shift_address;
        uint32 alignment_address;
        uint32 count_address;
        uint32 last_index_address;
        uint32 table_address;
        uint32 reserved_enabled_address;
        uint32 reserved_units_address;
    } SpuMallocGuestBinding;

    sint32 spu_bind_malloc_guest(const SpuMallocGuestBinding *binding);
    uint32 spu_init_malloc_guest(uint32 count, uint32 table);
    uint32 spu_sequence_mark_table(void);

    typedef struct SpuSequenceTrack
    {
        SpuSequenceStream stream;
        uint8 playing;
        uint8 loop_active;
        uint8 running_status;
        uint8 channel;
        // TODO Name unimplemented controller byte roles from complete SDK operations
        uint8 control_data[8];
        uint8 play_limit;
        uint8 plays_done;
        sint8 next_sequence;
        sint8 next_track;
        uint8 time_signature[2];
        uint8 bank;
        uint8 channel_pan[16];
        uint8 channel_program[16];
        uint16 volume_step;
        uint16 volume_prev;
        uint16 tempo_step;
        uint16 tempo_interval;
        uint16 resolution;
        uint16 countdown;
        uint16 tick_step;
        uint16 initial_tick_step;
        uint16 volume_left;
        uint16 volume_right;
        uint16 saved_left;
        uint16 saved_right;
        uint16 channel_volume[16];
        uint16 muted_channels;
        uint32 initial_delta;
        uint32 elapsed_delta;
        uint32 initial_tempo;
        uint32 delta;
        uint32 tempo;
        uint32 flags;
        uint32 volume_duration;
        uint32 volume_elapsed;
        uint32 tempo_duration;
        uint32 tempo_remaining;
        uint32 tempo_target;
    } SpuSequenceTrack;

    typedef uint32 (*SpuSequenceNoteCallback)(uint32 sequence, uint32 track, uint32 note, uint32 velocity);
    typedef uint32 (*SpuSequenceEventCallback)(uint32 sequence, uint32 track, uint32 value);
    typedef uint32 (*SpuSequencePairCallback)(uint32 sequence, uint32 track);
    typedef void (*SpuSequenceMarkCallback)(sint16 sequence, sint16 track, sint16 value);
    typedef void (*SpuSequenceVabCallback)(sint16 sequence, sint16 track, sint16 program, VagAtr attributes, sint16 tone, uint8 value);

    typedef struct SpuSequenceCallbacks
    {
        SpuSequenceNoteCallback note;
        SpuSequenceEventCallback program;
        SpuSequencePairCallback pitch;
        SpuSequenceEventCallback meta;
        SpuSequenceEventCallback control;
        SpuSequencePairCallback bank;
        SpuSequenceEventCallback data_entry;
        SpuSequenceEventCallback volume;
        SpuSequenceEventCallback pan;
        SpuSequenceEventCallback expression;
        SpuSequenceEventCallback damper;
        SpuSequenceEventCallback nrpn_lsb;
        SpuSequenceEventCallback nrpn_msb;
        SpuSequenceEventCallback rpn_lsb;
        SpuSequenceEventCallback rpn_msb;
        SpuSequenceEventCallback reverb;
        SpuSequencePairCallback reset_all;
        SpuSequenceVabCallback attributes[20];
        SpuSequenceMarkCallback marks[32][16];
    } SpuSequenceCallbacks;

    SpuSequenceCallbacks *spu_sequence_callbacks(void);
    void spu_sequence_clear_marks(void);
    uint32 spu_sequence_dispatch_note(uint32 sequence, uint32 track, uint32 note, uint32 velocity);
    uint32 spu_sequence_dispatch_program(uint32 sequence, uint32 track, uint32 program);
    uint32 spu_sequence_dispatch_pitch(uint32 sequence, uint32 track);
    uint32 spu_sequence_dispatch_meta(uint32 sequence, uint32 track, uint32 event);
    uint32 spu_sequence_dispatch_control(uint32 sequence, uint32 track, uint32 control);
    uint32 spu_sequence_dispatch_controller(uint32 sequence, uint32 track, uint8 control, uint8 value);
    void spu_sequence_dispatch_mark(sint16 sequence, sint16 track, sint16 value);

    typedef struct SpuSequenceClock
    {
        sint32 mode;
        uint32 manual;
        PsxInterruptCallback tick;
        PsxInterruptCallback previous_vblank;
        uint8 direct_vblank;
        uint8 half_rate;
        uint8 interrupt;
        uint32 phase;
    } SpuSequenceClock;

    uint32 spu_sequence_scheduler_set_table(SpuSequenceScheduler *scheduler, SpuSequenceSchedulerTrack *table, sint16 sequences, sint16 separations);
    uint32 spu_sequence_scheduler_play_tick(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, SpuSequenceHandler read_event);
    uint32 spu_sequence_scheduler_crescendo(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, SpuSequenceVolumeGet get_volume, SpuSequenceVolumeSet set_volume);
    uint32 spu_sequence_scheduler_decrescendo(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation, SpuSequenceVolumeGet get_volume, SpuSequenceVolumeSet set_volume);
    void spu_sequence_scheduler_tempo_ramp(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 separation);
    uint32 spu_sequence_scheduler_tick_mode(SpuSequenceScheduler *scheduler, SpuSequenceClock *clock, uint32 mode, uint32 video);

    uint32 spu_sequence_scheduler_initialize_track(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 bank, const uint8 *data, SpuSequenceHandler read_delta);
    uint32 spu_sequence_scheduler_open(SpuSequenceScheduler *scheduler, const uint8 *sequence, uint32 bytes, sint16 bank, SpuSequenceInitialize initialize, SpuSequenceHandlers *handlers, const SpuSequenceHandlers *default_handlers, SpuToneCommand *commands, const SpuToneCommand *default_commands);
    uint32 spu_sequence_scheduler_initialize(SpuSequenceScheduler *scheduler, SpuSequenceVmInitialize initialize_voices);
    void spu_sequence_scheduler_irq0(SpuSequenceClock *clock);
    void spu_sequence_scheduler_half_rate(SpuSequenceClock *clock);
    void spu_sequence_scheduler_stop_timer(SpuSequenceClock *clock);
    void spu_sequence_scheduler_start_timer(SpuSequenceClock *clock, uint32 mode, PsxInterruptCallback irq0, PsxInterruptCallback half_rate);

    uint32 spu_sequence_vm_set_sequence_volume(SpuSequenceScheduler *scheduler, sint16 handle, sint16 left, sint16 right, sint16 mode);
    sint16 SsIsEos(sint16 sequence, sint16 separation);
    void SsSeqSetVol(sint16 sequence, sint16 left, sint16 right);
    uint32 SsSeqPlay(sint16 sequence, uint8 mode, sint16 repetitions);
    void SsSeqStop(sint16 sequence);
    uint32 spu_sequence_vm_get_sequence_volume(SpuSequenceScheduler *scheduler, sint16 handle, uint16 *left, uint16 *right);
    uint32 spu_sequence_vm_sequence_key_off(sint16 sequence);
    uint32 spu_sequence_vm_set_volume(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 bank, sint16 program, uint16 volume, uint16 pan);
    uint32 spu_sequence_vm_key_off_now(void);
    uint32 spu_sequence_vm_noise_voice(uint32 mode, uint32 voices);
    uint32 spu_sequence_vm_allocate(void);
    uint32 spu_sequence_vm_prepare_voice(void);
    uint16 spu_sequence_vm_note_pitch(void);
    uint32 spu_sequence_vm_noise_off(uint8 voice);
    uint32 spu_sequence_vm_select_tones(uint8 *tones, uint8 *samples);
    uint16 spu_sequence_vm_tone_pitch(uint16 note, uint16 fraction);
    uint32 spu_sequence_vm_voice_bend(sint16 voice, sint16 sequence, sint16 bank, sint16 program, uint16 bend);
    void spu_sequence_vm_voice_bend_offset(uint8 voice, sint16 bend);
    sint32 spu_sequence_vm_voice_set_pitch(uint16 voice, sint16 bank, sint16 program, sint16 note, uint16 new_note, uint16 fine);
    sint32 spu_sequence_vm_voice_set_adsr(uint16 voice, sint16 bank, sint16 program, sint16 note, uint16 adsr1, uint16 adsr2);
    uint32 spu_sequence_vm_sequence_bend(sint16 sequence, sint16 bank, sint16 program, uint16 bend);
    uint32 spu_sequence_vm_key_off(sint16 sequence, sint16 bank, sint16 program, uint16 note);

    extern SpuReverbAttr spu_reverb_attributes;

    uint32 spu_sequence_vm_flush(void);
    uint32 spu_sequence_vm_initialize(uint8 voices);
    uint32 spu_sequence_vm_key_on(SpuSequenceScheduler *scheduler, sint16 sequence, sint16 bank, sint16 program, uint16 note, uint16 velocity, uint16 pan);
    uint32 spu_sequence_vm_noise_on(SpuSequenceScheduler *scheduler, uint8 voice);
    void spu_sequence_vm_noise_key_on_direct(uint8 voice, sint16 left, sint16 right);
    void spu_sequence_vm_noise_key_on_allocated(sint16 left, sint16 right);
    uint32 spu_sequence_vm_key_on_now(SpuSequenceScheduler *scheduler, uint32 layers, sint16 pitch);
    PsxInterruptCallback SpuSetTransferCallback(PsxInterruptCallback callback);
    uint32 spu_sequence_vm_transfer_mode(uint32 mode);
    uint32 spu_sequence_vm_reverb_depth(sint16 left, sint16 right);
    uint32 spu_sequence_vm_reverb_type(uint32 mode);
    uint32 spu_sequence_vm_reverb_feedback(uint8 value);
    uint32 spu_sequence_vm_reverb_delay(uint8 value);

    sint32 _spu_init(uint32 mode);
    void spu_sequence_vm_reset_device(uint32 mode);
    void spu_sequence_vm_dma(void);
    PsxInterruptCallback spu_sequence_vm_register_dma(PsxInterruptCallback callback);
    void spu_sequence_vm_quit(void);
    uint32 spu_sequence_vm_initialize_device(uint32 mode, PsxInterruptCallback dma_callback);

    SpuSequenceClock *spu_sequence_clock(void);
    void spu_sequence_set_tick_mode(sint32 mode);
    void spu_sequence_start_timer(uint32 vblank_counter);
    void spu_sequence_stop_timer(void);

    typedef struct SpuSequenceManager
    {
        uint32 locked;
        uint32 opened;
        uint32 tick_rate;
        sint16 sequence_count;
        sint16 track_count;
    } SpuSequenceManager;

    SpuSequenceManager *spu_sequence_manager(void);
    SpuSequenceTrack *spu_sequence_track(sint16 sequence, sint16 track);
    void spu_sequence_reset(void);

    void spu_sequence_stream_init(uint32 sequences, uint32 tracks);
    SpuSequenceStream *spu_sequence_stream(sint16 sequence, sint16 track);
    void spu_sequence_stream_bind(SpuSequenceStream *stream, const uint8 *data, uint32 size);
    void spu_sequence_stream_clear(SpuSequenceStream *stream);
    uint8 spu_sequence_stream_byte(const SpuSequenceStream *stream, uint32 offset);
    uint32 spu_sequence_stream_delta(SpuSequenceStream *stream, uint32 *elapsed);

    extern const uint16 spu_sequence_voice_defaults[8];
    extern const uint16 spu_sequence_common_defaults[16];
    extern const uint16 spu_note_pitch_table[192];

    typedef struct SpuVmGuestBinding
    {
        uint32 context;
        uint32 voices;
        uint32 program_pointer;
        uint32 header_pointer;
        uint32 tone_pointer;
        uint32 bank_flags;
        uint32 program_limit;
        uint32 header_table;
        uint32 program_table;
        uint32 tone_table;
        uint32 voice_count;
        uint32 protection;
        uint32 channel_masks;
        uint32 release_offset;
        uint32 mono_mode;
        uint32 voice_masks;
        uint32 pending_keyoff;
    } SpuVmGuestBinding;

    sint32 spu_bind_vm_guest(const SpuVmGuestBinding *binding);
    void spu_vm_guest_binding(SpuVmGuestBinding *binding);
    void spu_voice_cache_binding(uint32 *values, uint32 *flags);

    typedef struct SpuHeapBinding
    {
        uint32 *entries;
        uint32 *last_index;
        uint32 capacity;
        uint32 limit;
        uint32 address_shift;
        uint32 alignment_mask;
        uint32 reserved_bytes;
    } SpuHeapBinding;

    sint32 spu_bind_heap(const SpuHeapBinding *binding);
    sint32 spu_heap_failed(void);

    typedef struct SndVoiceStats
    {
        sint16 vagId;
        sint16 vabId;
        uint16 pitch;
        sint16 note;
        sint16 tone;
        sint16 prog_num;
        sint16 prog_actual;
        sint16 vol;
        sint16 pan;
    } SndVoiceStats;

    void SsSetSerialAttr(sint8 serial, sint8 attribute, sint8 value);
    sint8 SsSetReservedVoice(sint8 count);
    sint8 SsBlockVoiceAllocation(void);
    sint8 SsUnBlockVoiceAllocation(void);
    sint32 SsAllocateVoices(uint8 count, uint8 priority);
    void SsSetVoiceSettings(sint32 voice, SndVoiceStats *settings);
    sint16 SsGetActualProgFromProg(sint16 bank, sint16 program);
    sint32 SsUtGetVagAddr(sint16 bank, sint16 vag);
    void SsFlush(void);
    uint32 SpuSetNoiseVoice(sint32 mode, uint32 mask);
    sint16 SsUtSetReverbType(sint16 type);
    void SsUtSetReverbDepth(sint16 left, sint16 right);
    void SsSetRVol(sint16 left, sint16 right);
    void SsGetSerialVol(uint8 serial, uint16 *volume);
    sint32 SpuInitMalloc(sint32 count, char *records);
    sint32 SpuMalloc(sint32 bytes);
    void SpuFree(uint32 address);

    uint32 xport_guest_buffer_address(void *host_buffer, size_t bytes);

    typedef struct SpuDmaEventState
    {
        uint32 initialized;
        uint32 event_handle;
    } SpuDmaEventState;

    extern SpuDmaEventState spu_dma_event_state;

    typedef struct SpuKeyControl
    {
        uint32 key_state;
        uint32 event_mode;
        uint32 event_flags;
        uint32 pending_state;
        uint16 key_on[2];
        uint16 key_off[2];
        uint16 pitch_mask[2];
        uint16 noise_mask[2];
        uint16 reverb_mask[2];
    } SpuKeyControl;

    extern SpuKeyControl *const spu_key_control;
    uint32 spu_set_key(sint32 on_off, uint32 voice_bit);
    uint32 _SsUtResolveADSR(uint16 first, uint16 second, uint16 *fields);
    uint32 _SsUtBuildADSR(const uint16 *fields, uint16 *first, uint16 *second);
    extern uint16 *const spu_sample_notes;
    extern const uint8 spu_silent_sample[16];
    void _spu_writeByIO(const uint8 *source, uint32 bytes);

    typedef struct SpuNativeTransferState
    {
        uint32 requested_mode;
        uint32 normalized_mode;
        uint32 completion;
        PsxInterruptCallback completion_callback;
        uint32 source_address;
        uint32 dma_blocks;
        uint16 address_units;
    } SpuNativeTransferState;

    extern SpuNativeTransferState *const spu_native_transfer_state;

    typedef struct SpuNativeTransferGuestBinding
    {
        uint32 requested_mode;
        uint32 normalized_mode;
        uint32 address_units;
        uint32 completion;
        uint32 completion_callback;
        uint32 source_address;
        uint32 dma_blocks;
        uint32 address_shift;
    } SpuNativeTransferGuestBinding;

    sint32 spu_bind_native_transfer_guest(const SpuNativeTransferGuestBinding *binding);

    typedef struct SpuTransferBinding
    {
        uint32 *requested_mode;
        uint32 *normalized_mode;
        uint16 *address_units;
        uint32 *completion;
        uint32 *completion_callback;
        uint32 *source_address;
        uint32 *dma_blocks;
        uint32 address_shift;
        uint32 address_rounding_enabled;
        uint32 address_rounding_unit;
        uint32 address_alignment_mask;
        uint32 block_bytes;
        uint32 ticks_per_block;
        uint64 (*clock)(void *context);
        sint32 (*wait_until)(void *context, uint64 deadline);
        sint32 (*complete_event)(void *context);
        uint32 (*guest_address)(void *context, const void *source);
        void *context;
        PsxInterruptCallback *native_completion_callback;
    } SpuTransferBinding;

    sint32 spu_bind_transfer(const SpuTransferBinding *binding);
    sint32 spu_clear_reverb_transfer(sint32 mode, const void *source, const SpuTransferBinding *binding, uint32 *direction);
    sint32 spu_clear_reverb_work_area(sint32 mode, const SpuTransferBinding *binding, uint32 *direction);
    sint32 spu_clear_reverb_guest(const SpuNativeTransferGuestBinding *guest, uint32 begin, uint32 bytes, uint32 source, uint32 direction);
    sint32 spu_clear_reverb_native(uint32 begin, uint32 bytes);
    sint32 spu_transfer_failed(void);
    sint32 SpuSetTransferMode(sint32 mode);
    uint32 SpuSetTransferStartAddr(uint32 address);
    uint32 SpuWrite(const void *source, uint32 byte_count);
    sint32 spu_stop_transfer(void);
    uint32 spu_read_blocks(void *destination, uint16 address_units, uint16 blocks);
    sint32 SpuReadDecodedData(void *destination, sint32 flags);
    uint32 spu_write_host(const void *source, uint32 byte_count);
    sint32 SpuIsTransferCompleted(sint32 mode);

    /* Exposed for deterministic block vectors. `history1` is the immediately
 * preceding decoded sample; `history2` is the sample before it. */

    /* BEGIN GENERATED MODULE API */
    SPU_ADSR_PHASE spu_voice_phase(sint32 voice);
    sint32 spu_decode_adpcm_block(const uint8 block[16], sint16 *history1, sint16 *history2, sint16 output[28]);
    sint32 spu_upload(uint32 byte_address, const void *source, uint32 byte_count);
    sint32 spu_download(uint32 byte_address, void *destination, uint32 byte_count);
    sint32 spu_load_ram_file(const char *path);
    sint32 spu_load_handoff_file(const char *path);
    void spu_mmio_initialize(void);
    sint32 spu_mmio_read(void *user, uint32 physical_address, uint32 width, uint32 *value);
    sint32 spu_mmio_write(void *user, uint32 physical_address, uint32 width, uint32 value, uint32 byte_mask);
    void spu_dispatch_pending_irq(void);
    const uint8 *spu_ram_data(void);

    typedef struct SpuVabNativeView
    {
        const uint8 *header;
        const uint8 *programs;
        const uint8 *tones;
        uint32 header_bytes;
        uint32 program_bytes;
        uint32 tone_bytes;
        uint32 body_address;
        uint32 body_bytes;
    } SpuVabNativeView;

    sint16 spu_vab_open_head_sticky(const uint8 *header, uint32 header_bytes, sint16 bank, uint32 body_address);
    sint16 spu_vab_trans_body_native(const uint8 *body, uint32 body_bytes, sint16 bank);
    sint32 spu_vab_select_native(uint16 bank, sint16 program);
    uint32 spu_vab_set_program_volume(sint16 bank, sint16 program, uint8 value);
    uint32 spu_vab_set_program_pan(sint16 bank, sint16 program, uint8 value);
    sint16 spu_vab_get_vag_atr_native(sint16 bank, sint16 program, sint16 tone, VagAtr *output);
    uint32 spu_vab_nrpn_attr_by_value(sint16 attribute, sint16 bank, sint16 program, sint16 tone, VagAtr attributes, uint8 value);
    sint32 spu_vab_native_view(sint16 bank, SpuVabNativeView *view);
    void spu_vab_reset_native(void);
    uint32 spu_vab_close_sticky(sint16 bank);
    SpuVabNativeView spu_vab_native_current(void);
    SpuVabNativeView spu_vm_native_bank(void);
    sint32 spu_native_vab_address(sint16 bank);
    void spu_native_set_mute(sint32 muted);
    uint64 spu_voice_sample_position(sint32 voice);
    uint32 spu_end_flags(void);
    uint16 spu_voice_envelope(sint32 voice);
    void spu_mix(sint32 *interleaved_stereo, uint32 frame_count);
    void spu_render(sint16 *interleaved_stereo, uint32 frame_count);
    typedef uint32 (*SpuCdPcmSource)(void *context, sint16 *output, uint32 frames);
    // Supply 44100 Hz stereo PCM through the existing CD volume and reverb path
    void spu_set_cd_pcm_source(SpuCdPcmSource source, void *context);
    sint32 spu_take_xa_pcm(XportFmvAudioRemainder *audio);
    sint32 spu_save_xa_pcm(void);
    void spu_discard_xa_pcm(void);
    void spu_render_vblank(sint16 *interleaved_stereo, uint32 frame_count);

    /* END GENERATED MODULE API */

    sint32 cd_stream_pop_sector_native(uint8 output[2352]);
    sint32 cd_restore_fifo_native(const uint8 *bytes, uint32 count, uint32 mode, uint32 next_sector);
    sint32 cd_read_sector_fifo_native(uint32 sector, uint32 mode);
    void cd_bind_command_native(void (*command)(uint8 command, uint32 next_sector, uint32 mode));
    void cd_set_reading_native(sint32 reading);
    void cd_stream_consumer_native(sint32 enabled);

#if defined(__cplusplus)
}
#endif

#endif

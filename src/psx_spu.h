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

    void cd_dispatch_ready_event(void);
    sint32 CdInit(void);
    sint32 cd_mount_cue(const char *cue_path);
    sint32 cd_mount_stream_file(const char *path);
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
    sint32 SpuInitMalloc(sint32 count, char *records);
    sint32 SpuMalloc(sint32 bytes);
    void SpuFree(uint32 address);

    uint32 xport_guest_buffer_address(void *host_buffer, size_t bytes);

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
    } SpuTransferBinding;

    sint32 spu_bind_transfer(const SpuTransferBinding *binding);
    sint32 spu_transfer_failed(void);
    sint32 SpuSetTransferMode(sint32 mode);
    uint32 SpuSetTransferStartAddr(uint32 address);
    uint32 SpuWrite(const void *source, uint32 byte_count);
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
    sint32 spu_mmio_read(void *user, uint32 physical_address, uint32 width, uint32 *value);
    sint32 spu_mmio_write(void *user, uint32 physical_address, uint32 width, uint32 value, uint32 byte_mask);
    const uint8 *spu_ram_data(void);
    sint32 spu_native_vab_address(sint16 bank);
    void spu_native_set_mute(sint32 muted);
    uint64 spu_voice_sample_position(sint32 voice);
    uint32 spu_end_flags(void);
    uint16 spu_voice_envelope(sint32 voice);
    void spu_mix(sint32 *interleaved_stereo, uint32 frame_count);
    void spu_render(sint16 *interleaved_stereo, uint32 frame_count);
    void spu_render_vblank(sint16 *interleaved_stereo, uint32 frame_count);
    int spu_state_io(FILE *file, int load);
    int spu_lockstep_portable_io(FILE *file, int load);
#if defined(LOCKSTEP_DEBUG)
    size_t spu_lockstep_checkpoint_size(void);
    int spu_lockstep_checkpoint_capture(void *data, size_t size);
    int spu_lockstep_checkpoint_restore(const void *data, size_t size);
    int spu_lockstep_checkpoint_synchronize(void);
    int spu_lockstep_checkpoint_rebase_clock(void);
    int spu_lockstep_portable_selftest(void);
#endif
    /* END GENERATED MODULE API */

    sint32 cd_stream_pop_sector_native(uint8 output[2352]);
    void cd_stream_consumer_native(sint32 enabled);

#if defined(__cplusplus)
}
#endif

#endif

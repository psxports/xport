# Runtime audio contracts

Section routing and verification provenance: [LIBRARY.md](LIBRARY.md).

## Native decoded SPU PCM

- `sint32 SpuReadDecodedData(void *destination, sint32 flags)` reads the existing shared SPU RAM through `spu_download` after existing transfer completion. Require initialized SPU and a writable native destination. Return the native capture cursor half (`0` or `1`); return `-1` for invalid input or failed completion/copy
- Four contiguous banks at SPU RAM offsets `0`, `1024`, `2048`, `3072` contain 512 little-endian signed16 samples each: native CD left/right after CD routing and track volume but before SPU CD volume, then voices 1/3 after ADSR but before voice volume. The existing mixers write their computed samples; there is no second decoder
- Flag `5` copies source bytes `0..2047` to destination `0..2047`; require 2048 destination bytes. Flag `6` copies source `2048..4095` to destination `2048..4095`; require 4096 destination bytes and retain its first half. Other flags copy all 4096 bytes
- One 512-frame cursor advances with generated voice PCM. In ordinary `spu_render`, CD and voice banks share the render positions. Clock-synchronized voice generation advances the same cursor before output and is not advanced twice.
- Reset clears banks/cursor through the existing SPU state reset. Do not use these internal blocks as a stable game-save ABI
- Run `python -B tools/gpu_observer_test.py --fixture spu_decoded --configuration Debug` from a consuming project. `spu_control` and `spu_pending_key` remain regression fixtures; their unused BIOS/IRQ/event boundaries abort explicitly

Game/XA/runtime audio and exact hardware timing remain separate verification

## Native VAB bank ownership

- `vab_open_head` and `spu_vab_open_head_sticky` use separate slots in static `vab_parse_banks[2]` for temporary CPU-side bank metadata. Hold the existing audio lock while writing a staging slot and publishing the validated bank; release it on every failure. Never place the complete `VAB_BANK` staging record on a game stack. VB samples still transfer through `SpuWrite` into SPU RAM
- `SpuVabBank` owns mutable VH bytes and bounded program/tone views;`SpuVabState` holds16 banks,current borrowed owner reference and SDK working program capacity. Keep binary file fields in standard little-endian codecs and SPU RAM allocation addresses numeric

Bank16 is rejected instead of preserving an invalid cache alias;no host pointer is written into encoded tables. P2 VB upload now takes a native input span;separate VB files load into native buffers retained through completion,loop parent17 uses a temporary native buffer. Scene VH/SEQ parent ownership is native with two stream buffers;menu embedded parent ownership remains migration work

## Native SPU DMA registration and event ownership

- Restored original `setIntrDMA(channel,callback)` uses the native callback table and performs the reviewed DICR master/channel enable update only when the callback changes. Callback publication precedes the MMIO update;return the actual previous native pointer. Existing low-level `DMACallback` remains a pointer exchange for current consumers
- `SpuDmaEventState` owns native SPU initialized/BIOS event-handle scalars. P2 init/quit retain event class/spec,critical sections and Close/Disable order. Full P2 driver reset clears actual native DMA registrations before DICR reset;do not maintain a numeric guest callback mirror

User completion callbacks and generic BIOS/IRQ/graph ownership remain separate work

## Native SPU silent sample and FIFO source

- `spu_silent_sample[16]` is the shared immutable PsyQ init data of16 bytes07. Existing common init upload and P2 mode0 reset use this table,which matches original EXE80087D9C bytes;P2 no longer reads that payload from DRAM
- Genuine `_spu_writeByIO(const uint8 *source,uint32 bytes)` reads native little-endian halfwords and preserves FIFO/control/status device access order and existing polling/timeout behavior. Its existing P2 caller supplies the full16-byte span;SPU address units remain numeric device state. No game-local pointer adapter or callback/helper is introduced

## Native sample-note metadata and queued noise

- `spu_sample_notes` points to the single24-word array embedded in SPU_STATE. P2/shared attribute setter,getter and native voice start share it;private per-voice sample-note mirrors are removed. SPU_INITIAL_SAMPLE_NOTE C000 comes from original SDK mode0 initialization;full init resets it,partial P2 init preserves it. Keep P2 original pitch conversion unchanged
- SpuKeyControl noise_mask owns the computed shadow pairB1560+404/+406,not an unused buffer tail. P2 noise selection now uses native event_mode/event_flags and uint16 fields;direct writes remain device MMIO. Read/write order,flagOR4 and24-bit result are preserved

Remaining queue providers/shared algorithm consolidation are not declared complete

## Native SPU key control

- `SpuKeyControl` embedded in SPU_STATE owns the SDK cached key bitmap,event mode/flags,pending-state bitmap and uint16 key-on/off pairs. Public `spu_key_control` points to that single owner.
- Preserve queued register overwrite/opposite cancellation,24-bit masks and direct key MMIO order. LIBSND VM events and physical mixer key state are distinct stages;do not merge their lifetimes or invent a flush/event-mode provider
- Cold/native SDK reset zeroes the owner;P2 hardware reset preserves key-register clear order and retires unread buffer tail/direction writes. Pointer-free control participates in SPU state storage.

## Native SPU transfer callback domain

- `SpuNativeTransferState.completion_callback` is a real PsxInterruptCallback. Its runtime size is not a PS1 binary-layout contract. `SpuTransferBinding` appends native_completion_callback while retaining the existing numeric guest completion_callback slot;exactly one domain must be bound. Keep constructors zero-initialized and rebuild consumers against the updated header
- Transfer presence checks and temporary reverb callback removal/restoration use the selected domain directly. No numeric presence mirror or invented callback provider
- SPU state file save rejects a non-NULL native callback;load stages and rejects such data before replacing live SPU state. Broader callback rebinding and transfer dispatch integration require final validation

## Native SEQ clock callback ownership

Cold defaults preserve original mode60/manual1/interrupt127/phase0 and bind the reviewed tick entry directly

Keep those contracts distinct.

### SsSeqCalledTbyT

- Original `SsSeqCalledTbyT` native entry has a void callback ABI because callers do not consume its result;preserve effects/reentrancy/loop control and original markers. Do not cast incompatible function pointers

Legacy generic IRQ/DMA/graphics and mixed SDK reset ownership remain separate work

## Native voice modulation callbacks

- `spu_voice_modulation_callbacks.volume/pitch` hold actual native functions taking one voice index;they borrow provider lifetime and are outside portable SPU snapshots. P2 and shared native flush dispatch volume before pitch and reload state after callbacks. NULL with active modulation is an explicit failure,never a no-op

Full P2/common VM ownership and mixed-provider semantics remain separate work. Preserve32-bit tick counters,typed track pointers and slot/flag reloads across callbacks

No runtime or original-callee signature equivalence claim

## Native pending sequence teardown

- Before native parent/source teardown,cancel pending loader state in its existing lifecycle operations. Wait for submitted SPU completion before releasing a source;consume the current native archive completion flag,close the explicit pending bank and clear loading fields. Do not reset an unrelated outer level-loader phase

This repairs an original cancellation leak/stale-state path;normal flag1 close results are retained

Arbitrary SDK close concurrency and full outer-loader behavior remain unverified

## Native scene sequence parent resources

- P2 scene VH/SEQ parents now have two native SceneStream owners with exact root extents and sector-rounded read allocations. Keep original role sharing/swaps and release unique owners on reload/reset/unload;do not add an address registry or per-asset cache
- Low-level sequence loading accepts native parent pointers and validates VH/SEQ child spans. The still-borrowed SceneSection uses a temporary small slot index,never a host pointer;menu embedded parents retain one explicit guest input boundary. Copied SDK VH/SEQ owners do not retain the scene parent

This does not close native SceneSection/graph ownership or runtime equivalence

## Native SDK SPU address conversion

- `_spu_FsetRXXa(register_index,address)` restores the genuine LIBSPU two-argument converter with fixed PSX8-byte addressing. Mode-1 returns low16 units,mode-2 returns the rounded full byte address,and other indices write a halfword register then return rounded bytes. Preserve conditional round-up,uint32 wrap and numeric MMIO indexing
- Shared `SPU_RAM_ADDRESS_SHIFT/UNIT/MASK` own3/8/7;use these SDK constants instead of consuming-game defaults. Generic transfer bindings remain parameterized and must not be silently routed through fixed SDK conversion

Existing original markers preserved;no synthetic game helper or runtime equivalence claim

## Native sequence startup and pitch defaults

- `spu_sequence_voice_defaults[8]` and `spu_sequence_common_defaults[16]` expose existing immutable SDK register defaults. Shared initialization and reviewed native callers use one set of literals;keep MMIO order and widths
- `_spu_note2pitch` restores the genuine four-argument SDK converter. Preserve signed16 semitone narrowing,truncating division,remainder and masked shifts;do not substitute a converter with different negative-remainder behavior. Shared immutable `spu_note_pitch_table[192]` supplies both reviewed P2 copies. Out-of-range native indices abort diagnostically,not read neighboring guest bytes;this is an explicit invalid-access repair
- Existing `SpuSequenceCallbacks.marks[32][16]`,dispatch and clear own native marker callbacks.
- P2 startup now owns a native reverb descriptor with explicit mode/depth mask7;the original missing-mask stale-stack bug is intentionally repaired. No runtime equivalence claim

## Native VM controls and reverb inputs

- Reuse `spu_vm_state()->release_offset/mono_mode` for native VM damper and mono state;do not introduce a project scalar mirror. Keep original partial resets and ADSR/volume arithmetic
- `SpuReverbAttr` is a native SDK input descriptor,distinct from active `SpuReverbControl` ownership. Native working descriptors use direct fields and pointers;preserve mask-controlled updates and repeated-reset retention. Unwritten original startup fields require separate bootstrap evidence,not invented zero initialization

Structural work remains unverified;final Debug x86 controls/reverb scenarios pending

## Native VAB sample sources

- `SpuWrite` accepts native sources with the native transfer binding;the current implementation copies source bytes during submission and handles DMA tail padding internally. Keep caller-owned storage alive through its established completion phase;do not serialize host pointers as guest DMA addresses

Original loader phases own separate-file native storage;borrowed menu spans are mapped only at the remaining resource boundary. Loop VH/SEQ copies outlive their released parent bundle

## Native sequence range failures

- Inline owner/cursor/span checks precede current native SEQ byte reads;subtract only after excluding out-of-range offsets. Reject an unavailable sequence slot with the existing open failure result before allocation. Check native tone-table indices before dispatch
- Invalid native byte ranges or command indices stop with diagnostics,not substitute bytes/events. This is an explicit invalid-access bug repair;do not claim original malformed-input equivalence or full parser validation. No project read helper is added

Final Debug x86 validation pending

## Native sequence stream ownership

- `SpuSequenceTrack.stream_data/stream_size` describe caller-managed native stream ownership. Cursor/start/loop uint32 fields are relative file positions; never store host pointers in those fields. Release buffers only after sequence consumers stop,and handle reset/close without double-free
- P2 existing open copies an archive-sized native input span;close/reset release ownership. Scene parent bundles are native two-buffer owners;menu parent input mapping and broader malformed-stream semantics remain open. Per-read owner/cursor/span guards and20-command/installed-slot bounds are implemented inline;invalid native reads/indices abort diagnostically. These guards are not yet runtime-verified. Recording a length alone does not establish a bounded parser

Final Debug x86 lifecycle/loop/tempo verification pending

## Sequence control fields

- `SpuSequenceTrack` exposes native repeat,tick,tempo,ramp,volume,bank and mask fields plus16 channel-volume values. Keep signed16 interpretations and original write truncation explicit. Current native storage and callers use typed records without an original-stride layout requirement
- Event fields now name running status,channel programs/pans,RPN/NRPN phases,loop/chain state and lifecycle latch. Captured flag/volume and pan/program aliases refer to actual native members. `cursor_offset`, `start_offset` and `loop_offset` now hold relative positions in an owned native stream;per-read bounds are implemented inline;broader malformed-stream handling and runtime validation remain open.

## Native sequence track storage

- `SpuSequenceScheduler.tracks[32]` stores typed `SpuSequenceTrack*` pointers to caller-owned native SDK records. Never serialize these pointers or publish them into guest cursor words. Keep supplied storage alive until every sequence consumer releases it
- P2 supplies two native records for reviewed2x1 configuration. Record selection now uses typed pointer arithmetic without a172-byte layout requirement; named fields and member pointers replace byte views. File-relative cursors now reference owned native streams;per-read extent guards are implemented;broader malformed-stream semantics and runtime validation remain open. This does not establish original game C++ classes or final completeness

Earlier numeric track-offset documentation is historical; its migration boundary is superseded. Final Debug x86 lifecycle/volume/tempo verification pending

## Sequence volume output ABI

Keep output write order,original signed return and partial-buffer lifetime. No wrapper/getter was added

- Record120/122 output mappings now use aligned native storage directly. The unused mixed numeric/address return carriers were removed after scoped MIPS/caller review.

## Native sequence scheduling state

- `SpuSequenceScheduler` holds caller-owned rate,slot bitmap,processing guard,clock mode,external-clock setting,sequence/separation counts and timer flags. Moving these fields does not authorize new scheduling policy; preserve integer tempo/target arithmetic,signed reads,callback order and partial reset
- Derive cold values from reviewed executable data instead of zeroing every field. Final Debug x86 tempo/mode/reset verification pending

## Native VAB attribute workspaces

- `SpuToneParameterHandler` accepts a native const byte pointer for32-byte tone input. Keep caller input and modifier output storage distinct when original code copies a by-value record. No host pointer may be converted to a guest stack offset

Temporary sample/tone lists likewise use native caller storage. Resource ownership remains separate from workspace ownership

Final Debug x86 VAB/SEQ/ADSR verification pending

## Native tone command signatures

- `SpuToneCommand` stores distinct parameter-handler and scalar-value-handler pointers. Set only the applicable variant at the original lifecycle boundary and preserve actual return signatures; never cast heterogeneous callbacks into one function type

Encoded VAB temporary buffers remain a separately recorded migration boundary; final Debug x86 controls/ADSR verification pending

## Native sequence handlers

- `SpuSequenceHandlers` is caller-owned native callback storage using distinct note,value and two-argument handler signatures. Preserve return types and original argument narrowing. Do not cast heterogeneous callbacks into one signature or store host pointers in encoded SEQ data
- Initialize native pointers at the original lifecycle boundary and preserve callback selection/order. No new game helper; final Debug x86 integration pending

## Reverb and allocation ownership

- Keep one active `SpuReverbControl` owner. Existing `spu_reverb_export/import` transport local snapshots under SDK locking; do not maintain guest shadow reservation/work fields alongside native mode/enable APIs. Import validates enabled/control consistency and resets the native reverb cursor
- Bind allocation records and their actual last-index owner through `SpuHeapBinding` when shared reverb occupancy checks must inspect a legacy native allocator. Supply count+1 capacity and preserve pointer lifetime; unbind when the owner resets. Copied configuration fields are not live reservation state; a legacy allocator that queries active reservation must continue doing so

Final Debug x86 reverb,reservation and repeated-init scenarios pending

## Native voice lifecycle records

- `SpuVoiceState` contains caller-owned sample,age,pitch,envelope,velocity,pan,note,sequence,program/tone/bank,priority,status and modulation state. It has no host pointer and no portable binary layout. Keep numeric identities and explicit signed/partial-width reads at their original consumers
- P2 supplies24 records and preserves original selection tie order,aging,numeric52-byte-offset return and partial modulation initialization. Existing shared VM APIs are unchanged; final Debug x86 integration pending

## Native note working context

- `SpuNoteContext` provides caller-owned byte/halfword SDK working scalars for program,tone,note,volume/pan,pitch,sequence,sample and voice selection. It contains no resource or host pointer. Keep signed interpretation and partial-width reads explicit at their original consumers; the native layout is not an encoded VAB/SEQ record

No mixer or sequence scheduling change; final Debug x86 validation pending

## Native voice event state

- `SpuVoiceEventState` holds caller-owned key-on/key-off/reverb low/high halfword masks,16 silence-history words,a history index,voice count and silence-cleanup flag. Keep device addresses and shift/narrowing semantics numeric; the record is native state, not a serialized format
- Migrate queueing, cancellation, flush and reset consumers together. Preserve the original history window and partial-reset behavior; capacity does not prove every history record participates in a reduction. Keep actual SPU writes through the shared device interface

No scheduling or device algorithm change; final Debug x86 validation pending

## Native voice register cache

- `SpuVoiceRegisterCache` is caller-owned native SDK state:16 VAB flags,192 halfword register values (24 voices times8),24 dirty flags. The original register slot order is left/right volume,pitch,start,ADSR1,ADSR2,envelope,repeat. Keep halfword truncation and existing dirty-bit flush order

The former voice0 byte alias and its layout assertion are removed;bank flags and voice registers are native fields with no alias-layout requirement. Do not treat this native record as a portable save format

No scheduler change or required migration for other games; final project Debug x86 validation remains pending

## Native VAB transfer metadata

- `SpuVabTransferState` is caller-owned SDK metadata: sixteen sample byte lengths, sixteen numeric SPU RAM addresses and a 16-bit bank count. It owns no sample buffer and contains no host pointers. Keep VAB file decoding, lifecycle and buffer ownership explicit at the caller
- Native metadata does not expand the sixteen-bank domain. When migrating a legacy caller that admits an out-of-table index, review original aliases before replacing encoded accesses. Do not index a native array beyond its declared size or change the original test without reviewed behavior evidence

Flags and borrowed VAB resource cursors remain separate migration work; final integration validation pending

## SPU device constants and default reverb regions

- `psx_spu.h` publishes `SPU_REGISTER_BASE` and the SPU DMA address/block/control/priority/delay register constants as numeric device addresses. Keep actual device access through standard xport memory accessors; these values are not host pointers
- `spu_reverb_default_work[10]` exposes the existing immutable PsyQ work-region table in eight-byte SPU address units. Index only after the caller's original mode-domain check. This default table is independent of the private copies installed by `spu_bind_reverb_presets`; no override mutates it

No scheduling or device algorithm change; final integration validation pending

## Native SPU transfer state

- `SpuNativeTransferState` in `psx_spu.h` exposes the existing device transfer record through `SpuNativeTransferState *const spu_native_transfer_state`; it adds no second mutable owner. Fields are `requested_mode`, `normalized_mode`, `completion`, `completion_callback`, `source_address`, `dma_blocks` and `address_units`. Preserve fixed widths and the existing 28-byte snapshot layout; the public pointer is not serialized
- Use these fields only with the native transfer binding installed by `SpuInit`. A game that migrates legacy SDK state must remove its guest binding and supply reviewed cold defaults and original reset writes. Do not read these fields as substitutes for an active guest/custom binding. Existing guest/custom APIs remain unchanged
- `completion_callback` retains the existing encoded SDK callback identity; this API does not convert it into a host function pointer. `source_address` is numeric guest DMA metadata and is zero for a native-only source; never narrow a host pointer into it
- `spu_clear_reverb_native(begin, bytes)` clears an exact SPU RAM byte range through the current native binding and the existing transfer-region algorithm. It uses a shared immutable 1024-byte silence block. It returns `-1` unless the binding is native, `begin` is inside SPU RAM, the range fits, address shift is at most three and no transfer is pending, active or failed. It does not install a binding or reset transfer state. Preserve the existing region algorithm's mode/callback save and restore, chunking, completion and return semantics
- These APIs change ownership and source-buffer boundaries only; mixer, ADSR, SEQ scheduling and transfer wait policy remain unchanged. Structural edits are unverified; final project Debug x86 integration and SDK boundary scenarios remain pending

## SPU reverb work-area transfers

`SpuClearReverbWorkArea` and the `0x100` clear flag in `SpuSetReverbModeParam` use transfers of at most1024 bytes, rounded up to64-byte DMA blocks with SPU RAM wrapping. Mode4's final992-byte chunk therefore also clears32 bytes at SPU RAM offset0. Each block invokes the existing transfer wait/completion boundary outside the audio lock. Clear temporarily suppresses the completion callback and changes normalized mode1 to0, restores those fields on success and failure, and never changes the normal `SpuWrite` completion word. The attribute clear path disables reverb writes through both control bit7 and its existing enabled state, then restores them. Ordinary `SpuWrite` and callback delivery contracts are unchanged.

### spu_clear_reverb_transfer

`spu_clear_reverb_transfer(mode, source, binding, direction)` is a synchronous scoped adapter for translated SDK state. The caller supplies a readable1024-byte source and live existing transfer fields; optional direction storage receives0. It never installs or replaces the global binding. Both clock/wait callbacks must be present together or absent together; absence selects an already-completed native upload, not a physical timing model. Completion-event delivery is mandatory for every block. An absent guest-address callback uses `xport_guest_buffer_address`; native default clears use an opaque host zero buffer. No new persistent binding, queue or snapshot layout is introduced.

### spu_clear_reverb_work_area

`spu_clear_reverb_work_area(mode, binding, direction)` selects the existing private1024-byte zero source and delegates to `spu_clear_reverb_transfer`. Use this native entry when the caller needs its own live transfer fields and per-block completion-event callback. The binding and its context must remain valid until synchronous return. Preserve the underlying mode/occupancy checks,64-byte block rounding, callback/mode save and restore, clock policy and return values. A native source has `source_address == 0`; do not fabricate a guest address or RAM-fetch evidence. This entry does not rebind global transfer state or reset its pending/failure/deadline/operation fields; it is not a metadata setter. Existing `SpuClearReverbWorkArea`, explicit-source and native byte-range APIs are unchanged. Compilation and execution checks for this additive entry remain part of the consuming project's final verification.

## Native VAB attributes

- `psx.h` declares PsyQ `VabHdr`, `ProgAtr` and `VagAtr` with fixed-width fields and compile-time boundary sizes32/16/32, using `D:/psyq/include/LIBSND.H` as the ABI source
- `SsUtGetVabHdr(bank, out)`, `SsUtGetProgAtr(bank, program, out)` and `SsUtGetVagAtr(bank, program, tone, out)` read the native bank owned by `SsVabOpenHead`; they lock the existing audio mutex, copy attributes into the caller-owned output and return0, or return-1 without writing output for an invalid/null request
- Bank indices are0..15, program indices0..127 and tone indices0..15. Tone attributes require a declared nonempty program. Unused slots of a declared program retain their original32-byte source records; sample validation still applies only to declared playback tones
- The native bank retains one complete32-byte header, sixteen-byte program records and32-byte tone records. Playback reads the same records. Header master volume is read from this owner rather than a separate mutable copy. File/system reserved bytes are retained except the PsyQ runtime program words: word8 holds the compact tone-group index; halfwords12/14 contain sample addresses in eight-byte SPU units. Getters do not publish the translated guest VM context
- No guest bank-address publication, game callback registration, per-game address lookup or fallback is added. Existing guest VM contracts remain separate until their complete consumer migration; do not switch a game to native getters while leaving mutable guest tone writers authoritative
- Structurally implemented but unverified.

Do not run extra fixtures during that project's structural phase

### spu_vab_native_view

### Native VAB record views
- `spu_vab_native_view(bank, out)` returns1 and a borrowed read-only `SpuVabNativeView` for an open native bank, or0 without changing output for an invalid bank/null output. The view exposes explicit byte lengths for header/program/compact-tone records plus SPU body address/size
- The SDK owns every returned byte. Pointers remain valid only while that bank remains open and the native SDK has not been reset/reinitialized. Callers must serialize bank lifetime and any metadata mutation; the function's internal audio lock does not pin the returned view
- Program word8 indexes compact tone groups. Native playback and `SsUtGetVagAtr` resolve logical programs through that same word. The complete declared compact tone block is retained, including unused/reserved groups; active-tone sample validation remains unchanged
- SPU body addresses are device byte offsets. Program halfwords12/14 use eight-byte device units; they are never host pointers. Do not serialize returned host pointers, cast them to PS1 addresses, cast away read-only qualification or introduce a guest compatibility copy
- These views are for native SDK consumer migration. Apocalypse must migrate all mutable context/tone/program readers and writers before switching its VAB loading away from guest storage; current translated guest SDK publication remains a separate unfinished contract
### VM bank representation selection
- `XPORT_NATIVE_VAB=1` selects native VAB records in `psx_seq.c`; absent/zero retains the existing guest VM bank table contract. This is an explicit compile-time representation choice, not a missing-bank fallback
- Native `_SsVmVSetUp` validates the open SDK bank/logical program, publishes bank/program/compact-group IDs into the existing VM scalar context and does not publish guest header/program/tone pointers. Native metadata reads borrow `SpuVabNativeView` records; invalid selected-bank lifetime fails fast
- Tone/note selection, sample-address/ADSR setup, header gain and key-on metadata use SDK-private record references. The default representation still resolves the original bound guest pointers and uses standard `r_u8/r_u16` accesses. Voice/cache/sequence state remains under its existing binding; this switch does not establish full native VM ownership
- Apocalypse's translated bank-selection entry delegates to canonical `_SsVmVSetUp`, retaining its original provenance marker. Do not enable the native macro there until every mutable VAB attribute writer and bank resource/close path has migrated to the same native owner
- Both representations are structurally edited and unverified. Final verification must cover legacy bound reads/event scope for existing consumers and native sparse/empty programs, note/tone masks, VAG addressing, ADSR/release, gain/pan, bank lifetime and callback ordering. Record exact affected-consumer scope
### Native VAB game resource ownership
- Apocalypse selects `XPORT_NATIVE_VAB=1` for Win and Web. Its VAB resource loader owns a temporary file buffer, calls `SsVabOpenHead`, transfers samples synchronously with `SsVabTransBody`/`SsVabTransCompleted`, then frees the file buffer. The SDK owns all retained records and SPU sample storage until `SsVabClose` or SDK reset
- `spu_vm_native_bank()` returns the read-only view selected by the bound VM context's bank byte. Missing context or closed bank fails fast. Consumers must serialize bank lifetime; returned pointers do not hold a lock or pin
- Under native VAB mode `spu_bind_vm_guest` requires the retained scalar context, voice/cache/sequence/protection bindings, but not legacy VAB header/program/tone pointer tables, bank flags/count/limit. The default mode retains its prior table requirements
- Implemented translated sequence callbacks change channel/voice/context state, not VAB file bytes. Unimplemented callback targets fail fast. This source migration does not implement missing SDK mutators or establish complete native SEQ/VM ownership
### Native SEQ resource spans
- `SpuSequenceStream` owns stream positions and borrows a caller-owned immutable byte span. Positions are32-bit offsets into that span, not PS1 or host addresses. The current/start/loop-start/replay/loop-end fields follow the original five cursor roles; `loop_end_set` distinguishes a missing marker from file offset0
- `spu_sequence_stream_init(sequences, tracks)` replaces the SDK-owned descriptor table, clearing all borrowed spans; `(0, 0)` releases it. Native counts require1..32 sequences and1..32767 tracks. `spu_sequence_stream(sequence, track)` rejects negative or unconfigured indices. Callers serialize descriptor lifetime against callbacks and SDK reset
- `spu_sequence_stream_bind(stream, data, size)` clears positions and binds a nonempty borrowed span. `spu_sequence_stream_clear(stream)` detaches it. `spu_sequence_stream_byte(stream, offset)` fails fast for missing data or an out-of-span read. None of these functions serialize host pointers or assume source bytes remain available after detach
- `XPORT_NATIVE_SEQUENCE=1` selects native cursor and event-byte reads in `_SsReadDeltaValue`, `_SsSetControlChange`, `_SsContNrpn2` and `_SsGetSeqData`. Absent/zero retains their guest cursor contract. Native mode now accesses typed track fields directly; the earlier internal cursor selectors have been retired. Numeric callback bindings remain a separate unfinished family
- Apocalypse enables native SEQ mode for Win/Web. Its resource owner allocates the rounded DATA read span and binds the exact file byte length, holds the bytes through playback, closes/detaches every sequence track before free, and preserves the original delta multiplication/low-word accumulation and repeat/event order in source. Header format diagnostics retain original outcomes; span violations and loading over a live owned buffer fail fast
- Native sequence/track state and manager mask/tick/count/lock fields now have SDK owners as defined below. VM voice/cache/timer and numeric callbacks remain unfinished.
### Native SEQ track and manager ownership
- `SpuSequenceTrack` combines its borrowed stream with typed playback/status/control bytes, sixteen channel pan/program/volume entries, signed next-sequence/track IDs, fixed-width volume/tempo ramps, flags and delta counters. Runtime structure padding and stride have no PS1 meaning
- `spu_sequence_track(sequence, track)` returns the configured SDK-owned track and fails fast for invalid indices. `spu_sequence_stream` returns its embedded stream. `spu_sequence_stream_init` allocates the single track table and updates native manager counts; it does not maintain a second scalar or stream mirror
- `SpuSequenceManager` owns lock/opened-mask/tick-rate/count fields in original role order. `spu_sequence_manager()` returns this native owner. Cold static state is zero; `spu_sequence_reset()` releases descriptors and sets the SDK initial tick rate60 with zero counts/masks/lock. Callers serialize reset/reconfiguration against every borrowed track pointer
- Native sequence mode selects typed records in channel mute, note/program/control/NRPN/delta/event/tick and VM key-on volume/channel consumers. Default shared mode retains the guest-bound record/table implementation. Native mode no longer needs `xport_libsnd_bind_sequence_table`; handler/mark/VM voice-cache bindings remain separate unfinished contracts
- Identity33 bypasses the sequence owner in both key-on volume/channel consumers; it represents a sound effect and must not request unconfigured sequence33. The original8008CB1C pitch callback consumes only sequence/track; native event dispatch forwards two arguments and removes the unconsumed carried table address. The default shared representation keeps its prior carried-register call contract
- The translated game controls retain original write order, signed/unsigned halfword interpretations and callback/service order. Stop/replay/chaining, per-frame volume/tempo ramps and native save-volume output pointers use the same owner. No176-byte game pointer-table calculation or game storage-base argument remains
- `control_data[8]` preserves the coherent original controller-byte group. Exact names for unimplemented selector/data roles and complete controller operations remain TODO; grouping does not establish complete SDK support. Fresh table reconfiguration uses new zeroed tracks; current game producer configures1x1 during initialization.

Native VM voices/cache/timers/callbacks and full SDK ownership consolidation remain required

### SpuSequenceCallbacks

### Native sequence callbacks
- `SpuSequenceCallbacks` is the SDK-native owner of note/program/pitch/meta/control, bank/controller/reset-all, twenty VAB-attribute hooks and32x16 mark hooks. `spu_sequence_callbacks()` returns this owner; no original addresses, numeric target registry or guest handler table is published
- Typed note callbacks take four arguments, ordinary event/controller callbacks three, pitch/bank/reset-all two. Mark callbacks use the PsyQ void(short,short,short) contract and VAB-attribute hooks use the six-argument VagAtr-by-value prototype from `LIBSND.H`. Event return words preserve the current translated call contracts; they are not a claim that vendor public void APIs returned game results
- Native note/program/pitch/meta/control dispatch loads one typed pointer and calls it with semantic arguments; a missing required callback fails fast. Controller dispatch selects the explicit MIDI controller domain6/7/10/11/64/98/99/100/101/91/121; reset-all calls two arguments. Null mark hooks are deliberately skipped as in the original; mark indices require0..31 and0..15
- `spu_sequence_reset` clears the complete callback owner after releasing track storage. `spu_sequence_clear_marks()` clears only mark hooks. Apocalypse initializes note/program/pitch/control/volume/pan/damper from its translated functions and leaves all originally null hooks null, including the twenty attribute hooks. Registration preserves original order and does not clear marks implicitly
- Native `_SsContNrpn1`, `_SsSetControlChange` and `_SsGetSeqData` use this owner; the default shared representation retains its guest handler/mark tables and numeric callback service. Apocalypse retires its numeric `xport_libsnd_sequence_call` implementation because no selected native consumer requires it
- The earlier project event path still carried an unused guest table argument in running-status pitch-bend. Typed two-argument dispatch removes it; complete original8008CB1C and PsyQ declarations establish the semantic input count. No new meta/NRPN/attribute callback bodies or fallback behavior are invented
### Native sequence timer lifecycle
- `SpuSequenceClock` owns tick mode/manual flag, typed tick/previous-VBlank callbacks, direct-VBlank/half-rate/interrupt bytes and phase. Initial native SDK values come from the original clock defaults60/manual1/interrupt127/phase0; the game supplies its actual tick function through the native callback field
- In native sequence mode `SsSetTickMode` also invokes `spu_sequence_set_tick_mode`: bit0x1000 selects manual mode and low12 bits; modes0/5 derive50 or60 from `GetVideoMode`, modes1/4 select the correct VBlank/custom rate, modes2/3 publish240/120. Signed negative modes retain the original default60 rate. No video state or renderer implementation is changed
- Native `SsStart` invokes `spu_sequence_start_timer(0)` after releasing the audio mutex. Mode0 publishes interrupt127 and returns; manual custom-rate mode returns before registration. Other paths retain original source IDs, signed division, half-rate selection, low16 counter targets, critical-section ordering and reset/configure/register order
- Tick callback types are native void(void). The chained handler calls the previous VBlank callback first, then reloads/calls the current tick callback. The half-rate handler clears phase before the alternate tick call. Missing required callbacks fail fast; no callback address decoding or guest timer-event trampoline is used
- Native `InterruptCallback` and `VSyncCallback` own actual registrations. Timer binding tokens retain previous and installed native pointers for reset/release, including a deliberate cleared VBlank slot. VSyncCallback's intptr return is a host token and is never narrowed to32 bits or treated as a PS1 pointer
- `spu_sequence_stop_timer` removes/restores only still-owned registrations under a critical section and preserves later external replacements. SDK sequence reset stops owned hooks before releasing track/callback storage; `SsQuit` performs sequence reset outside the audio mutex. Device counters retain their last programmed values; hook cleanup does not invent saved counter periods or undo later device users
- Apocalypse's `InitializeMusicSequenceTimer` binds the translated tick body, and `StartMusicSequencer` retains original8008EC4C provenance while calling canonical `SsStart`. The native scene VBlank callback preserves MDEC pumping before `AdvanceSceneVBlank` and uses the same VSyncCallback slot, preventing separate native/guest registrations
- The project retires numerical music/interrupt/event wrappers and the game BIOS guest-callback dispatcher after migrating its selected non-null producers. Remaining project OpenEventGuest operations use zero callbacks for event signaling. Generic callback boot/exception/IRQ bookkeeping and other SDK state are separate unfinished ownership families
- The old partial port updated only libsnd.tick_mode and left translated FD05C/FD060 clock state at image defaults; native mode now connects the complete original SetTickMode operation. This is a source correction from original MIPS, not evidence of tested timing or playback
### Native callback bootstrap
- `XPORT_NATIVE_CALLBACK_BOOTSTRAP=1` selects native LIBETC bootstrap in canonical `ResetCallback`; absent/zero retains its prior shared behavior. Apocalypse selects the option for Win and Web. The existing SDK process-lifetime initialization latch owns the once decision; no game DRAM latch or exception-table mirror remains
- First native bootstrap clears callback arrays, enables BIOS callback delivery, masks/acknowledges IRQs, preserves original DMA priority0x33333333, root-counter1 mode0x107, PAD/counter clear policy, DMA interrupt control0 and final IRQ mask9, then exits the critical section. Device register addresses are explicit device boundaries, not game pointers
- The native IRQ-mask/status API replaces dereferenced game pointer roots. DMA priority/counter mode/interrupt control retain existing scalar device writes. VBlank and DMA callbacks use their existing native platform services; no MIPS exception stack, setjmp context, software dispatch vectors or fake host copy of the old1050-word work area is introduced
- Native reset is process-scoped and idempotent. The project replaces graphics/audio/resource bootstrap aliases with `ResetCallback` calls, removes the unused numerical return/address relays, and preserves original device setup order. Existing CD DMA callback state and other SDK/resource owners remain separate; this does not retire the game's initial EXE loader
- Apocalypse's game-constructor once latch is a different owner: native cold startup resets it, and RunGameSession initializes gravity, motion constants and camera defaults in the prior order. The old helper return was unused; the native interface is void. No original MIPS address is invented for this pre-existing host initialization adapter

## Native libsnd voice manager

- Define `XPORT_NATIVE_VM=1` only together with `XPORT_NATIVE_VAB=1` and `XPORT_NATIVE_SEQUENCE=1`. Game voice setup, sequencer allocation/key events, `SsQueueRegisters`, `SsVoiceCheck`, `SsUtSetDetVVol` and direct voice API limits must use the same `SpuVmState` returned by `spu_vm_state()`
- `SpuVoiceState` and `SpuNoteContext` are native semantic records; their C layout is not the original 54-byte voice or packed context encoding. MIDI channel and note are separate fields. Use `spu_vm_voice()` for checked voice selection, never byte offsets or guest bindings
- The VM owns 24 voice records, selected note, key-on/off/reverb/noise halves, 16 silence-history masks, count/cleanup policy, protection, mono/release settings, 192 deferred register halfwords and 24 dirty bytes. Register words retain the hardware word order; they are not host pointers
- `spu_vm_reset()` resets this owner at the consuming game's cold-start/BSS reset boundary. `SsInit`/`SsQuit` do not additionally zero these records: the translated sequencer initializer retains its original partial field resets. Borrowed pointers expire semantically at explicit reset; callers must not retain selected-record values across callbacks that can select or reset voices. Existing guest binding APIs and default implementations remain available outside this explicit mode
- SPU hardware/transfer/malloc/reverb state and native VM state are distinct owners.
- Native VM structural migration is uncompiled and unverified; allocation ties, partial initialization, event masks, volume order, playback, reset/reentry and affected default clients require final verification

## Native LIBSPU and LIBSND initialization

- `XPORT_NATIVE_VM=1` selects the connected native `SsInit`/`SsFlush` path. `SsInit` preserves the original callback bootstrap, cold SPU initialization, mode7 work-area clear, device register defaults, partial voice initialization, mark clearing and manager reset order. The default `SsInit` retains its previous once initialization behavior
- `SpuInit` in this mode owns the original LIBSPU cold register writes and the16-byte initial ADPCM block. Addresses `0x1F801C00` and `0x1F8010F0` are device boundaries. Transfer mode, completion, address units and block bookkeeping use the existing native SPU transfer owner; do not rebind these fields to game DRAM
- Native transfer completion is synchronous. The removed game DMA callback/event pair had no nonzero callback producer and no remaining event consumer after using the canonical clear/transfer operations. Do not add a BIOS event loop or numeric callback mirror to reproduce internal DMA plumbing
- `SpuVmState.heap_records` owns the32-record LIBSND heap workspace plus its sentinel slot. Initialize it through `SpuInitMalloc`; SPU byte addresses and allocation flags remain device values. Native heap entries do not alias deferred register queues or other original BSS workspaces
- This native VM profile uses immediate SPU mask updates: the selected game has no producer enabling the original register queue. `SpuSetNoiseVoice` and the native `SpuSetReverbVoice` accept clear0, set1 and replacement8, with24-bit public masks and16-bit low/high device writes. Other shared clients retain the previous `SpuSetReverbVoice` implementation
- `SsSetSerialAttr` and `SsSetSerialVol` build native `SpuCommonAttr` inputs in all configurations. `SsSetSerialVol` writes the existing SPU CD/external volume registers with the PsyQ upper clamp and factor 258; `SsGetSerialVol` reads those same registers. CD PCM uses the CD routing matrix and one SPU volume stage; do not add a second software serial-volume multiplier. Rebuild consumers and invalidate earlier raw SPU snapshots after this private state-layout change
- Native `SsAllocateVoices`, `SsSetVoiceSettings`, `SsSetReservedVoice`, `SsBlockVoiceAllocation` and `SsUnBlockVoiceAllocation` share the VM/sequence owners. Use the PsyQ `SndVoiceStats` fields; its pan input is narrowed at the original byte write. These new voice APIs and `SsFlush` fail fast outside the native VM profile until a client supplies the corresponding representation
- Native `SsUtGetVagAtr` retains the original selected-bank/program context update and partial destination writes; it leaves unassigned bytes untouched. `SsGetActualProgFromProg` reads the compact program group. Native `SsUtGetVagAddr` selects program0 before reading SPU address units from the program tails. Retain endian codecs at VAB metadata boundaries
- `SsUtSetReverbType` and `SsUtSetReverbDepth` use local parameter workspaces. Mutable depth/mode/work-area/feedback/delay state belongs to the existing SPU reverb owner; no game copies or EXE-table bindings are required. Shared reverb presets and register/pitch tables remain canonical SDK data
- These source migrations are uncompiled and unverified. Noise key-on and automatic voice volume/pitch modulation remain explicit unsupported operations

## Voice attribute SDK boundary

SpuSetVoiceAttr accepts const SpuVoiceAttr*. It never modifies caller attributes. The shared runtime owns per-voice sample notes and registers. Apply sample-note changes before NOTE and use the genuine _spu_note2pitch four-argument converter; do not substitute the former private converter with different negative-remainder semantics. Start and loop addresses use _spu_FsetRXXa(-1,address), preserving ceil-to-address-unit, uint32 wrapping and uint16 register narrowing rather than floor shifts. Keep mask0/all, selective volume modes, raw-ADSR-before-components, rate clamps, locking and device-clock synchronization. Structural change only; final shared-consumer and Debug validation remains pending.

## Distinct sequence track owners

SpuSequenceSchedulerTrack is the scheduler-bound native track used through SpuSequenceScheduler.tracks. SpuSequenceTrack is the psx_seq.c native-manager track returned by spu_sequence_track. Both embed the same SpuSequenceStream representation for data, size and cursor ownership; their remaining fields and operations retain distinct SDK contracts. The reviewed original P2 record stride is172 bytes and the Apocalypse stride is176 bytes; these source strides do not define the native C structure sizes. P2 repeat counters retain16-bit storage and its parameter callback takes VagAtr by value; Apocalypse retains its byte repeat counters and six-argument void callback. Do not redirect either version EOF, NRPN, DataEntry or delta reader merely because stream storage is shared. These are distinct live schemas, not aliases. Never merge their typedef/tag names or cast one schema to the other. Migrate the owning operations, lifecycle and callbacks before retiring either. Internal callback-driver critical sections use xport_bios_enter_critical/xport_bios_exit_critical to avoid stale or platform-conflicting public names. P2 evidence: status/native-refactoring/shared-sequence-type-ownership.json. Final validation remains pending.

## Bounded native sticky VAB owner

spu_vab_open_head_sticky(header,header_bytes,bank,body_address) and spu_vab_trans_body_native(body,body_bytes,bank) use the existing libsnd.banks owner. The header owns decoded32-byte metadata,64/128 program records,declared tone groups and bounded sample-size table. Preserve exact SPU base,version4/8-byte lengths,compact group word8,partial sample address words and no automatic cursor advance. Decode completely before valid publication;reject source/SPU overflow. The native body uses mode0,address normalization and SpuWrite clamp/rounding;successful native copy retains no borrowed host pointer. Bank transferred/VAB completion and the separate SDK transfer completion latch retain their distinct contracts. Pointer-only legacy open/direct-upload body paths remain separate.

spu_vab_select_native selects the same owner through native_vm.note.bank/program/program_tone_index with low-byte group semantics. Native partial GetProg writes0..4 andhalf6;native GetVag/SetVag use bytes0..5,7,6,8..13 thenhalf16/18/20/22,leaving other fields unchanged. Bounds precede access. Invalid native pending/negative/out-of-span cases return-1 as an explicit invalid-access repair;do not claim original malformed-input equivalence. Keep guest profile branches and public view layout.

spu_vab_reset_native clears only VAB metadata,cursor and completion at the SDK owner reset point;never invoke it lazily while loading. spu_vab_native_current borrows the view selected by native_vm.note without another current-bank cache. The private validity byte is0empty,1legacy-auto,2bounded-sticky;prior readers use logical validity. spu_vab_close_sticky accepts only producer2:invalid bank returns0,empty bank returnsbank*4,open bank frees the SDK heap address,clears its entry and returns remaining valid-bank count. It must not reclaim the automatic cursor or disturb another pending transfer. Impossible mixed producer/pending ownership returnsFFFFFFFF without mutation. Legacy SsVabClose retains its original auto-cursor reclaim.

These structural changes are not runtime validation.

## Native by-value NRPN VAB attributes

`spu_vab_nrpn_attr_by_value(attribute, bank, program, tone, VagAtr attributes, value)` is an additive host SDK boundary for the reviewed by-value32-byte tone-attribute contract. Attributes0..14 reproduce the reviewed scalar/ADSR edits through the existing native VAB getter/setter and the shared ADSR codecs. Other attribute indices return0xFFFFFFFF before accessing a bank. This interface does not identify a unique SDK build: matching original objects occur in multiple SDK releases.

The getter result intentionally does not skip subsequent edits or the setter. Attribute1 calls SpuSetReverb(0) for value0 and SpuSetReverb(1) for value4 after the setter regardless of its result; other values return4. Other attributes return the sign-extended setter result as uint32. The P2 callback profile currently ignores these results; the host interface preserves its existing raw-result bridge contract. It does not change an AP six-argument void callback ABI or install default sequence callbacks.

Attributes11..14 use deterministic Resolve/Build initialization in place of the reviewed original leftover-stack ADSR inputs. This is an explicit native repair policy, not proof of original stack equivalence or a change to AP routing. The original shared Build behavior for field7 remains unchanged. Input snapshots use a by-value VagAtr; the existing libsnd native bank remains the only mutable owner. No additional track, bank, reset, transfer or optional psx_seq.c dependency is introduced.

## Shared SEQ stream codec and P2 by-value callback

The existing spu_sequence_stream_bind/clear/byte implementation lives in psx_spu.c, so SPU profiles may use the bounded stream representation without linking optional psx_seq.c or supplying its dispatcher. Signatures, guards, memset behavior, debug diagnostic prefix and abort conditions are preserved. The canonical SEQ manager and tracks remain in psx_seq.c.

### spu_sequence_stream_delta

`spu_sequence_stream_delta(stream, elapsed)` implements the reviewed P2 original80073D58 operation. The caller supplies valid stream/elapsed objects. Advance the cursor before each bounded byte read; return0 for the first zero byte without writing elapsed; combine7-bit groups with uint32 wrap and no invented four-byte limit; multiply by10 and add the wrapped result to elapsed. This function does not change the stream start, loop, replay or end fields. AP _SsReadDeltaValue is not redirected: its current read-before-cursor-store failure order remains distinct pending original-domain review.

The distinct P2 SpuToneParameterHandler now takes VagAtr by value as its fourth argument. P2 table callers and all15 original marker bridges use that same five-argument contract, removing the temporary byte-pointer snapshots. The canonical AP six-argument void SpuSequenceVabCallback and routing are unchanged. Current P2 dispatch ignores handler return values; raw SDK results remain preserved by the bridges. The existing native ADSR repair policy for attributes11..14 remains documented separately.

Changes are structurally applied after cross-project source review, with final Debug x86 integration and audio scenarios pending; no intermediate execution proof is claimed.

## Distinct scheduler Data Entry profile

`spu_sequence_scheduler_data_entry(record, sequence, separation, value, commands, command_count, read_delta)` implements the reviewed native scheduler-profile operation from P2 original8007201C. It operates on the explicit SpuSequenceSchedulerTrack supplied by the caller and does not obtain or modify the canonical AP manager/track/default callbacks. It uses the existing native VAB getter/setter and the supplied five-argument by-value parameter callbacks or value callbacks; handler results remain ignored.

Preconditions: record is live and channel is0..15; commands covers command_count entries; a selected bounded entry provides parameter or value; read_delta is valid and resolves the supplied sequence/separation according to the caller profile. P2 producers initialize channel to0 or event&15, populate all20 command slots in SEQ open before playback activation, and pass its original delta bridge directly. These are internal valid-caller contracts, not additional game success fallbacks. Out-of-range commands retain the existing native diagnostic/abort constraint.

Keep the initial program query before phase tests, pending loop-count precedence, bank/channel-program/kind reloads per original iteration, callback reentry and delta-store-before-phase-clear order. The required delta callback preserves the original sequence/track lookup across reentry; do not replace it by a cached-stream read merely for convenience.

The explicit P2 native profile initializes local ProgAtr/VagAtr to zero before partial getters and first by-value copy. This defines otherwise uninitialized stack input and rejected-program tone count; it is a separately authorized native repair, not original stack equivalence or a general AP/PsyQ contract. No extra getter or early error return is introduced. Canonical AP DataEntry and its six-argument void callbacks remain unchanged.

## Common SEQ reference and coordinated migration

The human-selected reference is Pandemonium2 SEQ, including its measured physical-clock and SPU update fixes. Migration direction is Pandemonium2 -> shared xport runtime -> Apocalypse. This target supersedes earlier agreements to retain two independent engines indefinitely. Descriptions above of separate current P2/AP schemas and callbacks document transitional interfaces; they do not establish AP as the reference algorithm. Complete common-engine extraction and AP adoption remain pending.

Keep nominal SDK tick mode60 separate from the measured NTSC physical field rate53693175/(3413*263). Preserve rational time/sample remainder and VBlank PCM production; output-buffer completion must not quantize SEQ/SPU state changes. Do not introduce a render-frame completion gate.

P2 owns reference analysis and coordinated shared-core migration; the Apocalypse task is asked to own AP caller adaptation. Obtain its dependency/ABI contract before changing shared interfaces. Retain required SDK version differences as explicit narrow compatibility contracts backed by original code. Keep one common engine and mutable owner, typed native tracks and bounded streams; never cast original172/176-byte schemas into each other. Preserve per-image function provenance and markers when replacing project bodies by common implementations. Keep optional SEQ linkage explicit for SPU-only consumers.

Do not claim adoption from shared-file visibility alone. Final Debug x86 integration/audio verification is deferred under the current task policy; do not run intermediate builds or web checks.

## Common scheduler operations from the P2 reference

`spu_sequence_scheduler_set_table`, `spu_sequence_scheduler_play_tick`, `spu_sequence_scheduler_crescendo`, `spu_sequence_scheduler_decrescendo`, `spu_sequence_scheduler_tempo_ramp` and `spu_sequence_scheduler_tick_mode` in psx_spu.c contain the extracted P2 reference algorithms. They take an explicit live SpuSequenceScheduler; tick-mode selection also takes the live SpuSequenceClock and the already obtained video mode. No second scheduler or clock is allocated. Keep set-table partial initialization and original caller lifetime; this is not a blanket track reset or an allocator.

Volume callbacks use SpuSequenceVolumeGet/Set with packed sint16 handles and original uint32 results. Play-tick uses SpuSequenceHandler for event dispatch. Preserve captured record pointers, subsequent live slot reloads and callback order. Callers must supply the same live native context and valid callbacks used by their sequence engine. Do not turn the remaining AP native track into a scheduler track with a cast. Complete engine/owner unification remains a coordinated migration.

P2 keeps original marker bridges for800759D4,800750F0,80074A98,80074D74,80075DF4,80075BB4. Shared bodies have no game-specific marker. This is an applied structural extraction, not a claim of current execution equivalence; final Debug x86 verification remains pending.

## Common reference SEQ events and transport

The common spu_sequence_scheduler_* event/controller and transport functions in psx_spu.c implement the extracted P2 reference operations. Callers supply the live scheduler and typed handlers, plus exact external SPU/VM callbacks where required. They do not allocate a second mutable engine, depend on a P2 game symbol, or reinterpret an AP track as a scheduler track. Complete AP adoption remains pending.

Preserve P2 NRPN pending loop-count precedence, signed16 repeat fields, running-status return carriers, cursor-before-bounded-read ordering and delta-before-note dispatch. The full common end_of_track operation implements repeats and chaining, including the original repeated key-off. It is not the old three-argument AP _SsSeqGetEof abort stub; redirect that entry only with its caller/state contract migrated.

SpuSequencePumpOps preserves distinct uint32 and void callback signatures for flush, play, ramps, pause, resume and stop. Supply a live table throughout callbacks; do not cast heterogeneous callbacks. The pump keeps processing==1 reentry behavior, live track/count/flag reloads and both independent tempo-ramp branches. Stop/close retain partial initialization and original stream release order; no blanket reset or extra owner is introduced.

These26 operations follow the earlier6 scheduler operations. Current structural migration has no new execution proof; no intermediate builds, tests or web runs are authorized by these receipts.

## Common reference SEQ open, clock and mutable owners

`spu_sequence_scheduler_initialize_track`, `open` and `initialize` now contain the P2 header parser, copied native stream ownership, callback binding and reset operations. Open receives live handlers/commands and immutable default bindings, copying only the original assigned fields after initialization. Inactive parameter/value callback fields remain untouched. Preserve original occupied-slot/error behavior, partial writes, integer tempo rounding, delta ordering and stream release order. SpuSequenceInitialize keeps the original three-argument parser callback. SpuSequenceVmInitialize takes uint8 voices and returns uint32, matching the P2 VM entry.

The common irq0, half_rate, start_timer and stop_timer scheduler operations take an explicit live SpuSequenceClock. Keep native IRQ0 previous-callback capture distinct from VSyncCallback; reload tick after previous-callback execution. Preserve manual/direct-vblank/IRQ6 decisions, half-rate phase and critical sections. Nominal mode60 is not the physical VBlank frequency.

The single clock and SpuSequenceCallbacks storage, existing getters, mark clearing and marker dispatch now live in psx_spu.c. Optional psx_seq.c uses those same owners through the existing APIs. This removes the P2 clock/marks linkage dependency on the older AP engine without adding any duplicate state or changing AP callback signatures.

### spu_sequence_scheduler

`spu_sequence_scheduler`, `spu_sequence_handlers` and `spu_tone_commands[20]` are common extern state defined once in psx_spu.c. P2 project definitions/declarations were removed; existing direct field consumers retain the same objects and EXE-derived initialization. Track storage remains caller-owned. AP legacy manager/track state is a transitional consumer boundary until coordinated adoption of the common P2 engine; it is not a second final reference.

Full AP adoption, VM consolidation and final Debug x86 validation remain pending; do not claim current runtime equivalence from earlier audio proof.

## Common reference voice backend and lifecycle

The extracted `spu_sequence_vm_*` operations implement P2 volume, voice/tone selection, pitch/bend, noise, key-on/off, initialization, flush and reverb controls in common psx_spu.c. Use the existing single SpuVmState/VAB owners. Operations that read tracks take an explicit live SpuSequenceScheduler. Do not introduce a project VM table or redirect to older AP algorithms merely because the PsyQ name matches. Public P2 original functions retain typed marker bridges; common internal calls use common backends.

Preserve P2 priority/envelope/age selection order, unsigned/signed narrowing,192-entry pitch lookup, fine quantization, masked shifts and quadratic volume. ADSR2 adds release_offset to the entire halfword. Noise-off keeps sample, clears NOISEL/H and returns52*voice. Noise voice masks retain queued mode and only mode0/1 mutation. Sequence33 key-on still applies quadratic volume. Full noise-on is implemented; the old AP static abort stub is not the reference.

### spu_reverb_attributes

Flush retains the15-history-word reduction,24-voice modulation callback traversal, original device write order and clearing key event masks before device writes. Initialization accepts uint8 voice count, preserves partial modulation resets, VAB reset and default register writes. No new AP protection mask, release clamp, noise-event flush or generalized reset is added to these P2 algorithms. The common `spu_reverb_attributes` workspace is defined once; partial reverb field writes and original EXE-derived startup/reset remain unchanged.

AP adoption and retirement of its older competing algorithms remain pending. Current structural changes have not been compiled or executed; final Debug x86 verification remains deferred.

## Common P2 SPU device and callback-driver lifecycle

`spu_sequence_vm_initialize_device(mode, dma_callback)`, reset_device, dma, register_dma and quit contain the extracted P2 low-level SPU lifecycle. Preserve the once-only host device/MMIO guard, distinct cold/warm writes, original sample defaults, DMA event allocation/teardown order, polling bounds, critical sections and reverb/heap reset. The typed DMA callback supplied by P2 remains its original marker thunk; common code reloads the live completion callback before delivery. Do not replace warm initialization with the older _SpuInit abort path.

### psx_callback_reset_driver

`psx_callback_reset_driver` in psx.c retains the original P2 native driver flag gate and exact IRQ/MMIO/VSync/DMA initialization order. Existing generic ResetCallback APIs and AP callers are unchanged. All existing common state owners remain single-instance; no game shadow table is added.

AP adoption/old implementation retirement and final Debug x86 validation remain pending; no current execution proof is claimed.

## Extracted XA music files

`xport_xa_play_file_at(path, repeat, sector)` starts at the first complete XA packet whose original BCD header identifies an LBA greater than or equal to `sector`. Zero starts at the beginning; negative values fail. Skipping uses the existing incremental transport, resets decoder history at the requested start, and counts skipped packets in the absolute file mixer frame position. A target beyond the final packet reports EOF through the existing owner and position API. Repeat starts subsequent passes at the beginning. Stop releases the owner; reopening at a saved sector works after another stream occupied it.

### xport_xa_play_file

`xport_xa_play_file(const char *path, sint32 repeat)` in `psx_spu.h` starts a single-channel raw XA file through the existing CD audio owner, SPU mixer and Web MUSIC transport. Use complete 2352-byte sectors, one file/channel/coding tuple, and a `MUSIC/NAME.XA` path. Zero repeat plays once; positive repeat resets XA decoder history and loops at decoded EOF; negative repeat fails. Web waits only for the existing initial transport prefix, then decodes complete available sectors; an underrun leaves decoder position/history unchanged and resumes on arrival. Existing CdControl mute/demute, pause/play and stop control the same owner. CdPlay retains numeric IMA WAV behavior. Validate with `python -B -m unittest tests.test_xa_file_stream tests.test_web_packager -v` from tools and `node tools/tests/web_assets_test.cjs`.

### XPORT_AUDIO_VBLANK_QUEUE

Set `XPORT_AUDIO_VBLANK_QUEUE=1` for native Windows to render PCM on the game thread before each VBlank IRQ and queue it for waveOut. The output worker consumes complete PCM blocks and never advances SPU voices. Device buffers affect output latency, not note quantization or envelope reads. The physical-output FIFO holds `XPORT_AUDIO_BUFFER_COUNT * XPORT_AUDIO_BUFFER_FRAMES` stereo frames. Keep read/write updates under the audio lock. When a new field exceeds FIFO capacity, advance the read cursor by the exact overflow and count those discarded oldest output frames in `g_xport_audio_dropped_frames`;append the complete new field without waiting or sleeping. This discard affects only already rendered physical output;do not skip SPU rendering,SEQ events,capture callbacks or VBlank delivery. Reset the counter at audio initialization. Oversized submissions and invalid queue occupancy remain fatal. Use64-bit absolute cursors across the32-bit boundary. Never restore synchronous sound-device backpressure on the VBlank/game thread: a debugger stop or blocked host creates elapsed fields whose realtime drain creates another equally large elapsed interval. During native VBlank catch-up,poll host messages at least once per32remaining fields while retaining every field and its fractional clock remainder. Defaults retain the legacy worker-rendered path;consumers using the queued path require a rebuild. Verify `python -B -m unittest tests.test_audio_field_clock tests.test_native_timing tests.test_render_wait tests.test_pad_ports -v` from tools. Tests cover exact ordinary FIFO order,overflow/latest-sample retention,120seconds without a consumer,64-bit cursors and complete elapsed-field delivery with cooperative polling.

### XPORT_SPU_VOICE_CAPTURE

Set `XPORT_SPU_VOICE_CAPTURE` to a writable filename before native audio starts to capture raw 44100 Hz signed little-endian 16-bit PCM. Each frame contains 24 stereo voices in voice-index order, after ADSR/voice volume and before master volume/reverb. An unset variable leaves capture disabled. Opening or writing failure aborts; normal exit flushes the final block. Forced termination can omit the last buffered 256 frames. `spu_voice_capture_sample_count` counts captured frames for debugger start markers. No game bindings or ABI changes are required.

### gpu_observer_test

SPU MMIO accepts24-voice key-on/off low/high writes, reverb depth/mask/base/parameters and manual FIFO writes with transfer control4. These writes update the same mixer/reverb state as native SDK calls. Manual transfer batches are synchronous, limited to32 halfwords; FIFO reads and other transfer controls remain unsupported. Existing ABI and consumer registrations are unchanged. Verify with `gpu_observer_test --fixture spu_control` and `--fixture spu_pending_key`.

### _96_remove

`_96_remove()` preserves BIOS A0:72: enter the critical section, close the five CD event handles in DRAM, then dequeue both BIOS CD interrupt nodes at priority zero. It returns the second dequeue result and does not exit the critical section. This additive entry introduces no required bindings or modules.

### SsChannelMute

`SsChannelMute(sequence, separation, mask)` writes the low16 mask at offset128 of the existing bound LIBSND sequence record (176 bytes per separation). It sign-extends the two indices, preserves wrapping address arithmetic and returns the wrapped separation offset. It requires the existing sequence-table binding; it adds no binding or snapshot state. `_SsNoteOn` already consumes this mask.

### SsVoKeyOff

`SsVoKeyOff(voice_program, note_volume)` extracts signed16 bank from bits8..23, low8 program and low16 note from bits8..23, then uses `_SsVmKeyOff` with identity33. The low8 second carrier is unused by the original downstream entry. This additive API reuses the existing libsnd VM binding and adds no consumer binding or snapshot state.

### xport 3

- `xport.h`: base public header for fixed-width types, PsyQ PAD constants, data `GDB_DATA` exports, platform services, required game bindings and the mandatory project-defined `WND_TITLE`, `WND_WIDTH`, `WND_HEIGHT` and `FIELD_RATE` contract
- `psx.c` / `psx.h`: platform-neutral PsyQ-compatible scalar types, `DRAM`/`SCRATCHPAD` address mapping through `psx_addr`, GPU packet ABI, matrix/GTE helpers, pad services and PS-X executable/CD loading
- `psx_gpu.c` / `psx_gpu.h`: the 16-bit `VRAM` source of truth, direct PsyQ image operations, immediate `DrawPrim` submission, ordering-table traversal, packet rasterization and display-rectangle conversion to the 32-bit platform presentation buffer; `LoadImagePSX` and `StoreImage` consume native C pointers

### psx_font

- `psx_font.c`: native PsyQ `FntLoad`, `FntOpen`, `SetDumpFnt`, `FntPrint` and `FntFlush`; compile this module for consumers of these entries. The SDK atlas and eight font slots are private; packet storage uses registered native links. `FntPrint` accepts an implicit dump-font format or an existing font ID cast to `const char *` followed by the format; supported conversions are `d`, `x`, `X`, `c`, `s` and `%%`, with decimal widths. Hex digits are uppercase; leading-zero padding applies to hex. The shared character budget is 1024. Native overflow preserves the original failure count in isolated storage; invalid formats, null strings and negative capacities fail without reproducing original memory corruption. Font state is not serialized by the GPU snapshot.
- `psx_spu.c` / `psx_spu.h`: deterministic SPU RAM, PsyQ libsnd/VAB voices, PSX ADPCM, ADSR, IMA ADPCM Red Book transport, raw-disc XA/STR transport and final PCM mixing
- `psx_xa.c` / `psx_xa.h`: independent XA ADPCM sector decoding and resampling; transport, format scope and native arithmetic limitations: [NATIVE_XA.md](NATIVE_XA.md)
- `psx_mdec.c` / `psx_mdec.h`: incremental MDEC RLE, integer IDCT, color conversion, command/FIFO state and delayed output in four depths. The canonical PsyQ scale matrix selects a fixed-point AAN path with exact sparse handling and matrix fallback for ambiguous rounding components; any other uploaded matrix uses the generic matrix path. `MdecState` owns tables, partial coefficients, queues and remaining copy ticks; `mdec_state_io` saves portable little-endian state and rejects invalid loads without changing the decoder. IDCT takes the original hardware scale-matrix order. CPU bus stalls and hardware DMA addressing/interrupt delivery remain pending; native PsyQ entry bindings and STR playback are implemented in `psx_press.c`, `psx_vlc.c` and `psx_stream.c` as documented in [PSYQ_VIDEO.md](PSYQ_VIDEO.md). Numerical and state fixtures do not establish hardware equivalence
- `platform/win/main.c`: Windows process entry, timer, exhaustive message/input polling, window, WaveOut audio, audio synchronization and file services

### psx

`psx.c` loads a `PSX_EXE` description into shared guest RAM and provides common CD path, size and read helpers. Games own only executable addresses, paths and the call into their translated entry.

### gpu_

PsyQ entries are implemented directly in their owning shared module. Internal subsystem functions, types and constants use the `gpu_*`, `spu_*` and `gte_*` namespaces. Platform presentation and quit state use `xport_present` and `xport_isquit`; do not add `psx_*` forwarding aliases, parallel `PsyQ*` compatibility state or game-owned GPU trampolines.

### psx_seq

The optional `psx_seq.c` module owns `_SsSeqPlay`, `_SsGetSeqData`, `_SsReadDeltaValue` , `_SsSetControlChange` , `_SsContNrpn2` , `_SsContNrpn1` , `_SsSetProgramChange` and `_SsNoteOn`. They implement the PsyQ LIBSND duration/countdown loop, explicit and running-status event decode, and variable-length delta values using numeric guest records. Bind the guest pointer table with `xport_libsnd_bind_sequence_table` and the 17-word event/control handler table with `xport_libsnd_bind_sequence_handlers`. Both bindings belong to `LIBSND_STATE`, are included in the existing size-bound snapshot, validated on restore and cleared on subsystem reset. Register `psx_seq.c` in each consuming native project and implement the required `xport_libsnd_sequence_call(target, argc, arguments)` service to dispatch original numeric handler targets synchronously. Only consumers of this optional module acquire that link dependency; existing SPU consumers and voice APIs are unchanged. `_SsSeqGetEof` remains an explicit fail-fast SDK boundary. This implementation does not establish audible sequence playback or original equivalence.

### xport 4

Extend the required game-binding section of `xport.h` only for public cross-platform contracts. The platform audio worker calls shared `spu_render`; games use PsyQ `Ss*`, `Spu*` and `Cd*` calls and own no render callback, mixer, VAB registry or Red Book decoder.

### CdlATV

The host Red Book path interprets each `CdlATV` byte as a normalized 8-bit weight: `0xFF` is direct unity and two `0x7F` contributions form a unity mono sum. Apply the CD matrix before SPU CD volume, voice summation, final saturation and master volume. Saturate every decoded PSX and IMA ADPCM sample before storing it in predictor history.

### CdSetToc

`CdSetToc` installs the native disc catalogue. `CdGetToc` and `CdGetToc2` return its track count and copy the lead-out entry plus every track entry under the audio lock. A null output or an unset catalogue returns zero. The native model does not issue physical CD commands; the reviewed `MAIN.EXE` implementation ignores the `CdGetToc2` mode argument.

### DMACallback

`DMACallback` keeps an independent native callback slot for each of the seven DMA channels and returns the previous binding. Both it and `CdDataCallback` accept and return `PsxInterruptCallback`, a native `void (*)(void)`; they do not use the CD status/result callback type `CdlCB`. Invalid channels return null. `CdDataCallback` uses channel 3. This registry implements registration and removal, not hardware DMA interrupt delivery.

### spu_dispatch_pending_irq

Native SPU MMIO accepts the eight halfword registers of each voice, IRQ-address register `0x1F801DA4` and control IRQ-enable bit `0x40`. Existing unsupported volume-sweep and control-routing writes still fail. RAM reads/writes through ADPCM decoding, transfers and reverb latch one pending IRQ when they touch the enabled eight-byte IRQ address; clearing IRQ enable cancels the latch. Late address/enable changes inspect the current transfer address and decoded voice blocks. IRQ enable keeps silent voices advancing their ADPCM readers. Call `spu_dispatch_pending_irq` only on the game thread: it removes a pending notification under the audio lock, releases the lock and raises native IRQ9. Windows and Web deliver through `xport_audio_vblank`; the Windows mixer worker never enters game code. Existing callback APIs and project registrations are unchanged. Added latch fields change the private size-tagged SPU state; older state sizes are rejected. Capture-buffer IRQs, physical transfer timing, exact delivery timing and RRJ audible/playable behavior remain unverified.

### spu_render

In native builds, Red Book EOF detection in `spu_render` records one pending `CdlDataEnd` notification under the internal audio lock. The logical game-thread interrupt service dispatches it at `VSync`, native event polling, or critical-section exit, with a null result and without holding the audio lock. Disabled PsyQ interrupts defer delivery; the dispatch guard prevents recursive callback entry. Stopping or starting playback, resuming it, or replacing/removing the ready callback cancels an obsolete notification. Re-registering the same callback preserves it. Games must not lock host audio around their own state. This native scheduling contract does not establish physical CD interrupt timing.

### CD_cw

`CD_cw` dispatches through the immediate native CD command model and converts successful dispatch to zero, as required by `CdControlB`. `CD_sync` returns that model's completion status. Hardware command timing and interrupt delivery are not modeled by these native entries.

### SpuInit

Ordinary SPU initialization creates internal native transfer state when no explicit transfer binding exists. `SpuInit` and `SsInit` share that initialization path. Explicit custom or guest bindings retain their existing selection. Native state uses eight-byte addresses, 64-byte DMA blocks, initial address units `0x200`, and a completed initial transfer. `SpuWrite` accepts native source pointers under this internal owner; it reads only `byte_count` bytes, zero-fills the alignment tail, and supports SPU RAM wrapping. Guest sources keep their original rounded-block reads. Internal native transfer completes synchronously; physical DMA timing and interrupt callbacks are not established. Games need no transfer clock, wait callback or state binding. Private size-tagged SPU state includes native transfer fields; bound transfers remain outside the portable guest-only snapshot contract. The common `SpuSetTransferStartAddr` return is an aligned byte address; the reviewed RR original returns eight-byte units. Its caller ignores the result; callers requiring the original return must account for that documented difference.

### XPORT_SPU_WRITE_MAX_BYTES

The `XPORT_SPU_WRITE_MAX_BYTES` build setting selects a reviewed PsyQ `SpuWrite` variant: default `0x7f000u` for early SDKs, or `0x7eff0u` for later SDKs. Other values fail compilation. Select the variant in consuming-project native/Web build definitions from complete original MIPS evidence; this changes only the public count cap before the existing transfer path. The late-SDK `spu_control` fixture selects `0x7eff0u`; preserve that expectation independently of runtime output. Native/guest pointer domains, padding, transfer address, completion, events and callbacks retain their existing contracts.

### SpuInitMalloc

`SpuInitMalloc(count, records)` uses the caller's aligned native buffer for `(count + 1) * 8` bytes of allocator records. The extra record holds the terminal free span. A nonpositive count, null buffer or unaligned buffer returns zero without replacing the allocator. Positive initialization writes the first record, owns the last index inside SPU state, binds a limit of `count`, and returns `count`. The caller must keep the buffer alive until the next initialization. Native addresses in these records are SPU RAM byte offsets, not game DRAM addresses. `SpuInit` clears the allocator binding and reverb reservation and restores the reviewed SDK default work address `0xF204` in eight-byte units. Use `SpuReserveReverbWorkArea` to reserve it and let mode changes update the allocator reservation. Legacy explicit guest heap bindings remain available for unmigrated consumers. Native record buffers are outside the portable guest-only snapshot contract; size-tagged SPU snapshots include the native last index but do not serialize caller-owned record buffers.

### SpuGetVoiceAttr

`SpuGetVoiceAttr` must preserve the caller's `voice` and `mask` fields because PsyQ treats `mask` as invalid output. A caller may reuse the same structure with `SpuSetVoiceAttr`; clearing the mask would convert a selective update into an all-fields update.

An ADPCM loop-start block updates the voice repeat-address register. Preserve that register across later selective pitch and volume writes; an end-and-repeat block jumps to the registered address without restarting ADSR.

### psx_spu

PsyQ reverb work units and ten 68-byte preset records are immutable defaults in `psx_spu.c`. Ordinary `SpuSetReverbModeParam` and `SpuClearReverbWorkArea` require no game table installation. The exact 40-byte work table and 680-byte preset table match both reviewed `LIBSPU.LIB` distributions; their SHA-256 values are `10fe46fe139ce95f76ad9d3cbc5a6f890df2767a828344f4c44374480930acec` and `795040d1dbeca4a4e6f2727cba671bf98a963f43c68edcb3ddebd0c347d82f4d`. `spu_bind_reverb_presets` remains a legacy override: it copies both inputs into private storage and selects those copies. Keep default SDK tables out of consuming games.

The SPU reverb processor uses the existing 32 parameter words and shared SPU RAM work area. It collects voice sends after ADSR and channel volume, collects CD sends after CD routing and volume, and processes the combined stereo input once per output frame before master volume. The wet network runs at 22.05 kHz with 39-tap input and output FIR filters. Disabling reverb stops work-area writes while retaining tail reads and cursor advancement. External input has no native stream source.

### tools

Private SPU snapshots include the reverb cursor, phase and FIR histories. This changes the size-tagged serialized state; snapshots written before this addition are rejected rather than partially restored. The public PsyQ ABI is unchanged. `tools/tests/native/spu_reverb.c` exercises the actual processor, original presets, voice and CD routing, disabled writes, work-area wrapping and snapshot continuity; these native checks do not establish hardware equivalence.

### XPORT_NATIVE_GPU_CD

With `XPORT_NATIVE_GPU_CD` defined for every translation unit, `xport.h` selects zero GPU/CD address bindings at compile time; the game must omit those five definitions. Without it, `GetGraphType` uses the required consumer-defined `const uint32 xport_gpu_graph_type_address` in `xport.h` to select its state owner. Zero explicitly selects the shared native rasterizer, which models the standard PS1 GPU and returns type zero without reading guest memory. A nonzero value preserves the unsigned guest-byte read for projects whose translated PsyQ state still lives in DRAM; define that address from the reviewed original entry. This address is distinct from the ResetGraph internal byte flag. Missing data binding still fails at link time. No registration or runtime fallback is added.

### xport 5

Declare the required CD data bindings in `xport.h`: `xport_cd_sync_callback_address`, `xport_cd_ready_callback_address`, `xport_cd_status_address` and `xport_cd_setloc_table_address`. Native consumers select `XPORT_NATIVE_GPU_CD` for every translation unit and omit all four definitions (legacy native builds can still define them as zero); use typed `CdSyncCallback`/`CdReadyCallback`; guest consumers keep their reviewed nonzero addresses and unchanged guest paths. Zero Setloc-table binding selects the shared LIBCD command flags and typed callback owner in `CdControlB`. Preliminary Nop/Setloc commands suppress the native sync callback, the primary command restores it, and exhaustion restores it after four attempts, following the original SYS routine. Zero status binding makes `CdStatus` read the existing native controller Nop status under the audio lock: mounted media bit 1, XA reading bit 5 and XA error bit 0. This reuses the controller model; it does not add physical drive timing, seek/lid status or new Red Book status flags. No runtime fallback or registration is added. `CdSyncCallbackPSX`/`CdReadyCallbackPSX` remain guest-only entries and require nonzero guest callback slots. `nextfile` returns NULL because no native memory-card medium is mounted, and `FlushCache` requires no native instruction-cache operation; neither is evidence of complete SDK emulation.

### _SsContNrpn1

For `_SsContNrpn1`, bind the numeric guest mark callback table with `xport_libsnd_bind_sequence_mark_table`. SF binds its original `8013B988` table before `_SsSeqPlay`. The address is reset with libsnd, validated on restore and included in the size-bound state; older state sizes are rejected. SPU-only consumers retain their existing link contract.

### SpuVmGuestBinding

Bind `SpuVmGuestBinding` through `spu_bind_vm_guest` before sequence VM calls. Its seventeen numeric addresses identify original context, voice records, protection mask, channel masks, release offset, mono mode, voice masks, pending keyoff and VAB globals; all are validated on binding and restore and cleared on libsnd reset. SF already publishes these tables through its VAB loader. The size-bound snapshot rejects the previous layout. Existing SPU-only consumers retain their link contract.

### _SsVmAlloc

`_SsVmAlloc` preserves the original protected-voice, signed priority, unsigned envelope and signed-age tie selection rules. The numeric protection mask is a mandatory field of `SpuVmGuestBinding`; SF binds `80115618`.

### _SsVmDoAllocate

`_SsVmDoAllocate` publishes sample addresses and ADSR words into the existing libsnd voice cache, clears the voice in sixteen channel masks and updates cache flags. `SpuVmGuestBinding` requires the channel-mask table and release-offset halfword. The cache remains owned by the existing voice-volume binding.

### SsQueueRegisters

`SsQueueRegisters(voice, attributes)` writes the existing deferred libsnd voice cache. After `SsInit`, bind its numeric values and dirty-byte tables with `xport_libsnd_bind_voice_volume`. `SndRegisterAttr` follows PsyQ's seven-halfword layout: left/right volume, pitch, mask, address units, ADSR1, ADSR2. Mask zero selects all fields; masks 1,2,4,8,32,16 select left,right,ADSR1,ADSR2,pitch,address in that order. Volume words are masked to15bits. The left dirty byte is read before its volume store; later dirty reads follow their field stores. The entry returns void, supports voices0..23 and aborts on invalid native input or missing cache binding. It reuses existing snapshot state and adds no mixer or immediate hardware update.

### SsPitchFromNote

`SsPitchFromNote` uses immutable twelve-word semitone and128-word fine tables from the reviewed PsyQ object. It retains signed16 fine/semitone arithmetic, truncating division, negative remainder correction, masked variable shifts, wrapping rounding and the original `0x3FFF` clamp. `note2pitch` reads the bound VM note/center/fine fields and calls this canonical entry. Table SHA-256 values: `3e437f2e509de31900083459ac0a2b058b144857800649cdcb2cf6cd0b272e73` and `36e9acaac352c12ac16d14a1d59c3cfda3f674c87e3ad76698144bdc1b20f451`. Existing SPU pitch converters retain their contracts.

### spu_note_pitch_table

`spu_note_pitch_table[192]` exposes the existing immutable twelve-semitone, sixteen-step pitch table owned by `psx_spu.c`; internal SPU note converters use the same array. Table LE16 SHA-256: `ec8b6f74419c647d94e99240f0ec6aa6f66ee45689b548eb26d1f779de2674b9`. Declaration: `psx_spu.h`. This additive data symbol preserves existing converter rounding, negative-note handling, voice layout and call contracts.

### _SsVmKeyOnNow

`_SsVmKeyOnNow(pitch)` consumes only the original pitch carrier; the original first argument is overwritten before use. It writes pitch and volume cache entries, voice pitch records, reverb masks and pending key-on/off masks in original order. Signed velocity/program/tone loads precede wrapping32 volume products and unsigned pan/square stages. `SpuVmGuestBinding` additionally requires mono-mode, voice-mask-block and pending-keyoff addresses. This cache publication does not establish physical SPU timing or audible playback.

### SsVoiceCheck

`SsVoiceCheck(voice, bank_program, note)` uses the existing `SpuVmGuestBinding.voices` owner and 54-byte voice records. An unsigned voice outside `0..23` returns `-1` before reading the binding. Valid voices require a bound table and compare signed halfwords at offsets `24`, `20` and `14` in that order against the arithmetic `bank_program >> 8`, its low byte and the signed16 note. All matches return zero; any mismatch returns `-1`. Missing binding aborts. This additive API changes neither existing callers nor snapshots.

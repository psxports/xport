# PsyQ video runtime

## Ownership and inputs

- `src/psx_mdec.c`: shared incremental MDEC device decoder and portable decoder state
- `src/psx_press.c`, `src/psx_press.h`: native PsyQ `LIBPRESS` entry points and transfer/callback state
- `src/psx_vlc.c`, `src/psx_vlc_table.inc`: VLC decoding, continuation state and compact immutable SDK-derived lookup data
- `src/psx_stream.c`, `src/psx_stream.h`: native PsyQ `LIBCD` STR ring and frame assembly
- Consuming projects retain movie selection, resource allocation, guest-address adapters and game callback dispatch

SDK contracts come from `D:\psyq\include\LIBPRESS.H`, `D:\psyq\include\LIBCD.H`, `D:\psyq\DOCS\LIBREF46.PDF`, the streaming file-format reference and the SDK samples under `D:\psyq\psx\sample\press`. These are local development inputs; the runtime must not open `D:\psyq` or depend on its installation.

The compact VLC lookup data is derived from PsyQ 4.6 `LIBPRESS.LIB`, SHA-256 `6ed87fda121f6dfa1a48de964599413cca3480cdb7b28cda438caef35055c159`. Preserve its Sony copyright and provenance. Runtime constants occupy 1720 bytes: 110 AC entries (440 bytes), three AC index tables (768 bytes) and packed DC prefixes/widths (512 bytes). No expanded 69632-byte table is stored or allocated by the default decoder. SDK batching, bit consumption and continuation boundaries are retained. With `MDEC_CUSTOM=1`, `DecDCTvlcBuild` reconstructs the caller-owned 69632-byte SDK table on demand; its SHA-256 remains `49dc16eab652c0bb30a23a7652cccfd1423777e2e08e71e028904bd8e020841b`.

## Boundary invariants

- PsyQ `u_long` is represented by `uint32`, including on 64-bit hosts
- Public buffer parameters and callbacks are native pointers; translated guest addresses are converted by the consumer
- `DECDCTENV` contains 64 Y quantizers, 64 chroma quantizers and 64 signed 16-bit IDCT matrix entries
- `MDEC_CUSTOM` defaults to zero; only `MDEC_CUSTOM=1` exposes `DECDCTTAB` (34816 unsigned 16-bit entries), `DecDCTvlc2`, `DecDCTvlcSize2` and `DecDCTvlcBuild`
- Callback registration returns the previous native callback pointer; do not narrow it to the SDK's 32-bit integer carrier
- STR sectors contain a 32-byte header followed by 2016 payload bytes; the ring allocation is 2048 bytes per sector
- RGB24 display reads three bytes per pixel from 16-bit VRAM storage; display X remains measured in VRAM halfwords
- Shared runtime files contain no title-specific function addresses, globals or guest callbacks

## Native playback contract

`DecDCTin` applies the PsyQ depth/STP bits to the caller's command word and snapshots the payload into runtime-owned storage. The caller may immediately reuse its VLC buffer. `DecDCTout` rounds the requested length down to 32-word SDK blocks and drains the output and completion callback chain before returning. A callback may queue the next output buffer; that transfer cannot modify buffers until the active callback returns. These native transfers deliberately bypass hardware DMA scheduling and MDEC/CD timing conflicts.

`DecDCTinSync` and `DecDCToutSync` support polling and bounded completion waits. `mdec_psyq_pump` remains available for explicit decoder progress. `DecDCTReset(1)` preserves quantization and IDCT tables; mode zero installs the SDK defaults. The low-level `MdecState` API retains its separate incremental clock/state contract.

`DecDCTinCallback` registers through `DMACallback(0, callback)` and `DecDCToutCallback` through `DMACallback(1, callback)`; both return that channel's previous native pointer. Complete PsyQ 3.6 and 4.5 objects establish both wrappers; the complete original NFSHS owner separately establishes the output wrapper. The SDK input wrapper has not been identified in the NFSHS executable. Native input/output completion reads the matching registry through `psx_dma_callback_native`; the audio lock protects the pointer read but is released before invoking it. Callback replacement through either API affects the next matching completion. This change does not implement hardware DMA scheduling, interrupt masking or concurrent callback lifetime management.

`DecDCTvlc` owns its continuation state. A null input resumes decoding. Size values below two select unlimited output; other values select the SDK halfword limit. The optional custom decoder owns independent continuation state. Define `MDEC_CUSTOM=1` consistently for the consuming build to enable its API.

`StSetRing` owns no allocation: keep the caller's `2048 * ring_size` bytes alive until `StUnSetRing`. `StSetStream` installs a native CD ready callback and `StUnSetRing` restores the previous callback. `StGetNext` locks a complete frame; `StFreeRing` releases its contiguous sectors. `StNextStatus` reports state without pumping or locking. `StSetEmulate` consumes a caller-owned sequence of 2048-byte STR sectors; the caller must provide enough sectors for the configured end condition.

For the native raw-sector path, pass the actual read mode to `stream_set_read_mode` and call `stream_pump` when polling for progress. `stream_feed_sector` accepts a 2048-byte user sector with an optional physical `CdlLOC`. `StGetBackloc` requires read mode bit `0x20` and a completed physical frame. Seek/stop commands remain the transport caller's responsibility.

## Validation and limits

Apocalypse now calls the shared DecDCT, VLC and St APIs through guest-address adapters. Native playback was exercised, and the user confirmed that the block corruption and frame splitting were fixed. The reusable native fixture `tools/tests/native/psyq_video.c` exercises input-buffer reuse, callback queue ownership, neutral RGB24 output, environment retention and STR lock/release contracts. Consumer build logs and playback records are under `Apocalypse-xport/status/psyq-video-*`.

The implementation is a native playback service, not complete PS1 hardware emulation. The standard PsyQ IDCT matrix uses the fixed-point AAN core; strict error bounds select the existing matrix calculation for rounding components that the factorization cannot prove exact. Uploaded custom matrices always use the generic path. `tools/tests/native/mdec_idct.c` compares all signed single-coefficient inputs, one million random transforms, full blocks and the custom-matrix fallback against that generic path. XA audio decoding, physical CD interrupts, DMA bus arbitration and hardware timing conflicts are not implemented by these modules. Passing fixtures and native playback do not establish hardware equivalence or every malformed-stream behavior.

## Extracted stream files

`cd_mount_stream_file(path)` in `psx_spu` mounts RIFF/CDXA `data` chunks, raw 2352-byte Mode2 sectors or 2048-byte STR user sectors. Seek sector zero with `CdIntToPos(0, &pos)` and `CdControlB(CdlSeekL, &pos, NULL)` before `CdRead2`. The existing SPU clock advances the same bounded audio/data queues. User sectors receive a native Mode2 data envelope and contain no XA audio. This mount replaces the prior image only after validating container and sector bounds; an invalid file preserves the existing mount. `cd_mount_cue` retains its raw-disc behavior and resets file offsets and stride when replacing a stream file. Consumers retain paths, movie descriptors, frame limits and presentation policy.

## Compact VLC regression

- `tools/tests/native/vlc_compact.c` is a standalone fixture that includes the actual decoder
- Build/run with `MDEC_CUSTOM=0` and `MDEC_CUSTOM=1`; supply the required `WND_TITLE`, `WND_WIDTH`, `WND_HEIGHT` and `FIELD_RATE` definitions
- Example from a configured MSVC terminal: `cl /O2 /DMDEC_CUSTOM=1 /DWND_TITLE=0 /DWND_WIDTH=960 /DWND_HEIGHT=768 /DFIELD_RATE=50 tools/tests/native/vlc_compact.c /Fe:_build/vlc-compact/regression.exe /Fo:_build/vlc-compact/`
- Assert the original SDK-table hash when custom support is enabled; compare 12000 deterministic v2/v3 stream outputs to the golden checksum captured from the previous decoder, including all 222 signed AC codes, escapes, EOB and six output limits
- Migration validation also compared every reconstructed table byte and one million AC lookahead windows against the previous SDK table

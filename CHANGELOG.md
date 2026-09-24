# Changelog

All shared Xport changes that can affect consuming projects are recorded here. Revisions use UTC ISO 8601 timestamps and are listed newest first.

## 2026-09-24T16:58:21Z

- Scope: native ordering-table traversal
- Compatibility: Compatible
- Changed: Preserved the native 16-MiB address segment across every DMA tag and initialized the resolved packet pointer; packet lengths no longer replace the segment after the first ordering-table entry
- Upgrade: Rebuild projects that compile the shared GPU runtime
- Automation: none
- Verify: X code_refresh; X build_native --configuration Release

## 2026-09-24T15:27:06Z

- Scope: Win32 frame pacing and hot-path output
- Compatibility: Compatible
- Changed: Required `timeBeginPeriod(1)` before paced sleeps with matching shutdown cleanup, prohibited ordinary per-frame stream flushes, and removed forbidden unused-parameter casts from shared `WinMain`
- Upgrade: Remove per-frame stdout/stderr writes from ordinary gameplay while preserving bounded diagnostic logs
- Automation: none
- Verify: X code_refresh; X build_native --configuration Release

## 2026-09-24T15:08:43Z

- Scope: platform-independent initialized game data
- Compatibility: Breaking
- Changed: Replaced the transient Win32 `RCDATA` contract with a project-owned byte array in a dedicated C module and removed `xport_resource_read`
- Upgrade: Compile the generated game-data C module directly; remove `.rc`, binary resource blobs and `ResourceCompile` entries
- Automation: none
- Verify: X code_refresh; X build_native --configuration Release

## 2026-09-24T14:56:20Z

- Scope: embedded initialized game data
- Compatibility: Compatible
- Changed: Added the Win32 `xport_resource_read` platform service for reviewed numeric `RCDATA`, allowing native cold starts to restore immutable initialized RAM without opening an original executable at runtime
- Upgrade: Embed only reviewed initialized-data ranges with source hash and generator provenance; do not embed original executable code or disc assets
- Automation: none
- Verify: X code_refresh; X build_native --configuration Release

## 2026-09-24T14:37:50Z

- Scope: native Windows entry point
- Compatibility: Compatible
- Changed: Replaced the manual `mainCRTStartup` linker override with shared platform `WinMain`, forwarding CRT `__argc` and `__argv` to `xport_main`
- Upgrade: Remove `EntryPointSymbol` from native projects and compile the updated shared Win32 platform source
- Automation: none
- Verify: X code_refresh; X build_native --configuration Release

## 2026-09-24T14:32:48Z

- Scope: native Windows subsystem
- Compatibility: Compatible
- Changed: New native projects use the Windows subsystem, preventing a console window while preserving redirected stdout/stderr for trace and converge logs
- Upgrade: Change every native build configuration from Console to Windows
- Automation: none
- Verify: X build_native --configuration Release

## 2026-09-24T14:24:41Z

- Scope: final native build configuration
- Compatibility: Compatible
- Changed: Debug builds remain available for diagnosis, code refresh and converge; task completion now requires the configured native executable to be rebuilt as Release, `build_native` accepts `--configuration Debug|Release`, and `project_init` emits both native configurations
- Upgrade: Before declaring a task complete, run `X build_native --configuration Release`; repeat the smallest relevant Release smoke check when completion depends on runtime behavior
- Automation: none
- Verify: X build_native --configuration Release

## 2026-09-24T13:35:33Z

- Scope: Red Book music capability gate
- Compatibility: Compatible
- Changed: `convert_music` now accepts an explicit disabled `capabilities.redbook_audio` only when the single reviewed CUE contains no `AUDIO` tracks; XA-ADPCM data tracks remain untouched
- Upgrade: Set `capabilities.redbook_audio.enabled=false` with a nonempty reason only for a reviewed data-only disc
- Automation: none
- Verify: X convert_music

## 2026-09-24T13:10:20Z

- Scope: shared command index
- Compatibility: Compatible
- Changed: Added the active lifecycle, build/database, legacy migration, workflow and routed diagnostic commands to the command index; classified unlisted Python modules as internal libraries, tests or migrations
- Upgrade: None
- Automation: marker-only
- Verify: X doctor

## 2026-09-24T12:54:25Z

- Scope: canonical emulated memory identifiers
- Compatibility: Breaking
- Changed: Renamed the shared `dram` and `scratchpad` objects to `DRAM` and `SCRATCHPAD`, matching `VRAM`
- Upgrade: Replace direct active-code references to `dram` and `scratchpad`; do not add compatibility macros or duplicate storage
- Automation: none
- Verify: X code_refresh

## 2026-09-24T12:47:04Z

- Scope: VRAM framebuffer display offsets
- Compatibility: Compatible
- Changed: GPU display offsets now translate the selected framebuffer inside the host output and fill exposed pixels with black instead of changing the VRAM source origin
- Upgrade: None; keep title-specific `DISPENV.screen` baseline handling in the project adapter
- Automation: marker-only
- Verify: X code_refresh

## 2026-09-24T12:20:46Z

- Scope: canonical native Red Book asset path
- Compatibility: Breaking
- Changed: `CdPlay` now loads decimal track WAV files only from `MUSIC` relative to the native working directory
- Upgrade: Move native Red Book assets from `bin/DATA/MUSIC` to `bin/MUSIC`; remove assumptions about the former path
- Automation: none
- Verify: X code_refresh

## 2026-09-24T12:10:37Z

- Scope: Red Book music asset conversion
- Compatibility: Compatible
- Changed: Added `tools/adpcm-xq/build.py` with an inline pinned upstream commit, archive URL and SHA-256; `prepare.bat` downloads, verifies and builds the shared BSD-3-Clause ADPCM-XQ encoder, while `convert_music` parses the project CUE, excludes pregaps at `INDEX 01`, and encodes every CD audio track as stereo 44.1-kHz standard 4-bit IMA ADPCM WAV with lookahead 5
- Upgrade: Configure `paths.music_source` or use project `iso`; configure `paths.music_output` or accept `bin/MUSIC`, then invoke the agent command `convert_music`
- Automation: none
- Verify: X convert_music

## 2026-09-24T12:04:03Z

- Scope: shared PsyQ sound and Red Book runtime ownership
- Compatibility: Breaking
- Changed: Moved VAB parsing, libsnd voice allocation, final PCM rendering and stereo 44.1-kHz IMA ADPCM Red Book transport into `psx_spu.c`; the platform worker now calls shared `spu_render`; added the consumed PsyQ `Ss*` and `Cd*` API
- Upgrade: Remove game-owned audio render, mixer, IMA ADPCM, VAB and libsnd compatibility modules; use `Ss*`, `Spu*` and `Cd*` directly; place decimal track WAV files under `MUSIC` relative to the native working directory
- Automation: none
- Verify: X code_refresh

## 2026-09-24T11:58:01Z

- Scope: shared first-pass translation module sizing
- Compatibility: Compatible
- Changed: Required stable numbered batch C/H modules for uncategorized translations, opening a new batch before 2,000 source lines or 100 translated functions; oversized single functions receive their own module
- Upgrade: Stop extending oversized catch-all translation files; add the next numbered batch to the native build and leave already indexed functions in place until the later semantic-consolidation phase
- Automation: none
- Verify: X code_refresh

## 2026-09-24T11:52:26Z

- Scope: shared first-pass decompilation memory access
- Compatibility: Compatible
- Changed: Added writable `byte_`, `word_` and `dword_` guest-memory lvalue macros to `xport.h`; each accepts a complete guest address and routes it through `psx_addr`, whose public declaration now lives in `xport.h`
- Upgrade: Use `byte_(0xADDRESS)`, `word_(0xADDRESS)` and `dword_(0xADDRESS)` when translating matching IDA globals; retain explicit signed casts and do not add complete guest addresses directly to `dram`
- Automation: none
- Verify: X code_refresh

## 2026-09-24T11:20:01Z

- Scope: shared PSX address mapping and host runtime environment switches
- Compatibility: Breaking
- Changed: Moved guest/native address resolution into `psx.c` as `psx_addr` over the canonical `dram` and `scratchpad`, removed the project-owned `xport_address` binding, and fixed host switch names in the shared runtime
- Upgrade: Replace `xport_address` with `psx_addr`; remove project RAM/scratchpad duplicates and environment-name extern definitions; use `XPORT_AUDIO_OUTPUT`, `XPORT_AUDIO_BACKEND`, `XPORT_RASTERIZE` and `XPORT_VERIFY_VRAM`
- Automation: none
- Verify: X code_refresh

## 2026-09-24T10:18:37Z

- Scope: Windows host audio render boundary
- Compatibility: Breaking
- Changed: Removed the public `xport_audio_render` binding and made the Windows audio worker call the game-owned `audio_runtime_render` mixer directly
- Upgrade: Remove the `xport_audio_render` trampoline; make `audio_runtime_render` clear the complete destination before any early return
- Automation: none
- Verify: X code_refresh

## 2026-09-24T10:00:02Z

- Scope: shared SPU fixed-point mixer arithmetic
- Compatibility: Compatible
- Changed: Quantized master volume to `0..256` before applying it to the 32-bit voice accumulator, restored 32-bit volume and Gaussian products after proving their bounds, and retained the single final `sint16` clamp
- Upgrade: Remove 64-bit arithmetic added only to protect master-volume multiplication; apply normalized master volume with a 32-bit multiply followed by `>> 8`
- Automation: none
- Verify: X code_refresh

## 2026-09-24T09:43:22Z

- Scope: shared SPU and host audio mixing
- Compatibility: Breaking
- Changed: Replaced the 16-bit intermediate SPU output with `spu_mix` into a 32-bit accumulation buffer, mixed music before the sole final clamp, and widened Gaussian and fixed-point volume products to 64 bits before shifting
- Upgrade: Replace calls to `spu_render` with a zeroed `sint32` accumulation buffer passed to `spu_mix`; add every audio source in 32 bits and clamp once when writing the final interleaved `sint16` output
- Automation: none
- Verify: X code_refresh

## 2026-09-24T09:34:21Z

- Scope: shared PsyQ SPU ABI and state ownership
- Compatibility: Breaking
- Changed: Removed the parallel `VOICE_00_LEFT_RIGHT` register mirror and made `SPU_STATE` canonical; added the PsyQ 4.6 `SpuVolume`, `SpuVoiceAttr`, `SpuCommonAttr`, `SPU_VOICE_*`, `SPU_COMMON_*` and implemented `Spu*` entrypoints used by consumers
- Upgrade: Replace project-defined SPU attribute layouts and direct `spu_set_*` voice calls with shared `SpuVoiceAttr`, `SpuSetVoiceAttr`, `SpuGetVoiceAttr`, `SpuSetKey` and `SpuSetCommonAttr`; do not access `VOICE_00_LEFT_RIGHT`
- Automation: none
- Verify: X code_refresh

## 2026-09-24T09:17:34Z

- Scope: shared PsyQ SPU register state
- Compatibility: Compatible
- Changed: Made `spu_init` and every `spu_set_*` register setter update the emulated SPU register mirror so render-time synchronization cannot restore stale zero voice or master-volume registers
- Upgrade: Remove project workarounds for silent SPU voices and use the shared setters for voice registers, pitch, volume and master volume
- Automation: none
- Verify: X code_refresh

## 2026-09-24T09:02:19Z

- Scope: shared PsyQ GPU polygon rasterization
- Compatibility: Compatible
- Changed: Replaced bounding-box barycentric polygon traversal with scanline span traversal while retaining VRAM, texture, shading and semitransparency behavior
- Upgrade: Compile the shared `psx_gpu.c` translation unit with optimization enabled; for MSVC Debug builds disable incompatible basic runtime checks on that translation unit
- Automation: none
- Verify: X code_refresh

## 2026-09-24T08:46:10Z

- Scope: shared PsyQ GPU runtime and frame presentation
- Compatibility: Breaking
- Changed: Made `PutDispEnv` state-only like PsyQ, added E3/E4/E5/E6 `DR_ENV` execution and background tiles to `DrawOTag`, and removed per-packet `VirtualQuery` validation from optimized native OT traversal
- Upgrade: Call `gpu_present` once after every logical frame's final `DrawOTag`, and never use `PutDispEnv` as a presentation boundary
- Automation: none
- Verify: X code_refresh

## 2026-09-24T08:34:46Z

- Scope: shared PsyQ GPU runtime
- Compatibility: Breaking
- Changed: Made `psx_gpu.c` the sole owner of the PsyQ graph, ordering-table and draw/display-environment entrypoints; `PutDispEnv` now scans out the selected VRAM rectangle through `gpu_present`
- Upgrade: Remove `XPORT_GPU_EXTERNAL_CONTROL` and all project definitions of `ResetGraph`, `DrawSync`, `SetDispMask`, `DrawOTag`, ordering-table helpers and environment helpers
- Automation: none
- Verify: X code_refresh

## 2026-09-24T08:22:11Z

- Scope: shared PsyQ ordering-table runtime
- Compatibility: Compatible
- Changed: Made `ClearOTag`, `ClearOTagR`, `AddPrim` and `AddPrims` consume native C pointers directly and preserved the native 16-MiB segment while `DrawOTag` follows 24-bit OT links
- Upgrade: Remove project ordering-table pointer trampolines; ensure each native OT and its linked primitive pool occupy one 16-MiB segment
- Automation: marker-only
- Verify: X code_refresh

## 2026-09-24T08:14:57Z

- Scope: shared PsyQ PAD runtime
- Compatibility: Compatible
- Changed: Corrected `PadInitDirect` digital packets to use the libpad buffer layout `00 41 low high` for a connected controller and eight `FF` bytes for a disconnected controller
- Upgrade: Remove project workarounds for malformed direct-pad packets and publish active-low button words through the shared `pad_publish`
- Automation: none
- Verify: X code_refresh

## 2026-09-24T08:02:51Z

- Scope: shared PsyQ GTE ABI
- Compatibility: Compatible
- Changed: Matched PsyQ 4.6 `LIBGTE.H` names and 32-bit integer ABI by exposing `sint32 ccos(sint32 a)` and `sint32 csin(sint32 a)`, using the existing 4.12 table and removing the non-public `csincos` declaration
- Upgrade: Remove project declarations of `csincos`; use only the shared `ccos` and `csin` PsyQ prototypes with `int` mapped to `sint32`
- Automation: none
- Verify: X code_refresh

## 2026-09-24T07:50:41Z

- Scope: shared PsyQ GTE runtime
- Compatibility: Compatible
- Changed: Added the confirmed PsyQ `csincos`, `ccos` and `csin` 4.12 fixed-point implementations so game calls cannot resolve to the incompatible C runtime complex-math symbols
- Upgrade: Remove project-local copies of these functions and use their exact PsyQ prototypes from the shared runtime
- Automation: marker-only
- Verify: X code_refresh

## 2026-09-24T06:08:30Z

- Scope: shared GPU runtime, project bindings, project bootstrap
- Compatibility: Breaking
- Changed: Removed `XPORT_GPU_BINDINGS`; shared GPU state now derives drawing bounds from PsyQ environments and keeps reset state internally
- Upgrade: Remove the project-owned `xport_gpu_bindings` declaration
- Automation: none
- Verify: X code_refresh

## 2026-09-24T05:42:31Z

- Scope: upgrade workflow, project bootstrap
- Compatibility: Compatible
- Changed: Added `CHANGELOG.md`, the project `XPORT REVISION` marker and the low-context `X upgrade` planner and automation
- Upgrade: Add the current revision marker directly before the project-owned `xport_main` definition
- Automation: marker-only
- Verify: X doctor

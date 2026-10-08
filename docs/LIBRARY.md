# Shared PSX host runtime

Projects compile this canonical PSX/PsyQ runtime directly. Game code calls its PsyQ-compatible API; shared subsystems call required game-owned symbols declared in `xport.h`. No callback registration or fallback is permitted; missing bindings fail at link time.

## Modules

- `xport.h`: base public header for fixed-width types, PsyQ PAD constants, function `GDB_CALL` and data `GDB_DATA` exports, platform services, required game bindings and the mandatory project-defined `WND_TITLE`, `WND_WIDTH`, `WND_HEIGHT` and `FIELD_RATE` contract
- `psx.c` / `psx.h`: platform-neutral PsyQ-compatible scalar types, `DRAM`/`SCRATCHPAD` address mapping through `psx_addr`, GPU packet ABI, matrix/GTE helpers, pad services and PS-X executable/CD loading
- `psx_gpu.c` / `psx_gpu.h`: the 16-bit `VRAM` source of truth, direct PsyQ image operations, immediate `DrawPrim` submission, ordering-table traversal, packet rasterization and display-rectangle conversion to the 32-bit platform presentation buffer; `LoadImagePSX` and `StoreImage` consume native C pointers
- `psx_spu.c` / `psx_spu.h`: deterministic SPU RAM, PsyQ libsnd/VAB voices, PSX ADPCM, ADSR, IMA ADPCM Red Book transport and final PCM mixing
- `xport_trace.c` / `xport_trace.h`: guest-memory access, memory observation, audit events, call and device tracing, lockstep boundaries, frame replay, shared mappings and checkpoints
- `platform/win/main.c`: Windows process entry, timer, exhaustive message/input polling, window, WaveOut audio, audio synchronization and file services

Each platform backend owns the native process entrypoint and calls the required game-owned `void xport_main(void)`. The backend consumes common flags, exposes remaining title-specific arguments through `xport_arg_count` and `xport_arg_values`, and receives the result through `xport_set_exit_code`. A port to another operating system implements the platform service declarations from `xport.h` in its own `platform/<name>/main.c`; `psx.c` contains no operating-system calls.

`psx.c` loads a `PSX_EXE` description into shared guest RAM and provides common CD path, size and read helpers. Games own only executable addresses, paths and the call into their translated entry. `xport_trace.c` owns memory access, audit, device events and lockstep diagnostics; a game-owned `game_except.c` provides the static exception arrays used by lockstep builds; no configuration parser runs inside the game.

PsyQ entries are implemented directly in their owning shared module. Internal subsystem functions, types and constants use the `gpu_*`, `spu_*` and `gte_*` namespaces. Platform presentation and quit state use `xport_present` and `xport_isquit`; do not add `psx_*` forwarding aliases, parallel `PsyQ*` compatibility state or game-owned GPU trampolines.

Use `rsincos(angle, &sine, &cosine)` when a translated game function reads PsyQ's packed `rcossin_tbl` directly. This function computes both original signed 16-bit table values for the same angle and is not equivalent to calling `rsin` and `rcos` separately. Do not expose or duplicate the packed guest table solely for those reads.

Game memory maps, translated game logic, title-specific resource loading, WIP behavior and checkpoint orchestration stay in each project. Extend the required game-binding section of `xport.h` only for public cross-platform contracts. The platform audio worker calls shared `spu_render`; games use PsyQ `Ss*`, `Spu*` and `Cd*` calls and own no render callback, mixer, VAB registry or Red Book decoder.

The host Red Book path interprets each `CdlATV` byte as a normalized 8-bit weight: `0xFF` is direct unity and two `0x7F` contributions form a unity mono sum. Apply the CD matrix before SPU CD volume, voice summation, final saturation and master volume. Saturate every decoded PSX and IMA ADPCM sample before storing it in predictor history.

`CdSetToc` installs the native disc catalogue. `CdGetToc` and `CdGetToc2` return its track count and copy the lead-out entry plus every track entry under the audio lock. A null output or an unset catalogue returns zero. The native model does not issue physical CD commands; the reviewed `MAIN.EXE` implementation ignores the `CdGetToc2` mode argument.

`DMACallback` keeps an independent native callback slot for each of the seven DMA channels and returns the previous binding. Invalid channels return null. `CdDataCallback` uses channel 3. This registry implements registration and removal, not hardware DMA interrupt delivery.

`CD_cw` dispatches through the immediate native CD command model and converts successful dispatch to zero, as required by `CdControlB`. `CD_sync` returns that model's completion status. Hardware command timing and interrupt delivery are not modeled by these native entries.

`xport_gte_execute` uses the existing MVMVA engine for its supported commands: `0x480012` transforms V0 with translation, `0x49E012` transforms IR without translation, and `0x486012` transforms V0 without translation for `MulMatrix0`. Other commands remain unsupported. All three preserve signed fixed-point rounding, MAC wrapping and IR saturation in that engine.

`SpuGetVoiceAttr` must preserve the caller's `voice` and `mask` fields because PsyQ treats `mask` as invalid output. A caller may reuse the same structure with `SpuSetVoiceAttr`; clearing the mask would convert a selective update into an all-fields update.

An ADPCM loop-start block updates the voice repeat-address register. Preserve that register across later selective pitch and volume writes; an end-and-repeat block jumps to the registered address without restarting ADSR.

Set `XPORT_FRAME_PROFILE` to an absolute CSV path before launching a Windows native executable to measure every successful `xport_present`. Each row records the interval since the prior presentation, time spent in the FIELD_RATE wait, non-waiting wall time, GDI presentation time, main-thread and process CPU time, submitted WaveOut buffers, callback semaphore overruns, output sample count, samples at either 16-bit rail and the maximum observed PCM magnitude. Close the game normally to flush and close the CSV. An unset or empty variable performs no profiling and opens no file.

Set `XPORT_AUDIO_CAPTURE` to an absolute file path to capture the exact signed 16-bit little-endian stereo 44.1 kHz PCM submitted to WaveOut before host muting. Close the game normally to flush the capture. An unset or empty variable performs no capture.

Set `XPORT_AUDIO_STEM_CAPTURE` to an absolute file path to capture six signed 32-bit little-endian channels at 44.1 kHz before saturation: voice left/right, CD left/right and combined left/right. This diagnostic is independent of `XPORT_AUDIO_CAPTURE`. An unset or empty variable performs no stem capture.

Set `XPORT_AUDIO_VOICE_PROFILE` with `XPORT_AUDIO_STEM_CAPTURE` to an absolute CSV path to sample every active SPU voice at 10 Hz. Each row records the voice index, ADSR phase and envelope, pitch, left and right volume, sample note, start/current/repeat SPU RAM addresses, ADPCM block flags and key state. Close the game normally to flush the profile. An unset or empty variable performs no voice profiling.

`GetGraphType` reads the unsigned guest byte at required game-owned `const uint32 xport_gpu_graph_type_address`, declared in `xport.h`. Each consumer must define this binding from its reviewed original entry. This address is distinct from the ResetGraph internal byte flag. Missing data binding fails at link time.

`OpenEvent`, `EnableEvent`, `DisableEvent` and native `CloseEvent` retain the minimal eight-record native event registry. Native handles are untagged and start at one; callbacks are stored but not dispatched; exhausted opens retain the prior monotonic handle behavior. Tagged BIOS handles in `CloseEvent` keep their guest-table path. This registry does not implement complete BIOS event delivery. `CdStatus` returns zero, `nextfile` returns NULL, and `FlushCache` is a no-op. These services are shared compatibility stubs, not evidence of complete SDK emulation.

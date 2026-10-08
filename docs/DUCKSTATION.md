# DuckStation lockstep runtime

Normative extension of `../AGENTS.md`

The shared runtime is reproduced from `duckstation/duckstation-manifest.json`. The manifest pins upstream commit `5fd366809053fe287291de7a39752c4d5d5b146b`, dependencies, build settings, the complete `internal-trace-api.patch` and required executable markers. `tools/duckstation_build.py` builds and publishes the distribution. Project BIOS, disc images, settings and save states remain in isolated project data directories.

The patch exposes only the runtime features needed by recording, coverage discovery and converge:

- `XPORT_DUCKSTATION_DATA_ROOT` selects the project runtime
- `qXportLockstepRecordStart` buffers one effective two-port controller state at each FrameDone without pausing; `qXportLockstepRecordRead` drains bounded pages after one final stop
- `qXportFunctionCoverageStart`, `qXportFunctionCoverageTarget`, `qXportFunctionCoverageStop` and `qXportFunctionCoverageRead` collect the first executed instruction at each registered function entry without pausing gameplay; independent in-emulator open-addressed target and seen tables reject unregistered and duplicate addresses
- `qXportLockstepReplay`, `qXportLockstepInput` and `qXportLockstepSeed` reproduce a record from its exact initial state
- `qXportLockstepStart`, `qXportLockstepBoundary`, `qXportLockstepMode` and `qXportLockstepNextEvent` expose semantic call and VBlank boundaries
- `qXportLockstepAttach` and `qXportLockstepSeed` initialize shared DRAM, scratchpad and canonical VRAM from the paused original state; `qXportLockstepExceptions`, `qXportLockstepCompare` and `qXportLockstepDiff` compare shared CPU memory inside DuckStation
- `qXportLockstepDMA` captures bounded DMA state for a divergence packet
- `qXportLockstepCheckpointCapture:FRAME`, `qXportLockstepCheckpointRestore:FRAME` and protocol-4 `qXportLockstepCheckpointRestore:FRAME:TARGET` retain one rolling machine state for diagnosis and cross-attempt prefix resume
- Protocol 23 publishes ordered SPU key and envelope-read events with exact sample ticks through the shared deferred mapping; comparison rejects missing, extra, reordered or unconsumed events

The runtime has no raw trace writer, memory sidecar, periodic state capture, GPU/SPU dump stream or project trace profile. Recording uses the save state passed to `record.ps1 -Action start` and stores only its immutable copy plus the FrameDone input sequence.

`duckstation_deploy --role ROLE --ensure` creates or validates an isolated runtime. New projects reserve `user` for manual play and `lockstep` for record + converge. The tracked settings template supplies the role's GDB port. Interactive recording forces normal speed, disables rewind/runahead and preserves controls and audio. Automated converge keeps fast-forward enabled in both prefix and detailed replay; detailed mode adds semantic boundary stops without changing host presentation speed.

`duckstation_run --role user --visible` and the project `DuckStation.bat` are for manual play and save-state creation. Manual recording uses `record.ps1 -Action start` or the generated project `record SLOT NAME`; both create their worker on `WinSta0\Default`. Do not launch a second emulator before recording. For a `stop` task, run `record.ps1 -Action stop`; it resolves the single active recording and closes the owned process after committing the record.

For interactive original-function discovery, follow [FUNCTION_COVERAGE.md](FUNCTION_COVERAGE.md). The DuckStation protocol observes registered entry addresses directly; it must not infer calls from call depth, return matching, PRE_CALL, POST_CALL or reviewed tail-call metadata.

The event stream must be stopped at a semantic boundary before comparison. Python transports compact events and bounded difference windows only; it never loads full DRAM or scratchpad. VRAM and SPU RAM remain outside the initial comparator because CPU access is mediated by GPU, DMA and PsyQ wrappers.

Protocol 23 patch `25a99c8a66cd7af93bd052414b1e96f8d5ef9b75be0d3176a347b3f0d234ba2b` emits the literal `MAIN.EXE` as the image field for every event. For this verified runtime only, the replay adapter binds that transport label to `record.original.image`. The runtime launch verifies executable/receipt hashes; record validation verifies the original executable hash and ABI image before binding. Preserve the wire label, record ID, original executable hash and patch hash in event source metadata. Reject any different wire image, side or protocol. Other runtime patches retain their image fields unchanged. This binding supplies missing transport metadata; canonical image equality, all event fields and memory comparisons remain mandatory.

Converge verifies the actual launched EXE against its adjacent `xport-runtime.json` receipt and requires the current lockstep protocol and the record's upstream DuckStation commit. Each attempt stores the verified executable and patch hashes. The source manifest describes a build recipe; an attention packet cites the verified running patch instead. A compatible record from an earlier patch is marked as such in telemetry, while the runtime hashes are omitted from the model repair prompt.

Checkpoint capture reuses two in-memory buffers. DuckStation serializes into the candidate buffer and swaps it with the active checkpoint only after a successful save, so a failed capture leaves the preceding checkpoint valid. Restore uses the exact serialized length. No per-frame checkpoint file is written. `tools/lockstep_duck_checkpoint_probe.py --rolling-captures` verifies repeated captures before restoring the latest one.

The three-field restore resumes at `FRAME+1` in PREFIX_GUARD and switches to detailed comparison only at `TARGET`. The two-field command immediately enters a detailed epoch and remains the same-process diagnostic path. The supervisor and rebuilt native process use the same target; the native cold-start prefix flag is consumed after its first use so a later checkpoint rewind can enter DETAILED mode.

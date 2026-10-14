# DuckStation lockstep runtime

Normative extension of `../AGENTS.md`

The shared runtime is reproduced from `duckstation/duckstation-manifest.json`. The manifest pins upstream commit `5fd366809053fe287291de7a39752c4d5d5b146b`, dependencies, build settings, the complete `internal-trace-api.patch` and required executable markers. `tools/duckstation_build.py` builds and publishes the distribution. Project BIOS, disc images, settings and save states remain in isolated project data directories.

The xport build uses one cooperative execution mode. CPU interpretation and software GPU rendering run on the core thread. CPU instruction ticks do not advance device time. Registered blocking MIPS branches in the project's `duckstation.wait_points` advance the common event queue to its next deadline; a branch that does not wait leaves device time unchanged. Equal deadlines use event-name order. Guest IRQ flags remain pending until a wait service delivers them. GPU commands, DMA copies, MDEC block publication and SPU RAM transfers complete synchronously with real data; CD sector reads use the core thread. CRTC, timers, audio sampling and serial events use the same virtual queue.

`qXportWaitPoints:PC:PC` loads the project's blocking-poll registry before record or replay begins. `qXportSpuRAMRead:OFFSET:SIZE` imports initial SPU RAM after the ordinary DuckStation loader has accepted the state; Native must not parse a particular save serializer version. `qXportFastForward:0|1` changes host presentation speed only and is used by the determinism probe. These commands do not select execution profiles.

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

Recording embeds the initial save state and the two-port FrameDone sequence in `record.bin`. The binary record has no version, hash inventory or migration path. Use the ordinary DuckStation save loader; do not add save-state version or profile admission checks.

`duckstation_deploy --role ROLE --ensure` creates or validates an isolated runtime. New projects reserve `user` for manual play and `lockstep` for record + converge. The tracked settings template supplies the role's GDB port. Interactive recording forces normal speed, disables rewind/runahead and preserves controls and audio. Automated converge keeps fast-forward enabled in both prefix and detailed replay; detailed mode adds semantic boundary stops without changing host presentation speed.

`duckstation_run --role user --visible` and the project `DuckStation.bat` are for manual play and save-state creation. Manual recording uses `record.ps1 -Action start` or the generated project `record SLOT NAME`; both create their worker on `WinSta0\Default`. Do not launch a second emulator before recording. For a `stop` task, run `record.ps1 -Action stop`; it resolves the single active recording and closes the owned process after committing the record.

For interactive original-function discovery, follow [FUNCTION_COVERAGE.md](FUNCTION_COVERAGE.md). The DuckStation protocol observes registered entry addresses directly; it must not infer calls from call depth, return matching, PRE_CALL, POST_CALL or reviewed tail-call metadata.

The event stream must be stopped at a semantic boundary before comparison. Python transports compact events and bounded difference windows only; it never loads full DRAM or scratchpad. VRAM and SPU RAM remain outside the initial comparator because CPU access is mediated by GPU, DMA and PsyQ wrappers.

Protocol 23 emits the fixed `MAIN.EXE` transport label. The adapter binds it to the current project's original image after runtime verification. Preserve the transport label and source identity in diagnostic metadata. Do not maintain a patch-hash allowlist for this binding.

Converge verifies the actual launched EXE against its adjacent `xport-runtime.json` receipt and requires the current lockstep protocol. Each attempt stores the verified executable and patch hashes. The source manifest describes a build recipe; an attention packet cites the verified running patch instead. A compatible record from an earlier patch is marked as such in telemetry, while the runtime hashes are omitted from the model repair prompt.

Checkpoint bytes start with two `u32` sizes, then contain the ordinary machine state and the cooperative wait continuation. The continuation preserves the serviced poll addresses, the active poll and its event deadline across a FrameDone/GDB stop. Normal `.sav` files retain DuckStation's own format and loader.

Checkpoint capture reuses two in-memory buffers. DuckStation serializes into the candidate buffer and swaps it with the active checkpoint only after a successful save, so a failed capture leaves the preceding checkpoint valid. Restore uses the exact serialized length. No per-frame checkpoint file is written. `tools/lockstep_duck_checkpoint_probe.py --rolling-captures` verifies repeated captures before restoring the latest one.

The three-field restore resumes at `FRAME+1` in PREFIX_GUARD and switches to detailed comparison only at `TARGET`. The two-field command immediately enters a detailed epoch and remains the same-process diagnostic path. The supervisor and rebuilt native process use the same target; the native cold-start prefix flag is consumed after its first use so a later checkpoint rewind can enter DETAILED mode.

Final verification command after all implementation and the integrated build:

```text
python -B tools/lockstep_determinism_probe.py --project PROJECT_ROOT --record NAME --frames N
```

The probe compares complete exported machine checkpoints and FrameDone events for normal speed, fast-forward and fast-forward with additional instruction breakpoints. It must observe actual additional stops. A missing frame, unequal event or unequal machine state is a failure. Results are written to the project's `status/diagnostics/cooperative-determinism/result.json`. Do not treat the existence of the probe as acceptance evidence; acceptance requires its successful run and the Native contract checks.

Native rolling and portable checkpoints preserve the CD sector cursor, partially consumed sector FIFO, current sector bytes, mode, command completion and response, and complete XA decoder, PCM and data queues. Keep the mounted disc handle as a process binding; sector reads seek from the restored cursor. The Road Rash check is `python -B RoadRashJailbreak-xport/tools/test_cooperative_cd_checkpoint.py` from the xports workspace after a LockstepDebug build. It must restore the next unread FIFO bytes after mutation through both memory and file checkpoints and resume the existing audio decoder test in a fresh process. Its result is `RoadRashJailbreak-xport/status/diagnostics/cooperative-cd-checkpoint/result.json`.

Native CD sync and audio-end callbacks enter an ordered queue stored in the sound checkpoint. A bound wait permits the existing BIOS IRQ service to drain it; ordinary functions, direct CD delivery attempts and `VSync(-1)` leave it pending. The CD check also verifies copied response bytes, callback order and queue restoration. Headless LockstepDebug uses the core-thread null audio path even when `XPORT_AUDIO_BACKEND=waveout` is present; host audio consumption must not advance device state.

Native completed MDEC DMA callbacks and DrawOTag completion callbacks use services registered in the common wait dispatcher. MDEC copies decoded output synchronously and retains its existing per-channel buffer ownership across recursive completion callbacks. DrawOTag records pending completion separately from rasterization; GPU checkpoints preserve that pending count. Run `python -B RoadRashJailbreak-xport/tools/test_cooperative_device_callbacks.py` after a LockstepDebug build. Require real VRAM and decoded pixel values before callback delivery, a cleared MDEC busy flag, no callbacks from `VSync(-1)`, correct recursive MDEC buffer ownership inside the wait, and GPU callback delivery after checkpoint restore. Evidence is `RoadRashJailbreak-xport/status/diagnostics/cooperative-device-callbacks/result.json`.

Road Rash portable checkpoints also store the complete Native MDEC decoder, owned input buffer, remaining DMA destination as a guest address and pending completion flags. The device check restores both a completed output with pending callbacks and an unfinished output waiting for input. SPU portable bindings store their Native/guest owner and guest control addresses; loading reconstructs host pointers without resetting saved registers. Run `python -B RoadRashJailbreak-xport/tools/test_cooperative_project_checkpoint.py` to verify integrated checkpoint export, mutation and jump, fresh-process import, and real synchronous SPU copies through both restored binding types. Evidence is `RoadRashJailbreak-xport/status/diagnostics/cooperative-project-checkpoint/result.json`. Road Rash SPU completion and CD streaming services must retain pending work when `psx_callbacks_allowed()` is false.

Accepted Native CD commands update the bound drive queue: Setloc remains pending until Read, repeated Read preserves the current deadline, Setmode changes the sector interval, and Pause/Stop cancel reading and pending ready events. A bound drive owns sector publication; empty FIFO reads and the audio consumer must not fetch sectors independently. Published XA sectors feed the existing decoder without producing a data-ready event. Mount the disc before portable state loading, then restore the CD IRQ and command bindings without clearing the restored FIFO. Run `python -B RoadRashJailbreak-xport/tools/test_cooperative_cd_commands.py`; require a real ISO volume descriptor, no publication outside a bound wait, correct queue transitions and no implicit reads from an empty FIFO. Evidence is `RoadRashJailbreak-xport/status/diagnostics/cooperative-cd-commands/result.json`.

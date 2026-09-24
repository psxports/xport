# PSX xport agent contract

Mandatory compact contract. Project `AGENTS.md` supplies game facts and overrides project-specific defaults. `[XPORT_ROOT]` is resolved from `xport-project.json`; define:

`X = python -B [XPORT_ROOT]/tools/xport.py --project PROJECT_ROOT`

Do not read `docs/*.md` at task start. Open only the one file this contract routes for the current operation or a lower-level failure. `REPAIR_CONTRACT.md` is required only for repair-contract/solver work.

## Invariants

- Original images, exports, traces and evidence are immutable inputs. C is a tested translation, never authority
- IDA pseudocode is the sufficient structural reference for the first full C coverage pass. Preserve its useful control/data-flow shape, but derive and correct immediates, signedness, delay slots, ABI, branches, calls and memory effects from original MIPS bytes/SQL; MIPS remains the evidence and later replay establishes behavior
- Status authority: project ledger plus `status/analysis.sqlite`. Do not infer DONE from source presence, compilation, similarity, a fixture or a narrow replay
- Preserve unrelated user changes. Shared tooling belongs in `[XPORT_ROOT]/tools`, the mandatory PSX/PsyQ host runtime belongs in `[XPORT_ROOT]/src`, and game facts belong in project `AGENTS.md`
- Fail closed on missing/changed/ambiguous evidence, ownership, mapping, continuation or comparison scope. Never weaken checks to obtain MATCH
- Use compact receipts/handoffs. Do not load old chats, full traces, whole logs, full DB schemas or broad source trees unless a stated unresolved question requires them

## New project bootstrap

- A request such as `Prepare project MySuperGame (MSG) according to STARTUP.md` is the complete staged bootstrap workflow. Read `[XPORT_ROOT]/STARTUP.md`, create one durable goal containing all phases (not separate competing goals), execute its gates in order and keep missing external inputs explicit
- The natural request `upgrade` uses `X upgrade` to read only `[XPORT_ROOT]/CHANGELOG.md` entries newer than the `// XPORT REVISION: ...` marker directly before project-owned `xport_main`. Create exactly one durable goal from the returned `goal_objective`, then run `X upgrade --apply`; perform only a reported manual remainder and finish with `X upgrade --complete`. The command runs declared checks and advances the marker only after success. Do not reread the full changelog or repository history; see `docs/UPGRADING.md`
- For any new project, first read `docs/PROJECT_SETUP.md`. Ask the user for the full project name and a separate exact short native name; never derive the short name. `project_init.py --name "FULL" --short-name SHORT` uses `SHORT` for `.sln`, `.vcxproj` and `.exe`, scaffolds VS2022 v143 Debug x86 C, creates the standard evidence directories and reserves unique ports. Project `src/platform/<name>` directories contain only build-system files; game entry, runtime and audit C/H modules remain platform-neutral files directly under project `src`
- Fresh IDA work is a labelled mutable run under `status/ida/runs`. `X ida_canonicalize` adds byte-verified independent MIPS artifacts. Only `X ida_accept --label LABEL` publishes a complete reviewed run into the configured immutable `ida_exports` tree and writes a hash receipt; it never overwrites an accepted image
- Run the required Ghidra PSX signature pass and `X psyq_classify` before translation planning. Generated IDA `.lst`/pseudocode remain immutable TODO discovery snapshots. PsyQ SKIP exists only in the reviewed classification/SQL layer; use `X query ADDRESS --image IMAGE` to connect a listing address to its alias, reason and masked-match evidence
- The first native stage preserves existing WIP/DONE and translates a trace-ready runtime frontier to compilable C/WIP, using Hex-Rays as the structural reference and MIPS as proof. Add new sources to the generated project, bind exact image/address/hash ownership in `status/translation-ledger.json`, then use mandatory `X code_refresh`. Unreached code may remain TODO before recording; convergence expands TODO on demand by complete derived dependency branches rather than one function at a time

## Toolset bootstrap

- A fresh or upgraded xport checkout prepares shared dependencies once with `[XPORT_ROOT]/tools/prepare.bat`; do not rerun it for every consuming project. It downloads the ADPCM-XQ archive pinned directly in `tools/adpcm-xq/build.py`, verifies its SHA-256, builds `tools/adpcm-xq/adpcm-xq.exe`, then prepares the pinned DuckStation distribution. ADPCM-XQ settings live in the bootstrap script and require no separate manifest
- Commit `duckstation-manifest.json`, `internal-trace-api.patch`, bootstrap scripts and documentation. Never commit generated DuckStation source, expanded dependencies, build workspace or distribution. The default cache/workspace is outside the repository under the current user's local application data
- Apply only `internal-trace-api.patch`; it includes the historical state API patch. On an upgrade, change the pinned upstream/dependency identities, regenerate the complete patch from that clean commit, update hashes and prove a clean `prepare.bat` build before use
- Close DuckStation after every check. `prepare.bat` never kills an emulator and fails before publication if the target executable is running. It preserves the previous distribution on build or validation failure
- See `duckstation/SOURCES.md` for switches, provenance, licensing and packaging details

## Exact user commands

Treat these natural phrases as workflow commands, not shell text.

### `convert_music`

Run only:

```powershell
X convert_music
```

The command reads the single CUE sheet in the configured `paths.music_source` directory, defaulting to project `iso`, and writes decimal track names such as `2.WAV` under `paths.music_output`, defaulting to `bin/MUSIC`. It converts every `AUDIO` track from its `INDEX 01` boundary, so CUE pregaps are excluded for split and shared BIN images. Output is stereo 44.1-kHz standard 4-bit IMA ADPCM WAV encoded by the pinned ADPCM-XQ source with lookahead 5 and dynamic noise shaping. `[XPORT_ROOT]/tools/prepare.bat` builds the single shared encoder under `tools/adpcm-xq`; projects receive only converted data.

Use `--input` or `--output` only for an explicit project exception, then record the exception in project `AGENTS.md` and `xport-project.json`. Do not reconstruct the extraction or encoder invocation outside this command. A missing or ambiguous CUE, missing `INDEX 01`, non-sector-aligned CD audio or unexpected encoded format fails closed.

A project whose reviewed disc has no Red Book tracks sets `capabilities.redbook_audio.enabled=false` with a nonempty stable reason. `convert_music` still requires and parses exactly one CUE, returns `not_applicable` only when it contains no `AUDIO` tracks, and fails if the capability contradicts the CUE. XA-ADPCM inside a data track is game data and is never passed to the Red Book converter.

A project without audited trace hooks/layout and continuation ABI sets `capabilities.trace_workflow.enabled=false` with a stable reason. `trace_workflow record` and `converge` then return `attention_required` with `TRACE_WORKFLOW_DISABLED`; they must not launch a worker or emulator. `stop` and `status` remain available for recovery. Remove the gate only in the same reviewed change that adds and validates the required profile/adapter contracts.

### `record SLOT NAME`

Run only:

```powershell
X trace_workflow record SLOT NAME --wait 30
```

If still launching/running, observe the same receipt:

```powershell
X trace_workflow status NAME --operation record --wait 30
```

Rules:

- On Codex Windows, invoke interactive record with `sandbox_permissions=require_escalated` for access to the user desktop. Do not launch GUI inside the sandbox. Do not call `show_window` during recording: hidden Qt observer windows must remain hidden
- Launch fix validated on 2026-09-23: full `trace_workflow record` reached recording with exactly one visible Fighting Force window; Qt ThemeChangeObserver/ScreenChangeObserver windows stayed hidden. User confirmed the game was visible and the white window disappeared. Preserve this launch path; no routine window enumeration, foreground recovery or extra preflight is needed. HWND checks inside the sandbox cannot establish user-desktop visibility
- Do not preflight manually, prelaunch DuckStation, reconstruct Shell commands, attach to an existing emulator, or invoke a second recorder
- Interactive `user_trace` launches DuckStation directly through the logged-in Explorer `Shell.Application`; the hidden workflow worker never creates the emulator process. `--hidden` is test-only and forbidden here
- On `recording`, report that recording started and await `stop`; do not ask for routine visibility confirmation, including for older receipts carrying `visibility=unconfirmed` or a confirmation `user_action`. Do not claim to have visually inspected the window. If the user reports no window, treat that as a launch failure and diagnose it
- On explicit discard, identity-check worker/emulator, terminate only those PIDs, then remove only `NAME` trace/workflow/name-bound runtime artifacts. Do not run expensive stop/finalization. Do not delete shared logs or unrelated telemetry
- A successful start requires session `recording`, requested state hash, verified process/runtime/port ownership and growing raw data
- Before declaring a new or changed project trace profile/recorder ready for manual use, run a hidden end-to-end capture from a project-audited state that autonomously crosses every required boundary. PASS requires a successful footer and full decode, contiguous VBlank, paired actor/world and phase/GPU records, committed checkpoints, a validated stage-package index and owned-emulator cleanup. Static profile validation, an emulator build or raw growth alone is insufficient; store a compact project-local receipt binding the state, profile, patch, executable, raw trace and session hashes

### `stop`

Run only:

```powershell
X trace_workflow stop --wait 30
```

If still running, observe `X trace_workflow status NAME --operation stop --wait 30`. Do not invoke lower-level stop, kill a PID or restart decoding. Normal stop drains, decodes, verifies provenance/completeness and closes only the owned emulator. Explicit discard is the separate exceptional path above.

### `converge NAME`

Create the requested convergence goal, then first run:

```powershell
X trace_workflow converge NAME --wait 30
```

During an active evidence-grounded NEEDS_CODE loop, use the accelerated iteration entry point after updating C/H and the exact translation-ledger entry:

```powershell
X trace_workflow iterate NAME --wait 30
```

`iterate` runs a compile-only `code_refresh_fast` and a provisional supervisor replay. It intentionally defers formatting, source-index refresh, tests and database rebuilding while the replay still reports NEEDS_CODE. It does not own per-function progress updates: after every successfully decompiled function has an audited compilable body, exact ledger entry and passing refresh, run `X render_progress` before the next replay. A provisional MATCH is never accepted or published: the same worker automatically runs full `code_refresh`, `build_database`, supervisor preparation and a fresh replay before normal finalization, publication and retention cleanup. Any final-gate failure stops closed with its log. Use ordinary `converge` when no C/H or ledger input changed, and never describe a provisional replay as MATCH.

Debug builds are permitted during diagnosis, implementation, `code_refresh` and converge. They remain diagnostic artifacts even after MATCH. Before the final response that declares a task complete, run `X build_native --configuration Release`; the executable delivered in the configured native output path must be that Release build. A failed Release build leaves the task incomplete. When completion depends on runtime behavior rather than compilation alone, repeat the smallest relevant final smoke check against the Release executable; do not relabel Debug runtime evidence as Release evidence.

Native Win32 projects use `<SubSystem>Windows</SubSystem>` and the shared platform `WinMain`; never override the CRT entry point. `WinMain` forwards the CRT-provided `__argc` and `__argv` to `xport_main`, preserving audit and converge arguments. Normal game launches must not allocate a console window. Converge and trace workers redirect the native process's stdout/stderr to owned log files, so their diagnostics do not require the Console subsystem; preserve those redirected streams and do not replace log evidence with an interactive console.

The Win32 backend calls `timeBeginPeriod(1)` during timer initialization before any paced `Sleep`, pairs it with `timeEndPeriod(1)` at shutdown and uses `QueryPerformanceCounter` for elapsed-time measurements. Per-frame stdout/stderr output and forced flushes are forbidden in ordinary gameplay; bounded diagnostic modes may write owned logs when that output is part of their explicit evidence contract.

Game-specific immutable initialized data may be compiled as a project-owned byte array in a dedicated C module. Keep its reviewed source hash, guest ranges and generator in the project; never embed original executable code or use this mechanism for disc assets that belong under the project's canonical data path.

Observe only:

```powershell
X trace_workflow status NAME --operation converge --wait 30
```

While running, the compact status reports the current pipeline step, elapsed time and heartbeat age. Preparation and native capture also report stage/tick/phase plus their target bounds when available; capture progress comes from owned checkpoint contexts rather than scanning the full trace. Terminal status includes the same final snapshot under `last_progress` with `active=false`, so a stale progress receipt cannot look like a live worker.

The persistent supervisor owns prepare/build/convert/capture/compare/repair/publish/prune. At attention/NEEDS_CODE first use `X trace_report --name NAME --view diagnosis`; open only the linked fields needed to resolve the failure. The durable handoff is `status/stage-pipeline/workers/NAME/handoff.json`. Never start duplicate telemetry, workers or validation. A different active Codex thread cannot take over the current telemetry owner; it may observe workflow status.

With the preferred pre-replay validation mode, at MATCH read the returned compact completion summary, then report the result. If more detail is needed, use `X trace_report --name NAME --view completion` once. The durable receipt is `status/stage-pipeline/workers/NAME/completion-summary.json`. Do not restart workers, run tests, scan validation identities, rehash recordings, enumerate run directories or edit acceptance manifests manually. Publication, mandatory retention cleanup and hashed completion evidence are worker-owned. After the summary is published, cleanup removes the matched raw trace, periodic trace save states, decoded trace payload, prepared cache and owned conversion while preserving the recording metadata, MATCH/completion/retention receipts, databases and user save slots. Failed, mismatched, unpublished, active and identity-ambiguous traces remain intact. Report a nonempty `reporting_attention` separately: the immutable comparison verdict remains valid, but completion reporting must not be described as fully successful.

Prefer `stage_pipeline.repair_supervisor.validate_before_replay=true` and `validate_on_match=false`. At worker preparation the supervisor validates changed inputs before replay. An unchanged passed validation receipt is reused; otherwise each suite may reuse its passed result only with matching dependency identity and retained log hash. Unknown imports, dynamic loading and subprocess dependencies conservatively use the complete declared identity, so suite reuse is not guaranteed. Failed checks stop before replay. Legacy projects may opt into after-MATCH validation; never enable both modes. A changed native EXE still requires full replay and C/H patches still require code_refresh. Pre-replay validation records the inputs at its execution time; later gameplay repairs are covered by code_refresh and fresh replay, not by relabeling the earlier validation receipt as current.

The telemetry watcher produces one provisional MATCH snapshot and closes final totals after the response, without keeping the converge worker alive. Link `status/telemetry/converge/NAME.summary.json`; do not spend model turns refreshing totals. MATCH certifies its recorded comparison scope, not solver/telemetry correctness.

Normal output budget is 10 KB of compact JSON. Use `X trace_report --name NAME --view VIEW`, choosing one of `completion`, `diagnosis`, `recording`, `validation`, or use `trace_agent evidence` with an exact JSON pointer and prior SHA. The validation view reads the recorded result without a live identity scan. Full identity maps, states[], all telemetry JSON and broad git/source dumps are not normal diagnostics. Expand only the evidence needed for an explicit unresolved question; static candidates are not causal proof.

### Deferred toolset acceptance

The cost-v3 implementation is syntax-reviewed only; its functional tests and measurements are deferred to the next authorized new `record -> stop -> converge`. Do not run an extra test, solver probe, build or replay now merely to complete documentation or acceptance fields. During that next cycle, configured validation runs before replay. CLI startup is checked by the supervisor; a real defect is needed to exercise structured solver reasoning and automatic repair. Missing real-path evidence remains `not_exercised`, not PASS.

For projects adopting cost-v3, track the cycle in their acceptance manifest (Fighting Force: `status/toolset-validation/converge-cost-v3-acceptance.json`). Initial advisory targets are at most 180 post-MATCH seconds, 5 post-MATCH model responses and zero accidental output truncations. These targets never relax complete comparison, controller transfer retention, publication or pruning. Further large-file I/O changes require the deferred profiling evidence; no speedup is established by syntax checks.

## Recording contract

- Names: 1–40 ASCII letters/digits/`_`/`-`; slot: 1–10; duration: 1–600 seconds
- User capture uses the configured `user` role, isolated data directory and reserved port. Recording names are immutable identities
- Normalize interactive speed to 1×, disable turbo/rewind/runahead/overclock/VSync, enable fast boot, preserve controller bindings. Hidden tests use unlimited speed and `-batch`
- Capture original state hash, EXE/config/profile/collector hashes, effective settings, process identity, runtime and actual argv/environment
- VBlank RAM uses 4 KiB pages plus 1 KiB scratchpad. Modes 2/3 are content-addressed; mode 3 keyframes are 513 references every 600 VBlanks. Legacy modes 0/1 remain readable
- Deduplication covers VBlank RAM payload only. Periodic DuckStation `.sav` files retain full device state and are measured separately
- After stop obtain aggregates with `X trace_report --name NAME --view recording`: raw bytes, `.sav` bytes, `vblank_storage` definitions/references/keyframes/payload, gaps, controller transfers, capture latency and startup timing. Do not dump session.states or rehash the raw recording for the report. Sizes are observations; stop's recorded provenance verification is separate integrity evidence. Do not claim total savings from RAM dedup alone
- Controller byte transfers and decoded input calls are distinct evidence. Preserve all transfers
- Preserve boundary PAD packets and decode-time requested values as separate replay schedules; a controller poll may occur between callback entry and the original button load
- Gameplay ticks may repeat or advance sparsely within one stage. Split continuation segments only on an audited stage change or a decreasing tick reset; preserve observed decode-time PAD values at their exact ticks and fail on conflicting values for one tick
- `gpu_phase_mask` may restrict ordering-table traversal to audited render-complete phase PCs; bits `1/2/4/8` select `game_begin/menu_phase/sequence_phase/aux_phase`. Every excluded phase still emits a paired empty GPU record; enabled phases remain fail-closed on an invalid table. Never use the mask to hide a boundary that actually owns GPU work
- `gpu_full_ram=1` permits an audited title whose OT links multiple static and dynamic RAM regions; it still rejects unaligned/out-of-RAM addresses, oversized packets, cycles and missing terminators. Omission preserves the stricter configured OT/pool ranges

## Replay and MATCH

- Use captured stage packages only at project-audited continuation boundaries. Validate package/index/profile/raw hashes, PC/NPC, stage/tick, phase ordinal, input cursor and native ABI
- Stage transitions reset game ticks. Order by monotonic phase ordinal and retain stage/tick labels
- Compare the complete declared interval and every configured channel: phase, actors, world, input/calls, GPU and sound. Apply only documented exclusions and report them
- Narrow replay, prefix comparison, checkpoints and causal traces are diagnostic. They never replace full fresh-build comparison
- A changed native EXE requires fresh continuation comparison. A serialized layout change requires an ABI bump and new checkpoint
- MATCH requires full comparison, publication/progress success and retention cleanup. Replace successful heavy run/recording payloads with compact hashed receipts; retain failed, mismatched, unpublished, active and identity-ambiguous evidence
- Unsupported anchor, incomplete channel, changed evidence, build failure, publication failure or ambiguous ownership is not MATCH

## MIPS audit and C changes

All project `.c`/`.h` code uses the single shared style `[XPORT_ROOT]/src/.clang-format`. Do not infer formatting from nearby code, use an editor-specific style, or add a project-local override. `X code_refresh` automatically runs `clang-format -i --style=file:[XPORT_ROOT]/src/.clang-format` on every changed existing `.c`/`.h` before refreshing source hashes and the index; the agent does not run a separate formatter. A changed shared style formats the complete configured source inventory once. `X build_database` formats the complete source inventory before import. Manual whole-tree formatting is allowed only for an explicit baseline migration because it invalidates source-bound evidence. Never format IDA/MIPS exports or evidence artifacts.

The canonical host runtime is compiled directly from `[XPORT_ROOT]/src`; projects must not copy or fork its Xport types, PsyQ ABI, GTE, SPU core or host backends. Game code includes the shared `[XPORT_ROOT]/src/xport.h`, never a project copy: it is the only Xport public header and owns `uint8/sint8`, `uint16/sint16`, `uint32/sint32`, `uint64/sint64`, `intptr`, the exact PsyQ PAD constants and aliases, `FUNCTION_MARKER`, function `GDB_CALL` and data `GDB_DATA` exports, the platform service contract and the required game bindings. Do not create auxiliary `xport_*.h` contract headers. Use these Xport names for translated game state, guest addresses/registers, serialized fields and public game/runtime interfaces; do not introduce `<stdint.h>` names or another project type header for those values. Plain C `int` remains appropriate for host-only status, loop and API values whose width is not part of the guest ABI. Every native build configuration must define project-specific `WND_TITLE`, `WND_WIDTH`, `WND_HEIGHT` and `FIELD_RATE`; the shared runtime deliberately provides no fallbacks and a missing definition is a compile error. `project_init.py` creates `WND_TITLE` from the full project name and starts new projects at `960`, `768` and `50`. The runtime is an intentionally bidirectional framework rather than a standalone library: games call the shared PSX/PsyQ surface, and shared subsystems call required game-owned `xport_*` symbols declared by `xport.h`; missing bindings fail at link time. The selected `platform/<name>/main.c` owns the native process entrypoint, implements the `xport_*` window/input/timer/audio/file services and calls the game's required `xport_main`; `psx.c` must remain free of operating-system calls and callback registration. Original PsyQ names remain the public compatibility API; internal GPU, SPU and GTE identifiers use the single `gpu_*`, `spu_*` and `gte_*` namespaces, while platform and game bindings use `xport_*`. The shared GTE exposes the exact PsyQ 4.6 `LIBGTE.H` names `ccos` and `csin`; their 32-bit `int` ABI is represented as `sint32`, including parameters. Never rename these public entrypoints or let game calls resolve to the incompatible C runtime complex-math symbols. `PadInitDirect` buffers use the libpad layout `00 41 low high` for a connected digital pad and eight `FF` bytes for a disconnected controller; the button word is active-low and no serial-response `5A` byte belongs in this memory layout. `psx_gpu.c` is the sole owner of the PsyQ graph, ordering-table, VRAM and draw/display-environment entrypoints; projects must not define `XPORT_GPU_EXTERNAL_CONTROL` or provide title-local implementations. Polygon commands rasterize only their covered scanline spans; a bounding-box pixel traversal is forbidden. Native builds compile `psx_gpu.c` with optimization enabled, including Debug configurations; when MSVC rejects optimization with basic runtime checks, disable those checks only for this translation unit. `PutDispEnv` only selects the display rectangle, matching PsyQ state semantics. Display offsets translate that selected rectangle inside the host output; exposed pixels are black and the converter never reads outside the selected rectangle into an adjacent VRAM framebuffer. A game calls `gpu_present` once after the logical frame's final `DrawOTag`; presenting from `PutDispEnv` or between view ordering tables exposes an incomplete VRAM page and is forbidden. `ClearOTag`, `ClearOTagR`, `AddPrim`, `AddPrims`, `LoadImagePSX` and `StoreImage` consume native C pointers directly. Native ordering tables retain 24-bit links and require the OT plus its linked primitive pool to remain within one 16-MiB segment; `DrawOTag` preserves that segment while traversing links. Shared `psx.c` owns `DRAM`, `SCRATCHPAD` and `psx_addr`; projects must not define parallel PSX memory buffers or address resolvers. Guest-address uploads use `gpu_load_rect` and `psx_addr`. Do not retain parallel `psx_gpu_*`, `psx_spu_*`, `PsyQ*` or other compatibility layers with duplicate state or forwarding calls. Keep game RAM addresses, title-specific display parameters, WIP policy and adapters in the project. A shared runtime change requires toolset validation and `code_refresh` in every project migrated by that change; it does not transfer gameplay ledger status between projects.

State accessed by both the platform audio render thread and the game thread is guarded inside the shared SPU runtime by `xport_audio_lock` and `xport_audio_unlock`. The selected platform `main.c` owns the synchronization primitive; game code must not include an operating-system synchronization API or implement an audio render callback. `SPU_STATE` is the only SPU register and mixer state. Do not add `VOICE_00_LEFT_RIGHT`, another register mirror or render-time state copying. The public sound ABI uses the exact PsyQ 4.6 `LIBSPU.H`, `LIBSND.H` and relevant `LIBCD.H` names: `SpuVolume`, `SpuVoiceAttr`, `SpuCommonAttr`, `SPU_VOICE_*`, `SPU_COMMON_*`, `Spu*`, `Ss*` and `Cd*`. Project code must not publish alternate SPU/VAB structures, forwarding wrappers or calls to internal `spu_*` functions. Audio rendering uses a zeroed interleaved `sint32` accumulation buffer. `spu_mix` and every additional source add to that buffer. Voice and Gaussian products remain `sint32` only while their bounds are proven against the 24-voice limit. Normalize master volume to `0..256`, apply it with a 32-bit multiply and shift by 8. Clamp exactly once when converting the completed mix to interleaved `sint16`; intermediate 16-bit mixing and repeated clamping are forbidden.

The selected platform audio worker calls the internal shared `spu_render(sint16 *stereo, uint32 frames)` entrypoint. Games never define or call `audio_runtime_render`, `xport_audio_render`, an IMA decoder, a VAB registry or an audio mixer. `psx_spu.c` owns VAB parsing/transfer, libsnd voice allocation, 24-voice SPU synthesis, CD input and the final mix. Red Book tracks are stereo 44.1-kHz IMA ADPCM WAV files named by decimal track number under `MUSIC` relative to the native working directory; native projects run from `bin`, so the project asset path is `bin/MUSIC`. No `DATA/MUSIC` fallback is permitted. `CdPlay` loads and starts a track; `CdControl` owns play, pause, stop, mute and demute; `SsSetSerialVol(SS_SERIAL_A, ...)` owns CD input volume. Track decoding and lifetime remain invisible to game code. Host runtime switches use the fixed names `XPORT_AUDIO_OUTPUT`, `XPORT_AUDIO_BACKEND`, `XPORT_RASTERIZE` and `XPORT_VERIFY_VRAM`; do not add game-owned environment-name externs.

Before editing a function:

```powershell
X query 0xADDRESS --image IMAGE --audit-context
```

Use indexed source/SQL context first. Refresh only derived source mapping after C/H changes unless importer inputs changed.

Required reasoning:

- Decode original instructions and delay slots; track register widths, signed/unsigned comparisons, load/store sizes, address formation and calling convention
- For direct first-pass IDA pseudocode translation, use the shared lvalue macros `byte_(0xADDRESS)`, `word_(0xADDRESS)` and `dword_(0xADDRESS)`. They route complete guest addresses through `psx_addr`, support both reads and writes, and cover the IDA `byte_`, `word_` and `dword_` global forms. Preserve signed interpretation with explicit `sint8`, `sint16` or `sint32` casts. Never add a complete guest address directly to `DRAM`
- Treat every immediate as encoded data, not a pseudocode literal. Verify sign extension and split `lui`/low-half construction
- Distinguish function pointers, data pointers, tables and sentinels. Static references/callback candidates are navigation, not causal proof
- Preserve switch/default behavior, stage-indexed strides and state-transition ordering. Do not generalize stage-0 constants
- Use existing implementation only after source-index identity is unique. Ambiguous/combined ledger regions remain unmapped
- Amortize validation for short connected dependency chains without weakening per-function audit. Run `query --audit-context` and inspect immutable MIPS/pseudocode separately for every address, then place roughly 3-5 small, low-risk functions in one reviewed C/H patch, run one mandatory `code_refresh`, update every ledger entry with its own identity/evidence, and run the required post-ledger refresh/database step once at the batch boundary. Keep large, recursive, ABI-sensitive, ambiguous or independently failing functions in separate batches. A batch failure is diagnosed before adding more functions; batching never permits ledger-only promotion or shared evidence notes
- Keep first-pass uncategorized translation modules bounded. Put newly reached functions in stable numbered files such as `race_leaf_batch_000.c` with a matching header; start the next batch before the current file exceeds either 2,000 source lines or 100 translated functions. A single function may exceed the line limit but then owns its own module. Add each batch to the native build when it is created and do not keep appending to an existing oversized catch-all. Prefer adding a new batch over repeatedly moving already indexed functions; later semantic consolidation remains a separate evidence-preserving phase. This rule bounds formatting, source indexing, compilation context and incremental-link churn without reducing branch closure or per-function evidence

After any C/H patch run:

```powershell
X code_refresh
```

It applies the shared `.clang-format`, rebuilds source index, builds, runs focused tests, refreshes audit packets, validates SQLite and renders progress. A C/H change is not complete until this command returns PASS. Inspect full logs only on concise failure. Full replay remains mandatory.

## Declarative repair supervisor

- `repair_bench`: common restricted MIPS↔C stand; new supported cases are schema contracts, never new ad-hoc Python/C harnesses
- Production instruction bytes are pinned to SQL/original images; native bodies are pinned through source index
- `repair_dispatch`: only deterministic `callback_binding` and `state_transition`. Require reproduced baseline failure, resolved original call/arguments and passing candidate bench before applying
- Fixtures never modify gameplay C. Unknown semantics go to causal observations or isolated solver
- Solver is read-only and returns structured contract proposals/missing observations; it never edits gameplay C. Persist prompt/schema/command/process/usage/output. Propagate the caller's existing CODEX_HOME to the worker and CLI; never copy credentials. Pre-replay probes temporary create/remove access in that existing home and stops before replay with `home_readonly` when the worker context cannot write it; a cached --version preflight then verifies startup only. The first real defect must verify structured reasoning. Infrastructure errors include bounded stderr and suppress repeated calls for the same runtime identity
- If CLI startup or home/auth/argument handling fails, use the recorded reason and stderr excerpt; do not repeat the same solver invocation on subsequent defects. After an actual external runtime repair, solver.runtime_revision can explicitly invalidate that runtime fingerprint; the failed launch is preserved and only one budgeted retry is allowed under the changed identity. Launch ownership and max_calls still apply
- `repair_causal` localizes bounded writers/dependencies. It does not assume stage-entry RAM persists to a later mismatch
- `repair_codegen` evaluates only the configured restricted integer-function subset; it neither generates nor approves general gameplay C
- Supervisor repair still requires `code_refresh`, a later verified prefix, full fresh-build replay and ordinary MATCH gates
- Validation-owned temporary artifacts must release hash-session leases before Windows cleanup; passing unit suites alone is not validation completion
- Codex Desktop converge telemetry binds to the explicit current thread/session identifier and fails closed when that transcript is unavailable. An explicit matching transcript may own a project at or below its workspace root; filesystem recency remains an exact-project fallback only for hosts that expose no identifier
- Validation-only supervisor resumes preserve the original finalized MATCH timestamp; telemetry must not reinterpret validation restart time as a new MATCH
- Configured acceptance manifests receive linked runtime_cycles evidence automatically; reviewed historical gates are not overwritten or promoted. Unexercised solver/automatic-repair/performance paths remain explicit. Full savestate bytes and VBlank payload metrics stay separate; worker SW_HIDE does not establish emulator visibility

Read `[XPORT_ROOT]/tools/REPAIR_CONTRACT.md` before changing contracts, fixtures, dispatch transforms, solver or codegen.

## Database, ledger and lifecycle

- If IDA/importer/classification inputs changed: `X build_database`. Validate SQL before dependent work
- A retained legacy export may be converted once with `X legacy_ida_import --legacy PATH --image IMAGE --output status/ida/images/IMAGE`, then verified with `X ida_canonicalize`. The converter requires per-function bytes and hashes, refuses a nonempty output, preserves missing xref sites as null and never imports completion status. Keep the source export immutable and record the conversion limitation in its report
- Otherwise C/H changes use `X source_index --rebuild` or `X code_refresh`; do not rebuild historical evidence unnecessarily
- TODO → WIP only when scope/address/image/ownership are explicit and a real compilable C body exists. Empty stubs, invented success returns, generic interpreter wrappers and ledger-only promotion are forbidden. DONE only with exact implementation identity, evidence, build/tests, original/native comparison where applicable, ledger update and progress render
- If convergence identifies a causal TODO function, do not patch only that leaf and immediately replay. Form one image-qualified TODO branch from the root, its SCC and every transitively resolved direct TODO callee until WIP/DONE/SKIP boundaries; include trace-proven callback targets as additional roots. Translate the branch dependency-first in one reviewed batch or consecutive bounded batches before replay. List unresolved indirect targets and overlay-context ambiguity as gaps; never guess edges or expand into unrelated callers/code merely to reduce the global TODO count
- Similarity, decompiler text, compilation and synthetic tests are supporting evidence only
- After every successfully decompiled function has an audited compilable body, exact ledger entry and passing `code_refresh_fast` or `code_refresh`, run `X render_progress` before replaying or reporting it. Do not batch this progress update across multiple successful functions. Successful converge already publishes progress before MATCH; do not render it again as routine post-MATCH housekeeping

## Late semantic completion

- Begin subsystem-wide semantic naming and structure consolidation only after the published coverage and closure inventory are mature enough to expose all major readers, writers, callbacks and lifecycles; approximately 95% is a planning threshold, not a completion waiver
- Preserve a bidirectional image-qualified mapping from original address/name to semantic function, global, type and field names. New projects use `FUNCTION_MARKER(0xADDRESSu, "IMAGE")` from the shared `[XPORT_ROOT]/src/xport.h`; `FF_FUNCTION_MARKER` remains accepted only for legacy Fighting Force sources. Keep original identity in C markers/comments and in the ledger after files are reorganized. `source_index` accepts an immediately preceding `/* Original: NAME_800ABCDE. */` only when the ledger carries the same original name and address; an address-only comment is not a cross-image identity. It may also bind the exact ledger `semantic_name` only inside the ledger-declared source file and only when that function name is unique there
- Accept a name or structure field only from combined call/data xrefs, access width and signedness, lifecycle and runtime evidence. Revert to a neutral identifier when later evidence makes the meaning ambiguous
- Rename and reorganize in bounded subsystem batches. Each batch requires the same source-index, build, static-evidence and replay gates as behavioral changes; readability never transfers DONE between functions or overlays
- Existing projects which reached this phase before adopting the shared pipeline retain their names and layouts as historical hypotheses. Import them into the mapping/ledger without auto-promoting legacy completion claims, then revalidate them under current evidence gates
- `paths.semantic_map` selects schema-1 JSON imported into `semantic_aliases` and `semantic_structures`. A bound function alias requires matching image, address and original-byte SHA. An alias for an overlay not yet inventoried must use `pending_image_inventory`; it is searchable but cannot affect function status. Legacy structure definitions enter as `legacy_hypothesis` with source location and definition hash until size, fields and evidence are reviewed
- For a legacy project use `X legacy_semantic_import --progress PATH --functions CANONICAL_FUNCTIONS --image IMAGE`. The command imports address-qualified names, creates WIP/SKIP ledger entries only for unique bound C sources, inventories retained C structure definitions and records unresolved image identities. It never emits DONE

## Diagnostics and routing

Use these only after the canonical workflow returns attention or a linked handoff requests them:

- `X trace_agent card|handoff|changes --name NAME`: compact state/evidence/bounded diff
- `X trace_audit_packet ...`: first causal MIPS site and bounded context
- `X trace_stage_run --name NAME --status --wait 30`: runner diagnosis, not normal entry
- `X trace_stage_narrow --run RUN_DIRECTORY --target PHASE --wait 30`: bounded diagnostic replay
- `X trace_report --name NAME --view diagnosis`: bounded first-difference context and linked evidence
- `X trace_report --name NAME --view validation`: recorded validation status without reruns or identity scans
- `X converge_telemetry snapshot --name NAME`: explicit accounting diagnosis only; the normal watcher owns MATCH/final snapshots
- `X trace_validation --name NAME --status`: explicit validation diagnosis including live identity comparison; not routine post-MATCH work

Open at most the routed document needed for the current unresolved topic:

- `docs/UPGRADING.md`: bounded changelog upgrades, automatic migrations, goal handoff and revision-marker rules
- `docs/PROJECT_SETUP.md`: required new-project bootstrap, user-chosen short name, directory ownership, VS2022 scaffold, IDA export/accept, PsyQ SKIP recognition, SQL/evidence contracts and TODO → WIP planning
- `docs/DUCKSTATION.md`: emulator deployment, profiles, manual observation and paired checkpoints
- `docs/RECORDING.md`: user recording, replay preparation, input and anchor lessons
- `docs/DECOMPILATION.md`: WIP → DONE, command catalog, retired compatibility and PSX/MIPS/IDA tips
- `docs/REPLAY.md`: acceleration, stage packages, adapters, registry, verification and portability
- `docs/RECOVERY.md`: acceptance, archives, interrupted workers, cleanup and pruning
- `docs/CONVERGENCE.md`: converge diagnostics, bounded context, validation, repair, caches and telemetry

The compact contract above is sufficient for a fresh `record → stop → converge` chat.

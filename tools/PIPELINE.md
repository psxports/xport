# PSX xport agent contract

Mandatory compact contract. Project `AGENTS.md` supplies game facts and overrides project-specific defaults. `[XPORT_ROOT]` is resolved from `xport-project.json`; define:

`X = python -B [XPORT_ROOT]/tools/xport.py --project PROJECT_ROOT`

Do not read `docs/*.md` at task start. Open only the one file this contract routes for the current operation or a lower-level failure. `REPAIR_CONTRACT.md` is required only for repair-contract/solver work.

## Invariants

- Original images, exports, traces and evidence are immutable inputs. C is a tested translation, never authority
- IDA pseudocode is navigation only. Never translate it directly to C. Derive immediates, signedness, delay slots, ABI, branches, calls and memory effects from original MIPS bytes/SQL and validate with the MIPS↔C bench or replay
- Status authority: project ledger plus `status/analysis.sqlite`. Do not infer DONE from source presence, compilation, similarity, a fixture or a narrow replay
- Preserve unrelated user changes. Shared behavior belongs in `[XPORT_ROOT]/tools`; game facts belong in project `AGENTS.md`
- Fail closed on missing/changed/ambiguous evidence, ownership, mapping, continuation or comparison scope. Never weaken checks to obtain MATCH
- Use compact receipts/handoffs. Do not load old chats, full traces, whole logs, full DB schemas or broad source trees unless a stated unresolved question requires them

## Toolset bootstrap

- A fresh xport checkout prepares the shared modified emulator with `[XPORT_ROOT]/tools/prepare.bat`. It checks the pinned manifest and patch, obtains the exact DuckStation commit and official dependency archive, verifies hashes, builds VS2022 Release x64 v143, validates xport API markers and atomically publishes `tools/duckstation/distribution`
- Commit `duckstation-manifest.json`, `internal-trace-api.patch`, bootstrap scripts and documentation. Never commit generated DuckStation source, expanded dependencies, build workspace or distribution. The default cache/workspace is outside the repository under the current user's local application data
- Apply only `internal-trace-api.patch`; it includes the historical state API patch. On an upgrade, change the pinned upstream/dependency identities, regenerate the complete patch from that clean commit, update hashes and prove a clean `prepare.bat` build before use
- Close DuckStation after every check. `prepare.bat` never kills an emulator and fails before publication if the target executable is running. It preserves the previous distribution on build or validation failure
- See `duckstation/SOURCES.md` for switches, provenance, licensing and packaging details

## Exact user commands

Treat these natural phrases as workflow commands, not shell text.

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

Observe only:

```powershell
X trace_workflow status NAME --operation converge --wait 30
```

The persistent supervisor owns prepare/build/convert/capture/compare/repair/publish/prune. At attention/NEEDS_CODE first use `X trace_report --name NAME --view diagnosis`; open only the linked fields needed to resolve the failure. The durable handoff is `status/stage-pipeline/workers/NAME/handoff.json`. Never start duplicate telemetry, workers or validation. A different active Codex thread cannot take over the current telemetry owner; it may observe workflow status.

With the preferred pre-replay validation mode, at MATCH read the returned compact completion summary, then report the result. If more detail is needed, use `X trace_report --name NAME --view completion` once. The durable receipt is `status/stage-pipeline/workers/NAME/completion-summary.json`. Do not restart workers, run tests, scan validation identities, rehash recordings, enumerate run directories or edit acceptance manifests manually. Publication, mandatory pruning and hashed completion evidence are worker-owned. Report a nonempty `reporting_attention` separately: the immutable comparison verdict remains valid, but completion reporting must not be described as fully successful.

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

## Replay and MATCH

- Use captured stage packages only at project-audited continuation boundaries. Validate package/index/profile/raw hashes, PC/NPC, stage/tick, phase ordinal, input cursor and native ABI
- Stage transitions reset game ticks. Order by monotonic phase ordinal and retain stage/tick labels
- Compare the complete declared interval and every configured channel: phase, actors, world, input/calls, GPU and sound. Apply only documented exclusions and report them
- Narrow replay, prefix comparison, checkpoints and causal traces are diagnostic. They never replace full fresh-build comparison
- A changed native EXE requires fresh continuation comparison. A serialized layout change requires an ABI bump and new checkpoint
- MATCH requires full comparison, publication/progress success and configured pruning. Preserve original recording; successful heavy run directories may be replaced by compact receipts when project policy requires it
- Unsupported anchor, incomplete channel, changed evidence, build failure, publication failure or ambiguous ownership is not MATCH

## MIPS audit and C changes

All project `.c`/`.h` code uses the single shared style `[XPORT_ROOT]/src/.clang-format`. Do not infer formatting from nearby code, use an editor-specific style, or add a project-local override. `X code_refresh` automatically runs `clang-format -i --style=file:[XPORT_ROOT]/src/.clang-format` on every changed existing `.c`/`.h` before refreshing source hashes and the index; the agent does not run a separate formatter. A changed shared style formats the complete configured source inventory once. `X build_database` formats the complete source inventory before import. Manual whole-tree formatting is allowed only for an explicit baseline migration because it invalidates source-bound evidence. Never format IDA/MIPS exports or evidence artifacts.

Before editing a function:

```powershell
X query 0xADDRESS --image IMAGE --audit-context
```

Use indexed source/SQL context first. Refresh only derived source mapping after C/H changes unless importer inputs changed.

Required reasoning:

- Decode original instructions and delay slots; track register widths, signed/unsigned comparisons, load/store sizes, address formation and calling convention
- Treat every immediate as encoded data, not a pseudocode literal. Verify sign extension and split `lui`/low-half construction
- Distinguish function pointers, data pointers, tables and sentinels. Static references/callback candidates are navigation, not causal proof
- Preserve switch/default behavior, stage-indexed strides and state-transition ordering. Do not generalize stage-0 constants
- Use existing implementation only after source-index identity is unique. Ambiguous/combined ledger regions remain unmapped

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
- Solver is read-only and returns structured contract proposals/missing observations; it never edits gameplay C. Persist prompt/schema/command/process/usage/output. Propagate the caller's existing CODEX_HOME to the worker and CLI; never copy credentials. A cached --version preflight verifies startup only. The first real defect must verify structured reasoning. Infrastructure errors include bounded stderr and suppress repeated calls for the same runtime identity
- If CLI startup or home/auth/argument handling fails, use the recorded reason and stderr excerpt; do not repeat the same solver invocation on subsequent defects. After an actual external runtime repair, solver.runtime_revision can explicitly invalidate that runtime fingerprint; launch ownership and max_calls still apply
- `repair_causal` localizes bounded writers/dependencies. It does not assume stage-entry RAM persists to a later mismatch
- `repair_codegen` evaluates only the configured restricted integer-function subset; it neither generates nor approves general gameplay C
- Supervisor repair still requires `code_refresh`, a later verified prefix, full fresh-build replay and ordinary MATCH gates
- Validation-owned temporary artifacts must release hash-session leases before Windows cleanup; passing unit suites alone is not validation completion
- Codex Desktop converge telemetry binds to the explicit current thread/session identifier and fails closed when that transcript is unavailable; filesystem recency is only a fallback for hosts that expose no identifier
- Validation-only supervisor resumes preserve the original finalized MATCH timestamp; telemetry must not reinterpret validation restart time as a new MATCH
- Configured acceptance manifests receive linked runtime_cycles evidence automatically; reviewed historical gates are not overwritten or promoted. Unexercised solver/automatic-repair/performance paths remain explicit. Full savestate bytes and VBlank payload metrics stay separate; worker SW_HIDE does not establish emulator visibility

Read `[XPORT_ROOT]/tools/REPAIR_CONTRACT.md` before changing contracts, fixtures, dispatch transforms, solver or codegen.

## Database, ledger and lifecycle

- If IDA/importer/classification inputs changed: `X build_database`. Validate SQL before dependent work
- Otherwise C/H changes use `X source_index --rebuild` or `X code_refresh`; do not rebuild historical evidence unnecessarily
- TODO → WIP only when scope/address/image/ownership are explicit. DONE only with exact implementation identity, evidence, build/tests, original/native comparison where applicable, ledger update and progress render
- Similarity, decompiler text, compilation and synthetic tests are supporting evidence only
- Use `X render_progress` before intermediate reports and after accepted status changes. Successful converge already publishes progress before MATCH; do not render it again as routine post-MATCH housekeeping

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

- `docs/PROJECT_SETUP.md`: ownership, project layout, IDA export/import, PsyQ recognition, SQL/evidence contracts and TODO → WIP planning
- `docs/DUCKSTATION.md`: emulator deployment, profiles, manual observation and paired checkpoints
- `docs/RECORDING.md`: user recording, replay preparation, input and anchor lessons
- `docs/DECOMPILATION.md`: WIP → DONE, command catalog, retired compatibility and PSX/MIPS/IDA tips
- `docs/REPLAY.md`: acceleration, stage packages, adapters, registry, verification and portability
- `docs/RECOVERY.md`: acceptance, archives, interrupted workers, cleanup and pruning
- `docs/CONVERGENCE.md`: converge diagnostics, bounded context, validation, repair, caches and telemetry

The compact contract above is sufficient for a fresh `record → stop → converge` chat.

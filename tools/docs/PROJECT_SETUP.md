# Project setup, analysis and planning

Normative extension of `../PIPELINE.md`. Read this file only when that contract routes the current task here.

## 0. Ownership and change policy

### Fresh-task converge entry

- Read the consuming project's AGENTS.md and this file; resolve `X` as `python -B [XPORT_ROOT]/tools/xport.py --project PROJECT_ROOT`. Do not fetch old chats or full transcripts by default
- `X trace_agent card --name NAME` lists supported commands. For an existing attempt first read `X trace_agent handoff --name NAME`; it contains state, evidence, build/trace identities, first difference and commands
- Single-call entry: `X trace_workflow converge NAME --wait 30`. It starts/reuses request telemetry and enters the existing persistent worker. Observe with `X trace_workflow status NAME --operation converge --wait 30`; do not manually repeat preflights or accounting starts. The lower-level trace_stage_run/converge_telemetry commands remain for diagnosis
- At NEEDS_CODE read the first difference before the terminal failure. Fetch only the linked diagnostic/audit block; fix the causal MIPS defect, then repeat run/repair. No manual capture configuration or input cursor calculation is required
- Read the per-channel context in the same handoff: an earlier world difference can explain a camera mismatch. Batch independent evidence reads; each additional query should answer a stated unresolved hypothesis. Do not repeatedly discover schemas or open whole files when a linked field/excerpt suffices
- `X trace_agent changes --name NAME` shows a bounded diff against the worker's source baseline for this recording, not repository history. The baseline is context only, never correctness evidence
- At MATCH read the receipt/finalization and one `X converge_telemetry snapshot --name NAME`. Final token totals are closed by the post-response watcher; use `status/telemetry/converge/NAME.summary.json`, not the full per-response array. Do not repeat unchanged successful checks
- When an optimization-validation cycle is explicitly authorized, execute trace_validation exactly once. If `stage_pipeline.repair_supervisor.validate_on_match` is enabled, the worker owns that execution after MATCH and the agent only reads `X trace_validation --name NAME --status`; otherwise run `--run` once. Ordinary converge does not implicitly authorize extra suites, failure injection or another project. A user deferral overrides this step
- If the project enables `stage_pipeline.repair_supervisor`, the same persistent worker handles supported deterministic repairs and read-only solver escalation before NEEDS_CODE. Read its linked repair evidence, not a new agent-written harness. If `validate_on_match` is explicitly authorized/configured, that worker performs the single deferred validation; use `--status` instead of launching it again. Contract format, commands and trust boundaries: [REPAIR_CONTRACT.md](../REPAIR_CONTRACT.md)


- One maintained toolset and DuckStation installation: `[XPORT_ROOT]/tools`. Never copy shared Python/C++/DLL/resource implementations into a game project. Thin relative launchers/import shims are allowed; they contain no implementation.
- Shared improvements belong here and must work with unrelated games. Parameterize game addresses, image names, structures, ABI hooks, exclusions and paths; do not add game-name conditionals. Test a distinct project/profile and the affected established workflow.
- Update this document with each shared behavior/format/command change. Agents of every project may improve it. Preserve validated constraints and provenance; replace obsolete instructions instead of appending contradictory history. Keep project-only findings in that project's `AGENTS.md` and evidence files.
- No fixed final methodology: promote a verified reusable finding into this document/toolset. State applicability, test evidence and limitations. Do not promote a game's workaround into a platform rule.
- Preserve licenses, upstream revision, patches, build inputs and uncommitted user work. Shared DuckStation is a locally modified build, not an upstream Save State/trace API promise.
- Generated reports, traces, images, fixtures, states, databases and emulator build snapshots belong in the consuming project's `status` or `tools`. Shared source contains implementation/docs/templates/distribution only.
- Before launching/building/stopping check live process identity. Never interrupt manual play. A timeout means observe the same job/process; do not launch a replacement. Verify PID, creation time, executable, endpoint and project data directory; a shared EXE path or stale PID file alone is insufficient.
- After every completed or failed check, close emulator instances started by the agent for that check. Verify current PID/creation time, executable, project data and endpoint ownership before stopping; use the owned stop command in cleanup/finally. Preserve pre-existing user sessions and active recordings. Leave a test instance running only when explicitly requested by the user; report that handoff. An observation timeout alone does not end the check.
- Full debugger output and traces go to files. Agent output: short result, first difference, evidence path. Buffer trace writes; do not rewrite a growing JSON per frame or load whole RAM dumps into context.

## 1. Create and configure a project

Before running any initializer, ask the user for both the human-readable project name and an explicit short native name. Never derive, abbreviate or silently normalize the short name. It must start with an ASCII letter, contain only ASCII letters, digits or underscores and be at most 16 characters. The exact value names the solution, project and executable; for example full name `Fighting Force`, short name `FF` produces `FF.sln`, `FF.vcxproj` and `FF.exe`.

Run the single startup supervisor from the shared toolset. It allocates sibling-aware ports, creates the scaffold, runs diagnosis/build, and continues through all startup gates:

```powershell
python -B [XPORT_ROOT]/tools/startup.py start --project [PROJECT_ROOT] --name "[FULL PROJECT NAME]" --short-name [SHORT]
```

Initialization refuses every file it would overwrite, locks allocation, checks sibling reservations and live listeners, assigns distinct audit/trace/user GDB ports and creates a compilable VS2022 v143 `Debug|Win32` C console skeleton. Every generated native build configuration defines the project-specific `WND_TITLE="[FULL PROJECT NAME] (xport)"`, `WND_WIDTH=960`, `WND_HEIGHT=768` and `FIELD_RATE=50`; all four definitions are mandatory because the shared runtime has no fallbacks. The initializer also creates a project `AGENTS.md` routing stub, the empty translation ledger and configures `code_refresh` to build the short-name solution. Fill the stub with reviewed image identities, language evidence/decision, dummy scope, wrappers and hooks before relying on those facts. Closed emulators still reserve ports. Do not copy a project's port to another project. One GDB controller per instance.

Standard layout:

- `src/`: Platform-neutral C game implementation and `.h` declarations; preserve guest widths/addresses. The initializer creates `src/game_main.c` with the required game entry/bindings; game code includes the shared `[XPORT_ROOT]/src/xport.h` for the complete Xport contract, fixed-width types and `FUNCTION_MARKER` and must not create a project copy or auxiliary `xport_*.h` contract header.
- `src/platform/win/`: Only `[SHORT].sln` and `[SHORT].vcxproj`; VS2022 v143 Debug x86, compiled as C. The solution platform is `x86` and maps to the vcxproj `Win32` platform; configured solution builds must pass `x86`. The project compiles the shared `[XPORT_ROOT]/src/platform/win/main.c` backend and adds translated project `src` files explicitly.
- `bin/`: Native Working Directory. The build output is exactly `bin/[SHORT].exe`, directly under `bin`.
  - `bin/DATA/`: Runtime game data extracted from the first, data track of the CD image.
  - `bin/MUSIC/`: Decimal-name IMA ADPCM WAV tracks generated from the CD audio tracks by `X convert_music`; never place them under `bin/DATA`.
- `_build/`: native compiler intermediates. Never point to another game's build tree.
- `iso/`: user-supplied disc images and tracks; record hashes/provenance and never treat filenames as identity.
- `orig/`: immutable accepted original inputs. `orig/images/[IMAGE]/` is the accepted canonical IDA snapshot; `orig/ghidra/` may retain reviewed SDK-recognition inputs when the project chooses that path.
- `status/`: reproducible databases, progress, ledgers, evidence, logs, trace sessions and reports.
  - `status/ida/runs/[LABEL]/`: fresh labelled IDA databases, logs, configs and exports under review.
  - `status/ida/accepted/[LABEL].json`: hash receipt for a run copied into the immutable configured `ida_exports` tree.
  - `status/ghidra/runs/[LABEL]/`: Ghidra PsyQ signature reports/logs; reviewed classification normally lives at `status/ghidra/classification.json`.
  - `status/translation-ledger.json`: editable TODO/WIP/DONE/SKIP implementation mappings; SQL is a derived view.
- `tools/`: project-specific adapters/fixtures, emulator data, temporary emulator builds, recovery archives and thin shared entrypoints. `tools/ida/[IMAGE].json` stores image facts and `tools/ida/databases/` stores manual IDBs copied into labelled runs by `--export-existing`. No maintained shared-tool copies.
- `xport-project.json`: machine-readable context including full `name`, user-chosen `short_name`, paths and ports. `AGENTS.md`: game revision, language decision, agreed dummy scope, actual hooks/layouts, adapter paths, build/launch details and links to shared rules; no duplicated generic methodology.

Command notation below: `X TOOL args` means `python -B [XPORT_ROOT]/tools/xport.py --project [PROJECT_ROOT] TOOL args`. Relative config paths resolve against project root; manifest paths resolve against that manifest's directory. `XPORT_PROJECT` is the explicit environment alternative. Missing context is an error.

Required/configurable fields:

- `schema:1`, full `name`, user-chosen `short_name`, `toolset` (normally `../xport/tools`). Existing projects without `short_name` remain readable, but new projects must never omit it.
- `paths`: `native_solution`, `native_executable`, `native_working_directory`, `native_build`, `disc_image`; optional `original_executable`, `native_game_collector`.
- Analysis: `paths.ida_configs` default `tools/ida`, `paths.ida_exports` default `status/ida/images`, `paths.analysis_database` default `status/analysis.sqlite`, `paths.sdk_classification`, `paths.similarity_directory`; `analysis.images`, optional unambiguous `analysis.default_image`, `analysis.wrapper_references`.
- Emulator: `duckstation.host`, `gdb_port`, `reserved_gdb_ports`, `default_role`, `data_directory`, `data_directories` by role, `trace_profile`. Default port must equal its role reservation.
- Trace/native adapter: `trace_layout`, `native_trace`, optional `adapters.python_paths` (project-local, after shared modules). Observation details are in [DUCKSTATION.md](DUCKSTATION.md); recording and replay preparation are in [RECORDING.md](RECORDING.md).

Prerequisites: Python >=3.10; Capstone for independent MIPS export; VS2022 v143 C++ x86/x64; CDB from Windows Debugging Tools. Licensed IDA/Hex-Rays and Ghidra with the PSX plugin are external/common installations. Use `-B` to avoid shared bytecode artifacts.

Gate: pin source image/disc hashes and provenance, identify executable/overlays, configure paths/ports, confirm build configuration and language decision. Do not infer C++ from an object pointer or C from absent vtables; record evidence or the user's explicit language decision.

### Red Book conversion

Run only `X convert_music`. It reads the single configured CUE, starts each `AUDIO` track at `INDEX 01`, and writes decimal track names under the configured music output using the pinned shared ADPCM-XQ encoder. Missing or ambiguous CUE data, non-sector-aligned audio and unexpected output format fail closed. Use input/output overrides only for a recorded project exception.

Projects with no reviewed Red Book tracks set `capabilities.redbook_audio.enabled=false` with a stable reason. The command still validates the CUE and returns `not_applicable` only when it contains no `AUDIO` tracks. XA-ADPCM inside a data track is game data, not converter input.

## 2. Export original code; establish SQL

1. Identify each executable/overlay revision. Record SHA-256, file segments/offsets, load addresses, entry, GP and overlay residency. Identity is `(image revision, virtual address)`, never name/address alone.
2. Create `tools/ida/[IMAGE].json`: `image`, `entry`, `gp`, `segments` (`start/end/name/class`), `loads` (`path/base/offset`), `seeds`, `code_ranges`, optional `external_symbols`. Use actual image facts. IDA auto names are not debug symbols.
3. Supply the licensed IDA path and reviewed image profiles through the current startup attention packet. The supervisor plans under `startup-ida-plan`, executes under `startup-ida-v1`, canonicalizes, and accepts the result as one guarded gate.

Startup's acceptance gate is the only normal publication path into the configured immutable `paths.ida_exports` tree (new projects use `orig/images`). It requires canonical byte verification, complete per-function artifacts, absent destination image directories and writes a complete hash receipt without deleting the labelled run. It never replaces a prior accepted image; re-analysis needs a new reviewed input root/migration rather than overwriting evidence. Existing integration targets IDA 7.7 MIPS little endian, pins every declared PSX code range to MIPS32 and normalizes any locally inferred MIPS16 instruction heads back to aligned 32-bit words before export; validate another version. The importer/canonicalizer use the final load entry as code image: split-code multi-load images require an explicit mapping extension, not guessed offsets.

Canonical output for every accepted `orig/images/[IMAGE]`:

- `functions/[ADDRESS].lst`: IDA instruction listing, preserved as discovery evidence.
- `functions/[ADDRESS].mips.txt`: independent Capstone words/disassembly with explicit delay-slot flags and source hash.
- `pseudocode/[ADDRESS].c`: Hex-Rays draft when decompilation succeeded; the sufficient structural reference for the first WIP coverage pass, while original MIPS remains the authority for every semantic detail.
- `[IMAGE].lst` and `[IMAGE].c`: aggregate listing and pseudocode convenience files.
- `functions.json`, `export-report.json`, call/data/symbol JSON and decompilation failures: machine-readable boundaries, raw words, provenance and gaps.

4. Inspect export failures, chunks, raw instructions and source-byte verification. Canonicalization adds independently decoded MIPS words/delay-slot flags and checks bytes/hashes; it does not prove boundaries or pseudocode semantics. Preserve undecoded words and data/code ambiguities.
5. Set accepted `paths.ida_exports` and matching `paths.ida_configs`. Startup owns database construction, independent similarity validation and initial progress rendering. After startup, rebuild derived views with:

```powershell
X build_database
X render_progress
X query 0xADDRESS --image [IMAGE]
```

Gate: SQL integrity/foreign-key checks; all functions/errors inventoried; source hashes match; duplicate evidence validated. Progress is generated from SQL, not independent manually edited counts.

Required PsyQ initial analysis is a startup-owned gate. The supervisor invokes the shared `tools/ghidra/RecognizePsyq.java` post-script against the installed PSX plugin signature data, classifies auditable masked candidates, then pauses for review of both `accepted` and `pending`. It selects the reviewed result through `paths.sdk_classification` before database construction. If Ghidra or the plugin is unavailable, the gate remains blocked; never silently omit it.

SKIP is applied only by the reviewed classification merge, not by editing generated IDA files or recognizing a familiar instruction sequence by eye. The classifier rechecks masked bytes against the original image, requires the IDA function entry to match a recognized label and every IDA chunk to fit within the matched PsyQ object. Short generic signatures, unresolved callees and conflicting aliases remain `pending`/TODO. Accepted entries record `image`, `address`, alias, exact `.lst` and pseudocode paths, matched objects/versions, hashes, reason and wrapper status. `build_database` imports these entries as SKIP while preserving original IDA names and TODO discovery snapshots.

An `.lst` alone deliberately has no mutable SKIP annotation. To identify a possible PsyQ routine while reading it, take its image and address from `orig/images/[IMAGE]/functions/[ADDRESS].lst` and run `X query 0xADDRESS --image IMAGE`. Treat it as SKIP only when the result contains both `function.status: "SKIP"` and a nonempty `library_classification`; inspect alias/reason/evidence there. A `pending` signature, name similarity, call to a known SDK routine or wrapper symbol is not SKIP. SKIP means an explicit replacement boundary, not DONE or runtime equivalence; validate the replacement ABI, widths, layouts, callbacks, side effects, timing and ownership before accepting behavior. If the matched body mixes game logic with SDK code, return it to TODO. Do not reuse another game's aliases or wrapper evidence automatically.

## 3. Data and evidence contracts

Current implementation: `analysis.sqlite` is rebuildable static analysis/duplicate index. Statuses import from `status/translation-ledger.json`; run evidence/queue is separate `status/controls/automation/runs.sqlite`; legacy coverage may be `status/controls/coverage.json`. Preserve each existing source until an explicit lossless migration. Never store the only manual conclusion in a disposable database. One editable source of truth per datum; derive JSON/HTML when SQL becomes authoritative.

Required model (do not claim every future table already exists):

- Image: revision/hash, source path, load segments/file offsets, entry/GP. Function: image/address, name, chunks/boundaries/confidence, size/code hash, MIPS/pseudocode paths. Instruction: address/bytes/word, disassembly, owner and delay-slot flag.
- Symbols, data references, strings and structures carry origin/confidence. Retain unclassified ranges, failed decompilation, uncertain boundaries and unresolved indirect transfers.
- Control edge: source image/function/site, target image/address, direct/conditional/tail/indirect kind, resolution evidence and ambiguity. Pointer-looking data remains a candidate until usage is proven. Same overlay address does not resolve identity.
- Duplicate pair: exact bytes vs documented MIPS normalization vs similarity, input hashes/algorithm version, differing instructions/constants/addresses, flow shape and reuse risks. Normalized equality never transfers DONE.
- Implementation: TODO/WIP/DONE/SKIP separate from static audit, dynamic coverage and comparison. Link original hash, C location and implementation revision.
- Derived C source index: `source_files`, `implementations`, `implementation_identifiers`, `source_declarations` and `implementation_declarations` map `(image,address)` to an exact C range only through an image-qualified `FUNCTION_MARKER`, an exact address-bearing symbol, or one unique address-bearing symbol in the ledger-declared source. Partial/combined regions remain `ambiguous`/`unmapped`; never select a nearby function by line proximity. Store source/body/declaration hashes and parser version. The translation ledger remains the editable mapping authority; these tables are reproducible views. `FF_FUNCTION_MARKER` remains parser-compatible for existing Fighting Force evidence but is not the generic name for new projects
- Evidence: type/scope/result, artifact path/hash, input image/source/build identities. Mark dependent evidence stale on changed inputs; retain history.
- Run: native/emulator hashes, effective configuration, paired checkpoints/provenance, character/level/context, absolute input schedule and actual decoded input, range/counts, raster/audio/shared-RAM settings. Link all channels by run/ordinal/tick.
- WIP events: unique branch identity, every occurrence, first/last event, continued/fatal status, prior skips and scenarios. Absence in one trace is not proof of unreachability.
- Large binary traces stay files; SQLite stores metadata, hashes, relationships and searchable indices.

Importer requirements: transactions, versioned schema/migrations, foreign keys/indices, reproducible import, `PRAGMA integrity_check` and `foreign_key_check`, artifact existence/hash checks. Reimport may replace derived analysis, not manual status/evidence. Export schema/importer/query CLI, provenance and unknowns when transferring a database.

`X source_index --rebuild` atomically refreshes only the derived C tables in an existing valid analysis database. It first requires every ledger `(image,address,original SHA)` to match `functions`, then preserves all unrelated/manual tables through SQLite backup and verifies integrity/foreign keys before replacement. When the parser, ledger and source inventory are unchanged, it returns without rebuilding. Ordinary edits confined to existing `.c` files update only those files, their ledger implementations, identifiers and declaration links. Header changes, added/deleted source files, ledger changes and parser changes deliberately trigger a full derived-index rebuild. Use it when IDA/static tables are current and only C sources changed. `X build_database` also creates the index during a complete reproducible import; do not bypass its stricter evidence checks merely to hide stale ledger artifacts

After every edit to project `.c`/`.h`, `X code_refresh` is a mandatory completion gate. Do not report the change as complete, start evidence capture, or continue to another code correction until the command returns `PASS`. Its source-index stage detects changed files by indexed SHA-256, removes trailing spaces/tabs, automatically runs `clang-format -i --style=file:[XPORT_ROOT]\src\.clang-format` on those exact existing `.c`/`.h`, rereads their content and only then refreshes the index. A changed shared style automatically formats the complete source inventory once. It records formatter version, style SHA-256, formatted paths and files with trimmed whitespace in its result/metadata; the agent does not issue a separate formatting command. Keep `AlignEscapedNewlines: DontAlign` in the shared style so long multiline macros do not receive padding up to `ColumnLimit` and consume audit-context budgets. `X build_database` formats the complete source inventory before a full reproducible import. Format the whole `src` tree manually only for an explicit baseline migration because it invalidates source hashes, ranges and source-bound evidence across many functions. Never format IDA/MIPS exports or evidence artifacts

`X code_refresh` performs the required routine sequence in one process: automatic format/index refresh, configured native build, configured focused tests, configured source/audit packet regeneration, `progress.html` rendering and SQLite integrity/foreign-key checks. It writes concise stdout plus full project-local `code_refresh.report` and `code_refresh.log`; the agent reads details only on failure. Configure project-specific `code_refresh.build`, `tests` argv arrays and retained `artifacts` in `xport-project.json`; never hardcode a game's build, test or address list in the shared script. The build process guard must reject replacement of a running manual game. `X source_index --rebuild`, `--no-build` and `--no-tests` are allowed only for an explicitly narrower diagnostic operation; they do not satisfy the normal post-edit completion gate

Do not emit standalone `(void)parameter;` statements solely to suppress unused formal-parameter warnings; they consume source and audit-context budget without preserving game behavior. Configure the project compiler to ignore only that warning (MSVC `/wd4100`; equivalent targeted option on other compilers). Keep unused-local and other warnings enabled. `X code_refresh` rejects newly present standalone casts whose identifier belongs to the containing function signature. Preserve `(void)function_call()` and other expressions with possible side effects

`X query 0xADDRESS --image IMAGE --audit-context [--budget 12000] [--source-line N] [--mips-address 0xSITE] [--pseudo-line N] [--full-source]` returns one content-bound context instead of making the agent search whole `.c/.h` files. It includes the exact C body when it fits, only lexically referenced declarations in the source include closure, bounded MIPS/IDA, callers/callees, reuse candidates, evidence and explicit gaps. A TODO function with no implementation-index row returns an explicit unmapped implementation and the original audit context so its first translation can follow the same gate. Default output is 4–64 KiB bounded; oversized bodies return an exact path/range/hash plus a selected window. `--full-source` returns the entire body only if the requested budget can contain it and otherwise fails. A changed indexed file fails with an instruction to refresh the index; no mtime trust

`X source_context 0xADDRESS --image IMAGE [the same selection options] [--output status/CONTEXT.json]` is the standalone form. Prefer `query --audit-context` for interactive audit and `--output` for retained evidence. Interactive stdout is compact single-line JSON to avoid spending context on indentation; retained evidence files remain readable formatted JSON. Declaration matches are lexical candidates, not proof of runtime use or type correctness. Conditional duplicate declarations may remain visible because this index does not run the platform preprocessor

## 4. Establish the trace-ready TODO → WIP frontier

1. Select functions from current user scope, startup/trace boundaries, actual trace/WIP sites and dependencies. Query callers/callees and implemented duplicate candidates before new analysis. Unreached functions may remain TODO before the first recording.
2. Build transitive static dependencies using image-aware edges. Report unresolved targets separately. Implement dependency groups before callers; collapse cycles into SCCs. Static graph is possible control flow, not execution order; only a trace proves sequence.
3. Preserve existing WIP/DONE implementations. For every selected TODO function retain original hash, current IDA pseudocode, image-verified MIPS and candidate reuse differences. Use pseudocode as the sufficient structural reference for this first pass, then resolve and correct every constant, signed operation, delay slot, ABI decision, branch, call and memory effect from MIPS bytes, callers and applicable evidence. Translate Hex-Rays failures directly from MIPS.
4. Emit C `.c/.h`; explicit guest widths, layout and address translation. Use the established platform/PsyQ wrapper. Preserve input, RNG, callbacks, timing, side effects, audio and engine cutscenes. Dummy scope must be explicitly agreed in project AGENTS; no automatic game-logic stubs.
5. Represent unresolved branches honestly as registered WIP with site/context logging. Manual discovery may skip registered WIP if the project supports it; strict evidence mode (`--debug` ABI) must stop. Never suppress input or silently classify an unknown fatal guard as harmless.
6. Add a once-per-function call marker when coverage discovery is requested. Markers prove calls, not correctness. Hits after skipped logic can be downstream artifacts.
7. Build Debug x86: `X build_native [--plan]`; solution controls `bin/[PROJECT].exe` and `_build`. Build first checks for a running manual native game. Intermediate artifacts follow the project's build paths.
8. Record WIP implementation status, exact unresolved pieces and static audit scope. During convergence, if the causal function is TODO, translate its whole derived branch before replaying: root plus SCC plus transitively resolved direct TODO callees until WIP/DONE/SKIP boundaries, with trace-proven callbacks added as roots. Keep unresolved indirect/overlay edges explicit. Do not mark DONE for pseudocode translation, successful compilation or marker coverage. Empty stubs, invented success returns, generic interpreter wrappers and ledger-only promotion do not count. Honor user-requested batch boundaries; do not insert emulator tests before an explicitly deferred test stage.

The first function follows this reproducible sequence:

```powershell
X query 0xADDRESS --image IMAGE --audit-context
# Include the shared xport.h and add the C body/declaration with its exact image-qualified FUNCTION_MARKER
# Add the source file to src/platform/win/[SHORT].vcxproj when it is new
# Add/update the reviewed status/translation-ledger.json entry shown below
X code_refresh
```

Minimal WIP ledger entry (JSON addresses are decimal numbers; copy the exact original SHA from `query`, never invent it):

```json
{
  "image": "GAME.EXE",
  "address": 2147549184,
  "sha256": "ORIGINAL_FUNCTION_SHA256",
  "status": "WIP",
  "source": "src/game.c",
  "evidence": ["status/audits/GAME.EXE-80010000.md"],
  "note": "MIPS-audited translated scope and explicit remaining gaps"
}
```

TODO means no accepted implementation. Move to WIP only after the exact `(image,address,original SHA)` has an owned C location and reviewable evidence; compilation by itself is insufficient. `FUNCTION_MARKER(0xADDRESSu, "IMAGE")`, defined by the shared `[XPORT_ROOT]/src/xport.h`, binds a source body to that identity when naming is not uniquely address-bearing. Keep unresolved branches as explicit WIP guards. A new `.c` file is not built until it is added to `[SHORT].vcxproj`. After every C/H edit, `code_refresh` is the mandatory format/index/Debug-x86-build/check/progress gate.

Gate before recording: the selected startup/trace frontier is implemented as WIP/DONE/SKIP; remaining TODO and unresolved edges are inventoried; implementation/known gaps are reviewable; no invented semantics; build succeeds where required; source/evidence ledger and `status/progress.html` reflect reality. Global TODO zero is a later full-coverage milestone, not a pre-record gate. Before every intermediate report run `X render_progress`; after changing importer inputs run `X build_database` first. Preserve legacy log encoding (ASCII additions if required).

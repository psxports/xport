# Project startup, setup, analysis and planning

Normative extension of `../AGENTS.md`. Read this file only when that contract routes the current task here.

`startup FULL_NAME SHORT_NAME` initializes the current Codex project directory. Both names are user input; never infer or normalize `SHORT_NAME`. The legacy phrase `Prepare project FULL_NAME (SHORT_NAME) according to STARTUP.md` is equivalent.

## Agent protocol

1. Resolve the current project directory and shared `[XPORT_ROOT]`. If `xport-project.json` exists, stop with `STARTUP_ALREADY_INITIALIZED` before creating a goal or changing files.
2. Create one durable goal for startup, then launch exactly one supervisor:

   ```powershell
   python -B [XPORT_ROOT]/tools/startup.py start --project PROJECT_ROOT --name "FULL_NAME" --short-name SHORT_NAME
   ```

3. Treat the returned JSON as authoritative. The command returns after dispatching a detached worker; use revision-aware `status` while work continues. Windows worker liveness uses the native process exit status so `status` neither duplicates an active detached worker nor retains a stale PID. Do not reproduce deterministic steps with individual scripts or inspect full logs unless it returns `failed`.
4. On `attention`, review the bounded context and prefilled proposal, then submit the reusable response once. On `failed`, fix the stated cause and invoke `retry` with the exact failed revision; it deduplicates compact diagnostics and removes only failed generated runs.
5. Complete the goal only when the supervisor returns `complete`. Original images, accepted exports and unrelated user work remain immutable.

Compact observation and submission:

```powershell
python -B [XPORT_ROOT]/tools/startup.py status --project PROJECT_ROOT --after-revision REVISION
python -B [XPORT_ROOT]/tools/startup.py submit --project PROJECT_ROOT
python -B [XPORT_ROOT]/tools/startup.py retry --project PROJECT_ROOT --expected-revision REVISION
```

`status/startup.json` is the durable monotonic state, `status/startup.log` holds full command output, and `status/startup-response.json` is the sole reusable response file. An unchanged `status --after-revision` returns `changed: false`; do not call a model again. A response is accepted only when both `revision` and `attention_sha256` match the current attention packet. When a frontier review establishes semantically incomplete pseudocode, record the hash-bound `analysis.pseudocode_reviews` file, then run `python -B [XPORT_ROOT]/tools/startup.py refresh-frontier --project PROJECT_ROOT --expected-revision REVISION`. This requires the exact paused frontier revision, regenerates complete evidence through the supervisor and replaces the response template; running or unrelated attention states are rejected.

The `verify_frontier` gate renders progress after synchronizing the source index and ledger into SQL. Completion counters and convergence scope must describe this refreshed state. Repair an already completed stale handoff with `python -B [XPORT_ROOT]/tools/startup.py refresh-completion --project PROJECT_ROOT --expected-revision REVISION`. It requires the exact completed revision, resumes at source verification, refreshes progress and revalidates the recording/convergence and isolated deployment gates. It preserves accepted exports and original inputs; it does not initialize another project or expand translation scope.

Response shape:

```json
{
  "revision": 4,
  "attention_sha256": "hash from startup.json",
  "decision": {}
}
```

A faulty accepted export discovered at the exact paused `psyq_review` or `frontier_translation` revision with an empty translation ledger can be superseded through the same supervisor with `python -B [XPORT_ROOT]/tools/startup.py reexport --project PROJECT_ROOT --expected-revision REVISION --label NEW_LABEL --exports orig/NEW_ROOT`. Both label and destination must be unused; the destination must stay under project `orig`. The command preserves previous accepted inputs, exports, runs and receipts, archives the prior config/state and resumes export, independent canonicalization, acceptance and PsyQ recognition under fresh labels. It rejects running, stale and post-translation states. Optional image-profile `function_boundaries` rows contain aligned `start`, exclusive `end`, exact original-byte `sha256` and an evidence-based `reason`. The exporter validates all ranges, hashes and non-overlap before changing IDA owners, preserves adjacent data and requires the exact new boundaries before export; accepted old evidence remains immutable. It never overwrites old evidence or initializes another project.

## Supervisor-owned gates

The supervisor performs and records these gates in order:

1. Refuse an initialized or conflicting root; create the C/VS2022 scaffold and unique runtime ports
   The generated C entry implements `void xport_main(void)` and defines the required `xport_gpu_graph_type_address` binding as unresolved (`0u`). Replace that binding only with original-image evidence before calling GPU wrappers; the scaffold value is not a reviewed guest address.
2. Synchronize the standard GitHub labels/issues, then run project diagnosis and Debug native build
   The generated Windows project compiles shared `xport_trace.c` alongside the PSX runtime. It also compiles shared `psx_xa.c` for XA decoding. The scaffold defines all four `xport_cd_*_address` bindings as unresolved (`0u`); replace them only with audited original-image evidence before using guest CD storage.
3. Inventory immutable inputs and hashes; propose PS-X EXE profiles and reuse a persisted licensed tool path only when the path and executable identity validate
4. Preflight IDA processor/decompiler initialization, Ghidra help output, PsyQ data and native build platform before expensive analysis; accept a legacy Ghidra launcher's nonzero `-help` exit only when the canonical headless usage marker is present. Select the highest discovered Ghidra semantic version that has a matching installed PSX extension profile; a `psyq` retry repairs a mismatched automatically discovered pair
5. Convert Red Book audio when configured
6. Plan, export, independently canonicalize and fail-closed accept IDA/MIPS evidence
7. Run Ghidra PsyQ recognition; pause with aggregate and per-item review details
8. Build SQLite/progress and independently validate similarity evidence
9. Issue one MIPS/IDA translation packet containing only the configured executable entrypoint or entrypoints; pause for reviewed C changes, then rebuild the source index and synchronize ledger statuses into SQLite
   Do not follow calls, expand dependencies or translate an SCC during startup. Always refresh the source index before trace-contract setup, including when an entrypoint already has a reviewed classification. Leave every other game function for `function_converge NAME` followed by `coverage NAME`.
   Startup does not require a native link after entrypoint translation because its direct callees may intentionally remain TODO. Coverage owns the later Debug and Release build gates.
10. As the concluding gate, create root `DuckStation.bat`, deploy and validate isolated data/settings for every reserved `audit`, `trace`, and `user` role, reuse only receipt-identical partial deployments, and publish their receipts with timing metrics in the compact handoff

The model owns only the ambiguous attention kinds `image_review`, `tool_paths`, `psyq_review`, and `frontier_translation`. It must preserve image-qualified identities, audit the complete MIPS, use pseudocode only when its completeness metadata permits it, keep unsupported facts explicit, and never treat compilation, SKIP classification or wrapper presence as runtime equivalence.

The remaining sections define startup evidence and project layout. Read `DECOMPILATION.md` only for a translation attention or decompilation failure. Runtime `record`, `stop`, and `converge` are separate workflows.

## Detailed project setup, analysis and planning

## 0. Ownership and change policy

- One maintained toolset and DuckStation installation: `[XPORT_ROOT]/tools`. Never copy shared Python/C++/DLL/resource implementations into a game project. Thin relative launchers/import shims are allowed; they contain no implementation. Startup copies the default `icon.rc`/`icon.ico` asset pair as the explicit project-owned icon exception; shared runtime code remains referenced.
- Shared improvements belong here and must work with unrelated games. Parameterize game addresses, image names, structures, ABI hooks, exclusions and paths; do not add game-name conditionals. Test a distinct project/profile and the affected established workflow.
- Update this document with each shared behavior/format/command change. Agents of every project may improve it. Preserve validated constraints and provenance; replace obsolete instructions instead of appending contradictory history. Keep project-only findings in that project's `AGENTS.md` and evidence files.
- No fixed final methodology: promote a verified reusable finding into this document/toolset. State applicability, test evidence and limitations. Do not promote a game's workaround into a platform rule.
- Preserve licenses, upstream revision, patches, build inputs and uncommitted user work. Shared DuckStation is a locally modified build, not an upstream Save State/trace API promise.
- Generated reports, traces, images, fixtures, states, databases and emulator build snapshots belong in the consuming project's `status` or `tools`. Shared source contains implementation/docs/templates/distribution only.
- Before launching/building/stopping check live process identity. Never interrupt manual play. A timeout means observe the same job/process; do not launch a replacement. Verify PID, creation time, executable, endpoint and project data directory; a shared EXE path or stale PID file alone is insufficient.
- After every completed or failed check, close emulator instances started by the agent for that check. Verify current PID/creation time, executable, project data and endpoint ownership before stopping; use the owned stop command in cleanup/finally. Preserve pre-existing user sessions and active recordings. Leave a test instance running only when explicitly requested by the user; report that handoff. An observation timeout alone does not end the check.
- Full debugger output and traces go to files. Agent output: short result, first difference, evidence path. Buffer trace writes; do not rewrite a growing JSON per frame or load whole RAM dumps into context.

## 1. Create and configure a project

Before running any initializer, require both the human-readable project name and an explicit short native name from the user. Never derive, abbreviate or silently normalize the short name. It must start with an ASCII letter, contain only ASCII letters, digits or underscores and be at most 16 characters. The exact value names `[SHORT].sln`, `[SHORT].vcxproj` and `[SHORT].exe`.

Project README template:

- `[XPORT_ROOT]/tools/templates/project/README.md.in` -> `[PROJECT_ROOT]/README.md`
- Render the current template layout without project-specific markup changes: centered linked `cover.png` without width or height attributes, project title, description, Play Game link and the `decomp`, `refactor`, `enhance`, `remaster` status rows
- Define `[PROJECT_NAME]` by removing whitespace from `[FULL_NAME]` and appending `-xport` (for example, `Pandemonium 2` -> `Pandemonium2-xport`). Use it for the README title and repository URLs; preserve `[FULL_NAME]` with spaces in the description. Do not derive it from the root directory or append `(xport)`
- Play Game is exactly `https://psxports.to/[SHORT_NAME]/`, preserving the supplied short name
- Use `@@FULL_NAME@@`, `@@PROJECT_NAME@@`, `@@REPOSITORY@@` and `@@SHORT_NAME@@`; HTML-escape displayed names and URL-encode repository/path values
- Refuse missing templates, missing/unknown tokens and an existing root `README.md` before writing scaffold files; never regenerate an existing README on retry or completion refresh
- The cover link expects a project-owned root `cover.png`; startup does not fabricate or copy another game's cover
- Progress images use `https://psxports.pages.dev/progress?repo=[PROJECT_NAME]&label=STAGE`; labels are `decomp`, `refactor`, `enhance`, `remaster`

### GitHub labels and default issues

The supervisor runs the mandatory `github` step immediately after writing the local scaffold and before diagnosis/build. The local `initialize_project` helper only writes files; `startup.py start` owns the complete initialization, including GitHub. New `xport-project.json` contains `github.repository: "psxports/[PROJECT_NAME]"`. The repository must already exist, have Issues enabled and support the organization issue type `Task`; startup does not create a repository. For a deliberately different repository identity, correct this explicit config field before retrying the failed `github` step. Never infer an existing project's repository from its directory.

- Canonical profile: `[XPORT_ROOT]/tools/templates/project/github.json`; it is a fixed versioned snapshot, not a live dependency on another game's issues
- Install GitHub CLI (`gh`) and authorize with `gh auth login --hostname github.com --web`. For a CLI outside PATH, set `XPORT_GH` to its absolute executable path. Use GitHub CLI credentials or its supported `GH_TOKEN`/`GITHUB_TOKEN` environment variables; require repository Issues write access. Do not put tokens in project config, templates or logs
- Create the six labels below with the exact color/description from the profile. Delete every other repository label, including labels currently assigned to issues or PRs; GitHub removes deleted labels from those items
- Create all 29 default issues as `open`, type `Task`, with the indicated stage label and no assignees. Create them sequentially in the `issues` array order of the canonical profile; do not alphabetize or parallelize issue creation
- Read all pages of open and closed issues before creating anything. Match exact titles, ignore PRs and preserve existing issues, including their state, body and assignees. A matching title must already have the required stage label and `Task` type; fail on conflicting metadata or duplicate titles rather than creating another issue
- Reconcile missing labels/issues on retry. Do not recreate completed issues or roll back successful remote writes after a partial API failure. Keep the `github` step failed until a retry succeeds; do not rerun local initialization or rewrite README
- Save the successful result with issue numbers/URLs in `status/startup-github.json` and the `github` step result in `status/startup.json`
- Missing CLI/authentication, repository, Issues support, issue type, write permissions, network access or API quota is a failed `github` step. Fix the cause, then run `startup.py retry --project PROJECT_ROOT --expected-revision REVISION`
- Explicit reconciliation for an initialized project: `python -B [XPORT_ROOT]/tools/startup_github.py --project PROJECT_ROOT`; require the explicit `github.repository` config field first. This command applies the same label deletion contract

Labels (hex colors):

- `startup`: `#fbca04`, empty description
- `decomp`: `#8ae13c`, empty description
- `refactor`: `#c5def5`, empty description
- `enhance`: `#0052cc`, empty description
- `remaster`: `#5319e7`, empty description
- `question`: `#d876e3`, description `Further information is requested`; no default issues

Default issues in creation order (group by `startup` -> `decomp` -> `refactor` -> `enhance` -> `remaster`; preserve the title order within each group):

- `startup` (1): `Project initialization`
- `decomp` (6): `Main Menu`, `Render`, `Audio`, `Video`, `Gameplay`, `Save Data`
- `refactor` (7): `Modules`, `Function Names`, `Variable Names`, `Struct Names`, `Initial DRAM state`, `Use native RAM`, `x64 support`
- `enhance` (9): `WideScreen support`, `Native resolution`, `Frame lerp`, `Renderer`, `Increase view distance`, `Disable LODs`, `Floating Point math`, `Disable subdiv`, `Texture Filtering`
- `remaster` (6): `Effects`, `Textures`, `Environment`, `UI`, `Fog`, `Soft Shadows`

Game-specific tasks such as `Race`, `Split Screen`, `Camera control`, `Menu effect from NTSC-U`, `Boats`, `Water shader` and `Reduce input lag (25 -> 50 Hz)` are excluded from the default profile. Add them separately only when applicable to the game. Do not delete existing game-specific issues when reconciling the default profile.

Mandatory Windows templates:

- `[XPORT_ROOT]/src/platform/win/xport.vcxproj` -> `[PROJECT_ROOT]/src/platform/win/[SHORT].vcxproj`
- `[XPORT_ROOT]/src/platform/win/xport.sln` -> `[PROJECT_ROOT]/src/platform/win/[SHORT].sln`

The agent must use both canonical templates through `startup.py`; it renders them with `@@PROJECT_GUID@@`, `@@SHORT_NAME@@`, `@@FULL_NAME@@` and `@@SHARED_SOURCE@@`. Use one new project GUID in both files, the exact user-supplied short name, an XML-escaped full name and the relative shared-source path. Never author these files from scratch or copy another game's project/solution. Preserve template Debug/Release compiler, linker and runtime settings, including Release `/O1`, `/Gw` and switchable xcrt. Fail startup if either template is missing or tokens remain unresolved; do not invent a substitute. After generation, verify that the solution refers to `[SHORT].vcxproj` with the same GUID and maps Debug/Release `x86` to project `Win32` and `x64` to project `x64`.

Default Windows icon:

- Startup copies `[XPORT_ROOT]/src/platform/win/icon.rc` and `icon.ico` into `[PROJECT_ROOT]/src/platform/win`; the local resource defines `MAINICON` from the adjacent local `icon.ico`
- The canonical vcxproj template references local `ResourceCompile Include="icon.rc"` and `Image Include="icon.ico"` in Debug and Release; the resource compiler include path contains `$(ProjectDir)`
- Shared files are the default assets for new projects. Each project owns its copies and can replace its local icon independently; later shared-default changes do not alter existing projects
- Startup refuses missing shared `icon.rc` or `icon.ico`, or existing destination `icon.rc`/`icon.ico`, before writing scaffold files
- Shared `main.c` loads `MAINICON` for the window and its small icon; the resource is embedded in the executable
- Existing project files are unchanged; apply the resource item explicitly when updating an existing project
- When updating an existing project with a user-supplied local `src/platform/win/icon.ico`, create adjacent `icon.rc` containing `MAINICON ICON "icon.ico"`; compile that local resource instead of the shared default and keep the icon search directory explicit

Run the single startup supervisor from the shared toolset. It allocates sibling-aware ports, creates the scaffold, runs diagnosis/build, and continues through all startup gates:

```powershell
python -B [XPORT_ROOT]/tools/startup.py start --project [PROJECT_ROOT] --name "[FULL PROJECT NAME]" --short-name [SHORT]
```

Initialization refuses every file it would overwrite, locks allocation, checks sibling reservations and live listeners, assigns distinct audit/trace/user GDB ports plus a separate lockstep compatibility role and creates a compilable VS2022 v143 Windows GUI C skeleton with Debug/Release for Win32 and x64. Every generated native build configuration defines the project-specific `WND_TITLE="[FULL PROJECT NAME] (xport)"`, `WND_WIDTH=960`, `WND_HEIGHT=720` and `FIELD_RATE=50`; all four definitions are mandatory because the shared runtime has no fallbacks. The initializer copies the canonical `tools/templates/project/.gitignore`, creates a project `AGENTS.md` routing stub, creates the empty translation ledger and configures `code_refresh` to build the short-name solution. The generated ignore file excludes the root `_build`, `bin`, `status`, `tools`, `orig`, `iso` and `.cursor` trees; startup refuses to replace an existing `.gitignore`. Fill the stub with reviewed image identities, language evidence/decision, dummy scope, wrappers and hooks before relying on those facts. Closed emulators still reserve ports. Do not copy a project's port to another project. One GDB controller per instance.

Standard layout:

- `README.md`: One-time render of the canonical project README template; preserve it after initialization
- `.gitignore`: Exact copy of `[XPORT_ROOT]/tools/templates/project/.gitignore`; keep generated build, runtime, analysis, original-image and agent-state trees out of project history
- `src/`: Platform-neutral C game implementation and `.h` declarations; preserve guest widths/addresses. The initializer creates `src/game_main.c` with the required game entry/bindings; game code includes the shared `[XPORT_ROOT]/src/xport.h` for the complete Xport contract, fixed-width types and `FUNCTION_MARKER` and must not create a project copy or auxiliary `xport_*.h` contract header.
- `src/platform/win/`: `[SHORT].sln` and `[SHORT].vcxproj`, plus local default `icon.rc` and `icon.ico`; project and solution are rendered from the mandatory canonical templates above; VS2022 v143 Debug/Release x86 and x64, compiled as C. Solution `x86` maps to project `Win32`; solution/project `x64` maps directly. Intermediates are separated by architecture, executable names remain shared, and xcrt is enabled for x86/x64 Release. See [PLATFORMS.md](PLATFORMS.md); converge remains x86. The project compiles the shared `[XPORT_ROOT]/src/platform/win/main.c` backend and adds translated project `src` files explicitly.
- `bin/`: Native Working Directory. Debug x86 outputs exactly `bin/[SHORT]_debug.exe`. Release x86 outputs exactly `bin/[SHORT].exe`. Both files remain directly under `bin`; one configuration never overwrites the other.
  - `bin/DATA/`: Runtime game data extracted from the first, data track of the CD image.
  - `bin/MUSIC/`: Decimal-name IMA ADPCM WAV tracks generated from the CD audio tracks by `X convert_music`; never place them under `bin/DATA`.
- `_build/`: native compiler intermediates. Never point to another game's build tree.
- `iso/`: user-supplied disc images and tracks; record hashes/provenance and never treat filenames as identity.
- `orig/`: immutable accepted original inputs. `orig/images/[IMAGE]/` is the accepted canonical IDA snapshot; `orig/ghidra/` may retain reviewed SDK-recognition inputs when the project chooses that path.
- `status/`: reproducible databases, progress, ledgers, evidence, logs, trace sessions and reports.
  - `status/ida/runs/[LABEL]/`: fresh labelled IDA databases, logs, configs and exports under review.
  - `status/ida/accepted/[LABEL].json`: hash receipt for a run copied into the immutable configured `ida_exports` tree.
  - `status/ghidra/runs/[LABEL]/`: Ghidra PsyQ signature reports/logs; write reviewed classification to `status/ghidra/classification.json` unless `paths.sdk_classification` declares another project-owned path.
  - `status/translation-ledger.json`: editable TODO/WIP/DONE/SKIP implementation mappings; SQL is a derived view.
- `tools/`: project-specific adapters/fixtures, emulator data, temporary emulator builds, recovery archives and thin shared entrypoints. `tools/ida/[IMAGE].json` stores image facts and `tools/ida/databases/` stores manual IDBs copied into labelled runs by `--export-existing`. No maintained shared-tool copies.
- `xport-project.json`: machine-readable context including full `name`, user-chosen `short_name`, paths and ports. `AGENTS.md`: game revision, language decision, agreed dummy scope, actual hooks/layouts, adapter paths, build/launch details and links to shared rules; no duplicated generic methodology.

Command notation below: `X TOOL args` means `python -B [XPORT_ROOT]/tools/xport.py --project [PROJECT_ROOT] TOOL args`. Relative config paths resolve against project root; manifest paths resolve against that manifest's directory. `XPORT_PROJECT` is the explicit environment alternative. Missing context is an error.

Required/configurable fields:

- `schema:1`, full `name`, user-chosen `short_name`, `toolset` (default `../xport/tools`). Existing projects without `short_name` remain readable, but new projects must never omit it.
- `paths`: `native_solution`, Release `native_executable`, Debug `native_debug_executable`, `native_working_directory`, `native_build`, `disc_image`; optional `original_executable`, `native_game_collector`.
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

GP validation is a mandatory blocking gate between steps 2 and 3. Treat the PS-X EXE header GP as a hint. Derive the runtime value from the unique adjacent MIPS32 `lui $gp` plus `addiu $gp,$gp` initializer in the exact loaded payload, or retain another explicitly reviewed machine-level derivation. Store the nonzero value in `gp` and its exact derivation in `gp_evidence`. Before creating an IDA command, the supervisor rereads the configured executable bytes and requires one unambiguous initializer whose value equals both fields. For a PSLZ executable whose GP is supplied by BIOS Exec, use `gp_evidence.source: "bios_exec_pslz"` with `original_path`, `original_sha256`, `bios_path`, `bios_sha256` and `bios_exec_offset`. The validator rereads both inputs, requires the exact BIOS MIPS sequence loading entry/GP from EXEC offsets 0/4 before `jalr`, verifies the configured load against the separately decoded payload, executes the complete original PSLZ unpacker with branch and load delays, and requires byte-identical output plus unchanged header GP at the metadata-derived entry. Cache-flush and critical-section BIOS calls are modeled as RAM-preserving; any other instruction or transfer is rejected. Hash changes, differing payload/load/entry or GP changes fail closed. Header values alone do not satisfy this route. Run `python -B [XPORT_ROOT]/tools/psx_pslz.py --project PROJECT_ROOT` and `python -B [XPORT_ROOT]/tools/psx_pslz_mips.py --project PROJECT_ROOT`; their project-owned outputs are `status/pslz-review/*.payload.bin`, `report.json` and `mips-verification.json`. Original packed inputs remain immutable; IDA profiles load only verified decoded payloads with offset 0. Zero GP, multiple distinct candidates, missing evidence, stale offsets, or a header/runtime mismatch blocks startup for review. Do not launch IDA, export a listing, run Hex-Rays or accept an existing IDB until this gate passes.

Raw overlays that inherit GP from a reviewed executable use `gp_evidence.source: "inherited_runtime_initializer"`. Configure exactly one offset-zero raw OVL load, a nonzero GP equal to the accepted direct parent initializer, observed aligned entry seeds inside PS1 RAM, and reviewed code ranges; do not declare the entire overlay as code when it includes data. Evidence binds `parent_config`, `parent_original_path`, `parent_acceptance_receipt`, `overlay_original_path`, `runtime_observation`, `runtime_provenance` and `bios_path`, each with its exact `_sha256` companion. Paths remain project-owned; parent configs are under `tools/ida`, acceptance receipts under `status/ida/accepted`, and runtime observations/provenance under `status`.

The inherited-GP validator rereads the parent config, original bytes, acceptance receipt and every accepted parent artifact, then invokes the direct parent GP validator. The parent must be a distinct image with `runtime_initializer` evidence; recursive inheritance and self-parenting are rejected. The raw OVL hash/load must match the profile. For every seed, require one runtime event with exact image, PC, load base and `r28 == gp`, a snapshot of at least four bytes whose exact length equals event `size`, unchanged snapshot/event hashes and matching original entry bytes. Snapshot paths stay inside the observation directory. Runtime provenance binds observer PID, executable path and process creation identity, the same BIOS hash, and the event/snapshot artifact hashes. This gate proves the observed inherited GP contract; it does not prove complete function semantics or replay an older capture.

Static loader evidence uses `gp_evidence.source: "inherited_static_loader"` with a distinct accepted direct `runtime_initializer` parent. Bind `parent_config`, `parent_original_path`, `parent_acceptance_receipt`, `overlay_original_path` and `loader_review` with `_sha256` companions. The completed status review binds a full source review, accepted original owner bodies, exact JAL sites and decoded filename, format, destination and capacity materializers. Review the complete filename forwarding, completion, cache, fallback and callee-preserved GP semantics before acceptance. Validation expands every IDA macro into original words and requires exact full chunk byte and word-map equality; independent parent and whole-overlay scans reject GP writers outside the accepted initializer pair. Declare bounded nonzero seeds with original callers and code ranges. This route returns `static_entries` and empty `observed_entries`; it never claims runtime observation. A table-driven loader needs a separately reviewed contract.

Static dependency exports from an accepted observed overlay use a distinct image name and `gp_evidence.source: "accepted_overlay_profile"`. Bind `overlay_config` and `overlay_acceptance_receipt` with `_sha256` companions. The source config must use `inherited_runtime_initializer`; recursive supplemental inheritance and self-parenting are rejected. Validation rereads every accepted source artifact and revalidates the complete observed source GP contract. Keep `gp`, `loads`, `segments` and `external_symbols` identical to the source. Declare unique aligned seeds, an entry among those seeds, and one nonempty aligned original-file-bounded `code_ranges` interval per seed in the same order; zero-only filler is rejected. Review control flow, indirect jump tables and function bounds before publication. Supplemental seeds are static evidence and return no observed entries; this route does not claim they ran in the capture. Preserve the accepted source profile, receipt and artifacts.

Run `X ida_export --ida LICENSED_IDA_PATH --image IMAGE --label UNIQUE_PLAN_LABEL --plan`, inspect the validated commands, then run the same command without `--plan` under a different unused export label. Both stages preserve accepted images and write only a fresh labelled `status/ida/runs` directory. Canonicalization and guarded startup acceptance remain mandatory before selecting exports for database import. Zero/missing GP, stale hashes, absent parent acceptance, ambiguous/missing runtime entries, differing GP, foreign paths, invalid raw load bounds or incomplete snapshots reject the inherited route.

The exporter applies the accepted GP after recreating and loading segments, before seeding functions or authoritative code-range analysis. It assigns `MIPS_GP` through the processor configuration directive and rejects every decoded GP-relative load/store whose IDA data reference differs from `gp + sign_extend(imm16)`. `export-report.json.gp_verification` records actual checked references, not just the requested configuration. Both assignment and reference verification precede per-function listings and pseudocode export. Therefore GP-relative operands and data references are based on the reviewed runtime register value in the first canonical export. Never generate pseudocode with a guessed GP and repair the resulting global addresses afterward. Each accepted export records the GP in `export-report.json`; canonicalization and database import must preserve the matching image profile.

Startup's acceptance gate is the only normal publication path into the configured immutable `paths.ida_exports` tree (new projects use `orig/images`). It requires canonical byte verification, complete per-function artifacts, absent destination image directories and writes a complete hash receipt without deleting the labelled run. It never replaces a prior accepted image; re-analysis needs a new reviewed input root/migration rather than overwriting evidence. Existing integration targets IDA 7.7 MIPS little endian, pins every declared PSX code range to MIPS32 and normalizes any locally inferred MIPS16 instruction heads back to aligned 32-bit words before export; validate another version. The importer/canonicalizer use the final load entry as code image: split-code multi-load images require an explicit mapping extension, not guessed offsets.

Canonical output for every accepted `orig/images/[IMAGE]`:

- `functions/[ADDRESS].lst`: IDA instruction listing, preserved as discovery evidence.
- `functions/[ADDRESS].mips.txt`: independent Capstone words/disassembly with explicit delay-slot flags and source hash.
- `pseudocode/[ADDRESS].c`: Hex-Rays draft when decompilation succeeded; it is a structural reference only when complete, while original MIPS remains the authority for every semantic detail. Missing or partial output is recorded and is not a failure.
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

Required PsyQ initial analysis is a startup-owned gate. The supervisor invokes the shared `tools/ghidra/RecognizePsyq.java` post-script against the installed PSX plugin signature data, classifies auditable masked candidates, then pauses for review of both `accepted` and `pending`. It selects the reviewed result through `paths.sdk_classification` before database construction. `run_ghidra` passes the configured image identity as the fourth `RecognizePsyq.java` argument; reports retain `imported_program` separately. Classification resolves the canonical identity from the configured report filename, requires `report.image` to equal that identity or the exact configured load basename for older reports, and independently verifies base, GP and every masked match against the configured bytes. Never infer image identity from a decoded payload basename alone. If Ghidra or the plugin is unavailable, the gate remains blocked; never silently omit it.

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
- Derived C source index: `source_files`, `implementations`, `implementation_identifiers`, `source_declarations` and `implementation_declarations` map `(image,address)` to an exact C range only through an image-qualified `FUNCTION_MARKER`, an exact address-bearing symbol, or one unique address-bearing symbol in the ledger-declared source. Partial or combined regions remain `ambiguous` or `unmapped`; never select a nearby function by line proximity. Store source, body, declaration hashes and parser version. The translation ledger remains the editable mapping authority; these tables are reproducible views
- Evidence: type/scope/result, artifact path/hash, input image/source/build identities. Mark dependent evidence stale on changed inputs; retain history.
- Run: native/emulator hashes, effective configuration, paired checkpoints/provenance, character/level/context, absolute input schedule and actual decoded input, range/counts, raster/audio/shared-RAM settings. Link all channels by run/ordinal/tick.
- WIP events: unique branch identity, every occurrence, first/last event, continued/fatal status, prior skips and scenarios. Absence in one trace is not proof of unreachability.
- Large binary traces stay files; SQLite stores metadata, hashes, relationships and searchable indices.

Importer requirements: transactions, versioned schema/migrations, foreign keys/indices, reproducible import, `PRAGMA integrity_check` and `foreign_key_check`, artifact existence/hash checks. Reimport may replace derived analysis, not manual status/evidence. Export schema/importer/query CLI, provenance and unknowns when transferring a database.

`X source_index --rebuild` atomically refreshes only the derived C tables in an existing valid analysis database. It first requires every ledger `(image,address,original SHA)` to match `functions`, then preserves all unrelated/manual tables through SQLite backup and verifies integrity/foreign keys before replacement. When the parser, ledger and source inventory are unchanged, it returns without rebuilding. Ordinary edits confined to existing `.c` files update only those files, their ledger implementations, identifiers and declaration links. Header changes, added/deleted source files, ledger changes and parser changes deliberately trigger a full derived-index rebuild. Use it when IDA/static tables are current and only C sources changed. `X build_database` also creates the index during a complete reproducible import; do not bypass its stricter evidence checks merely to hide stale ledger artifacts

After every edit to project `.c`/`.h`, `X code_refresh` is a mandatory completion gate. Do not report the change as complete, start evidence capture, or continue to another code correction until the command returns `PASS`. Its source-index stage detects changed files by indexed SHA-256, removes trailing spaces/tabs, automatically runs `clang-format -i --style=file:[XPORT_ROOT]\src\.clang-format` on those exact existing `.c`/`.h`, rereads their content and only then refreshes the index. A changed shared style automatically formats the complete source inventory once. It records formatter version, style SHA-256, formatted paths and files with trimmed whitespace in its result/metadata; the agent does not issue a separate formatting command. Keep `AlignEscapedNewlines: DontAlign` in the shared style so long multiline macros do not receive padding up to `ColumnLimit` and consume audit-context budgets. `X build_database` formats the complete source inventory before a full reproducible import. Format the whole `src` tree manually only for an explicit baseline migration because it invalidates source hashes, ranges and source-bound evidence across many functions. Never format IDA/MIPS exports or evidence artifacts

`X code_refresh` performs the required routine sequence in one process: automatic format/index refresh, configured native build, configured focused tests, configured source/audit packet regeneration, `progress.html` rendering and SQLite integrity/foreign-key checks. It writes concise stdout plus full project-local `code_refresh.report` and `code_refresh.log`; the agent reads details only on failure. Configure project-specific `code_refresh.build`, `tests` argv arrays and retained `artifacts` in `xport-project.json`; never hardcode a game's build, test or address list in the shared script. The build process guard must reject replacement of a running manual game. `X source_index --rebuild`, `--no-build` and `--no-tests` are allowed only for an explicitly narrower diagnostic operation; they do not satisfy the normal post-edit completion gate

`X render_progress` refreshes an existing `status/progress-data.json` atomically with the same current counts, translation ledger and derived source-index summary used for the HTML. It preserves static-analysis metrics, limitations and unrelated fields. A source-only index refresh must not leave machine-readable startup counters at an earlier database-import revision.

Do not emit standalone `(void)parameter;` statements solely to suppress unused formal-parameter warnings; they consume source and audit-context budget without preserving game behavior. Configure the project compiler to ignore only that warning (MSVC `/wd4100`; equivalent targeted option on other compilers). Keep unused-local and other warnings enabled. `X code_refresh` rejects newly present standalone casts whose identifier belongs to the containing function signature. Preserve `(void)function_call()` and other expressions with possible side effects

`X query 0xADDRESS --image IMAGE --audit-context [--budget 12000] [--source-line N] [--mips-address 0xSITE] [--pseudo-line N] [--full-source]` returns one content-bound context instead of making the agent search whole `.c/.h` files. It includes the exact C body when it fits, only lexically referenced declarations in the source include closure, bounded MIPS/IDA, callers/callees, reuse candidates, evidence and explicit gaps. A TODO function with no implementation-index row returns an explicit unmapped implementation and the original audit context so its first translation can follow the same gate. Default output is 4Р В Р’В Р вЂ™Р’В Р В Р’В Р Р†Р вЂљР’В Р В Р’В Р вЂ™Р’В Р В Р вЂ Р В РІР‚С™Р РЋРІвЂћСћР В Р’В Р В РІР‚В Р В Р’В Р Р†Р вЂљРЎв„ўР В Р Р‹Р РЋРІвЂћСћ64 KiB bounded; oversized bodies return an exact path/range/hash plus a selected window. `--full-source` returns the entire body only if the requested budget can contain it and otherwise fails. A changed indexed file fails with an instruction to refresh the index; no mtime trust

`X source_context 0xADDRESS --image IMAGE [the same selection options] [--output status/CONTEXT.json]` is the standalone form. Use `query --audit-context` for interactive audit. Use `--output` when the evidence must persist. Interactive stdout is compact single-line JSON to avoid spending context on indentation; retained evidence files remain readable formatted JSON. Declaration matches are lexical candidates, not proof of runtime use or type correctness. Conditional duplicate declarations may remain visible because this index does not run the platform preprocessor

## 4. Translate only the executable entrypoint during startup

`source_context` distinguishes a full pseudocode text view from a reviewed semantic gap. After an accepted export, project-owned `analysis.pseudocode_reviews` may bind an incomplete draft by image/address, original function hash and pseudocode hash without editing that export. Its response sets `pseudocode.complete: false`, keeps text availability in `content_complete` and includes the review reason/hash. Use the review schema and active coverage packet migration in `COVERAGE.md`; a complete file read alone does not resolve an identified omitted MIPS path.

1. Resolve startup roots only from `analysis.native_entrypoints`. If that field is absent, use the configured `analysis.default_image` entry. Do not infer additional roots from the call graph.
2. Put only configured roots whose SQL status is `TODO` in the startup packet. Do not include direct callees, transitive dependencies, callback targets, leaves or strongly connected components. Preserve WIP, DONE and reviewed SKIP roots without re-auditing them.
3. For each selected root retain the original hash, image-verified complete MIPS and available pseudocode with completeness metadata. Use complete pseudocode as the structural baseline and prove every constant, signed operation, delay slot, ABI decision, branch, call and memory effect from MIPS. Translate missing or incomplete pseudocode directly from MIPS.
4. Emit only the entrypoint C body, required declarations and its exact image-qualified `FUNCTION_MARKER`. Calls to still-TODO functions remain ordinary declared calls. Do not create stubs during startup, inline guessed implementations or widen startup scope to satisfy the linker. The later coverage supervisor alone may create the link-only TODO abort stubs defined by `COVERAGE.md`.
5. After accepting the entrypoint, rebuild the source index and derived SQL state. This is the startup translation gate. A full native link is deliberately deferred because unimplemented callees are expected.
6. Finish startup trace-contract and emulator deployment gates. Then obtain first-call order with `function_converge NAME`, stop that recording, and run `coverage NAME`. Coverage translates and proves the remaining recorded functions in packets of 16.
7. Coverage owns source-module allocation, ledger updates, intermediate validation, the first complete Debug link and the single final Release build. No later workflow may restore dependency expansion to startup.

The first function follows this reproducible sequence:

```powershell
X query 0xADDRESS --image IMAGE --audit-context
# Include the shared xport.h and add the C body/declaration with its exact image-qualified FUNCTION_MARKER
# Add the source file to src/platform/win/[SHORT].vcxproj when it is new
# Add/update the reviewed status/translation-ledger.json entry shown below
X source_index --rebuild --no-format
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

TODO means no accepted implementation. Move to WIP only after the exact `(image,address,original SHA)` has an owned C location and reviewable evidence; compilation by itself is insufficient. `FUNCTION_MARKER(0xADDRESSu, "IMAGE")`, defined by the shared `[XPORT_ROOT]/src/xport.h`, binds a source body to that identity when naming is not uniquely address-bearing. Keep unresolved branches as explicit WIP guards. A new `.c` file is not built until it is added to `[SHORT].vcxproj`. The startup entrypoint edit uses the narrower source-index gate because unresolved TODO callees are expected. Once coverage has supplied a linkable set, `code_refresh` is the normal format/index/Debug-x86-build/check/progress gate.

The public marker forwards the caller's image, address, function, file, line, host return address and enabled channel flags to `xport_function_marker` in shared `xport_trace.c`. Channel bits 1 and 2 preserve the caller translation unit's `XPORT_ENABLE_AUDIT` and `LOCKSTEP_DEBUG` choices even when shared source has different audit flags. The service emits those enabled events with the original caller identity; LockstepDebug requires the shared source to be built for that channel. The host return address is captured at the macro callsite, never inside the forwarding service. Both ordinary game translation units and trace-enabled units use the same public macro.

Gate before recording: every configured executable entrypoint is WIP, DONE or reviewed SKIP; all other TODO functions and unresolved edges remain inventoried; the entrypoint implementation and evidence are reviewable; no semantics were invented; the source index, ledger and `status/progress.html` reflect reality. A successful native link and global TODO zero are later coverage milestones. Before every intermediate report run `X render_progress`; after changing importer inputs run `X build_database` first. Preserve legacy log encoding (ASCII additions if required).

## Windows runtime template

Keep `src/platform/win/xport.vcxproj` custom runtime configuration when generating projects. Debug on both architectures uses the normal CRT; Release on both architectures includes shared `platform/win/xcrt.c` by default; each architecture also assembles its matching `platform/win/xcrt_x86.asm` or `platform/win/xcrt_x64.asm` through the canonical MASM imports. `/p:UseXcrt=false` restores the standard CRT in the same Release configuration. Runtime/file/format limits and Release linker settings: [LIBRARY.md](LIBRARY.md#windows-release-crt). Do not introduce local CRT replacements or new runtime configurations.

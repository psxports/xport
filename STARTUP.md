# New PSX xport project startup

Trigger: `Prepare project FULL_NAME (SHORT) according to STARTUP.md`.

`FULL_NAME` is the human-readable name. `SHORT` is the exact user-chosen native name used for `.sln`, `.vcxproj` and `.exe`; never infer, expand, shorten or normalize it.

## Agent execution contract

1. Read `[XPORT_ROOT]/tools/PIPELINE.md`, then `[XPORT_ROOT]/tools/docs/PROJECT_SETUP.md`, before changing project files.
2. Create one durable goal covering the complete preparation described below. Do not create separate competing goals for the phases. Track phases as gates inside the single goal.
3. Use this default objective:

   `Prepare the PSX xport project FULL_NAME (SHORT) from immutable original inputs through a verified VS2022 C scaffold, accepted byte-verified IDA/MIPS exports, reviewed PsyQ classification, reproducible SQL/progress, and the first MIPS-audited compilable WIP translation.`

4. If the user did not specify a project root, default to a sibling of `xport` named `FULL_NAME-xport`. If that path is invalid, ambiguous or already contains project files, stop and ask for the exact root; never merge with or overwrite an existing project.
5. Inspect supplied files and the intended project root before asking questions. Ask one concise question only for genuinely missing authority or inputs, such as the disc/EXE path, uncertain revision, licensed IDA path, Ghidra/PSX-plugin path, language decision or dummy scope.
6. Preserve immutable inputs and unrelated user work. Never copy another game's addresses, ports, SDK classifications, wrapper claims, runtime hooks or completion evidence.
7. Report each completed phase briefly: result, important identities/counts, unresolved facts and the next phase. Do not claim the overall goal complete until every mandatory gate below passes. Missing licensed tools or original inputs are explicit blockers, not permission to invent results.

## Phase 0 — Resolve inputs and scope

Required user/project facts:

- Full human-readable name and exact short native name
- Project root, or permission to use the default sibling path
- Original disc image and/or executable paths and known revision
- C as the native source language, unless the user explicitly chooses otherwise before initialization
- Licensed IDA/Hex-Rays path and Ghidra plus PSX-plugin path, when not discoverable locally
- Any explicitly agreed dummy/replacement scope; never assume game logic, audio, cutscenes, input or storage may be stubbed

Gate:

- Inputs exist and are readable
- The destination will not overwrite a configured project or generated targets
- Unknown facts are listed instead of guessed

## Phase 1 — Initialize and prove the native scaffold

From the workspace parent run:

```powershell
python -B xport/tools/project_init.py --root PROJECT_ROOT --name "FULL_NAME" --short-name SHORT
python -B xport/tools/xport.py --project PROJECT_ROOT doctor
python -B xport/tools/xport.py --project PROJECT_ROOT build_native
```

The initializer must create:

- `src/platform/win/SHORT.sln`
- `src/platform/win/SHORT.vcxproj`
- `src/game_main.c`
- shared `[XPORT_ROOT]/src/xport.h` as the only Xport public header, providing the platform/game contract, fixed-width types and `FUNCTION_MARKER(address, image)`
- mandatory quoted `WND_TITLE="FULL_NAME (xport)"` in every native build configuration, with no runtime fallback
- `bin/SHORT.exe` after the build
- `_build/`, `iso/`, `orig/images/`, `orig/ghidra/`, `status/ida/runs/`, `status/ghidra/` and `tools/ida/databases/`
- `xport-project.json`, project `AGENTS.md` and an empty `status/translation-ledger.json`

Update the generated project `AGENTS.md` with confirmed game facts as they become available. Do not put generic xport methodology there.

Gate:

- `doctor` reports the correct full name, short name and paths
- VS2022 v143 `Debug|Win32` compiles as C
- The output is exactly `bin/SHORT.exe`, working directory `bin`, intermediates `_build`

## Phase 2 — Inventory immutable original images

1. Copy or reference the user-authorized original inputs without modifying them.
2. Record SHA-256, provenance and revision for the disc, executables and overlays.
3. Determine each image's file offset, load address, entry point, GP, segments and code ranges from headers/bytes and auditable analysis.
4. Create `tools/ida/IMAGE.json` for every executable identity and configure `analysis.images`, `analysis.default_image` only when unambiguous, `paths.disc_image` and other applicable paths in `xport-project.json`.
5. Record overlapping virtual-address spaces as distinct `(image,address)` identities.
6. When the staged `iso` directory contains a CUE sheet with `AUDIO` tracks, invoke the agent workflow command below. Do not reproduce its current script or encoder arguments in this startup contract.

   > convert_music

Gate:

- Every configured image is pinned to an original hash and source path
- Entry, GP, loads, segments and code ranges are explicit
- Unknown overlays/ranges remain explicit gaps
- Every CUE `AUDIO` track is present as a decimal-name 4-bit IMA ADPCM WAV under `bin/MUSIC`, or the disc has no audio tracks
- `bin/DATA` contains the runtime data extracted from the first, data track of the CD image; `bin/MUSIC` contains only tracks generated by `convert_music`

## Phase 3 — Export, canonicalize and accept IDA evidence

Use unique labels; a plan label is never reused as the executed run:

```powershell
X ida_export --ida IDAT_PATH --label export-plan --plan
X ida_export --ida IDAT_PATH --label export-v1
X ida_canonicalize --exports status/ida/runs/export-v1 --configs status/ida/runs/export-v1
X ida_accept --label export-v1
```

Here and below:

```text
X = python -B [XPORT_ROOT]/tools/xport.py --project PROJECT_ROOT
```

For every accepted function require:

- `orig/images/IMAGE/functions/ADDRESS.lst` — IDA listing
- `orig/images/IMAGE/functions/ADDRESS.mips.txt` — independent byte-verified MIPS words and delay slots
- `orig/images/IMAGE/pseudocode/ADDRESS.c` when Hex-Rays succeeded
- aggregate `IMAGE.lst` and `IMAGE.c`
- machine-readable functions, calls, references, symbols, failures and export provenance

`ida_accept` must reject incomplete, uncanonicalized or already accepted images and retain its receipt under `status/ida/accepted`. Never publish by manually copying a run into `orig/images`.

Gate:

- Source bytes and per-function hashes verify
- IDA failures and undecoded words remain visible
- Accepted images are immutable and have a hash receipt

## Phase 4 — Recognize PsyQ and review SKIP

```powershell
X ghidra_recognize --ghidra GHIDRA_INSTALL --plugin PSX_PLUGIN --label psyq-v1
X psyq_classify --reports status/ghidra/runs/psyq-v1 --output status/ghidra/classification.json
```

Review both `accepted` and `pending` entries. Apply PsyQ SKIP only when the classifier has rechecked the masked bytes, matched the exact IDA function entry and contained every IDA chunk in the recognized PsyQ object. Short generic signatures, missing recognized callees, conflicting aliases and mixed game/SDK bodies remain TODO.

An `.lst` is immutable discovery evidence and is never edited to say SKIP. Connect a listing to classification by exact image/address:

```powershell
X query 0xADDRESS --image IMAGE
```

Treat the function as PsyQ SKIP only when `function.status` is `SKIP` and `library_classification` contains the reviewed alias, reason and masked-match evidence. SKIP is a planned replacement boundary, not DONE or runtime equivalence.

Gate:

- Every configured image has a recognition report or an explicit blocked/deferred record
- Accepted and pending candidates are reviewed separately
- Wrapper presence is not presented as ABI/runtime proof

## Phase 5 — Build the analysis database and progress views

Configure `paths.sdk_classification`, then run:

```powershell
X build_database
X validate_similarity
X render_progress
```

Gate:

- SQLite integrity and foreign keys pass
- Function identities are image-qualified
- TODO/WIP/DONE/SKIP counts come from the ledger/classification merge
- Listings, pseudocode, call edges, failures, gaps and similarity evidence are queryable
- PsyQ aliases remain separate from immutable IDA names

## Phase 6 — Establish a trace-ready compilable WIP frontier

Preserve every existing WIP and DONE implementation. Translate the initial runtime frontier needed to start representative recording and convergence: startup, main dispatch, trace/checkpoint boundaries and their resolved TODO dependency branches. Work in small, closed dependency groups so each patch remains reviewable; prefer groups that establish useful platform/startup structure without inventing unavailable device or game behavior. Unreached functions may remain TODO before the first recording.

For every selected function:

```powershell
X query 0xADDRESS --image IMAGE --audit-context
```

Then:

1. Use Hex-Rays pseudocode as the sufficient structural reference for the first full WIP coverage pass. Preserve its useful control flow and data-flow shape instead of restarting decompilation from scratch.
2. Use original MIPS bytes as the mandatory evidence for constants, signedness, wrapping, delay slots, ABI, calls, branches and memory effects. Correct the pseudocode wherever it conflicts with those bytes; translate the small set of Hex-Rays failures directly from MIPS.
3. Write C declarations and bodies using explicit guest widths/address helpers and the selected wrapper boundary.
4. Include the shared `[XPORT_ROOT]/src/xport.h` and place `FUNCTION_MARKER(0xADDRESSu, "IMAGE")` inside the exact implementation body when an address-bearing function name is not used. Do not create a project-local type or `xport.h` copy.
5. Keep new `.c/.h` files directly under `src` and add them to `src/platform/win/SHORT.vcxproj`.
6. Add a reviewed WIP entry to `status/translation-ledger.json` with exact image, decimal address, original SHA-256, source, evidence paths and remaining gaps. Never promote status without a real owned C body.
7. Represent unresolved reachable behavior with explicit registered WIP handling; do not silently stub or suppress it.
8. Run `X code_refresh` after each completed C/H patch before beginning another correction.

Compilation or pseudocode translation alone never earns DONE. Unless compatible original/native runtime evidence already exists in the declared scope, the first translated functions remain WIP. Empty bodies, invented success returns, generic interpreter wrappers and ledger-only status changes do not count as translation coverage. When convergence reaches a TODO function, translate its whole derived TODO branch before replaying: the root, its strongly connected component and transitively resolved direct TODO dependencies until WIP/DONE/SKIP boundaries. Add trace-proven callback targets as explicit additional roots; retain unresolved indirect or overlay-context targets as gaps instead of guessing them.

Gate:

- Exact source ownership is mapped by the source index
- The initial record/converge frontier is WIP/DONE/SKIP and remaining TODO is inventoried, not hidden
- VS2022 Debug x86 build passes through `code_refresh`
- Ledger, source hashes, audit packet and progress are current
- Remaining unknowns are explicit

## Phase 7 — Final preparation handoff

Run no redundant builds or tests after the final passing gate. Produce a compact handoff containing:

- Full name, short name, root and key input hashes
- Configured images with entry/GP and accepted IDA receipt
- PsyQ accepted/pending counts and replacement limitations
- SQL integrity and TODO/WIP/DONE/SKIP counts
- Native solution/executable paths and final build result
- First WIP function group, evidence and unresolved branches
- Exact next recommended task

Mark the single startup goal complete only when Phases 0–7 pass. Runtime recording, `record → stop → converge`, gameplay completion and promotion from WIP to DONE are later tasks unless the user explicitly includes them in the startup scope.

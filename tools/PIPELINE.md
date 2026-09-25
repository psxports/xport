# PSX xport agent entrypoint

For an initialized project, read this file and the consuming project's `AGENTS.md` at task start. `startup` is the exception: no project config or `AGENTS.md` exists yet, so use the shared xport root containing this file and follow `STARTUP.md`. Do not preload other documents, old chats, full logs, traces, database schemas or broad source trees.

For initialized projects, resolve `[XPORT_ROOT]` from `xport-project.json` and define:

`X = python -B [XPORT_ROOT]/tools/xport.py --project PROJECT_ROOT`

## Core contract

- Original images, exports, traces and evidence are immutable inputs. C is a tested translation, never authority
- IDA pseudocode supplies first-pass structure; original MIPS bytes and SQL determine immediates, signedness, delay slots, ABI, control flow and memory effects; replay establishes behavior
- Status authority is `status/translation-ledger.json` plus `status/analysis.sqlite`. Source presence, compilation, similarity, fixtures and narrow replay do not imply `DONE`
- Bind every mapped external memory image to an exact identity. Fail closed on missing, changed or ambiguous evidence, ownership, mapping, continuation or comparison scope
- Preserve unrelated changes. Shared tools belong in `[XPORT_ROOT]/tools`, shared PSX/PsyQ host runtime in `[XPORT_ROOT]/src`, and game facts/adapters/evidence in the consuming project
- Use compact receipts and linked excerpts. Expand context only to answer a stated unresolved question

## Task entrypoints

- `startup FULL_NAME SHORT_NAME`: refuse with `STARTUP_ALREADY_INITIALIZED` if the current Codex project directory contains `xport-project.json`; otherwise create one goal and run `python -B [XPORT_ROOT]/tools/startup.py start --project PROJECT_ROOT --name "FULL_NAME" --short-name SHORT_NAME`. Follow only its revisioned attention packets; `STARTUP.md` defines the compact response protocol
- `upgrade`: run `X upgrade`, create its single requested durable goal, run `X upgrade --apply`, complete only the returned manual remainder, then `X upgrade --complete`; details are in `docs/UPGRADING.md`
- `convert_music`: run `X convert_music`; project exceptions and disc/audio contracts are in `docs/PROJECT_SETUP.md`
- `record SLOT NAME`: run `X trace_workflow record SLOT NAME --wait 30`; use only the returned status command and then await `stop`; details are in `docs/RECORDING.md`
- `stop`: run `X trace_workflow stop --wait 30`; do not invoke lower-level stop, kill or decode paths unless the workflow returns recovery attention
- `converge NAME`: create the requested convergence goal and run `X trace_workflow converge NAME --wait 30`; observe only revisioned status. On code attention, consume the issued packet and submit the bounded repair through `X trace_workflow submit-repair`; details are in `docs/CONVERGENCE.md` and `REPAIR_CONTRACT.md`
- Manual MIPS/IDA decompilation or C correction: read `docs/DECOMPILATION.md`

## Change gates

- Before editing an implementation, use `X query 0xADDRESS --image IMAGE --audit-context`; keep image-qualified source and ledger identity exact
- After every project C/H patch, run `X code_refresh`. If IDA, importer or classification inputs changed, run `X build_database` first. Run `X render_progress` before an intermediate report when the workflow has not already published it
- `TODO -> WIP` requires a real compilable body with explicit image/address/ownership. `DONE` additionally requires exact identity, static evidence, build/tests and applicable original/native runtime evidence
- A causal TODO expands to its complete bounded image-qualified dependency branch. Translate dependency-first; do not guess indirect or overlay edges
- Debug builds are diagnostic. Before declaring a native-code task complete, run `X build_native --configuration Release`; runtime claims require the smallest applicable Release smoke check
- Shared command, format, behavior or assumption changes require the matching focused document update and a new immutable UTC entry in `[XPORT_ROOT]/CHANGELOG.md`

## On-demand documents

- `docs/PROJECT_SETUP.md`: initialization, ownership/layout, IDA/Ghidra import, SQL/evidence model, music conversion and trace-ready frontier
- `docs/UPGRADING.md`: bounded changelog upgrades, migrations and revision markers
- `docs/DUCKSTATION.md`: shared emulator deployment, profiles, observation and checkpoints
- `docs/RECORDING.md`: interactive record/stop, capture validation, input and anchor preparation
- `docs/REPLAY.md`: stage packages, adapters, registry, comparison and replay portability
- `docs/CONVERGENCE.md`: worker states, diagnostics, repair submission, validation, caching, cleanup and telemetry
- `REPAIR_CONTRACT.md`: declarative repair, solver and codegen trust boundaries
- `docs/DECOMPILATION.md`: MIPS/IDA audit, command reference, WIP/DONE gates and semantic completion
- `docs/RECOVERY.md`: interrupted workers, archives, retention and recovery acceptance
- `duckstation/SOURCES.md`: upstream identity, patching, licensing, build and packaging

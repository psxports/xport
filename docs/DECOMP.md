# Decomp

Agent command: `decomp`. Static helper: `X decomp [--image IMAGE] [--limit N] [--after CURSOR]`. The helper prints a queue, not C code, a goal or a completed task.

## Inputs and goal

Read current [context](CONTEXT.md), this contract and only affected [DECOMP_REFERENCE.md](DECOMP_REFERENCE.md) sections. Before editing, create/reuse one active goal covering the entire requested translation, integration and final verification. Keep it active through repairs; a queue, plan or module is not completion. Explicit narrower user scope wins.

Default scope: every known game function in every accepted image, in deterministic image/address order, except reviewed SDK bodies and authorized save/movie replacements below. Use accepted bytes, inventories, GP, full MIPS and available pseudocode; preserve originals and complete translations. Do not launch emulators, collect execution coverage or require functions.bin. Unknown original boundaries or reachable semantics block affected functions; never invent implementations.

## Preparation and reuse

Before the first new body verify code/data boundaries, original identities, GP and reviewed SDK classifications. Use indexed tools to build/reuse exact-body and operand-template families within/across images; preserve every occurrence and substitution. Classify pointer/address/offset differences; they alone do not establish different algorithms. Mnemonic similarity finds candidates, not equivalence.

Before each translation inspect exact/family C and evidence. Reuse unchanged reviewed logic only after proving substitutions and each member's ABI, data/layout, aliasing, residency and dispatch. Keep unresolved variants separate; add no generic wrappers solely for duplicates. Work-unit reduction neither transfers SDK/body acceptance nor replaces original-function accounting.

## Translation

Analyze each function while writing it; follow the [mandatory audit](DECOMP_REFERENCE.md#mandatory-function-translation-audit), not a postponed whole-pass review. Record unresolved producer/consumer questions and revisit them when dependencies become available. Saved drafts are not complete translations.

Pending authorization blocks only affected functions/dependencies. Record the scope, request approval once for a concrete proposal and continue independent functions in order. Resume affected work after approval; never infer consent. Do not stop decomp or block the whole goal while the reviewed remaining queue permits independent work.

Decompiled code is supported only on little-endian target systems.

Name every recovered original function `sub_[address]` at its original virtual address, e.g. `sub_8001002C`. Keep one implementation per original address regardless of earlier semantic C names. Host-only helpers use normal language-specific names.

1. Honor the recorded language/standard and [naming contract](DECOMP_REFERENCE.md#target-language-and-naming). Establish one signature registry, semantic arguments and guest-address versus host-pointer domains
2. Read complete MIPS and complete available pseudocode. Original MIPS is authority. Use pseudocode as structural baseline only when complete; record its actual completeness instead of inventing missing output
3. Derive widths/signedness from operations and relevant producers/consumers. Split unrelated register lifetimes; fix declarations/signatures instead of casting each use. Remove redundant casts before final audit; retain required intermediate extension/narrowing, wrapping, signed comparisons/shifts. Use established ranges, never one trace; see [numeric contracts](DECOMP_REFERENCE.md#semantic-types-and-integer-conversions)
4. Translate the whole function, including all blocks, delay slots, calls, carrier effects and error paths. No representative paths, approximations, successful stubs or deferred reachable logic. Audit the final body against the complete MIPS and relevant state-carrying helpers
5. Save complete functions immediately with image-qualified FUNCTION_MARKER and provenance; bind through project_context decompile after review. Pending callees stay declarations. A prototype, partial body or abort stub is not complete
6. Continue the entire selected pass before integration. Keep one current scope/family record and only genuine expensive gaps in project context; no per-cast/per-edit diary

Use [audit registration](DECOMP_REFERENCE.md#audit-registration) for reviewed bindings.

## Modules

Resume existing modules without relocation. Name new modules src/batch_1.c, batch_2.c, ... (.cpp for C++). Hard limit:16 complete original functions per batch; keep functions whole, use no batch build gate and record new modules for final integration. After each batch write/header update report `T/N` and rounded percentage from the frozen inventory below; deferred bindings do not change the fully written/audited count.

Form each batch as an exact in-context working copy. The translator owns it exclusively during decompilation and guarantees no other process edits it. Create no file before emitting its completed contents; never reread/reload the pending batch. Accumulate whole audited functions/corrections in context, then emit the complete module once before the next `batch_*.c`. Body audits/bindings may wait for one end-of-goal pass against emitted sources/current hashes.

Use one signature registry, `src/batch.h`, included by every batch C/C++ module; do not duplicate shared declarations locally. On each batch emission run `python -B [XPORT_ROOT]/tools/register_decomp.py --project PROJECT_ROOT --update-header src/batch_N.c`; add declarations for all nonstatic functions and newly needed cross-module callees to `batch.h` in that step. Keep `static` helpers local; preserve guest-address/native-pointer domains and argument widths. Reconcile duplicate signatures without forwarding wrappers.

## Runtime boundaries

Reuse xport PsyQ APIs; reviewed SDK bodies are not game translations. Validate actual caller ABI, ownership and callbacks. No parallel SDK/device state or shared implementation replacement without separately authorized evidence-based scope. SDK identity/wrapper names do not prove integration; see [SDK reuse](DECOMP_REFERENCE.md#reuse-the-existing-xport-sdk-runtime).

Use shared guest memory and standard r_u*/r_s*/w_u* accessors. Native pointer boundaries require an explicit bounded span through psx_addr; do not create project-local replacements. Native-state migration belongs to authorized refactor.

Replace identified game CD file/read/seek/readiness adapters with extracted bin/DATA access through shared file services; preserve file errors, lengths, callbacks and gameplay effects. Native/Web runtime must not read ISO/BIN/CUE as asset fallback.

Do not decompile memory-card operations, managers or slot/confirmation screens. Remove their calls/UI from the executable path; connect save/load actions directly to xport_save/xport_load with one project save and no selection, prompts or confirmations. Retain payload packing/validation and gameplay effects, not card machinery. Do not decompile movie-player internals: fullscreen uses xport_fmv_play; in-game movies use xport_fmv_open/next/close. Preserve scene timing, texture upload and setup/cleanup around those services. Record omitted original identities as authorized replacements, never translated bodies or unfinished stubs. Follow [replacement contracts](DECOMP_REFERENCE.md#authorized-save-and-movie-replacements), [FMV.md](FMV.md) and [host tools](HOST_TOOLS.md).

## Debug-only assertions

XASSERT is executable only under _DEBUG; other builds do not evaluate its argument. Arguments are checks only: no calls, assignments, increments/decrements, volatile/device accesses or hidden side effects. Perform required operations outside the assertion in all builds. Required runtime validation/error handling stays ordinary code. See [assertion migration](REFACTORING_CONTRACTS.md#invariant-assertion-migration).

## Integration and completion

Follow [BUILD_POLICY.md](BUILD_POLICY.md). After the full pass audit project-wide signatures, layouts, producer/consumer assumptions, dispatch, callbacks and ownership; close questions and review affected bodies before integration. This supplements per-function audits. Register all modules; reconcile SDK boundaries/dependencies. Run indexing, configured Debug checks and agreed scenarios; repair failures from original evidence, then perform applicable final builds. The final gate includes Web build/package, required MP4 preparation and checking startup, assets/input and save/movie integration. Never defer initial Web delivery to enhance. Explicit user exclusions apply. Inspect requested rendering/input/audio, not just linking.

Before completion [stage sources](CONTEXT.md#git-source-index); preserve runtime limitations and historical audits. Complete only when every selected function has a full implementation or reviewed authorized replacement, integration is resolved and all required final checks/user gates pass. Static completeness, builds and scenario execution are separate claims.

## Progress

Freeze the selected image-qualified inventory S. Accepted exclusions E require reviewed SDK identity, authorized replacement or explicit user removal; candidates/unknowns do not qualify. A=S-E. T is the number of current complete original-body bindings in A, N=|A|. Duplicates count by original occurrence, not template count. Exclusions contribute to neither numerator nor denominator.

Report T/N, current phase and blocker. Rename the current chat to `decomp: n%`, n=floor(100*T/N+0.5), at start/resumption and whenever rounded progress changes. For N=0 report 100% and explain the empty active set. Record genuine exclusion/scope changes and retain frozen counts. In Codex use mcp__codex_app.set_thread_title(title=...) without threadId; if unavailable report progress in chat and continue. A 100% title measures translation only, never completion of final integration.

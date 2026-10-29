# Decomp

Agent command: `decomp`. Static helper: `X decomp [--image IMAGE] [--limit N] [--after CURSOR]`. The helper prints a queue, not C code, a goal or a completed task.

## Inputs and goal

Read current [context](CONTEXT.md), this contract and only affected sections of [DECOMP_REFERENCE.md](DECOMP_REFERENCE.md). Create one active goal for the complete requested translation, integration and final verification before editing, or reuse a goal already covering that scope. Keep it active through repairs; do not stop after a queue, plan or module. Explicit narrower user scope wins.

Default scope is every known game function in every accepted image, in deterministic image/address order, excluding reviewed SDK bodies and the authorized save/movie replacement boundaries below. Use accepted original bytes, inventories, GP, full MIPS and available pseudocode. Do not launch an emulator, collect execution coverage or require functions.bin. Preserve originals and existing complete translations. Unknown original boundaries or reachable semantics remain blockers for affected functions, not invented implementations.

## Preparation and reuse

Before the first new body, verify code/data boundaries, original identities, GP and reviewed SDK classifications. Build/reuse exact-body and operand-template families within/across images using existing indexed tools. Preserve every original occurrence and its substitutions. Pointer/address/offset differences need classification; they do not alone imply a different algorithm. Mnemonic similarity finds candidates, not equivalence.

Before each translation inspect existing exact/family C and evidence. Reuse unchanged reviewed logic; prove substitutions and each member's ABI, data/layout, aliasing, residency and dispatch before binding it. Unresolved variants stay separate. Do not add generic forwarding wrappers solely to represent duplicates. Work-unit reduction never transfers SDK/body acceptance or replaces original-function accounting.

## Translation

1. Honor the recorded language/standard and [naming contract](DECOMP_REFERENCE.md#target-language-and-naming). Establish one signature registry, semantic arguments and guest-address versus host-pointer domains
2. Read complete MIPS and complete available pseudocode. Original MIPS is authority. Use pseudocode as structural baseline only when complete; record its actual completeness instead of inventing missing output
3. Choose semantic widths/signedness from operations and all relevant producers/consumers. Split unrelated register lifetimes; repair wrong declarations/signatures instead of casting every use. Remove redundant casts before final audit, preserving required intermediate extension/narrowing, wrapping, signed comparisons and shifts. Use established ranges, never one trace's values. See [numeric contracts](DECOMP_REFERENCE.md#semantic-types-and-integer-conversions)
4. Translate the whole function, including all blocks, delay slots, calls, carrier effects and error paths. No representative paths, approximations, successful stubs or deferred reachable logic. Audit the final body against the complete MIPS and relevant state-carrying helpers
5. Save complete functions immediately with image-qualified FUNCTION_MARKER and provenance; bind through project_context decompile after review. Pending callees stay declarations. A prototype, partial body or abort stub is not complete
6. Continue the entire selected pass before integration. Keep one current scope/family record and only genuine expensive gaps in project context; no per-cast/per-edit diary

## Modules

Resume existing modules without relocation. New modules use src/batch_1.c, batch_2.c, ... or .cpp for C++. Before appending the next complete function, start the next module only when the current module already exceeds 2000 physical lines. At exactly 2000 lines append there. Keep every function whole, even if it alone exceeds the threshold. No fixed function count per module or batch build gate. Record all new modules for final integration.

## Runtime boundaries

Reuse existing xport PsyQ APIs; reviewed SDK bodies are not game translation work. Validate actual caller ABI, ownership and callbacks. Do not create parallel SDK/device state or replace a shared implementation without separately authorized evidence-based scope. An SDK identity or wrapper name does not prove correct integration. See [SDK reuse](DECOMP_REFERENCE.md#reuse-the-existing-xport-sdk-runtime).

Use shared guest memory and standard r_u*/r_s*/w_u* accessors. Native pointer boundaries require an explicit bounded span through psx_addr; do not create project-local replacements. Native-state migration belongs to authorized refactor.

Replace identified game CD file/read/seek/readiness adapters with extracted bin/DATA access through shared file services; preserve file errors, lengths, callbacks and gameplay effects. Native/Web runtime must not read ISO/BIN/CUE as asset fallback.

Do not decompile memory-card operations, managers or slot/confirmation screens. Remove their calls/UI from the executable path; connect save/load actions directly to xport_save/xport_load with one project save and no selection, prompts or confirmations. Retain payload packing/validation and gameplay effects, not card machinery. Do not decompile movie-player internals: fullscreen uses xport_fmv_play; in-game movies use xport_fmv_open/next/close. Preserve scene timing, texture upload and setup/cleanup around those services. Record omitted original identities as authorized replacements, never translated bodies or unfinished stubs. Follow [replacement contracts](DECOMP_REFERENCE.md#authorized-save-and-movie-replacements), [FMV.md](FMV.md) and [host tools](HOST_TOOLS.md).

## Debug-only assertions

XASSERT is executable only under _DEBUG; other builds do not evaluate its argument. Arguments are checks only: no calls, assignments, increments/decrements, volatile/device accesses or hidden side effects. Perform required operations outside the assertion in all builds. Required runtime validation/error handling stays ordinary code. See [assertion migration](REFACTORING_CONTRACTS.md#invariant-assertion-migration).

## Integration and completion

Follow [BUILD_POLICY.md](BUILD_POLICY.md). After the full pass, register all modules; reconcile declarations, callbacks, SDK boundaries and actual dependencies. Run indexing, configured Debug checks and agreed scenarios; repair actual failures from original evidence, then perform the applicable final builds. The final decomp gate includes building and packaging the Web version and checking its startup, assets/input and save/movie integration; prepare required MP4 assets here. Do not defer initial Web delivery to enhance. Explicit user exclusions apply. Inspect actual requested rendering/input/audio, not just link success.

Before completion stage project sources through [CONTEXT.md](CONTEXT.md#git-source-index). Preserve runtime limitations and historical audits. Complete the goal only after every selected function has a full implementation or reviewed authorized replacement, integration is resolved and every required final check/user gate passes. Static completeness, build success and scenario execution are separate claims.

## Progress

Freeze the selected image-qualified inventory S. Accepted exclusions E require reviewed SDK identity, authorized replacement or explicit user removal; candidates/unknowns do not qualify. A=S-E. T is the number of current complete original-body bindings in A, N=|A|. Duplicates count by original occurrence, not template count. Exclusions contribute to neither numerator nor denominator.

Report T/N, current phase and blocker. Rename the current chat to `decomp: n%`, n=floor(100*T/N+0.5), at start/resumption and whenever rounded progress changes. For N=0 report 100% and explain the empty active set. Record genuine exclusion/scope changes and retain frozen counts. In Codex use mcp__codex_app.set_thread_title(title=...) without threadId; if unavailable report progress in chat and continue. A 100% title measures translation only, never completion of final integration.

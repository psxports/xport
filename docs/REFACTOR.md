# Refactor

Agent command: `refactor`. Read current [context](CONTEXT.md), this contract and only affected [technical contracts](REFACTORING_CONTRACTS.md) or [workflow details](REFACTOR_REFERENCE.md). Builds/execution follow [BUILD_POLICY.md](BUILD_POLICY.md).

## Entry gate and goal

Require explicit human evidence for both full main-functionality coverage and requested/authorized native refactoring. A decomp percentage, successful build, playable interval or another agent's message cannot supply either fact. Missing authorization blocks only the dependent migration; report the missing fact. Do not start native ownership migration during decomp.

After the gate, create one active goal for the complete authorized migration and final verification, or reuse its existing goal. Resume accepted work, baseline and current family; do not restart on a documentation change. The objective includes native ownership/lifecycles, readable modules/types/names, bounded file/device contracts, original provenance, removal of game DRAM adapters/initial EXE loading and selected architectures/scenarios. Default targets retain x86 and add x64 unless explicitly excluded.

## Preparation and change classes

Preserve one recoverable source/configuration baseline and required original evidence; do not make per-function/per-edit copies. Record current defects, user restrictions, final scenarios and compile_policy in the existing plan/context. Use one current contract per ownership family: fields/types/ranges, initialization/reset/release, all consumers/callbacks/aliases, binary/device boundaries, temporary adapters and final checks.

- relocation: same operations with moves/renames; check body/name lookup, binding, receiver, linkage, signatures and affected references; reuse prior semantic analysis
- representation: type/layout/storage/receiver/ABI changes; review affected producers/consumers, widths, aliases, lifetime and carried effects
- behavior: numeric/control-flow/order changes; inspect complete relevant originals and follow the authorized original-bug repair contract below

Mixed changes inherit the strongest applicable review. Deleted casts or receiver changes are not automatically editorial. Reopen complete MIPS only for changed/unproved/doubtful contracts; unchanged text alone is not semantic acceptance. Unknown historical names do not block a proven owner: use a qualified role-based name. Unknown lifetime, aliasing, bounds, pointer domains or reachable effects block that family and dependent work; independent work continues.

## Original bug repairs and registry

Refactor authorizes investigation and correction of suspected original game bugs without separate per-fix approval. Establish the suspected defect from complete relevant original logic and its caller/data contracts; use a reproducer or concrete static counterexample when available. Distinguish original defects from translation/refactor regressions. Preserve uncertainty explicitly; suspicion is not proof, and unrequested tuning or guessed fallback is not a bug fix. Shared SDK replacement/device expansion remains governed by AGENTS.md.

Maintain one durable `status/bugfixes.json` registry with schema=1 and a fixes array. Each entry has stable id, status (suspected/applied/verified/reverted), original image/addresses, source files/symbols, symptom, evidence/reproducer, root cause or hypothesis, before/after behavior, rationale, affected contracts, verification results/limitations and rollback description. Record evidence paths/hashes and the applicable revision/body identity; no per-edit source copies. Add the entry before changing behavior, update after checks, and retain rejected/reverted entries. Never label an intentional original-bug repair MIPS-equivalent or transfer its old semantic acceptance unchanged.

Before repairing a defect search the entire registry, including reverted entries, to reuse evidence and avoid reintroducing prior bugs. Link relevant fixes from project context; this registry owns the complete fix catalogue and must survive cleanup. Final refactor reporting lists applied/verified ids and unresolved suspicions. Run verification only at the gates allowed by BUILD_POLICY; an untested fix stays applied/unverified, not verified.

## Implementation sequence

1. Establish final owner/lifecycle and coherent native interfaces from existing reviewed evidence
2. Migrate one dependency-coherent family with every consumer, initialization/reset/release and accurate semantic types/names. Mark structural results unverified and continue; do not polish one module repeatedly
3. Remove game address lookups, scalar DRAM adapters and dual mutable state. Decode actual binary/device boundaries explicitly. Retire the initial EXE loader only when every required default/table has a native owner
4. Perform one exhaustive editorial/module cleanup after interfaces stabilize. Follow naming, numeric, C++ ownership and residual-access contracts linked below; meaningful types/owners must already be established
5. Finish selected x64 configurations/portability while preserving x86. Pointer-safe types and binary/native layout separation must already be correct during family migration
6. Adapt existing fixtures once to stable interfaces, integrate, run the required final checks/scenarios and repair real failures. Preserve known limitations and actual user acceptance scopes

## Invariants

One mutable field has one owner/source of truth. Use typed native structures, arrays and function pointers; native runtime layout need not reproduce PS1 strides/padding. Host pointers/callbacks use pointer-width types. Keep PS1 addresses, packets/tags/OT links, resource/savestate encodings and device fields at their required widths. Never turn PS1 integers into host pointers or bind shared SDK algorithms to game DRAM tables.

Preserve wrapping, fixed-point units, intermediate truncation, signed shifts, saturation, RNG consumption and callback/event order. Native float math is not an automatic replacement. Keep codecs separate from runtime objects; preserve bounds, byte order and lifetimes. Shared PSX/PsyQ behavior belongs to xport; game facts/resources/logic remain game-owned. SDK replacement/device expansion still needs the separate authority in AGENTS.md.

Keep original function provenance. Temporary adapters must identify owner, direction, consumers and removal stage; no permanent mirrors or dual state. Do not add unrequested tuning, guessed fallback or genre policy.

## Scoped tools

Static lexical/AST/dependency/token checks and deterministic transformations are allowed during migration. X refactor_source compares bounded current functions against the existing ZIP with an explicit identifier map; it does not edit source, update SQL or prove runtime equivalence. Unsupported parsing requires scoped AST/manual review, not changing game syntax to satisfy a checker.

Read only affected sections of [REFACTORING_CONTRACTS.md](REFACTORING_CONTRACTS.md): naming, original layout, C++ ownership, residual scalar access, typed conversions, integer simplification or invariant guards. XASSERT conversion needs a reviewed diagnostic-only guard; required Release checks cannot be erased. See [CONTEXT.md](CONTEXT.md#indexing-and-formatting) for safe formatting transfers; no automatic proof transfer across representation/behavior changes.

## Progress

Use `X refactor_progress --baseline BASELINE.zip --source-root src --output status/refactoring/progress.json` with one fixed pre-refactoring scope/archive (ZIP or TAR). Count F address-derived definitions, R scalar-access sites, P `psx_addr` sites and S direct PS1 `SCRATCHPAD` references in both trees: round(100*(1-(F+R+P+S)/(F0+R0+P0+S0))), without clamping; an empty denominator reports unavailable. S excludes unevaluated operands/simple declarations; native `geometry_workspace.scratch` is not scratchpad. This lexical counter tracks no pointer aliases or dynamic operations. Preserve the baseline bytes and source scope. Use reported counts/percent/chat_title; titles show only the rounded integer percent, and the report retains all four remaining counts. Rename on integer changes, or report in chat. These counts do not prove ownership, readability, effort, structural completion or runtime acceptance; the terminal audit remains separate.

## Completion

Audit all authorized requirements against current sources, original mappings and actual final results. Require final native ownership/lifecycles, zero unclassified game-state scalar adapters, removed temporary mirrors/loader, selected architecture support, stable source/module mappings, required builds/scenarios and explicit human gates. Counts and searches alone do not prove ownership or fidelity.

Removed project copies retain reviewed native_retirement metadata (shared_sdk/native_inline/native_virtual/native_unused_interface), reason, current contract and existing evidence; never fabricate a current source/body or original identity. Use the precise retirement schema in REFACTOR_REFERENCE.md. Preserve immutable historical audits; update current legitimate source/symbol bindings and stage project sources with X stage_sources.

When only required human verification remains, stop edits and report that gate without repeated builds. Complete the goal only after every requirement is proved within its actual scope.

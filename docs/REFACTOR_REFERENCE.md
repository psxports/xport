# Refactor workflow details

Task lifecycle: [REFACTOR.md](REFACTOR.md). Read only the affected section. Numeric, layout and ownership contracts: [REFACTORING_CONTRACTS.md](REFACTORING_CONTRACTS.md). Build decisions are owned by [BUILD_POLICY.md](BUILD_POLICY.md).
## Workflow v2: scope and change classes

- Preserve every authorized final requirement. Use one aggregate goal; `native-ready/unverified` is an intermediate structural state, never a delivery or runtime claim. Native ownership, receiver, accurate types and established meaningful names belong to migration; exhaustive editorial cleanup belongs to one planned final pass
- Freeze the current requirement set before bulk edits: language/features, architectures, naming/module rules, ownership families, compile policy, final scenarios and explicit human gates. Later user changes override it; update the affected scope rather than restarting unrelated families
- Classify each family change before editing. `relocation`: moves/renames with the same operations; reuse the previous semantic analysis and check body preservation, name lookup, overload resolution, receiver, linkage, signatures and every affected reference. `representation`: storage/layout/type/receiver/ABI changes; review affected producers/consumers, widths, aliases, lifetime and register-carried effects. `behavior`: operation/control-flow/numeric/order changes; inspect complete relevant originals and follow [original bug repairs and registry](REFACTOR.md#original-bug-repairs-and-registry)
- Reopen full MIPS only when the changed contract is not covered by existing evidence, the body changes semantically, or prior analysis is doubtful. Reuse unchanged reviewed evidence by identity; unchanged text alone never establishes correct binding, behavior or runtime acceptance
- Mixed families inherit the strongest applicable review per changed operation. A receiver conversion, signedness change or deleted cast is not automatically editorial. Never relabel an unresolved representation/behavior change as relocation to bypass its analysis
- Known receiver/state/lifecycle evidence is sufficient for native ownership. Investigate address neighbors only for ambiguous, conflicting or unsupported ownership; distinguish original method recovery from chosen native static placement. Do not reconstruct historical source files as a completion gate
- Unknown historical names or individual flag meanings do not block completion when execution contracts are established: choose a neutral role-based name and record the qualification once. Unknown ownership, pointer domains, sizes/ranges, aliases, lifetime or side effects block the affected family and dependent consumers; independent work continues. Do not investigate hypothetical malformed inputs without a contract reason or hide unresolved native fields in anonymous storage

## Allowed tools and verification policy

Use [BUILD_POLICY.md](BUILD_POLICY.md). Static source/AST/relationship and token comparisons remain available during migration; they are not runtime acceptance.

### Bounded source comparison

For progress accounting, preserve an existing archive's enclosing directory with `refactor_progress --archive-prefix DIR`. Use `--direct-c` only to retain a previously fixed direct `src/*.c` scope; the default includes nested C/C++ implementation files. Apply the same scope to baseline and current sources without rewriting the archive. The formula and category rules belong to [REFACTOR.md](REFACTOR.md#progress).

Reuse the existing recoverable ZIP; do not create per-function snapshots. For a relocation family, create one reviewed identifier map only if needed (`{"OldName":"NewName","OldCall":"Owner::NewCall"}`), then run:

```text
X refactor_source --baseline status/refactoring/before.zip --file src/old_module.cpp --current-file src/new_module.cpp --rename-map status/refactoring/renames.json --output status/refactoring/source-current.json
```

- Substitute actual preserved paths. Repeat `--file`/`--current-file` for all baseline/destination files of this family, including unchanged siblings needed to locate moved bodies; `--archive-prefix DIR` strips a containing ZIP directory. Omit rename-map when no identifiers changed
- No current-file selector scans the chosen current source root to find moves; use explicit destinations for bounded repeated family work. Default source root is `src`; C and C++ out-of-class/free definitions only. Inline class bodies, globals, headers, overload/binding semantics and build inputs still need scoped review
- Report compares indexed baseline bodies to current bodies by explicit `(image,address)`, or unique reviewed symbol for unmarked helpers. It never invents original identities. Review missing/ambiguous matches; do not bind an unrelated body or revert native migration to make this tool green
- Exit `0`: selected function tokens/signatures/markers agree under the declared map. Exit `1`: changed/missing/ambiguous functions need review. Exit `2`: invalid inputs or unsupported parsing. None is semantic/runtime acceptance; even `0` requires lookup, signature/receiver and type-contract review. Never widen rename maps to conceal a representation/behavior change
- Report stays at one current status path; it includes input/body/file hashes and limitations. No source, SQL, build input or historical report is changed. Use the static result to avoid rereading unchanged bodies, not to skip changed contracts. Known parser limitations or unsupported C++ forms use existing AST tools/manual scoped comparison instead of modifying game syntax for the checker

## Current contracts and existing-goal adoption

- Keep one current project plan and one current contract per ownership family (or keyed sections of one existing report). Reuse existing paths and SQL context keys. Each family records owner/init/reset/release, types/ranges, direct/indirect consumers, binary/device boundaries, change classes, symbol/source mappings, qualifications and pending final checks
- State shared rules once per type/format/family and list only meaningful exceptions. Create separate evidence for behavior changes or disputed contracts, not each rename/cast/parenthesis. Keep historical receipts immutable and read them only for a current question
- On adoption read current goal/plan, baseline, current family and direct user restrictions. Do not recreate the goal, baseline, ledger or source tree; do not repeat accepted work or reopen completed refactor/enhance goals solely because this document changed
- Record `workflow_version: 2`, current phase, `compile_policy`, remaining families, deferred editorial work, agreed targets and carryover gates in the existing current plan/context. Preserve all original requirements and review dispositions
- Map completed families to their actual current result without retrospective re-audits; record changed dependencies needing review. Finish the in-flight coherent family under its existing restrictions, then use the new classification/contract rules. Avoid whole-project renaming while another family has unstable interfaces
- For already-native work resume at the remaining cleanup/portability/verification stage. For final verification already in progress continue repairs and affected checks; do not restart structural migration or repeat unchanged passing checks solely for policy adoption
- Make one planned fixture adaptation pass after interfaces stabilize; necessary corrections during final repair remain allowed. Existing user acceptance is scoped evidence, never silently expanded to x64 or other scenarios

## Transition

Before the first refactoring edit:

1. Record the two user statements, baseline source/executable identities, completed coverage evidence, known defects and scoped validation results under the project's `status`
2. Preserve a recoverable pre-refactoring source/configuration baseline and its required original inputs; preserve historical reports and hashes
3. Preserve unrelated jobs and manual game sessions

Original MIPS remains semantic authority.

## Invariants

- One owner and one source of truth per mutable field; record initialization, lifetime, reset and release
- Use typed structures, arrays, references and function pointers. Runtime layouts need not retain PS1 strides, padding or pointer widths
- Decode resource indices/offsets at bounded file/device boundaries. Never cast PS1 integers to host pointers or hide mutable state behind named address lookups
- Keep binary codecs separate from runtime structures; validate lengths, count/capacity, endian, signedness and referenced ranges. Do not serialize host pointers or raw runtime padding
- PsyQ/device fields use fixed-width types (`sint32`/`uint32`, not host `long`); assert boundary sizes/offsets without constraining native runtime structures to historical layouts
- Preserve fixed-point units, truncation, signed shifts, low-word arithmetic, wrapping, saturation, RNG consumption and event/callback order. Native floating-point math is not an automatic substitute
- Preserve original function provenance in markers/reference mappings. Trace metadata for host pointers must identify host pointers
- Common PsyQ types, SDK algorithms/tables, GPU/SPU/GTE behavior and platform synchronization belong to xport. Game constants, resources and logic belong to the consuming project. Never bind a shared SDK algorithm to a game's DRAM table
- Keep device packet tags, OT identifiers, VRAM/SPU addresses and binary offsets where their contracts require them; classify them instead of converting them into game pointers
- Remove the initial DRAM loader only after every required default/table has an explicit native owner and initialization. Preserve shared DRAM required by devices or other projects
- Temporary adapters must name owner, direction, consumers and removal stage; no persistent dual mutable state. Remove all such adapters before completion
- Do not introduce unrelated behavior changes, genre assumptions, performance tuning or guessed fallbacks. Perform x64 migration only in the final implementation stage defined below

## Naming and structure

- Record the target language before edits. C functions/variables/fields use `lower_snake_case`, structure tags/typedefs `UPPER_SNAKE_CASE`; C++ types/free functions/methods use `PascalCase`, variables/fields `lower_snake_case`, implementations `.cpp`. Preserve SDK/ABI names and user-selected features
- Establish meaningful owner/type/function names during migration, without inventing historical semantics. Follow the detailed [naming and structure contract](REFACTORING_CONTRACTS.md#naming-and-structure) for naming changes; apply its exhaustive editorial rules once after interfaces stabilize
- Preserve existing anchor links into this workflow; the technical appendix remains mandatory for the affected scope, not optional cleanup

## Original layout and module reconstruction

Read [the technical contract](REFACTORING_CONTRACTS.md#original-layout-and-module-reconstruction) before changing this area. Reuse unchanged reviewed contracts; do not reload unrelated sections. These requirements remain mandatory within their affected scope.

## C++ ownership reconstruction

Read [the technical contract](REFACTORING_CONTRACTS.md#c-ownership-reconstruction) before changing this area. Reuse unchanged reviewed contracts; do not reload unrelated sections. These requirements remain mandatory within their affected scope.

## Residual scalar-access gate

Read [the technical contract](REFACTORING_CONTRACTS.md#residual-scalar-access-gate) before changing this area. Reuse unchanged reviewed contracts; do not reload unrelated sections. These requirements remain mandatory within their affected scope.

## Typed access, conversions and scopes

Read [the technical contract](REFACTORING_CONTRACTS.md#typed-access-conversions-and-scopes) before changing this area. Reuse unchanged reviewed contracts; do not reload unrelated sections. These requirements remain mandatory within their affected scope.

## Whole-project cleanup passes

Read [the technical contract](REFACTORING_CONTRACTS.md#whole-project-cleanup-passes) before changing this area. Reuse unchanged reviewed contracts; do not reload unrelated sections. These requirements remain mandatory within their affected scope.

## Project plan

Create a dependency-ordered plan under project `status`. Each stage defines objective, owned files/state, all consumers, prerequisites, checks, deliverables and user gate. Use the user-authorized goal lifecycle; if an aggregate goal is active, track stage subgoals in its plan rather than attempting another active goal.

Sequence under one goal; combine steps only when dependencies justify it:

1. Record authorization/baseline, existing translation quality/known defects, final requirements, user restrictions, compile policy and project-specific acceptance scenarios
2. Establish each family's final native owner, types, consumers, lifecycle and semantic module. Review only missing/changed contracts; reuse known evidence
3. Migrate the largest coherent family with all consumers, initialization/reset/release, necessary resource decoding, pointer-safe types for selected architectures, meaningful names and mappings. Mark it structurally migrated/unverified and proceed to dependent families
4. Close residual game DRAM/adapters and the initial EXE loader; classify actual file/device/SDK boundaries. Complete one exhaustive editorial/module-organization pass; meaningful ownership must already be established
5. Finish x64 configurations/remaining portability edits while retaining x86 when in scope. Do not postpone pointer safety or encoded/native layout separation until this step
6. Adapt fixtures to stable final interfaces, run final verification and repair actual failures. Retain all agreed deliverables; structural readiness or cleanup percentage never completes the goal

Inventory assignments, aliases, macros, dynamic bases, callbacks, overlapping views and indirect consumers once per family, then update changed relationships. A scalar-access search alone cannot close ownership. Unresolved reachable effects stay on their affected dependency scope.

### Final implementation stage: x64 alongside x86

- Include Windows x64 portability in every new refactoring plan; retain existing Windows x86 configurations and behavior. An explicit user restriction overrides this default; record the excluded target and do not report the migration complete
- Follow [PLATFORMS.md](PLATFORMS.md). Add Debug/Release x64 configurations where absent, separate architecture intermediates and preserve architecture-switch relinking for shared executable paths
- Audit native pointers, callbacks, pointer arithmetic, integer roundtrips, allocation sizes, alignment and pointer-bearing layouts. Use native pointers or pointer-width types such as `intptr`/`uintptr` for host values; preserve original widths for encoded resources, saves, PS1 addresses, GPU packets and device data. Do not mechanically widen `uint32` fields
- For an original register-sized return that carries either a native pointer or an integer status, use `intptr` across the complete native contract: declaration, definition, local result, caller, callback typedef and dispatch table. Preserve signed status values and explicit 32-bit arithmetic semantics within the operation. A cast to `uintptr` followed by assignment or return through `uint32` still truncates the pointer and is not a fix
- Prefer typed object/function/member pointers when the value is always a pointer; do not replace typed callbacks or C++ member pointers with integer storage. Use `intptr` only where the reviewed mixed pointer/integer contract or an existing API boundary requires it. An API returning the previous native callback, such as `VSyncCallback`, must retain its complete address on x64; guest callback fields retain their original widths
- Distinguish native pointer relocation from encoded offset relocation. Recompute native allocation sizes, alignment and interior-pointer fixups using the final host layouts; decode original 32-bit offsets before constructing host pointers. Do not reinterpret a packed resource as a widened native structure or apply PS1 object sizes to pointer-bearing classes
- Remove pointer truncation and assumptions that native addresses fit below 4 GiB.
- Apply the same verification policy to portability edits; the default defers compilation and execution, with compile-only diagnostics allowed only under the explicitly selected abi-boundaries policy. Carry Debug x86 and Debug x64 builds, architecture-sensitive checks and agreed game scenarios into final verification. Run architectures sequentially when they share executable output paths
- Final verification must cover both native architectures unless the user explicitly excludes one. Use [BUILD_POLICY.md](BUILD_POLICY.md) for Release/Web scope; a passing x64 build does not replace required x86 or runtime checks

## Execution granularity and throughput

- Migrate coherent ownership/lifecycle families, never arbitrary function counts. Include related operations, all consumers and boundary adapters; do not combine unrelated owners merely to enlarge a batch
- Keep one working dependency inventory and reuse unchanged original/source evidence. Do not restart whole-project searches for each field or reread historical receipts for reporting
- Select final method/module placement early and perform deterministic updates together. Exhaustive style cleanup has one planned pass; do not use cosmetic stages as a substitute for native ownership
- Record structurally migrated families as unverified. Do not adapt old fixtures repeatedly to intermediate interfaces; use the stable-interface pass before final verification
- Use historical timings to diagnose costs; do not launch new benchmarks during migration. Parallelize independent reads safely; final checks may run concurrently only with isolated outputs/shared state. No additional agents are authorized
- Internal batches are not user gates.

## Stage execution and validation

Apply [packet/resource access rules](REFACTORING_CONTRACTS.md#packet-and-resource-access) when that boundary changes.

Structural migration:

1. Apply Workflow v2 change classes and the current family contract; consult complete relevant MIPS only for missing/changed semantic facts, and existing shared SDK contracts for boundary decisions
2. Update owned storage, operations, consumers, declarations/build inputs and reference mappings together; preserve markers, numeric semantics and unrelated work. Use static consistency tools under the allowed-tools policy
3. Continue dependent families without runtime gates or intermediate code_refresh; use compile-only diagnostics only when the recorded project policy permits them. Keep unresolved execution contracts and final checks explicit
4. After native ownership/consumers and adapter retirement are complete, mark native-ready/unverified; finish the single editorial pass and required portability work before entering final verification. This status is not completion

Final verification and repair phase:

1. Fix further fixture defects only when final verification reveals them. Add necessary whole-operation checks for codecs, bounds, aliasing, lifetime, pointer exchange, numeric semantics and provenance. Do not weaken tests solely to accept the refactored implementation
2. Run the complete `X code_refresh` with ordinary Debug configured, including applicable indexing/mapping, compilation/build and all configured checks. Do not use `--no-build` or `--no-tests` to claim the full gate passed
3. Run the agreed scenarios against the actual current executable: cold startup; UI/input transitions; gameplay and state changes; finish/results; exit/reentry; save/load; audio/video; repeated sessions and any applicable optional modes/replays. Report the actual observed scope of each scenario
4. Diagnose and repair failures, preserving their historical records. Repeat the affected checks and required integration gates until the agreed final verification succeeds. Reuse valid unchanged evidence; do not substitute a narrow passing check for a broader required one
5. Follow [BUILD_POLICY.md](BUILD_POLICY.md); explicit project/user restrictions override default targets
6. Record final source/executable identities, commands/results, remaining limitations and explicitly required user acceptance. Final verification is a substantive repair stage and may require multiple iterations; structural migration alone does not complete the goal

Original-MIPS reading where required by change class, dataflow/alias analysis, source editing, marker/reference maintenance and static consistency tools are migration work. Executed tests and games remain deferred; compilation follows the explicit project policy. Do not label unverified code tested, equivalent or delivered.

## Evidence and terminal conditions

Source-index retirement after native consolidation:

- Keep original image/address/hash/status and historical source bindings in the translation ledger
- Relocate a retained body to its actual current source and symbol only from explicit original markers or reviewed mapping evidence
- For a removed project copy, record `native_retirement` with `kind` (`shared_sdk`, `native_inline`, `native_virtual` or `native_unused_interface`), nonempty `reason`, `current_contract` and an `evidence` list of existing project files, optionally with fragments. Use `native_unused_interface` for a reviewed removed interface without current callers; do not claim that its operation was inlined or that the original function was dead
- This metadata retires the project source copy, not the original function or its behavioral requirements. Shared SDK replacement and inline/virtual contracts retain their final checks
- The source index records `retired_native_copy` with no source/body/hash. Do not fabricate files, bind unrelated bodies or promote ledger status. A current original marker conflicts with retirement and must fail indexing
- Missing active source paths, invalid retirement metadata or missing evidence fail indexing. Check `PRAGMA foreign_key_check` and preserve ordinary mapping/status behavior when changing the indexer

Use the existing current project/family contracts plus linked historical evidence; do not add a second reporting hierarchy:

- Authorization/baseline; stage plan and acceptance status
- Ownership map with initialization/lifetime, classified binary/device boundaries and temporary-adapter removal list
- Source/executable hashes, applicable original mappings, commands/logs and exact check scopes

Do not reuse stale fixture hashes after edits. Do not rewrite old failures as PASS or delete known limitations. Check terminal state from actual process/session handles, not a leftover status file. When user verification is the only remaining gate, stop edits; do not generate repeated tests/builds to simulate progress. Apply the goal tool's blocked threshold if automatic continuation repeats that gate.

Update the goal to complete only after this audit. Preserve original inputs, README.md, unrelated changes and shared functionality for other projects.

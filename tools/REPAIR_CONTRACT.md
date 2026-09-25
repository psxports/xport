# Declarative MIPS/C repair contracts (schema 1)

The bench uses original instruction words, not IDA pseudocode. It extracts existing native function bodies through the source index. A new supported case is JSON data: no additional Python harness, C harness or solver-written game implementation. The shared harness is generated mechanically. Unsupported instructions, ABI, declarations or memory are explicit limitations, not invitations to guess.

Commands use `X = python -B [XPORT_ROOT]/tools/xport.py --project PROJECT_ROOT`:

```
X repair_bench --contract tools/repair-contracts/CASE.json --output status/repair/CASE --run
X repair_dispatch --contract tools/repair-contracts/CASE.json --output status/repair/index.json
X repair_dispatch --contract tools/repair-contracts/CASE.json --output status/repair/candidate.json --transform
X repair_causal --audit status/repair/CASE --output status/repair/causal.json
X repair_codegen --contract tools/repair-contracts/CASE.json --output status/repair/codegen.json
```

Omitting bench `--run` loads/checks contract inputs and prints case count; it does not compile or execute. Outputs belong in project status/tools. Do not run these commands when tests are deferred. A candidate JSON is not an applied patch.

## Data contract

- `schema: 1`, `kind: "project" | "fixture"`, `name`: C identifier
- Numbers: JSON integers or Python-base-style strings (`"0x80010000"`); arithmetic wraps to 32 bits
- `original.entry`, `original.stops`: entry and explicit return boundaries
- Fixtures provide `original.words`: address-to-instruction-word map, and `native.source`: isolated C text. Production never accepts these as authoritative executable/source inputs
- Production provides `original.image`, `original.functions: [{address, sha256}]` and `original.segments: [{path, sha256, base, offset}]`. The function hash covers concatenated instruction bytes in SQL address order. Every word must match exactly one pinned original image segment whose path/base/offset is a configured IDA image load. Paths stay inside the project
- Production `native.functions: [{image, address, sha256}]` pins exact source-index body hashes. Every mapping must be unique/current; combined or ambiguous ledger regions fail closed. Automatic application requires exactly one function in one existing `src/*.c` file. Hook symbols must also map to their original addresses in the source index
- `native.symbol`, `native.arguments: [{register, type}]`, `native.return_type` (default uint32; void supported), `native.variables` (existing parameter names used by symbolic expressions)
- Argument types: uint32/sint32/uint16/sint16/uint8/sint8, uint32 pointer, sint32 pointer, void pointer. Pointers are mapped into guest RAM. Existing bodies may use shared ff_u8/u16/u32, ff_s8/s16/s32, ff_w8/w16/w32 and ff_ptr helpers. Extra types, global state and arbitrary helper dependencies are not silently synthesized; unresolved compilation needs a supported contract extension
- `ram_size` defaults to 2 MiB (64..2097152); `fill` defaults to zero. `memory_images: [{path, sha256, offset}]` pins RAM seed bytes. `memory: [{address, width, value}]` adds declarative writes; widths 1/2/4
- `registers`: register-number-to-expression map; r0 is always zero. Call entry has no pending load/branch. HI/LO start zero; functions needing incoming HI/LO are outside this contract
- `cases: [{binding: value, ...}]` and/or `axes: {binding: [values...]}` select finite inputs. Rows must have identical binding keys. Default max_cases 256, hard maximum 4096 and 128 MiB aggregate RAM budget. Split large batches
- Expressions are integer literals, named bindings, or `{op, args: [left, right]}`. Ops: add, sub, and, or, xor, shl, shr; shifts mask count with 31. No Python/C expression evaluation
- `hooks: [{address, symbol, arity, return, return_type, writes}]`: explicit call models, arity 0..8. Arguments 0..3 come from a0..a3, additional words from sp+16. The model's expressions use `arg0`..`arg7` and case bindings. Ordered target/argument calls are compared independently of final RAM/result. Hooks are assumptions, not evidence of the original callee's behavior
- `scratch: [[physical_offset, byte_length]]`: excluded scratch storage, explicitly retained in the report. `compare_return` defaults true; use false only when return is outside the declared ABI (e.g. void), with that limitation retained in the contract
- `max_steps`, `max_events`, `max_native_events` bound execution/logging. Native process and compiler have separate 120-second limits. Artifacts retain generated C, binary inputs/outputs, build/native logs, executable and input identities

All RAM bytes outside explicit scratch exclusions, the selected return and ordered hook calls are compared. CPU flags, timing, devices, pixels and PCM are not covered. The extracted restricted interpreter is a diagnostic oracle, not independent certification of PS1 semantics. Exceptions/GTE/MMIO, unaligned/out-of-range RAM and unsupported opcodes fail closed. The production all-channel runner remains mandatory.

## Semantic dispatcher index and deterministic transforms

`dispatch` contains `entry`, `stops`, `selector_register`, `values`, optional `registers`, `memory`, `scratch`, `max_steps`. Symbolic register values are `{variable: "existing_parameter"}` or integers. Symbolic memory entries are `{address, width, value}`; address uses the expression representation, value may be `{selector: true}`. Declarations describe entry assumptions and must have explicit evidence; stage-entry RAM cannot stand in for an unknown later call entry.

The index follows actual MIPS paths for each selector, including signed/unsigned immediates and branch/load delays. Unknown branches, indirect targets, unmodelled reads, observable stores outside the call-only class and unsupported instructions produce `unresolved` rows. Calls clobber volatile symbolic registers. The index is conditional on the ABI/memory models, not a universal dispatcher proof.

`repair` contains:

- `kind`: callback_binding or state_transition
- `selector`, `selector_name`: concrete selector and existing C variable
- `style`: switch or if
- `anchor`: unique exact insertion text in the existing function
- `source_sha256`: pre-edit function body identity
- `transition_target`: mandatory original call target for state_transition

Both transforms require exactly one resolved call and a forwarding return where compared. Callback binding also requires selector == resolved callback address. State transition requires the resolved target == transition_target. Arguments are emitted from symbolic machine effects, never invented from pseudocode. The generated insertion is a new case/conditional before the anchor; arbitrary replacements and new C bodies are forbidden. Finite differential checks protect concrete cases, not every possible input, hence fresh replay still gates acceptance.

The repaired selector must occur in differential inputs; an unexercised insertion is rejected. Non-void automatic repairs must compare the return value. Existing CRLF/LF source conventions are retained during application.

Production `trigger` selects the exact observed failure: `difference_sha256` (canonical JSON hash of first_difference) takes precedence, otherwise `pc` with optional `stage` and actor `state`. A terminal-PC recipe cannot override an earlier difference. Contract registries hold production JSON only; fixtures are never applied to game source.

## Persistent supervisor

The outer converge protocol is schema 1 and is configured under `stage_pipeline.supervisor_protocol`. It emits only revisioned meaningful state transitions and one self-contained `attention.json` per unique failure signature. Human/agent-authored gameplay repairs return through `trace_workflow submit-repair`; its manifest is bound to the issued packet, pre-edit identities, exact branch and ledger rows. The submission wrapper owns fast refresh, progress rendering, provisional replay and the existing full final gate. It never generates gameplay C. The legacy `iterate` entry point is not an independent repair path.

Fast refresh may leave function status stale in the analysis database, so branch closure overlays current image-qualified ledger rows before classifying TODO/WIP boundaries. A follow-up integration repair for an already-WIP causal function may change only its dispatcher or wiring file; it must still declare the exact causal branch and current ledger rows, while the ledger itself remains unchanged. New TODO translations continue to require matching source and ledger changes.

When a terminal guard cannot identify an overlay unambiguously, preserve the original diagnostic and issue an explicit audited observation with `trace_supervisor_protocol refresh-attention --name NAME --image IMAGE --address ADDRESS`. This creates a hashed `causal-observation.json`; it does not infer an image from a colliding address. Fatal WIP evidence is watched while the native child runs so a Windows abort dialog cannot leave a false healthy heartbeat indefinitely.

Configure `stage_pipeline.repair_supervisor` with enabled, contracts_directory, max_repairs, validate_before_replay, validate_on_match and solver `{mode: "codex" | "queue", max_calls, timeout_seconds, home?}`. Prefer validation before replay; enabling both validation timings is rejected. The canonical trace_workflow entry owns the worker and accounting.

For each new failure identity the worker selects one matching registry contract or asks the isolated solver. It runs baseline reproduction, semantic transformation, candidate bench, an owned backup/atomic source insertion, mandatory code_refresh and the original fresh-build replay. A failure must advance both the absolute phase and verified prefix before a partial patch can be retained for the next defect; that state is not MATCH. No progress restores only exact supervisor-owned bytes and refreshes. Foreign edits, uncertain process ownership and interrupted formatting are explicit attention states, never overwritten.

Durable artifacts: workers/NAME/repairs/state.json; per-signature contracts, index/candidate evidence, baseline/candidate-check benches, backup and patch-result.json; completion.json after existing publication/pruning and optional validation. Trace names remain bound to recordings by the existing worker baseline. Repeated identical failures and solver calls have persisted budgets, not an unbounded model loop.

Solver runs `codex exec --sandbox read-only --json` with a structured output schema. It may return a pinned contract, missing observations or unsupported; it may not patch source, translate pseudocode, build, run tests or manipulate emulators. OS read-only sandbox is the mutation boundary; the prompt additionally restricts tools/task scope. CLI availability, configured permissions and transcript availability are environmental prerequisites. Interrupted launch is not duplicated automatically. Logs/usage remain separate; full telemetry discovers solver thread IDs and reports missing transcript gaps.

With validate_before_replay, configured trace_validation runs before replay and retains a hashed input identity. Passed suite results may be reused only with unchanged dependencies and log hash; unknown dynamic dependencies use the full declared identity. A failed pre-replay check stops the workflow before capture. Validation describes those toolset inputs at its recorded time; later gameplay repairs are gated by code_refresh and full fresh-build replay. No validation or live identity scan is performed after MATCH in this mode. Legacy validate_on_match retains its original opt-in behavior. Existing mandatory successful-run pruning remains intact.

The worker passes an explicit existing CODEX_HOME to solver processes. Before replay, preflight verifies that the current worker context can create and remove a temporary file in that existing home, then uses the bounded cached --version check; it never copies credentials or substitutes another home. A read-only home stops before replay with `home_readonly`. Actual structured reasoning is established only by a real defect. Home/auth/CLI errors retain an excerpt and disable repeated solver calls for the same executable/home fingerprint in this trace. To deliberately retry after an external runtime/auth repair, increment solver.runtime_revision (or start the next trace); the worker preserves the failed launch under `failed-runtime-NNNN` and permits one budgeted retry only when the runtime fingerprint changed. A task-identity rejection before child launch releases its reservation rather than consuming a model call. Per-defect launch ownership and max_calls still apply.

After publication and pruning, completion-summary.json records existing MATCH evidence, scope, validation timing, recording aggregates and repair outcome. Configured acceptance manifests receive links under runtime_cycles without promoting unrelated real gates. Telemetry aggregation belongs to its watcher, not the completion critical path.

## Causal evidence and bounded C-generation assessment

For failures outside the two transform classes, the same declarative bench records original instruction/register/read/write/call events and native helper accesses with generated-source line numbers. causal.json identifies the last observed writer of the first different byte and a bounded backward register-value slice. This is localization: missing control dependencies, unchanged-register writes, direct-pointer C accesses and unknown live-entry state are explicitly not a causality proof. Obtaining an exact live function-entry snapshot remains a requested observation when available captured data does not establish it; this release does not install live game watchpoints.

repair_codegen is a feasibility classifier, not a C generator. It inventories the bounded integer subset, delay slots, direct/indirect calls and blockers for each contract. Initial candidates are pure integer leaf functions and RAM-only functions with explicit call ABI. GTE/devices, exceptions, unknown indirect calls and unspecified entry state are rejected. A future deterministic emitter must use guest-width operations and independently validated instruction semantics; it must pass this bench plus fresh full replay. Do not resume agent pseudocode-to-C conversion under the name of generation.

## Deferred acceptance

The consuming project owns fixtures and test suites. First execute on the explicitly authorized next new record -> converge. Separate criteria: declarative baseline mismatch/candidate pass for both transforms; signed-immediate and memory-writer diagnosis; negative identities/unknown branches; transactional restore and solver isolation; unchanged game source for fixture tests; real new-record all-channel MATCH; telemetry including solver; distinct-project and controlled performance evidence. Unit fixtures cannot promote any real gate. Static syntax inspection is not runtime PASS.

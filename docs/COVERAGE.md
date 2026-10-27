# Function coverage: full translation pass, then integration

## Original function recording and timing

Collect the original function list through [FUNCTION_COVERAGE.md](FUNCTION_COVERAGE.md). The `function_coverage start` launcher uses DuckStation's default CPU-driven timing without an extra timing flag. `-standard-timing` remains an explicit emulator alias for this default; it is not an argument to `coverage NAME` or the xport CLI.

`-standard-timing` selects CPU-driven device time before boot, including ordinary interrupt dispatch and scheduled CD/SIO operations. Function discovery uses ordinary disc/BIOS boot and requires no project `duckstation.wait_points`, `sio_delay` or executable injection. GDB registers function targets and drains the first-entry list; it does not select timing through commands.

`coverage NAME` consumes the completed function recording and does not launch DuckStation. Record and converge explicitly select cooperative time through their launchers with `-lockstep-timing`; function discovery does not use that flag.

## Default workflow

Read [DECOMPILATION.md](DECOMPILATION.md), especially [Full-pass translation and batch modules](DECOMPILATION.md#full-pass-translation-and-batch-modules). That section owns module naming, the soft 2000-line threshold, complete-function placement, full-pass scheduling and deferred integration/build rules. Do not duplicate or replace those rules with proof-packet limits.

`coverage NAME` defaults to saving all selected game-function bodies before integration, builds and runtime repair. A source module is not an acceptance or build checkpoint. Strict per-function proof and full-record convergence require an explicit request; they are separate deliverables. More specific user restrictions, including draft-only, no-build and platform/configuration limits, take precedence.

Before translation, inspect the active goal. Reuse it only when its objective already covers the requested project, scope and deliverable; otherwise create a coverage goal with those requirements and the completion conditions below. An unrelated active goal must not be silently replaced or treated as coverage authorization. Keep the coverage objective active across modules, integration and recoverable failures; a saved module or successful background launch is not completion. Mark the goal complete only after verifying the entire requested deliverable, including its required final checks. For an explicitly narrower draft-only or no-build request, use that scope without adding build/runtime requirements.

## Inputs and scope

- Resolve project identity from the current project and preserve immutable original exports, recording, source, ledger and receipts. `NAME` identifies the completed recording under `status/function-coverage/NAME`
- Establish the selected image-qualified set from the original inventory and reviewed SDK classifications. Default full-image coverage includes recorded game functions first and remaining known game functions in the selected image afterward; a user-requested recorded-only or named subset stays narrower. Do not include deferred images implicitly
- Freeze and report both sets separately: recorded game functions `R` and the complete selected translation set `N`. Exclude SDK functions only through reviewed classification. Keep later out-of-scope integration dependencies separately listed rather than silently changing a denominator
- Resume saved usable translations. Existing complete bodies and reviewed shared implementations do not need retranslation merely to adopt this workflow
- Retain one signature registry: image/address, native symbol, return type, semantic arguments, guest offsets versus host pointers, original evidence and unresolved contracts
- Save the concise current scope, ordering, agreed scenario/configurations and completion condition with `X project_context set --key coverage:NAME --kind task --stage coverage`. Save only expensive semantic/integration gaps as question records with an exact next step. Register complete bodies using `project_context decompile` from AGENTS.md; no new native-delivery JSON or per-function diary. Historical delivery/signature/proof artifacts remain evidence, not a second editable current-state store
- Maintain the coverage goal defined above. Never fabricate proof completion, rewrite immutable recording bytes or register partial bodies as decompiled

## Translation phase

The active model owns signatures, complete translations, headers and provenance; do not spawn workers or message other chats without explicit authorization. Follow the canonical decompilation policy for the complete pass and sequential modules. Reuse reviewed original evidence and unchanged bodies; investigate actual ABI, arithmetic and alias uncertainties rather than repeatedly auditing already reviewed code.

Save full bodies through the ordinary source workflow, then register complete audited bodies in project.sqlite without running an indexing/build pipeline. Translation presence does not imply runtime verification. Pending callees remain declarations. Do not make strict supervisor packets or their fixed size the pacing unit for this phase. If the existing proof tooling requires a complete packet, source snapshots or intermediate gates, retain its contract for an explicitly requested strict run; do not bypass validators or spend the ordinary pass extending proof infrastructure.

During translation report saved complete bodies as `T/N`, recorded-subset coverage separately when useful, the active module and actual semantic blockers. A partial body, prototype or fail-fast stub never contributes to `T`. No build/runtime result is expected before the integration phase.

### Chat title

- When the environment supports renaming the current chat, set its title to `coverage: N%` at the start or resumption of coverage
- In the Codex desktop app, use `mcp__codex_app.set_thread_title` with the calculated `title` and omit `threadId` to rename the current chat. Repeat this call when the rounded percentage changes
- Compute the title percentage from registered complete bodies `T` in the frozen selected translation set divided by that set's total, rounded to the nearest whole percent: `floor(100 * T / total + 0.5)`. For example, `378/651` gives `coverage: 58%`. Use the full selected set, not only the recorded subset or the image-wide queue
- Recompute after registering completed bodies and rename the current chat when the rounded percentage changes. Read the counts from the authoritative selected set and translation bindings; drafts, prototypes and stubs do not count
- The title measures translation presence. A title of `coverage: 100%` does not establish integration, build, runtime or equivalence verification and does not authorize completing the goal before its remaining gates pass
- If chat renaming is unavailable, continue coverage and report `T/total` normally. Do not create another chat or a scheduled automation to update the title

## Final integration and repair

After the full pass, perform the integration and verification sequence defined in DECOMPILATION.md. Register the accumulated modules and reconcile callers, callbacks, signatures and shared SDK interfaces together. Prefer existing shared PsyQ services; keep ordinary guest-memory access through standard xport `r_u*`, `r_s*` and `w_u*`, with explicit bounded native-pointer boundaries. Do not invent project-local accessor substitutes.

Use final Debug builds and the agreed real scenario to diagnose remaining failures, optionally under cdb. Check actual rendering/input and requested audio; successful compilation or function execution alone does not establish playable output. Preserve user-owned processes; run owned automation hidden and muted. Preserve shared-runtime consumers when repairing a contract, and record the exact changes and limitations.

Complete the requested final configurations after Debug repair, respecting narrower user restrictions. No routine per-function proof fixtures, full-record replay or proof-tool development is required for ordinary coverage. A specifically requested strict equivalence/converge task retains its own gates.

## Completion and reporting

Completion requires saved full implementations/reviewed replacements for every selected function, resolved integration, successful required final builds and the agreed scenario. A draft-only request ends at its explicitly narrower deliverable. Record current source/executable identities, commands/results, actual tested modes/levels/media and remaining limitations; do not claim original equivalence from these checks.

Report `T/N`, the current phase and the concrete remaining blocker. Use `X project_context queue --image IMAGE` for the binary translation inventory; report the selected R/N denominators separately. If strict `X/Y/Z` proof counts are shown, label them separately. Ordinary completion does not require the strict supervisor's `coverage status` to report proof completion.

If an hour passes without saved complete source or improved integration/runtime evidence, identify the critical dependency and eliminate repeated reads, reviews and premature checks. Retain completed work and continue independent translations when a named semantic gap needs more analysis.

## Explicit strict-audit commands

Historical strict proof metadata remains isolated from the ordinary binary translation queue. The existing supervisor's packets of up to 16 functions, proof validators, transactional allocation, corrective routes and build gates are documented in [COVERAGE_STRICT.md](COVERAGE_STRICT.md). They remain unchanged implementation contracts and do not govern ordinary source-module formation or the full translation pass.

# Acceptance, recovery and interrupted stage work

Normative extension of `../PIPELINE.md`. Read this file only when that contract routes the current task here.

## 11. Acceptance and recovery

Before completion, verify each requested deliverable against files/commands/results; a narrow test cannot prove unrelated scope. Preserve original objective. Report exact limits and unresolved checks; do not close a goal because only easy paths pass.

Archive superseded copies with original→archive paths, sizes/SHA-256, verification and restoration instructions. Validate resolved paths before recursive moves. Preserve user data and trace paths unless the user explicitly authorizes a trace/evidence reset; always preserve explicitly excluded inputs such as selected Save Slots. Never overwrite live settings/states on rollback. Test recovery into a fresh directory before replacing anything. Archives are historical evidence, not alternate maintained code. A toolset upgrade invalidates dependent identities normally; never rewrite historical hashes to manufacture acceptance.

### Interrupted stage work

- Repeating trace_stage_run observes a live worker; observation timeout is not failure. A running receipt with a verified dead worker permits at most two capture recoveries. Check native process absence before touching artifacts; preserve any live/manual game and fail on process-inspection error.
- Recovery archives only direct owned capture outputs/logs/receipts/checkpoints under the same run's recovery/attempt-N. Input manifests stay unchanged. Persist recovery intent before moves and a commit before recreating checkpoint directories; interrupted archive/cleanup resumes idempotently. Both full and narrow captures use this path. Never accept the archived partial capture as MATCH.
- A launch with no proven worker identity remains ambiguous and is not restarted automatically. Inspect ownership explicitly. Converter launch intent likewise prevents duplication; a committed candidate is hash-validated and reused, while an uncommitted interrupted conversion returns ANCHOR_UNSUPPORTED without guessing success.
- Preparation holds an OS lock. Before committing a prepared manifest, an interrupted phase-index directory can be archived and rebuilt, at most twice. Once committed, changed cached artifacts are rejected rather than silently regenerated. Missing/unfinalized traces are checked before native build.

- Cached stage verdicts bind comparison JSON, request, contract manifest and capture receipt by SHA-256, then validate compared channel contents. Changed evidence is rejected; a zero-change resume does not execute native capture again.
- Optional project setting `stage_pipeline.prune_successful_runs=true` replaces each published MATCH run directory with `status/stage-pipeline/completed/RUN_ID.json`. The compact receipt retains the full comparison result, input hash, timings and scope. It removes the heavy native capture/checkpoints and the continuation record whose evidence pointed into that directory. Mismatch, failed capture, unpublished results and ambiguous cleanup are retained. Cleanup intent is journaled before deletion and resumes after interruption. A pruned identical request returns MATCH from the compact receipt; a changed build/tool identity creates a new run.


### Stage pipeline acceptance gate

- Required delivery evidence: ordinary recording with committed stage package -> bounded automatic project conversion -> complete native channel comparison; fresh build continuation; real reset/transition contract; earliest-difference diagnostic; narrow checkpoint replay; interrupted-worker/index recovery; immutable cache rejection; persistent budget exhaustion. Unit tests alone do not replace these real-capture gates.
- Retain a project-local acceptance manifest mapping each requirement to evidence path/hash, current native/emulator identities, tests and exact scope. Keep raw artifacts local. Update progress only after comparison commits; retry publication failures without duplicating captures. Capture/compare timings are separate from preparation/build and cache verification.
- Fail-closed outcomes are part of the contract: unsupported boundaries/device modes, missing historical packages, changed evidence, ambiguous worker ownership. Never turn them into MATCH or an unbounded emulator/bootstrap search. The toolset performs capture/replay/localization; demonstrated game-code fixes still require the authorized project audit workflow.

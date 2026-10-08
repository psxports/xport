# Shared xport toolset

This file is the only agent entrypoint for xport. Resolve `[XPORT_ROOT]` from a consuming project's `xport-project.json`. Define `X` as `python -B [XPORT_ROOT]/tools/xport.py --project PROJECT_ROOT`.

## Mandatory rules

- Never create, edit, move, delete or regenerate any file named `README.md` in xport or a consuming project
- Keep xport-owned Markdown documentation in `[XPORT_ROOT]/docs`; `AGENTS.md`, `CHANGELOG.md` and the immutable root `README.md` are the only root exceptions
- Write documentation for agents: use explicit commands, inputs, outputs, invariants, failure conditions and terminal conditions; remove narrative, duplication, optional wording and contradictory rules
- Put reusable tools in `tools`, the shared PSX/PsyQ host runtime in `src`, and game facts, adapters, generated artifacts and evidence in the consuming project
- Resolve project identity and paths from `xport-project.json`; never infer them from a source path or game name
- For an existing consuming project, use the nearest ancestor of the current working directory that contains `xport-project.json`, or an explicit project path from the user. If neither identifies one project, ask for its path; never select a sibling from its name, prior status or available artifacts
- Preserve unrelated work, original inputs, third-party licenses, upstream identities and source hashes
- Treat original MIPS bytes as semantic authority. Use IDA pseudocode as a structural baseline only when it exists and is complete. Missing or partial pseudocode is normal: record `pseudocode_complete: false` and `pseudocode_used_as_baseline: false`, then audit the complete MIPS
- During converge, reject hacks: no frame, time, address, input, value or observed-case compensation; no difference masking, event suppression or reproducer special case; edit only required audited functions whose complete original evidence proves the replacement
- Write brief code comments, start them with a capital letter and omit the final period

## Task router

- `startup FULL_NAME SHORT_NAME`, project creation, ownership, layout, build gates and music conversion: read [docs/STARTUP.md](docs/STARTUP.md)
- `record SLOT NAME`, `stop` and capture validation: read [docs/RECORDING.md](docs/RECORDING.md)
- `function_converge NAME` and its matching `stop`: read [docs/FUNCTION_COVERAGE.md](docs/FUNCTION_COVERAGE.md)
- `boost NAME` or `boost` with an explicitly supplied function set: read [docs/BOOST.md](docs/BOOST.md); run one fast three-agent draft translation pass without a goal, audit, build or runtime workflow
- `coverage NAME`: create an active goal, then read [docs/COVERAGE.md](docs/COVERAGE.md); process evidence-bound batches of 16 in first-call order until the final Release build passes, then mark the goal complete
- `converge NAME`: create an active goal, then read [docs/CONVERGENCE.md](docs/CONVERGENCE.md); keep the same supervisor alive and under event-driven observation until full-record `MATCH`, required user input or an explicit user stop; mark the goal complete only after verified `MATCH`
- Manual translation or repair of decompiled C: read [docs/DECOMPILATION.md](docs/DECOMPILATION.md)
- Lockstep protocols and repair artifacts: read [docs/LOCKSTEP.md](docs/LOCKSTEP.md)
- Device evidence: read [docs/DEVICE_PROVENANCE.md](docs/DEVICE_PROVENANCE.md)
- DuckStation deployment and provenance: read [docs/DUCKSTATION.md](docs/DUCKSTATION.md) and [docs/DUCKSTATION_SOURCES.md](docs/DUCKSTATION_SOURCES.md)
- Shared library structure: read [docs/LIBRARY.md](docs/LIBRARY.md)
- Web builds and packaging: read [docs/WEB.md](docs/WEB.md)

Read only the documents routed by the current task. Do not preload old chats, complete logs, unrelated evidence or the whole documentation tree.

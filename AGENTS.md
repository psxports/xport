# Shared xport toolset

This file is the only agent entrypoint for xport. Resolve `[XPORT_ROOT]` from a consuming project's `xport-project.json`. Define `X` as `python -B [XPORT_ROOT]/tools/xport.py --project PROJECT_ROOT`.

## Mandatory rules

- Present results in Markdown; do not use Canvas for presentation
- Startup may create a new consuming project `README.md` once from `tools/templates/project/README.md.in`; refuse an existing README before initialization. The explicit `commit decomp` command may only comment the Play Game anchor as defined in `docs/COMMIT.md`. Otherwise never create, edit, move, delete or regenerate any file named `README.md` in xport or a consuming project
- Keep xport-owned Markdown documentation in `[XPORT_ROOT]/docs`; `AGENTS.md`, `CHANGELOG.md` and the immutable root `README.md` are the only root exceptions
- Write documentation for agents: use explicit commands, inputs, outputs, invariants, failure conditions and terminal conditions; remove narrative, duplication, optional wording and contradictory rules
- Put reusable tools in `tools`, the shared PSX/PsyQ host runtime in `src`, and game facts, adapters, generated artifacts and evidence in the consuming project
- Reuse existing SDK-reviewed xport PsyQ APIs; coverage/converge does not authorize parallel SDK implementations, duplicate `*_guest.c` layers or rewriting the runtime for guest RAM equality. Follow [docs/DECOMPILATION.md#reuse-the-existing-xport-sdk-runtime](docs/DECOMPILATION.md#reuse-the-existing-xport-sdk-runtime); obtain separate explicit authorization before replacing shared SDK implementations or expanding device emulation
- Resolve project identity and paths from `xport-project.json`; never infer them from a source path or game name
- For an existing consuming project, use the nearest ancestor of the current working directory that contains `xport-project.json`, or an explicit project path from the user. If neither identifies one project, ask for its path; never select a sibling from its name, prior status or available artifacts
- Preserve unrelated work, original inputs, third-party licenses, upstream identities and source hashes
- Use standard xport `r_u8/r_u16/r_u32`, `r_s8/r_s16/r_s32` and `w_u8/w_u16/w_u32` for scalar guest-memory access; do not introduce project-local replacement accessors or lvalue macros such as `P2_U32`; use `psx_addr` only for an explicit native pointer ABI boundary with the required mapped span
- Treat original MIPS bytes as semantic authority. Use IDA pseudocode as a structural baseline only when it exists and is complete. Missing or partial pseudocode is normal: record `pseudocode_complete: false` and `pseudocode_used_as_baseline: false`, then audit the complete MIPS
- During converge, reject hacks: no frame, time, address, input, value or observed-case compensation; no difference masking, event suppression or reproducer special case; edit only required audited functions whose complete original evidence proves the replacement
- Start native refactoring only on explicit user request after explicit user confirmation of full main-functionality coverage; refactored code is outside converge acceptance. Follow [docs/REFACTORING.md](docs/REFACTORING.md); never initiate this transition during translation, coverage or converge
- Write brief code comments, start them with a capital letter and omit the final period

## Task router

- `commit decomp`: read [docs/COMMIT.md](docs/COMMIT.md); batch commit/push initialized sibling projects only, with the exact message `decomp progress`
- `startup FULL_NAME SHORT_NAME`, project creation, ownership, layout, build gates and music conversion: read [docs/STARTUP.md](docs/STARTUP.md)
- `record SLOT NAME`, `stop` and capture validation: read [docs/RECORDING.md](docs/RECORDING.md)
- `function_converge NAME` and its matching `stop`: read [docs/FUNCTION_COVERAGE.md](docs/FUNCTION_COVERAGE.md)
- `boost NAME` or `boost` with an explicitly supplied function set: read [docs/BOOST.md](docs/BOOST.md); run one fast three-agent draft translation pass without a goal, audit, build or runtime workflow
- `coverage NAME`: read [docs/COVERAGE.md](docs/COVERAGE.md) and its canonical translation rules in [docs/DECOMPILATION.md](docs/DECOMPILATION.md#full-pass-translation-and-batch-modules); complete the selected function pass before integration/builds. Preserve saved bodies/proof history and separate translation counts from proof counts. No workers without explicit authorization; strict gates only for an explicitly requested strict deliverable
- `converge NAME`: create an active goal, then read [docs/CONVERGENCE.md](docs/CONVERGENCE.md); keep the same supervisor alive and under event-driven observation until full-record `MATCH`, required user input or an explicit user stop; mark the goal complete only after verified `MATCH`
- Manual translation or repair of decompiled C: read [docs/DECOMPILATION.md](docs/DECOMPILATION.md)
- User-initiated native/readability refactoring after confirmed full main-functionality coverage: read [docs/REFACTORING.md](docs/REFACTORING.md); verify its entry gate before planning or changing the state model
- Lockstep protocols and repair artifacts: read [docs/LOCKSTEP.md](docs/LOCKSTEP.md)
- Device evidence: read [docs/DEVICE_PROVENANCE.md](docs/DEVICE_PROVENANCE.md)
- Regional PS-X EXE analysis and automated function/data matching: read [docs/REGION_COMPARE.md](docs/REGION_COMPARE.md)
- DuckStation deployment and provenance: read [docs/DUCKSTATION.md](docs/DUCKSTATION.md) and [docs/DUCKSTATION_SOURCES.md](docs/DUCKSTATION_SOURCES.md)
- Graphics enhancement, OG/HD migration and GitHub `enhance` tasks: read [docs/ENHANCE.md](docs/ENHANCE.md), then [docs/VID.md](docs/VID.md) for exact API contracts
- Windows x86/x64, AP platform flags, architecture selection and x86-only converge constraints: read [docs/PLATFORMS.md](docs/PLATFORMS.md)
- Shared library structure: read [docs/LIBRARY.md](docs/LIBRARY.md)
- Web builds and packaging: read [docs/WEB.md](docs/WEB.md)

Read only the documents routed by the current task. Do not preload old chats, complete logs, unrelated evidence or the whole documentation tree.
# Build policy for coverage and converge

- Ordinary coverage: no builds during the translation pass; integrate and build Debug after the complete pass, following docs/DECOMPILATION.md
- Converge and explicitly requested strict coverage retain their documented Debug gates
- Do not run Release builds for batches, retries, or intermediate corrections
- Run one Release build after the task is complete as its final verification, or when the user explicitly requests Release

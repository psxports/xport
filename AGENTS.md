# Shared xport toolset

This file is the only agent entrypoint for xport. Resolve `[XPORT_ROOT]` from a consuming project's `xport-project.json`. Define `X` as `python -B [XPORT_ROOT]/tools/xport.py --project PROJECT_ROOT`.

## Mandatory rules

- Present results in Markdown; do not use Canvas for presentation
- Startup may create a new consuming project `README.md` once from `tools/templates/project/README.md.in`; refuse an existing README before initialization. Otherwise never create, edit, move, delete or regenerate any file named `README.md` in xport or a consuming project
- Keep xport-owned Markdown documentation in `[XPORT_ROOT]/docs`; `AGENTS.md`, `CHANGELOG.md` and the immutable root `README.md` are the only root exceptions
- Write documentation for agents: use explicit commands, inputs, outputs, invariants, failure conditions and terminal conditions; remove narrative, duplication, optional wording and contradictory rules
- Put reusable tools in `tools`, the shared PSX/PsyQ host runtime in `src`, and game facts, adapters, generated artifacts and evidence in the consuming project
- Reuse existing SDK-reviewed xport PsyQ APIs; coverage/converge does not authorize parallel SDK implementations, duplicate `*_guest.c` layers or rewriting the runtime for guest RAM equality. Follow [docs/DECOMPILATION.md#reuse-the-existing-xport-sdk-runtime](docs/DECOMPILATION.md#reuse-the-existing-xport-sdk-runtime); obtain separate explicit authorization before replacing shared SDK implementations or expanding device emulation
- Replace identified fullscreen movie players with shared `xport_fmv_*` calls instead of decompiling their internal PsyQ playback loops; preserve game-specific setup and teardown. Follow [docs/DECOMPILATION.md#fullscreen-movie-playback](docs/DECOMPILATION.md#fullscreen-movie-playback); this rule does not cover MDEC texture/block decoding or in-scene video
- Web builds must use prepared MP4/WebCodecs through `xport_fmv_*` for supported standalone movies, including fullscreen and in-scene texture movies. Set and verify `video.web_enabled`, source registrations and packaging; do not ship their original STR files or defer this requirement to enhance. Original STR paths remain API/manifest keys, not Web payloads. Keep embedded level streams and isolated MDEC blocks on their existing path; follow [docs/FMV.md#web-movie-policy](docs/FMV.md#web-movie-policy)
- Read game assets from extracted `bin/DATA` through `xport_fopen` and the shared file APIs; replace original game CD file-read/seek/readiness wrappers during decompilation instead of reproducing PsyQ drive polling. Never use disc images as runtime data sources. Follow [docs/DECOMPILATION.md#runtime-file-access](docs/DECOMPILATION.md#runtime-file-access)
- Resolve project identity and paths from `xport-project.json`; never infer them from a source path or game name
- For an existing consuming project, use the nearest ancestor of the current working directory that contains `xport-project.json`, or an explicit project path from the user. If neither identifies one project, ask for its path; never select a sibling from its name, prior status or available artifacts
- Preserve unrelated work, original inputs, third-party licenses, upstream identities and source hashes
- Use standard xport `r_u8/r_u16/r_u32`, `r_s8/r_s16/r_s32` and `w_u8/w_u16/w_u32` for scalar guest-memory access; do not introduce project-local replacement accessors or lvalue macros such as `P2_U32`; use `psx_addr` only for an explicit native pointer ABI boundary with the required mapped span
- Treat original MIPS bytes as semantic authority. Use IDA pseudocode as a structural baseline only when it exists and is complete. Missing or partial pseudocode is normal: record `pseudocode_complete: false` and `pseudocode_used_as_baseline: false`, then audit the complete MIPS
- During converge, reject hacks: no frame, time, address, input, value or observed-case compensation; no difference masking, event suppression or reproducer special case; edit only required audited functions whose complete original evidence proves the replacement
- Start native refactoring only on explicit user request after explicit user confirmation of full main-functionality coverage; refactored code is outside converge acceptance. Follow [docs/REFACTORING.md](docs/REFACTORING.md); never initiate this transition during translation, coverage or converge
- Write brief code comments, start them with a capital letter and omit the final period

## Compact project context for every stage

- Before startup continuation, coverage, decompilation, converge, refactoring or enhance, run `X project_context show`. For a function use `X query 0xADDRESS --image IMAGE`; for a subsystem use `X project_context show --subsystem NAME`. Read full linked evidence only for the current question. Missing context means unknown
- Current context and complete-body bindings live in `status/project.sqlite`; `analysis.sqlite` remains rebuildable. Reads do not initialize storage; `X project_context init` does. Keep only current task scope, useful findings/checks and expensive open questions; never maintain a diary or load all history
- Save concise context with `X project_context set --key KEY --kind task|finding|check|question --summary TEXT --scope TEXT --next-step TEXT`; optional selectors are `--stage STAGE`, `--image IMAGE --address 0xADDRESS`, `--subsystem NAME`, `--configuration TEXT`. Repeat `--evidence PATH` for exact report/source/executable/configuration dependencies; shared paths use `@xport/src/...`. Findings/checks require scope and evidence. Text fields are capped at 1200 characters. Reuse a key to update; `--closed` hides resolved questions. Use `set --help` for flags
- `show --key KEY` reads one record; the same selectors narrow context. Default limit 8, maximum 32; `--after NEXT` continues. Global task scope accompanies focused queries. Results distinguish unchanged references, changed references needing review and unbound claims; unchanged hashes never grant semantic approval or bypass an authorization/proof gate
- `X project_context queue --image IMAGE [--state uncovered|decompiled|sdk]` returns counts and a bounded queue (`--limit N`, `--after NEXT`). Ordinary game accounting has only uncovered or decompiled bodies. Reviewed SDK identities are separate candidates for existing APIs; verify caller ABI. Existing source candidates are shown without assuming completeness
- After saving a complete MIPS-audited body run `X project_context decompile --image IMAGE --address 0xADDRESS --source src/batch_1.c --symbol SYMBOL --complete --evidence AUDIT_PATH`. This saves original/body hashes without building or refreshing indexes. The exact image-qualified marker is required. `--complete` attests semantic completeness; partial bodies, prototypes and stubs do not count. Rebind existing complete bodies from their audits rather than retranslating or promoting old WIP labels
- Changed bodies require review/rebinding; refactoring updates source/symbol provenance. Verification stays separate. TODO/WIP/DONE/SKIP in legacy exports/strict adapters are historical protocol metadata, not an agent-maintained progress ladder. Do not generate or maintain `progress.html`; `render_progress` only returns the SQL queue
- `X query 0xADDRESS --image IMAGE --view callers|callees|data_refs|indirect|reuse_peers --limit 8 [--fields COLUMN,...] [--cursor NEXT]` pages relationships; restart after analysis refresh. `--view sdk` shows classification. `--audit-context` retains the full-audit contract; summaries never replace complete MIPS

## Task router

- `startup FULL_NAME SHORT_NAME`, project creation, ownership, layout, build gates and music conversion: read [docs/STARTUP.md](docs/STARTUP.md)
- `record SLOT NAME`, `stop` and capture validation: read [docs/RECORDING.md](docs/RECORDING.md)
- `function_coverage NAME` and its matching `stop`: read [docs/FUNCTION_COVERAGE.md](docs/FUNCTION_COVERAGE.md)
- `coverage NAME`: create an active goal before translation, or reuse an active goal that already covers this request; read [docs/COVERAGE.md](docs/COVERAGE.md) and its canonical translation rules in [docs/DECOMPILATION.md](docs/DECOMPILATION.md#full-pass-translation-and-batch-modules). Complete the selected function pass before integration/builds; mark the goal complete only after the requested coverage deliverable and its required checks are verified. Preserve saved bodies/proof history and separate translation counts from proof counts. No workers without explicit authorization; strict gates only for an explicitly requested strict deliverable
- `converge NAME`: create an active goal, then read [docs/CONVERGENCE.md](docs/CONVERGENCE.md); keep the same supervisor alive and under event-driven observation until full-record `MATCH`, required user input or an explicit user stop; mark the goal complete only after verified `MATCH`
- Manual translation or repair of decompiled C: read [docs/DECOMPILATION.md](docs/DECOMPILATION.md)
- User-initiated native/readability refactoring after confirmed full main-functionality coverage: read [docs/REFACTORING.md](docs/REFACTORING.md), workflow v2; verify its entry gate, then create an active goal for the complete authorized migration and final verification, or reuse an active goal covering that scope, before changing the state model. Existing goals adopt in place. Classify relocation/representation/behavior changes, reuse covered analysis, allow static consistency tools, preserve explicit compilation restrictions and read technical appendix sections only for the affected scope. Keep the goal active through all stages and final repair; complete it only after the documented terminal audit
- Lockstep protocols and repair artifacts: read [docs/LOCKSTEP.md](docs/LOCKSTEP.md)
- Device evidence: read [docs/DEVICE_PROVENANCE.md](docs/DEVICE_PROVENANCE.md)
- Regional PS-X EXE analysis and automated function/data matching: read [docs/REGION_COMPARE.md](docs/REGION_COMPARE.md)
- Standalone PsyQ SDK catalog, symbol recognition and Ghidra comparison: read [docs/PSYQ_ANALYZER.md](docs/PSYQ_ANALYZER.md)
- DuckStation deployment and provenance: read [docs/DUCKSTATION.md](docs/DUCKSTATION.md) and [docs/DUCKSTATION_SOURCES.md](docs/DUCKSTATION_SOURCES.md)
- Graphics enhancement, OG/HD migration and GitHub `enhance` tasks: read [docs/ENHANCE.md](docs/ENHANCE.md), then [docs/VID.md](docs/VID.md) for exact API contracts; for fullscreen movies and registered Web video, read [docs/FMV.md](docs/FMV.md)
- Windows x86/x64, AP platform flags, architecture selection and x86-only converge constraints: read [docs/PLATFORMS.md](docs/PLATFORMS.md)
- Shared library structure: read [docs/LIBRARY.md](docs/LIBRARY.md)
- Web builds and packaging: read [docs/WEB.md](docs/WEB.md)

Read only the documents routed by the current task. Do not preload old chats, complete logs, unrelated evidence or the whole documentation tree.
# Build policy for coverage and converge

- Ordinary coverage: no builds during the translation pass; integrate and build Debug after the complete pass, following docs/DECOMPILATION.md
- Converge and explicitly requested strict coverage retain their documented Debug gates
- Do not run Release builds for batches, retries, or intermediate corrections
- Run one Release build after the task is complete as its final verification, or when the user explicitly requests Release

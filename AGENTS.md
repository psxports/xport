# Xport agent entrypoint

Read this file before any xport project. Resolve PROJECT_ROOT from the nearest ancestor containing xport-project.json or an explicit user path; never select a sibling from its name/history. Resolve [XPORT_ROOT] from that config. X means `python -B [XPORT_ROOT]/tools/xport.py --project PROJECT_ROOT`.

## Shared rules

- Preserve unrelated work, original/accepted inputs, licenses and user-owned processes. Existing README.md files are immutable: startup may create its new template once; otherwise only stage an existing README without editing/regenerating it
- Put reusable tools in tools, shared PSX/PsyQ runtime in src, agent Markdown in docs, and game facts/resources/provenance in the consuming project. Use one state owner; no parallel SDK/device implementation or replacement without separately authorized evidence-based scope
- Reuse reviewed xport SDK APIs and validate the actual caller boundary. Do not translate identified SDK bodies already implemented here; a separately authorized suspected defect needs concrete divergent evidence. Shared changes must preserve other consumers
- Original MIPS bytes are semantic authority. Complete available pseudocode is a structural baseline; missing/partial output requires complete MIPS work and explicit completeness metadata. No guessed algorithms, representative paths or claims beyond the checked scope. Intentional original-game bug fixes during refactor follow its [repair registry contract](docs/REFACTOR.md#original-bug-repairs-and-registry)
- Guest scalar access uses shared r_u*/r_s*/w_u*. psx_addr is only a bounded native-pointer boundary. No per-project replacement accessors. Native-state migration requires the refactor entry gate
- Use extracted bin/DATA through shared file APIs; no native/Web disc-image fallback. During decomp replace card saving/UI and movie players through [DECOMP_REFERENCE.md](docs/DECOMP_REFERENCE.md#authorized-save-and-movie-replacements). Supported Web movies require prepared MP4/WebCodecs before delivery
- Before a stage read current context through X project_context show; follow [CONTEXT.md](docs/CONTEXT.md) for scoped reads, bindings, formatting and indexing. Current findings go there, not into shared API docs or per-edit diaries
- All build timing/configuration and final execution rules are owned by [BUILD_POLICY.md](docs/BUILD_POLICY.md). Do not invent extra batch gates or repeat a build without a new reason
- Before any Git/GitHub operation read the mandatory decision procedure in [HOST_TOOLS.md](docs/HOST_TOOLS.md#git-and-github-mandatory-route). Git transport uses Plink/Pageant; gh credentials are not a Git prerequisite. Use shared host binaries; a missing PATH entry does not authorize downloads
- Before a consuming-project stage completes, X stage_sources must stage all src additions/edits/deletions and existing/tracked root cover.png/README.md, including ignored source files. It preserves unrelated staged work; no commit/push is implied. Successful code_refresh already includes staging
- At stage boundaries remove owned obsolete outputs/snapshots/logs/build files after checking exact paths, dependencies and active writers. Preserve current databases/plans, canonical inputs, required recoverable baseline and unresolved-check artifacts. Historical summaries/hashes can remain when obsolete raw inputs are removed; mark those inputs unavailable. No same-disk backup copies, whole-tree guesses or bypass of approval-review rejections
- Present results in Markdown, not Canvas. Write brief code comments with initial capitals and no final period

## Task router

Agent commands create/reuse one covering goal and execute the entire authorized stage, including final checks; a helper response, plan or partial module is not completion. No subagents or messages to other chats without explicit user authorization. Respect narrower user scope and existing completed work.

- startup FULL_NAME SHORT_NAME -> [STARTUP.md](docs/STARTUP.md)
- decomp or original game-code repair -> [DECOMP.md](docs/DECOMP.md); full accepted static inventory, decomp: n% title
- refactor -> [REFACTOR.md](docs/REFACTOR.md); verify explicit human main-functionality coverage and migration authorization before ownership changes
- enhance -> [ENHANCE.md](docs/ENHANCE.md); numbered plan and enhance: n% from completed items
- Shared runtime/API work -> [LIBRARY.md](docs/LIBRARY.md), then only the affected subsystem section
- Renderer API -> [VID.md](docs/VID.md); Web packaging -> [WEB.md](docs/WEB.md); movies -> [FMV.md](docs/FMV.md)
- Native architecture/XCRT -> [PLATFORMS.md](docs/PLATFORMS.md)
- SDK catalog/recognition -> [PSYQ_ANALYZER.md](docs/PSYQ_ANALYZER.md); regional originals -> [REGION_COMPARE.md](docs/REGION_COMPARE.md)
- Manual original-game emulator -> [DUCKSTATION.md](docs/DUCKSTATION.md); source/build provenance -> [DUCKSTATION_SOURCES.md](docs/DUCKSTATION_SOURCES.md)
- Documentation/tool cleanup -> [DOCS.md](docs/DOCS.md)

Read the selected stage contract in full, then technical sections only when needed. Never preload all references, logs, chats or history. Unknown evidence remains unknown.

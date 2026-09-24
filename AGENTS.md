# Shared xport toolset

Maintain reusable implementation in this repository; update `tools/PIPELINE.md` whenever commands, formats, behavior or assumptions change.
Add a new immutable UTC revision to `CHANGELOG.md` for every shared change that can affect a consuming project. Keep Upgrade, Automation and Verify instructions concrete and compact so `X upgrade` never needs repository-history discovery.
Read `tools/PIPELINE.md` before applying this toolset to a new project. Resolve project context from `xport-project.json`, never from the shared source path or a guessed game name.
Project initialization must define a quoted project-specific `WND_TITLE` in every native build configuration; the shared runtime has no fallback title.
Native Win32 executables use the Windows subsystem and the shared platform `WinMain`; do not override the CRT entry point. Launching a game must not create a console window, while redirected stdout/stderr remain available to automated trace and converge logs.
Win32 timer initialization must call `timeBeginPeriod(1)` before frame pacing uses `Sleep`, retain matching `timeEndPeriod(1)` at shutdown, and use `QueryPerformanceCounter` for measurements.
Debug builds are permitted during diagnosis, implementation, `code_refresh` and converge. Before declaring a task complete, run `X build_native --configuration Release`; the final delivered native executable must be the Release build, and a failed Release build leaves the task incomplete.
Generated reports, tests, traces, images, databases, runtime settings/states and build outputs belong in the consuming project's status or tools directory. Game sources and native build outputs follow that project's layout.
Reserve unique GDB ports at initialization, including separate runtime roles. Verify listener/process identity before control. Preserve manual games and never restart an active job on an observation timeout.
Keep game addresses, ABI assumptions, layouts and validated exclusions in explicit project profiles/adapters. Do not copy evidence-backed completion claims or SDK classifications between games.
Preserve DuckStation/third-party licenses and upstream/source hashes. Change shared emulator source here, build into a project-owned snapshot, validate, then update distribution metadata and the full patch.
Project compatibility launchers contain no independent implementation. Historical snapshots remain evidence, not alternate maintained sources.
Retired shared commands live under `tools/archive` with a hash manifest and restoration note. Do not add archived directories to `tools/xport.py` dispatch or restore a retired command for new work. Check Python imports and project configuration before retiring another file; current libraries may retain historical names.
Write brief comments, start with a capital letter, omit the final period.
Maintain one shared executable/DLL/resource installation; project deployments contain only isolated data/settings. Never copy shared implementations into a game. Improve `tools/PIPELINE.md` across projects; game-specific facts remain in their AGENTS.md.
Treat every `*.md` file as agent-only context. Include only actionable instructions, invariants, commands, required evidence and failure conditions. Remove narrative, repetition, introductions, conclusions and rationale that does not affect agent decisions; keep every instruction complete and unambiguous.

# Shared xport tools

Maintain reusable implementation here; update PIPELINE.md whenever commands, formats, behavior or assumptions change.
Read PIPELINE.md before applying these tools to a new project. Resolve project context from xport-project.json, never from the shared source path or a guessed game name.
Generated reports, tests, traces, images, databases, runtime settings/states and build outputs belong in the consuming project's status or tools directory. Game sources and native build outputs follow that project's layout.
Reserve unique GDB ports at initialization, including separate runtime roles. Verify listener/process identity before control. Preserve manual games and never restart an active job on an observation timeout.
Keep game addresses, ABI assumptions, layouts and validated exclusions in explicit project profiles/adapters. Do not copy evidence-backed completion claims or SDK classifications between games.
Preserve DuckStation/third-party licenses and upstream/source hashes. Change shared emulator source here, build into a project-owned snapshot, validate, then update distribution metadata and the full patch.
Project compatibility launchers contain no independent implementation. Historical snapshots remain evidence, not alternate maintained sources.
Retired shared commands live under tools/archive with a hash manifest and restoration note. Do not add archived directories to xport.py dispatch or restore a retired command for new work. Check Python imports and project configuration before retiring another file; current libraries may retain historical names.
Write brief comments, start with a capital letter, omit the final period.
Maintain one shared executable/DLL/resource installation; project deployments contain only isolated data/settings. Never copy shared implementations into a game. Improve PIPELINE.md across projects; game-specific facts remain in their AGENTS.md.

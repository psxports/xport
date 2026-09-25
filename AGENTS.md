# Shared xport toolset

Read `tools/PIPELINE.md` before using or changing the toolset. Keep that file a minimal task router; put operation-specific contracts in the focused document it links.

- Reusable tools belong in `tools`; the mandatory PSX/PsyQ host runtime belongs in `src`; game facts, adapters and evidence stay in consuming projects
- Update the affected focused documentation whenever a command, format, behavior or assumption changes
- Add one immutable UTC revision to `CHANGELOG.md` for every shared change that can affect consumers; keep upgrade, automation and verification steps executable without history discovery
- Resolve project context from `xport-project.json`, never from a source path or guessed game name
- Preserve unrelated work, third-party licenses, upstream identities and source hashes. Historical snapshots are evidence, not alternate maintained implementations
- Keep one shared DuckStation executable/DLL/resource installation; projects own only isolated data, settings and runtime artifacts
- Fail closed on ambiguous process, port, artifact or project ownership. Observe the same active job after a timeout and never interrupt manual play
- Generated reports, traces, databases, settings, states and builds belong in the consuming project, not the shared source tree
- Native task completion requires the Release gate defined by `tools/PIPELINE.md`
- Write brief comments, start with a capital letter and omit the final period
- Treat every Markdown file as agent context: retain only actionable rules, commands, evidence pointers and failure conditions; remove narrative, duplication and completed-run history

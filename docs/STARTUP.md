# Startup

Agent command: `startup FULL_NAME SHORT_NAME`. This contract owns stage execution; [STARTUP_REFERENCE.md](STARTUP_REFERENCE.md) supplies only the technical section needed by an attention/failure. Dependencies are in [HOST_TOOLS.md](HOST_TOOLS.md); build decisions in [BUILD_POLICY.md](BUILD_POLICY.md).

## Entry and goal

Use the current requested project directory. Both names must be explicit user inputs; never derive or normalize SHORT_NAME. Refuse an existing xport-project.json with STARTUP_ALREADY_INITIALIZED before initialization or a new goal. Validate names/template availability through startup.py.

For a new project, create one durable goal for completed setup/analysis/handoff and dispatch exactly one supervisor:

```text
python -B [XPORT_ROOT]/tools/startup.py start --project PROJECT_ROOT --name "FULL_NAME" --short-name SHORT_NAME
```

The worker is detached. Keep the same monotonic state and goal across continuation. Do not repeat deterministic steps manually, launch parallel workers or treat dispatch as completion.

## Supervisor protocol

```text
python -B [XPORT_ROOT]/tools/startup.py status --project PROJECT_ROOT --after-revision REVISION
python -B [XPORT_ROOT]/tools/startup.py submit --project PROJECT_ROOT
python -B [XPORT_ROOT]/tools/startup.py retry --project PROJECT_ROOT --expected-revision REVISION
```

status/startup.json is authoritative state; startup.log contains detailed failures, startup-response.json is the single response. On unchanged revision wait on the same job without rereading logs or invoking another model. On attention read its bounded evidence, perform only required original analysis and submit one response matching revision and attention_sha256. On failure fix the named cause and retry that exact revision. Do not alter receipts to admit changed originals.

Use the existing toolchain receipt for licensed analysis tools. Bundled FFmpeg is tools/ffmpeg/ffmpeg.exe; bundled GitHub CLI is tools/github-cli/bin/gh.exe. Their absence from PATH does not authorize a download. Preserve Pageant/PuTTY Git SSH configuration and existing GitHub API login; follow HOST_TOOLS.md for distinct discovery/authentication failures. GitHub label/issue synchronization is authorized by startup; no other messaging or repository mutation is implied.

For an existing completed startup, use `startup.py refresh-completion --project PROJECT_ROOT --expected-revision REVISION` to refresh the final scaffold/deployment handoff without initialization or re-export. Add `--psyq-label FRESH_LABEL` to rerun the current xport SDK detector against unchanged accepted originals and return to the normal `psyq_review` gate before rebuilding the static index. The label must be unused; stale revisions, running states and another detector are rejected. Existing exports, classifications and translated source bindings remain preserved. Retry input inventory excludes configured/accepted analysis destinations inside orig, including custom re-export roots; original files outside those generated destinations remain hash-checked.

## Required dependency order

Before startup work, use the chat's "Approve for me" permission mode: approvals_reviewer=auto_review, approval_policy=on-request, sandbox_mode=workspace-write. Do not use Full Access, disable the sandbox or create permission-template/config files. Verify the effective settings in the next turn_context; the visible selector or a previous chat's settings alone are insufficient. If a current turn still uses reviewer=user, finish at a safe boundary and resume the same goal with the corrected mode. Routine Git/build escalation then goes to automatic review; never bypass a denial or add per-command exceptions as the default solution.

1. Initialize from the shared Windows sln/vcxproj, project README and .gitignore templates using exact names/GUID/substitutions. Never copy another game or hand-author replacement build templates. Preserve shared Debug/Release/x86/x64/XCRT settings
2. Initialize the project Git index when absent through the supervisor's git_index step; stage .gitignore. Synchronize configured GitHub labels/issues and diagnose tools without building
3. Extract all original discs to bin/DATA through extract_data; retain originals, accepted hashes and uncertain formats. Never replace files with different bytes on retry
4. Inventory original PS-X EXE/images and verify actual entry/GP, load segments and code/data boundaries before listing/pseudocode export. Do not copy an executable into orig merely to satisfy an old path convention
5. Prepare configured Red Book audio and supported STR -> MP4 assets with shared tools; unsupported formats remain explicit decomp work. No automatic container/overlay discovery by filename
6. Export complete original MIPS and available pseudocode; independently canonicalize and hash-bind accepted evidence. SDK/PsyQ recognition does not replace supplemental Ghidra listing/pseudocode requirements
7. Review SDK identities, initialize static SQLite inventory and validated reuse index. Preserve unknown boundaries, unresolved indirect targets and failed original decompilation as unknown
8. Preserve the full game-function queue for decomp; do not translate callees during setup. Create DuckStation.bat and deploy isolated settings/data for the actually reserved roles (new projects: user and audit), preserving BIOS/licenses and user sessions
9. At the end of setup, discover the game's real Memory Card icon and establish its writer/resource provenance. Produce exact 16x16 indexed ICO at src/platform/win/icon.ico, with local MAINICON binding. No guessed texture, cover art, resizing or default icon substitution
10. Run the final native build only after icon acceptance, then publish the compact handoff and stage src plus root cover.png/README.md through X stage_sources. Complete the goal only after supervisor complete and all requested startup deliverables are verified

## Technical routing

For an attention read only the matching section of STARTUP_REFERENCE.md: names/templates/GitHub, icon/TIM, extraction/music/video, original profile/GP, IDA/Ghidra acceptance, or static database/import details. Do not preload the reference or all evidence. Technical contract gaps require a targeted supervisor fix and check, not an alternate undocumented workflow.

Startup produces prepared inputs, scaffold, accepted analysis, runtime deployment and a decomp handoff; it does not prove game playability or original equivalence. Use [CONTEXT.md](CONTEXT.md) for ongoing state/indexing and [DECOMP.md](DECOMP.md) for the full translation task. No generated HTML progress dashboard or second mutable status hierarchy.

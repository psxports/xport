# Shared Xport upgrades

`[XPORT_ROOT]/CHANGELOG.md` is the ordered authority for shared tool and runtime changes that can affect consuming projects. Its UTC ISO 8601 revision headings are unique and newest first. Each entry states its scope, compatibility, change, project upgrade, automation and verification commands.

Published entries are immutable because project markers assert that their exact migration contract was completed. Correct an older mistake with a new revision instead of editing or reordering history.

Every project that owns `xport_main` keeps exactly one marker directly before its definition:

```c
// XPORT REVISION: 2026-09-24T05:42:31Z
int xport_main(int argc, char **argv)
```

The marker means that all changelog entries through that revision were reviewed, applicable migrations were completed and their declared verification passed. Never advance it speculatively. Projects that have not adopted the shared runtime and do not own `xport_main` are not silently marked current.

## Agent command

The natural request `upgrade` means:

1. Run `X upgrade` once. It reads only the changelog delta after the project marker and returns a bounded plan plus `goal_objective`.
2. If the result is `goal_required`, create exactly one durable goal from that objective. Goal creation is a Codex product action and cannot be performed by the Python launcher. If the current request already has that goal, do not create another one.
3. Run `X upgrade --apply`. Safe declared migration scripts are idempotent and execute once per upgrade identity. A fully automatic delta is verified and completed immediately.
4. On `needs_agent`, use only the returned pending entries and their named files/actions. Do not reread the entire changelog or investigate unrelated history. Finish with `X upgrade --complete`.
5. `--complete` reruns declared verification and advances the marker only after every command passes. `current` means no goal or work is required. `attention_required` preserves the old marker and reports the bounded failure.

Optional `--target REVISION` upgrades to an earlier published changelog revision. The default target is the newest revision.

## Changelog entry contract

Use this exact compact shape:

```md
## 2026-09-24T05:42:31Z

- Scope: shared runtime
- Compatibility: Breaking
- Changed: One concise description
- Upgrade: One concrete project action, or None
- Automation: marker-only
- Verify: X doctor; X code_refresh
```

`Automation` values are:

- `marker-only` when no project edit beyond the final marker is necessary
- `none` when the agent must perform the stated upgrade
- `script:tools/migrations/NAME.py` for a trusted idempotent migration script under the shared toolset

Migration scripts accept `--project PROJECT_ROOT --apply`, preserve unrelated work, fail closed on ambiguous input and return nonzero without advancing the marker. Prefer `marker-only` and generic scripts; never hide game-specific semantic decisions in shared automation.

Verification commands use `X TOOL [ARGS]` and are separated by semicolons. Keep the list proportional to the affected scope. A documentation or marker-only change can use `X doctor`; shared C/runtime changes normally require `X code_refresh` in each affected project.

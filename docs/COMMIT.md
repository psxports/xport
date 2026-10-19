# Commit decompilation progress

Trigger: the explicit user command `commit decomp`. This authorizes the README edits, staging, commit and push below; do not request another confirmation for those operations. This is an agent command routed by `AGENTS.md`, not a shell executable or a subcommand of `xport.py`.

## Scope

- Resolve the workspace as the parent of the shared xport toolset root. Enumerate only its immediate `*-xport` directories
- Skip every project without a local `xport-project.json`; do not modify, stage, commit or push it
- This command explicitly selects all initialized sibling projects, including when invoked from a consuming project's chat. Do not include the shared `xport` or workspace `.github` repository
- Read applicable project instructions. Preserve unrelated work and existing staged changes; do not reset, stash, clean, switch branches or stop other agents

## Prepare each project

1. Resolve `SHORT_NAME` from `xport-project.json.short_name`. For legacy projects without that field, accept the single unambiguous short name already present in their README Play Game URL. Never invent or derive it from the directory/game name; report an unresolved project and continue the others
2. Check `[WORKSPACE]/.github/site/[SHORT_NAME]/index.html`. If absent, wrap only the README HTML Play Game anchor in `<!-- ... -->`. Do not nest comments or rewrite other README content. If a build exists, preserve the link's current state. This narrowly scoped README edit is an explicit exception to the usual README immutability rule for this command only
3. Preserve the project `.gitignore`; startup owns copying it from the canonical template and adding it to Git index. Do not create, replace or synchronize ignore rules during `commit decomp`
4. Check the current branch, origin, GitHub identity and unmerged paths. Use the explicit `github.repository` config and validate that origin resolves to that repository. Fetch the corresponding remote branch before committing. Skip detached HEAD, conflicts, authentication failures or remote divergence with a concrete error; never force push or silently overwrite remote work
5. Stage tracked modifications/deletions with `git add -u`. Preserve existing staged index-only removals, especially local `AGENTS.md`, `xport-project.json` and generated reports; do not re-add those untracked local copies
6. Explicitly stage `README.md`, all non-ignored contents of `src`, root `cover.png` if present and `.gitattributes` if present. Include new source/header/resource/project files and source deletions. Respect ignore rules for generated files; do not force-add ignored build output
7. Do not indiscriminately use `git add .` or `git add -A` at the root. Leave other untracked files, local launchers, extracted assets, reports and build artifacts untouched unless already staged or explicitly requested by the user. Check that no non-ignored new `src` file was missed and no newly added file exceeds GitHub's size limit

## Commit and push

- If `git diff --cached --quiet` reports no staged changes, skip both commit and push, even if older unpushed commits exist
- Otherwise create one ordinary commit with the exact message `decomp progress`; do not use `--allow-empty`, amend earlier commits or bypass hooks
- Push the newly created commit to the same branch in the verified origin repository without force. Existing authenticated GitHub CLI credentials may be used for a per-command HTTPS push; do not expose tokens or persistently rewrite remotes
- If commit or push fails, preserve the index/worktree and any successfully created commit, report the error, and continue other projects. Retry a failed push using the recorded commit instead of creating another commit
- Treat staging as a snapshot. Concurrent agents may produce newer edits; do not repeatedly commit in a loop or interrupt them. Report remaining changes after the batch
- Do not run builds, tests, emulator sessions or site deployments merely to perform this Git command

## Verification and result

- Record each selected/skipped project, created commit hash, branch and push result in a temporary batch receipt outside shared source
- Verify the exact commit message, the expected Play Game comment and inclusion of selected new `src` files. Preserve the `.gitignore` already tracked or staged by startup; include any tracked edits through the ordinary `git add -u` step
- Confirm the remote branch points to the pushed commit. If another actor advanced it, verify the pushed commit is an ancestor and report that fact instead of rewriting history
- Report counts of committed/pushed, unchanged, uninitialized and failed projects; identify changed README links and any newer local changes. Include actionable details for failed repositories; do not claim the whole batch succeeded if one failed

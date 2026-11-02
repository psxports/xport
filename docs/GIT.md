# Git execution authorization

This is standing user authorization for routine Git work in xport repositories. Do not ask the user to approve each command or wait for their presence. Permission to perform an operation and permission to escape a sandbox are different decisions.

## Routine operations

Run necessary status/diff/log/show/ls-files/rev-parse checks, source staging, local branch creation, non-destructive checkout/switch, fetch and other read-only remote checks autonomously. Preserve user work and unrelated index entries. Stage src, cover.png and README.md as required by CONTEXT; never edit README merely to stage it.

When the current task calls for a commit or remote branch/update, its authorization covers the required local commits of task-owned changes and normal non-force pushes to that repository/branch; do not ask again for every constituent command. This rule does not require unsolicited commits/pushes or authorize unrelated publication, deployment or repository changes. Inspect existing staged work before a commit and exclude unrelated changes without discarding them. Do not change branches if doing so would overwrite work.

Ask only for an action that would discard user work, rewrite published history, force-push, delete a remote branch/repository, change credentials/security settings, or exceed the requested repository/task scope. Explain the exact destructive or additional effect. Routine index writes and branch creation are not such effects.

## Execution and failures

Project `.codex/config.toml` defaults to workspace-write, on-request and approvals_reviewer=auto_review. Routine required sandbox escalations go through automatic review, not a separate user question. Existing chat overrides and managed policy can take precedence; do not claim a running chat adopted file changes without checking its actual permissions. Never bypass a review denial.

Start every Git command with ordinary tool permissions. Never set require_escalated merely because a command invokes Git, writes the index, contacts a remote or was escalated in an earlier task. Do not turn a previous approval into a requirement to ask again. Use reusable command-prefix approval only when the active execution tool permits it and an actual required operation was denied.

On an actual sandbox/access/network denial, follow the active tool's approval mechanism for the narrowly scoped operation. Documentation cannot grant OS/sandbox permissions or bypass automatic approval review. Do not evade a rejection with another shell/process/tool. Explain the actual denial and continue independent authorized work; do not call it missing user authorization for Git.

Authentication uses Pageant through the existing Git for Windows OpenSSH bridge described in [HOST_TOOLS.md](HOST_TOOLS.md#git-and-github-mandatory-route). Git operations must not depend on gh login. A real authentication/host-key failure is separate from command approval; report its evidence and exact unresolved requirement.

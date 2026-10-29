# Host executable locations

Resolve paths from [XPORT_ROOT], not the consuming project. A missing PATH entry does not mean a dependency is absent. Use existing shared binaries; do not download/install another per-project distribution or change system PATH/authentication to compensate for discovery failure.

## STR to MP4

FFmpeg: `[XPORT_ROOT]/tools/ffmpeg/ffmpeg.exe`.
Current workspace: `C:/Projects/xports/xport/tools/ffmpeg/ffmpeg.exe`.

`X convert_video` selects explicit --ffmpeg, video.ffmpeg or XPORT_FFMPEG first, then this bundled executable, then PATH. Use the default bundle for STR/MDEC -> H.264/AAC MP4. No ffprobe is required by this converter. If the bundle is missing or fails, report the exact path/error; do not fetch an official distribution automatically. See [FMV.md](FMV.md) for accepted formats.

## GitHub CLI and Pageant

GitHub CLI: `[XPORT_ROOT]/tools/github-cli/bin/gh.exe`.
Current workspace: `C:/Projects/xports/xport/tools/github-cli/bin/gh.exe`.

Startup uses XPORT_GH when explicitly set, then the shared portable executable, then PATH. The shared copy is reused across projects and retains its LICENSE. Do not extract another ZIP into a project. `startup_github.py` performs API calls on the shared hidden desktop.

### Git and GitHub mandatory route

Execute this decision procedure in order. Do not substitute another authentication route after a failure.

1. Classify the requested operation. Local status/add/commit/branch/switch needs no network or gh authentication. Remote refs/branch creation/fetch/push uses Git with Plink/Pageant. Issues/labels/PR API operations use gh. Never use gh api to create a Git ref or require gh auth status/login before a Git operation. A failed API token blocks only the dependent API step; continue authorized local/Git work.
2. For remote Git, inspect the exact repository URL, effective GIT_SSH_COMMAND/GIT_SSH/core.sshCommand/ssh.variant and running Pageant. Pageant is C:/Program Files/TortoiseGit/bin/pageant.exe. Keep existing Plink options; if no Plink client is configured, select C:/Program Files/TortoiseGit/bin/TortoiseGitPlink.exe (fallback only if absent: TortoisePlink.exe in the same directory). Set GIT_SSH_VARIANT=plink and the selected command with -batch -agent in the child process only. Do not use implicit OpenSSH. If origin is HTTPS, pass the same repository's git@github.com:OWNER/REPO.git explicitly; do not try HTTPS credentials or rewrite origin.
3. Run read-only git ls-remote with that exact child environment and SSH URL, with a bounded timeout. If isolated-desktop agent discovery fails or times out, retry once on WinSta0/Default without a visible console. Record the actual client, desktop and error. Pageant access across desktops is not assumed. An HTTPS failure, implicit-SSH host-key error or timeout does not establish invalid credentials.
4. After a successful Git diagnostic perform only the authorized operation. To create a remote branch without switching local branches: resolve refs/heads/main and the target ref using ls-remote, ensure the main commit exists locally (fetch that ref through the same SSH route if needed), then git push SSH_URL MAIN_SHA:refs/heads/BRANCH. Do not force, overwrite an existing different ref or change the working tree. Recheck the remote ref after success. Local branch creation uses ordinary git branch and requires no authentication diagnostic.
5. If the prescribed Git route fails, report only the demonstrated requirement: unavailable client/agent, untrusted host key, rejected key, network failure or repository permission. Missing/invalid gh tokens are never a Git blocker. Do not offer gh login as a workaround. Do not generate/copy/upload keys, install SSH clients, reset credentials, run gh auth setup-git, auto-accept host keys or disable verification. Request user participation only for the specific unresolved requirement; continue independent authorized work.
6. Only for an API operation, use the resolved shared gh executable's auth status --hostname github.com and the existing keyring/token login. Pageant does not authenticate gh api. A demonstrated missing/invalid API login requires user participation for that API step; do not initiate browser login/reset automatically. Never print tokens or copy credentials into project files.

## Analysis and builds

Use `toolchain_preflight.py` for licensed IDA/Ghidra/PSX extension discovery, persisted paths and identity validation. Reuse the existing validated receipt; a missing analysis dependency is not permission to download proprietary tools. Native compiler discovery remains in build_native; preserve its VS2022/architecture requirements.

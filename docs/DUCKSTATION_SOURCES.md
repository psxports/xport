# DuckStation source and distribution

Upstream: `https://github.com/stenzek/duckstation.git`.

`duckstation-manifest.json` pins the upstream commit, dependency release and SHA-256, build configuration, runtime contract and complete local patch. `internal-trace-api.patch` is the only patch applied. Do not restore a standalone state API patch; those changes are already included in the complete patch.

Regenerate the patch only from the explicitly selected prepared source tree. Pass that tree through `duckstation_refresh_patch.py --source PATH`; the command has no implicit source-tree fallback. Use the same tree whose changes were reviewed and built.

From a clean xport checkout run:

```bat
[XPORT_ROOT]\tools\prepare.bat
```

The bootstrap:

1. Clones the exact upstream commit into a user cache
2. Downloads the official Windows x64 dependency pack and verifies its SHA-256
3. Applies and checks the complete xport patch against the clean checkout
4. Generates deterministic version metadata
5. Builds `duckstation-qt` with VS2022 Release x64 v143
6. Verifies required runtime files and xport API markers
7. Atomically publishes `[XPORT_ROOT]/tools/duckstation/distribution`

The historical `ReleaseLTCG` executable suffix is retained for project compatibility; the pinned build configuration is `Release`. The build receipt records the actual configuration.

Source, expanded dependencies, build workspaces and distribution are generated artifacts and are excluded from Git. The default cache and workspace are under `%LOCALAPPDATA%\xport`, outside the repository. A successful build removes its generated default workspace. A failed build preserves its workspace for diagnosis. `--keep-workspace` preserves a successful default workspace. An explicit `--workspace PATH` is preserved unless `--clean-workspace` is also passed. `--offline` permits only already verified cache content. `--verify-only` validates the manifest, patch and required host tools without downloading or building.

Close processes executing the shared distribution executable before publishing. The publisher checks the resolved executable path; a retained runtime in another directory does not block replacement. If a matching process name has no readable executable path, publication fails closed. The bootstrap never terminates an emulator and preserves the existing distribution if build, validation or atomic replacement fails.

Preserve the upstream `LICENSE`, contributor attribution and third-party notices when distributing the binary. Runtime settings, memory cards, logs, saves and traces remain project-owned and are never part of the shared distribution. The user-supplied BIOS may use an explicitly configured external path, currently `C:\Projects\xports\scph5502.bin`; deployment references it without copying it into projects or the shared distribution.

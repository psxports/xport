# xport agent entrypoints

- Shared contract and exact commands: `tools/PIPELINE.md`
- New-project workflow: `STARTUP.md`
- Project initialization details: `tools/docs/PROJECT_SETUP.md`
- Shared migration history: `CHANGELOG.md`
- Upgrade workflow: `tools/docs/UPGRADING.md`
- One-time shared dependency preparation: `tools/prepare.bat`

New-project trigger: `Prepare project FULL_NAME (SHORT) according to STARTUP.md`. Require the user-supplied full name and exact short native name; never infer the short name.

Upgrade trigger: `upgrade`. Resolve and execute the bounded changelog delta through `X upgrade` as specified by `tools/docs/UPGRADING.md`.

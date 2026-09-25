# xport agent entrypoints

- Shared contract and exact commands: `tools/PIPELINE.md`
- New-project workflow: `STARTUP.md`
- Project initialization details: `tools/docs/PROJECT_SETUP.md`
- Shared migration history: `CHANGELOG.md`
- Upgrade workflow: `tools/docs/UPGRADING.md`
- One-time shared dependency preparation: `tools/prepare.bat`

New-project command: `startup FULL_NAME SHORT_NAME`. It launches the single `tools/startup.py` supervisor in the current Codex project directory; `STARTUP.md` defines its revisioned attention protocol. Require both user-supplied names and never infer the short name. The former `Prepare project FULL_NAME (SHORT_NAME) according to STARTUP.md` phrase remains an equivalent trigger.

Upgrade trigger: `upgrade`. Resolve and execute the bounded changelog delta through `X upgrade` as specified by `tools/docs/UPGRADING.md`.

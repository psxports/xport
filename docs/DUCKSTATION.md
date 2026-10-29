# DuckStation manual original-game runtime

Normative extension of `../AGENTS.md`. Build and deployment provenance: [DUCKSTATION_SOURCES.md](DUCKSTATION_SOURCES.md).

DuckStation is used for manual original-game inspection and ordinary debugger work. Decompilation reads accepted static analysis and never launches the emulator to discover functions. CPU, GPU and device timing use pinned upstream behavior. The sole local patch supports `XPORT_DUCKSTATION_DATA_ROOT`, keeping project settings, memory cards and save states separate from the shared executable.

`duckstation_deploy --role user --ensure` creates or validates the isolated project runtime. Resolve its reserved GDB port and data directory from `xport-project.json`; preserve project ownership and user data. Use the tracked settings template; interactive defaults retain controls and audio, automated hidden launches mute audio. Never copy another project's state or ports.

`duckstation_run --role user --visible` and the generated `DuckStation.bat` perform ordinary disc/BIOS startup, unpaused. `--state PATH` or `--slot 1..10` selects an ordinary original-game save state. `--disc ORIGINAL.cue` overrides the disc without changing project identity and cannot combine with `--game-exe`. Existing role processes are preserved; close the existing role before selecting a new scenario. Use the shared desktop launcher for visible manual work and honor required GUI permission. Do not launch a second process to compensate for a hidden first window.

The template references user-supplied BIOS `C:/Projects/xports/scph5502.bin`; do not download or package BIOS files. Project originals remain analysis/manual-emulator inputs, never native game runtime data sources. Preserve upstream licenses and attribution.

# Shared PSX host runtime

Projects compile this canonical PSX/PsyQ runtime directly. Game code calls its PsyQ-compatible API; shared subsystems call required game-owned symbols declared in `xport.h`. No callback registration or fallback is permitted; missing bindings fail at link time.

## Modules

- `xport.h`: the only Xport public header; owns fixed-width types, PsyQ PAD constants, `FUNCTION_MARKER`, function `GDB_CALL` and data `GDB_DATA` exports, platform services, required game bindings and the mandatory project-defined `WND_TITLE`, `WND_WIDTH`, `WND_HEIGHT` and `FIELD_RATE` contract
- `psx.c` / `psx.h`: platform-neutral PsyQ-compatible scalar types, `DRAM`/`SCRATCHPAD` address mapping through `psx_addr`, GPU packet ABI and matrix/GTE helpers
- `psx_gpu.c` / `psx_gpu.h`: the 16-bit `VRAM` source of truth, direct PsyQ image operations, ordering-table traversal, packet rasterization and display-rectangle conversion to the 32-bit platform presentation buffer; `LoadImagePSX` and `StoreImage` consume native C pointers
- `psx_pad.c` / `psx_pad.h`: PsyQ pad initialization, polling and serialized pad state
- `psx_spu.c` / `psx_spu.h`: deterministic SPU RAM, PsyQ libsnd/VAB voices, PSX ADPCM, ADSR, IMA ADPCM Red Book transport and final PCM mixing
- `platform/win/main.c`: Windows process entry, timer, exhaustive message/input polling, window, WaveOut audio, audio synchronization and file services

Each platform backend owns the native process entrypoint and calls the required game-owned `xport_main`. A port to another operating system implements the platform service declarations from `xport.h` in its own `platform/<name>/main.c`; `psx.c` contains no operating-system calls.

PsyQ entries are implemented directly in their owning shared module. Internal subsystem functions, types and constants use the `gpu_*`, `spu_*` and `gte_*` namespaces. Platform presentation and quit state use `xport_present` and `xport_isquit`; do not add `psx_*` forwarding aliases, parallel `PsyQ*` compatibility state or game-owned GPU trampolines.

Game memory maps, translated game logic, title-specific resource loading, WIP behavior and checkpoint orchestration stay in each project. Extend the required game-binding section of `xport.h` only for public cross-platform contracts. The platform audio worker calls shared `spu_render`; games use PsyQ `Ss*`, `Spu*` and `Cd*` calls and own no render callback, mixer, VAB registry or Red Book decoder.

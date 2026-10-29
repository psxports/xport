# Shared runtime index

Read this index, then only the section for the API/subsystem being changed. Do not load every runtime reference. Shared headers define actual signatures; source defines implementation. Documentation never promotes unverified integration to acceptance. Project rollout notes and original evidence remain in RUNTIME_EVIDENCE.json and the owning project, not in stage instructions.

## Modules

- psx.c/psx.h: common PS1/PsyQ types, guest DRAM/scratchpad access, BIOS/timers/IRQ/GTE/PAD, executable loading and shared adapters
- psx_gpu.c/psx_gpu.h: VRAM owner, packet ABI, software raster, draw state/image transfers and native scanout
- psx_spu.c/psx_spu.h and psx_seq.c: SPU/mixer, VAB/voice/SEQ owners, native audio/IMA/XA services
- psx_mdec.c, psx_press.c, psx_vlc.c, psx_stream.c: MDEC/STR algorithms and PsyQ entry boundaries; xport_fmv.c owns standalone playback
- xport_trace.c/xport_trace.h: reusable guest-memory access/observation, audit and call/device events; not a game-state owner
- vid.h/vid_ogl.c: graphics API and current GL implementation; platform/win and platform/web own process/input/timing/audio/file services

## Reference routing

- [Audio](LIBRARY_AUDIO.md): SPU, SEQ/VM, VAB/voices, CD/XA, streaming, callbacks and SDK ownership
- [Video](LIBRARY_VIDEO.md): GPU packets/raster/transfers, GTE, scanout and HD math
- [System](LIBRARY_SYSTEM.md): platform entry, input/PAD, BIOS/IRQ/timers, files/saves and XCRT

Find the API heading with rg, then read that complete section using docs_check --section docs/FILE.md#anchor. Do not read all three references. Original verification/rollout notes remain under their source section keys in RUNTIME_EVIDENCE.json; consult the relevant key, not the whole history. Source implementations and test scopes can have changed since that historical evidence.

## Related contracts

[VID.md](VID.md): renderer API. [FMV.md](FMV.md): standalone/Web movies. [WEB.md](WEB.md): Web packaging and assets. [PLATFORMS.md](PLATFORMS.md): architecture/XCRT. [BUILD_POLICY.md](BUILD_POLICY.md): when checks run.

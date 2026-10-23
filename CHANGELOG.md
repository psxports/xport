# Changelog

## 2026-10-22

- Fixed native queued-audio recovery after long host/debugger stops: physical PCM overflow retires oldest output without blocking VBlank or the game thread;queue capacity follows the two output buffers and `g_xport_audio_dropped_frames` records loss. SPU/SEQ/capture processing retains every field. Native field catch-up polls host events at least once per32fields. Rebuild consumers using `XPORT_AUDIO_VBLANK_QUEUE=1`;legacy worker-rendered audio is unchanged. Shared audio/timing/render-wait/PAD checks pass;P2 Debug x86 real WaveOut recovers after an8second process stop. Evidence: `Pandemonium2-xport/status/enhance/audio-stall-recovery.json`. This does not certify other games or audible output

## 2026-10-16 18:19:31 UTC

- Added `r_masked_u32` for aligned word reads with explicit LWL/LWR observation byte lanes. Existing scalar and MMIO contracts are unchanged; partial MMIO is rejected because the device read callback has no mask. No new consumer binding, serialized state or required migration. Native x86/x64 and x86 LOCKSTEP_DEBUG contract tests pass; FF texture-loader raw/C comparison passes37 cases including two unaligned sources with exact ordered masks. Contract: docs/MASKED_MEMORY.md. Evidence: FightingForce-xport/status/implementation-completion/masked-memory-compatibility-review.json and texture-loader-full-review.json. Other consumers' gameplay and physical bus timing are not certified

## 2026-10-15 01:59:41 UTC

- Implemented native GP0(02) VRAM fill in DrawPrim and OT command execution: RGB555, 16-pixel X/width alignment, zero dimensions, wrapping and existing interlace/frame-skip policy. GPU/device observers retain original commands and exact VRAM writes; API, bindings and snapshot layouts are unchanged. Debug GPU observer and FF display integration pass; Rapid Racer Debug compiles. Agent Armstrong compilation is blocked by old packet fields and Gran Turismo linking by unrelated missing vid symbols. Evidence: FightingForce-xport/status/implementation-completion/display-initialize-review.json. Consumers require a rebuild only

## 2026-10-07 16:20:02 UTC

- Corrected SetDefDrawEnv graph-type word read, signed full-height threshold and ordered store widths; GetGraphType now requires the game-owned xport_gpu_graph_type_address data binding. Evidence: AutoDestruct-xport/status/audits/SetDefDrawEnv-static.json; guest ABI and dynamic proof pending

## 2026-10-07 16:08:43 UTC

- Matched the seven InitGeom GTE control writes to reviewed 1.EXE:800835A4, preserved unrelated GTE and matrix-stack state, and added DQA/DQB snapshot fields and register import; evidence: AutoDestruct-xport/status/audits/InitGeom-gte-static.json. CPU/BIOS effects, snapshot channel, Debug batch and dynamic verification remain pending

## 2026-10-07 15:53:33 UTC

- Matched SetDefDispEnv store widths and order to the complete reviewed 1.EXE:800834E0 MIPS body, including the final height store; source-bound evidence: AutoDestruct-xport/status/audits/SetDefDispEnv-static.json. Owner guest ABI binding, Debug batch and dynamic validation remain pending

## 2026-10-07 15:14:48 UTC

- Added CloseEvent from the complete reviewed B0/9 MIPS body: low-16 handle indexing, guest table read, status-word clear and return 1; evidence: AutoDestruct-xport/status/audits/CloseEvent-static.json. Debug batch build, caller CPU context and dynamic validation remain pending

## 2026-10-07 15:02:19 UTC

- Implemented SysDeqIntRP with guest addresses, ordered IRQ-chain reads/unlink stores and removed-node return; static evidence: AutoDestruct-xport/status/audits/bios-close-dequeue.md. Debug batch build and dynamic validation remain pending

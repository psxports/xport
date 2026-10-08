# Changelog

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

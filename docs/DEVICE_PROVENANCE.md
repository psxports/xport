# DMA, GPU and SPU provenance

Device provenance explains writes which have no CPU store instruction. The shared device API in `xport_trace.c` and the pinned DuckStation observer use the same operation lifecycle: `create`, optional `submit` and `fetch`, one or more `write` events, then `complete`. Every event carries an operation ID, optional parent operation, frame, guest clock, device primitive, source and destination range, channel, command and evidence strength. It deliberately has no CPU PC.

`exact` means the observer saw the memory effect at the implementation point that performed it. `candidate` means the operation can affect the range but completion or byte coverage was not observed precisely. `uncovered` is an explicit instrumentation gap. A candidate renderer command can guide investigation, but it cannot prove the last writer. An operation that remains active when the trace ends is a lost completion and invalidates complete provenance.

The initial covered primitives are RAM DMA, GPU packet fetch, VRAM upload/copy/fill, GPU raster completion, SPU upload/DMA/capture/reverb. Native VRAM copy/fill and SPU transfer paths report exact writes. The original DuckStation SPU paths report exact writes; GPU backend enqueue reports candidates until an exact backend completion observer exists. Other device internals must emit an uncovered gap instead of borrowing a CPU PC or claiming exact coverage.

GPU packets and their ordering table stay in DRAM through the frame. Packet creation, DMA fetch and raster work are linked by operation IDs so diagnostics can follow the source. OT is already part of DRAM and is not a separate equality gate.

The runtime sink is process-global and optional. Call `xport_device_trace_bind`, update its boundary clock with `xport_device_trace_set_time`, and zero-initialize each `XportDeviceOperation` before its first `begin`. Disabling the sink leaves production behavior unchanged.

Original traces use XPEVT001 kind 5. Convert or query them with:

```powershell
python xport/tools/trace_device_provenance.py convert GAME-xport/status/execution-traces/NAME/events.xpe --output GAME-xport/status/diagnostics/NAME/device.json
python xport/tools/trace_device_provenance.py validate GAME-xport/status/diagnostics/NAME/device.json
python xport/tools/trace_device_provenance.py last-writer GAME-xport/status/diagnostics/NAME/device.json --region spu_ram --offset 0 --size 2
```

The execution SQLite index combines CPU and device writes by event sequence. Device results identify an operation and primitive and never invent a PC. A missing precise writer remains an observation gap unless all relevant CPU and device paths are exact for that byte range and window.

# Native platforms

## Platform identity

`src/ap.h`, included by `xport.h`, derives platform identity from compiler target macros. Do not set contradictory target flags in a consuming project.

- Windows x86 defines `AP_WIN` and `AP_X86`
- Windows x64 defines `AP_WIN` and `AP_X64`
- Emscripten defines `AP_WEB`, without `AP_WIN`, `AP_X86` or `AP_X64`

OS and CPU identity are independent. Use AP macros for xport-owned platform branches; keep compiler feature macros such as `_MSC_VER` for compiler-specific syntax and leave third-party headers unchanged. Invalid OS/CPU combinations fail compilation.

Native x64 support must not add x64 argument/stack probes, relax MATCH, change guest widths or add memory exceptions. Existing mapped DRAM remains 2 MiB and scratchpad remains 1 KiB; their backing storage, little-endian accesses and comparison protocols are unchanged.

## Build selection and files

Resolve the project from `xport-project.json`. Native solutions provide Debug/Release for x86 and x64; solution `x86` maps to project `Win32`, while x64 maps directly.

```text
python -B [XPORT_ROOT]/tools/xport.py --project PROJECT_ROOT build_native --architecture x86 --configuration Debug
python -B [XPORT_ROOT]/tools/xport.py --project PROJECT_ROOT build_native --architecture x64 --configuration Debug
```

`--plan` reports the command and executable without building. The default architecture remains x86 for existing projects. `code_refresh --architecture x64` overrides the configured refresh platform for that invocation. Existing projects need x64 configurations before selecting that architecture; startup templates already include them.

Intermediates use `_build/x86/CONFIG` and `_build/x64/CONFIG`. Executables retain `bin/SHORT.exe` and `bin/SHORT_debug.exe`, with working directory `bin`. The latest architecture build replaces that configuration's executable. Do not build both architectures concurrently against those shared output files. Project targets record the last successfully linked architecture per configuration and force relinking on a switch, including when the other architecture has a newer executable timestamp. They do not recompile unchanged intermediates merely to switch architecture.

Release on x86 and x64 selects xcrt when `UseXcrt` is enabled. `/p:UseXcrt=false` selects standard static CRT `/MT`; Debug uses `/MDd`. Shared `xcrt.c` contains portable services. Each architecture assembles its own `platform/win/xcrt_x86.asm` or `platform/win/xcrt_x64.asm` through the MASM build customization, providing its own register ABI helpers, math entry points and stack probing. Both Release architectures disable ASLR/relocations and use `/Gs2147483647` to suppress ordinary stack probes. x64 retains full-width addresses and does not require loading below 4 GiB; its XCRT `__chkstk` helper remains available for code compiled with other probe thresholds. Use [BUILD_POLICY.md](BUILD_POLICY.md) for build timing and selected configurations.

## Pointer and format boundaries

Host pointers, callbacks and pointer arithmetic retain pointer width (`uintptr_t`, `intptr_t`, `size_t`, `ptrdiff_t` where appropriate). Original PS1 addresses, offsets, registers, DMA tags and asset/save fields retain their original fixed widths. Do not mechanically widen guest words or serialize native structures containing pointers.

Register complete native OT/packet pools through `gpu_register_packet_range`. The registry maps full host addresses to independent 24-bit links. On x64, unregistered native OT/packet pointers passed to chain helpers are rejected instead of being truncated. Numeric guest links and terminators remain valid for `setaddr`; registered pointers use the same registry. A registered pool must stay alive while its links can be used; this API does not unregister temporary allocations.

The native `VSyncCallback` return uses `intptr` so a previous host callback address survives on x64; guest SDK wrappers and original guest callback fields retain their PS1 contracts. Pointer-bearing native structures can grow. Asset/packet formats must not grow: use explicit decode/encode and size/offset assertions at their boundaries.

VID owns GPU handles and pointer-sized buffer offsets. Its compact world/UI vertex sizes remain 52/44 bytes on both architectures. The CPU word size does not change GLSL interfaces, uint32 index buffers, VRAM layout or original raster arithmetic. Game rendering/physics algorithms require no architecture-driven redesign.

## Targeted checks


```text
python -B [XPORT_ROOT]/tools/xport.py --project PROJECT_ROOT gpu_observer_test --architecture x64 --configuration Debug
python -B [XPORT_ROOT]/tools/xport.py --project PROJECT_ROOT gpu_observer_test --fixture vsync_callback --architecture x64 --configuration Debug
```

Fixture x64 outputs use `_build/FIXTURE/x64/Debug`; the existing x86 fixture paths remain valid. Run x86 variants as regression checks.

From `[XPORT_ROOT]/tools`, select `XPORT_TEST_ARCH=x64` or `x86` in the environment, then run:

```text
python -B -m unittest tests.test_architecture tests.test_native_timing tests.test_pad_ports -v
```

The helper selects `vcvars64.bat`/`vcvars32.bat` before compiling native snippets. Web builds use the pinned Emscripten toolchain and stay wasm32. Validate AP_WEB from an actual Emscripten compile and rebuild Web after common-header changes.

Verify OG chains above 4 GiB, callback roundtrips, raster/VRAM/VSync, actual native GL shader/pixel tests, float math, saves and project layouts. Keep full replay acceptance with its existing x86 project/record owner.

Pilot evidence and validation limits: consuming project's `status/audits/x64/report.json`. No game Release build or runtime acceptance is implied by a Debug fixture or project-property evaluation.


Native VM platform startup binds the existing `spu_mmio_read`/`spu_mmio_write` callbacks to `0x1F801C00..0x1F801DFF` after `xport_memory_init`. The native SEQ/VM flush uses these SPU register accesses; XA mixing follows a separate path and can remain audible when this binding is missing. Keep this device-register boundary separate from game DRAM ownership. Do not enable unrelated DMA/GPU bus paths to restore native SPU output.

## XCRT architecture checks

Run the complete unoptimized runtime fixture independently of game Release builds:

```text
python -B [XPORT_ROOT]/tools/tests/xcrt_test.py --architecture x64 --output PROJECT_ROOT/status/audits/xcrt-x64/x64
python -B [XPORT_ROOT]/tools/tests/xcrt_test.py --architecture x86 --output PROJECT_ROOT/status/audits/xcrt-x64/x86
```

The fixture links `/NODEFAULTLIB`, checks startup, actual global C++ construction/destruction, LIFO `atexit` including registration during exit, allocation, strings, native file I/O, integer division and non-unwinding jumps, and rejects CRT DLL imports. x86 directly checks the `__ultof3` register ABI (`EDX:ECX` unsigned 64-bit input, `XMM0` binary32 output) against an integer round-to-nearest-even reference at powers of two, halfway boundaries and 16384 additional values. x64 checks float/double sin/cos/atan2/sqrt against dynamically loaded UCRT, restores all nonvolatile integer/XMM registers and MXCSR/x87 control words across a jump, and exercises `__chkstk` with a 32 KiB stack allocation. These fixtures do not establish game Release runtime acceptance.

## Minimal XCRT and C++ lifetime

Keep `atexit` callbacks and static destruction available to shared consumers.

`WinMainCRTStartup` initializes the heap, walks linker-sorted `.CRT$XCA` through `.CRT$XCZ` C++ initializers and calls `exit` after `WinMain`. `atexit` retains callbacks in a heap-backed LIFO list; callbacks run outside its lock and may register additional callbacks. `abort` does not run exit callbacks. This is ordinary C++ static lifetime support, independent of STL.

Release excludes diagnostic formatting, output and diagnostic strings at compile time. Keep failure conditions and termination outside Debug-only output blocks. XCRT must not implement `printf`, `fprintf`, `vfprintf`, `fputs`, `fflush` or `__acrt_iob_func`; do not replace them with success stubs or pull in the standard CRT to satisfy diagnostic calls. Debug may use normal CRT diagnostics. BIOS standard-output writes validate their arguments and discard their payload in Release. Native XCRT file writes are synchronous and unbuffered; actual resource/save writes retain their byte-count checks.

Existing `sprintf`/`snprintf` format into caller-owned buffers using Windows formatters and do not implement the complete ISO printf contract. They are distinct from diagnostic output and remain available for required game text/path construction.

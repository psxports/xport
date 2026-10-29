# Runtime system contracts

Section routing and verification provenance: [LIBRARY.md](LIBRARY.md).

## Native DCPL command

- `xport_gte_execute(0x680029)` owns the DCPL operation formerly implemented in Apocalypse. It consumes current RGBC, IR0/IR1..3 and far-color registers; applies signed44-bit accumulation wrapping, arithmetic shift12, signed IR saturation and RGB saturation; then publishes MAC/IR, the RGB FIFO and FLAG. Preserve the RGBC command byte
- Only the reviewed `0x680029` encoding is enabled for opcode0x29; other encodings retain the unsupported-command path. Existing command implementations and GPU/OGL algorithms are unchanged
- Use existing native44-bit wrap, floor-shift, IR/RGB limit and FLAG helpers. The signed44 minimum must shift to `INT32_MIN` without negating a narrowed `INT32_MIN`

## sprintfPSX

`sprintfPSX(destination, format, value)` accepts numeric guest addresses and one uint32 argument: literals, signed `%d` and `%%`; writes a terminating NUL and returns the character count. Unsupported conversions or a second decimal argument abort explicitly. This is a limited compatibility formatter for reviewed callers, not full variadic sprintf. Verify from tools with `python -B -m unittest tests.test_sprintf_psx -v`. No existing ABI or consumer migration changes.

## mallocPSX

`mallocPSX(bytes)` and `freePSX(address)` operate on the BIOS user heap initialized by `psx_bios_init_user_heap`. Inputs and returned pointers are numeric guest addresses. The allocator preserves four-byte headers, the low-bit free flag, lazy adjacent-block coalescing, the next-fit cursor and heap growth state in DRAM. Failure retains the original zero or 0xFFFFFFFF result according to the failed scan path; free adds no native-pointer or null-pointer special case. These additive APIs introduce no required bindings or modules. Verify from tools with `python -B -m unittest tests.test_bios_user_heap -v`.

## Native inline image-load packets

- `void SetDrawLoad(DR_LOAD *packet, const PSX_RECT *rectangle)` constructs the reviewed PsyQ LIBGPU P35 packet in a 68-byte writable buffer. The caller supplies the inline pixel words after the constructor and links the packet through the existing OT APIs
- The constructor reads signed16 width/height, computes `(width * height + 1) / 2` with signed truncation, stores length `words + 4` for unsigned `words < 13` and length0 otherwise, writes GP0 A0/rectangle words and places the final cache-clear word at `packet + length * 4`. Preserve original ordered reads for overlapping rectangle/packet storage. Native void ABI discards the original final-word pointer return
- Existing OT A0 execution delegates to `LoadImagePSX`; construction and linking do not upload immediately. Existing VRAM ownership, invalidation and wrapped destination coordinates remain with that service. No complete upload is claimed for malformed dimensions or the constructor's zero-length output

The historical constructor review did not establish a passing whole-GPU regression; run the current observer fixture for current GPU acceptance

## Native BIOS display callback and event domains

- `psx_bios_vsync_callback(callback)` installs a typed native callback at the existing BIOS display-lane point and returns its previous native-domain pointer. It is separate from native IRQ0 and the later VSyncCallback lane used by SEQ. Numeric VSyncCallbackPSX and native display registration clear each other;ResetCallbackPSX clears both
- P2 game-clock and SPU DMA event registrations use existing typed OpenEvent and matching native lifecycle APIs. Keep original event class/spec/mode and critical-section order;handles are opaque native pool values,not guest table addresses. AdvanceGameClock uses exact PsxEventCallback ABI with unchanged unsigned counter updates

## Native root-counter stop

- `psx_root_counter_stop(counter)` is the native StopRCnt835B8 body with reviewed immutable masks10/20/40/1 and I_MASK port1F801074. It selects counter low16,clears only the selected mask and returns1;native selector outside0..3 aborts before table access
- Legacy `StopRCnt(counter,control_slot,mask_table)` remains unchanged for current guest-bound consumers. P2 ShutdownGameClock now uses the native variant without EXE table addresses and preserves event/critical-section order

## Native callback driver lifecycle

- `PsxCallbackDriver psx_callback_driver` is the single initialized/saved I_MASK/DPCR owner for shared ResetCallback and P2. Process-local IRQ/DMA/VBlank tables retain actual function pointers;retire guest exception-context storage and callback-address mirrors instead of allocating dummy native register arrays
- Genuine PsyQ `stopIntr()` preserves EnterCriticalSection,device save/disable/acknowledge and DPCR mask order;`restartIntr()` restores I_MASK/DPCR then exits the critical section. ReturnNULL for the original inactive/no-change branch or the actual native owner pointer for success. Native restart does not publish a host context address to the guest BIOS vector

Callback rebinding and mixed BIOS/native stop/reset semantics remain unverified;existing StopCallback API is unchanged

## Native VBlank callback slots

- Genuine PsyQ `setIntrVSync(slot,callback)` stores actual native pointers in8 process-local slots and returns the previous pointer;invalid slots returnNULL without access. `trapIntrVSync()` calls current slots0..7 in order,reading each just before invocation so earlier callbacks can change later slots
- P2 native IRQ0 installs the trap at the existing reset point;shared initial ResetCallback and P2 cold/full reset clear the native table. The original unused guest counter remains retired;native VBlank timing is unchanged. BIOS display callback and native IRQ0/SEQ chaining remain separate ordered routes
- Original wrapper7AA1C restores its two actual inputs after recovering omitted prelude7AA14. Generic IRQ driver state and callback snapshot/rebinding remain migration work

## Native video mode and draw completion

- Use the existing SetVideoMode/GetVideoMode owner for SDK mode state;consuming-game cold startup supplies its reviewed regional default. Do not keep a writable guest video mirror
- Native DrawSync callbacks/previous returns are actual PsxInterruptCallback values. P2 registers original MarkDrawComplete directly with a void ABI and explicitly deregisters at graph reset;remove its numeric address decoder and guest callback field. Preserve original state updates and markers

BIOS/display and native/audio VBlank routes still need a complete ordering-aware migration

## Native SDK allocation records

- `SpuMallocRecord` contains numeric SPU RAM `address` with SDK flag bits and `size`; it contains no host pointer. `SpuMallocState` holds count, last index and a native pointer to caller-owned records. These C-compatible types do not imply an original C++ class
- Supply count+1 records for the allocator's tail/split record. Preserve caller lifetime through every allocation and free. Decode cold defaults explicitly; repeated SDK initialization need not clear records beyond the first
- The state record contains a host pointer: never copy it into an encoded resource, guest DRAM or a portable snapshot. Existing `SpuHeapBinding` API remains unchanged. Do not run independent allocators against the same SPU RAM domain
- P2 native storage migration uses its original legacy SDK bodies and return contracts; shared algorithm consolidation remains pending. Final Debug x86 validation deferred

## Native game saves

- Declare `sint32 xport_save(const void *data, sint32 size)` and `sint32 xport_load(void *data, sint32 capacity)` in `xport.h`; implementations belong to the Windows/web backend
- `save`: return `1` after storage completes, `-1` on failure. Require a non-null buffer and positive size
- `load`: return bytes copied, `0` when no save is found, `-1` on failure or invalid arguments. Require a non-null buffer and positive capacity. Reject empty or oversized saves before copying; never truncate
- Windows: `%APPDATA%\xport\SHORT_NAME.sav`. Define `SHORT_NAME="PROJECT_SHORT_NAME"` on the shared Windows `main.c` compile item; startup renders it from the project template. Debug and Release share the same save, without `_debug`. Save through a sibling `.tmp` and replace the destination only after a complete write; unavailable file attributes are treated as no save
- Web: IndexedDB database `XPORT_SAVES`, object store `saves`, key `SHORT_NAME` from the consuming Makefile. Keep saves separate from the replaceable asset cache. Asyncify waits for transaction completion; return failure for unavailable/blocked storage. Saves remain scoped to the browser origin and profile
- Game code owns payload layout, version/checksum validation, packing/unpacking and any slots inside the payload. The backend stores raw bytes without a memory-card header or serialization wrapper
- Call from the game/main thread. Keep load destinations valid until return. Do not invoke from interrupts, audio callbacks or overlapping Asyncify calls
- Project migration replaces card management and asynchronous completion handling with these synchronous calls. Do not save native pointers or substitute these calls blindly for every individual memory-card sector transfer
- Verification: from `tools`, run `python -B -m unittest tests.test_game_saves -v`. Requires MSVC, bundled Emscripten, Node with `playwright` available through `NODE_PATH`, and Chrome (`CHROME_EXE` overrides its path). Tests compile the actual backend functions; Windows files stay inside `_build`, web tests use real WASM/IndexedDB and reload the page. No game runs or user saves are modified

### SpuSetKey

Native `SpuSetKey(SPU_OFF, mask)` cancels unrendered SDK key-on bits for the same voices after queuing key-off. This prevents a later SDK stop from leaving a voice in Sustain when both operations reach one native mixer buffer. API signatures and raw MMIO key-latch handling are unchanged. Native `SpuGetKeyStatus` reports a queued SDK key-on as `SPU_ON` until the mixer evaluates its first envelope; this prevents an immediate status poll from stopping Rapid Racer short effects before they start. Full Rapid Racer MIPS branches: `RapidRacer-xport/orig/images/MAIN.EXE/functions/80036990.lst` and `80077620.lst`; verify both native paths with `gpu_observer_test.py --fixture spu_pending_key`

### catan

PsyQ `sint32 catan(sint32 a)` in `psx.c` evaluates the original COR_06 twelve-step fixed-point slope angle, including the final zero increments and unsigned low-word arithmetic with signed shifts. It owns immutable SDK constants and does not read a game DRAM table. Original identity: Apocalypse `SLUS_003.73` `0x8008702C`, reviewed Ghidra PsyQ `LIBGTE.LIB` `COR_06.OBJ` matches 410-470; prototype: `D:/psyq/include/LIBGTE.H:445`. Those checks do not certify other consumers or all 32-bit inputs.

### xport 2

Projects compile this canonical PSX/PsyQ runtime directly. Game code calls its PsyQ-compatible API; shared subsystems call required game-owned symbols declared in `xport.h`. No callback registration or fallback is permitted; missing bindings fail at link time.

### XPORT_DISABLE_FUNCTION_MARKERS

`XPORT_DISABLE_FUNCTION_MARKERS` makes `FUNCTION_MARKER(address, image)` a compile-time no-op without evaluating arguments. It is opt-in per consuming build configuration; other projects retain their existing marker behavior.

### xport_cd_file_size_native

At the native host-file boundary, `xport_cd_file_size_native(src, root, size)` and `xport_cd_file_read_native(src, root, dst, cap, size)` accept ordinary string and buffer pointers. They strip leading separators and the CD version suffix, convert backslashes, enforce the 260-byte host path capacity and reject colons in the CD path and `..` sequences in the resulting path, matching the existing guest path resolver. The caller owns the strings and destination through the synchronous call. Native reads use `xport_file_read` directly and do not publish guest-copy memory events. The existing guest-address file APIs retain their prior behavior for other consumers.

### xport_gpu_guest_dispatch

For a consuming project that supplies `xport_gpu_guest_dispatch`, define `XPORT_GAME_GPU_GUEST_DISPATCH=1` for its complete native build. This disables only the legacy shared fail-fast definition in `psx_gpu.c`; the existing declaration, signature and callers remain unchanged. Supply the symbol in an already registered project binding module and fail fast when its GPU adapter is absent. With the macro absent or zero, existing consumers retain the shared definition and need no new source or binding.

Native Windows/Web input owns two independent digital pad ports. P1: WASD directions; I/J/K/L Triangle/Square/Cross/Circle; 1/Q/3/E L2/L1/R2/R1; Enter Start; Backspace Select. P2: arrow cluster directions; NumPad 5/1/2/3 Triangle/Square/Cross/Circle; NumPad 7/4/9/6 L2/L1/R2/R1; NumPad Enter Start; NumPad Plus Select. NumPad 0 is unmapped. Windows distinguishes NumPad Enter and Num Lock off navigation keys through the extended-key flag. Arrow keys belong only to P2. XInput/browser gamepads use the first two connected devices in enumeration order, one per port. Touch and the existing input override API affect P1 only. Invalid port indices return zero.

### PadInitDirect

After `PadInitDirect`, call `PadStartCom` to enable native packet publication. Active polling publishes connected `0x41` packets for both keyboard ports and any bound SDK owner packets; it preserves active-low button byte order. `PadStopCom` suspends polling. Native serial state is independent per port. Game-specific split-screen evidence belongs to the consuming project.

### xport_main

Each platform backend owns the native process entrypoint and calls the required game-owned `void xport_main(void)`. The backend consumes common flags directly and receives the result through `xport_set_exit_code`. A port to another operating system implements the platform service declarations from `xport.h` in its own `platform/<name>/main.c`; `psx.c` contains no operating-system calls.

### psx 2

Translated code must include `psx.h` and use the shared debugger-visible `r_u8`, `r_u16`, `r_u32`, `r_s8`, `r_s16`, `r_s32`, `w_u8`, `w_u16`, `w_u32` and `w_masked_u32` functions for numeric guest-memory access. These functions preserve PSX aliases, unaligned little-endian access, DRAM/Scratchpad selection through the canonical `psx_addr` mapping, MMIO dispatch and trace observation. Do not add game-owned memory helpers, macros or DRAM/Scratchpad address decoders. Debug builds export stable function symbols so a debugger can break on every read or write; memory diagnostics may wrap writes with source metadata but still call the same shared symbols. The shared `w_u8_at`, `w_u16_at`, `w_u32_at` and `w_masked_u32_at` diagnostic adapters attach C source metadata and are never implemented or called explicitly by a game.

### psx_gte

`psx_gte.h` native inline load/store helpers use the canonical COP2 register services, not a consumer-owned register bank. The 136 cases compare complete fixture buffers and data/control banks; matrix/vector padding is zero and register services are explicit fixtures. This verifies bounded operand/register/field-offset transfers, not core arithmetic, COP2 command execution, timing or every pointer/layout case.

### xport_gte_execute 2

`xport_gte_execute` uses the existing MVMVA engine for its supported commands: `0x480012` transforms V0 with translation, `0x49E012` transforms IR without translation, and `0x486012` transforms V0 without translation for `MulMatrix0`. Other commands remain unsupported. All three preserve signed fixed-point rounding, MAC wrapping and IR saturation in that engine.

### OpenEvent

`OpenEvent` allocates one of 32 native records and returns an untagged handle in `1..32`; exhaustion returns `-1`. `EnableEvent`, `DisableEvent` and `CloseEvent` validate allocated native handles. `DeliverEvent` matches enabled class/specification records: mode `0x1000` calls the typed native callback, and mode `0x2000` sets a pending event consumed by `TestEvent`. Tagged BIOS handles retain their separate guest-table path. Root-counter IRQ and VBlank delivery use the shared interrupt state, including deferred delivery while interrupts are disabled. Hardware DMA interrupt delivery remains outside the native callback registry contract.

### PadGetState

`PadGetState` reads an SDK owner installed through `psx_pad_bind_owner_packets` before using the unbound native connection model. Port selection uses bits `0xF0`. Default owner-state queries follow PsyQ 4.5: bytes `+0x37` and `+0x38`, a non-self owner link with byte `+0x39`, and packet status select the error path. Define `XPORT_PAD_STATE_PSYQ43` for the PsyQ 4.3 contract used by NFSHS: inspect the upper half of the word at `+0x34`, and inspect byte `+0x38` only when the owner link at `+0x10` is not self-linked. Both contracts inspect packet status and map error states 2/3 to 1 and 6 to 4; other states remain unchanged. Select the contract consistently in native and Web project definitions. This setting changes only the bound owner-state query, not the public signature, unbound active/connected model, multitap owner selection or PAD transport. Serial scheduling and hardware timing remain unproved.

### PadSetAct

`PadSetAct` retains the caller-owned native buffer and the low byte of its length; it does not copy, clamp or read the buffer. The caller keeps the buffer alive while registered and replaces it before freeing it. Null pointers and zero lengths replace the previous registration. `PadSetActPSX` maps a nonzero numeric pointer independently of length and publishes the original address at owner offset `0x28` and the length byte at `0x34` when a numeric owner is bound. Native pointers remain private runtime state and are never narrowed into guest words. These actuator bindings are not serialized by `pad_state_io`. Native PAD polling samples the registered bytes through the six-byte `PadSetActAlign` map: entry 0 is the binary small motor and entry 1 is the variable large motor; FF entries are ignored. `PadInitDirect` clears the six-byte actuator maps to FF, so unconfigured bytes cannot drive either motor. Unsupported digital pads remain rejected. Verify with `python -B -m unittest tests.test_pad_rumble_alignment tests.test_pad_ports -v` from tools. `PadStopCom` stops host output. Replay-bound polling does not drive physical motors.

### xport_input_rumble_supported

Host vibration uses `xport_input_rumble_supported(controller)` and `xport_input_rumble(controller, low, high)`, with independent 16-bit low/high frequency strengths and the same first-two-connected-device mapping as input. Each platform retains the active input type independently for both players; keyboard/touch is the initial type and suppresses motor output without hiding actuator capabilities. A new mapped keyboard/touch press stops the corresponding motors immediately; a new gamepad button or mapped stick-direction press permits output. Held gamepad buttons and keyboard autorepeat do not change the active type. Run python -B -m unittest tests.test_pad_ports -v from tools for the device-transition and independent-port checks. Windows dynamically loads `XInputSetState`, suppresses unchanged motor commands, clears outputs on focus loss/shutdown and checks focus before further output. Web uses `Gamepad.vibrationActuator.playEffect('dual-rumble')` when available, refreshes bounded 250 ms effects at most every 100 ms while unchanged, handles rejected promises, and resets output on blur/shutdown. Web hardware/API support is queried instead of assumed; absent support returns zero capabilities. `PadInfoAct` reads the bound PsyQ owner actuator descriptors when present; the unbound host path exposes two actuators when supported: terms 1/2/3 return [1,2,0] for the small binary motor and [1,1,1] for the large variable motor. Unsupported hosts return no actuators; bound owner descriptors retain priority. Physical motor strength/timing requires user hardware verification.

### qsortPSX

Use `qsortPSX`, `randPSX` and `srandPSX` instead of overriding host libc symbols. `qsortPSX` preserves the original middle-pivot and strict-less-than partition order, including equal keys. The random generator owns the BIOS seed in shared guest memory and returns a signed SDK integer in `0..32767`. `psyq_rand_next(uint32 *seed)` performs the same wrapping recurrence with an explicit native seed owner for LIBC2 consumers. Do not alias a game LIBC2 seed to the BIOS seed; consumer initialization and reseeding remain game-owned.

### __ltdf2

Explicit reconstructed calls to `__ltdf2`, `__floatsisf`, `__fixsfsi`, `__divsf3`, `__extendsfdf2`, `__fixdfsi`, `__floatsidf` and `__truncdfsf2` use the shared runtime rather than native casts/operators. Their source algorithms preserve PsyQ bit ordering, half-up rounding, variable-shift count masking, saturation and non-IEEE zero/division behavior. `__ltdf2` returns only `-1` or `0` and orders NaN encodings by magnitude; it is not a host three-way comparator. `__fixsfsi(-0.0f)` returns `INT_MIN`. `__floatsisf(INT_MIN)` preserves the original nonterminating normalization loop; do not use this entry to introduce a new integer-conversion policy.

### __mulsf3PSX

`__mulsf3PSX(uint32, uint32)` accepts and returns encoded binary32 words, matching the original soft-float register carriers. Do not return this result through a native `float` function: x86 x87 quiets signaling NaNs and changes the original result bits. Preserve words across nested arithmetic calls. `psx_float_bits` and `psx_float_value` marshal native numeric values at the boundary; native conversion to a numeric float is not a guarantee of signaling-NaN bit preservation. The multiply uses the original high-part product and separately truncated cross-products, not a native floating-point multiplication or a full 48-bit mantissa product.

### __extendsfdf2

`__extendsfdf2` normalizes nonzero binary32 subnormals and rebases every nonzero exponent literally, including exponent `255`; this differs from native infinity/NaN promotion. `__fixdfsi` preserves the original negative-zero result and reports overflow for either sign. `__truncdfsf2` preserves half-up rounding and original modulo-32 underflow shift counts. Both integer-to-float entries retain the original nonterminating normalization for `INT_MIN`.

### __adddf3PSX

`__adddf3PSX`, `__muldf3PSX` and `__divdf3PSX` accept and return encoded binary64 values through `uint64` carriers. Keep nested arithmetic encoded; use `psx_double_bits` and `psx_double_value` only at native numeric boundaries, where signaling NaNs may be quieted. Addition preserves signed mantissa alignment and guard-bit-dependent rounding. Multiplication omits the low-21-bit product and truncates each cross-product separately. Division uses the original 54-bit restoring quotient and a 32-bit masked underflow rounding increment; a zero denominator produces the signed all-ones NaN encoding without an error event. Overflow reports `0x22` at points `0x0b`, `0x0d` and `0x0f` respectively. These contracts derive from complete original MIPS, not native IEEE arithmetic. Consumer caller audits remain separate requirements.

### LIBMATH

The native `LIBMATH` globals `math_errno` and `math_err_point` belong to `psx.c`. `_err_math` writes both before delivering class `0xF4000002`, specification `0x301` for error `0x21` or `0x302` for error `0x22`; other error values deliver no event. Original title-specific DRAM addresses of these SDK globals are not automatically native-symbol aliases. A consumer reading those guest addresses needs an audited mapping. These helpers do not establish equivalence for every reconstructed caller or every possible operand pair.

### VectorNormal

`VectorNormal` and `VectorNormalS` use the immutable 192-entry PsyQ reciprocal square-root table owned by `src/psx.c`. PsyQ mathematical tables must belong to the shared runtime; never read them through consuming-game DRAM addresses or project table bindings. Zero vectors use coefficient zero and return zero components and length; the GPF operation sets IR0 to zero. This safe native zero contract does not reproduce the original invalid lookup before the reciprocal table or its image-dependent IR0 value.

### psx_vblank_bind_native_counter

`psx_vblank_bind_native_counter(counter)` binds a reviewed guest SDK counter to native VBlank delivery while retaining the existing native and BIOS callback registries. Each delivered native VBlank increments the bound word before callbacks; passing zero removes the binding. `psx_vblank_bind_guest` retains its separate eight-entry guest callback-table route, which takes precedence when installed. Consuming games must derive the counter address from their original SDK callback and must not increment it from rendered frames or input polling.

### _SsNoteOn

`_SsNoteOn` preserves channel mute/velocity handling and complete voice-call arguments. `_SsVmKeyOff` uses the original voice-record matching and pending-mask path. `_SsVmKeyOn`, `_SsVmVSetUp` and `_SsVmSelectToneAndVag` use the numeric VM binding; noise service remains a named fail-fast boundary.

### _SsVmKeyOff

`_SsVmKeyOff` scans unprotected voice records with signed halfword matching and fresh signed voice-count reads. `vmNoiseOff` clears the original record fields; `_SsVmKeyOffNow` clears record key/pitch fields and updates pending-keyoff and key-on masks in original order. All three reuse the existing numeric VM binding. Device playback and physical interrupt timing are separate from SDK cache publication.

## Windows Release runtime contract

- Release uses ordinary `fseek`/`ftell`; native disc/stream files and offsets must fit signed 32-bit positions
- Windows Release omits separate resource `ferror` checks and PC-file `fflush`; reads retain their returned byte counts, and closing a stream commits buffered writes
- `--collect` is Debug-only; Release omits collection state and logging
- Debug retains resource error checks and PC-file flushing
- Release disables compiler stack probes with `/Gs2147483647`; do not provide an empty `__chkstk` implementation
- Before accepting a probe-free Release, check generated code for remaining probes and audit large or dynamic stack allocations; move page-sized buffers to heap or persistent storage when necessary

SPU rendering and XA sector decoding use private transient static workspaces under the existing audio lock. They are not serialized state. Clear render sends for each chunk; never allocate the 37632-byte XA decode workspace on an unprobed thread stack.

## Windows Release CRT

`src/platform/win/xcrt.c` is the shared custom C runtime for Windows x86/x64 Release. Architecture-specific ABI helpers live in `src/platform/win/xcrt_x86.asm` and `src/platform/win/xcrt_x64.asm`; both require the MASM build customization. The canonical Windows template and synchronized consuming Windows projects enable it by default; local project icons override the shared default through an adjacent `icon.rc`. Debug on both architectures keeps its standard CRT. Build either existing Release architecture with `/p:UseXcrt=false` to exclude xcrt and restore standard `/MT` linking; do not add separate runtime configurations.

### xcrt

`xcrt.c` supplies `atoi`, `strncpy`, `strcspn`, `strncat`, ASCII C-locale `toupper` and `sprintf`. `strncat` preserves bytes after its terminating NUL; `strcspn` scans until a rejected byte or NUL; `toupper` maps ASCII lowercase and preserves other byte values and EOF. `atoi` accepts ASCII whitespace, optional sign and decimal digits with 32-bit unsigned accumulation. `strncpy` copies at most the requested count and pads the remaining bytes with zero. Debug and standard-CRT Release do not compile these definitions.

### platform

C++ consumers include shared `platform/win/xcrt.h` with `XCRT_CPP_IMPLEMENTATION` defined in exactly one C++ translation unit. The header emits definitions only under `XPORT_XCRT`; Debug, Web and `UseXcrt=false` retain their platform runtime. Scalar/array `new` use `malloc`, and scalar/array, nothrow and compiler-emitted sized `delete` use `free`. Zero-size allocations request one byte. Ordinary allocation failure aborts because XCRT has no C++ exception unwinding; nothrow allocation returns null on failure. Placement forms remain supplied by `<new>`. C++98 syntax is retained; sized deallocation is enabled for current MSVC and compilers advertising it. `_purecall` aborts in `xcrt.c`. This does not supply STL, `atexit`, dynamic global constructors/destructors, `new_handler`, exception objects or aligned allocation. Keep required Release registries in zero-initialized POD storage and avoid relying on CRT startup initialization.

### __chkstk

Enabled: `/NODEFAULTLIB`, `/ENTRY:WinMainCRTStartup`, `/Zl`, kernel32/user32/gdi32/winmm/shlwapi imports only. Both Release architectures disable C++ exceptions, RTTI, GS, SDL, CFG, ASLR/relocations, manifest and ordinary stack probing; x86 also disables SAFESEH; omits frame pointers and enables COMDAT/ref elimination. DEP remains enabled. Read-only data merges into `.text`; retain `.data` separately to avoid expanding zero-initialized memory on disk. Large Release automatic allocations must move to suitable owned storage before stack probing can remain disabled. Both architectures use `/Gs2147483647`. XCRT x64 still supplies `__chkstk` for objects compiled with other probe thresholds, without linking the Microsoft CRT.

### xcrt_x86

Runtime contracts: process-heap allocation; WinAPI memory/string helpers implemented in C; x86 integer arithmetic helpers and non-unwinding setjmp/longjmp in `xcrt_x86.asm`; x64 non-unwinding setjmp/longjmp preserving RBX/RBP/RSI/RDI/R12Р В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р Р†Р вЂљРІвЂћСћР В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В Р вЂ Р В РІР‚С™Р Р†РІР‚С›РЎС›Р В Р’В Р Р†Р вЂљРІвЂћСћР В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р Р†Р вЂљРІвЂћСћР В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В Р’В Р Р†Р вЂљР’В Р В Р’В Р вЂ™Р’В Р В Р вЂ Р В РІР‚С™Р РЋРІвЂћСћР В Р’В Р Р†Р вЂљРІвЂћСћР В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р Р†Р вЂљРІвЂћСћР В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В Р вЂ Р В РІР‚С™Р Р†РІР‚С›РЎС›Р В Р’В Р Р†Р вЂљРІвЂћСћР В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В Р вЂ Р В РІР‚С™Р вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р В РІР‚В Р В Р’В Р Р†Р вЂљРЎв„ўР В Р Р‹Р Р†РІР‚С›РЎС›Р В Р’В Р вЂ™Р’В Р В Р’В Р В РІР‚в„–Р В Р’В Р В РІР‚В Р В Р вЂ Р В РІР‚С™Р РЋРІР‚С”Р В Р Р‹Р РЋРІР‚С”Р В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р Р†Р вЂљРІвЂћСћР В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р В РІР‚В Р В Р’В Р Р†Р вЂљРЎв„ўР В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р Р†Р вЂљРІвЂћСћР В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В Р’В Р Р†Р вЂљР’В Р В Р’В Р вЂ™Р’В Р В Р вЂ Р В РІР‚С™Р РЋРІвЂћСћР В Р’В Р В Р вЂ№Р В Р вЂ Р Р†Р вЂљРЎвЂєР РЋРЎвЂєР В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В Р’В Р Р†Р вЂљРІвЂћвЂ“Р В Р’В Р вЂ™Р’В Р В Р’В Р В РІР‚в„–Р В Р’В Р В РІР‚В Р В Р вЂ Р В РІР‚С™Р РЋРІР‚С”Р В Р Р‹Р РЋРІР‚С”R15, XMM6Р В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р Р†Р вЂљРІвЂћСћР В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В Р вЂ Р В РІР‚С™Р Р†РІР‚С›РЎС›Р В Р’В Р Р†Р вЂљРІвЂћСћР В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р Р†Р вЂљРІвЂћСћР В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В Р’В Р Р†Р вЂљР’В Р В Р’В Р вЂ™Р’В Р В Р вЂ Р В РІР‚С™Р РЋРІвЂћСћР В Р’В Р Р†Р вЂљРІвЂћСћР В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р Р†Р вЂљРІвЂћСћР В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В Р вЂ Р В РІР‚С™Р Р†РІР‚С›РЎС›Р В Р’В Р Р†Р вЂљРІвЂћСћР В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В Р вЂ Р В РІР‚С™Р вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р В РІР‚В Р В Р’В Р Р†Р вЂљРЎв„ўР В Р Р‹Р Р†РІР‚С›РЎС›Р В Р’В Р вЂ™Р’В Р В Р’В Р В РІР‚в„–Р В Р’В Р В РІР‚В Р В Р вЂ Р В РІР‚С™Р РЋРІР‚С”Р В Р Р‹Р РЋРІР‚С”Р В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р Р†Р вЂљРІвЂћСћР В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р В РІР‚В Р В Р’В Р Р†Р вЂљРЎв„ўР В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р Р†Р вЂљРІвЂћСћР В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В Р’В Р Р†Р вЂљР’В Р В Р’В Р вЂ™Р’В Р В Р вЂ Р В РІР‚С™Р РЋРІвЂћСћР В Р’В Р В Р вЂ№Р В Р вЂ Р Р†Р вЂљРЎвЂєР РЋРЎвЂєР В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В Р’В Р Р†Р вЂљРІвЂћвЂ“Р В Р’В Р вЂ™Р’В Р В Р’В Р В РІР‚в„–Р В Р’В Р В РІР‚В Р В Р вЂ Р В РІР‚С™Р РЋРІР‚С”Р В Р Р‹Р РЋРІР‚С”XMM15, MXCSR and x87 control words; minimal WinMain startup without argc/argv parsing or environment lookup; strtoul; unbuffered native files. `FILE *` carries a HANDLE and must never be dereferenced or passed to another CRT. File positions use signed32 fseek/ftell and SetFilePointer; files must be smaller than 2 GiB. Append uses FILE_APPEND_DATA, including after seeks. Text modes retain raw bytes without CRLF translation. Serialize compound operations on a shared file handle in the caller.

### snprintf

`snprintf` calls `wvnsprintfA` with the supplied capacity and va_list; `sprintf` calls `wvsprintfA` for existing resource-name callers. Both use WinAPI formatting; `sprintf` requires caller-owned capacity and a result shorter than 1024 bytes. It uses WinAPI format syntax and truncation/error return semantics, without C99 extensions or float formatting. No remove/rename/vsnprintf, printf/scanf, standard streams, fprintf/vfprintf, vsprintf, fgetc/fputc/fputs, fflush or 64-bit seek APIs. Console diagnostics and journal/trace-stream helpers are Debug-only. Resource reads retain size bounds; additional read-error diagnostics are Debug-only. Debug `--collect` truncates the log on startup and writes each normalized resource path once through `fwrite`, using at most 1024 in-memory name hashes.

### XPORT_AUDIO_OUTPUT

Windows `XPORT_AUDIO_OUTPUT` and `XPORT_AUDIO_BACKEND` are read only under `_DEBUG`. Release uses ordinary audio output; headless execution uses the null sink.

### tests

An absent or unrecognized value keeps the headless-dependent default. The null sink retains SPU field processing and its counters; it removes physical-device queue backpressure. Use it for graphics/state debugger audits and label those results as excluding physical audio timing and audible acceptance. Retain separate WaveOut checks. Verify selection with `tests.test_audio_field_clock.AudioFieldClockTests.test_debug_null_sink_is_independent_of_graphics_and_ignored_by_release`.

Only Debug WinMain recognizes the exact `--collect` argument; Release compiles out collection and ignores the switch. Neither build creates argc/argv. Windows has no platform argument array or executable-path buffer. xport_arg_count/xport_arg_values are removed from the shared API; consuming projects own migration of any former callers.

XCRT startup initializes the process heap and calls WinMain with GetModuleHandleA(NULL), NULL, GetCommandLineA() and SW_SHOWDEFAULT, then ExitProcess. No GetStartupInfoA, environment block, command copying or executable-name parsing. XPORT_RASTERIZE/XPORT_VERIFY_VRAM environment overrides are Debug-only; Release headless rasterization is disabled.

MDEC floor rounding uses arithmetic shifts, including negative values, in IDCT and YUV conversion. Do not implement division by powers of two through general 64-bit division helpers in these hot paths. XCRT x86 integer division uses shared MASM helpers: hardware DIV for 64/32, a 32-step remainder loop for a wide divisor. x64 uses native integer instructions. Its sin/cos/atan2/sqrt float/double entry points marshal XMM arguments/results to a balanced x87 stack; trigonometric arguments retain the same practical game-range limits as the x86 x87 helpers.

Windows Release template uses /O1 and /Gw. Collect output uses ../src/platform/web/collect.log relative to the native working directory (bin). abort terminates pseudo-handle (HANDLE)-1. No GetLastError/SetLastError, GetCurrentProcess, GetModuleFileNameA, VirtualQuery or UpdateWindow calls. Presentation owns drawing; there is no custom WM_PAINT handler. Windows xport_memory_readable performs only numeric native-address/nonempty/overflow checks, not OS page validation; callers own pointer validity.

Canonical Windows template and RapidRacer share Debug/Release compiler, linker and xcrt settings. Debug enables XPORT_ENABLE_AUDIT and XPORT_MEMORY_DIAGNOSTICS; Release disables function markers. Keep game-specific bindings such as XPORT_NATIVE_GPU_CD in the consuming project, not the template.

## Analog gamepad input

Windows XInput and web Gamepad input latch both sticks when xport_input_read is called. xport_input_analog returns the availability flag and four PS1 bytes in RX,RY,LX,LY order: left/up=0, center=128, right/down=255. By default, analog axes and digital D-pad/keyboard/touch buttons remain independent. Windows inverts XInput Y; browser axes already point down. Missing devices center the axes, and input overrides return centered axes with no live analog device.

XPORT_PAD_HACK defaults to 0. Define XPORT_PAD_HACK=1 only in a consuming project that requires the compatibility mapping: the left stick ORs digital D-pad directions above 40% per axis, and physical D-pad/keyboard directions on both ports override the pressed left-stick axes; web touch directions also do this on P1. Opposite directions produce 128, untouched axes retain sampled analog values, and right-stick values/device availability remain unchanged. Synthesized D-pad bits never feed back into analog axes. Keep the option identical across all compiled input units; do not enable it globally or in the startup template.

Native direct PAD buffers, owner packets and serial read replies publish 0x73 and all four axes for analog gamepads in analog mode, or 0x41 in digital mode or for keyboard input. Serial replies latch a single report at command 0x42. Replay-bound packets retain their recorded analog values and bypass native publishing.

### PadInitDirect 2

Native PAD starts in digital mode, including when the host controller has analog sticks. `PadInitDirect` restores digital mode. Report analog capability through `PadInfoMode`; switch packet format to `0x73` only after the game requests analog mode through `PadSetMainMode`. Do not infer the selected PsyQ protocol mode from host-device capability: digital-only games reject the entire `0x73` report, including ordinary buttons. Verify digital startup with an analog-capable host, both ports, explicit analog/digital selection, preserved stick bytes and reinitialization in `tests.test_pad_ports`.

Native PadSetMainMode accepts ports 0/0x10 and modes 0=digital, 1=analog for a connected analog-capable device after PadStartCom. It completes synchronously and refreshes direct/owner reports; subsequent serial replies use the selected mode. Keyboard-only input, missing initialization/devices, invalid ports/modes and lock values outside 0..3 return 0. Lock 2 unlocks, 3 locks the host mode button, 0/1 preserve the lock state; host input has no analog-mode button, and explicit game API requests remain allowed while locked. Both ports have independent mode/lock state, reset to digital/unlocked by PadInitDirect.

Native PadInfoMode terms: 1=current ID (4 or 7), 2=extended analog-capable model ID (7 or 0), 3=current mode offset (0 or 1), 4=supported ID table (offset -1 returns count; entries 4 and 7 for analog-capable input, only 4 for keyboard). It exposes analog capability even while the selected mode is digital. An explicitly bound SDK owner takes priority in PadInfoMode and PadSetMainMode, retaining its query fields and queued commands even outside replay; the native analog override applies only when no owner is bound.

### spu_bind_key_guest

Bind SDK key storage through `spu_bind_key_guest(state, event_mode, event_flags, pending_mask, registers)` after guest memory binding. Pass the original SDK addresses; `registers` names the eight-byte pending key-on/key-off halfword block. `SpuSetKey` masks voices to 24 bits, rejects modes other than 0/1, publishes immediate key-state writes, and implements event-mode queue writes and cancellation of opposite pending bits. An unbound consumer retains direct host key operations. Binding changes configuration only; it must not seed or copy guest key state.

### SsVabOpenHead

Native `SsVabOpenHead` permits reserved tone groups: the header program count locates the sample-size table, while nonempty program records select packed tone groups. The active group count must fit the declared capacity; equality is not required. Apocalypse `level7.vab` has five reserved groups and four active programs.

### spu_clear_reverb_guest

`spu_clear_reverb_guest(guest, begin, bytes, source, direction)` clears an explicitly reviewed SPU byte range through the common synchronous block-transfer core. The caller owns the original allocation-table check and range derivation. Source maps a readable 1024-byte guest buffer; direction maps four guest bytes. The adapter installs a native guest transfer binding only when none is active, rejects mismatched existing guest fields, active transfers, failed backends and ranges outside SPU RAM, and preserves the requested mode, normalized mode and callback. It does not change ordinary SpuClearReverbWorkArea binding behavior. Native validation: `python -B tools/gpu_observer_test.py --fixture spu_control --configuration Debug` from a consuming project; covers two-block writes, adjacent sentinels, field restoration and binding/range rejection. This native backend does not establish physical DMA or BIOS interrupt timing.

### PAD_init2

BIOS legacy joypad entry points `PAD_init2(type, destination)` and `PAD_dr()` use the existing pad polling service. Valid types are `0x20000000/1`; initialization returns 2, invalid types return 0. The packed destination uses each controller's original button-byte order, excludes unsupported IDs and is updated only while pad communication is active. A successful direct `InitPAD` supersedes the legacy destination. Kernel heap reinitialization clears legacy buffer ownership. Existing consumers require no migration. Run `python -B -m unittest tests.test_bios_pad_optional_port -v` from `tools`; the test covers two ports, packed legacy input, rejection and stopping. Contract: [BIOS joypad functions](https://psx-spx.consoledev.net/ps1/kernelbios/joypad-functions/). Consuming entry: `Pandemonium2-xport:8008373C`.

### ApplyRotMatrixLV

`ApplyRotMatrixLV(const VECTOR *source, VECTOR *destination)` accepts native pointers to three signed32 components and returns `destination`. It reads all inputs before writing outputs, permits overlapping vectors, preserves the destination pad and retains the two GTE commands, register effects and low-word wrapping of the numeric implementation. `ApplyRotMatrixLVPSX` retains its numeric guest-address ABI, scalar guest-memory accessor sequence and original destination-address return. No game bindings or required consumer migration. Verify from `tools`: `python -B -m unittest tests.test_apply_rot_matrix_lv tests.test_gte_scale -v`;8192 numeric/native cases compare all memory and complete GTE state. This establishes ABI conservation against the preserved implementation, not independent original-machine equivalence.

### MulRotMatrix

`MulRotMatrix(MATRIX *matrix)` and `MulRotMatrix0(const MATRIX *source, MATRIX *destination)` accept native packed MATRIX rotation spans of20 bytes. They preserve three0x486012 GTE operations, signed halfword loading, all source reads before output stores, the five output words including the upper halfword at offset18, and destination pointer returns. Translation bytes20..31 remain untouched. The existing numeric PSX entry points retain their scalar guest-memory implementation and numeric returns. Pandemonium2 uses the native forms at explicit mapped scratch/resource boundaries; migration and final Debug x86 verification remain pending.

### SquareRoot12

`SquareRoot12(sint32 input)` uses the existing native192-entry PsyQ square-root table for inputs0..0x7FFFFFFF. It retains LZCS/LZCR GTE effects,normalization,table quantization,signed coefficient loading,masked shifts and zero return. It does not call host floating-point sqrt or the GTE-free SquareRoot0 path. Signed original layouts can address data before the table and require their reviewed compatibility boundary;Pandemonium2 keeps SquareRoot12OriginalLayout for that case. SquareRoot12PSX and its numeric table/address behavior remain unchanged. Native migration is unverified until final Debug x86 verification.

### InitPADNative

`InitPADNative(buffer0,size0,buffer1,size1)` is the native pointer form of `InitPAD`: port0 requires a non-null buffer with 8..PSX_DRAM_SIZE bytes; port1 is either null/zero or has the same bounds. The caller owns stable writable buffers until re-registration. It retains InitPAD service initialization, disconnected publication and StartPAD lifecycle; it does not reset main mode as PadInitDirect does. No host pointer is written to BIOS guest buffer descriptors. Numeric InitPAD and replay bindings retain their existing contracts. Native project save/reset lifecycle must preserve or re-register the buffers; this entrypoint alone does not serialize native game state. Structural implementation pending final Debug verification; P2 evidence: status/native-refactoring/gamepad-packet.json

## Native CD command ownership

For game asset reads, [DECOMP_REFERENCE.md#runtime-file-access](DECOMP_REFERENCE.md#runtime-file-access) takes precedence: use extracted DATA and `xport_fopen`, replace drive-only readiness/seek wrappers, and never use disc images during gameplay. The CD compatibility contracts below describe retained shared stream/audio services; they do not require reimplementing a CD controller in each game.

### XPORT_NATIVE_CD

- Define `XPORT_NATIVE_CD=1` to select native CD ownership independently of the renderer. `xport.h` makes the four legacy CD address bindings zero in this mode; consuming games must remove their corresponding symbol definitions. `XPORT_NATIVE_GPU_CD` retains its existing combined behavior and default clients retain their address-binding contract
- The shared native CD backend owns status, command completion/response, last accepted command, debug level, typed ready/sync/data callbacks, sector FIFO/cursor and XA state. `CdLastCom()` returns the last accepted command; do not maintain a second last-command byte in game memory. `CdStatus`, `CdControlB`, `CdControlF` and `CdSync` use the existing native backend when CD bindings are zero
- `cd_notify_command` publishes the accepted command before invoking its native service. Command failures leave the previous command identity intact. `cd_set_reading_native` in the explicit native CD profile updates this identity to Pause9/ReadS27 while preserving queued STR frames, XA samples and decoder history; it does not reissue a resetting controller command
- Native-profile `CdInit` retains the original five reset attempts and failure diagnostic. Default clients retain the previous single native initialization attempt. Both use the same mounted DATA stream backend and native audio-device setup
- Keep DATA file lookup, virtual-to-file-relative sector translation and reverse translation of GetlocP responses in the consuming game. They are file-boundary mappings, not SDK controller state. Resource reads use `xport_fopen`; no disc-image or original-EXE lookup is introduced
- Use `CdlATV` and `CdMix` for native movie audio mixing, and typed `CdReadyCallback`/`CdSyncCallback`/`CdDataCallback` for handlers. Retire numeric callback dispatch, guest CD event/result arrays and manually repeated DMA completion from native game code when the actual native resource path no longer consumes them
- The CD command owner currently has process lifetime; `CdInit` retains its existing sector/FIFO/read-error reset scope.
- This structural migration is uncompiled and unverified. Final checks must cover accepted/failed commands, implicit Setloc/retries/callback restoration, GetlocP rebasing, file switches/bounds, resource reads, STR pause/resume/end, XA voice playback and affected default shared clients

## Native CD read completion callback

- `CdReadCallback(CdlCB)` exchanges the single native LIBSND read callback under the audio lock and returns its previous value. NULL unregisters it; do not substitute CdDataCallback/DMA3, ready or sync callback slots
- Existing native blocking CdRead transfers keep their result/mode contract. A started transfer with a registered callback queues CdlComplete or CdlDiskError after unlocking through the ordinary CD IRQ/service gate; this does not reconstruct original asynchronous sector scheduling/retry timing
- Pending callback kinds are sync0, ready1 and read2. Read completion snapshots eight response bytes; ready/sync producers supply only their established first byte. Dispatch resolves the current callback and copies response before unlocking, then invokes user code. Never execute callbacks while holding the audio lock
- LIBSND portable IO rejects a nonNULL native read callback; stored-size prefixes reject old incompatible layouts.

## SDK key operation and observed MIPS result

Canonical void SpuSetKey delegates to shared spu_set_key(sint32,uint32). The latter is a host-runtime boundary for translated code that observes the original MIPS result: invalid modes return1 without mutation; immediate mode returns the updated cached key bitmap; queued mode returns opposite high-halfword overlap, or its cleared value when overlap exists. A low-halfword-only cancellation still returns0. Both APIs use one shared operation and owner; retain native and optional guest bindings, audio lock/device synchronization, trace hooks and host pending-start cancellation. Do not reconstruct these queues or cached fields independently in a consuming game. Preserve original caller markers at thin project boundaries until their provenance adapters can be removed. P2 evidence: status/native-refactoring/native-spu-key-operation.json. Final native/guest/shared-consumer checks remain pending.

SpuKeyControl owns pending key_on/key_off/pitch_mask/noise_mask/reverb_mask pairs in that reset order. These CPU queues are distinct from live DSP masks. With no guest-key binding and event_mode bit0 set, SpuSetReverbVoice modes0/1 modify only the pending reverb pair and OR event_flags with8; other modes query without mutation, except mode8 retains its existing configuration-specific contract: native VM immediately replaces live registers, non-VM queries the live mask. Selection and pending read/modify/query share one audio lock. The private pending helper assumes that lock and performs no MMIO. VM immediate MMIO retains its prior lock span. Guest bindings remain setup-owned and must not change concurrently merely because the reader is locked. No new public pitch setter, flush, noise behavior, transfer rebinding or DSP metadata setter is introduced.

The expanded SpuKeyControl changes nested SPU_STATE layout. No complete failed-load rollback is promised.

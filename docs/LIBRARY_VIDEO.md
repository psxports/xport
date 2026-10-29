# Runtime video contracts

Section routing and verification provenance: [LIBRARY.md](LIBRARY.md).

## Native ClearImage2

- `ClearImage2(PSX_RECT *rect, uint8 r, uint8 g, uint8 b)` owns the native GPU clear packet and returns0 after `DrawOTag`. Null rectangles and packet-range registration failure abort; no game DRAM binding is required
- Clamp width/height in the supplied rectangle to0..`GPU_VRAM_WIDTH-1` and0..`GPU_VRAM_HEIGHT-1`. Preserve the original low16-bit XY/size packing, E1bit0x400 and draw-mode flags
- For x/width aligned to64 pixels use the GPU fill command. Otherwise submit a tile with full draw area and zero offset, followed by captured draw-area/offset restoration. Keep mask-clear and packet order; do not replace this path with `ClearImage`
- Packet storage is shared static14-word state, rewritten for each selected path. Registration occurs on every call. Preserve the existing `DrawOTag` lifetime/reentrancy contract; this migration does not change GPU/OGL algorithms or establish complete PsyQ equivalence

## GPU environment and GTE readback

- `GetDrawEnv(DRAWENV *output)` copies the current92-byte environment from the bound PsyQ cache or the same GPU owner's native `PutDrawEnv` cache and returns `output`. Null output or no native environment returnsNULL without writing output. GPU resets invalidate the native cache
- `ReadColorMatrix(MATRIX *output)` reads controls16..20 into the first20 bytes, then21..23 into the final12 bytes. Preserve matrix padding and far-color fields; no GTE state is changed
- `SetFogFar(distance, projection)` uses wrapped32-bit negative distance shifted6, signed division and DQA/DQB writes. DQB is0x01400000. Division by zero or INT_MIN/-1 aborts; this API is not interchangeable with `SetFogNear`
- Verify `python -B tools/tests/graphics_readback_sdk_test.py --architecture x86 --output OUTPUT` and repeat with `--architecture x64`. The isolated fixture checks actual publication/getter functions, GTE accessors, matrix bytes, fog arithmetic and trap boundaries. Raster consumers use a test sink; reset invalidation is checked statically, not by a game/render/Web run

## Native GTE render contexts

- Data register23 (RES1) retains a full32-bit value through `xport_gte_read_data`/`xport_gte_write_data`. It does not affect GTE arithmetic. Native context save/restore includes it; `PsxGteSnapshot` remains unchanged
- `psx_gte_save_context(PsxGteContext *context)` captures native GTE registers,screen-offset fractions and LZCS/LZCR into caller-owned storage
- `psx_gte_restore_context(const PsxGteContext *context)` restores that complete register context directly;do not reconstruct it by replaying register writes because SXYP,IRGB and read-only aliases have write side effects
- Null arguments are no-ops. Neither operation executes a GTE command,dispatches a callback or accesses game memory

The new context does not include the PsyQ SDK matrix stack,game matrix slots or lighting owners;save those separately when a replay uses them

### python

- Use a saved caller context around isolated OG render calculations. Restore it before returning;do not change simulation time or repeat gameplay updates to rebuild a render result
- Verify `python -B -m unittest tests.test_gte_context tests.test_gte_scale tests.test_gte_math_sdk -v` from tools. The context check covers all readable data/control registers,fractional screen offsets,FIFO/IRGB aliases,LZ state and nested restores

## GPU reset and status boundaries

- Genuine PsyQ `_status(void)` reads GP1/status at1F801814. P2 status-bit31 wrapper has no driver argument:its original omitted prelude loads a fixed jump table,not an object parameter
- P2 queue reset retains ordered DMA/DPCR/GP1 writes at confirmed physical device ports and local saved IRQ mask. Unread legacy guest queue indices,command workspace,environment/dimension/cache clears are retired;shared native raster state and original callback owner remain authoritative. Optional guest observation bindings for other consumers are unchanged
- P2 mode0 reset specialization still awaits full SDK consolidation. No runtime-equivalence claim

## Native GPU control cache

- `GpuGraphControl gpu_graph_control` owns byte-sized graph type,queue flag,reverse flag and256 command bytes.
- Genuine PsyQ `_ctl(command)` writes GP1 port1F801814 before caching its low byte by command>>24;`_getctl(index)` returns the zero-extended cache byte. Invalid indices>=256 abort instead of reading unrelated memory. Direct queue-reset GP1 writes bypass this cache as in the original

Existing guest observation bindings remain separate contracts;full shared LIBGPU ownership and snapshot compatibility policy are not declared complete

## Native GPU debug ownership

- `graph_debug_level` in `psx_gpu.h` exposes the existing single SDK owner;`SetGraphDebug` and portable GPU scalar storage retain their current sint32 contract. Original byte-oriented consumers read its low uint8 value directly;do not add a DRAM mirror or invented getter

SetGraphReverse/SetGrapQue omitted IDA preludes read this state;their native entries take only the actual reverse/enabled argument.

- Native debug/reset and ABI changes are unverified pending final Debug x86;GPU queue/cache/other environment ownership remains separate migration work

## Graphics presentation

### Color and alpha fog
- `vid_fog(fog)` enables two-stage fog for subsequent custom-shader draws in the active frame. Supply `VID_FOG` with radial world distances `0 < near_dist < far_dist < horizon_dist`, normalized RGB `color`, and positive `focal[2]` matching the current projection (`clip.x = camera.x * focal[0]`, `clip.y = -camera.y * focal[1]`, `clip.w = camera.z`). `vid_fog(NULL)` disables it. Each draw snapshots the parameters
- The vertex shader reconstructs camera-space position and computes its length. It clamps logarithmic coefficients `log(max(d,near)/near)/log(far/near)` and `log(max(d,far)/far)/log(horizon/far)`, then squares each to reduce early fog density. Rasterization interpolates the coefficients; the fragment shader performs no distance, logarithm or smoothstep calculation. This approximation depends on polygon tessellation
- Custom fragment shaders call `vidFog(color)` after computing their surface color. Opaque/alpha RGB first blends into the supplied fog color; alpha then fades to zero using the second coefficient. Fully transparent fragments are discarded before depth writes. Additive/subtractive materials fade RGB toward zero using the first coefficient. Default VID shaders do not apply fog
- Fog-enabled opaque custom-shader materials use ordinary source-alpha blending; other blend modes and depth-write policies remain intact. Render horizon first and disable fog before UI. The second range reveals existing framebuffer contents, including any earlier geometry; no background texture, GPU copy or extra texture sample is used
### HD float transforms

### xport

`xport.h` defines `VEC3F` (`x/y/z`, `v[3]`) and `QUATF` (`x/y/z/w`, `v[4]`). `psx.h` defines `VECTORF` (`vx/vy/vz`) and `MATRIXF` (`float m[3][3]`, `float t[3]`). These types and the float operations in `psx.c` serve HD rendering only. Consuming projects retain their integer physics, collision, AI and simulation state; HD reads a simulation sample and must never write float transform results back to it.

Float coefficients use identity `1.0f`, translations use world units, and rotation angles use radians. Convert encoded assets and simulation inputs at the consuming game's render boundary. Build float poses from source angles, quaternions or basis vectors; converting a completed fixed-point render matrix is not a completed HD migration. Float operations accept valid finite inputs and non-null pointers, do not use the COP2 register bank, and do not emulate fixed-point rounding, wrapping or saturation.

### ApplyMatrixf

`ApplyMatrixf` applies the 3x3 part only; `RotTransf` also adds translation. `MulMatrix0f(left, right, dst)` computes `left.m * right.m` and preserves `dst.t`; `MulMatrixf` writes the left operand and `MulMatrix2f` writes the right operand. `CompMatrixf` composes both rotation/scale and translation. `TransposeMatrixf` transposes only the 3x3 part and preserves destination translation. All these operations support destination aliasing an input. `IdentityMatrixf` resets both rotation and translation.

### RotMatrixf

Axis rotations premultiply the 3x3 part and preserve translation. `RotMatrixf` builds `Rx * Ry * Rz`; `RotMatrixYXZf` builds `Ry * Rx * Rz`; both preserve destination translation. `ScaleMatrixf` scales columns, and `TransMatrixf` replaces translation. `NormalizeVectorf` returns the original length and outputs a unit vector; zero input outputs zero. `NormalizeQuatf` returns the original norm and outputs a unit quaternion; zero input outputs identity quaternion. `MulQuatf` uses Hamilton multiplication and supports in-place destinations. `QuatMatrixf` normalizes the input orientation, builds its 3x3 rotation and preserves translation.

### OuterProductf

`OuterProductf` computes a cross product and supports destination aliasing either input. `ProjectVertexf` projects an already transformed camera-space vector. `RotTransPersf` first applies the supplied matrix, then delegates to `ProjectVertexf`. Both projection entries return homogeneous clip coordinates, retain float depth in `w`, and reverse camera Y for OpenGL/WebGL presentation. Callers supply focal coefficients and `0 < near_z < far_z`. Near/far map to NDC depth -1/+1. They perform no perspective divide, viewport rounding or visibility rejection; the renderer owns clipping and viewport policy.

### python 2

Run `python -B xport/tools/tests/gte_float_test.py --output PROJECT_ROOT/status/audits/vid` on a Windows MSVC host. The fixture compiles the exact current float function closure with `/Od /MDd /D_DEBUG` and enabled assertions. It checks rotation order, composition, in-place aliases, fractional vectors, normalization, scale, projection planes and quaternion poses. It does not prove game integration or a complete runtime COP2 snapshot comparison.

The shared shader-only VID interface, OpenGL/WebGL build inputs, resource ownership and OG/HD transition contract are in [VID.md](VID.md).

Single-segment Gouraud GP0 `50/52` lines use native integer color interpolation with the existing clipping, draw offset and semitransparency path. The GPU observer fixture covers endpoints, midpoint, clipping, offset and blending; this does not establish hardware-exact line coverage or interpolation. Gouraud polylines are outside this implementation.

## Native INTPL and unmodulated lighting commands

- Entry: existing `xport_gte_execute` in `src/psx.c`; commands INTPL `0x11`, NCS `0x1E` and NCT `0x20`
- Reuse the existing GTE state, matrix, MAC/IR overflow and saturation, RGB FIFO and FLAG primitives; honor both shift and lm bits
- INTPL retains signed 32-bit far-color registers and saturates the intermediate signed difference before interpolation
- NCS performs the light and color transforms without color modulation; NCT applies those stages to V0, V1 and V2 with cumulative FLAG and ordered RGB FIFO updates
- Existing public `gte_intpl12`, color-modulated helpers and register/state ownership remain unchanged
- AC3 authorization and bounded register validation: consuming project `status/native-sdk-migration/native-gte-operation-route-proposal.json` and `native-gte-added-ops-test.json`; these fixtures do not establish hardware timing or full-game equivalence

## Native packed-color GTE access

- Entry: existing `xport_gte_write_data/read_data` in `src/psx.c`
- Writing IRGB data register28 unpacks the low15 RGB555 bits into current IR1, IR2 and IR3 scaled by128; high input bits are ignored
- Reading IRGB28 or ORGB29 derives RGB555 from signed IR1/2/3 divided by128 and saturated to0..31; ORGB writes preserve state
- These accesses preserve MAC, RGB FIFO, FLAG and all other state; no separate IRGB storage, memory domain or driver is introduced
- AC3 authorization and bounded validation: consuming project `status/native-sdk-migration/native-gte-packed-color-proposal.json` and `native-gte-packed-color-test.json`; actual game scenario acceptance remains separate

## Batched triple projection

- `RotTransPers3N(vertices, screen_xy, depth, flags, groups)` in `psx.h`/`psx.c` uses the existing `gte_project3_full_depth` and GTE state owner
- Each group consumes three contiguous8-byte `SVECTOR` values and writes three packed32-bit XY values, three unsigned16-bit full depths and one16-bit `FLAG >> 12`
- Buffers must cover `max(groups,1)` groups; zero and negative counts still process one triple. `INT32_MIN` aborts after projection before output stores
- Stage each complete input triple before projection and publish XY, depths, then flags; later groups observe any earlier output alias writes
- Validate packed widths, alias order, nonpositive counts and same-owner GTE state with `python -B tools/tests/gte_batched_projection_test.py --architecture x86 --output OUTPUT` and the equivalent x64 command
- Fixtures establish the API contract; game rendering, Web behavior and original hardware timing require separate verification

### psx_identity_matrix

`psx_identity_matrix` is the immutable fixed-point identity MATRIX with diagonal4096 and zero translation. `psx_identity_rotation_words[5]` is the same rotation in packed GTE control-register order,including zero upper padding in the fifth word. Use the matrix for native PsyQ matrix inputs and the words for packed register/record copies;neither depends on a consuming game's DRAM or executable tables. Preserve non-const SDK signatures by passing a local matrix copy when required;do not cast away const.

### FIELD_RATE

`FIELD_RATE` remains the nominal SDK rate. The default physical VBlank ratio follows DuckStation: FIELD_RATE=50 selects 53203425/(3406*314) Hz (PAL); FIELD_RATE=60 selects 53693175/(3413*263) Hz (NTSC). Other rates retain FIELD_RATE/1. Explicit FIELD_RATE_NUMERATOR / FIELD_RATE_DENOMINATOR overrides remain supported; define both for a custom profile. Native VBlank polling, field pacing, GPU skip deadlines and Windows-null/Web PCM counts share this ratio. xport_frame_fract(uint64 *idx) returns the phase and publishes the frame index from one timestamp; xport_frame() returns only the absolute physical field index from the same calculation. xport_timer_wait_frame(target_field) checks that index after each Sleep(1) on Windows or emscripten_sleep(1) on Web; an already reached target returns immediately. No inverse field-to-time conversion or accumulated rounded periods are used. Consumers use these helpers for logic indices and interpolation phases; nominal SDK tick constants remain unchanged.

### xport_gte_execute

GTE `xport_gte_execute` supports GPF (0x3D) and GPL (0x3E), preserving sf/lm, 44-bit MAC overflow, IR saturation and RGB FIFO/code. Verify from tools with `python -B -m unittest tests.test_gte_scale -v`. BIOS `InitPAD` accepts a null/zero unused second port without publishing into DRAM0; enabled buffer validation is unchanged. Verify with `python -B -m unittest tests.test_bios_pad_optional_port -v`. These changes add no bindings or required consumer migrations.

## Modules

Native and Web execute every submitted GPU frame. Blocking `VSync(0)` and `VSync(n>1)` count fields since the preceding completed native blocking wait; fields already delivered by event polling or query modes satisfy that wait. Every completed blocking wait updates its counter. Query modes and provider-bound/SDK-sampled waits retain their contracts. GPU primitives, OT passes, background fills, ClearImage, menu clearing and presentation are not suppressed based on elapsed host time. Hardware interlaced field behavior and the explicit OG/HD raster switch remain unchanged.

## gpu_observer_test 2

Validate GPU raster/state/transfers with `gpu_observer_test.py --configuration Debug`, VSync binding isolation with `--fixture vsync_callback`, and native/Web waits with `python -B -m unittest tests.test_native_timing tests.test_web_timing` from `tools`. Use project-specific final verification gates when required; no gameplay equivalence is implied by isolated fixtures.

## gpu_frame_begin

`gpu_frame_begin` and `gpu_frame_end` bracket a complete host frame across blocking VSync and every viewport/OT submission. `gpu_present` suppresses output while a scope is active; call it after the final end to display completed pixels. Scopes nest, do not suppress GPU commands or transfers, and reset on GPU reset/load.

## GsSortOtPSX

`GsSortOtPSX(source_ot, destination_ot)` sorts a numeric guest five-word LIBGS ordering-table descriptor into another. It preserves packet-length bytes, follows 24-bit links to the terminal tag and retains alias-sensitive read/write order. This additive API requires no new consumer bindings or modules and leaves existing native GPU links unchanged.

## GetODE

`GetODE()` returns GPUSTAT bit31 through the existing supplied GPU status source when bound. Native execution owns `GpuScanoutState` in `psx_gpu.c`: GP1 display origin, vertical range and mode configure scanout; the platform timer advances raster ticks at the PAL/NTSC nominal video clocks. Progressive mode uses display-origin plus scanline parity; interleaved mode uses the latched display field within the active range. Native `PutDispEnv` publishes these commands without guest PsyQ bindings. The scanout state, fractional tick carry and native display cache belong to the size-bound GPU snapshot; prior scalar-block sizes are rejected and loading rebases the host timer. This raster-clock model does not establish CPU-cycle, DMA, IRQ or rendering timing equivalence and does not implement a complete native GPUSTAT register. Game code calls only `GetODE`, without host synchronization or guest MMIO reads.

## GsSetFlatLight

`GsSetFlatLight(index, descriptor)` uses the existing integer `SquareRoot0` and wraps the direction square sum and negated fixed-point numerators to 32 bits. Zero length returns `-1` without publishing matrices. Indices `0..2` replace one direction row and one color column; other indices retain both matrices and return zero after color publication. Bind caller-owned native `MATRIX` objects with `gte_bind_gs_light_matrices` before the call and clear the binding before those objects expire. An absent binding aborts. This new entry adds no required consumer symbols or project modules. A translated guest consumer imports and exports its reviewed LIBGS matrices through its own typed adapter. The binding is transient host state and is outside the portable snapshot contract.

## DrawPrim

Native GP0(02) fills in `DrawPrim` and OT execution convert RGB888 to RGB555, align X down and width up to16pixels, mask width/height to10/9bits and wrap coordinates in1024x512 VRAM. Zero dimensions write nothing. Drawing clip/offset/mask state does not affect fills; disabled polygon raster still executes them. Interlaced480i skips the active display-field parity unless draw-to-display is enabled, using existing scanout state. No API/binding/snapshot migration is required. This does not establish GPU-cycle timing equivalence

## psx 3

PsyQ GPU packet types remain declared in `psx.h`. Use their fixed-width fields directly: signed SDK `long` fields require `sint32`, unsigned words require `uint32`, coordinates require `sint16`. Do not duplicate these types in consuming games. Packet sizes include the four-byte DMA tag: `POLY_FT4` and `POLY_GT3` are 40 bytes, `LINE_F3` is 24 bytes, `SPRT` is 20 bytes, and `DR_MODE` is 12 bytes.

## XY

GPU packet views: `XY.xy` or `XY.x/y`; `RGBC.rgbc` or `RGBC.r/g/b/code`; `UV.uv` or `UV.u/v`. Vertex records are `POINT_C` (RGBC rgbc + XY xy, 8 bytes), `POINT_T` (XY xy + UV uv + uint16 attr, 8 bytes) and `POINT_CT` (RGBC rgbc + XY xy + UV uv + uint16 attr, 12 bytes). `P_TAG` has `RGBC rgbc` at byte 4 and remains 8 bytes; generic command access uses `rgbc.code`. Flat packets keep one `RGBC rgbc`; all vertices are stored only in `p[]`. `attr` is CLUT at vertex 0, texture page at vertex 1, and the original format's reserved/depth-bias word at later vertices; retain each audited consumer's meaning. `LINE_F2..5` use `XY p[]`; `LINE_G2..5` use `POINT_C p[]`. F5/G5 are xport extensions, not original PsyQ typedefs; repeat vertex 0 at vertex 4 to close a quad. `setLineF/G2..5` preserve command/length/terminator semantics. Binary packet layouts remain unchanged; source callers must migrate removed named vertex/color/UV/texture fields to these views. Sprite/tile owners use `p[0]`; generic PsyQ setters remain callable.

## gpu_register_packet_range

For native packet owners, call `gpu_register_packet_range(begin, size)` before linking their packets. It returns one for a registered range, including an exact repeated registration, and zero for null, empty, unaligned, overlapping, guest-memory, exhausted-count or exhausted-link-space requests. Ranges must remain allocated at the same address for the process lifetime; no unregister operation exists. The registry accepts at most 32 disjoint ranges and allocates aligned 24-bit links in `[0x200000, 0x800000)`. Use the native PsyQ functions `ClearOTag`, `ClearOTagR`, `AddPrim` and `AddPrims`; the raw lowercase tag macros do not encode these registered links. `DrawOTag` resolves registered links alongside ordinary guest RAM chains and rejects a registered packet whose header length exceeds its range. Registration is opt-in; existing `gpu_register_dma_range` and legacy pointer tags retain their compatibility path. Do not combine registered links with unregistered native pointer tags in one chain: their 24-bit domains can overlap. Native or guest ordering tables with guest and registered packets are supported.

## PutDrawEnv

`PutDrawEnv` accepts a native `DRAWENV *` without narrowing the pointer to a guest address. Packet encoding, observer submission, raster application and the bound PsyQ cache use that same owner. `gpu_put_draw_env` and `gpu_draw_env` retain their explicit guest-address contracts for translated device boundaries. A native environment cannot be submitted to a guest-only linked-submit hook; that hook reports failure instead of inventing a guest mirror.

## rsincos

Use `rsincos(angle, &sine, &cosine)` when a translated game function reads PsyQ's packed `rcossin_tbl` directly. This function computes both original signed 16-bit table values for the same angle and is not equivalent to calling `rsin` and `rcos` separately. Do not expose or duplicate the packed guest table solely for those reads. The private `gte_sincos_quarter[1024][2]` in `psx.c` reconstructs all 4096 packed pairs by quadrant swaps and signs, preserving duplicated endpoints. The reconstructed 16384-byte table has SHA-256 `b106234165685d79392c926192d1b2f9e30125307abde6040747e401a35d0e87`; it matches both reviewed PsyQ `LIBGTE.LIB` distributions and the classified original library data. Scalar `rsin`/`rcos` retain their separate 1025-entry quarter table.

## psx_irq_native_mask_exchange

`psx_irq_native_mask_exchange(mask)` returns the previous 16-bit native IRQ mask and applies the new mask through `psx_irq_write` to the existing native interrupt state. It does not register or replace an MMIO handler, allocate additional state or dispatch interrupts. Existing consumers need no migration. Gran Turismo uses it for its reviewed `0x1F801074` mask exchange after the observed GPU reset blocker; other unresolved device registers retain fail-fast boundaries.

## psx_irq_native_acknowledge

`psx_irq_native_acknowledge(sources)` clears the selected native IRQ status bits through the existing `psx_irq_write` state contract and rejects masks outside the eleven IRQ sources. `ResetGraph(1)` also supports the synchronous native rasterizer when distinct DRAM queue head/tail words are bound by `gpu_bind_soft_reset`, and no transfer observer, linked submit hook or asynchronous sync sampler is installed. It clears the bound queue, acknowledges GPU IRQ and issues GP1 IRQ/FIFO reset commands while retaining VRAM and drawing/display state. Native packet submissions are complete calls, so this path has no partial FIFO or active hardware DMA transfer. Other reset paths retain their previous contracts; asynchronous modes remain unsupported by this native path. Gran Turismo scopes this binding to its observed `0x1F8010A8` startup blocker and keeps unresolved hard-reset hardware accesses fail-fast.

## VectorNormalS

`VectorNormalS` snapshots the three input components before output stores, truncates them to signed16 IR values, executes the original SQR0/LZCS/GPF0 sequence and returns the signed squared length. Signed squared-length overflow stops before LZCS or output writes. It preserves masked arithmetic output shifts, GPF IR/MAC/FLAG and RGB FIFO effects; unrelated GTE state and structure padding remain unchanged. The existing signature and null-output return-zero behavior are retained; a readable input VECTOR is required. `VectorNormal` and `VectorNormalSS` are unchanged. Native callers that intentionally reproduce an image-specific zero-vector IR0 may restore only IR0 after a zero return: all three squared input components are then zero, so the GPF MAC/IR/FLAG and RGB FIFO effects are independent of its coefficient. No new normalization API is introduced; no current build/runtime acceptance is claimed during structural migration.

## gpu_decode_tim_body

`gpu_decode_tim_body(src, size, image)` is a bounded native TIM codec in `psx_gpu.c`. The input starts at the mode word after the four-byte TIM magic; full-file callers validate magic `0x10` themselves. `GpuTimImage` owns rectangle values and borrows pixel/CLUT pointers from the input for its lifetime. Decode supports pixel modes 0..3 with optional CLUT, checks the CLUT block extent and both rectangle payload lengths against the supplied span, requires four-byte CLUT block alignment and leaves the output unchanged on failure. The final pixel block length word is ignored, as in the existing SDK-style readers; actual pixel extent is derived from rectangle dimensions. This also accepts files whose final length word excludes the block header. Pixel payloads may have two-byte padding; no pixel-format reinterpretation or allocation occurs. This codec is not an implementation of the stateful PsyQ `OpenTIM`/`ReadTIM` ABI.

## gpu_render_list

`gpu_render_list_create/free` owns an opaque CPU copy of native drawing commands for frozen OG reconstruction. After applying the initial native `DRAWENV` in canonical VRAM coordinates, call `gpu_render_list_begin(list, environment)`, execute the original `DrawPrim`/`DrawOTag` stream once and call `gpu_render_list_end(list)`. Collection does not enable rasterization. The GPU's existing command decoder copies complete commands; the game does not parse packets. Only one collection may be active; linked-submit providers and nonzero alternate raster surfaces are unsupported. Freeing an active list detaches collection. A later begin reuses its capacity.

Supported commands are polygons, lines, rectangles, NOP/cache-clear and E1..E6 drawing state. GP0 fills/image transfers, direct SDK uploads/moves/clears, port writes or another native environment during collection invalidate the list; end and replay return zero. The initial environment's clear remains supported. GPU graph reset invalidates previously completed lists. This API does not preserve textures, palettes, framebuffer seeds or simulation state. The caller owns those dependencies and must establish that its frozen scene is reconstructible from the current canonical VRAM. Readback operations are not replayed. HD rendering still consumes semantic game snapshots, not this OG command stream.

`gpu_render_list_replay(list)` synchronously executes a completed list through the same software rasterizer, including its initial environment, while preserving the caller's drawing state and raster-selection flag. It invalidates the VRAM version, does not present, dispatch DMA/SDK callbacks or run game code, and rejects an active collection or invalid list. The caller brackets reconstruction/image moves with the existing frame scopes and builds the complete original image before exposing it. Neither lists nor replay create another VRAM/device owner. Target acceptance and source/executable evidence belong to the consuming project.

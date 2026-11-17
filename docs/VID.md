# VID

For the reusable OG/HD migration, float math, shader and batching workflow used by `enhance` tasks, read [ENHANCE.md](ENHANCE.md).

For original PS1 texture decoding and selective palette-aware bilinear filtering, use the shader-source helpers and adapter contract in [VID_PSX_SAMPLING.md](VID_PSX_SAMPLING.md).

## Build

Windows VID calls `wglSwapIntervalEXT(1)` when the extension is available; its absence or a failed call does not reject initialization. `SwapBuffers` paces presentation when VSync is active; Windows render-wait helpers add no software FPS limiter. Physical PS1 field timing stays independent. Web continues using `requestAnimationFrame`.

Successful screen presentation owns pacing. Windows waits in `SwapBuffers` with the configured swap interval. Web `vid_end` closes the frame before calling `xport_present_complete`, which yields to `requestAnimationFrame` and records the serviced physical field. Legacy `vid_blit` submissions return after swapping so polling the PS1 devices can update the screen without an additional animation-frame wait. Legacy game loops retain their PS1 clock and VSync pacing. The fullscreen FMV loop calls `xport_present_complete` after its screen submission. Offscreen targets, cancelled/failed frames, zero-sized output and headless frames do not yield. Presentation does not advance game simulation. Hidden tabs follow browser throttling.

`xport_render_wait_until(deadline)` remains a compatibility deadline comparison on both platforms. It does not wait or generate another animation frame. Existing callers after present do not yield twice. New game code must not use an infinite deadline to pace rendering.

- Compile `src/vid_ogl.c` and define `GAPI_OGL` for Windows and Web
- Windows links `opengl32.lib`, `gdi32.lib` and `user32.lib`; base GL functions use DLL imports, extended GL/WGL functions use the supplied `src/libs/gl` function-pointer types and dynamic lookup
- Windows requires an OpenGL 3.3 core context; Web requires WebGL 2.0 and `MIN_WEBGL_VERSION=2`, `MAX_WEBGL_VERSION=2`
- GDI presentation and Canvas2D presentation are removed; no graphics fallback exists
- A headless host does not call `vid_init` and creates no graphics context
- Backend selection is compile-time; `xport_hd` selects only the game's frame producer

## Platform lifecycle

1. Create the native window or browser canvas
2. Call `vid_init(window)` on Windows or `vid_init(NULL)` on Web
3. Fail initialization when the context, required functions or shaders are unavailable
4. Call `vid_free` before destroying the native window

Games register application resource cleanup through `xport_set_cleanup(callback)`. The host clears the callback before invoking it once during `xport_shutdown`, before audio/VID/context teardown. Passing NULL removes the registration. Windows window-close unwinding returns to host shutdown without requiring the game stack to return normally. No game-specific callback is required from other projects

Windows Alt+Enter stores the current window style and placement, enters a monitor-sized `WS_POPUP` window through `SetWindowPos` with `SWP_FRAMECHANGED`, and restores the exact stored style and placement on exit. Removing `WS_OVERLAPPEDWINDOW` alone does not establish this fullscreen contract. Preserve the requested swap interval and game clock during this transition. Alt+Enter testing is not a mandatory integration or completion check; retain human confirmation when supplied. ETW/PresentMon tracing is diagnostic only; window geometry alone does not establish hardware flip or exclusive presentation

`vid.h` is the common public interface. Resource layouts, context ownership and GL objects are private to `vid_ogl.c`. Rendering uses vertex buffers and shaders; no fixed-function pipeline is permitted.

## OG output

`xport_present` retains its source-rectangle contract and calls `vid_blit`. Input pixels are 32-bit `0x00RRGGBB`, top row first. Output is opaque, nearest-filtered and centered in the largest 4:3 rectangle fitting the client area. The remaining area is black. The display's source dimensions do not define its aspect ratio.

`vid_resize(width, height)` synchronizes the drawable size and returns physical pixel dimensions. Web uses the canvas CSS size multiplied by device pixel ratio. A minimized zero-sized drawable suppresses output without destroying the context.

`vid_begin` records the drawable dimensions used by the frame's viewport coordinates. If the drawable changes before `vid_end`, VID scales the queued viewport edges to the new dimensions. Shared split edges use the same integer calculation, including odd drawable sizes. The next frame uses newly generated geometry and the current viewport aspect

`gpu_set_polygon_tpage(uint16 page)` preserves the native software GPU state effect of an embedded textured-polygon page without emitting geometry: replace `active_tpage` bits `0x09ff`, retain all other page bits and the independently stored draw flags. It does not write VRAM, rasterize, update the PsyQ DRAWENV cache or emit a GP0 transfer. The original software primitive dispatcher uses the same operation before raster suppression. Native HD producers can use it at the original ordering boundary after required full E1 state commands; it is not a replacement for those commands or a guest packet submission API. The `gpu_observer` HD raster-policy check covers page selection with independent draw flags and unchanged VRAM.

Native GP0(E6) uses the existing GPU mask owner for primitive writes: bit0 forces output bit15, bit1 protects destinations whose bit15 is already set. Transparency and textured blending test the source texel STP before the output force bit is applied; transparent zero texels remain unwritten. DrawPrim, DrawOTag and supported port primitives share this behavior. GP0(02) fill remains independent of these mask bits; masked port VRAM copies remain explicitly unsupported. GpuRasterState.draw_flags carries mask force/check at bits11/12 alongside bits9/10; import validates and restores them, export preserves them, and GPU initialization/reset clears them. The portable GPU scalar snapshot includes the mask word and rejects older scalar-block sizes; gameplay account saves are separate. AC3 regression evidence: AceCombat3-xport/status/framebuffer-feedback/native-mask-proposal.json and test-x86.json/test-x64.json. The existing gpu_observer suite tests primitive paths, source STP, mask protection, reset, import/export and snapshot persistence.

`gpu_restore_raster(state)` restores a validated `GpuRasterState` without reading or writing VRAM, changing raster enablement or presenting. It restores TPAGE, draw flags/mask, texture window, clipping and offset through the software GPU owner. Use paired `gpu_export_raster`/`gpu_restore_raster` around temporary native reconstruction; this does not restore PsyQ environment caches or record commands for an active render-list capture. `gpu_import_raster` retains its full-image import contract and uses the same state validation/application.

## Resources

- `vid_mesh_static` seals committed vertices against edits and invalidates CPU data views. VID retains staging until upload, then releases it; already uploaded meshes release it immediately. Queued GPU ownership remains unchanged
- `vid_mesh_create` owns a fixed-capacity CPU vertex array; `vid_mesh_data` exposes that array. With an initialized context it also creates the GL buffer handle immediately; pre-init/headless handles remain CPU resources until rendering
- `VID_VERTEX` contains homogeneous clip coordinates, normalized UV and floating-point RGBA; `vid_mesh_create` / `vid_mesh_data` select this 40-byte layout
- `vid_mesh_create_3d` / `vid_mesh_data_3d` select the 72-byte `VID_VERTEX3D` layout: `pos[4]`, `uv[2]`, `color[4]`, `n[4]`, `extra[4]`. Wrong-layout accessors return NULL; both layouts share commit/free ownership
- `vid_mesh_create_16` / `vid_mesh_data_16` select the 52-byte `VID_VERTEX16` layout: signed 16-bit position/UV/normal components, byte colors, four float effect parameters, four unsigned 16-bit texture attributes and two unsigned 16-bit rectangle attributes
- `vid_mesh_create_2d` / `vid_mesh_data_2d` select the 44-byte `VID_VERTEX2D` layout: float clip position/UV/alpha, byte colors and the same unsigned texture/rectangle attributes
- `vid_index_create(capacity)` owns a uint32 index array and its GPU element buffer. Fill `vid_index_data`, then call `vid_index_commit(indices, used_count)`. Indices are vertex offsets; indexed submission ranges are index offsets/counts. Values must address existing mesh vertices
- `vid_index_static` seals committed indices against further edits and invalidates returned data pointers. VID frees the CPU copy after GPU upload, or immediately if already uploaded. A headless/unrendered resource retains the sealed copy until upload/free. Mutable indices retain their CPU copy; `vid_index_free` releases ownership
- `vid_tex_create` owns a tightly packed RGBA8 CPU image; `vid_tex_create_fmt` also supports 16-bit RGBA5551, with R in bits 15..11 and A in bit 0, plus R8UI and RGBA16UI integer textures. Integer textures use nearest filtering and clamp addressing
- `vid_shader_create(vertex, fragment)` owns a linked vertex/fragment program. NULL vertex source selects the common VID clip stage; NULL fragment source fails. `vid_shader_free` releases its owner reference
- `vid_sample_create` owns a persistent color-probe result; `vid_sample_color` returns the last executed probe as `0x00RRGGBB`, initially zero; `vid_sample_free` releases its owner reference
- Fragment input omits a version header; VID supplies `vUV`, `vColor`, `uTex`, `uTextured`, `uParams[2]` and `outColor`. Supply a `main` body compatible with GLSL 330 and GLSL ES 300
- Material `depth_bias_slope` and `depth_bias_units` apply polygon depth offset to filled triangles; both default to zero. Negative values pull coplanar overlays toward the camera while retaining depth testing. VID applies the queued material snapshot and clears polygon offset after its queue flush
- Material `params[8]` are copied with each legacy draw or explicit pass; a committed material retains its shader
- `vid_mat_create` owns the material data; `vid_mat_commit` retains its selected texture and `aux[VID_AUX_TEXTURES]` textures. Custom shaders declare the auxiliary samplers they use
- `vid_tex_create_array_fmt(width, height, layers, format)` supports RGBA8 and RGBA5551 array storage; the original `vid_tex_create_array` remains RGBA8
- `vid_tex_static` seals a committed CPU-backed texture against further edits. Returned CPU data pointers become invalid; VID retains staging until its first upload, then frees it. An already uploaded texture discards CPU staging immediately. This does not itself draw or upload; queued resources retain their ordinary GPU ownership. Headless resources retain staging until upload or release
- After writing mesh or texture data, call its `commit` operation before drawing
- `vid_mesh_commit_range(geometry, first, count)` commits an edited vertex range in a live, mutable, unqueued mesh. Pending ranges accumulate their enclosing interval. The first upload and ordinary `vid_mesh_commit` upload the whole mesh; subsequent range commits update only the dirty interval without reallocating storage or changing its static usage hint. Zero count is a no-op; invalid ranges and queued/immutable/released edits abort. Vertices outside the committed range must remain unchanged
- `vid_tex_commit_rows(texture, first, count)` commits changed full-width rows of the CPU image. Pending row commits accumulate their enclosing interval; ordinary `vid_tex_commit` marks the whole image. Initial allocation uploads the complete image. A zero count is a no-op; invalid ranges abort. Queued draws still sample the final committed image at execution
- Resource data pointers remain stable until the owning resource is freed, except index pointers invalidated by `vid_index_static`
- Queued commands retain their mesh, indices, material, shader and textures until `vid_end` or `vid_cancel`
- Mesh/material data cannot be changed while queued. Texture data can be updated and committed before `vid_end`; all queued draws sample the final committed image. This supports uploading a shared dynamic texture after geometry collection
- `vid_mat_data` returns writable material data only while the material is live and unqueued. `vid_mat_read` returns a borrowed const view of live material data, including while queued, without adding a reference or permitting edits. Both return NULL for NULL/released handles. Read views must not outlive the material or be assumed unchanged after an allowed edit/commit
- `vid_mat_revision(material)` returns a nonzero monotonically assigned commit revision for a live committed material, or zero for NULL/released/uncommitted handles. Revisions distinguish replacement materials even when allocator addresses are reused; they do not retain the material or permit queued edits
- Freeing a caller's ownership after submission defers destruction until the frame releases the final reference
- Every created resource must be freed by its owner; `vid_free` releases remaining resources at platform shutdown

The game stores semantic resource selections and VID references. It never allocates GPU objects, performs GPU uploads or inspects GL handles.

## GPU image feedback

`vid_target_create_scene_ldr()` pairs the ordinary RGBA8 color/depth-renderbuffer target with the same R8 surface attachment. It retains LDR saturation for color operations; `vid_target_create_scene()` keeps its HDR color and sampleable depth. `vid_target_copy` copies color0 and, when both targets own a surface attachment, its R8 contents and GPU version as well. It does not copy depth.

`vid_target_select_mip(target, level)` selects a nonzero mip of a ready RGBA8/RGBA16F 2D color target for an offscreen frame. Set a finite mip count with `vid_tex_set_mip_levels` before capture. The base dimensions remain unchanged; viewport coordinates refer to the selected mip. It allocates the configured chain and validates each mip FBO on first use/resize, then reuses storage. Mip passes preserve the base texture version and take ownership of custom mip contents: later trilinear sampling does not generate over them. Only sample levels already supplied for the current base image; producing the complete chain is the caller's responsibility. Rendering the base/resize invalidates prior contents. Feedback, queued lifetime and cancellation rules still apply; use a distinct scratch texture between H/V passes. Cube, array, depth, base-level and out-of-range selections are rejected. This path has no depth or surface attachment and never presents

`vid_target_copy_mip(destination, level, source)` copies the lower-left mip-sized rectangle of a ready color target into a nonzero destination mip. Call outside a frame with distinct live ready2D targets, matching RGBA8/RGBA16F formats and no queued destination readers. It retains mip0 and its version, allocates missing lower levels once per size, and marks derived mips current only after every configured valid level has been supplied for that base version. Rendering/resizing invalidates that completion state by version/storage. Returns0 on invalid input or GPU failure. The producer owns filter shaders/scratch targets; no CPU image upload or automatic mip generation occurs for a completed custom chain.


- Create a `VID_TARGET` with `vid_target_create`; release it with `vid_target_free`
- After `vid_begin`, call `vid_target_select(target)` before recording any draw, pass or probe. Selection fails for NULL/released targets, an existing selection, a nonempty queue or a target image referenced by queued draws
- `vid_end` renders color and depth into the selected target at the actual drawable size, presents its color image, and retains the completed image after presentation. Targets are opt-in; ordinary `vid_begin` and OG `vid_blit` keep their existing destination
- `vid_target_select_offscreen(target)` uses the same selection validation and target lifetime. `vid_end` completes the target without copying its color to the previous framebuffer or swapping the window. Use this for native frozen-scene reconstruction at an image-capture boundary while OG remains selected; do not use it to render an extra scene every tick
- `vid_target_select_offscreen_size(target, width, height)` selects a fixed-size offscreen image (positive dimensions, at most16384 each). Viewport coordinates use that image size rather than the drawable; allocation occurs at execution. This supports explicitly requested dynamic reflection probes without presentation, swap or pacing. Target lifetime/feedback rejection and zero-drawable behavior match ordinary offscreen selection. Subsequent ordinary selection restores drawable sizing
- `vid_target_create_depth()` creates a depth-only D24 target for fixed-size offscreen rendering. Its borrowed texture uses linear `LEQUAL` comparison sampling (`sampler2DShadow`); alpha-test shaders may discard casters, and no color image or duplicate depth renderbuffer is allocated. Use offscreen selection only; color target copies reject depth targets
- `vid_target_create_depth_array(size, layers)` creates one D24 2D texture array (1..256 layers) without CPU pixels or a second depth buffer. Within an empty active frame, `vid_target_select_depth_layer(target, layer)` selects a fixed-size offscreen layer; execution attaches only that layer and clears its depth. Other layers persist. Sampling uses `sampler2DArrayShadow` with linear LEQUAL comparisons; filtering never crosses layers. Only successfully rendered layers may be sampled. Each layer completes without presentation or pacing, retaining ordinary target feedback and resource lifetime rules
- `vid_target_create_cube(size)` owns one RGBA8 `GL_TEXTURE_CUBE_MAP` with six square faces and one shared depth renderbuffer. `vid_target_select_cube_face(target, face)` selects face0..5 (+X,-X,+Y,-Y,+Z,-Z), fixed-size offscreen; each `vid_end` completes that face without swap/pacing. Sampling requires all six faces completed. Cube targets are rejected by the2D color-copy API
- `vid_shader_create_cube(vertex, fragment)` supplies `samplerCube uTex`; sampling uses a direction vector. Desktop VID enables `GL_TEXTURE_CUBE_MAP_SEAMLESS` once per context and clamps S/T/R; linear sampling crosses face boundaries. Cubic targets retain the normal borrowed-texture/material/queued-lifetime rules and have no CPU pixel mirror
- `vid_target_select_offset(target, y)` presents the retained target with a vertical sampling offset in pixels of the drawable captured by `vid_begin`. Positive values sample higher source rows in bottom-left coordinates; exposed bands are black. Execution scales the offset to a resized drawable and uses nearest sampling with half-pixel rounding. The target image stays unshifted for later copying. Nonfinite offsets are rejected. Cancellation resets the offset; zero matches ordinary target selection
- Allocation and resizing occur on execution. A minimized frame leaves the previous target image unchanged. `vid_cancel` releases the selection and preserves the previous completed image
- `vid_target_texture` returns a borrowed RGBA8 texture. Do not free or commit this texture; `vid_tex_data` returns NULL. Materials retain it through the ordinary texture ownership contract. UV origin is bottom-left, unlike top-row-first uploaded CPU images
- Sample a target only after a successful rendered frame or copy. Draw/pass recording rejects sampling the selected output as the primary or auxiliary texture
- Outside an active frame, `vid_target_copy(destination, source)` copies the last completed source image on the GPU. It preserves source dimensions, rejects self-copy, uncompleted/released sources and destinations referenced by queued draws, and returns zero on failure
- Copy only at a real image-capture boundary. Subsequent source renders do not modify the copy. Color fades alone use geometry and do not allocate targets
- A selected target retains its owner until end/cancellation. Freeing the caller reference while selected defers destruction. A material can retain a completed texture after its target is freed
- The backend owns framebuffer, depth, texture storage and presentation operations. Games do not read GL handles or copy full images through CPU memory

## HD frames

1. Call `vid_begin` with the clear color
2. Populate and commit resource data before submitting draws that reference it
3. Select a top-left-origin viewport with `vid_viewport`
4. Call `vid_pass(material)` once for a group sharing render state, then `vid_submit(mesh, indices_or_NULL, first, count)` for each triangle range. NULL indices select sequential vertices; non-NULL indices select uint32 indexed geometry. `vid_draw` remains the self-contained legacy alternative
5. Call `vid_end` once, or `vid_cancel` on an abandoned frame

An explicit pass snapshots viewport, fog, material and current draw uniforms. Each subsequent `vid_submit` snapshots its current draw uniforms; zero vectors leave the pass/object uniforms unchanged. Change viewport/fog by starting another pass. A legacy `vid_draw` ends the explicit-pass recording scope; call `vid_pass` again before another submit. Keep referenced mesh/index/material data immutable while commands are queued. `vid_sample` uses its own viewport and preserves command order; begin a new pass after a probe before resuming geometry

`vid_clear_depth()` queues a depth-only clear in the current viewport of the selected target. It preserves color and follows recorded command order, including viewport scaling if the drawable changes before execution. It requires an active frame, returns zero on failure and ends the current explicit pass; call `vid_pass` before the next `vid_submit`. The backend enables depth writes for the clear and the next pass restores its material state. Cancellation discards the command. The clear performs no presentation, readback, simulation or VRAM operation. Use an explicit game-owned layering boundary, such as a screen-space 3D group, rather than clearing between ordinary world objects

Consecutive explicit passes may be coalesced only while the material, viewport, fog and inherited uniform state are equivalent. A changed nonzero uniform payload remains an ordered state change even before geometry; a later zero-vector submission must inherit it. Legacy draws, samples, depth clears and target-part changes end the reuse scope. Queued immutable uniform identity can skip byte comparison; execution compares/uploads only the shader-active prefix, retaining the zero-vector inheritance contract

The renderer sets viewport/scissor, depth/blend/cull, program and textures once per explicit pass. Submissions bind their mesh VAO, upload changed resources and object uniforms, and issue the draw. Mesh attribute layout is configured once in its owned VAO. Retain one explicit pass across adjacent submissions with the same material state; changing object uniforms alone does not require another pass

Texture filter/wrap settings are retained with each VID-owned texture. The backend sets them on first sampling or an actual setting change; integer textures force nearest/clamp before comparison. Pixel uploads, target resize and target copy preserve settings on the same GL object. If a future path replaces a texture's GL object while retaining its owner, invalidate the saved settings before sampling. There is no global GL state cache or draw-statistics subsystem

Depth comparison mode/function are initialized with the owned texture object and retained across warm binds. Storage updates preserve these object parameters; replacing an object requires invalidating its comparison and sampler setup. Raw depth remains non-comparison data

Vertex-buffer uploads use `GL_STATIC_DRAW`; the committed version still determines uploads, and later commits remain valid. Do not change this hint to `GL_DYNAMIC_DRAW` without an actual WebGL indexed-identity regression test. On ANGLE/D3D11, dynamic vertex storage can add the minimum element index twice to `gl_VertexID`, while fetching the correct vertex attributes. A shader that indexes material/morph textures through `gl_VertexID` then reads another face. Verify nonzero minimum indices, including values above 65535, against shader-reported integer identities and rendered material pixels. Do not subtract an observed offset in game shaders. Reproducer and actual browser evidence: Pandemonium2-xport `status/enhance/checks/web-vertex-id-isolation` and `status/enhance/web-indexed-vertex-identity.json`.

The draw list preserves submission order. Materials select depth test/write, blending and nearest/linear filtering. `repeat=0` clamps UV to the image edges; `repeat=1` repeats the complete texture on both axes, including UV outside 0..1. The producer selects clip coordinates or source geometry plus a custom vertex stage. VID stores no camera motion history, interpolation or reset policy. `cull=0` disables face culling; `cull=1` retains clockwise triangles; `cull=2` retains counterclockwise triangles

`VID_BLEND_PREMULTIPLIED` uses `ONE, ONE_MINUS_SRC_ALPHA`. The shader supplies premultiplied RGB: `(rgb * opacity, opacity)` for alpha, `(rgb * intensity, 0)` for additive, and `(rgb, 1)` for opaque fragments. Compatible primitives can share a pass while preserving submission order. Depth/write, clipping, shader and resource boundaries still apply; subtractive LDR blending remains a separate mode. Existing blend enum values and behavior are unchanged.


## Custom vertex stage

- Supply shader bodies without a version header; VID prepends GLSL 330 core on Windows or GLSL ES 300 with highp float/int on Web
- Bind `aPos` at location 0, `aUV` at 1 and `aColor` at 2. The 3D layouts additionally bind normals at 3 and effect parameters at 4. Compact layouts bind integer texture attributes at 5 and rectangle attributes at 6 through `glVertexAttribIPointer`; use `uvec4` / `uvec2` inputs and matching flat fragment varyings. The game defines shader names and field meaning
- Emit `gl_Position`, `vUV`, `vColor`, `vDepth` and `vFog` to match the supplied fragment prefix. Initialize every output on clipped/special-case paths
- Declare `uniform vec4 uDraw[N]`, where `N <= VID_UNIFORM_VECTORS`. Call `vid_uniforms(values, N)` inside an active frame before the corresponding `vid_pass`, `vid_submit` or `vid_draw`. VID copies each call into immutable frame-owned storage; queued commands reference that snapshot until end/cancellation. Subsequent source-array changes or calls cannot affect an earlier draw; callers never lend stack storage to the queue. The supplied count must fit the declared array
- `vid_uniforms(NULL, 0)` clears subsequent command parameters. `vid_begin` also clears them; shader compilation and uniform upload remain private to VID
- Commit static meshes once after asset preparation. Changing matrices or lighting updates draw parameters without committing or uploading vertices again. Dynamic meshes retain capacity and commit generated source vertices each frame
- VID uploads a mesh only when its committed version changes. Multiple ranges and subsequent unchanged frames reuse its existing GPU buffer

RapidRacer's `hd.c` owns world, water and UI shaders. World draws use 19 vectors for model/camera matrices, projection/depth partition, light/color matrices, ground UV plane and special-effect parameters. The water stage adds two vectors for previous/current ticks, interpolation and course/split-screen parameters. CPU code interpolates object/camera poses, prepares per-object light attenuation, builds dynamic trails/effects and rejects whole objects/groups. GPU code transforms/projects vertices, evaluates flat/Gouraud lighting and radial fog, adjusts ground UVs and expands wireframe edges. Source positions/normals are signed 16-bit, colors are bytes and PSX UVs use half-texel units; unlit dynamic positions preserve subunit residuals in otherwise unused normal components. Texture page, CLUT and sampling flags travel as flat vertex attributes, never per-material uniforms. Physics and OG GTE/raster code do not consume this shader state

RapidRacer builds one immutable water mesh per level. Two static uint32 index buffers order complete segments forward/backward without duplicating a ring sequence; prefix offsets select one contiguous draw, or two at a circular seam. The dedicated water vertex stage reads the 512x16 R8UI `render_curve` table and a RGBA16UI texture containing previous/current impact ticks and live pickup flags, plus the original bonus-color table. It computes waves, deformations, color and UV scrolling for both ticks and interpolates them. Binary shore weights select `mix(base, wave, weight)` and retain anchored corners. Only changed impact/pickup history is committed; geometry stays unchanged. Route billboards share the same GPU animation and submit before water with depth writes

Native bank geometry and all unique static shore/environment groups share one immutable vertex buffer. Per-view/per-blend uint32 indices select the `track_vis` interval and conservatively visible groups; a group stamp prevents duplicate indices from shared section references. Index buffers upload only when their selected values/count change. Debug `editor_vis` uses these same ranges. Boats retain ordinary per-pose draws with parts grouped by render pass. Dynamic effects and horizon geometry combine compatible source data; UI combines adjacent primitives with the same blend and viewport while retaining per-vertex texture metadata. No instancing is used

Shaders, VRAM, curve texture and growing dynamic buffer capacities live until application shutdown. Race reset frees level meshes/indices and impact texture, retains common resources and resets interpolation history. VID owns all GPU objects and uploads

`vid_sample(sample, x, y)` queues a single color-pixel read at that point in submission order. Coordinates are normalized within the current viewport, top-left origin, each in `[0, 1)`. The queue retains the sample until end or cancellation. Reads occur from the current draw target before swap, after earlier commands and before later commands. Resize uses the same rescaled viewport as draws. Cancellation preserves the previous result; an unrendered/minimized frame does not update it. OGL uses synchronous `glReadPixels`; limit probes to required pixels

RapidRacer submits the sun before world geometry and the six lens ghosts after world geometry. It queues a center-pixel probe before HUD. The following frame uses the RGB555-white predicate and original 10/20-step fade; per-view probes persist across races, while fade/validity reset with the game renderer. Direction, projection and ghost positions use float math. Paused rendering does not update fade or probes

## OG/HD transition

Windows and Web initialize `extern int xport_hd` to 1. The host toggles it on a non-repeated Tab keydown or an L3 press from either player controller. Each controller tracks its own press edge; holding L3 does not repeat the toggle. Tab and L3 are consumed by the host rather than mapped into game PAD buttons. The game latches its frame path at a complete producer boundary.

Web `xport_input_init` can run at host startup and again from PsyQ PAD initialization. Remove the previous Emscripten keydown, keyup and blur registrations before registering their replacements. Registering a non-NULL callback appends another listener; duplicated Tab callbacks can toggle twice and leave the original mode selected. Preserve PAD state reset on every initialization. `tests.test_pad_ports` checks six initializations, one retained listener per event type, one Tab toggle, autorepeat rejection and PAD isolation; also verify the actual Emscripten callback registration in a browser.

Paused-scene ownership belongs to the game. RapidRacer renders a fresh HD scene and full-window pause overlay without dispatching simulation callbacks; it queries the current projection and cached target indicator and preserves game RNG/render sampling/trail state around render-only work. Switching to OG during pause constructs a complete OG scene and creates its software capture once. VID retains no pause, simulation or interpolation history

RapidRacer registers `hd_free` through `xport_set_cleanup` before calling `hd_init` during application startup. Host shutdown invokes it before destroying VID and its GL context, including Windows window-close unwinding. Race exit calls `hd_reset`: cancel pending work, clear counts and invalidate the VRAM upload version while retaining shader, VRAM image, materials and mesh capacity. Immediate UI/effect draws use a reusable mesh pool; reset its cursor once per frame, keep each queued mesh immutable and retain handles between frames and races. Grow capacity only when required

Render-only snapshots belong to game code. RapidRacer preserves RNG/render sampling/trail state while rebuilding a frozen transition scene; the game accumulates the original two-buffer circle fade through geometry and subtractive materials. Switching to OG during this transition builds a complete rasterized scene before presentation. Frozen OG submission uses the draw stage without a second video sync

`gpu_set_raster(0)` suppresses software primitive rasterization and OG presentation while preserving draw-state commands and image transfers. `gpu_set_raster(1)` restores rasterization after HD. The first returned OG frame must be constructed completely before presentation.

RapidRacer brackets each ordinary OG frame with `gpu_frame_begin` before its blocking VSync and `gpu_frame_end` after all viewports/OT passes, then calls `gpu_present`. VSync polling cannot present an unfinished display page or a partly drawn split frame. These scopes suppress presentation only; simulation, GPU commands and VRAM transfers retain their existing behavior. Scopes nest and reset on GPU reset/load; they are excluded from portable GPU state

`gpu_vram_version()` changes on image upload/CLUT writes, MoveImage, StoreImage, clear, DMA upload, snapshot import/load and OG-to-HD raster transition. Consumers compare one remembered version for the complete VRAM image. RapidRacer uploads one 1024x512 RGBA5551 image before rendering the collected HD frame; its game-owned fragment shader handles native tpage/CLUT addressing. No per-material texture decode or hash is performed

Original PSX texture colors expand five-bit channels with bit replication: `(c << 3) | (c >> 2)`. Normalize those eight-bit values by 255 for HD shading. Keep the original STP bit separate from color expansion

RapidRacer submits environment/effects and route billboards, opaque water, opaque actors and blended actors. All water tests depth without writing it. Its per-view draw ranges retain route-strip order independently of material batching: `camera->dir == 1` reverses the collected strip groups; other directions retain them. Both paths draw far strips before near strips and preserve polygon order within each strip. This is a directional route traversal without geometric sorting. Water mesh/indices live until race reset; common dynamic capacity lives until `hd_free`. No polygon offset is applied. User acceptance must cover both directions, distant racers, wakes and Hawaii/Bear Lake scenes; synthetic material/order tests do not prove those gameplay cases

RapidRacer's `hd.c` selects the mode-8 showcase path at the scene boundary. Its radial surface reads the horizon owner's radius table; event models, selected marker colors and border points remain owned by their original game modules. GPU geometry uses compact source vertices, the shared VRAM image and retained VID meshes. Event shading evaluates the original light/color/depth-cue sequence in the GPU vertex shader using per-object light parameters. Runtime acceptance requires an available original challenge, visible radial/event geometry, resize and complete OG/HD roundtrips; a successful build alone does not establish that acceptance

## Verification

- `tools/tests/native/vid_ogl_test.c`: actual Windows GL shaders/FBO pixels, OG color/orientation/crop/4:3, float HD geometry, deferred resource release and resize between frame submission/presentation with odd split dimensions; an optional argument supplies a game fragment source file for native compilation/link verification
- `python -B -m unittest tests.test_pad_ports tests.test_web_timing -v` from `tools`: Tab/PAD isolation, repeat handling and Web timing
- `X gpu_observer_test --configuration Debug`: actual software raster suppression, retained draw state/image transfers, complete-frame presentation recovery and restored OG output, plus existing GPU fixtures. Debug is the default; Release requires an explicit configuration argument. Debug outputs use `_build/gpu-observer/Debug`
- Consuming projects own actual browser/game scenarios and their evidence under `status`

## RapidRacer batching verification

- Evidence: consuming project `status/audits/hd-batching/report.json`
- `extract_shaders.py` extracts current shader strings; `vid_test.c` exercises actual native VID rendering, packed lighting, mixed CLUT indexed batches above vertex 65535, immutable EBO ownership, UI batching, GPU water interpolation/anchoring/UV, plus OG and resource/resize fixtures
- `extract_topology.py` extracts current river/shore builders and range selection; `topology_test.c` covers quad/triangle/hole topology, binary anchoring, both circular directions and duplicate shore group selection. Its frustum test is stubbed visible; it does not prove game culling
- `webgl.html` compiles/links current shaders and renders integer-texture water interpolation/anchoring pixels in WebGL 2.0
- Debug Windows/Web builds verify integration. These fixtures do not establish gameplay appearance, FPS or actual RenderDoc call totals; the user verifies the game


## Mipmapped anisotropic material sampling

VID_MATERIAL_DATA.filter accepts VID_FILTER_NEAREST (0), VID_FILTER_LINEAR (1), opt-in VID_FILTER_ANISOTROPIC (2), and VID_FILTER_TRILINEAR (3). Mode3 generates the requested mip chain after changed CPU or GPU contents, uses GL_LINEAR_MIPMAP_LINEAR minification and linear magnification, and keeps anisotropy1. Mode1 remains bilinear without mip sampling. Mode2 builds the requested mip chain after changed CPU/GPU image contents, uses linear mip interpolation for minification and linear magnification, and requests min(8, device maximum) anisotropy through EXT/ARB texture_filter_anisotropic. Without that extension it retains trilinear mip sampling. Integer textures retain nearest sampling. Existing0/1materials retain their previous behavior

Anisotropic sampling requires an independently decoded bounded image: filtering packed VRAM/palette indices or generating mipmaps for a shared atlas does not preserve palette or region semantics. The game owns decoded asset caches, source invalidation, STP/alpha class images and texture boundaries. VID owns mip generation, sampler capabilities/settings and queued GPU resource lifetime


## Queue-local GL state submission

An uninterrupted frame-queue flush compares the applied viewport/scissor, cull/depth/blend state, program, uniforms, texture-unit bindings, VAO and element buffer before emitting GL calls. All applied-state knowledge is discarded at the flush boundary; there is no global GL state cache shared with other owners. A depth-clear command updates the known depth-write mask. Program changes invalidate the local uniform values; VAO changes invalidate the local element-buffer binding. Texture checks include uploaded versions and the texture object's actual sampler settings, including integer-format overrides and cross-unit sampler changes. The active texture unit is restored before returning

The queue still executes the same draws, index ranges, transparent ordering, probes, clears and target operations. This optimization removes redundant API calls; it does not promise a particular frame rate or change game geometry/materials


## Texture arrays

`vid_tex_create_array(width, height, layers)` creates an RGBA8 2D array with 1..256 equally sized layers. `vid_tex_data` exposes contiguous layer-major pixels, each layer containing width*height*4 bytes. Commit, material ownership and queued lifetime follow the ordinary texture contract. Each layer has its own mip chain; filtering never interpolates between layers. Row commits on arrays upload the complete array. The existing texture factories retain their 2D target and formats.

`vid_shader_create_array(vertex, fragment)` uses the ordinary custom-shader interface with `uTex` declared as a high-precision sampler2DArray; sample it with vec3(normalizedUV, layer). Only the primary texture is an array; auxiliary textures retain their existing sampler declarations. A material must pair an array shader with an array texture. Nearest, linear, trilinear and anisotropic x8 modes use the same settings as 2D textures. Atlas layouts, gutters, palette decoding and alpha classes remain game-owned.


`vid_tex_set_mip_levels(texture, levels)` sets an individual texture's maximum mip count, including level0. Zero (the default) retains the complete natural chain;1 keeps only the base image. Call it before submission, with no queued draw references. A changed limit invalidates mip/sampler state. Before generation the backend applies GL_TEXTURE_MAX_LEVEL=levels-1 to both 2D and array textures, so it limits generation as well as sampling; small images naturally stop earlier. Existing consumers retain full chains unless they opt in.

Draw uniforms support up to64vec4 values. Execution clamps uploads to the shader's declared active `uDraw` array length, so existing shorter UI/game shaders retain their original interface. Depth comparison textures are not integer color textures; auxiliary sampling retains linear comparison filtering.


## Persistent offscreen parts and allocation validation

`vid_target_next_part(part)` queues a face/layer change inside an active offscreen cube/array frame. It clears the selected part (depth and cube color), ends the explicit pass scope and preserves the flush-local applied-state cache. Submit subsequent geometry with its intended pass; the target and queued resources must remain alive until `vid_end`. This command never swaps or paces presentation. Single-part targets do not support this operation.

VID creates and validates a persistent framebuffer for each target part when storage is allocated or replaced. Unchanged parts reuse those attachments. Web Release checks allocation failures and context loss without synchronous GL error/completeness queries on warmed draws. Native and diagnostic builds retain their error checks. Buffer capacity growth and initial image/FBO allocation retain explicit validation; buffer updates reuse allocated storage.

`VID_RGBA32F` is a nearest/clamped floating-point data image for shader instance tables (16 bytes per texel), not a replacement for decoded color images. Its CPU storage and queued lifetime follow ordinary VID textures. Games retain ownership of instance contents and immutable geometry.


## Auxiliary texture bindings

A material supports `VID_AUX_TEXTURES` (eight) auxiliary images through `aux[0..7]`. Custom shaders declare the corresponding `uAux0` through `uAux7` samplers with their actual image types. VID assigns units1..8 once after program linking; `uTex` retains unit0. Existing0/1bindings and zero-initialized materials keep their behavior. Bindings participate in commit validation, queued reference retention, feedback rejection and flush-local state tracking. A shader may use fewer bindings; no placeholder resource is required for unused samplers.

Rendered color targets (ordinary/HDR 2D and cubemaps) invalidate their mip chain after a successful target pass; successful vid_target_copy also invalidates the destination chain. The texture version advances with GPU contents already uploaded; this never schedules a CPU pixel upload. A later trilinear- or anisotropic-filtered use rebuilds mips once under the existing per-texture level limit. Consumers that retain linear/nearest filtering keep their existing sampling behavior. Capture all required faces before sampling when a coherent six-face mip chain is needed.


## HDR color targets and auxiliary filters

`vid_target_create_hdr()` creates an ordinary 2D color target with RGBA16F GPU storage and the existing depth-renderbuffer/lifetime contract. The borrowed color image has no CPU pixel array. Explicit offscreen dimensions and persistent resize/storage reuse work as for ordinary targets. Existing factories keep their prior formats. `VID_RGBA16F` may also be used with `vid_tex_create_fmt`: its CPU data is interleaved IEEE binary16 RGBA,8bytes per texel, uploaded with GL_HALF_FLOAT. Array factories retain their existing allowed formats. Color-buffer float support is required by the backend; platform support is not established by allocation alone.

`VID_MATERIAL_DATA.aux_filter[index]` selects auxiliary image filtering under the same enum as the primary filter. Zero preserves the earlier nearest behavior. Integer/data images retain forced nearest sampling; comparison depth retains linear comparison filtering. Auxiliary repeat remains clamped. Filter selection participates in the existing flush-local texture/sampler cache; material commit/queued lifetime and target-feedback rejection remain unchanged.

`vid_target_create_hdr_depth()` replaces the ordinary HDR target's depth renderbuffer with a sampleable D24 image. `vid_target_depth_texture()` returns that borrowed image, or NULL when unavailable. It uses `VID_DEPTH24_RAW`: nearest/clamped sampling through sampler2D with comparison disabled. The target owns its storage and release; there is no CPU pixel array or duplicate depth attachment. Existing comparison-depth factories are unchanged.

`vid_target_create_cube_depth(size)` is an opt-in RGBA8 color cube with a GPU-owned `VID_DEPTH24_RAW` cube at the same size. `vid_target_depth_texture()` returns its borrowed raw depth cube for `samplerCube`, nearest/clamp, mip0 and comparison disabled. Both images receive the corresponding face attachment in the existing capture pass; no extra geometry pass, CPU pixel array or depth renderbuffer is created. Complete all six poses before sampling either image; preserve existing queued lifetime/feedback rules. The caller must supply the exact capture near/far convention when reconstructing depth. Ordinary color-only cube factories retain their previous behavior. Depth belongs to opaque depth-writing geometry; a blended color composite does not have a unique matching depth.

`vid_target_create_scene()` additionally owns a normalized R8 attachment at fragment output location1. `vid_target_surface_texture()` returns its borrowed image. The attachment clears to zero each frame; a material opts into writing it with `surface_write != 0` and must declare its own output at location1. Other materials write only color0. Blending uses the material's existing blend equation for both attachments; shaders may preserve an underlying mask with source alpha0 where appropriate. VID does not assign semantic meaning to the mask. Color0 has an explicit output location in the supplied shader prefix. Ordinary targets/materials keep their previous behavior.

All three scene attachments resize together, participate in queued resource/feedback checks and are released by the target. Resolve only after ending the producing target frame. `VID_R8` is also an opt-in normalized one-byte CPU image format; existing integer R8UI remains distinct. These additions require actual platform framebuffer/shader verification; native support is not proof of Web support.

Material auxiliary texture bindings support uAux0..uAux7; existing slot indices retain their meaning. The primary texture plus eight auxiliary slots use nine texture units, within the GL3.3/WebGL2 fragment minimum of sixteen. VID_UNIFORM_VECTORS is72; queued uniform snapshots retain the supplied count. These capacity extensions allow a forward material to read shadow and light-record captures while retaining original material and palette textures. Existing shaders may continue to declare shorter arrays.

Active optional auxiliary sampler2D and sampler2DArrayShadow inputs receive shared typed1x1neutral textures when material slots are null. Shader reflection selects the type; depth comparison returns unoccluded and ordinary2D reads black. The queue-local binding cache distinguishes null bindings by sampler type, and context shutdown deletes the neutral objects. This maintains valid WebGL bindings even when a shader feature is disabled by a runtime uniform.

Lazy first-use upload preserves depth formats for both2D and array textures: VID_DEPTH24/VID_DEPTH24_RAW use DEPTH_COMPONENT24 storage with DEPTH_COMPONENT/UNSIGNED_INT input. A render-target texture may be bound before its first target render; it must never be allocated through the color-array fallback. TD5 selected Web evidence: TestDrive5-xport/status/enhancement/web-remaster-regression.md.


## Reusable cubemap capture and reflection

`vid_cube_capture(target, clear, render, context)` records all six faces of an existing cube target and completes one offscreen frame. Return 1 only if begin, every selection/callback and vid_end succeed. Null/released/non-cube targets, null RGB clear/callback and an already active frame return 0 without taking over a caller frame. A begin failure records nothing; selection/callback failure cancels its own frame. No swap, pacing or game update occurs. No GPU image/target is allocated by this helper. Mip policy remains on the target texture; unchanged images reuse the existing chain.

`VID_CUBE_RENDER` is `sint32 (*)(void *context, const VID_CUBE_FACE *face)`. The callback receives a borrowed descriptor valid for that invocation: index 0..5 (+X,-X,+Y,-Y,+Z,-Z), square pixel size, and row-major world-to-camera rotation with positive Z forward and positive Y down. The helper selects/clears each part and sets its full viewport. The caller supplies the origin, a matching 90-degree projection, geometry/material/fog/uniform commands and returns nonzero on success. It must not begin/end/cancel a frame, switch output targets, free/mutate the target, recursively call capture or sample the selected image. Earlier callbacks may have changed project state when a later callback fails; the caller restores its camera/view/caches on every exit and uses the return value as readiness. Do not use a failed capture as a newly completed probe. Ordinary low-level target APIs remain available.

Include `vid_reflection.h` and prepend `VID_CUBE_REFLECTION_GLSL` to a cube fragment shader created through `vid_shader_create_cube`. It defines:

- `vidCubeRay(position, normal, row0, row1, row2)`: normalized reflection from camera-space surface position/normal, transformed by the supplied view-to-world rows. Inputs must have nonzero position/normal; rays use the capture's world axes
- `vidCubeFresnel(nDotV, base)`: Schlick-like base+(1-base)*(1-clamp(abs(nDotV),0,1))^5, with caller-selected base reflectance in [0,1]
- `vidCubeSurface(image, ray, roughness, maxLod, coat, nDotV, coatF0)`: sample samplerCube using maxLod*clamp(roughness,0,1)^2; optionally mix sharp mip0 using clamped coat times the Fresnel term. Caller supplies a valid completed cube and nonnegative available maxLod, chooses output gain/alpha/blend and material values; there are no fixed uniform slots or game profiles

`VID_CUBE_PARALLAX_GLSL` optionally adds `vidCubeDepthPoint(image, ray, clip, point)` and `vidCubeParallaxRay(image, ray, offset, clip, texel, limits)`. Rays are normalized world directions; offset is receiver-minus-probe origin in matching units; clip contains positive near/far for the capture's standard non-reversed perspective projection; texel is an angular neighboring-ray step. It reconstructs face view-Z then radial distance using the ray's major axis, estimates a local plane from three raw mip0 samples and intersects the receiver's outgoing ray with that plane. The corrected ray can feed every color LOD while depth stays mip0. limits contains maximum offset/radial-distance ratio, neighbor world-span/radial-distance ratio, relative proposed-hit/depth residual and minimum grazing cosine. No game dimensions or material profiles are embedded. Sky, seam neighborhoods, discontinuities, degenerate/grazing planes, nonpositive intersections, large offsets and inconsistent re-sampled hits retain the original ray. This is a bounded local approximation: it cannot recover disoccluded geometry or assign a unique depth to a blended composite. The caller owns confidence thresholds and material eligibility.

The helper does not supply a game shader, visibility system, owner filter, capture scheduler or material registry. These remain caller policy. It uses the same true cube targets, six-face completion, seamless desktop cube sampling and backend mip behavior as the lower-level API. CPU orchestration check (no GL/game/presentation) from tools: `python -B -m unittest tests.test_vid_cube_capture -v`. It covers rotations, six callbacks, rejection, cancellation and begin/end failures; it does not prove GPU pixels or shader integration.



## Shared direct-light BRDF

Include `vid_brdf.h` and concatenate `VID_BRDF_GLSL` once into a fragment shader. It declares no uniforms, textures, varyings or game material IDs. Evaluate it after normalizing the interpolated surface normal; N, V (surface to eye) and L (surface to light) must be normalized in the same coordinate space.

- `vidBrdfFresnel(float f0, float cosine)` is scalar Schlick Fresnel
- `vidBrdfGgx(float roughness, float f0, float nl, float nv, float nh, float vh)` evaluates the GGX distribution with Schlick-GGX geometry and Fresnel, including incident NdotL. Supply clamped nonnegative dot products and nv >= 0.0001; roughness/F0 are in [0,1]. The distribution floors alpha=roughness squared at 0.04
- `vidBrdfSpecular(vec3 material, vec2 coatProfile, vec3 n, vec3 v, vec3 l)` combines a base and clearcoat lobe. Material contains perceptual roughness, scalar base F0 and coat weight; coatProfile contains coat perceptual roughness/F0. All material values are in [0,1]. It guards degenerate half vectors and returns zero if both base F0 and coat weight are disabled. The result already includes incident NdotL: multiply by light radiance/color and visibility, without a second NdotL factor
- `vidBrdfTransmission(float coat, float nv, float coatF0, float attenuation)` returns 1-attenuation*coat*Schlick(coatF0,nv), an optional caller-controlled base-color attenuation, not volume transmission

For example, a material `(0.30,0.04,1.0)` with coat `(0.12,0.04)` gives a rougher dielectric base plus narrow clearcoat. These are example policy values, not defaults hardcoded into the helper. Material masks, authored normal extraction, transforms/inverse transpose, lighting units, diffuse response, tint, shadow visibility, HDR/exposure and color-space conversions remain caller-owned. Tint only metallic specular by albedo when that matches the chosen material model. The scalar layered approximation does not add multiple scattering or a full energy-conserving diffuse model. Cubemap helpers in `vid_reflection.h` are independent and do not automatically match direct-light energy.

## Shared shadow sampling

`vid_shadow.h` supplies `VID_SHADOW_SAMPLING_GLSL`, a fragment-source helper independent of geometry, light ownership, uniform slots and world-unit scale. Prepend it to a GLSL330/GLSL ES300 fragment through VID. It uses sampler2DArrayShadow with linear LEQUAL comparison, as provided by `vid_target_create_depth_array`; it does not require compute, image load/store, sampler objects or a GL extension. WebGL2 runtime compatibility still requires the consuming shader/device check.

- `vidShadowTent3(uv, texel, p0, p1, w0, w1)` returns two sample positions and normalized weights per axis for a separable three-tap tent. Combining axes uses four bilinear comparison samples. texel is positive reciprocal image width/height, uv is normalized receiver position; neither includes an atlas/layer transform
- `vidShadowGradient(dx, dy)` accepts dFdx/dFdy of normalized receiver shadow coordinates (uv and depth) and returns depth derivative per normalized UV, or zero for a degenerate UV Jacobian. Evaluate derivatives before divergent material/light/cascade branches; the helper takes derivatives as arguments and does not make a divergent derivative defined
- `vidShadowTent3Array(image, coord, layer, texel, gradient)` returns weighted visibility. coord.xy is normalized UV, coord.z is comparison depth after caller bias, layer is a valid completed integer layer expressed as float. gradient corrects receiver depth at each tap; pass zero for ordinary PCF. The function does not perform frustum rejection, bias selection or edge fade. Reject invalid/outside UV/depth/layers and apply a caller guard band before sampling; texture clamping is not a light-frustum policy

Use existing VID depth array targets and vid_target_select_depth_layer/vid_target_next_part/vid_end for offscreen shadow capture. They retain texture ownership/queued lifetime and avoid screen presentation. Keep caster geometry, alpha-cutout (source alpha/STP semantics), skinning/wheel transforms, billboard orientation, raster cull/depth bias, light projection and per-view visibility in the project. Receivers use the same interpolated pose and coordinate convention as the depth pass. Supply normalized projection depth in [0,1] with compare reference biased toward the light for ordinary LEQUAL; there is no universal world-Y convention or bias magnitude.

Spot shadows may require per-tap ray/receiver-plane intersection rather than linear depth-gradient correction. The shared tent positions/weights are usable with that custom sampling function; do not silently replace such a sampler with the ordinary array helper. Shared helpers contain no lamp angle/radius, cascade split, texel grid origin, ambient model, light energy or object-identity rules.

Initial extraction is source-reviewed against an existing consumer's math, not proof of a new GPU integration. Consumers must check their actual shader link, comparison format/filter, layer/edge behavior, alpha-cutout and split-view/capture isolation under their authorized target matrix. No screen capture is required to use this API.

### Submission statistics

vid_stats_read copies cumulative VID_STATS without querying the GPU or resetting counters. Take differences around completed submissions. draws/triangles count actual geometry commands in vid_render; program_changes counts cached draw-program changes, texture_binds counts material upload/bind and empty2D fallback bindings, state_changes counts changed cached cull/depth/write/blend/offset categories. uniform_uploads/bytes cover uDraw glUniform4fv uploads; vertex_bytes/index_bytes cover vid_buffer_upload payloads. These counters are not total driver GL calls, GPU timing or presentation FPS. Setup, target allocation and final blits are outside these scoped counters.

vid_shader_draw_count returns the cumulative actual geometry draw count for a live shader (zero forNULL). Counts belong to shader lifetime and require no GL query. Compare snapshots to attribute the scoped submission statistics to individual production shaders.

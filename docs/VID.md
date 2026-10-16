# VID

For the reusable OG/HD migration, float math, shader and batching workflow used by `enhance` tasks, read [ENHANCE.md](ENHANCE.md).

## Build

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

`vid.h` is the common public interface. Resource layouts, context ownership and GL objects are private to `vid_ogl.c`. Rendering uses vertex buffers and shaders; no fixed-function pipeline is permitted.

## OG output

`xport_present` retains its source-rectangle contract and calls `vid_blit`. Input pixels are 32-bit `0x00RRGGBB`, top row first. Output is opaque, nearest-filtered and centered in the largest 4:3 rectangle fitting the client area. The remaining area is black. The display's source dimensions do not define its aspect ratio.

`vid_resize(width, height)` synchronizes the drawable size and returns physical pixel dimensions. Web uses the canvas CSS size multiplied by device pixel ratio. A minimized zero-sized drawable suppresses output without destroying the context.

`vid_begin` records the drawable dimensions used by the frame's viewport coordinates. If the drawable changes before `vid_end`, VID scales the queued viewport edges to the new dimensions. Shared split edges use the same integer calculation, including odd drawable sizes. The next frame uses newly generated geometry and the current viewport aspect

## Resources

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
- Material `params[8]` are copied with each legacy draw or explicit pass; a committed material retains its shader
- `vid_mat_create` owns the material data; `vid_mat_commit` retains its selected texture and `aux[2]` textures. Custom shaders access auxiliary texture units through `uAux0` and `uAux1`
- After writing mesh or texture data, call its `commit` operation before drawing
- Resource data pointers remain stable until the owning resource is freed, except index pointers invalidated by `vid_index_static`
- Queued commands retain their mesh, indices, material, shader and textures until `vid_end` or `vid_cancel`
- Mesh/material data cannot be changed while queued. Texture data can be updated and committed before `vid_end`; all queued draws sample the final committed image. This supports uploading a shared dynamic texture after geometry collection
- Freeing a caller's ownership after submission defers destruction until the frame releases the final reference
- Every created resource must be freed by its owner; `vid_free` releases remaining resources at platform shutdown

The game stores semantic resource selections and VID references. It never allocates GPU objects, performs GPU uploads or inspects GL handles.

## HD frames

1. Call `vid_begin` with the clear color
2. Populate and commit resource data before submitting draws that reference it
3. Select a top-left-origin viewport with `vid_viewport`
4. Call `vid_pass(material)` once for a group sharing render state, then `vid_submit(mesh, indices_or_NULL, first, count)` for each triangle range. NULL indices select sequential vertices; non-NULL indices select uint32 indexed geometry. `vid_draw` remains the self-contained legacy alternative
5. Call `vid_end` once, or `vid_cancel` on an abandoned frame

An explicit pass snapshots viewport, fog, material and current draw uniforms. Each subsequent `vid_submit` snapshots its current draw uniforms; zero vectors leave the pass/object uniforms unchanged. Change viewport/fog by starting another pass. A legacy `vid_draw` ends the explicit-pass recording scope; call `vid_pass` again before another submit. Keep referenced mesh/index/material data immutable while commands are queued. `vid_sample` uses its own viewport and preserves command order; begin a new pass after a probe before resuming geometry

The renderer sets viewport/scissor, depth/blend/cull, program and textures once per explicit pass. Submissions bind their mesh VAO, upload changed resources and object uniforms, and issue the draw. Mesh attribute layout is configured once in its owned VAO. There is no GL state cache or draw-statistics subsystem

The draw list preserves submission order. Materials select depth test/write, blending and nearest/linear filtering. `repeat=0` clamps UV to the image edges; `repeat=1` repeats the complete texture on both axes, including UV outside 0..1. The producer selects clip coordinates or source geometry plus a custom vertex stage. VID stores no camera motion history, interpolation or reset policy. `cull=0` disables face culling; `cull=1` retains clockwise triangles; `cull=2` retains counterclockwise triangles

## Custom vertex stage

- Supply shader bodies without a version header; VID prepends GLSL 330 core on Windows or GLSL ES 300 with highp float/int on Web
- Bind `aPos` at location 0, `aUV` at 1 and `aColor` at 2. The 3D layouts additionally bind normals at 3 and effect parameters at 4. Compact layouts bind integer texture attributes at 5 and rectangle attributes at 6 through `glVertexAttribIPointer`; use `uvec4` / `uvec2` inputs and matching flat fragment varyings. The game defines shader names and field meaning
- Emit `gl_Position`, `vUV`, `vColor`, `vDepth` and `vFog` to match the supplied fragment prefix. Initialize every output on clipped/special-case paths
- Declare `uniform vec4 uDraw[N]`, where `N <= VID_UNIFORM_VECTORS`. Call `vid_uniforms(values, N)` inside an active frame before the corresponding `vid_pass`, `vid_submit` or `vid_draw`. VID copies the values into each draw command; subsequent source-array changes or calls cannot affect an earlier draw. The supplied count must fit the declared array
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

Paused-scene ownership belongs to the game. RapidRacer renders a fresh HD scene and full-window pause overlay without dispatching simulation callbacks; it queries the current projection and cached target indicator and preserves game RNG/render sampling/trail state around render-only work. Switching to OG during pause constructs a complete OG scene and creates its software capture once. VID retains no pause, simulation or interpolation history

RapidRacer registers `hd_free` through `xport_set_cleanup` before calling `hd_init` during application startup. Host shutdown invokes it before destroying VID and its GL context, including Windows window-close unwinding. Race exit calls `hd_reset`: cancel pending work, clear counts and invalidate the VRAM upload version while retaining shader, VRAM image, materials and mesh capacity. Immediate UI/effect draws use a reusable mesh pool; reset its cursor once per frame, keep each queued mesh immutable and retain handles between frames and races. Grow capacity only when required

Render-only snapshots belong to game code. RapidRacer preserves RNG/render sampling/trail state while rebuilding a frozen transition scene; the game accumulates the original two-buffer circle fade through geometry and subtractive materials. Switching to OG during this transition builds a complete rasterized scene before presentation. Frozen OG submission uses the draw stage without a second video sync that could latch frame skipping again

`gpu_set_raster(0)` suppresses software primitive rasterization and OG presentation while preserving draw-state commands and image transfers. `gpu_set_raster(1)` restores rasterization and clears the latched frame-skip backlog after HD. The first returned OG frame must be constructed completely before presentation. `LOCKSTEP_DEBUG` retains rasterization.

RapidRacer brackets each ordinary OG frame with `gpu_frame_begin` before its blocking VSync and `gpu_frame_end` after all viewports/OT passes, then calls `gpu_present`. VSync polling cannot present an untouched skipped display page during recovery or a partly drawn split frame. These scopes suppress presentation only; frame-skip selection, simulation, GPU commands and VRAM transfers retain their existing behavior. Scopes nest and reset on GPU reset/load; they are excluded from portable GPU state

`gpu_vram_version()` changes on image upload/CLUT writes, MoveImage, StoreImage, clear, DMA upload, snapshot import/load and OG-to-HD raster transition. Consumers compare one remembered version for the complete VRAM image. RapidRacer uploads one 1024x512 RGBA5551 image before rendering the collected HD frame; its game-owned fragment shader handles native tpage/CLUT addressing. No per-material texture decode or hash is performed

Original PSX texture colors expand five-bit channels with bit replication: `(c << 3) | (c >> 2)`. Normalize those eight-bit values by 255 for HD shading. Keep the original STP bit separate from color expansion

RapidRacer submits environment/effects and route billboards, opaque water, opaque actors and blended actors. Route checkpoint/bonus billboards test and write depth in the world pass before all water; they are not interleaved with water strips. All water tests depth without writing it. Its per-view draw ranges retain route-strip order independently of material batching: `camera->dir == 1` reverses the collected strip groups; other directions retain them. Both paths draw far strips before near strips and preserve polygon order within each strip. This is a directional route traversal without geometric sorting. Water mesh/indices live until race reset; common dynamic capacity lives until `hd_free`. No polygon offset is applied. User acceptance must cover both directions, distant racers, wakes and Hawaii/Bear Lake scenes; synthetic material/order tests do not prove those gameplay cases

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

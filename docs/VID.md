# VID

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
- Vertices contain homogeneous clip coordinates, normalized UV and floating-point RGBA
- `vid_tex_create` owns a tightly packed RGBA8 CPU image; `vid_tex_create_fmt` also supports 16-bit RGBA5551, with R in bits 15..11 and A in bit 0
- `vid_shader_create` owns a material fragment program and uses the common VID vertex stage; `vid_shader_free` releases its owner reference
- `vid_sample_create` owns a persistent color-probe result; `vid_sample_color` returns the last executed probe as `0x00RRGGBB`, initially zero; `vid_sample_free` releases its owner reference
- Fragment input omits a version header; VID supplies `vUV`, `vColor`, `uTex`, `uTextured`, `uParams[2]` and `outColor`. Supply a `main` body compatible with GLSL 330 and GLSL ES 300
- Material `params[8]` are copied with each submitted draw; a committed material retains its shader
- `vid_mat_create` owns the material data; `vid_mat_commit` retains its selected texture
- After writing mesh or texture data, call its `commit` operation before drawing
- Resource data pointers remain stable until the owning resource is freed
- A queued draw retains its mesh, material and texture until `vid_end` or `vid_cancel`
- Mesh/material data cannot be changed while queued. Texture data can be updated and committed before `vid_end`; all queued draws sample the final committed image. This supports uploading a shared dynamic texture after geometry collection
- Freeing a caller's ownership after submission defers destruction until the frame releases the final reference
- Every created resource must be freed by its owner; `vid_free` releases remaining resources at platform shutdown

The game stores semantic resource selections and VID references. It never allocates GPU objects, performs GPU uploads or inspects GL handles.

## HD frames

1. Call `vid_begin` with the clear color
2. Populate and commit resource data before submitting draws that reference it
3. Select a top-left-origin viewport with `vid_viewport`
4. Submit triangle ranges with `vid_draw`
5. Call `vid_end` once, or `vid_cancel` on an abandoned frame

The draw list preserves submission order. Materials select depth test/write, blending and nearest/linear filtering. `repeat=0` clamps UV to the image edges; `repeat=1` repeats the complete texture on both axes, including UV outside 0..1. The producer supplies already transformed geometry; VID stores no camera motion history, interpolation or reset policy.

`vid_sample(sample, x, y)` queues a single color-pixel read at that point in submission order. Coordinates are normalized within the current viewport, top-left origin, each in `[0, 1)`. The queue retains the sample until end or cancellation. Reads occur from the current draw target before swap, after earlier commands and before later commands. Resize uses the same rescaled viewport as draws. Cancellation preserves the previous result; an unrendered/minimized frame does not update it. OGL uses synchronous `glReadPixels`; limit probes to required pixels

RapidRacer submits the sun before world geometry and the six lens ghosts after world geometry. It queues a center-pixel probe before HUD. The following frame uses the RGB555-white predicate and original 10/20-step fade; per-view probes persist across races, while fade/validity reset with the game renderer. Direction, projection and ghost positions use float math. Paused rendering does not update fade or probes

## OG/HD transition

Windows and Web initialize `extern int xport_hd` to 1. The host toggles it on a non-repeated Tab keydown. It does not map Tab into PSX PAD buttons. The game latches its frame path at a complete producer boundary.

Paused-scene ownership belongs to the game. RapidRacer renders a fresh HD scene and full-window pause overlay without dispatching simulation callbacks; it queries the current projection and cached target indicator and preserves game RNG/render sampling/trail state around render-only work. Switching to OG during pause constructs a complete OG scene and creates its software capture once. VID retains no pause, simulation or interpolation history

RapidRacer registers `hd_free` through `xport_set_cleanup` before calling `hd_init` during application startup. Host shutdown invokes it before destroying VID and its GL context, including Windows window-close unwinding. Race exit calls `hd_reset`: cancel pending work, clear counts and invalidate the VRAM upload version while retaining shader, VRAM image, materials and mesh capacity. Immediate UI/effect draws use a reusable mesh pool; reset its cursor once per frame, keep each queued mesh immutable and retain handles between frames and races. Grow capacity only when required

Render-only snapshots belong to game code. RapidRacer preserves RNG/render sampling/trail state while rebuilding a frozen transition scene; the game accumulates the original two-buffer circle fade through geometry and subtractive materials. Switching to OG during this transition builds a complete rasterized scene before presentation. Frozen OG submission uses the draw stage without a second video sync that could latch frame skipping again

`gpu_set_raster(0)` suppresses software primitive rasterization and OG presentation while preserving draw-state commands and image transfers. `gpu_set_raster(1)` restores rasterization and clears the latched frame-skip backlog after HD. The first returned OG frame must be constructed completely before presentation. `LOCKSTEP_DEBUG` retains rasterization.

RapidRacer brackets each ordinary OG frame with `gpu_frame_begin` before its blocking VSync and `gpu_frame_end` after all viewports/OT passes, then calls `gpu_present`. VSync polling cannot present an untouched skipped display page during recovery or a partly drawn split frame. These scopes suppress presentation only; frame-skip selection, simulation, GPU commands and VRAM transfers retain their existing behavior. Scopes nest and reset on GPU reset/load; they are excluded from portable GPU state

`gpu_vram_version()` changes on image upload/CLUT writes, MoveImage, StoreImage, clear, DMA upload, snapshot import/load and OG-to-HD raster transition. Consumers compare one remembered version for the complete VRAM image. RapidRacer uploads one 1024x512 RGBA5551 image before rendering the collected HD frame; its game-owned fragment shader handles native tpage/CLUT addressing. No per-material texture decode or hash is performed

Original PSX texture colors expand five-bit channels with bit replication: `(c << 3) | (c >> 2)`. Normalize those eight-bit values by 255 for HD shading. Keep the original STP bit separate from color expansion

RapidRacer submits environment/effects and route billboards, opaque water, opaque actors and blended actors. Route checkpoint/bonus billboards test and write depth in the world pass before all water; they are not interleaved with water strips. All water tests depth without writing it. Its per-view draw ranges retain route-strip order independently of material batching: `camera->dir == 1` reverses the collected strip groups; other directions retain them. Both paths draw far strips before near strips and preserve polygon order within each strip. This is a directional route traversal without geometric sorting. Retain range capacity across frames/races and free it during `hd_free`. No polygon offset is applied. User acceptance must cover both directions, distant racers, wakes and Hawaii/Bear Lake scenes; synthetic material/order tests do not prove those gameplay cases

RapidRacer's `hd.c` selects the mode-8 showcase path at the scene boundary. Its radial surface reads the horizon owner's radius table; event models, selected marker colors and border points remain owned by their original game modules. GPU geometry uses float clip coordinates, the shared VRAM image and retained VID meshes. Event shading reuses the original CPU lighting function with float-projected depth cue. Runtime acceptance requires an available original challenge, visible radial/event geometry, resize and complete OG/HD roundtrips; a successful build alone does not establish that acceptance

## Verification

- `tools/tests/native/vid_ogl_test.c`: actual Windows GL shaders/FBO pixels, OG color/orientation/crop/4:3, float HD geometry, deferred resource release and resize between frame submission/presentation with odd split dimensions; an optional argument supplies a game fragment source file for native compilation/link verification
- `python -B -m unittest tests.test_pad_ports tests.test_web_timing -v` from `tools`: Tab/PAD isolation, repeat handling and Web timing
- `X gpu_observer_test --configuration Debug`: actual software raster suppression, retained draw state/image transfers, complete-frame presentation recovery and restored OG output, plus existing GPU fixtures. Debug is the default; Release requires an explicit configuration argument. Debug outputs use `_build/gpu-observer/Debug`
- Consuming projects own actual browser/game scenarios and their evidence under `status`

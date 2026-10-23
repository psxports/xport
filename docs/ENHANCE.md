# Graphics enhancement (`enhance`)

Use this workflow for graphics work tracked with the GitHub label `enhance`. It applies to existing xport games, including racing, action and first-person games. Keep game-specific algorithms, constants, visibility data and verification evidence in the consuming project.

Inputs: a working original renderer, identified simulation/render boundaries, original assets and a reproducible scene. Outputs: an independently selectable HD producer, unchanged gameplay state, VID-owned graphics resources and evidence for both producers. [VID.md](VID.md) defines the exact API and resource contracts; [LIBRARY.md](LIBRARY.md) defines shared timing, GPU and input behavior.

## 1. Separate the two frame producers

Retain the original PsyQ/GTE/software-raster path as OG. Implement HD as a separate producer of geometry and VID commands. HD does not need to emulate PsyQ packets or ordering tables; it consumes the game's native objects, source geometry, camera, lights and texture identifiers.

The host exposes `extern int xport_hd`, initially 1. Tab or an L3 press from either controller toggles the selection on a press edge. These controls belong to the host and do not become game PAD buttons. Latch the selection at a complete game-frame boundary so a toggle cannot mix producers or split-screen views within one frame.

Both producers use the compiled graphics implementation. OG rasterizes into its original framebuffer and presents through `vid_blit`; HD renders into the native drawable resolution. Select the graphics implementation at compilation, currently `GAPI_OGL` on Windows and Web. Do not recreate a context when toggling OG/HD. GDI-only and Canvas2D output are absent.

On Windows, OpenGL 3.3 core uses shaders. Base OpenGL entrypoints link through `opengl32.lib`; resolve extended GL/WGL entrypoints dynamically using the declarations in `src/libs/gl`. Web uses WebGL 2.0/GLSL ES 300. Keep GL calls, handles and implementation structures private to `vid_ogl.c`; games include the common `vid.h`. Future implementations can use names such as `vid_vk.c` and `vid_dx12.c` without exposing their APIs to games.

## 2. Put HD rendering in the game-owned `hd.c`

Create `hd.c`/`hd.h` in the consuming game. Put HD frame construction, shaders, material selection, render-only snapshots, geometry caches and draw submission there. Use `hd_*` entrypoints and private helpers, arranged in dependency order without unnecessary forward declarations.

Original functions remain the integration points. Add a branch at a coherent rendering boundary, call its HD counterpart and return before entering the original packet-building path. Avoid scattering mode checks throughout polygon loops. A representative integration shape is:

```c
void scene_draw(const SCENE *scene)
{
    if (game_hd_frame)
    {
        hd_scene(scene);
        return;
    }
    scene_draw_og(scene);
}
```

This is an illustration, not a shared xport API. The game owns the latched `game_hd_frame` value and its scene types. Keep hooks at scene/view, object group, UI submission and lifecycle boundaries. A frame dispatcher can use names such as `game_frame_og` and `game_frame_hd`.

Keep private asset decoding or stateful calculations with their original owner when that owner is required to interpret the data safely. Small `hd_*` adapters can expose immutable source data or a float rendering result. Do not move gameplay, input semantics or asset ownership into `hd.c` merely to eliminate an include. Such adapters are exceptions to concentrating the rendering implementation in `hd.c`, not another graphics implementation distributed across every module.

Identify renderer calls that also update RNG, animation, audio, visibility bookkeeping or simulation state. Run required state updates once per logic tick and keep HD redraws observational. Reusing an original function that advances state during every redraw breaks variable render cadence and pause behavior.

## 3. Preserve software GPU state while HD is active

Use `gpu_set_raster(0)` to suppress software primitive rasterization and OG presentation during HD. Do not rasterize a second software frame solely to keep the toggle available. Continue required GPU state commands, VRAM uploads, palette uploads, DMA transfers, moves and readback operations.

Use `gpu_set_raster(1)` before returning to OG. Build a complete original frame before presenting it; restoring rasterization clears the previous frame-skip backlog. Do not display a stale or half-constructed page after a toggle. Bracket a complete OG frame with `gpu_frame_begin`/`gpu_frame_end` across VSync and all viewports, then call `gpu_present`.

Framebuffer-dependent effects need separate treatment: preserved StoreImage/MoveImage semantics do not produce a fresh HD framebuffer in software VRAM. Identify screen feedback, occlusion probes, pause captures and transitions explicitly. For a small visibility probe, use an ordered `vid_sample` before HUD and consume its completed result on the next frame. Rebuild a paused/transition scene without advancing simulation. Use a larger render-target solution only when the effect requires actual image feedback; a color fade alone does not justify another full-screen render target.

## 4. Add a separate float rendering math path

Keep fixed-point gameplay and original GTE use unchanged. Introduce float equivalents only where rendering consumes transforms, trigonometry, projection or lighting. Shared types include `VEC3F` and `QUATF` in `xport.h`, and `VECTORF`/`MATRIXF` in `psx.h`.

Use the existing helpers instead of a second project-local math library:

- `IdentityMatrixf`, `RotMatrixf`, `RotMatrixYXZf`, axis rotations
- `MulMatrix0f`, `MulMatrixf`, `MulMatrix2f`, `CompMatrixf`
- `TransMatrixf`, `ScaleMatrixf`, `TransposeMatrixf`, `ApplyMatrixf`, `RotTransf`
- `NormalizeVectorf`, `OuterProductf`, quaternion conversion/normalization/multiplication and `SlerpQuatf`
- `ProjectVertexf` and `RotTransPersf` for an initial CPU projection or coarse visibility checks

Float rotation helpers take radians and do not access GTE registers. Convert original 12-bit turn units using `RAD12`. Convert rotation coefficients from their original fixed-point scale, preserve translation units, matrix order, handedness, camera sign conventions and original lighting normalization. Do not divide translations by the rotation scale. Confirm these rules for the source game before replacing its render math.

The initial HD implementation can transform/project float geometry on CPU and submit clip vertices. This establishes the camera, aspect, visibility and material behavior before GPU migration. Then move per-vertex work into vertex shaders while retaining CPU float object/camera poses.

Never write an interpolated transform back to physics, collision, AI, aiming or gameplay visibility state. More frequent HD input sampling must not introduce more physics steps or change their duration. Treat any input-rate change as a separate audited gameplay change.

## 5. Add frame interpolation and separate logic ticks from rendering

This enhancement stage uses the shared `xport_frame_fract(uint64 *idx)` API from `xport.h`. The toolset owns conversion of elapsed application time into a physical logic-frame index and interpolation phase; the game owns simulation steps, render snapshots and their interpolation. Use this API when adding frame interpolation to a project instead of introducing a game-local clock calculation:

```c
uint64 frame_index;
float frame_fract = xport_frame_fract(&frame_index);
```

The function writes the current tick index through the required `idx` pointer and returns its fractional phase as `float`. Both values come from one monotonic integer timestamp since application startup; the calculation does not require double-precision time. Calling it observes the clock without advancing simulation, waiting for VBlank or rendering a frame. Sample it again after processing pending ticks, since simulation work can cross another tick boundary.

The physical rate follows the shared PAL/NTSC profile: nominal `FIELD_RATE=50` selects `53203425/1069484` Hz, and `FIELD_RATE=60` selects `53693175/897619` Hz. Other nominal rates use `FIELD_RATE/1`; explicit `FIELD_RATE_NUMERATOR` and `FIELD_RATE_DENOMINATOR` overrides remain available. Games must not duplicate this conversion or assume exactly 50/60 ticks per wall-clock second. `FIELD_RATE` belongs to the game, not the monitor refresh rate.

`xport_frame()` returns only the physical field index. `xport_timer_wait_frame(target_field)` waits for an absolute target field using `Sleep(1)` on Windows or `emscripten_sleep(1)` on Web; it returns immediately if the target has already been reached. `xport_frame_time` is removed; game code uses field indices and interpolation phases instead of converting fields back into time. The game owns its last processed tick cursor. Reseed that cursor and previous/current render samples at scene entry, resume and OG/HD transitions where elapsed time must not be replayed as catch-up simulation.

Run logic for pending `frame_index` steps. Render independently using `frame_fract` between the two most recent complete states, N-1 and N. At 0 the image shows N-1; at 1 it shows N. This adds up to one tick of visual latency. It does not predict N+1 or modify physics. Use the host render wait separately from logic timing; native `VSync(-1)` observes field delivery without imposing another blocking field per HD redraw.

When preserving an original game-owned counter deadline, use `xport_render_wait_until(uint64 deadline)` for the intervening HD redraws. Its argument is an absolute `xport_timer_get` microsecond deadline. Return 1 selects a monitor/render event; return 0 selects the supplied deadline. Windows preserves its monitor schedule across earlier deadline returns; Web races requestAnimationFrame against a timeout and cancels the losing callback. Operating-system or browser delays can deliver either event late: service the original clock and recheck the game deadline before drawing. If a render event is delivered at or after the game deadline, retain one pending presentation request, process the due logic tick and consume that request after collecting the completed scene. Returning to logic must not discard an event already consumed from the host schedule; clear pending requests on producer reset or OG return. The function does not advance game logic or poll game PAD APIs. Retain the original counter configuration, wait fractions and logical update count; do not replace an independently clocked original simulation with one update per physical field. The unbounded `xport_render_wait()` remains available. Verify the actual implementations with `python -B -m unittest tests.test_render_wait tests.test_native_timing tests.test_web_timing tests.test_pad_ports -v` from the toolset `tools` directory.

Capture only values that move: object/camera position and orientation, animated scale, procedural parameters and necessary effect state. Interpolate positions/scales linearly and orientations with normalized quaternions/Slerp. Preserve camera modes and real projection changes. Reset previous=current on creation, teleport, respawn, camera discontinuity, scene change and OG/HD reseeding.

Fixed object/effect pools provide stable slots. Reset their previous state directly in creation functions rather than adding IDs or searching the pool. For trails, match samples through their real lifecycle; interpolate the final section's disappearance so removing a tail sample does not cause a visible jump every tick. Storing one more vertex does not solve incorrect sample correspondence.

Do not snapshot static HUD/pause primitives just because the scene is interpolated. Menus can use pure analytical animation of position and scale when the existing animation model permits it. Evaluating N+1 is safe only as a render-only calculation: it must not dispatch another menu update, consume input/RNG or change selection/unlocks. Keep text and discrete selection changes tied to logic.

## 6. Establish native-resolution layout

OG uses best-fit output in the largest client-area rectangle with the original 4:3 aspect. Respect the actual display crop; framebuffer storage can contain unused PAL rows. HD uses the drawable dimensions returned by `vid_resize`, including device-pixel scaling on Web.

Use the current viewport aspect for projection and coarse visibility. Remove original screen-space rejection based on a fixed 4:3 rectangle; test whole-object bounds against the current frustum. Increasing the shader viewport cannot recover objects already rejected on CPU. Preserve vertical framing and derive horizontal framing from aspect and the game's camera convention.

At 4:3, preserve the original UI/HUD size and placement. At wider aspects, use vertical resolution as the UI scale, keep menus centered, anchor edge HUD elements to their respective sides and retain center-aligned notices in the center. Draw UI directly at native resolution; do not upscale a software UI framebuffer. Each split-screen viewport uses its own aspect, anchors and original layout rules. Shared edges must remain consistent at odd drawable dimensions.

Enable filtering by content category, not globally. UI, world geometry and full-screen backdrops can require different sampling. Clamp filtering to a logical image region so an atlas neighbor or repeat seam does not create a line on the horizon. Preserve the chosen half-texel conventions and verify moving/scaled UI at fractional display scale.

## 7. Move transforms and lighting into vertex shaders

Upload immutable source geometry once, then send object/camera/light parameters per draw. Move model/view transformation, projection, per-vertex lighting, ground/detail UV calculation, billboard/line expansion and fog coefficients to the vertex stage as needed. Keep CPU coarse visibility and interpolated object poses; do not rebuild the vertex buffer when only a matrix changes.

Use native-resolution clip projection with explicit near/far planes and depth mapping. Ordering tables no longer determine ordinary HD 3D visibility. Replace PS1 subdivision used to hide affine distortion with ordinary triangles; perspective-correct interpolation and GPU clipping remove the need for that subdivision in the HD path. Keep OG subdivision intact.

Start with the original flat/Gouraud lighting behavior, including source color scaling, saturation and special depth cues. Preserve a flat face's intended lighting across both triangles. Improved material lighting is a later change, not a prerequisite for GPU transforms.

Shaders are game-owned source strings passed to `vid_shader_create`; VID adds GLSL 330/ES300 declarations and compiles/links them. Use explicit attribute locations, initialize all outputs on clipped paths and match integer/flat interfaces between stages. Pass `uDraw` vectors with `vid_uniforms`; their count must fit both the declared shader array and `VID_UNIFORM_VECTORS`. Matrix/light layouts and their vector counts belong to the game, not to VID's ABI.

A useful split is a general world stage, a native-resolution UI stage, and specialized stages for large procedural surfaces. Separate shaders by actual algorithm requirements; blend and depth state still belong to passes.

## 8. Keep source geometry compact

The implemented VID layouts support gradual migration:

- `VID_VERTEX`: 40-byte float clip position, UV and RGBA, suitable for the first CPU-transformed implementation
- `VID_VERTEX3D`: 72-byte float position, UV, RGBA, normal and extra parameters, useful as CPU staging and an initial GPU source format
- `VID_VERTEX16`: 52-byte compact source format for the batched world path
- `VID_VERTEX2D`: 44-byte native-resolution UI format

The compact world layout is:

```c
typedef struct VID_VERTEX16
{
    sint16 pos[4], uv[2];
    uint8 color[4];
    sint16 n[4];
    float extra[4];
    uint16 tex[4], rect[2];
} VID_VERTEX16;
```

Keep original integer positions/normals when their range and precision fit this representation. Matrices and vertex shader calculations remain float. Interpolation of an object's pose therefore does not require rewriting or rounding its mesh vertices. Dynamic world-space geometry that has fractional positions must preserve that precision: the reference implementation packs a residual into unused normal components for unlit vertices and identifies it with an attribute flag. Use a float layout where compact encoding would lose required range or precision; do not blindly narrow every procedural effect.

Texture metadata uses integer attributes at locations 5/6 and flat integer varyings to the fragment stage. It carries page, CLUT, sampling/STP flags, logical image size and region origin. UVs can represent half texels through a documented integer scale. Normals, colors and mode fields require explicit scale conventions; do not enable normalized GL attribute conversion unless the shader expects it.

Use uint32 index buffers for merged geometry that can exceed 65535 vertices. An element buffer is GPU storage, separate from the vertex buffer. `vid_index_data` exposes mutable CPU staging, `vid_index_commit` uploads changed content on execution, and `vid_index_static` seals static data and releases its CPU copy after upload. Keep small range offsets on CPU. Mutable visibility indices retain CPU storage because selection changes.

## 9. Sample the original VRAM once

During the first HD iteration retain one 1024x512 `VID_RGBA5551` VRAM texture. Compare a single `gpu_vram_version()` with the last uploaded version and commit the complete image when invalidated. Uploads/CLUT changes, moves, readback, clears, DMA and state import can invalidate the version; use the shared counter instead of inspecting every material's VRAM region.

Decode original 4-bit, 8-bit and 16-bit texture addressing in the game fragment shader. Preserve palette lookup, transparent zero texels, STP and native channel expansion `(c << 3) | (c >> 2)`. The GPU texture's RGBA5551 transport layout differs from a raw PS1 word's channel/flag bit positions; perform the required conversion and reconstruct the original word consistently.

Page and CLUT must be vertex metadata rather than per-material uniforms. Different textures can then share a draw when their actual GPU state and object parameters agree. Atlas-adjusted UVs can remove part of page addressing only if indexed-palette lookup and page format still have enough metadata.

Later replacement textures/models/materials can use separate VID resources. Keep that remaster layer separate from the original-asset HD path so OG remains available for comparison.

## 10. Replace polygon-sized draws with persistent geometry and passes

Optimize the source of calls before adding state caches, statistics infrastructure or instancing. A large number of tiny meshes/material-uniform updates cannot be fixed by a more complex fragment shader.

1. Decode/cache source assets at level load or first use, independent of a viewport. Combine static environment groups into a shared vertex buffer. Preserve group bounds and index ranges for visibility selection.
2. Collect visible indices per viewport and required pass. Deduplicate a group referenced by several visibility cells. Update index buffers only when their contents/count change; leave the static vertex buffer untouched.
3. Merge compatible mesh parts using shader/pass state, not texture page/CLUT. Texture identities already travel with vertices. Retain ordinary object draws for different interpolated poses; a few actors do not justify an instancing subsystem.
4. Use explicit `vid_pass` for depth test/write, blend equation, culling, shader and textures. Submit compatible ranges with `vid_submit`, updating only required object parameters. VID configures persistent VAOs once and uploads only committed resource versions.
5. Accumulate dynamic effects into retained buffers. Preserve source order within a pass; do not merge across an intervening incompatible object transform and reorder alpha geometry. Flush UI on viewport/blend or scene/overlay boundaries.
6. Retain capacities across frames. Grow when necessary; do not allocate a mesh, shader or material for every primitive or redraw.

For a large animated surface, build its topology once. Give it a specialized vertex shader and pass tick/interpolation parameters plus animation tables in integer textures or suitably sized uniforms. Compute deformation, color changes and UV scrolling there for N-1/N and interpolate the results. Keep dynamic impact/activity history separate from immutable geometry. A binary deformation weight permits `mix(original, deformed, weight)` for fixed boundary vertices without a branch.

For an ordered circular surface, keep one forward and one reverse index sequence. Prefix offsets select one draw range, or two when visibility crosses the ring seam; do not double the sequence just to avoid a second draw. This is a topology-specific optimization. Room/portal visibility, arbitrary terrain and action-game sectors need their own index collectors; do not impose a route-segment representation on them.

Measure comparable scenes in RenderDoc after restructuring. Distinguish draw count, total API calls, CPU preparation, GPU duration and frame rate. A low API/draw ratio is not itself a goal. One pilot capture fell from 1245 draws/36771 API calls to 18/309 after this architecture change; these are reported capture results, not a target or a guarantee for another game.

## 11. Preserve material and occlusion semantics

Separate opaque, alpha, additive and subtractive passes. A shader cannot choose depth writes or blend equations per polygon. Preserve ordering wherever blending is noncommutative; a material regrouping that changes overlap is a rendering change.

A textured PS1 semitransparent primitive can contain both opaque STP=0 and blended STP=1 texels. Split those into disjoint shader-masked passes where needed. Do not draw a nonzero texel twice. Conversely, do not assume a primitive's semitransparency bit makes every texel blended. If original assets prove an effect's palette contains only one class, omit its empty pass. Validate that property offline for all variants; do not add per-frame texture/CLUT scans or force a different blend equation based on the effect's name.

Use depth writes for opaque geometry where they reproduce the intended visibility. Some original intersecting surfaces/effects deliberately relied on submission order. Their migration can require a separate ordered pass with depth testing but no depth writes. Determine that order from the scene's topology/camera or explicit sorting. Do not prescribe one universal water/actor/effect order across games or use polygon offset to hide an unexplained depth mismatch.

Overlay 3D objects can use a reserved depth interval through projection mapping instead of clearing world depth before every overlay. Keep near/far projection and actual viewport values explicit; the common shader can apply this mapping without a separate program per interval.

For a longer draw distance, audit every CPU rejection, visibility list, LOD selector and effect-specific distance gate. Original sections can overlap geometrically because the original renderer never exposed them together. Represent intended visibility explicitly; increasing a global distance or disabling LOD does not prove every section should be visible. Debug visibility editors belong to game tools, not to the shared VID API.

Fog can mask valid distant appearance/disappearance, not invalid visibility. Compute camera-space radial distance in the vertex shader and interpolate completed fog coefficients. Screen-space depth changes with viewing direction and is unsuitable for stable radial fog. A useful two-stage model blends into a scene fog color, then reduces opacity toward an already drawn horizon. Choose colors and distances from each scene's atmosphere; preserve opaque/alpha/additive behavior and avoid an offscreen scene texture when ordinary blending is sufficient. Large polygons may require denser geometry or per-fragment distance if vertex coefficients are visibly inaccurate.

## 12. Resource lifetime and integration checks

The host owns context initialization/destruction through `vid_init`, `vid_resize`, `vid_free`. The game owns VID resource references; VID owns their CPU/GPU storage, handles, uploads and queued lifetime. Do not add a private VID header or require games to manage VAOs, buffers or GL textures.

Create game shaders and common growing buffers during application initialization/first use and retain them until shutdown. Register game cleanup with `xport_set_cleanup` so resources are released before the graphics context. A level reset releases level geometry, selected indices and level-specific animation data; it cancels pending commands and reseeds interpolation while retaining common capacity. Freeing a submitted reference defers destruction until queued work completes. Do not modify queued mesh/index/material data; update a shared texture before `vid_end` only according to VID's documented final-image semantics.

Build Debug after an implementation stage. Build Release only when the project's/user's build policy permits it. Use the consuming project's Windows solution and Web Makefile; resolve their paths from `xport-project.json`. Shared targeted checks include:

```text
python -B [XPORT_ROOT]/tools/xport.py --project PROJECT_ROOT gpu_observer_test --configuration Debug
python -B [XPORT_ROOT]/tools/xport.py --project PROJECT_ROOT gpu_observer_test --fixture vsync_callback --configuration Debug
```

From `[XPORT_ROOT]/tools`, run `python -B -m unittest tests.test_native_timing tests.test_web_timing tests.test_pad_ports -v` after changing shared pacing/input. Native renderer fixtures live under `tools/tests/native`; the consuming game owns extraction/tests for its custom shader interfaces and procedural geometry.

For each stage verify actual shader compilation/linking and pixels on native GL and WebGL 2.0, buffer ownership, large indices, visibility boundaries and pass order. Exercise OG-to-HD-to-OG, pause, transitions, restart, object creation/destruction, wide/4:3/odd-sized viewports and every supported split layout when runtime verification is authorized. Keep shader success, build success and gameplay acceptance as separate evidence.

Test timing at a workload near the field budget as well as at low load. Count elapsed VBlanks since the preceding completed native blocking wait, include the initial wait span in frame-skip scheduling, and preserve whole-frame submission boundaries. A renderer that submits at 50Hz can still expose only 25 unique frames if every second raster/presentation is skipped. Interpolation in HD is not proof of correct OG cadence.

Completion requires the original producer to remain usable, HD rendering to consume render-only state, all resources to have an owner/lifetime, and the requested behavior to be verified at its actual scope. Record project evidence under `status`; leave asset-specific visibility, fog values, effect palettes and captures out of shared task instructions. Replacement assets, higher-detail materials and further GPU algorithms are subsequent `enhance` work, not prerequisites for preserving the initial original-asset HD path.

## 13. Diagnose migration defects

Start from one reproducible scene, camera pose, viewport and logical state. Compare OG/HD at that state; a different tick, camera or animation phase can invalidate the comparison. Record whether the defect exists in the original image, appeared in the first HD implementation, or appeared after GPU/batching changes. Keep diagnostics temporary and out of normal game controls unless the user requests them.

### Render functions advance the world

Symptoms: changing render rate changes animation speed, repeated paused redraws move objects, toggling OG/HD changes effects, or input/audio/RNG behaves differently at high refresh rates.

Trace the original render function and its callees for writes to simulation owners, RNG calls, effect allocation, animation counters, audio dispatch and controller sampling. Classify each write as a logic update, a render snapshot or transient packet construction. Move required updates to the existing logic-tick boundary and let HD consume their results. Preserve the original number and order of gameplay updates. Pass an explicit update/render-only distinction where a stateful owner must still prepare data.

Verify repeated renders of a fixed state leave gameplay values unchanged. Pause/resume, two viewports and extra render-only frames must not dispatch those updates twice. Saving/restoring a small legacy render-only state can be a bounded adapter during migration; do not clone the world or replay physics to obtain another image.

### Switching or pausing exposes stale pixels

Symptoms: the first returned OG frame is blank, alternates with an old page, shows only one split-screen view, or a pause/transition differs after toggling modes.

Follow the complete frame boundary: producer selection, raster-enable change, VSync/frame-skip latch, draw/display page selection, all OT passes, overlay and presentation. An old software framebuffer remains old while HD is active. Rebuild a complete OG image before exposing it and suppress automatic presentation during its construction. Render all views before releasing the frame boundary.

For a paused or frozen transition, rebuild presentation from the frozen state without advancing callbacks, RNG or effect history. A screen capture used by OG is not automatically an HD capture. Identify which effect actually requires pixels and which can be drawn from geometry/color parameters. Compare switching in both directions during ordinary play, pause, fades and scene exit; do not validate only an unpaused steady frame.

### Interpolation creates periodic jumps

Symptoms: actors jump on spawn, a camera swings across a mode change, a trail end snaps every tick, or a menu position is smooth while its size still steps.

First verify that previous and current samples represent the same live object or effect section. In a fixed pool, seed previous=current at allocation/reuse. Reset on a discontinuity rather than interpolating between unrelated poses. Keep orientation representation normalized and preserve the correct matrix composition when converting to/from quaternions.

For a trail, inspect insertion, index shifting, section pairing and removal separately. The oldest section needs a continuous fade/removal policy consistent with N-1/N sampling; reversing the interpolation factor or adding a vertex without fixing correspondence does not solve a lifecycle error. Use a deliberately reduced test tick rate only as a diagnostic, with gameplay timing implications understood, and restore the project's field rate afterward.

For UI animation, inspect position, scale, parent transforms, scrolling and wrapping together. Sample all continuous properties at the same render phase. Evaluate analytical menu animation only through pure calculations; never compute a future state by consuming another input/update. Static HUD and pause rectangles require layout evaluation, not per-tick geometry snapshots or identity tracking for their text.

### Depth reveals tricks hidden by the original image

Symptoms: distant scenery appears above a nearer surface, inverted scenery looks like a reflection, a vehicle becomes partly submerged after enabling depth writes, or an attached effect occludes its owner unexpectedly.

Inspect the primitive's source geometry and original order before changing projection. PS1 scenes can use intersecting strips, mirrored geometry, decorative underside polygons and color/texture shading that resemble real 3D effects. Treat a suspected reflection as a hypothesis until the original path or asset proves it. Geometry visible only in HD can also mean the original CPU visibility rejected it; depth is not the only possible cause.

Compare depth test, depth write, shader depth mapping, camera/model matrices and submission order for the overlapping draws. Determine whether the original effect is geometric, texture-based, vertex-colored or a deliberate ordered overlay. Preserve intended appearance through an explicit pass or depth interval. Do not add polygon offset as an unexplained compensation. If the same defect is present in OG/original gameplay, record it separately and keep its repair outside the migration unless requested.

### Two draws do not necessarily mean double contribution

Inspect the actual surviving fragments, not just repeated vertex/index ranges in a capture. Mixed STP primitives legitimately submit identical geometry to two disjoint masks; one mask can also be entirely empty.

Decode the effect's original palette and texture format offline. Check transparent zero, STP=0 and STP=1 colors across every relevant asset variant and any runtime palette writers. If one nonzero class is absent and the invariant persists at runtime, retain only its contributing pass. If both classes occur, retain both masks. Check blend equation, vertex alpha and depth-write policy independently; a fog-enabled opaque pass may have alpha blending enabled without being an additive effect.

An effect named flame must not be forced into an additive pass solely from its name. In the pilot, the flame palette contained only non-STP visible colors while the splash palette contained only STP visible colors, so their empty passes were opposite. This is an asset property to establish for each game, not a shared renderer rule. Confirm the retained image and geometry counts after removing a pass; do not introduce runtime scans of every material's VRAM region.

### Localize by the first stage that loses correctness

- Missing objects or section-sized popping: inspect CPU candidates, original distance gates, visibility lists, deduplication, LOD choice and current-aspect frustum bounds before changing shader clipping or fog
- Geometry present in the draw but overlapped incorrectly: inspect projected depth, test/write state and order; distinguish opaque surfaces from ordered effects
- Gaps at surface boundaries: compare shared source vertices and deformation weights; an originally fixed edge must remain fixed on both sides, including previous/current samples
- Atlas borders, horizon seams or changed UI texel boundaries: inspect logical image rectangles, repeat/clamp, neighboring texel access, filtering and half-texel convention before applying a global UV offset
- Whole-image brightness or incorrect transparency: inspect channel expansion, color normalization, STP masks, blend equation and duplicate contributions before changing lighting
- Tick-period motion jumps: inspect sample identity, reset points and interpolation phase before modifying simulation or adding more snapshots
- Low unique-frame cadence with normal game speed: inspect blocking waits, elapsed-field accounting, frame-skip decisions and complete presentation boundaries; distinguish repeated presentations from genuinely new images
- Too many draw/API calls: locate polygon-sized submissions, per-texture uniforms, repeated static uploads and incompatible pass boundaries before adding state caches or instancing

For each repair retain a minimal reproducer and the evidence that identifies the failing stage. Verify both the original invariant and the corrected HD behavior; a screenshot, shader pixel test, topology check, build and timing test prove different parts of that result.

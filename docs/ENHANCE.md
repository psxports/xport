# Enhance

Agent command: `enhance`. Read current [context](CONTEXT.md), this contract and only the affected sections of [ENHANCE_REFERENCE.md](ENHANCE_REFERENCE.md). [VID.md](VID.md) owns graphics API behavior; [BUILD_POLICY.md](BUILD_POLICY.md) owns builds/execution. Do not expand beyond the requested enhancement.

## Goal and plan

Create one active goal for the complete requested implementation and final verification, or resume the existing covering goal. First save a concrete numbered plan with observable completion conditions, dependencies, integration and final checks in the current project plan/context. Reuse existing work and start implementation in the same turn; a plan, dispatched build or isolated feature is not completion.

Plan from actual simulation/render/resource boundaries and supported platforms. Required inputs are a working original producer, original assets and reproducible scenes. Initial Web build/delivery belongs to decomp; do not add it as a deferred enhancement. Recheck Web here only for agreed affected enhancements under BUILD_POLICY. Record unsupported/unknown contracts before guessing an HD replacement.

## Implementation contract

1. Retain selectable OG rendering; add a separate game-owned HD producer (hd.c/hd.h or the project's recorded implementation language). Choose OG/HD once per complete frame, including all split views
2. HD consumes render-only native snapshots through VID. Preserve simulation updates, input, RNG, audio dispatch and original call order. Repeated renders, paused redraws and extra viewports must not update gameplay again
3. Keep the existing PsyQ/GTE/software GPU owner and required draw-state/image-transfer effects. Suppress only raster/presentation according to the established API; do not create another mutable VRAM/device model
4. Use a separate render-only float path, consistent transforms/projection/fog and compact original geometry. Preserve logic fixed-point behavior. Match exact shader attribute/uniform contracts and coordinate domains from VID and the relevant reference section
5. Interpolate only render state. Separate original logic ticks from presentation; seed/reset snapshots on scene changes, restart and discontinuities. Do not predict a future logic step that consumes input/RNG or changes state
6. Establish native-resolution UI/layout, then persistent geometry, passes and batching where requested. Preserve material/alpha/STP, texture window, CLUT/TPAGE, ordering/depth/visibility and unsupported-case behavior. No blanket sort or guessed packet reinterpretation
7. Give every shader/buffer/texture/cache an owner and initialization/reset/release scope. Host owns graphics context lifetime; game owns VID references; VID owns implementation resources and queued lifetime. Do not edit/free submitted resources before their contract permits it

Read only the reference section for the current item: producer boundary, GPU state, float math, interpolation, layout, shaders/geometry, VRAM sampling, persistent passes, visibility or resource lifetime. Follow [FMV.md](FMV.md) and [HOST_TOOLS.md](HOST_TOOLS.md) only for relevant movie preparation.

## Verification and repair

Use one reproducible logical state, camera and viewport to diagnose an OG/HD difference. Distinguish original defects, initial HD defects and batching/shader regressions. Keep temporary diagnostics outside normal controls unless requested.

For selected targets verify real shader compilation/linking and pixels, pass/occlusion order, buffer lifetimes, supported index ranges and viewport/split layouts. Exercise OG->HD->OG, pause/resume, transitions/restart and object/resource teardown. For timing/input changes cover field cadence under low and near-budget load; HD interpolation alone does not establish correct OG cadence. Build success, pixels, state preservation, performance and user acceptance are separate claims.

## Progress

At start/resumption and each rounded change rename the current chat to `enhance: n%`, n=floor(100*completed_plan_items/total_plan_items+0.5). In Codex use mcp__codex_app.set_thread_title(title=...) without threadId. If unavailable report the same fraction in chat and continue.

Keep a nonempty plan and stable denominator. Partial/unverified items remain open. A genuine scope/dependency discovery may revise the plan; record why and the new counts. Never delete unfinished items or split completed ones to raise progress. Integration and final verification remain explicit open items until their evidence passes. Do not add per-item build gates just for the progress counter.

## Completion

Require the requested enhancement on its agreed targets, usable original rendering, unchanged gameplay semantics, render-only HD inputs, correct resource lifetimes and all required final checks/user acceptance. Record current source/executable identities, scenarios and limitations in project evidence; asset-specific algorithms, palettes, fog values and captures do not belong in shared workflow docs. Higher-detail replacement assets or unrequested algorithms are separate work.

Stage project sources through X stage_sources. Report 100% and complete the goal only after every required plan item and final gate is complete.

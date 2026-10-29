# Build and execution policy

This file owns build timing/configuration rules. Explicit user restrictions override defaults. Checks alone do not authorize extra platforms or gameplay changes; original-game repairs use the explicit authorization in [REFACTOR.md](REFACTOR.md#original-bug-repairs-and-registry). Do not copy this policy into other documents.

## Task classes

- Documentation, indexing/CLI changes and static analysis: run affected tool/document checks. Do not build or launch games unless actual runtime/source/build-input changes require it
- Startup: supervisor builds the scaffold in Debug only after accepted game-icon resources. Original function translation and final game verification belong to decomp. No automatic Release build
- Decomp: no builds, runtime tests, launches or code_refresh during the full translation pass. Integrate the entire pass, then use Debug x86 and the agreed game scenario. After repairs, run final native Release and build/package/verify Web before completing decomp unless the user explicitly excludes a target. No per-function/module Release or Web builds
- Refactor: default migration is static-only, including no intermediate code_refresh or fixture adaptation. An explicitly selected compile_policy=abi-boundaries allows compile-only diagnostics for affected units after a coherent ABI family; no linking/tests/game execution. After interfaces stabilize, adapt fixtures once, perform configured Debug checks/scenarios and final native Release unless excluded. Default targets include x86 and x64; explicit narrower scope wins
- Enhance: verify coherent planned items in Debug and on agreed platforms/scenarios. Do not build per small edit. Run one final native Release after final Debug repair when native delivery is in scope; Web-only delivery has no native Release requirement
- Isolated source repair: use affected Debug checks, then final native Release if that native deliverable is in scope and not excluded. Toolset-only Python/document work does not acquire a game delivery requirement

## In-goal tests and final target matrix

For decomp, refactor and enhance, intermediate native builds/tests use Debug x86. Run all owned test/game/browser processes on the hidden desktop without audible output; no visible windows or consoles. Native Debug uses XPORT_AUDIO_OUTPUT=0; Web runtime checks use ?mute=1 (or &mute=1). Keep PCM decoding/mixing and clocks active. Static tools without windows run without a visible console. Manual GUI/audio requires an explicit user request. This rule does not introduce intermediate tests where the task class above forbids them.

Only when the goal explicitly implements/fixes x64 or Web behavior may affected intermediate checks use that target: x64 uses Debug x64; Web uses its Debug/development build and a hidden muted browser. A default final-target requirement alone does not enable intermediate cross-target builds. Keep checks to the changed target/contract and do not add unrelated configurations.

After implementation and default Debug repairs, check the remaining required configurations once at the final gate: native Release, selected x64 configurations and selected Web delivery. Decomp requires final Web delivery by default; refactor retains x64 by default; enhance rechecks Web only when it is an agreed affected target, not as first-time porting. All final automated executions remain hidden and muted. If a final check reveals a defect, repair and repeat only affected checks/configurations. Do not finish with unverified required targets or substitute build success for execution evidence.

## Tools and completion

`X code_refresh` is the final integration helper: format/index, configured Debug build, scoped tests/artifacts, database integrity and source staging. It is not a required gate after every edit. `--no-build`, `--no-tests`, `--no-format` narrow diagnostics; report exactly what was skipped. A narrowed run cannot substitute for an explicitly required full gate.

Keep code_refresh.build.configuration=Debug. Use `X build_native --configuration Release` for final native verification. Never force Release from a migration/package helper. A runtime failure may need further repairs and rechecks; do not rebuild again without a new change, failure or unresolved requirement.

Preserve user-owned games and jobs. Never overwrite a running executable or kill a manual process without authority. Owned automated game checks run hidden and muted; manual requested play retains the requested GUI/audio. Observe process completion; a launch, timeout or old report is not a passing result.

Completion requires every applicable check and explicitly required user acceptance. Record actual source/executable identities and scope. Build success does not prove gameplay or MIPS equivalence. If only human verification remains, report that gate without generating more builds to simulate progress.

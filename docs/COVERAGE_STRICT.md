# Strict coverage command reference

Applies only to an explicitly requested strict audit and the implemented contracts of existing strict supervisor commands. Ordinary coverage follows [COVERAGE.md](COVERAGE.md) and the canonical [full-pass translation policy](DECOMPILATION.md#full-pass-translation-and-batch-modules). The 16-function proof packets, transactional source allocation and intermediate gates below describe this separate strict route, not the default translation workflow.


Normative extension of `../AGENTS.md` for the chat command:

```text
coverage NAME
```

`coverage NAME` consumes the completed `status/function-coverage/NAME/functions.bin` produced by `function_converge NAME`. It translates and proves only the recorded game functions in first-call order. Direct game dependencies absent from the recording receive supervisor-generated link-only TODO stubs; they do not expand the audit set. Coverage never launches DuckStation and never invokes Codex CLI or another model process.

Before any audit work, create one active goal whose terminal condition is a complete coverage state and a successful final Release build. Keep that goal active across all batches. Mark it complete only after `X coverage status NAME` reports `status: complete`. An explicit user stop is the only normal early termination.

Run:

```text
X coverage start NAME
X coverage next NAME
```

`start` validates the immutable function list and its per-recording `receipt.json`, binds the configured `analysis.default_image`, preserves first-call order, and creates `status/function-coverage/NAME/coverage.json`. It inspects image-qualified direct `call`, `conditional_link` and out-of-function `tail_candidate` edges from those recorded functions. A direct target absent from `functions.bin`, without an accepted implementation and without reviewed PsyQ/SKIP classification becomes a deterministic link-only TODO stub. Do not traverse dependencies of that stub: its only behavior is immediate failure. A name, familiar listing or guessed PsyQ identity is not a library exclusion. A `SKIP` row without reviewed `library_classification` remains in the game-function denominator when it was recorded.

The progress string is always `X/Y/Z`:

- `X` is the number of game functions with an accepted complete proof. Existing `DONE` functions with database-validated static and runtime evidence start proved and are not audited again
- `Y` is the number of those game functions with a uniquely mapped accepted C implementation whose status is `WIP`, `DONE` or reviewed `SKIP`
- `Z` is the number of functions present in `functions.bin` after the proven PsyQ/SKIP exclusion. Link-only stubs are absent from `X`, `Y` and `Z`

Print `X/Y/Z` when the command starts, after every accepted batch and on completion. After each batch, add one short list of the semantic discrepancies found and corrected. Do not print build logs or unchanged-function summaries unless a gate fails.

`next` creates one immutable `coverage-batch-NNNN.json` containing up to 16 pending functions in first-call order. Every function context contains the complete immutable MIPS, complete available IDA pseudocode with explicit completeness, current mapped C, exact hashes and a machine-derived checklist. Treat complete pseudocode as the structural baseline and MIPS as semantic authority. Audit every basic block, branch target, delay slot, callsite, argument, return, register-carried value and memory effect. Do not return a local observed-case patch or simplified algorithm.

Packets bind both consuming-project `src` and shared xport `src` identities. Shared runtime edits and machine mappings use `@xport/src/...`; the supervisor applies project and shared changes in one rollback transaction. Reject shared edits without the packet binding or after either bound source identity changes.

Before reusing saved drafts without ledger bindings, run `X source_index --rebuild --no-format`. The derived index admits only a unique native body whose exact original address symbol belongs to one image and has no conflicting explicit marker. Ledger bindings take precedence. Discovery changes neither ledger status nor accepted coverage; audit the existing body through the normal packet contract. Rebuilding refreshes or removes these derived bindings after source changes.

To bind shared sources for an older active packet, run `X coverage next NAME --reissue`. Reissue preserves the previous packet bytes, records its SHA-256 and replacement path in `packet_revisions`, and creates `coverage-batch-NNNN-revision-RRRR.json`. It rejects changed original function, MIPS, pseudocode, checklist hashes or batch order. Reaudit changed C before proposing proof against the replacement packet; previous draft claims do not become accepted proofs.

To correct a packet derived by the historical parser that read at most two instruction words per IDA line, run `X coverage next NAME --refresh-machine-audit`. This migration requires zero accepted coverage proofs, identical complete listing payloads, unchanged original/pseudocode bindings and batch order, an exact match to the legacy checklist derivation, and complete current words whose SHA-256 matches the original function. It preserves the previous packet bytes and records the corrected instruction counts and added/changed check IDs in `machine_audit_refresh`. It rejects arbitrary checklist changes and cannot be combined with `--refresh-evidence`. Reaudit the corrected checks and source mappings; no draft proof is accepted by migration.

For a specific unproved effect of a direct reviewed SDK callee, supply `X coverage next NAME --sdk-dependencies PROJECT_STATUS_JSON`. The JSON has exactly `schema: 1` and a nonempty `dependencies` array. Each row has exactly `image`, integer `caller_address`, integer `address` and nonempty `effect`. The caller must be in the current 16-function packet or an earlier dependency row; the callee must have reviewed SKIP classification whose JSON evidence binds its image/address/function SHA-256 and alias. Full original MIPS, a direct call/tail edge, complete available pseudocode and the callee checklist are mandatory. Missing pseudocode remains valid. Reject duplicates and incomplete or foreign original ranges. Preserve the old packet and bind the resulting `sdk_dependencies` into the new failure signature. Dependencies do not change batch size or the recorded denominator. Ordinary reissue rebuilds these contexts from the stored requests. This packet extension supplies audit evidence; source edits outside required game functions still require the existing supervisor authorization path.

A packet with `sdk_dependencies` also requires a `sdk_audits` proposal array covering each dependency exactly once. Each row binds the normal function/MIPS/pseudocode/machine hashes, complete audit and baseline flags, empty unresolved semantics and ordered machine coverage. It additionally names `source_file` and `source_symbol` for its canonical `@xport/src/...` implementation. SDK machine mappings must lie in that implementation or an unambiguously reachable shared helper. Missing, duplicate, foreign, incomplete or unrelated SDK audits reject the complete batch. Apply/prove receipts retain `sdk_audits`; SDK proofs do not enter the recorded coverage count.

To authorize an edit inside an existing shared SDK C function, its complete SDK audit also supplies `function_before` and `source_before_sha256` (SHA-256 of that exact UTF-8 string). The supervisor requires one named function, an exact complete current body, a packet-bound shared source baseline and the complete coverage audit validator. The normal `edits` anchor must lie inside this body. It applies game and shared replacements together and restores both trees and the previous receipt if audit/build/finalization fails. This authorization does not permit header/declaration changes, source creation or edits outside the named SDK function. It does not use lockstep diagnostics.

For a reviewed re-export that changes listing annotations or pseudocode, preserve the old accepted root and select the new root through `paths.ida_exports`. Back up the existing analysis database and project configuration, then rebuild the derived database without changing C/H bytes:

```text
X build_database --no-format
X coverage next NAME --refresh-evidence status/ida/accepted/LABEL.json
```

`--refresh-evidence` is a separate migration from ordinary `--reissue`. It requires an active packet, zero accepted proofs, a project-owned IDA acceptance receipt matching the configured export root, its unchanged config with byte-validated nonzero GP, and unchanged accepted metadata and packet artifacts. It permits changed MIPS listing/pseudocode hashes only when image/address order, original function hashes and raw machine-check hashes remain identical. It writes a new packet revision and records the acceptance receipt path/hash, selected destination and `accepted_proofs_reused: false`; the previous packet remains immutable. Reaudit the complete replacement packet. Reject a migration with accepted proofs, changed original bytes/checks/order, stale acceptance hashes or evidence outside the accepted image. Never rewrite a packet or reuse draft proof claims from its predecessor.

The response is one JSON proposal for the complete batch:

```json
{
  "schema": 1,
  "edits": [],
  "function_audits": [],
  "summaries": [
    {
      "image": "MAIN.EXE",
      "address": 2147549184,
      "discrepancies": ["Exact short description"],
      "note": "Complete MIPS audit; compatible runtime evidence remains unverified"
    }
  ]
}
```

Return one `function_audits` row for every function in the packet. Bind every supplied hash, set `complete_mips_audit: true`, set `pseudocode_used_as_baseline` exactly to the supplied `pseudocode_complete`, set `unresolved_semantics: []`, and cover every machine check exactly once in order with groups of at most eight checks mapped to nonblank C lines. Do not edit project files or the ledger directly. Write the complete proposal atomically to the path reported by `next`.

## Exact V8 batch13 addressable frame scopes

The existing `user_authorized_guest_frame_delegation` route additionally accepts only `SLUS_005.10:8003E2FC` and `SLUS_005.10:8003E520` through `coverage_v8_batch13_frames.py`. The validator binds the exact human reply, immutable review and complete candidate bodies by hash. Frame sizes are 128 and 56 bytes. Only original frame identity, aliases, lifetime and saved ABI/SP/RA observations are excluded. Actual local memory effects, call arguments, arithmetic, branches, GTE operations and returns retain the complete strict audit requirement.

Supply the exact `guest_frame_review` and `guest_frame_authorization` references, the reviewed unresolved/waived effects and ABI-only check IDs, `original_equivalence: false`, `implementation_status: WIP`, `retained_semantics_complete: true` and empty `retained_unresolved_semantics`. Map every original check in order. ABI-only checks map to the corresponding existing frame service failure site. Reject changed owners, original hashes, candidate bodies, frame sizes or excluded effects. Existing source transaction, receipt/state retention and final build gates remain mandatory. This route does not authorize another function or promote an owner to DONE.

## Exact V8 batch12 human-authorized scopes

The V8 strict batch12 route is limited to the eight main-image owners in `coverage_v8_batch12_scopes.py` and the exact hash-bound review and human authorization retained under the consuming project's `status/function-coverage/first`. It does not authorize other projects, functions, SDK replacements or additional gaps.

Supply `replacement_kind: user_authorized_v8_batch12_scope`, exact `game_scope_review` and `game_scope_authorization` references, `original_equivalence: false`, `implementation_status: WIP`, `retained_semantics_complete: true` and empty `retained_unresolved_semantics`. Both unresolved and waived effect arrays must exactly match the approved owner. `waived_machine_checks` is the bounded original-site inventory; all original machine checks still require ordered C mappings. Receipt and state retain the scoped proof basis separately from full equivalence.

Predicted source must equal the reviewed whole candidate with two exact retained-algorithm corrections. In 2263C, test the incoming `bytes` carrier initially and read frame+56 at the loop tail, matching original 2266C/22744/2274C. In F9BC, reload sine from frame+192 before correction arithmetic, matching original 301BC. Preserve the immutable approved review; these corrections add no excluded effect.

`game_sp_ra_prototype_updates` permits exactly the existing F998 declaration in `src/game_native_delivery_2.c` to add scalar `incoming_sp` and `incoming_ra` after `object`. Require complete scoped F998/E2BC audits, the original E5B4 direct call and E5B8 argument move, exactly two current E2BC calls and one existing declaration. Reject additional callers, shared references, aliases, directive references, split identifiers and token pasting. Bodies and this declaration use the existing rollback transaction and complete-batch gate.

## Reviewed indirect game dependencies

Run `X coverage indirect-dependencies NAME --review PROJECT_STATUS_JSON` to admit a closed original target set before translating a computed game transfer. The review uses `schema: 1`, `contract: "xport-coverage-indirect-dependencies-v1"`, `coverage_name`, the active `packet` path/SHA-256, `owner`, `closed_target_set` path/SHA-256 and nonempty `sites`. The owner binds image, integer address, all four packet audit hashes and an IDA acceptance path/SHA-256. Each site binds integer `site`, its exact machine `check` and nonempty unique `targets`; each target binds image, integer address, function SHA-256, symbol and acceptance path/SHA-256.

The closed-set file uses `contract: "xport-indirect-target-review-v1"`, the owner's image/address, `reviewed_complete: true`, nonempty hash-bound original `evidence` and identical `sites`. Review the complete table/index/loader semantics before asserting closure. Reject guesses, changed original bytes, foreign images, SDK targets, stale acceptance and duplicate owner admissions. A site must be an original `JALR` call with a nonzero link register or a `JR` control transfer through a nonzero register other than `$ra`; validate the database word, machine-check kind and reserved encoding bits. Ordinary `JR $ra` returns cannot admit game dependencies.

The supervisor retains rollback evidence and generates link-only stubs for accepted original game targets absent from the immutable recording. Recorded targets and existing implementations retain their normal status. This admission does not change the recorded denominator or add proofs. Reissue the active packet after generated source changes; include every reviewed site for one owner in the same admission.

## Optional consuming-project permissions

Read `coverage_permissions` only from the selected consuming project's `xport-project.json`. Omitted keys default to `false`; reject unknown keys and non-boolean values. Never infer permission from a sibling project or shared toolset configuration.

- `allow_unverified_translation`: Authorizes a separate translation-only supervisor path without function comparisons, tests or semantic proof admission
- `allow_missing_bios_stubs`: Authorizes explicit fail-fast replacements for unavailable BIOS calls
- `allow_missing_sdk_stubs`: Authorizes explicit fail-fast replacements for unavailable SDK calls

Permission does not claim implementation or proof. Keep unverified translations out of the accepted-proof counter. Missing-call stubs still require exact identity, prototype and replacement scope; never invent successful behavior. Keep the final requested Release build as a structural deliverable.

For an authorized consuming project, use `X coverage next NAME --unverified` and `X coverage translate NAME --proposal PROJECT_STATUS_JSON`. The proposal has `schema: 1`, `mode: "unverified_translation"` and the complete packet in `functions`, with exact `image`, `address`, `symbol` and C `text` per row. Supply declarations and caller/prototype edits required by those translations. The supervisor writes a separate translation receipt and counts `unverified_translated`; it admits zero semantic proofs. Reissue stale source bindings with `X coverage next NAME --unverified --reissue`. The existing `coverage apply` proof contract remains unchanged; never fabricate audit fields.
## Explicit native C ABI frame permission

An explicit user instruction may authorize native C ABI handling of service-only guest frame saves for individually reviewed owners. Bind the instruction in a project-owned JSON policy with `contract: "xport-user-authorized-native-abi-frames-v1"`, exact project identity, full-original frame review and each owner's four original audit hashes. Restrict the permission to decoded SP adjustment and callee-saved register save/restore checks. Preserve stack arguments, addressable locals and algorithm-carried values; reject other frame uses or missing retained-semantic reviews.

Keep these admissions separate from original-equivalence proofs. Require `original_equivalence: false`, `WIP`, complete retained original audits and explicit frame-check inventories. Ordinary game memory, calls, branches, SDK and device effects retain their normal requirements. A policy is authorization only; it does not itself admit a function or increase coverage.

The policy has exactly `schema: 1`, `contract`, `project`, `user_instruction`, `review`, `owners`, `original_equivalence: false` and `implementation_status: "WIP"`. `project` binds resolved project `path` and `xport_project_sha256`; `review` binds a project-owned status path/SHA-256. Each owner contains the six normal identity/hash fields plus exact ordered `frame_checks` copied from the packet. The supported instruction is `Р”Р°, РѕР±С‹С‡РЅС‹Р№ C ABI СЃ СЏРІРЅС‹Рј РґРѕРїСѓСЃРєРѕРј`; never supply it without the user's authorization.

Each owner audit uses `replacement_kind: "user_authorized_native_abi_frame"`, `native_abi_authorization` path/SHA-256, `waived_machine_checks`, `native_body_sha256` of the LF-normalized complete native body, `source_file`, `source_symbol`, `native_register_bindings`, `retained_semantics_complete: true`, `retained_unresolved_semantics: []`, `original_equivalence: false` and `implementation_status: "WIP"`. Keep explicit nonempty `unresolved_semantics` equal to `waived_original_effects` for the permitted guest frame memory gap only. Register bindings name every saved S0вЂ“S7/FP register in numeric order with `register`, `native_representation`, `source_file`, `source_symbol`, `source_line`, `source_anchor` and `explanation`; RA uses ordinary C return handling. Preserve the exact original marker. Map every retained machine check once in order; omit only the exact authorized frame checks from normal mappings.

`native_representation` is a native identifier or an exact unsigned integer literal present as a complete token in its source anchor. A folded constant still requires an explanation of its complete original definition and uses; do not introduce artificial register variables solely to satisfy the inventory.

The supervisor retains `native_abi_frame_admissions` and `native_abi_frame_count` in receipts, and `native_abi_frames` with the count in coverage state. Revalidate persisted policy, original and native source bindings on resume. Accepted scoped owners remain ledger `WIP`, with `proof_basis: "user_authorized_native_abi_frame_scoped_audit"` and explicit non-equivalence. All normal complete-batch, source-index, rollback and final build gates remain mandatory.

## Explicit missing system and SDK replacement policy

The default original audit validator remains unchanged. An explicit user instruction permitting missing system/SDK stubs may authorize an unconditional fail-fast replacement without claiming original equivalence. Existing canonical implementations and ordinary game functions cannot use this exception.

Supply `missing_stub_authorization` in a new-translation proposal with exactly `path` and `sha256`. The path must identify a project-owned JSON file under `status`; SHA-256 binds its exact bytes. The document has exactly `schema: 1`, `contract: "xport-user-authorized-missing-stubs-v1"`, `user_instruction: "СЂР°Р·СЂРµС€Р°СЋ Р·Р°РіР»СѓС€РєРё РґР»СЏ СЃРёСЃС‚РµРјРЅС‹С… Рё SDK С„СѓРЅРєС†РёР№ РєРѕС‚РѕСЂС‹Рµ РѕС‚СЃСѓС‚СЃС‚РІСѓСЋС‚ РІ xport toolset"` and nonempty `entries`. This reference, rather than a bare config boolean, activates the explicit waiver contract.

Each `sdk` or `system_function` entry has exactly `kind`, `symbol`, `prototype`, `source_file`, `image`, integer `address` and `function_sha256`. `prototype` is the complete C prototype with named plain parameters and a final semicolon. SDK identity matches a reviewed packet dependency and canonical alias; source is an existing packet-bound compiled `@xport/src/*.c` file. A recorded `system_function` must be a checked short syscall or BIOS thunk from immutable MIPS. A game function containing helper calls is not a system boundary.

Each `system_bridge` entry instead has exactly `kind`, `symbol`, `prototype`, `source_file`, `caller_image`, integer `caller_address` and `caller_function_sha256`. Its exact required game caller references the service in the submitted translation. Submit bridges in `system_translations` with rows containing exactly `symbol`, `path` and `text`. Bridges use the same compiled shared-source transaction as `sdk_translations`. A missing guest callback bridge does not authorize replacing an existing host callback API.

An existing compiled bridge may be retained through the same policy and `system_translations` row. Its exact current body and submitted `text` must match after trimming outer whitespace, and both must pass the authorized prototype and unconditional-abort validation. Retention changes no source bytes and still records explicit non-equivalence. Reject existing real behavior, a different source, duplicate symbols, stale shared identity or any attempted body replacement.

Stub audit rows retain normal original/MIPS/pseudocode/checklist bindings and complete-read flags. They additionally require `replacement_kind: "user_authorized_missing_stub"`, `original_equivalence: false`, nonempty `waived_original_effects` and nonempty `unresolved_semantics`. Ordered machine mappings document waived effects at the abort boundary; they do not establish original equivalence. Each body preserves the exact authorized prototype and contains only optional `(void)parameter;` suppressions followed by unconditional `abort();`. Recorded-system bodies additionally retain their exact normal marker. Reject returns, synthetic data, conditional aborts, unrelated helpers and existing API bodies.

Only `coverage apply` admits these replacements. The rollback transaction retains authorization path/hash, complete audit rows, `missing_stub_admissions` and `missing_stub_count`. Coverage state retains separate `missing_stubs` and count. An accepted explicit scoped audit contributes to `X` and separately reports no original equivalence; this is not an unverified implementation proof. Recorded replacement ledger rows remain `WIP`, never `DONE`. Source hashes, scope, registration and rollback remain mandatory.

## Explicit absent-device replacements

An explicit user instruction to omit memory cards, multitap and MDEC video permits device-scoped replacements with absent-device behavior. It does not authorize successful card operations, removal of ordinary direct controllers or dummy game logic. Keep this exception separate from missing BIOS/SDK abort stubs and normal original-equivalence audits.

Supply `absent_device_authorization` with exactly `path` and `sha256`, referencing an exact UTF-8 project-owned JSON file under `status`. The document has exactly `schema: 1`, `contract: "xport-user-authorized-absent-devices-v1"`, `user_instruction: "РёРіРЅРѕСЂРёСЂСѓР№ РєР°СЂС‚С‹ РїР°РјСЏС‚Рё, pad tap, mdec РІРёРґРµРѕ, РёСЃРїРѕР»СЊР·СѓР№ РґР»СЏ РЅРёС… Р·Р°РіР»СѓС€РєРё, Р±СѓРґС‚Рѕ РєР°СЂС‚С‹ РЅРµС‚, tap РЅРµС‚, РІРёРґРµРѕ С‚РѕР¶Рµ РЅРµС‚"` and nonempty `entries`. Each entry has exactly `device`, `image`, integer `address`, `function_sha256`, `symbol`, `prototype`, `source_file`, `scope_evidence` and `replacement_sha256`. Supported device names are `memory_cards`, `multitap` and `mdec_video`; image/address identities must be unique. Bind the replacement hash to the exact C text after trimming outer whitespace.

The no-op admission path supports recorded game wrappers with a void prototype whose complete original evidence establishes device-only scope. Their bodies contain the exact original marker and one `(void)parameter;` suppression for each named parameter, in declaration order. Reject calls, writes, returns, aborts, additional functions and changed ABIs. Device query return codes and shared SDK replacements remain unsupported by this path.

For a mixed game/device function, an entry may additionally contain `omission` with exactly `baseline_text`, `baseline_sha256`, `before`, `after` and `original_range`. Bind the complete prior C draft by exact UTF-8 hash; `before` identifies one unique block strictly inside that function body. `after` contains only the corresponding comment: `/* Memory cards are absent by user instruction */`, `/* Multitap is absent by user instruction */` or `/* MDEC video is absent by user instruction */`. The exact resulting function must equal the baseline with that one substitution and preserve its original marker and ABI. `original_range` is an aligned, nonempty half-open pair of original instruction addresses. The audit supplies `waived_machine_checks` naming exactly every checklist entry in that range, in order, `retained_semantics_complete: true` and `retained_unresolved_semantics: []`. Normal ordered source mappings still cover the entire checklist. Reject changed retained code, synthetic returns, wrong omitted check sets and unresolved retained game semantics. A preserved C draft is a scope binding, not proof: audit all retained behavior against original MIPS before admission.

Audit rows retain all normal original/MIPS/pseudocode/checklist hashes, complete-read flags and ordered machine mappings. Set `replacement_kind: "user_authorized_absent_device"`, the exact `device`, `original_equivalence: false`, and nonempty `waived_original_effects` and `unresolved_semantics`. These mappings document expressly omitted effects; they do not prove original equivalence. All other functions still require the normal complete semantic audit.

Only `coverage apply` admits replacements in the normal rollback transaction. The receipt retains the authorization reference, `absent_device_admissions` and `absent_device_count`; coverage state retains a separate `absent_devices` inventory. An accepted scoped audit contributes to `X` with explicit non-equivalence, while its ledger entry remains `WIP`, never `DONE`. Admission does not change the recorded denominator, permit partial batches or bypass source identity, registration, Debug and final Release gates. Rejected or merely drafted replacements contribute zero accepted proofs.

## Reviewed image-qualified migration of recorded zero-fill entries

A packed address recording retains its original `analysis.default_image`, `functions.bin`, receipt and first-call order. When reviewed coldboot loader evidence and a separate runtime observation prove that recorded zero-fill entries belong to accepted overlay functions, use `X coverage migrate-images NAME --request status/function-coverage/NAME/image-migration-request.json`. This command does not rerecord or rewrite the original capture and does not claim that the separate observation replays the original session.

The request has `schema: 1`, `contract: "xport-coverage-image-migration-v1"`, hash-bound `state`, `functions`, `recording_receipt`, `coldboot_review`, `observation`, `old_acceptance`, a complete hash-bound `packets` list, and `mappings`. Each mapping names integer `address`, accepted `image`, exact `function_sha256` and hash-bound `acceptance` receipt. Bound inputs use `path` and `sha256`. Acceptance receipts remain under `status/ida/accepted`; all run/review evidence remains project-owned. The coldboot review explicitly binds the no-save-state user fact and supported intended image/address mapping.

Migration requires zero accepted proofs and translations, exactly the existing recorded order and denominator, complete original packet preservation, accepted source/config/artifact hashes and GP validation, and mappings for exactly every recorded function whose complete accepted original body is zero fill. Reject nonzero original replacement, duplicate/foreign/missing mappings, stale inputs, changed capture/receipt/order, ambiguous observed entries and an active packet already containing a migrated entry. Every real target body must match its complete accepted original bytes and the complete corresponding prefix of a hashed runtime snapshot. The snapshot may include unrelated trailing bytes because an older zero-fill boundary was invalid; those bytes do not enter the real target body or proof.

The supervisor preserves prestate and packets in a new `image-migration-preserved-*` directory, writes a hash-bound migration receipt and changes only the affected rows to explicit image-qualified identities. A failed state write restores prestate and removes the new receipt. Every later load validates override identities against that receipt and rechecks preserved capture/receipt/packet hashes. Run `X coverage next NAME --reissue` after the migration to bind current sources; the active main-image packet retains its exact original function bindings. Keep old packet bytes immutable.

Image-qualified rows use their accepted image for context, original hash, mapped C, ledger identity and proof matching. Direct overlay dependencies resolve only to exact nonzero accepted function entries, prioritizing a valid entry in the source image or an explicitly migrated target. Missing or ambiguous original target metadata is retained as `dependency_pending`; do not invent boundaries, drop edges, produce guessed stubs or expand the recorded audit denominator. Supply broader reviewed original exports and run `X coverage start NAME` to refresh this inventory and create exact image-qualified abort stubs for accepted unrecorded targets. Existing stubs are retained. Pending metadata blocks apply/prove for its owner packet and prevents all final Release/completion paths, while unrelated packets may proceed. `X coverage status NAME` exposes the pending inventory.

## Link-only TODO stubs

For semantically incomplete pseudocode preserved in an accepted export, set `analysis.pseudocode_reviews` to a project-owned `status` JSON file with exactly `schema: 1` and `reviews`. Each review contains exactly `image`, integer `address`, `function_sha256`, `pseudocode_sha256` and a nonempty evidence-based `reason`. Reviews only downgrade completeness; stale hashes, duplicate identities and foreign paths fail closed. Context keeps the original text and hash, sets `pseudocode.complete: false`, retains full-text availability in `content_complete`, and binds the review file hash. Audit the complete MIPS and set `pseudocode_used_as_baseline: false` for that function. For an active packet with zero accepted proofs, use `coverage next NAME --refresh-evidence status/ida/accepted/LABEL.json` with the existing receipt matching the unchanged accepted export root. The supervisor preserves the prior packet and records the review hash in the refresh receipt. No original export files are edited.

The supervisor, not the model, owns link-only stubs. During `coverage start`, it writes or updates `src/game_link_stubs.c`, registers that source in the native project and creates a `status: "TODO"`, `link_only_stub: true` ledger entry for every required unrecorded direct target. Each generated body contains the exact image/address `FUNCTION_MARKER` and calls `abort()`. It exists only to satisfy the native linker and to stop immediately if the supposedly unreachable path executes.

Link-only stubs obey these invariants:

- Never add them to the coverage state function list or the `X/Y/Z` denominator
- Never submit them to the model, audit their original bodies or follow their callees merely to make a build link
- Never promote them to WIP, DONE or SKIP and never count their marker as a C translation
- Preserve their original image/address/SHA identity and `TODO` ledger status
- Use the exact symbols supplied in the packet's `link_only_stubs` list when a translated recorded function calls those targets
- Permit Debug and Release builds with remaining link-only stubs; this represents the explicitly recorded functional slice
- If execution later records a stub address in another `functions.bin`, remove its generated stub before creating the packet, treat the function as missing C, translate its complete pseudocode/MIPS body through the normal supervisor transaction, clear `link_only_stub` and move the ledger entry to WIP only after accepted proof

Do not write a guessed return, success value, empty body or partial implementation. The only permitted unrecorded-function stub is the supervisor-generated marker plus unconditional `abort()` form. A direct target with an existing accepted WIP/DONE implementation or reviewed PsyQ/SKIP classification needs no stub.

## New translations without a mapped C implementation

For an active packet containing `implementation.mapping_status: "unmapped"`, submit new bodies through `coverage apply`. Do not create or append game source files directly. `tools/coverage_translation.py` owns allocation, native-project registration, audit validation and rollback; the existing `lockstep_repair_proposal.py` path remains restricted to substitutions inside required mapped functions.

Use this proposal shape:

```json
{
  "schema": 1,
  "edits": [],
  "declarations": "uint32 sub_80010000(void);",
  "translations": [
    {
      "image": "IMAGE_FROM_PACKET",
      "address": 2147549184,
      "symbol": "sub_80010000",
      "text": "COMPLETE_AUDITED_C_FUNCTION_WITH_EXACT_FUNCTION_MARKER"
    }
  ],
  "function_audits": [],
  "summaries": []
}
```

The example is a schema illustration, not an admissible proof. Replace the illustrative image, address, symbol and body with packet-bound values. Supply every required `function_audits` row and summary using the normal complete-batch contract above.

Inputs and invariants:

- `translations` contains exactly one row for every unmapped function in the active packet and no other rows. Each row has exactly `image`, `address`, `symbol` and `text`; `address` is a JSON integer
- Each `text` contains one complete C function whose parsed symbol equals `symbol`, with exactly one literal `FUNCTION_MARKER(0xADDRESSu, "IMAGE")` binding to that row's image and address
- `declarations` is an optional string for prototypes and required non-function declarations. It must not contain function bodies. Keep support code within the audited translations; do not use declarations to introduce an unaudited helper
- `edits` contains no consuming-project substitutions. Already mapped game functions in a mixed packet remain exact and receive complete audits. It may contain shared SDK body substitutions only under the packet-bound transaction contract below
- The consuming-project and shared source identities must still match the active packet. Preserve every original function, MIPS, pseudocode and machine-check binding
- Every machine check maps to the resulting C source using the existing ordered, complete machine-coverage contract. New bodies do not waive semantic audit or Debug validation

## Exact retained effects in the existing V8 batch10 frame scope

The four-owner frame authorization remains immutable. For `SLUS_005.10:80043C34`, the validator corrects only `result = CdSyncCallbackPSX(0u);` to `result = CdReadyCallbackPSX(0u);` in its expected body. Require the complete original context and exact retained checklist word `80043CA8:0C0123EF` (JAL80048FBC). This call cannot enter the waived checks. Reject the old incorrect callback, changed arguments and every other body change.

For the exact approved `80016364` body, the original numerical fixture may retain its limited evidence when shared GTE changes add only disjoint branches. Supply a separate hash-bound `backend_compatibility_review` using `xport-16364-disjoint-gte-extension-review-v1`, exact owner, commands `00280030`/`00180001`, data registers0..5 and complete baseline/result bodies for `xport_gte_execute` and `xport_gte_write_data`. Baseline hashes must equal the immutable numerical review. Current complete bodies must equal the reviewed results. Removing only added command14/28 and register30/31 branches must reproduce each exact baseline; all other numerical closure bodies remain exact. Reject common decoder changes, covered branch changes, other changed helpers, stale bodies or broader scopes. This retains existing proof of unchanged covered branches; it proves no added command and grants no additional original-effect waiver.

Verify with `test_coverage_batch10_frame_delegation.py`, `test_coverage_16364_callback_delegation.py`, `test_coverage_disjoint_gte_extension.py` and the complete `test_coverage*.py` suite.

## Shared SDK additions with mapped game owners

For an already mapped complete packet, `coverage apply` accepts the same `sdk_translations`, `system_translations` and `sdk_header_edits` contracts alongside ordinary required-owner `edits`. Omit `translations`: existing game bodies remain in their owning modules. Never delete a mapped body to enter the new-game allocation path.

The supervisor derives complete predicted caller bodies from exact required-owner substitutions and their original markers. This internal SDK caller view admits no game translations or proofs. It invokes the existing packet-bound SDK identity, complete-audit, prototype, missing-service scope, compiled registration and predicted-source validators. Unsupported SDK helpers, declarations, stale identities and partial packets remain rejected.

Apply game edits, SDK additions and header prototypes in one rollback transaction. Preserve game/shared before identities, exact changed files, SDK audits and explicit scoped admission inventories in the resulting receipt. Audit or finalization failure restores all source and prior receipt bytes; normal coverage finalization restores ledger/state bytes. Complete machine mappings, ordinary strict audit gates and final Debug/Release gates remain mandatory.

Verify this route and its source/receipt rollback before use:

```text
python -B -m unittest discover -s [XPORT_ROOT]/tools/tests -p "test_coverage_mapped_sdk.py"
python -B -m unittest discover -s [XPORT_ROOT]/tools/tests -p "test_lockstep_repair_proposal.py"
python -B -m unittest discover -s [XPORT_ROOT]/tools/tests -p "test_coverage*.py"
```

## Shared SDK additions with new game translations

For an unmapped function whose old game declaration conflicts with its fully audited native ABI, a new-translation proposal may include `game_prototype_updates`. Each row contains exactly `path`, `before` and `after`. The source is an existing packet-bound project `src/*.c` or `src/*.h`; the unchanged packet source identity is mandatory. Both strings are complete plain prototypes for the same symbol, `before` is a unique exact source anchor outside every existing function body, and `after` matches the admitted translation signature exactly after whitespace normalization. Newly proposed declarations for that symbol must use the same ABI. Reject foreign symbols, mapped functions, macros, body edits, duplicate updates, nonunique anchors and stale source.

The supervisor reconciles admitted declarations before calculating the complete batch allocation, including an old active module when new bodies move to the next module. `prepare_game_prototype_updates` supplies predicted bytes for other bound modules or headers. The normal apply transaction backs up and restores every updated game file together with new translations, shared changes, registration and receipt. Changed-file/source-index reporting includes reconciled declaration files; receipts retain `game_prototype_updates`. Caller function bodies remain byte-for-byte unchanged.

A `game_prototype_updates` row may additionally specify `occurrence_count: 2` to reconcile exactly two identical existing declarations for its one required unmapped function. Omission still requires one unique anchor. Reject any other count, count mismatch, comment/string/macro match, function-local declaration or function-body change. Validate both anchors independently before replacing them together; the same packet identities, exact translated ABI, transaction and rollback requirements apply. This narrowly bounded reconciliation does not permit bulk replacements or edits to caller bodies.

A new-translation proposal may correct a previously unresolved public runtime address through `runtime_constant_updates`. Each row has exactly `path`, `before`, `after`, `image`, integer `address`, `function_sha256`, `symbol` and integer `value`. The original required unmapped getter must contain exactly four instruction words: `LUI v0`, `LW v0, signed-offset(v0)`, `JR ra`, `NOP`. Decode `value` from those immutable words; the admitted translation must contain only its exact marker and `return r_u32(value);`. The target must already be a global `const uint32` literal declaration under packet-bound project `src`, and the same symbol must already have `extern const uint32` declaration in packet-bound shared headers. Only its literal initializer changes. Reject other getter algorithms, foreign hooks, incorrect hashes/values, macro/comment/function-local anchors, qualifier changes and duplicate updates.

`prepare_game_updates` combines prototype and runtime-constant predictions before module allocation; `prepare_runtime_constant_updates` also accepts already predicted prototype bytes. The normal supervisor transaction backs up every changed game file, reports the complete changed-file set and retains `runtime_constant_updates` in the audit receipt. Game getters remain normally audited; this narrow address-binding correction introduces no SDK equivalence waiver and never places game-specific addresses in shared source. Declaration changes require `coverage apply`, not `coverage prove`.

An explicitly authorized existing SDK abort stub may be retained without a `sdk_translations` addition. Its `sdk_audits` row uses `replacement_kind: "user_authorized_missing_stub"`, `original_equivalence: false`, nonempty `waived_original_effects` and `unresolved_semantics`, and an exact project-owned `missing_stub_authorization` policy reference. The supervisor checks the packet dependency, canonical alias, compiled packet-bound source and exact policy prototype. The current body must contain only optional parameter suppressions followed by unconditional `abort()`. Do not supply `function_before` or edits for this retained stub. Reject an existing real implementation, guessed return or changed shared identity. Retain the explicit non-equivalence in the receipt; this route does not authorize replacing existing SDK behavior with a stub.

The new-translation supervisor can admit `sdk_translations` and existing shared SDK body substitutions in the same rollback transaction. This does not authorize placeholders or prove runtime equivalence. Every packet `sdk_dependencies` row still requires a complete `sdk_audits` row, exact original hashes, empty unresolved semantics and ordered machine mappings to its canonical implementation.

### Explicitly authorized game guest-frame boundary

A separate direct user instruction may authorize one packet-bound recorded game owner to retain a complete audit of its game algorithm while excluding unavailable guest-frame identity, lifetime and original-stack alias effects. Permission to create missing SDK/system stubs alone does not authorize this route. Bind the exact additional user instruction, owner image/address and all four original hashes to a project-owned status authorization reference. Do not reuse it for another function.

Keep the ordinary empty-unresolved game audit rule unchanged. The separately identified admission requires literal `original_equivalence: false`, explicit nonempty stack gaps, `retained_semantics_complete: true`, empty retained-algorithm gaps and `WIP`. Require exactly paired absent fail-fast acquire/release services with exact caller-policy bindings and compiled prototypes. Only decoded original stack allocation/deallocation and ABI save/restore checks may be waived; normal matrix loads/stores, branches and game calls remain fully audited and mapped in order.

Require a hash-bound complete baseline and result with only the reviewed frame storage/cleanup substitution. The separate authorization also binds the complete already reviewed baseline body SHA-256 and its full-original review path/hash; resealing a changed baseline is rejected. The acquired guest range and SDK host view must name the same bounded storage. Acquisition must precede every frame use; every initialized exit must pass exactly once through cleanup with the original result preserved. Reject fixed-SP guesses, heap substitutions, fabricated inverse pointer mappings, dead calls, macro or cleanup bypasses and unrelated algorithm edits. Retain exact authorization, review and explicit gaps in a separate receipt/status inventory. An accepted scoped admission may advance recorded coverage under that explicit permission; it does not prove original or runtime equivalence and cannot promote the owner to `DONE`. All remaining coverage and final Debug/Release gates still apply.

### Exact canonical trigonometry helper correction

An SDK owner audit may supply `sdk_helper_corrections: [{"path": "status/REVIEW.json", "sha256": "SHA256"}]` for the canonical `rsincos` quarter-table mirror endpoints only. Reject more than one review, foreign status paths, changed review hashes, stale packet/failure-signature bindings, missing complete canonical `RotMatrixYXZ` SDK owner audits and changed original owner bytes. The review binds the immutable PS-X executable, payload metadata, original packed table, three original SDK lookup instruction chains, unchanged 1025-entry quarter table and exact complete helper before/after bodies.

The only admitted change replaces both `0x7ffu - angle` endpoints with `0x800u - angle` and both `0xfffu - angle` endpoints with `0x1000u - angle`. Require the canonical helper skeleton and recompute equality to the original packed table for all 65536 signed halfword angles. Preserve the API, output store order and quarter table. Require three direct helper calls from the predicted canonical SDK owner. Bind the executable identity and payload metadata to the unique matching executable in the completed project's passed startup inventory. Require the helper/owner source to be registered by `ClCompile` in the single native project beside the explicitly configured solution. Apply the exact ordinary shared body edit through the same supervisor transaction, retain its review reference in the receipt and revalidate actual resulting bodies during final audit. Reject omitted edits, extra helper changes, changed tables and incomplete corrections. This route neither admits other helpers nor replaces whole-function SDK/game audits or runtime evidence.

### SDK adapters delegating unavailable guest system services

The explicit missing-system user authorization may admit an SDK adapter that retains real operations and delegates only absent guest services. This is a scoped non-equivalence admission, never an ordinary SDK proof or an abort replacement for an existing real SDK function. Use `replacement_kind: "user_authorized_system_delegation"`, literal `original_equivalence: false`, nonempty `unresolved_semantics` and `waived_original_effects`, and nonempty `system_delegations` in its SDK audit. The ordinary empty-unresolved audit rule remains unchanged for other SDK functions.

Each delegation row has exactly `symbol`, `source_file`, `prototype`, `check_ids` and integer `source_line`. Bind a unique `system_bridge` entry in the hashed `missing_stub_authorization` policy. Its recorded caller image/address/function hash must equal the SDK dependency's packet-bound caller. The canonical SDK adapter must directly call that bridge, and its recorded game caller must directly call the adapter. Unsupported indirect or hidden native delegation is rejected. Complete original SDK hashes, pseudocode baseline flags, every normal machine check and the whole-function audit remain mandatory.

The supervisor validates complete predicted shared sources after all additions and replacements. Each bridge must be unique, compiled and contain only optional parameter suppressions followed by unconditional `abort()`. Map each waived original check exactly once to the resulting absolute unconditional-abort line of that exact bridge. A group may not mix waived and nonwaived checks. Reject foreign or duplicate checks, entire-algorithm waivers, missing policy bindings, false equivalence, discarded gaps, comment/string/prototype references, unsupported call forms and a whole SDK body disguised as a bridge. Existing source identities, exact edit scope, compilation registration and rollback gates remain mandatory.

The receipt retains `sdk_system_delegation_admissions`, including exact policy reference, delegation rows, explicit unresolved effects and `WIP` status. Coverage status reports `sdk_system_delegation_count` and `sdk_system_delegations` separately from recorded game progress. Admission neither expands the recorded denominator nor proves runtime equivalence. The final Debug and Release gates remain required.

`sdk_translations` is an optional list. Each row has exactly `image`, integer `address`, `symbol`, `path` and `text`. Its image/address must bind one unique packet SDK dependency. The symbol must match its reviewed classification alias or that alias with a `PSX` collision suffix. Text contains exactly one complete new function and no outside declarations or helpers. A marker is optional; when present it must be the exact dependency image/address marker. Its SDK audit names the same canonical source path and symbol and has no `function_before`.

The path must name an existing packet-bound `@xport/src/*.c` source already registered by `ClCompile` in the one unambiguous native `.vcxproj` beside the configured solution. Reject new shared files, unregistered sources, existing or duplicate symbols, escaping paths, foreign dependencies and changed shared identities. The supervisor appends admitted functions to their owning sources; it never changes shared compilation registration.

Existing shared SDK substitutions use normal `edits` rows with exactly `path`, `before` and `after`. Their unique anchors must lie inside the complete current body authorized by `sdk_audits.function_before` and `source_before_sha256`. The resulting body remains exactly one function with the same symbol and no outside declarations or helpers. Reject game substitutions and edits outside authorized bodies.

`sdk_header_edits` optionally contains exact `path`, `before` and `after` rows for existing packet-bound `@xport/src/*.h` files. Each `before` is a unique nonempty current-source anchor. `after` preserves that anchor verbatim and appends only prototypes naming newly admitted SDK symbols, each once. Reject unrelated declarations, macros, helpers, renamed anchors and header edits without an admitted prototype. Non-function binding declarations require a separate reviewed contract and are unsupported here.

For an already mapped owner, ledger finalization selects only the complete implementation path and symbol bound by its immutable packet context, with exactly its one original marker. A differently named native helper carrying the same address does not replace that binding. Source indexing may resolve multiple marker candidates only through the exact ledger semantic name and matching sole marker. Unmapped discovery retains its unique-marker requirement; duplicate symbols, stale or foreign paths, wrong markers and unresolved candidates reject finalization.

The receipt contains every SDK audit, the shared before identity/files, resulting shared identity and exact changed shared file list. Audit mappings use resulting absolute source lines. Audit or finalization failure restores all changed game/shared sources, headers, project registration and the previous receipt; normal finalization restores ledger bytes. Existing-body corrections and additions are applied together before audit validation. Compilation remains a structural gate and never substitutes for a complete semantic audit.

Allocation and outputs:

1. The supervisor appends the entire new-body set and declarations to `module_plan.active_module` and ensures it includes `psx.h` for the game API and function markers
2. If the resulting module would exceed `module_plan.line_limit`, it places the entire set in `module_plan.next_module`, starting with `#include "psx.h"`. It never splits the new bodies between modules
3. A new module requires exactly one `.vcxproj` beside the configured `paths.native_solution`. The supervisor registers its relative `ClCompile` path before the Debug gate; it does not accept arbitrary project-file edits from the proposal
4. The supervisor creates the packet's sibling `.audit.json` receipt against the resulting source identity, validates every required audit and creates or updates the exact image/address ledger entry. It rebuilds the source index for an intermediate batch. On the last proof it runs the complete Debug `code_refresh --no-format` gate
5. Success advances coverage through the normal accepted-batch path. The receipt records the changed C/H paths and `registered_project` when a module was registered

Failure conditions include a missing or duplicate translation, a foreign address/image, mismatched symbol or marker, extra function bodies in declarations, unauthorized simultaneous `edits`, a stale source identity, an existing next-module path, an indivisible batch larger than the module limit, ambiguous native-project selection, duplicate project registration, incomplete audit or failed Debug gate.

On audit or gate failure, restore the previous active-module bytes, native-project bytes and audit receipt; remove only the new module and receipt created by this transaction. The existing coverage finalizer restores ledger bytes on `code_refresh` failure, and the apply entrypoint rebuilds the source index after rollback. Do not treat a rejected proposal as accepted coverage.

Write the complete proposal atomically to the path returned by `coverage next`, then use the same `coverage apply NAME --proposal ...` command below. Translation allocation changes neither the `X/Y/Z` definition nor the final Release terminal condition.

Verification of this toolset path:

```text
python -B -m unittest discover -s [XPORT_ROOT]/tools/tests -p "test_coverage*.py"
```

If source changes are required, run:

```text
X coverage apply NAME --proposal status/function-coverage/NAME/coverage-proposal.json
```

If every current C function is already exact, return `edits: []` and run:

```text
X coverage prove NAME --proposal status/function-coverage/NAME/coverage-proposal.json
```

The supervisor rejects partial batches, stale source, changed original hashes, foreign paths, nonunique edit anchors, incomplete pseudocode claims, missing machine checks and unresolved semantics. It alone applies source substitutions, writes the audit receipt, updates ledger evidence and keeps an unaudited or dynamically unverified implementation at `WIP`. Intermediate packets rebuild the source index without linking because calls to later recorded TODO functions may still be unresolved. The final packet runs one complete Debug `code_refresh --no-format`; compilation and `code_refresh` are structural gates, not semantic proof. It may retain `DONE` only when compatible runtime evidence exists.

New translation code uses `src/game_main.c`, then `src/game_batch1.c`, `src/game_batch2.c`, and so on. A module has a hard 2000 physical-line limit for accepting another batch. Treat each 16-function batch as indivisible: if adding the complete batch would make the active module exceed 2000 lines, put the whole new batch in the next module and register that module in the native project before the Debug gate. Never split one batch across the old and new module. Existing functions remain in their owning modules during corrective audits; do not move them merely to satisfy the allocation rule.

Repeat `next` and `apply` or `prove` until no pending recorded functions remain. The supervisor runs the complete Debug link after the last packet and exactly one Release build after the last proof; link-only TODO stubs make unrecorded direct branches linkable without expanding coverage. An unresolved final linker reference to an analyzed game function means the direct dependency or symbol binding is missing: repair that derived edge/binding and rerun `coverage start NAME` so the supervisor creates the exact abort stub. Do not add the target to `Z` unless it appears in `functions.bin`. An unresolved indirect transfer remains an explicit evidence gap until its target is reviewed; after binding, an unrecorded target receives the same link-only stub. `coverage status NAME` resumes a finished audit that is waiting only for the Release gate. Success requires `status: complete`, `X == Z`, a valid audit receipt for every newly proved recorded function, successful source-index validation for every intermediate packet, one successful final Debug `code_refresh` and one successful final Release build.

The low-level files are resumable state, not permission to skip evidence. Never sort or rewrite `functions.bin`, count excluded PsyQ in `Z`, promote a function from compilation alone, or reuse an audit whose original, pseudocode, machine-check or C source identity changed.

A proved caller may retain an exact previously audited inline dependency disposition through `X coverage inline-dependencies NAME --review status/function-coverage/NAME/inline-constructor-dependency-disposition.json`. The supervisor requires an existing unique unresolved original call edge, a hashed accepted owner audit and review, unchanged original/context/native-function identities, exact call mapping, target bytes and native effect lines. It preserves the old state and writes a separate admission receipt; it changes neither source nor recording membership. Every subsequent state load revalidates these bindings. This is a caller/site/target-specific disposition, not an exemption for other interior SDK entries.

Admit a reviewed original indirect game dependency with `X coverage indirect-dependencies NAME --review status/function-coverage/NAME/indirect-dependency-review.json`. The project-owned review uses `schema: 1`, `contract: xport-coverage-indirect-dependencies-v1`, `coverage_name`, a hash-bound active `packet`, `owner` and `sites`. Bind the owner's exact image/address, function/MIPS/pseudocode/machine-check hashes and accepted export receipt. Each site contains its integer original JALR address, exact machine checklist `check` and a nonempty unique `targets` list. Each target binds selected image, original entry address, `function_sha256`, original `symbol` and accepted receipt. All targets must have nonzero accepted game originals and no SDK classification.

Bind `closed_target_set` to a project-owned status JSON using `contract: xport-indirect-target-review-v1`, exact owner image/address, `reviewed_complete: true`, hash-bound nonempty `evidence` and exactly the same `sites`. This flag records a completed original constructor/writer/table audit; it is not supplied merely from observed callbacks. Reject stale, foreign, ambiguous, duplicate or incomplete evidence. Admission validates accepted profiles, receipts, raw bodies, classification, packet membership, four owner hashes and exact JALR checks. It preserves prior coverage state and writes a hash-bound admission receipt; every later state load revalidates its review and unchanged recorded membership/order.

The supervisor generates unrecorded ordinary game target abort stubs only through `sync_link_stubs`, retaining existing stubs and refreshing the source index without formatting. Recorded future targets remain pending and receive no stub. These targets enter dependency queries without following their callees, expanding capture membership, admitting proofs or granting ordinary game SDK waivers. Failure restores state, stub source, ledger, native project and analysis index. After a changed stub/index admission, reissue the active packet with `X coverage next NAME --reissue` before preparing its proposal.

## Return-only declarations in an active mapped batch

For a complete mapped strict packet, `coverage apply` accepts `game_return_abi_audits` and `game_return_prototype_updates`. Each ABI audit binds the required owner's six identity/hash fields, `old_return: "void"`, `new_return: "uint32"` or `"sint32"`, `complete_mips_audit: true`, the exact `pseudocode_used_as_baseline` flag, `argument_abi_unchanged: true` and a nonempty complete-original `return_review`. Supply one exact complete owner replacement in normal `edits`; retain its symbol, marker and parameter list verbatim. The normal complete owner proof remains mandatory.

Each prototype update has exactly `path`, `before` and `after`. Cover all existing plain declarations of every reviewed owner in packet-bound project source. Change only the return type, retaining the rest of each declaration verbatim. Reject incomplete declaration sets, ambiguous anchors, local declarations, directives, split identifiers, function-pointer escapes, foreign or duplicate owners, stale original hashes, shared references and parameter changes. The supervisor validates updates before passing them into the normal source/receipt/ledger/index/build rollback transaction; receipts retain both ABI reviews and declaration updates. `coverage prove` rejects this source-changing route. An ABI declaration repair does not authorize excluding any original return value or other semantic effect.

## Corrective audits of accepted mapped game bodies

For the existing Vigilante 8 batch containing `800441C8` and `80044574`, a hash-bound `game_v0_carrier_review` may restore the actual implicit V0 input as the final `uint32 incoming_v0` native parameter. This is an original-effect-preserving ABI migration, not a waiver. Both complete mapped owner replacements still require the normal full original hashes, completeness flags, empty unresolved semantics and ordered machine maps. The zero-channel path must return the entire incoming word; reject abort replacement or a fabricated constant.

The supervisor validates every current native call and all four existing declarations against the current source snapshot. Auxiliary caller edits permit only appending a pure scalar carrier, sequencing the existing nested volume call before the original channel load, capturing the second volume return before the unchanged sum and declaring that uninitialized scalar. Retain every unrelated caller byte. Bind original main-image caller words, function/listing hashes and direct edges; bind SkiResrt callsites to the reviewed full DLL hash and raw words. Its summed-volume delay slot must preserve the second return separately from the sum. Reject stale sources, proved caller bodies, architectural-argument changes, foreign paths, missing callers/declarations and added effects. Auxiliary caller ABI reviews add no game proofs and claim no equivalence for the callers' unchanged partial implementations. Apply caller bodies and declarations inside the same source/receipt/ledger/index/build rollback transaction; retain the review reference in the receipt. `coverage prove` rejects this source-changing route.

Use `X coverage_corrective prepare NAME --request STATUS_JSON` when one or two already proved recorded game bodies need a compilation or semantic correction. The request is a project-owned status JSON with `schema: 1`, `functions: [{"image": "IMAGE", "address": INTEGER}]` and optional `prototype_repairs` defined below. Reject other request fields. This command creates an immutable `coverage-corrective-NNNN.json`; it does not change the active coverage packet, denominator, first-call order or accepted count. Complete current mapped C, original MIPS, available complete pseudocode and derived machine checks must fit the evidence budget. Original hashes must match the previous accepted ordinary game audit. Missing-system or absent-device replacement proofs are ineligible for this route.

Return a proposal containing `schema: 1`, `failure_signature`, `edits`, `function_audits`, `summaries` and optional `game_prototype_updates` and `callee_abi_audits` defined below. Reject other fields. Supply exactly one complete current function body replacement per required function; retain its native signature, symbol and marker. Reject partial substitutions, outside declarations in body replacements, foreign bodies, preprocessor directives, helper additions, SDK changes and waiver fields. Reaudit every original block, call, delay slot, memory effect and return; provide all normal original hashes, completeness flags and ordered machine maps using resulting absolute source lines within the required mapped bodies. Supply one normal semantic summary per function. Compilation alone does not prove semantics.

Apply with `X coverage_corrective apply NAME --packet PACKET_JSON --proposal PROPOSAL_JSON`. The supervisor revalidates coverage state bytes, previous proof receipt hashes, original and current body contexts, project/shared identities and exact mapped edit scope. It reuses the normal mapped-function transaction, complete coverage audit validator, ledger finalization and source-index gate while recorded functions remain pending. Once all recorded functions are proved, corrective finalization reruns the full Debug gate and then Release; it publishes complete state only after both succeed. Success replaces only the selected functions' accepted proof receipt links and appends `corrective_audits`; it preserves normal batch history, progress, function order and the active packet pointer. The new receipt retains immutable prior accepted bindings. Old packets and receipts remain unchanged.

Failure restores edited sources, ledger, state and any previous corrective receipt, then refreshes the source index. A retained active packet becomes stale after a successful source correction: explicitly reissue it with `X coverage next NAME --reissue`, rebase its source mappings and regenerate its proposal before applying. Do not hand-edit an accepted C body or its proof/state links.

### Corrective declarations for pending unmapped game callees

An optional `prototype_repairs` request contains at most two rows with exactly `image`, integer `address`, `symbol` and complete named plain `prototype`. The callee must be a pending recorded game function with no mapped body, or an unrecorded supervisor link-only TODO stub. Reject accepted or real mapped callees and reviewed SDK targets. The callee is ABI evidence only: do not implement it, add it to the required owner audits or change recording membership, coverage counts or first-call order.

Prepare binds the complete original callee MIPS, exact contiguous original instruction addresses, original function/listing/pseudocode/checklist hashes, all available pseudocode text, proposed native prototype, every existing declaration and every mapped native caller. A reviewed semantically partial pseudocode payload is valid when `content_complete: true`; require `pseudocode_complete: false` and baseline false. Missing or budget-truncated nonnull artifacts reject preparation. Owner prepare uses the same payload distinction and retains the accepted pseudocode baseline flag.

All target references must be existing plain declarations or direct calls in the exact required mapped corrective owners. Require every current native caller to be included in the one- or two-owner complete audit scope with matching marker, source, symbol and body SHA-256. Require original same-image direct call evidence from complete original words. Reject function-pointer escapes, unknown callers, target tokens in directives or continuations, split target identifiers after escaped-newline removal, aliases and any target references in shared source. Conservatively reject token-pasting operators `##` or `%:%:` anywhere in the project or shared source snapshot when a prototype repair is requested. A link-only target may retain its exact registered supervisor macro invocation and uint32 return ABI; never edit that stub body.

Supply one `callee_abi_audits` row per bound callee, with its six identity/hash fields, exact `native_prototype`, complete MIPS read and pseudocode flags, empty unresolved semantics, ordered `argument_bindings` and `return_binding`. Each argument binds its zero-based index, original a0..a3 register, exact native type, `guest_word` or `host_pointer` representation and nonempty original-PC explanations. The return binds its exact native type, V0 and original-PC explanations. This is an ABI review, not an implementation proof; instruction membership checks do not certify an explanation's semantics.

One additional saved-register carrier is permitted only as the final scalar `uint32 incoming_s1` parameter, bound to original `s1` with `guest_word` representation. It never denotes architectural a2. Its argument row additionally requires `saved_register_callers`, covering exactly every bound mapped caller. Each row contains the caller's six original identity/hash fields, `definition_pc`, `call_pc`, `native_parameter` and a nonempty full-preservation review `explanation`. The packet retains complete caller contexts. The defining word must be an exact ADDU/OR move from a0..a3 to S1; the corresponding native caller parameter must be named scalar uint32. Reject any encoded S1 write through the direct call's delay slot, unsupported scan instructions, foreign or stale caller evidence, and native calls whose final argument differs from that parameter. Callee evidence must include its original S1 stack save and a forwarding JAL before its first S1 definition. Full caller/callee and transitive-carrier review remains mandatory: this bounded scan does not prove that intervening calls preserve S1. All ordinary argument rules and full corrective body audit/transaction gates remain unchanged.

Supply `game_prototype_updates` containing all and only the packet's existing declarations, with exactly `path`, `before`, `after`. Each unique anchor lies outside every function and directive in an existing packet-bound game C/H file. `after` is the exact requested prototype. Reject duplicate, foreign, stale, macro or function-local anchors. Caller signatures and markers remain unchanged; complete caller body edits contain the needed guest-to-native pointer conversions.

The corrective supervisor refreshes all callee, declaration, caller and shared-reference bindings before applying. It passes only validated declaration edits into a narrow trusted transaction callback; a model proposal cannot authorize that callback. The normal owner proof validator, source identities, changed-file checks, ledger/source-index/build gates and complete rollback still apply to bodies and declarations together. The receipt retains callee ABI reviews and prototype repair bindings separately from accepted owner proofs. Reissue the active coverage packet after success.

### SDK COP2 sites omitted from the machine checklist

Only canonical `SquareRoot0` or `SquareRoot0PSX` may delegate the paired missing GTE data-register capabilities `xport_gte_write_lzcs(uint32 value)` and `xport_gte_read_lzcr(void)` using empty `check_ids`. Each row must add exactly one `original_sites` object with integer `pc`, integer `word`, `transfer` (`mtc2` or `mfc2`) and integer `data_register` (30 or 31). The exact original words are `0x4884F000` and `0x4802F800`; write precedes read. Other service rows retain the nonempty checklist requirement.

Pass the immutable packet explicitly to `authorized_delegation`. Require one full matched SDK context, all four original hashes, complete contiguous instruction bytes matching the function hash, original listing bytes matching its hash, and a regenerated unchanged machine checklist. Sites already represented by checklist checks are rejected. Never invent checklist IDs or modify the original checklist to admit these effects.

Predicted-source validation requires unique unconditional reachable write/read service calls in original order, exact compiled authorized abort services, unchanged normal algorithm mappings and retained unresolved service effects. Full function proof and compiled-source gates still apply. No ordinary game algorithm or other COP2 capability gains a waiver through this route.
# Build policy

- During explicitly requested strict coverage, use the Debug gates required by the supervisor; these are not ordinary full-pass translation checkpoints
- Do not build Release during coverage batches or intermediate fixes
- Build Release once after the whole task is complete as the final verification, or when the user explicitly requests it

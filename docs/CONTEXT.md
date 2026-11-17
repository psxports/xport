# Context and source bindings

Resolve project/X through [../AGENTS.md](../AGENTS.md). Before a stage run `X project_context show`; narrow owners with `X query 0xADDRESS --image IMAGE` or subsystems with `X project_context show --subsystem NAME`. Read full original evidence; missing context is unknown.

## Current state

`status/project.sqlite` owns current tasks/findings/checks/questions and complete bindings; `analysis.sqlite` is rebuildable. Reads never initialize storage; use `X project_context init`. Keep one current record per issue/family, no diary or duplicate JSON progress hierarchy.

Update keys with `X project_context set --key KEY --kind task|finding|check|question --summary TEXT --scope TEXT --next-step TEXT`. Options: --stage, --image/--address, --subsystem, --configuration, repeated --evidence (shared paths: @xport/src/...). Findings/checks require scope/evidence; text fields allow 1200 characters. --closed hides resolved questions. Read with `show --key KEY`; --after pages results (default 8, maximum 32).

Hashes prove neither semantics nor approval. Review changed references; retain historical evidence/limits without rereading each turn.

## Translation bindings

`X project_context queue [--image IMAGE] [--state uncovered|decompiled|sdk] [--limit N] [--after CURSOR]` reads current inventory. SDK entries need reviewed identities and existing API/caller-ABI checks. Legacy TODO/WIP/DONE/SKIP labels or candidate source do not establish completeness.

After full MIPS review run `X project_context decompile --image IMAGE --address 0xADDRESS --source src/batch_1.cpp --symbol SYMBOL --complete --evidence AUDIT_PATH`. Preserve image-qualified original identity/current source/symbol/body hash. Inline .h/.hpp bodies bind by qualified scope; header constructor initializers are unsupported. No credit for prototypes, partial bodies or fail-fast stubs. Bind audited bodies/evidence without retranslating for accounting.

`X query 0xADDRESS --image IMAGE --view callers|callees|data_refs|indirect|reuse_peers --limit 8 [--fields COLUMN,...] [--cursor NEXT]` pages relations; --view sdk reads classification. --audit-context binds original/source evidence. Bounded windows/summaries never replace full MIPS: read the returned path/range when evidence exceeds the response budget.

Queue exclusions use project-local `analysis.decompilation_exclusions`. Schema1: duplicate `groups`. Schema2 adds `removals`: image, integer address, original sha256, reason `user_removal`, explicit human authorization text and nonempty `{path, sha256}` evidence. Record actual authorization before removal. Queue reads reject stale originals/evidence and duplicate identities; preserve original inventory/bindings. Report removals separately from SDK exclusions and translations.

Schema3 `native_replacements`: image/address/sha256, `reason: native_file_replacement`, `reviewed: true`, scope, source/symbol/source_body_sha256, nonempty SHA-bound evidence. Authority: kind `runtime_file_access`, path `@xport/docs/DECOMP_REFERENCE.md`, current sha256/exact quote. Optional dependencies bind source/symbol/body hash. Validate originals/authority/evidence/bodies; no SDK/MIPS credit or waiver of final checks


Save replacements use `native_save_replacement`, kind `runtime_game_save` and the exact quote starting `Replace card-sector I/O, card managers`. Apply schema-3 checks; retain payload/gameplay

Movie replacements use `native_movie_replacement`, kind `runtime_game_movie` and the exact quote starting `Fullscreen movies call xport_fmv_play;`. Apply schema-3 checks; retain scene logic and final verification


Queue/SDK/decompile checks read `paths.sdk_classification` directly: validate image/address/original SHA and reject stale/duplicate/missing input. An explicit file replaces derived SQL even for an empty accepted list; without it use SQL. No analysis refresh or SDK acceptance is implied

## Indexing and formatting

At integration, code_refresh/source_index update derived C/C++ tables and reject invalid mappings. Follow [BUILD_POLICY.md](BUILD_POLICY.md); never refresh per function.

After reviewed decomp boundary corrections, `X build_database --original-only` refreshes accepted original inventory/relations/similarity without native formatting/indexing. Native derived tables stay absent until integration; complete bindings remain in project.sqlite. Preserve frozen scope and explicitly record reviewed additions/superseded occurrences.

`X gp_audit --complete-bindings` reads current complete bindings without native derived tables. Reject stale original/source hashes. Review lexical unresolved indexed addresses semantically; missing literals prove neither mismatch nor correctness.

Before editing the formatter snapshots current registered bodies. Rebind only with unchanged full-file tokens/directives, sensitive macro spelling, .h/.hpp headers and original identities. Changed/stale bodies, unsupported syntax, line-sensitive expressions or changed header contracts remain needs_review. formatting_rebindings holds the latest scoped transfer; formatting_transfers stores deduplicated hash provenance, no source copies. Original audits/runtime evidence stay immutable and unpromoted. A transfer proves only the checked editorial change preserves the source contract.

Review source/configuration/header changes outside this narrow transfer. Never update hashes to hide semantic changes. Refresh source locations after legitimate moves/renames; retain original provenance.

## Git source index

Before a consuming-project stage completes run `X stage_sources`: stage all src additions/edits/deletions (including ignored sources requested by the user), plus existing/tracked root cover.png and README.md. Preserve unrelated staged paths; no commit/push or README edits/regeneration. Successful code_refresh includes staging. Report non-repositories/nested Git roots as blockers; never initialize/reconfigure an unrelated repository.

# Current context and source bindings

Resolve project and X through [../AGENTS.md](../AGENTS.md). Before a stage run `X project_context show`; for an owner use `X query 0xADDRESS --image IMAGE`, for a subsystem use `X project_context show --subsystem NAME`. Read complete original evidence only for the current question; missing context is unknown.

## Current state

`status/project.sqlite` owns current tasks/findings/checks/questions and complete translation bindings. `analysis.sqlite` is derived/rebuildable. Reads do not initialize storage; `X project_context init` does. Keep one current record per issue/family, never a diary or duplicate JSON progress hierarchy.

`X project_context set --key KEY --kind task|finding|check|question --summary TEXT --scope TEXT --next-step TEXT` updates a key. Use --stage, --image/--address, --subsystem, --configuration and repeat --evidence for dependencies; shared evidence uses @xport/src/.... Findings/checks require scope/evidence. Text fields have a 1200-character cap. --closed hides resolved questions. `show --key KEY` narrows reading; --after continues pages (default 8, maximum 32).

Unchanged reference hashes mean references are unchanged, not that semantics or user approval are proved. Changed references need review. Preserve immutable historical evidence and limitations without loading it on every turn.

## Translation bindings

`X project_context queue [--image IMAGE] [--state uncovered|decompiled|sdk] [--limit N] [--after CURSOR]` reads the current inventory. SDK entries require reviewed identities and existing API/caller-ABI checks. Legacy TODO/WIP/DONE/SKIP labels do not promote a body. Candidate source is not automatically a complete translation.

After complete MIPS review use `X project_context decompile --image IMAGE --address 0xADDRESS --source src/batch_1.cpp --symbol SYMBOL --complete --evidence AUDIT_PATH`. Preserve image-qualified original identity and exact current source/symbol/body hash. Prototypes, partial bodies and fail-fast stubs do not count. Resume existing audited bodies by binding their evidence; do not retranslate them just to change accounting.

`X query 0xADDRESS --image IMAGE --view callers|callees|data_refs|indirect|reuse_peers --limit 8 [--fields COLUMN,...] [--cursor NEXT]` pages relations. --view sdk reads classification. --audit-context binds original/source evidence; a bounded window or summary is not the complete MIPS required for audit. Read the exact returned path/range when full evidence exceeds the response budget.

Queue exclusions use project-local `analysis.decompilation_exclusions`. Schema 1 retains duplicate `groups`; schema 2 additionally accepts `removals` with image, integer address, original sha256, reason `user_removal`, explicit authorization text and nonempty evidence entries `{path, sha256}`. Record actual human authorization before using removals. Queue reads reject stale originals/evidence and duplicate identities; they preserve original inventory and translation bindings. Report removals separately from SDK exclusions and completed translations.

Schema 3 adds `native_replacements`: image/address/sha256, `reason: native_file_replacement`, `reviewed: true`, scope, source/symbol/source_body_sha256 and nonempty SHA-bound evidence. Authorization: kind `runtime_file_access`, path `@xport/docs/DECOMP_REFERENCE.md`, current sha256 and exact contract quote. Optional dependencies bind source/symbol/body hash. Validate every original, authority, evidence and body; replacements grant no SDK/MIPS credit and retain final checks


Save replacements use `native_save_replacement` and authority kind `runtime_game_save`, with an exact current quote starting `Replace card-sector I/O, card managers`. Apply all schema-3 validation above; bind roles/callers, retain payload/gameplay and final checks


Queue, SDK queries and decompile checks read configured `paths.sdk_classification` directly. Validate accepted entries against image/address/original SHA; reject stale, duplicate or missing input. An explicit file replaces the derived SQL view, including an empty accepted list. Without this setting use SQL. This refreshes no analysis or SDK acceptance

## Indexing and formatting

At the integration gate, code_refresh/source_index update derived C/C++ tables and reject invalid mappings. Follow [BUILD_POLICY.md](BUILD_POLICY.md); do not refresh after each translated function.

The formatter snapshots current registered bodies before editing. It may rebind only across unchanged full-file tokens/directives, preserved sensitive macro spelling, unchanged .h/.hpp headers and original identities. Changed/stale bodies, unsupported syntax, line-sensitive expressions and changed header contracts remain needs_review. formatting_rebindings stores the latest scoped transfer; formatting_transfers retains deduplicated hash provenance without source copies. Immutable original audits and runtime evidence are not rewritten or promoted. A formatting transfer proves only that the checked editorial transformation preserves the existing source contract.

Source/configuration/header changes outside this narrow transfer require their affected review. Never update hashes just to conceal a semantic change. Refresh source locations after legitimate moves/renames and retain original provenance.

## Git source index

Before a consuming-project stage completes, run `X stage_sources`. It stages all additions/edits/deletions under src plus existing or tracked root cover.png and README.md, including ignored source files as explicitly requested by the user. It preserves unrelated staged paths and performs no commit/push. README is staged without being edited or regenerated. Successful code_refresh includes this step. A non-repository or nested Git root is a reported blocker, not permission to initialize/reconfigure an unrelated repository.

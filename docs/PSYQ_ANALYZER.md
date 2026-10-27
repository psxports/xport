# Standalone PsyQ recognition

Use `tools/psyq_catalog.py`, `tools/psyq_analyzer.py` and `tools/psyq_benchmark.py`. Recognition reads original SDK files and original PS-X EXE bytes. Ghidra, IDA, their signature catalogs and pseudocode are not recognition dependencies.

Accepted four-project comparison and pinned evidence: [PSYQ_ANALYZER_VALIDATION.md](PSYQ_ANALYZER_VALIDATION.md). Resolve the current validated run through `psyq/validation/latest.json`; earlier diagnostic runs are not acceptance evidence.

## Preserved SDK store

Keep SDK evidence under `[XPORT_ROOT]/psyq/VERSION/sources/SOURCE_ID`:

- `sdk/`: unchanged runtime `.LIB`, standalone `.OBJ`, ECOFF `.A`, `.H` and `.INC` files with their original relative paths
- `manifest.json`: original archive hash, upstream URL when available, original and stored paths, sizes, SHA-256 hashes and extraction-tool identities
- `sdk.source.json`: directory-library hash and source-manifest identity for catalog provenance

`psyq/sources.json` selects the preserved source directories and binds their manifests. Before a manifest-based build, verify every listed library and header, reject missing or additional files, and reject any changed manifest or member. Multiple sources can have the same SDK version; preserve their identities separately. Identical matching patterns are scanned once.

Preserve a new source with a fresh source ID:

```powershell
python -B XPORT_ROOT/tools/psyq_store.py --version 4.3 --source-id original --archive SDK_ARCHIVE.zip --store XPORT_ROOT/psyq --url ORIGINAL_URL
```

Append the returned source row to `psyq/sources.json` only after extraction succeeds. Existing preserved sources are never overwritten. Archive readers support tar, ZIP, 7z, ZIP-wrapped multipart RAR, nested LZH/ZIP payloads, SDK directories and ISO9660 data tracks in 2048/2336/2352-byte sectors. 7z/RAR/LZH require installed 7-Zip or an explicit `--sevenzip PATH`. No SDK executable is run. SDK license and copyright text inside retained files remains unchanged.

`tools/psyq_fetch.py --url URL --size REMOTE_ZIP_BYTES --expected-sha1 EXPECTED_REMOTE_SHA1 --output FRESH.zip` retrieves only the data track and CUE from a remote CD ZIP using exact HTTP ranges. The sidecar records the source URL, original member CRCs, extracted-member SHA-256 hashes and compact-archive SHA-256. `full_remote_hash_verified: false` is mandatory: a partial download does not verify the complete upstream archive hash. Keep that sidecar with the compact archive until storing the source.

## Build the catalog

```powershell
python -B XPORT_ROOT/tools/psyq_catalog.py --manifest XPORT_ROOT/psyq/sources.json --output XPORT_ROOT/psyq/catalog.sqlite
```

Use a fresh output path for subsequent builds. Explicit additional sources use repeatable `--sdk VERSION=ARCHIVE_OR_DIRECTORY`. The SQLite database contains source archives/directories, files, original library-member hashes, complete text bytes, masks, symbols, relocation expressions, equivalent ECOFF procedures, object signatures and individual exported/procedure spans.

Read `issues` and the per-version `metadata.summary` before claiming complete SDK inventory. Unknown records, malformed members and unsupported relocation forms are retained as explicit issues; those members do not produce signatures. `inventory_complete: false` does not invalidate separately parsed members, but does prohibit a claim of full SDK coverage.

## Analyze an original image

```powershell
python -B XPORT_ROOT/tools/psyq_analyzer.py --catalog XPORT_ROOT/psyq/catalog.sqlite --image PROJECT/ORIGINAL_EXE --image-name IMAGE --gp 0xRUNTIME_GP --output PROJECT/status/psyq-analyzer/FRESH_RUN/IMAGE
```

`--gp` is optional; absent GP-dependent checks remain unresolved. For a project, obtain image identity, executable path and runtime GP from its configured image profile. Never infer a sibling project or choose an excluded image. Default input is a complete PS-X EXE; scan only its declared initialized payload.

For a reviewed raw MIPS overlay, supply all three explicit options: `--raw-base ADDRESS --raw-offset FILE_OFFSET --raw-size BYTES`. Resolve them from a verified original loader and the selected profile's bounded code range; validate runtime GP before scanning. The base is the runtime address of the first selected byte. Require aligned, nonempty, in-file bounds and reject PS-X EXE input in raw mode. Unaligned data tails outside the selected span are permitted. Never synthesize a PS-X header or infer executable content from an extension. `report.input_layout` binds the selected raw mapping, while `input_sha256` binds the entire unchanged file. Inventory binding uses that exact mapping and full original-body hashes. Data outside reviewed code ranges is not a function-discovery seed.

The Python batch API `scan_loaded(objects, catalog_info, image, image_name, gp, raw_layout)` reuses the result of `load_objects(catalog)` for independent scans. It does not combine simultaneously resident images or inject parent-image call anchors. Unresolved imported calls prevent confirmation. SDK calls into a parent executable do not establish SDK bodies inside the overlay. Raw scans without preserved matching Ghidra detector baselines remain standalone recognition; do not fabricate comparison baselines.

Outputs:

- `report.json`: image/catalog/tool hashes, matching SDK signature spans, every relocation check, rejected candidates, symbol alternatives and confirmation policy
- `symbols.csv`: image addresses, status, selected name, alternatives and compatible SDK versions
- `analysis.sqlite`: indexed `symbols` and `matches`, with evidence and provenance; this is separate from project coverage storage
- `apply_ida.py`, `ApplyPsyqNames.java`: scripts for importing confirmed labels into an existing disassembler database

The import scripts verify every selected original-byte span before applying any name. Existing user-defined names are preserved; the Ghidra script also preserves imported names. Run the IDA script in IDA, or place the Java file in Ghidra's script directory and run `ApplyPsyqNames`. These scripts assign labels and do not invent function boundaries, types or prototypes. The generated scripts are inspected and byte guards are tested; running inside an interactive disassembler is a separate operation.

Supply `--inventory PROJECT/orig/images/IMAGE/functions` to bind immutable complete `.mips.txt` bodies without Ghidra. Require matching image names, contiguous original bytes and body hashes. The `entries` SQL table distinguishes `confirmed`, `candidate` and `unrecognized` library membership. Confirmation requires the entire original body to fit inside a sufficiently strong fully checked SDK signature. An internal body can have confirmed SDK membership while its original name remains unknown. A matching entry prefix does not confirm a longer IDA body. This metadata does not change any project's classification or accounting.

## Matching contract

1. Search exact original instruction/data bytes with only linker-relocated fields masked. Preserve opcode/register bits and nonrelocated constants.
2. Scan full object text and bounded exported/procedure spans. For a span inside an object, retain the original section origin when interpreting local references.
3. Check internal calls and uniquely anchored imported calls. Reject contradictions and repeat candidate elimination until stable.
4. Pair HI16/LO16 expressions with signed-low carry. Infer unknown data-section/symbol bases inside each candidate and reject inconsistent repeated accesses. Common/BSS declarations are independent linker-allocated symbols; never assign them fixed relative offsets from their declaration order. Unsupported expression arithmetic or unresolved call targets prevent automatic naming.
5. A strong match needs at least 64 completely fixed bytes, a 16-byte exact anchor and fixed-byte entropy of at least 2.0. Exact BIOS selector stubs are the short-pattern exception. Ignore patterns below 16 fixed bytes or entropy 1.5. An empty `jr ra; nop` is not an identifiable SDK signature.
6. Rank eligible evidence by completely fixed bytes of matching context. A unique strongest eligible name becomes `confirmed`; competing equally strong names stay `ambiguous`. Weaker alternatives remain visible. A sole name without sufficient evidence remains `candidate`.

`confirmed` attests this catalog's reproducible binary-identity policy. Inferred data destinations are consistency checks, not independent evidence of the original global-variable name. Export spans are symbol spans; an exported text label may designate data. Exact procedure sizes exist only when original SDK debug/ECOFF metadata provides them. Recognition does not attest native ABI compatibility, runtime equivalence or translated code.

Do not convert `candidate` or `ambiguous` to SDK exclusions. This command changes no immutable exports, current SDK classification, project coverage denominator, native source or `status/project.sqlite`. Integrate confirmed identities into a project's classification through its applicable review/integration stage, retaining original image/body hashes and inventory.

## Startup integration

The startup supervisor selects this detector by default (`analysis.sdk_detector: "xport"`). `paths.sdk_catalog` overrides the shared catalog path. At the running `psyq` step, `psyq_project.py` writes a fresh `status/psyq-analyzer/runs/LABEL` classification, binds it to accepted original body hashes and selects that path for the subsequent existing `psyq_review`. Only reviewed classifications reach `build_database` and the translation queue. Short/unresolved matches and ambiguous names remain pending. Confirmed internal SDK bodies without a recoverable original name receive an explicit technical `SDK_INTERNAL_ADDRESS` alias, not an invented public API name.

The publisher refuses calls outside the running startup `psyq` stage. It does not migrate an existing coverage scope. The four-project benchmark uses the read-only analysis path and never invokes this publisher. Explicit `analysis.sdk_detector: "ghidra"` retains legacy startup recognition; no silent fallback is permitted. Supplemental Ghidra decompilation remains a separate startup requirement.

## Compare with preserved Ghidra results

```powershell
python -B XPORT_ROOT/tools/psyq_benchmark.py --catalog XPORT_ROOT/psyq/catalog.sqlite --project FIGHTING_FORCE_PROJECT --project GEX3_PROJECT --project TD5_PROJECT --project TEKKEN3_PROJECT --run FRESH_RUN --output XPORT_ROOT/psyq/validation/FRESH_RUN.json
```

The benchmark resolves all selected, nonexcluded images from each explicit `xport-project.json`. Require a matching single-load image profile and exactly one preserved original detector report at `orig/ghidra/IMAGE.json` or `status/ghidra/runs/startup-psyq-v1/IMAGE.json`. Reject ambiguous baselines, stale patterns, mismatched image identities, invalid load bases and changed original listing hashes.

Reports live under each project's configured status directory in `psyq-analyzer/FRESH_RUN/IMAGE`. The aggregate binds each input, report and comparison by SHA-256. Comparisons deduplicate versions and omit technical `loc_`/`text_` labels on both sides. Count symbol addresses separately from original IDA entries. Ghidra's named-address count includes ambiguous alternatives; compare it with all analyzer candidates as well as the stricter confirmed count. `confirmed_name_differences` separately reports selected automatic names conflicting with all Ghidra names at the same address. Neither side is ground truth.

For an explicitly narrower comparison, repeat `--image IMAGE` on `psyq_benchmark.py`. Every requested image must be configured and nonexcluded in the explicit projects; missing/excluded identities fail before scanning. Omitting this flag retains the complete configured selection. Record unsupported raw overlays and missing baselines separately; the filter must not be presented as a full-project recognition result or change project exclusions. The scope-filter tests are `python -B -m unittest discover -s tests -p test_psyq_benchmark_scope.py` from `tools`.

## Verification

```powershell
Set-Location XPORT_ROOT/tools
python -B -m unittest discover -s tests -p test_psyq_analyzer.py -v
```

Require passing relocation, false-positive, function-span, archive/directory, LIB2, original-byte, provenance and header-integrity checks. For a catalog change, rebuild from the preserved store and rerun the affected benchmark cases. Native Debug/Release game builds are not a verification of this Python-only tool.

After raw-input changes, also run `python -B -m unittest discover -s tests -p test_psyq_raw.py` from `tools`. These checks cover selected-span isolation, runtime address mapping, whole-file identity, full original inventory binding and rejection of malformed bounds or PS-X inputs disguised as raw. Raw support changes the analyzer source hash; preserve earlier validated reports and record the new source hash in fresh runs without reusing their acceptance label.

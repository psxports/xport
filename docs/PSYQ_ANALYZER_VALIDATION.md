# PsyQ analyzer validation

Accepted run: `validation-v3`, 2026-10-24. Commands and matching invariants: [PSYQ_ANALYZER.md](PSYQ_ANALYZER.md). Machine-readable selection: [latest.json](../psyq/validation/latest.json).

## Deliverable

Standalone recognition uses original SDK libraries, relocation records and original PS-X EXE bytes. It does not read Ghidra signatures or require a running disassembler. Immutable IDA MIPS exports supply complete original body boundaries. Generated IDA/Ghidra scripts import guarded confirmed labels. Startup now selects this detector by default; the existing `psyq_review` stage still precedes project database publication.

The catalog uses relocation-aware byte signatures rather than mnemonic-only hashes. Registers, opcodes and nonrelocated constants remain significant. Internal/imported calls and repeated HI/LO data references are checked. Equivalent SDK versions are retained as alternatives; a compatible binary does not prove one exclusive SDK version. Binary identity does not attest the host implementation or caller ABI.

## Preserved inputs

The store `psyq/VERSION/sources/SOURCE_ID` contains 698 original binary library/object files and 1,371 headers, totaling 61,895,534 bytes across 16 source snapshots. Versions: **2.0, 2.6, 3.0, 3.3, 3.5, 3.6, 4.0, 4.1, 4.3, 4.4, 4.5, 4.6, 4.7**. [sources.json](../psyq/sources.json) binds every per-source manifest and every retained member. Rebuilds verify all bytes offline before parsing.

Sources include the supplied local archives, [FoxdieTeam/psyq_sdk](https://github.com/FoxdieTeam/psyq_sdk), [arthus Psy-Q](https://psx.arthus.net/sdk/Psy-Q/) and [Internet Archive SDK collection](https://archive.org/download/ps1_sdks). FoxdieTeam commit `c2ef6aec5823c6ca3a2ceef4ee370495e92fa4d2` has 84 top-level library/object files identical to preserved 4.3/4.4/4.5 inputs; per-version evidence records the checks. CD data tracks have verified ZIP member CRCs and recorded SHA-256 hashes. Partial remote retrieval does not verify the entire upstream ZIP SHA-1; manifests explicitly retain that distinction.

The SQLite catalog contains 30,104 objects, 4,727 unique full-object patterns and 9,002 unique object/symbol-span patterns. Equivalent ECOFF metadata contributes 5,855 linked records. Six rejected occurrences represent two unique malformed SDK 3.6 members, `MOUSE.OBJ` and `PAD.OBJ`; their original bytes and parser issues remain preserved. Inventory completeness is therefore explicitly false. These members produce no signatures.

Catalog SHA-256: `91d19b491790bbfedd092bf28e9304ac48c52344ce9300c41baea1cc7b857b9c`.

## Comparison

Both detectors were checked against the same original bytes. Ghidra counts include alternative labels from preserved raw detector reports, with versions deduplicated and technical `loc_`/`text_` names removed. РІР‚СљAll xportРІР‚Сњ includes weak and ambiguous candidates. РІР‚СљConfirmedРІР‚Сњ requires a unique strongest eligible name and no unresolved relocations. These are symbol addresses, not function counts. The last column separately counts entire original IDA bodies inside strong fully checked SDK spans; some internal bodies have no recovered SDK name.

| Project / image | Ghidra addresses | All xport | Confirmed names | Additional confirmed names | Ghidra only | Confirmed SDK bodies / verified original bodies |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| FightingForce / `SLUS_004.33` | 274 | 284 | 216 | 0 | 0 | 176 / 259 |
| FightingForce / `GAME.EXE` | 312 | 412 | 183 | 1 | 2 | 156 / 1273 |
| Gex 3 / `SLUS_008.06` | 120 | 369 | 136 | 73 | 0 | 141 / 2103 |
| Test Drive 5 / `SLUS_006.10` | 315 | 468 | 328 | 86 | 0 | 288 / 973 |
| Tekken 3 / `SLUS_004.02` | 411 | 595 | 339 | 72 | 3 | 294 / 1474 |

Across all five images, **zero confirmed selected-name conflicts** with Ghidra. The one candidate-name disagreement is Fighting Force `GAME.EXE` at `0x8006CD5C`: Ghidra `__main`, weak xport alternative `SsUtVibrateOff`; no automatic name is assigned.

Ghidra-only addresses remain explicit:

- Fighting Force `GAME.EXE`: `0x80061120` (`SpuInit`/`SsUtReverbOff`) and `0x8006CE04` (`stup0`)
- Tekken 3: `0x8004B770`, `0x8004C370` (`SpuInit`/`SsUtReverbOff`) and `0x80094D38` (`_padInitDirSeq`)

Matching-symbol candidates for these addresses include relocation contradictions, retained with actual/expected values in [discrepancies-v3.json](../psyq/validation/discrepancies-v3.json). The `stup0` label belongs to a larger `__main` span rejected by inconsistent data references. These exclusions reflect the current conservative recognition policy; they do not establish that every Ghidra label is wrong. Modified/relinked code and absent SDK variants still require investigation.

TestDrive5 `0x800600FC` is a 284-byte original body crossing SDK signature boundaries. Its result is `candidate` / `sdk_overlap`, with explicit matched spans. An SDK prefix never excludes the entire longer body. Configured `MOVPLAY.EXE` remains excluded from the benchmark.

## Evidence and checks

- [comparison-v3.json](../psyq/validation/comparison-v3.json): five images, input/profile/report/comparison hashes and absolute per-project report paths
- [catalog-build-v2.json](../psyq/validation/catalog-build-v2.json): source-store/catalog identities and complete parser statistics
- [tests-v2.json](../psyq/validation/tests-v2.json) and [tests-v2.txt](../psyq/validation/tests-v2.txt): **36 tests passed**, bound to tool-source hashes
- [Ghidra reference manifest](../psyq/validation/ghidra-reference/manifest.json): preserved installed plugin source ZIP/JAR, properties and patches

The installed plugin reference (`psyq/SigApplier.java`, `PsyqSig.java`, `MaskedBytes.java`, `DetectPsyQ.java`) loads JSON masked patterns/labels, applies game-specific patches, scans masked bytes and uses entropy/BIOS exceptions. Its version probe uses an embedded marker or selected legacy LIBGPU patterns. The benchmark wrapper scans all available signature versions. The new recognizer builds its own catalog from SDK object relocations and adds reference-consistency checks; the preserved plugin is comparison evidence only.

Tests cover relocation masking, register/constant specificity, call disambiguation, HI/LO carry, independently allocated common symbols, short-pattern rejection, version/name ambiguity, full-body boundaries, malformed catalog guards, LIB2/ECOFF parsing, archive/directory provenance, header tampering, remote ZIP ranges/CRC and startup publication guards. Catalog integrity, all source-store members and final tool/report hashes were verified. Generated importer scripts were not executed inside live IDA/Ghidra sessions. Native game builds were not run for this Python-only change.

The benchmark wrote fresh analysis folders only. Existing project SDK classifications, immutable original exports, translation accounting and native sources were not migrated. Earlier development/validation runs are diagnostic and superseded by `validation-v3`, especially `validation-v2` which predates the common-symbol allocation correction. Recognition is self-contained; supplemental Ghidra decompilation remains a separate startup requirement.

# Regional PS-X executable comparison

## Scope and inputs

Use `tools/region_compare.py` for an automated first-pass comparison of two regional PS-X EXE distributions. Supply an explicit consuming project, two extracted image directories containing `SYSTEM.CNF` and `MAIN.EXE`, an installed MIPS-capable IDA, and a new project-owned output directory. The tool resolves the boot executable from each `SYSTEM.CNF` and compares MAIN with MAIN and boot with boot separately.

This command does not translate code, accept function boundaries, rename the NTSC database from inferred matches, change game code, or perform semantic review. Unmatched functions are not evidence of added, removed or changed functionality.

## Commands

Run from the consuming project root. Resolve `XPORT_ROOT` from `xport-project.json`.

```powershell
python -B "[XPORT_ROOT]/tools/region_compare.py" --project . --output status/region-compare --pal orig --ntsc iso/NTSC-U/extracted --ida "C:/Tools/IDA Pro 7.7.220118/idat.exe"
```

Repeat comparison without opening IDA:

```powershell
python -B "[XPORT_ROOT]/tools/region_compare.py" --project . --output status/region-compare --compare-only
```

Rerun only strict verification and regenerate the report from the saved similarity results:

```powershell
python -B "[XPORT_ROOT]/tools/region_compare.py" --project . --output status/region-compare --verify-only
```

Normal analysis/comparison also runs this verifier. Neither `--compare-only` nor `--verify-only` invokes IDA or reads pseudocode.

The first command reuses completed exports only when their GP and source mapping agree. Source SHA-256 values are retained in `inputs.json`; changing an original requires a new output directory. `--compare-only` also verifies current source hashes against that manifest. Preserve the original inputs and existing canonical PAL databases/exports. All new IDA databases and exports belong under the supplied output directory.

## GP and memory mapping

1. Validate the PS-X EXE header, entry point and loaded span
2. Detect runtime `lui gp` / `addiu gp,gp` initialization with the shared startup scanner; require one distinct value and reject disagreement with a nonzero header GP
3. Validate the entry-point BSS clearing loop and derive its start/end; map the initialized file span plus its BSS extension
4. Invoke the shared IDA exporter in MIPS little-endian mode with separate databases for every region/image
5. Rebuild authoritative segments, assign GP, then analyze the rebuilt image
6. Require every decoded GP-relative byte/halfword/word load/store checked by the exporter to have the expected `gp + signed16(offset)` data reference

Reject unknown startup/BSS patterns, contradictory GP evidence, missing exports, GP reference mismatches or changed instruction bytes. The startup recognition is deliberately limited to the observed PS-X startup convention; another game may require reviewed mapping support rather than silently guessing. Code discovery seeds entry points, stack-frame prologues and direct calls; boundaries remain `IDA_discovered_unreviewed`. Pseudocode failures are retained and do not block byte-level comparison.

## Matching and candidate evidence

- Verify each exported physical instruction against the source binary; split IDA multiword macros into physical MIPS32 words
- Match unique byte-identical functions of at least 16 bytes
- Match unique normalized instruction sequences: direct jump/call operands, local branch targets, GP-relative references and short LUI/address-construction sequences are represented independently of relocation addresses; indexed `addu` table addressing retains its LUI base
- Preserve ordinary numeric constants, structure offsets and device addresses; do not blank every immediate
- Seed data correspondence from unique strings and distinctive equal initialized IDA items; propagate unambiguous references at corresponding instructions of exact/normalized functions
- Decode MIPS physical words through `tools/mips_compare.py`, retaining signed field offsets, access widths, constants, branch kinds and coprocessor operations
- Track bounded register-value provenance through moves, address arithmetic, argument aliases and stack spills; account for delayed loads and the call delay slot before caller-register clobbering
- Extract normalized operation events, non-stack memory access signatures, known direct-call identities and address-independent control-flow histograms
- Identify known callees only through the existing unique exact/normalized function map; their numeric addresses and display names never contribute similarity
- Compare operation/memory/call/control fingerprints against all unmatched NTSC functions, without a size cutoff; fully score the best 32 preliminary candidates plus every equal-size candidate and retain the best five
- Preliminary weights are operation-event bag 45%, memory 30%, known calls 20% and control flow 5%; absent-in-both components are omitted and weights renormalized
- Detailed `similarity_pct` weights are normalized instructions 25%, opcodes 15%, normalized event sequence 30%, memory fields 20%, known calls 7% and control flow 3%; absent-in-both optional components are omitted and weights renormalized
- Sort both function inventories and the report by physical instruction size; expose size similarity separately and never use numeric address proximity
- Keep all similarity-only pairs unaccepted, even at 100%; accept only unique exact/normalized signatures into the function map
- Similarity values are code-similarity percentages, not probabilities or semantic equivalence

Ordering is deterministic, including tied candidate generation. Existing accepted PAL SDK names and game `FUNCTION_MARKER` names are display labels only, never independent matching evidence.

## Output and interpretation

- `inputs.json`, per-image configs/commands/logs and `.idb`: input hashes, GP/BSS evidence, exact launch commands and isolated IDA databases
- `pal/<image>/` and `ntsc-u/<image>/`: complete exported function listings, pseudocode where available, call/data references, symbols, strings and `comparison-features.json`
- `comparison.json`: full one-to-one map, changed instruction words/address operands, ranked candidates, data correspondence, unmatched items and independent `functions_by_size` inventories on both sides
- `functions.csv`: every PAL function ordered by size, with its map or best unaccepted candidate and `similarity_pct` in 0–100 units
- `potential-differences.json`: functions requiring evaluation, changed mapped data and unmatched NTSC functions/data
- `index.html`: local searchable function list sorted by size with similarity percentages, strict verification reasons and a changed-data shortlist; only strictly verified equivalents are hidden by default

Categories:

- `identical_bytes`: function bytes are equal
- `address_operands_only`: normalized sequences are equal; this does not prove equality of referenced data or program behavior
- `target_change_candidate`: a changed operand contradicts an already mapped target
- `potential_code_change`: structural match with changed code, still tentative
- `ambiguous` / `no_candidate`: no accepted function correspondence
- `identical_item`: equal bytes and size for the mapped IDA data item
- `mapped_pointer_relocation`: all changed words are explained by established target mappings
- `potential_data_change`: mapped item bytes or item sizes differ
- `reference_only`: no pair of initialized item byte spans is available

IDA data boundaries may cover a scalar or a fragment of a larger array/structure. BSS correspondence is reference evidence, not a comparison of initialized bytes. The number of discovered functions can differ because of real code differences, compiler changes or analysis boundaries. Do not infer semantic changes from these counts.

Register provenance is a bounded linear fingerprint, not an emulator or full SSA/CFG analysis. Conditional paths are not solved, memory aliasing is not proven and call outputs are abstract. Partial/unaligned loads retain their instruction kind without emulating merge behavior. The broad search reduces the size-selection blind spot but its 32-candidate preliminary shortlist can still miss a true match. Do not claim behavioral equivalence from 100% fingerprint similarity.

## Strict exclusion pass

`tools/mips_equiv.py` is separate from heuristic similarity. It checks established pairs and unoccupied one-to-one candidate proposals with similarity at least 80% and a forward gap of at least 5 points. A candidate's differing instructions do not establish a regional difference until correspondence is accepted; such a case stays `unknown`.

- Compare every physical instruction in order, retaining opcodes, constants, widths, signedness, coprocessor selectors and all delay slots
- Require equal local branch/jump targets by corresponding instruction index; reject unresolved fallthrough across chunk gaps
- Permit a global bijection of defined caller-saved temporary registers in straight-line leaf functions; retain ABI registers and reject read-before-definition or control-flow permutations
- Validate GP-relative accesses and supported straight-line LUI/absolute accesses against one-to-one data mappings and every accessed initialized byte span; validate repeated accesses through an absolute base, not just its first load
- Reject unknown extents, indexed/address-taking uses, pointer-like initialized words, unavailable BSS bytes and overlapping contradictory byte mappings
- Require fixed GP, unobserved numeric return addresses and supported private stack allocation/restoration; reject indirect control flow, unsupported instructions and stack-address escapes that can expose return addresses
- Require a strict proof for every direct called/tail function. Compute the greatest fixed point of locally passing bodies; mutually recursive groups can remain only if their entire outgoing dependency closure passes

Statuses are `equivalent`, `difference_detected` and `unknown`, with exact reasons and instruction sites where available. `difference_detected` records concrete instruction/data differences under an established correspondence, not a behavioral counterexample. `equivalent` is instruction-trace equivalence within the declared ABI/mapped-memory model, not a whole-game theorem. Input pointer correspondence must preserve aliases/layout; private stack frames must not alias caller objects. CD assets, timing and changed external inputs remain outside this check.

The first verifier deliberately does not support instruction/block reordering, arbitrary register SSA joins or SMT. These cases remain `unknown`. The default HTML view and `potential-differences.json` omit only `equivalent`; byte-identical and similarity-100% functions with unresolved dependencies remain visible. CSV and full JSON retain all functions and proof details.

## Verification and terminal condition

```powershell
$env:PYTHONPATH = "[XPORT_ROOT]/tools"
python -B -m unittest discover -s "[XPORT_ROOT]/tools/tests" -p test_region_compare.py -v
python -B -m unittest discover -s "[XPORT_ROOT]/tools/tests" -p test_mips_equiv.py -v
```

Before delivering a project report, verify all source hashes, saved databases, zero GP mismatches, complete accounting of both function sets and one-to-one function/data maps. Record analysis versions, coverage, candidate counts and limitations in project-owned evidence. Stop after delivering the shortlist when semantic inspection has not been authorized.

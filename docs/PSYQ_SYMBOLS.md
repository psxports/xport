# Recover SDK names from an original PsyQ CD

For the reusable versioned SDK store, standalone SQLite analyzer and multi-project comparison, use [PSYQ_ANALYZER.md](PSYQ_ANALYZER.md). The command below is the preserved specialized 4.3 CD recovery tool.

Run `tools/psyq_sdk_symbols.py` for static symbol recovery from the Runtime Library 4.3 ZIP containing one Mode2/2352 data track and seven audio tracks.

```powershell
python -B XPORT_ROOT/tools/psyq_sdk_symbols.py --archive SDK_ARCHIVE.zip --image PROJECT/bin/DATA/SLUS_006.10 --original-inventory PROJECT/orig/images/SLUS_006.10/functions --output PROJECT/status/psyq43-recovery/RUN
```

Inputs:

- Original ZIP, read directly without extracting executables or running SDK programs
- Original PS-X EXE with its own load address and declared payload size
- Immutable complete `.mips.txt` exports with image-qualified entry and raw-body SHA-256
- Fresh output directory; an existing output directory is rejected

The tool verifies every ZIP member CRC, records archive/member hashes, reads ISO9660 through Mode2 Form1 sectors, and parses every `LIB` v1 / `LNK` v2 member under `PSX/LIB`. Unknown records, unsupported relocations and parse failures are explicit errors in the report; require `parse_errors: 0` before treating the SDK inventory as complete.

It also reads the ECOFF local procedure tables under `COFF/LIB`, including static procedure names and source filenames. Each COFF text section must match its corresponding LNK text section outside relocation immediates before its names can be mapped into the target. Require `coff_parse_errors: 0`; retain unmatched format variants explicitly. ECOFF metadata is big endian while its PSX instructions remain little endian. File descriptor string bases must be applied independently.

It matches complete `.text` sections outside relocation immediates, preserves opcode/register bits, and checks resolvable imported call targets against strong object matches. Strong objects require at least 64 fixed bytes and a fixed run of at least 16 bytes. These thresholds rank evidence; they do not prove every unresolved relocation or an original function boundary.

Outputs:

- `report.json`: CD provenance, library/object hashes, real SDK symbol tables, masks/relocations, matching locations, rejected candidates and original-entry name candidates
- `disc-files.json`: ISO9660 file paths, extents and lengths
- `function-name-candidates.csv`: strong names and weaker alternatives for existing original entries
- `summary.json`: bounded counts and parse status

Review actual symbol offsets before changing names. A public SDK symbol eight bytes before an IDA entry can identify a missed prefix. A `.text` symbol can also denote data or an internal block. Short identical wrappers and names with unresolved relocation targets remain candidates. Retain conflicts rather than selecting the first match. Do not invent names for SDK internals absent from the original symbol table.

This command changes no original export, classification, native source, analysis database or coverage count. Save reviewed aliases and boundary corrections as separate evidence. Apply derived database changes at the task's permitted integration gate. Excluded images remain excluded.

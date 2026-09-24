"""Convert an immutable legacy per-function IDA export to the canonical xport format"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

from xport_project import artifact_path, project_root


R = project_root()
INSTRUCTION = re.compile(r"^([0-9A-Fa-f]{8})\s+((?:[0-9A-Fa-f]{2}\s+){3}[0-9A-Fa-f]{2})\s+(.*)$")


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def parse_listing(path):
    instructions = []
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        match = INSTRUCTION.match(line)
        if not match:
            continue
        instructions.append({
            "address": int(match.group(1), 16),
            "bytes": bytes.fromhex(match.group(2)).hex(),
            "text": match.group(3).strip(),
        })
    return instructions


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--legacy", type=Path, required=True)
    parser.add_argument("--image", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    legacy = args.legacy.resolve()
    output = artifact_path(args.output)
    if output.exists() and any(output.iterdir()):
        parser.error("Output must be absent or empty")
    index = read_json(legacy / "index.json")
    report = read_json(legacy / "export-report.json")
    failures = read_json(legacy / "decompilation-failures.json")
    old_refs = read_json(legacy / "data-xrefs.json")
    output.mkdir(parents=True, exist_ok=True)
    (output / "functions").mkdir()
    (output / "pseudocode").mkdir()
    records = []
    symbols = {}
    for item in index:
        address = int(item["address"], 16)
        instructions = parse_listing(legacy / item["assembly"])
        raw = b"".join(bytes.fromhex(entry["bytes"]) for entry in instructions)
        if len(raw) != item["instruction_count"] * 4:
            raise ValueError(f"Instruction count mismatch at {item['address']}")
        if hashlib.sha256(raw).hexdigest() != item["bytes_sha256"]:
            raise ValueError(f"Instruction hash mismatch at {item['address']}")
        chunks = [[int(chunk["start"], 16), int(chunk["end"], 16)] for chunk in item["chunks"]]
        pseudocode = None
        if item.get("pseudocode"):
            source = legacy / item["pseudocode"]
            if not source.is_file():
                raise FileNotFoundError(source)
            pseudocode = f"pseudocode/{address:08X}.c"
            shutil.copyfile(source, output / pseudocode)
        shutil.copyfile(legacy / item["assembly"], output / "functions" / f"{address:08X}.lst")
        records.append({
            "address": address,
            "end": int(item["end"], 16),
            "name": item["name"],
            "chunks": chunks,
            "sha256": item["bytes_sha256"],
            "size": len(raw),
            "legacy_span_size": item["size"],
            "status": "TODO",
            "boundary_status": "legacy_IDA_discovered_unreviewed",
            "pseudocode": pseudocode,
            "instructions": instructions,
        })
        symbols[address] = {"address": address, "name": item["name"], "kind": "code", "size": len(raw)}
    data_refs = []
    for item in old_refs:
        target = int(item["address"], 16)
        symbols.setdefault(target, {"address": target, "name": item["name"], "kind": "data", "size": None})
        for source in item.get("referenced_by", []):
            data_refs.append({"source": int(source, 16), "site": None, "target": target, "type": 0})
    starts = {entry["address"] for entry in records}
    unknown_sources = sorted({entry["source"] for entry in data_refs if entry["source"] not in starts})
    if unknown_sources:
        raise ValueError("Legacy data references contain unknown source functions")
    old_graph = read_json(legacy / "callgraph.json")
    edges = [{
        "source": int(item["caller"], 16),
        "site": None,
        "target": int(item["callee"], 16),
        "kind": "legacy_function_scope",
    } for item in old_graph.get("edges", [])]
    write_json(output / "functions.json", records)
    write_json(output / "symbols.json", sorted(symbols.values(), key=lambda item: item["address"]))
    write_json(output / "data-xrefs.json", data_refs)
    write_json(output / "callgraph.json", edges)
    write_json(output / "indirect-calls.json", [])
    write_json(output / "strings.json", [])
    write_json(output / "decompilation-failures.json", [{"address": int(item["address"], 16), "error": item["error"]} for item in failures])
    write_json(output / "export-report.json", {
        "image": args.image,
        "ida": report.get("ida_version", "legacy"),
        "hexrays": "legacy export",
        "python": report.get("python_version"),
        "gp": int(report["expected_gp"], 16),
        "segments": report["segments"],
        "functions": len(records),
        "pseudocode": sum(record["pseudocode"] is not None for record in records),
        "failures": len(failures),
        "legacy_source": str(legacy.relative_to(R)),
        "legacy_source_sha256": hashlib.sha256((legacy / "index.json").read_bytes()).hexdigest(),
        "limitations": [
            "Data-reference sites were not present in the legacy export and remain null",
            "Legacy call edges are retained for provenance; build_database reconstructs direct edges from verified words",
        ],
    })
    print(json.dumps({"image": args.image, "functions": len(records), "data_refs": len(data_refs), "output": str(output)}))


if __name__ == "__main__":
    main()

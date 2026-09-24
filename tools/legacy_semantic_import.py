"""Import legacy names and C structure inventory without transferring DONE status"""
import argparse
import hashlib
import json
from pathlib import Path
import re

from xport_project import artifact_path, project_root


R = project_root()
ADDRESS = re.compile(r"^0x[0-9A-Fa-f]{8}$")
QUALIFIED = re.compile(r"^([A-Z][A-Z0-9]*)_([0-9A-F]{8})$")


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def write_json(path, value):
    path = artifact_path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def source_lookup():
    result = {}
    for path in (R / "src").rglob("*"):
        if path.is_file():
            result.setdefault(path.name.lower(), []).append(path)
    return result


def structures():
    result = []
    for path in sorted((R / "src").rglob("*.h")):
        lines = path.read_text(encoding="utf-8-sig", errors="replace").splitlines(keepends=True)
        index = 0
        while index < len(lines):
            match = re.search(r"\btypedef\s+struct(?:\s+([A-Za-z_]\w*))?", lines[index])
            if not match:
                index += 1
                continue
            start = index
            text = ""
            depth = 0
            opened = False
            while index < len(lines):
                text += lines[index]
                depth += lines[index].count("{") - lines[index].count("}")
                opened = opened or "{" in lines[index]
                if opened and depth == 0 and ";" in lines[index]:
                    break
                index += 1
            tail = re.search(r"}\s*([A-Za-z_]\w*)\s*;", text)
            name = tail.group(1) if tail else match.group(1)
            if name:
                result.append({
                    "name": name,
                    "source": path.relative_to(R).as_posix(),
                    "start_line": start + 1,
                    "definition_sha256": hashlib.sha256(text.encode("utf-8")).hexdigest(),
                    "status": "legacy_hypothesis",
                })
            index += 1
    unique = {}
    for item in result:
        unique.setdefault((item["name"], item["source"], item["start_line"]), item)
    return list(unique.values())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--progress", type=Path, required=True)
    parser.add_argument("--functions", type=Path, required=True)
    parser.add_argument("--image", required=True)
    parser.add_argument("--semantic-output", type=Path, default=Path("status/semantic-map.json"))
    parser.add_argument("--ledger-output", type=Path, default=Path("status/translation-ledger.json"))
    args = parser.parse_args()
    progress_path = args.progress.resolve()
    functions_path = args.functions.resolve()
    progress = read_json(progress_path)
    canonical = read_json(functions_path)
    functions = {item["address"]: item for item in canonical}
    sources = source_lookup()
    aliases = []
    ledger = []
    unresolved = []
    seen_aliases = set()
    for item in progress.get("functions", []):
        if not ADDRESS.match(str(item.get("address", ""))) or not item.get("semantic_name"):
            continue
        address = int(item["address"], 16)
        original = item.get("original_name") or item.get("name")
        qualified = QUALIFIED.match(original or "")
        image = args.image if address in functions else qualified.group(1) if qualified else None
        if image is None:
            unresolved.append({"reason": "image_identity_missing", **item})
            continue
        binding = "bound" if image == args.image and address in functions else "pending_image_inventory"
        key = (image, address, "function", item["semantic_name"])
        if key in seen_aliases:
            continue
        seen_aliases.add(key)
        candidates = sources.get(Path(item.get("source_file") or "").name.lower(), [])
        source = candidates[0].relative_to(R).as_posix() if len(candidates) == 1 else None
        alias = {
            "image": image,
            "address": address,
            "kind": "function",
            "original_name": original,
            "semantic_name": item["semantic_name"],
            "binding": binding,
            "status": "legacy_hypothesis",
            "legacy_status": item.get("status"),
            "source": source,
        }
        if binding == "bound":
            alias["sha256"] = functions[address]["sha256"]
        aliases.append(alias)
        if binding == "bound" and source:
            ledger.append({
                "image": image,
                "address": address,
                "sha256": functions[address]["sha256"],
                "status": "SKIP" if item.get("status") == "SKIP" else "WIP",
                "source": source,
                "evidence": [progress_path.relative_to(R).as_posix()],
                "original_name": original,
                "semantic_name": item["semantic_name"],
                "legacy_status": item.get("status"),
                "note": "Imported from the legacy project ledger; current pipeline evidence gates have not promoted this entry to DONE",
            })
    aliases.sort(key=lambda item: (item["image"], item["address"], item["semantic_name"]))
    ledger.sort(key=lambda item: (item["image"], item["address"]))
    structure_records = structures()
    write_json(args.semantic_output, {
        "schema": 1,
        "source": progress_path.relative_to(R).as_posix(),
        "functions": aliases,
        "structures": structure_records,
        "unresolved": unresolved,
    })
    write_json(args.ledger_output, {"schema": 1, "entries": ledger})
    print(json.dumps({"aliases": len(aliases), "ledger": len(ledger), "unresolved": len(unresolved), "structures": len(structure_records)}))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Audit project lockstep markers and a selected ABI frontier"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from collections import defaultdict
from pathlib import Path


CONTRACT = "xport-lockstep-abi-v1"
KINDS = {"scalar", "guest_pointer"}
RETURNS = {"void", "scalar", "guest_pointer"}
MARKER = re.compile(
    r"\bFUNCTION_MARKER(?:_ARGS)?\(\s*0x([0-9a-fA-F]+)u?\s*,\s*\"([^\"]+)\""
)
FUNCTION = re.compile(
    r"^(?:static\s+)?(?:[A-Za-z_]\w*[\s\*]+)+([A-Za-z_]\w*)\s*\([^;]*\)\s*$"
)
HEADER = re.compile(r"entry=([0-9A-Fa-f]+)\s+end=([0-9A-Fa-f]+)")
INSTRUCTION = re.compile(r"^([0-9A-Fa-f]{8})\s+.*?\s(j|jr|jalr)\s+(\S+)")
ENCODED_INSTRUCTION = re.compile(
    r"^([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{2})\s+([0-9A-Fa-f]{2})\s+"
    r"([0-9A-Fa-f]{2})\s+([0-9A-Fa-f]{2})\s+"
)
MIPS_REGISTERS = {"s0": 16, "s1": 17, "s2": 18, "s3": 19,
                  "s4": 20, "s5": 21, "s6": 22, "s7": 23,
                  "a0": 4, "a1": 5, "a2": 6, "a3": 7,
                  "t0": 8, "t1": 9, "t2": 10, "t3": 11, "t4": 12,
                  "t5": 13, "t6": 14, "t7": 15, "t8": 24, "t9": 25,
                  "v0": 2, "v1": 3, "gp": 28, "sp": 29, "ra": 31}


class CoverageError(ValueError):
    pass


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _exact(value: dict, required: set[str], name: str) -> None:
    if not isinstance(value, dict) or set(value) != required:
        raise CoverageError(f"{name} fields must be exactly {sorted(required)}")


def load_registry(path: Path) -> dict:
    value = json.loads(path.read_text(encoding="utf-8"))
    _exact(value, {"schema", "contract", "status", "image", "image_sha256", "frontier",
                   "functions", "trusted_boundaries", "reviewed_tail_calls",
                   "reviewed_indirect_returns", "evidence"}, "registry")
    if value["schema"] != 1 or value["contract"] != CONTRACT or value["status"] != "reviewed":
        raise CoverageError("ABI registry must be reviewed xport-lockstep-abi-v1")
    if not re.fullmatch(r"[0-9a-f]{64}", value["image_sha256"]):
        raise CoverageError("ABI registry image identity is invalid")
    if not isinstance(value["frontier"], str) or not value["frontier"]:
        raise CoverageError("ABI frontier is empty")
    addresses = set()
    symbols = set()
    for index, row in enumerate(value["functions"]):
        _exact(row, {"address", "symbol", "arguments", "return", "evidence"}, f"function {index}")
        address = int(row["address"], 0)
        if address & 3 or address in addresses or row["symbol"] in symbols:
            raise CoverageError("ABI functions must have unique aligned addresses and symbols")
        addresses.add(address)
        symbols.add(row["symbol"])
        if row["return"] not in RETURNS:
            raise CoverageError("Unsupported ABI return kind")
        if not isinstance(row["arguments"], list) or len(row["arguments"]) > 16:
            raise CoverageError("ABI argument count is invalid")
        for position, argument in enumerate(row["arguments"]):
            if not isinstance(argument, dict) or set(argument) not in (
                    {"kind", "bytes"}, {"kind", "bytes", "register"}):
                raise CoverageError("ABI argument fields are invalid")
            if ("register" in argument and argument["register"] not in MIPS_REGISTERS or
                    "register" not in argument and position >= 4):
                raise CoverageError("ABI argument requires a supported original register")
            if argument["kind"] not in KINDS or not isinstance(argument["bytes"], int):
                raise CoverageError("Unsupported ABI argument")
            if argument["kind"] == "guest_pointer" and argument["bytes"] < 1:
                raise CoverageError("Guest pointers require a positive byte extent")
            if argument["kind"] == "scalar" and argument["bytes"] != 0:
                raise CoverageError("Scalars must have a zero byte extent")
    trusted_addresses = set()
    for index, row in enumerate(value["trusted_boundaries"]):
        _exact(row, {"address", "native_symbol", "policy", "evidence"}, f"trusted boundary {index}")
        address = int(row["address"], 0)
        if address & 3 or address in addresses or address in trusted_addresses:
            raise CoverageError("Trusted boundary address is invalid or duplicated")
        if row["policy"] != "trusted_wrapper":
            raise CoverageError("Unknown trusted boundary policy")
        trusted_addresses.add(address)
    tails = set()
    for index, row in enumerate(value["reviewed_tail_calls"]):
        _exact(row, {"source", "target", "evidence"}, f"tail call {index}")
        pair = (int(row["source"], 0), int(row["target"], 0))
        if pair in tails or pair[0] & 3 or pair[1] & 3:
            raise CoverageError("Reviewed tail call is invalid or duplicated")
        tails.add(pair)
    reviewed_returns = set()
    for index, row in enumerate(value["reviewed_indirect_returns"]):
        _exact(row, {"function", "save_pc", "return_pc", "register",
                     "listing_sha256", "evidence"}, f"indirect return {index}")
        function = int(row["function"], 0)
        save_pc = int(row["save_pc"], 0)
        return_pc = int(row["return_pc"], 0)
        if (function, return_pc) in reviewed_returns or function not in addresses:
            raise CoverageError("Reviewed indirect return is duplicated or outside the frontier")
        if save_pc & 3 or return_pc & 3 or not function <= save_pc < return_pc:
            raise CoverageError("Reviewed indirect return addresses are invalid")
        if row["register"] not in MIPS_REGISTERS:
            raise CoverageError("Reviewed indirect return register is invalid")
        if not re.fullmatch(r"[0-9a-f]{64}", row["listing_sha256"]):
            raise CoverageError("Reviewed indirect return listing identity is invalid")
        if not isinstance(row["evidence"], list) or not row["evidence"]:
            raise CoverageError("Reviewed indirect return requires evidence")
        reviewed_returns.add((function, return_pc))
    if not isinstance(value["evidence"], list) or not value["evidence"]:
        raise CoverageError("ABI registry requires evidence")
    return value


def scan_markers(root: Path, source_roots: list[str]) -> list[dict]:
    rows = []
    for source_root in source_roots:
        for path in sorted((root / source_root).rglob("*.c")):
            symbol = None
            for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
                match = FUNCTION.match(line.strip())
                if match:
                    symbol = match.group(1)
                marker = MARKER.search(line)
                if marker:
                    rows.append({
                        "image": marker.group(2), "address": int(marker.group(1), 16),
                        "symbol": symbol, "path": path.relative_to(root).as_posix(), "line": number,
                    })
    return rows


def listing_control(path: Path) -> tuple[list[dict], list[dict]]:
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    header = HEADER.search(lines[0]) if lines else None
    if not header:
        raise CoverageError(f"Listing has no function bounds: {path}")
    start, end = int(header.group(1), 16), int(header.group(2), 16)
    indirect = []
    tails = []
    for line in lines[1:]:
        match = INSTRUCTION.match(line)
        if not match:
            continue
        pc, opcode, operand = int(match.group(1), 16), match.group(2), match.group(3)
        if opcode in {"jr", "jalr"} and "$ra" not in operand:
            indirect.append({"pc": f"0x{pc:08x}", "instruction": line.strip()})
        if opcode == "j":
            target_match = re.search(r"(?:sub_|loc_)?([0-9A-Fa-f]{8})", operand)
            if target_match:
                target = int(target_match.group(1), 16)
                if not start <= target < end:
                    tails.append({"source": pc, "target": target, "instruction": line.strip()})
    return indirect, tails


def reviewed_indirect_return(path: Path, row: dict) -> bool:
    if digest(path) != row["listing_sha256"]:
        return False
    words = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines()[1:]:
        match = ENCODED_INSTRUCTION.match(line)
        if match:
            words[int(match.group(1), 16)] = int.from_bytes(
                bytes(int(match.group(index), 16) for index in range(2, 6)), "little")
    register = MIPS_REGISTERS[row["register"]]
    save_pc = int(row["save_pc"], 0)
    return_pc = int(row["return_pc"], 0)
    return (words.get(save_pc) == (31 << 21) | (register << 11) | 0x21
            and words.get(return_pc) == (register << 21) | 0x08)


def audit(root: Path, config_path: Path, registry_path: Path) -> dict:
    config = json.loads(config_path.read_text(encoding="utf-8"))
    registry = load_registry(registry_path)
    markers = scan_markers(root, config["lockstep"]["source_roots"])
    by_identity = defaultdict(list)
    for marker in markers:
        by_identity[(marker["image"].lower(), marker["address"])].append(marker)
    duplicates = [rows for rows in by_identity.values() if len(rows) > 1]
    functions = []
    errors = []
    reviewed_tails = {(int(row["source"], 0), int(row["target"], 0))
                      for row in registry["reviewed_tail_calls"]}
    reviewed_returns = {(int(row["function"], 0), int(row["return_pc"], 0)): row
                        for row in registry["reviewed_indirect_returns"]}
    for row in registry["functions"]:
        address = int(row["address"], 0)
        candidates = by_identity.get((registry["image"].lower(), address), [])
        listing = root / "orig" / "images" / registry["image"] / "functions" / f"{address:08X}.lst"
        item = {"address": row["address"], "symbol": row["symbol"], "marker": candidates,
                "listing": listing.relative_to(root).as_posix(), "listing_sha256": None,
                "indirect_calls": [], "unreviewed_tail_calls": []}
        if len(candidates) != 1 or candidates[0]["symbol"] != row["symbol"]:
            errors.append(f"missing_boundary_binding:{row['address']}:{row['symbol']}")
        if not listing.is_file():
            errors.append(f"missing_original_listing:{row['address']}")
        else:
            item["listing_sha256"] = digest(listing)
            indirect, tails = listing_control(listing)
            item["indirect_calls"] = [call for call in indirect if not (
                (review := reviewed_returns.get((address, int(call["pc"], 0))))
                and reviewed_indirect_return(listing, review))]
            item["unreviewed_tail_calls"] = [tail for tail in tails
                                               if (tail["source"], tail["target"]) not in reviewed_tails]
            if item["indirect_calls"] or item["unreviewed_tail_calls"]:
                errors.append(f"missing_boundary_binding:{row['address']}:indirect_or_tail")
        functions.append(item)
    if duplicates:
        errors.extend(f"duplicate_marker:{rows[0]['image']}:{rows[0]['address']:08x}"
                      for rows in duplicates)
    return {
        "schema": 1, "contract": "xport-lockstep-coverage-v1",
        "status": "PASS" if not errors else "FAIL", "frontier": registry["frontier"],
        "registry_sha256": digest(registry_path), "marker_count": len(markers),
        "unique_marker_count": len(by_identity), "duplicate_markers": duplicates,
        "functions": functions, "trusted_boundaries": registry["trusted_boundaries"],
        "errors": errors,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--registry", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = args.project.resolve()
    result = audit(root, root / "xport-project.json", (root / args.registry).resolve())
    output = (root / args.output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": result["status"], "markers": result["marker_count"],
                      "unique": result["unique_marker_count"], "errors": result["errors"]}))
    if result["status"] != "PASS":
        raise SystemExit(1)


if __name__ == "__main__":
    main()

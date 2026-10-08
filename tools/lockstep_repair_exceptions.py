"""Evidence-bound memory exceptions supplied with a converge repair"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

from lockstep_contract import validate_exceptions


CONTRACT = "xport-lockstep-repair-exceptions-v1"
ALLOWED_KINDS = {"firmware_runtime", "trusted_device_state"}
REGION_SIZES = {"dram": 0x200000, "scratchpad": 0x400}


def _range_end(row: dict) -> int:
    return row["offset"] + row["size"]


def _overlaps(left: dict, right: dict) -> bool:
    return (left["region"] == right["region"] and
            left["offset"] < _range_end(right) and
            right["offset"] < _range_end(left))


def validate_repair_ranges(rows, packet: dict, compiled: dict) -> list[dict]:
    if not isinstance(rows, list):
        raise ValueError("Repair memory_exceptions must be a list")
    if len(rows) > 32:
        raise ValueError("Repair declares too many memory exceptions")
    normalized = []
    names = set()
    for index, source in enumerate(rows):
        if not isinstance(source, dict) or set(source) != {
                "region", "offset", "size", "kind", "name", "reason"}:
            raise ValueError(f"Invalid repair memory exception {index}")
        region = source["region"]
        offset = source["offset"]
        size = source["size"]
        kind = source["kind"]
        name = source["name"]
        reason = source["reason"]
        if region not in REGION_SIZES or type(offset) is not int or type(size) is not int:
            raise ValueError("Repair memory exception has an invalid range")
        if offset < 0 or size < 1 or offset + size > REGION_SIZES[region]:
            raise ValueError("Repair memory exception is outside guest memory")
        if kind not in ALLOWED_KINDS:
            raise ValueError("Repair memory exception has an invalid classification")
        if (not isinstance(name, str) or not name or len(name) > 80 or
                not all(character.isalnum() or character in "_-" for character in name)):
            raise ValueError("Repair memory exception has an invalid name")
        if name in names:
            raise ValueError("Repair memory exception names must be unique")
        if not isinstance(reason, str) or not reason.strip() or len(reason) > 512:
            raise ValueError("Repair memory exception requires a bounded reason")
        names.add(name)
        normalized.append({"region": region, "offset": offset, "size": size,
                           "kind": kind, "owner": "game", "name": "repair_" + name,
                           "reason": reason.strip()})
    normalized.sort(key=lambda row: (row["region"], row["offset"]))
    for prior, current in zip(normalized, normalized[1:]):
        if _overlaps(prior, current):
            raise ValueError("Repair memory exceptions overlap")
    existing = compiled.get("ranges") if isinstance(compiled, dict) else None
    if not isinstance(existing, list):
        raise ValueError("Attention packet lacks compiled memory exceptions")
    for row in normalized:
        if any(_overlaps(row, prior) for prior in existing):
            raise ValueError("Repair memory exception overlaps a compiled exception")
    comparison = packet.get("comparison") or (packet.get("result") or {}).get("comparison") or {}
    if (comparison.get("region") in REGION_SIZES and
            type(comparison.get("offset")) is int and
            type(comparison.get("length")) is int):
        active = {"region": comparison["region"], "offset": comparison["offset"],
                  "size": comparison["length"]}
        if any(_overlaps(row, active) for row in normalized):
            raise ValueError("Repair memory exception overlaps the active divergence")
    if sum(row["size"] for row in normalized) > 0x10000:
        raise ValueError("Repair memory exceptions exceed the 64 KiB limit")
    return normalized


def activate_repair_ranges(path: Path, rows, packet: dict, source_identity: str) -> dict | None:
    if rows is None:
        return None
    normalized = validate_repair_ranges(
        rows, packet, packet.get("compiled_memory_exceptions") or {})
    if not normalized:
        return None
    failure_signature = packet.get("failure_signature")
    if not isinstance(failure_signature, str) or len(failure_signature) != 64:
        raise ValueError("Attention packet lacks a failure signature")
    current = {"schema": 1, "contract": CONTRACT, "ranges": []}
    if path.is_file():
        current = json.loads(path.read_text(encoding="utf-8"))
        if (not isinstance(current, dict) or current.get("schema") != 1 or
                current.get("contract") != CONTRACT or
                not isinstance(current.get("ranges"), list)):
            raise ValueError("Stored repair memory exceptions are invalid")
    additions = []
    for row in normalized:
        stored = {**row, "source_identity": source_identity,
                  "failure_signature": failure_signature}
        additions.append(stored)
    combined = [*current["ranges"], *additions]
    comparable = [{key: row[key] for key in ("region", "offset", "size")}
                  for row in combined]
    comparable.sort(key=lambda row: (row["region"], row["offset"]))
    for prior, following in zip(comparable, comparable[1:]):
        if _overlaps(prior, following):
            raise ValueError("Stored repair memory exceptions overlap")
    value = {"schema": 1, "contract": CONTRACT, "ranges": combined}
    encoded = (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")
    temporary = path.with_suffix(".tmp")
    temporary.write_bytes(encoded)
    temporary.replace(path)
    return {"path": str(path), "sha256": hashlib.sha256(encoded).hexdigest(),
            "added": additions}


def merge_repair_ranges(compiled: dict, path: Path) -> dict:
    validate_exceptions(compiled)
    if not path.is_file():
        return compiled
    stored = json.loads(path.read_text(encoding="utf-8"))
    if (not isinstance(stored, dict) or stored.get("schema") != 1 or
            stored.get("contract") != CONTRACT or
            not isinstance(stored.get("ranges"), list)):
        raise ValueError("Stored repair memory exceptions are invalid")
    additions = []
    for row in stored["ranges"]:
        if not isinstance(row, dict) or not all(key in row for key in (
                "region", "offset", "size", "kind", "owner", "name", "reason",
                "source_identity", "failure_signature")):
            raise ValueError("Stored repair memory exception is incomplete")
        additions.append({key: row[key] for key in (
            "region", "offset", "size", "kind", "owner", "name")})
    ranges = sorted([*compiled["ranges"], *additions],
                    key=lambda row: (row["region"], row["offset"]))
    merged = {**compiled, "ranges": ranges}
    return validate_exceptions(merged)

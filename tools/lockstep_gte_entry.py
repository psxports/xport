"""Compare paired original and native GTE state at a function entry"""

from __future__ import annotations


def _word(registers: dict, name: str) -> int:
    value = registers["gte_" + name]
    return int(value, 16) if isinstance(value, str) else int(value)


def _matrix(registers: dict, prefix: str) -> list[int]:
    result = []
    for index in range(5):
        value = _word(registers, f"{prefix}_{index}")
        result.append(value & 0xFFFF)
        if index < 4:
            result.append((value >> 16) & 0xFFFF)
    return result


def compare_paired_gte_entry(probe: dict) -> dict:
    entry = probe.get("native_entry") or {}
    native = probe.get("native_gte_at_entry") or {}
    if not entry or not native:
        return {"status": "unpaired", "reason": "entry_snapshot_missing"}
    try:
        address = int(entry["output_address"], 16)
        native_memory = next(row for row in entry["memory"]
                             if row["argument"] == "output_address")
        paired = []
        for hit in probe.get("hits") or []:
            registers = hit.get("registers") or {}
            if (hit.get("kind") != "breakpoint" or
                    int(registers.get("r4", "0"), 16) != address):
                continue
            original_memory = next((row for row in hit.get("memory") or []
                                    if int(row["address"], 16) == address and
                                    row.get("size") == native_memory.get("size")), None)
            if (original_memory is not None and
                    original_memory.get("data") == native_memory.get("data") and
                    hit.get("gte_registers")):
                paired.append(hit)
        if len(paired) != 1:
            return {"status": "unpaired", "reason": "unique_matching_entry_missing",
                    "candidate_count": len(paired)}
        original = paired[0]["gte_registers"]
        fields = {
            "rotation": _matrix(original, "rt"),
            "light": _matrix(original, "llm"),
            "color": _matrix(original, "lcm"),
            "translation": [_word(original, name) for name in ("trx", "try", "trz")],
            "back_color": [_word(original, name) for name in ("rbk", "gbk", "bbk")],
            "h": _word(original, "h"),
            "ofx": _word(original, "ofx"),
            "ofy": _word(original, "ofy"),
            "flag": _word(original, "flag"),
            "sxy": [_word(original, f"sxy{index}") for index in range(3)],
            "sz": [_word(original, f"sz{index}") for index in range(4)],
        }
        candidate = dict(native)
        for name in ("ofx", "ofy"):
            candidate[name] = (int(candidate[name]) << 16) & 0xFFFFFFFF
        differences = {name: {"original": value, "native": candidate[name]}
                       for name, value in fields.items() if value != candidate[name]}
        return {"status": "difference" if differences else "match",
                "input_address": f"0x{address:08X}",
                "fields_compared": list(fields), "differences": differences}
    except (KeyError, TypeError, ValueError, StopIteration):
        return {"status": "unpaired", "reason": "entry_evidence_incomplete"}

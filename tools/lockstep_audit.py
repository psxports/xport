"""Machine-derived MIPS facts for a lockstep function audit"""

from __future__ import annotations

import hashlib
import json
import re


LINE = re.compile(r"^\s*([0-9a-fA-F]{8})\s+((?:[0-9a-fA-F]{2}\s+){4,})")


def words_from_listing(lines: list[str]) -> dict[int, int]:
    words = {}
    for line in lines:
        match = LINE.match(line)
        if not match:
            continue
        address = int(match.group(1), 16)
        data = bytes.fromhex(match.group(2))
        if len(data) % 4:
            raise ValueError("MIPS listing has a partial instruction")
        for offset in range(0, len(data), 4):
            pc = address + offset
            word = int.from_bytes(data[offset:offset + 4], "little")
            if pc in words and words[pc] != word:
                raise ValueError("MIPS listing has conflicting instruction bytes")
            words[pc] = word
    if not words:
        raise ValueError("MIPS listing has no instruction words")
    return words


def instruction_kinds(word: int) -> tuple[str, ...]:
    opcode = word >> 26
    if opcode == 1 and ((word >> 16) & 31) in {16, 17}:
        return ("call",)
    if opcode == 3 or (opcode == 0 and (word & 63) == 9):
        return ("call",)
    if (opcode in {1, 2, 4, 5, 6, 7} or
            (opcode == 0 and (word & 63) == 8) or
            (opcode in {0x10, 0x11, 0x12, 0x13} and ((word >> 21) & 31) == 8)):
        return ("control",)
    if opcode in {0x28, 0x29, 0x2a, 0x2b, 0x2e, 0x38, 0x39, 0x3a, 0x3b}:
        return ("store",)
    if opcode in {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,
                  0x30, 0x31, 0x32, 0x33}:
        return ("load",)
    return ()


def _register_effects(word: int) -> tuple[set[int], set[int]] | None:
    opcode = word >> 26
    rs, rt, rd = (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31
    if opcode == 0:
        function = word & 63
        if function in {0, 2, 3, 4, 6, 7}:
            reads = {rt} if function in {0, 2, 3} else {rs, rt}
            return reads, {rd}
        if function in {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2a, 0x2b}:
            return {rs, rt}, {rd}
        if function in {0x10, 0x12}:
            return set(), {rd}
        if function in {0x11, 0x13}:
            return {rs}, set()
        return None
    if opcode in {8, 9, 10, 11, 12, 13, 14, 15}:
        return ({rs} if opcode != 15 else set()), {rt}
    if opcode in {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26}:
        return {rs}, {rt}
    if opcode in {0x28, 0x29, 0x2a, 0x2b, 0x2e}:
        return {rs, rt}, set()
    return None


def checklist(lines: list[str]) -> dict:
    words = words_from_listing(lines)
    checks = []
    missing_delay_slots = []
    for pc, word in sorted(words.items()):
        for kind in instruction_kinds(word):
            checks.append({"id": f"0x{pc:08x}:{kind}", "pc": f"0x{pc:08x}",
                           "kind": kind, "word": f"0x{word:08x}"})
            if kind in {"call", "control"}:
                if pc + 4 in words:
                    delay = words[pc + 4]
                    checks.append({"id": f"0x{pc + 4:08x}:delay", "pc": f"0x{pc + 4:08x}",
                                   "kind": "delay", "word": f"0x{delay:08x}"})
                else:
                    missing_delay_slots.append(f"0x{pc + 4:08x}")
            if kind == "call":
                written = set()
                for step in range(2, 14):
                    later_pc = pc + 4 * step
                    later = words.get(later_pc)
                    if later is None or any(item in {"call", "control"}
                                            for item in instruction_kinds(later)):
                        break
                    effects = _register_effects(later)
                    if effects is None:
                        break
                    reads, writes = effects
                    for register in sorted(({4, 5, 6, 7} & reads) - written):
                        checks.append({"id": f"0x{later_pc:08x}:carrier:a{register - 4}",
                                       "pc": f"0x{later_pc:08x}", "kind": "register_carrier",
                                       "register": f"a{register - 4}",
                                       "call_pc": f"0x{pc:08x}",
                                       "word": f"0x{later:08x}"})
                    written.update(writes)
    if missing_delay_slots:
        raise ValueError("MIPS listing misses delay slots: " + ",".join(missing_delay_slots))
    checks.sort(key=lambda item: (item["pc"], item["kind"]))
    encoded = json.dumps(checks, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return {"schema": 1, "contract": "xport-lockstep-machine-audit-v1",
            "instruction_count": len(words), "checks": checks,
            "sha256": hashlib.sha256(encoded).hexdigest()}


def receipt_template(packet: dict, source_identity: str, changed_files: set[str]) -> dict:
    context = packet.get("source_context") or {}
    contexts = [context, *(context.get("audit_cluster") or [])]
    if (context.get("writer_library") or {}).get("audit_source"):
        contexts.append(context["writer_library"])
    by_function = {
        ((item.get("function") or {}).get("image"),
         (item.get("function") or {}).get("address")): item
        for item in contexts if isinstance(item, dict)
    }
    audits = []
    required = (context.get("audit_contract") or {}).get("required_functions") or []
    for binding in required:
        audit = {**binding, "pseudocode_used_as_baseline": False,
                 "complete_mips_audit": False, "unresolved_semantics": ["Review required"]}
        detail = by_function.get((binding.get("image"), binding.get("address"))) or {}
        checks = (detail.get("machine_audit") or {}).get("checks") or []
        implementation = detail.get("audit_source") or detail.get("implementation") or {}
        audit["machine_coverage"] = [
            {"check_ids": [item["id"] for item in checks[index:index + 8]],
             "source_file": implementation.get("path"),
             "source_symbol": implementation.get("symbol"), "source_line": None}
            for index in range(0, len(checks), 8)
        ]
        audits.append(audit)
    receipt = {
        "schema": 1, "contract": "xport-lockstep-function-audit-v1",
        "failure_signature": packet["failure_signature"],
        "source_identity": source_identity,
        "changed_files": sorted(changed_files),
        "function_audits": audits,
    }
    shared = (packet.get("identities") or {}).get("shared_source_identity")
    if shared is not None:
        receipt["shared_source_identity"] = shared
    return receipt

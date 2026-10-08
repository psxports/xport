"""Verify the narrow legacy two-word listing parser correction"""

import hashlib
import re

from lockstep_audit import checklist, words_from_listing


LEGACY_LINE = re.compile(r"^\s*([0-9a-fA-F]{8})\s+((?:[0-9a-fA-F]{2}\s+){4,8})")


def verify_machine_refresh(previous, old_bindings, bindings, contexts, state):
    if any(row.get("proof_status") == "proved" for row in state["functions"]):
        raise ValueError("Machine refresh requires zero accepted coverage proofs")
    old_context = previous["source_context"]
    old_contexts = [old_context, *(old_context.get("audit_cluster") or [])]
    if not (len(old_contexts) == len(contexts) == len(bindings) == len(old_bindings)):
        raise ValueError("Machine refresh changed batch size")
    changes = []
    for old, new, old_binding, binding in zip(old_contexts, contexts, old_bindings, bindings):
        strip = lambda row: {k: v for k, v in row.items() if k != "machine_audit_sha256"}
        if strip(old_binding) != strip(binding):
            raise ValueError("Machine refresh changed original evidence or batch order")
        if old["mips"] != new["mips"] or old["mips"].get("complete") is not True:
            raise ValueError("Machine refresh requires identical complete original listings")
        lines = old["mips"]["lines"]
        corrected = checklist(lines)
        if new["machine_audit"] != corrected or binding["machine_audit_sha256"] != corrected["sha256"]:
            raise ValueError("Corrected machine checklist does not match current derivation")
        full_words = words_from_listing(lines)
        raw = b"".join(word.to_bytes(4, "little") for _, word in sorted(full_words.items()))
        if hashlib.sha256(raw).hexdigest() != binding["function_sha256"]:
            raise ValueError("Machine refresh does not cover the original function bytes")
        legacy_lines = []
        for line in lines:
            match = LEGACY_LINE.match(line)
            if match:
                address = int(match[1], 16)
                data = bytes.fromhex(match[2])
                for offset in range(0, len(data), 4):
                    legacy_lines.append(f"{address + offset:08x} " + data[offset:offset + 4].hex(" ") + " ")
        legacy = checklist(legacy_lines)
        if old["machine_audit"] != legacy or old_binding["machine_audit_sha256"] != legacy["sha256"]:
            raise ValueError("Previous checklist is not the exact legacy parser derivation")
        if legacy != corrected:
            old_checks = {c["id"]: c for c in legacy["checks"]}
            changes.append({"image": binding["image"], "address": binding["address"],
                            "previous_sha256": legacy["sha256"], "corrected_sha256": corrected["sha256"],
                            "previous_instruction_count": legacy["instruction_count"],
                            "corrected_instruction_count": corrected["instruction_count"],
                            "added_or_changed_check_ids": [c["id"] for c in corrected["checks"] if old_checks.get(c["id"]) != c]})
    if not changes:
        raise ValueError("No legacy machine-parser correction is required")
    return {"schema": 1, "reason": "Read all original words in IDA pseudo-instruction lines",
            "accepted_proofs_reused": False, "changes": changes}

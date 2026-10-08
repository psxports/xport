"""Bind a specific direct SDK dependency without expanding recorded coverage"""

import hashlib
import json
import sqlite3

from lockstep_audit import checklist, words_from_listing


def build_sdk_dependencies(root, project, contexts, requests, budget):
    from source_context import compact
    known = {(c["function"]["image"], c["function"]["address"]): c for c in contexts}
    result = []
    seen = set()
    with sqlite3.connect((root / project["paths"]["analysis_database"]).as_uri() + "?mode=ro", uri=True) as db:
        db.row_factory = sqlite3.Row
        for request in requests:
            if (not isinstance(request, dict) or set(request) != {"image", "caller_address", "address", "effect"} or
                    type(request["caller_address"]) is not int or type(request["address"]) is not int):
                raise ValueError("Invalid SDK dependency request")
            caller_key = (request["image"], request["caller_address"])
            target_key = (request["image"], request["address"])
            if caller_key not in known or target_key in seen:
                raise ValueError("SDK dependency caller is outside the packet or dependency is duplicated")
            callee = compact(*target_key, budget, full_source=True, full_original=True)
            classification = [dict(row) for row in db.execute(
                "SELECT alias,reason,evidence FROM library_classification WHERE image=? AND address=?", target_key)]
            binding = bind_sdk_dependency(known[caller_key], callee, classification, request["effect"])
            callee["machine_audit"] = binding["machine_audit"]
            result.append({"binding": binding, "context": callee})
            known[target_key] = callee
            seen.add(target_key)
    return result


def bind_sdk_dependency(caller, callee, classification, effect):
    if not isinstance(effect, str) or not effect.strip():
        raise ValueError("SDK dependency needs a specific unproved caller effect")
    owner = caller["function"]
    function = callee["function"]
    if function["image"] != owner["image"]:
        raise ValueError("SDK dependency belongs to another image")
    if function.get("status") != "SKIP" or not classification:
        raise ValueError("SDK dependency requires reviewed library classification")
    if not all(isinstance(row, dict) and row.get("alias") and row.get("reason") and row.get("evidence")
               for row in classification):
        raise ValueError("SDK dependency classification is incomplete")
    for context in (caller, callee):
        if context["mips"].get("complete") is not True:
            raise ValueError("SDK dependency requires complete original MIPS")
        words = words_from_listing(context["mips"]["lines"])
        start = context["function"]["address"]
        end = context["function"].get("end", max(words) + 4)
        if set(words) != set(range(start, end, 4)):
            raise ValueError("SDK dependency MIPS range is incomplete or belongs to another address")
        raw = b"".join(word.to_bytes(4, "little") for _, word in sorted(words.items()))
        if hashlib.sha256(raw).hexdigest() != context["function"]["sha256"]:
            raise ValueError("SDK dependency original bytes do not match function identity")
    for row in classification:
        try:
            evidence = json.loads(row["evidence"])
        except (TypeError, ValueError):
            raise ValueError("SDK dependency classification has no bound JSON evidence") from None
        if (not isinstance(evidence, dict) or evidence.get("image") != function["image"] or
                evidence.get("address") != function["address"] or
                evidence.get("sha256") != function["sha256"] or
                evidence.get("alias") != row["alias"]):
            raise ValueError("SDK dependency classification identity does not match original")
    sites = []
    for pc, word in words_from_listing(caller["mips"]["lines"]).items():
        if word >> 26 in (2, 3):
            target = ((pc + 4) & 0xF0000000) | ((word & 0x3FFFFFF) << 2)
            if target == function["address"]:
                sites.append(pc)
    if not sites:
        raise ValueError("SDK dependency has no bound direct caller edge")
    pseudo = callee["pseudocode"]
    if pseudo.get("path") and pseudo.get("complete") is not True:
        raise ValueError("SDK dependency pseudocode payload is incomplete")
    machine = checklist(callee["mips"]["lines"])
    return {"image": function["image"], "address": function["address"],
            "caller_address": owner["address"], "caller_function_sha256": owner["sha256"],
            "callsite_addresses": sorted(sites), "specific_unproved_effect": effect.strip(),
            "function_sha256": function["sha256"], "mips_sha256": callee["mips"]["sha256"],
            "pseudocode_sha256": pseudo.get("sha256"),
            "pseudocode_complete": bool(pseudo.get("path") and pseudo.get("complete")),
            "machine_audit_sha256": machine["sha256"], "machine_audit": machine,
            "library_classification": classification, "complete_mips_audit": False,
            "recorded_denominator_expanded": False}

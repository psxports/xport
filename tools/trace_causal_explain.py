"""Build a bounded causal capsule from immutable mismatch evidence"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import tempfile


CONTRACT = "xport-causal-request-v1"
CAPSULE = "xport-causal-capsule-v1"
REGIONS = {"ram": 1, "vram": 2, "scratchpad": 3, "spu_ram": 4}


class CausalError(ValueError):
    """Raised when causal evidence violates its contract"""


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")


def value_hash(value):
    return hashlib.sha256(canonical(value)).hexdigest()


def file_hash(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = json.dumps(value, indent=2, sort_keys=True) + "\n"
    handle, temporary = tempfile.mkstemp(prefix=path.name + ".", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(handle, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def _require_keys(value, keys, label):
    if not isinstance(value, dict) or not set(keys) <= set(value):
        raise CausalError(f"{label} is missing required fields")


def validate_request(request, base=None):
    _require_keys(request, {"schema", "contract", "mismatch", "identities", "budgets", "evidence"}, "request")
    if request["schema"] != 1 or request["contract"] != CONTRACT:
        raise CausalError("Unsupported causal request")
    mismatch = request["mismatch"]
    _require_keys(mismatch, {"id", "kind", "frame", "reproduced"}, "mismatch")
    if mismatch["reproduced"] is not True or not isinstance(mismatch["id"], str) or not mismatch["id"]:
        raise CausalError("Mismatch must be reproduced and identified")
    if mismatch["kind"] == "memory":
        _require_keys(mismatch, {"region", "offset", "size", "original", "native"}, "memory mismatch")
        if mismatch["region"] not in REGIONS or type(mismatch["offset"]) is not int or type(mismatch["size"]) is not int or mismatch["size"] < 1:
            raise CausalError("Invalid memory mismatch range")
    elif mismatch["kind"] not in ("call", "missing_call", "missing_store", "ot_consumption", "runtime_failure"):
        raise CausalError("Unsupported mismatch kind")
    identities = request["identities"]
    if not isinstance(identities, dict) or not identities or any(not isinstance(key, str) or not isinstance(value, str) or not value for key, value in identities.items()):
        raise CausalError("Immutable identities are required")
    budgets = request["budgets"]
    expected = {"max_passes", "max_writers", "max_definitions", "max_requests"}
    if not isinstance(budgets, dict) or set(budgets) != expected or any(type(budgets[key]) is not int or budgets[key] < 1 for key in expected):
        raise CausalError("Invalid causal budgets")
    if budgets["max_passes"] > 3:
        raise CausalError("Causal explain supports at most three passes")
    if not isinstance(request["evidence"], dict):
        raise CausalError("Evidence must be an object")
    base = Path(base or ".").resolve()
    for item in request.get("artifacts", []):
        _require_keys(item, {"path", "sha256"}, "artifact identity")
        path = (base / item["path"]).resolve() if not Path(item["path"]).is_absolute() else Path(item["path"]).resolve()
        if not path.is_file() or file_hash(path) != item["sha256"]:
            raise CausalError("Causal artifact identity changed: " + item["path"])


def decode_def_use(word):
    """Return exact register def/use for the supported MIPS subset"""
    word &= 0xFFFFFFFF
    opcode = word >> 26
    rs, rt, rd = (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31
    funct = word & 63
    if word == 0:
        return {"known": True, "defines": [], "uses": []}
    if opcode == 0:
        if funct in (0, 2, 3):
            return {"known": True, "defines": [rd] if rd else [], "uses": [rt]}
        if funct in (4, 6, 7):
            return {"known": True, "defines": [rd] if rd else [], "uses": [rs, rt]}
        if funct in (8,):
            return {"known": True, "defines": [], "uses": [rs], "control": True}
        if funct in (9,):
            return {"known": True, "defines": [rd or 31], "uses": [rs], "control": True}
        if funct in (16, 18):
            return {"known": True, "defines": [rd] if rd else [], "uses": []}
        if funct in (17, 19):
            return {"known": True, "defines": [], "uses": [rs]}
        if funct in (24, 25, 26, 27):
            return {"known": True, "defines": [], "uses": [rs, rt]}
        if funct in (32, 33, 34, 35, 36, 37, 38, 39, 42, 43):
            return {"known": True, "defines": [rd] if rd else [], "uses": [rs, rt]}
        return {"known": False, "defines": [], "uses": []}
    if opcode in (2, 3):
        return {"known": True, "defines": [31] if opcode == 3 else [], "uses": [], "control": True}
    if opcode in (4, 5, 6, 7):
        uses = [rs] + ([rt] if opcode in (4, 5) else [])
        return {"known": True, "defines": [], "uses": uses, "control": True}
    if opcode == 1:
        return {"known": True, "defines": [31] if rt in (16, 17) else [], "uses": [rs], "control": True}
    if opcode == 15:
        return {"known": True, "defines": [rt] if rt else [], "uses": []}
    if opcode in (8, 9, 10, 11, 12, 13, 14):
        return {"known": True, "defines": [rt] if rt else [], "uses": [rs]}
    if opcode in (32, 33, 34, 35, 36, 37, 38):
        return {"known": True, "defines": [rt] if rt else [], "uses": [rs], "memory_read": True}
    if opcode in (40, 41, 42, 43, 46):
        return {"known": True, "defines": [], "uses": [rs, rt], "memory_write": True}
    return {"known": False, "defines": [], "uses": []}


def targeted_definitions(events, register, limit):
    needed = {int(register)}
    selected = []
    unknown = None
    for event in reversed(events):
        if not needed or len(selected) >= limit:
            break
        semantics = event.get("semantics")
        if semantics is None and "instruction" in event:
            semantics = decode_def_use(int(event["instruction"]))
        if not isinstance(semantics, dict) or semantics.get("known") is not True:
            if needed:
                unknown = {"pc": event.get("pc"), "instruction": event.get("instruction")}
            break
        definitions = set(semantics.get("defines", []))
        hit = definitions & needed
        if not hit:
            continue
        selected.append({"pc": event.get("pc"), "instruction": event.get("instruction"),
                         "defines": sorted(hit), "uses": list(semantics.get("uses", [])),
                         "memory_read": bool(semantics.get("memory_read")),
                         "value_changed": event.get("value_changed")})
        needed -= hit
        needed.update(reg for reg in semantics.get("uses", []) if reg)
    selected.reverse()
    return {"definitions": selected, "unresolved_registers": sorted(needed), "unknown": unknown,
            "bounded": len(selected) >= limit and bool(needed)}


def _request(kind, reason, **target):
    value = {"kind": kind, "reason": reason, "target": target}
    value["request_hash"] = value_hash(value)
    return value


def _dedupe_requests(requests, evidence_hashes, journal, maximum):
    previous = set()
    if journal and Path(journal).is_file():
        prior = json.loads(Path(journal).read_text(encoding="utf-8"))
        previous = {(item.get("request_hash"), item.get("evidence_hash")) for item in prior.get("requests", [])}
    output = []
    for item in requests:
        evidence_hash = evidence_hashes.get(item["request_hash"])
        duplicate = (item["request_hash"], evidence_hash) in previous
        output.append(dict(item, evidence_hash=evidence_hash, execute=not duplicate,
                           disposition="deduplicated" if duplicate else "requested"))
        if len(output) >= maximum:
            break
    return output


def explain(request, journal=None, base=None):
    validate_request(request, base)
    mismatch = request["mismatch"]
    evidence = request["evidence"]
    budgets = request["budgets"]
    passes = []
    candidates = []
    unresolved = []
    requests = []

    writers = evidence.get("writers", {})
    original = writers.get("original", [])[:budgets["max_writers"]]
    native = writers.get("native", [])[:budgets["max_writers"]]
    pass1 = {"pass": 1, "kind": "exact_bytes_to_writers", "original": original, "native": native}
    if mismatch["kind"] == "memory" and (not original or not native):
        missing = "original" if not original else "native"
        requests.append(_request("last_writer", "No exact writer covers the first differing byte",
                                 side=missing, region=mismatch["region"], offset=mismatch["offset"], size=mismatch["size"]))
    calls = evidence.get("calls", {})
    left_call, right_call = calls.get("original"), calls.get("native")
    if left_call and right_call and left_call.get("function") == right_call.get("function"):
        equal_inputs = left_call.get("inputs") == right_call.get("inputs")
        differing_effects = left_call.get("effects") != right_call.get("effects")
        if equal_inputs and differing_effects:
            candidates.append({"kind": "callee_or_runtime", "function": left_call["function"],
                               "evidence_strength": "candidate", "reason": "equal_observed_inputs_different_effects",
                               "limitation": "hidden state may differ"})
            pass1["call_relation"] = "equal_inputs_different_effects"
        elif not equal_inputs:
            left_inputs, right_inputs = left_call.get("inputs", []), right_call.get("inputs", [])
            ordinal = next((index for index in range(max(len(left_inputs), len(right_inputs)))
                            if (left_inputs[index] if index < len(left_inputs) else None) !=
                               (right_inputs[index] if index < len(right_inputs) else None)), None)
            names = left_call.get("input_names", right_call.get("input_names", []))
            candidates.append({"kind": "call_input", "function": left_call["function"],
                               "evidence_strength": "exact_call", "ordinal": ordinal,
                               "name": names[ordinal] if ordinal is not None and ordinal < len(names) else None,
                               "original": left_inputs[ordinal] if ordinal is not None and ordinal < len(left_inputs) else None,
                               "native": right_inputs[ordinal] if ordinal is not None and ordinal < len(right_inputs) else None})
            pass1["call_relation"] = "differing_inputs"
    ot = evidence.get("ot_consumption")
    if mismatch["kind"] == "ot_consumption" and isinstance(ot, dict) and ot.get("native_submit") and ot.get("original_absent") is True:
        candidates.append({"kind": "control_flow_or_submission", "function": ot.get("function"),
                           "callsite": ot.get("callsite"), "operation": ot.get("operation"),
                           "evidence_strength": "exact_event",
                           "reason": "native_submission_absent_from_original_frame"})
        pass1["ot_relation"] = "extra_native_submission"
    failure = evidence.get("runtime_failure")
    if mismatch["kind"] == "runtime_failure" and isinstance(failure, dict) and failure.get("observed") is True:
        candidates.append({"kind": "failing_native_boundary", "function": failure.get("function"),
                           "source": failure.get("source"), "line": failure.get("line"),
                           "fallback": failure.get("fallback"), "inputs": failure.get("inputs", []),
                           "evidence_strength": "exact_event",
                           "reason": "reproduced_native_failure_at_boundary",
                           "limitation": "the corrupt input may originate earlier"})
        pass1["runtime_relation"] = "reproduced_boundary_failure"
    if original and native:
        left, right = original[-1], native[-1]
        if left.get("writer_kind") == right.get("writer_kind") == "cpu" and left.get("function") == right.get("function"):
            candidates.append({"kind": "writer_function", "function": left.get("function"),
                               "evidence_strength": "exact_writer", "original_pc": left.get("pc"),
                               "native_source": right.get("source")})
        elif left != right:
            candidates.append({"kind": "writer_divergence", "evidence_strength": "exact_writer",
                               "original": left, "native": right})
    passes.append(pass1)

    if budgets["max_passes"] >= 2:
        pass2 = {"pass": 2, "kind": "follow_data_or_missing_event", "observations": []}
        for difference in evidence.get("input_differences", []):
            kind = difference.get("kind")
            if kind == "pointee":
                requests.append(_request("last_writer", "Pointer target differs before the call",
                                         side=difference.get("side", "both"), region=difference["region"],
                                         offset=difference["offset"], size=difference["size"]))
            elif kind == "copied_bad_value":
                requests.append(_request("source_read", "Destination copied an already differing source",
                                         invocation=difference.get("invocation"), source=difference["source"],
                                         size=difference["size"]))
        if mismatch["kind"] in ("missing_call", "missing_store") or evidence.get("missing_event"):
            missing = evidence.get("missing_event", {"kind": mismatch["kind"]})
            requests.append(_request("call_branch", "Expected call or store is absent",
                                     invocation=missing.get("invocation"), callsite=missing.get("callsite"),
                                     branch_pc=missing.get("branch_pc")))
        pass2["observations"] = evidence.get("followups", [])
        passes.append(pass2)

    if budgets["max_passes"] >= 3:
        definition = evidence.get("definition_target")
        if definition:
            sliced = targeted_definitions(evidence.get("definition_events", []), definition["register"],
                                          budgets["max_definitions"])
            passes.append({"pass": 3, "kind": "targeted_definitions", "target": definition, **sliced})
            if sliced["unknown"]:
                requests.append(_request("instruction_semantics", "Unknown instruction blocks the exact def/use chain",
                                         **sliced["unknown"], register=definition["register"]))
            elif sliced["unresolved_registers"]:
                requests.append(_request("preceding_state", "Definition chain reaches the start of observed state",
                                         registers=sliced["unresolved_registers"], before_pc=definition.get("before_pc")))
        else:
            passes.append({"pass": 3, "kind": "targeted_definitions", "status": "not_required"})

    evidence_hashes = {item["request_hash"]: value_hash(evidence.get("responses", {}).get(item["request_hash"]))
                       for item in requests if item["request_hash"] in evidence.get("responses", {})}
    issued = _dedupe_requests(requests, evidence_hashes, journal, budgets["max_requests"])
    unresolved.extend({"kind": "missing_observation", "request": item} for item in issued if item["execute"])
    actionable = bool(candidates) and any(item["evidence_strength"] in ("exact_writer", "exact_call", "exact_event", "candidate") for item in candidates)
    request_identity = value_hash(request)
    capsule = {
        "schema": 1, "contract": CAPSULE, "status": "actionable" if actionable else "missing_observation",
        "request_hash": request_identity, "mismatch": mismatch, "identities": request["identities"],
        "reproduced_failure": True, "passes": passes, "causal_candidates": candidates,
        "source_snippets": evidence.get("source_snippets", []), "unresolved_assumptions": unresolved,
        "requests": issued, "suggested_bench_boundary": evidence.get("bench_boundary"),
        "budgets": budgets,
        "limitations": ["A last writer is not by itself a root cause", "Equal observed inputs do not exclude hidden state",
                        "Names and historical comments are never causal evidence"],
    }
    capsule["capsule_hash"] = value_hash(capsule)
    return capsule


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--request", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--journal", type=Path)
    args = parser.parse_args()
    request = json.loads(args.request.read_text(encoding="utf-8"))
    result = explain(request, args.journal, args.request.parent)
    atomic_json(args.output, result)
    print(json.dumps({"status": result["status"], "capsule_hash": result["capsule_hash"],
                      "output": str(args.output.resolve())}, separators=(",", ":")))


if __name__ == "__main__":
    main()

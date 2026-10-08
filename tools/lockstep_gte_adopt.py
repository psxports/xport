"""Attach hash-bound GTE entry equivalence to a divergence packet"""

from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import re

from lockstep_gte_entry import compare_paired_gte_entry
from lockstep_paired_entry import _address, _evidence


def read_gte_entry_request(directory: Path, request: dict, previous: dict) -> dict:
    required = {"kind", "diagnostic_file", "sha256", "image", "owner_function",
                "native_symbol"}
    if set(request) != required or request.get("kind") != "gte_entry":
        raise ValueError("Invalid GTE entry request")
    probe = _evidence(directory, request["diagnostic_file"], request["sha256"])
    image = request["image"]
    owner = _address(request["owner_function"])
    symbol = request["native_symbol"]
    if (not isinstance(image, str) or not image or len(image) > 40 or
            not isinstance(symbol, str) or
            not re.fullmatch(r"[A-Za-z_]\w{0,100}" + f"{owner:08X}",
                             symbol, re.IGNORECASE)):
        raise ValueError("GTE entry owner identity is invalid")
    current = previous.get("expected_original_event") or {}
    matched = previous.get("last_matching_boundary") or {}
    prior = matched.get("original") or {}
    frame = current.get("frame")
    if (type(frame) is not int or current.get("kind") != "POST_CALL" or
            current.get("image") != image or
            str(current.get("function", "")).lower() != f"0x{owner:08x}" or
            prior.get("kind") != "PRE_CALL" or
            any(prior.get(key) != current.get(key) for key in
                ("frame", "function", "invocation")) or
            matched.get("memory_compared") is not True or
            (matched.get("comparison") or {}).get("status") != "match" or
            probe.get("target_frame") != frame - 1 or
            (probe.get("alignment") or {}).get("aligned") is not True or
            ((probe.get("alignment") or {}).get("comparison") or {}).get("status") != "match"):
        raise ValueError("GTE entry evidence is not bound to a matched call entry")
    entry = probe.get("native_entry") or {}
    inputs = prior.get("arguments") or []
    if (entry.get("symbol") != symbol or not inputs or
            int(entry.get("output_address", "0"), 16) != inputs[0]):
        raise ValueError("GTE entry first argument differs from the matched PRE_CALL")
    hits = [hit for hit in probe.get("hits") or [] if isinstance(hit, dict) and
            str(hit.get("pc", "")).lower() == f"0x{owner:08x}" and
            int((hit.get("registers") or {}).get("r4", "0"), 16) == inputs[0]]
    if len(hits) != 1:
        raise ValueError("GTE entry original function hit is absent or ambiguous")
    comparison = compare_paired_gte_entry(probe)
    if comparison.get("status") != "match":
        raise ValueError("GTE entry state differs or is not uniquely paired")
    evidence = {
        "diagnostic_file": request["diagnostic_file"], "sha256": request["sha256"],
        "frame": frame, "function": f"0x{owner:08x}",
        "input_address": comparison["input_address"],
        "fields_compared": comparison["fields_compared"],
        "status": "match",
    }
    packet = copy.deepcopy(previous)
    packet["gte_entry_provenance"] = evidence
    packet["cause_statement"] = (
        "Full RAM and the sampled GTE state match at this function entry; audit the "
        "function's MIPS, C implementation and called GTE helpers for the output difference"
    )
    packet["failure_signature"] = hashlib.sha256(json.dumps({
        "prior": previous["failure_signature"], "evidence": evidence,
    }, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    packet["serialized_bytes"] = len(json.dumps(
        packet, sort_keys=True, separators=(",", ":")).encode())
    if packet["serialized_bytes"] > packet["budget_bytes"]:
        raise ValueError("GTE entry packet exceeds its byte budget")
    return packet

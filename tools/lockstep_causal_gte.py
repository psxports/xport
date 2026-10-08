"""Redirect a RAM divergence to the function that first changed GTE state"""

from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path

from lockstep_audit import words_from_listing
from lockstep_packet import build_packet
from source_context import compact


LIGHT_NAMES = tuple(f"gte_llm_{index}" for index in range(5))


def _original_light(snapshot: dict) -> list[int]:
    control = snapshot.get("original_gte_control_at_alignment") or {}
    if any(name not in control for name in LIGHT_NAMES):
        raise ValueError("Original GTE light matrix is incomplete")
    words = [int(control[name], 16) for name in LIGHT_NAMES]
    return [part for word in words for part in (word & 0xffff, word >> 16)][:9]


def _native_light(snapshot: dict) -> list[int]:
    values = (snapshot.get("native_gte_at_alignment") or {}).get("light")
    if not isinstance(values, list) or len(values) != 9 or any(
            type(value) is not int or value < 0 or value > 0xffff for value in values):
        raise ValueError("Native GTE light matrix is incomplete")
    return values


def validate_gte_transition(pre: dict, post: dict) -> dict:
    boundaries = []
    for snapshot, kind in ((pre, "PRE_CALL"), (post, "POST_CALL")):
        alignment = snapshot.get("alignment") or {}
        matched = alignment.get("last_step") or {}
        original = matched.get("original") or {}
        native = matched.get("native") or {}
        if (alignment.get("aligned") is not True or
                (alignment.get("comparison") or {}).get("status") != "match" or
                matched.get("status") != "matched" or
                original.get("kind") != kind or native.get("kind") != kind or
                original.get("function") != native.get("function") or
                original.get("frame") != native.get("frame")):
            raise ValueError("GTE transition needs matched PRE/POST boundaries")
        boundaries.append(original)
    first, last = boundaries
    for key in ("frame", "image", "function", "callsite", "target", "invocation"):
        if first.get(key) != last.get(key):
            raise ValueError("GTE transition crosses function invocations")
    if first.get("event_sequence", -1) >= last.get("event_sequence", -1):
        raise ValueError("GTE transition boundaries are out of order")
    original_pre, native_pre = _original_light(pre), _native_light(pre)
    original_post, native_post = _original_light(post), _native_light(post)
    if original_pre != native_pre or original_post == native_post:
        raise ValueError("GTE transition is not the first proven light-matrix divergence")
    return {
        "function": first["function"], "frame": first["frame"],
        "callsite": first["callsite"], "invocation": first["invocation"],
        "pre_event": first["event_sequence"], "post_event": last["event_sequence"],
        "original_pre": original_pre, "native_pre": native_pre,
        "original_post": original_post, "native_post": native_post,
    }


def _verify_excluded_pointer_inputs(pre: dict, exceptions: dict) -> None:
    original = ((pre.get("alignment") or {}).get("last_step") or {}).get("original") or {}
    original_reads = pre.get("original_memory_at_alignment") or []
    native_reads = pre.get("native_memory_at_alignment") or []
    for argument in original.get("arguments") or []:
        if type(argument) is not int:
            continue
        if 0x1F800000 <= argument < 0x1F800400:
            region, offset = "scratchpad", argument - 0x1F800000
        else:
            region, offset = "dram", argument & 0x1FFFFF
        excluded = [row for row in exceptions.get("ranges") or []
                    if row.get("region") == region and
                    row.get("offset", 0) <= offset <
                    row.get("offset", 0) + row.get("size", 0)]
        if not excluded:
            continue
        minimum = min(32, min(row["offset"] + row["size"] - offset
                              for row in excluded))
        address = f"0x{argument:08X}"
        original_rows = [row for row in original_reads if row.get("address") == address and
                         row.get("size", 0) >= minimum]
        native_rows = [row for row in native_reads if row.get("address") == address and
                       row.get("size", 0) >= minimum]
        if (len(original_rows) != 1 or len(native_rows) != 1 or
                original_rows[0].get("data") != native_rows[0].get("data")):
            raise ValueError("Excluded pointer input was not proven equal at PRE_CALL")


def build_causal_packet(previous: dict, divergence: dict, pre: dict, post: dict,
                        writer: dict) -> dict:
    evidence = validate_gte_transition(pre, post)
    _verify_excluded_pointer_inputs(pre, previous["compiled_memory_exceptions"])
    if (evidence["frame"] != (divergence.get("original") or {}).get("frame") or
            (divergence.get("comparison") or {}).get("status") != "difference"):
        raise ValueError("GTE transition does not precede the RAM divergence")
    hits = writer.get("hits") or []
    if len(hits) != 1 or not isinstance(hits[0].get("pc"), str):
        raise ValueError("Original GTE writer instruction is unverified")
    writer_pc = int(hits[0]["pc"], 16)
    listing = ((previous.get("source_context") or {}).get("mips") or {}).get("lines") or []
    word = words_from_listing(listing).get(writer_pc)
    if word is None or word >> 26 != 0x3A:
        raise ValueError("GTE writer instruction is not a listed SWC2")
    refinement = copy.deepcopy(previous["refinement"])
    writes = refinement.get("original_writes") or []
    if not writes:
        raise ValueError("Original RAM writer is unavailable")
    writes[0].update(pc=f"0x{writer_pc:08x}",
                     instruction_word=f"0x{word:08x}")
    owner = int(evidence["function"], 16)

    def context_provider(image: str, address: int) -> dict:
        context = compact(image, address, 262144, True, address, full_original=True)
        if (context.get("mips") or {}).get("complete") is not True:
            raise ValueError("Causal function lacks complete MIPS evidence")
        return context

    packet = build_packet(
        divergence, refinement, previous["identities"],
        previous["compiled_memory_exceptions"], previous["reproduce"],
        context_provider=context_provider,
        writer_owner_provider=lambda _image, _pc: {
            "address": owner, "kind": "gte_transition_function",
            "writer_address": int((previous.get("diagnostic_owner") or {}).get(
                "writer_function") or 0),
        },
    )
    packet["gte_causal_evidence"] = evidence
    packet["cause_statement"] = (
        "RAM matched at PRE_CALL and POST_CALL; GTE light matrices matched before "
        "this function and diverged after it"
    )
    base = packet["failure_signature"]
    proof = json.dumps(evidence, sort_keys=True, separators=(",", ":"))
    packet["failure_signature"] = hashlib.sha256(
        f"{base}:{owner:08x}:{proof}".encode()).hexdigest()
    packet["case_key"] = hashlib.sha256(
        f"{packet['case_key']}:{owner:08x}".encode()).hexdigest()
    packet["serialized_bytes"] = len(json.dumps(
        packet, sort_keys=True, separators=(",", ":")).encode())
    if packet["serialized_bytes"] > packet["budget_bytes"]:
        raise ValueError("Causal packet exceeds its byte budget")
    return packet


def read_causal_request(directory: Path, request: dict,
                        previous: dict, divergence: dict) -> dict:
    expected = {"pre", "post", "writer"}
    if set(request) != expected:
        raise ValueError("Causal request needs pre, post and writer snapshots")
    snapshots = []
    for key in ("pre", "post", "writer"):
        name = request[key]
        if (not isinstance(name, str) or Path(name).name != name or
                not name.startswith("diagnostic-result-gte-") or
                not name.endswith(".json")):
            raise ValueError("Causal request references an invalid diagnostic file")
        path = (directory / name).resolve()
        if not path.is_relative_to(directory.resolve()) or not path.is_file():
            raise ValueError("Causal diagnostic file is unavailable")
        snapshots.append(json.loads(path.read_text(encoding="utf-8")))
    return build_causal_packet(previous, divergence, *snapshots)

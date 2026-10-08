#!/usr/bin/env python3
"""Build bounded evidence packets for the first lockstep divergence"""

from __future__ import annotations

import copy
import hashlib
import json
from dataclasses import dataclass
from typing import Callable

from lockstep_audit import checklist
from lockstep_contract import (canonical, digest, exact_keys, exceptions_identity,
                               require_object, validate_event, validate_exceptions)


PACKET_CONTRACT = "xport-lockstep-attention-v1"
MAX_PACKET_BYTES = 2097152
DEFAULT_PACKET_BYTES = 1048576
CLASSIFICATIONS = {
    "wrong_pointer", "missing_store", "extra_store", "wrong_branch", "wrong_target",
    "bad_stack_argument", "memory_value_divergence", "control_flow_divergence",
    "runtime_divergence",
}


class PacketError(ValueError):
    pass


def _audit_function_binding(context: dict) -> dict:
    function = require_object(context.get("function"), "audit function")
    mips = require_object(context.get("mips"), "audit MIPS")
    pseudocode = require_object(context.get("pseudocode"), "audit pseudocode")
    return {
        "image": function["image"],
        "address": function["address"],
        "function_sha256": function["sha256"],
        "mips_sha256": mips["sha256"],
        "pseudocode_sha256": pseudocode["sha256"],
        "pseudocode_complete": pseudocode.get("complete") is True,
        "machine_audit_sha256": context["machine_audit"]["sha256"],
    }


def scope_legacy_audit_packet(packet: dict) -> dict | None:
    context = packet.get("source_context") or {}
    cluster = context.get("audit_cluster") or []
    required = (context.get("audit_contract") or {}).get("required_functions") or []
    if (not cluster or len(required) != len(cluster) + 1 or
            context.get("audit_scope") is not None or
            all((row.get("implementation") or {}).get("complete") is True
                for row in cluster)):
        return None
    if ((context.get("implementation") or {}).get("complete") is not True or
            (context.get("mips") or {}).get("complete") is not True):
        raise PacketError("Legacy packet owner is not repairable")
    updated = copy.deepcopy(packet)
    scoped = updated["source_context"]
    scoped["audit_contract"]["required_functions"] = [required[0]]
    scoped["audit_cluster"] = []
    scoped["audit_scope"] = "owner_function"
    scoped["unreviewed_original_callees"] = [row["address"] for row in cluster]
    updated["failure_signature"] = hashlib.sha256(
        f"{packet['failure_signature']}:owner-scope-v1".encode()).hexdigest()
    updated["serialized_bytes"] = len(canonical(updated).encode("utf-8"))
    if updated["serialized_bytes"] > updated["budget_bytes"]:
        raise PacketError("Scoped packet exceeds its byte budget")
    return updated


def require_writer_library_audit(packet: dict) -> dict | None:
    context = packet.get("source_context") or {}
    library = context.get("writer_library") or {}
    required = (context.get("audit_contract") or {}).get("required_functions") or []
    if not library or library.get("audit_source"):
        return None
    function = library.get("function") or {}
    key = (function.get("image"), function.get("address"))
    if key in {(row.get("image"), row.get("address")) for row in required}:
        return None
    if (packet.get("diagnostic_owner") or {}).get("kind") != "writer_library_caller":
        raise PacketError("Writer library audit has no classified caller")
    if any((library.get(name) or {}).get("complete") is not True
           for name in ("mips", "pseudocode")):
        raise PacketError("Writer library audit lacks complete original evidence")
    owner = context.get("implementation") or {}
    if owner.get("complete") is not True or not owner.get("path") or not owner.get("symbol"):
        raise PacketError("Writer library audit lacks a complete owning C function")
    if not (library.get("library_classification") or {}).get("alias"):
        raise PacketError("Writer library audit lacks reviewed classification")
    updated = copy.deepcopy(packet)
    destination = updated["source_context"]["writer_library"]
    destination["machine_audit"] = checklist(destination["mips"]["lines"])
    destination["audit_source"] = {key: owner[key] for key in ("path", "symbol")}
    updated["source_context"]["audit_contract"]["required_functions"].append(
        _audit_function_binding(destination))
    updated["failure_signature"] = hashlib.sha256(
        f"{packet['failure_signature']}:writer-library-audit-v1".encode()).hexdigest()
    updated["serialized_bytes"] = len(canonical(updated).encode("utf-8"))
    if updated["serialized_bytes"] > updated["budget_bytes"]:
        raise PacketError("Writer library audit packet exceeds its byte budget")
    return updated


@dataclass(frozen=True)
class RefinementRequest:
    kind: str
    begin_event: int
    end_event: int
    probes: tuple[dict, ...]


def failure_signature(kind: str, divergence: dict, identities: dict,
                      refinement: dict | None = None) -> str:
    comparison = divergence.get("comparison") or {}
    original = divergence.get("original") or {}
    native = divergence.get("native") or {}
    value = {
        "kind": kind,
        "record_id": identities.get("record_id"),
        "native_source_identity": identities.get("native_source_identity"),
        "original": _event_key(original),
        "native": _event_key(native),
        "runtime": {
            "kind": divergence.get("kind"),
            "error": divergence.get("error"),
        },
        "difference": {
            key: comparison.get(key) for key in ("region", "offset", "length", "original", "candidate")
        },
    }
    if identities.get("shared_source_identity") is not None:
        value["shared_source_identity"] = identities["shared_source_identity"]
    if refinement is not None:
        value["evidence"] = {
            "interval_complete": refinement.get("interval_complete"),
            "original_writes": [
                {key: row.get(key) for key in
                 ("region", "offset", "width", "pc", "instruction_word", "before", "after")}
                for row in refinement.get("original_writes") or []],
            "native_writes": [
                {key: row.get(key) for key in
                 ("region", "offset", "width", "before", "after")}
                for row in refinement.get("native_writes") or []],
            "branch_slice": refinement.get("branch_slice"),
            "argument_location": refinement.get("argument_location"),
            "argument_mismatch": refinement.get("argument_mismatch"),
            "target_resolution_complete": refinement.get("target_resolution_complete"),
        }
    return hashlib.sha256(canonical(value).encode("utf-8")).hexdigest()


def case_key(divergence: dict, identities: dict) -> str:
    comparison = divergence.get("comparison") or {}
    original = divergence.get("original") or {}
    native = divergence.get("native") or {}
    entry_arguments, entry_boundary = _entry_arguments(divergence, "original")
    value = {
        "record_id": identities.get("record_id"),
        "status": divergence.get("status"),
        "kind": divergence.get("kind"),
        "original": _semantic_event_key(original),
        "native": _semantic_event_key(native),
        "entry_arguments": entry_arguments if entry_boundary == "PRE_CALL" else None,
        "difference_location": {
            key: comparison.get(key) for key in ("region", "offset", "length")
        },
    }
    return hashlib.sha256(canonical(value).encode("utf-8")).hexdigest()


def _event_key(event: dict) -> dict:
    return {key: event.get(key) for key in (
        "kind", "frame", "image", "function", "parent_invocation", "invocation",
        "depth", "call_ordinal", "callsite", "target", "return_pc",
    )}


def _semantic_event_key(event: dict) -> dict:
    return {key: event.get(key) for key in (
        "kind", "frame", "image", "function", "depth", "callsite",
        "target", "return_pc",
    )}


def refinement_request(divergence: dict) -> RefinementRequest:
    last = divergence.get("last_match") or {}
    prior = last.get("original") or {}
    current = divergence.get("original") or {}
    begin = int(prior.get("event_sequence", -1))
    end = int(current.get("event_sequence", begin + 1))
    if divergence.get("status") == "memory_divergence":
        difference = require_object(divergence.get("comparison"), "comparison")
        region = difference.get("region")
        offset = difference.get("offset")
        length = difference.get("length")
        if region not in {"dram", "scratchpad"} or not all(
                isinstance(value, int) and not isinstance(value, bool) for value in (offset, length)):
            raise PacketError("Memory divergence lacks a bounded region")
        if length < 1 or length > 64:
            raise PacketError("Memory refinement is limited to 64 bytes")
        probes = ({"kind": "data_watchpoint", "side": "original", "region": region,
                   "offset": offset, "length": length},
                  {"kind": "data_watchpoint", "side": "native", "region": region,
                   "offset": offset, "length": length})
        return RefinementRequest("memory", begin, end, probes)
    callsite = current.get("callsite")
    probes = ({"kind": "branch_slice", "side": "original", "callsite": callsite,
               "include_delay_slot": True},
              {"kind": "source_location", "side": "native"})
    return RefinementRequest("control", begin, end, probes)


def boundary_evidence(divergence: dict) -> dict | None:
    if divergence.get("status") != "memory_divergence":
        return None
    original = divergence.get("original") or {}
    native = divergence.get("native") or {}
    if (original.get("kind") != "POST_CALL" or native.get("kind") != "POST_CALL" or
            not original.get("function") or original.get("function") != native.get("function") or
            not original.get("callsite") or
            original.get("frame") != native.get("frame") or
            original.get("invocation") != native.get("invocation") or
            not divergence.get("last_match")):
        return None
    request = refinement_request(divergence)
    return {
        "schema": 1, "contract": "xport-lockstep-refinement-v1",
        "request": {"kind": request.kind, "begin_event": request.begin_event,
                    "end_event": request.end_event, "probes": list(request.probes)},
        "interval_complete": False,
        "original_writes": [], "native_writes": [], "branch_slice": [],
        "native_location": None,
        "pointer_evidence": {"native_writer_ring": divergence["native_writer_ring"]}
        if "native_writer_ring" in divergence else {},
        "argument_location": None,
        "argument_mismatch": False, "target_resolution_complete": False,
    }


def deferred_evidence(divergence: dict) -> dict:
    request = refinement_request(divergence)
    return {
        "schema": 1, "contract": "xport-lockstep-refinement-v1",
        "request": {"kind": request.kind, "begin_event": request.begin_event,
                    "end_event": request.end_event, "probes": list(request.probes)},
        "interval_complete": False,
        "original_writes": [], "native_writes": [], "branch_slice": [],
        "native_location": None,
        "pointer_evidence": {"native_writer_ring": divergence["native_writer_ring"]}
        if "native_writer_ring" in divergence else {},
        "argument_location": None,
        "argument_mismatch": False, "target_resolution_complete": False,
    }


def prearmed_evidence(divergence: dict) -> dict | None:
    if divergence.get("status") != "memory_divergence":
        return None
    comparison = divergence.get("comparison") or {}
    observed = divergence.get("watch_evidence") or {}
    target = observed.get("target") or {}
    if (target.get("region") != comparison.get("region") or
            target.get("offset") != comparison.get("offset") or
            not observed.get("original_writes") or not observed.get("native_writes")):
        return None
    def agrees_with_terminal(writer: dict, terminal: str) -> bool:
        offset = comparison.get("offset")
        length = comparison.get("length")
        start = writer.get("offset")
        width = writer.get("width")
        if not all(type(value) is int for value in (offset, length, start, width)):
            return False
        if length < 1 or width < 1 or start > offset or start + width < offset + length:
            return False
        try:
            observed_bytes = bytes.fromhex(writer.get("after", ""))
            terminal_bytes = bytes.fromhex(terminal)
        except (TypeError, ValueError):
            return False
        if len(observed_bytes) != width or len(terminal_bytes) != length:
            return False
        begin = offset - start
        return observed_bytes[begin:begin + length] == terminal_bytes

    if (not agrees_with_terminal(observed["original_writes"][-1],
                                 comparison.get("original", "")) or
            not agrees_with_terminal(observed["native_writes"][-1],
                                     comparison.get("candidate", ""))):
        return None
    allowed = {"region", "offset", "width", "pc", "before", "after", "source",
               "instruction_word", "stop_pc",
               "caller_pc", "caller2_pc", "caller_symbol", "caller2_symbol",
               "caller3_symbol", "caller4_symbol", "sp", "ra"}
    request = refinement_request(divergence)
    return {
        "schema": 1, "contract": "xport-lockstep-refinement-v1",
        "request": {"kind": request.kind, "begin_event": request.begin_event,
                    "end_event": request.end_event, "probes": list(request.probes)},
        "interval_complete": False,
        "original_writes": [{key: value for key, value in item.items() if key in allowed}
                            for item in observed["original_writes"][-1:]],
        "native_writes": [{key: value for key, value in item.items() if key in allowed}
                          for item in observed["native_writes"][-1:]],
        "branch_slice": [], "native_location": None,
        "pointer_evidence": {"native_writer_ring": divergence["native_writer_ring"]}
        if "native_writer_ring" in divergence else {},
        "argument_location": None, "argument_mismatch": False,
        "target_resolution_complete": False,
    }


def last_contributing_writer(writes: list[dict], comparison: dict) -> dict | None:
    offset = comparison.get("offset")
    length = comparison.get("length")
    if type(offset) is not int or type(length) is not int or length < 1:
        return None
    try:
        original = bytes.fromhex(comparison["original"])
        candidate = bytes.fromhex(comparison["candidate"])
    except (KeyError, TypeError, ValueError):
        return None
    if len(original) != length or len(candidate) != length:
        return None
    for writer in reversed(writes):
        start = writer.get("offset")
        width = writer.get("width")
        if type(start) is not int or type(width) is not int or width < 1:
            continue
        try:
            before = bytes.fromhex(writer.get("before", ""))
            after = bytes.fromhex(writer.get("after", ""))
        except (TypeError, ValueError):
            continue
        if len(before) != width or len(after) != width:
            continue
        for index in range(length):
            position = offset + index - start
            if (0 <= position < width and original[index] != candidate[index] and
                    before[position] != after[position] and after[position] == original[index]):
                return writer
    return None


def classify(divergence: dict, refinement: dict) -> tuple[str, str]:
    status = divergence.get("status")
    if status == "runtime_divergence":
        return "runtime_divergence", "observed"
    if status == "control_flow_divergence":
        if refinement.get("argument_location") == "stack" and refinement.get("argument_mismatch") is True:
            return "bad_stack_argument", "proven"
        if divergence.get("kind") == "wrong_call_target_or_branch":
            if refinement.get("target_resolution_complete") is True:
                return "wrong_target", "proven"
            return "wrong_branch", "observed"
        return "control_flow_divergence", "observed"
    if status != "memory_divergence":
        raise PacketError("Unsupported divergence status")
    if refinement.get("interval_complete") is not True:
        return "memory_value_divergence", "observed"
    original = refinement.get("original_writes") or []
    native = refinement.get("native_writes") or []
    if original and not native:
        return "missing_store", "proven"
    if native and not original:
        return "extra_store", "proven"
    pointer = refinement.get("pointer_evidence") or {}
    if (original and native and pointer.get("complete") is True and
            pointer.get("expected_address") != pointer.get("actual_address")):
        return "wrong_pointer", "proven"
    return "memory_value_divergence", "observed"


def _validate_refinement(value: dict, request: RefinementRequest) -> dict:
    value = require_object(value, "refinement")
    exact_keys(value, ("schema", "contract", "request", "interval_complete", "original_writes",
                       "native_writes", "branch_slice", "native_location", "pointer_evidence",
                       "argument_location", "argument_mismatch", "target_resolution_complete"))
    if value["schema"] != 1 or value["contract"] != "xport-lockstep-refinement-v1":
        raise PacketError("Unsupported refinement contract")
    expected = {
        "kind": request.kind, "begin_event": request.begin_event,
        "end_event": request.end_event, "probes": list(request.probes),
    }
    if value["request"] != expected:
        raise PacketError("Refinement does not match its replay request")
    if type(value["interval_complete"]) is not bool:
        raise PacketError("interval_complete must be boolean")
    for side in ("original_writes", "native_writes"):
        if not isinstance(value[side], list) or len(value[side]) > 16:
            raise PacketError("Writer evidence is unbounded")
        for writer in value[side]:
            exact_keys(require_object(writer, "writer"),
                       ("region", "offset", "width", "pc", "before", "after", "source"),
                       ("caller_pc", "caller2_pc", "caller_symbol", "caller2_symbol",
                        "caller3_symbol", "caller4_symbol", "sp", "ra",
                        "instruction_word", "stop_pc"))
    if not isinstance(value["branch_slice"], list) or len(value["branch_slice"]) > 64:
        raise PacketError("Branch slice is unbounded")
    return value


def _trim_context(context: dict, budget: int, packet: dict) -> None:
    if (len(canonical(packet).encode("utf-8")) > budget and
            (context.get("audit_contract") or {}).get("complete_original_required") is True):
        raise PacketError("Complete function audit context exceeds the packet byte budget")
    paths = (
        ("mips", "lines"), ("pseudocode", "lines"), ("implementation", "lines"),
        ("implementation", "text"), ("declarations", None),
    )
    while len(canonical(packet).encode("utf-8")) > budget:
        changed = False
        for first, second in paths:
            value = context.get(first)
            if second is None and isinstance(value, list) and value:
                value.pop()
                changed = True
            elif isinstance(value, dict):
                child = value.get(second)
                if isinstance(child, list) and child:
                    child.pop()
                    value["complete"] = False
                    changed = True
                elif isinstance(child, str) and child:
                    value[second] = child[:max(0, len(child) // 2)]
                    value["complete"] = False
                    changed = True
            if len(canonical(packet).encode("utf-8")) <= budget:
                return
        if not changed:
            raise PacketError("Required packet fields exceed the byte budget")


def _entry_arguments(divergence: dict, side: str) -> tuple[list | None, str]:
    event = (divergence.get(side) or
             (divergence.get("last_match") or {}).get(side) or {})
    if event.get("kind") == "PRE_CALL":
        return event.get("arguments"), "PRE_CALL"
    if event.get("kind") != "POST_CALL":
        return None, "UNAVAILABLE"
    previous = (divergence.get("last_match") or {}).get(side) or {}
    if (previous.get("kind") == "PRE_CALL" and
            all(previous.get(key) == event.get(key) for key in
                ("image", "function", "frame", "invocation"))):
        return previous.get("arguments"), "PRE_CALL"
    return None, "UNAVAILABLE"


def build_packet(divergence: dict, refinement: dict, identities: dict, exceptions: dict,
                 reproduce: list[str], context_provider: Callable[[str, int], dict] | None = None,
                 budget: int = DEFAULT_PACKET_BYTES,
                 writer_owner_provider: Callable[[str, int], int | None] | None = None) -> dict:
    if budget < 8192 or budget > MAX_PACKET_BYTES:
        raise PacketError("Packet budget must be 8192..2097152 bytes")
    request = refinement_request(divergence)
    refinement = _validate_refinement(refinement, request)
    last_match = divergence.get("last_match") or {}
    original = require_object(divergence.get("original") or last_match.get("original"), "original event")
    native = require_object(divergence.get("native") or last_match.get("native"), "native event")
    validate_event(original)
    validate_event(native)
    identities = require_object(identities, "identities")
    exact_keys(identities, (
        "record_id", "original_executable_sha256", "native_executable_sha256",
        "native_pdb_sha256", "native_source_identity", "duckstation_patch_sha256",
        "exceptions_identity",
    ), optional=("shared_source_identity",))
    for key, value in identities.items():
        digest(value, key)
    validate_exceptions(exceptions)
    if identities["exceptions_identity"] != exceptions_identity(exceptions):
        raise PacketError("Packet memory exception identity mismatch")
    if not isinstance(reproduce, list) or not reproduce or not all(
            isinstance(value, str) and value for value in reproduce):
        raise PacketError("Packet requires an exact reproduce command")
    image = original["image"]
    function_text = original.get("function") or original.get("target")
    owner_kind = "boundary_function"
    writer_function = None
    writer_pc_text = None
    writer_return_address = None
    writer_interval = refinement.get("interval_complete") is True
    if (refinement.get("original_writes") and
            (original.get("kind") in {"PRE_CALL", "POST_CALL"} or writer_interval)):
        writer = (last_contributing_writer(refinement["original_writes"],
                                           divergence.get("comparison") or {})
                  if refinement.get("interval_complete") is True else
                  refinement["original_writes"][0])
        if writer is None:
            # A narrower original store and a wider native store can overlap the watch
            # Only the native store may change the differing bytes
            writer = refinement["original_writes"][-1]
        writer_pc = writer.get("pc")
        writer_pc_text = writer_pc
        writer_return_address = writer.get("ra")
        if isinstance(writer_pc, str) and writer_pc.startswith("0x"):
            resolver = writer_owner_provider
            if resolver is None and context_provider is None:
                from source_context import writer_audit_owner
                resolved = writer_audit_owner(image, int(writer_pc, 16), writer.get("ra"))
            else:
                resolved = resolver(image, int(writer_pc, 16)) if resolver else None
            if isinstance(resolved, int):
                resolved = {"address": resolved, "kind": "original_writer_function",
                            "writer_address": resolved}
            if isinstance(resolved, dict) and isinstance(resolved.get("address"), int):
                function_text = f"0x{resolved['address']:08x}"
                owner_kind = resolved.get("kind", "original_writer_function")
                writer_function = resolved.get("writer_address")
            else:
                owner_kind = "unresolved_original_writer"
        else:
            owner_kind = "unresolved_original_writer"
    if not function_text:
        last = divergence.get("last_match") or {}
        last_original = last.get("original") or {}
        last_native = last.get("native") or {}
        function_text = (last_original.get("function") or last_original.get("target") or
                         native.get("function") or native.get("target") or
                         last_native.get("function") or last_native.get("target"))
    if function_text:
        function = int(function_text, 16)
        if context_provider is None:
            from source_context import compact
            callsite = int(original["callsite"], 16) if original.get("callsite") else function
            context = compact(
                image, function, max(4096, budget - 131072), True, callsite,
                full_original=True,
            )
        else:
            context = context_provider(image, function)
        audit_cluster = context.get("audit_cluster") or []
        if any((item.get("implementation") or {}).get("complete") is not True
               for item in audit_cluster):
            raise PacketError("Explicit audit helper lacks a complete C implementation")
        for item in [context, *audit_cluster]:
            item["machine_audit"] = checklist(item["mips"]["lines"])
        required_functions = [_audit_function_binding(context)]
        required_functions.extend(_audit_function_binding(item) for item in audit_cluster)
        context["audit_contract"] = {
            "pseudocode_baseline_required": False,
            "complete_original_required": True,
            "complete_mips_audit_required": True,
            "audit_direct_helpers": True,
            "semantic_simplification_allowed": False,
            "single_observed_difference_patch_allowed": False,
            "required_receipt_contract": "xport-lockstep-function-audit-v1",
            "required_functions": required_functions,
        }
        context["audit_cluster"] = audit_cluster
        if owner_kind == "writer_library_caller" and writer_function is not None and context_provider is None:
            from source_context import compact, reviewed_library_identity
            context["writer_library"] = compact(
                image, writer_function, max(32768, min(262144, budget // 4)),
                True, writer_function, full_original=True,
            )
            classification = reviewed_library_identity(image, writer_function)
            if classification is None:
                raise PacketError("Writer library lacks a reviewed classification")
            if (context["writer_library"].get("mips") or {}).get("complete") is not True:
                raise PacketError("Writer library lacks complete original evidence")
            context["writer_library"]["library_classification"] = classification
            context["writer_library"]["machine_audit"] = checklist(
                context["writer_library"]["mips"]["lines"])
            context["writer_library"]["audit_source"] = {
                key: (context.get("implementation") or {}).get(key)
                for key in ("path", "symbol")
            }
            context["audit_contract"]["required_functions"].append(
                _audit_function_binding(context["writer_library"]))
        if (context.get("mips") or {}).get("complete") is not True:
            raise PacketError("Lockstep repair requires complete MIPS context")
    else:
        context = {
            "schema": 1,
            "image": image,
            "address": None,
            "function": None,
            "implementation": None,
            "declarations": [],
            "gaps": ["The divergence is between VBlank boundaries and has no function identity"],
        }
    kind, confidence = classify(divergence, refinement)
    if kind not in CLASSIFICATIONS:
        raise PacketError("Unknown attention classification")
    comparison = divergence.get("comparison")
    original_arguments, original_boundary = _entry_arguments(divergence, "original")
    native_arguments, native_boundary = _entry_arguments(divergence, "native")
    packet = {
        "schema": 1,
        "contract": PACKET_CONTRACT,
        "classification": kind,
        "confidence": confidence,
        "diagnostic_owner": {"kind": owner_kind, "function": function_text,
                             "writer_function": writer_function,
                             "writer_pc": writer_pc_text,
                             "writer_return_address": writer_return_address},
        "failure_signature": failure_signature(kind, divergence, identities, refinement),
        "case_key": case_key(divergence, identities),
        "identities": identities,
        "last_matching_boundary": divergence.get("last_match"),
        "expected_original_event": original,
        "actual_native_event": native,
        "comparison": comparison,
        "invocation_path": {
            "original": divergence.get("original_stack", []),
            "native": divergence.get("native_stack", []),
        },
        "callsite": {
            "address": original.get("callsite"),
            "instruction_word": original.get("instruction_word"),
            "instruction": original.get("instruction"),
            "delay_slot_address": original.get("delay_slot_pc"),
            "delay_slot_word": original.get("delay_slot_word"),
            "delay_slot": original.get("delay_slot"),
            "expected_target": original.get("target"),
            "actual_target": native.get("target"),
            "return_pc": original.get("return_pc"),
        },
        "arguments": {
            "original": original_arguments,
            "native": native_arguments,
            "original_boundary": original_boundary,
            "native_boundary": native_boundary,
        },
        "returns": {"original": original.get("return_value"), "native": native.get("return_value")},
        "runtime_failure": ({
            "kind": divergence.get("kind"),
            "error": divergence.get("error"),
        } if divergence.get("status") == "runtime_divergence" else None),
        "refinement": refinement,
        "cause_statement": (
            "Interval evidence proves this classification" if confidence == "proven" else
            "The first writer is an observation and is not a proven root cause"
        ),
        "source_context": context,
        "compiled_memory_exceptions": exceptions,
        "reproduce": reproduce,
    }
    _trim_context(context, budget, packet)
    size = len(canonical(packet).encode("utf-8"))
    packet["serialized_bytes"] = size
    packet["budget_bytes"] = budget
    final_size = len(canonical(packet).encode("utf-8"))
    if final_size > budget:
        _trim_context(context, budget - (final_size - size), packet)
        packet["serialized_bytes"] = len(canonical(packet).encode("utf-8"))
    if packet["serialized_bytes"] > budget:
        raise PacketError("Attention packet exceeds its byte budget")
    return packet


class RefinementCoordinator:
    """Repeat exactly one divergence interval through a fresh replay backend"""

    def __init__(self, replay_backend):
        self.replay_backend = replay_backend

    def run(self, divergence: dict) -> dict:
        request = refinement_request(divergence)
        payload = {
            "kind": request.kind,
            "begin_event": request.begin_event,
            "end_event": request.end_event,
            "probes": list(request.probes),
        }
        result = self.replay_backend.replay_interval(payload)
        return _validate_refinement(result, request)

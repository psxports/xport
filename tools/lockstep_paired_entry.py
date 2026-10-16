"""Promote a matched function entry and later writer evidence to an audit owner"""

from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import re

from lockstep_audit import checklist, words_from_listing
from lockstep_operand_provenance import _donor_packet
from lockstep_packet import _audit_function_binding
from source_context import compact


_NAME = re.compile(r"[A-Za-z0-9_-]{1,80}\.json\Z")
_HASH = re.compile(r"[0-9a-f]{64}\Z")
_ADDRESS = re.compile(r"0x[0-9a-fA-F]{8}\Z")


def _address(value: object) -> int:
    if not isinstance(value, str) or not _ADDRESS.fullmatch(value):
        raise ValueError("Paired entry address is invalid")
    return int(value, 16)


def _evidence(directory: Path, name: object, digest: object) -> dict:
    if (not isinstance(name, str) or not _NAME.fullmatch(name) or
            not isinstance(digest, str) or not _HASH.fullmatch(digest)):
        raise ValueError("Paired entry evidence identity is invalid")
    raw = (directory / name).read_bytes()
    if len(raw) > 262144 or hashlib.sha256(raw).hexdigest() != digest:
        raise ValueError("Paired entry evidence changed or is oversized")
    result = json.loads(raw)
    if not isinstance(result, dict) or result.get("schema") != 1 or result.get("complete") is not True:
        raise ValueError("Paired entry evidence is incomplete")
    return result


def _memory(rows: object, address: int, size: int) -> bytes:
    if not isinstance(rows, list):
        raise ValueError("Paired entry memory is missing")
    matches = [row for row in rows if isinstance(row, dict) and
               _address(row.get("address")) == address and row.get("size") == size]
    if len(matches) != 1:
        raise ValueError("Paired entry memory sample is ambiguous")
    try:
        data = bytes.fromhex(matches[0]["data"])
    except (KeyError, TypeError, ValueError) as error:
        raise ValueError("Paired entry memory sample is invalid") from error
    if len(data) != size:
        raise ValueError("Paired entry memory sample is truncated")
    return data


def read_paired_entry_request(root: Path, directory: Path, request: dict,
                              previous: dict, source_roots: tuple[Path, ...]) -> dict:
    if request.get("buffer_alias") is True:
        from lockstep_buffer_alias import read_buffer_alias_request
        return read_buffer_alias_request(root, directory, request, previous, source_roots)
    common = {"kind", "diagnostic_file", "sha256", "image", "owner_function",
              "native_symbol", "input_addresses"}
    scalar_count = request.get("scalar_arguments")
    if scalar_count is not None:
        if type(scalar_count) is not int or not 1 <= scalar_count <= 4:
            raise ValueError("Paired scalar argument count is invalid")
        common.add("scalar_arguments")
    extra_store = request.get("extra_store") is True
    callee_store = "callee_function" in request
    if extra_store and callee_store:
        raise ValueError("A callee store cannot be an extra store")
    required = common | ({"callee_function"} if callee_store else
                         {"extra_store"} if extra_store else
                         {"writer_file", "writer_sha256"})
    donor_keys = {"evidence_packet", "evidence_packet_sha256"}
    call_path_keys = {"entry_function", "bridge_function", "native_first_address",
                      "iso9660_path_input"}
    extended = call_path_keys.issubset(request)
    if scalar_count is not None and extended:
        raise ValueError("Scalar entry cannot use a pointer call path")
    valid_keys = required | (call_path_keys if extended else set())
    if (set(request) not in (valid_keys, valid_keys | donor_keys) or
            request.get("kind") != "paired_entry"):
        raise ValueError("Invalid paired entry request")
    _donor_packet(directory, request, previous)
    probe = _evidence(directory, request["diagnostic_file"], request["sha256"])
    writers = (None if extra_store or callee_store else
               _evidence(directory, request["writer_file"], request["writer_sha256"]))
    image = request["image"]
    if not isinstance(image, str) or not image or len(image) > 40:
        raise ValueError("Paired entry image is invalid")
    owner = _address(request["owner_function"])
    callee = _address(request["callee_function"]) if callee_store else None
    if callee_store and extended:
        raise ValueError("Callee store cannot use the ISO9660 call path")
    entry = _address(request["entry_function"]) if extended else owner
    bridge = _address(request["bridge_function"]) if extended else None
    native_first = _address(request["native_first_address"]) if extended else None
    if extended and request.get("iso9660_path_input") is not True:
        raise ValueError("Paired entry call path requires an ISO9660 path input")
    symbol = request["native_symbol"]
    if not isinstance(symbol, str) or not re.fullmatch(r"[A-Za-z_]\w{0,100}", symbol):
        raise ValueError("Paired entry native symbol is invalid")
    addresses = request["input_addresses"]
    if (not isinstance(addresses, list) or len(addresses) not in (1, 2) or
            any(not isinstance(value, str) for value in addresses)):
        raise ValueError("Paired entry inputs are invalid")
    if extended and len(addresses) != 2:
        raise ValueError("Paired entry call path requires two inputs")
    parsed_addresses = list(map(_address, addresses))
    first = parsed_addresses[0]
    second = parsed_addresses[1] if len(parsed_addresses) == 2 else None
    if second is not None and first == second:
        raise ValueError("Paired entry inputs must be distinct")
    expected_frame = (previous.get("expected_original_event") or {}).get("frame")
    alignment = probe.get("alignment") or {}
    if (type(expected_frame) is not int or probe.get("target_frame") != expected_frame - 1 or
            (writers is not None and writers.get("target_frame") != expected_frame - 1) or
            alignment.get("aligned") is not True or
            (alignment.get("comparison") or {}).get("status") != "match"):
        raise ValueError("Paired entry evidence does not align with the divergence")
    hits = [row for row in probe.get("hits") or [] if isinstance(row, dict) and
            str(row.get("pc", "")).lower() == f"0x{entry:08x}" and
            isinstance(row.get("registers"), dict) and
            (scalar_count is not None or (
                str(row["registers"].get("r4", "")).lower() == f"0x{first:08x}" and
                (second is None or
                 str(row["registers"].get("r5", "")).lower() == f"0x{second:08x}")))]
    if len(hits) != 1:
        raise ValueError("Original function entry is absent or ambiguous")
    native = probe.get("native_entry") or {}
    expected_storage = "none" if scalar_count is not None or second is None else "guest"
    if scalar_count is not None:
        arguments = native.get("arguments") or []
        if (len(arguments) != 4 or any(
                _address(hits[0]["registers"].get(f"r{index + 4}")) != _address(arguments[index])
                for index in range(scalar_count))):
            raise ValueError("Function scalar inputs already differ at entry")
        expected_first = _address(hits[0]["registers"].get("r4"))
    else:
        expected_first = native_first if extended else first
    if (native.get("symbol") != symbol or native.get("second_storage") != expected_storage or
            str(native.get("output_address", "")).lower() !=
            f"0x{expected_first:08x}" or
            (scalar_count is None and second is not None and
             str(native.get("right_address", "")).lower() != f"0x{second:08x}")):
        raise ValueError("Native function entry does not match the original")
    native_rows = (native.get("probe_memory") if scalar_count is not None else
                   native.get("memory")) or []
    sample_first = native_first if extended else first
    size = next((row.get("size") for row in native_rows
                 if isinstance(row, dict) and
                 row.get("address", "").lower() == f"0x{sample_first:08x}"), None)
    if type(size) is not int or not 1 <= size <= 256:
        raise ValueError("Paired entry sample size is invalid")
    original_first = _memory(hits[0].get("memory"), first, size)
    native_first_bytes = _memory(native_rows, native_first if extended else first, size)
    second_size = (next((row.get("size") for row in native_rows if
                        isinstance(row, dict) and
                        row.get("address", "").lower() == f"0x{second:08x}"), None)
                   if scalar_count is not None and second is not None else size)
    if second is not None and (type(second_size) is not int or not 1 <= second_size <= 256):
        raise ValueError("Paired entry second sample size is invalid")
    original_second = (_memory(hits[0].get("memory"), second, second_size)
                       if second is not None else None)
    native_second = (_memory(native_rows, second, second_size)
                     if second is not None else None)
    if extended:
        normalize = lambda value: value.split(b"\0", 1)[0].replace(b"/", b"\\").upper().removesuffix(b";1")
        inputs_match = normalize(original_first) == normalize(native_first_bytes)
    else:
        inputs_match = original_first == native_first_bytes
    if not inputs_match or original_second != native_second:
        raise ValueError("Function inputs already differ at entry")
    context = compact(image, entry, 262144, True, entry, full_original=True)
    writer_context = (compact(image, owner, 262144, True, owner, full_original=True)
                      if extended else context)
    bridge_context = (compact(image, bridge, 262144, True, bridge, full_original=True)
                      if extended else None)
    contexts = [context] + ([bridge_context, writer_context] if extended else [])
    if callee_store:
        writer_context = compact(image, callee, 262144, True, callee, full_original=True)
        contexts.append(writer_context)
    if any((part.get("complete") is not True) for row in contexts for part in
           (row.get("implementation") or {}, row["mips"])):
        raise ValueError("Paired entry audit source is incomplete")
    start = callee if callee_store else owner
    end = int(writer_context["function"]["end"])
    writer_pcs = []
    native_writer_sites = []
    if callee_store:
        refinement = previous.get("refinement") or {}
        words = words_from_listing(context["mips"]["lines"])
        targets = {((pc + 4) & 0xf0000000) | ((word & 0x03ffffff) << 2)
                   for pc, word in words.items() if word >> 26 == 3}
        writer_pcs = sorted({int(row["pc"], 16)
                             for row in refinement.get("original_writes") or []
                             if start <= int(row["pc"], 16) < end})
        native_writes = refinement.get("native_writes") or []
        pattern = re.compile(rf"(?:^|!){re.escape(symbol)}\+0x[0-9a-f]+(?:\s|\Z)", re.I)
        owned = [row for row in native_writes if any(
            pattern.search(str(row.get(key, ""))) for key in
            ("caller_symbol", "caller2_symbol", "caller3_symbol", "caller4_symbol"))]
        if (refinement.get("interval_complete") is not True or callee not in targets or
                not writer_pcs or not owned):
            raise ValueError("Callee store lacks a complete interval and proven owner call")
    elif extra_store:
        refinement = previous.get("refinement") or {}
        native_writes = refinement.get("native_writes") or []
        if (previous.get("classification") != "extra_store" or
                refinement.get("original_writes") or not native_writes):
            raise ValueError("Paired extra-store evidence is not an unmatched native write")
        symbol_pattern = re.compile(
            rf"(?:^|!){re.escape(symbol)}\+0x[0-9a-f]+(?:\s|\Z)", re.IGNORECASE)
        for row in native_writes:
            matches = [(key, row.get(key)) for key in
                       ("caller_symbol", "caller2_symbol", "caller3_symbol", "caller4_symbol")
                       if isinstance(row, dict) and isinstance(row.get(key), str) and
                       symbol_pattern.search(row[key])]
            if len(matches) != 1:
                raise ValueError("Native extra store is not uniquely owned by the proposed function")
            native_writer_sites.append({"pc": row.get("pc"),
                                        "stack_field": matches[0][0],
                                        "symbol": matches[0][1]})
    else:
        writer_pcs = sorted({int(row["pc"], 16) for row in writers.get("hits") or []
                             if isinstance(row, dict) and row.get("kind") == "watchpoint" and
                             isinstance(row.get("pc"), str) and
                             _ADDRESS.fullmatch(row["pc"]) and
                             start <= int(row["pc"], 16) < end})
        if not writer_pcs:
            raise ValueError("No observed writer belongs to the proposed function")
    if extended:
        entry_listing = "\n".join(context["mips"]["lines"]).lower()
        bridge_listing = "\n".join(bridge_context["mips"]["lines"]).lower()
        if f"{bridge:08x}" not in entry_listing or f"{owner:08x}" not in bridge_listing:
            raise ValueError("Paired entry call path is absent from original MIPS")
    for row in contexts:
        row["machine_audit"] = checklist(row["mips"]["lines"])
    if callee_store:
        writer_context["audit_role"] = "downstream_writer"
        context["audit_cluster"] = [writer_context]
    if extended:
        bridge_context["audit_role"] = "call_path_bridge"
        writer_context["audit_role"] = "downstream_writer"
        context["audit_cluster"] = [bridge_context, writer_context]
    context["audit_contract"] = {
        "pseudocode_baseline_required": False,
        "complete_original_required": True,
        "complete_mips_audit_required": True,
        "audit_direct_helpers": True,
        "semantic_simplification_allowed": False,
        "single_observed_difference_patch_allowed": False,
        "required_receipt_contract": "xport-lockstep-function-audit-v1",
        "required_functions": [_audit_function_binding(row) for row in contexts],
    }
    evidence = {
        "diagnostic_file": request["diagnostic_file"], "sha256": request["sha256"],
        "frame": expected_frame, "input_addresses": addresses,
        "input_bytes_match": True, "writer_pcs": [f"0x{pc:08x}" for pc in writer_pcs],
    }
    if scalar_count is not None:
        evidence["scalar_arguments"] = native["arguments"][:scalar_count]
    if callee_store:
        evidence.update({"callee_function": f"0x{callee:08x}",
                         "native_owner_writes": len(owned)})
    elif extra_store:
        evidence.update({"extra_store": True, "native_writer_sites": native_writer_sites})
    else:
        evidence.update({"writer_file": request["writer_file"],
                         "writer_sha256": request["writer_sha256"]})
    if extended:
        evidence.update({"entry_function": f"0x{entry:08x}",
                         "bridge_function": f"0x{bridge:08x}",
                         "native_first_address": f"0x{native_first:08x}",
                         "iso9660_path_equivalent": True})
    packet = copy.deepcopy(previous)
    packet["source_context"] = context
    packet["diagnostic_owner"] = {
        "kind": ("paired_entry_call_path" if extended else
                 "paired_entry_extra_store" if extra_store else "paired_entry_writer"),
        "function": f"0x{entry:08x}" if extended else f"0x{owner:08x}",
        "writer_function": f"0x{owner:08x}" if extended else None}
    packet["paired_entry_provenance"] = evidence
    packet["cause_statement"] = (
        "Both sampled inputs match at function entry and the unmatched native store is owned "
        "by this native function; audit its complete original implementation and called helpers"
        if extra_store else
        "Both sampled inputs match at function entry and original writes occur within "
        "this function; audit its complete implementation and called helpers")
    if callee_store:
        packet["cause_statement"] = (
            "The sampled owner entry inputs match; its proven Original callee writes "
            "the divergent storage, while the Native interval contains a write under "
            "the same owner. Audit the owner call decision and the complete callee")
    packet["failure_signature"] = hashlib.sha256(json.dumps({
        "prior": previous["failure_signature"], "owner": owner, "evidence": evidence,
    }, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    packet["serialized_bytes"] = len(json.dumps(
        packet, sort_keys=True, separators=(",", ":")).encode())
    if packet["serialized_bytes"] > packet["budget_bytes"]:
        raise ValueError("Paired entry attention packet exceeds its byte budget")
    return packet


def read_writer_sequence_request(root: Path, directory: Path, request: dict,
                                 previous: dict, source_roots: tuple[Path, ...]) -> dict:
    required = {"kind", "diagnostic_file", "sha256", "image", "owner_function"}
    if set(request) != required or request.get("kind") != "writer_sequence":
        raise ValueError("Invalid writer sequence request")
    probe = _evidence(directory, request["diagnostic_file"], request["sha256"])
    image = request["image"]
    owner = _address(request["owner_function"])
    if not isinstance(image, str) or not image or len(image) > 40:
        raise ValueError("Writer sequence image is invalid")
    event = previous.get("expected_original_event") or {}
    comparison = previous.get("comparison") or {}
    frame = event.get("frame")
    length = comparison.get("length")
    offset = comparison.get("offset")
    if (type(frame) is not int or type(offset) is not int or
            type(length) is not int or length not in (1, 2, 4) or
            comparison.get("region") != "dram" or
            probe.get("target_frame") != frame - 1 or
            probe.get("checkpoint_pair_resumed") != frame - 1 or
            comparison.get("status") != "difference"):
        raise ValueError("Writer sequence is not bound to the divergent checkpoint")
    context = compact(image, owner, 262144, True, owner, full_original=True)
    if any((row.get("complete") is not True) for row in
           (context.get("implementation") or {}, context["mips"])):
        raise ValueError("Writer sequence audit source is incomplete")
    end = int(context["function"]["end"])
    address = 0x80000000 | offset
    observed = []
    for hit in probe.get("hits") or []:
        if not isinstance(hit, dict) or hit.get("kind") != "watchpoint":
            continue
        pc = _address(hit.get("pc"))
        if not owner <= pc < end:
            continue
        data = _memory(hit.get("memory"), address, length)
        observed.append({"pc": f"0x{pc:08x}", "after": data.hex()})
    candidate = comparison.get("candidate")
    original = comparison.get("original")
    if (not isinstance(candidate, str) or not isinstance(original, str) or
            len(candidate) != length * 2 or len(original) != length * 2):
        raise ValueError("Writer sequence comparison bytes are invalid")
    pair = next((index for index in range(len(observed) - 1)
                 if observed[index]["after"] == candidate and
                 observed[index + 1]["after"] == original), None)
    if pair is None:
        raise ValueError("Writer sequence does not show the candidate-to-original correction")
    context["machine_audit"] = checklist(context["mips"]["lines"])
    context["audit_contract"] = {
        "pseudocode_baseline_required": False,
        "complete_original_required": True,
        "complete_mips_audit_required": True,
        "audit_direct_helpers": True,
        "semantic_simplification_allowed": False,
        "single_observed_difference_patch_allowed": False,
        "required_receipt_contract": "xport-lockstep-function-audit-v1",
        "required_functions": [_audit_function_binding(context)],
    }
    evidence = {"diagnostic_file": request["diagnostic_file"],
                "sha256": request["sha256"], "frame": frame,
                "address": f"0x{address:08x}", "candidate_then_original": observed[pair:pair + 2]}
    packet = copy.deepcopy(previous)
    packet["source_context"] = context
    packet["diagnostic_owner"] = {
        "kind": "checkpoint_writer_sequence", "function": f"0x{owner:08x}"}
    packet["writer_sequence_provenance"] = evidence
    packet["cause_statement"] = (
        "A checkpoint replay observed the proposed function write the candidate bytes "
        "and then correct them to the original terminal bytes before the divergent boundary"
    )
    packet["failure_signature"] = hashlib.sha256(json.dumps({
        "prior": previous["failure_signature"], "owner": owner, "evidence": evidence,
    }, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    packet["serialized_bytes"] = len(json.dumps(
        packet, sort_keys=True, separators=(",", ":")).encode())
    if packet["serialized_bytes"] > packet["budget_bytes"]:
        raise ValueError("Writer sequence attention packet exceeds its byte budget")
    return packet

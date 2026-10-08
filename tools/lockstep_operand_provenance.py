"""Adopt a hash-bound paired operand probe as an upstream function audit"""

from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import re

from lockstep_audit import checklist
from lockstep_packet import _audit_function_binding
from lockstep_repair_context import build_prompt
from source_context import compact


_NAME = re.compile(r"[A-Za-z0-9_-]{1,80}\.json\Z")
_HASH = re.compile(r"[0-9a-f]{64}\Z")
_ADDRESS = re.compile(r"0x[0-9a-fA-F]{8}\Z")


def _donor_packet(directory: Path, request: dict, previous: dict) -> dict:
    name = request.get("evidence_packet")
    if name is None:
        return previous
    expected = request.get("evidence_packet_sha256")
    if (not isinstance(name, str) or not re.fullmatch(
            r"attention-[0-9]{4}(?:-[A-Za-z0-9_-]+)?\.json", name) or
            not isinstance(expected, str) or not _HASH.fullmatch(expected)):
        raise ValueError("Invalid source attention packet identity")
    raw = (directory / name).read_bytes()
    if len(raw) > 2097152 or hashlib.sha256(raw).hexdigest() != expected:
        raise ValueError("Source attention packet changed or is oversized")
    donor = json.loads(raw)
    if not isinstance(donor, dict):
        raise ValueError("Source attention packet is not an object")
    donor_ids = donor.get("identities") or {}
    current_ids = previous.get("identities") or {}
    if not isinstance(donor_ids, dict) or not isinstance(current_ids, dict):
        raise ValueError("Source attention packet identities are invalid")
    for key in ("record_id", "original_executable_sha256", "native_executable_sha256",
                "native_source_identity", "shared_source_identity"):
        if not isinstance(current_ids.get(key), str) or not current_ids[key]:
            raise ValueError("Current attention packet lacks a reusable identity")
    for key in ("record_id", "original_executable_sha256", "native_executable_sha256",
                "native_pdb_sha256", "native_source_identity", "shared_source_identity",
                "duckstation_patch_sha256", "exceptions_identity"):
        if donor_ids.get(key) != current_ids.get(key):
            raise ValueError("Source attention packet belongs to another build or record")
    donor_difference = donor.get("comparison") or {}
    current_difference = previous.get("comparison") or {}
    donor_event = donor.get("expected_original_event") or {}
    current_event = previous.get("expected_original_event") or {}
    if (not current_difference.get("region") or
            type(current_difference.get("offset")) is not int or
            type(current_event.get("frame")) is not int or
            not current_event.get("function") or not current_event.get("callsite")):
        raise ValueError("Current divergence lacks a reusable boundary")
    if (any(donor_difference.get(key) != current_difference.get(key) for key in
            ("region", "offset", "length", "original", "candidate")) or
            any(donor_event.get(key) != current_event.get(key) for key in
                ("frame", "kind", "image", "function", "callsite", "return_pc"))):
        raise ValueError("Source attention packet describes a different divergence")
    return donor


def derive_operand_request(directory: Path, previous: dict, diagnostic_file: str,
                           helper_function: str, evidence_packet: str | None = None) -> dict:
    if not _NAME.fullmatch(diagnostic_file):
        raise ValueError("Invalid operand provenance diagnostic name")
    raw = (directory / diagnostic_file).read_bytes()
    if len(raw) > 262144:
        raise ValueError("Operand provenance diagnostic is oversized")
    probe = json.loads(raw)
    if not isinstance(probe, dict):
        raise ValueError("Operand provenance diagnostic is not an object")
    donor_fields = {}
    if evidence_packet is not None:
        if not isinstance(evidence_packet, str) or not _NAME.fullmatch(evidence_packet):
            raise ValueError("Invalid source attention packet name")
        donor_fields = {
            "evidence_packet": evidence_packet,
            "evidence_packet_sha256": hashlib.sha256(
                (directory / evidence_packet).read_bytes()).hexdigest(),
        }
    donor = _donor_packet(directory, donor_fields, previous)
    callee = _address((donor.get("diagnostic_owner") or {}).get("function"))
    native = probe.get("native_entry") or {}
    if not isinstance(native, dict) or native.get("second_storage") != "host":
        raise ValueError("Operand provenance requires a host-array entry probe")
    symbol = native.get("caller_symbol")
    caller = (re.search(r"\b[A-Za-z_]\w*_([0-9A-Fa-f]{8})\+", symbol)
              if isinstance(symbol, str) else None)
    if caller is None:
        raise ValueError("Native caller address is unavailable")
    left = _address(native.get("output_address"))
    hits = [row for row in probe.get("hits") or [] if isinstance(row, dict) and
            str(row.get("pc", "")).lower() == f"0x{callee:08x}" and
            isinstance(row.get("registers"), dict) and
            str(row["registers"].get("r4", "")).lower() == f"0x{left:08x}"]
    if len(hits) != 1:
        raise ValueError("Original operand call is absent or ambiguous")
    registers = hits[0]["registers"]
    return {
        "kind": "operand_provenance", "diagnostic_file": diagnostic_file,
        "sha256": hashlib.sha256(raw).hexdigest(),
        "image": (previous.get("source_context") or {}).get("image"),
        "owner_function": f"0x{int(caller.group(1), 16):08x}",
        "callee_function": f"0x{callee:08x}",
        "return_pc": registers.get("r31"),
        "left_address": f"0x{left:08x}",
        "right_address": registers.get("r5"),
        "helper_function": helper_function,
        "native_symbol": native.get("symbol"),
        **donor_fields,
    }


def _address(value: object) -> int:
    if not isinstance(value, str) or not _ADDRESS.fullmatch(value):
        raise ValueError("Operand provenance address is invalid")
    return int(value, 16)


def _memory(rows: object, address: int) -> bytes:
    if not isinstance(rows, list):
        raise ValueError("Operand provenance memory is missing")
    matches = [row for row in rows if isinstance(row, dict) and
               _address(row.get("address")) == address and row.get("size") == 16]
    if len(matches) != 1:
        raise ValueError("Operand provenance memory sample is ambiguous")
    try:
        value = bytes.fromhex(matches[0]["data"])
    except (KeyError, TypeError, ValueError) as error:
        raise ValueError("Operand provenance memory bytes are invalid") from error
    if len(value) != 16:
        raise ValueError("Operand provenance requires 16-byte operands")
    return value


def read_operand_request(root: Path, directory: Path, request: dict,
                         previous: dict, source_roots: tuple[Path, ...]) -> dict:
    keys = {"kind", "diagnostic_file", "sha256", "image", "owner_function",
            "callee_function", "return_pc", "left_address", "right_address",
            "helper_function", "native_symbol"}
    donor_keys = {"evidence_packet", "evidence_packet_sha256"}
    if (set(request) not in (keys, keys | donor_keys) or
            request.get("kind") != "operand_provenance"):
        raise ValueError("Invalid operand provenance request")
    name, digest = request["diagnostic_file"], request["sha256"]
    if (not isinstance(name, str) or not _NAME.fullmatch(name) or
            not isinstance(digest, str) or not _HASH.fullmatch(digest)):
        raise ValueError("Invalid operand provenance file identity")
    path = directory / name
    raw = path.read_bytes()
    if len(raw) > 262144 or hashlib.sha256(raw).hexdigest() != digest:
        raise ValueError("Operand provenance evidence changed or is oversized")
    probe = json.loads(raw)
    if not isinstance(probe, dict):
        raise ValueError("Operand provenance probe is not an object")
    owner = _address(request["owner_function"])
    callee = _address(request["callee_function"])
    return_pc = _address(request["return_pc"])
    left = _address(request["left_address"])
    right = _address(request["right_address"])
    helper = _address(request["helper_function"])
    if len({owner, callee, helper}) != 3:
        raise ValueError("Operand provenance audit functions must be distinct")
    image = request["image"]
    if (not isinstance(image, str) or not image or len(image) > 40 or
            not isinstance(request["native_symbol"], str) or
            not re.fullmatch(r"[A-Za-z_]\w{0,100}", request["native_symbol"])):
        raise ValueError("Invalid operand provenance function identity")
    donor = _donor_packet(directory, request, previous)
    if ((donor.get("diagnostic_owner") or {}).get("function", "").lower() !=
            f"0x{callee:08x}" or
            (donor.get("source_context") or {}).get("image") != image):
        raise ValueError("Operand provenance does not match the waiting writer")
    expected_frame = (previous.get("expected_original_event") or {}).get("frame")
    alignment = probe.get("alignment") or {}
    if (not isinstance(alignment, dict) or
            not isinstance(alignment.get("comparison"), dict)):
        raise ValueError("Operand provenance alignment is invalid")
    if (probe.get("schema") != 1 or probe.get("complete") is not True or
            type(probe.get("target_frame")) is not int or
            probe["target_frame"] + 1 != expected_frame or
            alignment.get("aligned") is not True or
            alignment["comparison"].get("status") != "match"):
        raise ValueError("Operand provenance probe is incomplete or unaligned")
    hits = [row for row in probe.get("hits") or [] if isinstance(row, dict) and
            isinstance(row.get("pc"), str) and row["pc"].lower() ==
            f"0x{callee:08x}" and
            isinstance(row.get("registers"), dict) and
            str(row["registers"].get("r31", "")).lower() == f"0x{return_pc:08x}"]
    if len(hits) != 1:
        raise ValueError("Operand provenance call entry is absent or ambiguous")
    native = probe.get("native_entry") or {}
    if not isinstance(native, dict) or not isinstance(native.get("caller_symbol"), str):
        raise ValueError("Native operand entry is incomplete")
    caller = re.search(r"\b[A-Za-z_]\w*_([0-9A-Fa-f]{8})\+",
                       native.get("caller_symbol", ""))
    if (native.get("symbol") != request["native_symbol"] or
            native.get("second_storage") != "host" or
            native.get("output_address", "").lower() != f"0x{left:08x}" or
            caller is None or int(caller.group(1), 16) != owner):
        raise ValueError("Native operand caller does not match the proposed owner")
    if ((hits[0].get("registers") or {}).get("r4", "").lower() != f"0x{left:08x}" or
            (hits[0].get("registers") or {}).get("r5", "").lower() !=
            f"0x{right:08x}"):
        raise ValueError("Original operand registers do not match sampled addresses")
    original_left = _memory(hits[0].get("memory"), left)
    original_right = _memory(hits[0].get("memory"), right)
    native_left = _memory(native.get("memory"), left)
    native_right_address = _address(native.get("right_address"))
    native_right = _memory(native.get("memory"), native_right_address)
    if (original_left != native_left or original_right == native_right or
            native.get("right_data") != native_right.hex()):
        raise ValueError("Operand provenance does not isolate a differing input")
    context = compact(image, owner, 262144, True, return_pc, full_original=True)
    helper_context = compact(image, helper, 262144, True, helper, full_original=True)
    listing = "\n".join(context["mips"]["lines"]).lower()
    callee_call = re.search(r"^([0-9a-f]{8})\s+.*\bjal\s+\S*" +
                            f"{callee:08x}" + r"\b", listing, re.MULTILINE)
    helper_calls = [int(row.group(1), 16) for row in re.finditer(
        r"^([0-9a-f]{8})\s+.*\bjal\s+\S*" + f"{helper:08x}" + r"\b",
        listing, re.MULTILINE)]
    if (not any(str(line).lower().startswith(f"{return_pc:08x}")
                for line in context["mips"]["lines"]) or
            callee_call is None or int(callee_call.group(1), 16) + 8 != return_pc or
            not any(site < int(callee_call.group(1), 16) for site in helper_calls) or
            any((row.get("complete") is not True) for row in
                (context.get("implementation") or {}, context["mips"],
                 helper_context.get("implementation") or {}, helper_context["mips"]))):
        raise ValueError("Operand provenance audit source is incomplete")
    for row in (context, helper_context):
        row["machine_audit"] = checklist(row["mips"]["lines"])
    helper_context["audit_role"] = "inlined_callee"
    helper_context["audit_source"] = {
        key: (context.get("implementation") or {}).get(key)
        for key in ("path", "symbol", "start_line", "text", "complete")
    }
    context["audit_cluster"] = [helper_context]
    context["audit_contract"] = {
        "pseudocode_baseline_required": False,
        "complete_original_required": True,
        "complete_mips_audit_required": True,
        "audit_direct_helpers": True,
        "semantic_simplification_allowed": False,
        "single_observed_difference_patch_allowed": False,
        "required_receipt_contract": "xport-lockstep-function-audit-v1",
        "required_functions": [_audit_function_binding(row) for row in
                               (context, helper_context)],
    }
    evidence = {
        "diagnostic_file": name, "sha256": digest, "frame": probe["target_frame"] + 1,
        "callee": f"0x{callee:08x}", "return_pc": f"0x{return_pc:08x}",
        "native_symbol": native["symbol"], "native_caller": native["caller_symbol"],
        "left_address": f"0x{left:08x}", "original_left": original_left.hex(),
        "native_left": native_left.hex(), "right_address": f"0x{right:08x}",
        "original_right": original_right.hex(), "native_right": native_right.hex(),
    }
    packet = copy.deepcopy(previous)
    packet["source_context"] = context
    packet["diagnostic_owner"] = {
        "kind": "paired_operand_producer", "function": f"0x{owner:08x}",
        "downstream_writer": f"0x{callee:08x}", "return_pc": f"0x{return_pc:08x}",
    }
    packet["operand_provenance"] = evidence
    packet["cause_statement"] = (
        "The left operand matches at the downstream call entry, while its right "
        "operand already differs; audit the caller and named helper before changing the writer"
    )
    packet["failure_signature"] = hashlib.sha256(json.dumps({
        "prior": previous["failure_signature"], "owner": owner, "helper": helper,
        "evidence": evidence,
    }, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    packet["serialized_bytes"] = len(json.dumps(
        packet, sort_keys=True, separators=(",", ":")).encode())
    if packet["serialized_bytes"] > packet["budget_bytes"]:
        raise ValueError("Operand provenance packet exceeds its byte budget")
    build_prompt(packet, root=root, source_roots=source_roots)
    return packet

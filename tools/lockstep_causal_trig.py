"""Prove a game sine table mismatch before proposing a rotation repair"""

from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import re
import struct

from lockstep_audit import checklist
from lockstep_source_coverage import index_functions
from lockstep_supervisor import source_snapshot
from source_context import compact


def _paired(snapshot: dict, address: int, size: int) -> tuple[bytes, bytes]:
    alignment = snapshot.get("alignment") or {}
    if (alignment.get("aligned") is not True or
            (alignment.get("comparison") or {}).get("status") != "match"):
        raise ValueError("Causal memory probe is not aligned")
    result = []
    for side in ("original", "native"):
        rows = snapshot.get(f"{side}_memory_at_alignment") or []
        matches = [row for row in rows if row.get("address") == f"0x{address:08X}" and
                   row.get("size") == size]
        if len(matches) != 1:
            raise ValueError("Causal memory probe is incomplete")
        data = bytes.fromhex(matches[0]["data"])
        if len(data) != size:
            raise ValueError("Causal memory probe has the wrong size")
        result.append(data)
    return result[0], result[1]


def _psyq_quarter(root: Path) -> list[int]:
    shared = Path(__file__).resolve().parents[1] / "src" / "psx.c"
    text = shared.read_text(encoding="utf-8")
    match = re.search(r"psyq_sine_quarter\[1025\]\s*=\s*\{([^}]*)\}", text, re.S)
    if match is None:
        raise ValueError("Native sine table is unavailable")
    values = [int(value) for value in re.findall(r"\d+", match.group(1))]
    if len(values) != 1025:
        raise ValueError("Native sine table has the wrong size")
    return values


def _native_sine(table: list[int], angle: int) -> int:
    index = angle & 0xfff
    if index <= 0x400:
        return table[index]
    if index <= 0x800:
        return table[0x800 - index]
    if index <= 0xc00:
        return -table[index - 0x800]
    return -table[0x1000 - index]


def _rotate(matrix: bytes, cosine: int, sine: int) -> list[int]:
    values = list(struct.unpack_from("<9h", matrix))
    result = values[:]
    for column in range(3):
        first, third = values[column], values[6 + column]
        result[column] = (cosine * first + sine * third) >> 12
        result[6 + column] = (-sine * first + cosine * third) >> 12
    return result


def validate_trig_cause(root: Path, snapshots: dict[str, dict], request: dict) -> dict:
    angle_address = int(request["angle_address"], 0)
    source_address = int(request["source_matrix_address"], 0)
    table_base = int(request["table_base_address"], 0)
    matrix_address = int(request["matrix_address"], 0)
    original_angle, native_angle = _paired(snapshots["angle"], angle_address, 4)
    source_original, source_native = _paired(snapshots["source"], source_address, 32)
    matrix_original, matrix_native = _paired(snapshots["matrix"], matrix_address, 32)
    if (original_angle != native_angle or source_original != source_native or
            matrix_original == matrix_native):
        raise ValueError("Trig inputs or matrix divergence are not proven")
    raw_angle = struct.unpack("<i", original_angle)[0]
    angle = -(abs(raw_angle) // 2) if raw_angle < 0 else raw_angle // 2
    lookup_address = table_base + ((abs(angle) & 0xfff) << 2)
    trig_original, trig_native = _paired(snapshots["table"], lookup_address, 4)
    if trig_original != trig_native:
        raise ValueError("Game sine table differs between participants")
    packed = struct.unpack("<I", trig_original)[0]
    original_sine, original_cosine = struct.unpack("<2h", trig_original)
    if angle < 0:
        original_sine = -original_sine
    table = _psyq_quarter(root)
    native_sine = _native_sine(table, angle)
    native_cosine = _native_sine(table, angle + 0x400)
    original_result = _rotate(source_original, original_cosine, original_sine)
    native_result = _rotate(source_native, native_cosine, native_sine)
    if (original_result != list(struct.unpack_from("<9h", matrix_original)) or
            native_result != list(struct.unpack_from("<9h", matrix_native))):
        raise ValueError("Trig values do not reproduce both scratchpad matrices")
    if original_sine != native_sine or original_cosine == native_cosine:
        raise ValueError("Trig mismatch is not isolated to the cosine lookup")
    return {
        "angle": angle, "lookup_address": f"0x{lookup_address:08x}",
        "game_table_word": f"0x{packed:08x}",
        "original_sine": original_sine, "native_sine": native_sine,
        "original_cosine": original_cosine, "native_cosine": native_cosine,
        "source_matrix": list(struct.unpack_from("<9h", source_original)),
        "original_matrix": original_result, "native_matrix": native_result,
    }


def build_trig_packet(root: Path, previous: dict, evidence: dict,
                      request: dict) -> dict:
    packet = copy.deepcopy(previous)
    image = request["image"]
    address = int(request["original_function"], 0)
    context = compact(image, address, 262144, True, address, full_original=True)
    source = source_snapshot(root, (Path("src"),))
    if source["identity"] != packet["identities"]["native_source_identity"]:
        raise ValueError("Source changed before trig causal audit")
    path = request["source_file"]
    symbol = request["source_symbol"]
    if (Path(path).is_absolute() or ".." in Path(path).parts or
            not Path(path).is_relative_to(Path("src"))):
        raise ValueError("Trig helper source is outside game ownership")
    index = index_functions(root, {path: source["files"][path]})
    function = index[(path, symbol)]
    lines = (root / path).read_text(encoding="utf-8-sig").splitlines()
    body = "\n".join(lines[function["start_line"] - 1:function["end_line"]])
    context["implementation"] = {
        "path": path, "symbol": symbol,
        "start_line": function["start_line"], "end_line": function["end_line"],
        "text": body, "complete": True,
        "sha256": hashlib.sha256(body.encode()).hexdigest(),
    }
    context["machine_audit"] = checklist(context["mips"]["lines"])
    binding = {
        "image": image, "address": address,
        "function_sha256": context["function"]["sha256"],
        "mips_sha256": context["mips"]["sha256"],
        "pseudocode_sha256": context["pseudocode"]["sha256"],
        "pseudocode_complete": context["pseudocode"].get("complete") is True,
        "machine_audit_sha256": context["machine_audit"]["sha256"],
    }
    context["audit_contract"] = {
        "pseudocode_baseline_required": False,
        "complete_original_required": True,
        "complete_mips_audit_required": True,
        "audit_direct_helpers": True,
        "semantic_simplification_allowed": False,
        "single_observed_difference_patch_allowed": False,
        "required_receipt_contract": "xport-lockstep-function-audit-v1",
        "required_functions": [binding],
    }
    context["audit_cluster"] = []
    packet["source_context"] = context
    packet["diagnostic_owner"] = {
        "kind": "original_trig_helper", "function": f"0x{address:08x}",
        "writer_function": (previous.get("diagnostic_owner") or {}).get("writer_function"),
        "writer_pc": (previous.get("diagnostic_owner") or {}).get("writer_pc"),
    }
    packet.pop("gte_causal_evidence", None)
    packet["trig_causal_evidence"] = evidence
    packet["cause_statement"] = (
        "Game cosine and native rcos differ; their values reproduce the original and "
        "native scratchpad matrices byte-for-byte before the later GTE and RAM divergence"
    )
    proof = json.dumps(evidence, sort_keys=True, separators=(",", ":"))
    packet["failure_signature"] = hashlib.sha256(
        f"{previous['failure_signature']}:trig:{proof}".encode()).hexdigest()
    packet["case_key"] = hashlib.sha256(
        f"{previous['case_key']}:trig".encode()).hexdigest()
    packet["serialized_bytes"] = len(json.dumps(
        packet, sort_keys=True, separators=(",", ":")).encode())
    if packet["serialized_bytes"] > packet["budget_bytes"]:
        raise ValueError("Trig causal packet exceeds its byte budget")
    return packet


def read_trig_request(root: Path, directory: Path, request: dict,
                      previous: dict) -> dict:
    required = {"kind", "angle", "source", "table", "matrix", "angle_address",
                "source_matrix_address", "table_base_address", "matrix_address",
                "image", "original_function", "source_file", "source_symbol"}
    if set(request) != required or request.get("kind") != "trig_table":
        raise ValueError("Invalid trig causal request")
    snapshots = {}
    for key in ("angle", "source", "table", "matrix"):
        name = request[key]
        if (not isinstance(name, str) or Path(name).name != name or
                not name.startswith("diagnostic-result-") or
                not name.endswith(".json")):
            raise ValueError("Trig causal diagnostic name is invalid")
        path = (directory / name).resolve()
        if not path.is_relative_to(directory.resolve()) or not path.is_file():
            raise ValueError("Trig causal diagnostic is missing")
        snapshots[key] = json.loads(path.read_text(encoding="utf-8"))
    evidence = validate_trig_cause(root, snapshots, request)
    return build_trig_packet(root, previous, evidence, request)

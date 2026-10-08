"""Adopt a completed writer refinement without replaying or changing source"""

from __future__ import annotations

import json
import hashlib
import math
from pathlib import Path
import re

from lockstep_packet import build_packet
from lockstep_repair_context import build_prompt


def read_refined_request(root: Path, directory: Path, request: dict,
                         previous: dict, result: dict, source_identity: str,
                         source_roots: tuple[Path, ...]) -> dict:
    if set(request) != {"kind", "run_name"} or request.get("kind") != "writer_refinement":
        raise ValueError("Invalid writer refinement request")
    name = request.get("run_name")
    if (not isinstance(name, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,40}", name)
            or name == directory.name):
        raise ValueError("Invalid writer refinement run name")
    report_path = root / "status" / "lockstep-converge" / name / "refinement.json"
    report_bytes = report_path.read_bytes()
    report = json.loads(report_bytes.decode("utf-8"))
    if (report.get("schema") != 1 or report.get("source_run") != directory.name or
            report.get("source_identity") != source_identity or
            report.get("comparison") != result.get("comparison")):
        raise ValueError("Writer refinement does not match the waiting defect")
    evidence = report.get("evidence") or {}
    if evidence.get("interval_complete") is not True:
        raise ValueError("Writer refinement interval is incomplete")
    seconds = report.get("seconds")
    if (type(seconds) not in (int, float) or not math.isfinite(seconds) or
            seconds < 0):
        raise ValueError("Writer refinement duration is invalid")
    packet = build_packet(result, evidence, previous["identities"],
                          previous["compiled_memory_exceptions"], previous["reproduce"])
    if packet.get("case_key") != previous.get("case_key"):
        raise ValueError("Writer refinement changed defect identity")
    packet["external_refinement"] = {
        "run_name": name, "report_sha256": hashlib.sha256(report_bytes).hexdigest(),
        "seconds": seconds,
        "original_writes": len(evidence.get("original_writes") or []),
        "native_writes": len(evidence.get("native_writes") or []),
    }
    packet["serialized_bytes"] = len(json.dumps(
        packet, sort_keys=True, separators=(",", ":")).encode("utf-8"))
    if packet["serialized_bytes"] > packet["budget_bytes"]:
        raise ValueError("Writer refinement packet exceeds its byte budget")
    build_prompt(packet, root=root, source_roots=source_roots)
    return packet


def validate_external_refinement(state: dict, packet: dict) -> bool:
    row = packet.get("external_refinement") or {}
    name = row.get("run_name")
    if not isinstance(name, str):
        raise ValueError("Adopted packet lacks an external refinement identity")
    history = state.get("external_refinements") or []
    prior = next((item for item in history if item.get("run_name") == name), None)
    if prior is not None:
        if prior != row:
            raise ValueError("External refinement report changed after adoption")
        return True
    return False


def record_external_refinement(state: dict, packet: dict) -> bool:
    if validate_external_refinement(state, packet):
        return False
    row = packet["external_refinement"]
    history = state.setdefault("external_refinements", [])
    history.append(dict(row))
    telemetry = state.setdefault("telemetry", {})
    telemetry["external_refinement_pairs"] = telemetry.get("external_refinement_pairs", 0) + 1
    telemetry["external_refinement_seconds"] = (
        telemetry.get("external_refinement_seconds", 0.0) + row["seconds"])
    return True

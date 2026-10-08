"""Capture one bounded writer probe for an existing lockstep divergence"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
import time

from lockstep_project_runtime import ProjectLockstepHooks
from lockstep_packet import build_packet
from lockstep_record import load_record
from lockstep_supervisor import source_snapshot
from lockstep_writer_timeline import correlate_writers
from trace_workflow import name_check
from windows_desktop import launch_default_desktop
from xport_project import load_project


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--record", required=True)
    parser.add_argument("--source-run", required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--attempt", type=int)
    parser.add_argument("--stop-after-writes", type=int, default=0)
    parser.add_argument("--desktop-child", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    name_check(args.record)
    name_check(args.source_run)
    name_check(args.name)
    root, config = load_project(args.project)
    record = load_record(root / "status" / "lockstep-records" / args.record / "record.json")
    source_dir = root / "status" / "lockstep-converge" / args.source_run
    output_dir = root / "status" / "lockstep-converge" / args.name
    if output_dir.exists():
        raise ValueError("Refinement output already exists")
    state = json.loads((source_dir / "state.json").read_text(encoding="utf-8"))
    attempts = state.get("attempts") or []
    if not attempts:
        raise ValueError("Source run has no attempts")
    selected = attempts[-1] if args.attempt is None else next(
        (attempt for attempt in attempts if attempt.get("attempt") == args.attempt), None)
    if selected is None:
        raise ValueError("Source attempt is unavailable")
    if (selected.get("result") or {}).get("status") != "memory_divergence":
        raise ValueError("Source run lacks a terminal memory divergence")
    result = selected["result"]
    packet_owner = selected if selected.get("packet") else next(
        (attempt for attempt in reversed(attempts) if attempt.get("packet")), None)
    if packet_owner is None:
        raise ValueError("Source attention packet is unavailable")
    source_packet_path = Path(packet_owner["packet"])
    if not source_packet_path.is_file() or not source_packet_path.resolve().is_relative_to(source_dir):
        raise ValueError("Source attention packet is unavailable")
    source_packet = json.loads(source_packet_path.read_text(encoding="utf-8"))
    roots = tuple(Path(value) for value in config["lockstep"].get("source_roots", ["src"]))
    identity = source_snapshot(root, roots)["identity"]
    if identity != selected.get("source_identity"):
        raise ValueError("Source changed since the divergence")
    if not args.desktop_child:
        command = [sys.executable, "-B", str(Path(__file__).resolve()),
                   "--project", str(root), "--record", args.record,
                   "--source-run", args.source_run, "--name", args.name,
                   "--desktop-child"]
        if args.attempt is not None:
            command.extend(("--attempt", str(args.attempt)))
        if args.stop_after_writes:
            command.extend(("--stop-after-writes", str(args.stop_after_writes)))
        pid = launch_default_desktop(command, root, visible=True)
        print(json.dumps({"status": "started", "pid": pid,
                          "desktop": "WinSta0\\Default", "output": str(output_dir)}))
        return 0
    output_dir.mkdir(parents=True)
    hooks = ProjectLockstepHooks(root, config, args.name, record, telemetry="none")
    started = time.monotonic()
    try:
        evidence = hooks.refine(
            result, record, stop_after_writes=args.stop_after_writes)
        report = {"schema": 1, "source_run": args.source_run,
                  "source_attempt": selected.get("attempt"),
                  "source_identity": identity,
                  "comparison": result.get("comparison"),
                  "seconds": round(time.monotonic() - started, 3),
                  "evidence": evidence,
                  "writer_timeline": correlate_writers(
                      evidence["original_writes"], evidence["native_writes"])}
        packet = build_packet(
            result, evidence, source_packet["identities"],
            source_packet["compiled_memory_exceptions"], source_packet["reproduce"])
        if selected.get("packet") and packet["case_key"] != source_packet["case_key"]:
            raise RuntimeError("Refinement changed defect identity")
        refined_path = output_dir / "attention-refined.json"
        refined_path.write_text(json.dumps(packet, indent=2) + "\n", encoding="utf-8")
        report["refined_packet"] = str(refined_path)
        (output_dir / "refinement.json").write_text(
            json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({"status": "complete", "output": str(output_dir / "refinement.json"),
                          "seconds": report["seconds"],
                          "original_writes": len(evidence["original_writes"]),
                          "native_writes": len(evidence["native_writes"])}))
        return 0
    except Exception as error:
        (output_dir / "error.json").write_text(json.dumps({
            "schema": 1, "error": str(error)}, indent=2) + "\n", encoding="utf-8")
        raise


if __name__ == "__main__":
    raise SystemExit(main())

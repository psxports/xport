"""Run one bounded converge diagnosis without invoking or waiting for a model"""

from __future__ import annotations

import argparse
import copy
import json
from pathlib import Path
import sys

from lockstep_metrics import summarize
from lockstep_project_runtime import ProjectLockstepHooks
from lockstep_record import load_record
from lockstep_supervisor import LockstepSupervisor
from trace_workflow import name_check
from windows_desktop import launch_default_desktop
from xport_project import load_project


def diagnostic_config(config: dict) -> dict:
    result = copy.deepcopy(config)
    result["lockstep"].pop("repair_executor", None)
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--record", required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--desktop-child", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    name_check(args.name)
    root, config = load_project(args.project)
    config = diagnostic_config(config)
    if args.name == args.record:
        raise ValueError("Benchmark name must differ from the source record")
    record_path = root / "status" / "lockstep-records" / args.record / "record.bin"
    record = load_record(record_path)
    directory = root / "status" / "lockstep-converge" / args.name
    if directory.exists():
        raise ValueError("Benchmark output already exists; choose a fresh name")
    if not args.desktop_child:
        command = [sys.executable, "-B", str(Path(__file__).resolve()),
                   "--project", str(root), "--record", args.record,
                   "--name", args.name, "--desktop-child"]
        pid = launch_default_desktop(command, root, visible=True)
        print(json.dumps({"status": "started", "pid": pid,
                          "desktop": "WinSta0\\Default", "output": str(directory)}))
        return 0
    hooks = ProjectLockstepHooks(root, config, args.name, record, telemetry="none")
    hooks.live_repair_wait = False
    supervisor = LockstepSupervisor(
        root, directory, record, hooks,
        source_roots=tuple(Path(value) for value in config["lockstep"].get("source_roots", ["src"])),
        max_repairs=1,
        max_steps=int(config["lockstep"].get("max_events", 1_000_000)),
    )
    result = supervisor.run_interactive()
    report = summarize(supervisor.state)
    report["result_status"] = result["status"]
    report["source_record"] = args.record
    (directory / "benchmark.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, sort_keys=True))
    return 0 if result["status"] in {"ATTENTION_REQUIRED", "COMPLETE"} else 1


if __name__ == "__main__":
    raise SystemExit(main())

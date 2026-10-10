"""Public lockstep record and converge entry points"""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

from trace_worker import alive, process_identity, write_receipt
from windows_desktop import launch_default_desktop, process_desktop
from xport_process import normalized_environment
from xport_project import load_project


def name_check(name):
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,40}", name):
        raise ValueError("Invalid recording name")
    return name


def require_lockstep(config):
    lockstep = config.get("lockstep") or {}
    if lockstep.get("schema") != 1 or lockstep.get("enabled") is not True:
        raise ValueError("LOCKSTEP_PROJECT_NOT_MIGRATED")
    if lockstep.get('repair_executor') == 'codex_cli':
        raise ValueError('CODEX_CLI_REPAIR_REMOVED: use the manual proposal inbox')
    return lockstep


def resolve_state(root, value):
    state = Path(value)
    if not state.is_absolute():
        state = root / state
    state = state.resolve()
    if not state.is_file():
        raise ValueError("STATE_REQUIRED: exact initial save state does not exist")
    return state


def active_record_names(root):
    records = root / "status" / "lockstep-records"
    if not records.is_dir():
        return []
    active = []
    for session_path in records.glob("*/session.json"):
        try:
            session = json.loads(session_path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        if session.get("status") in {"starting", "recording"}:
            active.append(session_path.parent.name)
    return sorted(active)


def active_record_name(root):
    active = active_record_names(root)
    if not active:
        raise ValueError("LOCKSTEP_RECORD_SESSION_REQUIRED")
    if len(active) != 1:
        raise ValueError("LOCKSTEP_RECORD_SINGLETON_VIOLATION: " + ", ".join(active))
    return active[0]


def record(root, config, state_value, name, frames, timeout):
    require_lockstep(config)
    name_check(name)
    if not 1 <= frames <= 1_000_000:
        raise ValueError("FRAMES_REQUIRED: frames must be in 1..1000000")
    state = resolve_state(root, state_value)
    output = root / "status" / "lockstep-records" / name / "record.json"
    if output.exists():
        raise ValueError("LOCKSTEP_RECORD_EXISTS: choose a new recording name")
    from lockstep_record_cli import record as record_lockstep
    result = record_lockstep(root, state, output, frames, timeout)
    result["duckstation_launch_reason"] = "record_capture"
    return result


def stop_record(root, config, name, wait):
    require_lockstep(config)
    if name is None:
        name = active_record_name(root)
    name_check(name)
    directory = root / "status" / "lockstep-records" / name
    session_path = directory / "session.json"
    if not session_path.is_file():
        raise ValueError("LOCKSTEP_RECORD_SESSION_REQUIRED")
    session = json.loads(session_path.read_text(encoding="utf-8"))
    if session.get("status") == "complete":
        return session
    if session.get("status") != "recording":
        raise ValueError(f"LOCKSTEP_RECORD_NOT_ACTIVE: {session.get('status')}")
    (directory / "stop.request").write_text("stop\n", encoding="ascii")
    deadline = time.monotonic() + wait
    while time.monotonic() < deadline:
        session = json.loads(session_path.read_text(encoding="utf-8"))
        if session.get("status") in {"complete", "failed"}:
            return session
        time.sleep(0.05)
    return {"status": "stopping", "name": name, "session": str(session_path)}


def converge(name, wait, telemetry, reprobe=False, run_name=None):
    name_check(name)
    run_name = name_check(run_name or name)
    root, config = load_project()
    require_lockstep(config)
    record_path = root / "status" / "lockstep-records" / name / "record.json"
    if not record_path.is_file():
        raise ValueError("LOCKSTEP_RECORD_REQUIRED: record the scenario before converge")
    state_path = root / "status" / "lockstep-converge" / run_name / "state.json"
    if run_name != name and state_path.is_file():
        from lockstep_record import load_record
        state = json.loads(state_path.read_text(encoding="utf-8"))
        if state.get("record_id") != load_record(record_path)["record_id"]:
            raise ValueError("Converge run belongs to another or unbound record")
    from lockstep_runtime import control
    return control(root, config, run_name, record_path, wait, telemetry, reprobe=reprobe)


def launch_converge(root, name, wait, telemetry, reprobe=False, run_name=None):
    name_check(name)
    run_name = name_check(run_name or name)
    directory = root / "status" / "lockstep-converge" / run_name
    directory.mkdir(parents=True, exist_ok=True)
    receipt_path = directory / "supervisor.json"
    if receipt_path.is_file():
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        if alive(receipt.get("process")):
            if reprobe:
                raise ValueError("Stop the active supervisor before requesting a reprobe")
            if receipt.get("record", receipt.get("name")) != name:
                raise ValueError("Active converge run belongs to another record")
            return {"status": "running", "name": run_name, "record": name,
                    "process": receipt["process"]}
    interpreter = Path(sys.executable)
    windowed_interpreter = interpreter.with_name("pythonw.exe")
    if os.name == "nt" and windowed_interpreter.is_file():
        interpreter = windowed_interpreter
    command = [
        str(interpreter), "-B", str(Path(__file__).resolve()), "converge", name,
        "--desktop-child", "--wait", str(wait), "--telemetry", telemetry,
    ]
    if run_name != name:
        command.extend(("--run-name", run_name))
    if reprobe:
        command.append("--reprobe")
    pid = launch_default_desktop(
        command, root, visible=True, environment=normalized_environment())
    identity = process_identity(pid)
    if identity is None:
        raise RuntimeError("Lockstep supervisor exited during desktop launch")
    receipt = {
        "schema": 1, "contract": "xport-lockstep-supervisor-process-v1",
        "name": run_name, "record": name, "process": identity,
        "desktop": process_desktop(),
    }
    write_receipt(receipt_path, receipt)
    return {"status": "running", "name": run_name, "record": name, "process": identity,
            "receipt": str(receipt_path)}


def status(root, config, name):
    require_lockstep(config)
    name_check(name)
    recording = root / "status" / "lockstep-records" / name / "session.json"
    if recording.is_file():
        value = json.loads(recording.read_text(encoding="utf-8"))
        worker = value.get("worker")
        value["worker_live"] = alive(worker) if worker else False
        return value
    path = root / "status" / "lockstep-converge" / name / "state.json"
    if not path.is_file():
        return {"status": "idle", "reason": "No lockstep converge state"}
    return json.loads(path.read_text(encoding="utf-8"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("record", "stop", "converge", "status"))
    parser.add_argument("values", nargs="*")
    parser.add_argument("--frames", type=int)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--wait", type=float, default=30.0)
    parser.add_argument("--telemetry", choices=("codex", "none"), default="codex")
    parser.add_argument("--reprobe", action="store_true")
    parser.add_argument("--run-name")
    parser.add_argument("--desktop-child", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    root, config = load_project()
    if not 0 <= args.wait <= 60:
        parser.error("--wait must be in 0..60")
    try:
        if args.action == "record" and len(args.values) == 2:
            if args.frames is None:
                raise ValueError("INTERACTIVE_RECORD_ENTRYPOINT_REQUIRED: use tools/record.ps1")
            result = record(root, config, args.values[0], args.values[1], args.frames, args.timeout)
        elif args.action == "stop" and not args.values:
            result = stop_record(root, config, None, args.wait)
        elif args.action == "converge" and len(args.values) == 1:
            if os.name == "nt" and not args.desktop_child:
                result = launch_converge(root, args.values[0], args.wait, args.telemetry,
                                         reprobe=args.reprobe, run_name=args.run_name)
            else:
                result = converge(args.values[0], args.wait, args.telemetry,
                                  reprobe=args.reprobe, run_name=args.run_name)
        elif args.action == "status" and len(args.values) == 1:
            result = status(root, config, args.values[0])
        else:
            parser.error(
                "Use record STATE NAME [--frames N], stop, converge NAME, or status NAME"
            )
    except (ValueError, OSError, RuntimeError, subprocess.SubprocessError) as error:
        result = {"status": "attention_required", "error": str(error)}
    if args.desktop_child and args.action == "converge" and len(args.values) == 1:
        child_name = name_check(args.run_name or args.values[0])
        child_result = root / "status" / "lockstep-converge" / child_name / "desktop-child-result.json"
        write_receipt(child_result, result)
    print(json.dumps(result, separators=(",", ":")))
    return 0 if result.get("status") in {
        "recorded", "recording", "stopping", "complete", "COMPLETE", "MATCH", "running", "idle", "unchanged"
    } else 1


if __name__ == "__main__":
    raise SystemExit(main())

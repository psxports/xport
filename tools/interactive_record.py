"""Launch manual recording on the Windows interactive desktop"""

import argparse
import json
from pathlib import Path
import sys
import time

from trace_worker import alive, process_identity, write_receipt
from trace_workflow import (active_record_names, name_check, require_lockstep,
                            resolve_state, status, stop_record)
from lockstep_record_slot import resolve_slot
from windows_desktop import launch_default_desktop
from xport_project import load_project


def start(root, config, state_value, name, timeout, *, slot=None):
    require_lockstep(config)
    name_check(name)
    active = active_record_names(root)
    if active:
        raise ValueError("LOCKSTEP_RECORD_ACTIVE: " + ", ".join(active))
    if (state_value is None) == (slot is None):
        raise ValueError("Choose exactly one save state file or slot")
    state = resolve_slot(root, config, slot) if slot is not None else resolve_state(root, state_value)
    directory = root / "status" / "lockstep-records" / name
    if directory.exists():
        raise ValueError("LOCKSTEP_RECORD_EXISTS: choose a new recording name")
    directory.mkdir(parents=True)
    pythonw = Path(sys.executable).with_name("pythonw.exe")
    if not pythonw.is_file():
        directory.rmdir()
        raise RuntimeError("pythonw.exe is required for interactive recording")
    command = [
        str(pythonw), "-B", str(Path(__file__).with_name("lockstep_record_session.py")),
        "--project", str(root), "--state", str(state), "--name", name,
        "--timeout", str(timeout),
    ]
    try:
        pid = launch_default_desktop(command, root)
        worker = process_identity(pid)
        if not worker:
            raise RuntimeError("Interactive recorder exited during launch")
        write_receipt(directory / "launch.json", {
            "schema": 1, "contract": "xport-interactive-record-launch-v1",
            "desktop": "WinSta0\\Default", "name": name, "worker": worker,
        })
    except Exception:
        if directory.exists() and not any(directory.iterdir()):
            directory.rmdir()
        raise
    session_path = directory / "session.json"
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if session_path.is_file():
            session = json.loads(session_path.read_text(encoding="utf-8"))
            if session.get("status") in {"recording", "failed"}:
                return session
        if not alive(worker):
            raise RuntimeError("Interactive recorder exited before arming")
        time.sleep(0.05)
    raise RuntimeError("Interactive recorder did not arm before timeout")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("start", "stop", "status"))
    parser.add_argument("name", nargs="?")
    parser.add_argument("--state")
    parser.add_argument("--slot", type=int)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--wait", type=float, default=30.0)
    args = parser.parse_args()
    root, config = load_project()
    try:
        if args.action == "start" and args.name:
            result = start(root, config, args.state, args.name, args.timeout,
                           slot=args.slot)
        elif args.action == "stop" and args.name is None:
            result = stop_record(root, config, None, args.wait)
        elif args.action == "status" and args.name:
            result = status(root, config, args.name)
        else:
            parser.error("Use start NAME, stop, or status NAME")
    except (OSError, RuntimeError, ValueError) as error:
        result = {"status": "attention_required", "error": str(error)}
    print(json.dumps(result, separators=(",", ":")))
    return 0 if result.get("status") in {"recording", "stopping", "complete", "idle"} else 1


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Run one visible input-only lockstep recording session"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import time

from duckstation_runtime import lockstep_timing_arguments
from duckstation_runtime import (DATA_ROOT_VARIABLE, configure_recording_settings,
                                 executable as duckstation_executable, launch_process)
from lockstep_gdb import DuckStationLockstep
from lockstep_record import FrameDoneRecorder, create_record, save_record
from lockstep_record_cli import disc_inventory, recording_timing
from trace_worker import process_identity, write_receipt
from xport_process import normalized_environment
from xport_project import load_project


def wait_port(host: str, port: int, process, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"DuckStation exited before GDB attach: {process.returncode}")
        try:
            with socket.create_connection((host, port), timeout=0.1):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("DuckStation did not expose its reserved GDB endpoint")


def update(path: Path, session: dict, **values) -> None:
    session.update(values)
    write_receipt(path, session)


def run(root: Path, state: Path, name: str, timeout: float) -> int:
    os.environ["XPORT_PROJECT"] = str(root)
    config = json.loads((root / "xport-project.json").read_text(encoding="utf-8"))
    lockstep = config["lockstep"]
    video_standard, field_rate = recording_timing(config)
    role = lockstep["runtime"].get("record_role", "user")
    host = config["duckstation"].get("host", "127.0.0.1")
    port = int(config["duckstation"]["reserved_gdb_ports"][role])
    directory = root / config["duckstation"]["data_directories"][role]
    recording_settings = configure_recording_settings(directory, port, hidden=False)
    cue = (root / config["paths"]["disc_image"]).resolve()
    original = (root / config["paths"]["original_executable"]).resolve()
    manifest_path = Path(__file__).resolve().parent / "duckstation" / "duckstation-manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    output = root / "status" / "lockstep-records" / name
    session_path = output / "session.json"
    stop_path = output / "stop.request"
    record_path = output / "record.bin"
    initial_state = output / "initial.sav"
    settings_snapshot = output / "settings.ini"
    session = {
        "schema": 1, "contract": "xport-lockstep-record-session-v1", "name": name,
        "status": "starting", "worker": process_identity(os.getpid()), "frames": 0,
        "initial_state": str(initial_state), "record": str(record_path),
        "recording_settings": recording_settings,
    }
    write_receipt(session_path, session)
    shutil.copy2(state, initial_state)
    shutil.copy2(directory / "settings.ini", settings_snapshot)
    process = None
    adapter = None
    try:
        command = [str(duckstation_executable()), *lockstep_timing_arguments(duckstation_executable()), "-fastboot", "-statefile",
                   str(initial_state), str(cue)]
        process = launch_process(
            command, directory,
            normalized_environment(updates={DATA_ROOT_VARIABLE: str(directory)}), True,
        )
        update(session_path, session, emulator=process_identity(process.pid))
        wait_port(host, port, process, timeout)
        adapter = DuckStationLockstep(host, port)
        stop = adapter.remote.packet("?")
        if not stop.startswith(("S", "T")):
            raise RuntimeError("DuckStation did not start paused")
        adapter.configure_wait_points(config["duckstation"]["wait_points"], config["duckstation"].get("sio_delay"))
        adapter.start_record()
        recorder = FrameDoneRecorder()
        update(session_path, session, status="recording", started=time.time())
        adapter.resume_record()
        while not stop_path.exists():
            if process.poll() is not None:
                raise RuntimeError(f"DuckStation exited during recording: {process.returncode}")
            time.sleep(0.05)
        capture_stopped = time.time()
        for event in adapter.finish_record():
            recorder.accept(event)
        if not recorder.frames:
            raise RuntimeError("Recording stopped before the first FrameDone")
        value = create_record(
            initial_state=initial_state, disc=disc_inventory(cue), executable=original,
            image=lockstep["original_image"],
            duckstation_commit=manifest["upstream"]["commit"],
            patch_sha256=manifest["patch"]["sha256"], settings=settings_snapshot,
            video_standard=video_standard, field_rate=field_rate, frames=recorder.frames,
        )
        save_record(record_path, value)
        update(session_path, session, status="complete", frames=len(recorder.frames),
               record_id=value["record_id"], capture_stopped=capture_stopped,
               finished=time.time())
        return 0
    except Exception as error:
        update(session_path, session, status="failed", error=str(error), finished=time.time())
        return 1
    finally:
        if adapter is not None:
            adapter.close()
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--state", type=Path, required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--timeout", type=float, default=30.0)
    args = parser.parse_args()
    return run(args.project.resolve(), args.state.resolve(), args.name, args.timeout)


if __name__ == "__main__":
    raise SystemExit(main())

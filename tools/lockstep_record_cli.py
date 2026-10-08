#!/usr/bin/env python3
"""Record effective FrameDone input for one lockstep scenario"""

from __future__ import annotations

import argparse
import json
import os
import socket
import subprocess
import time
from pathlib import Path

from duckstation_runtime import (DATA_ROOT_VARIABLE, executable as duckstation_executable,
                                 launch_process, validate_settings)
from lockstep_gdb import DuckStationLockstep
from lockstep_record import FrameDoneRecorder, create_record, save_record
from xport_process import normalized_environment
from xport_project import load_project


def wait_port(host: str, port: int, process, timeout: float = 30.0) -> None:
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


def disc_inventory(cue: Path) -> list[Path]:
    paths = [cue.resolve()]
    for line in cue.read_text(encoding="utf-8-sig").splitlines():
        stripped = line.strip()
        if not stripped.upper().startswith("FILE \""):
            continue
        end = stripped.find('"', 6)
        if end < 0:
            raise ValueError("Malformed CUE FILE entry")
        track = (cue.parent / stripped[6:end]).resolve()
        if track not in paths:
            paths.append(track)
    if any(not path.is_file() for path in paths):
        raise FileNotFoundError("CUE inventory contains a missing track")
    return paths


def recording_timing(config: dict) -> tuple[str, int]:
    """Read the configured video identity without assuming PAL for every title"""
    timing = config.get("lockstep", {}).get("timing", {"video_standard": "PAL", "field_rate": 50})
    standard = timing.get("video_standard")
    rate = timing.get("field_rate")
    if (standard, rate) not in (("PAL", 50), ("NTSC", 60)):
        raise ValueError("Unsupported recording video timing")
    return standard, rate


def record(root: Path, state: Path, output: Path, frames: int, timeout: float) -> dict:
    os.environ["XPORT_PROJECT"] = str(root)
    config = json.loads((root / "xport-project.json").read_text(encoding="utf-8"))
    lockstep = config["lockstep"]
    video_standard, field_rate = recording_timing(config)
    role = lockstep["runtime"]["duckstation_role"]
    host = config["duckstation"].get("host", "127.0.0.1")
    port = int(config["duckstation"]["reserved_gdb_ports"][role])
    directory = (root / config["duckstation"]["data_directories"][role]).resolve()
    directory = validate_settings(directory, port)
    cue = (root / config["paths"]["disc_image"]).resolve()
    original = (root / "orig" / lockstep["original_image"]).resolve()
    manifest_path = Path(__file__).resolve().parent / "duckstation" / "duckstation-manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    command = [str(duckstation_executable()), "-batch", "-fastboot", "-statefile",
               str(state.resolve()), str(cue)]
    process = launch_process(command, directory,
                             normalized_environment(updates={DATA_ROOT_VARIABLE: str(directory)}), False)
    adapter = None
    started = time.monotonic()
    try:
        wait_port(host, port, process)
        adapter = DuckStationLockstep(host, port)
        stop = adapter.remote.packet("?")
        if not stop.startswith(("S", "T")):
            raise RuntimeError("DuckStation did not start paused")
        adapter.start_record(frames)
        recorder = FrameDoneRecorder()
        capture_started = time.monotonic()
        adapter.wait_record_limit(timeout)
        capture_seconds = time.monotonic() - capture_started
        for event in adapter.finish_record():
            recorder.accept(event)
        if len(recorder.frames) != frames:
            raise RuntimeError(
                f"DuckStation recorded {len(recorder.frames)} frames instead of {frames}"
            )
        value = create_record(
            initial_state=state, disc=disc_inventory(cue), executable=original,
            image=lockstep["original_image"],
            duckstation_commit=manifest["upstream"]["commit"],
            patch_sha256=manifest["patch"]["sha256"], settings=directory / "settings.ini",
            video_standard=video_standard, field_rate=field_rate, frames=recorder.frames,
        )
        output.parent.mkdir(parents=True, exist_ok=True)
        save_record(output, value)
        return {
            "status": "recorded", "record": str(output.resolve()),
            "record_id": value["record_id"], "frames": len(recorder.frames),
            "source_first_frame": recorder.source_first_frame,
            "capture_seconds": capture_seconds,
            "capture_frames_per_second": len(recorder.frames) / capture_seconds,
            "elapsed_seconds": time.monotonic() - started,
        }
    finally:
        if adapter is not None:
            adapter.close()
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path)
    parser.add_argument("--state", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--frames", type=int, default=100)
    parser.add_argument("--timeout", type=float, default=30.0)
    args = parser.parse_args()
    if not 1 <= args.frames <= 1_000_000:
        parser.error("--frames must be in 1..1000000")
    if args.project:
        root = args.project.resolve()
    else:
        root, unused = load_project()
    print(json.dumps(record(root, args.state, args.output, args.frames, args.timeout),
                     sort_keys=True))


if __name__ == "__main__":
    main()

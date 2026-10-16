"""Exercise one DuckStation in-memory checkpoint without a native replay"""

import argparse
import hashlib
import json
import mmap
import os
from pathlib import Path
import socket
import struct
import subprocess
import time
import uuid

from duckstation_runtime import lockstep_timing_arguments
from duckstation_runtime import launch_process, runtime_identity
from gdb_remote import Remote
from lockstep_contract import mapping_name
from lockstep_record import load_record
from xport_process import normalized_environment


def expect(remote, command, reply):
    actual = remote.packet(command)
    if actual != reply:
        raise RuntimeError(f"{command}: expected {reply}, got {actual}")


def queue_input(remote, record, frame):
    ports = record["frames"][frame]["ports"]
    types = {"disconnected": 0, "digital": 1, "analog": 2}
    values = [f"{types[port['type']]:x},{port['buttons']:x},{port['analog']:x}"
              for port in ports]
    expect(remote, f"qXportLockstepInput:{frame:x}:{values[0]}:{values[1]}",
           f"OK:{frame:x}")


def next_event(remote):
    stop = remote.packet("c", timeout=30)
    if not stop.startswith(("S", "T")):
        raise RuntimeError(f"Unexpected GDB stop: {stop}")
    reply = remote.packet("qXportLockstepNextEvent")
    if not reply.startswith("EVENT:"):
        raise RuntimeError(f"Missing lockstep event: {reply}")
    return json.loads(bytes.fromhex(reply[6:]).decode("utf-8"))


def memory_digest(remote):
    digest = hashlib.sha256()
    for offset in range(0, 0x200000, 0x1000):
        digest.update(bytes.fromhex(remote.packet(f"m{offset:x},1000")))
    digest.update(bytes.fromhex(remote.packet("m1f800000,400")))
    return digest.hexdigest()


def probe(project, emulator, frame, rolling_captures=False, record_name="warmup"):
    runtime = runtime_identity(emulator)
    if runtime["lockstep_protocol_version"] != 23:
        raise ValueError("Checkpoint probe mapping layout requires protocol 23")
    config = json.loads((project / "xport-project.json").read_text(encoding="utf-8"))
    port = int(config["duckstation"]["reserved_gdb_ports"]["lockstep"])
    data = project / config["duckstation"]["data_directories"]["lockstep"]
    if not record_name or Path(record_name).name != record_name or record_name in (".", ".."):
        raise ValueError("Record name must be one directory component")
    record_dir = project / "status" / "lockstep-records" / record_name
    record = load_record(record_dir / "record.bin")
    if frame < 0 or frame + 1 >= len(record["frames"]):
        raise ValueError("Probe frame must precede another recorded frame")
    with socket.socket() as check:
        if check.connect_ex(("127.0.0.1", port)) == 0:
            raise RuntimeError(f"GDB port {port} is already in use")

    run_id = uuid.uuid4().hex
    dram = mapping_name(run_id, "dram")
    scratchpad = mapping_name(run_id, "scratchpad")
    deferred = mapping_name(run_id, "deferred")
    vram = f"Local\\xport-lockstep-{run_id}-vram"
    deferred_size = (10 + 3 + 32 * 3 + 511 * 3 + 7 + 4096 * 5 + 2) * 4
    mappings = [mmap.mmap(-1, 0x200000, tagname=dram),
                mmap.mmap(-1, 0x400, tagname=scratchpad),
                mmap.mmap(-1, deferred_size, tagname=deferred),
                mmap.mmap(-1, 0x100000, tagname=vram)]
    mappings[2][:8] = struct.pack("<II", 0x44504C58, 12)
    disc = Path(record["disc"][0]["path"])
    state = Path(record["initial_state"]["path"])
    command = [str(emulator), *lockstep_timing_arguments(emulator), "-batch", "-fastboot", "-statefile", str(state), str(disc)]
    environment = normalized_environment(updates={
        "XPORT_DUCKSTATION_DATA_ROOT": str(data),
        "XPORT_GDB_RAM_BACKEND": "rsp",
    })
    process = None
    remote = None
    try:
        process = launch_process(command, data, environment, False)
        deadline = time.monotonic() + 30
        while remote is None:
            if process.poll() is not None:
                raise RuntimeError(f"DuckStation exited with {process.returncode}")
            try:
                remote = Remote("127.0.0.1", port)
            except OSError:
                if time.monotonic() >= deadline:
                    raise RuntimeError("DuckStation GDB endpoint did not open")
                time.sleep(0.1)
        if not remote.packet("?").startswith(("S", "T")):
            raise RuntimeError("DuckStation did not start paused")
        expect(remote, f"qXportLockstepAttach:{run_id}:{dram}:{scratchpad}:{deferred}", f"OK:{run_id}")
        wait_points = ":".join(f"{int(pc, 0):x}" for pc in config["duckstation"]["wait_points"])
        expect(remote, f"qXportWaitPoints:{wait_points}", "OK:wait")
        if config["duckstation"].get("sio_delay"):
            delay = ":".join(f"{int(pc, 0):x}" for pc in config['duckstation']['sio_delay'])
            expect(remote, f"qXportSioDelay:{delay}", "OK:sio")
        record_id = record["record_id"]
        expect(remote, f"qXportLockstepReplay:{record_id}", f"OK:{record_id}")
        expect(remote, f"qXportLockstepSeed:{record_id}", f"OK:{record_id}")
        target = frame + 2
        expect(remote, f"qXportLockstepMode:PREFIX_GUARD:{target:x}",
               f"OK:PREFIX_GUARD:{target:x}")
        expect(remote, "qXportLockstepStart", "OK:start")
        queue_input(remote, record, 0)
        capture = None
        captures = 0
        for index in range(frame + 1):
            event = next_event(remote)
            if event["kind"] != "VBLANK" or event["frame"] != index:
                raise RuntimeError(f"Unexpected prefix event: {event}")
            queue_input(remote, record, index + 1)
            if rolling_captures or index == frame:
                capture = remote.packet(f"qXportLockstepCheckpointCapture:{index:x}")
                if not capture.startswith(f"OK:{index:x}:"):
                    raise RuntimeError(f"Checkpoint capture failed: {capture}")
                captures += 1
        first = next_event(remote)
        if first["kind"] != "VBLANK" or first["frame"] != frame + 1:
            raise RuntimeError(f"Unexpected first continuation: {first}")
        first_digest = memory_digest(remote)
        expect(remote, f"qXportLockstepCheckpointRestore:{frame:x}", f"OK:{frame:x}")
        queue_input(remote, record, frame + 1)
        transition = next_event(remote)
        if (transition["kind"] != "MODE_TRANSITION" or transition["frame"] != frame + 1 or
                transition["event_sequence"] != 0):
            raise RuntimeError(f"Unexpected checkpoint transition: {transition}")
        second = next_event(remote)
        if second["kind"] != "VBLANK" or second["frame"] != frame + 1:
            raise RuntimeError(f"Unexpected restored continuation: {second}")
        second_digest = memory_digest(remote)
        return {
            "schema": 1, "status": "matched" if (first_digest == second_digest and
                first["controller_state"] == second["controller_state"]) else "diverged",
            "record_id": record_id,
            "runtime_identity": runtime,
            "checkpoint_frame": frame, "replayed_frame": frame + 1,
            "captures": captures,
            "digest_scope": "Complete DRAM and scratchpad without exclusions",
            "captured_bytes": int(capture.rsplit(":", 1)[1], 16),
            "first_digest": first_digest, "restored_digest": second_digest,
            "first_input": first["controller_state"],
            "restored_input": second["controller_state"],
        }
    finally:
        if remote is not None:
            remote.close()
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        for mapping in mappings:
            mapping.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--emulator", type=Path, required=True)
    parser.add_argument("--frame", type=int, default=1)
    parser.add_argument("--record", default="warmup")
    parser.add_argument("--rolling-captures", action="store_true")
    parser.add_argument("--receipt", type=Path, required=True)
    args = parser.parse_args()
    result = probe(args.project.resolve(), args.emulator.resolve(), args.frame,
                   rolling_captures=args.rolling_captures, record_name=args.record)
    args.receipt.parent.mkdir(parents=True, exist_ok=True)
    args.receipt.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    return 0 if result["status"] == "matched" else 1


if __name__ == "__main__":
    raise SystemExit(main())

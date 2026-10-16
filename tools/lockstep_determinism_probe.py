"""Compare complete machine checkpoints under speed changes and extra GDB stops"""

import argparse
import hashlib
import json
import mmap
import os
import struct
import time
import uuid
from pathlib import Path

from duckstation_runtime import lockstep_timing_arguments
from duckstation_runtime import (data_directory, environment, executable, launch_process,
                                 stop_owned_endpoint, validate_settings)
from gdb_remote import Remote, decode_registers
from lockstep_contract import mapping_name
from lockstep_duck_checkpoint_probe import expect, queue_input
from lockstep_record import load_record
from lockstep_record_cli import wait_port
from xport_project import load_project


def replay(root, config, record, output, frames, fast, extra_stops):
    role = config["lockstep"]["runtime"]["duckstation_role"]
    port = int(config["duckstation"]["reserved_gdb_ports"][role])
    host = config["duckstation"].get("host", "127.0.0.1")
    directory = validate_settings(data_directory(role=role), port)
    stop_owned_endpoint(directory, port)
    run_id = uuid.uuid4().hex
    names = [mapping_name(run_id, name) for name in ("dram", "scratchpad", "deferred")]
    sizes = [0x200000, 0x400, (10 + 3 + 32 * 3 + 511 * 3 + 7 + 4096 * 5 + 2) * 4]
    mappings = [mmap.mmap(-1, size, tagname=name) for name, size in zip(names, sizes)]
    mappings.append(mmap.mmap(-1, 0x100000, tagname=f"Local\\xport-lockstep-{run_id}-vram"))
    mappings[2][:8] = struct.pack("<II", 0x44504C58, 12)
    checkpoint = output / "machine.raw"
    env = environment(directory)
    env["XPORT_LOCKSTEP_CHECKPOINT_FILE"] = str(checkpoint)
    env["XPORT_GDB_RAM_BACKEND"] = "rsp"
    command = [str(executable()), *lockstep_timing_arguments(executable()), "-batch", "-fastboot", "-statefile",
               record["initial_state"]["path"], record["disc"][0]["path"]]
    process = None
    remote = None
    states = []
    stops = 0
    try:
        process = launch_process(command, directory, env, False)
        wait_port(host, port, process)
        remote = Remote(host, port)
        if not remote.packet("?").startswith(("S", "T")):
            raise RuntimeError("Initial save did not load paused")
        pcs = ":".join(f"{int(pc, 0):x}" for pc in config["duckstation"]["wait_points"])
        expect(remote, f"qXportWaitPoints:{pcs}", "OK:wait")
        if config["duckstation"].get("sio_delay"):
            delay = ":".join(f"{int(pc, 0):x}" for pc in config['duckstation']['sio_delay'])
            expect(remote, f"qXportSioDelay:{delay}", "OK:sio")
        expect(remote, f"qXportLockstepAttach:{run_id}:{':'.join(names)}", f"OK:{run_id}")
        record_id = record["record_id"]
        expect(remote, f"qXportLockstepReplay:{record_id}", f"OK:{record_id}")
        expect(remote, f"qXportLockstepSeed:{record_id}", f"OK:{record_id}")
        expect(remote, f"qXportLockstepMode:PREFIX_GUARD:{frames + 1:x}",
               f"OK:PREFIX_GUARD:{frames + 1:x}")
        expect(remote, "qXportLockstepStart", "OK:start")
        expect(remote, f"qXportFastForward:{int(fast)}", "OK:presentation")
        for frame in range(frames):
            queue_input(remote, record, frame)
            breakpoint = None
            if extra_stops:
                pc = int(decode_registers(remote.packet("g"))["pc"], 0)
                breakpoint = f"{pc:x},4"
                expect(remote, f"Z0,{breakpoint}", "OK")
            while True:
                stop = remote.packet("c", timeout=60)
                if not stop.startswith(("S", "T")):
                    raise RuntimeError(f"Unexpected replay stop: {stop}")
                event_reply = remote.packet("qXportLockstepNextEvent")
                if breakpoint:
                    expect(remote, f"z0,{breakpoint}", "OK")
                    breakpoint = None
                if event_reply == "NONE":
                    stops += 1
                    time.sleep(0.05)
                    continue
                if not event_reply.startswith("EVENT:"):
                    raise RuntimeError(event_reply)
                event = json.loads(bytes.fromhex(event_reply[6:]))
                if event.get("kind") != "VBLANK" or event.get("frame") != frame:
                    raise RuntimeError(f"Unexpected event: {event}")
                break
            capture = remote.packet(f"qXportLockstepCheckpointCapture:{frame:x}")
            if not capture.startswith(f"OK:{frame:x}:"):
                raise RuntimeError(capture)
            size = int(capture.rsplit(":", 1)[1], 16)
            expect(remote, f"qXportLockstepCheckpointExport:{frame:x}", f"OK:{frame:x}:{size:x}")
            data = checkpoint.read_bytes()
            if len(data) != size:
                raise RuntimeError("Incomplete machine checkpoint")
            states.append({"event": event, "machine": hashlib.sha256(data).hexdigest()})
        return {"fast": fast, "extra_stops": extra_stops, "stops": stops, "states": states}
    finally:
        if remote:
            remote.close()
        if process:
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=10)
        for mapping in mappings:
            mapping.close()
        checkpoint.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--record", required=True)
    parser.add_argument("--frames", type=int, required=True)
    args = parser.parse_args()
    root, config = load_project(args.project)
    os.environ["XPORT_PROJECT"] = str(root)
    record = load_record(root / "status/lockstep-records" / args.record / "record.bin")
    if not 0 < args.frames <= len(record["frames"]):
        raise ValueError("Frame count is outside the record")
    output = root / "status/diagnostics/cooperative-determinism"
    output.mkdir(parents=True, exist_ok=True)
    trials = [replay(root, config, record, output, args.frames, fast, stops)
              for fast, stops in ((False, False), (True, False), (True, True))]
    baseline = trials[0]["states"]
    matched = all(trial["states"] == baseline for trial in trials[1:]) and trials[2]["stops"] > 0
    result = {"status": "matched" if matched else "diverged", "frames": args.frames, "trials": trials}
    (output / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({"status": result["status"], "frames": args.frames, "output": str(output)}))
    raise SystemExit(0 if matched else 1)


if __name__ == "__main__":
    main()

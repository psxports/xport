#!/usr/bin/env python3
"""Create and verify deterministic FrameDone lockstep records"""

from __future__ import annotations

import hashlib
import json
import struct
from pathlib import Path

from lockstep_contract import ContractError, digest, exact_keys, identity, integer, require_object


RECORD_CONTRACT = "xport-lockstep-record-v1"
PHASE = "after_frame_done_before_next_frame"
CONTROLLER_TYPES = {"disconnected": 0, "digital": 1, "analog": 2}
INPUT_MAGIC = b"XPLSREC1"
INPUT_HEADER_SIZE = 64
INPUT_ENTRY_SIZE = 28


def file_reference(path: Path) -> dict:
    path = path.resolve()
    if not path.is_file():
        raise FileNotFoundError(path)
    content = path.read_bytes()
    return {
        "path": str(path), "size": len(content),
        "sha256": hashlib.sha256(content).hexdigest(),
    }


def validate_file_reference(value, name):
    value = require_object(value, name)
    exact_keys(value, ("path", "size", "sha256"))
    if not isinstance(value["path"], str) or not value["path"]:
        raise ContractError(f"{name}.path must be nonempty")
    integer(value["size"], f"{name}.size")
    digest(value["sha256"], f"{name}.sha256")
    return value


def validate_controller(value):
    value = require_object(value, "controller")
    exact_keys(value, ("type", "buttons", "analog"))
    if value["type"] not in CONTROLLER_TYPES:
        raise ContractError("Unsupported controller type")
    buttons = integer(value["buttons"], "buttons")
    analog = integer(value["analog"], "analog")
    if buttons > 0xFFFF or analog > 0xFFFFFFFF:
        raise ContractError("Controller state is out of range")
    if value["type"] == "disconnected" and buttons:
        raise ContractError("Disconnected controller has pressed buttons")
    return value


def _record_payload(value):
    return {key: item for key, item in value.items() if key != "record_id"}


def validate_record(value):
    value = require_object(value, "record")
    timing = require_object(value["timing"], "timing")
    exact_keys(timing, ("video_standard", "field_rate", "phase"))
    if ((timing["video_standard"], timing["field_rate"]) not in (("PAL", 50), ("NTSC", 60)) or
            timing["phase"] != PHASE):
        raise ContractError("Invalid FrameDone timing identity")
    frames = value["frames"]
    if not isinstance(frames, list) or not frames:
        raise ContractError("Record has no FrameDone input")
    prior = None
    for row in frames:
        row = require_object(row, "frame")
        exact_keys(row, ("frame", "ports"))
        frame = integer(row["frame"], "frame")
        if prior is None and frame != 0:
            raise ContractError("FrameDone input must start at lockstep frame zero")
        if prior is not None and frame != prior + 1:
            raise ContractError("FrameDone input is not contiguous")
        prior = frame
        if not isinstance(row["ports"], list) or len(row["ports"]) != 2:
            raise ContractError("FrameDone input requires two ports")
        for controller in row["ports"]:
            validate_controller(controller)
    terminal = require_object(value["terminal"], "terminal")
    exact_keys(terminal, ("frame", "event"))
    if integer(terminal["frame"], "terminal.frame") != frames[-1]["frame"]:
        raise ContractError("Terminal frame does not match input schedule")
    if terminal["event"] not in {"VBLANK", "PROCESS_EXIT"}:
        raise ContractError("Invalid terminal event")
    return value


def create_record(*, initial_state: Path, disc: list[Path], executable: Path, image: str,
                  duckstation_commit: str, patch_sha256: str, settings: Path,
                  video_standard: str, field_rate: int, frames: list[dict], terminal_event="VBLANK") -> dict:
    value = {
        "schema": 1,
        "contract": RECORD_CONTRACT,
        "initial_state": file_reference(initial_state),
        "disc": [file_reference(path) for path in disc],
        "original": {"image": image, "executable": file_reference(executable)},
        "duckstation": {
            "commit": duckstation_commit, "patch_sha256": patch_sha256,
            "settings": file_reference(settings),
        },
        "timing": {"video_standard": video_standard, "field_rate": field_rate, "phase": PHASE},
        "frames": frames,
        "terminal": {"frame": frames[-1]["frame"], "event": terminal_event},
    }
    value["record_id"] = initial_state.parent.name.encode("utf-8")[:32].ljust(32, b"\0").hex()
    return validate_record(value)


def save_record(path: Path, value: dict) -> None:
    validate_record(value)
    state = Path(value["initial_state"]["path"]).read_bytes()
    events = value.get("events", [])
    value["record_id"] = path.parent.name.encode("utf-8")[:32].ljust(32, b"\0").hex()
    with path.open("xb") as stream:
        stream.write(struct.pack("<4I", len(state), len(value["frames"]), len(events),
                                 value["timing"]["field_rate"]))
        stream.write(state)
        for row in value["frames"]:
            ports = row["ports"]
            stream.write(struct.pack("<6I", CONTROLLER_TYPES[ports[0]["type"]],
                                     CONTROLLER_TYPES[ports[1]["type"]],
                                     ports[0]["buttons"], ports[1]["buttons"],
                                     ports[0]["analog"], ports[1]["analog"]))
        for event in events:
            stream.write(struct.pack("<4I", event["frame"], event["pc"],
                                     event["kind"], event["value"]))


def load_record(path: Path) -> dict:
    path = Path(path).resolve()
    with path.open("rb") as stream:
        state_size, frame_count, event_count, field_rate = struct.unpack("<4I", stream.read(16))
        state = stream.read(state_size)
        if len(state) != state_size:
            raise EOFError("Incomplete initial save state")
        frames = []
        names = {code: name for name, code in CONTROLLER_TYPES.items()}
        for frame in range(frame_count):
            first, second, buttons0, buttons1, analog0, analog1 = struct.unpack("<6I", stream.read(24))
            frames.append({"frame": frame, "ports": [
                {"type": names[first], "buttons": buttons0, "analog": analog0},
                {"type": names[second], "buttons": buttons1, "analog": analog1}]})
        events = [dict(zip(("frame", "pc", "kind", "value"),
                           struct.unpack("<4I", stream.read(16)))) for _ in range(event_count)]
    root = next(parent for parent in path.parents if (parent / "xport-project.json").is_file())
    config = json.loads((root / "xport-project.json").read_text(encoding="utf-8"))
    initial_state = path.parent / "initial.sav"
    initial_state.write_bytes(state)
    from lockstep_record_cli import disc_inventory
    manifest = json.loads((Path(__file__).parent / "duckstation/duckstation-manifest.json").read_text())
    settings = path.parent / "settings.ini"
    if not settings.is_file():
        role = config["lockstep"]["runtime"].get("record_role", "user")
        settings = root / config["duckstation"]["data_directories"][role] / "settings.ini"
    original = root / config["paths"]["original_executable"]
    value = create_record(initial_state=initial_state,
                          disc=disc_inventory(root / config["paths"]["disc_image"]),
                          executable=original, image=config["lockstep"]["original_image"],
                          duckstation_commit=manifest["upstream"]["commit"],
                          patch_sha256=manifest["patch"]["sha256"], settings=settings,
                          video_standard="NTSC" if field_rate == 60 else "PAL",
                          field_rate=field_rate, frames=frames)
    value["record_id"] = path.parent.name.encode("utf-8")[:32].ljust(32, b"\0").hex()
    value["events"] = events
    return value


def verify_record_inputs(value: dict) -> dict:
    validate_record(value)
    references = [value["initial_state"], *value["disc"], value["original"]["executable"],
                  value["duckstation"]["settings"]]
    changed = []
    for reference in references:
        path = Path(reference["path"])
        if not path.is_file():
            changed.append(reference["path"])
    return {"status": "match" if not changed else "identity_mismatch", "changed": changed}


def write_native_input(path: Path, value: dict) -> str:
    validate_record(value)
    identity_bytes = bytes.fromhex(value["record_id"])
    header = bytearray(INPUT_HEADER_SIZE)
    header[:8] = INPUT_MAGIC
    struct.pack_into("<5I", header, 8, 1, INPUT_HEADER_SIZE, INPUT_ENTRY_SIZE,
                     value["timing"]["field_rate"], len(value["frames"]))
    header[32:64] = identity_bytes
    with path.open("xb") as stream:
        stream.write(header)
        for row in value["frames"]:
            ports = row["ports"]
            stream.write(struct.pack(
                "<7I", row["frame"], CONTROLLER_TYPES[ports[0]["type"]],
                CONTROLLER_TYPES[ports[1]["type"]], ports[0]["buttons"], ports[1]["buttons"],
                ports[0]["analog"], ports[1]["analog"],
            ))
    return hashlib.sha256(path.read_bytes()).hexdigest()


class FrameDoneRecorder:
    def __init__(self):
        self.frames = []
        self.source_first_frame = None

    def accept(self, event: dict) -> None:
        if event.get("side") != "original" or event.get("kind") != "VBLANK":
            raise ContractError("Recorder accepts only original VBLANK events")
        controllers = event.get("controller_state")
        if not isinstance(controllers, list) or len(controllers) != 2:
            raise ContractError("VBLANK lacks controller state")
        source_frame = integer(event.get("frame"), "frame")
        if self.source_first_frame is None:
            self.source_first_frame = source_frame
        if source_frame != self.source_first_frame + len(self.frames):
            raise ContractError("Recorded FrameDone sequence is not contiguous")
        self.frames.append({
            "frame": len(self.frames),
            "ports": [dict(validate_controller(controller)) for controller in controllers],
        })

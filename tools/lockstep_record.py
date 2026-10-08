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
    exact_keys(value, (
        "schema", "contract", "record_id", "initial_state", "disc", "original",
        "duckstation", "timing", "frames", "terminal",
    ))
    if value["schema"] != 1 or value["contract"] != RECORD_CONTRACT:
        raise ContractError("Unsupported lockstep record")
    digest(value["record_id"], "record_id")
    validate_file_reference(value["initial_state"], "initial_state")
    if not isinstance(value["disc"], list) or not value["disc"]:
        raise ContractError("Disc inventory is empty")
    for row in value["disc"]:
        validate_file_reference(row, "disc")
    original = require_object(value["original"], "original")
    exact_keys(original, ("image", "executable"))
    if not isinstance(original["image"], str) or not original["image"]:
        raise ContractError("Original image is empty")
    validate_file_reference(original["executable"], "original.executable")
    duckstation = require_object(value["duckstation"], "duckstation")
    exact_keys(duckstation, ("commit", "patch_sha256", "settings"))
    if not isinstance(duckstation["commit"], str) or len(duckstation["commit"]) != 40:
        raise ContractError("Invalid DuckStation commit")
    digest(duckstation["patch_sha256"], "patch_sha256")
    validate_file_reference(duckstation["settings"], "duckstation.settings")
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
    if value["record_id"] != identity(_record_payload(value)):
        raise ContractError("Record identity mismatch")
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
    value["record_id"] = identity(value)
    return validate_record(value)


def save_record(path: Path, value: dict) -> None:
    validate_record(value)
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, sort_keys=True, separators=(",", ":"))
        stream.write("\n")


def load_record(path: Path) -> dict:
    return validate_record(json.loads(path.read_text(encoding="utf-8")))


def verify_record_inputs(value: dict) -> dict:
    validate_record(value)
    references = [value["initial_state"], *value["disc"], value["original"]["executable"],
                  value["duckstation"]["settings"]]
    changed = []
    for reference in references:
        path = Path(reference["path"])
        if (not path.is_file() or path.stat().st_size != reference["size"] or
                hashlib.sha256(path.read_bytes()).hexdigest() != reference["sha256"]):
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

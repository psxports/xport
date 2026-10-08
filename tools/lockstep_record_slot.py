"""Resolve an isolated DuckStation game save slot to an exact state file"""

from __future__ import annotations

import re
import struct
from pathlib import Path


SAVE_MAGIC = 0x43435544
SAVE_VERSION = 85
HEADER_SIZE = 216
SERIAL_OFFSET = 136
SERIAL_SIZE = 32


def resolve_slot(root: Path, config: dict, slot: int) -> Path:
    if type(slot) is not int or not 1 <= slot <= 10:
        raise ValueError("Save slot must be 1..10")
    serial = config["duckstation"].get("save_serial")
    if not isinstance(serial, str) or not re.fullmatch(r"[A-Z]{4}-[0-9]{5}", serial):
        raise ValueError("duckstation.save_serial is required for slot recording")
    role = config["lockstep"]["runtime"].get("record_role", "user")
    relative = config["duckstation"]["data_directories"][role]
    data_directory = (root / relative).resolve()
    if not data_directory.is_relative_to((root / "tools").resolve()):
        raise ValueError("Save slot directory is outside project tools")
    path = data_directory / "savestates" / f"{serial}_{slot}.sav"
    if not path.is_file():
        raise FileNotFoundError(f"Game save slot {slot} is missing: {path}")
    with path.open("rb") as stream:
        header = stream.read(HEADER_SIZE)
    if len(header) != HEADER_SIZE or struct.unpack_from("<2I", header) != (SAVE_MAGIC, SAVE_VERSION):
        raise ValueError("Save slot has an unsupported DuckStation header")
    saved_serial = header[SERIAL_OFFSET:SERIAL_OFFSET + SERIAL_SIZE].split(b"\0", 1)[0]
    if saved_serial != serial.encode("ascii"):
        raise ValueError("Save slot belongs to another game")
    compression, compressed, uncompressed, offset = struct.unpack_from("<4I", header, 200)
    if (compression not in {0, 2} or uncompressed > 32 * 1024 * 1024 or
            offset < HEADER_SIZE or offset + compressed != path.stat().st_size):
        raise ValueError("Save slot payload bounds are invalid")
    return path

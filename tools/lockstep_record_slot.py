"""Resolve an isolated DuckStation game save slot to an exact state file"""

from __future__ import annotations

import re
from pathlib import Path


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
    # Let DuckStation load and interpret its own save state
    return path

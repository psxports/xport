"""Resolve current C function spans for lockstep MIPS audit receipts"""

from __future__ import annotations

import re
from pathlib import Path

from source_index import functions, mask_c


CALL = re.compile(r"\b([A-Za-z_]\w*)\s*\(")


def index_functions(root: Path, source_files: dict[str, str]) -> dict[tuple[str, str], dict]:
    found = {}
    for name in sorted(source_files):
        if Path(name).suffix.lower() not in {".c", ".cpp"}:
            continue
        text = (root / name).read_text(encoding="utf-8-sig")
        for row in functions(text, name):
            key = (name, row["name"])
            if key in found:
                raise ValueError("Duplicate C function in one translation unit: " + row["name"])
            body = mask_c(text[row["start"]:row["end"]])
            signature = body[:body.find("{")]
            found[key] = {
                "path": name, "symbol": row["name"],
                "start_line": row["start_line"], "end_line": row["end_line"],
                "calls": set(CALL.findall(body[body.find("{") + 1:])),
                "static": bool(re.search(r"\bstatic\b", signature)),
            }
    return found


def reachable_functions(index: dict[tuple[str, str], dict],
                        start_path: str, start_symbol: str) -> set[tuple[str, str]]:
    start = (start_path, start_symbol)
    if start not in index:
        raise ValueError("Original C implementation is missing from current source")
    by_symbol = {}
    for key, row in index.items():
        by_symbol.setdefault(row["symbol"], []).append(key)
    seen = set()
    queue = [start]
    while queue:
        key = queue.pop()
        if key in seen:
            continue
        seen.add(key)
        caller = index[key]
        for symbol in caller["calls"]:
            candidates = by_symbol.get(symbol, [])
            local = [item for item in candidates if item[0] == key[0]]
            global_candidates = [item for item in candidates if not index[item]["static"]]
            selected = local if local else global_candidates
            if len(selected) == 1:
                queue.extend(selected)
    return seen


def validate_location(index: dict[tuple[str, str], dict],
                      reachable: set[tuple[str, str]] | None,
                      source_file: str, source_symbol: str,
                      source_line: int) -> bool:
    key = (source_file, source_symbol)
    row = index.get(key)
    return bool(row and row["start_line"] <= source_line <= row["end_line"] and
                (reachable is None or key in reachable))

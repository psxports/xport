"""Prove accessor coverage for translated guest-memory writers"""

import argparse
import hashlib
import json
import re
from pathlib import Path

from xport_project import atomic_json
from xport_project import artifact_path, load_project


SCHEMA = 1
ROOT_CALL = r"(?:rrj_at|psx_addr|xport_guest_ptr)\s*\("
ASSIGNMENT = r"(?:\+=|-=|\*=|/=|%=|&=|\|=|\^=|\+\+|--|(?<![=!<>])=(?!=))"
DIRECT_STORE = re.compile(r"(?:\*\s*\([^;\n]*" + ROOT_CALL + r"[^;\n]*\)|" + ROOT_CALL + r"[^()\n;]*\)\s*\[[^\]]+\])\s*" + ASSIGNMENT)
ALIAS = re.compile(r"(?P<const>\bconst\b[^;\n]*)?\b(?:uint8|uint16|uint32|sint8|sint16|sint32|uint8_t|uint16_t|uint32_t|int8_t|int16_t|int32_t|void)\s*\*\s*(?P<name>[A-Za-z_]\w*)\s*=\s*(?:\([^;\n]+\)\s*)?(?P<root>rrj_at|psx_addr|xport_guest_ptr)\s*\(")
DIRECT_REGION_STORE = re.compile(r"\b(?P<region>DRAM|SCRATCHPAD)\s*\[[^\]]+\]\s*" + ASSIGNMENT)
KNOWN_OBSERVED_CALLS = {
    "rrj_put16", "rrj_put32", "xport_store_u8", "xport_store_le16", "xport_store_le32",
    "ClearOTag", "ClearOTagR", "SetDrawMode", "SetDrawMode1", "SetPolyF3", "SetPolyF4",
    "SetPolyFT3", "SetPolyFT4", "SetPolyG3", "SetPolyG4", "SetPolyGT3", "SetPolyGT4",
    "SetSemiTrans", "SetShadeTex", "SetSprt", "SetSprt8", "SetSprt16", "SetTile",
    "SetTile1", "SetTile8", "SetTile16", "setRGB0", "setRGB1", "setRGB2", "setRGB3",
    "setXY0", "setXY2", "setXY3", "setXY4", "setUV0", "setUV3", "setUV4", "setWH",
    "setTPage", "setClut", "setRECT", "setVector",
}
READ_ONLY_CALLS = {
    "fwrite", "memcmp", "DrawOTag", "DrawOTagEnv", "LoadImagePSX", "gpu_import_raster",
    "write_file", "full_write", "read_checkpoint_gte",
}
DIAGNOSTIC_PATHS = {"src/race_trace_runtime.c", "src/rr_trace_runtime.c", "src/wip.c"}


class CoverageError(ValueError):
    """Raised when writer coverage evidence is invalid"""


def _sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def _line(text, offset):
    return text.count("\n", 0, offset) + 1


def _function_span(text, offset):
    opening = text.rfind("{", 0, offset)
    while opening >= 0:
        depth = 0
        for index in range(opening, len(text)):
            if text[index] == "{":
                depth += 1
            elif text[index] == "}":
                depth -= 1
                if depth == 0:
                    if index >= offset:
                        return opening, index + 1
                    break
        opening = text.rfind("{", 0, opening)
    return 0, len(text)


def _alias_uses(body, name):
    escaped = []
    direct = re.compile(r"(?:\*\s*" + re.escape(name) + r"\b|\b" + re.escape(name) + r"\s*\[[^\]]+\])\s*" + ASSIGNMENT)
    for match in direct.finditer(body):
        if re.search(ROOT_CALL, body[match.start():body.find(";", match.start()) + 1]):
            continue
        escaped.append((match.start(), "alias_scalar_store", None))
    for call in re.finditer(r"\b([A-Za-z_]\w*)\s*\(([^;{}]*)\)", body):
        function, arguments = call.group(1), call.group(2)
        if not re.search(r"\b" + re.escape(name) + r"\b", arguments):
            continue
        first = arguments.split(",", 1)[0]
        if function in {"memcpy", "memmove", "memset", "fread"} and re.search(r"\b" + re.escape(name) + r"\b", first):
            escaped.append((call.start(), "alias_bulk_store", function))
        elif function in KNOWN_OBSERVED_CALLS:
            continue
        elif function in READ_ONLY_CALLS:
            continue
        elif function not in {"sizeof", "if", "for", "while", "switch"}:
            escaped.append((call.start(), "unknown_external_alias", function))
    return escaped


def audit(root, family):
    root = Path(root).resolve()
    entries = []
    modules = []
    for path in sorted((*root.joinpath("src").rglob("*.c"), *root.joinpath("src").rglob("*.h"))):
        relative = path.relative_to(root).as_posix()
        text = path.read_text(encoding="utf-8-sig", errors="strict")
        digest = _sha256(path)
        module_entries = []
        for match in DIRECT_STORE.finditer(text):
            module_entries.append({"kind": "direct_root_store", "line": _line(text, match.start()), "symbol": None, "backend": None})
        for match in DIRECT_REGION_STORE.finditer(text):
            module_entries.append({"kind": "direct_region_store", "line": _line(text, match.start()), "symbol": match.group("region"), "backend": None})
        for match in ALIAS.finditer(text):
            if match.group("const"):
                continue
            start, end = _function_span(text, match.start())
            body = text[start:end]
            for use, kind, symbol in _alias_uses(body, match.group("name")):
                absolute = start + use
                if absolute == match.start():
                    continue
                module_entries.append({"kind": kind, "line": _line(text, absolute), "symbol": symbol or match.group("name"), "backend": None})
        for call in re.finditer(r"\b(memcpy|memmove|memset|fread)\s*\(\s*(" + ROOT_CALL + r")", text):
            module_entries.append({"kind": "direct_bulk_store", "line": _line(text, call.start()), "symbol": call.group(1), "backend": None})
        diagnostic = relative in DIAGNOSTIC_PATHS or relative == "src/game.c"
        for entry in module_entries:
            entry["path"] = relative
            entry["source_sha256"] = digest
            if diagnostic:
                entry["backend"] = "diagnostic-import-excluded"
            entries.append(entry)
        modules.append({
            "path": relative,
            "source_sha256": digest,
            "backend": "diagnostic-excluded" if diagnostic else "accessor-observer",
            "candidate_gaps": len(module_entries),
            "uncovered": sum(1 for item in module_entries if item["backend"] is None),
        })
    uncovered = [entry for entry in entries if entry["backend"] is None]
    return {
        "schema": SCHEMA,
        "status": "complete" if not uncovered else "incomplete",
        "family": family,
        "scope": "translated DRAM and scratchpad writers in production project translation units",
        "excluded_scopes": [
            "diagnostic checkpoint/import code",
            "VRAM and SPU RAM device writers handled by device provenance",
            "MMIO side effects handled by bound callbacks",
            "host-only locals and buffers",
        ],
        "implementation_sha256": _sha256(__file__),
        "project_root": str(root),
        "modules": modules,
        "entries": entries,
        "summary": {
            "modules": len(modules),
            "candidate_gaps": len(entries),
            "excluded": sum(1 for item in entries if item["backend"] is not None),
            "uncovered": len(uncovered),
        },
        "toolchain": {
            "primary_backend": "accessor-observer",
            "llvm_newpm": "unavailable-no-pinned-clang-cl-sdk",
            "double_logging_policy": "one declared backend per module",
            "threads": "single-threaded game runtime",
            "unwind_longjmp": "unsupported-in-declared-scope",
            "inline_asm_unknown_external": "uncovered-unless-explicitly_classified",
        },
    }


def validate(report):
    if report.get("schema") != SCHEMA or report.get("status") not in {"complete", "incomplete"}:
        raise CoverageError("Invalid writer coverage report")
    if report["summary"]["uncovered"] != sum(1 for item in report["entries"] if item["backend"] is None):
        raise CoverageError("Writer coverage count mismatch")
    if report["status"] == "complete" and report["summary"]["uncovered"]:
        raise CoverageError("Complete writer coverage contains gaps")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--family", choices=("rr", "rrj"), required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--allow-incomplete", action="store_true")
    args = parser.parse_args()
    root, _ = load_project()
    report = validate(audit(root, args.family))
    output = artifact_path(args.output)
    if output.exists():
        if json.loads(output.read_text(encoding="utf-8-sig")) != report:
            raise CoverageError("Writer coverage output already contains different immutable evidence")
    else:
        atomic_json(output, report)
    print(json.dumps({"status": report["status"], "summary": report["summary"], "output": str(output)}, separators=(",", ":")))
    if report["status"] != "complete" and not args.allow_incomplete:
        raise SystemExit(2)


if __name__ == "__main__":
    main()

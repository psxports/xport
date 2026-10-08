#!/usr/bin/env python3
"""Build and verify paired native memory-observer fixtures."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
from pathlib import Path
from typing import Any


def parse_fixture_output(output: str) -> dict[str, Any]:
    lines = [line.strip() for line in output.splitlines() if line.strip().startswith("{")]
    if not lines:
        raise ValueError("fixture output has no JSON record")
    record = json.loads(lines[-1])
    required = {"state_hash", "events", "sourced", "same_value", "copies", "fills", "filtered"}
    missing = sorted(required - record.keys())
    if missing:
        raise ValueError(f"fixture record is missing: {', '.join(missing)}")
    return record


def compare_records(plain: dict[str, Any], instrumented: dict[str, Any]) -> dict[str, Any]:
    semantic_fields = ("state_hash", "events", "same_value", "copies", "fills", "filtered")
    mismatches = [name for name in semantic_fields if plain[name] != instrumented[name]]
    status = "passed" if not mismatches and plain["sourced"] == 0 and instrumented["sourced"] > 0 else "failed"
    return {
        "schema": 1,
        "status": status,
        "semantic_mismatches": mismatches,
        "plain": plain,
        "instrumented": instrumented,
    }


def coverage_from_audit(document: dict[str, Any]) -> dict[str, Any]:
    summary = document.get("summary", {})
    categories = summary.get("categories", summary.get("by_category", summary.get("by_kind", {})))
    if not isinstance(categories, dict):
        categories = {}
    pointer_escapes = sum(
        int(value) for key, value in categories.items()
        if "pointer_escape" in str(key)
    )
    legacy = sum(
        int(value) for key, value in categories.items()
        if str(key).startswith("legacy_") or str(key).startswith("legacy:")
    )
    return {
        "schema": 1,
        "status": "complete" if pointer_escapes == 0 and legacy == 0 else "gaps",
        "pointer_escape_gaps": pointer_escapes,
        "legacy_gaps": legacy,
        "claim": "accessor-observer coverage only" if pointer_escapes else "accessor-observer coverage",
    }


def file_identity(path: Path) -> dict[str, Any]:
    data = path.read_bytes()
    return {"path": str(path), "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def run_fixture(path: Path) -> tuple[dict[str, Any], str]:
    completed = subprocess.run([str(path)], check=True, capture_output=True, text=True)
    return parse_fixture_output(completed.stdout), completed.stdout


def build_pair(args: argparse.Namespace) -> dict[str, Any]:
    source = args.source.resolve()
    xport_src = args.xport_src.resolve()
    output = args.output_dir.resolve()
    plain_objects = output / "plain"
    instrumented_objects = output / "instrumented"
    plain_objects.mkdir(parents=True, exist_ok=True)
    instrumented_objects.mkdir(parents=True, exist_ok=True)
    common = (
        'cl /nologo /W4 /WX /O2 /std:c17 /DWND_TITLE=L"fixture" '
        f'/DWND_WIDTH=1 /DWND_HEIGHT=1 /DFIELD_RATE=60 /I"{xport_src}" '
        f'"{source}" "{xport_src / "xport_trace.c"}"'
    )
    plain_exe = output / "plain.exe"
    instrumented_exe = output / "instrumented.exe"
    build_script = output / "build_pair.cmd"
    build_script.write_text(
        "@echo off\n"
        f'call "{args.vcvars.resolve()}" >nul\n'
        "if errorlevel 1 exit /b %errorlevel%\n"
        f'{common} /Fe:"{plain_exe}" /Fo{plain_objects}\\\n'
        "if errorlevel 1 exit /b %errorlevel%\n"
        f'{common} /DOBSERVER_INSTRUMENTED /Fe:"{instrumented_exe}" /Fo{instrumented_objects}\\\n'
        "if errorlevel 1 exit /b %errorlevel%\n",
        encoding="utf-8",
    )
    subprocess.run(["cmd.exe", "/d", "/c", str(build_script)], check=True)
    plain, _ = run_fixture(plain_exe)
    instrumented, _ = run_fixture(instrumented_exe)
    result = compare_records(plain, instrumented)
    result["artifacts"] = [file_identity(plain_exe), file_identity(instrumented_exe)]
    if args.audit:
        result["coverage"] = coverage_from_audit(json.loads(args.audit.read_text(encoding="utf-8")))
    args.receipt.parent.mkdir(parents=True, exist_ok=True)
    args.receipt.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if result["status"] != "passed":
        raise SystemExit(1)
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    compare = subparsers.add_parser("compare")
    compare.add_argument("plain")
    compare.add_argument("instrumented")
    coverage = subparsers.add_parser("coverage")
    coverage.add_argument("audit", type=Path)
    build = subparsers.add_parser("build")
    build.add_argument("--vcvars", type=Path, required=True)
    build.add_argument("--source", type=Path, required=True)
    build.add_argument("--xport-src", type=Path, required=True)
    build.add_argument("--output-dir", type=Path, required=True)
    build.add_argument("--receipt", type=Path, required=True)
    build.add_argument("--audit", type=Path)
    args = parser.parse_args()
    if args.command == "compare":
        result = compare_records(parse_fixture_output(args.plain), parse_fixture_output(args.instrumented))
    elif args.command == "coverage":
        result = coverage_from_audit(json.loads(args.audit.read_text(encoding="utf-8")))
    else:
        result = build_pair(args)
    print(json.dumps(result, indent=2))
    return 0 if result.get("status") in {"passed", "complete", "gaps"} else 1


if __name__ == "__main__":
    raise SystemExit(main())

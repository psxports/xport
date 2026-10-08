#!/usr/bin/env python3
"""Collect semantic original-function calls during one interactive run"""

from __future__ import annotations

import argparse
from contextlib import closing
import configparser
import json
from datetime import datetime, timezone
import os
from pathlib import Path
import re
import struct
import sqlite3
import hashlib
import time

from duckstation_runtime import (data_directory, environment, executable, launch_process,
                                 record_owner, stop_owned_endpoint, validate_settings)
from gdb_endpoint import listener_process
from gdb_remote import Remote
from trace_worker import alive, write_receipt
from xport_project import load_project, project_path


FEATURES = (
    "qXportFunctionCoverageStart+",
    "qXportFunctionCoverageTarget+",
    "qXportFunctionCoverageStop+",
    "qXportFunctionCoverageRead+",
)
RUN_NAME = re.compile(r"[A-Za-z0-9._-]+")


def validate_run_name(name: str) -> str:
    if not 1 <= len(name) <= 64 or name in {".", ".."} or not RUN_NAME.fullmatch(name):
        raise ValueError("Function coverage NAME must be one 1..64 character ASCII path component")
    return name


def load_functions(path: Path, *, status: str | None = None,
                   database: Path | None = None, image: str | None = None) -> list[dict]:
    document = json.loads(path.read_text(encoding="utf-8-sig"))
    if not isinstance(document, list):
        raise ValueError("Function export must be a list")
    selected = None
    if status is not None:
        if database is None or image is None:
            raise ValueError("Status filtering requires a database and image")
        with closing(sqlite3.connect(database.resolve().as_uri() + "?mode=ro", uri=True)) as connection:
            rows = connection.execute(
                "SELECT address, sha256, status FROM functions WHERE image=?", (image,)).fetchall()
        identities = {address: (sha256, current_status) for address, sha256, current_status in rows}
        selected = set()
        for item in document:
            identity = identities.get(item.get("address"))
            if identity is None or identity[0] != item.get("sha256"):
                raise ValueError("Status database does not match the original function export")
            if identity[1] == status:
                selected.add(item["address"])
    by_address = {}
    for item in document:
        address = item.get("address")
        name = item.get("name")
        if type(address) is not int or address & 3 or not isinstance(name, str) or not name:
            raise ValueError("Function export contains an invalid entry")
        if address in by_address and by_address[address] != name:
            raise ValueError(f"Function export contains conflicting address 0x{address:08X}")
        by_address[address] = name
    result = [{"address": address, "name": by_address[address]} for address in sorted(by_address)
              if selected is None or address in selected]
    if not result or len(result) > 2048:
        raise ValueError(f"Function coverage requires 1..2048 targets, got {len(result)}")
    return result


def expect(remote: Remote, command: str, expected: str) -> str:
    reply = remote.packet(command)
    if not reply.startswith(expected):
        raise RuntimeError(f"{command} failed: {reply}")
    return reply


def configure_paused_launch(directory: Path, port: int, hidden: bool) -> None:
    validate_settings(directory, port)
    ini = configparser.ConfigParser(interpolation=None)
    ini.optionxform = str
    ini.read(directory / "settings.ini", encoding="utf-8-sig")
    ini["Main"]["StartPaused"] = "true"
    ini["Main"]["EmulationSpeed"] = "0" if hidden else "1"
    ini["BIOS"]["PatchFastBoot"] = "true"
    with (directory / "settings.ini").open("w", encoding="utf-8") as stream:
        ini.write(stream)


def start(args) -> None:
    root, config = load_project()
    role = args.role
    port = int(config["duckstation"]["reserved_gdb_ports"][role])
    directory = data_directory(role=role)
    receipt_path = root / "status/runtime/function-coverage.json"
    if receipt_path.is_file():
        previous = json.loads(receipt_path.read_text(encoding="utf-8-sig"))
        if previous.get("status") == "running" and alive(previous.get("process")):
            raise RuntimeError("Function coverage is already active")
    try:
        owner = listener_process(port)
    except OSError:
        owner = None
    if owner:
        raise RuntimeError(f"Port {port} already belongs to PID {owner['Id']}")

    functions_path = project_path("ida_exports") / args.image / "functions.json"
    database = project_path("analysis_database") if args.status else None
    functions = load_functions(functions_path, status=args.status, database=database, image=args.image)
    configure_paused_launch(directory, port, args.hidden)
    command = [str(executable()), "-fastboot", str(project_path("disc_image"))]
    if args.hidden:
        command.insert(1, "-batch")
    process = launch_process(command, directory, environment(directory), visible=not args.hidden)
    owner_receipt = record_owner(directory, port, process.pid)
    deadline = time.monotonic() + 30
    while True:
        try:
            owner = listener_process(port)
            if owner["Id"] != process.pid:
                raise RuntimeError("Unexpected GDB endpoint owner")
            break
        except OSError:
            if process.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("DuckStation did not expose its GDB endpoint")
            time.sleep(0.1)

    remote = Remote(config["duckstation"].get("host", "127.0.0.1"), port)
    try:
        features = remote.packet("qSupported")
        missing = [feature for feature in FEATURES if feature not in features]
        if missing:
            raise RuntimeError("DuckStation lacks function coverage commands: " + ", ".join(missing))
        expect(remote, "qXportFunctionCoverageStart", "OK:coverage")
        for item in functions:
            expect(remote, f"qXportFunctionCoverageTarget:{item['address']:x}", "OK:coverage_target")
        started = datetime.now(timezone.utc)
        run_name = validate_run_name(args.name or started.strftime("run-%Y%m%dT%H%M%SZ"))
        output = root / "status/function-coverage" / run_name
        output.mkdir(parents=True, exist_ok=False)
        receipt = {
            "schema": 1,
            "contract": "xport-function-coverage-v1",
            "status": "running",
            "project": str(root),
            "image": args.image,
            "role": role,
            "port": port,
            "process": {key: owner_receipt[key] for key in ("pid", "created")},
            "source": str(functions_path),
            "registered": len(functions),
            "started_at": started.isoformat(),
            "output": str(output),
        }
        if args.status:
            receipt["target_filter"] = {
                "status": args.status,
                "database": str(database),
                "database_sha256": hashlib.sha256(database.read_bytes()).hexdigest(),
                "full_image_coverage": False,
            }
        write_receipt(receipt_path, receipt)
        write_receipt(output / "receipt.json", receipt)
        remote.resume_acknowledged()
    except Exception:
        process.terminate()
        raise
    finally:
        remote.close()
    print(json.dumps({"status": "running", "pid": process.pid, "targets": len(functions),
                      "output": str(output)}))


def read_page(remote: Remote, offset: int) -> tuple[int, list[int]]:
    reply = remote.packet(f"qXportFunctionCoverageRead:{offset:x}:40")
    fields = reply.split(":", 3)
    if len(fields) != 4 or fields[0] != "COVERAGE" or int(fields[2], 16) != offset:
        raise RuntimeError(f"Invalid coverage page: {reply}")
    total = int(fields[1], 16)
    addresses = []
    if fields[3]:
        for encoded in fields[3].split(";"):
            addresses.append(int(encoded, 16))
    return total, addresses


def encode_addresses(addresses: list[int]) -> bytes:
    if len(addresses) != len(set(addresses)):
        raise RuntimeError("DuckStation returned duplicate function addresses")
    if any(type(address) is not int or not 0 < address <= 0xFFFFFFFF or address & 3
           for address in addresses):
        raise RuntimeError("DuckStation returned an invalid function address")
    return b"".join(struct.pack("<I", address) for address in addresses)


def stop_coverage(remote: Remote) -> None:
    deadline = time.monotonic() + 5
    while True:
        reply = remote.packet("qXportFunctionCoverageStop")
        if reply.startswith("OK:coverage:"):
            return
        if reply != "ERROR:coverage:coverage_not_active":
            raise RuntimeError(f"qXportFunctionCoverageStop failed: {reply}")
        probe = remote.packet("qXportFunctionCoverageRead:0:1")
        if probe.startswith("COVERAGE:"):
            return
        if time.monotonic() >= deadline:
            raise RuntimeError(f"Function coverage did not reach a stopped state: {probe}")
        time.sleep(0.05)


def stop(args) -> None:
    root, config = load_project()
    receipt_path = root / "status/runtime/function-coverage.json"
    if not receipt_path.is_file():
        raise RuntimeError("No function coverage session is active")
    receipt = json.loads(receipt_path.read_text(encoding="utf-8-sig"))
    if receipt.get("status") != "running" or not alive(receipt.get("process")):
        raise RuntimeError("Function coverage receipt does not identify a live session")
    port = int(receipt["port"])
    remote = Remote(config["duckstation"].get("host", "127.0.0.1"), port)
    try:
        remote.s.settimeout(30)
        stop_coverage(remote)
        addresses = []
        total = None
        while total is None or len(addresses) < total:
            page_total, page = read_page(remote, len(addresses))
            if total is None:
                total = page_total
            if page_total != total or not page and len(addresses) < total:
                raise RuntimeError("Coverage page sequence is incomplete")
            addresses.extend(page)
    finally:
        remote.close()

    output = Path(receipt["output"])
    binary_path = output / "functions.bin"
    temporary = binary_path.with_suffix(".bin.tmp")
    temporary.write_bytes(encode_addresses(addresses))
    os.replace(temporary, binary_path)
    stopped_at = datetime.now(timezone.utc).isoformat()
    receipt.update(status="complete", stopped_at=stopped_at,
                   unique_functions=len(addresses), binary_list=str(binary_path),
                   byte_size=binary_path.stat().st_size,
                   format="Packed little-endian uint32 addresses in first-call order")
    write_receipt(receipt_path, receipt)
    write_receipt(output / "receipt.json", receipt)
    stop_owned_endpoint(data_directory(role=receipt["role"]), port=port)
    print(json.dumps({"status": "complete", "functions": total,
                      "bytes": binary_path.stat().st_size,
                      "output": str(binary_path)}))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="action", required=True)
    start_parser = subparsers.add_parser("start")
    start_parser.add_argument("--image", default="MAIN.EXE")
    start_parser.add_argument("--role", default="user")
    start_parser.add_argument("--name")
    start_parser.add_argument("--hidden", action="store_true")
    start_parser.add_argument("--status", choices=("TODO", "WIP", "DONE", "SKIP"))
    subparsers.add_parser("stop")
    args = parser.parse_args()
    start(args) if args.action == "start" else stop(args)


if __name__ == "__main__":
    main()

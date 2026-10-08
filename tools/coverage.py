#!/usr/bin/env python3
"""Drive evidence-bound C translation of one recorded function set"""

from __future__ import annotations

import argparse
import contextlib
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import sqlite3
import struct
import subprocess
import sys

from coverage_translation import apply_translations
from function_coverage import validate_run_name
from lockstep_audit import checklist
from lockstep_repair_proposal import apply_proposal
from lockstep_supervisor import source_snapshot
from source_context import compact
from source_index import functions as indexed_functions
from trace_worker import write_receipt
from coverage_qualified import image_of, validate_state as validate_image_state, rows as qualified_rows, dependencies as qualified_dependencies, guard_packet as guard_dependency_packet
from xport_process import normalized_environment
from xport_project import load_project
from coverage_missing_stubs import authorized_audit, policy_entries, validate_stub_body, system_function_allowed, stub_summary
from coverage_absent_devices import authorized_audit as authorized_absent_audit, policy_entries as absent_policy_entries, validate_noop_body, admission_summary as absent_admission_summary, retain_admissions as retain_absent_admissions


CONTRACT = "xport-function-audit-coverage-v1"
SOURCE_ROOTS = (Path("src"),)
BATCH_SIZE = 16
MODULE_LINE_LIMIT = 2000


def shared_source_root() -> Path:
    _root, project = load_project()
    return Path(project["toolset"]).resolve().parent


def checked_shared_snapshot(packet: dict) -> tuple[Path, dict]:
    root = shared_source_root()
    snapshot = source_snapshot(root, SOURCE_ROOTS)
    identity = (packet.get("identities") or {}).get("shared_source_identity")
    if not identity:
        raise ValueError("Coverage packet has no shared source binding; a new packet revision is required")
    if identity != snapshot["identity"]:
        raise ValueError("Shared source changed after the coverage packet was created")
    return root, snapshot


def digest_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def digest(path: Path) -> str:
    return digest_bytes(path.read_bytes())


def read_json(path: Path) -> dict:
    value = json.loads(path.read_text(encoding="utf-8-sig"))
    if not isinstance(value, dict):
        raise ValueError(f"Expected a JSON object: {path}")
    return value


def paths(root: Path, name: str) -> dict[str, Path]:
    directory = root / "status" / "function-coverage" / validate_run_name(name)
    return {
        "directory": directory,
        "functions": directory / "functions.bin",
        "recording": directory / "receipt.json",
        "state": directory / "coverage.json",
        "packet": directory / "coverage-next.json",
    }


def read_addresses(path: Path) -> list[int]:
    raw = path.read_bytes()
    if not raw or len(raw) % 4:
        raise ValueError("functions.bin must be nonempty packed little-endian uint32 data")
    addresses = [row[0] for row in struct.iter_unpack("<I", raw)]
    if len(addresses) != len(set(addresses)) or any(address & 3 for address in addresses):
        raise ValueError("functions.bin contains duplicate or unaligned addresses")
    return addresses


def recording_receipt(root: Path, selected: dict[str, Path], image: str) -> dict:
    path = selected["recording"]
    if path.is_file():
        receipt = read_json(path)
    else:
        runtime = root / "status" / "runtime" / "function-coverage.json"
        receipt = read_json(runtime) if runtime.is_file() else {}
        if Path(receipt.get("output", "")).resolve() != selected["directory"].resolve():
            count = selected["functions"].stat().st_size // 4
            receipt = {
                "schema": 1,
                "contract": "xport-function-coverage-v1",
                "status": "complete",
                "image": image,
                "binary_list": str(selected["functions"]),
                "unique_functions": count,
                "legacy_default_image_binding": True,
            }
    if (receipt.get("contract") != "xport-function-coverage-v1" or
            receipt.get("status") != "complete" or receipt.get("image") != image or
            Path(receipt.get("binary_list", "")).resolve() != selected["functions"].resolve()):
        raise ValueError("Function coverage recording is incomplete or belongs to another image")
    if receipt.get("unique_functions") != selected["functions"].stat().st_size // 4:
        raise ValueError("Function coverage receipt count does not match functions.bin")
    return receipt


def function_rows(root: Path, database: Path, image: str,
                  addresses: list[int]) -> list[dict]:
    with contextlib.closing(sqlite3.connect(database.as_uri() + "?mode=ro", uri=True)) as db:
        db.row_factory = sqlite3.Row
        tables = {row[0] for row in db.execute(
            "SELECT name FROM sqlite_master WHERE type='table'")}
        if "implementations" not in tables:
            raise ValueError("Source index is absent; run build_database")
        result = []
        for address in addresses:
            row = db.execute(
                """SELECT f.address,f.name,f.status,f.sha256,f.listing,f.pseudocode,
                          i.mapping_status,i.source_path,i.symbol,i.body_sha256,
                          lc.alias AS library_alias
                   FROM functions f LEFT JOIN implementations i
                     ON i.image=f.image AND i.address=f.address
                   LEFT JOIN library_classification lc
                     ON lc.image=f.image AND lc.address=f.address
                   WHERE f.image=? AND f.address=?""", (image, address)).fetchone()
            if row is None:
                raise ValueError(f"Recorded address is absent from analysis: 0x{address:08X}")
            result.append(dict(row))
    return result


def direct_link_dependencies(database: Path, image: str,
                             recorded: list[int]) -> list[int]:
    dependencies = []
    seen = set(recorded)
    with contextlib.closing(sqlite3.connect(database.as_uri() + "?mode=ro", uri=True)) as db:
        for address in recorded:
            reviewed_skip = db.execute(
                """SELECT 1 FROM functions f JOIN library_classification lc
                     ON lc.image=f.image AND lc.address=f.address
                   WHERE f.image=? AND f.address=? AND f.status='SKIP'""",
                (image, address)).fetchone()
            if reviewed_skip:
                continue
            targets = db.execute(
                """SELECT DISTINCT c.target_function
                     FROM edges e JOIN edge_candidates c ON c.edge=e.id
                    WHERE e.source_image=? AND e.source_function=?
                      AND c.target_image=?
                      AND e.kind IN ('call','tail_candidate','conditional_link')
                    ORDER BY e.site,c.target_function""",
                (image, address, image)).fetchall()
            for target, in targets:
                if target not in seen:
                    seen.add(target)
                    dependencies.append(target)
    return dependencies


def register_source(project_path: Path, source: Path) -> bool:
    text = project_path.read_text(encoding="utf-8-sig")
    relative = os.path.relpath(source, project_path.parent)
    includes = re.findall(r'<ClCompile\s+Include="([^"]+)"', text)
    if any(value.replace("/", "\\").lower() == relative.lower()
           for value in includes):
        return False
    if text.count("</Project>") != 1:
        raise ValueError("Invalid native project closing tag")
    project_path.write_text(text.replace(
        "</Project>", '<ItemGroup><ClCompile Include="' + relative +
        '" /></ItemGroup>\n</Project>'), encoding="utf-8")
    return True


def sync_link_stubs(root: Path, project: dict, image: str,
                    recorded: list[int], dependencies: list[dict], qualified_recorded=None) -> dict:
    path = root / "src" / "game_link_stubs.c"
    ledger_path = root / "status" / "translation-ledger.json"
    ledger = read_json(ledger_path)
    entries = {(item["image"], item["address"]): item
               for item in ledger.get("entries") or []}
    retained = {}
    requested = set()
    recorded_keys = set(qualified_recorded) if qualified_recorded is not None else {(image, address) for address in recorded}
    for key, entry in entries.items():
        if (entry.get("link_only_stub") is True and entry.get("status") == "TODO" and
                key not in recorded_keys):
            retained[key] = entry
    for row in dependencies:
        if row.get("mapping_status") == "mapped" or (
                row.get("status") == "SKIP" and row.get("library_alias")):
            continue
        symbol = row["name"]
        if not re.fullmatch(r"[A-Za-z_]\w*", symbol):
            raise ValueError(f"Link-only stub has an invalid C symbol: {symbol}")
        row_image = row.get("image", image)
        key = (row_image, row["address"])
        entry = entries.get(key)
        if entry is None:
            entry = {
                "image": row_image,
                "address": row["address"],
                "sha256": row["sha256"],
                "status": "TODO",
                "source": "src/game_link_stubs.c",
                "semantic_name": symbol,
                "evidence": [],
                "link_only_stub": True,
                "note": "Link-only abort stub for an unrecorded direct dependency",
            }
            ledger.setdefault("entries", []).append(entry)
            entries[key] = entry
        elif entry.get("sha256") != row["sha256"]:
            raise ValueError("Link-only stub has stale original identity")
        elif entry.get("status") != "TODO":
            continue
        else:
            entry["source"] = "src/game_link_stubs.c"
            entry["semantic_name"] = symbol
            entry["link_only_stub"] = True
        retained[key] = entry
        requested.add(key)
    lines = [
        '#include "psx.h"',
        "#include <stdlib.h>",
        "",
        "#define XPORT_LINK_STUB(symbol, address, image) \\",
        "    uint32 symbol() { FUNCTION_MARKER(address, image); abort(); return 0; }",
        "",
    ]
    for key in sorted(retained):
        entry = retained[key]
        lines.append(f'XPORT_LINK_STUB({entry["semantic_name"]}, '
                     f'0x{entry["address"]:08X}u, "{entry["image"]}")')
    content = "\n".join(lines) + "\n"
    changed = not path.is_file() or path.read_text(encoding="utf-8-sig") != content
    if retained or path.is_file():
        path.write_text(content, encoding="utf-8")
        solution = (root / project["paths"]["native_solution"]).resolve()
        projects = list(solution.parent.glob("*.vcxproj"))
        if len(projects) != 1:
            raise ValueError("Link-only stubs need one unambiguous native project")
        changed = register_source(projects[0], path) or changed
    ledger_text = json.dumps(ledger, indent=2) + "\n"
    if ledger_path.read_text(encoding="utf-8-sig") != ledger_text:
        ledger_path.write_text(ledger_text, encoding="utf-8")
        changed = True
    return {
        "count": len(retained),
        "changed": changed,
        "source": path.relative_to(root).as_posix() if retained else None,
        "functions": [{"image": key[0], "address": key[1],
                       "symbol": retained[key]["semantic_name"]}
                      for key in sorted(requested)],
    }


def is_c_implementation(row: dict) -> bool:
    return row.get("mapping_status") == "mapped" and row.get("status") in {
        "WIP", "DONE", "SKIP"
    }


def eligible_rows(rows: list[dict]) -> list[dict]:
    return [row for row in rows
            if not (row.get("status") == "SKIP" and row.get("library_alias"))]


def progress(state: dict) -> dict:
    entries = state["functions"]
    return {
        "proved": sum(item.get("proof_status") == "proved" for item in entries),
        "decompiled": sum(item.get("c_status") == "decompiled" for item in entries),
        "total": len(entries),
    }


def progress_text(value: dict) -> str:
    return f"{value['proved']}/{value['decompiled']}/{value['total']}"


def physical_lines(path: Path) -> int:
    if not path.is_file():
        return 0
    raw = path.read_bytes()
    return raw.count(b"\n") + (1 if raw and not raw.endswith(b"\n") else 0)


def module_plan(root: Path) -> dict:
    source = root / "src"
    main = source / "game_main.c"
    batches = []
    for path in source.glob("game_batch*.c"):
        suffix = path.stem.removeprefix("game_batch")
        if suffix.isdigit():
            batches.append((int(suffix), path))
    batches.sort()
    active = batches[-1][1] if batches else main
    index = batches[-1][0] if batches else 0
    lines = physical_lines(active)
    return {
        "line_limit": MODULE_LINE_LIMIT,
        "active_module": active.relative_to(root).as_posix(),
        "active_lines": lines,
        "remaining_lines": max(0, MODULE_LINE_LIMIT - lines),
        "next_module": f"src/game_batch{index + 1}.c",
        "batch_is_indivisible": True,
    }


def save_state(path: Path, state: dict) -> None:
    state["progress"] = progress(state)
    write_receipt(path, state)


def start_qualified(root, project, state, selected, receipt):
    if state.get("image") != project.get("analysis", {}).get("default_image") or digest(selected["functions"]) != state.get("functions_sha256"):
        raise ValueError("Migrated coverage differs from configured immutable recording")
    recorded = read_addresses(selected["functions"])
    if recorded != [item["address"] for item in state["functions"]]:
        raise ValueError("Migrated coverage order or denominator changed")
    database = (root / project["paths"]["analysis_database"]).resolve()
    rows = qualified_rows(root, database, state)
    for item, row in zip(state["functions"], rows):
        if item["original_sha256"] != row["sha256"]:
            raise ValueError("Migrated function original identity changed")
        item["name"] = row["name"]
        item["c_status"] = "decompiled" if is_c_implementation(row) else "missing"
    dependencies, pending = qualified_dependencies(root, project, state)
    recorded_keys = {(image_of(state, item), item["address"]) for item in state["functions"]}
    stubs = sync_link_stubs(root, project, state["image"], recorded, dependencies, recorded_keys)
    if stubs["changed"]:
        source_refresh(root)
    state["dependency_pending"] = pending
    state["link_only_stubs"] = stubs
    save_state(selected["state"], state)
    return {"status": state["status"], "progress": progress_text(state["progress"]),
            "recording": receipt["binary_list"], "state": str(selected["state"]),
            "dependency_pending": pending, "main_dependency_unresolved": state.get("main_dependency_unresolved", [])}


def migrate_images(name, request_path):
    import coverage_image_migration
    import uuid
    root, project = load_project()
    selected = paths(root, name)
    request_path = request_path.resolve()
    if not request_path.is_relative_to(root / "status") or not request_path.is_file():
        raise ValueError("Migration request must be project-owned status evidence")
    request = read_json(request_path)
    if (root / request["state"]["path"]).resolve() != selected["state"].resolve():
        raise ValueError("Migration request belongs to another coverage run")
    state, receipt = coverage_image_migration.validate(root, project, request)
    dependencies, pending = qualified_dependencies(root, project, state)
    state["dependency_pending"] = pending
    receipt["known_dependencies"] = [{"image": row["image"], "address": row["address"], "function_sha256": row["sha256"]} for row in dependencies]
    receipt["dependency_pending"] = pending
    receipt["main_dependency_unresolved"] = state.get("main_dependency_unresolved", [])
    receipt["request_path"] = request_path.relative_to(root).as_posix()
    receipt["request_sha256"] = digest(request_path)
    backup = selected["directory"] / ("image-migration-preserved-" + uuid.uuid4().hex)
    backup.mkdir()
    before = selected["state"].read_bytes()
    (backup / "coverage.json").write_bytes(before)
    for binding in request["packets"]:
        original = (root / binding["path"]).resolve()
        (backup / original.name).write_bytes(original.read_bytes())
    receipt["preserved_directory"] = backup.relative_to(root).as_posix()
    receipt_path = backup / "migration-receipt.json"
    try:
        write_receipt(receipt_path, receipt)
        state["image_migration"] = {"path": receipt_path.relative_to(root).as_posix(), "sha256": digest(receipt_path)}
        save_state(selected["state"], state)
        validate_image_state(root, state)
    except BaseException:
        selected["state"].write_bytes(before)
        receipt_path.unlink(missing_ok=True)
        raise
    return {"status": "image_migrated", "progress": progress_text(state["progress"]),
            "migrated_functions": len(receipt["changes"]), "receipt": str(receipt_path),
            "dependency_pending": pending, "original_capture_unchanged": True}


def start(name: str) -> dict:
    root, project = load_project()
    selected = paths(root, name)
    if not selected["functions"].is_file():
        raise ValueError(f"Function coverage does not exist: {selected['functions']}")
    image = project.get("analysis", {}).get("default_image")
    if not image:
        raise ValueError("analysis.default_image is required")
    receipt = recording_receipt(root, selected, image)
    if selected["state"].is_file():
        existing = read_json(selected["state"])
        validate_image_state(root, existing)
        if existing.get("image_migration"):
            return start_qualified(root, project, existing, selected, receipt)
    recorded_addresses = read_addresses(selected["functions"])
    database = (root / project["paths"]["analysis_database"]).resolve()
    dependency_addresses = direct_link_dependencies(database, image, recorded_addresses)
    dependency_rows = function_rows(root, database, image, dependency_addresses)
    stubs = sync_link_stubs(root, project, image, recorded_addresses, dependency_rows)
    if stubs["changed"]:
        source_refresh(root)
    recorded_rows = function_rows(root, database, image, recorded_addresses)
    rows = eligible_rows(recorded_rows)
    identity = digest(selected["functions"])
    if selected["state"].is_file():
        state = read_json(selected["state"])
        if (state.get("contract") != CONTRACT or state.get("functions_sha256") != identity or
                state.get("image") != image):
            raise ValueError("Existing coverage audit belongs to another function set")
    else:
        state = {
            "schema": 1,
            "contract": CONTRACT,
            "status": "running",
            "name": name,
            "image": image,
            "recording_receipt": (str(selected["recording"])
                                  if selected["recording"].is_file() else None),
            "legacy_default_image_binding": receipt.get("legacy_default_image_binding", False),
            "functions_sha256": identity,
            "source_recording_sha256": digest(selected["functions"]),
            "recorded_functions": len(recorded_addresses),
            "link_only_stubs": stubs,
            "excluded_psyq": len(recorded_rows) - len(rows),
            "functions": [{
                "address": row["address"],
                "name": row["name"],
                "original_sha256": row["sha256"],
                "c_status": "decompiled" if is_c_implementation(row) else "missing",
                "proof_status": "proved" if row["status"] == "DONE" else "pending",
                "proof_basis": "existing_done" if row["status"] == "DONE" else None,
            } for row in rows],
        }
    current = {row["address"]: row for row in rows}
    state["recorded_functions"] = len(recorded_addresses)
    state["link_only_stubs"] = stubs
    outside = [item for item in state["functions"] if item["address"] not in current]
    if outside and state.get("active_packet"):
        raise ValueError("Active coverage packet uses the obsolete static-closure denominator")
    state["functions"] = [item for item in state["functions"]
                          if item["address"] in current]
    for item in state["functions"]:
        row = current[item["address"]]
        if item["original_sha256"] != row["sha256"]:
            raise ValueError(f"Original function changed: 0x{item['address']:08X}")
        item["name"] = row["name"]
        item["c_status"] = "decompiled" if is_c_implementation(row) else "missing"
    save_state(selected["state"], state)
    return {"status": state["status"], "progress": progress_text(state["progress"]),
            "recording": receipt["binary_list"], "state": str(selected["state"])}


def load_state(name: str) -> tuple[Path, dict, dict, dict[str, Path]]:
    root, project = load_project()
    selected = paths(root, name)
    if not selected["state"].is_file():
        start(name)
    state = read_json(selected["state"])
    if state.get("contract") != CONTRACT or state.get("name") != name:
        raise ValueError("Invalid function audit coverage state")
    validate_image_state(root, state)
    if state.get("image_migration") and state["image"] != project.get("analysis", {}).get("default_image"):
        raise ValueError("Migrated recording default image changed")
    from coverage_qualified import validate_inline_state, validate_inline_admissions
    validate_inline_admissions(root, state)
    validate_inline_state(root, project, state)
    from coverage_indirect import validate_state as validate_indirect_state
    validate_indirect_state(root, project, state)
    return root, project, state, selected


def admit_inline_dependencies(name: str, review_path: Path) -> dict:
    import copy
    import uuid
    import coverage_image_migration as migration
    from coverage_qualified import validate_inline_state
    root, project, state, selected = load_state(name)
    review_path = review_path.resolve()
    if not review_path.is_relative_to((root / 'status').resolve()) or not review_path.is_file():
        raise ValueError('Inline dependency review must be project-owned status evidence')
    binding = {'path': review_path.relative_to(root).as_posix(), 'sha256': digest(review_path)}
    review = read_json(review_path)
    key = (review['source_image'], review['source_function'], review['site'], review['target'])
    for current in state.get('dependency_inline_dispositions', []):
        previous = read_json(migration.bound(root, current, 'status'))
        if key == (previous['source_image'], previous['source_function'], previous['site'], previous['target']):
            raise ValueError('Duplicate inline dependency disposition')
    gaps = state.get('main_dependency_unresolved', []) + state.get('dependency_pending', [])
    if not any(key == (row['source_image'], row['source_function'], row['site'], row['target']) for row in gaps):
        raise ValueError('Inline disposition does not match an existing unresolved edge')
    database = (root / project['paths']['analysis_database']).resolve()
    from contextlib import closing
    with closing(sqlite3.connect(database)) as db:
        matches = db.execute("SELECT COUNT(*) FROM edges WHERE source_image=? AND source_function=? AND site=? AND target=? AND kind='call'", key).fetchone()[0]
    if matches != 1:
        raise ValueError('Inline disposition lacks unique original database call edge')
    updated = copy.deepcopy(state)
    updated.setdefault('dependency_inline_dispositions', []).append(binding)
    validate_inline_state(root, project, updated)
    for field in ['main_dependency_unresolved', 'dependency_pending']:
        updated[field] = [row for row in updated.get(field, []) if key != (row['source_image'], row['source_function'], row['site'], row['target'])]
    before = selected['state'].read_bytes()
    preserved = selected['directory'] / ('inline-dependency-preserved-' + uuid.uuid4().hex)
    preserved.mkdir()
    receipt = {'schema': 1, 'contract': 'xport-coverage-inline-admission-v1', 'review': binding,
               'state_before_sha256': migration.digest(before), 'edge': list(key)}
    try:
        (preserved / 'coverage.json').write_bytes(before)
        receipt_path = preserved / 'admission-receipt.json'
        receipt_path.write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
        updated.setdefault('dependency_inline_admissions', []).append({'path': receipt_path.relative_to(root).as_posix(), 'sha256': digest(receipt_path)})
        save_state(selected['state'], updated)
    except Exception:
        selected['state'].write_bytes(before)
        for child in preserved.iterdir():
            child.unlink()
        preserved.rmdir()
        raise
    return {'status': updated['status'], 'review': binding, 'receipt': str(receipt_path),
            'main_dependency_unresolved': updated.get('main_dependency_unresolved', [])}


def finish_release(root: Path, state: dict, selected: dict) -> None:
    completed = copy.deepcopy(state)
    completed["release_build"] = release_build(root)
    completed["status"] = "complete"
    save_state(selected["state"], completed)
    state.clear()
    state.update(completed)


def status(name: str) -> dict:
    root, project, state, selected = load_state(name)
    database = (root / project["paths"]["analysis_database"]).resolve()
    rows = qualified_rows(root, database, state)
    for item, row in zip(state["functions"], rows):
        item["c_status"] = "decompiled" if is_c_implementation(row) else "missing"
    all_proved = all(item["proof_status"] == "proved" for item in state["functions"]) and not state.get("dependency_pending") and not state.get("main_dependency_unresolved")
    if all_proved:
        if state.get("release_build", {}).get("exit_code") == 0:
            state["status"] = "complete"
            save_state(selected["state"], state)
        else:
            state["status"] = "release_required"
            save_state(selected["state"], state)
            finish_release(root, state, selected)
    else:
        state["status"] = "running"
        save_state(selected["state"], state)
    value = state["progress"]
    return {"status": state["status"], "progress": progress_text(value), **value,
            "missing_stub_count": state.get("missing_stub_count", 0),
            "missing_stubs": state.get("missing_stubs", []),
            "sdk_system_delegation_count": state.get("sdk_system_delegation_count", 0),
            "sdk_system_delegations": state.get("sdk_system_delegations", []),
            "game_frame_delegation_count": state.get("game_frame_delegation_count", 0),
            "game_frame_delegations": state.get("game_frame_delegations", []),
            "absent_device_count": state.get("absent_device_count", 0),
            "absent_devices": state.get("absent_devices", []),
            "dependency_pending": state.get("dependency_pending", []),
            "main_dependency_unresolved": state.get("main_dependency_unresolved", []),
            "state": str(selected["state"])}


def next_packet(name: str, budget: int, reissue: bool = False, refresh_evidence: Path | None = None,
                refresh_machine_audit: bool = False, sdk_dependencies: Path | None = None, unverified: bool = False) -> dict:
    root, _project, state, selected = load_state(name)
    if refresh_evidence is not None and refresh_machine_audit:
        raise ValueError("Original evidence and machine parser refreshes must be separate")
    reissue = reissue or refresh_evidence is not None or refresh_machine_audit or sdk_dependencies is not None
    from coverage_permissions import coverage_permissions
    if unverified and not coverage_permissions(_project)["allow_unverified_translation"]:
        raise ValueError("Selected project does not authorize translation-only batches")
    active = state.get("active_packet")
    if unverified and active and not reissue:
        receipt_path = (root / active).with_suffix(".translation.json")
        if receipt_path.is_file():
            receipt = read_json(receipt_path)
            identities = {(row["image"], row["address"]) for row in receipt.get("functions", [])}
            translated = {(image_of(state, row), row["address"]) for row in state["functions"]
                          if row.get("translation_status") == "unverified"}
            if (receipt.get("contract") == "xport-unverified-translation-v1" and
                    receipt.get("packet", "").replace("\\", "/") == active.replace("\\", "/") and
                    identities and identities <= translated):
                state.setdefault("translated_packets", []).append(active)
                state["active_packet"] = None
                active = None
    previous_packet = None
    if reissue and not active:
        raise ValueError("No active coverage packet to reissue")
    if active:
        active_path = (root / active).resolve()
        if not active_path.is_file() or not active_path.is_relative_to(selected["directory"].resolve()):
            raise ValueError("Active coverage batch packet is missing")
        packet = read_json(active_path)
        if reissue:
            previous_packet = packet
        else:
            return {"status": "attention", "progress": progress_text(progress(state)),
                    "batch": [{"address": f"0x{row['address']:08X}", "name": row["name"]}
                              for row in state["functions"]
                              if row["address"] in {item["address"] for item in
                                                   packet["source_context"]["audit_contract"]["required_functions"]}],
                    "packet": str(active_path),
                    "proposal": str(selected["directory"] / "coverage-proposal.json"),
                    "serialized_bytes": active_path.stat().st_size}
    pending = [item for item in state["functions"]
               if item["proof_status"] != "proved" and (not unverified or item.get("translation_status") != "unverified")][:BATCH_SIZE]
    if not pending and unverified:
        save_state(selected["state"], state)
        return {"status": "translations_complete", "unverified_translated": state.get("unverified_translated", 0)}
    if not pending:
        if state.get("dependency_pending") or state.get("main_dependency_unresolved"):
            return {"status": "attention", "progress": progress_text(progress(state)), "dependency_pending": state.get("dependency_pending", []),
            "main_dependency_unresolved": state.get("main_dependency_unresolved", [])}
        if not state.get("release_build"):
            state["release_build"] = release_build(root)
        state["status"] = "complete"
        save_state(selected["state"], state)
        return {"status": "complete", "progress": progress_text(state["progress"])}
    contexts = []
    bindings = []
    for item in pending:
        context = compact(image_of(state, item), item["address"], budget,
                          full_source=True, full_original=True)
        if (context["mips"].get("complete") is not True or
                (context["pseudocode"].get("path") and
                 context["pseudocode"].get("content_complete", context["pseudocode"].get("complete")) is not True)):
            raise ValueError("Complete original evidence does not fit the per-function budget")
        machine = checklist(context["mips"]["lines"])
        pseudo_complete = bool(context["pseudocode"].get("path") and
                               context["pseudocode"].get("complete"))
        binding = {
            "image": image_of(state, item),
            "address": item["address"],
            "function_sha256": context["function"]["sha256"],
            "mips_sha256": context["mips"]["sha256"],
            "pseudocode_sha256": context["pseudocode"]["sha256"],
            "pseudocode_complete": pseudo_complete,
            "machine_audit_sha256": machine["sha256"],
        }
        context["machine_audit"] = machine
        contexts.append(context)
        bindings.append(binding)
    audit_contract = {
        "pseudocode_baseline_required_when_complete": True,
        "complete_mips_audit_required": True,
        "semantic_simplification_allowed": False,
        "single_observed_difference_patch_allowed": False,
        "required_functions": bindings,
    }
    contexts[0]["audit_contract"] = audit_contract
    contexts[0]["audit_cluster"] = contexts[1:]
    sdk_requests = (previous_packet or {}).get("sdk_dependency_requests", [])
    if sdk_dependencies is not None:
        request_path = sdk_dependencies.resolve()
        if not request_path.is_relative_to((root / "status").resolve()) or not request_path.is_file():
            raise ValueError("SDK dependency requests must be a project-owned status file")
        request_data = read_json(request_path)
        if (not isinstance(request_data, dict) or set(request_data) != {"schema", "dependencies"} or request_data["schema"] != 1 or
                not isinstance(request_data["dependencies"], list) or not request_data["dependencies"]):
            raise ValueError("SDK dependency request file is invalid")
        sdk_requests = request_data["dependencies"]
    sdk_contexts = []
    if sdk_requests:
        from coverage_dependency import build_sdk_dependencies
        sdk_contexts = build_sdk_dependencies(root, _project, contexts, sdk_requests, budget)
    source = source_snapshot(root, SOURCE_ROOTS)
    shared = source_snapshot(shared_source_root(), SOURCE_ROOTS)
    signature = digest_bytes(json.dumps({
        "coverage": state["functions_sha256"], "bindings": bindings,
        "source": source["identity"], "shared_source": shared["identity"],
        **({"sdk_dependencies": [row["binding"] for row in sdk_contexts]} if sdk_contexts else {}),
    }, sort_keys=True, separators=(",", ":")).encode())
    packet = {
        "schema": 1,
        "contract": "xport-function-audit-packet-v1",
        "coverage_name": name,
        "failure_signature": signature,
        "progress": progress_text(progress(state)),
        "source_identity": source["identity"],
        "identities": {"shared_source_identity": shared["identity"]},
        "batch_size": len(contexts),
        "module_plan": module_plan(root),
        "link_only_stubs": state.get("link_only_stubs", {}).get("functions", []),
        "source_context": contexts[0],
        **({"sdk_dependency_requests": sdk_requests, "sdk_dependencies": sdk_contexts} if sdk_contexts else {}),
        "instructions": [
            "Use complete IDA pseudocode as the structural baseline when present",
            "Audit every MIPS block, branch, delay slot, call, argument, return and memory effect",
            "Audit all functions in this batch before returning the proposal",
            "Keep the complete batch in one module and use next_module when it would exceed 2000 lines",
            "Use the supplied link-only stub symbols for unrecorded direct callees; do not audit or expand them",
            "Return exact source edits and complete machine_coverage; do not edit files directly",
            "Set unresolved_semantics to [] only when each whole function is proved",
        ],
    }
    batch_number = len(state.get("batches") or []) + len(state.get("translated_packets") or []) + 1
    packet_path = selected["directory"] / f"coverage-batch-{batch_number:04d}.json"
    if previous_packet is not None:
        old_bindings = previous_packet["source_context"]["audit_contract"]["required_functions"]
        if refresh_machine_audit:
            from coverage_machine_refresh import verify_machine_refresh
            packet["machine_audit_refresh"] = verify_machine_refresh(
                previous_packet, old_bindings, bindings, contexts, state)
        elif refresh_evidence is not None:
            from coverage_evidence import verify_refresh
            packet["evidence_refresh"] = verify_refresh(
                root, _project, state, refresh_evidence, old_bindings, bindings, contexts)
        elif bindings != old_bindings:
            raise ValueError("Reissued coverage packet changed original evidence or batch order")
        revision = 1
        while packet_path.exists():
            packet_path = selected["directory"] / f"coverage-batch-{batch_number:04d}-revision-{revision:04d}.json"
            revision += 1
        packet["supersedes"] = active
    elif packet_path.exists():
        raise ValueError("Coverage packet already exists; refusing to overwrite immutable evidence")
    write_receipt(packet_path, packet)
    if previous_packet is not None:
        state.setdefault("packet_revisions", []).append({
            "previous": active, "replacement": packet_path.relative_to(root).as_posix(),
            "previous_sha256": digest((root / active).resolve()),
            **({"evidence_refresh": packet["evidence_refresh"]} if "evidence_refresh" in packet else {}),
            **({"machine_audit_refresh": packet["machine_audit_refresh"]} if "machine_audit_refresh" in packet else {}),
        })
    state["active_packet"] = packet_path.relative_to(root).as_posix()
    save_state(selected["state"], state)
    return {"status": "attention", "progress": progress_text(progress(state)),
            "batch": [{"address": f"0x{item['address']:08X}", "name": item["name"],
                       "decompiled": item["c_status"] == "decompiled"}
                      for item in pending],
            "packet": str(packet_path),
            "proposal": str(selected["directory"] / "coverage-proposal.json"),
            "serialized_bytes": packet_path.stat().st_size}


def validate_audit(packet_path: Path, after: dict, changed: set[str] | None) -> bool:
    packet = read_json(packet_path)
    receipt = read_json(packet_path.with_suffix(".audit.json"))
    required = packet["source_context"]["audit_contract"]["required_functions"]
    audits = receipt.get("function_audits") or []
    if not 1 <= len(required) <= BATCH_SIZE or len(audits) != len(required):
        raise ValueError("Coverage proposal must audit the complete batch")
    contexts = [packet["source_context"],
                *(packet["source_context"].get("audit_cluster") or [])]
    sdk = packet.get("sdk_dependencies") or []
    sdk_audits = receipt.get("sdk_audits") or []
    sdk_keys = {(row["binding"]["image"], row["binding"]["address"]) for row in sdk}
    received_sdk = {(row.get("image"), row.get("address")) for row in sdk_audits if isinstance(row, dict)}
    if len(sdk_audits) != len(sdk_keys) or received_sdk != sdk_keys:
        raise ValueError("Coverage proof must audit every packet-bound SDK dependency")
    required = [*required, *(row["binding"] for row in sdk)]
    contexts.extend({**row["context"], "machine_audit": row["binding"]["machine_audit"]} for row in sdk)
    audits = [*audits, *sdk_audits]
    indexed_context = {(item["image"], int(item["address"], 0)): item
                       for item in contexts}
    indexed_audit = {(item.get("image"), item.get("address")): item for item in audits}
    if len(indexed_audit) != len(audits):
        raise ValueError("Coverage proposal contains duplicate function audits")
    for binding in required:
        key = (binding["image"], binding["address"])
        audit = indexed_audit.get(key)
        context = indexed_context.get(key)
        if audit is None or context is None:
            raise ValueError("Coverage proposal misses a batch function")
        for field in ("image", "address", "function_sha256", "mips_sha256",
                      "pseudocode_sha256", "machine_audit_sha256"):
            if audit.get(field) != binding.get(field):
                raise ValueError("Coverage audit conflicts with packet " + field)
        waiver = authorized_audit(after.get("root"), receipt, binding, audit,
                                  "sdk" if key in sdk_keys else "system_function")
        absent_waiver = authorized_absent_audit(after.get("root"), receipt, binding, audit)
        from coverage_game_frame_delegation import authorized_frame_delegation
        frame_delegation = authorized_frame_delegation(after.get("root"), packet, receipt, binding, audit)
        if frame_delegation is not None and key in sdk_keys:
            raise ValueError("Guest-frame scope cannot be relabeled as an SDK audit")
        from coverage_sdk_system_delegation import authorized_delegation
        delegation = authorized_delegation(after.get("root"), receipt, binding, audit, packet)
        if delegation is not None and key not in sdk_keys:
            raise ValueError("Recorded game algorithms cannot use SDK system delegation")
        if absent_waiver is not None:
            if key in sdk_keys:
                raise ValueError("Absent-device no-op admission currently supports recorded game wrappers only")
            if not any(a.get("image") == key[0] and a.get("address") == key[1]
                       and a.get("source_symbol") == audit.get("source_symbol")
                       for a in receipt.get("absent_device_admissions") or []):
                raise ValueError("Absent-device function has no supervisor transaction admission")
            owner = Path(after["root"]).resolve()
            source = (owner / absent_waiver["source_file"]).resolve()
            if not source.is_relative_to((owner / "src").resolve()) or not source.is_file():
                raise ValueError("Absent-device source escapes its admitted owner")
            text = source.read_text(encoding="utf-8-sig")
            parsed = [f for f in indexed_functions(text, str(source))
                      if f["name"] == absent_waiver["symbol"]]
            if len(parsed) != 1:
                raise ValueError("Absent-device source symbol is not unique")
            validate_noop_body(text[parsed[0]["start"]:parsed[0]["end"]], absent_waiver)
            from coverage_absent_devices import validate_omission_checks
            validate_omission_checks(absent_waiver, audit, context["machine_audit"]["checks"])
        if waiver is not None:
            if not any(a.get("image") == key[0] and a.get("address") == key[1] and
                       a.get("source_symbol") == audit.get("source_symbol")
                       for a in receipt.get("missing_stub_admissions") or []):
                raise ValueError("Missing stub has no supervisor transaction admission")
            if key not in sdk_keys and not system_function_allowed(context):
                raise ValueError("Recorded game behavior cannot be waived as a system stub")
            owner = shared_source_root() if key in sdk_keys else Path(after["root"])
            relative = Path(waiver["source_file"])
            if key in sdk_keys:
                relative = Path(*relative.parts[1:])
            source = (owner / relative).resolve()
            if not source.is_relative_to((owner / "src").resolve()) or not source.is_file():
                raise ValueError("Missing stub source escapes its admitted owner")
            text = source.read_text(encoding="utf-8-sig")
            parsed = [f for f in indexed_functions(text, str(relative)) if f["name"] == waiver["symbol"]]
            if len(parsed) != 1:
                raise ValueError("Missing stub source symbol is not unique")
            body = text[parsed[0]["start"]:parsed[0]["end"]]
            if key not in sdk_keys:
                body = re.sub(r'FUNCTION_MARKER\(0x[0-9A-Fa-f]+u,\s*"[^"\n]+"\)\s*;', '', body)
            validate_stub_body(body, waiver)
        if (audit.get("complete_mips_audit") is not True or
                audit.get("pseudocode_used_as_baseline") is not
                binding.get("pseudocode_complete") or
                (audit.get("unresolved_semantics") != [] and waiver is None and absent_waiver is None and delegation is None and frame_delegation is None)):
            raise ValueError("Coverage audit is incomplete")
        expected = [item["id"] for item in context["machine_audit"]["checks"]]
        reachable = source_index = None
        if key in sdk_keys:
            from lockstep_source_coverage import index_functions, reachable_functions
            sdk_path, sdk_symbol = audit.get("source_file"), audit.get("source_symbol")
            if (not isinstance(sdk_path, str) or not sdk_path.startswith("@xport/src/") or
                    not isinstance(sdk_symbol, str) or not sdk_symbol):
                raise ValueError("SDK audit must name its canonical shared implementation")
            shared_root = shared_source_root()
            shared_snapshot = source_snapshot(shared_root, SOURCE_ROOTS)
            expected_shared = receipt.get("shared_source_identity") or packet.get("identities", {}).get("shared_source_identity")
            if shared_snapshot["identity"] != expected_shared:
                raise ValueError("SDK audit shared source identity changed")
            shared_index = index_functions(shared_root, shared_snapshot["files"])
            source_index = {("@xport/" + path, symbol): {**row, "path": "@xport/" + path}
                            for (path, symbol), row in shared_index.items()}
            reachable = reachable_functions(source_index, sdk_path, sdk_symbol)
        covered = []
        for group in audit.get("machine_coverage") or []:
            ids = group.get("check_ids") if isinstance(group, dict) else None
            span = group.get("check_range") if isinstance(group, dict) else None
            if span is not None:
                if (ids is not None or not isinstance(span, list) or len(span) != 2 or
                        any(type(value) is not int for value in span) or
                        not 0 <= span[0] <= span[1] < len(expected) or
                        span[1] - span[0] >= 8):
                    raise ValueError("Invalid coverage check range")
                ids = expected[span[0]:span[1] + 1]
            path = group.get("source_file") if isinstance(group, dict) else None
            line = group.get("source_line") if isinstance(group, dict) else None
            if (not isinstance(ids, list) or not 1 <= len(ids) <= 8 or
                    not isinstance(path, str) or type(line) is not int or line < 1):
                raise ValueError("Invalid MIPS-to-C coverage mapping")
            if "root" not in after:
                raise ValueError("Source snapshot has no root")
            relative = Path(path)
            shared_mapping = relative.parts[:1] == ("@xport",)
            if shared_mapping:
                owner = shared_source_root()
                expected_shared = receipt.get("shared_source_identity") or (packet.get("identities") or {}).get("shared_source_identity")
                if not expected_shared or source_snapshot(owner, SOURCE_ROOTS)["identity"] != expected_shared:
                    raise ValueError("Shared MIPS mapping has no matching source identity")
                relative = Path(*relative.parts[1:])
            else:
                owner = Path(after["root"]).resolve()
            source = (owner / relative).resolve()
            if relative.is_absolute() or ".." in relative.parts or not source.is_relative_to((owner / "src").resolve()) or not source.is_file():
                raise ValueError("MIPS-to-C mapping escapes owned source")
            lines = source.read_text(encoding="utf-8-sig").splitlines()
            text = lines[line - 1].strip() if line <= len(lines) else ""
            if not text or text in {"{", "}", "};"} or "FUNCTION_MARKER" in text:
                raise ValueError("MIPS-to-C mapping names an empty or marker-only line")
            if reachable is not None:
                from lockstep_source_coverage import validate_location
                if not validate_location(source_index, reachable, path, group.get("source_symbol"), line):
                    raise ValueError("SDK mapping is outside its canonical implementation or reachable helper")
            covered.extend(ids)
        if covered != expected:
            raise ValueError("MIPS-to-C coverage must cover every machine check once in order")
    from coverage_sdk_system_delegation import validate_delegated_sources, delegation_summary
    from coverage_game_frame_delegation import validate_frame_delegated_sources, frame_delegation_summary
    if receipt.get("game_frame_delegation_admissions", []) != frame_delegation_summary(receipt):
        raise ValueError("Guest-frame receipt inventory differs from its explicit scope")
    if (type(receipt.get("game_frame_delegation_count", 0)) is not int or
            receipt.get("game_frame_delegation_count", 0) != len(frame_delegation_summary(receipt))):
        raise ValueError("Guest-frame receipt count differs from its admitted scope")
    if receipt.get("game_frame_delegation_admissions"):
        validate_frame_delegated_sources(after["root"], packet, receipt, shared_source_root(), {}, required)
    if receipt.get("sdk_helper_corrections"):
        from coverage_sdk_helper_corrections import validate_helper_changes
        validate_helper_changes(packet, receipt, shared_source_root(), after["root"], {})
    if any(a.get("replacement_kind") == "user_authorized_system_delegation" for a in receipt.get("sdk_audits") or []):
        validate_delegated_sources(after["root"], packet, receipt, shared_source_root(), {})
    if receipt.get("sdk_system_delegation_admissions", []) != delegation_summary(receipt):
        raise ValueError("SDK delegated-system receipt inventory differs from its audited boundaries")
    if receipt.get("absent_device_authorization"):
        absent_policy_entries(after["root"], receipt)
        expected_absent = absent_admission_summary(receipt)
        if (receipt.get("absent_device_admissions") != expected_absent or
                receipt.get("absent_device_count") != len(expected_absent)):
            raise ValueError("Absent-device receipt inventory differs from its admitted functions")
    if receipt.get("missing_stub_authorization"):
        entries = policy_entries(after["root"], receipt)
        expected_stubs = stub_summary(receipt)
        if receipt.get("missing_stub_admissions") != expected_stubs or receipt.get("missing_stub_count") != len(expected_stubs):
            raise ValueError("Missing stub receipt inventory differs from its admitted boundaries")
        for bridge in receipt.get("system_translations") or []:
            matches = [e for e in entries if e["kind"] == "system_bridge" and e["symbol"] == bridge["symbol"] and
                       e["source_file"] == bridge["path"]]
            if len(matches) != 1:
                raise ValueError("Missing system bridge receipt lacks exact authorization")
            shared_root = shared_source_root()
            expected_shared = receipt.get("shared_source_identity")
            if source_snapshot(shared_root, SOURCE_ROOTS)["identity"] != expected_shared:
                raise ValueError("Missing system bridge source identity changed")
            relative = Path(bridge["path"])
            if relative.parts[:2] != ("@xport", "src") or ".." in relative.parts:
                raise ValueError("Missing system bridge escapes canonical source")
            text = (shared_root / Path(*relative.parts[1:])).read_text(encoding="utf-8-sig")
            parsed = [f for f in indexed_functions(text, bridge["path"]) if f["name"] == bridge["symbol"]]
            if len(parsed) != 1 or text[parsed[0]["start"]:parsed[0]["end"]].strip() != bridge["text"].strip():
                raise ValueError("Missing system bridge body changed after admission")
            validate_stub_body(bridge["text"], matches[0])
    return True


def code_refresh(root: Path) -> dict:
    command = [sys.executable, "-B", str(Path(__file__).with_name("code_refresh.py")),
               "--no-format"]
    result = subprocess.run(command, cwd=root, env=normalized_environment(),
                            capture_output=True, text=True, errors="replace")
    if result.returncode:
        raise RuntimeError("Batch code_refresh failed: " + (result.stdout + result.stderr)[-1000:])
    value = json.loads(result.stdout.strip().splitlines()[-1])
    if value.get("result") != "PASS":
        raise RuntimeError("Batch code_refresh did not return PASS")
    return value


def source_refresh(root: Path) -> dict:
    command = [sys.executable, "-B", str(Path(__file__).with_name("source_index.py")),
               "--rebuild", "--no-format"]
    result = subprocess.run(command, cwd=root, env=normalized_environment(),
                            capture_output=True, text=True, errors="replace")
    if result.returncode:
        raise RuntimeError("Batch source refresh failed: " +
                           (result.stdout + result.stderr)[-1000:])
    return json.loads(result.stdout.strip().splitlines()[-1])


def release_build(root: Path) -> dict:
    command = [sys.executable, "-B", str(Path(__file__).with_name("build_native.py")),
               "--configuration", "Release"]
    result = subprocess.run(command, cwd=root, env=normalized_environment(),
                            capture_output=True, text=True, errors="replace")
    output = (result.stdout + result.stderr).strip()
    if result.returncode:
        raise RuntimeError("Final Release build failed: " + output[-1000:])
    value = json.loads(result.stdout.strip().splitlines()[-1])
    if value.get("exit_code") != 0:
        raise RuntimeError("Final Release build did not return success")
    return value


def update_ledger(root: Path, packet_path: Path, packet: dict,
                  proposal: dict) -> bytes:
    ledger_path = root / "status" / "translation-ledger.json"
    original = ledger_path.read_bytes()
    document = json.loads(original.decode("utf-8-sig"))
    entries = {(item["image"], item["address"]): item
               for item in document.get("entries") or []}
    summaries = {(item.get("image"), item.get("address")): item
                 for item in proposal.get("summaries") or [] if isinstance(item, dict)}
    required = packet["source_context"]["audit_contract"]["required_functions"]
    required_keys = {(item["image"], item["address"]) for item in required}
    if set(summaries) != required_keys:
        raise ValueError("Coverage proposal requires one summary for every batch function")
    evidence = packet_path.with_suffix(".audit.json").relative_to(root).as_posix()
    for binding in required:
        key = (binding["image"], binding["address"])
        entry = entries.get(key)
        if entry is None:
            matches = []
            for source in sorted((root / "src").glob("*.c")):
                relative = source.relative_to(root).as_posix()
                for function in indexed_functions(
                        source.read_text(encoding="utf-8-sig"), relative):
                    if (binding["address"], binding["image"]) in function["markers"]:
                        matches.append(function)
            if len(matches) != 1:
                raise ValueError(
                    f"Coverage needs one source mapping for {key[0]}:{key[1]:08X}")
            match = matches[0]
            entry = {
                "image": binding["image"],
                "address": binding["address"],
                "sha256": binding["function_sha256"],
                "status": "WIP",
                "source": match["path"],
                "semantic_name": match["name"],
                "evidence": [],
            }
            document.setdefault("entries", []).append(entry)
            entries[key] = entry
        if entry.get("sha256") != binding["function_sha256"]:
            raise ValueError("Coverage ledger entry has stale original identity")
        if entry.get("link_only_stub") is True:
            matches = []
            for source in sorted((root / "src").glob("*.c")):
                relative = source.relative_to(root).as_posix()
                for function in indexed_functions(
                        source.read_text(encoding="utf-8-sig"), relative):
                    if (binding["address"], binding["image"]) in function["markers"]:
                        matches.append(function)
            if len(matches) != 1:
                raise ValueError("Coverage did not replace the link-only stub exactly once")
            entry["source"] = matches[0]["path"]
            entry["semantic_name"] = matches[0]["name"]
            entry.pop("link_only_stub", None)
        if entry.get("status") == "TODO":
            entry["status"] = "WIP"
        retained = list(entry.get("evidence") or [])
        if evidence not in retained:
            retained.append(evidence)
        entry["evidence"] = retained
        entry["static_evidence"] = evidence
        audit = next((a for a in proposal.get("function_audits") or []
                      if (a.get("image"), a.get("address")) == key), {})
        if audit.get("replacement_kind") == "user_authorized_missing_stub":
            entry["status"] = "WIP"
            entry["replacement_kind"] = audit["replacement_kind"]
            entry["original_equivalence"] = False
            entry["waived_original_effects"] = audit["waived_original_effects"]
            entry["unresolved_semantics"] = audit["unresolved_semantics"]
            entry["missing_stub_authorization"] = proposal["missing_stub_authorization"]
        if audit.get("replacement_kind") == "user_authorized_absent_device":
            entry["status"] = "WIP"
            entry["replacement_kind"] = audit["replacement_kind"]
            entry["device"] = audit["device"]
            entry["original_equivalence"] = False
            entry["waived_original_effects"] = audit["waived_original_effects"]
            entry["unresolved_semantics"] = audit["unresolved_semantics"]
            entry["absent_device_authorization"] = proposal["absent_device_authorization"]
        summary = summaries.get(key)
        if summary:
            discrepancies = summary.get("discrepancies")
            if not isinstance(discrepancies, list) or not all(
                    isinstance(value, str) and value for value in discrepancies):
                raise ValueError("Coverage summary discrepancies must be nonempty strings")
            entry["note"] = summary.get("note") or (
                "Complete MIPS audit; runtime equivalence remains unverified")
        if entry.get("status") == "DONE" and not entry.get("runtime_evidence"):
            raise ValueError("Coverage cannot keep DONE without runtime evidence")
    ledger_path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    return original


def finalize_batch(root: Path, packet_path: Path, packet: dict,
                   proposal: dict, full_build: bool) -> dict:
    ledger_path = root / "status" / "translation-ledger.json"
    original = ledger_path.read_bytes()
    try:
        update_ledger(root, packet_path, packet, proposal)
        return code_refresh(root) if full_build else source_refresh(root)
    except BaseException:
        ledger_path.write_bytes(original)
        raise


def is_final_batch(state: dict, packet: dict) -> bool:
    required = {(item.get("image", state.get("image")), item["address"]) for item in
                packet["source_context"]["audit_contract"]["required_functions"]}
    pending = {(image_of(state, item), item["address"]) for item in state["functions"]
               if item["proof_status"] != "proved"}
    return pending == required and not state.get("dependency_pending") and not state.get("main_dependency_unresolved")


def translate(name: str, proposal_path: Path) -> dict:
    from coverage_unverified import apply_unverified_sources

    root, project, state, selected = load_state(name)
    active = state.get("active_packet")
    if not active:
        raise ValueError("Run coverage next before translating")
    packet_path = (root / active).resolve()
    if not packet_path.is_relative_to(selected["directory"].resolve()) or not packet_path.is_file():
        raise ValueError("Invalid active translation packet")
    proposal = read_json(proposal_path.resolve())
    supported = {"schema", "mode", "functions", "declarations", "function_audits",
                 "sdk_audits", "integration_pending", "missing_stub_authorization",
                 "stub_replacements", "support_functions", "header_edits", "sdk_stubs", "caller_edits", "prototype_edits", "context_type_edits"}
    if set(proposal) - supported or proposal.get("integration_pending"):
        raise ValueError("Translation proposal contains unresolved integration or unsupported support edits")
    packet = read_json(packet_path)
    if (packet.get("identities") or {}).get("shared_source_identity"):
        checked_shared_snapshot(packet)
    previous_state = selected["state"].read_bytes()

    def finalize(receipt, prepared):
        try:
            identities = {(row["image"], row["address"]) for row in prepared}
            for item in state["functions"]:
                key = (image_of(state, item), item["address"])
                if key in identities:
                    item["translation_status"] = "unverified"
                    item["translation_receipt"] = packet_path.with_suffix(
                        ".translation.json").relative_to(root).as_posix()
            state["unverified_translated"] = sum(
                item.get("translation_status") == "unverified" for item in state["functions"])
            save_state(selected["state"], state)
            return {"status": "translated_unverified", "progress": progress_text(state["progress"]),
                    "unverified_translated": state["unverified_translated"],
                    "accepted_proofs_added": 0, "receipt": receipt}
        except BaseException:
            selected["state"].write_bytes(previous_state)
            raise

    return apply_unverified_sources(root, project, packet_path, packet, proposal, finalize)

def apply(name: str, proposal_path: Path) -> dict:
    root, _project, state, selected = load_state(name)
    packet_path = (root / state.get("active_packet", "")).resolve()
    if (not state.get("active_packet") or not packet_path.is_file() or
            not packet_path.is_relative_to(selected["directory"].resolve())):
        raise ValueError("Run coverage next before applying a proposal")
    packet = read_json(packet_path)
    guard_dependency_packet(state, packet)
    proposal = read_json(proposal_path.resolve())
    before = source_snapshot(root, SOURCE_ROOTS)
    if before["identity"] != packet.get("source_identity"):
        raise ValueError("Source changed after the coverage packet was created")
    if not proposal.get("edits") and not proposal.get("translations"):
        raise ValueError("Use coverage prove for an audit that needs no source edit")

    def validator(path: Path, after: dict, changed: set[str]) -> bool:
        return validate_audit(path, {**after, "root": str(root)}, changed)

    shared_options = {}
    if (packet.get("identities") or {}).get("shared_source_identity"):
        checked_shared_snapshot(packet)
    if proposal.get("sdk_translations") or proposal.get("system_translations") or any(isinstance(edit, dict) and str(edit.get("path", "")).startswith("@xport/") for edit in proposal.get("edits", [])):
        shared_root, shared_before = checked_shared_snapshot(packet)
        shared_options = {"shared_root": shared_root, "shared_before": shared_before}
    metadata_paths = (selected["state"], root / "status" / "translation-ledger.json")
    metadata_before = {path: path.read_bytes() for path in metadata_paths}
    state_before = copy.deepcopy(state)
    committed = []

    def finalize_and_accept(*_unused):
        refreshed = finalize_batch(root, packet_path, packet, proposal, full_build)
        receipt = read_json(packet_path.with_suffix(".audit.json"))
        changed = sorted(receipt["changed_files"])
        source_result = {"status": "patched", "origin": "supervisor_applied_model_code",
                         "allowed_files": changed, "changed_files": changed,
                         "shared_source_identity": receipt.get("shared_source_identity"),
                         "changed_shared_files": sorted(receipt.get("changed_shared_files") or []),
                         "memory_exceptions": refreshed}
        accepted = accept_proof(state, selected, packet, source_result, defer_release=True)
        accepted["summaries"] = proposal.get("summaries") or []
        committed.append(accepted)
        return refreshed

    try:
        full_build = is_final_batch(state, packet)
        if proposal.get("translations"):
            result = apply_translations(
                root, packet_path, packet, before, proposal, validator,
                finalize_and_accept,
                root / _project["paths"]["native_solution"], **shared_options)
        else:
            result = apply_proposal(root, SOURCE_ROOTS, packet_path, before, proposal,
                                    audit_validator=validator, **shared_options,
                                    post_apply=finalize_and_accept)
    except BaseException as failure:
        receipt_path = packet_path.with_suffix(".audit.json")
        committed_source = bool(committed) and not getattr(failure, "_coverage_source_rollback_complete", False)
        rollback_errors = []
        for path, raw in ({} if committed_source else metadata_before).items():
            try:
                path.write_bytes(raw)
            except BaseException as error:
                rollback_errors.append(error)
        if not committed_source:
            state.clear()
            state.update(state_before)
        subprocess.run([sys.executable, "-B", str(Path(__file__).with_name("source_index.py")),
                        "--rebuild", "--no-format"], cwd=root,
                       env=normalized_environment(), capture_output=True, text=True)
        if rollback_errors:
            raise RuntimeError("Coverage metadata rollback failed: " + "; ".join(str(error) for error in rollback_errors)) from failure
        raise
    if len(committed) != 1:
        raise ValueError("Coverage transaction did not commit proof exactly once")
    accepted = committed[0]
    if state["status"] == "release_required":
        finish_release(root, state, selected)
        accepted["coverage_status"] = state["status"]
    return accepted


def prove(name: str, proposal_path: Path) -> dict:
    root, _project, state, selected = load_state(name)
    packet_path = (root / state.get("active_packet", "")).resolve()
    if (not state.get("active_packet") or not packet_path.is_file() or
            not packet_path.is_relative_to(selected["directory"].resolve())):
        raise ValueError("Run coverage next before accepting proof")
    packet = read_json(packet_path)
    guard_dependency_packet(state, packet)
    proposal = read_json(proposal_path.resolve())
    if proposal.get("absent_device_authorization"):
        raise ValueError("Absent-device admission requires coverage apply and its supervisor transaction")
    if proposal.get("missing_stub_authorization"):
        raise ValueError("Missing stub admission requires coverage apply and its supervisor transaction")
    if proposal.get("game_prototype_updates") or proposal.get("runtime_constant_updates"):
        raise ValueError("Bound game declaration updates require coverage apply")
    if proposal.get("edits") not in ([], None):
        raise ValueError("Use coverage apply when the proposal contains edits")
    audits = proposal.get("function_audits")
    if proposal.get("schema") != 1 or not isinstance(audits, list):
        raise ValueError("Coverage proof requires schema=1 and function_audits")
    current = source_snapshot(root, SOURCE_ROOTS)
    if current["identity"] != packet.get("source_identity"):
        raise ValueError("Source changed after the coverage packet was created")
    shared = None
    if (packet.get("identities") or {}).get("shared_source_identity"):
        _shared_root, shared = checked_shared_snapshot(packet)
    receipt = {
        "schema": 1,
        "contract": "xport-lockstep-function-audit-v1",
        "failure_signature": packet["failure_signature"],
        "source_identity": current["identity"],
        "changed_files": [],
        "function_audits": audits,
        "sdk_audits": proposal.get("sdk_audits") or [],
    }
    if shared is not None:
        receipt["shared_source_identity"] = shared["identity"]
    write_receipt(packet_path.with_suffix(".audit.json"), receipt)
    ledger_path = root / "status" / "translation-ledger.json"
    ledger_before = ledger_path.read_bytes()
    try:
        validate_audit(packet_path, {**current, "root": str(root)}, set())
        update_ledger(root, packet_path, packet, proposal)
        if is_final_batch(state, packet):
            code_refresh(root)
        else:
            source_refresh(root)
    except Exception:
        ledger_path.write_bytes(ledger_before)
        packet_path.with_suffix(".audit.json").unlink(missing_ok=True)
        raise
    accepted = accept_proof(state, selected, packet, {"status": "proved"})
    accepted["summaries"] = proposal.get("summaries") or []
    return accepted


def accept_proof(state: dict, selected: dict[str, Path], packet: dict,
                 result: dict, defer_release: bool = False) -> dict:
    bindings = packet["source_context"]["audit_contract"]["required_functions"]
    identities = {(item["image"], item["address"]) for item in bindings}
    addresses = {item["address"] for item in bindings}
    receipt_path = selected["directory"].parents[2] / state["active_packet"].removesuffix(".json")
    receipt_path = receipt_path.with_suffix(".audit.json")
    receipt = read_json(receipt_path) if receipt_path.is_file() else {}
    stubs = receipt.get("missing_stub_admissions") or []
    retained = state.setdefault("missing_stubs", [])
    for stub in stubs:
        if stub not in retained:
            retained.append(stub)
    state["missing_stub_count"] = len(retained)
    delegated = state.setdefault("sdk_system_delegations", [])
    for admission in receipt.get("sdk_system_delegation_admissions") or []:
        if admission not in delegated:
            delegated.append(admission)
    state["sdk_system_delegation_count"] = len(delegated)
    frame_admissions = receipt.get("game_frame_delegation_admissions") or []
    retained_frames = state.setdefault("game_frame_delegations", [])
    for admission in frame_admissions:
        if admission not in retained_frames:
            retained_frames.append(admission)
    state["game_frame_delegation_count"] = len(retained_frames)
    absent = receipt.get("absent_device_admissions") or []
    retained_absent = retain_absent_admissions(state, absent)
    for item in state["functions"]:
        if (image_of(state, item), item["address"]) in identities:
            item["c_status"] = "decompiled"
            item["proof_status"] = "proved"
            item["proof_basis"] = "coverage_audit"
            if any(s["image"] == image_of(state, item) and s["address"] == item["address"] for s in frame_admissions):
                item["proof_basis"] = "user_authorized_guest_frame_scoped_audit"
                item["original_equivalence"] = False
            if any(s.get("image") == item.get("image", packet["source_context"]["image"]) and
                   s.get("address") == item["address"] for s in stubs):
                item["proof_basis"] = "user_authorized_missing_stub_scoped_audit"
                item["original_equivalence"] = False
            if any(s["image"] == image_of(state, item) and s["address"] == item["address"] for s in absent):
                item["proof_basis"] = "user_authorized_absent_device_scoped_audit"
                item["original_equivalence"] = False
            item["audit_receipt"] = state["active_packet"].removesuffix(".json") + ".audit.json"
    state.setdefault("batches", []).append({
        "packet": state["active_packet"],
        "audit_receipt": state["active_packet"].removesuffix(".json") + ".audit.json",
        "addresses": sorted(addresses),
        "changed_files": result.get("changed_files") or [],
    })
    state["active_packet"] = None
    all_proved = all(row["proof_status"] == "proved" for row in state["functions"]) and not state.get("dependency_pending") and not state.get("main_dependency_unresolved")
    state["status"] = "release_required" if all_proved else "running"
    save_state(selected["state"], state)
    if all_proved and not defer_release:
        state["release_build"] = release_build(Path(state.get("project", selected["directory"].parents[2])))
        state["status"] = "complete"
        save_state(selected["state"], state)
    value = progress(state)
    return {**result, "coverage_status": state["status"],
            "progress": progress_text(value), "missing_stub_count": state["missing_stub_count"],
            "missing_stubs": retained, "absent_device_count": state["absent_device_count"],
            "absent_devices": retained_absent, **value}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_subparsers(dest="action", required=True)
    for action in ("start", "status", "next"):
        command = actions.add_parser(action)
        command.add_argument("name")
        if action == "next":
            command.add_argument("--unverified", action="store_true")
            command.add_argument("--reissue", action="store_true",
                                 help="Replace an active packet without changing its original evidence")
            command.add_argument("--refresh-evidence", type=Path,
                                 help="Bind an accepted re-export with unchanged original bytes and zero accepted proofs")
            command.add_argument("--refresh-machine-audit", action="store_true",
                                 help="Correct legacy two-word listing parsing with unchanged originals and zero accepted proofs")
            command.add_argument("--sdk-dependencies", type=Path,
                                 help="Bind specific direct reviewed SDK dependencies from a project status JSON file")
            command.add_argument("--budget", type=int, default=256 * 1024,
                                 help="Maximum evidence bytes per function")
    for action in ("apply", "prove", "translate"):
        command = actions.add_parser(action)
        command.add_argument("name")
        command.add_argument("--proposal", type=Path, required=True)
    migration_command = actions.add_parser("migrate-images")
    migration_command.add_argument("name")
    migration_command.add_argument("--request", type=Path, required=True)
    inline_command = actions.add_parser("inline-dependencies")
    inline_command.add_argument("name")
    inline_command.add_argument("--review", type=Path, required=True)
    indirect_command = actions.add_parser("indirect-dependencies")
    indirect_command.add_argument("name")
    indirect_command.add_argument("--review", type=Path, required=True)
    args = parser.parse_args()
    if args.action == "indirect-dependencies":
        from coverage_indirect import admit
        result = admit(args.name, args.review)
    elif args.action == "inline-dependencies":
        result = admit_inline_dependencies(args.name, args.review)
    elif args.action == "migrate-images":
        result = migrate_images(args.name, args.request)
    elif args.action == "start":
        result = start(args.name)
    elif args.action == "status":
        result = status(args.name)
    elif args.action == "next":
        result = next_packet(args.name, args.budget, args.reissue, args.refresh_evidence,
                             args.refresh_machine_audit, args.sdk_dependencies, args.unverified)
    elif args.action == "translate":
        result = translate(args.name, args.proposal)
    elif args.action == "apply":
        result = apply(args.name, args.proposal)
    else:
        result = prove(args.name, args.proposal)
    print(json.dumps(result))


if __name__ == "__main__":
    main()

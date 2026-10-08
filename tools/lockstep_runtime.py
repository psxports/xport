#!/usr/bin/env python3
"""Project binding for the shared lockstep converge supervisor"""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys

from lockstep_record import load_record
from lockstep_supervisor import LockstepSupervisor
from source_index import index_needs_refresh, source_inventory, source_relative, sha


def ensure_source_index(root: Path, config: dict, name: str) -> None:
    database = (root / config['paths']['analysis_database']).resolve()
    ledger = root / 'status' / 'translation-ledger.json'
    if not index_needs_refresh(root, database, ledger):
        return
    before = {source_relative(root, path): sha(path.read_bytes())
              for path in source_inventory(root)}
    environment = os.environ.copy()
    environment['XPORT_PROJECT'] = str(root)
    command = [sys.executable, '-B', str(Path(__file__).with_name('source_index.py')),
               '--rebuild', '--no-format']
    result = subprocess.run(command, cwd=root, env=environment,
                            capture_output=True, text=True)
    log = root / 'status' / 'lockstep-converge' / name / 'source-index-startup.log'
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text(result.stdout + result.stderr, encoding='utf-8')
    if result.returncode:
        raise RuntimeError('source_index failed before converge; see ' + str(log))
    after = {source_relative(root, path): sha(path.read_bytes())
             for path in source_inventory(root)}
    if before != after or index_needs_refresh(root, database, ledger):
        raise RuntimeError('source_index did not preserve and refresh source before converge')


def control(root: Path, config: dict, name: str, record_path: Path, wait: float,
            telemetry: str, reprobe: bool = False) -> dict:
    lockstep = config.get("lockstep") or {}
    if lockstep.get("schema") != 1 or lockstep.get("enabled") is not True:
        raise ValueError("LOCKSTEP_PROJECT_NOT_MIGRATED")
    if lockstep.get('repair_executor') == 'codex_cli':
        raise ValueError('CODEX_CLI_REPAIR_REMOVED: use the manual proposal inbox')
    record = load_record(record_path)
    ensure_source_index(root, config, name)
    from lockstep_project_runtime import ProjectLockstepHooks
    hooks = ProjectLockstepHooks(root, config, name, record, telemetry=telemetry,
                                 record_name=record_path.parent.name)
    directory = root / "status" / "lockstep-converge" / name
    supervisor = LockstepSupervisor(
        root, directory, record, hooks,
        source_roots=tuple(Path(value) for value in lockstep.get("source_roots", ["src"])),
        max_repairs=(int(lockstep["max_repairs"]) if lockstep.get("max_repairs") is not None else None),
        max_steps=int(lockstep.get("max_events", 1_000_000)),
        reprobe=reprobe,
    )
    return supervisor.run_interactive()

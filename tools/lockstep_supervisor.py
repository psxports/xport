#!/usr/bin/env python3
"""Durable rebuild, replay, diagnose and repair loop for lockstep converge"""

from __future__ import annotations

import hashlib
import inspect
import itertools
import json
import time
import uuid
from pathlib import Path

from lockstep_packet import boundary_evidence, case_key, deferred_evidence, prearmed_evidence
from lockstep_progress import SupervisorProgress
from lockstep_record import verify_record_inputs
from trace_worker import write_receipt


SUPERVISOR_CONTRACT = "xport-lockstep-supervisor-v1"
TRANSITIONS = {
    "STARTING": {"VALIDATING", "FAILED"},
    "VALIDATING": {"LAUNCHING_ORIGINAL", "ATTENTION_REQUIRED", "FAILED"},
    "LAUNCHING_ORIGINAL": {"LAUNCHING_NATIVE", "VALIDATING", "FAILED"},
    "LAUNCHING_NATIVE": {"ATTACHING_DEBUGGERS", "VALIDATING", "FAILED"},
    "ATTACHING_DEBUGGERS": {"REPLAYING", "VALIDATING", "FAILED"},
    "REPLAYING": {"ATTENTION_REQUIRED", "LAUNCHING_ORIGINAL", "VALIDATING", "MATCH", "FAILED"},
    "ATTENTION_REQUIRED": {"VERIFYING_REPAIR", "VALIDATING", "FAILED"},
    "VERIFYING_REPAIR": {"LAUNCHING_ORIGINAL", "VALIDATING", "FAILED"},
    "MATCH": {"FINALIZING", "FAILED"},
    "FINALIZING": {"COMPLETE", "FAILED"},
    "COMPLETE": set(),
    "FAILED": {"VALIDATING"},
}

INTERRUPTIBLE_STATES = {
    "LAUNCHING_ORIGINAL",
    "LAUNCHING_NATIVE",
    "ATTACHING_DEBUGGERS",
    "REPLAYING",
    "VERIFYING_REPAIR",
    "MATCH",
    "FINALIZING",
}


class LockstepSupervisorError(RuntimeError):
    pass


def file_digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def source_snapshot(root: Path, source_roots: tuple[Path, ...]) -> dict:
    files = {}
    resolved_root = root.resolve()
    for source_root in source_roots:
        directory = (root / source_root).resolve()
        if not directory.is_relative_to(resolved_root) or not directory.is_dir():
            raise LockstepSupervisorError(f"Invalid source ownership root: {source_root}")
        for path in sorted(directory.rglob("*")):
            if path.is_file() and path.suffix.lower() in {".c", ".h", ".cpp", ".hpp"}:
                files[path.relative_to(resolved_root).as_posix()] = file_digest(path)
    encoded = json.dumps(files, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return {"identity": hashlib.sha256(encoded).hexdigest(), "files": files}


def shared_source_identity(root: Path, config: dict) -> str:
    toolset = Path(config["toolset"])
    if not toolset.is_absolute():
        toolset = root / toolset
    return source_snapshot(toolset.resolve().parent, (Path("src"),))["identity"]


def source_metadata(root: Path, source_roots: tuple[Path, ...]) -> dict:
    files = {}
    resolved_root = root.resolve()
    for source_root in source_roots:
        directory = (root / source_root).resolve()
        if not directory.is_relative_to(resolved_root) or not directory.is_dir():
            raise LockstepSupervisorError(f"Invalid source ownership root: {source_root}")
        for path in directory.rglob("*"):
            if path.is_file() and path.suffix.lower() in {".c", ".h", ".cpp", ".hpp"}:
                stat = path.stat()
                files[path.relative_to(resolved_root).as_posix()] = (stat.st_size, stat.st_mtime_ns)
    return files


def changed_sources(before: dict, after: dict) -> set[str]:
    names = set(before["files"]) | set(after["files"])
    return {name for name in names if before["files"].get(name) != after["files"].get(name)}


def divergence_frame(result: dict) -> int:
    for event in (result.get("original"), result.get("native"),
                  (result.get("last_match") or {}).get("original")):
        if isinstance(event, dict) and isinstance(event.get("frame"), int):
            return max(0, event["frame"])
    return 0


def replay_target_frame(result: dict) -> int:
    return max(0, divergence_frame(result) - 1)


def semantic_attempt_case(attempt: dict, record_id: str) -> str | None:
    result = attempt.get("result") or {}
    original = result.get("original") or {}
    native = result.get("native") or {}
    if (result.get("status") in {
            "memory_divergence", "control_flow_divergence", "runtime_divergence"} and
            all(original.get(key) is not None for key in
                ("kind", "frame", "image", "function", "target")) and
            all(native.get(key) is not None for key in
                ("kind", "frame", "image", "function", "target"))):
        return case_key(result, {"record_id": record_id})
    return attempt.get("case_key")


def bind_diagnostic_request(request: dict, result: dict) -> dict:
    if request.get("until_divergence") is not True:
        return request
    event = result.get("original") or {}
    paired_memory = request.get("kind") == "paired_memory"
    if (result.get("status") not in {
            "memory_divergence", "control_flow_divergence", "runtime_divergence"} or
            not isinstance(event.get("kind"), str) or
            type(event.get("frame")) is not int or
            (not paired_memory and type(event.get("event_sequence")) is not int)):
        raise ValueError("Current divergence has no exact original event boundary")
    if paired_memory and (result.get("status") != "memory_divergence" or
                          request.get("expected_difference") is not None):
        raise ValueError("Paired memory probe requires a current memory divergence")
    target = replay_target_frame(result)
    if (request.get("until") or
            ("target_frame" in request and int(request["target_frame"]) != target)):
        raise ValueError("Diagnostic divergence boundary conflicts with explicit request")
    bound = dict(request)
    bound.pop("until_divergence")
    bound["target_frame"] = target
    bound.setdefault("anchor", {"kind": "VBLANK"})
    keys = (("kind", "frame", "function", "callsite") if paired_memory else
            ("kind", "frame", "function", "event_sequence"))
    bound["until"] = {key: event.get(key) for key in keys}
    if paired_memory:
        difference = result.get("comparison") or {}
        bound["expected_difference"] = {key: difference.get(key) for key in
                                        ("region", "offset", "original", "candidate")}
    return bound


def watch_target_from_result(result: dict) -> dict | None:
    difference = result.get("comparison") or {}
    region = difference.get("region")
    offset = difference.get("offset")
    limits = {"dram": 0x200000, "scratchpad": 0x400}
    if (result.get("status") != "memory_divergence" or region not in limits or
            type(offset) is not int or not 0 <= offset < limits[region]):
        return None
    return {"region": region, "offset": offset}


def same_memory_divergence(first: dict, second: dict) -> bool:
    if first.get("status") != "memory_divergence" or second.get("status") != "memory_divergence":
        return False
    keys = ("region", "offset", "original", "candidate")
    left = first.get("comparison") or {}
    right = second.get("comparison") or {}
    if any(left.get(key) != right.get(key) for key in keys):
        return False
    event_keys = ("frame", "kind", "function", "callsite", "invocation")
    left_event = first.get("original") or {}
    right_event = second.get("original") or {}
    return all(left_event.get(key) == right_event.get(key) for key in event_keys)


class LockstepSupervisor:
    def __init__(self, root: Path, directory: Path, record: dict, hooks, *,
                 source_roots: tuple[Path, ...] = (Path("src"),), max_repairs: int | None = None,
                 max_steps: int = 1_000_000, reprobe: bool = False):
        self.root = root.resolve()
        self.directory = directory.resolve()
        self.directory.mkdir(parents=True, exist_ok=True)
        self.record = record
        self.scan_target_frame = max(0, len(record["frames"]) - 1)
        self.hooks = hooks
        self.source_roots = source_roots
        self.max_repairs = max_repairs
        self.max_steps = max_steps
        self.reprobe = reprobe
        self._retained_checkpoint = None
        self.path = self.directory / "state.json"
        self.progress = SupervisorProgress(getattr(self.hooks, "progress_window", False))
        self.progress.receipt_path = self.directory / "progress-process.json"
        if self.path.exists():
            self.state = json.loads(self.path.read_text(encoding="utf-8"))
            if self.state.get("contract") != SUPERVISOR_CONTRACT:
                raise LockstepSupervisorError("Supervisor state contract mismatch")
            if self.state.get("record_id", record["record_id"]) != record["record_id"]:
                raise LockstepSupervisorError("Supervisor state belongs to another record")
        else:
            self.state = {
                "schema": 1, "contract": SUPERVISOR_CONTRACT, "status": "STARTING",
                "record_id": record["record_id"],
                "revision": 0, "transitions": [], "attempts": [], "model_signatures": [],
                "refined_cases": [],
                "accepted_repairs": [], "telemetry": {
                    "duckstation_launches": 0, "native_launches": 0, "model_calls": 0,
                    "attention_packets": 0,
                    "model_accounting_complete": True, "model_invocations_reported": 0,
                    "debug_builds": 0, "python_memory_bytes": 0, "launch_reasons": {},
                    "phase_seconds": {}, "phase_counts": {},
                },
            }
            self._save()
        telemetry = self.state["telemetry"]
        if self.state.pop("live_checkpoint", None) is not None:
            self._save()
        self.state.setdefault("refined_cases", [])
        telemetry.setdefault("attention_packets", telemetry.get("model_calls", 0))
        telemetry.setdefault("phase_seconds", {})
        telemetry.setdefault("phase_counts", {})
        if "launch_reasons" not in telemetry:
            telemetry["launch_reasons"] = {}
            for process in ("duckstation", "native"):
                count = telemetry.get(process + "_launches", 0)
                if count:
                    telemetry["launch_reasons"][process + ":historical_unclassified"] = count
            self._save()
        self.maximum_match = self._recover_maximum_match()
        if "progress_process" in self.state:
            self.state.pop("progress_process", None)
            self._save()

    def _recover_maximum_match(self) -> int:
        maximum = 0
        for attempt in self.state["attempts"]:
            target_frame = int(attempt.get("target_frame", 0))
            result = attempt.get("result") or {}
            if result.get("status") == "match_complete" and not attempt.get("checkpoint_pair_resumed"):
                return len(self.record["frames"])
            if attempt.get("checkpoint_pair_resumed"):
                continue
            confirmed = attempt.get("confirmed_match_frames")
            if type(confirmed) is int and confirmed >= 0:
                maximum = max(maximum, confirmed)
            checkpoint = attempt.get("checkpoint_replay") or {}
            checkpoint_frame = checkpoint.get("from_frame")
            if type(checkpoint_frame) is int and checkpoint_frame >= 0:
                maximum = max(maximum, checkpoint_frame + 1)
            event = (result.get("last_match") or {}).get("original") or {}
            if event.get("kind") == "VBLANK" and type(event.get("frame")) is int:
                maximum = max(maximum, event["frame"] + 1)
            if target_frame and target_frame < self.scan_target_frame:
                result_modes = {
                    event.get("mode") for event in (result.get("original"), result.get("native"))
                    if isinstance(event, dict) and event.get("mode")
                }
                if "DETAILED" in result_modes:
                    maximum = max(maximum, target_frame + 1)
        return min(maximum, len(self.record["frames"]))

    def _write_progress(self, *, active: bool, current: int | None = None,
                        mode: str | None = None, target_frame: int | None = None,
                        confirmed_match: bool = False) -> None:
        frame_count = len(self.record["frames"])
        current_value = int(self.progress.snapshot.get("current", 0) if current is None else current)
        if confirmed_match:
            self.maximum_match = max(self.maximum_match, current_value)
        self.progress.update({
            "schema": 1, "contract": "xport-lockstep-progress-v1", "active": active,
            "frame_count": frame_count, "maximum_match": min(self.maximum_match, frame_count),
            "current": min(current_value, frame_count), "mode": mode,
            "target_frame": target_frame,
        })

    def _start_progress_window(self) -> None:
        self.progress.start()

    def _stop_progress_window(self) -> None:
        self._write_progress(active=False)
        self.progress.close()

    def _count_launch(self, process: str, reason: str) -> None:
        telemetry = self.state["telemetry"]
        telemetry[process + "_launches"] += 1
        reasons = telemetry["launch_reasons"]
        key = process + ":" + reason
        reasons[key] = reasons.get(key, 0) + 1
        self._save()

    def _save(self) -> None:
        write_receipt(self.path, self.state)

    def _timed(self, phase: str, operation, *args, **kwargs):
        started = time.monotonic()
        try:
            return operation(*args, **kwargs)
        finally:
            elapsed = time.monotonic() - started
            telemetry = self.state["telemetry"]
            seconds = telemetry["phase_seconds"]
            counts = telemetry["phase_counts"]
            seconds[phase] = seconds.get(phase, 0.0) + elapsed
            counts[phase] = counts.get(phase, 0) + 1

    def _implementation_identity(self):
        identity = getattr(self.hooks, "implementation_identity", None)
        return identity() if identity else None

    def _transition(self, status: str, **fields) -> None:
        current = self.state["status"]
        if status != current and status not in TRANSITIONS[current]:
            raise LockstepSupervisorError(f"Illegal lockstep transition: {current} -> {status}")
        visible = {"status": status, **fields}
        if any(self.state.get(key) != value for key, value in visible.items()):
            self.state.update(visible)
            self.state["revision"] += 1
            self.state["transitions"].append({"revision": self.state["revision"],
                                               "at": time.time(), **visible})
        self._save()

    @staticmethod
    def _close(value) -> None:
        if value is not None:
            try:
                value.close()
            except Exception:
                pass

    def _discard_retained_checkpoint(self) -> None:
        retained = self._retained_checkpoint
        self._retained_checkpoint = None
        if self.state.pop("live_checkpoint", None) is not None:
            self._save()
        if retained is not None:
            self._close(retained["session"])
            self._close(retained["original"])

    def _discard_all_checkpoints(self) -> None:
        self._discard_retained_checkpoint()
        if self.state.pop("durable_checkpoint", None) is not None:
            clear = getattr(self.hooks, "clear_restart_checkpoint", None)
            if clear is not None:
                clear()
            self._save()

    def _prepare_durable_resume(self, target_frame: int) -> dict | None:
        saved = self.state.get("durable_checkpoint") or {}
        prepare = getattr(self.hooks, "prepare_restart_checkpoint", None)
        frame = saved.get("frame")
        if (prepare is None or type(frame) is not int or
                not 0 <= frame <= target_frame):
            return None
        path = self.directory / "last-match.xpck"
        try:
            if not path.is_file() or file_digest(path) != saved.get("sha256"):
                raise LockstepSupervisorError("Durable checkpoint identity changed")
            if prepare() != frame:
                raise LockstepSupervisorError("Durable checkpoint frame changed")
            return {"frame": frame, "run_id": uuid.uuid4().hex, "durable": True}
        except Exception as error:
            self.state["durable_checkpoint_error"] = str(error)
            self.state.pop("durable_checkpoint", None)
            clear = getattr(self.hooks, "clear_restart_checkpoint", None)
            if clear is not None:
                clear()
            self._save()
            return None

    def _prepare_durable_diagnostic(self, target_frame: int) -> dict | None:
        saved = self.state.get("durable_checkpoint") or {}
        if self._retained_checkpoint is not None or saved.get("frame") != target_frame:
            return None
        return self._prepare_durable_resume(target_frame)

    def _launch(self, run_id: str):
        retained = self._retained_checkpoint
        if retained is not None and retained["run_id"] == run_id:
            self._retained_checkpoint = None
            self.state.pop("live_checkpoint", None)
            native = scheduler = None
            try:
                if file_digest(retained["path"]) != retained["sha256"]:
                    raise LockstepSupervisorError("Native checkpoint changed before resume")
                self._transition("LAUNCHING_ORIGINAL", run_id=run_id,
                                 checkpoint_resume=retained["frame"])
                self._transition("LAUNCHING_NATIVE", run_id=run_id)
                native, scheduler = self._timed(
                    "checkpoint_pair_resume", self.hooks.resume_checkpoint,
                    retained["session"].original_adapter, run_id, self.record,
                    retained["frame"], retained["path"])
                self._count_launch("native", "checkpoint_pair_resume")
                self._transition("ATTACHING_DEBUGGERS", run_id=run_id)
                return retained["original"], native, scheduler
            except Exception:
                self._close(scheduler)
                self._close(native)
                self._close(retained["session"])
                self._close(retained["original"])
                raise
        self._transition("LAUNCHING_ORIGINAL", run_id=run_id)
        original = self._timed("launch_original", self.hooks.launch_original, run_id, self.record)
        self._count_launch("duckstation", "converge_attempt")
        try:
            self._transition("LAUNCHING_NATIVE", run_id=run_id)
            native = self._timed("launch_native", self.hooks.launch_native, run_id, self.record)
            self._count_launch("native", "converge_attempt")
        except Exception:
            self._close(original)
            raise
        try:
            self._transition("ATTACHING_DEBUGGERS", run_id=run_id)
            scheduler = self._timed("attach", self.hooks.attach, original, native, run_id, self.record)
            return original, native, scheduler
        except Exception:
            self._close(native)
            self._close(original)
            raise

    def _verify_repair_audit(self, packet_path: str | Path | None, after: dict,
                             changed: set[str] | None, *, missing_allowed: bool = False) -> bool:
        validate = getattr(self.hooks, "validate_repair_audit", None)
        if validate is None:
            return True
        ready = validate(Path(packet_path) if packet_path else None, after, changed)
        if ready is False and not missing_allowed:
            raise LockstepSupervisorError("Complete function audit receipt is required")
        return ready is not False

    def _verify_repair_scope(self, before: dict, repair: dict,
                             packet_path: str | Path | None = None) -> dict:
        after = source_snapshot(self.root, self.source_roots)
        changed = changed_sources(before, after)
        allowed = set(repair.get("allowed_files") or [])
        declared = set(repair.get("changed_files") or [])
        changed_shared = set(repair.get('changed_shared_files') or [])
        if not changed and not changed_shared:
            raise LockstepSupervisorError("Repair changed no owned source")
        if changed != declared or not changed <= allowed:
            raise LockstepSupervisorError(
                "Foreign source change: observed=" + ",".join(sorted(changed)) +
                " declared=" + ",".join(sorted(declared))
            )
        if changed_shared:
            current_shared = getattr(self.hooks, 'shared_source_identity', lambda: None)()
            if current_shared != repair.get('shared_source_identity'):
                raise LockstepSupervisorError('Foreign shared source change during repair')
        if repair.get("audit_verified") is not True:
            self._verify_repair_audit(packet_path, after, changed)
        return after

    def _apply_repair_proposal(self, before: dict, packet_path: Path,
                               proposal: dict) -> dict:
        from lockstep_repair_proposal import apply_proposal
        from lockstep_repair_exceptions import (activate_repair_ranges,
                                                validate_repair_ranges)

        validator = getattr(self.hooks, 'validate_repair_audit', None)
        if validator is None:
            raise ValueError('Complete function audit validator is unavailable')
        if source_snapshot(self.root, self.source_roots)['identity'] != before['identity']:
            raise LockstepSupervisorError('Source changed outside supervisor repair')
        packet = json.loads(packet_path.read_text(encoding='utf-8'))
        repair_ranges = proposal.get('memory_exceptions')
        if repair_ranges is not None:
            validate_repair_ranges(
                repair_ranges, packet, packet.get('compiled_memory_exceptions') or {})
        exception_path = self.directory / 'repair-memory-exceptions.json'
        shared_root_method = getattr(self.hooks, 'shared_source_root', None)
        shared_root = shared_root_method() if shared_root_method is not None else None
        shared_before = (source_snapshot(shared_root, (Path('src'),))
                         if shared_root is not None else None)
        applied = apply_proposal(self.root, self.source_roots, packet_path,
                                 before, proposal, audit_validator=validator,
                                 build_validator=self._preflight_repair_build,
                                 shared_root=shared_root, shared_before=shared_before,
                                 post_apply=(lambda after: activate_repair_ranges(
                                     exception_path, repair_ranges, packet, after['identity']))
                                 if repair_ranges is not None else None)
        after = source_snapshot(self.root, self.source_roots)
        write_receipt(self.directory / 'repair-proposal.result.json', {
            'status': 'applied', 'source_identity': after['identity'],
            'changed_files': applied['changed_files'],
            'shared_source_identity': applied['shared_source_identity'],
            'changed_shared_files': applied['changed_shared_files'],
            'memory_exceptions': applied.get('memory_exceptions'),
        })
        refresh = getattr(self.hooks, 'refresh_after_repair', None)
        if refresh is not None:
            self._timed('source_refresh', refresh)
        return {**applied, 'audit_verified': True}

    def _preflight_repair_build(self, after: dict) -> dict:
        build = None
        try:
            build = self._timed('debug_build_preflight', self.hooks.build_debug,
                                after['identity'])
            return build
        finally:
            telemetry = self.state['telemetry']
            if build is not None:
                key = 'debug_build_reuses' if build.get('reused', False) else 'debug_builds'
                telemetry[key] = telemetry.get(key, 0) + 1
            elif getattr(self.hooks, '_last_debug_build_was_invoked', False):
                telemetry['debug_builds'] = telemetry.get('debug_builds', 0) + 1
            self._save()

    def _wait_for_external_repair(self, before: dict,
                                  packet_path: str | Path | None = None) -> dict:
        if packet_path is not None and self.state["attempts"][-1].get(
                "repair_request_invocations", 0) == 0:
            from lockstep_packet import require_writer_library_audit, scope_legacy_audit_packet
            prior = json.loads(Path(packet_path).read_text(encoding="utf-8"))
            scoped = scope_legacy_audit_packet(prior)
            upgraded = require_writer_library_audit(scoped or prior)
            migrated = upgraded or scoped
            if migrated is not None:
                if source_snapshot(self.root, self.source_roots)["identity"] != before["identity"]:
                    raise LockstepSupervisorError("Source changed before audit scope migration")
                attempt = self.state["attempts"][-1]
                suffix = "audit" if upgraded is not None else "scope"
                scoped_path = self.directory / f"attention-{attempt['attempt']:04d}-{suffix}.json"
                sequence = 1
                while scoped_path.is_file():
                    if json.loads(scoped_path.read_text(encoding="utf-8")) == migrated:
                        break
                    scoped_path = self.directory / f"attention-{attempt['attempt']:04d}-{suffix}-{sequence}.json"
                    sequence += 1
                if not scoped_path.is_file():
                    write_receipt(scoped_path, migrated)
                attempt.setdefault("causal_chain", []).append(str(packet_path))
                attempt["packet"] = str(scoped_path)
                attempt["failure_signature"] = migrated["failure_signature"]
                packet_path = scoped_path
                self._transition("ATTENTION_REQUIRED", reason=(
                    "writer_library_audit_transition" if upgraded is not None else
                    "owner_audit_scope_transition"),
                                 packet=str(scoped_path),
                                 failure_signature=migrated["failure_signature"])
        self._write_progress(
            active=True, current=0, mode="WAITING_REPAIR",
            target_frame=replay_target_frame(
                (self.state["attempts"][-1].get("result")
                 if self.state["attempts"] else {}) or {}),
        )
        metadata = source_metadata(self.root, self.source_roots)
        shared_root_method = getattr(self.hooks, 'shared_source_root', None)
        shared_root = shared_root_method() if shared_root_method is not None else None
        shared_before = (source_snapshot(shared_root, (Path('src'),))
                         if shared_root is not None else None)
        next_full_check = time.monotonic() + 5.0
        while True:
            causal_path = self.directory / "causal-request.json"
            if causal_path.is_file():
                result_path = self.directory / "causal-request.result.json"
                try:
                    from lockstep_causal_gte import read_causal_request
                    if source_snapshot(self.root, self.source_roots)["identity"] != before["identity"]:
                        raise ValueError("Source changed before causal diagnosis")
                    prior = json.loads(Path(packet_path).read_text(encoding="utf-8"))
                    request = json.loads(causal_path.read_text(encoding="utf-8"))
                    attempt = self.state["attempts"][-1]
                    if request.get("kind") == "audit_cluster":
                        from lockstep_causal_cluster import read_cluster_request
                        packet = read_cluster_request(
                            self.root, self.directory, request, prior, self.source_roots)
                        suffix = "cluster"
                    elif request.get("kind") == "trig_table":
                        from lockstep_causal_trig import read_trig_request
                        packet = read_trig_request(self.root, self.directory, request, prior)
                        suffix = "trig"
                    elif request.get("kind") == "writer_refinement":
                        from lockstep_refinement_adopt import read_refined_request
                        packet = read_refined_request(
                            self.root, self.directory, request, prior, attempt["result"],
                            before["identity"], self.source_roots)
                        suffix = "refined"
                    elif request.get("kind") == "library_reference":
                        from lockstep_library_reference import read_reference_request
                        packet = read_reference_request(
                            self.root, request, prior, self.source_roots)
                        suffix = "reference"
                    elif request.get("kind") == "operand_provenance":
                        from lockstep_operand_provenance import read_operand_request
                        packet = read_operand_request(
                            self.root, self.directory, request, prior, self.source_roots)
                        suffix = "operand"
                    elif request.get("kind") == "paired_entry":
                        from lockstep_paired_entry import read_paired_entry_request
                        packet = read_paired_entry_request(
                            self.root, self.directory, request, prior, self.source_roots)
                        suffix = "entry"
                    elif request.get("kind") == "writer_sequence":
                        from lockstep_paired_entry import read_writer_sequence_request
                        packet = read_writer_sequence_request(
                            self.root, self.directory, request, prior, self.source_roots)
                        suffix = "writer"
                    elif request.get("kind") == "gte_entry":
                        from lockstep_gte_adopt import read_gte_entry_request
                        packet = read_gte_entry_request(self.directory, request, prior)
                        suffix = "gte-entry"
                    else:
                        packet = read_causal_request(
                            self.directory, request, prior, attempt["result"])
                        suffix = "causal"
                    stem = f"attention-{attempt['attempt']:04d}-{suffix}"
                    if suffix == "refined":
                        from lockstep_refinement_adopt import (record_external_refinement,
                                                               validate_external_refinement)
                        validate_external_refinement(self.state, packet)
                    new_path = self.directory / f"{stem}.json"
                    sequence = 1
                    while new_path.is_file():
                        existing = json.loads(new_path.read_text(encoding="utf-8"))
                        if existing == packet:
                            break
                        new_path = self.directory / f"{stem}-{sequence}.json"
                        sequence += 1
                    if not new_path.is_file():
                        write_receipt(new_path, packet)
                    if suffix == "refined":
                        record_external_refinement(self.state, packet)
                    attempt.setdefault("causal_chain", []).append(str(packet_path))
                    attempt["packet"] = str(new_path)
                    attempt["failure_signature"] = packet["failure_signature"]
                    attempt["repair_request_invocations"] = 0
                    attempt["model_request_in_flight"] = False
                    attempt.pop("repair_attention", None)
                    packet_path = new_path
                    self._transition("ATTENTION_REQUIRED", reason=(
                        "audit_cluster_transition" if suffix == "cluster" else
                        "trig_table_transition" if suffix == "trig" else
                        "writer_refinement_transition" if suffix == "refined" else
                        "library_reference_transition" if suffix == "reference" else
                        "operand_provenance_transition" if suffix == "operand" else
                        "paired_entry_transition" if suffix == "entry" else
                        "writer_sequence_transition" if suffix == "writer" else
                        "gte_entry_transition" if suffix == "gte-entry" else
                        "gte_causal_transition"),
                                     packet=str(new_path),
                                     failure_signature=packet["failure_signature"])
                    write_receipt(result_path, {"status": "adopted", "packet": str(new_path),
                                                "function": packet["diagnostic_owner"]["function"]})
                    if (suffix == 'reference' and packet['failure_signature'] not in
                            self.state['model_signatures']):
                        repair = self._request_repair(
                            packet, new_path, before, attempt,
                            packet['failure_signature'], wait_on_attention=False)
                        if repair.get('status') == 'patched':
                            return repair
                except (OSError, ValueError, KeyError, TypeError, RuntimeError) as error:
                    write_receipt(result_path, {"status": "rejected", "error": str(error)})
                causal_path.unlink(missing_ok=True)
            request_path = self.directory / "diagnostic-request.json"
            if request_path.is_file():
                result_path = self.directory / "diagnostic-result.json"
                used_checkpoint = False
                durable_diagnostic = None
                request = {}
                try:
                    request = json.loads(request_path.read_text(encoding="utf-8"))
                    if not isinstance(request, dict):
                        raise ValueError("Diagnostic request is not an object")
                    request = bind_diagnostic_request(
                        request, self.state["attempts"][-1].get("result") or {})
                    kind = request.get("kind", "instruction")
                    if kind not in ("instruction", "paired_memory"):
                        raise ValueError("Unknown diagnostic probe kind")
                    attempt = self.state["attempts"][-1]
                    current_source = source_snapshot(self.root, self.source_roots)
                    attempt["build"] = self._preflight_repair_build(current_source)
                    probe = getattr(self.hooks, "paired_memory_probe" if kind == "paired_memory"
                                    else "instruction_probe")
                    checkpoint = self._retained_checkpoint
                    target_frame = int(request.get("target_frame", 0))
                    if checkpoint is not None and "checkpoint" in inspect.signature(probe).parameters:
                        if file_digest(checkpoint["path"]) != checkpoint["sha256"]:
                            raise LockstepSupervisorError("Native checkpoint changed before diagnostic probe")
                        if target_frame < checkpoint["frame"]:
                            raise LockstepSupervisorError("Diagnostic target precedes retained checkpoint")
                        used_checkpoint = True
                    elif checkpoint is None:
                        durable_diagnostic = self._prepare_durable_diagnostic(target_frame)
                    self._write_progress(active=True, current=0, mode="DIAGNOSTIC",
                                         target_frame=target_frame)
                    result = self._timed(kind + "_probe", probe,
                        request, self.record,
                        progress=lambda frame: self._write_progress(
                            active=True, current=frame, mode="DIAGNOSTIC",
                            target_frame=target_frame),
                        **({"checkpoint": checkpoint} if used_checkpoint else {}))
                    if used_checkpoint:
                        checkpoint["sha256"] = file_digest(checkpoint["path"])
                        result["checkpoint_pair_resumed"] = checkpoint["frame"]
                    elif durable_diagnostic is not None:
                        result["checkpoint_pair_resumed"] = durable_diagnostic["frame"]
                        result["durable_checkpoint_pair_resumed"] = True
                except Exception as error:
                    result = {"schema": 1, "complete": False, "error": str(error)}
                    if used_checkpoint:
                        self._discard_retained_checkpoint()
                finally:
                    if durable_diagnostic is not None:
                        clear = getattr(self.hooks, "clear_restart_checkpoint", None)
                        if clear is not None:
                            clear()
                temporary = result_path.with_suffix(".tmp")
                temporary.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
                temporary.replace(result_path)
                request_path.unlink(missing_ok=True)
                if request.get("auto_operand_provenance") is not None:
                    auto_path = self.directory / "diagnostic-auto-causal.result.json"
                    try:
                        from lockstep_operand_provenance import (derive_operand_request,
                                                                 read_operand_request)
                        helper = request["auto_operand_provenance"]
                        if packet_path is None or not isinstance(helper, str):
                            raise ValueError("Automatic operand provenance needs a helper address")
                        prior = json.loads(Path(packet_path).read_text(encoding="utf-8"))
                        raw = result_path.read_bytes()
                        name = "diagnostic-operand-" + hashlib.sha256(raw).hexdigest()[:16] + ".json"
                        preserved = self.directory / name
                        if preserved.is_file():
                            if preserved.read_bytes() != raw:
                                raise ValueError("Immutable operand diagnostic changed")
                        else:
                            temporary = preserved.with_suffix(".tmp")
                            temporary.write_bytes(raw)
                            temporary.replace(preserved)
                        derived = derive_operand_request(self.directory, prior, name, helper)
                        read_operand_request(self.root, self.directory, derived, prior,
                                             self.source_roots)
                        if causal_path.is_file():
                            raise ValueError("Another causal request is already pending")
                        write_receipt(causal_path, derived)
                        write_receipt(auto_path, {"status": "queued", "diagnostic_file": name,
                                                  "request": str(causal_path)})
                    except (OSError, ValueError, KeyError, TypeError, RuntimeError) as error:
                        write_receipt(auto_path, {"status": "rejected", "error": str(error)})
                self._write_progress(
                    active=True, current=0, mode="WAITING_REPAIR",
                    target_frame=replay_target_frame(
                        (self.state["attempts"][-1].get("result")
                         if self.state["attempts"] else {}) or {}),
                )
            proposal_path = self.directory / 'repair-proposal.json'
            if proposal_path.is_file():
                result_path = self.directory / 'repair-proposal.result.json'
                try:
                    if packet_path is None or proposal_path.stat().st_size > 1024 * 1024:
                        raise ValueError('Repair proposal is missing a packet or exceeds 1 MiB')
                    proposal = json.loads(proposal_path.read_text(encoding='utf-8'))
                    applied = self._apply_repair_proposal(
                        before, Path(packet_path), proposal)
                except (ValueError, TypeError, KeyError, RuntimeError) as error:
                    write_receipt(result_path, {'status': 'rejected', 'error': str(error)})
                    proposal_path.unlink(missing_ok=True)
                    time.sleep(0.25)
                    continue
                proposal_path.unlink(missing_ok=True)
                return applied
            current_metadata = source_metadata(self.root, self.source_roots)
            if current_metadata != metadata or time.monotonic() >= next_full_check:
                metadata = current_metadata
                next_full_check = time.monotonic() + 5.0
                after = self._timed('source_snapshot', source_snapshot,
                                    self.root, self.source_roots)
                if after['identity'] != before['identity']:
                    changed = changed_sources(before, after)
                    return {
                        'status': 'patched',
                        'origin': 'external_source_change',
                        'source_identity': after['identity'],
                        'allowed_files': sorted(changed),
                        'changed_files': sorted(changed),
                        'changed_shared_files': [],
                        'audit_verified': True,
                    }
                if shared_before is not None:
                    shared_after = source_snapshot(shared_root, (Path('src'),))
                    if shared_after['identity'] != shared_before['identity']:
                        changed_shared = changed_sources(shared_before, shared_after)
                        return {
                            'status': 'patched',
                            'origin': 'external_shared_source_change',
                            'source_identity': after['identity'],
                            'shared_source_identity': shared_after['identity'],
                            'allowed_files': [],
                            'changed_files': [],
                            'changed_shared_files': sorted(changed_shared),
                            'audit_verified': True,
                        }
            time.sleep(0.25)

    def _recover_interrupted_run(self, status: str) -> None:
        if self.state["attempts"]:
            attempt = self.state["attempts"][-1]
            if "finished_at" not in attempt:
                attempt["status"] = "interrupted"
                attempt["finished_at"] = time.time()
                attempt["interrupted_state"] = status
        self._transition("FAILED", error=f"Interrupted lockstep process in {status}")
        self._transition("VALIDATING", reason="retry_interrupted_process")

    def _resume_diagnostic_attempt(self, status: str) -> dict | None:
        if status != "REPLAYING" or not self.state["attempts"]:
            return None
        attempt = self.state["attempts"][-1]
        result = attempt.get("result") or {}
        if ("finished_at" not in attempt or attempt.get("packet") or
                result.get("kind") == "protocol_or_runtime_failure" or
                result.get("status") not in {
                    "memory_divergence", "control_flow_divergence", "runtime_divergence",
                }):
            return None
        return attempt

    def _checkpoint_refinement(self, attempt: dict, result: dict,
                               phase: str, progress) -> dict | None:
        retained = self._retained_checkpoint
        refine = self.hooks.refine
        if retained is None or "checkpoint" not in inspect.signature(refine).parameters:
            return None
        try:
            if file_digest(retained["path"]) != retained["sha256"]:
                raise LockstepSupervisorError("Native checkpoint changed before refinement")
            refined = self._timed(phase, refine, result, self.record,
                                  progress=progress, checkpoint=retained)
            self._count_launch("native", phase)
            if not refined.get("interval_complete"):
                attempt["checkpoint_refinement_error"] = "refinement_interval_incomplete"
                return None
            self.state["telemetry"]["checkpoint_refinements"] = (
                self.state["telemetry"].get("checkpoint_refinements", 0) + 1)
            return refined
        except Exception as error:
            attempt["checkpoint_refinement_error"] = str(error)
            return None

    def _diagnose_and_repair(self, attempt: dict, current_source: dict,
                             pending_signature: str | None) -> dict:
        result = attempt["result"]
        attempt_number = int(attempt["attempt"])
        refinement = prearmed_evidence(result) or boundary_evidence(result)
        if refinement is None and self._retained_checkpoint is not None:
            refinement = deferred_evidence(result)
            attempt["checkpoint_refinement_deferred"] = True
            self.state["telemetry"]["checkpoint_refinements_deferred"] = (
                self.state["telemetry"].get("checkpoint_refinements_deferred", 0) + 1)
        elif refinement is None:
            progress = lambda frame: self._write_progress(
                active=True, current=frame, mode="DIAGNOSTIC",
                target_frame=replay_target_frame(result))
            refinement = self._checkpoint_refinement(
                attempt, result, "checkpoint_diagnostic_refinement", progress)
            if refinement is None:
                self._discard_retained_checkpoint()
                if getattr(self.hooks, "watch_target", None) is not None:
                    self.hooks.set_watch_target(None)
                self._count_launch("duckstation", "diagnostic_refinement")
                self._count_launch("native", "diagnostic_refinement")
                refine = self.hooks.refine
                if "progress" in inspect.signature(refine).parameters:
                    refinement = self._timed("refinement", refine, result, self.record, progress=progress)
                else:
                    refinement = self._timed("refinement", refine, result, self.record)
        else:
            self.state["telemetry"]["refinement_skipped"] = (
                self.state["telemetry"].get("refinement_skipped", 0) + 1)
        packet = self.hooks.build_packet(result, refinement, attempt["build"])
        signature = packet["failure_signature"]
        case_key = packet.get("case_key")
        previous_attempt = next((row for row in reversed(self.state["attempts"][:-1])
                                 if row.get("failure_signature") == pending_signature), None)
        previous_case = (semantic_attempt_case(previous_attempt, self.record["record_id"])
                         if previous_attempt else None)
        same_case = case_key is not None and previous_case == case_key
        if (same_case and self._retained_checkpoint is None and
                result.get("status") == "memory_divergence" and
                not refinement.get("interval_complete") and
                not refinement.get("original_writes") and
                not refinement.get("native_writes") and
                case_key not in self.state["refined_cases"]):
            progress = lambda frame: self._write_progress(
                active=True, current=frame, mode="DIAGNOSTIC",
                target_frame=replay_target_frame(result))
            reused = self._checkpoint_refinement(
                attempt, result, "checkpoint_same_case_refinement", progress)
            if reused is not None:
                refinement = reused
            else:
                self._discard_retained_checkpoint()
                if getattr(self.hooks, "watch_target", None) is not None:
                    self.hooks.set_watch_target(None)
                self._count_launch("duckstation", "same_case_writer_refinement")
                self._count_launch("native", "same_case_writer_refinement")
                refine = self.hooks.refine
                if "progress" in inspect.signature(refine).parameters:
                    refinement = self._timed("same_case_writer_refinement", refine,
                                             result, self.record, progress=progress)
                else:
                    refinement = self._timed("same_case_writer_refinement", refine,
                                             result, self.record)
            packet = self.hooks.build_packet(result, refinement, attempt["build"])
            if packet.get("case_key") != case_key:
                raise LockstepSupervisorError("Writer refinement changed defect identity")
            signature = packet["failure_signature"]
            self.state["refined_cases"].append(case_key)
            self.state["telemetry"]["same_case_writer_refinements"] = (
                self.state["telemetry"].get("same_case_writer_refinements", 0) + 1)
        attempt["case_key"] = case_key
        function = (packet.get("source_context") or {}).get("function") or {}
        attempt["source_function"] = function.get("address")
        self._attach_related_history(packet, attempt)
        packet_path = self.directory / f"attention-{attempt_number:04d}.json"
        write_receipt(packet_path, packet)
        attempt["attention_packet_bytes"] = packet_path.stat().st_size
        try:
            from lockstep_repair_context import PROMPT_PREFIX, build_context
            context = build_context(packet, root=self.root, source_roots=self.source_roots)
            attempt["candidate_model_context_bytes"] = len(context.encode('utf-8'))
            attempt['candidate_model_prompt_bytes'] = (
                len(PROMPT_PREFIX.encode('utf-8')) +
                attempt['candidate_model_context_bytes'])
        except (KeyError, TypeError, ValueError):
            attempt["candidate_model_context_bytes"] = None
            attempt['candidate_model_prompt_bytes'] = None
        attempt.update(status="attention", failure_signature=signature, packet=str(packet_path))
        previous_result = (previous_attempt.get("result") or {}) if previous_attempt else {}
        previous_event = previous_result.get("original") or {}
        current_event = result.get("original") or {}
        previous_frame = (previous_event.get("frame")
                          if type(previous_event.get("frame")) is int else None)
        current_frame = (current_event.get("frame")
                         if type(current_event.get("frame")) is int else None)
        later_boundary = (previous_frame is not None and previous_frame == current_frame and
                          previous_event.get("mode") == current_event.get("mode") == "DETAILED" and
                          type(previous_event.get("event_sequence")) is int and
                          type(current_event.get("event_sequence")) is int and
                          current_event["event_sequence"] > previous_event["event_sequence"])
        advanced = (previous_frame is not None and current_frame is not None and
                    (current_frame > previous_frame or later_boundary))
        if (previous_frame is not None and current_frame is not None and
                current_frame < previous_frame):
            attempt["repair_regressed_before_prior_frame"] = previous_frame
        if pending_signature and signature != pending_signature and not same_case and advanced:
            self.state["accepted_repairs"].append({
                "failure_signature": pending_signature,
                "accepted_by": "advanced_to_new_divergence", "attempt": attempt_number,
            })
        known_case = case_key is not None and any(
            semantic_attempt_case(row, self.record["record_id"]) == case_key and
            row.get("failure_signature") in self.state["model_signatures"]
            for row in self.state["attempts"][:-1])
        if signature in self.state["model_signatures"] or known_case:
            reason = "unchanged_defect_case" if known_case and signature != pending_signature \
                else "unchanged_failure_signature"
            self._transition("ATTENTION_REQUIRED", reason=reason,
                             packet=str(packet_path), failure_signature=signature)
            if getattr(self.hooks, "live_repair_wait", False):
                repair = self._timed("repair_wait", self._wait_for_external_repair,
                                     current_source, packet_path)
                active_packet = Path(attempt["packet"])
                active_signature = attempt["failure_signature"]
                self._transition("VERIFYING_REPAIR", packet=str(active_packet),
                                 failure_signature=active_signature)
                current_source = self._verify_repair_scope(current_source, repair, active_packet)
                attempt["repair"] = repair
                self._save()
                return {
                    "current_source": current_source,
                    "pending_signature": active_signature,
                    "pending_target_frame": self.scan_target_frame,
                }
            return {"terminal": {
                "status": "ATTENTION_REQUIRED", "reason": reason,
                "packet": str(packet_path), "state": str(self.path),
            }}
        self._transition("ATTENTION_REQUIRED", packet=str(packet_path), failure_signature=signature)
        self.state["telemetry"]["attention_packets"] += 1
        self._save()
        repair = self._request_repair(packet, packet_path, current_source, attempt, signature)
        if repair.get("status") != "patched":
            return {"terminal": {
                "status": "ATTENTION_REQUIRED",
                "reason": repair.get("reason", "repair_unavailable"),
                "packet": str(packet_path), "state": str(self.path),
            }}
        active_packet = Path(attempt["packet"])
        active_signature = attempt["failure_signature"]
        self._transition("VERIFYING_REPAIR", packet=str(active_packet),
                         failure_signature=active_signature)
        current_source = self._verify_repair_scope(current_source, repair, active_packet)
        attempt["repair"] = repair
        self._save()
        return {
            "current_source": current_source,
            "pending_signature": active_signature,
            "pending_target_frame": self.scan_target_frame,
        }

    def _request_repair(self, packet: dict, packet_path: Path, current_source: dict,
                        attempt: dict, signature: str,
                        *, wait_on_attention: bool = True) -> dict:
        telemetry = self.state["telemetry"]
        reserved = telemetry.get("model_requests_reserved",
                                 telemetry.get("model_invocations_reported", 0))
        if signature not in self.state["model_signatures"]:
            self.state["model_signatures"].append(signature)
        attempt["model_request_in_flight"] = True
        telemetry["model_requests_reserved"] = reserved + 1
        self._save()
        repair = self._timed("repair_request", self.hooks.request_repair, packet, packet_path)
        attempt["model_request_in_flight"] = False
        if type(repair.get("model_invocations")) is int and repair["model_invocations"] >= 0:
            telemetry = self.state["telemetry"]
            invocations = repair["model_invocations"]
            attempt["repair_request_invocations"] = invocations
            if invocations == 0:
                self.state["model_signatures"].remove(signature)
                telemetry["model_requests_reserved"] -= 1
            telemetry["model_invocations_reported"] = (
                telemetry.get("model_invocations_reported", 0) + invocations)
            usage = repair.get("model_usage")
            fields = ("input_tokens", "output_tokens", "cached_input_tokens",
                      "reasoning_output_tokens", "total_tokens")
            if (invocations > 0 and isinstance(usage, dict) and
                    all(type(usage.get(key)) is int and usage[key] >= 0 for key in fields) and
                    usage["total_tokens"] == usage["input_tokens"] + usage["output_tokens"] and
                    usage["cached_input_tokens"] <= usage["input_tokens"] and
                    usage["reasoning_output_tokens"] <= usage["output_tokens"]):
                telemetry["model_usage_reported_invocations"] = (
                    telemetry.get("model_usage_reported_invocations", 0) + invocations)
                for key in fields:
                    name = "model_" + key + "_reported"
                    telemetry[name] = telemetry.get(name, 0) + usage[key]
            self._save()
        else:
            attempt["repair_request_invocations"] = None
            self.state["telemetry"]["model_accounting_complete"] = False
            self._save()
        if repair.get('status') == 'patched':
            raise LockstepSupervisorError(
                'Repair executor edited source instead of returning a proposal')
        if repair.get('status') == 'proposed':
            try:
                applied = self._apply_repair_proposal(
                    current_source, packet_path, repair.get('proposal'))
            except LockstepSupervisorError:
                raise
            except (ValueError, TypeError, KeyError, RuntimeError) as error:
                write_receipt(self.directory / 'repair-proposal.result.json',
                              {'status': 'rejected', 'error': str(error)})
                repair = {key: value for key, value in repair.items() if key != 'proposal'}
                repair.update(status='attention_required', reason='invalid_model_proposal',
                              model_reason=str(error)[:2048])
            else:
                repair = {key: value for key, value in repair.items() if key != 'proposal'}
                repair.update(applied)
        if repair.get("status") == "attention_required":
            attempt["repair_attention"] = {
                "reason": repair.get("reason"),
                "model_reason": (repair.get("model_reason") or "")[:2048],
            }
            self._save()
        if (wait_on_attention and repair.get("status") != "patched" and
                getattr(self.hooks, "live_repair_wait", False)):
            repair = self._timed("repair_wait", self._wait_for_external_repair,
                                 current_source, packet_path)
        return repair

    def _attach_related_history(self, packet: dict, current: dict) -> None:
        function = current.get("source_function")
        case = current.get("case_key")
        if function is None and case is None:
            return
        related = []
        for previous in self.state["attempts"]:
            if previous is current or not previous.get("packet"):
                continue
            previous_case = semantic_attempt_case(previous, self.record["record_id"])
            previous_function = previous.get("source_function")
            if previous_case is None and previous_function is None:
                path = Path(previous["packet"]).resolve()
                if path.is_relative_to(self.directory) and path.is_file():
                    older = json.loads(path.read_text(encoding="utf-8"))
                    previous_case = older.get("case_key")
                    previous_function = ((older.get("source_context") or {}).get("function") or {}).get("address")
            if (case is not None and previous_case == case) or (
                    function is not None and previous_function == function):
                related.append({
                    "attempt": previous.get("attempt"),
                    "frame": divergence_frame(previous.get("result") or {}),
                    "case_key": previous_case,
                    "same_case": previous_case is not None and previous_case == case,
                    "packet": previous["packet"],
                })
        if not related:
            return
        packet["related_attempt_count"] = len(related)
        packet["related_attempts"] = related[-12:]
        budget = packet.get("budget_bytes")
        if not isinstance(budget, int):
            return
        while packet["related_attempts"]:
            size = len(json.dumps(packet, sort_keys=True, separators=(",", ":")).encode("utf-8"))
            if size <= budget:
                packet["serialized_bytes"] = size
                return
            packet["related_attempts"].pop(0)
        packet.pop("related_attempts")
        packet.pop("related_attempt_count")

    def run(self) -> dict:
        pending_signature = None
        pending_target_frame = self.scan_target_frame
        pending_watch_target = None
        resume_diagnostic = None
        self._write_progress(active=True, current=0, mode="PREFIX_GUARD",
                             target_frame=self.scan_target_frame)
        self._start_progress_window()
        try:
            current_status = self.state["status"]
            if current_status == "COMPLETE":
                return {"status": "COMPLETE", "state": str(self.path),
                        "result": self.state.get("result")}
            if current_status in INTERRUPTIBLE_STATES:
                resume_diagnostic = self._resume_diagnostic_attempt(current_status)
                if resume_diagnostic is None:
                    if self.state["attempts"]:
                        pending_target_frame = int(
                            self.state["attempts"][-1].get("target_frame", 0))
                        if pending_target_frame == 0 and self.scan_target_frame > 0:
                            pending_target_frame = self.scan_target_frame
                        completed = next((item for item in reversed(self.state["attempts"][:-1])
                                          if item.get("result")), None)
                        if ((completed or {}).get("result", {}).get("kind") ==
                                "mode_epoch_divergence"):
                            pending_target_frame = self.scan_target_frame
                    self._recover_interrupted_run(current_status)
                current_status = "VALIDATING"
            if current_status == "ATTENTION_REQUIRED" and self.reprobe:
                previous = self.state["attempts"][-1]
                if (self.directory / 'repair-proposal.json').exists():
                    raise LockstepSupervisorError("Cannot reprobe with a pending repair proposal")
                result = previous.get("result") or {}
                if result.get("status") not in {
                        "memory_divergence", "control_flow_divergence", "runtime_divergence"}:
                    raise LockstepSupervisorError("Attention has no divergence to reprobe")
                if (result.get("kind") in {"premature_process_exit", "mode_epoch_divergence"} and
                        result.get("last_match") is None):
                    pending_target_frame = self.scan_target_frame
                else:
                    pending_target_frame = replay_target_frame(result)
                pending_watch_target = watch_target_from_result(result)
                self._transition("VALIDATING", reason="explicit_attention_reprobe")
                current_status = "VALIDATING"
            if current_status == "ATTENTION_REQUIRED":
                current_source = source_snapshot(self.root, self.source_roots)
                current_implementation = self._implementation_identity()
                previous = self.state["attempts"][-1]
                if current_source["identity"] == previous["source_identity"]:
                    result_path = self.directory / 'repair-proposal.result.json'
                    proof = (json.loads(result_path.read_text(encoding='utf-8'))
                             if result_path.is_file() else {})
                    shared_only_repair = (
                        proof.get('status') == 'applied' and
                        proof.get('source_identity') == current_source['identity'] and
                        proof.get('changed_files') == [] and
                        bool(proof.get('changed_shared_files')) and
                        proof.get('shared_source_identity') ==
                        getattr(self.hooks, 'shared_source_identity', lambda: None)()
                    )
                    if shared_only_repair:
                        self._verify_repair_audit(previous.get('packet'), current_source, set())
                        previous['repair'] = {
                            'status': 'patched', 'origin': 'supervisor_applied_model_code',
                            'source_identity': current_source['identity'],
                            'shared_source_identity': proof['shared_source_identity'],
                            'allowed_files': [], 'changed_files': [],
                            'changed_shared_files': proof['changed_shared_files'],
                            'audit_verified': True,
                        }
                    elif (previous.get("repair_request_invocations") == 0 and
                            not previous.get("model_request_in_flight")):
                        packet_path = Path(previous["packet"])
                        packet = json.loads(packet_path.read_text(encoding="utf-8"))
                        repair = self._request_repair(
                            packet, packet_path, current_source, previous,
                            previous["failure_signature"])
                        if repair.get("status") != "patched":
                            return {"status": "ATTENTION_REQUIRED",
                                    "reason": repair.get("reason", "repair_unavailable"),
                                    "packet": str(packet_path), "state": str(self.path)}
                        packet_path = Path(previous["packet"])
                        current_source = self._verify_repair_scope(
                            current_source, repair, packet_path)
                        previous["repair"] = repair
                    elif getattr(self.hooks, "live_repair_wait", False):
                        repair = self._wait_for_external_repair(
                            current_source, previous.get("packet"))
                        current_source = self._verify_repair_scope(
                            current_source, repair, previous.get("packet"))
                        previous["repair"] = repair
                    else:
                        return {"status": "ATTENTION_REQUIRED", "reason": "repair_not_applied",
                                "packet": previous.get("packet"), "state": str(self.path)}
                elif current_source["identity"] != previous["source_identity"]:
                    changed = changed_sources({'files': previous['source_files']},
                                              current_source)
                    self._verify_repair_audit(previous.get('packet'), current_source, changed,
                                              missing_allowed=True)
                    previous['repair'] = {
                        'status': 'patched', 'origin': 'external_source_change',
                        'source_identity': current_source['identity'],
                        'allowed_files': sorted(changed),
                        'changed_files': sorted(changed),
                        'changed_shared_files': [],
                        'audit_verified': True,
                    }
                    refresh = getattr(self.hooks, 'refresh_after_repair', None)
                    if refresh is not None:
                        refresh()
                pending_signature = previous.get("failure_signature")
                pending_target_frame = self.scan_target_frame
                if "repair" not in previous or previous["repair"].get("status") != "patched":
                    previous["repair"] = {
                        "status": "patched", "origin": "external_model",
                        "source_identity": current_source["identity"],
                        "implementation_identity": current_implementation,
                    }
                self._transition("VERIFYING_REPAIR", failure_signature=pending_signature,
                                 packet=previous.get("packet"))
            elif current_status == "STARTING":
                self._transition("VALIDATING")
                current_source = None
            elif current_status == "VALIDATING":
                current_source = None
            elif current_status == "FAILED":
                current_source = source_snapshot(self.root, self.source_roots)
                current_implementation = self._implementation_identity()
                previous = self.state["attempts"][-1] if self.state["attempts"] else {}
                previous_result = previous.get("result") or {}
                diagnostic_failure = str(self.state.get("error", "")).startswith((
                    "Only one refinement watchpoint is supported",
                    "Unknown fields: shared_source_identity",
                ))
                saved_validator = getattr(self.hooks, "validate_saved_diagnostic", None)
                resume_failed_diagnostic = (
                    diagnostic_failure and saved_validator is not None and
                    not previous.get("packet") and previous_result.get("status") in {
                        "memory_divergence", "control_flow_divergence", "runtime_divergence"} and
                    current_source["identity"] == previous.get("source_identity") and
                    saved_validator(previous, current_source["identity"]))
                failed_identity = self.state.get("failed_source_identity") or (
                    previous.get("repair") or {}).get("source_identity") or previous.get("source_identity")
                failed_implementation = self.state.get("failed_implementation_identity")
                scope_failure = str(self.state.get("error", "")).startswith("Foreign source change:")
                launch_access_failure = (
                    "[WinError 5]" in str(self.state.get("error", "")) and
                    previous.get("status") == "launching" and not previous_result)
                launch_infrastructure_failure = (
                    previous.get("status") == "launching" and not previous_result)
                retryable_failure = any(marker in str(self.state.get("error", "")) for marker in (
                    "already_attached", "No project-owned process receipt for this endpoint",
                    "Shared xport source changed after Debug build",
                    "Endpoint, creation identity or project data directory differs",
                )) or launch_access_failure or launch_infrastructure_failure
                if (current_source["identity"] == failed_identity and
                        current_implementation == failed_implementation and
                        not scope_failure and not retryable_failure and
                        not resume_failed_diagnostic):
                    return {"status": "FAILED", "error": self.state.get("error"),
                            "state": str(self.path)}
                same_failed_revision = (
                    current_source["identity"] == previous.get("source_identity") and
                    current_implementation == previous.get("implementation_identity")
                )
                if scope_failure:
                    pending_target_frame = self.scan_target_frame
                elif resume_failed_diagnostic:
                    resume_diagnostic = previous
                elif (same_failed_revision and not previous.get("packet") and
                        previous_result.get("status") in {
                        "memory_divergence", "control_flow_divergence", "runtime_divergence",
                }):
                    resume_diagnostic = previous
                elif previous_result.get("status") in {
                        "memory_divergence", "control_flow_divergence", "runtime_divergence",
                }:
                    pending_target_frame = replay_target_frame(previous_result)
                    pending_watch_target = watch_target_from_result(previous_result)
                elif previous and not previous.get("result"):
                    pending_target_frame = int(previous.get("target_frame", 0))
                    for completed in reversed(self.state["attempts"][:-1]):
                        if completed.get("repair") and completed.get("failure_signature"):
                            pending_signature = completed["failure_signature"]
                            break
                self._transition("VALIDATING", reason="retry_source_changed")
            else:
                raise LockstepSupervisorError(f"Cannot resume lockstep state: {current_status}")
            verification = verify_record_inputs(self.record)
            if verification["status"] != "match":
                raise LockstepSupervisorError("Record input identity mismatch")
            self.hooks.validate(self.record)
            if current_source is None:
                current_source = source_snapshot(self.root, self.source_roots)
            if resume_diagnostic is not None and hasattr(self.hooks, 'duckstation_runtime'):
                saved_runtime = resume_diagnostic.get('duckstation_runtime')
                if saved_runtime is None:
                    resume_diagnostic = None
                else:
                    self.hooks.duckstation_runtime = saved_runtime
            if resume_diagnostic is not None:
                saved_build = resume_diagnostic.get("build") or {}
                rebuilt = self._timed(
                    "debug_build", self.hooks.build_debug, current_source["identity"])
                resume_diagnostic["build"] = rebuilt
                resume_diagnostic["implementation_identity"] = self._implementation_identity()
                if not rebuilt.get("reused", False):
                    self.state["telemetry"]["debug_builds"] += 1
                else:
                    self.state["telemetry"]["debug_build_reuses"] = (
                        self.state["telemetry"].get("debug_build_reuses", 0) + 1)
                self._save()
                if (saved_build.get("binary_sha256") != rebuilt.get("binary_sha256") or
                        saved_build.get("pdb_sha256") != rebuilt.get("pdb_sha256")):
                    pending_target_frame = replay_target_frame(resume_diagnostic["result"])
                    pending_watch_target = watch_target_from_result(resume_diagnostic["result"])
                else:
                    diagnostic = self._diagnose_and_repair(
                        resume_diagnostic, current_source, pending_signature)
                    if diagnostic.get("terminal"):
                        return diagnostic["terminal"]
                    current_source = diagnostic["current_source"]
                    pending_signature = diagnostic["pending_signature"]
                    pending_target_frame = diagnostic["pending_target_frame"]
                    pending_watch_target = None
            diagnostic_frame = None
            for attempt_number in itertools.count(len(self.state["attempts"])):
                if self.max_repairs is not None and attempt_number > self.max_repairs:
                    raise LockstepSupervisorError("Repair attempt limit reached")
                # An initial-state replay can diagnose divergence before the first checkpoint
                target_frame = (self.scan_target_frame if diagnostic_frame is None
                                else diagnostic_frame)
                diagnostic_frame = None
                self.hooks.set_target_frame(target_frame)
                set_watch_target = getattr(self.hooks, "set_watch_target", None)
                if set_watch_target is not None:
                    set_watch_target(pending_watch_target)
                build = self._timed("debug_build", self.hooks.build_debug,
                                    current_source["identity"])
                if not build.get("reused", False):
                    self.state["telemetry"]["debug_builds"] += 1
                else:
                    self.state["telemetry"]["debug_build_reuses"] = (
                        self.state["telemetry"].get("debug_build_reuses", 0) + 1)
                resumed_checkpoint = (self._retained_checkpoint or
                                      self._prepare_durable_resume(target_frame))
                run_id = resumed_checkpoint["run_id"] if resumed_checkpoint else uuid.uuid4().hex
                attempt = {
                    "attempt": attempt_number, "run_id": run_id, "source_identity": current_source["identity"],
                    "source_files": current_source["files"],
                    "implementation_identity": self._implementation_identity(), "build": build,
                    "started_at": time.time(), "status": "launching",
                    "mode": "PREFIX_GUARD" if target_frame else "DETAILED",
                    "target_frame": target_frame,
                }
                if resumed_checkpoint:
                    attempt["checkpoint_pair_resumed"] = resumed_checkpoint["frame"]
                self.state["attempts"].append(attempt)
                self._write_progress(active=True, current=(resumed_checkpoint["frame"] + 1
                                    if resumed_checkpoint else 0), mode=attempt["mode"],
                                     target_frame=target_frame)
                self._save()
                original = native = scheduler = result = None
                retained_frame = None
                try:
                    try:
                        original, native, scheduler = self._launch(run_id)
                    except Exception as error:
                        if not resumed_checkpoint:
                            raise
                        attempt["status"] = "checkpoint_resume_rejected"
                        attempt["checkpoint_resume_error"] = str(error)
                        attempt["finished_at"] = time.time()
                        if resumed_checkpoint.get("durable"):
                            self.state.pop("durable_checkpoint", None)
                            clear = getattr(self.hooks, "clear_restart_checkpoint", None)
                            if clear is not None:
                                clear()
                        self._transition("VALIDATING", reason="checkpoint_resume_fallback")
                        continue
                    runtime_identity = getattr(original, 'runtime_identity', None)
                    if runtime_identity is not None:
                        attempt['duckstation_runtime'] = runtime_identity
                        attempt['record_patch_matches_runtime'] = (
                            runtime_identity['patch_sha256'] ==
                            self.record['duckstation']['patch_sha256'])
                    self._transition("REPLAYING", run_id=run_id)
                    def on_progress(step):
                        original_event = step.get("original") or {}
                        if (step.get("status") == "matched" and
                                step.get("full_memory_compared") is True and
                                original_event.get("kind") == "VBLANK"):
                            key = "suffix_match_frames" if resumed_checkpoint else "confirmed_match_frames"
                            attempt[key] = max(attempt.get(key, 0), int(original_event["frame"]) + 1)
                            self._write_progress(
                                active=True, current=int(original_event["frame"]) + 1,
                                mode=original_event.get("mode"), target_frame=target_frame,
                                confirmed_match=not bool(resumed_checkpoint),
                            )

                    detailed_run = getattr(scheduler, "run_with_progress", None)
                    verification_frame = None
                    previous_attempt = (self.state["attempts"][-2]
                                        if len(self.state["attempts"]) >= 2 else None)
                    if (target_frame and previous_attempt and previous_attempt.get("provisional") and
                            replay_target_frame(previous_attempt.get("result") or {}) == target_frame):
                        verification_frame = divergence_frame(previous_attempt.get("result") or {})
                        set_stop_frame = getattr(scheduler, "set_diagnostic_stop_frame", None)
                        if set_stop_frame is not None:
                            set_stop_frame(verification_frame)
                            attempt["diagnostic_verification_frame"] = verification_frame
                    phase = "replay_prefix_guard" if target_frame else "replay_detailed"
                    result = (self._timed(phase, detailed_run, self.max_steps, on_progress)
                              if detailed_run else self._timed(phase, scheduler.run, self.max_steps))
                    checkpoint_frame = getattr(scheduler, "checkpoint_frame", None)
                    if getattr(scheduler, "checkpoint_captures", 0):
                        attempt["checkpoint_captures"] = scheduler.checkpoint_captures
                        attempt["checkpoint_bytes"] = scheduler.checkpoint_bytes
                        attempt["checkpoint_capture_bytes_total"] = (
                            getattr(scheduler, "checkpoint_capture_bytes_total", 0))
                        attempt["checkpoint_capture_seconds"] = round(
                            getattr(scheduler, "checkpoint_capture_seconds", 0.0), 6)
                    if getattr(scheduler, "checkpoint_error", None):
                        attempt["checkpoint_error"] = scheduler.checkpoint_error
                    observed_divergence_frame = (result.get("original") or {}).get("frame")
                    if (checkpoint_frame is not None and isinstance(observed_divergence_frame, int) and
                            checkpoint_frame + 1 == observed_divergence_frame and
                            result.get("status") in {"memory_divergence", "control_flow_divergence"}):
                        try:
                            set_checkpoint_stop = getattr(
                                scheduler, "set_checkpoint_replay_stop_frame", None)
                            if set_checkpoint_stop is not None:
                                set_checkpoint_stop(observed_divergence_frame)
                            replay_target = watch_target_from_result(result)
                            replay_method = scheduler.replay_from_checkpoint
                            replay_keywords = ({"watch_target": replay_target}
                                               if "watch_target" in inspect.signature(
                                                   replay_method).parameters else {})
                            replay = self._timed(
                                "checkpoint_diagnostic_replay", replay_method,
                                checkpoint_frame, self.max_steps, on_progress,
                                **replay_keywords)
                            if (replay.get("status") in {"memory_divergence", "control_flow_divergence"} and
                                    (replay.get("original") or {}).get("frame") == observed_divergence_frame and
                                    (replay.get("original") or {}).get("mode") == "DETAILED"):
                                result = replay
                                attempt["checkpoint_replay"] = replay["checkpoint_replay"]
                                retained_frame = checkpoint_frame
                                self.state["telemetry"]["checkpoint_diagnostic_reuses"] = (
                                    self.state["telemetry"].get("checkpoint_diagnostic_reuses", 0) + 1)
                                watch_target = watch_target_from_result(replay)
                                observed_target = (replay.get("watch_evidence") or {}).get("target")
                                if (watch_target is not None and watch_target != observed_target and
                                        replay.get("status") == "memory_divergence"):
                                    try:
                                        if set_checkpoint_stop is not None:
                                            set_checkpoint_stop(observed_divergence_frame)
                                        watched = self._timed(
                                            "checkpoint_writer_replay", scheduler.replay_from_checkpoint,
                                            checkpoint_frame, self.max_steps, on_progress, watch_target)
                                        if same_memory_divergence(replay, watched):
                                            result = watched
                                            attempt["checkpoint_writer_replay"] = watched["checkpoint_replay"]
                                            self.state["telemetry"]["checkpoint_writer_reuses"] = (
                                                self.state["telemetry"].get("checkpoint_writer_reuses", 0) + 1)
                                        else:
                                            attempt["checkpoint_writer_error"] = "writer_replay_changed_divergence"
                                    except Exception as error:
                                        attempt["checkpoint_writer_error"] = str(error)
                            else:
                                attempt["checkpoint_error"] = "detailed_replay_did_not_reproduce"
                                attempt["status"] = "checkpoint_diagnostic_not_reproduced"
                                attempt["result"] = result
                                attempt["result_status"] = result.get("status")
                                attempt["checkpoint_diagnostic"] = replay
                                save_transcript = getattr(
                                    self.hooks, "save_native_transcript", None)
                                if save_transcript is not None:
                                    attempt["native_transcript"] = save_transcript(
                                        f"checkpoint-{attempt_number}")
                                attempt["finished_at"] = time.time()
                                self._save()
                                raise LockstepSupervisorError(
                                    "Checkpoint detailed replay did not reproduce the prefix "
                                    f"divergence at frame {observed_divergence_frame}")
                        except LockstepSupervisorError:
                            raise
                        except Exception as error:
                            attempt["checkpoint_error"] = str(error)
                            attempt["status"] = "checkpoint_diagnostic_failed"
                            attempt["result"] = result
                            attempt["result_status"] = result.get("status")
                            save_transcript = getattr(self.hooks, "save_native_transcript", None)
                            if save_transcript is not None:
                                attempt["native_transcript"] = save_transcript(
                                    f"checkpoint-{attempt_number}")
                            attempt["finished_at"] = time.time()
                            self._save()
                            raise LockstepSupervisorError(
                                "Checkpoint detailed replay failed: " + str(error)) from error
                    if (retained_frame is None and checkpoint_frame is not None and
                            isinstance(observed_divergence_frame, int) and
                            observed_divergence_frame > checkpoint_frame + 1 and
                            result.get("status") == "memory_divergence" and
                            target_frame):
                        raise LockstepSupervisorError(
                            "Prefix divergence checkpoint is not the preceding FrameDone: "
                            f"checkpoint {checkpoint_frame}, divergence {observed_divergence_frame}")
                    if (retained_frame is None and resumed_checkpoint and
                            result.get("status") in {"memory_divergence", "control_flow_divergence"}):
                        retained_frame = (checkpoint_frame if checkpoint_frame is not None
                                          else resumed_checkpoint["frame"])
                finally:
                    portable_path = (getattr(scheduler, "checkpoint_path", None)
                                     if scheduler is not None else None)
                    if portable_path is None:
                        portable_path = self.directory / "last-match.native"
                    retain = (retained_frame is not None and result is not None and
                              result.get("status") in {"memory_divergence", "control_flow_divergence"} and
                              portable_path.is_file() and scheduler is not None and original is not None)
                    portable_sha256 = None
                    if retain:
                        try:
                            portable_sha256 = file_digest(portable_path)
                        except OSError as error:
                            attempt["checkpoint_error"] = str(error)
                            retain = False
                    if retain:
                        self._retained_checkpoint = {
                            "run_id": run_id, "frame": retained_frame,
                            "path": portable_path, "sha256": portable_sha256,
                            "session": scheduler, "original": original,
                        }
                        self.state["live_checkpoint"] = {
                            "run_id": run_id, "frame": retained_frame,
                            "native_sha256": portable_sha256,
                        }
                        attempt["checkpoint_pair_available"] = retained_frame
                        export = getattr(self.hooks, "export_restart_checkpoint", None)
                        if export is not None:
                            try:
                                self._timed("durable_checkpoint_export", export,
                                            scheduler, retained_frame, portable_path)
                                container = self.directory / "last-match.xpck"
                                self.state["durable_checkpoint"] = {
                                    "frame": retained_frame,
                                    "sha256": file_digest(container),
                                }
                                attempt["durable_checkpoint_available"] = retained_frame
                            except Exception as error:
                                attempt["durable_checkpoint_error"] = str(error)
                                self.state.pop("durable_checkpoint", None)
                    else:
                        self._close(scheduler)
                    self._close(native)
                    if not retain:
                        self._close(original)
                attempt["result"] = result
                attempt["result_status"] = result.get("status")
                attempt["finished_at"] = time.time()
                self._save()
                resumed_frame = (result.get("original") or {}).get("frame")
                if (resumed_checkpoint and resumed_checkpoint.get("durable") and
                        result.get("status") in {"memory_divergence", "control_flow_divergence"} and
                        isinstance(resumed_frame, int) and resumed_frame < self.maximum_match):
                    attempt["status"] = "checkpoint_resume_rejected"
                    attempt["checkpoint_resume_error"] = (
                        f"Resumed prefix diverged at frame {resumed_frame} before "
                        f"previously matched frame {self.maximum_match}")
                    self._discard_retained_checkpoint()
                    self.state.pop("durable_checkpoint", None)
                    clear = getattr(self.hooks, "clear_restart_checkpoint", None)
                    if clear is not None:
                        clear()
                    self._transition("VALIDATING", reason="checkpoint_prefix_mismatch")
                    pending_target_frame = target_frame
                    continue
                if (result.get("status") == "runtime_divergence" and
                        result.get("kind") == "no_progress" and
                        not result.get("last_match")):
                    attempt["status"] = "transient_runtime_retry"
                    consecutive = 0
                    for previous_attempt in reversed(self.state["attempts"]):
                        previous_result = previous_attempt.get("result") or {}
                        if (int(previous_attempt.get("target_frame", 0)) != target_frame or
                                previous_result.get("status") != "runtime_divergence" or
                                previous_result.get("kind") != "no_progress" or
                                previous_result.get("last_match")):
                            break
                        consecutive += 1
                    self._save()
                    if consecutive >= 3:
                        raise LockstepSupervisorError(
                            "Native target made no initial boundary progress after 3 retries")
                    pending_target_frame = target_frame
                    continue
                if result.get("status") == "match_complete":
                    if resumed_checkpoint:
                        attempt["status"] = "checkpoint_suffix_matched_pending_full_prefix_acceptance"
                        attempt["provisional"] = True
                        self._discard_all_checkpoints()
                        pending_target_frame = self.scan_target_frame
                        pending_watch_target = None
                        self._save()
                        continue
                    attempt["status"] = "matched"
                    if pending_signature:
                        self.state["accepted_repairs"].append({
                            "failure_signature": pending_signature,
                            "accepted_by": "continuous_replay_progress", "attempt": attempt_number,
                        })
                    self._transition("MATCH", run_id=run_id)
                    self._transition("FINALIZING", run_id=run_id)
                    final = self.hooks.finalize_match(result, build)
                    self._transition("COMPLETE", result=final)
                    return {"status": "COMPLETE", "state": str(self.path), "result": final}
                if result.get("status") == "diagnostic_match":
                    raise LockstepSupervisorError(
                        "Detailed replay reached the bad VBlank without reproducing its divergence")
                if (result.get("status") == "memory_divergence" and
                        result.get("kind") == "ranged_memory_unresolved"):
                    raise LockstepSupervisorError(
                        "Temporary memory comparison remained unresolved at record exit")
                if result.get("status") not in {
                        "memory_divergence", "control_flow_divergence", "runtime_divergence"}:
                    raise LockstepSupervisorError("Unexpected scheduler result: " + str(result.get("status")))
                result_modes = {
                    event.get("mode") for event in (result.get("original"), result.get("native"))
                    if isinstance(event, dict) and event.get("mode")
                }
                if target_frame and "PREFIX_GUARD" in result_modes:
                    attempt["status"] = "prefix_divergence"
                    attempt["provisional"] = True
                    self._save()
                    if checkpoint_frame is None and target_frame == self.scan_target_frame:
                        diagnostic_frame = replay_target_frame(result)
                        pending_watch_target = watch_target_from_result(result)
                        attempt["cold_diagnostic_requested"] = True
                        self._save()
                        continue
                    raise LockstepSupervisorError(
                        "Prefix divergence has no reproducible detailed checkpoint replay")
                diagnostic = self._diagnose_and_repair(attempt, current_source, pending_signature)
                if diagnostic.get("terminal"):
                    return diagnostic["terminal"]
                current_source = diagnostic["current_source"]
                pending_signature = diagnostic["pending_signature"]
                pending_target_frame = diagnostic["pending_target_frame"]
                pending_watch_target = None
        except Exception as error:
            try:
                failed_source_identity = getattr(error, "failed_source_identity", None) or source_snapshot(self.root, self.source_roots)["identity"]
            except Exception:
                failed_source_identity = None
            self._transition("FAILED", error=str(error), failed_source_identity=failed_source_identity,
                             failed_implementation_identity=getattr(error, "failed_implementation_identity", None) or self._implementation_identity())
            return {"status": "FAILED", "error": str(error), "state": str(self.path)}
        finally:
            self._discard_retained_checkpoint()
            if self.state["status"] == "ATTENTION_REQUIRED":
                self._write_progress(active=True, current=0, mode="WAITING_REPAIR",
                                     target_frame=replay_target_frame(
                                         (self.state["attempts"][-1].get("result")
                                          if self.state["attempts"] else {}) or {}))
            elif self.state["status"] == "FAILED":
                self._write_progress(active=True, mode="FAILED")
            else:
                self._stop_progress_window()

    def run_interactive(self) -> dict:
        def action():
            while True:
                result = self.run()
                if result.get("status") != "FAILED" or not self.progress.enabled:
                    return result
                if not self._wait_failed_diagnostics():
                    return result
        return self.progress.run_action(action)

    def _wait_failed_diagnostics(self) -> bool:
        while True:
            if (source_snapshot(self.root, self.source_roots)["identity"] ==
                    self.state.get("failed_source_identity") and
                    (self.state.get("error") == "Shared xport source changed after Debug build" or
                     self._implementation_identity() != self.state.get("failed_implementation_identity"))):
                return True
            proposal_path = self.directory / "repair-proposal.json"
            packet_path = self.directory / "failed-prefix-runtime.json"
            if proposal_path.is_file() and packet_path.is_file():
                try:
                    proposal = json.loads(proposal_path.read_text(encoding="utf-8"))
                    packet = json.loads(packet_path.read_text(encoding="utf-8"))
                    if (proposal.get("memory_exceptions") != [] or
                            not proposal.get("adapter_audits") or
                            any(not any(edit.get('path') == audit.get('path') and
                                        edit.get('before', '') and
                                        edit['before'] in audit.get('function_before', '')
                                        for audit in proposal['adapter_audits'])
                                for edit in proposal.get('edits', [])) or
                            packet.get("identities", {}).get("native_source_identity") !=
                                self.state.get("failed_source_identity")):
                        raise ValueError("Failed prefix accepts only audited runtime adapter edits")
                    self._apply_repair_proposal(source_snapshot(self.root, self.source_roots),
                                                packet_path, proposal)
                except Exception as error:
                    write_receipt(self.directory / "repair-proposal.result.json",
                                  {"status": "rejected", "error": str(error)})
                    proposal_path.unlink(missing_ok=True)
                else:
                    proposal_path.unlink(missing_ok=True)
                    return True
            request_path = self.directory / "diagnostic-request.json"
            if not request_path.is_file():
                time.sleep(0.2)
                continue
            try:
                current = source_snapshot(self.root, self.source_roots)
                if current["identity"] != self.state.get("failed_source_identity"):
                    raise LockstepSupervisorError("Source changed after failed replay")
                attempt = next(row for row in reversed(self.state["attempts"])
                               if (row.get("result") or {}).get("status") == "memory_divergence"
                               and (row["result"].get("original") or {}).get("mode") == "PREFIX_GUARD"
                               and row.get("source_identity") == current["identity"])
                request = bind_diagnostic_request(
                    json.loads(request_path.read_text(encoding="utf-8")), attempt["result"])
                request["prefix_only"] = True
                kind = request.get("kind", "instruction")
                if kind not in {"instruction", "paired_memory", "writer_refinement"}:
                    raise ValueError("Unknown failed replay diagnostic kind")
                build = self._preflight_repair_build(current)
                if kind == "writer_refinement":
                    target_frame = int(attempt['result']['original']['frame']) - 1
                    durable = self._prepare_durable_diagnostic(target_frame)
                    try:
                        result = self._timed("failed_prefix_writer_refinement", self.hooks.refine,
                                             attempt["result"], self.record, prefix_only=True)
                    finally:
                        if durable is not None:
                            self.hooks.clear_restart_checkpoint()
                    if durable is not None:
                        result['checkpoint_pair_resumed'] = durable['frame']
                    result['complete'] = bool(result.get('interval_complete'))
                    result['target_frame'] = target_frame
                    write_receipt(self.directory / 'failed-prefix-writer-last.json', result)
                    if not result.get("interval_complete"):
                        raise RuntimeError("Failed prefix writer interval is incomplete")
                    refinement = {key: value for key, value in result.items()
                                  if key not in {'complete', 'target_frame', 'checkpoint_pair_resumed'}}
                    packet = self.hooks.build_packet(attempt["result"], refinement, build)
                    write_receipt(packet_path, packet)
                    result["runtime_repair_packet"] = str(packet_path)
                    write_receipt(self.directory / "diagnostic-result.json", result)
                    request_path.unlink(missing_ok=True)
                    continue
                probe = getattr(self.hooks, "paired_memory_probe" if kind == "paired_memory"
                                else "instruction_probe")
                result = self._timed("failed_prefix_diagnostic", probe, request, self.record,
                    progress=lambda frame: self._write_progress(
                        active=True, current=frame, mode="DIAGNOSTIC",
                        target_frame=request["target_frame"]))
                result["proof_scope"] = "read_only_failed_prefix_diagnostic"
                result["acceptance_eligible"] = False
            except Exception as error:
                result = {"schema": 1, "complete": False, "error": str(error)}
            write_receipt(self.directory / "diagnostic-result.json", result)
            request_path.unlink(missing_ok=True)
            self._write_progress(active=True, mode="FAILED")

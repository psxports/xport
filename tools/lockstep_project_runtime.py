#!/usr/bin/env python3
"""Generic process and debugger binding for a migrated lockstep project"""

from __future__ import annotations

import hashlib
import json
import mmap
import os
import socket
import struct
import subprocess
import sys
import time
import uuid
from pathlib import Path

from duckstation_runtime import (data_directory, environment as duckstation_environment,
                                 executable as duckstation_executable, launch_process,
                                 record_owner, runtime_identity, stop_owned_endpoint,
                                 validate_settings)
from duckstation_state85 import initial_state as read_duckstation_state
from lockstep_cdb import CdbController, CdbError
from lockstep_contract import exceptions_identity, mapping_name, validate_exceptions
from lockstep_coverage import MIPS_REGISTERS, load_registry
from lockstep_events import LockstepScheduler
from lockstep_gte_entry import compare_paired_gte_entry

GTE_CONTROL_NAMES = (
    *(f"gte_rt_{index}" for index in range(5)), "gte_trx", "gte_try", "gte_trz",
    *(f"gte_llm_{index}" for index in range(5)), "gte_rbk", "gte_gbk", "gte_bbk",
    *(f"gte_lcm_{index}" for index in range(5)), "gte_rfc", "gte_gfc", "gte_bfc",
    "gte_ofx", "gte_ofy", "gte_h", "gte_dqa", "gte_dqb", "gte_zsf3", "gte_zsf4",
    "gte_flag",
)
from gdb_remote import decode_registers
from lockstep_gdb import DuckStationLockstep, NativeCdbParticipant
from lockstep_packet import build_packet
from lockstep_packet import refinement_request
from lockstep_portable_checkpoint import extract_checkpoint, write_checkpoint
from lockstep_record import write_native_input
from lockstep_supervisor import changed_sources, shared_source_identity as toolset_source_identity, source_snapshot
from lockstep_writer_timeline import correlate_writers
from trace_worker import write_receipt
from xport_process import normalized_environment


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def validate_portable_checkpoint(path: Path, record_id: str, frame: int) -> None:
    with path.open("rb") as stream:
        header = stream.read(80)
    if len(header) != 80:
        raise RuntimeError("Native checkpoint header is truncated")
    magic, version, next_frame, _, saved_record_id = struct.unpack("<4sIII64s", header)
    if (magic != b"XPCS" or version != 6 or next_frame != frame + 1 or
            saved_record_id != record_id.encode("ascii")):
        raise RuntimeError("Native checkpoint does not match retained frame and record")


def native_checkpoint_export_path(directory: Path, load_path: Path | None) -> Path:
    primary = (directory / "last-match.native").resolve()
    alternate = (directory / "last-match.next.native").resolve()
    if load_path is not None and Path(load_path).resolve() == primary:
        return alternate
    return primary


def initial_spu_ram_path(record: dict) -> Path:
    state_path = Path(record["initial_state"]["path"])
    output = state_path.with_suffix(".spu")
    expected = record["initial_state"]["sha256"]
    receipt = output.with_suffix(".spu.json")
    if output.is_file() and output.stat().st_size == 512 * 1024 and receipt.is_file():
        value = json.loads(receipt.read_text(encoding="utf-8"))
        if value.get("initial_state_sha256") == expected:
            return output
    sound = read_duckstation_state(state_path)["spu"]
    ram = sound["ram"]
    if len(ram) != 512 * 1024:
        raise RuntimeError("DuckStation initial SPU RAM has an invalid size")
    temporary = output.with_suffix(".spu.tmp")
    temporary.write_bytes(ram)
    os.replace(temporary, output)
    write_receipt(receipt, {
        "schema": 1,
        "contract": "xport-lockstep-initial-spu-ram-v1",
        "initial_state_sha256": expected,
        "size": len(ram),
        "sha256": digest(output),
    })
    return output


def wait_port(host: str, port: int, process: subprocess.Popen, timeout: float = 30.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"DuckStation exited before GDB attach: {process.returncode}")
        try:
            with socket.create_connection((host, port), timeout=0.1):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("DuckStation did not expose its reserved GDB endpoint")


def refinement_target_frame(result: dict) -> int:
    last_match = (result.get("last_match") or {}).get("original")
    if isinstance(last_match, dict) and isinstance(last_match.get("frame"), int):
        frame = last_match["frame"]
        return max(0, frame if last_match.get("kind") == "VBLANK" else frame - 1)
    events = (result.get("original"), result.get("native"))
    frame = next((event["frame"] for event in events
                  if isinstance(event, dict) and isinstance(event.get("frame"), int)), 0)
    return max(0, frame - 1)


def diagnostic_stop_kind(stop: str, pc: int, addresses: tuple[int, ...],
                         watchpoints: tuple) -> str | None:
    if watchpoints and stop.startswith("T") and any(
            marker in stop for marker in ("watch:", "awatch:", "rwatch:")):
        return "watchpoint"
    if pc in addresses:
        return "breakpoint"
    return None


def _same_boundary(event: dict, expected: dict) -> bool:
    return all(event.get(key) == expected.get(key) for key in
               ("kind", "frame", "function", "invocation"))


def _matches_refinement_anchor(event: dict, anchor: dict) -> bool:
    identity_matches = all(event.get(key) == anchor.get(key) for key in (
        "kind", "frame", "image", "function", "callsite", "target",
    ))
    if not identity_matches:
        return False
    for key in ("invocation", "parent_invocation", "call_ordinal", "arguments", "return_value"):
        if key in anchor and event.get(key) != anchor.get(key):
            return False
    return True


def align_refinement(session, anchor: dict, target_frame: int, max_steps: int = 100_000,
                     progress=None, allow_ranged=False) -> dict:
    last_step = None
    if anchor.get("kind") == "START":
        comparison = session.original_adapter.compare("refine-begin")
        return {
            "aligned": comparison.get("status") == "match" or
                       (allow_ranged and comparison.get("status") in
                        {"ranged_match", "deferred"}),
            "last_step": None,
            "comparison": comparison,
            "target_frame": target_frame,
        }
    transitioned = target_frame == 0
    for _ in range(max_steps):
        last_step = session.scheduler.step()
        if last_step["status"] != "matched":
            return {"aligned": False, "last_step": last_step, "comparison": None}
        original = last_step.get("original") or {}
        if progress is not None and original.get("kind") == "VBLANK":
            progress(int(original.get("frame", 0)) + 1)
        if original.get("kind") == "MODE_TRANSITION":
            transitioned = True
            if (anchor.get("kind") == "VBLANK" or
                    _matches_refinement_anchor(original, anchor)):
                break
            continue
        if transitioned and _matches_refinement_anchor(original, anchor):
            break
    else:
        return {"aligned": False, "last_step": last_step, "comparison": None,
                "error": "refinement_anchor_not_found"}
    comparison = session.original_adapter.compare("refine-begin")
    return {
        "aligned": comparison.get("status") == "match" or
                   (allow_ranged and comparison.get("status") in
                    {"ranged_match", "deferred"}),
        "last_step": last_step,
        "comparison": comparison,
        "target_frame": target_frame,
    }


class OwnedProcess:
    def __init__(self, process: subprocess.Popen, *, port: int | None = None):
        self.process = process
        self.port = port

    def close(self) -> None:
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)


class AttachedSession:
    def __init__(self, scheduler, original_adapter, native_adapter, *, checkpoint_enabled=False,
                 checkpoint_target_frame=0, native_checkpoint_path=None):
        self.scheduler = scheduler
        self.original_adapter = original_adapter
        self.native_adapter = native_adapter
        self.checkpoint_enabled = checkpoint_enabled
        self.checkpoint_target_frame = checkpoint_target_frame
        self.checkpoint_frame = None
        self.checkpoint_error = None
        self.checkpoint_captures = 0
        self.checkpoint_bytes = 0
        self.checkpoint_capture_bytes_total = 0
        self.checkpoint_capture_seconds = 0.0
        self.native_checkpoint_path = (Path(native_checkpoint_path).resolve()
                                       if native_checkpoint_path is not None else None)
        self.checkpoint_path = None
        self.diagnostic_stop_frame = None
        self.checkpoint_replay_stop_frame = None

    def set_diagnostic_stop_frame(self, frame: int | None) -> None:
        self.diagnostic_stop_frame = frame

    def set_checkpoint_replay_stop_frame(self, frame: int | None) -> None:
        self.checkpoint_replay_stop_frame = frame

    def _with_writer_ring(self, result: dict) -> dict:
        controller = self.native_adapter.controller
        if (result.get("status") in {
                "memory_divergence", "control_flow_divergence", "runtime_divergence"} or
                self.scheduler.watch_target is not None):
            try:
                ring = controller.writer_ring()
                if ring.get("armed"):
                    result["native_writer_ring"] = ring
            except CdbError as error:
                if self.scheduler.watch_target is not None:
                    result["native_writer_ring"] = {"available": False, "error": str(error)}
        return result

    def run(self, max_steps: int) -> dict:
        return self._with_writer_ring(self.scheduler.run(max_steps))

    def run_with_progress(self, max_steps: int, progress) -> dict:
        class DiagnosticBoundaryReached(Exception):
            def __init__(self, result):
                self.result = result

        def observed(result):
            event = result.get("original") or {}
            if (self.checkpoint_enabled and self.checkpoint_error is None and
                    result.get("status") == "matched" and event.get("kind") == "VBLANK" and
                    result.get("full_memory_compared") is True and
                    event.get("mode") == "PREFIX_GUARD" and
                    event["frame"] < self.checkpoint_target_frame and self.original_adapter.record and
                    event["frame"] + 1 < len(self.original_adapter.record["frames"])):
                try:
                    started = time.perf_counter()
                    self.checkpoint_bytes = self.original_adapter.capture_checkpoint(event["frame"])
                    self.checkpoint_frame = event["frame"]
                    self.checkpoint_path = self.native_checkpoint_path
                    self.checkpoint_captures += 1
                    self.checkpoint_capture_bytes_total += self.checkpoint_bytes
                except Exception as error:
                    self.checkpoint_error = str(error)
                finally:
                    self.checkpoint_capture_seconds += time.perf_counter() - started
            progress(result)
            event = result.get("original") or {}
            if (self.diagnostic_stop_frame is not None and
                    result.get("status") == "matched" and
                    result.get("memory_compared") is True and
                    event.get("kind") == "VBLANK" and
                    int(event.get("frame", -1)) >= self.diagnostic_stop_frame):
                raise DiagnosticBoundaryReached(result)
        try:
            return self._with_writer_ring(self.scheduler.run(max_steps, observed))
        except DiagnosticBoundaryReached as reached:
            return {
                **reached.result,
                "status": "diagnostic_match",
                "kind": "provisional_boundary_not_reproduced",
                "last_match": reached.result,
            }

    def replay_from_checkpoint(self, frame: int, max_steps: int, progress=None,
                               watch_target: dict | None = None) -> dict:
        class DiagnosticBoundaryReached(Exception):
            def __init__(self, result):
                self.result = result

        if not self.checkpoint_enabled or self.checkpoint_frame != frame or max_steps < 2:
            raise RuntimeError("Matching checkpoint is unavailable")
        self.original_adapter.restore_checkpoint(frame)
        controller = self.native_adapter.controller
        controller.restore_checkpoint()
        self.native_adapter = NativeCdbParticipant(controller)
        self.scheduler = LockstepScheduler(self.original_adapter, self.native_adapter,
                                          initial_mode="PREFIX_GUARD",
                                          watch_target=watch_target)
        transition = self.scheduler.step()
        event = transition.get("original") or {}
        if (transition.get("status") != "matched" or event.get("kind") != "MODE_TRANSITION" or
                event.get("frame") != frame + 1):
            raise RuntimeError("Checkpoint replay did not start at the paired detailed transition: " +
                               str({"status": transition.get("status"), "kind": event.get("kind"),
                                    "frame": event.get("frame"),
                                    "native_frame": (transition.get("native") or {}).get("frame"),
                                    "error": transition.get("error")}))
        if progress is not None:
            progress(transition)
        ring_error = None
        if watch_target is not None:
            try:
                controller.arm_writer_ring(watch_target["region"], watch_target["offset"])
            except CdbError as error:
                ring_error = str(error)
        stop_frame = self.checkpoint_replay_stop_frame
        self.checkpoint_replay_stop_frame = None

        def observed(result):
            if progress is not None:
                progress(result)
            event = result.get("original") or {}
            if (stop_frame is not None and result.get("status") == "matched" and
                    result.get("memory_compared") is True and
                    event.get("kind") == "VBLANK" and
                    int(event.get("frame", -1)) >= stop_frame):
                raise DiagnosticBoundaryReached(result)

        try:
            result = self.scheduler.run(max_steps - 1, observed)
        except DiagnosticBoundaryReached as reached:
            result = {
                **reached.result,
                "status": "diagnostic_match",
                "kind": "checkpoint_boundary_not_reproduced",
                "last_match": reached.result,
            }
        collect_evidence = getattr(self.scheduler, "watch_evidence", None)
        evidence = collect_evidence() if collect_evidence is not None else None
        if evidence is not None:
            result["watch_evidence"] = evidence
        result["checkpoint_replay"] = {"from_frame": frame, "first_event": "MODE_TRANSITION"}
        if ring_error is not None:
            result["checkpoint_replay"]["writer_ring_error"] = ring_error
        return self._with_writer_ring(result)

    def close(self) -> None:
        self.original_adapter.close()


class ProjectLockstepHooks:
    def __init__(self, root: Path, config: dict, name: str, record: dict, *, telemetry: str,
                 record_name: str | None = None):
        self.root = Path(root).resolve()
        self.config = config
        self.name = name
        self.record_name = record_name or name
        self.record = record
        self.telemetry_mode = telemetry
        self.lockstep = config["lockstep"]

        self.runtime = self.lockstep["runtime"]
        self.exceptions = None
        self.abi_path = (self.root / self.lockstep["memory"]["abi_registry"]).resolve()
        self.abi = load_registry(self.abi_path)
        self.boundary_targets = tuple(int(row["address"], 0) for row in self.abi["functions"])
        self.argument_registers = {
            int(row["address"], 0): tuple(MIPS_REGISTERS[argument.get("register", f"a{index}")]
                                        for index, argument in enumerate(row["arguments"]))
            for row in self.abi["functions"]
            if any("register" in argument for argument in row["arguments"])
        }
        self.tail_calls = tuple((int(row["source"], 0), int(row["target"], 0))
                                for row in self.abi["reviewed_tail_calls"])
        self.input_path = self.root / "status" / "lockstep-converge" / name / f"{record['record_id']}.input"
        self.input_path.parent.mkdir(parents=True, exist_ok=True)
        if not self.input_path.exists():
            write_native_input(self.input_path, record)
        self.current_run_id: str | None = None
        self.native_controller: CdbController | None = None
        self.target_frame = 0
        self.progress_window = True
        self.live_repair_wait = True
        self.watch_target: dict | None = None
        self.portable_load_path: Path | None = None
        self.portable_duck_frame: int | None = None
        self.native_checkpoint_path: Path | None = None
        self._last_debug_build: dict | None = None
        self._last_debug_build_key: str | None = None
        self.duckstation_runtime: dict | None = None
        self.handoff_adapter: DuckStationLockstep | None = None
        self.handoff_frame: int | None = None
        self.handoff_registers: dict[str, str] | None = None
        self.handoff_device_state: dict[str, int] | None = None
        self.handoff_gte_control: dict[str, str] | None = None
        self.handoff_config: dict | None = None

    def native_executable(self, configuration: str) -> Path:
        paths = self.config["paths"]
        if configuration.casefold() == "debug":
            value = paths.get("native_debug_executable")
            if value is None:
                release = Path(paths["native_executable"])
                value = str(release.with_name(release.stem + "_debug" + release.suffix))
        else:
            value = paths["native_executable"]
        path = Path(value)
        return path.resolve() if path.is_absolute() else (self.root / path).resolve()

    def set_target_frame(self, frame: int) -> None:
        if frame < 0 or frame >= len(self.record["frames"]):
            raise ValueError("Lockstep target frame is outside the record")
        self.target_frame = frame

    def set_watch_target(self, target: dict | None) -> None:
        self.watch_target = target

    def set_portable_load(self, path: Path | None) -> None:
        self.portable_load_path = Path(path).resolve() if path is not None else None

    def export_restart_checkpoint(self, session, frame: int, native_path: Path) -> dict:
        runtime = self.duckstation_runtime or runtime_identity(duckstation_executable())
        if runtime["lockstep_protocol_version"] < 5 or session.checkpoint_frame != frame:
            raise RuntimeError("Durable paired checkpoint is unavailable")
        validate_portable_checkpoint(native_path, self.record["record_id"], frame)
        directory = self.root / "status" / "lockstep-converge" / self.name
        duck_path = directory / "last-match.duck.raw"
        size = session.original_adapter.export_checkpoint(frame)
        if duck_path.stat().st_size != size:
            raise RuntimeError("Exported DuckStation checkpoint size changed")
        try:
            return write_checkpoint(
                directory / "last-match.xpck", record_id=self.record["record_id"],
                frame=frame, duckstation_state=duck_path, native_state=native_path,
                duckstation_identity=runtime["executable_sha256"], native_abi="XPCS-v6")
        finally:
            duck_path.unlink(missing_ok=True)

    def prepare_restart_checkpoint(self) -> int | None:
        directory = self.root / "status" / "lockstep-converge" / self.name
        container = directory / "last-match.xpck"
        if not container.is_file():
            return None
        runtime = runtime_identity(duckstation_executable())
        if runtime["lockstep_protocol_version"] < 5:
            return None
        duck_path = directory / "last-match.duck.raw"
        native_path = directory / "last-match.native"
        manifest = extract_checkpoint(
            container, record_id=self.record["record_id"],
            duckstation_identity=runtime["executable_sha256"], native_abi="XPCS-v6",
            duckstation_state=duck_path, native_state=native_path)
        frame = manifest["frame"]
        validate_portable_checkpoint(native_path, self.record["record_id"], frame)
        self.set_portable_load(native_path)
        self.portable_duck_frame = frame
        return frame

    def clear_restart_checkpoint(self) -> None:
        self.portable_duck_frame = None
        self.set_portable_load(None)

    def implementation_identity(self) -> str:
        files = list(Path(__file__).parent.glob("lockstep*.py"))
        for pattern in ("*.c", "*.h", "*.cpp", "*.hpp"):
            files.extend((Path(__file__).parent.parent / "src").rglob(pattern))
        files.extend((self.abi_path,
                      Path(__file__).parent / "duckstation" / "duckstation-manifest.json",
                      Path(__file__).parent / "duckstation" / "internal-trace-api.patch"))
        files.extend((
            self.root / "status" / "translation-ledger.json",
            self.root / self.config["paths"]["analysis_database"],
            self.root / self.config["paths"]["ida_exports"] / self.lockstep["original_image"] / "functions.json",
        ))
        repair_exceptions = (self.root / "status" / "lockstep-converge" / self.name /
                             "repair-memory-exceptions.json")
        if repair_exceptions.is_file():
            files.append(repair_exceptions)
        value = hashlib.sha256()
        for path in sorted(files):
            value.update(str(path.resolve()).encode("utf-8"))
            value.update(bytes.fromhex(digest(path)))
        return value.hexdigest()

    def shared_source_identity(self) -> str:
        return toolset_source_identity(self.root, self.config)

    def shared_source_root(self) -> Path:
        toolset = Path(self.config['toolset'])
        if not toolset.is_absolute():
            toolset = self.root / toolset
        return toolset.resolve().parent

    def validate_saved_diagnostic(self, attempt: dict, source_identity: str) -> bool:
        build = attempt.get("build") or {}
        if (build.get("source_identity") != source_identity or
                build.get("shared_source_identity") != self.shared_source_identity()):
            return False
        executable = self.native_executable("Debug")
        pdb = self.root / "_build" / "x86" / "Debug" / (executable.stem + ".pdb")
        if (build.get("binary") != str(executable) or build.get("pdb") != str(pdb) or
                not executable.is_file() or not pdb.is_file() or
                build.get("binary_sha256") != digest(executable) or
                build.get("pdb_sha256") != digest(pdb)):
            return False
        try:
            return attempt.get("duckstation_runtime") == runtime_identity(duckstation_executable())
        except (OSError, ValueError):
            return False

    def validate(self, record: dict) -> None:
        if self.lockstep.get("msbuild_property") != "LockstepDebug=true":
            raise ValueError("LOCKSTEP_DEBUG is the only supported mapped-memory build")
        if self.lockstep["native_configuration"] != "Debug" or self.lockstep["native_platform"] != "x86":
            raise ValueError("Lockstep requires Debug x86")
        if record["original"]["image"] != self.lockstep["original_image"]:
            raise ValueError("Record original image differs from project binding")
        if self.abi["image"] != self.lockstep["original_image"] or self.abi["image_sha256"] != record["original"]["executable"]["sha256"]:
            raise ValueError("ABI registry belongs to another original image")
    def build_debug(self, source_identity: str) -> dict:
        self._last_debug_build_was_invoked = False
        shared_identity = self.shared_source_identity()
        solution = (self.root / self.config["paths"]["native_solution"]).resolve()
        executable = self.native_executable("Debug")
        pdb = self.root / "_build" / "x86" / "Debug" / (executable.stem + ".pdb")
        msbuild = self._msbuild_executable()
        build_inputs = [solution, self.root / "xport-project.json"]
        for directory in (self.root, self.root / "src", Path(__file__).parent.parent / "src"):
            if directory == self.root:
                for pattern in ("*.vcxproj", "*.props", "*.targets"):
                    build_inputs.extend(directory.glob(pattern))
            else:
                for pattern in ("*.vcxproj", "*.props", "*.targets"):
                    build_inputs.extend(directory.rglob(pattern))
        build_directory = (self.root / self.config["paths"].get("native_build", "_build")).resolve()
        for directory in (self.root / "src", Path(__file__).parent.parent / "src"):
            build_inputs.extend(path for path in directory.rglob("*")
                                if path.is_file() and
                                not path.resolve().is_relative_to(build_directory) and
                                path.resolve() not in {executable, pdb.resolve()})
        command = [str(msbuild), str(solution), "/t:Build", "/p:Configuration=Debug", "/p:Platform=x86",
                   "/p:LockstepDebug=true", "/m:1", "/nr:false", "/v:minimal", "/nologo"]
        environment = normalized_environment()
        key_data = {
            "source": source_identity,
            "shared_source": shared_identity,
            "inputs": {str(path.resolve()): digest(path)
                       for path in sorted(set(build_inputs)) if path.is_file()},
            "msbuild": [str(msbuild), msbuild.stat().st_size, msbuild.stat().st_mtime_ns],
            "command": command,
            "environment_sha256": hashlib.sha256(json.dumps(
                environment, sort_keys=True).encode("utf-8")).hexdigest(),
        }
        build_key = hashlib.sha256(json.dumps(key_data, sort_keys=True).encode("utf-8")).hexdigest()
        cache_path = self.root / "status" / "lockstep-converge" / self.name / "debug-build.cache.json"
        previous = self._last_debug_build
        if previous is None and cache_path.is_file():
            try:
                candidate = json.loads(cache_path.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                candidate = None
            if (isinstance(candidate, dict) and
                    candidate.get("contract") == "xport-lockstep-debug-build-cache-v1"):
                previous = candidate
        if (isinstance(previous, dict) and previous.get("build_key") == build_key and
                previous.get("source_identity") == source_identity and
                previous.get("shared_source_identity") == shared_identity and
                previous.get("binary") == str(executable) and
                previous.get("pdb") == str(pdb) and
                isinstance(previous.get("binary_sha256"), str) and
                isinstance(previous.get("pdb_sha256"), str) and
                executable.is_file() and pdb.is_file() and
                digest(executable) == previous["binary_sha256"] and
                digest(pdb) == previous["pdb_sha256"]):
            if self.shared_source_identity() != shared_identity:
                raise RuntimeError("Shared xport source changed during Debug build reuse")
            self._last_debug_build = previous
            self._last_debug_build_key = build_key
            return {**previous, "reused": True}
        self._last_debug_build_was_invoked = True
        result = subprocess.run(command, cwd=self.root, env=environment, capture_output=True, text=True)
        log = self.root / "status" / "lockstep-converge" / self.name / "debug-build.log"
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text(result.stdout + result.stderr, encoding="utf-8")
        if result.returncode or not executable.is_file() or not pdb.is_file():
            raise RuntimeError("Lockstep Debug x86 build failed")
        if self.shared_source_identity() != shared_identity:
            raise RuntimeError("Shared xport source changed during Debug build")
        build = {
            "configuration": "Debug", "platform": "x86", "lockstep_debug": True,
            "source_identity": source_identity, "binary": str(executable), "pdb": str(pdb),
            "shared_source_identity": shared_identity,
            "binary_sha256": digest(executable), "pdb_sha256": digest(pdb), "log": str(log),
            "build_key": build_key, "reused": False,
        }
        self._last_debug_build = build
        self._last_debug_build_key = build_key
        write_receipt(cache_path, {"contract": "xport-lockstep-debug-build-cache-v1", **build})
        return build

    def _require_current_debug_build(self) -> None:
        build = self._last_debug_build
        executable = self.native_executable("Debug")
        pdb = self.root / "_build" / "x86" / "Debug" / (executable.stem + ".pdb")
        if (not isinstance(build, dict) or build.get("binary") != str(executable) or
                build.get("pdb") != str(pdb) or not executable.is_file() or
                not pdb.is_file() or build.get("binary_sha256") != digest(executable) or
                build.get("pdb_sha256") != digest(pdb)):
            raise RuntimeError("Lockstep Debug artifacts changed after build")

    @staticmethod
    def _msbuild_executable() -> Path:
        return Path(r"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe")

    def launch_original(self, run_id: str, record: dict):
        role = self.runtime["duckstation_role"]
        host = self.config["duckstation"].get("host", "127.0.0.1")
        port = int(self.config["duckstation"]["reserved_gdb_ports"][role])
        directory = validate_settings(data_directory(role=role), port)
        stop_owned_endpoint(directory, port)
        executable = duckstation_executable()
        runtime = runtime_identity(executable)
        if runtime['upstream_commit'] != record['duckstation']['commit']:
            raise ValueError('DuckStation upstream commit differs from the record')
        state = Path(record["initial_state"]["path"])
        disc = Path(record["disc"][0]["path"])
        command = [str(executable), "-batch", "-fastboot", "-statefile", str(state), str(disc)]
        environment = duckstation_environment(directory)
        if self.lockstep.get("same_revision_checkpoint"):
            environment["XPORT_LOCKSTEP_CHECKPOINT_FILE"] = str(
                self.root / "status" / "lockstep-converge" / self.name /
                "last-match.duck.raw")
        process = launch_process(command, directory, environment, False)
        try:
            record_owner(directory, port, process.pid)
            wait_port(host, port, process)
        except Exception:
            OwnedProcess(process, port=port).close()
            raise
        owned = OwnedProcess(process, port=port)
        owned.host = host
        owned.runtime_identity = runtime
        self.duckstation_runtime = runtime
        handoff = self.lockstep.get("handoff")
        if handoff is not None:
            try:
                self._prepare_handoff(owned, record, handoff)
            except Exception:
                owned.close()
                raise
        return owned

    def _prepare_handoff(self, original, record: dict, handoff: dict) -> None:
        configured_routes = handoff.get("routes")
        routes = configured_routes if configured_routes is not None else [handoff]
        if not isinstance(routes, list) or not routes or len(routes) > 8:
            raise ValueError("Lockstep handoff routes are invalid")
        parsed_routes = []
        all_addresses = []
        for route in routes:
            breakpoints = route.get("ordered_breakpoints") if isinstance(route, dict) else None
            expected = route.get("expected_registers") if isinstance(route, dict) else None
            expected = expected or {}
            if (not isinstance(breakpoints, list) or not breakpoints or
                    len(breakpoints) > 8 or
                    not all(isinstance(value, str) for value in breakpoints) or
                    not isinstance(route.get("continuation"), str) or
                    not route["continuation"] or
                    not isinstance(expected, dict) or
                    not all(isinstance(key, str) and
                            (isinstance(value, str) or
                             (isinstance(value, list) and value and
                              all(isinstance(item, str) for item in value)))
                            for key, value in expected.items())):
                raise ValueError("Lockstep handoff contract is invalid")
            addresses = [int(value, 0) for value in breakpoints]
            if (len(set(addresses)) != len(addresses) or
                    any(address < 0x80000000 or address > 0xBFFFFFFF
                        for address in addresses)):
                raise ValueError("Lockstep handoff breakpoint is outside guest code")
            parsed_routes.append((route, addresses, expected))
            all_addresses.extend(addresses)
        if len(set(all_addresses)) != len(all_addresses):
            raise ValueError("Lockstep handoff routes overlap")
        adapter = DuckStationLockstep(original.host, original.port,
                                     runtime_identity=original.runtime_identity)
        try:
            stop = adapter.remote.packet("?")
            if not stop.startswith(("S", "T")):
                raise RuntimeError("DuckStation did not start paused at the record state")
            adapter.start_prelude(record)
            armed = set(all_addresses)
            for address in all_addresses:
                adapter._expect(adapter.remote.packet(f"Z0,{address:x},4"), "OK",
                                "handoff breakpoint")
            selected = None
            addresses = None
            expected = None
            next_index = None
            first_frame_breakpoint = None
            while selected is None or next_index is None or next_index < len(addresses):
                reply = adapter.remote.packet("c", timeout=30.0)
                if not reply.startswith(("S", "T")):
                    raise RuntimeError(f"Unexpected DuckStation handoff stop: {reply}")
                pending = adapter.remote.packet("qXportLockstepNextEvent")
                if pending != "NONE":
                    event = adapter._consume_pending(pending)
                    if event["kind"] in {"FAULT", "PROCESS_EXIT"}:
                        target = addresses[next_index or 0] if addresses else all_addresses[0]
                        raise RuntimeError(
                            f"DuckStation ended before handoff 0x{target:08X}: {event}")
                    if first_frame_breakpoint is not None and adapter.input_index > 0:
                        adapter._expect(
                            adapter.remote.packet(
                                f"Z0,{first_frame_breakpoint:x},4"), "OK",
                            "handoff first-frame breakpoint")
                        armed.add(first_frame_breakpoint)
                        first_frame_breakpoint = None
                    continue
                registers = adapter.registers()
                pc = int(registers["pc"], 16)
                if pc not in armed:
                    raise RuntimeError(
                        f"Unexpected handoff breakpoint PC {registers['pc']}")
                if selected is None:
                    matches = [item for item in parsed_routes if pc in item[1]]
                    if len(matches) != 1:
                        raise RuntimeError(
                            f"Ambiguous handoff breakpoint PC {registers['pc']}")
                    selected, addresses, expected = matches[0]
                    for unused in tuple(armed):
                        if unused not in addresses:
                            adapter._expect(
                                adapter.remote.packet(f"z0,{unused:x},4"), "OK",
                                "handoff unused route breakpoint clear")
                            armed.remove(unused)
                index = addresses.index(pc)
                if next_index is not None and index != next_index:
                    raise RuntimeError(
                        f"Out-of-order handoff breakpoint PC {registers['pc']}")
                if next_index is None:
                    for skipped in addresses[:index]:
                        adapter._expect(
                            adapter.remote.packet(f"z0,{skipped:x},4"), "OK",
                            "handoff skipped breakpoint clear")
                        armed.remove(skipped)
                adapter._expect(adapter.remote.packet(f"z0,{pc:x},4"), "OK",
                                "handoff breakpoint clear")
                armed.remove(pc)
                next_index = index + 1
                if next_index == len(addresses) and adapter.input_index == 0:
                    first_frame_breakpoint = pc
                    next_index = index
            for name, value in expected.items():
                values = value if isinstance(value, list) else [value]
                accepted = {f"0x{int(item, 0):08x}".lower() for item in values}
                if registers.get(name, "").lower() not in accepted:
                    raise RuntimeError(
                        f"Lockstep handoff register {name} differs: {registers.get(name)}")
            if not 0 < adapter.input_index < len(record["frames"]):
                raise RuntimeError("Lockstep handoff did not consume a valid input prefix")
            device_state = adapter.handoff_state()
            gte_control = adapter.gte_control_registers()
            self.handoff_adapter = adapter
            self.handoff_frame = adapter.input_index
            self.handoff_registers = registers
            self.handoff_device_state = device_state
            self.handoff_gte_control = gte_control
            self.handoff_config = selected
        except Exception:
            adapter.close()
            raise

    def launch_native(self, run_id: str, record: dict, writer_target: dict | None = None):
        self._require_current_debug_build()
        executable = self.native_executable("Debug")
        pdb = self.root / "_build" / "x86" / "Debug" / (executable.stem + ".pdb")
        environment = {
            "XPORT_LOCKSTEP_RUN_ID": run_id,
            "XPORT_LOCKSTEP_DRAM_MAPPING": mapping_name(run_id, "dram"),
            "XPORT_LOCKSTEP_SCRATCHPAD_MAPPING": mapping_name(run_id, "scratchpad"),
            "XPORT_LOCKSTEP_DEFERRED_MAPPING": mapping_name(run_id, "deferred"),
            "XPORT_LOCKSTEP_INPUT_PATH": str(self.input_path),
            "XPORT_LOCKSTEP_RECORD_ID": record["record_id"],
            "XPORT_LOCKSTEP_TARGET_FRAME": str(self.target_frame),
        }
        disc_image = Path(self.config["paths"]["disc_image"])
        if not disc_image.is_absolute():
            disc_image = self.root / disc_image
        environment["XPORT_DISC_IMAGE"] = str(disc_image.resolve())
        environment["XPORT_LOCKSTEP_INITIAL_SPU_RAM"] = str(initial_spu_ram_path(record))
        if self.handoff_frame is not None:
            handoff = self.handoff_config or self.lockstep["handoff"]
            environment["XPORT_LOCKSTEP_START_FRAME"] = str(self.handoff_frame)
            environment["XPORT_LOCKSTEP_CONTINUATION"] = str(handoff["continuation"])
            argument_registers = handoff.get("argument_registers", ["r16", "r17"])
            if (not isinstance(argument_registers, list) or
                    not all(isinstance(name, str) and name in self.handoff_registers
                            for name in argument_registers)):
                raise RuntimeError("Lockstep handoff argument registers are invalid")
            for index, name in enumerate(argument_registers):
                environment[f"XPORT_LOCKSTEP_CONTINUATION_ARG{index}"] = str(
                    int(self.handoff_registers[name], 16))
            environment["XPORT_LOCKSTEP_TIMER1"] = str(
                self.handoff_device_state["timer1"])
            environment["XPORT_LOCKSTEP_GPU_STATUS"] = str(
                self.handoff_device_state["gpu_status"])
            environment["XPORT_LOCKSTEP_FIELD_LINES"] = str(
                self.handoff_device_state["field_lines"])
            if (not isinstance(self.handoff_gte_control, dict) or
                    any(name not in self.handoff_gte_control for name in GTE_CONTROL_NAMES)):
                raise RuntimeError("Lockstep handoff GTE control state is invalid")
            for index, name in enumerate(GTE_CONTROL_NAMES):
                environment[f"XPORT_LOCKSTEP_GTE_CONTROL_{index}"] = str(
                    int(self.handoff_gte_control[name], 16))
        if self.lockstep.get("same_revision_checkpoint"):
            directory = self.root / "status" / "lockstep-converge" / self.name
            self.native_checkpoint_path = native_checkpoint_export_path(
                directory, self.portable_load_path)
            environment["XPORT_LOCKSTEP_CHECKPOINT"] = "1"
            environment["XPORT_LOCKSTEP_PORTABLE_EXPORT_PATH"] = str(
                self.native_checkpoint_path)
        if self.portable_load_path is not None:
            environment["XPORT_LOCKSTEP_PORTABLE_LOAD_PATH"] = str(self.portable_load_path)
            environment["XPORT_LOCKSTEP_REUSE_MAPPING"] = "1"
            if (self.duckstation_runtime and
                    self.duckstation_runtime.get("lockstep_protocol_version", 0) >= 4 and
                    self.target_frame > 0):
                environment["XPORT_LOCKSTEP_PREFIX_RESUME"] = "1"
        controller = CdbController(
            executable, pdb, arguments=tuple(self.runtime["native_arguments"]),
            cwd=(self.root / self.config["paths"]["native_working_directory"]).resolve(),
            memory_ready=True, environment=environment,
            boundary_addresses=frozenset(self.boundary_targets),
        )
        try:
            controller.start()
            controller.wait_memory_ready()
        except Exception:
            log = self.root / "status" / "lockstep-converge" / self.name / \
                "native-launch-failure.log"
            log.write_text(controller.output[-1024 * 1024:], encoding="utf-8")
            controller.close()
            raise
        self.exceptions = controller.memory_exceptions(record["original"]["executable"]["sha256"])
        from lockstep_repair_exceptions import merge_repair_ranges
        self.exceptions = merge_repair_ranges(
            self.exceptions,
            self.root / "status" / "lockstep-converge" / self.name /
            "repair-memory-exceptions.json")
        self.current_run_id = run_id
        self.native_controller = controller
        return controller

    def attach(self, original, native, run_id: str, record: dict):
        original_adapter = self.handoff_adapter or DuckStationLockstep(
            original.host, original.port, runtime_identity=original.runtime_identity)
        if self.handoff_adapter is None:
            stop = original_adapter.remote.packet("?")
            if not stop.startswith(("S", "T")):
                original_adapter.close()
                raise RuntimeError("DuckStation did not start paused at the record state")
        peer = {
            "schema": 1, "contract": "xport-lockstep-peer-v1", "run_id": run_id,
            "dram": mapping_name(run_id, "dram"),
            "scratchpad": mapping_name(run_id, "scratchpad"),
            "deferred": mapping_name(run_id, "deferred"),
            "sizes": {"dram": 0x200000, "scratchpad": 0x400},
        }
        if self.exceptions is None:
            raise RuntimeError("Native memory exceptions were not loaded")
        validate_exceptions(self.exceptions)
        restored_frame = self.portable_duck_frame
        restored_native_path = self.portable_load_path
        if self.handoff_adapter is not None:
            original_adapter.attach_handoff(
                peer, self.exceptions, self.boundary_targets, self.handoff_frame,
                self.target_frame, self.tail_calls, self.argument_registers)
            self.handoff_adapter = None
        else:
            original_adapter.attach(peer, self.exceptions, record, self.boundary_targets,
                                    self.tail_calls, self.target_frame, self.argument_registers,
                                    portable_frame=restored_frame)
        if restored_frame is not None:
            (self.root / "status" / "lockstep-converge" / self.name /
             "last-match.duck.raw").unlink(missing_ok=True)
            self.portable_duck_frame = None
            self.set_portable_load(None)
        native_adapter = NativeCdbParticipant(native, initially_stopped=True)
        if restored_frame is not None:
            baseline = original_adapter.compare("checkpoint-baseline")
            if baseline["status"] != "match":
                raise RuntimeError(f"Portable checkpoint memory mismatch: {baseline}")
        scheduler = LockstepScheduler(
            original_adapter, native_adapter,
            initial_mode="PREFIX_GUARD" if restored_frame is not None else None,
            watch_target=self.watch_target)
        if self.watch_target is not None:
            native.arm_writer_ring(
                self.watch_target["region"], self.watch_target["offset"])
        if self.watch_target is not None and self.target_frame == 0:
            scheduler._arm_watch({"mode": "DETAILED"})
        session = AttachedSession(
            scheduler, original_adapter, native_adapter,
            checkpoint_enabled=bool(self.lockstep.get("same_revision_checkpoint")),
            checkpoint_target_frame=self.target_frame,
            native_checkpoint_path=self.native_checkpoint_path)
        if restored_frame is not None:
            session.checkpoint_frame = restored_frame
            session.checkpoint_path = restored_native_path
        return session

    def resume_checkpoint(self, original_adapter, run_id: str, record: dict,
                          frame: int, path: Path):
        if original_adapter.run_id != run_id or not path.is_file():
            raise RuntimeError("Paired checkpoint identity is unavailable")
        validate_portable_checkpoint(path, record["record_id"], frame)
        self.set_portable_load(path)
        try:
            native = self.launch_native(run_id, record)
        finally:
            self.set_portable_load(None)
        try:
            if original_adapter.exceptions_identity != exceptions_identity(self.exceptions):
                raise RuntimeError("Checkpoint memory exceptions changed")
            prefix_target = (self.target_frame if self.duckstation_runtime and
                             self.duckstation_runtime.get("lockstep_protocol_version", 0) >= 4 and
                             self.target_frame > frame + 1 else None)
            original_adapter.restore_checkpoint(frame, prefix_target)
            native_adapter = NativeCdbParticipant(native, initially_stopped=True)
            scheduler = LockstepScheduler(original_adapter, native_adapter,
                                          initial_mode="PREFIX_GUARD",
                                          watch_target=self.watch_target)
            session = AttachedSession(scheduler, original_adapter, native_adapter,
                                      checkpoint_enabled=bool(self.lockstep.get("same_revision_checkpoint")),
                                      checkpoint_target_frame=self.target_frame,
                                      native_checkpoint_path=self.native_checkpoint_path)
            if session.checkpoint_enabled:
                session.checkpoint_frame = frame
                session.checkpoint_path = Path(path).resolve()
            return native, session
        except Exception:
            native.close()
            raise

    def instruction_probe(self, request: dict, record: dict, progress=None,
                          checkpoint: dict | None = None) -> dict:
        target_frame = int(request.get("target_frame", 0))
        anchor = request.get("anchor") or {}
        addresses = tuple(int(value, 0) if isinstance(value, str) else int(value)
                          for value in request.get("breakpoints", ()))
        watchpoints = tuple(
            (int(row["address"], 0) if isinstance(row["address"], str) else int(row["address"]),
             int(row.get("size", 4)))
            for row in request.get("watchpoints", ()))
        register_filter = request.get("register_filter") or {}
        memory_reads = request.get("memory_reads") or []
        native_entry = request.get("native_entry")
        capture_gte = request.get("capture_gte", False)
        until = request.get("until") or {}
        max_stops = int(request.get("max_stops", 100000))
        stop_after_hits = int(request.get("stop_after_hits", 0))
        if ((not addresses and not watchpoints) or len(addresses) > 16 or len(watchpoints) > 4 or
                max_stops < 1 or max_stops > 1_000_000 or
                stop_after_hits < 0 or stop_after_hits > max_stops):
            raise ValueError("Instruction probe request is invalid")
        if any(size not in (1, 2, 4) for _, size in watchpoints):
            raise ValueError("Instruction probe watchpoint request is invalid")
        if len(memory_reads) > 16:
            raise ValueError("Instruction probe memory request is invalid")
        if type(capture_gte) is not bool:
            raise ValueError("Instruction probe GTE request is invalid")
        for read in memory_reads:
            size = int(read.get("size", 0))
            if size < 1 or size > 256:
                raise ValueError("Instruction probe memory request is invalid")
        if native_entry is not None:
            if not isinstance(native_entry, dict) or not isinstance(native_entry.get("symbol"), str):
                raise ValueError("Native entry probe request is invalid")
            entry_size = int(native_entry.get("size", 16))
            if not 1 <= entry_size <= 256:
                raise ValueError("Native entry probe size is invalid")
            entry_address = native_entry.get("output_address")
            entry_address = (int(entry_address, 0) if isinstance(entry_address, str)
                             else int(entry_address))
            entry_max_hits = int(native_entry.get("max_hits", 128))
            entry_match_occurrence = int(native_entry.get("match_occurrence", 1))
            entry_second_storage = native_entry.get("second_storage", "guest")
            entry_caller_offsets = tuple(
                int(value, 0) if isinstance(value, str) else int(value)
                for value in native_entry.get("caller_stack_offsets", ()))
            if entry_second_storage not in {"guest", "host", "none"} or (entry_second_storage == "host" and entry_size != 16):
                raise ValueError("Native entry second storage is invalid")
        previous_target = self.target_frame
        self.set_target_frame(target_frame)
        run_id = uuid.uuid4().hex
        original_process = native_process = session = None
        installed_addresses = []
        installed_watchpoints = []
        result = {"schema": 1, "target_frame": target_frame, "hits": [], "events": [],
                  "stop_counts": {"total": 0, "semantic_event": 0,
                                  "instruction_breakpoint": 0, "data_watchpoint": 0,
                                  "other": 0}}
        try:
            if checkpoint is not None:
                if target_frame < checkpoint["frame"]:
                    raise RuntimeError("Instruction probe target precedes retained checkpoint")
                run_id = checkpoint["run_id"]
                native_process, session = self.resume_checkpoint(
                    checkpoint["session"].original_adapter, run_id, record,
                    checkpoint["frame"], checkpoint["path"])
            else:
                original_process = self.launch_original(run_id, record)
                native_process = self.launch_native(run_id, record)
                session = self.attach(original_process, native_process, run_id, record)
            alignment = align_refinement(session, anchor, target_frame, progress=progress)
            result["alignment"] = alignment
            if capture_gte and alignment["aligned"]:
                result["original_gte_control_at_alignment"] = \
                    session.original_adapter.gte_control_registers()
                result["native_gte_at_alignment"] = native_process.gte_snapshot()
            if memory_reads:
                mappings = {}
                try:
                    mappings["dram"] = mmap.mmap(
                        -1, 0x200000, tagname=mapping_name(run_id, "dram"), access=mmap.ACCESS_READ)
                    mappings["scratchpad"] = mmap.mmap(
                        -1, 0x400, tagname=mapping_name(run_id, "scratchpad"), access=mmap.ACCESS_READ)
                    result["native_memory_at_alignment"] = []
                    result["original_memory_at_alignment"] = []
                    for read in memory_reads:
                        address = read.get("address")
                        address = int(address, 0) if isinstance(address, str) else int(address)
                        size = int(read["size"])
                        if 0x1F800000 <= address <= 0x1F8003FF:
                            region = "scratchpad"
                            offset = address - 0x1F800000
                        else:
                            region = "dram"
                            offset = address & 0x1FFFFF
                        result["native_memory_at_alignment"].append({
                            "address": f"0x{address:08X}",
                            "size": size,
                            "data": mappings[region][offset:offset + size].hex(),
                        })
                        result["original_memory_at_alignment"].append({
                            "address": f"0x{address:08X}",
                            "size": size,
                            "data": session.original_adapter.remote.read_memory(
                                address & 0x1FFFFFFF, size).hex(),
                        })
                finally:
                    for mapping in mappings.values():
                        mapping.close()
            if native_entry is not None and alignment["aligned"]:
                entry = native_process.probe_function_entry(
                    native_entry["symbol"], entry_address, max_hits=entry_max_hits,
                    second_storage=entry_second_storage,
                    caller_stack_offsets=entry_caller_offsets,
                    match_occurrence=entry_match_occurrence)
                if capture_gte:
                    result["native_gte_at_entry"] = native_process.gte_snapshot()
                views = {}
                try:
                    views["dram"] = mmap.mmap(
                        -1, 0x200000, tagname=mapping_name(run_id, "dram"), access=mmap.ACCESS_READ)
                    views["scratchpad"] = mmap.mmap(
                        -1, 0x400, tagname=mapping_name(run_id, "scratchpad"), access=mmap.ACCESS_READ)
                    entry["memory"] = []
                    for key in (("output_address",) if entry_second_storage == "none" else
                                ("output_address", "right_address")):
                        if key == "right_address" and entry_second_storage == "host":
                            entry["memory"].append({"argument": key, "address": entry[key],
                                                    "storage": "host", "size": 16,
                                                    "data": entry["right_data"]})
                            continue
                        address = int(entry[key], 16)
                        if 0x1F800000 <= address <= 0x1F8003FF:
                            region, offset = "scratchpad", address - 0x1F800000
                        else:
                            region, offset = "dram", address & 0x1FFFFF
                        if offset + entry_size > len(views[region]):
                            raise RuntimeError("Native entry input crosses guest memory boundary")
                        entry["memory"].append({"argument": key, "address": entry[key],
                                                "size": entry_size,
                                                "data": views[region][offset:offset + entry_size].hex()})
                    entry["probe_memory"] = []
                    for read in memory_reads:
                        address = read.get("address")
                        address = int(address, 0) if isinstance(address, str) else int(address)
                        size = int(read["size"])
                        if 0x1F800000 <= address <= 0x1F8003FF:
                            region, offset = "scratchpad", address - 0x1F800000
                        else:
                            region, offset = "dram", address & 0x1FFFFF
                        if offset + size > len(views[region]):
                            raise RuntimeError("Native entry probe memory crosses guest memory boundary")
                        entry["probe_memory"].append({
                            "address": f"0x{address:08X}", "size": size,
                            "data": views[region][offset:offset + size].hex(),
                        })
                    result["native_entry"] = entry
                finally:
                    for view in views.values():
                        view.close()
            if not alignment["aligned"] and request.get("continue_native_to_until_on_divergence"):
                for _ in range(max_stops):
                    native_event = session.native_adapter.advance(30.0)
                    if until and all(native_event.get(key) == value for key, value in until.items()):
                        result["native_terminal_event"] = native_event
                        break
                else:
                    result["error"] = "native_probe_stop_limit"
                    return result
                mappings = {}
                try:
                    mappings["dram"] = mmap.mmap(
                        -1, 0x200000, tagname=mapping_name(run_id, "dram"), access=mmap.ACCESS_READ)
                    mappings["scratchpad"] = mmap.mmap(
                        -1, 0x400, tagname=mapping_name(run_id, "scratchpad"), access=mmap.ACCESS_READ)
                    result["native_memory_at_terminal"] = []
                    for read in memory_reads:
                        address = read.get("address")
                        address = int(address, 0) if isinstance(address, str) else int(address)
                        size = int(read["size"])
                        if 0x1F800000 <= address <= 0x1F8003FF:
                            region = "scratchpad"
                            offset = address - 0x1F800000
                        else:
                            region = "dram"
                            offset = address & 0x1FFFFF
                        result["native_memory_at_terminal"].append({
                            "address": f"0x{address:08X}",
                            "size": size,
                            "data": mappings[region][offset:offset + size].hex(),
                        })
                finally:
                    for mapping in mappings.values():
                        mapping.close()
            if not alignment["aligned"] and not request.get("continue_original_on_divergence"):
                return result
            remote = session.original_adapter.remote
            for address in addresses:
                reply = remote.packet(f"Z0,{address:x},4")
                if reply != "OK":
                    raise RuntimeError(f"Instruction breakpoint failed: {reply}")
                installed_addresses.append(address)
            for address, size in watchpoints:
                reply = remote.packet(f"Z2,{address & 0x1FFFFFFF:x},{size:x}")
                if reply != "OK":
                    raise RuntimeError(f"Data watchpoint failed: {reply}")
                installed_watchpoints.append((address, size))
            for _ in range(max_stops):
                stop = remote.packet("c", timeout=30.0)
                if not stop.startswith(("S", "T")):
                    raise RuntimeError(f"Unexpected DuckStation probe stop: {stop}")
                result["stop_counts"]["total"] += 1
                pending = remote.packet("qXportLockstepNextEvent")
                if pending != "NONE":
                    result["stop_counts"]["semantic_event"] += 1
                    event = session.original_adapter._consume_pending(pending)
                    result["last_semantic_event"] = {
                        key: event.get(key) for key in
                        ("kind", "frame", "function", "event_sequence")
                    }
                    if until and all(event.get(key) == value for key, value in until.items()):
                        result["terminal_event"] = event
                        if memory_reads:
                            result["terminal_memory"] = []
                            for read in memory_reads:
                                address = read.get("address")
                                address = int(address, 0) if isinstance(address, str) else int(address)
                                size = int(read["size"])
                                result["terminal_memory"].append({
                                    "address": f"0x{address:08X}",
                                    "size": size,
                                    "data": remote.read_memory(address & 0x1FFFFFFF, size).hex(),
                                })
                        result["complete"] = True
                        break
                    continue
                registers = decode_registers(remote.packet("g"))
                pc = int(registers.get("pc", "0"), 16)
                hit_kind = diagnostic_stop_kind(stop, pc, addresses, watchpoints)
                if hit_kind is None:
                    result["stop_counts"]["other"] += 1
                    continue
                counter = ("data_watchpoint" if hit_kind == "watchpoint" else
                           "instruction_breakpoint")
                result["stop_counts"][counter] += 1
                if any(registers.get(name) != value for name, value in register_filter.items()):
                    continue
                hit = {
                    "kind": hit_kind,
                    "pc": registers.get("pc"),
                    "registers": registers,
                }
                if capture_gte:
                    hit["gte_registers"] = {name: value for name, value in registers.items()
                                            if name.startswith("gte_")}
                if memory_reads:
                    hit["memory"] = []
                    for read in memory_reads:
                        address = read.get("address")
                        address = int(address, 0) if isinstance(address, str) else int(address)
                        size = int(read["size"])
                        physical = address & 0x1FFFFFFF
                        hit["memory"].append({
                            "address": f"0x{address:08X}",
                            "size": size,
                            "data": remote.read_memory(physical, size).hex(),
                        })
                result["hits"].append(hit)
                if stop_after_hits and len(result["hits"]) >= stop_after_hits:
                    result["complete"] = True
                    result["terminal_reason"] = "hit_limit"
                    break
            else:
                result["error"] = "probe_stop_limit"
            if capture_gte and native_entry is not None:
                result["gte_entry_comparison"] = compare_paired_gte_entry(result)
        finally:
            cleanup_error = None
            if session is not None and checkpoint is not None:
                remote = session.original_adapter.remote
                for address, size in reversed(installed_watchpoints):
                    try:
                        if remote.packet(f"z2,{address & 0x1FFFFFFF:x},{size:x}") != "OK":
                            cleanup_error = "Instruction probe watchpoint cleanup failed"
                    except Exception as error:
                        cleanup_error = str(error)
                for address in reversed(installed_addresses):
                    try:
                        if remote.packet(f"z0,{address:x},4") != "OK":
                            cleanup_error = "Instruction probe breakpoint cleanup failed"
                    except Exception as error:
                        cleanup_error = str(error)
            if session is not None and checkpoint is None:
                session.close()
            if native_process is not None:
                transcript = self.root / "status" / "lockstep-converge" / self.name / \
                    "native-probe.log"
                transcript.write_text(native_process.output[-1024 * 1024:], encoding="utf-8")
                native_process.close()
            if original_process is not None:
                original_process.close()
            self.set_target_frame(previous_target)
            if cleanup_error is not None:
                raise RuntimeError(cleanup_error)
        return result

    def paired_memory_probe(self, request: dict, record: dict, progress=None,
                            checkpoint: dict | None = None) -> dict:
        target_frame = int(request.get("target_frame", 0))
        anchor = request.get("anchor") or {}
        until = request.get("until") or {}
        reads = request.get("memory_reads") or []
        max_steps = int(request.get("max_steps", 2000))
        expected_difference = request.get("expected_difference") or {}
        if (not isinstance(until, dict) or not all(key in until for key in
                ("kind", "frame", "function", "callsite")) or
                not isinstance(expected_difference, dict) or
                not all(expected_difference.get(key) is not None for key in
                        ("region", "offset", "original", "candidate")) or
                not isinstance(reads, list) or not 1 <= len(reads) <= 16 or
                not 1 <= max_steps <= 100000):
            raise ValueError("Paired memory probe request is invalid")
        ranges = []
        total = 0
        for read in reads:
            if not isinstance(read, dict):
                raise ValueError("Paired memory probe range is invalid")
            raw = read.get("address")
            address = int(raw, 0) if isinstance(raw, str) else int(raw)
            size = int(read.get("size", 0))
            if 0x1F800000 <= address < 0x1F800400:
                region, offset, limit = "scratchpad", address - 0x1F800000, 0x400
            elif ((address & 0xE0000000) in (0, 0x80000000, 0xA0000000) and
                  (address & 0x1FFFFFFF) < 0x200000):
                region, offset, limit = "dram", address & 0x1FFFFF, 0x200000
            else:
                raise ValueError("Paired memory probe address is outside guest RAM")
            if size < 1 or size > 256 or offset + size > limit:
                raise ValueError("Paired memory probe range is invalid")
            total += size
            ranges.append((address, size, region, offset))
        if total > 4096:
            raise ValueError("Paired memory probe exceeds 4096 bytes")

        previous_target = self.target_frame
        self.set_target_frame(target_frame)
        run_id = checkpoint["run_id"] if checkpoint is not None else uuid.uuid4().hex
        original_process = native_process = session = None
        try:
            if checkpoint is not None:
                if target_frame < checkpoint["frame"]:
                    raise RuntimeError("Paired probe target precedes retained checkpoint")
                native_process, session = self.resume_checkpoint(
                    checkpoint["session"].original_adapter, run_id, record,
                    checkpoint["frame"], checkpoint["path"])
            else:
                original_process = self.launch_original(run_id, record)
                native_process = self.launch_native(run_id, record)
                session = self.attach(original_process, native_process, run_id, record)
            alignment = align_refinement(session, anchor, target_frame, progress=progress,
                                         allow_ranged=True)
            if not alignment["aligned"]:
                return {"schema": 1, "complete": False, "alignment": alignment}
            for steps in range(1, max_steps + 1):
                step = session.scheduler.step()
                event = step.get("original") or {}
                if progress is not None and event.get("kind") == "VBLANK":
                    progress(int(event["frame"]) + 1)
                if all(event.get(key) == value for key, value in until.items()):
                    if step.get("status") != "memory_divergence" or any(
                            (step.get("comparison") or {}).get(key) != value
                            for key, value in expected_difference.items()):
                        raise RuntimeError("Bound diagnostic event did not reproduce divergence")
                    mappings = {}
                    try:
                        for region in {row[2] for row in ranges}:
                            length = 0x200000 if region == "dram" else 0x400
                            mappings[region] = mmap.mmap(
                                -1, length, tagname=mapping_name(run_id, region),
                                access=mmap.ACCESS_READ)
                        samples = []
                        for address, size, region, offset in ranges:
                            original = session.original_adapter.remote.read_memory(
                                address & 0x1FFFFFFF, size)
                            native = mappings[region][offset:offset + size]
                            samples.append({
                                "address": f"0x{address:08X}", "size": size,
                                "original": original.hex(), "native": native.hex(),
                                "different_offsets": [index for index in range(size)
                                                      if original[index] != native[index]],
                            })
                    finally:
                        for mapping in mappings.values():
                            mapping.close()
                    return {"schema": 1, "complete": True, "target_frame": target_frame,
                            "alignment": alignment, "terminal": step, "steps": steps,
                            "samples": samples}
                if step.get("status") != "matched":
                    return {"schema": 1, "complete": False, "target_frame": target_frame,
                            "alignment": alignment, "earlier_divergence": step,
                            "steps": steps}
            return {"schema": 1, "complete": False, "target_frame": target_frame,
                    "alignment": alignment, "error": "paired_probe_step_limit"}
        finally:
            if session is not None and checkpoint is None:
                session.close()
            if native_process is not None:
                native_process.close()
            if original_process is not None:
                original_process.close()
            self.set_target_frame(previous_target)

    def refine(self, result: dict, record: dict, progress=None,
               checkpoint: dict | None = None, stop_after_writes: int = 0) -> dict:
        if stop_after_writes < 0 or stop_after_writes > 16:
            raise ValueError("Refinement writer stop limit is invalid")
        if self.native_controller is not None:
            transcript = self.root / "status" / "lockstep-converge" / self.name / "native-attempt.log"
            transcript.write_text(self.native_controller.output[-1024 * 1024:], encoding="utf-8")
        request = refinement_request(result)
        branch_slice = []
        original = result.get("original") or {}
        original_writes = []
        native_writes = []
        pointer_evidence = {}
        interval_complete = False
        refinement_target = refinement_target_frame(result)
        previous_target = self.target_frame
        self.set_target_frame(refinement_target)
        if request.kind == "control":
            branch_slice.append({
                "address": original.get("callsite"),
                "instruction": original.get("instruction"),
                "delay_slot_address": original.get("delay_slot_pc"),
                "delay_slot": original.get("delay_slot"),
            })
            run_id = checkpoint["run_id"] if checkpoint is not None else uuid.uuid4().hex
            original_process = native_process = session = None
            try:
                if checkpoint is not None:
                    native_process, session = self.resume_checkpoint(
                        checkpoint["session"].original_adapter, run_id, record,
                        checkpoint["frame"], checkpoint["path"])
                else:
                    original_process = self.launch_original(run_id, record)
                    native_process = self.launch_native(run_id, record)
                    session = self.attach(original_process, native_process, run_id, record)
                anchor = ((result.get("last_match") or {}).get("original") or {})
                alignment = align_refinement(
                    session, anchor, refinement_target, progress=progress)
                pointer_evidence["refinement_alignment"] = alignment
                if alignment["aligned"]:
                    pointer_evidence["original_gte_control"] = \
                        session.original_adapter.gte_control_registers()
                    pointer_evidence["native_gte_snapshot"] = native_process.gte_snapshot()
                    interval_complete = True
            finally:
                if session is not None and checkpoint is None:
                    session.close()
                if native_process is not None:
                    transcript = self.root / "status" / "lockstep-converge" / self.name / \
                        "native-refinement.log"
                    transcript.write_text(native_process.output[-1024 * 1024:], encoding="utf-8")
                    native_process.close()
                if original_process is not None:
                    original_process.close()
                self.set_target_frame(previous_target)
        elif request.kind == "memory":
            difference = result["comparison"]
            region = difference["region"]
            offset = difference["offset"]
            watch_offset = offset & ~3
            watch_length = min(4, (0x200000 if region == "dram" else 0x400) - watch_offset)
            run_id = checkpoint["run_id"] if checkpoint is not None else uuid.uuid4().hex
            original_process = native_process = session = None
            try:
                if checkpoint is not None:
                    native_process, session = self.resume_checkpoint(
                        checkpoint["session"].original_adapter, run_id, record,
                        checkpoint["frame"], checkpoint["path"])
                else:
                    original_process = self.launch_original(run_id, record)
                    native_process = self.launch_native(
                        run_id, record,
                        writer_target={"region": region, "offset": watch_offset,
                                       "length": watch_length})
                    session = self.attach(original_process, native_process, run_id, record)
                original_complete = False
                native_complete = False
                address = watch_offset if region == "dram" else 0x1f800000 + watch_offset
                anchor = ((result.get("last_match") or {}).get("original") or {})
                alignment = align_refinement(
                    session, anchor, refinement_target, progress=progress,
                    allow_ranged=True)
                pointer_evidence["refinement_alignment"] = alignment
                if alignment["aligned"]:
                    pointer_evidence["original_dma_begin"] = session.original_adapter.dma_registers()
                    pointer_evidence["original_gte_control"] = \
                        session.original_adapter.gte_control_registers()
                    matched = result.get("last_match") or {}
                    if matched.get("native", {}).get("kind") == "PRE_CALL":
                        pointer_evidence["native_gte_snapshot"] = native_process.gte_snapshot()
                    original_arguments = (matched.get("original") or {}).get("arguments") or []
                    native_arguments = (matched.get("native") or {}).get("arguments") or []
                    bounded_arguments = []
                    followed_addresses = set()
                    followed_pointers = []
                    for index, value in enumerate(original_arguments):
                        descriptor = native_arguments[index] if index < len(native_arguments) else {}
                        physical = value & 0x1fffffff
                        typed = descriptor.get("kind") == 2
                        if typed:
                            size = min(int(descriptor.get("bytes", 0)), 64)
                        elif ((value & 0xe0000000) in {0x80000000, 0xa0000000} and
                              physical < 0x200000):
                            size = min(64, 0x200000 - physical)
                        else:
                            continue
                        if size < 1:
                            continue
                        data = session.original_adapter.remote.read_memory(physical, size)
                        bounded_arguments.append({
                            "address": f"0x{value & 0xffffffff:08x}", "bytes": size,
                            "data": data.hex(), "binding": "typed" if typed else "inferred_kseg",
                        })
                        for position in range(0, max(0, size - 3), 4):
                            pointer = int.from_bytes(data[position:position + 4], "little")
                            pointer_physical = pointer & 0x1fffffff
                            if not pointer_physical or pointer_physical >= 0x200000 or pointer_physical in followed_addresses:
                                continue
                            followed_addresses.add(pointer_physical)
                            followed_pointers.append({
                                "source": f"0x{value & 0xffffffff:08x}+0x{position:x}",
                                "address": f"0x{pointer:08x}", "bytes": 64,
                                "data": session.original_adapter.remote.read_memory(pointer_physical, 64).hex(),
                            })
                            if len(followed_pointers) == 8:
                                break
                        if len(followed_pointers) == 8:
                            break
                    pointer_evidence["original_pointer_arguments"] = bounded_arguments
                    pointer_evidence["original_followed_pointers"] = followed_pointers
                    session.original_adapter.install_data_watchpoint(
                        region, watch_offset, watch_length)
                    native_process.set_data_watchpoint(region, watch_offset, watch_length)
                    original_terminal = result.get("original") or {}
                    native_terminal = result.get("native") or {}
                    original_before = session.original_adapter.remote.read_memory(
                        address, watch_length).hex()
                    for _ in range(100_000):
                        original_stop = session.original_adapter.advance_refinement(30.0)
                        pointer_evidence["original_last_stop"] = original_stop
                        if original_stop.get("stop") == "boundary":
                            if _same_boundary(original_stop.get("event") or {}, original_terminal):
                                original_complete = True
                                pointer_evidence["original_stop_reason"] = "terminal_boundary"
                                break
                            continue
                        if original_stop.get("stop") != "watchpoint":
                            pointer_evidence["original_stop_reason"] = "unexpected_stop"
                            break
                        after = session.original_adapter.remote.read_memory(address, watch_length).hex()
                        if after == original_before:
                            pointer_evidence["original_unchanged_watch_hits"] = \
                                pointer_evidence.get("original_unchanged_watch_hits", 0) + 1
                            continue
                        if len(original_writes) >= 16:
                            pointer_evidence["original_writer_limit_reached"] = True
                            break
                        original_writes.append({
                            "region": region, "offset": watch_offset, "width": watch_length,
                            "pc": original_stop.get("pc"), "before": original_before, "after": after,
                            "instruction_word": original_stop.get("instruction_word"),
                            "stop_pc": original_stop.get("stop_pc"),
                            "sp": original_stop.get("registers", {}).get("r29"),
                            "ra": original_stop.get("registers", {}).get("r31"),
                            "source": "gdb_data_watchpoint",
                        })
                        original_before = after
                        if stop_after_writes and len(original_writes) >= stop_after_writes:
                            pointer_evidence["original_stop_reason"] = "writer_limit"
                            break
                    else:
                        pointer_evidence["original_stop_reason"] = "boundary_limit"
                    pointer_evidence["original_dma_end"] = session.original_adapter.dma_registers()
                    for _ in range(100_000):
                        native_process.continue_execution()
                        native_stop = native_process.next_event(30.0)
                        pointer_evidence["native_last_stop"] = native_stop
                        if native_stop.get("stop") != "watchpoint":
                            if _same_boundary(native_stop, native_terminal):
                                native_complete = True
                                pointer_evidence["native_stop_reason"] = "terminal_boundary"
                                break
                            if native_stop.get("kind") in {"PRE_CALL", "POST_CALL", "VBLANK"}:
                                continue
                            pointer_evidence["native_stop_reason"] = "unexpected_stop"
                            break
                        if len(native_writes) >= 16:
                            pointer_evidence["native_writer_limit_reached"] = True
                            break
                        native_write = {
                            "region": region, "offset": native_stop["offset"],
                            "width": native_stop["width"],
                            "pc": native_stop["pc"], "before": native_stop["before"],
                            "after": native_stop["after"], "caller_pc": native_stop.get("caller_pc"),
                            "caller2_pc": native_stop.get("caller2_pc"),
                            "caller_symbol": native_stop.get("caller_symbol"),
                            "caller2_symbol": native_stop.get("caller2_symbol"),
                            "caller3_symbol": native_stop.get("caller3_symbol"),
                            "caller4_symbol": native_stop.get("caller4_symbol"),
                            "source": "cdb_data_watchpoint",
                        }
                        if native_stop["before"] == native_stop["after"]:
                            pointer_evidence["native_unchanged_watch_hits"] = \
                                pointer_evidence.get("native_unchanged_watch_hits", 0) + 1
                            native_process.set_data_watchpoint(
                                region, watch_offset, watch_length)
                            continue
                        native_writes.append(native_write)
                        if stop_after_writes and len(native_writes) >= stop_after_writes:
                            pointer_evidence["native_stop_reason"] = "writer_limit"
                            break
                        native_process.set_data_watchpoint(region, watch_offset, watch_length)
                    else:
                        pointer_evidence["native_stop_reason"] = "boundary_limit"
                    interval_complete = original_complete and native_complete
                    try:
                        pointer_evidence["native_writer_ring"] = native_process.writer_ring()
                    except CdbError as error:
                        pointer_evidence["native_writer_ring"] = {"available": False,
                                                                   "error": str(error)}
                    pointer_evidence["writer_timeline"] = correlate_writers(
                        original_writes, native_writes)
                    if interval_complete:
                        pointer_evidence["original_scratchpad_at_terminal"] = (
                            session.original_adapter.remote.read_memory(0x1F800000, 0x400).hex())
                        with mmap.mmap(-1, 0x400, tagname=mapping_name(run_id, "scratchpad"),
                                       access=mmap.ACCESS_READ) as scratchpad:
                            pointer_evidence["native_scratchpad_at_terminal"] = scratchpad[:].hex()
            finally:
                if session is not None and checkpoint is None:
                    session.close()
                if native_process is not None:
                    transcript = self.root / "status" / "lockstep-converge" / self.name / \
                        "native-refinement.log"
                    transcript.write_text(native_process.output[-1024 * 1024:], encoding="utf-8")
                    native_process.close()
                if original_process is not None:
                    original_process.close()
                self.set_target_frame(previous_target)
        return {
            "schema": 1, "contract": "xport-lockstep-refinement-v1",
            "request": {
                "kind": request.kind, "begin_event": request.begin_event,
                "end_event": request.end_event, "probes": list(request.probes),
            },
            "interval_complete": interval_complete,
            "original_writes": original_writes, "native_writes": native_writes,
            "branch_slice": branch_slice, "native_location": None,
            "pointer_evidence": pointer_evidence, "argument_location": None,
            "argument_mismatch": False, "target_resolution_complete": False,
        }

    def save_native_transcript(self, name: str) -> str | None:
        if self.native_controller is None:
            return None
        if not name or any(character not in "abcdefghijklmnopqrstuvwxyz0123456789-_" for character in name):
            raise ValueError("Native transcript name is invalid")
        path = self.root / "status" / "lockstep-converge" / self.name / f"native-{name}.log"
        path.write_text(self.native_controller.output[-1024 * 1024:], encoding="utf-8")
        return str(path)

    def build_packet(self, result: dict, refinement: dict, build: dict) -> dict:
        if self.duckstation_runtime is None:
            raise RuntimeError('DuckStation runtime identity is unavailable')
        shared_identity = build["shared_source_identity"]
        if shared_identity != self.shared_source_identity():
            raise RuntimeError("Shared xport source changed after Debug build")
        identities = {
            "record_id": self.record["record_id"],
            "original_executable_sha256": self.record["original"]["executable"]["sha256"],
            "native_executable_sha256": build["binary_sha256"],
            "native_pdb_sha256": build["pdb_sha256"],
            "native_source_identity": build["source_identity"],
            "shared_source_identity": shared_identity,
            "duckstation_patch_sha256": self.duckstation_runtime['patch_sha256'],
            "exceptions_identity": exceptions_identity(self.exceptions),
        }
        reproduce = ["python", "-B", str(Path(__file__).with_name("xport.py")), "--project",
                     str(self.root), "trace_workflow", "converge", self.record_name,
                     "--wait", "30"]
        if self.name != self.record_name:
            reproduce.extend(("--run-name", self.name))
        return build_packet(result, refinement, identities, self.exceptions, reproduce)

    def request_repair(self, packet: dict, packet_path: Path) -> dict:
        shared = (packet.get("identities") or {}).get("shared_source_identity")
        if shared is not None and shared != self.shared_source_identity():
            return {"status": "attention_required", "reason": "shared_source_changed",
                    "model_invocations": 0}
        if self.lockstep.get("repair_executor") == "openai_api":
            from lockstep_repair_openai import execute
            return execute(self.root, self.config, packet_path)
        return {"status": "attention_required", "reason": "model_repair_binding_pending",
                "model_invocations": 0}

    def validate_repair_audit(self, packet_path: Path | None, source: dict,
                              changed: set[str] | None) -> bool:
        if packet_path is None:
            raise RuntimeError("Lockstep repair audit has no attention packet")
        receipt_path = packet_path.with_suffix(".audit.json")
        if not receipt_path.is_file():
            return False
        packet = json.loads(packet_path.read_text(encoding="utf-8"))
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        if receipt.get("schema") != 1 or receipt.get("contract") != "xport-lockstep-function-audit-v1":
            raise RuntimeError("Unsupported lockstep function audit receipt")
        if receipt.get("failure_signature") != packet.get("failure_signature"):
            raise RuntimeError("Lockstep function audit receipt has the wrong failure signature")
        if receipt.get("source_identity") != source.get("identity"):
            raise RuntimeError("Lockstep function audit receipt does not bind the repaired source")
        shared = (packet.get("identities") or {}).get("shared_source_identity")
        shared_root = self.shared_source_root() if shared is not None else None
        shared_snapshot = (source_snapshot(shared_root, (Path('src'),))
                           if shared_root is not None else None)
        if shared is not None:
            if receipt.get("shared_source_identity") != shared_snapshot['identity']:
                raise RuntimeError("Function audit receipt lacks the shared source identity")
            if shared != shared_snapshot['identity']:
                baseline = receipt.get('shared_source_before_files')
                if (not isinstance(baseline, dict) or
                        receipt.get('shared_source_before_identity') != shared or
                        hashlib.sha256(json.dumps(
                            baseline, sort_keys=True, separators=(',', ':')).encode('utf-8')
                        ).hexdigest() != shared or
                        set(receipt.get('changed_shared_files') or []) != changed_sources(
                            {'files': baseline}, shared_snapshot)):
                    raise RuntimeError('Shared xport repair lacks a complete source transition')
            elif receipt.get('changed_shared_files'):
                raise RuntimeError('Function audit claims unchanged shared source was edited')
        receipt_files = set(receipt.get("changed_files") or [])
        if changed is not None and receipt_files != changed:
            raise RuntimeError("Lockstep function audit receipt has the wrong changed files")
        audits = receipt.get("function_audits")
        if not isinstance(audits, list):
            raise RuntimeError("Lockstep function audit receipt lacks function_audits")
        indexed = {
            (item.get("image"), item.get("address")): item
            for item in audits if isinstance(item, dict)
        }
        source_context = packet.get("source_context") or {}
        contexts = [source_context, *(source_context.get("audit_cluster") or [])]
        if source_context.get("writer_library"):
            contexts.append(source_context["writer_library"])
        indexed_context = {
            ((item.get("function") or {}).get("image"),
             (item.get("function") or {}).get("address")): item
            for item in contexts if isinstance(item, dict)
        }
        required = (source_context.get("audit_contract") or {}).get(
            "required_functions") or []
        source_function_index = None
        all_source_files = dict(source.get('files') or {})
        if shared_snapshot is not None:
            all_source_files.update({'@xport/' + name: digest for name, digest in
                                     shared_snapshot['files'].items()})
        for binding in required:
            audit = indexed.get((binding.get("image"), binding.get("address")))
            if audit is None:
                raise RuntimeError("Lockstep function audit receipt misses a required function")
            for key in ("function_sha256", "mips_sha256", "pseudocode_sha256"):
                if audit.get(key) != binding.get(key):
                    raise RuntimeError(f"Lockstep function audit receipt has the wrong {key}")
            machine_hash = binding.get("machine_audit_sha256")
            if machine_hash:
                from lockstep_audit import checklist
                context = indexed_context.get((binding.get("image"), binding.get("address")))
                if context is None:
                    raise RuntimeError("Lockstep machine audit context is missing")
                machine = checklist((context.get("mips") or {}).get("lines") or [])
                if machine["sha256"] != machine_hash or audit.get("machine_audit_sha256") != machine_hash:
                    raise RuntimeError("Lockstep machine audit hash mismatch")
                expected = {item["id"] for item in machine["checks"]}
                implementation = context.get("audit_source") or context.get("implementation") or {}
                if context is source_context.get("writer_library") and context.get("audit_source"):
                    owner = source_context.get("implementation") or {}
                    if any(implementation.get(name) != owner.get(name)
                           for name in ("path", "symbol")):
                        raise RuntimeError("Writer library audit source differs from owner")
                if context.get("audit_role") == "inlined_callee":
                    owner = source_context.get("implementation") or {}
                    if not context.get("audit_source") or any(
                            implementation.get(name) != owner.get(name)
                            for name in ("path", "symbol")):
                        raise RuntimeError("Inlined callee audit source differs from owner")
                implementation_path = implementation.get("path")
                implementation_symbol = implementation.get("symbol")
                reachable = None
                if implementation_path and implementation_symbol:
                    from lockstep_source_coverage import index_functions, reachable_functions

                    if source_function_index is None:
                        source_function_index = index_functions(self.root, source["files"])
                        if shared_snapshot is not None:
                            shared_index = index_functions(shared_root, shared_snapshot['files'])
                            source_function_index.update({
                                ('@xport/' + path, symbol):
                                {**row, 'path': '@xport/' + path}
                                for (path, symbol), row in shared_index.items()
                            })
                    reachable = reachable_functions(
                        source_function_index, implementation_path, implementation_symbol)
                covered = []
                for group in audit.get("machine_coverage") or []:
                    ids = group.get("check_ids") if isinstance(group, dict) else None
                    check_range = group.get("check_range") if isinstance(group, dict) else None
                    if check_range is not None:
                        if (ids is not None or not isinstance(check_range, list) or
                                len(check_range) != 2 or
                                any(type(value) is not int for value in check_range) or
                                not 0 <= check_range[0] <= check_range[1] < len(machine["checks"]) or
                                check_range[1] - check_range[0] >= 8):
                            raise RuntimeError("Invalid MIPS-to-C coverage range")
                        ids = [item["id"] for item in
                               machine["checks"][check_range[0]:check_range[1] + 1]]
                    name = group.get("source_file") if isinstance(group, dict) else None
                    line = group.get("source_line") if isinstance(group, dict) else None
                    if (not isinstance(ids, list) or not 1 <= len(ids) <= 8 or
                            not all(isinstance(item, str) for item in ids) or
                            not isinstance(name, str) or name not in all_source_files or
                            type(line) is not int or line < 1):
                        raise RuntimeError("Invalid MIPS-to-C coverage mapping")
                    is_shared = name.startswith('@xport/')
                    owner = shared_root if is_shared else self.root
                    relative = name[len('@xport/'):] if is_shared else name
                    path = (owner / relative).resolve()
                    if not path.is_relative_to(owner) or not path.is_file():
                        raise RuntimeError("MIPS-to-C mapping escapes project source")
                    source_lines = path.read_text(encoding="utf-8").splitlines()
                    source_text = source_lines[line - 1].strip() if line <= len(source_lines) else ""
                    if (not source_text or source_text in {"{", "}", "};"} or
                            "FUNCTION_MARKER" in source_text or
                            "XPORT_LOCKSTEP_BOUNDARY" in source_text):
                        raise RuntimeError("MIPS-to-C mapping names an empty or marker-only source line")
                    if reachable is not None:
                        from lockstep_source_coverage import validate_location

                        symbol = group.get("source_symbol")
                        if (not isinstance(symbol, str) or not validate_location(
                                source_function_index, reachable, name, symbol, line)):
                            raise RuntimeError(
                                "MIPS-to-C mapping is outside the translated function or a reachable helper")
                    covered.extend(ids)
                if len(covered) != len(expected) or set(covered) != expected:
                    raise RuntimeError("MIPS-to-C coverage omits or duplicates a machine check")
            expected_pseudocode = binding.get("pseudocode_complete", True) is True
            if (audit.get("pseudocode_used_as_baseline") is not expected_pseudocode or
                    audit.get("complete_mips_audit") is not True or
                    audit.get("unresolved_semantics") != []):
                raise RuntimeError("Lockstep function audit receipt is incomplete")
        return True

    def refresh_after_repair(self) -> None:
        roots = tuple(Path(value) for value in self.lockstep.get("source_roots", ["src"]))
        source_before = source_snapshot(self.root, roots)["identity"]
        command = [sys.executable, "-B", str(Path(__file__).with_name("source_index.py")),
                   "--rebuild", "--no-format"]
        result = subprocess.run(
            command, cwd=self.root, env=normalized_environment(), capture_output=True, text=True,
        )
        log = self.root / "status" / "lockstep-converge" / self.name / "source-index.log"
        log.write_text(result.stdout + result.stderr, encoding="utf-8")
        if result.returncode:
            raise RuntimeError("source_index failed after model repair")
        if source_snapshot(self.root, roots)["identity"] != source_before:
            raise RuntimeError("source_index changed source after audit validation")

    def finalize_match(self, result: dict, build: dict) -> dict:
        release = self.build_release()
        return {"status": "MATCH", "record_id": self.record["record_id"],
                "proof_scope": "recorded_interval_from_initial_save_state",
                "debug_build": build, "release_build_started": True,
                "release_build": release}

    def build_release(self) -> dict:
        from code_refresh import process_running

        solution = (self.root / self.config["paths"]["native_solution"]).resolve()
        executable = self.native_executable("Release")
        if process_running(executable.name):
            raise RuntimeError("Release executable is running; preserve manual game")
        msbuild = self._msbuild_executable()
        command = [str(msbuild), str(solution), "/t:Rebuild", "/p:Configuration=Release",
                   "/p:Platform=x86", "/m:1", "/nr:false", "/v:minimal", "/nologo"]
        shared_identity = self.shared_source_identity()
        result = subprocess.run(command, cwd=self.root, env=normalized_environment(),
                                capture_output=True, text=True)
        log = self.root / "status" / "lockstep-converge" / self.name / "release-build.log"
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text(result.stdout + result.stderr, encoding="utf-8")
        if result.returncode or not executable.is_file():
            raise RuntimeError("Final Release x86 build failed")
        if self.shared_source_identity() != shared_identity:
            raise RuntimeError("Shared xport source changed during Release build")
        return {
            "configuration": "Release", "platform": "x86", "lockstep_debug": False,
            "binary": str(executable), "binary_sha256": digest(executable), "log": str(log),
            "shared_source_identity": shared_identity,
        }

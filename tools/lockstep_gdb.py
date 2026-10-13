#!/usr/bin/env python3
"""DuckStation GDB adapter for the xport lockstep scheduler"""

from __future__ import annotations

import json
import mmap
import re
import struct
import time
from gdb_remote import Remote, decode_registers
from lockstep_contract import (exceptions_identity, parse_compare_reply, validate_event,
                               validate_exceptions, validate_peer)
from lockstep_record import CONTROLLER_TYPES, validate_record


class LockstepGdbError(RuntimeError):
    pass


class DuckStationLockstep:
    def __init__(self, host: str, port: int, *, runtime_identity: dict | None = None):
        self.remote = Remote(host, port)
        self.attached = False
        self.exceptions_identity: str | None = None
        self.run_id: str | None = None
        self.mode: str | None = None
        self.record: dict | None = None
        self.input_index = 0
        self.watchpoints: list[tuple[int, int]] = []
        self.argument_registers: dict[int, tuple[int, ...]] = {}
        self.runtime_identity = runtime_identity

    def _bind_event_image(self, event: dict) -> dict:
        runtime = self.runtime_identity
        # Protocol 23 emits a fixed transport label rather than an executable identity
        if not runtime or runtime.get("patch_sha256") not in {
                "25a99c8a66cd7af93bd052414b1e96f8d5ef9b75be0d3176a347b3f0d234ba2b",
                "0c3155d5f8ad2854e87d9e2393b45f67cf2785b88a949f76b941561b4cddb069",
                "1c625bb0b3e7016945eb9f9f1ed72439ca3cce1d80abf4cb55bb3b21abd28207",
                "e53eb603936c6cf69505c85dceb65660992e798cdb7f5f48a991bff184c72c29"}:
            return event
        if runtime.get("lockstep_protocol_version") != 23:
            raise LockstepGdbError("Fixed image label has an incompatible protocol")
        if event["side"] != "original" or event["image"] != "MAIN.EXE":
            raise LockstepGdbError("DuckStation event disagrees with its fixed image label contract")
        if self.mode != "replay" or not self.record:
            raise LockstepGdbError("DuckStation image binding requires a validated replay record")
        event = dict(event)
        event["image"] = self.record["original"]["image"]
        event["source"] = dict(event.get("source") or {})
        event["source"]["transport_image"] = "MAIN.EXE"
        event["source"]["image_binding"] = {
            "record_id": self.record["record_id"],
            "executable_sha256": self.record["original"]["executable"]["sha256"],
            "patch_sha256": runtime["patch_sha256"],
        }
        return event

    def start_prelude(self, record: dict) -> None:
        validate_record(record)
        if self.mode is not None or self.attached:
            raise LockstepGdbError("DuckStation lockstep mode is already active")
        record_id = record["record_id"]
        self._expect(self.remote.packet(f"qXportLockstepReplay:{record_id}"),
                     f"OK:{record_id}", "prelude replay")
        self._expect(self.remote.packet("qXportLockstepPreludeStart"),
                     "OK:prelude", "prelude start")
        self.mode = "replay"
        self.record = record
        self.input_index = 0
        self._queue_next_input()

    def handoff_state(self) -> dict[str, int]:
        if self.mode != "replay" or self.attached:
            raise LockstepGdbError("DuckStation handoff state is unavailable")
        reply = self.remote.packet("qXportLockstepHandoffState")
        match = re.fullmatch(r"HANDOFF:([0-9a-f]{8}):([0-9a-f]{8}):([0-9a-f]+)", reply)
        if not match:
            raise LockstepGdbError(f"Invalid DuckStation handoff state: {reply}")
        state = {
            "timer1": int(match.group(1), 16),
            "gpu_status": int(match.group(2), 16),
            "field_lines": int(match.group(3), 16),
        }
        if state["timer1"] > 0xffff or state["field_lines"] not in (263, 314):
            raise LockstepGdbError("DuckStation handoff device state is unsupported")
        return state

    def timer_state(self) -> dict:
        if self.mode != "replay" or not self.remote.stopped:
            raise LockstepGdbError("DuckStation must be paused in replay for timer state")
        reply = self.remote.packet("qXportLockstepTimerState")
        fields = reply.split(":")
        if (len(fields) != 5 or fields[0] != "TIMERSTATE" or
                not all(re.fullmatch(r"[0-9a-fA-F]+", value) for value in fields[1:]) or
                len(fields[4]) != 118):
            raise LockstepGdbError(f"Malformed DuckStation timer state: {reply}")
        version, clock, pending = (int(value, 16) for value in fields[1:4])
        if version != 85 or clock > 0xffffffffffffffff or pending > 0x7fffffff:
            raise LockstepGdbError("Unsupported DuckStation timer state header")
        from duckstation_state85 import Reader, timers
        reader = Reader(bytes.fromhex(fields[4]))
        try:
            state = timers(reader)
            reader.end()
        except ValueError as error:
            raise LockstepGdbError("Malformed DuckStation timer serializer") from error
        if (state["sysclk_div8_carry"] >= 8 or
                any(row["mode"] > 0x1fff or row["counter"] > 0xffff or row["target"] > 0xffff
                    for row in state["counters"])):
            raise LockstepGdbError("Invalid DuckStation timer registers")
        return {"serializer_version": version, "cpu_clock": clock,
                "pending_sysclk_ticks": pending, **state}

    def beam_state(self) -> dict:
        if self.mode != "replay" or not self.remote.stopped:
            raise LockstepGdbError("DuckStation must be paused in replay for beam state")
        reply = self.remote.packet("qXportLockstepBeamState")
        fields = reply.split(":")
        if (len(fields) != 8 or fields[0] != "BEAMSTATE" or len(fields[7]) != 24 or
                not all(re.fullmatch(r"[0-9a-fA-F]+", value) for value in fields[1:])):
            raise LockstepGdbError(f"Malformed DuckStation beam state: {reply}")
        clock, frequency, tick, line, start, end = (int(value, 16) for value in fields[1:7])
        irq = dict(zip(("status", "mask", "lines"), struct.unpack("<3I", bytes.fromhex(fields[7]))))
        if (clock > 0xffffffffffffffff or frequency not in (53203425, 53693175) or
                tick >= 65536 or line >= 1024 or not 0 <= start < end <= 1024 or
                any(value & ~0x7ff for value in irq.values())):
            raise LockstepGdbError("Invalid DuckStation beam/IRQ state")
        return {"cpu_clock": clock, "gpu_frequency": frequency, "tick": tick, "line": line,
                "vertical_start": start, "vertical_end": end, "irq": irq}

    def attach_handoff(self, peer: dict, exceptions: dict,
                       boundary_targets: tuple[int, ...], start_frame: int,
                       target_frame: int,
                       tail_calls: tuple[tuple[int, int], ...] = (),
                       argument_registers: dict[int, tuple[int, ...]] | None = None) -> None:
        validate_peer(peer)
        validate_exceptions(exceptions)
        if (self.mode != "replay" or self.attached or not self.record or
                self.input_index != start_frame or not 0 < start_frame < len(self.record["frames"])):
            raise LockstepGdbError("DuckStation handoff state is invalid")
        reply = self.remote.packet(
            f"qXportLockstepAttach:{peer['run_id']}:{peer['dram']}:{peer['scratchpad']}:{peer['deferred']}"
        )
        self._expect(reply, f"OK:{peer['run_id']}", "handoff attach")
        identity = exceptions_identity(exceptions)
        ranges = ";".join(
            f"{row['region']},{row['offset']:x},{row['size']:x}" for row in exceptions["ranges"])
        self._expect(self.remote.packet(f"qXportLockstepExceptions:{identity}:{ranges}"),
                     f"OK:{identity}", "handoff exceptions")
        record_id = self.record["record_id"]
        self._expect(self.remote.packet(f"qXportLockstepSeed:{record_id}"),
                     f"OK:{record_id}", "handoff memory seed")
        mode = "PREFIX_GUARD" if target_frame > start_frame else "DETAILED"
        mode_target = target_frame if mode == "PREFIX_GUARD" else 0
        self._expect(self.remote.packet(f"qXportLockstepMode:{mode}:{mode_target:x}"),
                     f"OK:{mode}:{mode_target:x}", "handoff mode")
        self._expect(self.remote.packet(f"qXportLockstepStartAt:{start_frame:x}"),
                     f"OK:start_at:{start_frame:x}", "handoff start")
        if not boundary_targets:
            raise LockstepGdbError("Lockstep ABI frontier has no boundary targets")
        for target in boundary_targets:
            self._expect(self.remote.packet(f"qXportLockstepBoundary:{target:x}"),
                         "OK:boundary", "handoff boundary")
        for source, target in tail_calls:
            self._expect(self.remote.packet(f"qXportLockstepTail:{source:x}:{target:x}"),
                         "OK:tail", "handoff tail")
        self.attached = True
        self.exceptions_identity = identity
        self.run_id = peer["run_id"]
        self.argument_registers = argument_registers or {}

    def attach(self, peer: dict, exceptions: dict, record: dict,
               boundary_targets: tuple[int, ...],
               tail_calls: tuple[tuple[int, int], ...] = (), target_frame: int = 0,
               argument_registers: dict[int, tuple[int, ...]] | None = None,
               portable_frame: int | None = None) -> None:
        validate_peer(peer)
        validate_exceptions(exceptions)
        validate_record(record)
        if self.attached:
            raise LockstepGdbError("DuckStation lockstep peer is already attached")
        if portable_frame is not None:
            if not 0 <= portable_frame + 1 < len(record["frames"]):
                raise LockstepGdbError("Portable checkpoint frame is outside the record")
            self._expect(self.remote.packet(
                f"qXportLockstepCheckpointImport:{portable_frame:x}:prepare"),
                f"OK:{portable_frame:x}:prepared", "checkpoint preparation")
        reply = self.remote.packet(
            f"qXportLockstepAttach:{peer['run_id']}:{peer['dram']}:{peer['scratchpad']}:{peer['deferred']}"
        )
        self._expect(reply, f"OK:{peer['run_id']}", "attach")
        identity = exceptions_identity(exceptions)
        ranges = ";".join(
            f"{row['region']},{row['offset']:x},{row['size']:x}" for row in exceptions["ranges"])
        reply = self.remote.packet(f"qXportLockstepExceptions:{identity}:{ranges}")
        self._expect(reply, f"OK:{identity}", "exceptions")
        record_id = record["record_id"]
        reply = self.remote.packet(f"qXportLockstepReplay:{record_id}")
        self._expect(reply, f"OK:{record_id}", "replay")
        reply = self.remote.packet(f"qXportLockstepSeed:{record_id}")
        self._expect(reply, f"OK:{record_id}", "initial memory seed")
        mode = "PREFIX_GUARD" if target_frame else "DETAILED"
        reply = self.remote.packet(f"qXportLockstepMode:{mode}:{target_frame:x}")
        self._expect(reply, f"OK:{mode}:{target_frame:x}", "mode")
        self._expect(self.remote.packet("qXportLockstepStart"), "OK:start", "start")
        if not boundary_targets:
            raise LockstepGdbError("Lockstep ABI frontier has no boundary targets")
        for target in boundary_targets:
            self._expect(self.remote.packet(f"qXportLockstepBoundary:{target:x}"),
                         "OK:boundary", "boundary")
        for source, target in tail_calls:
            self._expect(self.remote.packet(f"qXportLockstepTail:{source:x}:{target:x}"),
                         "OK:tail", "tail")
        self.attached = True
        self.exceptions_identity = identity
        self.run_id = peer["run_id"]
        self.mode = "replay"
        self.record = record
        self.argument_registers = argument_registers or {}
        if portable_frame is not None:
            command = f"qXportLockstepCheckpointImport:{portable_frame:x}"
            if target_frame > portable_frame + 1:
                command += f":{target_frame:x}"
            self._expect(self.remote.packet(command), f"OK:{portable_frame:x}",
                         "checkpoint import")
            previous = record["frames"][portable_frame]
            ports = [
                f"{CONTROLLER_TYPES[port['type']]:x},{port['buttons']:x},{port['analog']:x}"
                for port in previous["ports"]
            ]
            self._expect(self.remote.packet(
                f"qXportLockstepPadRestore:{portable_frame:x}:{ports[0]}:{ports[1]}"),
                f"OK:{portable_frame:x}", "checkpoint controller restore")
            self.input_index = portable_frame + 1
        else:
            self.input_index = 0
        self._queue_next_input()

    def start_record(self, limit: int = 0) -> None:
        if self.mode is not None or self.attached:
            raise LockstepGdbError("DuckStation lockstep mode is already active")
        if limit < 0 or limit > 0xFFFFFFFF:
            raise LockstepGdbError("DuckStation record limit is invalid")
        command = f"qXportLockstepRecordStart:{limit:x}" if limit else "qXportLockstepRecordStart"
        self._expect(self.remote.packet(command), "OK:record", "record")
        self.mode = "record"

    def resume_record(self) -> None:
        if self.mode != "record" or not self.remote.stopped:
            raise LockstepGdbError("DuckStation record mode is not ready")
        self.remote.send_no_reply("c")

    def wait_record_limit(self, timeout: float) -> None:
        if self.mode != "record" or not self.remote.stopped:
            raise LockstepGdbError("DuckStation record mode is not ready")
        reply = self.remote.packet("c", timeout=timeout)
        if not reply.startswith(("S", "T")):
            raise LockstepGdbError(f"Unexpected DuckStation record stop reply: {reply}")

    def finish_record(self) -> list[dict]:
        if self.mode != "record":
            raise LockstepGdbError("DuckStation record mode is not active")
        if not self.remote.stopped:
            self.remote.interrupt()
        frames = []
        offset = 0
        total = None
        names = {0: "disconnected", 1: "digital", 2: "analog"}
        while total is None or offset < total:
            command = f"qXportLockstepRecordRead:{offset:x}:40"
            reply = self.remote.packet(command)
            if offset == 0:
                deadline = time.monotonic() + 2.0
                while reply == "ERROR:record:invalid_record_read" and time.monotonic() < deadline:
                    time.sleep(0.01)
                    reply = self.remote.packet(command)
            fields = reply.split(":", 3)
            if len(fields) != 4 or fields[0] != "RECORD":
                raise LockstepGdbError(f"Malformed DuckStation record reply: {reply}")
            try:
                returned_total = int(fields[1], 16)
                returned_offset = int(fields[2], 16)
            except ValueError as error:
                raise LockstepGdbError(f"Malformed DuckStation record header: {reply}") from error
            if returned_offset != offset or (total is not None and returned_total != total):
                raise LockstepGdbError("DuckStation record cursor changed during drain")
            total = returned_total
            rows = [] if not fields[3] else fields[3].split(";")
            if not rows and offset < total:
                raise LockstepGdbError("DuckStation returned an empty record page")
            for row in rows:
                try:
                    values = [int(value, 16) for value in row.split(",")]
                except ValueError as error:
                    raise LockstepGdbError(f"Malformed DuckStation record row: {row}") from error
                if len(values) != 7 or values[1] not in names or values[4] not in names:
                    raise LockstepGdbError(f"Malformed DuckStation record row: {row}")
                frames.append({
                    "side": "original", "kind": "VBLANK", "frame": values[0],
                    "controller_state": [
                        {"type": names[values[1]], "buttons": values[2], "analog": values[3]},
                        {"type": names[values[4]], "buttons": values[5], "analog": values[6]},
                    ],
                })
            offset += len(rows)
        return frames

    def _queue_next_input(self) -> None:
        if not self.record or self.input_index >= len(self.record["frames"]):
            return
        row = self.record["frames"][self.input_index]
        encoded = []
        for port in row["ports"]:
            encoded.append(f"{CONTROLLER_TYPES[port['type']]:x},{port['buttons']:x},{port['analog']:x}")
        reply = self.remote.packet(f"qXportLockstepInput:{row['frame']:x}:{encoded[0]}:{encoded[1]}")
        self._expect(reply, f"OK:{row['frame']:x}", "input")

    def capture_checkpoint(self, frame: int) -> int:
        if self.mode != "replay" or not self.remote.stopped or frame < 0:
            raise LockstepGdbError("DuckStation checkpoint capture requires a stopped replay")
        reply = self.remote.packet(f"qXportLockstepCheckpointCapture:{frame:x}")
        fields = reply.split(":")
        if len(fields) != 3 or fields[:2] != ["OK", f"{frame:x}"]:
            raise LockstepGdbError(f"DuckStation checkpoint capture failed: {reply}")
        try:
            size = int(fields[2], 16)
        except ValueError as error:
            raise LockstepGdbError(f"Malformed DuckStation checkpoint size: {reply}") from error
        if size <= 0 or size > 11 * 1024 * 1024:
            raise LockstepGdbError(f"DuckStation checkpoint size is invalid: {size}")
        return size

    def export_checkpoint(self, frame: int) -> int:
        if self.mode != "replay" or not self.remote.stopped or frame < 0:
            raise LockstepGdbError("DuckStation checkpoint export requires a captured replay")
        reply = self.remote.packet(f"qXportLockstepCheckpointExport:{frame:x}")
        fields = reply.split(":")
        if len(fields) != 3 or fields[:2] != ["OK", f"{frame:x}"]:
            raise LockstepGdbError(f"DuckStation checkpoint export failed: {reply}")
        try:
            size = int(fields[2], 16)
        except ValueError as error:
            raise LockstepGdbError(f"Malformed DuckStation checkpoint export: {reply}") from error
        if not 0 < size <= 11 * 1024 * 1024:
            raise LockstepGdbError("DuckStation checkpoint export size is invalid")
        return size

    def restore_checkpoint(self, frame: int, target_frame: int | None = None) -> None:
        if (self.mode != "replay" or not self.remote.stopped or not self.record or
                frame < 0 or frame + 1 >= len(self.record["frames"])):
            raise LockstepGdbError("DuckStation checkpoint restore requires a stopped replay")
        if target_frame is not None and not frame + 1 < target_frame < len(self.record["frames"]):
            raise LockstepGdbError("DuckStation prefix checkpoint target is invalid")
        if self.watchpoints:
            self.clear_data_watchpoints()
        command = f"qXportLockstepCheckpointRestore:{frame:x}"
        if target_frame is not None:
            command += f":{target_frame:x}"
        reply = self.remote.packet(command)
        self._expect(reply, f"OK:{frame:x}", "checkpoint restore")
        self.input_index = frame + 1
        self._queue_next_input()

    @staticmethod
    def _expect(reply: str, expected: str, operation: str) -> None:
        if reply != expected:
            raise LockstepGdbError(f"DuckStation lockstep {operation} failed: {reply}")

    def advance(self, timeout: float) -> dict:
        if self.mode not in {"record", "replay"}:
            raise LockstepGdbError("DuckStation lockstep mode is not active")
        for _ in range(2):
            reply = self.remote.packet("c", timeout=timeout)
            if not reply.startswith(("S", "T")):
                raise LockstepGdbError(f"Unexpected DuckStation stop reply: {reply}")
            pending = self.remote.packet("qXportLockstepNextEvent")
            if pending != "NONE":
                return self._consume_pending(pending)
            if not self.watchpoints:
                raise LockstepGdbError("DuckStation stopped without a lockstep event")
            registers = decode_registers(self.remote.packet("g"))
            instruction_pc, instruction_word = self.last_instruction()
            target = self.watch_target
            address = target["offset"] if target["region"] == "dram" else 0x1f800000 + target["offset"]
            after = self.remote.read_memory(address, 1).hex()
            self.watch_hits.append({
                "region": target["region"], "offset": target["offset"], "width": 1,
                "pc": instruction_pc, "instruction_word": instruction_word,
                "stop_pc": registers.get("pc"), "before": "unknown", "after": after,
                "ra": registers.get("r31"), "sp": registers.get("r29"),
                "source": "gdb_prearmed_watchpoint",
            })
            self.clear_data_watchpoints()
        raise LockstepGdbError("DuckStation did not reach a boundary after its watchpoint")

    def _consume_pending(self, pending: str) -> dict:
        if pending == "NONE":
            raise LockstepGdbError("DuckStation stopped without a lockstep event")
        if not pending.startswith("EVENT:"):
            raise LockstepGdbError(f"Malformed DuckStation event reply: {pending}")
        try:
            event = json.loads(bytes.fromhex(pending[6:]).decode("utf-8"))
        except (ValueError, UnicodeDecodeError, json.JSONDecodeError) as error:
            raise LockstepGdbError("DuckStation event payload is invalid") from error
        event = validate_event(event)
        event = self._bind_event_image(event)
        if self.mode == "replay" and event["kind"] == "PRE_CALL":
            registers_for_target = self.argument_registers.get(int(event["target"], 16))
            if registers_for_target is not None:
                registers = decode_registers(self.remote.packet("g"))
                original_arguments = event["arguments"]
                if len(original_arguments) != 4 or any(registers.get(f"r{index}") is None or
                       int(registers[f"r{index}"], 16) != original_arguments[index - 4]
                       for index in range(4, 8)):
                    raise LockstepGdbError("GDB registers disagree with the boundary argument snapshot")
                if any(registers.get(f"r{index}") is None for index in registers_for_target):
                    raise LockstepGdbError("GDB omitted a reviewed argument register")
                event = dict(event)
                event["arguments"] = [int(registers[f"r{index}"], 16)
                                      for index in registers_for_target]
        if self.mode == "replay" and event["kind"] == "FAULT":
            source = event.get("source") or {}
            if (source.get("error") == "missing_frame_input" and self.record and
                    self.input_index == len(self.record["frames"])):
                event = dict(event)
                event["kind"] = "PROCESS_EXIT"
                event["source"] = {key: source[key] for key in
                                   ("transport_image", "image_binding") if key in source}
                event["source"]["terminal"] = "record_complete"
        if self.mode == "replay" and event["kind"] == "VBLANK":
            if not self.record or self.input_index >= len(self.record["frames"]):
                raise LockstepGdbError("DuckStation produced an extra VBLANK")
            expected = self.record["frames"][self.input_index]
            if event["frame"] != expected["frame"] or event["controller_state"] != expected["ports"]:
                raise LockstepGdbError("DuckStation effective FrameDone input differs from the record")
            self.input_index += 1
            self._queue_next_input()
        return event

    @staticmethod
    def watchpoint_addresses(region: str, offset: int) -> tuple[int, ...]:
        if region == "dram" and 0 <= offset < 0x200000:
            return offset, 0x80000000 + offset, 0xA0000000 + offset
        if region == "scratchpad" and 0 <= offset < 0x400:
            return (0x1F800000 + offset, 0x9F800000 + offset, 0xBF800000 + offset)
        raise LockstepGdbError("Watchpoint is outside lockstep memory")

    def install_data_watchpoint(self, region: str, offset: int, length: int = 1,
                                *, precise: bool = False) -> None:
        if not self.remote.stopped or length < 1 or length > 64:
            raise LockstepGdbError("DuckStation must be stopped for a bounded watchpoint")
        start = offset if precise else max(0, offset - 3)
        span = length + offset - start
        for address in self.watchpoint_addresses(region, start):
            reply = self.remote.packet(f"Z2,{address:x},{span:x}")
            self._expect(reply, "OK", "watchpoint")
            self.watchpoints.append((address, span))
        self.watch_target = {"region": region, "offset": offset}
        if not hasattr(self, "watch_hits"):
            self.watch_hits = []

    def take_watch_hits(self) -> list[dict]:
        hits = list(getattr(self, "watch_hits", []))
        self.watch_hits = []
        return hits

    def clear_data_watchpoints(self) -> None:
        if not self.remote.stopped:
            raise LockstepGdbError("DuckStation must be stopped to clear watchpoints")
        for address, length in reversed(self.watchpoints):
            self._expect(self.remote.packet(f"z2,{address:x},{length:x}"), "OK", "watchpoint clear")
        self.watchpoints.clear()

    def advance_refinement(self, timeout: float) -> dict:
        reply = self.remote.packet("c", timeout=timeout)
        if not reply.startswith(("S", "T")):
            raise LockstepGdbError(f"Unexpected DuckStation stop reply: {reply}")
        pending = self.remote.packet("qXportLockstepNextEvent")
        if pending != "NONE":
            return {"stop": "boundary", "event": self._consume_pending(pending)}
        registers = decode_registers(self.remote.packet("g"))
        instruction_pc, instruction_word = self.last_instruction()
        return {"stop": "watchpoint", "side": "original", "stop_reply": reply,
                "pc": instruction_pc, "instruction_word": instruction_word,
                "stop_pc": registers.get("pc"), "registers": registers}

    def last_instruction(self) -> tuple[str, str]:
        reply = self.remote.packet("qXportLockstepLastInstruction")
        if not re.fullmatch(r"INSTRUCTION:[0-9a-fA-F]{8}:[0-9a-fA-F]{8}", reply):
            raise LockstepGdbError("DuckStation did not identify the watchpoint instruction")
        _, pc, word = reply.split(":")
        return "0x" + pc.lower(), "0x" + word.lower()

    def compare(self, request_id: str) -> dict:
        if not self.exceptions_identity:
            raise LockstepGdbError("Compiled memory exception identity is unavailable")
        reply = self.remote.packet(
            f"qXportLockstepCompare:{request_id}:{self.exceptions_identity}", timeout=30.0
        )
        return parse_compare_reply(reply, request_id)

    def registers(self) -> dict[str, str]:
        if not self.remote.stopped:
            raise LockstepGdbError("DuckStation must be stopped before reading registers")
        return decode_registers(self.remote.packet("g"))

    def gte_control_registers(self) -> dict[str, str]:
        if not self.remote.stopped:
            raise LockstepGdbError("DuckStation must be stopped before reading GTE registers")
        description = self.remote.packet("qXfer:features:read:target.xml:0,1")
        if not description.startswith(("m", "l")):
            raise LockstepGdbError(f"DuckStation target description is unavailable: {description}")
        names = (
            [f"gte_rt_{index}" for index in range(5)] + ["gte_trx", "gte_try", "gte_trz"] +
            [f"gte_llm_{index}" for index in range(5)] + ["gte_rbk", "gte_gbk", "gte_bbk"] +
            [f"gte_lcm_{index}" for index in range(5)] + ["gte_rfc", "gte_gfc", "gte_bfc"] +
            ["gte_ofx", "gte_ofy", "gte_h", "gte_dqa", "gte_dqb", "gte_zsf3", "gte_zsf4", "gte_flag"]
        )
        result = {}
        for index, name in enumerate(names, 105):
            encoded = self.remote.packet(f"p{index:x}")
            if len(encoded) != 8:
                raise LockstepGdbError(f"Malformed DuckStation GTE register {name}: {encoded}")
            result[name] = f"0x{int.from_bytes(bytes.fromhex(encoded), 'little'):08X}"
        return result

    def dma_registers(self) -> dict[str, str]:
        if not self.remote.stopped:
            raise LockstepGdbError("DuckStation must be stopped before reading DMA registers")
        reply = self.remote.packet("qXportLockstepDMA")
        fields = reply.split(":")
        names = [
            f"{channel}_{register}"
            for channel in ("mdecin", "mdecout", "gpu", "cdrom", "spu", "pio", "otc")
            for register in ("madr", "bcr", "chcr")
        ] + ["dpcr", "dicr"]
        if len(fields) != len(names) + 1 or fields[0] != "DMA" or any(len(value) != 8 for value in fields[1:]):
            raise LockstepGdbError(f"Malformed DuckStation DMA state: {reply}")
        return {name: f"0x{int(value, 16):08X}" for name, value in zip(names, fields[1:])}

    def interrupt(self) -> None:
        if not self.remote.stopped:
            self.remote.interrupt()

    def close(self) -> None:
        if getattr(self, "watchpoints", None) and self.remote.stopped:
            try:
                self.clear_data_watchpoints()
            except Exception:
                pass
        if self.attached:
            try:
                self.remote.packet(f"qXportLockstepDetach:{self.run_id}")
            except Exception:
                pass
            self.attached = False
        self.remote.close()


class NativeCdbParticipant:
    def __init__(self, controller, *, initially_stopped: bool = False):
        self.controller = controller
        self.first = not initially_stopped
        self.watch_hits = []

    def advance(self, timeout: float) -> dict:
        if self.first:
            self.first = False
        else:
            self.controller.continue_execution()
        for _ in range(2):
            event = self.controller.next_event(timeout)
            if event.get("stop") != "watchpoint":
                return event
            self.watch_hits.append({**event, "source": "cdb_prearmed_watchpoint"})
            self.controller.continue_execution()
        raise LockstepGdbError("Native target did not reach a boundary after its watchpoint")

    def install_data_watchpoint(self, region: str, offset: int) -> None:
        if not self.controller._refinement_watchpoint:
            self.controller.set_data_watchpoint(region, offset, 1)

    def take_watch_hits(self) -> list[dict]:
        hits = list(self.watch_hits)
        self.watch_hits.clear()
        return hits

    def deferred_state(self) -> dict:
        name = next((value for key, value in self.controller.environment.items()
                     if key.upper() == "XPORT_LOCKSTEP_DEFERRED_MAPPING"), None)
        if not name:
            raise LockstepGdbError("Native deferred mapping identity is unavailable")
        size = (10 + 3 + 32 * 3 + 512 * 3 + 4 + 4096 * 5) * 4
        with mmap.mmap(-1, size, tagname=name, access=mmap.ACCESS_READ) as view:
            for _ in range(32):
                view.seek(0)
                header = view.read(10 * 4)
                before = struct.unpack_from("<I", header, 8)[0]
                if before & 1:
                    time.sleep(0)
                    continue
                view.seek(8)
                after = struct.unpack("<I", view.read(4))[0]
                if before == after and not (after & 1):
                    fields = struct.unpack("<10I", header)
                    state = dict(zip((
                        "magic", "version", "sequence", "generation", "range_count",
                        "boundary_count", "catchup_count", "open", "resolved",
                        "canonical_count",
                    ), fields))
                    if (state["magic"] != 0x44504C58 or state["version"] != 12 or
                            state["range_count"] > 512 or state["canonical_count"] > 32 or
                            state["open"] > 1 or
                            state["resolved"] > 1):
                        raise LockstepGdbError("Native deferred mapping state is invalid")
                    return state
                time.sleep(0)
        raise LockstepGdbError("Native deferred mapping state did not stabilize")

    def interrupt(self) -> None:
        if self.controller._running:
            self.controller.interrupt()

    def close(self) -> None:
        self.controller.close()

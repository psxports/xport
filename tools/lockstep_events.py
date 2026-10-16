#!/usr/bin/env python3
"""Fail-closed event matching and two-process lockstep scheduling"""

from __future__ import annotations

import socket
from dataclasses import dataclass

from lockstep_contract import validate_event


BOUNDARY_KINDS = {"PRE_CALL", "POST_CALL", "TAIL_CALL", "VBLANK"}


class EventProtocolError(RuntimeError):
    pass


@dataclass(frozen=True)
class MatchDecision:
    matched: bool
    kind: str


def _guest_pointer_matches(value: int, descriptor: dict) -> bool:
    physical = value & 0x1fffffff
    extent = descriptor.get("bytes", 0)
    if descriptor.get("region") == 1:
        return physical < 0x200000 and extent <= 0x200000 - physical and descriptor.get("offset") == physical
    if descriptor.get("region") == 2:
        return (0x1f800000 <= physical < 0x1f800400 and
                extent <= 0x1f800400 - physical and
                descriptor.get("offset") == physical - 0x1f800000)
    return False


def _arguments_match(original: dict, native: dict) -> bool:
    original_values = original.get("arguments")
    native_values = native.get("arguments")
    if original_values is None and native_values is None:
        return True
    if not isinstance(original_values, list) or not isinstance(native_values, list):
        return False
    if len(original_values) < len(native_values):
        return False
    for value, descriptor in zip(original_values, native_values):
        if not isinstance(value, int) or not isinstance(descriptor, dict):
            return False
        if descriptor.get("kind") == 1:
            if (value & 0xffffffff) != (descriptor.get("scalar", -1) & 0xffffffff):
                return False
        elif descriptor.get("kind") == 2:
            if not _guest_pointer_matches(value, descriptor):
                return False
        elif descriptor.get("kind") == 6:
            if (value & 0xffffffff) != (descriptor.get("scalar", -1) & 0xffffffff):
                return False
        else:
            return False
    return True


class EventStreamValidator:
    def __init__(self, side: str, *, initial_mode: str | None = None):
        self.side = side
        self.next_sequence = 0
        self.next_ordinal: dict[int, int] = {}
        if initial_mode not in {None, "PREFIX_GUARD"}:
            raise ValueError("Unsupported initial event mode")
        self.mode: str | None = initial_mode
        self.epoch = 0

    def accept(self, event: dict) -> dict:
        validate_event(event)
        if event["side"] != self.side:
            raise EventProtocolError(f"Expected {self.side} event, received {event['side']}")
        if event["event_sequence"] != self.next_sequence:
            raise EventProtocolError(
                f"{self.side} event sequence {event['event_sequence']} != {self.next_sequence}"
            )
        self.next_sequence += 1
        if self.mode is None:
            if event["epoch"] != 0:
                raise EventProtocolError(f"{self.side} initial epoch must be zero")
            self.mode = event["mode"]
        elif event["kind"] == "MODE_TRANSITION":
            if (self.mode != "PREFIX_GUARD" or event["mode"] != "DETAILED" or
                    event["epoch"] != self.epoch + 1):
                raise EventProtocolError(f"{self.side} invalid mode transition")
            self.mode = event["mode"]
            self.epoch = event["epoch"]
            self.next_ordinal.clear()
        elif event["mode"] != self.mode or event["epoch"] != self.epoch:
            raise EventProtocolError(f"{self.side} mode or epoch changed without transition")
        if self.mode == "PREFIX_GUARD" and event["kind"] not in {
                "VBLANK", "FAULT", "PROCESS_EXIT"}:
            raise EventProtocolError(f"{self.side} emitted detailed event in PREFIX_GUARD")
        if event["kind"] == "PRE_CALL":
            parent = event["parent_invocation"]
            expected = self.next_ordinal.get(parent, 0)
            if event["call_ordinal"] != expected:
                raise EventProtocolError(
                    f"{self.side} call ordinal {event['call_ordinal']} != {expected} for parent {parent}"
                )
            self.next_ordinal[parent] = expected + 1
        elif event["kind"] == "TAIL_CALL":
            parent = event["parent_invocation"]
            expected = self.next_ordinal.get(parent, 0)
            if not expected or event["call_ordinal"] != expected - 1:
                raise EventProtocolError(
                    f"{self.side} tail ordinal {event['call_ordinal']} does not replace parent slot {expected - 1}"
                )
        return event


def match_events(original: dict, native: dict) -> MatchDecision:
    if original["mode"] != native["mode"] or original["epoch"] != native["epoch"]:
        return MatchDecision(False, "mode_epoch_divergence")
    if original["kind"] == "FAULT" or native["kind"] == "FAULT":
        return MatchDecision(False, "runtime_fault")
    if original["kind"] == "PROCESS_EXIT" or native["kind"] == "PROCESS_EXIT":
        if original["kind"] == native["kind"]:
            return MatchDecision(True, "process_exit")
        return MatchDecision(False, "premature_process_exit")
    if original["kind"] != native["kind"]:
        pair = (original["kind"], native["kind"])
        if pair == ("PRE_CALL", "POST_CALL"):
            return MatchDecision(False, "missing_call")
        if pair == ("POST_CALL", "PRE_CALL"):
            return MatchDecision(False, "extra_call")
        if "VBLANK" in pair:
            return MatchDecision(False, "missing_or_extra_subtree")
        return MatchDecision(False, "event_kind_divergence")
    if (original["parent_invocation"] != native["parent_invocation"] or
            original["invocation"] != native["invocation"] or
            original["depth"] != native["depth"]):
        return MatchDecision(False, "call_stack_divergence")
    if original["frame"] != native["frame"]:
        return MatchDecision(False, "frame_divergence")
    if original["kind"] == "VBLANK" and original.get("controller_state") != native.get("controller_state"):
        return MatchDecision(False, "input_schedule_divergence")
    if original["image"].lower() != native["image"].lower():
        return MatchDecision(False, "image_divergence")
    if original["function"] != native["function"] or original["target"] != native["target"]:
        return MatchDecision(False, "wrong_call_target_or_branch")
    if original["call_ordinal"] != native["call_ordinal"]:
        return MatchDecision(False, "call_ordinal_divergence")
    if original["kind"] in {"PRE_CALL", "TAIL_CALL"} and not _arguments_match(original, native):
        return MatchDecision(False, "argument_divergence")
    if (original["kind"] == "POST_CALL" and native.get("return_value") is not None and
            original.get("return_value") != native.get("return_value")):
        return MatchDecision(False, "return_value_divergence")
    return MatchDecision(True, "matched")


class LockstepScheduler:
    def __init__(self, original, native, *, timeout: float = 10.0,
                 deferred_timeout: float = 120.0, watch_target: dict | None = None,
                 initial_mode: str | None = None):
        self.original = original
        self.native = native
        self.timeout = timeout
        self.deferred_timeout = deferred_timeout
        self.original_stream = EventStreamValidator("original", initial_mode=initial_mode)
        self.native_stream = EventStreamValidator("native", initial_mode=initial_mode)
        self.last_match: dict | None = None
        self.ranged_memory_pending = False
        self.deferred_generation: int | None = None
        self.native_backlog: list[dict] = []
        self.halted = False
        self.steps = 0
        self.original_call_stack: list[dict] = []
        self.native_call_stack: list[dict] = []
        self.watch_target = watch_target
        self.original_watch_history: list[dict] = []
        self.native_watch_history: list[dict] = []

    def _watch_evidence(self) -> dict | None:
        if self.watch_target is None:
            return None
        self.original_watch_history.extend(self.original.take_watch_hits())
        self.native_watch_history.extend(self.native.take_watch_hits())
        return {"target": self.watch_target,
                "original_writes": list(self.original_watch_history),
                "native_writes": list(self.native_watch_history)}

    def watch_evidence(self) -> dict | None:
        return self._watch_evidence()

    def _arm_watch(self, event: dict) -> None:
        if self.watch_target is None or event.get("mode") != "DETAILED":
            return
        region, offset = self.watch_target["region"], self.watch_target["offset"]
        if not self.original.watchpoints:
            self.original.install_data_watchpoint(region, offset, 1)
        self.native.install_data_watchpoint(region, offset)

    @staticmethod
    def _stack_frame(event: dict) -> dict:
        return {key: event.get(key) for key in (
            "image", "function", "invocation", "parent_invocation", "callsite", "target",
            "return_pc", "source",
        ) if key in event}

    def _event_stack(self, stack: list[dict], event: dict) -> list[dict]:
        if event["kind"] == "MODE_TRANSITION":
            return []
        result = [dict(frame) for frame in stack]
        if event["kind"] == "PRE_CALL":
            result.append(self._stack_frame(event))
        elif event["kind"] == "TAIL_CALL":
            if result:
                result.pop()
            result.append(self._stack_frame(event))
        return result

    def _commit_stack(self, stack: list[dict], event: dict) -> None:
        if event["kind"] == "MODE_TRANSITION":
            stack.clear()
        elif event["kind"] == "PRE_CALL":
            stack.append(self._stack_frame(event))
        elif event["kind"] == "TAIL_CALL":
            if stack:
                stack.pop()
            stack.append(self._stack_frame(event))
        elif event["kind"] == "POST_CALL" and stack:
            stack.pop()

    def _interrupt_both(self) -> None:
        for participant in (self.original, self.native):
            try:
                participant.interrupt()
            except Exception:
                pass

    def _advance_pair(self) -> tuple[dict, dict]:
        if self.native_backlog:
            original = self.original_stream.accept(self.original.advance(self.timeout))
            return original, self.native_backlog.pop(0)
        # Publish the original device state before Native consumes this boundary
        original = self.original_stream.accept(self.original.advance(self.timeout))
        publish = getattr(self.native, 'publish_original_boundary', None)
        if publish is not None:
            publish(original, self.original)
        native = self.native_stream.accept(self.native.advance(self.timeout))
        return original, native

    def _drain_native_deferred(self, generation: int) -> None:
        state = self.native.deferred_state()
        if state["generation"] != generation or not state["open"]:
            raise EventProtocolError("Deferred generation is not open in native state")
        for _ in range(100000):
            native = self.native_stream.accept(self.native.advance(self.deferred_timeout))
            if native["kind"] in {"FAULT", "PROCESS_EXIT"}:
                raise EventProtocolError("Native terminated before deferred load commit")
            self.native_backlog.append(native)
            state = self.native.deferred_state()
            if state["generation"] != generation:
                raise EventProtocolError("Native deferred generation changed before commit")
            if not state["open"]:
                return
        raise EventProtocolError("Native deferred load exceeded the event limit")

    def step(self) -> dict:
        if self.halted:
            raise RuntimeError("Lockstep scheduler is halted")
        try:
            original, native = self._advance_pair()
        except Exception as error:
            self._interrupt_both()
            self.halted = True
            no_progress = isinstance(error, (TimeoutError, socket.timeout)) or error.__class__.__name__ == "CdbTimeout"
            return {
                "status": "runtime_divergence", "kind": "no_progress" if no_progress else "protocol_or_runtime_failure",
                "error": str(error), "last_match": self.last_match,
            }
        decision = match_events(original, native)
        original_stack = self._event_stack(self.original_call_stack, original)
        native_stack = self._event_stack(self.native_call_stack, native)
        if not decision.matched:
            self.halted = True
            return {
                "status": "control_flow_divergence", "kind": decision.kind,
                "original": original, "native": native, "last_match": self.last_match,
                "original_stack": original_stack, "native_stack": native_stack,
                "memory_compared": False,
            }
        if decision.kind == "process_exit":
            self.halted = True
            if self.ranged_memory_pending or self.deferred_generation is not None:
                return {
                    "status": "memory_divergence", "kind": "ranged_memory_unresolved",
                    "original": original, "native": native, "last_match": self.last_match,
                    "memory_compared": False,
                }
            return {
                "status": "match_complete", "kind": "process_exit",
                "original": original, "native": native, "last_match": self.last_match,
                "memory_compared": False,
            }
        request_id = f"event-{self.steps}"
        comparison = self.original.compare(request_id) if original["kind"] in BOUNDARY_KINDS else None
        if comparison and comparison["status"] == "deferred":
            generation = comparison.get("generation")
            if (not isinstance(generation, int) or generation <= 0 or
                    (self.deferred_generation is not None and
                     self.deferred_generation != generation) or self.ranged_memory_pending):
                self.halted = True
                return {
                    "status": "runtime_divergence", "kind": "invalid_deferred_memory_state",
                    "original": original, "native": native, "comparison": comparison,
                    "last_match": self.last_match, "original_stack": original_stack,
                    "native_stack": native_stack, "memory_compared": False,
                }
            if self.deferred_generation is None:
                self.deferred_generation = generation
                try:
                    self._drain_native_deferred(generation)
                except Exception as error:
                    self._interrupt_both()
                    self.halted = True
                    return {
                        "status": "runtime_divergence", "kind": "deferred_native_catchup_failed",
                        "error": str(error), "original": original, "native": native,
                        "comparison": comparison, "last_match": self.last_match,
                        "original_stack": original_stack, "native_stack": native_stack,
                        "memory_compared": False,
                    }
        if comparison and comparison["status"] not in {"match", "ranged_match", "deferred"}:
            self.halted = True
            return {
                "status": "memory_divergence", "kind": comparison["status"],
                "original": original, "native": native, "comparison": comparison,
                "last_match": self.last_match, "original_stack": original_stack,
                "native_stack": native_stack, "memory_compared": True,
                "watch_evidence": self._watch_evidence(),
            }
        if (comparison and comparison["status"] == "match" and
                self.deferred_generation is not None):
            comparison = {
                "status": "ranged_match", "request_id": comparison["request_id"],
                "generation": self.deferred_generation,
            }
            self.deferred_generation = None
        result = {
            "status": "matched", "kind": original["kind"], "original": original,
            "native": native, "comparison": comparison, "original_stack": original_stack,
            "native_stack": native_stack,
            "memory_compared": bool(comparison and comparison["status"] != "deferred"),
            "full_memory_compared": bool(comparison and comparison["status"] == "match"),
        }
        self._commit_stack(self.original_call_stack, original)
        self._commit_stack(self.native_call_stack, native)
        self._watch_evidence()
        self._arm_watch(original)
        if comparison and comparison["status"] in {"deferred", "ranged_match"}:
            if comparison["status"] == "ranged_match":
                self.ranged_memory_pending = True
        else:
            if comparison and comparison["status"] == "match":
                self.ranged_memory_pending = False
            self.last_match = result
        self.steps += 1
        return result

    def run(self, max_steps: int, progress=None) -> dict:
        if max_steps < 1:
            raise ValueError("max_steps must be positive")
        for _ in range(max_steps):
            result = self.step()
            if progress is not None:
                progress(result)
            if result["status"] != "matched":
                return result
        self.halted = True
        self._interrupt_both()
        return {"status": "runtime_divergence", "kind": "step_limit", "last_match": self.last_match}

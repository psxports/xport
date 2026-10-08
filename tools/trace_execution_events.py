"""Read and write bounded original CPU memory-effect traces"""

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import struct


MAGIC = b"XPEVT001"
HEADER = struct.Struct("<8sII")
ENVELOPE = struct.Struct("<II")
STORE_HEADER = struct.Struct("<QQQQIIIIIII")
INVOCATION = struct.Struct("<QQQQIII")
CALL_HEADER = struct.Struct("<QQQQQIIIIIIIIIIII")
DEVICE_HEADER = struct.Struct("<QQQQQ" + "I" * 14)
END_PAYLOAD = struct.Struct("<QQ32s")
STORE = 1
INVOCATION_EDGE = 2
END = 3
CALL_SNAPSHOT = 4
DEVICE_EVENT = 5
REGIONS = {1: ("ram", 2 * 1024 * 1024), 2: ("vram", 1024 * 512 * 2), 3: ("scratchpad", 1024), 4: ("spu_ram", 512 * 1024)}
CPU_REGIONS = {1, 3}
MAX_MANIFEST = 1024 * 1024
MAX_PAYLOAD = 4096
EMPTY_LAST_SEQ = (1 << 64) - 1


class ExecutionFormatError(ValueError):
    """Raised when an execution trace violates its contract"""


@dataclass(frozen=True)
class StoreEvent:
    event_seq: int
    frame: int
    guest_clock: int
    invocation: int
    pc: int
    instruction: int
    region_id: int
    physical_offset: int
    virtual_address: int
    size: int
    byte_mask: int
    before: bytes
    after: bytes

    @property
    def region(self):
        return REGIONS[self.region_id][0]

    @property
    def changed_mask(self):
        value = 0
        for index in range(self.size):
            if self.byte_mask & (1 << index) and self.before[index] != self.after[index]:
                value |= 1 << index
        return value


@dataclass(frozen=True)
class InvocationEvent:
    event_seq: int
    frame: int
    guest_clock: int
    invocation: int
    pc: int
    hook_id: int
    edge: int


@dataclass(frozen=True)
class CallSnapshotEvent:
    event_seq: int
    frame: int
    guest_clock: int
    invocation: int
    parent_invocation: int
    pc: int
    hook_id: int
    edge: int
    relation: int
    callsite: int
    a0: int
    a1: int
    a2: int
    a3: int
    sp: int
    ra: int
    stack_words: tuple


@dataclass(frozen=True)
class DeviceEvent:
    event_seq: int
    operation_id: int
    parent_operation_id: int
    frame: int
    guest_clock: int
    device: int
    primitive: int
    phase: int
    direction: int
    evidence: int
    source_region: int
    destination_region: int
    source_offset: int
    destination_offset: int
    byte_count: int
    byte_mask: int
    channel: int
    command: int
    status: int


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def validate_manifest(value):
    if not isinstance(value, dict) or value.get("schema") != 1:
        raise ExecutionFormatError("Unsupported execution manifest")
    required = {"contract", "backend", "source_identity", "targets", "hooks", "window"}
    if required - set(value):
        raise ExecutionFormatError("Execution manifest is incomplete")
    if value["contract"] != "psx-targeted-execution-v1" or value["backend"] != "interpreter":
        raise ExecutionFormatError("Execution trace must use the interpreter contract")
    if not isinstance(value["source_identity"], dict) or not value["source_identity"]:
        raise ExecutionFormatError("Execution source identity is empty")
    targets = []
    for target in value["targets"]:
        region_id = target.get("region_id")
        offset, size = target.get("physical_offset"), target.get("size")
        if region_id not in REGIONS or type(offset) is not int or type(size) is not int or size < 1:
            raise ExecutionFormatError("Invalid execution target")
        if offset < 0 or offset + size > REGIONS[region_id][1]:
            raise ExecutionFormatError("Execution target is outside its physical region")
        targets.append({"region_id": region_id, "physical_offset": offset, "size": size})
    if not targets:
        raise ExecutionFormatError("Execution targets are empty")
    hooks = value["hooks"]
    if not isinstance(hooks, list):
        raise ExecutionFormatError("Invalid invocation hooks")
    normalized_hooks = []
    for hook in hooks:
        if type(hook) is int:
            if hook < 0 or hook > 0xFFFFFFFF:
                raise ExecutionFormatError("Invalid invocation hook")
            normalized_hooks.append(hook)
        elif (isinstance(hook, dict) and set(hook) == {"pc", "edge", "stack_words", "relation"} and
              type(hook["pc"]) is int and type(hook["edge"]) is int and type(hook["stack_words"]) is int and
              type(hook["relation"]) is int and 0x80000000 <= hook["pc"] < 0x80200000 and
              hook["edge"] in (1, 2) and 0 <= hook["stack_words"] <= 12 and 1 <= hook["relation"] <= 5):
            normalized_hooks.append(dict(hook))
        else:
            raise ExecutionFormatError("Invalid invocation hook")
    window = value["window"]
    if (not isinstance(window, dict) or type(window.get("first_frame")) is not int or
            type(window.get("last_frame")) is not int or window["last_frame"] < window["first_frame"]):
        raise ExecutionFormatError("Invalid execution window")
    return dict(value, targets=targets, hooks=normalized_hooks, window=dict(window))


def _read_exact(stream, size, label):
    value = stream.read(size)
    if len(value) != size:
        raise ExecutionFormatError(f"Truncated {label}")
    return value


class ExecutionWriter:
    def __init__(self, stream, manifest):
        self.stream = stream
        self.manifest = validate_manifest(manifest)
        encoded = canonical(self.manifest)
        if len(encoded) > MAX_MANIFEST:
            raise ExecutionFormatError("Execution manifest is too large")
        header = HEADER.pack(MAGIC, HEADER.size, len(encoded)) + encoded
        stream.write(header)
        self.file_hash = hashlib.sha256(header)
        self.count = 0
        self.last_seq = None
        self.last_frame = None
        self.last_clock = None
        self.finished = False

    def _write(self, kind, payload, include=True):
        if len(payload) > MAX_PAYLOAD:
            raise ExecutionFormatError("Execution payload is too large")
        record = ENVELOPE.pack(kind, len(payload)) + payload + hashlib.sha256(payload).digest()
        self.stream.write(record)
        if include:
            self.file_hash.update(record)

    def _sequence(self, event):
        expected = 0 if self.last_seq is None else self.last_seq + 1
        if event.event_seq != expected:
            raise ExecutionFormatError("Execution event sequence is not contiguous")
        if self.last_frame is not None and (event.frame < self.last_frame or event.guest_clock < self.last_clock):
            raise ExecutionFormatError("Execution event time moved backwards")

    def write_store(self, event):
        self._sequence(event)
        _validate_store(event, self.manifest)
        payload = STORE_HEADER.pack(event.event_seq, event.frame, event.guest_clock, event.invocation,
                                    event.pc, event.instruction, event.region_id, event.physical_offset,
                                    event.virtual_address, event.size, event.byte_mask)
        self._write(STORE, payload + event.before + event.after)
        self._accept(event)

    def write_invocation(self, event):
        self._sequence(event)
        if event.edge not in (1, 2):
            raise ExecutionFormatError("Invalid invocation edge")
        self._write(INVOCATION_EDGE, INVOCATION.pack(event.event_seq, event.frame, event.guest_clock,
                                                     event.invocation, event.pc, event.hook_id, event.edge))
        self._accept(event)

    def write_call(self, event):
        self._sequence(event)
        if event.edge not in (1, 2) or event.relation not in (1, 2, 3, 4, 5) or len(event.stack_words) > 12:
            raise ExecutionFormatError("Invalid call snapshot")
        payload = CALL_HEADER.pack(event.event_seq, event.frame, event.guest_clock, event.invocation,
                                   event.parent_invocation, event.pc, event.hook_id, event.edge, event.relation,
                                   event.callsite, event.a0, event.a1, event.a2, event.a3, event.sp, event.ra,
                                   len(event.stack_words))
        payload += struct.pack("<" + "I" * len(event.stack_words), *event.stack_words)
        self._write(CALL_SNAPSHOT, payload)
        self._accept(event)

    def write_device(self, event):
        self._sequence(event)
        _validate_device(event, self.manifest)
        values = tuple(getattr(event, field) for field in DeviceEvent.__dataclass_fields__)
        status = values[-1] & 0xffffffff
        self._write(DEVICE_EVENT, DEVICE_HEADER.pack(*values[:-1], status))
        self._accept(event)

    def _accept(self, event):
        self.count += 1
        self.last_seq = event.event_seq
        self.last_frame = event.frame
        self.last_clock = event.guest_clock

    def finish(self):
        if self.finished:
            raise ExecutionFormatError("Execution stream cannot be finished")
        last_seq = self.last_seq if self.last_seq is not None else EMPTY_LAST_SEQ
        payload = END_PAYLOAD.pack(self.count, last_seq, self.file_hash.digest())
        self._write(END, payload, include=False)
        self.finished = True


def _validate_store(event, manifest):
    if event.region_id not in CPU_REGIONS or event.size not in (1, 2, 4):
        raise ExecutionFormatError("Unsupported store region or size")
    if len(event.before) != event.size or len(event.after) != event.size:
        raise ExecutionFormatError("Store byte inventory has the wrong size")
    if event.byte_mask < 1 or event.byte_mask >= 1 << event.size:
        raise ExecutionFormatError("Store byte mask is invalid")
    if event.physical_offset < 0 or event.physical_offset + event.size > REGIONS[event.region_id][1]:
        raise ExecutionFormatError("Store is outside its physical region")
    covered = set()
    for target in manifest["targets"]:
        if target["region_id"] == event.region_id:
            covered.update(range(target["physical_offset"], target["physical_offset"] + target["size"]))
    for index in range(event.size):
        if event.byte_mask & (1 << index) and event.physical_offset + index not in covered:
            raise ExecutionFormatError("Store byte is outside targeted ranges")


def _validate_device(event, manifest):
    if event.device not in (1, 2, 3) or event.primitive not in range(1, 11) or event.phase not in range(1, 8):
        raise ExecutionFormatError("Invalid device identity, primitive or phase")
    if event.direction not in range(0, 4) or event.evidence not in (1, 2, 3):
        raise ExecutionFormatError("Invalid device direction or evidence")
    if event.source_region not in range(0, 5) or event.destination_region not in range(0, 5):
        raise ExecutionFormatError("Invalid device region")
    if event.phase == 4:
        if not event.byte_count or event.destination_region not in REGIONS:
            raise ExecutionFormatError("Device write has no destination range")
        limit = REGIONS[event.destination_region][1]
        if event.destination_offset < 0 or event.destination_offset + event.byte_count > limit:
            raise ExecutionFormatError("Device write is outside its physical region")
        if not any(target["region_id"] == event.destination_region and
                   event.destination_offset < target["physical_offset"] + target["size"] and
                   target["physical_offset"] < event.destination_offset + event.byte_count
                   for target in manifest["targets"]):
            raise ExecutionFormatError("Device write is outside targeted ranges")
    if event.phase == 7 and event.evidence != 3:
        raise ExecutionFormatError("Device gap must be uncovered")


class ExecutionReader:
    def __init__(self, stream):
        self.stream = stream
        raw = _read_exact(stream, HEADER.size, "execution header")
        magic, header_size, length = HEADER.unpack(raw)
        if magic != MAGIC or header_size != HEADER.size or length > MAX_MANIFEST:
            raise ExecutionFormatError("Invalid execution header")
        encoded = _read_exact(stream, length, "execution manifest")
        try:
            value = json.loads(encoded.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise ExecutionFormatError("Invalid execution manifest JSON") from error
        if canonical(value) != encoded:
            raise ExecutionFormatError("Execution manifest is not canonical")
        self.manifest = validate_manifest(value)
        self.file_hash = hashlib.sha256(raw + encoded)
        self.count = 0
        self.last_seq = None
        self.last_frame = None
        self.last_clock = None
        self.ended = False

    def iter_events(self):
        while True:
            envelope = self.stream.read(ENVELOPE.size)
            if not envelope:
                raise ExecutionFormatError("Missing execution END")
            if len(envelope) != ENVELOPE.size:
                raise ExecutionFormatError("Truncated execution envelope")
            kind, length = ENVELOPE.unpack(envelope)
            if kind not in (STORE, INVOCATION_EDGE, END, CALL_SNAPSHOT, DEVICE_EVENT) or length > MAX_PAYLOAD:
                raise ExecutionFormatError("Unknown execution record")
            payload = _read_exact(self.stream, length, "execution payload")
            payload_hash = _read_exact(self.stream, 32, "execution payload hash")
            if hashlib.sha256(payload).digest() != payload_hash:
                raise ExecutionFormatError("Execution payload hash mismatch")
            if kind == END:
                if len(payload) != END_PAYLOAD.size:
                    raise ExecutionFormatError("Invalid execution END")
                count, last_seq, file_hash = END_PAYLOAD.unpack(payload)
                expected_last_seq = self.last_seq if self.last_seq is not None else EMPTY_LAST_SEQ
                if count != self.count or last_seq != expected_last_seq or file_hash != self.file_hash.digest():
                    raise ExecutionFormatError("Execution END identity mismatch")
                if self.stream.read(1):
                    raise ExecutionFormatError("Bytes after execution END")
                self.ended = True
                return
            raw = envelope + payload + payload_hash
            self.file_hash.update(raw)
            event = self._decode(kind, payload)
            expected = 0 if self.last_seq is None else self.last_seq + 1
            if event.event_seq != expected:
                raise ExecutionFormatError("Execution event sequence is not contiguous")
            if self.last_frame is not None and (event.frame < self.last_frame or event.guest_clock < self.last_clock):
                raise ExecutionFormatError("Execution event time moved backwards")
            self.count += 1
            self.last_seq = event.event_seq
            self.last_frame = event.frame
            self.last_clock = event.guest_clock
            yield event

    def _decode(self, kind, payload):
        if kind == STORE:
            if len(payload) < STORE_HEADER.size:
                raise ExecutionFormatError("Truncated store event")
            fields = STORE_HEADER.unpack_from(payload)
            size = fields[-2]
            if len(payload) != STORE_HEADER.size + size * 2:
                raise ExecutionFormatError("Invalid store event length")
            event = StoreEvent(*fields, payload[STORE_HEADER.size:STORE_HEADER.size + size],
                               payload[STORE_HEADER.size + size:])
            _validate_store(event, self.manifest)
            return event
        if kind == CALL_SNAPSHOT:
            if len(payload) < CALL_HEADER.size:
                raise ExecutionFormatError("Truncated call snapshot")
            fields = CALL_HEADER.unpack_from(payload)
            count = fields[-1]
            if count > 12 or len(payload) != CALL_HEADER.size + count * 4:
                raise ExecutionFormatError("Invalid call snapshot length")
            event = CallSnapshotEvent(*fields[:-1], struct.unpack_from("<" + "I" * count, payload, CALL_HEADER.size))
            if event.edge not in (1, 2) or event.relation not in (1, 2, 3, 4, 5):
                raise ExecutionFormatError("Invalid call snapshot relation")
            return event
        if kind == DEVICE_EVENT:
            if len(payload) != DEVICE_HEADER.size:
                raise ExecutionFormatError("Invalid device event length")
            fields = list(DEVICE_HEADER.unpack(payload))
            if fields[-1] & 0x80000000:
                fields[-1] -= 0x100000000
            event = DeviceEvent(*fields)
            _validate_device(event, self.manifest)
            return event
        if len(payload) != INVOCATION.size:
            raise ExecutionFormatError("Invalid invocation event length")
        event = InvocationEvent(*INVOCATION.unpack(payload))
        if event.edge not in (1, 2):
            raise ExecutionFormatError("Invalid invocation edge")
        return event


def write_execution_file(path, manifest, events):
    with Path(path).open("wb") as stream:
        writer = ExecutionWriter(stream, manifest)
        for event in events:
            if isinstance(event, StoreEvent):
                writer.write_store(event)
            elif isinstance(event, CallSnapshotEvent):
                writer.write_call(event)
            elif isinstance(event, DeviceEvent):
                writer.write_device(event)
            else:
                writer.write_invocation(event)
        writer.finish()


def read_execution_file(path):
    with Path(path).open("rb") as stream:
        reader = ExecutionReader(stream)
        events = list(reader.iter_events())
        return reader.manifest, events, reader.ended

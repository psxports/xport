"""Validate and query bounded DMA, GPU and SPU provenance events"""

import argparse
import json
from pathlib import Path

from trace_execution_events import DeviceEvent, read_execution_file


CONTRACT = "xport-device-provenance-v1"
DEVICES = {"dma", "gpu", "spu"}
PRIMITIVES = {"ram_dma", "gpu_packet", "vram_upload", "vram_copy", "vram_fill", "gpu_raster", "spu_upload", "spu_dma", "spu_capture", "spu_reverb"}
PHASES = {"create", "submit", "fetch", "write", "raster_complete", "complete", "gap"}
REGIONS = {None, "ram", "vram", "scratchpad", "spu_ram"}
EVIDENCE = {"exact", "candidate", "uncovered"}
DIRECTIONS = {"none", "to_device", "from_device", "internal"}
DEVICE_NAMES = {1: "dma", 2: "gpu", 3: "spu"}
PRIMITIVE_NAMES = {1: "ram_dma", 2: "gpu_packet", 3: "vram_upload", 4: "vram_copy", 5: "vram_fill",
                   6: "gpu_raster", 7: "spu_upload", 8: "spu_dma", 9: "spu_capture", 10: "spu_reverb"}
PHASE_NAMES = {1: "create", 2: "submit", 3: "fetch", 4: "write", 5: "raster_complete", 6: "complete", 7: "gap"}
REGION_NAMES = {0: None, 1: "ram", 2: "vram", 3: "scratchpad", 4: "spu_ram"}
DIRECTION_NAMES = {0: "none", 1: "to_device", 2: "from_device", 3: "internal"}
EVIDENCE_NAMES = {1: "exact", 2: "candidate", 3: "uncovered"}


class DeviceTraceError(ValueError):
    """Raised when device provenance is incomplete or inconsistent"""


def from_xpe(path):
    _, events, ended = read_execution_file(path)
    converted = []
    for event in events:
        if not isinstance(event, DeviceEvent):
            continue
        converted.append({
            "sequence": len(converted), "operation_id": event.operation_id,
            "parent_operation_id": event.parent_operation_id, "frame": event.frame, "clock": event.guest_clock,
            "device": DEVICE_NAMES[event.device], "primitive": PRIMITIVE_NAMES[event.primitive],
            "phase": PHASE_NAMES[event.phase], "direction": DIRECTION_NAMES[event.direction],
            "evidence": EVIDENCE_NAMES[event.evidence], "source_region": REGION_NAMES[event.source_region],
            "source_offset": event.source_offset, "destination_region": REGION_NAMES[event.destination_region],
            "destination_offset": event.destination_offset, "byte_count": event.byte_count,
            "byte_mask": event.byte_mask, "channel": event.channel, "command": event.command,
            "status": event.status,
        })
    return {"schema": 1, "contract": CONTRACT, "complete": ended, "events": converted}


def load_trace(path):
    path = Path(path)
    if path.read_bytes()[:8] == b"XPEVT001":
        return from_xpe(path)
    return json.loads(path.read_text(encoding="utf-8"))


def validate(trace, allow_incomplete=False):
    if not isinstance(trace, dict) or trace.get("schema") != 1 or trace.get("contract") != CONTRACT:
        raise DeviceTraceError("Unsupported device provenance trace")
    events = trace.get("events")
    if not isinstance(events, list):
        raise DeviceTraceError("Device events are missing")
    active = {}
    completed = set()
    last_frame = -1
    for sequence, event in enumerate(events):
        required = {"sequence", "operation_id", "parent_operation_id", "frame", "clock", "device", "primitive", "phase", "direction", "evidence", "source_region", "source_offset", "destination_region", "destination_offset", "byte_count", "byte_mask", "channel", "command", "status"}
        if not isinstance(event, dict) or set(event) != required or event["sequence"] != sequence:
            raise DeviceTraceError("Invalid device event envelope or sequence")
        if event["device"] not in DEVICES or event["primitive"] not in PRIMITIVES or event["phase"] not in PHASES:
            raise DeviceTraceError("Unknown device, primitive or phase")
        if event["direction"] not in DIRECTIONS or event["evidence"] not in EVIDENCE:
            raise DeviceTraceError("Unknown direction or evidence strength")
        if event["source_region"] not in REGIONS or event["destination_region"] not in REGIONS:
            raise DeviceTraceError("Unknown device memory region")
        integer_fields = ("operation_id", "parent_operation_id", "frame", "clock", "source_offset", "destination_offset", "byte_count", "byte_mask", "channel", "command", "status")
        if any(type(event[name]) is not int for name in integer_fields) or event["operation_id"] < 1 or event["frame"] < last_frame:
            raise DeviceTraceError("Invalid device identity, time or scalar field")
        last_frame = event["frame"]
        identity = event["operation_id"]
        if event["phase"] == "create":
            if identity in active or identity in completed:
                raise DeviceTraceError("Duplicate device operation")
            if event["parent_operation_id"] and event["parent_operation_id"] not in active and event["parent_operation_id"] not in completed:
                raise DeviceTraceError("Unknown parent device operation")
            active[identity] = (event["device"], event["primitive"], event["direction"], event["channel"], event["command"])
        else:
            context = active.get(identity)
            actual = (event["device"], event["primitive"], event["direction"], event["channel"], event["command"])
            if context is None or context != actual:
                raise DeviceTraceError("Device event has no matching active operation")
            if event["phase"] == "write" and (not event["byte_count"] or event["destination_region"] is None):
                raise DeviceTraceError("Device write has no destination range")
            if event["phase"] == "gap" and event["evidence"] != "uncovered":
                raise DeviceTraceError("Coverage gap must be uncovered")
            if event["phase"] == "complete":
                del active[identity]
                completed.add(identity)
    complete = trace.get("complete") is True
    if not allow_incomplete and (active or not complete):
        missing = sorted(active)
        raise DeviceTraceError(f"Lost device completion for operations {missing}" if missing else "Device trace is not finalized")
    return {"active": sorted(active), "events": len(events), "complete": complete and not active}


def last_writer(trace, region, offset, size):
    validate(trace)
    if region not in REGIONS - {None} or offset < 0 or size < 1:
        raise DeviceTraceError("Invalid last-writer target")
    target_end = offset + size
    candidates = []
    for event in trace["events"]:
        if event["phase"] != "write" or event["destination_region"] != region:
            continue
        event_end = event["destination_offset"] + event["byte_count"]
        if event["destination_offset"] < target_end and offset < event_end:
            candidates.append(event)
    if not candidates:
        return {"status": "missing_precise_writer", "region": region, "offset": offset, "size": size}
    event = candidates[-1]
    status = "exact" if event["evidence"] == "exact" else "candidate"
    return {"status": status, "operation_id": event["operation_id"], "event": event}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    check = sub.add_parser("validate")
    check.add_argument("trace", type=Path)
    check.add_argument("--allow-incomplete", action="store_true")
    query = sub.add_parser("last-writer")
    query.add_argument("trace", type=Path)
    query.add_argument("--region", required=True)
    query.add_argument("--offset", type=lambda value: int(value, 0), required=True)
    query.add_argument("--size", type=lambda value: int(value, 0), required=True)
    query.add_argument("--output", type=Path)
    convert = sub.add_parser("convert")
    convert.add_argument("trace", type=Path)
    convert.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    trace = load_trace(args.trace)
    if args.command == "validate":
        result = validate(trace, args.allow_incomplete)
    elif args.command == "last-writer":
        result = last_writer(trace, args.region, args.offset, args.size)
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    else:
        validate(trace)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(trace, indent=2) + "\n", encoding="utf-8")
        result = {"status": "converted", "events": len(trace["events"]), "output": str(args.output)}
    print(json.dumps(result, separators=(",", ":")))


if __name__ == "__main__":
    main()

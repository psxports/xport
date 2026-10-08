"""Normalize DuckStation register/stack call snapshots through an ABI registry"""

import argparse
import hashlib
import json
from pathlib import Path

from trace_call_match import CONTRACT, CallTraceError, digest_value, validate_registry, validate_trace


def _hash_pointee(data, expected):
    if expected == 0:
        return None
    if not isinstance(data, str):
        raise CallTraceError("Bounded pointee bytes are missing")
    raw = bytes.fromhex(data)
    if len(raw) != expected:
        raise CallTraceError("Bounded pointee byte count mismatch")
    return hashlib.sha256(raw).hexdigest()


def normalize_pointer(value, contract, snapshot):
    kind = contract["type"]["pointer"]
    size = contract["pointee_bytes"]
    if kind == "guest":
        physical = value & 0x1FFFFFFF
        if physical < 0x00800000:
            region, offset = "ram", physical & 0x001FFFFF
        elif 0x1F800000 <= physical < 0x1F800400:
            region, offset = "scratchpad", physical - 0x1F800000
        elif 0x1FC00000 <= physical < 0x1FC80000:
            region, offset = "bios", physical - 0x1FC00000
        elif 0x1F801000 <= physical < 0x1F803000:
            region, offset = "mmio", physical - 0x1F801000
        else:
            raise CallTraceError("Guest pointer is outside registered regions")
        return {"kind": "guest", "region": region, "offset": offset, "bytes": size,
                "pointee_sha256": _hash_pointee(snapshot.get("pointee"), size)}
    token = snapshot.get("pointer_token")
    if not isinstance(token, str) or not token:
        raise CallTraceError("Host/opaque pointer requires an explicit token")
    if kind == "host_bounded":
        return {"kind": kind, "token": token, "bytes": size, "pointee_sha256": _hash_pointee(snapshot.get("pointee"), size)}
    return {"kind": kind, "token": token}


def convert(registry, raw):
    registry = validate_registry(registry)
    functions = {item["id"]: item for item in registry["functions"]}
    events = []
    active = set()
    for sequence, snapshot in enumerate(raw.get("snapshots", [])):
        if snapshot.get("pending_load_applied") is not True:
            raise CallTraceError("Original entry was sampled before pending-load commit")
        function = functions.get(snapshot.get("function"))
        if not function:
            raise CallTraceError("Raw call snapshot has unknown function")
        edge = snapshot.get("edge")
        values = []
        result = None
        if edge == "enter":
            for contract in function["arguments"]:
                location = contract["location"]
                if isinstance(location, str):
                    value = snapshot.get("registers", {}).get(location)
                else:
                    value = snapshot.get("stack_words", {}).get(str(location["stack_offset"]))
                if type(value) is not int:
                    raise CallTraceError("Raw call argument is missing")
                values.append(normalize_pointer(value, contract, snapshot.get("pointers", {}).get(contract["name"], {}))
                              if isinstance(contract["type"], dict) else value)
            active.add(snapshot["invocation"])
        elif edge == "exit":
            if snapshot["invocation"] not in active:
                raise CallTraceError("Raw exit has no active invocation")
            active.remove(snapshot["invocation"])
            kind = function["return"]["type"]
            result = None if kind == "void" else snapshot.get("registers", {}).get("v0")
            if kind != "void" and type(result) is not int:
                raise CallTraceError("Raw return value is missing")
        else:
            raise CallTraceError("Raw call edge is invalid")
        events.append({"sequence": sequence, "edge": edge, "function": function["id"],
                       "invocation": snapshot["invocation"], "parent_invocation": snapshot.get("parent_invocation"),
                       "callsite": snapshot["callsite"], "relation": snapshot["relation"], "arguments": values,
                       "return": result, "frame": snapshot["frame"], "clock": snapshot["clock"]})
    trace = {"schema": 1, "contract": CONTRACT, "origin": "original", "registry_sha256": digest_value(registry),
             "events": events, "complete": raw.get("complete") is True and not active}
    return validate_trace(trace, registry, "original")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--registry", type=Path, required=True)
    parser.add_argument("--raw", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = convert(json.loads(args.registry.read_text(encoding="utf-8")), json.loads(args.raw.read_text(encoding="utf-8")))
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(result, stream, indent=2)
        stream.write("\n")
    print(json.dumps({"status": "complete", "events": len(result["events"]), "output": str(args.output.resolve())}, separators=(",", ":")))


if __name__ == "__main__":
    main()

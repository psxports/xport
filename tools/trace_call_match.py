"""Validate ABI registries and compare contextual original/native call traces"""

import argparse
import hashlib
import json
from pathlib import Path


REGISTRY_SCHEMA = 1
TRACE_SCHEMA = 1
CONTRACT = "xport-call-trace-v1"
SCALARS = {"u8", "s8", "u16", "s16", "u32", "s32"}
RELATIONS = {"call", "tail", "recursion", "callback", "interrupt"}
POINTER_KINDS = {"guest", "host_bounded", "opaque"}


class CallTraceError(ValueError):
    """Raised when call evidence cannot be compared safely"""


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def digest_value(value):
    return hashlib.sha256(canonical(value)).hexdigest()


def validate_registry(value):
    if not isinstance(value, dict) or value.get("schema") != REGISTRY_SCHEMA or value.get("contract") != "xport-abi-registry-v1":
        raise CallTraceError("Unsupported ABI registry")
    functions = value.get("functions")
    if not isinstance(functions, list) or not functions:
        raise CallTraceError("ABI registry has no functions")
    identities = set()
    addresses = set()
    for function in functions:
        required = {"id", "image", "address", "native_symbol", "prototype", "arguments", "return", "relations"}
        if not isinstance(function, dict) or set(function) != required:
            raise CallTraceError("Invalid ABI function entry")
        identity = function["id"]
        address_key = (function["image"], function["address"])
        if not isinstance(identity, str) or not identity or identity in identities or address_key in addresses:
            raise CallTraceError("Duplicate ABI function identity")
        if not isinstance(function["image"], str) or not function["image"] or type(function["address"]) is not int:
            raise CallTraceError("Invalid image-qualified function address")
        if not isinstance(function["native_symbol"], str) or not function["native_symbol"].isidentifier():
            raise CallTraceError("Native symbol is required")
        identities.add(identity)
        addresses.add(address_key)
        prototype = function["prototype"]
        if not isinstance(prototype, str) or "(" not in prototype or ")" not in prototype:
            raise CallTraceError("Function prototype is required")
        for index, argument in enumerate(function["arguments"]):
            _validate_argument(argument, index)
        result = function["return"]
        if not isinstance(result, dict) or set(result) != {"type", "location"}:
            raise CallTraceError("Invalid return contract")
        if result["type"] != "void" and (not isinstance(result["type"], str) or result["type"] not in SCALARS) and not isinstance(result["type"], dict):
            raise CallTraceError("Unsupported return type")
        if result["location"] not in {None, "v0", "v0:v1"}:
            raise CallTraceError("Invalid return location")
        if not isinstance(function["relations"], list) or any(item not in RELATIONS for item in function["relations"]):
            raise CallTraceError("Unsupported call relation")
    return value


def _validate_argument(argument, index):
    required = {"name", "type", "location", "pointee_bytes"}
    if not isinstance(argument, dict) or set(argument) != required:
        raise CallTraceError("Invalid ABI argument entry")
    if not isinstance(argument["name"], str) or not argument["name"]:
        raise CallTraceError("Argument name is required")
    kind = argument["type"]
    if not isinstance(kind, str) or kind not in SCALARS:
        if not isinstance(kind, dict) or set(kind) != {"pointer"} or kind["pointer"] not in POINTER_KINDS:
            raise CallTraceError("Unsupported argument type")
    location = argument["location"]
    expected = f"a{index}" if index < 4 else None
    if isinstance(location, str) and location in {"a0", "a1", "a2", "a3"}:
        if location != expected:
            raise CallTraceError("Register argument location does not match its ordinal")
    elif not isinstance(location, dict) or set(location) != {"stack_offset"} or type(location["stack_offset"]) is not int or location["stack_offset"] < 16 or location["stack_offset"] % 4:
        raise CallTraceError("Stack argument needs an explicit aligned ABI offset")
    if isinstance(kind, dict):
        if type(argument["pointee_bytes"]) is not int or argument["pointee_bytes"] < 0 or argument["pointee_bytes"] > 4096:
            raise CallTraceError("Pointer capture must be explicitly bounded")
    elif argument["pointee_bytes"] is not None:
        raise CallTraceError("Scalar argument cannot have pointee capture")


def validate_trace(value, registry, expected_origin=None):
    if not isinstance(value, dict) or value.get("schema") != TRACE_SCHEMA or value.get("contract") != CONTRACT:
        raise CallTraceError("Unsupported call trace")
    if value.get("registry_sha256") != digest_value(registry):
        raise CallTraceError("Call trace ABI registry identity mismatch")
    origin = value.get("origin")
    if origin not in {"original", "native"} or expected_origin and origin != expected_origin:
        raise CallTraceError("Call trace origin mismatch")
    known = {item["id"]: item for item in registry["functions"]}
    events = value.get("events")
    if not isinstance(events, list):
        raise CallTraceError("Call trace events are missing")
    active = set()
    for sequence, event in enumerate(events):
        required = {"sequence", "edge", "function", "invocation", "parent_invocation", "callsite", "relation", "arguments", "return", "frame", "clock"}
        if not isinstance(event, dict) or set(event) != required or event["sequence"] != sequence:
            raise CallTraceError("Invalid call event envelope or sequence")
        function = known.get(event["function"])
        if not function or event["edge"] not in {"enter", "exit"} or event["relation"] not in function["relations"]:
            raise CallTraceError("Call event has unknown identity, edge or relation")
        if type(event["invocation"]) is not int or event["invocation"] < 1 or event["parent_invocation"] is not None and type(event["parent_invocation"]) is not int:
            raise CallTraceError("Invalid invocation identity")
        if type(event["callsite"]) is not int or type(event["frame"]) is not int or type(event["clock"]) is not int:
            raise CallTraceError("Invalid callsite or time")
        if event["edge"] == "enter":
            if event["invocation"] in active or len(event["arguments"]) != len(function["arguments"]) or event["return"] is not None:
                raise CallTraceError("Invalid call entry")
            for argument, contract in zip(event["arguments"], function["arguments"]):
                _validate_value(argument, contract["type"], contract["pointee_bytes"])
            active.add(event["invocation"])
        else:
            if event["invocation"] not in active or event["arguments"]:
                raise CallTraceError("Call exit has no matching entry")
            _validate_value(event["return"], function["return"]["type"], None)
            active.remove(event["invocation"])
    if active or value.get("complete") is not True:
        raise CallTraceError("Call trace is incomplete")
    return value


def _validate_value(value, kind, pointee_bytes):
    if kind == "void":
        if value is not None:
            raise CallTraceError("Void value must be null")
        return
    if isinstance(kind, str) and kind in SCALARS:
        if type(value) is not int:
            raise CallTraceError("Scalar call value must be an integer")
        return
    pointer = kind["pointer"]
    if not isinstance(value, dict) or value.get("kind") != pointer:
        raise CallTraceError("Pointer normalization kind mismatch")
    if pointer == "guest":
        if set(value) != {"kind", "region", "offset", "bytes", "pointee_sha256"} or value["region"] not in {"ram", "scratchpad", "bios", "mmio"}:
            raise CallTraceError("Invalid normalized guest pointer")
        if type(value["offset"]) is not int or value["offset"] < 0 or value["bytes"] != pointee_bytes:
            raise CallTraceError("Guest pointer bounds mismatch")
    elif pointer == "host_bounded":
        if set(value) != {"kind", "token", "bytes", "pointee_sha256"} or value["bytes"] != pointee_bytes:
            raise CallTraceError("Invalid bounded host pointer")
    elif set(value) != {"kind", "token"}:
        raise CallTraceError("Invalid opaque pointer")


def _context(event):
    return (event["edge"], event["function"], event["parent_invocation"], event["callsite"], event["relation"])


def compare_calls(original, native, registry):
    validate_registry(registry)
    validate_trace(original, registry, "original")
    validate_trace(native, registry, "native")
    left, right = original["events"], native["events"]
    count = max(len(left), len(right))
    for index in range(count):
        if index >= len(left):
            return _mismatch("extra_native_call", index, None, right[index])
        if index >= len(right):
            return _mismatch("missing_native_call", index, left[index], None)
        a, b = left[index], right[index]
        if _context(a) != _context(b):
            return _mismatch("call_context", index, a, b)
        if a["edge"] == "enter":
            function = next(item for item in registry["functions"] if item["id"] == a["function"])
            for ordinal, (actual, expected) in enumerate(zip(b["arguments"], a["arguments"])):
                if actual != expected:
                    contract = function["arguments"][ordinal]
                    return _mismatch("argument", index, a, b, {"ordinal": ordinal, "name": contract["name"], "expected": expected, "actual": actual})
        elif a["return"] != b["return"]:
            return _mismatch("return", index, a, b, {"expected": a["return"], "actual": b["return"]})
    return {"schema": 1, "contract": "xport-call-match-v1", "status": "match", "events": len(left)}


def _mismatch(kind, index, original, native, detail=None):
    result = {"schema": 1, "contract": "xport-call-match-v1", "status": "mismatch", "kind": kind,
              "event_index": index, "original": original, "native": native}
    if detail is not None:
        result["detail"] = detail
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--registry", type=Path, required=True)
    parser.add_argument("--original", type=Path, required=True)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    registry = validate_registry(json.loads(args.registry.read_text(encoding="utf-8")))
    original = json.loads(args.original.read_text(encoding="utf-8"))
    native = json.loads(args.native.read_text(encoding="utf-8"))
    result = compare_calls(original, native, registry)
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(result, stream, indent=2)
        stream.write("\n")
    print(json.dumps(result, separators=(",", ":")))
    return 0 if result["status"] == "match" else 1


if __name__ == "__main__":
    raise SystemExit(main())

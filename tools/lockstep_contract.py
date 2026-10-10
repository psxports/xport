"""Strict contracts shared by the lockstep recorder and supervisor"""
import hashlib
import json
import re


SCHEMA = 1
BUILD_CONTRACT = "xport-lockstep-build-v1"
PEER_CONTRACT = "xport-lockstep-peer-v1"
EVENT_CONTRACT = "xport-lockstep-event-v2"
EXCEPTION_CONTRACT = "xport-lockstep-exceptions-v1"
REGION_SIZES = {"dram": 0x200000, "scratchpad": 0x400}
EVENT_KINDS = {"PRE_CALL", "POST_CALL", "TAIL_CALL", "VBLANK", "MODE_TRANSITION",
               "PROCESS_EXIT", "FAULT"}
LOCKSTEP_MODES = {"DETAILED", "PREFIX_GUARD"}
EXCEPTION_OWNERS = {"xport", "game"}
EXCEPTION_KINDS = {"guest_stack", "firmware_runtime", "trusted_device_state"}
SIDES = {"original", "native"}
HEX32 = re.compile(r"^0x[0-9a-fA-F]{1,8}$")
SHA256 = re.compile(r"^[0-9a-f]{64}$")
RUN_ID = re.compile(r"^[0-9a-f]{32}$")


class ContractError(ValueError):
    pass


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def identity(value):
    return hashlib.sha256(canonical(value).encode("utf-8")).hexdigest()


def require_object(value, name):
    if not isinstance(value, dict):
        raise ContractError(f"{name} must be an object")
    return value


def exact_keys(value, required, optional=()):
    keys = set(value)
    required = set(required)
    optional = set(optional)
    missing = sorted(required - keys)
    unknown = sorted(keys - required - optional)
    if missing:
        raise ContractError("Missing fields: " + ", ".join(missing))
    if unknown:
        raise ContractError("Unknown fields: " + ", ".join(unknown))


def integer(value, name, minimum=0):
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        raise ContractError(f"{name} must be an integer >= {minimum}")
    return value


def address(value, name, nullable=False):
    if value is None and nullable:
        return None
    if not isinstance(value, str) or not HEX32.fullmatch(value):
        raise ContractError(f"{name} must be a canonical 32-bit hex address")
    return int(value, 16)


def digest(value, name):
    if not isinstance(value, str) or not SHA256.fullmatch(value):
        raise ContractError(f"{name} must be a lowercase SHA-256")
    return value


def validate_build(value):
    value = require_object(value, "build")
    exact_keys(value, ("schema", "contract", "lockstep_debug", "platform", "architecture",
                       "binary_sha256", "pdb_sha256", "source_identity"))
    if value["schema"] != SCHEMA or value["contract"] != BUILD_CONTRACT:
        raise ContractError("Unsupported build contract")
    if value["lockstep_debug"] is not True:
        raise ContractError("LOCKSTEP_DEBUG is required")
    if value["platform"] != "windows" or value["architecture"] != "x86":
        raise ContractError("Lockstep build must be Windows x86")
    digest(value["binary_sha256"], "binary_sha256")
    digest(value["pdb_sha256"], "pdb_sha256")
    digest(value["source_identity"], "source_identity")
    return value


def mapping_name(run_id, region):
    if not isinstance(run_id, str) or not RUN_ID.fullmatch(run_id):
        raise ContractError("run_id must contain 32 lowercase hex digits")
    if region not in {*REGION_SIZES, "deferred"}:
        raise ContractError("Unknown lockstep memory region")
    return f"Local\\xport-lockstep-{run_id}-{region}"


def validate_peer(value):
    value = require_object(value, "peer")
    exact_keys(value, ("schema", "contract", "run_id", "dram", "scratchpad", "deferred", "sizes"))
    if value["schema"] != SCHEMA or value["contract"] != PEER_CONTRACT:
        raise ContractError("Unsupported peer contract")
    run_id = value["run_id"]
    if not isinstance(run_id, str) or not RUN_ID.fullmatch(run_id):
        raise ContractError("Invalid run_id")
    sizes = require_object(value["sizes"], "sizes")
    exact_keys(sizes, REGION_SIZES)
    for region, size in REGION_SIZES.items():
        if sizes[region] != size:
            raise ContractError(f"Invalid {region} size")
        if value[region] != mapping_name(run_id, region):
            raise ContractError(f"Invalid {region} mapping name")
    if value["deferred"] != mapping_name(run_id, "deferred"):
        raise ContractError("Invalid deferred mapping name")
    return value


def validate_event(value):
    value = require_object(value, "event")
    required = ("schema", "contract", "side", "kind", "mode", "epoch", "event_sequence", "frame", "image",
                "function", "parent_invocation", "invocation", "depth", "call_ordinal",
                "callsite", "target", "return_pc")
    optional = ("instruction_word", "instruction", "delay_slot_pc", "delay_slot_word",
                "delay_slot", "source", "arguments", "return_value", "registers",
                "link_register", "link_value", "gte_identity", "controller_state")
    exact_keys(value, required, optional)
    if value["schema"] != SCHEMA or value["contract"] != EVENT_CONTRACT:
        raise ContractError("Unsupported event contract")
    if value["side"] not in SIDES or value["kind"] not in EVENT_KINDS:
        raise ContractError("Unknown event side or kind")
    if value["mode"] not in LOCKSTEP_MODES:
        raise ContractError("Unknown lockstep mode")
    for field in ("epoch", "event_sequence", "frame", "parent_invocation", "invocation", "depth", "call_ordinal"):
        integer(value[field], field)
    if not isinstance(value["image"], str) or not value["image"]:
        raise ContractError("image must be nonempty")
    for field in ("function", "callsite", "target", "return_pc"):
        address(value[field], field, nullable=True)
    if value["kind"] in {"PRE_CALL", "POST_CALL", "TAIL_CALL"}:
        for field in ("function", "target"):
            if value[field] is None:
                raise ContractError(f"{field} is required for call events")
    if value["side"] == "original" and value["kind"] in {"PRE_CALL", "TAIL_CALL"}:
        for field in ("instruction_word", "instruction", "delay_slot_pc", "delay_slot_word", "delay_slot"):
            if field not in value:
                raise ContractError(f"{field} is required for original call events")
    if "link_register" in value:
        integer(value["link_register"], "link_register")
        integer(value["link_value"], "link_value")
    if "gte_identity" in value and (not isinstance(value["gte_identity"], str) or
                                     not re.fullmatch(r"[0-9a-f]{16}", value["gte_identity"])):
        raise ContractError("gte_identity must contain 16 lowercase hex digits")
    if value["kind"] == "VBLANK":
        controllers = value.get("controller_state")
        if not isinstance(controllers, list) or len(controllers) != 2:
            raise ContractError("VBLANK requires two controller states")
        for controller in controllers:
            controller = require_object(controller, "controller_state")
            exact_keys(controller, ("type", "buttons", "analog"))
            if controller["type"] not in {"disconnected", "digital", "analog"}:
                raise ContractError("Unsupported controller type")
            buttons = integer(controller["buttons"], "buttons")
            analog = integer(controller["analog"], "analog")
            if buttons > 0xffff or analog > 0xffffffff:
                raise ContractError("Controller state is out of range")
    if value["kind"] == "MODE_TRANSITION":
        source = require_object(value.get("source"), "source")
        exact_keys(source, ("previous_mode", "next_mode", "target_frame"),
                   ("transport_image", "image_binding"))
        if "transport_image" in source or "image_binding" in source:
            if value["side"] != "original" or source.get("transport_image") != "MAIN.EXE":
                raise ContractError("Invalid original transport image binding")
            binding = require_object(source.get("image_binding"), "image_binding")
            exact_keys(binding, ("record_id", "executable_sha256", "patch_sha256"))
            for field in ("record_id", "executable_sha256", "patch_sha256"):
                digest(binding[field], "image_binding." + field)
        if source["previous_mode"] not in LOCKSTEP_MODES or source["next_mode"] not in LOCKSTEP_MODES:
            raise ContractError("Unknown transition mode")
        if source["previous_mode"] == source["next_mode"]:
            raise ContractError("Mode transition must change mode")
        integer(source["target_frame"], "target_frame")
    return value


def validate_exceptions(value):
    value = require_object(value, "exceptions")
    exact_keys(value, ("schema", "contract", "image_sha256", "ranges"))
    if value["schema"] != SCHEMA or value["contract"] != EXCEPTION_CONTRACT:
        raise ContractError("Unsupported exception contract")
    digest(value["image_sha256"], "image_sha256")
    if not isinstance(value["ranges"], list) or not value["ranges"]:
        raise ContractError("At least one memory exception range is required")
    prior_end = {region: 0 for region in REGION_SIZES}
    prior_region = None
    for index, row in enumerate(value["ranges"]):
        row = require_object(row, f"ranges[{index}]")
        exact_keys(row, ("region", "offset", "size", "kind", "owner", "name"))
        region = row["region"]
        if region not in REGION_SIZES:
            raise ContractError("Invalid memory exception region")
        offset = integer(row["offset"], "offset")
        size = integer(row["size"], "size", 1)
        if offset + size > REGION_SIZES[region]:
            raise ContractError("Memory exception is outside the region")
        if row["kind"] not in EXCEPTION_KINDS or row["owner"] not in EXCEPTION_OWNERS:
            raise ContractError("Invalid memory exception classification")
        if not isinstance(row["name"], str) or not row["name"]:
            raise ContractError("Memory exception name must be nonempty")
        if prior_region is not None and region < prior_region:
            raise ContractError("Memory exceptions are not sorted by region")
        if offset < prior_end[region]:
            raise ContractError("Memory exceptions overlap or are unsorted")
        prior_end[region] = offset + size
        prior_region = region
    return value


def exceptions_identity(value):
    validate_exceptions(value)
    return identity(value)


def parse_compare_reply(reply, request_id):
    if not isinstance(reply, str) or not reply:
        raise ContractError("Empty compare reply")
    parts = reply.split(":")
    if len(parts) == 2 and parts[0] == "OK":
        if parts[1] != request_id:
            raise ContractError("Compare request ID mismatch")
        return {"status": "match", "request_id": request_id}
    if len(parts) == 3 and parts[0] == "DEFERRED":
        if parts[1] != request_id:
            raise ContractError("Deferred compare request ID mismatch")
        try:
            generation = int(parts[2], 16)
        except ValueError as exc:
            raise ContractError("Invalid deferred compare generation") from exc
        if generation < 1 or generation > 0xffffffff:
            raise ContractError("Deferred compare generation is out of range")
        return {"status": "deferred", "request_id": request_id,
                "generation": generation}
    if len(parts) == 3 and parts[0] == "RANGED":
        if parts[1] != request_id:
            raise ContractError("Ranged compare request ID mismatch")
        try:
            generation = int(parts[2], 16)
        except ValueError as exc:
            raise ContractError("Invalid ranged compare generation") from exc
        if generation < 1 or generation > 0xffffffff:
            raise ContractError("Ranged compare generation is out of range")
        return {"status": "ranged_match", "request_id": request_id,
                "generation": generation}
    if len(parts) == 7 and parts[0] == "DIFF":
        _, seen, region, offset_text, length_text, original, candidate = parts
        if seen != request_id or region not in REGION_SIZES:
            raise ContractError("Invalid compare difference identity")
        try:
            offset = int(offset_text, 16)
            length = int(length_text, 16)
        except ValueError as exc:
            raise ContractError("Invalid compare difference range") from exc
        if length < 1 or offset < 0 or offset + length > REGION_SIZES[region]:
            raise ContractError("Compare difference is outside the region")
        if len(original) != length * 2 or len(candidate) != length * 2:
            raise ContractError("Compare difference payload length mismatch")
        try:
            bytes.fromhex(original)
            bytes.fromhex(candidate)
        except ValueError as exc:
            raise ContractError("Compare difference payload is not hexadecimal") from exc
        return {"status": "difference", "request_id": request_id, "region": region,
                "offset": offset, "length": length, "original": original, "candidate": candidate}
    if len(parts) == 3 and parts[0] == "ERROR" and parts[1] == request_id and parts[2]:
        return {"status": "error", "request_id": request_id, "code": parts[2]}
    raise ContractError("Malformed compare reply")

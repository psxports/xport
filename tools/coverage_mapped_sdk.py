"""Prepare strict SDK additions for already mapped coverage owners"""

import copy
from pathlib import Path

from source_index import functions


def mapped_sdk_view(root, packet, proposal, changes):
    if packet.get("contract") != "xport-function-audit-packet-v1":
        raise ValueError("Mapped SDK additions require a coverage packet")
    if proposal.get("translations"):
        raise ValueError("Mapped SDK additions cannot include game translations")
    context = packet["source_context"]
    contexts = [context, *(context.get("audit_cluster") or [])]
    required = context.get("audit_contract", {}).get("required_functions") or []
    expected = {(r["image"], r["address"]) for r in required}
    rows = []
    for item in contexts:
        function, implementation = item["function"], item["implementation"]
        key = (function["image"], function["address"])
        relative = Path(implementation.get("path") or "")
        source = (Path(root) / relative).resolve()
        if (key not in expected or implementation.get("complete") is not True or
                relative.is_absolute() or ".." in relative.parts or
                relative.suffix.lower() != ".c" or
                not source.is_relative_to((Path(root) / "src").resolve()) or not source.is_file()):
            raise ValueError("Mapped SDK caller needs an existing complete owned C body")
        text = changes.get(source)
        if text is None:
            text = source.read_text(encoding="utf-8-sig")
        elif isinstance(text, bytes):
            text = text.decode("utf-8-sig").replace("\r\n", "\n")
        parsed = [f for f in functions(text, relative.as_posix())
                  if f["name"] == implementation.get("symbol")]
        if len(parsed) != 1 or parsed[0]["markers"] != [(key[1], key[0])]:
            raise ValueError("Mapped SDK caller symbol or original marker changed")
        body = parsed[0]
        rows.append({"image": key[0], "address": key[1], "symbol": body["name"],
                     "text": text[body["start"]:body["end"]]})
    if len(rows) != len(expected) or {(r["image"], r["address"]) for r in rows} != expected:
        raise ValueError("Mapped SDK caller view must cover the complete packet")
    view = copy.deepcopy(proposal)
    view["translations"] = rows
    return view


def prepare_mapped_sdk(root, packet, proposal, view, changes, shared_root, shared_before, solution):
    from coverage_sdk_translations import prepare_sdk_translations
    sdk_view = copy.deepcopy(view)
    sdk_view["edits"] = [e for e in proposal["edits"] if e["path"].startswith("@xport/")]
    shared_changes = prepare_sdk_translations(packet, sdk_view, shared_root, shared_before, solution, root)
    from coverage_game_frame_delegation import validate_frame_delegated_sources
    bindings = packet["source_context"]["audit_contract"]["required_functions"]
    validate_frame_delegated_sources(root, packet, view, shared_root,
                                     {**changes, **shared_changes}, bindings)
    from coverage_native_abi_frame import validate_native_abi_sources
    validate_native_abi_sources(root, packet, proposal, changes, bindings)
    from coverage_v8_batch12_scopes import validate_sources
    validate_sources(root, packet, proposal, changes, bindings)
    return shared_changes


def mapped_sdk_receipt(proposal):
    from coverage_sdk_system_delegation import delegation_summary
    from coverage_game_frame_delegation import frame_delegation_summary
    from coverage_missing_stubs import stub_summary
    fields = {"sdk_system_delegation_admissions": delegation_summary(proposal),
              "game_frame_delegation_admissions": frame_delegation_summary(proposal)}
    fields["game_frame_delegation_count"] = len(fields["game_frame_delegation_admissions"])
    if proposal.get("missing_stub_authorization"):
        fields.update(missing_stub_authorization=proposal["missing_stub_authorization"],
                      missing_stub_admissions=stub_summary(proposal),
                      system_translations=proposal.get("system_translations") or [])
        fields["missing_stub_count"] = len(fields["missing_stub_admissions"])
    if proposal.get("sdk_helper_corrections"):
        fields["sdk_helper_corrections"] = proposal["sdk_helper_corrections"]
    return fields

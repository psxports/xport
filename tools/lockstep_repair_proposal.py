"""Apply a model's read-only code proposal under supervisor ownership"""

from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path

from lockstep_audit import receipt_template
from lockstep_supervisor import changed_sources, source_snapshot


def replacement_bytes(current: bytes, before: str, after: str) -> tuple[bytes, bytes]:
    old = before.encode('utf-8')
    new = after.encode('utf-8')
    if current.count(old) == 1:
        return old, new
    old_lf = old.replace(b'\r\n', b'\n')
    new_lf = new.replace(b'\r\n', b'\n')
    old_crlf = old_lf.replace(b'\n', b'\r\n')
    if current.count(old_crlf) == 1:
        return old_crlf, new_lf.replace(b'\n', b'\r\n')
    if current.count(old_lf) == 1:
        return old_lf, new_lf
    return old, new


def apply_proposal(root: Path, source_roots: tuple[Path, ...], packet_path: Path,
                   before: dict, proposal: dict, audit_validator=None,
                   shared_validator=None, build_validator=None,
                   shared_root: Path | None = None, shared_before: dict | None = None,
                   post_apply=None, declaration_validator=None) -> dict:
    if not isinstance(proposal, dict):
        raise ValueError('Repair proposal must be an object')
    edits = proposal.get("edits")
    audits = proposal.get("function_audits")
    adapter_audits = proposal.get("adapter_audits") or []
    if (proposal.get("schema") != 1 or not isinstance(edits, list) or not edits or
            not isinstance(audits, list) or not isinstance(adapter_audits, list)):
        raise ValueError("Repair proposal requires edits and audit lists")
    replacements: dict[Path, bytes] = {}
    original: dict[Path, bytes] = {}
    project_paths: set[Path] = set()
    shared_paths: set[Path] = set()
    if (shared_root is None) != (shared_before is None):
        raise ValueError('Shared repair needs both source root and baseline')
    shared_root = shared_root.resolve() if shared_root is not None else None
    for edit in edits:
        if not isinstance(edit, dict) or set(edit) != {"path", "before", "after"}:
            raise ValueError("Invalid repair edit")
        relative = Path(edit["path"])
        is_shared = relative.parts[:1] == ('@xport',)
        owned = Path(*relative.parts[1:]) if is_shared else relative
        owner = shared_root if is_shared else root
        roots = (Path('src'),) if is_shared else source_roots
        if (relative.is_absolute() or ".." in relative.parts or
                relative.suffix.lower() not in {".c", ".h"} or owner is None or
                not any(owned.is_relative_to(base) for base in roots)):
            raise ValueError("Repair edit is outside owned C/H source")
        path = (owner / owned).resolve()
        if not path.is_relative_to(owner.resolve()) or not path.is_file():
            raise ValueError("Repair source is missing or escapes project")
        (shared_paths if is_shared else project_paths).add(path)
        if path not in original:
            original[path] = path.read_bytes()
        current = replacements.get(path, original[path])
        old, new = edit["before"], edit["after"]
        if not isinstance(old, str) or not old or not isinstance(new, str):
            raise ValueError("Repair edit needs literal source strings")
        encoded, replacement = replacement_bytes(current, old, new)
        if current.count(encoded) != 1:
            raise ValueError("Repair source anchor is not unique and exact")
        replacements[path] = current.replace(encoded, replacement, 1)
    if source_snapshot(root, source_roots)["identity"] != before["identity"]:
        raise ValueError("Source changed while preparing repair")
    if shared_root is not None and source_snapshot(shared_root, (Path('src'),))['identity'] != shared_before['identity']:
        raise ValueError('Shared source changed while preparing repair')
    if shared_validator is not None:
        shared_validator()
    packet = json.loads(packet_path.read_text(encoding="utf-8"))
    if shared_paths and (packet.get('identities') or {}).get(
            'shared_source_identity') != shared_before['identity']:
        raise ValueError('Shared repair requires a packet-bound source baseline')
    required = (packet.get("source_context", {}).get("audit_contract", {})
                .get("required_functions") or [])
    expected = {(row["image"], row["address"]) for row in required}
    received = {(row.get("image"), row.get("address")) for row in audits
                if isinstance(row, dict)}
    if len(audits) != len(expected) or received != expected:
        raise ValueError("Repair audit does not cover every required function")
    sdk_bodies = []
    if packet.get("sdk_dependencies") or proposal.get("sdk_audits"):
        if audit_validator is None:
            raise ValueError("SDK edits require the complete coverage audit validator")
        from coverage_sdk_edits import authorized_sdk_bodies
        sdk_bodies = authorized_sdk_bodies(packet, proposal, shared_root)
    adapter_bodies = []
    for audit in adapter_audits:
        required_adapter_keys = {"path", "symbol", "function_before", "function_sha256",
                                 "failure_signature", "evidence"}
        if (not isinstance(audit, dict) or
                not required_adapter_keys <= set(audit) or
                set(audit) - required_adapter_keys - {"scope", "kind"}):
            raise ValueError("Invalid adapter audit")
        relative = Path(audit["path"])
        scope = audit.get("scope", "project")
        kind = audit.get("kind", "function")
        symbol = audit["symbol"]
        body = audit["function_before"]
        evidence = audit["evidence"]
        is_shared = relative.parts[:1] == ('@xport',)
        owned = Path(*relative.parts[1:]) if is_shared else relative
        owner = shared_root if is_shared else root
        roots = (Path('src'),) if is_shared else source_roots
        if (scope not in {"project", "shared"} or kind not in {"function", "declaration"} or
                is_shared != (scope == "shared") or owner is None or
                relative.is_absolute() or ".." in relative.parts or
                owned.suffix.lower() not in {".c", ".h"} or
                not any(owned.is_relative_to(base) for base in roots) or
                not isinstance(symbol, str) or not symbol or
                not isinstance(body, str) or not body or
                (kind == "function" and
                 not re.search(r"\b" + re.escape(symbol) + r"\s*\(", body)) or
                (kind == "declaration" and
                 not re.search(r"\b" + re.escape(symbol) + r"\b", body)) or
                audit["failure_signature"] != packet.get("failure_signature") or
                not isinstance(evidence, list) or not evidence):
            raise ValueError("Adapter audit is not bound to owned source and failure")
        path = (owner / owned).resolve()
        if not path.is_relative_to(owner.resolve()):
            raise ValueError("Adapter audit source escapes its owner")
        current = original.get(path, path.read_bytes() if path.is_file() else b"")
        encoded, _ = replacement_bytes(current, body, body)
        if (current.count(encoded) != 1 or
                hashlib.sha256(body.encode("utf-8")).hexdigest() != audit["function_sha256"]):
            raise ValueError("Adapter audit function body does not match source")
        diagnostic_bound = False
        for item in evidence:
            if not isinstance(item, dict) or set(item) != {"path", "sha256"}:
                raise ValueError("Invalid adapter audit evidence")
            evidence_relative = Path(item["path"])
            evidence_path = (packet_path.parent / evidence_relative).resolve()
            if (evidence_relative.is_absolute() or ".." in evidence_relative.parts or
                    evidence_relative.suffix.lower() != ".json" or
                    not evidence_path.is_relative_to(packet_path.parent.resolve()) or
                    not evidence_path.is_file() or
                    hashlib.sha256(evidence_path.read_bytes()).hexdigest() != item["sha256"]):
                raise ValueError("Adapter audit evidence is missing or changed")
            if evidence_relative.as_posix() == "diagnostic-result.json":
                diagnostic = json.loads(evidence_path.read_text(encoding="utf-8"))
                event_frame = (packet.get("expected_original_event") or {}).get("frame")
                boundary = packet.get("last_matching_boundary") or {}
                expected_frame = (event_frame - 1 if type(event_frame) is int else
                                  (boundary.get("original") or {}).get("frame"))
                if (diagnostic.get("complete") is not True or
                        diagnostic.get("target_frame") != expected_frame):
                    raise ValueError("Adapter diagnostic is not bound to this boundary")
                diagnostic_bound = True
        if not diagnostic_bound:
            raise ValueError("Adapter audit requires the current diagnostic result")
        adapter_bodies.append((relative.as_posix(), body))
    contexts = [packet.get("source_context") or {},
                *((packet.get("source_context") or {}).get("audit_cluster") or [])]
    audited_bodies = list(sdk_bodies)
    for context in contexts:
        function = context.get("function") or {}
        implementation = context.get("implementation") or {}
        if ((function.get("image"), function.get("address")) not in expected or
                implementation.get("complete") is not True or
                not isinstance(implementation.get("path"), str) or
                not isinstance(implementation.get("text"), str)):
            continue
        audited_bodies.append((implementation["path"], implementation["text"]))
    for edit in edits:
        if not (any(path == edit["path"] and edit["before"] in body
                    for path, body in audited_bodies) or
                any(path == edit["path"] and edit["before"] in body
                    for path, body in adapter_bodies) or
                (declaration_validator is not None and declaration_validator(edit) is True)):
            raise ValueError("Repair edit is outside a required audited function")
    bindings = {(row['image'], row['address']): row for row in required}
    bound_audits = []
    for audit in audits:
        binding = bindings[(audit['image'], audit['address'])]
        bound = dict(audit)
        for key in ('function_sha256', 'mips_sha256', 'pseudocode_sha256',
                    'machine_audit_sha256'):
            if key in binding:
                if key in bound and bound[key] != binding[key]:
                    raise ValueError('Repair audit conflicts with packet ' + key)
                bound[key] = binding[key]
        bound_audits.append(bound)
    receipt_path = packet_path.with_suffix(".audit.json")
    previous_receipt = receipt_path.read_bytes() if receipt_path.is_file() else None
    post_result = None
    try:
        for path, data in replacements.items():
            path.write_bytes(data)
        if shared_validator is not None and not shared_paths:
            shared_validator()
        after = source_snapshot(root, source_roots)
        changed = changed_sources(before, after)
        expected_changes = {path.relative_to(root).as_posix() for path in project_paths}
        shared_after = (source_snapshot(shared_root, (Path('src'),))
                        if shared_root is not None else None)
        changed_shared = (changed_sources(shared_before, shared_after)
                          if shared_after is not None else set())
        expected_shared = {path.relative_to(shared_root).as_posix() for path in shared_paths}
        if ((not changed and not changed_shared) or changed != expected_changes or
                changed_shared != expected_shared):
            raise ValueError("Repair changed unexpected source")
        receipt = receipt_template(packet, after["identity"], changed)
        if shared_paths:
            receipt['shared_source_before_identity'] = shared_before['identity']
            receipt['shared_source_before_files'] = shared_before['files']
            receipt['shared_source_identity'] = shared_after['identity']
            receipt['changed_shared_files'] = sorted(changed_shared)
        receipt["function_audits"] = bound_audits
        receipt["sdk_audits"] = proposal.get("sdk_audits") or []
        receipt["adapter_audits"] = adapter_audits
        receipt_path.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
        if audit_validator is not None and not audit_validator(packet_path, after, changed):
            raise ValueError("Repair audit validation failed")
        if build_validator is not None and build_validator(after) is False:
            raise ValueError("Repair Debug build validation failed")
        if post_apply is not None:
            post_result = post_apply(after)
        return {"status": "patched", "origin": "supervisor_applied_model_code",
                "allowed_files": sorted(changed), "changed_files": sorted(changed),
                "shared_source_identity": shared_after['identity'] if shared_after else None,
                "changed_shared_files": sorted(changed_shared),
                "memory_exceptions": post_result}
    except BaseException as failure:
        rollback_errors = []
        restore = {**original, receipt_path: previous_receipt}
        for path, data in restore.items():
            try:
                if data is None:
                    path.unlink(missing_ok=True)
                else:
                    path.write_bytes(data)
            except BaseException as error:
                rollback_errors.append(str(path) + ": " + str(error))
        if rollback_errors:
            error = RuntimeError("Repair rollback failed: " + "; ".join(rollback_errors))
            error._coverage_source_rollback_complete = False
            raise error from failure
        failure._coverage_source_rollback_complete = True
        raise

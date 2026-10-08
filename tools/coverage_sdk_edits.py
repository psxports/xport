"""Authorize existing shared SDK bodies for a coverage supervisor transaction"""

import hashlib
from pathlib import Path

from source_index import functions
from coverage_missing_stubs import authorized_audit
from coverage_sdk_system_delegation import authorized_delegation


def authorized_sdk_bodies(packet, proposal, shared_root, project_root=None):
    dependencies = packet.get("sdk_dependencies") or []
    audits = proposal.get("sdk_audits") or []
    if not dependencies and not audits and not proposal.get("sdk_helper_corrections"):
        return []
    if packet.get("contract") != "xport-function-audit-packet-v1" or shared_root is None:
        raise ValueError("SDK edit authorization requires a coverage packet and shared source")
    required = {(r["binding"]["image"], r["binding"]["address"]): r["binding"] for r in dependencies}
    received = {(r.get("image"), r.get("address")) for r in audits if isinstance(r, dict)}
    if len(audits) != len(required) or received != set(required):
        raise ValueError("SDK edits require every packet-bound SDK audit")
    result = []
    for audit in audits:
        binding = required[(audit["image"], audit["address"])]
        for field in ("function_sha256", "mips_sha256", "pseudocode_sha256", "machine_audit_sha256"):
            if audit.get(field) != binding.get(field):
                raise ValueError("SDK edit audit conflicts with original " + field)
        waiver = authorized_audit(project_root, proposal, binding, audit, "sdk") if project_root else None
        delegation = authorized_delegation(project_root, proposal, binding, audit, packet)
        if (audit.get("complete_mips_audit") is not True or (audit.get("unresolved_semantics") != [] and waiver is None and delegation is None) or
                audit.get("pseudocode_used_as_baseline") is not binding.get("pseudocode_complete")):
            raise ValueError("SDK edit audit is incomplete")
        body = audit.get("function_before")
        if waiver is not None and body is not None:
            raise ValueError("An existing SDK implementation cannot become a missing stub")
        if body is None:
            continue
        path, symbol = audit.get("source_file"), audit.get("source_symbol")
        relative = Path(path) if isinstance(path, str) else Path()
        if (relative.parts[:2] != ("@xport", "src") or ".." in relative.parts or
                relative.suffix.lower() != ".c" or not isinstance(body, str) or not body):
            raise ValueError("SDK edit body is outside canonical shared C source")
        parsed = functions(body, path)
        if len(parsed) != 1 or parsed[0]["name"] != symbol:
            raise ValueError("SDK edit authorization needs exactly its named function")
        source = (shared_root / Path(*relative.parts[1:])).resolve()
        if not source.is_relative_to((shared_root / "src").resolve()) or not source.is_file():
            raise ValueError("SDK edit source is missing or escapes shared src")
        current = source.read_text(encoding="utf-8-sig")
        normalized = body.replace("\r\n", "\n")
        if (current.count(normalized) != 1 or
                hashlib.sha256(body.encode()).hexdigest() != audit.get("source_before_sha256")):
            raise ValueError("SDK edit body does not match current shared source")
        rows = [r for r in functions(current, path) if r["name"] == symbol]
        if len(rows) != 1 or current[rows[0]["start"]:rows[0]["end"]].strip() != normalized.strip():
            raise ValueError("SDK edit body is not the complete current function")
        result.append((path, body))
    from coverage_sdk_helper_corrections import authorized_helper_bodies
    result.extend(authorized_helper_bodies(packet, proposal, shared_root, project_root))
    return result

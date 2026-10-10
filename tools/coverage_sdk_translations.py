"""Validate packet-bound additions to compiled canonical shared SDK sources"""

from pathlib import Path
import re
import xml.etree.ElementTree as ET

from coverage_sdk_edits import authorized_sdk_bodies
from lockstep_supervisor import source_snapshot
from source_index import functions
from coverage_missing_stubs import authorized_audit, policy_entries, validate_stub_body
from coverage_sdk_system_delegation import authorized_delegation, validate_delegated_sources


def prepare_sdk_translations(packet, proposal, shared_root, shared_before, solution, project_root=None):
    project_root = project_root or solution.parents[3]
    rows = proposal.get("sdk_translations") or []
    bridges = proposal.get("system_translations") or []
    headers = proposal.get("sdk_header_edits") or []
    edits = proposal.get("edits") or []
    if not isinstance(rows, list) or not isinstance(bridges, list) or not isinstance(headers, list) or not isinstance(edits, list):
        raise ValueError("SDK additions and edits must be lists")
    waived = {(a.get("image"), a.get("address")) for a in proposal.get("sdk_audits") or []
              if a.get("replacement_kind") == "user_authorized_missing_stub"}
    if not rows and not bridges and not edits and not waived:
        if headers:
            raise ValueError("SDK header edits require admitted SDK translations")
        if proposal.get("sdk_helper_corrections"):
            raise ValueError("SDK helper correction requires its exact shared body edit")
        return {}
    if shared_root is None or shared_before is None:
        raise ValueError("SDK additions require packet-bound shared sources")
    expected = packet.get("identities", {}).get("shared_source_identity")
    if expected != shared_before["identity"] or source_snapshot(shared_root, (Path("src"),))["identity"] != expected:
        raise ValueError("Shared source changed after the coverage packet was created")
    authorized = authorized_sdk_bodies(packet, proposal, shared_root, project_root)
    dependencies = {(d["binding"]["image"], d["binding"]["address"]): d["binding"]
                    for d in packet.get("sdk_dependencies") or []}
    audits = {(a["image"], a["address"]): a for a in proposal.get("sdk_audits") or []}
    projects = list(solution.parent.glob("*.vcxproj"))
    if len(projects) != 1:
        raise ValueError("SDK additions need one unambiguous native project")
    project = projects[0]
    tree = ET.fromstring(project.read_text(encoding="utf-8-sig"))
    compiled = {(project.parent / e.attrib["Include"].replace("\\", "/")).resolve()
                for e in tree.iter() if e.tag.split("}")[-1] == "ClCompile" and "Include" in e.attrib}
    existing = {f["name"] for path in shared_before["files"]
                for f in functions((shared_root / path).read_text(encoding="utf-8-sig"), path)}
    changes, seen, symbols = {}, set(), set()

    def owned(path, suffix):
        relative = Path(path) if isinstance(path, str) else Path()
        source = (shared_root / Path(*relative.parts[1:])).resolve()
        if (relative.parts[:2] != ("@xport", "src") or ".." in relative.parts or
                relative.suffix != suffix or not source.is_relative_to((shared_root / "src").resolve()) or
                not source.is_file() or source.relative_to(shared_root).as_posix() not in shared_before["files"]):
            raise ValueError("SDK addition escapes existing packet-bound shared source")
        return source

    additions = {(r.get("image"), r.get("address")) for r in rows if isinstance(r, dict)}
    for key in waived - additions:
        if key not in dependencies:
            raise ValueError("Existing SDK stub waiver has no packet dependency")
        audit = audits[key]
        entry = authorized_audit(project_root, proposal, dependencies[key], audit, "sdk")
        source = owned(audit["source_file"], ".c")
        if source not in compiled:
            raise ValueError("Existing SDK stub is not registered for native compilation")
        current = source.read_text(encoding="utf-8-sig")
        matches = [f for f in functions(current, audit["source_file"])
                   if f["name"] == audit["source_symbol"]]
        if len(matches) != 1:
            raise ValueError("Existing SDK stub requires one exact canonical symbol")
        function = matches[0]
        validate_stub_body(current[function["start"]:function["end"]], entry)

    for row in rows:
        if not isinstance(row, dict) or set(row) != {"image", "address", "symbol", "path", "text"}:
            raise ValueError("Invalid SDK translation row")
        if type(row["address"]) is not int or not isinstance(row["text"], str) or not isinstance(row["symbol"], str):
            raise ValueError("Invalid SDK translation types")
        key = (row["image"], row["address"])
        if key not in dependencies or key in seen:
            raise ValueError("SDK translation must bind a unique packet dependency")
        binding, audit = dependencies[key], audits[key]
        waiver = authorized_audit(project_root, proposal, binding, audit, "sdk")
        aliases = {r["alias"] for r in binding.get("library_classification") or []}
        symbol = row["symbol"]
        if symbol not in aliases and not any(symbol == alias + "PSX" for alias in aliases):
            raise ValueError("SDK translation symbol differs from reviewed canonical alias")
        parsed = functions(row["text"], row["path"])
        if (len(parsed) != 1 or parsed[0]["name"] != symbol or symbol in existing or symbol in symbols or
                parsed[0]["markers"] not in ([], [(row["address"], row["image"])])):
            raise ValueError("SDK addition must contain one new canonical function without helpers")
        if row["text"][:parsed[0]["start"]].strip() or row["text"][parsed[0]["end"]:].strip():
            raise ValueError("SDK translation contains text outside its function")
        if audit.get("source_file") != row["path"] or audit.get("source_symbol") != symbol or audit.get("function_before") is not None:
            raise ValueError("SDK addition audit must name its new canonical implementation")
        if waiver is not None:
            validate_stub_body(row["text"], waiver)
        source = owned(row["path"], ".c")
        if source not in compiled:
            raise ValueError("SDK addition source is not registered for native compilation")
        original = changes.get(source, source.read_text(encoding="utf-8-sig"))
        changes[source] = original.rstrip() + "\n\n" + row["text"].strip() + "\n"
        seen.add(key)
        symbols.add(symbol)
    entries = policy_entries(project_root, proposal) if bridges else []
    contexts = [packet["source_context"], *(packet["source_context"].get("audit_cluster") or [])]
    for row in bridges:
        if not isinstance(row, dict) or set(row) != {"symbol", "path", "text"}:
            raise ValueError("Invalid missing system bridge translation")
        matches = [e for e in entries if e["kind"] == "system_bridge" and
                   e["symbol"] == row["symbol"] and e["source_file"] == row["path"]]
        if len(matches) != 1 or row["symbol"] in symbols:
            raise ValueError("Missing system bridge requires a unique authorized service")
        entry = matches[0]
        caller = [c for c in contexts if c["function"]["image"] == entry["caller_image"] and
                  c["function"]["address"] == entry["caller_address"] and
                  c["function"]["sha256"] == entry["caller_function_sha256"]]
        if len(caller) != 1:
            from coverage_sdk_nested_delegation import nested_caller_chain
            delegated_targets = [b for b in dependencies.values() if
                b.get("image") == entry["caller_image"] and b.get("caller_address") == entry["caller_address"] and
                b.get("caller_function_sha256") == entry["caller_function_sha256"] and
                any(a.get("replacement_kind") == "user_authorized_system_delegation" and
                    any(r.get("symbol") == row["symbol"] for r in a.get("system_delegations", []))
                    for a in proposal.get("sdk_audits", []) if
                    (a.get("image"), a.get("address")) == (b["image"], b["address"]))]
            if len(delegated_targets) != 1 or not nested_caller_chain(project_root, packet, proposal, delegated_targets[0]):
                raise ValueError("Missing system bridge has no exact required caller binding")
        game_bodies = [r["text"] for r in proposal.get("translations") or []
                       if r["image"] == entry["caller_image"] and r["address"] == entry["caller_address"]]
        if not any(re.search(r"\b" + re.escape(row["symbol"]) + r"\s*\(", text) for text in game_bodies):
            delegated = [a for key, a in audits.items() if
                         a.get("replacement_kind") == "user_authorized_system_delegation" and
                         any(e == entry for e in authorized_delegation(project_root, proposal, dependencies[key], a, packet))]
            if not delegated:
                raise ValueError("Missing system bridge is not referenced by its required game translation")
        validate_stub_body(row["text"], entry)
        source = owned(row["path"], ".c")
        if source not in compiled:
            raise ValueError("Missing system bridge source is not registered for native compilation")
        current = changes.get(source, source.read_text(encoding="utf-8-sig"))
        if row["symbol"] in existing:
            retained = [f for f in functions(current, row["path"]) if f["name"] == row["symbol"]]
            if len(retained) != 1:
                raise ValueError("Existing system bridge requires one exact compiled symbol")
            function = retained[0]
            body = current[function["start"]:function["end"]]
            validate_stub_body(body, entry)
            if body.strip() != row["text"].strip():
                raise ValueError("Retained system bridge text must match its exact current body")
            symbols.add(row["symbol"])
            continue
        changes[source] = current.rstrip() + "\n\n" + row["text"].strip() + "\n"
        symbols.add(row["symbol"])
    for edit in edits:
        if not isinstance(edit, dict) or set(edit) != {"path", "before", "after"}:
            raise ValueError("Invalid shared SDK body edit")
        source = owned(edit["path"], ".c")
        if source not in compiled:
            raise ValueError("Shared SDK edit source is not registered for native compilation")
        before, after = edit["before"], edit["after"]
        bodies = [body.replace("\r\n", "\n") for path, body in authorized if path == edit["path"]]
        if (not isinstance(before, str) or not before or not isinstance(after, str) or
                not any(before in body for body in bodies)):
            raise ValueError("Shared SDK edit anchor is outside its authorized complete body")
        body = next(body for body in bodies if before in body)
        replaced_body = body.replace(before, after)
        parsed_body = functions(replaced_body, edit["path"])
        if (len(parsed_body) != 1 or parsed_body[0]["name"] != functions(body, edit["path"])[0]["name"] or
                replaced_body[:parsed_body[0]["start"]].strip() or replaced_body[parsed_body[0]["end"]:].strip()):
            raise ValueError("Shared SDK body edit contains unaudited declarations or helpers")
        current = changes.get(source, source.read_text(encoding="utf-8-sig"))
        if current.count(before) != 1:
            raise ValueError("Shared SDK edit anchor is not unique")
        updated = current.replace(before, after)
        old_functions = {f["name"] for f in functions(current, edit["path"])}
        new_functions = {f["name"] for f in functions(updated, edit["path"])}
        if old_functions != new_functions:
            raise ValueError("Shared SDK body edit cannot add or remove functions")
        changes[source] = updated
    declared = set()
    for edit in headers:
        if not isinstance(edit, dict) or set(edit) != {"path", "before", "after"}:
            raise ValueError("Invalid SDK header edit")
        source = owned(edit["path"], ".h")
        before, after = edit["before"], edit["after"]
        if not isinstance(before, str) or not before or not isinstance(after, str) or not after.startswith(before):
            raise ValueError("SDK header edit must preserve its exact anchor")
        added = after[len(before):].strip()
        prototypes = re.findall(r"[A-Za-z_][A-Za-z_0-9 \t*]*\b([A-Za-z_]\w*)\s*\([^{};#]*\)\s*;", added)
        remainder = re.sub(r"[A-Za-z_][A-Za-z_0-9 \t*]*\b[A-Za-z_]\w*\s*\([^{};#]*\)\s*;", "", added).strip()
        if not prototypes or remainder or any(n not in symbols or n in declared for n in prototypes):
            raise ValueError("SDK header edit permits only admitted function prototypes")
        current = changes.get(source, source.read_text(encoding="utf-8-sig"))
        if current.count(before) != 1:
            raise ValueError("SDK header edit anchor is not unique")
        changes[source] = current.replace(before, after)
        declared.update(prototypes)
    validate_delegated_sources(project_root, packet, proposal, shared_root, changes)
    from coverage_sdk_helper_corrections import validate_helper_changes
    validate_helper_changes(packet, proposal, shared_root, project_root, changes)
    return {path: text.encode("utf-8") for path, text in changes.items()}

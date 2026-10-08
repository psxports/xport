"""Apply packet-bound new coverage translations in one rollback transaction"""

from pathlib import Path
import json
import os
import re
import xml.etree.ElementTree as ET

from lockstep_audit import receipt_template
from lockstep_supervisor import source_snapshot, changed_sources
from source_index import functions
from coverage_sdk_translations import prepare_sdk_translations
from coverage_missing_stubs import policy_entries, stub_summary, authorized_audit, validate_stub_body, system_function_allowed
from coverage_absent_devices import policy_entries as absent_policy_entries, authorized_audit as authorized_absent_audit, validate_noop_body, admission_summary as absent_admission_summary
from xport_project import load_project


def prepare_game_prototype_updates(root, packet, proposal):
    rows = proposal.get("game_prototype_updates") or []
    if not isinstance(rows, list):
        raise ValueError("Game prototype updates must be a list")
    if not rows:
        return {}
    snapshot = source_snapshot(root, (Path("src"),))
    if snapshot["identity"] != packet.get("source_identity"):
        raise ValueError("Game prototype updates require the unchanged packet source identity")
    context = packet["source_context"]
    missing = {(c["function"]["image"], c["function"]["address"]) for c in
               [context, *(context.get("audit_cluster") or [])]
               if c.get("implementation", {}).get("mapping_status") == "unmapped"}
    admitted = {}
    for row in proposal.get("translations") or []:
        if (row.get("image"), row.get("address")) not in missing:
            continue
        parsed = functions(row["text"], "translation")
        if len(parsed) == 1 and parsed[0]["name"] == row["symbol"]:
            opening = row["text"].index("{", parsed[0]["start"])
            admitted[row["symbol"]] = re.sub(r"\s+", "", row["text"][parsed[0]["start"]:opening]) + ";"
    changes, seen = {}, set()
    pattern = r"(?:extern\s+)?[A-Za-z_][A-Za-z_0-9 \t*]*\b([A-Za-z_]\w*)\s*\([^{};#]*\)\s*;"
    for declaration in re.finditer(pattern, proposal.get("declarations") or ""):
        if declaration.group(1) in admitted:
            normalized = re.sub(r"\s+", "", re.sub(r"^\s*extern\s+", "", declaration.group(0)))
            if normalized != admitted[declaration.group(1)]:
                raise ValueError("Proposed declaration conflicts with the exact translated ABI")
    for row in rows:
        if (not isinstance(row, dict) or not {"path", "before", "after"} <= set(row) or
                set(row) - {"path", "before", "after", "occurrence_count"}):
            raise ValueError("Invalid game prototype update")
        occurrence_count = row.get("occurrence_count", 1)
        if type(occurrence_count) is not int or occurrence_count not in (1, 2):
            raise ValueError("Game prototype occurrence count must be exactly 1 or 2")
        relative = Path(row["path"]) if isinstance(row["path"], str) else Path()
        path = (root / relative).resolve()
        if (relative.is_absolute() or ".." in relative.parts or relative.parts[:1] != ("src",) or
                relative.suffix not in {".c", ".h"} or not path.is_relative_to((root / "src").resolve()) or
                relative.as_posix() not in snapshot["files"]):
            raise ValueError("Game prototype update escapes packet-bound game source")
        before, after = row["before"], row["after"]
        old = re.fullmatch(pattern, before.strip()) if isinstance(before, str) else None
        new = re.fullmatch(pattern, after.strip()) if isinstance(after, str) else None
        if (old is None or new is None or old.group(1) != new.group(1) or new.group(1) not in admitted or
                (relative.as_posix(), new.group(1)) in seen or before == after):
            raise ValueError("Game prototype update must name one current unmapped translation")
        normalized = re.sub(r"\s+", "", re.sub(r"^\s*extern\s+", "", after.strip()))
        if normalized != admitted[new.group(1)]:
            raise ValueError("Game prototype update conflicts with the exact translated ABI")
        text = changes.get(path, path.read_bytes().decode("utf-8-sig"))
        if text.count(before) != occurrence_count:
            raise ValueError("Game prototype update anchor count differs from the exact requested count")
        masked = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                        lambda match: ' ' * len(match.group(0)), text, flags=re.S)
        existing = functions(text, relative.as_posix())
        for occurrence in re.finditer(re.escape(before), text):
            start = occurrence.start()
            prefix = text[text.rfind("\n", 0, start) + 1:start]
            if masked[start:start + len(before)] != before or re.match(r"\s*#", prefix):
                raise ValueError("Game prototype update anchor is not an actual C declaration")
            if any(f["start"] <= start < f["end"] or f["name"] == new.group(1) for f in existing):
                raise ValueError("Game prototype update cannot change an existing function body")
        updated = text.replace(before, after)
        if [text[f["start"]:f["end"]] for f in existing] != [updated[f["start"]:f["end"]] for f in functions(updated, relative.as_posix())]:
            raise ValueError("Game prototype update changed a function body")
        changes[path] = updated
        seen.add((relative.as_posix(), new.group(1)))
    return {path: text.encode("utf-8") for path, text in changes.items()}


def prepare_runtime_constant_updates(root, packet, proposal, base_changes=None):
    changes = dict(base_changes or {})
    rows = proposal.get("runtime_constant_updates") or []
    if not isinstance(rows, list):
        raise ValueError("Runtime constant updates must be a list")
    if not rows:
        return changes
    snapshot = source_snapshot(root, (Path("src"),))
    if snapshot["identity"] != packet.get("source_identity"):
        raise ValueError("Runtime constant updates require the unchanged packet source identity")
    _project_root, project = load_project(root)
    shared_root = Path(project["toolset"]).resolve().parent
    shared = source_snapshot(shared_root, (Path("src"),))
    if shared["identity"] != packet.get("identities", {}).get("shared_source_identity"):
        raise ValueError("Runtime constant update shared headers differ from the packet")
    context = packet["source_context"]
    contexts = [context, *(context.get("audit_cluster") or [])]
    required = {(b["image"], b["address"]): b for b in context["audit_contract"]["required_functions"]}
    admitted = {(r["image"], r["address"]): r for r in proposal.get("translations") or []}
    pattern = r"const\s+uint32\s+([A-Za-z_]\w*)\s*=\s*(0x[0-9A-Fa-f]+|[0-9]+)[uU]\s*;"
    seen = set()
    for row in rows:
        if not isinstance(row, dict) or set(row) != {"path", "before", "after", "image", "address", "function_sha256", "symbol", "value"}:
            raise ValueError("Invalid runtime constant update")
        if type(row["address"]) is not int or type(row["value"]) is not int:
            raise ValueError("Runtime constant binding and value must be integers")
        key = (row["image"], row["address"])
        binding = required.get(key)
        owner = next((c for c in contexts if (c["function"]["image"], c["function"]["address"]) == key), None)
        if (binding is None or binding["function_sha256"] != row["function_sha256"] or key not in admitted or
                owner is None or owner.get("implementation", {}).get("mapping_status") != "unmapped"):
            raise ValueError("Runtime constant needs its exact current unmapped getter binding")
        words = []
        for line in owner.get("mips", {}).get("lines", []):
            match = re.match(r"^[0-9A-Fa-f]{8}\s+((?:[0-9A-Fa-f]{2}\s+)+)", line)
            if match:
                raw = bytes.fromhex(match.group(1))
                if len(raw) % 4:
                    raise ValueError("Runtime constant getter has incomplete original words")
                words.extend(int.from_bytes(raw[i:i + 4], "little") for i in range(0, len(raw), 4))
        if (len(words) != 4 or words[0] >> 16 != 0x3C02 or words[1] >> 16 != 0x8C42 or
                words[2:] != [0x03E00008, 0]):
            raise ValueError("Runtime constant original is not an exact absolute word getter")
        low = words[1] & 0xFFFF
        value = (((words[0] & 0xFFFF) << 16) + (low if low < 0x8000 else low - 0x10000)) & 0xFFFFFFFF
        if row["value"] != value:
            raise ValueError("Runtime constant differs from the original getter address")
        translation = admitted[key]
        parsed = functions(translation["text"], "getter")
        if len(parsed) != 1 or parsed[0]["name"] != translation["symbol"] or parsed[0]["markers"] != [(key[1], key[0])]:
            raise ValueError("Runtime constant needs its exact admitted getter translation")
        text = translation["text"]
        body = text[text.index("{", parsed[0]["start"]) + 1:text.rfind("}")]
        body = re.sub(r'FUNCTION_MARKER\(0x[0-9A-Fa-f]+u,\s*"[^"\n]+"\)\s*;', '', body).strip()
        expression = re.fullmatch(r"return\s+r_u32\(\s*(0x[0-9A-Fa-f]+|[0-9]+)[uU]\s*\)\s*;", body)
        if expression is None or int(expression.group(1), 0) != value:
            raise ValueError("Runtime constant translation does not return the exact original word load")
        before, after = row["before"], row["after"]
        old = re.fullmatch(pattern, before.strip()) if isinstance(before, str) else None
        new = re.fullmatch(pattern, after.strip()) if isinstance(after, str) else None
        if (old is None or new is None or old.group(1) != row["symbol"] or new.group(1) != row["symbol"] or
                int(new.group(2), 0) != value or row["symbol"] in seen):
            raise ValueError("Runtime constant update must preserve its exact public const uint32 type and symbol")
        prototype = r"\bextern\s+const\s+uint32\s+" + re.escape(row["symbol"]) + r"\s*;"
        declared = False
        for name in shared["files"]:
            if name.endswith(".h"):
                header = (shared_root / name).read_text(encoding="utf-8-sig")
                header = re.sub(r"/\*.*?\*/|//[^\n]*", "", header, flags=re.S)
                declared |= re.search(prototype, header) is not None
        if not declared:
            raise ValueError("Runtime constant is not an existing public shared const uint32 hook")
        relative = Path(row["path"]) if isinstance(row["path"], str) else Path()
        path = (root / relative).resolve()
        if (relative.is_absolute() or ".." in relative.parts or relative.parts[:1] != ("src",) or
                relative.suffix not in {".c", ".h"} or not path.is_relative_to((root / "src").resolve()) or
                relative.as_posix() not in snapshot["files"]):
            raise ValueError("Runtime constant update escapes packet-bound game source")
        current = changes.get(path, path.read_bytes()).decode("utf-8-sig")
        if current.count(before) != 1 or before == after:
            raise ValueError("Runtime constant anchor must be unique and change its literal value")
        start = current.index(before)
        masked = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                        lambda match: ' ' * len(match.group(0)), current, flags=re.S)
        if (masked[start:start + len(before)] != before or
                re.match(r"\s*#", current[current.rfind("\n", 0, start) + 1:start]) or
                any(f["start"] <= start < f["end"] for f in functions(current, str(relative)))):
            raise ValueError("Runtime constant anchor is not a complete global declaration")
        changes[path] = current.replace(before, after).encode("utf-8")
        seen.add(row["symbol"])
    return changes


def prepare_game_updates(root, packet, proposal):
    return prepare_runtime_constant_updates(root, packet, proposal,
                                            prepare_game_prototype_updates(root, packet, proposal))


def prepare_translations(root, packet, proposal):
    rows = proposal.get("translations")
    edits = proposal.get("edits")
    shared_only = isinstance(edits, list) and all(
        isinstance(e, dict) and str(e.get("path", "")).startswith("@xport/src/") for e in edits)
    if proposal.get("schema") != 1 or not shared_only or not isinstance(rows, list) or not rows:
        raise ValueError("New translations require a nonempty translations list and only authorized shared SDK edits")
    context = packet["source_context"]
    contexts = [context, *(context.get("audit_cluster") or [])]
    missing = {(item["function"]["image"], item["function"]["address"]) for item in contexts
               if item.get("implementation", {}).get("mapping_status") == "unmapped"}
    received = [(item.get("image"), item.get("address")) for item in rows if isinstance(item, dict)]
    if len(received) != len(rows) or len(set(received)) != len(rows) or set(received) != missing:
        raise ValueError("Translations must cover every unmapped batch function exactly once")
    declarations = proposal.get("declarations", "")
    if not isinstance(declarations, str) or functions(declarations, "declarations"):
        raise ValueError("Translation declarations cannot contain function bodies")
    if proposal.get("missing_stub_authorization"):
        guarded = re.sub(r'^\s*#\s*include\s*<stdlib\.h>\s*$', '', declarations, flags=re.MULTILINE)
        if "#" in guarded:
            raise ValueError("Missing stub declarations permit only the standard stdlib.h include")
    bodies = []
    bindings = {(b["image"], b["address"]): b for b in context.get("audit_contract", {}).get("required_functions", [])}
    audits = {(a["image"], a["address"]): a for a in proposal.get("function_audits") or []}
    policy_entries(root, proposal)
    absent_policy_entries(root, proposal)
    for row in rows:
        if set(row) != {"image", "address", "symbol", "text"} or not isinstance(row["text"], str):
            raise ValueError("Invalid translation row")
        parsed = functions(row["text"], "translation")
        key = (row["address"], row["image"])
        if len(parsed) != 1 or parsed[0]["name"] != row["symbol"] or parsed[0]["markers"] != [key]:
            raise ValueError("Translation needs one function and its exact image/address marker")
        audit = audits.get((row["image"], row["address"]))
        if audit and audit.get("replacement_kind") == "user_authorized_guest_frame_delegation":
            from coverage_game_frame_delegation import authorized_frame_delegation
            binding = bindings.get((row["image"], row["address"]))
            if binding is None:
                raise ValueError("Guest-frame owner has no required original binding")
            authorized_frame_delegation(root, packet, proposal, binding, audit)
        if audit and audit.get("replacement_kind") == "user_authorized_absent_device":
            binding = bindings.get((row["image"], row["address"]))
            if binding is None:
                raise ValueError("Absent-device function has no required original binding")
            entry = authorized_absent_audit(root, proposal, binding, audit)
            validate_noop_body(row["text"], entry)
        if audit and audit.get("replacement_kind") == "user_authorized_missing_stub":
            binding = bindings.get((row["image"], row["address"]))
            if binding is None:
                raise ValueError("Missing system function has no required original binding")
            entry = authorized_audit(root, proposal, binding, audit, "system_function")
            original = next(c for c in contexts if c["function"]["image"] == row["image"] and c["function"]["address"] == row["address"])
            if not system_function_allowed(original):
                raise ValueError("Missing system stub original is not a checked syscall or BIOS thunk")
            text = re.sub(r'FUNCTION_MARKER\(0x[0-9A-Fa-f]+u,\s*"[^"\n]+"\)\s*;', '', row["text"])
            validate_stub_body(text, entry)
        bodies.append(row["text"].strip() + "\n")
    addition = "\n" + declarations.strip() + "\n\n" + "\n".join(bodies)
    plan = packet["module_plan"]
    active = root / plan["active_module"]
    prototype_changes = prepare_game_updates(root, packet, proposal)
    original = (prototype_changes[active].decode("utf-8") if active in prototype_changes else
                active.read_text(encoding="utf-8-sig") if active.is_file() else '#include "psx.h"\n')
    target = active
    header = '' if re.search(r'^\s*#\s*include\s*[<"]psx\.h[>"]', original, re.MULTILINE) else '#include "psx.h"\n'
    complete = header + original + addition
    if len(complete.splitlines()) > plan["line_limit"]:
        target = root / plan["next_module"]
        if target.exists():
            raise ValueError("Next coverage module already exists")
        complete = '#include "psx.h"\n' + addition
    if len(complete.splitlines()) > plan["line_limit"]:
        raise ValueError("Indivisible translation batch exceeds module line limit")
    return target, complete.encode("utf-8")


def apply_translations(root, packet_path, packet, before, proposal, validate, finalize, solution,
                       shared_root=None, shared_before=None):
    if source_snapshot(root, (Path("src"),))["identity"] != before["identity"]:
        raise ValueError("Source changed while preparing translations")
    target, data = prepare_translations(root, packet, proposal)
    game_changes = {**prepare_game_updates(root, packet, proposal), target: data}
    shared_changes = prepare_sdk_translations(packet, proposal, shared_root, shared_before, solution, root)
    from coverage_game_frame_delegation import validate_frame_delegated_sources
    frame_bindings = packet["source_context"].get("audit_contract", {}).get("required_functions", [])
    validate_frame_delegated_sources(root, packet, proposal, shared_root,
                                     {**game_changes, **shared_changes}, frame_bindings)
    receipt_path = packet_path.with_suffix(".audit.json")
    originals = {target: target.read_bytes() if target.is_file() else None,
                 receipt_path: receipt_path.read_bytes() if receipt_path.is_file() else None}
    originals.update({path: path.read_bytes() for path in game_changes if path != target})
    originals.update({path: path.read_bytes() for path in shared_changes})
    registration = None
    if not target.exists():
        projects = list(solution.parent.glob("*.vcxproj"))
        if len(projects) != 1:
            raise ValueError("New module needs one unambiguous native project")
        project = projects[0]
        originals[project] = project.read_bytes()
        text = originals[project].decode("utf-8-sig")
        tree = ET.fromstring(text)
        ns = {"m": "http://schemas.microsoft.com/developer/msbuild/2003"}
        includes = [item.attrib.get("Include", "") for item in tree.findall(".//m:ClCompile", ns)]
        relative = os.path.relpath(target, project.parent)
        if any(value.replace("/", "\\").lower() == relative.lower() for value in includes):
            raise ValueError("New module is already registered")
        if text.count("</Project>") != 1:
            raise ValueError("Invalid native project closing tag")
        registration = text.replace("</Project>",
            '<ItemGroup><ClCompile Include="' + relative + '" /></ItemGroup>\n</Project>').encode("utf-8")
    try:
        for path, content in game_changes.items():
            path.write_bytes(content)
        for path, content in shared_changes.items():
            path.write_bytes(content)
        if registration is not None:
            project.write_bytes(registration)
        after = source_snapshot(root, (Path("src"),))
        changed = changed_sources(before, after)
        receipt = receipt_template(packet, after["identity"], changed)
        receipt["function_audits"] = proposal.get("function_audits")
        from coverage_game_frame_delegation import frame_delegation_summary
        receipt["game_frame_delegation_admissions"] = frame_delegation_summary(proposal)
        receipt["game_frame_delegation_count"] = len(receipt["game_frame_delegation_admissions"])
        receipt["sdk_audits"] = proposal.get("sdk_audits") or []
        if proposal.get("sdk_helper_corrections"):
            receipt["sdk_helper_corrections"] = proposal["sdk_helper_corrections"]
        from coverage_sdk_system_delegation import delegation_summary
        receipt["sdk_system_delegation_admissions"] = delegation_summary(proposal)
        if proposal.get("absent_device_authorization"):
            receipt["absent_device_authorization"] = proposal["absent_device_authorization"]
            receipt["absent_device_admissions"] = absent_admission_summary(proposal)
            receipt["absent_device_count"] = len(receipt["absent_device_admissions"])
        if proposal.get("game_prototype_updates"):
            receipt["game_prototype_updates"] = proposal["game_prototype_updates"]
        if proposal.get("runtime_constant_updates"):
            receipt["runtime_constant_updates"] = proposal["runtime_constant_updates"]
        if proposal.get("missing_stub_authorization"):
            receipt["missing_stub_authorization"] = proposal["missing_stub_authorization"]
            receipt["missing_stub_admissions"] = stub_summary(proposal)
            receipt["missing_stub_count"] = len(receipt["missing_stub_admissions"])
            receipt["system_translations"] = proposal.get("system_translations") or []
        if shared_changes:
            shared_after = source_snapshot(shared_root, (Path("src"),))
            changed_shared = changed_sources(shared_before, shared_after)
            expected_shared = {p.relative_to(shared_root).as_posix() for p in shared_changes}
            if changed_shared != expected_shared:
                raise ValueError("SDK transaction changed unexpected shared source")
            receipt["shared_source_before_identity"] = shared_before["identity"]
            receipt["shared_source_before_files"] = shared_before["files"]
            receipt["shared_source_identity"] = shared_after["identity"]
            receipt["changed_shared_files"] = sorted(changed_shared)
        if registration is not None:
            receipt["registered_project"] = project.relative_to(root).as_posix()
        receipt_path.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
        if validate(packet_path, {**after, "root": str(root)}, changed) is False:
            raise ValueError("Coverage translation audit validation failed")
        result = finalize()
        return {"status": "patched", "origin": "supervisor_applied_model_code",
                "allowed_files": sorted(changed), "changed_files": sorted(changed),
                "shared_source_identity": shared_after["identity"] if shared_changes else None,
                "changed_shared_files": sorted(changed_shared) if shared_changes else [],
                "memory_exceptions": result}
    except BaseException as failure:
        rollback_errors = []
        for path, original in originals.items():
            try:
                if original is None:
                    path.unlink(missing_ok=True)
                else:
                    path.write_bytes(original)
            except BaseException as error:
                rollback_errors.append(str(path) + ": " + str(error))
        if rollback_errors:
            error = RuntimeError("Translation rollback failed: " + "; ".join(rollback_errors))
            error._coverage_source_rollback_complete = False
            raise error from failure
        failure._coverage_source_rollback_complete = True
        raise

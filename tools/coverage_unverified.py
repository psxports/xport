"""Prepare project-authorized translations without claiming semantic proofs"""

from coverage_permissions import coverage_permissions
from source_index import functions


def prepare_unverified_batch(project, packet, proposal):
    if not coverage_permissions(project)["allow_unverified_translation"]:
        raise ValueError("Selected project does not authorize unverified translation")
    if proposal.get("schema") != 1 or proposal.get("mode") != "unverified_translation":
        raise ValueError("Expected an explicit unverified_translation proposal")
    context = packet["source_context"]
    contexts = [context, *(context.get("audit_cluster") or [])]
    required = {(item["function"]["image"], item["function"]["address"]): item
                for item in contexts}
    rows = proposal.get("functions")
    if not isinstance(rows, list) or len(rows) != len(required):
        raise ValueError("Translation must contain the complete recorded packet")
    prepared = []
    seen = set()
    for row in rows:
        if not isinstance(row, dict) or set(row) != {"image", "address", "symbol", "text"}:
            raise ValueError("Invalid unverified translation row")
        key = (row["image"], row["address"])
        if type(row["address"]) is not int or key not in required or key in seen:
            raise ValueError("Duplicate or foreign translation identity")
        if not isinstance(row["text"], str):
            raise ValueError("Translation body must be C text")
        parsed = functions(row["text"], "unverified_translation")
        if (len(parsed) != 1 or parsed[0]["name"] != row["symbol"] or
                parsed[0]["markers"] != [(row["address"], row["image"]) ]):
            raise ValueError("Translation must bind one function to its exact marker")
        prepared.append({**row, "implementation": required[key].get("implementation", {}),
                         "proof_status": "pending", "verification_performed": False})
        seen.add(key)
    if seen != set(required):
        raise ValueError("Incomplete recorded packet")
    if proposal.get("function_audits") or proposal.get("sdk_audits"):
        raise ValueError("Unverified translations cannot carry proof claims")
    return prepared


def plan_source_changes(root, packet, prepared, declarations=""):
    from pathlib import Path

    root = Path(root).resolve()
    if not isinstance(declarations, str) or functions(declarations, "declarations"):
        raise ValueError("Declarations cannot contain function bodies")
    edits = {}
    additions = []
    for row in prepared:
        mapping = row["implementation"]
        if mapping.get("mapping_status") == "unmapped":
            additions.append(row["text"].strip())
            continue
        relative = mapping.get("path", "")
        path = (root / relative).resolve()
        if not path.is_relative_to(root / "src") or path.suffix != ".c":
            raise ValueError("Mapped translation must name a project C source")
        text = path.read_text(encoding="utf-8-sig")
        matches = [item for item in functions(text, relative)
                   if item["name"] == row["symbol"] and
                   item["markers"] == [(row["address"], row["image"])]]
        if len(matches) != 1:
            raise ValueError("Mapped translation has no unique original C body")
        item = matches[0]
        edits.setdefault(path, []).append((item["start"], item["end"], row["text"].strip()))
    changes = {}
    for path, replacements in edits.items():
        text = path.read_text(encoding="utf-8-sig")
        boundary = len(text)
        for start, end, replacement in sorted(replacements, reverse=True):
            if end > boundary:
                raise ValueError("Overlapping mapped replacements")
            text = text[:start] + replacement + text[end:]
            boundary = start
        changes[path] = text
    if additions or declarations.strip():
        plan = packet["module_plan"]
        active = (root / plan["active_module"]).resolve()
        following = (root / plan["next_module"]).resolve()
        if any(not path.is_relative_to(root / "src") or path.suffix != ".c"
               for path in (active, following)):
            raise ValueError("Module plan escapes project source")
        original = changes.get(active, active.read_text(encoding="utf-8-sig"))
        import re
        type_pattern = re.compile(r"typedef\s+struct\s+\w+\s*\{[^{}]*\}\s*(\w+)\s*;")
        known_types = {}
        def retain_type(match):
            name = match.group(1)
            normalized = re.sub(r"\s+", " ", match.group(0)).strip()
            if name in known_types:
                if known_types[name] != normalized:
                    raise ValueError("Conflicting translation context declaration: " + name)
                return ""
            known_types[name] = normalized
            return match.group(0)
        original = type_pattern.sub(retain_type, original)
        appended_declarations = type_pattern.sub(retain_type, declarations)
        addition = "\n" + appended_declarations.strip() + "\n\n" + "\n\n".join(additions) + "\n"
        target, text = active, original + addition
        if len(text.splitlines()) > plan["line_limit"]:
            if following.exists():
                raise ValueError("Next translation module already exists")
            addition = '\n' + declarations.strip() + '\n\n' + '\n\n'.join(additions) + '\n'
            target, text = following, '#include "psx.h"\n' + addition
        changes[target] = text
    if any(len(text.splitlines()) > packet["module_plan"]["line_limit"]
           for text in changes.values()):
        raise ValueError("Translation module exceeds its line limit")
    return {path: text.encode("utf-8") for path, text in changes.items()}


def write_source_transaction(changes, finalize):
    originals = {path: path.read_bytes() if path.exists() else None for path in changes}
    try:
        for path, data in changes.items():
            path.write_bytes(data)
        return finalize()
    except BaseException:
        for path, data in originals.items():
            if data is None:
                path.unlink(missing_ok=True)
            else:
                path.write_bytes(data)
        raise


def add_module_registration(root, solution, changes):
    import os
    import xml.etree.ElementTree as ET
    from pathlib import Path
    from xml.sax.saxutils import quoteattr

    root = Path(root).resolve()
    solution = Path(solution).resolve()
    new_modules = [path for path in changes if path.suffix == ".c" and not path.exists()]
    if not new_modules:
        return
    projects = list(solution.parent.glob("*.vcxproj"))
    if len(projects) != 1:
        raise ValueError("New translation module needs one native project")
    project = projects[0]
    if not project.is_relative_to(root / "src"):
        raise ValueError("Native project escapes consuming source")
    text = project.read_text(encoding="utf-8-sig")
    tree = ET.fromstring(text)
    ns = {"m": "http://schemas.microsoft.com/developer/msbuild/2003"}
    includes = {item.attrib.get("Include", "").replace("/", "\\").casefold()
                for item in tree.findall(".//m:ClCompile", ns)}
    rows = []
    for path in new_modules:
        relative = os.path.relpath(path, project.parent).replace("/", "\\")
        if relative.casefold() in includes:
            raise ValueError("New module is already registered")
        includes.add(relative.casefold())
        rows.append("<ClCompile Include=" + quoteattr(relative) + " />")
    if text.count("</Project>") != 1:
        raise ValueError("Invalid native project closing tag")
    addition = "<ItemGroup>" + "".join(rows) + "</ItemGroup>\n</Project>"
    changes[project] = text.replace("</Project>", addition).encode("utf-8")


def apply_unverified_sources(root, project, packet_path, packet, proposal, finalize):
    import json
    from pathlib import Path
    from lockstep_supervisor import source_snapshot

    root = Path(root).resolve()
    before = source_snapshot(root, (Path("src"),))
    if before["identity"] != packet.get("source_identity"):
        raise ValueError("Project source changed after packet creation")
    prepared = prepare_unverified_batch(project, packet, proposal)
    stub_scopes = admit_recorded_stubs(root, project, packet, proposal, prepared)
    support = prepare_support_functions(packet, proposal, prepared)
    changes = plan_source_changes(root, packet, prepared + support, proposal.get("declarations", ""))
    add_caller_edits(root, proposal, prepared, changes)
    if any(path.is_relative_to(root / "src") and path.suffix == ".c" and
           len(data.decode("utf-8").splitlines()) > packet["module_plan"]["line_limit"]
           for path, data in changes.items()):
        raise ValueError("Caller edits exceed the project module line limit")
    sdk_stubs = add_shared_stubs(root, project, packet, proposal, changes)
    add_project_header_edits(root, proposal, changes)
    add_module_registration(root, root / project["paths"]["native_solution"], changes)
    receipt_path = Path(packet_path).with_suffix(".translation.json")
    if not receipt_path.resolve().is_relative_to(root / "status"):
        raise ValueError("Translation receipt escapes project status")
    receipt = {
        "schema": 1,
        "contract": "xport-unverified-translation-v1",
        "coverage_name": packet["coverage_name"],
        "packet": str(Path(packet_path).relative_to(root)),
        "source_before_identity": before["identity"],
        "semantic_verification_performed": False,
        "accepted_proof_count": 0,
        "stub_replacements": stub_scopes,
        "sdk_stubs": sdk_stubs,
        "support_symbols": [row["symbol"] for row in support],
        "functions": [{"image": row["image"], "address": row["address"],
                       "symbol": row["symbol"], "proof_status": "pending"}
                      for row in prepared],
        "changed_files": sorted(str(path.relative_to(root)) if path.is_relative_to(root)
                                else "@xport/" + path.relative_to(Path(project["toolset"]).resolve().parent).as_posix()
                                for path in changes),
    }
    changes[receipt_path] = (json.dumps(receipt, indent=2) + "\n").encode("utf-8")
    return write_source_transaction(changes, lambda: finalize(receipt, prepared))


def admit_recorded_stubs(root, project, packet, proposal, prepared):
    import re
    from coverage_missing_stubs import policy_entries, validate_stub_body

    permissions = coverage_permissions(project)
    entries = policy_entries(root, proposal)
    scopes = proposal.get("stub_replacements", [])
    if not isinstance(scopes, list):
        raise ValueError("Stub replacements must be explicit scope rows")
    context = packet["source_context"]
    contexts = [context, *(context.get("audit_cluster") or [])]
    originals = {(item["function"]["image"], item["function"]["address"]): item["function"]
                 for item in contexts}
    bodies = {row["symbol"]: row for row in prepared}
    seen = set()
    for scope in scopes:
        if (not isinstance(scope, dict) or set(scope) != {
                "symbol", "original_equivalence", "waived_original_effects"} or
                scope["original_equivalence"] is not False or
                not isinstance(scope["waived_original_effects"], list) or
                not scope["waived_original_effects"] or
                not all(isinstance(effect, str) and effect for effect in scope["waived_original_effects"])):
            raise ValueError("Stub replacement needs explicit waived effects and no equivalence claim")
        symbol = scope["symbol"]
        if symbol not in bodies or symbol in seen:
            raise ValueError("Duplicate or foreign recorded stub")
        row = bodies[symbol]
        original = originals[(row["image"], row["address"])]
        matches = [entry for entry in entries if entry["kind"] == "system_function" and
                   entry["symbol"] == symbol and entry["image"] == row["image"] and
                   entry["address"] == row["address"] and
                   entry["function_sha256"] == original["sha256"]]
        if not permissions["allow_missing_bios_stubs"] or len(matches) != 1:
            raise ValueError("Recorded BIOS stub lacks project permission and exact authorization")
        text = re.sub(r'FUNCTION_MARKER\(0x[0-9A-Fa-f]+u,\s*"[^"\n]+"\)\s*;', '', row["text"])
        validate_stub_body(text, matches[0])
        seen.add(symbol)
    for row in prepared:
        if re.search(r"\babort\s*\(", row["text"]) and row["symbol"] not in seen:
            raise ValueError("An aborting recorded replacement needs explicit stub authorization")
    return scopes


def prepare_support_functions(packet, proposal, prepared):
    rows = proposal.get("support_functions", [])
    if not isinstance(rows, list):
        raise ValueError("Support functions must be explicit rows")
    callers = {(row["image"], row["address"]): row for row in prepared}
    symbols = {row["symbol"] for row in prepared}
    result = []
    for row in rows:
        if not isinstance(row, dict) or set(row) != {
                "symbol", "text", "caller_image", "caller_address"}:
            raise ValueError("Invalid support function")
        if row["symbol"] in symbols or not isinstance(row["text"], str):
            raise ValueError("Duplicate or invalid support function")
        parsed = functions(row["text"], "support")
        caller = callers.get((row["caller_image"], row["caller_address"]))
        if len(parsed) != 1 or parsed[0]["name"] != row["symbol"] or parsed[0]["markers"]:
            raise ValueError("Support function must have one symbol and no recorded-function marker")
        if caller is None or row["symbol"] not in functions(caller["text"], "caller")[0]["identifiers"]:
            raise ValueError("Support function has no direct required caller binding")
        if "abort" in parsed[0]["identifiers"]:
            raise ValueError("Missing-call support stubs require a separate authorized admission")
        symbols.add(row["symbol"])
        result.append({**row, "implementation": {"mapping_status": "unmapped"}})
    return result


def add_project_header_edits(root, proposal, changes):
    from pathlib import Path

    root = Path(root).resolve()
    rows = proposal.get("header_edits", [])
    if not isinstance(rows, list):
        raise ValueError("Header edits must be explicit rows")
    for row in rows:
        if not isinstance(row, dict) or set(row) != {"path", "before", "after"}:
            raise ValueError("Invalid project header edit")
        path = (root / row["path"]).resolve()
        if not path.is_relative_to(root / "src") or path.suffix != ".h" or not path.is_file():
            raise ValueError("Header edit must name an existing project source header")
        if (not isinstance(row["before"], str) or not row["before"] or
                not isinstance(row["after"], str) or functions(row["after"], "header")):
            raise ValueError("Header edits cannot introduce function bodies")
        text = changes[path].decode("utf-8") if path in changes else path.read_text(encoding="utf-8-sig")
        if text.count(row["before"]) != 1:
            raise ValueError("Header edit anchor is stale or ambiguous")
        changes[path] = text.replace(row["before"], row["after"]).encode("utf-8")


def add_shared_stubs(root, project, packet, proposal, changes):
    import json
    import re
    from pathlib import Path
    from coverage_missing_stubs import validate_stub_body
    from lockstep_supervisor import source_snapshot

    rows = proposal.get("sdk_stubs", [])
    if not rows:
        return []
    if not isinstance(rows, list) or not coverage_permissions(project)["allow_missing_sdk_stubs"]:
        raise ValueError("Selected project does not authorize SDK stubs")
    shared = Path(project["toolset"]).resolve().parent
    before = source_snapshot(shared, (Path("src"),))
    if before["identity"] != packet.get("identities", {}).get("shared_source_identity"):
        raise ValueError("Shared source changed after packet creation")
    classification_path = (Path(root) / project["paths"]["sdk_classification"]).resolve()
    if not classification_path.is_relative_to(Path(root).resolve()):
        raise ValueError("SDK classification escapes consuming project")
    accepted = json.loads(classification_path.read_text(encoding="utf-8-sig"))["accepted"]
    context = packet["source_context"]
    callers = [context, *(context.get("audit_cluster") or [])]
    symbols = set()
    prototypes = []
    for row in rows:
        if not isinstance(row, dict) or set(row) != {
                "image", "address", "function_sha256", "symbol", "path", "prototype", "text"}:
            raise ValueError("Invalid SDK stub row")
        aliases = [item for item in accepted if item["image"] == row["image"] and
                   item["address"] == row["address"] and item["sha256"] == row["function_sha256"] and
                   any(match["name"] == row["symbol"] for match in item["matches"])]
        if len(aliases) != 1 or row["symbol"] in symbols:
            raise ValueError("SDK stub needs one reviewed canonical identity")
        needle = re.compile(r"\b(?:jal|j)\s+(?:sub|loc)_" + f'{row["address"]:08X}' + r"\b", re.I)
        if not any(item["function"]["image"] == row["image"] and
                   any(needle.search(line) for line in item.get("mips", {}).get("lines", []))
                   for item in callers):
            raise ValueError("SDK stub has no direct recorded caller")
        if not row["path"].startswith("@xport/src/"):
            raise ValueError("SDK stub must belong to shared source")
        path = (shared / row["path"][7:]).resolve()
        if not path.is_relative_to(shared / "src") or path.suffix != ".c" or not path.is_file():
            raise ValueError("SDK stub needs an existing shared C module")
        if any(item["name"] == row["symbol"] for source in (shared / "src").glob("*.c")
               for item in functions(source.read_text(encoding="utf-8-sig"), str(source))):
            raise ValueError("SDK stub cannot replace an existing shared implementation")
        validate_stub_body(row["text"], {"symbol": row["symbol"], "prototype": row["prototype"]})
        text = changes[path].decode("utf-8") if path in changes else path.read_text(encoding="utf-8-sig")
        changes[path] = (text.rstrip() + "\n\n" + row["text"].strip() + "\n").encode("utf-8")
        symbols.add(row["symbol"])
        prototypes.append(row["prototype"])
    header = shared / "src" / "psx.h"
    text = header.read_text(encoding="utf-8-sig")
    closing = text.rfind("#endif")
    if closing < 0:
        raise ValueError("Shared public header has no closing include guard")
    text = text[:closing] + "\n" + "\n".join(prototypes) + "\n\n" + text[closing:]
    changes[header] = text.encode("utf-8")
    return [{"image": row["image"], "address": row["address"], "symbol": row["symbol"],
             "original_equivalence": False} for row in rows]


def add_caller_edits(root, proposal, prepared, changes):
    from pathlib import Path

    root = Path(root).resolve()
    targets = {row["symbol"] for row in prepared}
    rows = proposal.get("caller_edits", [])
    if not isinstance(rows, list):
        raise ValueError("Caller edits must be explicit rows")
    for row in rows:
        if not isinstance(row, dict) or set(row) != {"path", "symbol", "before", "after"}:
            raise ValueError("Invalid caller edit")
        path = (root / row["path"]).resolve()
        if not path.is_relative_to(root / "src") or path.suffix != ".c" or not path.is_file():
            raise ValueError("Caller edit needs an existing project C source")
        before = functions(row["before"], "caller_before")
        after = functions(row["after"], "caller_after")
        if (len(before) != 1 or len(after) != 1 or before[0]["name"] != row["symbol"] or
                after[0]["name"] != row["symbol"] or not before[0]["markers"] or
                before[0]["markers"] != after[0]["markers"] or
                not targets.intersection(before[0]["identifiers"]) or
                not targets.intersection(after[0]["identifiers"])):
            raise ValueError("Caller correction must preserve one original binding and call a packet function")
        text = changes[path].decode("utf-8") if path in changes else path.read_text(encoding="utf-8-sig")
        if text.count(row["before"]) != 1:
            raise ValueError("Caller correction anchor is stale or ambiguous")
        changes[path] = text.replace(row["before"], row["after"]).encode("utf-8")
    import re
    type_rows = proposal.get("context_type_edits", [])
    if not isinstance(type_rows, list):
        raise ValueError("Context type edits must be explicit rows")
    for row in type_rows:
        if not isinstance(row, dict) or set(row) != {"path", "before", "after", "count"}:
            raise ValueError("Invalid context type edit")
        path = (root / row["path"]).resolve()
        if not path.is_relative_to(root / "src") or path.suffix != ".c" or not path.is_file():
            raise ValueError("Context type edit escapes project C sources")
        pattern = r"typedef\s+struct\s+(Game\w+CallContext)\s*\{(?:\s*uint32\s+\w+\s*;)+\s*\}\s*\1\s*;"
        before_type = re.fullmatch(pattern, row["before"].strip())
        after_type = re.fullmatch(pattern, row["after"].strip())
        moved_to_header = (before_type is not None and row["after"] == "" and
                           any(row["before"] in header.get("after", "") and
                               row["before"] not in header.get("before", "")
                               for header in proposal.get("header_edits", [])))
        if (not before_type or
                (not moved_to_header and
                 (not after_type or before_type.group(1) != after_type.group(1)))):
            raise ValueError("Context edit must preserve a plain structure or move it intact to a project header")
        text = changes[path].decode("utf-8") if path in changes else path.read_text(encoding="utf-8-sig")
        if type(row["count"]) is not int or row["count"] < 1 or text.count(row["before"]) != row["count"]:
            raise ValueError("Context type anchor is stale or ambiguous")
        changes[path] = text.replace(row["before"], row["after"]).encode("utf-8")
    declarations = proposal.get("prototype_edits", [])
    if not isinstance(declarations, list):
        raise ValueError("Prototype edits must be explicit rows")
    for row in declarations:
        if not isinstance(row, dict) or set(row) != {"path", "symbol", "before", "after"} or row["symbol"] not in targets:
            raise ValueError("Prototype edit must name a required packet function")
        path = (root / row["path"]).resolve()
        if not path.is_relative_to(root / "src") or path.suffix not in {".c", ".h"} or not path.is_file():
            raise ValueError("Prototype edit escapes existing project source")
        for declaration in (row["before"], row["after"]):
            if (not isinstance(declaration, str) or functions(declaration, "prototype") or
                    any(character in declaration for character in '{}#') or
                    not declaration.strip().endswith(';') or row["symbol"] + '(' not in declaration):
                raise ValueError("Expected one plain packet prototype")
        text = changes[path].decode("utf-8") if path in changes else path.read_text(encoding="utf-8-sig")
        if text.count(row["before"]) != 1:
            raise ValueError("Prototype edit anchor is stale or ambiguous")
        changes[path] = text.replace(row["before"], row["after"]).encode("utf-8")

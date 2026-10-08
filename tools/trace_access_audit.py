"""Inventory guest-memory and diagnostic access syntax for bounded migrations"""

import argparse
import hashlib
import json
import re
from pathlib import Path

from xport_project import artifact_path, atomic_json, load_project


SCHEMA = 2
FAMILIES = {
    "rr": {"byte_", "word_", "dword_", "FUNCTION_MARKER", "rr_trace_boundary", "rr_trace_replay",
           "memcpy", "memmove", "memset", "psx_addr", "DRAM", "SCRATCHPAD", "VRAM",
           "fopen", "xport_fopen", "fread", "fwrite"},
    "rrj": {"rrj_at", "rrj_read32", "rrj_write32", "rrj_u16", "rrj_u32", "rrj_put16", "rrj_put32",
            "FUNCTION_MARKER", "RRJ_WIP", "RRJ_WIP3", "rrj_wip_site", "rrj_wip_handoff",
            "rrj_wip_stop", "memcpy", "memmove", "memset", "psx_addr", "DRAM", "SCRATCHPAD", "VRAM",
            "fopen", "xport_fopen", "fread", "fwrite"},
}
DIRECT_REGIONS = {"DRAM", "SCRATCHPAD", "VRAM"}
COMMON_ACCESS = {"r_u8", "r_u16", "r_u32", "r_s8", "r_s16",
                 "r_s32", "w_u8", "w_u16", "w_u32",
                 "w_masked_u32", "xport_update_u8", "xport_update_u16", "xport_update_u32",
                 "xport_load_le16", "xport_load_le32", "xport_store_le16",
                 "xport_store_le32", "xport_guest_copy", "xport_guest_fill", "xport_guest_ptr", "xport_guest_cptr"}
for _family_names in FAMILIES.values():
    _family_names.update(COMMON_ACCESS)


class AuditError(ValueError):
    """Raised when an access audit cannot be trusted"""


def file_sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _tokens(text):
    result = []
    index = 0
    length = len(text)
    while index < length:
        character = text[index]
        if character.isspace():
            index += 1
            continue
        if text.startswith("//", index):
            end = text.find("\n", index + 2)
            index = length if end < 0 else end + 1
            continue
        if text.startswith("/*", index):
            end = text.find("*/", index + 2)
            if end < 0:
                raise AuditError("Unterminated block comment")
            index = end + 2
            continue
        if character in "\"'":
            quote = character
            start = index
            index += 1
            while index < length:
                if text[index] == "\\":
                    index += 2
                elif text[index] == quote:
                    index += 1
                    break
                else:
                    index += 1
            else:
                raise AuditError("Unterminated string or character literal")
            result.append(("literal", text[start:index], start, index))
            continue
        if character.isalpha() or character == "_":
            start = index
            index += 1
            while index < length and (text[index].isalnum() or text[index] == "_"):
                index += 1
            result.append(("identifier", text[start:index], start, index))
            continue
        matched = next((operator for operator in ("<<=", ">>=", "++", "--", "+=", "-=", "*=", "/=",
                                                   "%=", "&=", "|=", "^=", "==", "!=", "<=", ">=", "->",
                                                   "&&", "||", "<<", ">>") if text.startswith(operator, index)), None)
        if matched:
            result.append(("punct", matched, index, index + len(matched)))
            index += len(matched)
        else:
            result.append(("punct", character, index, index + 1))
            index += 1
    return result


def _calls(text, names):
    tokens = _tokens(text)
    pairs = {}
    brace_pairs = {}
    stack = []
    brace_stack = []
    for index, token in enumerate(tokens):
        if token[1] == "(":
            stack.append(index)
        elif token[1] == ")":
            if not stack:
                raise AuditError("Unbalanced closing parenthesis")
            opening = stack.pop()
            pairs[opening] = index
        elif token[1] == "{":
            brace_stack.append(index)
        elif token[1] == "}":
            if not brace_stack:
                raise AuditError("Unbalanced closing brace")
            brace_pairs[brace_stack.pop()] = index
    if stack:
        raise AuditError("Unbalanced opening parenthesis")
    if brace_stack:
        raise AuditError("Unbalanced opening brace")
    functions = _functions(text, tokens, pairs, brace_pairs)
    nodes = []
    for index, token in enumerate(tokens[:-1]):
        if token[0] != "identifier" or token[1] not in names or tokens[index + 1][1] != "(":
            continue
        closing = pairs.get(index + 1)
        if closing is None:
            raise AuditError(f"Unclosed call to {token[1]}")
        nodes.append({"name": token[1], "token": index, "close_token": closing,
                      "start": token[2], "end": tokens[closing][3]})
    call_tokens = {node["token"] for node in nodes}
    for index, token in enumerate(tokens):
        if token[0] == "identifier" and token[1] in DIRECT_REGIONS and index not in call_tokens:
            nodes.append({"name": token[1], "token": index, "close_token": index,
                          "start": token[2], "end": token[3], "arguments": []})
    nodes.sort(key=lambda item: (item["start"], -item["end"]))
    parent_stack = []
    for node in nodes:
        while parent_stack and node["start"] >= parent_stack[-1]["end"]:
            parent_stack.pop()
        node["parent"] = parent_stack[-1]["name"] if parent_stack and node["end"] <= parent_stack[-1]["end"] else None
        parent_stack.append(node)
        node["before"] = tokens[node["token"] - 1][1] if node["token"] else None
        node["before2"] = tokens[node["token"] - 2][1] if node["token"] > 1 else None
        node["after"] = tokens[node["close_token"] + 1][1] if node["close_token"] + 1 < len(tokens) else None
        if "arguments" not in node:
            node["arguments"] = _arguments(text, tokens, node["token"] + 1, node["close_token"])
        function = next((item for item in functions if item["start"] <= node["start"] < item["end"]), None)
        node["function_symbol"] = function["symbol"] if function else None
        node["function_address"] = function["address"] if function else None
    return nodes


def _functions(text, tokens, parenthesis_pairs, brace_pairs):
    reverse_parentheses = {closing: opening for opening, closing in parenthesis_pairs.items()}
    controls = {"if", "for", "while", "switch"}
    functions = []
    for opening_brace, closing_brace in brace_pairs.items():
        opening_parenthesis = reverse_parentheses.get(opening_brace - 1)
        if opening_parenthesis is None or opening_parenthesis == 0:
            continue
        name = tokens[opening_parenthesis - 1]
        if name[0] != "identifier" or name[1] in controls:
            continue
        body_end = tokens[closing_brace][3]
        body = text[tokens[opening_brace][2]:body_end]
        marker = re.search(r"\bFUNCTION_MARKER\s*\(\s*(0x[0-9A-Fa-f]+)[uUlL]*", body)
        suffix = re.search(r"_([0-9A-Fa-f]{8})$", name[1])
        address = marker.group(1).lower() if marker else ("0x" + suffix.group(1).lower() if suffix else None)
        functions.append({"symbol": name[1], "address": address, "start": name[2], "end": body_end})
    return functions


def _arguments(text, tokens, opening, closing):
    if closing == opening + 1:
        return []
    depth = 0
    starts = [tokens[opening][3]]
    spans = []
    for index in range(opening + 1, closing):
        value = tokens[index][1]
        if value in ("(", "[", "{"):
            depth += 1
        elif value in (")", "]", "}"):
            depth -= 1
        elif value == "," and depth == 0:
            spans.append((starts[-1], tokens[index][2]))
            starts.append(tokens[index][3])
    spans.append((starts[-1], tokens[closing][2]))
    return [text[start:end].strip() for start, end in spans]


def _classification(family, node):
    name = node["name"]
    arguments = node["arguments"]
    assignment = node["after"] in {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="}
    update = node["after"] in {"++", "--"} or node["before"] in {"++", "--"}
    if name in DIRECT_REGIONS:
        access = "region_extent" if node["before"] == "(" and node["before2"] == "sizeof" else "pointer_escape"
        return "common_access", access, True, name, None
    if name == "psx_addr":
        return "common_access", "pointer_escape", True, "psx_addr", None
    if name in {"xport_guest_ptr", "xport_guest_cptr"}:
        return "common_access", "pointer_escape", True, name, None
    if name in COMMON_ACCESS:
        access = "read" if name.startswith("r_") or "load" in name else "write"
        if name in {"xport_guest_copy", "xport_guest_fill"}:
            access = "bulk"
        return "common_access", access, True, name, None
    if family == "rr" and name in {"byte_", "word_", "dword_"}:
        width = {"byte_": 8, "word_": 16, "dword_": 32}[name]
        address_of = node["before"] == "&" and node["before2"] in {
            None, "(", "[", "{", ",", "=", ":", "?", ";", "return"
        }
        if address_of:
            return "data_access", "pointer_binding", True, f"xport_guest_ptr(ADDRESS, sizeof(uint{width}))", None
        if update or (assignment and node["after"] != "="):
            return "data_access", "compound_lvalue", True, f"xport_update_u{width}(ADDRESS, OPERATION, VALUE)", None
        if assignment:
            return "data_access", "write", True, f"w_u{width}({arguments[0]}, VALUE)", None
        return "data_access", "read", True, f"r_u{width}({arguments[0]})", None
    if family == "rrj" and name == "rrj_read32":
        return "common_access", "read", True, "r_u32(ADDRESS)", None
    if family == "rrj" and name == "rrj_write32":
        return "common_access", "write", True, "w_u32(ADDRESS, VALUE)", None
    if family == "rrj" and name in {"rrj_u16", "rrj_u32", "rrj_put16", "rrj_put32"}:
        replacement = {"rrj_u16": "xport_load_le16", "rrj_u32": "xport_load_le32",
                       "rrj_put16": "xport_store_le16", "rrj_put32": "xport_store_le32"}[name]
        return "common_access", "read" if name.startswith("rrj_u") else "write", True, replacement, None
    if family == "rrj" and name == "rrj_at":
        if node["parent"] in {"rrj_u16", "rrj_u32", "rrj_put16", "rrj_put32"}:
            return "common_access", "covered_pointer", True, "covered_by_packed_parent", None
        return "common_access", "pointer_escape", True, "rrj_at", None
    if name == "FUNCTION_MARKER":
        return "diagnostic", "function_marker", True, "XPORT_FUNCTION_MARKER(IMAGE, ADDRESS)", None
    if name in {"memcpy", "memmove", "memset"}:
        guest = any("psx_addr(" in argument or "xport_guest_ptr(" in argument or
                    "xport_guest_cptr(" in argument for argument in arguments)
        if guest:
            replacement = "xport_guest_fill" if name == "memset" else "xport_guest_copy"
            return "bulk_access", name, True, replacement, None
        return "common_access", "host_bulk", True, name, None
    if name in {"fopen", "xport_fopen", "fread", "fwrite"}:
        supported = name == "xport_fopen"
        replacement = "xport_fopen" if name == "fopen" else name
        reason = None if supported else "file_io_ownership_requires_B15A_review"
        return "diagnostic", "file_io", supported, replacement, reason
    if "trace" in name:
        return "diagnostic", "trace_runtime", False, None, "project_semantic_binding_or_later_B15A_infrastructure"
    if "wip" in name.lower() or name.startswith("RRJ_WIP"):
        return "diagnostic", "wip_failure", False, "XPORT_WIP_or_XPORT_FAILURE", "project_fallback_semantics_require_review"
    return "diagnostic", "other", False, None, "unclassified_candidate"


def audit_project(root, family):
    root = Path(root).resolve()
    if family not in FAMILIES:
        raise AuditError("Unknown project access family")
    source_root = root / "src"
    if not source_root.is_dir():
        raise AuditError("Project src directory is missing")
    entries = []
    files = []
    failures = []
    for path in sorted((*source_root.rglob("*.c"), *source_root.rglob("*.h"))):
        relative = path.relative_to(root).as_posix()
        data = path.read_bytes()
        try:
            text = data.decode("utf-8-sig")
            nodes = _calls(text, FAMILIES[family])
        except (UnicodeDecodeError, AuditError) as error:
            failures.append({"path": relative, "reason": str(error)})
            continue
        files.append({"path": relative, "sha256": hashlib.sha256(data).hexdigest(), "candidates": len(nodes)})
        for node in nodes:
            category, access, supported, replacement, reason = _classification(family, node)
            line = text.count("\n", 0, node["start"]) + 1
            prior_newline = text.rfind("\n", 0, node["start"])
            original = text[node["start"]:node["end"]]
            entries.append({
                "id": hashlib.sha256(f"{relative}\0{node['start']}\0{original}".encode("utf-8")).hexdigest(),
                "path": relative,
                "source_sha256": hashlib.sha256(data).hexdigest(),
                "span": {"start": node["start"], "end": node["end"], "line": line,
                         "column": node["start"] - prior_newline},
                "original_sha256": hashlib.sha256(original.encode("utf-8")).hexdigest(),
                "symbol": node["name"],
                "category": category,
                "access": access,
                "supported": supported,
                "replacement": replacement,
                "unsupported_reason": reason,
                "parent_symbol": node["parent"],
                "function_symbol": node["function_symbol"],
                "function_address": node["function_address"],
            })
    counts = {}
    for entry in entries:
        key = f"{entry['category']}:{entry['access']}"
        counts[key] = counts.get(key, 0) + 1
    return {
        "schema": SCHEMA,
        "status": "complete" if not failures else "incomplete",
        "mode": "plan",
        "family": family,
        "implementation_sha256": file_sha256(__file__),
        "project_root": str(root),
        "files": files,
        "entries": entries,
        "summary": {
            "files_scanned": len(files),
            "candidates": len(entries),
            "supported": sum(1 for item in entries if item["supported"]),
            "unresolved": sum(1 for item in entries if not item["supported"]),
            "by_kind": counts,
        },
        "failures": failures,
    }


def validate_report(report):
    required = {"schema", "status", "mode", "family", "implementation_sha256", "project_root", "files", "entries", "summary", "failures"}
    if not isinstance(report, dict) or set(report) != required or report.get("schema") != SCHEMA:
        raise AuditError("Invalid access audit report envelope")
    if report["status"] not in {"complete", "incomplete"} or report["mode"] != "plan" or report["family"] not in FAMILIES:
        raise AuditError("Invalid access audit report identity")
    if not isinstance(report["implementation_sha256"], str) or len(report["implementation_sha256"]) != 64:
        raise AuditError("Invalid access audit implementation identity")
    if not isinstance(report["entries"], list) or report["summary"].get("candidates") != len(report["entries"]):
        raise AuditError("Invalid access audit entry count")
    for entry in report["entries"]:
        fields = {"id", "path", "source_sha256", "span", "original_sha256", "symbol", "category",
                  "access", "supported", "replacement", "unsupported_reason", "parent_symbol",
                  "function_symbol", "function_address"}
        if not isinstance(entry, dict) or set(entry) != fields:
            raise AuditError("Invalid access audit entry")
        if len(entry["source_sha256"]) != 64 or len(entry["original_sha256"]) != 64 or type(entry["supported"]) is not bool:
            raise AuditError("Invalid access audit identity")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plan", action="store_true", required=True)
    parser.add_argument("--family", choices=sorted(FAMILIES), required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    root, _ = load_project()
    report = validate_report(audit_project(root, args.family))
    output = artifact_path(args.output)
    if output.exists():
        existing = json.loads(output.read_text(encoding="utf-8-sig"))
        if existing != report:
            raise AuditError("Access audit output already contains different immutable evidence")
    else:
        atomic_json(output, report)
    print(json.dumps({"status": report["status"], "family": report["family"],
                      "summary": report["summary"], "output": str(output)}, separators=(",", ":")))


if __name__ == "__main__":
    main()

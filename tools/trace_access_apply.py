"""Apply one source-bound access batch without global text substitution"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import tempfile

from trace_access_audit import FAMILIES, _calls, validate_report
from xport_project import atomic_json
from xport_project import artifact_path, load_project


SCHEMA = 1
READERS = {
    "byte_": "r_u8",
    "word_": "r_u16",
    "dword_": "r_u32",
}
WRITERS = {
    "byte_": "w_u8",
    "word_": "w_u16",
    "dword_": "w_u32",
}
UPDATERS = {
    "byte_": "xport_update_u8",
    "word_": "xport_update_u16",
    "dword_": "xport_update_u32",
}
UPDATE_OPERATIONS = {
    "+=": "XPORT_MEMORY_UPDATE_ADD",
    "-=": "XPORT_MEMORY_UPDATE_SUBTRACT",
    "*=": "XPORT_MEMORY_UPDATE_MULTIPLY",
    "/=": "XPORT_MEMORY_UPDATE_DIVIDE",
    "%=": "XPORT_MEMORY_UPDATE_REMAINDER",
    "&=": "XPORT_MEMORY_UPDATE_AND",
    "|=": "XPORT_MEMORY_UPDATE_OR",
    "^=": "XPORT_MEMORY_UPDATE_XOR",
    "<<=": "XPORT_MEMORY_UPDATE_SHIFT_LEFT",
    ">>=": "XPORT_MEMORY_UPDATE_SHIFT_RIGHT",
    "++": "XPORT_MEMORY_UPDATE_ADD",
    "--": "XPORT_MEMORY_UPDATE_SUBTRACT",
}
POINTER_TYPES = {
    "byte_": ("uint8", "sizeof(uint8)"),
    "word_": ("uint16", "sizeof(uint16)"),
    "dword_": ("uint32", "sizeof(uint32)"),
}


class ApplyError(ValueError):
    """Raised when a requested edit is not exactly source-bound"""


def _digest_bytes(data):
    return hashlib.sha256(data).hexdigest()


def _entry_id(path, start, original):
    return hashlib.sha256(f"{path}\0{start}\0{original}".encode("utf-8")).hexdigest()


def _render(text, start, end, replacements):
    result = text[start:end]
    for item_start, item_end, value in sorted(replacements, reverse=True):
        if start <= item_start and item_end <= end:
            result = result[:item_start - start] + value + result[item_end - start:]
    return result


def _outermost(replacements):
    return [item for item in replacements
            if not any(other[0] <= item[0] and item[1] <= other[1] and other != item
                       for other in replacements)]


def _assignment_end(text, start):
    index = start
    depth = 0
    quote = None
    while index < len(text):
        character = text[index]
        if quote:
            if character == "\\":
                index += 2
                continue
            if character == quote:
                quote = None
        elif character in "\"'":
            quote = character
        elif text.startswith("//", index):
            newline = text.find("\n", index + 2)
            index = len(text) if newline < 0 else newline
            continue
        elif text.startswith("/*", index):
            closing = text.find("*/", index + 2)
            if closing < 0:
                raise ApplyError("Unterminated comment in assignment")
            index = closing + 2
            continue
        elif character in "([{" :
            depth += 1
        elif character in ")]}":
            depth -= 1
        elif character == ";" and depth == 0:
            return index
        index += 1
    raise ApplyError("Simple assignment has no terminating semicolon")


def _operator_start(text, start):
    index = start
    while index > 0 and text[index - 1].isspace():
        index -= 1
    return index


def _guest_pointer(argument):
    value = argument.strip()
    nodes = _calls(value, {"psx_addr", "xport_guest_ptr", "xport_guest_cptr"})
    if len(nodes) != 1 or nodes[0]["start"] != 0 or nodes[0]["end"] != len(value):
        return None
    node = nodes[0]
    if node["name"] == "psx_addr" and len(node["arguments"]) == 2:
        return node["arguments"][0]
    if node["name"] in {"xport_guest_ptr", "xport_guest_cptr"} and len(node["arguments"]) == 3:
        return node["arguments"][1]
    return None


def _atomic_text(path, text):
    path = Path(path)
    descriptor, temporary = tempfile.mkstemp(prefix=path.name + ".", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="") as stream:
            stream.write(text)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def apply_batch(root, report, batch, apply=False):
    validate_report(report)
    if report["schema"] < 2 or batch.get("schema") != 1:
        raise ApplyError("Unsupported access report or batch schema")
    selected = batch.get("selected", [])
    if not 1 <= len(selected) <= 5:
        raise ApplyError("Batch must contain between one and five functions")
    selected_ids = {entry_id for function in selected for entry_id in function["entry_ids"]}
    report_entries = {entry["id"]: entry for entry in report["entries"]}
    if not selected_ids or not selected_ids <= set(report_entries):
        raise ApplyError("Batch references unknown access entries")

    root = Path(root).resolve()
    file_rows = []
    pending = []
    for relative in sorted({report_entries[entry_id]["path"] for entry_id in selected_ids}):
        path = root / relative
        data = path.read_bytes()
        expected_hashes = {report_entries[entry_id]["source_sha256"] for entry_id in selected_ids
                           if report_entries[entry_id]["path"] == relative}
        if expected_hashes != {_digest_bytes(data)}:
            raise ApplyError(f"Source identity changed: {relative}")
        text = data.decode("utf-8-sig")
        nodes = _calls(text, FAMILIES[report["family"]])
        node_by_id = {_entry_id(relative, node["start"], text[node["start"]:node["end"]]): node for node in nodes}
        wanted = [node_by_id.get(entry_id) for entry_id in selected_ids if report_entries[entry_id]["path"] == relative]
        if any(node is None for node in wanted):
            raise ApplyError(f"Source span no longer resolves: {relative}")

        read_nodes = []
        write_nodes = []
        update_nodes = []
        pointer_nodes = []
        bulk_nodes = []
        for entry_id in selected_ids:
            entry = report_entries[entry_id]
            if entry["path"] != relative:
                continue
            node = node_by_id[entry_id]
            if entry["access"] == "read" and node["name"] in READERS:
                read_nodes.append(node)
            elif entry["access"] == "write" and node["name"] in WRITERS and node["after"] == "=":
                write_nodes.append(node)
            elif entry["access"] == "compound_lvalue" and node["name"] in UPDATERS:
                update_nodes.append(node)
            elif entry["access"] == "pointer_binding" and node["name"] in POINTER_TYPES and node["before"] == "&":
                pointer_nodes.append(node)
            elif entry["category"] == "bulk_access" and node["name"] in {"memcpy", "memmove", "memset"}:
                bulk_nodes.append(node)
            else:
                raise ApplyError(f"Entry requires a semantic handler: {entry_id}")

        read_replacements = []
        for node in sorted(read_nodes, key=lambda item: item["end"] - item["start"]):
            argument_start = text.find("(", node["start"], node["end"]) + 1
            argument_end = node["end"] - 1
            argument = _render(text, argument_start, argument_end,
                               _outermost(read_replacements)).strip()
            read_replacements.append((node["start"], node["end"],
                f"{READERS[node['name']]}({argument})"))

        replacements = list(_outermost(read_replacements))
        for node in write_nodes:
            equals = node["end"]
            while equals < len(text) and text[equals].isspace():
                equals += 1
            if equals >= len(text) or text[equals] != "=":
                raise ApplyError("Write is not a simple assignment")
            expression_start = equals + 1
            expression_end = _assignment_end(text, expression_start)
            expression = _render(text, expression_start, expression_end,
                                 _outermost(read_replacements)).strip()
            argument_start = text.find("(", node["start"], node["end"]) + 1
            argument_end = node["end"] - 1
            argument = _render(text, argument_start, argument_end,
                               _outermost(read_replacements)).strip()
            value = f"{WRITERS[node['name']]}({argument}, {expression})"
            replacements.append((node["start"], expression_end, value))

        for node in update_nodes:
            argument_start = text.find("(", node["start"], node["end"]) + 1
            argument_end = node["end"] - 1
            argument = _render(text, argument_start, argument_end,
                               _outermost(read_replacements)).strip()
            replacement_start = node["start"]
            operation = node["after"]
            if operation in {"++", "--"}:
                expression_end = node["end"]
                operand = "1u"
            elif operation in UPDATE_OPERATIONS:
                operator_start = node["end"]
                while operator_start < len(text) and text[operator_start].isspace():
                    operator_start += 1
                expression_start = operator_start + len(operation)
                expression_end = _assignment_end(text, expression_start)
                operand = _render(text, expression_start, expression_end,
                                  _outermost(read_replacements)).strip()
            elif node["before"] in {"++", "--"}:
                operation = node["before"]
                replacement_start = _operator_start(text, node["start"])
                replacement_start -= len(operation)
                expression_end = node["end"]
                operand = "1u"
            else:
                raise ApplyError("Unsupported compound or update expression")
            value = (f"{UPDATERS[node['name']]}({argument}, "
                     f"{UPDATE_OPERATIONS[operation]}, {operand})")
            replacements.append((replacement_start, expression_end, value))

        for node in pointer_nodes:
            argument_start = text.find("(", node["start"], node["end"]) + 1
            argument_end = node["end"] - 1
            argument = _render(text, argument_start, argument_end,
                               _outermost(read_replacements)).strip()
            pointer_type, length = POINTER_TYPES[node["name"]]
            replacement_start = _operator_start(text, node["start"]) - 1
            value = (f"({pointer_type} *)xport_guest_ptr("
                     f"{argument}, {length})")
            replacements.append((replacement_start, node["end"], value))

        for node in bulk_nodes:
            arguments = node["arguments"]
            if node["name"] in {"memcpy", "memmove"} and len(arguments) == 3:
                destination = _guest_pointer(arguments[0])
                source = _guest_pointer(arguments[1])
                if destination is None and source is None:
                    raise ApplyError("Bulk copy has no guest operand")
                destination_ref = (f"xport_guest_ref({destination})" if destination is not None
                                   else f"xport_host_ref({arguments[0]})")
                source_ref = (f"xport_guest_ref({source})" if source is not None
                              else f"xport_host_ref((void *)({arguments[1]}))")
                value = ("xport_guest_copy("
                         f"{destination_ref}, {source_ref}, {arguments[2]})")
            elif node["name"] == "memset" and len(arguments) == 3:
                destination = _guest_pointer(arguments[0])
                if destination is None:
                    raise ApplyError("Bulk fill has no guest destination")
                value = ("xport_guest_fill("
                         f"{destination}, (uint8)({arguments[1]}), {arguments[2]})")
            else:
                raise ApplyError("Unsupported bulk expression")
            replacements.append((node["start"], node["end"], value))

        for write in write_nodes + update_nodes:
            end = _assignment_end(text, write["end"])
            replacements = [item for item in replacements
                            if item[0] == write["start"] or not (write["start"] < item[0] < end)]
        for pointer in pointer_nodes:
            replacements = [item for item in replacements
                            if item[1] == pointer["end"] or not (pointer["start"] < item[0] < pointer["end"])]
        for bulk in bulk_nodes:
            replacements = [item for item in replacements
                            if (item[0] == bulk["start"] and item[1] == bulk["end"]) or
                            not (bulk["start"] < item[0] < bulk["end"])]
        updated = _render(text, 0, len(text), replacements)
        if '#include "xport_trace.h"' not in updated:
            first_include_end = updated.find("\n", updated.find("#include"))
            if first_include_end < 0:
                raise ApplyError(f"Cannot place memory API include: {relative}")
            updated = updated[:first_include_end + 1] + '#include "xport_trace.h"\n' + updated[first_include_end + 1:]
        updated_data = updated.encode("utf-8")
        if updated_data == data:
            raise ApplyError(f"Batch made no changes: {relative}")
        pending.append((path, updated))
        file_rows.append({"path": relative, "before_sha256": _digest_bytes(data),
                          "after_sha256": _digest_bytes(updated_data)})

    if apply:
        for path, updated in pending:
            _atomic_text(path, updated)
    return {"schema": SCHEMA, "status": "applied" if apply else "planned",
            "family": report["family"], "selected_functions": len(selected),
            "transformed_entries": len(selected_ids), "files": file_rows}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", required=True)
    parser.add_argument("--batch", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--apply", action="store_true")
    args = parser.parse_args()
    root, _ = load_project()
    report_path = artifact_path(args.report)
    batch_path = artifact_path(args.batch)
    report = json.loads(report_path.read_text(encoding="utf-8-sig"))
    batch = json.loads(batch_path.read_text(encoding="utf-8-sig"))
    if batch.get("source_report_sha256") != _digest_bytes(report_path.read_bytes()):
        raise ApplyError("Batch does not bind the supplied report")
    result = apply_batch(root, report, batch, args.apply)
    result["source_report_sha256"] = _digest_bytes(report_path.read_bytes())
    result["source_batch_sha256"] = _digest_bytes(batch_path.read_bytes())
    output = artifact_path(args.output)
    if output.exists():
        if json.loads(output.read_text(encoding="utf-8-sig")) != result:
            raise ApplyError("Apply output already contains different immutable evidence")
    else:
        atomic_json(output, result)
    print(json.dumps({"status": result["status"], "files": len(result["files"]),
                      "entries": result["transformed_entries"], "output": str(output)}, separators=(",", ":")))


if __name__ == "__main__":
    main()

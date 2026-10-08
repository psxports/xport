"""Plan and apply hash-bound direct guest-pointer migrations"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re

from source_context import compact
from trace_access_audit import _arguments, _functions, _tokens


SCHEMA = 1
TYPES = {
    "uint8_t": (8, False), "int8_t": (8, True),
    "uint16_t": (16, False), "int16_t": (16, True),
}
ASSIGNMENTS = {"=", "|=", "&=", "-=", "+="}
UPDATES = {
    "|=": "XPORT_MEMORY_UPDATE_OR", "&=": "XPORT_MEMORY_UPDATE_AND",
    "-=": "XPORT_MEMORY_UPDATE_SUBTRACT", "+=": "XPORT_MEMORY_UPDATE_ADD",
}


class PointerMigrationError(ValueError):
    """Raised when a direct pointer migration is ambiguous"""


def digest_bytes(value):
    return hashlib.sha256(value).hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def _parse(text):
    tokens = _tokens(text)
    stack = []
    pairs = {}
    brace_stack = []
    brace_pairs = {}
    for index, token in enumerate(tokens):
        if token[1] == "(":
            stack.append(index)
        elif token[1] == ")":
            if not stack:
                raise PointerMigrationError("Unbalanced closing parenthesis")
            pairs[stack.pop()] = index
        elif token[1] == "{":
            brace_stack.append(index)
        elif token[1] == "}":
            if not brace_stack:
                raise PointerMigrationError("Unbalanced closing brace")
            brace_pairs[brace_stack.pop()] = index
    if stack or brace_stack:
        raise PointerMigrationError("Unbalanced source")
    return tokens, pairs, brace_pairs


def _direct_nodes(text):
    tokens, pairs, _ = _parse(text)
    result = []
    for index, token in enumerate(tokens):
        if token[1] != "rrj_at" or index < 5 or index + 1 >= len(tokens):
            continue
        prefix = [item[1] for item in tokens[index - 5:index]]
        if prefix[:2] != ["*", "("] or prefix[2] not in TYPES or prefix[3:] != ["*", ")"]:
            continue
        closing = pairs.get(index + 1)
        if closing is None:
            raise PointerMigrationError("Unclosed rrj_at call")
        arguments = _arguments(text, tokens, index + 1, closing)
        if len(arguments) != 3:
            raise PointerMigrationError("rrj_at must have three arguments")
        result.append({
            "start": tokens[index - 5][2], "end": tokens[closing][3], "close_token": closing,
            "type": prefix[2], "arguments": arguments,
            "after": tokens[closing + 1][1] if closing + 1 < len(tokens) else None,
            "after_token": closing + 1,
        })
    return tokens, result


def _apply_edits(text, edits):
    end = len(text)
    output = []
    for start, stop, replacement in sorted(edits, reverse=True):
        if stop > end:
            raise PointerMigrationError("Overlapping pointer migrations")
        output.append(text[stop:end])
        output.append(replacement)
        end = start
    output.append(text[:end])
    return "".join(reversed(output))


def _read_pass(text):
    total = 0
    while True:
        _, nodes = _direct_nodes(text)
        reads = [node for node in nodes if node["after"] not in ASSIGNMENTS]
        if not reads:
            return text, total
        innermost = [node for node in reads if not any(
            other["start"] > node["start"] and other["end"] < node["end"] for other in reads)]
        edits = []
        for node in innermost:
            width, signed = TYPES[node["type"]]
            memory, address, size = node["arguments"]
            if int(re.sub(r"[uUlL]+$", "", size), 0) != width // 8:
                raise PointerMigrationError("Direct dereference width differs from rrj_at extent")
            suffix = f"s{width}" if signed else f"u{width}"
            replacement = f"r_{suffix}({address})"
            edits.append((node["start"], node["end"], replacement))
        text = _apply_edits(text, edits)
        total += len(edits)


def _statement_end(tokens, start):
    round_depth = square_depth = brace_depth = 0
    for index in range(start, len(tokens)):
        value = tokens[index][1]
        if value == "(": round_depth += 1
        elif value == ")": round_depth -= 1
        elif value == "[": square_depth += 1
        elif value == "]": square_depth -= 1
        elif value == "{": brace_depth += 1
        elif value == "}": brace_depth -= 1
        elif value == ";" and round_depth == square_depth == brace_depth == 0:
            return index
    raise PointerMigrationError("Direct write has no statement terminator")


def _write_pass(text):
    tokens, nodes = _direct_nodes(text)
    edits = []
    for node in nodes:
        operation = node["after"]
        if operation not in ASSIGNMENTS:
            continue
        width, _ = TYPES[node["type"]]
        memory, address, size = node["arguments"]
        if int(re.sub(r"[uUlL]+$", "", size), 0) != width // 8:
            raise PointerMigrationError("Direct write width differs from rrj_at extent")
        semicolon = _statement_end(tokens, node["after_token"] + 1)
        value_start = tokens[node["after_token"]][3]
        value = text[value_start:tokens[semicolon][2]].strip()
        if not value:
            raise PointerMigrationError("Direct write has an empty value")
        if operation == "=":
            replacement = f"w_u{width}({address}, (uint32_t)({value}));"
        else:
            replacement = (f"xport_update_u{width}({address}, {UPDATES[operation]}, "
                           f"(uint32_t)({value}));")
        edits.append((node["start"], tokens[semicolon][3], replacement))
    return _apply_edits(text, edits), len(edits)


def transform_text(text):
    updated, reads = _read_pass(text)
    updated, writes = _write_pass(updated)
    _, remaining = _direct_nodes(updated)
    if remaining:
        raise PointerMigrationError("Direct rrj_at dereferences remain after transformation")
    return updated, {"reads": reads, "writes": writes}


def _function_inventory(text, sites):
    tokens, pairs, brace_pairs = _parse(text)
    functions = _functions(text, tokens, pairs, brace_pairs)
    inventory = {}
    for site in sites:
        function = next((row for row in functions if row["start"] <= site["start"] < row["end"]), None)
        if function is None:
            key = (None, None, None)
        else:
            body = text[function["start"]:function["end"]]
            marker = re.search(r'\bFUNCTION_MARKER\s*\(\s*(0x[0-9A-Fa-f]+)[uUlL]*\s*,\s*"([^"]+)"', body)
            address = marker.group(1).lower() if marker else function["address"]
            image = marker.group(2) if marker else None
            key = (function["symbol"], address, image)
        inventory[key] = inventory.get(key, 0) + 1
    return [{"symbol": key[0], "address": key[1], "image": key[2], "sites": count}
            for key, count in sorted(inventory.items(), key=lambda row: tuple(value or "" for value in row[0]))]


def make_plan(project):
    project = Path(project).resolve()
    files = []
    functions = {}
    totals = {"reads": 0, "writes": 0}
    for path in sorted((project / "src").glob("*.c")):
        data = path.read_bytes()
        text = data.decode("utf-8-sig")
        _, sites = _direct_nodes(text)
        if not sites:
            continue
        updated, counts = transform_text(text)
        for key in totals: totals[key] += counts[key]
        inventory = _function_inventory(text, sites)
        for row in inventory:
            key = (row["symbol"], row["address"], row["image"])
            functions[key] = functions.get(key, 0) + row["sites"]
        files.append({"path": path.relative_to(project).as_posix(), "sha256": digest_bytes(data),
                      "output_sha256": digest_bytes(updated.encode("utf-8")), **counts})
    if not files:
        raise PointerMigrationError("No direct rrj_at dereferences were found")
    mapped = [{"symbol": key[0], "address": key[1], "image": key[2], "sites": count}
              for key, count in sorted(functions.items(), key=lambda row: tuple(value or "" for value in row[0]))]
    return {"schema": SCHEMA, "contract": "xport-direct-pointer-migration-v1",
            "project": str(project), "implementation_sha256": digest_bytes(Path(__file__).read_bytes()),
            "files": files, "functions": mapped, "summary": {**totals, "files": len(files),
            "mapped_functions": sum(row["address"] is not None and row["image"] is not None for row in mapped),
            "unmapped_functions": sum(row["address"] is None or row["image"] is None for row in mapped)}}


def apply_plan(plan_path, evidence_path, manifest_path):
    plan_path = Path(plan_path).resolve()
    plan = json.loads(plan_path.read_text(encoding="utf-8"))
    if plan.get("schema") != SCHEMA or plan.get("contract") != "xport-direct-pointer-migration-v1":
        raise PointerMigrationError("Unsupported pointer migration plan")
    if plan.get("implementation_sha256") != digest_bytes(Path(__file__).read_bytes()):
        raise PointerMigrationError("Pointer migration implementation changed after planning")
    project = Path(plan["project"]).resolve()
    os.environ["XPORT_PROJECT"] = str(project)
    prepared = []
    for row in plan["files"]:
        path = project / row["path"]
        data = path.read_bytes()
        if digest_bytes(data) != row["sha256"]:
            raise PointerMigrationError("Pointer migration source identity changed: " + row["path"])
        updated, counts = transform_text(data.decode("utf-8-sig"))
        encoded = updated.encode("utf-8")
        if counts != {"reads": row["reads"], "writes": row["writes"]} or digest_bytes(encoded) != row["output_sha256"]:
            raise PointerMigrationError("Pointer migration output identity changed: " + row["path"])
        prepared.append((path, encoded))
    queries = []
    for row in plan["functions"]:
        if row["address"] is None or row["image"] is None:
            continue
        try:
            context = compact(row["image"], int(row["address"], 0))
            queries.append({**row, "status": "queried", "context_sha256": digest_bytes(canonical(context))})
        except ValueError as error:
            queries.append({**row, "status": "not_in_analysis_database", "error": str(error),
                            "context_sha256": None})
    evidence = {"schema": 1, "contract": "xport-image-qualified-query-evidence-v1",
                "plan_sha256": digest_bytes(plan_path.read_bytes()), "queries": queries,
                "mapped_functions": sum(row["status"] == "queried" for row in queries),
                "unmapped_markers": sum(row["status"] != "queried" for row in queries)}
    evidence_path = Path(evidence_path).resolve()
    evidence_path.parent.mkdir(parents=True, exist_ok=True)
    with evidence_path.open("x", encoding="utf-8") as stream:
        json.dump(evidence, stream, indent=2); stream.write("\n")
    for path, encoded in prepared:
        pending = path.with_suffix(path.suffix + ".pointer-migrate.pending")
        pending.write_bytes(encoded)
        pending.replace(path)
    manifest = {"schema": 1, "contract": "xport-direct-pointer-migration-v1", "status": "applied",
                "plan": str(plan_path), "plan_sha256": digest_bytes(plan_path.read_bytes()),
                "query_evidence": str(evidence_path), "query_evidence_sha256": digest_bytes(evidence_path.read_bytes()),
                "files": [{"path": str(path), "sha256": digest_bytes(encoded), "bytes": len(encoded)}
                          for path, encoded in prepared], "summary": plan["summary"]}
    manifest_path = Path(manifest_path).resolve()
    with manifest_path.open("x", encoding="utf-8") as stream:
        json.dump(manifest, stream, indent=2); stream.write("\n")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    plan = commands.add_parser("plan")
    plan.add_argument("--project", type=Path, required=True)
    plan.add_argument("--output", type=Path, required=True)
    apply = commands.add_parser("apply")
    apply.add_argument("--plan", type=Path, required=True)
    apply.add_argument("--query-evidence", type=Path, required=True)
    apply.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.command == "plan":
        result = make_plan(args.project)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("x", encoding="utf-8") as stream:
            json.dump(result, stream, indent=2); stream.write("\n")
    else:
        result = apply_plan(args.plan, args.query_evidence, args.output)
    print(json.dumps({"status": result.get("status", "planned"), "summary": result["summary"]}, indent=2))


if __name__ == "__main__":
    main()

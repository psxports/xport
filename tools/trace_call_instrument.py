"""Generate bounded diagnostic C copies with typed call entry/exit hooks"""

import argparse
import hashlib
import json
import re
from pathlib import Path

from trace_call_match import CallTraceError, digest_value, validate_registry


class InstrumentError(ValueError):
    """Raised when native call instrumentation cannot preserve semantics"""


def _matching_brace(text, opening):
    depth = 0
    state = "code"
    index = opening
    while index < len(text):
        pair = text[index:index + 2]
        character = text[index]
        if state == "code":
            if pair == "//":
                state = "line"
                index += 2
                continue
            if pair == "/*":
                state = "block"
                index += 2
                continue
            if character in "\"'":
                state = character
            elif character == "{":
                depth += 1
            elif character == "}":
                depth -= 1
                if depth == 0:
                    return index
        elif state == "line":
            if character == "\n":
                state = "code"
        elif state == "block":
            if pair == "*/":
                state = "code"
                index += 2
                continue
        elif character == "\\":
            index += 2
            continue
        elif character == state:
            state = "code"
        index += 1
    raise InstrumentError("Function body has no matching closing brace")


def _function_span(text, symbol):
    pattern = re.compile(r"\b" + re.escape(symbol) + r"\s*\([^;{}]*\)\s*\{")
    matches = list(pattern.finditer(text))
    if len(matches) != 1:
        raise InstrumentError(f"Expected one definition of {symbol}, found {len(matches)}")
    opening = text.find("{", matches[0].start(), matches[0].end())
    return matches[0].start(), opening, _matching_brace(text, opening)


def _hook_value(argument):
    name = argument["name"]
    kind = argument["type"]
    if isinstance(kind, str):
        return f"XPORT_CALL_SCALAR({name})"
    if kind["pointer"] == "guest":
        return f"XPORT_CALL_GUEST_POINTER({name}, {argument['pointee_bytes']}u)"
    if kind["pointer"] == "host_bounded":
        return f"XPORT_CALL_HOST_POINTER({name}, {argument['pointee_bytes']}u)"
    return f"XPORT_CALL_OPAQUE_POINTER({name})"


def instrument_function(text, function):
    _, opening, closing = _function_span(text, function["native_symbol"])
    body = text[opening + 1:closing]
    if re.search(r"\b(?:setjmp|longjmp|throw|__try|__except)\b", body):
        raise InstrumentError("longjmp/unwind function is unsupported")
    if re.search(r"^\s*#", body, re.MULTILINE):
        raise InstrumentError("macro-expanded function body is unsupported")
    values = ", ".join(_hook_value(item) for item in function["arguments"])
    if values:
        enter = (f'\n    XPORT_CALL_TRACE_ENTER("{function["id"]}", '
                 f'{len(function["arguments"])}u, {values});')
    else:
        enter = f'\n    xport_call_trace_enter("{function["id"]}", 0u);'
    return_type = function["return"]["type"]
    replacements = []
    for ordinal, match in enumerate(re.finditer(r"\breturn\s*([^;]*);", body)):
        expression = match.group(1).strip()
        if return_type == "void":
            if expression:
                raise InstrumentError("Void function returns a value")
            replacement = f'{{ XPORT_CALL_TRACE_EXIT_VOID("{function["id"]}"); return; }}'
        else:
            if not expression:
                raise InstrumentError("Non-void function has an empty return")
            ctype = function["prototype"].split(function["native_symbol"], 1)[0].strip()
            temporary = f"xport_call_return_{ordinal}"
            replacement = (f'{{ {ctype} {temporary} = ({expression}); '
                           f'XPORT_CALL_TRACE_EXIT_SCALAR("{function["id"]}", {temporary}); return {temporary}; }}')
        replacements.append((match.start(), match.end(), replacement))
    if return_type != "void" and not replacements:
        raise InstrumentError("Non-void function has no explicit return")
    for start, end, replacement in reversed(replacements):
        body = body[:start] + replacement + body[end:]
    if return_type == "void":
        body += f'\n    XPORT_CALL_TRACE_EXIT_VOID("{function["id"]}");\n'
    return text[:opening + 1] + enter + body + text[closing:]


def instrument(source, registry, function_ids):
    registry = validate_registry(registry)
    selected = [item for item in registry["functions"] if item["id"] in function_ids]
    if len(selected) != len(set(function_ids)):
        raise InstrumentError("Requested function identity is missing from the registry")
    text = source
    results = []
    for function in selected:
        try:
            text = instrument_function(text, function)
            results.append({"function": function["id"], "status": "instrumented"})
        except InstrumentError as error:
            results.append({"function": function["id"], "status": "unsupported", "reason": str(error)})
    if any(item["status"] == "instrumented" for item in results) and '#include "xport_trace.h"' not in text:
        text = '#include "xport_trace.h"\n' + text
    return text, results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--registry", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--function", action="append", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    registry = json.loads(args.registry.read_text(encoding="utf-8"))
    source = args.source.read_text(encoding="utf-8-sig")
    output, results = instrument(source, registry, args.function)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(output, encoding="utf-8")
    report = {"schema": 1, "contract": "xport-call-instrument-v1", "source": str(args.source.resolve()),
              "source_sha256": hashlib.sha256(args.source.read_bytes()).hexdigest(), "registry_sha256": digest_value(validate_registry(registry)),
              "output": str(args.output.resolve()), "results": results,
              "status": "complete" if all(item["status"] == "instrumented" for item in results) else "gaps"}
    with args.report.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
        stream.write("\n")
    print(json.dumps(report, separators=(",", ":")))
    return 0 if report["status"] == "complete" else 2


if __name__ == "__main__":
    raise SystemExit(main())

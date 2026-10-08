"""Build a complete but bounded single-turn lockstep repair context"""

from __future__ import annotations

import json
import re
from pathlib import Path


PROMPT_PREFIX = (
    'Audit every required function against its complete MIPS. Treat IDA pseudocode as '
    'optional supporting evidence: use it as a baseline only when marked complete, and '
    'continue from MIPS when it is absent or partial. '
    'For writer_library_inline, audit every original library operation. If the current '
    'C omits an effect, propose code that implements it in the owning function or a '
    'reachable shared helper, then map that original operation to the proposed C line. '
    'Abstain only when the original evidence cannot establish the required behavior. '
    'If writer_library is present, audit its relevant writer effects against its listing '
    'and any available pseudocode before replacing them in C. '
    'A library_reference is C reference evidence, not proof of this game binary; '
    'verify its behavior against the supplied original MIPS. '
    'Inspect an included C helper only to understand its effects; if its original MIPS '
    'or an omitted helper is needed to prove the repair, abstain and name it. '
    'For each callsite, audit the target, delay slot, argument values, return use and '
    'any register-carried or memory side effect needed by this repair. Do not require '
    'every callee listing solely because the owner calls it; identify the exact '
    'unproved effect that makes a particular callee necessary. Cover all branches, '
    'delay slots, call arguments, register effects, pointers and stores. '
    'Do not guess unresolved behavior. Do not add frame, time, address, input, '
    'value or observed-case compensation; do not mask a difference, suppress an '
    'event or special-case the reproducer. A memory exception is allowed only for '
    'a bounded firmware-runtime or trusted-device-state range whose non-game ownership '
    'is proven by the supplied evidence. It must not overlap the active divergence, '
    'game output, ordering tables or an existing exception. Edit only a required audited function '
    'and only when complete original evidence proves every affected path. '
    'Do not call tools or edit files or databases. Edits may target project-owned src C/H or shared '
    '@xport/src C/H; use the @xport prefix for shared paths. Return only '
    'JSON: schema=1, edits=[{path,before,after}] with exact '
    'unique C/H anchors, and function_audits with complete machine_coverage mapped to '
    'real C lines. Return memory_exceptions=[] unless such a range is proven; otherwise '
    'use [{region,offset,size,kind,name,reason}] with integer offsets and sizes. '
    'Use zero-based inclusive check_range=[first,last] for each coverage '
    'group, at most eight adjacent checks per range, covering every check exactly once. '
    'If proof is incomplete, return edits=[] and a reason. The supervisor '
    'applies code and writes receipts. Evidence JSON: '
)


_MIPS_LINE = re.compile(
    r'^(\s*[0-9A-Fa-f]{8})\s{2,}((?:[0-9A-Fa-f]{2}\s+){3}[0-9A-Fa-f]{2})\s+(.*)$'
)


def _compact_mips(lines: list[str]) -> str:
    def compact(line: str) -> str:
        match = _MIPS_LINE.fullmatch(line)
        if match is None:
            return line
        return match[1] + ' ' + ''.join(match[2].split()) + ' ' + match[3]

    return '\n'.join(compact(line) for line in lines)


def _local_helpers(root: Path, source_roots: tuple[Path, ...], packet: dict,
                   functions: list[dict]) -> list[dict]:
    from lockstep_source_coverage import index_functions
    from lockstep_supervisor import source_snapshot

    identity = (packet.get('identities') or {}).get('native_source_identity')
    snapshot = source_snapshot(root, source_roots)
    if snapshot['identity'] != identity:
        raise ValueError('Repair source no longer matches the attention packet')
    helpers = []
    seen = {(row['source']['path'], row['source']['symbol']) for row in functions
            if row.get('audit_role') != 'writer_library_inline'}
    for function in functions:
        if function.get('audit_role') == 'writer_library_inline':
            continue
        path = function['source']['path']
        symbol = function['source']['symbol']
        if not isinstance(path, str) or path not in snapshot['files']:
            raise ValueError('Repair implementation is outside owned source')
        index = index_functions(root, {path: snapshot['files'][path]})
        frontier = [(path, symbol)]
        for _depth in range(2):
            following = []
            for parent in frontier:
                if parent not in index:
                    raise ValueError('Repair implementation is absent from current C')
                for called in sorted(index[parent]['calls']):
                    key = (path, called)
                    if key not in index or key in seen:
                        continue
                    seen.add(key)
                    row = index[key]
                    lines = (root / path).read_text(encoding='utf-8-sig').splitlines()
                    helpers.append({'path': path, 'symbol': called,
                                    'start_line': row['start_line'],
                                    'text': '\n'.join(lines[row['start_line'] - 1:row['end_line']])})
                    following.append(key)
                    if len(helpers) > 16:
                        raise ValueError('Repair context has too many local C helpers')
            frontier = following
    return helpers


def _event(event: dict | None, *, original: bool) -> dict | None:
    if not isinstance(event, dict):
        return None
    keys = ('kind', 'frame', 'function', 'callsite', 'instruction_word',
            'delay_slot_pc', 'delay_slot_word', 'return_value') if original else (
            'kind', 'frame', 'function', 'return_value')
    result = {key: event[key] for key in keys if event.get(key) is not None}
    if original and event.get('kind') == 'PRE_CALL' and event.get('arguments') is not None:
        result['arguments'] = event['arguments']
    if not original and isinstance(event.get('source'), dict):
        source = event['source']
        result['source'] = {key: source[key] for key in ('symbol', 'file', 'line')
                            if source.get(key) is not None}
    return result


def _refinement(value: dict | None) -> dict | None:
    if not isinstance(value, dict):
        return None
    result = {'interval_complete': value.get('interval_complete')}
    for side in ('original_writes', 'native_writes'):
        writes = value.get(side) or []
        fields = ('pc', 'instruction_word', 'stop_pc', 'ra', 'sp', 'region',
                  'offset', 'width', 'before', 'after',
                  'caller_symbol', 'caller2_symbol', 'caller3_symbol')
        result[side] = [{key: row[key] for key in fields if row.get(key) is not None}
                        for row in writes if isinstance(row, dict)]
    for key in ('branch_slice', 'argument_location', 'target_resolution_complete'):
        if value.get(key):
            result[key] = value[key]
    ring = ((value.get('pointer_evidence') or {}).get('native_writer_ring') or {})
    if ring.get('available'):
        result['native_writer_ring'] = {
            'dropped': ring.get('dropped'), 'truncated': ring.get('truncated'),
            'entries': [{key: row[key] for key in
                         ('offset', 'width', 'before', 'after', 'byte_mask', 'source')
                         if row.get(key) is not None}
                        for row in ring.get('entries') or []],
        }
    return result


def _relevant_local_helpers(packet: dict, helpers: list[dict]) -> list[dict]:
    context = packet.get('source_context') or {}
    wanted = set(context.get('related_helper_symbols') or [])
    if not wanted:
        return helpers
    return [row for row in helpers if row['symbol'] in wanted]


def build_context(packet: dict, *, root: Path | None = None,
                  source_roots: tuple[Path, ...] = (Path('src'),)) -> str:
    context = packet.get("source_context") or {}
    cluster = [context, *(context.get("audit_cluster") or [])]
    library = context.get("writer_library") or {}
    if library.get("audit_source"):
        cluster.append(library)
    required = (context.get("audit_contract") or {}).get("required_functions") or []
    by_key = {
        ((row.get("function") or {}).get("image"),
         (row.get("function") or {}).get("address")): row
        for row in cluster
    }
    functions = []
    for binding in required:
        key = (binding.get("image"), binding.get("address"))
        row = by_key.get(key)
        if row is None:
            raise ValueError("Repair packet lacks a required function")
        library_inline = row is library
        inline_callee = row.get("audit_role") == "inlined_callee"
        implementation = ((context.get("implementation") if
                           (library_inline or inline_callee) else
                           row.get("implementation")) or {})
        if (library_inline or inline_callee) and any((row.get("audit_source") or {}).get(name) !=
                                  (context.get("implementation") or {}).get(name)
                                  for name in ("path", "symbol")):
            raise ValueError("Inlined audit source differs from its owning C function")
        mips = row.get("mips") or {}
        pseudocode = row.get("pseudocode") or {}
        if implementation.get("complete") is not True or mips.get("complete") is not True:
            raise ValueError("Repair packet lacks complete C or MIPS")
        checks = (row.get("machine_audit") or {}).get("checks") or []
        function = {
            "binding": {key: binding[key] for key in ('image', 'address')},
            "source": {key: implementation.get(key) for key in
                       ("path", "symbol", "start_line", "text")},
            "mips": _compact_mips(mips["lines"]),
            "pseudocode": "\n".join(pseudocode.get("lines") or []),
            "pseudocode_complete": pseudocode.get("complete") is True,
            "machine_checks": " ".join(item["id"] for item in checks),
            "machine_check_group_starts": list(range(0, len(checks), 8)),
        }
        if library_inline:
            function["audit_role"] = "writer_library_inline"
            function["classification"] = library.get("library_classification")
        if inline_callee:
            function["audit_role"] = "inlined_callee"
            mapped = row.get("implementation") or {}
            function["mapped_source"] = {key: mapped.get(key) for key in
                                         ("path", "symbol", "start_line", "text")}
        functions.append(function)
    evidence = {
        "schema": 1,
        "classification": packet.get('classification'),
        "diagnostic_owner": packet.get('diagnostic_owner'),
        "comparison": {key: value for key, value in (packet.get('comparison') or {}).items()
                       if key in {'region', 'offset', 'length', 'original', 'candidate'}},
        "expected_original_event": _event(packet.get('expected_original_event'), original=True),
        "actual_native_event": _event(packet.get('actual_native_event'), original=False),
        "refinement": _refinement(packet.get('refinement')),
        "functions": functions,
    }
    if isinstance(packet.get('arguments'), dict):
        evidence['entry_arguments'] = packet['arguments']
    if context.get('unreviewed_original_callees'):
        evidence['unreviewed_original_callees'] = context['unreviewed_original_callees']
    if (packet.get('diagnostic_owner') or {}).get('kind') == 'writer_library_caller':
        library = context.get('writer_library') or {}
        mips = library.get('mips') or {}
        pseudocode = library.get('pseudocode') or {}
        if mips.get('complete') is not True:
            raise ValueError('Original writer library lacks complete MIPS')
        if not library.get('audit_source'):
            evidence['writer_library'] = {
                'image': library.get('image'), 'address': library.get('address'),
                'mips': _compact_mips(mips['lines']),
                'pseudocode': '\n'.join(pseudocode.get('lines') or []),
                'pseudocode_complete': pseudocode.get('complete') is True,
            }
            if library.get('library_classification'):
                evidence['writer_library']['classification'] = library['library_classification']
    if packet.get("gte_causal_evidence"):
        evidence["gte_causal_evidence"] = packet["gte_causal_evidence"]
    if packet.get("gte_entry_provenance"):
        evidence["gte_entry_provenance"] = packet["gte_entry_provenance"]
    if packet.get("trig_causal_evidence"):
        evidence["trig_causal_evidence"] = packet["trig_causal_evidence"]
    if packet.get('library_reference'):
        evidence['library_reference'] = packet['library_reference']
    if packet.get('operand_provenance'):
        evidence['operand_provenance'] = packet['operand_provenance']
    if root is not None:
        helpers = _local_helpers(Path(root), source_roots, packet, functions)
        candidates = _relevant_local_helpers(packet, helpers)
        image = (context.get('function') or {}).get('image')
        required_keys = {(row.get('image'), row.get('address')) for row in required}
        missing = []
        for helper in candidates:
            address = re.search(r'([0-9A-Fa-f]{8})$', helper['symbol'])
            if address and (image, int(address[1], 16)) not in required_keys:
                missing.append(helper['symbol'])
        if missing:
            raise ValueError('Relevant original C helpers lack required MIPS audit: ' +
                             ', '.join(missing))
        evidence['local_c_helpers'] = []
        evidence['omitted_local_c_helpers'] = [row['symbol'] for row in helpers]
        for helper in candidates:
            selected = [*evidence['local_c_helpers'], helper]
            omitted = [row['symbol'] for row in helpers
                       if row['symbol'] not in {item['symbol'] for item in selected}]
            candidate = {**evidence, 'local_c_helpers': selected,
                         'omitted_local_c_helpers': omitted}
            if len(PROMPT_PREFIX.encode('utf-8')) + len(json.dumps(
                    candidate, ensure_ascii=False, separators=(',', ':')).encode('utf-8')) <= 65536:
                evidence = candidate
        if not evidence['omitted_local_c_helpers']:
            evidence.pop('omitted_local_c_helpers')
    encoded = json.dumps(evidence, ensure_ascii=False, separators=(",", ":"))
    if len(PROMPT_PREFIX.encode('utf-8')) + len(encoded.encode("utf-8")) > 65536:
        raise ValueError("Repair evidence exceeds the single-turn context budget")
    return encoded


def build_prompt(packet: dict, *, root: Path | None = None,
                 source_roots: tuple[Path, ...] = (Path('src'),)) -> str:
    return PROMPT_PREFIX + build_context(packet, root=root,
                                         source_roots=source_roots)

"""Attach a bounded, hash-bound C reference to a waiting library audit"""

from __future__ import annotations

import copy
import hashlib
import json
import re
from pathlib import Path

from lockstep_repair_context import build_prompt
from source_index import functions


_SYMBOL = re.compile(r'[A-Za-z_]\w*\Z')
_HASH = re.compile(r'[0-9a-fA-F]{64}\Z')


def _symbol_entry(name: str, lines: list[str], entries: dict) -> dict:
    row = entries.get(name)
    if row is not None:
        start, end = row['start_line'], row['end_line']
    else:
        matches = [index for index, line in enumerate(lines)
                   if re.match(r'^\s*#\s*define\s+' + re.escape(name) + r'\b', line)]
        if len(matches) != 1:
            raise ValueError('Library reference symbol is absent or ambiguous: ' + name)
        start = end = matches[0] + 1
        while lines[end - 1].rstrip().endswith('\\'):
            end += 1
            if end > len(lines):
                raise ValueError('Unterminated library reference macro')
    return {'symbol': name, 'start_line': start, 'end_line': end,
            'text': '\n'.join(lines[start - 1:end])}


def read_reference_request(root: Path, request: dict, previous: dict,
                           source_roots: tuple[Path, ...]) -> dict:
    if (set(request) - {'kind', 'alias', 'sources', 'expand_callees'} or
            not {'kind', 'alias', 'sources'} <= set(request) or
            request['kind'] != 'library_reference' or
            type(request.get('expand_callees', False)) is not bool):
        raise ValueError('Invalid library reference request')
    library = ((previous.get('source_context') or {}).get('writer_library') or {})
    classification = library.get('library_classification') or {}
    alias = classification.get('alias')
    if not isinstance(alias, str) or request['alias'] != alias:
        raise ValueError('Library reference alias does not match the classified writer')
    sources = request['sources']
    if not isinstance(sources, list) or not 1 <= len(sources) <= 4:
        raise ValueError('Library reference needs one to four source files')
    selected = []
    names = set()
    catalogs = {}
    inventories = {}
    for source in sources:
        if (not isinstance(source, dict) or
                not {'path', 'sha256', 'symbols'} <= set(source) or
                set(source) - {'path', 'sha256', 'symbols', 'line_ranges'}):
            raise ValueError('Invalid library reference source')
        path_value, expected, symbols = source['path'], source['sha256'], source['symbols']
        ranges = source.get('line_ranges', [])
        if (not isinstance(path_value, str) or not Path(path_value).is_absolute() or
                not isinstance(expected, str) or not _HASH.fullmatch(expected) or
                not isinstance(symbols, list) or len(symbols) > 8 or
                any(not isinstance(name, str) or not _SYMBOL.fullmatch(name)
                    for name in symbols) or
                not isinstance(ranges, list) or len(ranges) > 3 or
                not symbols and not ranges):
            raise ValueError('Invalid library reference identity or symbols')
        path = Path(path_value).resolve(strict=True)
        if not path.is_file() or path.stat().st_size > 1048576:
            raise ValueError('Library reference source is missing or oversized')
        raw = path.read_bytes()
        actual = hashlib.sha256(raw).hexdigest()
        if actual != expected.lower():
            raise ValueError('Library reference source hash changed')
        text = raw.decode('utf-8-sig')
        entries = {row['name']: row for row in functions(text, path.name)}
        lines = text.splitlines()
        catalogs[str(path)] = set(entries) | set(re.findall(
            r'^\s*#\s*define\s+([A-Za-z_]\w*)', text, re.MULTILINE))
        inventories[str(path)] = (lines, entries)
        extracted = []
        for name in symbols:
            if (str(path), name) in names:
                raise ValueError('Duplicate library reference symbol')
            names.add((str(path), name))
            extracted.append(_symbol_entry(name, lines, entries))
        for span in ranges:
            if (not isinstance(span, list) or len(span) != 2 or
                    any(type(value) is not int for value in span) or
                    not 1 <= span[0] <= span[1] <= len(lines) or
                    span[1] - span[0] >= 20):
                raise ValueError('Invalid library reference line range')
            start, end = span
            extracted.append({'symbol': f'lines:{start}-{end}',
                              'start_line': start, 'end_line': end,
                              'text': '\n'.join(lines[start - 1:end])})
        selected.append({'path': str(path), 'sha256': actual, 'entries': extracted})
    known = set().union(*catalogs.values())
    pending = [(source, entry) for source in selected for entry in source['entries']]
    while pending:
        source, entry = pending.pop(0)
        local = catalogs[source['path']]
        if entry['symbol'].startswith('lines:'):
            continue
        body = re.sub(r'/\*.*?\*/|//[^\n]*', '', entry['text'], flags=re.DOTALL)
        for callee in sorted(set(re.findall(r'\b([A-Za-z_]\w*)\s*\(', body)) & known):
            if callee in local:
                target = source
            elif any(name == callee for _, name in names):
                continue
            else:
                target = next(item for item in selected
                              if callee in catalogs[item['path']])
            key = (target['path'], callee)
            if key in names:
                continue
            if not request.get('expand_callees'):
                raise ValueError('Library reference omits available callee: ' + callee)
            if len([name for path, name in names if path == target['path']]) >= 8:
                raise ValueError('Library reference callee expansion exceeds eight symbols')
            names.add(key)
            lines, entries = inventories[target['path']]
            expanded = _symbol_entry(callee, lines, entries)
            target['entries'].append(expanded)
            pending.append((target, expanded))
    reference = {'alias': alias, 'sources': selected}
    if len(json.dumps(reference, ensure_ascii=False).encode('utf-8')) > 7168:
        raise ValueError('Library reference exceeds its 7-KiB evidence budget')
    packet = copy.deepcopy(previous)
    packet['library_reference'] = reference
    base_signature = (previous.get('library_reference_base_signature') or
                      previous.get('failure_signature'))
    if isinstance(base_signature, str) and _HASH.fullmatch(base_signature):
        packet['library_reference_base_signature'] = base_signature
        packet['failure_signature'] = hashlib.sha256(json.dumps(
            {'base_signature': base_signature, 'library_reference': reference},
            sort_keys=True, separators=(',', ':')).encode('utf-8')).hexdigest()
    packet['serialized_bytes'] = len(json.dumps(
        packet, sort_keys=True, separators=(',', ':')).encode('utf-8'))
    if packet['serialized_bytes'] > packet['budget_bytes']:
        raise ValueError('Library reference packet exceeds its byte budget')
    build_prompt(packet, root=root, source_roots=source_roots)
    return packet

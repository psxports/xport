"""Prepare explicit user-authorized SDK abort stubs without claiming SDK proof"""

import hashlib
from pathlib import Path
import re

from lockstep_supervisor import source_snapshot
from source_index import functions


def prepare_stub_plan(packet, request, shared_root):
    if (set(request) != {'schema', 'authorization', 'stubs'} or request['schema'] != 1 or
            not isinstance(request['authorization'], dict) or
            request['authorization'].get('explicit_user_authorization') is not True or
            not isinstance(request['authorization'].get('statement'), str) or
            not request['authorization']['statement'].strip()):
        raise ValueError('SDK stubs require recorded explicit user authorization')
    rows = request['stubs']
    if not isinstance(rows, list) or not rows:
        raise ValueError('SDK stub request is empty')
    shared_root = Path(shared_root).resolve()
    snapshot = source_snapshot(shared_root, (Path('src'),))
    if snapshot['identity'] != packet['identities']['shared_source_identity']:
        raise ValueError('SDK stub plan has stale shared source identity')
    dependencies = {(d['binding']['image'], d['binding']['address']): d['binding']
                    for d in packet.get('sdk_dependencies', [])}
    existing = set()
    for path in (shared_root / 'src').rglob('*.c'):
        existing.update(f['name'] for f in functions(path.read_text(encoding='utf-8-sig'), str(path)))
    seen = set()
    planned = []
    for row in rows:
        if set(row) != {'image', 'address', 'symbol', 'prototype'} or type(row['address']) is not int:
            raise ValueError('Invalid SDK stub row')
        key = (row['image'], row['address'])
        if key not in dependencies or key in seen:
            raise ValueError('SDK stub lacks a unique packet dependency')
        aliases = {r['alias'] for r in dependencies[key]['library_classification']}
        symbol = row['symbol']
        if symbol not in aliases or symbol in existing:
            raise ValueError('SDK stub must name a missing reviewed canonical API')
        prototype = row['prototype']
        if not isinstance(prototype, str) or not re.fullmatch(
                r'(?:void|uint32|sint32)\s+' + re.escape(symbol) + r'\([^{};#\r\n]*\);', prototype):
            raise ValueError('Invalid SDK stub prototype')
        text = prototype[:-1] + '\n{\n    /* User-authorized unimplemented SDK boundary */\n    abort();\n}\n'
        parsed = functions(text, '@xport/src/psx.c')
        if len(parsed) != 1 or parsed[0]['name'] != symbol:
            raise ValueError('SDK stub does not contain exactly one function')
        planned.append({**row, 'path': '@xport/src/psx.c', 'text': text,
                        'text_sha256': hashlib.sha256(text.encode()).hexdigest(),
                        'implementation_status': 'unimplemented_abort_stub',
                        'semantic_equivalence_proved': False, 'coverage_proof_credit': 0})
        seen.add(key)
        existing.add(symbol)
    return {'schema': 1, 'shared_source_identity': snapshot['identity'],
            'authorization': request['authorization'], 'stubs': planned,
            'applied': False, 'scope': 'Reviewable plan; supervisor application and packet scope transition still required'}

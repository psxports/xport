"""Validate return-only declarations for complete mapped coverage owners"""

import re
from pathlib import Path

from source_index import functions, mask_c
from coverage_corrective_prototypes import FIELDS, lexical_tokens, phase2


def validate_return_updates(root, packet, proposal, shared_root=None):
    updates = proposal.get('game_return_prototype_updates') or []
    reviews = proposal.get('game_return_abi_audits') or []
    if not updates and not reviews:
        return []
    if packet.get('contract') != 'xport-function-audit-packet-v1' or proposal.get('translations'):
        raise ValueError('Return-only ABI repair requires complete mapped coverage owners')
    if not isinstance(updates, list) or not isinstance(reviews, list) or not reviews:
        raise ValueError('Return-only ABI repair needs declaration and complete ABI reviews')
    context = packet['source_context']
    contexts = [context, *context.get('audit_cluster', [])]
    required = context['audit_contract']['required_functions']
    expected = []
    seen = set()
    for review in reviews:
        if set(review) != {*FIELDS, 'old_return', 'new_return', 'complete_mips_audit',
                           'pseudocode_used_as_baseline', 'argument_abi_unchanged', 'return_review'}:
            raise ValueError('Invalid mapped return ABI review')
        key = (review['image'], review['address'])
        bound = [b for b in required if (b['image'], b['address']) == key]
        owners = [c for c in contexts if (c['function']['image'], c['function']['address']) == key]
        if key in seen or len(bound) != 1 or len(owners) != 1 or any(review[k] != bound[0][k] for k in FIELDS):
            raise ValueError('Mapped return ABI owner is duplicate, foreign or stale')
        seen.add(key)
        owner = owners[0]
        impl = owner['implementation']
        if (impl.get('complete') is not True or review['old_return'] != 'void' or
                review['new_return'] not in ('uint32', 'sint32') or
                review['complete_mips_audit'] is not True or
                review['pseudocode_used_as_baseline'] is not bound[0]['pseudocode_complete'] or
                review['argument_abi_unchanged'] is not True or
                not isinstance(review['return_review'], str) or not review['return_review'].strip()):
            raise ValueError('Mapped return ABI review is incomplete or changes arguments')
        symbol = impl['symbol']
        shared = shared_root
        if shared:
            for source in (Path(shared) / 'src').rglob('*'):
                if source.is_file() and source.suffix.lower() in ('.c', '.h'):
                    logical, _ = phase2(source.read_bytes().decode('utf-8-sig'))
                    if re.search(r'\b' + re.escape(symbol) + r'\b', lexical_tokens(logical)):
                        raise ValueError('Mapped game return ABI rejects shared references')
        signature = re.compile(r'(void|uint32|sint32)\s+' + re.escape(symbol) + r'\s*\(([^(){};#]*)\)\s*\{')
        before_head = signature.match(impl['text'])
        edits = [e for e in proposal.get('edits', []) if e.get('path') == impl['path'] and e.get('before') == impl['text']]
        if len(edits) != 1 or not before_head or before_head[1] != review['old_return']:
            raise ValueError('Mapped return ABI needs one complete owner replacement')
        after_head = signature.match(edits[0]['after'])
        parsed = functions(edits[0]['after'], impl['path'])
        if (not after_head or after_head[1] != review['new_return'] or
                before_head[2] != after_head[2] or len(parsed) != 1 or
                parsed[0]['name'] != symbol or parsed[0]['markers'] != [(key[1], key[0])]):
            raise ValueError('Mapped return ABI replacement changes owner arguments or marker')
        declaration = re.compile(r'(?:extern\s+)?(void|uint32|sint32)\s+' + re.escape(symbol) + r'\s*\([^(){};#]*\)\s*;')
        token = re.compile(r'\b' + re.escape(symbol) + r'\b')
        for source in sorted((Path(root) / 'src').rglob('*')):
            if source.suffix.lower() not in ('.c', '.h') or not source.is_file():
                continue
            text = source.read_bytes().decode('utf-8-sig')
            logical, positions = phase2(text)
            masked = lexical_tokens(logical)
            for occurrence in token.finditer(masked):
                if positions[occurrence.end() - 1] - positions[occurrence.start()] != len(symbol) - 1:
                    raise ValueError('Mapped return ABI rejects split identifiers')
            if any(line.lstrip().startswith('#') and token.search(line) for line in masked.splitlines()):
                raise ValueError('Mapped return ABI rejects directive references')
            bodies = functions(text, source.relative_to(root).as_posix())
            covered = []
            for match in declaration.finditer(mask_c(text)):
                if any(b['start'] <= match.start() < b['end'] for b in bodies):
                    raise ValueError('Mapped return ABI rejects local declarations')
                if match[1] != review['old_return']:
                    raise ValueError('Mapped return ABI has inconsistent existing declarations')
                old = text[match.start():match.end()]
                new = old[:match.start(1)-match.start()] + review['new_return'] + old[match.end(1)-match.start():]
                if text.count(old) != 1:
                    raise ValueError('Mapped return ABI declaration anchor is ambiguous')
                expected.append({'path': source.relative_to(root).as_posix(), 'before': old, 'after': new})
                covered.append(match.span())
            for body in bodies:
                body_text = mask_c(text)[body['start']:body['end']]
                for occurrence in token.finditer(body_text):
                    location = body['start'] + occurrence.start()
                    if body['name'] == symbol and location == body['start'] + body_text.index(symbol):
                        if source.relative_to(root).as_posix() != impl['path']:
                            raise ValueError('Mapped return ABI has a duplicate definition')
                    elif not re.match(r'\s*\(', body_text[occurrence.end():]):
                        raise ValueError('Mapped return ABI rejects function-pointer escapes')
                    covered.append((location, body['start'] + occurrence.end()))
            for occurrence in token.finditer(mask_c(text)):
                if not any(start <= occurrence.start() < end for start, end in covered):
                    raise ValueError('Mapped return ABI rejects unknown outside references')
    if not expected or sorted(updates, key=lambda e: (e.get('path', ''), e.get('before', ''))) != sorted(expected, key=lambda e: (e['path'], e['before'])):
        raise ValueError('Mapped return ABI must update all and only existing declarations')
    return expected

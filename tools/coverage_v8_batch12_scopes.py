"""Validate eight exact human-authorized V8 scopes without relaxing other checks"""

import hashlib
from pathlib import Path
import re

from coverage_game_frame_delegation import FIELDS, _review
from coverage_native_abi_frame import original_words
from source_index import functions
from coverage_corrective_prototypes import lexical_tokens, phase2

REPLACEMENT = 'user_authorized_v8_batch12_scope'
REVIEW_SHA = 'a6d6a81b9129f8cbadc243888b34e90e65c5f27c17e5c68ab391ca5444f884fc'
AUTH_SHA = '1417f72ce74faf3f2fbb17fd691e70c6c036e4cefbf9da6691b78885cf1a07d8'
OWNERS = {0x8002263C, 0x80021B80, 0x8002E630, 0x8003E598,
          0x8002E2BC, 0x80023940, 0x8002F998, 0x8002F9BC}
FRAME_RANGES = {0x8002263C: (24, 48, 56), 0x8002E630: (24, 60, 64),
                0x8003E598: (40, 44, 48), 0x8002E2BC: (80, 100, 104),
                0x8002F9BC: (240, 276, 280)}
CALLBACKS = {0x80021B80: (0x80021BB4, 0x80021BC8),
             0x8002E2BC: (0x8002E40C,), 0x8002F9BC: (0x80030368, 0x8003069C)}
TRANSFERS = {0x80023940: (0x80023970, 0x00400008),
             0x8002F998: (0x8002F9B4, 0x03E00008)}


def sha(text):
    return hashlib.sha256(text.encode('utf-8')).hexdigest()


def reviewed_body(owner):
    body = owner['after'].replace('\r\n', '\n')
    if owner['address'] == 0x8002263C:
        # Preserve initial A0 and read the remaining word only at the loop tail
        before = '    while (r_u32(frame + 56u) != 0u)'
        tail = '            sub_80015BF0(size, 0u);\n        }\n    }'
        if body.count(before) != 1 or body.count(tail) != 1:
            raise ValueError('Batch12 reviewed 2263C loop anchor changed')
        body = body.replace(before, '    while (bytes != 0u)').replace(
            tail, '            sub_80015BF0(size, 0u);\n        }\n        bytes = r_u32(frame + 56u);\n    }')
    if owner['address'] == 0x8002F9BC:
        anchor = '                value = v8_physics_add(v8_physics_mul(cosine, friction), v8_physics_mul(sine, lateral));'
        if body.count(anchor) != 1:
            raise ValueError('Batch12 reviewed F9BC sine reload anchor changed')
        body = body.replace(anchor, '                sine = r_s32(frame + 192u);\n' + anchor)
    return body


def scope_checks(context):
    address = context['function']['address']
    result = []
    for check in context['machine_audit']['checks']:
        word = int(check['word'], 0)
        op, rs, rt = word >> 26, (word >> 21) & 31, (word >> 16) & 31
        offset = word & 65535
        pc = int(check['pc'], 0)
        eligible = False
        if address in FRAME_RANGES:
            low, high, size = FRAME_RANGES[address]
            eligible = (op in (35, 43) and rs == 29 and rt in (*range(16, 24), 30, 31)
                        and low <= offset <= high and offset % 4 == 0)
            eligible |= op == 9 and rs == rt == 29 and offset == size
        if address == 0x8003E598:
            eligible |= op == 35 and rs == 29 and offset in (28, 36)
        if pc in CALLBACKS.get(address, ()):
            if word >> 26 != 0 or word & 63 != 9 or (word >> 11) & 31 != 31 or check['kind'] != 'call':
                raise ValueError('Batch12 approved callback is not its original JALR')
            eligible = True
        if address in TRANSFERS and pc == TRANSFERS[address][0]:
            if word != TRANSFERS[address][1] or check['kind'] != 'control':
                raise ValueError('Batch12 approved transfer encoding changed')
            eligible = True
        if eligible:
            result.append(check['id'])
    return result


def authorized_scope(root, packet, proposal, binding, audit):
    if audit.get('replacement_kind') != REPLACEMENT:
        return None
    if root is None or binding.get('image') != 'SLUS_005.10' or binding.get('address') not in OWNERS or binding.get('library_classification') or packet.get('contract') != 'xport-function-audit-packet-v1':
        raise ValueError('Batch12 scope exceeds exact recorded V8 owners')
    if any(audit.get(key) != binding.get(key) for key in FIELDS):
        raise ValueError('Batch12 scope original binding changed')
    reference = audit.get('game_scope_authorization')
    if not isinstance(reference, dict) or reference.get('sha256') != AUTH_SHA:
        raise ValueError('Batch12 scope lacks the exact human authorization')
    authorization = _review(root, reference)
    if authorization.get('schema') != 1 or authorization.get('contract') != 'v8-user-authorized-strict-batch12-scopes-v1' or authorization.get('user_instruction') != 'да' or authorization.get('approved_owner_count') != 8 or authorization.get('original_equivalence') is not False or authorization.get('implementation_status') != 'WIP' or authorization.get('expansion_authorized') is not False:
        raise ValueError('Batch12 human authorization contract changed')
    scope_reference = authorization.get('review')
    if not isinstance(scope_reference, dict) or scope_reference.get('sha256') != REVIEW_SHA or audit.get('game_scope_review') != scope_reference:
        raise ValueError('Batch12 scope differs from the human-reviewed artifact')
    review = _review(root, scope_reference)
    owners = review.get('owners', [])
    if review.get('contract') != 'v8-strict-batch12-proposed-scope-review-v1' or len(owners) != 8 or {row['address'] for row in owners} != OWNERS:
        raise ValueError('Batch12 approved owner inventory changed')
    owner = next(row for row in owners if row['address'] == binding['address'])
    if any(owner.get(key) != binding.get(key) for key in FIELDS):
        raise ValueError('Batch12 reviewed original identity changed')
    if sha(owner['before']) != owner['before_sha256'] or sha(owner['after']) != owner['after_sha256']:
        raise ValueError('Batch12 reviewed whole body hash changed')
    for key in ('source_file', 'source_symbol'):
        if audit.get(key) != owner[key]:
            raise ValueError('Batch12 scope native owner changed')
    gaps = owner['scope']['effects']
    if (audit.get('unresolved_semantics') != gaps or audit.get('waived_original_effects') != gaps or
            audit.get('original_equivalence') is not False or audit.get('implementation_status') != 'WIP' or
            audit.get('retained_semantics_complete') is not True or audit.get('retained_unresolved_semantics') != [] or
            audit.get('complete_mips_audit') is not True or audit.get('pseudocode_used_as_baseline') is not binding.get('pseudocode_complete')):
        raise ValueError('Batch12 retained audit or bounded gaps are incomplete')
    contexts = [packet['source_context'], *packet['source_context'].get('audit_cluster', [])]
    matches = [c for c in contexts if c['function']['image'] == binding['image'] and c['function']['address'] == binding['address']]
    if len(matches) != 1:
        raise ValueError('Batch12 full original context is missing')
    context = matches[0]
    from coverage_sdk_nested_delegation import _complete_context
    _complete_context(root, dict(binding, machine_audit=context['machine_audit']), context)
    if context['implementation']['path'] != owner['source_file'] or context['implementation']['symbol'] != owner['source_symbol']:
        raise ValueError('Batch12 packet native binding changed')
    words = dict(original_words(context))
    if binding['address'] in FRAME_RANGES:
        size = FRAME_RANGES[binding['address']][2]
        if list(words.values()).count(0x27BD0000 | ((-size) & 65535)) != 1 or list(words.values()).count(0x27BD0000 | size) != 1:
            raise ValueError('Batch12 original frame extent changed')
    waived = scope_checks(context)
    if audit.get('waived_machine_checks') != waived:
        raise ValueError('Batch12 waiver includes effects outside the exact authorized scope')
    if not [c for c in context['machine_audit']['checks'] if c['id'] not in waived]:
        raise ValueError('Batch12 retained original checklist is empty')
    return owner


def validate_sources(root, packet, proposal, changes, bindings):
    for audit in proposal.get('function_audits', []):
        if audit.get('replacement_kind') != REPLACEMENT:
            continue
        matches = [b for b in bindings if b['image'] == audit.get('image') and b['address'] == audit.get('address')]
        if len(matches) != 1:
            raise ValueError('Batch12 source has no exact required owner')
        owner = authorized_scope(root, packet, proposal, matches[0], audit)
        path = (Path(root) / owner['source_file']).resolve()
        if not path.is_relative_to((Path(root) / 'src').resolve()) or not path.is_file():
            raise ValueError('Batch12 scoped source escapes the project')
        text = changes.get(path, path.read_bytes())
        if isinstance(text, bytes):
            text = text.decode('utf-8-sig')
        text = text.replace('\r\n', '\n')
        parsed = [f for f in functions(text, owner['source_file']) if f['name'] == owner['source_symbol']]
        if len(parsed) != 1 or parsed[0]['markers'] != [(owner['address'], owner['image'])]:
            raise ValueError('Batch12 scoped source symbol or marker changed')
        body = text[parsed[0]['start']:parsed[0]['end']]
        if body != reviewed_body(owner):
            raise ValueError('Batch12 source differs from the complete reviewed candidate')
        for group in audit.get('machine_coverage', []):
            if group.get('source_file') != owner['source_file'] or group.get('source_symbol') != owner['source_symbol'] or not parsed[0]['start_line'] <= group.get('source_line', 0) <= parsed[0]['end_line']:
                raise ValueError('Batch12 mapping escapes its retained whole body')


def summary(proposal):
    keys = (*FIELDS, 'source_file', 'source_symbol', 'replacement_kind', 'original_equivalence',
            'implementation_status', 'unresolved_semantics', 'waived_original_effects',
            'waived_machine_checks', 'game_scope_review', 'game_scope_authorization')
    return [{key: audit[key] for key in keys} for audit in proposal.get('function_audits', []) if audit.get('replacement_kind') == REPLACEMENT]


def validate_updates(root, packet, proposal):
    updates = proposal.get('game_sp_ra_prototype_updates') or []
    if not updates:
        return []
    expected = {'path': 'src/game_native_delivery_2.c', 'before': 'uint32 sub_8002F998(uint32 object);',
                'after': 'uint32 sub_8002F998(uint32 object, uint32 incoming_sp, uint32 incoming_ra);'}
    if updates != [expected]:
        raise ValueError('Batch12 SP/RA migration permits one exact existing declaration')
    required = packet['source_context']['audit_contract']['required_functions']
    for address in (0x8002F998, 0x8002E2BC):
        binding = next((b for b in required if b['address'] == address and b['image'] == 'SLUS_005.10'), None)
        audit = next((a for a in proposal.get('function_audits', []) if a.get('address') == address and a.get('image') == 'SLUS_005.10'), None)
        if binding is None or audit is None or authorized_scope(root, packet, proposal, binding, audit) is None:
            raise ValueError('Batch12 SP/RA migration requires both complete scoped owner audits')
    contexts = [packet['source_context'], *packet['source_context'].get('audit_cluster', [])]
    caller = next(c for c in contexts if c['function']['address'] == 0x8002E2BC)
    words = dict(original_words(caller))
    if words.get(0x8002E5B4) != 0x0C00BE66 or words.get(0x8002E5B8) != 0x02002021:
        raise ValueError('Batch12 SP/RA migration original direct call changed')
    import json
    config = json.loads((Path(root) / 'xport-project.json').read_text(encoding='utf-8-sig'))
    shared = Path(config['toolset']).parent
    for path in (shared / 'src').rglob('*'):
        if path.is_file() and path.suffix.lower() in ('.c', '.h'):
            logical, _ = phase2(path.read_text(encoding='utf-8-sig'))
            masked = lexical_tokens(logical)
            if re.search(r'\bsub_8002F998\b|##|%:%:', masked):
                raise ValueError('Batch12 SP/RA migration rejects shared references or token pasting')
    token = re.compile(r'\bsub_8002F998\s*\(')
    declaration_count = call_count = 0
    for path in (Path(root) / 'src').rglob('*'):
        if not path.is_file() or path.suffix.lower() not in ('.c', '.h'):
            continue
        text = path.read_text(encoding='utf-8-sig').replace('\r\n', '\n')
        logical, positions = phase2(text)
        masked = lexical_tokens(logical)
        if re.search(r'##|%:%:', masked):
            raise ValueError('Batch12 SP/RA migration rejects token pasting')
        for occurrence in re.finditer(r'\bsub_8002F998\b', masked):
            if positions[occurrence.end() - 1] - positions[occurrence.start()] != len('sub_8002F998') - 1:
                raise ValueError('Batch12 SP/RA migration rejects split identifiers')
        if any(line.lstrip().startswith('#') and re.search(r'\bsub_8002F998\b', line) for line in masked.splitlines()):
            raise ValueError('Batch12 SP/RA migration rejects directive references')
        parsed = functions(text, path.relative_to(root).as_posix())
        for match in token.finditer(text):
            enclosing = [f for f in parsed if f['start'] <= match.start() < f['end']]
            if enclosing and enclosing[0]['name'] == 'sub_8002F998':
                if match.start() != enclosing[0]['start'] + text[enclosing[0]['start']:].index('sub_8002F998'):
                    raise ValueError('Batch12 SP/RA migration rejects recursive or escaped owner references')
            elif enclosing:
                if enclosing[0]['name'] != 'sub_8002E2BC' or text[match.start():].split(';', 1)[0] != 'sub_8002F998(object)':
                    raise ValueError('Batch12 SP/RA migration has an unreviewed native caller')
                call_count += 1
            else:
                if path.relative_to(root).as_posix() != expected['path'] or not text[match.start():].startswith('sub_8002F998(uint32 object);') or text.count(expected['before']) != 1:
                    raise ValueError('Batch12 SP/RA migration has an unreviewed declaration')
                declaration_count += 1
        if text.count('sub_8002F998') != len(list(token.finditer(text))):
            raise ValueError('Batch12 SP/RA migration rejects target aliases and escapes')
    if declaration_count != 1 or call_count != 2:
        raise ValueError('Batch12 SP/RA migration native reference census changed')
    return updates


def validate_state(root, state):
    admissions = state.get('v8_batch12_scopes', [])
    if (not isinstance(admissions, list) or type(state.get('v8_batch12_scope_count', 0)) is not int or
            len(admissions) != state.get('v8_batch12_scope_count', 0) or
            len({(a['image'], a['address']) for a in admissions}) != len(admissions) or
            any(a['image'] != 'SLUS_005.10' or a['address'] not in OWNERS for a in admissions)):
        raise ValueError('Batch12 persisted scope count changed')
    for admission in admissions:
        rows = [f for f in state['functions'] if f['address'] == admission['address'] and f.get('image', state['image']) == admission['image']]
        if len(rows) != 1 or rows[0].get('proof_basis') != 'user_authorized_v8_batch12_scoped_audit' or rows[0].get('original_equivalence') is not False:
            raise ValueError('Batch12 persisted proof disposition changed')
        from coverage import read_json
        receipt_path = (Path(root) / rows[0]['audit_receipt']).resolve()
        if not receipt_path.is_relative_to((Path(root) / 'status').resolve()):
            raise ValueError('Batch12 persisted receipt escapes the project')
        receipt = read_json(receipt_path)
        if admission not in summary(receipt) or receipt.get('v8_batch12_scope_admissions') != summary(receipt):
            raise ValueError('Batch12 persisted receipt scope changed')
        packet_path = receipt_path.with_name(receipt_path.name.replace('.audit.json', '.json'))
        packet = read_json(packet_path)
        audit = next(a for a in receipt['function_audits'] if a['address'] == admission['address'] and a['image'] == admission['image'])
        binding = next(b for b in packet['source_context']['audit_contract']['required_functions'] if b['address'] == admission['address'] and b['image'] == admission['image'])
        authorized_scope(root, packet, receipt, binding, audit)

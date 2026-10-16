"""Four exact human-approved game frame bodies, retaining ordinary effects"""

import re

from coverage_game_frame_delegation import FIELDS, PROTOTYPES, _review, sha
from source_index import functions

SCOPE_SHA256 = 'a2b095acdd43df19a244c71c5951a1c7e75baf4f89010eab47727f6337daeac9'
OWNERS = {0x80016024: 216, 0x80016364: 80, 0x80043C34: 32, 0x8001DCC8: 96}
AUTH_KEYS = {'schema', 'contract', 'user_instruction', 'scope_review', 'owners',
             'authorization_granted', 'ordinary_memory_effects_waived',
             'original_equivalence', 'implementation_status', 'scope'}


def frame_check_release(check, size):
    word = int(check['word'], 0)
    return word >> 26 == 35 or (word >> 26 == 9 and word & 65535 == size)


def authorized_batch10_frame(root, packet, proposal, binding, audit):
    address = binding.get('address')
    if address in (0x8003E2FC, 0x8003E520):
        from coverage_v8_batch13_frames import authorized_frame
        return authorized_frame(root, packet, proposal, binding, audit)
    if root is None or address not in OWNERS or binding.get('image') != 'SLUS_005.10' or binding.get('library_classification'):
        raise ValueError('Batch10 frame authorization exceeds four exact game owners')
    if packet.get('contract') != 'xport-function-audit-packet-v1':
        raise ValueError('Batch10 frame requires authoritative audit packet')
    if any(audit.get(k) != binding.get(k) for k in FIELDS):
        raise ValueError('Batch10 frame original binding changed')
    auth = _review(root, audit.get('guest_frame_authorization'))
    if set(auth) != AUTH_KEYS or type(auth['schema']) is not int or auth['schema'] != 1 or auth['contract'] != 'xport-user-authorized-batch10-game-frame-scope-v1' or auth['user_instruction'] != 'ок, разрешаю' or auth['authorization_granted'] is not True or auth['ordinary_memory_effects_waived'] is not False or auth['original_equivalence'] is not False or auth['implementation_status'] != 'WIP' or auth['scope'] != 'Only four reviewed owners; exact guest-frame identity, alias, lifetime and saved ABI slots gaps; all other game effects remain required':
        raise ValueError('Batch10 frame lacks exact separate human authorization')
    scope_ref = auth['scope_review']
    if not isinstance(scope_ref, dict) or scope_ref.get('sha256') != SCOPE_SHA256 or audit.get('guest_frame_review') != scope_ref:
        raise ValueError('Batch10 frame review differs from immutable human-reviewed scope')
    scope = _review(root, scope_ref)
    if scope.get('contract') != 'xport-review-pending-batch10-game-frame-scope-v1' or scope.get('accepted_claim') is not False:
        raise ValueError('Batch10 frame scope contract changed')
    approved = scope.get('owners', [])
    expected_auth = [{'image': o['image'], 'address': o['address'], 'original_hashes': o['original_hashes'], 'candidate_text_sha256': o['candidate_text_sha256'], 'frame_bytes': o['frame_bytes']} for o in approved]
    if auth['owners'] != expected_auth or {o['address']: o['frame_bytes'] for o in approved} != OWNERS or len(approved) != 4:
        raise ValueError('Batch10 frame authorization owner set changed')
    row = next(o for o in approved if o['address'] == address)
    if any(binding.get(k) != v for k, v in row['original_hashes'].items()) or audit.get('source_symbol') != row['symbol'] or row['complete_original_and_available_pseudocode_read'] is not True:
        raise ValueError('Batch10 frame original or native owner differs from reviewed scope')
    text = row['candidate_text']
    if sha(text) != row['candidate_text_sha256']:
        raise ValueError('Batch10 frame reviewed whole native body changed')
    contexts = [packet.get('source_context', {}), *packet.get('source_context', {}).get('audit_cluster', [])]
    matches = [c for c in contexts if c.get('function', {}).get('image') == binding['image'] and c.get('function', {}).get('address') == address and c.get('function', {}).get('sha256') == binding['function_sha256']]
    if len(matches) != 1:
        raise ValueError('Batch10 frame complete original context missing')
    context = matches[0]
    if context.get('machine_audit', {}).get('sha256') != binding['machine_audit_sha256'] or context.get('mips', {}).get('sha256') != binding['mips_sha256'] or context.get('pseudocode', {}).get('sha256') != binding['pseudocode_sha256']:
        raise ValueError('Batch10 frame immutable original context hashes changed')
    from coverage_sdk_nested_delegation import _complete_context
    _complete_context(root, dict(binding, machine_audit=context['machine_audit']), context)
    checks = context['machine_audit']['checks']
    frame_checks = row['proposed_abi_only_check_ids']
    gaps = row['excluded_frame_effects'].copy()
    waived = frame_checks.copy()
    callback = None
    if audit.get('callback_abi_authorization') is not None or audit.get('gte_projection_review') is not None:
        if address != 0x80016364:
            raise ValueError('Callback/GTE review cannot extend another frame owner')
        from coverage_16364_callback_delegation import callback_scope
        callback = callback_scope(root, packet, proposal, binding, audit, row, context)
        gaps.extend(callback['gaps'])
        wanted = set(waived + callback['check_ids'])
        waived = [c['id'] for c in checks if c['id'] in wanted]
        text = callback['result_text']
    if audit.get('implementation_status') != 'WIP' or audit.get('original_equivalence') is not False or audit.get('unresolved_semantics') != gaps or audit.get('waived_original_effects') != gaps or audit.get('retained_semantics_complete') is not True or audit.get('retained_unresolved_semantics') != [] or audit.get('complete_mips_audit') is not True or audit.get('pseudocode_used_as_baseline') is not binding.get('pseudocode_complete'):
        raise ValueError('Batch10 frame retained proof or explicit bounded gaps missing')
    if audit.get('waived_machine_checks') != waived or [c['id'] for c in checks if c['id'] in waived] != waived or len(set(waived)) != len(waived):
        raise ValueError('Batch10 frame waiver includes non-approved effects')
    # Check each ABI operation against its original encoded instruction
    words = {}
    for line in context.get('mips', {}).get('lines', []):
        match = re.match(r'^([0-9A-Fa-f]{8})\s+((?:[0-9A-Fa-f]{2}\s+)+)', line)
        if match:
            raw = bytes.fromhex(match.group(2))
            for i in range(0, len(raw), 4):
                words[int(match.group(1), 16) + i] = int.from_bytes(raw[i:i+4], 'little')
    size = OWNERS[address]
    if list(words.values()).count(0x27BD0000 | ((-size) & 65535)) != 1 or list(words.values()).count(0x27BD0000 | size) != 1:
        raise ValueError('Batch10 frame original allocation size changed')
    for check in checks:
        if check['id'] not in frame_checks:
            continue
        word = int(check['word'], 0)
        if words.get(int(check['pc'], 0)) != word:
            raise ValueError('Batch10 frame ABI checklist word differs from original')
        op, base, reg, offset = word >> 26, (word >> 21) & 31, (word >> 16) & 31, word & 65535
        saved = op in {35, 43} and base == 29 and reg in {*range(16, 24), 31}
        outgoing = address == 0x80016024 and op == 43 and base == 29 and reg == 17 and offset == 16
        release = op == 9 and base == reg == 29 and offset == size
        if not (saved or outgoing or release):
            raise ValueError('Batch10 frame waived check is ordinary algorithm effect')
    # Preserve the approved frame scope while correcting the original ready callback
    if address == 0x80043C34:
        call = next((c for c in checks if c['id'] == '0x80043ca8:call'), None)
        if words.get(0x80043CA8) != 0x0C0123EF or call is None or int(call['word'], 0) != 0x0C0123EF or call['id'] in waived:
            raise ValueError('Batch10 ready callback correction lacks exact retained original JAL')
        before = 'result = CdSyncCallbackPSX(0u);'
        after = 'result = CdReadyCallbackPSX(0u);'
        if text.count(before) != 1 or after in text:
            raise ValueError('Batch10 approved callback correction anchor changed')
        text = text.replace(before, after, 1)
    normal = [c for c in checks if c['id'] not in waived]
    if not any(c['kind'] in {'load', 'store'} for c in normal) or not any(c['kind'] == 'call' for c in normal):
        raise ValueError('Batch10 frame retained normal algorithm checklist missing')
    parsed = functions(text, 'approved-frame')
    if len(parsed) != 1 or parsed[0]['name'] != row['symbol']:
        raise ValueError('Batch10 frame reviewed native owner is not unique')
    entries = []
    for prototype in PROTOTYPES:
        symbol = prototype.split('(')[0].split()[-1]
        entries.append({'kind': 'system_bridge', 'symbol': symbol, 'prototype': prototype,
                        'source_file': '@xport/src/psx.c', 'caller_image': binding['image'],
                        'caller_address': address, 'caller_function_sha256': binding['function_sha256']})
    return {'review': {'result_text': text, 'frame_size': size}, 'entries': entries,
            'frame_machine_checks': frame_checks, 'callback': callback}

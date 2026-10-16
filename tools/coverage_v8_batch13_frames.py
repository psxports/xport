"""Validate two exact human-authorized V8 addressable frame candidates"""

import hashlib

from coverage_game_frame_delegation import FIELDS, PROTOTYPES, _review
from coverage_native_abi_frame import original_words
from coverage_sdk_nested_delegation import _complete_context
from source_index import functions

REVIEW_SHA = '824b65f317950cc5e0715c7852a98e194d8df18b8e3542bd55fcb0fb4538baac'
AUTH_SHA = 'fe42410d7841b6fbd2c73fcfec16b06443e77a08baaa4853bdd8f681a7d16c6e'
OWNERS = {0x8003E2FC: (128, 104, 120), 0x8003E520: (56, 48, 52)}


def authorized_frame(root, packet, proposal, binding, audit):
    address = binding.get('address')
    if (root is None or address not in OWNERS or binding.get('image') != 'SLUS_005.10'
            or binding.get('library_classification')
            or packet.get('contract') != 'xport-function-audit-packet-v1'):
        raise ValueError('Batch13 frame exceeds two exact recorded owners')
    auth_ref = audit.get('guest_frame_authorization')
    review_ref = audit.get('guest_frame_review')
    if (not isinstance(auth_ref, dict) or auth_ref.get('sha256') != AUTH_SHA
            or not isinstance(review_ref, dict) or review_ref.get('sha256') != REVIEW_SHA):
        raise ValueError('Batch13 frame requires exact human authorization and review')
    auth = _review(root, auth_ref)
    review = _review(root, review_ref)
    if (auth.get('review') != review_ref or auth.get('answer') != 'Разрешаю эти два frame scope'
            or auth.get('owners') != list(OWNERS) or auth.get('scope_expansion_allowed') is not False
            or auth.get('original_equivalence') is not False or auth.get('accepted_proofs_added') != 0):
        raise ValueError('Batch13 frame human authorization scope changed')
    rows = review.get('owners', [])
    if len(rows) != 2 or {row.get('address') for row in rows} != set(OWNERS):
        raise ValueError('Batch13 frame reviewed owner set changed')
    row = next(row for row in rows if row['address'] == address)
    if any(row['original_binding'].get(key) != binding.get(key)
           or audit.get(key) != binding.get(key) for key in FIELDS):
        raise ValueError('Batch13 frame original bindings changed')
    contexts = [packet['source_context'], *packet['source_context'].get('audit_cluster', [])]
    matches = [c for c in contexts if c['function']['image'] == binding['image']
               and c['function']['address'] == address]
    if len(matches) != 1:
        raise ValueError('Batch13 frame complete original context missing')
    context = matches[0]
    _complete_context(root, dict(binding, machine_audit=context['machine_audit']), context)
    size, low, high = OWNERS[address]
    words = original_words(context)
    if (hashlib.sha256(b''.join(word.to_bytes(4, 'little') for _, word in words)).hexdigest()
            != binding['function_sha256']
            or sum(word == (0x27BD0000 | ((-size) & 65535)) for _, word in words) != 1
            or sum(word == (0x27BD0000 | size) for _, word in words) != 1):
        raise ValueError('Batch13 frame raw original or allocation changed')
    eligible = []
    for check in context['machine_audit']['checks']:
        word = int(check['word'], 0)
        op, base, reg, offset = word >> 26, (word >> 21) & 31, (word >> 16) & 31, word & 65535
        saved = op in (35, 43) and base == 29 and reg in (*range(16, 24), 31)
        saved = saved and low <= offset <= high and offset % 4 == 0
        released = op == 9 and base == reg == 29 and offset == size
        if saved or released:
            eligible.append(check['id'])
    gaps = row['scope']['excluded_effects']
    if (row['scope']['frame_bytes'] != size or row['scope']['saved_abi_checks'] != eligible
            or audit.get('waived_machine_checks') != eligible
            or audit.get('unresolved_semantics') != gaps or audit.get('waived_original_effects') != gaps
            or audit.get('retained_semantics_complete') is not True
            or audit.get('retained_unresolved_semantics') != []
            or audit.get('implementation_status') != 'WIP' or audit.get('original_equivalence') is not False
            or audit.get('complete_mips_audit') is not True
            or audit.get('pseudocode_used_as_baseline') is not binding['pseudocode_complete']):
        raise ValueError('Batch13 frame excludes retained effects or lacks complete retained audit')
    body = row['after']
    parsed = functions(body, row['source_file'])
    if (hashlib.sha256(body.encode()).hexdigest() != row['after_sha256']
            or hashlib.sha256(row['before'].encode()).hexdigest() != row['before_sha256']
            or len(parsed) != 1 or parsed[0]['name'] != row['source_symbol']
            or parsed[0]['markers'] != [(address, binding['image'])]
            or audit.get('source_file') != row['source_file']
            or audit.get('source_symbol') != row['source_symbol']):
        raise ValueError('Batch13 frame reviewed body or native owner changed')
    entries = []
    for prototype in PROTOTYPES:
        entries.append({'kind': 'system_bridge', 'symbol': prototype.split('(')[0].split()[-1],
                        'prototype': prototype, 'source_file': '@xport/src/psx.c',
                        'caller_image': binding['image'], 'caller_address': address,
                        'caller_function_sha256': binding['function_sha256']})
    return {'review': {'result_text': body, 'frame_size': size}, 'entries': entries,
            'frame_machine_checks': eligible, 'callback': None}

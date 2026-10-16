"""Bind a native-only input/output alias to complete upstream MIPS audits"""

import copy
import hashlib
import json
import re

from lockstep_audit import checklist, words_from_listing
from lockstep_packet import _audit_function_binding
from source_context import compact


def _sample(rows, address, size):
    matches = [row for row in rows or [] if int(row['address'], 16) == address and row['size'] >= size]
    if len(matches) != 1:
        raise ValueError('Alias input memory is missing or ambiguous')
    value = bytes.fromhex(matches[0]['data'])
    if len(value) != matches[0]['size']:
        raise ValueError('Alias input memory is truncated')
    return value[:size]


def _entry(probe, address, symbol, frame, storage="guest"):
    alignment = probe.get('alignment') or {}
    if (probe.get('complete') is not True or probe.get('target_frame') != frame - 1 or
            alignment.get('aligned') is not True or
            (alignment.get('comparison') or {}).get('status') != 'match'):
        raise ValueError('Alias evidence lacks its matched initial boundary')
    hits = [row for row in probe.get('hits') or [] if int(row.get('pc', '0'), 16) == address]
    native = probe.get('native_entry') or {}
    if len(hits) != 1 or native.get('symbol') != symbol:
        raise ValueError('Alias function entry is absent or ambiguous')
    if native.get('second_storage') != storage:
        raise ValueError('Alias evidence requires explicit guest storage')
    arguments = native.get('arguments') or []
    if len(arguments) != 4:
        raise ValueError('Alias native arguments are incomplete')
    return hits[0], native, [int(value, 16) for value in arguments]


def validate_alias_evidence(owner_probe, callee_probe, writer_probe, owner, callee, writer, frame):
    original, native, arguments = _entry(owner_probe, owner, f'sub_{owner:08X}', frame)
    received, native_callee, callee_arguments = _entry(callee_probe, callee, f'sub_{callee:08X}', frame)
    output, native_writer, writer_arguments = _entry(writer_probe, writer, f'sub_{writer:08X}', frame, 'none')
    source_position = int(original['registers']['r5'], 16)
    source_center = int(original['registers']['r6'], 16)
    source_output = int(original['registers']['r4'], 16)
    original_rows = original.get('memory') or []
    native_rows = (native.get('memory') or []) + (native.get('probe_memory') or [])
    # Prefer probe reads when the argument sample covers the same input
    native_rows = {int(row['address'], 16): row for row in native_rows}
    native_rows = list(native_rows.values())
    for source, target, size in ((source_position, arguments[1], 12), (source_center, arguments[2], 6)):
        if _sample(original_rows, source, size) != _sample(native_rows, target, size):
            raise ValueError('Alias function inputs already differ')
    if (int(received['registers']['r4'], 16) != source_position or
            callee_arguments[0] != arguments[1] or callee_arguments[1] != arguments[2]):
        raise ValueError('Native callee does not overwrite the live center input')
    callee_native_rows = {int(row['address'], 16): row for row in
                          (native_callee.get('memory') or []) +
                          (native_callee.get('probe_memory') or [])}
    for source, target, size in ((source_position, arguments[1], 12),
                                 (source_center, arguments[2], 6)):
        expected = _sample(original_rows, source, size)
        if (expected != _sample(received.get('memory'), source, size) or
                expected != _sample(list(callee_native_rows.values()), target, size)):
            raise ValueError('Alias callee inputs changed before the helper call')
    if int(received['registers']['r6'], 16) != callee_arguments[2]:
        raise ValueError('Alias helper player inputs differ')
    source_callee_output = int(received['registers']['r5'], 16)
    if source_callee_output < source_center + 6 and source_center < source_callee_output + 6:
        raise ValueError('The alleged alias also exists in Original')
    if (int(output['registers']['r4'], 16) != source_output or
            writer_arguments[0] != arguments[0]):
        raise ValueError('Alias output does not reach the divergent writer')
    owner_caller = native.get('caller_symbol', '').split('+', 1)[0]
    if not owner_caller or native_writer.get('caller_symbol', '').split('+', 1)[0] != owner_caller:
        raise ValueError('Alias owner and writer have different callers')
    if not re.search(rf'!sub_{owner:08X}\+0x', native_callee.get('caller_symbol', ''), re.I):
        raise ValueError('Alias callee is outside the proposed owner')
    if int(received['registers']['r31'], 16) - 8 < owner:
        raise ValueError('Original alias callee return is outside the owner')
    source_quad = _sample(output.get('memory'), source_output, 32)
    native_quad = _sample(native_writer.get('memory'), arguments[0], 32)
    if source_quad[:6] == native_quad[:6] or source_quad[16:22] != native_quad[16:22]:
        raise ValueError('Alias evidence does not isolate the overwritten input')
    return dict(source_center=f'0x{source_center:08X}', native_center=f'0x{arguments[2]:08X}',
                source_callee_output=f'0x{source_callee_output:08X}',
                native_callee_output=f'0x{callee_arguments[1]:08X}',
                source_quad=source_quad.hex(), native_quad=native_quad.hex())


def read_buffer_alias_request(root, directory, request, previous, source_roots):
    from lockstep_paired_entry import _address, _evidence

    keys = {'kind', 'buffer_alias', 'image', 'owner_function', 'callee_function',
            'owner_file', 'owner_sha256', 'callee_file', 'callee_sha256', 'writer_file', 'writer_sha256'}
    if set(request) != keys or request['kind'] != 'paired_entry' or request['buffer_alias'] is not True:
        raise ValueError('Invalid input alias request')
    owner, callee = _address(request['owner_function']), _address(request['callee_function'])
    image = request['image']
    old_context = previous.get('source_context') or {}
    writer = (old_context.get('function') or {}).get('address')
    if image != old_context.get('image') or not isinstance(writer, int):
        raise ValueError('Alias evidence has no current writer identity')
    probes = [_evidence(directory, request[name + '_file'], request[name + '_sha256'])
              for name in ('owner', 'callee', 'writer')]
    frame = previous['expected_original_event']['frame']
    evidence = validate_alias_evidence(*probes, owner, callee, writer, frame)
    contexts = [compact(image, address, 262144, True, address, full_original=True)
                for address in (owner, callee)] + [copy.deepcopy(old_context)]
    for context in contexts:
        if any(context[key].get('complete') is not True for key in ('implementation', 'mips')):
            raise ValueError('Alias audit source is incomplete')
        context['machine_audit'] = checklist(context['mips']['lines'])
    call_pc = int(probes[1]['hits'][0]['registers']['r31'], 16) - 8
    words = words_from_listing(contexts[0]['mips']['lines'])
    if call_pc not in words or (words[call_pc] >> 26) != 3 or ((words[call_pc] & 0x03ffffff) << 2 | 0x80000000) != callee:
        raise ValueError('Original alias helper call is not proven by MIPS')
    contexts[0]['audit_cluster'] = contexts[1:]
    contexts[0]['audit_contract'] = dict(old_context['audit_contract'],
        required_functions=[_audit_function_binding(row) for row in contexts])
    packet = copy.deepcopy(previous)
    packet['source_context'] = contexts[0]
    packet['diagnostic_owner'] = {'kind': 'native_buffer_alias', 'function': f'0x{owner:08X}'}
    packet['buffer_alias_provenance'] = dict(evidence, request=request)
    packet['cause_statement'] = 'Equal position and center inputs enter the owner; its Native callee output aliases the center while Original uses a separate local, and the resulting quad reaches the same divergent writer'
    packet['failure_signature'] = hashlib.sha256(json.dumps(
        {'prior': previous['failure_signature'], 'alias': evidence, 'request': request}, sort_keys=True).encode()).hexdigest()
    packet['serialized_bytes'] = len(json.dumps(packet).encode())
    if packet['serialized_bytes'] > packet['budget_bytes']:
        raise ValueError('Alias audit packet exceeds its byte budget')
    return packet

"""Bind declaration repair to a complete unmapped callee and every native caller"""
from __future__ import annotations
import copy
import hashlib
from pathlib import Path
import re
from source_index import functions, mask_c
from lockstep_audit import checklist, words_from_listing
from source_context import reviewed_library_identity

FIELDS = ('image', 'address', 'function_sha256', 'mips_sha256',
          'pseudocode_sha256', 'machine_audit_sha256')
RETURNS = r'(?:void|uint32|sint32)'
PARAM = re.compile(r'(?P<type>(?:const\s+)?(?:char|uint8|uint32|sint32)(?:\s*\*)?)(?:\s+|(?<=\*))(?P<name>[A-Za-z_]\w*)$')


def prototype(text, symbol, allow_unspecified=False):
    pattern = rf'(?:extern\s+)?({RETURNS})\s+{re.escape(symbol)}\s*\(([^()]*)\)\s*;'
    match = re.fullmatch(pattern, text.strip())
    if not match:
        raise ValueError('Corrective prototype must be one plain named ABI')
    if not match[2].strip() and allow_unspecified:
        return {'return_type': match[1], 'parameters': None}
    tokens = [] if match[2].strip() == 'void' else match[2].split(',')
    if len(tokens) > 4 or any(not token.strip() for token in tokens):
        raise ValueError('Corrective prototype supports at most four explicit register arguments')
    parameters = []
    for token in tokens:
        parameter = PARAM.fullmatch(token.strip())
        if not parameter:
            raise ValueError('Corrective prototype rejects variadic, array or function pointer parameters')
        parameters.append({'type': re.sub(r'\s*\*\s*', ' *', re.sub(r'\s+', ' ', parameter['type'].strip())), 'name': parameter['name']})
    if len({p['name'] for p in parameters}) != len(parameters):
        raise ValueError('Corrective prototype parameter names must be unique')
    return {'return_type': match[1], 'parameters': parameters}


def phase2(text):
    logical, positions, start = [], [], 0
    for removed in re.finditer(r'\\\r?\n', text):
        logical.append(text[start:removed.start()])
        positions.extend(range(start, removed.start()))
        start = removed.end()
    logical.append(text[start:])
    positions.extend(range(start, len(text)))
    return ''.join(logical), positions


def lexical_tokens(text):
    return re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                  lambda match: re.sub(r'[^\r\n]', ' ', match.group(0)), text, flags=re.S)


def native_references(root, snapshot, symbol, required, stub):
    declarations, callers = [], []
    pattern = re.compile(rf'(?:extern\s+)?{RETURNS}\s+{re.escape(symbol)}\s*\([^{{}};#]*\)\s*;')
    token_pattern = re.compile(rf'\b{re.escape(symbol)}\b')
    for relative in sorted(snapshot['files']):
        path = root / relative
        text = path.read_bytes().decode('utf-8-sig')
        # Apply escaped-newline removal before inspecting directive tokens
        logical, positions = phase2(text)
        if '##' in logical or '%:%:' in logical:
            raise ValueError('Corrective prototype rejects token-pasting source')
        token_text = lexical_tokens(logical)
        for occurrence in token_pattern.finditer(token_text):
            first, last = occurrence.start(), occurrence.end() - 1
            if positions[last] - positions[first] != last - first:
                raise ValueError('Corrective prototype rejects split target identifiers')
        for token_line in token_text.splitlines():
            if token_line.lstrip().startswith('#') and token_pattern.search(token_line):
                raise ValueError('Corrective prototype rejects macro or directive target aliases')
        masked = mask_c(text)
        bodies = functions(text, relative)
        covered = []
        for declaration in pattern.finditer(masked):
            start, end = declaration.span()
            if any(f['start'] <= start < f['end'] for f in bodies):
                raise ValueError('Corrective prototype cannot alter a function-local declaration')
            prefix = text[text.rfind('\n', 0, start) + 1:start]
            if prefix.lstrip().startswith('#'):
                raise ValueError('Corrective prototype cannot alter a preprocessor directive')
            before = text[start:end]
            prototype(before, symbol, allow_unspecified=True)
            declarations.append({'path': relative, 'before': before, 'start': start})
            covered.append((start, end))
        for body in bodies:
            if body['name'] == symbol:
                raise ValueError('Corrective prototype cannot change a real mapped callee')
            body_text = masked[body['start']:body['end']]
            occurrences = list(token_pattern.finditer(body_text))
            if not occurrences:
                continue
            if len(body['markers']) != 1 or (body['markers'][0][1], body['markers'][0][0]) not in required:
                raise ValueError('Every mapped native caller must be one of the complete corrective owners')
            implementation = required[(body['markers'][0][1], body['markers'][0][0])]
            if (implementation['path'] != relative or implementation['symbol'] != body['name'] or
                    implementation['sha256'] != body['sha256']):
                raise ValueError('Corrective caller reference is not the exact required mapped body')
            for occurrence in occurrences:
                # Only direct C calls can carry a repaired declaration
                suffix = body_text[occurrence.end():]
                if not re.match(r'\s*\(', suffix):
                    raise ValueError('Corrective prototype rejects escaping function-pointer references')
                covered.append((body['start'] + occurrence.start(), body['start'] + occurrence.end()))
            callers.append({'image': body['markers'][0][1], 'address': body['markers'][0][0],
                            'path': relative, 'symbol': body['name'], 'body_sha256': body['sha256']})
        if stub and relative == stub['source']:
            line = f'XPORT_LINK_STUB({symbol}, 0x{stub["address"]:08X}u, "{stub["image"]}")'
            if text.count(line) != 1:
                raise ValueError('Corrective prototype target is not its exact supervisor link stub')
            location = text.index(line)
            covered.append((location, location + len(line)))
        for occurrence in token_pattern.finditer(masked):
            if not any(start <= occurrence.start() < end for start, end in covered):
                raise ValueError('Corrective prototype has an unreviewed macro, alias or outside reference')
    if not declarations or not callers:
        raise ValueError('Corrective prototype needs existing declarations and mapped direct callers')
    return sorted(declarations, key=lambda r: (r['path'], r['start'])), sorted(callers, key=lambda r: (r['path'], r['address']))


def build_bindings(root, project, state, views, requests, snapshot, compact, budget=262144, shared_root=None, shared_snapshot=None):
    if not isinstance(requests, list) or len(requests) > 2:
        raise ValueError('Corrective prototype repairs must be a list of at most two callees')
    if requests and (shared_root is None or shared_snapshot is None):
        raise ValueError('Corrective prototype needs a complete shared source reference scan')
    required = {(v['image'], v['function']['address']): v['implementation'] for v in views}
    result, seen = [], set()
    for request in requests:
        if not isinstance(request, dict) or set(request) != {'image', 'address', 'symbol', 'prototype'}:
            raise ValueError('Invalid corrective callee request')
        image, address, symbol = request['image'], request['address'], request['symbol']
        if not isinstance(image, str) or type(address) is not int or not isinstance(symbol, str):
            raise ValueError('Corrective callee identity must be image qualified')
        key = image, address
        if key in seen or key in required:
            raise ValueError('Corrective callee cannot duplicate a owner or another repair')
        seen.add(key)
        abi = prototype(request['prototype'], symbol)
        token_pattern = re.compile(rf'\b{re.escape(symbol)}\b')
        for relative in shared_snapshot['files']:
            shared_text, _positions = phase2((shared_root / relative).read_bytes().decode('utf-8-sig'))
            if '##' in shared_text or '%:%:' in shared_text:
                raise ValueError('Corrective prototype rejects shared token-pasting source')
            if token_pattern.search(shared_text):
                raise ValueError('Corrective game prototype has an unauthorized shared source reference')
        if reviewed_library_identity(image, address) is not None:
            raise ValueError('Corrective game prototype cannot alter a reviewed SDK callee')
        view = compact(image, address, budget, full_source=True, full_original=True)
        if (view['function']['name'] != symbol or view['implementation'].get('mapping_status') != 'unmapped' or
                not view['mips'].get('complete') or
                (view['pseudocode'].get('path') and
                 (view['pseudocode'].get('first_line') != 1 or view['pseudocode'].get('problem') or
                  not view['pseudocode'].get('sha256') or
                  view['pseudocode'].get('content_complete', view['pseudocode'].get('complete')) is not True or
                  len(view['pseudocode'].get('lines', [])) != view['pseudocode'].get('total_lines')))):
            raise ValueError('Corrective callee must be uniquely unmapped with complete original evidence')
        recorded = [r for r in state['functions'] if
                    r.get('image', state.get('image')) == image and r['address'] == address]
        stubs = [r for r in state.get('link_only_stubs', {}).get('functions', []) if
                 r['image'] == image and r['address'] == address and r['symbol'] == symbol]
        if recorded:
            if len(recorded) != 1 or recorded[0].get('proof_status') != 'pending' or recorded[0].get('c_status') != 'missing':
                raise ValueError('Corrective callee is an implemented or accepted recorded function')
        elif len(stubs) != 1:
            raise ValueError('Unrecorded corrective callee must be a recognized supervisor link-only TODO stub')
        stub = None
        if stubs:
            stub = {**stubs[0], 'source': state['link_only_stubs']['source']}
            if abi['return_type'] != 'uint32':
                raise ValueError('Link-only stub repair must retain its uint32 return ABI')
        words = words_from_listing(view['mips']['lines'])
        raw = b''.join(word.to_bytes(4, 'little') for _, word in sorted(words.items()))
        if (sorted(words) != list(range(address, view['function']['end'], 4)) or
                len(raw) != view['function']['bytes'] or hashlib.sha256(raw).hexdigest() != view['function']['sha256']):
            raise ValueError('Corrective callee complete MIPS does not bind the original function hash')
        machine = checklist(view['mips']['lines'])
        declarations, callers = native_references(root, snapshot, symbol, required, stub)
        # The original caller must have a direct original call to this exact entry
        for caller in callers:
            original = next(v for v in views if (v['image'], v['function']['address']) == (caller['image'], caller['address']))
            if caller['image'] != image:
                raise ValueError('Corrective prototype requires original same-image call evidence')
            direct = []
            for pc, word in words_from_listing(original['mips']['lines']).items():
                opcode = word >> 26
                if opcode in (2, 3):
                    direct.append(((pc + 4) & 0xF0000000) | ((word & 0x3FFFFFF) << 2))
                elif opcode == 1 and ((word >> 16) & 31) in (16, 17):
                    displacement = word & 0xFFFF
                    if displacement & 0x8000:
                        displacement -= 0x10000
                    direct.append((pc + 4 + (displacement << 2)) & 0xFFFFFFFF)
            if address not in direct:
                raise ValueError('Corrective native caller has no exact original direct callee edge')
        binding = dict(image=image, address=address, function_sha256=view['function']['sha256'],
                       mips_sha256=view['mips']['sha256'], pseudocode_sha256=view['pseudocode']['sha256'],
                       machine_audit_sha256=machine['sha256'])
        result.append({'request': copy.deepcopy(request), 'binding': binding, 'context': view,
                       'machine_audit': machine, 'abi': abi, 'declarations': declarations,
                       'mapped_callers': callers, 'caller_contexts': [copy.deepcopy(v) for v in views
                           if any((v['image'], v['function']['address']) == (c['image'], c['address']) for c in callers)],
                       'shared_reference_scan_identity': shared_snapshot['identity'], 'scope': 'pending_recorded' if recorded else 'link_only_todo'})
    return result


def _writes_s1(word):
    opcode = word >> 26
    if opcode == 0:
        if (word & 63) not in (0, 2, 3, 4, 6, 7, 8, 9, 12, 13, 16, 17, 18, 19, 24, 25, 26, 27, 32, 33, 34, 35, 36, 37, 38, 39, 42, 43):
            raise ValueError('Saved S1 scan encountered an unsupported original instruction')
        return (word & 63) not in (8, 12, 13, 17, 19, 24, 25, 26, 27) and ((word >> 11) & 31) == 17
    if opcode not in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 32, 33, 34, 35, 36, 37, 38, 40, 41, 42, 43, 46, 48, 56):
        raise ValueError('Saved S1 scan encountered an unsupported original instruction')
    return opcode in (8, 9, 10, 11, 12, 13, 14, 15, 32, 33, 34, 35, 36, 37, 38, 48, 56) and ((word >> 16) & 31) == 17


def validate_saved_s1(bound, argument, proposal):
    """Validate a saved carrier without treating it as a third architectural argument"""
    rows = argument['saved_register_callers']
    callers = bound['mapped_callers']
    contexts = bound.get('caller_contexts', [])
    if not isinstance(rows, list) or len(rows) != len(callers):
        raise ValueError('Saved S1 needs every immutable mapped caller')
    callee_words = words_from_listing(bound['context']['mips']['lines'])
    evidence_words = [callee_words[item['pc']] for item in argument['evidence']]
    if not any(w >> 26 == 43 and ((w >> 21) & 31) == 29 and ((w >> 16) & 31) == 17 for w in evidence_words):
        raise ValueError('Saved S1 needs an original callee stack save')
    calls = [item['pc'] for item in argument['evidence'] if callee_words[item['pc']] >> 26 == 3]
    if not calls or any(_writes_s1(w) for pc, w in callee_words.items() if pc <= min(calls) + 4):
        raise ValueError('Saved S1 needs a forwarding call before its first definition')
    seen = set()
    for row in rows:
        keys = set(FIELDS) | {'call_pc', 'definition_pc', 'native_parameter', 'explanation'}
        if not isinstance(row, dict) or set(row) != keys:
            raise ValueError('Saved S1 caller evidence has foreign fields')
        key = row['image'], row['address']
        matches = [c for c in contexts if (c['image'], c['function']['address']) == key]
        mapped = [c for c in callers if (c['image'], c['address']) == key]
        if key in seen or len(matches) != 1 or len(mapped) != 1:
            raise ValueError('Saved S1 caller is duplicate or unknown')
        seen.add(key)
        context = matches[0]
        words = words_from_listing(context['mips']['lines'])
        expected = dict(image=key[0], address=key[1], function_sha256=context['function']['sha256'],
                        mips_sha256=context['mips']['sha256'], pseudocode_sha256=context['pseudocode']['sha256'],
                        machine_audit_sha256=checklist(context['mips']['lines'])['sha256'])
        raw = b''.join(w.to_bytes(4, 'little') for _, w in sorted(words.items()))
        if (not context['mips'].get('complete') or hashlib.sha256(raw).hexdigest() != expected['function_sha256'] or
                any(row[k] != expected[k] for k in FIELDS)):
            raise ValueError('Saved S1 caller original hashes are stale')
        definition, call = row['definition_pc'], row['call_pc']
        if type(definition) is not int or type(call) is not int or definition not in words or call not in words or definition >= call or call + 4 not in words:
            raise ValueError('Saved S1 definition or call PC is foreign')
        word = words[definition]
        rs, rt, rd = (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31
        source = rs if rt == 0 else rt if rs == 0 else -1
        if word >> 26 != 0 or (word & 63) not in (33, 37) or rd != 17 or source not in range(4, 8):
            raise ValueError('Saved S1 must originate in an exact original scalar argument move')
        jump = words[call]
        target = ((call + 4) & 0xF0000000) | ((jump & 0x3FFFFFF) << 2)
        original_calls = [pc for pc, w in words.items() if w >> 26 == 3 and
                          (((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)) == bound['binding']['address']]
        if original_calls != [call]:
            raise ValueError('Saved S1 route needs exactly one original callee call per owner')
        if jump >> 26 != 3 or target != bound['binding']['address'] or any(_writes_s1(w) for pc, w in words.items() if definition < pc <= call + 4):
            raise ValueError('Saved S1 caller clobbers its carrier or calls another target')
        if not isinstance(row['explanation'], str) or not row['explanation'].strip():
            raise ValueError('Saved S1 preservation needs a full caller review explanation')
        text = context['implementation']['text']
        head = text[:text.index('{')].strip() + ';'
        abi = prototype(head, mapped[0]['symbol'])
        parameters = abi['parameters']
        if source - 4 >= len(parameters) or parameters[source - 4] != {'type': 'uint32', 'name': row['native_parameter']}:
            raise ValueError('Saved S1 native carrier is not the original named scalar parameter')
        edits = [e for e in proposal.get('edits', []) if e.get('path') == mapped[0]['path'] and e.get('before', '').strip() == text.strip()]
        if len(edits) != 1:
            raise ValueError('Saved S1 needs the exact full corrected caller body')
        native_calls = re.findall(r'\b' + re.escape(bound['request']['symbol']) + r'\s*\(([^()]*)\)', mask_c(edits[0]['after']))
        references = re.findall(r'\b' + re.escape(bound['request']['symbol']) + r'\b', mask_c(edits[0]['after']))
        if len(native_calls) != 1 or len(native_calls) != len(references) or any(len(args.split(',')) != len(bound['abi']['parameters']) or args.split(',')[-1].strip() != row['native_parameter'] for args in native_calls):
            raise ValueError('Saved S1 native calls must forward the exact named parameter')


def validate_repairs(root, packet, proposal):
    bindings = packet.get('prototype_repair_bindings', [])
    audits = proposal.get('callee_abi_audits', [])
    updates = proposal.get('game_prototype_updates', [])
    if (not isinstance(audits, list) or not isinstance(updates, list) or len(audits) != len(bindings) or
            any(not isinstance(row, dict) for row in [*audits, *updates])):
        raise ValueError('Corrective ABI review must cover every bound callee exactly once')
    expected_updates, validated_audits = [], []
    seen = set()
    for audit in audits:
        key = audit.get('image'), audit.get('address')
        matches = [b for b in bindings if (b['binding']['image'], b['binding']['address']) == key]
        if len(matches) != 1 or key in seen:
            raise ValueError('Foreign or duplicate corrective callee ABI audit')
        seen.add(key)
        bound = matches[0]
        keys = set(FIELDS) | {'native_prototype', 'complete_mips_audit', 'pseudocode_complete',
                             'pseudocode_used_as_baseline', 'unresolved_semantics', 'argument_bindings', 'return_binding'}
        if set(audit) != keys or any(audit.get(k) != bound['binding'][k] for k in FIELDS):
            raise ValueError('Corrective ABI audit has stale original bindings or foreign fields')
        complete = bool(bound['context']['pseudocode'].get('path') and bound['context']['pseudocode'].get('complete'))
        if (audit['native_prototype'] != bound['request']['prototype'] or audit['complete_mips_audit'] is not True or
                audit['pseudocode_complete'] is not complete or audit['pseudocode_used_as_baseline'] is not complete or
                audit['unresolved_semantics'] != []):
            raise ValueError('Corrective callee ABI review is incomplete')
        words = words_from_listing(bound['context']['mips']['lines'])
        def check_evidence(items):
            if not isinstance(items, list) or not items:
                raise ValueError('Corrective ABI carrier needs original instruction evidence')
            for item in items:
                if (not isinstance(item, dict) or set(item) != {'pc', 'explanation'} or type(item['pc']) is not int or
                        item['pc'] not in words or not isinstance(item['explanation'], str) or not item['explanation'].strip()):
                    raise ValueError('Corrective ABI instruction evidence is foreign or empty')
        arguments = audit['argument_bindings']
        if not isinstance(arguments, list) or len(arguments) != len(bound['abi']['parameters']):
            raise ValueError('Corrective ABI review does not cover every argument')
        for index, (argument, parameter) in enumerate(zip(arguments, bound['abi']['parameters'])):
            saved = parameter['name'] == 'incoming_s1'
            expected_keys = {'index', 'original_register', 'native_type', 'representation', 'evidence'} | ({'saved_register_callers'} if saved else set())
            if (not isinstance(argument, dict) or set(argument) != expected_keys or
                    type(argument['index']) is not int or argument['index'] != index or argument['original_register'] != ('s1' if saved else f'a{index}') or
                    argument['native_type'] != parameter['type'] or
                    argument['representation'] != ('host_pointer' if '*' in parameter['type'] else 'guest_word')):
                raise ValueError('Corrective ABI argument carrier does not match the exact native prototype')
            check_evidence(argument['evidence'])
            if saved:
                if index != len(arguments) - 1 or index < 1 or parameter['type'] != 'uint32':
                    raise ValueError('Saved S1 must be one final uint32 incoming_s1 parameter')
                validate_saved_s1(bound, argument, proposal)
        returned = audit['return_binding']
        if (not isinstance(returned, dict) or set(returned) != {'native_type', 'original_register', 'evidence'} or
                returned['native_type'] != bound['abi']['return_type'] or returned['original_register'] != 'v0'):
            raise ValueError('Corrective ABI return carrier is invalid')
        check_evidence(returned['evidence'])
        for declaration in bound['declarations']:
            expected_updates.append({'path': declaration['path'], 'before': declaration['before'], 'after': bound['request']['prototype']})
        validated_audits.append(copy.deepcopy(audit))
    if sorted(updates, key=lambda r: (r.get('path', ''), r.get('before', ''))) != sorted(expected_updates, key=lambda r: (r['path'], r['before'])):
        raise ValueError('Corrective prototype updates must cover all and only bound existing declarations')
    # Reject ambiguous replacement anchors; common declaration duplication needs its own bounded contract
    for update in updates:
        if set(update) != {'path', 'before', 'after'} or not all(isinstance(update[k], str) for k in update):
            raise ValueError('Invalid corrective prototype source anchor')
        text = (root / update['path']).read_bytes().decode('utf-8-sig')
        if text.count(update['before']) != 1 or update['before'] == update['after']:
            raise ValueError('Corrective prototype source anchor is duplicate, unchanged or stale')
    return copy.deepcopy(updates), validated_audits

"""Preserve the real V8 incoming V0 ABI without an original-effect exception"""

import hashlib
import json
import re
from pathlib import Path

from coverage_game_frame_delegation import FIELDS, _review
from coverage_native_abi_frame import original_words
from source_index import functions, mask_c

OWNERS = {0x800441C8: 'sub_800441C8', 0x80044574: 'sub_80044574'}
MODULE_SHA = '0ba98fcb90100584fcfef9a7f0fb3145a819dbdad69e82e97b3fbfc5d0ca06a9'


def arguments(text):
    depth, start, result = 0, 0, []
    for index, char in enumerate(text):
        if char == '(':
            depth += 1
        elif char == ')':
            depth -= 1
        elif char == ',' and depth == 0:
            result.append(text[start:index].strip())
            start = index + 1
        if depth < 0 or char in '{};#"':
            raise ValueError('V0 carrier rejects non-expression arguments')
    if depth:
        raise ValueError('V0 carrier argument parentheses are incomplete')
    result.append(text[start:].strip())
    return result


def carrier_patch(patch):
    if set(patch) != {'kind', 'before', 'after'}:
        raise ValueError('Invalid V0 carrier patch')
    old, new, kind = patch['before'], patch['after'], patch['kind']
    if not isinstance(old, str) or not old or not isinstance(new, str) or '#' in old + new:
        raise ValueError('V0 carrier patch needs plain exact C')
    if kind == 'append_carrier_argument':
        pattern = r'(sub_800441C8|sub_80044574)\(([^;]*)\);([\s\S]*)'
        a, b = re.fullmatch(pattern, old), re.fullmatch(pattern, new)
        if not a or not b or a[1] != b[1] or a[3] != b[3]:
            raise ValueError('V0 carrier patch changes another statement')
        before, after = arguments(a[2]), arguments(b[2])
        if after[:-1] != before or len(before) != (1 if a[1] == 'sub_800441C8' else 2) or not re.fullmatch(r'[A-Za-z_]\w*|[0-9]+u', after[-1]):
            raise ValueError('V0 carrier patch changes architectural arguments')
        return {'symbol': a[1], 'carrier': after[-1], 'volume': before[-1]}
    if kind == 'sequence_nested_call':
        a = re.fullmatch(r'(sub_80044574)\(([^;]*)\);', old)
        b = re.fullmatch(r'([A-Za-z_]\w*)\s*=\s*(sub_800446DC\([^;]*\));\s*sub_80044574\(([^;]*)\);', new)
        if not a or not b:
            raise ValueError('V0 carrier sequence is not the exact volume call')
        before, after = arguments(a[2]), arguments(b[3])
        if len(before) != 2 or before[1] != b[2] or after != [before[0], b[1], b[1]]:
            raise ValueError('V0 carrier sequence changes calls or arguments')
        return {'symbol': a[1], 'carrier': b[1], 'volume': b[1]}
    if kind == 'capture_result':
        a = re.fullmatch(r'([A-Za-z_]\w*)\s*\+=\s*(sub_800446DC\([^;]*\));', old)
        b = re.fullmatch(r'([A-Za-z_]\w*)\s*=\s*(sub_800446DC\([^;]*\));\s*([A-Za-z_]\w*)\s*\+=\s*([A-Za-z_]\w*);', new)
        if not a or not b or a[1] != b[3] or a[2] != b[2] or b[1] != b[4] or b[1] == a[1]:
            raise ValueError('V0 carrier capture changes the original sum or call')
        return {'capture': b[1]}
    if kind == 'local_carrier_declaration':
        if not new.startswith(old) or not re.fullmatch(r'\s+uint32\s+([A-Za-z_]\w*);', new[len(old):]):
            raise ValueError('V0 carrier declaration permits one uninitialized scalar only')
        if not re.fullmatch(r'uint32\s+[^;{}#]+;', old):
            raise ValueError('V0 carrier declaration anchor is not a plain local')
        return {'declaration': re.fullmatch(r'\s+uint32\s+([A-Za-z_]\w*);', new[len(old):])[1]}
    raise ValueError('Unsupported V0 carrier patch kind')


def validate_v0_updates(root, packet, proposal):
    reference = proposal.get('game_v0_carrier_review')
    if not reference:
        return []
    root = Path(root)
    review = _review(root, reference)
    if (packet.get('contract') != 'xport-function-audit-packet-v1' or proposal.get('translations') or
            review.get('contract') != 'xport-complete-incoming-v0-abi-v1' or review.get('schema') != 1 or
            review.get('excluded_original_effects') != [] or review.get('accepted_proofs_added') != 0 or
            review.get('all_current_native_direct_calls_reviewed') is not True):
        raise ValueError('V0 carrier requires a strict original-effects-preserving mapped packet')
    packet_ref = review['packet']
    packet_path = (root / packet_ref['path']).resolve()
    if (not packet_path.is_relative_to((root / 'status').resolve()) or
            hashlib.sha256(packet_path.read_bytes()).hexdigest() != packet_ref['sha256'] or
            json.loads(packet_path.read_text(encoding='utf-8')) != packet or
            hashlib.sha256((root/'xport-project.json').read_bytes()).hexdigest() != review['project_sha256']):
        raise ValueError('V0 carrier packet or project binding changed')
    context = packet['source_context']
    contexts = [context, *context.get('audit_cluster', [])]
    required = {r['address']:r for r in context['audit_contract']['required_functions'] if r['image'] == 'SLUS_005.10'}
    if {r['address'] for r in review['owners']} != set(OWNERS) or len(review['owners']) != 2:
        raise ValueError('V0 carrier supports exactly the two reviewed V8 owners')
    for owner in review['owners']:
        address = owner['address']
        bound = required.get(address)
        views = [c for c in contexts if c['function']['address'] == address and c['function']['image'] == 'SLUS_005.10']
        if not bound or len(views) != 1 or any(owner[k] != bound[k] for k in FIELDS):
            raise ValueError('V0 carrier original owner is missing or stale')
        impl = views[0]['implementation']
        if (impl.get('complete') is not True or impl['symbol'] != OWNERS[address] or
                owner['function_before'] != impl['text'] or owner['source_file'] != impl['path'] or
                owner.get('complete_original_read') is not True or owner.get('zero_channel_return_preserved') is not True or
                owner.get('original_effects_excluded') != [] or
                owner.get('implicit_input') != {'register':'v0','native_parameter':'uint32 incoming_v0','representation':'guest_word'}):
            raise ValueError('V0 carrier owner is not the complete exact mapped body')
        edits = [e for e in proposal['edits'] if e['path'] == impl['path'] and e['before'] == impl['text'] and e['after'] == owner['function_after']]
        if len(edits) != 1:
            raise ValueError('V0 carrier owner requires its complete normal strict replacement')
        pattern = r'(void|uint32)\s+' + OWNERS[address] + r'\(([^()]*)\)\s*\{'
        before, after = re.match(pattern, impl['text']), re.match(pattern, owner['function_after'])
        if not before or not after or before[1] != 'void' or after[1] != 'uint32' or after[2] != before[2] + ', uint32 incoming_v0':
            raise ValueError('V0 carrier changes architectural owner parameters')
        if not re.search(r'if\s*\(channel == 0u\)\s*return incoming_v0;', owner['function_after']) or 'abort' in owner['function_after']:
            raise ValueError('V0 carrier must retain the actual zero-channel return without exception')
        words = original_words(views[0])
        if hashlib.sha256(b''.join(w.to_bytes(4,'little') for _, w in words)).hexdigest() != bound['function_sha256']:
            raise ValueError('V0 carrier complete original words changed')
    import sqlite3
    database = sqlite3.connect(root/'status/analysis.sqlite')
    database.row_factory = sqlite3.Row
    original = {}
    for row in review['original_callers']:
        bound = database.execute('select * from functions where image=? and address=?',(row['image'],row['address'])).fetchone()
        if (not bound or row['image'] != 'SLUS_005.10' or bound['sha256'] != row['function_sha256'] or bound['listing'] != row['listing'] or
                row.get('complete_original_read') is not True):
            raise ValueError('V0 carrier original caller binding changed')
        listing = (root / row['listing']).resolve()
        if not listing.is_relative_to((root/'orig').resolve()) or hashlib.sha256(listing.read_bytes()).hexdigest() != row['listing_sha256']:
            raise ValueError('V0 carrier original caller listing changed')
        from lockstep_audit import words_from_listing
        words = words_from_listing(listing.read_text(encoding='utf-8').splitlines())
        original[row['address']] = words
        if hashlib.sha256(b''.join(word.to_bytes(4,'little') for _,word in sorted(words.items()))).hexdigest() != row['function_sha256']:
            raise ValueError('V0 carrier original caller bytes changed')
    module = review['module_original']
    source = (root/module['source']).resolve()
    if module['image'] != 'Terrain/SkiResrt.dll' or not source.is_relative_to((root/'bin/DATA').resolve()) or module['sha256'] != MODULE_SHA or hashlib.sha256(source.read_bytes()).hexdigest() != MODULE_SHA or module.get('full_module_equivalence_claimed') is not False:
        raise ValueError('V0 carrier module original identity changed')
    raw = source.read_bytes()
    for site in module['raw_original_sites']:
        if int.from_bytes(raw[site['offset']:site['offset']+4],'little') != site['word']:
            raise ValueError('V0 carrier module original instruction changed')
    state = json.loads((root/'status/function-coverage/first/coverage.json').read_text(encoding='utf-8'))
    proved = {r['address'] for r in state['functions'] if r.get('audit_receipt')}
    extras, caller_sources, declared_sources = [], set(), set()
    for caller in review['caller_repairs']:
        relative = Path(caller['source_file'])
        path = (root/relative).resolve()
        if relative.is_absolute() or '..' in relative.parts or not path.is_relative_to((root/'src').resolve()) or relative.suffix != '.c':
            raise ValueError('V0 carrier caller source escapes project')
        text = path.read_text(encoding='utf-8-sig')
        bodies = [f for f in functions(text, relative.as_posix()) if f['name'] == caller['source_symbol']]
        if (len(bodies) != 1 or text[bodies[0]['start']:bodies[0]['end']] != caller['function_before'] or
                hashlib.sha256(caller['function_before'].encode()).hexdigest() != caller['function_before_sha256'] or
                bodies[0]['markers'] != [tuple(m) for m in caller['markers']] or
                any(address in proved for address,_ in bodies[0]['markers']) or
                caller.get('carrier_review_complete') is not True or
                caller.get('caller_implementation_equivalence_claimed') is not False or
                caller.get('unrelated_native_algorithm_retained_verbatim') is not True):
            raise ValueError('V0 carrier caller is stale, proved or outside retained exact scope')
        predicted, calls, captures, declarations = caller['function_before'], [], [], []
        for patch in caller['patches']:
            effect = carrier_patch(patch)
            if predicted.count(patch['before']) != 1:
                raise ValueError('V0 carrier caller patch is not unique')
            predicted = predicted.replace(patch['before'],patch['after'],1)
            if 'symbol' in effect:
                calls.append(effect)
            if 'capture' in effect:
                captures.append(effect['capture'])
            if 'declaration' in effect:
                declarations.append(effect['declaration'])
        if predicted != caller['function_after'] or captures != declarations:
            raise ValueError('V0 carrier caller changes unrelated algorithm or locals')
        seen_calls = []
        for site in caller['callsites']:
            symbol = OWNERS.get(site['target'])
            matches = [c for c in calls if c['symbol'] == symbol and c['carrier'] == site['native_carrier']]
            if len(matches) != 1 or not site.get('review'):
                raise ValueError('V0 carrier site lacks exact native argument evidence')
            if site['image'] == 'SLUS_005.10':
                words = original.get(site['caller'],{})
                pc = site['pc']
                if words.get(pc) != (3<<26) | ((site['target']>>2)&0x03FFFFFF) or site['definition_pc'] not in words:
                    raise ValueError('V0 carrier original JAL or definition changed')
                edge = database.execute('select 1 from edges where source_image=? and source_function=? and site=? and target=?',('SLUS_005.10',site['caller'],pc,site['target'])).fetchone()
                if not edge:
                    raise ValueError('V0 carrier lacks the original direct caller edge')
                if 'delay_pc' in site and site['delay_pc'] != pc + 4:
                    raise ValueError('V0 carrier original delay PC changed')
            elif site['image'] == module['image']:
                pc = site['pc_offset']
                word = int.from_bytes(raw[pc:pc+4],'little')
                delay = int.from_bytes(raw[pc+4:pc+8],'little')
                if word != (3<<26)|((site['target']>>2)&0x03FFFFFF) or site['delay_offset'] != pc+4:
                    raise ValueError('V0 carrier module JAL or delay changed')
                if delay == 0x02022821 and (matches[0]['carrier'] == matches[0]['volume'] or matches[0]['carrier'] not in captures):
                    raise ValueError('V0 carrier must preserve the second volume, separately from the sum')
                if pc == 0xAB8 and (int.from_bytes(raw[0xAB4:0xAB8],'little') != 0x240204B0 or site['native_carrier'] != '1200u'):
                    raise ValueError('V0 carrier changes the original timer word')
            else:
                raise ValueError('V0 carrier caller original image is foreign')
            seen_calls.append((symbol,site['native_carrier']))
        if sorted(seen_calls) != sorted((c['symbol'],c['carrier']) for c in calls):
            raise ValueError('V0 carrier does not cover every changed native call')
        extras.append({'path':relative.as_posix(),'before':caller['function_before'],'after':predicted})
        caller_sources.add((relative.as_posix(),caller['source_symbol']))
    expected = []
    for path in sorted((root/'src').rglob('*')):
        if not path.is_file() or path.suffix.lower() not in ('.c','.h'):
            continue
        relative = path.relative_to(root).as_posix()
        text = path.read_text(encoding='utf-8-sig')
        masked = mask_c(text)
        bodies = functions(text, relative)
        for symbol in OWNERS.values():
            for body in bodies:
                if body['name'] == symbol:
                    continue
                if re.search(r'\b'+symbol+r'\s*\(',masked[body['start']:body['end']]) and (relative,body['name']) not in caller_sources:
                    raise ValueError('V0 carrier has an unreviewed native caller')
            pattern = re.compile(r'void\s+'+symbol+r'\(([^(){};]*)\);')
            for match in pattern.finditer(masked):
                if any(f['start'] <= match.start() < f['end'] for f in bodies):
                    raise ValueError('V0 carrier rejects local declarations')
                before = text[match.start():match.end()]
                after = 'uint32 '+symbol+'('+match[1]+', uint32 incoming_v0);'
                if text.count(before) != 1:
                    raise ValueError('V0 carrier declaration is ambiguous')
                expected.append({'path':relative,'before':before,'after':after})
    if sorted(expected,key=lambda e:(e['path'],e['before'])) != sorted(review['prototype_updates'],key=lambda e:(e['path'],e['before'])) or len(expected) != 4:
        raise ValueError('V0 carrier must update all and only its four existing declarations')
    database.close()
    return extras + expected

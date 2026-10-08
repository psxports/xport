"""Plan fixed persistent-stack ABI repairs without editing sources or admitting proofs"""
from __future__ import annotations
import copy
import hashlib
from pathlib import Path
import re
from coverage_corrective_prototypes import prototype, phase2, lexical_tokens
from lockstep_audit import checklist, words_from_listing
from source_index import functions, mask_c
from lockstep_supervisor import source_snapshot

CARRIER = 'typedef struct sf_stack_state { uint32 sp; uint32 s[8]; uint32 fp; uint32 ra; } sf_stack_state;'
HASHES = ('function_sha256', 'mips_sha256', 'pseudocode_sha256', 'machine_audit_sha256')

def normalized(text):
    return re.sub(r'\s+', '', text)

def frame_layout(words):
    adjustments, stores = [], []
    for pc, word in sorted(words.items()):
        op, base, reg = word >> 26, (word >> 21) & 31, (word >> 16) & 31
        offset = word & 65535
        if offset & 32768:
            offset -= 65536
        if op == 9 and base == reg == 29:
            adjustments.append({'address': pc, 'delta': offset})
        if op == 43 and base == 29 and reg in (*range(16, 24), 30, 31):
            stores.append({'address': pc, 'register': reg, 'offset': offset})
    if not adjustments or adjustments[0]['delta'] >= 0:
        raise ValueError('Frame ABI requires an original explicit stack prologue')
    return {'frame_size': -adjustments[0]['delta'], 'adjustments': adjustments, 'saved_register_stores': stores}

def owner_context(view):
    implementation, original, pseudo = view['implementation'], view['mips'], view['pseudocode']
    if not implementation.get('complete') or not original.get('complete'):
        raise ValueError('Frame ABI needs complete mapped C and MIPS')
    if pseudo.get('path') and (pseudo.get('problem') or not pseudo.get('content_complete', pseudo.get('complete')) or pseudo.get('first_line') != 1 or len(pseudo.get('lines', [])) != pseudo.get('total_lines')):
        raise ValueError('Frame ABI needs all available pseudocode')
    words = words_from_listing(original['lines'])
    fn = view['function']
    raw = b''.join(word.to_bytes(4, 'little') for _, word in sorted(words.items()))
    if sorted(words) != list(range(fn['address'], fn['end'], 4)) or len(raw) != fn['bytes'] or hashlib.sha256(raw).hexdigest() != fn['sha256']:
        raise ValueError('Frame ABI original bytes do not bind the whole function')
    machine = checklist(original['lines'])
    return words, dict(function_sha256=fn['sha256'], mips_sha256=original['sha256'], pseudocode_sha256=pseudo.get('sha256'), machine_audit_sha256=machine['sha256'])

def changed_prototype(before, symbol):
    abi = prototype(before, symbol)
    if len(abi['parameters']) > 3:
        raise ValueError('Frame ABI supports up to three existing explicit parameters')
    params = [p['type'] + ' ' + p['name'] for p in abi['parameters']]
    params.append('sf_stack_state *stack')
    return abi['return_type'] + ' ' + symbol + '(' + ', '.join(params) + ');'

def build_bindings(root, packet, request, snapshot, *, shared_root, shared_snapshot):
    """Require trusted corrective contexts; return a read-only migration plan"""
    views = [packet['source_context'], *packet['source_context'].get('audit_cluster', [])]
    fresh = source_snapshot(root, (Path('src'),))
    if fresh['identity'] != snapshot['identity'] or packet.get('source_identity') != snapshot['identity'] or fresh['files'] != snapshot['files']:
        raise ValueError('Frame ABI source snapshot changed')
    shared_fresh = source_snapshot(shared_root, (Path('src'),))
    if shared_fresh != shared_snapshot or packet.get('identities', {}).get('shared_source_identity') != shared_snapshot['identity']:
        raise ValueError('Frame ABI shared source snapshot changed')
    if packet.get('contract') != 'xport-function-corrective-packet-v1' or not 1 <= len(views) <= 2 or len(packet.get('accepted_bindings', [])) != len(views):
        raise ValueError('Frame ABI preserves the one-or-two accepted-owner audit limit')
    if set(request) != {'schema', 'carrier_declaration', 'repairs'} or request['schema'] != 1 or request['carrier_declaration'] != CARRIER:
        raise ValueError('Frame ABI permits only its fixed data carrier, not helpers')
    repairs = request['repairs']
    if not isinstance(repairs, list) or not 1 <= len(repairs) <= len(views):
        raise ValueError('Frame ABI repair count exceeds the corrective owners')
    required = {(v['image'], v['function']['address']): v for v in views}
    if len(required) != len(views):
        raise ValueError('Frame ABI duplicate owner context')
    for identity, view in required.items():
        _, binding = owner_context(view)
        accepted = [row for row in packet['accepted_bindings'] if (row['image'], row['address']) == identity]
        if len(accepted) != 1 or any(accepted[0]['audit_binding'].get(key) != binding[key] for key in HASHES):
            raise ValueError('Frame ABI complete owner differs from its accepted proof')
    result, used = [], set()
    for repair in repairs:
        if set(repair) != {'image', 'address', 'before', 'after', 'original_binding', 'frame_layout'}:
            raise ValueError('Frame ABI repair fields differ from the fixed contract')
        identity = repair['image'], repair['address']
        if type(repair['address']) is not int or identity not in required or identity in used:
            raise ValueError('Frame ABI target is not a unique corrective owner')
        used.add(identity)
        view = required[identity]
        words, binding = owner_context(view)
        if repair['original_binding'] != binding or repair['frame_layout'] != frame_layout(words):
            raise ValueError('Frame ABI original hashes or stack provenance differ')
        accepted = [r for r in packet['accepted_bindings'] if (r['image'], r['address']) == identity]
        if len(accepted) != 1 or any(accepted[0]['audit_binding'].get(k) != binding[k] for k in HASHES):
            raise ValueError('Frame ABI target differs from its accepted proof')
        symbol = view['implementation']['symbol']
        for relative in shared_snapshot['files']:
            text = (shared_root / relative).read_text(encoding='utf-8-sig')
            logical, _ = phase2(text)
            if '##' in logical or '%:%:' in logical:
                raise ValueError('Frame ABI rejects shared token-pasting source')
            if re.search(r'\b' + re.escape(symbol) + r'\b', lexical_tokens(logical)):
                raise ValueError('Frame ABI rejects an unbound shared native reference')
        parsed = functions(view['implementation']['text'], view['implementation']['path'])
        if len(parsed) != 1 or parsed[0]['name'] != symbol:
            raise ValueError('Frame ABI requires one exact mapped body')
        current = view['implementation']['text'].split('{', 1)[0].strip() + ';'
        if normalized(current) != normalized(repair['before']) or normalized(changed_prototype(repair['before'], symbol)) != normalized(repair['after']):
            raise ValueError('Frame ABI may only append the fixed stack parameter')
        declarations, callers = [], []
        token = re.compile(r'\b' + re.escape(symbol) + r'\b')
        for relative in sorted(snapshot['files']):
            path = Path(relative)
            if path.is_absolute() or '..' in path.parts or not path.is_relative_to(Path('src')) or path.suffix not in ('.c', '.h'):
                raise ValueError('Frame ABI source inventory is not owned C/H')
            text = (root / path).read_text(encoding='utf-8-sig')
            logical, positions = phase2(text)
            tokens = lexical_tokens(logical)
            if '##' in logical or '%:%:' in logical or any(line.lstrip().startswith('#') and token.search(line) for line in tokens.splitlines()):
                raise ValueError('Frame ABI rejects directive aliases')
            for occurrence in token.finditer(tokens):
                if positions[occurrence.end() - 1] - positions[occurrence.start()] != len(symbol) - 1:
                    raise ValueError('Frame ABI rejects split identifiers')
            masked, covered = mask_c(text), []
            bodies = functions(text, relative)
            pattern = re.compile(r'(?:extern\s+)?(?:void|uint32|sint32)\s+' + re.escape(symbol) + r'\s*\([^{};#]*\)\s*;')
            for declaration in pattern.finditer(masked):
                if any(f['start'] <= declaration.start() < f['end'] for f in bodies):
                    raise ValueError('Frame ABI rejects local declarations')
                before = text[declaration.start():declaration.end()]
                if normalized(before.removeprefix('extern ')) != normalized(repair['before']):
                    raise ValueError('Frame ABI declaration is not the exact old prototype')
                declarations.append({'path': relative, 'before': before, 'after': repair['after']})
                covered.append(declaration.span())
            for body in bodies:
                occurrences = list(token.finditer(masked[body['start']:body['end']]))
                if not occurrences:
                    continue
                matches = [v for v in views if v['implementation']['path'] == relative and v['implementation']['symbol'] == body['name'] and v['implementation']['sha256'] == body['sha256']]
                if len(matches) != 1:
                    raise ValueError('Every native caller must be a complete corrective owner')
                caller = matches[0]
                for occurrence in occurrences:
                    at = body['start'] + occurrence.start()
                    if body['name'] == symbol and at < text.index('{', body['start']):
                        covered.append((at, at + len(symbol)))
                        continue
                    if not re.match(r'\s*\(', masked[at + len(symbol):body['end']]):
                        raise ValueError('Frame ABI rejects escaping function pointers')
                    caller_words, _ = owner_context(caller)
                    links = [{'site': pc, 'return_address': pc + 8} for pc, w in caller_words.items() if w >> 26 == 3 and (((pc + 4) & 0xf0000000) | ((w & 0x3ffffff) << 2)) == repair['address']]
                    if caller['image'] != repair['image'] or not links:
                        raise ValueError('Frame ABI caller has no exact original JAL edge')
                    callers.append({'image': caller['image'], 'address': caller['function']['address'], 'path': relative, 'symbol': body['name'], 'body_sha256': body['sha256'], 'original_links': links})
                    covered.append((at, at + len(symbol)))
            if any(not any(a <= occurrence.start() < z for a, z in covered) for occurrence in token.finditer(masked)):
                raise ValueError('Frame ABI has a foreign outside reference')
        if not declarations:
            raise ValueError('Frame ABI needs an existing exact declaration')
        result.append({'request': copy.deepcopy(repair), 'binding': binding, 'declarations': declarations, 'callers': callers})
    return {'carrier_declaration': CARRIER, 'repairs': result, 'source_identity': snapshot['identity'], 'shared_source_identity': shared_snapshot['identity'], 'admission': False, 'requires': 'Corrective whole-body audits, fixed carrier declaration transaction, accepted-proof rollback and source revalidation before apply'}

"""Validate a bounded byte selector in an accepted GP-preserving overlay."""
import hashlib
import json
import struct

from mips_function_fixture import MipsFunctionFixture
from overlay_gp import _bound, _digest, _owned


def prove_byte_selector(code, start, loader, gp):
    from overlay_gp_thread import _pair, _target

    if len(code) != 96 or start % 4 or not 0x80000000 <= start < start + len(code) <= 0x80200000:
        raise ValueError('Byte resource selector requires its complete 24-word suffix')
    if type(gp) is not int or not 0 < gp <= 0xffffffff:
        raise ValueError('Byte resource selector requires a nonzero inherited GP')
    words = struct.unpack('<24I', code)
    fixed = {1: 0x24020007, 6: 0x24030004, 8: 0x2442fffe, 9: 0x2c420002,
             10: 0x10400002, 14: 0, 15: 0x8fbf0028, 16: 0x8fb50024,
             17: 0x8fb40020, 18: 0x8fb3001c, 19: 0x8fb20018, 20: 0x8fb10014,
             21: 0x8fb00010, 22: 0x03e00008, 23: 0x27bd0030}
    if any(words[index] != value for index, value in fixed.items()):
        raise ValueError('Byte resource selector branch, delay or epilogue differs')
    written0 = _pair(words[0], words[3], 0x3c03, 0xac62)
    written1 = _pair(words[2], words[7], 0x3c04, 0xac83)
    selected_byte = _pair(words[4], words[5], 0x3c02, 0x9042)
    if (not all(0x80000000 <= address < 0x80200000 for address in (written0, written1, selected_byte)) or
            written0 % 4 or written1 % 4 or written0 > 0x801ffffc or written1 > 0x801ffffc or
            words[11] >> 16 != 0x2404 or words[12] >> 16 != 0x2404 or
            words[11] & 0x8000 or words[12] & 0x8000 or
            _target(start + 52, words[13]) != loader):
        raise ValueError('Byte resource selector materializer, index or loader differs')
    addresses = (written0, written1, selected_byte)
    stack = 0x801d0000
    if any(stack <= address < stack + 48 for address in addresses):
        raise ValueError('Byte resource selector aliases the fixture return frame')
    if (written0 <= selected_byte < written0 + 4 or written1 <= selected_byte < written1 + 4 or
            written0 == written1):
        raise ValueError('Byte resource selector state fields alias')
    selections = []
    for value in range(256):
        memory = bytearray(0x200000)
        memory[selected_byte & 0x1fffffff] = value
        stop = 0x801fff00
        struct.pack_into('<I', memory, (stack & 0x1fffffff) + 40, stop)
        registers = [0] * 32
        registers[28], registers[29], registers[31] = gp, stack, stop
        captured = []

        def capture(machine):
            captured.append((machine.registers[4], machine.registers[28]))
        machine = MipsFunctionFixture(code, start, memory, registers, calls={loader: capture})
        end, steps = machine.execute(start, limit=40)
        expected = words[12 if value in (2, 3) else 11] & 0xffff
        if (end != stop or steps > 25 or captured != [(expected, gp)] or machine.registers[28] != gp or
                struct.unpack_from('<I', machine.memory, written0 & 0x1fffffff)[0] != 7 or
                struct.unpack_from('<I', machine.memory, written1 & 0x1fffffff)[0] != 4):
            raise ValueError('Original byte selector fixture diverges')
        selections.append(expected)
    return {'indices': sorted(set(selections)), 'cases': 256, 'gp': gp,
            'suffix_sha256': hashlib.sha256(code).hexdigest(),
            'selections_sha256': hashlib.sha256(json.dumps(selections).encode()).hexdigest()}


def validate_selectors(root, profile, evidence, review, loader):
    import startup

    indices = []
    proofs = []
    for selector in review.get('overlay_resource_selectors', []):
        path = _bound(root, selector, 'profile', 'tools/ida')
        caller = json.loads(path.read_text(encoding='utf-8-sig'))
        inherited = caller.get('gp_evidence', {})
        if (caller.get('image') == profile.get('image') or caller.get('gp') != profile['gp'] or
                inherited.get('source') != 'inherited_thread_resource_loader' or
                inherited.get('parent_config') != evidence['parent_config']):
            raise ValueError('Resource selector requires a distinct accepted overlay with the same direct GP parent')
        previous = _bound(root, inherited, 'loader_review', 'status')
        if json.loads(previous.read_text(encoding='utf-8-sig')).get('overlay_resource_selectors'):
            raise ValueError('Recursive overlay resource selector inheritance is unsupported')
        startup.validate_gp_config(root, caller)
        receipt_path = _bound(root, selector, 'acceptance_receipt', 'status/ida/accepted')
        receipt = json.loads(receipt_path.read_text(encoding='utf-8-sig'))
        accepted = [row for row in receipt.get('images', []) if row.get('image') == caller.get('image')]
        if receipt.get('version') != 1 or len(accepted) != 1:
            raise ValueError('Missing accepted overlay selector image')
        accepted = accepted[0]
        destination = (root / receipt['destination']).resolve()
        if not destination.is_relative_to(root / 'orig') or not destination.is_dir():
            raise ValueError('Foreign or missing accepted overlay export directory')
        image_root = destination / caller['image']
        manifest = startup.verify_ida_image(root, image_root.parent, path.parent, caller['image'])
        if (manifest['files'] != accepted.get('files') or manifest['config_sha256'] != accepted.get('config_sha256') or
                _owned(root, accepted['config']) != path):
            raise ValueError('Overlay selector acceptance is stale')
        functions = json.loads((image_root / 'functions.json').read_text(encoding='utf-8-sig'))
        owners = [row for row in functions if row['address'] == selector['function']['address']]
        if len(owners) != 1 or any(selector['function'].get(key) != owners[0][key] for key in ('sha256', 'chunks', 'size')):
            raise ValueError('Resource selector owner is not a complete accepted original')
        loads = caller.get('loads', [])
        if len(loads) != 1 or loads[0].get('offset', 0) != 0:
            raise ValueError('Overlay selector requires one offset-zero original load')
        raw = _owned(root, loads[0]['path']).read_bytes()
        base = loads[0]['base']
        body = b''.join(raw[a-base:z-base] for a, z in owners[0]['chunks'])
        if hashlib.sha256(body).hexdigest() != owners[0]['sha256']:
            raise ValueError('Overlay selector owner differs from original bytes')
        start = selector['start']
        if (type(start) is not int or not any(a <= start < start + 96 <= z for a, z in owners[0]['chunks']) or
                selector.get('end') != start + 96):
            raise ValueError('Overlay selector suffix escapes its accepted owner')
        proof = prove_byte_selector(raw[start-base:start-base+96], start, loader, profile['gp'])
        index = selector['index']
        if type(index) is not int or index not in proof['indices']:
            raise ValueError('Requested resource is not selected by the accepted original')
        indices.append(index)
        proofs.append(dict(image=caller['image'], function=owners[0]['address'], start=start,
                           selected_index=index, profile_sha256=_digest(path), **proof))
    return indices, proofs

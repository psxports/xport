"""Read and compare bounded PSX ordering-table chains at GPU submission"""

import hashlib
import struct


class OtError(ValueError):
    pass


def capture_ot(read, root, *, max_nodes=65536, max_words=262144):
    address = root & 0xFFFFFF
    nodes, seen, words, read_bytes = [], set(), 0, 0
    while address != 0xFFFFFF:
        if address & 3 or address > 0x200000 - 4:
            raise OtError(f'Invalid OT link 0x{address:06X}')
        if address in seen:
            raise OtError(f'OT cycle at 0x{address:06X}')
        if len(nodes) >= max_nodes:
            raise OtError('OT node budget exceeded')
        seen.add(address)
        header_bytes = read(address, 4)
        if len(header_bytes) != 4:
            raise OtError('Truncated OT header')
        header = struct.unpack('<I', header_bytes)[0]
        count = header >> 24
        if address + 4 + count * 4 > 0x200000:
            raise OtError('OT packet exceeds DRAM')
        words += count
        if words > max_words:
            raise OtError('OT payload budget exceeded')
        payload = read(address + 4, count * 4) if count else b''
        if len(payload) != count * 4:
            raise OtError('Truncated OT payload')
        read_bytes += 4 + len(payload)
        nodes.append({'address': address, 'header': header,
                      'words': list(struct.unpack(f'<{count}I', payload))})
        address = header & 0xFFFFFF
    canonical = b''.join(struct.pack('<I', len(node['words'])) +
                         struct.pack(f'<{len(node["words"])}I', *node['words'])
                         for node in nodes)
    return {'root': root, 'nodes': nodes, 'payload_words': words,
            'read_bytes': read_bytes, 'sha256': hashlib.sha256(canonical).hexdigest()}


def compare_ot(original, native):
    for index, (left, right) in enumerate(zip(original['nodes'], native['nodes'])):
        if left['words'] != right['words']:
            for word, (expected, actual) in enumerate(zip(left['words'], right['words'])):
                if expected != actual:
                    return {'status': 'OT_DIFFERENCE', 'node': index, 'word': word,
                            'original': f'{expected:08x}', 'native': f'{actual:08x}'}
            return {'status': 'OT_DIFFERENCE', 'node': index, 'kind': 'packet_length',
                    'original': len(left['words']), 'native': len(right['words'])}
    if len(original['nodes']) != len(native['nodes']):
        return {'status': 'OT_DIFFERENCE', 'kind': 'chain_length',
                'original': len(original['nodes']), 'native': len(native['nodes'])}
    return {'status': 'OT_SUBMISSION_MATCH', 'nodes': len(original['nodes']),
            'payload_words': original['payload_words'], 'sha256': original['sha256']}


def compare_draw_ot(original, native):
    left_root, right_root = original['root'] & 0xFFFFFF, native['root'] & 0xFFFFFF
    if left_root != right_root:
        return {'status': 'OT_DIFFERENCE', 'kind': 'DrawOTag_pointer',
                'original': f'{left_root:06x}', 'native': f'{right_root:06x}'}
    for index, (left, right) in enumerate(zip(original['nodes'], native['nodes'])):
        if left['address'] != right['address']:
            return {'status': 'OT_DIFFERENCE', 'kind': 'node_pointer', 'node': index,
                    'original': f'{left["address"]:06x}', 'native': f'{right["address"]:06x}'}
        if left['header'] != right['header']:
            return {'status': 'OT_DIFFERENCE', 'kind': 'header', 'node': index,
                    'write_address': left['address'],
                    'original': f'{left["header"]:08x}', 'native': f'{right["header"]:08x}'}
        comparison = compare_ot({'nodes': [left], 'payload_words': len(left['words']), 'sha256': ''},
                                {'nodes': [right], 'payload_words': len(right['words']), 'sha256': ''})
        if comparison['status'] != 'OT_SUBMISSION_MATCH':
            comparison['node'] = index
            if 'word' in comparison:
                comparison['write_address'] = left['address'] + 4 + 4 * comparison['word']
            return comparison
    return compare_ot(original, native)

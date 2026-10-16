"""Compare a bounded incoming OT chain at one paired DrawOTag entry"""
import hashlib
import mmap
import struct

from lockstep_contract import mapping_name


def compare_ot(remote, run_id, address, *, view=None):
    if view is None:
        with mmap.mmap(-1, 0x200000, tagname=mapping_name(run_id, 'dram'),
                       access=mmap.ACCESS_READ) as mapping:
            return compare_ot(remote, run_id, address, view=mapping)
    link = address & 0xffffff
    seen, nodes, total = set(), [], 0
    while link != 0xffffff:
        if link in seen or link & 3 or link > 0x1ffffc or len(seen) >= 64:
            raise ValueError('OT entry chain is invalid or exceeds 64 nodes')
        if total + 4 > 4096:
            raise ValueError('OT entry chain exceeds 4096 bytes')
        seen.add(link)
        original = remote.read_memory(link, 4)
        native = view[link:link + 4]
        total += 4
        if original != native:
            return dict(status='difference', address=f'0x{0x80000000 | link:08X}',
                        original=original.hex(), native=native.hex(), nodes=nodes, bytes=total)
        tag = struct.unpack('<I', original)[0]
        size = (tag >> 24) * 4
        if size > 256 or link + 4 + size > 0x200000 or total + size > 4096:
            raise ValueError('OT entry packet is invalid or exceeds bounded input budget')
        if size:
            original += remote.read_memory(link + 4, size)
            native += view[link + 4:link + 4 + size]
            total += size
        if original != native:
            first = next(i for i, (a, b) in enumerate(zip(original, native)) if a != b)
            return dict(status='difference', address=f'0x{0x80000000 | (link + first):08X}',
                        packet=f'0x{0x80000000 | link:08X}', original=original.hex(),
                        native=native.hex(), nodes=nodes, bytes=total)
        nodes.append(dict(address=f'0x{0x80000000 | link:08X}', tag=f'0x{tag:08X}',
                          sha256=hashlib.sha256(original).hexdigest()))
        link = tag & 0xffffff
    return dict(status='match', nodes=nodes, bytes=total)

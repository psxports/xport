"""Conservative instruction-trace equivalence with explicit relocation/dependency checks."""
from collections import Counter, defaultdict
import struct


TEMPS = {1, *range(8, 16), 24, 25}
MEM = {32: 1, 33: 2, 35: 4, 36: 1, 37: 2, 40: 1, 41: 2, 43: 4, 50: 4, 58: 4}


def signed16(word):
    return (word & 65535)-(65536 if word & 32768 else 0)


def ram(address):
    return 0x80000000 <= address < 0x80200000


def decode(word):
    op, rs, rt, rd, fn = word >> 26, word >> 21 & 31, word >> 16 & 31, word >> 11 & 31, word & 63
    if op == 0:
        if fn in (0, 2, 3):
            return (rt,), (rd,), (word >> 6 & 31, fn), False
        if fn in (4, 6, 7, 32, 33, 34, 35, 36, 37, 38, 39, 42, 43):
            return (rs, rt), (rd,), (word >> 6 & 31, fn), False
        if fn in (24, 25, 26, 27):
            return (rs, rt), (), (rd, word >> 6 & 31, fn), False
        if fn in (16, 18):
            return (), (rd,), (rs, rt, word >> 6 & 31, fn), False
        if fn in (17, 19):
            return (rs,), (), (rt, rd, word >> 6 & 31, fn), False
        if fn == 8 and rs == 31:
            return (rs,), (), (rt, rd, word >> 6 & 31, fn), False
        return None
    if op in (2, 3):
        return (), (), (), False
    if op in (4, 5):
        return (rs, rt), (), (), False
    if op in (6, 7) and rt == 0 or op == 1 and rt in (0, 1):
        return (rs,), (), (rt,), False
    if op in (8, 9, 10, 11, 12, 13, 14):
        return (rs,), (rt,), (), False
    if op == 15 and rs == 0:
        return (), (rt,), (), False
    if op in MEM:
        if op in (40, 41, 43):
            return (rs, rt), (), (), False
        if op in (50, 58):
            return (rs,), (), (rt,), False
        return (rs,), (rt,), (), True
    if op == 18:
        if rs in (0, 2):
            return (), (rt,), (rs, word & 65535), True
        if rs in (4, 6):
            return (rt,), (), (rs, word & 65535), False
        if rs >= 16:
            return (), (), (word & 0x3ffffff,), False
    return None


class StrictMips:
    def __init__(self, pal, ntsc, pairs, data_map):
        self.a, self.b, self.pairs, self.data_map = pal, ntsc, pairs, data_map

    def data(self, a, b, width):
        if self.data_map.get(a) != b:
            return 'unknown', 'Data target has no established one-to-one mapping'
        left, right = self.a.bytes(a, width), self.b.bytes(b, width)
        if left is None or right is None:
            return 'unknown', 'Initialized data span unavailable (including BSS)'
        if width == 4 and (ram(struct.unpack('<I', left)[0]) or ram(struct.unpack('<I', right)[0])):
            return 'unknown', 'Pointer-like data requires typed target/extent proof'
        if left != right:
            return 'difference_detected', 'Referenced initialized data bytes differ'
        return 'local_equivalent', ''

    def pair(self, a, b):
        af, bf = self.a.features[a], self.b.features[b]
        left, right = af['words'], bf['words']
        deps, data = set(), []

        def stop(status, reason, index=None):
            row = dict(status=status, reason=reason, dependencies=[hex(x) for x in sorted(deps)], data_spans=data)
            if index is not None:
                row.update(pal_site=hex(af['addresses'][index]), ntsc_site=hex(bf['addresses'][index]),
                           pal_word=hex(left[index]), ntsc_word=hex(right[index]))
            return row

        if len(left) != len(right):
            return stop('unknown', 'Instruction counts differ; reordered/inserted blocks are outside this proof model')
        if not left:
            return stop('unknown', 'Empty instruction listing')
        local_a = {addr: i for i, addr in enumerate(af['addresses'])}
        local_b = {addr: i for i, addr in enumerate(bf['addresses'])}
        # This proof preserves physical instruction order and therefore delay-slot order
        complex_flow = any(w >> 26 in (1, 2, 3, 4, 5, 6, 7) for w in left+right)
        ra_slots = {signed16(w) for w in left if w >> 26 == 43 and (w >> 21 & 31) == 29 and (w >> 16 & 31) == 31}
        frame = None
        if ra_slots:
            sp_writes = [(i, w) for i, w in enumerate(left) if decode(w) and 29 in decode(w)[1]]
            if (len(sp_writes) != 2 or any(w >> 16 != 0x27bd for _, w in sp_writes)
                    or signed16(sp_writes[0][1]) >= 0 or signed16(sp_writes[0][1])+signed16(sp_writes[1][1]) != 0):
                return stop('unknown', 'Private return-address frame allocation/restoration is not established')
            frame = (sp_writes[0][0], sp_writes[1][0], -signed16(sp_writes[0][1]))
            if any(slot < 0 or slot+4 > frame[2] for slot in ra_slots):
                return stop('unknown', 'Saved return address is outside the private frame')
        mapping, reverse, defined, absolute = {}, {}, set(), {}
        pending = None
        immediates = {}
        for i, (w, v) in enumerate(zip(left, right)):
            op, other = w >> 26, v >> 26
            old_pending, pending = pending, None
            da, db = decode(w), decode(v)
            if da is None or db is None:
                return stop('unknown', 'Unsupported instruction or indirect control flow', i)
            if op != other or da[2] != db[2] or len(da[0]) != len(db[0]) or len(da[1]) != len(db[1]):
                return stop('difference_detected', 'Instruction operation/width/signedness differs', i)
            for read, registers_a, registers_b in ((True, da[0], db[0]), (False, da[1], db[1])):
                for ra, rb in zip(registers_a, registers_b):
                    if ra != rb and (complex_flow or ra not in TEMPS or rb not in TEMPS):
                        return stop('unknown', 'Register permutation is not supported across ABI registers or control flow', i)
                    if mapping.get(ra, rb) != rb or reverse.get(rb, ra) != ra:
                        return stop('difference_detected', 'Register def/use correspondence is inconsistent', i)
                    if read and ra != rb and ra not in defined:
                        return stop('unknown', 'Renamed temporary is read before a proven definition', i)
                    mapping[ra], reverse[rb] = rb, ra
            if 28 in da[1] or 28 in db[1]:
                return stop('unknown', 'Function changes GP; fixed-GP relocation proof is inapplicable', i)
            rs, rt = w >> 21 & 31, w >> 16 & 31
            if 28 in da[0] and not (op in MEM and rs == 28 and not (op in (40, 41, 43) and rt == 28)):
                return stop('unknown', 'GP value is observed outside a bounded global access', i)
            if i and (left[i-1] >> 26 in (1, 2, 3, 4, 5, 6, 7) or left[i-1] == 0x03e00008):
                if op in (1, 2, 3, 4, 5, 6, 7) or w == 0x03e00008:
                    return stop('unknown', 'Control transfer in a delay slot is outside the supported model', i)
            if frame and op in MEM and rs == 29 and rt == 31 and not frame[0] < i < frame[1]:
                return stop('unknown', 'Return-address access occurs outside the allocated frame', i)
            if 31 in da[0] and not (w == 0x03e00008 or op == 43 and rs == 29 and rt == 31):
                return stop('unknown', 'Return address is observed outside a private stack save/return', i)
            if 31 in da[1] and not (op == 35 and rs == 29 and signed16(w) in ra_slots):
                return stop('unknown', 'Return address is explicitly replaced outside a stack restore', i)
            if op in MEM and rs == 29 and signed16(w) in ra_slots and rt != 31:
                return stop('unknown', 'Saved return address slot is observed as ordinary data', i)
            if ra_slots and rs == 29 and rt != 29 and op not in MEM:
                return stop('unknown', 'Private stack address escapes a frame containing a saved return address', i)
            abs_access = op in MEM and rs in absolute
            if any(reg in absolute for reg in da[0] if not (abs_access and reg == rs)):
                return stop('unknown', 'Absolute pointer escapes a bounded memory access', i)
            if abs_access and op in (40, 41, 43) and rt in absolute:
                return stop('unknown', 'Absolute pointer is stored as a value', i)
            if op == 3 and absolute:
                return stop('unknown', 'Absolute pointer remains live across a call', i)
            if abs_access:
                hi_a, hi_b = absolute[rs]
                ta, tb = hi_a+signed16(w), hi_b+signed16(v)
                status, reason = self.data(ta, tb, MEM[op])
                if status != 'local_equivalent':
                    return stop(status, reason, i)
                data.append(dict(pal=hex(ta), ntsc=hex(tb), size=MEM[op]))
            if op in (2, 3):
                ta = ((af['addresses'][i]+4) & 0xf0000000) | ((w & 0x3ffffff) << 2)
                tb = ((bf['addresses'][i]+4) & 0xf0000000) | ((v & 0x3ffffff) << 2)
                if ta in local_a or tb in local_b:
                    if local_a.get(ta) != local_b.get(tb):
                        return stop('difference_detected', 'Local jump targets different corresponding instructions', i)
                elif self.pairs.get(ta) == tb:
                    deps.add(ta)
                else:
                    return stop('unknown', 'External call/tail target correspondence is unresolved', i)
            elif op in (1, 4, 5, 6, 7):
                ta = af['addresses'][i]+4+4*signed16(w)
                tb = bf['addresses'][i]+4+4*signed16(v)
                if ta not in local_a or tb not in local_b:
                    return stop('unknown', 'Conditional branch leaves the function listing', i)
                if local_a[ta] != local_b[tb]:
                    return stop('difference_detected', 'Conditional branch topology differs', i)
            elif op == 15 and (ram((w & 65535) << 16) or ram((v & 65535) << 16)):
                if rt == 0:
                    return stop('unknown', 'RAM-looking LUI targets the zero register', i)
                if complex_flow:
                    return stop('unknown', 'Absolute-pointer provenance across control flow is outside this proof model', i)
                if i+1 >= len(left):
                    return stop('unknown', 'Unbounded RAM address construction', i)
                nw, nv = left[i+1], right[i+1]
                if (nw >> 26 not in MEM or nv >> 26 != nw >> 26 or
                        (nw >> 21 & 31) != (w >> 16 & 31) or (nv >> 21 & 31) != (v >> 16 & 31)):
                    return stop('unknown', 'Address-taking/indexed access needs data extent and alias proof', i)
                immediates[i+1] = True
            elif op in MEM and (w >> 21 & 31) == 28 and (v >> 21 & 31) == 28:
                ta, tb = self.a.gp+signed16(w), self.b.gp+signed16(v)
                status, reason = self.data(ta, tb, MEM[op])
                if status != 'local_equivalent':
                    return stop(status, reason, i)
                data.append(dict(pal=hex(ta), ntsc=hex(tb), size=MEM[op]))
            elif op in (8, 9, 13) and ((w >> 21 & 31) == 28 or (v >> 21 & 31) == 28):
                return stop('unknown', 'GP-relative address escapes a bounded access')
            elif op != 0 and op != 18 and i not in immediates and not abs_access and (w & 65535) != (v & 65535):
                return stop('difference_detected', 'Literal constant, field offset or instruction bits differ', i)
            if old_pending is not None:
                defined.add(old_pending)
                absolute.pop(old_pending, None)
            if da[3]:
                pending = da[1][0] if da[1] else None
            else:
                defined.update(da[1])
                for reg in da[1]:
                    absolute.pop(reg, None)
            if op == 15 and i+1 in immediates:
                absolute[rt] = ((w & 65535) << 16, (v & 65535) << 16)
            if i+1 < len(left) and (af['addresses'][i+1] != af['addresses'][i]+4 or bf['addresses'][i+1] != bf['addresses'][i]+4):
                if i == 0 or not (left[i-1] >> 26 == 2 or left[i-1] == 0x03e00008):
                    return stop('unknown', 'A physical chunk gap has an unproven fallthrough', i)
        if left[-2:] != [0x03e00008, 0] and left[-1] >> 26 not in (2,):
            # Non-NOP return delay slots are supported when the return itself is present
            if len(left) < 2 or left[-2] != 0x03e00008:
                return stop('unknown', 'Listing has no supported terminal return/tail')
        return stop('local_equivalent', 'All physical operations, ABI registers, branch targets and bounded data spans agree')


def verify(pal, ntsc, report):
    pairs = {int(row['pal'], 16): int(row['ntsc'], 16) for row in report['matches']}
    trusted = set(pairs)
    occupied = set(pairs.values())
    proposals = []
    for row in report['pending']:
        candidates = row['candidates']
        if candidates:
            best = candidates[0]
            gap = best['similarity_pct']-(candidates[1]['similarity_pct'] if len(candidates) > 1 else 0)
            if best['similarity_pct'] >= 80 and gap >= 5:
                proposals.append((int(row['pal'], 16), int(best['ntsc'], 16)))
    counts = Counter(b for _, b in proposals)
    for a, b in proposals:
        if b not in occupied and counts[b] == 1:
            pairs[a] = b
            occupied.add(b)
    data_map = {int(row['pal'], 16): int(row['ntsc'], 16) for row in report['data']}
    checker = StrictMips(pal, ntsc, pairs, data_map)
    checks = {a: checker.pair(a, b) for a, b in pairs.items()}
    byte_map, reverse_bytes = defaultdict(lambda: defaultdict(set)), defaultdict(lambda: defaultdict(set))
    conflicts = set()
    for a, row in checks.items():
        if row['status'] != 'local_equivalent':
            continue
        for span in row['data_spans']:
            for offset in range(span['size']):
                src, dst = int(span['pal'], 16)+offset, int(span['ntsc'], 16)+offset
                byte_map[src][dst].add(a)
                reverse_bytes[dst][src].add(a)
    for targets in list(byte_map.values())+list(reverse_bytes.values()):
        if len(targets) > 1:
            for owners in targets.values():
                conflicts.update(owners)
    for a in conflicts:
        checks[a].update(status='unknown', reason='Overlapping data spans contradict a common byte-level memory mapping')
    survivors = {a for a, row in checks.items() if row['status'] == 'local_equivalent'}
    # Greatest fixed point retains mutually recursive groups only when every body passes
    while True:
        remaining = {a for a in survivors if all(int(dep, 16) in survivors for dep in checks[a]['dependencies'])}
        if remaining == survivors:
            break
        survivors = remaining
    for collection in ('matches', 'pending'):
        for row in report[collection]:
            a = int(row['pal'], 16)
            proof = checks.get(a, dict(status='unknown', reason='No unambiguous proposed correspondence', dependencies=[], data_spans=[])).copy()
            if a in survivors:
                proof['status'] = 'equivalent'
                proof['reason'] = 'Strict local proof and complete call-dependency closure passed'
            elif proof['status'] == 'local_equivalent':
                proof.update(status='unknown', reason='A called function has no completed strict proof')
            elif a not in trusted and proof['status'] == 'difference_detected':
                proof.update(status='unknown', reason='Candidate bodies differ; version correspondence is not established', pair_difference=proof['reason'])
            if a in pairs:
                proof['ntsc'] = hex(pairs[a])
            row['verification'] = proof
    report['strict_summary'] = dict(Counter(row['verification']['status'] for key in ('matches', 'pending') for row in report[key]))
    report['strict_contract'] = dict(model='Aligned MIPS instruction traces with ABI-fixed registers and straight-line defined temporary bijections',
        dependencies='Greatest fixed point including recursive groups; any unresolved external call blocks exclusion',
        scope='Same ABI inputs, values of read-before-write non-ABI registers, mapped memory state, GTE state and external inputs; initialized data spans checked',
        exclusions='No SMT, block/instruction reordering, indirect calls, data pointer extents, partial loads, mutable BSS or CD asset equivalence',
        assumptions='Input pointer/memory correspondence preserves aliases and field layouts; private ABI stack frames do not alias caller objects; this is function-code equivalence under mapped input state, not whole-game behavior')

"""Extract bounded MIPS data-flow fingerprints from physical listing words, without decompilation."""
from collections import Counter


def signed16(word):
    return (word & 65535)-(65536 if word & 32768 else 0)


def bounded(value, depth=2):
    if not isinstance(value, tuple):
        return value
    if depth == 0:
        return (value[0],)
    return tuple(bounded(item, depth-1) for item in value)


def add(value, offset):
    if not offset:
        return value
    if value[0] == 'const':
        return ('const', (value[1]+offset) & 0xffffffff)
    if value[0] == 'addr':
        return ('addr', value[1], value[2]+offset)
    return ('addr', bounded(value), offset)


def location(value, offset):
    if value[0] == 'addr':
        return value[1], value[2]+offset
    return value, offset


def dice(left, right):
    total = sum(left.values())+sum(right.values())
    return 2*sum(min(count, right.get(key, 0)) for key, count in left.items())/total if total else 0


def fingerprint(feature, call_ids):
    regs = [('unknown',)]*32
    regs[0] = ('const', 0)
    for n in range(4):
        regs[4+n] = ('arg', n)
    regs[28], regs[29], regs[31] = ('global',), ('stack',), ('return',)
    stack, events, memory, calls, branches = {}, [], [], [], []
    pending_load, pending_call = None, None
    words = feature['words']
    for i, word in enumerate(words):
        old_load, old_call = pending_load, pending_call
        pending_load, pending_call = None, None
        op, rs, rt, rd, fn = word >> 26, word >> 21 & 31, word >> 16 & 31, word >> 11 & 31, word & 63
        imm, shift = signed16(word), word >> 6 & 31
        operand = feature['operands'].get(i)
        dst, value = None, None
        if word == 0:
            pass
        elif op == 0:
            if fn in (0x21, 0x25) and (rs == 0 or rt == 0):
                dst, value = rd, regs[rs or rt]
            elif fn in (0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2a, 0x2b):
                kind = {0x20: 'add', 0x21: 'add', 0x22: 'sub', 0x23: 'sub', 0x24: 'and',
                        0x25: 'or', 0x26: 'xor', 0x27: 'nor', 0x2a: 'slt', 0x2b: 'sltu'}[fn]
                dst, value = rd, (kind, bounded(regs[rs]), bounded(regs[rt]))
                events.append((kind, bounded(regs[rs]), bounded(regs[rt])))
            elif fn in (0, 2, 3, 4, 6, 7):
                kind = {0: 'sll', 2: 'srl', 3: 'sra', 4: 'sllv', 6: 'srlv', 7: 'srav'}[fn]
                dst, value = rd, (kind, bounded(regs[rt]), bounded(regs[rs]) if fn >= 4 else shift)
                events.append(value)
            elif fn in (0x18, 0x19, 0x1a, 0x1b):
                events.append(('muldiv', fn, bounded(regs[rs]), bounded(regs[rt])))
            elif fn in (0x10, 0x12):
                dst, value = rd, ('hilo', fn)
                events.append(value)
            elif fn in (8, 9):
                events.append(('return' if rs == 31 else 'indirect',))
                if fn == 9:
                    pending_call = 'indirect'
            else:
                events.append(('special', fn))
        elif op in (8, 9, 13) and operand:
            dst, value = rt, ('global',)
        elif op in (8, 9):
            dst, value = rt, add(regs[rs], imm)
            if rs != 29 and rt != 29:
                events.append(('addi', bounded(regs[rs]), imm))
        elif op == 13 and rs == 0:
            dst, value = rt, ('const', word & 65535)
            events.append(('addi', ('const', 0), word & 65535))
        elif op == 15:
            dst, value = rt, ('global',) if feature['tokens'][i] != hex(word) else ('const', (word & 65535) << 16)
        elif op in (10, 11, 12, 13, 14):
            dst, value = rt, ('immediate', op, bounded(regs[rs]), imm if op in (10, 11) else word & 65535)
            events.append(value)
        elif op in (32, 33, 34, 35, 36, 37, 38, 40, 41, 42, 43, 46, 48, 50, 56, 58):
            base, offset = (('global',), 0) if operand else location(regs[rs], imm)
            load = op in (32, 33, 34, 35, 36, 37, 38, 48, 50)
            kind = ('load' if load else 'store', op)
            if base == ('stack',):
                if load:
                    value = stack.get(offset, ('local',))
                else:
                    stack[offset] = regs[rt]
            else:
                entry = (*kind, bounded(base), offset)
                memory.append(entry)
                value = ('load', op, bounded(base), offset)
                events.append(entry if load else (*entry, bounded(regs[rt])))
            if load and op < 48:
                pending_load = (rt, value)
        elif op in (2, 3):
            if op == 3:
                target = operand[1] if operand else None
                pending_call = call_ids.get(target, 'unknown_call')
            else:
                events.append(('jump', 'local' if not operand else 'tail'))
        elif op in (1, 4, 5, 6, 7):
            entry = ('branch', op, rt if op == 1 else -1, imm < 0)
            branches.append(entry)
            events.append((*entry, bounded(regs[rs]), bounded(regs[rt]) if op in (4, 5) else ()))
        elif op in (16, 17, 18):
            if rs in (0, 2):
                pending_load = (rt, ('cop_read', op, rs, rd))
            elif rs in (4, 6):
                events.append(('cop_write', op, rs, rd, bounded(regs[rt])))
            else:
                events.append(('cop_op', op, word & 0x1ffffff))
        else:
            events.append(('opcode', op))
        if old_load and old_load[0]:
            regs[old_load[0]] = old_load[1]
        if dst:
            regs[dst] = bounded(value, 3)
        if old_call:
            calls.append(old_call)
            events.append(('call', old_call, *(bounded(regs[n]) if regs[n][0] != 'addr' or regs[n][1] != ('stack',) else ('stack',) for n in range(4, 8))))
            for n in (2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 31):
                regs[n] = ('result', old_call) if n in (2, 3) else ('unknown',)
            stack.clear()
        regs[0] = ('const', 0)
    topology = Counter((len(block[2]),) for block in feature['cfg'])
    topology.update(branches)
    return dict(events=events, event_bag=Counter(events), memory=Counter(memory),
                calls=Counter(call for call in calls if call not in ('unknown_call', 'indirect')), topology=topology)


class MipsSimilarity:
    def __init__(self, pal, ntsc, matches):
        self.left, self.right = {}, {}
        left_ids, right_ids = {}, {}
        for idx, (a, b) in enumerate(sorted(matches.items())):
            # Each established pair gets its own identity even when code signatures repeat
            identity = 'callee_%d'%idx
            left_ids[a] = right_ids[b] = identity
        for image, target, ids in ((pal, self.left, left_ids), (ntsc, self.right, right_ids)):
            for address, feature in image.features.items():
                target[address] = fingerprint(feature, ids)

    def evidence(self, a, b):
        left, right = self.left[a], self.right[b]
        return {key: dice(left[key], right[key]) for key in ('event_bag', 'memory', 'calls', 'topology')}

    def shortlist(self, a, available, limit=32):
        ranked = []
        left = self.left[a]
        for b in available:
            ev = self.evidence(a, b)
            weights = dict(event_bag=.45, memory=.30, calls=.20, topology=.05)
            active = {k: w for k, w in weights.items() if left[k] or self.right[b][k]}
            score = sum(ev[k]*w for k, w in active.items())/sum(active.values()) if active else 0
            ranked.append((score, b))
        return [b for _, b in sorted(ranked, key=lambda row: (-row[0], row[1]))[:limit]]

"""Execute bounded raw MIPS-I owners with explicit external call fixtures."""

import struct


class MipsFunctionFixture:
    @staticmethod
    def signed(value):
        value &= 0xffffffff
        return value - 0x100000000 if value & 0x80000000 else value

    def __init__(self, code, base, memory, registers, calls=None, allow_ram_aliases=False):
        self.code = code
        self.base = base
        self.memory = bytearray(memory)
        self.registers = list(registers)
        self.calls = calls or {}
        self.events = []
        self.access_widths = []
        self.byte_masks = []
        self.executed = set()
        self.allow_ram_aliases = allow_ram_aliases
        self.hi = self.lo = 0

    def access(self, address, value=None, width=4, byte_mask=None):
        bases = (0, 0x80000000, 0xa0000000) if self.allow_ram_aliases else (0x80000000,)
        base = next((base for base in bases if base <= address <= base + 2097152 - width), None)
        if width not in (1, 2, 4) or address & (width - 1) or base is None:
            raise ValueError(f'Fixture access outside aligned cached RAM: {address:08x}')
        mask = (1 << width) - 1 if byte_mask is None else byte_mask
        if not isinstance(mask, int) or mask <= 0 or mask >= (1 << width):
            raise ValueError("Fixture byte mask is invalid")
        offset = address - base
        if value is None:
            value = int.from_bytes(self.memory[offset:offset + width], 'little')
            value &= sum(0xff << (8 * lane) for lane in range(width) if mask & (1 << lane))
            self.events.append((address, value, 0))
        else:
            value &= (1 << (width * 8)) - 1
            raw = value.to_bytes(width, 'little')
            for lane in range(width):
                if mask & (1 << lane):
                    self.memory[offset + lane] = raw[lane]
            self.events.append((address, value, 1))
        self.access_widths.append(width)
        self.byte_masks.append(mask)
        return value

    def execute(self, entry, limit=2000000):
        pc, pending_branch, pending_load = entry, None, None
        r = self.registers
        for steps in range(limit):
            if pc in self.calls:
                if pending_branch is not None or pending_load is not None:
                    raise ValueError('Pending pipeline state at fixture boundary')
                return_pc = r[31]
                self.calls[pc](self)
                pc = return_pc
                continue
            if not self.base <= pc < self.base + len(self.code):
                return pc, steps
            self.executed.add(pc)
            word = struct.unpack_from('<I', self.code, pc - self.base)[0]
            op, rs, rt, rd = word >> 26, (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31
            imm = word & 0xffff
            displacement = imm - 0x10000 if imm & 0x8000 else imm
            next_pc = pc + 4 if pending_branch is None else pending_branch
            branch_delay = pending_branch is not None
            previous_load, pending_load, pending_branch = pending_load, None, None
            destination = None
            if op == 0:
                fn = word & 63
                if fn in (0, 2, 3):
                    destination = rd
                    shift = (word >> 6) & 31
                    r[rd] = {0: lambda: r[rt] << shift,
                             2: lambda: r[rt] >> shift,
                             3: lambda: self.signed(r[rt]) >> shift}[fn]()
                elif fn in (4, 6, 7):
                    destination = rd
                    shift = r[rs] & 31
                    r[rd] = {4: lambda: r[rt] << shift,
                             6: lambda: r[rt] >> shift,
                             7: lambda: self.signed(r[rt]) >> shift}[fn]()
                elif fn == 8:
                    pending_branch = r[rs]
                elif fn == 9:
                    pending_branch = r[rs]
                    destination, r[rd] = rd, pc + 8
                elif fn in (16, 18):
                    destination = rd
                    r[rd] = self.hi if fn == 16 else self.lo
                elif fn in (24, 25):
                    left = self.signed(r[rs]) if fn == 24 else r[rs]
                    right = self.signed(r[rt]) if fn == 24 else r[rt]
                    product = (left * right) & 0xffffffffffffffff
                    self.lo = product & 0xffffffff
                    self.hi = product >> 32
                elif fn == 13:
                    high, low = (word >> 16) & 0x3ff, (word >> 6) & 0x3ff
                    code = f'{high}, {low}' if low else str(high)
                    raise ValueError(f'Guest MIPS BREAK {code}')
                elif fn in (26, 27):
                    dividend = self.signed(r[rs]) if fn == 26 else r[rs]
                    divisor = self.signed(r[rt]) if fn == 26 else r[rt]
                    if divisor == 0:
                        self.lo = 1 if fn == 26 and dividend < 0 else 0xffffffff
                        self.hi = dividend & 0xffffffff
                    else:
                        quotient = abs(dividend) // abs(divisor)
                        if (dividend < 0) != (divisor < 0):
                            quotient = -quotient
                        self.lo = quotient & 0xffffffff
                        self.hi = (dividend - quotient * divisor) & 0xffffffff
                elif fn in (32, 34):
                    left, right = self.signed(r[rs]), self.signed(r[rt])
                    value = left + right if fn == 32 else left - right
                    if not -0x80000000 <= value <= 0x7fffffff:
                        raise ValueError(f'Guest MIPS arithmetic overflow at {pc:08x}')
                    destination, r[rd] = rd, value
                elif fn in (33, 35, 36, 37, 38, 39, 42, 43):
                    destination = rd
                    r[rd] = {33: lambda: r[rs] + r[rt], 35: lambda: r[rs] - r[rt],
                             36: lambda: r[rs] & r[rt], 37: lambda: r[rs] | r[rt], 38: lambda: r[rs] ^ r[rt],
                             39: lambda: ~(r[rs] | r[rt]),
                             42: lambda: int(self.signed(r[rs]) < self.signed(r[rt])),
                             43: lambda: int(r[rs] < r[rt])}[fn]()
                else:
                    raise ValueError(f'Unsupported special instruction {word:08x}')
            elif op in (2, 3):
                pending_branch = ((pc + 4) & 0xf0000000) | ((word & 0x3ffffff) << 2)
                if op == 3:
                    destination, r[31] = 31, pc + 8
            elif op in (4, 5):
                # Even a not-taken branch has one delay instruction
                taken = (r[rs] == r[rt]) == (op == 4)
                pending_branch = pc + 4 + displacement * 4 if taken else pc + 8
            elif op in (6, 7):
                taken = (self.signed(r[rs]) <= 0) == (op == 6)
                pending_branch = pc + 4 + displacement * 4 if taken else pc + 8
            elif op == 1 and rt in (0, 1):
                taken = (self.signed(r[rs]) < 0) == (rt == 0)
                pending_branch = pc + 4 + displacement * 4 if taken else pc + 8
            elif op == 8:
                value = self.signed(r[rs]) + displacement
                if not -0x80000000 <= value <= 0x7fffffff:
                    raise ValueError(f'Guest MIPS arithmetic overflow at {pc:08x}')
                destination, r[rt] = rt, value
            elif op == 9:
                destination, r[rt] = rt, r[rs] + displacement
            elif op == 15:
                destination, r[rt] = rt, imm << 16
            elif op in (10, 11, 12, 13, 14):
                destination = rt
                r[rt] = {10: lambda: int(self.signed(r[rs]) < displacement),
                         11: lambda: int(r[rs] < (displacement & 0xffffffff)),
                         12: lambda: r[rs] & imm, 13: lambda: r[rs] | imm,
                         14: lambda: r[rs] ^ imm}[op]()
            elif op in (34, 38, 42, 46):
                address = (r[rs] + displacement) & 0xffffffff
                lane = address & 3
                left = op in (34, 42)
                shift = 8 * (3 - lane if left else lane)
                byte_mask = (1 << (lane + 1)) - 1 if left else (15 << lane) & 15
                if op in (34, 38):
                    original = previous_load[1] if previous_load and previous_load[0] == rt else r[rt]
                    value = self.access(address & ~3, byte_mask=byte_mask)
                    bits = (0xffffffff << shift) & 0xffffffff if left else 0xffffffff >> shift
                    loaded = value << shift if left else value >> shift
                    pending_load = (rt, (original & ~bits) | (loaded & bits))
                else:
                    value = r[rt] >> shift if left else (r[rt] << shift) & 0xffffffff
                    self.access(address & ~3, value, byte_mask=byte_mask)
            elif op in (32, 33, 35, 36, 37, 40, 41, 43):
                address = (r[rs] + displacement) & 0xffffffff
                width = {32: 1, 33: 2, 35: 4, 36: 1, 37: 2, 40: 1, 41: 2, 43: 4}[op]
                if op in (32, 33, 35, 36, 37):
                    value = self.access(address, width=width)
                    if op in (32, 33) and value & (1 << (width * 8 - 1)):
                        value -= 1 << (width * 8)
                    pending_load = (rt, value)
                else:
                    self.access(address, r[rt], width=width)
            else:
                raise ValueError(f'Unsupported instruction {word:08x}')
            if branch_delay and pending_branch is not None:
                raise ValueError('Control transfer in a delay slot is unsupported')
            if previous_load is not None and previous_load[0] != destination:
                r[previous_load[0]] = previous_load[1]
            for index, value in enumerate(r):
                r[index] = value & 0xffffffff
            r[0] = 0
            pc = next_pc & 0xffffffff
        raise ValueError('Fixture instruction limit exceeded')

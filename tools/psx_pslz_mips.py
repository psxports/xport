"""Execute original PSLZ MIPS and compare the result with the separate decoder"""
import hashlib
import json
import struct
from pathlib import Path

from psx_pslz import inspect


def signed(value):
    return value - 0x100000000 if value & 0x80000000 else value


def execute(path):
    raw = path.read_bytes()
    pc, gp, base, size = struct.unpack_from('<4I', raw, 16)
    entry_region = pc
    metadata = struct.unpack_from('<7I', raw, pc - base + 2048 - 28)
    target = metadata[6]
    memory = bytearray(2097152)
    memory[base & 0x1fffff:(base & 0x1fffff) + size] = raw[2048:2048 + size]
    registers = [0] * 32
    registers[28] = gp
    registers[29] = 0x801fff00
    pending = None
    pending_load = None
    steps = 0
    history = []
    while pc != target:
        if pc == 0xa0 and registers[9] == 0x44:
            pc = registers[31]
            continue
        steps += 1
        if steps > 40000000:
            raise ValueError('Instruction limit exceeded')
        word = struct.unpack_from('<I', memory, pc & 0x1fffff)[0]
        history.append((hex(pc), hex(word), hex(registers[16]), hex(registers[2])))
        history = history[-12:]
        if not (entry_region <= pc < entry_region + 0x200 or metadata[4] <= pc < metadata[4] + 0x200):
            raise ValueError(f'Unexpected transfer outside unpacker: {history}')
        opcode = word >> 26
        rs, rt, rd = (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31
        shift, function = (word >> 6) & 31, word & 63
        immediate = word & 65535
        displacement = immediate - 65536 if immediate & 32768 else immediate
        next_pc = pending if pending is not None else pc + 4
        pending = None
        previous_load = pending_load
        pending_load = None
        if opcode == 0:
            if function == 0:
                registers[rd] = registers[rt] << shift
            elif function == 2:
                registers[rd] = registers[rt] >> shift
            elif function == 3:
                registers[rd] = signed(registers[rt]) >> shift
            elif function in (8, 9):
                jump = registers[rs]
                if function == 9:
                    registers[rd] = pc + 8
                pending = jump
            elif function == 12:
                if registers[4] not in (1, 2):
                    raise ValueError('Unexpected syscall')
            elif function == 33:
                registers[rd] = registers[rs] + registers[rt]
            elif function == 35:
                registers[rd] = registers[rs] - registers[rt]
            elif function == 36:
                registers[rd] = registers[rs] & registers[rt]
            elif function == 37:
                registers[rd] = registers[rs] | registers[rt]
            elif function == 39:
                registers[rd] = ~(registers[rs] | registers[rt])
            elif function == 43:
                registers[rd] = int(registers[rs] < registers[rt])
            else:
                raise ValueError(f'Unsupported special instruction {word:08x} at {pc:08x}: {history}')
        elif opcode in (2, 3):
            pending = ((pc + 4) & 0xf0000000) | ((word & 0x3ffffff) << 2)
            if opcode == 3:
                registers[31] = pc + 8
        elif opcode in (4, 5):
            condition = registers[rs] == registers[rt]
            if condition == (opcode == 4):
                pending = pc + 4 + displacement * 4
        elif opcode == 9:
            registers[rt] = registers[rs] + displacement
        elif opcode == 12:
            registers[rt] = registers[rs] & immediate
        elif opcode == 13:
            registers[rt] = registers[rs] | immediate
        elif opcode == 15:
            registers[rt] = immediate << 16
        elif opcode in (35, 36, 40, 43):
            address = (registers[rs] + displacement) & 0x1fffff
            if opcode == 35:
                pending_load = (rt, struct.unpack_from('<I', memory, address)[0])
            elif opcode == 36:
                pending_load = (rt, memory[address])
            elif opcode == 40:
                memory[address] = registers[rt] & 255
            else:
                struct.pack_into('<I', memory, address, registers[rt])
        else:
            raise ValueError(f'Unsupported instruction {word:08x} at {pc:08x}: {history}')
        if previous_load is not None:
            registers[previous_load[0]] = previous_load[1]
        registers = [value & 0xffffffff for value in registers]
        registers[0] = 0
        pc = next_pc & 0xffffffff
    return bytes(memory[base & 0x1fffff:(base & 0x1fffff) + metadata[2]]), steps, registers[28]


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project', required=True, type=Path)
    root = parser.parse_args().project.resolve()
    results = []
    for path in sorted((root / 'orig').iterdir()):
        if not path.is_file() or path.read_bytes()[:8] != b'PS-X EXE':
            continue
        expected, report = inspect(path)
        actual, instructions, gp = execute(path)
        if expected != actual:
            raise ValueError(f'MIPS result differs from decoder: {path.name}')
        results.append({'image': path.name, 'result': 'MATCH', 'instructions': instructions,
                        'gp_at_decoded_entry': gp,
                        'decoded_sha256': hashlib.sha256(actual).hexdigest(),
                        'limitations': 'Cache flush and critical-section BIOS calls modeled as RAM-preserving'})
    (root / 'status' / 'pslz-review' / 'mips-verification.json').write_text(
        json.dumps(results, indent=2) + '\n', encoding='utf8')
    print(json.dumps(results))

"""Reproduce the reviewed PSLZ reverse decoder without modifying original inputs"""
import hashlib
import json
import struct
from pathlib import Path

from capstone import Cs, CS_ARCH_MIPS, CS_MODE_LITTLE_ENDIAN, CS_MODE_MIPS32


def inspect(path):
    raw = path.read_bytes()
    entry, gp, base, size = struct.unpack_from('<4I', raw, 16)
    header_offset = entry - base + 2048 - 28
    magic, destination, output_size, source, relocation, stub_size, target = struct.unpack_from(
        '<7I', raw, header_offset)
    if magic != 0x5a4c5350:
        raise ValueError('Missing PSLZ magic')
    memory = bytearray(2097152)
    begin = base & 0x1fffff
    memory[begin:begin + size] = raw[2048:2048 + size]
    reader = source & 0x1fffff
    writer = destination & 0x1fffff
    end = writer
    while True:
        flags = memory[reader]
        reader -= 1
        for _ in range(8):
            if reader >= writer:
                break
            value = memory[reader]
            reader -= 1
            if flags & 1:
                distance = memory[reader]
                reader -= 1
                if distance & 128:
                    distance = ((distance & 127) << 8) | memory[reader]
                    reader -= 1
                reference = writer + distance + 1
                for _ in range(value + 3):
                    if not 0 <= writer <= reference <= end:
                        raise ValueError('Invalid back reference')
                    memory[writer] = memory[reference]
                    writer -= 1
                    reference -= 1
            else:
                memory[writer] = value
                writer -= 1
            flags >>= 1
        if reader >= writer:
            break
    output = bytes(memory[writer + 1:end + 1])
    output_base = 0x80000000 + writer + 1
    if len(output) != output_size or output_base != base:
        raise ValueError('Decoded range differs from original metadata')
    disassembler = Cs(CS_ARCH_MIPS, CS_MODE_MIPS32 | CS_MODE_LITTLE_ENDIAN)
    gp_instructions = []
    for offset in range(0, len(output), 4):
        for instruction in disassembler.disasm(output[offset:offset + 4], output_base + offset):
            if '$gp' in instruction.op_str:
                gp_instructions.append({'address': instruction.address,
                                        'bytes': instruction.bytes.hex(),
                                        'instruction': instruction.mnemonic + ' ' + instruction.op_str})
    return output, {'source': str(path), 'source_sha256': hashlib.sha256(raw).hexdigest(),
                    'decoded_sha256': hashlib.sha256(output).hexdigest(),
                    'load_address': output_base, 'entry': target, 'header_gp': gp,
                    'decoded_bytes': len(output), 'metadata_offset': header_offset,
                    'gp_instruction_candidates': gp_instructions}


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project', required=True, type=Path)
    root = parser.parse_args().project.resolve()
    destination = root / 'status' / 'pslz-review'
    destination.mkdir(parents=True, exist_ok=True)
    reports = []
    for path in sorted((root / 'orig').iterdir()):
        if not path.is_file() or path.read_bytes()[:8] != b'PS-X EXE':
            continue
        output, report = inspect(path)
        output_path = destination / (path.name + '.payload.bin')
        if output_path.exists() and output_path.read_bytes() != output:
            raise ValueError('Refusing to replace differing decoded evidence')
        output_path.write_bytes(output)
        report['decoded_path'] = str(output_path.relative_to(root))
        reports.append(report)
    (destination / 'report.json').write_text(json.dumps(reports, indent=2) + '\n', encoding='utf8')
    print(json.dumps([{'image': Path(r['source']).name, 'decoded_bytes': r['decoded_bytes'],
                       'decoded_sha256': r['decoded_sha256'],
                       'gp_candidates': len(r['gp_instruction_candidates'])} for r in reports]))

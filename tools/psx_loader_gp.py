"""Validate BIOS-derived GP for a byte-verified PSLZ executable"""
import hashlib
import struct
from pathlib import Path

from psx_pslz import inspect
from psx_pslz_mips import execute


def checked_path(root, value):
    path = Path(value)
    return path if path.is_absolute() else Path(root) / path


def validate(root, profile, evidence):
    original = checked_path(root, evidence['original_path'])
    raw = original.read_bytes()
    if hashlib.sha256(raw).hexdigest() != evidence['original_sha256']:
        raise ValueError('Loader GP original executable hash changed')
    if raw[:8] != b'PS-X EXE':
        raise ValueError('Loader GP requires an original PS-X EXE')
    header_gp = struct.unpack_from('<I', raw, 20)[0]
    if not header_gp or header_gp != profile['gp']:
        raise ValueError('Loader GP differs from the original executable header')
    bios = checked_path(root, evidence['bios_path']).read_bytes()
    if hashlib.sha256(bios).hexdigest() != evidence['bios_sha256']:
        raise ValueError('Loader GP BIOS hash changed')
    offset = evidence['bios_exec_offset']
    expected = (0x8c8b0000, 0x8c9c0004, 0x00a02021, 0x0160f809, 0x00c02821)
    if struct.unpack_from('<5I', bios, offset) != expected:
        raise ValueError('Loader GP BIOS entry/GP transfer instructions differ')
    decoded, report = inspect(original)
    load = profile['loads'][0]
    loaded = checked_path(root, load['path']).read_bytes()[load.get('offset', 0):]
    if loaded != decoded or load['base'] != report['load_address']:
        raise ValueError('Loader GP decoded payload or load address differs')
    if profile['entry'] != report['entry']:
        raise ValueError('Loader GP entry differs from original PSLZ metadata')
    actual, steps, gp = execute(original)
    if actual != decoded or gp != header_gp:
        raise ValueError('Loader GP original MIPS execution differs from reviewed payload/GP')
    return {'gp': gp, 'source': 'bios_exec_pslz', 'original_sha256': report['source_sha256'],
            'decoded_sha256': report['decoded_sha256'], 'instructions': steps}

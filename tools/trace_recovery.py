"""Inspect an interrupted user recording without inventing completion evidence"""
import struct

from trace_cache import digest
from trace_vblank import Decoder


def inspect_prefix(source):
    counts = {kind: 0 for kind in range(1, 19)}
    memory = Decoder()
    checkpoints = []
    footer = None
    problem = None
    valid_bytes = 12
    with source.open('rb') as stream:
        header = stream.read(12)
        if len(header) != 12:
            raise ValueError('Truncated trace header')
        magic, start, duration = struct.unpack('<3I', header)
        if magic != 0x32544646 or not 1 <= duration <= 600:
            raise ValueError('Invalid user trace header')
        while True:
            offset = stream.tell()
            raw = stream.read(12)
            if not raw:
                break
            try:
                if footer is not None:
                    raise ValueError('Trailing data after footer')
                if len(raw) != 12:
                    raise ValueError('Truncated record header')
                kind, tick, length = struct.unpack('<3I', raw)
                if kind not in counts or length > 32*1024*1024:
                    raise ValueError('Invalid record envelope')
                payload = stream.read(length)
                if len(payload) != length:
                    raise ValueError('Truncated record payload')
                if kind == 8:
                    memory.accept(payload, tick, offset)
                elif kind == 9:
                    if length <= 20:
                        raise ValueError('Truncated checkpoint request')
                    number, frame, low, high, pc = struct.unpack_from('<5I', payload)
                    name = payload[20:].decode('ascii')
                    if number != len(checkpoints) or not 1 <= len(name) <= 64 or any(
                            c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_' for c in name):
                        raise ValueError('Invalid checkpoint identity')
                    checkpoints.append({'index': number, 'name': name, 'frame': frame,
                                        'emulated_ticks': low | high << 32, 'pc': pc,
                                        'committed': False})
                elif kind == 10:
                    if length != 12:
                        raise ValueError('Invalid checkpoint completion')
                    number, success, prepare_us = struct.unpack('<3I', payload)
                    if number >= len(checkpoints) or checkpoints[number].get('completion_seen') or success not in (0, 1):
                        raise ValueError('Invalid checkpoint completion identity')
                    checkpoints[number].update(completion_seen=True, committed=bool(success), prepare_us=prepare_us)
                elif kind == 5:
                    if length != 4 or struct.unpack('<I', payload)[0] not in (0, 1):
                        raise ValueError('Invalid footer')
                    footer = bool(struct.unpack('<I', payload)[0])
                counts[kind] += 1
                valid_bytes = stream.tell()
            except (ValueError, UnicodeError) as error:
                problem = {'offset': offset, 'reason': str(error)}
                break
    size = source.stat().st_size
    return {'source': str(source.resolve()), 'sha256': digest(source), 'bytes': size,
            'structural_prefix_bytes': valid_bytes, 'unverified_suffix_bytes': size-valid_bytes,
            'start_game_tick': start, 'duration_limit_seconds': duration,
            'footer_success': footer, 'problem': problem, 'records': counts,
            'vblank_index': memory.index, 'checkpoints': checkpoints,
            'comparison_ready': False,
            'scope': 'Framing and VBlank memory prefix only; complete channel validation requires decode'}

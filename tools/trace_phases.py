"""Read ordered native/original execution boundaries and compare their captured state"""
from trace_layout import PHASE_PCS, PHASE_BYTES, SOUND_BYTES
import argparse
from collections import Counter
import json
from pathlib import Path
import struct
from itertools import islice, zip_longest
from trace_phase_gpu import decode_packets
from gpu_packet_semantics import canonical_packet, IGNORE_RAW_SPRITE_RGB


def iter_phases(path, sound=False, gpu=False, allow_incomplete=False, diagnostics=None):
    row = None
    ordinal = 0
    file_size = Path(path).stat().st_size
    with Path(path).open('rb') as stream:
        header = stream.read(12)
        if len(header) != 12:
            raise ValueError('Truncated phase stream header')
        magic, version, size = struct.unpack('<3I', header)
        if magic not in (0x32544646, 0x31504646):
            raise ValueError('Unsupported phase stream')
        native = magic == 0x31504646
        if native and (version not in (1, 2, 3) or size != PHASE_BYTES):
            raise ValueError('Unsupported native phase schema')
        if sound and native and version < 2:
            raise ValueError('Native phase stream lacks sound capture capability')
        if gpu and native and version < 3:
            raise ValueError('Native phase stream lacks GPU capture capability')
        complete = False
        while raw := stream.read(12):
            if len(raw) != 12 and allow_incomplete:
                if diagnostics is not None: diagnostics['tail'] = 'truncated_envelope'
                break
            if len(raw) != 12 or complete:
                raise ValueError('Truncated or trailing phase record')
            kind, tick, length = struct.unpack('<3I', raw)
            allowed = (4, 5, 15, 17) if version >= 3 else ((4, 5, 15) if version == 2 else (5, 15))
            if length > 32*1024*1024 or not 1 <= kind <= 18 or (native and kind not in allowed):
                raise ValueError('Invalid phase record envelope')
            if stream.tell()+length > file_size:
                if allow_incomplete:
                    if diagnostics is not None: diagnostics['tail'] = 'truncated_payload'
                    break
                raise ValueError('Truncated phase payload')
            if kind not in (4, 5, 15, 17):
                stream.seek(length, 1)
                continue
            payload = stream.read(length)
            if len(payload) != length:
                raise ValueError('Truncated phase payload')
            if kind == 5:
                if payload != struct.pack('<I', 1):
                    if allow_incomplete:
                        if diagnostics is not None: diagnostics['tail'] = 'unsuccessful_footer'
                        break
                    raise ValueError('Phase stream did not finish successfully')
                complete = True
            elif kind == 15:
                if length != PHASE_BYTES:
                    raise ValueError('Invalid phase payload size')
                pc, frame, low, high, stage, callback, pad1, pad2 = struct.unpack_from('<8I', payload)
                if pc not in PHASE_PCS:
                    raise ValueError('Unknown phase PC')
                if row is not None:
                    if gpu and 'gpu' not in row:
                        raise ValueError('Missing phase GPU evidence')
                    row['sound_complete'] = True
                    yield row
                row = {'ordinal': ordinal, 'pc': pc, 'game_tick': tick, 'frame': frame,
                             'emulated_ticks': None if native else low | high << 32,
                             'state': payload[16:], 'stage': stage, 'callback': callback}
                ordinal += 1
                if sound:
                    row['sound'] = []
            elif kind == 17:
                pc, packets = decode_packets(payload)
                if row is None or row['pc'] != pc or row['game_tick'] != tick or row.get('gpu_present'):
                    raise ValueError('Unpaired or duplicate phase GPU record')
                row['gpu_present'] = True
                if gpu:
                    row['gpu'] = packets
            elif kind == 4 and sound:
                if length not in (8, 12, 16):
                    raise ValueError('Invalid phase sound record')
                values = struct.unpack('<'+'I'*(length//4), payload)
                expected = SOUND_BYTES
                if expected.get(values[0]) != length:
                    raise ValueError('Unknown phase sound signature')
                if row is not None:
                    row['sound'].append([tick, *values])
        if diagnostics is not None: diagnostics['footer_complete'] = complete
        if not complete and not allow_incomplete:
            raise ValueError('Missing phase completion footer')
        if gpu and (row is None or 'gpu' not in row):
            if allow_incomplete:
                if diagnostics is not None: diagnostics['tail'] = 'missing_terminal_gpu'
                return
            raise ValueError('Missing phase GPU evidence')
        if row is not None:
            row['sound_complete'] = complete
            yield row


def read_phases(path, sound=False, gpu=False):
    return list(iter_phases(path, sound=sound, gpu=gpu))


def compare(original, native, sound=False, gpu=False, include_last_sound=False):
    raw_gpu_differences = 0
    first_raw_gpu_difference = None
    if not original or not native:
        return {'passed': False, 'first_difference': {'ordinal': 0, 'field': 'missing_phase_evidence'}}
    for i in range(max(len(original), len(native))):
        if i >= len(original) or i >= len(native):
            return {'passed': False, 'first_difference': {'ordinal': i, 'field': 'boundary_count'}}
        a, b = original[i], native[i]
        for field in ('pc', 'game_tick', 'state'):
            if a[field] != b[field]:
                diff = {'ordinal': i, 'pc': a['pc'], 'game_tick': a['game_tick'], 'field': field}
                if field == 'state':
                    offset = next(j for j, (x, y) in enumerate(zip(a[field], b[field])) if x != y)
                    diff.update(byte_offset=offset, original=a[field][offset], native=b[field][offset])
                else:
                    diff.update(original=a[field], native=b[field])
                return {'passed': False, 'first_difference': diff}
        if sound and (include_last_sound or i < len(original)-1) and a['sound'] != b['sound']:
            left, right = a['sound'], b['sound']
            first = next(j for j in range(max(len(left), len(right)))
                         if j >= len(left) or j >= len(right) or left[j] != right[j])
            return {'passed':False, 'first_difference':{'ordinal':i, 'game_tick':a['game_tick'],
                    'field':'sound', 'event':first, 'original':left[first] if first < len(left) else None,
                    'native':right[first] if first < len(right) else None}}
        if gpu and a['gpu'] != b['gpu']:
            left, right = a['gpu'], b['gpu']
            for j in range(max(len(left), len(right))):
                if j >= len(left) or j >= len(right) or left[j] != right[j]:
                    raw_gpu_differences += 1
                    if first_raw_gpu_difference is None:
                        first_raw_gpu_difference = {'ordinal':i, 'packet':j,
                            'original':left[j].hex() if j < len(left) else None,
                            'native':right[j].hex() if j < len(right) else None}
            if len(left)==len(right) and all(x==y or canonical_packet(x, IGNORE_RAW_SPRITE_RGB)==canonical_packet(y, IGNORE_RAW_SPRITE_RGB) for x,y in zip(left,right)):
                continue
            first = next(j for j in range(max(len(left), len(right)))
                         if j >= len(left) or j >= len(right) or canonical_packet(left[j], IGNORE_RAW_SPRITE_RGB) != canonical_packet(right[j], IGNORE_RAW_SPRITE_RGB))
            return {'passed':False, 'first_difference':{'ordinal':i, 'game_tick':a['game_tick'],
                    'field':'gpu', 'packet':first, 'original_packets':len(left), 'native_packets':len(right),
                    'original':left[first].hex() if first < len(left) else None,
                    'native':right[first].hex() if first < len(right) else None}}
    return {'passed': True, 'boundaries': len(original), 'first_difference': None,
            'sound_events':sum(len(r['sound']) for r in (original if include_last_sound else original[:-1])) if sound else None,
            'gpu_packets':sum(len(r['gpu']) for r in original) if gpu else None,
            'raw_gpu_differences':raw_gpu_differences, 'first_raw_gpu_difference':first_raw_gpu_difference,
            'scope': 'Ordered PCs, counters and selected phase state'+('; ordered sound calls in complete intervals' if sound else '')+('; OT payloads excluding documented textured-quad padding only; raw differences retained' if gpu else '')+'; no pixels/PCM or full RAM proof'}


def compare_streams(original_path, native_path, begin=0, sound=False, gpu=False):
    original = iter_phases(original_path, sound=sound, gpu=gpu)
    native = iter_phases(native_path, sound=sound, gpu=gpu)
    pairs = zip_longest(islice(original, begin, None), native)
    totals = {'boundaries':0, 'sound_events':0 if sound else None, 'gpu_packets':0 if gpu else None,
              'raw_gpu_differences':0, 'first_raw_gpu_difference':None}
    try:
        current = next(pairs, None)
        while current is not None:
            a, b = current
            if a is None or b is None:
                return {'passed':False, 'first_difference':{'ordinal':totals['boundaries'], 'field':'boundary_count'}}
            following = next(pairs, None)
            result = compare([a], [b], sound=sound, gpu=gpu,
                             include_last_sound=following is not None and following[0] is not None)
            if not result['passed']:
                result['first_difference']['ordinal'] += totals['boundaries']
                return result
            if totals['first_raw_gpu_difference'] is None and result['first_raw_gpu_difference']:
                first = dict(result['first_raw_gpu_difference'])
                first['ordinal'] += totals['boundaries']; totals['first_raw_gpu_difference'] = first
            for key in ('boundaries', 'sound_events', 'gpu_packets', 'raw_gpu_differences'):
                if result[key] is not None:
                    totals[key] += result[key]
            totals['scope'] = result['scope']
            current = following
        if not totals['boundaries']:
            return {'passed':False, 'first_difference':{'ordinal':0, 'field':'missing_phase_evidence'}}
        return dict(totals, passed=True, first_difference=None)
    finally:
        original.close(); native.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--native', type=Path)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    from xport_project import artifact_path
    args.report = artifact_path(args.report)
    rows = read_phases(args.source)
    report = {'boundaries': len(rows), 'pcs': {hex(k): v for k, v in Counter(r['pc'] for r in rows).items()},
              'transitions': [{k: row[k] for k in ('ordinal', 'pc', 'game_tick', 'stage')}
                              for i, row in enumerate(rows) if not i or rows[i-1]['pc'] != row['pc']]}
    if args.native:
        report['comparison'] = compare(rows, read_phases(args.native))
    with args.report.open('x') as stream:
        json.dump(report, stream, indent=2)
    print(json.dumps(report))


if __name__ == '__main__':
    main()

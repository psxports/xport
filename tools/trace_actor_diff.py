"""Compare configured actor boundaries without discarding inactive-slot differences"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from trace_layout import PROFILE, CONFIG, ACTOR_BYTES
RECORD_SIZE = 4+ACTOR_BYTES
ACTOR_SIZE = CONFIG.get('trace_layout', {}).get('actor_stride', PROFILE['actors_size'])
FIELDS = CONFIG.get('trace_layout', {}).get('actor_fields', [])
if type(ACTOR_SIZE) is not int or ACTOR_SIZE <= 0 or PROFILE['actors_size'] % ACTOR_SIZE:
    raise ValueError('Actor stride does not divide captured actor block')
for pos, size, name, fmt in FIELDS:
    if pos < 0 or size != struct.calcsize(fmt) or pos+size > ACTOR_SIZE:
        raise ValueError('Actor field exceeds declared record')


def validate(data):
    if not data or len(data) % RECORD_SIZE:
        raise ValueError('Empty or truncated actor trace')
    ticks = [struct.unpack_from('<I', data, i)[0]
             for i in range(0, len(data), RECORD_SIZE)]
    if any(b != a + 1 for a, b in zip(ticks, ticks[1:])):
        raise ValueError('Missing, duplicate or out-of-order actor boundary')
    return ticks


def compare(original, native):
    result = {'schema': 'ff-actor-diff-v1', 'passed': False,
              'original_sha256': hashlib.sha256(original).hexdigest(),
              'native_sha256': hashlib.sha256(native).hexdigest()}
    try:
        a_ticks, b_ticks = validate(original), validate(native)
    except ValueError as error:
        return dict(result, outcome='invalid_trace', error=str(error))
    if a_ticks != b_ticks:
        return dict(result, outcome='invalid_trace', error='Boundary ranges differ',
                    original_range=[a_ticks[0], a_ticks[-1], len(a_ticks)],
                    native_range=[b_ticks[0], b_ticks[-1], len(b_ticks)])
    result.update(start_tick=a_ticks[0], end_tick=a_ticks[-1],
                  records=len(a_ticks), compared_bytes=len(original))
    if original == native:
        return dict(result, passed=True, outcome='match', first_difference=None)
    # Locate the first differing record before inspecting individual bytes
    index = next(i for i in range(len(a_ticks))
                 if original[i*RECORD_SIZE:(i+1)*RECORD_SIZE] !=
                 native[i*RECORD_SIZE:(i+1)*RECORD_SIZE])
    start = index * RECORD_SIZE
    a, b = original[start:start+RECORD_SIZE], native[start:start+RECORD_SIZE]
    offsets = [i for i in range(RECORD_SIZE) if a[i] != b[i]]
    offset = offsets[0]
    detail = {'tick': a_ticks[index], 'record_index': index,
              'record_offset': offset, 'original_byte': a[offset],
              'native_byte': b[offset], 'changed_bytes_in_record': len(offsets)}
    if 4 <= offset < (4+PROFILE['actors_size']):
        slot, field_offset = divmod(offset - 4, ACTOR_SIZE)
        base = 4 + slot * ACTOR_SIZE
        detail.update(region='actor', slot=slot, field_offset=field_offset)
        detail['context'] = {
            name: {'original': struct.unpack_from(fmt, a, base+pos)[0],
                   'native': struct.unpack_from(fmt, b, base+pos)[0]}
            for pos, size, name, fmt in FIELDS}
        for pos, size, name, fmt in FIELDS:
            if pos <= field_offset < pos + size:
                detail.update(field=name, **detail['context'][name])
                break
        else:
            detail['field'] = 'unknown_byte'
    else:
        detail.update(region='camera', field_offset=offset-(4+PROFILE['actors_size']))
    return dict(result, outcome='mismatch', first_difference=detail)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--original', type=Path, required=True)
    parser.add_argument('--native', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    from xport_project import artifact_path
    args.report = artifact_path(args.report)
    result = compare(args.original.read_bytes(), args.native.read_bytes())
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(result))
    return {'match': 0, 'mismatch': 1, 'invalid_trace': 2}[result['outcome']]


if __name__ == '__main__':
    raise SystemExit(main())

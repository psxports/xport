"""Validate and compare synchronized configured captures without starting either runtime"""
import argparse
import itertools
import json
from pathlib import Path
import struct

from gpu_packet_semantics import canonical_packet, IGNORE_FT4_PADDING
from trace_actor_diff import compare as actor_diff, validate as actor_ticks
from trace_cache import digest
from trace_schedule import check_inputs

from trace_layout import PROFILE, SOUND_BYTES
SOUND_ARGS = {pc:size//4-1 for pc,size in SOUND_BYTES.items()}


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def worlds(path, offset=0, observe_offset=None):
    """Stream complete world boundaries with bounded per-packet allocations"""
    with Path(path).open('rb') as stream:
        stream.seek(offset)
        def take(size):
            data = stream.read(size)
            if len(data) != size:
                raise ValueError('Truncated world/GPU stream')
            return data
        while True:
            record_offset=stream.tell()
            header = stream.read(8)
            if not header:
                return
            if len(header) != 8:
                raise ValueError('Truncated world header')
            tick, count = struct.unpack('<II', header)
            if count > PROFILE['world_max_count']:
                raise ValueError('Invalid world object count')
            pools = take(PROFILE['world_a_size']+PROFILE['world_b_size']+PROFILE['world_item_size']*count+PROFILE['world_c_size']+8)
            packets = []
            while True:
                words = struct.unpack('<I', take(4))[0]
                if not words:
                    break
                if words > 255 or len(packets) >= 131072:
                    raise ValueError('Invalid GPU packet count/length')
                packets.append(take(words*4))
            if observe_offset is not None:observe_offset(record_offset)
            yield tick, count, pools, packets


def validate_bundle(paths, start, end, require_inputs=True):
    required = {'actors', 'sound', 'world'} | ({'inputs'} if require_inputs else set())
    if not required <= paths.keys():
        raise ValueError('Missing channels: '+str(sorted(required-paths.keys())))
    if type(start) is not int or type(end) is not int or not 0 <= start < end:
        raise ValueError('Invalid trace range')
    ticks = actor_ticks(Path(paths['actors']).read_bytes())
    if len(ticks) != end-start+1 or ticks[0] != start or ticks[-1] != end:
        raise ValueError('Actor range differs from capture contract')
    last = start
    events = read_json(paths['sound'])
    if not isinstance(events, list):
        raise ValueError('Invalid sound stream')
    for event in events:
        if not isinstance(event, list) or len(event) < 2 or any(type(v) is not int for v in event):
            raise ValueError('Invalid sound event')
        address, tick = event[:2]
        if address not in SOUND_ARGS or len(event) != SOUND_ARGS[address]+2:
            raise ValueError('Unknown sound event schema')
        if not last <= tick < end or any(not 0 <= v <= 0xffffffff for v in event):
            raise ValueError('Out-of-order or out-of-range sound event')
        last = tick
    if 'inputs' in paths:
        inputs = read_json(paths['inputs'])
        if not isinstance(inputs, list) or len(inputs) != end-start:
            raise ValueError('Incomplete input stream')
        for tick, event in enumerate(inputs, start):
            if not isinstance(event, list) or len(event) != 3 or any(type(v) is not int for v in event):
                raise ValueError('Invalid input event')
            if event[0] != tick or not 0 <= event[1] <= 65535 or event[1] != event[2]:
                raise ValueError('Missing boundary or requested/decoded input disagreement')
    frames = packets = 0
    for tick, count, pools, gpu in worlds(paths['world']):
        if tick != start+frames or tick > end:
            raise ValueError('World boundary sequence differs from actor stream')
        frames += 1
        packets += len(gpu)
    if frames != end-start+1:
        raise ValueError('Incomplete world stream')
    return {'complete': True, 'boundaries': frames, 'sound_calls': len(events),
            'gpu_packets': packets, 'decoded_inputs_checked': 'inputs' in paths}


def cache_validator(identity, paths):
    capture = identity['capture']
    if capture['schema'] != 'ff-boundary-v1':
        raise ValueError('Unsupported trace schema')
    result = validate_bundle(paths, capture['start_tick'], capture['end_tick'])
    schedule = capture['options'].get('scheduled_segments')
    result['schedule_verified'] = False
    if schedule is not None:
        result.update(check_inputs(read_json(paths['inputs']), schedule, capture['start_tick'], capture['end_tick']))
    return result


def compare_bundles(original, native, start, end, raw_sprite_rgb=False, schedule=None):
    result = {'schema': 'ff-bundle-comparison-v1', 'passed': False,
              'scope': 'Actors/camera, original decoded inputs, world and GPU semantics, sound calls; no pixels/PCM',
              'start_tick': start, 'end_tick': end}
    try:
        result['original_validation'] = validate_bundle(original, start, end)
        result['native_validation'] = validate_bundle(native, start, end, require_inputs=False)
        result['schedule_verified'] = False
        if schedule is not None:
            result.update(check_inputs(read_json(original['inputs']), schedule, start, end))
            if 'inputs' in native:
                check_inputs(read_json(native['inputs']), schedule, start, end)
    except (OSError, ValueError, TypeError, struct.error) as error:
        return dict(result, outcome='invalid_trace', error=str(error))
    result['hashes'] = {side: {name: digest(path) for name, path in paths.items()}
                        for side, paths in [('original', original), ('native', native)]}
    differences = []
    actors = actor_diff(Path(original['actors']).read_bytes(), Path(native['actors']).read_bytes())
    result['actors_exact'] = actors['passed']
    if not actors['passed']:
        differences.append(dict(channel='actors', **actors['first_difference']))
    a_sound, b_sound = read_json(original['sound']), read_json(native['sound'])
    result['sound_exact'] = a_sound == b_sound
    for index, (a, b) in enumerate(itertools.zip_longest(a_sound, b_sound)):
        if a != b:
            differences.append(dict(channel='sound', index=index,
                                    tick=min(e[1] for e in (a, b) if e is not None), original=a, native=b))
            break
    raw_bad = semantic_bad = world_bad = 0
    for a, b in zip(worlds(original['world']), worlds(native['world'])):
        tick, count, pools, gpu = a
        if a[1:3] != b[1:3]:
            world_bad += 1
            if world_bad == 1:
                offset = next((i for i, (x, y) in enumerate(zip(pools, b[2])) if x != y), None)
                differences.append(dict(channel='world', tick=tick, offset=offset,
                                        original_count=count, native_count=b[1]))
        if gpu != b[3]:
            raw_bad += 1
        ca = [canonical_packet(p, raw_sprite_rgb) for p in gpu]
        cb = [canonical_packet(p, raw_sprite_rgb) for p in b[3]]
        if ca != cb:
            semantic_bad += 1
            if semantic_bad == 1:
                index = next(i for i, (x, y) in enumerate(itertools.zip_longest(ca, cb)) if x != y)
                differences.append(dict(channel='gpu', tick=tick, packet_index=index,
                                        original_packets=len(gpu), native_packets=len(b[3])))
    result.update(world_mismatched_frames=world_bad, gpu_raw_mismatched_frames=raw_bad,
                  gpu_semantic_mismatched_frames=semantic_bad,
                  exclusions={'ft4_padding': IGNORE_FT4_PADDING, 'raw_sprite_rgb': raw_sprite_rgb})
    differences.sort(key=lambda d: d['tick'])
    first = differences[0] if differences else None
    result.update(passed=not differences, outcome='mismatch' if differences else 'match',
                  first_difference=first, channel_first_differences=differences)
    if first:
        tick = first['tick']
        result['context'] = {
            'inputs': [e for e in read_json(original['inputs']) if tick-2 <= e[0] <= tick+2],
            'original_sound': [e for e in a_sound if tick-2 <= e[1] <= tick+2][:16],
            'native_sound': [e for e in b_sound if tick-2 <= e[1] <= tick+2][:16]}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    from xport_project import artifact_path
    args.report = artifact_path(args.report)
    spec = read_json(args.manifest)
    root = args.manifest.resolve().parent
    paths = {side: {name: (root/path).resolve() for name, path in spec[side].items()}
             for side in ('original', 'native')}
    result = compare_bundles(paths['original'], paths['native'], spec['start_tick'],
                             spec['end_tick'], spec.get('raw_sprite_rgb', False),
                             schedule=read_json(root/spec['input_schedule']) if 'input_schedule' in spec else None)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print(json.dumps({k: result.get(k) for k in ('outcome', 'passed', 'first_difference', 'error')}))
    return {'match': 0, 'mismatch': 1, 'invalid_trace': 2}[result['outcome']]


if __name__ == '__main__':
    raise SystemExit(main())

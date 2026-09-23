"""Strict stage channel verification using an explicit boundary contract"""
import argparse
import json
from itertools import zip_longest
from pathlib import Path
import struct
import time

from trace_actor_diff import compare as actor_compare, RECORD_SIZE
from trace_bundle import worlds
from gpu_packet_semantics import canonical_packet, IGNORE_RAW_SPRITE_RGB, IGNORE_FT4_PADDING
from trace_phases import compare_streams
from trace_phase_index import compare_window
from trace_cache import digest
from pipeline_metrics import measured


def contract_segments(contract):
    segments=contract.get('segments')
    if not isinstance(segments,list) or not segments:
        raise ValueError('Explicit gameplay segments required')
    previous=contract['phase_begin']
    for segment in segments:
        fields=('start_phase','end_phase','start_tick','end_tick','stage')
        if any(type(segment.get(key)) is not int for key in fields):
            raise ValueError('Invalid segment field')
        if not 0<=segment['start_tick']<=segment['end_tick'] or segment['stage']<0:
            raise ValueError('Invalid segment tick/stage')
        if not previous<=segment['start_phase']<segment['end_phase']<=contract['phase_end']:
            raise ValueError('Overlapping or out-of-range phase segments')
        if segment['end_phase']-segment['start_phase']!=segment['end_tick']-segment['start_tick']+1:
            raise ValueError('Missing gameplay boundary within segment')
        previous=segment['end_phase']
    if contract.get('native_tail_ticks'):
        raise ValueError('Schema 2 requires aligned boundaries, not undeclared transition tails')
    return segments


def contract_ticks(contract):
    if contract.get('schema')==2:
        return [tick for segment in contract_segments(contract)
                for tick in range(segment['start_tick'],segment['end_tick']+1)], []
    start, end = contract['start_tick'], contract['end_tick']
    if type(start) is not int or type(end) is not int or not 0 <= start <= end:
        raise ValueError('Invalid gameplay interval')
    tails = contract.get('native_tail_ticks', [])
    if not isinstance(tails, list) or any(type(t) is not int for t in tails):
        raise ValueError('Invalid explicit native tail contract')
    return list(range(start, end + 1)), tails


def verify_actors(original, native, contract):
    ticks, tails = contract_ticks(contract)
    if len(original) != len(ticks) * RECORD_SIZE or len(native) != (len(ticks) + len(tails)) * RECORD_SIZE:
        return dict(passed=False, first_difference={'field': 'actor_count'})
    for data, expected in ((original, ticks), (native, ticks + tails)):
        actual = [struct.unpack_from('<I', data, offset)[0] for offset in range(0, len(data), RECORD_SIZE)]
        if actual != expected:
            return dict(passed=False, first_difference={'field': 'actor_ticks'})
    if contract.get('schema')==2:
        offset=0
        for segment in contract_segments(contract):
            size=(segment['end_tick']-segment['start_tick']+1)*RECORD_SIZE
            result=actor_compare(original[offset:offset+size],native[offset:offset+size])
            if not result['passed']:
                if result.get('first_difference'):
                    result['first_difference']['record_index']+=offset//RECORD_SIZE
                return result
            offset+=size
        return dict(passed=True,records=len(ticks),first_difference=None)
    result = actor_compare(original, native[:len(original)])
    result['excluded_native_tail_ticks'] = tails
    return result


def verify_world(original, native, contract):
    ticks, tails = contract_ticks(contract)
    count = raw = packets = tail_count = 0
    sentinel = object()
    for ordinal, (left, right) in enumerate(zip_longest(original, native, fillvalue=sentinel)):
        if ordinal >= len(ticks):
            index = ordinal - len(ticks)
            if left is not sentinel or right is sentinel or index >= len(tails) or right[0] != tails[index]:
                return dict(passed=False, first_difference={'field': 'world_extra_boundary', 'ordinal': ordinal})
            tail_count += 1
            continue
        if left is sentinel or right is sentinel:
            return dict(passed=False, first_difference={'field': 'world_missing_boundary', 'ordinal': ordinal})
        if left[0] != ticks[ordinal] or right[0] != ticks[ordinal]:
            return dict(passed=False, first_difference={'field': 'world_tick', 'ordinal': ordinal,
                        'original': left[0], 'native': right[0]})
        if left[1:3] != right[1:3]:
            from trace_world_context import describe
            return dict(passed=False, first_difference=dict(field='world_state',tick=ticks[ordinal],record_index=ordinal,context=describe(left,right)))
        raw += left[3] != right[3]
        packets += len(left[3])
        if left[3] != right[3] and [canonical_packet(p, IGNORE_RAW_SPRITE_RGB) for p in left[3]] != [canonical_packet(p, IGNORE_RAW_SPRITE_RGB) for p in right[3]]:
            from trace_world_context import gpu_detail
            return dict(passed=False,first_difference=dict(field='gpu',tick=ticks[ordinal],record_index=ordinal,context=gpu_detail(left[3],right[3])))
        count += 1
    if count != len(ticks) or tail_count != len(tails):
        return dict(passed=False, first_difference={'field': 'world_count'})
    return dict(passed=True, boundaries=count, gpu_packets=packets, raw_gpu_mismatched_frames=raw,
                excluded_native_tail_ticks=tails, exclusions={'ft4_padding': IGNORE_FT4_PADDING,
                'raw_sprite_rgb': IGNORE_RAW_SPRITE_RGB}, first_difference=None)


def verify_inputs(original, native, contract):
    ticks, _ = contract_ticks(contract)
    excluded=0
    if contract.get('independent_stage_segments'):
        terminal_positions=[];position=0
        for segment in contract_segments(contract):
            position += segment['end_phase']-segment['start_phase']
            terminal_positions.append(position-1)
        if len(original)!=len(ticks):
            return dict(passed=False,first_difference={'field':'original_input_count'})
        for position in terminal_positions:
            if original[position][0]!=ticks[position]:
                return dict(passed=False,first_difference={'field':'segment_terminal_input_tick','ordinal':position})
        excluded=len(terminal_positions)
        excluded_set=set(terminal_positions)
        original=[row for ordinal,row in enumerate(original) if ordinal not in excluded_set]
        ticks=[tick for ordinal,tick in enumerate(ticks) if ordinal not in excluded_set]
    elif contract.get('terminal_gameplay_boundary_only'):
        if contract.get('schema')!=2 or contract_segments(contract)[-1]['end_phase']!=contract['phase_end']:
            raise ValueError('Terminal input exclusion requires the final gameplay boundary')
        excluded=contract.get('original_terminal_input_records')
        if type(excluded) is not int or excluded not in (0,1):
            raise ValueError('Declare zero or one original terminal input record')
        if excluded:
            if not original or original[-1][0]!=ticks[-1]:
                return dict(passed=False,first_difference={'field':'terminal_input_tick'})
            original=original[:-1]
        ticks=ticks[:-1]
    if len(native) % 12:
        return dict(passed=False, first_difference={'field': 'input_truncated'})
    rows = [list(row) for row in struct.iter_unpack('<3I', native)]
    if len(original) != len(ticks) or len(rows) != len(ticks):
        return dict(passed=False, first_difference={'field': 'input_count'})
    for i, (a, b) in enumerate(zip(original, rows)):
        if len(a) != 3 or a[0] != ticks[i] or b[0] != ticks[i] or a != b:
            return dict(passed=False, first_difference={'field': 'input', 'ordinal': i, 'original': a, 'native': b})
    return dict(passed=True, records=len(rows), excluded_terminal_input_records=excluded, first_difference=None)


def verify_receipt(spec):
    receipt = json.loads(Path(spec['receipt']).read_text())
    if receipt.get('exit_code') != 0:
        raise ValueError('Capture did not finish successfully')
    for name, path in spec['native'].items():
        if receipt.get('artifact_hashes', {}).get(name) != digest(path):
            raise ValueError('Capture receipt does not bind native channel: ' + name)
    expected = spec['contract']['input_calls_end']
    calls = Path(spec['input_calls']).read_bytes()
    from trace_layout import PROFILE
    stride = 8 + PROFILE['pad_size']
    if len(calls) < 4 or calls[:4] != b'FFI1' or (len(calls)-4) % stride:
        raise ValueError('Invalid input-call stream')
    if receipt.get('input_calls_sha256') != digest(spec['input_calls']):
        raise ValueError('Input-call stream differs from capture')
    result = dict(passed=receipt.get('exit_code') == 0 and
        receipt.get('input_calls_consumed') == expected and receipt.get('input_calls_available') == expected,
        expected_end=expected, consumed=receipt.get('input_calls_consumed'))
    result['passed'] &= (len(calls)-4)//stride == expected
    if not result['passed']:
        raise ValueError('Capture input cursor/count differs from declared completion')
    return result


def verify_segment_phases(spec):
    from trace_phases import iter_phases
    from trace_layout import PROFILE
    contract=spec['contract']
    fields=('stage','start_phase','end_phase','start_tick','end_tick')
    expected=[{key:segment[key] for key in fields} for segment in contract_segments(contract)]
    actual=[]
    for row in iter_phases(spec['original']['phases']):
        ordinal=row['ordinal']
        if ordinal<contract['phase_begin'] or ordinal>=contract['phase_end'] or row['pc']!=PROFILE['game_begin']:
            continue
        if not actual or actual[-1]['stage']!=row['stage'] or actual[-1]['end_phase']!=ordinal or actual[-1]['end_tick']+1!=row['game_tick']:
            actual.append(dict(stage=row['stage'],start_phase=ordinal,end_phase=ordinal+1,start_tick=row['game_tick'],end_tick=row['game_tick']))
        else:
            actual[-1].update(end_phase=ordinal+1,end_tick=row['game_tick'])
    if actual!=expected:raise ValueError('Declared stage segments differ from original phases')


@measured("verify")
def verify(spec):
    started = time.perf_counter()
    original, native, contract = spec['original'], spec['native'], spec['contract']
    if contract.get('schema') not in (1,2) or contract.get('phase_end', 0) <= contract.get('phase_begin', -1):
        raise ValueError('Explicit versioned phase contract required')
    for side in (original, native):
        for name in ('actors', 'world', 'inputs', 'phases'):
            if not Path(side[name]).is_file():
                raise ValueError('Missing channel: ' + name)
    input_receipt = verify_receipt(spec)
    if spec.get('raw_sha256') and digest(original['phases'])!=spec['raw_sha256']:
        raise ValueError('Original raw trace differs from indexed identity')
    if contract['schema']==2:
        verify_segment_phases(spec)
    if spec.get('phase_index'):
        excluded_sound={segment['end_phase']-1 for segment in contract_segments(contract)} \
            if contract.get('independent_stage_segments') else set()
        phase = compare_window(spec['phase_index'], spec['raw_sha256'], native['phases'],
                               contract['phase_begin'], contract['phase_end'], excluded_sound)
    else:
        phase = compare_streams(original['phases'], native['phases'], begin=contract['phase_begin'], sound=True, gpu=True)
        if phase.get('first_difference') and 'ordinal' in phase['first_difference']:
            phase['first_difference']['ordinal'] += contract['phase_begin']
    if phase.get('passed') and phase['boundaries'] != contract['phase_end'] - contract['phase_begin']:
        phase = dict(passed=False, first_difference={'field': 'phase_count'})
    reports = {'phases': phase,
        'actors': verify_actors(Path(original['actors']).read_bytes(), Path(native['actors']).read_bytes(), contract),
        'world': verify_world(worlds(original['world']), worlds(native['world']), contract),
        'inputs': verify_inputs(json.loads(Path(original['inputs']).read_text()), Path(native['inputs']).read_bytes(), contract)}
    reports['input_calls'] = input_receipt
    from trace_stage_difference import first_difference
    first = first_difference(reports,contract)
    hashes={side: {k: digest(v) for k,v in paths.items()} for side,paths in [('original',original),('native',native)]}
    return dict(schema=1, passed=first is None, contract=contract, channels=reports,
                first_difference=first, seconds=time.perf_counter()-started,
                scope='Declared phase state, actors/camera, world, decoded inputs, ordered sound calls and GPU semantics; no pixels/PCM',
                hashes=hashes)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    result = verify(json.loads(args.manifest.read_text()))
    from xport_project import artifact_path
    artifact_path(args.report).write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({k:result[k] for k in ('passed','first_difference','seconds')}))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())

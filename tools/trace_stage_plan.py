"""Prepare immutable stage replay inputs and bounded anchor requirements"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import time

from trace_cache import digest
from trace_layout import PROFILE, PROFILE_PATH
from trace_phases import read_phases
from trace_phase_index import build as build_index
from user_menu_replay import decode_pad_schedule, input_calls, input_call_cursor, menu_jobs
from trace_worker import write_receipt
from trace_finalize_lock import exclusive_finalize
from xport_project import load_project, artifact_path


def select_boundary(rows, entry_tick):
    if type(entry_tick) is not int or entry_tick < 0:
        raise ValueError('Project stage entry tick must be explicit')
    for index, row in enumerate(rows):
        if row['pc'] == PROFILE['game_begin'] and row['game_tick'] == entry_tick:
            return index
    raise ValueError('ANCHOR_UNSUPPORTED: no configured stage entry boundary in recording')


def validate_cache(manifest):
    for item in manifest['artifacts'].values():
        if digest(item['path']) != item['sha256']:
            raise ValueError('Prepared trace artifact changed: ' + item['path'])


def stage_pad_schedule(rows, segment):
    records=[];start=None;end=None;mask=None
    for row in rows[segment['start_phase']:segment['end_phase']]:
        pad1=struct.unpack_from('<I',row['state'],8)[0]
        value=((pad1 >> 16) ^ 0xffff) & 0xffff
        if mask==value and row['game_tick']==end+1:
            end=row['game_tick']
        else:
            if start is not None:records.append(struct.pack('<3I',start,end,mask))
            start=end=row['game_tick'];mask=value
    if start is not None:records.append(struct.pack('<3I',start,end,mask))
    return b''.join(records)


@__import__("pipeline_metrics").measured("prepare")
def prepare(name):
    started = time.perf_counter()
    root, project = load_project()
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,64}', name):
        raise ValueError('Invalid trace name')
    session_path = root/'status/user-traces'/name/'session.json'
    session = json.loads(session_path.read_text())
    if session['status'] != 'complete' or not session['capture']['validation']['complete']:
        raise ValueError('Trace is not finalized')
    settings = project.get('stage_pipeline', {})
    if 'entry_tick' not in settings:
        raise ValueError('ANCHOR_UNSUPPORTED: configure audited stage_pipeline.entry_tick')
    identity = dict(raw_sha256=session['raw_sha256'], profile_sha256=digest(PROFILE_PATH),
                    planner_sha256=digest(__file__), extractor_sha256=digest(Path(__file__).with_name('trace_stage_channels.py')),
                    indexer_sha256=digest(Path(__file__).with_name('trace_phase_index.py')), entry_tick=settings['entry_tick'])
    key = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()
    directory = artifact_path('status/stage-pipeline/prepared/'+key)
    directory.mkdir(parents=True, exist_ok=True)
    with exclusive_finalize(directory):
        manifest_path = directory/'manifest.json'
        hash_started = time.perf_counter()
        if digest(session['raw']) != identity['raw_sha256']:
            raise ValueError('Original trace changed')
        hash_seconds = time.perf_counter()-hash_started
        if manifest_path.exists():
            manifest = json.loads(manifest_path.read_text())
            if manifest['identity'] != identity:
                raise ValueError('Prepared identity changed')
            validate_cache(manifest)
            return dict(manifest=str(manifest_path), cached=True, seconds=time.perf_counter()-started,
                        hash_seconds=hash_seconds)
        rows = read_phases(session['raw'])
        begin = select_boundary(rows, settings['entry_tick'])
        game = [r for r in rows[begin:] if r['pc'] == PROFILE['game_begin']]
        segments = []
        for row in game:
            if not segments or row['stage'] != segments[-1]['stage'] or row['game_tick'] != segments[-1]['end_tick']+1:
                segments.append(dict(stage=row['stage'], start_phase=row['ordinal'],
                                     start_tick=row['game_tick'], end_tick=row['game_tick']))
            else:
                segments[-1]['end_tick'] = row['game_tick']
            segments[-1]['end_phase'] = row['ordinal']+1
        scope_end=segments[-1]['end_phase']
        scoped_rows=rows[:scope_end]
        calls, count = input_calls(session['raw'], scoped_rows)
        for segment in segments:
            terminal = rows[segment['end_phase']-1]
            segment['input_calls_end'] = input_call_cursor(session['raw'], rows, terminal['emulated_ticks'])
        outputs = {'input_calls': directory/'input-calls.bin', 'jobs': directory/'menu.jobs.bin',
                   'pad': directory/'neutral.pad.bin', 'boundaries': directory/'boundaries.json'}
        outputs['input_calls'].write_bytes(calls)
        outputs['jobs'].write_bytes(menu_jobs(rows, dict(pc=rows[0]['pc'], tick=rows[0]['game_tick']), cross_phase=True))
        outputs['pad'].write_bytes(b'')
        for ordinal,segment in enumerate(segments):
            key='pad_segment_'+str(ordinal)
            outputs[key]=directory/('segment-'+str(ordinal)+'.pad.bin')
            outputs[key].write_bytes(stage_pad_schedule(rows,segment))
        write_receipt(outputs['boundaries'], [{k:r[k] for k in ('ordinal','pc','stage','game_tick','emulated_ticks')} for r in rows])
        from trace_stage_channels import extract
        channel_metadata={}
        channels=extract(session['raw'],begin,scope_end,directory,channel_metadata)
        outputs.update({'original_'+k:v for k,v in channels.items()})
        input_rows=json.loads(channels['inputs'].read_text())
        input_cursor=0
        for ordinal,segment in enumerate(segments):
            phase_count=segment['end_phase']-segment['start_phase']
            key='decode_pad_segment_'+str(ordinal)
            outputs[key]=directory/('segment-'+str(ordinal)+'.decode-pad.bin')
            outputs[key].write_bytes(decode_pad_schedule(input_rows[input_cursor:input_cursor+phase_count],
                segment['start_tick'],segment['end_tick']))
            input_cursor+=phase_count
        if input_cursor!=len(input_rows):raise ValueError('Decoded input records exceed gameplay segments')
        index_dir = directory/'phase-index'
        # An incomplete index is never a cache hit
        if index_dir.exists():
            # The preparation lock proves no index builder still owns this directory
            attempt=1
            while (directory/('interrupted-phase-index-'+str(attempt))).exists():attempt+=1
            if attempt>2:raise ValueError('Preparation recovery attempt limit exhausted')
            index_dir.replace(directory/('interrupted-phase-index-'+str(attempt)))
        build_index(session['raw'], index_dir, session['raw_sha256'])
        outputs['phase_index'] = index_dir/'index.json'
        manifest = dict(schema=1, identity=identity, trace=name, original=session['raw'],
            session=str(session_path), phase_begin=begin, phase_end=scope_end, excluded_prefix=[0,begin],
            excluded_suffix=[scope_end,len(rows)],
            stage=rows[begin]['stage'], pc=rows[begin]['pc'], tick=rows[begin]['game_tick'],
            segments=segments, input_calls_end=count, terminal_gameplay_boundary_only=True,
            original_terminal_input_records=channel_metadata['terminal_input_records'],
            artifacts={k:dict(path=str(p),sha256=digest(p)) for k,p in outputs.items()},
            timings=dict(hash_seconds=hash_seconds, prepare_seconds=time.perf_counter()-started))
        write_receipt(manifest_path, manifest)
        return dict(manifest=str(manifest_path), cached=False, seconds=time.perf_counter()-started,
                    phase_begin=begin, segments=len(segments))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', required=True)
    args = parser.parse_args()
    print(json.dumps(prepare(args.name)))


if __name__ == '__main__':
    main()

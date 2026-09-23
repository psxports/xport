"""Replay recordings from a menu anchor through menu, cutscene and gameplay phases"""
from xport_project import load_project, project_path
from trace_layout import PROFILE, INPUT_CALL_BYTES
import re
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

from trace_cache import digest
from trace_phases import read_phases
from trace_runner import database, execute_job, refresh_reports
from trace_evidence import export_coverage
from trace_worker import write_receipt
from trace_phase_checkpoint import context as checkpoint_context, select as select_phase_checkpoint, verified_end

ROOT, PROJECT = load_project()


def input_calls(source, rows):
    """Keep decoder calls inside complete phase intervals, excluding capture edges"""
    data = bytearray(struct.pack('<I', 0x31494646))
    count = 0
    with Path(source).open('rb') as stream:
        stream.read(12)
        while envelope := stream.read(12):
            if len(envelope) != 12:
                raise ValueError('Truncated input envelope')
            kind, tick, length = struct.unpack('<3I', envelope)
            if kind != 16:
                stream.seek(length, 1)
                continue
            payload = stream.read(length)
            if length != INPUT_CALL_BYTES or len(payload) != length:
                raise ValueError('Invalid input call record')
            controller, pad, frame, low, high, caller = struct.unpack_from('<6I', payload)
            if controller not in (0, 1) or pad != (PROFILE['pad_first'], PROFILE['pad_second'])[controller]:
                raise ValueError('Invalid input controller/address')
            clock = low | high << 32
            if rows[0]['emulated_ticks'] <= clock < rows[-1]['emulated_ticks']:
                data.extend(struct.pack('<2I', tick, controller) + payload[24:])
                count += 1
    if len(rows) < 2 or not count:
        raise ValueError('Recording lacks per-call input evidence for complete intervals')
    return bytes(data), count


def input_call_cursor(source, rows, clock_limit):
    """Return the replay cursor immediately before a recorded phase boundary"""
    count = 0
    with Path(source).open('rb') as stream:
        stream.read(12)
        while envelope := stream.read(12):
            if len(envelope) != 12:
                raise ValueError('Truncated input envelope')
            kind, tick, length = struct.unpack('<3I', envelope)
            if kind != 16:
                stream.seek(length, 1)
                continue
            payload = stream.read(length)
            if length != INPUT_CALL_BYTES or len(payload) != length:
                raise ValueError('Invalid input call record')
            controller, pad, frame, low, high, caller = struct.unpack_from('<6I', payload)
            if controller not in (0, 1) or pad != (PROFILE['pad_first'], PROFILE['pad_second'])[controller]:
                raise ValueError('Invalid input controller/address')
            clock = low | high << 32
            if rows[0]['emulated_ticks'] <= clock < clock_limit:
                count += 1
    return count


def decode_pad_schedule(records, start_tick, end_tick):
    """Encode one recorded decode-time pad value for every gameplay tick"""
    if type(start_tick) is not int or type(end_tick) is not int or start_tick > end_tick:
        raise ValueError('Invalid decode pad interval')
    if len(records) != end_tick-start_tick+1:
        raise ValueError('Decode pad interval is incomplete')
    ranges=[]
    for index,row in enumerate(records):
        tick=start_tick+index
        if (not isinstance(row,list) or len(row)!=3 or row[0]!=tick or
                any(type(value) is not int for value in row) or not 0<=row[1]<=0xffff):
            raise ValueError('Invalid decode-time pad record')
        value=row[1]
        if ranges and ranges[-1][2]==value:
            ranges[-1][1]=tick
        else:ranges.append([tick,tick,value])
    return b''.join(struct.pack('<3I',*row) for row in ranges)


def menu_jobs(rows, anchor, cross_phase=False):
    if not rows or rows[0]['pc'] != anchor['pc'] or rows[0]['game_tick'] != anchor['tick']:
        raise ValueError('Menu recording does not start at the registered anchor')
    data = bytearray()
    for index, row in enumerate(rows):
        if cross_phase and row['pc'] != PROFILE['menu_phase']:
            continue
        if not cross_phase and (row['pc'] != PROFILE['menu_phase'] or row['game_tick'] != anchor['tick']+index):
            raise ValueError('Cross-phase input adapter is required for this recording')
        pad1, pad2 = struct.unpack_from('<2I', row['state'], 8)
        connected = [(pad & 255) == 0 for pad in (pad1, pad2)]
        if connected == [False, True]:
            raise ValueError('Only the second controller is connected; adapter cannot represent this state')
        for pad, present in zip((pad1, pad2), connected):
            if present and (pad >> 8) & 255 != 0x41:
                raise ValueError('Unsupported controller packet type')
        data.extend(struct.pack('<3I', sum(connected), (pad1 >> 16) ^ 65535, (pad2 >> 16) ^ 65535))
    return bytes(data)


def capture(config_path):
    config = json.loads(Path(config_path).read_text())
    executable = project_path('native_executable')
    process_name = executable.stem.replace("'", "''")
    guard = subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-Command',
                            "if (Get-Process -Name '"+process_name+"' -ErrorAction SilentlyContinue) { exit 3 }"], capture_output=True)
    if guard.returncode:
        raise RuntimeError('Native executable is already running; preserve it')
    prefix = PROJECT['native_trace']['environment_prefix']
    if not re.fullmatch('[A-Z][A-Z0-9_]*_', prefix):
        raise ValueError('Invalid native environment prefix')
    env = {k.upper():v for k,v in os.environ.items() if not k.upper().startswith(prefix)}
    options = dict(AUDIT_PAD_SCHEDULE=config['pad'], MENU_CHECKPOINT_LOAD=config['checkpoint'],
                   TRACE_PHASE_PATH=config['output'], AUDIO_OUTPUT='0', RASTERIZE='0', VERIFY_VRAM='0', CAPTURE_WIP='1')
    if config.get('decode_pad'):
        options['AUDIT_DECODE_PAD_SCHEDULE'] = config['decode_pad']
    if config.get('phase_restore'):
        options.pop('MENU_CHECKPOINT_LOAD')
        options['PHASE_CHECKPOINT_LOAD'] = config['checkpoint']
    if config.get('checkpoint_directory'):
        options.update(PHASE_CHECKPOINT_DIRECTORY=config['checkpoint_directory'],
                       PHASE_CHECKPOINT_INTERVAL=str(config.get('checkpoint_interval',300)))
    if 'input_calls' in config:
        options.update(TRACE_INPUT_CALLS=config['input_calls'], MENU_PHASE_COUNT=str(config['phase_count']),
                       AUDIT_END_TICK=str(config.get('end_tick',900)))
    if 'input_calls_end' in config:
        options['TRACE_INPUT_CALLS_END'] = str(config['input_calls_end'])
    if config.get('phase_aligned'):
        options['AUDIT_PHASE_ALIGNED'] = '1'
    if config.get('world'):
        options['TRACE_WORLD'] = '1'
    if config.get('audit_output'):
        options['AUDIT_OUTPUT'] = config['audit_output']
    env.update({prefix+key:value for key,value in options.items()})
    arguments = [value.format_map(config) for value in PROJECT['native_trace']['phase_arguments']]
    if PROJECT['native_trace'].get('wip_log_argument'):
        arguments += [PROJECT['native_trace']['wip_log_argument'], str(Path(config_path).parent/'wip.jsonl')]
    from pipeline_metrics import span, child_environment, process_counters
    with span('native_capture') as metrics:
        with subprocess.Popen([str(executable), *arguments],
                cwd=project_path('native_working_directory','bin'),env=child_environment(env),
                creationflags=subprocess.CREATE_NO_WINDOW,stdout=subprocess.PIPE,stderr=subprocess.STDOUT) as child:
            output,_=child.communicate()
            metrics.update(process_counters(child._handle),pid=child.pid,output_bytes=len(output),exit_code=child.returncode)
            result=subprocess.CompletedProcess(child.args,child.returncode,output)
    log = result.stdout.decode(errors='replace')
    print(log, end='')
    if result.returncode:
        from trace_failures import classify_native, EXIT_CODES
        failure = classify_native(result.returncode, log)
        write_receipt(Path(config_path).parent/'capture-failure.json', failure)
        return EXIT_CODES[failure['outcome']]
    return 0


def replay(args, session, anchor):
    if args.from_tick is not None:
        raise ValueError('Use --from-phase for a recording with resetting game counters')
    if (args.resume_index is None) != (args.from_phase is None):
        raise ValueError('Phase seek requires --resume-index and --from-phase together')
    if session['status'] != 'complete' or session['state_sha256'] != anchor['original_state_sha256']:
        raise ValueError('Recording is incomplete or uses another initial state')
    if digest(project_path('native_executable')) != anchor['exe_sha256'] or digest(anchor['checkpoint']) != anchor['checkpoint_sha256']:
        raise ValueError('Native menu checkpoint is incompatible')
    if digest(session['raw']) != session['raw_sha256']:
        raise ValueError('Raw recording changed')
    selected, index = None, None
    if args.resume_index:
        index = json.loads(args.resume_index.read_text())
        selected = select_phase_checkpoint(index, session['raw_sha256'], anchor['exe_sha256'], args.from_phase)
    request = {'anchor':anchor, 'raw_sha256':session['raw_sha256'], 'selected':selected,
               'resume_index_sha256':digest(args.resume_index) if args.resume_index else None}
    folder = ROOT/'status/user-traces'/args.name/'replays'/args.label
    if not args.continue_run:
        folder.mkdir(parents=True, exist_ok=False)
        rows = read_phases(session['raw'])
        begin = selected['ordinal'] if selected else 0
        if selected and rows[begin]['pc'] != selected['pc']:
            raise ValueError('Checkpoint PC does not match original phase')
        jobs = folder/'menu.jobs.bin'; jobs.write_bytes(menu_jobs(rows, anchor, cross_phase=True))
        pad = folder/'neutral.pad.bin'; pad.write_bytes(b'')
        config = {'checkpoint': anchor['checkpoint'], 'jobs': str(jobs), 'pad': str(pad),
                  'output': str(folder/'native.phases')}
        calls, count = input_calls(session['raw'], rows)
        call_path = folder/'input-calls.bin'; call_path.write_bytes(calls)
        config.update(input_calls=str(call_path), input_call_count=count, phase_count=len(rows),
                      end_tick=max([900]+[r['game_tick']+1 for r in rows if r['pc'] == PROFILE['game_begin']]))
        if selected:
            if digest(call_path) != index['input_calls_sha256'] or digest(jobs) != index['menu_jobs_sha256']:
                raise ValueError('Checkpoint input history differs')
            config.update(checkpoint=selected['checkpoint'], phase_restore=True, phase_count=len(rows)-begin)
        checkpoint_folder = folder/'native-checkpoints'; checkpoint_folder.mkdir()
        config.update(checkpoint_directory=str(checkpoint_folder), checkpoint_interval=args.checkpoint_interval)
        write_receipt(folder/'capture.json', config)
        write_receipt(folder/'request.json', request)
        spec = {'comparison_kind': 'phases', 'phase_sound': True, 'start_tick': begin, 'phase_start':begin, 'end_tick': len(rows),
                'phase_gpu': all(row.get('gpu_present') for row in rows),
                'failure_exit_codes': {'native': {'20': 'game_failure', '21': 'native_crash',
                                                '22': 'infrastructure_error', '23': 'unclassified_failure'}},
                'original': {'phases': session['raw']}, 'native': {'phases': config['output']},
                'commands': {'native': [sys.executable, str(Path(__file__).resolve()), '--capture', str(folder/'capture.json')]},
                'dependencies': {'exe': str(project_path('native_executable')), 'checkpoint': config['checkpoint'],
                    'source': session['raw'], 'jobs': str(jobs), 'pad': str(pad),
                    'input_calls': str(call_path),
                    'checkpoint_adapter': str(Path(__file__).with_name('trace_phase_checkpoint.py')),
                    'config': str(folder/'capture.json'), 'request': str(folder/'request.json'),
                    'adapter': str(Path(__file__).resolve())}}
        if selected:
            spec['dependencies']['checkpoint_context'] = selected['context']
            spec['dependencies']['checkpoint_index'] = str(args.resume_index.resolve())
        ordinals = sorted({begin, *range(((begin//args.checkpoint_interval)+1)*args.checkpoint_interval, len(rows), args.checkpoint_interval)})
        for ordinal in ordinals:
            path = checkpoint_folder/f'phase-{ordinal:010d}.ffcp'
            spec['native']['checkpoint_'+str(ordinal)] = str(path)
            spec['native']['context_'+str(ordinal)] = str(path)+'.ctx'
        write_receipt(folder/'runner-spec.json', spec)
    else:
        if json.loads((folder/'request.json').read_text()) != request:
            raise ValueError('Persisted menu replay inputs changed')
        spec = json.loads((folder/'runner-spec.json').read_text())
    db_path = ROOT/'status/controls/automation/runs.sqlite'
    with database(db_path) as db:
        result = execute_job(db, 'menu-'+args.label, spec, ROOT, folder/'runner', native_only=True)
        export_coverage(db, db_path.parent/'coverage-export.json')
    write_receipt(folder/'runner-status.json', result)
    if result['outcome'] in ('match', 'mismatch'):
        report = json.loads(Path(result['report']).read_text())
        prefix_end = verified_end(report)
        config = json.loads((folder/'capture.json').read_text())
        checkpoints = []
        for path in sorted(Path(config['checkpoint_directory']).glob('*.ffcp')):
            context_path = Path(str(path)+'.ctx')
            state = checkpoint_context(context_path)
            if state['ordinal'] >= prefix_end:
                continue
            checkpoints.append(dict(state, checkpoint=str(path), checkpoint_sha256=digest(path),
                                    context=str(context_path), context_sha256=digest(context_path)))
        write_receipt(folder/'checkpoint-index.json', {'version':1, 'raw_sha256':session['raw_sha256'],
                      'exe_sha256':anchor['exe_sha256'], 'comparison_report':result['report'],
                      'comparison_sha256':digest(result['report']), 'input_calls_sha256':digest(config['input_calls']),
                      'menu_jobs_sha256':digest(config['jobs']), 'checkpoints':checkpoints})
    refresh_reports(db_path)
    print(json.dumps(result))


if __name__ == '__main__':
    if len(sys.argv) != 3 or sys.argv[1] != '--capture':
        raise SystemExit('Use trace_stage_run for convergence or invoke --capture only through its owned worker')
    raise SystemExit(capture(sys.argv[2]))

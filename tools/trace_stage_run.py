"""Resume a hidden stage capture and strict comparison through a SQLite journal"""
import argparse
import contextlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import time

from pipeline_journal import connect, ensure_run, set_step, step, reserve_attempt
from trace_cache import digest
from trace_stage_plan import prepare, validate_cache
from trace_stage_verify import verify
from trace_phase_checkpoint import context
from trace_worker import write_receipt, process_identity, alive
from trace_finalize_lock import exclusive_finalize
from xport_project import load_project, project_path, artifact_path
from trace_layout import PROFILE
from pipeline_metrics import measured, span
from trace_hash_session import hash_session


def preflight(plan, anchor):
    validate_cache(plan)
    if anchor['raw_sha256'] != plan['identity']['raw_sha256'] or anchor['phase_begin'] != plan['phase_begin']:
        raise ValueError('Anchor boundary belongs to another trace')
    if digest(anchor['checkpoint']) != anchor['checkpoint_sha256']:
        raise ValueError('Anchor checkpoint changed')
    if digest(anchor['context']) != anchor['context_sha256']:
        raise ValueError('Anchor context changed')
    if Path(anchor['context']).resolve() != Path(anchor['checkpoint']+'.ctx').resolve():
        raise ValueError('Native loads context adjacent to its checkpoint')
    state = context(anchor['context'])
    if state['ordinal'] != plan['phase_begin'] or state['pc'] != plan['pc']:
        raise ValueError('Anchor continuation differs from stage boundary')
    calls = Path(plan['artifacts']['input_calls']['path']).read_bytes()
    stride = 8 + PROFILE['pad_size']
    if calls[:4] != b'FFI1' or (len(calls)-4) % stride or (len(calls)-4)//stride != plan['input_calls_end']:
        raise ValueError('Truncated input-call stream')
    if not 0 <= state['input_ordinal'] < plan['input_calls_end']:
        raise ValueError('Anchor input cursor outside full stream')
    for offset in range(4,len(calls),stride):
        if struct.unpack_from('<I',calls,offset+4)[0] not in (0,1):
            raise ValueError('Invalid input controller')
    for key in ('input_calls','jobs'):
        if anchor[key+'_sha256'] != plan['artifacts'][key]['sha256']:
            raise ValueError('Anchor input history differs: '+key)
    jobs = Path(plan['artifacts']['jobs']['path']).stat().st_size
    if jobs % 12 or not 0 <= state['menu_job'] <= jobs//12:
        raise ValueError('Menu job cursor outside stream')
    if not anchor.get('abi') or not anchor.get('adapter_version'):
        raise ValueError('Explicit checkpoint ABI and adapter version required')
    return state


@measured("capture")
def capture_worker(path):
    from user_menu_replay import capture
    request = json.loads(path.read_text())
    folder = path.parent
    receipt_path = folder/'capture-receipt.json'
    receipt = dict(status='running', worker=process_identity(os.getpid()), started=time.time())
    write_receipt(receipt_path, receipt)
    started = time.perf_counter()
    try:
        before = {key:digest(value) for key,value in request['dependencies'].items()}
        if before != request['dependency_hashes']:
            raise ValueError('Capture dependencies changed before launch')
        master = json.loads((folder/'capture.json').read_text())
        configs = master.get('segments')
        if configs is None:
            configs = [str(folder/'capture.json')]
        with (folder/'capture.log').open('w') as log, contextlib.redirect_stdout(log):
            code = 0
            for config in configs:
                code = capture(Path(config))
                if code:
                    break
        if code == 0 and len(configs) > 1:
            merge_segment_capture(folder, configs, request['native'])
        if {key:digest(value) for key,value in request['dependencies'].items()} != before:
            raise ValueError('Capture dependencies changed during execution')
        log = (folder/'capture.log').read_text()
        cursors = re.findall(r'diagnostic_input_calls (\d+)',log)
        checkpoints=[]
        # Retain complete prefix checkpoints after a native failure
        if configs:
            for config_path in configs:
                config=json.loads(Path(config_path).read_text())
                if config.get('checkpoint_directory'):
                    for checkpoint in sorted(Path(config['checkpoint_directory']).glob('phase-*.ffcp')):
                        ctx=Path(str(checkpoint)+'.ctx')
                        if not ctx.is_file():
                            receipt.setdefault('incomplete_checkpoints',[]).append(str(checkpoint))
                            continue
                        state=context(ctx)
                        checkpoints.append(dict(checkpoint=str(checkpoint),checkpoint_sha256=digest(checkpoint),
                            context=str(ctx),context_sha256=digest(ctx),ordinal=state['ordinal'],state=state))
        receipt['checkpoints']=checkpoints
        receipt.update(status='finished', exit_code=code, input_calls_consumed=int(cursors[-1]) if cursors else None,
            input_calls_available=request['input_calls_end'], input_calls_sha256=before['input_calls'],
            exe_sha256=before['exe'], dependency_hashes=before,
            artifact_hashes={k:digest(v) for k,v in request['native'].items()} if code==0 else {})
    except Exception as error:
        receipt.update(status='failed',exit_code=2,error=str(error))
    receipt.update(seconds=time.perf_counter()-started, finished=time.time())
    write_receipt(receipt_path,receipt)
    return receipt['exit_code']


def merge_phase_streams(paths, output):
    header = None
    records = []
    for path in paths:
        data = Path(path).read_bytes()
        if len(data) < 28:
            raise ValueError('Truncated segment phase stream')
        if header is None:
            header = data[:12]
        elif data[:12] != header:
            raise ValueError('Segment phase schema differs')
        offset = 12
        complete = False
        while offset < len(data):
            if offset+12 > len(data):
                raise ValueError('Truncated segment phase envelope')
            kind,tick,length = struct.unpack_from('<3I',data,offset)
            end = offset+12+length
            if end > len(data):
                raise ValueError('Truncated segment phase payload')
            if kind == 5:
                if data[offset+12:end] != struct.pack('<I',1) or end != len(data):
                    raise ValueError('Invalid segment phase completion')
                complete = True
            else:
                records.append(data[offset:end])
            offset = end
        if not complete:
            raise ValueError('Incomplete segment phase stream')
    Path(output).write_bytes(header+b''.join(records)+struct.pack('<3I',5,0,4)+struct.pack('<I',1))


def merge_segment_capture(folder, configs, native):
    configs = [json.loads(Path(path).read_text()) for path in configs]
    phase_paths = [config['output'] for config in configs]
    merge_phase_streams(phase_paths,native['phases'])
    for channel in ('actors','world','inputs'):
        suffix='.'+channel
        with Path(native[channel]).open('wb') as target:
            for config in configs:
                source=Path(config['audit_output']+suffix)
                with source.open('rb') as stream:
                    while block:=stream.read(1024*1024):target.write(block)


def launch_or_observe(folder, wait_seconds):
    from pipeline_metrics import child_environment
    from trace_stage_recovery import recover
    recovered = recover(folder)
    if recovered is not None:
        return recovered
    receipt_path = folder/'capture-receipt.json'
    launch_path = folder/'launch.json'
    if not receipt_path.exists():
        if launch_path.exists():
            launch = json.loads(launch_path.read_text())
            if not alive(launch.get('worker')):
                raise RuntimeError('Worker stopped without receipt; explicit recovery required')
        else:
            # Persist launching before spawn so an interrupted launch never duplicates a child
            write_receipt(launch_path, {'status':'launching','owner':process_identity(os.getpid())})
            with (folder/'worker.log').open('wb') as log:
                child = subprocess.Popen([sys.executable,'-B',str(Path(__file__).resolve()),'--capture',str(folder/'request.json')],
                    cwd=project_path('native_working_directory'),stdout=log,stderr=subprocess.STDOUT,env=child_environment(),
                    creationflags=subprocess.CREATE_NO_WINDOW)
            write_receipt(launch_path, {'status':'started','worker':process_identity(child.pid)})
    deadline = time.monotonic()+wait_seconds
    while True:
        receipt = json.loads(receipt_path.read_text()) if receipt_path.exists() else None
        if receipt and receipt['status'] in ('finished','failed'):
            return receipt
        if receipt and not alive(receipt.get('worker')):
            raise RuntimeError('Capture worker disappeared; preserve artifacts and recover explicitly')
        if time.monotonic() >= deadline:
            return dict(status='running', receipt=str(receipt_path))
        time.sleep(.1)


def run(args):
    root, project = load_project()
    work = artifact_path('status/stage-pipeline')
    work.mkdir(parents=True,exist_ok=True)
    with exclusive_finalize(work), contextlib.closing(connect(work/'runs.sqlite')) as db:
        from trace_stage_cleanup import prune
        for row in db.execute("SELECT run_id FROM steps WHERE name='cleanup' AND status='pruning'").fetchall():
            prune(db,row['run_id'],work)
        from trace_stage_finalize import diagnose
        for row in db.execute("SELECT run_id,receipt FROM steps WHERE name='diagnostic' AND status IN ('planned','running')").fetchall():
            prior_diagnostic=json.loads(row[1])
            owner=db.execute('SELECT request FROM runs WHERE id=?',(row[0],)).fetchone()
            if json.loads(owner[0]).get('trace')!=args.name:continue
            result=diagnose(db,row[0],Path(prior_diagnostic['folder']),args.wait)
            return dict(status='mismatch',diagnostic=result)
        # Do not build or launch while a previous owned capture remains live
        pending = db.execute("SELECT receipt FROM steps WHERE name='capture' AND status='running'").fetchall()
        for row in pending:
            previous = json.loads(row[0])
            path = Path(previous['folder'])/'capture-receipt.json'
            if path.exists():
                current = json.loads(path.read_text())
                if current['status']=='running' and alive(current.get('worker')):
                    return dict(status='running',folder=previous['folder'])
        prepared = prepare(args.name)
        plan = json.loads(Path(prepared['manifest']).read_text())
        build_started = time.perf_counter()
        with span('build'):
            build = subprocess.run([sys.executable,'-B',str(Path(__file__).with_name('build_native.py'))],capture_output=True,text=True)
        (work/'build-driver.log').write_text(build.stdout+build.stderr)
        if build.returncode:
            raise RuntimeError('Native build failed; see build-driver.log')
        build_seconds = time.perf_counter()-build_started
        from trace_stage_registry import resolve, accept, key
        registry = work/'anchors'
        if args.anchor:
            selected_anchor = json.loads(args.anchor.read_text())
        else:
            stage_settings=dict(project.get('stage_pipeline',{}))
            if stage_settings.get('adapter'):
                stage_settings['adapter_sha256']=digest(root/stage_settings['adapter'])
            selected = resolve(plan,stage_settings,registry)
            selected_anchor = selected[1] if selected else None
        from trace_stage_convert import inputs as conversion_inputs, convert as convert_stage
        conversion=conversion_inputs(root,project,plan)
        request_id = dict(trace=args.name,manifest_sha256=digest(prepared['manifest']),
                         exe_sha256=digest(project_path('native_executable')),
                         project_sha256=digest(root/'xport-project.json'),
                         runner_sha256=digest(__file__), verifier_sha256=digest(Path(__file__).with_name('trace_stage_verify.py')),
                         collector_sha256=digest(Path(__file__).with_name('user_menu_replay.py')),
                         capture_runtime_sha256={name:digest(Path(__file__).with_name(name+'.py')) for name in
                           ('trace_runner','trace_comparison','trace_bundle','trace_actor_diff','trace_evidence',
                            'trace_worker','trace_schedule','trace_cache','trace_hash_session','gpu_packet_semantics','trace_phases','trace_phase_gpu')},
                         registry_sha256=digest(Path(__file__).with_name('trace_stage_registry.py')),
                         diagnostic_sha256=digest(Path(__file__).with_name('trace_stage_narrow.py')),
                         finalize_sha256=digest(Path(__file__).with_name('trace_stage_finalize.py')),
                         cleanup_sha256=digest(Path(__file__).with_name('trace_stage_cleanup.py')),
                         recovery_sha256=digest(Path(__file__).with_name('trace_stage_recovery.py')),
                         difference_sha256=digest(Path(__file__).with_name('trace_stage_difference.py')),
                         packet_sha256=digest(Path(__file__).with_name('trace_diagnostic_packet.py')),
                         converter_inputs=conversion,converter_sha256=digest(Path(__file__).with_name('trace_stage_convert.py')),
                         anchor_sha256=key(selected_anchor) if selected_anchor and (args.anchor or conversion is None) else None)
        verification_names=('trace_stage_verify','trace_stage_difference','trace_phase_index','trace_actor_diff','trace_bundle','trace_phases','gpu_packet_semantics')
        verification_identity={name:digest(Path(__file__).with_name(name+'.py')) for name in verification_names}
        publication_identity={name:digest(Path(__file__).with_name(name+'.py')) for name in ('trace_stage_registry','render_progress','trace_stage_cleanup')}
        request_id={k:v for k,v in request_id.items() if k not in ('verifier_sha256','registry_sha256','diagnostic_sha256','finalize_sha256','cleanup_sha256','recovery_sha256','difference_sha256','packet_sha256')}
        request_id['checkpoint_abi']=project.get('stage_pipeline',{}).get('native_abi')
        run_id = ensure_run(db,request_id)
        os.environ['XPORT_RUN_ID']=run_id
        prior = step(db,run_id,'compare')
        cleanup=step(db,run_id,'cleanup')
        if cleanup and cleanup['status']=='pruned' and prior and prior['receipt'].get('verification_identity')!=verification_identity:
            request_id['revalidation']=verification_identity
            run_id=ensure_run(db,request_id)
            os.environ['XPORT_RUN_ID']=run_id
            prior=step(db,run_id,'compare')
        if prior and prior['status'] in ('match','mismatch') and prior['receipt'].get('verification_identity')==verification_identity:
            cleanup=step(db,run_id,'cleanup')
            if cleanup:
                if cleanup['status'] in ('pruning','pruned'):return prune(db,run_id,work)
                raise ValueError('Invalid persisted cleanup state')
            report_path = Path(prior['receipt']['report'])
            if digest(report_path) != prior['receipt']['sha256']:
                raise ValueError('Persisted comparison report changed')
            for name,sha in prior['receipt']['sidecars'].items():
                if digest(report_path.parent/name)!=sha:
                    raise ValueError('Persisted comparison sidecar changed: '+name)
            report = json.loads(report_path.read_text())
            for side in ('original','native'):
                spec = json.loads((report_path.parent/'manifest.json').read_text())
                if any(digest(path)!=report['hashes'][side][key] for key,path in spec[side].items()):
                    raise ValueError('Persisted comparison input changed')
            diagnostic=None
            if prior['status']=='mismatch':
                diagnostic=diagnose(db,run_id,report_path.parent,args.wait)
            if prior['status']=='match':
                accepted_step=step(db,run_id,'continuation')
                if not accepted_step or accepted_step['receipt'].get('publication_identity')!=publication_identity:
                    saved=step(db,run_id,'anchor')['receipt']
                    with span('acceptance'):
                        accepted=accept(plan,saved.get('anchors',[saved['anchor']]),report_path.parent,registry)
                    set_step(db,run_id,'continuation','accepted',dict(entry=str(accepted),sha256=digest(accepted),publication_identity=publication_identity))
            publication = step(db,run_id,'publish')
            if publication is None or publication['status'] == 'publication_failed' or publication['receipt'].get('publication_identity')!=publication_identity:
                with span('publication'), (report_path.parent/'progress.log').open('w') as log:
                    refreshed = subprocess.run([sys.executable,'-B',str(Path(__file__).with_name('render_progress.py'))],stdout=log,stderr=subprocess.STDOUT)
                state = prior['status'] if refreshed.returncode==0 else 'publication_failed'
                set_step(db,run_id,'publish',state,{'report':str(report_path),'publication_identity':publication_identity})
                if state=='publication_failed':
                    return dict(status=state,report=str(report_path))
            if prior['status']=='match' and project.get('stage_pipeline',{}).get('prune_successful_runs') is True:
                return prune(db,run_id,work,report_path.parent)
            return dict(status=prior['status'],cached=True,report=str(report_path),diagnostic=diagnostic)
        set_step(db,run_id,'build','built',{'exe_sha256':request_id['exe_sha256']},build_seconds)
        set_step(db,run_id,'prepare','prepared',prepared,prepared['seconds'])
        anchor_step = step(db,run_id,'anchor')
        if not anchor_step:
            settings = project.get('stage_pipeline',{})
            budget = reserve_attempt(db,run_id,'anchor',settings.get('anchor_attempt_limit',2),settings.get('anchor_seconds_limit',60))
            if not budget['allowed']:
                set_step(db,run_id,'anchor','ANCHOR_UNSUPPORTED',budget)
                return dict(status='ANCHOR_UNSUPPORTED',reason='No registered stage adapter candidate',budget=budget)
            started = time.perf_counter()
            anchor = selected_anchor if len(plan['segments']) == 1 else None
            anchors=[];states=[];conversions=[]
            if anchor is None:
                for ordinal,segment in enumerate(plan['segments']):
                    segment_plan=dict(plan)
                    segment_plan.update(phase_begin=segment['start_phase'],phase_end=segment['end_phase'],
                                        stage=segment['stage'],tick=segment['start_tick'],pc=PROFILE['game_begin'])
                    segment_plan_path=work/'conversions'/run_id/('segment-'+str(ordinal)+'-plan.json')
                    segment_plan_path.parent.mkdir(parents=True,exist_ok=True)
                    write_receipt(segment_plan_path,segment_plan)
                    remaining=budget['remaining_seconds']-(time.perf_counter()-started)
                    converted=convert_stage(conversion,segment_plan_path,work/'conversions'/run_id/('segment-'+str(ordinal)),remaining)
                    conversions.append(converted)
                    if converted['status']!='candidate':
                        set_step(db,run_id,'convert',converted['status'],{'segments':conversions},time.perf_counter()-started)
                        set_step(db,run_id,'anchor','ANCHOR_UNSUPPORTED',converted)
                        return dict(status='ANCHOR_UNSUPPORTED',reason=converted['reason'])
                    anchors.append(converted['anchor']);states.append(preflight(segment_plan,converted['anchor']))
                set_step(db,run_id,'convert','candidate',{'segments':conversions},time.perf_counter()-started)
                anchor=anchors[0]
            else:
                anchors=[anchor];states=[preflight(plan,anchor)]
            state = states[0]
            if time.perf_counter()-started > budget['remaining_seconds']:
                set_step(db,run_id,'anchor','ANCHOR_UNSUPPORTED',{'reason':'time budget exceeded'})
                return dict(status='ANCHOR_UNSUPPORTED',reason='Anchor time budget exceeded')
            set_step(db,run_id,'anchor','candidate',{'anchor':anchor,'anchors':anchors,'contexts':states,'context':state},time.perf_counter()-started)
        else:
            if anchor_step['status'] != 'candidate':
                return dict(status=anchor_step['status'],reason='Persisted anchor failure')
            anchor = anchor_step['receipt']['anchor']
            anchors=anchor_step['receipt'].get('anchors',[anchor])
            if len(anchors)!=len(plan['segments']):
                raise ValueError('Persisted anchor count differs from stage segments')
            for segment,item in zip(plan['segments'],anchors):
                segment_plan=dict(plan)
                segment_plan.update(phase_begin=segment['start_phase'],phase_end=segment['end_phase'],
                                    stage=segment['stage'],tick=segment['start_tick'],pc=PROFILE['game_begin'])
                preflight(segment_plan,item)
        folder = work/'runs'/run_id
        folder.mkdir(parents=True,exist_ok=True)
        if not (folder/'request.json').exists():
            native = {k:str(folder/('native-state.'+k)) for k in ('actors','world','inputs')}
            native['phases']=str(folder/'native.phases')
            artifacts = {k:v['path'] for k,v in plan['artifacts'].items()}
            if len(plan['segments'])==1:
                segment = plan['segments'][0]
                config = dict(checkpoint=anchor['checkpoint'],phase_restore=True,phase_aligned=True,jobs=artifacts['jobs'],pad=artifacts['pad_segment_0'],decode_pad=artifacts['decode_pad_segment_0'],
                    input_calls=artifacts['input_calls'],output=native['phases'],phase_count=segment['end_phase']-segment['start_phase'],
                    input_calls_end=segment['input_calls_end'],
                    end_tick=segment['end_tick']+1,world=True,audit_output=str(folder/'native-state'),
                    checkpoint_directory=str(folder/'native-checkpoints'),checkpoint_interval=300)
                (folder/'native-checkpoints').mkdir(exist_ok=True)
                write_receipt(folder/'capture.json',config)
                config_paths=[str(folder/'capture.json')]
            else:
                config_paths=[]
                for ordinal,(segment,item) in enumerate(zip(plan['segments'],anchors)):
                    prefix=folder/('segment-'+str(ordinal));checkpoints=prefix/'native-checkpoints';checkpoints.mkdir(parents=True,exist_ok=True)
                    segment_pad=artifacts['pad_segment_'+str(ordinal)]
                    config=dict(checkpoint=item['checkpoint'],phase_restore=True,phase_aligned=True,jobs=artifacts['jobs'],pad=segment_pad,decode_pad=artifacts['decode_pad_segment_'+str(ordinal)],
                        input_calls=artifacts['input_calls'],output=str(prefix/'native.phases'),phase_count=segment['end_phase']-segment['start_phase'],
                        input_calls_end=segment['input_calls_end'],end_tick=segment['end_tick']+1,world=True,audit_output=str(prefix/'native-state'),
                        checkpoint_directory=str(checkpoints),checkpoint_interval=300)
                    config_path=prefix/'capture.json';prefix.mkdir(parents=True,exist_ok=True);write_receipt(config_path,config);config_paths.append(str(config_path))
                write_receipt(folder/'capture.json',{'schema':1,'segments':config_paths})
            source_session=json.loads(Path(plan['session']).read_text())
            decoded=Path(source_session['decoded_directory'])
            deps = dict(exe=str(project_path('native_executable')),
                        input_calls=artifacts['input_calls'],jobs=artifacts['jobs'],pad=artifacts['pad'],config=str(folder/'capture.json'),
                        project=str(root/'xport-project.json'),collector=str(Path(__file__).with_name('user_menu_replay.py')),
                        runner=str(Path(__file__).resolve()),phase_metadata=str(decoded/'phase-boundaries.json'),
                        input_metadata=str(decoded/'game-input-calls.json'))
            for ordinal,(item,config_path) in enumerate(zip(anchors,config_paths)):
                deps['checkpoint_'+str(ordinal)]=item['checkpoint'];deps['context_'+str(ordinal)]=item['context'];deps['config_'+str(ordinal)]=config_path
                deps['pad_'+str(ordinal)]=artifacts.get('pad_segment_'+str(ordinal),artifacts['pad'])
                deps['decode_pad_'+str(ordinal)]=artifacts['decode_pad_segment_'+str(ordinal)]
            for name in ('trace_runner','trace_comparison','trace_bundle','trace_actor_diff','trace_evidence',
                         'trace_worker','trace_schedule','trace_cache','gpu_packet_semantics','trace_phases','trace_phase_gpu'):
                deps['capture_runtime_'+name]=str(Path(__file__).with_name(name+'.py'))
            write_receipt(folder/'request.json',dict(trace=plan['trace'],dependencies=deps,dependency_hashes={k:digest(v) for k,v in deps.items()},
                          native=native,input_calls_end=plan['input_calls_end']))
            session = json.loads(Path(plan['session']).read_text())
            original = {k:artifacts['original_'+k] for k in ('actors','world','inputs')}
            original['phases']=plan['original']
            write_receipt(folder/'manifest.json',dict(original=original,native=native,receipt=str(folder/'capture-receipt.json'),
                input_calls=artifacts['input_calls'],phase_index=artifacts['phase_index'],raw_sha256=plan['identity']['raw_sha256'],
                contract=dict(schema=2,phase_begin=plan['phase_begin'],phase_end=plan['phase_end'],
                    segments=plan['segments'],independent_stage_segments=len(plan['segments'])>1,
                    terminal_gameplay_boundary_only=plan['terminal_gameplay_boundary_only'],
                    original_terminal_input_records=plan['original_terminal_input_records'],input_calls_end=plan['input_calls_end'])))
        probe_step=step(db,run_id,'fix_probe')
        if not probe_step or probe_step['status']=='running':
            from trace_fix_probe import probe
            with span('fix_probe'):
                probed=probe(db,run_id,folder,plan,args.wait)
            set_step(db,run_id,'fix_probe',probed['status'],probed)
            if probed['status']=='running':return dict(status='running',folder=str(folder),phase='fix_probe')
        set_step(db,run_id,'capture','running',{'folder':str(folder)})
        receipt = launch_or_observe(folder,args.wait)
        if receipt['status']=='running':
            return dict(status='running',folder=str(folder))
        if receipt.get('exit_code') != 0:
            set_step(db,run_id,'capture','capture_failed',receipt,receipt.get('seconds',0))
            from trace_failure_packet import failure_packet
            detail=failure_packet(folder)
            if detail.get('first_difference'):
                replay=diagnose(db,run_id,folder,args.wait)
                detail.update(status=replay['status'],replay=replay)
            return dict(status='capture_failed',folder=str(folder),diagnostic=detail)
        set_step(db,run_id,'capture','captured',{'folder':str(folder)},receipt['seconds'])
        started = time.perf_counter()
        report = verify(json.loads((folder/'manifest.json').read_text()))
        write_receipt(folder/'comparison.json',report)
        status = 'match' if report['passed'] else 'mismatch'
        if not report['passed']:
            from trace_diagnostic_packet import packet
            diagnostic = packet(folder/'comparison.json')
            diagnostic.update(manifest=str(folder/'manifest.json'),capture_receipt=str(folder/'capture-receipt.json'))
            write_receipt(folder/'diagnostic-packet.json',diagnostic)
            set_step(db,run_id,'localize','localized',{'packet':str(folder/'diagnostic-packet.json'),'sha256':digest(folder/'diagnostic-packet.json')})
        changed = set_step(db,run_id,'compare',status,{'report':str(folder/'comparison.json'),
                            'sha256':digest(folder/'comparison.json'),'verification_identity':verification_identity,
                            'sidecars':{name:digest(folder/name) for name in ('manifest.json','request.json','capture-receipt.json')}},time.perf_counter()-started)
        if status=='match':
            with span('acceptance'):
                accepted=accept(plan,anchors,folder,registry)
            set_step(db,run_id,'continuation','accepted',dict(entry=str(accepted),sha256=digest(accepted),publication_identity=publication_identity))
        diagnostic=None
        if status=='mismatch':
            diagnostic=diagnose(db,run_id,folder,args.wait)
        if changed:
            with span('publication'), (folder/'progress.log').open('w') as log:
                refreshed = subprocess.run([sys.executable,'-B',str(Path(__file__).with_name('render_progress.py'))],stdout=log,stderr=subprocess.STDOUT)
            set_step(db,run_id,'publish',status if refreshed.returncode==0 else 'publication_failed',{'report':str(folder/'comparison.json'),'publication_identity':publication_identity})
            if refreshed.returncode:
                return dict(status='publication_failed',report=str(folder/'comparison.json'))
        if status=='match' and project.get('stage_pipeline',{}).get('prune_successful_runs') is True:
            return prune(db,run_id,work,folder)
        return dict(status=status,report=str(folder/'comparison.json'),first_difference=report['first_difference'],diagnostic=diagnostic)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name')
    parser.add_argument('--anchor',type=Path)
    parser.add_argument('--wait',type=float,default=20)
    parser.add_argument('--capture',type=Path)
    parser.add_argument('--inline',action='store_true')
    parser.add_argument('--status',action='store_true')
    args = parser.parse_args()
    if args.capture:
        with hash_session():return capture_worker(args.capture)
    if not args.name or not 0 <= args.wait <= 30:
        parser.error('Require --name and --wait in range 0..30')
    try:
        if not args.inline:
            from trace_converge_worker import control
            result=control(args.name,'status' if args.status else 'start',args.wait,args.anchor)
        else:
            os.environ['XPORT_TRACE_NAME']=args.name
            with hash_session():result=run(args)
    except (ValueError, OSError, RuntimeError) as error:
        result=dict(status='ANCHOR_UNSUPPORTED' if str(error).startswith('ANCHOR_UNSUPPORTED:') else 'invalid_or_interrupted',error=str(error))
    print(json.dumps(result))
    return 0 if result['status'] in ('running','match','MATCH') else 1


if __name__ == '__main__':
    raise SystemExit(main())

"""Run a bounded cross-build diagnostic only; full acceptance always follows"""
import json
from pathlib import Path
from trace_cache import digest
from trace_phase_checkpoint import context
from trace_worker import write_receipt
from xport_project import load_project, project_path


def probe(db, run_id, folder, plan, wait):
    from trace_stage_run import launch_or_observe
    from trace_phase_index import compare_window
    root,project=load_project()
    path=folder/'fix-probe';selection=path/'selection.json'
    if (path/'result.json').exists():return json.loads((path/'result.json').read_text())
    if not selection.exists():
        current=json.loads(db.execute('SELECT request FROM runs WHERE id=?',(run_id,)).fetchone()[0])
        # Cross-build replay requires an explicit project ABI compatibility promise
        if not current.get('checkpoint_abi') or not project.get('stage_pipeline',{}).get('diagnostic_cross_build_checkpoint'):
            return dict(status='skipped',reason='No project ABI compatibility declaration')
        selected=None
        for row in db.execute('SELECT id,request FROM runs WHERE id<>? ORDER BY created DESC',(run_id,)):
            request=json.loads(row[1])
            if request.get('trace')!=plan['trace'] or request.get('exe_sha256')==current.get('exe_sha256'):continue
            if request.get('checkpoint_abi')!=current.get('checkpoint_abi'):continue
            if request.get('converter_inputs')!=current.get('converter_inputs'):continue
            prior=folder.parent/row[0]
            if not all((prior/name).is_file() for name in ('capture-receipt.json','manifest.json','request.json')):continue
            spec=json.loads((prior/'manifest.json').read_text())
            if spec['raw_sha256']!=plan['identity']['raw_sha256']:continue
            packet=prior/'diagnostic-packet.json'
            if not packet.exists():continue
            p=json.loads(packet.read_text());difference=p.get('first_difference') or {}
            target=p.get('ordinal',difference.get('ordinal'))
            if type(target) is not int:continue
            segment=next((s for s in plan['segments'] if s['start_phase']<=target<s['end_phase']),None)
            if segment is None:continue
            receipt=json.loads((prior/'capture-receipt.json').read_text())
            limit=p.get('verified_prefix_end',difference.get('verified_prefix_end'))
            choices=[c for c in receipt.get('checkpoints',[]) if type(limit) is int and
                     segment['start_phase']<=c['ordinal']<min(target,limit)]
            if choices:checkpoint=max(choices,key=lambda c:c['ordinal'])
            else:
                cfg=json.loads((prior/'capture.json').read_text())
                if cfg.get('segments'):cfg=json.loads(Path(cfg['segments'][plan['segments'].index(segment)]).read_text())
                cp=cfg['checkpoint'];ctx=cp+'.ctx';state=context(ctx)
                if state['ordinal']!=segment['start_phase']:continue
                deps=json.loads((prior/'request.json').read_text())
                expected={str(Path(v).resolve()):deps['dependency_hashes'][k] for k,v in deps['dependencies'].items()}
                if expected.get(str(Path(cp).resolve()))!=digest(cp) or expected.get(str(Path(ctx).resolve()))!=digest(ctx):continue
                checkpoint=dict(checkpoint=cp,context=ctx,state=state,ordinal=state['ordinal'],
                    checkpoint_sha256=digest(cp),context_sha256=digest(ctx))
            for field in ('checkpoint','context'):
                if digest(checkpoint[field])!=checkpoint[field+'_sha256']:raise ValueError('Probe checkpoint changed')
            if context(checkpoint['context'])!=checkpoint['state']:raise ValueError('Probe context changed')
            selected=dict(prior=str(prior),checkpoint=checkpoint,target=target,segment=segment,
                          source_exe=receipt['exe_sha256'],abi=current['checkpoint_abi'])
            break
        if selected is None:return dict(status='skipped',reason='No compatible prior failure checkpoint')
        path.mkdir(exist_ok=True);write_receipt(selection,selected)
    selected=json.loads(selection.read_text());checkpoint=selected['checkpoint'];segment=selected['segment']
    for key in ('checkpoint','context'):
        if digest(checkpoint[key])!=checkpoint[key+'_sha256']:raise ValueError('Persisted probe checkpoint changed')
    end=min(segment['end_phase'],selected['target']+3)
    session=json.loads(Path(plan['session']).read_text());decoded=Path(session['decoded_directory'])
    phases=json.loads((decoded/'phase-boundaries.json').read_text());calls=json.loads((decoded/'game-input-calls.json').read_text())
    clock_start=phases[0]['emulated_ticks'];clock_end=phases[end-1]['emulated_ticks']
    cursor=sum(clock_start<=c['emulated_ticks']<clock_end for c in calls)
    ordinal=plan['segments'].index(segment);artifacts=plan['artifacts']
    config=dict(checkpoint=checkpoint['checkpoint'],phase_restore=True,phase_aligned=True,
        jobs=artifacts['jobs']['path'],pad=artifacts['pad_segment_'+str(ordinal)]['path'],
        decode_pad=artifacts['decode_pad_segment_'+str(ordinal)]['path'],
        input_calls=artifacts['input_calls']['path'],output=str(path/'native.phases'),
        phase_count=end-checkpoint['ordinal'],input_calls_end=cursor,end_tick=segment['end_tick']+1,
        world=True,audit_output=str(path/'native-state'))
    if not (path/'request.json').exists():
        write_receipt(path/'capture.json',config)
        dependencies=dict(exe=str(project_path('native_executable')),checkpoint=checkpoint['checkpoint'],
            context=checkpoint['context'],config=str(path/'capture.json'),input_calls=config['input_calls'],
            jobs=config['jobs'],pad=config['pad'],decode_pad=config['decode_pad'])
        for name in ('trace_stage_run','user_menu_replay','trace_phase_checkpoint','trace_fix_probe','trace_prefix','trace_world_index'):
            dependencies[name]=str(Path(__file__).with_name(name+'.py'))
        native={k:str(path/('native-state.'+k)) for k in ('actors','world','inputs')};native['phases']=config['output']
        write_receipt(path/'request.json',dict(dependencies=dependencies,
            dependency_hashes={k:digest(v) for k,v in dependencies.items()},native=native,input_calls_end=cursor))
    receipt=launch_or_observe(path,wait)
    if receipt['status']=='running':return dict(status='running',directory=str(path))
    result=dict(status='diagnostic_failed',full_verification_required=True,source_exe=selected['source_exe'],
                current_exe=digest(project_path('native_executable')),begin=checkpoint['ordinal'],end=end,
                warning='Cross-build state may encode old prefix behavior; this is not acceptance evidence')
    if receipt.get('exit_code')==0:
        if receipt.get('input_calls_consumed')!=cursor:raise ValueError('Probe input cursor mismatch')
        native=json.loads((path/'request.json').read_text())['native']
        for name,channel_path in native.items():
            if digest(channel_path)!=receipt['artifact_hashes'][name]:raise ValueError('Probe channel changed: '+name)
        from trace_prefix import compare_channels
        spec=json.loads((folder/'manifest.json').read_text())
        comparison=compare_channels(spec,native,checkpoint['ordinal'],end,require_complete=True)
        result.update(status='diagnostic_match' if comparison['window_match'] else 'diagnostic_mismatch',comparison=comparison)
    write_receipt(path/'result.json',result)
    return result

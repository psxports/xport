"""Small evidence views and deterministic completion receipts without replay or tests"""
import argparse
import hashlib
import json
from pathlib import Path

from trace_context import bounded
from trace_worker import write_receipt
from xport_project import artifact_path, load_project


def read(path):
    path=Path(path)
    if not path.exists():return None
    if path.stat().st_size>32*1024*1024:raise ValueError('Select a smaller evidence file: '+str(path))
    return json.loads(path.read_text(encoding='utf-8-sig'))


def reference(path):
    path=Path(path)
    return dict(path=str(path),sha256=hashlib.sha256(path.read_bytes()).hexdigest())


def recording(name):
    folder=artifact_path('status/user-traces')/name
    session=read(folder/'session.json')
    if session is None:return dict(status='not_started')
    capture=session.get('capture',{})
    states=capture.get('states',[]);paths=[Path(s['path']) for s in states if s.get('path')]
    available=[p for p in paths if p.is_file()]
    start=read(artifact_path('status/workflow')/('record-'+name+'.json')) or {}
    stop=read(artifact_path('status/workflow')/('stop-'+name+'.json')) or {}
    raw=Path(session['raw']) if session.get('raw') else None
    return dict(status=session['status'],session=reference(folder/'session.json'),
        raw_sha256=session.get('raw_sha256'),raw_bytes=raw.stat().st_size if raw and raw.is_file() else None,
        vblank_storage=capture.get('vblank_storage'),seconds=capture.get('summary',{}).get('seconds'),
        controller_transfers=capture.get('controller_transfers'),missing_vblanks=stop.get('summary',{}).get('missing_vblanks'),
        savestates=dict(recorded=len(states),available=len(available),total_bytes=sum(p.stat().st_size for p in available)),
        capture_latency=read(folder/'decoded/capture-latency.json'),
        startup={k:start.get(k) for k in ('armed_after_seconds','resumed_after_seconds','startup_seconds')},
        visibility=dict(status='not_inferred',reason='Worker show mode is not emulator visibility evidence'),
        scope='VBlank payload and full savestates reported separately; file sizes are observations, not integrity revalidation')


def validation(name):
    link=read(artifact_path('status/toolset-validation')/name/'latest.json')
    if not link:return dict(status='not_started')
    path=Path(link['report']);ref=reference(path)
    if ref['sha256']!=link['sha256']:raise ValueError('Validation report changed')
    value=read(path)
    return dict(status=value['status'],report=ref,seconds=value.get('seconds'),
        suites=[{k:s.get(k) for k in ('exit_code','seconds','reused','log','log_sha256')} for s in value.get('suites',[])],
        error=value.get('error'),identity_sha256=hashlib.sha256(json.dumps(value.get('identity'),sort_keys=True).encode()).hexdigest(),
        scope='Recorded validation result; live identity recheck is performed before replay, not by this view')


def diagnosis(name):
    folder=artifact_path('status/stage-pipeline/workers')/name
    handoff=read(folder/'handoff.json') or {}
    attention=read(folder/'attention.json')
    if attention is not None:
        attention['path']=str(folder/'attention.json')
        return bounded(attention,10000)
    result={k:handoff.get(k) for k in ('status','first_difference','terminal_failure','verified_prefix_end',
        'channel_context','audit','candidate_audits','data_references','diagnostic','repair','error')}
    result['handoff']=str(folder/'handoff.json')
    result['evidence_rule']='Candidate sites are not causal proof; request exact captured words or a bounded observation when live state is missing'
    return bounded(result,10000)


def publish(name, result, finalization, validation_result, repair_state):
    root,config=load_project();folder=artifact_path('status/stage-pipeline/workers')/name
    if not result.get('receipt'):
        return dict(status='not_available',reason='Compact publication requires a published comparison receipt')
    ref=reference(result['receipt'])
    if ref['sha256']!=result.get('receipt_sha256'):raise ValueError('Published MATCH receipt changed')
    accepted=read(ref['path']);comparison=accepted['comparison']
    if comparison.get('passed') is not True or finalization.get('status')!='complete':
        raise ValueError('Completion summary requires published and finalized MATCH')
    metrics=recording(name)
    if metrics.get('raw_sha256')!=accepted['source_raw_sha256']:raise ValueError('Recording identity differs from MATCH')
    attempts=repair_state.get('attempts',[])
    automatic=sum(a.get('status')=='applied_requires_full_replay' for a in attempts)
    receipt=dict(schema=1,status='complete',trace=name,run_id=accepted['run_id'],
        match=ref,scope=comparison['scope'],contract=comparison.get('contract'),
        source_raw_sha256=accepted['source_raw_sha256'],exe_sha256=accepted['request_identity']['exe_sha256'],
        channels={k:{field:v.get(field) for field in ('passed','matched','records','packets','events') if field in v}
                  for k,v in comparison.get('channels',{}).items()},
        finalization=finalization,validation=validation_result,recording=metrics,
        repairs=dict(attempts=len(attempts),automatic_candidates_applied=automatic,solver_calls=repair_state.get('solver_calls',0)),
        telemetry=dict(summary=str(artifact_path('status/telemetry/converge')/(name+'.summary.json')),
                       totals='Watcher closes totals after the final response'),
        evidence_levels=dict(full_match='passed',automatic_repair='candidate_applied_full_match' if automatic else 'not_exercised',
            solver_reasoning='see_solver_result_receipts',visibility='not_inferred',controlled_speedup='not_exercised'))
    target=folder/'completion-summary.json';write_receipt(target,receipt)
    entry=dict(trace=name,run_id=accepted['run_id'],evidence=reference(target),scope=comparison['scope'],
               full_match='passed',validation_status=(validation_result or {}).get('status','not_requested'))
    # Append evidence without promoting historical gates or overwriting reviewed assertions
    from trace_finalize_lock import exclusive_finalize
    with exclusive_finalize(artifact_path('status/toolset-validation')):
        for relative in config.get('stage_pipeline',{}).get('acceptance_manifests',[]):
            path=(root/relative).resolve()
            if not path.is_relative_to(root/'status/toolset-validation'):raise ValueError('Acceptance path outside validation status')
            value=read(path)
            if value is None:raise ValueError('Configured acceptance manifest missing: '+str(path))
            value.setdefault('runtime_cycles',{})[name]=entry
            write_receipt(path,value)
    return dict(status='complete',report=str(target),match=ref,scope=comparison['scope'],
                validation_status=entry['validation_status'],pruned=result.get('pruned',False))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name',required=True)
    parser.add_argument('--view',choices=('completion','diagnosis','recording','validation'),default='completion')
    args=parser.parse_args()
    from trace_agent import name_check
    name_check(args.name)
    if args.view=='recording':value=recording(args.name)
    elif args.view=='validation':value=validation(args.name)
    elif args.view=='diagnosis':value=diagnosis(args.name)
    else:
        path=artifact_path('status/stage-pipeline/workers')/args.name/'completion-summary.json'
        value=read(path) or dict(status='not_available',path=str(path))
        value['full_evidence']=str(path)
    print(json.dumps(bounded(value,10000),ensure_ascii=True,separators=(',',':')))


if __name__=='__main__':main()

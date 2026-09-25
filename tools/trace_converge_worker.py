"""Drive the pipeline without model polling; stop at MATCH or actionable diagnostics"""
import argparse
import contextlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
from trace_worker import alive, process_identity, write_receipt
from trace_finalize_lock import exclusive_finalize
from xport_project import artifact_path, load_project


class AttentionStop(Exception):
    pass


def match_time(finalization):
    return finalization.get('match_at',time.time())


def progress_view(folder, state, active, now=None):
    progress_path=folder/'progress.json'
    if not progress_path.exists():
        return None
    progress=json.loads(progress_path.read_text())
    now=time.time() if now is None else now
    result=dict(active=active)
    for key in ('step','detail','stage','tick','phase','phase_end','tick_end','segment','updated'):
        if key in progress:result[key]=progress[key]
    result['elapsed_seconds']=(state.get('finished') or now)-(progress.get('started') or state.get('started',now))
    result['heartbeat_age_seconds']=now-progress.get('updated',now)
    owner=progress.get('worker') or state.get('worker')
    if owner:result['worker_pid']=owner.get('pid')
    return result


def running_view(name, path, folder, state):
    result=dict(status='running',supervisor_state=state.get('status'),revision=state.get('revision',0),trace=name,state=str(path))
    progress=progress_view(folder,state,True)
    if progress:result.update(progress)
    return result


def final_gate(root, name, folder):
    tools = Path(__file__).resolve().parent
    results = {}
    for tool in ('code_refresh', 'build_database'):
        log_path = folder / ('final-' + tool.replace('_', '-') + '.log')
        started = time.perf_counter()
        command = [sys.executable, '-B', str(tools / 'xport.py'), '--project', str(root), tool]
        with log_path.open('w', encoding='utf-8') as log:
            outcome = subprocess.run(command, cwd=root, stdout=log, stderr=subprocess.STDOUT)
        results[tool] = dict(exit_code=outcome.returncode,
                             seconds=round(time.perf_counter() - started, 3), log=str(log_path))
        if outcome.returncode:
            raise RuntimeError('Final ' + tool + ' failed; inspect ' + str(log_path))
    write_receipt(folder / 'final-gate.json', dict(status='complete', trace=name, steps=results,
                                                    completed=time.time()))
    return results


def worker(name, anchor=None, final_gate_on_match=False):
    from trace_stage_run import run
    from trace_hash_session import hash_session
    from pipeline_metrics import span
    os.environ['XPORT_TRACE_NAME']=name
    folder=artifact_path('status/stage-pipeline/workers')/name
    folder.mkdir(parents=True,exist_ok=True)
    from trace_supervisor_protocol import issue_attention, transition
    prior=json.loads((folder/'state.json').read_text()) if (folder/'state.json').exists() else {}
    state=dict(worker=process_identity(os.getpid()),trace=name,started=time.time(),
               revision=prior.get('revision',0),protocol_metrics=prior.get('protocol_metrics',{}),
               attention_signatures=prior.get('attention_signatures',[]),
               transitions=prior.get('transitions',[]))
    if prior.get('status') in ('STARTING','VALIDATING','REPLAYING','ATTENTION_REQUIRED',
                               'VERIFYING_REPAIR','MATCH','FINALIZING','COMPLETE','FAILED'):
        state['status']=prior['status']
    def retain_attention(attention, attention_path):
        state['attention']=str(attention_path)
        repeated=attention['failure_signature'] in state['attention_signatures']
        if not repeated:
            state['attention_signatures'].append(attention['failure_signature'])
            metrics=state.setdefault('protocol_metrics',{})
            metrics['model_interventions_required']=metrics.get('model_interventions_required',0)+1
        transition(state,'ATTENTION_REQUIRED',attention_kind=attention['kind'],
                   failure_signature=attention['failure_signature'],
                   model_intervention_required=not repeated)
    transition(state,'STARTING')
    write_receipt(folder/'state.json',state)
    os.environ['XPORT_CONVERGE_PROGRESS']=str(folder/'progress.json')
    from trace_converge_progress import update
    update('baseline',trace=name,started=state['started'])
    try:
        transition(state,'VALIDATING');write_receipt(folder/'state.json',state)
        from trace_agent import baseline
        baseline(name)
        root,config=load_project()
        from repair_supervisor import Supervisor
        supervisor=Supervisor(root,config,name,folder)
        if supervisor.enabled:
            update('validation',detail='recover')
            supervisor.recover()
            if final_gate_on_match:
                state['preparation']=dict(status='deferred_to_final_gate')
            else:
                update('validation',detail='prepare')
                state['preparation']=supervisor.prepare()
                if state['preparation']['status']!='ready':
                    problem='infrastructure_failure' if state['preparation']['status']=='solver_unavailable' else 'validation_failure'
                    state['result']=dict(status='attention_required',preparation=state['preparation'])
                    attention,attention_path=issue_attention(name,folder,state['result'],dict(status=problem))
                    retain_attention(attention,attention_path)
                    raise AttentionStop()
        os.environ['XPORT_CONVERGE_LEASE_ROOT']=str(folder/'emulator-leases')
        with hash_session(), span('converge_worker'):
            while True:
                transition(state,'REPLAYING');write_receipt(folder/'state.json',state)
                update('stage_run',detail='dispatch')
                result=run(argparse.Namespace(name=name,anchor=Path(anchor) if anchor else None,wait=30))
                pending=result.get('status')=='running' or (result.get('diagnostic') or {}).get('status')=='running'
                if pending:
                    time.sleep(.5)
                    continue
                status=result.get('status')
                if supervisor.enabled:
                    supervisor.observe(result)
                    if status in ('mismatch','capture_failed'):
                        state['repair']=supervisor.repair(result)
                        write_receipt(folder/'progress.json',dict(result=result,repair=state['repair'],updated=time.time()))
                        if state['repair']['status']=='patched':
                            transition(state,'VERIFYING_REPAIR');write_receipt(folder/'state.json',state)
                            continue
                if status=='match' and final_gate_on_match and not state.get('final_gate'):
                    transition(state,'VERIFYING_REPAIR');write_receipt(folder/'state.json',state)
                    update('final_gate',detail='code_refresh_and_build_database')
                    state['provisional_match']=result
                    state['final_gate']=final_gate(root,name,folder)
                    if supervisor.enabled:
                        update('validation',detail='prepare_after_final_gate')
                        state['preparation']=supervisor.prepare()
                        if state['preparation']['status']!='ready':
                            state['result']=dict(status='attention_required',preparation=state['preparation'])
                            attention,attention_path=issue_attention(name,folder,state['result'],dict(status='validation_failure'))
                            retain_attention(attention,attention_path)
                            raise AttentionStop()
                    update('stage_run',detail='fresh_post_gate_replay')
                    continue
                state['result']=result
                if status in ('mismatch','capture_failed'):
                    attention,attention_path=issue_attention(name,folder,result,state.get('repair'))
                    retain_attention(attention,attention_path)
                    break
                if status!='match':
                    transition(state,'FAILED',error='Replay returned terminal status '+str(status))
                    break
                transition(state,'MATCH');write_receipt(folder/'state.json',state)
                if state['status']=='MATCH':
                    transition(state,'FINALIZING');write_receipt(folder/'state.json',state)
                    from trace_converge_finish import finish
                    state['finalization']=finish(name,result)
                    state['match_at']=match_time(state['finalization'])
                    if state['finalization'].get('status')!='complete':
                        transition(state,'FAILED',error='Finalization did not complete')
                    elif supervisor.enabled:
                        state['supervisor_completion']=supervisor.finish(result)
                        if state['supervisor_completion']['status']!='complete':
                            attention,attention_path=issue_attention(name,folder,result,dict(status='validation_failure'))
                            retain_attention(attention,attention_path)
                    if state['status']=='FINALIZING':
                        from trace_report import publish
                        try:
                            state['completion_summary']=publish(name,result,state['finalization'],
                                (state.get('supervisor_completion') or {}).get('validation'),supervisor.state)
                            from trace_retention import prune_successful_trace
                            state['retention']=prune_successful_trace(name,result,state['completion_summary'])
                        except (ValueError,OSError,KeyError) as error:
                            state['reporting_attention']=str(error)
                            transition(state,'FAILED',error=str(error))
                    if state['status']=='FINALIZING':transition(state,'COMPLETE',verdict='MATCH')
                break
    except AttentionStop:
        pass
    except Exception as error:
        lowered=str(error).lower()
        kind='foreign_source_change' if 'source changed' in lowered or 'foreign' in lowered else 'infrastructure_failure' if any(word in lowered for word in ('infrastructure','home_readonly','solver unavailable')) else None
        if kind:
            state['result']=state.get('result') or dict(status='attention_required')
            attention,attention_path=issue_attention(name,folder,state['result'],dict(status=kind,error=str(error)))
            retain_attention(attention,attention_path)
        else:
            transition(state,'FAILED',error=str(error))
    state['finished']=time.time()
    _,project=load_project()
    targets=project.get('stage_pipeline',{}).get('supervisor_protocol',{}).get('acceptance',{})
    submissions=list((folder/'submissions').glob('*.json')) if (folder/'submissions').is_dir() else []
    submitted=bool(submissions)
    actual=dict(unchanged_model_calls=state.get('protocol_metrics',{}).get('model_calls_on_unchanged_state',0),
                unique_failure_signatures=len(state.get('attention_signatures',[])),
                model_interventions_required=state.get('protocol_metrics',{}).get('model_interventions_required',0),
                submissions=len(submissions),
                full_gate_after_provisional_match=bool(state.get('final_gate')) if submitted else 'not_exercised',
                terminal_state=state['status'],revision=state['revision'])
    telemetry = ('Watcher summary is linked from completion artifacts' if state['status'] == 'COMPLETE' else
                 'Converge stopped at revisioned attention; no MATCH watcher summary is expected')
    metrics=dict(schema=1,trace=name,targets=targets,actual=actual,
                 telemetry=telemetry,
                 invariants=dict(comparison_scope='owned_by_existing_runner',
                                 controller_transfers='owned_by_existing_recording_and_comparison',
                                 pruning='owned_by_trace_converge_finish_and_trace_retention',
                                 supervisor_generated_gameplay_c_edits='none'))
    write_receipt(folder/'protocol-metrics.json',metrics)
    # A small durable handoff replaces a transcript dump
    handoff=dict(trace=name,status=state['status'],revision=state['revision'],result=state.get('result'),error=state.get('error'),
                 attention=state.get('attention'),
                 next_action='Read attention.json, patch the complete issued dependency branch, then use submit-repair exactly once' if state['status']=='ATTENTION_REQUIRED' else 'Read receipt',
                 state=str(folder/'state.json'))
    from trace_agent import card
    handoff['commands']=card(name)['commands']
    handoff['finalization']=state.get('finalization')
    handoff['repair']=state.get('repair')
    handoff['supervisor_completion']=state.get('supervisor_completion')
    handoff['preparation']=state.get('preparation')
    handoff['completion_summary']=state.get('completion_summary')
    handoff['retention']=state.get('retention')
    handoff['reporting_attention']=state.get('reporting_attention')
    result=state.get('result') or {};detail=result.get('diagnostic') or {}
    diagnostic=detail.get('packet')
    if not diagnostic and result.get('report'):diagnostic=str(Path(result['report']).with_name('diagnostic-packet.json'))
    if diagnostic and Path(diagnostic).is_file():
        packet=json.loads(Path(diagnostic).read_text())
        handoff.update(diagnostic=diagnostic,first_difference=packet.get('first_difference'),
            terminal_failure=packet.get('terminal_failure'),verified_prefix_end=packet.get('verified_prefix_end'),
            audit=packet.get('audit'),build=packet.get('build'),prefix_report=packet.get('prefix_report'))
        handoff['channel_context']=packet.get('channel_context')
        handoff['data_references']=packet.get('data_references')
        handoff['candidate_audits']=packet.get('candidate_audits')
        # Full actor snapshots stay in the linked packet
        if handoff.get('terminal_failure'):handoff['terminal_failure']={k:v for k,v in handoff['terminal_failure'].items() if k!='event'}
    if result.get('receipt') and Path(result['receipt']).is_file():
        accepted=json.loads(Path(result['receipt']).read_text())
        handoff['accepted']=dict(receipt=result['receipt'],sha256=result.get('receipt_sha256'),
            build=accepted['request_identity']['exe_sha256'],trace_sha256=accepted['source_raw_sha256'],scope=accepted['comparison']['scope'])
    from trace_context import bounded
    write_receipt(folder/'handoff.json',bounded(handoff))
    write_receipt(folder/'state.json',state)
    # The existing watcher owns accounting so MATCH does not wait for aggregation
    return 0 if state['status']=='COMPLETE' else 1


def control(name, action='start', wait=0, anchor=None, final_gate_on_match=False,
            resume_attention=False, after_revision=None):
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,64}',name):raise ValueError('Invalid trace name')
    if not 0<=wait<=60:raise ValueError('Wait must be 0..60 seconds')
    root,_=load_project();directory=artifact_path('status/stage-pipeline/workers');directory.mkdir(parents=True,exist_ok=True)
    folder=directory/name;folder.mkdir(exist_ok=True);path=folder/'state.json'
    with exclusive_finalize(directory):
        prior=json.loads(path.read_text()) if path.exists() else None
        launch=folder/'launch.json'
        owner=prior.get('worker') if prior else None
        if launch.exists():owner=json.loads(launch.read_text()).get('worker') or owner
        if action=='start' and prior and prior.get('status')=='COMPLETE':action='status'
        if action=='start' and not alive(owner):
            for other in directory.glob('*/launch.json'):
                if other!=launch and alive(json.loads(other.read_text()).get('worker')):
                    raise RuntimeError('Another project converge worker is active')
            if prior and prior.get('status') in ('STARTING','VALIDATING','REPLAYING','VERIFYING_REPAIR','MATCH','FINALIZING') and not (resume_attention and prior.get('status')=='VERIFYING_REPAIR'):
                raise RuntimeError('Worker died; inspect its capture ownership before explicit recovery')
            if prior and prior.get('status')=='ATTENTION_REQUIRED' and not resume_attention:
                attention_path=Path(prior.get('attention',''))
                attention=json.loads(attention_path.read_text()) if attention_path.is_file() else {}
                branch_ready=attention.get('todo_dependency_branch',{}).get('complete')
                if branch_ready and attention.get('kind') in ('todo_branch','ambiguous_semantics','validation_failure'):
                    return dict(status='attention_required',supervisor_state='ATTENTION_REQUIRED',
                                revision=prior.get('revision',0),attention=prior.get('attention'),trace=name,
                                reason='Use submit-repair with the manifest issued by attention.json')
            if launch.exists() and json.loads(launch.read_text()).get('status')=='launching':
                raise RuntimeError('Ambiguous worker launch; inspect ownership before recovery')
            write_receipt(launch,dict(status='launching',trace=name))
            cmd=[sys.executable,'-B',str(Path(__file__).resolve()),'--worker','--name',name]
            if anchor:cmd+=['--anchor',str(Path(anchor).resolve())]
            if final_gate_on_match:cmd+=['--final-gate-on-match']
            with (folder/'worker.log').open('w') as log:
                from repair_solver import runtime
                _,environment,_=runtime({})
                child=subprocess.Popen(cmd,cwd=root,env=environment,stdout=log,stderr=subprocess.STDOUT,
                    creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
            owner=process_identity(child.pid)
            if owner is None:raise RuntimeError('Worker exited before ownership was committed')
            write_receipt(launch,dict(status='started',worker=owner,mode='fast_iteration' if final_gate_on_match else 'canonical'))
            # Old terminal receipts are not the new worker's result
            prior=None
    until=time.monotonic()+wait
    while True:
        current=json.loads(path.read_text()) if path.exists() else None
        if current and (not owner or current.get('worker')==owner):
            status=current.get('status')
            active=status in ('STARTING','VALIDATING','REPLAYING','VERIFYING_REPAIR','MATCH','FINALIZING')
            if not active:
                from trace_context import bounded
                if status=='COMPLETE' and current.get('verdict')=='MATCH':
                    result=dict(status='MATCH',trace=name,result=current.get('result'),
                        completion=current.get('completion_summary'),reporting_attention=current.get('reporting_attention'),
                        supervisor_state=status,revision=current.get('revision',0),handoff=str(folder/'handoff.json'))
                    progress=progress_view(folder,current,False)
                    if progress:result['last_progress']=progress
                    return bounded(result,10000)
                result=dict(status='attention_required' if status=='ATTENTION_REQUIRED' else 'failed',
                    supervisor_state=status,revision=current.get('revision',0),trace=name,error=current.get('error'),
                    repair=current.get('repair'),preparation=current.get('preparation'),
                    attention=current.get('attention'),diagnostic_view=f'X trace_report --name {name} --view diagnosis',
                    handoff=str(folder/'handoff.json'),state=str(path))
                if status=='ATTENTION_REQUIRED':
                    result['notification']='required' if current.get('model_intervention_required',True) else 'suppressed_duplicate_failure'
                progress=progress_view(folder,current,False)
                if progress:result['last_progress']=progress
                return bounded(result,10000)
        if after_revision is not None and current and current.get('revision',0)>after_revision:
            return running_view(name,path,folder,current)
        if time.monotonic()>=until:
            status='running' if owner and alive(owner) else 'inactive'
            if current and after_revision is not None and current.get('revision',0)<=after_revision:
                return dict(status='unchanged',trace=name,revision=current.get('revision',0),state=str(path))
            return running_view(name,path,folder,current or {}) if status=='running' else dict(status=status,trace=name,state=str(path))
        time.sleep(.2)


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('action',nargs='?',choices=('start','status'),default='start')
    p.add_argument('--name',required=True);p.add_argument('--wait',type=float,default=0);p.add_argument('--anchor');p.add_argument('--worker',action='store_true')
    p.add_argument('--final-gate-on-match',action='store_true')
    p.add_argument('--after-revision',type=int)
    a=p.parse_args()
    if a.worker:return worker(a.name,a.anchor,a.final_gate_on_match)
    print(json.dumps(control(a.name,a.action,a.wait,a.anchor,a.final_gate_on_match,after_revision=a.after_revision)))
    return 0


if __name__=='__main__':raise SystemExit(main())

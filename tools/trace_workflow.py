"""Single-call record, stop and converge entry points with durable launch receipts"""
import argparse
import base64
import contextlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

from trace_finalize_lock import exclusive_finalize
from trace_worker import alive, process_identity, write_receipt
from xport_project import load_project, artifact_path


def name_check(name):
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,40}',name):raise ValueError('Invalid recording name')
    return name


def resolve_slot(root, config, slot):
    if not 1<=slot<=10:raise ValueError('STATE_REQUIRED: slot must be 1..10')
    pattern=config.get('user_trace',{}).get('slot_pattern','')
    if '{slot}' not in pattern:raise ValueError('STATE_REQUIRED: configure user_trace.slot_pattern')
    from duckstation_runtime import data_directory
    state=(root/pattern.format(slot=slot)).resolve()
    runtime=data_directory(role='user').resolve()
    if not state.is_relative_to(runtime) or not state.is_file():
        raise ValueError('STATE_REQUIRED: exact slot file must exist inside user runtime')
    return state


def choose_stop(root, name=None):
    sessions=[]
    for path in (root/'status/user-traces').glob('*/session.json'):
        value=json.loads(path.read_text())
        if value.get('status') in ('starting','recording','finalizing'):
            sessions.append(value)
    if len(sessions)>1:raise ValueError('Ambiguous active recordings; inspect ownership')
    if name:
        name_check(name)
        if sessions and sessions[0]['name']!=name:raise ValueError('Another recording is active')
        return name
    if sessions:return sessions[0]['name']
    for path in (root/'status/workflow').glob('record-*.json'):
        if json.loads(path.read_text()).get('status') in ('launching','running'):
            raise ValueError('Recording launch is still unresolved; observe it before stop')
    current=root/'status/workflow/current.json'
    if current.exists():
        value=json.loads(current.read_text());name=name_check(value['name'])
        path=root/'status/user-traces'/name/'session.json'
        if path.exists() and json.loads(path.read_text()).get('status')=='complete':return name
    return None


def shell_launch(argv, root):
    if os.name!='nt':raise ValueError('Interactive recording requires Windows Shell')
    def quote(value):return "'"+str(value).replace("'","''")+"'"
    command='$s=New-Object -ComObject Shell.Application; $s.ShellExecute('+quote(argv[0])+','+quote(subprocess.list2cmdline(argv[1:]))+','+quote(root)+",'open',0)"
    encoded=base64.b64encode(command.encode('utf-16le')).decode('ascii')
    subprocess.run(['powershell.exe','-NoProfile','-NonInteractive','-EncodedCommand',encoded],
                   check=True,creationflags=subprocess.CREATE_NO_WINDOW)


def observe(path, wait):
    deadline=time.monotonic()+wait
    while True:
        value=json.loads(path.read_text())
        if value['status'] not in ('launching','running'):return value
        if value.get('worker') and not alive(value['worker']):
            return dict(status='attention_required',reason='Worker exited without terminal receipt; do not relaunch',receipt=str(path))
        if time.monotonic()>=deadline:return dict(status=value['status'],receipt=str(path),action=value['action'],name=value['name'])
        time.sleep(.1)


def launch(root, config, action, name, slot, wait):
    name_check(name)
    directory=artifact_path('status/workflow');directory.mkdir(parents=True,exist_ok=True)
    path=directory/(action+'-'+name+'.json')
    with exclusive_finalize(directory):
        if not path.exists():
            for other in directory.glob('*.json'):
                value=json.loads(other.read_text())
                if value.get('status') in ('launching','running'):
                    raise ValueError('Another workflow launch is unresolved: '+str(other))
            request=dict(schema=1,status='launching',action=action,name=name,requested_at=time.time(),project=str(root))
            if action=='record':
                if (root/'status/user-traces'/name).exists():raise ValueError('Recording name already exists')
                request.update(slot=slot,state=str(resolve_slot(root,config,slot)),
                               seconds=config.get('user_trace',{}).get('default_seconds',600),
                               worker_host='python',shell_show='SW_HIDE')
            write_receipt(path,request)
            argv=[sys.executable,'-B',str(Path(__file__).with_name('xport.py')),'--project',str(root),
                  'trace_workflow','--worker',str(path)]
            # DuckStation visibility is owned by user_trace Start-Process
            shell_launch(argv,root)
    return observe(path,wait)


def recorder(args):
    import user_trace
    previous=sys.argv
    try:
        sys.argv=['user_trace',*args]
        user_trace.main()
    finally:sys.argv=previous


def worker(path):
    root,config=load_project();path=artifact_path(path)
    request=json.loads(path.read_text())
    if request['project']!=str(root) or request['status']!='launching':raise ValueError('Unexpected workflow launch identity')
    request.update(status='running',worker=process_identity(os.getpid()),worker_started_at=time.time())
    write_receipt(path,request)
    log=path.with_suffix('.log')
    try:
        with log.open('w',encoding='utf-8') as stream, contextlib.redirect_stdout(stream), contextlib.redirect_stderr(stream):
            name=request['name'];session_path=root/'status/user-traces'/name/'session.json'
            from duckstation_runtime import verify_owner
            from gdb_endpoint import listener_process
            if request['action']=='record':
                from trace_cache import digest
                state=resolve_slot(root,config,request['slot'])
                if str(state)!=request['state']:raise ValueError('Slot configuration changed')
                state_hash=digest(state)
                recorder(['start','--name',name,'--state',str(state),'--seconds',str(request['seconds'])])
                session=json.loads(session_path.read_text())
                if session['status']!='recording' or session['state_sha256']!=state_hash or digest(state)!=state_hash:
                    raise ValueError('Requested slot or recorder state changed')
                owner=verify_owner(session['runtime'],session['port'],listener_process(session['port']))
                if {k:owner[k] for k in ('pid','created')}!=session['process_identity']:raise ValueError('Recorder owner changed')
                raw=Path(session['runtime'])/'savestates'/('ff-trace-'+name+'.bin')
                initial=raw.stat().st_size if raw.exists() else 0
                deadline=time.monotonic()+15
                while (not raw.exists() or raw.stat().st_size<=initial) and time.monotonic()<deadline:
                    if not alive(session['process_identity']):raise ValueError('Recorder exited before confirmation')
                    time.sleep(.1)
                if not raw.exists() or raw.stat().st_size<=initial:raise ValueError('Recording armed but raw growth unconfirmed; inspect existing recording')
                request.update(status='recording',session=str(session_path),raw_growth_verified=True,
                               armed_after_seconds=session['started_wall_time']-request['requested_at'],
                               resumed_after_seconds=session.get('resumed_wall_time',session['started_wall_time'])-request['requested_at'],
                               startup_seconds=session.get('startup_seconds'))
                write_receipt(path.parent/'current.json',dict(name=name))
            else:
                session=json.loads(session_path.read_text())
                owner=None
                if alive(session.get('process_identity')):
                    owner=verify_owner(session['runtime'],session['port'],listener_process(session['port']))
                    if {k:owner[k] for k in ('pid','created')}!=session['process_identity']:raise ValueError('Stop owner changed')
                try:
                    recorder(['stop','--name',name])
                except Exception:
                    failed_session=json.loads(session_path.read_text())
                    if failed_session.get('status')=='capture_failed' and owner:
                        from duckstation_stop import stop_owned
                        try:
                            request['cleanup']=stop_owned(owner)
                        except Exception as cleanup_error:
                            request['cleanup_attention']=str(cleanup_error)
                    raise
                session=json.loads(session_path.read_text())
                if session['status']!='complete':raise ValueError('Finalization incomplete; preserve emulator')
                if not session.get('provenance_verification',{}).get('listed_files_unchanged'):
                    raise ValueError('Recording provenance changed; preserve evidence and inspect')
                from duckstation_stop import stop_owned
                cleanup=stop_owned(owner) if owner else dict(status='already_exited')
                capture=session['capture']
                request.update(status='complete',session=str(session_path),cleanup=cleanup,
                               summary=dict(seconds=capture['summary']['seconds'],vblanks=capture['vblanks'],
                                            controller_transfers=capture['controller_transfers'],missing_vblanks=0))
    except Exception as error:
        request.update(status='attention_required',error=str(error))
    request.update(finished_at=time.time(),log=str(log));write_receipt(path,request)
    return 0 if request['status'] in ('recording','complete') else 1


def converge(name, wait, telemetry, fast_iteration=False):
    name_check(name)
    root,_=load_project()
    session=artifact_path('status/user-traces')/name/'session.json'
    if not session.exists() or json.loads(session.read_text()).get('status')!='complete':
        raise ValueError('Converge requires a completed recording')
    if telemetry=='codex':
        from converge_telemetry import active_path, start
        path=active_path()
        from telemetry_intervals import boundary
        prior=json.loads(path.read_text()) if path.exists() else None
        caller=os.environ.get('CODEX_THREAD_ID') or os.environ.get('CODEX_SESSION_ID')
        if prior and caller and prior.get('session_id')!=caller and not boundary(prior,time.time())[1]:
            raise ValueError('Converge telemetry belongs to another active thread; observe the existing operation')
        if prior is None or prior.get('trace')!=name or boundary(prior,time.time())[1]:start(name)
    from trace_converge_worker import control
    if fast_iteration:
        current=control(name,'status',0)
        if current.get('status')=='running':return current
        command=[sys.executable,'-B',str(Path(__file__).with_name('xport.py')),'--project',str(root),'code_refresh_fast']
        refresh=subprocess.run(command,cwd=root,capture_output=True,text=True,errors='replace')
        if refresh.returncode:
            return dict(status='attention_required',error='Fast refresh failed',output=(refresh.stdout+refresh.stderr)[-4000:])
    result=control(name,'start',wait,final_gate_on_match=fast_iteration)
    if fast_iteration:result['refresh']='provisional_build_only'
    result['telemetry']='Codex session plus pipeline' if telemetry=='codex' else 'Pipeline only; no agent transcript attribution requested'
    return result


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('action',nargs='?',choices=('record','stop','converge','iterate','status'))
    p.add_argument('values',nargs='*');p.add_argument('--worker',type=Path)
    p.add_argument('--wait',type=float,default=30);p.add_argument('--telemetry',choices=('codex','none'),default='codex')
    p.add_argument('--operation',choices=('record','stop','converge'),default='record')
    a=p.parse_args();root,config=load_project()
    if a.worker:return worker(a.worker)
    if not 0<=a.wait<=60:p.error('Wait must be 0..60 seconds')
    try:
        capability=config.get('capabilities',{}).get('trace_workflow',{'enabled':True})
        if a.action in ('record','converge','iterate') and not capability.get('enabled',True):
            raise ValueError('TRACE_WORKFLOW_DISABLED: '+capability.get('reason','project trace workflow is disabled'))
        if a.action=='record' and len(a.values)==2:
            result=launch(root,config,'record',a.values[1],int(a.values[0]),a.wait)
        elif a.action=='stop' and len(a.values)<=1:
            name=choose_stop(root,a.values[0] if a.values else None)
            result=launch(root,config,'stop',name,None,a.wait) if name else dict(status='idle',reason='No active recording')
        elif a.action=='converge' and len(a.values)==1:result=converge(a.values[0],a.wait,a.telemetry)
        elif a.action=='iterate' and len(a.values)==1:result=converge(a.values[0],a.wait,a.telemetry,True)
        elif a.action=='status' and len(a.values)==1:
            name=name_check(a.values[0])
            if a.operation=='converge':
                from trace_converge_worker import control
                result=control(name,'status',a.wait)
            else:result=observe(artifact_path('status/workflow')/(a.operation+'-'+name+'.json'),a.wait)
        else:p.error('Use record SLOT NAME, stop [NAME], converge NAME, iterate NAME, or status NAME --operation OP')
    except (ValueError,OSError,RuntimeError,subprocess.SubprocessError) as error:
        result=dict(status='attention_required',error=str(error))
    from trace_context import bounded
    print(json.dumps(bounded(result,10000),separators=(',',':')))
    return 0 if result['status'] in ('recording','complete','MATCH','running','launching','idle') else 1


if __name__=='__main__':raise SystemExit(main())

"""Run an isolated read-only solver that may propose contracts, never gameplay C"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
import tempfile

from repair_contract import identity, sha
from trace_worker import write_receipt, process_identity
from pipeline_metrics import measured
from xport_process import normalized_environment

INFRASTRUCTURE_REASONS=('home_unavailable','home_readonly','authentication_unavailable','cli_incompatible')


def runtime(settings):
    """Resolve the caller's existing home without copying credentials"""
    env=normalized_environment()
    requested=settings.get('home') or env.get('CODEX_HOME')
    if not requested:
        profile=env.get('USERPROFILE') or env.get('HOME')
        if not profile:
            try:profile=str(Path.home())
            except RuntimeError:profile=None
        if profile:requested=str(Path(profile)/'.codex')
    executable=shutil.which(settings.get('executable','codex'))
    problem=None
    if not executable:problem='executable_unavailable'
    elif not requested or not Path(requested).is_dir():problem='home_unavailable'
    if requested:env['CODEX_HOME']=str(Path(requested).resolve())
    binary=Path(executable) if executable else None
    stat=binary.stat() if binary and binary.is_file() else None
    provenance=dict(executable=executable,home=env.get('CODEX_HOME'),
                    home_exists=bool(requested and Path(requested).is_dir()),runtime_revision=settings.get('runtime_revision',0),
                    binary_size=stat.st_size if stat else None,binary_mtime_ns=stat.st_mtime_ns if stat else None)
    return executable,env,dict(provenance,fingerprint=identity(provenance),problem=problem)


def failure_reason(text, code, timed_out):
    value=text.lower()
    if 'could not find home directory' in value:return 'home_unavailable'
    if 'readonly database' in value or 'read-only database' in value:return 'home_readonly'
    if any(s in value for s in ('unauthorized','authentication','not logged in','401')):return 'authentication_unavailable'
    if any(s in value for s in ('unexpected argument','unrecognized option','unknown option')):return 'cli_incompatible'
    if timed_out:return 'timeout'
    return 'process_failed' if code else 'invalid_or_missing_answer'


def home_write_probe(home):
    """Verify the existing Codex home is writable in this process context"""
    try:
        with tempfile.NamedTemporaryFile(prefix='.xport-solver-write-',dir=home):
            pass
    except OSError as error:
        return str(error)[:1000]
    return None


def retryable_runtime_failure(result, settings):
    """Allow one preserved retry only after the configured runtime identity changes"""
    reason=failure_reason(result.get('stderr_excerpt',''),result.get('exit_code',1),result.get('timed_out',False))
    prior=(result.get('runtime') or {}).get('fingerprint')
    current=runtime(settings)[2]['fingerprint']
    return reason in INFRASTRUCTURE_REASONS and bool(prior) and prior!=current


def preserve_failed_launch(directory):
    index=1
    while (directory/f'failed-runtime-{index:04d}').exists():index+=1
    archive=directory/f'failed-runtime-{index:04d}';archive.mkdir()
    for name in ('result.json','launch.json','answer.json','events.jsonl','stderr.log'):
        path=directory/name
        if path.exists():path.replace(archive/name)
    return archive


def preflight(settings, directory):
    """Check CLI startup without a model request"""
    executable,env,info=runtime(settings);path=Path(directory)/'solver-runtime.json'
    prior=json.loads(path.read_text()) if path.exists() else None
    if not info['problem']:
        problem=home_write_probe(env['CODEX_HOME'])
        info['home_write_probe']='failed' if problem else 'passed'
        if problem:
            info['problem']='home_readonly';info['stderr_excerpt']=problem
    if not info['problem']:
        if prior and prior.get('fingerprint')==info['fingerprint'] and prior.get('status')=='available':
            info.update(exit_code=prior.get('exit_code'),version=prior.get('version'),
                        stderr_excerpt=prior.get('stderr_excerpt'))
        else:
            try:
                check=subprocess.run([executable,'--version'],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                                     timeout=15,text=True,encoding='utf-8',errors='replace',
                                     creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
                info.update(exit_code=check.returncode,version=check.stdout[:200],stderr_excerpt=check.stderr[:1000])
                if check.returncode:info['problem']=failure_reason(check.stderr,check.returncode,False)
            except (OSError,subprocess.TimeoutExpired) as error:info['problem']='launch_failed';info['stderr_excerpt']=str(error)[:1000]
    info['status']='available' if not info['problem'] else 'unavailable'
    info['scope']='CLI launch only; structured solver reasoning requires a real defect'
    write_receipt(path,info);return info


RESULT_SCHEMA={
    'type':'object','additionalProperties':False,
    'properties':{'decision':{'type':'string','enum':['candidate','need_observation','unsupported']},
                  'reason':{'type':'string'},'contract_json':{'type':'string'},
                  'requested_observations':{'type':'array','items':{'type':'string'}}},
    'required':['decision','reason','contract_json','requested_observations']}


@measured('repair_solver')
def solve(root, directory, task, settings):
    directory=Path(directory); directory.mkdir(parents=True,exist_ok=True)
    identity_hash=identity(task); done=directory/'result.json'; launch=directory/'launch.json'
    if done.exists():
        result=json.loads(done.read_text())
        if retryable_runtime_failure(result,settings):preserve_failed_launch(directory)
        else:
            if result.get('task_sha256')!=identity_hash:raise ValueError('Solver task identity changed')
            return result
    if launch.exists():
        raise RuntimeError('Interrupted solver launch requires explicit ownership recovery; no duplicate invocation')
    write_receipt(directory/'task.json',task);write_receipt(directory/'result-schema.json',RESULT_SCHEMA)
    prompt=('Investigate the exact xport failure in task.json. Read project AGENTS.md and shared PIPELINE.md. '
            'Do not edit files, run builds/tests/emulators, or translate IDA pseudocode into C. '
            'The repair engine accepts only a schema-1 project contract for callback_binding or state_transition '
            'derived from pinned original MIPS bytes and mapped existing native source. '
            'Read the contract reference named in task.json. Return that JSON as contract_json only when the '
            'entry/register/memory/call assumptions have explicit evidence. Otherwise return need_observation '
            'or unsupported with concrete missing observations. Never invent runtime RAM from stage-entry data. '
            'Use paths relative to the project. This solver invocation has one defect scope; no unrelated work.\n'
            +json.dumps(task))
    (directory/'prompt.txt').write_text(prompt,encoding='utf-8')
    executable,env,runtime_info=runtime(settings)
    if runtime_info['problem']:
        result=dict(status='needs_solver',task_sha256=identity_hash,reason=runtime_info['problem'],
                    infrastructure_failure=True,runtime=runtime_info,task=str(directory/'task.json'))
        write_receipt(done,result);return result
    output=directory/'answer.json'
    argv=[executable,'exec','--sandbox','read-only','--json','--cd',str(root),
          '--output-schema',str(directory/'result-schema.json'),'-o',str(output),'-']
    started=time.time();write_receipt(launch,dict(status='launching',task_sha256=identity_hash,argv=argv,started=started))
    with (directory/'events.jsonl').open('w',encoding='utf-8') as events, (directory/'stderr.log').open('w',encoding='utf-8') as errors:
        child=subprocess.Popen(argv,cwd=root,env=env,stdin=subprocess.PIPE,stdout=events,stderr=errors,text=True,encoding='utf-8',
                               creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
        write_receipt(launch,dict(status='running',worker=process_identity(child.pid),task_sha256=identity_hash,argv=argv,started=started))
        timed_out=False
        try:child.communicate(prompt,timeout=min(int(settings.get('timeout_seconds',300)),600))
        except subprocess.TimeoutExpired:
            # This is the direct solver child, not an emulator or a discovered process
            child.kill();child.communicate();timed_out=True
    usage=[]
    for line in (directory/'events.jsonl').read_text(encoding='utf-8').splitlines():
        try:event=json.loads(line)
        except ValueError:continue
        if event.get('usage'):usage.append(event['usage'])
    answer=None
    if output.exists() and not timed_out and child.returncode==0:
        try:answer=json.loads(output.read_text(encoding='utf-8'))
        except (ValueError,OSError):pass
    if answer is not None:
        if not isinstance(answer,dict) or set(answer)!=set(RESULT_SCHEMA['required']):answer=None
        elif answer.get('decision') not in RESULT_SCHEMA['properties']['decision']['enum']:answer=None
        elif not isinstance(answer['reason'],str) or not isinstance(answer['contract_json'],str):answer=None
        elif not isinstance(answer['requested_observations'],list) or not all(isinstance(v,str) for v in answer['requested_observations']):answer=None
    with (directory/'stderr.log').open(encoding='utf-8',errors='replace') as stream:excerpt=stream.read(2000)
    reason=None if answer is not None else failure_reason(excerpt,child.returncode,timed_out)
    result=dict(status='answered' if answer is not None else 'needs_solver',task_sha256=identity_hash,
                answer=answer,exit_code=child.returncode,timed_out=timed_out,seconds=time.time()-started,
                reason=reason,infrastructure_failure=reason in INFRASTRUCTURE_REASONS,
                stderr_excerpt=excerpt,runtime=runtime_info,
                usage=usage,events=str(directory/'events.jsonl'),events_sha256=sha((directory/'events.jsonl').read_bytes()),
                scope='Solver reasoning is a proposal; only deterministic transformations can modify game code')
    write_receipt(done,result);return result

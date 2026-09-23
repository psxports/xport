"""One explicit validation command with logs, preflight and graded evidence"""
import argparse
import ast
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid

from pipeline_metrics import span, child_environment
from trace_cache import digest
from trace_worker import write_receipt
from xport_project import artifact_path, load_project


REAL_GATES = ('fresh_build_all_channel_match', 'real_tick_reset_transition',
              'changed_abi_rejection', 'changed_source_cache_rejection',
              'cleanup_resumption_ownership', 'fresh_chat_without_history',
              'distinct_project_profile', 'comparable_end_to_end_performance')


def preflight():
    root, config = load_project()
    suites = config.get('stage_pipeline', {}).get('validation_suites', [])
    if not suites: raise ValueError('Configure stage_pipeline.validation_suites for this project')
    files = {}; commands = []
    for suite in suites:
        directory = (root/suite['directory']).resolve(); pattern = suite['pattern']
        if not directory.is_relative_to(root) or '/' in pattern or '\\' in pattern:
            raise ValueError('Validation suite must select project-local test files')
        selected = sorted(directory.glob(pattern))
        if not selected: raise ValueError('Empty validation suite: '+str(directory/pattern))
        for path in selected:
            ast.parse(path.read_text(encoding='utf-8-sig'),filename=str(path))
            files[str(path)] = digest(path)
        commands.append([sys.executable,'-B','-m','unittest','discover','-s',str(directory),'-p',pattern])
    for path in sorted(Path(__file__).parent.glob('*.py')):
        files[str(path)] = digest(path)
    for folder,patterns in ((root/'tools',('*.py',)),(root/'src',('*.c','*.h'))):
        for pattern in patterns:
            for path in sorted(folder.rglob(pattern)):files[str(path)]=digest(path)
    files[str(root/'xport-project.json')] = digest(root/'xport-project.json')
    for relative in config.get('stage_pipeline',{}).get('validation_inputs',[]):
        path=(root/relative).resolve()
        if not path.is_relative_to(root) or not path.is_file():raise ValueError('Invalid validation input')
        files[str(path)]=digest(path)
    return dict(commands=commands, hashes=files, python=sys.version, platform=sys.platform,
                scope='File presence and syntax only; fixture semantics require execution')


def hash_benchmark(directory):
    from trace_hash_session import hash_session, release_under
    try:
        path=directory/'hash-benchmark.ram';path.write_bytes(bytes(range(256))*32768)
        size=path.stat().st_size
        with hash_session():
            start=time.perf_counter();cold=digest(path);cold_seconds=time.perf_counter()-start
            start=time.perf_counter();warm=digest(path);warm_seconds=time.perf_counter()-start
            blocked=None
            if os.name=='nt':
                try:
                    with path.open('r+b') as stream:stream.write(b'!')
                except OSError:blocked=True
                else:blocked=False
        release_under(directory)
        with path.open('r+b') as stream:stream.write(b'!')
        changed=digest(path)
        if cold!=warm or changed==cold or blocked is False:
            raise ValueError('Hash lease/release or changed-input rejection failed')
        return dict(status='passed',level='synthetic_benchmark',size=size,
            cold_seconds=cold_seconds,warm_seconds=warm_seconds,
            writes_blocked_during_lease=blocked,changed_after_release=True,
            scope='Same-process immutable file hash; OS disk cache uncontrolled; not end-to-end speedup')
    finally:
        release_under(directory)


def run(name):
    from trace_agent import name_check
    from trace_finalize_lock import exclusive_finalize
    name_check(name);root,_=load_project()
    directory=artifact_path('status/toolset-validation')/name
    directory.mkdir(parents=True,exist_ok=True)
    os.environ['XPORT_TRACE_NAME']=name
    with exclusive_finalize(directory), span('validation'):
        identity=preflight();latest=directory/'latest.json'
        attempt=directory/uuid.uuid4().hex;attempt.mkdir()
        value=dict(schema=2,status='running',trace=name,identity=identity,started=time.time(),suites=[],
                   real_gates={key:dict(status='deferred',level='real_run',evidence=[]) for key in REAL_GATES})
        report=attempt/'report.json';write_receipt(report,value)
        write_receipt(latest,dict(report=str(report),sha256=digest(report)))
        try:
            from validation_dependencies import suite_identity, key
            cache=artifact_path('status/toolset-validation/suite-cache');cache.mkdir(parents=True,exist_ok=True)
            for index,cmd in enumerate(identity['commands']):
                suite_input=suite_identity(cmd,identity);cache_path=cache/(key(suite_input)+'.json')
                cached=json.loads(cache_path.read_text()) if cache_path.exists() else None
                if cached and cached.get('identity')==suite_input:
                    prior=cached['result'];prior_log=Path(prior['log'])
                    if prior.get('exit_code')==0 and not prior.get('timed_out') and prior_log.is_file() and digest(prior_log)==prior['log_sha256']:
                        value['suites'].append(dict(prior,reused=True,seconds=0,original_seconds=prior.get('seconds'),cache_receipt=str(cache_path)))
                        continue
                log=attempt/f'suite-{index}.log';start=time.perf_counter()
                code=None;timed_out=False
                try:
                    with log.open('w',encoding='utf-8') as output:
                        process=subprocess.run(cmd,cwd=root,stdout=output,stderr=subprocess.STDOUT,
                            env=child_environment(),timeout=600,
                            creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
                    code=process.returncode
                except subprocess.TimeoutExpired:timed_out=True
                value['suites'].append(dict(argv=cmd,exit_code=code,timed_out=timed_out,
                    seconds=time.perf_counter()-start,log=str(log),log_sha256=digest(log),level='unit'))
                write_receipt(report,value)
                write_receipt(latest,dict(report=str(report),sha256=digest(report)))
                if timed_out or code:raise ValueError('Unit suite failed or timed out; inspect '+str(log))
                if preflight()!=identity:raise ValueError('Validation inputs changed during suite execution')
                write_receipt(cache_path,dict(identity=suite_input,result=value['suites'][-1]))
            from trace_minimization_checks import run as synthetic
            value['synthetic']=dict(synthetic(),level='synthetic')
            with tempfile.TemporaryDirectory(prefix='hash-',dir=attempt) as temporary:
                value['hash_benchmark']=hash_benchmark(Path(temporary))
            if preflight()!=identity:raise ValueError('Validation inputs changed during execution')
            value['status']='automated_checks_passed'
        except Exception as error:
            value.update(status='failed',error=str(error))
        value['finished']=time.time();value['seconds']=value['finished']-value['started']
        value['scope']='Automated checks only; deferred real gates cannot be promoted by unit or synthetic results'
        write_receipt(report,value);write_receipt(latest,dict(report=str(report),sha256=digest(report)))
        return dict(status=value['status'],report=str(report),seconds=value['seconds'],
                    error=value.get('error'),real_gates=value['real_gates'])


def ensure(name):
    """Reuse only a passed receipt with exact current inputs, before replay"""
    from validation_dependencies import changed_components, key
    current=preflight();latest=artifact_path('status/toolset-validation')/name/'latest.json'
    previous=None
    if latest.exists():
        link=json.loads(latest.read_text());path=Path(link['report'])
        if digest(path)!=link['sha256']:raise ValueError('Stored validation receipt changed')
        previous=json.loads(path.read_text())
        if previous['identity']==current and previous['status']=='automated_checks_passed':
            return dict(status=previous['status'],report=str(path),report_sha256=link['sha256'],
                        reused=True,identity_sha256=key(current),seconds=0)
    result=run(name)
    result.pop('real_gates',None)
    result.update(report_sha256=digest(result['report']),identity_sha256=key(current),
                  changed_components=changed_components((previous or {}).get('identity'),current))
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name',required=True);parser.add_argument('--run',action='store_true')
    parser.add_argument('--status',action='store_true');args=parser.parse_args()
    from trace_agent import name_check
    name_check(args.name)
    if args.run and args.status:parser.error('Choose --run or --status')
    if args.status:
        latest=artifact_path('status/toolset-validation')/args.name/'latest.json'
        if not latest.is_file():print(json.dumps(dict(status='not_started')));return 0
        link=json.loads(latest.read_text());path=Path(link['report'])
        if digest(path)!=link['sha256']:raise ValueError('Validation receipt changed')
        value=json.loads(path.read_text())
        print(json.dumps(dict(status=value['status'],report=str(path),seconds=value.get('seconds'),
            identity_current=preflight()==value['identity'],scope=value.get('scope','Automated checks in progress'))))
        return 0
    if not args.run:
        value=preflight();print(json.dumps(dict(status='planned',commands=value['commands'],scope=value['scope'])))
        return 0
    result=run(args.name);print(json.dumps(result))
    return 0 if result['status']=='automated_checks_passed' else 1


if __name__=='__main__':raise SystemExit(main())

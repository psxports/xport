"""Measure one converge workflow from Codex transcripts and the stage journal"""
import argparse
from collections import Counter, defaultdict
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import sqlite3
import time

from trace_worker import write_receipt
from xport_project import load_project, artifact_path


TOKEN_FIELDS=('input_tokens','cached_input_tokens','cache_write_input_tokens',
              'output_tokens','reasoning_output_tokens','total_tokens')


def stamp(value):
    return datetime.fromisoformat(value.replace('Z','+00:00')).timestamp()


def metadata(path):
    try:
        with path.open(encoding='utf-8') as stream:
            row=json.loads(stream.readline())
        return row['payload'] if row.get('type')=='session_meta' else None
    except (OSError,ValueError,KeyError):
        return None


def transcript_root():
    return Path(os.environ.get('CODEX_HOME',str(Path.home()/'.codex'))) / 'sessions'


def select_transcript(project):
    candidates=[]; requested=os.environ.get('CODEX_THREAD_ID') or os.environ.get('CODEX_SESSION_ID')
    for path in transcript_root().glob('*/*/*/rollout-*.jsonl'):
        meta=metadata(path)
        if not meta or meta.get('thread_source')!='user' or meta.get('originator')!='Codex Desktop':continue
        try:cwd=Path(meta['cwd']).resolve()
        except (KeyError,OSError):continue
        if cwd!=project.resolve():continue
        session_id=meta.get('session_id') or meta.get('id')
        if requested and session_id==requested:return path,meta
        candidates.append((path.stat().st_mtime,path,meta))
    if requested:raise ValueError('Current Codex Desktop transcript not found for this project: '+requested)
    if not candidates:raise ValueError('No Codex Desktop user transcript for this project')
    return max(candidates,key=lambda item:item[0])[1:]


def current_turn(path):
    size=path.stat().st_size
    start=max(0,size-32*1024*1024)
    last=None
    with path.open('rb') as stream:
        stream.seek(start)
        if start:stream.readline()
        while True:
            offset=stream.tell();line=stream.readline()
            if not line:break
            try:row=json.loads(line)
            except ValueError:continue
            payload=row.get('payload',{})
            if row.get('type')=='event_msg' and payload.get('type')=='task_started':
                last=(offset,row['timestamp'],payload.get('turn_id'))
    if last is None:raise ValueError('No active Codex turn in transcript tail')
    return last


def active_path():return artifact_path('status/telemetry/converge/active.json')


def report_path(name):return artifact_path('status/telemetry/converge/'+name+'.json')


def start(name):
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,64}',name):raise ValueError('Invalid trace name')
    path=active_path()
    if path.exists():
        prior=json.loads(path.read_text())
        snapshot(prior,finalize=True)
        if path.exists():raise ValueError('Another converge telemetry session is active: '+prior['trace'])
    root,_=load_project();transcript,meta=select_transcript(root)
    offset,started,turn=current_turn(transcript)
    value=dict(schema=1,status='active',trace=name,project=str(root),started_at=started,
               started_epoch=stamp(started),root_turn_id=turn,session_id=meta['session_id'],
               transcript=str(transcript),transcript_offset=offset,created=time.time())
    path.parent.mkdir(parents=True,exist_ok=True);write_receipt(path,value)
    write_receipt(report_path(name),dict(value,coverage=dict(tokens='pending',pipeline='pending')))
    import subprocess, sys
    from trace_worker import process_identity
    with path.with_name('watcher.log').open('a') as log:
        child=subprocess.Popen([sys.executable,'-B',__file__,'watch'],stdout=log,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
    write_receipt(path.with_name('watcher.json'),dict(worker=process_identity(child.pid),trace=name))
    return value


def rows(path,offset=0):
    with Path(path).open('rb') as stream:
        stream.seek(offset)
        if offset:stream.readline() if stream.tell()!=offset else None
        for raw in stream:
            try:yield json.loads(raw)
            except ValueError:continue


from telemetry_intervals import session_measure

def child_sessions(session_id,started,ended):
    result=[]
    for path in transcript_root().glob('*/*/*/rollout-*.jsonl'):
        try:
            if path.stat().st_mtime<started:continue
        except OSError:continue
        meta=metadata(path)
        if meta and meta.get('parent_thread_id')==session_id:
            result.append(session_measure(path,0,started,ended,meta.get('thread_source','auxiliary')))
    return result


def solver_sessions(name,started,ended,known):
    """Include isolated CLI solvers even when their transcript has no parent metadata"""
    wanted=set();unlinked=[]
    folder=artifact_path('status/stage-pipeline/workers')/name/'repairs'
    for launch in folder.rglob('solver/launch.json'):
        record=json.loads(launch.read_text())
        if not started<=record.get('started',0)<=ended:continue
        path=launch.with_name('events.jsonl');linked=False
        for line in path.read_text(encoding='utf-8').splitlines() if path.exists() else []:
            try:event=json.loads(line)
            except ValueError:continue
            if event.get('type')=='thread.started' and event.get('thread_id'):
                wanted.add(event['thread_id']);linked=True
        if not linked:unlinked.append(str(launch))
    wanted-=known
    result=[]; found=set()
    for path in transcript_root().glob('*/*/*/rollout-*.jsonl'):
        meta=metadata(path)
        if meta and (meta.get('id') or meta.get('session_id')) in wanted:
            result.append(session_measure(path,0,started,ended,'repair_solver'))
            found.add(meta.get('id') or meta.get('session_id'))
    return result,sorted(wanted-found),unlinked


def pipeline_measure(name,started):
    path=artifact_path('status/stage-pipeline/runs.sqlite')
    result=dict(database=str(path),runs=0,statuses={},wall_seconds=0,steps={})
    if not path.exists():return result
    db=sqlite3.connect(path);selected=[]
    for rid,request,status,created,updated in db.execute('select id,request,status,created,updated from runs where created>=?',(started,)):
        if json.loads(request).get('trace')!=name:continue
        selected.append(rid);result['runs']+=1;result['statuses'][status]=result['statuses'].get(status,0)+1
        result['wall_seconds']+=max(0,updated-created)
    totals=defaultdict(float)
    for rid in selected:
        for step,seconds in db.execute('select name,seconds from steps where run_id=?',(rid,)):totals[step]+=seconds
    result['steps']=dict(sorted(totals.items()));db.close();return result


def has_match(name,started):
    path=artifact_path('status/stage-pipeline/runs.sqlite')
    if not path.exists():return False
    db=sqlite3.connect(path)
    for request,status,created in db.execute('select request,status,created from runs where created>=?',(started,)):
        if json.loads(request).get('trace')==name and status in ('match','pruned'):
            db.close();return True
    db.close();return False


def snapshot(state=None,finalize=False):
    path=active_path()
    if state is None:
        if not path.exists():return dict(status='inactive')
        state=json.loads(path.read_text())
    from telemetry_intervals import boundary
    ended,closed=boundary(state,time.time())
    main=session_measure(state['transcript'],state['transcript_offset'],state['started_epoch'],ended,'primary')
    sessions=[main,*child_sessions(state['session_id'],state['started_epoch'],ended)]
    solvers,missing_solvers,unlinked_solvers=solver_sessions(state['trace'],state['started_epoch'],ended,{s['session_id'] for s in sessions})
    sessions+=solvers
    token_totals={key:sum(session['tokens'][key] for session in sessions) for key in (*TOKEN_FIELDS,'uncached_input_tokens')}
    report=dict(schema=1,status=('complete' if has_match(state['trace'],state['started_epoch']) else 'closed') if closed else 'active',
        trace=state['trace'],project=state['project'],started_at=state['started_at'],measured_at=datetime.now(timezone.utc).isoformat(),
        wall_seconds=ended-state['started_epoch'],requests=sum(s['requests'] for s in sessions),tokens=token_totals,
        sessions=sessions,pipeline=pipeline_measure(state['trace'],state['started_epoch']),
        coverage=dict(tokens='exact token_usage_record totals for primary and child Codex sessions',
            requests='model responses with response_id; transport retries are not exposed',
            latency='observed local interval proxy, not HTTP RTT',cost='unavailable without account billing data'))
    from pipeline_metrics import summarize
    report['operations']=summarize(artifact_path('status/telemetry/operations'),state['started_epoch'],ended,state['trace'])
    report['coverage']['latency']='Exclusive local item intervals; server RTT unknown'
    report['coverage']['missing_solver_transcripts']=missing_solvers
    report['coverage']['unlinked_solver_launches']=unlinked_solvers
    report['coverage']['tokens']='Exact recorded session totals including discovered isolated solvers; absent solver transcripts are explicit coverage gaps'
    report['pipeline']['note']='Legacy last-step snapshot; use operations for timing, do not sum nested steps'
    worker=artifact_path('status/stage-pipeline/workers')/state['trace']/'state.json'
    if worker.exists():
        final=json.loads(worker.read_text());match=final.get('match_at')
        if match and state['started_epoch']<=match<=ended:
            report['milestones']=dict(match_seconds=match-state['started_epoch'],after_match_seconds=ended-match,
                finalization=final.get('finalization'))
    from trace_optimization_budget import evaluate
    from telemetry_intervals import response_buckets
    match_seconds=report.get('milestones',{}).get('match_seconds')
    report['response_attribution']=response_buckets(sessions,state['started_epoch'],ended,
        state['started_epoch']+match_seconds if match_seconds is not None else None)
    report['time_views']=dict(note='Independent overlapping views; never sum operations and local_items',
        validation_seconds=report['operations']['category_union_seconds'].get('validation',0),
        publication_seconds=report['operations']['category_union_seconds'].get('publication',0),
        unknown_is_not_network=True)
    report['optimization']=evaluate(report)
    write_receipt(report_path(state['trace']),report)
    compact={k:v for k,v in report.items() if k not in ('sessions',)}
    compact['session_totals']=[{k:v for k,v in session.items() if k not in ('responses','transcript')} for session in sessions]
    write_receipt(report_path(state['trace']).with_suffix('.summary.json'),compact)
    if report['status'] in ('complete','closed') and path.exists():path.unlink()
    return report


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('action',choices=('start','snapshot','auto','watch'))
    parser.add_argument('--name');args=parser.parse_args()
    if args.name:
        from trace_agent import name_check
        name_check(args.name)
    if args.action=='watch':
        from telemetry_intervals import boundary
        path=active_path()
        if not path.exists():return
        state=json.loads(path.read_text());match_snapshot=False
        while path.exists():
            current=json.loads(path.read_text())
            if current['root_turn_id']!=state['root_turn_id']:return
            _,closed=boundary(state,time.time())
            if closed:
                snapshot(state,finalize=True)
                return
            if not match_snapshot:
                worker=artifact_path('status/stage-pipeline/workers')/state['trace']/'state.json'
                completed=json.loads(worker.read_text()) if worker.exists() else {}
                if completed.get('status')=='MATCH' and completed.get('finished',0)>=state['started_epoch']:
                    snapshot(state);match_snapshot=True
            time.sleep(3)
        return
    if args.action=='start':
        if not args.name:parser.error('--name is required for start')
        result=start(args.name)
    else:
        if not active_path().exists() and args.name and report_path(args.name).exists():result=json.loads(report_path(args.name).read_text())
        else:result=snapshot(finalize=args.action=='auto')
    print(json.dumps({k:result.get(k) for k in ('status','trace','wall_seconds','requests','tokens','milestones','optimization')},indent=2))


if __name__=='__main__':main()

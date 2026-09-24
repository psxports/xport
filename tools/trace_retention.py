"""Prune trace payloads after a published full MATCH"""
import argparse
import json
from pathlib import Path
import shutil
import time

from trace_cache import digest
from trace_hash_session import release_under
from trace_worker import alive, write_receipt
from xport_project import artifact_path


def load(path):return json.loads(Path(path).read_text())


def owned(path, parent):
    path=Path(path).resolve();parent=Path(parent).resolve()
    if path!=parent and not path.is_relative_to(parent):raise ValueError('Retention cleanup escaped owned directory')
    return path


def size(path):
    if path.is_file():return path.stat().st_size
    return sum(item.stat().st_size for item in path.rglob('*') if item.is_file()) if path.is_dir() else 0


def remove(path, parent, removed):
    path=owned(path,parent)
    if not path.exists():return
    count=size(path);release_under(path)
    if path.is_dir():shutil.rmtree(path)
    else:path.unlink()
    removed.append(dict(path=str(path),bytes=count))


def matching_prepared(work, raw_sha256):
    prepared=work/'prepared'
    if not prepared.exists():return []
    result=[]
    for folder in prepared.iterdir():
        manifest=folder/'manifest.json'
        if not folder.is_dir() or not manifest.is_file() or manifest.stat().st_size>8*1024*1024:continue
        if raw_sha256 in manifest.read_text(encoding='utf-8-sig'):result.append(folder)
    return result


def matching_runs(work, raw_sha256):
    runs=work/'runs';result=[]
    if not runs.exists():return result
    for folder in runs.iterdir():
        manifest=folder/'manifest.json'
        if not folder.is_dir() or not manifest.is_file():continue
        if load(manifest).get('raw_sha256')!=raw_sha256:continue
        for path in folder.rglob('*receipt.json'):
            value=load(path)
            if value.get('worker') and alive(value['worker']):raise ValueError('Matched run still has a live worker')
        result.append(folder)
    return result


def receipt_for(name):
    completed=artifact_path('status/stage-pipeline/completed')
    matches=[]
    for path in completed.glob('*.json'):
        value=load(path)
        if value.get('trace')==name and value.get('status')=='match':matches.append((path,value))
    if not matches:raise ValueError('No published MATCH receipt for trace')
    return max(matches,key=lambda item:item[1].get('pruned_at',0))


def prune_successful_trace(name, result=None, completion=None):
    work=artifact_path('status/stage-pipeline').resolve()
    completed=(work/'completed').resolve();retained=completed/'retention';retained.mkdir(parents=True,exist_ok=True)
    if result and result.get('receipt'):
        source=Path(result['receipt']).resolve()
        if source.parent!=completed or digest(source)!=result.get('receipt_sha256'):raise ValueError('Published MATCH receipt changed')
        match=load(source)
    else:source,match=receipt_for(name)
    if match.get('status')!='match' or match.get('trace')!=name or match.get('comparison',{}).get('passed') is not True:
        raise ValueError('Retention requires a published full MATCH')
    raw_sha256=match['source_raw_sha256'];target=retained/(name+'-'+raw_sha256[:16]+'.json')
    prior=load(target) if target.exists() else None
    trace_root=owned(artifact_path('status/user-traces')/name,artifact_path('status/user-traces'))
    session_path=trace_root/'session.json'
    if not session_path.is_file():raise ValueError('Completed recording session is missing')
    session=load(session_path)
    if session.get('status')!='complete' or session.get('raw_sha256')!=raw_sha256:
        raise ValueError('Recording identity differs from published MATCH')
    if alive(session.get('process_identity')):raise ValueError('Recording process is still active')
    runtime=Path(session['runtime']).resolve();savestates=owned(runtime/'savestates',runtime)
    candidates=[]
    for key in ('raw','source_state'):
        if session.get(key):candidates.append(owned(session[key],savestates))
    for state in session.get('capture',{}).get('states',[]):
        if state.get('path'):candidates.append(owned(state['path'],savestates))
    candidates.append(owned(savestates/('ff-stage-'+name+'.bin'),savestates))
    if (trace_root/'decoded').exists():candidates.append(owned(trace_root/'decoded',trace_root))
    candidates.extend(matching_prepared(work,raw_sha256))
    conversion=work/'conversions'/match['run_id']
    if conversion.exists():candidates.append(owned(conversion,work/'conversions'))
    matched_runs=matching_runs(work,raw_sha256)
    candidates.extend(matched_runs)
    for folder in matched_runs:
        conversion=work/'conversions'/folder.name
        if conversion.exists():candidates.append(owned(conversion,work/'conversions'))
    unique=[];seen=set()
    for path in candidates:
        path=Path(path).resolve()
        if path not in seen:seen.add(path);unique.append(path)
    planned=[dict(path=str(path),bytes=size(path)) for path in unique if path.exists()]
    if not planned and prior and prior.get('status')=='complete':return prior
    record=dict(schema=1,status='pruning',trace=name,run_id=match['run_id'],source_raw_sha256=raw_sha256,
        match_receipt=str(source),match_receipt_sha256=digest(source),completion=completion,
        planned=planned,removed=list((prior or {}).get('removed',[])),started=time.time())
    write_receipt(target,record)
    for path in unique:
        parent=trace_root if path==trace_root/'decoded' else work/'prepared' if path.parent==work/'prepared' else work/'conversions' if path.parent==work/'conversions' else work/'runs' if path.parent==work/'runs' else savestates
        remove(path,parent,record['removed'])
    record.update(status='complete',finished=time.time(),bytes_removed=sum(item['bytes'] for item in record['removed']))
    write_receipt(target,record);return record


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--name',required=True)
    args=parser.parse_args();print(json.dumps(prune_successful_trace(args.name),separators=(',',':')))


if __name__=='__main__':main()

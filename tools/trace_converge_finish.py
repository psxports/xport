"""Idempotently finish a published MATCH without model-driven housekeeping"""
import json
from pathlib import Path
import shutil
import sqlite3
import time
from pipeline_metrics import measured
from trace_cache import digest
from trace_hash_session import release_under
from trace_worker import write_receipt, alive
from xport_project import artifact_path


def load(path):return json.loads(Path(path).read_text())


def remove_owned(path,parent):
    path=Path(path).resolve();parent=Path(parent).resolve()
    if path.parent!=parent:raise ValueError('Cleanup escaped owned parent')
    release_under(path)
    if path.exists():shutil.rmtree(path)


@measured('finalize')
def finish(name,result):
    work=artifact_path('status/stage-pipeline');worker=work/'workers'/name
    if result.get('receipt'):
        if digest(result['receipt'])!=result['receipt_sha256']:raise ValueError('Published receipt changed')
        success=load(result['receipt']);raw=success['source_raw_sha256'];current=success['run_id']
        if success['comparison'].get('passed') is not True:raise ValueError('Finalization requires complete MATCH')
    else:
        report=Path(result['report']);comparison=load(report)
        if comparison.get('passed') is not True:raise ValueError('Finalization requires complete MATCH')
        raw=load(report.parent/'manifest.json')['raw_sha256'];current=report.parent.name
    target=worker/'finalization.json'
    prior=load(target) if target.exists() else {}
    if prior.get('status')=='complete' and prior.get('run_id')==current:return prior
    receipt=dict(status='running',run_id=current,trace=name,match_at=prior.get('match_at',time.time()) if prior.get('run_id')==current else time.time(),removed=[],preserved=[],emulators=[])
    write_receipt(target,receipt)
    with sqlite3.connect(work/'runs.sqlite') as db:
        candidates=list(db.execute("SELECT id,request,status FROM runs WHERE id<>? AND EXISTS (SELECT 1 FROM steps WHERE run_id=runs.id AND ((name='capture' AND status='capture_failed') OR (name='compare' AND status='mismatch')))",(current,)))
    resolved=work/'completed'/'resolved';resolved.mkdir(parents=True,exist_ok=True)
    for rid,request,status in candidates:
        if json.loads(request).get('trace')!=name:continue
        folder=work/'runs'/rid
        if not (folder/'manifest.json').exists():continue
        if load(folder/'manifest.json').get('raw_sha256')!=raw:continue
        live=False
        for path in folder.rglob('*receipt.json'):
            value=load(path)
            if value.get('worker') and alive(value['worker']):live=True;break
        for path in folder.rglob('launch.json'):
            value=load(path)
            if value.get('status')=='launching' or alive(value.get('worker')):live=True;break
        if live:
            receipt['preserved'].append(dict(run=rid,reason='live worker'));continue
        evidence={}
        for n in ('diagnostic-packet.json','prefix-comparison.json','audit-block.json','comparison.json','capture-receipt.json'):
            if (folder/n).exists():evidence[n]=dict(sha256=digest(folder/n),value=load(folder/n))
        write_receipt(resolved/(rid+'.json'),dict(trace=name,run_id=rid,resolved_by=current,raw_sha256=raw,evidence=evidence))
        remove_owned(folder,work/'runs')
        conversion=work/'conversions'/rid
        if conversion.exists():remove_owned(conversion,work/'conversions')
        receipt['removed'].append(rid);write_receipt(target,receipt)
    # Only explicit workflow leases authorize process shutdown; pre-existing sessions remain untouched
    from duckstation_stop import stop_owned
    leases=worker/'emulator-leases'
    if leases.exists():
        for path in sorted(leases.glob('*.json')):
            record=load(path)
            if record.get('trace')!=name or record.get('purpose')!='converge_check':raise ValueError('Invalid workflow emulator lease')
            try:receipt['emulators'].append(stop_owned(record))
            except (OSError,ValueError,RuntimeError) as error:
                receipt['emulators'].append(dict(status='blocked',lease=str(path),error=str(error)))
    receipt.update(status='attention_required' if any(x['status']=='blocked' for x in receipt['emulators']) else 'complete',finished=time.time())
    receipt['seconds_after_match']=receipt['finished']-receipt['match_at']
    write_receipt(target,receipt);return receipt

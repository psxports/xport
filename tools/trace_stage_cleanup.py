"""Replace a successful heavy stage run with a compact resumable receipt"""
import json
from pathlib import Path
import shutil
import time

from pipeline_journal import set_step, step
from trace_cache import digest
from trace_worker import write_receipt


@__import__("pipeline_metrics").measured("cleanup")
def prune(db, run_id, work, folder=None):
    work=Path(work).resolve()
    runs=(work/'runs').resolve()
    completed=(work/'completed').resolve()
    cleanup=step(db,run_id,'cleanup')
    if cleanup and cleanup['status']=='pruned':return cleanup['receipt']
    if cleanup and cleanup['status']!='pruning':raise ValueError('Invalid cleanup state')
    if cleanup:
        receipt=cleanup['receipt'];folder=Path(receipt['run_directory'])
    else:
        folder=Path(folder).resolve()
        if folder.parent!=runs or folder.name!=run_id:raise ValueError('Run cleanup escaped owned directory')
        compare=step(db,run_id,'compare');publish=step(db,run_id,'publish')
        if not compare or compare['status']!='match' or not publish or publish['status']!='match':
            raise ValueError('Only a published MATCH may be pruned')
        report_path=folder/'comparison.json'
        report=json.loads(report_path.read_text())
        if report.get('passed') is not True or digest(report_path)!=compare['receipt']['sha256']:
            raise ValueError('Successful comparison evidence changed')
        completed.mkdir(parents=True,exist_ok=True)
        receipt_path=completed/(run_id+'.json')
        timings={row['name']:row['seconds'] for row in db.execute('SELECT name,seconds FROM steps WHERE run_id=?',(run_id,))}
        request_identity=json.loads(db.execute('SELECT request FROM runs WHERE id=?',(run_id,)).fetchone()[0])
        manifest_path=folder/'manifest.json';manifest=json.loads(manifest_path.read_text())
        value=dict(schema=1,status='match',run_id=run_id,trace=request_identity['trace'],request_identity=request_identity,
            source_raw_sha256=manifest['raw_sha256'],manifest_sha256=digest(manifest_path),
            comparison_sha256=digest(report_path),comparison=report,timings=timings,
            run_directory=str(folder),pruned_at=time.time())
        write_receipt(receipt_path,value)
        continuation=step(db,run_id,'continuation')
        receipt=dict(status='match',pruned=True,receipt=str(receipt_path),receipt_sha256=digest(receipt_path),
            run_directory=str(folder),continuation=continuation['receipt']['entry'] if continuation else None)
        set_step(db,run_id,'cleanup','pruning',receipt)
    folder=Path(receipt['run_directory']).resolve()
    if folder.parent!=runs or folder.name!=run_id:raise ValueError('Persisted cleanup escaped owned directory')
    continuation=receipt.get('continuation')
    if continuation:
        target=Path(continuation).resolve();allowed=(work/'anchors/continuations').resolve()
        if target.parent!=allowed:raise ValueError('Continuation cleanup escaped registry')
        if target.exists():target.unlink()
    from trace_hash_session import release_under
    release_under(folder)
    if folder.exists():shutil.rmtree(folder)
    if not Path(receipt['receipt']).is_file() or digest(receipt['receipt'])!=receipt['receipt_sha256']:
        raise ValueError('Compact successful-run receipt changed')
    set_step(db,run_id,'cleanup','pruned',receipt)
    return receipt

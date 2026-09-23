"""Resume diagnostic publication independently of a cached comparison verdict"""
from pathlib import Path
from pipeline_journal import step, set_step


def diagnose(db, run_id, folder, wait, replay=None):
    previous=step(db,run_id,'diagnostic')
    if previous and previous['status'] not in ('planned','running'):
        return previous['receipt']
    if replay is None:
        from trace_stage_narrow import run
        replay=run
    # Persist intent before spawning so a controller interruption is resumable
    set_step(db,run_id,'diagnostic','planned',{'folder':str(folder)})
    result=replay(Path(folder),wait)
    receipt=dict(result,folder=str(folder))
    set_step(db,run_id,'diagnostic',result['status'],receipt)
    return receipt

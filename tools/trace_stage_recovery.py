"""Recover a proven dead capture without overwriting its evidence or a live game"""
import json
from pathlib import Path
import subprocess
from trace_worker import alive, write_receipt
from xport_project import project_path


def native_idle():
    name=project_path('native_executable').stem.replace("'","''")
    result=subprocess.run(['powershell','-NoProfile','-NonInteractive','-Command',
        "$ErrorActionPreference='Stop'; if (Get-Process -Name '"+name+"' -ErrorAction SilentlyContinue) { exit 3 }"],
        capture_output=True)
    if result.returncode:raise RuntimeError('Native process exists or inspection failed; preserve it before recovery')


def recover(folder, limit=2):
    folder=Path(folder).resolve()
    pending=folder/'recovery-pending.json'
    if pending.exists():
        action=json.loads(pending.read_text())
    else:
        path=folder/'capture-receipt.json'
        if not path.exists():return None
        receipt=json.loads(path.read_text())
        if receipt.get('status')!='running' or alive(receipt.get('worker')):return None
        if not receipt.get('worker'):raise ValueError('Missing worker identity; recovery is ambiguous')
        completed=list((folder/'recovery').glob('attempt-*/recovery.json'))
        if len(completed)>=limit:
            return dict(status='failed',exit_code=2,error='Capture recovery attempt limit exhausted')
        request=json.loads((folder/'request.json').read_text())
        config=json.loads((folder/'capture.json').read_text())
        paths=[folder/name for name in ('capture-receipt.json','launch.json','worker.log','capture.log','capture-failure.json')]
        paths.extend(Path(value) for value in request['native'].values())
        if config.get('checkpoint_directory'):paths.append(Path(config['checkpoint_directory']))
        items=[]
        for path in paths:
            path=path.resolve()
            if path.parent!=folder:raise ValueError('Capture recovery requires direct owned artifacts')
            if path.exists() and path.name not in items:items.append(path.name)
        action=dict(attempt=len(completed)+1,worker=receipt['worker'],items=items,
            checkpoint_directory=Path(config['checkpoint_directory']).name if config.get('checkpoint_directory') else None)
        native_idle()
        write_receipt(pending,action)
    if alive(action['worker']):raise RuntimeError('Recovery worker identity is live; preserve it')
    native_idle()
    destination=folder/'recovery'/('attempt-'+str(action['attempt']))
    destination.mkdir(parents=True,exist_ok=True)
    committed=destination/'recovery.json'
    for name in ([] if committed.exists() else action['items']):
        if Path(name).name!=name:raise ValueError('Invalid recovery artifact')
        source=folder/name;target=destination/name
        if source.exists():
            if target.exists():raise ValueError('Both recovery source and destination exist')
            source.replace(target)
        elif not target.exists():raise ValueError('Recovery artifact disappeared')
    if not committed.exists():write_receipt(committed,action)
    if action.get('checkpoint_directory'):
        name=action['checkpoint_directory']
        if Path(name).name!=name:raise ValueError('Invalid checkpoint directory')
        (folder/name).mkdir(exist_ok=True)
    pending.unlink()
    return None

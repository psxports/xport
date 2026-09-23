"""Invoke a project stage adapter once within a persisted caller deadline"""
import json
from pathlib import Path
import subprocess
import sys
import time
from trace_cache import digest
from trace_worker import write_receipt


def inputs(root,project,plan):
    value=project.get('stage_pipeline',{}).get('adapter')
    session=json.loads(Path(plan['session']).read_text())
    package=session.get('stage_packages',{})
    if not value or package.get('status')!='captured':return None
    adapter=(root/value).resolve()
    if not adapter.is_relative_to(root.resolve()) or adapter.suffix!='.py':raise ValueError('Adapter must be project-local Python')
    index=Path(package['index']).resolve()
    if digest(index)!=package['index_sha256']:raise ValueError('Stage package index changed')
    return dict(adapter=str(adapter),adapter_sha256=digest(adapter),index=str(index),index_sha256=digest(index))


@__import__("pipeline_metrics").measured("convert")
def convert(spec,plan_path,folder,seconds):
    if spec is None:return dict(status='ANCHOR_UNSUPPORTED',reason='No captured stage package or project adapter')
    if seconds<=0:return dict(status='ANCHOR_UNSUPPORTED',reason='Anchor deadline exhausted')
    folder=Path(folder).resolve();folder.mkdir(parents=True,exist_ok=True)
    before={key:digest(spec[key]) for key in ('adapter','index')}
    if any(before[key]!=spec[key+'_sha256'] for key in before):raise ValueError('Converter input changed')
    output=folder/'candidate'
    command=[sys.executable,'-B',spec['adapter'],'--plan',str(Path(plan_path).resolve()),'--index',spec['index'],'--output',str(output)]
    prior=folder/'conversion-receipt.json'
    if prior.exists():
        receipt=json.loads(prior.read_text())
        if receipt['inputs']!=spec or receipt['command']!=command:
            raise ValueError('Persisted converter identity changed')
        if receipt['status']=='candidate':
            if digest(receipt['candidate'])!=receipt['candidate_sha256']:
                raise ValueError('Persisted converter candidate changed')
            if json.loads(Path(receipt['candidate']).read_text())!=receipt['anchor']:
                raise ValueError('Persisted candidate contents changed')
        return receipt
    if output.exists() or (folder/'conversion-intent.json').exists():
        return dict(status='ANCHOR_UNSUPPORTED',reason='Interrupted converter has no committed receipt; preserve output, do not launch a duplicate')
    write_receipt(folder/'conversion-intent.json',dict(inputs=spec,command=command))
    started=time.perf_counter()
    with (folder/'converter.log').open('wb') as log:
        try:
            result=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,timeout=seconds,
                creationflags=subprocess.CREATE_NO_WINDOW)
        except subprocess.TimeoutExpired:
            receipt=dict(status='ANCHOR_UNSUPPORTED',reason='Converter deadline exceeded')
        else:
            if result.returncode:
                receipt=dict(status='ANCHOR_UNSUPPORTED',reason='Project adapter rejected the stage',exit_code=result.returncode)
            else:
                if any(digest(spec[key])!=before[key] for key in before):raise ValueError('Converter input changed during execution')
                candidate=output/'candidate.json'
                receipt=dict(status='candidate',anchor=json.loads(candidate.read_text()),candidate=str(candidate),candidate_sha256=digest(candidate))
    receipt.update(seconds=time.perf_counter()-started,inputs=spec,command=command)
    write_receipt(folder/'conversion-receipt.json',receipt)
    return receipt

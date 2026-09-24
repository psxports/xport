"""Compact command card, durable handoff and task-relative source changes"""
import argparse
import difflib
import hashlib
import json
from pathlib import Path
import re
import zipfile
from trace_worker import write_receipt
from xport_project import load_project, artifact_path


def name_check(name):
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,64}',name):raise ValueError('Invalid trace name')
    return name


def baseline(name):
    root,_=load_project();directory=artifact_path('status/stage-pipeline/workers')/name_check(name)
    directory.mkdir(parents=True,exist_ok=True);path=directory/'source-baseline.zip'
    session=json.loads((artifact_path('status/user-traces')/name/'session.json').read_text())
    identity=session.get('raw_sha256');metadata=directory/'source-baseline.json'
    if metadata.exists():
        record=json.loads(metadata.read_text())
        if record['trace_sha256']!=identity:raise ValueError('Trace name reused with another recording')
        if not path.exists():raise ValueError('Missing task source baseline')
        return record
    temporary=path.with_suffix('.pending');files={}
    with zipfile.ZipFile(temporary,'w',zipfile.ZIP_DEFLATED) as archive:
        for source in sorted((root/'src').rglob('*')):
            if not source.is_file() or source.suffix not in ('.c','.h'):continue
            data=source.read_bytes();key=source.relative_to(root).as_posix()
            archive.writestr(key,data);files[key]=hashlib.sha256(data).hexdigest()
    temporary.replace(path)
    record=dict(trace_sha256=identity,files=files,archive=str(path))
    write_receipt(metadata,record);return record


def changes(name,limit=10000):
    root,_=load_project();directory=artifact_path('status/stage-pipeline/workers')/name_check(name)
    meta=json.loads((directory/'source-baseline.json').read_text());out=[];remaining=limit;changed=[]
    with zipfile.ZipFile(directory/'source-baseline.zip') as archive:
        paths=set(meta['files'])|{p.relative_to(root).as_posix() for p in (root/'src').rglob('*') if p.is_file() and p.suffix in ('.c','.h')}
        for key in sorted(paths):
            after=(root/key).read_bytes() if (root/key).is_file() else b''
            if meta['files'].get(key)==hashlib.sha256(after).hexdigest():continue
            changed.append(key)
            before=archive.read(key) if key in meta['files'] else b''
            diff=''.join(difflib.unified_diff(before.decode(errors='replace').splitlines(True),after.decode(errors='replace').splitlines(True),fromfile='baseline/'+key,tofile=key,n=3))
            out.append(diff[:max(0,remaining)]);remaining-=len(diff)
    return dict(changed_files=changed,diff=''.join(out),bounded=remaining<0,baseline=str(directory/'source-baseline.json'))


def card(name):
    name_check(name)
    return dict(trace=name,commands={
        'begin':f'X trace_workflow converge {name} --wait 30',
        'run_or_repair':f'X trace_workflow converge {name} --wait 30',
        'fast_iteration':f'X trace_workflow iterate {name} --wait 30',
        'observe':f'X trace_workflow status {name} --operation converge --wait 30',
        'summary':f'X trace_report --name {name} --view completion',
        'diagnosis':f'X trace_report --name {name} --view diagnosis',
        'record_metrics':f'X trace_report --name {name} --view recording',
        'handoff':f'X trace_agent handoff --name {name}',
        'changes':f'X trace_agent changes --name {name}',
        'evidence':f'X trace_agent evidence --name {name} --path EVIDENCE --pointer /FIELD [--since SHA256]',
        'runtime_words':'X trace_context --package-index INDEX --phase PHASE --address 0xADDRESS --words 4 --output status/WORDS.json',
        'validation_status':f'X trace_validation --name {name} --status',
        'bench':'X repair_bench --contract CONTRACT.json --output status/AUDIT --run',
        'dispatch_index':'X repair_dispatch --contract CONTRACT.json --output status/DISPATCH.json',
        'causal':'X repair_causal --audit status/AUDIT --output status/CAUSE.json',
        'codegen_assessment':'X repair_codegen --contract CONTRACT.json --output status/CODEGEN.json',
        'audit':'X trace_audit_packet 0xSITE --image IMAGE --output status/AUDIT.json [--state N --radius N --pseudo-line N]',
        'narrow':'X trace_stage_narrow --run RUN_DIRECTORY [--target ABSOLUTE_PHASE --wait 20]',
        'telemetry':f'X converge_telemetry snapshot --name {name}'},
        rules=['Read project AGENTS.md and shared PIPELINE.md; no old chat history by default',
               'Inspect first_difference before terminal_failure',
               'If the causal function is TODO, translate its complete image-qualified TODO dependency branch before replay',
               'Use run_or_repair after a code fix; it performs the cross-build probe and full verification',
               'Never weaken acceptance or use a partial diagnostic as MATCH',
               'Only inspect bounded evidence needed for the current defect'])


def evidence(path, pointer='', since=None):
    from trace_cache import digest
    from trace_context import bounded
    root,_=load_project();path=Path(path)
    if not path.is_absolute():path=root/path
    path=path.resolve()
    if not path.is_relative_to(root):raise ValueError('Evidence must be inside this project')
    identity=digest(path)
    if since==identity:return dict(path=str(path),sha256=identity,unchanged=True)
    value=json.loads(path.read_text(encoding='utf-8-sig'))
    if pointer:
        if not pointer.startswith('/'):raise ValueError('Use a JSON pointer starting with /')
        for key in pointer[1:].split('/'):
            key=key.replace('~1','/').replace('~0','~')
            value=value[int(key)] if isinstance(value,list) else value[key]
    if digest(path)!=identity:raise ValueError('Evidence changed while reading')
    return dict(path=str(path),sha256=identity,pointer=pointer,value=bounded({'selected':value}))


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('action',choices=('card','handoff','changes','evidence'))
    p.add_argument('--name',required=True);p.add_argument('--path',type=Path)
    p.add_argument('--pointer',default='');p.add_argument('--since')
    a=p.parse_args();name_check(a.name)
    if a.action=='card':value=card(a.name)
    elif a.action=='changes':value=changes(a.name)
    elif a.action=='evidence':
        if not a.path:p.error('--path is required for evidence')
        value=evidence(a.path,a.pointer,a.since)
    else:
        path=artifact_path('status/stage-pipeline/workers')/a.name/'handoff.json'
        value=json.loads(path.read_text()) if path.exists() else dict(status='not_started',commands=card(a.name)['commands'])
    from trace_context import bounded
    print(json.dumps(bounded(value,limit=10000),ensure_ascii=True,separators=(',',':')))


if __name__=='__main__':main()

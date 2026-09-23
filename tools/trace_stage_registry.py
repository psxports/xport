"""Resolve compatible stage candidates and retain build-specific continuation evidence"""
import argparse
import hashlib
import json
from pathlib import Path

from trace_cache import digest
from trace_worker import write_receipt
from xport_project import artifact_path, load_project, project_path


def identity(plan, anchor):
    return dict(raw_sha256=plan['identity']['raw_sha256'],
        profile_sha256=plan['identity']['profile_sha256'],phase_begin=plan['phase_begin'],
        pc=plan['pc'],stage=plan['stage'],tick=plan['tick'],abi=anchor['abi'],
        adapter_version=anchor['adapter_version'],input_calls_sha256=anchor['input_calls_sha256'],
        jobs_sha256=anchor['jobs_sha256'])


def key(value):
    return hashlib.sha256(json.dumps(value,sort_keys=True,separators=(',',':')).encode()).hexdigest()


def immutable(path, value):
    encoded=json.dumps(value,sort_keys=True,indent=2)+'\n'
    if path.exists():
        if json.loads(path.read_text())!=value:raise ValueError('Registry entry is immutable')
    else:
        path.parent.mkdir(parents=True,exist_ok=True)
        with path.open('x') as stream:stream.write(encoded)


def register(plan, anchor, directory):
    from trace_stage_run import preflight
    preflight(plan,anchor)
    entry=dict(schema=1,identity=identity(plan,anchor),candidate=anchor)
    path=directory/(key(entry['identity'])+'-'+key(anchor)+'.json')
    immutable(path,entry)
    return path


def resolve(plan, settings, directory):
    from trace_stage_run import preflight
    matches=[]
    for path in sorted(directory.glob('*.json')):
        entry=json.loads(path.read_text())
        if entry.get('schema')!=1:raise ValueError('Unsupported stage registry schema')
        anchor=entry['candidate']
        expected=identity(plan,anchor)
        if entry['identity']!=expected:continue
        if anchor['abi']!=settings.get('native_abi'):continue
        if anchor.get('adapter_sha256') and anchor['adapter_sha256']!=settings.get('adapter_sha256'):continue
        if anchor['adapter_version'] not in settings.get('compatible_adapters',[]):continue
        preflight(plan,anchor)
        matches.append((path,anchor))
    if len(matches)>1:raise ValueError('Ambiguous compatible stage candidates; specify --anchor')
    return matches[0] if matches else None


@__import__("pipeline_metrics").measured("acceptance")
def accept(plan, anchors, folder, directory):
    report_path=folder/'comparison.json'
    spec_path=folder/'manifest.json'
    receipt_path=folder/'capture-receipt.json'
    request_path=folder/'request.json'
    report=json.loads(report_path.read_text())
    spec=json.loads(spec_path.read_text())
    receipt=json.loads(receipt_path.read_text())
    request=json.loads(request_path.read_text())
    if report.get('passed') is not True or receipt.get('exit_code')!=0:
        raise ValueError('Fresh passing continuation required')
    if spec['raw_sha256']!=plan['identity']['raw_sha256'] or report['contract']!=spec['contract']:
        raise ValueError('Comparison belongs to another trace/contract')
    if report['contract']['phase_begin']!=plan['phase_begin'] or report['contract']['phase_end']!=plan['phase_end']:
        raise ValueError('Continuation does not cover the requested interval')
    if isinstance(anchors,dict):anchors=[anchors]
    if len(anchors)!=len(plan['segments']):raise ValueError('Accepted anchor count differs from stage segments')
    for ordinal,anchor in enumerate(anchors):
        numbered='_'+str(ordinal)
        suffix=numbered if 'checkpoint'+numbered in request['dependency_hashes'] else ''
        for name,expected in (('checkpoint'+suffix,anchor['checkpoint_sha256']),('context'+suffix,anchor['context_sha256'])):
            if receipt['dependency_hashes'][name]!=expected or request['dependency_hashes'][name]!=expected:
                raise ValueError('Capture used another anchor')
    for name,path in request['dependencies'].items():
        if digest(path)!=request['dependency_hashes'][name] or receipt['dependency_hashes'][name]!=request['dependency_hashes'][name]:
            raise ValueError('Continuation dependency changed: '+name)
    for side in ('original','native'):
        for name,path in spec[side].items():
            if digest(path)!=report['hashes'][side][name]:raise ValueError('Compared channel changed')
            if side=='native' and digest(path)!=receipt['artifact_hashes'][name]:raise ValueError('Captured channel changed')
    candidates=[]
    for segment,anchor in zip(plan['segments'],anchors):
        segment_plan=dict(plan)
        segment_plan.update(phase_begin=segment['start_phase'],phase_end=segment['end_phase'],
                            stage=segment['stage'],tick=segment['start_tick'])
        candidate=register(segment_plan,anchor,directory)
        candidates.append(dict(path=str(candidate),sha256=digest(candidate),identity=identity(segment_plan,anchor)))
    value=dict(schema=2,identities=[item['identity'] for item in candidates],candidates=candidates,
        exe_sha256=receipt['exe_sha256'],scope=report['scope'],contract=report['contract'],
        evidence={name:dict(path=str(path),sha256=digest(path)) for name,path in
            (('comparison',report_path),('manifest',spec_path),('receipt',receipt_path),('request',request_path))})
    path=directory/'continuations'/(key(value)+'.json')
    immutable(path,value)
    return path


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action',choices=('register','resolve','accept'))
    parser.add_argument('--plan',type=Path,required=True)
    parser.add_argument('--anchor',type=Path)
    parser.add_argument('--run',type=Path)
    args=parser.parse_args()
    plan=json.loads(args.plan.read_text())
    directory=artifact_path('status/stage-pipeline/anchors')
    if args.action=='resolve':
        match=resolve(plan,load_project()[1].get('stage_pipeline',{}),directory)
        result=dict(status='candidate' if match else 'ANCHOR_UNSUPPORTED',entry=str(match[0]) if match else None)
    else:
        anchor=json.loads(args.anchor.read_text())
        path=register(plan,anchor,directory) if args.action=='register' else accept(plan,anchor,args.run,directory)
        result=dict(status='registered' if args.action=='register' else 'accepted',entry=str(path))
    print(json.dumps(result))


if __name__=='__main__':main()

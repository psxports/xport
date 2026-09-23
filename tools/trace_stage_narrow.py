"""Replay a diagnostic window from a captured checkpoint before the first divergence"""
import argparse
import json
from pathlib import Path
import time

from trace_cache import digest
from trace_phase_checkpoint import context
from trace_phase_index import compare_window
from trace_worker import write_receipt


def choose(report, receipt, request, target, config):
    contract=report['contract']
    if type(target) is not int or not contract['phase_begin']<=target<contract['phase_end']:
        raise ValueError('Diagnostic target outside verified comparison scope')
    if report['passed']:
        prefix_end=contract['phase_end']
    else:
        prefix_end=(report.get('first_difference') or {}).get('verified_prefix_end')
    if type(prefix_end) is not int:
        raise ValueError('Cannot seek across an unlocalized or incomplete channel')
    from trace_stage_verify import contract_segments
    segment=next(s for s in contract_segments(contract) if s['start_phase']<=target<s['end_phase'])
    choices=[]
    for row in receipt.get('checkpoints',[]):
        if segment['start_phase']<=row['ordinal']<=target and row['ordinal']<prefix_end:
            if digest(row['checkpoint'])!=row['checkpoint_sha256'] or digest(row['context'])!=row['context_sha256']:
                raise ValueError('Captured checkpoint changed')
            actual=context(row['context'])
            if actual!=row['state'] or actual['ordinal']!=row['ordinal']:raise ValueError('Checkpoint context metadata changed')
            choices.append(row)
    if choices:return max(choices,key=lambda row:row['ordinal'])
    # The capture's initial anchor remains a candidate when divergence starts immediately
    checkpoint=config['checkpoint'];ctx=checkpoint+'.ctx'
    expected={str(Path(v).resolve()):request['dependency_hashes'][k] for k,v in request['dependencies'].items()}
    if digest(checkpoint)!=expected.get(str(Path(checkpoint).resolve())) or digest(ctx)!=expected.get(str(Path(ctx).resolve())):
        raise ValueError('Initial anchor changed')
    state=context(ctx)
    if state['ordinal']!=segment['start_phase']:raise ValueError('Initial anchor boundary differs')
    return dict(checkpoint=checkpoint,context=ctx,ordinal=state['ordinal'],state=state,
        checkpoint_sha256=digest(checkpoint),context_sha256=digest(ctx))


def run(folder, wait=0, target=None):
    from trace_stage_run import launch_or_observe
    from xport_project import artifact_path
    folder=artifact_path(folder)
    report_path=folder/'comparison.json'
    if not report_path.exists():report_path=folder/'prefix-comparison.json'
    report=json.loads(report_path.read_text())
    partial=report.get('diagnostic_only',False)
    if partial:
        report=dict(report,first_difference=dict(report.get('first_difference') or {},verified_prefix_end=report.get('verified_prefix_end')))
    request=json.loads((folder/'request.json').read_text())
    if request.get('trace'):__import__('os').environ['XPORT_TRACE_NAME']=request['trace']
    receipt=json.loads((folder/'capture-receipt.json').read_text())
    spec=json.loads((folder/'manifest.json').read_text())
    if target is None:target=(report.get('first_difference') or {}).get('ordinal')
    if target is None:return dict(status='not_localizable',reason='No precise first-divergence phase')
    for name,path in request['dependencies'].items():
        if digest(path)!=request['dependency_hashes'][name]:raise ValueError('Diagnostic dependency changed: '+name)
    if partial:
        for path,expected in report.get('artifact_hashes',{}).items():
            if digest(path)!=expected:raise ValueError('Prefix artifact changed: '+path)
        if digest(folder/'capture-receipt.json')!=report['capture_receipt_sha256']:raise ValueError('Failure receipt changed')
        if digest(spec['original']['phases'])!=report['source_sha256']:raise ValueError('Original source changed')
    else:
        for side in ('original','native'):
            for name,path in spec[side].items():
                if digest(path)!=report['hashes'][side][name]:raise ValueError('Compared input changed')
    if partial and report.get('verified_prefix_end') is None:
        return dict(status='not_localizable',reason='Unverified prefix requires initial anchor replay')
    from trace_stage_verify import contract_segments
    segments=contract_segments(report['contract'])
    segment=next(s for s in segments if s['start_phase']<=target<s['end_phase'])
    config=json.loads((folder/'capture.json').read_text())
    if config.get('segments'):config=json.loads(Path(config['segments'][segments.index(segment)]).read_text())
    selected=choose(report,receipt,request,target,config)
    end=min(segment['end_phase'],target+3)
    session_root=Path(request['dependencies'].get('phase_metadata',spec['original']['inputs'])).parent
    phases=json.loads((session_root/'phase-boundaries.json').read_text())
    calls=json.loads((session_root/'game-input-calls.json').read_text())
    begin_clock=phases[0]['emulated_ticks'];end_clock=phases[end-1]['emulated_ticks']
    cursor=sum(begin_clock<=row['emulated_ticks']<end_clock for row in calls)
    directory=folder/('diagnostic-phase-'+str(target))
    directory.mkdir(exist_ok=True)
    started=time.perf_counter()
    config.update(checkpoint=selected['checkpoint'],output=str(directory/'native.phases'),
        phase_count=end-selected['ordinal'],audit_output=str(directory/'native-state'),input_calls_end=cursor)
    config.pop('checkpoint_directory',None)
    native={k:str(directory/('native-state.'+k)) for k in ('actors','world','inputs')}
    native['phases']=config['output']
    dependencies=dict(request['dependencies'],checkpoint=selected['checkpoint'],context=selected['context'],
        config=str(directory/'capture.json'),diagnostic=str(Path(__file__).resolve()))
    if not (directory/'request.json').exists():
        write_receipt(directory/'capture.json',config)
        write_receipt(directory/'request.json',dict(trace=request.get('trace'),dependencies=dependencies,
            dependency_hashes={k:digest(v) for k,v in dependencies.items()},native=native,
            input_calls_end=cursor))
        write_receipt(directory/'selection.json',dict(checkpoint=selected,begin=selected['ordinal'],end=end,
            target=target,comparison_sha256=digest(report_path),receipt_sha256=digest(folder/'capture-receipt.json')))
    else:
        selection=json.loads((directory/'selection.json').read_text())
        if selection['checkpoint']!=selected or selection['end']!=end or selection['comparison_sha256']!=digest(report_path) or selection['receipt_sha256']!=digest(folder/'capture-receipt.json'):
            raise ValueError('Persisted diagnostic selection changed')
    result=launch_or_observe(directory,wait)
    if result['status']=='running':return dict(status='running',directory=str(directory))
    if result.get('exit_code')!=0:return dict(status='capture_failed',directory=str(directory))
    if result.get('input_calls_consumed')!=cursor:raise ValueError('Narrow replay did not consume its exact input interval')
    for name,path in native.items():
        if digest(path)!=result['artifact_hashes'][name]:raise ValueError('Diagnostic capture changed: '+name)
    from trace_prefix import compare_channels
    comparison=compare_channels(spec,native,selected['ordinal'],end,require_complete=True)
    value=dict(status='diagnostic_match' if comparison['window_match'] else 'diagnostic_mismatch',
        comparison=comparison,begin=selected['ordinal'],end=end,target=target,seconds=time.perf_counter()-started,
        scope='Diagnostic phase-state/actors/world/inputs/GPU/sound continuation; does not replace complete channel acceptance')
    write_receipt(directory/'comparison.json',value)
    return dict(status=value['status'],report=str(directory/'comparison.json'),begin=selected['ordinal'],end=end)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run',type=Path,required=True)
    parser.add_argument('--target',type=int)
    parser.add_argument('--wait',type=float,default=0)
    args=parser.parse_args()
    if not 0<=args.wait<=30:parser.error('--wait must be 0..30')
    print(json.dumps(run(args.run,args.wait,args.target)))


if __name__=='__main__':main()

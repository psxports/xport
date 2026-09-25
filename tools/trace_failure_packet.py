"""Convert native failure evidence into a bounded actionable packet"""
import json
from pathlib import Path
import re
from trace_worker import write_receipt
from xport_project import load_project


def failure_packet(folder):
    folder=Path(folder);root,project=load_project()
    events=[]
    for path in [folder/'wip.jsonl',*sorted(folder.glob('segment-*/wip.jsonl'))]:
        if path.exists():
            for line in path.open(encoding='utf-8'):
                try: row=json.loads(line)
                except ValueError:continue
                if row.get('event')=='wip':events.append(row)
    event=events[-1] if events else {}
    causal_event=next((row for row in reversed(events) if row.get('pc') not in (None,'00000000','0')),event)
    log=folder/'capture.log'
    tail=''
    if log.exists():
        with log.open('rb') as f:
            f.seek(max(0,log.stat().st_size-16384));tail=f.read().decode(errors='replace')
    sites=re.findall(r'(?:pc |branch )(8[0-9A-Fa-f]{7})',tail)
    callbacks=re.findall(r'XPORT_CALLBACK target=([0-9A-Fa-f]+) object=([0-9A-Fa-f]+) kind=([0-9A-Fa-f]+) tick=(\d+)',tail)
    if callbacks:
        target,obj,kind,tick=callbacks[-1]
        event['callback']=dict(target=target,object=obj,kind=kind,tick=int(tick))
    site=int(causal_event['pc'],16) if causal_event.get('pc') else int(sites[-1],16) if sites else None
    receipt=json.loads((folder/'capture-receipt.json').read_text())
    spec=json.loads((folder/'manifest.json').read_text())
    ordinal=None
    if 'stage' in event and 'tick' in event:
        candidates=[s['start_phase']+event['tick']-s['start_tick'] for s in spec['contract'].get('segments',[])
                    if s['stage']==event['stage'] and s['start_tick']<=event['tick']<=s['end_tick']]
        if len(candidates)==1:ordinal=candidates[0]
    checkpoints=[c for c in receipt.get('checkpoints',[]) if ordinal is not None and c['ordinal']<=ordinal]
    selected=max(checkpoints,key=lambda c:c['ordinal']) if checkpoints else None
    result=dict(status='NEEDS_CODE',kind='capture_failure',event=event,causal_event=causal_event,pc=hex(site) if site else None,
        ordinal=ordinal,build=receipt.get('exe_sha256'),checkpoint=selected,
        signature=dict(pc=site,stage=event.get('stage'),actors=[(a.get('slot'),a.get('state')) for a in event.get('actors',[])]),
        log=str(log),tail=tail[-1600:],audit=None)
    from trace_prefix import failure_prefix
    try: prefix=failure_prefix(folder)
    except (ValueError,OSError,KeyError) as error:
        prefix=dict(first_difference=None,verified_prefix_end=None,problem=str(error))
    result['terminal_failure']=dict(pc=result['pc'],ordinal=ordinal,event=event)
    result['prefix_report']=str(folder/'prefix-comparison.json') if (folder/'prefix-comparison.json').exists() else None
    result['verified_prefix_end']=prefix.get('verified_prefix_end')
    result['prefix_problem']=prefix.get('problem')
    difference=prefix.get('first_difference')
    result['first_difference']=difference
    from trace_context import enrich
    result.update(enrich(prefix,folder))
    if difference:
        result.update(kind='earlier_prefix_difference',ordinal=difference['ordinal'],pc=None,checkpoint=None)
        limit=prefix.get('verified_prefix_end')
        choices=[c for c in receipt.get('checkpoints',[]) if type(limit) is int and c['ordinal']<min(limit,difference['ordinal'])]
        result['checkpoint']=max(choices,key=lambda c:c['ordinal']) if choices else None
    # A terminal PC is a candidate only when no earlier state difference is known
    if site and not difference:
        from trace_audit_packet import audit
        evidence=audit(site,state=event.get('actors',[{}])[0].get('state') if event.get('actors') else None)
        write_receipt(folder/'audit-block.json',evidence)
        result['audit']=str(folder/'audit-block.json')
    write_receipt(folder/'diagnostic-packet.json',result)
    return dict(packet=str(folder/'diagnostic-packet.json'),pc=result['pc'],ordinal=result['ordinal'],audit=result['audit'],
                prefix=result['prefix_report'],first_difference=difference)

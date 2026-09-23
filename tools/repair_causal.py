"""Locate observed writers and bounded register dependencies in an isolated mismatch"""
import argparse
import json
from pathlib import Path
from repair_contract import sha
from trace_worker import write_receipt
from xport_project import artifact_path


def overlaps(event,address,width=1):
    start=event.get('address',-1)
    return start < address+width and address < start+event.get('width',0)


def slice_events(events, writer, limit=128):
    needed={2} if writer is None else set(); selected=[]
    if writer is not None:
        pc=events[writer].get('pc')
        instruction=next((e for e in events[writer+1:] if e.get('kind')=='instruction' and e['pc']==pc),None)
        if instruction: needed.add((instruction['word']>>16)&31)
    for event in reversed(events[:writer+1] if writer is not None else events):
        kind=event.get('kind')
        if kind=='instruction':
            changed={i for i,(a,b) in enumerate(zip(event['before'],event['after'])) if a!=b}
            if changed & needed:
                w=event['word']; selected.append(dict(pc=event['pc'],word=w,registers=sorted(changed&needed)))
                needed-=changed; needed.update(((w>>21)&31,(w>>16)&31)); needed.discard(0)
        elif kind in ('read','call') and selected:
            selected.append(event)
        if len(selected)>=limit: break
    return dict(events=list(reversed(selected)),unresolved_registers=sorted(needed),bounded=len(selected)>=limit,
                scope='Observed value-changing register dependencies; control dependencies and unchanged writes may be absent')


def analyze(directory):
    directory=Path(directory); report=json.loads((directory/'report.json').read_text())
    failure=report['failures'][0]
    paths=[directory/'original-events.json',directory/'native-events.json']
    original,native=[json.loads(path.read_text()) for path in paths]
    memory=failure.get('memory'); address=memory['address'] if memory else None
    writers=[i for i,e in enumerate(original) if e.get('kind')=='write' and address is not None and overlaps(e,address)]
    native_writers=[e for e in native if e.get('kind')==1 and address is not None and overlaps(e,address)]
    last=writers[-1] if writers else None
    return dict(schema=1,status='observations',failure=failure,
                original_writer=original[last] if last is not None else None,
                native_writer=native_writers[-1] if native_writers else None,
                dependencies=slice_events(original,last),
                event_hashes={p.name:sha(p.read_bytes()) for p in paths},
                scope='Exact isolated execution observations; writer is a candidate cause, not a proof of causality; direct C pointer writes have no helper event',
                missing_native_writer=bool(memory and not native_writers))


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--audit',required=True);p.add_argument('--output',required=True);a=p.parse_args()
    result=analyze(artifact_path(a.audit));target=artifact_path(a.output);target.parent.mkdir(parents=True,exist_ok=True)
    write_receipt(target,result);print(json.dumps(dict(status=result['status'],output=str(target))))


if __name__=='__main__':raise SystemExit(main())

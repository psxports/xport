"""Bound session accounting to a recorded turn and deduplicate responses"""
from collections import Counter, defaultdict
from datetime import datetime
import json
from pathlib import Path


def stamp(value):
    return datetime.fromisoformat(value.replace('Z','+00:00')).timestamp()


def boundary(state, now):
    from converge_telemetry import rows
    for row in rows(state['transcript'],state['transcript_offset']):
        if stamp(row['timestamp'])<state['started_epoch']:continue
        p=row.get('payload',{})
        if row['type']=='event_msg' and p.get('type')=='task_complete' and p.get('turn_id')==state['root_turn_id']:
            return min(now,stamp(row['timestamp'])),True
        if row['type']=='event_msg' and p.get('type')=='task_started' and p.get('turn_id')!=state['root_turn_id']:
            return min(now,stamp(row['timestamp'])),True
    return now,False


def session_measure(path, offset, started, ended, role):
    from converge_telemetry import metadata, rows, TOKEN_FIELDS
    meta=metadata(Path(path)) or {};seen={};models={};usage=[];intervals=[];counts=Counter()
    for row in rows(path,offset):
        when=stamp(row['timestamp'])
        if not started<=when<=ended:continue
        p=row.get('payload',{});kind=row['type']
        if kind=='turn_context':models[p.get('turn_id')]=p.get('model')
        if kind=='token_usage_record':
            identity=p.get('response_id')
            if not identity:continue
            if identity in seen:
                if seen[identity]!=p['usage']:raise ValueError('Conflicting response usage')
                continue
            seen[identity]=p['usage']
            usage.append(dict(timestamp=row['timestamp'],response_id=identity,turn_id=p.get('turn_id'),
                model=models.get(p.get('turn_id')),**{k:int(p['usage'].get(k,0)) for k in TOKEN_FIELDS}))
        if kind=='event_msg' and p.get('type')=='item_completed':
            name=p.get('item',{}).get('type','unknown');counts[name]+=1
            a=max(started,p.get('started_at_ms',0)/1000);b=min(ended,p.get('completed_at_ms',0)/1000)
            if b>a:intervals.append((a,b,name))
    points=sorted({v for a,b,_ in intervals for v in (a,b)})
    exclusive=defaultdict(float);busy=0
    gaps=[];cursor=started
    for a,b,_ in sorted(intervals):
        if a>cursor:gaps.append((cursor,a))
        cursor=max(cursor,b)
    if cursor<ended:gaps.append((cursor,ended))
    for a,b in zip(points,points[1:]):
        types={name for x,y,name in intervals if x<=a and y>=b}
        if types:
            name=next(iter(types)) if len(types)==1 else 'overlapping_items'
            exclusive[name]+=b-a;busy+=b-a
    totals={k:sum(u[k] for u in usage) for k in TOKEN_FIELDS}
    totals['uncached_input_tokens']=totals['input_tokens']-totals['cached_input_tokens']
    own_id=Path(path).stem[-36:]
    return dict(role=role,thread_source=meta.get('thread_source'),session_id=own_id,
        metadata_session_id=meta.get('session_id'),parent_thread_id=meta.get('parent_thread_id'),
        transcript=str(path),requests=len(usage),tokens=totals,responses=usage,
        local_items=dict(counts=dict(counts),exclusive_seconds=dict(exclusive),
                         observed_union_seconds=busy,unknown_seconds=max(0,ended-started-busy),
                         unknown_gaps=dict(count=len(gaps),top=[dict(start=a,end=b,seconds=b-a)
                             for a,b in sorted(gaps,key=lambda pair:pair[1]-pair[0],reverse=True)[:8]],
                             attribution='Uncovered local intervals; not inferred server or network time')),
        server_seconds=None,transport_retries=None)


def response_buckets(sessions, started, ended, match=None):
    bounds=[('before_match',started,match),('after_match',match,ended)] if match is not None else [('unmatched',started,ended)]
    result={}
    for index,(name,a,b) in enumerate(bounds):
        rows=[r for session in sessions for r in session['responses']
              if a<=stamp(r['timestamp']) and (stamp(r['timestamp'])<b or index==len(bounds)-1 and stamp(r['timestamp'])==b)]
        inputs=sum(r['input_tokens'] for r in rows);cached=sum(r['cached_input_tokens'] for r in rows)
        output=sum(r['output_tokens'] for r in rows)
        result[name]=dict(wall_seconds=b-a,responses=len(rows),input_tokens=inputs,
            cached_input_tokens=cached,uncached_plus_output=inputs-cached+output,output_tokens=output,
            mean_input_tokens=inputs/len(rows) if rows else None)
    return dict(buckets=result,assignment='Usage timestamp, not request start; boundary-crossing responses are not split')

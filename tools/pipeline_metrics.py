"""Append-only operation spans with process-local nesting and explicit unknown time"""
from contextlib import contextmanager
from contextvars import ContextVar
from functools import wraps
import json
import os
from pathlib import Path
import time
import uuid
import heapq
from collections import Counter, defaultdict
from xport_process import normalized_environment

_parent = ContextVar('operation_parent', default=None)


def child_environment(base=None):
    env=normalized_environment(base)
    parent=_parent.get() or os.environ.get('XPORT_PARENT_OPERATION')
    if parent:env['XPORT_PARENT_OPERATION']=parent
    return env


def process_counters(handle=None):
    if os.name!='nt':
        t=os.times();return dict(cpu_user_seconds=t.user,cpu_kernel_seconds=t.system)
    import ctypes
    from ctypes import wintypes as w
    kernel=ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.GetProcessTimes.argtypes=[w.HANDLE,*([ctypes.POINTER(w.FILETIME)]*4)]
    class IO(ctypes.Structure):
        _fields_=[(key,ctypes.c_ulonglong) for key in ('read_operations','write_operations','other_operations','read_bytes','write_bytes','other_bytes')]
    kernel.GetProcessIoCounters.argtypes=[w.HANDLE,ctypes.POINTER(IO)]
    target=handle if handle is not None else w.HANDLE(-1)
    times=[w.FILETIME() for _ in range(4)];io=IO();result={}
    if kernel.GetProcessTimes(target,*(ctypes.byref(t) for t in times)):
        result.update(cpu_kernel_seconds=((times[2].dwHighDateTime<<32)|times[2].dwLowDateTime)/1e7,
                      cpu_user_seconds=((times[3].dwHighDateTime<<32)|times[3].dwLowDateTime)/1e7)
    if kernel.GetProcessIoCounters(target,ctypes.byref(io)):
        result.update({key:getattr(io,key) for key,_ in IO._fields_})
    return result


@contextmanager
def span(name, **fields):
    from xport_project import artifact_path
    directory = artifact_path('status/telemetry/operations')
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / (str(os.getpid()) + '.jsonl')
    identifier = uuid.uuid4().hex
    parent = _parent.get() or os.environ.get('XPORT_PARENT_OPERATION')
    before=process_counters()
    start = time.perf_counter_ns()
    epoch = time.time()
    def emit(event, **extra):
        row = dict(operation_id=identifier, parent_id=parent, name=name, event=event,
                   pid=os.getpid(), trace=os.environ.get('XPORT_TRACE_NAME'),
                   run_id=os.environ.get('XPORT_RUN_ID'), **fields, **extra)
        with path.open('a', encoding='utf-8') as f:
            f.write(json.dumps(row, ensure_ascii=True) + '\n')
    emit('start', epoch=epoch, monotonic_ns=start)
    token = _parent.set(identifier)
    outcome = 'ok'
    result = {}
    try:
        yield result
    except BaseException:
        outcome = 'error'
        raise
    finally:
        end = time.perf_counter_ns()
        after=process_counters()
        result['process_delta']={key:after[key]-before.get(key,0) for key in after}
        emit('end', epoch=time.time(), monotonic_ns=end, seconds=(end-start)/1e9,
             outcome=outcome, measurements=result)
        _parent.reset(token)


def measured(name):
    def decorate(function):
        @wraps(function)
        def call(*args, **kwargs):
            with span(name):
                return function(*args, **kwargs)
        return call
    return decorate


def exclusive_intervals(records):
    events=defaultdict(list)
    for index,record in enumerate(records):
        events[record['begin']].append((1,index))
        events[record['end']].append((-1,index))
    points=sorted(events);active=set();pids=Counter();heap=[];exclusive={};busy=0
    for a,b in zip(points,points[1:]):
        for direction,index in events[a]:
            record=records[index]
            if direction==1:
                active.add(index);pids[record['pid']]+=1
                heapq.heappush(heap,(-record['begin'],-int(bool(record.get('parent_id'))),index))
            else:
                active.discard(index);pids[record['pid']]-=1
                if pids[record['pid']]==0:del pids[record['pid']]
        while heap and heap[0][2] not in active:heapq.heappop(heap)
        if not heap:continue
        name=records[heap[0][2]]['name'] if len(pids)==1 else 'concurrent_operations'
        exclusive[name]=exclusive.get(name,0)+b-a;busy+=b-a
    return exclusive,busy


def summarize(directory, begin, end, trace):
    starts = {}; records = []
    for path in Path(directory).glob('*.jsonl'):
        for line in path.open(encoding='utf-8'):
            try: row = json.loads(line)
            except ValueError: continue
            if row.get('trace') != trace: continue
            if row['event'] == 'start': starts[row['operation_id']] = row
            elif row['operation_id'] in starts:
                first = starts[row['operation_id']]
                a, b = max(begin, first['epoch']), min(end, row['epoch'])
                if b > a: records.append(dict(first, end=b, begin=a, measurements=row.get('measurements', {})))
    exclusive,busy=exclusive_intervals(records)
    milestones={}
    for record in records:
        item=milestones.setdefault(record['name'],dict(first_start=record['begin'],last_end=record['end'],count=0))
        item['first_start']=min(item['first_start'],record['begin']);item['last_end']=max(item['last_end'],record['end']);item['count']+=1
    category_unions={}
    for name in milestones:
        total=0;right=begin
        for record in sorted((r for r in records if r['name']==name),key=lambda r:r['begin']):
            total+=max(0,record['end']-max(right,record['begin']))
            right=max(right,record['end'])
        category_unions[name]=total
    hash_paths={}
    for record in records:
        if record['name']!='hash':continue
        item=hash_paths.setdefault(record.get('path','unknown'),dict(calls=0,bytes_hashed=0,cache_hits=0,seconds=0))
        item['calls']+=1
        item['bytes_hashed']+=record['measurements'].get('bytes_hashed',0)
        item['cache_hits']+=int(record['measurements'].get('cache_hit',False))
        item['seconds']+=record['end']-record['begin']
    ranked=sorted(hash_paths.items(),key=lambda item:item[1]['bytes_hashed'],reverse=True)
    return dict(exclusive_seconds=exclusive, observed_union_seconds=busy,milestones=milestones,
                category_union_seconds=category_unions,
                hash_io=dict(top_paths=[dict(path=p,**v) for p,v in ranked[:12]],
                    distinct_paths=len(ranked),omitted_paths=max(0,len(ranked)-12),
                    note='Per-file durations may overlap across processes; not additive wall time'),
                native_child_totals={key:sum(r['measurements'].get(key,0) for r in records if r['name']=='native_capture') for key in ('cpu_user_seconds','cpu_kernel_seconds','read_bytes','write_bytes','output_bytes')},
                unobserved_seconds=max(0, end-begin-busy), operations=len(records),
                bytes_hashed=sum(r['measurements'].get('bytes_hashed', 0) for r in records),
                hash_cache_hits=sum(r['measurements'].get('cache_hit', False) for r in records))

"""Compare diagnostic channels on absolute phases without accepting a partial capture"""
import json
from itertools import islice
from pathlib import Path
import struct

from pipeline_metrics import measured
from trace_actor_diff import RECORD_SIZE, compare as actor_compare
from trace_bundle import worlds
from trace_cache import digest
from trace_phase_index import rows as phase_rows
from trace_phases import iter_phases, compare as phase_compare
from trace_stage_verify import contract_segments
from trace_world_context import describe, gpu_detail
from gpu_packet_semantics import canonical_packet, IGNORE_RAW_SPRITE_RGB
from trace_worker import write_receipt


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def fixed(path, size, offset=0):
    with Path(path).open('rb') as stream:
        stream.seek(offset*size)
        while block := stream.read(size):
            if len(block) != size: raise ValueError('Truncated fixed-size record')
            yield block


def axis(contract):
    return [(s['start_phase']+i, s['start_tick']+i)
            for s in contract_segments(contract)
            for i in range(s['end_phase']-s['start_phase'])]


def cursor(metadata, end):
    phases=read(Path(metadata)/'phase-boundaries.json')
    calls=read(Path(metadata)/'game-input-calls.json')
    if not 0 < end <= len(phases): raise ValueError('Input cursor outside original phase range')
    begin_clock=phases[0]['emulated_ticks']; end_clock=phases[end-1]['emulated_ticks']
    return sum(begin_clock <= row['emulated_ticks'] < end_clock for row in calls)


def pair_channel(left, right, coordinates, check):
    count=0; first=None; problem=None
    try:
        for ordinal,tick in coordinates:
            b=next(right,None)
            if b is None: break
            a=next(left,None)
            if a is None: raise ValueError('Original channel ended before declared contract')
            difference=check(a,b,tick)
            if difference:
                first=dict(difference,ordinal=ordinal);break
            count+=1
        else:
            if next(right,None) is not None: raise ValueError('Extra native boundary')
    except (ValueError,OSError,struct.error) as error:
        problem=str(error)
    finally:
        for it in (left,right):
            if hasattr(it,'close'):it.close()
    return dict(matched=count,first_difference=first,problem=problem,
                available_prefix_end=coordinates[count][0] if count<len(coordinates) else
                (coordinates[-1][0]+1 if coordinates else None))


def actor_check(a,b,tick):
    if struct.unpack_from('<I',a)[0]!=tick or struct.unpack_from('<I',b)[0]!=tick:
        return dict(field='actor_tick',expected=tick)
    if a!=b: return actor_compare(a,b).get('first_difference') or dict(field='actor_record')


def world_check(a,b,tick):
    if a[0]!=tick or b[0]!=tick: return dict(field='world_tick',expected=tick,original=a[0],native=b[0])
    if a[1:3]!=b[1:3]: return dict(field='world_state',context=describe(a,b))
    if a[3]!=b[3] and [canonical_packet(p,IGNORE_RAW_SPRITE_RGB) for p in a[3]] != [canonical_packet(p,IGNORE_RAW_SPRITE_RGB) for p in b[3]]:
        return dict(field='gpu',context=gpu_detail(a[3],b[3]))


@measured('prefix_compare')
def compare_channels(spec, native, begin, end, require_complete=False):
    """Compare a complete narrow window or available failure prefix, never grant acceptance"""
    coordinates=axis(spec['contract'])
    selected=[(o,t) for o,t in coordinates if begin<=o<end]
    offset=sum(o<begin for o,t in coordinates)
    if not selected or selected[0][0]!=begin: raise ValueError('Diagnostic range is outside gameplay contract')
    from trace_world_index import seek
    reports={}
    for name, left, right, check in (
        ('actors',lambda:fixed(spec['original']['actors'],RECORD_SIZE,offset),lambda:fixed(native['actors'],RECORD_SIZE),actor_check),
        ('world',lambda:seek(spec['original']['world'],offset),lambda:worlds(native['world']),world_check)):
        try: reports[name]=pair_channel(left(),right(),selected,check)
        except (OSError,ValueError) as error: reports[name]=dict(matched=0,problem=str(error),first_difference=None,available_prefix_end=begin)
    terminal={s['end_phase']-1 for s in contract_segments(spec['contract'])}
    original_inputs=read(spec['original']['inputs'])
    input_coordinates=[(o,t) for o,t in selected if o<end-1 and o not in terminal]
    original_selected=(original_inputs[i] for i,(o,t) in enumerate(coordinates) if begin<=o<end-1 and o not in terminal)
    def input_check(a,b,tick):
        b=list(struct.unpack('<3I',b))
        if a!=b or a[0]!=tick:return dict(field='input',original=a,native=b,expected=tick)
    reports['inputs']=pair_channel(iter(original_selected),fixed(native['inputs'],12),input_coordinates,input_check)
    # The final boundary has no complete post-boundary input/sound interval
    if reports['inputs']['matched']==len(input_coordinates) and not reports['inputs']['problem'] and not reports['inputs']['first_difference']:
        reports['inputs']['available_prefix_end']=end-1
    phase_tail={}; count=0; first=None; issue=None; sound_end=begin
    left=phase_rows(spec['phase_index'],begin,end,spec['raw_sha256'])
    right=iter_phases(native['phases'],sound=True,gpu=True,allow_incomplete=True,diagnostics=phase_tail)
    try:
        for ordinal in range(begin,end):
            b=next(right,None)
            if b is None:break
            a=next(left)
            include_sound=ordinal<end-1 and ordinal not in terminal and b.get('sound_complete',False)
            result=phase_compare([a],[b],sound=True,gpu=True,include_last_sound=include_sound)
            if not result['passed']:
                first=dict(result['first_difference'],ordinal=ordinal);break
            count+=1
            if include_sound or ordinal in terminal:sound_end=ordinal+1
        else:
            if next(right,None) is not None:raise ValueError('Extra native phase')
    except (ValueError,OSError,StopIteration,struct.error) as error:issue=str(error) or 'Missing original phase'
    finally:left.close();right.close()
    reports['phases']=dict(matched=count,first_difference=first,problem=issue,
        available_prefix_end=min(begin+count,sound_end),tail=phase_tail)
    differences=[dict(channel=k,**v['first_difference']) for k,v in reports.items() if v.get('first_difference')]
    first=min(differences,key=lambda d:d['ordinal']) if differences else None
    safe_end=min(v.get('available_prefix_end') or begin for v in reports.values())
    if first:safe_end=min(safe_end,first['ordinal'])
    complete=all(v['matched']==(len(input_coordinates) if k=='inputs' else len(selected)) and not v['problem'] and not v['first_difference'] for k,v in reports.items())
    if require_complete and not phase_tail.get('footer_complete'):complete=False
    return dict(schema=1,diagnostic_only=True,passed=False,window_match=complete,
        begin=begin,end=end,channels=reports,first_difference=first,
        verified_prefix_end=safe_end,scope='Available complete diagnostic boundaries; final interval input/sound may be incomplete; never full MATCH')


@measured('failure_localize')
def failure_prefix(folder):
    folder=Path(folder);spec=read(folder/'manifest.json');request=read(folder/'request.json');receipt=read(folder/'capture-receipt.json')
    # A failed dependency check is not trustworthy execution evidence
    bound=receipt.get('dependency_hashes')==request.get('dependency_hashes') and bool(receipt.get('dependency_hashes'))
    if not bound:return dict(diagnostic_only=True,passed=False,verified_prefix_end=None,first_difference=None,problem='Capture dependencies were not bound')
    for key,path in request['dependencies'].items():
        if digest(path)!=request['dependency_hashes'][key]:
            return dict(diagnostic_only=True,passed=False,verified_prefix_end=None,first_difference=None,problem='Capture dependency changed: '+key)
    config=read(folder/'capture.json');results=[];captured={}
    configs=config.get('segments')
    if configs:
        for segment,path in zip(contract_segments(spec['contract']),configs):
            cfg=read(path);native={k:cfg['audit_output']+'.'+k for k in ('actors','world','inputs')};native['phases']=cfg['output']
            if not Path(native['phases']).exists():break
            captured.update({str(Path(p).resolve()):digest(p) for p in native.values() if Path(p).is_file()})
            result=compare_channels(spec,native,segment['start_phase'],segment['end_phase']);results.append(result)
            if not result['window_match']:break
    else:
        captured.update({str(Path(p).resolve()):digest(p) for p in spec['native'].values() if Path(p).is_file()})
        results=[compare_channels(spec,spec['native'],spec['contract']['phase_begin'],spec['contract']['phase_end'])]
    differences=[r['first_difference'] for r in results if r.get('first_difference')]
    first=min(differences,key=lambda d:d['ordinal']) if differences else None
    safe=spec['contract']['phase_begin']
    for result in results:
        safe=result['verified_prefix_end']
        if not result['window_match']:break
    value=dict(schema=1,diagnostic_only=True,passed=False,contract=spec['contract'],segments=results,
        first_difference=first,verified_prefix_end=safe,source_sha256=spec['raw_sha256'],
        capture_receipt_sha256=digest(folder/'capture-receipt.json'),
        artifact_hashes=dict(captured,**{str(Path(p).resolve()):digest(p) for p in spec['original'].values()}),
        scope='Prefix diagnosis after capture failure; terminal failure is separate; no acceptance')
    write_receipt(folder/'prefix-comparison.json',value)
    return value

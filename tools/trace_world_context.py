"""Resolve a serialized world byte using the active project's capture layout"""
from trace_layout import PROFILE
from xport_project import load_project


def gpu_detail(left,right):
    from gpu_packet_semantics import canonical_packet, IGNORE_RAW_SPRITE_RGB
    for index in range(max(len(left),len(right))):
        a=left[index] if index<len(left) else None
        b=right[index] if index<len(right) else None
        if a is not None and b is not None and (a==b or canonical_packet(a,IGNORE_RAW_SPRITE_RGB)==canonical_packet(b,IGNORE_RAW_SPRITE_RGB)):continue
        offset=next((i for i,(x,y) in enumerate(zip(a or b'',b or b'')) if x!=y),None)
        return dict(packet=index,original_packets=len(left),native_packets=len(right),
                    raw_byte=offset,raw_word=offset//4 if offset is not None else None,
                    original=a[:256].hex() if a is not None else None,
                    native=b[:256].hex() if b is not None else None,
                    note='Raw byte context; ignored padding may precede the semantic difference')


def describe(left,right):
    a,b=left[2],right[2]
    offsets=[i for i,(x,y) in enumerate(zip(a,b)) if x!=y]
    result=dict(original_count=left[1],native_count=right[1],different_bytes=len(offsets))
    if left[1]!=right[1]:return dict(result,reason='Object counts differ; later offsets are not aligned')
    regions=[('world_a',PROFILE['world_a_size']),('world_b',PROFILE['world_b_size']),
             ('world_dynamic',PROFILE['world_item_size']*left[1]),('world_c',PROFILE['world_c_size']),('scalars',8)]
    _,project=load_project();fields=project.get('trace_layout',{}).get('world_fields',[])
    decoded=[]
    for offset in offsets[:16]:
        begin=0
        for name,size in regions:
            if begin<=offset<begin+size:
                local=offset-begin;stride=PROFILE['world_item_size'] if name in ('world_a','world_b','world_dynamic') else None
                field=local%stride if stride else local
                address=PROFILE.get(name)
                if name=='scalars':
                    address=PROFILE.get('world_scalar_a' if local<4 else 'world_scalar_b')
                    address=address+(local%4) if isinstance(address,int) else None
                elif isinstance(address,int):address+=local
                decoded.append(dict(offset=offset,pool=name,slot=local//stride if stride else None,
                    field_offset=field,field=next((f[2] for f in fields if stride and f[0]<=field<f[0]+f[1]),None),
                    address=hex(address) if isinstance(address,int) else None,
                    original=a[max(begin,offset-4):offset+8].hex(),native=b[max(begin,offset-4):offset+8].hex()))
                break
            begin+=size
    return dict(result,bytes=decoded,truncated=len(offsets)>16)

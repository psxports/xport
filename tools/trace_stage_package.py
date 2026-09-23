"""Validate coherent stage entry packages and bind them to finalized trace phases"""
import argparse
import json
from pathlib import Path
import struct

from trace_cache import digest
from trace_worker import write_receipt
from xport_project import artifact_path

MAGIC = 0x31545358
SECTIONS = {1:'ram', 2:'scratchpad', 3:'cpu', 4:'gpu', 5:'spu'}


def packages(source):
    with Path(source).open('rb') as stream:
        def take(size):
            value = stream.read(size)
            if len(value) != size:
                raise ValueError('Truncated stage package')
            return value
        magic, schema, device_version = struct.unpack('<3I',take(12))
        if magic != MAGIC or schema != 1 or device_version != 85:
            raise ValueError('Unsupported stage package/device schema')
        last = -1
        complete = False
        while envelope := stream.read(8):
            if len(envelope) != 8 or complete:
                raise ValueError('Truncated or trailing stage envelope')
            kind,length = struct.unpack('<2I',envelope)
            if kind == 2:
                if length != 4 or take(4) != struct.pack('<I',1):
                    raise ValueError('Unsuccessful stage recording')
                complete = True
                continue
            if kind != 1 or not 36 <= length <= 64*1024*1024:
                raise ValueError('Invalid stage package envelope')
            payload = take(length)
            ordinal,pc,tick,stage,frame,lo,hi,cursor,count = struct.unpack_from('<9I',payload)
            if ordinal <= last or count != len(SECTIONS):
                raise ValueError('Out-of-order package or incorrect section count')
            last=ordinal
            offset=36
            sections={}
            for _ in range(count):
                if offset+8 > len(payload):
                    raise ValueError('Missing stage section')
                section,size = struct.unpack_from('<2I',payload,offset)
                offset+=8
                if section not in SECTIONS or section in sections or not 0 < size <= 16*1024*1024 or offset+size > len(payload):
                    raise ValueError('Invalid or duplicate stage section')
                sections[section]=payload[offset:offset+size]
                offset+=size
            if offset != len(payload) or len(sections[1]) != 0x200000 or len(sections[2]) != 1024:
                raise ValueError('Incorrect RAM/scratchpad or trailing bytes')
            yield dict(ordinal=ordinal,pc=pc,game_tick=tick,stage=stage,frame=frame,
                       emulated_ticks=lo | hi<<32,raw_input_cursor=cursor,device_version=device_version,sections=sections)
        if not complete:
            raise ValueError('Stage package lacks committed footer')


def export(session, output):
    source = Path(session['runtime'])/'savestates'/('ff-stage-'+session['name']+'.bin')
    if not source.exists():
        return dict(status='unavailable',reason='Recording has no stage package sidecar')
    if session['status'] != 'complete':
        raise ValueError('Finalize raw trace before exporting stage packages')
    decoded=Path(session['decoded_directory'])
    phases=json.loads((decoded/'phase-boundaries.json').read_text())
    calls=json.loads((decoded/'game-input-calls.json').read_text())
    source_sha=digest(source)
    index=[]
    # Validate the completion footer and every package before publishing files
    records=list(packages(source))
    for record in records:
        ordinal=record['ordinal']
        if ordinal >= len(phases):
            raise ValueError('Package boundary outside trace')
        phase=phases[ordinal]
        for key in ('ordinal','pc','game_tick','stage','frame','emulated_ticks'):
            if record[key] != phase[key]:
                raise ValueError('Package differs from phase: '+key)
        cursor=record['raw_input_cursor']
        if cursor > len(calls) or (cursor and calls[cursor-1]['emulated_ticks'] > record['emulated_ticks']) or (cursor < len(calls) and calls[cursor]['emulated_ticks'] < record['emulated_ticks']):
            raise ValueError('Package input cursor inconsistent with recorded calls')
    output=artifact_path(output)
    output.mkdir(parents=True,exist_ok=True)
    for record in records:
        row={k:v for k,v in record.items() if k!='sections'}
        row['sections']={}
        for section,data in record['sections'].items():
            name=SECTIONS[section]
            path=output/f"phase-{record['ordinal']:010d}.{name}"
            if path.exists() and path.read_bytes()!=data:
                raise ValueError('Refusing to replace a changed stage package')
            if not path.exists():
                path.write_bytes(data)
            row['sections'][name]=dict(path=str(path),size=len(data),sha256=digest(path))
        index.append(row)
    if digest(source)!=source_sha:
        raise ValueError('Stage sidecar changed during export')
    result=dict(schema=1,status='captured',source=str(source),source_sha256=source_sha,
        raw_sha256=session['raw_sha256'],phase_metadata_sha256=digest(decoded/'phase-boundaries.json'),
        input_metadata_sha256=digest(decoded/'game-input-calls.json'),packages=index,
        scope='Coherent guest CPU/GPU/SPU state and RAM/scratchpad at configured stage entry; native wrapper conversion and continuation remain required')
    write_receipt(output/'index.json',result)
    return dict(status='captured',packages=len(index),index=str(output/'index.json'),index_sha256=digest(output/'index.json'))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--session',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    print(json.dumps(export(json.loads(args.session.read_text()),args.output)))


if __name__=='__main__':
    main()

"""Extract original channels by phase position rather than resetting game ticks"""
import json
from pathlib import Path
import struct


def extract(source,begin,end,directory,metadata=None):
    if not 0<=begin<end:raise ValueError('Invalid phase interval')
    directory=Path(directory)
    paths={name:directory/('original.'+name) for name in ('actors','world','inputs')}
    ordinal=-1;inputs=[];complete=False;terminal_inputs=0
    with Path(source).open('rb') as stream,paths['actors'].open('wb') as actors,paths['world'].open('wb') as world:
        header=stream.read(12)
        if len(header)!=12 or struct.unpack_from('<I',header)[0]!=0x32544646:raise ValueError('Expected original user trace')
        size=Path(source).stat().st_size
        while raw:=stream.read(12):
            if complete or len(raw)!=12:raise ValueError('Trailing/truncated trace envelope')
            kind,tick,length=struct.unpack('<3I',raw)
            if not 1<=kind<=18 or length>32*1024*1024 or stream.tell()+length>size:raise ValueError('Invalid raw envelope')
            if kind==15:ordinal+=1
            selected=begin<=ordinal<end
            if kind==5:
                if stream.read(length)!=struct.pack('<I',1):raise ValueError('Uncommitted original trace')
                complete=True
            elif selected and kind in (1,2,3):
                payload=stream.read(length)
                if kind in (1,2):
                    (actors if kind==1 else world).write(struct.pack('<I',tick)+payload)
                else:
                    if length!=8:raise ValueError('Invalid decoded input')
                    inputs.append([tick,*struct.unpack('<2I',payload)])
                    if ordinal==end-1:terminal_inputs+=1
            else:stream.seek(length,1)
    if not complete or ordinal+1<end:raise ValueError('Incomplete phase interval')
    paths['inputs'].write_text(json.dumps(inputs)+'\n')
    if metadata is not None:metadata['terminal_input_records']=terminal_inputs
    return paths

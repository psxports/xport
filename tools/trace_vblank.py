"""Validate legacy/page-deduplicated VBlank snapshots and reconstruct RAM"""
import hashlib
import struct

RAM_SIZE = 0x200000+1024
PAGE_COUNT = 513


def page_size(page):
    if not 0 <= page < PAGE_COUNT:raise ValueError('Invalid RAM page')
    return 1024 if page == 512 else 4096


def content_entries(payload):
    if len(payload) < 20:raise ValueError('Truncated VBlank header')
    frame, low, high, mode, count = struct.unpack_from('<5I', payload)
    if mode not in (2,3) or count > PAGE_COUNT:raise ValueError('Invalid content-addressed VBlank header')
    position=20;entries=[]
    for _ in range(count):
        need=12 if mode==2 else 8
        if position+need>len(payload):raise ValueError('Truncated content-addressed page entry')
        page,content=struct.unpack_from('<2I',payload,position);position+=8
        page_size(page)
        if not content:raise ValueError('Invalid page content identity')
        data=None
        if mode==2:
            present=struct.unpack_from('<I',payload,position)[0];position+=4
            if present not in (0,1):raise ValueError('Invalid page definition flag')
            if present:
                size=page_size(page)
                if position+size>len(payload):raise ValueError('Truncated page definition')
                data=payload[position:position+size];position+=size
        entries.append((page,content,data))
    if position!=len(payload):raise ValueError('Trailing VBlank payload')
    return frame,low | high<<32,mode,entries


class Decoder:
    def __init__(self, retain_catalog=False):
        self.memory=None;self.index=[];self.keyframe=None
        self.contents={};self.page_ids=[0]*PAGE_COUNT;self.retain_catalog=retain_catalog
        self.statistics={'records':0,'payload_bytes':0,'legacy_records':0,'content_records':0,
                         'page_definitions':0,'page_definition_bytes':0,'page_references':0,
                         'reference_keyframes':0,'reference_keyframe_bytes':0}

    def preload(self, content, data):
        prior=self.contents.get(content)
        if prior is not None and prior!=data:raise ValueError('Content identity collision')
        self.contents[content]=data

    def accept(self, payload, tick, offset):
        if len(payload)<20:raise ValueError('Truncated VBlank header')
        frame,low,high,mode,count=struct.unpack_from('<5I',payload);clock=low | high<<32
        if self.index and (frame!=self.index[-1]['frame']+1 or clock<=self.index[-1]['emulated_ticks']):
            raise ValueError('VBlank sequence or emulated clock discontinuity')
        definitions=[];references=[]
        self.statistics['records']+=1;self.statistics['payload_bytes']+=len(payload)
        if mode in (0,1):
            self.statistics['legacy_records']+=1
            if count>PAGE_COUNT or (mode==1 and count!=PAGE_COUNT):raise ValueError('Invalid VBlank page count')
            if mode==1:self.memory=bytearray(RAM_SIZE);self.keyframe=offset
            elif self.memory is None:raise ValueError('Delta without initial memory keyframe')
            position=20;previous=-1
            for _ in range(count):
                if position+4>len(payload):raise ValueError('Truncated page index')
                page=struct.unpack_from('<I',payload,position)[0];position+=4;size=page_size(page)
                if page<=previous or position+size>len(payload):raise ValueError('Invalid, duplicate or truncated RAM page')
                self.memory[page*4096:page*4096+size]=payload[position:position+size]
                previous=page;position+=size
            if position!=len(payload):raise ValueError('Trailing VBlank payload')
        elif mode in (2,3):
            self.statistics['content_records']+=1
            frame,clock,mode,entries=content_entries(payload)
            if mode==3:
                if len(entries)!=PAGE_COUNT:raise ValueError('Reference keyframe must map every page')
                self.memory=bytearray(RAM_SIZE);self.keyframe=offset
            elif self.memory is None:
                if len(entries)!=PAGE_COUNT:raise ValueError('Initial content frame must map every page')
                self.memory=bytearray(RAM_SIZE);self.keyframe=offset
            previous=-1
            for page,content,data in entries:
                if page<=previous:raise ValueError('Invalid or duplicate content page')
                previous=page;references.append(content)
                if data is not None:self.preload(content,data);definitions.append(content)
                if content not in self.contents:raise ValueError('Reference to undefined page content')
                value=self.contents[content]
                if len(value)!=page_size(page):raise ValueError('Page content size disagrees with destination')
                self.memory[page*4096:page*4096+len(value)]=value;self.page_ids[page]=content
            self.statistics['page_definitions']+=len(definitions)
            self.statistics['page_definition_bytes']+=sum(len(self.contents[x]) for x in definitions)
            self.statistics['page_references']+=len(references)
            if mode==3:
                self.statistics['reference_keyframes']+=1
                self.statistics['reference_keyframe_bytes']+=len(payload)
            if not self.retain_catalog:
                live=set(self.page_ids);live.discard(0)
                self.contents={key:value for key,value in self.contents.items() if key in live}
        else:raise ValueError('Invalid VBlank mode')
        entry={'frame':frame,'emulated_ticks':clock,'game_tick':tick,'offset':offset,
               'keyframe_offset':self.keyframe,'memory_sha256':hashlib.sha256(self.memory).hexdigest()}
        if mode in (2,3):entry.update(mode=mode,definitions=definitions)
        self.index.append(entry);return entry


def read_payload(source,offset):
    with source.open('rb') as stream:
        stream.seek(offset);header=stream.read(12)
        if len(header)!=12:raise ValueError('Truncated indexed record')
        kind,tick,size=struct.unpack('<III',header)
        if kind!=8 or size>RAM_SIZE+PAGE_COUNT*16:raise ValueError('Index does not point to a VBlank record')
        payload=stream.read(size)
        if len(payload)!=size:raise ValueError('Truncated indexed VBlank')
        return tick,payload


def reconstruct(source,index,frame):
    target=next(row for row in index if row['frame']==frame)
    selected=[row for row in index if row['offset']>=target['keyframe_offset'] and row['frame']<=frame]
    decoder=Decoder(retain_catalog=True)
    if selected and selected[0].get('mode') in (2,3):
        needed=set()
        for row in selected:
            _,payload=read_payload(source,row['offset'])
            needed.update(content for _,content,_ in content_entries(payload)[3])
        sources={content:row['offset'] for row in index if row['frame']<=frame for content in row.get('definitions',[])}
        if not needed<=sources.keys():raise ValueError('Missing indexed page definition')
        for offset in sorted(set(sources[content] for content in needed)):
            _,payload=read_payload(source,offset)
            for page,content,data in content_entries(payload)[3]:
                if content in needed and data is not None:decoder.preload(content,data)
    for row in selected:
        tick,payload=read_payload(source,row['offset']);actual=decoder.accept(payload,tick,row['offset'])
        if actual!=row:raise ValueError('Indexed memory evidence differs')
    return bytes(decoder.memory)

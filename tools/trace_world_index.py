"""Seek original world boundaries using a content-bound ordinal index"""
import json
from pathlib import Path
import struct
from pipeline_metrics import measured
from trace_bundle import worlds
from trace_cache import digest
from trace_layout import PROFILE_PATH
from trace_finalize_lock import exclusive_finalize
from trace_worker import write_receipt
from xport_project import artifact_path


@measured('world_index')
def index(path):
    path=Path(path);sha=digest(path);profile=digest(PROFILE_PATH)
    folder=artifact_path('status/stage-pipeline/channel-index');folder.mkdir(parents=True,exist_ok=True)
    meta=folder/(sha+'-'+profile[:16]+'.json');binary=meta.with_suffix('.bin')
    with exclusive_finalize(folder):
        if meta.exists():
            value=json.loads(meta.read_text())
            if value['source_sha256']!=sha or value['profile_sha256']!=profile or digest(binary)!=value['offsets_sha256']:
                raise ValueError('World index identity changed')
            return value,binary
        offsets=[]
        for _ in worlds(path,observe_offset=offsets.append):pass
        temporary=binary.with_suffix('.pending')
        temporary.write_bytes(b''.join(struct.pack('<Q',offset) for offset in offsets));temporary.replace(binary)
        value=dict(source_sha256=sha,profile_sha256=profile,records=len(offsets),source_bytes=path.stat().st_size,offsets_sha256=digest(binary))
        write_receipt(meta,value);return value,binary


def seek(path,ordinal):
    if not ordinal:yield from worlds(path);return
    value,binary=index(path)
    if not 0<=ordinal<value['records']:raise ValueError('World ordinal outside indexed source')
    with binary.open('rb') as stream:
        stream.seek(ordinal*8);data=stream.read(8)
    if len(data)!=8:raise ValueError('Truncated world index')
    offset=struct.unpack('<Q',data)[0]
    if offset>=value['source_bytes']:raise ValueError('World index offset outside source')
    yield from worlds(path,offset=offset)

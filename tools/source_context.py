"""Return a content-bound, bounded audit context for one original function"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sqlite3
from xport_project import load_project, artifact_path


def digest(path):
    h=hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda:stream.read(1024*1024),b''):h.update(block)
    return h.hexdigest()


def checked_text(root,db,relative,cache):
    if relative in cache:return cache[relative]
    row=db.execute('SELECT sha256 FROM source_files WHERE path=?',(relative,)).fetchone()
    if not row:raise ValueError('Source is not indexed: '+relative)
    path=(root/relative).resolve()
    if not path.is_relative_to((root/'src').resolve()) or not path.is_file():raise ValueError('Indexed source path is invalid: '+relative)
    if digest(path)!=row['sha256']:raise ValueError('Indexed source changed; run build_database: '+relative)
    cache[relative]=path.read_bytes().decode('utf-8-sig');return cache[relative]


def slice_record(root,db,row,cache):
    text=checked_text(root,db,row['source_path'],cache)
    body=text[row['start_offset']:row['end_offset']]
    if hashlib.sha256(body.encode()).hexdigest()!=row['body_sha256']:raise ValueError('Indexed function range changed')
    return body


def declaration_records(root,db,image,address,cache):
    rows=db.execute('''SELECT d.*,x.reason FROM implementation_declarations x JOIN source_declarations d ON d.id=x.declaration
                       WHERE x.image=? AND x.address=? ORDER BY CASE d.kind WHEN 'prototype' THEN 0 WHEN 'type' THEN 1 WHEN 'object' THEN 2 ELSE 3 END,d.path,d.start_line''',(image,address)).fetchall()
    result=[]
    for row in rows:
        value=dict(row);text=checked_text(root,db,row['path'],cache);snippet=text[row['start_offset']:row['end_offset']]
        if hashlib.sha256(snippet.encode()).hexdigest()!=row['sha256']:raise ValueError('Indexed declaration range changed')
        result.append(dict(path=row['path'],name=row['name'],kind=row['kind'],start_line=row['start_line'],end_line=row['end_line'],sha256=row['sha256'],text=snippet,reason=row['reason']))
    return result


def excerpt(lines,center,count):
    if not lines:return dict(first_line=1,lines=[],complete=True)
    center=max(0,min(len(lines)-1,center));start=max(0,center-count//3);end=min(len(lines),start+count);start=max(0,end-count)
    return dict(first_line=start+1,lines=lines[start:end],complete=start==0 and end==len(lines),total_lines=len(lines))


def original_excerpt(root,path,address=None,count=48,line=None):
    if not path:return dict(path=None,sha256=None,first_line=1,lines=[],complete=False,total_lines=0)
    source=(root/path).resolve()
    if not source.is_file():return dict(path=path,sha256=None,first_line=1,lines=[],complete=False,total_lines=0,problem='missing')
    lines=source.read_text(errors='replace').splitlines();center=max(0,(line or 1)-1)
    if address is not None and line is None:
        center=next((i for i,line in enumerate(lines) if re.match(r'^\s*'+f'{address:08X}',line,re.I)),0)
    return dict(path=path,sha256=digest(source),**excerpt(lines,center,count))


def limited(rows,limit=8):return dict(count=len(rows),rows=[dict(x) for x in rows[:limit]],truncated=len(rows)>limit)


def compact(image,address,budget=12000,full_source=False,mips_address=None,source_line=None,pseudo_line=None):
    if not 4096<=budget<=65536:raise ValueError('Budget must be 4096..65536 bytes')
    root,project=load_project();database=root/project['paths']['analysis_database'];cache={}
    with sqlite3.connect(database.as_uri()+'?mode=ro',uri=True) as db:
        db.row_factory=sqlite3.Row
        required={'implementations','source_files','implementation_declarations'}
        tables={r[0] for r in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}
        if not required<=tables:raise ValueError('Source index is absent; run build_database')
        function=db.execute('SELECT * FROM functions WHERE image=? AND address=?',(image,address)).fetchone()
        if not function:raise ValueError('Function entry not found for image/address')
        implementation=db.execute('SELECT * FROM implementations WHERE image=? AND address=?',(image,address)).fetchone()
        if not implementation:raise ValueError('Implementation index entry is absent')
        impl=dict(implementation);body=None;body_view=None
        if impl['mapping_status']=='mapped':
            body=slice_record(root,db,implementation,cache);lines=body.splitlines()
            body_view=dict(path=impl['source_path'],symbol=impl['symbol'],start_line=impl['start_line'],end_line=impl['end_line'],sha256=impl['body_sha256'],complete=True,text=body)
            if not full_source and len(json.dumps(body_view).encode())>budget//2:
                center=(source_line-impl['start_line']) if source_line else 0
                view=excerpt(lines,center,max(16,min(60,budget//180)))
                body_view.update(complete=view['complete'],first_line=impl['start_line']+view['first_line']-1,lines=view['lines'],total_lines=view['total_lines']);body_view.pop('text',None)
        declarations=declaration_records(root,db,image,address,cache) if body is not None else []
        callees=list(db.execute('SELECT site,target,kind,resolution FROM edges WHERE source_image=? AND source_function=? ORDER BY site',(image,address)))
        callers=list(db.execute('SELECT e.source_image,e.source_function,e.site,e.kind,e.resolution FROM edges e JOIN edge_candidates c ON c.edge=e.id WHERE c.target_image=? AND c.target_function=? ORDER BY e.source_image,e.source_function,e.site',(image,address)))
        peers=list(db.execute('SELECT peer_image,peer_address,kind,size_delta,control_shape_equal,risks FROM function_reuse WHERE image=? AND address=? ORDER BY kind,peer_image,peer_address',(image,address)))
        identifiers=[r[0] for r in db.execute('SELECT identifier FROM implementation_identifiers WHERE image=? AND address=? ORDER BY identifier',(image,address))]
        ledger=json.loads((root/'status/translation-ledger.json').read_text(encoding='utf-8-sig'))
        evidence=next((x for x in ledger.get('entries',[]) if x['image']==image and x['address']==address),None)
        result=dict(schema=1,image=image,address=hex(address),function=dict(function),implementation=body_view or dict(path=impl['source_path'],mapping_status=impl['mapping_status'],mapping_basis=impl['mapping_basis']),
            declarations=declarations,identifiers=dict(count=len(identifiers),names=identifiers[:64],truncated=len(identifiers)>64),
            callees=limited(callees),callers=limited(callers),reuse_peers=limited(peers),
            mips=original_excerpt(root,function['listing'],mips_address or address,56),
            pseudocode=original_excerpt(root,function['pseudocode'],None,40,pseudo_line),
            evidence=evidence,gaps=[])
    if impl['mapping_status']!='mapped':result['gaps'].append('No unique explicit address-to-C-function mapping: '+impl['mapping_basis'])
    if function['boundary_status']!='audited':result['gaps'].append('Original boundary status: '+function['boundary_status'])
    if function['status']!='DONE':result['gaps'].append('Implementation status is '+function['status'])
    if body_view and not body_view['complete']:result['gaps'].append('C body exceeds bounded view; use --full-source or --source-line')
    # Preserve exact C/declaration context before secondary original-code excerpts
    for key,minimum in (('pseudocode',8),('mips',16)):
        while len(result[key]['lines'])>minimum and len(json.dumps(result,ensure_ascii=True).encode())>budget:
            result[key]['lines'].pop();result[key]['complete']=False
    for row in reversed(result['declarations']):
        if len(json.dumps(result,ensure_ascii=True).encode())<=budget:break
        row.pop('text',None);row['text_omitted']=True
    for key in ('pseudocode','mips'):
        while result[key]['lines'] and len(json.dumps(result,ensure_ascii=True).encode())>budget:
            result[key]['lines'].pop();result[key]['complete']=False
    while result['declarations'] and len(json.dumps(result,ensure_ascii=True).encode())>budget:
        result['declarations'].pop();result['declarations_truncated']=True
    if len(json.dumps(result,ensure_ascii=True).encode())>budget and body_view and full_source:
        raise ValueError('Exact C body does not fit requested budget; raise --budget or omit --full-source')
    result['serialized_bytes']=len(json.dumps(result,ensure_ascii=True).encode());result['budget_bytes']=budget
    return result


def parse_address(value):
    return int(value, 0) if value.lower().startswith('0x') else int(value, 16)


def main():
    root,project=load_project();default=project.get('analysis',{}).get('default_image')
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('address',type=parse_address);p.add_argument('--image',default=default)
    p.add_argument('--budget',type=int,default=12000);p.add_argument('--full-source',action='store_true');p.add_argument('--mips-address',type=parse_address);p.add_argument('--source-line',type=int);p.add_argument('--pseudo-line',type=int);p.add_argument('--output',type=Path)
    a=p.parse_args()
    if not a.image:p.error('--image is required')
    value=compact(a.image,a.address,a.budget,a.full_source,a.mips_address,a.source_line,a.pseudo_line)
    if a.output:
        path=artifact_path(a.output);path.parent.mkdir(parents=True,exist_ok=True);path.write_text(json.dumps(value,indent=2)+'\n');print(json.dumps(dict(output=str(path),sha256=digest(path),serialized_bytes=value['serialized_bytes'])))
    else:print(json.dumps(value,separators=(',',':')))


if __name__=='__main__':main()

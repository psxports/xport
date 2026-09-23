"""Build image-aware SQLite, callgraph and progress from verified IDA exports."""
from pathlib import Path
import sqlite3,json,hashlib,datetime,html
from xport_project import project_root, project_path
R=project_root()
def read(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def write(p,d):p.write_text(json.dumps(d,indent=2)+'\n',encoding='utf8')
def main():
 from source_index import format_sources
 source_paths=[p.relative_to(R).as_posix() for p in (R/'src').rglob('*') if p.is_file() and p.suffix.lower() in ('.c','.h')]
 source_format=format_sources(R,source_paths)
 classification_path=project_path('sdk_classification','status/ghidra/classification.json')
 classifications=read(classification_path)['accepted'] if classification_path.exists() else []
 sdk={(x['image'],x['address']):x for x in classifications}
 ledger_path=R/'status/translation-ledger.json'
 ledger=read(ledger_path)['entries'] if ledger_path.exists() else []
 translations={(x['image'],x['address']):x for x in ledger}
 assert len(translations)==len(ledger),'Duplicate translation identity'
 translated_seen=set()
 path=project_path('analysis_database', 'status/analysis.sqlite');path.parent.mkdir(parents=True,exist_ok=True);tmp=path.with_suffix('.sqlite.tmp')
 if tmp.exists():tmp.unlink()
 db=sqlite3.connect(tmp);db.executescript('''
 PRAGMA foreign_keys=ON;
 CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT);
 CREATE TABLE library_classification(image TEXT,address INTEGER,alias TEXT,reason TEXT,evidence TEXT,PRIMARY KEY(image,address));
 CREATE TABLE images(id TEXT PRIMARY KEY,source TEXT,sha256 TEXT,base INTEGER,file_offset INTEGER,size INTEGER,gp INTEGER,entry INTEGER);
 CREATE TABLE functions(image TEXT,address INTEGER,end INTEGER,name TEXT,status TEXT,bytes INTEGER,sha256 TEXT,boundary_status TEXT,listing TEXT,pseudocode TEXT,PRIMARY KEY(image,address),FOREIGN KEY(image) REFERENCES images(id));
 CREATE TABLE chunks(image TEXT,function INTEGER,start INTEGER,end INTEGER,PRIMARY KEY(image,function,start),FOREIGN KEY(image,function) REFERENCES functions(image,address));
 CREATE TABLE instructions(image TEXT,function INTEGER,address INTEGER,word TEXT,bytes TEXT,disassembly TEXT,delay_slot INTEGER,PRIMARY KEY(image,function,address),FOREIGN KEY(image,function) REFERENCES functions(image,address));
 CREATE TABLE symbols(image TEXT,address INTEGER,name TEXT,kind TEXT,size INTEGER,confidence TEXT,PRIMARY KEY(image,address),FOREIGN KEY(image) REFERENCES images(id));
 CREATE TABLE data_refs(image TEXT,function INTEGER,site INTEGER,target INTEGER,type INTEGER,FOREIGN KEY(image,function) REFERENCES functions(image,address));
 CREATE TABLE edges(id INTEGER PRIMARY KEY,source_image TEXT,source_function INTEGER,site INTEGER,target INTEGER,kind TEXT,resolution TEXT,FOREIGN KEY(source_image,source_function) REFERENCES functions(image,address));
 CREATE TABLE edge_candidates(edge INTEGER,target_image TEXT,target_function INTEGER,PRIMARY KEY(edge,target_image,target_function),FOREIGN KEY(edge) REFERENCES edges(id),FOREIGN KEY(target_image,target_function) REFERENCES functions(image,address));
 CREATE TABLE indirect_transfers(image TEXT,function INTEGER,site INTEGER,instruction TEXT,status TEXT,FOREIGN KEY(image,function) REFERENCES functions(image,address));
 CREATE TABLE decompilation_failures(image TEXT,function INTEGER,error TEXT,FOREIGN KEY(image,function) REFERENCES functions(image,address));
 CREATE TABLE pointer_candidates(image TEXT,site INTEGER,target INTEGER,classification TEXT);
 CREATE INDEX instructions_address ON instructions(image,address);
 CREATE INDEX data_target ON data_refs(image,target);
 CREATE INDEX edges_target ON edges(target);
 CREATE INDEX edges_source ON edges(source_image,source_function);
 CREATE VIEW variables AS SELECT * FROM symbols WHERE kind='data';
 ''')
 exported={};configs={};by_address={};metrics=[];graph=[];nodes=[];failures=[];gaps=[]
 for out in sorted(project_path('ida_exports', 'status/ida/images').iterdir()):
  image=out.name;cfg=read(project_path('ida_configs', 'tools/ida')/(image+'.json'));configs[image]=cfg;src=cfg['loads'][-1];source_path=Path(src['path']);source_path=source_path if source_path.is_absolute() else R/source_path;raw=source_path.read_bytes();offset=src.get('offset',0);base=src['base']
  db.execute('INSERT INTO images VALUES(?,?,?,?,?,?,?,?)',(image,str(source_path.resolve()),hashlib.sha256(raw).hexdigest(),base,offset,len(raw),cfg['gp'],cfg['entry']))
  funcs=read(out/'functions.json');exported[image]=funcs
  for f in funcs:
   if (image,f['address']) in sdk:
    match=sdk[(image,f['address'])];assert f['sha256']==match['sha256'];f=dict(f,status='SKIP')
    db.execute('INSERT INTO library_classification VALUES(?,?,?,?,?)',(image,f['address'],match['alias'],match['reason'],json.dumps(match)))
   key=(image,f['address'])
   if key in translations:
    t=translations[key]
    assert t['sha256']==f['sha256'],'Stale translation evidence: '+str(key)
    assert t['status'] in ['TODO','WIP','DONE','SKIP']
    assert (R/t['source']).is_file() and all((R/p).is_file() for p in t['evidence'])
    if t['status']=='DONE':
     assert t.get('static_evidence') and t.get('runtime_evidence'),'DONE needs static and runtime evidence'
    f=dict(f,status=t['status']);translated_seen.add(key)
   by_address.setdefault(f['address'],[]).append(key)
   listing=(out/'functions'/('%08X.lst'%f['address'])).relative_to(R).as_posix();pseudo=(out/f['pseudocode']).relative_to(R).as_posix() if f['pseudocode'] else None
   db.execute('INSERT INTO functions VALUES(?,?,?,?,?,?,?,?,?,?)',(image,f['address'],f['end'],f['name'],f['status'],f['size'],f['sha256'],f['boundary_status'],listing,pseudo))
   db.executemany('INSERT INTO chunks VALUES(?,?,?,?)',[(image,f['address'],a,b) for a,b in f['chunks']])
   db.executemany('INSERT INTO instructions VALUES(?,?,?,?,?,?,?)',[(image,f['address'],i['address'],i['word'],i['bytes'],i['text'],i['delay_slot']) for i in f['raw_instructions']])
   nodes.append(dict(id='%s:%08X'%key,image=image,address=f['address'],name=f['name'],status=f['status'],listing=listing,pseudocode=pseudo))
  symbols={s['address']:s for s in read(out/'symbols.json')}
  refs=read(out/'data-xrefs.json')
  for x in refs:symbols.setdefault(x['target'],dict(address=x['target'],name='DAT_%08X'%x['target'],kind='data',size=None))
  for f in funcs:symbols[f['address']]=dict(address=f['address'],name=f['name'],kind='code',size=f['size'])
  db.executemany('INSERT INTO symbols VALUES(?,?,?,?,?,?)',[(image,s['address'],s['name'],s['kind'],s.get('size'),'IDA inferred; not debugging symbols') for s in symbols.values()])
  db.executemany('INSERT INTO data_refs VALUES(?,?,?,?,?)',[(image,x['source'],x['site'],x['target'],x['type']) for x in refs])
  for f in read(out/'decompilation-failures.json'):
   db.execute('INSERT INTO decompilation_failures VALUES(?,?,?)',(image,f['address'],f['error']));failures.append(dict(image=image,**f))
  covered={i['address'] for f in funcs for i in f['raw_instructions']};inside={a for lo,hi in cfg['code_ranges'] for a in range(lo,hi,4)}
  missing=sorted(inside-covered)
  for a in missing:
   if gaps and gaps[-1]['image']==image and gaps[-1]['end']==a:gaps[-1]['end']=a+4
   else:gaps.append(dict(image=image,start=a,end=a+4,status='unclassified_gap_or_padding'))
  # Potential data function pointers are candidates, never silently added as call edges.
  ptrs=[]
  for i in range(0,len(raw)-offset-3,4):
   site=base+i
   if site in covered:continue
   target=int.from_bytes(raw[offset+i:offset+i+4],'little')
   if any(lo<=target<hi and target%4==0 for lo,hi in cfg['code_ranges']):ptrs.append((image,site,target,'candidate_requires_reader_or_dispatch_proof'))
  db.executemany('INSERT INTO pointer_candidates VALUES(?,?,?,?)',ptrs)
  report=read(out/'export-report.json')
  metrics.append(dict(image=image,functions=len(funcs),pseudocode=report['pseudocode'],failures=report['failures'],instruction_words=len(covered),covered_code_bytes=len(inside&covered)*4,provisional_code_bytes=len(inside)*4,unclassified_gap_bytes=len(missing)*4,symbols=len(symbols),data_refs=len(refs),pointer_candidates=len(ptrs)))
 for image,funcs in exported.items():
  for f in funcs:
   seen=set()
   for i in f['raw_instructions']:
    a=i['address'];w=int(i['word'],16);op=w>>26;target=None;kind=None
    if op in [2,3]:target=((a+4)&0xf0000000)|((w&0x3ffffff)<<2);kind='call' if op==3 else 'tail_candidate'
    elif op==1 and (w>>16&31) in [16,17]:
     imm=w&65535;imm=imm-65536 if imm&32768 else imm;target=(a+4+imm*4)&0xffffffff;kind='conditional_link'
    elif op==0 and w&63 in [8,9] and not (w&63==8 and w>>21&31==31):
     db.execute('INSERT INTO indirect_transfers VALUES(?,?,?,?,?)',(image,f['address'],a,i['text'],'unresolved_static'));continue
    if target is None or kind=='tail_candidate' and any(lo<=target<hi for lo,hi in f['chunks']):continue
    if (a,target) in seen:continue
    seen.add((a,target));candidates=by_address.get(target,[])
    local=[c for c in candidates if c[0]==image]
    if not local:
     local=[(image,z['address']) for z in exported[image] if any(lo<=target<hi for lo,hi in z['chunks'])]
    if local:chosen=local;resolution='same_image'
    else:chosen=[];resolution='unresolved_target'
    cur=db.execute('INSERT INTO edges(source_image,source_function,site,target,kind,resolution) VALUES(?,?,?,?,?,?)',(image,f['address'],a,target,kind,resolution));eid=cur.lastrowid
    db.executemany('INSERT INTO edge_candidates VALUES(?,?,?)',[(eid,c[0],c[1]) for c in chosen])
    graph.append(dict(source='%s:%08X'%(image,f['address']),site=a,target=target,kind=kind,resolution=resolution,candidates=['%s:%08X'%c for c in chosen]))
 assert translated_seen==set(translations),'Translation refers to missing function'
 from function_similarity import run as compare_mips_functions
 similarity_summary=compare_mips_functions(db)
 from source_index import populate as index_sources
 source_summary=index_sources(db,R,ledger)
 for k,v in {'schema_version':'3','source':'IDA 7.7 + Hex-Rays; instruction bytes independently verified against files','identity':'image + virtual address; names are not debug symbols','status_policy':'TODO unless evidence-backed PsyQ replacement SKIP; aliases stored separately','source_index_parser':source_summary['parser_version'],'source_index_ledger_sha256':hashlib.sha256(ledger_path.read_bytes()).hexdigest(),'source_format_style_sha256':source_format['style_sha256'],'source_formatter_version':source_format['formatter'] or 'unavailable','created_utc':datetime.datetime.now(datetime.timezone.utc).isoformat()}.items():db.execute('INSERT INTO metadata VALUES(?,?)',(k,v))
 db.commit();assert db.execute('PRAGMA integrity_check').fetchone()[0]=='ok';assert not db.execute('PRAGMA foreign_key_check').fetchall()
 totals={k:sum(x[k] for x in metrics) for k in ['functions','pseudocode','failures','instruction_words','symbols','data_refs']};totals['edges']=len(graph);totals['indirect_transfers']=db.execute('SELECT count(*) FROM indirect_transfers').fetchone()[0];totals['unresolved_targets']=sum(e['resolution']=='unresolved_target' for e in graph);totals['overlay_context_edges']=sum(e['resolution']=='overlay_context_required' for e in graph)
 statuses={s:0 for s in ['TODO','WIP','DONE','SKIP']};statuses.update(dict(db.execute('SELECT status,count(*) FROM functions GROUP BY status')))
 db.close();tmp.replace(path)
 write(project_path('callgraph', 'status/callgraph.json'),dict(identity='image:address',nodes=nodes,edges=graph));write(R/'status/code-gaps.json',gaps);write(R/'status/decompilation-failures.json',failures)
 progress=dict(phase='environment preparation',decompilation_status=statuses,metrics=metrics,totals=totals,limitations=['Function boundaries and code/data classification are discovery results, not fully audited.','Indirect transfers and overlay-context edges require future proof.','Hex-Rays failures are explicit and do not suppress MIPS listings.','Runtime status is recorded separately; static exports do not validate gameplay.'])
 progress['function_similarity']=similarity_summary
 progress['source_index']=source_summary
 if ledger_path.exists():
  progress['translation_ledger']=read(ledger_path)
  progress['phase']='native decompilation'
 progress['source_format']=source_format
 write(R/'status/progress-data.json',progress)
 print(json.dumps(totals))
if __name__=='__main__':main()

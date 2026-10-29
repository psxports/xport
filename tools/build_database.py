"""Build image-aware SQLite, callgraph and progress from verified IDA exports."""
from pathlib import Path
import sqlite3,json,hashlib,datetime,html
from xport_project import load_project, project_root, project_path
from ida_ctree_schema import validate as validate_ctree
R=project_root()
def read(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def write(p,d):p.write_text(json.dumps(d,indent=2)+'\n',encoding='utf8')
def main(no_format=False):
 _,project_config=load_project(R)
 if project_config.get('analysis',{}).get('function_queue') not in (None,'image_qualified','unique_exact_bodies','operand_templates'):
  raise ValueError('Unsupported analysis.function_queue mode')
 from ida_index import select_exports, index_profile
 index_exports=select_exports(R,project_config)
 index_inputs=[dict(image=read(out/'export-report.json')['image'],path=out.relative_to(R).as_posix(),
                    functions_sha256=hashlib.sha256((out/'functions.json').read_bytes()).hexdigest())
               for out in index_exports]
 from source_index import format_sources, SOURCE_EXTENSIONS
 source_paths=[p.relative_to(R).as_posix() for p in (R/'src').rglob('*') if p.is_file() and p.suffix.lower() in SOURCE_EXTENSIONS]
 source_format=(dict(files=[],trimmed_files=[],formatter=None,style_sha256=hashlib.sha256((Path(__file__).resolve().parents[1]/"src/.clang-format").read_bytes()).hexdigest())
                if no_format else format_sources(R,source_paths))
 classification_path=project_path('sdk_classification','status/ghidra/classification.json')
 classifications=read(classification_path)['accepted'] if classification_path.exists() else []
 sdk={(x['image'],x['address']):x for x in classifications}
 ledger_path=R/'status/translation-ledger.json'
 from project_context import ledger_document, ledger_bytes
 ledger=ledger_document(R)['entries']
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
 CREATE TABLE ctree_functions(image TEXT,function INTEGER,path TEXT,sha256 TEXT,ida TEXT,hexrays TEXT,gp INTEGER,image_sha256 TEXT,prototype TEXT,PRIMARY KEY(image,function),FOREIGN KEY(image,function) REFERENCES functions(image,address));
 CREATE TABLE ctree_lvars(image TEXT,function INTEGER,id INTEGER,name TEXT,type TEXT,location TEXT,is_argument INTEGER,PRIMARY KEY(image,function,id),FOREIGN KEY(image,function) REFERENCES ctree_functions(image,function));
 CREATE TABLE ctree_nodes(image TEXT,function INTEGER,id INTEGER,kind TEXT,op INTEGER,op_name TEXT,ea INTEGER,parent INTEGER,children TEXT,type TEXT,variable TEXT,number INTEGER,PRIMARY KEY(image,function,id),FOREIGN KEY(image,function) REFERENCES ctree_functions(image,function));
 CREATE TABLE pointer_candidates(image TEXT,site INTEGER,target INTEGER,classification TEXT);
 CREATE TABLE semantic_aliases(image TEXT,address INTEGER,kind TEXT,original_name TEXT,semantic_name TEXT,binding TEXT,status TEXT,legacy_status TEXT,source TEXT,sha256 TEXT,PRIMARY KEY(image,address,kind,semantic_name));
 CREATE TABLE semantic_structures(name TEXT,source TEXT,start_line INTEGER,definition_sha256 TEXT,status TEXT,PRIMARY KEY(name,source,start_line));
 CREATE INDEX instructions_address ON instructions(image,address);
 CREATE INDEX data_target ON data_refs(image,target);
 CREATE INDEX edges_target ON edges(target);
 CREATE INDEX edges_source ON edges(source_image,source_function);
 CREATE VIEW variables AS SELECT * FROM symbols WHERE kind='data';
 ''')
 exported={};configs={};by_address={};metrics=[];graph=[];nodes=[];failures=[];gaps=[]
 for out in index_exports:
  image=read(out/'export-report.json')['image'];cfg=read(index_profile(R,project_config,image));configs[image]=cfg;src=cfg['loads'][-1];source_path=Path(src['path']);source_path=source_path if source_path.is_absolute() else R/source_path;raw=source_path.read_bytes();offset=src.get('offset',0);base=src['base']
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
    from source_index import ledger_source
    ledger_source(R,t)
    assert all((R/p).is_file() for p in t['evidence'])
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
 ctree_count=0
 for binding in project_config.get('analysis',{}).get('ctree_exports',[]):
  folder=(R/binding['path']).resolve();image=binding['image']
  if not folder.is_relative_to(R) or not folder.is_dir():raise ValueError('Invalid CTree export path')
  for sidecar in sorted((folder/'ctree').glob('*.json')):
   document=validate_ctree(read(sidecar));function=document['entry']
   if document['image']!=image:raise ValueError('CTree image binding mismatch')
   current=db.execute('SELECT sha256 FROM functions WHERE image=? AND address=?',(image,function)).fetchone()
   if not current or current[0]!=document['function_sha256']:raise ValueError('Stale CTree function identity')
   image_path=Path(document['image_identity']['path'])
   if hashlib.sha256(image_path.read_bytes()).hexdigest()!=document['image_identity']['sha256']:raise ValueError('Stale CTree image identity')
   relative=sidecar.relative_to(R).as_posix();sidecar_sha=hashlib.sha256(sidecar.read_bytes()).hexdigest()
   db.execute('INSERT INTO ctree_functions VALUES(?,?,?,?,?,?,?,?,?)',(image,function,relative,sidecar_sha,document['ida'],document['hexrays'],document['gp'],document['image_identity']['sha256'],json.dumps(document['prototype'],sort_keys=True)))
   db.executemany('INSERT INTO ctree_lvars VALUES(?,?,?,?,?,?,?)',[(image,function,row['id'],row['name'],json.dumps(row['type'],sort_keys=True),row['location'],int(row['is_argument'])) for row in document['lvars']])
   db.executemany('INSERT INTO ctree_nodes VALUES(?,?,?,?,?,?,?,?,?,?,?,?)',[(image,function,row['id'],row['kind'],row['op'],row['op_name'],row['ea'],row['parent'],json.dumps(row['children']),json.dumps(row.get('type'),sort_keys=True),json.dumps(row.get('variable'),sort_keys=True),row.get('number')) for row in document['nodes']])
   ctree_count+=1
 from analysis_images import resident_parents, resident_targets
 parents=resident_parents(project_config,configs)
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
    else:
     chosen=resident_targets(image,target,parents,exported,by_address)
     resolution='resident_parent' if chosen else 'unresolved_target'
    cur=db.execute('INSERT INTO edges(source_image,source_function,site,target,kind,resolution) VALUES(?,?,?,?,?,?)',(image,f['address'],a,target,kind,resolution));eid=cur.lastrowid
    db.executemany('INSERT INTO edge_candidates VALUES(?,?,?)',[(eid,c[0],c[1]) for c in chosen])
    graph.append(dict(source='%s:%08X'%(image,f['address']),site=a,target=target,kind=kind,resolution=resolution,candidates=['%s:%08X'%c for c in chosen]))
 from resolved_indirect_calls import populate as add_reviewed_indirect_calls
 indirect_summary=add_reviewed_indirect_calls(db,R,project_config,exported,graph)
 assert translated_seen==set(translations),'Translation refers to missing function'
 semantic_path=project_path('semantic_map','status/semantic-map.json')
 semantic={'functions':[],'structures':[]}
 if semantic_path.exists():
  semantic=read(semantic_path);assert semantic.get('schema')==1,'Unsupported semantic map schema'
  seen=set()
  for item in semantic.get('functions',[]):
   key=(item['image'],item['address'],item['kind'],item['semantic_name']);assert key not in seen,'Duplicate semantic alias';seen.add(key)
   assert item['binding'] in ['bound','pending_image_inventory','retired_native_copy']
   if item['binding']=='retired_native_copy':
    from source_index import ledger_source
    assert item.get('source') is None and item.get('native_retirement') is not None
    assert ledger_source(R,item)==(None,None)
   else:
    assert item.get('native_retirement') is None,'Retirement requires explicit retired binding'
   if item['binding'] in ['bound','retired_native_copy']:
    match=db.execute('SELECT sha256 FROM functions WHERE image=? AND address=?',(item['image'],item['address'])).fetchone()
    assert match and item.get('sha256')==match[0],'Stale bound semantic alias: '+str(key)
   if item.get('source'):assert (R/item['source']).is_file(),'Missing semantic source: '+item['source']
   db.execute('INSERT INTO semantic_aliases VALUES(?,?,?,?,?,?,?,?,?,?)',(item['image'],item['address'],item['kind'],item['original_name'],item['semantic_name'],item['binding'],item['status'],item.get('legacy_status'),item.get('source'),item.get('sha256')))
  for item in semantic.get('structures',[]):
   source=R/item['source'];assert source.is_file(),'Missing semantic structure source: '+item['source']
   db.execute('INSERT INTO semantic_structures VALUES(?,?,?,?,?)',(item['name'],item['source'],item['start_line'],item['definition_sha256'],item['status']))
 if project_config.get('analysis',{}).get('function_queue') in ('unique_exact_bodies','operand_templates'):
  from function_work_units import populate as populate_work_units
  similarity_summary=populate_work_units(db,R,project_config)
  if project_config['analysis']['function_queue']=='operand_templates':
   from operand_template_queue import populate as populate_templates
   populate_templates(db,R,project_config)
 else:
  from function_similarity import run as compare_mips_functions
  similarity_summary=compare_mips_functions(db)
 from source_index import populate as index_sources
 source_summary=index_sources(db,R,ledger)
 for k,v in {'schema_version':'5','source':'IDA 7.7 + Hex-Rays; instruction bytes independently verified against files','identity':'image + virtual address; names are not debug symbols','status_policy':'Historical strict-proof metadata; ordinary queue uses project.sqlite complete source bindings','source_index_parser':source_summary['parser_version'],'source_index_ledger_sha256':hashlib.sha256(ledger_bytes(R)).hexdigest(),'semantic_map_sha256':hashlib.sha256(semantic_path.read_bytes()).hexdigest() if semantic_path.exists() else 'absent','source_format_style_sha256':source_format['style_sha256'],'source_formatter_version':source_format['formatter'] or 'unavailable','ctree_functions':str(ctree_count),'created_utc':datetime.datetime.now(datetime.timezone.utc).isoformat()}.items():db.execute('INSERT INTO metadata VALUES(?,?)',(k,v))
 db.execute('INSERT INTO metadata VALUES(?,?)',('original_index_exports',json.dumps(dict(inputs=index_inputs,selection=project_config.get('analysis',{}).get('index_exports',[]),additional=project_config.get('analysis',{}).get('additional_index_exports',[])),sort_keys=True)))
 db.commit();assert db.execute('PRAGMA integrity_check').fetchone()[0]=='ok';assert not db.execute('PRAGMA foreign_key_check').fetchall()
 totals={k:sum(x[k] for x in metrics) for k in ['functions','pseudocode','failures','instruction_words','symbols','data_refs']};totals['edges']=len(graph);totals['indirect_transfers']=db.execute('SELECT count(*) FROM indirect_transfers').fetchone()[0];totals['unresolved_targets']=sum(e['resolution']=='unresolved_target' for e in graph);totals['overlay_context_edges']=sum(e['resolution']=='overlay_context_required' for e in graph)
 totals['semantic_aliases']=len(semantic.get('functions',[]));totals['semantic_structures']=len(semantic.get('structures',[]))
 totals['ctree_functions']=ctree_count
 totals['resident_parent_edges']=sum(e['resolution']=='resident_parent' for e in graph)
 totals['reviewed_indirect_calls']=indirect_summary
 db.close();tmp.replace(path)
 write(project_path('callgraph', 'status/callgraph.json'),dict(identity='image:address',nodes=nodes,edges=graph));write(R/'status/code-gaps.json',gaps);write(R/'status/decompilation-failures.json',failures)
 from project_context import queue
 progress=dict(phase='environment preparation',translation_queue=queue(R,project_config,limit=1)['counts'],metrics=metrics,totals=totals,limitations=['Function boundaries and code/data classification are discovery results, not fully audited.','Indirect transfers and overlay-context edges require future proof.','Hex-Rays failures are explicit and do not suppress MIPS listings.','Runtime checks are separate from complete translation bindings.'])
 progress['function_similarity']=similarity_summary
 progress['source_index']=source_summary
 if ledger:
  progress['phase']='native decompilation'
 progress['source_format']=source_format
 write(R/'status/progress-data.json',progress)
 print(json.dumps(totals))
if __name__=='__main__':
 import argparse
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--no-format',action='store_true',help='Rebuild evidence views without changing C/H source bytes')
 main(no_format=parser.parse_args().no_format)

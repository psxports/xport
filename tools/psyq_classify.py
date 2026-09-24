"""Validate Ghidra plugin matches against source bytes and map IDA identities."""
from pathlib import Path
import json,hashlib,re,collections
from xport_project import load_project, project_path, artifact_path
R,PROJECT=load_project()
def read(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def sha(b):return hashlib.sha256(b).hexdigest()
def main():
 import argparse
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--reports',type=Path,required=True)
 parser.add_argument('--output',type=Path,default=R/'status/ghidra/classification.json')
 args=parser.parse_args()
 args.output=artifact_path(args.output)
 exports=project_path('ida_exports','status/ida/images')
 wrappers=[(R/p).resolve() for p in PROJECT.get('analysis',{}).get('wrapper_references',[])]
 texts={str(p):p.read_text(encoding='utf-8-sig') for p in wrappers}
 assert all((args.reports/(image+'.json')).is_file() for image in PROJECT['analysis']['images']), 'Run recognition for both images first'
 accepted=[];pending=[]
 for file in sorted((args.reports).glob('*.json')):
  if file.stem not in PROJECT['analysis']['images']:continue
  report=read(file);image=report['image'];cfg=read(project_path('ida_configs','tools/ida')/(image+'.json'));load=cfg['loads'][-1];raw=(R/load['path']).read_bytes();payload=raw[load.get('offset',0):];base=load['base'];assert base==report['base'];assert cfg['gp']==report['gp']
  funcs=read(exports/image/'functions.json');byaddr=collections.defaultdict(list)
  for m in report['matches']:
   pattern=bytes.fromhex(m['pattern']);mask=bytes.fromhex(m['mask']);off=m['address']-base;actual=payload[off:off+len(pattern)]
   assert len(pattern)==len(mask) and off>=0 and len(actual)==len(pattern) and all((a&k)==(b&k) for a,b,k in zip(actual,pattern,mask))
   for label in m['labels']:
    if label['name'].startswith('loc_'):continue
    byaddr[m['address']+label['offset']].append(dict(name=label['name'],version=m['version'],library=m['library'],object=m['object'],object_start=m['address'],object_end=m['address']+len(pattern),object_sha256=sha(actual),entropy=m['entropy'],bios=m['bios']))
  for f in funcs:
   hits=[m for m in byaddr[f['address']] if all(m['object_start']<=a<b<=m['object_end'] for a,b in f['chunks'])]
   if not hits:continue
   names=sorted({m['name'] for m in hits if not m['name'].startswith('text_')})
   listing=(exports/image/'functions'/('%08X.lst'%f['address'])).relative_to(R).as_posix()
   pseudocode=(exports/image/f['pseudocode']).relative_to(R).as_posix() if f.get('pseudocode') else None
   entry=dict(image=image,address=f['address'],original_name=f['name'],sha256=f['sha256'],listing=listing,pseudocode=pseudocode,matches=hits,names=names)
   longest=max(m['object_end']-m['object_start'] for m in hits)
   is_bios=any(m['bios'] for m in hits)
   calls=[]
   for ins in f['raw_instructions']:
    word=int.from_bytes(bytes.fromhex(ins['bytes']),'little')
    if word>>26==3:calls.append(((ins['address']+4)&0xf0000000)|((word&0x3ffffff)<<2))
   if not is_bios and (longest<32 or longest<64 and any(not byaddr[t] for t in calls)):
    entry['reason']='Short generic signature without independently recognized call target; insufficient SDK identity evidence';pending.append(entry);continue
   if len(names)>1:
    entry['reason']='Conflicting API labels; needs review';pending.append(entry);continue
   name=names[0] if names else hits[0]['object'].replace('.','_')+'_'+format(f['address']-hits[0]['object_start'],'X')
   refs=[dict(path=p,sha256=sha(Path(p).read_bytes()),line=s[:m.start()].count('\n')+1) for p,s in texts.items() if (m:=re.search(r'\b'+re.escape(name)+r'\s*\(',s))]
   entry.update(alias=name,status='SKIP',replacement='Planned xport PsyQ/host boundary; Configured wrapper files are reference implementations only; host CRT for libc',wrapper_references=refs,wrapper_status='symbol present; project ABI/runtime integration remains untested' if refs else 'SDK internal or API adapter still to map at replacement boundary',reason='Whole IDA function lies inside a Ghidra PSX-plugin PsyQ masked object match; SDK implementation replaced at host wrapper boundary')
   accepted.append(entry)
 result=dict(policy='Original image/address names retained. SKIP is replacement scope, not DONE or runtime equivalence. Ambiguous API names remain TODO.',wrapper_files=[dict(path=p,sha256=sha(Path(p).read_bytes())) for p in texts],accepted=accepted,pending=pending)
 args.output.parent.mkdir(parents=True,exist_ok=True)
 args.output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf8')
 print('SKIP',len(accepted),'pending',len(pending),'wrapper symbol present',sum(bool(x['wrapper_references']) for x in accepted))
if __name__=='__main__':main()

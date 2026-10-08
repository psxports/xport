"""Plan or execute exact interrupted batch9 rollback through a reviewed hash-bound plan"""
import argparse,copy,hashlib,itertools,json,os,re,xml.etree.ElementTree as ET
from source_index import functions
from pathlib import Path
from lockstep_supervisor import source_snapshot

def sha(raw):return hashlib.sha256(raw).hexdigest()
def identity(files):return sha(json.dumps(files,sort_keys=True,separators=(',',':')).encode())
def owned(root,name):
 p=(root/name).resolve()
 if not p.is_relative_to(root.resolve()) or '..' in Path(name).parts:raise ValueError('Recovery path escapes owner')
 return p
def exact_replace(text,after,before):
 if not after or text.count(after)!=1:raise ValueError('Recovery inverse anchor not exact unique')
 return text.replace(after,before)
def byte_candidates(text):
 for newline in ('\n','\r\n'):
  for bom in (b'',b'\xef\xbb\xbf'):
   for tail in range(5):yield bom+(text.rstrip('\n')+'\n'*tail).replace('\n',newline).encode()
def recover_hashed_bytes(text,expected):
 found={raw for raw in byte_candidates(text) if sha(raw)==expected}
 if len(found)!=1:raise ValueError('Restored shared bytes do not match authoritative preimage hash')
 return found.pop()
def inverse_shared(text,proposal,path):
 for edit in reversed(proposal.get('sdk_header_edits',[]) if path.endswith('.h') else proposal.get('edits',[])):
  if edit['path']==path:text=exact_replace(text,edit['after'],edit['before'])
 if path.endswith('.c'):
  for row in reversed(proposal.get('sdk_translations',[])+proposal.get('system_translations',[])):
   if row['path']!=path:continue
   suffix='\n\n'+row['text'].strip()+'\n'
   if not text.endswith(suffix):raise ValueError('SDK append suffix is not exact or external body changed')
   text=text[:-len(suffix)]+'\n'
 return text

def forward_shared(text,proposal,path):
 if path.endswith('.c'):
  for row in proposal.get('sdk_translations',[])+proposal.get('system_translations',[]):
   if row['path']==path:text=text.rstrip()+'\n\n'+row['text'].strip()+'\n'
  for edit in proposal.get('edits',[]):
   if edit['path']==path:text=exact_replace(text,edit['before'],edit['after'])
 else:
  for edit in proposal.get('sdk_header_edits',[]):
   if edit['path']==path:text=exact_replace(text,edit['before'],edit['after'])
 return text

FORMATTED_AFTER_HASHES={
 'SetDrawMove':'abf9b6f8a9c344b438ea787e1cd04cd1ab6498a2e00f7fb6944bda8a8cd8c447',
 'ClearOTagPSX':'ae906e44657ecfe9c23446192266e1a9dbbeedfc21db08c110d86b760776b5e9',
 'RotMatrixYXZ':'2ddd7444a5c8464e0cd0fcc4fd236f5a6c40eafca43735fada3281e7dd9f0fbd'}
def reviewed_formatted_proposal(proposal,shared):
 result=copy.deepcopy(proposal);accepted=[]
 for field,key in (('sdk_translations','text'),('edits','after')):
  for row in result.get(field,[]):
   parsed=functions(row[key],row['path']);symbol=parsed[0]['name']
   if symbol not in FORMATTED_AFTER_HASHES:continue
   text=owned(shared,row['path'].removeprefix('@xport/')).read_bytes().decode('utf-8')
   if row[key] in text:continue
   actual=[f for f in functions(text,row['path']) if f['name']==symbol]
   if len(actual)!=1:raise ValueError('Formatted recovery function not unique')
   body=text[actual[0]['start']:actual[0]['end']]
   if sha(body.strip().encode())!=FORMATTED_AFTER_HASHES[symbol]:raise ValueError('Formatted current body differs from exact root-reviewed hash')
   tokens=r'"(?:\\.|[^"\\])*"|\w+|[^\s]'
   if re.findall(tokens,body)!=re.findall(tokens,row[key]):raise ValueError('Reviewed formatting changes C tokens')
   row[key]=body;accepted.append({'symbol':symbol,'current_after_sha256':sha(body.strip().encode())})
 return result,accepted

def relocated_driver(root,shared,proposal,registered_project):
 symbol='xport_gpu_clear_otag_r_guest_driver';rows=[r for r in proposal.get('system_translations',[]) if r['symbol']==symbol]
 if not rows:return proposal,None
 if len(rows)!=1 or rows[0]['path']!='@xport/src/psx_gpu.c':raise ValueError('Relocation must bind only exact original guest GPU bridge')
 row=rows[0];definitions=[]
 for path in (shared/'src').rglob('*.c'):
  text=path.read_bytes().decode('utf-8')
  for f in functions(text,'@xport/'+path.relative_to(shared).as_posix()):
   if f['name']==symbol:definitions.append((path,text,f))
 if len(definitions)!=1:raise ValueError('Relocated service requires exactly one current shared definition')
 path,text,function=definitions[0]
 if path==(shared/'src/psx_gpu.c'):return proposal,None
 if path!=(shared/'src/psx.c'):raise ValueError('Relocation destination outside exact approved psx.c')
 body=text[function['start']:function['end']]
 if body.strip()!=row['text'].strip():raise ValueError('Relocated bridge body differs from exact receipt-bound definition')
 project=owned(root,registered_project);tree=ET.fromstring(project.read_bytes());compiled={(project.parent/e.attrib['Include']).resolve() for e in tree.iter() if e.tag.split('}')[-1]=='ClCompile' and 'Include' in e.attrib}
 if path.resolve() not in compiled:raise ValueError('Relocated bridge destination is not compiled')
 result=copy.deepcopy(proposal);result['system_translations']=[r for r in result['system_translations'] if r['symbol']!=symbol]
 info={'symbol':symbol,'original_path':row['path'],'current_path':'@xport/src/psx.c','body_sha256':sha(body.encode()),'body':body,'current_character_offset':function['start'],'prefix_sha256':sha(text[:function['start']].encode()),'suffix_sha256':sha(text[function['end']:].encode())}
 return result,info

def prepare(root,packet_path,proposal_path,receipt_path,allow_scoped_inverse=False):
 root=root.resolve();config=json.loads((root/'xport-project.json').read_text(encoding='utf-8-sig'));shared=Path(config['toolset']).resolve().parent
 paths=[packet_path.resolve(),proposal_path.resolve(),receipt_path.resolve()]
 if any(not p.is_relative_to((root/'status').resolve()) for p in paths):raise ValueError('Recovery evidence must be project status owned')
 packet,proposal,receipt=[json.loads(p.read_bytes()) for p in paths]
 if not re.fullmatch(r'coverage-batch-0009-revision-\d{4}\.json',packet_path.name) or receipt_path!=packet_path.with_suffix('.audit.json'):raise ValueError('Recovery supports only interrupted batch9 receipt')
 if packet.get('progress')!='128/128/216' or any(d.get('failure_signature')!=packet['failure_signature'] for d in (proposal,receipt)):raise ValueError('Recovery packet/proposal/receipt identity mismatch')
 state_path=root/'status/function-coverage/first/coverage.json';state=json.loads(state_path.read_bytes())
 if state.get('progress')!={'proved':128,'decompiled':128,'total':216} or state.get('active_packet')!=packet_path.relative_to(root).as_posix() or (root/'status/function-coverage/first/coverage-proposal-batch9-accepted.json').exists():raise ValueError('Ledger is not unchanged interrupted batch9')
 if receipt.get('function_audits')!=proposal.get('function_audits') or receipt.get('sdk_audits')!=proposal.get('sdk_audits') or len(proposal.get('translations',[]))!=16:raise ValueError('Interrupted receipt differs from concrete full proposal')
 module=owned(root,packet['module_plan']['next_module'])
 expected=('#include "psx.h"\n\n'+proposal['declarations'].strip()+'\n\n'+'\n'.join(r['text'].strip()+'\n' for r in proposal['translations'])).encode()
 if module.name!='game_batch3.c' or module.read_bytes()!=expected:raise ValueError('Created module differs from receipt-bound full proposal')
 changes={module:None};current=source_snapshot(root,(Path('src'),))
 if current['identity']!=receipt['source_identity']:raise ValueError('Project source changed after interrupted transaction')
 alternatives=[]
 for update in proposal.get('game_prototype_updates',[]):
  path=owned(root,update['path']);text=path.read_text(encoding='utf-8-sig');text=exact_replace(text,update['after'],update['before']);alternatives.append((path,list(set(byte_candidates(text)))))
 restored=None
 for combo in itertools.product(*(a[1] for a in alternatives)):
  files=dict(current['files']);files.pop(module.relative_to(root).as_posix())
  for (path,_),raw in zip(alternatives,combo):files[path.relative_to(root).as_posix()]=sha(raw)
  if identity(files)==packet['source_identity']:
   candidate=dict(zip((a[0] for a in alternatives),combo))
   if restored is not None and candidate!=restored:raise ValueError('Ambiguous original project bytes')
   restored=candidate
 if restored is None:raise ValueError('All restored project bytes fail immutable packet source identity')
 changes.update(restored)
 before_shared=receipt['shared_source_before_files'];post_files=dict(before_shared);shared_modes=[];inverse_proposal,formatted_bodies=reviewed_formatted_proposal(proposal,shared);inverse_proposal,relocation=relocated_driver(root,shared,inverse_proposal,receipt['registered_project'])
 for relative in receipt['changed_shared_files']:
  path=owned(shared,relative);symbolic='@xport/'+relative;current_raw=path.read_bytes();stripped_current=current_raw.decode('utf-8')
  if relocation and symbolic==relocation['current_path']:
   body=relocation['body'];offset=relocation['current_character_offset']
   if stripped_current[offset:offset+len(body)]!=body or stripped_current.count(body)!=1:raise ValueError('Relocated body context drift')
   stripped_current=stripped_current[:offset]+stripped_current[offset+len(body):]
  text=inverse_shared(stripped_current,inverse_proposal,symbolic);replayed=forward_shared(text,inverse_proposal,symbolic)
  if relocation and symbolic==relocation['current_path']:
   offset=relocation['current_character_offset'];replayed=replayed[:offset]+relocation['body']+replayed[offset:]
  if replayed.encode()!=current_raw:raise ValueError('Scoped inverse forward replay differs from exact current bytes')
  try:raw=recover_hashed_bytes(text,before_shared[relative]);mode='exact_historical_preimage'
  except ValueError:
   if not allow_scoped_inverse:raise
   raw=text.encode();mode='scope_exact_inverse_preserving_external_bytes_historical_preimage_unavailable'
  changes[path]=raw;post_files[relative]=sha(current_raw);shared_modes.append({'path':symbolic,'mode':mode,'historical_before_sha256':before_shared[relative],'restored_current_baseline_sha256':sha(raw),'current_sha256':sha(current_raw),'forward_replay_current_exact':True})
 post_matches=identity(post_files)==receipt['shared_source_identity']
 if not post_matches and not allow_scoped_inverse:raise ValueError('Touched shared resulting hashes differ from receipt')
 project=owned(root,receipt['registered_project']);raw=project.read_bytes();relative=os.path.relpath(module,project.parent)
 fragment=('<ItemGroup><ClCompile Include="'+relative+'" /></ItemGroup>\n').encode()
 if raw.count(fragment)!=1:raise ValueError('Unique exact supervisor XML registration missing')
 restored_xml=raw.replace(fragment,b'',1);ns={'m':'http://schemas.microsoft.com/developer/msbuild/2003'}
 def nodes(data):return [n.attrib for n in ET.fromstring(data).findall('.//m:ClCompile',ns)]
 oldnodes=nodes(raw);newnodes=nodes(restored_xml);target=[n for n in oldnodes if n.get('Include')==relative]
 if len(target)!=1 or [n for n in oldnodes if n.get('Include')!=relative]!=newnodes:raise ValueError('XML recovery changes unrelated compile registrations')
 changes[project]=restored_xml
 archive=receipt_path.with_name(receipt_path.stem+'.interrupted-archive.json')
 if archive.exists():raise ValueError('Interrupted archive already exists')
 plan={'schema':1,'contract':'xport-interrupted-batch9-recovery-v1','progress':'128/128/216','evidence':[{'path':p.relative_to(root).as_posix(),'sha256':sha(p.read_bytes())} for p in paths],'ledger_sha256':sha(state_path.read_bytes()),'archive_receipt':archive.relative_to(root).as_posix(),'project_before_identity':packet['source_identity'],'shared_before_identity':receipt['shared_source_before_identity'],'registration_provenance_limitation':'Packet and receipt do not retain XML preimage hash; only unique exact generated registration is removed, preserving every other current byte and compilation node','operations':[{'path':str(p),'current_sha256':sha(p.read_bytes()),'restored_sha256':sha(raw) if raw is not None else None,'delete_created_module':raw is None} for p,raw in changes.items()],'execution_performed':False,'allow_scoped_inverse':allow_scoped_inverse,'shared_inverse_modes':shared_modes,'historical_touched_postimage_matches_receipt':post_matches,'exact_root_reviewed_formatted_bodies':formatted_bodies,'exact_relocated_service':relocation}
 return plan,changes,archive

def execute(root,packet_path,proposal_path,receipt_path,reviewed_plan):
 plan,changes,archive=prepare(root,packet_path,proposal_path,receipt_path,reviewed_plan.get('allow_scoped_inverse',False))
 if plan!=reviewed_plan:raise ValueError('Recovery plan drifted after root review')
 originals={p:p.read_bytes() for p in changes};receipt_raw=receipt_path.read_bytes();ledger=root/'status/function-coverage/first/coverage.json'
 if sha(ledger.read_bytes())!=plan['ledger_sha256']:raise ValueError('Ledger drift immediately before recovery writes')
 for row in plan['operations']:
  if sha(originals[Path(row['path'])])!=row['current_sha256'] or sha(Path(row['path']).read_bytes())!=row['current_sha256']:raise ValueError('Operation drift immediately before recovery writes')
 for row in plan['evidence']:
  if sha((root/row['path']).read_bytes())!=row['sha256']:raise ValueError('Evidence drift immediately before recovery writes')
 if archive.exists():raise ValueError('Archive appeared immediately before recovery writes')
 archive_created=False
 try:
  for p,raw in changes.items():
   if raw is None:p.unlink()
   else:p.write_bytes(raw)
  with archive.open('xb') as stream:
   archive_created=True;stream.write(receipt_raw)
  receipt_path.unlink()
  for p,raw in changes.items():
   if raw is None:
    if p.exists():raise ValueError('Created module recovery deletion failed')
   elif sha(p.read_bytes())!=sha(raw):raise ValueError('Restored file post-write hash mismatch')
  if source_snapshot(root,(Path('src'),))['identity']!=plan['project_before_identity']:raise ValueError('Restored project identity mismatch')
  if archive.read_bytes()!=receipt_raw or sha(ledger.read_bytes())!=plan['ledger_sha256']:raise ValueError('Archive or unchanged ledger verification failed')
 except BaseException as failure:
  errors=[]
  for p,raw in originals.items():
   try:p.write_bytes(raw)
   except BaseException as error:errors.append(error)
  try:receipt_path.write_bytes(receipt_raw)
  except BaseException as error:errors.append(error)
  try:
   if archive_created:archive.unlink(missing_ok=True)
  except BaseException as error:errors.append(error)
  if errors:raise RuntimeError('Recovery rollback attempted every file; failures: '+ '; '.join(type(e).__name__+': '+str(e) for e in errors)) from failure
  raise
 return {'status':'restored','progress':'128/128/216','archive':str(archive)}

def main():
 parser=argparse.ArgumentParser();parser.add_argument('--project',type=Path,required=True);parser.add_argument('--packet',type=Path,required=True);parser.add_argument('--proposal',type=Path,required=True);parser.add_argument('--receipt',type=Path,required=True);parser.add_argument('--plan',type=Path,required=True);parser.add_argument('--execute-reviewed-plan-sha256');parser.add_argument('--allow-scoped-inverse',action='store_true');a=parser.parse_args()
 if a.execute_reviewed_plan_sha256:
  raw=a.plan.read_bytes()
  if sha(raw)!=a.execute_reviewed_plan_sha256:raise ValueError('Reviewed recovery plan hash mismatch')
  print(json.dumps(execute(a.project,a.packet,a.proposal,a.receipt,json.loads(raw))))
 else:
  plan,_,_=prepare(a.project,a.packet,a.proposal,a.receipt,a.allow_scoped_inverse);a.plan.write_text(json.dumps(plan,indent=2)+'\n');print(json.dumps({'status':'planned','operations':len(plan['operations']),'plan_sha256':sha(a.plan.read_bytes()),'native_changes':False}))
if __name__=='__main__':main()

"""Apply exactly three reviewed V8 declaration fixes without changing function bodies"""
import hashlib,json,os,sys
from pathlib import Path
import coverage as supervisor
from source_index import functions
from xport_project import load_project,allocation_lock

def sha(b):return hashlib.sha256(b).hexdigest()
def main():
 root,config=load_project(Path(sys.argv[1]));os.environ['XPORT_PROJECT']=str(root)
 status=root/'status/function-coverage/first'
 proposal=status/'proposal-batch10-build-canonical-declarations-status.json'
 if sha(proposal.read_bytes())!='a76a65f4f7cbc9e963db1fa8042cf4a0bec7253e714f79aa6f541535410bccb7':raise ValueError('Exact reviewed proposal changed')
 data=json.loads(proposal.read_text(encoding='utf-8'));shared=Path(config['toolset']).resolve().parent
 with allocation_lock(root),allocation_lock(shared):
  if sha((shared/'src/psx.h').read_bytes())!=data['shared_header_sha256'] or sha((shared/'src/psx.c').read_bytes())!=data['shared_implementation_sha256']:raise ValueError('Canonical ABI source changed')
  baseline=supervisor.source_snapshot(root,supervisor.SOURCE_ROOTS);shared_before=supervisor.source_snapshot(shared,supervisor.SOURCE_ROOTS)
  old={k:(root/k).read_bytes() for k in data['source_before_sha256']};new=dict(old)
  for k,v in old.items():
   if sha(v)!=data['source_before_sha256'][k]:raise ValueError('Project source changed')
  for edit in data['edits']:
   key=edit['path'];text=new[key].decode('utf-8');newline='\r\n' if b'\r\n' in new[key] else '\n';before=edit['before'].replace('\n',newline);after=edit['after'].replace('\n',newline)
   if text.count(before)!=edit['expected_count']:raise ValueError('Declaration anchor changed')
   new[key]=text.replace(before,after,1).encode('utf-8')
  for key in old:
   def bodies(raw):
    text=raw.decode('utf-8');return [(f['name'],text[f['start']:f['end']],text[:f['start']].count('\n')) for f in functions(text,key)]
   if bodies(old[key])!=bodies(new[key]):raise ValueError('Function body or line moved')
  protected={status/'coverage.json',root/'status/translation-ledger.json',root/'xport-project.json'};invariants={x:x.read_bytes() for x in protected};index=root/config['paths']['analysis_database'];index_old=index.read_bytes();receipt=status/'review-batch10-build-declarations-applied-root.json'
  if receipt.exists():raise ValueError('Receipt exists')
  def atomic(path,raw):
   temporary=path.with_name(path.name+'.compat.tmp');temporary.write_bytes(raw);os.replace(temporary,path)
  try:
   for key,raw in new.items():atomic(root/key,raw)
   result=supervisor.source_refresh(root)
   if any(x.read_bytes()!=v for x,v in invariants.items()) or supervisor.source_snapshot(shared,supervisor.SOURCE_ROOTS)!=shared_before:raise ValueError('Protected state changed')
   expected=dict(baseline['files']);expected.update({k:sha(v) for k,v in new.items()})
   if supervisor.source_snapshot(root,supervisor.SOURCE_ROOTS)['files']!=expected:raise ValueError('Unexpected source mutation')
   output={'schema':1,'contract':'xport-project-canonical-declaration-compatibility-review-v1','result':'APPLIED','progress':'144/144/216','proposal_sha256':sha(proposal.read_bytes()),'function_bodies_and_start_lines_unchanged':True,'source_before':{k:sha(v) for k,v in old.items()},'source_after':{k:sha(v) for k,v in new.items()},'source_index_refresh':result,'proof_counter_changed':False,'packet_reissue_required':True}
   atomic(receipt,(json.dumps(output,indent=2)+'\n').encode());print(json.dumps(output))
  except BaseException:
   for key,raw in old.items():
    if (root/key).read_bytes() not in (raw,new[key]):raise RuntimeError('External drift prevents safe rollback')
    atomic(root/key,raw)
   atomic(index,index_old)
   if receipt.exists():receipt.unlink()
   raise
if __name__=='__main__':main()

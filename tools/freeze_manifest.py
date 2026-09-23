"""Freeze generated evidence and project scripts after validation; verify on demand."""
from pathlib import Path
import json,hashlib,argparse,datetime
from xport_project import project_root, artifact_path
R=project_root();M=artifact_path('status/evidence-manifest.json')
def sha(p):
 h=hashlib.sha256()
 with p.open('rb') as f:
  for b in iter(lambda:f.read(1048576),b''):h.update(b)
 return h.hexdigest()
def main():
 a=argparse.ArgumentParser();a.add_argument('--verify',action='store_true');a.add_argument('--folder',action='append',help='Project-local directory to inventory; repeat as needed');args=a.parse_args()
 if args.verify:
  d=json.loads(M.read_text());bad=[x['path'] for x in d['files'] if not (R/x['path']).is_file() or sha(R/x['path'])!=x['sha256']]
  assert not bad,bad
  print('PASS frozen evidence:',len(d['files']));return
 files=[]
 for folder in args.folder or ['orig','status','tools']:
  base=(R/folder).resolve()
  if not base.is_relative_to(R):raise ValueError('Inventory must remain inside project')
  for p in base.rglob('*'):
   if not p.is_file() or p==M or '__pycache__' in p.parts:continue
   if folder=='tools' and ('runtime' in p.parts or 'gdb' in p.parts):continue
   if p.suffix in ['.pyc','.log','.id0','.id1','.nam','.til']:continue
   files.append(dict(path=p.relative_to(R).as_posix(),sha256=sha(p),size=p.stat().st_size))
 d=dict(created_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),scope='Generated evidence and project scripts; runtime binaries/BIOS pinned separately in environment.json; original inputs in input-manifest.json',files=sorted(files,key=lambda x:x['path']))
 M.write_text(json.dumps(d,indent=2)+'\n');print('Frozen evidence:',len(files))
if __name__=='__main__':main()

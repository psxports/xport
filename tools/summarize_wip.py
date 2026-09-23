"""Summarize manual discovery journals without dropping their repeated occurrences."""
from pathlib import Path
import argparse,collections,json
p=argparse.ArgumentParser();p.add_argument('journal',type=Path);p.add_argument('--output',type=Path);a=p.parse_args()
from xport_project import artifact_path
if a.output:a.output=artifact_path(a.output)
groups={};sessions=0
for number,line in enumerate(a.journal.read_text(encoding='utf-8').splitlines(),1):
 if not line.strip():continue
 try:row=json.loads(line)
 except ValueError as e:raise SystemExit(f'Invalid JSON at line {number}: {e}')
 if row.get('event')=='session':sessions+=1;continue
 if row.get('event')!='wip':continue
 key=(row['pc'],row['file'],row['line'],row['function'])
 g=groups.setdefault(key,dict(pc=key[0],file=key[1],line=key[2],function=key[3],hits=0,continued=0,fatal=0,first_log_line=number,last_log_line=number,observed_before_any_skip=False))
 g['hits']+=1;g['continued']+=int(row['continued']);g['fatal']+=int(not row['continued']);g['last_log_line']=number
 g['observed_before_any_skip']|=row['prior_skips']==0
r=dict(journal=str(a.journal),sessions=sessions,unique_sites=len(groups),total_hits=sum(g['hits'] for g in groups.values()),branches=sorted(groups.values(),key=lambda g:(-g['hits'],g['pc'])),scope='Frequency is for these manual runs. Hits after skipped logic may be downstream artifacts; reproduce them in strict mode before decompilation acceptance.')
text=json.dumps(r,ensure_ascii=False,indent=2)+'\n'
if a.output:a.output.write_text(text,encoding='utf-8')
else:print(text,end='')

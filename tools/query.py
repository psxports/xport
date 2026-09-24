"""Small read-only function/call/data explorer for analysis.sqlite."""
import argparse,sqlite3,json
from pathlib import Path
from xport_project import project_root, project_path, load_project
R=project_root()
def parse_address(value):
    return int(value, 0) if value.lower().startswith('0x') else int(value, 16)

p=argparse.ArgumentParser();p.add_argument('address',type=parse_address);p.add_argument('--image',default=load_project()[1].get('analysis', {}).get('default_image'));p.add_argument('--full',action='store_true');p.add_argument('--limit',type=int,default=8)
p.add_argument('--audit-context',action='store_true');p.add_argument('--budget',type=int,default=12000);p.add_argument('--full-source',action='store_true');p.add_argument('--mips-address',type=parse_address);p.add_argument('--source-line',type=int);p.add_argument('--pseudo-line',type=int);a=p.parse_args()
if a.audit_context:
 from source_context import compact
 if not a.image:p.error('--image is required unless analysis.default_image is configured')
 print(json.dumps(compact(a.image,a.address,a.budget,a.full_source,a.mips_address,a.source_line,a.pseudo_line),separators=(',',':')));raise SystemExit
c=sqlite3.connect(project_path('analysis_database', 'status/analysis.sqlite').as_uri()+'?mode=ro',uri=True);c.row_factory=sqlite3.Row
if not a.image:p.error('--image is required unless analysis.default_image is configured')
f=c.execute('SELECT * FROM functions WHERE image=? AND address=?',(a.image,a.address)).fetchone()
if not f:raise SystemExit('Function entry not found; specify the image and exact hexadecimal entry address.')
def rows(sql,args):return [dict(x) for x in c.execute(sql,args)]
out=dict(function=dict(f),callees=rows('SELECT * FROM edges WHERE source_image=? AND source_function=?',(a.image,a.address)),callers=rows('SELECT e.* FROM edges e JOIN edge_candidates x ON e.id=x.edge WHERE x.target_image=? AND x.target_function=?',(a.image,a.address)),data_refs=rows('SELECT * FROM data_refs WHERE image=? AND function=?',(a.image,a.address)),indirect=rows('SELECT * FROM indirect_transfers WHERE image=? AND function=?',(a.image,a.address)))
out['library_classification']=rows('SELECT alias,reason,evidence FROM library_classification WHERE image=? AND address=?',(a.image,a.address))
out['semantic_aliases']=rows('SELECT * FROM semantic_aliases WHERE image=? AND address=? ORDER BY kind,semantic_name',(a.image,a.address))
out['reuse_summary']=rows('SELECT * FROM function_similarity WHERE image=? AND address=?',(a.image,a.address))
out['reuse_peers']=rows('SELECT r.*,f.name AS peer_name,f.bytes AS peer_bytes,f.status AS peer_status FROM function_reuse r JOIN functions f ON f.image=r.peer_image AND f.address=r.peer_address WHERE r.image=? AND r.address=? ORDER BY r.kind,r.peer_image,r.peer_address',(a.image,a.address))
if not a.full:
    for key,value in list(out.items()):
        if isinstance(value,list):
            out[key]={'count':len(value),'rows':value[:max(0,min(a.limit,32))],'truncated':len(value)>max(0,min(a.limit,32))}
print(json.dumps(out,separators=(',',':')))

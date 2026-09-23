"""Bounded SQL and connected MIPS context for one proven code site"""
import argparse
import json
from pathlib import Path
import re
import sqlite3
from trace_cache import digest
from xport_project import load_project, artifact_path


def candidates(address,image=None):
    root,project=load_project();image=image or project.get('analysis',{}).get('default_image')
    database=root/project['paths']['analysis_database']
    with sqlite3.connect(database.as_uri()+'?mode=ro',uri=True) as db:
        db.row_factory=sqlite3.Row
        rows=[dict(r) for r in db.execute('SELECT d.image,d.function,d.site,d.target,d.type,f.name,f.listing,f.pseudocode FROM data_refs d LEFT JOIN functions f ON f.image=d.image AND f.address=d.function WHERE d.image=? AND d.target BETWEEN ? AND ? ORDER BY ABS(d.target-?),d.site LIMIT 12',(image,max(0,address-3),address,address))]
    return dict(address=hex(address),image=image,candidates=rows,scope='Nearby static data references only; not proven writers; indirect structure access may have no static reference')


@__import__('pipeline_metrics').measured('audit_packet')
def audit(site, image=None, state=None, radius=32, pseudo_line=None):
    root, project = load_project()
    image = image or project.get('analysis', {}).get('default_image')
    database = root/project['paths']['analysis_database']
    with sqlite3.connect(database.as_uri()+'?mode=ro', uri=True) as db:
        db.row_factory = sqlite3.Row
        found = db.execute('SELECT * FROM functions WHERE image=? AND address<=? AND end>?',
                           (image, site, site)).fetchall()
        if len(found) != 1: return dict(site=hex(site), image=image, ambiguous=True)
        function = dict(found[0]); address = function['address']
        peers = [dict(r) for r in db.execute('SELECT * FROM function_reuse WHERE image=? AND address=? LIMIT 6', (image,address))]
        edges = [dict(r) for r in db.execute('SELECT * FROM edges WHERE source_image=? AND source_function=? AND site BETWEEN ? AND ? LIMIT 20',
                                            (image,address,max(address,site-128),site+256))]
        callers=[dict(r) for r in db.execute('SELECT source_image,source_function,site,kind FROM edges WHERE target=? LIMIT 8',(address,))]
    base = root/project['paths'].get('ida_exports','orig/images')/image
    mips = base/'functions'/f'{address:08X}.mips.txt'
    if not mips.is_file(): mips = base/'functions'/f'{address:08X}.lst'
    lines = mips.read_text(errors='replace').splitlines() if mips.is_file() else []
    positions = [(i,int(m.group(1),16)) for i,line in enumerate(lines)
                 if (m:=re.match(r'^\s*([0-9A-Fa-f]{8})\b',line))]
    pivot = next((i for i,a in positions if a>=site),0)
    radius=max(8,min(128,radius))
    begin,end = max(0,pivot-12), min(len(lines),pivot+radius+2)
    targets = sorted(set(int(v,16) for line in lines[begin:end]
                         for v in re.findall(r'(?:loc_|0x)(800[0-9A-Fa-f]{5})',line)))
    extras=[]
    for target in targets[:8]:
        at=next((i for i,a in positions if a==target),None)
        if at is not None and not begin<=at<end:
            extras.append(dict(target=hex(target),lines=lines[at:at+6]))
    pseudo=base/'pseudocode'/f'{address:08X}.c'
    pseudolines=pseudo.read_text(errors='replace').splitlines() if pseudo.exists() else []
    pseudo_at=max(0,(pseudo_line or 1)-1)
    if state is not None and pseudo_line is None:
        pseudo_at=next((max(0,i-5) for i,line in enumerate(pseudolines) if re.search(r'\bcase\s+'+str(state)+r'\s*:|==\s*'+str(state)+r'\b',line)),0)
    constants=[line for line in lines[begin:end] if re.search(r'\b(li|addiu|ori|xori|sltiu?)\b',line)][:24]
    from source_context import compact
    try:
        source_context=compact(image,address,5000,False,site,None,pseudo_line)
        c_context=dict(implementation=source_context['implementation'],declarations=source_context['declarations'],gaps=source_context['gaps'])
    except ValueError as error:
        c_context=dict(problem=str(error),implementation=None,declarations=[],gaps=['Source context unavailable'])
    result=dict(site=hex(site),image=image,function=function,peers=peers,edges=edges,callers=callers,
        mips=dict(path=str(mips),sha256=digest(mips) if mips.exists() else None,
                  first_line=begin+1,lines=lines[begin:end],branch_target_context=extras),
        pseudocode=dict(path=str(pseudo),sha256=digest(pseudo) if pseudo.exists() else None,
                        first_line=pseudo_at+1,lines=pseudolines[pseudo_at:pseudo_at+32]),
        implementation=c_context,
        constant_mapping_review=constants,
        checklist=['Trace branch and delay-slot writes','Check all adjacent dispatch cases and default',
                   'Verify each constant mapping; do not extrapolate from one case',
                   'Check loads, signedness, aliases and caller stack arguments',
                   'Review duplicate differences; no automatic DONE'],
        truncated=begin>0 or end<len(lines))
    # Keep links and hashes when the context budget requires shorter excerpts
    for collection in (result['implementation']['declarations'],result['pseudocode']['lines'],
                       result['mips']['branch_target_context'],result['constant_mapping_review'],
                       result['mips']['lines'],result['edges'],result['peers'],result['callers']):
        while collection and len(json.dumps(result,indent=2).encode())>12000:
            collection.pop();result['truncated']=True
    return result


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('site',nargs='?',type=lambda s:int(s,0))
    p.add_argument('--address',type=lambda s:int(s,0))
    p.add_argument('--image');p.add_argument('--output',type=Path,required=True)
    p.add_argument('--state',type=int);p.add_argument('--radius',type=int,default=32);p.add_argument('--pseudo-line',type=int)
    a=p.parse_args()
    if (a.site is None)==(a.address is None):p.error('Specify either a code site or --address')
    value=candidates(a.address,a.image) if a.address is not None else audit(a.site,a.image,a.state,a.radius,a.pseudo_line)
    path=artifact_path(a.output)
    path.parent.mkdir(parents=True,exist_ok=True);path.write_text(json.dumps(value,indent=2)+'\n')
    print(json.dumps(dict(audit=str(path),site=hex(a.site) if a.site is not None else None,address=value.get('address'),function=value.get('function',{}).get('name'))))


if __name__=='__main__':main()

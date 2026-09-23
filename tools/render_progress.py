"""Render project progress directly from image-aware SQLite analysis"""
import datetime
import html
import json
from pathlib import Path
import sqlite3
from xport_project import load_project, project_path, artifact_path


def render():
    root, config = load_project()
    database = project_path('analysis_database', 'status/analysis.sqlite')
    with sqlite3.connect(database.as_uri()+'?mode=ro', uri=True) as db:
        db.row_factory = sqlite3.Row
        tables = {r[0] for r in db.execute("SELECT name FROM sqlite_master WHERE type IN ('table','view')")}
        required = {'functions', 'symbols', 'edges', 'edge_candidates', 'indirect_transfers'}
        if not required <= tables:
            raise ValueError('Missing analysis schema: '+', '.join(sorted(required-tables)))
        peers = {}
        if 'function_reuse' in tables:
            for row in db.execute('SELECT * FROM function_reuse ORDER BY image,address,peer_image,peer_address'):
                peers.setdefault((row['image'], row['address']), []).append(dict(row))
        sdk = {(r['image'], r['address']): dict(r) for r in db.execute('SELECT * FROM library_classification')} if 'library_classification' in tables else {}
        functions = []
        counts = dict.fromkeys(('TODO','WIP','DONE','SKIP'), 0)
        for row in db.execute('SELECT * FROM functions ORDER BY bytes,image,address'):
            key = (row['image'], row['address'])
            if row['status'] not in counts:
                raise ValueError('Unknown implementation status: '+str(row['status']))
            counts[row['status']] += 1
            classification = sdk.get(key)
            note = 'Implementation status from SQL; coverage is not correctness'
            if classification:
                note += '; '+classification['reason']
            if peers.get(key):
                note += '; MIPS reuse candidates: audit constants, calls and layouts before reuse'
            functions.append({'id': '%s:%08X'%key, 'module': 'PSYQ/BIOS' if classification else row['image'],
                              'image':row['image'], 'address':'0x%08X'%row['address'],
                              'name':row['name'], 'status':row['status'], 'comments':note,
                              'listing':row['listing'], 'pseudocode':row['pseudocode'],
                              'size':row['bytes'], 'reuse':peers.get(key, [])})
        data = [dict(module=r['image'], address='0x%08X'%r['address'], name=r['name'],
                     description=r['confidence']) for r in db.execute("SELECT * FROM symbols WHERE kind='data' ORDER BY image,address")]
        edges = [dict(source='%s:%08X'%(r['source_image'],r['source_function']),
                      target='%s:%08X'%(r['target_image'],r['target_function']),kind=r['kind'])
                 for r in db.execute("SELECT e.*,c.target_image,c.target_function FROM edges e JOIN edge_candidates c ON e.id=c.edge WHERE e.resolution IN ('same_image','resident')")]
        indirect = db.execute('SELECT count(*) FROM indirect_transfers').fetchone()[0]
    payload = dict(functions=functions, data=data, graph=dict(nodes=functions,edges=edges,
                   meta=dict(asmFunctions=len(functions),directEdges=len(edges),unresolvedIndirectCalls=indirect)))
    template = Path(__file__).with_name('status-template.html').read_text(encoding='utf-8')
    denominator = sum(counts[k] for k in ('TODO','WIP','DONE'))
    for status,count in counts.items():
        percent = 'excluded from %' if status=='SKIP' else f'{100*count/denominator if denominator else 0:.1f}%'
        template = template.replace('__'+status+'_COUNT__',str(count)).replace('__'+status+'_PERCENT__',percent)
    template = template.replace('__PROJECT__',html.escape(config['name']))
    template = template.replace('__CHART_PERCENT__',f"{100*counts['DONE']/denominator if denominator else 0:.1f}")
    template = template.replace('__MODULES__',''.join('<option>'+html.escape(m)+'</option>' for m in sorted({r['module'] for r in functions+data})))
    template = template.replace('__SUB__','Generated '+datetime.datetime.now(datetime.timezone.utc).isoformat()+' from SQL. Functions use image + address identity. Reuse candidates are not semantic equivalence. Implementation status does not certify all runtime contexts.')
    template = template.replace('__DATA__',json.dumps(payload,ensure_ascii=False).replace('</','<\\/'))
    output = artifact_path('status/progress.html')
    output.parent.mkdir(parents=True,exist_ok=True)
    output.write_text(template,encoding='utf-8')
    return {'output':str(output),'functions':len(functions),'data':len(data),'edges':len(edges),'counts':counts}


if __name__ == '__main__':
    print(json.dumps(render()))

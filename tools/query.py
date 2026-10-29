"""Read concise current context and paged function relationships from SQLite"""
import argparse
import base64
from contextlib import closing
import hashlib
import json
import sqlite3

from project_context import analysis_path, current_library_classification, function_summary, show
from xport_project import load_project, project_path


def parse_address(value):
    return int(value, 16)


def explore(root, config, image, address, view='current', limit=8, cursor=None, fields=None):
    if not image or not 1 <= limit <= 32:
        raise ValueError('Explicit image and limit 1..32 are required')
    path = analysis_path(root, config)
    generation = hashlib.sha256((str(path.stat().st_mtime_ns) + ':' + str(path.stat().st_size)).encode()).hexdigest()
    start = 0
    if cursor:
        saved = json.loads(base64.urlsafe_b64decode(cursor))
        if saved[:5] != [generation, image, address, view, fields]:
            raise ValueError('Query changed or analysis was refreshed; restart pagination')
        start = saved[5]
    with closing(sqlite3.connect(path.as_uri()+'?mode=ro', uri=True)) as db:
        db.row_factory = sqlite3.Row
        function = db.execute('SELECT * FROM functions WHERE image=? AND address=?', (image,address)).fetchone()
        if not function:
            raise ValueError('Function entry not found for image/address')
        function = dict(function)
        function.pop('status', None)
        result = {'function': {key:function[key] for key in ('image','address','name','sha256')}}
        if config.get('analysis',{}).get('evidence_backend')=='ghidra':
            result['analysis_limitations'] = 'Boundaries are unreviewed; data references, cross-image call resolution and similarity are incomplete'
        if view in ('current', 'sdk', 'all'):
            result.update(function=function, translation=function_summary(root,config,image,address),
                          project_context=show(root,config,image=image,address=address,limit=3))
        sql = {
            'callees': 'SELECT * FROM edges WHERE source_image=? AND source_function=? ORDER BY site,target,kind,resolution',
            'callers': 'SELECT e.* FROM edges e JOIN edge_candidates x ON e.id=x.edge WHERE x.target_image=? AND x.target_function=? ORDER BY e.source_image,e.source_function,e.site,e.id',
            'data_refs': 'SELECT * FROM data_refs WHERE image=? AND function=? ORDER BY site,target,type',
            'indirect': 'SELECT * FROM indirect_transfers WHERE image=? AND function=? ORDER BY site,instruction,status',
            'reuse_peers': 'SELECT * FROM function_reuse WHERE image=? AND address=? ORDER BY peer_image,peer_address,kind',
        }
        if view in ('current', 'sdk', 'all'):
            classification = current_library_classification(root, config, db).get((image, address))
            rows = [classification] if classification else []
            result['library_classification'] = []
            for row in rows:
                evidence = row['evidence'] if isinstance(row['evidence'], dict) else json.loads(row['evidence'])
                result['library_classification'].append({'alias':row['alias'],'reason':row['reason'],
                    'wrapper_status':evidence.get('wrapper_status'),
                    'boundary':'Inspect the existing shared API and actual caller ABI before reuse'})
        views = list(sql) if view == 'all' else [view] if view in sql else []
        for name in views:
            statement = sql[name]
            count = db.execute('SELECT count(*) FROM ('+statement+')', (image,address)).fetchone()[0]
            rows = [dict(row) for row in db.execute(statement+' LIMIT ? OFFSET ?', (image,address,limit,start))]
            if fields:
                columns = set(rows[0]) if rows else {r[0] for r in db.execute(statement+' LIMIT 0', (image,address)).description}
                if not set(fields) <= columns:
                    raise ValueError('Unknown fields for '+name)
                rows = [{key:row[key] for key in fields} for row in rows]
            next_cursor = base64.urlsafe_b64encode(json.dumps([generation,image,address,view,fields,start+len(rows)]).encode()).decode() if start+len(rows)<count else None
            result[name] = {'count':count,'rows':rows,'next':next_cursor}
        return result


def main():
    root, config = load_project()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('address', type=parse_address)
    parser.add_argument('--image', default=config.get('analysis',{}).get('default_image'))
    parser.add_argument('--view', choices=('current','sdk','callers','callees','data_refs','indirect','reuse_peers','all'), default='current')
    parser.add_argument('--limit',type=int,default=8)
    parser.add_argument('--cursor')
    parser.add_argument('--fields', help='Comma-separated relationship columns')
    parser.add_argument('--full',action='store_true',help='Legacy spelling for a bounded all-sections view')
    parser.add_argument('--audit-context',action='store_true')
    parser.add_argument('--budget',type=int,default=12000)
    parser.add_argument('--full-source',action='store_true')
    parser.add_argument('--mips-address',type=parse_address)
    parser.add_argument('--source-line',type=int)
    parser.add_argument('--pseudo-line',type=int)
    args = parser.parse_args()
    if not args.image:
        parser.error('--image is required')
    if args.audit_context:
        from source_context import compact
        result = compact(args.image,args.address,args.budget,args.full_source,args.mips_address,args.source_line,args.pseudo_line)
    else:
        result = explore(root,config,args.image,args.address,'all' if args.full else args.view,
                         args.limit,args.cursor,args.fields.split(',') if args.fields else None)
    print(json.dumps(result,ensure_ascii=False,separators=(',',':')))


if __name__ == '__main__':
    main()

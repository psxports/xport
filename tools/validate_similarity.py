"""Audit every eligible pair, SQL recommendation and saved MIPS evidence."""
from pathlib import Path
import itertools
import json
import sqlite3
from xport_project import project_path
from function_similarity import ROOT, load_functions, evidence, pair_listing, match_kind, digest


def validate(db):
    functions=load_functions(db)
    by_id={f['id']:f for f in functions}
    expected={}
    eligible=0
    # Deliberately use all combinations, not the optimized sorted window
    for a,b in itertools.combinations(functions,2):
        if abs(a['size']-b['size'])>=16:
            continue
        eligible+=1
        kind=match_kind(a,b)
        if kind:
            p=evidence(a,b,kind);expected[p['id']]=p
    stored=json.loads((project_path('similarity_directory', 'status/similarity')/'matches.json').read_text())
    assert len(stored)==len(expected) and {p['id']:p for p in stored}==expected
    rows=db.execute('SELECT id,left_image,left_address,right_image,right_address,kind,size_delta,compared_words,differing_words,byte_identical,control_shape_equal,risks,evidence,details FROM function_similarity_pairs').fetchall()
    assert len(rows)==len(expected)
    neighbor_counts={f['id']:0 for f in functions}
    for row in rows:
        pid,li,la,ri,ra,kind,delta,n,d,identical,flow,risks,path,detail=row
        p=expected[pid];a=by_id[p['left']];b=by_id[p['right']]
        assert (li,la,ri,ra)==(a['image'],a['address'],b['image'],b['address'])
        assert (kind,delta,n,d,identical,flow)==(p['kind'],p['size_delta'],p['compared_words'],p['differing_words'],p['byte_identical'],p['control_shape_equal'])
        assert json.loads(detail)==p and json.loads(risks)==p['risks'] and path==p['path']
        assert (ROOT/path).read_text(encoding='utf8')==pair_listing(a,b,p)
        neighbor_counts[a['id']]+=1;neighbor_counts[b['id']]+=1
    sql_functions=db.execute('SELECT image,address,size_rank,bytes,source_sha256,peer_count,suggested_image,suggested_address FROM function_similarity ORDER BY size_rank').fetchall()
    assert len(sql_functions)==len(functions)
    for index,(f,row) in enumerate(zip(functions,sql_functions),1):
        im,ea,rank,size,sha,count,bi,ba=row
        assert (im,ea,rank,size,sha,count)==(f['image'],f['address'],index,f['size'],f['sha256'],neighbor_counts[f['id']])
        if (bi,ba)!=(im,ea):
            assert db.execute('SELECT 1 FROM function_reuse WHERE image=? AND address=? AND peer_image=? AND peer_address=?',(im,ea,bi,ba)).fetchone(), 'Non-direct recommended base'
    summary=json.loads((project_path('similarity_directory', 'status/similarity')/'summary.json').read_text())
    assert summary['eligible_pairs_compared']==eligible and summary['matching_pairs']==len(expected)
    inventory=json.loads((project_path('similarity_directory', 'status/similarity')/'sorted-functions.json').read_text())
    assert digest(inventory)==summary['inventory_sha256']
    assert [x['id'] for x in inventory]==[f['id'] for f in functions]
    assert [x['bytes'] for x in inventory]==sorted(f['size'] for f in functions)
    assert json.loads(db.execute("SELECT value FROM metadata WHERE key='function_similarity'").fetchone()[0])==summary
    assert json.loads((ROOT/'status/progress-data.json').read_text())['function_similarity']==summary
    assert not db.execute('PRAGMA foreign_key_check').fetchall()
    result=dict(passed=True,functions_checked=len(functions),all_combinations_checked=len(functions)*(len(functions)-1)//2,eligible_pairs=eligible,matched_pairs=len(expected),evidence_files_checked=len(rows))
    (ROOT/'status/similarity-validation.json').write_text(json.dumps(result,indent=2)+'\n')
    return result


if __name__=='__main__':
    with sqlite3.connect(project_path('analysis_database', 'status/analysis.sqlite').as_uri()+'?mode=ro',uri=True) as db:
        print(json.dumps(validate(db)))

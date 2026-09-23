"""Exhaustive size-window comparison of original PSX MIPS words (not pseudocode).

Registers, opcode, signedness and operation order remain significant. Immediate
values are ignored only in known R3000 instruction formats. Unknown words stay
exact. NOP-relaxed matches are a separate, explicitly weaker reuse candidate.
"""
from pathlib import Path
import collections
import hashlib
import json

from xport_project import project_root, project_path
ROOT = project_root()
SIMILARITY = project_path('similarity_directory', 'status/similarity')
SIMILARITY_REL = SIMILARITY.relative_to(ROOT).as_posix()
VERSION = 'psx-mips-reuse-v1'
MAX_DELTA = 16  # exclusive, before any normalization


def digest(value):
    return hashlib.sha256(json.dumps(value, separators=(',', ':'), sort_keys=True).encode()).hexdigest()


def normalize(word):
    """Return (format, significant bits); never decode through IDA macros."""
    op, rs, fn = word >> 26, (word >> 21) & 31, word & 63
    if word == 0:
        return ('nop', 0)
    if op in (2, 3):
        return ('jump', word & 0xFC000000)
    if (op == 1 and (word >> 16) & 31 not in (0, 1, 16, 17)) or (op == 15 and rs != 0):
        return ('exact', word)
    if op in (1, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
              32, 33, 34, 35, 36, 37, 38, 40, 41, 42, 43, 46,
              48, 49, 50, 51, 56, 57, 58, 59):
        return ('imm16', word & 0xFFFF0000)
    if op == 0 and rs == 0 and fn in (0, 2, 3):
        return ('shift', word & ~0x7C0)
    if op == 0 and fn in (12, 13):
        return ('trap-code', word & 0xFC00003F)
    if op in (16, 17, 18, 19) and rs == 8:
        return ('cop-branch', word & 0xFFFF0000)
    if op == 18 and word & 0x02000000 and fn in (1, 6, 12, 16, 17, 18, 19, 20, 22, 27, 28, 30, 32, 40, 41, 42, 45, 46, 48, 61, 62, 63):
        # GTE command kept; SF/LM/MX/V/CV and other numeric fields ignored.
        return ('gte-command', word & 0xFE00003F)
    return ('exact', word)


def has_delay(word):
    op, rs, fn = word >> 26, (word >> 21) & 31, word & 63
    return op in (1, 2, 3, 4, 5, 6, 7) or (op == 0 and fn in (8, 9)) or (op in (16, 17, 18, 19) and rs == 8)


def prepare(function):
    f = dict(function)
    f['id'] = '%s:%08X' % (f['image'], f['address'])
    f['tokens'] = tuple(normalize(x['word']) for x in f['words'])
    f['active'] = [i for i, w in enumerate(f['words']) if w['word'] != 0]
    f['compact'] = tuple(f['tokens'][i] for i in f['active'])
    f['exact_hash'] = digest(f['tokens'])
    f['compact_hash'] = digest(f['compact'])
    return f


def load_functions(db):
    result = []
    images = {r[0]: r for r in db.execute('SELECT id,source,sha256,base,file_offset FROM images')}
    blobs = {}
    for name, source, expected, base, offset in images.values():
        raw = (ROOT / source).read_bytes()
        assert hashlib.sha256(raw).hexdigest() == expected, ('image changed', name)
        blobs[name] = raw[offset:]
    for image, address, name, size, sha, status in db.execute('SELECT image,address,name,bytes,sha256,status FROM functions ORDER BY bytes,image,address'):
        rows = db.execute('SELECT address,word,disassembly FROM instructions WHERE image=? AND function=? ORDER BY address', (image, address)).fetchall()
        words = []
        base = images[image][3]
        for ea, word, text in rows:
            raw = blobs[image][ea-base:ea-base+4]
            assert len(raw) == 4 and int.from_bytes(raw, 'little') == int(word, 16), (image, ea)
            words.append(dict(address=ea, word=int(word, 16), text=text))
        assert len(words)*4 == size
        assert hashlib.sha256(b''.join(x['word'].to_bytes(4, 'little') for x in words)).hexdigest() == sha
        result.append(prepare(dict(image=image, address=address, name=name, size=size, sha256=sha, status=status, words=words)))
    return result


def match_kind(a, b):
    if abs(a['size'] - b['size']) >= MAX_DELTA:
        return None
    if a['tokens'] == b['tokens']:
        return 'normalized_exact'
    if a['compact'] and a['compact'] == b['compact']:
        return 'nop_relaxed'
    return None


def window_pairs(functions):
    ordered = sorted(functions, key=lambda f: (f['size'], f['image'], f['address']))
    for i, a in enumerate(ordered):
        for b in ordered[i+1:]:
            if b['size'] - a['size'] >= MAX_DELTA:
                break
            yield a, b


def shape(f, indices):
    """Advisory control-flow check; jump addresses remain ignored for matching."""
    ordinals = {f['words'][i]['address']: n for n, i in enumerate(indices)}
    physical = {w['address']: w for w in f['words']}
    result = []
    for index in indices:
        item = f['words'][index]
        ea, w = item['address'], item['word']
        op, target = w >> 26, None
        if op in (2, 3):
            target = ((ea+4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
        elif op in (1, 4, 5, 6, 7) or (op in (16, 17, 18, 19) and (w >> 21) & 31 == 8):
            imm = w & 65535
            target = ea+4+(imm-65536 if imm & 32768 else imm)*4
        if target is not None:
            if target in ordinals:
                target = ('internal', ordinals[target])
            elif target in physical:
                target = ('omitted_nop',)
            else:
                target = ('external',)
        previous = physical.get(ea-4)
        result.append((target, bool(previous and has_delay(previous['word']))))
    return result


def evidence(a, b, kind):
    ia = list(range(len(a['words']))) if kind == 'normalized_exact' else a['active']
    ib = list(range(len(b['words']))) if kind == 'normalized_exact' else b['active']
    assert len(ia) == len(ib)
    differences = []
    for n, (i, j) in enumerate(zip(ia, ib)):
        x, y = a['words'][i], b['words'][j]
        assert normalize(x['word']) == normalize(y['word'])
        if x['word'] != y['word']:
            differences.append(dict(index=n, left_address=x['address'], right_address=y['address'], left_word='%08X'%x['word'], right_word='%08X'%y['word'], ignored_format=normalize(x['word'])[0]))
    flow_equal = shape(a, ia) == shape(b, ib)
    nop_left = [x['address'] for x in a['words'] if x['word'] == 0] if kind == 'nop_relaxed' else []
    nop_right = [x['address'] for x in b['words'] if x['word'] == 0] if kind == 'nop_relaxed' else []
    risk = []
    if len(a['compact']) <= 4:
        risk.append('tiny_generic_pattern')
    if kind == 'nop_relaxed':
        risk.append('NOP/load_delay/timing_review_required')
    if not flow_equal:
        risk.append('internal_target_or_delay_slot_shape_differs')
    if differences:
        risk.append('constants_targets_and_memory_layout_require_adaptation')
    pair_id = 'MIPS-' + digest([a['id'], b['id']])[:20]
    return dict(id=pair_id, left=a['id'], right=b['id'], kind=kind, size_delta=abs(a['size']-b['size']), compared_words=len(ia), differing_words=len(differences), byte_identical=a['sha256']==b['sha256'], control_shape_equal=flow_equal, risks=risk, differences=differences, omitted_left_nops=nop_left, omitted_right_nops=nop_right, path=SIMILARITY_REL+'/pairs/'+pair_id+'.mips.txt')


def pair_listing(a, b, p):
    ia = list(range(len(a['words']))) if p['kind']=='normalized_exact' else a['active']
    ib = list(range(len(b['words']))) if p['kind']=='normalized_exact' else b['active']
    lines = [VERSION, p['id'], 'LEFT '+a['id']+' SHA256 '+a['sha256'], 'RIGHT '+b['id']+' SHA256 '+b['sha256'], 'KIND '+p['kind']+'; size delta '+str(p['size_delta']), 'Candidate for reuse, NOT semantic equivalence; statuses unchanged.', 'Risks: '+', '.join(p['risks']), 'Omitted left NOP addresses: '+', '.join('%08X'%x for x in p['omitted_left_nops']), 'Omitted right NOP addresses: '+', '.join('%08X'%x for x in p['omitted_right_nops']), '', 'LEFT ADDRESS WORD INSTRUCTION | RIGHT ADDRESS WORD INSTRUCTION | NORMALIZED']
    for i,j in zip(ia,ib):
        x,y=a['words'][i],b['words'][j]
        lines.append('%08X %08X %-48s | %08X %08X %-48s | %s %08X%s' % (x['address'],x['word'],x['text'],y['address'],y['word'],y['text'],*normalize(x['word']),' * ignored operand difference' if x['word']!=y['word'] else ''))
    return '\n'.join(lines)+'\n'


def run(db, emit=True):
    functions = load_functions(db)
    by_id = {f['id']: f for f in functions}
    comparisons = 0
    size_deltas = collections.Counter()
    pairs = []
    for a,b in window_pairs(functions):
        comparisons += 1
        size_deltas[abs(a['size']-b['size'])] += 1
        kind = match_kind(a,b)
        if kind:
            pairs.append(evidence(a,b,kind))
    neighbors = collections.defaultdict(list)
    for p in pairs:
        neighbors[p['left']].append((p, p['right']))
        neighbors[p['right']].append((p, p['left']))
    group_ids = {f['id']: 'SEQ-'+f['compact_hash'][:16] for f in functions if f['id'] in neighbors}
    recommendations = {}
    for f in functions:
        candidates = [(f['id'], None)] + [(other,p) for p,other in neighbors[f['id']]]
        # Direct peers only, never infer equivalence transitively through groups.
        def rank(candidate):
            other,p = candidate; g=by_id[other]
            return ({'DONE':0,'WIP':1,'TODO':2,'SKIP':3}[g['status']], bool(p and not p['control_shape_equal']), bool(p and p['kind']=='nop_relaxed'), -g['size'], g['image'], g['address'])
        recommendations[f['id']] = min(candidates,key=rank)[0]
    summary = dict(version=VERSION, functions=len(functions), size_delta_exclusive=MAX_DELTA, eligible_pairs_compared=comparisons, matching_pairs=len(pairs), kinds=dict(collections.Counter(p['kind'] for p in pairs)), matched_functions=sum(bool(neighbors[f['id']]) for f in functions), sequence_groups=len(set(group_ids.values())), matched_non_skip_functions=sum(f['status']!='SKIP' and bool(neighbors[f['id']]) for f in functions), cross_image_pairs=sum(by_id[p['left']]['image']!=by_id[p['right']]['image'] for p in pairs), within_image_pairs=sum(by_id[p['left']]['image']==by_id[p['right']]['image'] for p in pairs), byte_identical_pairs=sum(p['byte_identical'] for p in pairs), control_shape_differences=sum(not p['control_shape_equal'] for p in pairs), tiny_pattern_pairs=sum('tiny_generic_pattern' in p['risks'] for p in pairs), status_policy='reuse candidates only; TODO/WIP/DONE/SKIP unchanged')
    summary['eligible_pairs_by_size_delta'] = {str(k):v for k,v in sorted(size_deltas.items())}
    if not emit:
        return functions, pairs, summary
    db.executescript('''
    DROP VIEW IF EXISTS function_reuse;
    DROP VIEW IF EXISTS functions_by_size;
    DROP TABLE IF EXISTS function_similarity_pairs;
    DROP TABLE IF EXISTS function_similarity;
    CREATE TABLE function_similarity(image TEXT,address INTEGER,size_rank INTEGER,bytes INTEGER,word_count INTEGER,non_nop_words INTEGER,normalized_sha256 TEXT,nop_relaxed_sha256 TEXT,source_sha256 TEXT,sequence_group TEXT,peer_count INTEGER,suggested_image TEXT,suggested_address INTEGER,PRIMARY KEY(image,address),FOREIGN KEY(image,address) REFERENCES functions(image,address),FOREIGN KEY(suggested_image,suggested_address) REFERENCES functions(image,address));
    CREATE TABLE function_similarity_pairs(id TEXT PRIMARY KEY,left_image TEXT,left_address INTEGER,right_image TEXT,right_address INTEGER,kind TEXT,size_delta INTEGER CHECK(size_delta<16),compared_words INTEGER,differing_words INTEGER,byte_identical INTEGER,control_shape_equal INTEGER,risks TEXT,evidence TEXT,details TEXT,FOREIGN KEY(left_image,left_address) REFERENCES functions(image,address),FOREIGN KEY(right_image,right_address) REFERENCES functions(image,address));
    CREATE INDEX similarity_right ON function_similarity_pairs(right_image,right_address);
    CREATE INDEX similarity_left ON function_similarity_pairs(left_image,left_address);
    CREATE VIEW functions_by_size AS SELECT f.*,s.size_rank FROM functions f JOIN function_similarity s USING(image,address) ORDER BY f.bytes,f.image,f.address;
    CREATE VIEW function_reuse AS SELECT left_image AS image,left_address AS address,right_image AS peer_image,right_address AS peer_address,id,kind,size_delta,control_shape_equal,risks,evidence FROM function_similarity_pairs UNION ALL SELECT right_image,right_address,left_image,left_address,id,kind,size_delta,control_shape_equal,risks,evidence FROM function_similarity_pairs;
    ''')
    out = SIMILARITY; (out/'pairs').mkdir(parents=True,exist_ok=True)
    inventory = []
    for index,f in enumerate(functions,1):
        representative = by_id[recommendations[f['id']]]
        db.execute('INSERT INTO function_similarity VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)',(f['image'],f['address'],index,f['size'],len(f['words']),len(f['compact']),f['exact_hash'],f['compact_hash'],f['sha256'],group_ids.get(f['id']),len(neighbors[f['id']]),representative['image'],representative['address']))
        inventory.append(dict(rank=index,id=f['id'],bytes=f['size'],source_sha256=f['sha256'],normalized_sha256=f['exact_hash'],nop_relaxed_sha256=f['compact_hash'],sequence_group=group_ids.get(f['id']),peer_count=len(neighbors[f['id']]),suggested_base=representative['id']))
    for p in pairs:
        a,b=by_id[p['left']],by_id[p['right']]
        db.execute('INSERT INTO function_similarity_pairs VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)',(p['id'],a['image'],a['address'],b['image'],b['address'],p['kind'],p['size_delta'],p['compared_words'],p['differing_words'],int(p['byte_identical']),int(p['control_shape_equal']),json.dumps(p['risks']),p['path'],json.dumps(p)))
        (ROOT/p['path']).write_text(pair_listing(a,b,p),encoding='utf8')
    summary['inventory_sha256'] = digest(inventory)
    for key,value in [('sorted-functions.json',inventory),('matches.json',pairs),('summary.json',summary)]:
        (out/key).write_text(json.dumps(value,indent=2)+'\n',encoding='utf8')
    db.execute('INSERT OR REPLACE INTO metadata VALUES(?,?)',('function_similarity',json.dumps(summary)))
    return summary


if __name__ == '__main__':
    import sqlite3
    db=sqlite3.connect(project_path('analysis_database', 'status/analysis.sqlite'))
    db.execute('PRAGMA foreign_keys=ON')
    summary=run(db)
    assert not db.execute('PRAGMA foreign_key_check').fetchall()
    db.commit();db.close()
    progress_path=ROOT/'status/progress-data.json'
    progress=json.loads(progress_path.read_text());progress['function_similarity']=summary
    progress_path.write_text(json.dumps(progress,indent=2)+'\n')
    print(json.dumps(summary))

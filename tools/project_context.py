"""Store concise project context and complete translation bindings in SQLite"""
from __future__ import annotations

import argparse
from contextlib import closing
from functools import lru_cache
import hashlib
import json
from pathlib import Path
import sqlite3

from xport_project import load_project


def digest(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def database_path(root, config):
    path = (root / config.get('paths', {}).get('project_database', 'status/project.sqlite')).resolve()
    if not path.is_relative_to((root / 'status').resolve()):
        raise ValueError('Project database must remain under project status')
    return path


def connect(root, config, write=False):
    path = database_path(root, config)
    if not write:
        if not path.is_file():
            return None
        db = sqlite3.connect(path.as_uri() + '?mode=ro', uri=True)
    else:
        path.parent.mkdir(parents=True, exist_ok=True)
        db = sqlite3.connect(path, timeout=5)
    db.row_factory = sqlite3.Row
    db.execute('PRAGMA foreign_keys=ON')
    if write:
        db.executescript('''
        CREATE TABLE IF NOT EXISTS context (
          key TEXT PRIMARY KEY, kind TEXT NOT NULL, stage TEXT NOT NULL,
          image TEXT NOT NULL, address INTEGER, subsystem TEXT NOT NULL,
          summary TEXT NOT NULL, scope TEXT NOT NULL, configuration TEXT NOT NULL,
          next_step TEXT NOT NULL, closed INTEGER NOT NULL DEFAULT 0);
        CREATE INDEX IF NOT EXISTS context_subject ON context(image,address,subsystem,stage);
        CREATE TABLE IF NOT EXISTS references_context (
          key TEXT NOT NULL REFERENCES context(key) ON DELETE CASCADE,
          path TEXT NOT NULL, sha256 TEXT NOT NULL, PRIMARY KEY(key,path));
        CREATE TABLE IF NOT EXISTS translations (
          image TEXT NOT NULL, address INTEGER NOT NULL, original_sha256 TEXT NOT NULL,
          source TEXT NOT NULL, symbol TEXT NOT NULL, body_sha256 TEXT NOT NULL,
          PRIMARY KEY(image,address));
        CREATE TABLE IF NOT EXISTS translation_evidence (
          image TEXT NOT NULL,address INTEGER NOT NULL,path TEXT NOT NULL,sha256 TEXT NOT NULL,
          PRIMARY KEY(image,address,path),
          FOREIGN KEY(image,address) REFERENCES translations(image,address) ON DELETE CASCADE);
        ''')
    return db


def reference(root, value):
    path = (root / value.split('#', 1)[0]).resolve()
    shared = Path(__file__).resolve().parents[1]
    if value.startswith('@xport/'):
        path = (shared / value.removeprefix('@xport/').split('#', 1)[0]).resolve()
        if not path.is_relative_to(shared):
            raise ValueError('Reference escapes xport')
    elif not path.is_relative_to(root):
        raise ValueError('Reference escapes project')
    if not path.is_file():
        raise ValueError('Missing evidence: ' + value)
    return path


def bounded(value, name, maximum=1200):
    if not isinstance(value, str) or len(value) > maximum:
        raise ValueError(name + ' exceeds ' + str(maximum) + ' characters')
    return value


def put(root, config, *, key, kind, stage='', image='', address=None, subsystem='',
        summary, scope='', configuration='', next_step='', evidence=(), closed=False):
    if kind not in ('task', 'finding', 'check', 'question'):
        raise ValueError('Unknown context kind')
    if not key.strip() or not summary.strip() or (address is not None and not image):
        raise ValueError('Context needs a key, summary and image-qualified addresses')
    if kind in ('finding', 'check') and (not evidence or not scope.strip()):
        raise ValueError('Findings/checks require evidence and explicit proof scope')
    if len(evidence) > 16:
        raise ValueError('Use at most 16 direct evidence references per concise record')
    values = [bounded(key, 'key', 128), kind, bounded(stage, 'stage', 64),
              bounded(image, 'image', 128), address, bounded(subsystem, 'subsystem', 128),
              bounded(summary, 'summary'), bounded(scope, 'scope'),
              bounded(configuration, 'configuration'), bounded(next_step, 'next_step'), int(closed)]
    refs = [(key, value, digest(reference(root, value))) for value in dict.fromkeys(evidence)]
    with closing(connect(root, config, True)) as db, db:
        db.execute('''INSERT INTO context VALUES(?,?,?,?,?,?,?,?,?,?,?)
          ON CONFLICT(key) DO UPDATE SET kind=excluded.kind,stage=excluded.stage,
          image=excluded.image,address=excluded.address,subsystem=excluded.subsystem,
          summary=excluded.summary,scope=excluded.scope,configuration=excluded.configuration,
          next_step=excluded.next_step,closed=excluded.closed''', values)
        db.execute('DELETE FROM references_context WHERE key=?', (key,))
        db.executemany('INSERT INTO references_context VALUES(?,?,?)', refs)
    return {'key': key, 'saved': True}


def reference_state(root, rows):
    result = []
    for row in rows:
        item = dict(row)
        try:
            item['current'] = digest(reference(root, item['path'])) == item['sha256']
        except ValueError:
            item['current'] = False
        result.append(item)
    return result


def show(root, config, *, image=None, address=None, subsystem=None, stage=None,
         key=None, limit=8, after='', include_closed=False):
    if not 1 <= limit <= 32:
        raise ValueError('Limit must be 1..32')
    if address is not None and not image:
        raise ValueError('Address requires image')
    db = connect(root, config)
    if db is None:
        return {'initialized': False, 'records': [], 'next': None}
    clauses = ['key>?']; args = [after]
    if not include_closed:
        clauses.append('closed=0')
    for name, value in (('image', image), ('address', address), ('subsystem', subsystem),
                        ('stage', stage), ('key', key)):
        if value is not None:
            # Global task scope accompanies a focused query
            if name in ('image', 'address', 'subsystem', 'stage'):
                clauses.append(f'({name}=? OR (kind=\'task\' AND {name} IS NULL))' if name == 'address'
                               else f'({name}=? OR (kind=\'task\' AND {name}=\'\'))')
            else:
                clauses.append(name + '=?')
            args.append(value)
    with closing(db):
        rows = db.execute('SELECT * FROM context WHERE ' + ' AND '.join(clauses) +
                          ' ORDER BY key LIMIT ?', [*args, limit + 1]).fetchall()
        records = []
        for row in rows[:limit]:
            item = dict(row)
            refs = db.execute('SELECT path,sha256 FROM references_context WHERE key=? ORDER BY path',
                              (item['key'],)).fetchall()
            item['evidence'] = reference_state(root, refs)
            item['applicability'] = ('needs_review' if any(not r['current'] for r in item['evidence'])
                                     else 'references_unchanged' if refs else 'not_bound')
            records.append(item)
    return {'initialized': True, 'records': records,
            'next': records[-1]['key'] if len(rows) > limit else None}


def analysis_path(root, config):
    return (root / config.get('paths', {}).get('analysis_database', 'status/analysis.sqlite')).resolve()


def current_library_classification(root, config, db):
    relative = config.get('paths', {}).get('sdk_classification')
    if not relative:
        return {(row['image'], row['address']): dict(row)
                for row in db.execute('SELECT * FROM library_classification')}
    document = json.loads(reference(root, relative).read_text(encoding='utf-8-sig'))
    accepted = document.get('accepted') if isinstance(document, dict) else None
    if not isinstance(accepted, list):
        raise ValueError('SDK classification requires an accepted list')
    result = {}
    for entry in accepted:
        if (not isinstance(entry, dict) or not isinstance(entry.get('image'), str) or
                not entry['image'].strip() or type(entry.get('address')) is not int or
                not isinstance(entry.get('alias'), str) or not entry['alias'].strip() or
                not isinstance(entry.get('reason'), str) or not entry['reason'].strip()):
            raise ValueError('Invalid accepted SDK classification')
        identity = (entry['image'], entry['address'])
        original = db.execute('SELECT sha256 FROM functions WHERE image=? AND address=?', identity).fetchone()
        if identity in result or original is None or original['sha256'] != entry.get('sha256'):
            raise ValueError('Duplicate or stale SDK classification: %s:%08X' % identity)
        result[identity] = {'image': entry['image'], 'address': entry['address'],
                            'alias': entry['alias'], 'reason': entry['reason'],
                            'evidence': entry, 'classification_source': relative}
    return result


def original_function(root, config, image, address):
    with closing(sqlite3.connect(analysis_path(root, config).as_uri() + '?mode=ro', uri=True)) as db:
        db.row_factory = sqlite3.Row
        row = db.execute('SELECT * FROM functions WHERE image=? AND address=?', (image, address)).fetchone()
    if row is None:
        raise ValueError('Unknown image/function')
    return dict(row)


def source_body(root, source, symbol, cache=None, *, image=None, address=None):
    from source_index import source_functions
    path = reference(root, source)
    if not path.is_relative_to((root / 'src').resolve()) or path.suffix.lower() not in ('.c', '.cpp', '.cc', '.h', '.hpp'):
        raise ValueError('Translation source must be project src C/C++')
    if cache is not None and source in cache:
        parsed = cache[source]
    else:
        parsed = source_functions(path.read_text(encoding='utf-8-sig'), source)
        from marker_names import canonicalize
        parsed = canonicalize(root, parsed)
        if cache is not None:
            cache[source] = parsed
    rows = [row for row in parsed if row['name'] == symbol]
    if len(rows) > 1 and image is not None and address is not None:
        rows = [row for row in rows if (address, image) in row['markers']]
    if len(rows) != 1:
        raise ValueError('Translation needs one unique complete source function')
    return rows[0]


def decompile(root, config, image, address, source, symbol, evidence, complete=False):
    if not complete or not evidence:
        raise ValueError('Explicit complete-body audit and evidence are required; drafts do not count')
    original = original_function(root, config, image, address)
    with closing(sqlite3.connect(analysis_path(root, config).as_uri() + '?mode=ro', uri=True)) as db:
        db.row_factory = sqlite3.Row
        if (image, address) in current_library_classification(root, config, db):
            raise ValueError('Reviewed SDK identity is a separate runtime API boundary, not a game translation')
    body = source_body(root, source, symbol, image=image, address=address)
    from function_entry_bindings import marker_binding
    entry = marker_binding(root, config, image, address, original, body)
    if entry:
        evidence = [*evidence, *entry['evidence']]
    refs = [(image, address, value, digest(reference(root, value))) for value in dict.fromkeys(evidence)]
    with closing(connect(root, config, True)) as db, db:
        db.execute('''INSERT INTO translations VALUES(?,?,?,?,?,?) ON CONFLICT(image,address)
          DO UPDATE SET original_sha256=excluded.original_sha256,source=excluded.source,
          symbol=excluded.symbol,body_sha256=excluded.body_sha256''',
                   (image, address, original['sha256'], source, symbol, body['sha256']))
        db.execute('DELETE FROM translation_evidence WHERE image=? AND address=?', (image, address))
        db.executemany('INSERT INTO translation_evidence VALUES(?,?,?,?)', refs)
    return {'image': image, 'address': address, 'decompiled': True, 'proof': 'See scoped evidence'}


def translation_state(root, original, row, cache=None):
    result = {'decompiled': False}
    if row:
        result.update(source=row['source'], symbol=row['symbol'])
        try:
            body = source_body(root, row['source'], row['symbol'], cache, image=row['image'], address=row['address'])
            valid = row['original_sha256'] == original['sha256'] and body['sha256'] == row['body_sha256']
            if valid and (original['address'], original['image']) not in body['markers']:
                from function_entry_bindings import marker_binding
                marker_binding(root, load_project(root)[1], original['image'], original['address'], original, body)
        except (ValueError, OSError):
            valid = False
        result.update(decompiled=valid, binding='current' if valid else 'needs_review')
    return result


def formatting_snapshot(root, paths):
    """Capture only current complete bindings before the formatter changes bytes"""
    root = Path(root).resolve()
    _, config = load_project(root)
    db = connect(root, config)
    if db is None:
        return None
    selected = set(paths)
    with closing(db):
        rows = [dict(row) for row in db.execute('SELECT * FROM translations') if row['source'] in selected]
    files = {source: (root / source).read_bytes() for source in {row['source'] for row in rows}}
    current, cache = [], {}
    for row in rows:
        try:
            if source_body(root, row['source'], row['symbol'], cache, image=row['image'], address=row['address'])['sha256'] == row['body_sha256']:
                current.append(row)
        except (OSError, ValueError):
            pass
    headers = {}
    roots = (root / 'src', Path(__file__).resolve().parents[1] / 'src')
    for folder in roots:
        for path in folder.rglob('*'):
            if path.is_file() and path.suffix.lower() in ('.h', '.hpp'):
                headers[path.resolve()] = path.read_bytes()
    return {'config': config, 'rows': current, 'files': files, 'headers': headers}


@lru_cache(maxsize=2)
def formatting_macros(headers):
    """Reuse header macro analysis across all files in one formatting pass"""
    import re
    result = {}
    for raw in headers:
        text = re.sub(r'\\\r?\n', '', raw.decode('utf-8-sig'))
        for match in re.finditer(r'^\s*#\s*define\s+(\w+)(.*)$', text, re.M):
            body = match[2]
            if body.startswith('(') and ')' in body:
                body = body.split(')', 1)[1]
            result[match[1]] = result.get(match[1], '') + '\n' + body
    return result


def editorial_equal(before, after, headers=()):
    """Reject changed tokens, directives and spelling-sensitive macro arguments"""
    import re
    from refactor_source import tokens
    from source_index import mask_c, matching
    old, new = before.decode('utf-8-sig'), after.decode('utf-8-sig')
    if tokens(old) != tokens(new):
        return False
    special = {'__LINE__', '__COUNTER__', '__INCLUDE_LEVEL__'}
    if set(tokens(old)[0]) & special:
        return False
    sensitive = {'assert', 'XASSERT'}
    macros = dict(formatting_macros(tuple(headers)))
    source_macros = formatting_macros((before,))
    for name, body in source_macros.items():
        macros[name] = macros.get(name, '') + '\n' + body
    uses = {name: set(re.findall(r'\b\w+\b', body)) for name, body in macros.items()}
    line_macros = {name for name in macros if uses[name] & special}
    while True:
        added = {name for name in macros if uses[name] & line_macros} - line_macros
        if not added:
            break
        line_macros.update(added)
    if (set(tokens(old)[0]) & line_macros) - {'XASSERT', 'FUNCTION_MARKER', 'FUNCTION_MARKER_ARGS'}:
        return False
    sensitive.update(name for name, body in macros.items() if '#' in body)
    while True:
        added = {name for name in macros if uses[name] & sensitive} - sensitive
        if not added:
            break
        sensitive.update(added)
    def spellings(text):
        masked = mask_c(text)
        result = []
        for match in re.finditer(r'\b([A-Za-z_]\w*)\s*\(', masked):
            name = match[1]
            if name not in sensitive and not (name.isupper() and name not in macros):
                continue
            start = masked.index('(', match.start())
            end = matching(masked, start, '(', ')')
            line = text.count('\n', 0, match.start()) if name.isupper() and name not in macros and name not in sensitive else None
            result.append((name, text[start:end + 1], line))
        return result
    return spellings(old) == spellings(new)


def rebind_formatted(root, snapshot):
    """Transfer complete-body bindings only across checked formatter-only edits"""
    result = {'rebound': 0, 'requires_review': []}
    if snapshot is None:
        return result
    if any(not path.is_file() or path.read_bytes() != raw for path, raw in snapshot['headers'].items()):
        result['requires_review'] = list(snapshot['files'])
        return result
    safe = set()
    after_files = {}
    for source, before in snapshot['files'].items():
        after = (root / source).read_bytes()
        try:
            if editorial_equal(before, after, snapshot['headers'].values()):
                safe.add(source)
                after_files[source] = after
            else:
                result['requires_review'].append(source)
        except (UnicodeError, ValueError):
            result['requires_review'].append(source)
    with closing(connect(root, snapshot['config'], True)) as db, db:
        db.execute('''CREATE TABLE IF NOT EXISTS formatting_rebindings (
          image TEXT,address INTEGER,before_sha256 TEXT,after_sha256 TEXT,
          source_sha256 TEXT,PRIMARY KEY(image,address))''')
        db.execute('''CREATE TABLE IF NOT EXISTS formatting_transfers (
          image TEXT,address INTEGER,source TEXT,before_sha256 TEXT,after_sha256 TEXT,
          before_source_sha256 TEXT,after_source_sha256 TEXT,
          PRIMARY KEY(image,address,source,before_sha256,after_sha256,before_source_sha256,after_source_sha256))''')
        cache = {}
        for row in snapshot['rows']:
            if row['source'] not in safe:
                continue
            try:
                body = source_body(root, row['source'], row['symbol'], cache, image=row['image'], address=row['address'])
                original = original_function(root, snapshot['config'], row['image'], row['address'])
                if original['sha256'] != row['original_sha256']:
                    continue
                from function_entry_bindings import marker_binding
                marker_binding(root, snapshot['config'], row['image'], row['address'], original, body)
            except (OSError, ValueError):
                continue
            if (root / row['source']).read_bytes() != after_files[row['source']]:
                raise ValueError('Source changed during formatting rebind')
            cursor = db.execute('''UPDATE translations SET body_sha256=?
              WHERE image=? AND address=? AND source=? AND symbol=? AND body_sha256=? AND original_sha256=?''',
              (body['sha256'], row['image'], row['address'], row['source'], row['symbol'], row['body_sha256'], row['original_sha256']))
            if cursor.rowcount and body['sha256'] != row['body_sha256']:
                db.execute('INSERT OR REPLACE INTO formatting_rebindings VALUES(?,?,?,?,?)',
                           (row['image'], row['address'], row['body_sha256'], body['sha256'],
                            hashlib.sha256(after_files[row['source']]).hexdigest()))
                db.execute('INSERT OR IGNORE INTO formatting_transfers VALUES(?,?,?,?,?,?,?)',
                           (row['image'], row['address'], row['source'], row['body_sha256'], body['sha256'],
                            hashlib.sha256(snapshot['files'][row['source']]).hexdigest(),
                            hashlib.sha256(after_files[row['source']]).hexdigest()))
                result['rebound'] += 1
    return result


def function_scopes(root, config, db):
    relative = config.get('analysis', {}).get('function_scope')
    if not relative:
        return {}
    document = json.loads(reference(root, relative).read_text(encoding='utf-8-sig'))
    if document.get('schema') != 'xport-function-scope-v1':
        raise ValueError('Unsupported function scope schema')
    result = {}
    for item in document['rows']:
        identity = (document['image'], int(item['address'], 16))
        original = db.execute('SELECT sha256 FROM functions WHERE image=? AND address=?', identity).fetchone()
        if identity in result or not original or original[0] != item['original_sha256']:
            raise ValueError('Duplicate or stale function scope identity')
        if item['category'] not in ('needed', 'possibly_needed', 'replaced', 'dead_code_candidate'):
            raise ValueError('Unknown scope category')
        result[identity] = {'category': item['category'], 'reason': item['reason'], 'evidence': relative}
    return result


def source_candidates(db, image=None, address=None):
    columns = {r[1] for r in db.execute('PRAGMA table_info(implementations)')}
    if not {'image', 'address', 'source_path', 'symbol', 'mapping_status'} <= columns:
        return {}
    statement = "SELECT * FROM implementations WHERE mapping_status='mapped'"
    args = ()
    if image is not None:
        statement += ' AND image=? AND address=?'
        args = (image, address)
    return {(r['image'], r['address']): {'source': r['source_path'], 'symbol': r['symbol'],
            'completeness': 'unconfirmed; inspect existing body/audit before retranslating'}
            for r in db.execute(statement, args)}


def queue(root, config, image=None, limit=8, after=None, state=None):
    if not 1 <= limit <= 32 or state not in (None, 'uncovered', 'decompiled', 'sdk'):
        raise ValueError('Invalid queue limit/state')
    if config.get('analysis', {}).get('function_queue') not in (None, 'image_qualified', 'unique_exact_bodies', 'operand_templates'):
        raise ValueError('Unsupported analysis.function_queue mode')
    path = analysis_path(root, config)
    if not path.is_file():
        return {'available': False, 'counts': {}, 'functions': [], 'next': None}
    saved = {}
    db = connect(root, config)
    if db:
        with closing(db):
            saved = {(r['image'], r['address']): dict(r) for r in db.execute('SELECT * FROM translations')}
    with closing(sqlite3.connect(path.as_uri() + '?mode=ro', uri=True)) as db:
        db.row_factory = sqlite3.Row
        scopes = function_scopes(root, config, db)
        candidates = source_candidates(db)
        libraries = current_library_classification(root, config, db)
        originals = db.execute('SELECT image,address,name,sha256 FROM functions' +
                               (' WHERE image=?' if image else '') + ' ORDER BY image,address',
                               (image,) if image else ()).fetchall()
        excluded = decompilation_exclusions(root, config, db)
        excluded_count = sum((row['image'], row['address']) in excluded for row in originals)
        originals = [row for row in originals if (row['image'], row['address']) not in excluded]
        if config.get('analysis', {}).get('function_queue') in ('unique_exact_bodies', 'operand_templates'):
            result = unique_body_queue(root, config, db, originals, saved, libraries, candidates, scopes,
                                       limit=limit, after=after, state=state)
            result['excluded_occurrences'] = excluded_count
            return result
        counts = {'uncovered': 0, 'decompiled': 0, 'sdk': 0}
        selected = []; cache = {}
        for original in originals:
            original = dict(original); identity = (original['image'], original['address'])
            binding = translation_state(root, original, saved.get(identity), cache)
            classification = libraries.get(identity)
            category = 'sdk' if classification else 'decompiled' if binding['decompiled'] else 'uncovered'
            counts[category] += 1
            cursor = original['image'] + ':' + format(original['address'], '08X')
            if (state is None or state == category) and (after is None or cursor > after):
                if len(selected) <= limit:
                    row = dict(image=identity[0], address=identity[1], name=original['name'],
                               decompiled=binding['decompiled'], **{k: v for k, v in binding.items() if k != 'decompiled'})
                    if classification:
                        row['sdk'] = {'alias': classification['alias'], 'reason': classification['reason'],
                                      'boundary': 'Reviewed identity; validate existing runtime API and caller ABI'}
                    if identity in scopes:
                        row['scope'] = scopes[identity]
                    if not binding['decompiled'] and identity in candidates:
                        row['candidate_source'] = candidates[identity]
                    row['_cursor'] = cursor
                    selected.append(row)
    next_cursor = selected[limit - 1]['_cursor'] if len(selected) > limit else None
    for row in selected:
        row.pop('_cursor')
    return {'available': True, 'counts': counts, 'functions': selected[:limit], 'next': next_cursor,
            'excluded_occurrences': excluded_count}


def decompilation_exclusions(root, config, db):
    relative = config.get('analysis', {}).get('decompilation_exclusions')
    if relative is None:
        return {}
    path = (root / relative).resolve()
    if not path.is_relative_to(root.resolve()):
        raise ValueError('Decompilation exclusions must remain inside the project')
    document = json.loads(path.read_text(encoding='utf-8'))
    if document.get('schema') not in (1, 2, 3):
        raise ValueError('Unsupported decompilation exclusions schema')
    if document.get('schema') == 1 and document.get('removals'):
        raise ValueError('Explicit removals require exclusions schema 2')
    if document.get('schema') != 3 and document.get('native_replacements'):
        raise ValueError('Native replacements require exclusions schema 3')
    result = {}
    seen = set()
    for group in document.get('groups', []):
        image, representative = group['image'], group['representative']
        copies = group['copies']
        if len(copies) < 2 or sum(copy['address'] == representative for copy in copies) != 1:
            raise ValueError('Invalid exclusion representative')
        for copy in copies:
            identity = (image, copy['address'])
            if identity in seen:
                raise ValueError('Duplicate exclusion identity')
            seen.add(identity)
            row = db.execute('SELECT sha256 FROM functions WHERE image=? AND address=?', identity).fetchone()
            if row is None or row['sha256'] != copy['sha256']:
                raise ValueError('Stale decompilation exclusion: %s:%08X' % identity)
            if copy['address'] != representative:
                result[identity] = (image, representative)
    for removal in document.get('removals', []):
        identity = (removal['image'], removal['address'])
        if identity in seen:
            raise ValueError('Duplicate exclusion identity')
        seen.add(identity)
        if removal.get('reason') != 'user_removal' or not removal.get('authorization', '').strip():
            raise ValueError('Explicit removal requires recorded user authorization')
        evidence = removal.get('evidence', [])
        if not evidence:
            raise ValueError('Explicit removal requires evidence')
        for item in evidence:
            if digest(reference(root, item['path'])) != item['sha256']:
                raise ValueError('Stale removal evidence: %s' % item['path'])
        row = db.execute('SELECT sha256 FROM functions WHERE image=? AND address=?', identity).fetchone()
        if row is None or row['sha256'] != removal['sha256']:
            raise ValueError('Stale decompilation exclusion: %s:%08X' % identity)
        result[identity] = None
    source_cache = {}
    for replacement in document.get('native_replacements', []):
        identity = (replacement['image'], replacement['address'])
        if identity in seen:
            raise ValueError('Duplicate exclusion identity')
        seen.add(identity)
        contracts = {
            'native_file_replacement': ('runtime_file_access',
                '- During decompilation replace identified game file lookup, open, read, seek and CD-ready wrappers'),
            'native_save_replacement': ('runtime_game_save',
                'Replace card-sector I/O, card managers, formatting/directory/device checks and save-slot/confirmation screens'),
            'native_movie_replacement': ('runtime_game_movie',
                'Fullscreen movies call xport_fmv_play; in-game/background/texture movies call xport_fmv_open'),
        }
        permission = contracts.get(replacement.get('reason'))
        if (permission is None or replacement.get('reviewed') is not True or
                not replacement.get('scope', '').strip()):
            raise ValueError('Native replacement requires an explicit completed scope review')
        authority = replacement.get('authorization', {})
        if (authority.get('kind') != permission[0] or
                authority.get('path') != '@xport/docs/DECOMP_REFERENCE.md'):
            raise ValueError('Native replacement requires its matching runtime contract')
        contract = reference(root, authority['path'])
        if digest(contract) != authority.get('sha256'):
            raise ValueError('Stale native replacement authorization')
        quote = authority.get('quote', '')
        if (not quote.startswith(permission[1]) or
                quote not in contract.read_text(encoding='utf-8')):
            raise ValueError('Native replacement authorization quote is missing or stale')
        evidence = replacement.get('evidence', [])
        if not evidence:
            raise ValueError('Native replacement requires evidence')
        for item in evidence:
            if digest(reference(root, item['path'])) != item['sha256']:
                raise ValueError('Stale replacement evidence: %s' % item['path'])
        row = db.execute('SELECT sha256 FROM functions WHERE image=? AND address=?', identity).fetchone()
        if row is None or row['sha256'] != replacement['sha256']:
            raise ValueError('Stale decompilation exclusion: %s:%08X' % identity)
        for binding in [replacement, *replacement.get('dependencies', [])]:
            body = source_body(root, binding['source'], binding['symbol'], cache=source_cache)
            if body['sha256'] != binding['source_body_sha256']:
                raise ValueError('Stale native replacement body: %s:%s' %
                                 (binding['source'], binding['symbol']))
        result[identity] = None
    return result


def unique_body_queue(root, config, db, originals, saved, libraries, candidates, scopes, *, limit, after, state):
    from function_work_units import queue_groups

    mode = config.get('analysis', {}).get('function_queue', 'unique_exact_bodies')
    template_metadata = {}
    if mode == 'operand_templates':
        from operand_template_queue import queue_groups as template_groups
        groups, template_metadata = template_groups(db, originals, root, config)
    else:
        groups = queue_groups(db, originals)
    counts = {'uncovered': 0, 'decompiled': 0, 'sdk': 0}
    occurrence_counts = {'uncovered': 0, 'decompiled': 0, 'sdk': 0}
    selected, cache = [], {}
    default = config.get('analysis', {}).get('default_image')
    for unit, members in groups.items():
        game, sdk, complete = [], [], []
        bindings = {}
        for original in members:
            identity = (original['image'], original['address'])
            binding = translation_state(root, dict(original), saved.get(identity), cache)
            bindings[identity] = binding
            category = 'sdk' if identity in libraries else 'decompiled' if binding['decompiled'] else 'uncovered'
            occurrence_counts[category] += 1
            (sdk if category == 'sdk' else complete if category == 'decompiled' else game).append(original)
        category = 'uncovered' if game else 'decompiled' if complete else 'sdk'
        counts[category] += 1
        choices = game or complete or sdk
        representative = min(choices, key=lambda row: (row['image'] != default, row['image'], row['address']))
        identity = (representative['image'], representative['address'])
        cursor = identity[0] + ':' + format(identity[1], '08X')
        if (state is not None and state != category) or after is not None and cursor <= after:
            continue
        row = dict(image=identity[0], address=identity[1], name=representative['name'], **bindings[identity])
        row.update(work_unit=unit, occurrence_count=len(members), grouping='Exact original bytes, GP and relative chunk layout; no acceptance transfer',
                   occurrences=[{'image': item['image'], 'address': item['address']} for item in members[:8]],
                   additional_occurrences=max(0, len(members) - 8), evidence='status/decompilation-units.json', _cursor=cursor)
        if complete and game:
            row['task'] = 'Review existing complete body against remaining image contexts and bind aliases; do not retranslate automatically'
            row['complete_occurrences'] = [{'image': item['image'], 'address': item['address'], **bindings[(item['image'], item['address'])]} for item in complete[:8]]
        if sdk and game:
            row['task'] = 'Review duplicate occurrence ABI against the known SDK body; do not translate the SDK algorithm again'
            row['reviewed_sdk_occurrences'] = [{'image': item['image'], 'address': item['address'], 'alias': libraries[(item['image'], item['address'])]['alias']} for item in sdk[:8]]
        if identity in libraries:
            classification = libraries[identity]
            row['sdk'] = {'alias': classification['alias'], 'reason': classification['reason'], 'boundary': 'Reviewed occurrence only; verify native API and caller ABI'}
        if identity in scopes:
            row['scope'] = scopes[identity]
        if identity in candidates and not bindings[identity]['decompiled']:
            row['candidate_source'] = candidates[identity]
        if mode == 'operand_templates':
            row.update(template=template_metadata[unit], evidence=config['analysis']['function_queue_templates']['path'],
                       grouping='Verified address, direct-call and scalar-constant specialization; no acceptance transfer')
            row['task'] = 'Audit the representative once; instantiate recorded substitutions and validate each image ABI/data/residency context before binding complete bodies'
        selected.append(row)
    selected.sort(key=lambda row: row['_cursor'])
    next_cursor = selected[limit - 1]['_cursor'] if len(selected) > limit else None
    for row in selected:
        row.pop('_cursor')
    return {'available': True, 'grouping': mode, 'counts': counts,
            'image_qualified_counts': occurrence_counts, 'functions': selected[:limit], 'next': next_cursor}


def function_summary(root, config, image, address):
    original = original_function(root, config, image, address)
    db = connect(root, config)
    saved = None
    if db:
        with closing(db):
            row = db.execute('SELECT * FROM translations WHERE image=? AND address=?', (image, address)).fetchone()
            saved = dict(row) if row else None
    result = translation_state(root, original, saved)
    if config.get('analysis', {}).get('function_queue') == 'operand_templates':
        with closing(sqlite3.connect(analysis_path(root, config).as_uri() + '?mode=ro', uri=True)) as db:
            row = db.execute('SELECT t.id,t.evidence FROM function_template_members m JOIN function_templates t ON t.id=m.template_id WHERE m.image=? AND m.address=?', (image, address)).fetchone()
            if row is None:
                raise ValueError('Operand-template occurrence missing; rebuild analysis')
            result['decompilation_template'] = {'id': row[0], **json.loads(row[1])}
    if not result['decompiled']:
        with closing(sqlite3.connect(analysis_path(root, config).as_uri() + '?mode=ro', uri=True)) as db:
            db.row_factory = sqlite3.Row
            candidate = source_candidates(db, image, address).get((image, address))
        if candidate:
            result['candidate_source'] = candidate
    return result


def ledger_document(root):
    """Adapt SQL source bindings to existing strict/index contracts without writing JSON"""
    root = Path(root).resolve()
    config = load_project(root)[1] if (root / 'xport-project.json').is_file() else {}
    path = root / 'status/translation-ledger.json'
    document = json.loads(path.read_text(encoding='utf-8-sig')) if path.is_file() else {'version': 1, 'entries': []}
    entries = {(r['image'], r['address']): r for r in document['entries']}
    from function_entry_bindings import reviewed_entries
    native_entries = reviewed_entries(root, config)
    if len(entries) != len(document['entries']):
        raise ValueError('Duplicate historical ledger identity')
    db = connect(root, config)
    if db:
        with closing(db):
            for row in db.execute('SELECT * FROM translations ORDER BY image,address'):
                key = (row['image'], row['address'])
                refs = [r[0].split('#', 1)[0] for r in db.execute(
                    'SELECT path FROM translation_evidence WHERE image=? AND address=? ORDER BY path', key)
                    if not r[0].startswith('@xport/')]
                entry = dict(entries.get(key, {}), image=row['image'], address=row['address'],
                             sha256=row['original_sha256'], source=row['source'],
                             semantic_name=row['symbol'], symbol=row['symbol'], status='WIP', evidence=refs)
                entry.pop('native_retirement', None)
                entry.pop('link_only_stub', None)
                if key in native_entries:
                    binding = native_entries[key]
                    if row['original_sha256'] != binding['original_sha256']:
                        raise ValueError('Changed registered original entry identity')
                    entry['native_entry'] = binding['native_entry']
                    entry['entry_binding_evidence'] = binding['evidence']
                entries[key] = entry
    return {**document, 'entries': list(entries.values())}


def ledger_bytes(root):
    document = ledger_document(root)
    path = Path(root) / 'status/translation-ledger.json'
    if path.is_file():
        raw = path.read_bytes()
        if json.loads(raw.decode('utf-8-sig')) == document:
            return raw
    return json.dumps(document, sort_keys=True).encode('utf-8')


def main():
    root, config = load_project()
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_subparsers(dest='action', required=True)
    actions.add_parser('init')
    for action in ('show', 'set', 'queue', 'decompile'):
        p = actions.add_parser(action)
        p.add_argument('--image')
        if action != 'queue':
            p.add_argument('--address', type=lambda v: int(v, 16))
        if action in ('show', 'set'):
            p.add_argument('--stage'); p.add_argument('--subsystem'); p.add_argument('--key')
        if action in ('show', 'queue'):
            p.add_argument('--limit', type=int, default=8); p.add_argument('--after')
        if action == 'show':
            p.add_argument('--include-closed', action='store_true')
        if action == 'set':
            p.add_argument('--kind', choices=('task', 'finding', 'check', 'question'), required=True)
            p.add_argument('--summary', required=True); p.add_argument('--scope', default='')
            p.add_argument('--configuration', default=''); p.add_argument('--next-step', default='')
            p.add_argument('--closed', action='store_true'); p.add_argument('--evidence', action='append', default=[])
        if action == 'queue':
            p.add_argument('--state', choices=('uncovered', 'decompiled', 'sdk'))
        if action == 'decompile':
            p.add_argument('--source', required=True); p.add_argument('--symbol', required=True)
            p.add_argument('--evidence', action='append', required=True)
            p.add_argument('--complete', action='store_true')
    args = vars(parser.parse_args()); action = args.pop('action')
    if action == 'init':
        with closing(connect(root, config, True)) as db:
            db.commit()
        result = {'database': str(database_path(root, config))}
    elif action == 'show':
        args['after'] = args['after'] or ''
        result = show(root, config, **args)
    elif action == 'set':
        for name in ('stage', 'subsystem', 'image'):
            args[name] = args[name] or ''
        if not args['key']:
            parser.error('set requires --key')
        result = put(root, config, **args)
    elif action == 'queue':
        result = queue(root, config, **args)
    else:
        if not args['image'] or args['address'] is None:
            parser.error('decompile requires --image and --address')
        result = decompile(root, config, **args)
    print(json.dumps(result, ensure_ascii=False, separators=(',', ':')))


if __name__ == '__main__':
    main()

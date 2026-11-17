"""Cache scoped checked findings with explicit inputs and transitive invalidation."""

from contextlib import closing
import hashlib
import json
from pathlib import Path
import sqlite3

from source_index import functions, PARSER_VERSION


def digest(value):
    return hashlib.sha256(value).hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=False)


class SummaryStore:
    """Freshness is conditional on declared inputs, never a semantic proof."""

    def __init__(self, db, root):
        self.db, self.root = db, Path(root).resolve()
        self.observed = {}
        self.parsed_files = 0
        db.executescript('''
            CREATE TABLE IF NOT EXISTS checked_summaries (
                key TEXT PRIMARY KEY, revision TEXT NOT NULL, record TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS checked_summary_bodies (
                path TEXT PRIMARY KEY, content_sha256 TEXT NOT NULL,
                parser TEXT NOT NULL, bodies TEXT NOT NULL);
        ''')

    def fingerprint(self, item):
        path = (self.root / item['path']).resolve()
        if item.get('tree'):
            if item.get('symbol') or not path.is_dir():
                raise ValueError('tree input requires an existing source directory without a symbol')
            rows = []
            for source in sorted(path.rglob('*')):
                if source.is_file() and source.suffix.lower() in ('.c', '.cpp', '.h', '.hpp', '.inc', '.asm', '.s', '.def'):
                    rows.append([source.relative_to(path).as_posix(), self.file_data(source)[1]])
            return digest(canonical(rows).encode())
        data, content_hash = self.file_data(path)
        if not item.get('symbol'):
            return content_hash
        parser = PARSER_VERSION + ':' + digest(Path(__file__).with_name('source_index.py').read_bytes())
        cached = self.db.execute('SELECT content_sha256, parser, bodies FROM checked_summary_bodies WHERE path=?',
                                 (str(path),)).fetchone()
        if cached and cached[:2] == (content_hash, parser):
            bodies = json.loads(cached[2])
        else:
            text = data.decode('utf-8-sig').replace('\r\n', '\n').replace('\r', '\n')
            bodies = {}
            for row in functions(text, str(path)):
                bodies.setdefault(row['name'], []).append(row['sha256'])
            self.db.execute('INSERT OR REPLACE INTO checked_summary_bodies VALUES(?,?,?,?)',
                            (str(path), content_hash, parser, canonical(bodies)))
            self.parsed_files += 1
        matches = bodies.get(item['symbol'], [])
        if len(matches) != 1:
            raise ValueError(f"missing or ambiguous function {item['symbol']} in {path}")
        return matches[0]

    def file_data(self, path):
        if path not in self.observed:
            data = path.read_bytes()
            self.observed[path] = data, digest(data)
        return self.observed[path]

    def bind(self, path, symbol=None):
        item = {'path': str(path)}
        if symbol:
            item['symbol'] = symbol
        return dict(item, sha256=self.fingerprint(item))

    def bind_tree(self, path):
        item = {'path': str(path), 'tree': True}
        return dict(item, sha256=self.fingerprint(item))

    def get(self, key, stack=(), memo=None):
        memo = {} if memo is None else memo
        if key in memo:
            return memo[key]
        if key in stack:
            return {'key': key, 'state': 'stale', 'reasons': ['dependency cycle']}
        row = self.db.execute('SELECT revision, record FROM checked_summaries WHERE key=?', (key,)).fetchone()
        if row is None:
            return {'key': key, 'state': 'missing', 'reasons': ['no checked summary']}
        revision, payload = row
        record = json.loads(payload)
        reasons = []
        for item in record['inputs']:
            try:
                if self.fingerprint(item) != item['sha256']:
                    reasons.append({'input': item, 'reason': 'content changed'})
            except (OSError, ValueError, UnicodeError) as error:
                reasons.append({'input': item, 'reason': str(error)})
        for dep, expected in record['dependencies'].items():
            current = self.get(dep, (*stack, key), memo)
            if current['state'] != 'current' or current.get('revision') != expected:
                reasons.append({'dependency': dep, 'state': current['state'],
                                'revision': current.get('revision'), 'reasons': current['reasons']})
        result = {'key': key, 'state': 'stale' if reasons else 'current', 'revision': revision,
                  'record': record, 'reasons': reasons}
        memo[key] = result
        return result

    def put_many(self, records):
        """Atomically accept only currently bound summaries and dependency revisions."""
        if not isinstance(records, list) or not records or not all(isinstance(r, dict) for r in records):
            raise ValueError('empty summary batch')
        if not all(isinstance(r.get('key'), str) for r in records):
            raise ValueError('summary keys must be strings')
        pending = {record['key']: record for record in records}
        if len(pending) != len(records):
            raise ValueError('duplicate summary key')
        accepted, visiting = {}, set()

        def accept(key):
            if key in accepted:
                return accepted[key]
            if key in visiting:
                raise ValueError('summary dependency cycle: ' + key)
            visiting.add(key)
            raw = pending[key]
            for name in ('key', 'scope', 'claims', 'assumptions', 'limits', 'inputs', 'dependencies'):
                if name not in raw:
                    raise ValueError('missing summary field: ' + name)
            if not all(isinstance(raw[n], str) and raw[n].strip() for n in ('key', 'scope', 'limits')):
                raise ValueError('key, scope and limits must be nonempty strings')
            if (not isinstance(raw['inputs'], list) or not raw['inputs']
                    or not isinstance(raw['claims'], list) or not raw['claims']
                    or not all(isinstance(c, str) and c.strip() for c in raw['claims'])
                    or not isinstance(raw['assumptions'], (list, dict))
                    or not isinstance(raw['dependencies'], (list, dict))
                    or not all(isinstance(dep, str) and dep for dep in raw['dependencies'])):
                raise ValueError('summaries require claims and bound inputs')
            for item in raw['inputs']:
                if not isinstance(item, dict) or not all(isinstance(item.get(n), str) for n in ('path', 'sha256')):
                    raise ValueError('summary inputs require a path and sha256')
                if self.fingerprint(item) != item['sha256']:
                    raise ValueError('stale summary input: ' + item['path'])
            dependencies = {}
            for dep in raw['dependencies']:
                current = accept(dep) if dep in pending else self.get(dep)
                if current['state'] != 'current':
                    raise ValueError('stale or missing dependency: ' + dep)
                if isinstance(raw['dependencies'], dict) and raw['dependencies'][dep] != current['revision']:
                    raise ValueError('dependency revision changed: ' + dep)
                dependencies[dep] = current['revision']
            record = dict(raw, dependencies=dependencies)
            revision = digest(canonical(record).encode())
            result = {'key': key, 'state': 'current', 'revision': revision, 'record': record, 'reasons': []}
            accepted[key] = result
            visiting.remove(key)
            return result

        self.db.execute('SAVEPOINT checked_summary_batch')
        try:
            for key in pending:
                accept(key)
            for result in accepted.values():
                self.db.execute('INSERT OR REPLACE INTO checked_summaries VALUES(?,?,?)',
                                (result['key'], result['revision'], canonical(result['record'])))
            self.db.execute('RELEASE checked_summary_batch')
        except Exception:
            self.db.execute('ROLLBACK TO checked_summary_batch')
            self.db.execute('RELEASE checked_summary_batch')
            raise
        return [accepted[key] for key in pending]


def analysis_worklist(store, jobs):
    """Schedule declared checks without promoting findings into semantic acceptance."""
    if not isinstance(jobs, list):
        raise ValueError('analysis_worklist must be a job array')
    seen, memo, rows = set(), {}, []
    for job in jobs:
        if not isinstance(job, dict):
            raise ValueError('each analysis job must be an object')
        key, result = job.get('key'), job.get('result')
        requires = job.get('requires', [])
        disposition = job.get('disposition', 'finding')
        priority = job.get('priority', 0)
        if (not isinstance(key, str) or not key.strip() or key in seen
                or not isinstance(result, str) or not result.strip()
                or not isinstance(requires, list)
                or any(not isinstance(dep, str) or not dep.strip() for dep in requires)
                or len(set(requires)) != len(requires) or result in requires
                or disposition not in ('finding', 'open-frontier')
                or type(priority) is not int):
            raise ValueError('invalid or duplicate analysis job: ' + str(key))
        seen.add(key)
        checked = store.get(result, memo=memo)
        prerequisites = [store.get(dep, memo=memo) for dep in requires]
        waiting = [dep['key'] for dep in prerequisites if dep['state'] != 'current']
        if waiting:
            state = 'waiting'
        elif checked['state'] == 'current':
            state = 'deferred-unchanged' if disposition == 'open-frontier' else 'reuse-finding'
        else:
            state = 'ready'
        record = checked.get('record', {})
        rows.append({'key': key, 'result': result, 'state': state, 'priority': priority,
                     'result_state': checked['state'], 'revision': checked.get('revision'),
                     'waiting_on': waiting, 'reasons': checked['reasons'],
                     'prerequisites': [{k: dep.get(k) for k in ('key', 'state', 'revision', 'reasons')}
                                       for dep in prerequisites],
                     'scope': record.get('scope'), 'claims': record.get('claims', []),
                     'assumptions': record.get('assumptions'), 'limits': record.get('limits'),
                     'next_step': job.get('next_step', '')})
    rows.sort(key=lambda row: (-row['priority'], row['key']))
    ready = [row['key'] for row in rows if row['state'] == 'ready']
    return {'jobs': rows, 'ready': ready, 'next': ready[0] if ready else None,
            'counts': {state: sum(row['state'] == state for row in rows) for state in
                       ('ready', 'waiting', 'reuse-finding', 'deferred-unchanged')},
            'limits': 'Scheduling only for the declared jobs and summary inputs. A current finding is not '
                      'ownership, semantic or runtime acceptance. An unchanged open frontier remains unresolved. '
                      'Missing jobs and undeclared dependencies are not discovered. No check is executed.'}


def cached_check(root, key, inputs, contract, run, validate, summaries=()):
    """Reuse validated checker output only for identical bound inputs and contract."""
    cache = Path(root) / 'status/refactoring/memory-access.sqlite'
    cache.parent.mkdir(parents=True, exist_ok=True)
    with closing(sqlite3.connect(cache, timeout=20)) as db:
        store = SummaryStore(db, root)
        registered = store.put_many(summaries) if summaries else []
        bound = [store.bind(**item) for item in inputs]
        current = store.get(key)
        expected = dict(contract, inputs=bound,
                        dependencies={item['key']: item['revision'] for item in registered})
        hit = current['state'] == 'current' and all(
            current['record'].get(k) == v for k, v in expected.items())
        if hit:
            output = current['record']['output']
        else:
            output = run()
        validate(output)
        if not hit:
            store.put_many([dict(expected, key=key, output=output)])
        db.commit()
        return output, {'hit': hit, 'parsed_files': store.parsed_files,
                        'previous_state': current['state']}

"""Index exact mismatch symptoms without equating them with root causes"""
import argparse
import hashlib
import json
from pathlib import Path

from trace_cache import canonical, digest


def migrate(db):
    db.executescript('''
    CREATE TABLE IF NOT EXISTS evidence_schema(version INTEGER PRIMARY KEY);
    CREATE TABLE IF NOT EXISTS symptom_groups(
      id TEXT PRIMARY KEY, identity TEXT NOT NULL);
    CREATE TABLE IF NOT EXISTS symptom_occurrences(
      job TEXT PRIMARY KEY REFERENCES jobs(id),
      symptom TEXT NOT NULL REFERENCES symptom_groups(id),
      first_difference TEXT NOT NULL, report TEXT, report_sha256 TEXT);
    CREATE INDEX IF NOT EXISTS occurrences_by_symptom ON symptom_occurrences(symptom);
    INSERT OR IGNORE INTO evidence_schema VALUES(1);
    CREATE TABLE IF NOT EXISTS function_coverage(
      job TEXT NOT NULL REFERENCES jobs(id), image TEXT NOT NULL, address INTEGER NOT NULL,
      first_tick INTEGER NOT NULL, stage INTEGER NOT NULL, PRIMARY KEY(job,image,address));
    CREATE TABLE IF NOT EXISTS coverage_sources(
      job TEXT PRIMARY KEY REFERENCES jobs(id), path TEXT NOT NULL, sha256 TEXT NOT NULL,
      complete INTEGER NOT NULL CHECK(complete=0));
    ''')


def record_functions(db, job, result):
    path = result.get('artifact_paths', {}).get('native', {}).get('functions')
    if not path:
        return
    value = json.loads(Path(path).read_text())
    dependencies = result['manifest_identity']['dependencies']
    if (value.get('schema') != 'ff-first-use-v1' or value.get('complete') is not False or
        value.get('build_sha256') != dependencies.get('exe') or value.get('pad_sha256') != dependencies.get('__native_pad')):
        raise ValueError('Function coverage provenance differs')
    db.execute('INSERT INTO coverage_sources VALUES(?,?,?,0)', (job, path, digest(path)))
    for function in value['functions']:
        db.execute('INSERT INTO function_coverage VALUES(?,?,?,?,?)',
                   (job, function['image'], function['address'], function['first_tick'], function['stage']))


def record(db, job, result, report=None):
    if result.get('outcome') != 'mismatch':
        return None
    first = result.get('first_difference')
    if not isinstance(first, dict) or 'channel' not in first or 'tick' not in first:
        raise ValueError('Mismatch lacks channel/tick evidence')
    manifest = result.get('manifest_identity', {})
    original = manifest.get('original_identity') or {}
    # Unknown provenance must not group unrelated legacy jobs together
    capture = json.loads(json.dumps(original.get('capture', {})))
    capture.get('options', {}).pop('scheduled_segments', None)
    scope = ({'game': original['game'], 'emulator': original.get('emulator'),
              'settings': original.get('settings'), 'capture': capture}
             if original.get('game') else {'job': job})
    fields = ('channel', 'tick', 'region', 'slot', 'field', 'field_offset',
              'offset', 'packet_index', 'index', 'original', 'native',
              'original_byte', 'native_byte', 'original_count', 'native_count')
    identity = {'version': 1, 'scope': scope,
                'first_difference': {k: first[k] for k in fields if k in first}}
    encoded = canonical(identity).decode()
    key = hashlib.sha256(encoded.encode()).hexdigest()
    existing = db.execute('SELECT symptom FROM symptom_occurrences WHERE job=?', (job,)).fetchone()
    if existing and existing[0] != key:
        raise ValueError('Stored job evidence changed; preserve the previous attempt')
    db.execute('INSERT OR IGNORE INTO symptom_groups VALUES(?,?)', (key, encoded))
    db.execute('INSERT OR IGNORE INTO symptom_occurrences VALUES(?,?,?,?,?)',
               (job, key, json.dumps(first), str(report) if report else None,
                digest(report) if report else None))
    return key


def summary(db):
    return {'groups': [dict(row) for row in db.execute('''
      SELECT g.id, count(o.job) AS occurrences FROM symptom_groups g
      JOIN symptom_occurrences o ON o.symptom=g.id GROUP BY g.id ORDER BY g.id''')],
            'outcomes': {row[0]: row[1] for row in db.execute('SELECT status,count(*) FROM jobs GROUP BY status')},
            'scope': 'Exact first-divergence symptoms, not proven shared root causes'}


def export_coverage(db, path):
    runs = []
    for row in db.execute('SELECT id,name,status,spec,result FROM jobs ORDER BY started,id'):
        identity = json.loads(row['spec'])
        result = json.loads(row['result']) if row['result'] else {}
        spec = identity.get('spec', {})
        runs.append({'job': row['id'], 'name': row['name'], 'outcome': row['status'],
                     'native_build_sha256': identity.get('dependencies', {}).get('exe'),
                     'scenario_sha256': identity.get('dependencies', {}).get('__schedule'),
                     'start_tick': spec.get('start_tick'), 'end_tick': spec.get('end_tick'),
                     'channels': sorted(result.get('artifact_paths', {}).get('original', {})),
                     'capture_mode': result.get('capture_mode'),
                     'original_cache_hit': result.get('original_cache_hit'),
                     'artifacts': result.get('artifact_paths'),
                     'first_difference': result.get('first_difference')})
        runs[-1]['function_coverage'] = {'complete': False,
            'build_sha256': runs[-1]['native_build_sha256'], 'scenario_sha256': runs[-1]['scenario_sha256'],
            'functions': [dict(item) for item in db.execute('SELECT image,address,first_tick,stage FROM function_coverage WHERE job=? ORDER BY image,address', (row['id'],))]}
    document = {'schema': 'ff-run-coverage-v1', 'source': 'runs.sqlite', 'runs': runs,
                'scope': 'Recorded comparison coverage per build and scenario, not function DONE or current artifact revalidation'}
    path = Path(path)
    temporary = path.with_suffix(path.suffix+'.tmp')
    temporary.write_text(json.dumps(document, indent=2)+'\n', encoding='utf-8')
    temporary.replace(path)
    return document


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--database', type=Path, required=True)
    parser.add_argument('--index-existing', action='store_true')
    args = parser.parse_args()
    from xport_project import artifact_path
    args.database = artifact_path(args.database)
    from trace_runner import database
    with database(args.database) as db:
        if args.index_existing:
            for row in db.execute("SELECT id,result FROM jobs WHERE status='mismatch'").fetchall():
                record(db, row['id'], json.loads(row['result']))
        print(json.dumps(summary(db)))


if __name__ == '__main__':
    main()

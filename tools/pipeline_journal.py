"""Transactional stage journal with immutable request identities and bounded attempts"""
from contextlib import contextmanager
import hashlib
import json
from pathlib import Path
import sqlite3
import time


def identity(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def connect(path):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    db = sqlite3.connect(path, timeout=10)
    db.row_factory = sqlite3.Row
    db.execute('PRAGMA foreign_keys=ON')
    db.execute('PRAGMA journal_mode=WAL')
    version = db.execute('PRAGMA user_version').fetchone()[0]
    if version not in (0, 1):
        db.close()
        raise ValueError('Unsupported pipeline journal schema')
    if version == 0:
        db.executescript('''
        BEGIN IMMEDIATE;
        CREATE TABLE IF NOT EXISTS runs (
          id TEXT PRIMARY KEY, request TEXT NOT NULL, status TEXT NOT NULL,
          created REAL NOT NULL, updated REAL NOT NULL);
        CREATE TABLE IF NOT EXISTS steps (
          run_id TEXT NOT NULL REFERENCES runs(id), name TEXT NOT NULL,
          status TEXT NOT NULL, receipt TEXT, seconds REAL NOT NULL DEFAULT 0,
          updated REAL NOT NULL, PRIMARY KEY(run_id,name));
        CREATE TABLE IF NOT EXISTS budgets (
          run_id TEXT NOT NULL REFERENCES runs(id), name TEXT NOT NULL,
          attempts INTEGER NOT NULL, started REAL NOT NULL,
          max_attempts INTEGER NOT NULL, max_seconds REAL NOT NULL,
          PRIMARY KEY(run_id,name));
        CREATE TABLE IF NOT EXISTS events (
          id INTEGER PRIMARY KEY, run_id TEXT NOT NULL REFERENCES runs(id),
          name TEXT NOT NULL, status TEXT NOT NULL, time REAL NOT NULL, detail TEXT NOT NULL);
        PRAGMA user_version=1;
        COMMIT;
        ''')
    return db


@contextmanager
def transaction(db):
    db.execute('BEGIN IMMEDIATE')
    try:
        yield
        db.commit()
    except BaseException:
        db.rollback()
        raise


def ensure_run(db, request):
    key = identity(request)
    now = time.time()
    with transaction(db):
        db.execute('INSERT OR IGNORE INTO runs VALUES (?,?,?,?,?)',
                   (key, json.dumps(request, sort_keys=True), 'prepared', now, now))
        actual = json.loads(db.execute('SELECT request FROM runs WHERE id=?', (key,)).fetchone()[0])
        if actual != request:
            raise ValueError('Run identity collision')
    return key


def set_step(db, run_id, name, status, receipt=None, seconds=0):
    now = time.time()
    with transaction(db):
        previous = db.execute('SELECT status,receipt FROM steps WHERE run_id=? AND name=?', (run_id,name)).fetchone()
        encoded = json.dumps(receipt, sort_keys=True)
        changed = previous is None or previous['status'] != status or previous['receipt'] != encoded
        db.execute('INSERT INTO steps VALUES (?,?,?,?,?,?) ON CONFLICT(run_id,name) DO UPDATE SET '
                   'status=excluded.status, receipt=excluded.receipt, seconds=excluded.seconds, updated=excluded.updated',
                   (run_id,name,status,encoded,seconds,now))
        db.execute('UPDATE runs SET status=?,updated=? WHERE id=?', (status,now,run_id))
        if changed:
            db.execute('INSERT INTO events(run_id,name,status,time,detail) VALUES (?,?,?,?,?)',
                       (run_id,name,status,now,encoded))
    return changed


def step(db, run_id, name):
    row = db.execute('SELECT * FROM steps WHERE run_id=? AND name=?', (run_id,name)).fetchone()
    if row is None:
        return None
    result = dict(row)
    result['receipt'] = json.loads(result['receipt'])
    return result


def reserve_attempt(db, run_id, name, max_attempts, max_seconds, now=None):
    if type(max_attempts) is not int or max_attempts < 1 or not 0 < max_seconds <= 3600:
        raise ValueError('Invalid anchor search budget')
    now = time.time() if now is None else now
    with transaction(db):
        db.execute('INSERT OR IGNORE INTO budgets VALUES (?,?,?,?,?,?)',
                   (run_id,name,0,now,max_attempts,max_seconds))
        row = db.execute('SELECT * FROM budgets WHERE run_id=? AND name=?', (run_id,name)).fetchone()
        if row['max_attempts'] != max_attempts or row['max_seconds'] != max_seconds:
            raise ValueError('Budget changed inside existing run')
        remaining = max_seconds - max(0, now-row['started'])
        if row['attempts'] >= max_attempts or remaining <= 0:
            return dict(allowed=False, outcome='ANCHOR_UNSUPPORTED', attempts=row['attempts'], remaining_seconds=max(0,remaining))
        db.execute('UPDATE budgets SET attempts=attempts+1 WHERE run_id=? AND name=?', (run_id,name))
        return dict(allowed=True, attempts=row['attempts']+1, remaining_seconds=remaining)

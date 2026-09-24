"""Run manifest jobs with persistent outcomes and conservative crash recovery"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sqlite3
import subprocess
import sys
import time

from xport_project import project_root, activate_adapters, implementation_path, load_project
activate_adapters()

from trace_bundle import compare_bundles, read_json, validate_bundle, cache_validator
from trace_cache import canonical, digest, fingerprint, lookup, publish
from trace_schedule import pad_bytes
import trace_evidence
from trace_worker import process_identity, alive
from trace_comparison import validate as validate_capture, compare_capture
from xport_process import normalized_environment


class CaptureFailure(RuntimeError):
    def __init__(self, phase, code, spec):
        super().__init__('Capture failed: '+phase+'; exit '+str(code))
        declared = spec.get('failure_exit_codes', {}).get(phase, {}).get(str(code))
        allowed = {'game_failure', 'native_crash', 'infrastructure_error', 'unclassified_failure'}
        self.outcome = declared if declared in allowed else 'infrastructure_error'


def database(path):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    db = sqlite3.connect(path, timeout=30)
    db.row_factory = sqlite3.Row
    db.execute('PRAGMA foreign_keys=ON')
    db.executescript('''
    CREATE TABLE IF NOT EXISTS runner_meta(version INTEGER PRIMARY KEY CHECK(version=1));
    INSERT OR IGNORE INTO runner_meta VALUES(1);
    CREATE TABLE IF NOT EXISTS jobs(
      id TEXT PRIMARY KEY, name TEXT NOT NULL, spec TEXT NOT NULL,
      status TEXT NOT NULL, phase TEXT, pid INTEGER, started REAL, finished REAL,
      result TEXT, artifact_hashes TEXT);
    CREATE TABLE IF NOT EXISTS events(
      id INTEGER PRIMARY KEY, job TEXT NOT NULL REFERENCES jobs(id),
      at REAL NOT NULL, phase TEXT NOT NULL, detail TEXT NOT NULL);
    ''')
    trace_evidence.migrate(db)
    if 'owner' not in {row[1] for row in db.execute('PRAGMA table_info(jobs)')}:
        db.execute('ALTER TABLE jobs ADD COLUMN owner TEXT')
        db.commit()
    return db


def event(db, job, phase, detail):
    db.execute('INSERT INTO events(job,at,phase,detail) VALUES(?,?,?,?)',
               (job, time.time(), phase, json.dumps(detail)))
    db.commit()


def refresh_reports(path):
    root = project_root()
    if Path(path).resolve() == root/'status/controls/automation/runs.sqlite':
        subprocess.run([sys.executable, str(Path(__file__).with_name('render_progress.py'))], cwd=root, check=True,
                       stdout=subprocess.DEVNULL)


def paths_for(spec, root):
    return {side: {name: (root/path).resolve() for name, path in spec[side].items()}
            for side in ('original', 'native')}


def artifact_hashes(paths):
    return {side: {name: digest(path) for name, path in entries.items()}
            for side, entries in paths.items()}


def original_identity(config, root):
    value = {group: fingerprint({name: root/path for name, path in files.items()})
             for group, files in config['files'].items()}
    # Hash shared implementations even when a legacy manifest names entry-point wrappers
    context_root, project = load_project()
    value['settings']['__project_configuration'] = digest(context_root/'xport-project.json')
    profile = project.get('duckstation', {}).get('trace_profile')
    if profile:
        value['settings']['__trace_profile'] = digest(context_root/profile)
    for module in ('trace_internal', 'trace_vblank', 'trace_user_input', 'trace_layout', 'trace_profile', 'gdb_remote', 'shared_ram'):
        value['collector']['__shared_'+module] = digest(implementation_path(module))
    value['capture'] = json.loads(json.dumps(config['capture']))
    if 'schedule' in config['files']['scenario']:
        value['capture']['options']['scheduled_segments'] = read_json(root/config['files']['scenario']['schedule'])
    return value


def execute_job(db, name, spec, root, output, compare_only=False, native_only=False):
    if not re.fullmatch('[a-zA-Z0-9_-]{1,80}', name):
        raise ValueError('Invalid job name')
    dependencies = {key: digest(root/path) for key, path in spec.get('dependencies', {}).items()}
    schedule = read_json(root/spec['input_schedule']) if 'input_schedule' in spec else None
    if schedule is not None:
        dependencies['__schedule'] = digest(root/spec['input_schedule'])
    if 'native_pad' in spec:
        dependencies['__native_pad'] = digest(root/spec['native_pad'])
    cache_config = spec.get('original_cache')
    cache_identity = original_identity(cache_config, root) if cache_config else None
    context_root, context_config = load_project()
    context_files = {'project_configuration': context_root/'xport-project.json'}
    if context_config.get('duckstation', {}).get('trace_profile'):
        context_files['trace_profile'] = context_root/context_config['duckstation']['trace_profile']
    context_hashes = fingerprint(context_files)
    identity = {'name': name, 'spec': spec, 'dependencies': dependencies,
                'compare_only': compare_only, 'native_only': native_only, 'root': str(root),
                'original_identity': cache_identity, 'project_context': context_hashes,
                'tool_sha256': {name: digest(implementation_path(name)) for name in
                               ('trace_runner.py', 'trace_bundle.py', 'trace_actor_diff.py',
                                'gpu_packet_semantics.py', 'trace_cache.py', 'trace_schedule.py', 'trace_evidence.py',
                                'trace_worker.py', 'trace_comparison.py', 'trace_phases.py', 'trace_phase_gpu.py', 'trace_layout.py', 'trace_profile.py')}}
    key = hashlib.sha256(canonical(identity)).hexdigest()
    db.execute('INSERT OR IGNORE INTO jobs(id,name,spec,status) VALUES(?,?,?,?)',
               (key, name, canonical(identity).decode(), 'queued'))
    db.commit()
    row = db.execute('SELECT * FROM jobs WHERE id=?', (key,)).fetchone()
    paths = paths_for(spec, root)
    if row['status'] in ('match', 'mismatch'):
        try:
            saved_paths = json.loads(row['result']).get('artifact_paths', paths)
            unchanged = json.loads(row['artifact_hashes']) == artifact_hashes(saved_paths)
        except OSError:
            unchanged = False
        if unchanged:
            return {'name': name, 'job': key, 'outcome': row['status'], 'reused_result': True}
        return {'name': name, 'job': key, 'outcome': 'needs_attention',
                'error': 'Completed artifacts changed; use a new job name and preserve history'}
    recovering = False
    if row['status'] == 'running' and row['owner']:
        try:
            recovering = not alive(json.loads(row['owner']))
        except OSError:
            recovering = False
    if row['status'] != 'queued' and not recovering:
        return {'name': name, 'job': key, 'outcome': 'needs_attention',
                'phase': row['phase'], 'pid': row['pid'],
                'error': 'Prior attempt is unresolved; inspect its process and events before retrying'}
    owner = json.dumps(process_identity(os.getpid()))
    if recovering:
        changed = db.execute("UPDATE jobs SET owner=? WHERE id=? AND status='running' AND owner=?",
                             (owner, key, row['owner'])).rowcount
    else:
        changed = db.execute("UPDATE jobs SET status='running',started=?,phase='preflight',owner=? WHERE id=? AND status='queued'",
                             (time.time(), owner, key)).rowcount
    db.commit()
    if changed != 1:
        return {'name': name, 'job': key, 'outcome': 'needs_attention', 'error': 'Job already claimed'}
    directory = output/key
    directory.mkdir(parents=True, exist_ok=True)
    result = None
    try:
        if recovering:
            for phase in ('original', 'native'):
                ticket_path = directory/(phase+'-ticket.json')
                if not ticket_path.exists():
                    continue
                receipt_path = directory/(phase+'-receipt.json')
                receipt = read_json(receipt_path) if receipt_path.exists() else None
                if receipt is None or alive(receipt.get('worker')) or alive(receipt.get('child')):
                    db.execute('UPDATE jobs SET owner=? WHERE id=?', (row['owner'], key))
                    db.commit()
                    return {'name': name, 'job': key, 'outcome': 'needs_attention',
                            'error': 'Existing worker is live or launch is unresolved; never restart it'}
                ticket = read_json(ticket_path)
                if (receipt.get('ticket_sha256') != digest(ticket_path) or ticket.get('job') != key or
                    ticket.get('command') != spec.get('commands', {}).get(phase) or ticket.get('cwd') != str(root)):
                    raise ValueError('Recovered worker identity differs')
                if receipt.get('status') != 'finished':
                    raise RuntimeError('Recovered worker failed: '+phase)
                if receipt.get('exit_code') != 0:
                    raise CaptureFailure(phase, receipt.get('exit_code'), spec)
                if receipt.get('artifact_hashes') != {name: digest(path) for name, path in ticket['outputs'].items()}:
                    raise ValueError('Completed capture artifacts changed before recovery')
        if schedule is not None and 'native_pad' in spec:
            if (root/spec['native_pad']).read_bytes() != pad_bytes(schedule):
                raise ValueError('Native pad bytes differ from manifest schedule')
        if not compare_only:
            commands = spec.get('commands', {})
            phases = ('native',) if native_only else ('original', 'native')
            cache_hit = False
            if cache_config and not native_only:
                if (cache_identity['capture']['start_tick'], cache_identity['capture']['end_tick']) != (spec['start_tick'], spec['end_tick']):
                    raise ValueError('Cache range differs from job range')
                hit = lookup(root/cache_config['directory'], cache_identity, cache_validator)
                cache_hit = hit['hit']
                event(db, key, 'original_cache', {'hit': cache_hit, 'key': hit['key']})
                if cache_hit:
                    paths['original'] = hit['artifacts']
                    phases = ('native',)
            if not set(phases) <= commands.keys():
                raise ValueError('Live jobs require capture commands for requested phases')
            if not dependencies:
                raise ValueError('Live jobs require declared executable/scenario dependencies')
            reference_hashes = None
            if native_only:
                validate_capture(spec, paths['original'])
                reference_hashes = {name: digest(path) for name, path in paths['original'].items()}
                event(db, key, 'existing_reference', reference_hashes)
            for phase in phases:
                ticket_path = directory/(phase+'-ticket.json')
                receipt_path = directory/(phase+'-receipt.json')
                if recovering and ticket_path.exists():
                    if not receipt_path.exists():
                        return {'name': name, 'job': key, 'outcome': 'needs_attention',
                                'error': 'Worker launch is unresolved; no completion receipt'}
                    receipt = read_json(receipt_path)
                    if receipt.get('ticket_sha256') != digest(ticket_path):
                        raise ValueError('Worker receipt identity differs')
                    if alive(receipt.get('worker')) or alive(receipt.get('child')):
                        db.execute('UPDATE jobs SET owner=? WHERE id=?', (row['owner'], key))
                        db.commit()
                        return {'name': name, 'job': key, 'outcome': 'needs_attention',
                                'error': 'Existing capture is still running; observe it without restarting'}
                    if receipt.get('status') != 'finished':
                        raise RuntimeError('Capture worker died without a successful completion receipt')
                    if receipt['exit_code']:
                        raise RuntimeError('Recovered capture failed: '+phase)
                    validate_capture(spec, paths[phase])
                    event(db, key, 'recovered_capture', {'phase': phase, 'receipt': str(receipt_path)})
                    if phase == 'original' and cache_config:
                        publish(root/cache_config['directory'], cache_identity, paths['original'],
                                cache_validator, lambda: original_identity(cache_config, root))
                    continue
                # Refuse outputs left by an earlier capture rather than silently overwrite
                if any(path.exists() for path in paths[phase].values()):
                    raise ValueError('Capture outputs already exist: '+phase)
                command = commands[phase]
                if not isinstance(command, list) or not command or any(not isinstance(s, str) for s in command):
                    raise ValueError('Commands must be explicit argv lists')
                env = normalized_environment(updates={k: str(v) for k, v in spec.get('environment', {}).items()})
                env.update(FF_AUDIO_OUTPUT='0', FF_RASTERIZE='0', FF_VERIFY_VRAM='0')
                log = directory/(phase+'.log')
                db.execute('UPDATE jobs SET phase=? WHERE id=?', (phase, key))
                db.commit()
                event(db, key, 'launch_intent', {'phase': phase, 'argv': command, 'cwd': str(root), 'log': str(log)})
                with log.open('xb') as stream:
                    with ticket_path.open('x', encoding='utf-8') as ticket:
                        json.dump({'job': key, 'phase': phase, 'command': command, 'cwd': str(root),
                                   'outputs': {name: str(path) for name, path in paths[phase].items()}}, ticket)
                    worker = [sys.executable, str(Path(__file__).parent/'trace_worker.py'),
                              '--ticket', str(ticket_path), '--receipt', str(receipt_path)]
                    proc = subprocess.Popen(worker, cwd=root, env=env, stdout=stream,
                                            stderr=subprocess.STDOUT, shell=False,
                                            creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
                    db.execute('UPDATE jobs SET pid=? WHERE id=?', (proc.pid, key))
                    db.commit()
                    event(db, key, 'launched', {'phase': phase, 'pid': proc.pid})
                    code = proc.wait()
                event(db, key, 'exited', {'phase': phase, 'pid': proc.pid, 'exit_code': code})
                db.execute('UPDATE jobs SET pid=NULL WHERE id=?', (key,))
                db.commit()
                if code:
                    raise CaptureFailure(phase, code, spec)
                if phase == 'original' and cache_config:
                    publish(root/cache_config['directory'], cache_identity, paths['original'],
                            cache_validator, lambda: original_identity(cache_config, root))
            if reference_hashes is not None and reference_hashes != {name: digest(path) for name, path in paths['original'].items()}:
                raise ValueError('Existing reference changed during native capture')
        current_dependencies = {key: digest(root/path) for key, path in spec.get('dependencies', {}).items()}
        for field, dependency_key in [('input_schedule', '__schedule'), ('native_pad', '__native_pad')]:
            if field in spec:
                current_dependencies[dependency_key] = digest(root/spec[field])
        if dependencies != current_dependencies or fingerprint(context_files) != context_hashes:
            raise ValueError('Inputs changed while capturing')
        db.execute("UPDATE jobs SET phase='compare' WHERE id=?", (key,))
        db.commit()
        result = compare_capture(spec, paths['original'], paths['native'], schedule=schedule)
        result['manifest_identity'] = identity
        result['artifact_paths'] = {side: {name: str(path) for name, path in entries.items()} for side, entries in paths.items()}
        result['original_cache_hit'] = cache_hit if not compare_only else False
        result['capture_mode'] = 'existing_artifacts' if compare_only else ('native_with_existing_reference' if native_only else 'subprocess_capture')
        hashes = artifact_hashes(paths) if result['outcome'] != 'invalid_trace' else None
        report = directory/'comparison.json'
        report.write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
        db.execute('UPDATE jobs SET status=?,finished=?,result=?,artifact_hashes=? WHERE id=?',
                   (result['outcome'], time.time(), json.dumps(result), json.dumps(hashes), key))
        if result.get('first_difference'):
            from trace_diagnostic_packet import packet
            diagnostic = packet(report)
            (directory/'diagnostic-packet.json').write_text(json.dumps(diagnostic, indent=2)+'\n')
        trace_evidence.record(db, key, result, report)
        trace_evidence.record_functions(db, key, result)
        db.commit()
        event(db, key, 'finished', {'outcome': result['outcome'], 'report': str(report)})
        return {'name': name, 'job': key, 'outcome': result['outcome'],
                'first_difference': result.get('first_difference'), 'report': str(report)}
    except (OSError, ValueError, RuntimeError, KeyError, TypeError) as error:
        outcome = error.outcome if isinstance(error, CaptureFailure) else 'infrastructure_error'
        db.execute("UPDATE jobs SET status=?,finished=?,result=? WHERE id=?",
                   (outcome, time.time(), json.dumps({'error': str(error)}), key))
        db.commit()
        event(db, key, outcome, {'error': str(error)})
        return {'name': name, 'job': key, 'outcome': outcome, 'error': str(error)}


def main():
    raise SystemExit('trace_runner is an internal capture library; use trace_stage_run')


if __name__ == '__main__':
    raise SystemExit(main())

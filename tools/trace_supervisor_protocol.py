"""Versioned converge state, attention packets and repair submissions"""
import argparse
import hashlib
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import time

from trace_worker import write_receipt
from xport_project import artifact_path, load_project


STATES = ('STARTING', 'VALIDATING', 'REPLAYING', 'ATTENTION_REQUIRED',
          'VERIFYING_REPAIR', 'MATCH', 'FINALIZING', 'COMPLETE', 'FAILED')
ATTENTION_KINDS = ('todo_branch', 'ambiguous_semantics', 'missing_observation',
                   'unsupported_instruction', 'infrastructure_failure',
                   'foreign_source_change', 'validation_failure')
ALLOWED_TRANSITIONS = {
    'STARTING': {'VALIDATING', 'FAILED'},
    'VALIDATING': {'REPLAYING', 'ATTENTION_REQUIRED', 'FAILED'},
    'REPLAYING': {'VERIFYING_REPAIR', 'ATTENTION_REQUIRED', 'MATCH', 'FAILED'},
    'ATTENTION_REQUIRED': {'STARTING', 'VERIFYING_REPAIR', 'FAILED'},
    'VERIFYING_REPAIR': {'STARTING', 'REPLAYING', 'ATTENTION_REQUIRED', 'FAILED'},
    'MATCH': {'FINALIZING', 'FAILED'},
    'FINALIZING': {'COMPLETE', 'ATTENTION_REQUIRED', 'FAILED'},
    'COMPLETE': set(),
    'FAILED': {'STARTING'},
}


def digest(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def canonical(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def address_number(value):
    return value if isinstance(value, int) else int(str(value), 0)


def source_snapshot(root):
    root = Path(root)
    files = {}
    for path in sorted((root / 'src').rglob('*')):
        if path.is_file() and path.suffix.lower() in ('.c', '.h'):
            files[path.relative_to(root).as_posix()] = digest(path)
    return dict(identity=canonical(files), files=files)


def ledger_rows(root):
    path = Path(root) / 'status/translation-ledger.json'
    if not path.is_file():
        return {}
    ledger = json.loads(path.read_text(encoding='utf-8-sig'))
    return {(row['image'], address_number(row['address'])): row for row in ledger.get('entries', [])}


def transition(state, status, **fields):
    if status not in STATES:
        raise ValueError('Unknown converge supervisor state: ' + status)
    visible = dict(status=status)
    visible.update({key: value for key, value in fields.items() if value is not None})
    current = state.get('status')
    if current and current != status and status not in ALLOWED_TRANSITIONS[current]:
        raise ValueError('Illegal converge supervisor transition: ' + current + ' -> ' + status)
    prior = {key: state.get(key) for key in visible}
    if prior == visible:
        return False
    state.update(visible)
    state['revision'] = int(state.get('revision', 0)) + 1
    state['transitioned_at'] = time.time()
    state.setdefault('transitions', []).append(dict(revision=state['revision'],
        transitioned_at=state['transitioned_at'], **visible))
    metrics = state.setdefault('protocol_metrics', {})
    metrics['meaningful_transitions'] = metrics.get('meaningful_transitions', 0) + 1
    metrics.setdefault('model_calls_on_unchanged_state', 0)
    return True


def failure_signature(packet):
    selected = {key: packet.get(key) for key in
                ('first_difference', 'terminal_failure', 'pc', 'ordinal', 'build')}
    return canonical(selected)


def load_audit(root, packet):
    audit = packet.get('audit')
    if isinstance(audit, dict):
        return audit
    if audit and Path(audit).is_file():
        return json.loads(Path(audit).read_text(encoding='utf-8-sig'))
    pc = packet.get('pc')
    event = packet.get('event') or (packet.get('terminal_failure') or {}).get('event') or {}
    pc = pc or event.get('pc')
    if not pc:
        return None
    image = packet.get('image')
    if not image:
        _, config = load_project(root)
        image = config.get('analysis', {}).get('default_image')
    if not image:
        return None
    from trace_audit_packet import audit as build_audit
    return build_audit(int(str(pc), 0), image,
                       event.get('actors', [{}])[0].get('state') if event.get('actors') else None)


def todo_branch(root, config, audit, additional_roots=()):
    if not audit or audit.get('ambiguous') or not audit.get('function'):
        return dict(nodes=[], unresolved=[], complete=False, reason='Causal function is not uniquely identified')
    start = (audit['image'], address_number(audit['function']['address']))
    database = Path(root) / config['paths']['analysis_database']
    roots = [start, *[(image, address_number(address)) for image,address in additional_roots if image == start[0]]]
    pending = list(dict.fromkeys(roots))
    additional = set(roots[1:])
    seen = set()
    nodes = []
    boundaries = []
    unresolved = []
    current_ledger = ledger_rows(root)
    db = sqlite3.connect(database.as_uri() + '?mode=ro', uri=True)
    try:
        db.row_factory = sqlite3.Row
        while pending:
            image, address = pending.pop()
            if (image, address) in seen:
                continue
            seen.add((image, address))
            function = db.execute('SELECT image,address,name,status,bytes,sha256,listing,pseudocode FROM functions WHERE image=? AND address=?',
                                  (image, address)).fetchone()
            if not function:
                unresolved.append(dict(image=image, address=hex(address), reason='missing_function'))
                continue
            row = dict(function)
            ledger_row = current_ledger.get((image, address))
            if ledger_row:
                row['status'] = ledger_row.get('status', row['status'])
            row['address'] = hex(row['address'])
            if (image, address) in additional and row['status'] != 'TODO':
                boundaries.append(dict(image=image, address=hex(address), status=row['status']))
                continue
            nodes.append(row)
            if row['status'] != 'TODO':
                continue
            edges = db.execute('SELECT e.id,e.site,e.kind,e.resolution FROM edges e WHERE e.source_image=? AND e.source_function=?',
                               (image, address)).fetchall()
            for edge in edges:
                targets = db.execute('SELECT target_image,target_function FROM edge_candidates WHERE edge=?',
                                     (edge['id'],)).fetchall()
                if len(targets) != 1:
                    unresolved.append(dict(image=image, source=hex(address), site=hex(edge['site']),
                                           kind=edge['kind'], resolution=edge['resolution'], candidates=len(targets)))
                    continue
                target = (targets[0]['target_image'], targets[0]['target_function'])
                if target[0] != start[0]:
                    unresolved.append(dict(image=image, source=hex(address), site=hex(edge['site']),
                                           kind=edge['kind'], resolution='cross_image_boundary',
                                           target_image=target[0], target_address=hex(target[1])))
                    continue
                target_status = db.execute('SELECT status FROM functions WHERE image=? AND address=?', target).fetchone()
                effective_status = (current_ledger.get(target) or {}).get(
                    'status', target_status['status'] if target_status else None)
                if effective_status == 'TODO':
                    pending.append(target)
                elif effective_status:
                    boundaries.append(dict(image=target[0], address=hex(target[1]), status=effective_status))
    finally:
        db.close()
    nodes.sort(key=lambda row: (row['image'], int(row['address'], 16)))
    boundaries = sorted({(row['image'], row['address'], row['status']) for row in boundaries})
    limits = config.get('stage_pipeline', {}).get('supervisor_protocol', {}).get('batch_limits',
        dict(functions=5, instructions=2048, modules=2))
    review_batches = []
    current = []
    instructions = 0
    for node in nodes:
        count = max(1, (int(node.get('bytes') or 0) + 3) // 4)
        if current and (len(current) >= int(limits['functions']) or instructions + count > int(limits['instructions'])):
            review_batches.append(dict(nodes=current, instructions=instructions))
            current = []
            instructions = 0
        current.append(dict(image=node['image'], address=node['address']))
        instructions += count
    if current:
        review_batches.append(dict(nodes=current, instructions=instructions,
                                   oversized_single=instructions > int(limits['instructions'])))
    return dict(root=dict(image=start[0], address=hex(start[1])),
                roots=[dict(image=image, address=hex(address)) for image,address in roots], nodes=nodes,
                boundaries=[dict(image=image, address=address, status=status) for image,address,status in boundaries],
                unresolved=unresolved, complete=not unresolved, review_batches=review_batches,
                order='Dependency-first within SCCs; split only at configured bounded-batch limits')


def classify_attention(packet, audit, repair):
    text = json.dumps(repair or {}, sort_keys=True).lower()
    if 'validation' in text:
        return 'validation_failure'
    if any(word in text for word in ('infrastructure', 'solver_unavailable', 'home_readonly')):
        return 'infrastructure_failure'
    if 'source changed' in text or 'foreign' in text:
        return 'foreign_source_change'
    if 'unsupported' in text or 'instruction' in text:
        return 'unsupported_instruction'
    if not audit or audit.get('ambiguous') or not audit.get('function'):
        return 'missing_observation'
    if audit['function'].get('status') == 'TODO':
        return 'todo_branch'
    gaps = audit.get('implementation', {}).get('gaps') or []
    if any(word in str(gap).lower() for gap in gaps
           for word in ('no unique', 'unavailable', 'ambiguous', 'boundary')):
        return 'ambiguous_semantics'
    return 'todo_branch'


def complete_causal_context(audit):
    if not audit or not audit.get('function'):
        return None
    result = dict(image=audit.get('image'), address=hex(address_number(audit['function']['address'])),
                  function=audit['function'], callers=audit.get('callers',[]),
                  callees=audit.get('edges',[]), implementation=audit.get('implementation'))
    for key in ('mips', 'pseudocode'):
        path = Path((audit.get(key) or {}).get('path',''))
        result[key] = dict(path=str(path), sha256=digest(path) if path.is_file() else None,
                           lines=path.read_text(errors='replace').splitlines() if path.is_file() else [])
    return result


def issue_attention(name, folder, result, repair=None):
    root, config = load_project()
    detail = result.get('diagnostic') or {}
    packet_path = detail.get('packet')
    if not packet_path and result.get('report'):
        packet_path = str(Path(result['report']).with_name('diagnostic-packet.json'))
    packet = json.loads(Path(packet_path).read_text(encoding='utf-8-sig')) if packet_path and Path(packet_path).is_file() else {}
    audit = load_audit(root, packet)
    if audit and audit.get('function'):
        key = (audit['image'], address_number(audit['function']['address']))
        ledger_row = ledger_rows(root).get(key)
        if ledger_row:
            audit['function']['status'] = ledger_row.get('status', audit['function'].get('status'))
    event = packet.get('event') or (packet.get('terminal_failure') or {}).get('event') or {}
    callback = event.get('callback') or {}
    callback_roots = []
    if audit and callback.get('target'):
        target = str(callback['target'])
        callback_roots.append((audit.get('image'), int(target, 16) if not target.lower().startswith('0x') else int(target, 0)))
    branch = todo_branch(root, config, audit, callback_roots)
    ledger = root / 'status/translation-ledger.json'
    ledger_before_path = Path(folder) / 'ledger-before.json'
    ledger_before = json.loads(ledger.read_text(encoding='utf-8-sig')) if ledger.is_file() else {'entries': []}
    write_receipt(ledger_before_path, ledger_before)
    snapshot = source_snapshot(root)
    signature = failure_signature(packet)
    if not packet:
        signature = canonical({'repair': repair, 'result_status': result.get('status')})
    kind = classify_attention(packet, audit, repair)
    if kind == 'todo_branch' and not branch['complete']:
        kind = 'ambiguous_semantics'
    template_path = Path(folder) / 'repair-manifest.template.json'
    value = dict(schema=1, trace=name, status='ATTENTION_REQUIRED', kind=kind,
                 failure_signature=signature, first_difference=packet.get('first_difference'),
                 verified_prefix=packet.get('verified_prefix_end'), image=(audit or {}).get('image'),
                 address=hex((audit or {}).get('function', {}).get('address')) if isinstance((audit or {}).get('function', {}).get('address'), int) else (audit or {}).get('function', {}).get('address'), causal_context=audit,
                 causal_function=complete_causal_context(audit),
                 captured_ram_words=packet.get('data_references') or packet.get('channel_context'),
                 source=snapshot, ledger=dict(path=str(ledger), sha256=digest(ledger) if ledger.is_file() else None,
                                              snapshot=str(ledger_before_path), snapshot_sha256=digest(ledger_before_path)),
                 todo_dependency_branch=branch, repair=repair,
                 evidence_hashes=dict(diagnostic=digest(packet_path) if packet_path and Path(packet_path).is_file() else None,
                                      audit=canonical(audit) if audit else None),
                 batch_limits=config.get('stage_pipeline', {}).get('supervisor_protocol', {}).get('batch_limits',
                    dict(functions=5, instructions=2048, modules=2)),
                 acceptance=['Edit only the issued image-qualified causal branch',
                             'Update every changed function ledger entry with exact evidence',
                             'Submit one manifest with X trace_workflow submit-repair NAME --manifest PATH',
                             'A provisional MATCH must pass full code_refresh, build_database and a fresh replay'],
                 prohibited=['Supervisor-generated gameplay C', 'Guessed ABI or structures',
                             'Broadened WIP/dummy scope', 'Ledger-only DONE promotion'],
                 manifest_template=str(template_path))
    if kind == 'missing_observation':
        value['required_observation'] = 'Provide an exact causal image/address or captured live-entry observation; candidates are not causal proof'
    path = Path(folder) / 'attention.json'
    write_receipt(path, value)
    template = dict(schema=1, failure_signature=signature, attention_sha256=digest(path),
                    expected_source_identity=snapshot['identity'], expected_ledger_sha256=value['ledger']['sha256'],
                    image=value['image'], address=value['address'], changed_files=[], changed_functions=[], ledger_entries=[],
                    completed_dependencies=[], evidence_hashes=value['evidence_hashes'])
    write_receipt(template_path, template)
    return value, path


def run_step(root, folder, tool, arguments=()):
    log = Path(folder) / ('submit-' + tool.replace('_', '-') + '.log')
    command = [sys.executable, '-B', str(Path(__file__).with_name('xport.py')), '--project', str(root), tool, *arguments]
    started = time.perf_counter()
    with log.open('w', encoding='utf-8') as output:
        result = subprocess.run(command, cwd=root, stdout=output, stderr=subprocess.STDOUT)
    receipt = dict(tool=tool, exit_code=result.returncode,
                   seconds=round(time.perf_counter() - started, 3), log=str(log))
    if result.returncode:
        raise RuntimeError(tool + ' failed; inspect ' + str(log))
    return receipt


def submit(name, manifest_path, wait=30):
    root, config = load_project()
    folder = artifact_path('status/stage-pipeline/workers') / name
    attention_path = folder / 'attention.json'
    if not attention_path.is_file():
        raise ValueError('No issued attention packet for trace ' + name)
    attention = json.loads(attention_path.read_text(encoding='utf-8-sig'))
    repairable_kinds = ('todo_branch', 'ambiguous_semantics', 'validation_failure')
    if attention.get('kind') not in repairable_kinds or not attention.get('todo_dependency_branch', {}).get('complete'):
        raise ValueError('Repair submission requires a complete image-qualified causal branch; satisfy the requested observation first')
    manifest_path = Path(manifest_path).resolve()
    if not manifest_path.is_relative_to(root) or not manifest_path.is_file():
        raise ValueError('Repair manifest must be an existing project file')
    manifest = json.loads(manifest_path.read_text(encoding='utf-8-sig'))
    required = {'schema', 'failure_signature', 'attention_sha256', 'expected_source_identity',
                'expected_ledger_sha256', 'changed_functions', 'ledger_entries', 'image',
                'address', 'changed_files', 'completed_dependencies', 'evidence_hashes'}
    missing = sorted(required - set(manifest))
    if missing:
        raise ValueError('Repair manifest is missing: ' + ', '.join(missing))
    if manifest['schema'] != 1 or manifest['failure_signature'] != attention['failure_signature']:
        raise ValueError('Repair manifest does not match the issued failure')
    if manifest['attention_sha256'] != digest(attention_path):
        raise ValueError('Issued attention packet changed')
    if manifest['expected_source_identity'] != attention['source']['identity']:
        raise ValueError('Unexpected pre-edit source identity')
    if manifest['expected_ledger_sha256'] != attention['ledger']['sha256']:
        raise ValueError('Unexpected pre-edit ledger identity')
    if manifest['image'] != attention.get('image') or manifest['address'] != attention.get('address'):
        raise ValueError('Repair image/address differs from the issued causal function')
    before = attention['source']['files']
    after = source_snapshot(root)
    changed = sorted(key for key in set(before) | set(after['files']) if before.get(key) != after['files'].get(key))
    declared_files = sorted(set(manifest['changed_files']))
    if changed != declared_files:
        raise ValueError('Changed source files differ from manifest: ' + json.dumps(changed))
    ledger_path = root / 'status/translation-ledger.json'
    ledger = json.loads(ledger_path.read_text(encoding='utf-8-sig'))
    ledger_rows = {(row['image'], row['address']): row for row in ledger.get('entries', [])}
    before_ledger_path = Path(attention['ledger']['snapshot'])
    if digest(before_ledger_path) != attention['ledger']['snapshot_sha256']:
        raise ValueError('Issued ledger snapshot changed')
    before_ledger = json.loads(before_ledger_path.read_text(encoding='utf-8-sig'))
    before_rows = {(row['image'], row['address']): row for row in before_ledger.get('entries', [])}
    for item in manifest['changed_functions']:
        key = (item['image'], address_number(item['address']))
        if key not in ledger_rows or ledger_rows[key].get('source') != item['source']:
            raise ValueError('Changed function lacks an exact current ledger mapping: ' + str(key))
        if item['image'] != attention['image']:
            raise ValueError('Repair crosses image/overlay boundary')
    branch = {(row['image'], address_number(row['address'])) for row in attention['todo_dependency_branch']['nodes']}
    completed = {(row['image'], address_number(row['address'])) for row in manifest['completed_dependencies']}
    changed_functions = {(row['image'], address_number(row['address'])) for row in manifest['changed_functions']}
    if changed_functions != branch or completed != branch:
        raise ValueError('Repair must complete exactly the issued TODO dependency branch')
    modules = {path for path in declared_files if Path(path).suffix.lower() == '.c'}
    if len(modules) > int(attention['batch_limits']['modules']):
        raise ValueError('Repair exceeds the issued module boundary')
    changed_ledger = {key for key in set(before_rows) | set(ledger_rows)
                      if before_rows.get(key) != ledger_rows.get(key)}
    if not changed_ledger <= changed_functions:
        raise ValueError('Changed ledger identities include undeclared functions')
    required_ledger = {key for key in branch
                       if key not in before_rows or before_rows[key].get('status') == 'TODO'}
    if not required_ledger <= changed_ledger:
        raise ValueError('New TODO translations require changed ledger entries')
    changed_ledger_files = {ledger_rows[key].get('source') for key in changed_ledger if key in ledger_rows}
    if not changed_ledger_files <= set(declared_files):
        raise ValueError('Every changed ledger source must be a declared changed file')
    if manifest['ledger_entries'] != [ledger_rows[(row['image'], address_number(row['address']))]
                                      for row in manifest['changed_functions']]:
        raise ValueError('Manifest ledger entries are not exact current entries')
    if not manifest['evidence_hashes']:
        raise ValueError('Repair manifest requires evidence hashes')
    submissions = folder / 'submissions'
    submissions.mkdir(exist_ok=True)
    submission_path = submissions / (manifest['attention_sha256'] + '.json')
    if submission_path.is_file():
        previous = json.loads(submission_path.read_text(encoding='utf-8-sig'))
        state_path = folder / 'state.json'
        state = json.loads(state_path.read_text(encoding='utf-8-sig')) if state_path.is_file() else {}
        if previous.get('status') == 'verified' and state.get('status') == 'VERIFYING_REPAIR':
            from trace_converge_worker import control
            result = control(name, 'start', wait, final_gate_on_match=True, resume_attention=True)
            result['submission'] = str(submission_path)
            result['resumed_verified_submission'] = True
            return result
        raise ValueError('This attention packet already has a repair submission')
    state_path = folder / 'state.json'
    state = json.loads(state_path.read_text(encoding='utf-8-sig')) if state_path.is_file() else {'revision': 0}
    transition(state, 'VERIFYING_REPAIR', failure_signature=attention['failure_signature'])
    write_receipt(state_path, state)
    try:
        steps = [run_step(root, folder, 'source_index', ('--rebuild',)),
                 run_step(root, folder, 'code_refresh_fast'), run_step(root, folder, 'render_progress')]
    except (ValueError, OSError, RuntimeError, subprocess.SubprocessError) as error:
        attention['kind'] = 'validation_failure'
        attention['validation_failure'] = dict(error=str(error), failed_at=time.time())
        write_receipt(attention_path, attention)
        template_path = Path(attention['manifest_template'])
        template = json.loads(template_path.read_text(encoding='utf-8-sig'))
        template['attention_sha256'] = digest(attention_path)
        write_receipt(template_path, template)
        transition(state, 'ATTENTION_REQUIRED', attention_kind='validation_failure',
                   failure_signature=attention['failure_signature'], model_intervention_required=True)
        state['attention'] = str(attention_path)
        state['error'] = str(error)
        write_receipt(state_path, state)
        raise
    formatted = source_snapshot(root)
    formatted_changed = sorted(key for key in set(before) | set(formatted['files'])
                               if before.get(key) != formatted['files'].get(key))
    if formatted_changed != changed:
        raise RuntimeError('Formatting changed files outside the submitted repair scope')
    receipt = dict(schema=1, status='verified', trace=name, failure_signature=attention['failure_signature'],
                   attention_sha256=manifest['attention_sha256'],
                   manifest=str(manifest_path), manifest_sha256=digest(manifest_path), source=formatted,
                   ledger_sha256=digest(ledger_path), steps=steps, submitted_at=time.time())
    write_receipt(submission_path, receipt)
    write_receipt(folder / 'repair-submission.json', receipt)
    from trace_converge_worker import control
    result = control(name, 'start', wait, final_gate_on_match=True, resume_attention=True)
    result['submission'] = str(submission_path)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('shadow','refresh-attention'))
    parser.add_argument('--name', required=True)
    parser.add_argument('--diagnostic', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--image')
    parser.add_argument('--address')
    args = parser.parse_args()
    if args.action == 'refresh-attention':
        folder = artifact_path('status/stage-pipeline/workers') / args.name
        state = json.loads((folder / 'state.json').read_text(encoding='utf-8-sig'))
        result = state.get('result') or {}
        if bool(args.image) != bool(args.address):
            parser.error('refresh-attention requires both --image and --address')
        if args.image:
            detail = result.get('diagnostic') or {}
            source_path = detail.get('packet')
            packet = json.loads(Path(source_path).read_text(encoding='utf-8-sig')) if source_path else {}
            packet['pc'] = hex(address_number(args.address))
            packet['image'] = args.image
            packet.pop('audit', None)
            packet['causal_observation'] = dict(kind='explicit_image_address', image=args.image,
                                                address=hex(address_number(args.address)),
                                                source_diagnostic=source_path,
                                                source_diagnostic_sha256=digest(source_path) if source_path else None)
            observation_path = folder / 'causal-observation.json'
            write_receipt(observation_path, packet)
            result = dict(result, diagnostic=dict(packet=str(observation_path)))
        value, path = issue_attention(args.name, folder, result, state.get('repair'))
        state['attention'] = str(path)
        state['attention_kind'] = value['kind']
        state['failure_signature'] = value['failure_signature']
        write_receipt(folder / 'state.json', state)
        print(json.dumps({'status':'refreshed','attention':str(path),'sha256':digest(path)},separators=(',',':')))
        return
    if not args.diagnostic or not args.output:
        parser.error('shadow requires --diagnostic and --output')
    output = artifact_path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    value, path = issue_attention(args.name, output,
                                  {'status': 'mismatch', 'diagnostic': {'packet': str(args.diagnostic.resolve())}})
    legacy = json.loads(args.diagnostic.read_text(encoding='utf-8-sig'))
    comparison = dict(schema=1, status='complete', trace=args.name,
                      source_diagnostic=dict(path=str(args.diagnostic.resolve()), sha256=digest(args.diagnostic)),
                      legacy=dict(required_manual_routing=True,
                                  first_difference=legacy.get('first_difference'),
                                  verified_prefix=legacy.get('verified_prefix_end'),
                                  audit=legacy.get('audit')),
                      supervisor=dict(single_attention_packet=True, attention=str(path),
                                      attention_sha256=digest(path), kind=value['kind'],
                                      branch_nodes=len(value['todo_dependency_branch']['nodes']),
                                      branch_complete=value['todo_dependency_branch']['complete']),
                      invariant_checks=dict(failure_signature_bound=True,
                                            source_and_ledger_bound=True,
                                            gameplay_source_edited=False,
                                            comparison_scope_changed=False,
                                            controller_transfers_changed=False,
                                            pruning_changed=False))
    comparison_path = output / 'shadow-comparison.json'
    write_receipt(comparison_path, comparison)
    print(json.dumps({'status': 'shadow_complete', 'attention': str(path),
                      'sha256': digest(path), 'kind': value['kind'],
                      'failure_signature': value['failure_signature'],
                      'comparison': str(comparison_path)}, separators=(',', ':')))


if __name__ == '__main__':
    main()

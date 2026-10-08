"""Admit reviewed original indirect dependencies without changing capture membership"""
import copy
from contextlib import closing
import json
from pathlib import Path
import sqlite3
import uuid
import coverage_image_migration as migration

CONTRACT = 'xport-coverage-indirect-dependencies-v1'
HASHES = ('function_sha256', 'mips_sha256', 'pseudocode_sha256', 'machine_audit_sha256')


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def binding(root, path):
    return {'path': path.relative_to(root).as_posix(), 'sha256': migration.digest(path.read_bytes())}


def validate_review(root, project, state, review, active=False):
    root = Path(root).resolve()
    accepted = {}
    def accepted_image(image, receipt):
        key = image, json.dumps(receipt, sort_keys=True)
        if key not in accepted:
            accepted[key] = migration.accepted_image(root, project, image, receipt)
        return accepted[key]
    if review.get('contract') != CONTRACT or review.get('schema') != 1 or review.get('coverage_name') != state['name']:
        raise ValueError('Invalid indirect dependency review contract')
    packet_path = migration.bound(root, review['packet'])
    if active and packet_path != (root / state.get('active_packet', '')).resolve():
        raise ValueError('Indirect dependency review is not the active packet')
    packet = read(packet_path)
    if packet.get('coverage_name') != state['name']:
        raise ValueError('Indirect dependency packet belongs to another run')
    owner = review['owner']
    contexts = [packet['source_context']] + packet['source_context'].get('audit_cluster', [])
    matches = [c for c in contexts if c['function']['image'] == owner['image'] and c['function']['address'] == owner['address']]
    if len(matches) != 1:
        raise ValueError('Missing or ambiguous indirect owner context')
    context = matches[0]
    required = packet['source_context']['audit_contract']['required_functions']
    identity = [r for r in required if (r['image'], r['address']) == (owner['image'], owner['address'])]
    if len(identity) != 1 or any(owner.get(h) != identity[0].get(h) for h in HASHES):
        raise ValueError('Indirect owner has stale original four-hash identity')
    recorded = {(r.get('image', state['image']), r['address']): r for r in state['functions']}
    if (owner['image'], owner['address']) not in recorded or recorded[(owner['image'], owner['address'])]['original_sha256'] != owner['function_sha256']:
        raise ValueError('Indirect owner is outside immutable recorded membership')
    profile, originals, _ = accepted_image(owner['image'], owner['acceptance'])
    original = originals.get(owner['address'])
    if not original or original['sha256'] != owner['function_sha256'] or not any(migration.original_body(root, profile, original)):
        raise ValueError('Indirect owner lacks exact nonzero accepted original')
    for field, key in [('mips', 'mips_sha256'), ('pseudocode', 'pseudocode_sha256')]:
        migration.bound(root, {'path': context[field]['path'], 'sha256': owner[key]}, 'orig')
    if context['machine_audit']['sha256'] != owner['machine_audit_sha256']:
        raise ValueError('Indirect owner machine checklist changed')
    closed = read(migration.bound(root, review['closed_target_set']))
    if closed.get('contract') != 'xport-indirect-target-review-v1' or closed.get('owner') != {k: owner[k] for k in ('image', 'address')} or closed.get('reviewed_complete') is not True:
        raise ValueError('Indirect targets lack a complete owner-specific closed set review')
    for evidence in closed.get('evidence', []):
        migration.bound(root, evidence)
    if not closed.get('evidence'):
        raise ValueError('Closed set review has no hash-bound original evidence')
    sites = review.get('sites', [])
    if not sites or sites != closed.get('sites') or len({s['site'] for s in sites}) != len(sites):
        raise ValueError('Indirect site target sets differ or contain duplicates')
    database = (root / project['paths']['analysis_database']).resolve()
    rows, keys = [], set()
    with closing(sqlite3.connect(database.as_uri() + '?mode=ro', uri=True)) as db:
        for site in sites:
            pc = site['site']
            checks = [c for c in context['machine_audit']['checks'] if int(c['pc'], 0) == pc and c['kind'] in ('call', 'control')]
            if len(checks) != 1 or checks[0] != site['check']:
                raise ValueError('Indirect site differs from exact machine check')
            word = int(checks[0]['word'], 0)
            instruction = db.execute('SELECT word FROM instructions WHERE image=? AND function=? AND address=?', (owner['image'], owner['address'], pc)).fetchall()
            rs, rt, rd, shift, operation = (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31, (word >> 6) & 31, word & 63
            linked = operation == 9 and checks[0]['kind'] == 'call' and rd != 0
            tail = operation == 8 and checks[0]['kind'] == 'control' and rs not in (0, 31) and rd == 0
            if (len(instruction) != 1 or int(instruction[0][0], 16) != word
                    or word >> 26 != 0 or rt != 0 or shift != 0 or not (linked or tail)):
                raise ValueError('Indirect site lacks unique original JALR or non-return JR')
            targets = site.get('targets', [])
            if not targets or len({(t['image'], t['address']) for t in targets}) != len(targets):
                raise ValueError('Empty or duplicate indirect target set')
            for target in targets:
                key = target['image'], target['address']
                if target['image'] not in project.get('analysis', {}).get('images', []):
                    raise ValueError('Indirect target image is not selected')
                profile, metadata, _ = accepted_image(key[0], target['acceptance'])
                function = metadata.get(key[1])
                if not function or function.get('status') != 'TODO' or function['sha256'] != target.get('function_sha256') or not any(migration.original_body(root, profile, function)):
                    raise ValueError('Indirect target lacks exact nonzero accepted original')
                result = db.execute('SELECT status,sha256,name FROM functions WHERE image=? AND address=?', key).fetchall()
                sdk = db.execute('SELECT COUNT(*) FROM library_classification WHERE image=? AND address=?', key).fetchone()[0]
                if len(result) != 1 or result[0][0] not in ('TODO', 'WIP', 'DONE') or result[0][1] != target['function_sha256'] or function['name'] != target['symbol'] or sdk:
                    raise ValueError('Indirect target classification or symbol differs from original game metadata')
                if key not in keys:
                    rows.append({'image': key[0], 'address': key[1], 'sha256': target['function_sha256'], 'name': target['symbol'], 'status': 'TODO', 'recorded': key in recorded})
                    keys.add(key)
    return rows


def validate_state(root, project, state):
    root = Path(root).resolve()
    if not state.get('name'):
        if state.get('indirect_dependency_admissions'):
            raise ValueError('Indirect admission has no coverage run identity')
        return []
    directory = root / 'status/function-coverage' / state['name']
    retained = {(root / item['path']).resolve() for item in state.get('indirect_dependency_admissions', [])}
    actual = {path.resolve() for path in directory.glob('indirect-dependencies-preserved-*/admission-receipt.json')}
    if retained != actual:
        raise ValueError('Indirect admission receipt membership changed')
    rows, keys, owners = [], set(), set()
    for item in state.get('indirect_dependency_admissions', []):
        receipt_path = migration.bound(Path(root).resolve(), item)
        receipt = read(receipt_path)
        if receipt.get('contract') != CONTRACT:
            raise ValueError('Invalid indirect admission receipt')
        preserved = receipt_path.parent / 'coverage.json'
        if not preserved.is_file() or migration.digest(preserved.read_bytes()) != receipt.get('state_before_sha256'):
            raise ValueError('Indirect admission lost preserved state')
        before = read(preserved)
        immutable = lambda value: [(r.get('image', value['image']), r['address'], r['original_sha256']) for r in value['functions']]
        if before['name'] != state['name'] or before['image'] != state['image'] or immutable(before) != immutable(state) or before.get('functions_sha256') != state.get('functions_sha256'):
            raise ValueError('Indirect admission changed immutable capture membership')
        review = read(migration.bound(Path(root).resolve(), receipt['review']))
        owner = review['owner']['image'], review['owner']['address']
        if owner in owners:
            raise ValueError('Duplicate indirect owner admission')
        owners.add(owner)
        validated = validate_review(root, project, state, review)
        if validated != receipt.get('targets'):
            raise ValueError('Indirect admission target identities changed')
        for row in validated:
            key = row['image'], row['address']
            if key not in keys:
                rows.append(row)
                keys.add(key)
    return rows


def admit(name, review_path):
    import coverage
    root, project, state, selected = coverage.load_state(name)
    root = root.resolve()
    review_path = Path(review_path).resolve()
    if not review_path.is_relative_to(root / 'status') or not review_path.is_file():
        raise ValueError('Indirect dependency review must be project-owned status evidence')
    review_binding = binding(root, review_path)
    review = read(review_path)
    targets = validate_review(root, project, state, review, active=True)
    for old in state.get('indirect_dependency_admissions', []):
        previous = read(migration.bound(root, read(migration.bound(root, old))['review']))
        if previous['owner']['image'] == review['owner']['image'] and previous['owner']['address'] == review['owner']['address']:
            raise ValueError('Duplicate indirect owner admission')
    solution = (root / project['paths']['native_solution']).resolve()
    files = [selected['state'], root / 'src/game_link_stubs.c', root / 'status/translation-ledger.json', root / project['paths']['analysis_database']] + list(solution.parent.glob('*.vcxproj'))
    database = root / project['paths']['analysis_database']
    files += [Path(str(database) + suffix) for suffix in ('-wal', '-shm', '-journal')]
    if any(not f.resolve().is_relative_to(root) for f in files):
        raise ValueError('Indirect transaction has foreign mutation path')
    snapshots = {f: f.read_bytes() if f.is_file() else None for f in files}
    temporary = (root / project['paths']['analysis_database']).with_suffix('.source-index.tmp')
    if temporary.exists():
        raise ValueError('Unowned source-index temporary file')
    preserved = selected['directory'] / ('indirect-dependencies-preserved-' + uuid.uuid4().hex)
    updated = copy.deepcopy(state)
    try:
        preserved.mkdir()
        (preserved / 'coverage.json').write_bytes(snapshots[selected['state']])
        receipt = {'schema': 1, 'contract': CONTRACT, 'review': review_binding, 'state_before_sha256': migration.digest(snapshots[selected['state']]), 'targets': targets}
        receipt_path = preserved / 'admission-receipt.json'
        receipt_path.write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
        updated.setdefault('indirect_dependency_admissions', []).append(binding(root, receipt_path))
        validate_state(root, project, updated)
        recorded = {(r.get('image', state['image']), r['address']) for r in state['functions']}
        stubs = coverage.sync_link_stubs(root, project, state['image'], [r['address'] for r in state['functions']], [r for r in targets if not r['recorded']], recorded)
        if stubs['changed']:
            coverage.source_refresh(root)
        updated['link_only_stubs'] = stubs
        coverage.save_state(selected['state'], updated)
        coverage.load_state(name)
    except BaseException:
        for path, original in snapshots.items():
            if original is None:
                path.unlink(missing_ok=True)
            else:
                path.write_bytes(original)
        temporary.unlink(missing_ok=True)
        if preserved.exists():
            for child in preserved.iterdir():
                child.unlink()
            preserved.rmdir()
        raise
    return {'status': 'indirect_dependencies_admitted', 'receipt': str(receipt_path), 'targets': targets, 'recorded_membership_unchanged': True, 'packet_reissue_required': stubs['changed']}

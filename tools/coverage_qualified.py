"""Resolve reviewed image-qualified coverage rows and dependency gaps"""
from contextlib import closing
import json
import sqlite3
import struct
from pathlib import Path
import coverage_image_migration as migration


def image_of(state, row):
    return row.get('image', state.get('image'))


def validate_state(root, state):
    overrides = {row['address']: (row['image'], row['original_sha256']) for row in state['functions'] if row.get('image') and row['image'] != state['image']}
    if not overrides and not state.get('image_migration'):
        return
    binding = state.get('image_migration')
    if not binding:
        raise ValueError('Coverage image override lacks reviewed migration receipt')
    path = migration.bound(Path(root).resolve(), binding)
    receipt = json.loads(path.read_text(encoding='utf-8-sig'))
    expected = {row['address']: (row['image'], row['function_sha256']) for row in receipt.get('changes', [])}
    if receipt.get('contract') != 'xport-coverage-image-migration-v1' or overrides != expected:
        raise ValueError('Coverage image identities differ from migration receipt')
    root = Path(root).resolve()
    binary = migration.bound(root, receipt['request']['functions'])
    raw = binary.read_bytes()
    if len(raw) % 4:
        raise ValueError('Invalid preserved capture length')
    addresses = list(struct.unpack('<%dI' % (len(raw) // 4), raw))
    if addresses != [row['address'] for row in state['functions']]:
        raise ValueError('Migrated state membership or first-call order changed')
    preserved = (path.parent / 'coverage.json').resolve()
    if not preserved.is_file() or migration.digest(preserved.read_bytes()) != receipt.get('state_before_sha256'):
        raise ValueError('Missing or stale preserved coverage state')
    before = json.loads(preserved.read_text(encoding='utf-8-sig'))
    for field in ['name', 'image', 'recorded_functions', 'functions_sha256', 'source_recording_sha256']:
        if state.get(field) != before.get(field):
            raise ValueError('Migrated state changed immutable run identity')
    if state.get('recorded_functions', len(addresses)) != len(addresses):
        raise ValueError('Migrated state denominator differs from capture')
    if [row['address'] for row in before['functions']] != addresses:
        raise ValueError('Preserved state differs from capture order')
    changes = {row['address']: row for row in receipt['changes']}
    for current, original in zip(state['functions'], before['functions']):
        change = changes.get(current['address'])
        expected_image = change['image'] if change else image_of(before, original)
        expected_sha = change['function_sha256'] if change else original['original_sha256']
        if image_of(state, current) != expected_image or current.get('original_sha256') != expected_sha:
            raise ValueError('Migrated state changed an original function identity')
    migration.bound(root, receipt['request']['recording_receipt'])
    for packet in receipt['request']['packets']:
        migration.bound(root, packet)
    if state['functions_sha256'] != receipt['request']['functions']['sha256']:
        raise ValueError('Migrated state differs from immutable capture')


def rows(root, database, state):
    from coverage import function_rows
    result = []
    for item in state['functions']:
        image = image_of(state, item)
        row = function_rows(root, database, image, [item['address']])[0]
        row['image'] = image
        result.append(row)
    return result


def inline_dispositions(root, state, profiles, metadata):
    """Validate exact accepted caller-specific inline evidence"""
    from source_index import functions
    allowed = set()
    for binding in state.get('dependency_inline_dispositions', []):
        path = migration.bound(root.resolve(), binding, 'status')
        disposition = json.loads(path.read_text(encoding='utf-8-sig'))
        if disposition.get('contract') != 'xport-coverage-inline-dependency-v1':
            raise ValueError('Unsupported inline dependency disposition')
        image, address = disposition['source_image'], disposition['source_function']
        owner = next((row for row in state['functions'] if image_of(state, row) == image and row['address'] == address), None)
        if not owner or owner.get('proof_status') != 'proved':
            raise ValueError('Inline dependency owner is not accepted')
        audit_path = migration.bound(root.resolve(), disposition['audit_receipt'], 'status')
        if (root / owner.get('audit_receipt', '')).resolve() != audit_path:
            raise ValueError('Inline disposition differs from accepted owner receipt')
        audit = json.loads(audit_path.read_text(encoding='utf-8-sig'))
        matches = [row for row in audit.get('function_audits', []) if row['image'] == image and row['address'] == address]
        if audit.get('contract') != 'xport-lockstep-function-audit-v1' or len(matches) != 1:
            raise ValueError('Missing accepted inline owner audit')
        proof = matches[0]
        for field in ['function_sha256', 'mips_sha256', 'pseudocode_sha256', 'machine_audit_sha256']:
            if proof.get(field) != disposition['owner_context'][field]:
                raise ValueError('Inline disposition owner context changed')
        original = metadata.get(image, {}).get(address)
        if not original or original['sha256'] != owner['original_sha256'] or original['sha256'] != proof['function_sha256']:
            raise ValueError('Inline disposition original owner changed')
        owner_bytes = migration.original_body(root, profiles[image], original)
        offset = 0
        instruction = None
        for start, end in original['chunks']:
            if start <= disposition['site'] < end:
                instruction = struct.unpack_from('<I', owner_bytes, offset + disposition['site'] - start)[0]
                break
            offset += end - start
        if instruction is None or instruction >> 26 != 3 or (((disposition['site'] + 4) & 0xf0000000) | ((instruction & 0x3ffffff) << 2)) != disposition['target']:
            raise ValueError('Inline original call does not match disposition')
        if not proof.get('complete_mips_audit') or proof.get('unresolved_semantics'):
            raise ValueError('Inline owner lacks complete accepted proof')
        review = json.loads(migration.bound(root.resolve(), disposition['review'], 'status').read_text(encoding='utf-8-sig'))
        summary = [row for row in review.get('summaries', []) if row.get('image') == image and row.get('address') == address and not row.get('discrepancies')]
        if len(summary) != 1 or summary[0].get('note') != disposition['review_note']:
            raise ValueError('Inline semantic review changed')
        target = disposition['target_original']
        if target['address'] != disposition['target'] or not any(migration.original_body(root, profiles[image], target)):
            raise ValueError('Inline target bytes are absent')
        source = disposition['native_function']
        source_path = (root / source['path']).resolve()
        if not source_path.is_relative_to((root / 'src').resolve()):
            raise ValueError('Inline source is outside project source')
        text = source_path.read_text(encoding='utf-8-sig')
        native = [row for row in functions(text, source['path']) if row['name'] == source['symbol']]
        if len(native) != 1 or native[0]['sha256'] != source['sha256']:
            raise ValueError('Accepted inline native function changed')
        check = f"0x{disposition['site']:08x}:call"
        mappings = [row for row in proof['machine_coverage'] if check in row['check_ids'] and row['source_file'] == source['path'] and row['source_symbol'] == source['symbol']]
        if len(mappings) != 1:
            raise ValueError('Inline call lacks exact accepted mapping')
        for effect in disposition['native_effects']:
            line = native[0]['start_line'] + effect['relative_line'] - 1
            if text.splitlines()[line - 1].strip() != effect['text']:
                raise ValueError('Inline native effect changed')
        if not disposition['native_effects'] or mappings[0]['source_line'] != native[0]['start_line'] + disposition['native_effects'][0]['relative_line'] - 1:
            raise ValueError('Inline mapping does not name reviewed effect')
        allowed.add((image, address, disposition['site'], disposition['target']))
    return allowed


def validate_inline_admissions(root, state):
    root = Path(root).resolve()
    reviewed = set()
    for binding in state.get('dependency_inline_admissions', []):
        path = migration.bound(root, binding, 'status')
        receipt = json.loads(path.read_text(encoding='utf-8-sig'))
        preserved = path.parent / 'coverage.json'
        if receipt.get('contract') != 'xport-coverage-inline-admission-v1' or not preserved.is_file() or migration.digest(preserved.read_bytes()) != receipt.get('state_before_sha256'):
            raise ValueError('Missing or stale inline admission preservation')
        review = receipt['review']
        migration.bound(root, review, 'status')
        reviewed.add((review['path'], review['sha256']))
    expected = {(row['path'], row['sha256']) for row in state.get('dependency_inline_dispositions', [])}
    if expected != reviewed:
        raise ValueError('Inline disposition lacks supervisor admission receipt')


def validate_inline_state(root, project, state):
    if not state.get('dependency_inline_dispositions'):
        return set()
    if not state.get('image_migration'):
        raise ValueError('Inline dispositions require qualified migration')
    root = Path(root)
    profiles, metadata = {}, {}
    for item in state['dependency_inline_dispositions']:
        review = json.loads(migration.bound(root.resolve(), item, 'status').read_text(encoding='utf-8-sig'))
        image = review['source_image']
        profiles[image] = json.loads((root / project['paths'].get('ida_configs', 'tools/ida') / (image + '.json')).read_text(encoding='utf-8-sig'))
        metadata[image] = {row['address']: row for row in json.loads((root / project['paths']['ida_exports'] / image / 'functions.json').read_text(encoding='utf-8-sig'))}
    return inline_dispositions(root, state, profiles, metadata)


def dependencies(root, project, state):
    from coverage import direct_link_dependencies, function_rows
    root = Path(root)
    database = (root / project['paths']['analysis_database']).resolve()
    recorded = {(image_of(state, item), item['address']) for item in state['functions']}
    overrides = {item['address']: image_of(state, item) for item in state['functions'] if image_of(state, item) != state['image']}
    dependencies = {}
    pending = []
    main_addresses = [item['address'] for item in state['functions']]
    for address in direct_link_dependencies(database, state['image'], main_addresses):
        key = (state['image'], address)
        if key not in recorded and address not in overrides:
            row = function_rows(root, database, key[0], [address])[0]
            row['image'] = key[0]
            dependencies[key] = row
    profiles = {}
    metadata = {}
    for image in project.get('analysis', {}).get('images', []):
        profile_path = root / project['paths'].get('ida_configs', 'tools/ida') / (image + '.json')
        metadata_path = root / project['paths']['ida_exports'] / image / 'functions.json'
        if profile_path.is_file() and metadata_path.is_file():
            profiles[image] = json.loads(profile_path.read_text(encoding='utf-8-sig'))
            metadata[image] = {row['address']: row for row in json.loads(metadata_path.read_text(encoding='utf-8-sig'))}
    accepted = set()
    if state.get('image_migration'):
        for receipt_path in sorted((root / 'status/ida/accepted').glob('*.json')):
            receipt = json.loads(receipt_path.read_text(encoding='utf-8-sig'))
            for manifest in receipt.get('images', []):
                image = manifest.get('image')
                if image not in metadata or image == state['image']:
                    continue
                profile, index, _names = migration.accepted_image(root, project, image, {
                    'path': receipt_path.relative_to(root).as_posix(),
                    'sha256': migration.digest(receipt_path.read_bytes())})
                profiles[image], metadata[image] = profile, index
                accepted.add(image)
    inline = inline_dispositions(root, state, profiles, metadata) if state.get('image_migration') else set()
    main_pending = []
    nonzero_cache = {}

    def nonzero(image, address):
        key = image, address
        if key not in nonzero_cache:
            row = metadata.get(image, {}).get(address)
            nonzero_cache[key] = bool(row and any(migration.original_body(root, profiles[image], row)))
        return nonzero_cache[key]

    with closing(sqlite3.connect(database)) as db:
        db.row_factory = sqlite3.Row
        for item in state['functions']:
            source_image = image_of(state, item)
            is_main = source_image == state['image']
            if is_main and not state.get('image_migration'):
                continue
            if is_main and db.execute("SELECT 1 FROM functions f JOIN library_classification l ON l.image=f.image AND l.address=f.address WHERE f.image=? AND f.address=? AND f.status='SKIP'", (source_image, item['address'])).fetchone():
                continue
            edges = db.execute("SELECT site,target,kind FROM edges WHERE source_image=? AND source_function=? AND kind IN ('call','conditional_link','tail_candidate') ORDER BY site", (source_image, item['address']))
            for edge in edges:
                target = edge['target']
                if (source_image, item['address'], edge['site'], target) in inline:
                    continue
                own = metadata.get(source_image, {}).get(item['address'], {})
                if any(start <= target < end for start, end in own.get('chunks', [])):
                    continue
                if is_main and nonzero(source_image, target):
                    continue
                if not is_main and nonzero(source_image, target):
                    target_image = source_image
                elif target in overrides:
                    target_image = overrides[target]
                else:
                    candidates = [image for image in metadata if (not is_main or image in accepted) and nonzero(image, target)]
                    target_image = candidates[0] if len(candidates) == 1 else None
                if target_image is None:
                    (main_pending if is_main else pending).append({'source_image': source_image, 'source_function': item['address'], 'site': edge['site'], 'target': target, 'kind': edge['kind'], 'reason': 'Missing or ambiguous reviewed original target metadata'})
                    continue
                key = target_image, target
                if key not in recorded and key not in dependencies:
                    row = function_rows(root, database, key[0], [target])[0]
                    row['image'] = key[0]
                    dependencies[key] = row
    if state.get('image_migration'):
        state['main_dependency_unresolved'] = main_pending
    from coverage_indirect import validate_state as validate_indirect_state
    for target in validate_indirect_state(root, project, state):
        key = target['image'], target['address']
        if key not in recorded and key not in dependencies:
            row = function_rows(root, database, key[0], [key[1]])[0]
            row['image'] = key[0]
            dependencies[key] = row
    return list(dependencies.values()), pending


def guard_packet(state, packet):
    owners = {(row['image'], row['address']) for row in packet['source_context']['audit_contract']['required_functions']}
    missing = [row for row in state.get('dependency_pending', []) + state.get('main_dependency_unresolved', []) if (row['source_image'], row['source_function']) in owners]
    if missing:
        raise ValueError('Coverage packet has unresolved original dependency metadata: ' + str(len(missing)))

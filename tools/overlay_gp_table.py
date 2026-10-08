"""Validate an accepted overlay's reviewed table loader GP inheritance"""
import hashlib
import json
from pathlib import Path

from overlay_gp import _bound, _digest, _owned
from overlay_gp_static import _destination, _integer


def _accepted(root, evidence, prefix):
    config_path = _bound(root, evidence, prefix + '_config', 'tools/ida')
    config = json.loads(config_path.read_text(encoding='utf-8-sig'))
    raw_path = _bound(root, evidence, prefix + '_original_path')
    loads = config.get('loads', [])
    if len(loads) != 1 or _owned(root, loads[0].get('path')) != raw_path:
        raise ValueError('Accepted loader original differs from configured load')
    base, offset = loads[0].get('base'), loads[0].get('offset', 0)
    _integer(base, 'accepted base')
    _integer(offset, 'accepted offset')
    raw = raw_path.read_bytes()
    receipt_path = _bound(root, evidence, prefix + '_acceptance_receipt', 'status/ida/accepted')
    receipt = json.loads(receipt_path.read_text(encoding='utf-8-sig'))
    rows = [row for row in receipt.get('images', []) if row.get('image') == config.get('image')]
    if receipt.get('version') != 1 or len(rows) != 1:
        raise ValueError('Missing unique accepted loader image')
    row = rows[0]
    if _owned(root, row.get('config')) != config_path or row.get('config_sha256') != _digest(config_path):
        raise ValueError('Loader profile is not accepted')
    destination = (root / receipt.get('destination', '')).resolve()
    image_root = (destination / config['image']).resolve()
    if not destination.is_relative_to(root) or not image_root.is_relative_to(destination) or not row.get('files'):
        raise ValueError('Invalid accepted loader root')
    names = set()
    for item in row['files']:
        artifact = (image_root / item['path']).resolve()
        if item['path'] in names or not artifact.is_relative_to(image_root) or not artifact.is_file() or _digest(artifact) != item.get('sha256'):
            raise ValueError('Stale or duplicate accepted loader artifact')
        names.add(item['path'])
    if not {'functions.json', 'export-report.json'}.issubset(names):
        raise ValueError('Missing accepted loader metadata')

    def original(address, size):
        if type(address) is not int or type(size) is not int or size <= 0 or address < base or address - base + offset + size > len(raw):
            raise ValueError('Loader original range escapes raw file')
        return raw[address-base+offset:address-base+offset+size]

    functions = json.loads((image_root / 'functions.json').read_text(encoding='utf-8-sig'))
    if not isinstance(functions, list) or not functions:
        raise ValueError('Missing accepted loader functions')
    entries, words, writers = {}, {}, {}
    for function in functions:
        address = function.get('address')
        if type(address) is not int or address in entries or not function.get('chunks'):
            raise ValueError('Invalid accepted loader function identity')
        body, expected = bytearray(), {}
        for bounds in function['chunks']:
            if not isinstance(bounds, list) or len(bounds) != 2:
                raise ValueError('Invalid accepted loader chunk')
            start, end = bounds
            if type(start) is not int or type(end) is not int or start % 4 or end % 4 or start >= end:
                raise ValueError('Unaligned accepted loader chunk')
            data = original(start, end-start)
            body.extend(data)
            for site in range(start, end, 4):
                if site in expected:
                    raise ValueError('Overlapping loader chunks')
                word = int.from_bytes(data[site-start:site-start+4], 'little')
                expected[site] = word
                if _destination(word) == 28:
                    writers[site] = word
        if len(body) != function.get('size') or hashlib.sha256(body).hexdigest() != function.get('sha256'):
            raise ValueError('Accepted loader body differs from original')
        instructions = function.get('instructions')
        if not isinstance(instructions, list) or sum(len(bytes.fromhex(item.get('bytes', ''))) for item in instructions) != len(expected) * 4:
            raise ValueError('Incomplete accepted loader instructions')
        actual = {}
        for instruction in instructions:
            site, data = instruction.get('address'), bytes.fromhex(instruction.get('bytes', ''))
            if type(site) is not int or site % 4 or not data or len(data) % 4:
                raise ValueError('Invalid accepted instruction')
            for index in range(0, len(data), 4):
                if site + index in actual:
                    raise ValueError('Duplicate accepted instruction word')
                actual[site+index] = int.from_bytes(data[index:index+4], 'little')
        if actual != expected:
            raise ValueError('Accepted loader words differ from full original chunks')
        if set(words).intersection(expected):
            raise ValueError('Accepted original chunks overlap across functions')
        entries[address] = function
        words.update(expected)
    return {'config': config, 'config_path': config_path, 'raw_path': raw_path, 'raw': raw, 'base': base, 'offset': offset, 'receipt': receipt_path, 'root': image_root, 'files': names, 'entries': entries, 'words': words, 'writers': writers, 'original': original}


def validate(root, profile, evidence):
    root = Path(root).resolve()
    if evidence.get('source') != 'accepted_overlay_table_loader':
        raise ValueError('Unsupported accepted overlay table loader source')
    gp = _integer(profile.get('gp'), 'gp')
    if not gp:
        raise ValueError('Missing nonzero inherited GP')
    loader, main = _accepted(root, evidence, 'loader'), _accepted(root, evidence, 'main')
    source, parent = loader['config'], main['config']
    if source.get('image') in (parent.get('image'), profile.get('image')) or parent.get('image') == profile.get('image'):
        raise ValueError('Table loader requires three distinct images')
    source_gp = source.get('gp_evidence', {})
    if source_gp.get('source') != 'inherited_runtime_initializer' or parent.get('gp_evidence', {}).get('source') != 'runtime_initializer':
        raise ValueError('Table loader requires an accepted observed direct overlay parent')
    if _owned(root, source_gp.get('parent_config')) != main['config_path'] or _owned(root, source_gp.get('parent_original_path')) != main['raw_path'] or _owned(root, source_gp.get('parent_acceptance_receipt')) != main['receipt']:
        raise ValueError('Table loader MAIN differs from observed GP parent')
    if source.get('gp') != gp or parent.get('gp') != gp:
        raise ValueError('Table loader GP differs from accepted parents')
    import overlay_gp
    result = overlay_gp.validate(root, source, source_gp)
    initializer = result['initializer']['site']
    allowed = {site: main['words'].get(site) for site in (initializer, initializer + 4)}
    if None in allowed.values() or main['writers'] != allowed:
        raise ValueError('MAIN contains an unreviewed GP writer')
    if loader['offset'] != 0 or len(loader['raw']) % 4 or any(_destination(int.from_bytes(loader['raw'][index:index+4], 'little')) == 28 for index in range(0, len(loader['raw']), 4)):
        raise ValueError('Accepted loader raw image contains a possible GP writer')
    overlay_path = _bound(root, evidence, 'overlay_original_path')
    raw = overlay_path.read_bytes()
    loads, segments = profile.get('loads'), profile.get('segments')
    if not isinstance(loads, list) or len(loads) != 1 or _owned(root, loads[0].get('path')) != overlay_path or loads[0].get('offset', 0) != 0:
        raise ValueError('Table target requires exactly one raw original load')
    base = _integer(loads[0].get('base'), 'target base')
    if base % 4 or len(raw) % 4 or not 0x80000000 <= base < base + len(raw) <= loader['base']:
        raise ValueError('Table target load overlaps retained loader or escapes RAM')
    if not isinstance(segments, list) or len(segments) != 1 or segments[0].get('start') != base or segments[0].get('end') != base + len(raw):
        raise ValueError('Table target segment differs from exact raw load')
    if any(_destination(int.from_bytes(raw[index:index+4], 'little')) == 28 for index in range(0, len(raw), 4)):
        raise ValueError('Table target raw image has an unclassified possible GP writer')
    review_path = _bound(root, evidence, 'loader_review', 'status')
    review = json.loads(review_path.read_text(encoding='utf-8-sig'))
    if review.get('schema') != 1 or review.get('source') != evidence['source'] or review.get('reviewed_complete') is not True or review.get('runtime_observed') is not False or review.get('gp') != gp or review.get('overlay_sha256') != _digest(overlay_path) or review.get('overlay_base') != base:
        raise ValueError('Missing exact complete static table loader review')
    bindings = review.get('source_reviews')
    if not isinstance(bindings, list) or len(bindings) != 2:
        raise ValueError('Missing loader table and candidate CFG reviews')
    source_reviews = {}
    for binding in bindings:
        path = _owned(root, binding.get('path'), 'status')
        kind = binding.get('kind')
        if kind not in ('table', 'entry_cfg') or kind in source_reviews or binding.get('sha256') != _digest(path):
            raise ValueError('Stale full static loader source review')
        source_reviews[kind] = json.loads(path.read_text(encoding='utf-8-sig'))
    owners = {}
    for owner in review.get('functions', []):
        image = {'loader': loader, 'main': main}.get(owner.get('image'))
        if not image:
            raise ValueError('Foreign table route image')
        function = image['entries'].get(owner.get('address'))
        key = (owner['image'], owner.get('address'))
        if key in owners or not function or any(owner.get(field) != function.get(field) for field in ('sha256', 'chunks', 'size')):
            raise ValueError('Table route owner is not an exact accepted original')
        owners[key] = function

    def word(image_name, owner, site):
        function = owners.get((image_name, owner))
        if not function or type(site) is not int or not any(start <= site < end for start, end in function['chunks']):
            raise ValueError('Table route instruction escapes reviewed full owner')
        return {'loader': loader, 'main': main}[image_name]['words'][site]

    def sequence(role, expected):
        row = review.get(role, {})
        actual = [word(row.get('image'), row.get('owner'), row.get('site') + 4 * index) for index in range(len(expected))]
        if actual != expected:
            raise ValueError('Original table route words differ: ' + role)
        return row

    table = review.get('table', {})
    address, rows = table.get('address'), table.get('rows')
    if type(address) is not int or address % 4 or not isinstance(rows, list) or not 1 <= len(rows) <= 1024:
        raise ValueError('Invalid complete basename table')
    table_bytes = loader['original'](address, len(rows) * 4)
    if table.get('sha256') != hashlib.sha256(table_bytes).hexdigest():
        raise ValueError('Basename table hash differs')
    selected = []
    for index, row in enumerate(rows):
        pointer = int.from_bytes(table_bytes[index*4:index*4+4], 'little')
        name = row.get('basename')
        if row.get('level_index') != index or row.get('pointer') != pointer or not isinstance(name, str) or not name or not name.isascii() or not name.replace('_', '').isalnum() or loader['original'](pointer, len(name) + 1) != name.encode('ascii') + b'\0':
            raise ValueError('Original table row pointer or basename differs')
        if name + '.OVL' == overlay_path.name:
            selected.append(index)
    if not selected or review.get('selected_indices') != selected:
        raise ValueError('Selected indices do not exactly bind target file')
    table_review = source_reviews['table']
    if table_review.get('schema') != 1 or _owned(root, table_review.get('original_path')) != loader['raw_path'] or table_review.get('original_sha256') != _digest(loader['raw_path']) or table_review.get('accepted_init_load_base') != loader['base'] or table_review.get('basename_table') != address or table_review.get('rows') != rows:
        raise ValueError('Full table source review differs from accepted original')
    index_address = _integer(review.get('index_address'), 'table index address')
    sequence('index_materializer', [0x3c110000 | ((index_address+0x8000)>>16), 0x26310000 | (index_address & 0xffff)])
    sequence('mode_exclusions', [0x24020001, 0x1662001c, 0x24020003])
    sequence('mode_three_guard', [0x1662001d, 0x27b00060])
    sequence('table_selection', [0x86220000, 0, 0x00021080, 0x3c010000 | (address >> 16), 0x00220821, 0x8c260000 | (address & 0xffff)])
    formatter = _integer(review.get('format_address'), 'overlay format address')
    sequence('format_materializer', [0x3c050000 | ((formatter+0x8000)>>16), 0x24a50000 | (formatter & 0xffff)])
    if loader['original'](formatter, 7) != b'%s.OVL\0':
        raise ValueError('Original overlay filename format differs')
    sequence('destination_materializer', [0x3c050000 | ((base+0x8000)>>16), 0x24a50000 | (base & 0xffff)])
    calls = review.get('calls')
    seeds, ranges = profile.get('seeds'), profile.get('code_ranges')
    if not isinstance(seeds, list) or not seeds or len(set(seeds)) != len(seeds) or profile.get('entry') not in seeds or not isinstance(ranges, list) or len(ranges) != len(seeds) or review.get('seeds') != seeds or review.get('code_ranges') != ranges:
        raise ValueError('Missing unique bounded target seeds')
    for seed, bounds in zip(seeds, ranges):
        if type(seed) is not int or seed % 4 or not isinstance(bounds, list) or len(bounds) != 2 or bounds[0] != seed or type(bounds[1]) is not int or bounds[1] % 4 or not base <= seed < bounds[1] <= base + len(raw) or not any(raw[seed-base:bounds[1]-base]):
            raise ValueError('Invalid nonzero target code range')
        candidates = [candidate for candidate in source_reviews['entry_cfg'].get('functions', []) if candidate.get('entry') == seed and _owned(root, candidate.get('original_path')) == overlay_path]
        if len(candidates) != 1:
            raise ValueError('Missing unique fully reviewed target CFG')
        candidate = candidates[0]
        listing = _owned(root, candidate.get('listing'), 'status')
        if candidate.get('original_sha256') != _digest(overlay_path) or candidate.get('load_base_candidate') != base or candidate.get('end_exclusive_conservative') != bounds[1] or candidate.get('candidate_body_sha256') != hashlib.sha256(raw[seed-base:bounds[1]-base]).hexdigest() or candidate.get('complete_boundary_listing_reviewed') is not True or candidate.get('missing_linear_words') != [] or candidate.get('listing_sha256') != _digest(listing):
            raise ValueError('Target CFG body, listing or complete review differs')
    if not isinstance(calls, list) or not calls:
        raise ValueError('Missing exact original table route calls')
    seen, selected_callers = set(), set()
    for call in calls:
        image_name, owner, site, target = (call.get(key) for key in ('image', 'owner', 'site', 'target'))
        if (image_name, site) in seen:
            raise ValueError('Duplicate table route call')
        seen.add((image_name, site))
        opcode = word(image_name, owner, site)
        if opcode >> 26 != 3 or ((site+4)&0xf0000000) | ((opcode & 0x3ffffff)<<2) != target or word(image_name, owner, site+4) != call.get('delay_word'):
            raise ValueError('Table route JAL/delay differs from original')
        if target in seeds and image_name == 'main':
            selected_callers.add(target)
    if selected_callers != set(seeds):
        raise ValueError('Target seeds lack accepted MAIN callers')
    loader_call = review.get('loader_call')
    if not isinstance(loader_call, dict) or loader_call not in calls or loader_call.get('image') != 'loader' or loader_call.get('delay_word') != 0x24060001:
        raise ValueError('Missing exact synchronous copied-load invocation')
    sync_owner = loader_call['target']
    if ('main', sync_owner) not in owners:
        raise ValueError('Synchronous loader lacks full accepted original')
    staging = _integer(review.get('staging_destination'), 'staging destination')
    if staging % 4 or not 0x80000000 <= staging < staging + len(raw) <= 0x80200000:
        raise ValueError('Copied-load staging escapes aligned RAM')
    expected_checks = {'sync_flag': [0x327000ff, 0x16000002, 0, 0x02802821], 'copy_destination': [0x02802021, 0x3c050000 | ((staging+0x8000)>>16), 0x24a50000 | (staging & 0xffff)], 'copy_bound': [0x3c020000 | ((loader['base']+0x8000)>>16), 0x24420000 | (loader['base'] & 0xffff), 0x3c100000 | ((base+0x8000)>>16), 0x26100000 | (base & 0xffff), 0x00501023]}
    for role, words in expected_checks.items():
        row = sequence(role, words)
        if row.get('image') != 'main' or row.get('owner') != sync_owner:
            raise ValueError('Copied-load evidence belongs to foreign routine')
    semantics = review.get('semantic_contract')
    required = {'mode_exclusions', 'index_domain', 'filename_forwarding', 'copy_bounds', 'completion_before_dispatch', 'dispatch_guards', 'callee_preserved_gp'}
    if not isinstance(semantics, dict) or set(semantics) != required or any(not isinstance(value, str) or not value.strip() for value in semantics.values()):
        raise ValueError('Missing complete reviewed table loader semantics')
    return {'gp': gp, 'initializer': result['initializer'], 'inherited_from': source['image'], 'static_entries': seeds, 'observed_entries': [], 'table_indices': selected, 'loader_review_sha256': _digest(review_path)}

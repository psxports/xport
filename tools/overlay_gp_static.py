"""Validate statically reviewed overlay loaders without runtime observations"""
import hashlib
import json
from pathlib import Path

from overlay_gp import _bound, _digest, _owned


def _integer(value, label):
    if type(value) is not int or not 0 <= value <= 0xffffffff:
        raise ValueError('Invalid integer: ' + label)
    return value


def _destination(word):
    op, rs, rt, rd, fn = word >> 26, word >> 21 & 31, word >> 16 & 31, word >> 11 & 31, word & 63
    if op == 0:
        return rd if fn in (0, 2, 3, 4, 6, 7, 9, 16, 18, 32, 33, 34, 35, 36, 37, 38, 39, 42, 43) else None
    if op in (8, 9, 10, 11, 12, 13, 14, 15, 32, 33, 34, 35, 36, 37, 38):
        return rt
    if op in (16, 17, 18, 19) and rs in (0, 2):
        return rt
    return None


def validate(root, profile, evidence):
    root = Path(root).resolve()
    if evidence.get('source') != 'inherited_static_loader':
        raise ValueError('Unsupported static loader GP source')
    gp = _integer(profile.get('gp'), 'gp')
    if not gp:
        raise ValueError('Missing nonzero GP')
    parent_path = _bound(root, evidence, 'parent_config', 'tools/ida')
    parent = json.loads(parent_path.read_text(encoding='utf-8-sig'))
    if parent.get('image') == profile.get('image') or parent.get('gp_evidence', {}).get('source') != 'runtime_initializer' or parent.get('gp') != gp:
        raise ValueError('Static loader requires a distinct direct initializer parent')
    parent_raw_path = _bound(root, evidence, 'parent_original_path')
    loads = parent.get('loads', [])
    if len(loads) != 1 or _owned(root, loads[0].get('path')) != parent_raw_path:
        raise ValueError('Parent original differs from load')
    main_load = loads[0]
    raw = parent_raw_path.read_bytes()
    main_base = _integer(main_load.get('base'), 'parent base')
    main_offset = _integer(main_load.get('offset', 0), 'parent offset')

    def original(address, size):
        if type(address) is not int or type(size) is not int or size <= 0:
            raise ValueError('Invalid raw original range')
        offset = address - main_base + main_offset
        if offset < main_offset or offset + size > len(raw):
            raise ValueError('Original range escapes parent load')
        return raw[offset:offset + size]

    receipt_path = _bound(root, evidence, 'parent_acceptance_receipt', 'status/ida/accepted')
    receipt = json.loads(receipt_path.read_text(encoding='utf-8-sig'))
    rows = [row for row in receipt.get('images', []) if row.get('image') == parent.get('image')]
    if receipt.get('version') != 1 or len(rows) != 1:
        raise ValueError('Missing unique accepted parent')
    row = rows[0]
    if _owned(root, row.get('config')) != parent_path or row.get('config_sha256') != _digest(parent_path):
        raise ValueError('Parent profile is not accepted')
    destination = (root / receipt.get('destination', '')).resolve()
    image_root = (destination / parent['image']).resolve()
    if not destination.is_relative_to(root) or not image_root.is_relative_to(destination) or not row.get('files'):
        raise ValueError('Invalid accepted artifact root')
    names = set()
    for item in row['files']:
        artifact = (image_root / item['path']).resolve()
        if item['path'] in names or not artifact.is_relative_to(image_root) or not artifact.is_file() or _digest(artifact) != item.get('sha256'):
            raise ValueError('Stale or duplicate accepted parent artifact')
        names.add(item['path'])
    if 'functions.json' not in names or 'export-report.json' not in names:
        raise ValueError('Missing full accepted parent metadata')
    from startup import validate_gp_config
    parent_result = validate_gp_config(root, parent)
    initializer = parent_result['initializer']['site']
    initializer_words = (0x3c1c0000 | (gp >> 16), 0x279c0000 | (gp & 0xffff))
    if gp & 0x8000:
        initializer_words = (0x3c1c0000 | ((gp + 0x10000) >> 16), initializer_words[1])
    if original(initializer, 8) != b''.join(word.to_bytes(4, 'little') for word in initializer_words):
        raise ValueError('Parent initializer is not the reviewed LUI/ADDIU pair')
    functions = json.loads((image_root / 'functions.json').read_text(encoding='utf-8-sig'))
    if not isinstance(functions, list) or not functions:
        raise ValueError('Missing accepted original functions')
    entries, instruction_words, gp_writes = {}, {}, {}
    for function in functions:
        address = function.get('address')
        chunks = function.get('chunks')
        if type(address) is not int or address in entries or not isinstance(chunks, list) or not chunks:
            raise ValueError('Invalid accepted function identity')
        body, expected = bytearray(), {}
        for chunk in chunks:
            if not isinstance(chunk, list) or len(chunk) != 2:
                raise ValueError('Invalid original chunk')
            start, end = chunk
            if type(start) is not int or type(end) is not int or start % 4 or end % 4 or start >= end:
                raise ValueError('Unaligned original chunk')
            data = original(start, end - start)
            body.extend(data)
            for pos in range(start, end, 4):
                if pos in expected:
                    raise ValueError('Overlapping original chunks')
                word = int.from_bytes(data[pos-start:pos-start+4], 'little')
                expected[pos] = word
                if _destination(word) == 28:
                    gp_writes[pos] = word
        if len(body) != function.get('size') or hashlib.sha256(body).hexdigest() != function.get('sha256'):
            raise ValueError('Accepted function body differs from raw original')
        supplied = function.get('instructions')
        if not isinstance(supplied, list) or sum(len(bytes.fromhex(item.get('bytes', ''))) for item in supplied) != len(expected) * 4:
            raise ValueError('Incomplete accepted original instruction list')
        actual = {}
        for instruction in supplied:
            pc = instruction.get('address')
            try:
                data = bytes.fromhex(instruction.get('bytes', ''))
            except (ValueError, TypeError) as error:
                raise ValueError('Invalid original instruction bytes') from error
            if type(pc) is not int or pc % 4 or not data or len(data) % 4:
                raise ValueError('Duplicate or incomplete original instruction')
            for offset in range(0, len(data), 4):
                if pc + offset in actual:
                    raise ValueError('Duplicate original instruction word')
                actual[pc + offset] = int.from_bytes(data[offset:offset+4], 'little')
        if actual != expected:
            raise ValueError('Accepted instructions differ from raw chunks')
        entries[address] = function
        instruction_words.update(expected)
    if gp_writes != {initializer: initializer_words[0], initializer + 4: initializer_words[1]}:
        raise ValueError('Parent code contains an unreviewed GP writer')

    overlay_path = _bound(root, evidence, 'overlay_original_path')
    overlay_raw = overlay_path.read_bytes()
    overlay_loads = profile.get('loads', [])
    if len(overlay_loads) != 1 or _owned(root, overlay_loads[0].get('path')) != overlay_path or overlay_loads[0].get('offset', 0) != 0:
        raise ValueError('Static overlay requires one exact raw load')
    base = _integer(overlay_loads[0].get('base'), 'overlay base')
    if base % 4 or len(overlay_raw) % 4 or not 0x80000000 <= base < base + len(overlay_raw) <= 0x80200000:
        raise ValueError('Overlay range escapes aligned RAM')
    segments = profile.get('segments')
    if not isinstance(segments, list) or len(segments) != 1 or segments[0].get('start') != base or segments[0].get('end') != base + len(overlay_raw):
        raise ValueError('Overlay segment differs from exact raw load')
    if any(_destination(int.from_bytes(overlay_raw[pos:pos+4], 'little')) == 28 for pos in range(0, len(overlay_raw), 4)):
        raise ValueError('Overlay has a possible GP writer')
    seeds, ranges = profile.get('seeds'), profile.get('code_ranges')
    if not isinstance(seeds, list) or not seeds or len(set(seeds)) != len(seeds) or profile.get('entry') not in seeds or not isinstance(ranges, list) or len(ranges) != len(seeds):
        raise ValueError('Missing bounded unique static seeds')
    for seed, bounds in zip(seeds, ranges):
        if type(seed) is not int or seed % 4 or not isinstance(bounds, list) or len(bounds) != 2:
            raise ValueError('Invalid static seed range')
        start, end = bounds
        if type(start) is not int or type(end) is not int or start != seed or end % 4 or not base <= start < end <= base + len(overlay_raw) or not any(overlay_raw[start-base:end-base]):
            raise ValueError('Static seed range escapes nonzero original')
    review_path = _bound(root, evidence, 'loader_review', 'status')
    review = json.loads(review_path.read_text(encoding='utf-8-sig'))
    if review.get('schema') != 1 or review.get('source') != 'inherited_static_loader' or review.get('reviewed_complete') is not True or review.get('runtime_observed') is not False:
        raise ValueError('Missing completed static-only loader review')
    if review.get('parent_image') != parent['image'] or review.get('overlay_sha256') != _digest(overlay_path) or review.get('overlay_base') != base or review.get('gp') != gp or review.get('seeds') != seeds or review.get('code_ranges') != ranges:
        raise ValueError('Loader review identity differs from profile')
    source_binding = review.get('static_source_review')
    if not isinstance(source_binding, dict):
        raise ValueError('Missing full static source review binding')
    source_path = _bound(root, {'source_review': source_binding.get('path'), 'source_review_sha256': source_binding.get('sha256')}, 'source_review', 'status')
    source_review = json.loads(source_path.read_text(encoding='utf-8-sig'))
    for label, expected_path in [('input', overlay_path), ('parent_original', parent_raw_path), ('parent_profile', parent_path), ('parent_acceptance', receipt_path)]:
        binding = source_review.get(label, {})
        if _owned(root, binding.get('path')) != expected_path or binding.get('sha256') != _digest(expected_path):
            raise ValueError('Static source review identity differs: ' + label)
    source_gp = source_review.get('gp', {}).get('value')
    try:
        source_gp = int(source_gp, 0) if isinstance(source_gp, str) else source_gp
    except ValueError as error:
        raise ValueError('Invalid static source GP') from error
    if type(source_gp) is not int or source_gp != gp:
        raise ValueError('Static source review GP differs')
    source_artifacts = source_review.get('source_evidence')
    if not isinstance(source_artifacts, list) or not source_artifacts:
        raise ValueError('Missing full source review artifacts')
    source_names = set()
    for binding in source_artifacts:
        artifact = _owned(root, binding.get('path'))
        if not artifact.is_relative_to(image_root) or artifact.relative_to(image_root).as_posix() not in names or artifact in source_names or binding.get('sha256') != _digest(artifact):
            raise ValueError('Stale or foreign static source artifact')
        source_names.add(artifact)
    owners = review.get('functions')
    if not isinstance(owners, list) or not owners:
        raise ValueError('Missing full reviewed loader function bodies')
    reviewed = {}
    for owner in owners:
        address = owner.get('address')
        function = entries.get(address)
        if address in reviewed or not function or not any(any(original(start, end-start)) for start, end in function['chunks']) or owner.get('sha256') != function['sha256'] or owner.get('chunks') != function['chunks'] or owner.get('size') != function['size']:
            raise ValueError('Loader function is not an exact accepted original')
        reviewed[address] = function

    def checked(site, owner):
        if owner not in reviewed or not any(start <= site < end for start, end in reviewed[owner]['chunks']):
            raise ValueError('Route instruction escapes reviewed owner')
        return instruction_words[site]

    calls = review.get('calls')
    if not isinstance(calls, list) or not calls:
        raise ValueError('Missing exact loader call route')
    seen_calls = set()
    for call in calls:
        site, owner, target = call.get('site'), call.get('owner'), call.get('target')
        if site in seen_calls:
            raise ValueError('Duplicate loader call site')
        seen_calls.add(site)
        word = checked(site, owner)
        if word >> 26 != 3 or (((site + 4) & 0xf0000000) | ((word & 0x3ffffff) << 2)) != target:
            raise ValueError('Loader call target differs from original JAL')
        checked(site + 4, owner)
        if target not in reviewed and target not in seeds:
            raise ValueError('Loader call target is not a reviewed owner or seed')
    if any(not any(call['target'] == seed for call in calls) for seed in seeds):
        raise ValueError('Static seed has no reviewed original caller')
    constants = review.get('constants')
    if not isinstance(constants, list) or not constants:
        raise ValueError('Missing loader literal materializers')
    roles = {}
    for constant in constants:
        role, site, owner, register, value = (constant.get(key) for key in ('role', 'site', 'owner', 'register', 'value'))
        if role in roles or role not in ('filename_stem', 'overlay_format', 'disc_format', 'destination', 'capacity') or type(register) is not int or not 1 <= register <= 31:
            raise ValueError('Invalid or duplicate loader role')
        first, second = checked(site, owner), checked(site + 4, owner)
        if first >> 26 != 15 or first >> 16 & 31 != register or second >> 26 != 9 or second >> 21 & 31 != register or second >> 16 & 31 != register:
            raise ValueError('Loader literal is not an adjacent LUI/ADDIU pair')
        decoded = ((first & 0xffff) << 16) + ((second & 0xffff) - (0x10000 if second & 0x8000 else 0))
        if decoded & 0xffffffff != value:
            raise ValueError('Loader literal value differs from original')
        roles[role] = value
        if role in ('filename_stem', 'overlay_format', 'disc_format'):
            literal = constant.get('literal')
            if not isinstance(literal, str) or not literal or '\0' in literal or original(value, len(literal.encode('ascii')) + 1) != literal.encode('ascii') + b'\0':
                raise ValueError('Loader string differs from original')
            roles[role] = literal
    if set(roles) != {'filename_stem', 'overlay_format', 'disc_format', 'destination', 'capacity'}:
        raise ValueError('Incomplete filename and destination loader roles')
    if roles['overlay_format'] != '%s.OVL' or roles['disc_format'] != '\\BIN\\%s;1' or roles['filename_stem'] + '.OVL' != overlay_path.name or roles['destination'] != base or roles['capacity'] < ((len(overlay_raw) + 2047) & ~2047) or base + roles['capacity'] > 0x80200000:
        raise ValueError('Loader roles do not select the exact overlay asset and load')
    semantics = review.get('semantic_contract')
    required = {'filename_forwarding', 'destination_forwarding', 'completion_before_dispatch', 'cache_residency', 'fallback_same_identity', 'callee_preserved_gp'}
    if not isinstance(semantics, dict) or set(semantics) != required or any(not isinstance(value, str) or not value.strip() for value in semantics.values()):
        raise ValueError('Missing complete reviewed loader semantics')
    return {'gp': gp, 'initializer': parent_result['initializer'], 'inherited_from': parent['image'], 'static_entries': seeds, 'observed_entries': [], 'loader_review_sha256': _digest(review_path)}

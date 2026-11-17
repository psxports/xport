"""Validate reviewed resource overlays entered through fixed game task records"""
import hashlib
import json
from pathlib import Path

from overlay_gp import _bound, _digest, _owned
from overlay_gp_static import _destination, _integer


def _target(site, word):
    if word >> 26 != 3:
        raise ValueError('Thread resource route requires an original JAL')
    return ((site + 4) & 0xf0000000) | ((word & 0x3ffffff) << 2)


def _pair(high, low, first, second):
    if high >> 16 != first or low >> 16 != second:
        raise ValueError('Thread resource address materializer differs')
    return (((high & 0xffff) << 16) + ((low & 0xffff) - (0x10000 if low & 0x8000 else 0))) & 0xffffffff


def prove_task_carrier(word, roles):
    getter, initialize = roles['gp_getter'], roles['initialize']
    scheduler, open_thread = roles['scheduler'], roles['open_thread']
    if (word(getter), word(getter + 4)) != (0x03e00008, 0x03801021):
        raise ValueError('Thread GP getter is not JR RA / MOVE V0,GP')
    if tuple(word(open_thread + n * 4) for n in range(3)) != (0x240a00b0, 0x01400008, 0x2409000e):
        raise ValueError('Thread creation is not the BIOS OpenTh selector')
    task_base = _pair(word(initialize + 0x24), word(initialize + 0x2c), 0x3c02, 0x2450)
    if not 0x80000000 <= task_base <= 0x80200000 - 3 * 128:
        raise ValueError('Task GP records escape cached main RAM')
    fixed_init = {0x10: 0x00009021, 0x34: 0xa6000004, 0x50: 0xac530094,
                  0x54: 0xae020048, 0x58: 0x2a420003, 0x5c: 0x1440fff5, 0x60: 0x26100080}
    if any(word(initialize + offset) != value for offset, value in fixed_init.items()):
        raise ValueError('Three-record GP capture loop differs')
    if _target(initialize + 0x4c, word(initialize + 0x4c)) != getter:
        raise ValueError('Task initializer calls another GP producer')
    scheduler_state = _pair(word(scheduler + 4), word(scheduler + 0xc), 0x3c02, 0x2451)
    if scheduler_state != task_base + 4:
        raise ValueError('Scheduler and initializer use different task records')
    fixed_scheduler = {0x2c: 0x26300008, 0x7c: 0x8e04fffc, 0x80: 0x8e050008,
                       0x84: 0x8e06003c, 0x8c: 0, 0xcc: 0x26100080, 0xd0: 0x26310080}
    if any(word(scheduler + offset) != value for offset, value in fixed_scheduler.items()):
        raise ValueError('OpenTh entry/stack/GP carrier or task stride differs')
    if _target(scheduler + 0x88, word(scheduler + 0x88)) != open_thread:
        raise ValueError('Scheduler does not call the reviewed BIOS thread entry')
    return task_base


def validate(root, profile, evidence):
    import startup

    root = Path(root).resolve()
    if evidence.get('source') != 'inherited_thread_resource_loader':
        raise ValueError('Unsupported thread resource GP evidence')
    parent_path = _bound(root, evidence, 'parent_config', 'tools/ida')
    parent = json.loads(parent_path.read_text(encoding='utf-8-sig'))
    if (parent.get('image') == profile.get('image') or
            parent.get('gp_evidence', {}).get('source') not in ('runtime_initializer', 'entry_runtime_initializer')):
        raise ValueError('Thread overlay requires a distinct direct-initializer parent')
    inherited = startup.validate_gp_config(root, parent)
    if not inherited['gp'] or profile.get('gp') != inherited['gp']:
        raise ValueError('Thread overlay GP differs from parent initializer')
    raw_path = _bound(root, evidence, 'parent_original_path')
    if len(parent.get('loads', [])) != 1 or _owned(root, parent['loads'][0]['path']) != raw_path:
        raise ValueError('Thread parent original load differs')
    load = parent['loads'][0]
    raw = raw_path.read_bytes()[load.get('offset', 0):]
    parent_base = load['base']

    def data(address, size):
        if type(address) is not int or type(size) is not int or size <= 0:
            raise ValueError('Invalid thread parent byte range')
        offset = address - parent_base
        if offset < 0 or offset + size > len(raw):
            raise ValueError('Thread parent evidence escapes original')
        return raw[offset:offset + size]

    def word(address):
        if address % 4:
            raise ValueError('Unaligned thread parent word')
        return int.from_bytes(data(address, 4), 'little')

    receipt_path = _bound(root, evidence, 'parent_acceptance_receipt', 'status/ida/accepted')
    receipt = json.loads(receipt_path.read_text(encoding='utf-8-sig'))
    accepted = [row for row in receipt.get('images', []) if row.get('image') == parent['image']]
    if receipt.get('version') != 1 or len(accepted) != 1:
        raise ValueError('Missing unique accepted thread parent')
    accepted = accepted[0]
    image_root = root / receipt['destination'] / parent['image']
    manifest = startup.verify_ida_image(root, image_root.parent, parent_path.parent, parent['image'])
    if (manifest['files'] != accepted.get('files') or manifest['config_sha256'] != accepted.get('config_sha256') or
            _owned(root, accepted['config']) != parent_path):
        raise ValueError('Thread parent acceptance is stale')
    functions = json.loads((image_root / 'functions.json').read_text(encoding='utf-8-sig'))
    by_address = {row['address']: row for row in functions}
    if len(by_address) != len(functions):
        raise ValueError('Duplicate accepted thread parent function')
    first = min(start for row in functions for start, end in row['chunks'])
    last = max(end for row in functions for start, end in row['chunks'])
    writers = {site: word(site) for site in range(first, last, 4) if _destination(word(site)) == 28}
    initializer = inherited['initializer']['site']
    if writers != {initializer: word(initializer), initializer + 4: word(initializer + 4)}:
        raise ValueError('Accepted parent text or its gaps contain another GP writer')

    review_path = _bound(root, evidence, 'loader_review', 'status')
    review = json.loads(review_path.read_text(encoding='utf-8-sig'))
    if (review.get('schema') != 1 or review.get('reviewed_complete') is not True or
            review.get('runtime_observed') is not False or review.get('source') != evidence['source'] or
            review.get('parent_image') != parent['image'] or review.get('gp') != inherited['gp']):
        raise ValueError('Missing complete static thread resource review')
    required = {'gp_capture', 'task_layout', 'thread_creation', 'resource_selection',
                'completed_load', 'entry_restart', 'code_data_partition', 'context_lifetime'}
    semantics = review.get('semantic_contract', {})
    if set(semantics) != required or any(not isinstance(x, str) or not x.strip() for x in semantics.values()):
        raise ValueError('Incomplete thread resource semantic review')
    roles = {}
    for role, binding in review['functions'].items():
        address = _integer(binding.get('address'), 'thread owner')
        original = by_address.get(address)
        if not original or any(binding.get(k) != original[k] for k in ('sha256', 'chunks', 'size')):
            raise ValueError('Thread route owner is not an accepted original')
        body = b''.join(data(a, z - a) for a, z in original['chunks'])
        if hashlib.sha256(body).hexdigest() != original['sha256']:
            raise ValueError('Thread owner differs from original bytes')
        roles[role] = address
    required_roles = {'initialize', 'scheduler', 'gp_getter', 'open_thread', 'resource_loader',
                      'wait', 'capacity', 'queue', 'dispatcher', 'restart'}
    if not required_roles <= set(roles):
        raise ValueError('Missing thread resource owner roles')
    sizes = {'initialize': 0xb0, 'scheduler': 0x100, 'gp_getter': 8, 'open_thread': 12,
             'resource_loader': 0x58, 'wait': 0x50, 'restart': 0x50}
    for role, size in sizes.items():
        address = roles[role]
        if by_address[address]['chunks'] != [[address, address + size]]:
            raise ValueError('Thread carrier requires its complete contiguous owner form')
    task_base = prove_task_carrier(word, roles)
    loader, wait, restart = roles['resource_loader'], roles['wait'], roles['restart']
    fixed_loader = {0x10: 0x00048040, 0x14: 0x02048021, 0x18: 0x00108080,
                    0x1c: 0x02028021, 0x24: 0x8e040000, 0x28: 0x8e050004,
                    0x38: 0, 0x3c: 0x8e040008, 0x44: 0}
    if any(word(loader + offset) != value for offset, value in fixed_loader.items()):
        raise ValueError('Resource record/entry forwarding differs')
    table = _pair(word(loader + 4), word(loader + 8), 0x3c02, 0x2442)
    destination_pointer = _pair(word(loader + 0x2c), word(loader + 0x30), 0x3c06, 0x8cc6)
    if (_target(loader + 0x34, word(loader + 0x34)) != wait or
            _target(loader + 0x40, word(loader + 0x40)) != restart or
            _target(wait + 0x10, word(wait + 0x10)) != roles['queue'] or
            _target(wait + 0x18, word(wait + 0x18)) != roles['capacity'] or
            word(wait + 0x1c) != 0x24040040 or
            word(restart + 0x10) != 0x24020002 or word(restart + 0x18) != 0xa6020000 or
            word(restart + 0x20) != 0xae040004):
        raise ValueError('Resource wait/restart route differs')

    overlay = _bound(root, evidence, 'overlay_original_path')
    original = overlay.read_bytes()
    loads = profile.get('loads', [])
    if (len(loads) != 1 or _owned(root, loads[0]['path']) != overlay or loads[0].get('offset', 0) != 0 or
            review.get('overlay_sha256') != _digest(overlay)):
        raise ValueError('Thread overlay raw identity differs')
    base = loads[0]['base']
    if not 0x80000000 <= base < base + len(original) <= 0x80200000 or word(destination_pointer) != base:
        raise ValueError('Thread overlay destination differs from resource loader')
    if base < task_base + 3 * 128 and base + len(original) > task_base:
        raise ValueError('Resource load overwrites its task GP records')
    extraction_path = _bound(root, review, 'extraction', 'status')
    extraction = json.loads(extraction_path.read_text(encoding='utf-8-sig'))
    files = [f for disc in extraction['discs'] for f in disc['files'] if (root / f['path']).resolve() == overlay]
    if len(files) != 1 or files[0]['sha256'] != _digest(overlay) or files[0]['bytes'] != len(original):
        raise ValueError('Thread overlay is not the unchanged extracted file')
    selected_entries = []
    selected_indices = []
    for route in review.get('resource_calls', []):
        owner = roles.get(route['owner'])
        if owner is None or not any(a <= route['site'] < route['site'] + 8 <= z for a, z in by_address[owner]['chunks']):
            raise ValueError('Resource selector escapes reviewed caller')
        index = route['index']
        if type(index) is not int or not 0 <= index <= 32767:
            raise ValueError('Invalid resource selector')
        if _target(route['site'], word(route['site'])) != loader or word(route['site'] + 4) != (0x24040000 | index):
            raise ValueError('Original caller selects another resource')
        selected_indices.append(index)
    from overlay_gp_thread_selector import validate_selectors
    additional_indices, selector_proofs = validate_selectors(root, profile, evidence, review, loader)
    selected_indices.extend(additional_indices)
    for index in selected_indices:
        location, count, entry = (word(table + 12 * index + n * 4) for n in range(3))
        msf = list(data(location, 3))
        if any(x >> 4 > 9 or x & 15 > 9 for x in msf):
            raise ValueError('Invalid original resource BCD position')
        m, s, f = [(x >> 4) * 10 + (x & 15) for x in msf]
        if s >= 60 or f >= 75 or (m * 60 + s) * 75 + f - 150 != files[0]['lba'] or count != (len(original) + 2047) // 2048:
            raise ValueError('Resource record selects another file extent')
        selected_entries.append(entry)
    if not selected_entries or profile.get('entry') not in selected_entries:
        raise ValueError('Overlay entry has no reviewed original resource route')

    segments, cursor = profile.get('segments', []), base
    for segment in sorted(segments, key=lambda x: x['start']):
        a, z = segment['start'], segment['end']
        if a != cursor or not a < z <= base + len(original) or a % 4 or segment.get('class') not in ('CODE', 'DATA') or segment['class'] == 'CODE' and z % 4:
            raise ValueError('Thread overlay code/data partition is invalid')
        cursor = z
    if cursor != base + len(original):
        raise ValueError('Thread overlay partition omits bytes')
    ranges = [[x['start'], x['end']] for x in segments if x['class'] == 'CODE']
    if not ranges or profile.get('code_ranges') != ranges or review.get('code_ranges') != ranges:
        raise ValueError('Thread overlay CODE ranges differ from review')
    seeds = profile.get('seeds', [])
    if (not seeds or len(seeds) != len(set(seeds)) or review.get('seeds') != seeds or
            profile['entry'] not in seeds or any(type(s) is not int or s % 4 or not any(a <= s < z for a, z in ranges) for s in seeds)):
        raise ValueError('Thread overlay seed escapes reviewed CODE')
    candidates = {x['address']: x for x in review.get('noncode_gp_candidates', [])}
    if len(candidates) != len(review.get('noncode_gp_candidates', [])):
        raise ValueError('Duplicate thread overlay DATA GP candidate')
    found = set()
    for offset in range(0, len(original) - 3, 4):
        value = int.from_bytes(original[offset:offset + 4], 'little')
        if _destination(value) != 28:
            continue
        address = base + offset
        row = candidates.get(address, {})
        if (row.get('word') != value or not isinstance(row.get('reason'), str) or not row['reason'].strip() or
                not any(x['class'] == 'DATA' and x['start'] <= address < address + 4 <= x['end'] for x in segments)):
            raise ValueError('Thread overlay contains an unreviewed GP writer')
        found.add(address)
    if found != set(candidates):
        raise ValueError('Thread DATA GP candidates differ from original bytes')
    return {'gp': inherited['gp'], 'initializer': inherited['initializer'], 'inherited_from': parent['image'],
            'task_base': task_base, 'record_gp_offset': 72, 'static_entries': selected_entries,
            'observed_entries': [], 'loader_review_sha256': _digest(review_path),
            'overlay_selector_proofs': selector_proofs,
            'scope': 'Original resource/thread carrier and CODE GP preservation; no native thread or gameplay verification'}

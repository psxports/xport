"""Validate a reviewed zero-fill to image-qualified coverage migration"""
import copy
import hashlib
import json
from pathlib import Path
import struct


def digest(data):
    return hashlib.sha256(data).hexdigest()


def bound(root, item, prefix='status'):
    value = item.get('path')
    if not isinstance(value, str) or not value:
        raise ValueError('Missing bound path')
    path = (root / value).resolve()
    if not path.is_relative_to(root / prefix) or not path.is_file():
        raise ValueError('Foreign or missing migration input')
    if digest(path.read_bytes()) != item.get('sha256'):
        raise ValueError('Stale migration input hash')
    return path


def accepted_image(root, project, image, receipt_binding):
    receipt_path = bound(root, receipt_binding, 'status/ida/accepted')
    receipt = json.loads(receipt_path.read_text(encoding='utf-8-sig'))
    destination = (root / project['paths']['ida_exports']).resolve()
    if not destination.is_relative_to(root) or (root / receipt.get('destination', '')).resolve() != destination or receipt.get('version') != 1:
        raise ValueError('Acceptance receipt differs from selected exports')
    matches = [row for row in receipt.get('images', []) if row.get('image') == image]
    if len(matches) != 1:
        raise ValueError('Missing or ambiguous accepted migration image')
    manifest = matches[0]
    profile_path = bound(root, {'path': manifest.get('config'), 'sha256': manifest.get('config_sha256')}, 'tools/ida')
    profile = json.loads(profile_path.read_text(encoding='utf-8-sig'))
    if profile.get('image') != image:
        raise ValueError('Accepted profile has foreign image')
    from startup import validate_gp_config
    validate_gp_config(root, profile)
    image_root = destination / image
    names = set()
    for artifact in manifest.get('files', []):
        path = (image_root / artifact['path']).resolve()
        if artifact['path'] in names or not path.is_relative_to(image_root) or not path.is_file() or digest(path.read_bytes()) != artifact.get('sha256'):
            raise ValueError('Stale or ambiguous accepted image artifact')
        names.add(artifact['path'])
    if not {'functions.json', 'export-report.json'} <= names:
        raise ValueError('Incomplete accepted image metadata')
    functions = json.loads((image_root / 'functions.json').read_text(encoding='utf-8-sig'))
    index = {}
    for row in functions:
        if row['address'] in index:
            raise ValueError('Duplicate accepted function entry')
        index[row['address']] = row
    return profile, index, names


def original_body(root, profile, function):
    loads = profile.get('loads', [])
    if len(loads) != 1:
        raise ValueError('Migration requires one exact original load')
    load = loads[0]
    path = (root / load['path']).resolve()
    if not path.is_relative_to(root) or not path.is_file():
        raise ValueError('Foreign original function input')
    raw = path.read_bytes()
    result = bytearray()
    for start, end in function['chunks']:
        lo = load.get('offset', 0) + start - load['base']
        hi = load.get('offset', 0) + end - load['base']
        if start % 4 or end % 4 or not 0 <= lo < hi <= len(raw):
            raise ValueError('Function range escapes original image')
        result.extend(raw[lo:hi])
    if len(result) != function['size'] or digest(result) != function['sha256']:
        raise ValueError('Original function bytes differ from accepted metadata')
    return bytes(result)


def validate(root, project, request):
    root = Path(root).resolve()
    if request.get('schema') != 1 or request.get('contract') != 'xport-coverage-image-migration-v1':
        raise ValueError('Unsupported coverage image migration request')
    state_path = bound(root, request['state'])
    state = json.loads(state_path.read_text(encoding='utf-8-sig'))
    if any(row.get('proof_status') == 'proved' or row.get('translation_status') for row in state['functions']):
        raise ValueError('Image migration requires zero accepted proofs and translations')
    if state.get('image_migrations'):
        raise ValueError('Existing image migration cannot be replaced')
    binary_path = bound(root, request['functions'])
    raw = binary_path.read_bytes()
    if len(raw) % 4 or not raw:
        raise ValueError('Invalid immutable recorded function list')
    addresses = list(struct.unpack('<%dI' % (len(raw) // 4), raw))
    if len(addresses) != len(set(addresses)) or addresses != [row['address'] for row in state['functions']]:
        raise ValueError('Migration must preserve complete recorded order and denominator')
    if digest(raw) != state.get('functions_sha256') or digest(raw) != state.get('source_recording_sha256'):
        raise ValueError('State differs from immutable capture')
    recording_path = bound(root, request['recording_receipt'])
    recording = json.loads(recording_path.read_text(encoding='utf-8-sig'))
    if recording.get('contract') != 'xport-function-coverage-v1' or recording.get('status') != 'complete' or recording.get('image') != state.get('image') or recording.get('unique_functions') != len(addresses) or (root / recording.get('binary_list', '')).resolve() != binary_path:
        raise ValueError('Recording receipt differs from capture')
    packet_paths = {path.resolve() for path in state_path.parent.glob('coverage-batch-*.json') if not path.name.endswith('.audit.json')}
    supplied_packets = {bound(root, row) for row in request.get('packets', [])}
    if supplied_packets != packet_paths or len(supplied_packets) != len(request.get('packets', [])):
        raise ValueError('Migration must bind every preserved packet exactly once')
    if not state.get('active_packet') or (root / state['active_packet']).resolve() not in supplied_packets:
        raise ValueError('Migration requires a preserved active packet')
    review_path = bound(root, request['coldboot_review'])
    review = json.loads(review_path.read_text(encoding='utf-8-sig'))
    if review.get('user_facts', {}).get('capture_started_without_save_state') is not True:
        raise ValueError('Missing reviewed coldboot provenance')
    intended = review.get('conclusion', {}).get('supported_intended_mapping', {})
    old_profile, old_functions, _ = accepted_image(root, project, state['image'], request['old_acceptance'])
    zero = set()
    for row in state['functions']:
        old = old_functions.get(row['address'])
        if old is None or old['sha256'] != row['original_sha256']:
            raise ValueError('Old recorded identity differs from accepted original')
        if not any(original_body(root, old_profile, old)):
            zero.add(row['address'])
    mappings = request.get('mappings', [])
    if not mappings or len({row.get('address') for row in mappings}) != len(mappings) or {row.get('address') for row in mappings} != zero:
        raise ValueError('Migration must replace exactly all recorded zero-fill identities')
    observed_path = bound(root, request['observation'])
    observation = json.loads(observed_path.read_text(encoding='utf-8-sig'))
    result = copy.deepcopy(state)
    result_rows = {row['address']: row for row in result['functions']}
    changes = []
    images = {}
    for mapping in mappings:
        image = mapping['image']
        address = mapping['address']
        if image == state['image'] or f'{address:08X}' not in intended.get(image, []):
            raise ValueError('Image mapping lacks reviewed static coldboot identity')
        if image not in images:
            images[image] = accepted_image(root, project, image, mapping['acceptance'])
        profile, functions, names = images[image]
        function = functions.get(address)
        if function is None or function['sha256'] != mapping.get('function_sha256'):
            raise ValueError('Migration target differs from accepted function')
        for field in ['pseudocode']:
            if function.get(field) and function[field] not in names:
                raise ValueError('Target lacks complete accepted artifact')
        body = original_body(root, profile, function)
        if not any(body):
            raise ValueError('Real overlay function cannot be zero fill')
        events = [event for event in observation.get('events', []) if event.get('pc') == address and event.get('image') == image and event.get('load_base') == profile['loads'][0]['base']]
        if len(events) != 1:
            raise ValueError('Missing or ambiguous observed target identity')
        event = events[0]
        snapshot = (observed_path.parent / event.get('snapshot', '')).resolve()
        if not snapshot.is_relative_to(observed_path.parent) or not snapshot.is_file() or digest(snapshot.read_bytes()) != event.get('bytes_sha256'):
            raise ValueError('Stale observed target snapshot')
        captured = snapshot.read_bytes()
        if event.get('size') != len(captured) or len(function['chunks']) != 1 or function['chunks'][0][0] != address or len(captured) < len(body) or captured[:len(body)] != body:
            raise ValueError('Observed full actual function differs from accepted body')
        if profile.get('gp_evidence', {}).get('runtime_observation_sha256') != digest(observed_path.read_bytes()):
            raise ValueError('Target GP acceptance differs from migration observation')
        old = result_rows[address]
        changes.append({'address': address, 'old_image': state['image'], 'old_function_sha256': old['original_sha256'], 'image': image, 'function_sha256': function['sha256'], 'size': function['size'], 'observation_sequence': event['sequence']})
        old.update({'image': image, 'original_sha256': function['sha256'], 'name': function['name']})
    active = json.loads((root / state['active_packet']).read_text(encoding='utf-8-sig'))
    active_addresses = {row['address'] for row in active['source_context']['audit_contract']['required_functions']}
    if active_addresses & zero:
        raise ValueError('Current packet already contains migrated identities; explicit packet rebuild required')
    receipt = {'schema': 1, 'contract': request['contract'], 'request': copy.deepcopy(request), 'changes': changes, 'original_capture_unchanged': True, 'separate_coldboot_not_original_replay': True, 'proved_functions_reused': 0, 'state_before_sha256': digest(state_path.read_bytes()), 'active_packet_before_sha256': digest((root / state['active_packet']).read_bytes())}
    return result, receipt

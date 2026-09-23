"""Content-verified original trace cache with explicit capture provenance"""
import hashlib
import json
from pathlib import Path
import re
import uuid

SCHEMA = 'ff-original-cache-v1'
GROUPS = ('game', 'emulator', 'settings', 'initial_state', 'scenario', 'collector')


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'),
                      ensure_ascii=True, allow_nan=False).encode('ascii')


def digest(path):
    from trace_hash_session import digest as session_digest
    return session_digest(path)


def fingerprint(files):
    if not files:
        raise ValueError('Empty provenance group')
    return {name: digest(path) for name, path in sorted(files.items())}


def validate_identity(identity):
    if set(identity) != set(GROUPS) | {'capture'}:
        raise ValueError('Missing or unknown identity group')
    for group in GROUPS:
        values = identity[group]
        if not isinstance(values, dict) or not values:
            raise ValueError('Empty identity group: '+group)
        for name, value in values.items():
            if not isinstance(name, str) or not name or not isinstance(value, str) or not re.fullmatch('[0-9a-f]{64}', value):
                raise ValueError('Invalid fingerprint: '+group)
    capture = identity['capture']
    if set(capture) != {'schema', 'channels', 'start_tick', 'end_tick', 'options'}:
        raise ValueError('Incomplete capture contract')
    if not isinstance(capture['schema'], str) or not capture['schema']:
        raise ValueError('Missing trace schema')
    channels = capture['channels']
    if not isinstance(channels, list) or not channels or any(not isinstance(x, str) or not re.fullmatch('[a-z][a-z0-9_-]*', x) for x in channels):
        raise ValueError('Invalid capture channels')
    if len(set(channels)) != len(channels):
        raise ValueError('Duplicate capture channel')
    start, end = capture['start_tick'], capture['end_tick']
    if type(start) is not int or type(end) is not int or not 0 <= start < end:
        raise ValueError('Invalid tick range')
    if not isinstance(capture['options'], dict):
        raise ValueError('Missing effective capture options')
    canonical(identity)


def identity_key(identity):
    validate_identity(identity)
    return hashlib.sha256(canonical(identity)).hexdigest()


def publish(cache, identity, artifacts, validator, current_identity):
    """Publish only after capture validation and rechecking its original inputs"""
    key = identity_key(identity)
    if current_identity() != identity:
        raise ValueError('Capture inputs changed during recording')
    if set(artifacts) != set(identity['capture']['channels']):
        raise ValueError('Missing or unexpected artifact channel')
    paths = {name: Path(path).resolve() for name, path in artifacts.items()}
    before = {name: {'path': str(path), 'size': path.stat().st_size,
                     'sha256': digest(path)} for name, path in paths.items()}
    validation = validator(identity, paths)
    if validation.get('complete') is not True:
        raise ValueError('Capture is incomplete')
    for name, path in paths.items():
        if path.stat().st_size != before[name]['size'] or digest(path) != before[name]['sha256']:
            raise ValueError('Artifact changed during validation')
    if current_identity() != identity:
        raise ValueError('Capture inputs changed during validation')
    document = {'schema': SCHEMA, 'key': key, 'identity': identity,
                'artifacts': before, 'validation': validation}
    cache = Path(cache)
    cache.mkdir(parents=True, exist_ok=True)
    destination = cache/(key+'.json')
    temporary = cache/(key+'.'+uuid.uuid4().hex+'.tmp')
    try:
        temporary.write_bytes(canonical(document)+b'\n')
        # Windows rename refuses an existing destination, preserving prior evidence
        if destination.exists():
            raise FileExistsError(destination)
        temporary.rename(destination)
    finally:
        temporary.unlink(missing_ok=True)
    return destination


def lookup(cache, identity, validator):
    key = identity_key(identity)
    path = Path(cache)/(key+'.json')
    try:
        document = json.loads(path.read_text(encoding='utf-8'))
        if document['schema'] != SCHEMA or document['key'] != key or document['identity'] != identity:
            raise ValueError('Cache identity mismatch')
        artifacts = document['artifacts']
        if set(artifacts) != set(identity['capture']['channels']):
            raise ValueError('Incomplete artifact set')
        paths = {}
        for name, artifact in artifacts.items():
            source = Path(artifact['path'])
            if source.stat().st_size != artifact['size'] or digest(source) != artifact['sha256']:
                raise ValueError('Artifact hash mismatch: '+name)
            paths[name] = source
        validation = validator(identity, paths)
        if validation.get('complete') is not True:
            raise ValueError('Capture validation failed')
        return {'hit': True, 'key': key, 'artifacts': paths, 'validation': validation}
    except (OSError, ValueError, KeyError, TypeError) as error:
        return {'hit': False, 'key': key, 'reason': str(error)}

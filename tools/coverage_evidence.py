"""Validate a reviewed export refresh without changing original function identities"""

import hashlib
import json
from pathlib import Path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_refresh(root, project, state, receipt_path, old_bindings, bindings, contexts):
    if any(row.get('proof_status') == 'proved' for row in state['functions']):
        raise ValueError('Export refresh requires zero accepted proofs')
    root = root.resolve()
    path = (root / receipt_path).resolve()
    if not path.is_relative_to(root / 'status/ida/accepted') or not path.is_file():
        raise ValueError('Export refresh requires an owned IDA acceptance receipt')
    receipt = json.loads(path.read_text(encoding='utf-8-sig'))
    destination = root / project['paths']['ida_exports']
    if receipt.get('version') != 1 or (root / receipt.get('destination', '')).resolve() != destination.resolve():
        raise ValueError('Export refresh receipt does not bind the selected export root')
    old = [(row['image'], row['address'], row['function_sha256'], row['machine_audit_sha256']) for row in old_bindings]
    new = [(row['image'], row['address'], row['function_sha256'], row['machine_audit_sha256']) for row in bindings]
    if old != new:
        raise ValueError('Export refresh changed original bytes, machine checks or batch order')
    images = {item['image']: item for item in receipt.get('images', [])}
    for context in contexts:
        image = context['image']
        manifest = images.get(image)
        if manifest is None:
            raise ValueError('Export refresh image is absent from acceptance receipt')
        config = (root / manifest['config']).resolve()
        if digest(config) != manifest['config_sha256']:
            raise ValueError('Export refresh has stale accepted configuration')
        profile = json.loads(config.read_text(encoding='utf-8-sig'))
        from startup import validate_gp_config
        validate_gp_config(root, profile)
        files = {item['path']: item for item in manifest['files']}
        image_root = (destination / image).resolve()
        for name in ('functions.json', 'export-report.json'):
            if name not in files or digest(image_root / name) != files[name]['sha256']:
                raise ValueError('Export refresh metadata changed after acceptance')
        for field in ('mips', 'pseudocode'):
            evidence = context[field]
            if not evidence.get('path'):
                continue
            artifact = (root / evidence['path']).resolve()
            if not artifact.is_relative_to(image_root):
                raise ValueError('Export refresh evidence escapes accepted image')
            name = artifact.relative_to(image_root).as_posix()
            if name not in files or digest(artifact) != files[name]['sha256'] or evidence['sha256'] != files[name]['sha256']:
                raise ValueError('Export refresh artifact differs from accepted evidence')
    reviews = [context['pseudocode']['review'] for context in contexts if context['pseudocode'].get('review')]
    return {'receipt': path.relative_to(root).as_posix(), 'receipt_sha256': digest(path),
            'destination': destination.relative_to(root).as_posix(), 'accepted_proofs_reused': False,
            **({'pseudocode_reviews': reviews} if reviews else {})}

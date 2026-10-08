"""Validate an overlay GP inherited from accepted parent machine evidence"""
import hashlib
import json
from pathlib import Path


def _digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _owned(root, value, prefix=None):
    if not isinstance(value, str) or not value:
        raise ValueError('Missing owned evidence path')
    path = (root / value).resolve()
    if not path.is_relative_to(root) or (prefix and not path.is_relative_to(root / prefix)) or not path.is_file():
        raise ValueError('Foreign or missing evidence path')
    return path


def _bound(root, evidence, field, prefix=None):
    path = _owned(root, evidence.get(field), prefix)
    if evidence.get(field + '_sha256') != _digest(path):
        raise ValueError('Stale evidence hash: ' + field)
    return path


def validate(root, profile, evidence):
    root = Path(root).resolve()
    if evidence.get('source') == 'inherited_static_loader':
        from overlay_gp_static import validate as validate_static_loader
        return validate_static_loader(root, profile, evidence)
    if evidence.get('source') == 'accepted_overlay_profile':
        return validate_supplement(root, profile, evidence)
    if evidence.get('source') != 'inherited_runtime_initializer':
        raise ValueError('Unsupported inherited GP evidence')
    gp = profile.get('gp')
    if type(gp) is not int or not 0 < gp <= 0xffffffff:
        raise ValueError('Overlay requires nonzero integer GP')
    parent_path = _bound(root, evidence, 'parent_config', 'tools/ida')
    parent = json.loads(parent_path.read_text(encoding='utf-8-sig'))
    if parent.get('image') == profile.get('image') or parent.get('gp_evidence', {}).get('source') != 'runtime_initializer':
        raise ValueError('Inherited GP requires a distinct direct parent initializer')
    if parent.get('gp') != gp:
        raise ValueError('Parent and overlay GP differ')
    parent_original = _bound(root, evidence, 'parent_original_path')
    loads = parent.get('loads', [])
    if len(loads) != 1 or _owned(root, loads[0].get('path')) != parent_original:
        raise ValueError('Parent original differs from configured load')
    receipt_path = _bound(root, evidence, 'parent_acceptance_receipt', 'status/ida/accepted')
    receipt = json.loads(receipt_path.read_text(encoding='utf-8-sig'))
    matches = [row for row in receipt.get('images', []) if row.get('image') == parent.get('image')]
    if receipt.get('version') != 1 or len(matches) != 1:
        raise ValueError('Missing accepted parent image')
    row = matches[0]
    if _owned(root, row.get('config')) != parent_path or row.get('config_sha256') != _digest(parent_path):
        raise ValueError('Parent config is not accepted')
    destination = (root / receipt.get('destination', '')).resolve()
    if not destination.is_relative_to(root) or not row.get('files'):
        raise ValueError('Missing accepted parent files')
    for item in row['files']:
        artifact = (destination / parent['image'] / item['path']).resolve()
        if not artifact.is_relative_to(destination / parent['image']) or not artifact.is_file() or _digest(artifact) != item.get('sha256'):
            raise ValueError('Stale accepted parent artifact')
    from startup import validate_gp_config
    parent_result = validate_gp_config(root, parent)
    overlay_loads = profile.get('loads', [])
    if len(overlay_loads) != 1:
        raise ValueError('Overlay requires exactly one load')
    load = overlay_loads[0]
    overlay = _bound(root, evidence, 'overlay_original_path')
    if _owned(root, load.get('path')) != overlay or load.get('offset', 0) != 0:
        raise ValueError('Overlay original differs from raw configured load')
    base = load.get('base')
    size = overlay.stat().st_size
    if type(base) is not int or not 0x80000000 <= base < base + size <= 0x80200000:
        raise ValueError('Overlay load escapes PS1 RAM')
    seeds = profile.get('seeds')
    if not isinstance(seeds, list) or not seeds or any(type(seed) is not int or seed % 4 or not base <= seed < base + size for seed in seeds):
        raise ValueError('Overlay seeds escape raw load')
    if profile.get('entry') not in seeds:
        raise ValueError('Overlay entry is not an observed seed')
    observation_path = _bound(root, evidence, 'runtime_observation', 'status')
    observation = json.loads(observation_path.read_text(encoding='utf-8-sig'))
    if observation.get('schema') != 1 or not observation.get('scenario') or not observation.get('owner'):
        raise ValueError('Missing runtime observation provenance')
    provenance_path = _bound(root, evidence, 'runtime_provenance', 'status')
    provenance = json.loads(provenance_path.read_text(encoding='utf-8-sig'))
    owner = observation['owner']
    runtime = provenance.get('runtime_receipt', {})
    if provenance.get('schema') != 1 or provenance.get('pid') != owner.get('Id') or runtime.get('pid') != owner.get('Id') or runtime.get('executable') != owner.get('Path'):
        raise ValueError('Runtime owner differs from observation provenance')
    if not isinstance(runtime.get('created'), int) or runtime['created'] <= 0:
        raise ValueError('Missing runtime process creation identity')
    bios = _bound(root, evidence, 'bios_path')
    if provenance.get('bios_sha256') != _digest(bios):
        raise ValueError('BIOS differs from observation provenance')
    artifacts = {item.get('name'): item.get('sha256') for item in provenance.get('artifacts', [])}
    if artifacts.get(observation_path.name) != _digest(observation_path):
        raise ValueError('Observation differs from provenance artifact')
    observed = []
    for seed in seeds:
        matches = [event for event in observation.get('events', []) if event.get('image') == profile.get('image') and event.get('pc') == seed and event.get('load_base') == base]
        if len(matches) != 1:
            raise ValueError('Missing or ambiguous overlay entry observation')
        event = matches[0]
        try:
            observed_gp = int(event['registers']['r28'], 0)
        except (KeyError, TypeError, ValueError):
            raise ValueError('Missing observed overlay GP') from None
        if observed_gp != gp:
            raise ValueError('Observed overlay GP differs')
        snapshot = _owned(root, str(observation_path.parent.relative_to(root) / event.get('snapshot', '')), 'status')
        if not snapshot.is_relative_to(observation_path.parent):
            raise ValueError('Observed snapshot escapes event directory')
        if _digest(snapshot) != event.get('bytes_sha256'):
            raise ValueError('Stale observed entry bytes')
        snapshot_bytes = snapshot.read_bytes()
        if len(snapshot_bytes) < 4 or event.get('size') != len(snapshot_bytes):
            raise ValueError('Incomplete observed entry snapshot')
        if artifacts.get(snapshot.name) != _digest(snapshot):
            raise ValueError('Snapshot differs from provenance artifact')
        raw = overlay.read_bytes()[seed-base:seed-base+len(snapshot_bytes)]
        if len(raw) == 0 or snapshot.read_bytes()[:min(32, len(raw))] != raw[:min(32, len(raw))]:
            raise ValueError('Observed entry does not match overlay')
        observed.append(seed)
    return {'gp': gp, 'initializer': parent_result['initializer'], 'inherited_from': parent['image'], 'observed_entries': observed}


def validate_supplement(root, profile, evidence):
    """Bind static supplemental seeds to an accepted observed overlay"""
    root = Path(root).resolve()
    source_path = _bound(root, evidence, 'overlay_config', 'tools/ida')
    source = json.loads(source_path.read_text(encoding='utf-8-sig'))
    if source.get('image') == profile.get('image') or source.get('gp_evidence', {}).get('source') != 'inherited_runtime_initializer':
        raise ValueError('Supplement requires a distinct directly observed overlay')
    receipt_path = _bound(root, evidence, 'overlay_acceptance_receipt', 'status/ida/accepted')
    receipt = json.loads(receipt_path.read_text(encoding='utf-8-sig'))
    rows = [row for row in receipt.get('images', []) if row.get('image') == source.get('image')]
    if receipt.get('version') != 1 or len(rows) != 1:
        raise ValueError('Missing accepted overlay image')
    row = rows[0]
    if _owned(root, row.get('config')) != source_path or row.get('config_sha256') != _digest(source_path):
        raise ValueError('Overlay source config is not accepted')
    destination = (root / receipt.get('destination', '')).resolve()
    image_root = (destination / source['image']).resolve()
    if not destination.is_relative_to(root) or not image_root.is_relative_to(destination) or not row.get('files'):
        raise ValueError('Missing accepted overlay artifacts')
    for item in row['files']:
        artifact = (image_root / item['path']).resolve()
        if not artifact.is_relative_to(image_root) or not artifact.is_file() or _digest(artifact) != item.get('sha256'):
            raise ValueError('Stale accepted overlay artifact')
    if type(profile.get('gp')) is not int:
        raise ValueError('Supplement requires integer GP')
    for field in ('gp', 'loads', 'segments', 'external_symbols'):
        if profile.get(field) != source.get(field):
            raise ValueError('Supplement changes overlay identity: ' + field)
    result = validate(root, source, source['gp_evidence'])
    load = profile['loads'][0]
    raw = _owned(root, load['path']).read_bytes()
    base = load['base']
    seeds = profile.get('seeds')
    ranges = profile.get('code_ranges')
    if not isinstance(seeds, list) or not seeds or len(set(seeds)) != len(seeds) or profile.get('entry') not in seeds:
        raise ValueError('Missing unique supplemental seeds')
    if not isinstance(ranges, list) or len(ranges) != len(seeds):
        raise ValueError('Supplement requires a bounded range per seed')
    for seed, bounds in zip(seeds, ranges):
        if type(seed) is not int or seed % 4 or not isinstance(bounds, list) or len(bounds) != 2:
            raise ValueError('Invalid supplemental seed range')
        start, end = bounds
        if type(start) is not int or type(end) is not int or start != seed or end % 4 or not base <= start < end <= base + len(raw):
            raise ValueError('Supplemental range escapes original load')
        if not any(raw[start-base:end-base]):
            raise ValueError('Supplemental seed contains only zero filler')
    return {'gp': result['gp'], 'initializer': result['initializer'], 'inherited_from': source['image'], 'static_entries': seeds, 'observed_entries': []}

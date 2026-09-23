"""Inventory file inputs for a user capture without claiming live settings verification"""
import hashlib
import json
from pathlib import Path
import re

from trace_cache import digest


def disc_files(disc):
    disc = disc.resolve()
    paths = {disc}
    if disc.suffix.lower() == '.cue':
        for line in disc.read_text(encoding='utf-8-sig').splitlines():
            if not re.match(r'^\s*FILE\s', line, re.I):
                continue
            match = re.fullmatch(r'\s*FILE\s+(?:"([^"]+)"|(\S+))\s+\S+\s*', line, re.I)
            if not match:
                raise ValueError('Unsupported CUE FILE directive')
            paths.add((disc.parent/(match[1] or match[2])).resolve())
    for path in paths:
        if not path.is_file():
            raise ValueError('Missing disc component: '+str(path))
    return paths


def inventory(runtime, disc, state, tools, installation=None):
    runtime = runtime.resolve()
    installation = Path(installation).resolve() if installation is not None else runtime
    groups = {'disc': disc_files(disc), 'source_state': {state.resolve()},
              'runtime': set(installation.glob('*.dll')) | set(installation.glob('*.exe')),
              'configuration': {runtime/'settings.ini'}, 'tools': {p.resolve() for p in tools}}
    for folder, group in [('QtPlugins', 'runtime'), ('resources', 'runtime'), ('bios', 'bios'),
                          ('gamesettings', 'configuration'), ('inputprofiles', 'configuration'),
                          ('cheats', 'game_modifications'), ('patches', 'game_modifications'),
                          ('subchannels', 'game_modifications'), ('memcards', 'initial_memory_cards')]:
        base = installation if group == 'runtime' else runtime
        groups.setdefault(group, set()).update(p for p in (base/folder).rglob('*') if p.is_file())
    if (runtime/'xport-trace-profile.ini').is_file():
        groups['configuration'].add(runtime/'xport-trace-profile.ini')
    files = {group: [{'path': str(p.resolve()), 'size': p.stat().st_size, 'sha256': digest(p)}
                     for p in sorted(paths)] for group, paths in groups.items()}
    encoded = json.dumps(files, sort_keys=True, separators=(',', ':')).encode()
    return {'schema': 1, 'files': files, 'inventory_sha256': hashlib.sha256(encoded).hexdigest(),
            'effective_settings_verified': False, 'cache_eligible': False,
            'scope': 'File inputs at recording start; live effective settings and mid-recording changes require separate evidence'}


def verify_files(manifest):
    changed = []
    for group, files in manifest['files'].items():
        for row in files:
            path = Path(row['path'])
            if not path.is_file() or digest(path) != row['sha256']:
                changed.append({'group': group, 'path': row['path']})
    return {'listed_files_unchanged': not changed, 'changed': changed,
            'scope': 'Listed file hashes only; additions and effective live configuration are not inferred'}

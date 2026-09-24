"""Plan and complete low-context upgrades to the current shared Xport revision"""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

from xport_project import artifact_path, load_project


REVISION_PATTERN = r'\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z'
MARKER_RE = re.compile(r'^[ \t]*// XPORT REVISION: ('+REVISION_PATTERN+r')[ \t]*$', re.MULTILINE)
MAIN_RE = re.compile(r'^[ \t]*int[ \t]+xport_main[ \t]*\(', re.MULTILINE)


def validate_revision(value):
    if not re.fullmatch(REVISION_PATTERN, value or ''):
        raise ValueError('Invalid Xport revision: '+str(value))
    datetime.datetime.fromisoformat(value.replace('Z', '+00:00'))
    return value


def parse_changelog(text):
    matches = list(re.finditer(r'^## ('+REVISION_PATTERN+r')[ \t]*$', text, re.MULTILINE))
    if not matches:
        raise ValueError('CHANGELOG.md has no Xport revisions')
    entries = []
    for index, match in enumerate(matches):
        revision = validate_revision(match.group(1))
        end = matches[index+1].start() if index+1 < len(matches) else len(text)
        fields = {}
        for key, value in re.findall(r'^- ([A-Za-z]+):[ \t]*(.*)$', text[match.end():end], re.MULTILINE):
            fields[key.lower()] = value.strip()
        missing = {'scope', 'compatibility', 'changed', 'upgrade', 'automation', 'verify'}-fields.keys()
        if missing:
            raise ValueError('Changelog revision '+revision+' is missing: '+', '.join(sorted(missing)))
        entries.append(dict(revision=revision, **fields))
    revisions = [entry['revision'] for entry in entries]
    if revisions != sorted(revisions, reverse=True) or len(revisions) != len(set(revisions)):
        raise ValueError('Changelog revisions must be unique and newest first')
    return entries


def shared_changelog():
    path = Path(__file__).resolve().parent.parent/'CHANGELOG.md'
    raw = path.read_text(encoding='utf-8-sig')
    return path, raw, parse_changelog(raw)


def sources(root):
    folder = root/'src'
    return sorted((*folder.rglob('*.c'), *folder.rglob('*.cpp'))) if folder.is_dir() else []


def marker_state(root):
    definitions = []
    markers = []
    for path in sources(root):
        text = path.read_text(encoding='utf-8-sig')
        definitions.extend((path, match.start()) for match in MAIN_RE.finditer(text))
        markers.extend((path, match.group(1), match.start(), match.end()) for match in MARKER_RE.finditer(text))
    if len(definitions) != 1:
        raise ValueError('Expected exactly one project-owned xport_main definition, found '+str(len(definitions)))
    if len(markers) > 1:
        raise ValueError('Expected at most one XPORT REVISION marker, found '+str(len(markers)))
    path, position = definitions[0]
    if markers:
        marker_path, revision, start, end = markers[0]
        if marker_path != path or marker_path.read_text(encoding='utf-8-sig')[end:position].strip():
            raise ValueError('XPORT REVISION must be directly before xport_main')
        return dict(path=path, revision=validate_revision(revision), marker=(start, end), main=position)
    return dict(path=path, revision=None, marker=None, main=position)


def set_marker(state, revision):
    revision = validate_revision(revision)
    path = state['path']
    text = path.read_text(encoding='utf-8-sig')
    marker = '// XPORT REVISION: '+revision
    if state['marker']:
        start, end = state['marker']
        updated = text[:start]+marker+text[end:]
    else:
        updated = text[:state['main']]+marker+'\n'+text[state['main']:]
    path.write_text(updated, encoding='utf-8', newline='')


def complete_marker(root, revision):
    set_marker(marker_state(root), revision)


def select_entries(entries, current, target):
    validate_revision(target)
    known = {entry['revision'] for entry in entries}
    if target not in known:
        raise ValueError('Target revision is not present in CHANGELOG.md: '+target)
    if current and current not in known:
        raise ValueError('Project revision is not present in CHANGELOG.md: '+current)
    if current and current > target:
        raise ValueError('Project revision is newer than requested target')
    return [entry for entry in reversed(entries) if (not current or entry['revision'] > current) and entry['revision'] <= target]


def split_commands(value):
    commands = []
    for part in value.split(';'):
        words = part.strip().split()
        if not words:
            continue
        if len(words) < 2 or words[0] != 'X' or not re.fullmatch(r'[A-Za-z0-9_]+', words[1]):
            raise ValueError('Verify commands must use `X TOOL [ARGS]`: '+part.strip())
        commands.append(words[1:])
    return commands


def migration_script(value):
    if value in ('none', 'marker-only'):
        return None
    prefix = 'script:'
    if not value.startswith(prefix):
        raise ValueError('Unsupported Automation value: '+value)
    shared = Path(__file__).resolve().parent.parent
    path = (shared/value[len(prefix):].strip()).resolve()
    if not path.is_relative_to(shared/'tools/migrations') or not path.is_file() or path.suffix != '.py':
        raise ValueError('Migration script must be a Python file under tools/migrations')
    return path


def objective(project, current, target, pending):
    changes = '; '.join(entry['revision']+': '+entry['upgrade'] for entry in pending)
    return ('Upgrade the Xport consumer project '+project['name']+' from '+(current or 'an unversioned baseline')+
            ' to '+target+'. Apply only the pending CHANGELOG entries: '+changes+
            '. Preserve project-specific behavior and unrelated user changes, use the shared upgrade automation first, '
            'complete any explicitly reported manual actions, run the declared verification commands, and update the '
            'XPORT REVISION marker only through the upgrade command after all checks pass.')


def receipt_path():
    path = artifact_path('status/xport-upgrade/current.json')
    path.parent.mkdir(parents=True, exist_ok=True)
    return path


def compact_entries(entries):
    return [{key: entry[key] for key in ('revision', 'scope', 'compatibility', 'changed', 'upgrade', 'automation', 'verify')}
            for entry in entries]


def run_automation(root, pending, receipt):
    completed = set(receipt.get('automation_completed', []))
    results = receipt.setdefault('automation_results', [])
    for entry in pending:
        if entry['revision'] in completed:
            continue
        script = migration_script(entry['automation'])
        if script:
            process = subprocess.run([sys.executable, '-B', str(script), '--project', str(root), '--apply'],
                                     cwd=root, capture_output=True, text=True, errors='replace')
            result = dict(revision=entry['revision'], script=str(script), exit_code=process.returncode,
                          output=(process.stdout+process.stderr)[-4000:])
            results.append(result)
            if process.returncode:
                raise RuntimeError('Migration failed for '+entry['revision']+': '+result['output'])
        completed.add(entry['revision'])
        receipt['automation_completed'] = sorted(completed)
        receipt_path().write_text(json.dumps(receipt, indent=2)+'\n', encoding='utf-8')
    return receipt


def verify(root, pending):
    commands = []
    seen = set()
    for entry in pending:
        for command in split_commands(entry['verify']):
            key = tuple(command)
            if key not in seen:
                commands.append(command);seen.add(key)
    results = []
    launcher = Path(__file__).with_name('xport.py')
    for command in commands:
        process = subprocess.run([sys.executable, '-B', str(launcher), '--project', str(root), *command],
                                 cwd=root, capture_output=True, text=True, errors='replace')
        result = dict(command=['X', *command], exit_code=process.returncode,
                      output=(process.stdout+process.stderr)[-4000:])
        results.append(result)
        if process.returncode:
            raise RuntimeError('Verification failed: '+' '.join(result['command'])+'\n'+result['output'])
    return results


def plan(root, project, requested_target=None):
    changelog_path, raw, entries = shared_changelog()
    state = marker_state(root)
    target = validate_revision(requested_target) if requested_target else entries[0]['revision']
    pending = select_entries(entries, state['revision'], target)
    manual = [entry for entry in pending if entry['automation'] == 'none' and entry['upgrade'].lower() != 'none']
    return dict(schema=1, project=project['name'], project_root=str(root), changelog=str(changelog_path),
                changelog_sha256=hashlib.sha256(raw.encode()).hexdigest(), current_revision=state['revision'],
                target_revision=target, marker_path=str(state['path']), pending=compact_entries(pending),
                manual_revisions=[entry['revision'] for entry in manual],
                goal_objective=objective(project, state['revision'], target, pending) if pending else None), state, pending


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--apply', action='store_true', help='Run safe declared automation')
    mode.add_argument('--complete', action='store_true', help='Verify the upgrade and advance the marker')
    parser.add_argument('--target', help='UTC changelog revision; defaults to latest')
    args = parser.parse_args()
    root, project = load_project()
    try:
        document, state, pending = plan(root, project, args.target)
        if not pending:
            result = dict(status='current', **document)
        elif not args.apply and not args.complete:
            result = dict(status='goal_required', next_command='X upgrade --apply', **document)
        else:
            receipt = dict(schema=1, project=str(root), current_revision=state['revision'],
                           target_revision=document['target_revision'], changelog_sha256=document['changelog_sha256'],
                           automation_completed=[], automation_results=[])
            path = receipt_path()
            if path.exists():
                prior = json.loads(path.read_text(encoding='utf-8-sig'))
                identity = ('project', 'current_revision', 'target_revision', 'changelog_sha256')
                if all(prior.get(key) == receipt.get(key) for key in identity):
                    receipt = prior
            receipt = run_automation(root, pending, receipt)
            manual = [entry for entry in pending if entry['automation'] == 'none' and entry['upgrade'].lower() != 'none']
            if manual and not args.complete:
                result = dict(status='needs_agent', next_command='X upgrade --complete', **document)
            else:
                checks = verify(root, pending)
                complete_marker(root, document['target_revision'])
                receipt.update(status='complete', verification=checks)
                path.write_text(json.dumps(receipt, indent=2)+'\n', encoding='utf-8')
                result = dict(status='complete', verification=checks, **document)
    except (ValueError, OSError, RuntimeError, subprocess.SubprocessError) as error:
        result = dict(status='attention_required', error=str(error))
    print(json.dumps(result, separators=(',', ':')))
    return 1 if result['status'] == 'attention_required' else 0


if __name__ == '__main__':
    raise SystemExit(main())

"""Create the standard GitHub labels and startup tasks without duplicates"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
from urllib.parse import quote
from windows_desktop import run_hidden_desktop
from xport_process import host_executable


PROFILE = Path(__file__).resolve().parent/'templates/project/github.json'


class GitHubAPI:
    def __init__(self):
        self.executable = host_executable('gh', 'github-cli/bin/gh.exe', os.environ.get('XPORT_GH'))

    def __call__(self, endpoint, method='GET', payload=None):
        args = [self.executable, 'api', '--hostname', 'github.com', endpoint, '--method', method]
        if payload is not None:
            args += ['--input', '-']
        try:
            result = run_hidden_desktop(args, input=json.dumps(payload) if payload is not None else None,
                                    capture_output=True, text=True, encoding='utf-8', timeout=60)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise ValueError('STARTUP_GITHUB_REQUEST_FAILED: verify GitHub CLI and connection; retry the same step') from error
        if result.returncode:
            # Keep credentials and response bodies out of startup diagnostics
            raise ValueError('STARTUP_GITHUB_REQUEST_FAILED: '+method+' '+endpoint+
                             '; verify auth status using '+self.executable+', repository existence and Issues write access, then retry')
        return json.loads(result.stdout) if result.stdout.strip() else None


def pages(api, endpoint):
    result = []
    page = 1
    while True:
        separator = '&' if '?' in endpoint else '?'
        batch = api(endpoint+separator+'per_page=100&page='+str(page))
        result.extend(batch)
        if len(batch) < 100:
            return result
        page += 1


def sync_project(root, api=None):
    root = Path(root).resolve()
    config = json.loads((root/'xport-project.json').read_text(encoding='utf-8-sig'))
    repository = config.get('github', {}).get('repository')
    if not isinstance(repository, str) or not re.fullmatch(r'psxports/[A-Za-z0-9_.-]+-xport', repository):
        raise ValueError('STARTUP_GITHUB_REPOSITORY: set github.repository to the explicit psxports/NAME-xport repository')
    profile = json.loads(PROFILE.read_text(encoding='utf-8-sig'))
    api = api or GitHubAPI()
    base = 'repos/'+repository
    info = api(base)
    if info['full_name'].lower() != repository.lower() or not info.get('has_issues'):
        raise ValueError('STARTUP_GITHUB_REPOSITORY: repository identity mismatch or Issues disabled')
    existing_labels = {label['name']: label for label in pages(api, base+'/labels')}
    all_issues = pages(api, base+'/issues?state=all')
    desired_labels = {label['name'] for label in profile['labels']}
    by_title = {}
    for issue in all_issues:
        if 'pull_request' not in issue:
            by_title.setdefault(issue['title'], []).append(issue)
    for task in profile['issues']:
        matches = by_title.get(task['title'], [])
        if len(matches) > 1:
            raise ValueError('STARTUP_GITHUB_DUPLICATE_TITLE: '+task['title'])
        if matches and (task['label'] not in {label['name'] for label in matches[0]['labels']} or
                        (matches[0].get('type') or {}).get('name') != profile['type']):
            raise ValueError('STARTUP_GITHUB_TASK_CONFLICT: '+task['title'])
    for label in profile['labels']:
        current = existing_labels.get(label['name'])
        if current is None:
            api(base+'/labels', 'POST', label)
        elif any((current.get(key) or '') != label[key] for key in ('color', 'description')):
            api(base+'/labels/'+quote(label['name'], safe=''), 'PATCH', label)
    extras = sorted(set(existing_labels)-desired_labels)
    for name in extras:
        api(base+'/labels/'+quote(name, safe=''), 'DELETE')
    tasks = []
    for task in profile['issues']:
        matches = by_title.get(task['title'], [])
        issue = matches[0] if matches else api(base+'/issues', 'POST', {
            'title': task['title'], 'labels': [task['label']], 'type': profile['type'], 'assignees': []})
        if not matches and (issue['state'] != 'open' or issue.get('assignees') or
                            (issue.get('type') or {}).get('name') != profile['type'] or
                            task['label'] not in {label['name'] for label in issue['labels']}):
            raise ValueError('STARTUP_GITHUB_CREATE_MISMATCH: '+task['title'])
        tasks.append({'number': issue['number'], 'title': task['title'], 'label': task['label'],
                      'url': issue['html_url'], 'created': not bool(matches)})
    return {'repository': repository, 'branch': info.get('default_branch'),
            'labels': sorted(desired_labels), 'removed_labels': extras, 'issues': tasks}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project', type=Path, required=True)
    args = parser.parse_args()
    try:
        print(json.dumps(sync_project(args.project), indent=2))
    except (ValueError, OSError) as error:
        parser.exit(1, str(error)+'\n')


if __name__ == '__main__':
    main()

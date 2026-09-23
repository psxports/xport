"""Prepare and run project-configured IDA exports into project-owned artifacts"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
from xport_project import load_project, project_path, artifact_path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ida', type=Path, required=True, help='Absolute path to licensed idat executable')
    parser.add_argument('--image', action='append', help='Configured image name; repeat for multiple images')
    parser.add_argument('--label', required=True, help='Unique export run label')
    parser.add_argument('--export-existing', action='store_true')
    parser.add_argument('--plan', action='store_true', help='Write reviewable configs without launching IDA')
    args = parser.parse_args()
    import re
    if not re.fullmatch('[A-Za-z0-9_-]{1,64}', args.label):
        parser.error('Invalid label')
    root, config = load_project()
    configs = project_path('ida_configs', 'tools/ida')
    images = args.image or config.get('analysis', {}).get('images')
    if not images:
        parser.error('Specify --image or analysis.images; game images are never inferred')
    run = artifact_path('status/ida/runs/'+args.label)
    run.mkdir(parents=True, exist_ok=False)
    commands = []
    for image in images:
        if Path(image).name != image or not re.fullmatch('[A-Za-z0-9_.-]+', image):
            parser.error('Invalid image name')
        task = json.loads((configs/(image+'.json')).read_text(encoding='utf-8-sig'))
        task['output'] = str(run/image)
        task['rebuild'] = not args.export_existing
        for load in task['loads']:
            load['path'] = str((root/load['path']).resolve())
        if task.get('external_symbols'):
            task['external_symbols'] = str((root/task['external_symbols']).resolve())
        task_path = run/(image+'.json')
        task_path.write_text(json.dumps(task, indent=2)+'\n', encoding='utf-8')
        database = project_path('ida_databases', 'tools/ida/databases')/(image+'.idb')
        script = Path(__file__).with_name('ida_analyze_export.py')
        command = [str(args.ida), '-A', '-L'+str(run/(image+'.log')),
                   '-S"'+str(script)+'" "'+str(task_path)+'"']
        if database.exists():
            if not args.plan:
                import shutil
                shutil.copy2(database, run/(image+'.idb'))
            command.append(str(run/(image+'.idb')))
        else:
            if args.export_existing:
                raise ValueError('Existing IDA database missing: '+str(database))
            command.extend(['-c', '-pmipsl', '-o'+str(run/(image+'.idb')), task['loads'][-1]['path']])
        commands.append(command)
    (run/'commands.json').write_text(json.dumps(commands, indent=2)+'\n', encoding='utf-8')
    if not args.plan:
        for command in commands:
            subprocess.run(command, check=True, cwd=root)
    print(json.dumps({'run': str(run), 'planned': args.plan, 'images': images,
                      'next': 'Audit export reports and canonicalize this directory before selecting it for SQL import'}))


if __name__ == '__main__':
    main()

"""Run the shared toolset with an explicit project context"""
import argparse
import json
import os
from pathlib import Path
import runpy
import sys
from xport_project import load_project


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project', help='Project root or xport-project.json')
    parser.add_argument('tool', help='Shared script name or doctor')
    parser.add_argument('arguments', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    root, config = load_project(args.project)
    os.environ['XPORT_PROJECT'] = str(root)
    if args.tool == 'doctor':
        print(json.dumps({'project': str(root), 'name': config['name'],
                          'toolset': str(Path(__file__).resolve().parent),
                          'paths': config.get('paths', {})}, indent=2))
        return
    if Path(args.tool).name != args.tool:
        parser.error('Use a tool name, not a path')
    script = Path(__file__).resolve().parent/(args.tool.removesuffix('.py')+'.py')
    if not script.is_file() or script.name == 'xport.py':
        parser.error('Unknown shared tool: '+args.tool)
    os.chdir(root)
    sys.argv = [str(script), *args.arguments]
    runpy.run_path(str(script), run_name='__main__')


if __name__ == '__main__':
    main()

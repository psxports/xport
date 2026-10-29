"""Run the shared toolset with an explicit project context"""
import argparse
import json
import os
from pathlib import Path
import runpy
import sys
from xport_project import load_project
from xport_process import normalize_current_environment


def main():
    normalize_current_environment()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project', help='Project root or xport-project.json')
    parser.add_argument('tool', help='Shared script name or doctor')
    parser.add_argument('arguments', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    tool = args.tool.removesuffix('.py')
    tool_arguments = list(args.arguments)
    if tool == 'startup':
        parser.error('Run startup.py directly because a new project has no xport-project.json')
    removed = {'project_init', 'ida_accept', 'ghidra_recognize'}
    if tool in removed:
        parser.error('This startup-only stage is owned by startup.py')
    root, config = load_project(args.project)
    os.environ['XPORT_PROJECT'] = str(root)
    if args.tool == 'doctor':
        print(json.dumps({'project': str(root), 'name': config['name'],
                          'short_name': config.get('short_name'),
                          'toolset': str(Path(__file__).resolve().parent),
                          'paths': config.get('paths', {})}, indent=2))
        return
    if tool == 'stage_sources':
        if tool_arguments:
            parser.error('stage_sources takes no arguments')
        from xport_project import stage_sources
        print(json.dumps(stage_sources(root)))
        return
    if tool in ('refactor', 'enhance'):
        if tool_arguments:
            parser.error('This agent task takes its scope from the user request and current project context')
        document = 'docs/REFACTOR.md' if tool == 'refactor' else 'docs/ENHANCE.md'
        print(json.dumps({'command': tool, 'project': str(root), 'document': document,
                          'goal_required': True,
                          'action': 'Agent must read the complete contract, analyze the project, create/reuse its covering goal and execute all required stages',
                          'terminal': 'Complete implementation and required final verification; a plan or helper response is not completion'}))
        return
    if Path(args.tool).name != args.tool:
        parser.error('Use a tool name, not a path')
    script = Path(__file__).resolve().parent/(tool+'.py')
    if not script.is_file() or script.name == 'xport.py':
        parser.error('Unknown shared tool: '+args.tool)
    os.chdir(root)
    sys.argv = [str(script), *tool_arguments]
    runpy.run_path(str(script), run_name='__main__')


if __name__ == '__main__':
    main()

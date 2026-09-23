"""Resolve explicit project configuration independently of the shared tool location"""
import json
import os
from pathlib import Path

CONFIG_NAME = 'xport-project.json'


def load_project(root=None):
    explicit = root or os.environ.get('XPORT_PROJECT')
    if explicit:
        candidate = Path(explicit).expanduser().resolve()
        config = candidate if candidate.is_file() else candidate/CONFIG_NAME
    else:
        here = Path.cwd().resolve()
        config = next((p/CONFIG_NAME for p in (here, *here.parents)
                       if (p/CONFIG_NAME).is_file()), None)
        if config is None:
            raise ValueError('Specify --project or XPORT_PROJECT, or run inside a configured project')
    document = json.loads(config.read_text(encoding='utf-8-sig'))
    if document.get('schema') != 1 or not document.get('name'):
        raise ValueError('Unsupported project configuration')
    return config.parent, document


def project_root():
    return load_project()[0]


def project_path(key, default=None):
    root, config = load_project()
    value = config.get('paths', {}).get(key, default)
    if value is None:
        raise ValueError('Missing project path: '+key)
    path = Path(value)
    return path.resolve() if path.is_absolute() else (root/path).resolve()


def artifact_path(relative):
    root = project_root()
    path = (root/relative).resolve()
    if not any(path.is_relative_to(root/base) for base in ('status', 'tools')):
        raise ValueError('Generated artifacts must remain in project status or tools')
    return path


def activate_adapters():
    import sys
    root, config = load_project()
    for relative in config.get('adapters', {}).get('python_paths', ['tools']):
        directory = (root/relative).resolve()
        if not directory.is_relative_to(root):
            raise ValueError('Project adapters must live inside the project')
        if str(directory) not in sys.path:
            sys.path.append(str(directory))


def implementation_path(name):
    import importlib
    return Path(importlib.import_module(name.removesuffix('.py')).__file__).resolve()

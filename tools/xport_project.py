"""Resolve explicit project configuration independently of the shared tool location"""
from contextlib import contextmanager
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import uuid

CONFIG_NAME = 'xport-project.json'


def stage_sources(root):
    """Stage only the user-selected source and root presentation paths"""
    from xport_process import normalized_environment
    root = Path(root).resolve()
    def git(*args):
        return subprocess.check_output(['git', '-C', str(root), *args],
                                       env=normalized_environment(), text=True,
                                       stderr=subprocess.PIPE,
                                       creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    if git('rev-parse', '--is-inside-work-tree').strip() != 'true' or git('rev-parse', '--show-prefix').strip():
        raise ValueError('Source staging requires the project itself to be the Git root')
    paths = [name for name in ('src', 'cover.png', 'README.md')
             if (root / name).exists() or git('ls-files', '--', name).strip()]
    if paths:
        git('add', '--all', '--force', '--', *paths)
    staged = git('diff', '--cached', '--name-only', '--', *paths).splitlines() if paths else []
    return {'paths': paths, 'staged_files': len(staged), 'commit': False, 'push': False}


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name+'.'+uuid.uuid4().hex+'.tmp')
    temporary.write_text(json.dumps(value, indent=2)+'\n', encoding='utf-8')
    os.replace(temporary, path)


@contextmanager
def allocation_lock(workspace):
    if os.name != 'nt':
        yield
        return
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateMutexW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.LPCWSTR]
    kernel.CreateMutexW.restype = wintypes.HANDLE
    kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.ReleaseMutex.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    name = 'Local\\xport-ports-'+hashlib.sha256(str(Path(workspace).resolve()).casefold().encode()).hexdigest()
    handle = kernel.CreateMutexW(None, False, name)
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    acquired = False
    try:
        result = kernel.WaitForSingleObject(handle, 30000)
        if result not in (0, 0x80):
            raise RuntimeError('Xport port allocation is busy; retry without changing reservations')
        acquired = True
        yield
    finally:
        if acquired:
            kernel.ReleaseMutex(handle)
        kernel.CloseHandle(handle)


def reservations(workspace):
    used = {}
    for path in sorted(Path(workspace).glob('*/'+CONFIG_NAME)):
        config = json.loads(path.read_text(encoding='utf-8-sig'))
        settings = config.get('duckstation', {})
        ports = settings.get('reserved_gdb_ports', {})
        values = list(ports.values())
        if len(set(values)) != len(values):
            raise ValueError('Duplicate runtime role ports: '+str(path))
        if 'gdb_port' in settings:
            values.append(settings['gdb_port'])
        for port in set(values):
            if type(port) is not int or not 1024 <= port <= 65535:
                raise ValueError('Invalid reserved GDB port: '+str(path))
            if port in used:
                raise ValueError('Conflicting GDB port '+str(port)+': '+str(used[port])+' and '+str(path))
            used[port] = path
    return used


def player_count(config, default=2):
    value = config.get('players', default)
    if type(value) is not int or value < 1:
        raise ValueError('players must be a positive integer')
    return value


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
    short_name = document.get('short_name')
    reserved = {'CON', 'PRN', 'AUX', 'NUL', *(f'COM{i}' for i in range(1, 10)), *(f'LPT{i}' for i in range(1, 10))}
    if short_name is not None and (not re.fullmatch(r'[A-Za-z][A-Za-z0-9_]{0,15}', short_name) or short_name.upper() in reserved):
        raise ValueError('Invalid configured short_name')
    player_count(document)
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


def native_executable_path(configuration='Release'):
    root, config = load_project()
    paths = config.get('paths', {})
    if configuration.casefold() == 'debug':
        value = paths.get('native_debug_executable')
        if value is None:
            release = Path(paths['native_executable'])
            value = str(release.with_name(release.stem+'_debug'+release.suffix))
    else:
        value = paths['native_executable']
    path = Path(value)
    return path.resolve() if path.is_absolute() else (root/path).resolve()


def artifact_path(relative):
    root = project_root()
    path = (root/relative).resolve()
    if not any(path.is_relative_to(root/base) for base in ('status', 'tools', '_build')):
        raise ValueError('Generated artifacts must remain in project status, tools or _build')
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

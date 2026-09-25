"""Resolve explicit project configuration independently of the shared tool location"""
from contextlib import contextmanager
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import re

CONFIG_NAME = 'xport-project.json'


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

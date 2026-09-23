"""Initialize a standard project with workspace-unique GDB endpoint reservations"""
import argparse
from contextlib import contextmanager
import ctypes
from ctypes import wintypes
import hashlib
import json
from pathlib import Path
import socket
from xport_project import CONFIG_NAME


@contextmanager
def allocation_lock(workspace):
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateMutexW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.LPCWSTR]
    kernel.CreateMutexW.restype = wintypes.HANDLE
    kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.ReleaseMutex.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    name = 'Local\\xport-ports-'+hashlib.sha256(str(workspace.resolve()).casefold().encode()).hexdigest()
    handle = kernel.CreateMutexW(None, False, name)
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    acquired = False
    try:
        result = kernel.WaitForSingleObject(handle, 30000)
        if result not in (0, 0x80):
            raise RuntimeError('Project initialization is busy; retry without changing reservations')
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


def initialize(root, name, first_port=2400):
    root = Path(root).resolve()
    if not name or any(c in name for c in '/\\:'):
        raise ValueError('Invalid project name')
    with allocation_lock(root.parent):
        target = root/CONFIG_NAME
        if target.exists():
            raise ValueError('Project already configured; preserve its assigned ports')
        used = reservations(root.parent)
        sockets, ports = [], {}
        try:
            candidate = first_port
            for role in ('audit', 'trace', 'user'):
                while candidate <= 65535:
                    port = candidate; candidate += 1
                    if port in used:
                        continue
                    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                    listener.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
                    try:
                        listener.bind(('127.0.0.1', port))
                    except OSError:
                        listener.close()
                        continue
                    sockets.append(listener); ports[role] = port
                    break
                else:
                    raise ValueError('No free GDB ports')
            for relative in ('src/platform/win', 'bin', '_build', 'orig', 'status', 'tools'):
                (root/relative).mkdir(parents=True, exist_ok=True)
            config = {'schema': 1, 'name': name, 'toolset': str(Path(__file__).resolve().parent),
                      'paths': {'status': 'status', 'project_tools': 'tools',
                                'analysis_database': 'status/analysis.sqlite',
                                'native_executable': 'bin/'+name+'.exe',
                                'native_working_directory': 'bin',
                                'native_solution': 'src/platform/win/'+name+'.sln',
                                'native_build': '_build'},
                      'duckstation': {'host': '127.0.0.1', 'gdb_port': ports['user'],
                                      'reserved_gdb_ports': ports,
                                      'default_role':'user', 'data_directory':'tools/duckstation/data/user',
                                        'data_directories':{role:'tools/duckstation/data/'+role for role in ports}}}
            with target.open('x', encoding='utf-8') as stream:
                json.dump(config, stream, indent=2)
            return config
        finally:
            for listener in sockets:
                listener.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--name', required=True)
    parser.add_argument('--first-port', type=int, default=2400)
    args = parser.parse_args()
    if not 1024 <= args.first_port <= 65533:
        parser.error('First port must be 1024..65533')
    print(json.dumps(initialize(args.root, args.name, args.first_port), indent=2))


if __name__ == '__main__':
    main()

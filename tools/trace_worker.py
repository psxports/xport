"""Persist capture completion independently of the queue controller"""
import argparse
import ctypes
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import subprocess
import time

from trace_cache import digest


def process_identity(pid):
    if os.name != 'nt':
        try:
            stat = Path('/proc')/str(pid)/'stat'
            fields = stat.read_text().rsplit(')', 1)[1].split()
            return {'pid': pid, 'created': fields[19]}
        except FileNotFoundError:
            return None
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
    kernel.OpenProcess.restype = w.HANDLE
    kernel.GetProcessTimes.argtypes = [w.HANDLE, *([ctypes.POINTER(w.FILETIME)]*4)]
    kernel.GetExitCodeProcess.argtypes = [w.HANDLE, ctypes.POINTER(w.DWORD)]
    kernel.CloseHandle.argtypes = [w.HANDLE]
    handle = kernel.OpenProcess(0x1000, False, pid)
    if not handle:
        error = ctypes.get_last_error()
        if error == 87:
            return None
        raise ctypes.WinError(error)
    try:
        code = w.DWORD()
        times = [w.FILETIME() for _ in range(4)]
        if not kernel.GetExitCodeProcess(handle, ctypes.byref(code)) or not kernel.GetProcessTimes(handle, *(ctypes.byref(t) for t in times)):
            raise ctypes.WinError(ctypes.get_last_error())
        if code.value != 259:
            return None
        return {'pid': pid, 'created': (times[0].dwHighDateTime << 32) | times[0].dwLowDateTime}
    finally:
        kernel.CloseHandle(handle)


def alive(identity):
    return identity is not None and process_identity(identity['pid']) == identity


def write_receipt(path, value):
    temporary = path.with_suffix('.tmp')
    with temporary.open('w', encoding='utf-8') as stream:
        json.dump(value, stream)
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(path)


def run(ticket_path, receipt_path):
    ticket = json.loads(ticket_path.read_text(encoding='utf-8'))
    receipt = {'ticket_sha256': digest(ticket_path), 'worker': process_identity(os.getpid()),
               'status': 'starting', 'started': time.time()}
    if receipt_path.exists():
        raise ValueError('Receipt already exists')
    write_receipt(receipt_path, receipt)
    try:
        child = subprocess.Popen(ticket['command'], cwd=ticket['cwd'], shell=False,
                                 creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        receipt.update(status='running', child=process_identity(child.pid))
        write_receipt(receipt_path, receipt)
        code = child.wait()
        if code == 0:
            receipt['artifact_hashes'] = {name: digest(path) for name, path in ticket.get('outputs', {}).items()}
        receipt.update(status='finished', exit_code=code, finished=time.time())
    except (OSError, ValueError) as error:
        receipt.update(status='failed', error=str(error), finished=time.time())
        code = 2
    write_receipt(receipt_path, receipt)
    return code


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ticket', type=Path, required=True)
    parser.add_argument('--receipt', type=Path, required=True)
    args = parser.parse_args()
    from xport_project import artifact_path
    args.receipt = artifact_path(args.receipt)
    return run(args.ticket, args.receipt)


if __name__ == '__main__':
    raise SystemExit(main())

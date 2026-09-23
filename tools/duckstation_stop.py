"""Stop only an emulator explicitly leased to this convergence workflow"""
import argparse
import ctypes
from ctypes import wintypes as w
import json
from pathlib import Path
from duckstation_runtime import verify_owner
from gdb_endpoint import listener_process
from trace_worker import alive
from xport_project import artifact_path


def stop_owned(record):
    identity={k:record[k] for k in ('pid','created')}
    if not alive(identity):return dict(status='already_exited',pid=identity['pid'])
    for session_path in artifact_path('status/user-traces').glob('*/session.json'):
        session=json.loads(session_path.read_text())
        if session.get('status') in ('starting','recording','finalizing') and session.get('process_identity')==identity:
            raise ValueError('Emulator has an active user recording')
    owner=listener_process(record['port'])
    actual=verify_owner(record['data_directory'],record['port'],owner)
    if any(actual.get(k)!=record.get(k) for k in ('pid','created','port','executable','project','data_directory')):
        raise ValueError('Emulator lease no longer owns endpoint')
    kernel=ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.OpenProcess.argtypes=[w.DWORD,w.BOOL,w.DWORD];kernel.OpenProcess.restype=w.HANDLE
    kernel.GetProcessTimes.argtypes=[w.HANDLE,*([ctypes.POINTER(w.FILETIME)]*4)]
    kernel.TerminateProcess.argtypes=[w.HANDLE,w.UINT];kernel.CloseHandle.argtypes=[w.HANDLE]
    kernel.WaitForSingleObject.argtypes=[w.HANDLE,w.DWORD]
    handle=kernel.OpenProcess(0x1000|0x100000|1,False,record['pid'])
    if not handle:raise ctypes.WinError(ctypes.get_last_error())
    try:
        times=[w.FILETIME() for _ in range(4)]
        if not kernel.GetProcessTimes(handle,*(ctypes.byref(t) for t in times)):raise ctypes.WinError(ctypes.get_last_error())
        created=(times[0].dwHighDateTime<<32)|times[0].dwLowDateTime
        if created!=record['created']:raise ValueError('Emulator PID was reused')
        if not kernel.TerminateProcess(handle,0):raise ctypes.WinError(ctypes.get_last_error())
        if kernel.WaitForSingleObject(handle,5000)!=0:raise RuntimeError('Owned emulator did not exit')
    finally:kernel.CloseHandle(handle)
    return dict(status='stopped',pid=record['pid'])


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--lease',type=Path,required=True);a=p.parse_args()
    path=artifact_path(a.lease);record=json.loads(path.read_text())
    if record.get('purpose')!='converge_check' or not record.get('trace'):raise ValueError('Explicit converge lease required')
    print(json.dumps(stop_owned(record)))


if __name__=='__main__':main()

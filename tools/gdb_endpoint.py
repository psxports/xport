"""Inspect the live owner of a loopback GDB endpoint on Windows"""
import ctypes
from ctypes import wintypes as w
import socket
import struct

def listener_process(port):
    query = ctypes.WinDLL('iphlpapi').GetExtendedTcpTable
    query.argtypes = [ctypes.c_void_p, ctypes.POINTER(w.DWORD), w.BOOL, w.ULONG, w.ULONG, w.ULONG]
    query.restype = w.DWORD
    size = w.DWORD()
    query(None, ctypes.byref(size), False, 2, 3, 0)
    table = ctypes.create_string_buffer(size.value)
    if query(table, ctypes.byref(size), False, 2, 3, 0):
        raise OSError('Cannot inspect GDB listener')
    pids = {row[5] for i in range(struct.unpack_from('<I', table)[0])
            for row in [struct.unpack_from('<6I', table, 4+i*24)]
            if row[1] == 0x0100007f and socket.ntohs(row[2] & 65535) == port}
    if len(pids) != 1:
        raise OSError('Expected one loopback GDB listener')
    pid = pids.pop()
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
    kernel.OpenProcess.restype = w.HANDLE
    kernel.QueryFullProcessImageNameW.argtypes = [w.HANDLE, w.DWORD, w.LPWSTR, ctypes.POINTER(w.DWORD)]
    kernel.CloseHandle.argtypes = [w.HANDLE]
    handle = kernel.OpenProcess(0x1000, False, pid)
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        path = ctypes.create_unicode_buffer(32768)
        size = w.DWORD(len(path))
        if not kernel.QueryFullProcessImageNameW(handle, 0, path, ctypes.byref(size)):
            raise ctypes.WinError(ctypes.get_last_error())
        return {'Id': pid, 'Path': path.value}
    finally:
        kernel.CloseHandle(handle)

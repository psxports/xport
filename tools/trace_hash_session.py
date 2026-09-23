"""Reuse hashes only while Windows read handles forbid writes and replacement"""
from contextlib import contextmanager
from contextvars import ContextVar
import ctypes
from ctypes import wintypes
import hashlib
import os
from pathlib import Path

_session = ContextVar('hash_session', default=None)


@contextmanager
def hash_session():
    if _session.get() is not None:
        yield
        return
    cache = {}
    token = _session.set(cache)
    try:
        yield
    finally:
        for _, handle in cache.values():
            _kernel().CloseHandle(handle)
        _session.reset(token)


def _kernel():
    k = ctypes.WinDLL('kernel32', use_last_error=True)
    k.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                             ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    k.CreateFileW.restype = wintypes.HANDLE
    k.CloseHandle.argtypes = [wintypes.HANDLE]
    return k


def release_under(directory):
    cache=_session.get()
    if cache is None:return
    directory=Path(directory).resolve()
    for path in list(cache):
        if Path(path).is_relative_to(directory):
            _,handle=cache.pop(path)
            _kernel().CloseHandle(handle)


def digest(path):
    from pipeline_metrics import span
    path = Path(path).resolve()
    cache = _session.get()
    with span('hash', path=str(path)) as metrics:
        if cache is not None and str(path) in cache:
            metrics.update(cache_hit=True, bytes_hashed=0)
            return cache[str(path)][0]
        handle = None
        # Only immutable capture binaries are pinned; mutable metadata and builds are always rehashed
        if cache is not None and os.name == 'nt' and path.suffix.lower() in (
                '.bin', '.world', '.actors', '.phases', '.ffcp', '.ctx',
                '.ram', '.scratchpad', '.cpu', '.gpu', '.spu', '.inputs'):
            k = _kernel()
            handle = k.CreateFileW(str(path), 0x80000000, 1, None, 3, 0x08000000, None)
            if handle == ctypes.c_void_p(-1).value:
                raise ctypes.WinError(ctypes.get_last_error())
        try:
            h = hashlib.sha256(); size = 0
            buffer = bytearray(1024*1024); view = memoryview(buffer)
            with path.open('rb') as f:
                while count := f.readinto(buffer):
                    h.update(view[:count]); size += count
            result = h.hexdigest()
            metrics.update(cache_hit=False, bytes_hashed=size)
            if handle is not None:
                cache[str(path)] = (result, handle)
                handle = None
            return result
        finally:
            if handle is not None: _kernel().CloseHandle(handle)

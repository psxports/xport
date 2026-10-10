"""Run a test command on a hidden Windows desktop with audio disabled"""
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import subprocess
import sys
from windows_desktop import StartupInfo, ProcessInformation
from xport_process import normalize_current_environment


def main():
    if os.name != 'nt' or len(sys.argv) < 3:
        raise SystemExit('Usage on Windows: hidden_test_run.py DIRECTORY EXECUTABLE [ARGUMENTS]')
    directory = Path(sys.argv[1]).resolve()
    if not directory.is_dir():
        raise ValueError('Test working directory does not exist')
    user = ctypes.WinDLL('user32', use_last_error=True)
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    user.CreateDesktopW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR, ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
    user.CreateDesktopW.restype = wintypes.HANDLE
    user.CloseDesktop.argtypes = [wintypes.HANDLE]
    kernel.CreateProcessW.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, ctypes.c_void_p, ctypes.c_void_p, wintypes.BOOL, wintypes.DWORD, ctypes.c_void_p, wintypes.LPCWSTR, ctypes.POINTER(StartupInfo), ctypes.POINTER(ProcessInformation)]
    kernel.CreateProcessW.restype = wintypes.BOOL
    kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    normalize_current_environment()
    name = 'XportTest_' + str(os.getpid())
    desktop = user.CreateDesktopW(name, None, None, 0, 0x10000000, None)
    if not desktop:
        raise ctypes.WinError(ctypes.get_last_error())
    os.environ['XPORT_AUDIO_OUTPUT'] = '0'
    startup = StartupInfo()
    startup.cb = ctypes.sizeof(startup)
    startup.lpDesktop = name
    startup.dwFlags = 1
    startup.wShowWindow = 0
    process = ProcessInformation()
    command = ctypes.create_unicode_buffer(subprocess.list2cmdline(sys.argv[2:]))
    try:
        if not kernel.CreateProcessW(None, command, None, None, False, 0x08000000, None, str(directory), ctypes.byref(startup), ctypes.byref(process)):
            raise ctypes.WinError(ctypes.get_last_error())
        print(json.dumps({'pid': process.dwProcessId, 'desktop': name, 'audio_output': 0}), flush=True)
        if kernel.WaitForSingleObject(process.hProcess, 0xFFFFFFFF) != 0:
            raise ctypes.WinError(ctypes.get_last_error())
        code = wintypes.DWORD()
        if not kernel.GetExitCodeProcess(process.hProcess, ctypes.byref(code)):
            raise ctypes.WinError(ctypes.get_last_error())
        print(json.dumps({'exit_code': code.value}), flush=True)
        raise SystemExit(code.value)
    finally:
        if process.hThread:
            kernel.CloseHandle(process.hThread)
        if process.hProcess:
            kernel.CloseHandle(process.hProcess)
        user.CloseDesktop(desktop)


if __name__ == '__main__':
    main()

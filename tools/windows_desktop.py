"""Create a Windows process on the interactive default desktop"""

import ctypes
from ctypes import wintypes
import os
from pathlib import Path
import subprocess


CREATE_NEW_PROCESS_GROUP = 0x00000200
CREATE_UNICODE_ENVIRONMENT = 0x00000400
CREATE_NO_WINDOW = 0x08000000
STARTF_USESHOWWINDOW = 0x00000001
SW_HIDE = 0
SW_SHOWNORMAL = 1


class StartupInfo(ctypes.Structure):
    _fields_ = [
        ("cb", wintypes.DWORD), ("lpReserved", wintypes.LPWSTR),
        ("lpDesktop", wintypes.LPWSTR), ("lpTitle", wintypes.LPWSTR),
        ("dwX", wintypes.DWORD), ("dwY", wintypes.DWORD),
        ("dwXSize", wintypes.DWORD), ("dwYSize", wintypes.DWORD),
        ("dwXCountChars", wintypes.DWORD), ("dwYCountChars", wintypes.DWORD),
        ("dwFillAttribute", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
        ("wShowWindow", wintypes.WORD), ("cbReserved2", wintypes.WORD),
        ("lpReserved2", ctypes.POINTER(ctypes.c_byte)),
        ("hStdInput", wintypes.HANDLE), ("hStdOutput", wintypes.HANDLE),
        ("hStdError", wintypes.HANDLE),
    ]


class ProcessInformation(ctypes.Structure):
    _fields_ = [
        ("hProcess", wintypes.HANDLE), ("hThread", wintypes.HANDLE),
        ("dwProcessId", wintypes.DWORD), ("dwThreadId", wintypes.DWORD),
    ]


class DefaultDesktopProcess:
    def __init__(self, pid: int, handle):
        self.pid = pid
        self._handle = handle

    def poll(self):
        if self._handle is None:
            return self.returncode
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        exit_code = wintypes.DWORD()
        if not kernel.GetExitCodeProcess(self._handle, ctypes.byref(exit_code)):
            raise ctypes.WinError(ctypes.get_last_error())
        if exit_code.value == 259:
            return None
        self.returncode = int(exit_code.value)
        kernel.CloseHandle(self._handle)
        self._handle = None
        return self.returncode

    def close(self):
        if self._handle is not None:
            ctypes.WinDLL("kernel32", use_last_error=True).CloseHandle(self._handle)
            self._handle = None

    def __del__(self):
        self.close()


def _create_default_desktop(command, directory: Path, visible: bool, environment):
    if os.name != "nt":
        raise RuntimeError("Interactive process requires Windows WinSta0\\Default")
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    create = kernel.CreateProcessW
    create.argtypes = [
        wintypes.LPCWSTR, wintypes.LPWSTR, ctypes.c_void_p, ctypes.c_void_p,
        wintypes.BOOL, wintypes.DWORD, ctypes.c_void_p, wintypes.LPCWSTR,
        ctypes.POINTER(StartupInfo), ctypes.POINTER(ProcessInformation),
    ]
    create.restype = wintypes.BOOL
    startup = StartupInfo()
    startup.cb = ctypes.sizeof(startup)
    startup.lpDesktop = "WinSta0\\Default"
    startup.dwFlags = 0 if visible else STARTF_USESHOWWINDOW
    startup.wShowWindow = SW_SHOWNORMAL if visible else SW_HIDE
    process = ProcessInformation()
    command_line = ctypes.create_unicode_buffer(subprocess.list2cmdline(command))
    environment_block = None
    environment_pointer = None
    if environment is not None:
        entries = [f"{key}={value}" for key, value in sorted(environment.items(), key=lambda item: item[0].upper())]
        environment_block = ctypes.create_unicode_buffer("\0".join(entries) + "\0\0")
        environment_pointer = ctypes.cast(environment_block, ctypes.c_void_p)
    flags = CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT
    if not visible:
        flags |= CREATE_NO_WINDOW
    if not create(
        command[0], command_line, None, None, False,
        flags,
        environment_pointer, str(directory), ctypes.byref(startup), ctypes.byref(process),
    ):
        error = ctypes.get_last_error()
        raise OSError(error, "Cannot launch process on WinSta0\\Default")
    kernel.CloseHandle(process.hThread)
    return kernel, process


def launch_default_desktop(command, directory: Path, *, visible: bool = False, environment=None) -> int:
    kernel, process = _create_default_desktop(command, directory, visible, environment)
    kernel.CloseHandle(process.hProcess)
    return int(process.dwProcessId)


def launch_default_desktop_process(command, directory: Path, *, visible: bool = False, environment=None):
    unused, process = _create_default_desktop(command, directory, visible, environment)
    return DefaultDesktopProcess(int(process.dwProcessId), process.hProcess)

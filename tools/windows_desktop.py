"""Create a Windows process on the interactive default desktop"""

import ctypes
from ctypes import wintypes
import os
from pathlib import Path
import subprocess
import re
import uuid


def process_desktop(environment=None):
    source = os.environ if environment is None else environment
    name = source.get("XPORT_WINDOWS_DESKTOP", "WinSta0\\Default")
    if not re.fullmatch(r"WinSta0\\[A-Za-z0-9_-]{1,64}", name):
        raise ValueError("Invalid Windows process desktop")
    return name


CREATE_NEW_PROCESS_GROUP = 0x00000200
CREATE_UNICODE_ENVIRONMENT = 0x00000400
CREATE_NO_WINDOW = 0x08000000
STARTF_USESHOWWINDOW = 0x00000001
SW_HIDE = 0
SW_SHOWNORMAL = 1


def open_worker_desktop():
    user = ctypes.WinDLL("user32", use_last_error=True)
    user.CreateDesktopW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR, ctypes.c_void_p,
                                   wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
    user.CreateDesktopW.restype = wintypes.HANDLE
    user.CloseDesktop.argtypes = [wintypes.HANDLE]
    handle = user.CreateDesktopW("XportConverge", None, None, 0, 0x01FF, None)
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    return user, handle


_worker_desktop_lease = None


def signal_worker_ready():
    global _worker_desktop_lease
    name = os.environ.get("XPORT_WORKER_READY_EVENT")
    if not name:
        return
    _worker_desktop_lease = open_worker_desktop()
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenEventW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
    kernel.OpenEventW.restype = wintypes.HANDLE
    kernel.SetEvent.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    event = kernel.OpenEventW(2, False, name)
    if not event or not kernel.SetEvent(event):
        raise ctypes.WinError(ctypes.get_last_error())
    kernel.CloseHandle(event)


def launch_worker_desktop(command, directory: Path, *, environment):
    user, handle = open_worker_desktop()
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateEventW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.BOOL, wintypes.LPCWSTR]
    kernel.CreateEventW.restype = wintypes.HANDLE
    kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    name = "Local\\xport-worker-ready-" + uuid.uuid4().hex
    event = kernel.CreateEventW(None, True, False, name)
    if not event:
        user.CloseDesktop(handle)
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        worker_environment = dict(environment, XPORT_WINDOWS_DESKTOP="WinSta0\\XportConverge",
                                  XPORT_WORKER_READY_EVENT=name)
        process = launch_default_desktop_process(command, directory, visible=False,
                                                environment=worker_environment)
        if kernel.WaitForSingleObject(event, 10000) != 0:
            raise RuntimeError("Hidden converge worker did not initialize")
        return process.pid
    finally:
        kernel.CloseHandle(event)
        user.CloseDesktop(handle)


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
    startup.lpDesktop = process_desktop(environment)
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

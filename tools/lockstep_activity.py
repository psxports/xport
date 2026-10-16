#!/usr/bin/env python3
"""Show an agent's current audit step in the owned converge window title"""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path


PROCESS_QUERY_LIMITED_INFORMATION = 0x1000


def activity_title(frame: int, activity: str) -> str:
    if frame < 0:
        raise ValueError("Frame must be nonnegative")
    activity = " ".join(activity.split())
    if not activity or len(activity) > 100:
        raise ValueError("Activity must contain 1–100 characters")
    return f"xport converge · frame {frame} · {activity}"


def show_activity(receipt_path: Path, frame: int, activity: str) -> str:
    if os.name != "nt":
        raise RuntimeError("The converge window is available only on Windows")
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    from trace_worker import alive
    if not alive(receipt["process"]):
        raise RuntimeError("Supervisor process is not running")
    progress = json.loads(receipt_path.with_name("progress-process.json").read_text(encoding="utf-8"))
    if progress.get("supervisor") != receipt["process"]:
        raise ValueError("Progress process belongs to another supervisor")
    process = progress["process"]
    pid = int(process["pid"])
    created = int(process["created"])
    if receipt.get("contract") != "xport-lockstep-supervisor-process-v1" or pid <= 0:
        raise ValueError("Invalid supervisor process receipt")
    if receipt.get("progress_desktop", receipt.get("desktop")) != "WinSta0\\Default":
        raise ValueError("Supervisor progress window is not on the Default desktop")

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    kernel.GetProcessTimes.restype = wintypes.BOOL
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        raise RuntimeError("Supervisor process is not running")
    try:
        times = [wintypes.FILETIME() for _ in range(4)]
        if not kernel.GetProcessTimes(handle, *(ctypes.byref(value) for value in times)):
            raise OSError(ctypes.get_last_error(), "Cannot read supervisor creation time")
        actual = (times[0].dwHighDateTime << 32) | times[0].dwLowDateTime
        if actual != created:
            raise RuntimeError("Supervisor PID was reused")
    finally:
        kernel.CloseHandle(handle)

    user = ctypes.WinDLL("user32", use_last_error=True)
    user.OpenWindowStationW.argtypes = [wintypes.LPCWSTR, wintypes.BOOL, wintypes.DWORD]
    user.OpenWindowStationW.restype = wintypes.HANDLE
    user.SetProcessWindowStation.argtypes = [wintypes.HANDLE]
    user.SetProcessWindowStation.restype = wintypes.BOOL
    user.OpenDesktopW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    user.OpenDesktopW.restype = wintypes.HANDLE
    user.SetThreadDesktop.argtypes = [wintypes.HANDLE]
    user.SetThreadDesktop.restype = wintypes.BOOL
    station = user.OpenWindowStationW("WinSta0", False, 0x0001 | 0x0002 | 0x0004 | 0x0100)
    if not station or not user.SetProcessWindowStation(station):
        raise OSError(ctypes.get_last_error(), "Cannot attach to WinSta0")
    desktop = user.OpenDesktopW("Default", 0, False, 0x0001 | 0x0002 | 0x0040 | 0x0080 | 0x0100)
    if not desktop or not user.SetThreadDesktop(desktop):
        raise OSError(ctypes.get_last_error(), "Cannot attach to Default desktop")
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user.GetWindowThreadProcessId.restype = wintypes.DWORD
    user.GetWindowTextLengthW.argtypes = [wintypes.HWND]
    user.GetWindowTextLengthW.restype = ctypes.c_int
    user.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    user.GetWindowTextW.restype = ctypes.c_int
    user.SetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPCWSTR]
    user.SetWindowTextW.restype = wintypes.BOOL
    windows = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def collect(hwnd, unused):
        owner = wintypes.DWORD()
        user.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value != pid:
            return True
        length = user.GetWindowTextLengthW(hwnd)
        if length:
            title = ctypes.create_unicode_buffer(length + 1)
            user.GetWindowTextW(hwnd, title, length + 1)
            if title.value.startswith("xport converge"):
                windows.append(hwnd)
        return True

    user.EnumWindows.argtypes = [type(collect), wintypes.LPARAM]
    user.EnumWindows.restype = wintypes.BOOL
    if not user.EnumWindows(collect, 0):
        raise OSError(ctypes.get_last_error(), "Cannot enumerate supervisor windows")
    if len(windows) != 1:
        raise RuntimeError(f"Expected one owned converge window, found {len(windows)}")
    title = activity_title(frame, activity)
    if not user.SetWindowTextW(windows[0], title):
        raise OSError(ctypes.get_last_error(), "Cannot update converge window title")
    observed = ctypes.create_unicode_buffer(len(title) + 1)
    user.GetWindowTextW(windows[0], observed, len(observed))
    if observed.value != title:
        raise RuntimeError("Converge window did not display the activity title")
    return title


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--receipt", type=Path, required=True)
    parser.add_argument("--frame", type=int, required=True)
    parser.add_argument("--activity", required=True)
    args = parser.parse_args()
    print(show_activity(args.receipt, args.frame, args.activity))


if __name__ == "__main__":
    main()

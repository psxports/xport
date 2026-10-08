#!/usr/bin/env python3
"""In-process Tk progress window owned by the lockstep supervisor"""

from __future__ import annotations

import ctypes
from ctypes import wintypes
import gc
import os
from pathlib import Path
import queue
import threading


_TCL_ROOT = Path(__file__).resolve().parent / "runtime" / "tcl"
os.environ["TCL_LIBRARY"] = str(_TCL_ROOT / "tcl8.6")
os.environ["TK_LIBRARY"] = str(_TCL_ROOT / "tk8.6")

import tkinter as tk


def _attach_default_desktop():
    user = ctypes.WinDLL("user32", use_last_error=True)
    user.OpenWindowStationW.argtypes = [wintypes.LPCWSTR, wintypes.BOOL, wintypes.DWORD]
    user.OpenWindowStationW.restype = wintypes.HANDLE
    user.SetProcessWindowStation.argtypes = [wintypes.HANDLE]
    user.SetProcessWindowStation.restype = wintypes.BOOL
    user.OpenDesktopW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    user.OpenDesktopW.restype = wintypes.HANDLE
    user.SetThreadDesktop.argtypes = [wintypes.HANDLE]
    user.SetThreadDesktop.restype = wintypes.BOOL
    user.CloseDesktop.argtypes = [wintypes.HANDLE]
    station = user.OpenWindowStationW("WinSta0", False, 0x0001 | 0x0002 | 0x0004 | 0x0100)
    if not station or not user.SetProcessWindowStation(station):
        raise OSError(ctypes.get_last_error(), "Cannot attach supervisor to WinSta0")
    desktop = user.OpenDesktopW("Default", 0, False, 0x0001 | 0x0002 | 0x0040 | 0x0080 | 0x0100)
    if not desktop or not user.SetThreadDesktop(desktop):
        raise OSError(ctypes.get_last_error(), "Cannot attach progress thread to Default desktop")
    return user, desktop


class Bar(tk.Canvas):
    def __init__(self, parent, color: str):
        super().__init__(parent, width=480, height=24, bg="#d0d0d0",
                         highlightthickness=1, highlightbackground="#a0a0a0")
        self.fill = self.create_rectangle(0, 0, 0, 24, fill=color, outline="")
        self.label = self.create_text(240, 12, text="", fill="#101010")

    def update_value(self, text: str, value: int, count: int) -> None:
        width = 0 if count <= 0 else round(480 * min(max(value, 0), count) / count)
        self.coords(self.fill, 0, 0, width, 24)
        self.itemconfigure(self.label, text=text)


def progress_status_text(update: dict) -> str:
    mode = update.get("mode")
    target = update.get("target_frame")
    current = int(update.get("current", 0))
    if mode == "WAITING_REPAIR" and target is not None:
        return f"Divergence at frame {int(target) + 1} · waiting for repair"
    if mode == "DIAGNOSTIC" and target is not None:
        return f"Diagnosing frame {int(target) + 1}"
    if mode == "PREFIX_GUARD" and target is not None:
        return f"Fast forward to frame {int(target)}"
    if mode == "DETAILED":
        return f"Checking frame {current}"
    return str(mode or "")


class ProgressWindow:
    def __init__(self, updates: queue.SimpleQueue):
        self.updates = updates
        self.root = tk.Tk()
        self.root.title("xport converge")
        self.root.resizable(False, False)
        self.root.attributes("-topmost", False)
        self.root.configure(bg="#f0f0f0")
        self.root.geometry(f"512x116+{max(0, self.root.winfo_screenwidth() - 532)}+20")
        self.active = False
        self.root.protocol("WM_DELETE_WINDOW", self.keep_visible)
        body = tk.Frame(self.root, padx=15, pady=12, bg="#f0f0f0")
        body.pack(fill="both", expand=True)
        self.maximum = Bar(body, "#46a758")
        self.maximum.pack()
        self.current = Bar(body, "#3c78d8")
        self.current.pack(pady=(10, 0))
        self.status = tk.Label(body, text="", anchor="w", bg="#f0f0f0", fg="#303030")
        self.status.pack(fill="x", pady=(6, 0))
        self.root.update_idletasks()
        self.root.deiconify()
        self.root.lift()
        self.root.update()
        hwnd = self.root.winfo_id()
        user = ctypes.WinDLL("user32", use_last_error=True)
        user.GetAncestor.argtypes = [wintypes.HWND, wintypes.UINT]
        user.GetAncestor.restype = wintypes.HWND
        user.SetWindowPos.argtypes = [
            wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int,
            ctypes.c_int, ctypes.c_int, wintypes.UINT,
        ]
        top = user.GetAncestor(hwnd, 2) or hwnd
        flags = 0x0001 | 0x0002 | 0x0010 | 0x0040
        user.SetWindowPos(top, -1, 0, 0, 0, 0, flags)
        user.SetWindowPos(top, -2, 0, 0, 0, 0, flags)
        self.root.after(50, self.refresh)

    def keep_visible(self) -> None:
        if not self.active:
            return
        self.root.deiconify()
        self.root.lift()

    def refresh(self) -> None:
        latest = None
        while True:
            try:
                latest = self.updates.get_nowait()
            except queue.Empty:
                break
        if latest is not None:
            self.active = bool(latest.get("active", False))
            count = int(latest["frame_count"])
            maximum = int(latest["maximum_match"])
            current = int(latest["current"])
            self.maximum.update_value(f"Recorded interval MATCH: {maximum}/{count}", maximum, count)
            self.current.update_value(f"Current converge: {current}/{count}", current, count)
            self.status.configure(text=progress_status_text(latest))
            if not self.active:
                self.root.destroy()
                return
        if self.active and self.root.state() != "normal":
            self.keep_visible()
        self.root.after(50, self.refresh)

    def run(self) -> None:
        self.root.mainloop()


class SupervisorProgress:
    def __init__(self, enabled: bool):
        self.enabled = enabled and os.name == "nt"
        self.snapshot: dict = {}
        self._updates = queue.SimpleQueue()
        self._thread = None
        self._ready = threading.Event()
        self._error = None
        self._main_thread_window = False

    def start(self) -> None:
        if not self.enabled or self._thread is not None or self._main_thread_window:
            return
        self._thread = threading.Thread(target=self._run, name="xport-progress", daemon=True)
        self._thread.start()
        if not self._ready.wait(5):
            raise RuntimeError("Progress window did not initialize")
        if self._error is not None:
            raise RuntimeError("Progress window failed: " + str(self._error))

    def _run(self) -> None:
        desktop = None
        window = None
        try:
            desktop = _attach_default_desktop()
            window = ProgressWindow(self._updates)
            self._ready.set()
            window.run()
        except Exception as error:
            self._error = error
            self._ready.set()
        finally:
            window = None
            gc.collect()
            if desktop is not None:
                user, handle = desktop
                user.CloseDesktop(handle)

    def update(self, value: dict) -> None:
        self.snapshot = dict(value)
        if self.enabled:
            self._updates.put(self.snapshot)

    def close(self) -> None:
        if not self.snapshot:
            return
        value = dict(self.snapshot)
        value["active"] = False
        self.update(value)
        if self._thread is not None and not self._main_thread_window:
            self._thread.join(2)
            self._thread = None

    def run_action(self, action):
        if not self.enabled:
            return action()
        window = ProgressWindow(self._updates)
        outcome = {}
        self._main_thread_window = True

        def worker():
            try:
                outcome["result"] = action()
            except BaseException as error:
                outcome["error"] = error
            finally:
                if self.snapshot.get("active", False):
                    self.close()

        self._thread = threading.Thread(target=worker, name="xport-supervisor", daemon=False)
        self._thread.start()
        window.run()
        self._thread.join()
        self._thread = None
        self._main_thread_window = False
        window = None
        gc.collect()
        if "error" in outcome:
            raise outcome["error"]
        return outcome.get("result")

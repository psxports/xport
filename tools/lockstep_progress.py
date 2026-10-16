#!/usr/bin/env python3
"""Default-desktop progress UI with a pipe to the hidden supervisor"""

from __future__ import annotations

import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import queue
import threading
import sys
import uuid
from multiprocessing.connection import Client, Listener


_TCL_ROOT = Path(__file__).resolve().parent / "runtime" / "tcl"
os.environ["TCL_LIBRARY"] = str(_TCL_ROOT / "tcl8.6")
os.environ["TK_LIBRARY"] = str(_TCL_ROOT / "tk8.6")

import tkinter as tk


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
    if mode == "FAILED":
        return f"Replay stopped at frame {current} · diagnosis required"
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
        if latest is not None and latest.get("close"):
            self.root.destroy()
            return
        if latest is not None:
            self.active = bool(latest.get("active", False))
            count = int(latest["frame_count"])
            maximum = int(latest["maximum_match"])
            current = int(latest["current"])
            maximum_label = latest.get("maximum_label", "Recorded interval MATCH")
            current_label = latest.get("current_label", "Current converge")
            self.maximum.update_value(f"{maximum_label}: {maximum}/{count}", maximum, count)
            self.current.update_value(f"{current_label}: {current}/{count}", current, count)
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
        self.receipt_path = None
        self._updates = queue.SimpleQueue()
        self._thread = None
        self._ready = threading.Event()
        self._error = None
        self._process = None

    def start(self) -> None:
        if not self.enabled or self._thread is not None:
            return
        address = r"\\.\pipe\xport-progress-" + uuid.uuid4().hex
        key = os.urandom(32)
        listener = Listener(address, family="AF_PIPE", authkey=key)
        from windows_desktop import launch_default_desktop_process
        from xport_process import normalized_environment
        interpreter = Path(sys.executable).with_name("pythonw.exe")
        self._process = launch_default_desktop_process(
            [str(interpreter), "-B", str(Path(__file__).resolve()), address, key.hex()],
            Path.cwd(), visible=True,
            environment=normalized_environment(updates={"XPORT_WINDOWS_DESKTOP": "WinSta0\\Default"}))

        def relay():
            try:
                with listener.accept() as connection:
                    ready = json.loads(connection.recv_bytes())
                    if ready != {"ready": self._process.pid}:
                        raise RuntimeError("Progress UI identity mismatch")
                    if self.receipt_path is not None:
                        from trace_worker import process_identity, write_receipt
                        write_receipt(self.receipt_path, {
                            "supervisor": process_identity(os.getpid()),
                            "process": process_identity(self._process.pid),
                            "desktop": "WinSta0\\Default"})
                    self._ready.set()
                    while True:
                        value = self._updates.get()
                        connection.send_bytes(json.dumps(value).encode("utf-8"))
                        if not value.get("active", False):
                            break
            except Exception as error:
                self._error = error
                self._ready.set()
            finally:
                listener.close()

        self._thread = threading.Thread(target=relay, name="xport-progress-pipe", daemon=True)
        self._thread.start()
        if not self._ready.wait(10):
            raise RuntimeError("Progress window did not initialize")
        if self._error is not None:
            raise RuntimeError("Progress window failed: " + str(self._error))

    def update(self, value: dict) -> None:
        self.snapshot = dict(value)
        if self.enabled:
            self._updates.put(self.snapshot)

    def close(self) -> None:
        if not self.snapshot:
            return
        self.update(dict(self.snapshot, active=False))
        if self._thread is not None:
            self._thread.join(2)
            self._thread = None

    def run_action(self, action):
        result = action()
        if self.enabled and result.get("status") == "FAILED":
            self._thread.join()
        return result


def main():
    updates = queue.SimpleQueue()
    window = ProgressWindow(updates)
    connection = Client(sys.argv[1], family="AF_PIPE", authkey=bytes.fromhex(sys.argv[2]))
    connection.send_bytes(json.dumps({"ready": os.getpid()}).encode("utf-8"))

    def receive():
        try:
            while True:
                updates.put(json.loads(connection.recv_bytes()))
        except (EOFError, OSError):
            updates.put({"close": True})

    threading.Thread(target=receive, name="xport-progress-input", daemon=True).start()
    window.run()
    connection.close()


if __name__ == "__main__":
    main()

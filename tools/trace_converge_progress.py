"""Publish compact live convergence progress"""
import json
import os
from pathlib import Path
import time

from trace_worker import process_identity, write_receipt


TRANSIENT = ('detail', 'stage', 'tick', 'phase', 'phase_end', 'tick_end', 'segment')


def update(step, **fields):
    value = os.environ.get('XPORT_CONVERGE_PROGRESS')
    if not value:
        return
    path = Path(value)
    current = json.loads(path.read_text()) if path.exists() else {}
    if current.get('step') != step:
        for key in TRANSIENT:
            current.pop(key, None)
    current.update(status='running', step=step, updated=time.time(), worker=process_identity(os.getpid()), **fields)
    write_receipt(path, current)

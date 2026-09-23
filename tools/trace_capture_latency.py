"""Summarize measured recorder costs separately from whole-frame wall intervals"""
import math
import struct


def decode_timing(payload):
    if len(payload) != 20:
        raise ValueError('Invalid capture timing payload')
    frame, capture, interval, full, checkpoint = struct.unpack('<5I', payload)
    if full not in (0,1) or checkpoint not in (0,1):
        raise ValueError('Invalid capture timing flags')
    return {'frame':frame, 'capture_us':capture, 'wall_interval_us':interval,
            'full_memory':bool(full), 'checkpoint_scheduled':bool(checkpoint)}


def distribution(values):
    values = sorted(values)
    if not values:
        return None
    return {'samples':len(values), 'mean':sum(values)/len(values), 'p50':values[math.ceil(.5*len(values))-1],
            'p95':values[math.ceil(.95*len(values))-1], 'p99':values[math.ceil(.99*len(values))-1], 'max':values[-1]}


def summarize(rows):
    return {'vblanks':len(rows), 'units':'microseconds', 'quantiles':'nearest rank',
            'capture':distribution([r['capture_us'] for r in rows]),
            'capture_without_checkpoint':distribution([r['capture_us'] for r in rows if not r['checkpoint_scheduled']]),
            'capture_with_checkpoint':distribution([r['capture_us'] for r in rows if r['checkpoint_scheduled']]),
            'wall_interval':distribution([r['wall_interval_us'] for r in rows[1:]]),
            'scope':'VBlank RAM/delta capture and state preparation up to timing emission; excludes this timing record emission, phase callbacks and async disk completion. Wall intervals include emulation, pacing and any external pauses; not recording-only overhead'}

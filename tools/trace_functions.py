"""Preserve partial first-use coverage for one native process"""
import json


def parse(data, pid, build, pad):
    if data and not data.endswith(b'\n'):
        raise ValueError('Truncated function journal')
    rows = [json.loads(line) for line in data.splitlines() if line]
    sessions = [row for row in rows if row.get('event') == 'session']
    if len(sessions) != 1 or sessions[0]['pid'] != pid:
        raise ValueError('Function journal contains an unexpected process/session')
    functions = {}
    for row in rows:
        if row['pid'] != pid:
            raise ValueError('Mixed native processes in function journal')
        if row['event'] == 'session':
            continue
        if row['event'] != 'function_first_use':
            raise ValueError('Unknown function coverage record')
        address = int(row['pc'], 16)
        if not 0 <= address <= 0xffffffff or not isinstance(row['image'], str):
            raise ValueError('Invalid function identity')
        functions.setdefault((row['image'], address), {'image': row['image'], 'address': address,
            'first_tick': row['tick'], 'stage': row['stage']})
    return {'schema': 'ff-first-use-v1', 'pid': pid, 'build_sha256': build, 'pad_sha256': pad,
            'complete': False, 'functions': list(functions.values()),
            'scope': 'Instrumented first-use entries across the whole process including startup; absence does not prove unreachability'}

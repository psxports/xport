"""Classify explicit native failures without guessing a gameplay root cause"""
EXIT_CODES = {'game_failure': 20, 'native_crash': 21, 'infrastructure_error': 22, 'unclassified_failure': 23}


def classify_native(code, log):
    if code == 0:
        return None
    lines = log.splitlines()
    infrastructure = ('Cannot open functions-used.jsonl', 'Function journal write failed',
                      'WIP journal write failed', 'Cannot load native checkpoint:')
    guards = ('Fatal guard or unclassified WIP:', 'WIP handoff:', 'WIP level script', 'WIP npc')
    for line in lines:
        if line.startswith(infrastructure):
            return {'outcome': 'infrastructure_error', 'reason': line, 'native_exit_code': code}
    for line in lines:
        if line.startswith(guards):
            return {'outcome': 'game_failure', 'reason': line, 'native_exit_code': code}
    exceptions = {0xc0000005: 'Access violation', 0xc0000094: 'Integer divide by zero', 0xc00000fd: 'Stack overflow'}
    if code & 0xffffffff in exceptions:
        return {'outcome': 'native_crash', 'reason': exceptions[code & 0xffffffff], 'native_exit_code': code}
    return {'outcome': 'unclassified_failure', 'reason': 'No diagnostic proving the cause', 'native_exit_code': code}

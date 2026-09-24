"""Create deterministic child-process environments"""
import os


def _windows_environment(values):
    result = {}
    spellings = {}
    for key, value in values.items():
        canonical = key.upper()
        previous = spellings.get(canonical)
        if previous is None or key == canonical or previous != canonical:
            result[canonical] = value
            spellings[canonical] = key
    return result


def normalized_environment(base=None, updates=None):
    source = os.environ if base is None else base
    if os.name == 'nt':
        result = _windows_environment(source)
        if updates:
            result.update(_windows_environment(updates))
        return result
    result = dict(source)
    result.update(updates or {})
    return result


def normalize_current_environment():
    if os.name != 'nt':
        return
    normalized = normalized_environment()
    os.environ.clear()
    os.environ.update(normalized)

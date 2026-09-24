"""Resolve one shared emulator installation and isolated project data"""
import configparser
import ctypes
import os
from pathlib import Path
from ctypes import wintypes as w
from xport_project import load_project
from xport_process import normalized_environment

DATA_ROOT_VARIABLE = 'XPORT_DUCKSTATION_DATA_ROOT'


def show_window(pid):
    """Restore the largest visible window owned by pid and switch to it"""
    user = ctypes.WinDLL('user32', use_last_error=True)

    class Rect(ctypes.Structure):
        _fields_ = [('left', w.LONG), ('top', w.LONG), ('right', w.LONG), ('bottom', w.LONG)]

    callback_type = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    candidates = []

    @callback_type
    def collect(hwnd, unused):
        owner = w.DWORD()
        rect = Rect()
        user.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user.IsWindowVisible(hwnd) and user.GetWindowRect(hwnd, ctypes.byref(rect)):
            area = max(0, rect.right-rect.left)*max(0, rect.bottom-rect.top)
            if area >= 320*200:
                candidates.append((area, hwnd))
        return True

    user.EnumWindows(collect, 0)
    if not candidates:
        return False
    hwnd = max(candidates)[1]
    user.ShowWindow(hwnd, 9)  # SW_RESTORE
    user.SwitchToThisWindow(hwnd, True)
    return True


def executable():
    path = Path(__file__).resolve().parent/'duckstation/distribution/duckstation-qt-x64-ReleaseLTCG.exe'
    if not path.is_file():
        raise ValueError('Shared DuckStation executable is missing')
    return path


def data_directory(override=None, role=None):
    root, project = load_project()
    duck = project['duckstation']
    role = role or duck.get('default_role', 'user')
    if role not in duck['reserved_gdb_ports']:
        raise ValueError('Unreserved DuckStation role')
    relative = override or duck.get('data_directories', {}).get(role)
    if relative is None:
        if role == duck.get('default_role', 'user'):
            relative = duck.get('data_directory')
        relative = relative or ('tools/duckstation/data/'+role)
    path = (root/relative).resolve()
    if not path.is_relative_to((root/'tools').resolve()):
        raise ValueError('DuckStation data must remain in project tools')
    return path


def environment(directory):
    return normalized_environment(updates={DATA_ROOT_VARIABLE: str(data_directory(directory))})


def validate_settings(directory, port):
    root, project = load_project()
    directory = data_directory(directory)
    if port not in project['duckstation']['reserved_gdb_ports'].values():
        raise ValueError('GDB port is not reserved by this project')
    ini = configparser.ConfigParser(interpolation=None)
    ini.read(directory/'settings.ini', encoding='utf-8-sig')
    if not ini.getboolean('Debug', 'EnableGDBServer', fallback=False):
        raise ValueError('Project GDB server is disabled')
    if ini.getint('Debug', 'GDBServerPort', fallback=0) != port:
        raise ValueError('Project settings GDB port differs from reservation')
    return directory


def recording_settings(hidden=False):
    """Return the complete speed contract for one recording mode"""
    speed = '0' if hidden else '1'
    return {
        'mode': 'hidden' if hidden else 'interactive',
        'startup': {
            'Main': {
                'StartPaused': 'false',
                'EmulationSpeed': speed,
                'FastForwardSpeed': speed,
                'TurboSpeed': speed,
                'SyncToHostRefreshRate': 'false',
                'RewindEnable': 'false',
                'RunaheadFrameCount': '0',
                'SaveStateOnExit': 'false'
            },
            'BIOS': {'PatchFastBoot': 'true'},
            'CPU': {
                'OverclockEnable': 'false',
                'OverclockNumerator': '1',
                'OverclockDenominator': '1'
            },
            'Display': {'VSync': 'false'}
        },
        # StartPaused is a launch instruction and is not part of the captured effective settings
        'effective': {
            'Main': {
                'EmulationSpeed': speed,
                'FastForwardSpeed': speed,
                'TurboSpeed': speed,
                'SyncToHostRefreshRate': 'false',
                'RewindEnable': 'false',
                'RunaheadFrameCount': '0'
            },
            'CPU': {
                'OverclockEnable': 'false',
                'OverclockNumerator': '1',
                'OverclockDenominator': '1'
            },
            'Display': {'VSync': 'false'}
        }
    }


def configure_recording_settings(directory, port, hidden=False):
    """Normalize recording speed before launch while preserving user controls and audio"""
    from gdb_endpoint import listener_process
    from trace_cache import digest
    directory = validate_settings(directory, port)
    try:
        owner = listener_process(port)
    except OSError:
        owner = None
    if owner:
        raise RuntimeError('Recording requires a fresh emulator; close the existing project-owned user runtime first')
    plan = recording_settings(hidden)
    ini = configparser.ConfigParser(interpolation=None)
    ini.optionxform = str
    ini.read(directory/'settings.ini', encoding='utf-8-sig')
    for section, values in plan['startup'].items():
        if section not in ini:
            ini.add_section(section)
        for key, value in values.items():
            ini[section][key] = value
    temporary = directory/'settings.ini.recording.pending'
    with temporary.open('w', encoding='utf-8') as stream:
        ini.write(stream)
    temporary.replace(directory/'settings.ini')
    validate_settings(directory, port)
    return dict(plan, settings_sha256=digest(directory/'settings.ini'))


def verify_recording_settings(path, plan):
    """Verify the settings snapshot serialized by the emulator at recording start"""
    ini = configparser.ConfigParser(interpolation=None)
    ini.optionxform = str
    if not ini.read(path, encoding='utf-8-sig'):
        return {'verified': False, 'differences': [{'path': str(path), 'reason': 'missing'}]}
    differences = []
    for section, values in plan['effective'].items():
        for key, expected in values.items():
            actual = ini.get(section, key, fallback=None)
            if actual is None or actual.lower() != expected.lower():
                differences.append({'section': section, 'key': key,
                                    'expected': expected, 'actual': actual})
    return {'verified': not differences, 'mode': plan['mode'], 'differences': differences,
            'scope': 'Recorded effective speed, timing, runahead and CPU-overclock settings'}


def record_owner(directory, port, pid):
    from trace_worker import process_identity, write_receipt
    root, project = load_project()
    directory = validate_settings(directory, port)
    identity = process_identity(pid)
    if not identity:
        raise RuntimeError('Launched emulator is no longer alive')
    record = dict(identity, executable=str(executable()), data_directory=str(directory),
                  port=port, project=str(root))
    path = root/'status/runtime'/('duckstation-'+str(port)+'.json')
    path.parent.mkdir(parents=True, exist_ok=True)
    write_receipt(path, record)
    write_receipt(root/'status/runtime/duckstation-process.json', record)
    trace=os.environ.get('XPORT_TRACE_NAME');lease_root=os.environ.get('XPORT_CONVERGE_LEASE_ROOT')
    if trace and lease_root:
        expected=(root/'status/stage-pipeline/workers'/trace/'emulator-leases').resolve()
        if Path(lease_root).resolve()!=expected:raise ValueError('Invalid converge lease directory')
        expected.mkdir(parents=True,exist_ok=True)
        write_receipt(expected/(str(pid)+'.json'),dict(record,trace=trace,purpose='converge_check'))
    return record


def verify_owner(directory, port, process):
    import json
    from trace_worker import alive
    root, project = load_project()
    expected = data_directory(directory)
    path = root/'status/runtime'/('duckstation-'+str(port)+'.json')
    if not path.is_file():
        raise RuntimeError('No project-owned process receipt for this endpoint')
    record = json.loads(path.read_text(encoding='utf-8-sig'))
    if (record.get('port') != port or record.get('pid') != process['Id'] or not alive({k:record[k] for k in ('pid','created')}) or
        Path(record.get('executable','')).resolve() != executable() or
        Path(process['Path']).resolve() != executable() or
        Path(record.get('data_directory','')).resolve() != expected or
        Path(record.get('project','')).resolve() != root):
        raise RuntimeError('Endpoint, creation identity or project data directory differs')
    return record


def prepare_trace_profile(directory, port):
    """Install the validated project profile only while the endpoint is idle"""
    from trace_profile import validate
    from trace_cache import digest
    from gdb_endpoint import listener_process
    import shutil
    root, project = load_project()
    directory = validate_settings(directory, port)
    source = (root/project['duckstation']['trace_profile']).resolve(strict=True)
    validate(source)
    target = directory/'xport-trace-profile.ini'
    if not target.is_file() or digest(target) != digest(source):
        try:
            owner = listener_process(port)
        except OSError:
            owner = None
        if owner:
            raise RuntimeError('Trace profile differs while emulator is running; preserve it and restart explicitly')
        temporary = directory/'xport-trace-profile.ini.pending'
        shutil.copyfile(source, temporary)
        temporary.replace(target)
    return dict(path=str(target), sha256=digest(target))

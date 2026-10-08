"""Deploy an isolated project runtime without copying another project's state"""
import argparse
import configparser
import json
from pathlib import Path
import socket
from trace_cache import digest
from xport_project import allocation_lock, load_project, reservations
from duckstation_runtime import data_directory, validate_settings


def settings_for_role(role, default_role, port, template=None):
    template = template or Path(__file__).parent/'duckstation/settings.ini'
    ini = configparser.ConfigParser(interpolation=None)
    ini.optionxform = str
    if not ini.read(template, encoding='utf-8'):
        raise ValueError('DuckStation settings template is missing: '+str(template))
    ini['Main']['EmulationSpeed'] = '1' if role == default_role else '0'
    ini['Debug']['GDBServerPort'] = str(port)
    if role != default_role:
        ini['Main'].pop('FastForwardSpeed', None)
        ini.remove_section('Pad1')
        ini.remove_section('Hotkeys')
    return ini


def existing_deployment(root, config, role, port, source, target):
    report_path = target/'xport-deployment.json'
    if not report_path.is_file():
        raise ValueError('Existing DuckStation runtime has no deployment receipt: '+str(target))
    report = json.loads(report_path.read_text(encoding='utf-8-sig'))
    expected = {'project': config['name'], 'role': role, 'port': port,
                'runtime': str(target), 'distribution': str(source.resolve())}
    if any(report.get(key) != value for key, value in expected.items()):
        raise ValueError('Existing DuckStation runtime identity differs: '+str(target))
    validate_settings(target, port)
    current_settings_sha256 = digest(target/'settings.ini')
    settings_changed = report.get('settings_sha256') != current_settings_sha256
    default_role = config.get('duckstation', {}).get('default_role', 'user')
    if settings_changed and role != default_role:
        raise ValueError('Existing DuckStation settings differ from deployment receipt: '+str(target))
    missing = [name for name in ('bios', 'savestates', 'memcards', 'screenshots', 'cache')
               if not (target/name).is_dir()]
    if missing:
        raise ValueError('Existing DuckStation runtime is incomplete: '+', '.join(missing))
    profile = target/'xport-trace-profile.ini'
    if profile.is_file():
        profile.unlink()
    report.pop('trace_profile_status', None)
    report.pop('trace_profile_sha256', None)
    report['files'] = {path.relative_to(source).as_posix(): digest(path)
                       for path in sorted(source.rglob('*')) if path.is_file()}
    report_path.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    return dict(report, reused=True, settings_sha256_current=current_settings_sha256,
                settings_changed_since_deployment=settings_changed)


def deploy(role=None, destination=None, ensure=False):
    root, config = load_project()
    settings = config['duckstation']
    default_role = settings.get('default_role', 'user')
    role = role or default_role
    if role not in settings['reserved_gdb_ports']:
        raise ValueError('Unknown project runtime role: '+role)
    port = settings['reserved_gdb_ports'][role]
    source = Path(__file__).parent/'duckstation/distribution'
    if not (source/'duckstation-qt-x64-ReleaseLTCG.exe').is_file():
        raise ValueError('Shared DuckStation distribution is missing')
    target = data_directory(destination, role)
    if not target.is_relative_to((root/'tools').resolve()):
        raise ValueError('Runtime must be inside project tools')
    with allocation_lock(root.parent):
        assigned = reservations(root.parent)
        if assigned.get(port) != root/'xport-project.json':
            raise ValueError('Endpoint reservation does not belong to this project')
        if target.exists() and ensure:
            return existing_deployment(root, config, role, port, source, target)
        if target.exists():
            raise ValueError('Runtime already exists; never overwrite its binaries, settings or states')
        with socket.socket() as probe:
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            probe.bind(('127.0.0.1', port))
            target.mkdir(parents=True, exist_ok=False)
            files = {p.relative_to(source).as_posix():digest(p) for p in sorted(source.rglob('*')) if p.is_file()}
            ini = settings_for_role(role, default_role, port)
            with (target/'settings.ini').open('x', encoding='utf-8') as stream:
                ini.write(stream)
            for folder in ('bios','savestates','memcards','screenshots','cache'):
                (target/folder).mkdir(exist_ok=True)
            report = {'project':config['name'], 'role':role, 'port':port,
                      'runtime':str(target), 'data_directory':str(target), 'distribution':str(source.resolve()), 'shared_executable':True,
                      'files':files, 'settings_sha256':digest(target/'settings.ini'),
                      'launched':False, 'bios_supplied':False}
            (target/'xport-deployment.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--role', help='Configured runtime role; defaults to project default_role')
    parser.add_argument('--destination', help='New runtime directory inside project tools')
    parser.add_argument('--ensure', action='store_true', help='Validate and reuse an identical existing deployment')
    args = parser.parse_args()
    result = deploy(args.role, args.destination, args.ensure)
    print(json.dumps({k:v for k,v in result.items() if k!='files'}))


if __name__ == '__main__':
    main()

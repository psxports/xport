"""Deploy an isolated project runtime without copying another project's state"""
import argparse
import configparser
import json
from pathlib import Path
import shutil
import socket
from project_init import reservations, allocation_lock
from trace_cache import digest
from xport_project import load_project
from duckstation_runtime import data_directory


def deploy(role=None, destination=None):
    root, config = load_project()
    settings = config['duckstation']
    default_role = settings.get('default_role', 'user')
    role = role or default_role
    if role not in settings['reserved_gdb_ports']:
        raise ValueError('Unknown project runtime role: '+role)
    profile = root/settings['trace_profile'] if settings.get('trace_profile') else None
    if profile:
        from trace_profile import validate
        validate(profile)
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
        if target.exists():
            raise ValueError('Runtime already exists; never overwrite its binaries, settings or states')
        with socket.socket() as probe:
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            probe.bind(('127.0.0.1', port))
            target.mkdir(parents=True, exist_ok=False)
            files = {p.relative_to(source).as_posix():digest(p) for p in sorted(source.rglob('*')) if p.is_file()}
            ini = configparser.ConfigParser()
            ini.optionxform = str
            for section, values in {
                'Main': {'EmulationSpeed':'1' if role==default_role else '0',
                         'StartPaused':'true', 'SetupWizardIncomplete':'false',
                         'PauseOnFocusLoss':'false', 'RewindEnable':'false',
                         'RunaheadFrameCount':'0', 'SaveStateOnExit':'false'},
                'BIOS': {'PatchFastBoot':'true'},
                'CPU': {'OverclockEnable':'false'},
                'Display': {'VSync':'false'},
                'Audio': {'OutputMuted':'true'},
                'Debug': {'EnableGDBServer':'true', 'GDBServerPort':str(port)},
                'Hacks': {'ExportSharedMemory':'true'},
                'AutoUpdater': {'CheckAtStartup':'false'},
                'UI': {'UnofficialBuildWarningConfirmed':'true'}
            }.items():
                ini[section] = values
            with (target/'settings.ini').open('x', encoding='utf-8') as stream:
                ini.write(stream)
            for folder in ('bios','savestates','memcards','screenshots','cache'):
                (target/folder).mkdir(exist_ok=True)
            if profile:
                shutil.copy2(profile, target/'xport-trace-profile.ini')
            report = {'project':config['name'], 'role':role, 'port':port,
                      'runtime':str(target), 'data_directory':str(target), 'distribution':str(source.resolve()), 'shared_executable':True,
                      'files':files, 'settings_sha256':digest(target/'settings.ini'),
                      'launched':False, 'bios_supplied':False,
                      'trace_profile_status':'installed' if profile else 'missing; capture will return E25',
                      'trace_profile_sha256':digest(profile) if profile else None}
            (target/'xport-deployment.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--role', help='Configured runtime role; defaults to project default_role')
    parser.add_argument('--destination', help='New runtime directory inside project tools')
    args = parser.parse_args()
    result = deploy(args.role, args.destination)
    print(json.dumps({k:v for k,v in result.items() if k!='files'}))


if __name__ == '__main__':
    main()

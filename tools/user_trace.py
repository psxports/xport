"""Start and stop named recordings while the user plays in DuckStation"""
import argparse
import base64
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time

from xport_project import load_project, activate_adapters, implementation_path
activate_adapters()
from gdb_endpoint import listener_process
from trace_cache import digest
from trace_internal import decode
from trace_recovery import inspect_prefix
from trace_user_input import decode_polls
from trace_user_provenance import inventory, verify_files
from trace_worker import alive, process_identity, write_receipt
from trace_finalize_lock import exclusive_finalize

ROOT, PROJECT = load_project()
from gdb_remote import Remote
from duckstation_runtime import (executable, data_directory, environment, validate_settings,
                                 record_owner, verify_owner, show_window,
                                 configure_recording_settings, verify_recording_settings)


def launch_visible(command, working_directory, process_environment):
    """Launch DuckStation directly through the logged-in Explorer shell"""
    def quote(value):
        return "'"+str(value).replace("'", "''")+"'"
    batch=Path(working_directory)/('xport-visible-launch-'+str(os.getpid())+'.cmd')
    environment_lines=[]
    for key,value in process_environment.items():
        if key.startswith('XPORT_'):
            environment_lines.append('set "'+key+'='+value+'"')
    batch.write_text('@echo off\n'+'\n'.join(environment_lines)+'\nstart "" /D '+
                     subprocess.list2cmdline([str(working_directory)])+' '+
                     subprocess.list2cmdline(command)+'\ndel "%~f0"\n',encoding='utf-8')
    script=("$s=New-Object -ComObject Shell.Application; $s.ShellExecute("+
            quote(batch)+",'',"+quote(working_directory)+",'open',1)")
    encoded=base64.b64encode(script.encode('utf-16le')).decode('ascii')
    subprocess.run(['powershell.exe','-NoProfile','-NonInteractive','-EncodedCommand',encoded],
                   check=True,creationflags=subprocess.CREATE_NO_WINDOW)


def paused_command(remote, command, transient):
    deadline = time.monotonic()+5
    while True:
        reply = remote.packet(command, timeout=30)
        if reply != transient or time.monotonic() >= deadline:
            return reply
        # Observe the same paused connection while the current CPU slice unwinds
        time.sleep(.02)


def completed_summary(session):
    if session.get('status') != 'complete' or digest(session['raw']) != session['raw_sha256']:
        raise ValueError('Completed recording changed or lacks completion evidence')
    result = session['capture']
    if not result['validation']['complete']:
        raise ValueError('Recording lacks validated completion')
    return {'name': session['name'], 'status': 'complete', 'vblanks': result['vblanks'],
            'game_boundaries': result['records'].get(1, result['records'].get('1', 0)),
            'controller_transfers': result['controller_transfers'],
            'seconds': result['summary']['seconds'], 'states': len(result['states']),
            'missing_vblanks': 0,
            'integrity_scope': 'Validated contiguous VBlank sequence and finalized raw SHA-256; not per-record checksums'}


def finalize(session, folder):
    with exclusive_finalize(folder):
        saved = folder/'session.json'
        if saved.exists():
            session = json.loads(saved.read_text())
        if session.get('status') == 'complete':
            return completed_summary(session)
        return finalize_locked(session, folder)


def finalize_locked(session, folder):
    runtime = Path(session['runtime'])
    source = runtime/'savestates'/('ff-trace-'+session['name']+'.bin')
    source_sha = digest(source)
    if session.get('status') == 'finalizing' and session.get('raw_sha256') != source_sha:
        raise ValueError('Raw recording changed after finalization started')
    session.update(status='finalizing', raw=str(source), raw_sha256=source_sha)
    write_receipt(folder/'session.json', session)
    output = folder/'decoded'
    attempt = 0
    while output.exists():
        attempt += 1
        output = folder/f'decoded-attempt-{attempt:04d}'
    try:
        result = decode(source, output)
    except Exception as error:
        session.update(status='capture_failed', decoded_directory=str(output), failure=str(error))
        write_receipt(folder/'session.json', session)
        raise
    if session.get('recording_settings'):
        verification = verify_recording_settings(output/'effective-settings.ini', session['recording_settings'])
        result['recording_settings_verification'] = verification
        if not verification['verified']:
            session.update(status='capture_failed', capture=result, decoded_directory=str(output),
                           failure='Recorded effective settings differ from the recording contract')
            write_receipt(folder/'session.json', session)
            raise ValueError(session['failure'])
    polls = decode_polls(json.loads((output/'controller-transfers.json').read_text()))
    write_receipt(output/'controller-polls.json', polls)
    result['controller_polls'] = {'count': len(polls['polls']), 'issues': len(polls['issues']),
                                  'path': str(output/'controller-polls.json'),
                                  'digital_polls_valid': polls['digital_polls_valid']}
    for state in result['states']:
        path = runtime/'savestates'/('ff-audit-'+state['name']+'.sav')
        state.update(path=str(path), sha256=digest(path))
    if digest(source) != source_sha:
        raise ValueError('Raw recording changed during finalization')
    session.update(status='complete', capture=result, decoded_directory=str(output))
    from trace_stage_package import export as export_stage_packages
    session['stage_packages'] = export_stage_packages(session, output/'stage-packages')
    if session.get('provenance'):
        session['provenance_verification'] = verify_files(json.loads(Path(session['provenance']).read_text()))
    write_receipt(folder/'session.json', session)
    return completed_summary(session)


def recover(session, folder):
    if session.get('status') == 'complete':
        return completed_summary(session)
    if session.get('status') == 'finalizing':
        return finalize(session, folder)
    if alive(session.get('process_identity')):
        raise RuntimeError('Recorder process is still live; stop the same recording instead of recovering')
    source = Path(session['runtime'])/'savestates'/('ff-trace-'+session['name']+'.bin')
    report = inspect_prefix(source)
    for state in report['checkpoints']:
        path = source.parent/('ff-audit-'+state['name']+'.sav')
        state.update(path=str(path), exists=path.is_file())
        if path.is_file():
            state['sha256'] = digest(path)
    index = report.pop('vblank_index')
    write_receipt(folder/'recovered-vblank-index.json', index)
    write_receipt(folder/'recovery.json', report)
    if report['footer_success'] and report['problem'] is None:
        return finalize(session, folder)
    session.update(status='incomplete', recovery=str(folder/'recovery.json'))
    write_receipt(folder/'session.json', session)
    return {'name': session['name'], 'status': 'incomplete', 'vblanks_recovered': len(index),
            'comparison_ready': False, 'report': str(folder/'recovery.json')}


def main():
    entered=time.perf_counter();timings={}
    def mark(label):timings[label]=round(time.perf_counter()-entered,6)
    user_role = 'user' if 'user' in PROJECT['duckstation']['reserved_gdb_ports'] else PROJECT['duckstation'].get('default_role', 'user')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['start', 'stop', 'status', 'recover'])
    parser.add_argument('--name', required=True)
    parser.add_argument('--state', type=Path)
    parser.add_argument('--seconds', type=int, default=600)
    parser.add_argument('--runtime', type=Path, default=data_directory(role=user_role))
    parser.add_argument('--port', type=int, default=PROJECT['duckstation']['reserved_gdb_ports'][user_role])
    parser.add_argument('--disc', type=Path, default=(ROOT/PROJECT['paths']['disc_image'] if PROJECT.get('paths', {}).get('disc_image') else None))
    parser.add_argument('--hidden', action='store_true', help='Hide the automated test window')
    args = parser.parse_args()
    args.runtime = args.runtime.resolve()
    args.disc = args.disc.resolve() if args.disc else None
    if not re.fullmatch('[A-Za-z0-9_-]{1,40}', args.name) or not 1 <= args.seconds <= 600:
        parser.error('Name must be 1РІР‚вЂњ40 ASCII letters/digits/_/-; duration 1РІР‚вЂњ600 seconds')
    folder = ROOT/'status/user-traces'/args.name
    session_path = folder/'session.json'
    if args.action in ('status', 'recover'):
        session = json.loads(session_path.read_text())
        if args.action == 'recover':
            print(json.dumps(recover(session, folder)))
        else:
            source = Path(session['runtime'])/'savestates'/('ff-trace-'+args.name+'.bin')
            print(json.dumps({'name': args.name, 'status': session['status'],
                              'process_live': alive(session.get('process_identity')),
                              'raw_bytes': source.stat().st_size if source.exists() else None,
                              'scope': 'Session metadata only; no emulator pause or completion inference'}))
        return
    if args.action == 'start':
        mark('arguments_ready')
        if not args.runtime.is_relative_to((ROOT/'tools').resolve()):
            parser.error('Runtime must be inside project tools')
        if args.port not in PROJECT['duckstation']['reserved_gdb_ports'].values():
            parser.error('GDB port is not reserved by this project')
        if args.disc is None or not args.disc.is_file():
            parser.error('Configure paths.disc_image or supply --disc')
        if not args.state or not args.state.is_file():
            parser.error('A full Save State file is required')
        for path in (ROOT/'status/user-traces').glob('*/session.json'):
            old = json.loads(path.read_text())
            if old.get('status') in ['starting', 'recording'] and alive(old.get('process_identity')):
                raise RuntimeError('A recording already owns a live emulator: '+str(path.parent.name))
        if folder.exists():
            raise FileExistsError('Recording name already exists: '+args.name)
        state_name = 'user-source-'+args.name
        state_path = args.runtime/'savestates'/('ff-audit-'+state_name+'.sav')
        if state_path.exists():
            raise ValueError('Source state destination exists')
        from duckstation_runtime import prepare_trace_profile
        prepare_trace_profile(args.runtime, args.port)
        recording = configure_recording_settings(args.runtime, args.port, args.hidden)
        mark('profile_settings_ready')
        folder.mkdir(parents=True, exist_ok=False)
        shutil.copy2(args.state, state_path)
        validate_settings(args.runtime, args.port)
        exe = executable()
        session = {'name': args.name, 'status': 'starting', 'runtime': str(args.runtime.resolve()),
                   'executable': str(exe), 'port': args.port, 'exe_sha256': digest(exe), 'state_sha256': digest(state_path),
                   'settings_sha256': digest(args.runtime/'settings.ini'), 'source_state': str(state_path),
                   'seconds_limit': args.seconds, 'recording_settings': recording}
        mark('state_and_identity_ready');session['startup_seconds']=timings
        write_receipt(session_path, session)
        try:
            process = listener_process(args.port)
        except OSError:
            launch = [str(exe)]
            if args.hidden:
                launch.append('-batch')
            launch.extend(['-fastboot', '-statefile', str(state_path), str(args.disc.resolve())])
            process_environment=environment(args.runtime)
            if args.hidden:
                startup = subprocess.STARTUPINFO()
                startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
                startup.wShowWindow = 0
                proc = subprocess.Popen(launch,cwd=args.runtime,startupinfo=startup,env=process_environment)
                process_id=proc.pid
                record_owner(args.runtime, args.port, process_id)
                session['process_identity'] = process_identity(process_id)
            else:
                launch_visible(launch,args.runtime,process_environment)
            write_receipt(session_path, session)
            deadline = time.monotonic()+30
            while True:
                try:
                    process = listener_process(args.port)
                    break
                except OSError:
                    if (session.get('process_identity') and not alive(session['process_identity'])) or time.monotonic() >= deadline:
                        raise RuntimeError('Existing launched process has not exposed GDB; inspect it without restarting')
                    time.sleep(.1)
        if Path(process['Path']).resolve() != exe.resolve():
            raise RuntimeError('Port belongs to another runtime')
        if not session.get('process_identity'):
            record_owner(args.runtime, args.port, process['Id'])
            session['process_identity'] = process_identity(process['Id'])
            write_receipt(session_path, session)
        verify_owner(args.runtime, args.port, process)
        mark('emulator_ready')
        session['process_identity'] = process_identity(process['Id'])
        write_receipt(session_path, session)
        remote = Remote('127.0.0.1', args.port)
        try:
            reply = paused_command(remote, 'qFFLoadState:'+state_name.encode().hex(), 'E10')
            if reply != 'OK':
                session.update(status='start_failed', failure='State load returned '+reply)
                write_receipt(session_path, session)
                raise RuntimeError('State load failed: '+reply)
            mark('state_loaded')
            provenance = inventory(args.runtime, args.disc, state_path,
                                   [Path(__file__), implementation_path('trace_internal'), implementation_path('trace_vblank'),
                                    implementation_path('trace_user_input'), implementation_path('trace_user_provenance'),
                                    Path(__file__).with_name('gdb_remote.py'), Path(__file__).with_name('shared_ram.py'), implementation_path('trace_layout'),
                                    implementation_path('trace_profile'), ROOT/PROJECT['duckstation']['trace_profile'],
                                    ROOT/'xport-project.json'], installation=exe.parent)
            write_receipt(folder/'provenance.json', provenance)
            session['provenance'] = str(folder/'provenance.json')
            session['provenance_sha256'] = digest(folder/'provenance.json')
            mark('provenance_ready')
            reply = remote.packet('qFFUserStart:'+args.name.encode().hex()+','+struct.pack('<I', args.seconds).hex())
            if reply != 'OK':
                session.update(status='start_failed', failure='Recorder start returned '+reply)
                write_receipt(session_path, session)
                raise RuntimeError('Recording start failed: '+reply)
            session['status'] = 'recording'
            session['started_wall_time'] = time.time()
            write_receipt(session_path, session)
            remote.resume_acknowledged()
            mark('resumed');session['resumed_wall_time']=time.time()
            write_receipt(session_path,session)
        finally:
            remote.close()
        print(json.dumps({'name': args.name, 'status': 'recording', 'seconds_limit': args.seconds,'startup_seconds':timings}))
    else:
        session = json.loads(session_path.read_text())
        if session.get('status') == 'complete':
            print(json.dumps(completed_summary(session)))
            return
        if session.get('status') == 'finalizing':
            print(json.dumps(finalize(session, folder)))
            return
        if session['status'] != 'recording' or not alive(session['process_identity']):
            raise RuntimeError('Recorded emulator is not live; preserve artifacts for recovery')
        process = listener_process(session['port'])
        if process['Id'] != session['process_identity']['pid']:
            raise RuntimeError('GDB listener owner changed')
        remote = Remote('127.0.0.1', session['port'])
        try:
            reply = paused_command(remote, 'qFFUserStop', 'E20')
            if reply != 'OK':
                if reply == 'E23':
                    session.update(status='capture_failed', failure='Recorder closed and disarmed with E23')
                    write_receipt(session_path, session)
                raise RuntimeError('Recorder stop returned '+reply+'; raw partial artifacts are preserved')
        finally:
            remote.close()
        print(json.dumps(finalize(session, folder)))


if __name__ == '__main__':
    main()

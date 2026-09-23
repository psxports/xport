"""Launch one shared DuckStation with isolated project data and endpoint ownership"""
import argparse
import configparser
import json
from pathlib import Path
import subprocess
import time
from xport_project import load_project, project_path
from duckstation_runtime import executable, data_directory, environment, validate_settings, record_owner, verify_owner
from gdb_endpoint import listener_process


def show_existing_window(pid):
    import ctypes
    from ctypes import wintypes as w
    user = ctypes.WinDLL('user32', use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    user.EnumWindows.argtypes = [callback_type, w.LPARAM]
    user.GetWindowThreadProcessId.argtypes = [w.HWND, ctypes.POINTER(w.DWORD)]
    user.IsWindowVisible.argtypes = [w.HWND]
    user.IsIconic.argtypes = [w.HWND]
    user.ShowWindow.argtypes = [w.HWND, ctypes.c_int]
    user.SetForegroundWindow.argtypes = [w.HWND]
    windows = []
    @callback_type
    def collect(hwnd, unused):
        owner = w.DWORD()
        user.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user.IsWindowVisible(hwnd):
            windows.append(hwnd)
        return True
    user.EnumWindows(collect, 0)
    if not windows:
        return False
    hwnd = windows[0]
    if user.IsIconic(hwnd):
        user.ShowWindow(hwnd, 9)
    return bool(user.SetForegroundWindow(hwnd))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--role')
    parser.add_argument('--game-exe',action='store_true')
    parser.add_argument('--state',type=Path)
    parser.add_argument('--slot',type=int)
    parser.add_argument('--visible',action='store_true')
    parser.add_argument('--plan',action='store_true')
    args=parser.parse_args()
    root,config=load_project()
    role=args.role or config['duckstation'].get('default_role','user')
    port=config['duckstation']['reserved_gdb_ports'][role]
    data=data_directory(role=role)
    validate_settings(data,port)
    exe=executable()
    command=[str(exe),'-fastboot']
    if not args.visible:command.append('-batch')
    if args.state:
        command.extend(['-statefile',str(args.state.resolve(strict=True))])
    if args.slot is not None:
        if not 1<=args.slot<=10 or args.state:parser.error('Choose one state file or slot 1..10')
        command.extend(['-state',str(args.slot)])
    command.append(str(project_path('original_executable' if args.game_exe else 'disc_image')))
    if args.plan:
        print(json.dumps({'command':command,'data_directory':str(data),'port':port,'visible':args.visible}))
        return
    try:owner=listener_process(port)
    except OSError:owner=None
    if owner:
        try:
            verify_owner(data, port, owner)
        except (OSError, ValueError, RuntimeError, KeyError) as error:
            parser.exit(1, f'Port {port} belongs to an unverified process (PID {owner["Id"]}); no changes made. {error}\n')
        if not args.visible or args.state or args.slot is not None or args.game_exe:
            parser.exit(1, f'DuckStation is already running for this role (PID {owner["Id"]}, port {port}). Close it before starting another scenario; its current state was preserved.\n')
        focused = show_existing_window(owner['Id'])
        print('DuckStation is already running.' if focused else
              'DuckStation is already running. Select its window from the taskbar or Alt+Tab.')
        return
    ini=configparser.ConfigParser(interpolation=None);ini.optionxform=str
    ini.read(data/'settings.ini',encoding='utf-8-sig')
    ini['Main']['StartPaused']='false' if args.visible else 'true'
    ini['Main']['EmulationSpeed']='1' if args.visible else '0'
    ini['BIOS']['PatchFastBoot']='true'
    with (data/'settings.ini').open('w',encoding='utf-8') as stream:ini.write(stream)
    startup=subprocess.STARTUPINFO()
    if not args.visible:
        startup.dwFlags|=subprocess.STARTF_USESHOWWINDOW;startup.wShowWindow=0
    process=subprocess.Popen(command,cwd=data,env=environment(data),startupinfo=startup)
    receipt=record_owner(data,port,process.pid)
    deadline=time.monotonic()+30
    while True:
        try:
            owner=listener_process(port)
            if owner['Id']!=process.pid or Path(owner['Path']).resolve()!=exe:
                raise RuntimeError('Unexpected GDB endpoint owner')
            break
        except OSError:
            if process.poll() is not None or time.monotonic()>deadline:
                raise RuntimeError('Launched process has not exposed its endpoint; inspect the same PID, do not restart')
            time.sleep(.1)
    if args.visible:
        print('DuckStation started.')
    else:
        print(json.dumps({'pid':process.pid,'executable':str(exe),'data_directory':str(data),'port':port,'visible':args.visible}))


if __name__=='__main__':main()

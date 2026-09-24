"""Build the configured native port with Visual Studio 2022"""
import argparse
import json
import os
from pathlib import Path
import subprocess
from xport_project import project_path, artifact_path
from xport_process import normalized_environment


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--msbuild', type=Path)
    parser.add_argument('--configuration', choices=('Debug', 'Release'), default='Debug')
    parser.add_argument('--plan', action='store_true')
    args = parser.parse_args()
    executable = project_path('native_executable')
    solution = project_path('native_solution')
    if not solution.is_file():
        parser.error('Configured solution does not exist')
    msbuild = args.msbuild
    if msbuild is None:
        vswhere = Path(os.environ['ProgramFiles(x86)'])/'Microsoft Visual Studio/Installer/vswhere.exe'
        paths = subprocess.check_output([str(vswhere), '-latest', '-version', '[17.0,18.0)',
                  '-products', '*', '-requires', 'Microsoft.Component.MSBuild', '-find',
                  r'MSBuild\**\Bin\MSBuild.exe'], text=True,
                  env=normalized_environment()).splitlines()
        if not paths:
            raise RuntimeError('Visual Studio 2022 MSBuild not found')
        msbuild = Path(paths[0])
    command = [str(msbuild), str(solution), '/p:Configuration='+args.configuration, '/p:Platform=x86',
               '/m', '/nr:false', '/v:minimal', '/nologo']
    if args.plan:
        print(json.dumps({'command': command, 'configuration': args.configuration, 'executable': str(executable),
                          'working_directory': str(project_path('native_working_directory', 'bin'))}))
        return
    # Preserve a running manual game before touching its executable
    name = executable.stem.replace("'", "''")
    guard = subprocess.run(['powershell', '-NoProfile', '-Command',
                            "if (Get-Process -Name '"+name+"' -ErrorAction SilentlyContinue) { exit 1 }"],
                           capture_output=True)
    if guard.returncode:
        raise RuntimeError('Native executable is running; preserve manual game')
    log = artifact_path('status/build/native-build.log')
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open('wb') as stream:
        result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT,
                                env=normalized_environment())
    print(json.dumps({'exit_code': result.returncode, 'configuration': args.configuration, 'log': str(log), 'executable': str(executable)}))
    if result.returncode:
        raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()

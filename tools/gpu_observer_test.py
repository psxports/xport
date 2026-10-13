"""Build and execute a native GPU observer fixture in the consuming project"""
import json
import argparse
import os
from pathlib import Path
import subprocess

from xport_process import normalized_environment
from xport_project import project_path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixture', choices=('gpu_observer', 'vsync_callback', 'spu_control', 'spu_pending_key', 'pad_replay'), default='gpu_observer')
    parser.add_argument('--configuration', choices=('Debug', 'Release'), default='Debug')
    args = parser.parse_args()
    fixture = args.fixture
    env = normalized_environment()
    vswhere = Path(os.environ['ProgramFiles(x86)']) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    matches = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires',
                                      'Microsoft.Component.MSBuild', '-find', r'MSBuild\**\Bin\MSBuild.exe'],
                                     env=env, text=True).splitlines()
    if not matches:
        raise RuntimeError('Visual Studio MSBuild unavailable')
    output = project_path('native_build') / fixture.replace('_', '-')
    if args.configuration == 'Debug':
        output /= 'Debug'
    output.mkdir(parents=True, exist_ok=True)
    project = Path(__file__).parent / 'tests/native/gpu_observer.vcxproj'
    command = [matches[0], str(project), '/p:Configuration=' + args.configuration, '/p:Platform=Win32', '/p:NativeFixture=' + fixture,
               '/p:OutDir=' + output.as_posix() + '/',
               '/p:IntDir=' + (output / 'obj').as_posix() + '/', '/verbosity:minimal']
    with (output / 'test.log').open('w', encoding='utf-8') as log:
        code = subprocess.call(command, env=env, stdout=log, stderr=subprocess.STDOUT)
        if not code:
            code = subprocess.call([str(output / (fixture + '.exe'))], env=env,
                                   stdout=log, stderr=subprocess.STDOUT, timeout=30)
    print(json.dumps(dict(exit_code=code, configuration=args.configuration, log=str(output / 'test.log'))))
    raise SystemExit(code)


if __name__ == '__main__':
    main()

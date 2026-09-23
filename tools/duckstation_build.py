"""Reproduce and publish the shared xport DuckStation runtime"""

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import urllib.request
import uuid


DEV_SUFFIXES = {'.exp', '.ilk', '.iobj', '.ipdb', '.lib', '.pdb'}


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def atomic_json(path, value):
    path = Path(path)
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    os.replace(temporary, path)


def command(argv, cwd=None, capture=True, log=None):
    kwargs = dict(cwd=cwd, env=os.environ.copy(), text=log is None)
    if log is not None:
        kwargs.update(stdout=log, stderr=subprocess.STDOUT)
    elif capture:
        kwargs.update(stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    result = subprocess.run([str(item) for item in argv], **kwargs)
    if result.returncode:
        output = '' if log is not None else (result.stdout or '')
        raise RuntimeError(f'Command failed ({result.returncode}): {argv[0]}\n{output[-4000:]}')
    return '' if log is not None else (result.stdout or '')


def resolve_tool(explicit, names, candidates):
    if explicit:
        path = Path(explicit).expanduser().resolve()
        if path.is_file():
            return path
        raise FileNotFoundError(path)
    for candidate in candidates:
        expanded = Path(os.path.expandvars(candidate))
        if expanded.is_file():
            return expanded.resolve()
    for name in names:
        found = shutil.which(name)
        if found:
            return Path(found).resolve()
    raise FileNotFoundError('Required tool not found: ' + ', '.join(names))


def load_manifest(shared):
    path = shared / 'duckstation' / 'duckstation-manifest.json'
    manifest = json.loads(path.read_text(encoding='utf-8'))
    if manifest.get('schema') != 1:
        raise ValueError('Unsupported DuckStation manifest schema')
    patch = shared / 'duckstation' / manifest['patch']['path']
    actual = sha256(patch)
    expected = manifest['patch']['sha256'].lower()
    if actual != expected:
        raise ValueError(f'Patch hash mismatch: expected {expected}, got {actual}')
    headers = [line for line in patch.read_text(encoding='utf-8').splitlines()
               if line.startswith('+++ ') and line != '+++ /dev/null']
    if not headers or any(not line.startswith('+++ b/') for line in headers):
        raise ValueError('Patch paths must be relative to a clean upstream checkout')
    return path, manifest, patch


def identity(manifest_path, patch, script):
    digest = hashlib.sha256()
    for path in (manifest_path, patch, script):
        digest.update(path.name.encode('utf-8'))
        digest.update(path.read_bytes())
    return digest.hexdigest()


def default_cache():
    base = os.environ.get('LOCALAPPDATA') or str(Path.home() / 'AppData' / 'Local')
    return Path(base) / 'xport' / 'duckstation-cache'


def ensure_git_cache(cache, manifest, git, offline):
    repository = cache / 'upstream.git'
    commit = manifest['upstream']['commit']
    url = manifest['upstream']['repository']
    if not repository.is_dir():
        if offline:
            raise RuntimeError('Offline mode requires the cached upstream repository')
        repository.parent.mkdir(parents=True, exist_ok=True)
        command([git, 'init', '--bare', repository])
        command([git, '--git-dir', repository, 'remote', 'add', 'origin', url])
    else:
        remotes = command([git, '--git-dir', repository, 'remote']).split()
        if 'origin' not in remotes:
            command([git, '--git-dir', repository, 'remote', 'add', 'origin', url])
        elif command([git, '--git-dir', repository, 'remote', 'get-url', 'origin']).strip() != url:
            raise RuntimeError('Cached upstream repository URL differs from the manifest')
    present = subprocess.run([str(git), '--git-dir', str(repository), 'cat-file', '-e', f'{commit}^{{commit}}'],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
    if not present:
        if offline:
            raise RuntimeError(f'Offline cache does not contain upstream commit {commit}')
        command([git, '--git-dir', repository, 'fetch', '--depth=1', 'origin',
                 f'+{commit}:refs/heads/xport-base'])
    return repository


def ensure_archive(cache, dependency, offline):
    archive = cache / dependency['archive']
    expected = dependency['sha256'].lower()
    if archive.is_file() and sha256(archive) == expected:
        return archive
    if archive.exists():
        archive.unlink()
    if offline:
        raise RuntimeError(f'Offline cache does not contain {dependency["archive"]}')
    cache.mkdir(parents=True, exist_ok=True)
    partial = archive.with_suffix(archive.suffix + '.part')
    partial.unlink(missing_ok=True)
    print(f'Downloading {dependency["url"]}', flush=True)
    downloaded = 0
    with urllib.request.urlopen(dependency['url']) as response, partial.open('wb') as output:
        while True:
            block = response.read(1024 * 1024)
            if not block:
                break
            output.write(block)
            downloaded += len(block)
            if downloaded % (64 * 1024 * 1024) < len(block):
                print(f'  {downloaded // (1024 * 1024)} MiB', flush=True)
    actual = sha256(partial)
    if actual != expected:
        partial.unlink(missing_ok=True)
        raise ValueError(f'Dependency hash mismatch: expected {expected}, got {actual}')
    os.replace(partial, archive)
    return archive


def prepare_source(workspace, cache_repository, archive, manifest, patch, git, seven_zip):
    source = workspace / 'source'
    def source_git(*arguments):
        return [git, '-c', f'safe.directory={source.as_posix()}', '-C', source, *arguments]

    if not source.is_dir():
        command([git, 'clone', '--no-checkout', cache_repository, source])
        command(source_git('checkout', '--detach', manifest['upstream']['commit']))
    head = command(source_git('rev-parse', 'HEAD')).strip()
    if head != manifest['upstream']['commit']:
        raise RuntimeError(f'Workspace HEAD is {head}, expected {manifest["upstream"]["commit"]}')
    reverse = subprocess.run([str(item) for item in source_git('apply', '--reverse', '--check', patch)],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if reverse.returncode:
        command(source_git('apply', '--check', patch))
        command(source_git('apply', patch))
    command(source_git('diff', '--check'))
    prebuilt = source / 'dep' / 'prebuilt'
    dependency_root = prebuilt / 'windows-x64'
    if not dependency_root.is_dir():
        prebuilt.mkdir(parents=True, exist_ok=True)
        command([seven_zip, 'x', archive, f'-o{prebuilt}', '-y'])
    if not dependency_root.is_dir():
        raise RuntimeError('Dependency archive did not create dep/prebuilt/windows-x64')
    version_file = source / 'dep' / 'PREBUILT-VERSION'
    expected_version = manifest['dependencies']['windows-x64']['version']
    if version_file.read_text(encoding='utf-8').strip() != expected_version:
        raise RuntimeError('Upstream dependency version differs from the pinned dependency pack')
    scm = source / 'src' / 'scmversion'
    values = manifest['upstream']
    (scm / 'scmversion.cpp').write_text(
        f'// Generated by the xport DuckStation bootstrap\n'
        f'const char* g_scm_hash_str = "{values["commit"]}";\n'
        'const char* g_scm_branch_str = "xport-profile";\n'
        'const char* g_scm_tag_str = "xport-local";\n'
        f'const char* g_scm_version_str = "{values["version"]}-xport";\n'
        'const char* g_scm_date_str = "reproducible-local-build";\n', encoding='utf-8')
    (scm / 'gen_scmversion.bat').write_text('@echo off\r\nexit /b 0\r\n', encoding='ascii')
    return source


def build_source(workspace, source, manifest, msbuild):
    build = manifest['build']
    argv = [msbuild, source / build['solution'], f'/t:{build["target"]}',
            f'/p:Configuration={build["configuration"]}', f'/p:Platform={build["platform"]}',
            f'/p:PlatformToolset={build["toolset"]}', f'/m:{build["max_cpu_count"]}',
            '/nr:false', '/v:minimal', '/nologo']
    log_path = workspace / 'build.log'
    print('Building DuckStation Release x64 v143', flush=True)
    with log_path.open('w', encoding='utf-8', errors='replace') as log:
        command(argv, cwd=source, capture=False, log=log)
    executable = source / Path(build['source_executable'])
    if not executable.is_file():
        raise RuntimeError(f'Build completed without {build["source_executable"]}')
    return argv, log_path


def package_runtime(workspace, source, manifest, manifest_identity, build_command):
    package = workspace / 'package'
    if package.exists():
        shutil.rmtree(package)
    package.mkdir(parents=True)
    source_bin = source / 'bin' / 'x64'
    source_exe = Path(manifest['build']['source_executable']).name
    target_exe = manifest['build']['distribution_executable']
    for item in source_bin.rglob('*'):
        relative = item.relative_to(source_bin)
        if item.is_dir() or item.suffix.lower() in DEV_SUFFIXES:
            continue
        if item.suffix.lower() == '.exe' and item.name != source_exe:
            continue
        destination = package / relative
        if item.name == source_exe:
            destination = destination.with_name(target_exe)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(item, destination)
    license_path = package / 'LICENSE.txt'
    if not license_path.is_file():
        shutil.copy2(source / 'LICENSE', license_path)
    validate_package(package, manifest)
    hashes = {path.relative_to(package).as_posix(): sha256(path)
              for path in sorted(package.rglob('*')) if path.is_file()}
    executable_hash = hashes[target_exe]
    receipt = {
        'schema': 2,
        'manifest_identity': manifest_identity,
        'upstream_commit': manifest['upstream']['commit'],
        'upstream_version': manifest['upstream']['version'],
        'patch_sha256': manifest['patch']['sha256'],
        'dependency_version': manifest['dependencies']['windows-x64']['version'],
        'dependency_sha256': manifest['dependencies']['windows-x64']['sha256'],
        'configuration': 'Release x64 v143',
        'compatibility_executable_name': target_exe,
        'exe_sha256': executable_hash,
        'trace_profile_version': manifest['runtime']['trace_profile_version'],
        'stage_package_schema': manifest['runtime']['stage_package_schema'],
        'device_serializer_version': manifest['runtime']['device_serializer_version'],
        'data_root_environment': manifest['runtime']['data_root_environment'],
        'runtime_files': hashes,
        'validation': {
            'build': 'passed',
            'required_files': 'passed',
            'required_executable_markers': 'passed'
        },
        'generated_utc': dt.datetime.now(dt.timezone.utc).isoformat(),
        'build_parameters': {
            'target': manifest['build']['target'],
            'configuration': manifest['build']['configuration'],
            'platform': manifest['build']['platform'],
            'toolset': manifest['build']['toolset'],
            'max_cpu_count': manifest['build']['max_cpu_count']
        }
    }
    atomic_json(package / 'xport-runtime.json', receipt)
    atomic_json(workspace / 'build.json', {
        'status': 'complete', 'manifest_identity': manifest_identity,
        'package': str(package), 'command': [str(item) for item in build_command],
        'executable_sha256': executable_hash
    })
    return package, receipt


def validate_package(package, manifest):
    missing = [name for name in manifest['runtime']['required_files'] if not (package / name).is_file()]
    if missing:
        raise RuntimeError('Runtime package is incomplete: ' + ', '.join(missing))
    executable = package / manifest['build']['distribution_executable']
    data = executable.read_bytes()
    missing_markers = []
    for marker in manifest['runtime']['required_executable_markers']:
        if marker.encode('ascii') not in data and marker.encode('utf-16le') not in data:
            missing_markers.append(marker)
    if missing_markers:
        raise RuntimeError('Built executable lacks xport markers: ' + ', '.join(missing_markers))


def running_executable(name):
    result = subprocess.run(['tasklist', '/FI', f'IMAGENAME eq {name}', '/FO', 'CSV', '/NH'],
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    return result.returncode == 0 and name.lower() in result.stdout.lower()


def publish(package, destination, manifest):
    executable = manifest['build']['distribution_executable']
    if running_executable(executable):
        raise RuntimeError(f'{executable} is running; close the emulator before publishing')
    parent = destination.parent
    parent.mkdir(parents=True, exist_ok=True)
    token = uuid.uuid4().hex
    incoming = parent / f'.distribution-new-{token}'
    previous = parent / f'.distribution-old-{token}'
    shutil.copytree(package, incoming)
    validate_package(incoming, manifest)
    moved_previous = False
    try:
        if destination.exists():
            destination.rename(previous)
            moved_previous = True
        incoming.rename(destination)
    except Exception:
        if moved_previous and not destination.exists() and previous.exists():
            previous.rename(destination)
        raise
    finally:
        if incoming.exists():
            shutil.rmtree(incoming)
    if previous.exists():
        shutil.rmtree(previous)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, default=default_cache())
    parser.add_argument('--workspace', type=Path)
    parser.add_argument('--msbuild')
    parser.add_argument('--seven-zip')
    parser.add_argument('--git')
    parser.add_argument('--offline', action='store_true')
    parser.add_argument('--force', action='store_true')
    parser.add_argument('--no-build', action='store_true')
    parser.add_argument('--no-publish', action='store_true')
    parser.add_argument('--clean-workspace', action='store_true')
    parser.add_argument('--verify-only', action='store_true')
    args = parser.parse_args()

    shared = Path(__file__).resolve().parent
    manifest_path, manifest, patch = load_manifest(shared)
    manifest_identity = identity(manifest_path, patch, Path(__file__).resolve())
    git = resolve_tool(args.git, ['git.exe', 'git'],
                       [r'C:\Program Files\Git\cmd\git.exe'])
    seven_zip = resolve_tool(args.seven_zip, ['7z.exe', '7z'],
                             [r'C:\Program Files\7-Zip\7z.exe'])
    msbuild = resolve_tool(args.msbuild, ['MSBuild.exe'], [
        r'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe',
        r'C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe',
        r'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe'])
    if args.verify_only:
        print(json.dumps({'status': 'verified', 'manifest_identity': manifest_identity,
                          'upstream_commit': manifest['upstream']['commit'],
                          'patch_sha256': manifest['patch']['sha256'],
                          'tools': {'git': str(git), 'seven_zip': str(seven_zip), 'msbuild': str(msbuild)}}))
        return

    cache = args.cache.expanduser().resolve()
    workspace = (args.workspace.expanduser().resolve() if args.workspace else
                 cache.parent / 'duckstation-build' / manifest_identity[:16])
    if args.force and workspace.exists():
        shutil.rmtree(workspace)
    workspace.mkdir(parents=True, exist_ok=True)
    repository = ensure_git_cache(cache, manifest, git, args.offline)
    dependency = manifest['dependencies']['windows-x64']
    archive = ensure_archive(cache, dependency, args.offline)
    source = prepare_source(workspace, repository, archive, manifest, patch, git, seven_zip)
    if args.no_build:
        if not args.no_publish:
            raise ValueError('--no-build requires --no-publish')
        print(json.dumps({'status': 'source_prepared', 'workspace': str(workspace),
                          'manifest_identity': manifest_identity}))
        return
    build_command, log = build_source(workspace, source, manifest, msbuild)
    package, receipt = package_runtime(workspace, source, manifest, manifest_identity, build_command)
    if not args.no_publish:
        publish(package, shared / 'duckstation' / 'distribution', manifest)
    result = {'status': 'prepared', 'published': not args.no_publish,
              'workspace': str(workspace), 'build_log': str(log),
              'manifest_identity': manifest_identity, 'exe_sha256': receipt['exe_sha256']}
    if args.clean_workspace:
        shutil.rmtree(workspace)
        result['workspace_removed'] = True
    print(json.dumps(result))


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print(json.dumps({'status': 'failed', 'error': str(error)}), file=sys.stderr)
        raise SystemExit(1)

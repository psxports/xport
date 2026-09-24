"""Fetch and build the pinned ADPCM-XQ encoder"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import urllib.request
import zipfile


COMMIT = '2000733c6d92d73c10ddae3ae80232e173d565c8'
ARCHIVE_URL = f'https://github.com/dbry/adpcm-xq/archive/{COMMIT}.zip'
ARCHIVE_SHA256 = '827c01ac5ad102923e2a28c037de36d0f1a813c7e8a26b033eb80d84cb7d69c0'
SOURCE_FILES = ('adpcm-xq.c', 'adpcm-lib.c', 'adpcm-lib.h', 'adpcm-dns.c', 'license.txt')


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        while True:
            block = source.read(1024 * 1024)
            if not block:
                return digest.hexdigest()
            digest.update(block)


def download_archive(cache):
    archive = cache / f'adpcm-xq-{COMMIT}.zip'
    if archive.is_file() and sha256(archive) == ARCHIVE_SHA256:
        return archive
    archive.unlink(missing_ok=True)
    cache.mkdir(parents=True, exist_ok=True)
    temporary = archive.with_suffix('.download')
    temporary.unlink(missing_ok=True)
    request = urllib.request.Request(ARCHIVE_URL, headers={'User-Agent': 'xport-prepare'})
    with urllib.request.urlopen(request, timeout=60) as response, temporary.open('wb') as output:
        shutil.copyfileobj(response, output)
    actual = sha256(temporary)
    if actual != ARCHIVE_SHA256:
        temporary.unlink(missing_ok=True)
        raise RuntimeError(f'ADPCM-XQ archive SHA-256 mismatch: {actual}')
    temporary.replace(archive)
    return archive


def extract_source(archive, cache):
    source = cache / f'adpcm-xq-{COMMIT}'
    if source.is_dir() and all((source / name).is_file() for name in SOURCE_FILES):
        return source
    temporary = cache / f'.adpcm-xq-{COMMIT}.extracting'
    if temporary.exists():
        shutil.rmtree(temporary)
    temporary.mkdir()
    prefix = f'adpcm-xq-{COMMIT}/'
    with zipfile.ZipFile(archive) as package:
        names = set(package.namelist())
        for name in SOURCE_FILES:
            member = prefix + name
            if member not in names:
                raise RuntimeError('Pinned ADPCM-XQ archive lacks ' + name)
            destination = temporary / name
            with package.open(member) as input_file, destination.open('wb') as output:
                shutil.copyfileobj(input_file, output)
    if source.exists():
        shutil.rmtree(source)
    temporary.replace(source)
    return source


def visual_studio_environment():
    vswhere = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio' / 'Installer' / 'vswhere.exe'
    if not vswhere.is_file():
        raise RuntimeError('Visual Studio 2022 with C tools is required to build ADPCM-XQ')
    installation = subprocess.run([str(vswhere), '-latest', '-products', '*',
                                   '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                                   '-property', 'installationPath'], capture_output=True, text=True, check=True).stdout.strip()
    vcvars = Path(installation) / 'VC' / 'Auxiliary' / 'Build' / 'vcvars32.bat'
    if not vcvars.is_file():
        raise RuntimeError('Visual Studio x86 compiler environment was not found')
    return vcvars


def build(source, output):
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name('adpcm-xq.building.exe')
    temporary.unlink(missing_ok=True)
    with tempfile.TemporaryDirectory(prefix='xport-adpcm-xq-') as directory:
        batch = Path(directory) / 'build.bat'
        sources = ' '.join(f'"{source / name}"' for name in SOURCE_FILES[:3] if name != 'adpcm-lib.h')
        sources += f' "{source / "adpcm-dns.c"}"'
        batch.write_text('@echo off\ncall "{}" >nul\nif errorlevel 1 exit /b %errorlevel%\n'
                         'cl /nologo /O2 /std:c11 /D_CRT_SECURE_NO_WARNINGS {} /Fe:"{}"\n'.format(
                             visual_studio_environment(), sources, temporary))
        subprocess.run(['cmd.exe', '/d', '/c', str(batch)], check=True)
    temporary.replace(output)


def main():
    tool = Path(__file__).resolve().parent
    output = tool / 'adpcm-xq.exe'
    receipt = tool / '.build.json'
    if output.is_file() and receipt.is_file():
        state = json.loads(receipt.read_text(encoding='utf-8'))
        if state.get('commit') == COMMIT and state.get('archive_sha256') == ARCHIVE_SHA256 and state.get('executable_sha256') == sha256(output):
            print('ADPCM-XQ is already prepared')
            return
    cache = Path(os.environ.get('LOCALAPPDATA', Path.home() / 'AppData' / 'Local')) / 'xport' / 'sources' / 'adpcm-xq'
    archive = download_archive(cache)
    source = extract_source(archive, cache)
    build(source, output)
    state = dict(commit=COMMIT, archive_url=ARCHIVE_URL, archive_sha256=ARCHIVE_SHA256,
                 executable_sha256=sha256(output))
    temporary = receipt.with_suffix('.writing')
    temporary.write_text(json.dumps(state, indent=2) + '\n', encoding='utf-8')
    temporary.replace(receipt)
    print('Prepared ADPCM-XQ ' + COMMIT)


if __name__ == '__main__':
    main()

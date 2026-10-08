"""Prepare and validate the shared licensed analysis toolchain"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


TOOLS = Path(__file__).resolve().parent
SCHEMA = 1
CAPABILITIES = {
    'ida_processor': True,
    'ida_decompiler': True,
    'ghidra_psx_language': True,
    'ghidra_psyq_script': True,
}
PSX_EXTENSION_RELEASES = 'https://github.com/lab313ru/ghidra_psx_ldr/releases'


def digest_file(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024*1024), b''):
            value.update(block)
    return value.hexdigest()


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name+'.tmp')
    temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False)+'\n', encoding='utf-8')
    temporary.replace(path)


def receipt_path():
    override = os.environ.get('XPORT_TOOLCHAIN_RECEIPT')
    if override:
        return Path(override).expanduser().resolve()
    base = Path(os.environ['LOCALAPPDATA'])/'xport' if os.environ.get('LOCALAPPDATA') else Path.home()/'.xport'
    return base/'toolchain-preflight-v1.json'


def tool_cache_path():
    override = os.environ.get('XPORT_TOOL_CACHE')
    if override:
        return Path(override).expanduser().resolve()
    base = Path(os.environ['LOCALAPPDATA'])/'xport' if os.environ.get('LOCALAPPDATA') else Path.home()/'.xport'
    return base/'tool-paths.json'


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def cached_tools():
    try:
        value = read_json(tool_cache_path())
        return value.get('tools', {}) if value.get('schema') == 1 else {}
    except (OSError, ValueError, json.JSONDecodeError):
        return {}


def standard_candidates():
    roaming = os.environ.get('APPDATA')
    ida = sorted(Path('C:/Tools').glob('IDA*/idat.exe'), reverse=True)
    ghidra = sorted(Path('C:/Tools').glob('ghidra_*_PUBLIC'), reverse=True)
    plugin = []
    if roaming:
        plugin = sorted((path.parent.parent for path in Path(roaming).glob('ghidra/*/Extensions/*/data/psyq')), reverse=True)
    java = []
    found_java = shutil.which('java.exe') or shutil.which('java')
    if found_java:
        java.append(Path(found_java))
    java.extend(sorted(Path('C:/Program Files/Java').glob('jdk*/bin/java.exe'), reverse=True))
    return {'ida': ida, 'ghidra': ghidra, 'plugin': plugin, 'java': java}


def matching_ghidra(plugin, candidates):
    plugin = Path(plugin).resolve()
    profile = plugin.parent.parent.name if plugin.parent.name == 'Extensions' else None
    for candidate in candidates:
        candidate = Path(candidate).expanduser().resolve()
        if profile and candidate.name == profile and (candidate/'support/analyzeHeadless.bat').is_file():
            return candidate
    return None


def discover_tools(supplied=None):
    supplied = supplied or {}
    cached = cached_tools()
    standard = standard_candidates()
    plugin_values = (supplied.get('plugin'), os.environ.get('XPORT_PSX_PLUGIN'), cached.get('plugin'), *standard['plugin'])
    plugin = next((Path(value).expanduser().resolve() for value in plugin_values
                   if value and (Path(value).expanduser()/'data/psyq').is_dir()), None)
    ghidra_values = [value for value in (supplied.get('ghidra'), os.environ.get('XPORT_GHIDRA'),
                     os.environ.get('GHIDRA_HOME'), cached.get('ghidra'), *standard['ghidra']) if value]
    paired = matching_ghidra(plugin, ghidra_values) if plugin else None
    checks = {
        'ida': lambda path: path.is_file(),
        'ghidra': lambda path: (path/'support/analyzeHeadless.bat').is_file(),
        'java': lambda path: path.is_file(),
    }
    values = {
        'ida': (supplied.get('ida'), os.environ.get('XPORT_IDA'), os.environ.get('IDAT_PATH'), cached.get('ida'), *standard['ida']),
        'ghidra': (paired, *ghidra_values),
        'java': (supplied.get('java'), os.environ.get('XPORT_JAVA'),
                 Path(os.environ['JAVA_HOME'])/'bin/java.exe' if os.environ.get('JAVA_HOME') else None,
                 cached.get('java'), *standard['java']),
    }
    found = {'plugin': str(plugin)} if plugin else {}
    for name, candidates in values.items():
        for value in candidates:
            if value:
                path = Path(value).expanduser().resolve()
                if checks[name](path):
                    found[name] = str(path)
                    break
    absent = [name for name in ('ida', 'ghidra', 'java', 'plugin') if name not in found]
    if absent:
        hint = '; PSX extension releases by Ghidra version: '+PSX_EXTENSION_RELEASES if 'plugin' in absent else ''
        raise ValueError('Missing analysis tools: '+', '.join(absent)+hint)
    if Path(found['ghidra']).name != Path(found['plugin']).parent.parent.name:
        raise ValueError('Ghidra and PSX extension profiles do not match; install the matching release from '+
                         PSX_EXTENSION_RELEASES)
    return found


def identity_files(tools):
    plugin = Path(tools['plugin'])
    plugin_files = sorted(path for path in plugin.rglob('*') if path.is_file() and
                          (path.is_relative_to(plugin/'data/psyq') or path.name in
                           ('Module.manifest', 'extension.properties', 'ghidra_psx_ldr.jar')))
    return {
        'ida': [Path(tools['ida'])],
        'ghidra': [Path(tools['ghidra'])/'support/analyzeHeadless.bat'],
        'java': [Path(tools['java'])],
        'plugin': plugin_files,
        'probes': [Path(__file__).resolve(), TOOLS/'ida/ida_preflight.py', TOOLS/'ghidra/RecognizePsyq.java'],
    }


def identities(tools):
    result = {}
    for name, paths in identity_files(tools).items():
        if not paths or any(not path.is_file() for path in paths):
            raise ValueError('Toolchain identity files are missing: '+name)
        records = [{'path': str(path.resolve()), 'bytes': path.stat().st_size, 'sha256': digest_file(path)} for path in paths]
        encoded = json.dumps(records, sort_keys=True, separators=(',', ':')).encode('utf-8')
        result[name] = {'files': len(records), 'bytes': sum(item['bytes'] for item in records),
                        'sha256': hashlib.sha256(encoded).hexdigest()}
    return result


def validate_receipt(path=None):
    path = Path(path or receipt_path())
    command = str(TOOLS/'prepare.bat')+' --verify-only'
    try:
        receipt = read_json(path)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        raise RuntimeError('XPORT_TOOLCHAIN_NOT_PREPARED: run "'+command+'"') from error
    try:
        valid = (receipt.get('schema') == SCHEMA and receipt.get('passed') is True and
                 receipt.get('capabilities') == CAPABILITIES and
                 receipt.get('identities') == identities(receipt['tools']))
    except (KeyError, OSError, ValueError):
        valid = False
    if not valid:
        raise RuntimeError('XPORT_TOOLCHAIN_NOT_PREPARED: stale receipt; run "'+command+'"')
    return {'receipt': str(path.resolve()), 'tools': receipt['tools'],
            'capabilities': receipt['capabilities'], 'prepared_utc': receipt['prepared_utc']}


def validate_runtime_access():
    """Reject a Windows sandbox that hides the licensed IDA registry profile"""
    if os.name != 'nt':
        return {'ida_registry': 'not_required'}
    import winreg
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r'Software\Hex-Rays', 0, winreg.KEY_READ):
            pass
    except OSError as error:
        raise RuntimeError('XPORT_IDA_HOST_ACCESS_REQUIRED: HKCU\\Software\\Hex-Rays is unavailable; '
                           'rerun the exact startup command with host access outside the workspace sandbox') from error
    return {'ida_registry': r'HKCU\Software\Hex-Rays'}


def run_probes(tools, runner=subprocess.run):
    with tempfile.TemporaryDirectory(prefix='xport-toolchain-') as directory:
        work = Path(directory)
        probe = work/'probe.bin'
        probe.write_bytes(b'PS-X EXE'+b'\0'*(4096-8))
        ida_receipt = work/'ida.json'
        ida_command = [tools['ida'], '-A', '-c', '-pmipsl', '-o'+str(work/'probe.idb'),
                       '-S"'+str(TOOLS/'ida/ida_preflight.py')+'" "'+str(ida_receipt)+'"', str(probe)]
        ida = runner(ida_command, cwd=work, capture_output=True, text=True, timeout=120)
        if ida.returncode or not ida_receipt.is_file() or read_json(ida_receipt) != {'processor': True, 'decompiler': True}:
            raise RuntimeError('IDA processor/decompiler preflight failed')
        report = work/'ghidra.json'
        launcher = str(Path(tools['ghidra'])/'support/analyzeHeadless.bat')
        ghidra_command = [launcher, str(work), 'xport-probe', '-import', str(probe), '-loader', 'BinaryLoader',
                          '-loader-baseAddr', '0x80010000', '-loader-fileOffset', '0',
                          '-processor', 'PSX:LE:32:default', '-cspec', 'default', '-noanalysis',
                          '-scriptPath', str(TOOLS/'ghidra'), '-postScript', 'RecognizePsyq.java',
                          str(report), str(Path(tools['plugin'])/'data/psyq'), '0', '-deleteProject']
        environment = os.environ.copy()
        environment['JAVA_HOME'] = str(Path(tools['java']).parent.parent)
        environment['GHIDRA_JAVA_HOME'] = environment['JAVA_HOME']
        ghidra = runner(ghidra_command, cwd=work, capture_output=True, text=True, timeout=300, env=environment)
        output = ghidra.stdout+ghidra.stderr
        if ghidra.returncode or 'PSYQ_EXPORT_OK' not in output or not report.is_file():
            raise RuntimeError('Ghidra PSX language/PsyQ preflight failed; verify the matching extension from '+
                               PSX_EXTENSION_RELEASES)
    return CAPABILITIES.copy()


def prepare(supplied=None, runner=subprocess.run):
    tools = discover_tools(supplied)
    path = receipt_path()
    try:
        existing = validate_receipt(path)
        if existing['tools'] == tools:
            return dict(existing, cached=True)
    except RuntimeError:
        pass
    capabilities = run_probes(tools, runner)
    receipt = {'schema': SCHEMA, 'passed': True, 'tools': tools, 'identities': identities(tools),
               'capabilities': capabilities,
               'prepared_utc': datetime.datetime.now(datetime.timezone.utc).isoformat()}
    atomic_json(path, receipt)
    return {'receipt': str(path.resolve()), 'tools': tools, 'capabilities': capabilities,
            'prepared_utc': receipt['prepared_utc'], 'cached': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ida')
    parser.add_argument('--ghidra')
    parser.add_argument('--java')
    parser.add_argument('--plugin')
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    supplied = {name: getattr(args, name) for name in ('ida', 'ghidra', 'java', 'plugin') if getattr(args, name)}
    result = validate_receipt() if args.check else prepare(supplied)
    print(json.dumps({'status': 'verified' if args.check else 'prepared', **result}, ensure_ascii=False))


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print(json.dumps({'status': 'failed', 'error': str(error)}, ensure_ascii=False), file=os.sys.stderr)
        raise SystemExit(1)

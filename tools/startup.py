"""Run the complete new-project startup protocol through one durable supervisor"""
import argparse
import collections
import configparser
import contextlib
import datetime
import hashlib
import html
import importlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import sqlite3
import subprocess
import sys
import time
import uuid
import startup_github
import startup_disc
from urllib.parse import quote

from toolchain_preflight import validate_receipt as validate_toolchain_receipt, validate_runtime_access
from xport_process import normalized_environment
from xport_project import CONFIG_NAME, allocation_lock, reservations


TOOLS = Path(__file__).resolve().parent
XPORT_ROOT = TOOLS.parent
SHORT_NAME_PATTERN = re.compile(r'[A-Za-z][A-Za-z0-9_]{0,15}')
LABEL_PATTERN = re.compile(r'[A-Za-z0-9_-]{1,64}')
IMAGE_PATTERN = re.compile(r'[A-Za-z0-9_.-]+')
WINDOWS_RESERVED_NAMES = {'CON', 'PRN', 'AUX', 'NUL', *(f'COM{i}' for i in range(1, 10)),
                          *(f'LPT{i}' for i in range(1, 10))}
TERMINAL = {'attention', 'complete', 'failed', 'cancelled'}
NATIVE_TEMPLATE_DIR = XPORT_ROOT/'src/platform/win'
PROJECT_GITIGNORE_TEMPLATE = TOOLS/'templates/project/.gitignore'
PROJECT_README_TEMPLATE = TOOLS/'templates/project/README.md.in'
NATIVE_TEMPLATE_TOKENS = ('@@PROJECT_GUID@@', '@@SHORT_NAME@@', '@@FULL_NAME@@', '@@SHARED_SOURCE@@')
NATIVE_TEMPLATE_REQUIREMENTS = {
    'xport.sln': {'@@PROJECT_GUID@@', '@@SHORT_NAME@@'},
    'xport.vcxproj': set(NATIVE_TEMPLATE_TOKENS),
}


def digest_bytes(value):
    return hashlib.sha256(value).hexdigest()


def digest_file(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024*1024), b''):
            value.update(block)
    return value.hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=False).encode('utf-8')


def relative(root, path):
    return Path(path).resolve().relative_to(Path(root).resolve()).as_posix()


def state_path(root):
    return Path(root)/'status/startup.json'


def log_path(root):
    return Path(root)/'status/startup.log'


def response_path(root):
    return Path(root)/'status/startup-response.json'


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name+'.tmp')
    temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False)+'\n', encoding='utf-8')
    temporary.replace(path)


def load_state(root):
    path = state_path(root)
    if not path.is_file():
        raise ValueError('STARTUP_NOT_STARTED: '+str(path))
    value = read_json(path)
    if value.get('schema') != 1 or value.get('project_root') != str(Path(root).resolve()):
        raise ValueError('Invalid startup state identity')
    return value


def publish(root, state, **changes):
    meaningful = any(state.get(key) != value for key, value in changes.items())
    if meaningful:
        state.update(changes)
        state['revision'] = state.get('revision', 0)+1
        state['updated_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        atomic_json(state_path(root), state)
    return state


def compact(state, changed=True):
    result = {key: state.get(key) for key in ('schema', 'revision', 'status', 'step', 'project_root', 'name', 'short_name')}
    result['changed'] = changed
    if state.get('attention'):
        result['attention'] = state['attention']
    if state.get('failure'):
        result['failure'] = state['failure']
    if state.get('completion'):
        result['completion'] = state['completion']
    if state.get('progress'):
        result['progress'] = state['progress']
    result['log'] = str(log_path(state['project_root']))
    result['response'] = str(response_path(state['project_root']))
    return result


def set_attention(root, state, kind, summary, required, context=None, proposed_decision=None):
    body = {'kind': kind, 'summary': summary, 'required': required, 'context': context or {}}
    body['sha256'] = digest_bytes(canonical(body))
    state = publish(root, state, status='attention', step=kind, attention=body, failure=None)
    atomic_json(response_path(root), {'revision': state['revision'], 'attention_sha256': body['sha256'],
                                     'decision': proposed_decision or {}})
    return state


def validate_names(name, short_name):
    if not name or any(c in name for c in '/\\:";\r\n'):
        raise ValueError('Invalid project name')
    if not SHORT_NAME_PATTERN.fullmatch(short_name or '') or short_name.upper() in WINDOWS_RESERVED_NAMES:
        raise ValueError('Short name must be a non-reserved Windows name starting with a letter and containing 1..16 ASCII letters, digits or underscores')


def render_native_template(template_name, replacements):
    path = NATIVE_TEMPLATE_DIR/template_name
    if not path.is_file():
        raise ValueError('Missing native project template: '+str(path))
    text = path.read_text(encoding='utf-8-sig')
    tokens = set(re.findall(r'@@[A-Z_]+@@', text))
    unknown = sorted(tokens-set(NATIVE_TEMPLATE_TOKENS))
    if unknown:
        raise ValueError('Unresolved native project template tokens: '+', '.join(unknown))
    missing = sorted(NATIVE_TEMPLATE_REQUIREMENTS.get(template_name, set())-tokens)
    if missing:
        raise ValueError('Missing native project template tokens: '+', '.join(missing))
    absent = sorted(tokens-set(replacements))
    if absent:
        raise ValueError('No replacement for native project template tokens: '+', '.join(absent))
    for token, value in replacements.items():
        text = text.replace(token, value)
    return text



def render_project_readme(name, short_name):
    if not PROJECT_README_TEMPLATE.is_file():
        raise ValueError('Missing project README template: '+str(PROJECT_README_TEMPLATE))
    text = PROJECT_README_TEMPLATE.read_text(encoding='utf-8-sig')
    project_name = ''.join(name.split())+'-xport'
    replacements = {
        '@@FULL_NAME@@': html.escape(name, quote=True),
        '@@PROJECT_NAME@@': html.escape(project_name, quote=True),
        '@@REPOSITORY@@': quote(project_name, safe=''),
        '@@SHORT_NAME@@': quote(short_name, safe=''),
    }
    tokens = set(re.findall(r'@@[A-Z_]+@@', text))
    if tokens != set(replacements):
        raise ValueError('Invalid project README template tokens: '+', '.join(sorted(tokens ^ set(replacements))))
    return re.sub(r'@@[A-Z_]+@@', lambda match: replacements[match.group()], text)


def agents_text(name, short_name):
    return f'''# {name}

Resolve `[XPORT_ROOT]` from `xport-project.json` and read `[XPORT_ROOT]/AGENTS.md`. Keep only stable game-specific facts and task-routing links here; put detailed evidence under `status`. Never create, edit, move, delete or regenerate `README.md`.

## Project facts

- Native short name: `{short_name}`; language: C
- Solution: `src/platform/win/{short_name}.sln`; Debug executable: `bin/{short_name}_debug.exe`; Release executable: `bin/{short_name}.exe`; working directory: `bin`; intermediates: `_build`
- Runtime data: `bin/DATA`; Red Book output when applicable: `bin/MUSIC`
- Before relying on them, record reviewed image identities, language decision, dummy scope, hooks/layouts, adapter contract and evidence links here
'''


def duckstation_launcher_text(root):
    command = os.path.relpath(TOOLS/'xport.py', Path(root).resolve()).replace('/', '\\')
    return f'''@echo off
setlocal
cd /d "%~dp0"
python -B "%~dp0{command}" --project "%~dp0." duckstation_run --role user --visible %*
if errorlevel 1 (
  echo DuckStation launch failed. See the error above.
  pause
  exit /b 1
)
endlocal
'''


def ensure_duckstation_launcher(root):
    root = Path(root).resolve()
    path = root/'DuckStation.bat'
    expected = duckstation_launcher_text(root)
    if path.exists():
        if not path.is_file() or path.read_text(encoding='utf-8-sig') != expected:
            raise ValueError('Existing DuckStation.bat differs from the startup launcher')
        reused = True
    else:
        with path.open('x', encoding='utf-8', newline='\r\n') as stream:
            stream.write(expected)
        reused = False
    return {'path': 'DuckStation.bat', 'sha256': digest_file(path), 'reused': reused}


def record_launcher_text(root):
    command = os.path.relpath(TOOLS/'xport.py', Path(root).resolve()).replace('/', '\\')
    return f'''@echo off
setlocal
cd /d "%~dp0"
python -B "%~dp0{command}" --project "%~dp0." record %*
exit /b %errorlevel%
'''


def ensure_record_launcher(root):
    root = Path(root).resolve()
    path = root/'record.bat'
    expected = record_launcher_text(root)
    if path.exists():
        if not path.is_file() or path.read_text(encoding='utf-8-sig') != expected:
            raise ValueError('Existing record.bat differs from the startup launcher')
        reused = True
    else:
        with path.open('x', encoding='utf-8', newline='\r\n') as stream:
            stream.write(expected)
        reused = False
    return {'path': 'record.bat', 'sha256': digest_file(path), 'reused': reused}


def initialize_project(root, name, short_name, first_port=2400):
    root = Path(root).resolve()
    if (root/CONFIG_NAME).exists():
        raise ValueError('STARTUP_ALREADY_INITIALIZED: '+str(root/CONFIG_NAME))
    validate_names(name, short_name)
    platform = root/'src/platform/win'
    targets = [root/CONFIG_NAME, root/'README.md', root/'.gitignore', root/'AGENTS.md', root/'DuckStation.bat', root/'record.bat', root/'src/game_main.c', platform/(short_name+'.sln'),
               platform/(short_name+'.vcxproj'), platform/'icon.ico', platform/'icon.rc', root/'status/translation-ledger.json']
    existing = [str(path) for path in targets if path.exists()]
    if existing:
        raise ValueError('STARTUP_ROOT_NOT_EMPTY: project initialization would overwrite '+', '.join(existing))
    project_guid = str(uuid.uuid4()).upper()
    shared_source = os.path.relpath(XPORT_ROOT/'src', platform).replace('/', '\\')
    full_name = name.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')
    native_replacements = {'@@PROJECT_GUID@@': project_guid, '@@SHORT_NAME@@': short_name,
                           '@@FULL_NAME@@': full_name, '@@SHARED_SOURCE@@': shared_source}
    for resource in ('icon.rc', 'icon.ico'):
        if not (NATIVE_TEMPLATE_DIR/resource).is_file():
            raise ValueError('Missing default Windows icon resource: '+str(NATIVE_TEMPLATE_DIR/resource))
    readme = render_project_readme(name, short_name)
    solution = render_native_template('xport.sln', native_replacements)
    native_project = render_native_template('xport.vcxproj', native_replacements)
    with allocation_lock(root.parent):
        used = reservations(root.parent)
        sockets, ports = [], {}
        try:
            candidate = first_port
            for role in ('user', 'lockstep', 'audit', 'trace'):
                while candidate <= 65535:
                    port = candidate
                    candidate += 1
                    if port in used:
                        continue
                    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                    listener.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
                    try:
                        listener.bind(('127.0.0.1', port))
                    except OSError:
                        listener.close()
                        continue
                    sockets.append(listener)
                    ports[role] = port
                    break
                else:
                    raise ValueError('No free GDB ports')
            for relative_path in ('src/platform/win', 'bin', '_build', 'iso', 'orig/images', 'orig/ghidra',
                                  'status/ida/runs', 'status/ida/accepted', 'status/ghidra/runs', 'tools/ida/databases'):
                (root/relative_path).mkdir(parents=True, exist_ok=True)
            config = {'schema': 1, 'name': name, 'short_name': short_name, 'toolset': str(TOOLS),
                      'github': {'repository': 'psxports/'+''.join(name.split())+'-xport'},
                      'paths': {'status': 'status', 'project_tools': 'tools', 'analysis_database': 'status/analysis.sqlite',
                                'native_executable': 'bin/'+short_name+'.exe',
                                'native_debug_executable': 'bin/'+short_name+'_debug.exe',
                                'native_working_directory': 'bin',
                                'native_solution': 'src/platform/win/'+short_name+'.sln', 'native_build': '_build',
                                'ida_exports': 'orig/images', 'ida_configs': 'tools/ida',
                                'sdk_classification': 'status/ghidra/classification.json'},
                      'duckstation': {'host': '127.0.0.1', 'gdb_port': ports['user'], 'reserved_gdb_ports': ports,
                                      'default_role': 'user', 'data_directory': 'tools/duckstation/data/user',
                                      'data_directories': {role: 'tools/duckstation/data/'+role for role in ports}},
                       'lockstep': {'schema': 1, 'enabled': False, 'original_image': '',
                           'timing': {'video_standard': 'NTSC', 'field_rate': 60},
                           'native_configuration': 'Debug', 'native_platform': 'x86',
                           'msbuild_property': 'LockstepDebug=true', 'source_roots': ['src'],
                           'max_events': 1000000,
                           'memory': {'dram_size': 2097152, 'scratchpad_size': 1024},
                           'devices': {'vram': 'trusted_wrapper', 'spu_ram': 'trusted_wrapper'},
                           'runtime': {'duckstation_role': 'lockstep', 'record_role': 'user',
                                       'native_arguments': ['--lockstep-replay']}},
                      'code_refresh': {'build': {'solution': 'src/platform/win/'+short_name+'.sln',
                                                 'configuration': 'Debug', 'platform': 'x86',
                                                 'process_name': short_name+'_debug.exe'}, 'tests': [], 'artifacts': []}}
            (platform/(short_name+'.sln')).write_text(
                solution, encoding='utf-8', newline='')
            (platform/(short_name+'.vcxproj')).write_text(
                native_project, encoding='utf-8', newline='')
            main_text = '''#include "xport.h"

/* Unresolved until the original PsyQ graph type storage is audited */
const uint32 xport_gpu_graph_type_address = 0u;

/* Unresolved CD bindings until original storage is audited */
const uint32 xport_cd_sync_callback_address = 0u;
const uint32 xport_cd_ready_callback_address = 0u;
const uint32 xport_cd_status_address = 0u;
const uint32 xport_cd_setloc_table_address = 0u;

void xport_main(void)
{
    return;
}
'''
            (root/'src/game_main.c').write_text(main_text, encoding='utf-8', newline='')
            with (root/'README.md').open('x', encoding='utf-8', newline='') as stream:
                stream.write(readme)
            for resource in ('icon.rc', 'icon.ico'):
                shutil.copyfile(NATIVE_TEMPLATE_DIR/resource, platform/resource)
            shutil.copyfile(PROJECT_GITIGNORE_TEMPLATE, root/'.gitignore')
            (root/'AGENTS.md').write_text(agents_text(name, short_name), encoding='utf-8', newline='')
            (root/'status/translation-ledger.json').write_text(json.dumps({'version': 1, 'entries': []}, indent=2)+'\n', encoding='utf-8')
            with (root/CONFIG_NAME).open('x', encoding='utf-8') as stream:
                json.dump(config, stream, indent=2)
                stream.write('\n')
            return config
        finally:
            for listener in sockets:
                listener.close()


def stage_project_gitignore(root):
    root = Path(root).resolve()
    if not (root/'.gitignore').is_file():
        raise ValueError('STARTUP_GIT_INDEX: missing generated .gitignore')

    def git(*arguments):
        try:
            result = subprocess.run(['git', '-C', str(root), *arguments],
                                    capture_output=True, text=True, encoding='utf-8', timeout=30)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise ValueError('STARTUP_GIT_INDEX: Git is unavailable or timed out') from error
        if result.returncode:
            raise ValueError('STARTUP_GIT_INDEX: '+result.stderr.strip())
        return result.stdout.strip()

    created = not (root/'.git').exists()
    if created:
        git('init', '--quiet')
    if git('rev-parse', '--show-prefix'):
        raise ValueError('STARTUP_GIT_INDEX: project must be its own repository root')
    git('add', '--', '.gitignore')
    git('ls-files', '--error-unmatch', '--', '.gitignore')
    return {'repository_created': created, 'indexed_files': ['.gitignore']}


def project(root):
    return read_json(Path(root)/CONFIG_NAME)


def project_path(root, config, key, default=None):
    value = config.get('paths', {}).get(key, default)
    if value is None:
        raise ValueError('Missing project path: '+key)
    path = Path(value)
    return path.resolve() if path.is_absolute() else (Path(root)/path).resolve()


def artifact_path(root, relative_path):
    root = Path(root).resolve()
    path = Path(relative_path)
    path = path.resolve() if path.is_absolute() else (root/path).resolve()
    if not any(path.is_relative_to(root/base) for base in ('status', 'tools')):
        raise ValueError('Generated artifacts must remain in project status or tools')
    return path


def append_log(root, title, payload):
    path = log_path(root)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('a', encoding='utf-8') as stream:
        stream.write('\n['+datetime.datetime.now(datetime.timezone.utc).isoformat()+'] '+title+'\n')
        stream.write(payload)
        if not payload.endswith('\n'):
            stream.write('\n')


def run_shared(root, tool, arguments=()):
    command = [sys.executable, '-B', str(TOOLS/'xport.py'), '--project', str(Path(root).resolve()), tool, *arguments]
    result = subprocess.run(command, cwd=root, capture_output=True, text=True, env=normalized_environment())
    append_log(root, tool, '$ '+' '.join(command)+'\n'+result.stdout+result.stderr)
    if result.returncode:
        raise RuntimeError(tool+' failed; inspect '+str(log_path(root)))
    return result.stdout.strip()


def inventory(root):
    root = Path(root)
    candidates = []
    for base in (root/'iso', root/'bin/DATA', root/'orig'):
        if not base.exists():
            continue
        for path in sorted(item for item in base.rglob('*') if item.is_file()):
            if base.name == 'orig' and path.relative_to(base).parts[0] in ('images', 'ghidra'):
                continue
            item = {'path': relative(root, path), 'bytes': path.stat().st_size, 'sha256': digest_file(path)}
            with path.open('rb') as stream:
                header = stream.read(0x800)
            if header.startswith(b'PS-X EXE') and len(header) >= 0x20:
                item['psx_exe'] = {'entry': int.from_bytes(header[0x10:0x14], 'little'),
                                   'gp': int.from_bytes(header[0x14:0x18], 'little'),
                                   'load_address': int.from_bytes(header[0x18:0x1c], 'little'),
                                   'payload_size': int.from_bytes(header[0x1c:0x20], 'little'),
                                   'file_offset': 0x800}
            candidates.append(item)
    return {'file_count': len(candidates), 'total_bytes': sum(item['bytes'] for item in candidates),
            'files': candidates[:64], 'files_truncated': len(candidates) > 64,
            'system_configs': [item for item in candidates if Path(item['path']).name.upper() == 'SYSTEM.CNF'],
            'cue_files': [item['path'] for item in candidates if item['path'].lower().endswith('.cue')],
            'psx_executables': [item for item in candidates if item.get('psx_exe')]}


def runtime_gp_initializers(payload, load_address):
    """Find adjacent MIPS32 lui/addiu assignments to gp"""
    candidates = []
    for offset in range(0, len(payload)-7, 4):
        first = int.from_bytes(payload[offset:offset+4], 'little')
        second = int.from_bytes(payload[offset+4:offset+8], 'little')
        if (first >> 26) != 0x0f or ((first >> 16) & 31) != 28:
            continue
        if (second >> 26) != 0x09 or ((second >> 21) & 31) != 28 or ((second >> 16) & 31) != 28:
            continue
        low = second & 0xffff
        low = low-0x10000 if low & 0x8000 else low
        candidates.append({'site': load_address+offset,
                           'value': (((first & 0xffff) << 16)+low) & 0xffffffff})
    return candidates


def reviewed_gp_profile(root, item):
    facts = item['psx_exe']
    raw = (Path(root)/item['path']).read_bytes()
    begin = facts['file_offset']
    payload = raw[begin:begin+facts['payload_size']]
    candidates = runtime_gp_initializers(payload, facts['load_address'])
    values = sorted({candidate['value'] for candidate in candidates})
    header = facts['gp']
    if header and (not values or values == [header]):
        value = header
        source = 'psx_exe_header'
    elif not header and len(values) == 1:
        value = values[0]
        source = 'runtime_initializer'
    else:
        value = header
        source = 'review_required'
    return value, {'source': source, 'header_value': header, 'runtime_initializers': candidates}


def validate_gp_config(root, profile):
    loads = profile.get('loads', [])
    if len(loads) != 1:
        raise ValueError('IDA GP validation requires exactly one reviewed image load')
    load = loads[0]
    path = Path(load['path'])
    path = path if path.is_absolute() else Path(root)/path
    raw = path.read_bytes()
    begin = load.get('offset', 0)
    candidates = runtime_gp_initializers(raw[begin:], load['base'])
    values = sorted({candidate['value'] for candidate in candidates})
    gp = profile.get('gp')
    if type(gp) is not int or not 0 < gp <= 0xffffffff:
        raise ValueError('IDA config requires a reviewed nonzero runtime GP')
    evidence = profile.get('gp_evidence')
    if isinstance(evidence, dict) and evidence.get('source') in ('inherited_runtime_initializer', 'accepted_overlay_profile', 'inherited_static_loader'):
        from overlay_gp import validate as validate_overlay_gp
        return validate_overlay_gp(root, profile, evidence)
    if isinstance(evidence, dict) and evidence.get('source') == 'bios_exec_pslz':
        from psx_loader_gp import validate as validate_loader_gp
        return validate_loader_gp(root, profile, evidence)
    if len(values) != 1 or values[0] != gp:
        raise ValueError('IDA config GP differs from the unique runtime initializer')
    evidence = profile.get('gp_evidence')
    if not isinstance(evidence, dict) or evidence.get('source') not in ('runtime_initializer', 'psx_exe_header'):
        raise ValueError('IDA config requires machine-readable GP evidence')
    if evidence.get('runtime_initializers') != candidates:
        raise ValueError('IDA config GP evidence differs from the executable')
    return {'gp': gp, 'initializer': candidates[0]}


def image_review_proposal(root, found):
    root = Path(root)
    executables = found.get('psx_executables', [])
    names = [Path(item['path']).name for item in executables]
    if (not executables or len(names) != len(set(names)) or any(not IMAGE_PATTERN.fullmatch(name) for name in names)
            or any(item['psx_exe']['file_offset']+item['psx_exe']['payload_size'] > item.get('bytes', 0) for item in executables)):
        return None
    boot = None
    system_item = next((item for item in found.get('system_configs', found.get('files', [])) if Path(item['path']).name.upper() == 'SYSTEM.CNF'), None)
    system = root/system_item['path'] if system_item else root/'orig/SYSTEM.CNF'
    if system.is_file():
        match = re.search(r'(?im)^\s*BOOT\s*=\s*cdrom:\\+([^;\r\n]+)', system.read_text(errors='replace'))
        if match:
            requested = Path(match.group(1).replace('\\', '/')).name
            boot = next((item for item in executables if Path(item['path']).name.lower() == requested.lower()), None)
    default = max((item for item in executables if item is not boot), key=lambda item: item['psx_exe']['payload_size'], default=None)
    default = default or boot or max(executables, key=lambda item: item['psx_exe']['payload_size'])
    ordered = ([boot] if boot else [])+[item for item in executables if item is not boot]
    paths = {}
    if len(found.get('cue_files', [])) == 1:
        paths['disc_image'] = found['cue_files'][0]
        paths['music_source'] = str(Path(found['cue_files'][0]).parent).replace('\\', '/') or '.'
        paths['music_output'] = 'bin/MUSIC'
    if boot:
        paths['original_executable'] = boot['path']
    capabilities = {}
    if len(found.get('cue_files', [])) == 1:
        cue = root/found['cue_files'][0]
        audio = bool(re.search(r'(?im)^\s*TRACK\s+\d+\s+AUDIO\s*$', cue.read_text(errors='replace')))
        capabilities['redbook_audio'] = {'enabled': audio}
        if not audio:
            capabilities['redbook_audio']['reason'] = 'Reviewed CUE contains no AUDIO tracks'
    profiles = {}
    for item in ordered:
        facts = item['psx_exe'];name = Path(item['path']).name
        start = facts['load_address'];end = start+facts['payload_size']
        gp, gp_evidence = reviewed_gp_profile(root, item)
        profiles[name] = {'image': name, 'entry': facts['entry'], 'gp': gp, 'gp_evidence': gp_evidence,
            'segments': [{'start': start, 'end': end, 'name': name, 'class': 'CODE'}],
            'loads': [{'path': item['path'], 'base': start, 'offset': facts['file_offset']}],
            'seeds': [facts['entry']], 'code_ranges': [[start, end]]}
    patch = {'paths': paths, 'analysis': {'images': [Path(item['path']).name for item in ordered],
             'default_image': Path(default['path']).name},
             'lockstep': {'original_image': Path(default['path']).name}}
    if capabilities:
        patch['capabilities'] = capabilities
    return {'config_patch': patch, 'ida_configs': profiles}


def project_preflight(root):
    """Validate project-specific settings without launching host analysis tools"""
    root = Path(root).resolve()
    config = project(root)
    solution = (root/config['paths']['native_solution']).read_text(encoding='utf-8-sig')
    platform = config.get('code_refresh', {}).get('build', {}).get('platform', 'x86')
    if '|'+platform not in solution:
        raise ValueError('code_refresh platform is absent from the native solution: '+platform)
    return {'platform': platform}


def validate_capture_contract(root):
    root = Path(root).resolve()
    config = project(root)
    lockstep = config.get('lockstep') or {}
    if lockstep.get('schema') != 1 or lockstep.get('enabled') is not True:
        raise ValueError('Enable the lockstep record and converge contract')
    image = lockstep.get('original_image')
    if not isinstance(image, str) or not IMAGE_PATTERN.fullmatch(image):
        raise ValueError('Configure lockstep.original_image')
    runtime = lockstep.get('runtime', {})
    role = runtime.get('duckstation_role')
    record_role = runtime.get('record_role', 'user')
    directories = config.get('duckstation', {}).get('data_directories', {})
    ports = config.get('duckstation', {}).get('reserved_gdb_ports', {})
    if any(value not in directories or value not in ports for value in (role, record_role)):
        raise ValueError('Reserve the user and lockstep DuckStation runtimes')
    disc = root/config.get('paths', {}).get('disc_image', '')
    profile_path = project_path(root, config, 'ida_configs', 'tools/ida')/(image+'.json')
    if profile_path.is_file():
        profile = read_json(profile_path)
        if profile.get('image') != image or len(profile.get('loads', [])) != 1:
            raise ValueError('Recording image requires its exact single-load IDA profile')
        original = Path(profile['loads'][0]['path'])
        original = original if original.is_absolute() else root/original
    else:
        original = root/'orig'/image
    if not disc.is_file() or not original.is_file():
        raise ValueError('Lockstep recording requires the disc image and original executable')
    return {'schema': 1, 'original_image': image, 'duckstation_role': role,
            'record_role': record_role,
            'disc_image': relative(root, disc), 'original_executable': relative(root, original)}


def capture_contract_packet(root):
    try:
        current = validate_capture_contract(root)
        problem = None
    except (KeyError, OSError, ValueError, sqlite3.Error) as error:
        current = None
        problem = str(error)
    return {'current': current, 'problem': problem,
            'required_config': ['lockstep.schema=1', 'lockstep.enabled=true',
                                'lockstep.original_image', 'lockstep.runtime.duckstation_role'],
            'scope': 'Configure input-only recording from one exact save state'}


def validate_convergence_contract(root):
    """Require a reviewed project-native continuation before startup can complete"""
    root = Path(root).resolve()
    config = project(root)
    capture = validate_capture_contract(root)
    progress_path = root/'status/progress-data.json'
    if not progress_path.is_file():
        raise ValueError('Generate current translation progress before convergence review')
    decompilation = read_json(progress_path).get('decompilation_status', {})
    lockstep = config['lockstep']
    if lockstep.get('native_configuration') != 'Debug' or lockstep.get('native_platform') != 'x86':
        raise ValueError('Lockstep convergence requires Debug x86')
    if lockstep.get('msbuild_property') != 'LockstepDebug=true':
        raise ValueError('Configure the LOCKSTEP_DEBUG build property')
    memory = lockstep.get('memory', {})
    if memory.get('dram_size') != 2097152 or memory.get('scratchpad_size') != 1024:
        raise ValueError('Configure exact PS1 DRAM and scratchpad sizes')
    native_scope = frontier_packet(root)
    return {'contract': 'xport-lockstep-v2', 'capture': capture,
            'decompilation_status': decompilation, 'native_scope': native_scope}


def convergence_contract_packet(root):
    try:
        current = validate_convergence_contract(root)
        problem = None
    except (KeyError, OSError, ValueError, sqlite3.Error) as error:
        current = None
        problem = str(error)
    return {'current': current, 'problem': problem,
            'required_config': ['lockstep.native_configuration=Debug', 'lockstep.native_platform=x86',
                                'lockstep.msbuild_property=LockstepDebug=true', 'lockstep.memory'],
            'required_files': ['src/game_except.c', 'LOCKSTEP_DEBUG native build'],
            'scope': 'Configure shared-memory lockstep comparison and semantic call boundaries'}


def prepare_ida_export(root, ida, label, images=None, export_existing=False, plan=False):
    if not LABEL_PATTERN.fullmatch(label):
        raise ValueError('Invalid label')
    root = Path(root).resolve()
    config = project(root)
    configs = project_path(root, config, 'ida_configs', 'tools/ida')
    images = images or config.get('analysis', {}).get('images')
    if not images:
        raise ValueError('Specify images explicitly')
    run = artifact_path(root, 'status/ida/runs/'+label)
    run.mkdir(parents=True, exist_ok=False)
    commands = []
    for image in images:
        if Path(image).name != image or not IMAGE_PATTERN.fullmatch(image):
            raise ValueError('Invalid image name')
        task = read_json(configs/(image+'.json'))
        validate_gp_config(root, task)
        task['output'] = str(run/image)
        task['rebuild'] = not export_existing
        for load in task['loads']:
            load['path'] = str((root/load['path']).resolve())
        if task.get('external_symbols'):
            task['external_symbols'] = str((root/task['external_symbols']).resolve())
        task_path = run/(image+'.json')
        task_path.write_text(json.dumps(task, indent=2)+'\n', encoding='utf-8')
        database = project_path(root, config, 'ida_databases', 'tools/ida/databases')/(image+'.idb')
        script = TOOLS/'ida/ida_analyze_export.py'
        command = [str(ida), '-A', '-L'+str(run/(image+'.log')), '-S"'+str(script)+'" "'+str(task_path)+'"']
        if database.exists():
            if not plan:
                shutil.copy2(database, run/(image+'.idb'))
            command.append(str(run/(image+'.idb')))
        else:
            if export_existing:
                raise ValueError('Existing IDA database missing: '+str(database))
            command.extend(['-c', '-pmipsl', '-o'+str(run/(image+'.idb')), task['loads'][-1]['path']])
        commands.append(command)
    (run/'commands.json').write_text(json.dumps(commands, indent=2)+'\n', encoding='utf-8')
    if not plan:
        for command in commands:
            result = subprocess.run(command, cwd=root, capture_output=True, text=True)
            output = (result.stdout or '')+(result.stderr or '')
            if output:
                append_log(root, 'IDA export process', output)
            if result.returncode:
                if 'Fatal registry error' in output or 'Р С›РЎвЂљР С”Р В°Р В·Р В°Р Р…Р С• Р Р† Р Т‘Р С•РЎРѓРЎвЂљРЎС“Р С—Р Вµ' in output:
                    raise RuntimeError('XPORT_IDA_HOST_ACCESS_REQUIRED: IDA lost registry access; '
                                       'retry the startup revision with host access outside the workspace sandbox')
                raise subprocess.CalledProcessError(result.returncode, command, output=result.stdout,
                                                    stderr=result.stderr)
    return {'run': str(run), 'planned': plan, 'images': images}


def canonicalize_ida(root, exports, configs):
    import capstone
    root = Path(root).resolve()
    exports = artifact_path(root, exports)
    configs = Path(configs)
    configs = configs if configs.is_absolute() else root/configs
    decoder = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS32|capstone.CS_MODE_LITTLE_ENDIAN)
    results = []
    for out in sorted(path for path in exports.iterdir() if path.is_dir() and (path/'functions.json').is_file()):
        cfg = read_json(configs/(out.name+'.json'))
        source = cfg['loads'][-1]
        base = source['base']
        blob = (root/source['path']).read_bytes()[source.get('offset', 0):]
        records = read_json(out/'functions.json')
        full, all_c, raw_count, undecoded = [], [], 0, []
        expected = {'%08X'%function['address'] for function in records}
        expected_pseudo = {'%08X'%function['address'] for function in records if function['pseudocode']}
        for folder in ('functions', 'pseudocode'):
            for path in (out/folder).iterdir():
                allowed = expected_pseudo if folder == 'pseudocode' else expected
                if re.fullmatch(r'[0-9A-F]{8}\.(lst|mips\.txt|c)', path.name) and path.name[:8] not in allowed:
                    path.unlink()
        for record in records:
            words, delay_next = [], None
            report = read_json(out/'export-report.json')
            lines = ['; GENERATED IDA %s; image=%s entry=%08X end=%08X chunks=%s' %
                     (report['ida'], out.name, record['address'], record['end'], record['chunks'])]
            for item in record['instructions']:
                raw_item = bytes.fromhex(item['bytes'])
                lines.append('%08X  %-23s %s' % (item['address'], raw_item.hex(' '), item['text']))
                for address in range(item['address'], item['address']+len(raw_item), 4):
                    raw = blob[address-base:address-base+4]
                    expected_raw = raw_item[address-item['address']:address-item['address']+4]
                    if raw != expected_raw:
                        raise ValueError('IDA bytes differ from original image: '+out.name+':'+format(address, '08X')+
                                         ' image='+raw.hex()+' export='+expected_raw.hex())
                    word = int.from_bytes(raw, 'little')
                    decoded = list(decoder.disasm(raw, address))
                    text = (decoded[0].mnemonic+' '+decoded[0].op_str).strip() if decoded else '.word 0x%08X'%word
                    if not decoded:
                        undecoded.append(address)
                    op, fn = word>>26, word&63
                    branch = op in (1, 2, 3, 4, 5, 6, 7) or op == 0 and fn in (8, 9) or op in (16, 17, 18) and (word>>21&31) == 8
                    words.append({'address': address, 'bytes': raw.hex(), 'word': '%08X'%word, 'text': text,
                                  'delay_slot': address == delay_next})
                    delay_next = address+4 if branch else None
            if digest_bytes(b''.join(bytes.fromhex(item['bytes']) for item in words)) != record['sha256']:
                raise ValueError('Canonical function hash mismatch')
            record['raw_instructions'] = words
            raw_count += len(words)
            listing = '\n'.join(lines)+'\n'
            (out/'functions'/('%08X.lst'%record['address'])).write_text(listing)
            full.append(listing)
            raw_text = '; GENERATED independent Capstone %s; IDA listing is adjacent .lst\n; image=%s entry=%08X SHA256=%s\n' % (capstone.__version__, out.name, record['address'], record['sha256'])
            raw_text += '\n'.join('%08X %s %-10s %s%s' % (item['address'], item['bytes'], item['word'], item['text'], ' ; DELAY SLOT' if item['delay_slot'] else '') for item in words)+'\n'
            (out/'functions'/('%08X.mips.txt'%record['address'])).write_text(raw_text)
            if record['pseudocode']:
                all_c.append((out/record['pseudocode']).read_text())
        (out/'functions.json').write_text(json.dumps(records, indent=2))
        (out/(out.name+'.lst')).write_text('\n'.join(full))
        (out/(out.name+'.c')).write_text('\n'.join(all_c))
        report = read_json(out/'export-report.json')
        report.update(raw_instruction_words=raw_count, source_bytes_verified=True,
                      independent_decoder='capstone '+capstone.__version__, independent_undecoded_words=undecoded)
        (out/'export-report.json').write_text(json.dumps(report, indent=2))
        results.append({'image': out.name, 'functions': len(records), 'words': raw_count, 'undecoded': len(undecoded)})
    return results


def verify_ida_image(root, run, configs, image):
    source = run/image
    config = configs/(image+'.json')
    required = (source/'functions.json', source/'export-report.json', source/'functions', source/'pseudocode',
                source/(image+'.lst'), source/(image+'.c'), config)
    missing = [relative(root, path) for path in required if not path.exists()]
    if missing:
        raise ValueError('Incomplete IDA export for '+image+': '+', '.join(missing))
    report = read_json(source/'export-report.json')
    if report.get('image') != image or report.get('source_bytes_verified') is not True or not report.get('independent_decoder'):
        raise ValueError('IDA export is not canonicalized and byte-verified: '+image)
    functions = read_json(source/'functions.json')
    if not functions:
        raise ValueError('IDA export contains no functions: '+image)
    instruction_words = 0
    for function in functions:
        address = format(function['address'], '08X')
        instructions = function.get('raw_instructions')
        if not instructions:
            raise ValueError('Function lacks canonical raw instructions: '+image+':'+address)
        raw = b''.join(bytes.fromhex(item['bytes']) for item in instructions)
        if digest_bytes(raw) != function.get('sha256'):
            raise ValueError('Function byte hash mismatch: '+image+':'+address)
        instruction_words += len(instructions)
        paths = [source/'functions'/(address+'.lst'), source/'functions'/(address+'.mips.txt')]
        if function.get('pseudocode'):
            paths.append(source/function['pseudocode'])
        if any(not path.is_file() for path in paths):
            raise ValueError('Canonical function artifacts are missing: '+image+':'+address)
    if report.get('raw_instruction_words') != instruction_words:
        raise ValueError('Canonical instruction count does not match export report: '+image)
    files = [{'path': path.relative_to(source).as_posix(), 'bytes': path.stat().st_size, 'sha256': digest_file(path)}
             for path in sorted(item for item in source.rglob('*') if item.is_file())]
    return {'image': image, 'functions': len(functions), 'source': relative(root, source),
            'config': relative(root, config), 'config_sha256': digest_file(config), 'files': files}


def accept_ida(root, label, selected=None):
    if not LABEL_PATTERN.fullmatch(label):
        raise ValueError('Invalid IDA run label')
    root = Path(root).resolve()
    config = project(root)
    run = root/'status/ida/runs'/label
    if not run.is_dir():
        raise ValueError('IDA run does not exist: '+str(run))
    candidates = sorted(path.name for path in run.iterdir() if path.is_dir() and (path/'functions.json').is_file())
    images = selected or config.get('analysis', {}).get('images') or candidates
    if not images or len(images) != len(set(images)):
        raise ValueError('Select unique image names')
    unknown = sorted(set(images)-set(candidates))
    if unknown:
        raise ValueError('Images are absent from the IDA run: '+', '.join(unknown))
    destination = project_path(root, config, 'ida_exports', 'orig/images')
    if not destination.is_relative_to(root):
        raise ValueError('Accepted IDA exports must remain inside the project')
    configs = project_path(root, config, 'ida_configs', 'tools/ida')
    manifests = [verify_ida_image(root, run, configs, image) for image in images]
    collisions = [relative(root, destination/image) for image in images if (destination/image).exists()]
    if collisions:
        raise ValueError('Accepted immutable image already exists: '+', '.join(collisions))
    destination.mkdir(parents=True, exist_ok=True)
    staging = destination/('.accept-'+label+'-'+uuid.uuid4().hex)
    published = []
    try:
        staging.mkdir()
        for image in images:
            shutil.copytree(run/image, staging/image)
        for manifest in manifests:
            for item in manifest['files']:
                path = staging/manifest['image']/item['path']
                if path.stat().st_size != item['bytes'] or digest_file(path) != item['sha256']:
                    raise ValueError('Copied IDA artifact failed hash verification: '+str(path))
        for image in images:
            (staging/image).replace(destination/image)
            published.append(destination/image)
        staging.rmdir()
    except Exception:
        if staging.exists():
            shutil.rmtree(staging)
        for path in published:
            if path.exists():
                shutil.rmtree(path)
        raise
    receipt = {'version': 1, 'label': label, 'accepted_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
               'destination': relative(root, destination), 'images': manifests}
    receipt_path = root/'status/ida/accepted'/(label+'.json')
    receipt_path.parent.mkdir(parents=True, exist_ok=True)
    receipt_path.write_text(json.dumps(receipt, indent=2)+'\n', encoding='utf-8')
    return {'accepted': images, 'destination': relative(root, destination), 'receipt': relative(root, receipt_path)}


def run_ghidra(root, ghidra, plugin, label, images=None, plan=False):
    if not LABEL_PATTERN.fullmatch(label):
        raise ValueError('Invalid label')
    root = Path(root).resolve()
    config = project(root)
    images = images or config.get('analysis', {}).get('images')
    if not images:
        raise ValueError('Specify images explicitly')
    run = artifact_path(root, 'status/ghidra/runs/'+label)
    run.mkdir(parents=True, exist_ok=False)
    commands = []
    for image in images:
        if not IMAGE_PATTERN.fullmatch(image):
            raise ValueError('Invalid image')
        cfg = read_json(project_path(root, config, 'ida_configs', 'tools/ida')/(image+'.json'))
        load = cfg['loads'][-1]
        command = [str(Path(ghidra)/'support/analyzeHeadless.bat'), str(run), image,
                   '-import', str((root/load['path']).resolve()), '-loader', 'BinaryLoader',
                   '-loader-baseAddr', hex(load['base']), '-loader-fileOffset', hex(load.get('offset', 0)),
                   '-processor', 'PSX:LE:32:default', '-cspec', 'default', '-noanalysis',
                   '-scriptPath', str(TOOLS/'ghidra'), '-postScript', 'RecognizePsyq.java',
                   str(run/(image+'.json')), str(Path(plugin)/'data/psyq'), format(cfg['gp'], 'X'), image]
        commands.append(command)
        if not plan:
            with (run/(image+'.log')).open('wb') as stream:
                subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, check=True, cwd=root)
            if 'PSYQ_EXPORT_OK' not in (run/(image+'.log')).read_text(errors='replace'):
                raise RuntimeError('Signature export did not finish: '+image)
    (run/'commands.json').write_text(json.dumps(commands, indent=2)+'\n', encoding='utf-8')
    return {'run': str(run), 'planned': plan, 'images': images}


def classify_psyq(root, reports, output=None):
    root = Path(root).resolve()
    config = project(root)
    reports = Path(reports)
    reports = reports if reports.is_absolute() else root/reports
    output = artifact_path(root, output or 'status/ghidra/classification.json')
    exports = project_path(root, config, 'ida_exports', 'orig/images')
    wrappers = [(root/path).resolve() for path in config.get('analysis', {}).get('wrapper_references', [])]
    texts = {str(path): path.read_text(encoding='utf-8-sig') for path in wrappers}
    images = config['analysis']['images']
    if not all((reports/(image+'.json')).is_file() for image in images):
        raise ValueError('Run recognition for every image first')
    accepted, pending = [], []
    for file in sorted(reports.glob('*.json')):
        if file.stem not in images:
            continue
        report = read_json(file)
        image = file.stem
        cfg = read_json(project_path(root, config, 'ida_configs', 'tools/ida')/(image+'.json'))
        load = cfg['loads'][-1]
        if report['image'] not in (image, Path(load['path']).name):
            raise ValueError('Ghidra report image identity mismatch: '+image)
        raw = (root/load['path']).read_bytes()
        payload, base = raw[load.get('offset', 0):], load['base']
        if base != report['base'] or cfg['gp'] != report['gp']:
            raise ValueError('Ghidra report identity mismatch: '+image)
        functions = read_json(exports/image/'functions.json')
        by_address = collections.defaultdict(list)
        for match in report['matches']:
            pattern, mask = bytes.fromhex(match['pattern']), bytes.fromhex(match['mask'])
            offset = match['address']-base
            actual = payload[offset:offset+len(pattern)]
            if len(pattern) != len(mask) or offset < 0 or len(actual) != len(pattern) or not all((a&k) == (b&k) for a, b, k in zip(actual, pattern, mask)):
                raise ValueError('Ghidra masked bytes mismatch: '+image)
            for label in match['labels']:
                if label['name'].startswith('loc_'):
                    continue
                by_address[match['address']+label['offset']].append(
                    {'name': label['name'], 'version': match['version'], 'library': match['library'],
                     'object': match['object'], 'object_start': match['address'],
                     'object_end': match['address']+len(pattern), 'object_sha256': digest_bytes(actual),
                     'entropy': match['entropy'], 'bios': match['bios']})
        for function in functions:
            hits = [item for item in by_address[function['address']]
                    if all(item['object_start'] <= start < end <= item['object_end'] for start, end in function['chunks'])]
            if not hits:
                continue
            names = sorted({item['name'] for item in hits if not item['name'].startswith('text_')})
            listing = relative(root, exports/image/'functions'/('%08X.lst'%function['address']))
            pseudocode = relative(root, exports/image/function['pseudocode']) if function.get('pseudocode') else None
            entry = {'image': image, 'address': function['address'], 'original_name': function['name'],
                     'sha256': function['sha256'], 'listing': listing, 'pseudocode': pseudocode,
                     'matches': hits, 'names': names}
            longest = max(item['object_end']-item['object_start'] for item in hits)
            calls = []
            for instruction in function['raw_instructions']:
                word = int.from_bytes(bytes.fromhex(instruction['bytes']), 'little')
                if word>>26 == 3:
                    calls.append(((instruction['address']+4)&0xf0000000)|((word&0x3ffffff)<<2))
            if not any(item['bios'] for item in hits) and (longest < 32 or longest < 64 and any(not by_address[target] for target in calls)):
                entry['reason'] = 'Short generic signature without independently recognized call target; insufficient SDK identity evidence'
                pending.append(entry)
                continue
            if len(names) > 1:
                entry['reason'] = 'Conflicting API labels; needs review'
                pending.append(entry)
                continue
            name = names[0] if names else hits[0]['object'].replace('.', '_')+'_'+format(function['address']-hits[0]['object_start'], 'X')
            refs = []
            for path, text in texts.items():
                match = re.search(r'\b'+re.escape(name)+r'\s*\(', text)
                if match:
                    refs.append({'path': path, 'sha256': digest_file(path), 'line': text[:match.start()].count('\n')+1})
            entry.update(alias=name, status='SKIP',
                         replacement='Planned xport PsyQ/host boundary; Configured wrapper files are reference implementations only; host CRT for libc',
                         wrapper_references=refs,
                         wrapper_status='symbol present; project ABI/runtime integration remains untested' if refs else 'SDK internal or API adapter still to map at replacement boundary',
                         reason='Whole IDA function lies inside a Ghidra PSX-plugin PsyQ masked object match; SDK implementation replaced at host wrapper boundary')
            accepted.append(entry)
    result = {'policy': 'Original image/address names retained. SKIP is replacement scope, not DONE or runtime equivalence. Ambiguous API names remain TODO.',
              'wrapper_files': [{'path': path, 'sha256': digest_file(path)} for path in texts],
              'accepted': accepted, 'pending': pending}
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    return {'output': relative(root, output), 'accepted': len(accepted), 'pending': len(pending)}


def validate_similarity(root):
    root = Path(root).resolve()
    config = project(root)
    os.environ['XPORT_PROJECT'] = str(root)
    import validate_similarity as validator
    validator = importlib.reload(validator)
    database = project_path(root, config, 'analysis_database', 'status/analysis.sqlite')
    with sqlite3.connect(database.as_uri()+'?mode=ro', uri=True) as db:
        return validator.validate(db)


def apply_response(root, state, response):
    attention = state.get('attention')
    if state.get('status') != 'attention' or not attention:
        raise ValueError('STARTUP_NOT_WAITING_FOR_RESPONSE')
    if response.get('revision') != state['revision'] or response.get('attention_sha256') != attention['sha256']:
        raise ValueError('STARTUP_STALE_RESPONSE')
    decision = response.get('decision')
    if not isinstance(decision, dict):
        raise ValueError('STARTUP_RESPONSE_REQUIRES_DECISION')
    kind = attention['kind']
    if kind in ('image_review', 'tool_paths'):
        config_path = Path(root)/CONFIG_NAME
        config = read_json(config_path)
        patch = decision.get('config_patch', {})
        if any(key not in ('paths', 'analysis', 'capabilities', 'lockstep') for key in patch):
            raise ValueError('STARTUP_RESPONSE_CONFIG_SCOPE')
        for key, value in patch.items():
            if not isinstance(value, dict):
                raise ValueError('STARTUP_RESPONSE_CONFIG_OBJECT_REQUIRED')
            config.setdefault(key, {}).update(value)
        for name, value in decision.get('ida_configs', {}).items():
            if not IMAGE_PATTERN.fullmatch(name) or not isinstance(value, dict):
                raise ValueError('STARTUP_RESPONSE_INVALID_IDA_CONFIG')
            path = Path(root)/'tools/ida'/(name+'.json')
            path.parent.mkdir(parents=True, exist_ok=True)
            validate_gp_config(root, value)
            atomic_json(path, value)
        atomic_json(config_path, config)
        next_step = 'inventory'
    elif kind == 'psyq_review':
        if decision.get('approved') is not True:
            raise ValueError('STARTUP_PSYQ_REVIEW_NOT_APPROVED')
        next_step = 'database'
    elif kind == 'frontier_translation':
        if decision.get('approved') is not True or not decision.get('changed_files'):
            raise ValueError('STARTUP_FRONTIER_REQUIRES_REVIEWED_CHANGES')
        expected = attention.get('context', {}).get('functions', [])
        audits = decision.get('function_audits')
        if not isinstance(audits, list) or len(audits) != len(expected):
            raise ValueError('STARTUP_FRONTIER_REQUIRES_COMPLETE_FUNCTION_AUDITS')
        keyed = {(row.get('image'), row.get('address')): row for row in audits
                 if isinstance(row, dict)}
        for function in expected:
            context = function.get('audit_context') or {}
            mips = context.get('mips') or {}
            pseudocode = context.get('pseudocode') or {}
            if mips.get('complete') is not True:
                raise ValueError('STARTUP_FRONTIER_PACKET_CONTAINS_TRUNCATED_ORIGINAL')
            audit = keyed.get((function['image'], function['address']))
            if not audit:
                raise ValueError('STARTUP_FRONTIER_FUNCTION_AUDIT_MISSING')
            if (audit.get('mips_sha256') != mips.get('sha256') or
                    audit.get('pseudocode_sha256') != pseudocode.get('sha256')):
                raise ValueError('STARTUP_FRONTIER_FUNCTION_AUDIT_IDENTITY_MISMATCH')
            expected_pseudocode = pseudocode.get('complete') is True
            if (audit.get('pseudocode_used_as_baseline') is not expected_pseudocode or
                    audit.get('complete_mips_audit') is not True or
                    audit.get('unresolved_semantics') != []):
                raise ValueError('STARTUP_FRONTIER_FUNCTION_AUDIT_INCOMPLETE')
        next_step = 'verify_frontier'
    elif kind == 'trace_capture_review':
        if decision.get('approved') is not True or not decision.get('changed_files'):
            raise ValueError('STARTUP_TRACE_CAPTURE_REQUIRES_REVIEWED_CHANGES')
        validate_capture_contract(root)
        next_step = 'convergence_contract'
    elif kind == 'convergence_review':
        if decision.get('approved') is not True or not decision.get('changed_files'):
            raise ValueError('STARTUP_CONVERGENCE_REQUIRES_REVIEWED_CHANGES')
        validate_convergence_contract(root)
        next_step = 'emulator'
    else:
        raise ValueError('STARTUP_UNSUPPORTED_ATTENTION_KIND: '+kind)
    history = state.setdefault('history', [])
    history.append({'revision': state['revision'], 'attention_sha256': attention['sha256'],
                    'response_sha256': digest_bytes(canonical(response)), 'kind': kind,
                    'accepted_utc': datetime.datetime.now(datetime.timezone.utc).isoformat()})
    return publish(root, state, status='running', step=next_step, attention=None, failure=None)


def record_step(state, name, result, started=None):
    state.setdefault('steps', {})[name] = {'status': 'passed', 'result': result,
        'completed_utc': datetime.datetime.now(datetime.timezone.utc).isoformat()}
    if started is not None:
        state['steps'][name]['seconds'] = round(time.perf_counter()-started, 3)


def publish_progress(root, state, detail, **metrics):
    progress = {'detail': detail, 'updated_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(), **metrics}
    return publish(root, state, progress=progress)


def psyq_review_summary(root, classification):
    report = read_json(Path(root)/classification['output'])
    accepted = report.get('accepted', []);pending = report.get('pending', [])
    return {'classification': classification,
            'accepted_by_image': dict(collections.Counter(item['image'] for item in accepted)),
            'wrapper_statuses': dict(collections.Counter(item.get('wrapper_status', 'unknown') for item in accepted)),
            'accepted': [{'image': item['image'], 'address': '0x%08X'%item['address'],
                          'alias': item.get('alias'), 'objects': sorted({match['object'] for match in item.get('matches', [])}),
                          'wrapper_status': item.get('wrapper_status'), 'wrapper_references': item.get('wrapper_references', [])}
                         for item in accepted[:64]],
            'accepted_truncated': len(accepted) > 64,
            'pending': [{'image': item['image'], 'address': '0x%08X'%item['address'],
                         'names': item.get('names', []), 'reason': item.get('reason')} for item in pending[:64]],
            'pending_truncated': len(pending) > 64}


def frontier_packet(root):
    config = project(root)
    database = project_path(root, config, 'analysis_database', 'status/analysis.sqlite')
    with contextlib.closing(sqlite3.connect(database)) as db:
        functions = {(image, address): (name, sha, listing, pseudocode, status)
                     for image, address, name, sha, listing, pseudocode, status in db.execute(
                         'SELECT image,address,name,sha256,listing,pseudocode,status FROM functions')}
        configured = config.get('analysis', {}).get('native_entrypoints')
        if configured:
            roots = [(item['image'], int(str(item['address']), 0)) for item in configured]
        else:
            image = config.get('analysis', {}).get('default_image')
            ida = read_json(project_path(root, config, 'ida_configs', 'tools/ida')/(image+'.json'))
            roots = [(image, ida['entry'])]
        if any(identity not in functions for identity in roots):
            raise ValueError('Configured native entrypoint is not an analyzed function')
        selected = [identity for identity in roots if functions[identity][4] == 'TODO']
        remaining = sum(row[4] == 'TODO' for row in functions.values())
        rows = [(image, address, *functions[(image, address)][:4])
                for image, address in selected]
    os.environ['XPORT_PROJECT'] = str(Path(root).resolve())
    import source_context
    source_context = importlib.reload(source_context)
    functions = [{'image': image, 'address': address, 'name': name, 'sha256': sha,
                  'listing': listing, 'pseudocode': pseudocode,
                  'audit_context': source_context.compact(
                      image, address, budget=2097152, full_source=True, full_original=True),
                  'proposed_change': {'status': 'WIP', 'source': None, 'semantic_name': None,
                                      'evidence_required': ['reviewed MIPS/IDA context', 'source mapping', 'verification result']}}
                 for image, address, name, sha, listing, pseudocode in rows]
    for function in functions:
        context = function['audit_context']
        if (context.get('mips') or {}).get('complete') is not True:
            raise ValueError('STARTUP_FRONTIER_REQUIRES_COMPLETE_MIPS_CONTEXT')
    scope = 'Configured executable entrypoints only; callees are deferred to function coverage'
    return {'functions': functions, 'dependency_closed': False, 'cyclic': False, 'remaining_todo': remaining,
            'native_entrypoints': [{'image': image, 'address': address} for image, address in roots],
            'reachable_functions': None,
            'scope': scope,
            'translation_contract': {
                'pseudocode_baseline_required': False,
                'complete_mips_audit_required': True,
                'semantic_simplification_allowed': False,
                'unresolved_semantics_allowed': False,
                'code_refresh_is_semantic_evidence': False,
                'required_response_field': 'decision.function_audits',
            },
            'ledger': 'status/translation-ledger.json'}


def advance(root, state):
    root = Path(root).resolve()
    while state.get('status') == 'running':
        step = state['step']
        started = time.perf_counter()
        try:
            if step == 'initialize':
                result = initialize_project(root, state['name'], state['short_name'], state.get('first_port', 2400))
                os.environ['XPORT_PROJECT'] = str(root)
                record_step(state, step, {'name': result['name'], 'short_name': result['short_name']}, started)
                state = publish(root, state, step='git_index')
            elif step == 'git_index':
                result = stage_project_gitignore(root)
                record_step(state, step, result, started)
                state = publish(root, state, step='github')
            elif step == 'github':
                result = startup_github.sync_project(root)
                if result.get('branch'):
                    config = project(root)
                    config['github']['branch'] = result['branch']
                    atomic_json(root/CONFIG_NAME, config)
                atomic_json(root/'status/startup-github.json', result)
                record_step(state, step, result, started)
                state = publish(root, state, step='build')
            elif step == 'build':
                doctor = run_shared(root, 'doctor')
                build = run_shared(root, 'build_native')
                record_step(state, step, {'doctor': doctor, 'build': build}, started)
                state = publish(root, state, step='extract_data')
            elif step == 'extract_data':
                result = startup_disc.extract(root)
                atomic_json(root/'status/startup-extraction.json', result)
                record_step(state, step, {'disc_count': len(result['discs']), 'file_count': result['file_count']}, started)
                state = publish(root, state, step='inventory')
            elif step == 'inventory':
                found = inventory(root)
                state['inventory'] = found
                config = project(root)
                images = config.get('analysis', {}).get('images', [])
                configs = Path(root)/config.get('paths', {}).get('ida_configs', 'tools/ida')
                missing = [image for image in images if not (configs/(image+'.json')).is_file()]
                if not images or missing:
                    proposal = image_review_proposal(root, found)
                    state = set_attention(root, state, 'image_review',
                        'Confirm original inputs and image-qualified IDA profiles',
                        ['config_patch.paths', 'config_patch.analysis.images', 'ida_configs'],
                        {'inventory': found, 'proposal_available': proposal is not None}, proposal)
                    continue
                record_step(state, step, found, started)
                state = publish(root, state, step='preflight')
            elif step == 'preflight':
                toolchain = validate_toolchain_receipt()
                state.setdefault('decisions', {})['tools'] = toolchain['tools']
                result = {'toolchain': toolchain, 'project': project_preflight(root)}
                record_step(state, step, result, started)
                state = publish(root, state, step='music')
            elif step == 'music':
                config = project(root)
                cues = list((root/config.get('paths', {}).get('music_source', 'iso')).glob('*.cue'))
                if len(cues) == 1 and 'redbook_audio' in config.get('capabilities', {}):
                    result = run_shared(root, 'convert_music')
                elif len(cues) > 1:
                    raise ValueError('Multiple CUE files require an explicit music_source')
                else:
                    result = 'not_requested'
                record_step(state, step, result, started)
                state = publish(root, state, step='ida_export')
            elif step == 'ida_export':
                state = publish_progress(root, state, 'Running IDA export and independent byte verification')
                tools = state['decisions']['tools']
                labels = state.get('analysis_labels', {})
                plan = prepare_ida_export(root, tools['ida'], labels.get('ida_plan', 'startup-ida-plan'), plan=True)
                label = labels.get('ida', 'startup-ida-v1')
                actual = prepare_ida_export(root, tools['ida'], label)
                canonical = canonicalize_ida(root, 'status/ida/runs/'+label, 'tools/ida')
                accepted = accept_ida(root, label)
                record_step(state, step, {'plan': plan, 'actual': actual, 'canonical': canonical, 'accepted': accepted}, started)
                state = publish(root, state, step='psyq')
            elif step == 'psyq':
                state = publish_progress(root, state, 'Running Ghidra PsyQ recognition')
                tools = state['decisions']['tools']
                label = state.get('analysis_labels', {}).get('psyq', 'startup-psyq-v1')
                recognition = run_ghidra(root, tools['ghidra'], tools['plugin'], label)
                classification = classify_psyq(root, 'status/ghidra/runs/'+label)
                record_step(state, step, {'recognition': recognition, 'classification': classification}, started)
                state = set_attention(root, state, 'psyq_review', 'Review accepted and pending PsyQ classifications',
                                      ['decision.approved'], psyq_review_summary(root, classification))
            elif step == 'database':
                build = run_shared(root, 'build_database')
                similarity = validate_similarity(root)
                progress = run_shared(root, 'render_progress')
                record_step(state, step, {'build_database': build, 'similarity': similarity, 'progress': progress}, started)
                packet = frontier_packet(root)
                if packet['functions']:
                    state = set_attention(root, state, 'frontier_translation',
                                          'Translate only the reviewed executable entrypoint',
                                          ['decision.approved', 'decision.changed_files'], packet)
                else:
                    state = publish(root, state, step='verify_frontier')
            elif step == 'verify_frontier':
                refresh = run_shared(root, 'source_index', ('--rebuild', '--no-format'))
                progress = run_shared(root, 'render_progress')
                record_step(state, step, {'source_index': refresh,
                                          'progress': progress,
                                          'native_link_deferred_to_coverage': True}, started)
                state = publish(root, state, step='trace_contract')
            elif step == 'trace_contract':
                packet = capture_contract_packet(root)
                if packet['problem']:
                    state = set_attention(root, state, 'trace_capture_review',
                                          'Audit and configure the original recording contract',
                                          ['decision.approved', 'decision.changed_files'], packet)
                else:
                    record_step(state, step, packet['current'], started)
                    state = publish(root, state, step='convergence_contract')
            elif step == 'convergence_contract':
                packet = convergence_contract_packet(root)
                if packet['problem']:
                    state = set_attention(root, state, 'convergence_review',
                                          'Implement and review the native convergence contract',
                                          ['decision.approved', 'decision.changed_files'], packet)
                else:
                    record_step(state, step, packet['current'], started)
                    state = publish(root, state, step='emulator')
            elif step == 'emulator':
                if 'verify_frontier' not in state.get('steps', {}):
                    refresh = run_shared(root, 'source_index', '--rebuild', '--no-format')
                    record_step(state, 'verify_frontier', {'source_index': refresh,
                                'native_link_deferred_to_coverage': True})
                config = project(root)
                capture = validate_capture_contract(root)
                launcher = ensure_duckstation_launcher(root)
                record_launcher = ensure_record_launcher(root)
                deployments = {}
                for role in config['duckstation']['reserved_gdb_ports']:
                    deployments[role] = run_shared(root, 'duckstation_deploy', ('--role', role, '--ensure'))
                emulator = {'launcher': launcher, 'record_launcher': record_launcher,
                            'deployments': deployments, 'capture': capture,
                            'shared_distribution': str((TOOLS/'duckstation/distribution').resolve())}
                record_step(state, step, emulator, started)
                progress = read_json(root/'status/progress-data.json')
                completion = {'project': config['name'], 'short_name': config.get('short_name'),
                              'project_root': str(root), 'inputs': state.get('inventory', {}),
                              'ida': state['steps'].get('ida_export', {}).get('result'),
                              'psyq': state['steps'].get('psyq', {}).get('result'),
                              'decompilation_status': progress.get('decompilation_status'),
                              'native_solution': config['paths']['native_solution'],
                              'native_executable': config['paths']['native_executable'],
                              'native_debug_executable': config['paths']['native_debug_executable'],
                              'duckstation': emulator,
                              'convergence': validate_convergence_contract(root),
                              'source_index': state['steps']['verify_frontier']['result']['source_index'],
                              'next': 'Run function_converge NAME, stop, then coverage NAME'}
                created = datetime.datetime.fromisoformat(state['created_utc'])
                canonical_result = state['steps'].get('ida_export', {}).get('result', {}).get('canonical', [])
                classification = state['steps'].get('psyq', {}).get('result', {}).get('classification', {})
                completion['metrics'] = {
                    'total_seconds': round((datetime.datetime.now(datetime.timezone.utc)-created).total_seconds(), 3),
                    'step_seconds': {name: value.get('seconds') for name, value in state['steps'].items()},
                    'input_bytes': state.get('inventory', {}).get('total_bytes',
                                       sum(item.get('bytes', 0) for item in state.get('inventory', {}).get('files', []))),
                    'ida_functions': sum(item.get('functions', 0) for item in canonical_result),
                    'psyq_accepted': classification.get('accepted', 0), 'psyq_pending': classification.get('pending', 0),
                    'recovery_count': len(state.get('recoveries', []))}
                state = publish(root, state, status='complete', step='complete', completion=completion, attention=None)
            else:
                raise ValueError('Unknown startup step: '+str(step))
        except Exception as error:
            append_log(root, 'failure '+step, type(error).__name__+': '+str(error))
            state = publish(root, state, status='failed', failure={'step': step, 'type': type(error).__name__, 'message': str(error)})
    return state


def archive_failed_artifacts(root, state):
    """Retain compact diagnostics and deduplicate their content before a retry"""
    root = Path(root).resolve();step = state['failure']['step']
    targets = []
    if step == 'emulator':
        config = project(root)
        targets.extend(root/path for path in config['duckstation']['data_directories'].values())
    if step == 'ida_export':
        labels = state.get('analysis_labels', {})
        targets.extend(root/'status/ida/runs'/name for name in (labels.get('ida_plan', 'startup-ida-plan'), labels.get('ida', 'startup-ida-v1')))
    if step in ('psyq', 'database', 'verify_frontier'):
        targets.append(root/'status/ghidra/runs'/state.get('analysis_labels', {}).get('psyq', 'startup-psyq-v1'))
    recovery = root/'status/startup-recovery';blobs = recovery/'blobs';blobs.mkdir(parents=True, exist_ok=True)
    manifest = {'revision': state['revision'], 'step': step, 'files': []}
    for target in targets:
        if not target.exists():continue
        paths = [target] if target.is_file() else sorted(path for path in target.rglob('*') if path.is_file())
        for path in paths:
            size = path.stat().st_size;digest = digest_file(path);retained = size <= 1024*1024 and path.suffix.lower() in ('.json','.log','.txt','.md')
            if retained:
                blob = blobs/digest
                if not blob.exists():shutil.copy2(path, blob)
            manifest['files'].append({'path': relative(root, path), 'bytes': size, 'sha256': digest, 'retained': retained})
        if target.is_dir():shutil.rmtree(target)
        else:target.unlink()
    path = recovery/('revision-'+str(state['revision'])+'.json');atomic_json(path, manifest)
    return relative(root, path)


def retry(root, expected_revision, advance_now=True):
    root = Path(root).resolve()
    with StartupLock(root):
        state = load_state(root)
        if state.get('status') != 'failed':raise ValueError('STARTUP_RETRY_REQUIRES_FAILED_STATE')
        if state['revision'] != expected_revision:raise ValueError('STARTUP_STALE_RETRY')
        stored_inventory = dict(state.get('inventory', {}))
        if stored_inventory and 'total_bytes' not in stored_inventory:
            stored_inventory['total_bytes'] = sum(item.get('bytes', 0) for item in stored_inventory.get('files', []))
        current_inventory = inventory(root) if stored_inventory else {}
        if 'system_configs' not in stored_inventory:
            current_inventory.pop('system_configs', None)
        if stored_inventory and digest_bytes(canonical(current_inventory)) != digest_bytes(canonical(stored_inventory)):
            raise ValueError('STARTUP_INPUTS_CHANGED: review images before retry')
        receipt = archive_failed_artifacts(root, state)
        state.setdefault('recoveries', []).append({'revision': state['revision'], 'step': state['failure']['step'], 'receipt': receipt,
                                                   'utc': datetime.datetime.now(datetime.timezone.utc).isoformat()})
        state = publish(root, state, status='running', step=state['failure']['step'], failure=None,
                        progress={'detail': 'Retrying '+state['failure']['step']})
        return advance(root, state) if advance_now else state


class StartupLock:
    def __init__(self, root):
        self.path = Path(root)/'status/startup.lock'
        self.pid = os.getpid()

    def __enter__(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        try:
            with self.path.open('x', encoding='ascii') as stream:
                stream.write(str(self.pid)+'\n')
        except FileExistsError:
            try:
                owner = int(self.path.read_text().strip())
                running = process_is_running(owner)
            except ValueError:
                running = False
            if not running:
                self.path.unlink(missing_ok=True)
                with self.path.open('x', encoding='ascii') as stream:
                    stream.write(str(self.pid)+'\n')
            else:
                raise ValueError('STARTUP_WORKER_ALREADY_RUNNING: '+str(owner))
        return self

    def __exit__(self, *_):
        try:
            if self.path.read_text().strip() == str(self.pid):
                self.path.unlink()
        except FileNotFoundError:
            pass


def reexport(root, expected_revision, label, exports, advance_now=True):
    """Supersede a reviewed faulty export without modifying accepted evidence"""
    root = Path(root).resolve()
    with StartupLock(root):
        state = load_state(root)
        if state['revision'] != expected_revision:
            raise ValueError('STARTUP_STALE_REEXPORT')
        if state.get('status') != 'attention' or state.get('attention', {}).get('kind') not in ('psyq_review', 'frontier_translation'):
            raise ValueError('STARTUP_REEXPORT_REQUIRES_PSYQ_ATTENTION_OR_UNTRANSLATED_FRONTIER')
        ledger = read_json(root/'status/translation-ledger.json') if (root/'status/translation-ledger.json').exists() else {}
        if ledger.get('entries'):
            raise ValueError('STARTUP_REEXPORT_REQUIRES_EMPTY_TRANSLATION_LEDGER')
        if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_-]{0,63}', label):
            raise ValueError('STARTUP_INVALID_REEXPORT_LABEL')
        destination = (root/exports).resolve()
        if not destination.is_relative_to(root/'orig') or destination == root/'orig':
            raise ValueError('STARTUP_REEXPORT_REQUIRES_NEW_ORIG_ROOT')
        if destination.exists():
            raise ValueError('STARTUP_REEXPORT_DESTINATION_EXISTS')
        labels = {'ida': label, 'ida_plan': label+'-plan', 'psyq': label+'-psyq'}
        for key, value in labels.items():
            base = 'status/ghidra/runs' if key == 'psyq' else 'status/ida/runs'
            if (root/base/value).exists() or (root/'status/ida/accepted'/(value+'.json')).exists():
                raise ValueError('STARTUP_REEXPORT_LABEL_EXISTS')
        config = project(root)
        receipt = root/'status/startup-recovery'/('reexport-revision-'+str(expected_revision)+'.json')
        if receipt.exists():raise ValueError('STARTUP_REEXPORT_RECEIPT_EXISTS')
        atomic_json(receipt, {'state': state, 'config': config, 'new_export_root': relative(root,destination), 'labels': labels})
        config['paths']['ida_exports'] = relative(root,destination)
        atomic_json(root/CONFIG_NAME, config)
        state.setdefault('recoveries', []).append({'revision': expected_revision, 'step': 'ida_export',
                                                  'receipt': relative(root,receipt), 'preserved_accepted_exports': True})
        state['analysis_labels'] = labels
        state.get('steps', {}).pop('ida_export', None)
        state.get('steps', {}).pop('psyq', None)
        state = publish(root,state,status='running',step='ida_export',attention=None,failure=None,
                        progress={'detail': 'Re-exporting into a new immutable evidence root'})
        return advance(root,state) if advance_now else state


def refresh_frontier(root, expected_revision):
    """Regenerate paused translation evidence after a hash-bound pseudocode review"""
    root = Path(root).resolve()
    with StartupLock(root):
        state = load_state(root)
        if state.get('status') != 'attention' or state.get('attention', {}).get('kind') != 'frontier_translation':
            raise ValueError('STARTUP_REFRESH_REQUIRES_FRONTIER_ATTENTION')
        if state['revision'] != expected_revision:
            raise ValueError('STARTUP_STALE_REFRESH')
        packet = frontier_packet(root)
        return set_attention(root, state, 'frontier_translation',
                             'Translate only the reviewed executable entrypoint',
                             ['decision.approved', 'decision.changed_files'], packet)


def start(root, name, short_name, first_port=2400, advance_now=True):
    root = Path(root).resolve()
    with StartupLock(root):
        if (root/CONFIG_NAME).exists():
            raise ValueError('STARTUP_ALREADY_INITIALIZED: '+str(root/CONFIG_NAME))
        if state_path(root).exists():
            raise ValueError('STARTUP_ALREADY_STARTED: use status or submit')
        validate_names(name, short_name)
        state = {'schema': 1, 'revision': 1, 'status': 'running', 'step': 'initialize',
                 'project_root': str(root), 'name': name, 'short_name': short_name,
                 'first_port': first_port, 'steps': {}, 'history': [], 'decisions': {},
                 'attention': None, 'failure': None, 'completion': None,
                 'created_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
                 'updated_utc': datetime.datetime.now(datetime.timezone.utc).isoformat()}
        atomic_json(state_path(root), state)
        return advance(root, state) if advance_now else state


def refresh_completion(root, expected_revision, advance_now=True):
    """Regenerate a completed startup handoff from current derived source state"""
    root = Path(root).resolve()
    with StartupLock(root):
        state = load_state(root)
        if state['revision'] != expected_revision:
            raise ValueError('STARTUP_REVISION_MISMATCH')
        if state.get('status') != 'complete':
            raise ValueError('STARTUP_NOT_COMPLETE')
        validate_capture_contract(root)
        state = publish(root, state, status='running', step='verify_frontier',
                        completion=None, attention=None, failure=None)
        return advance(root, state) if advance_now else state


def reopen_convergence(root, advance_now=True):
    """Resume a completed legacy startup at the convergence readiness gate"""
    root = Path(root).resolve()
    with StartupLock(root):
        state = load_state(root)
        refresh = (state.get('status') == 'attention' and
                   state.get('attention', {}).get('kind') in ('convergence_review', 'frontier_translation'))
        if state.get('status') != 'complete' and not refresh:
            raise ValueError('STARTUP_NOT_COMPLETE: use status, submit or retry')
        progress_path = root/'status/progress-data.json'
        progress = read_json(progress_path).get('decompilation_status', {}) if progress_path.is_file() else {}
        next_step = 'database' if progress.get('TODO', 1) else 'convergence_contract'
        state = publish(root, state, status='running', step=next_step,
                        completion=None, attention=None, failure=None)
        return advance(root, state) if advance_now else state


def submit(root, response, advance_now=True):
    root = Path(root).resolve()
    with StartupLock(root):
        state = load_state(root)
        state = apply_response(root, state, read_json(response))
        return advance(root, state) if advance_now else state


def process_is_running(pid):
    pid = int(pid)
    if pid <= 0:
        return False
    if os.name == 'nt':
        import ctypes
        kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
        handle = kernel32.OpenProcess(0x1000, False, pid)
        if not handle:
            return False
        try:
            exit_code = ctypes.c_ulong()
            return bool(kernel32.GetExitCodeProcess(handle, ctypes.byref(exit_code))) and exit_code.value == 259
        finally:
            kernel32.CloseHandle(handle)
    try:
        os.kill(pid, 0)
        return True
    except (OSError, SystemError):
        return False


def dispatch_worker(root):
    root = Path(root).resolve();receipt_path = root/'status/startup-worker.json'
    if receipt_path.is_file():
        try:
            prior = read_json(receipt_path)
            if prior.get('status') == 'running' and process_is_running(prior['pid']):
                return prior
        except (ValueError, KeyError, json.JSONDecodeError):
            pass
    try:
        step = load_state(root).get('step')
    except (OSError, ValueError, json.JSONDecodeError):
        step = None
    if step in ('inventory', 'preflight', 'music', 'ida_export'):
        validate_runtime_access()
    log = (root/'status/startup-worker.log').open('ab')
    flags = 0
    desktop = None
    if os.name == 'nt' and os.environ.get('XPORT_WINDOWS_DESKTOP'):
        from windows_desktop import launch_default_desktop_process, process_desktop
        desktop = process_desktop()
    if os.name == 'nt':
        flags = subprocess.CREATE_NO_WINDOW|subprocess.CREATE_NEW_PROCESS_GROUP|subprocess.DETACHED_PROCESS
    command = [sys.executable, '-B', str(Path(__file__).resolve()), 'work', '--project', str(root)]
    if desktop:
        process = launch_default_desktop_process(command, root, visible=False, environment=dict(os.environ))
    else:
        process = subprocess.Popen(command,
                               cwd=root, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                               creationflags=flags, start_new_session=os.name != 'nt')
    receipt = {'pid': process.pid, 'status': 'running', 'created_utc': datetime.datetime.now(datetime.timezone.utc).isoformat()}
    atomic_json(receipt_path, receipt);log.close();return receipt


def work(root):
    root = Path(root).resolve();receipt_path = root/'status/startup-worker.json'
    try:
        if os.name == 'nt':
            from windows_desktop import current_desktop_name
            atomic_json(root/'status/startup-worker-desktop.json',
                        {'pid': os.getpid(), 'desktop': current_desktop_name(),
                         'requested': os.environ.get('XPORT_WINDOWS_DESKTOP', 'WinSta0\\Default')})
        with (root/'status/startup-worker.log').open('a', encoding='utf-8') as output:
            with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
                with StartupLock(root):return advance(root, load_state(root))
    finally:
        receipt = read_json(receipt_path) if receipt_path.is_file() else {'pid': os.getpid()}
        receipt.update(status='stopped', stopped_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
        atomic_json(receipt_path, receipt)


def status(root, after_revision=None, resume=True):
    root = Path(root).resolve()
    state = load_state(root)
    if resume and state.get('status') == 'running':
        try:
            with StartupLock(root):
                state = advance(root, load_state(root))
        except ValueError as error:
            if not str(error).startswith('STARTUP_WORKER_ALREADY_RUNNING'):
                raise
            state = load_state(root)
    return compact(state, changed=after_revision is None or state['revision'] > after_revision)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest='action', required=True)
    start_parser = subparsers.add_parser('start')
    start_parser.add_argument('--project', type=Path, default=Path.cwd())
    start_parser.add_argument('--name', required=True)
    start_parser.add_argument('--short-name', required=True)
    start_parser.add_argument('--first-port', type=int, default=2400)
    status_parser = subparsers.add_parser('status')
    status_parser.add_argument('--project', type=Path, default=Path.cwd())
    status_parser.add_argument('--after-revision', type=int)
    submit_parser = subparsers.add_parser('submit')
    submit_parser.add_argument('--project', type=Path, default=Path.cwd())
    submit_parser.add_argument('--response', type=Path)
    retry_parser = subparsers.add_parser('retry')
    retry_parser.add_argument('--project', type=Path, default=Path.cwd())
    retry_parser.add_argument('--expected-revision', required=True, type=int)
    work_parser = subparsers.add_parser('work')
    work_parser.add_argument('--project', type=Path, default=Path.cwd())
    refresh_parser = subparsers.add_parser('refresh-frontier')
    refresh_parser.add_argument('--project', type=Path, default=Path.cwd())
    refresh_parser.add_argument('--expected-revision', required=True, type=int)
    completion_parser = subparsers.add_parser('refresh-completion')
    completion_parser.add_argument('--project', type=Path, default=Path.cwd())
    completion_parser.add_argument('--expected-revision', required=True, type=int)
    reopen_parser = subparsers.add_parser('reopen-convergence')
    reopen_parser.add_argument('--project', type=Path, default=Path.cwd())
    reexport_parser = subparsers.add_parser('reexport')
    reexport_parser.add_argument('--project', type=Path, default=Path.cwd())
    reexport_parser.add_argument('--expected-revision', required=True, type=int)
    reexport_parser.add_argument('--label', required=True)
    reexport_parser.add_argument('--exports', required=True)
    args = parser.parse_args()
    try:
        if args.action == 'start':
            if not 1024 <= args.first_port <= 65533:
                parser.error('First port must be 1024..65533')
            validate_toolchain_receipt()
            result = compact(start(args.project, args.name, args.short_name, args.first_port, advance_now=False))
            dispatch_worker(args.project)
        elif args.action == 'submit':
            pending = load_state(args.project)
            if pending.get('attention', {}).get('kind') == 'image_review':
                validate_runtime_access()
            path = args.response or response_path(args.project)
            result = compact(submit(args.project, path, advance_now=False))
            dispatch_worker(args.project)
        elif args.action == 'retry':
            pending = load_state(args.project)
            if pending.get('failure', {}).get('step') == 'ida_export':
                validate_runtime_access()
            result = compact(retry(args.project, args.expected_revision, advance_now=False))
            dispatch_worker(args.project)
        elif args.action == 'reexport':
            validate_runtime_access()
            result = compact(reexport(args.project, args.expected_revision, args.label, args.exports, advance_now=False))
            dispatch_worker(args.project)
        elif args.action == 'work':
            result = compact(work(args.project))
        elif args.action == 'refresh-frontier':
            result = compact(refresh_frontier(args.project, args.expected_revision))
        elif args.action == 'refresh-completion':
            result = compact(refresh_completion(args.project, args.expected_revision, advance_now=False))
            dispatch_worker(args.project)
        elif args.action == 'reopen-convergence':
            result = compact(reopen_convergence(args.project, advance_now=False))
            dispatch_worker(args.project)
        else:
            state = load_state(args.project)
            if state.get('status') == 'running':dispatch_worker(args.project)
            result = compact(load_state(args.project), changed=args.after_revision is None or state['revision'] > args.after_revision)
    except (OSError, ValueError, RuntimeError) as error:
        print(json.dumps({'status': 'error', 'error': str(error)}, indent=2, ensure_ascii=False))
        raise SystemExit(2) from None
    print(json.dumps(result, indent=2, ensure_ascii=False))


if __name__ == '__main__':
    main()

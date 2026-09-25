"""Run the complete new-project startup protocol through one durable supervisor"""
import argparse
import collections
import datetime
import hashlib
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

from upgrade import shared_changelog
from xport_process import normalized_environment
from xport_project import CONFIG_NAME, allocation_lock, reservations


TOOLS = Path(__file__).resolve().parent
XPORT_ROOT = TOOLS.parent
SHORT_NAME_PATTERN = re.compile(r'[A-Za-z][A-Za-z0-9_]{0,15}')
LABEL_PATTERN = re.compile(r'[A-Za-z0-9_-]{1,64}')
IMAGE_PATTERN = re.compile(r'[A-Za-z0-9_.-]+')
WINDOWS_RESERVED_NAMES = {'CON', 'PRN', 'AUX', 'NUL', *(f'COM{i}' for i in range(1, 10)),
                          *(f'LPT{i}' for i in range(1, 10))}
CPP_PROJECT_TYPE = 'BC8A1FFA-BEE3-4634-8014-F334798102B3'
TERMINAL = {'attention', 'complete', 'failed', 'cancelled'}


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
    result['log'] = str(log_path(state['project_root']))
    result['response'] = str(response_path(state['project_root']))
    return result


def set_attention(root, state, kind, summary, required, context=None):
    body = {'kind': kind, 'summary': summary, 'required': required, 'context': context or {}}
    body['sha256'] = digest_bytes(canonical(body))
    state = publish(root, state, status='attention', step=kind, attention=body, failure=None)
    atomic_json(response_path(root), {'revision': state['revision'], 'attention_sha256': body['sha256'],
                                     'decision': {}})
    return state


def validate_names(name, short_name):
    if not name or any(c in name for c in '/\\:";\r\n'):
        raise ValueError('Invalid project name')
    if not SHORT_NAME_PATTERN.fullmatch(short_name or '') or short_name.upper() in WINDOWS_RESERVED_NAMES:
        raise ValueError('Short name must be a non-reserved Windows name starting with a letter and containing 1..16 ASCII letters, digits or underscores')


def solution_text(short_name, project_guid):
    return f'''Microsoft Visual Studio Solution File, Format Version 12.00
# Visual Studio Version 17
VisualStudioVersion = 17.0.31903.59
MinimumVisualStudioVersion = 10.0.40219.1
Project("{{{CPP_PROJECT_TYPE}}}") = "{short_name}", "{short_name}.vcxproj", "{{{project_guid}}}"
EndProject
Global
 GlobalSection(SolutionConfigurationPlatforms) = preSolution
  Debug|x86 = Debug|x86
  Release|x86 = Release|x86
 EndGlobalSection
 GlobalSection(ProjectConfigurationPlatforms) = postSolution
  {{{project_guid}}}.Debug|x86.ActiveCfg = Debug|Win32
  {{{project_guid}}}.Debug|x86.Build.0 = Debug|Win32
  {{{project_guid}}}.Release|x86.ActiveCfg = Release|Win32
  {{{project_guid}}}.Release|x86.Build.0 = Release|Win32
 EndGlobalSection
EndGlobal
'''


def project_text(name, short_name, project_guid, shared_source):
    window_title = (name+' (xport)').replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')
    return fr'''<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Debug|Win32"><Configuration>Debug</Configuration><Platform>Win32</Platform></ProjectConfiguration>
    <ProjectConfiguration Include="Release|Win32"><Configuration>Release</Configuration><Platform>Win32</Platform></ProjectConfiguration>
  </ItemGroup>
  <PropertyGroup Label="Globals"><VCProjectVersion>17.0</VCProjectVersion><ProjectGuid>{{{project_guid}}}</ProjectGuid><RootNamespace>{short_name}</RootNamespace><WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion></PropertyGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.Default.props" />
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Debug|Win32'" Label="Configuration"><ConfigurationType>Application</ConfigurationType><UseDebugLibraries>true</UseDebugLibraries><PlatformToolset>v143</PlatformToolset><CharacterSet>MultiByte</CharacterSet></PropertyGroup>
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Release|Win32'" Label="Configuration"><ConfigurationType>Application</ConfigurationType><UseDebugLibraries>false</UseDebugLibraries><PlatformToolset>v143</PlatformToolset><CharacterSet>MultiByte</CharacterSet><WholeProgramOptimization>true</WholeProgramOptimization></PropertyGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.props" />
  <PropertyGroup><OutDir>$(ProjectDir)..\..\..\bin\</OutDir><IntDir>$(ProjectDir)..\..\..\_build\x86\$(Configuration)\</IntDir><TargetName>{short_name}</TargetName><LocalDebuggerWorkingDirectory>$(OutDir)</LocalDebuggerWorkingDirectory><LocalDebuggerCommand>$(TargetPath)</LocalDebuggerCommand><DebuggerFlavor>WindowsLocalDebugger</DebuggerFlavor><LinkIncremental>false</LinkIncremental></PropertyGroup>
  <ItemDefinitionGroup Condition="'$(Configuration)|$(Platform)'=='Debug|Win32'">
    <ClCompile><WarningLevel>Level3</WarningLevel><DisableSpecificWarnings>4100;%(DisableSpecificWarnings)</DisableSpecificWarnings><SDLCheck>true</SDLCheck><PreprocessorDefinitions>_DEBUG;_CRT_SECURE_NO_WARNINGS;WND_TITLE=&quot;{window_title}&quot;;WND_WIDTH=960;WND_HEIGHT=768;FIELD_RATE=50;%(PreprocessorDefinitions)</PreprocessorDefinitions><AdditionalIncludeDirectories>$(ProjectDir)..\..;$(ProjectDir){shared_source};%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories><CompileAs>CompileAsC</CompileAs><Optimization>Disabled</Optimization><RuntimeLibrary>MultiThreadedDebugDLL</RuntimeLibrary><DebugInformationFormat>ProgramDatabase</DebugInformationFormat><ProgramDataBaseFileName>$(IntDir){short_name}-compile.pdb</ProgramDataBaseFileName><MultiProcessorCompilation>true</MultiProcessorCompilation></ClCompile>
    <Link><SubSystem>Windows</SubSystem><GenerateDebugInformation>true</GenerateDebugInformation><ProgramDatabaseFile>$(OutDir){short_name}.pdb</ProgramDatabaseFile><ImportLibrary>$(IntDir){short_name}.lib</ImportLibrary><AdditionalDependencies>user32.lib;gdi32.lib;winmm.lib;%(AdditionalDependencies)</AdditionalDependencies></Link>
  </ItemDefinitionGroup>
  <ItemDefinitionGroup Condition="'$(Configuration)|$(Platform)'=='Release|Win32'">
    <ClCompile><WarningLevel>Level3</WarningLevel><DisableSpecificWarnings>4100;%(DisableSpecificWarnings)</DisableSpecificWarnings><SDLCheck>true</SDLCheck><PreprocessorDefinitions>NDEBUG;_CRT_SECURE_NO_WARNINGS;WND_TITLE=&quot;{window_title}&quot;;WND_WIDTH=960;WND_HEIGHT=768;FIELD_RATE=50;%(PreprocessorDefinitions)</PreprocessorDefinitions><AdditionalIncludeDirectories>$(ProjectDir)..\..;$(ProjectDir){shared_source};%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories><CompileAs>CompileAsC</CompileAs><Optimization>MaxSpeed</Optimization><RuntimeLibrary>MultiThreadedDLL</RuntimeLibrary><FunctionLevelLinking>true</FunctionLevelLinking><IntrinsicFunctions>true</IntrinsicFunctions><MultiProcessorCompilation>true</MultiProcessorCompilation></ClCompile>
    <Link><SubSystem>Windows</SubSystem><GenerateDebugInformation>false</GenerateDebugInformation><ImportLibrary>$(IntDir){short_name}.lib</ImportLibrary><EnableCOMDATFolding>true</EnableCOMDATFolding><OptimizeReferences>true</OptimizeReferences><AdditionalDependencies>user32.lib;gdi32.lib;winmm.lib;%(AdditionalDependencies)</AdditionalDependencies></Link>
  </ItemDefinitionGroup>
  <ItemGroup><ClCompile Include="..\..\game_main.c" /><ClCompile Include="{shared_source}\psx.c" /><ClCompile Include="{shared_source}\psx_gpu.c" /><ClCompile Include="{shared_source}\psx_pad.c" /><ClCompile Include="{shared_source}\psx_spu.c" /><ClCompile Include="{shared_source}\platform\win\main.c"><ObjectFileName>$(IntDir)xport_main.obj</ObjectFileName></ClCompile></ItemGroup>
  <ItemGroup><ClInclude Include="{shared_source}\xport.h" /><ClInclude Include="{shared_source}\psx.h" /><ClInclude Include="{shared_source}\psx_gpu.h" /><ClInclude Include="{shared_source}\psx_pad.h" /><ClInclude Include="{shared_source}\psx_spu.h" /></ItemGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.targets" />
</Project>
'''


def agents_text(name, short_name):
    return f'''# {name}

Resolve `[XPORT_ROOT]` from `xport-project.json` and read `[XPORT_ROOT]/tools/PIPELINE.md`. Keep only stable game-specific facts and task-routing links here; put detailed evidence under `status`.

## Project facts

- Native short name: `{short_name}`; language: C
- Solution: `src/platform/win/{short_name}.sln`; executable: `bin/{short_name}.exe`; working directory: `bin`; intermediates: `_build`
- Runtime data: `bin/DATA`; Red Book output when applicable: `bin/MUSIC`
- Before relying on them, record reviewed image identities, language decision, dummy scope, hooks/layouts, adapter contract and evidence links here
'''


def initialize_project(root, name, short_name, first_port=2400):
    root = Path(root).resolve()
    if (root/CONFIG_NAME).exists():
        raise ValueError('STARTUP_ALREADY_INITIALIZED: '+str(root/CONFIG_NAME))
    validate_names(name, short_name)
    platform = root/'src/platform/win'
    targets = [root/CONFIG_NAME, root/'AGENTS.md', root/'src/game_main.c', platform/(short_name+'.sln'),
               platform/(short_name+'.vcxproj'), root/'status/translation-ledger.json']
    existing = [str(path) for path in targets if path.exists()]
    if existing:
        raise ValueError('STARTUP_ROOT_NOT_EMPTY: project initialization would overwrite '+', '.join(existing))
    with allocation_lock(root.parent):
        used = reservations(root.parent)
        sockets, ports = [], {}
        try:
            candidate = first_port
            for role in ('audit', 'trace', 'user'):
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
            project_guid = str(uuid.uuid4()).upper()
            shared_source = os.path.relpath(XPORT_ROOT/'src', platform).replace('/', '\\')
            config = {'schema': 1, 'name': name, 'short_name': short_name, 'toolset': str(TOOLS),
                      'paths': {'status': 'status', 'project_tools': 'tools', 'analysis_database': 'status/analysis.sqlite',
                                'native_executable': 'bin/'+short_name+'.exe', 'native_working_directory': 'bin',
                                'native_solution': 'src/platform/win/'+short_name+'.sln', 'native_build': '_build',
                                'ida_exports': 'orig/images', 'ida_configs': 'tools/ida',
                                'sdk_classification': 'status/ghidra/classification.json'},
                      'duckstation': {'host': '127.0.0.1', 'gdb_port': ports['user'], 'reserved_gdb_ports': ports,
                                      'default_role': 'user', 'data_directory': 'tools/duckstation/data/user',
                                      'data_directories': {role: 'tools/duckstation/data/'+role for role in ports}},
                      'stage_pipeline': {'supervisor_protocol': {'schema': 1, 'enabled': True, 'mode': 'enforced',
                          'one_launch': True, 'notify_on_revision_only': True,
                          'batch_limits': {'functions': 5, 'instructions': 2048, 'modules': 2},
                          'acceptance': {'unchanged_model_calls': 0, 'interventions_per_failure': 1,
                              'submissions_per_batch': 1, 'fast_refreshes_per_batch': 1,
                              'provisional_replays_per_batch': 1, 'full_gate_after_provisional_match': True,
                              'max_control_responses': 5, 'max_compactions': 2,
                              'deterministic_call_reduction_percent': 90}}},
                      'code_refresh': {'build': {'solution': 'src/platform/win/'+short_name+'.sln',
                                                 'configuration': 'Debug', 'platform': 'x86',
                                                 'process_name': short_name+'.exe'}, 'tests': [], 'artifacts': []}}
            (platform/(short_name+'.sln')).write_text(solution_text(short_name, project_guid), encoding='utf-8', newline='')
            (platform/(short_name+'.vcxproj')).write_text(project_text(name, short_name, project_guid, shared_source), encoding='utf-8', newline='')
            revision = shared_changelog()[2][0]['revision']
            main_text = f'''#include "xport.h"

// XPORT REVISION: {revision}
int xport_main(int argc, char **argv)
{{
    if (argc < 0 || (argc > 0 && argv == NULL))
        return -1;
    return 0;
}}
'''
            (root/'src/game_main.c').write_text(main_text, encoding='utf-8', newline='')
            (root/'AGENTS.md').write_text(agents_text(name, short_name), encoding='utf-8', newline='')
            (root/'status/translation-ledger.json').write_text(json.dumps({'version': 1, 'entries': []}, indent=2)+'\n', encoding='utf-8')
            with (root/CONFIG_NAME).open('x', encoding='utf-8') as stream:
                json.dump(config, stream, indent=2)
                stream.write('\n')
            return config
        finally:
            for listener in sockets:
                listener.close()


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
    for base in (root/'iso', root/'orig'):
        if not base.exists():
            continue
        for path in sorted(item for item in base.rglob('*') if item.is_file()):
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
    return {'file_count': len(candidates), 'files': candidates[:64], 'files_truncated': len(candidates) > 64,
            'cue_files': [item['path'] for item in candidates if item['path'].lower().endswith('.cue')],
            'psx_executables': [item for item in candidates if item.get('psx_exe')]}


def discover_tools(root, supplied=None):
    config = project(root)
    configured = config.get('analysis', {}).get('tool_paths', {})
    supplied = supplied or {}
    candidates = {
        'ida': (supplied.get('ida'), configured.get('ida'), os.environ.get('XPORT_IDA'), os.environ.get('IDAT_PATH')),
        'ghidra': (supplied.get('ghidra'), configured.get('ghidra'), os.environ.get('XPORT_GHIDRA'), os.environ.get('GHIDRA_HOME')),
        'plugin': (supplied.get('plugin'), configured.get('plugin'), os.environ.get('XPORT_PSX_PLUGIN')),
    }
    checks = {
        'ida': lambda path: path.is_file(),
        'ghidra': lambda path: (path/'support/analyzeHeadless.bat').is_file(),
        'plugin': lambda path: (path/'data/psyq').is_dir(),
    }
    found, rejected = {}, {}
    for name, values in candidates.items():
        for value in values:
            if not value:
                continue
            path = Path(value).expanduser().resolve()
            if checks[name](path):
                found[name] = str(path)
                break
            rejected[name] = str(path)
    return found, rejected


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
            subprocess.run(command, check=True, cwd=root)
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
                    if raw != raw_item[address-item['address']:address-item['address']+4]:
                        raise ValueError('IDA bytes differ from original image')
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
                   str(run/(image+'.json')), str(Path(plugin)/'data/psyq'), format(cfg['gp'], 'X')]
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
        image = report['image']
        cfg = read_json(project_path(root, config, 'ida_configs', 'tools/ida')/(image+'.json'))
        load = cfg['loads'][-1]
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
        if any(key not in ('paths', 'analysis', 'capabilities') for key in patch):
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
            atomic_json(path, value)
        atomic_json(config_path, config)
        state.setdefault('decisions', {}).update({'tools': decision.get('tools', state.get('decisions', {}).get('tools', {}))})
        next_step = 'inventory'
    elif kind == 'psyq_review':
        if decision.get('approved') is not True:
            raise ValueError('STARTUP_PSYQ_REVIEW_NOT_APPROVED')
        next_step = 'database'
    elif kind == 'frontier_translation':
        if decision.get('approved') is not True or not decision.get('changed_files'):
            raise ValueError('STARTUP_FRONTIER_REQUIRES_REVIEWED_CHANGES')
        next_step = 'verify_frontier'
    else:
        raise ValueError('STARTUP_UNSUPPORTED_ATTENTION_KIND: '+kind)
    history = state.setdefault('history', [])
    history.append({'revision': state['revision'], 'attention_sha256': attention['sha256'],
                    'response_sha256': digest_bytes(canonical(response)), 'kind': kind,
                    'accepted_utc': datetime.datetime.now(datetime.timezone.utc).isoformat()})
    return publish(root, state, status='running', step=next_step, attention=None, failure=None)


def record_step(state, name, result):
    state.setdefault('steps', {})[name] = {'status': 'passed', 'result': result,
        'completed_utc': datetime.datetime.now(datetime.timezone.utc).isoformat()}


def frontier_packet(root):
    config = project(root)
    database = project_path(root, config, 'analysis_database', 'status/analysis.sqlite')
    with sqlite3.connect(database) as db:
        root_row = db.execute("SELECT image,address FROM functions WHERE status='TODO' ORDER BY image,address LIMIT 1").fetchone()
        rows, queue, seen = [], collections.deque([root_row] if root_row else []), set()
        while queue and len(rows) < 5:
            identity = queue.popleft()
            if identity in seen:
                continue
            seen.add(identity)
            row = db.execute("SELECT image,address,name,sha256,listing,pseudocode FROM functions WHERE image=? AND address=? AND status='TODO'", identity).fetchone()
            if not row:
                continue
            rows.append(row)
            queue.extend(db.execute('''SELECT c.target_image,c.target_function
                FROM edges e JOIN edge_candidates c ON c.edge=e.id
                JOIN functions f ON f.image=c.target_image AND f.address=c.target_function
                WHERE e.source_image=? AND e.source_function=? AND e.kind='call' AND f.status='TODO'
                ORDER BY c.target_image,c.target_function''', identity).fetchall())
        dependency_closed = not any(identity not in seen for identity in queue)
    functions = [{'image': image, 'address': address, 'name': name, 'sha256': sha,
                  'listing': listing, 'pseudocode': pseudocode} for image, address, name, sha, listing, pseudocode in rows]
    return {'functions': functions, 'dependency_closed': dependency_closed,
            'scope': 'Initial dependency-closed TODO call frontier, bounded to five functions',
            'ledger': 'status/translation-ledger.json'}


def advance(root, state):
    root = Path(root).resolve()
    while state.get('status') == 'running':
        step = state['step']
        try:
            if step == 'initialize':
                result = initialize_project(root, state['name'], state['short_name'], state.get('first_port', 2400))
                os.environ['XPORT_PROJECT'] = str(root)
                record_step(state, step, {'name': result['name'], 'short_name': result['short_name']})
                state = publish(root, state, step='build')
            elif step == 'build':
                doctor = run_shared(root, 'doctor')
                build = run_shared(root, 'build_native')
                record_step(state, step, {'doctor': doctor, 'build': build})
                state = publish(root, state, step='inventory')
            elif step == 'inventory':
                found = inventory(root)
                state['inventory'] = found
                config = project(root)
                images = config.get('analysis', {}).get('images', [])
                configs = Path(root)/config.get('paths', {}).get('ida_configs', 'tools/ida')
                missing = [image for image in images if not (configs/(image+'.json')).is_file()]
                if not images or missing:
                    state = set_attention(root, state, 'image_review',
                        'Confirm original inputs and image-qualified IDA profiles',
                        ['config_patch.paths', 'config_patch.analysis.images', 'ida_configs'], found)
                    continue
                tools, rejected = discover_tools(root, state.get('decisions', {}).get('tools', {}))
                state.setdefault('decisions', {})['tools'] = tools
                absent = [name for name in ('ida', 'ghidra', 'plugin') if not tools.get(name)]
                if absent:
                    state = set_attention(root, state, 'tool_paths', 'Provide licensed analysis tool paths',
                                          ['tools.'+name for name in absent],
                                          {'inventory': found, 'rejected_paths': rejected,
                                           'environment': ['XPORT_IDA', 'XPORT_GHIDRA', 'XPORT_PSX_PLUGIN']})
                    continue
                record_step(state, step, found)
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
                record_step(state, step, result)
                state = publish(root, state, step='ida_export')
            elif step == 'ida_export':
                tools = state['decisions']['tools']
                plan = prepare_ida_export(root, tools['ida'], 'startup-ida-plan', plan=True)
                actual = prepare_ida_export(root, tools['ida'], 'startup-ida-v1')
                canonical = canonicalize_ida(root, 'status/ida/runs/startup-ida-v1', 'tools/ida')
                accepted = accept_ida(root, 'startup-ida-v1')
                record_step(state, step, {'plan': plan, 'actual': actual, 'canonical': canonical, 'accepted': accepted})
                state = publish(root, state, step='psyq')
            elif step == 'psyq':
                tools = state['decisions']['tools']
                recognition = run_ghidra(root, tools['ghidra'], tools['plugin'], 'startup-psyq-v1')
                classification = classify_psyq(root, 'status/ghidra/runs/startup-psyq-v1')
                record_step(state, step, {'recognition': recognition, 'classification': classification})
                state = set_attention(root, state, 'psyq_review', 'Review accepted and pending PsyQ classifications',
                                      ['decision.approved'], classification)
            elif step == 'database':
                build = run_shared(root, 'build_database')
                similarity = validate_similarity(root)
                progress = run_shared(root, 'render_progress')
                record_step(state, step, {'build_database': build, 'similarity': similarity, 'progress': progress})
                packet = frontier_packet(root)
                if packet['functions']:
                    state = set_attention(root, state, 'frontier_translation',
                                          'Translate the first reviewed dependency-closed WIP frontier',
                                          ['decision.approved', 'decision.changed_files'], packet)
                else:
                    state = publish(root, state, step='verify_frontier')
            elif step == 'verify_frontier':
                refresh = run_shared(root, 'code_refresh')
                config = project(root)
                progress = read_json(root/'status/progress-data.json')
                completion = {'project': config['name'], 'short_name': config.get('short_name'),
                              'project_root': str(root), 'inputs': state.get('inventory', {}),
                              'ida': state['steps'].get('ida_export', {}).get('result'),
                              'psyq': state['steps'].get('psyq', {}).get('result'),
                              'decompilation_status': progress.get('decompilation_status'),
                              'native_solution': config['paths']['native_solution'],
                              'native_executable': config['paths']['native_executable'],
                              'code_refresh': refresh,
                              'next': 'Validate trace contracts, then use record SLOT NAME when ready'}
                record_step(state, step, {'code_refresh': refresh})
                state = publish(root, state, status='complete', step='complete', completion=completion, attention=None)
            else:
                raise ValueError('Unknown startup step: '+str(step))
        except Exception as error:
            append_log(root, 'failure '+step, type(error).__name__+': '+str(error))
            state = publish(root, state, status='failed', failure={'step': step, 'type': type(error).__name__, 'message': str(error)})
    return state


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
                os.kill(owner, 0)
            except (OSError, ValueError):
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


def start(root, name, short_name, first_port=2400):
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
        return advance(root, state)


def submit(root, response):
    root = Path(root).resolve()
    with StartupLock(root):
        state = load_state(root)
        state = apply_response(root, state, read_json(response))
        return advance(root, state)


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
    args = parser.parse_args()
    try:
        if args.action == 'start':
            if not 1024 <= args.first_port <= 65533:
                parser.error('First port must be 1024..65533')
            result = compact(start(args.project, args.name, args.short_name, args.first_port))
        elif args.action == 'submit':
            path = args.response or response_path(args.project)
            result = compact(submit(args.project, path))
        else:
            result = status(args.project, args.after_revision)
    except (OSError, ValueError) as error:
        print(json.dumps({'status': 'error', 'error': str(error)}, indent=2, ensure_ascii=False))
        raise SystemExit(2) from None
    print(json.dumps(result, indent=2, ensure_ascii=False))


if __name__ == '__main__':
    main()

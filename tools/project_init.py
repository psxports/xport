"""Initialize a C/VS2022 xport project with unique runtime reservations"""
import argparse
from contextlib import contextmanager
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import uuid
from upgrade import shared_changelog
from xport_project import CONFIG_NAME


SHORT_NAME_PATTERN = re.compile(r'[A-Za-z][A-Za-z0-9_]{0,15}')
WINDOWS_RESERVED_NAMES = {'CON', 'PRN', 'AUX', 'NUL', *(f'COM{i}' for i in range(1, 10)),
                          *(f'LPT{i}' for i in range(1, 10))}
CPP_PROJECT_TYPE = 'BC8A1FFA-BEE3-4634-8014-F334798102B3'


@contextmanager
def allocation_lock(workspace):
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateMutexW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.LPCWSTR]
    kernel.CreateMutexW.restype = wintypes.HANDLE
    kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.ReleaseMutex.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    name = 'Local\\xport-ports-'+hashlib.sha256(str(workspace.resolve()).casefold().encode()).hexdigest()
    handle = kernel.CreateMutexW(None, False, name)
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    acquired = False
    try:
        result = kernel.WaitForSingleObject(handle, 30000)
        if result not in (0, 0x80):
            raise RuntimeError('Project initialization is busy; retry without changing reservations')
        acquired = True
        yield
    finally:
        if acquired:
            kernel.ReleaseMutex(handle)
        kernel.CloseHandle(handle)


def reservations(workspace):
    used = {}
    for path in sorted(Path(workspace).glob('*/'+CONFIG_NAME)):
        config = json.loads(path.read_text(encoding='utf-8-sig'))
        settings = config.get('duckstation', {})
        ports = settings.get('reserved_gdb_ports', {})
        values = list(ports.values())
        if len(set(values)) != len(values):
            raise ValueError('Duplicate runtime role ports: '+str(path))
        if 'gdb_port' in settings:
            values.append(settings['gdb_port'])
        for port in set(values):
            if type(port) is not int or not 1024 <= port <= 65535:
                raise ValueError('Invalid reserved GDB port: '+str(path))
            if port in used:
                raise ValueError('Conflicting GDB port '+str(port)+': '+str(used[port])+' and '+str(path))
            used[port] = path
    return used


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
  <PropertyGroup Label="Globals">
    <VCProjectVersion>17.0</VCProjectVersion><ProjectGuid>{{{project_guid}}}</ProjectGuid><RootNamespace>{short_name}</RootNamespace><WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion>
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.Default.props" />
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Debug|Win32'" Label="Configuration">
    <ConfigurationType>Application</ConfigurationType><UseDebugLibraries>true</UseDebugLibraries><PlatformToolset>v143</PlatformToolset><CharacterSet>MultiByte</CharacterSet>
  </PropertyGroup>
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Release|Win32'" Label="Configuration">
    <ConfigurationType>Application</ConfigurationType><UseDebugLibraries>false</UseDebugLibraries><PlatformToolset>v143</PlatformToolset><CharacterSet>MultiByte</CharacterSet><WholeProgramOptimization>true</WholeProgramOptimization>
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.props" />
  <PropertyGroup>
    <OutDir>$(ProjectDir)..\..\..\bin\</OutDir><IntDir>$(ProjectDir)..\..\..\_build\x86\$(Configuration)\</IntDir><TargetName>{short_name}</TargetName>
    <LocalDebuggerWorkingDirectory>$(OutDir)</LocalDebuggerWorkingDirectory><LocalDebuggerCommand>$(TargetPath)</LocalDebuggerCommand><DebuggerFlavor>WindowsLocalDebugger</DebuggerFlavor><LinkIncremental>false</LinkIncremental>
  </PropertyGroup>
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

Applies to this project. Resolve `[XPORT_ROOT]` from `xport-project.json` (`toolset` points to its `tools` directory) and read `[XPORT_ROOT]/tools/PIPELINE.md` before work. Shared PSX/MIPS methodology, SQL contracts, status gates and commands live there; keep game-specific facts here.

## Project facts

- Native short name: `{short_name}`; C sources; VS2022 v143 Debug/Release x86 with the Windows subsystem and shared `WinMain`; Debug is for iteration and Release is mandatory at task completion; solution `src/platform/win/{short_name}.sln`; executable `bin/{short_name}.exe`; working directory `bin`; intermediates `_build`
- Runtime layout: `bin/{short_name}.exe` is directly under `bin`; `bin/DATA` contains data extracted from the first CD data track; `bin/MUSIC` contains decimal-name IMA ADPCM WAV tracks generated by `convert_music`
- Record reviewed image revisions/hashes, load addresses, entry/GP, language evidence, wrapper references, dummy scope, runtime hooks/layouts and reserved-role changes here before relying on them
- Reusable tooling belongs in `[XPORT_ROOT]/tools`; the mandatory bidirectional PSX/PsyQ runtime and its complete public contract belong in `[XPORT_ROOT]/src`; `xport.h` is the only Xport header and game code uses the shared `uint8/sint8` through `uint64/sint64` names rather than project type copies or `<stdint.h>` names; game addresses, bindings, exceptions and evidence remain in this project
- Every native build configuration defines project-specific `WND_TITLE`, `WND_WIDTH`, `WND_HEIGHT` and `FIELD_RATE`; the shared runtime intentionally has no fallbacks
'''


def initialize(root, name, short_name, first_port=2400):
    root = Path(root).resolve()
    validate_names(name, short_name)
    platform = root/'src/platform/win'
    targets = [root/CONFIG_NAME, root/'AGENTS.md', root/'src/game_main.c',
               platform/(short_name+'.sln'), platform/(short_name+'.vcxproj'), root/'status/translation-ledger.json']
    existing = [str(path) for path in targets if path.exists()]
    if existing:
        raise ValueError('Project initialization would overwrite existing files: '+', '.join(existing))
    with allocation_lock(root.parent):
        used = reservations(root.parent)
        sockets, ports = [], {}
        try:
            candidate = first_port
            for role in ('audit', 'trace', 'user'):
                while candidate <= 65535:
                    port = candidate; candidate += 1
                    if port in used:
                        continue
                    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                    listener.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
                    try:
                        listener.bind(('127.0.0.1', port))
                    except OSError:
                        listener.close()
                        continue
                    sockets.append(listener); ports[role] = port
                    break
                else:
                    raise ValueError('No free GDB ports')
            directories = ('src/platform/win', 'bin', '_build', 'iso', 'orig/images', 'orig/ghidra',
                           'status/ida/runs', 'status/ida/accepted', 'status/ghidra', 'tools/ida/databases')
            for relative in directories:
                (root/relative).mkdir(parents=True, exist_ok=True)
            project_guid = str(uuid.uuid4()).upper()
            shared_source = os.path.relpath(Path(__file__).resolve().parent.parent/'src', platform).replace('/', '\\')
            config = {'schema': 1, 'name': name, 'short_name': short_name,
                      'toolset': str(Path(__file__).resolve().parent),
                      'paths': {'status': 'status', 'project_tools': 'tools',
                                'analysis_database': 'status/analysis.sqlite',
                                'native_executable': 'bin/'+short_name+'.exe',
                                'native_working_directory': 'bin',
                                'native_solution': 'src/platform/win/'+short_name+'.sln',
                                'native_build': '_build', 'ida_exports': 'orig/images',
                                'ida_configs': 'tools/ida',
                                'sdk_classification': 'status/ghidra/classification.json'},
                      'duckstation': {'host': '127.0.0.1', 'gdb_port': ports['user'],
                                      'reserved_gdb_ports': ports,
                                      'default_role': 'user', 'data_directory': 'tools/duckstation/data/user',
                                      'data_directories': {role: 'tools/duckstation/data/'+role for role in ports}},
                      'code_refresh': {'build': {'solution': 'src/platform/win/'+short_name+'.sln',
                                                 'configuration': 'Debug', 'platform': 'Win32',
                                                 'process_name': short_name+'.exe'},
                                       'tests': [], 'artifacts': []}}
            (platform/(short_name+'.sln')).write_text(solution_text(short_name, project_guid), encoding='utf-8', newline='')
            (platform/(short_name+'.vcxproj')).write_text(project_text(name, short_name, project_guid, shared_source), encoding='utf-8', newline='')
            xport_revision = shared_changelog()[2][0]['revision']
            main_text = f'''#include "xport.h"

// XPORT REVISION: {xport_revision}
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--name', required=True, help='Full human-readable project name')
    parser.add_argument('--short-name', required=True, help='User-chosen 1..16 character name for sln, vcxproj and exe')
    parser.add_argument('--first-port', type=int, default=2400)
    args = parser.parse_args()
    if not 1024 <= args.first_port <= 65533:
        parser.error('First port must be 1024..65533')
    print(json.dumps(initialize(args.root, args.name, args.short_name, args.first_port), indent=2))


if __name__ == '__main__':
    main()

"""Run routine post-edit formatting, indexing, build, checks and artifact refresh"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sqlite3
import subprocess
import sys
import time

from xport_project import artifact_path, load_project
from xport_process import normalized_environment


def digest(path):
    value=hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda:stream.read(1024*1024),b''):value.update(block)
    return value.hexdigest()


def msbuild_path():
    base=os.environ.get('ProgramFiles(x86)')
    if not base:raise ValueError('ProgramFiles(x86) is unavailable')
    vswhere=Path(base)/'Microsoft Visual Studio/Installer/vswhere.exe'
    installation=subprocess.check_output([str(vswhere),'-latest','-version','[17.0,18.0)','-products','*','-requires','Microsoft.VisualStudio.Component.VC.Tools.x86.x64','-property','installationPath'],text=True,env=normalized_environment()).strip()
    path=Path(installation)/'MSBuild/Current/Bin/MSBuild.exe'
    if not path.is_file():raise ValueError('VS2022 MSBuild was not found')
    return path


def process_running(name):
    if os.name!='nt' or not name:return False
    result=subprocess.run(['tasklist','/FI','IMAGENAME eq '+name,'/FO','CSV','/NH'],capture_output=True,text=True,env=normalized_environment())
    return result.returncode==0 and any(line.lower().startswith(('"'+name+'"').lower()) for line in result.stdout.splitlines())


def run_command(argv,root,log,environment=None):
    started=time.perf_counter();env=normalized_environment(updates=environment)
    result=subprocess.run(argv,cwd=root,env=env,capture_output=True,text=True,errors='replace')
    log.append('$ '+' '.join(str(x) for x in argv)+'\n'+result.stdout+result.stderr)
    if result.returncode:raise RuntimeError('Command failed with exit code '+str(result.returncode)+': '+str(argv[0]))
    return dict(seconds=round(time.perf_counter()-started,3),exit_code=result.returncode)


def write_json(path,value):
    path.parent.mkdir(parents=True,exist_ok=True);temporary=path.with_suffix(path.suffix+'.tmp')
    temporary.write_text(json.dumps(value,indent=2)+'\n',encoding='utf-8');temporary.replace(path)


def refresh_artifacts(root,items):
    from source_context import compact
    from trace_audit_packet import audit
    result=[]
    for item in items:
        started=time.perf_counter();kind=item['kind'];output=artifact_path(item['output'])
        if kind=='source_context':
            value=compact(item['image'],int(str(item['address']),0),item.get('budget',12000),item.get('full_source',False),
                int(str(item['mips_address']),0) if item.get('mips_address') is not None else None,item.get('source_line'),item.get('pseudo_line'))
        elif kind=='trace_audit_packet':
            value=audit(int(str(item['site']),0),item.get('image'),item.get('state'),item.get('radius',32),item.get('pseudo_line'))
        else:raise ValueError('Unknown code_refresh artifact kind: '+kind)
        write_json(output,value)
        result.append(dict(kind=kind,path=output.relative_to(root).as_posix(),sha256=digest(output),seconds=round(time.perf_counter()-started,3)))
    return result


def database_check(path):
    with sqlite3.connect(path.as_uri()+'?mode=ro',uri=True) as db:
        return dict(integrity=db.execute('PRAGMA integrity_check').fetchone()[0],foreign_key_errors=len(db.execute('PRAGMA foreign_key_check').fetchall()))


def source_hygiene(root):
    from source_index import functions
    suppressions=[]
    for path in (root/'src').rglob('*.c'):
        text=path.read_text(encoding='utf-8-sig')
        for function in functions(text,path.relative_to(root).as_posix()):
            brace=text.find('{',function['start'],function['end']);signature=text[function['start']:brace]
            for match in re.finditer(r'(?m)^\s*\(void\)([A-Za-z_]\w*);\s*$',text[brace:function['end']]):
                name=match.group(1)
                if re.search(r'\b'+re.escape(name)+r'\b',signature):
                    offset=brace+match.start();suppressions.append(dict(path=path.relative_to(root).as_posix(),line=text.count('\n',0,offset)+1,function=function['name'],parameter=name))
    return dict(void_parameter_suppressions=suppressions)


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--no-build',action='store_true');parser.add_argument('--no-tests',action='store_true');args=parser.parse_args()
    root,project=load_project();settings=project.get('code_refresh',{});started=time.perf_counter();steps={};logs=[];failure=None
    report_path=artifact_path(settings.get('report','status/code-refresh-latest.json'));log_path=artifact_path(settings.get('log','status/code-refresh-latest.log'))
    try:
        from source_index import rebuild
        at=time.perf_counter();steps['source_index']=rebuild();steps['source_index']['seconds']=round(time.perf_counter()-at,3)
        steps['source_hygiene']=source_hygiene(root)
        if steps['source_hygiene']['void_parameter_suppressions']:raise RuntimeError('Standalone (void)param suppressions are forbidden; configure the compiler unused-parameter warning')
        build=settings.get('build')
        if build and not args.no_build:
            process=build.get('process_name')
            if process_running(process):raise RuntimeError(process+' is running; close the manual game before rebuilding')
            solution=(root/build.get('solution',project['paths']['native_solution'])).resolve()
            command=[str(msbuild_path()),str(solution),'/m','/t:Build','/p:Configuration='+build.get('configuration','Debug'),'/p:Platform='+build.get('platform','Win32'),'/nologo','/verbosity:minimal']
            steps['build']=run_command(command,root,logs)
            executable=(root/project['paths']['native_executable']).resolve()
            steps['build'].update(executable=executable.relative_to(root).as_posix(),sha256=digest(executable))
        if not args.no_tests:
            tests=[]
            for test in settings.get('tests',[]):
                argv=[sys.executable if value=='{python}' else value for value in test['argv']]
                outcome=run_command(argv,root,logs);outcome['name']=test['name'];tests.append(outcome)
            steps['tests']=tests
        steps['artifacts']=refresh_artifacts(root,settings.get('artifacts',[]))
        progress=[sys.executable,str(Path(__file__).resolve().parent/'render_progress.py')]
        steps['progress']=run_command(progress,root,logs,{'XPORT_PROJECT':str(root)})
        database=(root/project['paths']['analysis_database']).resolve();steps['database']=database_check(database)
        if steps['database']['integrity']!='ok' or steps['database']['foreign_key_errors']:raise RuntimeError('SQLite validation failed')
    except Exception as error:
        failure=type(error).__name__+': '+str(error)
    report=dict(version=1,project=project['name'],result='FAIL' if failure else 'PASS',seconds=round(time.perf_counter()-started,3),steps=steps,error=failure)
    write_json(report_path,report);log_path.parent.mkdir(parents=True,exist_ok=True);log_path.write_text('\n'.join(logs),encoding='utf-8')
    print(json.dumps(dict(result=report['result'],seconds=report['seconds'],report=str(report_path),log=str(log_path),error=failure)))
    if failure:raise SystemExit(1)


if __name__=='__main__':main()

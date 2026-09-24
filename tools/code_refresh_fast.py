"""Build the native executable for a provisional convergence replay"""
import json
import time

from code_refresh import digest, msbuild_path, process_running, run_command, write_json
from xport_project import artifact_path, load_project


def main():
    root, project = load_project()
    settings = project.get('code_refresh', {})
    report_path = artifact_path('status/code-refresh-fast-latest.json')
    log_path = artifact_path('status/code-refresh-fast-latest.log')
    started = time.perf_counter()
    logs = []
    steps = {}
    failure = None
    try:
        from source_index import rebuild
        at = time.perf_counter()
        steps['source_index'] = rebuild(format_changed=False)
        steps['source_index']['seconds'] = round(time.perf_counter() - at, 3)
        build = settings.get('build')
        if not build:
            raise ValueError('code_refresh.build is required for provisional replay')
        process = build.get('process_name')
        if process_running(process):
            raise RuntimeError(process + ' is running; close the manual game before rebuilding')
        solution = (root / build.get('solution', project['paths']['native_solution'])).resolve()
        command = [str(msbuild_path()), str(solution), '/m', '/t:Build',
                   '/p:Configuration=' + build.get('configuration', 'Debug'),
                   '/p:Platform=' + build.get('platform', 'Win32'), '/nologo', '/verbosity:minimal']
        steps['build'] = run_command(command, root, logs)
        executable = (root / project['paths']['native_executable']).resolve()
        steps['build'].update(executable=executable.relative_to(root).as_posix(), sha256=digest(executable))
    except Exception as error:
        failure = type(error).__name__ + ': ' + str(error)
    report = dict(version=1, project=project['name'], result='FAIL' if failure else 'PASS',
                  scope='provisional_build_and_source_index', seconds=round(time.perf_counter() - started, 3),
                  steps=steps, error=failure,
                  acceptance='Full code_refresh, build_database and a fresh replay are required before MATCH')
    write_json(report_path, report)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text('\n'.join(logs), encoding='utf-8')
    print(json.dumps(dict(result=report['result'], seconds=report['seconds'], report=str(report_path),
                          log=str(log_path), error=failure)))
    if failure:
        raise SystemExit(1)


if __name__ == '__main__':
    main()

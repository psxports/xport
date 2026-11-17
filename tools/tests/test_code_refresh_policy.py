"""Verify integration never turns a routine refresh into a Release package"""
import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import code_refresh


class IntegrationPolicyTests(unittest.TestCase):
    def exercise(self, configuration, arguments=()):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        (root / 'bin').mkdir()
        executable = root / 'bin/game.exe'
        executable.write_bytes(b'fixture')
        project = {'name': 'Fixture', 'paths': {'analysis_database': 'status/analysis.sqlite',
                    'native_solution': 'src/platform/win/game.sln'},
                   'code_refresh': {'build': {'configuration': configuration}}}
        with contextlib.ExitStack() as stack:
            stack.enter_context(patch('sys.argv', ['code_refresh', *arguments]))
            stack.enter_context(patch.object(code_refresh, 'load_project', return_value=(root, project)))
            stack.enter_context(patch.object(code_refresh, 'artifact_path', side_effect=lambda p: root / p))
            index = stack.enter_context(patch('source_index.rebuild', return_value={}))
            stack.enter_context(patch.object(code_refresh, 'source_hygiene', return_value={'void_parameter_suppressions': []}))
            stack.enter_context(patch.object(code_refresh, 'process_running', return_value=False))
            stack.enter_context(patch.object(code_refresh, 'native_executable_path', return_value=executable))
            stack.enter_context(patch.object(code_refresh, 'msbuild_path', return_value=Path('msbuild')))
            command = stack.enter_context(patch.object(code_refresh, 'run_command', return_value={'exit_code': 0}))
            stack.enter_context(patch.object(code_refresh, 'refresh_artifacts', return_value=[]))
            stack.enter_context(patch.object(code_refresh, 'database_check', return_value={'integrity': 'ok', 'foreign_key_errors': 0}))
            stack.enter_context(patch('project_context.queue', return_value={'counts': {}}))
            stack.enter_context(patch('project_context.put'))
            stage = stack.enter_context(patch('xport_project.stage_sources', return_value={'staged_files': 1}))
            with contextlib.redirect_stdout(io.StringIO()):
                try:
                    code_refresh.main()
                except SystemExit as error:
                    self.assertEqual(error.code, 1)
            report = json.loads((root / 'status/code-refresh-latest.json').read_text())
        return report, index, command, stage

    def test_release_config_fails_before_formatting_or_execution(self):
        report, index, command, stage = self.exercise('Release')
        self.assertEqual(report['result'], 'FAIL')
        index.assert_not_called()
        command.assert_not_called()
        stage.assert_not_called()

    def test_debug_refresh_stages_sources_after_checks(self):
        report, index, command, stage = self.exercise('Debug')
        self.assertEqual(report['result'], 'PASS')
        self.assertIn('/p:Configuration=Debug', command.call_args.args[0])
        stage.assert_called_once()
        self.assertEqual(report['steps']['git_index']['staged_files'], 1)

    def test_explicit_no_build_does_not_run_release(self):
        report, index, command, stage = self.exercise('Release', ['--no-build'])
        self.assertEqual(report['result'], 'PASS')
        command.assert_not_called()


@unittest.skipUnless(code_refresh.os.name == 'nt', 'Requires Windows OEM codec')
class ProcessGuardTests(unittest.TestCase):
    def result(self, raw, code=0):
        from types import SimpleNamespace
        def run(*args, **kwargs):
            self.assertEqual(kwargs['encoding'], 'oem')
            self.assertEqual(args[0][:1], ['tasklist'])
            # Native OEM output must not depend on Python UTF8 mode
            return SimpleNamespace(returncode=code, stdout=raw.decode(kwargs['encoding']))
        return run

    def test_localized_no_process_and_case_insensitive_csv_match(self):
        localized='Информация: задачи не запущены'.encode('oem')
        with patch.object(code_refresh.os, 'name', 'nt'), patch.object(code_refresh.subprocess, 'run', side_effect=self.result(localized)):
            self.assertFalse(code_refresh.process_running('TD5_debug.exe'))
        with patch.object(code_refresh.os, 'name', 'nt'), patch.object(code_refresh.subprocess, 'run', side_effect=self.result(b'"td5_DEBUG.exe","1234","Console"')):
            self.assertTrue(code_refresh.process_running('TD5_debug.exe'))
        with patch.object(code_refresh.os, 'name', 'nt'), patch.object(code_refresh.subprocess, 'run', side_effect=self.result(b'"other.exe","1234","Console"')):
            self.assertFalse(code_refresh.process_running('TD5_debug.exe'))

    def test_query_error_stops_build(self):
        from types import SimpleNamespace
        results=[SimpleNamespace(returncode=1),SimpleNamespace(returncode=2)]
        with patch.object(code_refresh.os, 'name', 'nt'), patch.object(code_refresh.subprocess, 'run', side_effect=results):
            with self.assertRaisesRegex(RuntimeError, 'Cannot inspect'):
                code_refresh.process_running('TD5_debug.exe')

    def test_tasklist_failure_uses_literal_powershell_name(self):
        from types import SimpleNamespace
        for status, expected in ((0,False),(1,True)):
            results=[SimpleNamespace(returncode=1),SimpleNamespace(returncode=status)]
            with patch.object(code_refresh.os,'name','nt'), patch.object(code_refresh.subprocess,'run',side_effect=results) as run:
                self.assertEqual(code_refresh.process_running("game'one.exe"),expected)
                arguments=run.call_args_list[1].args[0]
                self.assertEqual(arguments[:4],['powershell','-NoProfile','-NonInteractive','-Command'])
                self.assertIn("-contains 'game''one'",arguments[4])
                self.assertTrue(run.call_args_list[1].kwargs['creationflags'])

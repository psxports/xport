import json
from pathlib import Path
import contextlib
import tempfile
import unittest
from unittest import mock
import sqlite3
import sys

import startup
import duckstation_deploy
import trace_workflow


class StartupSupervisorTests(unittest.TestCase):
    def test_display_name_allows_colon(self):
        startup.validate_names('Mortal Kombat Mythologies: Sub-Zero', 'MKM')

    def test_generated_readme_uses_supplied_names_and_template(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Unrelated-directory'
            startup.initialize_project(root, 'RapidRacer', 'RR', 45400)
            text = (root/'README.md').read_text(encoding='utf-8')
            self.assertEqual(text, startup.render_project_readme('RapidRacer', 'RR'))
            self.assertIn('>RapidRacer-xport</h1>', text)
            self.assertIn('https://psxports.to/RR/', text)
            for label in ('decomp', 'refactor', 'enhance', 'remaster'):
                self.assertIn('repo=RapidRacer-xport&label='+label, text)
            self.assertNotIn('@@', text)

    def test_readme_escapes_display_names_and_encodes_repository(self):
        text = startup.render_project_readme('Game & <Two>', 'G2')
        self.assertIn('>Game&amp;&lt;Two&gt;-xport</h1>', text)
        self.assertIn('repo=Game%26%3CTwo%3E-xport', text)
        self.assertIn('Game &amp; &lt;Two&gt; (PSX)', text)
        self.assertIn('https://psxports.to/G2/', text)

    def test_readme_removes_spaces_only_from_project_identity(self):
        text = startup.render_project_readme('Pandemonium 2', 'P2')
        self.assertIn('>Pandemonium2-xport</h1>', text)
        self.assertIn('Pandemonium 2 (PSX)', text)
        self.assertIn('repo=Pandemonium2-xport&label=decomp', text)
        self.assertIn('https://github.com/psxports/Pandemonium2-xport/issues?', text)
        self.assertIn('<img src="cover.png" alt="cover">', text)
        self.assertNotIn('width=', text)
        self.assertNotIn('height=', text)

    def test_initializer_preserves_existing_readme_without_scaffold(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            readme = root/'README.md'
            readme.write_bytes(b'User README\n')
            with self.assertRaisesRegex(ValueError, 'STARTUP_ROOT_NOT_EMPTY'):
                startup.initialize_project(root, 'Example', 'EX', 45410)
            self.assertEqual(readme.read_bytes(), b'User README\n')
            self.assertEqual(sorted(path.name for path in root.iterdir()), ['README.md'])

    def test_initializer_rejects_missing_or_invalid_readme_template_before_writes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Project'
            template = Path(directory)/'template.in'
            with mock.patch.object(startup, 'PROJECT_README_TEMPLATE', template):
                with self.assertRaisesRegex(ValueError, 'Missing project README template'):
                    startup.initialize_project(root, 'Example', 'EX', 45420)
                template.write_text('@@UNKNOWN@@', encoding='utf-8')
                with self.assertRaisesRegex(ValueError, 'Invalid project README template tokens'):
                    startup.initialize_project(root, 'Example', 'EX', 45420)
            self.assertFalse(root.exists())

    def test_generated_project_embeds_local_default_icon(self):
        import xml.etree.ElementTree as ET
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'IconProject'
            startup.initialize_project(root, 'Icon Game', 'IC', 45430)
            platform = root/'src/platform/win'
            tree = ET.parse(platform/'IC.vcxproj')
            ns = {'p': 'http://schemas.microsoft.com/developer/msbuild/2003'}
            resources = tree.findall('.//p:ResourceCompile', ns)
            self.assertEqual(len(resources), 1)
            resource = resources[0]
            path = (platform/resource.attrib['Include'].replace('\\', '/')).resolve()
            self.assertEqual(path, platform/'icon.rc')
            self.assertEqual(resource.find('p:AdditionalIncludeDirectories', ns).text,
                             '$(ProjectDir);%(AdditionalIncludeDirectories)')
            self.assertIsNone(resource.find('p:ExcludedFromBuild', ns))
            icon = tree.find('.//p:Image', ns)
            self.assertEqual((platform/icon.attrib['Include'].replace('\\', '/')).resolve(),
                             platform/'icon.ico')
            for filename in ('icon.ico', 'icon.rc'):
                self.assertEqual((platform/filename).read_bytes(),
                                 (startup.NATIVE_TEMPLATE_DIR/filename).read_bytes())

    def test_initializer_preserves_existing_icon_files(self):
        for filename in ('icon.ico', 'icon.rc'):
            with self.subTest(filename=filename), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)/'Project'
                platform = root/'src/platform/win'
                platform.mkdir(parents=True)
                existing = platform/filename
                existing.write_bytes(b'User icon')
                with self.assertRaisesRegex(ValueError, 'STARTUP_ROOT_NOT_EMPTY'):
                    startup.initialize_project(root, 'Icon Game', 'IC', 45450)
                self.assertEqual(existing.read_bytes(), b'User icon')
                self.assertFalse((root/'xport-project.json').exists())
                self.assertFalse((root/'README.md').exists())

    def test_initializer_rejects_missing_default_icon_before_writes(self):
        with tempfile.TemporaryDirectory() as directory:
            native = Path(directory)/'templates'
            native.mkdir()
            for missing in ('icon.rc', 'icon.ico'):
                for path in native.iterdir():
                    path.unlink()
                other = 'icon.ico' if missing == 'icon.rc' else 'icon.rc'
                (native/other).touch()
                root = Path(directory)/'Project'
                with mock.patch.object(startup, 'NATIVE_TEMPLATE_DIR', native):
                    with self.assertRaisesRegex(ValueError, 'Missing default Windows icon resource'):
                        startup.initialize_project(root, 'Icon Game', 'IC', 45440)
                self.assertFalse(root.exists())

    def test_generated_gitignore_matches_shared_template(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Ignored-xport'
            startup.initialize_project(root, 'Ignored Game', 'IG', 45150)
            self.assertEqual((root/'.gitignore').read_bytes(), startup.PROJECT_GITIGNORE_TEMPLATE.read_bytes())
            self.assertEqual((root/'.gitignore').read_text(encoding='utf-8').splitlines(), [
                '/_build/', '/bin/', '/status/', '/tools/', '/orig/', '/iso/', '/.cursor/',
            ])

    def test_git_index_stages_only_template_gitignore(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Project'
            startup.initialize_project(root, 'Example Game', 'EX', 45560)
            result = startup.stage_project_gitignore(root)
            self.assertTrue(result['repository_created'])
            staged = startup.subprocess.run(['git', '-C', str(root), 'diff', '--cached', '--name-only'],
                                            capture_output=True, text=True, check=True)
            self.assertEqual(staged.stdout.splitlines(), ['.gitignore'])
            indexed = startup.subprocess.run(['git', '-C', str(root), 'show', ':.gitignore'],
                                             capture_output=True, check=True)
            self.assertEqual(indexed.stdout, startup.PROJECT_GITIGNORE_TEMPLATE.read_bytes())

    def test_git_index_preserves_existing_staged_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Project'
            root.mkdir()
            startup.subprocess.run(['git', '-C', str(root), 'init', '--quiet'], check=True)
            (root/'user.txt').write_text('Keep staged content', encoding='utf-8')
            startup.subprocess.run(['git', '-C', str(root), 'add', '--', 'user.txt'], check=True)
            startup.initialize_project(root, 'Example Game', 'EX', 45570)
            result = startup.stage_project_gitignore(root)
            self.assertFalse(result['repository_created'])
            staged = startup.subprocess.run(['git', '-C', str(root), 'diff', '--cached', '--name-only'],
                                            capture_output=True, text=True, check=True)
            self.assertEqual(staged.stdout.splitlines(), ['.gitignore', 'user.txt'])

    def test_git_index_failure_retries_without_reinitializing(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Project'
            with mock.patch.object(startup, 'stage_project_gitignore', side_effect=ValueError('Index locked')):
                state = startup.start(root, 'Example Game', 'EX', 45580)
            self.assertEqual(state['status'], 'failed')
            self.assertEqual(state['failure']['step'], 'git_index')
            readme = (root/'README.md').read_bytes()
            with mock.patch.object(startup, 'run_shared', return_value='ok'), \
                    mock.patch.object(startup.startup_github, 'sync_project', return_value={'issues': []}):
                state = startup.retry(root, state['revision'])
            self.assertEqual(state['status'], 'attention')
            self.assertEqual(state['steps']['git_index']['status'], 'passed')
            self.assertEqual((root/'README.md').read_bytes(), readme)

    def test_initializer_refuses_existing_gitignore(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'ExistingIgnore-xport'
            root.mkdir()
            (root/'.gitignore').write_text('/custom/\n', encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'STARTUP_ROOT_NOT_EMPTY'):
                startup.initialize_project(root, 'Existing Ignore', 'EI', 45160)

    def test_generated_pdb_files_use_intermediate_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Symbols-xport'
            startup.initialize_project(root, 'Symbols Game', 'SY', 45200)
            project = (root/'src/platform/win/SY.vcxproj').read_text(encoding='utf-8')
            self.assertIn('<ProgramDataBaseFileName>$(IntDir)SY-compile.pdb</ProgramDataBaseFileName>', project)
            self.assertIn('<ProgramDatabaseFile>$(IntDir)SY_debug.pdb</ProgramDatabaseFile>', project)
            self.assertNotIn('$(OutDir)SY_debug.pdb', project)

    def start_to_image_review(self, root):
        with mock.patch.object(startup, 'run_shared', return_value='ok'), \
                mock.patch.object(startup.startup_github, 'sync_project', return_value={'repository': 'psxports/ExampleGame-xport'}):
            return startup.start(root, 'Example Game', 'EX', 45000)

    def test_start_runs_deterministic_steps_until_attention(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Example-xport'
            state = self.start_to_image_review(root)
            self.assertEqual(state['status'], 'attention')
            self.assertEqual(state['attention']['kind'], 'image_review')
            self.assertEqual(state['steps']['initialize']['status'], 'passed')
            self.assertNotIn('build', state['steps'])
            self.assertEqual(state['steps']['diagnose']['status'], 'passed')
            self.assertNotIn('emulator', state['steps'])
            self.assertFalse((root/'DuckStation.bat').exists())
            self.assertTrue((root/'status/startup.json').is_file())
            response = json.loads((root/'status/startup-response.json').read_text(encoding='utf-8'))
            self.assertEqual(response['revision'], state['revision'])
            self.assertEqual(response['attention_sha256'], state['attention']['sha256'])
            self.assertFalse((root/'status/startup.lock').exists())

    def test_start_rejects_initialized_or_started_project(self):
        with tempfile.TemporaryDirectory() as directory:
            initialized = Path(directory)/'Initialized-xport'
            startup.initialize_project(initialized, 'Initialized Game', 'IG', 45100)
            with self.assertRaisesRegex(ValueError, 'STARTUP_ALREADY_INITIALIZED'):
                startup.start(initialized, 'Bad: Input', 'INVALID-NAME', 45110)
            started = Path(directory)/'Started-xport'
            self.start_to_image_review(started)
            with self.assertRaisesRegex(ValueError, 'STARTUP_ALREADY_INITIALIZED'):
                startup.start(started, 'Example Game', 'EX', 45120)

    def test_status_does_not_increment_unchanged_revision(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Example-xport'
            state = self.start_to_image_review(root)
            result = startup.status(root, state['revision'])
            self.assertFalse(result['changed'])
            self.assertEqual(result['revision'], state['revision'])

    def test_response_is_bound_to_attention_revision_and_hash(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Example-xport'
            state = self.start_to_image_review(root)
            response = root/'status/startup-response.json'
            response.write_text(json.dumps({'revision': state['revision']-1,
                                             'attention_sha256': state['attention']['sha256'],
                                             'decision': {}}), encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'STARTUP_STALE_RESPONSE'):
                startup.submit(root, response)

    def test_response_rejects_foreign_config_scope(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Example-xport'
            state = self.start_to_image_review(root)
            response = {'revision': state['revision'], 'attention_sha256': state['attention']['sha256'],
                        'decision': {'config_patch': {'duckstation': {'gdb_port': 1}}}}
            with self.assertRaisesRegex(ValueError, 'STARTUP_RESPONSE_CONFIG_SCOPE'):
                startup.apply_response(root, state, response)

    def test_frontier_response_requires_hash_bound_complete_function_audit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Example-xport'
            (root/'status').mkdir(parents=True)
            function = {
                'image': 'MAIN.EXE', 'address': 0x80010000,
                'audit_context': {
                    'mips': {'complete': True, 'sha256': '1' * 64},
                    'pseudocode': {'complete': True, 'sha256': '2' * 64},
                },
            }
            state = {
                'schema': 1, 'revision': 7, 'status': 'attention', 'step': 'database',
                'history': [], 'attention': {
                    'kind': 'frontier_translation', 'sha256': '3' * 64,
                    'context': {'functions': [function]},
                },
            }
            incomplete = {
                'revision': 7, 'attention_sha256': '3' * 64,
                'decision': {'approved': True, 'changed_files': ['src/main.c']},
            }
            with self.assertRaisesRegex(ValueError, 'COMPLETE_FUNCTION_AUDITS'):
                startup.apply_response(root, state, incomplete)
            complete = json.loads(json.dumps(incomplete))
            complete['decision']['function_audits'] = [{
                'image': 'MAIN.EXE', 'address': 0x80010000,
                'mips_sha256': '1' * 64, 'pseudocode_sha256': '2' * 64,
                'pseudocode_used_as_baseline': True, 'complete_mips_audit': True,
                'unresolved_semantics': [],
            }]
            accepted = startup.apply_response(root, state, complete)
            self.assertEqual(accepted['step'], 'verify_frontier')

    def test_frontier_response_accepts_missing_pseudocode_with_false_baseline(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Example-xport'
            (root/'status').mkdir(parents=True)
            state = {
                'schema': 1, 'revision': 7, 'status': 'attention', 'step': 'database',
                'history': [], 'attention': {
                    'kind': 'frontier_translation', 'sha256': '3' * 64,
                    'context': {'functions': [{
                        'image': 'MAIN.EXE', 'address': 0x80010000,
                        'audit_context': {
                            'mips': {'complete': True, 'sha256': '1' * 64},
                            'pseudocode': {'complete': False, 'sha256': None},
                        },
                    }]},
                },
            }
            response = {
                'revision': 7, 'attention_sha256': '3' * 64,
                'decision': {
                    'approved': True, 'changed_files': ['src/main.c'],
                    'function_audits': [{
                        'image': 'MAIN.EXE', 'address': 0x80010000,
                        'mips_sha256': '1' * 64, 'pseudocode_sha256': None,
                        'pseudocode_used_as_baseline': False,
                        'complete_mips_audit': True, 'unresolved_semantics': [],
                    }],
                },
            }
            accepted = startup.apply_response(root, state, response)
            self.assertEqual(accepted['step'], 'verify_frontier')

    def test_duplicate_worker_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Example-xport'
            with startup.StartupLock(root):
                with self.assertRaisesRegex(ValueError, 'STARTUP_WORKER_ALREADY_RUNNING'):
                    with startup.StartupLock(root):
                        pass

    def test_emulator_deployment_resume_requires_matching_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Example-xport';target = root/'tools/duckstation/data/audit'
            source = Path(directory)/'distribution';source.mkdir(parents=True)
            target.mkdir(parents=True)
            (target/'settings.ini').write_text('[Debug]\nEnableGDBServer=true\nGDBServerPort=45000\n')
            for name in ('bios', 'savestates', 'memcards', 'screenshots', 'cache'):
                (target/name).mkdir()
            report = {'project': 'Example Game', 'role': 'audit', 'port': 45000,
                      'runtime': str(target), 'distribution': str(source.resolve()),
                      'settings_sha256': 'settings'}
            startup.atomic_json(target/'xport-deployment.json', report)
            with mock.patch.object(duckstation_deploy, 'validate_settings'), \
                 mock.patch.object(duckstation_deploy, 'digest', return_value='settings'):
                reused = duckstation_deploy.existing_deployment(root, {'name': 'Example Game'},
                                                                 'audit', 45000, source, target)
            self.assertTrue(reused['reused'])
            report['port'] = 45001
            startup.atomic_json(target/'xport-deployment.json', report)
            with self.assertRaisesRegex(ValueError, 'identity differs'):
                duckstation_deploy.existing_deployment(root, {'name': 'Example Game'},
                                                       'audit', 45000, source, target)

    def test_emulator_default_role_accepts_valid_mutable_settings(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Example-xport';target = root/'tools/duckstation/data/user'
            source = Path(directory)/'distribution';source.mkdir(parents=True)
            target.mkdir(parents=True)
            (target/'settings.ini').write_text('[Debug]\nEnableGDBServer=true\nGDBServerPort=45002\n')
            for name in ('bios', 'savestates', 'memcards', 'screenshots', 'cache'):
                (target/name).mkdir()
            report = {'project': 'Example Game', 'role': 'user', 'port': 45002,
                      'runtime': str(target), 'distribution': str(source.resolve()),
                      'settings_sha256': 'deployed-settings'}
            startup.atomic_json(target/'xport-deployment.json', report)
            config = {'name': 'Example Game', 'duckstation': {'default_role': 'user'}}
            with mock.patch.object(duckstation_deploy, 'validate_settings'), \
                 mock.patch.object(duckstation_deploy, 'digest', return_value='current-settings'):
                reused = duckstation_deploy.existing_deployment(root, config, 'user', 45002,
                                                                 source, target)
            self.assertTrue(reused['reused'])
            self.assertTrue(reused['settings_changed_since_deployment'])
            self.assertEqual(reused['settings_sha256_current'], 'current-settings')

    def test_emulator_user_defaults_include_keyboard_controls_and_hotkeys(self):
        user = duckstation_deploy.settings_for_role('user', 'user', 45002)
        self.assertEqual(user['Main']['Language'], 'en')
        self.assertEqual(user['Main']['FastForwardSpeed'], '0')
        self.assertEqual(user['Debug']['GDBServerPort'], '45002')
        self.assertEqual(user['Pad1']['Type'], 'AnalogController')
        self.assertEqual([user['Pad1'][key] for key in ('Up', 'Right', 'Down', 'Left')],
                         ['Keyboard/UpArrow', 'Keyboard/RightArrow', 'Keyboard/DownArrow', 'Keyboard/LeftArrow'])
        self.assertEqual([user['Pad1'][key] for key in ('LUp', 'LRight', 'LDown', 'LLeft')],
                         ['Keyboard/W', 'Keyboard/D', 'Keyboard/S', 'Keyboard/A'])
        self.assertEqual(user['Audio']['OutputMuted'], 'false')
        self.assertEqual(Path(user['Folders']['BIOS']),
                         Path(r'C:\Projects\xports'))
        self.assertTrue(all(user['BIOS'][key] == 'scph5502.bin'
                            for key in ('PathNTSCJ', 'PathNTSCU', 'PathPAL')))
        self.assertEqual(user['Hotkeys'], {'FastForward': 'Keyboard/Tab',
                                           'TogglePause': 'Keyboard/Space'})
        audit = duckstation_deploy.settings_for_role('audit', 'user', 45000)
        self.assertEqual(audit['Main']['Language'], 'en')
        self.assertNotIn('FastForwardSpeed', audit['Main'])
        self.assertNotIn('Pad1', audit)
        self.assertNotIn('Hotkeys', audit)
        self.assertEqual(audit['Audio']['OutputMuted'], 'true')
        self.assertEqual(audit['Folders']['BIOS'], user['Folders']['BIOS'])

    def test_emulator_bios_paths_resolve_against_the_selected_template(self):
        source = Path(duckstation_deploy.__file__).parent / 'duckstation/settings.ini'
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            template = folder / 'settings.ini'
            original = source.read_text(encoding='utf-8').replace(r'BIOS = C:\Projects\xports', 'BIOS = bios')
            template.write_text(original, encoding='utf-8')
            user = duckstation_deploy.settings_for_role('user', 'user', 45002, template)
            self.assertEqual(Path(user['Folders']['BIOS']), folder / 'bios')
            absolute = folder / 'custom-bios'
            template.write_text(original.replace('BIOS = bios', 'BIOS = ' + str(absolute)), encoding='utf-8')
            audit = duckstation_deploy.settings_for_role('audit', 'user', 45000, template)
            self.assertEqual(Path(audit['Folders']['BIOS']), absolute)
            self.assertEqual(audit['Debug']['GDBServerPort'], '45000')
            self.assertEqual(audit['Audio']['OutputMuted'], 'true')

    def test_emulator_preparation_is_the_final_startup_step(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Example-xport'
            startup.initialize_project(root, 'Example Game', 'EX', 45300)
            startup.atomic_json(root/'status/progress-data.json', {'decompilation_status': {'WIP': 1}})
            state = {'schema': 1, 'revision': 9, 'status': 'running', 'step': 'emulator',
                     'project_root': str(root.resolve()), 'name': 'Example Game', 'short_name': 'EX',
                     'created_utc': '2026-09-25T00:00:00+00:00', 'steps': {
                         'verify_frontier': {'status': 'passed', 'result': {'source_index': 'ok'}, 'seconds': 1},
                         'ida_export': {'result': {'canonical': []}},
                         'psyq': {'result': {'classification': {}}}},
                     'inventory': {}, 'recoveries': [], 'attention': None, 'failure': None}
            capture = {'schema': 1, 'original_image': 'MAIN.EXE'}
            with mock.patch.object(startup, 'validate_capture_contract', return_value=capture), \
                 mock.patch.object(startup, 'validate_convergence_contract', return_value={'native_abi': 'Example ABI 1'}), \
                 mock.patch.object(startup, 'run_shared', return_value='deployed') as run_shared:
                completed = startup.advance(root, state)
            self.assertEqual(completed['status'], 'complete')
            self.assertTrue((root/'DuckStation.bat').is_file())
            self.assertTrue((root/'record.bat').is_file())
            self.assertEqual([call.args[2][1] for call in run_shared.call_args_list], ['user', 'lockstep', 'audit', 'trace'])
            self.assertEqual(set(completed['completion']['duckstation']['deployments']), {'user', 'lockstep', 'audit', 'trace'})

    def test_emulator_resume_refreshes_missing_frontier_index(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'LibraryEntry-xport'
            startup.initialize_project(root, 'Library Entry', 'LE', 45305)
            startup.atomic_json(root/'status/progress-data.json', {'decompilation_status': {'SKIP': 1}})
            state = {'schema': 1, 'revision': 9, 'status': 'running', 'step': 'emulator',
                     'project_root': str(root.resolve()), 'name': 'Library Entry', 'short_name': 'LE',
                     'created_utc': '2026-09-25T00:00:00+00:00', 'steps': {},
                     'inventory': {}, 'recoveries': [], 'attention': None, 'failure': None}
            with mock.patch.object(startup, 'validate_capture_contract', return_value={'schema': 1}), \
                 mock.patch.object(startup, 'validate_convergence_contract', return_value={}), \
                 mock.patch.object(startup, 'run_shared', return_value='ok') as run_shared:
                completed = startup.advance(root, state)
            self.assertEqual(completed['status'], 'complete')
            self.assertEqual(completed['completion']['source_index'], 'ok')
            self.assertEqual(run_shared.call_args_list[0].args[1:],
                             ('source_index', '--rebuild', '--no-format'))

    def test_startup_pauses_before_emulator_until_capture_contract_is_reviewed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Example-xport'
            startup.initialize_project(root, 'Example Game', 'EX', 45310)
            state = {'schema': 1, 'revision': 9, 'status': 'running', 'step': 'trace_contract',
                     'project_root': str(root.resolve()), 'name': 'Example Game', 'short_name': 'EX',
                     'created_utc': '2026-09-25T00:00:00+00:00', 'steps': {}, 'inventory': {},
                     'recoveries': [], 'attention': None, 'failure': None}
            waiting = startup.advance(root, state)
            self.assertEqual(waiting['status'], 'attention')
            self.assertEqual(waiting['attention']['kind'], 'trace_capture_review')
            self.assertFalse((root/'DuckStation.bat').exists())

    def test_startup_pauses_for_convergence_contract_after_capture_review(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Example-xport'
            startup.initialize_project(root, 'Example Game', 'EX', 45315)
            state = {'schema': 1, 'revision': 9, 'status': 'running', 'step': 'convergence_contract',
                     'project_root': str(root.resolve()), 'name': 'Example Game', 'short_name': 'EX',
                     'created_utc': '2026-09-25T00:00:00+00:00', 'steps': {}, 'inventory': {},
                     'recoveries': [], 'attention': None, 'failure': None}
            startup.atomic_json(root/'status/progress-data.json', {'decompilation_status': {'TODO': 0}})
            with mock.patch.object(startup, 'validate_capture_contract', return_value={'schema': 1}):
                waiting = startup.advance(root, state)
            self.assertEqual(waiting['status'], 'attention')
            self.assertEqual(waiting['attention']['kind'], 'convergence_review')
            self.assertTrue(waiting['attention']['context']['problem'])
            self.assertFalse((root/'DuckStation.bat').exists())

    def test_reopen_convergence_resumes_completed_legacy_startup(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Example-xport'
            startup.initialize_project(root, 'Example Game', 'EX', 45316)
            state = {'schema': 1, 'revision': 9, 'status': 'complete', 'step': 'complete',
                     'project_root': str(root.resolve()), 'name': 'Example Game', 'short_name': 'EX',
                     'created_utc': '2026-09-25T00:00:00+00:00', 'steps': {}, 'inventory': {},
                     'recoveries': [], 'attention': None, 'failure': None, 'completion': {'legacy': True}}
            startup.atomic_json(startup.state_path(root), state)
            startup.atomic_json(root/'status/progress-data.json', {'decompilation_status': {'TODO': 0}})
            waiting = startup.reopen_convergence(root)
            self.assertEqual(waiting['status'], 'attention')
            self.assertEqual(waiting['attention']['kind'], 'convergence_review')
            self.assertIsNone(waiting['completion'])

    def test_verify_frontier_advances_to_trace_contract(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Example-xport'
            startup.initialize_project(root, 'Example Game', 'EX', 45317)
            state = {'schema': 1, 'revision': 9, 'status': 'running', 'step': 'verify_frontier',
                     'project_root': str(root.resolve()), 'name': 'Example Game', 'short_name': 'EX',
                     'created_utc': '2026-09-25T00:00:00+00:00', 'steps': {}, 'inventory': {},
                     'recoveries': [], 'attention': None, 'failure': None}
            with mock.patch.object(startup, 'run_shared', return_value='ok') as commands, \
                 mock.patch.object(startup, 'capture_contract_packet', return_value={'problem': 'review required'}):
                waiting = startup.advance(root, state)
            self.assertEqual(waiting['status'], 'attention')
            self.assertEqual(waiting['attention']['kind'], 'trace_capture_review')
            result = waiting['steps']['verify_frontier']['result']
            self.assertEqual(result['source_index'], 'ok')
            self.assertEqual(result['translation_queue'], 'ok')
            self.assertEqual([call.args[1] for call in commands.call_args_list], ['source_index', 'project_context'])
            self.assertTrue(result['native_link_deferred_to_coverage'])

    def test_refresh_completion_requires_exact_completed_revision(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root/'status').mkdir()
            state = {'schema': 1, 'revision': 9, 'status': 'complete', 'step': 'complete',
                     'project_root': str(root), 'completion': {'stale': True}, 'steps': {}}
            startup.atomic_json(startup.state_path(root), state)
            with mock.patch.object(startup, 'validate_capture_contract', return_value={'schema': 1}):
                with self.assertRaisesRegex(ValueError, 'REVISION_MISMATCH'):
                    startup.refresh_completion(root, 8, advance_now=False)
                refreshed = startup.refresh_completion(root, 9, advance_now=False)
            self.assertEqual(refreshed['revision'], 10)
            self.assertEqual(refreshed['step'], 'verify_frontier')
            self.assertEqual(refreshed['status'], 'running')
            self.assertIsNone(refreshed['completion'])
            with self.assertRaisesRegex(ValueError, 'NOT_COMPLETE'):
                startup.refresh_completion(root, 10, advance_now=False)

    def test_frontier_contains_only_todo_native_entrypoint(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Example-xport'
            (root/'status').mkdir(parents=True)
            (root/'tools/ida').mkdir(parents=True)
            startup.atomic_json(root/'xport-project.json', {
                'paths': {'analysis_database': 'status/analysis.sqlite', 'ida_configs': 'tools/ida'},
                'analysis': {'default_image': 'MAIN.EXE'}})
            startup.atomic_json(root/'tools/ida/MAIN.EXE.json', {'entry': 0x80010000})
            database = root/'status/analysis.sqlite'
            with contextlib.closing(sqlite3.connect(database)) as db:
                db.execute('CREATE TABLE functions '
                           '(image TEXT,address INTEGER,name TEXT,sha256 TEXT,listing TEXT,pseudocode TEXT,status TEXT)')
                db.execute('CREATE TABLE edges '
                           '(id INTEGER,source_image TEXT,source_function INTEGER,kind TEXT)')
                db.execute('CREATE TABLE edge_candidates '
                           '(edge INTEGER,target_image TEXT,target_function INTEGER)')
                db.executemany('INSERT INTO functions VALUES (?,?,?,?,?,?,?)', [
                    ('MAIN.EXE', 0x80010000, 'entry', 'a', 'entry.lst', 'entry.c', 'TODO'),
                    ('MAIN.EXE', 0x80010100, 'reachable', 'b', 'reachable.lst', 'reachable.c', 'TODO'),
                    ('MAIN.EXE', 0x80020000, 'orphan', 'c', 'orphan.lst', 'orphan.c', 'TODO')])
                db.execute('INSERT INTO edges VALUES (1,?,?,?)', ('MAIN.EXE', 0x80010000, 'call'))
                db.execute('INSERT INTO edge_candidates VALUES (1,?,?)', ('MAIN.EXE', 0x80010100))
                db.commit()
            source_context = mock.Mock()
            source_context.compact.return_value = {
                'function': 'entry',
                'mips': {'complete': True, 'sha256': '1' * 64},
                'pseudocode': {'complete': True, 'sha256': '2' * 64},
            }
            with mock.patch.dict(sys.modules, {'source_context': source_context}), \
                 mock.patch.object(startup.importlib, 'reload') as reload_module:
                reload_module.return_value = source_context
                packet = startup.frontier_packet(root)
            self.assertEqual(packet['remaining_todo'], 3)
            self.assertIsNone(packet['reachable_functions'])
            self.assertFalse(packet['dependency_closed'])
            self.assertEqual([item['address'] for item in packet['functions']], [0x80010000])
            with contextlib.closing(sqlite3.connect(database)) as db:
                db.execute("UPDATE functions SET status='WIP' WHERE address=?", (0x80010000,))
                db.commit()
            with mock.patch.dict(sys.modules, {'source_context': source_context}), \
                 mock.patch.object(startup.importlib, 'reload', return_value=source_context):
                packet = startup.frontier_packet(root)
            self.assertEqual(packet['remaining_todo'], 2)
            self.assertEqual(packet['functions'], [])

    def test_capture_contract_accepts_migrated_lockstep_project(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Example-xport'
            startup.initialize_project(root, 'Example Game', 'EX', 45320)
            config_path = root/'xport-project.json'
            config = json.loads(config_path.read_text())
            config['paths']['disc_image'] = 'iso/game.cue'
            config['lockstep']['enabled'] = True
            config['lockstep']['original_image'] = 'MAIN.EXE'
            startup.atomic_json(config_path, config)
            (root/'iso').mkdir(exist_ok=True)
            (root/'iso/game.cue').write_text('FILE game.bin BINARY\n')
            (root/'orig/MAIN.EXE').write_bytes(b'PS-X EXE')
            result = startup.validate_capture_contract(root)
            self.assertEqual(result['schema'], 1)
            self.assertEqual(result['original_image'], 'MAIN.EXE')

    def test_capture_contract_uses_image_profile_extracted_original(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Example-xport'
            startup.initialize_project(root, 'Example Game', 'EX', 45320)
            config = json.loads((root/'xport-project.json').read_text())
            config['paths']['disc_image'] = 'iso/game.cue'
            config['lockstep'].update(enabled=True, original_image='DISC1_MAIN.EXE')
            startup.atomic_json(root/'xport-project.json', config)
            (root/'iso/game.cue').write_text('FILE game.bin BINARY\n')
            original = root/'bin/DATA/1/MAIN.EXE'
            original.parent.mkdir(parents=True, exist_ok=True)
            original.write_bytes(b'PS-X EXE')
            profile = root/'tools/ida/DISC1_MAIN.EXE.json'
            startup.atomic_json(profile, {'image': 'DISC1_MAIN.EXE',
                                         'loads': [{'path': 'bin/DATA/1/MAIN.EXE', 'base': 2147549184, 'offset': 2048}]})
            result = startup.validate_capture_contract(root)
            self.assertEqual(result['original_executable'], 'bin/DATA/1/MAIN.EXE')
            self.assertFalse((root/'orig/DISC1_MAIN.EXE').exists())

    def test_image_review_proposes_psx_profiles_and_boot_image(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root/'orig').mkdir()
            (root/'orig/SYSTEM.CNF').write_text('BOOT = cdrom:\\\\SCUS_123.45;1\n')
            files = []
            for name, address, size in (('SCUS_123.45', 0x80010000, 16), ('GAME.EXE', 0x80020000, 32)):
                raw = bytearray(0x800+size);raw[:8] = b'PS-X EXE'
                raw[0x10:0x14] = address.to_bytes(4, 'little');raw[0x14:0x18] = (0x1234).to_bytes(4, 'little')
                raw[0x18:0x1c] = address.to_bytes(4, 'little');raw[0x1c:0x20] = size.to_bytes(4, 'little')
                raw[0x800:0x804] = (0x3c1c0000).to_bytes(4, 'little')
                raw[0x804:0x808] = (0x279c1234).to_bytes(4, 'little')
                path = root/'orig'/name;path.write_bytes(raw)
                files.append({'path': 'orig/'+name, 'bytes': len(raw), 'psx_exe': {'entry': address, 'gp': 0x1234,
                              'load_address': address, 'payload_size': size, 'file_offset': 0x800}})
            proposal = startup.image_review_proposal(root, {'psx_executables': files, 'cue_files': []})
            self.assertEqual(proposal['config_patch']['paths']['original_executable'], 'orig/SCUS_123.45')
            self.assertEqual(proposal['config_patch']['analysis']['default_image'], 'GAME.EXE')
            self.assertEqual(proposal['config_patch']['lockstep']['original_image'], 'GAME.EXE')
            self.assertEqual(set(proposal['ida_configs']), {'SCUS_123.45', 'GAME.EXE'})
            self.assertEqual(proposal['ida_configs']['GAME.EXE']['gp'], 0x1234)
            self.assertEqual(proposal['ida_configs']['GAME.EXE']['gp_evidence']['source'], 'psx_exe_header')

    def test_image_review_derives_zero_header_gp_from_runtime_initializer(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory);(root/'orig').mkdir()
            address = 0x80010000;raw = bytearray(0x820);raw[:8] = b'PS-X EXE'
            raw[0x10:0x14] = address.to_bytes(4, 'little')
            raw[0x18:0x1c] = address.to_bytes(4, 'little');raw[0x1c:0x20] = (0x20).to_bytes(4, 'little')
            raw[0x800:0x804] = (0x3c1c800b).to_bytes(4, 'little')
            raw[0x804:0x808] = (0x279c3d84).to_bytes(4, 'little')
            path = root/'orig/GAME.EXE';path.write_bytes(raw)
            item = {'path': 'orig/GAME.EXE', 'bytes': len(raw), 'psx_exe': {'entry': address, 'gp': 0,
                    'load_address': address, 'payload_size': 0x20, 'file_offset': 0x800}}
            proposal = startup.image_review_proposal(root, {'psx_executables': [item], 'cue_files': []})
            profile = proposal['ida_configs']['GAME.EXE']
            self.assertEqual(profile['gp'], 0x800b3d84)
            self.assertEqual(profile['gp_evidence']['source'], 'runtime_initializer')
            self.assertEqual(startup.validate_gp_config(root, profile)['gp'], 0x800b3d84)

    def test_gp_validation_rejects_zero_or_mismatched_value(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory);(root/'orig').mkdir()
            raw = (0x3c1c8006).to_bytes(4, 'little')+(0x279c6ecc).to_bytes(4, 'little')
            (root/'orig/BOOT.EXE').write_bytes(raw)
            profile = {'gp': 0, 'loads': [{'path': 'orig/BOOT.EXE', 'base': 0x80010000}],
                       'gp_evidence': {'source': 'runtime_initializer', 'runtime_initializers': []}}
            with self.assertRaisesRegex(ValueError, 'nonzero runtime GP'):
                startup.validate_gp_config(root, profile)
            profile['gp'] = 0x800b3d84
            with self.assertRaisesRegex(ValueError, 'differs from the unique runtime initializer'):
                startup.validate_gp_config(root, profile)

    def test_ida_export_assigns_gp_after_segments_and_before_function_analysis(self):
        script = (Path(startup.__file__).resolve().parent / 'ida' /
                  'ida_analyze_export.py').read_text(encoding='utf-8')
        main = script[script.index('def main():'):]
        segments = main.index("for part in cfg['segments']")
        assignment = main.index('apply_gp(cfg)', segments)
        seeds = main.index("for ea in cfg['seeds']", assignment)
        verification = main.index('gp_verification=verify_gp(cfg)', seeds)
        export = main.index('records=[]', verification)
        self.assertLess(segments, assignment)
        self.assertLess(assignment, seeds)
        self.assertLess(verification, export)

    def test_trace_workflow_requires_the_lockstep_project_contract(self):
        config = {'lockstep': {'schema': 1, 'enabled': True}}
        self.assertIs(trace_workflow.require_lockstep(config), config['lockstep'])
        with self.assertRaisesRegex(ValueError, 'LOCKSTEP_PROJECT_NOT_MIGRATED'):
            trace_workflow.require_lockstep({'lockstep': {'schema': 1, 'enabled': False}})
        with self.assertRaisesRegex(ValueError, 'LOCKSTEP_PROJECT_NOT_MIGRATED'):
            trace_workflow.require_lockstep({'duckstation': {}})

    def test_trace_workflow_rejects_incomplete_lockstep_contract(self):
        with self.assertRaisesRegex(ValueError, 'LOCKSTEP_PROJECT_NOT_MIGRATED'):
            trace_workflow.require_lockstep({'lockstep': {'enabled': True}})
        with self.assertRaisesRegex(ValueError, 'LOCKSTEP_PROJECT_NOT_MIGRATED'):
            trace_workflow.require_lockstep({'lockstep': {'schema': 1}})

    def test_project_preflight_checks_native_platform_without_host_tools(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Example-xport';startup.initialize_project(root, 'Example Game', 'EX', 45220)
            self.assertEqual(startup.project_preflight(root), {'platform': 'x86'})

    def test_preflight_step_only_validates_shared_receipt_and_project(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Example-xport';startup.initialize_project(root, 'Example Game', 'EX', 45230)
            state = {'schema': 1, 'revision': 2, 'status': 'running', 'step': 'preflight',
                     'project_root': str(root.resolve()), 'name': 'Example Game', 'short_name': 'EX',
                     'created_utc': '2026-09-25T00:00:00+00:00', 'steps': {}, 'inventory': {},
                     'recoveries': [], 'attention': None, 'failure': None}
            prepared = {'receipt': 'receipt.json', 'tools': {'ida': 'ida', 'ghidra': 'ghidra', 'plugin': 'plugin'},
                        'capabilities': {'ready': True}, 'prepared_utc': '2026-09-25T00:00:00+00:00'}
            with mock.patch.object(startup, 'validate_toolchain_receipt', return_value=prepared) as validate, \
                 mock.patch.object(startup, 'project_preflight', return_value={'platform': 'x86'}):
                result = startup.advance(root, state)
            validate.assert_called_once_with()
            self.assertEqual(result['steps']['preflight']['result']['toolchain'], prepared)
            self.assertEqual(result['attention']['kind'], 'icon_review')
            self.assertNotIn('build', result['steps'])

    def test_build_requires_game_icon_before_compiler(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'IconGate-xport'
            startup.initialize_project(root, 'Icon Gate', 'IG', 45240)
            state = {'schema': 1, 'revision': 1, 'status': 'running', 'step': 'build',
                     'steps': {}, 'history': [], 'decisions': {}}
            with mock.patch.object(startup, 'run_shared') as run:
                result = startup.advance(root, state)
            run.assert_not_called()
            self.assertEqual(result['failure']['step'], 'build')

    def test_status_resumes_interrupted_running_state(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Example-xport'
            root.mkdir()
            state = {'schema': 1, 'revision': 3, 'status': 'running', 'step': 'build',
                     'project_root': str(root.resolve()), 'name': 'Example Game', 'short_name': 'EX'}
            startup.atomic_json(startup.state_path(root), state)
            resumed = dict(state, revision=4, status='attention', step='image_review',
                           attention={'kind': 'image_review'}, failure=None)
            with mock.patch.object(startup, 'advance', return_value=resumed) as advance:
                result = startup.status(root, 3)
            advance.assert_called_once()
            self.assertTrue(result['changed'])
            self.assertEqual(result['revision'], 4)

    def test_dispatch_worker_replaces_stale_windows_pid(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Example-xport';(root/'status').mkdir(parents=True)
            startup.atomic_json(root/'status/startup-worker.json', {'pid': 123, 'status': 'running'})
            process = mock.Mock(pid=456)
            with mock.patch.object(startup, 'process_is_running', return_value=False):
                with mock.patch.object(startup.subprocess, 'Popen', return_value=process) as popen:
                    receipt = startup.dispatch_worker(root)
            self.assertEqual(receipt['pid'], 456)
            popen.assert_called_once()

    def test_dispatch_worker_uses_requested_windows_desktop(self):
        if startup.os.name != 'nt':
            self.skipTest('Windows desktop startup information')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Example-xport';(root/'status').mkdir(parents=True)
            with mock.patch.dict(startup.os.environ, {'XPORT_WINDOWS_DESKTOP': 'WinSta0\\XportConverge'}):
                with mock.patch('windows_desktop.launch_default_desktop_process', return_value=mock.Mock(pid=456)) as launch:
                    startup.dispatch_worker(root)
            self.assertEqual(launch.call_args.kwargs['environment']['XPORT_WINDOWS_DESKTOP'], 'WinSta0\\XportConverge')
            self.assertFalse(launch.call_args.kwargs['visible'])
            self.assertEqual(launch.call_args.args[1], root.resolve())

    def test_dispatch_worker_keeps_live_windows_pid(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Example-xport';(root/'status').mkdir(parents=True)
            prior = {'pid': 123, 'status': 'running'}
            startup.atomic_json(root/'status/startup-worker.json', prior)
            with mock.patch.object(startup, 'process_is_running', return_value=True):
                with mock.patch.object(startup.subprocess, 'Popen') as popen:
                    receipt = startup.dispatch_worker(root)
            self.assertEqual(receipt, prior)
            popen.assert_not_called()

    def test_dispatch_worker_requires_host_access_before_ida_path(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Example-xport';(root/'status').mkdir(parents=True)
            state = {'schema': 1, 'revision': 8, 'status': 'running', 'step': 'ida_export',
                     'project_root': str(root.resolve()), 'name': 'Example', 'short_name': 'EX'}
            startup.atomic_json(startup.state_path(root), state)
            with mock.patch.object(startup, 'validate_runtime_access',
                                   side_effect=RuntimeError('XPORT_IDA_HOST_ACCESS_REQUIRED')):
                with mock.patch.object(startup.subprocess, 'Popen') as popen:
                    with self.assertRaisesRegex(RuntimeError, 'XPORT_IDA_HOST_ACCESS_REQUIRED'):
                        startup.dispatch_worker(root)
            popen.assert_not_called()

    def test_retry_archives_small_diagnostics_and_removes_failed_run(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Example-xport';root.mkdir()
            run = root/'status/ida/runs/startup-ida-v1';run.mkdir(parents=True)
            (run/'error.log').write_text('failure details')
            current = startup.inventory(root)
            state = {'schema': 1, 'revision': 7, 'status': 'failed', 'step': 'ida_export',
                     'project_root': str(root.resolve()), 'name': 'Example', 'short_name': 'EX',
                     'inventory': current, 'failure': {'step': 'ida_export'}, 'steps': {}}
            startup.atomic_json(startup.state_path(root), state)
            with mock.patch.object(startup, 'advance', side_effect=lambda _root, value: value):
                retried = startup.retry(root, 7)
            self.assertEqual(retried['status'], 'running')
            self.assertFalse(run.exists())
            receipt = startup.read_json(root/retried['recoveries'][0]['receipt'])
            self.assertTrue(receipt['files'][0]['retained'])
            self.assertTrue((root/'status/startup-recovery/blobs'/receipt['files'][0]['sha256']).is_file())

    def test_retry_accepts_legacy_inventory_without_total_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Example-xport';root.mkdir()
            current = startup.inventory(root)
            current.pop('total_bytes')
            current.pop('system_configs')
            state = {'schema': 1, 'revision': 7, 'status': 'failed', 'step': 'verify_frontier',
                     'project_root': str(root.resolve()), 'name': 'Example', 'short_name': 'EX',
                     'inventory': current, 'failure': {'step': 'verify_frontier'}, 'steps': {}}
            startup.atomic_json(startup.state_path(root), state)
            with mock.patch.object(startup, 'advance', side_effect=lambda _root, value: value):
                retried = startup.retry(root, 7)
            self.assertEqual(retried['status'], 'running')


if __name__ == '__main__':
    unittest.main()

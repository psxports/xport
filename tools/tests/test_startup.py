import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import startup


class StartupSupervisorTests(unittest.TestCase):
    def start_to_image_review(self, root):
        with mock.patch.object(startup, 'run_shared', return_value='ok'):
            return startup.start(root, 'Example Game', 'EX', 45000)

    def test_start_runs_deterministic_steps_until_attention(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Example-xport'
            state = self.start_to_image_review(root)
            self.assertEqual(state['status'], 'attention')
            self.assertEqual(state['attention']['kind'], 'image_review')
            self.assertEqual(state['steps']['initialize']['status'], 'passed')
            self.assertEqual(state['steps']['build']['status'], 'passed')
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

    def test_duplicate_worker_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Example-xport'
            with startup.StartupLock(root):
                with self.assertRaisesRegex(ValueError, 'STARTUP_WORKER_ALREADY_RUNNING'):
                    with startup.StartupLock(root):
                        pass

    def test_tool_discovery_validates_configured_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Example-xport'
            startup.initialize_project(root, 'Example Game', 'EX', 45200)
            ida = root/'vendor/idat.exe'
            ghidra = root/'vendor/ghidra'
            plugin = root/'vendor/plugin'
            ida.parent.mkdir(parents=True)
            ida.touch()
            (ghidra/'support').mkdir(parents=True)
            (ghidra/'support/analyzeHeadless.bat').touch()
            (plugin/'data/psyq').mkdir(parents=True)
            found, rejected = startup.discover_tools(root, {'ida': ida, 'ghidra': ghidra, 'plugin': plugin})
            self.assertEqual(set(found), {'ida', 'ghidra', 'plugin'})
            self.assertEqual(rejected, {})

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


if __name__ == '__main__':
    unittest.main()

import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import converge_telemetry


class SelectTranscriptTests(unittest.TestCase):
    def make_transcript(self, root, session_id, cwd):
        path = root / '2026' / '09' / '23' / ('rollout-' + session_id + '.jsonl')
        path.parent.mkdir(parents=True, exist_ok=True)
        payload = {
            'session_id': session_id,
            'cwd': str(cwd),
            'thread_source': 'user',
            'originator': 'Codex Desktop',
        }
        path.write_text(json.dumps({'type': 'session_meta', 'payload': payload}) + '\n')
        return path

    def test_explicit_session_owns_nested_project(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            workspace = root / 'workspace'
            project = workspace / 'game'
            project.mkdir(parents=True)
            transcript = self.make_transcript(root / 'sessions', 'thread-1', workspace)
            with mock.patch.object(converge_telemetry, 'transcript_root', return_value=root / 'sessions'):
                with mock.patch.dict(os.environ, {'CODEX_THREAD_ID': 'thread-1'}, clear=False):
                    selected, _ = converge_telemetry.select_transcript(project)
            self.assertEqual(selected, transcript)

    def test_explicit_session_rejects_project_outside_workspace(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            workspace = root / 'workspace'
            project = root / 'other' / 'game'
            workspace.mkdir(parents=True)
            project.mkdir(parents=True)
            self.make_transcript(root / 'sessions', 'thread-1', workspace)
            with mock.patch.object(converge_telemetry, 'transcript_root', return_value=root / 'sessions'):
                with mock.patch.dict(os.environ, {'CODEX_THREAD_ID': 'thread-1'}, clear=False):
                    with self.assertRaisesRegex(ValueError, 'Current Codex Desktop transcript not found'):
                        converge_telemetry.select_transcript(project)


if __name__ == '__main__':
    unittest.main()

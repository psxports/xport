import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from trace_converge_progress import update
from trace_converge_worker import progress_view, running_view


class ConvergeProgressTests(unittest.TestCase):
    def test_step_change_clears_stale_tick(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'progress.json'
            with mock.patch.dict(os.environ, {'XPORT_CONVERGE_PROGRESS': str(path)}, clear=False):
                update('capture', started=10, stage=2, tick=345, phase=678)
                update('compare', detail='verify')
            value = json.loads(path.read_text())
            self.assertEqual(value['step'], 'compare')
            self.assertEqual(value['detail'], 'verify')
            self.assertNotIn('tick', value)
            self.assertNotIn('phase', value)

    def test_running_view_reports_live_position(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            progress = {
                'started': 100,
                'updated': 120,
                'step': 'capture',
                'detail': 'native_replay',
                'segment': 1,
                'stage': 3,
                'tick': 456,
                'tick_end': 900,
                'phase': 789,
                'phase_end': 1200,
            }
            (root / 'progress.json').write_text(json.dumps(progress))
            with mock.patch('trace_converge_worker.time.time', return_value=125):
                value = running_view('race', root / 'state.json', root, {'worker': {'pid': 42}})
            self.assertEqual(value['tick'], 456)
            self.assertEqual(value['phase'], 789)
            self.assertEqual(value['elapsed_seconds'], 25)
            self.assertEqual(value['heartbeat_age_seconds'], 5)
            self.assertEqual(value['worker_pid'], 42)

    def test_terminal_progress_is_inactive_and_uses_finished_elapsed_time(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            progress = {
                'started': 100,
                'updated': 120,
                'step': 'capture',
                'detail': 'native_replay',
                'tick': 456,
                'phase': 789,
                'worker': {'pid': 43},
            }
            (root / 'progress.json').write_text(json.dumps(progress))
            state = {'started': 100, 'finished': 130, 'worker': {'pid': 42}}
            value = progress_view(root, state, False, now=150)
            self.assertFalse(value['active'])
            self.assertEqual(value['tick'], 456)
            self.assertEqual(value['elapsed_seconds'], 30)
            self.assertEqual(value['heartbeat_age_seconds'], 30)
            self.assertEqual(value['worker_pid'], 43)


if __name__ == '__main__':
    unittest.main()

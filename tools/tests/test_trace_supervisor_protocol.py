import json
from pathlib import Path
import sqlite3
import tempfile
import unittest
from unittest import mock

from trace_converge_worker import control
from trace_supervisor_protocol import STATES, canonical, digest, source_snapshot, submit, todo_branch, transition


class SupervisorProtocolTests(unittest.TestCase):
    def test_transition_revision_changes_only_for_meaningful_state(self):
        state = {}
        self.assertTrue(transition(state, 'STARTING'))
        self.assertEqual(state['revision'], 1)
        self.assertFalse(transition(state, 'STARTING'))
        self.assertEqual(state['revision'], 1)
        self.assertTrue(transition(state, 'VALIDATING'))
        self.assertEqual(state['revision'], 2)
        self.assertEqual([row['status'] for row in state['transitions']], ['STARTING', 'VALIDATING'])
        self.assertEqual(tuple(STATES), ('STARTING', 'VALIDATING', 'REPLAYING',
            'ATTENTION_REQUIRED', 'VERIFYING_REPAIR', 'MATCH', 'FINALIZING', 'COMPLETE', 'FAILED'))
        with self.assertRaises(ValueError):
            transition(state, 'COMPLETE')

    def test_todo_branch_closes_direct_todo_dependencies_and_records_boundary(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            database = root / 'analysis.sqlite'
            db = sqlite3.connect(database)
            try:
                db.executescript('''
                    CREATE TABLE functions(image TEXT,address INTEGER,name TEXT,status TEXT,bytes INTEGER,sha256 TEXT,listing TEXT,pseudocode TEXT);
                    CREATE TABLE edges(id INTEGER PRIMARY KEY,source_image TEXT,source_function INTEGER,site INTEGER,target INTEGER,kind TEXT,resolution TEXT);
                    CREATE TABLE edge_candidates(edge INTEGER,target_image TEXT,target_function INTEGER);
                ''')
                db.executemany('INSERT INTO functions VALUES(?,?,?,?,?,?,?,?)', [
                    ('GAME', 0x80010000, 'root', 'TODO', 16, 'a', 'root.lst', 'root.c'),
                    ('GAME', 0x80010100, 'leaf', 'TODO', 12, 'b', 'leaf.lst', 'leaf.c'),
                    ('GAME', 0x80010200, 'done', 'WIP', 8, 'c', 'done.lst', 'done.c')])
                db.executemany('INSERT INTO edges VALUES(?,?,?,?,?,?,?)', [
                    (1, 'GAME', 0x80010000, 0x80010010, 0x80010100, 'call', 'same_image'),
                    (2, 'GAME', 0x80010100, 0x80010110, 0x80010200, 'call', 'same_image')])
                db.executemany('INSERT INTO edge_candidates VALUES(?,?,?)', [
                    (1, 'GAME', 0x80010100), (2, 'GAME', 0x80010200)])
                db.commit()
            finally:
                db.close()
            audit = {'image': 'GAME', 'function': {'address': 0x80010000}}
            value = todo_branch(root, {'paths': {'analysis_database': 'analysis.sqlite'}}, audit)
            self.assertTrue(value['complete'])
            self.assertEqual([row['address'] for row in value['nodes']], ['0x80010000', '0x80010100'])
            self.assertEqual(value['boundaries'], [{'image': 'GAME', 'address': '0x80010200', 'status': 'WIP'}])
            self.assertEqual(value['review_batches'][0]['instructions'], 7)

    def test_todo_branch_uses_current_ledger_over_stale_database_status(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'status').mkdir()
            (root / 'status/translation-ledger.json').write_text(json.dumps({'entries': [{
                'image': 'GAME', 'address': 0x80010000, 'status': 'WIP', 'source': 'src/game.c'}]}))
            database = root / 'analysis.sqlite'
            db = sqlite3.connect(database)
            try:
                db.executescript('''
                    CREATE TABLE functions(image TEXT,address INTEGER,name TEXT,status TEXT,bytes INTEGER,sha256 TEXT,listing TEXT,pseudocode TEXT);
                    CREATE TABLE edges(id INTEGER PRIMARY KEY,source_image TEXT,source_function INTEGER,site INTEGER,target INTEGER,kind TEXT,resolution TEXT);
                    CREATE TABLE edge_candidates(edge INTEGER,target_image TEXT,target_function INTEGER);
                ''')
                db.execute('INSERT INTO functions VALUES(?,?,?,?,?,?,?,?)',
                           ('GAME', 0x80010000, 'root', 'TODO', 16, 'a', 'root.lst', 'root.c'))
                db.commit()
            finally:
                db.close()
            audit = {'image': 'GAME', 'function': {'address': 0x80010000}}
            value = todo_branch(root, {'paths': {'analysis_database': 'analysis.sqlite'}}, audit)
            self.assertTrue(value['complete'])
            self.assertEqual(value['nodes'][0]['status'], 'WIP')

    def test_submit_validates_exact_branch_and_owns_deterministic_steps(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'src').mkdir()
            source = root / 'src/game.c'
            source.write_text('int repaired(void) { return 1; }\n')
            status = root / 'status'
            folder = status / 'stage-pipeline/workers/race'
            folder.mkdir(parents=True)
            before_files = {'src/game.c': canonical('old source')}
            before_entry = {'image': 'GAME', 'address': 0x80010000, 'source': 'src/game.c', 'status': 'TODO'}
            current_entry = {'image': 'GAME', 'address': 0x80010000, 'source': 'src/game.c', 'status': 'WIP'}
            ledger_path = status / 'translation-ledger.json'
            ledger_path.write_text(json.dumps({'entries': [current_entry]}))
            ledger_before = folder / 'ledger-before.json'
            ledger_before.write_text(json.dumps({'entries': [before_entry]}))
            attention = {
                'kind': 'todo_branch', 'failure_signature': 'failure', 'image': 'GAME', 'address': '0x80010000',
                'source': {'identity': canonical(before_files), 'files': before_files},
                'ledger': {'sha256': 'ledger-before', 'snapshot': str(ledger_before),
                           'snapshot_sha256': digest(ledger_before)},
                'todo_dependency_branch': {'complete': True, 'nodes': [
                    {'image': 'GAME', 'address': '0x80010000'}]},
                'batch_limits': {'functions': 5, 'instructions': 2048, 'modules': 2}}
            attention_path = folder / 'attention.json'
            attention_path.write_text(json.dumps(attention))
            manifest = {
                'schema': 1, 'failure_signature': 'failure', 'attention_sha256': digest(attention_path),
                'expected_source_identity': attention['source']['identity'], 'expected_ledger_sha256': 'ledger-before',
                'image': 'GAME', 'address': '0x80010000',
                'changed_files': ['src/game.c'],
                'changed_functions': [{'image': 'GAME', 'address': '0x80010000', 'source': 'src/game.c'}],
                'ledger_entries': [current_entry],
                'completed_dependencies': [{'image': 'GAME', 'address': '0x80010000'}],
                'evidence_hashes': {'mips': 'evidence'}}
            manifest_path = status / 'repair-manifest.json'
            manifest_path.write_text(json.dumps(manifest))
            config = {'paths': {'status': 'status'}}
            with mock.patch('trace_supervisor_protocol.load_project', return_value=(root, config)), \
                 mock.patch('trace_supervisor_protocol.artifact_path', side_effect=lambda value: root / value), \
                 mock.patch('trace_supervisor_protocol.run_step', side_effect=lambda root, folder, tool, arguments=(): {'tool': tool}), \
                 mock.patch('trace_converge_worker.control', return_value={'status': 'running'}) as control:
                result = submit('race', manifest_path, 7)
            self.assertEqual(result['status'], 'running')
            self.assertEqual(json.loads((folder / 'repair-submission.json').read_text())['source'], source_snapshot(root))
            control.assert_called_once_with('race', 'start', 7, final_gate_on_match=True, resume_attention=True)

    def test_status_after_same_revision_returns_unchanged_without_mutation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            folder = root / 'status/stage-pipeline/workers/race'
            folder.mkdir(parents=True)
            state = {'status': 'REPLAYING', 'revision': 4, 'worker': {'pid': 42, 'created': 1}}
            (folder / 'state.json').write_text(json.dumps(state))
            (folder / 'launch.json').write_text(json.dumps({'worker': state['worker']}))
            with mock.patch('trace_converge_worker.load_project', return_value=(root, {})), \
                 mock.patch('trace_converge_worker.artifact_path', side_effect=lambda value: root / value), \
                 mock.patch('trace_converge_worker.alive', return_value=True):
                value = control('race', 'status', 0, after_revision=4)
            self.assertEqual(value['status'], 'unchanged')
            self.assertEqual(value['revision'], 4)
            self.assertEqual(json.loads((folder / 'state.json').read_text()), state)


if __name__ == '__main__':
    unittest.main()

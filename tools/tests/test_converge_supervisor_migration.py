import json
from pathlib import Path
import tempfile
import unittest

from migrations.converge_supervisor_v1 import migrate


class ConvergeSupervisorMigrationTests(unittest.TestCase):
    def test_migration_adds_protocol_without_replacing_stage_configuration(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / 'xport-project.json'
            path.write_text(json.dumps({'schema': 1, 'stage_pipeline': {'entry_tick': 7}}))
            result = migrate(root, True)
            value = json.loads(path.read_text())
            self.assertTrue(result['applied'])
            self.assertEqual(value['stage_pipeline']['entry_tick'], 7)
            self.assertEqual(value['stage_pipeline']['supervisor_protocol']['mode'], 'enforced')
            self.assertEqual(value['stage_pipeline']['supervisor_protocol']['acceptance']['max_compactions'], 2)

    def test_migration_is_idempotent(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / 'xport-project.json'
            path.write_text(json.dumps({'schema': 1}))
            migrate(root, True)
            self.assertFalse(migrate(root, False)['changed'])


if __name__ == '__main__':
    unittest.main()

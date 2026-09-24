import tempfile
from pathlib import Path
import unittest

import upgrade


REVISION_1 = '2026-09-24T05:42:31Z'
REVISION_2 = '2026-09-24T06:00:00Z'


def changelog():
    return f'''# Changelog

## {REVISION_2}

- Scope: runtime
- Compatibility: Breaking
- Changed: Changed an interface
- Upgrade: Update the binding
- Automation: none
- Verify: X doctor; X code_refresh

## {REVISION_1}

- Scope: bootstrap
- Compatibility: Compatible
- Changed: Added a marker
- Upgrade: Add the marker
- Automation: marker-only
- Verify: X doctor
'''


class UpgradeTests(unittest.TestCase):
    def project(self, body):
        temporary = tempfile.TemporaryDirectory()
        root = Path(temporary.name)
        (root/'src').mkdir()
        path = root/'src/game_main.c'
        path.write_text(body, encoding='utf-8')
        return temporary, root, path

    def test_parse_and_select_only_newer_entries(self):
        entries = upgrade.parse_changelog(changelog())
        self.assertEqual([REVISION_2, REVISION_1], [entry['revision'] for entry in entries])
        self.assertEqual([REVISION_2], [entry['revision'] for entry in upgrade.select_entries(entries, REVISION_1, REVISION_2)])

    def test_insert_and_replace_marker(self):
        temporary, root, path = self.project('int xport_main(int argc, char **argv)\n{\n    return argc;\n}\n')
        with temporary:
            state = upgrade.marker_state(root)
            self.assertIsNone(state['revision'])
            upgrade.set_marker(state, REVISION_1)
            self.assertIn('// XPORT REVISION: '+REVISION_1+'\nint xport_main', path.read_text())
            state = upgrade.marker_state(root)
            upgrade.set_marker(state, REVISION_2)
            self.assertIn('// XPORT REVISION: '+REVISION_2+'\nint xport_main', path.read_text())

    def test_completion_relocates_entrypoint_after_migration(self):
        temporary, root, path = self.project('int removed;\nint xport_main(void)\n{\n    return 0;\n}\n')
        with temporary:
            path.write_text(path.read_text().replace('int removed;\n', ''))
            upgrade.complete_marker(root, REVISION_1)
            self.assertTrue(path.read_text().startswith('// XPORT REVISION: '+REVISION_1+'\nint xport_main'))

    def test_reject_marker_away_from_entrypoint(self):
        temporary, root, _ = self.project('// XPORT REVISION: '+REVISION_1+'\nint other;\nint xport_main(void)\n{\n}\n')
        with temporary:
            with self.assertRaisesRegex(ValueError, 'directly before'):
                upgrade.marker_state(root)

    def test_reject_changelog_out_of_order(self):
        text = changelog().replace('## '+REVISION_2, '## 2026-09-24T05:00:00Z')
        with self.assertRaisesRegex(ValueError, 'newest first'):
            upgrade.parse_changelog(text)

    def test_reject_multiple_entrypoints(self):
        temporary, root, _ = self.project('int xport_main(void)\n{\n}\nint xport_main(int argc)\n{\n}\n')
        with temporary:
            with self.assertRaisesRegex(ValueError, 'exactly one'):
                upgrade.marker_state(root)

    def test_verify_commands_are_restricted_to_xport_tools(self):
        self.assertEqual([['doctor'], ['code_refresh']], upgrade.split_commands('X doctor; X code_refresh'))
        with self.assertRaisesRegex(ValueError, 'must use'):
            upgrade.split_commands('python arbitrary.py')


if __name__ == '__main__':
    unittest.main()

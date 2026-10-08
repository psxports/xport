import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import toolchain_preflight as preflight


class ToolchainPreflightTests(unittest.TestCase):
    def tools(self, root):
        ida = root/'idat.exe';ida.write_text('ida')
        java = root/'jdk/bin/java.exe';java.parent.mkdir(parents=True);java.write_text('java')
        ghidra = root/'ghidra_12.0_PUBLIC';(ghidra/'support').mkdir(parents=True)
        (ghidra/'support/analyzeHeadless.bat').write_text('ghidra')
        plugin = root/'profiles/ghidra_12.0_PUBLIC/Extensions/ghidra_psx_ldr'
        (plugin/'data/psyq').mkdir(parents=True)
        (plugin/'data/psyq/patches.json').write_text('{}')
        (plugin/'Module.manifest').write_text('module')
        (plugin/'lib').mkdir();(plugin/'lib/ghidra_psx_ldr.jar').write_text('jar')
        return {'ida': str(ida), 'java': str(java), 'ghidra': str(ghidra), 'plugin': str(plugin)}

    def test_prepare_runs_capabilities_once_and_reuses_content_bound_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory);tools = self.tools(root);receipt = root/'receipt.json'
            runner = mock.Mock()
            with mock.patch.object(preflight, 'discover_tools', return_value=tools), \
                 mock.patch.object(preflight, 'run_probes', return_value=preflight.CAPABILITIES.copy()) as probes, \
                 mock.patch.dict('os.environ', {'XPORT_TOOLCHAIN_RECEIPT': str(receipt)}):
                first = preflight.prepare(runner=runner)
                second = preflight.prepare(runner=runner)
            self.assertFalse(first['cached'])
            self.assertTrue(second['cached'])
            probes.assert_called_once_with(tools, runner)

    def test_changed_tool_invalidates_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory);tools = self.tools(root);receipt = root/'receipt.json'
            value = {'schema': preflight.SCHEMA, 'passed': True, 'tools': tools,
                     'identities': preflight.identities(tools), 'capabilities': preflight.CAPABILITIES,
                     'prepared_utc': '2026-09-25T00:00:00+00:00'}
            preflight.atomic_json(receipt, value)
            Path(tools['ida']).write_text('changed')
            with self.assertRaisesRegex(RuntimeError, 'XPORT_TOOLCHAIN_NOT_PREPARED.*stale receipt'):
                preflight.validate_receipt(receipt)

    def test_missing_receipt_reports_executable_prepare_command(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(RuntimeError, r'XPORT_TOOLCHAIN_NOT_PREPARED.*prepare\.bat --verify-only'):
                preflight.validate_receipt(Path(directory)/'missing.json')

    def test_discovery_pairs_plugin_profile_with_matching_ghidra(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory);tools = self.tools(root)
            old = root/'ghidra_10.0_PUBLIC';(old/'support').mkdir(parents=True)
            (old/'support/analyzeHeadless.bat').write_text('old')
            with mock.patch.object(preflight, 'standard_candidates', return_value={
                    'ida': [Path(tools['ida'])], 'java': [Path(tools['java'])],
                    'ghidra': [old, Path(tools['ghidra'])], 'plugin': [Path(tools['plugin'])]}), \
                 mock.patch.object(preflight, 'cached_tools', return_value={}):
                found = preflight.discover_tools()
            self.assertEqual(found['ghidra'], tools['ghidra'])

    def test_mismatched_extension_points_to_versioned_releases(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory);tools = self.tools(root)
            wrong = root/'ghidra_13.0_PUBLIC';(wrong/'support').mkdir(parents=True)
            (wrong/'support/analyzeHeadless.bat').write_text('wrong')
            supplied = dict(tools, ghidra=str(wrong))
            with mock.patch.object(preflight, 'standard_candidates', return_value={
                    'ida': [], 'java': [], 'ghidra': [], 'plugin': []}), \
                 mock.patch.object(preflight, 'cached_tools', return_value={}):
                with self.assertRaisesRegex(ValueError, r'profiles do not match.*github\.com/lab313ru/ghidra_psx_ldr/releases'):
                    preflight.discover_tools(supplied)

    def test_runtime_access_reports_hidden_ida_registry(self):
        with mock.patch('winreg.OpenKey', side_effect=PermissionError('denied')):
            with self.assertRaisesRegex(RuntimeError, 'XPORT_IDA_HOST_ACCESS_REQUIRED'):
                preflight.validate_runtime_access()


if __name__ == '__main__':
    unittest.main()

import os
import unittest
from unittest import mock

import xport_process


class ProcessEnvironmentTests(unittest.TestCase):
    def test_windows_environment_has_one_case_insensitive_key(self):
        source = {'Path': 'alias', 'PATH': 'canonical', 'Temp': 'first', 'TEMP': 'canonical-temp'}
        with mock.patch.object(xport_process.os, 'name', 'nt'):
            result = xport_process.normalized_environment(source)
        self.assertEqual(result, {'PATH': 'canonical', 'TEMP': 'canonical-temp'})

    def test_windows_updates_override_base_case_insensitively(self):
        with mock.patch.object(xport_process.os, 'name', 'nt'):
            result = xport_process.normalized_environment({'Path': 'base'}, {'path': 'updated'})
        self.assertEqual(result, {'PATH': 'updated'})

    def test_non_windows_environment_preserves_case(self):
        with mock.patch.object(xport_process.os, 'name', 'posix'):
            result = xport_process.normalized_environment({'Path': 'a'}, {'PATH': 'b'})
        self.assertEqual(result, {'Path': 'a', 'PATH': 'b'})

    def test_current_windows_environment_is_replaced(self):
        environment = {'Path': 'alias', 'PATH': 'canonical'}
        with mock.patch.object(xport_process.os, 'name', 'nt'), \
                mock.patch.object(xport_process.os, 'environ', environment):
            xport_process.normalize_current_environment()
        self.assertEqual(environment, {'PATH': 'canonical'})


if __name__ == '__main__':
    unittest.main()

import json
import hashlib
import os
import sqlite3
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import source_index
import startup


class ProjectBootstrapTests(unittest.TestCase):
    def test_initializer_uses_user_short_name_for_native_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'FightingForce-xport'
            config = startup.initialize_project(root, 'Fighting Force', 'FF', 43000)
            self.assertEqual(config['short_name'], 'FF')
            self.assertEqual(config['paths']['native_solution'], 'src/platform/win/FF.sln')
            self.assertEqual(config['paths']['native_executable'], 'bin/FF.exe')
            self.assertEqual(config['paths']['native_debug_executable'], 'bin/FF_debug.exe')
            self.assertEqual(config['code_refresh']['build']['process_name'], 'FF_debug.exe')
            self.assertTrue((root/'src/platform/win/FF.sln').is_file())
            self.assertFalse((root/'DuckStation.bat').exists())
            self.assertIn('Native short name: `FF`', (root/'AGENTS.md').read_text(encoding='utf-8'))
            project = (root/'src/platform/win/FF.vcxproj').read_text(encoding='utf-8')
            solution = (root/'src/platform/win/FF.sln').read_text(encoding='utf-8')
            self.assertIn('Debug|x86 = Debug|x86', solution)
            self.assertIn('Release|x86 = Release|x86', solution)
            self.assertIn('Debug|Win32', project)
            self.assertIn('Release|Win32', project)
            self.assertEqual(project.count('<SubSystem>Windows</SubSystem>'), 2)
            self.assertNotIn('<EntryPointSymbol>', project)
            self.assertNotIn('<SubSystem>Console</SubSystem>', project)
            self.assertIn('<CompileAs>CompileAsC</CompileAs>', project)
            self.assertIn('<TargetName>FF_debug</TargetName>', project)
            self.assertIn('<TargetName>FF</TargetName>', project)
            self.assertIn(r'<OutDir>$(ProjectDir)..\..\..\bin\</OutDir>', project)
            self.assertIn('psx_spu.c', project)
            self.assertIn('psx_gpu.c', project)
            self.assertIn('platform\\win\\main.c', project)
            self.assertIn('\\xport.h', project)
            self.assertNotIn('xport_game.h', project)
            self.assertNotIn('xport_platform.h', project)
            self.assertEqual(project.count('FULL_NAME=&quot;Fighting Force&quot;'), 2)
            self.assertIn('WND_TITLE=&quot;Fighting Force (xport)&quot;', project)
            self.assertIn('WND_WIDTH=960', project)
            self.assertIn('WND_HEIGHT=768', project)
            self.assertIn('FIELD_RATE=50', project)
            self.assertEqual(project.count('_CRT_SECURE_NO_WARNINGS'), 2)
            self.assertEqual(project.count('<TreatWarningAsError>true</TreatWarningAsError>'), 2)
            self.assertEqual(project.count('<DisableSpecificWarnings>4100;%(DisableSpecificWarnings)</DisableSpecificWarnings>'), 2)
            self.assertIn('winmm.lib', project)
            main = (root/'src/game_main.c').read_text(encoding='utf-8')
            self.assertIn('void xport_main(void)', main)
            self.assertNotIn('audio_runtime_render', main)
            self.assertNotIn('spu_render', main)
            self.assertNotIn('xport_address', main)
            self.assertNotIn('xport_audio_output_environment', main)
            self.assertEqual({path.name for path in (root/'src/platform/win').iterdir()}, {'FF.sln', 'FF.vcxproj'})
            self.assertIn('#include "xport.h"', main)
            self.assertEqual(main.count('#include "xport.h"'), 1)
            self.assertFalse((root/'src/xport.h').exists())
            agents = (root/'AGENTS.md').read_text(encoding='utf-8')
            self.assertIn('Debug executable: `bin/FF_debug.exe`', agents)
            self.assertIn('Release executable: `bin/FF.exe`', agents)
            self.assertIn('Runtime data: `bin/DATA`', agents)
            self.assertIn('Red Book output when applicable: `bin/MUSIC`', agents)
            self.assertEqual(json.loads((root/'status/translation-ledger.json').read_text())['entries'], [])
            lockstep = config['lockstep']
            self.assertEqual(lockstep['schema'], 1)
            self.assertFalse(lockstep['enabled'])
            self.assertEqual(lockstep['native_platform'], 'x86')
            self.assertEqual(lockstep['runtime']['duckstation_role'], 'lockstep')
            self.assertEqual(lockstep['runtime']['record_role'], 'user')
            self.assertEqual(set(config['duckstation']['reserved_gdb_ports']), {'user', 'lockstep', 'audit', 'trace'})
            self.assertEqual(len(set(config['duckstation']['reserved_gdb_ports'].values())), 4)

    def test_initializer_defines_unresolved_gpu_graph_type_binding(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Binding-xport'
            startup.initialize_project(root, 'Binding Fixture', 'BF', 43110)
            main = (root/'src/game_main.c').read_text(encoding='utf-8')
            self.assertIn('const uint32 xport_gpu_graph_type_address = 0u;', main)
            self.assertIn('Unresolved until the original PsyQ graph type storage is audited', main)

    def test_initializer_renders_shared_native_templates_without_placeholders(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Template-xport'
            startup.initialize_project(root, 'Rock & Roll', 'RNR', 43010)
            solution = (root/'src/platform/win/RNR.sln').read_text(encoding='utf-8')
            project = (root/'src/platform/win/RNR.vcxproj').read_text(encoding='utf-8')
            self.assertIn('"RNR", "RNR.vcxproj"', solution)
            self.assertIn('<RootNamespace>RNR</RootNamespace>', project)
            self.assertIn('FULL_NAME=&quot;Rock &amp; Roll&quot;', project)
            self.assertNotIn('@@', solution+project)

    def test_native_template_renderer_rejects_unresolved_tokens(self):
        with tempfile.TemporaryDirectory() as directory:
            template_dir = Path(directory)
            (template_dir/'broken.sln').write_text('@@UNKNOWN@@\n', encoding='utf-8')
            with mock.patch.object(startup, 'NATIVE_TEMPLATE_DIR', template_dir):
                with self.assertRaisesRegex(ValueError, 'Unresolved native project template tokens'):
                    startup.render_native_template('broken.sln', {})

    def test_source_index_accepts_generic_and_legacy_function_markers(self):
        text = '''int first(void)\n{\n    FUNCTION_MARKER(0x80010000u, "GAME.EXE");\n    return 1;\n}\n\nint second(void)\n{\n    FF_FUNCTION_MARKER(0x80010004u, "GAME.EXE");\n    return 2;\n}\n\nint renamed(void)\n{\n    FUNCTION_MARKER_ARGS(0x80010008u, "GAME.EXE", XPORT_CALL_VALUE_SCALAR, 0u);\n    return 3;\n}\n'''
        parsed = source_index.functions(text, 'src/game.c')
        self.assertEqual(parsed[0]['markers'], [(0x80010000, 'GAME.EXE')])
        self.assertEqual(parsed[1]['markers'], [(0x80010004, 'GAME.EXE')])
        self.assertEqual(parsed[2]['markers'], [(0x80010008, 'GAME.EXE')])
        selected, status, basis = source_index.select_implementation(
            {'address': 0x80010008, 'image': 'GAME.EXE'}, parsed)
        self.assertEqual((selected['name'], status, basis), ('renamed', 'mapped', 'marker'))

    def test_source_index_synchronizes_ledger_statuses(self):
        with sqlite3.connect(':memory:') as db:
            db.execute('CREATE TABLE functions(image TEXT,address INTEGER,status TEXT)')
            db.executemany('INSERT INTO functions VALUES(?,?,?)', [('GAME.EXE', 1, 'TODO'), ('GAME.EXE', 2, 'DONE'),
                                                                  ('GAME.EXE', 3, 'SKIP')])
            result = source_index.sync_function_statuses(db,
                [{'image': 'GAME.EXE', 'address': 1, 'status': 'DONE'}], {('GAME.EXE', 1), ('GAME.EXE', 2), ('GAME.EXE', 3)})
            self.assertEqual(result['updated'], 1)
            self.assertEqual(result['reset_removed'], 1)
            self.assertEqual(dict((address, status) for address, status in db.execute('SELECT address,status FROM functions')),
                             {1: 'DONE', 2: 'TODO', 3: 'SKIP'})

    def test_initializer_rejects_derived_or_unsafe_short_names(self):
        for value in ('', 'Fighting Force', '1FF', 'name-with-dash', 'A'*17, 'CON'):
            with self.subTest(value=value), self.assertRaises(ValueError):
                startup.validate_names('Fighting Force', value)

    def test_initializer_rejects_unsafe_window_titles(self):
        for value in ('Bad "Title', 'Bad;Title', 'Bad\nTitle'):
            with self.subTest(value=value), self.assertRaises(ValueError):
                startup.validate_names(value, 'SAFE')

    def test_initializer_rejects_initialized_project(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Existing-xport'
            startup.initialize_project(root, 'Existing Game', 'EX', 43200)
            with self.assertRaisesRegex(ValueError, 'STARTUP_ALREADY_INITIALIZED'):
                startup.initialize_project(root, 'Existing Game', 'EX', 43200)

    def test_accept_publishes_only_complete_canonical_export(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'Example-xport'
            startup.initialize_project(root, 'Example Game', 'EX', 43100)
            config = root/'tools/ida/GAME.EXE.json'
            config.write_text('{}\n', encoding='utf-8')
            image = root/'status/ida/runs/export-v1/GAME.EXE'
            (image/'functions').mkdir(parents=True)
            (image/'pseudocode').mkdir()
            word = bytes.fromhex('00000000')
            function = {'address': 0x80010000, 'sha256': hashlib.sha256(word).hexdigest(),
                        'raw_instructions': [{'address': 0x80010000, 'bytes': word.hex()}],
                        'pseudocode': 'pseudocode/80010000.c'}
            (image/'functions.json').write_text(json.dumps([function]), encoding='utf-8')
            (image/'export-report.json').write_text(json.dumps({'image': 'GAME.EXE', 'source_bytes_verified': True,
                                                                 'independent_decoder': 'capstone test',
                                                                 'raw_instruction_words': 1}), encoding='utf-8')
            (image/'functions/80010000.lst').write_text('listing\n', encoding='utf-8')
            (image/'functions/80010000.mips.txt').write_text('mips\n', encoding='utf-8')
            (image/'pseudocode/80010000.c').write_text('void f(void) {}\n', encoding='utf-8')
            (image/'GAME.EXE.lst').write_text('listing\n', encoding='utf-8')
            (image/'GAME.EXE.c').write_text('void f(void) {}\n', encoding='utf-8')
            previous = os.environ.get('XPORT_PROJECT')
            os.environ['XPORT_PROJECT'] = str(root)
            try:
                result = startup.accept_ida(root, 'export-v1', ['GAME.EXE'])
                self.assertEqual(result['accepted'], ['GAME.EXE'])
                self.assertTrue((root/'orig/images/GAME.EXE/functions/80010000.mips.txt').is_file())
                receipt = json.loads((root/result['receipt']).read_text())
                self.assertEqual(receipt['images'][0]['functions'], 1)
                with self.assertRaises(ValueError):
                    startup.accept_ida(root, 'export-v1', ['GAME.EXE'])
            finally:
                if previous is None:
                    os.environ.pop('XPORT_PROJECT', None)
                else:
                    os.environ['XPORT_PROJECT'] = previous


if __name__ == '__main__':
    unittest.main()

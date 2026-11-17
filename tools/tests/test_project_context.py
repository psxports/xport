import json
from contextlib import closing
from pathlib import Path
import sqlite3
import tempfile
import unittest
from unittest.mock import patch

import project_context as context
import query


class ProjectContextTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / 'status').mkdir()
        (self.root / 'src').mkdir()
        self.config = {'schema': 1, 'name': 'Fixture', 'paths': {'analysis_database': 'status/analysis.sqlite'}}
        (self.root / 'xport-project.json').write_text(json.dumps(self.config))
        self.database = self.root / 'status/analysis.sqlite'
        with closing(sqlite3.connect(self.database)) as db, db:
            db.executescript('''
            CREATE TABLE functions(image,address,name,sha256,status,listing,pseudocode);
            INSERT INTO functions VALUES('MAIN',2147549184,'owner','original','WIP','owner.lst',NULL);
            INSERT INTO functions VALUES('MAIN',2147549200,'sdk','library','SKIP','sdk.lst',NULL);
            INSERT INTO functions VALUES('OVERLAY',2147549184,'other','overlay','DONE','other.lst',NULL);
            CREATE TABLE library_classification(image,address,alias,reason,evidence);
            INSERT INTO library_classification VALUES('MAIN',2147549200,'InitHeap','Reviewed identity','{}');
            CREATE TABLE edges(id,source_image,source_function,site,target,kind,resolution);
            INSERT INTO edges VALUES(1,'MAIN',2147549184,1,10,'call','same_image');
            INSERT INTO edges VALUES(2,'MAIN',2147549184,2,20,'call','same_image');
            INSERT INTO edges VALUES(3,'MAIN',2147549184,3,30,'call','same_image');
            ''')
        self.source = self.root / 'src/owner.c'
        self.source.write_text('void owner(void) { FUNCTION_MARKER(0x80010000, "MAIN"); work(); }\n')
        (self.root / 'status/audit.txt').write_text('Full owner audit; no runtime claim')

    def save(self, **kwargs):
        return context.put(self.root, self.config, key='audio', kind='finding',
                           summary='Queue lifecycle checked', scope='STR/XA; exact cutscene untested',
                           evidence=['status/audit.txt'], **kwargs)

    def bind(self):
        return context.decompile(self.root, self.config, 'MAIN', 0x80010000,
                                 'src/owner.c', 'owner', ['status/audit.txt'], complete=True)

    def test_overloads_bind_by_image_qualified_marker(self):
        self.source.write_text('void owner(int) { FUNCTION_MARKER(0x80010000, "MAIN"); work(); }\n'
                               'void owner(char) { FUNCTION_MARKER(0x80010000, "OVERLAY"); other(); }\n')
        with self.assertRaises(ValueError):
            context.source_body(self.root, 'src/owner.c', 'owner')
        self.assertTrue(self.bind()['decompiled'])
        self.assertTrue(context.function_summary(self.root, self.config, 'MAIN', 0x80010000)['decompiled'])
        self.assertTrue(context.decompile(self.root, self.config, 'OVERLAY', 0x80010000,
                        'src/owner.c', 'owner', ['status/audit.txt'], complete=True)['decompiled'])
        snapshot = context.formatting_snapshot(self.root, ['src/owner.c'])
        self.assertEqual(len(snapshot['rows']), 2)
        with self.assertRaises(ValueError):
            context.source_body(self.root, 'src/owner.c', 'owner', image='MAIN', address=0x80010010)
        self.source.write_text(self.source.read_text().replace('work();', 'changed();'))
        self.assertFalse(context.function_summary(self.root, self.config, 'MAIN', 0x80010000)['decompiled'])
        self.assertTrue(context.function_summary(self.root, self.config, 'OVERLAY', 0x80010000)['decompiled'])

    def test_header_inline_binding_and_stale_body(self):
        header=self.root/'src/owner.hpp'
        header.write_text('namespace N { class Item { public:\n'
                          'bool owner() const { FUNCTION_MARKER(0x80010000, "MAIN"); return true; }\n'
                          'bool prototype(); }; }\n')
        row=context.source_body(self.root,'src/owner.hpp','N::Item::owner')
        self.assertEqual(row['start_line'],2)
        self.assertIn('const {',header.read_text()[row['start']:row['end']])
        self.assertTrue(context.decompile(self.root,self.config,'MAIN',0x80010000,
                        'src/owner.hpp','N::Item::owner',['status/audit.txt'],complete=True)['decompiled'])
        with self.assertRaises(ValueError):
            context.source_body(self.root,'src/owner.hpp','N::Item::prototype')
        header.write_text(header.read_text().replace('return true','return false'))
        self.assertFalse(context.function_summary(self.root,self.config,'MAIN',0x80010000)['decompiled'])

    def test_header_overloads_keep_image_marker_disambiguation(self):
        (self.root/'src/owner.h').write_text(
            'class Item { public: int owner(int) { FUNCTION_MARKER(0x80010000, "MAIN"); return 1; } '
            'int owner(char) { FUNCTION_MARKER(0x80010000, "OVERLAY"); return 2; } };')
        with self.assertRaises(ValueError):
            context.source_body(self.root,'src/owner.h','Item::owner')
        for image in ['MAIN','OVERLAY']:
            self.assertTrue(context.decompile(self.root,self.config,image,0x80010000,
                            'src/owner.h','Item::owner',['status/audit.txt'],complete=True)['decompiled'])
        with self.assertRaises(ValueError):
            context.source_body(self.root,'src/owner.h','Item::owner',image='MAIN',address=0x80010010)

    def test_header_sdk_and_outside_source_are_rejected(self):
        text='class Item { bool sdk() { FUNCTION_MARKER(0x80010010, "MAIN"); work(); } };'
        (self.root/'src/owner.h').write_text(text)
        with self.assertRaisesRegex(ValueError,'SDK'):
            context.decompile(self.root,self.config,'MAIN',0x80010010,
                              'src/owner.h','Item::sdk',['status/audit.txt'],complete=True)
        (self.root/'outside.h').write_text(text)
        with self.assertRaisesRegex(ValueError,'project src'):
            context.source_body(self.root,'outside.h','Item::sdk')

    def test_missing_context_read_has_no_side_effect(self):
        self.assertFalse(context.show(self.root, self.config)['initialized'])
        self.assertFalse(context.database_path(self.root, self.config).exists())

    def test_legacy_duplicate_exclusions_keep_representative(self):
        with closing(sqlite3.connect(self.database)) as db, db:
            db.execute("INSERT INTO functions VALUES('MAIN',2147549216,'copy','copysha','WIP','copy.lst',NULL)")
        document = {'schema': 1, 'groups': [{'image': 'MAIN', 'representative': 0x80010000,
                     'copies': [{'address': 0x80010000, 'sha256': 'original'},
                                {'address': 0x80010020, 'sha256': 'copysha'}]}]}
        (self.root / 'status/exclusions.json').write_text(json.dumps(document))
        self.config['analysis'] = {'decompilation_exclusions': 'status/exclusions.json'}
        result = context.queue(self.root, self.config)
        self.assertEqual(result['excluded_occurrences'], 1)
        self.assertTrue(any(row['image'] == 'MAIN' and row['address'] == 0x80010000
                            for row in result['functions']))
        self.assertFalse(any(row['address'] == 0x80010020 for row in result['functions']))

    def test_authorized_removal_filters_only_bound_identity(self):
        path = self.root / 'status/exclusions.json'
        removal = {'image': 'MAIN', 'address': 0x80010000, 'sha256': 'original',
                   'reason': 'user_removal', 'authorization': 'User explicitly removed this entry',
                   'evidence': [{'path': 'status/audit.txt',
                                 'sha256': context.digest(self.root / 'status/audit.txt')}]}
        document = {'schema': 2, 'groups': [], 'removals': [removal]}
        self.config['analysis'] = {'decompilation_exclusions': 'status/exclusions.json'}
        path.write_text(json.dumps(document))
        result = context.queue(self.root, self.config)
        self.assertEqual(result['excluded_occurrences'], 1)
        self.assertFalse(any(row['image'] == 'MAIN' and row['address'] == 0x80010000
                             for row in result['functions']))
        self.assertTrue(any(row['image'] == 'OVERLAY' and row['address'] == 0x80010000
                            for row in result['functions']))
        for field, value in [('sha256', 'stale'), ('authorization', ''), ('reason', 'candidate')]:
            with self.subTest(field=field):
                invalid = json.loads(json.dumps(document))
                invalid['removals'][0][field] = value
                path.write_text(json.dumps(invalid))
                with self.assertRaises(ValueError):
                    context.queue(self.root, self.config)
        for mutation in ['stale_evidence', 'missing_evidence', 'duplicate', 'legacy']:
            with self.subTest(mutation=mutation):
                invalid = json.loads(json.dumps(document))
                if mutation == 'stale_evidence':
                    invalid['removals'][0]['evidence'][0]['sha256'] = 'stale'
                elif mutation == 'missing_evidence':
                    invalid['removals'][0]['evidence'] = []
                elif mutation == 'duplicate':
                    invalid['removals'].append(removal)
                else:
                    invalid['schema'] = 1
                path.write_text(json.dumps(invalid))
                with self.assertRaises(ValueError):
                    context.queue(self.root, self.config)

    def test_native_file_replacement_requires_current_authority_and_all_bodies(self):
        self.assert_native_replacement_contract('native_file_replacement', 'runtime_file_access',
            '- During decompilation replace identified game file lookup')

    def test_native_save_replacement_requires_current_authority_and_all_bodies(self):
        self.assert_native_replacement_contract('native_save_replacement', 'runtime_game_save',
            'Replace card-sector I/O, card managers, formatting/directory/device checks')

    def test_native_movie_replacement_requires_current_authority_and_all_bodies(self):
        self.assert_native_replacement_contract('native_movie_replacement', 'runtime_game_movie',
            'Fullscreen movies call xport_fmv_play; in-game/background/texture movies call xport_fmv_open')

    def assert_native_replacement_contract(self, reason, kind, prefix):
        backend = self.root / 'src/backend.c'
        backend.write_text('void backend(void) { transfer(); }\n')
        contract_path = '@xport/docs/DECOMP_REFERENCE.md'
        contract = context.reference(self.root, contract_path)
        quote = next(line for line in contract.read_text(encoding='utf-8').splitlines()
                     if line.startswith(prefix))
        replacement = {
            'image': 'MAIN', 'address': 0x80010000, 'sha256': 'original',
            'reason': reason, 'reviewed': True, 'scope': 'Reviewed native adapter',
            'source': 'src/owner.c', 'symbol': 'owner',
            'source_body_sha256': context.source_body(self.root, 'src/owner.c', 'owner')['sha256'],
            'authorization': {'kind': kind, 'path': contract_path,
                              'sha256': context.digest(contract), 'quote': quote},
            'dependencies': [{'source': 'src/backend.c', 'symbol': 'backend',
                              'source_body_sha256': context.source_body(
                                  self.root, 'src/backend.c', 'backend')['sha256']}],
            'evidence': [{'path': 'status/audit.txt',
                          'sha256': context.digest(self.root / 'status/audit.txt')}],
        }
        document = {'schema': 3, 'native_replacements': [replacement]}
        path = self.root / 'status/exclusions.json'
        self.config['analysis'] = {'decompilation_exclusions': 'status/exclusions.json'}
        path.write_text(json.dumps(document))
        result = context.queue(self.root, self.config)
        self.assertEqual(result['excluded_occurrences'], 1)
        self.assertFalse(any(row['image'] == 'MAIN' and row['address'] == 0x80010000
                             for row in result['functions']))
        self.assertTrue(any(row['image'] == 'OVERLAY' and row['address'] == 0x80010000
                            for row in result['functions']))
        self.assertFalse(context.function_summary(self.root, self.config, 'MAIN', 0x80010000)['decompiled'])
        for mutation in ['legacy', 'unreviewed', 'candidate', 'scope', 'original', 'authority',
                         'authority_path', 'authority_kind', 'quote', 'unrelated_quote', 'other_contract_quote',
                         'evidence', 'missing_evidence', 'body',
                         'dependency', 'duplicate']:
            with self.subTest(mutation=mutation):
                invalid = json.loads(json.dumps(document))
                item = invalid['native_replacements'][0]
                if mutation == 'legacy':
                    invalid['schema'] = 2
                elif mutation == 'unreviewed':
                    item['reviewed'] = False
                elif mutation == 'candidate':
                    item['reason'] = 'candidate'
                elif mutation == 'scope':
                    item['scope'] = ''
                elif mutation == 'original':
                    item['sha256'] = 'stale'
                elif mutation == 'authority':
                    item['authorization']['sha256'] = 'stale'
                elif mutation == 'authority_path':
                    item['authorization']['path'] = 'status/audit.txt'
                elif mutation == 'authority_kind':
                    item['authorization']['kind'] = ('runtime_game_save'
                        if kind == 'runtime_file_access' else 'runtime_file_access')
                elif mutation == 'quote':
                    item['authorization']['quote'] = 'Unstated permission'
                elif mutation == 'unrelated_quote':
                    item['authorization']['quote'] = contract.read_text(encoding='utf-8').splitlines()[0]
                elif mutation == 'other_contract_quote':
                    other = ('Replace card-sector I/O' if kind == 'runtime_file_access'
                             else '- During decompilation replace identified game file lookup')
                    item['authorization']['quote'] = next(line for line in
                        contract.read_text(encoding='utf-8').splitlines() if line.startswith(other))
                elif mutation == 'evidence':
                    item['evidence'][0]['sha256'] = 'stale'
                elif mutation == 'missing_evidence':
                    item['evidence'] = []
                elif mutation == 'body':
                    item['source_body_sha256'] = 'stale'
                elif mutation == 'dependency':
                    item['dependencies'][0]['source_body_sha256'] = 'stale'
                else:
                    invalid['native_replacements'].append(replacement)
                path.write_text(json.dumps(invalid))
                with self.assertRaises(ValueError):
                    context.queue(self.root, self.config)

    def test_formatter_rebinds_only_current_unchanged_tokens(self):
        self.source.write_text('void owner(void) { FUNCTION_MARKER(0x80010000,"MAIN"); work(); }\n')
        self.bind()
        before = context.formatting_snapshot(self.root, ['src/owner.c'])
        self.source.write_text('void owner(void)\n{\n    FUNCTION_MARKER(0x80010000, "MAIN");\n    work();\n}\n')
        result = context.rebind_formatted(self.root, before)
        self.assertEqual(result['rebound'], 1)
        self.assertTrue(context.function_summary(self.root, self.config, 'MAIN', 0x80010000)['decompiled'])
        with closing(context.connect(self.root, self.config)) as db:
            self.assertEqual(db.execute('SELECT COUNT(*) FROM formatting_rebindings').fetchone()[0], 1)
            self.assertEqual(db.execute('SELECT COUNT(*) FROM formatting_transfers').fetchone()[0], 1)

    def test_formatter_does_not_accept_changed_or_previously_stale_code(self):
        self.bind()
        before = context.formatting_snapshot(self.root, ['src/owner.c'])
        self.source.write_text(self.source.read_text().replace('work();', 'other();'))
        self.assertEqual(context.rebind_formatted(self.root, before)['rebound'], 0)
        self.assertFalse(context.function_summary(self.root, self.config, 'MAIN', 0x80010000)['decompiled'])
        stale = context.formatting_snapshot(self.root, ['src/owner.c'])
        self.source.write_text(self.source.read_text().replace('{', '{\n'))
        self.assertEqual(context.rebind_formatted(self.root, stale)['rebound'], 0)

    def test_editorial_comparison_rejects_sensitive_spelling_and_extensions(self):
        self.assertFalse(context.editorial_equal(b'XASSERT(a+b);', b'XASSERT(a + b);'))
        self.assertFalse(context.editorial_equal(b'int n=__LINE__;', b'\nint n = __LINE__;'))
        self.assertFalse(context.editorial_equal(b'#define N 1\nint a=N;', b'#define N 2\nint a=N;'))
        self.assertFalse(context.editorial_equal(b'STR(a+b);', b'STR(a + b);', [b'#define STR(x) #x']))
        self.assertFalse(context.editorial_equal(b'STR(a+b);', b'STR(a + b);',
                                                [b'#if DEBUG\n#define STR(x) #x\n#else\n#define STR(x) (x)\n#endif']))
        self.assertFalse(context.editorial_equal(b'int n=SITE;', b'\nint n=SITE;', [b'#define SITE __LINE__']))
        self.assertFalse(context.editorial_equal(b'int n=ALIAS;', b'\nint n=ALIAS;',
                                                [b'#define SITE __LINE__\n#define ALIAS SITE']))
        with self.assertRaises(ValueError):
            context.editorial_equal(b'auto s=R"(raw)";', b'auto s = R"(raw)";')

    def test_formatter_rejects_changed_headers(self):
        self.bind()
        header = self.root / 'src/owner.hpp'
        header.write_text('#define VALUE 1\n')
        before = context.formatting_snapshot(self.root, ['src/owner.c'])
        header.write_text('#define VALUE 2\n')
        self.source.write_text(self.source.read_text().replace('{', '{\n'))
        self.assertEqual(context.rebind_formatted(self.root, before)['rebound'], 0)

    def test_source_formatter_calls_the_scoped_binding_transfer(self):
        import source_index
        self.bind()
        def format_file(*args, **kwargs):
            self.source.write_text('void owner(void)\n{\n    FUNCTION_MARKER(0x80010000, "MAIN");\n    work();\n}\n')
        with patch.object(source_index, 'find_formatter', return_value=Path('formatter')), \
             patch.object(source_index.subprocess, 'check_output', return_value='formatter fixture'), \
             patch.object(source_index.subprocess, 'run', side_effect=format_file):
            result = source_index.format_sources(self.root, ['src/owner.c'])
        self.assertEqual(result['bindings']['rebound'], 1)
        self.assertTrue(context.function_summary(self.root, self.config, 'MAIN', 0x80010000)['decompiled'])

    def test_current_record_is_small_scoped_and_hash_bound(self):
        self.save(image='MAIN', address=0x80010000, configuration='Web')
        row = context.show(self.root, self.config, image='MAIN', address=0x80010000)['records'][0]
        self.assertIn('exact cutscene untested', row['scope'])
        self.assertEqual(row['configuration'], 'Web')
        self.assertEqual(row['applicability'], 'references_unchanged')
        self.assertFalse(context.show(self.root, self.config, image='OVERLAY', address=0x80010000)['records'])
        self.source.write_text(self.source.read_text() + '\n')
        self.assertEqual(context.show(self.root, self.config)['records'][0]['applicability'], 'references_unchanged')
        (self.root / 'status/audit.txt').write_text('Changed report')
        self.assertEqual(context.show(self.root, self.config)['records'][0]['applicability'], 'needs_review')

    def test_scoped_findings_require_evidence_and_updates_do_not_duplicate(self):
        with self.assertRaisesRegex(ValueError, 'proof scope'):
            context.put(self.root, self.config, key='bad', kind='check', summary='PASS')
        self.save(); self.save()
        self.assertEqual(len(context.show(self.root, self.config)['records']), 1)
        with self.assertRaises(ValueError):
            self.save(next_step='x' * 1201)

    def test_task_scope_and_closed_questions(self):
        context.put(self.root, self.config, key='task', kind='task', summary='Translate selected image')
        context.put(self.root, self.config, key='question', kind='question', image='MAIN',
                    address=0x80010000, summary='Receiver ABI?', closed=True)
        self.assertEqual(len(context.show(self.root, self.config, image='MAIN', address=0x80010000)['records']), 1)
        self.assertEqual(len(context.show(self.root, self.config, include_closed=True)['records']), 2)

    def test_legacy_wip_and_done_do_not_promote_partial_bodies(self):
        self.assertEqual(context.queue(self.root, self.config)['counts'],
                         {'uncovered': 2, 'decompiled': 0, 'sdk': 1})
        with self.assertRaisesRegex(ValueError, 'complete-body'):
            context.decompile(self.root, self.config, 'MAIN', 0x80010000,
                              'src/owner.c', 'owner', ['status/audit.txt'])
        self.bind()
        result = context.queue(self.root, self.config)
        self.assertEqual(result['counts'], {'uncovered': 1, 'decompiled': 1, 'sdk': 1})
        self.source.write_text(self.source.read_text().replace('work();', 'changed();'))
        self.assertFalse(context.function_summary(self.root, self.config, 'MAIN', 0x80010000)['decompiled'])
        self.assertEqual(context.queue(self.root, self.config)['counts']['uncovered'], 2)

    def test_wrong_marker_and_missing_body_rejected(self):
        self.source.write_text('void owner(void) { FUNCTION_MARKER(0x80010000, "OVERLAY"); work(); }')
        with self.assertRaisesRegex(ValueError, 'image-qualified'):
            self.bind()
        self.source.write_text('void owner(void);')
        with self.assertRaisesRegex(ValueError, 'unique complete'):
            self.bind()

    def test_sql_bindings_survive_analysis_rebuild_and_feed_source_index(self):
        self.bind(); self.save()
        document = context.ledger_document(self.root)
        self.assertEqual(document['entries'][0]['semantic_name'], 'owner')
        self.assertFalse((self.root / 'status/translation-ledger.json').exists())
        with closing(sqlite3.connect(self.database)) as db, db:
            db.execute('DROP TABLE edges')
        self.assertTrue(context.function_summary(self.root, self.config, 'MAIN', 0x80010000)['decompiled'])
        self.assertEqual(context.show(self.root, self.config)['records'][0]['summary'], 'Queue lifecycle checked')
        with closing(sqlite3.connect(self.database)) as db, db:
            db.execute("UPDATE functions SET sha256='changed' WHERE image='MAIN' AND address=2147549184")
        self.assertFalse(context.function_summary(self.root, self.config, 'MAIN', 0x80010000)['decompiled'])

    def test_relationship_pages_select_fields_and_reject_changed_database(self):
        first = query.explore(self.root, self.config, 'MAIN', 0x80010000,
                              view='callees', limit=2, fields=['site', 'target'])
        self.assertNotIn('status', first['function'])
        self.assertEqual(first['callees']['rows'], [{'site': 1, 'target': 10}, {'site': 2, 'target': 20}])
        second = query.explore(self.root, self.config, 'MAIN', 0x80010000,
                               view='callees', limit=2, fields=['site', 'target'], cursor=first['callees']['next'])
        self.assertEqual(second['callees']['rows'], [{'site': 3, 'target': 30}])
        with closing(sqlite3.connect(self.database)) as db, db:
            db.execute("UPDATE functions SET name='new' WHERE image='MAIN'")
        with self.assertRaisesRegex(ValueError, 'refreshed'):
            query.explore(self.root, self.config, 'MAIN', 0x80010000,
                          view='callees', limit=2, fields=['site', 'target'], cursor=first['callees']['next'])

    def test_current_sdk_file_overrides_stale_sql_without_rebuilding(self):
        self.bind()
        self.config['paths']['sdk_classification'] = 'status/current-sdk.json'
        path = self.root / 'status/current-sdk.json'
        accepted = {'image': 'MAIN', 'address': 0x80010000, 'sha256': 'original',
                    'alias': 'NewSDK', 'reason': 'Current reviewed original identity',
                    'wrapper_status': 'Caller integration remains separate'}
        path.write_text(json.dumps({'accepted': [accepted]}))
        result = context.queue(self.root, self.config, limit=8)
        self.assertEqual(result['counts'], {'uncovered': 2, 'decompiled': 0, 'sdk': 1})
        owner = next(row for row in result['functions'] if row['image'] == 'MAIN' and
                     row['address'] == 0x80010000)
        self.assertEqual(owner['sdk']['alias'], 'NewSDK')
        self.assertNotIn('sdk', next(row for row in result['functions']
                                    if row['image'] == 'OVERLAY'))
        current = query.explore(self.root, self.config, 'MAIN', 0x80010000, view='sdk')
        self.assertEqual(current['library_classification'][0]['alias'], 'NewSDK')
        self.assertEqual(query.explore(self.root, self.config, 'MAIN', 0x80010010,
                                      view='sdk')['library_classification'], [])
        with self.assertRaisesRegex(ValueError, 'Reviewed SDK identity'):
            self.bind()
        with closing(sqlite3.connect(self.database)) as db:
            self.assertEqual(db.execute('SELECT image,address,alias FROM library_classification').fetchall(),
                             [('MAIN', 0x80010010, 'InitHeap')])
        path.write_text(json.dumps({'accepted': []}))
        self.assertEqual(context.queue(self.root, self.config)['counts']['sdk'], 0)
        self.assertTrue(self.bind()['decompiled'])

    def test_current_sdk_file_rejects_invalid_or_stale_input_without_sql_fallback(self):
        self.config['paths']['sdk_classification'] = 'status/current-sdk.json'
        path = self.root / 'status/current-sdk.json'
        accepted = {'image': 'MAIN', 'address': 0x80010000, 'sha256': 'original',
                    'alias': 'NewSDK', 'reason': 'Current review'}
        for mutation in ['missing', 'root', 'missing_list', 'not_list', 'not_entry', 'duplicate',
                         'sha', 'unknown', 'address', 'alias', 'reason']:
            with self.subTest(mutation=mutation):
                document = {'accepted': [dict(accepted)]}
                if mutation == 'root':
                    document = []
                elif mutation == 'missing_list':
                    document = {}
                elif mutation == 'not_list':
                    document['accepted'] = {}
                elif mutation == 'not_entry':
                    document['accepted'] = ['candidate']
                elif mutation == 'duplicate':
                    document['accepted'].append(dict(accepted))
                elif mutation == 'sha':
                    document['accepted'][0]['sha256'] = 'stale'
                elif mutation == 'unknown':
                    document['accepted'][0]['image'] = 'UNKNOWN'
                elif mutation == 'address':
                    document['accepted'][0]['address'] = True
                elif mutation in ('alias', 'reason'):
                    document['accepted'][0][mutation] = ''
                path.write_text(json.dumps(document))
                if mutation == 'missing':
                    path.unlink()
                for check in [lambda: context.queue(self.root, self.config), self.bind,
                              lambda: query.explore(self.root, self.config, 'MAIN',
                                                    0x80010000, view='sdk')]:
                    with self.assertRaises(ValueError):
                        check()

    def test_queue_pages_are_image_qualified(self):
        one = context.queue(self.root, self.config, limit=1)
        two = context.queue(self.root, self.config, limit=1, after=one['next'])
        self.assertNotEqual(one['functions'], two['functions'])
        self.assertEqual(context.queue(self.root, self.config, image='OVERLAY')['counts']['uncovered'], 1)

    def test_source_candidates_are_visible_without_promoting_them(self):
        with closing(sqlite3.connect(self.database)) as db, db:
            db.executescript('''CREATE TABLE implementations(image,address,source_path,symbol,mapping_status);
                INSERT INTO implementations VALUES('MAIN',2147549184,'src/owner.c','owner','mapped');''')
        result = context.function_summary(self.root, self.config, 'MAIN', 0x80010000)
        self.assertFalse(result['decompiled'])
        self.assertEqual(result['candidate_source']['source'], 'src/owner.c')
        self.assertIn('candidate_source', context.queue(self.root, self.config)['functions'][0])

    def test_sql_source_mapping_overrides_old_ledger_without_rewriting_it(self):
        legacy = self.root/'status/translation-ledger.json'
        legacy.write_text(json.dumps({'version':1,'entries':[{'image':'MAIN','address':0x80010000,
            'sha256':'original','source':'src/old.c','status':'TODO','link_only_stub':True}]}))
        original = legacy.read_bytes()
        self.bind()
        entry = context.ledger_document(self.root)['entries'][0]
        self.assertEqual(entry['source'], 'src/owner.c')
        self.assertNotIn('link_only_stub', entry)
        self.assertEqual(legacy.read_bytes(), original)

    def test_old_function_scope_is_checked_without_excluding_candidates(self):
        path = self.root/'status/scope.json'
        document = {'schema':'xport-function-scope-v1','image':'MAIN','rows':[
            {'address':'80010000','original_sha256':'original','category':'dead_code_candidate','reason':'Unobserved'}]}
        path.write_text(json.dumps(document))
        self.config['analysis'] = {'function_scope':'status/scope.json'}
        result = context.queue(self.root,self.config)
        self.assertEqual(result['counts']['uncovered'],2)
        self.assertEqual(result['functions'][0]['scope']['category'],'dead_code_candidate')
        document['rows'][0]['original_sha256'] = 'stale'
        path.write_text(json.dumps(document))
        with self.assertRaisesRegex(ValueError,'stale'):
            context.queue(self.root,self.config)

    def test_sql_only_translation_can_rebuild_the_real_source_index(self):
        import source_index
        with closing(sqlite3.connect(self.database)) as db, db:
            db.executescript('CREATE UNIQUE INDEX function_identity ON functions(image,address); '
                             'CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT);')
        self.bind()
        with patch('source_index.source_inventory', return_value=[self.source]), \
             patch('xport_project.load_project', return_value=(self.root,self.config)), \
             patch('xport_project.project_path', return_value=self.database):
            result = source_index.rebuild(format_changed=False)
        self.assertEqual(result['mapped'],1)
        self.assertFalse((self.root/'status/translation-ledger.json').exists())
        self.assertTrue(context.function_summary(self.root,self.config,'MAIN',0x80010000)['decompiled'])



if __name__ == '__main__':
    unittest.main()

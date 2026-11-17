"""Focused cache freshness, dependency and checker-reuse tests."""

from contextlib import closing
import sqlite3
from tempfile import TemporaryDirectory
from pathlib import Path
import unittest

from analysis_summaries import SummaryStore, cached_check, analysis_worklist


class SummariesTest(unittest.TestCase):
    def setUp(self):
        self.temp = TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'a.c').write_text('int first(void) { return 1; }\nint other(void) { return 2; }\n')
        (self.root / 'a.h').write_text('#define LIMIT 3\n')
        self.db = sqlite3.connect(':memory:')
        self.addCleanup(self.db.close)

    def store(self):
        return SummaryStore(self.db, self.root)

    def record(self, key='a', dependencies=()):
        store = self.store()
        return {'key': key, 'scope': 'test scope', 'claims': ['return one'], 'assumptions': [],
                'limits': 'lexical test only', 'inputs': [store.bind('a.c', 'first'), store.bind('a.h')],
                'dependencies': list(dependencies)}

    def test_body_granularity_and_warm_parser(self):
        self.store().put_many([self.record()])
        warm = self.store()
        self.assertEqual(warm.get('a')['state'], 'current')
        self.assertEqual(warm.parsed_files, 0)
        (self.root / 'a.c').write_text('int first(void) { return 1; }\nint other(void) { return 9; }\n')
        self.assertEqual(self.store().get('a')['state'], 'current')
        (self.root / 'a.c').write_text('int first(void) { return 4; }\n')
        self.assertEqual(self.store().get('a')['state'], 'stale')

    def test_transitive_invalidation(self):
        self.store().put_many([self.record('c', ['b']), self.record('b', ['a']), self.record('a')])
        (self.root / 'a.h').write_text('#define LIMIT 4\n')
        result = self.store().get('c')
        self.assertEqual(result['state'], 'stale')
        self.assertTrue(any('dependency' in reason for reason in result['reasons']))

    def test_worklist_reuses_findings_and_defers_unchanged_frontiers(self):
        self.store().put_many([self.record('finding'), self.record('frontier')])
        jobs = [{'key': 'known', 'result': 'finding'},
                {'key': 'blocked', 'result': 'frontier', 'disposition': 'open-frontier'},
                {'key': 'new', 'result': 'missing', 'priority': 10}]
        result = analysis_worklist(self.store(), jobs)
        self.assertEqual(result['next'], 'new')
        self.assertEqual(result['counts'], {'ready': 1, 'waiting': 0, 'reuse-finding': 1,
                                           'deferred-unchanged': 1})
        self.assertEqual(result['jobs'][1]['limits'], 'lexical test only')
        (self.root / 'a.h').write_text('#define LIMIT 4\n')
        result = analysis_worklist(self.store(), jobs)
        self.assertEqual(result['counts']['ready'], 3)

    def test_worklist_transitive_changes_reopen_only_affected_checks(self):
        self.store().put_many([self.record('a'), self.record('child', ['a'])])
        jobs = [{'key': 'child', 'result': 'child'}]
        (self.root / 'a.c').write_text('int first(void) { return 1; }\nint other(void) { return 9; }\n')
        self.assertEqual(analysis_worklist(self.store(), jobs)['counts']['reuse-finding'], 1)
        changed = self.record('a')
        changed['claims'] = ['new contract']
        self.store().put_many([changed])
        self.assertEqual(analysis_worklist(self.store(), jobs)['next'], 'child')

    def test_worklist_waits_for_missing_or_stale_prerequisites(self):
        self.store().put_many([self.record('a')])
        jobs = [{'key': 'later', 'result': 'not-yet', 'requires': ['a', 'missing']}]
        result = analysis_worklist(self.store(), jobs)
        self.assertIsNone(result['next'])
        self.assertEqual(result['jobs'][0]['waiting_on'], ['missing'])
        (self.root / 'a.h').write_text('#define LIMIT 4\n')
        self.assertEqual(analysis_worklist(self.store(), jobs)['jobs'][0]['waiting_on'], ['a', 'missing'])

    def test_worklist_rejects_invalid_jobs_and_orders_deterministically(self):
        job = {'key': 'a', 'result': 'a'}
        for jobs in ({}, [job, job], [dict(job, requires=['a'])],
                     [dict(job, requires='b')], [dict(job, disposition='accepted')],
                     [dict(job, priority=True)], [dict(job, requires=[{}])]):
            with self.subTest(jobs=jobs), self.assertRaises(ValueError):
                analysis_worklist(self.store(), jobs)
        result = analysis_worklist(self.store(), [dict(job, key='b'), job])
        self.assertEqual(result['ready'], ['a', 'b'])

    def test_dependency_revision_and_atomic_rejection(self):
        store = self.store()
        original = store.put_many([self.record()])[0]
        changed = self.record()
        changed['claims'] = ['different claim']
        child = self.record('child', ['a'])
        store.put_many([child])
        store.put_many([changed])
        self.assertEqual(store.get('child')['state'], 'stale')
        child['dependencies'] = {'a': original['revision']}
        with self.assertRaises(ValueError):
            store.put_many([self.record('new'), child])
        self.assertEqual(store.get('new')['state'], 'missing')

    def test_cycles_missing_and_wrong_inputs(self):
        for records in ([self.record('a', ['b']), self.record('b', ['a'])],
                        [self.record('a', ['absent'])]):
            with self.assertRaises(ValueError):
                self.store().put_many(records)
        record = self.record()
        record['inputs'][0]['sha256'] = 'bad'
        with self.assertRaises(ValueError):
            self.store().put_many([record])
        self.assertEqual(self.store().get('a')['state'], 'missing')

    def test_missing_and_ambiguous_functions(self):
        self.store().put_many([self.record()])
        for source in ('int other(void) {return 1;}',
                       'int first(void) {return 1;}\nint first(void) {return 2;}'):
            (self.root / 'a.c').write_text(source)
            self.assertEqual(self.store().get('a')['state'], 'stale')
        (self.root / 'a.c').unlink()
        self.assertEqual(self.store().get('a')['state'], 'stale')

    def test_index_rebuild_preserves_summaries(self):
        from index_access import refresh
        (self.root / 'src').mkdir()
        (self.root / 'src/a.c').write_text('int f(void) {return 0;}')
        self.store().put_many([self.record()])
        self.db.commit()
        refresh(self.db, self.root, rebuild=True)
        self.assertEqual(self.store().get('a')['state'], 'current')

    def test_closed_world_tree_detects_new_callers_and_headers(self):
        record = self.record()
        record['inputs'].append(self.store().bind_tree('.'))
        self.store().put_many([record])
        (self.root / 'notes.txt').write_text('unrelated non-source file')
        self.assertEqual(self.store().get('a')['state'], 'current')
        added = self.root / 'new.c'
        added.write_text('int new_caller(void) { return first(); }')
        self.assertEqual(self.store().get('a')['state'], 'stale')
        added.unlink()
        self.assertEqual(self.store().get('a')['state'], 'current')
        added = self.root / 'new.h'
        added.write_text('#define new_alias first')
        self.assertEqual(self.store().get('a')['state'], 'stale')
        added.unlink()
        added = self.root / 'new.asm'
        added.write_text('call first')
        self.assertEqual(self.store().get('a')['state'], 'stale')

    def test_tree_missing_or_ambiguous_kind(self):
        for item in ({'path': 'absent', 'tree': True}, {'path': '.', 'tree': True, 'symbol': 'first'}):
            with self.assertRaises(ValueError):
                self.store().fingerprint(item)

    def test_cached_checker_and_rejected_output(self):
        calls = []
        def run():
            calls.append(1)
            return {'passed': True}
        def validate(output):
            if not output['passed']:
                raise ValueError('checker failed')
        contract = {k: v for k, v in self.record().items() if k not in ('key', 'inputs', 'dependencies')}
        def check():
            return cached_check(self.root, 'check', [{'path': 'a.h'}], contract, run, validate)
        self.assertFalse(check()[1]['hit'])
        self.assertTrue(check()[1]['hit'])
        self.assertEqual(len(calls), 1)
        (self.root / 'a.h').write_text('new header')
        self.assertFalse(check()[1]['hit'])
        with self.assertRaises(ValueError):
            cached_check(self.root, 'bad', [{'path': 'a.h'}], contract,
                         lambda: {'passed': False}, validate)
        with closing(sqlite3.connect(self.root / 'status/refactoring/memory-access.sqlite')) as db:
            self.assertEqual(SummaryStore(db, self.root).get('bad')['state'], 'missing')


if __name__ == '__main__':
    unittest.main()

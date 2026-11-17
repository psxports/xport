"""Regression checks for the memory access index."""

from argparse import Namespace
from contextlib import redirect_stdout
from io import StringIO
import json
from pathlib import Path
import sqlite3
import sys
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch

import index_access as index


class ScratchAccessIndexTest(unittest.TestCase):
    def setUp(self):
        self.db = sqlite3.connect(':memory:')
        self.addCleanup(self.db.close)
        self.db.executescript(index.SCHEMA)

    def load(self, source):
        sites, ingress, calls, definitions = index.summarize(source, 'src/example.c')
        self.db.executemany('INSERT INTO sites VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)', sites)
        self.db.executemany('INSERT INTO ingress VALUES (?, ?, ?, ?, ?, ?, ?, ?)', ingress)
        self.db.executemany('INSERT INTO calls VALUES (?, ?, ?, ?)', calls)
        self.db.executemany('INSERT INTO definitions VALUES (?, ?)', definitions)

    def query(self, offset, size=4, **options):
        args = {'offset': offset, 'size': size, 'limit': 100, 'root': None, 'function': None,
                'unresolved': False, 'reads': False, 'reads_before_write': False}
        return index.query(self.db, Namespace(**(args | options)))

    def test_byte_overlap_and_prefix_order(self):
        self.load('''
void sample(void)
{
    w_u16(0x9f800184, 0);
    uint32 all = r_u32(0x1f800184);
    uint16 low = r_u16(0x1f800184);
    if (all)
        w_u16(0x1f800186, 0);
    uint16 high = r_u16(0x1f800186);
}
''')
        self.assertEqual(
            [site['order'] for site in self.query(0x184, reads=True)['exact_spans']],
            ['no_prior_exact_write_for_some_bytes', 'covered_by_prior_exact_writes', 'unresolved'],
        )
        reads = self.query(0x186, 2, reads=True, reads_before_write=True)['exact_spans']
        self.assertEqual([site['operation'] for site in reads], ['r_u32', 'r_u16'])

    def test_indirect_address_through_two_named_calls(self):
        self.load('''
void sink(uint32 state) { w_u32(state + 0x164, 0); }
void relay(uint32 context) { sink(context + 4); }
void render(void) { relay(0x1f80001c); }
void other(void) { w_u32(0x1f800184, 0); }
''')
        result = self.query(0x184, root='render')
        self.assertFalse(result['exact_spans'])
        candidate, = result['call_candidates']
        self.assertEqual(candidate['lo'], index.BASE + 0x184)
        self.assertEqual([edge['caller'] for edge in candidate['call_chain']], ['render', 'relay'])

    def test_mutated_parameters_and_pointer_writes_stay_unresolved(self):
        self.load('''
void scan(uint32 context, uint32 offset)
{
    ++context;
    w_u32(context + 12, 0);
    memcpy(ptr, source, offset);
    /* w_u32(0x1f800184, 0); */
}
void run(void) { scan(0x1f800178, 4); }
''')
        result = self.query(0x184, root='run')
        self.assertFalse(result['exact_spans'])
        self.assertFalse(result['call_candidates'])
        unresolved = self.query(0, unresolved=True, function='scan')['unresolved']
        self.assertEqual([site['expression'] for site in unresolved], ['context + 12', 'ptr', 'source'])

    def test_dram_mirrors_and_writer_dependencies_outside_reader(self):
        self.load('''
void produce(void) { w_u16(0xa0210202, 0); }
void receive(void) { uint32 value = r_u32(0x80010200); }
void sink(uint32 base) { w_u8(base + 1, 0); }
void relay(uint32 base) { uint32 local = base + 2; sink(local); }
void run(void) { relay(0x800101fe); }
''')
        args = Namespace(limit=10, root=None, function='receive', unresolved=False, reads=True,
                         reads_before_write=False, dependencies=True)
        lo, hi = index.address_range(0x80010200, 4)
        result = index.query(self.db, args, lo=lo, hi=hi)
        dependency, = result['read_dependencies']
        self.assertEqual([edge['overlap'] for edge in dependency['overlapping_writers']],
                         [[0x10202, 0x10204], [0x10201, 0x10202]])
        indirect = dependency['overlapping_writers'][1]['writer']
        self.assertEqual([edge['caller'] for edge in indirect['call_chain']], ['run', 'relay'])

    def test_bounded_copy_and_masked_write_lanes(self):
        self.load('''
void sample(void) {
    uint32 address = 0x80010200;
    memcpy(psx_addr(address, 8), psx_addr(address + 8, 8), 4);
    w_masked_u32(address + 4, 0, 5);
    w_masked_u32(address + 12, 0, get_mask());
}
''')
        args = Namespace(limit=10, root=None, function=None, unresolved=False, reads=False,
                         writes=True, reads_before_write=False)
        result = index.query(self.db, args, lo=0x10200, hi=0x10210)
        self.assertEqual([(s['lo'], s['hi']) for s in result['exact_spans']],
                         [(0x10200, 0x10204), (0x10204, 0x10205), (0x10206, 0x10207)])
        self.assertEqual(result['unresolved_count'], 1)

    def test_indirect_cycles_are_bounded_and_ambiguous_symbols_are_not_bound(self):
        self.load('''
void step(uint32 base) { w_u8(base, 0); step(base + 1); }
void duplicate(uint32 base) { w_u8(base, 0); }
void duplicate(uint32 base) { w_u8(base + 1, 0); }
void run(void) { step(0x1f800100); duplicate(0x1f800100); }
''')
        result = self.query(0x100, 16)
        self.assertTrue(result['call_search_truncated'])
        self.assertEqual(len(result['call_candidates']), 8)
        self.assertEqual({site['symbol'] for site in result['call_candidates']}, {'step'})

    def test_refresh_reuses_unchanged_files_and_purges_deleted_files(self):
        with TemporaryDirectory() as temp:
            src = Path(temp) / 'src'
            src.mkdir()
            changed = src / 'changed.c'
            removed = src / 'removed.c'
            changed.write_text('void updated(void) { w_u32(0x1f800184, 0); }', encoding='utf-8')
            removed.write_text('void stale(void) { w_u32(0x1f800184, 0); }', encoding='utf-8')
            self.assertEqual(index.refresh(self.db, Path(temp)), {'indexed_files': 2, 'reused_files': 0})
            changed.write_text('void updated(void) { w_u32(0x1f800188, 0); }', encoding='utf-8')
            self.assertEqual(index.refresh(self.db, Path(temp)), {'indexed_files': 1, 'reused_files': 1})
            removed.unlink()
            self.assertEqual(index.refresh(self.db, Path(temp)), {'indexed_files': 0, 'reused_files': 1})
            self.assertEqual(self.query(0x184)['exact_spans_count'], 0)
            self.assertEqual(self.query(0x188)['exact_spans_count'], 1)

    def test_parameter_bound_masked_write_only_covers_active_lanes(self):
        self.load('''
void sink(uint32 address) { w_masked_u32(address, 0, 5); }
void valid(void) { sink(0x1f800184); }
void too_wide(void) { sink(0x1f8003ff); }
''')
        result = self.query(0, 1024, writes=True)
        self.assertEqual([(s['lo'], s['hi']) for s in result['call_candidates']],
                         [(index.BASE + 0x184, index.BASE + 0x185), (index.BASE + 0x186, index.BASE + 0x187)])

    def test_call_cache_reused_and_invalidated_by_changed_or_removed_caller(self):
        with TemporaryDirectory() as temp:
            src = Path(temp) / 'src'
            src.mkdir()
            (src / 'sink.c').write_text('void sink(uint32 base) { w_u8(base + 1, 0); }', encoding='utf-8')
            caller = src / 'caller.c'
            caller.write_text('void caller(void) { sink(0x80010200); }', encoding='utf-8')
            self.assertEqual(index.refresh(self.db, Path(temp), cache_calls=True)['call_cache'], 'rebuilt')
            self.assertIn(0x80010200, index.read_call_cache(self.db)[0]['sink', 0])
            with patch.object(index, 'bound_parameters', side_effect=AssertionError('cache missed')):
                self.assertEqual(index.refresh(self.db, Path(temp), cache_calls=True)['call_cache'], 'reused')
                self.assertIn(0x80010200, index.read_call_cache(self.db)[0]['sink', 0])
            caller.write_text('void caller(void) { sink(0x80010300); }', encoding='utf-8')
            index.refresh(self.db, Path(temp))
            state = index.refresh(self.db, Path(temp), cache_calls=True)
            self.assertEqual((state['indexed_files'], state['call_cache']), (0, 'rebuilt'))
            self.assertEqual(list(index.read_call_cache(self.db)[0]['sink', 0]), [0x80010300])
            caller.unlink()
            self.assertEqual(index.refresh(self.db, Path(temp), cache_calls=True)['call_cache'], 'rebuilt')
            self.assertFalse(index.read_call_cache(self.db)[0])

    def test_cli_batches_functions_and_reuses_writer_lookup(self):
        with TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'src').mkdir()
            (root / 'src/example.c').write_text('''
void write_value(uint32 base) { w_u32(base, 0); }
void run(void) { write_value(0x80010200); }
void one(void) { r_u8(0x80010200); }
void two(void) { r_u16(0x80010202); }
''', encoding='utf-8')
            argv = ['index', '0x80010200', '4', '--function', 'one', '--function', 'two',
                    '--dependencies', '--limit', '1']
            output = StringIO()
            with patch.object(index, 'load_project', return_value=(root, {})), \
                    patch.object(sys, 'argv', argv), redirect_stdout(output), \
                    patch.object(index, 'dependency_writers', wraps=index.dependency_writers) as writers:
                index.main()
                self.assertEqual(writers.call_count, 1)
            reports = json.loads(output.getvalue())['functions']
            for name, span in [('one', [0x10200, 0x10201]), ('two', [0x10202, 0x10204])]:
                dependency, = reports[name]['read_dependencies']
                match, = dependency['overlapping_writers']
                self.assertEqual(match['overlap'], span)
                self.assertEqual(match['writer']['symbol'], 'write_value')

    def test_triage_splits_partial_coverage_and_keeps_cross_function_writers(self):
        self.load('''
void receive(uint32 unknown) { r_u32(0x80010200); r_u8(unknown); }
void first(void) { w_u16(0xa0210200, 0); }
void second(uint32 base) { w_u16(base + 1, 0); }
void entry(void) { second(0x80010200); }
void only_write(void) { w_u8(0x80010204, 0); }
''')
        args = Namespace(function=[], limit=5)
        ranges = [(0x10200, 0x10205)]
        bindings = index.bound_parameters(self.db, None)
        report = index.triage(self.db, args, ranges, None, bindings)
        region, = report['ranges']
        self.assertEqual(set(region['byte_counts'].values()), {1, 2})
        self.assertEqual(region['byte_counts']['one_writer_function'], 2)
        multiple, = region['regions']['multiple_writer_functions']
        self.assertEqual((multiple['lo'], multiple['hi']), (0x10201, 0x10202))
        self.assertEqual([(w['symbol'], w['exact'], w['via_call']) for w in multiple['writers']],
                         [('first', 1, 0), ('second', 0, 1)])
        unknown, = region['regions']['reads_without_indexed_writer']
        self.assertEqual((unknown['lo'], unknown['hi']), (0x10203, 0x10204))
        self.assertEqual([(g['symbol'], g['direction'], g['count'], g['without_candidate'])
                          for g in report['unresolved_by_function']['groups']],
                         [('receive', 'r', 1, 1), ('second', 'w', 1, 0)])
        focused = index.triage(self.db, Namespace(function=['receive'], limit=0), ranges, None, bindings)
        focused_region, = focused['ranges']
        self.assertEqual(focused_region['region_counts']['multiple_writer_functions'], 1)
        self.assertEqual(focused_region['region_counts']['writes_only'], 0)
        self.assertFalse(any(focused_region['regions'].values()))

    def test_cli_triage_writes_a_replaceable_report_for_both_memory_regions(self):
        with TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'src').mkdir()
            (root / 'src/sample.c').write_text('void sample(void) { r_u8(0x1f800010); }', encoding='utf-8')
            report = root / 'reports' / 'memory.json'
            argv = ['index', '--triage', '--limit', '0', '--output', str(report)]
            with patch.object(index, 'load_project', return_value=(root, {})), \
                    patch.object(sys, 'argv', argv), redirect_stdout(StringIO()) as stdout:
                index.main()
                initial = json.loads(report.read_text(encoding='utf-8'))
                index.main()
                current = json.loads(report.read_text(encoding='utf-8'))
            self.assertEqual(stdout.getvalue(), '')
            self.assertEqual(current['call_cache'], 'reused')
            self.assertEqual(current['source_sha256'], initial['source_sha256'])
            self.assertEqual([r['address_range'] for r in current['ranges']],
                             [[0, index.DRAM_SIZE], [index.BASE, index.END]])
            self.assertEqual(current['ranges'][1]['byte_counts']['reads_without_indexed_writer'], 1)
            self.assertEqual(list(report.parent.iterdir()), [report])


if __name__ == '__main__':
    unittest.main()

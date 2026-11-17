import sqlite3
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace

import index_access as access


class UnknownExtentCallTests(unittest.TestCase):
    def query(self, expression, seed='0x1F800020', reads=False, writes=False):
        source = f'''
void leaf(uint32 scratch, const void *input) {{ {expression}; }}
void forward(uint32 scratch) {{ leaf(scratch + 8, 0); }}
void entry(void) {{ forward({seed}); }}
'''
        with sqlite3.connect(':memory:') as db:
            db.executescript(access.SCHEMA)
            sites, ingress, calls, definitions = access.summarize(source, 'src/fixture.c')
            db.executemany('INSERT INTO sites VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)', sites)
            db.executemany('INSERT INTO ingress VALUES (?, ?, ?, ?, ?, ?, ?, ?)', ingress)
            db.executemany('INSERT INTO calls VALUES (?, ?, ?, ?)', calls)
            db.executemany('INSERT INTO definitions VALUES (?, ?)', definitions)
            args = SimpleNamespace(root=None, unresolved=False, reads=reads, writes=writes,
                                   function=None, reads_before_write=False, dependencies=True, limit=100)
            return access.query(db, args, lo=access.BASE, hi=access.END)

    def test_unknown_size_retains_two_call_address_chain_without_span(self):
        report = self.query('psx_addr(scratch + 16, sizeof(MATRIX))')
        self.assertEqual(report['exact_spans_count'], 0)
        self.assertEqual(report['call_candidates_count'], 0)
        self.assertEqual(report['call_anchors_count'], 1)
        anchor = report['call_anchors'][0]
        self.assertEqual(anchor['lo'], access.BASE + 56)
        self.assertEqual(anchor['extent'], 'unknown')
        self.assertNotIn('hi', anchor)
        self.assertTrue(anchor['call_chain'])
        self.assertEqual(report['read_dependencies'], [])
        self.assertGreater(report['unresolved_count'], 0)

    def test_known_size_keeps_existing_span_and_dependency_behavior(self):
        report = self.query('w_u32(scratch + 16, 0)', writes=True)
        self.assertEqual(report['call_candidates_count'], 1)
        self.assertEqual(report['call_anchors_count'], 0)
        self.assertEqual(report['call_candidates'][0]['lo'], access.BASE + 56)
        self.assertEqual(report['call_candidates'][0]['hi'], access.BASE + 60)

    def test_unknown_copy_and_view_have_separate_unbounded_anchors(self):
        report = self.query('memcpy(psx_addr(scratch + 16, sizeof(MATRIX)), input, sizeof(MATRIX))')
        self.assertEqual(report['call_candidates_count'], 0)
        self.assertEqual(report['call_anchors_count'], 2)
        self.assertEqual({x['operation'] for x in report['call_anchors']}, {'memcpy', 'psx_addr'})

    def test_unknown_loan_capacity_cannot_prove_fixed_copy_span(self):
        report = self.query('memcpy(psx_addr(scratch + 16, sizeof(MATRIX)), input, 4)', writes=True)
        self.assertEqual(report['call_candidates_count'], 0)
        self.assertEqual(report['call_anchors_count'], 1)
        self.assertEqual(report['call_anchors'][0]['direction'], 'w')

    def test_unknown_writer_anchor_cannot_cover_a_known_read(self):
        report = self.query('memcpy(psx_addr(scratch + 16, sizeof(MATRIX)), input, sizeof(MATRIX)); r_u32(scratch + 16)')
        self.assertEqual(report['call_candidates_count'], 1)
        self.assertEqual(report['read_dependencies'][0]['overlapping_writer_count'], 0)
        self.assertGreater(report['unresolved_writer_count'], 0)
        self.assertEqual(report['call_anchors_count'], 2)

    def test_known_bounded_copy_preserves_exact_write_candidate(self):
        report = self.query('memcpy(psx_addr(scratch + 16, 4), input, 4)', writes=True)
        self.assertEqual(report['call_candidates_count'], 1)
        self.assertEqual(report['call_candidates'][0]['hi'], access.BASE + 60)
        self.assertEqual(report['call_anchors_count'], 0)

    def test_dynamic_displacement_does_not_acquire_a_guessed_anchor(self):
        report = self.query('psx_addr(scratch + unknown_index * 4, sizeof(MATRIX))')
        self.assertEqual(report['call_candidates_count'], 0)
        self.assertEqual(report['call_anchors_count'], 0)

    def test_known_out_of_region_span_is_not_an_unknown_extent_anchor(self):
        report = self.query('psx_addr(scratch + 16, 4)', seed='0x1F8003F0')
        self.assertEqual(report['call_candidates_count'], 0)
        self.assertEqual(report['call_anchors_count'], 0)


class ScalarCopyExtentTests(unittest.TestCase):
    def copies(self, body, source='src/fixture.c'):
        sites, _, _, _ = access.summarize(body, source)
        return [row for row in sites if row[3] == 'memcpy']

    def test_native_pointer_has_known_bytes_without_guest_span(self):
        rows = self.copies('void f(void *dst,const void *src){uint16 word;memcpy(dst,src,sizeof(word));}')
        self.assertEqual([row[8] for row in rows], [2, 2])
        self.assertTrue(all(row[6] is None and row[7] is None for row in rows))

    def test_native_literal_copy_width_is_not_discarded(self):
        rows = self.copies('void f(void *dst,const void *src){memcpy(dst,src,12);}')
        self.assertEqual([row[8] for row in rows], [12, 12])
        self.assertTrue(all(row[7] is None for row in rows))

    def test_rewritten_read_and_write_expressions_keep_their_width(self):
        source = '''void f(void *dst,const void *src){
uint32 value; uint16 half;
(memcpy(&value,src,sizeof(value)),value);
(half=(uint16)(uint32)(7),(void)memcpy(dst,&half,sizeof(half)));
}'''
        self.assertEqual([row[8] for row in self.copies(source)], [4, 4, 2, 2])

    def test_literal_arithmetic_and_bare_sizeof(self):
        rows = self.copies('void f(void*d,void*s){sint32 word;memcpy(d,s,2*sizeof word);}')
        self.assertEqual([row[8] for row in rows], [8, 8])

    def test_late_debug_branch_does_not_change_unconditional_object_type(self):
        source = 'void f(void*d,void*s){uint32 word;\n#ifndef NDEBUG\nputs("debug");\n#endif\nmemcpy(d,s,sizeof(word));}'
        self.assertEqual([row[8] for row in self.copies(source)], [4, 4])

    def test_unknown_mapper_capacity_stays_unbounded(self):
        source = 'void f(uint32 address,void*s){uint32 word;memcpy(psx_addr(address,sizeof(MATRIX)),s,sizeof(word));}'
        write, read = self.copies(source)
        self.assertIsNone(write[8])
        self.assertIsNone(write[7])
        self.assertEqual(read[8], 4)

    def test_same_scalar_size_capacity_proves_only_the_bounded_mapper_side(self):
        source = 'void f(void*s){uint32 word;memcpy(psx_addr(0x1F800020,sizeof(word)),s,sizeof(word));}'
        write, read = self.copies(source)
        self.assertEqual(write[6:9], (access.BASE + 32, access.BASE + 36, 4))
        self.assertEqual(read[8], 4)
        self.assertIsNone(read[7])

    def test_numeric_native_pointer_is_not_a_proved_guest_copy(self):
        rows = self.copies('void f(void*s){uint32 word;memcpy(0x1F800020,s,sizeof(word));}')
        self.assertEqual(rows[0][8], 4)
        self.assertIsNone(rows[0][7])

    def test_shadowing_and_unsupported_objects_stay_unknown(self):
        bad = ['uint16 word;{uint32 word;memcpy(d,s,sizeof(word));}',
               'uint16 word;{OTHER_TYPE *word;memcpy(d,s,sizeof(word));}',
               'uint16 word;{void (*word)(void);memcpy(d,s,sizeof(word));}',
               'uint16 word;{typedef uint64 word;memcpy(d,s,sizeof(word));}',
               'uint16 word;{enum {word=1};memcpy(d,s,sizeof(word));}',
               'uint16 word;{uint32 word[3];memcpy(d,s,sizeof(word));}',
               'uint32 *word;memcpy(d,s,sizeof(word));',
               'uint32 word[2];memcpy(d,s,sizeof(word));',
               'uint32 first,word;memcpy(d,s,sizeof(word));',
               'OTHER_TYPE word;memcpy(d,s,sizeof(word));',
               'uint32 word;DECLARE(word);memcpy(d,s,sizeof(word));',
               'memcpy(d,s,sizeof(uint32));']
        for body in bad:
            with self.subTest(body=body):
                self.assertTrue(all(row[8] is None for row in self.copies('void f(void*d,void*s){' + body + '}')))

    def test_redefinitions_and_cpp_stay_unknown(self):
        for define in ('#define word other\n', '#define uint16 uint64\n', '#define sizeof(x) 99\n'):
            source = define + 'void f(void*d,void*s){uint16 word;memcpy(d,s,sizeof(word));}'
            self.assertTrue(all(row[8] is None for row in self.copies(source)))
        source = 'void f(void*d,void*s){uint16 word;memcpy(d,s,sizeof(word));}'
        self.assertTrue(all(row[8] is None for row in self.copies(source, 'src/fixture.cpp')))


class NativeFieldTests(unittest.TestCase):
    def query(self, source, field, **options):
        with sqlite3.connect(':memory:') as db:
            db.executescript(access.SCHEMA)
            db.execute('INSERT INTO files VALUES (?,?)', ('src/fixture.c', 'input-sha'))
            db.executemany('INSERT INTO native_members VALUES (?,?,?,?,?,?)',
                           access.native_members(source, 'src/fixture.c'))
            _, _, calls, _ = access.summarize(source, 'src/fixture.c')
            db.executemany('INSERT INTO calls VALUES (?,?,?,?)', calls)
            return access.native_field_query(db, [field], **options)['native_fields'][0]

    def test_direct_write_read_compound_and_equality(self):
        result = self.query('''void entry(void){
            state.phase.index = 2;
            state.phase.index += 1;
            ++state.phase.index;
            if(state.phase.index == 4) sink(state.phase.index);
        }''', 'state.phase.index')
        self.assertEqual(result['direction_counts'], {'write': 1, 'read_write': 2, 'read': 2})
        self.assertTrue(all(row['relation'] == 'exact' for row in result['sites']))

    def test_ancestor_loans_and_whole_root_assignment_are_retained(self):
        result = self.query('''void f(void){
            state = initial;
            clear(&state);
            fill(&state.phase);
            use(state.phase.index);
            use(state.phase.index_extra);
        }''', 'state.phase.index', writes=True)
        self.assertEqual(result['direction_counts'], {'write': 1, 'address_candidate': 2})
        self.assertEqual([row['field'] for row in result['sites']], ['state', 'state', 'state.phase'])
        self.assertTrue(all(row['relation'] == 'ancestor' for row in result['sites']))

    def test_comments_strings_arrow_and_indexed_access(self):
        result = self.query('''void f(void){
            /* state->phase = 1; */ sink("state->phase = 2;");
            state -> phase = 3;
            state->phases[0] = 4;
        }''', 'state', dependencies=True)
        self.assertEqual(result['direction_counts'], {'write': 1, 'indexed_candidate': 1})
        self.assertEqual(result['sites'][0]['field'], 'state->phase')
        self.assertEqual(result['sites'][0]['source_sha256'], 'input-sha')
        self.assertEqual(len(result['sites'][0]['body_sha256']), 64)

    def test_incoming_calls_limits_and_invalid_paths(self):
        source = 'void leaf(void){state.phase=1;} void entry(void){leaf();}'
        result = self.query(source, 'state.phase', dependencies=True, limit=0)
        self.assertEqual(result['site_count'], 1)
        self.assertEqual(result['incoming_call_count'], 1)
        self.assertEqual(result['sites'], [])
        self.assertEqual(result['incoming_calls'], [])
        result = self.query(source, 'state.phase', dependencies=True)
        self.assertEqual(result['incoming_calls'][0]['caller'], 'entry')
        with self.assertRaises(ValueError):
            self.query(source, 'state.phase[0]')

    def test_function_hash_uses_same_newline_domain_as_source_index(self):
        text = 'void f(void){\nstate.phase=1;\n}\n'
        lf = access.native_members(text, 'src/fixture.c')
        crlf = access.native_members(text.replace('\n', '\r\n'), 'src/fixture.c')
        self.assertEqual(lf, crlf)

    def test_cache_reuses_then_replaces_and_deletes_native_sites(self):
        with TemporaryDirectory() as directory, sqlite3.connect(':memory:') as db:
            root = Path(directory)
            source = root / 'src/fixture.c'
            source.parent.mkdir()
            source.write_text('void f(void){state.phase=1;}')
            first = access.refresh(db, root)
            self.assertEqual(first['indexed_files'], 1)
            self.assertEqual(access.refresh(db, root)['reused_files'], 1)
            source.write_text('void f(void){state.other=2;}')
            self.assertEqual(access.refresh(db, root)['indexed_files'], 1)
            self.assertEqual(access.native_field_query(db, ['state.phase'])['native_fields'][0]['site_count'], 0)
            self.assertEqual(access.native_field_query(db, ['state.other'])['native_fields'][0]['site_count'], 1)
            source.unlink()
            access.refresh(db, root)
            self.assertEqual(db.execute('SELECT count(*) FROM native_members').fetchone()[0], 0)


if __name__ == '__main__':
    unittest.main()

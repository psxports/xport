"""preserve parser results and caller isolation across memoized source reads"""

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import source_index as index


class ParseCacheTests(unittest.TestCase):
    def setUp(self):
        index._cached_functions.cache_clear()

    def test_uncached_equivalence(self):
        examples = ('void f(void) { return; }',
                    '/* braces { } */ void f(void) { const char *s = "}"; }',
                    'void f(void) { FUNCTION_MARKER(0x80010000, "ACE3.BIN"); }',
                    '#if X\nvoid f(void) {}\n#else\nvoid g(void) {}\n#endif',
                    'void Object::Draw(int x) const { draw(x); }')
        for text in examples:
            self.assertEqual(index.functions(text, 'test.c'), index._parse_functions(text, 'test.c'))

    def test_nested_results_are_independent(self):
        text = 'void f(void) { FUNCTION_MARKER(0x80010000, "ACE3.BIN"); }'
        expected = index.functions(text, 'test.c')
        modified = index.functions(text, 'test.c')
        modified[0]['name'] = 'bad'
        modified[0]['markers'].clear()
        modified[0]['identifiers'].append('bad')
        modified.clear()
        self.assertEqual(index.functions(text, 'test.c'), expected)
        self.assertGreater(index._cached_functions.cache_info().hits, 0)

    def test_changed_text_and_path_do_not_reuse_rows(self):
        first = index.functions('void f(void) {}', 'a.c')
        changed = index.functions('void f(void) { tick(); }', 'a.c')
        other = index.functions('void f(void) {}', 'b.c')
        self.assertNotEqual(first[0]['sha256'], changed[0]['sha256'])
        self.assertEqual(other[0]['path'], 'b.c')
        for n in range(20):
            index.functions('void f(void) {}', str(n) + '.c')
        self.assertLessEqual(index._cached_functions.cache_info().currsize, 16)


if __name__ == '__main__':
    unittest.main()

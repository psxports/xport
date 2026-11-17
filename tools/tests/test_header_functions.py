import hashlib
import unittest
from source_index import declarations, header_functions, source_functions


class HeaderFunctionsTests(unittest.TestCase):
    def test_declarations_in_transparent_c_linkage_block(self):
        text = 'extern "C" {\nextern int global;\nvoid Run(void);\n}\n'
        rows = declarations(text, 'src/test.h', True)
        self.assertEqual([(r['name'], r['kind']) for r in rows], [('global', 'object'), ('Run', 'prototype')])
        for row in rows:
            self.assertEqual(row['sha256'], hashlib.sha256(text[row['start']:row['end']].encode()).hexdigest())

    def test_scopes_overloads_and_raw_ranges(self):
        text = ('namespace N { struct Outer { public:\r\n'
                'int f(int x) const { FUNCTION_MARKER(0x80010000, "MAIN"); return x; }\r\n'
                'private: struct Inner { int f() { return 2; } };\r\n'
                'int f(char x) { FUNCTION_MARKER(0x80010004, "MAIN"); return x; }\r\n'
                'int prototype(); }; }\r\n'
                'class Other { public: int f() { return 3; } };')
        rows=header_functions(text,'src/test.h')
        self.assertEqual([r['name'] for r in rows],['N::Outer::f','N::Outer::Inner::f','N::Outer::f','Other::f'])
        self.assertEqual(rows[0]['markers'],[(0x80010000,'MAIN')])
        self.assertEqual(rows[0]['start_line'],2)
        for row in rows:
            raw=text[row['start']:row['end']]
            self.assertEqual(row['sha256'],hashlib.sha256(raw.encode()).hexdigest())
            self.assertFalse(raw.startswith(('public:','private:')))
        self.assertIn('const {',text[rows[0]['start']:rows[0]['end']])

    def test_comments_literals_and_initializer_are_not_owners(self):
        text=('// class Fake { void bad() {} };\n'
              'class Real { const char *value = "class Fake { void bad() {} };"; '
              'Real(): value(0) {} '
              '~Real() {} int run() { const char *p="void bad() {}"; return 1; } };')
        rows=source_functions(text,'src/test.hpp')
        self.assertEqual([r['name'] for r in rows],['Real::~Real','Real::run'])

    def test_cpp_parser_remains_unchanged(self):
        self.assertEqual(source_functions('class C { int f() { return 1; } };','src/test.cpp'),[])
        self.assertEqual(source_functions('int f() { return 1; }','src/test.h')[0]['name'],'f')

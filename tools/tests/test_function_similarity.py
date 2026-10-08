import json
import os
from pathlib import Path
import sqlite3
import tempfile
import unittest

_project = tempfile.TemporaryDirectory()
_root = Path(_project.name)
(_root/'xport-project.json').write_text(json.dumps({
    'schema': 1,
    'name': 'Similarity test',
    'paths': {'similarity_directory': 'status/similarity'},
}))
os.environ['XPORT_PROJECT'] = str(_root)
import function_similarity
os.environ.pop('XPORT_PROJECT')


class FunctionSimilarityTests(unittest.TestCase):
    def test_instruction_rows_preserve_ida_chunk_order(self):
        db = sqlite3.connect(':memory:')
        db.executescript('''
        CREATE TABLE chunks(image TEXT,function INTEGER,start INTEGER,end INTEGER);
        CREATE TABLE instructions(image TEXT,function INTEGER,address INTEGER,word TEXT,disassembly TEXT);
        ''')
        image, function = 'GAME.EXE', 0x80002000
        db.executemany('INSERT INTO chunks VALUES(?,?,?,?)', [
            (image, function, 0x80002000, 0x80002008),
            (image, function, 0x80001000, 0x80001008),
        ])
        db.executemany('INSERT INTO instructions VALUES(?,?,?,?,?)', [
            (image, function, 0x80001004, '00000004', 'tail 2'),
            (image, function, 0x80002004, '00000002', 'entry 2'),
            (image, function, 0x80001000, '00000003', 'tail 1'),
            (image, function, 0x80002000, '00000001', 'entry 1'),
        ])

        rows = function_similarity.function_instruction_rows(db, image, function)

        self.assertEqual([row[0] for row in rows], [0x80002000, 0x80002004, 0x80001000, 0x80001004])


if __name__ == '__main__':
    unittest.main()

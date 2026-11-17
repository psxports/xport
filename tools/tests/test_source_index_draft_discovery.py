import json
import sqlite3
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import source_index

ADDRESS = 0x80012340
SYMBOL = "sub_80012340"


class DraftDiscoveryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / "src").mkdir()
        (self.root / "status").mkdir()
        self.database = self.root / "status/analysis.sqlite"
        self.ledger_path = self.root / "status/translation-ledger.json"
        self.ledger_path.write_text(json.dumps({"entries": []}))
        with sqlite3.connect(self.database) as db:
            db.executescript("CREATE TABLE functions(image TEXT,address INTEGER,name TEXT,sha256 TEXT,status TEXT,PRIMARY KEY(image,address)); CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT);")
            db.execute("INSERT INTO functions VALUES(?,?,?,?,?)", ("MAIN", ADDRESS, SYMBOL, "original", "TODO"))
        self.inventory = patch("source_index.source_inventory", side_effect=lambda root: sorted((root / "src").glob("*")))
        self.inventory.start()
        self.addCleanup(self.inventory.stop)

    def source(self, name="draft.c", text=None):
        path = self.root / "src" / name
        path.write_text(text or "int sub_80012340(void) { return 7; }\n")
        return path

    def populate(self, ledger=None):
        with sqlite3.connect(self.database) as db:
            return source_index.populate(db, self.root, ledger or [])

    def rows(self):
        with sqlite3.connect(self.database) as db:
            return db.execute("SELECT image,address,source_path,symbol,mapping_status,mapping_basis,body_sha256 FROM implementations").fetchall()

    def statuses(self):
        with sqlite3.connect(self.database) as db:
            return db.execute("SELECT image,address,status FROM functions ORDER BY image,address").fetchall()

    def rebuild(self):
        with patch("xport_project.load_project", return_value=(self.root, {})), patch("xport_project.project_path", return_value=self.database):
            return source_index.rebuild(format_changed=False)

    def test_inline_header_populate_and_incremental_refresh(self):
        path=self.source('inline.h', 'class Item { public: int owner() { FUNCTION_MARKER(0x80012340, "MAIN"); return 7; } };')
        ledger=[{'image':'MAIN','address':ADDRESS,'source':'src/inline.h','semantic_name':'Item::owner'}]
        self.populate(ledger)
        row=self.rows()[0]
        self.assertEqual(row[2:4],('src/inline.h','Item::owner'))
        old_hash=row[-1]
        path.write_text(path.read_text().replace('return 7','return 8'))
        with sqlite3.connect(self.database) as db:
            source_index.incremental_source_refresh(db,self.root,ledger,['src/inline.h'])
        self.assertNotEqual(self.rows()[0][-1],old_hash)

    def test_unique_unledger_address_body_is_derived_without_status_or_ledger_mutation(self):
        self.source()
        ledger_before = self.ledger_path.read_bytes()
        statuses_before = self.statuses()
        result = self.populate()
        row = self.rows()[0]
        self.assertEqual(result["mapped"], 1)
        self.assertEqual(row[:6], ("MAIN", ADDRESS, "src/draft.c", SYMBOL, "mapped", "unique_original_address_symbol"))
        self.assertEqual(self.statuses(), statuses_before)
        self.assertEqual(self.ledger_path.read_bytes(), ledger_before)

    def test_same_address_in_two_images_does_not_guess_image(self):
        self.source()
        with sqlite3.connect(self.database) as db:
            db.execute("INSERT INTO functions VALUES(?,?,?,?,?)", ("OVERLAY", ADDRESS, SYMBOL, "overlay", "TODO"))
        self.populate()
        self.assertEqual(self.rows(), [])

    def test_cpp_linkage_block_keeps_provenance_and_status(self):
        self.source("translated.cpp", 'extern "C" {\nint sub_80012340(void) { return 7; }\n}\n')
        before = self.statuses()
        self.populate()
        self.assertEqual(self.rows()[0][2:6], ("src/translated.cpp", SYMBOL, "mapped", "unique_original_address_symbol"))
        self.assertEqual(self.statuses(), before)

    def test_cpp_qualified_methods_and_constructor_initializer_provenance(self):
        text = ('Effect10d::Effect10d(unsigned object) : object_address(object) { '
                'FUNCTION_MARKER(0x80012340u, "MAIN"); }\n'
                'unsigned Effect10d::ReadMasked(unsigned address) { return address; }\n'
                'Effect10d::~Effect10d() {}\n')
        funcs = source_index.functions(text, "src/effect10d.cpp")
        self.assertEqual([f["name"] for f in funcs],
                         ["Effect10d::Effect10d", "Effect10d::ReadMasked", "Effect10d::~Effect10d"])
        self.assertEqual(funcs[0]["markers"], [(ADDRESS, "MAIN")])
        self.assertIn(": object_address(object)", text[funcs[0]["start"]:funcs[0]["end"]])

    def test_cpp_member_marker_maps_without_promoting_status(self):
        self.source("effect.cpp", 'void Effect10d::Update() { FUNCTION_MARKER(0x80012340u, "MAIN"); }\n')
        before = self.statuses()
        self.populate([{"image": "MAIN", "address": ADDRESS, "source": "src/effect.cpp", "semantic_name": "Effect10d::Update"}])
        self.assertEqual(self.rows()[0][2:4], ("src/effect.cpp", "Effect10d::Update"))
        self.assertEqual(self.statuses(), before)

    def test_cpp_header_linkage_keeps_both_function_declarations(self):
        text = 'extern "C" {\nint callback(int value);\nvoid reset(void);\n}\n'
        decls = source_index.declarations(text, "src/interface.hpp", True)
        self.assertEqual({d["name"] for d in decls}, {"callback", "reset"})

    def test_cpp_inventory_includes_implementation_and_header(self):
        cpp = self.source("translated.cpp")
        header = self.source("interface.hpp", "void reset(void);\n")
        self.inventory.stop()
        paths = source_index.source_inventory(self.root)
        self.assertIn(cpp.resolve(), paths)
        self.assertIn(header.resolve(), paths)

    def test_duplicate_bodies_across_files_are_not_discovered(self):
        self.source("first.c")
        self.source("second.c", "int sub_80012340(void) { return 8; }\n")
        self.populate()
        self.assertEqual(self.rows(), [])

    def test_ledger_binding_wins_over_unledger_matching_symbol(self):
        self.source()
        self.source("official.c", "int reviewed(void) { return 9; }\n")
        self.populate([{"image": "MAIN", "address": ADDRESS, "source": "src/official.c", "semantic_name": "reviewed"}])
        self.assertEqual(self.rows()[0][2:6], ("src/official.c", "reviewed", "mapped", "ledger_semantic_name"))

    def test_unresolved_ledger_binding_is_not_replaced_by_discovery(self):
        self.source()
        self.source("official.c", "int unrelated(void) { return 9; }\n")
        self.populate([{"image": "MAIN", "address": ADDRESS, "source": "src/official.c"}])
        self.assertEqual(self.rows()[0][2:6], ("src/official.c", None, "unmapped", "no_explicit_address_identity"))

    def test_rebuild_refreshes_edited_unledger_body_and_preserves_status(self):
        path = self.source()
        self.rebuild()
        first_hash = self.rows()[0][-1]
        statuses_before = self.statuses()
        path.write_text("int sub_80012340(void) { return 123; }\n")
        self.assertTrue(source_index.index_needs_refresh(self.root, self.database, self.ledger_path))
        result = self.rebuild()
        self.assertEqual(result["mode"], "full")
        self.assertNotEqual(self.rows()[0][-1], first_hash)
        self.assertEqual(self.statuses(), statuses_before)
        self.assertFalse(source_index.index_needs_refresh(self.root, self.database, self.ledger_path))

    def test_rebuild_removes_discovery_when_body_disappears(self):
        path = self.source()
        self.rebuild()
        path.write_text("int unrelated(void) { return 1; }\n")
        self.rebuild()
        self.assertEqual(self.rows(), [])
        self.assertEqual(self.statuses(), [("MAIN", ADDRESS, "TODO")])

    def test_conflicting_explicit_image_marker_prevents_address_only_discovery(self):
        self.source(text='int sub_80012340(void) { FUNCTION_MARKER(0x80012340u, "OVERLAY"); return 7; }\n')
        self.populate()
        self.assertEqual(self.rows(), [])


if __name__ == "__main__":
    unittest.main()

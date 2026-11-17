"""Verify PsyQ recognition across relocation, identity and archive boundaries"""
import io
import json
from pathlib import Path
import sqlite3
import struct
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile
from contextlib import closing

from psyq_catalog import build_catalog, connect, make_mask, parse_lnk, sha, manifest_sources, sdk_library_members
from psyq_analyzer import bind_inventory, bios_stub, compare, export_scripts, find_matches, refine_matches, scan, save_scan, summarize_symbols


BASE = 0x80010000


def pstr(text):
    data = text.encode('ascii')
    return bytes([len(data)]) + data


def expression(expr):
    if expr[0] == 0:
        return b'\0' + struct.pack('<I', expr[1])
    if expr[0] in (2, 4, 12, 22):
        return bytes([expr[0]]) + struct.pack('<H', expr[1])
    return bytes([expr[0]]) + expression(expr[1]) + expression(expr[2])


def lnk(code, names, relocs=(), imports=()):
    data = b'LNK\x02' + b'\x10' + struct.pack('<HHB', 1, 0, 4) + pstr('.text')
    data += b'\x06\x01\x00\x02' + struct.pack('<H', len(code)) + code
    for kind, offset, expr in relocs:
        data += b'\x0a' + struct.pack('<BH', kind, offset) + expression(expr)
    for i, name, offset in names:
        data += b'\x0c' + struct.pack('<HHI', i, 1, offset) + pstr(name)
    for i, name in imports:
        data += b'\x0e' + struct.pack('<H', i) + pstr(name)
    return data + b'\0'


def words(*values):
    return struct.pack('<' + 'I' * len(values), *values)


def coff_fixture(code):
    strings = b'test.c\0Work\0'
    sympos = 60 + len(code)
    header = struct.pack('>HHIIIHH', 0x162, 1, 0, sympos, 96, 0, 0)
    section = struct.pack('>8sIIIIIIHHI', b'.text', 0, 0, len(code), 60, 0, 0, 0, 0, 0)
    fields = [0] * 23
    fields[7], fields[8] = 2, sympos + 96 + len(strings)
    fields[13], fields[14] = len(strings), sympos + 96
    fields[17], fields[18] = 1, fields[8] + 24
    symbols = struct.pack('>III', 7, 0, (6 << 26) | (1 << 21))
    symbols += struct.pack('>III', 7, len(code), (8 << 26) | (1 << 21))
    fd = [0] * 19
    fd[3], fd[5] = len(strings), 2
    return header + section + code + struct.pack('>HH23I', 0x7009, 0, *fields) + strings + symbols + struct.pack('>10I2H7I', *fd)


def exe(path, payload):
    header = bytearray(0x800)
    header[:8] = b'PS-X EXE'
    struct.pack_into('<II', header, 0x18, BASE, len(payload))
    path.write_bytes(header + payload)


def object_fixture(code, name, relocs=(), imports=None, strong=True, identity=1):
    relocations = [dict(kind=k, offset=o, expression=e) for k, o, e in relocs]
    mask = make_mask(code, relocations)
    from psyq_catalog import literal_runs
    return dict(id=identity, text=code, mask=mask, normalized_sha256=sha(mask + bytes(a & m for a, m in zip(code, mask))),
                anchor=max(literal_runs(code, mask), key=lambda r: len(r[1]), default=(0, b'')),
                fixed_bytes=sum(m == 255 for m in mask), strong=strong, text_section=1,
                sections={'1': dict(name='.text', bytes=len(code))},
                symbols=[dict(id=1, name=name, section=1, offset=0, visibility='export', kind='label')],
                coff=[], imports=imports or {}, relocations=relocations, version='test', library='LIBTEST.LIB', name=name)


def write_inventory(path, image, entry, fragments):
    body = b''.join(code for _, code in fragments)
    text = f'; image={image} entry={entry:08X} SHA256={sha(body)}\n'
    for address, code in fragments:
        for offset in range(0, len(code), 4):
            word = code[offset:offset+4]
            text += f'{address+offset:08X} {word.hex()} {int.from_bytes(word, "little"):08X} instruction\n'
    path.write_text(text, encoding='utf-8')


class RecognitionTests(unittest.TestCase):
    def short_leaf_fixture(self):
        caller_code = words(0x27BDFFE8, 0xAFBF0010, 0x0C000000, 0,
                            *[0x24050000 + i for i in range(16)], 0x8FBF0010, 0x03E00008, 0x27BD0018)
        leaf_code = words(0x27BDFFE8, 0xAFBF0010, 0x24040004, 0x0C000000, 0,
                          0x8FBF0010, 0x03E00008, 0x27BD0018, 0)
        anchor_code = words(*[0x24630030 + i for i in range(18)], 0x03E00008, 0)
        caller = object_fixture(caller_code, 'Caller', [(74, 8, [2, 2])], {'2': 'Leaf'})
        leaf = object_fixture(leaf_code, 'Leaf', [(74, 12, [2, 2])], {'2': 'Anchor'}, False, 2)
        anchor = object_fixture(anchor_code, 'Anchor', identity=3)
        objects = [dict(o, sdk_id=7) for o in (caller, leaf, anchor)]
        payload = bytearray(caller_code + bytes(16) + leaf_code + bytes(16) + anchor_code)
        leaf_at = len(caller_code) + 16
        anchor_at = leaf_at + len(leaf_code) + 16
        struct.pack_into('<I', payload, 8, 0x0C000000 | ((BASE + leaf_at) >> 2 & 0x3FFFFFF))
        struct.pack_into('<I', payload, leaf_at + 12, 0x0C000000 | ((BASE + anchor_at) >> 2 & 0x3FFFFFF))
        return objects, payload, leaf_at, anchor_at

    def short_leaf_scan(self, objects, payload):
        matches, _ = find_matches(objects, payload, BASE)
        survivors, rejected = refine_matches(matches, payload, BASE)
        symbols = {s['address']: s for s in summarize_symbols(survivors)}
        return survivors, rejected, symbols

    def test_short_leaf_verifies_strong_caller_without_promoting_leaf_inventory(self):
        objects, payload, leaf_at, _ = self.short_leaf_fixture()
        survivors, rejected, symbols = self.short_leaf_scan(objects, payload)
        self.assertFalse(rejected)
        self.assertEqual(symbols[BASE]['status'], 'confirmed')
        self.assertEqual(symbols[BASE + leaf_at]['status'], 'candidate')
        caller = next(m for m in survivors if m['object']['name'] == 'Caller')
        proof = caller['checks'][0]['anchor_evidence']
        self.assertEqual(proof['leaf']['sdk_id'], 7)
        self.assertEqual(proof['calls'][0]['target']['name'], 'Anchor')
        self.assertEqual(proof['leaf']['actual_sha256'], sha(payload[leaf_at:leaf_at + 36]))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            image = root / 'GAME.EXE'
            exe(image, payload)
            inventory = root / 'inventory'
            inventory.mkdir()
            for address, code in ((BASE, payload[:len(objects[0]['text'])]),
                                  (BASE + leaf_at, payload[leaf_at:leaf_at + 36])):
                write_inventory(inventory / f'{address:08X}.mips.txt', image.name, address, [(address, code)])
            report = dict(input=str(image), input_sha256=sha(image.read_bytes()), image=image.name,
                          summary={}, symbols=list(symbols.values()), matches=[dict(address=m['address'],
                          bytes=m['bytes'], strong=m['object']['strong'], checks=m['checks'],
                          signature_id=m['object']['id']) for m in survivors])
            entries = {e['address']: e for e in bind_inventory(report, inventory)}
            self.assertEqual(entries[BASE]['library_status'], 'confirmed')
            self.assertEqual(entries[BASE + leaf_at]['library_status'], 'candidate')

    def test_short_leaf_bad_constant_or_call_target_does_not_verify_caller(self):
        for change in ('selector', 'target'):
            with self.subTest(change=change):
                objects, payload, leaf_at, anchor_at = self.short_leaf_fixture()
                offset = leaf_at + (8 if change == 'selector' else 12)
                word = 0x24040005 if change == 'selector' else 0x0C000000 | ((BASE + anchor_at + 4) >> 2 & 0x3FFFFFF)
                struct.pack_into('<I', payload, offset, word)
                _, _, symbols = self.short_leaf_scan(objects, payload)
                self.assertEqual(symbols[BASE]['status'], 'candidate')

    def test_short_leaf_requires_fully_checked_base_anchor(self):
        objects, payload, _, anchor_at = self.short_leaf_fixture()
        struct.pack_into('<I', payload, anchor_at, 0x0C000000)
        objects[2] = dict(object_fixture(bytes(payload[anchor_at:]), 'Anchor',
                          [(74, 0, [2, 2])], {'2': 'Missing'}, identity=3), sdk_id=7)
        _, _, symbols = self.short_leaf_scan(objects, payload)
        self.assertEqual(symbols[BASE]['status'], 'candidate')

    def test_short_leaf_duplicate_name_or_different_sdk_does_not_verify_caller(self):
        for change in ('duplicate', 'sdk', 'missing_sdk'):
            with self.subTest(change=change):
                objects, payload, leaf_at, _ = self.short_leaf_fixture()
                if change == 'duplicate':
                    payload += bytes(16) + payload[leaf_at:leaf_at + 36]
                elif change == 'sdk':
                    objects[1]['sdk_id'] = 8
                else:
                    del objects[1]['sdk_id']
                _, _, symbols = self.short_leaf_scan(objects, payload)
                self.assertEqual(symbols[BASE]['status'], 'candidate')

    def test_short_leaf_aliases_stay_ambiguous_while_strong_caller_can_be_verified(self):
        objects, payload, leaf_at, _ = self.short_leaf_fixture()
        alias = dict(objects[1], id=4, symbols=[dict(objects[1]['symbols'][0], name='LeafAlias')])
        _, _, symbols = self.short_leaf_scan(objects + [alias], payload)
        self.assertEqual(symbols[BASE]['status'], 'confirmed')
        self.assertEqual(symbols[BASE + leaf_at]['status'], 'ambiguous')

    def test_short_leaf_support_rejects_data_relocations_and_non_jal_calls(self):
        for change in ('data', 'leaf_jump', 'caller_jump', 'addend'):
            with self.subTest(change=change):
                objects, payload, leaf_at, _ = self.short_leaf_fixture()
                if change == 'data':
                    objects[1]['relocations'].append(dict(kind=16, offset=32, expression=[4, 2]))
                    objects[1]['mask'] = make_mask(objects[1]['text'], objects[1]['relocations'])
                elif change in ('leaf_jump', 'caller_jump'):
                    index, offset = (1, 12) if change == 'leaf_jump' else (0, 8)
                    position = leaf_at + offset if index == 1 else offset
                    word = struct.unpack_from('<I', payload, position)[0] & ~0x04000000
                    struct.pack_into('<I', payload, position, word)
                    code = bytearray(objects[index]['text'])
                    struct.pack_into('<I', code, offset, 0x08000000)
                    objects[index]['text'] = bytes(code)
                else:
                    objects[0]['relocations'][0]['expression'] = [44, [2, 2], [0, 4]]
                _, _, symbols = self.short_leaf_scan(objects, payload)
                self.assertEqual(symbols[BASE]['status'], 'candidate')

    def test_short_leaf_mismatched_caller_is_not_rejected_or_verified(self):
        objects, payload, leaf_at, _ = self.short_leaf_fixture()
        struct.pack_into('<I', payload, 8, 0x0C000000 | ((BASE + leaf_at + 4) >> 2 & 0x3FFFFFF))
        survivors, rejected, symbols = self.short_leaf_scan(objects, payload)
        self.assertFalse(rejected)
        self.assertEqual(symbols[BASE]['status'], 'candidate')
        caller = next(m for m in survivors if m['object']['name'] == 'Caller')
        self.assertNotIn('anchor_evidence', caller['checks'][0])

    def test_newly_verified_caller_cannot_anchor_another_short_leaf(self):
        objects, payload, _, _ = self.short_leaf_fixture()
        caller_code = bytearray(objects[0]['text'])
        struct.pack_into('<I', caller_code, 16, 0x240500A0)
        leaf_code = bytearray(objects[1]['text'])
        struct.pack_into('<I', leaf_code, 8, 0x24040005)
        caller = dict(object_fixture(bytes(caller_code), 'SecondCaller',
                      [(74, 8, [2, 2])], {'2': 'SecondLeaf'}, identity=4), sdk_id=7)
        leaf = dict(object_fixture(bytes(leaf_code), 'SecondLeaf',
                    [(74, 12, [2, 2])], {'2': 'Caller'}, False, 5), sdk_id=7)
        caller_at = len(payload) + 16
        leaf_at = caller_at + len(caller_code) + 16
        payload += bytes(16) + caller_code + bytes(16) + leaf_code
        struct.pack_into('<I', payload, caller_at + 8, 0x0C000000 | ((BASE + leaf_at) >> 2 & 0x3FFFFFF))
        struct.pack_into('<I', payload, leaf_at + 12, 0x0C000000 | (BASE >> 2 & 0x3FFFFFF))
        _, _, symbols = self.short_leaf_scan(objects + [caller, leaf], payload)
        self.assertEqual(symbols[BASE]['status'], 'confirmed')
        self.assertEqual(symbols[BASE + caller_at]['status'], 'candidate')

    def test_lnk_subtraction_uses_encoded_operand_order_and_checks_repeated_table_offsets(self):
        from psyq_analyzer import linear_expression
        first = [46, [0, 2048], [4, 2]]
        second = [46, [0, 6144], [4, 2]]
        code = words(0x3C040000, 0x24840000, 0x3C050000, 0x24A50000,
                     *[0x24020000 + i for i in range(20)])
        obj = object_fixture(code, 'TableLookup', [(82, 0, first), (84, 4, first),
                                                  (82, 8, second), (84, 12, second)])
        self.assertEqual(linear_expression(first, obj), ({('section', '2'): 1}, -2048))
        cancelled = [46, [4, 2], [44, [0, 8], [4, 2]]]
        self.assertEqual(linear_expression(cancelled, obj), ({}, 8))
        self.assertIsNone(linear_expression([50, [0, 2], [4, 2]], obj))
        payload = bytearray(code)
        for offset, word in ((0, 0x3C048006), (4, 0x2484B01C),
                             (8, 0x3C058006), (12, 0x24A5A01C)):
            struct.pack_into('<I', payload, offset, word)
        matches, _ = find_matches([obj], payload, BASE)
        survivors, rejected = refine_matches(matches, payload, BASE)
        self.assertFalse(rejected)
        self.assertEqual(summarize_symbols(survivors)[0]['name'], 'TableLookup')
        for bad_word in (0x24A5A020, 0x24A5C01C):
            with self.subTest(bad_word=bad_word):
                struct.pack_into('<I', payload, 12, bad_word)
                matches, _ = find_matches([obj], payload, BASE)
                survivors, rejected = refine_matches(matches, payload, BASE)
                self.assertFalse(survivors)
                self.assertTrue(rejected)

    def test_common_symbols_are_independent_linker_allocations_with_consistent_references(self):
        code = words(0x3C040000, 0x24840000, 0x3C050000, 0x24A50000, 0x3C060000, 0x24C60000,
                     *[0x24020000 + i for i in range(16)])
        relocs = [(82, 0, [2, 2]), (84, 4, [2, 2]), (82, 8, [2, 3]), (84, 12, [2, 3]),
                  (82, 16, [2, 2]), (84, 20, [2, 2])]
        blob = lnk(code, [(1, 'Work', 0)], relocs)[:-1]
        blob += b'\x10' + struct.pack('<HHB', 2, 0, 8) + pstr('.bss')
        blob += b'\x30' + struct.pack('<HHI', 2, 2, 4) + pstr('first')
        blob += b'\x30' + struct.pack('<HHI', 3, 2, 4) + pstr('second') + b'\0'
        sections, symbols, _, _ = parse_lnk(blob)
        self.assertTrue(all('offset' not in s for s in symbols if s['visibility'] == 'bss'))
        self.assertEqual(len(sections[2]['data']), 0)
        obj = object_fixture(code, 'Work', relocs)
        obj['symbols'] = symbols
        payload = bytearray(code)
        for offset, word in ((0, 0x3C04800A), (4, 0x2484F010), (8, 0x3C05800A), (12, 0x24A5F014),
                             (16, 0x3C06800A), (20, 0x24C6F010)):
            struct.pack_into('<I', payload, offset, word)
        matches, _ = find_matches([obj], payload, BASE)
        surviving, rejected = refine_matches(matches, payload, BASE)
        self.assertFalse(rejected)
        self.assertEqual(summarize_symbols(surviving)[0]['name'], 'Work')
        struct.pack_into('<I', payload, 20, 0x24C6F018)
        matches, _ = find_matches([obj], payload, BASE)
        surviving, rejected = refine_matches(matches, payload, BASE)
        self.assertFalse(surviving)
        self.assertTrue(rejected)

    def test_empty_return_is_not_an_identifiable_sdk_signature(self):
        obj = object_fixture(words(0x03E00008, 0), 'SsUtVibrateOff', strong=False)
        matches, _ = find_matches([obj], words(0x03E00008, 0), BASE)
        self.assertEqual(matches, [])

    def test_bios_selector_is_sufficient_for_a_short_kernel_stub(self):
        code = words(0x240A00B0, 0x01400008, 0x24090007, 0)
        obj = object_fixture(code, 'DeliverEvent', strong=False)
        matches, _ = find_matches([obj], code, BASE)
        matches, _ = refine_matches(matches, code, BASE)
        self.assertEqual(summarize_symbols(matches)[0]['name'], 'DeliverEvent')

    def test_exact_critical_section_syscalls_are_short_bios_anchors(self):
        for selector, name in [(1, 'EnterCriticalSection'), (2, 'ExitCriticalSection')]:
            with self.subTest(selector=selector):
                code = words(0x24040000 | selector, 0x0000000C, 0x03E00008, 0)
                obj = object_fixture(code, name, strong=False)
                self.assertTrue(bios_stub(obj))
                matches, _ = find_matches([obj], code, BASE)
                survivors, rejected = refine_matches(matches, code, BASE)
                self.assertFalse(rejected)
                symbol = summarize_symbols(survivors)[0]
                self.assertEqual((symbol['name'], symbol['status']), (name, 'confirmed'))

    def test_syscall_exception_rejects_other_selectors_registers_code_and_delay(self):
        valid = [0x24040001, 0x0000000C, 0x03E00008, 0]
        changes = [(0, 0x24040000), (0, 0x24040003), (0, 0x24020001),
                   (1, 0x0000004C), (1, 0), (2, 0x03E00009), (3, 0x24020001)]
        for index, word in changes:
            with self.subTest(index=index, word=word):
                body = list(valid)
                body[index] = word
                code = words(*body)
                obj = object_fixture(code, 'Other', strong=False)
                self.assertFalse(bios_stub(obj))
                matches, _ = find_matches([obj], code, BASE)
                survivors, _ = refine_matches(matches, code, BASE)
                self.assertEqual(summarize_symbols(survivors)[0]['status'], 'candidate')
        self.assertFalse(bios_stub(object_fixture(words(*valid, 0), 'Padded', strong=False)))
        self.assertFalse(bios_stub(object_fixture(words(*valid), 'Relocated',
                                                 [(16, 12, [2, 2])], {'2': 'Unknown'}, strong=False)))

    def test_syscall_anchor_resolves_imports_without_promoting_other_short_wrappers(self):
        code = words(0x27BDFFE8, 0xAFBF0010, 0x0C000000, 0,
                     *[0x24050000+i for i in range(16)], 0x8FBF0010, 0x03E00008, 0x27BD0018)
        caller = object_fixture(code, 'Caller', [(74, 8, [2, 2])], {'2': 'EnterCriticalSection'})
        stub = words(0x24040001, 0x0000000C, 0x03E00008, 0)
        target = bytearray(code + stub)
        struct.pack_into('<I', target, 8, 0x0C000000 | (((BASE+len(code)) >> 2) & 0x3FFFFFF))
        callee = object_fixture(stub, 'EnterCriticalSection', strong=False, identity=2)
        matches, _ = find_matches([caller, callee], target, BASE)
        survivors, rejected = refine_matches(matches, target, BASE)
        self.assertFalse(rejected)
        symbol = next(s for s in summarize_symbols(survivors) if s['address'] == BASE)
        self.assertEqual((symbol['name'], symbol['status']), ('Caller', 'confirmed'))
        other = words(0x24040003, 0x0000000C, 0x03E00008, 0)
        weak = object_fixture(other, 'EnterCriticalSection', strong=False, identity=3)
        target[-16:] = other
        matches, _ = find_matches([caller, weak], target, BASE)
        survivors, _ = refine_matches(matches, target, BASE)
        symbol = next(s for s in summarize_symbols(survivors) if s['address'] == BASE)
        self.assertEqual(symbol['status'], 'candidate')

    def test_registers_constants_and_opcode_remain_authoritative(self):
        code = words(0x27BDFFE8, 0xAFBF0010, 0x24040020, 0x8FBF0010, 0x03E00008, 0x27BD0018)
        obj = object_fixture(code, 'Work', strong=False)
        for index in (0, 1, 2):
            changed = bytearray(code)
            changed[index * 4 + 2] ^= 1
            matches, _ = find_matches([obj], bytes(changed), BASE)
            self.assertEqual(matches, [])
        self.assertEqual(len(find_matches([obj], code, BASE)[0]), 1)

    def test_call_destination_disambiguates_identical_wrappers(self):
        wrapper = words(0x27BDFFE8, 0xAFBF0010, 0x0C000000, 0, 0x8FBF0010, 0x03E00008, 0x27BD0018)
        write = object_fixture(wrapper, 'SpuWrite', [(74, 8, [2, 2])], {'2': '_spu_Fw'}, identity=1)
        read = object_fixture(wrapper, 'SpuRead', [(74, 8, [2, 2])], {'2': '_spu_Fr'}, identity=2)
        callee = words(0x24020001, 0x03E00008, 0, 0x24020003)
        other = words(0x24020002, 0x03E00008, 0, 0x24020004)
        fw = object_fixture(callee, '_spu_Fw', identity=3)
        fr = object_fixture(other, '_spu_Fr', identity=4)
        payload = bytearray(wrapper + callee + other)
        struct.pack_into('<I', payload, 8, 0x0C000000 | (((BASE + len(wrapper)) >> 2) & 0x3FFFFFF))
        matches, _ = find_matches([write, read, fw, fr], payload, BASE)
        survivors, rejected = refine_matches(matches, payload, BASE)
        self.assertEqual([m['object']['name'] for m in rejected_objects(rejected, [write, read, fw, fr])], ['SpuRead'])
        symbol = next(s for s in summarize_symbols(survivors) if s['address'] == BASE)
        self.assertEqual((symbol['name'], symbol['status']), ('SpuWrite', 'confirmed'))

    def test_unknown_call_does_not_receive_automatic_name(self):
        code = words(0x0C000000, 0, 0x03E00008, 0, 0x24020001)
        obj = object_fixture(code, 'Unknown', [(74, 0, [2, 2])], {'2': 'Missing'})
        matches, _ = find_matches([obj], code, BASE)
        survivors, _ = refine_matches(matches, code, BASE)
        self.assertEqual(summarize_symbols(survivors)[0]['status'], 'candidate')

    def test_hi_lo_signed_carry_and_repeated_data_constraint(self):
        code = words(0x3C040000, 0x24840000, 0x3C050000, 0x24A50000, 0x03E00008, 0)
        expr = [4, 2]
        relocs = [(82, 0, expr), (84, 4, expr), (82, 8, expr), (84, 12, expr)]
        obj = object_fixture(code, 'Lookup', relocs)
        relocated = words(0x3C048009, 0x2484F234, 0x3C058009, 0x24A5F234, 0x03E00008, 0)
        matches, _ = find_matches([obj], relocated, BASE)
        survivors, rejected = refine_matches(matches, relocated, BASE)
        self.assertFalse(rejected)
        self.assertEqual(summarize_symbols(survivors)[0]['status'], 'confirmed')
        damaged = bytearray(relocated)
        struct.pack_into('<I', damaged, 12, 0x24A5F235)
        matches, _ = find_matches([obj], damaged, BASE)
        survivors, rejected = refine_matches(matches, damaged, BASE)
        self.assertFalse(survivors)
        self.assertTrue(rejected)

    def test_versions_deduplicate_and_aliases_remain_ambiguous(self):
        code = words(0x24020001, 0x03E00008, 0, 0x24020002)
        a, b = object_fixture(code, 'A'), object_fixture(code, 'A', identity=2)
        b['version'] = 'other'
        matches, _ = find_matches([a, b], code, BASE)
        matches, _ = refine_matches(matches, code, BASE)
        self.assertEqual(len(summarize_symbols(matches)), 1)
        b['symbols'][0]['name'] = 'B'
        self.assertEqual(summarize_symbols(matches)[0]['status'], 'ambiguous')


def rejected_objects(rejected, objects):
    lookup = {o['id']: o for o in objects}
    return [dict(object=lookup[r['signature_id']]) for r in rejected]


class CatalogAndEvidenceTests(unittest.TestCase):
    def test_startup_uses_standalone_catalog_and_preserves_review_boundary(self):
        import startup
        from psyq_project import recognize
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            code = words(0x3C1C8004, 0x279C4000, *[0x24020000 + i for i in range(18)])
            blob = lnk(code, [(1, 'Work', 0)])
            archive = root / 'sdk.tar'
            with tarfile.open(archive, 'w') as tar:
                info = tarfile.TarInfo('LIB/WORK.OBJ')
                info.size = len(blob)
                tar.addfile(info, io.BytesIO(blob))
            catalog = root / 'catalog.sqlite'
            build_catalog([('test', archive)], catalog)
            image = root / 'BOOT.EXE'
            exe(image, code + code)
            exports = root / 'orig/images/BOOT.EXE'
            (exports / 'functions').mkdir(parents=True)
            functions = []
            for offset in (0, len(code)):
                address = BASE + offset
                text = f'; image=BOOT.EXE entry={address:08X} SHA256={sha(code)}\n'
                for i in range(0, len(code), 4):
                    word = code[i:i + 4]
                    text += f'{address + i:08X} {word.hex()} {int.from_bytes(word, "little"):08X} instruction\n'
                (exports / 'functions' / f'{address:08X}.mips.txt').write_text(text)
                (exports / 'functions' / f'{address:08X}.lst').write_text(text)
                functions.append(dict(address=address, name=f'sub_{address:08X}', sha256=sha(code),
                                      chunks=[[address, address + len(code)]], pseudocode=None))
            (exports / 'functions.json').write_text(json.dumps(functions))
            (root / 'tools/ida').mkdir(parents=True)
            (root / 'tools/ida/BOOT.EXE.json').write_text(json.dumps(dict(image='BOOT.EXE', gp=0x80044000,
                 gp_evidence=dict(source='runtime_initializer', header_value=0,
                                  runtime_initializers=startup.runtime_gp_initializers(code + code, BASE)),
                 loads=[dict(path='BOOT.EXE', base=BASE, offset=0x800)])))
            config = dict(schema=1, name='Fixture', toolset=str(Path(startup.__file__).parent),
                          paths=dict(status='status', sdk_catalog=str(catalog), ida_exports='orig/images', ida_configs='tools/ida'),
                          analysis=dict(images=['BOOT.EXE', 'EXCLUDED.EXE'], excluded_images=[dict(image='EXCLUDED.EXE')]))
            (root / 'xport-project.json').write_text(json.dumps(config))
            with self.assertRaisesRegex(ValueError, 'STARTUP_PSYQ_STAGE_REQUIRED'):
                recognize(root, 'unowned')
            (root / 'status').mkdir()
            (root / 'status/startup.json').write_text(json.dumps(dict(step='psyq', status='running')))
            recognition, classification = startup.recognize_psyq(root, {}, 'native-sdk')
            self.assertEqual(recognition['images'], ['BOOT.EXE'])
            self.assertEqual((classification['accepted'], classification['pending']), (2, 0))
            rows = json.loads((root / classification['output']).read_text())['accepted']
            for row in rows:
                self.assertEqual(row['alias'], 'Work')
                self.assertTrue(all(m['object_start'] <= row['address'] < m['object_end'] for m in row['matches']))
            self.assertEqual(json.loads((root / 'xport-project.json').read_text())['paths']['sdk_classification'], classification['output'])
            self.assertFalse((root / 'status/project.sqlite').exists())
            self.assertFalse((root / 'status/analysis.sqlite').exists())
            config = json.loads((root / 'xport-project.json').read_text())
            config['analysis']['sdk_detector'] = 'ghidra'
            config['paths']['sdk_classification'] = 'status/custom-sdk.json'
            (root / 'xport-project.json').write_text(json.dumps(config))
            with patch.object(startup, 'run_ghidra', return_value={}) as legacy, patch.object(
                    startup, 'classify_psyq', return_value={}) as classifier:
                startup.recognize_psyq(root, dict(ghidra='Ghidra', plugin='Plugin'), 'legacy')
                legacy.assert_called_once()
                self.assertEqual(classifier.call_args.kwargs['output'], 'status/custom-sdk.json')

    def test_identical_lnk_copies_share_proven_coff_names_without_inventing_extra_coff_objects(self):
        from psyq_analyzer import load_objects
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            code = words(*[0x24020000 + i for i in range(18)])
            blob = lnk(code, [(1, 'Work', 0)])
            coff = coff_fixture(code)
            ar_header = (b'WORK.o/'.ljust(16) + b'0'.ljust(12) + b'0'.ljust(6) + b'0'.ljust(6)
                         + b'644'.ljust(8) + str(len(coff)).encode().ljust(10) + b'`\n')
            ar = b'!<arch>\n' + ar_header + coff + (b'\n' if len(coff) % 2 else b'')
            archive = root / 'sdk.tar'
            with tarfile.open(archive, 'w') as tar:
                for name, data in [('LIB/WORK.OBJ', blob), ('PSX/LIB/WORK.OBJ', blob), ('COFF/LIB/WORK.A', ar)]:
                    info = tarfile.TarInfo(name)
                    info.size = len(data)
                    tar.addfile(info, io.BytesIO(data))
            catalog = root / 'catalog.sqlite'
            summary = build_catalog([('test', archive)], catalog)
            self.assertEqual((summary['objects'], summary['equivalent_coff'], summary['issues']), (2, 1, 0))
            objects, _ = load_objects(catalog)
            self.assertTrue(all(o['coff'] and o['coff'][0]['name'] == 'Work' for o in objects))

    def test_library_membership_requires_the_complete_original_body_without_inventing_names(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            payload = words(*[0x24020000 + i for i in range(20)])
            image = root / 'GAME.EXE'
            exe(image, payload)
            inventory = root / 'inventory'
            inventory.mkdir()
            for offset, size in ((0, 80), (16, 16)):
                body = payload[offset:offset + size]
                text = f'; image=GAME.EXE entry={BASE + offset:08X} SHA256={sha(body)}\n'
                for i in range(0, size, 4):
                    word = body[i:i + 4]
                    text += f'{BASE + offset + i:08X} {word.hex()} {int.from_bytes(word, "little"):08X} instruction\n'
                (inventory / f'{BASE + offset:08X}.mips.txt').write_text(text)
            report = dict(input=str(image), input_sha256=sha(image.read_bytes()), image='GAME.EXE', summary={},
                          symbols=[dict(address=BASE, name='SDKPrefix', names=['SDKPrefix'])],
                          matches=[dict(address=BASE, bytes=64, strong=True, checks=[], signature_id=1)])
            entries = {e['address']: e for e in bind_inventory(report, inventory)}
            self.assertEqual(entries[BASE]['library_status'], 'candidate')
            self.assertEqual(entries[BASE]['boundary_status'], 'sdk_overlap')
            self.assertEqual(entries[BASE + 16]['library_status'], 'confirmed')
            self.assertIsNone(entries[BASE + 16]['name'])

    def test_shared_tail_keeps_export_hash_and_requires_one_full_sdk_context(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            payload = words(*[0x24020000+i for i in range(22)])
            image = root/'GAME.EXE'
            exe(image, payload)
            inventory = root/'inventory'
            inventory.mkdir()
            listing = inventory/'shared.mips.txt'
            write_inventory(listing, image.name, BASE+8, [(BASE+8, payload[8:]), (BASE, payload[:8])])
            original_listing = listing.read_bytes()
            report = dict(input=str(image), input_sha256=sha(image.read_bytes()),
                image=image.name, summary={}, symbols=[],
                matches=[dict(address=BASE, bytes=len(payload), strong=True, checks=[], signature_id=1)])
            entry = bind_inventory(report, inventory)[0]
            self.assertEqual(entry['library_status'], 'confirmed')
            self.assertIsNone(entry['name'])
            self.assertEqual(entry['chunks'], [[BASE+8, BASE+len(payload)], [BASE, BASE+8]])
            self.assertEqual(entry['original_sha256'], sha(payload[8:]+payload[:8]))
            self.assertEqual(entry['bytes'], len(payload))
            self.assertEqual(listing.read_bytes(), original_listing)

    def test_separate_partial_sdk_matches_cannot_jointly_confirm_chunks(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tail = words(*[0x24020000+i for i in range(20)])
            main = words(*[0x24050000+i for i in range(20)])
            image = root/'GAME.EXE'
            exe(image, tail+bytes(16)+main)
            entry_address = BASE+len(tail)+16
            inventory = root/'inventory'
            inventory.mkdir()
            write_inventory(inventory/'shared.mips.txt', image.name, entry_address,
                            [(entry_address, main), (BASE, tail)])
            report = dict(input=str(image), input_sha256=sha(image.read_bytes()), image=image.name,
                          summary={}, symbols=[], matches=[
                              dict(address=entry_address, bytes=len(main), strong=True, checks=[], signature_id=1),
                              dict(address=BASE, bytes=len(tail), strong=True, checks=[], signature_id=2)])
            entry = bind_inventory(report, inventory)[0]
            self.assertEqual((entry['library_status'], entry['boundary_status']), ('candidate', 'sdk_overlap'))

    def test_chunk_addresses_entry_and_every_original_byte_are_checked(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            payload = words(*[0x24020000+i for i in range(22)])
            image = root/'GAME.EXE'
            exe(image, payload)
            inventory = root/'inventory'
            inventory.mkdir()
            listing = inventory/'shared.mips.txt'
            fragments = [(BASE+8, payload[8:]), (BASE, payload[:8])]
            corrupt_tail = bytearray(payload[:8])
            corrupt_tail[0] ^= 1
            cases = [(BASE+8, fragments+[(BASE, payload[:4])], 'overlapping'),
                     (BASE+8, [(BASE+8, payload[8:]), (BASE+1, payload[:8])], 'addresses'),
                     (BASE+len(payload)+4, fragments, 'addresses'),
                     (BASE+8, [(BASE+8, payload[8:]), (BASE, corrupt_tail)], 'differ from original')]
            for entry, chunks, error in cases:
                with self.subTest(error=error):
                    write_inventory(listing, image.name, entry, chunks)
                    report = dict(input=str(image), input_sha256=sha(image.read_bytes()), image=image.name,
                                  summary={}, symbols=[], matches=[])
                    with self.assertRaisesRegex(ValueError, error):
                        bind_inventory(report, inventory)

    def test_sdk_match_in_unowned_gap_is_not_function_overlap(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tail = words(*[0x24020000+i for i in range(8)])
            gap = words(*[0x24050000+i for i in range(16)])
            main = words(*[0x24060000+i for i in range(8)])
            image = root/'GAME.EXE'
            exe(image, tail+gap+main)
            entry_address = BASE+len(tail)+len(gap)
            inventory = root/'inventory'
            inventory.mkdir()
            write_inventory(inventory/'shared.mips.txt', image.name, entry_address,
                            [(entry_address, main), (BASE, tail)])
            report = dict(input=str(image), input_sha256=sha(image.read_bytes()), image=image.name,
                          summary={}, symbols=[], matches=[
                              dict(address=BASE+len(tail), bytes=len(gap), strong=True, checks=[], signature_id=1)])
            self.assertEqual(bind_inventory(report, inventory)[0]['library_status'], 'unrecognized')

    def test_ecoff_file_descriptor_ends_at_eof_without_four_extra_bytes(self):
        from psyq_sdk_symbols import parse_coff
        code = words(0x03E00008, 0)
        parsed_code, procedures = parse_coff(coff_fixture(code))
        self.assertEqual(parsed_code, code)
        self.assertEqual((procedures[0]['name'], procedures[0]['bytes']), ('Work', 8))

    def test_remote_subset_checks_ranges_crc_and_records_partial_provenance(self):
        from psyq_fetch import fetch
        data = io.BytesIO()
        with zipfile.ZipFile(data, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('SDK.bin', b'Original data track')
            archive.writestr('SDK.cue', 'TRACK 01 MODE2/2352')
            archive.writestr('audio.wav', b'Audio is not needed')
        raw = data.getvalue()
        class Response(io.BytesIO):
            status = 206
            def __init__(self, first, last):
                super().__init__(raw[first:last + 1])
                self.headers = {'Content-Range': f'bytes {first}-{last}/{len(raw)}'}
        def open_range(request, timeout):
            first, last = map(int, request.get_header('Range').removeprefix('bytes=').split('-'))
            return Response(first, last)
        with tempfile.TemporaryDirectory() as directory, patch('psyq_fetch.urlopen', open_range):
            output = Path(directory) / 'SDK.zip'
            evidence = fetch('https://example.test/SDK.zip', len(raw), output, 'unverified-upstream-sha1')
            self.assertFalse(evidence['full_remote_hash_verified'])
            self.assertEqual(evidence['sha256'], sha(output.read_bytes()))
            with zipfile.ZipFile(output) as archive:
                self.assertEqual(set(archive.namelist()), {'SDK.bin', 'SDK.cue'})
                self.assertEqual(archive.read('SDK.bin'), b'Original data track')

    def test_lib2_directory_binds_exact_object_extents(self):
        member = lnk(words(0x03E00008, 0), [(1, 'Empty', 0)])
        directory = struct.pack('<III', 12, len(member), 0) + pstr('EMPTY.OBJ\0')
        directory += struct.pack('<HH', 256, 0) + pstr('Empty') + b'\0\0'
        archive = b'LIB\x02' + struct.pack('<II', 12 + len(member), len(directory)) + member + directory
        self.assertEqual(list(sdk_library_members(archive)), [('EMPTY', 12, member)])
        damaged = bytearray(archive)
        struct.pack_into('<I', damaged, 12 + len(member) + 4, len(member) + 4)
        with self.assertRaisesRegex(ValueError, 'member extent'):
            list(sdk_library_members(damaged))

    def test_preserved_store_is_self_contained_and_rejects_changed_headers(self):
        from psyq_store import store_source
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / 'sdk.tar'
            code = words(*[0x24020000 + i for i in range(18)])
            with tarfile.open(archive, 'w') as tar:
                for name, data in [('LIB/WORK.OBJ', lnk(code, [(1, 'Work', 0)])), ('INCLUDE/SDK.H', b'#define SDK 1\n')]:
                    info = tarfile.TarInfo(name)
                    info.size = len(data)
                    tar.addfile(info, io.BytesIO(data))
            store = root / 'psyq'
            row = store_source('4.3', 'original', archive, store, 'https://example.test/sdk.tar')
            manifest = store / 'sources.json'
            manifest.write_text(json.dumps(dict(schema='xport-psyq-store-v1', sources=[row])))
            archive.unlink()
            sources = manifest_sources(manifest)
            summary = build_catalog(sources, root / 'catalog.sqlite')
            self.assertEqual(summary['objects'], 1)
            header = store / row['directory'] / 'INCLUDE/SDK.H'
            header.write_bytes(b'#define SDK 2\n')
            with self.assertRaisesRegex(ValueError, 'member hash mismatch'):
                manifest_sources(manifest)

    def test_function_span_survives_changed_neighbor_and_uses_original_section_base(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first = words(*[0x24040000 + i for i in range(18)])
            second = words(0x27BDFFE8, 0xAFBF0010, 0x0C000000,
                           *[0x24050000 + i for i in range(15)], 0x03E00008, 0x27BD0018)
            blob = lnk(first + second, [(1, 'First', 0), (2, 'Second', len(first))],
                       [(74, len(first) + 8, [2, 1])])
            archive = root / 'sdk.tar'
            with tarfile.open(archive, 'w') as tar:
                info = tarfile.TarInfo('LIB/WORK.OBJ')
                info.size = len(blob)
                tar.addfile(info, io.BytesIO(blob))
            catalog = root / 'catalog.sqlite'
            build_catalog([('test', archive)], catalog)
            payload = bytearray(bytes(16) + first + second)
            payload[16] ^= 1
            struct.pack_into('<I', payload, 16 + len(first) + 8, 0x0C000000 | (((BASE + 16) >> 2) & 0x3FFFFFF))
            image = root / 'GAME.EXE'
            exe(image, payload)
            report = scan(catalog, image)
            named = {s['address']: s['name'] for s in report['symbols'] if s['status'] == 'confirmed'}
            self.assertEqual(named[BASE + 16 + len(first)], 'Second')
            self.assertNotIn(BASE + 16, named)

    def test_archive_to_database_to_names_without_ghidra(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            code = words(*[0x24020000 + i for i in range(14)], 0x03E00008, 0)
            blob = lnk(code, [(1, 'Answer', 0)])
            archive = root / 'sdk.tar.gz'
            with tarfile.open(archive, 'w:gz') as tar:
                info = tarfile.TarInfo('LIB/ANSWER.OBJ')
                info.size = len(blob)
                tar.addfile(info, io.BytesIO(blob))
            catalog = root / 'catalog.sqlite'
            summary = build_catalog([('test', archive)], catalog)
            self.assertEqual((summary['objects'], summary['issues']), (1, 0))
            image = root / 'GAME.EXE'
            exe(image, code)
            report = scan(catalog, image)
            self.assertEqual(report['symbols'][0]['name'], 'Answer')
            save_scan(report, root / 'result')
            with closing(sqlite3.connect(root / 'result/analysis.sqlite')) as db:
                self.assertEqual(db.execute('select name from symbols').fetchone()[0], 'Answer')
            self.assertIn('original-byte mismatch', (root / 'result/apply_ida.py').read_text())
            self.assertIn('original-byte mismatch', (root / 'result/ApplyPsyqNames.java').read_text())
            with self.assertRaisesRegex(ValueError, 'overwrite'):
                build_catalog([('test', archive)], catalog)
            with self.assertRaisesRegex(ValueError, 'overwrite'):
                save_scan(report, root / 'result')
            with closing(sqlite3.connect(catalog)) as db:
                db.execute('UPDATE signatures SET normalized_sha256=?', ('0' * 64,))
                db.commit()
            with self.assertRaisesRegex(ValueError, 'signature extent/hash mismatch'):
                scan(catalog, image)

    def test_unknown_records_and_relocations_fail_closed(self):
        with self.assertRaisesRegex(ValueError, 'Unsupported LNK record'):
            parse_lnk(b'LNK\x02\xff\x00')
        with self.assertRaisesRegex(ValueError, 'Unsupported relocation'):
            make_mask(bytes(4), [dict(offset=0, kind=255)])
        with self.assertRaisesRegex(ValueError, 'Overlapping'):
            make_mask(bytes(4), [dict(offset=0, kind=74), dict(offset=0, kind=74)])

    def test_multichunk_relocations_use_their_own_chunk_base(self):
        code = words(0x24020001, 0x03E00008)
        blob = lnk(code, [(1, 'Work', 0)])[:-1]
        blob += b'\x02\x04\x00' + words(0x0C000000)
        blob += b'\x0a' + struct.pack('<BH', 74, 0) + expression([2, 1]) + b'\0'
        sections, _, _, relocations = parse_lnk(blob)
        self.assertEqual(len(sections[1]['data']), 12)
        self.assertEqual(relocations[0]['offset'], 8)

    def test_comparison_rejects_stale_ghidra_patterns(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            image = root / 'GAME.EXE'
            exe(image, words(0x24020001, 0x03E00008, 0))
            report = dict(input=str(image), input_sha256=sha(image.read_bytes()), image='GAME.EXE',
                          symbols=[], summary={})
            ghidra = root / 'ghidra.json'
            ghidra.write_text(json.dumps(dict(image='GAME.EXE', base=BASE, matches=[dict(
                address=BASE, pattern='ffffffff', mask='ffffffff', labels=[])])))
            with self.assertRaisesRegex(ValueError, 'does not match'):
                compare(report, ghidra)

    def test_truncated_image_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'GAME.EXE'
            exe(path, bytes(8))
            path.write_bytes(path.read_bytes()[:-1])
            from psyq_analyzer import read_exe
            with self.assertRaisesRegex(ValueError, 'truncated'):
                read_exe(path)


if __name__ == '__main__':
    unittest.main()

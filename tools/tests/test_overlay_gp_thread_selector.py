"""Check exact byte-selector carriers and rejection of unreviewed variants."""
import struct
import unittest

from overlay_gp_thread_selector import prove_byte_selector


class ByteResourceSelectorTests(unittest.TestCase):
    def setUp(self):
        self.start = 0x8006c4fc
        self.loader = 0x80012ff4
        self.gp = 0x80062d1c
        self.words = [0x3c038006, 0x24020007, 0x3c048006, 0xac622db8,
                      0x3c028006, 0x90422df4, 0x24030004, 0xac832d9c,
                      0x2442fffe, 0x2c420002, 0x10400002, 0x24040005,
                      0x24040001, 0x0c004bfd, 0, 0x8fbf0028,
                      0x8fb50024, 0x8fb40020, 0x8fb3001c, 0x8fb20018,
                      0x8fb10014, 0x8fb00010, 0x03e00008, 0x27bd0030]

    def prove(self, **changes):
        return prove_byte_selector(struct.pack('<24I', *self.words),
                                   changes.get('start', self.start),
                                   changes.get('loader', self.loader), changes.get('gp', self.gp))

    def test_all_byte_values_and_both_original_indices(self):
        result = self.prove()
        self.assertEqual(result['indices'], [1, 5])
        self.assertEqual(result['cases'], 256)
        self.assertEqual(result['gp'], self.gp)

    def test_other_bounded_resource_constants(self):
        self.words[11], self.words[12] = 0x24040008, 0x24040003
        self.assertEqual(self.prove()['indices'], [3, 8])

    def test_reject_changed_branch_and_epilogue_words(self):
        for index in (1, 6, 8, 9, 10, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23):
            with self.subTest(index=index):
                previous = self.words[index]
                self.words[index] ^= 1
                with self.assertRaises(ValueError):
                    self.prove()
                self.words[index] = previous

    def test_reject_gp_writer_or_other_materializer_register(self):
        for index in (0, 2, 3, 4, 5, 7):
            with self.subTest(index=index):
                previous = self.words[index]
                self.words[index] = 0x3c1c8006
                with self.assertRaises(ValueError):
                    self.prove()
                self.words[index] = previous

    def test_reject_different_loader(self):
        with self.assertRaises(ValueError):
            self.prove(loader=self.loader+4)

    def test_reject_unaligned_or_foreign_suffix(self):
        for start in (self.start+1, 0x1f801c00, 0x801fffc0):
            with self.subTest(start=start), self.assertRaises(ValueError):
                self.prove(start=start)

    def test_reject_empty_gp(self):
        with self.assertRaises(ValueError):
            self.prove(gp=0)

    def test_reject_negative_or_wrong_argument_register(self):
        for index, value in ((11, 0x2404ffff), (12, 0x2404ffff), (11, 0x24050005), (12, 0x24050001)):
            with self.subTest(index=index):
                previous = self.words[index]
                self.words[index] = value
                with self.assertRaises(ValueError):
                    self.prove()
                self.words[index] = previous

    def test_reject_mmio_byte_address(self):
        self.words[4], self.words[5] = 0x3c021f80, 0x90421c00
        with self.assertRaises(ValueError):
            self.prove()

    def test_reject_state_alias(self):
        self.words[5] = 0x90422db8
        with self.assertRaises(ValueError):
            self.prove()

    def test_reject_truncated_or_extended_suffix(self):
        code = struct.pack('<24I', *self.words)
        for changed in (code[:-4], code+b'\0'*4):
            with self.assertRaises(ValueError):
                prove_byte_selector(changed, self.start, self.loader, self.gp)


if __name__ == '__main__':
    unittest.main()

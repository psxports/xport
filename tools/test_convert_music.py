import tempfile
from pathlib import Path
import unittest

import convert_music


class ConvertMusicTests(unittest.TestCase):
    def test_split_bin_uses_index_one(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'disc.cue').write_text('FILE "track 02.bin" BINARY\n  TRACK 02 AUDIO\n    INDEX 00 00:00:00\n    INDEX 01 00:02:00\n')
            (root / 'track 02.bin').write_bytes(bytes(200 * convert_music.SECTOR_BYTES))
            track = convert_music.audio_ranges(root / 'disc.cue')[0]
            self.assertEqual(track['offset'], 150 * convert_music.SECTOR_BYTES)
            self.assertEqual(track['size'], 50 * convert_music.SECTOR_BYTES)

    def test_shared_bin_ends_at_next_pregap(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cue = ('FILE disc.bin BINARY\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n'
                   '  TRACK 02 AUDIO\n    INDEX 00 00:10:00\n    INDEX 01 00:12:00\n')
            (root / 'disc.cue').write_text(cue)
            (root / 'disc.bin').write_bytes(bytes(1000 * convert_music.SECTOR_BYTES))
            tracks = convert_music.audio_ranges(root / 'disc.cue')
            self.assertEqual(tracks[0]['size'], 750 * convert_music.SECTOR_BYTES)
            self.assertEqual(tracks[1]['offset'], 900 * convert_music.SECTOR_BYTES)

    def test_rejects_non_sector_aligned_audio(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'disc.cue').write_text('FILE track.bin BINARY\n  TRACK 02 AUDIO\n    INDEX 01 00:00:00\n')
            (root / 'track.bin').write_bytes(b'x')
            with self.assertRaisesRegex(ValueError, 'sector aligned'):
                convert_music.audio_ranges(root / 'disc.cue')


if __name__ == '__main__':
    unittest.main()

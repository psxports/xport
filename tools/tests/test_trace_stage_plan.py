import struct
import unittest

from trace_stage_segments import stage_segments
from trace_pad_schedule import decode_pad_schedule


class StagePlanTests(unittest.TestCase):
    def test_sparse_and_repeated_ticks_remain_one_stage_segment(self):
        rows = [
            {'stage': 7, 'game_tick': 0, 'ordinal': 10},
            {'stage': 7, 'game_tick': 0, 'ordinal': 14},
            {'stage': 7, 'game_tick': 20, 'ordinal': 18},
            {'stage': 7, 'game_tick': 30, 'ordinal': 22},
            {'stage': 7, 'game_tick': 0, 'ordinal': 26},
        ]
        segments = stage_segments(rows)
        self.assertEqual(len(segments), 2)
        self.assertEqual(segments[0]['gameplay_count'], 4)
        self.assertEqual((segments[0]['start_tick'], segments[0]['end_tick']), (0, 30))
        self.assertEqual(segments[1]['start_tick'], 0)

    def test_decode_schedule_preserves_sparse_ticks(self):
        encoded = decode_pad_schedule([[0, 1, 0], [0, 1, 0], [20, 1, 0], [30, 2, 0]], 0, 30)
        rows = [struct.unpack_from('<3I', encoded, offset) for offset in range(0, len(encoded), 12)]
        self.assertEqual(rows, [(0, 0, 1), (20, 20, 1), (30, 30, 2)])

    def test_decode_schedule_rejects_conflicting_duplicate_tick(self):
        with self.assertRaisesRegex(ValueError, 'Conflicting'):
            decode_pad_schedule([[0, 1, 0], [0, 2, 0]], 0, 0)


if __name__ == '__main__':
    unittest.main()

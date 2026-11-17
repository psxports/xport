"""Check absolute integer-rate logic clocks without accumulated time deltas"""
import random
import unittest
from tests import test_pad_ports as fixture


class TickClockTests(unittest.TestCase):
    compile_run = fixture.PadPortsTests.compile_run

    def test_absolute_tick_index_and_fraction(self):
        rng = random.Random(128)
        checks = []
        for rate in (16, 32, 64, 128, 1000):
            times = [0, 1, 7812, 7813, 15624, 15625, 999999, 1000000, 3153600000000000]
            for tick in (1, 2, 127, 128, 1000000, 100000000000):
                boundary = tick * 1000000 // rate
                times += [boundary - 1, boundary, boundary + 1]
            times += [rng.randrange(3153600000000000) for _ in range(100)]
            for time in times:
                index, remainder = divmod(time * rate, 1000000)
                checks.append(f'now={time}ull; phase=xport_frame_fract_ex(&idx,{rate}); assert(idx=={index}ull); assert(fabsf(phase-{remainder / 1000000:.9f}f)<0.0000001f);')
        self.compile_run('''
#include "xport.h"
#include <assert.h>
#include <math.h>
static uint64 now;
uint64 xport_timer_get(void) { return now; }
int main(void) { uint64 idx; float phase;
''' + '\n'.join(checks) + '\nreturn 0; }\n')


if __name__ == '__main__':
    unittest.main()

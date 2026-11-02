"""Exercise live SDK bounds and overlapping draw-area packet storage."""
import os
from pathlib import Path
import unittest
from tests.test_pad_ports import VCVARS, function
from tests import test_pad_ports as pad_tests

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(os.name == 'nt' and VCVARS.exists(), 'Requires MSVC')
class GpuDrawAreaTests(unittest.TestCase):
    compile_run = pad_tests.PadPortsTests.compile_run

    def test_live_bounds_wrap_and_alias(self):
        source = (ROOT / 'src/psx_gpu.c').read_text()
        fixture = r'''
#include "psx.h"
#include <assert.h>
#include <string.h>
#define VRAM_W 1024
#define VRAM_H 512
static uint32 draw_area_width_address, draw_area_height_address;
static uint16 width=320, height=240;
uint16 r_u16(uint32 address) { assert(address==4 || address==6); return address==4?width:height; }
sint16 r_s16(uint32 address) { return (sint16)r_u16(address); }

sint32 xport_store_u8(void *p,uint32 v) { *(uint8 *)p=(uint8)v; return 1; }
'''
        fixture += '\n'.join(function(source, name) for name in (
            'gpu_bind_draw_area_bounds', 'gpu_draw_area_coordinate',
            'gpu_draw_area_word', 'SetDrawArea'))
        fixture += r'''
int main(void) {
    uint32 packet[4]={0x99123456,0,0,0xDEADBEEF};
    PSX_RECT rectangle={-5,200,20,80};
    gpu_bind_draw_area_bounds(4,6);
    assert(SetDrawArea((DR_AREA *)packet,&rectangle)==(0xE4000000 | 14 | (239<<10)));
    assert(packet[0]==0x02123456);
    assert(packet[1]==(0xE3000000 | (200<<10)));
    assert(packet[3]==0xDEADBEEF);
    width=100; height=120;
    rectangle.x=150; rectangle.y=130; rectangle.w=1; rectangle.h=1;
    SetDrawArea((DR_AREA *)packet,&rectangle);
    assert(packet[1]==(0xE3000000 | 99 | (119<<10)));
    rectangle.x=32760; rectangle.w=20;
    SetDrawArea((DR_AREA *)packet,&rectangle);
    assert((packet[2]&1023)==0);
    width=0xFFFF; height=0; rectangle.x=1; rectangle.y=1;
    SetDrawArea((DR_AREA *)packet,&rectangle);
    assert(packet[1]==(0xE3000000 | 1022 | (1023<<10)));
    width=320; height=240;
    packet[0]=0x00020001; packet[1]=0x00040003;
    SetDrawArea((DR_AREA *)packet,(PSX_RECT *)packet);
    assert(packet[1]==(0xE3000000 | 1 | (239<<10)));
    assert(packet[2]==0xE4000000);
    gpu_bind_draw_area_bounds(0,0);
    rectangle.x=2000; rectangle.y=1000; rectangle.w=1; rectangle.h=1;
    SetDrawArea((DR_AREA *)packet,&rectangle);
    assert(packet[1]==(0xE3000000 | 1023 | (511<<10)));
    return 0;
}
'''
        self.compile_run(fixture)


if __name__ == '__main__':
    unittest.main()

"""Exercise the fullscreen player's actual entry sample and skip-input loop"""
from pathlib import Path
import unittest
from tests import test_pad_ports as fixture
ROOT = Path(__file__).resolve().parents[2]

@unittest.skipUnless(fixture.VCVARS.exists(), 'Requires MSVC')
class FmvSkipTests(unittest.TestCase):
    compile_run = fixture.PadPortsTests.compile_run

    def test_release_all_skip_buttons_before_accepting_a_new_press(self):
        source = (ROOT / 'src/xport_fmv.c').read_text()
        body = fixture.function(source, 'xport_fmv_play_configured')
        start = body.index('    for (controller =', body.index('    SetDispMask(0);'))
        entry = body[start:body.index('    last_frame =',start)]
        start = body.index('        buttons = 0;')
        poll = body[start:body.index('        if (finishing)',start)]
        prefix = r'''
#include "psx.h"
#include <assert.h>
static const uint32 (*input)[2];
static uint32 frame, observed_mask;
uint32 xport_input_read(sint32 controller) { return input[frame][controller]; }
sint32 run(const uint32 (*samples)[2],uint32 count,uint32 controllers,uint32 skip_buttons,sint32 skip_held) {
 struct Policy { sint32 skip_held; } policy={0}, *presentation=&policy;
 uint32 held=0,controller,buttons,interruption=0;
 sint32 skipped=0,skip_after_present=0;
 policy.skip_held=skip_held;input=samples;frame=0;observed_mask=0;
'''
        middle = '\n    for(frame=1;frame<count;++frame) {\n' + poll + '\n        if(skipped){observed_mask=interruption;return (sint32)frame;}\n    }\n    return 0;\n}\n'
        checks = r'''
int main(void) {
 const uint32 held_cross[][2]={{PADRdown,0},{PADRdown,0},{PADRdown|PADstart,0},{PADstart,0},{0,0},{PADstart,0}};
 const uint32 held_start[][2]={{PADstart,0},{PADstart|PADRdown,0},{PADRdown,0},{0,0},{PADRdown,0}};
 const uint32 fresh[][2]={{0,0},{PADRdown,0}};
 const uint32 p2[][2]={{0,PADRdown},{PADstart,PADRdown},{PADstart,0},{0,0},{0,PADstart}};
 const uint32 other[][2]={{PADLleft,0},{PADLleft|PADstart,0}};
 const uint32 end_first[][2]={{0,0},{PADRdown,0}};
 const uint32 start_second[][2]={{PADRdown,0},{PADRdown,0},{0,0},{PADRdown,0}};
 assert(run(held_cross,6,1,PADRdown|PADstart,0)==5);
 assert(run(held_start,5,1,PADRdown|PADstart,0)==4);
 assert(run(fresh,2,1,PADRdown|PADstart,0)==1);
 assert(run(p2,5,3,PADRdown|PADstart,0)==4);
 assert(run(p2,5,1,PADRdown|PADstart,0)==1);
 assert(run(held_cross,6,1,PADRdown|PADstart,1)==1);
 assert(observed_mask==PADRdown);
 assert(run(held_cross,6,0,PADRdown|PADstart,0)==0);
 assert(run(held_cross,6,1,0,0)==0);
 assert(run(other,2,1,PADRdown|PADstart,0)==1);
 assert(observed_mask==PADstart);
 assert(run(other,2,1,0xFFFF,1)==1);
 assert(observed_mask==(PADLleft|PADstart));
 assert(run(end_first,2,1,PADRdown|PADstart,0)==1);
 assert(run(start_second,4,1,PADRdown|PADstart,0)==3);
 return 0;
}
'''
        self.compile_run(prefix + entry + middle + checks)

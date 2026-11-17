"""Check shared uploads, page gaps and compatibility of the fullscreen API."""
from pathlib import Path
import unittest
from tests import test_pad_ports as fixture

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(fixture.VCVARS.exists(), 'Requires MSVC')
class FmvPageStrideTests(unittest.TestCase):
    compile_run = fixture.PadPortsTests.compile_run

    def test_page_uploads_preserve_gaps_and_legacy_defaults(self):
        source = (ROOT / 'src/xport_fmv.c').read_text()
        prefix = r'''
#include "psx.h"
#include <assert.h>
#include <string.h>
#define FMV_MAX_WIDTH 640
#define FMV_MAX_HEIGHT 256
static uint16 vram[512][1024];
static unsigned clears, uploads;
static struct Buffers { uint32 slice[16*256*3/4]; uint8 rgb[32*240*3]; } buffers;
static struct { struct Buffers *buffers; } fmv={&buffers};
sint32 ClearImage(PSX_RECT *r,uint8 red,uint8 green,uint8 blue) {
 int x,y;assert(!red && !green && !blue);
 assert(r->x>=0 && r->y>=0 && r->x+r->w<=1024 && r->y+r->h<=512);
 for(y=0;y<r->h;++y)for(x=0;x<r->w;++x)vram[r->y+y][r->x+x]=0;
 ++clears;return 0;
}
sint32 LoadImagePSX(PSX_RECT *r,uint32 *pixels) {
 int y;assert(r->x>=0 && r->y>=0 && r->x+r->w<=1024 && r->y+r->h<=512);
 for(y=0;y<r->h;++y)memcpy(&vram[r->y+y][r->x],(uint8 *)pixels+y*r->w*2,r->w*2);
 ++uploads;return 0;
}
'''
        bodies = ''.join(fixture.function(source, name) for name in
                         ('fmv_rectangle', 'fmv_clear_pages', 'fmv_upload_rgb'))
        checks = r'''
int main(void) {
 XportFmvPresentation p={0};XportFmvFrame f={32,240,1};PSX_RECT rect;
 uint32 i,y;memset(vram,0x5A,sizeof(vram));
 p.width=32;p.height=240;
 assert(fmv_rectangle(32,240,1,&p,256,&rect) && rect.y==256 && rect.h==240);
 assert(fmv_rectangle(32,240,1,&p,0,&rect) && rect.y==240 && rect.h==240);
 assert(!fmv_rectangle(32,240,1,&p,224,&rect));
 assert(!fmv_rectangle(32,240,1,&p,241,&rect));
 assert(!fmv_rectangle(32,240,1,&p,272,&rect));
 p.height=256;assert(!fmv_rectangle(32,240,1,&p,240,&rect));p.height=240;
 p.video_y=1;assert(!fmv_rectangle(32,240,1,&p,256,&rect));p.video_y=0;
 fmv_clear_pages(32,240,256);assert(clears==2);
 assert(!vram[0][0] && !vram[239][47] && !vram[256][0] && !vram[495][47]);
 assert(vram[240][0]==0x5A5A && vram[255][47]==0x5A5A && vram[496][0]==0x5A5A);
 assert(vram[0][48]==0x5A5A);
 for(i=0;i<sizeof(buffers.rgb);++i)buffers.rgb[i]=(uint8)(i*37+11);
 assert(fmv_upload_rgb(&f,1,&p,256) && uploads==2);
 for(y=0;y<240;++y)assert(!memcmp(vram[256+y],buffers.rgb+y*32*3,32*3));
 assert(vram[240][0]==0x5A5A && vram[255][0]==0x5A5A && vram[496][0]==0x5A5A);
 clears=0;fmv_clear_pages(32,240,0);assert(clears==1);
 assert(!vram[240][0] && !vram[479][47] && vram[496][0]==0x5A5A);
 return 0;
}
'''
        self.compile_run(prefix + bodies + checks)

    def test_legacy_forwarding_and_interruption_results(self):
        source = (ROOT / 'src/xport_fmv.c').read_text()
        entry = fixture.function(source, 'xport_fmv_play_configured')
        begin = entry.index('    if (interrupted_buttons)\n', entry.index('    fmv_close();'))
        result = entry[begin:entry.rindex('}')]
        prefix = r'''
#include "psx.h"
#include <assert.h>
static const XportFmvPresentation *expected;
static unsigned calls;
sint32 xport_fmv_play_configured(const char *path,uint32 last,sint32 repeat,const XportFmvPlayback *p) {
 assert(path && last==123 && repeat==1 && p && p->presentation==expected);
 assert(!p->page_stride && !p->interrupted_buttons && !p->skip_after_present && !p->preserve_volume);++calls;return 2;
}
'''
        body = fixture.function(source, 'xport_fmv_play_presented')
        body += 'static sint32 result(sint32 failed,sint32 skipped,uint32 interruption,uint32 *interrupted_buttons) {\n' + result + '}\n'
        checks = r'''
int main(void) {
 XportFmvPresentation p={0};uint32 mask=99;
 expected=&p;assert(xport_fmv_play_presented("clip",123,1,&p)==2);
 expected=0;assert(xport_fmv_play_presented("clip",123,1,0)==2 && calls==2);
 assert(result(0,1,PADstart|PADRdown,&mask)==2 && mask==(PADstart|PADRdown));
 assert(result(0,0,0,&mask)==1 && !mask);
 mask=99;assert(result(1,1,PADstart,&mask)==-1 && !mask);
 assert(result(0,1,PADstart,0)==2);
 return 0;
}
'''
        self.compile_run(prefix + body + checks)


    def test_post_presentation_input_and_volume_policy(self):
        source = (ROOT / 'src/xport_fmv.c').read_text()
        entry = fixture.function(source, 'xport_fmv_play_configured')
        start = entry.index('        if (!failed && !skipped && skip_after_present && presented)')
        stop = entry.index('#if !defined(AP_WEB)', start)
        post = entry[start:stop]
        start = entry.index('    fmv.sound.mask =')
        sound = entry[start:entry.index('    fmv.cleanup =', start)]
        prefix = r"""
#include "psx.h"
#include <assert.h>
#include <string.h>
static uint32 samples[2],reads;
static SpuCommonAttr captured;
static struct { SpuCommonAttr sound; } fmv;
uint32 xport_input_read(sint32 controller) {++reads;return samples[controller];}
void SpuGetCommonAttr(SpuCommonAttr *p) {p->mvol.left=777;p->cd.volume.left=888;}
void SpuSetCommonAttr(SpuCommonAttr *p) {captured=*p;}
static uint32 poll(sint32 failed,sint32 presented,sint32 skip_after_present) {
 uint32 controller,controllers=3,skip_buttons=0xFFFF,interruption=0;
 sint32 skipped=0;
"""
        bodies = prefix + post + 'return interruption;}\n'
        bodies += 'static void setup(sint32 preserve) {XportFmvPlayback options={0},*playback=&options;SpuCommonAttr sound={0};options.preserve_volume=preserve;\n' + sound + '}\n'
        checks = r"""
int main(void) {
 samples[0]=PADstart;samples[1]=PADRdown;reads=0;
 assert(!poll(0,0,1) && !reads);
 assert(!poll(1,1,1) && !reads);
 assert(!poll(0,1,0) && !reads);
 assert(poll(0,1,1)==(PADstart|PADRdown) && reads==2);
 setup(1);assert(captured.mask==SPU_COMMON_CDMIX && captured.cd.mix==SPU_ON);
 assert(fmv.sound.mvol.left==777 && fmv.sound.cd.volume.left==888);
 setup(0);assert(captured.mask & SPU_COMMON_MVOLL);
 assert(captured.mask & SPU_COMMON_CDVOLL);
 assert(captured.mvol.left==0x3FFF && captured.cd.volume.left==0x3FFF);
 return 0;
}
"""
        self.compile_run(bodies + checks)

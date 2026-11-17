"""Exercise actual native FMV source code with transport/decoder boundary doubles"""

import os
from pathlib import Path
import tempfile
import unittest

from tests import test_pad_ports as fixture
from windows_desktop import run_hidden_desktop

ROOT = Path(__file__).resolve().parents[2]


def source_fixture():
    source = (ROOT / 'src/xport_fmv.c').read_text()
    globals_start = source.index('static XportFmvBackend source_backend;')
    globals_end = source.index('static sint32 fmv_uses_source')
    buffers_start = source.index('enum\n{\n    FMV_RING_SECTORS')
    buffers_end = source.index('static struct', buffers_start)
    prefix = r'''
#include "psx_gpu.h"
#include "psx_press.h"
#include "psx_spu.h"
#include "psx_stream.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static uint64 now;
static sint32 mounted, reading, mount_ok=1, read_ok=1, ready_frame, pump_result;
static char mounted_path[260], custom_path[260];
static sint32 decode_ok=1, output_ok=1, gpu_uploads, strips, shutdowns;
static uint32 vlc_limit=146;
static sint32 custom_events, pcm_attached;
static uint16 ring_status;
static StHEADER next_header;
static uint32 compressed[4]={2,0,0,0};
static XportCleanup cleanup_handler;
static void old_cd(uint8 reason,uint8 *result) {(void)reason;(void)result;}
static void old_dct(void) {}
static void old_cleanup(void) {++shutdowns;}
static CdlCB sync_cb=old_cd, read_cb=old_cd, ready_cb=old_cd;
static DecDCTCallback input_cb=old_dct, output_cb=old_dct;
uint64 xport_timer_get(void) {return now;}
void xport_set_cleanup(XportCleanup value) {cleanup_handler=value;}
XportCleanup xport_get_cleanup(void) {return cleanup_handler;}
sint32 cd_mount_stream_file(const char *path) {assert(path);assert(strlen(path)<sizeof(mounted_path));memcpy(mounted_path,path,strlen(path)+1);mounted=mount_ok;return mounted;}
void cd_unmount_image(void) {mounted=0;reading=0;}
CdlCB CdSyncCallback(CdlCB value) {CdlCB old=sync_cb;sync_cb=value;return old;}
CdlCB CdReadCallback(CdlCB value) {CdlCB old=read_cb;read_cb=value;return old;}
CdlCB CdReadyCallback(CdlCB value) {CdlCB old=ready_cb;ready_cb=value;return old;}
DecDCTCallback DecDCTinCallback(DecDCTCallback value) {DecDCTCallback old=input_cb;input_cb=value;return old;}
DecDCTCallback DecDCToutCallback(DecDCTCallback value) {DecDCTCallback old=output_cb;output_cb=value;return old;}
void DecDCTReset(sint32 mode) {assert(mode==0 || mode==1);}
sint32 DecDCTvlcSize(sint32 value)
 {uint32 old=vlc_limit;vlc_limit=value<2 ? 0xFFFFFFFF : (uint32)value<<1;return (sint32)old;}
void StSetRing(uint32 *data,uint32 count) {assert((data && count==64) || (!data && !count));}
sint32 StSetChannel(uint32 channel) {assert(channel==0);return 0;}
void StSetStream(uint32 mode,uint32 start,uint32 end,void (*a)(void),void (*b)(void))
 {assert(mode==1 && start==0 && end==0xFFFFFFFF && !a && !b);}
void StUnSetRing(void) {ready_frame=0;ring_status=StFREE;}
void stream_set_read_mode(uint32 mode) {assert(mode==0xC0);}
CdlLOC *CdIntToPos(sint32 sector,CdlLOC *location) {assert(sector==0);memset(location,0,sizeof(*location));return location;}
sint32 CdControlB(uint8 command,uint8 *parameter,uint8 *result)
 {(void)parameter;(void)result;if(command==CdlStop)reading=0;else assert(command==CdlSeekL);return 1;}
sint32 CdRead2(sint32 mode) {assert(mode==0xC0);reading=read_ok;return read_ok;}
void cd_set_reading_native(sint32 value) {reading=value;}
void spu_discard_xa_pcm(void) {}
void spu_set_cd_pcm_source(SpuCdPcmSource provider,void *context)
 {(void)context;pcm_attached=provider!=NULL;}
uint16 StNextStatus(uint32 **data,uint32 **header)
 {*data=compressed;*header=(uint32 *)&next_header;return ready_frame ? StCOMPLETE : ring_status;}
sint32 stream_pump(void) {return pump_result;}
uint32 StGetNext(uint32 **data,uint32 **header)
 {*data=compressed;*header=(uint32 *)&next_header;return !ready_frame;}
uint32 StFreeRing(uint32 *data) {assert(data==compressed);ready_frame=0;ring_status=StFREE;return 0;}
sint32 DecDCTvlc(uint32 *data,uint32 *vlc) {assert(data==compressed);vlc[0]=1;return decode_ok ? 0 : -1;}
void DecDCTin(uint32 *vlc,sint32 mode) {assert(vlc[0]==1 && mode==3);strips=0;}
void DecDCTout(uint32 *slice,sint32 words)
 {sint32 i;for(i=0;i<words*4;++i)((uint8 *)slice)[i]=(uint8)(strips*17+i%48);++strips;}
sint32 DecDCToutSync(sint32 mode) {assert(mode==1);return !output_ok;}
sint32 LoadImagePSX(PSX_RECT *rectangle,uint32 *pixels) {(void)rectangle;(void)pixels;++gpu_uploads;return 0;}
static sint32 custom_open(void *context,const char *path) {(void)context;assert(strlen(path)<sizeof(custom_path));memcpy(custom_path,path,strlen(path)+1);++custom_events;return 1;}
static sint32 custom_next(void *context,uint8 *rgb,uint32 capacity,XportFmvFrame *frame)
 {(void)context;(void)rgb;(void)capacity;(void)frame;++custom_events;return 2;}
static void custom_pause(void *context,sint32 paused) {(void)context;assert(paused);++custom_events;}
static void custom_close(void *context) {(void)context;++custom_events;}
static uint32 custom_audio(void *context,sint16 *samples,uint32 frames)
 {(void)context;(void)samples;return frames;}
static sint32 custom_has_audio(void *context) {(void)context;return 1;}
'''
    bodies = source[globals_start:globals_end] + source[buffers_start:buffers_end]
    bodies += 'static sint32 fmv_audio_ready;\n'
    for name in ('xport_fmv_discard_audio', 'fmv_rectangle', 'xport_fmv_decode',
                 'fmv_native_shutdown', 'fmv_native_close', 'fmv_native_open',
                 'fmv_native_pause', 'fmv_native_next', 'xport_fmv_set_backend',
                 'xport_fmv_set_native_source', 'fmv_native_source_path',
                 'xport_fmv_open', 'xport_fmv_next', 'xport_fmv_pause', 'xport_fmv_close'):
        bodies += fixture.function(source, name)
    checks = r'''
static void frame_ready(uint32 number)
{
 memset(&next_header,0,sizeof(next_header));
 next_header.width=32;next_header.height=16;next_header.nSectors=1;
 next_header.frameSize=8;next_header.frameCount=number;ready_frame=1;
}
static void restored(void)
{
 assert(!source_active && !native_source.buffers && !mounted);
 assert(sync_cb==old_cd && read_cb==old_cd && ready_cb==old_cd);
 assert(input_cb==old_dct && output_cb==old_dct && vlc_limit==146);
 assert(cleanup_handler==old_cleanup && !gpu_uploads);
}
int main(void)
{
 uint8 rgb[32*16*3];uint32 x,y,c;XportFmvFrame frame={99,98,97};
 XportFmvBackend backend={custom_open,custom_next,custom_pause,custom_close,custom_audio,custom_has_audio};
 cleanup_handler=old_cleanup;mount_ok=0;
 assert(xport_fmv_open("missing")==-1 && !native_source.buffers);mount_ok=1;
 read_ok=0;assert(xport_fmv_open("read-failure")==-1);restored();read_ok=1;
 player_active=1;assert(xport_fmv_open("busy")==-1);player_active=0;
 assert(xport_fmv_open("standard")==1 && reading);
 assert(xport_fmv_open("busy")==-1 && source_active);
 assert(!xport_fmv_set_backend(&backend,NULL));
 assert(xport_fmv_next(NULL,sizeof(rgb),&frame)==-1);
 assert(xport_fmv_next(rgb,sizeof(rgb),&frame)==0 && frame.number==97);
 frame_ready(1);assert(xport_fmv_next(rgb,1,&frame)==-1 && ready_frame);
 xport_fmv_pause(1);assert(!reading);
 now=10000000;assert(xport_fmv_next(rgb,sizeof(rgb),&frame)==0 && ready_frame);
 xport_fmv_pause(0);assert(reading);
 assert(xport_fmv_next(rgb,sizeof(rgb),&frame)==1 && frame.width==32 && frame.height==16 && frame.number==1);
 for(y=0;y<16;++y)for(x=0;x<32;++x)for(c=0;c<3;++c)
  assert(rgb[(y*32+x)*3+c]==(uint8)((x/16)*17+(x%16)*3+c));
 frame_ready(2);assert(xport_fmv_next(rgb,sizeof(rgb),&frame)==1 && frame.number==2);
 frame_ready(1);assert(xport_fmv_next(rgb,sizeof(rgb),&frame)==1 && frame.number==1);
 pump_result=-3;assert(xport_fmv_next(rgb,sizeof(rgb),&frame)==2);
 assert(xport_fmv_next(rgb,sizeof(rgb),&frame)==2);xport_fmv_close();restored();
 pump_result=0;assert(xport_fmv_open("decode-failure")==1);frame_ready(1);decode_ok=0;
 assert(xport_fmv_next(rgb,sizeof(rgb),&frame)==-1 && !ready_frame);decode_ok=1;xport_fmv_close();restored();
 assert(xport_fmv_open("truncated")==1);ring_status=StBUSY;pump_result=-3;
 assert(xport_fmv_next(rgb,sizeof(rgb),&frame)==-1);xport_fmv_close();restored();
 pump_result=0;assert(xport_fmv_open("shutdown")==1);cleanup_handler();
 restored();assert(shutdowns==1);xport_fmv_close();
 assert(xport_fmv_set_backend(&backend,NULL));assert(xport_fmv_open("custom")==1 && pcm_attached);
 assert(xport_fmv_next(rgb,sizeof(rgb),&frame)==2);xport_fmv_pause(1);xport_fmv_close();
 assert(custom_events==4 && !pcm_attached);assert(xport_fmv_set_backend(NULL,NULL));
 {
  char logical[]="DATA/MENU.DCT", prepared[]="VIDEO/native/MENU.STR", oversized[261];
  assert(xport_fmv_set_native_source(logical,prepared));
  logical[0]='X';prepared[0]='X';
  assert(xport_fmv_open("DATA/MENU.DCT")==1);
  assert(!strcmp(mounted_path,"VIDEO/native/MENU.STR"));
  assert(!xport_fmv_set_native_source("other","other.str"));
  assert(!xport_fmv_set_native_source(NULL,NULL));xport_fmv_close();
  player_active=1;assert(!xport_fmv_set_native_source(NULL,NULL));player_active=0;
  memset(oversized,'a',sizeof(oversized)-1);oversized[sizeof(oversized)-1]=0;
  assert(!xport_fmv_set_native_source("", "other.str"));
  assert(!xport_fmv_set_native_source("other", NULL));
  assert(!xport_fmv_set_native_source("other", oversized));
  assert(xport_fmv_open("DATA/MENU.DCT")==1);
  assert(!strcmp(mounted_path,"VIDEO/native/MENU.STR"));xport_fmv_close();
  assert(xport_fmv_open("DATA/OTHER.STR")==1);
  assert(!strcmp(mounted_path,"DATA/OTHER.STR"));xport_fmv_close();
  assert(xport_fmv_set_backend(&backend,NULL));
  assert(xport_fmv_open("DATA/MENU.DCT")==1);
  assert(!strcmp(custom_path,"DATA/MENU.DCT"));xport_fmv_close();
  assert(xport_fmv_set_backend(NULL,NULL));
  assert(xport_fmv_set_native_source(NULL,NULL));
  assert(xport_fmv_open("DATA/MENU.DCT")==1);
  assert(!strcmp(mounted_path,"DATA/MENU.DCT"));xport_fmv_close();
 }
 return 0;
}
'''
    return prefix + bodies + checks


@unittest.skipUnless(os.name == 'nt' and fixture.VCVARS.exists(), 'Requires native MSVC')
class FmvNativeSourceTests(unittest.TestCase):
    def test_source_lifecycle_and_registered_backend(self):
        self.compile_run(source_fixture())

    def test_neutral_xa_sectors_do_not_feed_str(self):
        source = (ROOT / 'src/psx_stream.c').read_text()
        prefix = r'''
#include "psx_stream.h"
#include <assert.h>
#include <string.h>
static struct {
    sint32 pumping, configured, count, ended;
    const uint8 *emulate;
} stream = {0, 1, 1, 0, NULL};
sint32 StCdIntrFlag;
static uint8 submode;
static sint32 forwarded;
sint32 cd_stream_pop_sector_native(uint8 *raw)
{
    memset(raw, 0, 2352);
    raw[15] = 2;
    raw[18] = raw[22] = submode;
    raw[24] = 0x60;
    raw[25] = 1;
    return 1;
}
sint32 stream_feed_sector(const uint8 *sector, const CdlLOC *location)
{
    assert(location && sector[0] == 0x60 && sector[1] == 1);
    return ++forwarded;
}
'''
        checks = r'''
int main(void)
{
    submode = 0x42;
    assert(stream_pump() == 1 && forwarded == 1);
    submode = 0x20;
    assert(stream_pump() == 1 && forwarded == 1);
    submode = 0x08;
    assert(stream_pump() == 2 && forwarded == 2);
    return 0;
}
'''
        self.compile_run(prefix + fixture.function(source, 'stream_pump') + checks)

    def compile_run(self, source):
        with tempfile.TemporaryDirectory(prefix='xport-fmv-source-') as directory:
            folder = Path(directory)
            defines = '#define WND_TITLE "FMV source test"\n#define WND_WIDTH 512\n#define WND_HEIGHT 256\n#define FIELD_RATE 50\n'
            (folder / 'test.c').write_text(defines + source)
            command = f'call "{fixture.VCVARS}" >nul && cl /nologo /Od /Zi /W3 /WX /wd4100 /wd4244 /I"{ROOT / "src"}" test.c /Fe:test.exe user32.lib && test.exe'
            (folder / 'build.bat').write_text('@echo off\n' + command + '\n')
            environment = dict(os.environ, XPORT_AUDIO_OUTPUT='0')
            result = run_hidden_desktop(['cmd', '/d', '/c', 'build.bat'], cwd=folder,
                                        capture_output=True, text=True, timeout=60, env=environment)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

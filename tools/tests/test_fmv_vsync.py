"""Check actual fullscreen callback save/restore for both native and numeric bindings"""
import unittest
from tests import test_pad_ports as fixture

ROOT = fixture.ROOT


@unittest.skipUnless(fixture.VCVARS.exists(), 'Requires MSVC')
class FmvVsyncTests(unittest.TestCase):
    compile_run = fixture.PadPortsTests.compile_run

    def test_callback_domains_survive_fullscreen_save_restore(self):
        psx = (ROOT / 'src/psx.c').read_text()
        movie = (ROOT / 'src/xport_fmv.c').read_text()
        entry = fixture.function(movie, 'xport_fmv_play_configured')
        begin = entry.index('    {\n        PsxVsyncCallbacks disabled')
        save = entry[begin:entry.index('    fmv.sync =', begin)]
        close = fixture.function(movie, 'fmv_close')
        begin = close.index('    (void)psx_bios_vsync_exchange(')
        restore = close[begin:close.index('    gpu_set_raster(', begin)]
        source = r'''
#include "psx.h"
#include <assert.h>
static uint32 bios_vsync_callback;
static PsxInterruptCallback bios_vsync_native_callback;
static struct { PsxVsyncCallbacks vsync; } fmv;
static void callback(void) {}
'''
        source += fixture.function(psx, 'psx_bios_vsync_exchange')
        source += fixture.function(psx, 'psx_bios_vsync_callback')
        source += fixture.function(psx, 'VSyncCallbackPSX')
        source += 'static void enter_movie(void) {\n' + save + '}\n'
        source += 'static void leave_movie(void) {\n' + restore + '}\n'
        source += r'''
int main(void) {
 uint32 mode, repeat;
 for(mode=0;mode<3;++mode)for(repeat=0;repeat<3;++repeat) {
  (void)psx_bios_vsync_callback(NULL);
  if(mode==1)(void)psx_bios_vsync_callback(callback);
  if(mode==2)(void)VSyncCallbackPSX(0x80011CCC);
  enter_movie();
  assert(!bios_vsync_callback && !bios_vsync_native_callback);
  (void)VSyncCallbackPSX(0x80012710);
  leave_movie();
  assert(bios_vsync_callback==(mode==2 ? 0x80011CCC : 0));
  assert(bios_vsync_native_callback==(mode==1 ? callback : NULL));
 }
 return 0;
}
'''
        self.compile_run(source)

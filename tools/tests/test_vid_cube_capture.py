"""Test actual capture control flow without a graphics context or pacing."""
import os
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
STUB = r"""
#include <assert.h>
#include <string.h>
typedef unsigned uint32;typedef int sint32;
typedef struct{int x,y,w,h;}VID_RECT;
typedef struct{int cube;uint32 width;}TEX;
typedef struct{int released;TEX *texture;}VID_TARGET;
typedef struct{uint32 index,size;float rotation[3][3];}VID_CUBE_FACE;
typedef int(*VID_CUBE_RENDER)(void*,const VID_CUBE_FACE*);
static int frame_active,begin_fail,end_fail,part_fail=-1,render_fail=-1;
static int begins,ends,cancels,calls,parts;
static int vid_begin(float r,float g,float b){++begins;return frame_active=!begin_fail;}
static void vid_cancel(void){frame_active=0;++cancels;}
static int vid_end(void){frame_active=0;++ends;return !end_fail;}
static int vid_target_next_part(uint32 p){assert(frame_active);++parts;return p!=part_fail;}
static int vid_target_select_cube_face(VID_TARGET*t,uint32 p){return vid_target_next_part(p);}
static void vid_viewport(VID_RECT r){assert(r.x==0&&r.y==0&&r.w==64&&r.h==64);}
#include "capture_impl.inc"
static int render(void*c,const VID_CUBE_FACE*f){assert(c==(void*)1&&f->index==calls&&f->size==64);for(int i=0;i<3;i++)for(int j=0;j<3;j++){float d=0;for(int k=0;k<3;k++)d+=f->rotation[i][k]*f->rotation[j][k];assert(d==(i==j?1:0));}assert(f->rotation[2][f->index/2]==(f->index%2?-1:1));++calls;return f->index!=render_fail;}
static void reset(void){frame_active=begin_fail=end_fail=begins=ends=cancels=calls=parts=0;part_fail=render_fail=-1;}
int main(void){TEX tex={1,64};VID_TARGET t={0,&tex};float c[3]={0};
reset();assert(vid_cube_capture(&t,c,render,(void*)1));assert(calls==6&&parts==6&&ends==1&&!cancels&&!frame_active);
for(int i=0;i<6;i++){reset();part_fail=i;assert(!vid_cube_capture(&t,c,render,(void*)1));assert(calls==i&&cancels==1&&!ends&&!frame_active);reset();render_fail=i;assert(!vid_cube_capture(&t,c,render,(void*)1));assert(calls==i+1&&cancels==1&&!ends&&!frame_active);}
reset();frame_active=1;assert(!vid_cube_capture(&t,c,render,0));assert(frame_active&&!begins&&!cancels);
reset();assert(!vid_cube_capture(0,c,render,0));assert(!vid_cube_capture(&t,0,render,0));assert(!vid_cube_capture(&t,c,0,0));t.released=1;assert(!vid_cube_capture(&t,c,render,0));t.released=0;tex.cube=0;assert(!vid_cube_capture(&t,c,render,0));tex.cube=1;assert(!begins);
reset();begin_fail=1;assert(!vid_cube_capture(&t,c,render,0));assert(!calls&&!cancels);
reset();end_fail=1;assert(!vid_cube_capture(&t,c,render,(void*)1));assert(ends==1&&!frame_active);return 0;}
"""

class CubeCapture(unittest.TestCase):
    @unittest.skipUnless(os.name == 'nt', 'MSVC fixture')
    def test_capture_contract(self):
        out = ROOT / '_build/vid-cube-contract'
        out.mkdir(parents=True, exist_ok=True)
        source = (ROOT / 'src/vid_ogl.c').read_text(encoding='utf-8')
        start = source.index('sint32 vid_cube_capture(')
        end = source.index('\nsint32 vid_sample(', start)
        (out / 'capture_impl.inc').write_text(source[start:end], encoding='utf-8')
        (out / 'contract.c').write_text(STUB, encoding='utf-8')
        vswhere = Path(os.environ['ProgramFiles(x86)']) / 'Microsoft Visual Studio/Installer/vswhere.exe'
        installation = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True, creationflags=subprocess.CREATE_NO_WINDOW).strip()
        self.assertTrue(installation, 'MSVC x86 tools not found')
        setup = Path(installation) / 'VC/Auxiliary/Build/vcvarsall.bat'
        self.assertTrue(setup.is_file())
        command = f'call "{setup}" x86 >nul && cl /nologo /TC /std:c11 /W3 contract.c /Fe:contract.exe >compile.log 2>&1 && contract.exe'
        (out/'run.bat').write_text('@echo off\n'+command+'\nexit /b %errorlevel%\n')
        result = subprocess.run(['cmd','/d','/c','run.bat'], cwd=out, capture_output=True, text=True, creationflags=subprocess.CREATE_NO_WINDOW)
        self.assertEqual(result.returncode, 0, ((out/'compile.log').read_text(errors='replace') if (out/'compile.log').exists() else '')+result.stdout+result.stderr)

if __name__ == '__main__':
    unittest.main()

"""check the shared C packer through its public C++ caller boundary"""
import os
from pathlib import Path
import tempfile
import unittest

from tests import test_pad_ports as fixture
from windows_desktop import run_hidden_desktop

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(os.name == 'nt' and fixture.VCVARS.exists(), 'Requires native MSVC')
class MdecRgbPackingTests(unittest.TestCase):
    def test_cpp_linkage_rounding_channels_and_stp(self):
        body = fixture.function((ROOT / 'src/psx_mdec.c').read_text(), 'mdec_rgb_fifteen')
        defines = '#define WND_TITLE "MDEC pack test"\n#define WND_WIDTH 512\n#define WND_HEIGHT 256\n#define FIELD_RATE 60\n'
        checks = r'''
#include "psx_mdec.h"
#include <assert.h>
int main()
{
    assert(mdec_rgb_fifteen(0, 0) == 0);
    assert(mdec_rgb_fifteen(0, 1) == 0x8000);
    assert(mdec_rgb_fifteen(0xFFFFFF, 0) == 0x7FFF);
    assert(mdec_rgb_fifteen(0xFFFFFF, 1) == 0xFFFF);
    for (unsigned int shift = 0; shift < 3; ++shift)
    {
        for (unsigned int component = 0; component < 256; ++component)
        {
            unsigned int rounded = 0;
            while (rounded < 31 && component >= rounded * 8 + 4)
                ++rounded;
            unsigned int rgb = component << (shift * 8);
            unsigned int packed = rounded << (shift * 5);
            assert(mdec_rgb_fifteen(rgb, 0) == packed);
            assert(mdec_rgb_fifteen(rgb, 1) == (packed | 0x8000));
        }
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='xport-mdec-pack-') as directory:
            folder = Path(directory)
            (folder / 'packer.c').write_text(defines + '#include "psx_mdec.h"\n' + body)
            (folder / 'caller.cpp').write_text(defines + checks)
            command = (f'call "{fixture.VCVARS}" >nul && '
                f'cl /nologo /Od /Zi /W3 /WX /I"{ROOT / "src"}" /c packer.c && '
                f'cl /nologo /Od /Zi /W3 /WX /I"{ROOT / "src"}" caller.cpp packer.obj /Fe:test.exe && test.exe')
            (folder / 'build.bat').write_text('@echo off\n' + command + '\n')
            result = run_hidden_desktop(['cmd', '/d', '/c', 'build.bat'], cwd=folder,
                capture_output=True, text=True, timeout=60, env=dict(os.environ, XPORT_AUDIO_OUTPUT='0'))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

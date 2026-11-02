"""Exercise shared matrix and cross-product APIs through the actual GTE."""

import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tests.test_pad_ports import VCVARS, function

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(os.name == 'nt' and VCVARS.exists(), 'Requires MSVC')
class GteMathSdkTests(unittest.TestCase):
    def test_matrix_alias_padding_and_cross_product_state(self):
        source = (ROOT / 'src/psx.c').read_text()
        bodies = {}
        def collect(name):
            if name in bodies:
                return
            try:
                body = function(source, name)
            except AssertionError:
                return
            bodies[name] = body
            for call in set(re.findall(r'\b([A-Za-z_]\w*)\s*\(', body)):
                collect(call)
        for name in ('xport_gte_execute', 'xport_gte_write_data',
                     'xport_gte_read_data', 'xport_gte_read_flag',
                     'xport_gte_write_control', 'xport_gte_read_control',
                     'MulMatrix0', 'MulMatrix2', 'SetMulMatrix',
                     'ApplyMatrixSV', 'OuterProduct0'):
            collect(name)
        declarations = ''.join(body.split('{', 1)[0].strip() + ';\n' for body in bodies.values())
        globals_start = source.index('static PsxGteSnapshot gte;')
        globals_end = source.index('/* Functions. */', globals_start)
        closure = source[globals_start:globals_end] + declarations + ''.join(bodies.values())
        fixture = r'''
#include "psx.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
/* Native fixture memory backend */


'''
        fixture += closure + r'''#include <string.h>


int main(void)
{
    uint32 wire_word_1;
    MATRIX identity = {{{4096,0,0},{0,4096,0},{0,0,4096}}, {17,18,19}};
    MATRIX right = {{{1024,2048,3072},{-4096,512,256},{4,8,-16}}, {21,22,23}};
    MATRIX product, inplace;
    SVECTOR vertex = {32767,-32768,20,12345};
    VECTOR left = {1,2,3,0x12345678}, other = {4,5,6,0x11223344};
    uint32 controls[5], index;
    memcpy(&product, &identity, sizeof(product));
    memcpy(&inplace, &right, sizeof(inplace));
    MulMatrix0(&identity, &right, &product);
    assert(memcmp(product.m, right.m, 18) == 0);
    assert(*(const uint16 *)((uint8 *)&product + 18) == 0xFFFF);
    assert(product.t[0] == 17 && product.t[2] == 19);
    MulMatrix2(&identity, &inplace);
    assert(memcmp(inplace.m, product.m, 20) == 0);
    assert(inplace.t[0] == 21 && inplace.t[2] == 23);
    assert(SetMulMatrix(&identity, &right) == &identity);
    for (index = 0; index < 5; ++index)
        assert(xport_gte_read_control(index) == (index == 4 ? (uint32)-16 : (memcpy(&wire_word_1, (uint8 *)&product + index * 4, sizeof(wire_word_1)), wire_word_1)));
    xport_gte_write_control(5, 123456);
    identity.m[0][0] = 8192;
    identity.m[1][1] = 8192;
    assert(ApplyMatrixSV(&identity, &vertex, &vertex) == &vertex);
    assert(vertex.vx == 32767 && vertex.vy == -32768 && vertex.vz == 20);
    assert(vertex.pad == 12345);
    assert((sint32)xport_gte_read_data(25) == 65534);
    assert((sint32)xport_gte_read_data(26) == -65536);
    assert((xport_gte_read_flag() & 0x01800000) == 0x01800000);
    assert(xport_gte_read_control(5) == 123456);
    for (index = 0; index < 5; ++index) {
        xport_gte_write_control(index, 0x12345678 + index);
        controls[index] = xport_gte_read_control(index);
    }
    OuterProduct0(&left, &other, &left);
    assert(left.vx == -3 && left.vy == 6 && left.vz == -3);
    assert(left.pad == 0x12345678);
    assert((sint32)xport_gte_read_data(25) == -3);
    assert((sint32)xport_gte_read_data(9) == -3);
    for (index = 0; index < 5; ++index)
        assert(xport_gte_read_control(index) == controls[index]);
    left.vx = 32767; left.vy = -32768; left.vz = 32767;
    other.vx = -32768; other.vy = 32767; other.vz = -32768;
    OuterProduct0(&left, &other, &other);
    assert(other.vx == 65535 && other.vy == 0 && other.vz == -65535);
    assert((sint32)xport_gte_read_data(9) == 32767);
    assert((sint32)xport_gte_read_data(11) == -32768);
    assert(other.pad == 0x11223344);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='xport-gte-scale-') as directory:
            folder = Path(directory)
            (folder / 'test.c').write_text(fixture)
            (folder / 'config.h').write_text(
                '#define WND_TITLE "GTE test"\n#define WND_WIDTH 320\n'
                '#define WND_HEIGHT 240\n#define FIELD_RATE 60\n')
            (folder / 'build.bat').write_text(
                f'@echo off\ncall "{VCVARS}" >nul\n'
                f'cl /nologo /Od /Gy /Gw /I. /FIconfig.h /D_CRT_SECURE_NO_WARNINGS '
                f'/I"{ROOT / "src"}" test.c '
                '/Fe:test.exe /link /OPT:REF /INCREMENTAL:NO\n'
                'if errorlevel 1 exit /b 1\ntest.exe\n')
            result = subprocess.run(['cmd', '/d', '/c', 'build.bat'], cwd=folder,
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()

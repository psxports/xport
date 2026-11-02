"""Check exact GPU cache publication and GTE color/fog APIs with native fixtures."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from tests.test_pad_ports import function


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--architecture', choices=('x86', 'x64'), default='x86')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    gte = (ROOT / 'src/psx.c').read_text(encoding='utf-8-sig')
    gpu = (ROOT / 'src/psx_gpu.c').read_text(encoding='utf-8-sig')
    assert gpu.count('scanout_fraction = native_disp_cached = native_draw_cached = 0u;') == 2
    names = ('xport_gte_write_control', 'xport_gte_read_control', 'ReadColorMatrix', 'SetFogFar')
    gpu_names = ('read_s16_le_at', 'gpu_env_clamp', 'gpu_env_clip', 'gpu_put_draw_env_native', 'GetDrawEnv', 'PutDrawEnv')
    closure = '\n'.join(function(gte, name) for name in names)
    closure += '\n' + '\n'.join(function(gpu, name) for name in gpu_names)
    cases = [(10, 320), (-10, 320), (10, -320), (0, 1), (0x7fffffff, 1), (0x80000000, -1), (0x02000000, 1), (0x02000000, 7)]
    seed = 0x87345abc
    for _ in range(256):
        seed = (1664525 * seed + 1013904223) & 0xffffffff
        distance = seed
        seed = (1664525 * seed + 1013904223) & 0xffffffff
        projection = int(seed % 2049) - 1024
        if projection:
            cases.append((distance, projection))
    table = []
    for distance, projection in cases:
        numerator = (-distance * 64) & 0xffffffff
        numerator = numerator - 0x100000000 if numerator & 0x80000000 else numerator
        if numerator == -0x80000000 and projection == -1:
            continue
        quotient = abs(numerator) // abs(projection)
        if (numerator < 0) != (projection < 0):
            quotient = -quotient
        low = quotient & 0xffff
        expected = low | 0xffff0000 if low & 0x8000 else low
        table.append('{0x%08Xu, %d, 0x%08Xu}' % (distance & 0xffffffff, projection, expected))
    prefix = r"""
#include "psx_gpu.h"
#include <assert.h>
#include <limits.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#define VRAM_W GPU_VRAM_WIDTH
#define VRAM_H GPU_VRAM_HEIGHT
static PsxGteSnapshot gte;
static uint16 native_gte_ofx_fraction, native_gte_ofy_fraction;
static uint32 psyq_bound, native_draw_cached, draw_calls;
static DRAWENV native_draw_cache;
static GpuPsyqStateBinding psyq_binding;
static GpuLinkedSubmit linked_submit;
static void *linked_submit_context;
static sint32 linked_submit_failed;
static GpuTransferObserver transfer_observer;
static uint8 fixture_ram[256];
static jmp_buf division_trap;
/* Fixture storage and raster sink do not replace the cache or GTE APIs under test */


sint32 xport_store_u8(void *p, uint32 v) { *(uint8 *)p = (uint8)v; return 1; }
void *psx_addr(uint32 a, size_t n) { assert(a <= sizeof(fixture_ram) && n <= sizeof(fixture_ram) - a); return fixture_ram + a; }
static sint32 gpu_guest_address(const void *p, uint32 *a) { (void)p; (void)a; return 0; }
static void gpu_observe_dma_direction(void) { assert(0); }
static void gpu_observe_ot(uint32 a, const uint32 *p, sint32 n) { (void)a; (void)p; (void)n; assert(0); }
void gpu_draw_env_native(DRAWENV *environment, sint32 x, sint32 y) { assert(environment && x == 0 && y == 0); ++draw_calls; }
__declspec(noreturn) static void fixture_abort(void) { longjmp(division_trap, 1); }
#define abort fixture_abort
"""
    fixture = prefix + closure + '\nstatic const struct { uint32 distance; sint32 projection; uint32 dqa; } fog_cases[] = {\n' + ',\n'.join(table) + '\n};\n'
    fixture += r"""
int main(void)
{
    uint32 values[8] = {0x01020304, 0x89ABCDEF, 0x7FFF8001, 0xFFEEDDCC, 0xABCD8000, 0x12345678, 0x80000000, 0xFFFFFFFF};
    uint32 expected[8] = {0x01020304, 0x89ABCDEF, 0x7FFF8001, 0xFFEEDDCC, 0xFFFF8000, 0x12345678, 0x80000000, 0xFFFFFFFF};
    uint8 guarded[64], untouched[sizeof(DRAWENV)];
    DRAWENV input, output, copy;
    uint32 i;
    assert(sizeof(DRAWENV) == 92 && sizeof(MATRIX) == 32);
    for (i = 0; i < 8; ++i) xport_gte_write_control(16 + i, values[i]);
    memset(guarded, 0xA5, sizeof(guarded));
    ReadColorMatrix((MATRIX *)(guarded + 16));
    assert(memcmp(guarded + 16, expected, 32) == 0);
    for (i = 0; i < 16; ++i) assert(guarded[i] == 0xA5 && guarded[48 + i] == 0xA5);
    for (i = 0; i < 8; ++i) assert(xport_gte_read_control(16 + i) == expected[i]);
    for (i = 0; i < sizeof(fog_cases) / sizeof(fog_cases[0]); ++i) {
        SetFogFar((sint32)fog_cases[i].distance, fog_cases[i].projection);
        assert(xport_gte_read_control(27) == fog_cases[i].dqa);
        assert(xport_gte_read_control(28) == 0x01400000);
    }
    xport_gte_write_control(27, 123);
    xport_gte_write_control(28, 456);
    if (setjmp(division_trap) == 0) { SetFogFar(10, 0); assert(0); }
    assert(xport_gte_read_control(27) == 123 && xport_gte_read_control(28) == 456);
    if (setjmp(division_trap) == 0) { SetFogFar(0x02000000, -1); assert(0); }
    assert(xport_gte_read_control(27) == 123 && xport_gte_read_control(28) == 456);
    memset(&output, 0xA7, sizeof(output));
    memcpy(untouched, &output, sizeof(output));
    assert(GetDrawEnv(&output) == NULL && memcmp(untouched, &output, sizeof(output)) == 0);
    assert(GetDrawEnv(NULL) == NULL);
    for (i = 0; i < sizeof(input); ++i) ((uint8 *)&input)[i] = (uint8)(i * 17 + 3);
    input.clip.x = 1; input.clip.y = 2; input.clip.w = 320; input.clip.h = 240;
    input.isbg = 0; input.dfe = 1; input.dtd = 0;
    assert(PutDrawEnv(&input) == &input && draw_calls == 1);
    memcpy(&copy, &input, sizeof(copy));
    assert(GetDrawEnv(&output) == &output && memcmp(&output, &copy, sizeof(copy)) == 0);
    memset(&input, 0xCC, sizeof(input));
    memset(&output, 0, sizeof(output));
    assert(GetDrawEnv(&output) == &output && memcmp(&output, &copy, sizeof(copy)) == 0);
    psyq_bound = 1;
    psyq_binding.draw_cache = 32;
    input = copy; input.clip.x = 19;
    assert(PutDrawEnv(&input) == &input && draw_calls == 2);
    assert(memcmp(fixture_ram + 32, &input, sizeof(input)) == 0);
    assert(GetDrawEnv(&output) == &output && memcmp(&output, &input, sizeof(input)) == 0);
    assert(memcmp(&native_draw_cache, &copy, sizeof(copy)) == 0);
    psyq_bound = 0; native_draw_cached = 0;
    memcpy(untouched, &output, sizeof(output));
    assert(GetDrawEnv(&output) == NULL && memcmp(untouched, &output, sizeof(output)) == 0);
    return 0;
}
"""
    source = output / 'graphics-readback-test.c'
    source.write_text(fixture, encoding='utf-8')
    (output / 'config.h').write_text('#define WND_TITLE "Graphics SDK test"\n#define WND_WIDTH 320\n#define WND_HEIGHT 240\n#define FIELD_RATE 60\n', encoding='utf-8')
    setup = Path('C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build') / ('vcvars64.bat' if args.architecture == 'x64' else 'vcvars32.bat')
    script = output / 'build-run.bat'
    script.write_text('@echo off\ncall "' + str(setup) + '" >nul\ncl /nologo /Od /MDd /W3 /WX /wd4100 /wd4244 /I. /FIconfig.h /I"' + str(ROOT / 'src') + '" graphics-readback-test.c /Fe:graphics-readback-test.exe\nif errorlevel 1 exit /b %errorlevel%\ngraphics-readback-test.exe\nexit /b %errorlevel%\n', encoding='utf-8')
    with (output / 'build-run.log').open('w', encoding='utf-8') as log:
        result = subprocess.run(['cmd', '/d', '/c', str(script)], cwd=output, stdout=log, stderr=subprocess.STDOUT, timeout=60)
    report = {'result': 'PASS' if result.returncode == 0 else 'FAIL', 'exit_code': result.returncode, 'architecture': args.architecture, 'fog_cases': len(table), 'division_traps': 2, 'reset_invalidation_source_checks': 2, 'fixture_sha256': hashlib.sha256(source.read_bytes()).hexdigest(), 'sdk_sha256': {name: hashlib.sha256((ROOT / 'src' / name).read_bytes()).hexdigest() for name in ('psx.h', 'psx.c', 'psx_gpu.c')}, 'scope': 'Exact current read/write GTE control, matrix/fog functions and GPU draw publication/getter; independent expected values and storage guards. Raster sink and test memory isolate external consumers. Actual GPU reset entrypoints checked statically only; no game/render/Web proof'}
    (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report))
    raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()

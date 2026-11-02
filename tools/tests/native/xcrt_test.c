/* Exercise the complete runtime without linking Microsoft CRT */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <math.h>
#include <errno.h>
#include <float.h>
#include <xmmintrin.h>
#define CHECK(expr, code) do { if (!(expr)) return code; } while (0)

static jmp_buf jump;
static volatile int jumps;
static volatile uint64_t numerator = UINT64_C(0xfedcba9876543210);
static volatile uint64_t denominator = UINT64_C(0x123456789);

__declspec(noinline) static void jump_back(int value)
{
    #ifdef _M_X64
    volatile unsigned char stack[32768];
#else
    volatile unsigned char stack[512];
#endif
    stack[0] = 1;
    stack[sizeof(stack)-1] = 2;
    if (stack[0] + stack[sizeof(stack)-1] == 3)
        longjmp(jump, value);
}

static int test_jump(void)
{
    int value = setjmp(jump);
    if (value == 0)
    {
        jumps = 1;
        jump_back(0);
    }
    CHECK(value == 1 && jumps == 1, 40);
    value = setjmp(jump);
    if (!value)
        jump_back(73);
    CHECK(value == 73, 41);
    return 0;
}

#ifdef _M_X64
int xcrt_test_jump_regs(void);
#else
void _CIatan2(void);
void _libm_sse2_sin_precise(void);
void _libm_sse2_cos_precise(void);
void _libm_sse2_sqrt_precise(void);

static double sin_x86(double value)
{
    double result;
    __asm {
        movsd xmm0, value
        call _libm_sse2_sin_precise
        movsd result, xmm0
    }
    return result;
}

static double cos_x86(double value)
{
    double result;
    __asm {
        movsd xmm0, value
        call _libm_sse2_cos_precise
        movsd result, xmm0
    }
    return result;
}

static double sqrt_x86(double value)
{
    double result;
    __asm {
        movsd xmm0, value
        call _libm_sse2_sqrt_precise
        movsd result, xmm0
    }
    return result;
}

static double atan2_x86(double y, double x)
{
    double result;
    __asm {
        fld y
        fld x
        call _CIatan2
        fstp result
    }
    return result;
}
#define sin sin_x86
#define cos cos_x86
#define sqrt sqrt_x86
#define atan2 atan2_x86
#endif

static int test_math(void)
{
    HMODULE crt = LoadLibraryA("ucrtbase.dll");
    double (__cdecl *ref_sin)(double) = (void *)GetProcAddress(crt, "sin");
    double (__cdecl *ref_cos)(double) = (void *)GetProcAddress(crt, "cos");
    double (__cdecl *ref_atan2)(double, double) = (void *)GetProcAddress(crt, "atan2");
    double (__cdecl *ref_sqrt)(double) = (void *)GetProcAddress(crt, "sqrt");
    double (__cdecl *ref_floor)(double) = (void *)GetProcAddress(crt, "floor");
    double (__cdecl *ref_fmod)(double, double) = (void *)GetProcAddress(crt, "fmod");
    double (__cdecl *own_floor)(double) = floor;
    double (__cdecl *own_fmod)(double, double) = fmod;
    CHECK(crt && ref_sin && ref_cos && ref_atan2 && ref_sqrt && ref_floor && ref_fmod, 50);
    {
        uint64_t state = UINT64_C(0x1b98765a45678901);
        static const uint64_t edges[] = {0, UINT64_C(0x8000000000000000), 1, UINT64_C(0x8000000000000001), UINT64_C(0x000fffffffffffff), UINT64_C(0x0010000000000000), UINT64_C(0x3ff8000000000000), UINT64_C(0xbff8000000000000), UINT64_C(0x7fefffffffffffff), UINT64_C(0x7ff0000000000000), UINT64_C(0x7ff8000000000000)};
        for (int i = 0; i < 16384 + (int)(sizeof(edges)/sizeof(edges[0])); ++i)
        {
            union { uint64_t bits; double value; } x, y, actual, expected;
            state = state * UINT64_C(6364136223846793005) + 1;
            x.bits = i < (int)(sizeof(edges)/sizeof(edges[0])) ? edges[i] : state;
            state = state * UINT64_C(6364136223846793005) + 1;
            y.bits = state;
            actual.value = own_floor(x.value); expected.value = ref_floor(x.value);
            CHECK(actual.bits == expected.bits || (actual.value != actual.value && expected.value != expected.value), 59);
            actual.value = own_fmod(x.value, y.value); expected.value = ref_fmod(x.value, y.value);
            CHECK(actual.bits == expected.bits || (actual.value != actual.value && expected.value != expected.value), 60);
            if (i < (int)(sizeof(edges)/sizeof(edges[0])))
                for (int j = 0; j < (int)(sizeof(edges)/sizeof(edges[0])); ++j)
                {
                    y.bits = edges[j]; actual.value = own_fmod(x.value, y.value); expected.value = ref_fmod(x.value, y.value);
                    CHECK(actual.bits == expected.bits || (actual.value != actual.value && expected.value != expected.value), 61);
                }
        }
    }
    for (int i = -10000; i <= 10000; ++i)
    {
        double x = i * 0.123, delta;
#ifdef _M_X64
        float f = (float)x;
#endif
        delta = sin(x) - ref_sin(x);
        CHECK(delta < 2e-12 && delta > -2e-12, 51);
        delta = cos(x) - ref_cos(x);
        CHECK(delta < 2e-12 && delta > -2e-12, 52);
        delta = atan2(x, 3-x) - ref_atan2(x, 3-x);
        CHECK(delta < 2e-15 && delta > -2e-15, 53);
        CHECK(sqrt(x*x) == ref_sqrt(x*x), 54);
#ifdef _M_X64
        delta = sinf(f) - (float)ref_sin(f);
        CHECK(delta < 2e-7 && delta > -2e-7, 55);
        delta = cosf(f) - (float)ref_cos(f);
        CHECK(delta < 2e-7 && delta > -2e-7, 56);
        delta = atan2f(f, 3-f) - (float)ref_atan2(f, 3-f);
        CHECK(delta < 3e-7 && delta > -3e-7, 57);
        CHECK(sqrtf(f*f) == (float)ref_sqrt(f*f), 58);
        CHECK(floorf(f) == (float)ref_floor(f), 62);
#endif
    }
    FreeLibrary(crt);
    return 0;
}

int xcrt_cpp_initialized(void);

static void exit_c(void)
{
    FILE *file = fopen("xcrt-exit-order.txt", "ab");
    if (!file || fwrite("C", 1, 1, file) != 1 || fclose(file)) ExitProcess(92);
}

static void exit_a(void)
{
    FILE *file = fopen("xcrt-exit-order.txt", "ab");
    if (!file || fwrite("A", 1, 1, file) != 1 || fclose(file)) ExitProcess(93);
}

static void exit_b(void)
{
    FILE *file = fopen("xcrt-exit-order.txt", "ab");
    if (!file || fwrite("B", 1, 1, file) != 1 || fclose(file) || atexit(exit_c)) ExitProcess(94);
}

#ifdef _M_IX86
extern void _ultof3(void);
extern void _ltof3(void);

static uint32_t unsigned_float_bits(uint64_t value)
{
    uint32_t expected, exponent = 0;
    uint64_t scan = value, significand;
    if (!value) return 0;
    while (scan >>= 1) ++exponent;
    if (exponent <= 23) significand = value << (23 - exponent);
    else
    {
        unsigned shift = exponent - 23;
        uint64_t discarded = value & ((UINT64_C(1) << shift) - 1);
        uint64_t half = UINT64_C(1) << (shift - 1);
        significand = value >> shift;
        if (discarded > half || (discarded == half && (significand & 1)))
            ++significand;
        if (significand == UINT64_C(0x1000000))
        {
            significand >>= 1;
            ++exponent;
        }
    }
    expected = ((exponent + 127) << 23) | ((uint32_t)significand & 0x7fffff);
    return expected;
}

static int check_unsigned_float(uint64_t value)
{
    uint32_t low_word = (uint32_t)value, high_word = (uint32_t)(value >> 32), actual;
    float converted;
    __asm {
        mov ecx, low_word
        mov edx, high_word
        call _ultof3
        movss converted, xmm0
    }
    memcpy(&actual, &converted, sizeof(actual));
    return actual == unsigned_float_bits(value);
}

static int check_signed_float(uint64_t bits)
{
    uint32_t low_word = (uint32_t)bits, high_word = (uint32_t)(bits >> 32), actual;
    uint32_t sign = (high_word >> 31) << 31;
    uint64_t magnitude = sign ? 0 - bits : bits;
    float converted;
    __asm {
        mov ecx, low_word
        mov edx, high_word
        call _ltof3
        movss converted, xmm0
    }
    memcpy(&actual, &converted, sizeof(actual));
    return actual == (unsigned_float_bits(magnitude) | sign);
}

static int test_unsigned_float(void)
{
    uint64_t state = 1;
    unsigned bit, iteration;
    CHECK(check_unsigned_float(0), 72);
    CHECK(check_signed_float(0) && check_signed_float(UINT64_C(0x8000000000000000)) &&
          check_signed_float(UINT64_C(0x7fffffffffffffff)), 78);
    for (bit = 0; bit < 64; ++bit)
    {
        uint64_t value = UINT64_C(1) << bit;
        if (bit < 63)
        {
            CHECK(check_signed_float(value) && check_signed_float(0-value) &&
                  check_signed_float(value-1) && check_signed_float(0-(value-1)) &&
                  check_signed_float(value+1) && check_signed_float(0-(value+1)), 79);
            if (bit > 23)
            {
                uint64_t half = UINT64_C(1) << (bit-24);
                for (unsigned multiple = 1; multiple <= 3; multiple += 2)
                    for (int offset = -1; offset <= 1; ++offset)
                    {
                        uint64_t candidate = value + multiple * half + offset;
                        CHECK(check_signed_float(candidate) && check_signed_float(0-candidate), 80);
                    }
            }
        }
        CHECK(check_unsigned_float(value) && check_unsigned_float(value-1) && check_unsigned_float(value+1), 73);
        if (bit > 23)
        {
            uint64_t half = UINT64_C(1) << (bit-24);
            CHECK(check_unsigned_float(value+half-1) && check_unsigned_float(value+half) && check_unsigned_float(value+half+1), 74);
            CHECK(check_unsigned_float(value+3*half-1) && check_unsigned_float(value+3*half) && check_unsigned_float(value+3*half+1), 75);
        }
    }
    CHECK(check_unsigned_float(UINT64_MAX), 76);
    for (iteration = 0; iteration < 16384; ++iteration)
    {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        CHECK(check_unsigned_float(state), 77);
        CHECK(check_signed_float(state), 81);
    }
    return 0;
}
#endif

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command, int show)
{
    char path[MAX_PATH], text[MAX_PATH], *end;
    unsigned char *data = calloc(16, 16);
    FILE *file;
    int result;
    CHECK(xcrt_cpp_initialized() == 37, 25);
    DeleteFileA("xcrt-exit-order.txt");
    CHECK(!atexit(exit_a) && !atexit(exit_b), 26);
#ifdef _M_IX86
    result = test_unsigned_float();
    if (result) return result;
#endif
    CHECK(data != NULL, 1);
    for (int i=0; i<256; ++i) CHECK(data[i] == 0, 2);
    memset(data, 0xab, 256);
    data = realloc(data, 512);
    CHECK(data && data[255] == 0xab, 3);
    memmove(data+1, data, 255);
    CHECK(data[1] == 0xab && memchr(data, 0xab, 256), 4);
    free(data);
    CHECK(calloc(SIZE_MAX, 2) == NULL, 5);
    CHECK(atoi(" -125") == -125, 6);
    CHECK(strtoul("0x1234z", &end, 0) == 0x1234 && *end == 'z', 7);
    CHECK(sprintf(text, "%s:%d", "value", 123) == 9 && !strcmp(text, "value:123"), 8);
    CHECK(snprintf(text, sizeof(text), "%u", 456u) == 3 && !strcmp(text, "456"), 9);
    GetTempPathA(sizeof(path), path);
    CHECK(GetTempFileNameA(path, "xcr", 0, text), 10);
    file = fopen(text, "wb+");
    CHECK(file && fwrite("test", 1, 4, file) == 4 && ftell(file) == 4, 11);
    CHECK(!fseek(file, 0, SEEK_SET) && fread(path, 1, 4, file) == 4 && !memcmp(path, "test", 4), 12);
    CHECK(!fclose(file) && DeleteFileA(text), 13);
    CHECK(numerator / denominator == 0xe0000000ULL, 14);
    CHECK(numerator % denominator == 0x96543210ULL, 15);
    result = test_jump();
    if (result) return result;
#ifdef _M_X64
    result = xcrt_test_jump_regs();
    if (result) return result;
#endif
    result = test_math();
    if (result) return result;
    return 0;
}

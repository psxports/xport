/* Minimal Windows x86 Release runtime */
#if !defined(_M_IX86) || !defined(NDEBUG)
    #error XCRT requires an x86 Release build
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
    #define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _NO_CRT_STDIO_INLINE
    #define _NO_CRT_STDIO_INLINE
#endif
#include <windows.h>
#include <shlwapi.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <errno.h>
#include <setjmp.h>
#include <time.h>
#include <limits.h>
#pragma function(memcpy, memmove, memset, memcmp, memchr, strlen, strcmp, strcpy, strcat, abs)

static HANDLE process_heap;
static INIT_ONCE errno_once = INIT_ONCE_STATIC_INIT;
static DWORD errno_slot;

static float float_extremum(float a, float b, int maximum)
{
    union
    {
        float value;
        uint32_t bits;
    } first = {a}, second = {b};

    if (a != a)
        return b;
    if (b != b)
        return a;
    if (a == b)
    {
        if (a == 0)
            first.bits = maximum ? first.bits & second.bits : first.bits | second.bits;
        return first.value;
    }
    return maximum ? a > b ? a : b : a < b ? a : b;
}

float fminf(float a, float b)
{
    return float_extremum(a, b, 0);
}

float fmaxf(float a, float b)
{
    return float_extremum(a, b, 1);
}

#pragma function(floor)

double floor(double value)
{
    union
    {
        double value;
        uint64_t bits;
    } number = {value};

    int exponent = (int)(number.bits >> 52 & 0x7ff) - 1023;
    if (exponent >= 52)
        return value;
    if (exponent < 0)
    {
        if (!(number.bits & UINT64_C(0x7fffffffffffffff)))
            return value;
        return number.bits >> 63 ? -1.0 : 0.0;
    }
    uint64_t mask = (UINT64_C(1) << (52 - exponent)) - 1;
    if (!(number.bits & mask))
        return value;
    if (number.bits >> 63)
        number.bits += UINT64_C(1) << (52 - exponent);
    number.bits &= ~mask;
    return number.value;
}

// The compiler passes x in ST0 and y in ST1, returning atan2(y, x) in ST0
__declspec(naked) void _CIatan2(void)
{
    __asm {
        fpatan
        ret
    }
}

void *malloc(size_t size)
{
    return HeapAlloc(process_heap, 0, size ? size : 1);
}

void free(void *ptr)
{
    if (ptr)
        HeapFree(process_heap, 0, ptr);
}

void *calloc(size_t count, size_t size)
{
    size_t bytes;
    if (size && count > SIZE_MAX / size)
        return NULL;
    bytes = count * size;
    return HeapAlloc(process_heap, HEAP_ZERO_MEMORY, bytes ? bytes : 1);
}

void *realloc(void *ptr, size_t size)
{
    if (!ptr)
        return malloc(size);
    if (!size)
    {
        free(ptr);
        return NULL;
    }
    return HeapReAlloc(process_heap, 0, ptr, size);
}

__declspec(noreturn) void abort(void)
{
    TerminateProcess((HANDLE)-1, 3);
    for (;;)
    {
    }
}

static BOOL CALLBACK init_errno(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    (void)once;
    (void)parameter;
    (void)context;
    errno_slot = TlsAlloc();
    return errno_slot != TLS_OUT_OF_INDEXES;
}

int *_errno(void)
{
    int *value;
    if (!InitOnceExecuteOnce(&errno_once, init_errno, NULL, NULL))
        abort();
    value = TlsGetValue(errno_slot);
    if (!value)
    {
        value = calloc(1, sizeof(*value));
        if (!value || !TlsSetValue(errno_slot, value))
            abort();
    }
    return value;
}

void *memcpy(void *dst, const void *src, size_t size)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (size--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t size)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if ((uintptr_t)d > (uintptr_t)s && (uintptr_t)d - (uintptr_t)s < size)
        while (size--)
            d[size] = s[size];
    else
        memcpy(dst, src, size);
    return dst;
}

void *memset(void *dst, int value, size_t size)
{
    unsigned char *d = dst;
    while (size--)
        *d++ = (unsigned char)value;
    return dst;
}

int memcmp(const void *left, const void *right, size_t size)
{
    const unsigned char *a = left, *b = right;
    while (size--)
    {
        if (*a != *b)
            return (int)*a - (int)*b;
        ++a;
        ++b;
    }
    return 0;
}

void *memchr(const void *src, int value, size_t size)
{
    const unsigned char *p = src;
    while (size--)
    {
        if (*p == (unsigned char)value)
            return (void *)p;
        ++p;
    }
    return NULL;
}

size_t strlen(const char *src)
{
    const char *end = src;
    while (*end)
        ++end;
    return (size_t)(end - src);
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
    {
        ++a;
        ++b;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t size)
{
    while (size--)
    {
        int difference = (unsigned char)*a - (unsigned char)*b;
        if (difference || !*a)
            return difference;
        ++a;
        ++b;
    }
    return 0;
}

static unsigned char lower(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + 'a' - 'A') : c;
}

int _strnicmp(const char *a, const char *b, size_t size)
{
    while (size--)
    {
        int difference = lower((unsigned char)*a) - lower((unsigned char)*b);
        if (difference || !*a)
            return difference;
        ++a;
        ++b;
    }
    return 0;
}

char *strcpy(char *dst, const char *src)
{
    char *result = dst;
    do
    {
        *dst++ = *src;
    } while (*src++);
    return result;
}

char *strcat(char *dst, const char *src)
{
    strcpy(dst + strlen(dst), src);
    return dst;
}

char *strchr(const char *src, int value)
{
    do
    {
        if (*src == (char)value)
            return (char *)src;
    } while (*src++);
    return NULL;
}

char *strrchr(const char *src, int value)
{
    const char *result = NULL;
    do
    {
        if (*src == (char)value)
            result = src;
    } while (*src++);
    return (char *)result;
}

char *strstr(const char *src, const char *needle)
{
    size_t size = strlen(needle);
    if (!size)
        return (char *)src;
    while (*src)
    {
        if (!strncmp(src, needle, size))
            return (char *)src;
        ++src;
    }
    return NULL;
}

int abs(int value)
{
    return value < 0 ? -value : value;
}

static int space(unsigned char c)
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

static unsigned digit(unsigned char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c = lower(c);
    return c >= 'a' && c <= 'z' ? c - 'a' + 10u : 36u;
}

unsigned long strtoul(const char *src, char **end, int base)
{
    const char *p = src;
    unsigned long value = 0;
    unsigned d;
    int negative = 0, overflow = 0, any = 0;
    if (end)
        *end = (char *)src;
    if (base && (base < 2 || base > 36))
    {
        errno = EINVAL;
        return 0;
    }
    while (space((unsigned char)*p))
        ++p;
    if (*p == '-' || *p == '+')
        negative = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && lower((unsigned char)p[1]) == 'x' && digit((unsigned char)p[2]) < 16u)
    {
        base = 16;
        p += 2;
    }
    if (!base)
        base = *p == '0' ? 8 : 10;
    while ((d = digit((unsigned char)*p)) < (unsigned)base)
    {
        any = 1;
        if (value > (ULONG_MAX - d) / (unsigned)base)
            overflow = 1;
        else
            value = value * (unsigned)base + d;
        ++p;
    }
    if (any && end)
        *end = (char *)p;
    if (overflow)
    {
        errno = ERANGE;
        return ULONG_MAX;
    }
    return negative ? 0ul - value : value;
}

/* FILE pointers carry native handles and are never dereferenced */
FILE *fopen(const char *path, const char *mode)
{
    HANDLE handle;
    DWORD access, creation;
    if (!path || !mode || !*mode || !strchr("rwa", *mode))
        return NULL;
    access = *mode == 'r' ? GENERIC_READ : *mode == 'a' ? FILE_APPEND_DATA : GENERIC_WRITE;
    if (strchr(mode, '+'))
        access |= GENERIC_READ | (*mode == 'a' ? 0 : GENERIC_WRITE);
    creation = *mode == 'r' ? OPEN_EXISTING : *mode == 'w' ? CREATE_ALWAYS : OPEN_ALWAYS;
    handle = CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, creation, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE)
        return NULL;
    if (*mode == 'a')
    {
        SetFilePointer(handle, 0, NULL, FILE_END);
    }
    return (FILE *)handle;
}

int fclose(FILE *stream)
{
    return stream && CloseHandle((HANDLE)stream) ? 0 : EOF;
}

size_t fread(void *dst, size_t size, size_t count, FILE *stream)
{
    DWORD bytes = 0;
    if (!size || !count || !stream || count > MAXDWORD / size)
        return 0;
    ReadFile((HANDLE)stream, dst, (DWORD)(size * count), &bytes, NULL);
    return bytes / size;
}

size_t fwrite(const void *src, size_t size, size_t count, FILE *stream)
{
    DWORD bytes = 0;
    if (!size || !count || !stream || count > MAXDWORD / size)
        return 0;
    WriteFile((HANDLE)stream, src, (DWORD)(size * count), &bytes, NULL);
    return bytes / size;
}

int fseek(FILE *stream, long offset, int origin)
{
    DWORD position;
    if (!stream || origin < SEEK_SET || origin > SEEK_END)
        return -1;
    position = SetFilePointer((HANDLE)stream, offset, NULL, (DWORD)origin);
    return position != INVALID_SET_FILE_POINTER ? 0 : -1;
}

long ftell(FILE *stream)
{
    DWORD position;
    if (!stream)
        return -1;
    position = SetFilePointer((HANDLE)stream, 0, NULL, FILE_CURRENT);
    return position <= LONG_MAX ? (long)position : -1;
}

int snprintf(char *dst, size_t capacity, const char *format, ...)
{
    va_list args;
    int result;
    va_start(args, format);
    result = wvnsprintfA(dst, (int)capacity, format, args);
    va_end(args);
    return result;
}

static const float float_unsigned_offset = 2147483648.0f;

// Compiler helper takes a float in XMM0 and returns an unsigned word in EAX
__declspec(naked) void _ftol3(void)
{
    __asm {
        cvttss2si eax, xmm0
        test eax, eax
        jns done
        ucomiss xmm0, DWORD PTR float_unsigned_offset
        jb done
        sub esp, 16
        movups XMMWORD PTR[esp], xmm0
        subss xmm0, DWORD PTR float_unsigned_offset
        cvttss2si eax, xmm0
        xor eax, 80000000h
        movups xmm0, XMMWORD PTR[esp]
        add esp, 16
    done:
        ret
    }
}

// SSE2 math helpers take and return a double in XMM0
__declspec(naked) void _libm_sse2_sqrt_precise(void)
{
    __asm {
        sqrtsd xmm0, xmm0
        ret
    }
}

__declspec(naked) void _libm_sse2_sin_precise(void)
{
    __asm {
        sub esp, 8
        movsd QWORD PTR[esp], xmm0
        fld QWORD PTR[esp]
        fsin
        fstp QWORD PTR[esp]
        movsd xmm0, QWORD PTR[esp]
        add esp, 8
        ret
    }
}

__declspec(naked) void _libm_sse2_cos_precise(void)
{
    __asm {
        sub esp, 8
        movsd QWORD PTR[esp], xmm0
        fld QWORD PTR[esp]
        fcos
        fstp QWORD PTR[esp]
        movsd xmm0, QWORD PTR[esp]
        add esp, 8
        ret
    }
}

/* Shared division uses hardware DIV for 64 by 32 and at most 32 steps otherwise */
static __declspec(naked) uint64_t divide_words(uint32_t nl, uint32_t nh, uint32_t dl, uint32_t dh, int remainder, int signed_value)
{
    __asm {
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
        sub esp, 8
        xor ebx, ebx
        mov [ebp-16], ebx
        mov [ebp-20], ebx
        mov eax, [ebp+8]
        mov edx, [ebp+12]
        mov esi, [ebp+16]
        mov edi, [ebp+20]
        cmp dword ptr [ebp+28], 0
        je magnitudes_ready
        test edx, edx
        jns numerator_positive
        neg edx
        neg eax
        sbb edx, 0
        mov ebx, 1
    numerator_positive:
        mov [ebp-16], ebx
        test edi, edi
        jns denominator_positive
        neg edi
        neg esi
        sbb edi, 0
        xor ebx, 1
    denominator_positive:
        mov [ebp-20], ebx
    magnitudes_ready:
        test edi, edi
        jnz wide_divisor
        mov ebx, eax
        mov eax, edx
        xor edx, edx
        div esi
        mov edi, eax
        mov eax, ebx
        div esi
        cmp dword ptr [ebp+24], 0
        jne narrow_remainder
        mov edx, edi
        jmp apply_sign
    narrow_remainder:
        mov eax, edx
        xor edx, edx
        jmp apply_sign
    wide_divisor:
        mov ecx, edx
        xor edx, edx
        mov ebx, 32
    divide_bit:
        shl eax, 1
        rcl ecx, 1
        rcl edx, 1
        jc subtract_divisor
        cmp edx, edi
        jb next_bit
        ja subtract_divisor
        cmp ecx, esi
        jb next_bit
    subtract_divisor:
        sub ecx, esi
        sbb edx, edi
        inc eax
    next_bit:
        dec ebx
        jnz divide_bit
        cmp dword ptr [ebp+24], 0
        jne wide_remainder
        xor edx, edx
        jmp apply_sign
    wide_remainder:
        mov eax, ecx
    apply_sign:
        mov ecx, [ebp-20]
        cmp dword ptr [ebp+24], 0
        je sign_ready
        mov ecx, [ebp-16]
    sign_ready:
        test ecx, ecx
        jz divide_done
        neg edx
        neg eax
        sbb edx, 0
    divide_done:
        add esp, 8
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret
    }
}

__declspec(naked) void _allmul(void)
{
    __asm {
        push ebx
        mov eax, [esp+8]
        mov ecx, [esp+16]
        mul ecx
        mov ebx, edx
        mov edx, [esp+12]
        imul edx, ecx
        add ebx, edx
        mov ecx, [esp+20]
        mov edx, [esp+8]
        imul ecx, edx
        mov edx, ebx
        add edx, ecx
        pop ebx
        ret 16
    }
}

#define DIVIDE_HELPER(name, rem, sign) __declspec(naked) void name(void){__asm push ebp __asm mov ebp, esp __asm push sign __asm push rem __asm push[ebp + 20] __asm push[ebp + 16] __asm push[ebp + 12] __asm push[ebp + 8] __asm call divide_words __asm add esp, 24 __asm pop ebp __asm ret 16}
DIVIDE_HELPER(_alldiv, 0, 1)
DIVIDE_HELPER(_aulldiv, 0, 0)
DIVIDE_HELPER(_allrem, 1, 1)
DIVIDE_HELPER(_aullrem, 1, 0)

/* Combined helpers return quotient in EDX:EAX and remainder in EBX:ECX */
#define DIVREM_HELPER(name, sign) __declspec(naked) void name(void){__asm push ebp __asm mov ebp, esp __asm push esi __asm push edi __asm push sign __asm push 0 __asm push[ebp + 20] __asm push[ebp + 16] __asm push[ebp + 12] __asm push[ebp + 8] __asm call divide_words __asm add esp, 24 __asm mov esi, eax __asm mov edi, edx __asm push sign __asm push 1 __asm push[ebp + 20] __asm push[ebp + 16] __asm push[ebp + 12] __asm push[ebp + 8] __asm call divide_words __asm add esp, 24 __asm mov ebx, edx __asm mov ecx, eax __asm mov edx, edi __asm mov eax, esi __asm pop edi __asm pop esi __asm pop ebp __asm ret 16}
DIVREM_HELPER(_alldvrm, 1)
DIVREM_HELPER(_aulldvrm, 0)

__declspec(naked) void _allshl(void)
{
    __asm {
        cmp cl, 64
        jae empty
        cmp cl, 32
        jae shift_high
        shld edx, eax, cl
        shl eax, cl
        ret
    shift_high:
        mov edx, eax
        xor eax, eax
        and cl, 31
        shl edx, cl
        ret
    empty:
        xor eax, eax
        xor edx, edx
        ret
    }
}

__declspec(naked) void _aullshr(void)
{
    __asm {
        cmp cl, 64
        jae empty
        cmp cl, 32
        jae shift_low
        shrd eax, edx, cl
        shr edx, cl
        ret
    shift_low:
        mov eax, edx
        xor edx, edx
        and cl, 31
        shr eax, cl
        ret
    empty:
        xor eax, eax
        xor edx, edx
        ret
    }
}

__declspec(naked) void _allshr(void)
{
    __asm {
        cmp cl, 64
        jae sign_only
        cmp cl, 32
        jae shift_low
        shrd eax, edx, cl
        sar edx, cl
        ret
    shift_low:
        mov eax, edx
        sar edx, 31
        and cl, 31
        sar eax, cl
        ret
    sign_only:
        sar edx, 31
        mov eax, edx
        ret
    }
}

__declspec(naked) int _setjmp3(jmp_buf buffer, int count, ...)
{
    __asm {
        mov edx, [esp+4]
        mov [edx], ebp
        mov [edx+4], ebx
        mov [edx+8], edi
        mov [edx+12], esi
        lea eax, [esp+4]
        mov [edx+16], eax
        mov eax, [esp]
        mov [edx+20], eax
        xor eax, eax
        ret
    }
}

__declspec(noreturn) __declspec(naked) void longjmp(jmp_buf buffer, int value)
{
    __asm {
        mov edx, [esp+4]
        mov eax, [esp+8]
        test eax, eax
        jnz restore
        inc eax
    restore:
        mov ebp, [edx]
        mov ebx, [edx+4]
        mov edi, [edx+8]
        mov esi, [edx+12]
        mov esp, [edx+16]
        jmp dword ptr [edx+20]
    }
}

int _fltused = 0;
int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command, int show);

__declspec(noreturn) void exit(int code)
{
    ExitProcess((UINT)code);
}

__declspec(noreturn) void WinMainCRTStartup(void)
{
    process_heap = GetProcessHeap();
    ExitProcess((UINT)WinMain(GetModuleHandleA(NULL), NULL, GetCommandLineA(), SW_SHOWDEFAULT));
}

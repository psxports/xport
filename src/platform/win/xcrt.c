/* Minimal Windows Release runtime */
#include "../../ap.h"
#if !defined(AP_WIN) || (!defined(AP_X86) && !defined(AP_X64)) || !defined(NDEBUG)
    #error XCRT requires a Windows x86 or x64 Release build
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
#include <stddef.h>

#ifdef AP_X64
typedef char XCRT_JUMP_SIZE[sizeof(jmp_buf) == 256 ? 1 : -1];
typedef char XCRT_JUMP_RIP[offsetof(_JUMP_BUFFER, Rip) == 80 ? 1 : -1];
typedef char XCRT_JUMP_MXCSR[offsetof(_JUMP_BUFFER, MxCsr) == 88 ? 1 : -1];
typedef char XCRT_JUMP_XMM[offsetof(_JUMP_BUFFER, Xmm6) == 96 ? 1 : -1];
#endif
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

int __cdecl _purecall(void)
{
    abort();
    return 0;
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

#if defined(AP_X86)
int tolower(int character)
{
    if (character >= 'A' && character <= 'Z')
        return character + ('a' - 'A');
    return character;
}
#endif

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

size_t strcspn(const char *text, const char *reject)
{
    const char *begin = text;
    while (*text && !strchr(reject, *text))
        ++text;
    return (size_t)(text - begin);
}

char *strncat(char *destination, const char *source, size_t count)
{
    char *begin = destination;
    while (*destination)
        ++destination;
    while (count && *source)
    {
        *destination++ = *source++;
        --count;
    }
    *destination = 0;
    return begin;
}

int toupper(int value)
{
    return value >= 'a' && value <= 'z' ? value - 'a' + 'A' : value;
}

int atoi(const char *text)
{
    unsigned int value = 0;
    int negative;
    while (*text == ' ' || (*text >= '\t' && *text <= '\r'))
        ++text;
    negative = *text == '-';
    if (*text == '-' || *text == '+')
        ++text;
    while (*text >= '0' && *text <= '9')
        value = value * 10 + (unsigned int)(*text++ - '0');
    return (int)(negative ? 0u - value : value);
}

char *strncpy(char *destination, const char *source, size_t count)
{
    char *result = destination;
    while (count && *source)
    {
        *destination++ = *source++;
        --count;
    }
    while (count--)
        *destination++ = 0;
    return result;
}

int sprintf(char *destination, const char *format, ...)
{
    va_list arguments;
    int result;
    va_start(arguments, format);
    result = wvsprintfA(destination, format, arguments);
    va_end(arguments);
    return result;
}

int _fltused = 0;
int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command, int show);

typedef void(__cdecl *XcrtInitializer)(void);

typedef struct XcrtExitEntry
{
    XcrtInitializer callback;
    struct XcrtExitEntry *next;
} XcrtExitEntry;

static SRWLOCK exit_lock = SRWLOCK_INIT;
static XcrtExitEntry *exit_callbacks;

#pragma section(".CRT$XCA", read)
#pragma section(".CRT$XCZ", read)
__declspec(allocate(".CRT$XCA")) XcrtInitializer xcrt_init_begin[1] = {0};
__declspec(allocate(".CRT$XCZ")) XcrtInitializer xcrt_init_end[1] = {0};

int __cdecl atexit(void(__cdecl *callback)(void))
{
    XcrtExitEntry *entry;
    if (!callback)
        return -1;
    entry = (XcrtExitEntry *)malloc(sizeof(*entry));
    if (!entry)
        return -1;
    entry->callback = callback;
    AcquireSRWLockExclusive(&exit_lock);
    entry->next = exit_callbacks;
    exit_callbacks = entry;
    ReleaseSRWLockExclusive(&exit_lock);
    return 0;
}

__declspec(noreturn) void exit(int code)
{
    for (;;)
    {
        XcrtExitEntry *entry;
        XcrtInitializer callback;
        AcquireSRWLockExclusive(&exit_lock);
        entry = exit_callbacks;
        if (!entry)
        {
            ReleaseSRWLockExclusive(&exit_lock);
            break;
        }
        exit_callbacks = entry->next;
        callback = entry->callback;
        ReleaseSRWLockExclusive(&exit_lock);
        free(entry);
        callback();
    }
    ExitProcess((UINT)code);
}

__declspec(noreturn) void WinMainCRTStartup(void)
{
    XcrtInitializer *initializer;
    process_heap = GetProcessHeap();
    for (initializer = xcrt_init_begin + 1; initializer < xcrt_init_end; ++initializer)
        if (*initializer)
            (*initializer)();
    LPSTR command = "";
#if defined(_DEBUG)
    command = GetCommandLineA();
#endif
    exit(WinMain(GetModuleHandleA(NULL), NULL, command, SW_SHOWDEFAULT));
}

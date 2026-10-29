#ifndef AP_H
#define AP_H

#if defined(__EMSCRIPTEN__)
    #define AP_WEB 1
#elif defined(_WIN32)
    #define AP_WIN 1
#endif

#if !defined(AP_WEB)
    #if defined(_M_IX86) || defined(__i386__)
        #define AP_X86 1
    #elif defined(_M_X64) || defined(__x86_64__)
        #define AP_X64 1
    #endif
#endif

#if defined(AP_WIN) && defined(AP_WEB)
    #error AP_WIN and AP_WEB are mutually exclusive
#endif
#if defined(AP_X86) && defined(AP_X64)
    #error AP_X86 and AP_X64 are mutually exclusive
#endif
#if defined(AP_WEB) && (defined(AP_X86) || defined(AP_X64))
    #error Web targets must not select a native x86 architecture
#endif

#endif

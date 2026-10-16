#ifndef XPORT_PLATFORM_WIN_XCRT_H
#define XPORT_PLATFORM_WIN_XCRT_H

#if defined(__cplusplus) && defined(XPORT_XCRT)
    #include <stddef.h>
    #include <stdlib.h>
    #include <new>

    // Define XCRT_CPP_IMPLEMENTATION in exactly one C++ translation unit
    #if defined(XCRT_CPP_IMPLEMENTATION)
namespace std
{
const nothrow_t nothrow;
}

void *operator new(size_t size)
{
    void *object = malloc(size ? size : 1);
    if (!object)
        abort();
    return object;
}

void *operator new[](size_t size)
{
    return ::operator new(size);
}

void operator delete(void *object) throw()
{
    free(object);
}

void operator delete[](void *object) throw()
{
    free(object);
}

void *operator new(size_t size, const std::nothrow_t &) throw()
{
    return malloc(size ? size : 1);
}

void *operator new[](size_t size, const std::nothrow_t &) throw()
{
    return malloc(size ? size : 1);
}

void operator delete(void *object, const std::nothrow_t &) throw()
{
    free(object);
}

void operator delete[](void *object, const std::nothrow_t &) throw()
{
    free(object);
}

        // Current MSVC emits sized delete even for C++98 game source
        #if defined(_MSC_VER) || defined(__cpp_sized_deallocation)
void operator delete(void *object, size_t) throw()
{
    free(object);
}

void operator delete[](void *object, size_t) throw()
{
    free(object);
}
        #endif
    #endif
#endif

#endif

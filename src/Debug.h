#pragma once
void debug_tls(const char* str);
void debug_hex(const char* label, const unsigned char* buf, int len);
#ifdef _XBOX
#include <xtl.h>

// For a thread's __except filter: writes what crashed, where, and the return
// addresses up the stack to DebugInfo.txt, then lets the crash go on as it
// would have. The addresses are looked up in Release\Omni360.map.
LONG LogCrash(const char *where, EXCEPTION_POINTERS *info);

// Defines entry##Guarded, which runs entry with LogCrash watching - for
// CreateThread in its place.
#define GUARDED_THREAD(name, entry)                                                  \
    static DWORD WINAPI entry##Guarded(LPVOID param)                                \
    {                                                                                \
        __try                                                                        \
        {                                                                            \
            return entry(param);                                                     \
        }                                                                            \
        __except (LogCrash(name, GetExceptionInformation()))                         \
        {                                                                            \
            return 0;                                                                \
        }                                                                            \
    }
#endif

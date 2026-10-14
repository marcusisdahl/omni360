#include "Debug.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "OutputConsole.h"
void debug_tls(const char* str)
{
    // dprintf() already appends every line to LOG_FILE_PATH ("game:\
    // DebugInfo.txt") itself - this used to ALSO independently open, write,
    // and close that exact same file right after, writing the same line
    // twice with two separate open/close cycles for no reason.
    if(str)
        dprintf("%s\n", str);
}
void debug_hex(const char* label, const unsigned char* buf, int len)
{
    char line[128];
    char ascii[17];
    int i;

    debug_tls(label);
    for (i = 0; i < len; i++) {
        if (i % 16 == 0) {
            if (i != 0) {
                ascii[16] = 0;
                sprintf(line + strlen(line), "  | %s", ascii);
                debug_tls(line);
            }
            sprintf(line, "%08X: ", i);
        }

        sprintf(line + strlen(line), "%02X ", buf[i]);

        ascii[i % 16] = (buf[i] >= 32 && buf[i] <= 126) ? buf[i] : '.';
    }

    // Final line
    if (i % 16 != 0) {
        int pad = (16 - (i % 16)) * 3;
        for (int j = 0; j < pad; j++) {
            strcat(line, " ");
        }
        ascii[i % 16] = 0;
        sprintf(line + strlen(line), "  | %s", ascii);
        debug_tls(line);
    }
}


#ifdef _XBOX
// Straight to the file, not through dprintf: the thread is crashing, and
// dprintf draws to the screen and formats twice. Opened and closed for each
// line, so what's written is on the drive even if nothing after it runs.
static void CrashLine(const char *format, ...)
{
    char line[256];
    va_list args;
    va_start(args, format);
    _vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    line[sizeof(line) - 1] = '\0';

    FILE *f = fopen(LOG_FILE_PATH, "a");
    if (f != NULL)
    {
        fputs(line, f);
        fclose(f);
    }
}

LONG LogCrash(const char *where, EXCEPTION_POINTERS *info)
{
    __try
    {
        const EXCEPTION_RECORD *record = info->ExceptionRecord;
        const CONTEXT *context = info->ContextRecord;
        CrashLine("\n[crash] %s thread: exception %08lX at %08lX\n", where, (unsigned long)record->ExceptionCode,
                  (unsigned long)(ULONG_PTR)record->ExceptionAddress);
        if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
            CrashLine("[crash] %s address %08lX\n", record->ExceptionInformation[0] ? "writing" : "reading",
                      (unsigned long)record->ExceptionInformation[1]);
        CrashLine("[crash] iar %08lX  lr %08lX  sp %08lX\n", (unsigned long)context->Iar, (unsigned long)context->Lr,
                  (unsigned long)context->Gpr1);

        // The return addresses up the stack: each frame starts with its
        // caller's stack pointer, and the caller's link register is saved
        // 8 bytes below that.
        unsigned long sp = (unsigned long)context->Gpr1;
        for (int i = 0; i < 12 && sp != 0; ++i)
        {
            const unsigned long back = *(const unsigned long *)sp;
            if (back <= sp || back - sp > 1024 * 1024)
                break;
            CrashLine("[crash]   called from %08lX\n", *(const unsigned long *)(back - 8));
            sp = back;
        }
        CrashLine("[crash] look the addresses up in Release\\Omni360.map\n");
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        CrashLine("[crash] (the stack couldn't be read any further)\n");
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

#if defined(PLATFORM_VITA)

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <malloc.h>
#include <new>
#include <psp2/io/fcntl.h>

// Heap sizes must live in a native object as well(bc of LTO).
// the newlib malloc/new arena (PS2 guest RAM 32MB + GS + buffers live here).
extern "C" unsigned int _newlib_heap_size_user = 160u * 1024u * 1024u;
extern "C" unsigned int sceLibcHeapSize = 8u * 1024u * 1024u;
// Recompiled guest code nests C++ calls deeply: give the main thread room.
extern "C" unsigned int sceUserMainThreadStackSize = 8u * 1024u * 1024u;

namespace
{
    // Out of heap: written with sceIo and closed at once, so it survives the
    // abort that follows (buffered logs do not, at least in Vita3K).
    void reportOutOfMemory(size_t size, void *caller)
    {
        const struct mallinfo mi = mallinfo();
        char text[256];
        const int n = std::snprintf(text, sizeof(text), "out of memory: new(%u) from %p, heap used %.1f MB, arena %.1f MB, max %.1f MB\n",
                                    static_cast<unsigned>(size), caller, mi.uordblks / 1048576.0, mi.arena / 1048576.0,
                                    _newlib_heap_size_user / 1048576.0);
        const SceUID fd = sceIoOpen("ux0:data/celosia/oom.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
        if (fd >= 0)
        {
            sceIoWrite(fd, text, n > 0 ? n : 0);
            sceIoClose(fd);
        }
    }
}

void *operator new(size_t size)
{
    if (void *p = std::malloc(size ? size : 1))
        return p;
    reportOutOfMemory(size, __builtin_return_address(0));
    throw std::bad_alloc();
}
void *operator new[](size_t size) { return operator new(size); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept { std::free(p); }
void operator delete[](void *p, size_t) noexcept { std::free(p); }

namespace
{
    // The Vita has no console: send stdout/stderr to a file before main().
    struct LogToFile
    {
        LogToFile()
        {
            std::freopen("ux0:data/celosia/log.txt", "w", stdout);
            std::freopen("ux0:data/celosia/err.txt", "w", stderr);
            std::setvbuf(stdout, nullptr, _IOLBF, 0);
            std::setvbuf(stderr, nullptr, _IONBF, 0);
            std::fputs("[vita] log started\n", stdout);
            // The Vita has no environment: ux0:data/celosia/env.txt holds
            // KEY=VALUE lines (the PS2X_* switches of a scripted run).
            if (FILE *env = std::fopen("ux0:data/celosia/env.txt", "r"))
            {
                char line[4096];
                while (std::fgets(line, sizeof(line), env))
                {
                    line[std::strcspn(line, "\r\n")] = '\0';
                    char *eq = std::strchr(line, '=');
                    if (line[0] == '#' || !eq)
                        continue;
                    *eq = '\0';
                    setenv(line, eq + 1, 1);
                    std::fprintf(stdout, "[vita] env %s=%s\n", line, eq + 1);
                }
                std::fclose(env);
            }
            // The port runs native on the Vita (renderer, sound, IOP, system)
            // unless env.txt says otherwise.
            setenv("SOTC_NATIVE", "1", 0);
            // An uncaught exception (bad_alloc...) aborts without a word on
            // the Vita: say what it was and how full the heap is first.
            std::set_terminate([]
            {
                const char *what = "unknown";
                try
                {
                    if (std::exception_ptr e = std::current_exception())
                        std::rethrow_exception(e);
                }
                catch (const std::exception &ex)
                {
                    what = ex.what();
                }
                catch (...)
                {
                }
                const struct mallinfo mi = mallinfo();
                std::fprintf(stderr, "[vita] terminate: %s (heap used %.1f of %.1f MB)\n", what, mi.uordblks / 1048576.0,
                             _newlib_heap_size_user / 1048576.0);
                std::fprintf(stdout, "[vita] terminate: %s\n", what);
                std::fflush(stdout);
                std::fflush(stderr);
                std::abort();
            });
        }
    };
    LogToFile g_logToFile;

    void ttyPuts(const char *text)
    {
        if (text)
        {
            std::fputs(text, stdout); // Vita TTY (capturable via PrincessLog etc.)
        }
    }
}

// Overrides raylib's debugnet transport with TTY output.
extern "C"
{
    int debugNetInit(const char *, int, int)
    {
        return 0;
    }

    void debugNetFinish(void)
    {
    }

    int debugNetUDPSend(const char *text)
    {
        ttyPuts(text);
        return 0;
    }

    int debugNetUDPPrintf(const char *fmt, ...)
    {
        char line[512];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(line, sizeof(line), fmt, args);
        va_end(args);
        ttyPuts(line);
        return 0;
    }

    int debugNetPrintf(int level, const char *fmt, ...)
    {
        (void)level;
        char line[512];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(line, sizeof(line), fmt, args);
        va_end(args);
        ttyPuts(line);
        return 0;
    }
}

#endif // PLATFORM_VITA

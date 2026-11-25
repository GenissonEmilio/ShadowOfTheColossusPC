#include <memory>
#include "ps2_runtime.h"
#include "games_database.h"
#if defined(PS2X_ENABLE_DEBUG_UI) && !defined(PLATFORM_VITA)
#include "ps2_debug_panel.h"
#endif

#ifdef _DEBUG
#include "ps2_log.h"
#endif

#include <iostream>
#include <string>
#include <filesystem>
#include <exception>
#include <algorithm>
#include <cstdlib>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <charconv>
#include <cstring>
#include <string_view>
#endif

#if defined(__ANDROID__)
#include <android/log.h>
#include <unistd.h>
#include <thread>
#include <cstdio>
#include <cstring>
#endif

namespace
{
#if defined(_WIN32)
    constexpr unsigned firstChanceLogLimit = 8;
    struct WindowsAccessViolation
    {
        DWORD thread;
        const void *instruction;
        uintptr_t access;
        uintptr_t target;

        bool operator==(const WindowsAccessViolation &) const = default;
    };

    WindowsAccessViolation firstChanceLogs[firstChanceLogLimit]{};
    unsigned firstChanceLogCount = 0;
    std::atomic_flag firstChanceLogLock = ATOMIC_FLAG_INIT;
    using MappedFileName = DWORD (WINAPI *)(HANDLE, LPVOID, LPWSTR, DWORD);
    MappedFileName mappedFileName = nullptr;

    void writeWindowsAccessViolation(const EXCEPTION_POINTERS &exception, DWORD thread,
                                     std::string_view phase) noexcept
    {
        // Bypass buffered stdio, heap allocation and the DLL loader lock.
        const auto &record = *exception.ExceptionRecord;
        const DWORD lastError = GetLastError();
        char line[384];
        char *next = line;
        char *const end = line + sizeof(line);
        const auto append = [&](std::string_view text) {
            const auto size = std::min(text.size(), static_cast<size_t>(end - next));
            if (size != 0)
                std::memcpy(next, text.data(), size);
            next += size;
        };
        const auto number = [&](uintptr_t value) {
            next = std::to_chars(next, end, value, 16).ptr;
        };
        append("[crash] 0xc0000005 phase=");
        append(phase);
        append(" thread=0x");
        number(thread);
        const auto instruction = reinterpret_cast<uintptr_t>(record.ExceptionAddress);
        append(" instruction=0x");
        number(instruction);
#if defined(__x86_64__) || defined(_M_X64)
        append(" stack=0x");
        number(exception.ContextRecord->Rsp);
#endif
        MEMORY_BASIC_INFORMATION mapping{};
        if (VirtualQuery(record.ExceptionAddress, &mapping, sizeof(mapping)) && mapping.Type == MEM_IMAGE)
        {
            wchar_t path[MAX_PATH];
            const DWORD length = mappedFileName ? mappedFileName(GetCurrentProcess(), record.ExceptionAddress,
                                                                path, MAX_PATH) : 0;
            if (length > 0 && length < MAX_PATH)
            {
                DWORD start = length;
                while (start > 0 && path[start - 1] != L'\\' && path[start - 1] != L'/')
                    --start;
                if (length > start && length - start <= 96)
                {
                    append(" module=");
                    for (; start < length && next != end; ++start)
                    {
                        const wchar_t ch = path[start];
                        *next++ = ch >= 0x21 && ch <= 0x7e ? static_cast<char>(ch) : '_';
                    }
                }
            }
            append(" rva=0x");
            number(instruction - reinterpret_cast<uintptr_t>(mapping.AllocationBase));
        }
        if (record.NumberParameters >= 2)
        {
            append(" access=0x");
            number(record.ExceptionInformation[0]);
            append(" target=0x");
            number(record.ExceptionInformation[1]);
        }
        append("\r\n");
        DWORD written = 0;
        const HANDLE output = GetStdHandle(STD_ERROR_HANDLE);
        if (output && output != INVALID_HANDLE_VALUE)
            WriteFile(output, line, static_cast<DWORD>(next - line), &written, nullptr);
        SetLastError(lastError);
    }

    LONG WINAPI logFirstChanceWindowsAccessViolation(EXCEPTION_POINTERS *exception) noexcept
    {
        const auto &record = *exception->ExceptionRecord;
        if (record.ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
            return EXCEPTION_CONTINUE_SEARCH;

        // An unwind can fault recursively at the same instruction and target.
        // Leave room for distinct faults, and never wait inside a handler.
        if (!firstChanceLogLock.test_and_set(std::memory_order_acquire))
        {
            const WindowsAccessViolation fault{GetCurrentThreadId(), record.ExceptionAddress,
                record.NumberParameters >= 2 ? record.ExceptionInformation[0] : ~uintptr_t{0},
                record.NumberParameters >= 2 ? record.ExceptionInformation[1] : 0};
            const auto end = firstChanceLogs + firstChanceLogCount;
            const bool isNew = firstChanceLogCount < firstChanceLogLimit &&
                               std::find(firstChanceLogs, end, fault) == end;
            if (isNew)
                firstChanceLogs[firstChanceLogCount++] = fault;
            firstChanceLogLock.clear(std::memory_order_release);
            if (isNew)
                writeWindowsAccessViolation(*exception, fault.thread, "first-chance");
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    LONG WINAPI logUnhandledWindowsAccessViolation(EXCEPTION_POINTERS *exception) noexcept
    {
        const auto &record = *exception->ExceptionRecord;
        if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
            writeWindowsAccessViolation(*exception, GetCurrentThreadId(), "unhandled");

        // Preserve Windows' unhandled-exception processing and crash dumps.
        return EXCEPTION_CONTINUE_SEARCH;
    }
#endif

#if defined(__ANDROID__)
    int g_logcatPipeFds[2]{-1, -1};
    std::thread g_logcatThread;

    void stopLogcatRedirect()
    {
        std::fflush(stdout);
        std::fflush(stderr);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        if (g_logcatPipeFds[1] >= 0)
        {
            close(g_logcatPipeFds[1]);
            g_logcatPipeFds[1] = -1;
        }
        if (g_logcatThread.joinable())
        {
            g_logcatThread.join();
        }
    }

    void redirectStdioToLogcat()
    {
        if (pipe(g_logcatPipeFds) != 0)
        {
            return;
        }

        setvbuf(stdout, nullptr, _IOLBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
        dup2(g_logcatPipeFds[1], STDOUT_FILENO);
        dup2(g_logcatPipeFds[1], STDERR_FILENO);

        g_logcatThread = std::thread([]()
                                     {
                                         FILE *reader = fdopen(g_logcatPipeFds[0], "r");
                                         if (!reader)
                                         {
                                             return;
                                         }
                                         char line[1024];
                                         while (fgets(line, sizeof(line), reader))
                                         {
                                             size_t len = std::strlen(line);
                                             if (len > 0 && line[len - 1] == '\n')
                                             {
                                                 line[len - 1] = '\0';
                                             }
                                             __android_log_write(ANDROID_LOG_INFO, "ps2x", line);
                                         }
                                         fclose(reader);
                                         g_logcatPipeFds[0] = -1;
                                     });
        if (std::atexit(stopLogcatRedirect) != 0)
        {
            close(STDOUT_FILENO);
            close(STDERR_FILENO);
            close(g_logcatPipeFds[1]);
            g_logcatPipeFds[1] = -1;
            if (g_logcatThread.joinable())
            {
                g_logcatThread.join();
            }
        }
    }
#endif

    void setupTerminateLogger() // to help on release build crashs
    {
        std::set_terminate([]()
                           {
                               std::cerr << "[terminate] unhandled exception" << std::endl;
                               const std::exception_ptr ep = std::current_exception();
                               if (ep)
                               {
                                   try
                                   {
                                       std::rethrow_exception(ep);
                                   }
                                   catch (const std::system_error &e)
                                   {
                                       std::cerr << "[terminate] std::system_error code=" << e.code().value()
                                                 << " category=" << e.code().category().name()
                                                 << " message=" << e.what() << std::endl;
                                   }
                                   catch (const std::exception &e)
                                   {
                                       std::cerr << "[terminate] std::exception: " << e.what() << std::endl;
                                   }
                                   catch (...)
                                   {
                                       std::cerr << "[terminate] non-std exception" << std::endl;
                                   }
                               }
                               std::abort(); });
    }

    std::string normalizeGameId(const std::string &folderName)
    {
        std::string result = folderName;

        size_t underscore = result.find('_');
        if (underscore != std::string::npos)
            result[underscore] = '-';

        size_t dot = result.find('.');
        if (dot != std::string::npos)
            result.erase(dot, 1);

        std::ranges::transform(result, result.begin(), [](unsigned char character)
                               { return static_cast<char>(std::toupper(character)); });

        return result;
    }

    std::filesystem::path getExecutablePath(int argc, char *argv[])
    {
        if (argc >= 2 && argv[1] && argv[1][0] != '\0')
        {
            std::cout << "Using argv boot path" << std::endl;
            return std::filesystem::path(argv[1]);
        }
#if defined(PS2X_DEFAULT_BOOT_ELF)
        std::cout << "Using default boot file" << std::endl;
        const std::filesystem::path configuredPath = std::filesystem::path(PS2X_DEFAULT_BOOT_ELF);
#if defined(PLATFORM_VITA)
        return configuredPath;
#endif
        if (configuredPath.is_absolute())
        {
            return configuredPath;
        }
        return (std::filesystem::current_path() / configuredPath).lexically_normal();
#else
        throw std::runtime_error("Unable to determine executable path. Pass the guest ELF as argv[1] or define PS2X_DEFAULT_BOOT_ELF.");
#endif
    }
}

int main(int argc, char *argv[])
{
    if (argc == 2 && std::string(argv[1]) == "--version")
    {
#if defined(_WIN32)
        std::cout << "ShadowOfTheColossusPC Windows runtime 0.2.0\n";
#else
        std::cout << "ShadowOfTheColossusPC Linux runtime 0.2.0\n";
#endif
        return 0;
    }
#if defined(__ANDROID__)
    redirectStdioToLogcat();
#endif
#if defined(__linux__) && !defined(__ANDROID__)
    setenv("SOTC_NATIVE", "1", 0);
    setenv("PS2X_GS_THREADS", "2", 0);
#endif
    setupTerminateLogger();
#if defined(_WIN32)
    const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (kernel)
        mappedFileName = reinterpret_cast<MappedFileName>(GetProcAddress(kernel, "K32GetMappedFileNameW"));
    AddVectoredExceptionHandler(1, logFirstChanceWindowsAccessViolation);
    SetUnhandledExceptionFilter(logUnhandledWindowsAccessViolation);
#endif

    try
    {
        std::filesystem::path pathObj = getExecutablePath(argc, argv);

        std::string filePathStr = pathObj.string();
        std::string elfName = pathObj.filename().string();
        std::string normalizedId = normalizeGameId(elfName);

        // Which renderer draws: the native one (game overrides, SOTC_NATIVE=1)
        // or the emulated PS2 GS (software, or the GPU with PS2X_GS_HW=1).
        const char *native = std::getenv("SOTC_NATIVE");
        std::string windowTitle = (native && std::atoi(native) == 1) ? "Shadow of the Colossus PC (native) | "
                                  : std::getenv("PS2X_GS_HW")             ? "Celosia (emulated GS, GPU) | "
                                                                          : "Celosia (emulated GS) | ";
        const char *gameName = getGameName(normalizedId);

#if !defined(PLATFORM_VITA)
        if (gameName)
        {
            windowTitle += std::string(gameName) + " | " + elfName;
        }
        else
#endif
        {
            windowTitle += elfName;
        }

        // On the heap: the runtime is too large for small main-thread stacks (Vita).
        std::cout << "[main] creating runtime (" << sizeof(PS2Runtime) << " bytes)" << std::endl;
        auto runtimeOwner = std::make_unique<PS2Runtime>();
        PS2Runtime &runtime = *runtimeOwner;
        std::cout << "[main] runtime created" << std::endl;
#if defined(PS2X_ENABLE_DEBUG_UI) && !defined(PLATFORM_VITA)
        // This hook is to prevent leak rlimgui deps to recompiler etc
        PS2DebugPanel debugPanel;
        runtime.setDebugUiCallbacks(
            [](PS2Runtime &rt, void *userData)
            {
                (void)rt;
                static_cast<PS2DebugPanel *>(userData)->initialize();
            },
            [](PS2Runtime &rt, void *userData)
            {
                static_cast<PS2DebugPanel *>(userData)->draw(rt);
            },
            [](PS2Runtime &rt, void *userData)
            {
                (void)rt;
                static_cast<PS2DebugPanel *>(userData)->shutdown();
            },
            &debugPanel);
#endif
        if (!runtime.initialize(windowTitle.c_str()))
        {
            std::cerr << "Failed to initialize PS2 runtime" << std::endl;
            return 1;
        }

        std::cout << "[main] runtime initialized, loading " << filePathStr << std::endl;
        if (!runtime.loadELF(filePathStr))
        {
            std::cerr << "Failed to load ELF file: " << filePathStr << std::endl;
            return 1;
        }

        runtime.run();

#ifdef _DEBUG
        ps2_log::print_saved_location();
#endif
        std::cout.flush();
        std::cerr.flush();
        std::_Exit(0);
    }
    catch (const std::exception &e)
    {
        std::cerr << "[main] fatal exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[main] fatal exception: unknown" << std::endl;
    }

    std::cout.flush();
    std::cerr.flush();
    std::_Exit(1);
}

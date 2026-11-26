#include <memory>
#include "ps2_runtime.h"
#include "games_database.h"
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
#include "runtime/host_menu.h"
#include "runtime/state_manager.h"
#endif
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
#include "runtime/ps2_android_runtime.h"
#include <android/log.h>
#include <cerrno>
#include <unistd.h>
#include <thread>
#include <cstdio>
#include <fstream>
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
    using ThreadStackLimits = void (WINAPI *)(PULONG_PTR, PULONG_PTR);
    ThreadStackLimits threadStackLimits = nullptr;
    VS_FIXEDFILEINFO ntdllVersion{};

    void cacheNtdllVersion()
    {
        // Read the loaded DLL's VS_VERSIONINFO root before installing handlers.
        constexpr wchar_t versionKey[] = L"VS_VERSION_INFO";
        struct VersionResource
        {
            WORD length;
            WORD valueLength;
            WORD type;
            wchar_t key[sizeof(versionKey) / sizeof(wchar_t)];
            VS_FIXEDFILEINFO value;
        };

        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        const HRSRC resource = ntdll ? FindResourceW(ntdll, MAKEINTRESOURCEW(VS_VERSION_INFO),
                                                     reinterpret_cast<LPCWSTR>(RT_VERSION)) : nullptr;
        const DWORD size = resource ? SizeofResource(ntdll, resource) : 0;
        if (size < sizeof(VersionResource))
            return;
        const HGLOBAL loaded = LoadResource(ntdll, resource);
        const void *data = loaded ? LockResource(loaded) : nullptr;
        if (!data)
            return;

        VersionResource root;
        std::memcpy(&root, data, sizeof(root));
        if (root.length >= sizeof(root) && root.length <= size && root.valueLength >= sizeof(root.value) &&
            root.type == 0 && std::memcmp(root.key, versionKey, sizeof(versionKey)) == 0 &&
            root.value.dwSignature == VS_FFI_SIGNATURE)
            ntdllVersion = root.value;
    }

    void writeWindowsAccessViolation(const EXCEPTION_POINTERS &exception, DWORD thread,
                                     std::string_view phase) noexcept
    {
        // Bypass buffered stdio, heap allocation and the DLL loader lock.
        const auto &record = *exception.ExceptionRecord;
        const DWORD lastError = GetLastError();
        char line[512];
        char *next = line;
        char *const end = line + sizeof(line);
        const auto append = [&](std::string_view text) {
            const auto size = std::min(text.size(), static_cast<size_t>(end - next));
            if (size != 0)
                std::memcpy(next, text.data(), size);
            next += size;
        };
        const auto number = [&](uintptr_t value, int base = 16) {
            next = std::to_chars(next, end, value, base).ptr;
        };
        append("[crash] 0xc0000005 phase=");
        append(phase);
        append(" thread=0x");
        number(thread);
        const auto instruction = reinterpret_cast<uintptr_t>(record.ExceptionAddress);
        append(" instruction=0x");
        number(instruction);
#if defined(__x86_64__) || defined(_M_X64)
        append(" rip=0x");
        number(exception.ContextRecord->Rip);
        append(" stack=0x");
        number(exception.ContextRecord->Rsp);
#endif
        if (threadStackLimits)
        {
            ULONG_PTR low = 0, high = 0;
            threadStackLimits(&low, &high);
            append(" stack_low=0x");
            number(low);
            append(" stack_high=0x");
            number(high);
        }
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
        if (ntdllVersion.dwSignature == VS_FFI_SIGNATURE)
        {
            append(" ntdll_version=");
            number(HIWORD(ntdllVersion.dwFileVersionMS), 10);
            append(".");
            number(LOWORD(ntdllVersion.dwFileVersionMS), 10);
            append(".");
            number(HIWORD(ntdllVersion.dwFileVersionLS), 10);
            append(".");
            number(LOWORD(ntdllVersion.dwFileVersionLS), 10);
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
    void configureAndroidFrameOptions(const std::string &logFile)
    {
        if (!logFile.empty())
        {
            const auto path = std::filesystem::path(logFile).parent_path() / "renderer.env";
            std::ifstream file(path);
            for (std::string line; std::getline(file, line);)
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                const auto split = line.find('=');
                if (split == std::string::npos || line.size() != split + 2 ||
                    (line.back() != '0' && line.back() != '1'))
                    continue;
                const std::string name = line.substr(0, split);
                if (name == "SOTC_RENDER_THREAD" || name == "SOTC_REUSE_NATIVE_FRAME" ||
                    name == "SOTC_ANDROID_FRAME_STATS")
                    setenv(name.c_str(), line.c_str() + split + 1, 0);
            }
        }
        setenv("SOTC_RENDER_THREAD", "1", 0);
    }

    class AndroidLog
    {
    public:
        explicit AndroidLog(const std::string &path)
        {
            int pipeFds[2];
            if (pipe(pipeFds) != 0)
                return;

            std::fflush(stdout);
            std::fflush(stderr);
            m_stdout = dup(STDOUT_FILENO);
            m_stderr = dup(STDERR_FILENO);
            dup2(pipeFds[1], STDOUT_FILENO);
            dup2(pipeFds[1], STDERR_FILENO);
            close(pipeFds[1]);
            m_thread = std::thread(forwardLogs, pipeFds[0], path);
        }

        ~AndroidLog()
        {
            if (!m_thread.joinable())
                return;
            // Flush the pipe writers and restore them so the reader gets EOF.
            std::fflush(stdout);
            std::fflush(stderr);
            restore(m_stdout, STDOUT_FILENO);
            restore(m_stderr, STDERR_FILENO);
            m_thread.join();
        }

    private:
        static void forwardLogs(int reader, const std::string &path)
        {
            FILE *log = path.empty() ? nullptr : std::fopen(path.c_str(), "w");
            char bytes[4096], line[1024];
            size_t length = 0;
            const auto publish = [&]
            {
                line[length] = '\0';
                __android_log_write(ANDROID_LOG_INFO, "ps2x", line);
                length = 0;
            };
            // A FILE* would stay locked during a blocking read. Guest libc can
            // call fflush(nullptr), which walks every FILE in Android's libc.
            for (;;)
            {
                const ssize_t count = read(reader, bytes, sizeof(bytes));
                if (count < 0 && errno == EINTR)
                    continue;
                if (count <= 0)
                    break;
                if (log)
                {
                    std::fwrite(bytes, 1, static_cast<size_t>(count), log);
                    std::fflush(log);
                }
                for (ssize_t i = 0; i < count; ++i)
                {
                    if (bytes[i] == '\n')
                    {
                        publish();
                        continue;
                    }
                    line[length++] = bytes[i];
                    if (length == sizeof(line) - 1)
                        publish();
                }
            }
            if (length)
                publish();
            if (log)
                std::fclose(log);
            close(reader);
        }

        static void restore(int saved, int destination)
        {
            if (saved < 0)
                close(destination);
            else
            {
                dup2(saved, destination);
                close(saved);
            }
        }

        std::thread m_thread;
        int m_stdout = -1, m_stderr = -1;
    };
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
#if defined(__ANDROID__)
        const auto paths = ps2x::android::launchPaths();
        if (paths.bootElf.empty() || paths.discDirectory.empty())
            throw std::runtime_error("The disc and boot ELF paths must be set by the Android launcher.");
        return std::filesystem::path(paths.discDirectory) / paths.bootElf;
#elif defined(PS2X_DEFAULT_BOOT_ELF)
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
    const auto paths = ps2x::android::launchPaths();
    AndroidLog log(paths.logFile);
    if (!SOTC_HAS_GAME)
    {
        std::cerr << "[main] This build has no generated Shadow of the Colossus game code." << std::endl;
        return 1;
    }
    setenv("SOTC_NATIVE", "1", 1);
    configureAndroidFrameOptions(paths.logFile);
#if defined(SOTC_ANDROID_CAPTURE_MODULES)
    if (!paths.logFile.empty())
    {
        const auto captureFile = std::filesystem::path(paths.logFile).parent_path() / "capture-gamecore.bin";
        setenv("PS2X_DUMP_RAM_ON_MISSING", captureFile.c_str(), 1);
    }
#endif
    const std::string renderScale = std::to_string(paths.resolutionPercent / 100.0);
    setenv("SOTC_RENDER_SCALE", renderScale.c_str(), 1);
    setenv("PS2X_GS_THREADS", "2", 0);
    if (!paths.saveDirectory.empty())
    {
        setenv("PS2X_MEMCARD_DIR", paths.saveDirectory.c_str(), 1);
        const auto scratch = std::filesystem::path(paths.saveDirectory).parent_path();
        if (chdir(scratch.c_str()) != 0)
            std::cerr << "[main] Could not switch to the private working directory" << std::endl;
    }
#endif
#if defined(__linux__) && !defined(__ANDROID__)
    setenv("SOTC_NATIVE", "1", 0);
    setenv("PS2X_GS_THREADS", "2", 0);
#endif
    setupTerminateLogger();
#if defined(_WIN32)
    const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (kernel)
    {
        mappedFileName = reinterpret_cast<MappedFileName>(GetProcAddress(kernel, "K32GetMappedFileNameW"));
        threadStackLimits = reinterpret_cast<ThreadStackLimits>(GetProcAddress(kernel, "GetCurrentThreadStackLimits"));
    }
    cacheNtdllVersion();
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
#if defined(__ANDROID__)
        constexpr const char *nativeTitle = "Shadow of the Colossus (native) | ";
#else
        constexpr const char *nativeTitle = "Shadow of the Colossus PC (native) | ";
#endif
        std::string windowTitle = (native && std::atoi(native) == 1) ? nativeTitle
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

#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
#if defined(_WIN32)
        const auto *translatedBoot = _wgetenv(L"SOTC_CAMPAIGN_PT_BR_BOOT");
#else
        const auto *translatedBoot = std::getenv("SOTC_CAMPAIGN_PT_BR_BOOT");
#endif
        if (normalizedId == "SCUS-97472" && translatedBoot && *translatedBoot)
        {
            const std::filesystem::path localized(translatedBoot);
            std::error_code status;
            if (localized.filename() == pathObj.filename() && std::filesystem::is_regular_file(localized, status))
            {
                const bool systemPortuguese = std::getenv("SOTC_CAMPAIGN_SYSTEM_PT") != nullptr;
                const auto language = hostmenu::chooseCampaignLanguage(systemPortuguese);
                if (language == hostmenu::CampaignLanguage::Cancel)
                    std::_Exit(0);
                if (language == hostmenu::CampaignLanguage::PortugueseBrazil)
                {
                    filePathStr = localized.string();
                    // Snapshots contain game RAM and cached resources from the selected disc.
                    const auto statePath = savestates::directory() / "pt-BR";
#if defined(_WIN32)
                    _wputenv_s(L"PS2X_STATE_DIR", statePath.c_str());
#else
                    setenv("PS2X_STATE_DIR", statePath.c_str(), 1);
#endif
                    std::cout << "[campaign-language] pt-BR" << std::endl;
                }
                else
                    std::cout << "[campaign-language] en" << std::endl;
            }
        }
#endif

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
#if defined(__ANDROID__)
        return 0;
#else
        std::_Exit(0);
#endif
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
#if defined(__ANDROID__)
    return 1;
#else
    std::_Exit(1);
#endif
}

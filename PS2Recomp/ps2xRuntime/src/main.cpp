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
        std::cout << "ShadowOfTheColossusPC Linux runtime 0.2.0\n";
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

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <filesystem>
#include <string>

namespace
{
    bool validHandle(HANDLE handle)
    {
        return handle && handle != INVALID_HANDLE_VALUE;
    }

    std::wstring quote(const std::wstring &argument)
    {
        // In a Windows command line, backslashes preceding a quote or the
        // closing quote must be doubled for the CRT command-line parser.
        std::wstring result = L"\"";
        std::size_t slashes = 0;
        for (wchar_t c : argument)
        {
            if (c == L'\\')
            {
                ++slashes;
                continue;
            }
            result.append(c == L'"' ? 2 * slashes + 1 : slashes, L'\\');
            result += c;
            slashes = 0;
        }
        result.append(2 * slashes, L'\\');
        result += L'"';
        return result;
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), executable.size());
    if (length == 0 || length == executable.size())
        return 1;
    executable.resize(length);
    const auto root = std::filesystem::path(executable).parent_path();
    const auto prefix = root / L"compiler/toolchain";
    const auto desktop = root / L"desktop";

    SetEnvironmentVariableW(L"SOTC_PROGRAM_DIR", root.c_str());
    SetEnvironmentVariableW(L"SOTC_LAUNCHER_PID", std::to_wstring(GetCurrentProcessId()).c_str());
    SetEnvironmentVariableW(L"PYTHONHOME", prefix.c_str());
    SetEnvironmentVariableW(L"PYTHONNOUSERSITE", L"1");
    SetEnvironmentVariableW(L"PYTHONDONTWRITEBYTECODE", L"1");
    SetEnvironmentVariableW(L"PYTHONPATH", nullptr);
    SetEnvironmentVariableW(L"GI_TYPELIB_PATH", (prefix / L"lib/girepository-1.0").c_str());
    SetEnvironmentVariableW(L"GSETTINGS_SCHEMA_DIR", (prefix / L"share/glib-2.0/schemas").c_str());

    const auto python = prefix / L"bin/python.exe";
    std::wstring command = quote(python.native()) + L" -X utf8 " + quote((desktop / L"windows_bootstrap.py").native());
    int argc = 0;
    bool headless = false;
    if (auto arguments = CommandLineToArgvW(GetCommandLineW(), &argc))
    {
        if (argc > 1)
        {
            const std::wstring mode = arguments[1];
            headless = mode == L"--prepare" || mode == L"--play";
        }
        for (int i = 1; i < argc; ++i)
            command += L" " + quote(arguments[i]);
        LocalFree(arguments);
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    HANDLE nullInput = INVALID_HANDLE_VALUE;
    if (headless && validHandle(GetStdHandle(STD_OUTPUT_HANDLE)) && validHandle(GetStdHandle(STD_ERROR_HANDLE)))
    {
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        if (!validHandle(startup.hStdInput))
        {
            SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
            nullInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            startup.hStdInput = nullInput;
        }
        if (validHandle(startup.hStdInput))
        {
            startup.dwFlags |= STARTF_USESTDHANDLES;
            startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
            startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        }
    }
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(python.c_str(), command.data(), nullptr, nullptr,
                                        (startup.dwFlags & STARTF_USESTDHANDLES) != 0, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &startup, &process);
    if (validHandle(nullInput))
        CloseHandle(nullInput);
    if (!started)
    {
        if (!headless)
            MessageBoxW(nullptr, L"Extraia todos os arquivos do pacote antes de abrir o jogo.",
                        L"Shadow of the Colossus PC", MB_ICONERROR);
        return 1;
    }
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
}

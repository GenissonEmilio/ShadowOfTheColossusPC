#include "ps2_runtime_state_windows.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>

namespace ps2state::windows_file
{
    std::filesystem::path currentExecutable()
    {
        std::wstring path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length >= path.size()) return {};
        path.resize(length);
        return path;
    }

    bool writeAtomic(const std::filesystem::path &path, const std::vector<uint8_t> &bytes, std::string &error)
    {
        const auto directory = path.parent_path();
        std::error_code folderError;
        if (!directory.empty()) std::filesystem::create_directories(directory, folderError);
        if (folderError)
        {
            error = "Cannot create the save-state folder: " + folderError.message();
            return false;
        }

        static std::atomic<uint64_t> sequence{0};
        std::filesystem::path temporary;
        HANDLE file = INVALID_HANDLE_VALUE;
        DWORD failure = ERROR_SUCCESS;
        for (int attempt = 0; attempt < 32; ++attempt)
        {
            const auto nonce = sequence.fetch_add(1, std::memory_order_relaxed);
            temporary = directory / (L".state-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                     std::to_wstring(nonce) + L".tmp");
            file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) break;
            failure = GetLastError();
            if (failure != ERROR_FILE_EXISTS && failure != ERROR_ALREADY_EXISTS) break;
        }
        if (file == INVALID_HANDLE_VALUE)
        {
            error = "Cannot create the temporary save-state file (Windows error " + std::to_string(failure) + ").";
            return false;
        }

        failure = ERROR_SUCCESS;
        for (size_t offset = 0; offset < bytes.size();)
        {
            constexpr size_t chunkSize = 1024 * 1024;
            const auto count = static_cast<DWORD>(std::min(bytes.size() - offset, chunkSize));
            DWORD written = 0;
            if (!WriteFile(file, bytes.data() + offset, count, &written, nullptr))
            {
                failure = GetLastError();
                break;
            }
            if (!written)
            {
                failure = ERROR_WRITE_FAULT;
                break;
            }
            offset += written;
        }
        if (!failure && !FlushFileBuffers(file)) failure = GetLastError();
        if (!CloseHandle(file) && !failure) failure = GetLastError();

        // Commit with one same-directory rename. ReplaceFileW without a backup
        // can remove the previous file on some failed replacements.
        if (!failure && !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            failure = GetLastError();
        if (!failure) return true;

        const bool removed = DeleteFileW(temporary.c_str());
        const DWORD cleanupError = removed ? ERROR_SUCCESS : GetLastError();
        error = "Save-state write failed (Windows error " + std::to_string(failure) + "). The previous state was preserved.";
        if (cleanupError && cleanupError != ERROR_FILE_NOT_FOUND)
            error += " Could not remove the temporary file (Windows error " + std::to_string(cleanupError) + ").";
        return false;
    }
}

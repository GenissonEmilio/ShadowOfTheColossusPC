#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ps2state::windows_file
{
    std::filesystem::path currentExecutable();
    bool writeAtomic(const std::filesystem::path &path, const std::vector<uint8_t> &bytes, std::string &error);
}

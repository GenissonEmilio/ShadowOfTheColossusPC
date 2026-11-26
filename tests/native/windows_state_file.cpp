#include "ps2_runtime_state_windows.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace fs = std::filesystem;
using ps2state::windows_file::currentExecutable;
using ps2state::windows_file::writeAtomic;

static void require(bool passed, const std::string &message)
{
    if (!passed) throw std::runtime_error(message);
}

static std::vector<uint8_t> contents(const fs::path &path)
{
    std::ifstream stream(path, std::ios::binary);
    require(stream.is_open(), "Could not open the saved state");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

static int entries(const fs::path &path)
{
    return std::distance(fs::directory_iterator(path), fs::directory_iterator());
}

int wmain(int argc, wchar_t **argv)
{
    try
    {
        require(argc == 2, "Expected a private test directory");
        require(fs::is_regular_file(currentExecutable()), "Could not identify the current executable");
        const auto root = fs::path(argv[1]) / L"Estados do Eud\u00e1sio \u6587\u5b57";
        require(!fs::exists(root), "Expected a fresh test directory");
        const auto slot = root / L"R\u00e1pido \u6587\u5b57.sotcstate";
        const std::vector<uint8_t> first(1024 * 1024 + 41, 0xDA);
        const std::vector<uint8_t> second(95, 0x51);
        std::string error;

        const bool saved = writeAtomic(slot, first, error);
        require(saved, error);
        require(contents(slot) == first && entries(root) == 1, "First write was incomplete or left a temporary file");

        const HANDLE lock = CreateFileW(slot.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        require(lock != INVALID_HANDLE_VALUE, "Could not lock the existing save against replacement");
        const bool replacedWhileLocked = writeAtomic(slot, second, error);
        const bool unlocked = CloseHandle(lock);
        require(unlocked, "Could not close the fixture's lock");
        require(!replacedWhileLocked && !error.empty(), "Replacing a locked save unexpectedly succeeded");
        require(contents(slot) == first && entries(root) == 1, "Failed publish damaged the original or left a temporary file");

        error.clear();
        const bool replaced = writeAtomic(slot, second, error);
        require(replaced, error);
        require(contents(slot) == second && entries(root) == 1, "Replacing the existing save failed");

        error.clear();
        require(!writeAtomic(slot / L"invalid-parent.sotcstate", first, error) && !error.empty(),
                "A file was accepted as the save directory");
        require(contents(slot) == second && entries(root) == 1, "Invalid save directory changed the existing slot");
        std::cout << "Windows Unicode save-state creation, replacement and failure recovery passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

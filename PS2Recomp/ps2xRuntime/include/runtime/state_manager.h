#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
class PS2Runtime;
namespace savestates
{
    constexpr int SlotCount = 6; // 0 quick, 1..5 manual
    enum class Action { Save, Load, Delete };
    struct Slot { bool exists = false; uint64_t bytes = 0; std::string modified; };
    void bind(PS2Runtime *runtime);
    void request(Action action, int slot = 0);
    bool busy();
    std::string status();
    std::array<Slot, SlotCount> slots();
    std::filesystem::path directory();
    uint32_t checksum(const std::vector<uint8_t> &bytes);
    // Runtime executor side; not callable from the UI thread.
    bool take(Action &action, int &slot);
    void complete(const std::string &message);
}

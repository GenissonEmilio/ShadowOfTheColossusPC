#include "ps2_runtime_macros.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace ps2_stubs
{
    void _sceMcCoreInit(uint8_t *, R5900Context *, PS2Runtime *);
    void _sceMcCoreEnd(uint8_t *, R5900Context *, PS2Runtime *);
    void _sceMcCoreCreateSocket(uint8_t *, R5900Context *, PS2Runtime *);
    void _sceMcCoreWritePage(uint8_t *, R5900Context *, PS2Runtime *);
    void _sceMcCoreReadPage2(uint8_t *, R5900Context *, PS2Runtime *);
}

int main(int argc, char **argv)
{
    namespace fs = std::filesystem;
    constexpr uint32_t pageSize = 512;
    constexpr auto cardSize = (pageSize + 16) * 0x4000;
    const auto mode = argc == 3 ? std::string_view(argv[1]) : std::string_view();
    if (mode != "write" && mode != "read")
    {
        std::cerr << "Usage: memory_card write|read TEST_CARD_DIRECTORY\n";
        return 2;
    }

    std::error_code error;
    const auto directory = fs::canonical(argv[2], error);
    if (error || !directory.native().starts_with("/data/local/tmp/sotc-card-test-work/"))
    {
        std::cerr << "The probe requires a private directory under /data/local/tmp/sotc-card-test-work.\n";
        return 2;
    }
    const auto image = directory / "Mcd001.ps2";
    const auto status = fs::symlink_status(image);
    if ((mode == "write" && fs::exists(status)) ||
        (mode == "read" && (!fs::is_regular_file(status) || fs::file_size(image, error) != cardSize)) ||
        setenv("PS2X_MEMCARD_DIR", directory.c_str(), 1) != 0)
    {
        std::cerr << "The test card's initial state is invalid.\n";
        return 3;
    }

    std::vector<uint8_t> ram(PS2_RAM_SIZE);
    R5900Context ctx{};
    ps2_stubs::_sceMcCoreInit(ram.data(), &ctx, nullptr);
    constexpr uint32_t parameterAddress = 0x2000;
    constexpr uint32_t port = 2;
    std::memcpy(ram.data() + parameterAddress + 4, &port, sizeof(port));
    SET_GPR_U32(&ctx, 4, parameterAddress);
    ps2_stubs::_sceMcCoreCreateSocket(ram.data(), &ctx, nullptr);
    const uint32_t socket = GPR_U32(&ctx, 2);
    if (socket > 1)
    {
        std::cerr << "Could not create the test card socket.\n";
        return 4;
    }

    constexpr uint32_t dataAddress = 0x1000;
    constexpr uint32_t pageNumber = 128;
    auto *buffer = ram.data() + dataAddress;
    std::array<uint8_t, pageSize> pattern{};
    for (size_t i = 0; i < pattern.size(); ++i)
        pattern[i] = static_cast<uint8_t>(i * 17 + 31);

    SET_GPR_U32(&ctx, 4, socket);
    SET_GPR_U32(&ctx, 5, dataAddress);
    SET_GPR_U32(&ctx, 6, pageNumber);
    SET_GPR_U32(&ctx, 7, 1);
    if (mode == "write")
    {
        std::memcpy(buffer, pattern.data(), pattern.size());
        ps2_stubs::_sceMcCoreWritePage(ram.data(), &ctx, nullptr);
    }
    else
    {
        ps2_stubs::_sceMcCoreReadPage2(ram.data(), &ctx, nullptr);
    }
    const bool pageOk = GPR_U32(&ctx, 2) == 1 && std::memcmp(buffer, pattern.data(), pattern.size()) == 0;
    ps2_stubs::_sceMcCoreEnd(ram.data(), &ctx, nullptr);
    if (!pageOk || fs::file_size(image, error) != cardSize || error)
    {
        std::cerr << "Card page verification failed in " << mode << " mode.\n";
        return 5;
    }
    std::cout << "pid=" << getpid() << " mode=" << mode << " page=" << pageNumber
              << " data_bytes=" << pageSize << " card_bytes=" << cardSize << '\n';
}

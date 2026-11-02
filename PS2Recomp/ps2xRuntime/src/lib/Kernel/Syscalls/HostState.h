#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ps2_syscalls
{
    // Executor dispatcher boundary only, with external producers quiesced.
    // Busy RPCs are rejected; host closures/locks/pointers are never archived.
    bool captureKernelHostState(std::vector<uint8_t> &bytes, std::string &error);
    bool validateKernelHostState(const std::vector<uint8_t> &bytes, std::string &error);
    bool restoreKernelHostState(const std::vector<uint8_t> &bytes, std::string &error);
}

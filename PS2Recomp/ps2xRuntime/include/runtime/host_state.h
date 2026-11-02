#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace ps2_stubs {
bool captureCdState(std::vector<uint8_t>&, std::string&);
bool validateCdState(const std::vector<uint8_t>&, std::string&);
bool restoreCdState(const std::vector<uint8_t>&, std::string&);
bool captureMpegState(std::vector<uint8_t>&, std::string&);
bool validateMpegState(const std::vector<uint8_t>&, std::string&);
bool restoreMpegState(const std::vector<uint8_t>&, std::string&);
bool captureCardState(std::vector<uint8_t>&, std::string&);
bool validateCardState(const std::vector<uint8_t>&, std::string&);
bool restoreCardState(const std::vector<uint8_t>&, std::string&);
bool captureAudioState(std::vector<uint8_t>&, std::string&);
bool validateAudioState(const std::vector<uint8_t>&, std::string&);
bool restoreAudioState(const std::vector<uint8_t>&, std::string&);
bool captureLibcState(std::vector<uint8_t>&, std::string&);
bool validateLibcState(const std::vector<uint8_t>&, std::string&);
bool restoreLibcState(const std::vector<uint8_t>&, std::string&);
}
namespace ps2_syscalls {
bool captureFioState(std::vector<uint8_t>&, std::string&);
bool validateFioState(const std::vector<uint8_t>&, std::string&);
bool restoreFioState(const std::vector<uint8_t>&, std::string&);
}

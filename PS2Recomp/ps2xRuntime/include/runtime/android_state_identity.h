#pragma once

#include "runtime/save_state_archive.h"

namespace ps2state::android
{
    inline constexpr size_t MinBuildIdBytes = 8;
    inline constexpr size_t MaxBuildIdBytes = 64;

    inline bool writeBuildId(Writer &writer, const std::vector<uint8_t> &buildId)
    {
        if (buildId.size() < MinBuildIdBytes)
            return writer.fail("Cannot identify the current Android runtime.");
        return writer.vector(buildId, MaxBuildIdBytes);
    }

    inline bool verifyBuildId(Reader &reader, const std::vector<uint8_t> &current)
    {
        std::vector<uint8_t> saved;
        if (!reader.vector(saved, MaxBuildIdBytes))
            return false;
        if (current.size() < MinBuildIdBytes || saved != current)
            return reader.fail("Este estado pertence a outra versão do runtime. Use o save normal do jogo.");
        return true;
    }
}

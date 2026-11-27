#pragma once

#include <cstddef>
#include <cstdint>
#include <regex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace ps2recomp_tests
{
    inline std::unordered_map<uint32_t, std::string> registeredFunctions(const std::string &registration)
    {
        static const std::regex basePattern(R"(g_ps2RecompiledFunctionTableBase = 0x([0-9a-f]+)u;)");
        static const std::regex pointerPattern(R"(\n    ([a-zA-Z_][a-zA-Z_0-9]*),)");
        static const std::regex rangePattern(R"(\n    \{([0-9]+)u, ([0-9]+)u, ([0-9]+)u\},)");
        std::smatch match;
        if (!std::regex_search(registration, match, basePattern))
            throw std::runtime_error("Generated registration has no table base.");
        const auto base = static_cast<uint32_t>(std::stoul(match[1].str(), nullptr, 16));
        const auto begin = registration.begin();
        const auto end = registration.end();

        std::vector<std::string> wrappers;
        for (std::sregex_iterator it(begin, end, pointerPattern), stop; it != stop; ++it)
            wrappers.push_back((*it)[1].str());

        std::unordered_map<uint32_t, std::string> entries;
        for (std::sregex_iterator it(begin, end, rangePattern), stop; it != stop; ++it)
        {
            const auto first = static_cast<uint32_t>(std::stoul((*it)[1].str()));
            const auto count = static_cast<uint32_t>(std::stoul((*it)[2].str()));
            const auto index = static_cast<std::size_t>(std::stoul((*it)[3].str()));
            for (uint32_t slot = first; slot < first + count; ++slot)
                if (!entries.emplace(base + slot * 4u, wrappers.at(index)).second)
                    throw std::runtime_error("Generated registration contains overlapping ranges.");
        }
        return entries;
    }
}

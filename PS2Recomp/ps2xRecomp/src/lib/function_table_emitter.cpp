#include "ps2recomp/Emitters/function_table_emitter.h"
#include "ps2recomp/code_generator.h"
#include "ps2recomp/ps2_recompiler.h"
#include "ps2recomp/recompiler_reporter.h"
#include "ps2recomp/types.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ps2recomp
{
    FunctionTableEmitter::FunctionTableEmitter(CodeGenerator &codeGenerator)
        : m_codeGenerator(codeGenerator)
    {
    }

    std::string FunctionTableEmitter::emit(const std::vector<Function> &functions, const std::map<uint32_t, std::string> &stubs)
    {
        (void)stubs;

        CodeGenerator &cg = m_codeGenerator;
        std::vector<std::pair<uint32_t, std::string>> entries;
        std::unordered_set<uint32_t> registeredAddresses;

        auto addEntry = [&](uint32_t address, const std::string &name)
        {
            if (name.empty())
            {
                return;
            }
            if ((address & 3u) != 0u)
            {
                std::ostringstream oss;
                oss << "Unaligned function table entry for " << name << " at 0x" << std::hex << address;

                if (cg.m_reporter)
                {
                    cg.m_reporter->errorAt("function-table", name, address, oss.str());
                }

                throw std::runtime_error(oss.str());
            }
            if (!registeredAddresses.insert(address).second)
            {
                return;
            }
            entries.emplace_back(address, name);
        };

        std::vector<std::pair<uint32_t, std::string>> normalFunctions;
        std::vector<std::pair<uint32_t, std::string>> stubFunctions;
        std::vector<std::pair<uint32_t, std::string>> systemCallFunctions;
        std::vector<std::pair<uint32_t, std::string>> libraryFunctions;

        for (const auto &function : functions)
        {
            if (!function.isRecompiled && !function.isStub && !function.isSkipped)
                continue;

            std::string generatedName = cg.getFunctionName(function.start);

            if (function.isSkipped)
            {
                libraryFunctions.emplace_back(function.start, generatedName);
            }
            else if (function.isStub)
            {
                const auto target = PS2Recompiler::resolveStubTarget(function.name);
                if (target == StubTarget::Syscall)
                {
                    systemCallFunctions.emplace_back(function.start, generatedName);
                }
                else
                {
                    stubFunctions.emplace_back(function.start, generatedName);
                }
            }
            else
            {
                normalFunctions.emplace_back(function.start, generatedName);
            }
        }

        if (cg.m_bootstrapInfo.valid)
        {
            std::string entryTarget = cg.m_bootstrapInfo.entryName;
            if (entryTarget.empty())
            {
                entryTarget = cg.getFunctionName(cg.m_bootstrapInfo.entry);
            }
            if (entryTarget.empty())
            {
                throw std::runtime_error("No entry function name available for registration.");
            }
            addEntry(cg.m_bootstrapInfo.entry, entryTarget);
        }

        for (const auto &[address, name] : normalFunctions)
        {
            addEntry(address, name);
        }

        for (const auto &[ownerStart, targets] : cg.m_resumeEntryTargetsByOwner)
        {
            const std::string ownerName = cg.getFunctionName(ownerStart);
            if (ownerName.empty())
            {
                continue;
            }

            for (uint32_t target : targets)
            {
                addEntry(target, ownerName);
            }
        }

        for (const auto &[address, name] : stubFunctions)
        {
            addEntry(address, name);
        }
        for (const auto &[address, name] : systemCallFunctions)
        {
            addEntry(address, name);
        }
        for (const auto &[address, name] : libraryFunctions)
        {
            addEntry(address, name);
        }

        std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b)
                  { return a.first < b.first; });

        uint32_t tableBase = 0u;
        uint32_t tableEnd = 0u;
        uint32_t slotCount = 0u;
        if (!entries.empty())
        {
            if (entries.back().first > std::numeric_limits<uint32_t>::max() - 4u)
                throw std::runtime_error("Function table end exceeds the address range.");
            tableBase = entries.front().first;
            tableEnd = entries.back().first + 4u;
            slotCount = (tableEnd - tableBase) >> 2;
        }

        std::stringstream ss;
        ss << "#include <algorithm>\n";
        ss << "#include \"ps2_runtime.h\"\n";
        ss << "#include <ps2_recompiled_functions.h>\n";
        ss << "#include \"ps2_stubs.h\"\n";
        ss << "#include <ps2_recompiled_stubs.h>\n";
        ss << "#include \"ps2_syscalls.h\"\n\n";

        ss << "extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x" << std::hex << tableBase << "u;\n";
        ss << "extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x" << std::hex << tableEnd << "u;\n";
        ss << "extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = " << std::dec << slotCount << "u;\n";
        ss << "PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[" << std::dec << (slotCount == 0u ? 1u : slotCount) << "u] = {};\n\n";

        if (entries.empty())
            return ss.str();

        // A single pointer relocation per wrapper, even for wrappers with
        // many guest entry PCs. The remaining registration data is integers.
        std::unordered_map<std::string, uint32_t> functionIndices;
        ss << "namespace {\n";
        ss << "constexpr PS2Runtime::RecompiledFunction kGeneratedFunctions[] = {\n";
        for (const auto &entry : entries)
        {
            const auto index = static_cast<uint32_t>(functionIndices.size());
            if (functionIndices.try_emplace(entry.second, index).second)
                ss << "    " << entry.second << ",\n";
        }
        ss << "};\n\n";
        ss << "struct GeneratedFunctionRange { uint32_t firstSlot, count, functionIndex; };\n";
        ss << "constexpr GeneratedFunctionRange kGeneratedRanges[] = {\n";
        for (auto first = entries.begin(); first != entries.end();)
        {
            auto next = first + 1;
            while (next != entries.end() && next->first == (next - 1)->first + 4u && next->second == first->second)
                ++next;
            ss << "    {" << ((first->first - tableBase) >> 2) << "u, " << (next - first) << "u, "
               << functionIndices.at(first->second) << "u},\n";
            first = next;
        }
        ss << "};\n\n";
        ss << "struct GeneratedFunctionTableInitializer {\n";
        ss << "    GeneratedFunctionTableInitializer() {\n";
        ss << "        for (const auto &range : kGeneratedRanges)\n";
        ss << "            std::fill_n(g_ps2RecompiledFunctionTable + range.firstSlot, range.count,\n";
        ss << "                        kGeneratedFunctions[range.functionIndex]);\n";
        ss << "    }\n";
        ss << "};\n";
        ss << "static const GeneratedFunctionTableInitializer g_generatedFunctionTableInitializer;\n";
        ss << "}\n";

        return ss.str();
    }
}

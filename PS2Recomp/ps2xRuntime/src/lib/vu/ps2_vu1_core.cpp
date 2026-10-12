#include <vector>
#include <string>
#include <chrono>
#include <mutex>
#include <map>
#include <set>
#include "runtime/ps2_stats.h"
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_vu1_native.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "ps2_vu1_detail.h"

#include <algorithm>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <ps2_log.h>

namespace
{
    constexpr uint8_t laneForComponent(uint32_t component)
    {
        return static_cast<uint8_t>(1u << (3u - component));
    }
}

void VU1Interpreter::addVfRead(InstructionUsage &usage, uint8_t reg, uint8_t lanes)
{
    if (lanes == 0u)
        return;
    for (uint32_t index = 0; index < usage.vfReadCount; ++index)
    {
        if (usage.vfRead[index].reg == reg)
        {
            usage.vfRead[index].lanes |= lanes;
            return;
        }
    }
    if (usage.vfReadCount < usage.vfRead.size())
        usage.vfRead[usage.vfReadCount++] = {reg, lanes};
}

void VU1Interpreter::addVfWrite(InstructionUsage &usage, uint8_t reg, uint8_t lanes)
{
    if (reg == 0u || lanes == 0u)
        return;
    if (usage.vfWrite.reg == 0u)
        usage.vfWrite = {reg, lanes};
    else if (usage.vfWrite.reg == reg)
        usage.vfWrite.lanes |= lanes;
}

uint8_t VU1Interpreter::vfReadLanes(const InstructionUsage &usage, uint8_t reg)
{
    for (uint32_t index = 0; index < usage.vfReadCount; ++index)
    {
        if (usage.vfRead[index].reg == reg)
            return usage.vfRead[index].lanes;
    }
    return 0u;
}

VU1Interpreter::VU1Interpreter(Unit unit)
    : m_unit(unit)
{
    reset();
}

void VU1Interpreter::resetScheduler()
{
    m_nextCommitCycle = UINT64_MAX;
    m_flagPipeline = {};
    m_fdiv = {};
    m_efu = {};
    m_storePipeline = {};
    m_vfWritePipeline = {};
    m_viWritePipeline = {};
    m_accWritePipeline = {};
    resetXgkickState();
    m_vfReady = {};
    m_viReady = {};
    m_accReady = {};
    m_vfLatestWrite = {};
    m_viLatestWrite = {};
    m_accLatestWrite = {};
    m_nextWriteSequence = 0;
    m_efuResourceReady = 0;
    m_workingClip = m_state.clip;
    m_viBranchBackupValue = 0;
    m_viBranchBackupReg = 0;
    m_viBranchBackupValid = false;
    m_stopRequested = false;
    m_pendingHaltD = false;
    m_pendingHaltT = false;
}

void VU1Interpreter::reset()
{
    std::memset(&m_state, 0, sizeof(m_state));
    m_state.vf[0][3] = 1.0f;
    m_state.q = 1.0f;
    m_state.r = 0x3F800000u;
    m_cycle = 0;
    resetScheduler();
}

float VU1Interpreter::broadcast(const float *vf, uint8_t bc)
{
    return normalizeOperand(vf[bc & 3u]);
}

float VU1Interpreter::normalizeOperand(float value) const
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t exponent = (bits >> 23) & 0xFFu;
    if (exponent == 0u)
    {
        bits &= 0x80000000u;
    }
    else if (exponent == 0xFFu)
    {
        bits = (bits & 0x80000000u) | 0x7F7FFFFFu;
    }
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

float VU1Interpreter::normalizeResult(float value, uint32_t &laneFlags) const
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = bits & 0x80000000u;
    const uint32_t magnitude = bits & 0x7FFFFFFFu;
    const uint32_t exponent = (bits >> 23) & 0xFFu;

    laneFlags = sign != 0u ? 0x2u : 0u;
    if (magnitude == 0u)
    {
        laneFlags |= 0x1u;
    }
    else if (exponent == 0u)
    {
        laneFlags |= 0x5u;
        bits = sign;
    }
    else if (exponent == 0xFFu)
    {
        laneFlags |= 0x8u;
        bits = sign | 0x7F7FFFFFu;
    }

    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

uint32_t VU1Interpreter::microAddressMask() const
{
    return m_unit == Unit::VU1 ? 0x3FFFu : 0x0FFFu;
}

int32_t VU1Interpreter::readBranchVi(uint8_t reg) const
{
    if (reg == 0u)
        return 0;
    if (m_viBranchBackupValid &&
        m_viBranchBackupReg == reg)
    {
        return m_viBranchBackupValue;
    }
    return m_state.vi[reg];
}

void VU1Interpreter::recordViWriteForBranch(uint8_t reg, int32_t oldValue)
{
    if (reg == 0u)
        return;
    m_viBranchBackupValue = oldValue;
    m_viBranchBackupReg = reg;
    m_viBranchBackupValid = true;
}

void VU1Interpreter::applyDest(float *dst, const float *result, uint8_t dest)
{
    if (dest & 0x8u)
        dst[0] = result[0];
    if (dest & 0x4u)
        dst[1] = result[1];
    if (dest & 0x2u)
        dst[2] = result[2];
    if (dest & 0x1u)
        dst[3] = result[3];
}

void VU1Interpreter::applyDestAcc(const float *result, uint8_t dest)
{
    applyDest(m_state.acc, result, dest);
}

void VU1Interpreter::normalizeFmacResult(float *result, uint8_t dest,
                                         uint8_t laneFlags[4])
{
    for (uint32_t component = 0; component < 4u; ++component)
    {
        laneFlags[component] = 0u;
        if ((dest & laneForComponent(component)) == 0u)
            continue;

        long double exactResult = 0.0L;
        if (calculateFmacExactResult(component, exactResult))
        {
            laneFlags[component] = normalizeFmacExactResult(result[component], exactResult);
            continue;
        }

        uint32_t flags = 0u;
        result[component] = normalizeResult(result[component], flags);
        laneFlags[component] = static_cast<uint8_t>(flags);
    }
}

bool VU1Interpreter::calculateFmacExactResult(uint32_t component,
                                               long double &result) const
{
    const uint32_t upper = m_currentUpperInstruction;
    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t special = op >= 0x3Cu
                                ? static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu))
                                : 0xFFu;
    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);

    const auto operand = [this](float value)
    {
        return static_cast<long double>(normalizeOperand(value));
    };
    const auto vs = [&](uint32_t lane)
    {
        return operand(m_state.vf[fs][lane]);
    };
    const auto vt = [&](uint32_t lane)
    {
        return operand(m_state.vf[ft][lane]);
    };
    const auto acc = [&](uint32_t lane)
    {
        return operand(m_state.acc[lane]);
    };

    const long double q = operand(m_state.q);
    const long double i = operand(m_state.i);

    if (op < 0x3Cu)
    {
        if (op <= 0x03u)
            result = vs(component) + vt(op & 3u);
        else if (op <= 0x07u)
            result = vs(component) - vt(op & 3u);
        else if (op <= 0x0Bu)
            result = acc(component) + vs(component) * vt(op & 3u);
        else if (op <= 0x0Fu)
            result = acc(component) - vs(component) * vt(op & 3u);
        else if (op >= 0x18u && op <= 0x1Bu)
            result = vs(component) * vt(op & 3u);
        else
        {
            switch (op)
            {
            case 0x1Cu:
                result = vs(component) * q;
                break;
            case 0x1Eu:
                result = vs(component) * i;
                break;
            case 0x20u:
                result = vs(component) + q;
                break;
            case 0x21u:
                result = acc(component) + vs(component) * q;
                break;
            case 0x22u:
                result = vs(component) + i;
                break;
            case 0x23u:
                result = acc(component) + vs(component) * i;
                break;
            case 0x24u:
                result = vs(component) - q;
                break;
            case 0x25u:
                result = acc(component) - vs(component) * q;
                break;
            case 0x26u:
                result = vs(component) - i;
                break;
            case 0x27u:
                result = acc(component) - vs(component) * i;
                break;
            case 0x28u:
                result = vs(component) + vt(component);
                break;
            case 0x29u:
                result = acc(component) + vs(component) * vt(component);
                break;
            case 0x2Au:
                result = vs(component) * vt(component);
                break;
            case 0x2Cu:
                result = vs(component) - vt(component);
                break;
            case 0x2Du:
                result = acc(component) - vs(component) * vt(component);
                break;
            case 0x2Eu:
            {
                static constexpr uint8_t left[4] = {1u, 2u, 0u, 3u};
                static constexpr uint8_t right[4] = {2u, 0u, 1u, 3u};
                result = component == 3u
                             ? 0.0L
                             : acc(component) - vs(left[component]) * vt(right[component]);
                break;
            }
            default:
                return false;
            }
        }
        return true;
    }

    if (special <= 0x03u)
        result = vs(component) + vt(special & 3u);
    else if (special <= 0x07u)
        result = vs(component) - vt(special & 3u);
    else if (special <= 0x0Bu)
        result = acc(component) + vs(component) * vt(special & 3u);
    else if (special <= 0x0Fu)
        result = acc(component) - vs(component) * vt(special & 3u);
    else if (special >= 0x18u && special <= 0x1Bu)
        result = vs(component) * vt(special & 3u);
    else
    {
        switch (special)
        {
        case 0x1Cu:
            result = vs(component) * q;
            break;
        case 0x1Eu:
            result = vs(component) * i;
            break;
        case 0x20u:
            result = vs(component) + q;
            break;
        case 0x21u:
            result = acc(component) + vs(component) * q;
            break;
        case 0x22u:
            result = vs(component) + i;
            break;
        case 0x23u:
            result = acc(component) + vs(component) * i;
            break;
        case 0x24u:
            result = vs(component) - q;
            break;
        case 0x25u:
            result = acc(component) - vs(component) * q;
            break;
        case 0x26u:
            result = vs(component) - i;
            break;
        case 0x27u:
            result = acc(component) - vs(component) * i;
            break;
        case 0x28u:
            result = vs(component) + vt(component);
            break;
        case 0x29u:
            result = acc(component) + vs(component) * vt(component);
            break;
        case 0x2Au:
            result = vs(component) * vt(component);
            break;
        case 0x2Cu:
            result = vs(component) - vt(component);
            break;
        case 0x2Du:
            result = acc(component) - vs(component) * vt(component);
            break;
        case 0x2Eu:
        {
            static constexpr uint8_t left[4] = {1u, 2u, 0u, 3u};
            static constexpr uint8_t right[4] = {2u, 0u, 1u, 3u};
            result = component == 3u
                         ? 0.0L
                         : vs(left[component]) * vt(right[component]);
            break;
        }
        default:
            return false;
        }
    }
    return true;
}

uint8_t VU1Interpreter::normalizeFmacExactResult(float &value,
                                                  long double exactResult) const
{
    const bool negative = std::signbit(exactResult);
    const long double magnitude = std::fabs(exactResult);
    const long double maximum = static_cast<long double>(std::numeric_limits<float>::max());
    const long double minimum = static_cast<long double>(std::numeric_limits<float>::min());
    uint8_t flags = negative ? 0x2u : 0u;

    uint32_t bits = negative ? 0x80000000u : 0u;
    if (magnitude == 0.0L)
    {
        flags |= 0x1u;
        std::memcpy(&value, &bits, sizeof(value));
    }
    else if (magnitude > maximum)
    {
        flags |= 0x8u;
        bits |= 0x7F7FFFFFu;
        std::memcpy(&value, &bits, sizeof(value));
    }
    else if (magnitude < minimum)
    {
        flags |= 0x5u;
        std::memcpy(&value, &bits, sizeof(value));
    }

    return flags;
}

uint32_t VU1Interpreter::calculateFmacProductSticky(uint8_t dest) const
{
    uint32_t extraSticky = 0u;
    const uint32_t upper = m_currentUpperInstruction;
    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t special = op >= 0x3Cu ? static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu)) : 0xFFu;
    const bool productSum =
        (op >= 0x08u && op <= 0x0Fu) ||
        op == 0x21u || op == 0x23u || op == 0x25u || op == 0x27u ||
        op == 0x29u || op == 0x2Du || op == 0x2Eu ||
        (special >= 0x08u && special <= 0x0Fu) ||
        special == 0x21u || special == 0x23u || special == 0x25u ||
        special == 0x27u || special == 0x29u || special == 0x2Du;
    if (!productSum)
        return 0u;

    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((dest & laneForComponent(component)) == 0u)
            continue;
        static constexpr uint8_t crossLeft[4] = {1u, 2u, 0u, 3u};
        static constexpr uint8_t crossRight[4] = {2u, 0u, 1u, 3u};
        const uint8_t leftComponent = op == 0x2Eu ? crossLeft[component] : static_cast<uint8_t>(component);
        const float left = normalizeOperand(m_state.vf[fs][leftComponent]);
        float right = 0.0f;
        if ((op >= 0x08u && op <= 0x0Fu) || (special >= 0x08u && special <= 0x0Fu))
        {
            right = normalizeOperand(m_state.vf[ft][(op >= 0x08u && op <= 0x0Fu ? op : special) & 3u]);
        }
        else if (op == 0x21u || op == 0x25u || special == 0x21u || special == 0x25u)
        {
            right = normalizeOperand(m_state.q);
        }
        else if (op == 0x23u || op == 0x27u || special == 0x23u || special == 0x27u)
        {
            right = normalizeOperand(m_state.i);
        }
        else if (op == 0x2Eu)
        {
            right = normalizeOperand(m_state.vf[ft][crossRight[component]]);
        }
        else
        {
            right = normalizeOperand(m_state.vf[ft][component]);
        }

        float product = left * right;
        const long double exactProduct = static_cast<long double>(left) * static_cast<long double>(right);
        const uint8_t productFlags = normalizeFmacExactResult(product, exactProduct);
        // Product-sum instructions report Z/S/U/O from the add/subtract result
        // as current flags, while every product condition accumulates into the
        // corresponding sticky flag.
        extraSticky |= productFlags & 0xFu;
    }
    return extraSticky;
}

void VU1Interpreter::updateFmacFlags(const uint8_t laneFlags[4], uint8_t dest,
                                     uint32_t extraSticky)
{
    if (dest == 0u)
        return;

    uint32_t mac = 0u;
    uint32_t status = 0u;
    for (uint32_t component = 0; component < 4u; ++component)
    {
        const uint8_t lane = laneForComponent(component);
        if ((dest & lane) == 0u)
            continue;

        const uint32_t flags = laneFlags[component];
        if ((flags & 0x1u) != 0u)
            mac |= lane;
        if ((flags & 0x2u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 4;
        if ((flags & 0x4u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 8;
        if ((flags & 0x8u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 12;
        status |= flags;
    }

    FlagPipelineEntry *entry = nullptr;
    for (FlagPipelineEntry &candidate : m_flagPipeline)
    {
        if (!candidate.valid)
        {
            entry = &candidate;
            break;
        }
    }
    if (!entry)
    {
        reportReservedInstruction(true, 0xFFFFFFFFu);
        return;
    }

    *entry = {};
    entry->valid = true;
    entry->issueCycle = m_cycle;
    entry->readyCycle = m_cycle + kFmacLatency;
    m_nextCommitCycle = std::min<uint64_t>(m_nextCommitCycle, entry->readyCycle);
    entry->mac = mac;
    entry->status = status;
    entry->extraSticky = extraSticky;
    entry->writesMac = true;
    entry->writesStatus = true;
}

void VU1Interpreter::normalizeResultsOnly(float *result, uint8_t dest)
{
    // Same values the exact path produces (overflow clamps to +-max under
    // round-toward-zero, underflow flushes to signed zero), without flags.
    uint32_t ignored = 0u;
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((dest & laneForComponent(component)) != 0u)
            result[component] = normalizeResult(result[component], ignored);
    }
}

void VU1Interpreter::cheapFmacFlags(float *result, uint8_t dest, uint8_t laneFlags[4])
{
    // Z/S/U/O from the rounded-toward-zero result. Overflow under RTZ lands on
    // +-FLT_MAX instead of infinity, so that magnitude is reported as O.
    for (uint32_t component = 0; component < 4u; ++component)
    {
        laneFlags[component] = 0u;
        if ((dest & laneForComponent(component)) == 0u)
            continue;
        uint32_t flags = 0u;
        result[component] = normalizeResult(result[component], flags);
        uint32_t bits = 0u;
        std::memcpy(&bits, &result[component], sizeof(bits));
        if ((bits & 0x7FFFFFFFu) == 0x7F7FFFFFu)
            flags |= 0x8u;
        laneFlags[component] = static_cast<uint8_t>(flags);
    }
}

void VU1Interpreter::refreshFlagLiveness(const uint8_t *vuCode, uint32_t codeSize)
{
    if (m_unit != Unit::VU1 || !m_activeMemory)
    {
        m_macStatusFlagsLive = true;
        return;
    }
    const uint64_t generation = m_activeMemory->getVU1CodeGeneration();
    if (m_flagScanValid && generation == m_flagScanGeneration && vuCode == m_flagScanCode)
        return;
    static const bool forceFlags = std::getenv("PS2X_VU1_EXACT") != nullptr;
    bool live = forceFlags;
    bool stickyLive = forceFlags;
    for (uint32_t addr = 0; addr + 8u <= codeSize; addr += 8u)
    {
        uint32_t lower = 0u;
        uint32_t upper = 0u;
        std::memcpy(&lower, vuCode + addr, sizeof(lower));
        std::memcpy(&upper, vuCode + addr + 4u, sizeof(upper));
        if ((upper & 0x80000000u) != 0u)
            continue; // I bit: the lower word is an immediate
        const uint8_t opHi = static_cast<uint8_t>((lower >> 25) & 0x7Fu);
        const uint16_t imm12 = static_cast<uint16_t>((((lower >> 21) & 0x1u) << 11) | (lower & 0x7FFu));
        // FSEQ/FSAND/FSOR read status; FMEQ/FMAND/FMOR read MAC.
        if (opHi == 0x18u || opHi == 0x1Au || opHi == 0x1Bu)
            live = true;
        if (opHi == 0x14u || opHi == 0x16u || opHi == 0x17u)
        {
            live = true;
            // Sticky status bits (6..11) are only observable through these.
            if (opHi == 0x14u || (imm12 & 0xFC0u) != 0u)
                stickyLive = true;
        }
    }
    m_macStatusFlagsLive = live;
    m_stickyFlagsLive = stickyLive;
    m_flagScanGeneration = generation;
    m_flagScanCode = vuCode;
    m_flagScanValid = true;
}

void VU1Interpreter::applyFmacDest(float *dst, float *result, uint8_t dest)
{
    if (!m_macStatusFlagsLive)
    {
        normalizeResultsOnly(result, dest);
        applyDest(dst, result, dest);
        return;
    }
    if (!m_stickyFlagsLive)
    {
        uint8_t cheapFlags[4]{};
        cheapFmacFlags(result, dest, cheapFlags);
        updateFmacFlags(cheapFlags, dest, 0u);
        applyDest(dst, result, dest);
        return;
    }
    uint8_t laneFlags[4]{};
    normalizeFmacResult(result, dest, laneFlags);
    updateFmacFlags(laneFlags, dest, calculateFmacProductSticky(dest));
    applyDest(dst, result, dest);
}

void VU1Interpreter::applyFmacDestAcc(float *result, uint8_t dest)
{
    if (!m_macStatusFlagsLive)
    {
        normalizeResultsOnly(result, dest);
        applyDestAcc(result, dest);
        return;
    }
    if (!m_stickyFlagsLive)
    {
        uint8_t cheapFlags[4]{};
        cheapFmacFlags(result, dest, cheapFlags);
        updateFmacFlags(cheapFlags, dest, 0u);
        applyDestAcc(result, dest);
        return;
    }
    uint8_t laneFlags[4]{};
    normalizeFmacResult(result, dest, laneFlags);
    updateFmacFlags(laneFlags, dest, calculateFmacProductSticky(dest));
    applyDestAcc(result, dest);
}

void VU1Interpreter::queueFsset(uint16_t immediate)
{
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (entry.valid && entry.issueCycle == m_cycle)
            entry.writesStatus = false;
    }

    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (!entry.valid)
        {
            entry = {};
            entry.valid = true;
            entry.issueCycle = m_cycle;
            entry.readyCycle = m_cycle + kFmacLatency;
            m_nextCommitCycle = std::min<uint64_t>(m_nextCommitCycle, entry.readyCycle);
            entry.status = static_cast<uint32_t>(immediate) & 0xFC0u;
            entry.writesSticky = true;
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFFEu);
}

void VU1Interpreter::queueClip(uint32_t clip)
{
    m_workingClip = ((m_workingClip << 6) | (clip & 0x3Fu)) & 0xFFFFFFu;
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (!entry.valid)
        {
            entry = {};
            entry.valid = true;
            entry.issueCycle = m_cycle;
            entry.readyCycle = m_cycle + kFmacLatency;
            m_nextCommitCycle = std::min<uint64_t>(m_nextCommitCycle, entry.readyCycle);
            entry.clip = m_workingClip;
            entry.writesClip = true;
            return;
        }
    }
    reportReservedInstruction(true, 0xFFFFFFFDu);
}

void VU1Interpreter::queueFcset(uint32_t clip)
{
    m_workingClip = clip & 0xFFFFFFu;
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (entry.valid && entry.issueCycle == m_cycle)
            entry.writesClip = false;
    }
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (!entry.valid)
        {
            entry = {};
            entry.valid = true;
            entry.issueCycle = m_cycle;
            entry.readyCycle = m_cycle + kFmacLatency;
            m_nextCommitCycle = std::min<uint64_t>(m_nextCommitCycle, entry.readyCycle);
            entry.clip = m_workingClip;
            entry.writesClip = true;
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFFAu);
}

void VU1Interpreter::queueQ(float value, uint32_t latency, uint32_t statusDi)
{
    uint32_t ignoredFlags = 0u;
    value = normalizeResult(value, ignoredFlags);
    m_fdiv.valid = true;
    m_fdiv.readyCycle = m_cycle + latency;
    m_nextCommitCycle = std::min<uint64_t>(m_nextCommitCycle, m_fdiv.readyCycle);
    m_fdiv.value = value;
    m_fdiv.statusDi = statusDi & 0x30u;
}

void VU1Interpreter::queueP(float value, uint32_t latency)
{
    uint32_t ignoredFlags = 0u;
    value = normalizeResult(value, ignoredFlags);
    for (ScalarPipelineEntry &entry : m_efu)
    {
        if (!entry.valid)
        {
            entry.valid = true;
            entry.readyCycle = m_cycle + latency;
            m_nextCommitCycle = std::min<uint64_t>(m_nextCommitCycle, entry.readyCycle);
            entry.value = value;
            // EFU throughput is one cycle shorter than result visibility.
            m_efuResourceReady = m_cycle + (latency > 0u ? latency - 1u : 0u);
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFF9u);
}

void VU1Interpreter::queueStore(uint32_t address, const uint32_t words[4], uint8_t laneMask)
{
    // A store lands at the next cycle boundary, before anything else can read
    // VU memory, except PATH1 which reads lazily: catch it up first and write
    // now. PS2X_VU1_EXACT keeps the queued model.
    static const bool exactStores = std::getenv("PS2X_VU1_EXACT") != nullptr;
    if (!exactStores)
    {
        catchUpXgkick();
        if (m_activeVuData && address + 16u <= m_activeVuDataSize)
        {
            uint32_t *dst = reinterpret_cast<uint32_t *>(m_activeVuData + address);
            for (uint32_t component = 0; component < 4u; ++component)
                if ((laneMask & laneForComponent(component)) != 0u)
                    std::memcpy(dst + component, &words[component], 4);
        }
        return;
    }
    for (PendingStore &store : m_storePipeline)
    {
        if (!store.valid)
        {
            store.valid = true;
            store.readyCycle = m_cycle + 1u;
            m_nextCommitCycle = std::min<uint64_t>(m_nextCommitCycle, store.readyCycle);
            store.address = address;
            store.laneMask = laneMask;
            std::copy(words, words + 4, store.words.begin());
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFFCu);
}

void VU1Interpreter::queueVfWrite(uint8_t reg, uint8_t laneMask,
                                  const float value[4], uint32_t latency)
{
    if (reg == 0u || laneMask == 0u)
        return;
    for (PendingVfWrite &write : m_vfWritePipeline)
    {
        if (!write.valid)
        {
            write = {};
            write.valid = true;
            write.readyCycle = m_cycle + latency;
            m_nextCommitCycle = std::min<uint64_t>(m_nextCommitCycle, write.readyCycle);
            write.sequence = ++m_nextWriteSequence;
            write.reg = reg;
            write.laneMask = laneMask;
            std::copy(value, value + 4, write.value.begin());
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((laneMask & laneForComponent(component)) != 0u)
                    m_vfLatestWrite[reg][component] = write.sequence;
            }
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFF7u);
}

void VU1Interpreter::queueViWrite(uint8_t reg, int32_t value, uint32_t latency)
{
    if (reg == 0u)
        return;
    for (PendingViWrite &write : m_viWritePipeline)
    {
        if (!write.valid)
        {
            write = {};
            write.valid = true;
            write.readyCycle = m_cycle + latency;
            m_nextCommitCycle = std::min<uint64_t>(m_nextCommitCycle, write.readyCycle);
            write.sequence = ++m_nextWriteSequence;
            write.reg = reg;
            write.value = value;
            m_viLatestWrite[reg] = write.sequence;
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFF6u);
}

void VU1Interpreter::queueAccWrite(uint8_t laneMask, const float value[4], uint32_t latency)
{
    if (laneMask == 0u)
        return;
    for (PendingAccWrite &write : m_accWritePipeline)
    {
        if (!write.valid)
        {
            write = {};
            write.valid = true;
            write.readyCycle = m_cycle + latency;
            m_nextCommitCycle = std::min<uint64_t>(m_nextCommitCycle, write.readyCycle);
            write.sequence = ++m_nextWriteSequence;
            write.laneMask = laneMask;
            std::copy(value, value + 4, write.value.begin());
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((laneMask & laneForComponent(component)) != 0u)
                    m_accLatestWrite[component] = write.sequence;
            }
            return;
        }
    }
    reportReservedInstruction(true, 0xFFFFFFF5u);
}

void VU1Interpreter::commitReadyPipelinesSlow()
{
    uint64_t next = UINT64_MAX;
    // VF/VI/ACC write queues are only used by the exact delayed-write model.
    static const bool exactWrites = std::getenv("PS2X_VU1_EXACT") != nullptr;
    auto pending = [this, &next](bool valid, uint64_t ready)
    {
        if (!valid)
            return true;
        if (ready > m_cycle)
        {
            next = std::min(next, ready);
            return true;
        }
        return false;
    };
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (pending(entry.valid, entry.readyCycle))
            continue;

        if (entry.writesMac)
            m_state.mac = entry.mac;
        if (entry.writesStatus)
        {
            const uint32_t current = entry.status & 0xFu;
            m_state.status = (m_state.status & 0xFF0u) | current | ((current | entry.extraSticky) << 6);
        }
        if (entry.writesSticky)
        {
            m_state.status = (m_state.status & 0x03Fu) | (entry.status & 0xFC0u);
        }
        if (entry.writesClip)
            m_state.clip = entry.clip;
        entry = {};
    }

    if (!pending(m_fdiv.valid, m_fdiv.readyCycle))
    {
        m_state.q = m_fdiv.value;
        const uint32_t currentDi = m_fdiv.statusDi & 0x30u;
        m_state.status = (m_state.status & 0xFCFu) | currentDi | (currentDi << 6);
        m_fdiv = {};
    }

    for (ScalarPipelineEntry &entry : m_efu)
    {
        if (!pending(entry.valid, entry.readyCycle))
        {
            m_state.p = entry.value;
            entry = {};
        }
    }

    for (PendingStore &store : m_storePipeline)
    {
        if (pending(store.valid, store.readyCycle))
            continue;
        // PATH1 must read VU memory as it was before this store lands.
        catchUpXgkick();
        if (m_activeVuData && store.address + 16u <= m_activeVuDataSize)
        {
            uint32_t oldWords[4]{};
            std::memcpy(oldWords, m_activeVuData + store.address, sizeof(oldWords));
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((store.laneMask & laneForComponent(component)) != 0u)
                    oldWords[component] = store.words[component];
            }
            std::memcpy(m_activeVuData + store.address, oldWords, sizeof(oldWords));
        }
        store = {};
    }

    if (exactWrites)
    for (PendingVfWrite &write : m_vfWritePipeline)
    {
        if (pending(write.valid, write.readyCycle))
            continue;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((write.laneMask & laneForComponent(component)) != 0u &&
                m_vfLatestWrite[write.reg][component] == write.sequence)
            {
                m_state.vf[write.reg][component] = write.value[component];
            }
        }
        write = {};
    }

    if (exactWrites)
    for (PendingViWrite &write : m_viWritePipeline)
    {
        if (pending(write.valid, write.readyCycle))
            continue;
        if (m_viLatestWrite[write.reg] == write.sequence)
            m_state.vi[write.reg] = static_cast<int16_t>(write.value);
        write = {};
    }

    if (exactWrites)
    for (PendingAccWrite &write : m_accWritePipeline)
    {
        if (pending(write.valid, write.readyCycle))
            continue;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((write.laneMask & laneForComponent(component)) != 0u &&
                m_accLatestWrite[component] == write.sequence)
            {
                m_state.acc[component] = write.value[component];
            }
        }
        write = {};
    }
    m_nextCommitCycle = next;
}

void VU1Interpreter::recomputeNextCommitCycle()
{
    uint64_t next = UINT64_MAX;
    auto consider = [&next](bool valid, uint64_t ready)
    {
        if (valid && ready < next)
            next = ready;
    };
    for (const auto &e : m_flagPipeline)
        consider(e.valid, e.readyCycle);
    consider(m_fdiv.valid, m_fdiv.readyCycle);
    for (const auto &e : m_efu)
        consider(e.valid, e.readyCycle);
    for (const auto &e : m_storePipeline)
        consider(e.valid, e.readyCycle);
    for (const auto &e : m_vfWritePipeline)
        consider(e.valid, e.readyCycle);
    for (const auto &e : m_viWritePipeline)
        consider(e.valid, e.readyCycle);
    for (const auto &e : m_accWritePipeline)
        consider(e.valid, e.readyCycle);
    m_nextCommitCycle = next;
}

void VU1Interpreter::catchUpXgkickSlow()
{
    const uint64_t elapsed = m_xgkickDue - m_xgkick.progressedCycle;
    m_xgkick.progressedCycle = m_xgkickDue;
    if (!m_xgkick.active || !m_activeVuData || m_activeVuDataSize == 0u)
        return;

    m_xgkick.cycleCredit += static_cast<uint32_t>(std::min<uint64_t>(elapsed, 0x40000u));
    while (m_xgkick.active && m_xgkick.cycleCredit >= 2u)
    {
        m_xgkick.cycleCredit -= 2u;
        if (m_xgkick.copiedBytes > XgkickPipeline::kBufferSize - 16u)
        {
            reportReservedInstruction(false, 0xFFFFFFFBu);
            m_xgkick.active = false;
            return;
        }

        const uint32_t qwordOffset = m_xgkick.copiedBytes;
        const uint32_t source = (m_xgkick.sourceAddress + m_xgkick.copiedBytes) % m_activeVuDataSize;
        if (source + 16u <= m_activeVuDataSize)
        {
            std::memcpy(m_xgkick.packet.data() + m_xgkick.copiedBytes, m_activeVuData + source, 16u);
        }
        else
        {
            for (uint32_t i = 0; i < 16u; ++i)
                m_xgkick.packet[m_xgkick.copiedBytes + i] = m_activeVuData[(source + i) % m_activeVuDataSize];
        }
        m_xgkick.copiedBytes += 16u;

        if (m_xgkick.currentTagEnd == 0u)
        {
            uint64_t tagLo = 0;
            std::memcpy(&tagLo, m_xgkick.packet.data() + qwordOffset, sizeof(tagLo));
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint32_t format = static_cast<uint32_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;

            uint64_t tagBytes = 16u;
            if (format == 0u)
                tagBytes += static_cast<uint64_t>(nloop) * nreg * 16u;
            else if (format == 1u)
                tagBytes += ((static_cast<uint64_t>(nloop) * nreg + 1u) & ~1ull) * 8u;
            else if (format == 2u)
                tagBytes += static_cast<uint64_t>(nloop) * 16u;
            else
            {
                reportReservedInstruction(false, 0xFFFFFFF8u);
                m_xgkick.active = false;
                return;
            }

            if (tagBytes > XgkickPipeline::kBufferSize - qwordOffset)
            {
                reportReservedInstruction(false, 0xFFFFFFFBu);
                m_xgkick.active = false;
                return;
            }
            m_xgkick.currentTagEnd = qwordOffset + static_cast<uint32_t>(tagBytes);
            m_xgkick.currentTagEop = ((tagLo >> 15) & 1u) != 0u;
            if (m_xgkick.currentTagEop)
                m_xgkick.totalBytes = m_xgkick.currentTagEnd;
        }

        if (m_xgkick.copiedBytes >= m_xgkick.currentTagEnd)
        {
            if (m_xgkick.currentTagEop)
                finishXgkick();
            else
            {
                // The next transferred qword is another GIFtag.
                m_xgkick.currentTagEnd = 0u;
                m_xgkick.currentTagEop = false;
            }
        }
    }
}

void VU1Interpreter::traceZeroXyzPacket()
{
    const uint8_t *p = m_xgkick.packet.data();
    uint32_t off = 0;
    uint32_t zeros = 0, total = 0;
    uint32_t firstZeroQword = 0;
    while (off + 16u <= m_xgkick.totalBytes)
    {
        uint64_t tag = 0, regs = 0;
        std::memcpy(&tag, p + off, 8);
        std::memcpy(&regs, p + off + 8, 8);
        off += 16u;
        const uint32_t nloop = tag & 0x7FFFu;
        const uint32_t flg = (tag >> 58) & 3u;
        uint32_t nreg = (tag >> 60) & 0xFu;
        if (nreg == 0u)
            nreg = 16u;
        if (flg != 0u)
        {
            off += flg == 1u ? ((nloop * nreg + 1u) & ~1u) * 8u : nloop * 16u;
            continue;
        }
        for (uint32_t l = 0; l < nloop; ++l)
            for (uint32_t r = 0; r < nreg && off + 16u <= m_xgkick.totalBytes; ++r, off += 16u)
            {
                const uint32_t reg = (regs >> (r * 4u)) & 0xFu;
                if (reg != 4u && reg != 5u)
                    continue;
                uint64_t lo = 0, hi = 0;
                std::memcpy(&lo, p + off, 8);
                std::memcpy(&hi, p + off + 8, 8);
                const bool adc = ((hi >> 47) & 1u) != 0u;
                ++total;
                if ((lo & 0xFFFFu) == 0u && ((lo >> 32) & 0xFFFFu) == 0u && !adc)
                {
                    if (!zeros)
                        firstZeroQword = (m_xgkick.sourceAddress + off) / 16u;
                    ++zeros;
                }
            }
    }
    if (!zeros)
        return;
    static int dumpedPackets = 0;
    if (zeros < total && dumpedPackets < 4)
    {
        if (dumpedPackets == 0 && !m_traceRunData.empty())
        {
            if (FILE *f = std::fopen("logs/vu1_badrun.bin", "wb"))
            {
                std::fwrite(m_traceRunData.data(), 1, m_traceRunData.size(), f);
                std::fwrite(&m_traceRunState, 1, sizeof(m_traceRunState), f);
                std::fclose(f);
            }
            if (m_activeMemory)
                if (FILE *f = std::fopen("logs/ee_badrun.bin", "wb"))
                {
                    std::fwrite(m_activeMemory->getRDRAM(), 1, 32u * 1024u * 1024u, f);
                    std::fclose(f);
                }
        }
        ++dumpedPackets;
        std::fprintf(stderr, "[vu1:zeropkt] pc=0x%04x src=0x%x bytes=%u\n", m_state.pc, m_xgkick.sourceAddress / 16u, m_xgkick.totalBytes);
        for (uint32_t q = 0; q * 16u < m_xgkick.totalBytes && q < 80u; ++q)
        {
            uint32_t w[4];
            std::memcpy(w, p + q * 16u, 16);
            float f[4];
            std::memcpy(f, w, 16);
            std::fprintf(stderr, "  %3u: %08x %08x %08x %08x  (%g %g %g %g)\n", q, w[0], w[1], w[2], w[3], f[0], f[1], f[2], f[3]);
        }
    }
    static std::set<std::pair<uint64_t, uint32_t>> seen;
    static uint64_t events = 0;
    ++events;
    uint64_t hash = 1469598103934665603ull;
    if (m_activeMemory)
    {
        const uint8_t *code = m_activeMemory->getVU1Code();
        for (uint32_t i = 0; i < 0x4000u; ++i)
            hash = (hash ^ code[i]) * 1099511628211ull;
    }
    static int mixed = 0;
    if (seen.insert({hash, m_recTraceStartPc}).second || (events % 500u) == 0u || (zeros < total && mixed++ < 40))
        std::fprintf(stderr, "[vu1:zeroxyz] image=%016llx start=0x%04x pc=0x%04x zero=%u/%u qword=0x%x events=%llu\n",
                     static_cast<unsigned long long>(hash), m_recTraceStartPc, m_state.pc, zeros, total, firstZeroQword,
                     static_cast<unsigned long long>(events));
}

namespace
{
    // PS2X_VU1_PROFILE=1: what each VU1 routine (microcode image + entry pc)
    // sends to the GS, to plan native replacements.
    struct Vu1RoutineStats
    {
        uint64_t calls = 0, kicks = 0, bytes = 0, vertices = 0, instructions = 0;
        uint64_t prims[8]{};
        uint64_t textured = 0, blended = 0, fogged = 0, gouraud = 0;
        std::map<uint32_t, uint64_t> texPsm; // TEX0 PSM -> count
        std::map<uint32_t, uint64_t> regs;   // PACKED register ids seen
    };
    std::mutex g_vu1ProfileMutex;
    std::map<std::pair<uint64_t, uint32_t>, Vu1RoutineStats> g_vu1Profile;
    thread_local std::pair<uint64_t, uint32_t> t_vu1Routine{0, 0};
    thread_local bool t_vu1ProgramEnded = true;
    thread_local bool t_vu1Fresh = true;      // the next run() starts a program
    thread_local uint32_t t_vu1StopPc = 0;    // native verify: interpret up to here
    thread_local bool t_vu1InNative = false;
    thread_local VU1Interpreter *t_vu1KickTarget = nullptr; // native code's XGKICKs go here

    struct NativeLoop
    {
        uint64_t hash;
        uint32_t pc;
        vu1native::LoopFn fn;
        bool generated = false;
    };
    std::vector<NativeLoop> &nativeLoops()
    {
        static std::vector<NativeLoop> loops;
        return loops;
    }
    // 0: off, 1: on, 2: verify against the interpreter.
    int nativeMode()
    {
        static const int mode = []
        {
            const char *env = std::getenv("PS2X_VU1_NATIVE");
            if (!env)
                return 1;
            if (std::strcmp(env, "verify") == 0)
                return 2;
            return std::atoi(env) != 0 ? 1 : 0;
        }();
        return mode;
    }
    struct NativeVerifyStats
    {
        std::mutex mutex;
        uint64_t runs = 0, mismatches = 0;
        int reported = 0;
    } g_nativeVerify;
}
static void verifyNativeLoop(VU1Interpreter &I, vu1native::LoopFn fn, uint8_t *vuCode, uint32_t codeSize, uint8_t *vuData, uint32_t dataSize,
                             GS &gs, PS2Memory *memory, uint32_t maxCycles);
namespace
{

    // PS2X_VU1_SKIP=<image hash>@<entry pc>[,...]: drop what those routines
    // send to the GS (shows what they draw).
    const std::vector<std::pair<uint64_t, uint32_t>> &vu1SkipList()
    {
        static const std::vector<std::pair<uint64_t, uint32_t>> list = []
        {
            std::vector<std::pair<uint64_t, uint32_t>> out;
            const char *env = std::getenv("PS2X_VU1_SKIP");
            std::string spec = env ? env : "";
            size_t pos = 0;
            while (pos < spec.size())
            {
                const size_t comma = spec.find(',', pos);
                const std::string item = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                const size_t at = item.find('@');
                if (at != std::string::npos)
                    out.emplace_back(std::strtoull(item.substr(0, at).c_str(), nullptr, 16),
                                     static_cast<uint32_t>(std::strtoul(item.substr(at + 1).c_str(), nullptr, 16)));
                if (comma == std::string::npos)
                    break;
                pos = comma + 1;
            }
            return out;
        }();
        return list;
    }

    bool vu1ProfileEnabled()
    {
        static const bool enabled = std::getenv("PS2X_VU1_PROFILE") != nullptr || !vu1SkipList().empty();
        return enabled;
    }

    bool vu1SkipCurrent()
    {
        for (const auto &[hash, pc] : vu1SkipList())
            if ((hash == t_vu1Routine.first || (t_vu1Routine.first & 0xFFFFFFFF00000000ull) == (hash << 32)) && pc == t_vu1Routine.second)
                return true;
        return false;
    }

    void vu1ProfilePrimitive(Vu1RoutineStats &st, uint64_t prim)
    {
        st.prims[prim & 7u]++;
        if (prim & (1u << 3)) st.gouraud++;
        if (prim & (1u << 4)) st.textured++;
        if (prim & (1u << 5)) st.fogged++;
        if (prim & (1u << 6)) st.blended++;
    }

    void vu1ProfileKick(const uint8_t *data, uint32_t size)
    {
        std::lock_guard<std::mutex> lock(g_vu1ProfileMutex);
        Vu1RoutineStats &st = g_vu1Profile[t_vu1Routine];
        st.kicks++;
        st.bytes += size;
        uint32_t off = 0;
        while (off + 16u <= size)
        {
            uint64_t lo, hi;
            std::memcpy(&lo, data + off, 8);
            std::memcpy(&hi, data + off + 8, 8);
            off += 16;
            const uint32_t nloop = lo & 0x7FFFu;
            const bool eop = (lo >> 15) & 1u;
            const bool pre = (lo >> 46) & 1u;
            const uint64_t prim = (lo >> 47) & 0x7FFu;
            const uint32_t flg = (lo >> 58) & 3u;
            uint32_t nreg = (lo >> 60) & 0xFu;
            if (nreg == 0) nreg = 16;
            if (pre)
                vu1ProfilePrimitive(st, prim);
            if (flg == 0) // PACKED
            {
                for (uint32_t l = 0; l < nloop; ++l)
                    for (uint32_t r = 0; r < nreg && off + 16u <= size; ++r, off += 16)
                    {
                        const uint32_t reg = (hi >> (r * 4)) & 0xFu;
                        st.regs[reg]++;
                        if (reg == 0x4 || reg == 0x5)
                            st.vertices++;
                        else if (reg == 0xE)
                        {
                            uint64_t value, addr;
                            std::memcpy(&value, data + off, 8);
                            std::memcpy(&addr, data + off + 8, 8);
                            const uint32_t a = addr & 0xFFu;
                            if (a == 0x06 || a == 0x07)
                                st.texPsm[(value >> 20) & 0x3Fu]++;
                            else if (a == 0x00)
                                vu1ProfilePrimitive(st, value);
                        }
                    }
            }
            else if (flg == 1) // REGLIST
            {
                const uint64_t words = static_cast<uint64_t>(nloop) * nreg;
                off += static_cast<uint32_t>(((words + 1u) / 2u) * 16u);
            }
            else
                off += nloop * 16u;
            if (eop)
                break;
        }
    }

    void vu1ProfileReport()
    {
        static double last = 0;
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now - last < 10.0)
            return;
        last = now;
        std::lock_guard<std::mutex> lock(g_vu1ProfileMutex);
        std::vector<std::pair<std::pair<uint64_t, uint32_t>, Vu1RoutineStats *>> rows;
        for (auto &[key, st] : g_vu1Profile)
            rows.emplace_back(key, &st);
        std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b)
                  { return a.second->instructions > b.second->instructions; });
        std::fprintf(stderr, "[vu1prof] ---- routines by VU instructions (cumulative)\n");
        for (const auto &[key, st] : rows)
        {
            std::string psm, regs;
            for (const auto &[p, n] : st->texPsm)
                psm += std::to_string(p) + ":" + std::to_string(n) + " ";
            for (const auto &[r, n] : st->regs)
                regs += std::to_string(r) + " ";
            std::fprintf(stderr,
                         "[vu1prof] %016llx@%04x calls=%llu instr=%llu kicks=%llu KB=%llu verts=%llu prims(pt,ln,ls,tri,ts,tf,spr)=%llu,%llu,%llu,%llu,%llu,%llu,%llu tex=%llu abe=%llu fge=%llu iip=%llu psm={%s} regs={%s}\n",
                         (unsigned long long)key.first, key.second, (unsigned long long)st->calls, (unsigned long long)st->instructions,
                         (unsigned long long)st->kicks, (unsigned long long)(st->bytes / 1024), (unsigned long long)st->vertices,
                         (unsigned long long)st->prims[0], (unsigned long long)st->prims[1], (unsigned long long)st->prims[2],
                         (unsigned long long)st->prims[3], (unsigned long long)st->prims[4], (unsigned long long)st->prims[5],
                         (unsigned long long)st->prims[6], (unsigned long long)st->textured, (unsigned long long)st->blended,
                         (unsigned long long)st->fogged, (unsigned long long)st->gouraud, psm.c_str(), regs.c_str());
        }
    }
}

void VU1Interpreter::finishXgkick()
{
    if (!m_xgkick.active)
        return;

    // PS2X_TRACE_ZERO_XYZ=1: report programs that kick vertices at XY=(0,0).
    static const bool traceZeroXyz = std::getenv("PS2X_TRACE_ZERO_XYZ") != nullptr;
    if (traceZeroXyz)
        traceZeroXyzPacket();
    if (m_gifCapture)
    {
        const uint8_t *packet = m_xgkick.packet.data();
        m_gifCapture->insert(m_gifCapture->end(), packet, packet + m_xgkick.totalBytes);
        if (m_gifCaptureOnly)
        {
            m_xgkick.active = false;
            return;
        }
    }
    if (vu1ProfileEnabled())
    {
        vu1ProfileKick(m_xgkick.packet.data(), m_xgkick.totalBytes);
        if (vu1SkipCurrent())
        {
            m_xgkick.active = false;
            return;
        }
    }
    if (m_activeMemory)
        m_activeMemory->submitGifPacket(GifPathId::Path1, m_xgkick.packet.data(), m_xgkick.totalBytes);
    else if (m_activeGs)
        m_activeGs->processGIFPacket(m_xgkick.packet.data(), m_xgkick.totalBytes);
    m_xgkick.active = false;
}

void VU1Interpreter::startXgkick(uint32_t qwordAddress)
{
    if (m_unit != Unit::VU1 || !m_activeVuData || m_activeVuDataSize < 16u)
        return;

    const uint32_t sourceAddress = (qwordAddress * 16u) % m_activeVuDataSize;
    resetXgkickState();
    m_xgkick.active = true;
    m_xgkick.sourceAddress = sourceAddress;
    m_xgkick.cycleCredit = 1u; // XGKICK's issue cycle counts toward PATH1.
    m_xgkick.issueCycle = m_cycle;
    m_xgkick.progressedCycle = m_cycle;
    m_xgkickDue = m_cycle;
}

void VU1Interpreter::advanceOneCycle()
{
    ++m_cycle;
    m_state.cycles = m_cycle;
    // LSU commits become visible at the cycle boundary before PATH1 consumes
    // its next qword from VU memory.
    commitReadyPipelines();
    progressXgkick();
}

void VU1Interpreter::advanceTo(uint64_t targetCycle)
{
    while (m_cycle < targetCycle)
        advanceOneCycle();
}

bool VU1Interpreter::pipelinesPending() const
{
    if (m_fdiv.valid || const_cast<VU1Interpreter *>(this)->xgkickActive())
        return true;
    for (const ScalarPipelineEntry &entry : m_efu)
        if (entry.valid)
            return true;
    for (const FlagPipelineEntry &entry : m_flagPipeline)
        if (entry.valid)
            return true;
    for (const PendingStore &store : m_storePipeline)
        if (store.valid)
            return true;
    for (const PendingVfWrite &write : m_vfWritePipeline)
        if (write.valid)
            return true;
    for (const PendingViWrite &write : m_viWritePipeline)
        if (write.valid)
            return true;
    for (const PendingAccWrite &write : m_accWritePipeline)
        if (write.valid)
            return true;
    return false;
}

void VU1Interpreter::flushPipelines()
{
    while (pipelinesPending())
        advanceOneCycle();
}

uint64_t VU1Interpreter::calculatePairReadyCycle(const DecodedInstructionPair &decoded) const
{
    uint64_t ready = m_cycle;
    const InstructionUsage *usages[2] = {
        &decoded.upperUsage,
        &decoded.lowerUsage};
    for (const InstructionUsage *usage : usages)
    {
        if (!usage)
            continue;
        for (uint32_t index = 0; index < usage->vfReadCount; ++index)
        {
            const VfAccess &access = usage->vfRead[index];
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((access.lanes & laneForComponent(component)) != 0u)
                    ready = std::max(ready, m_vfReady[access.reg][component]);
            }
        }
        for (uint32_t bits = usage->viRead & 0xFFFEu; bits != 0u; bits &= bits - 1u)
            ready = std::max(ready, m_viReady[__builtin_ctz(bits)]);
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((usage->accRead & laneForComponent(component)) != 0u)
                ready = std::max(ready, m_accReady[component]);
        }
    }

    if (decoded.lowerUsage.pipeline == PipelineFdiv && m_fdiv.valid)
        ready = std::max(ready, m_fdiv.readyCycle);
    if (decoded.lowerUsage.pipeline == PipelineEfu)
        ready = std::max(ready, m_efuResourceReady);
    if (decoded.lowerUsage.waitQ && m_fdiv.valid)
        ready = std::max(ready, m_fdiv.readyCycle);
    if (decoded.lowerUsage.waitP)
    {
        for (const ScalarPipelineEntry &entry : m_efu)
            if (entry.valid)
                ready = std::max(ready, entry.readyCycle);
    }
    if (decoded.lowerUsage.pipeline == PipelineXgkick && const_cast<VU1Interpreter *>(this)->xgkickActive())
        ready = std::max(ready, m_cycle + 1u);
    return ready;
}

void VU1Interpreter::markPairWrites(const DecodedInstructionPair &decoded)
{
    const VfAccess lowerWrite = decoded.lowerUsage.vfWrite;
    if (lowerWrite.reg != 0u &&
        decoded.suppressedLowerVf != lowerWrite.reg)
    {
        const uint32_t latency = decoded.lowerUsage.vfLatency != 0u
                                     ? decoded.lowerUsage.vfLatency
                                     : decoded.lowerUsage.latency;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((lowerWrite.lanes & laneForComponent(component)) != 0u)
                m_vfReady[lowerWrite.reg][component] = m_cycle + latency;
        }
    }

    const VfAccess upperWrite = decoded.upperUsage.vfWrite;
    if (upperWrite.reg != 0u)
    {
        const uint32_t latency = decoded.upperUsage.vfLatency != 0u
                                     ? decoded.upperUsage.vfLatency
                                     : decoded.upperUsage.latency;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((upperWrite.lanes & laneForComponent(component)) != 0u)
                m_vfReady[upperWrite.reg][component] = m_cycle + latency;
        }
    }

    for (uint32_t bits = decoded.lowerUsage.viWrite & 0xFFFEu; bits != 0u; bits &= bits - 1u)
        m_viReady[__builtin_ctz(bits)] = m_cycle + (decoded.lowerUsage.viLatency != 0u ? decoded.lowerUsage.viLatency : decoded.lowerUsage.latency);
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((decoded.upperUsage.accWrite & laneForComponent(component)) != 0u)
            m_accReady[component] = m_cycle + kAccForwardLatency;
    }
}

VU1Interpreter::InstructionUsage VU1Interpreter::decodeUpperUsage(uint32_t upper) const
{
    InstructionUsage usage;
    usage.pipeline = PipelineFmac;
    usage.latency = kFmacLatency;

    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t dest = DEST(upper);
    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);
    const uint8_t fd = FD(upper);

    if (op <= 0x2Fu)
    {
        addVfRead(usage, fs, dest);
        addVfWrite(usage, fd, dest);
        if (op <= 0x1Bu)
            addVfRead(usage, ft, laneForComponent(op & 3u));
        else if (op >= 0x28u)
            addVfRead(usage, ft, op == 0x2Eu ? 0xEu : dest);
        if (op == 0x08u || op == 0x09u || op == 0x0Au || op == 0x0Bu ||
            op == 0x0Cu || op == 0x0Du || op == 0x0Eu || op == 0x0Fu ||
            op == 0x21u || op == 0x23u || op == 0x25u || op == 0x27u ||
            op == 0x29u || op == 0x2Du || op == 0x2Eu)
        {
            usage.accRead = dest;
        }
        return usage;
    }

    if (op >= 0x3Cu)
    {
        const uint8_t special = static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu));
        const bool writesAcc =
            special <= 0x0Fu ||
            (special >= 0x18u && special <= 0x1Cu) ||
            special == 0x1Eu ||
            (special >= 0x20u && special <= 0x2Au) ||
            (special >= 0x2Cu && special <= 0x2Eu);
        if (writesAcc)
        {
            addVfRead(usage, fs, dest);
            if (special <= 0x1Bu)
                addVfRead(usage, ft, laneForComponent(special & 3u));
            else if ((special >= 0x28u && special <= 0x2Eu))
                addVfRead(usage, ft, special == 0x2Eu ? 0xEu : dest);
            usage.accWrite = dest;
            if ((special >= 0x08u && special <= 0x0Fu) ||
                special == 0x21u || special == 0x23u || special == 0x25u ||
                special == 0x27u || special == 0x29u || special == 0x2Du)
            {
                usage.accRead = dest;
            }
        }
        else if (special >= 0x10u && special <= 0x17u)
        {
            addVfRead(usage, fs, dest);
            addVfWrite(usage, ft, dest);
        }
        else if (special == 0x1Du)
        {
            addVfRead(usage, fs, dest);
            addVfWrite(usage, ft, dest);
        }
        else if (special == 0x1Fu)
        {
            addVfRead(usage, fs, 0xEu);
            addVfRead(usage, ft, 0x1u);
            usage.writesClip = true;
        }
        else if (special != 0x2Fu && special != 0x30u)
        {
            usage.reserved = true;
        }
        return usage;
    }

    usage.reserved = true;
    return usage;
}

VU1Interpreter::InstructionUsage VU1Interpreter::decodeLowerUsage(uint32_t lower) const
{
    InstructionUsage usage;
    if (lower == 0u || lower == 0x8000033Cu)
        return usage;

    const uint8_t opHi = static_cast<uint8_t>((lower >> 25) & 0x7Fu);
    const uint8_t vfT = FT(lower);
    const uint8_t vfS = FS(lower);
    const uint8_t viT = VIT(lower);
    const uint8_t viS = VIS(lower);
    const uint8_t viD = VID(lower);
    const uint8_t dest = DEST(lower);
    auto readVi = [&](uint8_t reg)
    {
        if (reg != 0u)
            usage.viRead |= static_cast<uint16_t>(1u << reg);
    };
    auto writeVi = [&](uint8_t reg)
    {
        if (reg != 0u)
            usage.viWrite |= static_cast<uint16_t>(1u << reg);
    };

    switch (opHi)
    {
    case 0x00:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        addVfWrite(usage, vfT, dest);
        return usage;
    case 0x01:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viT);
        addVfRead(usage, vfS, dest);
        return usage;
    case 0x04:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x05:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viS);
        readVi(viT);
        return usage;
    case 0x08:
    case 0x09:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x10:
    case 0x12:
    case 0x13:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.readsClip = true;
        writeVi(1u);
        return usage;
    case 0x11:
        usage.pipeline = PipelineFmac;
        usage.latency = kFmacLatency;
        usage.writesClip = true;
        return usage;
    case 0x14:
    case 0x16:
    case 0x17:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        writeVi(viT);
        return usage;
    case 0x15:
        usage.pipeline = PipelineFmac;
        usage.latency = kFmacLatency;
        return usage;
    case 0x18:
    case 0x1A:
    case 0x1B:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x1C:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.readsClip = true;
        writeVi(viT);
        return usage;
    case 0x20:
        usage.pipeline = PipelineBranch;
        return usage;
    case 0x21:
        usage.pipeline = PipelineBranch;
        usage.latency = 1u;
        writeVi(viT);
        return usage;
    case 0x24:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        return usage;
    case 0x25:
        usage.pipeline = PipelineBranch;
        usage.latency = 1u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x28:
    case 0x29:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        readVi(viT);
        return usage;
    case 0x2C:
    case 0x2D:
    case 0x2E:
    case 0x2F:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        return usage;
    case 0x40:
        break;
    default:
        usage.reserved = true;
        return usage;
    }

    const uint8_t direct = static_cast<uint8_t>(lower & 0x3Fu);
    if (direct == 0x30u || direct == 0x31u || direct == 0x34u || direct == 0x35u)
    {
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        readVi(viT);
        writeVi(viD);
        return usage;
    }
    if (direct == 0x32u)
    {
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viT);
        return usage;
    }
    if (direct < 0x3Cu)
    {
        usage.reserved = true;
        return usage;
    }

    const uint8_t special = static_cast<uint8_t>((lower & 3u) | ((lower >> 4) & 0x7Cu));
    switch (special)
    {
    case 0x30:
    case 0x31:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfRead(usage, vfS, special == 0x31u ? 0xFu : dest);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x34:
    case 0x36:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        usage.viLatency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viS);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x35:
    case 0x37:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viT);
        writeVi(viT);
        addVfRead(usage, vfS, dest);
        break;
    case 0x38:
        usage.pipeline = PipelineFdiv;
        usage.latency = 7u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x39:
        usage.pipeline = PipelineFdiv;
        usage.latency = 7u;
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x3A:
        usage.pipeline = PipelineFdiv;
        usage.latency = 13u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x3B:
        usage.pipeline = PipelineFdiv;
        usage.waitQ = true;
        break;
    case 0x3C:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        writeVi(viT);
        break;
    case 0x3D:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        readVi(viS);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x3E:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        writeVi(viT);
        break;
    case 0x3F:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viS);
        readVi(viT);
        break;
    case 0x40:
    case 0x41:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfWrite(usage, vfT, dest);
        break;
    case 0x42:
    case 0x43:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        break;
    case 0x64:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfWrite(usage, vfT, dest);
        break;
    case 0x68:
    case 0x69:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        writeVi(viT);
        break;
    case 0x6C:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineXgkick;
        usage.latency = 2u;
        readVi(viS);
        break;
    case 0x70:
    case 0x71:
    case 0x72:
    case 0x73:
    case 0x74:
    case 0x75:
    case 0x76:
    case 0x77:
    case 0x78:
    case 0x79:
    case 0x7A:
    case 0x7C:
    case 0x7D:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineEfu;
        switch (special)
        {
        case 0x70:
            usage.latency = 11u;
            break;
        case 0x71:
        case 0x72:
        case 0x77:
            usage.latency = 18u;
            break;
        case 0x73:
            usage.latency = 24u;
            break;
        case 0x74:
        case 0x75:
        case 0x7C:
            usage.latency = 54u;
            break;
        case 0x76:
        case 0x78:
        case 0x7A:
            usage.latency = 12u;
            break;
        case 0x79:
            usage.latency = 29u;
            break;
        case 0x7D:
            usage.latency = 44u;
            break;
        default:
            break;
        }
        if (special >= 0x70u && special <= 0x73u)
            addVfRead(usage, vfS, 0xEu);
        else if (special == 0x74u)
            addVfRead(usage, vfS, 0xCu);
        else if (special == 0x75u)
            addVfRead(usage, vfS, 0xAu);
        else if (special == 0x76u)
            addVfRead(usage, vfS, 0xFu);
        else
            addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        break;
    case 0x7B:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineEfu;
        usage.waitP = true;
        break;
    default:
        usage.reserved = true;
        break;
    }
    return usage;
}

VU1Interpreter::DecodedInstructionPair VU1Interpreter::decodeInstructionPair(const uint8_t *vuCode, uint32_t pc) const
{
    DecodedInstructionPair decoded;
    std::memcpy(&decoded.lower, vuCode + pc, sizeof(decoded.lower));
    std::memcpy(&decoded.upper, vuCode + pc + sizeof(decoded.lower), sizeof(decoded.upper));
    decoded.iBit = (decoded.upper & 0x80000000u) != 0u;
    decoded.eBit = (decoded.upper & 0x40000000u) != 0u;
    decoded.mBit = (decoded.upper & 0x20000000u) != 0u;
    decoded.dBit = (decoded.upper & 0x10000000u) != 0u;
    decoded.tBit = (decoded.upper & 0x08000000u) != 0u;
    decoded.upperUsage = decodeUpperUsage(decoded.upper);
    if (!decoded.iBit)
        decoded.lowerUsage = decodeLowerUsage(decoded.lower);

    const uint8_t upperWriteReg = decoded.upperUsage.vfWrite.reg;
    if (upperWriteReg != 0u && (vfReadLanes(decoded.lowerUsage, upperWriteReg) != 0u || decoded.lowerUsage.vfWrite.reg == upperWriteReg))
    {
        decoded.upperVfShadowReg = upperWriteReg;
        if (decoded.lowerUsage.vfWrite.reg == upperWriteReg)
            decoded.suppressedLowerVf = upperWriteReg;
    }
    return decoded;
}

void VU1Interpreter::rebuildDecodedCodeCache(const uint8_t *vuCode, uint32_t codeSize,
                                             const PS2Memory *memory, uint64_t generation)
{
    const uint32_t pairCount = std::min<uint32_t>(codeSize / 8u, kMaxDecodedPairs);
    for (uint32_t i = 0; i < pairCount; ++i)
        m_decodedCodeCache[i] = decodeInstructionPair(vuCode, i * 8u);

    m_cachedVuCode = vuCode;
    m_cachedMemory = memory;
    m_cachedCodeSize = codeSize;
    m_cachedCodeGeneration = generation;
    m_decodedCodeCacheValid = true;
}

const VU1Interpreter::DecodedInstructionPair &VU1Interpreter::getDecodedInstructionPairForPc(
    const uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory, uint32_t pc)
{
    if ((pc & 7u) != 0u)
        {
        m_decodeScratch = decodeInstructionPair(vuCode, pc);
        return m_decodeScratch;
    }

    const bool trackedVu1Code = memory != nullptr &&
                                ((m_unit == Unit::VU1 && vuCode == memory->getVU1Code()) ||
                                 (m_unit == Unit::VU0 && vuCode == memory->getVU0Code()));
    if (!trackedVu1Code)
        {
        m_decodeScratch = decodeInstructionPair(vuCode, pc);
        return m_decodeScratch;
    }

    const uint64_t generation = m_unit == Unit::VU1 ? memory->getVU1CodeGeneration() : memory->getVU0CodeGeneration();
    if (!m_decodedCodeCacheValid ||
        m_cachedVuCode != vuCode ||
        m_cachedMemory != memory ||
        m_cachedCodeSize != codeSize ||
        m_cachedCodeGeneration != generation)
    {
        rebuildDecodedCodeCache(vuCode, codeSize, memory, generation);
    }
    const uint32_t pairIndex = pc / 8u;
    if (pairIndex >= kMaxDecodedPairs)
        {
        m_decodeScratch = decodeInstructionPair(vuCode, pc);
        return m_decodeScratch;
    }
    return m_decodedCodeCache[pairIndex];
}

void VU1Interpreter::reportReservedInstruction(bool upper, uint32_t instruction)
{
    RUNTIME_ERROR(
        "[VU" << (m_unit == Unit::VU1 ? "1" : "0")
              << " reserved " << (upper ? "upper" : "lower")
              << "] cycle=" << m_cycle
              << " pc=0x" << std::hex << m_state.pc
              << " instruction=0x" << instruction
              << std::dec << '\n');
    m_stopRequested = true;
}

void VU1Interpreter::execute(uint8_t *vuCode, uint32_t codeSize,
                             uint8_t *vuData, uint32_t dataSize,
                             GS &gs, PS2Memory *memory,
                             uint32_t startPC, uint32_t top, uint32_t itop,
                             uint32_t maxCycles)
{
    resetScheduler();
    m_state.pc = startPC & microAddressMask();
    m_recTraceStartPc = m_state.pc;
    m_state.ebit = false;
    m_state.haltAfterDelaySlot = false;
    m_state.stoppedByD = false;
    m_state.stoppedByT = false;
    m_state.top = top;
    m_state.itop = itop;
    m_state.branchPending = false;
    m_state.branchTarget = 0;
    m_state.branchDelay = 0;
    m_state.vf[0][0] = 0.0f;
    m_state.vf[0][1] = 0.0f;
    m_state.vf[0][2] = 0.0f;
    m_state.vf[0][3] = 1.0f;
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
}

void VU1Interpreter::resume(uint8_t *vuCode, uint32_t codeSize,
                            uint8_t *vuData, uint32_t dataSize,
                            GS &gs, PS2Memory *memory,
                            uint32_t top, uint32_t itop, uint32_t maxCycles)
{
    m_state.top = top;
    m_state.itop = itop;
    m_state.stoppedByD = false;
    m_state.stoppedByT = false;
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
}

void VU1Interpreter::run(uint8_t *vuCode, uint32_t codeSize,
                         uint8_t *vuData, uint32_t dataSize,
                         GS &gs, PS2Memory *memory, uint32_t maxCycles)
{
    m_activeVuData = vuData;
    m_activeVuDataSize = dataSize;
    static const bool traceZeroSnap = std::getenv("PS2X_TRACE_ZERO_XYZ") != nullptr;
    if (traceZeroSnap && vuData)
    {
        m_traceRunData.assign(vuData, vuData + dataSize);
        m_traceRunState = m_state;
    }
    m_activeGs = &gs;
    m_activeMemory = memory;
    refreshFlagLiveness(vuCode, codeSize);

    const int previousRoundingMode = std::fegetround();
    const bool useVuRounding = std::fesetround(FE_TOWARDZERO) == 0;
    const uint64_t budgetEnd = m_cycle + maxCycles;
    bool programEnded = false;
    bool interpret = true;
    RecVerify *recVerify = nullptr;
    RecompiledFn recompiled = m_recDisabled ? nullptr : findRecompiled(vuCode, codeSize, memory);
    if (t_vu1Fresh && !t_vu1InNative && recompiled && nativeMode() != 0)
    {
        for (const NativeLoop &loop : nativeLoops())
        {
            if (loop.hash != m_recHash || loop.pc != m_state.pc)
                continue;
            commitReadyPipelines();
            if (nativeMode() == 2)
                verifyNativeLoop(*this, loop.fn, vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
            else
            {
                uint64_t cycles = 0;
                t_vu1KickTarget = this;
                const uint32_t pc = loop.fn(m_state, vuData, dataSize, cycles);
                t_vu1KickTarget = nullptr;
                if (pc)
                {
                    m_state.pc = pc;
                    advanceTo(m_cycle + cycles);
                    ps2_stats::vu1Instr.fetch_add(cycles, std::memory_order_relaxed);
                }
            }
            break;
        }
    }
    const uint64_t profileInstrStart = ps2_stats::vu1Instr.load(std::memory_order_relaxed);
    if (vu1ProfileEnabled() && t_vu1ProgramEnded)
    {
        t_vu1Routine = {m_recHash, m_state.pc};
        std::lock_guard<std::mutex> lock(g_vu1ProfileMutex);
        g_vu1Profile[t_vu1Routine].calls++;
    }
    if (RecompiledFn fn = recompiled)
    {
        static const bool verify = std::getenv("PS2X_VU1_REC_VERIFY") != nullptr;
        if (verify)
            recVerify = beginRecVerify(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
        const int result = fn(*this, vuData, dataSize, gs, memory, budgetEnd);
        ps2_stats::vu1RecRuns.fetch_add(1, std::memory_order_relaxed);
        if (result == 2)
        {
            ps2_stats::vu1RecBails.fetch_add(1, std::memory_order_relaxed);
            noteRecompiledBail();
        }
        if (result == 1)
            programEnded = true;
        interpret = result == 2;
    }
    uint64_t interpretedPairs = 0;
    if (interpret && !m_recDisabled)
        ps2_stats::vu1InterpRuns.fetch_add(1, std::memory_order_relaxed);
    while (interpret && m_cycle < budgetEnd && !m_stopRequested)
    {
        commitReadyPipelines();
        if (t_vu1StopPc != 0 && m_state.pc == t_vu1StopPc && !m_state.branchPending)
            break;
        if (m_state.pc + 8u > codeSize)
            break;

        const DecodedInstructionPair &decoded = getDecodedInstructionPairForPc(vuCode, codeSize, memory, m_state.pc);
        if (decoded.upperUsage.reserved || decoded.lowerUsage.reserved)
        {
            reportReservedInstruction(decoded.upperUsage.reserved, decoded.upperUsage.reserved ? decoded.upper : decoded.lower);
            break;
        }

        uint64_t readyCycle = calculatePairReadyCycle(decoded);
        while (readyCycle > m_cycle)
        {
            if (readyCycle >= budgetEnd)
            {
                advanceTo(budgetEnd);
                break;
            }
            ps2_stats::vu1StallCycles.fetch_add(readyCycle - m_cycle, std::memory_order_relaxed);
            advanceTo(readyCycle);
            readyCycle = calculatePairReadyCycle(decoded);
        }
        if (m_cycle >= budgetEnd)
            break;

        ++interpretedPairs;
        uint8_t writtenVi = 0u;
        int32_t oldVi = 0;
        if (const uint32_t viWriteBits = decoded.lowerUsage.viWrite & 0xFFFEu)
        {
            writtenVi = static_cast<uint8_t>(__builtin_ctz(viWriteBits));
            oldVi = m_state.vi[writtenVi];
        }

        const VfAccess upperWrite = decoded.upperUsage.vfWrite;
        const VfAccess lowerWrite = decoded.lowerUsage.vfWrite;
        const bool hasUpperWrite = upperWrite.reg != 0u;
        const bool hasLowerWrite = lowerWrite.reg != 0u && decoded.suppressedLowerVf != lowerWrite.reg;
        const bool hasDistinctLowerWrite = hasLowerWrite && (!hasUpperWrite || lowerWrite.reg != upperWrite.reg);
        float oldUpperVf[4]{};
        float newUpperVf[4]{};
        float oldLowerVf[4]{};
        float newLowerVf[4]{};
        float oldAcc[4]{};
        float newAcc[4]{};
        static const bool exactPipelinesEarly = std::getenv("PS2X_VU1_EXACT") != nullptr;
        if (exactPipelinesEarly && hasUpperWrite)
            std::memcpy(oldUpperVf, m_state.vf[upperWrite.reg], sizeof(oldUpperVf));
        if (exactPipelinesEarly && hasDistinctLowerWrite)
            std::memcpy(oldLowerVf, m_state.vf[lowerWrite.reg], sizeof(oldLowerVf));
        if (exactPipelinesEarly && decoded.upperUsage.accWrite != 0u)
            std::memcpy(oldAcc, m_state.acc, sizeof(oldAcc));

        if (decoded.iBit)
        {
            execUpper(decoded.upper);
            float immediate = 0.0f;
            std::memcpy(&immediate, &decoded.lower, sizeof(immediate));
            m_state.i = normalizeOperand(immediate);
        }
        else if (decoded.upperVfShadowReg != 0u)
        {
            float oldVf[4]{};
            float upperVf[4]{};
            std::memcpy(oldVf,
                        m_state.vf[decoded.upperVfShadowReg],
                        sizeof(oldVf));
            execUpper(decoded.upper);
            std::memcpy(upperVf,
                        m_state.vf[decoded.upperVfShadowReg],
                        sizeof(upperVf));
            std::memcpy(m_state.vf[decoded.upperVfShadowReg],
                        oldVf,
                        sizeof(oldVf));
            execLower(decoded.lower, vuData, dataSize, gs, memory, decoded.upper);
            std::memcpy(m_state.vf[decoded.upperVfShadowReg],
                        upperVf,
                        sizeof(upperVf));
        }
        else
        {
            execUpper(decoded.upper);
            execLower(decoded.lower, vuData, dataSize, gs, memory, decoded.upper);
        }

        m_viBranchBackupValid = false;

        // Every VF/VI/ACC read already stalls until its producer's ready cycle
        // (calculatePairReadyCycle/markPairWrites), so results can be written
        // immediately. PS2X_VU1_EXACT=1 keeps the full delayed-write model.
        static const bool exactPipelines = std::getenv("PS2X_VU1_EXACT") != nullptr;
        if (exactPipelines && hasUpperWrite)
        {
            std::memcpy(newUpperVf, m_state.vf[upperWrite.reg], sizeof(newUpperVf));
            std::memcpy(m_state.vf[upperWrite.reg], oldUpperVf, sizeof(oldUpperVf));
            const uint32_t latency =
                decoded.upperUsage.vfLatency != 0u
                    ? decoded.upperUsage.vfLatency
                    : decoded.upperUsage.latency;
            queueVfWrite(upperWrite.reg, upperWrite.lanes, newUpperVf, latency);
        }
        if (exactPipelines && hasDistinctLowerWrite)
        {
            std::memcpy(newLowerVf, m_state.vf[lowerWrite.reg], sizeof(newLowerVf));
            std::memcpy(m_state.vf[lowerWrite.reg], oldLowerVf, sizeof(oldLowerVf));
            const uint32_t latency = decoded.lowerUsage.vfLatency != 0u
                                         ? decoded.lowerUsage.vfLatency
                                         : decoded.lowerUsage.latency;
            queueVfWrite(lowerWrite.reg, lowerWrite.lanes, newLowerVf, latency);
        }
        if (exactPipelines && decoded.upperUsage.accWrite != 0u)
        {
            std::memcpy(newAcc, m_state.acc, sizeof(newAcc));
            std::memcpy(m_state.acc, oldAcc, sizeof(oldAcc));
            // ACC is forwarded to the next upper instruction. Its arithmetic
            // flags still use the normal four-cycle FMAC timeline.
            queueAccWrite(decoded.upperUsage.accWrite, newAcc,
                          kAccForwardLatency);
        }
        if (exactPipelines && writtenVi != 0u)
        {
            const int32_t newVi = m_state.vi[writtenVi];
            m_state.vi[writtenVi] = oldVi;
            const uint32_t latency =
                decoded.lowerUsage.viLatency != 0u
                    ? decoded.lowerUsage.viLatency
                    : decoded.lowerUsage.latency;
            queueViWrite(writtenVi, newVi, latency);
        }

        markPairWrites(decoded);
        if (writtenVi != 0u && decoded.lowerUsage.delaysNextBranchRead)
            recordViWriteForBranch(writtenVi, oldVi);

        m_state.vf[0][0] = 0.0f;
        m_state.vf[0][1] = 0.0f;
        m_state.vf[0][2] = 0.0f;
        m_state.vf[0][3] = 1.0f;
        m_state.vi[0] = 0;

        uint32_t nextPc = m_state.pc + 8u;
        if (nextPc >= codeSize)
            nextPc = 0u;
        m_state.pc = nextPc;

        if (m_state.branchPending)
        {
            if (m_state.branchDelay == 0u)
            {
                m_state.pc = m_state.branchTarget & microAddressMask();
                m_state.branchPending = false;
            }
            else
            {
                --m_state.branchDelay;
            }
        }

        const bool dHalt = decoded.dBit && m_state.dBitEnabled;
        const bool tHalt = decoded.tBit && m_state.tBitEnabled;
        const bool haltBit = dHalt || tHalt;
        const bool haltBranch = haltBit && decoded.lowerUsage.pipeline == PipelineBranch;

        if (m_state.haltAfterDelaySlot)
        {
            m_state.stoppedByD = m_pendingHaltD;
            m_state.stoppedByT = m_pendingHaltT;
            programEnded = true;
        }
        else if (m_state.ebit)
            programEnded = true;
        else if (haltBit && !haltBranch)
        {
            m_state.stoppedByD = dHalt;
            m_state.stoppedByT = tHalt;
            programEnded = true;
        }
        else if (decoded.eBit)
            m_state.ebit = true;
        else if (haltBranch)
        {
            m_state.haltAfterDelaySlot = true;
            m_pendingHaltD = dHalt;
            m_pendingHaltT = tHalt;
        }

        advanceOneCycle();
        if (programEnded)
            break;
    }

    if (programEnded)
    {
        flushPipelines();
        m_state.ebit = false;
        m_state.haltAfterDelaySlot = false;
        m_pendingHaltD = false;
        m_pendingHaltT = false;
    }
    catchUpXgkick();
    if (interpretedPairs)
        ps2_stats::vu1Instr.fetch_add(interpretedPairs, std::memory_order_relaxed);
    if (!t_vu1InNative)
        t_vu1Fresh = programEnded;
    if (vu1ProfileEnabled())
    {
        {
            std::lock_guard<std::mutex> lock(g_vu1ProfileMutex);
            g_vu1Profile[t_vu1Routine].instructions += ps2_stats::vu1Instr.load(std::memory_order_relaxed) - profileInstrStart;
        }
        t_vu1ProgramEnded = programEnded;
        vu1ProfileReport();
    }
    m_state.cycles = m_cycle;
    if (useVuRounding && previousRoundingMode != -1)
        std::fesetround(previousRoundingMode);
    if (recVerify)
        endRecVerify(recVerify, vuData, dataSize);
}

void vu1native::registerLoop(uint64_t imageHash, uint32_t entryPc, LoopFn fn, bool generated)
{
    // Hand-written loops win over generated regions for the same entry.
    auto &loops = nativeLoops();
    for (NativeLoop &loop : loops)
        if (loop.hash == imageHash && loop.pc == entryPc)
        {
            if (!generated)
            {
                loop.fn = fn;
                loop.generated = false;
            }
            return;
        }
    loops.push_back({imageHash, entryPc, fn, generated});
}

void vu1native::xgkick(uint32_t qwordAddress)
{
    VU1Interpreter *I = t_vu1KickTarget;
    if (!I)
        return;
    I->startXgkick(qwordAddress);
    // The whole packet, now: credit the transfer until it ends.
    for (int guard = 0; I->m_xgkick.active && guard < 64; ++guard)
    {
        I->m_xgkickDue = I->m_xgkick.progressedCycle + 0x40000u;
        I->catchUpXgkickSlow();
    }
}

bool vu1native::isRegistered(uint64_t imageHash, uint32_t entryPc)
{
    for (const NativeLoop &loop : nativeLoops())
        if (loop.hash == imageHash && loop.pc == entryPc && !loop.generated)
            return true;
    return false;
}

// The native loop on copies of the state, the microcode on the interpreter
// up to the pc the native code returns; any difference is reported.
static void verifyNativeLoop(VU1Interpreter &I, vu1native::LoopFn fn, uint8_t *vuCode, uint32_t codeSize, uint8_t *vuData, uint32_t dataSize,
                                      GS &gs, PS2Memory *memory, uint32_t maxCycles)
{
    VU1State nativeState = I.m_state;
    std::vector<uint8_t> nativeData(vuData, vuData + dataSize);
    uint64_t cycles = 0;
    const uint32_t stopPc = fn(nativeState, nativeData.data(), dataSize, cycles);
    if (stopPc == 0)
        return;
    const bool savedDisabled = I.m_recDisabled;
    I.m_recDisabled = true;
    t_vu1InNative = true;
    t_vu1StopPc = stopPc;
    I.run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
    t_vu1StopPc = 0;
    t_vu1InNative = false;
    I.m_recDisabled = savedDisabled;
    I.flushPipelines();

    std::string diff;
    auto note = [&](const std::string &text)
    {
        if (diff.size() < 600)
            diff += text + " ";
    };
    if (I.m_state.pc != stopPc)
        note("pc=" + std::to_string(I.m_state.pc));
    for (int r = 0; r < 32; ++r)
        for (int c = 0; c < 4; ++c)
            if (std::memcmp(&I.m_state.vf[r][c], &nativeState.vf[r][c], 4) != 0)
            {
                uint32_t a, b;
                std::memcpy(&a, &I.m_state.vf[r][c], 4);
                std::memcpy(&b, &nativeState.vf[r][c], 4);
                char text[96];
                std::snprintf(text, sizeof(text), "vf%d.%c ref=%08x native=%08x", r, "xyzw"[c], a, b);
                note(text);
            }
    for (int r = 1; r < 16; ++r)
        if (static_cast<int16_t>(I.m_state.vi[r]) != static_cast<int16_t>(nativeState.vi[r]))
            note("vi" + std::to_string(r) + " ref=" + std::to_string(I.m_state.vi[r]) + " native=" + std::to_string(nativeState.vi[r]));
    for (int c = 0; c < 4; ++c)
        if (std::memcmp(&I.m_state.acc[c], &nativeState.acc[c], 4) != 0)
            note(std::string("acc.") + "xyzw"[c]);
    if (std::memcmp(&I.m_state.q, &nativeState.q, 4) != 0)
        note("q");
    if (std::memcmp(&I.m_state.i, &nativeState.i, 4) != 0)
        note("i");
    if ((I.m_state.clip & 0xFFFFFFu) != (nativeState.clip & 0xFFFFFFu))
        note("clip");
    uint32_t memDiffs = 0, firstDiff = 0;
    for (uint32_t a = 0; a < dataSize; ++a)
        if (vuData[a] != nativeData[a] && memDiffs++ == 0)
            firstDiff = a;
    if (memDiffs)
        note("mem " + std::to_string(memDiffs) + " bytes from qword " + std::to_string(firstDiff / 16));

    std::lock_guard<std::mutex> lock(g_nativeVerify.mutex);
    ++g_nativeVerify.runs;
    static std::map<uint64_t, uint64_t> perImage;
    ++perImage[I.m_recHash];
    if (!diff.empty())
    {
        ++g_nativeVerify.mismatches;
        if (g_nativeVerify.reported++ < 20)
            std::fprintf(stderr, "[vu1:native] MISMATCH hash=%016llx: %s\n", static_cast<unsigned long long>(I.m_recHash), diff.c_str());
    }
    if ((g_nativeVerify.runs & 0xFFFu) == 0u)
    {
        std::fprintf(stderr, "[vu1:native] verified %llu runs, %llu mismatches |", static_cast<unsigned long long>(g_nativeVerify.runs),
                     static_cast<unsigned long long>(g_nativeVerify.mismatches));
        for (const auto &[hash, runs] : perImage)
            std::fprintf(stderr, " %08llx:%llu", static_cast<unsigned long long>(hash >> 32), static_cast<unsigned long long>(runs));
        std::fprintf(stderr, "\n");
    }
}

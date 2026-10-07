#include "runtime/ps2_vu1.h"
#include "runtime/ps2_memory.h"
#include "ps2_vu1_detail.h"

#include <algorithm>
#include <cfenv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    std::unordered_map<uint64_t, VU1Interpreter::RecompiledFn> &registry()
    {
        static std::unordered_map<uint64_t, VU1Interpreter::RecompiledFn> map;
        return map;
    }

    uint64_t hashImage(const uint8_t *code, uint32_t size)
    {
        uint64_t hash = 1469598103934665603ull;
        for (uint32_t i = 0; i < size; ++i)
            hash = (hash ^ code[i]) * 1099511628211ull;
        return hash;
    }

    // Cheap 128-bit fingerprint of a code image (8-byte words, two lanes),
    // used to skip the byte-wise hash for images seen before.
    std::pair<uint64_t, uint64_t> fingerprint(const uint8_t *code, uint32_t size)
    {
        uint64_t a = 0x9E3779B97F4A7C15ull, b = 0xC2B2AE3D27D4EB4Full;
        for (uint32_t i = 0; i + 8u <= size; i += 8u)
        {
            uint64_t w;
            std::memcpy(&w, code + i, sizeof(w));
            a = (a ^ w) * 0xFF51AFD7ED558CCDull;
            a ^= a >> 29;
            b = (b + w) * 0xC4CEB9FE1A85EC53ull;
            b ^= b >> 31;
        }
        return {a, b};
    }

    struct FingerprintHash
    {
        size_t operator()(const std::pair<uint64_t, uint64_t> &key) const { return key.first ^ (key.second * 31u); }
    };

    uint8_t laneBit(uint32_t component) { return static_cast<uint8_t>(0x8u >> component); }
}

void VU1Interpreter::registerRecompiled(uint64_t imageHash, RecompiledFn fn)
{
    registry()[imageHash] = fn;
}

void VU1Interpreter::noteRecompiledBail()
{
    // PS2X_VU1_DUMP_MISSING also records pcs the recompiled code did not
    // cover, so the next generation includes them.
    static const char *missingDir = std::getenv("PS2X_VU1_DUMP_MISSING");
    static std::set<std::pair<uint64_t, uint32_t>> seen;
    if (!seen.insert({m_recHash, m_state.pc}).second)
        return;
    std::fprintf(stderr, "[vu1rec] bail image=%016llx pc=0x%04x\n", static_cast<unsigned long long>(m_recHash), m_state.pc);
    if (!missingDir)
        return;
    char path[1024];
    std::snprintf(path, sizeof(path), "%s/%016llx.entries", missingDir, static_cast<unsigned long long>(m_recHash));
    if (FILE *f = std::fopen(path, "a"))
    {
        std::fprintf(f, "%04x\n", m_state.pc);
        std::fclose(f);
    }
}

VU1Interpreter::RecompiledFn VU1Interpreter::findRecompiled(const uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory)
{
    static const bool disabled = std::getenv("PS2X_VU1_EXACT") != nullptr ||
                                 std::getenv("PS2X_VU1_NOREC") != nullptr;
    // With nothing recompiled yet, only keep going to record images.
    static const bool collecting = std::getenv("PS2X_VU1_DUMP_MISSING") != nullptr;
    if (disabled || m_unit != Unit::VU1 || !memory || vuCode != memory->getVU1Code() ||
        codeSize != 0x4000u || (registry().empty() && !collecting))
        return nullptr;
    const uint64_t generation = memory->getVU1CodeGeneration();
    static uint64_t imageHash = 0;
    if (generation != m_recGeneration || vuCode != m_recCode)
    {
        // Code changes with every microprogram upload; most images recur.
        static std::unordered_map<std::pair<uint64_t, uint64_t>, uint64_t, FingerprintHash> known;
        const auto print = fingerprint(vuCode, codeSize);
        const auto cached = known.find(print);
        if (cached != known.end())
            imageHash = cached->second;
        else
            imageHash = known.emplace(print, hashImage(vuCode, codeSize)).first->second;
        m_recHash = imageHash;
        const auto it = registry().find(imageHash);
        m_recFn = it != registry().end() ? it->second : nullptr;
        m_recGeneration = generation;
        m_recCode = vuCode;
    }
    // PS2X_VU1_DUMP_MISSING=<dir>: save microcode images that have no
    // recompiled version yet, with the pcs they start at, for the generator.
    static const char *missingDir = std::getenv("PS2X_VU1_DUMP_MISSING");
    if (!m_recFn && missingDir)
    {
        static std::set<std::pair<uint64_t, uint32_t>> seen;
        if (seen.insert({imageHash, m_state.pc}).second)
        {
            char path[1024];
            std::snprintf(path, sizeof(path), "%s/%016llx.bin", missingDir, static_cast<unsigned long long>(imageHash));
            if (FILE *existing = std::fopen(path, "rb"))
                std::fclose(existing);
            else if (FILE *f = std::fopen(path, "wb"))
            {
                std::fwrite(vuCode, 1, codeSize, f);
                std::fclose(f);
                std::fprintf(stderr, "[vu1rec] new microcode image %016llx (interpreted)\n",
                             static_cast<unsigned long long>(imageHash));
            }
            std::snprintf(path, sizeof(path), "%s/%016llx.entries", missingDir, static_cast<unsigned long long>(imageHash));
            if (FILE *f = std::fopen(path, "a"))
            {
                std::fprintf(f, "%04x\n", m_state.pc);
                std::fclose(f);
            }
        }
    }
    return m_recFn;
}

// ---------------------------------------------------------------------------
// PS2X_VU1_REC_VERIFY: run the interpreter on a copy first and compare.
// ---------------------------------------------------------------------------
struct VU1Interpreter::RecVerify
{
    std::unique_ptr<VU1Interpreter> reference;
    std::vector<uint8_t> referenceData;
    std::vector<uint8_t> referenceGif;
    std::vector<uint8_t> recompiledGif;
    uint32_t startPc = 0;
};

VU1Interpreter::RecVerify *VU1Interpreter::beginRecVerify(uint8_t *vuCode, uint32_t codeSize, uint8_t *vuData,
                                                          uint32_t dataSize, GS &gs, PS2Memory *memory,
                                                          uint32_t maxCycles)
{
    static RecVerify verify;
    if (!verify.reference)
        verify.reference = std::make_unique<VU1Interpreter>(m_unit);
    verify.startPc = m_state.pc;
    *verify.reference = *this;
    verify.reference->m_recDisabled = true;
    verify.referenceData.assign(vuData, vuData + dataSize);
    verify.referenceGif.clear();
    verify.recompiledGif.clear();
    verify.reference->m_gifCapture = &verify.referenceGif;
    verify.reference->m_gifCaptureOnly = true;
    const int rounding = std::fegetround();
    verify.reference->run(vuCode, codeSize, verify.referenceData.data(), dataSize, gs, memory, maxCycles);
    std::fesetround(rounding);
    m_gifCapture = &verify.recompiledGif;
    m_gifCaptureOnly = false;
    return &verify;
}

void VU1Interpreter::endRecVerify(RecVerify *verify, const uint8_t *vuData, uint32_t dataSize)
{
    m_gifCapture = nullptr;
    static uint64_t checks = 0;
    static uint64_t failures = 0;
    ++checks;
    const VU1Interpreter &ref = *verify->reference;
    std::ostringstream diff;
    auto cmp = [&diff](const char *name, const void *a, const void *b, size_t size)
    {
        if (std::memcmp(a, b, size) != 0)
            diff << ' ' << name;
    };
    for (int r = 0; r < 32; ++r)
    {
        if (std::memcmp(m_state.vf[r], ref.m_state.vf[r], 16) != 0)
        {
            diff << " vf" << r;
            for (int c = 0; c < 4; ++c)
                diff << (c ? "," : "(") << m_state.vf[r][c] << "/" << ref.m_state.vf[r][c];
            diff << ")";
        }
    }
    for (int r = 0; r < 16; ++r)
        if (m_state.vi[r] != ref.m_state.vi[r])
            diff << " vi" << r << "(" << m_state.vi[r] << "/" << ref.m_state.vi[r] << ")";
    cmp("acc", m_state.acc, ref.m_state.acc, sizeof(m_state.acc));
    cmp("q", &m_state.q, &ref.m_state.q, 4);
    cmp("p", &m_state.p, &ref.m_state.p, 4);
    cmp("i", &m_state.i, &ref.m_state.i, 4);
    cmp("mac", &m_state.mac, &ref.m_state.mac, 4);
    cmp("clip", &m_state.clip, &ref.m_state.clip, 4);
    // Sticky status bits (6..11) are not tracked for FMACs whose flags are
    // provably unobservable; no compiled image reads them.
    if ((m_state.status & 0x3Fu) != (ref.m_state.status & 0x3Fu))
        diff << " status";
    cmp("pc", &m_state.pc, &ref.m_state.pc, 4);
    cmp("cycle", &m_cycle, &ref.m_cycle, 8);
    cmp("stopD", &m_state.stoppedByD, &ref.m_state.stoppedByD, 1);
    cmp("stopT", &m_state.stoppedByT, &ref.m_state.stoppedByT, 1);
    if (verify->referenceData.size() != dataSize ||
        std::memcmp(verify->referenceData.data(), vuData, dataSize) != 0)
    {
        uint32_t first = 0;
        while (first < dataSize && verify->referenceData[first] == vuData[first])
            ++first;
        diff << " data@0x" << std::hex << first << std::dec;
    }
    if (verify->referenceGif != verify->recompiledGif)
        diff << " gif(" << verify->recompiledGif.size() << "/" << verify->referenceGif.size() << ")";
    if (!diff.str().empty())
    {
        ++failures;
        if (failures <= 50)
            std::fprintf(stderr, "[vu1rec:verify] MISMATCH start=0x%04x pc=0x%04x/0x%04x:%s\n",
                         verify->startPc, m_state.pc, ref.m_state.pc, diff.str().c_str());
    }
    if ((checks & 0x3FFu) == 0u)
        std::fprintf(stderr, "[vu1rec:verify] %llu runs, %llu mismatches\n",
                     static_cast<unsigned long long>(checks), static_cast<unsigned long long>(failures));
}

// ---------------------------------------------------------------------------
// Generator
// ---------------------------------------------------------------------------
namespace
{
    struct LaneOp
    {
        const char *name;
        bool toAcc;
        bool minmax;   // result is not normalized and sets no flags
        const char *expr; // per lane: s t a b q i, '#' = lane index
        bool cross;       // OPMULA/OPMSUB
    };

    // Returns false for upper instructions that are emitted as execUpper calls.
    bool describeUpper(uint32_t instr, LaneOp &out, uint8_t &bc)
    {
        const uint8_t op = instr & 0x3Fu;
        bc = instr & 3u;
        if (op < 0x3Cu)
        {
            switch (op >> 2)
            {
            case 0x0: out = {"ADDbc", false, false, "s+b", false}; return true;
            case 0x1: out = {"SUBbc", false, false, "s-b", false}; return true;
            case 0x2: out = {"MADDbc", false, false, "a+s*b", false}; return true;
            case 0x3: out = {"MSUBbc", false, false, "a-s*b", false}; return true;
            case 0x4: out = {"MAXbc", false, true, "max(s,b)", false}; return true;
            case 0x5: out = {"MINIbc", false, true, "min(s,b)", false}; return true;
            case 0x6: out = {"MULbc", false, false, "s*b", false}; return true;
            default:
                break;
            }
            switch (op)
            {
            case 0x1C: out = {"MULq", false, false, "s*q", false}; return true;
            case 0x1D: out = {"MAXi", false, true, "max(s,i)", false}; return true;
            case 0x1E: out = {"MULi", false, false, "s*i", false}; return true;
            case 0x1F: out = {"MINIi", false, true, "min(s,i)", false}; return true;
            case 0x20: out = {"ADDq", false, false, "s+q", false}; return true;
            case 0x21: out = {"MADDq", false, false, "a+s*q", false}; return true;
            case 0x22: out = {"ADDi", false, false, "s+i", false}; return true;
            case 0x23: out = {"MADDi", false, false, "a+s*i", false}; return true;
            case 0x24: out = {"SUBq", false, false, "s-q", false}; return true;
            case 0x25: out = {"MSUBq", false, false, "a-s*q", false}; return true;
            case 0x26: out = {"SUBi", false, false, "s-i", false}; return true;
            case 0x27: out = {"MSUBi", false, false, "a-s*i", false}; return true;
            case 0x28: out = {"ADD", false, false, "s+t", false}; return true;
            case 0x29: out = {"MADD", false, false, "a+s*t", false}; return true;
            case 0x2A: out = {"MUL", false, false, "s*t", false}; return true;
            case 0x2B: out = {"MAX", false, true, "max(s,t)", false}; return true;
            case 0x2C: out = {"SUB", false, false, "s-t", false}; return true;
            case 0x2D: out = {"MSUB", false, false, "a-s*t", false}; return true;
            case 0x2E: out = {"OPMSUB", false, false, "a-s*t", true}; return true;
            case 0x2F: out = {"MINI", false, true, "min(s,t)", false}; return true;
            default:
                return false;
            }
        }
        const uint8_t special = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
        switch (special >> 2)
        {
        case 0x0: out = {"ADDAbc", true, false, "s+b", false}; return true;
        case 0x1: out = {"SUBAbc", true, false, "s-b", false}; return true;
        case 0x2: out = {"MADDAbc", true, false, "a+s*b", false}; return true;
        case 0x3: out = {"MSUBAbc", true, false, "a-s*b", false}; return true;
        case 0x6: out = {"MULAbc", true, false, "s*b", false}; return true;
        default:
            break;
        }
        switch (special)
        {
        case 0x1C: out = {"MULAq", true, false, "s*q", false}; return true;
        case 0x1E: out = {"MULAi", true, false, "s*i", false}; return true;
        case 0x20: out = {"ADDAq", true, false, "s+q", false}; return true;
        case 0x21: out = {"MADDAq", true, false, "a+s*q", false}; return true;
        case 0x22: out = {"ADDAi", true, false, "s+i", false}; return true;
        case 0x23: out = {"MADDAi", true, false, "a+s*i", false}; return true;
        case 0x24: out = {"SUBAq", true, false, "s-q", false}; return true;
        case 0x25: out = {"MSUBAq", true, false, "a-s*q", false}; return true;
        case 0x26: out = {"SUBAi", true, false, "s-i", false}; return true;
        case 0x27: out = {"MSUBAi", true, false, "a-s*i", false}; return true;
        case 0x28: out = {"ADDA", true, false, "s+t", false}; return true;
        case 0x29: out = {"MADDA", true, false, "a+s*t", false}; return true;
        case 0x2A: out = {"MULA", true, false, "s*t", false}; return true;
        case 0x2C: out = {"SUBA", true, false, "s-t", false}; return true;
        case 0x2D: out = {"MSUBA", true, false, "a-s*t", false}; return true;
        case 0x2E: out = {"OPMULA", true, false, "s*t", true}; return true;
        case 0x2F:
        case 0x30: out = {"NOP", false, false, "", false}; return true;
        default:
            return false;
        }
    }

    // Expression for one result lane, mirroring the interpreter's formulas.
    std::string laneExpression(const LaneOp &op, uint32_t c, std::set<std::string> &uses)
    {
        static const char *lanes = "xyzw";
        uint32_t sLane = c, tLane = c;
        if (op.cross)
        {
            static const uint32_t sCross[3] = {1, 2, 0};
            static const uint32_t tCross[3] = {2, 0, 1};
            if (c == 3u)
                return "0.0f";
            sLane = sCross[c];
            tLane = tCross[c];
        }
        auto var = [&](char kind) -> std::string
        {
            std::string name;
            switch (kind)
            {
            case 's': name = std::string("s") + lanes[sLane]; break;
            case 't': name = std::string("t") + lanes[tLane]; break;
            case 'a': name = std::string("a") + lanes[c]; break;
            case 'b': name = "bc"; break;
            case 'q': name = "qv"; break;
            case 'i': name = "iv"; break;
            }
            uses.insert(name);
            return name;
        };
        const std::string e = op.expr;
        if (e.rfind("max(", 0) == 0 || e.rfind("min(", 0) == 0)
        {
            const std::string x = var(e[4]);
            const std::string y = var(e[6]);
            return "(" + x + (e[1] == 'a' ? " > " : " < ") + y + ") ? " + x + " : " + y;
        }
        std::string out;
        for (char ch : e)
        {
            if (ch == '+' || ch == '-' || ch == '*')
                out += std::string(" ") + ch + " ";
            else
                out += var(ch);
        }
        return out;
    }

    std::string hex(uint64_t value, int width = 0);

    // Direct code for common lower instructions; empty means execLower.
    std::string lowerCode(uint32_t instr, const std::string &in)
    {
        const uint8_t opHi = static_cast<uint8_t>((instr >> 25) & 0x7Fu);
        const int dest = (instr >> 21) & 0xF;
        const int ft = FT(instr), fs = FS(instr);
        const int vit = VIT(instr), vis = VIS(instr), vid = VID(instr);
        const int imm11 = IMM11(instr);
        const std::string D = std::to_string(dest);
        auto vi = [](int r) { return "S.vi[" + std::to_string(r) + "]"; };
        auto vf = [](int r) { return "S.vf[" + std::to_string(r) + "]"; };
        auto comp = [dest]() { return (dest & 8) ? 0 : (dest & 4) ? 1 : (dest & 2) ? 2 : 3; };
        auto setVi = [&](int r, const std::string &value) -> std::string
        {
            return r == 0 ? std::string() : in + vi(r) + " = " + value + ";\n";
        };
        switch (opHi)
        {
        case 0x00: // LQ
            return in + "vu1rec::loadQ<" + D + ">(" + vf(ft) + ", vuData, dataSize, (uint32_t)(int32_t)(" + vi(vis) + " + " + std::to_string(imm11) + "));\n";
        case 0x01: // SQ
            return in + "vu1rec::storeQ<" + D + ">(I, vuData, dataSize, (uint32_t)(int32_t)(" + vi(vit) + " + " + std::to_string(imm11) + "), " + vf(fs) + ");\n";
        case 0x04: // ILW
            return setVi(vit, "vu1rec::loadVi(vuData, dataSize, (uint32_t)(int32_t)(" + vi(vis) + " + " + std::to_string(imm11) + "), " + std::to_string(comp()) + "u, " + vi(vit) + ")");
        case 0x05: // ISW
            return in + "vu1rec::storeVi<" + D + ">(I, vuData, dataSize, (uint32_t)(int32_t)(" + vi(vis) + " + " + std::to_string(imm11) + "), " + vi(vit) + ");\n";
        case 0x08: // IADDIU
        case 0x09: // ISUBIU
        {
            const int imm15 = static_cast<int16_t>((instr & 0x7FF) | ((instr >> 10) & 0x7800));
            return setVi(vit, "(int16_t)(" + vi(vis) + (opHi == 0x08 ? " + " : " - ") + "(int16_t)" + std::to_string(imm15) + ")");
        }
        case 0x10: // FCEQ
            return setVi(1, "((S.clip & 0xFFFFFFu) == " + hex(instr & 0xFFFFFF) + "u) ? 1 : 0");
        case 0x12: // FCAND
            return setVi(1, "((S.clip & " + hex(instr & 0xFFFFFF) + "u) != 0u) ? 1 : 0");
        case 0x13: // FCOR
            return setVi(1, "((S.clip | " + hex(instr & 0xFFFFFF) + "u) == 0xFFFFFFu) ? 1 : 0");
        case 0x40:
            break;
        default:
            return {};
        }
        const uint8_t funct = instr & 0x3Fu;
        switch (funct)
        {
        case 0x30: return setVi(vid, "(int16_t)(" + vi(vis) + " + " + vi(vit) + ")");
        case 0x31: return setVi(vid, "(int16_t)(" + vi(vis) + " - " + vi(vit) + ")");
        case 0x32:
        {
            const int imm5 = static_cast<int16_t>(static_cast<int32_t>((instr >> 6) & 0x1F) << 27 >> 27);
            return setVi(vit, "(int16_t)(" + vi(vis) + " + (int16_t)" + std::to_string(imm5) + ")");
        }
        case 0x34: return setVi(vid, vi(vis) + " & " + vi(vit));
        case 0x35: return setVi(vid, vi(vis) + " | " + vi(vit));
        case 0x3C: case 0x3D: case 0x3E: case 0x3F: break;
        default: return {};
        }
        const uint8_t funct2 = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
        switch (funct2)
        {
        case 0x30: // MOVE
            return in + "{ float t[4]; std::memcpy(t, " + vf(fs) + ", 16); vu1rec::applyDest<" + D + ">(" + vf(ft) + ", t); }\n";
        case 0x31: // MR32
            return in + "{ const float t[4] = {" + vf(fs) + "[1], " + vf(fs) + "[2], " + vf(fs) + "[3], " + vf(fs) + "[0]}; vu1rec::applyDest<" + D + ">(" + vf(ft) + ", t); }\n";
        case 0x34: // LQI
            return in + "vu1rec::loadQ<" + D + ">(" + vf(ft) + ", vuData, dataSize, (uint32_t)(uint16_t)" + vi(vis) + ");\n" +
                   setVi(vis, "(int16_t)(" + vi(vis) + " + 1)");
        case 0x35: // SQI
            return in + "vu1rec::storeQ<" + D + ">(I, vuData, dataSize, (uint32_t)(uint16_t)" + vi(vit) + ", " + vf(fs) + ");\n" +
                   setVi(vit, "(int16_t)(" + vi(vit) + " + 1)");
        case 0x36: // LQD
            return setVi(vis, "(int16_t)(" + vi(vis) + " - 1)") +
                   in + "vu1rec::loadQ<" + D + ">(" + vf(ft) + ", vuData, dataSize, (uint32_t)(uint16_t)" + vi(vis) + ");\n";
        case 0x37: // SQD
            return setVi(vit, "(int16_t)(" + vi(vit) + " - 1)") +
                   in + "vu1rec::storeQ<" + D + ">(I, vuData, dataSize, (uint32_t)(uint16_t)" + vi(vit) + ", " + vf(fs) + ");\n";
        case 0x3C: // MTIR
        {
            const int c = (instr >> 21) & 3;
            return setVi(vit, "[&] { uint32_t b; std::memcpy(&b, &" + vf(fs) + "[" + std::to_string(c) + "], 4); return (int32_t)(int16_t)(b & 0xFFFF); }()");
        }
        case 0x3D: // MFIR
            return in + "{ const int32_t v = (int32_t)(int16_t)(" + vi(vis) + " & 0xFFFF); float f; std::memcpy(&f, &v, 4); const float t[4] = {f, f, f, f}; vu1rec::applyDest<" + D + ">(" + vf(ft) + ", t); }\n";
        case 0x3E: // ILWR
            return setVi(vit, "vu1rec::loadVi(vuData, dataSize, (uint32_t)(uint16_t)" + vi(vis) + ", " + std::to_string(comp()) + "u, " + vi(vit) + ")");
        case 0x3F: // ISWR
            return in + "vu1rec::storeVi<" + D + ">(I, vuData, dataSize, (uint32_t)(uint16_t)" + vi(vis) + ", " + vi(vit) + ");\n";
        default:
            return {};
        }
    }

    // Direct code for ITOF/FTOI/ABS; empty means execUpper.
    std::string upperConvertCode(uint32_t instr, const std::string &in)
    {
        if ((instr & 0x3Fu) < 0x3Cu)
            return {};
        const uint8_t special = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
        const int dest = (instr >> 21) & 0xF;
        const std::string D = std::to_string(dest);
        const std::string vs = "S.vf[" + std::to_string(FS(instr)) + "]";
        const std::string vt = "S.vf[" + std::to_string(FT(instr)) + "]";
        static const char *itofDiv[4] = {"", " / 16.0f", " / 4096.0f", " / 32768.0f"};
        static const char *ftoiScale[4] = {"1.0f", "16.0f", "4096.0f", "32768.0f"};
        std::ostringstream o;
        o << in << "{ I.m_currentUpperInstruction = " << hex(instr) << "u; float r[4]; ";
        if (special >= 0x10 && special <= 0x13)
        {
            o << "for (int c = 0; c < 4; ++c) { int32_t iv; std::memcpy(&iv, &" << vs << "[c], 4); r[c] = static_cast<float>(iv)" << itofDiv[special & 3] << "; } ";
        }
        else if (special >= 0x14 && special <= 0x17)
        {
            o << "for (int c = 0; c < 4; ++c) { const int32_t iv = vu1rec::floatToInt(vu1rec::norm(" << vs << "[c]), " << ftoiScale[special & 3] << "); std::memcpy(&r[c], &iv, 4); } ";
        }
        else if (special == 0x1D)
        {
            o << "for (int c = 0; c < 4; ++c) r[c] = std::fabs(vu1rec::norm(" << vs << "[c])); ";
        }
        else
            return {};
        o << "vu1rec::applyDest<" << D << ">(" << vt << ", r); }\n";
        return o.str();
    }

    struct ImageInfo
    {
        uint64_t hash = 0;
        std::vector<uint8_t> code;
        std::set<uint32_t> entries;
    };

    bool isBranchLower(uint32_t lower, bool iBit)
    {
        if (iBit)
            return false;
        const uint8_t opHi = static_cast<uint8_t>((lower >> 25) & 0x7Fu);
        return opHi == 0x20u || opHi == 0x21u || opHi == 0x24u || opHi == 0x25u ||
               (opHi >= 0x28u && opHi <= 0x2Fu);
    }

    std::string hex(uint64_t value, int width)
    {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "0x%0*llx", width, static_cast<unsigned long long>(value));
        return buffer;
    }
}

int VU1Interpreter::generateRecompiled(const char *dumpDir, const char *outDir)
{
    std::map<uint64_t, ImageInfo> images;
    DIR *dir = opendir(dumpDir);
    if (!dir)
    {
        std::fprintf(stderr, "[vu1rec] cannot open %s\n", dumpDir);
        return 1;
    }
    while (dirent *entry = readdir(dir))
    {
        const std::string name = entry->d_name;
        if (name.size() != 20 || name.substr(16) != ".bin")
            continue;
        ImageInfo info;
        info.hash = std::strtoull(name.substr(0, 16).c_str(), nullptr, 16);
        std::ifstream bin(std::string(dumpDir) + "/" + name, std::ios::binary);
        info.code.assign(std::istreambuf_iterator<char>(bin), std::istreambuf_iterator<char>());
        if (info.code.size() != 0x4000u || hashImage(info.code.data(), 0x4000u) != info.hash)
        {
            std::fprintf(stderr, "[vu1rec] skipping %s (bad size/hash)\n", name.c_str());
            continue;
        }
        std::ifstream entries(std::string(dumpDir) + "/" + name.substr(0, 16) + ".entries");
        std::string line;
        while (std::getline(entries, line))
            if (!line.empty())
                info.entries.insert(static_cast<uint32_t>(std::strtoul(line.c_str(), nullptr, 16)) & 0x3FF8u);
        images[info.hash] = std::move(info);
    }
    closedir(dir);

    auto interp = std::make_unique<VU1Interpreter>(Unit::VU1);
    for (const auto &[hash, image] : images)
    {
        const uint8_t *code = image.code.data();
        std::vector<DecodedInstructionPair> pairs(0x800);
        for (uint32_t pc = 0; pc < 0x4000u; pc += 8u)
            pairs[pc / 8u] = interp->decodeInstructionPair(code, pc);

        // Reachable pairs from the entry points. Over-approximation is fine:
        // anything not compiled hands control back to the interpreter. A
        // "final" visit is the delay slot of an unconditional jump or E bit,
        // which does not fall through.
        std::vector<bool> reachable(0x800, false);
        std::vector<bool> visitedFinal(0x800, false);
        std::vector<std::pair<uint32_t, bool>> work;
        for (uint32_t entry : image.entries)
            work.push_back({entry, false});
        if (work.empty())
            work.push_back({0u, false});
        while (!work.empty())
        {
            const uint32_t pc = work.back().first & 0x3FF8u;
            const bool final = work.back().second;
            work.pop_back();
            if (reachable[pc / 8u] || (final && visitedFinal[pc / 8u]))
                continue;
            (final ? visitedFinal : reachable)[pc / 8u] = true;
            const DecodedInstructionPair &d = pairs[pc / 8u];
            const uint32_t next = (pc + 8u) & 0x3FF8u;
            if (isBranchLower(d.lower, d.iBit))
            {
                const uint8_t opHi = static_cast<uint8_t>((d.lower >> 25) & 0x7Fu);
                if (opHi != 0x24u && opHi != 0x25u)
                    work.push_back({(pc + 8u + static_cast<int32_t>(IMM11(d.lower)) * 8) & 0x3FF8u, false});
                if (opHi == 0x21u || opHi == 0x25u)
                    work.push_back({(pc + 16u) & 0x3FF8u, false}); // return point
                const bool unconditional = opHi == 0x20u || opHi == 0x21u || opHi == 0x24u || opHi == 0x25u;
                if (!final)
                    work.push_back({next, unconditional});
            }
            else if (!final)
            {
                work.push_back({next, d.eBit});
                // MSCNT resumes after the E bit's delay slot.
                if (d.eBit)
                    work.push_back({(pc + 16u) & 0x3FF8u, false});
            }
        }
        for (uint32_t k = 0; k < 0x800u; ++k)
            if (visitedFinal[k])
                reachable[k] = true;

        // Flag liveness, identical to refreshFlagLiveness().
        bool flagsLive = false;
        bool stickyLive = false;
        for (uint32_t pc = 0; pc < 0x4000u; pc += 8u)
        {
            const DecodedInstructionPair &d = pairs[pc / 8u];
            if (d.iBit)
                continue;
            const uint8_t opHi = static_cast<uint8_t>((d.lower >> 25) & 0x7Fu);
            const uint16_t imm12 = static_cast<uint16_t>((((d.lower >> 21) & 0x1u) << 11) | (d.lower & 0x7FFu));
            if (opHi == 0x18u || opHi == 0x1Au || opHi == 0x1Bu)
                flagsLive = true;
            if (opHi == 0x14u || opHi == 0x16u || opHi == 0x17u)
            {
                flagsLive = true;
                if (opHi == 0x14u || (imm12 & 0xFC0u) != 0u)
                    stickyLive = true;
            }
        }

        // Per-instruction MAC/status liveness. An FMAC result's flags become
        // visible four cycles after issue and are replaced once a later FMAC's
        // flags become visible. Every pair takes at least one cycle, so a
        // reader four or more pairs after a later writer cannot observe the
        // earlier one. Program exits keep the flags of the last writer.
        std::vector<bool> flagWriter(0x800, false);
        std::vector<bool> flagReader(0x800, false);
        std::vector<bool> exitAfter(0x800, false);
        for (uint32_t k = 0; k < 0x800u; ++k)
        {
            const DecodedInstructionPair &d = pairs[k];
            LaneOp op{};
            uint8_t bc = 0;
            if (describeUpper(d.upper, op, bc) && !op.minmax && op.expr[0] != '\0' && DEST(d.upper) != 0u)
                flagWriter[k] = true;
            if (!d.iBit)
            {
                const uint8_t opHi = static_cast<uint8_t>((d.lower >> 25) & 0x7Fu);
                if (opHi == 0x18u || opHi == 0x1Au || opHi == 0x1Bu || opHi == 0x14u || opHi == 0x16u || opHi == 0x17u)
                    flagReader[k] = true;
            }
            const DecodedInstructionPair &prev = pairs[(k + 0x7FFu) & 0x7FFu];
            const uint8_t prevOp = static_cast<uint8_t>((prev.lower >> 25) & 0x7Fu);
            if (prev.eBit || d.dBit || d.tBit || !reachable[(k + 1u) & 0x7FFu] ||
                (isBranchLower(prev.lower, prev.iBit) && (prevOp == 0x24u || prevOp == 0x25u)))
                exitAfter[k] = true;
        }
        auto successors = [&](uint32_t k, uint32_t out[2]) -> int
        {
            int n = 0;
            out[n++] = (k + 1u) & 0x7FFu;
            const uint32_t prevIndex = (k + 0x7FFu) & 0x7FFu;
            const DecodedInstructionPair &prev = pairs[prevIndex];
            if (isBranchLower(prev.lower, prev.iBit))
            {
                const uint8_t opHi = static_cast<uint8_t>((prev.lower >> 25) & 0x7Fu);
                if (opHi != 0x24u && opHi != 0x25u)
                    out[n++] = ((prevIndex * 8u + 8u + static_cast<int32_t>(IMM11(prev.lower)) * 8) & 0x3FF8u) / 8u;
            }
            return n;
        };
        constexpr uint8_t kNoLaterWriter = 0;
        std::vector<bool> flagsNeeded(0x800, false);
        uint32_t writers = 0, needed = 0;
        for (uint32_t p = 0; p < 0x800u; ++p)
        {
            if (!reachable[p] || !flagWriter[p])
                continue;
            ++writers;
            if (!flagsLive)
                continue;
            // state: 0 = no later writer yet, 1..3 = pairs since a later writer
            std::vector<uint8_t> seen(0x800, 0);
            std::vector<std::pair<uint32_t, uint8_t>> stack;
            bool live = exitAfter[p];
            uint32_t next[2];
            for (int i = 0, n = successors(p, next); i < n; ++i)
                stack.push_back({next[i], kNoLaterWriter});
            while (!stack.empty() && !live)
            {
                const auto [x, state] = stack.back();
                stack.pop_back();
                if (!reachable[x] || (seen[x] & (1u << state)))
                    continue;
                seen[x] |= static_cast<uint8_t>(1u << state);
                if (flagReader[x])
                {
                    live = true;
                    break;
                }
                if (exitAfter[x] && state == kNoLaterWriter)
                {
                    live = true;
                    break;
                }
                uint8_t nextState = flagWriter[x] ? 1 : (state == kNoLaterWriter ? kNoLaterWriter : state + 1);
                if (nextState >= 4)
                    continue;
                for (int i = 0, n = successors(x, next); i < n; ++i)
                    stack.push_back({next[i], nextState});
            }
            if (live)
            {
                flagsNeeded[p] = true;
                ++needed;
            }
        }

        std::ostringstream o;
        const std::string fn = "vu1rec_" + hex(hash, 16).substr(2);
        o << "// Generated by VU1Interpreter::generateRecompiled from " << hex(hash, 16) << ".bin. Do not edit.\n"
          << "#include \"runtime/ps2_vu1_rec.h\"\n\n"
          << "namespace\n{\n"
          << "int " << fn << "(VU1Interpreter &I, uint8_t *vuData, uint32_t dataSize, GS &gs, PS2Memory *memory, uint64_t budgetEnd)\n{\n"
          << "    VU1State &S = I.m_state;\n"
          << "    uint64_t instructions = 0;\n"
          << "    uint64_t stallCycles = 0;\n"
          << "    int result = vu1rec::Budget;\n"
          << "    (void)vuData; (void)dataSize; (void)gs; (void)memory;\n"
          << "    for (;;)\n    {\n        switch (S.pc)\n        {\n";

        uint32_t compiled = 0;
        for (uint32_t pc = 0; pc < 0x4000u; pc += 8u)
        {
            if (!reachable[pc / 8u])
                continue;
            ++compiled;
            const DecodedInstructionPair &d = pairs[pc / 8u];
            const std::string in = "            ";
            o << "        case " << hex(pc, 4) << ":\n        {\n";
            o << in << "if (I.m_cycle >= budgetEnd || I.m_stopRequested)\n" << in << "    goto out;\n";
            o << in << "I.commitReadyPipelines();\n";
            if (d.upperUsage.reserved || d.lowerUsage.reserved)
            {
                o << in << "I.reportReservedInstruction(" << (d.upperUsage.reserved ? "true" : "false") << ", "
                  << hex(d.upperUsage.reserved ? d.upper : d.lower) << "u);\n"
                  << in << "goto out;\n        }\n";
                continue;
            }

            // Stall condition.
            std::ostringstream ready;
            {
                std::set<std::pair<int, int>> vfReads;
                uint16_t viReads = 0;
                uint8_t accReads = 0;
                for (const InstructionUsage *usage : {&d.upperUsage, &d.lowerUsage})
                {
                    for (uint32_t k = 0; k < usage->vfReadCount; ++k)
                        for (uint32_t c = 0; c < 4u; ++c)
                            if (usage->vfRead[k].reg != 0u && (usage->vfRead[k].lanes & laneBit(c)))
                                vfReads.insert({usage->vfRead[k].reg, static_cast<int>(c)});
                    viReads |= usage->viRead & 0xFFFEu;
                    accReads |= usage->accRead;
                }
                for (const auto &[reg, c] : vfReads)
                    ready << " r = std::max(r, I.m_vfReady[" << reg << "][" << c << "]);";
                for (uint32_t k = 1; k < 16; ++k)
                    if (viReads & (1u << k))
                        ready << " r = std::max(r, I.m_viReady[" << k << "]);";
                for (uint32_t c = 0; c < 4u; ++c)
                    if (accReads & laneBit(c))
                        ready << " r = std::max(r, I.m_accReady[" << c << "]);";
                const InstructionUsage &l = d.lowerUsage;
                if (l.pipeline == PipelineFdiv || l.waitQ)
                    ready << " if (I.m_fdiv.valid) r = std::max(r, I.m_fdiv.readyCycle);";
                if (l.pipeline == PipelineEfu)
                    ready << " r = std::max(r, I.m_efuResourceReady);";
                if (l.waitP)
                    ready << " for (const auto &e : I.m_efu) if (e.valid) r = std::max(r, e.readyCycle);";
                if (l.pipeline == PipelineXgkick)
                    ready << " if (I.xgkickActive()) r = std::max(r, I.m_cycle + 1u);";
            }
            if (!ready.str().empty())
            {
                o << in << "auto ready = [&]() -> uint64_t { uint64_t r = I.m_cycle;" << ready.str() << " return r; };\n"
                  << in << "if (const uint64_t r = ready(); r > I.m_cycle && !vu1rec::stall(I, r, budgetEnd, stallCycles, ready))\n"
                  << in << "    goto out;\n";
            }
            o << in << "++instructions;\n";

            const uint32_t viWriteBits = d.lowerUsage.viWrite & 0xFFFEu;
            const uint32_t writtenVi = viWriteBits ? static_cast<uint32_t>(__builtin_ctz(viWriteBits)) : 0u;
            const bool recordBranchVi = writtenVi != 0u && d.lowerUsage.delaysNextBranchRead;
            if (recordBranchVi)
                o << in << "const int32_t oldVi = S.vi[" << writtenVi << "];\n";

            // Upper instruction.
            std::ostringstream upper;
            {
                LaneOp op{};
                uint8_t bc = 0;
                const uint8_t dest = DEST(d.upper);
                const uint8_t ft = FT(d.upper), fs = FS(d.upper), fd = FD(d.upper);
                const bool known = describeUpper(d.upper, op, bc) && (!stickyLive || op.minmax || op.expr[0] == '\0');
                if (!known)
                {
                    const std::string convert = upperConvertCode(d.upper, in);
                    if (!convert.empty())
                        upper << convert;
                    else
                        upper << in << "I.execUpper(" << hex(d.upper) << "u);\n";
                }
                else if (op.expr[0] == '\0')
                {
                    upper << in << "I.m_currentUpperInstruction = " << hex(d.upper) << "u; // NOP\n";
                }
                else
                {
                    static const char *lanes = "xyzw";
                    std::set<std::string> uses;
                    std::string expressions[4];
                    for (uint32_t c = 0; c < 4u; ++c)
                        if (dest & laneBit(c))
                            expressions[c] = laneExpression(op, c, uses);
                    upper << in << "{ // " << op.name << "\n";
                    upper << in << "    I.m_currentUpperInstruction = " << hex(d.upper) << "u;\n";
                    for (const std::string &u : uses)
                    {
                        upper << in << "    const float " << u << " = vu1rec::norm(";
                        if (u == "bc")
                            upper << "S.vf[" << int(ft) << "][" << int(bc) << "]";
                        else if (u == "qv")
                            upper << "S.q";
                        else if (u == "iv")
                            upper << "S.i";
                        else
                        {
                            const uint32_t lane = static_cast<uint32_t>(std::strchr(lanes, u[1]) - lanes);
                            if (u[0] == 's')
                                upper << "S.vf[" << int(fs) << "][" << lane << "]";
                            else if (u[0] == 't')
                                upper << "S.vf[" << int(ft) << "][" << lane << "]";
                            else
                                upper << "S.acc[" << lane << "]";
                        }
                        upper << ");\n";
                    }
                    const std::string target = op.toAcc ? "S.acc" : "S.vf[" + std::to_string(fd) + "]";
                    if (op.minmax || !flagsNeeded[pc / 8u])
                    {
                        for (uint32_t c = 0; c < 4u; ++c)
                        {
                            if (!(dest & laneBit(c)))
                                continue;
                            if (op.minmax)
                                upper << in << "    " << target << "[" << c << "] = " << expressions[c] << ";\n";
                            else
                                upper << in << "    " << target << "[" << c << "] = vu1rec::norm(" << expressions[c] << ");\n";
                        }
                    }
                    else
                    {
                        upper << in << "    float res[4] = {";
                        for (uint32_t c = 0; c < 4u; ++c)
                            upper << (c ? ", " : "") << ((dest & laneBit(c)) ? expressions[c] : std::string("0.0f"));
                        upper << "};\n";
                        if (op.toAcc)
                            upper << in << "    I.applyFmacDestAcc(res, " << int(dest) << ");\n";
                        else
                            upper << in << "    I.applyFmacDest(" << target << ", res, " << int(dest) << ");\n";
                    }
                    upper << in << "}\n";
                }
            }

            std::string lower;
            if (d.iBit)
            {
                lower = in + "{ const uint32_t immBits = " + hex(d.lower) + "u; float immediate; std::memcpy(&immediate, &immBits, 4); S.i = vu1rec::norm(immediate); }\n";
            }
            else if (d.lower != 0u && d.lower != 0x8000033Cu)
            {
                lower = lowerCode(d.lower, in);
                if (lower.empty())
                    lower = in + "I.execLower(" + hex(d.lower) + "u, vuData, dataSize, gs, memory, " + hex(d.upper) + "u);\n";
            }

            if (!d.iBit && d.upperVfShadowReg != 0u)
            {
                const std::string reg = "S.vf[" + std::to_string(d.upperVfShadowReg) + "]";
                o << in << "{\n"
                  << in << "float oldVf[4], upperVf[4];\n"
                  << in << "std::memcpy(oldVf, " << reg << ", 16);\n"
                  << upper.str()
                  << in << "std::memcpy(upperVf, " << reg << ", 16);\n"
                  << in << "std::memcpy(" << reg << ", oldVf, 16);\n"
                  << lower
                  << in << "std::memcpy(" << reg << ", upperVf, 16);\n"
                  << in << "}\n";
            }
            else
            {
                o << upper.str() << lower;
            }

            o << in << "I.m_viBranchBackupValid = false;\n";

            // markPairWrites
            {
                const VfAccess lw = d.lowerUsage.vfWrite;
                if (lw.reg != 0u && d.suppressedLowerVf != lw.reg)
                {
                    const uint32_t latency = d.lowerUsage.vfLatency ? d.lowerUsage.vfLatency : d.lowerUsage.latency;
                    for (uint32_t c = 0; c < 4u; ++c)
                        if (lw.lanes & laneBit(c))
                            o << in << "I.m_vfReady[" << int(lw.reg) << "][" << c << "] = I.m_cycle + " << latency << "u;\n";
                }
                const VfAccess uw = d.upperUsage.vfWrite;
                if (uw.reg != 0u)
                {
                    const uint32_t latency = d.upperUsage.vfLatency ? d.upperUsage.vfLatency : d.upperUsage.latency;
                    for (uint32_t c = 0; c < 4u; ++c)
                        if (uw.lanes & laneBit(c))
                            o << in << "I.m_vfReady[" << int(uw.reg) << "][" << c << "] = I.m_cycle + " << latency << "u;\n";
                }
                const uint32_t viLatency = d.lowerUsage.viLatency ? d.lowerUsage.viLatency : d.lowerUsage.latency;
                for (uint32_t k = 1; k < 16; ++k)
                    if (viWriteBits & (1u << k))
                        o << in << "I.m_viReady[" << k << "] = I.m_cycle + " << viLatency << "u;\n";
                for (uint32_t c = 0; c < 4u; ++c)
                    if (d.upperUsage.accWrite & laneBit(c))
                        o << in << "I.m_accReady[" << c << "] = I.m_cycle + " << kAccForwardLatency << "u;\n";
            }
            if (recordBranchVi)
                o << in << "I.recordViWriteForBranch(" << writtenVi << ", oldVi);\n";
            o << in << "vu1rec::resetZeroRegs(S);\n";

            const uint32_t next = (pc + 8u) & 0x3FF8u;
            const bool isBranch = !d.iBit && d.lowerUsage.pipeline == PipelineBranch;
            o << in << "S.pc = " << hex(next, 4) << ";\n";
            const std::string tailCall = std::string("vu1rec::tail(I, ") + (d.dBit ? "true" : "false") + ", " +
                                         (d.tBit ? "true" : "false") + ", " + (d.eBit ? "true" : "false") + ", " +
                                         (isBranch ? "true" : "false") + ")";
            const std::string endTail = in + "{\n" + in + "    if (" + tailCall + ")\n" + in +
                                        "    {\n" + in + "        result = vu1rec::Ended;\n" + in +
                                        "        goto out;\n" + in + "    }\n" + in + "    continue;\n" + in + "}\n";
            if (d.eBit || d.dBit || d.tBit)
            {
                o << endTail;
            }
            else
            {
                o << in << "if (__builtin_expect(S.branchPending | S.haltAfterDelaySlot | S.ebit, 0))\n" << endTail;
                o << in << "I.advanceOneCycle();\n";
                if (next == 0u || !reachable[next / 8u])
                    o << in << "continue;\n";
            }
            o << "        }\n";
            if (pc + 8u < 0x4000u && reachable[(pc + 8u) / 8u])
                o << "            [[fallthrough]];\n";
        }
        o << "        default:\n            result = vu1rec::Bail;\n            goto out;\n        }\n    }\n"
          << "out:\n    vu1rec::flushStats(instructions, stallCycles);\n    return result;\n}\n\n"
          << "const vu1rec::Registrar registrar_" << hex(hash, 16).substr(2) << "(" << hex(hash, 16) << "ull, &" << fn << ");\n}\n";

        const std::string path = std::string(outDir) + "/vu1rec_" + hex(hash, 16).substr(2) + ".cpp";
        std::ofstream out(path);
        out << o.str();
        std::fprintf(stderr, "[vu1rec] %s: %u pairs compiled, %u/%u FMACs keep flags%s\n", path.c_str(), compiled,
                     needed, writers, stickyLive ? " (sticky live)" : "");
    }
    return 0;
}

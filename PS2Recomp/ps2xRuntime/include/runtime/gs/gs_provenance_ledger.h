#pragma once

#include "gs_provenance.h"
#include <array>
#include <cstddef>
#include <utility>
#include <vector>

namespace gs_provenance
{
    // Raw GS state only. Main-target/camera/depth eligibility belongs to the
    // frontend; a known bank is not permission to enable any renderer feature.
    struct Bank
    {
        uint64_t frame=0, zbuf=0, xyoffset=0, scissor=0, test=0, alpha=0;
        uint8_t known=0;
        static constexpr uint8_t Frame=1, Zbuf=2, XYOffset=4, Scissor=8, Test=16, Alpha=32;
        static constexpr uint8_t Depth=Frame|Zbuf|XYOffset|Scissor|Test;
    };
    struct VertexSnapshot
    {
        Source source;
        Bank bank;
        uint64_t ordinal=0, prim=0;
        uint32_t z=0;
        uint8_t context=0;
        bool kick=false, z24=false, known=false;
    };
    struct SubmissionLedger
    {
        Completion completion;
        std::vector<VertexSnapshot> vertices;
        uint32_t gaps=0;
        bool overflow=false;
    };
    enum class Match : uint8_t { Missing, Unique, Ambiguous };

    // Bounded collector for synchronous observations. Completed ledgers own
    // their snapshots: later register writes never change an earlier result.
    // takeCompleted is called on the submission thread before transferring
    // ownership to a frontend/render recording. No game pointers are stored.
    class ConservativeLedger final : public Observer
    {
    public:
        static constexpr std::size_t MaxVertices=8192, MaxCompleted=4;
        void reset(uint64_t epoch) noexcept override
        {
            m_epoch=epoch; invalidate(); m_active=false; m_current={};
            for (auto &slot:m_completed) slot={};
            m_count=0; m_first=0;
        }
        void begin(const Submission &submission) noexcept override
        {
            if (submission.token.epoch!=m_epoch) reset(submission.token.epoch);
            if (m_active) { invalidate(); ++m_dropped; }
            m_current={}; m_current.completion.submission=submission; m_active=true;
            if (!submission.synchronous || !submission.ordered) invalidate();
        }
        void gap(const Source &source, Gap) noexcept override
        {
            // A missing-origin external boundary invalidates current banks.
            // A stale callback from another submission cannot mutate a seal.
            if (source.token.submission && (!m_active || source.token!=m_current.completion.submission.token)) return;
            invalidate();
            if (m_active) ++m_current.gaps;
        }
        void write(const Source &source, uint8_t reg, uint64_t value) noexcept override
        {
            if (!m_active || source.token!=m_current.completion.submission.token) return;
            ++m_ordinal;
            if (!source.valid()) { gap(source,Gap::MissingOrigin); return; }
            switch (reg)
            {
            case 0x00: m_prim=value; m_globals|=1; break;
            case 0x1A: m_prmodecont=value; m_globals|=2; break;
            case 0x1B: m_prmode=value; m_globals|=4; break;
            case 0x4C: case 0x4D: m_banks[reg-0x4C].frame=value; m_banks[reg-0x4C].known|=Bank::Frame; break;
            case 0x4E: case 0x4F: m_banks[reg-0x4E].zbuf=value; m_banks[reg-0x4E].known|=Bank::Zbuf; break;
            case 0x18: case 0x19: m_banks[reg-0x18].xyoffset=value; m_banks[reg-0x18].known|=Bank::XYOffset; break;
            case 0x40: case 0x41: m_banks[reg-0x40].scissor=value; m_banks[reg-0x40].known|=Bank::Scissor; break;
            case 0x47: case 0x48: m_banks[reg-0x47].test=value; m_banks[reg-0x47].known|=Bank::Test; break;
            case 0x42: case 0x43: m_banks[reg-0x42].alpha=value; m_banks[reg-0x42].known|=Bank::Alpha; break;
            case 0x04: case 0x05: case 0x0C: case 0x0D: vertex(source,reg,value); break;
            default: break; // Other registers are available to a custom Observer.
            }
        }
        void end(const Completion &completion) noexcept override
        {
            if (!m_active || completion.submission.token!=m_current.completion.submission.token) return;
            m_current.completion=completion;
            if (!completion.complete || !completion.submission.synchronous || !completion.submission.ordered || m_current.overflow)
            {
                for (auto &v:m_current.vertices) v.known=false;
                invalidate();
            }
            if (m_count==MaxCompleted)
            {
                m_completed[m_first]={}; m_first=(m_first+1)%MaxCompleted; --m_count; ++m_dropped;
            }
            m_completed[(m_first+m_count)%MaxCompleted]=std::move(m_current); ++m_count; m_active=false;
        }
        bool takeCompleted(SubmissionLedger &out) noexcept
        {
            if (!m_count) return false;
            out=std::move(m_completed[m_first]); m_completed[m_first]={};
            m_first=(m_first+1)%MaxCompleted; --m_count; return true;
        }
        uint64_t dropped() const { return m_dropped; }
        static Match findKick(const SubmissionLedger &ledger, Domain domain, uint32_t address, const VertexSnapshot *&out)
        {
            out=nullptr;
            for (const auto &v:ledger.vertices)
                if (v.kick && v.source.domain==domain && v.source.address==address)
                {
                    if (out) { out=nullptr; return Match::Ambiguous; }
                    out=&v;
                }
            return out ? Match::Unique : Match::Missing;
        }
    private:
        void invalidate() noexcept { for(auto &b:m_banks)b.known=0; m_globals=0; }
        void vertex(const Source &source,uint8_t reg,uint64_t value) noexcept
        {
            if (m_current.vertices.size()>=MaxVertices) { m_current.overflow=true; gap(source,Gap::Budget); return; }
            VertexSnapshot v; v.source=source; v.ordinal=m_ordinal;
            v.kick=reg==0x04 || reg==0x05; v.z24=reg==0x04 || reg==0x0C;
            v.z=static_cast<uint32_t>(value>>32); if(v.z24)v.z&=0xFFFFFFu;
            const bool globals=(m_globals&3)==3 && ((m_prmodecont&1) || (m_globals&4));
            v.prim=(m_prmodecont&1) ? m_prim : ((m_prim&7u)|(m_prmode&~uint64_t{7}));
            v.context=(v.prim>>9)&1u; v.bank=m_banks[v.context];
            const auto &s=m_current.completion.submission;
            v.known=globals && (v.bank.known&Bank::Depth)==Bank::Depth && s.synchronous && s.ordered && !m_current.overflow;
            try { m_current.vertices.push_back(v); }
            catch (...) { m_current.overflow=true; invalidate(); }
        }
        Bank m_banks[2];
        uint64_t m_prim=0,m_prmode=0,m_prmodecont=0,m_epoch=0,m_ordinal=0,m_dropped=0;
        uint8_t m_globals=0;
        SubmissionLedger m_current;
        std::array<SubmissionLedger,MaxCompleted> m_completed;
        std::size_t m_count=0,m_first=0;
        bool m_active=false;
    };
}

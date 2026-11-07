#pragma once

#include <cstdint>

// Observation only: never forwards a register to the emulated GS. Callbacks
// run synchronously on the submission thread and must not call guest code,
// change the observer, throw, or keep pointers into DMA/compactor buffers.
namespace gs_provenance
{
    enum class Domain : uint8_t { Unknown, Ram, Scratchpad, Fifo, Vu };
    enum class Gap : uint8_t
    {
        MissingOrigin, FragmentOrigin, UnsupportedGif, UnknownVif, SkippedVu,
        DeferredPathMask, UnobservedPath, UnorderedPaths, Truncated, Budget,
        StateReset
    };
    struct Token
    {
        uint64_t epoch = 0, submission = 0, frame = 0;
        bool operator==(const Token &) const = default;
    };
    struct Source
    {
        Token token;
        uint64_t tagVisit = 0, span = 0;
        uint32_t address = 0;
        Domain domain = Domain::Unknown;
        bool tagData = false, synthetic = false;
        bool valid() const { return token.epoch && token.submission && domain != Domain::Unknown; }
        Source plus(uint32_t bytes) const { Source s = *this; s.address += bytes; return s; }
    };
    struct Submission
    {
        Token token; // frame is vif1Chains BEFORE a VIF1 chain increments it
        uint32_t channel = 0, mode = 0, address = 0;
        bool synchronous = false, ordered = false, frameBoundary = false;
    };
    struct Completion
    {
        Submission submission;
        bool complete = false; // complete scan, not DMA/GPU completion
    };
    struct CompactedSpan
    {
        Source source;
        uint32_t offset = 0, bytes = 0;
    };
    class Observer
    {
    public:
        virtual ~Observer() = default;
        virtual void begin(const Submission &) noexcept {}
        virtual void write(const Source &, uint8_t, uint64_t) noexcept {}
        virtual void gap(const Source &, Gap) noexcept {}
        virtual void compacted(const CompactedSpan &) noexcept {}
        virtual void end(const Completion &) noexcept {}
        virtual void reset(uint64_t) noexcept {}
    };

    // A stack-owned submission. finish() is called before the frame counter
    // increment; unexpected exits report an incomplete scan on the same thread.
    class Session
    {
    public:
        Session(Observer *observer, Submission submission) : m_observer(observer), m_submission(submission) {}
        ~Session() { finish(false); }
        Session(const Session &) = delete;
        Session &operator=(const Session &) = delete;
        void begin(bool synchronous, bool ordered) noexcept
        {
            if (!m_observer || m_begun) return;
            m_begun = true;
            m_submission.synchronous = synchronous;
            m_submission.ordered = ordered;
            m_observer->begin(m_submission);
            if (!synchronous) gap(Gap::UnobservedPath);
            else if (!ordered) gap(Gap::UnorderedPaths);
        }
        Source source(Domain domain, uint32_t address, uint64_t tagVisit = 0, bool tagData = false) noexcept
        {
            Source s; s.token=m_submission.token; s.domain=domain; s.address=address;
            s.tagVisit=tagVisit; s.span=++m_span; s.tagData=tagData; return s;
        }
        void gap(Gap reason) noexcept
        {
            if (m_observer && m_begun) m_observer->gap(source(Domain::Unknown,0),reason);
        }
        void finish(bool complete) noexcept
        {
            if (!m_observer || !m_begun || m_finished) return;
            m_finished=true;
            if (!complete && m_submission.synchronous) gap(Gap::Truncated);
            m_observer->end({m_submission,complete});
        }
    private:
        Observer *m_observer;
        Submission m_submission;
        uint64_t m_span=0;
        bool m_begun=false, m_finished=false;
    };
}

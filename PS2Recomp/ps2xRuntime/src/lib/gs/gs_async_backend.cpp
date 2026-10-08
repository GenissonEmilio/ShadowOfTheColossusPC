#include "runtime/gs/gs_async_backend.h"
#include "ThreadNaming.h"
#include "runtime/ps2_stats.h"

#include <chrono>
#include <cstring>

namespace
{
    // Upper bound on queued commands before the producer waits. A command is
    // ~850 bytes and there are two queues (filling, drained): on the Vita's
    // 160 MB heap 64K of them alone would take ~110 MB.
#if defined(PLATFORM_VITA)
    constexpr size_t kMaxQueuedCommands = 8192u;
#else
    constexpr size_t kMaxQueuedCommands = 65536u;
#endif
}

GSAsyncBackend::GSAsyncBackend(std::unique_ptr<GSRasterBackend> inner)
    : m_inner(std::move(inner))
{
    m_transferShadow.direction = 3u;
    m_worker = std::thread([this]
                           {
                               ThreadNaming::SetCurrentThreadName("GSRaster");
                               workerLoop(); });
}

GSAsyncBackend::~GSAsyncBackend()
{
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_stop = true;
    }
    m_workCv.notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

namespace
{
    thread_local bool t_staging = false;
    thread_local GSAsyncBackend *t_stagedBackend = nullptr;
    constexpr size_t kMaxStaged = 256;
}

void GSAsyncBackend::SetThreadStaging(bool enabled)
{
    if (!enabled)
        PublishThreadStaged();
    t_staging = enabled;
}

void GSAsyncBackend::PublishThreadStaged()
{
    if (GSAsyncBackend *backend = t_stagedBackend)
    {
        t_stagedBackend = nullptr;
        backend->publishStaged();
    }
}

void GSAsyncBackend::publishStaged()
{
    if (m_staged.empty())
        return;
    std::unique_lock<std::mutex> lock(m_queueMutex);
    m_spaceCv.wait(lock, [this]
                   { return m_producing.commands.size() < kMaxQueuedCommands; });
    for (Command &command : m_staged)
        m_producing.commands.push_back(std::move(command));
    m_enqueued += m_staged.size();
    m_staged.clear();
    const bool wake = m_workerSleeping;
    lock.unlock();
    if (wake)
        m_workCv.notify_one();
}

void GSAsyncBackend::enqueue(Command &&command, const uint8_t *data, uint32_t size, bool waitForSpace)
{
    if (t_staging)
    {
        if ((command.kind == Kind::Draw || command.kind == Kind::Clut) && !data && waitForSpace)
        {
            if (t_stagedBackend && t_stagedBackend != this)
                PublishThreadStaged();
            t_stagedBackend = this;
            m_staged.push_back(std::move(command));
            if (m_staged.size() >= kMaxStaged)
                PublishThreadStaged();
            return;
        }
        PublishThreadStaged(); // keep submission order
    }
    std::unique_lock<std::mutex> lock(m_queueMutex);
    if (waitForSpace)
        m_spaceCv.wait(lock, [this]
                       { return m_producing.commands.size() < kMaxQueuedCommands; });
    if (data && size)
    {
        command.args[0] = static_cast<uint32_t>(m_producing.bytes.size());
        command.args[1] = size;
        m_producing.bytes.insert(m_producing.bytes.end(), data, data + size);
    }
    m_producing.commands.push_back(std::move(command));
    ++m_enqueued;
    // Signal only a sleeping worker; a busy one picks the command up when it
    // swaps the queue (a signal per primitive cost a system call each).
    const bool wake = m_workerSleeping;
    lock.unlock();
    if (wake)
        m_workCv.notify_one();
}

void GSAsyncBackend::drain() const
{
    PublishThreadStaged();
    std::unique_lock<std::mutex> lock(m_queueMutex);
    const uint64_t target = m_enqueued;
    m_doneCv.wait(lock, [this, target]
                  { return m_completed >= target; });
}

void GSAsyncBackend::workerLoop()
{
    std::unique_lock<std::mutex> lock(m_queueMutex);
    bool unflushed = false;
    for (;;)
    {
        auto ready = [this]
        { return m_stop || !m_producing.commands.empty(); };
        // Readers of VRAM flush the inner backend themselves, so queued draws
        // are only flushed here once the producer goes quiet: while commands
        // keep coming, they accumulate into larger batches that split better
        // across the raster threads.
        if (!unflushed)
        {
            m_workerSleeping = true;
            m_workCv.wait(lock, ready);
            m_workerSleeping = false;
        }
        else if (m_workerSleeping = true, !m_workCv.wait_for(lock, std::chrono::microseconds(500), ready))
        {
            m_workerSleeping = false;
            lock.unlock();
            m_inner->Flush();
            unflushed = false;
            lock.lock();
            continue;
        }
        m_workerSleeping = false;
        if (m_producing.commands.empty() && m_stop)
            return;
        std::swap(m_producing, m_consuming);
        const uint64_t batchEnd = m_enqueued;
        lock.unlock();
        m_spaceCv.notify_all();

        for (const Command &command : m_consuming.commands)
            execute(command, m_consuming);
        unflushed = true;
        m_consuming.commands.clear();
        m_consuming.bytes.clear();

        lock.lock();
        m_completed = batchEnd;
        m_doneCv.notify_all();
    }
}

void GSAsyncBackend::execute(const Command &command, const Chunk &chunk)
{
    switch (command.kind)
    {
    case Kind::Draw:
        m_inner->Submit(command.batch);
        break;
    case Kind::Clut:
        m_inner->LoadClut(command.tex0, command.texclut);
        break;
    case Kind::Transfer:
        m_inner->BeginTransfer(command.transfer);
        break;
    case Kind::Upload:
        m_inner->UploadImage(chunk.bytes.data() + command.args[0], command.args[1]);
        break;
    case Kind::TextureFlush:
        m_inner->TextureFlush();
        break;
    case Kind::WriteVram:
        m_inner->WriteVram(command.args[0], command.args[1], command.args[2],
                           command.args[3], command.args[4], command.args[5]);
        break;
    case Kind::Clear:
        m_inner->ClearFramebuffer(*command.context, command.args[0]);
        break;
    case Kind::Present:
    {
        PresentationFrame frame = m_inner->Present(*command.present);
        std::lock_guard<std::mutex> lock(m_presentMutex);
        if (frame)
        {
            frame.vsyncTick = command.present->vsyncTick;
            // PS2X_CAPTURE_DUMP=<dir>:<first tick>: save 24 captures as PPM.
            static const char *dump = std::getenv("PS2X_CAPTURE_DUMP");
            static int dumped = 0;
            if (dump && dumped < 24)
            {
                const char *colon = std::strrchr(dump, ':');
                const uint64_t first = colon ? std::strtoull(colon + 1, nullptr, 10) : 0;
                if (frame.vsyncTick >= first)
                {
                    char path[512];
                    std::snprintf(path, sizeof(path), "%.*s/cap_%06llu.ppm", colon ? int(colon - dump) : int(std::strlen(dump)), dump,
                                  (unsigned long long)frame.vsyncTick);
                    if (FILE *f = std::fopen(path, "wb"))
                    {
                        std::fprintf(f, "P6\n%u %u\n255\n", frame.width, frame.height);
                        for (size_t i = 0; i + 3 < frame.pixels.size(); i += 4)
                            std::fwrite(&frame.pixels[i], 1, 3, f);
                        std::fclose(f);
                        ++dumped;
                    }
                }
            }
            m_frames.push_back(std::move(frame));
            if (m_frames.size() > 8u)
                m_frames.pop_front();
            ps2_stats::presentedFrames.fetch_add(1, std::memory_order_relaxed);
            ps2_stats::frameSequence.fetch_add(1, std::memory_order_relaxed);
        }
        --m_presentsQueued;
        break;
    }
    }
}

void GSAsyncBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    drain();
    m_inner->Initialize(vram, vramSize);
    m_hasVram = vram != nullptr;
}

void GSAsyncBackend::Reset()
{
    drain();
    m_inner->Reset();
    m_transferShadow = {};
    m_transferShadow.direction = 3u;
}

void GSAsyncBackend::Submit(const GSPrimitiveBatch &batch)
{
    Command command;
    command.kind = Kind::Draw;
    command.batch = batch;
    enqueue(std::move(command));
}

void GSAsyncBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    Command command;
    command.kind = Kind::Clut;
    command.tex0 = tex0;
    command.texclut = texclut;
    enqueue(std::move(command));
}

void GSAsyncBackend::BeginTransfer(const GSTransferCommand &transfer)
{
    m_transferShadow = {};
    m_transferShadow.x = transfer.trxpos.dsax;
    m_transferShadow.y = transfer.trxpos.dsay;
    m_transferShadow.totalPixels = static_cast<uint32_t>(transfer.trxreg.rrw) * static_cast<uint32_t>(transfer.trxreg.rrh);
    m_transferShadow.direction = transfer.direction;
    Command command;
    command.kind = Kind::Transfer;
    command.transfer = transfer;
    enqueue(std::move(command));
}

void GSAsyncBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    if (!data || sizeBytes == 0u)
        return;
    Command command;
    command.kind = Kind::Upload;
    enqueue(std::move(command), data, sizeBytes);
}

void GSAsyncBackend::Flush()
{
    // The worker flushes the inner backend when the queue goes idle, and
    // every VRAM reader flushes it first.
}

void GSAsyncBackend::TextureFlush()
{
    Command command;
    command.kind = Kind::TextureFlush;
    enqueue(std::move(command));
}

void GSAsyncBackend::Sync(GSSyncReason reason)
{
    // FINISH only signals completion to the EE; nothing reads local memory.
    // Presentation is captured on the raster thread (see Present).
    if (reason == GSSyncReason::Finish || reason == GSSyncReason::Presentation)
        return;
    drain();
    m_inner->Sync(reason);
}

PresentationFrame GSAsyncBackend::Present(const GSPresentationRequest &request)
{
    {
        // The newest capture taken at or before the requested vsync (the
        // window paces frames by their guest vsync).
        std::lock_guard<std::mutex> lock(m_presentMutex);
        if (!m_frames.empty())
        {
            for (auto it = m_frames.rbegin(); it != m_frames.rend(); ++it)
                if (it->vsyncTick <= request.vsyncTick)
                    return *it;
            return m_frames.front();
        }
    }
    // Nothing captured yet (start-up): render one synchronously.
    drain();
    return m_inner->Present(request);
}

void GSAsyncBackend::QueuePresent(const GSPresentationRequest &request)
{
    {
        std::lock_guard<std::mutex> lock(m_presentMutex);
        if (m_presentsQueued >= 8)
            return; // the raster thread is far behind; skip this frame
        ++m_presentsQueued;
    }
    Command command;
    command.kind = Kind::Present;
    command.present = std::make_shared<GSPresentationRequest>(request);
    enqueue(std::move(command), nullptr, 0, false); // never block the guest on presentation
}

bool GSAsyncBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    if (!m_hasVram || context.frame.fbw == 0u)
        return false;
    Command command;
    command.kind = Kind::Clear;
    command.context = std::make_shared<GSContext>(context);
    command.args[0] = rgba;
    enqueue(std::move(command));
    return true;
}

uint32_t GSAsyncBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    drain();
    return m_inner->ConsumeLocalToHostBytes(dst, maxBytes);
}

uint32_t GSAsyncBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    drain();
    return m_inner->ReadVram(psm, base, bw, x, y);
}

void GSAsyncBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    Command command;
    command.kind = Kind::WriteVram;
    command.args[0] = psm;
    command.args[1] = base;
    command.args[2] = bw;
    command.args[3] = x;
    command.args[4] = y;
    command.args[5] = value;
    enqueue(std::move(command));
}

void GSAsyncBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    drain();
    m_inner->SnapshotVram(out);
}

GSTransferSnapshot GSAsyncBackend::GetTransferSnapshot() const
{
    // Only used for debug views; avoid waiting on the raster thread.
    return m_transferShadow;
}

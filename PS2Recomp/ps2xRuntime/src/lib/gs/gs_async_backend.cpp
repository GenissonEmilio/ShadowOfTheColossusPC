#include "runtime/gs/gs_async_backend.h"
#include "ThreadNaming.h"

#include <cstring>

namespace
{
    // Upper bound on queued commands before the producer waits.
    constexpr size_t kMaxQueuedCommands = 65536u;
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

void GSAsyncBackend::enqueue(Command &&command, const uint8_t *data, uint32_t size)
{
    std::unique_lock<std::mutex> lock(m_queueMutex);
    m_spaceCv.wait(lock, [this]
                   { return m_producing.commands.size() < kMaxQueuedCommands; });
    if (data && size)
    {
        command.args[0] = static_cast<uint32_t>(m_producing.bytes.size());
        command.args[1] = size;
        m_producing.bytes.insert(m_producing.bytes.end(), data, data + size);
    }
    const bool wasEmpty = m_producing.commands.empty();
    m_producing.commands.push_back(std::move(command));
    ++m_enqueued;
    lock.unlock();
    if (wasEmpty)
        m_workCv.notify_one();
}

void GSAsyncBackend::drain() const
{
    std::unique_lock<std::mutex> lock(m_queueMutex);
    const uint64_t target = m_enqueued;
    m_doneCv.wait(lock, [this, target]
                  { return m_completed >= target; });
}

void GSAsyncBackend::workerLoop()
{
    std::unique_lock<std::mutex> lock(m_queueMutex);
    for (;;)
    {
        m_workCv.wait(lock, [this]
                      { return m_stop || !m_producing.commands.empty(); });
        if (m_producing.commands.empty() && m_stop)
            return;
        std::swap(m_producing, m_consuming);
        const uint64_t batchEnd = m_enqueued;
        lock.unlock();
        m_spaceCv.notify_all();

        for (const Command &command : m_consuming.commands)
            execute(command, m_consuming);
        m_inner->Flush();
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
    // The worker flushes the inner backend after every chunk.
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
    if (reason == GSSyncReason::Finish)
        return;
    drain();
    m_inner->Sync(reason);
}

PresentationFrame GSAsyncBackend::Present(const GSPresentationRequest &request)
{
    drain();
    return m_inner->Present(request);
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

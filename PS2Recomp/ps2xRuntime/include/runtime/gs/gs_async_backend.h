#pragma once

#include "runtime/gs/gs_backend.h"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// Runs another raster backend on its own thread. Commands that only change
// GS local memory (draws, uploads, CLUT loads, transfers) are queued in order;
// anything that reads local memory back waits until the queue has drained.
class GSAsyncBackend final : public GSRasterBackend
{
public:
    explicit GSAsyncBackend(std::unique_ptr<GSRasterBackend> inner);
    ~GSAsyncBackend() override;

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override;
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override;
    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;

    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    PresentationFrame Present(const GSPresentationRequest &request) override;
    void QueuePresent(const GSPresentationRequest &request) override;

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

private:
    enum class Kind : uint8_t
    {
        Draw,
        Clut,
        Transfer,
        Upload,
        TextureFlush,
        WriteVram,
        Clear,
        Present,
    };

    struct Command
    {
        Kind kind = Kind::Draw;
        GSPrimitiveBatch batch{};
        GSTex0Reg tex0{};
        GSTexClutReg texclut{};
        GSTransferCommand transfer{};
        uint32_t args[6]{};
        std::shared_ptr<GSContext> context;
        std::shared_ptr<GSPresentationRequest> present;
    };

    struct Chunk
    {
        std::vector<Command> commands;
        std::vector<uint8_t> bytes;
    };

    void enqueue(Command &&command, const uint8_t *data = nullptr, uint32_t size = 0, bool waitForSpace = true);
    void drain() const;
    void workerLoop();
    void execute(const Command &command, const Chunk &chunk);

    // Presentation never waits for the queue: the raster thread captures the
    // display when it reaches the point a frame was requested, and the host
    // shows the most recent capture.
    std::mutex m_presentMutex;
    PresentationFrame m_latestFrame;
    bool m_hasLatestFrame = false;
    int m_presentsQueued = 0; // captures requested and not yet taken (bounded)

    std::unique_ptr<GSRasterBackend> m_inner;
    std::thread m_worker;
    mutable std::mutex m_queueMutex;
    mutable std::condition_variable m_workCv;
    mutable std::condition_variable m_doneCv;
    mutable std::condition_variable m_spaceCv;
    Chunk m_producing;
    Chunk m_consuming;
    uint64_t m_enqueued = 0;
    uint64_t m_completed = 0;
    bool m_stop = false;
    bool m_hasVram = false;
    GSTransferSnapshot m_transferShadow{};
};

#pragma once

#include "runtime/gs/gs_types.h"

#include <cstdint>
#include <vector>

class GSRasterBackend
{
public:
    virtual ~GSRasterBackend() = default;

    virtual void Initialize(uint8_t *vram, uint32_t vramSize) = 0;
    virtual void Reset() = 0;

    virtual void Submit(const GSPrimitiveBatch &batch) = 0;
    // VU1 batches drawn on the GPU (GSVuBatch). Backends without a vertex
    // stage say no, and the caller draws the CPU way.
    virtual bool SupportsVuBatch() const { return false; }
    virtual void SubmitVuBatch(const GSVuBatch &) {}
    virtual void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) = 0;

    virtual void BeginTransfer(const GSTransferCommand &command) = 0;
    virtual void UploadImage(const uint8_t *data, uint32_t sizeBytes) = 0;

    virtual void Flush() = 0;
    virtual void TextureFlush() = 0;
    virtual void Sync(GSSyncReason reason) = 0;
    virtual PresentationFrame Present(const GSPresentationRequest &request) = 0;
    // Marks the point of the command stream where the game showed a frame
    // (guest vsync). Asynchronous backends capture the display there and
    // Present() returns the latest capture.
    virtual void QueuePresent(const GSPresentationRequest &) {}

    virtual bool ClearFramebuffer(const GSContext &context, uint32_t rgba) = 0;
    virtual uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) = 0;

    virtual uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const = 0;
    virtual void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) = 0;
    virtual void SnapshotVram(std::vector<uint8_t> &out) const = 0;
    virtual GSTransferSnapshot GetTransferSnapshot() const = 0;
};

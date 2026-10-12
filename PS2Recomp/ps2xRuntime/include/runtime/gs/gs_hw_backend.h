#pragma once

// Hardware GS renderer: draws GS primitives with the host GPU through an
// OpenGL subset that also maps onto vitaGL (PS Vita). GS frame and Z buffers
// become GPU render targets; textures are decoded from GS memory, which a
// GSCpuBackend keeps (uploads, CLUTs, transfers) without rasterizing. GPU
// results are written back to GS memory only when the CPU needs them
// (presentation, reads, textures from memory the GPU drew).
//
// PS2X_GS_HW=1 selects it (macOS for now).

#include "runtime/gs/gs_backend.h"

#include <functional>
#include <memory>

class GSCpuBackend;

class GSHwBackend final : public GSRasterBackend
{
public:
    // The host provides a GL context for the thread that drives this backend
    // (the GS worker): makeCurrent() is called there before the first GL call.
    static void setContextProvider(std::function<void()> makeCurrent);
    static bool available();
    // Vita: the GS thread puts each new frame on screen itself (one GL for
    // every thread; the host would otherwise hold it through every vsync).
    // Called with the frame's GL texture right after it is ready.
    static void setPresentHook(std::function<void(uint32_t texture, uint32_t width, uint32_t height)> hook);

    GSHwBackend();
    ~GSHwBackend() override;

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;
    void Submit(const GSPrimitiveBatch &batch) override;
    bool SupportsVuBatch() const override;
    void SubmitVuBatch(const GSVuBatch &batch) override;
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override;
    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;
    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    PresentationFrame Present(const GSPresentationRequest &request) override;
    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;
    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

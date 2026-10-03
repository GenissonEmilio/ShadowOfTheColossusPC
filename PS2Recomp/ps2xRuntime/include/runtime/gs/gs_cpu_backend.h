#pragma once
#include <functional>

#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_texture_page_cache.h"

#include <array>
#include <mutex>
#include <unordered_map>
#include <vector>

class GSCpuBackend final : public GSRasterBackend
{
public:
    GSCpuBackend();

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

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

private:
    void ResetUnlocked();
    void LoadClutUnlocked(const GSTex0Reg &tex0, const GSTexClutReg &texclut);
    uint32_t ReadVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const;
    uint32_t ReadTextureVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y);
    void WriteVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value);

    void DrawPrimitive(const GSPrimitiveBatch &batch);
    void DrawSprite(const GSPrimitiveBatch &batch);
    void DrawTriangle(const GSPrimitiveBatch &batch);
    void DrawLine(const GSPrimitiveBatch &batch);
    void WritePixel(const GSDrawState &state, int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t fog);
    uint32_t SampleTexture(const GSDrawState &state, float s, float t, float q, uint16_t u, uint16_t v);
    uint32_t LookupCLUT(const GSDrawState &state, uint8_t index, uint8_t cpsm, uint8_t csm, uint8_t csa, uint8_t sourcePsm);

    void PerformLocalToLocalTransfer();
    void PerformLocalToHostTransfer();
    PresentationFrame PresentFromLocalMemory(const GSPresentationRequest &request);
    bool CopyFrameToHostRgba(const GSFrameReg &frame,
                             uint32_t width,
                             uint32_t height,
                             std::vector<uint8_t> &outPixels,
                             bool preserveAlpha,
                             bool useLocalMemoryLayout,
                             bool frameBaseIsPages,
                             uint32_t sourceOriginX,
                             uint32_t sourceOriginY) const;

    using WriteVramFunc = void (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    using ReadVramFunc = uint32_t (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t);

    static constexpr size_t kPsmHandlerCount = 1u << 6u;
    mutable std::mutex m_mutex;
    uint8_t *m_vram = nullptr;
    uint32_t m_vramSize = 0;
    std::array<ReadVramFunc, kPsmHandlerCount> m_readVramFuncs{};
    std::array<WriteVramFunc, kPsmHandlerCount> m_writeVramFuncs{};
    std::array<uint16_t, 512> m_clut{};
    std::array<uint32_t, 2> m_clutCbp{};
    GSMem::TexturePageCache m_texturePageCache;
    bool m_textureDirectRead = false;
    std::vector<GSPrimitiveBatch> m_pending;
    // CLUT contents each queued draw was submitted with: loading a new CLUT
    // starts a new version instead of flushing the queue.
    std::vector<std::array<uint16_t, 512>> m_clutVersions;
    std::vector<uint64_t> m_clutVersionHash;
    std::vector<uint32_t> m_pendingClut;
    bool m_clutChanged = true;
    uint64_t m_clutHash = 0; // content hash of m_clut

    // Decoded-texture cache: textures sampled by queued draws are converted to
    // linear RGBA once and reused while none of their VRAM pages is written.
    struct TextureKey
    {
        uint32_t tbp0 = 0, tbw = 0, psm = 0, tw = 0, th = 0;
        uint32_t cpsm = 0, csm = 0, csa = 0, texa = 0;
        uint64_t clutHash = 0;
        bool operator==(const TextureKey &) const = default;
    };
    struct TextureKeyHash
    {
        size_t operator()(const TextureKey &key) const noexcept;
    };
    struct DecodedTexture
    {
        std::vector<uint32_t> texels;
        uint32_t width = 0, height = 0;
        uint64_t epoch = 0;
    };
    std::unordered_map<TextureKey, DecodedTexture, TextureKeyHash> m_textureCache;
    size_t m_textureCacheBytes = 0;
    std::array<uint64_t, 512> m_pageEpoch{}; // last write to each 8 KiB VRAM page
    uint64_t m_epochCounter = 1;
    std::vector<const DecodedTexture *> m_pendingTexture;
    std::vector<uint8_t> m_feedbackVram; // pre-draw texture copy for feedback draws
    const DecodedTexture *decodedTexture(const GSDrawState &state, const uint16_t *clut, uint64_t clutHash);
    void markVramWritten(uint64_t begin, uint64_t end);
    std::pair<uint64_t, uint64_t> m_uploadRange{0u, 0u}; // destination of the host->local transfer
    // True when queued draws read or write VRAM bytes in [begin, end).
    bool pendingTouches(uint64_t begin, uint64_t end) const;
    bool uploadTouchesPending(const GSTransferCommand &command) const;
    std::vector<std::vector<uint32_t>> m_bandBins;
    // VRAM byte ranges written by queued draws, and the ranges of the draw
    // being submitted (set by UpdateTextureReadMode).
    std::vector<std::pair<uint64_t, uint64_t>> m_pendingWrites;
    std::vector<std::pair<uint64_t, uint64_t>> m_pendingReads;
    std::pair<uint64_t, uint64_t> m_lastTexRange{0u, 0u};
    std::pair<uint64_t, uint64_t> m_lastFrameRange{0u, 0u};
    std::pair<uint64_t, uint64_t> m_lastZRange{0u, 0u};
    void flushPendingUnlocked(const char *reason = "other");
    void UpdateTextureReadMode(const GSPrimitiveBatch &batch);
    void drawRowsParallel(int y0, int y1, int rowWidth, const std::function<void(int)> &drawRow);

    GSTransferCommand m_transfer{};
    GSTransferSnapshot m_transferState{};
    std::vector<uint8_t> m_localToHostBuffer;
    size_t m_localToHostReadPos = 0;
};

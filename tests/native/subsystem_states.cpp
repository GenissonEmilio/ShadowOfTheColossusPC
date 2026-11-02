// ROM-free restoration, malformed-input and unsupported-mode checks.
#include "runtime/ps2_memory.h"
#include "ps2_runtime.h"
#include "runtime/ps2_vu1.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/native_renderer.h"
#include "runtime/host_state.h"
#include "Kernel/Stubs/MemoryCard.h"
#include "native/native_history_state.h"
#include "emulator/core/iop_memory.h"
#include "iop_module_manager.h"
#include "../../PS2Recomp/ps2xIOP/tests/iop_compat_test_support.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace {
    unsigned checks = 0;
    void check(bool ok, const char *what) {
        ++checks;
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::fflush(nullptr); std::_Exit(1); }
    }
    void check(bool ok, const std::string &what) { check(ok, what.c_str()); }
    void memoryState() {
        PS2Memory memory;
        check(memory.initialize(), "initialize memory");
        auto *ram = memory.getRDRAM(); auto *scratch = memory.m_scratchpad;
        memory.write32(0x12340, 0x87654321); memory.write32(0x70000010, 0xabcdef01);
        memory.vif1_regs.row[2] = 42; memory.dma_regs[2].qwc = 13;
        memory.m_eeTimers[1].compareBacklog = 7; memory.m_timerDeferred = 91;
        memory.m_tlbEntries.push_back({0x1000, 0x2000, 0xfff, true});
        memory.m_path3MaskedFifo.push_back(std::vector<uint8_t>(16, 0xa5));
        memory.m_pendingGifTransfers.push_back({false, 0x12000, 1, std::vector<uint8_t>(16, 0x44)});
        unsigned callbacks = 0;
        memory.setGsEventWriter([&](uint8_t, uint64_t) { ++callbacks; });
        std::vector<uint8_t> state; std::string error;
        check(memory.captureState(state, error), error);
        memory.write32(0x12340, 0); memory.vif1_regs.row[2] = 0;
        memory.m_eeTimers[1].compareBacklog = 0; memory.m_path3MaskedFifo.clear();
        check(memory.restoreState(state, error), error);
        check(memory.getRDRAM() == ram && memory.m_scratchpad == scratch, "EE storage addresses preserved");
        check(memory.read32(0x12340) == 0x87654321 && memory.read32(0x70000010) == 0xabcdef01, "RAM and scratch restored");
        check(memory.vif1_regs.row[2] == 42 && memory.dma_regs[2].qwc == 13, "VIF and DMA restored");
        check(memory.m_eeTimers[1].compareBacklog == 7 && memory.m_timerDeferred == 91, "timer backlog restored");
        check(memory.m_tlbEntries.back().valid && memory.m_tlbEntries.back().pfn == 0x2000, "TLB restored");
        check(memory.m_path3MaskedFifo[0][0] == 0xa5 && memory.m_pendingGifTransfers[0].chainData[0] == 0x44, "pending transfer bytes restored");
        memory.m_gsEventWrite(0, 0); check(callbacks == 1, "callback preserved, not serialized");
        memory.write32(0x12340, 99);
        state.pop_back(); check(!memory.restoreState(state, error), "truncated memory rejected");
        check(memory.read32(0x12340) == 99, "invalid memory archive leaves live RAM unchanged");
    }
    void vuState() {
        auto source = std::make_unique<VU1Interpreter>();
        source->state().q = 2.5f; source->state().pc = 8;
        source->m_fdiv.valid = true; source->m_fdiv.readyCycle = 23; source->m_fdiv.value = 3.5f;
        source->m_vfWritePipeline[0].valid = true; source->m_vfWritePipeline[0].reg = 5;
        source->m_vfWritePipeline[0].laneMask = 15;
        source->m_xgkick.active = true; source->m_xgkick.copiedBytes = 16;
        source->m_xgkick.currentTagEnd = 64; source->m_xgkick.totalBytes = 0;
        source->m_xgkick.packet[3] = 0x88; // EOP not yet encountered: totalBytes is legitimately zero.
        std::vector<uint8_t> state; std::string error;
        check(source->captureState(state, error), error);
        auto restored = std::make_unique<VU1Interpreter>();
        check(restored->restoreState(state, error), error);
        check(restored->state().q == 2.5f && restored->state().pc == 8, "VU scalar/PC restored");
        check(restored->m_fdiv.valid && restored->m_fdiv.readyCycle == 23 && restored->m_fdiv.value == 3.5f, "pending division restored");
        check(restored->m_vfWritePipeline[0].valid && restored->m_vfWritePipeline[0].reg == 5, "pending VF write restored");
        check(restored->m_xgkick.active && restored->m_xgkick.packet[3] == 0x88 && !restored->m_xgkick.totalBytes, "partial XGKICK restored before EOP");
        VU1Interpreter wrongUnit(VU1Interpreter::Unit::VU0);
        check(!wrongUnit.validateState(state, error), "VU0 rejects VU1 archive");
        state.pop_back(); restored->state().q = 77;
        check(!restored->restoreState(state, error) && restored->state().q == 77, "truncated VU leaves live context intact");
    }
    void iopState() {
        using namespace ps2x::iop::detail;
        IopMemory memory;
        const auto block = memory.allocate(33); const auto *ram = memory.ram().data();
        memory.write32(block, 0x11223344); memory.write32(IopMemory::ScratchBase, 0x55667788);
        memory.setInterruptMask(0x123);
        std::vector<uint8_t> state; std::string error;
        check(memory.captureState(state, error), error);
        memory.write32(block, 0); check(memory.freeAllocation(block), "free future IOP allocation");
        check(memory.restoreState(state, error), error);
        check(memory.ram().data() == ram && memory.read32(block) == 0x11223344, "IOP RAM value and address restored");
        check(memory.read32(IopMemory::ScratchBase) == 0x55667788 && memory.interruptMask() == 0x123, "IOP scratch and interrupt mask restored");
        check(memory.allocationContaining(block).has_value(), "IOP ownership/allocator restored");
        state.pop_back(); memory.write32(block, 7);
        check(!memory.restoreState(state, error) && memory.read32(block) == 7, "truncated IOP memory leaves bytes intact");

        IopModuleManager modules;
        const auto id = modules.loadHle("rom0:LIBSD").moduleId;
        check(modules.loadHle("rom0:LIBSD").moduleId == id, "second HLE reference shares the module ID");
        check(modules.captureState(state, error), error);
        modules.reset(); check(modules.restoreState(state, error), error);
        check(modules.stopHle(id, nullptr), "first restored module reference stops");
        const std::string_view aliases[] = {"libsd"};
        check(modules.isLoaded(aliases), "second restored module reference remains");
        check(modules.stopHle(id, nullptr) && !modules.isLoaded(aliases), "last restored module reference deactivates");

        iop_test::Host host;
        ps2x::iop::IopSubsystem source(host);
        check(!source.captureState(state, error), "uninitialized SG2 state explicitly rejected");
        std::vector<uint8_t> synthetic(128, 0);
        std::memcpy(synthetic.data(), "\x7f" "ELF", 4);
        const uint32_t end = 128; const uint16_t stride = 1, count = 0;
        std::memcpy(synthetic.data() + 0x20, &end, 4); std::memcpy(synthetic.data() + 0x2e, &stride, 2);
        std::memcpy(synthetic.data() + 0x30, &count, 2); std::memcpy(synthetic.data() + 0x40, "sg2iop_driver", 13);
        check(source.writeMemory(0x1000, synthetic.data(), synthetic.size()), "synthetic SG2 identification buffer written");
        check(source.loadModuleBuffer(0x1000).moduleId > 0, "native driver selected without proprietary IRX");
        check(source.loadModule("rom0:LIBSD").moduleId > 0, "HLE service initialized");
        source.runEeCycles(123456);
        check(source.captureState(state, error), error);
        ps2x::iop::IopSubsystem destination(host);
        check(destination.restoreState(state, error), error);
        check(destination.debugSnapshot().emulatorCycles == source.debugSnapshot().emulatorCycles, "native IOP clock restored across owners");
        check(destination.canBindRpc(0x80000701u), "HLE routes reconstructed from factories");
        check(destination.readMemory(0x1040, synthetic.data(), 13) && std::memcmp(synthetic.data(), "sg2iop_driver", 13) == 0, "IOP RAM preserved with SG2 state");
    }
    void gsState() {
        PS2Memory memory; check(memory.initialize(), "GS backing allocation");
        GS gs; gs.init(memory.getGSVRAM(), PS2_GS_VRAM_SIZE, &memory.gs_regs);
        native_renderer::setShown(true);
        memory.getGSVRAM()[17] = 0x55; gs.writeRegister(GS_REG_FRAME_1, 3u | (2ull << 16u));
        std::vector<uint8_t> state; std::string error;
        check(gs.captureState(state, error), error);
        memory.getGSVRAM()[17] = 0; gs.writeRegister(GS_REG_FRAME_1, 0);
        check(gs.restoreState(state, error), error);
        check(memory.getGSVRAM()[17] == 0x55 && gs.getDebugSnapshot().ctx[0].frame.fbp == 3, "GS registers and VRAM restored");
        state.pop_back(); check(!gs.restoreState(state, error), "truncated GS rejected");
        check(memory.getGSVRAM()[17] == 0x55, "invalid GS state does not mutate VRAM");
        native_renderer::setShown(false);
        gs.writeRegister(GS_REG_BITBLTBUF, 1ull << 48u); gs.writeRegister(GS_REG_TRXREG, 4u | (4ull << 32u));
        gs.writeRegister(GS_REG_TRXDIR, 0);
        native_renderer::setShown(true);
        check(!gs.prepareStateCapture(error), "partial GS image upload rejected");
        native_renderer::setShown(false);
        for (int i = 0; i < 8; ++i) gs.writeRegister(GS_REG_HWREG, 0x1122334455667788ull);
        native_renderer::setShown(true);
        check(gs.prepareStateCapture(error), "completed async image upload accepted at blocking barrier");
    }
    void historyCodec() {
        using namespace native_renderer::detail;
        HistoryState state; state.width = 8; state.height = 4; state.bloom = state.gsBloom = true;
        state.previous.resize(128, 0x22); state.bloomPixels.resize(8, 0x33); state.gsBloomPixels.resize(16384, 0x44);
        std::vector<uint8_t> blob; std::string error;
        ps2state::Writer writer(blob, error, HistoryStateLimit);
        check(historyStateCodec(writer, state), error);
        HistoryState decoded; check(decodeHistoryState(blob, decoded, error), error);
        check(decoded.previous == state.previous && decoded.bloomPixels == state.bloomPixels && decoded.gsBloomPixels == state.gsBloomPixels, "all temporal buffers round trip");
        blob[20] = 2; check(!decodeHistoryState(blob, decoded, error), "noncanonical history boolean rejected");
        blob[20] = 1; blob.pop_back(); check(!decodeHistoryState(blob, decoded, error), "truncated temporal pixels rejected");
        state.width = 8192; state.height = 8192;
        ps2state::Writer oversized(blob, error, HistoryStateLimit);
        check(!historyStateCodec(oversized, state), "history dimension budget checked before pixel allocation");
    }
    void legacyCardGuard() {
        std::string error;
        R5900Context context{};
        check(ps2_stubs::legacyCardStateCaptureSupported(error), "unused legacy filesystem card is supported");
        // No filesystem operation: an unknown fd still records a guest-visible
        // async command and result. An open-files-only guard would miss it.
        context.r[4] = _mm_set_epi64x(0, 12345);
        ps2_stubs::sceMcClose(nullptr, &context, nullptr);
        check(!ps2_stubs::legacyCardStateCaptureSupported(error), "legacy command metadata blocks states with no open FILE handles");
        ps2_stubs::sceMcEnd(nullptr, &context, nullptr);
        check(ps2_stubs::legacyCardStateCaptureSupported(error), "real libmc teardown leaves initial metadata");
        context.r[5] = _mm_set_epi64x(0, 3);
        ps2_stubs::mcSetFileSelectWindowCursol(nullptr, &context, nullptr);
        check(!ps2_stubs::legacyCardStateCaptureSupported(error), "legacy UI cursor blocks states without any card fd");
        ps2_stubs::mcSetFileSelectWindowCursolInit(nullptr, &context, nullptr);
        check(ps2_stubs::legacyCardStateCaptureSupported(error), "fixture restores its own legacy cursor");
    }
}
int main() {
    setenv("SOTC_NATIVE", "1", 1); setenv("PS2X_NATIVE_IOP_BOOT", "1", 1); setenv("PS2X_NATIVE_SG2", "1", 1);
    setenv("PS2X_MTVU", "0", 1); setenv("PS2X_GS_HW", "0", 1); setenv("PS2X_GS_NODRAW", "1", 1);
    memoryState(); vuState(); iopState(); gsState(); historyCodec(); legacyCardGuard();
    std::printf("PASS: %u subsystem state checks; no ROM, GPU context or personal files\n", checks);
}

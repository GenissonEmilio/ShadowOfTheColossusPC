#include "MiniTest.h"
#include "Kernel/Syscalls/System.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"

#include <array>
#include <memory>

namespace
{
    constexpr uint32_t kGuestEntry = 0x300000u;
    constexpr uint32_t kSampleCycles = 1024u;
    std::array<uint64_t, 3> fieldCycles;

    void sampleVideoFields(uint8_t *, R5900Context *, PS2Runtime *runtime)
    {
        auto &scheduler = runtime->eeScheduler();
        const uint64_t tick = scheduler.currentVSyncTick();
        if (tick > 0 && tick <= fieldCycles.size() && fieldCycles[tick - 1] == 0)
            fieldCycles[tick - 1] = scheduler.eeCycle();
        if (tick >= fieldCycles.size())
            runtime->requestStop();
        else
            scheduler.accountCycles(kSampleCycles);
    }
}

void register_pal_video_timing_tests()
{
    MiniTest::Case("EEVideoTiming", [](TestCase &suite)
    {
        suite.Run("European CRT mode selects the field cadence", [](TestCase &test)
        {
            const auto previousPaths = PS2Runtime::getIoPaths();
            struct Mode
            {
                const char *boot;
                uint32_t setting;
                uint64_t fieldMicroseconds;
            };
            const std::array modes{
                Mode{"SCES_533.26", 0x03u, 20000u},
                Mode{"SCES_533.26", 0x02u, 16667u},
                Mode{"SCUS_974.72", 0x03u, 16667u},
            };
            for (const auto &mode : modes)
            {
                std::vector<uint8_t> ram(PS2_RAM_SIZE);
                auto runtime = std::make_unique<PS2Runtime>();
                PS2Runtime::IoPaths paths;
                paths.elfPath = mode.boot;
                PS2Runtime::setIoPaths(paths);
                R5900Context context;
                context.pc = kGuestEntry;
                runtime->eeScheduler().reset(ram.data(), context);
                // The European menu can switch from its initial PAL mode to NTSC.
                SET_GPR_U32(&context, 5, 0x03u);
                ps2_syscalls::SetGsCrt(ram.data(), &context, runtime.get());
                SET_GPR_U32(&context, 5, mode.setting);
                ps2_syscalls::SetGsCrt(ram.data(), &context, runtime.get());
                runtime->registerFunction(kGuestEntry, sampleVideoFields);
                fieldCycles.fill(0);
                runtime->eeScheduler().run();

                const uint64_t expected = (mode.fieldMicroseconds * EeScheduler::kEeClockHz + 999999u) / 1000000u;
                const uint64_t measured = fieldCycles[2] - fieldCycles[1];
                test.IsTrue(measured + 2u * kSampleCycles >= expected && measured <= expected + 2u * kSampleCycles,
                            std::string(mode.boot) + " field cadence at mode " + std::to_string(mode.setting));
            }
            PS2Runtime::setIoPaths(previousPaths);
        });
    });
}

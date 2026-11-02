#include "MiniTest.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "Kernel/Syscalls/Common.h"
#include "Kernel/Syscalls/HostState.h"

#include <array>
#include <thread>

namespace
{
    struct StateTestEnv
    {
        std::vector<uint8_t> ram = std::vector<uint8_t>(PS2_RAM_SIZE);
        PS2Runtime runtime;
        R5900Context context;
        StateTestEnv()
        {
            context.pc = 0x300000u;
            runtime.eeScheduler().reset(ram.data(), context);
        }
    };

    std::vector<int> stateTrace;
    void stateMain(uint8_t *, R5900Context *, PS2Runtime *runtime)
    {
        stateTrace.push_back(1);
        runtime->eeScheduler().exitCurrent(false);
    }
    void stateHigh(uint8_t *, R5900Context *, PS2Runtime *runtime)
    {
        stateTrace.push_back(2);
        runtime->eeScheduler().exitCurrent(false);
    }
    void stateLow(uint8_t *, R5900Context *, PS2Runtime *runtime)
    {
        stateTrace.push_back(3);
        runtime->requestStop();
    }
}

void register_ee_save_state_tests()
{
    MiniTest::Case("EEState", [](TestCase &suite)
    {
        suite.Run("restores contexts and object waits into a different runtime", [](TestCase &test)
        {
            StateTestEnv source;
            EeScheduler &scheduler = source.runtime.eeScheduler();
            scheduler.bindMainContextForSyscall(source.context, source.ram.data());
            const int semaphore = scheduler.createSemaphore(0, 3, 0x1122u, 0x3344u);
            const int flag = scheduler.createEventFlag(0x50u, 0x66u, 0x77u);
            const int child = scheduler.createThread(EeThreadCreateParams{0u, 0x300010u, 0x180000u, 0x4000u, 0x1234u, 9, 0x55u});
            scheduler.startThread(child, 0xABCDEFu, source.context, false);
            scheduler.thread(child)->context.r[8] = _mm_set_epi64x(0x1122334455667788ll, 0x7766554433221100ll);
            scheduler.thread(child)->context.vu0_vf[17] = _mm_set_ps(4.0f, 3.0f, 2.0f, 1.0f);
            scheduler.thread(child)->context.f[19] = -123.5f;
            scheduler.thread(child)->context.llbit = 1;
            scheduler.thread(child)->context.lladdr = 0x12340u;
            try { scheduler.waitSemaphore(semaphore); }
            catch (const EeDispatcherTransfer &) {}
            scheduler.accountCycles(1234u);
            std::vector<uint8_t> state;
            std::string error;
            test.IsTrue(scheduler.captureState(state, error), "capture should preserve a kernel-object wait: " + error);
            if (state.empty()) return;
            StateTestEnv destination;
            EeScheduler &restored = destination.runtime.eeScheduler();
            test.IsTrue(restored.restoreState(state, error), "archive must not depend on the source runtime pointers: " + error);
            test.Equals(restored.eeCycle(), uint64_t{1234}, "EE cycle accounting is restored");
            test.Equals(restored.currentThreadId(), 0, "the blocked main thread must not become running");
            test.Equals(restored.thread(1)->status, EeThreadStatus::Waiting, "main remains blocked");
            test.Equals(restored.thread(1)->wait.reason, EeWaitReason::Semaphore, "wait kind is preserved");
            test.Equals(restored.semaphore(semaphore)->waiters.front(), 1, "semaphore queue points to the restored main thread");
            test.Equals(restored.eventFlag(flag)->bits, 0x50u, "event flags are restored");
            test.Equals(static_cast<uint64_t>(_mm_extract_epi64(restored.thread(child)->context.r[8], 1)), uint64_t{0x1122334455667788ull}, "upper GPR lane is restored");
            test.Equals(restored.thread(child)->context.f[19], -123.5f, "FPU context is restored");
            test.Equals(restored.thread(child)->context.lladdr, 0x12340u, "LL/SC reservation survives");
            std::array<float, 4> vector;
            _mm_storeu_ps(vector.data(), restored.thread(child)->context.vu0_vf[17]);
            test.Equals(vector[3], 4.0f, "VU0 vector context survives");
            restored.signalSemaphore(semaphore, true);
            test.Equals(restored.thread(1)->status, EeThreadStatus::Ready, "restored semaphore wakeup works");
            test.IsTrue(restored.semaphore(semaphore)->waiters.empty(), "wakeup removes the saved waiter");
        });

        suite.Run("truncation and malformed metadata leave live state unchanged", [](TestCase &test)
        {
            StateTestEnv env;
            EeScheduler &scheduler = env.runtime.eeScheduler();
            const int semaphore = scheduler.createSemaphore(2, 4, 0, 0);
            std::vector<uint8_t> state;
            std::string error;
            test.IsTrue(scheduler.captureState(state, error), "fixture captures: " + error);
            if (state.size() < 180u) return;
            scheduler.thread(1)->context.pc = 0x300040u;
            scheduler.semaphore(semaphore)->count = 1;
            for (const size_t length : {size_t{0}, size_t{1}, size_t{19}, state.size() / 2u, state.size() - 1u})
            {
                const std::vector<uint8_t> broken(state.begin(), state.begin() + length);
                test.IsFalse(scheduler.restoreState(broken, error), "truncated state must be rejected");
                test.Equals(scheduler.thread(1)->context.pc, 0x300040u, "failed restore cannot change the PC");
                test.Equals(scheduler.semaphore(semaphore)->count, 1, "failed restore cannot change kernel objects");
            }
            for (const size_t offset : {size_t{0}, size_t{8}, size_t{12}, size_t{172}})
            {
                auto broken = state;
                broken[offset] = offset == 172u ? 2u : static_cast<uint8_t>(broken[offset] ^ 0x80u);
                test.IsFalse(scheduler.restoreState(broken, error), "bad magic/version/context ABI/bool must be rejected");
                test.Equals(scheduler.thread(1)->context.pc, 0x300040u, "metadata rejection is atomic");
            }
            auto trailing = state; trailing.push_back(0);
            test.IsFalse(scheduler.validateState(trailing, error), "unexpected trailing bytes must be rejected");
            test.IsTrue(scheduler.restoreState(state, error), "valid state still loads after rejection cases");
        });

        suite.Run("closures and foreign executor captures are explicit failures", [](TestCase &test)
        {
            StateTestEnv env;
            EeScheduler &scheduler = env.runtime.eeScheduler();
            std::vector<uint8_t> output{0xAA, 0xBB};
            std::string error;
            scheduler.thread(1)->resumeCompletion = [](R5900Context &) {};
            test.IsFalse(scheduler.captureState(output, error), "host completion closure cannot be silently dropped");
            test.Equals(output.size(), size_t{2}, "capture failure preserves the caller's previous state");
            test.IsTrue(error.find("completion") != std::string::npos, "unsupported feature is diagnosed");
            scheduler.thread(1)->resumeCompletion = {};
            GuestInvocation callback;
            callback.kind = GuestInvocationKind::RpcCallback;
            callback.context.pc = 0x300010u;
            callback.onComplete = [](const R5900Context &, R5900Context &) {};
            scheduler.queueInvocation(std::move(callback));
            test.IsFalse(scheduler.captureState(output, error), "invocation closure cannot be silently dropped");
            bool foreignResult = true;
            std::thread other([&] { foreignResult = scheduler.captureState(output, error); });
            other.join();
            test.IsFalse(foreignResult, "only the executor may capture scheduler state");
        });

        suite.Run("restored ready queues execute in priority order", [](TestCase &test)
        {
            StateTestEnv source;
            auto &scheduler = source.runtime.eeScheduler();
            const int low = scheduler.createThread(EeThreadCreateParams{0, 0x300020u, 0x190000u, 0x4000u, 0, 20, 0});
            const int high = scheduler.createThread(EeThreadCreateParams{0, 0x300010u, 0x1A0000u, 0x4000u, 0, 4, 0});
            scheduler.startThread(low, 0, source.context, false); scheduler.startThread(high, 0, source.context, false);
            std::vector<uint8_t> state;
            std::string error;
            test.IsTrue(scheduler.captureState(state, error), "runnable fixture captures: " + error);
            if (state.empty()) return;
            StateTestEnv destination;
            destination.runtime.registerFunction(0x300000u, stateMain);
            destination.runtime.registerFunction(0x300010u, stateHigh);
            destination.runtime.registerFunction(0x300020u, stateLow);
            test.IsTrue(destination.runtime.eeScheduler().restoreState(state, error), "runnable state restores: " + error);
            stateTrace.clear();
            destination.runtime.eeScheduler().run();
            test.IsTrue(stateTrace == std::vector<int>({1, 2, 3}), "actual dispatcher resumes restored priority queues");
        });
    });

    MiniTest::Case("KernelHostState", [](TestCase &suite)
    {
        suite.Run("RPC globals and allocator counters restore without serializing pointers", [](TestCase &test)
        {
            std::string error;
            std::vector<uint8_t> original;
            if (!ps2_syscalls::captureKernelHostState(original, error)) { test.Fail("initial kernel host capture: " + error); return; }
            g_tls_index = 37;
            g_rpc_packet_index = 91;
            g_rpc_servers[0x1234u] = RpcServerState{0x1234u, 0x180000u};
            g_rpc_clients[0x190000u] = RpcClientState{false, 77, 0x1234u};
            std::vector<uint8_t> saved;
            test.IsTrue(ps2_syscalls::captureKernelHostState(saved, error), "idle RPC state captures: " + error);
            g_tls_index = 99; g_rpc_packet_index = 0; g_rpc_servers.clear(); g_rpc_clients.clear();
            test.IsTrue(ps2_syscalls::restoreKernelHostState(saved, error), "RPC host state restores: " + error);
            test.Equals(g_tls_index, 37u, "TLS allocation counter is restored");
            test.Equals(g_rpc_packet_index, 91u, "RPC packet allocation cursor is restored");
            test.Equals(g_rpc_clients.at(0x190000u).last_rpc, 77u, "RPC client payload is restored");
            auto broken = saved; broken.pop_back();
            g_tls_index = 123;
            test.IsFalse(ps2_syscalls::restoreKernelHostState(broken, error), "truncated kernel state is rejected");
            test.Equals(g_tls_index, 123u, "failed kernel restore leaves counters unchanged");
            g_rpc_clients.at(0x190000u).busy = true;
            std::vector<uint8_t> output{0xAB};
            test.IsFalse(ps2_syscalls::captureKernelHostState(output, error), "in-flight RPC cannot be captured as idle");
            test.Equals(output.size(), size_t{1}, "busy RPC failure does not replace the old save");
            g_rpc_clients.at(0x190000u).busy = false;
            test.IsTrue(ps2_syscalls::restoreKernelHostState(original, error), "test restores prior kernel globals: " + error);
        });
    });
}

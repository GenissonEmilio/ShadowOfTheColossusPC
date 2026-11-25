// Included by EeScheduler.cpp so the scheduler state has one implementation
// unit. This is a versioned guest-state archive, not a dump of C++ containers,
// closures, mutexes, thread IDs, or host addresses.
namespace
{
    constexpr uint64_t kEeStateMagic = 0x3144454843534545ull; // "EESCHED1"
    constexpr uint32_t kEeStateVersion = 1u;
    constexpr size_t kEeStateLimit = 16u * 1024u * 1024u;
    constexpr uint32_t kEeStateObjects = 4096u;
    constexpr uint32_t kEeStateThreads = 1024u;
    constexpr uint32_t kEeStateInvocationDepth = 256u;
    constexpr int64_t kEeStateMaxHostDelta = 366ll * 24ll * 60ll * 60ll * 1000000000ll;

    struct EeStateWriter
    {
        std::vector<uint8_t> data;
        void raw(const void *source, size_t size)
        {
            if (size > kEeStateLimit - data.size())
                throw std::runtime_error("EE state exceeds the size limit");
            const auto *first = static_cast<const uint8_t *>(source);
            data.insert(data.end(), first, first + size);
        }
        void u8(uint8_t value) { raw(&value, 1u); }
        void u32(uint32_t value)
        {
            for (unsigned shift = 0; shift != 32u; shift += 8u)
                u8(static_cast<uint8_t>(value >> shift));
        }
        void u64(uint64_t value)
        {
            for (unsigned shift = 0; shift != 64u; shift += 8u)
                u8(static_cast<uint8_t>(value >> shift));
        }
        void integer(int value) { u32(static_cast<uint32_t>(value)); }
        void boolean(bool value) { u8(value ? 1u : 0u); }
        void count(size_t value, uint32_t maximum)
        {
            if (value > maximum)
                throw std::runtime_error("EE state contains too many objects");
            u32(static_cast<uint32_t>(value));
        }
        void context(const R5900Context &value) { raw(&value, sizeof(value)); }
    };

    struct EeStateReader
    {
        const std::vector<uint8_t> &data;
        size_t offset = 0;
        void raw(void *destination, size_t size)
        {
            if (size > data.size() - offset)
                throw std::runtime_error("Truncated EE state");
            std::memcpy(destination, data.data() + offset, size);
            offset += size;
        }
        uint8_t u8()
        {
            uint8_t value;
            raw(&value, 1u);
            return value;
        }
        uint32_t u32()
        {
            uint32_t value = 0;
            for (unsigned shift = 0; shift != 32u; shift += 8u)
                value |= static_cast<uint32_t>(u8()) << shift;
            return value;
        }
        uint64_t u64()
        {
            uint64_t value = 0;
            for (unsigned shift = 0; shift != 64u; shift += 8u)
                value |= static_cast<uint64_t>(u8()) << shift;
            return value;
        }
        int integer() { return std::bit_cast<int32_t>(u32()); }
        bool boolean()
        {
            const uint8_t value = u8();
            if (value > 1u)
                throw std::runtime_error("Invalid boolean in EE state");
            return value != 0u;
        }
        uint32_t count(uint32_t maximum)
        {
            const uint32_t value = u32();
            if (value > maximum)
                throw std::runtime_error("EE state object count exceeds its limit");
            return value;
        }
        R5900Context context()
        {
            static_assert(std::is_trivially_copyable_v<R5900Context>);
            static_assert(sizeof(bool) == 1u);
            if (sizeof(R5900Context) > data.size() - offset)
                throw std::runtime_error("Truncated EE context");
            // Check the only C++ bool before creating a typed object from an
            // archive. An arbitrary object representation of bool is invalid.
            if (data[offset + offsetof(R5900Context, in_delay_slot)] > 1u)
                throw std::runtime_error("Invalid EE delay-slot boolean");
            R5900Context value;
            raw(&value, sizeof(value));
            if ((value.pc & 3u) != 0u)
                throw std::runtime_error("Unaligned EE program counter");
            return value;
        }
    };

    void eeStateEvent(EeStateWriter &writer, const EeEvent &event)
    {
        writer.u8(static_cast<uint8_t>(event.type));
        writer.u32(event.id);
        writer.u64(event.value);
    }

    EeEvent eeStateEvent(EeStateReader &reader)
    {
        EeEvent event;
        const uint8_t type = reader.u8();
        if (type == static_cast<uint8_t>(EeEventType::Stop) || type > static_cast<uint8_t>(EeEventType::Alarm))
            throw std::runtime_error("Invalid or terminating event in EE state");
        event.type = static_cast<EeEventType>(type);
        event.id = reader.u32();
        event.value = reader.u64();
        return event;
    }

    void eeStateInvocation(EeStateWriter &writer, const GuestInvocation &invocation)
    {
        if (invocation.onComplete)
            throw std::runtime_error("EE callback completion is not serializable; retry at a safe point");
        writer.u8(static_cast<uint8_t>(invocation.kind));
        writer.u64(invocation.sequence);
        writer.u64(invocation.tag);
        writer.context(invocation.context);
    }

    GuestInvocation eeStateInvocation(EeStateReader &reader)
    {
        GuestInvocation invocation;
        const uint8_t kind = reader.u8();
        if (kind > static_cast<uint8_t>(GuestInvocationKind::SifCommand))
            throw std::runtime_error("Invalid EE invocation kind");
        invocation.kind = static_cast<GuestInvocationKind>(kind);
        invocation.sequence = reader.u64();
        invocation.tag = reader.u64();
        invocation.context = reader.context();
        return invocation;
    }

    template<class Map, class Write>
    void eeStateMap(EeStateWriter &writer, const Map &objects, uint32_t maximum, Write write)
    {
        writer.count(objects.size(), maximum);
        // Stable output makes an unchanged scheduler state reproducible.
        std::vector<typename Map::key_type> keys;
        keys.reserve(objects.size());
        for (const auto &[key, value] : objects)
        {
            (void)value;
            keys.push_back(key);
        }
        std::sort(keys.begin(), keys.end());
        for (const auto &key : keys)
            write(objects.at(key));
    }

    void eeStateIds(EeStateWriter &writer, const std::deque<int> &ids)
    {
        writer.count(ids.size(), kEeStateThreads);
        for (const int id : ids)
            writer.integer(id);
    }

    std::deque<int> eeStateIds(EeStateReader &reader)
    {
        std::deque<int> ids;
        const uint32_t count = reader.count(kEeStateThreads);
        for (uint32_t i = 0; i < count; ++i)
            ids.push_back(reader.integer());
        return ids;
    }

    bool eeStateGuestAddress(uint32_t address, uint32_t size)
    {
        return address == 0u || (address & 0x1FFFFFFFu) <= PS2_RAM_SIZE - size;
    }
}

struct EeScheduler::SavedState
{
    std::array<std::deque<int>, kPriorityCount> ready;
    std::unordered_map<int, GuestThread> threads;
    std::unordered_map<int, EeSemaphore> semaphores;
    std::unordered_map<int, EeEventFlag> flags;
    std::unordered_map<int, EeAlarm> alarms;
    std::unordered_map<int, EeIrqHandler> intc, dmac;
    std::unordered_map<uint64_t, uint32_t> stackTops;
    struct Deadline
    {
        uint64_t cycle;
        int64_t hostDeltaNs;
        EeEvent event;
        uint64_t sequence;
    };
    std::vector<Deadline> deadlines;
    std::deque<EeEvent> events;
    std::deque<GuestInvocation> pendingInvocations;
    __m128 sharedRandom{};
    int nextThread = 2, nextInvocationThread = -1, nextSemaphore = 1, nextFlag = 1, nextAlarm = 1;
    int nextIntc = 1, nextDmac = 1, intcHead = 0, intcTail = 1000, dmacHead = 0, dmacTail = 1000;
    int currentThread = 0;
    uint32_t enabledIntc = 0, enabledDmac = 0, timerInterrupts = 0;
    uint32_t vsyncFlagAddress = 0, vsyncTickAddress = 0, gsCallback = 0, gsGp = 0, gsSp = 0;
    uint64_t nextDelay = 1, cycle = 0, accounted = 0, sliceEnd = 0;
    uint64_t eventSequence = 0, invocationSequence = 0, vsyncTick = 0;
    bool reschedule = false, sliceExpired = false, unwinding = false, gateEnabled = false;
    bool hostClockSync = false, checkpointPending = false;
    int64_t hostElapsedNs = 0;
};

bool EeScheduler::stateAccessAllowed(std::string &error) const
{
    if (m_executorThread != std::this_thread::get_id() || m_guestExecuting.load(std::memory_order_acquire) || m_insideInterrupt)
    {
        error = "EE state requires the executor at a dispatcher boundary";
        return false;
    }
    if (m_stopRequested.load(std::memory_order_acquire) || m_rdram == nullptr)
    {
        error = "EE state is unavailable while the runtime is stopping or uninitialized";
        return false;
    }
    return true;
}

bool EeScheduler::captureState(std::vector<uint8_t> &bytes, std::string &error) const
{
    if (!stateAccessAllowed(error))
        return false;
    try
    {
        if constexpr (std::endian::native != std::endian::little)
            throw std::runtime_error("EE context archive requires a little-endian build");
        EeStateWriter writer;
        writer.u64(kEeStateMagic);
        writer.u32(kEeStateVersion);
        writer.u32(sizeof(R5900Context));
        writer.u32(0x01020304u);
        std::lock_guard lock(m_eventMutex);
        const auto now = std::chrono::steady_clock::now();
        writer.raw(&m_sharedVu0Random, sizeof(m_sharedVu0Random));
        for (const int value : {m_nextThreadId, m_nextInvocationThreadId, m_nextSemaphoreId, m_nextEventFlagId,
                                m_nextAlarmId, m_nextIntcHandlerId, m_nextDmacHandlerId, m_intcHeadOrder,
                                m_intcTailOrder, m_dmacHeadOrder, m_dmacTailOrder, m_currentThreadId})
            writer.integer(value);
        for (const uint32_t value : {m_enabledIntcMask, m_enabledDmacMask, m_pendingEeTimerInterrupts,
                                     m_vsyncFlagAddress, m_vsyncTickAddress, m_gsVSyncCallback, m_gsVSyncCallbackGp, m_gsVSyncCallbackSp})
            writer.u32(value);
        for (const uint64_t value : {m_nextDelayToken, m_eeCycle, m_accountedCycle, m_sliceEndCycle,
                                     m_eventSequence, m_invocationSequence, m_vsyncTick})
            writer.u64(value);
        for (const bool value : {m_rescheduleRequested, m_timeSliceExpired, m_unwinding, m_gateEnabled,
                                 m_hostClockSync, m_checkpointPending.load(std::memory_order_acquire)})
            writer.boolean(value);
        writer.u64(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - m_hostClockBase).count()));
        eeStateMap(writer, m_threads, kEeStateThreads, [&](const GuestThread &thread)
        {
            if (thread.wait.completion || thread.resumeCompletion)
                throw std::runtime_error("EE thread " + std::to_string(thread.id) + " has a nonserializable completion; retry at a safe point");
            writer.integer(thread.id);
            writer.context(thread.context);
            for (uint32_t value : {thread.entry, thread.stack, thread.stackSize, thread.gp, thread.attr, thread.option, thread.arg, thread.wakeupCount, thread.tlsBase})
                writer.u32(value);
            writer.integer(thread.initialPriority);
            writer.integer(thread.currentPriority);
            writer.integer(thread.suspendCount);
            writer.boolean(thread.ownsStack);
            writer.u8(static_cast<uint8_t>(thread.status));
            writer.u8(static_cast<uint8_t>(thread.wait.reason));
            switch (thread.wait.reason)
            {
            case EeWaitReason::None:
            case EeWaitReason::Sleep:
                if (!std::holds_alternative<std::monostate>(thread.wait.payload))
                    throw std::runtime_error("Invalid live EE wait payload");
                break;
            case EeWaitReason::Semaphore:
                writer.integer(std::get<EeSemaphoreWait>(thread.wait.payload).id);
                break;
            case EeWaitReason::EventFlag:
            {
                const auto &wait = std::get<EeEventFlagWait>(thread.wait.payload);
                writer.integer(wait.id); writer.u32(wait.bits); writer.u32(wait.mode); writer.u32(wait.resultAddress);
                break;
            }
            case EeWaitReason::VSync:
            {
                const auto &wait = std::get<EeVSyncWait>(thread.wait.payload);
                writer.u64(wait.afterTick); writer.integer(wait.fixedResult);
                break;
            }
            case EeWaitReason::External:
            case EeWaitReason::Mpeg:
            {
                const auto &wait = std::get<EeExternalWait>(thread.wait.payload);
                writer.u32(wait.type); writer.u64(wait.token);
                break;
            }
            default: throw std::runtime_error("Invalid live EE wait reason");
            }
            writer.count(thread.invocations.size(), kEeStateInvocationDepth);
            for (const auto &invocation : thread.invocations)
                eeStateInvocation(writer, invocation);
        });
        for (const auto &queue : m_readyQueues)
            eeStateIds(writer, queue);
        eeStateMap(writer, m_semaphores, kEeStateObjects, [&](const EeSemaphore &object)
        {
            writer.integer(object.id); writer.integer(object.count); writer.integer(object.maxCount); writer.integer(object.initCount);
            writer.u32(object.attr); writer.u32(object.option); eeStateIds(writer, object.waiters);
        });
        eeStateMap(writer, m_eventFlags, kEeStateObjects, [&](const EeEventFlag &object)
        {
            writer.integer(object.id); writer.u32(object.attr); writer.u32(object.option); writer.u32(object.initBits); writer.u32(object.bits);
            eeStateIds(writer, object.waiters);
        });
        eeStateMap(writer, m_alarms, kEeStateObjects, [&](const EeAlarm &object)
        {
            writer.integer(object.id); writer.u32(object.ticks); writer.u32(object.handler); writer.u32(object.argument); writer.u32(object.gp); writer.u32(object.sp);
        });
        const auto writeHandlers = [&](const auto &handlers)
        {
            eeStateMap(writer, handlers, kEeStateObjects, [&](const EeIrqHandler &object)
            {
                writer.integer(object.id); writer.u32(object.cause); writer.u32(object.handler); writer.u32(object.argument);
                writer.u32(object.gp); writer.u32(object.sp); writer.boolean(object.enabled); writer.integer(object.order);
            });
        };
        writeHandlers(m_intcHandlers); writeHandlers(m_dmacHandlers);
        writer.count(m_events.size(), kEeStateObjects);
        for (const auto &event : m_events) eeStateEvent(writer, event);
        writer.count(m_deadlines.size(), kEeStateObjects);
        for (const auto &event : m_deadlines)
        {
            writer.u64(event.deadlineCycle);
            writer.u64(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(event.hostDeadline - now).count()));
            eeStateEvent(writer, event.event); writer.u64(event.sequence);
        }
        writer.count(m_pendingInvocations.size(), kEeStateObjects);
        for (const auto &invocation : m_pendingInvocations) eeStateInvocation(writer, invocation);
        writer.count(m_invocationStackTops.size(), kEeStateObjects);
        std::vector<std::pair<uint64_t, uint32_t>> stackTops(m_invocationStackTops.begin(), m_invocationStackTops.end());
        std::sort(stackTops.begin(), stackTops.end());
        for (const auto &[key, top] : stackTops) { writer.u64(key); writer.u32(top); }
        SavedState checked;
        if (!decodeState(writer.data, checked, error))
            return false;
        bytes.swap(writer.data);
        error.clear();
        return true;
    }
    catch (const std::exception &exception)
    {
        error = exception.what();
        return false;
    }
}

bool EeScheduler::decodeState(const std::vector<uint8_t> &bytes, SavedState &state, std::string &error) const
{
    try
    {
        if (bytes.size() > kEeStateLimit || std::endian::native != std::endian::little)
            throw std::runtime_error("Unsupported EE state size or host endianness");
        EeStateReader reader{bytes};
        if (reader.u64() != kEeStateMagic || reader.u32() != kEeStateVersion || reader.u32() != sizeof(R5900Context) || reader.u32() != 0x01020304u)
            throw std::runtime_error("Incompatible EE state format/context ABI");
        reader.raw(&state.sharedRandom, sizeof(state.sharedRandom));
        for (int *value : {&state.nextThread, &state.nextInvocationThread, &state.nextSemaphore, &state.nextFlag, &state.nextAlarm,
                           &state.nextIntc, &state.nextDmac, &state.intcHead, &state.intcTail, &state.dmacHead, &state.dmacTail, &state.currentThread})
            *value = reader.integer();
        for (uint32_t *value : {&state.enabledIntc, &state.enabledDmac, &state.timerInterrupts, &state.vsyncFlagAddress,
                                &state.vsyncTickAddress, &state.gsCallback, &state.gsGp, &state.gsSp})
            *value = reader.u32();
        for (uint64_t *value : {&state.nextDelay, &state.cycle, &state.accounted, &state.sliceEnd, &state.eventSequence, &state.invocationSequence, &state.vsyncTick})
            *value = reader.u64();
        for (bool *value : {&state.reschedule, &state.sliceExpired, &state.unwinding, &state.gateEnabled, &state.hostClockSync, &state.checkpointPending})
            *value = reader.boolean();
        state.hostElapsedNs = std::bit_cast<int64_t>(reader.u64());
        auto require = [](bool condition, const char *message)
        {
            if (!condition) throw std::runtime_error(message);
        };
        require(state.nextThread >= kFirstThreadId && state.nextThread <= kLastThreadId && state.nextInvocationThread < 0 &&
                state.nextInvocationThread > -65536 && state.nextSemaphore > 0 && state.nextFlag > 0 && state.nextAlarm > 0 &&
                state.nextIntc > 0 && state.nextDmac > 0 && state.nextDelay != 0u, "Invalid EE object allocator state");
        constexpr uint64_t maxCycle = kEeClockHz * 366ull * 24ull * 60ull * 60ull;
        require(state.accounted <= state.cycle && state.cycle <= maxCycle && state.sliceEnd <= maxCycle,
                "Invalid EE cycle accounting");
        require(state.hostElapsedNs >= -kEeStateMaxHostDelta && state.hostElapsedNs <= kEeStateMaxHostDelta,
                "Invalid EE host-clock offset");
        require((state.timerInterrupts & ~0xFu) == 0u && eeStateGuestAddress(state.vsyncFlagAddress, 4u) &&
                eeStateGuestAddress(state.vsyncTickAddress, 8u), "Invalid EE timer/flag state");
        for (uint32_t remaining = reader.count(kEeStateThreads); remaining != 0u; --remaining)
        {
            GuestThread object;
            object.id = reader.integer();
            object.context = reader.context();
            for (uint32_t *value : {&object.entry, &object.stack, &object.stackSize, &object.gp, &object.attr, &object.option,
                                    &object.arg, &object.wakeupCount, &object.tlsBase})
                *value = reader.u32();
            object.initialPriority = reader.integer(); object.currentPriority = reader.integer(); object.suspendCount = reader.integer();
            object.ownsStack = reader.boolean();
            const uint8_t status = reader.u8(), reason = reader.u8();
            require(object.id != 0 && object.id >= -65535 && object.id <= kLastThreadId && status <= static_cast<uint8_t>(EeThreadStatus::Dormant) &&
                    reason <= static_cast<uint8_t>(EeWaitReason::Mpeg) && object.initialPriority >= 0 && object.initialPriority < kPriorityCount &&
                    object.currentPriority >= 0 && object.currentPriority < kPriorityCount && object.suspendCount >= 0, "Invalid EE thread metadata");
            object.status = static_cast<EeThreadStatus>(status); object.wait.reason = static_cast<EeWaitReason>(reason);
            switch (object.wait.reason)
            {
            case EeWaitReason::None: case EeWaitReason::Sleep: break;
            case EeWaitReason::Semaphore: object.wait.payload = EeSemaphoreWait{reader.integer()}; break;
            case EeWaitReason::EventFlag:
            {
                EeEventFlagWait wait; wait.id = reader.integer(); wait.bits = reader.u32(); wait.mode = reader.u32(); wait.resultAddress = reader.u32();
                require(eeStateGuestAddress(wait.resultAddress, 4u), "Invalid EE event-flag result address");
                object.wait.payload = wait; break;
            }
            case EeWaitReason::VSync:
            {
                EeVSyncWait wait; wait.afterTick = reader.u64(); wait.fixedResult = reader.integer(); object.wait.payload = wait; break;
            }
            case EeWaitReason::External: case EeWaitReason::Mpeg:
            {
                EeExternalWait wait; wait.type = reader.u32(); wait.token = reader.u64(); object.wait.payload = wait; break;
            }
            }
            for (uint32_t i = reader.count(kEeStateInvocationDepth); i != 0u; --i)
                object.invocations.push_back(eeStateInvocation(reader));
            require(state.threads.emplace(object.id, std::move(object)).second, "Duplicate EE thread ID");
        }
        for (auto &queue : state.ready) queue = eeStateIds(reader);
        for (uint32_t remaining = reader.count(kEeStateObjects); remaining != 0u; --remaining)
        {
            EeSemaphore object;
            object.id = reader.integer(); object.count = reader.integer(); object.maxCount = reader.integer(); object.initCount = reader.integer();
            object.attr = reader.u32(); object.option = reader.u32(); object.waiters = eeStateIds(reader);
            require(object.id > 0 && object.count >= 0 && object.maxCount >= object.count && object.initCount >= 0 && object.initCount <= object.maxCount,
                    "Invalid EE semaphore");
            require(state.semaphores.emplace(object.id, std::move(object)).second, "Duplicate EE semaphore ID");
        }
        for (uint32_t remaining = reader.count(kEeStateObjects); remaining != 0u; --remaining)
        {
            EeEventFlag object;
            object.id = reader.integer(); object.attr = reader.u32(); object.option = reader.u32(); object.initBits = reader.u32(); object.bits = reader.u32();
            object.waiters = eeStateIds(reader);
            require(object.id > 0 && state.flags.emplace(object.id, std::move(object)).second, "Invalid/duplicate EE event flag");
        }
        for (uint32_t remaining = reader.count(kEeStateObjects); remaining != 0u; --remaining)
        {
            EeAlarm object;
            object.id = reader.integer(); const uint32_t ticks = reader.u32(); object.ticks = static_cast<uint16_t>(ticks);
            object.handler = reader.u32(); object.argument = reader.u32(); object.gp = reader.u32(); object.sp = reader.u32();
            require(object.id > 0 && ticks <= 65535u && object.handler != 0u && (object.handler & 3u) == 0u &&
                    state.alarms.emplace(object.id, object).second, "Invalid/duplicate EE alarm");
        }
        const auto readHandlers = [&](auto &handlers)
        {
            for (uint32_t remaining = reader.count(kEeStateObjects); remaining != 0u; --remaining)
            {
                EeIrqHandler object;
                object.id = reader.integer(); object.cause = reader.u32(); object.handler = reader.u32(); object.argument = reader.u32();
                object.gp = reader.u32(); object.sp = reader.u32(); object.enabled = reader.boolean(); object.order = reader.integer();
                require(object.id > 0 && (object.handler & 3u) == 0u && handlers.emplace(object.id, object).second, "Invalid/duplicate EE IRQ handler");
            }
        };
        readHandlers(state.intc); readHandlers(state.dmac);
        for (uint32_t remaining = reader.count(kEeStateObjects); remaining != 0u; --remaining) state.events.push_back(eeStateEvent(reader));
        for (uint32_t remaining = reader.count(kEeStateObjects); remaining != 0u; --remaining)
        {
            SavedState::Deadline deadline;
            deadline.cycle = reader.u64(); deadline.hostDeltaNs = std::bit_cast<int64_t>(reader.u64());
            deadline.event = eeStateEvent(reader); deadline.sequence = reader.u64();
            require(deadline.cycle <= maxCycle && deadline.hostDeltaNs >= -kEeStateMaxHostDelta && deadline.hostDeltaNs <= kEeStateMaxHostDelta &&
                    deadline.sequence != 0u && deadline.sequence <= state.eventSequence, "Invalid EE scheduled deadline");
            state.deadlines.push_back(deadline);
        }
        for (uint32_t remaining = reader.count(kEeStateObjects); remaining != 0u; --remaining) state.pendingInvocations.push_back(eeStateInvocation(reader));
        std::set<uint32_t> reservedStackTops;
        for (uint32_t remaining = reader.count(kEeStateObjects); remaining != 0u; --remaining)
        {
            const uint64_t key = reader.u64(); const uint32_t top = reader.u32();
            const int owner = std::bit_cast<int32_t>(static_cast<uint32_t>(key >> 32u));
            require(top >= ps2x::kernel_memory::kCallbackStackFloor + ps2x::kernel_memory::kCallbackStackSize &&
                    top <= ps2x::kernel_memory::kCallbackStackTop && (top & 15u) == 0u &&
                    (top <= ps2x::kernel_memory::kRpcPacketPoolBase ||
                     top - ps2x::kernel_memory::kCallbackStackSize >= ps2x::kernel_memory::kBootModePoolBase + ps2x::kernel_memory::kBootModePoolBytes) &&
                    owner != 0 && owner >= -65535 && owner <= kLastThreadId && static_cast<uint32_t>(key) <= kEeStateInvocationDepth &&
                    reservedStackTops.insert(top).second && state.stackTops.emplace(key, top).second,
                    "Invalid/duplicate EE callback stack reservation");
        }
        require(reader.offset == bytes.size(), "Trailing data in EE state");

        std::set<int> readyIds, waiterIds;
        int runningCount = 0;
        for (size_t priority = 0; priority < state.ready.size(); ++priority)
            for (const int id : state.ready[priority])
            {
                const auto thread = state.threads.find(id);
                require(thread != state.threads.end() && thread->second.status == EeThreadStatus::Ready &&
                        thread->second.currentPriority == static_cast<int>(priority) && readyIds.insert(id).second,
                        "Inconsistent EE ready queue");
            }
        const auto validateWaiters = [&](const auto &objects, EeWaitReason reason)
        {
            for (const auto &[id, object] : objects)
                for (const int tid : object.waiters)
                {
                    const auto thread = state.threads.find(tid);
                    require(thread != state.threads.end() && (thread->second.status == EeThreadStatus::Waiting || thread->second.status == EeThreadStatus::WaitingSuspended) &&
                            thread->second.wait.reason == reason && waitObjectId(thread->second.wait) == id && waiterIds.insert(tid).second,
                            "Inconsistent EE object wait queue");
                }
        };
        validateWaiters(state.semaphores, EeWaitReason::Semaphore); validateWaiters(state.flags, EeWaitReason::EventFlag);
        const auto validateInvocation = [&](const GuestInvocation &invocation)
        {
            require(invocation.sequence != 0u && invocation.sequence <= state.invocationSequence, "Invalid EE invocation sequence");
        };
        for (const auto &[id, object] : state.threads)
        {
            const bool waiting = object.status == EeThreadStatus::Waiting || object.status == EeThreadStatus::WaitingSuspended;
            require(waiting == (object.wait.reason != EeWaitReason::None), "Inconsistent EE thread wait state");
            require((object.status == EeThreadStatus::Ready) == readyIds.contains(id), "Missing EE ready-queue entry");
            if (object.status == EeThreadStatus::Running)
            {
                ++runningCount;
                require(id == state.currentThread, "Inconsistent EE running thread");
            }
            if (object.wait.reason == EeWaitReason::Semaphore || object.wait.reason == EeWaitReason::EventFlag)
                require(waiterIds.contains(id), "Missing EE waiter entry");
            if (object.status == EeThreadStatus::WaitingSuspended || object.status == EeThreadStatus::Suspended)
                require(object.suspendCount > 0, "Missing EE suspension count");
            for (const auto &invocation : object.invocations) validateInvocation(invocation);
        }
        require((state.currentThread == 0 && runningCount == 0) || (state.currentThread != 0 && runningCount == 1), "Missing EE running thread");
        for (const auto &invocation : state.pendingInvocations) validateInvocation(invocation);
        error.clear();
        return true;
    }
    catch (const std::exception &exception)
    {
        error = exception.what();
        return false;
    }
}

bool EeScheduler::validateState(const std::vector<uint8_t> &bytes, std::string &error) const
{
    SavedState state;
    return decodeState(bytes, state, error);
}

bool EeScheduler::restoreState(const std::vector<uint8_t> &bytes, std::string &error)
{
    if (!stateAccessAllowed(error))
        return false;
    SavedState state;
    if (!decodeState(bytes, state, error))
        return false;
    // Allocate/rebase every deadline before mutation. The commit below only
    // swaps owning containers and assigns scalar/context state.
    std::vector<ScheduledEvent> deadlines;
    const auto now = std::chrono::steady_clock::now();
    try
    {
        deadlines.reserve(state.deadlines.size());
        for (const auto &deadline : state.deadlines)
            deadlines.push_back(ScheduledEvent{deadline.cycle, now + std::chrono::nanoseconds(deadline.hostDeltaNs), deadline.event, deadline.sequence});
    }
    catch (const std::exception &exception)
    {
        error = exception.what();
        return false;
    }
    {
        std::lock_guard lock(m_eventMutex);
        m_profileContext.store(nullptr, std::memory_order_release);
        m_readyQueues.swap(state.ready); m_threads.swap(state.threads); m_semaphores.swap(state.semaphores);
        m_eventFlags.swap(state.flags); m_alarms.swap(state.alarms); m_intcHandlers.swap(state.intc); m_dmacHandlers.swap(state.dmac);
        m_invocationStackTops.swap(state.stackTops); m_events.swap(state.events); m_deadlines.swap(deadlines); m_pendingInvocations.swap(state.pendingInvocations);
        m_threadCacheId = -1; m_threadCache = nullptr;
        m_sharedVu0Random = state.sharedRandom;
        m_nextThreadId = state.nextThread; m_nextInvocationThreadId = state.nextInvocationThread;
        m_nextSemaphoreId = state.nextSemaphore; m_nextEventFlagId = state.nextFlag; m_nextAlarmId = state.nextAlarm;
        m_nextIntcHandlerId = state.nextIntc; m_nextDmacHandlerId = state.nextDmac;
        m_intcHeadOrder = state.intcHead; m_intcTailOrder = state.intcTail; m_dmacHeadOrder = state.dmacHead; m_dmacTailOrder = state.dmacTail;
        m_currentThreadId = state.currentThread; m_enabledIntcMask = state.enabledIntc; m_enabledDmacMask = state.enabledDmac;
        m_pendingEeTimerInterrupts = state.timerInterrupts; m_nextDelayToken = state.nextDelay;
        m_eeCycle = state.cycle; m_accountedCycle = state.accounted; m_sliceEndCycle = state.sliceEnd;
        m_eventSequence = state.eventSequence; m_invocationSequence = state.invocationSequence; m_vsyncTick = state.vsyncTick;
        m_vsyncFlagAddress = state.vsyncFlagAddress; m_vsyncTickAddress = state.vsyncTickAddress;
        m_gsVSyncCallback = state.gsCallback; m_gsVSyncCallbackGp = state.gsGp; m_gsVSyncCallbackSp = state.gsSp;
        m_rescheduleRequested = state.reschedule; m_timeSliceExpired = state.sliceExpired; m_unwinding = state.unwinding;
        m_gateEnabled = state.gateEnabled; m_hostClockSync = state.hostClockSync;
        m_hostClockBase = now - std::chrono::nanoseconds(state.hostElapsedNs);
        // The archive deadlines were rebased to `now`. If restore is part of
        // a runtime operation, its final scope must only exclude time since
        // this new anchor, not add the preceding parse/validation pause twice.
        if (m_stateOperationClockAnchor != std::chrono::steady_clock::time_point{})
            m_stateOperationClockAnchor = now;
        m_checkpointPending.store(state.checkpointPending || !m_events.empty() || !m_pendingInvocations.empty() ||
                                  m_rescheduleRequested || m_pendingEeTimerInterrupts != 0u || m_stateRequestPending.load(std::memory_order_acquire), std::memory_order_release);
        updateNextDeadline();
        m_debugPublishCountdown = 0u;
        m_snapshotWanted.store(true, std::memory_order_release);
        m_runtime.memory().gs().vsyncTick.store(m_vsyncTick, std::memory_order_release);
        ps2_stats::vblanks.store(m_vsyncTick, std::memory_order_relaxed);
        copyMainContextToRuntime();
    }
    m_eventCv.notify_all();
    error.clear();
    return true;
}

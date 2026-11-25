// Included by ps2_runtime.cpp: executor-side serialization, never UI-thread RAM access.
#include "runtime/save_state_archive.h"
#include "runtime/state_manager.h"
#include "runtime/host_state.h"
#include "Kernel/Syscalls/HostState.h"
#include <array>
#include <cerrno>
#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

bool PS2Runtime::captureRuntimeState(std::vector<uint8_t> &out, std::string &error)
{
    std::scoped_lock lock(m_eeKernelStateMutex,m_guestHeapMutex,m_asyncCallbackStackMutex);
    ps2state::Writer w(out,error); w.pod(uint32_t{0x31544e52});
    w.bytes(&m_cpuContext,sizeof(m_cpuContext));
    w.pod(m_reportedGsCsrEvents);
    w.pod(m_guestHeapBase);
    w.pod(m_guestHeapEnd);
    w.pod(m_guestHeapLimit);
    w.pod(m_guestHeapSuggestedBase);
    w.pod(m_guestHeapConfigured);
    w.pod(m_asyncCallbackStackFloor);
    w.pod(m_asyncCallbackStackTop);
    w.pod(m_guestJumpSerial);
    w.pod(m_directDispatch);
    w.pod(g_iopPendingEeCycles); w.pod(g_ps2FpuSetcsrNearest);
    w.pod(uint32_t(m_guestHeapBlocks.size()));
    for (const auto &b:m_guestHeapBlocks) { w.pod(b.addr); w.pod(b.size); w.pod(b.free); }
    w.pod(uint32_t(m_eeExitHandlers.size()));
    for (const auto &[id,handlers]:m_eeExitHandlers) { w.pod(id); w.vector(handlers,4096); }
    w.pod(uint32_t(m_eeSyscallOverrides.size()));
    for (const auto &[id,handler]:m_eeSyscallOverrides) { w.pod(id); w.pod(handler); }
    w.pod(uint32_t(m_eeSyscallMirrorAddresses.size()));
    for (auto addr:m_eeSyscallMirrorAddresses) w.pod(addr);
    w.pod(uint32_t(m_loadedModules.size()));
    for (const auto &m:m_loadedModules) { w.string(m.name); w.pod(m.baseAddress); w.pod(uint64_t(m.size)); w.pod(m.active); }
    return w.finish();
}
bool PS2Runtime::decodeRuntimeState(const std::vector<uint8_t> &blob,bool apply,std::string &error)
{
    ps2state::Reader r(blob,error); uint32_t magic=0; r.pod(magic);
    std::array<uint8_t,sizeof(R5900Context)> raw{}; r.bytes(raw.data(),raw.size());
    if (!r.good() || raw[offsetof(R5900Context,in_delay_slot)]>1) { error="Invalid EE context."; return false; }
    R5900Context cpu; std::memcpy(&cpu,raw.data(),raw.size());
    decltype(m_reportedGsCsrEvents) saved_m_reportedGsCsrEvents{}; r.pod(saved_m_reportedGsCsrEvents);
    decltype(m_guestHeapBase) saved_m_guestHeapBase{}; r.pod(saved_m_guestHeapBase);
    decltype(m_guestHeapEnd) saved_m_guestHeapEnd{}; r.pod(saved_m_guestHeapEnd);
    decltype(m_guestHeapLimit) saved_m_guestHeapLimit{}; r.pod(saved_m_guestHeapLimit);
    decltype(m_guestHeapSuggestedBase) saved_m_guestHeapSuggestedBase{}; r.pod(saved_m_guestHeapSuggestedBase);
    decltype(m_guestHeapConfigured) saved_m_guestHeapConfigured{}; r.pod(saved_m_guestHeapConfigured);
    decltype(m_asyncCallbackStackFloor) saved_m_asyncCallbackStackFloor{}; r.pod(saved_m_asyncCallbackStackFloor);
    decltype(m_asyncCallbackStackTop) saved_m_asyncCallbackStackTop{}; r.pod(saved_m_asyncCallbackStackTop);
    decltype(m_guestJumpSerial) saved_m_guestJumpSerial{}; r.pod(saved_m_guestJumpSerial);
    decltype(m_directDispatch) saved_m_directDispatch{}; r.pod(saved_m_directDispatch);
    uint64_t pending=0; bool fpu=false; r.pod(pending); r.pod(fpu);
    uint32_t count=0; r.pod(count);
    if (!r.good() || count>65536) { error="Invalid guest heap state."; return false; }
    std::vector<GuestHeapBlock> blocks(count);
    uint64_t previousEnd=0;
    for (auto &b:blocks) {
        r.pod(b.addr); r.pod(b.size); r.pod(b.free);
        if (b.addr<previousEnd || b.addr>PS2_RAM_SIZE || b.size>PS2_RAM_SIZE-b.addr) { error="Invalid heap block."; return false; }
        previousEnd=uint64_t(b.addr)+b.size;
    }
    std::unordered_map<int,std::vector<EeExitHandlerRegistration>> handlers;
    r.pod(count); if (!r.good() || count>4096) return false;
    for (uint32_t i=0;i<count;++i) { int id=0; std::vector<EeExitHandlerRegistration> list; r.pod(id); r.vector(list,4096); if (!handlers.emplace(id,std::move(list)).second) { error="Duplicate exit-handler owner."; return false; } }
    std::unordered_map<uint32_t,uint32_t> overrides;
    r.pod(count); if (!r.good() || count>4096) return false;
    for (uint32_t i=0;i<count;++i) { uint32_t id=0,addr=0; r.pod(id); r.pod(addr); if (!overrides.emplace(id,addr).second) { error="Duplicate syscall override."; return false; } }
    std::unordered_set<uint32_t> mirrors;
    r.pod(count); if (!r.good() || count>65536) return false;
    for (uint32_t i=0;i<count;++i) { uint32_t addr=0; r.pod(addr); if (addr>=PS2_RAM_SIZE || !mirrors.insert(addr).second) { error="Invalid syscall mirror."; return false; } }
    std::vector<LoadedModule> modules;
    r.pod(count); if (!r.good() || count>4096) return false;
    for (uint32_t i=0;i<count;++i) { LoadedModule m; uint64_t size=0; r.string(m.name); r.pod(m.baseAddress); r.pod(size); r.pod(m.active); if (m.baseAddress>=PS2_RAM_SIZE || size>PS2_RAM_SIZE-m.baseAddress) { error="Invalid guest module."; return false; } m.size=size; modules.push_back(std::move(m)); }
    if (!r.finish() || magic!=0x31544e52 || pending>=kIopBatchEeCycles ||
        saved_m_guestHeapBase>saved_m_guestHeapEnd || saved_m_guestHeapEnd>saved_m_guestHeapLimit || saved_m_guestHeapLimit>PS2_RAM_SIZE ||
        saved_m_asyncCallbackStackFloor>saved_m_asyncCallbackStackTop || saved_m_asyncCallbackStackTop>PS2_RAM_SIZE)
    { error="Invalid runtime state."; return false; }
    if (apply) {
        std::scoped_lock lock(m_eeKernelStateMutex,m_guestHeapMutex,m_asyncCallbackStackMutex);
        m_cpuContext=cpu;
        m_reportedGsCsrEvents=saved_m_reportedGsCsrEvents;
        m_guestHeapBase=saved_m_guestHeapBase;
        m_guestHeapEnd=saved_m_guestHeapEnd;
        m_guestHeapLimit=saved_m_guestHeapLimit;
        m_guestHeapSuggestedBase=saved_m_guestHeapSuggestedBase;
        m_guestHeapConfigured=saved_m_guestHeapConfigured;
        m_asyncCallbackStackFloor=saved_m_asyncCallbackStackFloor;
        m_asyncCallbackStackTop=saved_m_asyncCallbackStackTop;
        m_guestJumpSerial=saved_m_guestJumpSerial;
        m_directDispatch=saved_m_directDispatch;
        g_iopPendingEeCycles=pending; g_ps2FpuSetcsrNearest=fpu;
        m_guestHeapBlocks.swap(blocks); m_eeExitHandlers.swap(handlers); m_eeSyscallOverrides.swap(overrides);
        m_eeSyscallMirrorAddresses.swap(mirrors); m_loadedModules.swap(modules); g_dispatchHistory={};
        m_missingFunctionReported=false;
    }
    return true;
}

namespace {
    using MachineChunks=std::array<std::vector<uint8_t>,17>;
    bool decodeMachine(const std::vector<uint8_t> &blob,MachineChunks &chunks,std::string &error) {
        ps2state::Reader r(blob,error); uint32_t magic=0,count=0; r.pod(magic); r.pod(count);
        if (!r.good() || magic!=0x31504353 || count!=chunks.size()) { error="Unsupported machine-state layout."; return false; }
        for (auto &chunk:chunks) if (!r.vector(chunk,ps2state::DefaultLimit)) return false;
        return r.finish();
    }
}
bool PS2Runtime::captureMachineState(std::vector<uint8_t> &out,std::string &error) {
    if (!m_stateCaptureHook || !m_stateValidateHook || !m_stateRestoreHook) { error="Save states require the native Shadow runtime."; return false; }
    if (!m_audioBackend.stateCaptureSupported(error) || !native_renderer::stateBarrier(error)) return false;
    m_memory.syncVu1();
    if (!m_gs.prepareStateCapture(error)) return false;
    MachineChunks chunks;
    { auto &b=chunks[0]; auto &e=error; if (!(captureRuntimeState(b,e))) { error="Runtime: "+error; return false; } }
    { auto &b=chunks[1]; auto &e=error; if (!(m_eeScheduler->captureState(b,e))) { error="EE: "+error; return false; } }
    { auto &b=chunks[2]; auto &e=error; if (!(m_memory.captureState(b,e))) { error="Memory: "+error; return false; } }
    { auto &b=chunks[3]; auto &e=error; if (!(m_vu0.captureState(b,e))) { error="VU0: "+error; return false; } }
    { auto &b=chunks[4]; auto &e=error; if (!(m_vu1.captureState(b,e))) { error="VU1: "+error; return false; } }
    { auto &b=chunks[5]; auto &e=error; if (!(m_gs.captureState(b,e))) { error="GS: "+error; return false; } }
    { auto &b=chunks[6]; auto &e=error; if (!(m_iopSubsystem->captureState(b,e))) { error="IOP: "+error; return false; } }
    { auto &b=chunks[7]; auto &e=error; if (!(m_iopHost->captureState(b,e))) { error="IopHost: "+error; return false; } }
    { auto &b=chunks[8]; auto &e=error; if (!(m_vfs.captureState(b,e))) { error="VFS: "+error; return false; } }
    { auto &b=chunks[9]; auto &e=error; if (!(ps2_stubs::captureCdState(b,e))) { error="Cd: "+error; return false; } }
    { auto &b=chunks[10]; auto &e=error; if (!(ps2_stubs::captureMpegState(b,e))) { error="Mpeg: "+error; return false; } }
    { auto &b=chunks[11]; auto &e=error; if (!(ps2_stubs::captureCardState(b,e))) { error="Card: "+error; return false; } }
    { auto &b=chunks[12]; auto &e=error; if (!(ps2_stubs::captureAudioState(b,e))) { error="Audio: "+error; return false; } }
    { auto &b=chunks[13]; auto &e=error; if (!(ps2_stubs::captureLibcState(b,e))) { error="Libc: "+error; return false; } }
    { auto &b=chunks[14]; auto &e=error; if (!(ps2_syscalls::captureFioState(b,e))) { error="Fio: "+error; return false; } }
    { auto &b=chunks[15]; auto &e=error; if (!(ps2_syscalls::captureKernelHostState(b,e))) { error="KernelHost: "+error; return false; } }
    { auto &b=chunks[16]; auto &e=error; if (!(m_stateCaptureHook(*this,b,e))) { error="Native: "+error; return false; } }
    ps2state::Writer w(out,error); w.pod(uint32_t{0x31504353}); w.pod(uint32_t(chunks.size()));
    for (const auto &b:chunks) w.vector(b);
    return w.finish();
}
bool PS2Runtime::validateMachineState(const std::vector<uint8_t> &blob,std::string &error) {
    if (!m_stateValidateHook) { error="No native state decoder."; return false; }
    MachineChunks chunks; if (!decodeMachine(blob,chunks,error)) return false;
    const auto &paths=getIoPaths(); const PS2VfsMounts mounts{paths.hostRoot,paths.cdRoot,paths.mcRoot};
    { const auto &b=chunks[0]; auto &e=error; if (!(decodeRuntimeState(b,false,e))) { error="Runtime: "+error; return false; } }
    { const auto &b=chunks[1]; auto &e=error; if (!(m_eeScheduler->validateState(b,e))) { error="EE: "+error; return false; } }
    { const auto &b=chunks[2]; auto &e=error; if (!(m_memory.validateState(b,e))) { error="Memory: "+error; return false; } }
    { const auto &b=chunks[3]; auto &e=error; if (!(m_vu0.validateState(b,e))) { error="VU0: "+error; return false; } }
    { const auto &b=chunks[4]; auto &e=error; if (!(m_vu1.validateState(b,e))) { error="VU1: "+error; return false; } }
    { const auto &b=chunks[5]; auto &e=error; if (!(m_gs.validateState(b,e))) { error="GS: "+error; return false; } }
    { const auto &b=chunks[6]; auto &e=error; if (!(m_iopSubsystem->validateState(b,e))) { error="IOP: "+error; return false; } }
    { const auto &b=chunks[7]; auto &e=error; if (!(m_iopHost->validateState(b,e))) { error="IopHost: "+error; return false; } }
    { const auto &b=chunks[8]; auto &e=error; if (!(m_vfs.validateState(b,mounts,m_romDevice,e))) { error="VFS: "+error; return false; } }
    { const auto &b=chunks[9]; auto &e=error; if (!(ps2_stubs::validateCdState(b,e))) { error="Cd: "+error; return false; } }
    { const auto &b=chunks[10]; auto &e=error; if (!(ps2_stubs::validateMpegState(b,e))) { error="Mpeg: "+error; return false; } }
    { const auto &b=chunks[11]; auto &e=error; if (!(ps2_stubs::validateCardState(b,e))) { error="Card: "+error; return false; } }
    { const auto &b=chunks[12]; auto &e=error; if (!(ps2_stubs::validateAudioState(b,e))) { error="Audio: "+error; return false; } }
    { const auto &b=chunks[13]; auto &e=error; if (!(ps2_stubs::validateLibcState(b,e))) { error="Libc: "+error; return false; } }
    { const auto &b=chunks[14]; auto &e=error; if (!(ps2_syscalls::validateFioState(b,e))) { error="Fio: "+error; return false; } }
    { const auto &b=chunks[15]; auto &e=error; if (!(ps2_syscalls::validateKernelHostState(b,e))) { error="KernelHost: "+error; return false; } }
    { const auto &b=chunks[16]; auto &e=error; if (!(m_stateValidateHook(*this,b,e))) { error="Native: "+error; return false; } }
    return true;
}
bool PS2Runtime::restoreMachineState(const std::vector<uint8_t> &blob,std::string &error) {
    if (!validateMachineState(blob,error)) return false;
    MachineChunks chunks; if (!decodeMachine(blob,chunks,error)) return false;
    const auto &paths=getIoPaths(); const PS2VfsMounts mounts{paths.hostRoot,paths.cdRoot,paths.mcRoot};
    { const auto &b=chunks[2]; auto &e=error; if (!(m_memory.restoreState(b,e))) { error="Memory: "+error; return false; } }
    { const auto &b=chunks[3]; auto &e=error; if (!(m_vu0.restoreState(b,e))) { error="VU0: "+error; return false; } }
    { const auto &b=chunks[4]; auto &e=error; if (!(m_vu1.restoreState(b,e))) { error="VU1: "+error; return false; } }
    { const auto &b=chunks[5]; auto &e=error; if (!(m_gs.restoreState(b,e))) { error="GS: "+error; return false; } }
    { const auto &b=chunks[6]; auto &e=error; if (!(m_iopSubsystem->restoreState(b,e))) { error="IOP: "+error; return false; } }
    { const auto &b=chunks[7]; auto &e=error; if (!(m_iopHost->restoreState(b,e))) { error="IopHost: "+error; return false; } }
    { const auto &b=chunks[8]; auto &e=error; if (!(m_vfs.restoreState(b,mounts,m_romDevice,e))) { error="VFS: "+error; return false; } }
    { const auto &b=chunks[9]; auto &e=error; if (!(ps2_stubs::restoreCdState(b,e))) { error="Cd: "+error; return false; } }
    { const auto &b=chunks[10]; auto &e=error; if (!(ps2_stubs::restoreMpegState(b,e))) { error="Mpeg: "+error; return false; } }
    { const auto &b=chunks[11]; auto &e=error; if (!(ps2_stubs::restoreCardState(b,e))) { error="Card: "+error; return false; } }
    { const auto &b=chunks[12]; auto &e=error; if (!(ps2_stubs::restoreAudioState(b,e))) { error="Audio: "+error; return false; } }
    { const auto &b=chunks[13]; auto &e=error; if (!(ps2_stubs::restoreLibcState(b,e))) { error="Libc: "+error; return false; } }
    { const auto &b=chunks[14]; auto &e=error; if (!(ps2_syscalls::restoreFioState(b,e))) { error="Fio: "+error; return false; } }
    { const auto &b=chunks[15]; auto &e=error; if (!(ps2_syscalls::restoreKernelHostState(b,e))) { error="KernelHost: "+error; return false; } }
    { const auto &b=chunks[16]; auto &e=error; if (!(m_stateRestoreHook(*this,b,e))) { error="Native: "+error; return false; } }
    { const auto &b=chunks[0]; auto &e=error; if (!(decodeRuntimeState(b,true,e))) { error="Runtime: "+error; return false; } }
    { const auto &b=chunks[1]; auto &e=error; if (!(m_eeScheduler->restoreState(b,e))) { error="EE: "+error; return false; } }
    return true;
}

namespace {
    constexpr uint64_t kStateFileMagic=0x31545343544f5355ull;
    constexpr uint32_t kStateFileVersion=1;
    bool stateCoreIdentity(uint32_t &core,uint64_t &coreBytes,uint32_t &game,std::string &error) {
#if defined(__linux__) && !defined(__ANDROID__)
        static uint32_t cachedCore=0; static uint64_t cachedSize=0;
        if (!cachedSize) {
            std::error_code ec; cachedSize=std::filesystem::file_size("/proc/self/exe",ec);
            if (ec || !computeFileCrc32("/proc/self/exe",cachedCore)) { error="Cannot identify the current runtime."; cachedSize=0; return false; }
        }
        core=cachedCore; coreBytes=cachedSize;
        if (!computeFileCrc32(PS2Runtime::getIoPaths().elfPath.string(),game)) { error="Cannot identify the game disc."; return false; }
        return true;
#else
        error="Save states are currently supported on native Linux."; return false;
#endif
    }
    bool atomicStateFile(const std::filesystem::path &path,const std::vector<uint8_t> &bytes,std::string &error) {
#if defined(__linux__) && !defined(__ANDROID__)
        std::error_code ec; std::filesystem::create_directories(path.parent_path(),ec);
        if (ec) { error="Cannot create the save-state folder: "+ec.message(); return false; }
        static uint64_t sequence=0;
        const auto temporary=path.parent_path()/(".state-"+std::to_string(::getpid())+"-"+std::to_string(++sequence)+".tmp");
        const int fd=::open(temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if (fd<0) { error="Cannot create the temporary save-state file."; return false; }
        bool ok=true; size_t offset=0;
        while (offset<bytes.size()) {
            const auto count=::write(fd,bytes.data()+offset,bytes.size()-offset);
            if (count<0 && errno==EINTR) continue;
            if (count<=0) { ok=false; break; }
            offset+=static_cast<size_t>(count);
        }
        if (ok && ::fsync(fd)!=0) ok=false;
        if (::close(fd)!=0) ok=false;
        if (ok && ::rename(temporary.c_str(),path.c_str())!=0) ok=false;
        if (!ok) { ::unlink(temporary.c_str()); error="Save-state write failed; the previous state was preserved."; return false; }
        const int directory=::open(path.parent_path().c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
        if (directory<0) error="Estado gravado, mas não foi possível confirmar a persistência da pasta no disco.";
        else {
            if (::fsync(directory)!=0) error="Estado gravado, mas a confirmação de persistência da pasta falhou.";
            ::close(directory);
        }
        return true;
#else
        error="Save states are currently supported on native Linux."; return false;
#endif
    }
    bool readStateFile(const std::filesystem::path &path,std::vector<uint8_t> &payload,std::string &error) {
        std::error_code ec; const uint64_t size=std::filesystem::file_size(path,ec);
        if (ec) { error="Este slot ainda está vazio."; return false; }
        if (size<48 || size>ps2state::DefaultLimit+4096) { error="Invalid save-state file size."; return false; }
        std::ifstream input(path,std::ios::binary); std::vector<uint8_t> data(static_cast<size_t>(size));
        if (!input.read(reinterpret_cast<char *>(data.data()),data.size())) { error="Cannot read the save state."; return false; }
        ps2state::Reader r(data,error,ps2state::DefaultLimit+4096);
        uint64_t magic=0,coreBytes=0; uint32_t version=0,endian=0,context=0,core=0,game=0,crc=0; uint64_t stamp=0;
        r.pod(magic); r.pod(version); r.pod(endian); r.pod(context); r.pod(core); r.pod(coreBytes); r.pod(game); r.pod(stamp); r.pod(crc); r.vector(payload);
        if (!r.finish() || magic!=kStateFileMagic || version!=kStateFileVersion || endian!=0x12345678 || context!=sizeof(R5900Context))
        { error="Unsupported or damaged save-state format."; return false; }
        if (savestates::checksum(payload)!=crc) { error="Save-state checksum failed. No game data was changed."; return false; }
        uint32_t currentCore=0,currentGame=0; uint64_t currentSize=0;
        if (!stateCoreIdentity(currentCore,currentSize,currentGame,error)) return false;
        if (core!=currentCore || coreBytes!=currentSize || game!=currentGame)
        { error="Este estado pertence a outra versão do runtime ou do disco. Use o save normal do jogo."; return false; }
        return true;
    }
}
void PS2Runtime::processStateRequest()
{
    savestates::Action action; int slot=0;
    if (!savestates::take(action,slot)) return;
    struct TimingGuard { EeScheduler &scheduler; ~TimingGuard() { scheduler.rebaseHostClockAfterStateOperation(); } } timing{*m_eeScheduler};
    const auto path=savestates::directory()/(slot==0?"quick.sotcstate":"slot-"+std::to_string(slot)+".sotcstate");
    std::string error;
    try {
        if (action==savestates::Action::Delete) {
            std::error_code ec; const bool removed=std::filesystem::remove(path,ec);
            savestates::complete(ec?"Não foi possível remover o estado.":removed?"Estado removido.":"Este slot já está vazio."); return;
        }
        // Freeze the only external producer that can modify SG2/IOP state.
        std::lock_guard<std::mutex> audio(g_stateAudioMutex);
        std::vector<uint8_t> payload;
        if (action==savestates::Action::Save) {
            if (!captureMachineState(payload,error) || !validateMachineState(payload,error)) { savestates::complete("Não foi possível salvar: "+error); return; }
            uint32_t core=0,game=0; uint64_t coreBytes=0;
            if (!stateCoreIdentity(core,coreBytes,game,error)) { savestates::complete(error); return; }
            std::vector<uint8_t> file;
            ps2state::Writer w(file,error,ps2state::DefaultLimit+4096);
            w.pod(kStateFileMagic); w.pod(kStateFileVersion); w.pod(uint32_t{0x12345678}); w.pod(uint32_t(sizeof(R5900Context)));
            w.pod(core); w.pod(coreBytes); w.pod(game); w.pod(uint64_t(std::time(nullptr))); w.pod(savestates::checksum(payload)); w.vector(payload);
            if (!w.finish() || !atomicStateFile(path,file,error)) { savestates::complete("Não foi possível salvar: "+error); return; }
            savestates::complete(!error.empty()?error:slot==0?"Estado rápido salvo — F8 carrega.":"Estado salvo no slot "+std::to_string(slot)+".");
        } else {
            if (!readStateFile(path,payload,error) || !validateMachineState(payload,error)) { savestates::complete("Não foi possível carregar: "+error); return; }
            // A failed restore must not leave a half-restored running machine.
            std::vector<uint8_t> rollback;
            if (!captureMachineState(rollback,error)) { savestates::complete("Aguarde um ponto seguro para carregar: "+error); return; }
            bool restored=false;
            try { restored=restoreMachineState(payload,error); }
            catch (const std::exception &exception) { error=exception.what(); }
            if (!restored) {
                std::string rollbackError;
                bool rolledBack=false;
                try { rolledBack=restoreMachineState(rollback,rollbackError); }
                catch (const std::exception &exception) { rollbackError=exception.what(); }
                if (!rolledBack) {
                    requestStop(); savestates::complete("Falha ao restaurar o estado; o jogo foi encerrado para preservar os dados.");
                } else savestates::complete("Estado não carregado; a partida atual foi preservada: "+error);
                return;
            }
            desktop_input::resetAfterStateLoad();
            ps2pad::resetAfterStateLoad();
            savestates::complete(slot==0?"Estado rápido carregado.":"Estado carregado do slot "+std::to_string(slot)+".");
        }
    } catch (const std::exception &exception) {
        savestates::complete("Falha na operação de estado: "+std::string(exception.what()));
    }
}

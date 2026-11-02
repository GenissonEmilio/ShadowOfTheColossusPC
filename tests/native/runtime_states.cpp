// Coordinator/file-format integration with the REAL runtime subsystems.
// No ROM, window, GPU context, personal card, or game input is used. Only the
// game-specific native hook is a small synthetic state owner; GL history has
// its own real-renderer fixture. Requests use the production queue and run on
// the bound executor, outside guest execution, as the dispatcher would call it.
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "runtime/native_renderer.h"
#include "runtime/save_state_archive.h"
#include "runtime/state_manager.h"
#include "ps2x/iop/iop_subsystem.h"

#include <algorithm>
#include <array>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sys/resource.h>

namespace
{
    unsigned checks = 0;
    uint32_t nativeValue = 0;
    bool failNativeRestoreOnce = false;
    unsigned nativeRestores = 0;
    constexpr uint32_t kMarker = 0x1A0000u;
    constexpr uint32_t kSavedMarker = 0x11223344u;
    constexpr uint32_t kSavedPc = 0x300000u;

    void check(bool condition, const char *message)
    {
        ++checks;
        if (!condition)
        {
            std::fprintf(stderr, "FAIL: %s (status: %s)\n", message, savestates::status().c_str());
            std::fflush(nullptr);
            std::_Exit(1);
        }
    }

    bool captureNative(PS2Runtime &, std::vector<uint8_t> &out, std::string &error)
    {
        ps2state::Writer writer(out,error);
        return writer.pod(uint32_t{0x31585446}) && writer.pod(nativeValue) && writer.finish();
    }
    bool validateNative(PS2Runtime &, const std::vector<uint8_t> &data, std::string &error)
    {
        ps2state::Reader reader(data,error);
        uint32_t magic=0,value=0;
        if (!reader.pod(magic) || !reader.pod(value) || !reader.finish() || magic!=0x31585446)
            return reader.fail("invalid synthetic native owner");
        return true;
    }
    bool restoreNative(PS2Runtime &runtime, const std::vector<uint8_t> &data, std::string &error)
    {
        ++nativeRestores;
        if (!validateNative(runtime,data,error)) return false;
        if (failNativeRestoreOnce)
        {
            failNativeRestoreOnce=false;
            error="injected native commit failure";
            return false;
        }
        ps2state::Reader reader(data,error);
        uint32_t magic=0;
        return reader.pod(magic) && reader.pod(nativeValue) && reader.finish();
    }

    std::vector<uint8_t> readBytes(const std::filesystem::path &path)
    {
        std::ifstream input(path,std::ios::binary);
        check(input.good(),"open fixture file");
        return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
    }
    void writeBytes(const std::filesystem::path &path,const std::vector<uint8_t> &bytes)
    {
        std::ofstream output(path,std::ios::binary|std::ios::trunc);
        output.write(reinterpret_cast<const char *>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
        check(output.good(),"write fixture file");
    }

    std::unique_ptr<PS2Runtime> machine()
    {
        auto runtime=std::make_unique<PS2Runtime>();
        check(runtime->memory().initialize(),"allocate real runtime memory");
        check(runtime->syncCoreSubsystems(),"bind real GS/VU/IOP callbacks");
        R5900Context context;
        context.pc=kSavedPc;
        context.r[8]=_mm_set_epi64x(0x1234567890ABCDEFll,0x0FEDCBA098765432ll);
        runtime->cpu()=context;
        runtime->eeScheduler().reset(runtime->memory().getRDRAM(),context);
        // The native driver identifies this small synthetic buffer by its
        // own public loader. No proprietary IRX code is present or executed.
        std::array<uint8_t,128> image{};
        std::memcpy(image.data(),"\x7f" "ELF",4);
        const uint32_t end=128; const uint16_t stride=1, count=0;
        std::memcpy(image.data()+0x20,&end,4);
        std::memcpy(image.data()+0x2e,&stride,2);
        std::memcpy(image.data()+0x30,&count,2);
        std::memcpy(image.data()+0x40,"sg2iop_driver",13);
        check(runtime->iopSubsystem()->writeMemory(0x1000,image.data(),image.size()),"write synthetic native-driver identification");
        check(runtime->iopSubsystem()->loadModuleBuffer(0x1000).moduleId>0,"select real SG2 native service");
        check(runtime->iopSubsystem()->loadModule("rom0:LIBSD").moduleId>0,"initialize real HLE service routes");
        runtime->setStateHooks(captureNative,validateNative,restoreNative);
        native_renderer::setShown(true);
        savestates::bind(runtime.get());
        return runtime;
    }

    void operation(PS2Runtime &runtime,savestates::Action action,int slot=0)
    {
        check(!savestates::busy(),"request queue starts idle");
        savestates::request(action,slot);
        check(savestates::busy(),"production state request is queued");
        runtime.processStateRequest();
        check(!savestates::busy(),"coordinator completes the queued operation");
    }
    bool loaded()
    {
        const auto message=savestates::status();
        return message.starts_with("Estado rápido carregado.") || message.starts_with("Estado carregado do slot ");
    }
    bool saved()
    {
        const auto message=savestates::status();
        return message.starts_with("Estado rápido salvo") || message.starts_with("Estado salvo no slot ");
    }

    void writeSavedMachine(PS2Runtime &runtime)
    {
        runtime.memory().write32(kMarker,kSavedMarker);
        runtime.memory().write32(0x70000010,0xAABBCCDDu);
        runtime.vu0().state().q=2.5f;
        runtime.eeScheduler().accountCycles(321);
        runtime.eeScheduler().createSemaphore(3,5,0x11,0x22);
        nativeValue=0x55667788u;
    }
    void checkSavedMachine(PS2Runtime &runtime)
    {
        check(runtime.memory().read32(kMarker)==kSavedMarker,"RAM marker returns to the saved timeline");
        check(runtime.memory().read32(0x70000010)==0xAABBCCDDu,"scratchpad returns to the saved timeline");
        check(runtime.eeScheduler().thread(1)->context.pc==kSavedPc,"saved guest PC is restored");
        check(static_cast<uint64_t>(_mm_extract_epi64(runtime.eeScheduler().thread(1)->context.r[8],1))==0x1234567890ABCDEFull,
              "all 128-bit GPR lanes pass through the full file format");
        check(runtime.eeScheduler().eeCycle()==321,"guest clock is restored");
        check(runtime.vu0().state().q==2.5f,"VU state is restored");
        check(runtime.eeScheduler().semaphore(1) && runtime.eeScheduler().semaphore(1)->count==3,"kernel object state is restored");
        check(nativeValue==0x55667788u,"native owner is restored by its hook");
    }

    void failedLoadPreservesMachine(PS2Runtime &runtime,const std::filesystem::path &file,const std::vector<uint8_t> &invalid,const char *reason)
    {
        writeBytes(file,invalid);
        runtime.memory().write32(kMarker,0xDEADBEEFu);
        runtime.eeScheduler().thread(1)->context.pc=0x300040u;
        nativeValue=0xCAFEBABEu;
        const unsigned before=nativeRestores;
        operation(runtime,savestates::Action::Load);
        check(!loaded(),reason);
        check(runtime.memory().read32(kMarker)==0xDEADBEEFu && runtime.eeScheduler().thread(1)->context.pc==0x300040u,
              "invalid file does not mutate RAM or CPU context");
        check(nativeValue==0xCAFEBABEu && nativeRestores==before,"invalid file never enters native commit");
        check(!runtime.isStopRequested(),"preflight rejection leaves the current runtime usable");
    }

    void roundTripAndFiles(const std::filesystem::path &folder,std::vector<uint8_t> &golden)
    {
        auto runtime=machine();
        writeSavedMachine(*runtime);
        operation(*runtime,savestates::Action::Save);
        check(saved(),"Save reports success only after a complete file is written");
        const auto file=savestates::directory()/"quick.sotcstate";
        golden=readBytes(file);
        check(golden.size()>PS2_RAM_SIZE,"real memory and subsystem chunks are present");
        runtime->memory().write32(kMarker,99);
        runtime->memory().write32(0x70000010,0);
        runtime->eeScheduler().thread(1)->context.pc=0x300080u;
        runtime->eeScheduler().accountCycles(57);
        runtime->eeScheduler().pollSemaphore(1);
        runtime->vu0().state().q=9;
        nativeValue=7;
        operation(*runtime,savestates::Action::Load);
        check(loaded(),"Load succeeds through the coordinator");
        checkSavedMachine(*runtime);

        auto bad=golden; bad.resize(bad.size()-17);
        failedLoadPreservesMachine(*runtime,file,bad,"truncated full state is rejected");
        bad=golden; bad[44]^=0x80; // checksum field, not the checksum implementation
        failedLoadPreservesMachine(*runtime,file,bad,"incorrect payload CRC is rejected");
        bad=golden; bad[20]^=0x80; // executable/core identity field
        failedLoadPreservesMachine(*runtime,file,bad,"different executable/core identity is rejected");
        writeBytes(file,golden);

        // Fail AFTER RAM was applied, then prove that the production global
        // rollback really reconstructs the earlier machine from its archive.
        runtime->memory().write32(kMarker,0x13572468u);
        runtime->eeScheduler().thread(1)->context.pc=0x3000A0u;
        nativeValue=0x24681357u;
        failNativeRestoreOnce=true;
        const unsigned before=nativeRestores;
        operation(*runtime,savestates::Action::Load);
        check(!loaded() && nativeRestores==before+2,"failed native commit invokes a successful rollback");
        check(runtime->memory().read32(kMarker)==0x13572468u && runtime->eeScheduler().thread(1)->context.pc==0x3000A0u &&
              nativeValue==0x24681357u && !runtime->isStopRequested(),"rollback restores RAM/context/native owner and keeps runtime usable");

        // RLIMIT_FSIZE/SIGXFSZ are process-local. Force the real temporary-file
        // write to fail, even when this fixture is executed as root, without
        // replacing atomicStateFile or depending on filesystem permissions.
        struct rlimit previous;
        check(::getrlimit(RLIMIT_FSIZE,&previous)==0,"read per-process write limit");
        const auto previousSignal=std::signal(SIGXFSZ,SIG_IGN);
        struct rlimit limited=previous;
        limited.rlim_cur=std::min<rlim_t>(previous.rlim_max,1024);
        check(::setrlimit(RLIMIT_FSIZE,&limited)==0,"activate bounded write failure");
        operation(*runtime,savestates::Action::Save);
        check(::setrlimit(RLIMIT_FSIZE,&previous)==0,"restore per-process write limit");
        std::signal(SIGXFSZ,previousSignal);
        check(!saved(),"failed temporary write is not reported as saved");
        check(readBytes(file)==golden,"failed write preserves every byte of the previous slot");
        for (const auto &entry:std::filesystem::directory_iterator(savestates::directory()))
            check(entry.path().extension()!=".tmp","failed atomic write removes its private temporary file");
        savestates::bind(nullptr);
        std::puts("PASS: coordinator Save/Load, full memory/context, malformed file gates, rollback and failed-write preservation");
    }

    void newOwnerAndGates(const std::filesystem::path &folder,const std::vector<uint8_t> &golden)
    {
        auto runtime=machine();
        runtime->memory().write32(kMarker,0x11111111u);
        nativeValue=0;
        operation(*runtime,savestates::Action::Load);
        check(loaded(),"saved file restores into a new PS2Runtime owner");
        checkSavedMachine(*runtime);
        const auto file=savestates::directory()/"quick.sotcstate";
        const auto card=folder/"cards"/"Mcd001.ps2";
        {
            std::fstream data(card,std::ios::binary|std::ios::in|std::ios::out);
            data.seekp(12345); data.put(0x5a); data.flush();
            check(data.good(),"alter only the synthetic card fixture");
        }
        runtime->memory().write32(kMarker,0x22222222u);
        operation(*runtime,savestates::Action::Load);
        check(!loaded() && runtime->memory().read32(kMarker)==0x22222222u,"changed memory card is rejected without rewinding live RAM");
        check(readBytes(file)==golden,"card mismatch does not change the state slot");

        // A decoded legacy VAG bank must be rejected; it is not the native
        // SG2 mixer saved by this format. No audio device is initialized.
        std::array<uint8_t,64> vag{};
        std::memcpy(vag.data(),"VAGp",4);
        vag[15]=16; // big-endian ADPCM payload bytes
        vag[18]=0xAC; vag[19]=0x44; // big-endian 44100 Hz
        vag[49]=1; // one ADPCM block with end flag
        runtime->audioBackend().onVagTransferFromBuffer(vag.data(),vag.size(),0x1000);
        std::string error;
        check(!runtime->audioBackend().stateCaptureSupported(error),"synthetic VAG populated the legacy backend guard");
        operation(*runtime,savestates::Action::Save);
        check(!saved() && readBytes(file)==golden,"unsupported legacy audio cannot replace a valid slot");
        savestates::bind(nullptr);
        std::puts("PASS: new runtime owner, changed-card gate and legacy-audio gate preserve current state and files");
    }
}

int main(int argc,char **argv)
{
    check(argc==2,"provide an isolated fixture folder");
    const auto folder=std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(folder/"disc");
    std::filesystem::create_directories(folder/"cards");
    std::filesystem::create_directories(folder/"states");
    setenv("SOTC_NATIVE","1",1); setenv("PS2X_NATIVE_IOP_BOOT","1",1); setenv("PS2X_NATIVE_SG2","1",1);
    setenv("PS2X_MTVU","0",1); setenv("PS2X_GIF_THREAD","0",1); setenv("PS2X_GS_HW","0",1); setenv("PS2X_GS_NODRAW","1",1);
    setenv("PS2X_MEMCARD_DIR",(folder/"cards").c_str(),1); setenv("PS2X_STATE_DIR",(folder/"states").c_str(),1);
    unsetenv("PS2X_INPUT_SCRIPT"); unsetenv("SOTC_DIFFTEST");
    std::vector<uint8_t> identity(256,0x5a);
    std::memcpy(identity.data(),"synthetic-state-fixture-only",27);
    writeBytes(folder/"disc"/"fixture.elf",identity);
    writeBytes(folder/"cards"/"Mcd001.ps2",std::vector<uint8_t>(16384u*528u,0xff));
    PS2Runtime::IoPaths paths;
    paths.elfPath=folder/"disc"/"fixture.elf"; paths.elfDirectory=paths.hostRoot=paths.cdRoot=folder/"disc"; paths.mcRoot=folder/"cards";
    PS2Runtime::setIoPaths(paths);
    std::vector<uint8_t> golden;
    roundTripAndFiles(folder,golden);
    newOwnerAndGates(folder,golden);
    std::printf("PASS: %u full-runtime/file checks; synthetic VM, no ROM, window, GPU context or personal files\n",checks);
}

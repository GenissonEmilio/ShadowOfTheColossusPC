// Deterministic non-game fixtures: streamed PCM/callback restoration and VFS.
#include "runtime/ps2_vfs.h"
#include "runtime/ps2_rom_device.h"
#include "runtime/ps2_memory.h"
#include "runtime/save_state_archive.h"
#include "emulator/core/iop_memory.h"
#include "emulator/services/iop_sg2.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace
{
    using namespace ps2x::iop;
    using namespace ps2x::iop::detail;
    struct FixtureHost final : IopHost
    {
        std::vector<uint8_t> guest = std::vector<uint8_t>(1u << 20);
        bool range(uint32_t address, size_t size) const { return address <= guest.size() && size <= guest.size() - address; }
        bool readGuest(uint32_t a, void *d, size_t n) const override
        { if (!range(a,n)) return false; std::memcpy(d,guest.data()+a,n); return true; }
        bool writeGuest(uint32_t a, const void *s, size_t n) override
        { if (!range(a,n)) return false; std::memcpy(guest.data()+a,s,n); return true; }
        bool zeroGuest(uint32_t a, size_t n) override
        { if (!range(a,n)) return false; std::memset(guest.data()+a,0,n); return true; }
        bool normalizeGuestAddress(uint32_t a, uint32_t &b) const override { b=a; return range(a,1); }
        uint32_t allocateIopHandle(IopHandleKind) override { return 1; }
        uint32_t allocateGuest(uint32_t, uint32_t) override { return 0x80000; }
        void freeGuest(uint32_t) override {}
        void audioCommand(uint32_t, uint32_t, GuestBuffer, GuestBuffer) override {}
        std::string hostPath(HostPathKind) const override { return {}; }
        std::string translateGuestPath(std::string_view) const override { return {}; }
        uint64_t openHostFile(std::string_view) override { return 0; }
        bool hostFileSize(uint64_t, uint64_t &) const override { return false; }
        bool readHostFile(uint64_t, uint64_t, void *, size_t, size_t &) override { return false; }
        void closeHostFile(uint64_t) override {}
        int32_t memoryCard(const MemoryCardRequest &) override { return -1; }
        bool hasGuestFunction(uint32_t) const override { return false; }
        bool invokeGuestFunction(uint64_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t *) override { return false; }
        void log(LogLevel, std::string_view) override {}
    };

    void send(FixtureHost &host, Sg2Driver &driver, std::initializer_list<std::array<uint32_t,4>> commands)
    {
        uint32_t cursor=0x10000;
        for (const auto &command:commands) { assert(host.writeGuest(cursor,command.data(),16)); cursor+=16; }
        RpcRequest request;
        request.sid=Sg2Driver::Sid; request.function=0;
        request.send={0x10000,static_cast<uint32_t>(commands.size()*16)};
        request.receive={0x18000,400};
        assert(driver.handleRpc(request).handled);
    }

    void checkSg2()
    {
        FixtureHost host;
        IopMemory memory;
        Sg2Driver driver(host,memory);
        std::array<int16_t,2048> pcm{};
        for (size_t block=0;block<4;++block)
            for (size_t sample=0;sample<256;++sample)
            {
                pcm[block*512+sample]=static_cast<int16_t>(1000+block*700+sample);
                pcm[block*512+256+sample]=static_cast<int16_t>(-2000-static_cast<int>(block*500+sample));
            }
        assert(memory.writeRam(0x1000,pcm.data(),sizeof(pcm)));
        // Master volume is signed 15-bit fixed/sweep encoding: 0x3fff is
        // positive full scale; 0x7fff would decode to -2 and mute this fixture.
        send(host,driver,{{0x11,0,0,0},{0x06,0,0x3fff,0x3fff},{0x06,1,0x3fff,0x3fff},
                          {0x22,0x7fff,0x7fff,0},{0x20,0x1000,sizeof(pcm),0}});
        std::vector<int16_t> warm(317*2), expected(1500*2), actual(1500*2);
        assert(driver.mixer().readOutput(warm.data(),317)==317);
        std::string error;
        std::vector<uint8_t> state;
        assert(driver.captureState(state,error));
        assert(driver.mixer().readOutput(expected.data(),1500)==1500);
        assert(std::any_of(expected.begin(),expected.end(),[](int16_t value){return value!=0;}));
        send(host,driver,{{0x1f,0,0,0}}); // abandon PCM/refill callback on the later timeline
        assert(driver.restoreState(state,error));
        assert(driver.mixer().readOutput(actual.data(),1500)==1500);
        assert(actual==expected); // crosses multiple refill callbacks, not just the first buffered block

        std::vector<uint8_t> before, after;
        assert(driver.captureState(before,error));
        for (const size_t length:{size_t{0},size_t{7},state.size()/2,state.size()-1})
        {
            const std::vector<uint8_t> broken(state.begin(),state.begin()+length);
            assert(!driver.restoreState(broken,error));
            assert(driver.captureState(after,error));
            assert(after==before);
        }
        auto badMagic=state; badMagic[0]^=0xff;
        assert(!driver.validateState(badMagic,error));
        assert(driver.captureState(after,error) && after==before);
        std::puts("PASS: SG2 PCM, multiple refill callbacks, mixer continuation and atomic corruption rejection");
    }

    std::vector<uint8_t> invalidWritableDescriptor(const std::string &path)
    {
        ps2state::Writer writer;
        writer.pod(uint32_t{0x31534656}); writer.pod(int32_t{4}); writer.pod(uint32_t{1});
        writer.pod(int32_t{3}); writer.string(path);
        writer.pod(uint32_t{PS2_FIO_O_RDWR|PS2_FIO_O_TRUNC}); writer.pod(int64_t{0});
        return std::move(writer.data);
    }

    void checkVfs(const std::filesystem::path &folder)
    {
        std::filesystem::create_directories(folder);
        const std::string contents="0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        std::ofstream(folder/"read.bin",std::ios::binary)<<contents;
        std::ofstream(folder/"preserve.bin",std::ios::binary)<<"PRESERVE";
        const PS2VfsMounts mounts{folder,folder,folder};
        PS2RomDevice rom;
        PS2Vfs source;
        const int descriptor=source.open("host0:read.bin",PS2_FIO_O_RDONLY,mounts,rom);
        assert(descriptor>=3);
        std::array<char,7> bytes{};
        assert(source.read(descriptor,bytes.data(),3)==3);
        std::vector<uint8_t> state;
        std::string error;
        assert(source.captureState(state,error));
        assert(source.read(descriptor,bytes.data(),7)==7 && std::string(bytes.data(),7)=="3456789");
        PS2Vfs restored;
        assert(restored.restoreState(state,mounts,rom,error));
        assert(restored.read(descriptor,bytes.data(),7)==7 && std::string(bytes.data(),7)=="3456789");
        const int next=restored.open("host0:read.bin",PS2_FIO_O_RDONLY,mounts,rom);
        assert(next==descriptor+1);
        assert(restored.close(next)==0);
        std::vector<uint8_t> before,after;
        assert(restored.captureState(before,error));
        auto broken=state; broken.pop_back();
        assert(!restored.restoreState(broken,mounts,rom,error));
        assert(restored.captureState(after,error) && after==before);
        assert(!restored.restoreState(invalidWritableDescriptor("host0:preserve.bin"),mounts,rom,error));
        std::ifstream preserved(folder/"preserve.bin",std::ios::binary);
        assert(std::string(std::istreambuf_iterator<char>(preserved),{})=="PRESERVE");
        const int writable=restored.open("host0:preserve.bin",PS2_FIO_O_RDWR,mounts,rom);
        assert(writable>=3);
        std::vector<uint8_t> old{0x12,0x34};
        assert(!restored.captureState(old,error) && old==std::vector<uint8_t>({0x12,0x34}));
        assert(restored.close(writable)==0);
        std::filesystem::rename(folder/"read.bin",folder/"read.hidden");
        assert(!restored.restoreState(state,mounts,rom,error));
        std::filesystem::rename(folder/"read.hidden",folder/"read.bin");
        assert(restored.read(descriptor,bytes.data(),3)==3 && std::string(bytes.data(),3)=="ABC");
        std::puts("PASS: VFS descriptor/offset continuation, corruption and missing-file rejection, no truncation or partial mutation");
    }
}

int main(int argc,char **argv)
{
    assert(argc==2);
    setenv("PS2X_AUDIO_PULL","1",1);
    unsetenv("PS2X_SPU2_WAV");
    checkSg2();
    checkVfs(argv[1]);
}

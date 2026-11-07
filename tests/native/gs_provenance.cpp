#include "ps2_vif1_compact.h"
#include "runtime/gs/gs_provenance_ledger.h"
#include "runtime/ps2_stats.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

using namespace gs_provenance;
using Bytes=std::vector<uint8_t>;
using Write=std::pair<uint8_t,uint64_t>;
static unsigned checks=0;
static void require(bool ok,const char *message) { ++checks; if(!ok)throw std::runtime_error(message); }
template<class T> static void put(Bytes &b,T value)
{
    const auto *p=reinterpret_cast<const uint8_t *>(&value); b.insert(b.end(),p,p+sizeof(value));
}
constexpr uint64_t Frame=0x00080034, Zbuf=0x131000020ull, XY=0x800000008000ull, Scissor=0x01BF000001FF0000ull;
static std::vector<Write> root()
{
    return {{0x1A,1},{0x4C,Frame},{0x4D,Frame+1},{0x4E,Zbuf},{0x4F,Zbuf},
        {0x18,XY},{0x19,XY},{0x40,Scissor},{0x41,Scissor}};
}
static std::vector<Write> setup() { return {{0x47,0x5164A},{0x48,0}}; }
static std::vector<Write> triangle(unsigned context=0)
{
    return {{0,3|(uint64_t(context)<<9)},{5,0x0000003C80008000ull},
        {5,0x0000003C80008100ull},{5,0x0000003C81008000ull}};
}
static Bytes gif(const std::vector<Write> &writes)
{
    Bytes b; put(b,uint64_t(writes.size())|(uint64_t{1}<<15)|(uint64_t{1}<<60)); put(b,uint64_t{0xE});
    for(auto [reg,value]:writes){put(b,value);put(b,uint64_t(reg));} return b;
}
static Bytes direct(const std::vector<Write> &writes)
{
    Bytes body=gif(writes),b(12,0);put(b,uint32_t{0xD0000000}|uint32_t(body.size()/16));
    b.insert(b.end(),body.begin(),body.end());return b;
}
struct Capture final:Observer
{
    ConservativeLedger ledger;
    std::vector<CompactedSpan> spans;
    std::vector<Source> origins;
    std::vector<Gap> reasons;
    std::vector<Completion> completions;
    bool wrongSeal=false;
    void reset(uint64_t epoch) noexcept override { ledger.reset(epoch); }
    void begin(const Submission &s) noexcept override { ledger.begin(s); }
    void write(const Source&s,uint8_t reg,uint64_t v) noexcept override { origins.push_back(s);ledger.write(s,reg,v); }
    void gap(const Source&s,Gap reason) noexcept override { reasons.push_back(reason);ledger.gap(s,reason); }
    void compacted(const CompactedSpan&s) noexcept override { spans.push_back(s); }
    void end(const Completion&c) noexcept override
    {
        if(c.submission.frameBoundary && ps2_stats::vif1Chains.load()!=c.submission.token.frame)wrongSeal=true;
        completions.push_back(c);ledger.end(c);
    }
};
static Submission submission(uint64_t serial=1,uint64_t frame=42)
{
    Submission s; s.token={9,serial,frame};s.channel=0x10009000;s.mode=1;s.synchronous=true;s.ordered=true;return s;
}
static Source source(const Submission &s,uint32_t address,uint64_t visit=1)
{
    Source o;o.token=s.token;o.domain=Domain::Ram;o.address=address;o.tagVisit=visit;return o;
}
static SubmissionLedger take(Capture &capture)
{
    SubmissionLedger result;require(capture.ledger.takeCompleted(result),"completed ledger expected");return result;
}
static const VertexSnapshot &at(const SubmissionLedger &result,Domain domain,uint32_t address)
{
    const VertexSnapshot*v=nullptr;
    require(ConservativeLedger::findKick(result,domain,address,v)==Match::Unique,"unique physical kick expected");return *v;
}
static void parserCase(bool compact,bool split)
{
    Capture capture; auto info=submission();capture.begin(info);
    std::vector<Write> eventOracle{{0x60,0xA1},{0x61,0xB2},{0x62,0xC3}},observedEvents,legacyEvents;
    GifEventScanner::WriteFn ow=[&](uint8_t r,uint64_t v){observedEvents.emplace_back(r,v);};
    GifEventScanner::WriteFn lw=[&](uint8_t r,uint64_t v){legacyEvents.emplace_back(r,v);};
    GifEventScanner observed(ow,&capture),legacy(lw);
    VIFRegisters regs{},oldRegs{}; Vif1Walker::Path3Mask mask=[](bool){};
    Vif1Walker walker(regs,observed,mask),oldWalker(oldRegs,legacy,mask);
    Bytes output,oldOutput; Vif1Compactor compactor(0,output,observed),oldCompactor(0,oldOutput,legacy);
    auto feed=[&](const Bytes&bytes,uint32_t address) {
        auto one=[&](uint32_t offset,uint32_t count) {
            if(compact){compactor.feedObserved(bytes.data()+offset,count,source(info,address+offset));oldCompactor.feed(bytes.data()+offset,count);}
            else{walker.feedObserved(bytes.data()+offset,count,source(info,address+offset));oldWalker.feed(bytes.data()+offset,count);}
        };
        if(split && bytes.size()>32){one(0,20);one(20,7);one(27,bytes.size()-27);}else one(0,bytes.size());
    };
    feed(direct(root()),0x2000);feed(direct(setup()),0x3000);
    auto a=triangle();a.push_back(eventOracle[0]);feed(direct(a),0x4000);
    feed(direct(triangle(1)),0x4500);
    feed(direct({{0x4C,0x40061},{0x18,0x780000007800ull},{0x40,0xFF000000FF0000ull}}),0x5000);
    auto b=triangle();b.push_back(eventOracle[1]);feed(direct(b),0x6000);
    feed(direct(root()),0x7000);auto c=triangle();c.push_back(eventOracle[2]);feed(direct(c),0x8000);
    bool complete=observed.complete()&&(compact?compactor.complete():walker.complete());
    capture.end({info,complete});auto result=take(capture);
    require(result.completion.complete,"complete real parser scan");
    require(observedEvents==eventOracle && legacyEvents==eventOracle,"events 60/61/62 independent oracle");
    require(compact?output==oldOutput:std::memcmp(&regs,&oldRegs,sizeof(regs))==0,"observer leaves VIF output unchanged");
    require(capture.reasons.empty(),"supported parser stream has no hidden gap");
    constexpr uint32_t kickOffset=16+16+3*16;
    const auto &va=at(result,Domain::Ram,0x4000+kickOffset);
    const auto &vb=at(result,Domain::Ram,0x6000+kickOffset);
    const auto &vc=at(result,Domain::Ram,0x8000+kickOffset);
    require(va.known&&vc.known&&va.bank.frame==Frame&&vc.bank.frame==Frame,"A/C inherit main and restoration");
    require(vb.known&&vb.bank.frame==0x40061&&vb.bank.xyoffset!=XY,"B retains actual offscreen snapshot");
    const auto &hud=at(result,Domain::Ram,0x4500+kickOffset);
    require(hud.known&&hud.context==1&&hud.bank.test==0,"context2 TEST independent");
    if(compact)
    {
        require(!capture.spans.empty()&&capture.spans.front().source.address==0x2000,"retained span keeps physical source");
        require(std::any_of(capture.spans.begin(),capture.spans.end(),[](auto&s){return s.source.synthetic;}),"NOP substitute marked synthetic");
    }
}
static void gapsAndEpochs()
{
    Capture capture;auto info=submission();capture.begin(info);
    GifEventScanner::WriteFn write=[](uint8_t,uint64_t){};GifEventScanner scanner(write,&capture);
    auto feed=[&](auto writes,uint32_t address){auto bytes=gif(writes);scanner.feedObserved(bytes.data(),bytes.size(),source(info,address));};
    feed(root(),0x100);feed(setup(),0x300);
    scanner.observeVif(0x14,source(info,0x400),false);feed(triangle(),0x500);
    feed(root(),0x700);feed(setup(),0x900);feed(triangle(),0xB00);
    capture.end({info,true});auto result=take(capture);
    require(!at(result,Domain::Ram,0x500+16+3*16).known,"MSCAL invalidates");
    require(at(result,Domain::Ram,0xB00+16+3*16).known,"full restoration revalidates");
    // Identical physical packets in different CALL executions are ambiguous.
    info=submission(2);capture.begin(info);
    auto tri=gif(triangle());scanner.feedObserved(tri.data(),tri.size(),source(info,0x1000,2));
    scanner.feedObserved(tri.data(),tri.size(),source(info,0x1000,8));capture.end({info,true});result=take(capture);
    const VertexSnapshot*v=nullptr;
    require(ConservativeLedger::findKick(result,Domain::Ram,0x1000+16+3*16,v)==Match::Ambiguous,"repeated CALL must not select latest state");
    // A frame token is part of every source, and reset discards all old banks.
    info=submission(3,43);++info.token.epoch;capture.begin(info);
    scanner.feedObserved(tri.data(),tri.size(),source(info,0x2000));capture.end({info,true});result=take(capture);
    require(!at(result,Domain::Ram,0x2000+16+3*16).known,"epoch reset cannot inherit banks");
    // A completed partial scan cannot authorize even vertices seen before EOF.
    info=submission(4,44);capture.begin(info);feed(root(),0x3000);feed(setup(),0x4000);feed(triangle(),0x5000);
    capture.end({info,false});result=take(capture);
    require(!at(result,Domain::Ram,0x5000+16+3*16).known,"incomplete submission fails closed");
    info=submission(5);capture.begin(info);feed(root(),0x6000);feed(setup(),0x7000);
    scanner.observeVif(0x3F,source(info,0x8000),false);feed(triangle(),0x9000);capture.end({info,true});result=take(capture);
    require(!at(result,Domain::Ram,0x9000+16+3*16).known,"unknown opcode invalidates");
    // A split kick whose bytes come from different physical spans is not a
    // unique RAM source. Legacy event decoding still runs unchanged.
    info=submission(6);capture.begin(info);
    auto bytes=gif({{5,0x3C80008000ull},{0x60,123}});
    scanner.feedObserved(bytes.data(),20,source(info,0xA000));
    scanner.feedObserved(bytes.data()+20,bytes.size()-20,source(info,0xB000));capture.end({info,true});result=take(capture);
    require(result.gaps>0,"noncontiguous qword produces provenance gap");
}

static void budgetAndLateCases()
{
    Capture capture; auto info=submission(); capture.begin(info);
    GifEventScanner::WriteFn write=[](uint8_t,uint64_t){}; GifEventScanner scanner(write,&capture);
    auto feed=[&](const std::vector<Write>&writes,uint32_t address) {
        auto bytes=gif(writes); scanner.feedObserved(bytes.data(),bytes.size(),source(info,address));
    };
    feed(root(),0x100); feed(setup(),0x300); feed(triangle(),0x500);
    capture.end({info,true}); auto sealed=take(capture);
    require(at(sealed,Domain::Ram,0x500+16+3*16).known,"late test starts from known sealed snapshot");
    auto old=info; info=submission(2,43); capture.begin(info);
    capture.ledger.write(source(old,0x900),0x4C,0x40061);
    capture.ledger.gap(source(old,0x910),Gap::UnknownVif);
    capture.ledger.end({old,false});
    feed(triangle(),0x1000);capture.end({info,true});auto current=take(capture);
    const auto &v=at(current,Domain::Ram,0x1000+16+3*16);
    require(v.known&&v.bank.frame==Frame,"late write/gap/end cannot mutate active next submission");
    require(at(sealed,Domain::Ram,0x500+16+3*16).known,"late callbacks cannot mutate owned seal");
    SubmissionLedger extra;require(!capture.ledger.takeCompleted(extra),"late end cannot enqueue a stale seal");

    info=submission(3,44);capture.begin(info);feed(root(),0x2000);feed(setup(),0x3000);feed({{0,3}},0x4000);
    std::vector<Write> tooMany(ConservativeLedger::MaxVertices+1,{5,0x0000003C80008000ull});
    feed(tooMany,0x5000);capture.end({info,true});auto overflow=take(capture);
    require(overflow.overflow&&overflow.gaps>0,"vertex budget reports overflow/gap");
    require(overflow.vertices.size()==ConservativeLedger::MaxVertices,"vertex storage stays bounded");
    require(std::none_of(overflow.vertices.begin(),overflow.vertices.end(),[](auto &x){return x.known;}),"overflow invalidates the whole submission");

    ConservativeLedger ring;
    for(uint64_t serial=1;serial<=ConservativeLedger::MaxCompleted+2;++serial)
    {
        auto item=submission(serial);ring.begin(item);ring.end({item,true});
    }
    require(ring.dropped()==2,"completed ring counts dropped oldest submissions");
    uint64_t expected=3;unsigned count=0;
    while(ring.takeCompleted(extra)){require(extra.completion.submission.token.submission==expected++,"ring retains submission order");++count;}
    require(count==ConservativeLedger::MaxCompleted,"completed ring remains bounded");
}

#ifdef SOTC_PROVENANCE_MEMORY_TEST
extern "C" bool __wrap__ZN15native_renderer5shownEv(){return true;}
static void memoryCase()
{
    PS2Memory memory;require(memory.initialize(),"synthetic PS2Memory initialize");
    std::vector<Write> events;
    memory.setGsEventWriter([&](uint8_t r,uint64_t v){events.emplace_back(r,v);});
    Capture capture;memory.setGsProvenanceObserver(&capture);
    auto *ram=memory.getRDRAM();
    auto packet=[&](uint32_t address,Bytes bytes) {
        uint64_t tag=uint64_t(bytes.size()/16)|(uint64_t{6}<<28);std::memcpy(ram+address,&tag,8);
        std::memset(ram+address+8,0,8);std::memcpy(ram+address+16,bytes.data(),bytes.size());
    };
    // CPU generation order is deliberately B,C,off,setup,A,root.
    auto b=triangle();b.emplace_back(0x61,0xB2);packet(0x6000,direct(b));
    auto c=triangle();c.emplace_back(0x62,0xC3);packet(0x7000,direct(c));
    packet(0x5000,direct({{0x4C,0x40061}}));packet(0x3000,direct(setup()));
    auto a=triangle();a.emplace_back(0x60,0xA1);packet(0x4000,direct(a));packet(0x2000,direct(root()));
    auto chain=[&](std::vector<uint32_t> addresses,bool head) {
        uint32_t cursor=0x1000;
        for(auto address:addresses)
        {
            if(head){uint64_t tag=(uint64_t{2}<<28)|(uint64_t(cursor+0x40)<<32);std::memcpy(ram+cursor,&tag,8);std::memset(ram+cursor+8,0,8);cursor+=0x40;}
            uint64_t tag=(uint64_t{5}<<28)|(uint64_t(address)<<32);std::memcpy(ram+cursor,&tag,8);std::memset(ram+cursor+8,0,8);cursor+=16;
        }
        uint64_t end=uint64_t{7}<<28;std::memcpy(ram+cursor,&end,8);std::memset(ram+cursor+8,0,8);
    };
    ps2_stats::vif1Chains.store(100);
    chain({0x2000,0x3000,0x4000,0x5000,0x6000,0x2000,0x7000},true);
    memory.hostDmaStart(0x10009000,0x145,0,0,0x1000);
    auto first=take(capture);require(!capture.wrongSeal,"end callback precedes vif1Chains increment");
    require(first.completion.submission.token.frame==100&&ps2_stats::vif1Chains.load()==101,"exact native frame token N");
    require(first.completion.complete&&first.completion.submission.ordered,"real chain scan complete and ordered");
    constexpr uint32_t kick=16+16+16+3*16;
    require(at(first,Domain::Ram,0x4000+kick).known,"real DMA A known");
    require(at(first,Domain::Ram,0x6000+kick).bank.frame==0x40061,"real DMA offscreen state");
    require(at(first,Domain::Ram,0x7000+kick).bank.frame==Frame,"real DMA root restoration");
    require(events==std::vector<Write>{{0x60,0xA1},{0x61,0xB2},{0x62,0xC3}},"real DMA events unchanged");
    // The same addresses occur twice in the real CALL traversal.
    chain({0x2000,0x3000,0x4000,0x5000,0x4000},false);
    memory.hostDmaStart(0x10009000,0x145,0,0,0x1000);auto repeated=take(capture);
    const VertexSnapshot *v=nullptr;
    require(ConservativeLedger::findKick(repeated,Domain::Ram,0x4000+kick,v)==Match::Ambiguous,"real repeated CALL ambiguous");
    require(at(first,Domain::Ram,0x4000+kick).bank.frame==Frame,"sealed first frame immutable");
    require(repeated.completion.submission.token.frame==101,"second submission gets next frame");
    if(std::getenv("PS2X_VIF1_FULL") && std::string(std::getenv("PS2X_VIF1_FULL"))=="1")
        require(std::any_of(capture.spans.begin(),capture.spans.end(),[](auto&s){return s.source.tagData;}),"real TTE origin reaches compactor spans");
    // A RET+IRQ stops at a complete GIF boundary but before the rest of the
    // chain. Any vertices already seen belong to an incomplete submission.
    chain({0x2000,0x3000,0x4000,0x5000,0x6000,0x2000,0x7000},false);
    uint64_t irqTag;std::memcpy(&irqTag,ram+0x4000,8);irqTag|=uint64_t{1}<<31;std::memcpy(ram+0x4000,&irqTag,8);
    memory.hostDmaStart(0x10009000,0x1C5,0,0,0x1000);auto interrupted=take(capture);
    require(!interrupted.completion.complete,"IRQ/TIE cannot claim logical chain completion");
    require(!at(interrupted,Domain::Ram,0x4000+kick).known,"IRQ/TIE invalidates vertices observed before the stop");
    require(ConservativeLedger::findKick(interrupted,Domain::Ram,0x7000+kick,v)==Match::Missing,"IRQ/TIE does not fabricate continuation kicks");
    require(std::find(capture.reasons.begin(),capture.reasons.end(),Gap::Truncated)!=capture.reasons.end(),"IRQ/TIE emits explicit truncation gap");
    require(!capture.wrongSeal&&interrupted.completion.submission.token.frame==102,"IRQ/TIE seal retains pre-increment frame");
    packet(0x4000,direct(a));
    // Observer absent: the identical chain retains identical legacy events.
    memory.setGsProvenanceObserver(nullptr);events.clear();
    chain({0x2000,0x3000,0x4000,0x5000,0x6000,0x2000,0x7000},false);
    memory.hostDmaStart(0x10009000,0x145,0,0,0x1000);
    require(events==std::vector<Write>{{0x60,0xA1},{0x61,0xB2},{0x62,0xC3}},"no observer control actual memory path");
}
#endif
int main()
{
    try
    {
        for(bool compact:{false,true})for(bool split:{false,true})parserCase(compact,split);
        gapsAndEpochs();budgetAndLateCases();
#ifdef SOTC_PROVENANCE_MEMORY_TEST
        memoryCase();
#endif
        std::cout<<"PASS "<<checks<<" GS provenance assertions (real runtime parsers)\n";
    }
    catch(const std::exception&e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<"\n";return 1;}
}

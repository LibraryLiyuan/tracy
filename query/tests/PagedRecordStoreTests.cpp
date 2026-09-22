#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#if __has_include("TracyPagedRecordStore.hpp")
#include "TracyPagedRecordStore.hpp"
#include "TracyPolicyFrameSeries.hpp"
using namespace tracy::analysis;
static void Require(bool ok,const char* reason) { if(!ok) throw std::runtime_error(reason); }
template<class Marker,class Context> static auto RestoreSeries(const std::filesystem::path& path,const Context& context,const std::string& identity)
{ return ReadPolicyFrameSeries(path,context,1024*1024,{},identity); }
template<class Writer> static void FrameSeries(const std::filesystem::path& root,PagedRecordOptions options)
{
    // The nominal block size must not force a tiny final block to consume
    // a full-sized reader budget; only two real rows are present here.
    options.blockBytes=4*1024*1024;
    options.workspace=std::make_shared<AnalysisWorkspaceBudget>(16*1024*1024,8*1024*1024);
    PolicySignatureContext context; context.metricPreference="exclusive";
    std::vector<NeutralMergedFrameValues> frames(2);
    frames[0].frameIndex=7; frames[0].inclusiveNs=100; frames[0].exclusiveNs=30; frames[0].exact=true;
    frames[1].frameIndex=8; frames[1].inclusiveNs=200; frames[1].exclusiveNs=40; frames[1].exact=true;
    if constexpr(requires(Writer& writer) { WritePolicyFrameSeries(writer,context,frames); })
    {
        const auto identity=std::string(64,'a');
        { Writer writer(root/"series",identity,56,options); WritePolicyFrameSeries(writer,context,frames); writer.Commit(); }
        const auto restored=RestoreSeries<Writer>(root/"series",context,identity);
        Require(restored.size()==2 && restored[0].frameIndex==7 && restored[0].valueNs==30 &&
            restored[1].frameIndex==8 && restored[1].valueNs==40,"compressed series retains exact per-frame values");
    }
    else throw std::runtime_error("frame series still requires a raw uncompressed stream");
}
static void Run()
{
    const auto root=std::filesystem::temp_directory_path()/
        ("jn-paged-record-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto identity=std::string(64,'b');
    auto memory=std::make_shared<AnalysisWorkspaceBudget>(1024*1024,512*1024);
    PagedRecordOptions options; options.blockBytes=1024; options.workspace=memory;
    std::filesystem::create_directories(root);
    {
        PagedRecordWriter writer(root/"table",identity,16,options);
        for(uint64_t i=0;i<10000;++i) { const std::array<uint64_t,2> row{i,77}; writer.Append(row.data(),1); }
        writer.Commit();
    }
    Require(memory->Snapshot().currentBytes==0,"writer resources released");
    Require(std::filesystem::file_size(root/"table"/"data.bin")<100000,"repeated rows use compressed blocks");
    {
        PagedRecordReader reader(root/"table",identity,16,options);
        Require(reader.Count()==10000,"exact persisted count");
        for(auto ordinal:{uint64_t(9999),uint64_t(0),uint64_t(63),uint64_t(64),uint64_t(5123)}) {
            std::array<uint64_t,2> row{}; reader.Read(ordinal,row.data(),1);
            Require(row[0]==ordinal && row[1]==77,"cold random read crosses compressed block boundaries");
        }
        std::array<uint64_t,8> rows{}; reader.Read(62,rows.data(),4);
        Require(rows[0]==62 && rows[6]==65,"range read crosses block boundary");
        const auto readStats=reader.Statistics();
        Require(readStats.blockLoads==6,"reader reports real compressed block reloads");
        Require(readStats.decodedBytes==5376 && readStats.storedBytes>0,
            "reader reports bytes processed by block reloads");
        Require(readStats.checksumBytes==readStats.storedBytes && readStats.decompressions==6,
            "reader reports checksum and decompression work");
        bool rejected=false; try { reader.Read(10000,rows.data(),1); } catch(const std::exception&) { rejected=true; }
        Require(rejected,"out-of-range read rejected");
        reader.ValidateAll();
        Require(memory->Snapshot().currentBytes<128*1024,"directory memory does not grow with block count");
    }
    Require(memory->Snapshot().currentBytes==0,"reader resources released");
    bool wrongIdentity=false; try { PagedRecordReader bad(root/"table",std::string(64,'c'),16,options); }
    catch(const std::exception&) { wrongIdentity=true; }
    Require(wrongIdentity,"different trace cannot read table");
    {
        std::fstream file(root/"table"/"data.bin",std::ios::binary|std::ios::in|std::ios::out);
        const char corrupt=0; file.write(&corrupt,1);
    }
    bool corrupt=false; try { PagedRecordReader bad(root/"table",identity,16,options); bad.ValidateAll(); }
    catch(const std::exception&) { corrupt=true; }
    Require(corrupt,"corrupt block cannot be accepted");
    FrameSeries<PagedRecordWriter>(root,options);
    std::filesystem::remove_all(root);
}
#else
static void Run() { throw std::runtime_error("bounded compressed ordinal store is missing"); }
#endif
int main() { try { Run(); std::cout<<"paged record store passed\n"; return 0; }
catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; } }

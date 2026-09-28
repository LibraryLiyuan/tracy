#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <string_view>
#if __has_include("TracyPagedRecordStore.hpp")
#include "TracyPagedRecordStore.hpp"
#include "TracyAnalysisDiskBudget.hpp"
#include "TracyPolicyFrameSeries.hpp"
using namespace tracy::analysis;
static void Require(bool ok,const char* reason) { if(!ok) throw std::runtime_error(reason); }
static int ZoneReadCapacity(uint64_t count,bool incompressible=false)
{
    namespace fs=std::filesystem;
    if(count<100000 || count>10000000) return 2;
    const auto root=fs::temp_directory_path()/
        ("query-zone-read-capacity-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try
    {
        fs::create_directories(root);
        const std::string identity(64,'a');
        auto usage=std::make_shared<AnalysisDiskUsage>(std::vector<fs::path>{root});
        PagedRecordOptions options;
        options.workspace=std::make_shared<AnalysisWorkspaceBudget>(4ull<<30,2ull<<30);
        options.disk=std::make_shared<AnalysisDiskBudget>(AnalysisDiskBudget{usage,20ull<<30,{}});
        const auto started=std::chrono::steady_clock::now();
        options.cancelled=[started] { return std::chrono::steady_clock::now()-started>std::chrono::minutes(10); };
        constexpr size_t width=52;
        std::array<std::array<uint8_t,width>,4096> rows{};
        const auto writeStarted=std::chrono::steady_clock::now();
        {
            PagedRecordWriter writer(root/"zones",identity,width,options);
            for(uint64_t offset=0;offset<count;)
            {
                const auto take=std::min<uint64_t>(rows.size(),count-offset);
                for(uint64_t i=0;i<take;++i)
                {
                    const uint64_t ordinal=offset+i;
                    std::memcpy(rows[size_t(i)].data(),&ordinal,sizeof(ordinal));
                    const uint64_t parent=ordinal?ordinal-1:UINT64_MAX;
                    std::memcpy(rows[size_t(i)].data()+8,&parent,sizeof(parent));
                    if(incompressible)
                    {
                        uint64_t bits=ordinal+0x9E3779B97F4A7C15ull;
                        for(size_t byte=8;byte<width;++byte)
                        {
                            bits^=bits<<13;bits^=bits>>7;bits^=bits<<17;
                            rows[size_t(i)][byte]=uint8_t(bits);
                        }
                    }
                }
                writer.Append(rows.data(),take);
                offset+=take;
            }
            writer.Commit();
        }
        const auto writeMs=std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now()-writeStarted).count();
        const auto readStarted=std::chrono::steady_clock::now();
        uint64_t blockLoads=0,decodedBytes=0;
        {
            PagedRecordReader reader(root/"zones",identity,width,options);
            std::array<uint8_t,width> row{};
            for(uint64_t ordinal=0;ordinal<count;++ordinal)
            {
                reader.Read(ordinal,row.data(),1);
                uint64_t restored=UINT64_MAX;
                std::memcpy(&restored,row.data(),sizeof(restored));
                Require(restored==ordinal,"zone read probe lost ordinal");
            }
            const auto statistics=reader.Statistics();
            blockLoads=statistics.blockLoads;
            decodedBytes=statistics.decodedBytes;
        }
        const auto readMs=std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now()-readStarted).count();
        const auto disk=usage->Snapshot();
        std::cout<<"zone_read_records="<<count<<" incompressible="<<incompressible
            <<" raw_bytes="<<count*width
            <<" final_bytes="<<disk.currentBytes<<" peak_bytes="<<disk.peakBytes
            <<" write_ms="<<writeMs<<" read_ms="<<readMs
            <<" block_loads="<<blockLoads<<" decoded_bytes="<<decodedBytes<<'\n';
        std::error_code cleanup;
        fs::remove_all(root,cleanup);
        if(cleanup) throw std::runtime_error("zone read capacity cleanup: "+cleanup.message());
        return 0;
    }
    catch(const std::exception& error)
    {
        std::cerr<<error.what()<<'\n';
        std::error_code ignored;
        fs::remove_all(root,ignored);
        return 1;
    }
}
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
static void FrameSeriesBulkRead(const std::filesystem::path& root)
{
    const std::string identity(64,'b');
    PagedRecordOptions options;options.blockBytes=64*1024;
    options.workspace=std::make_shared<AnalysisWorkspaceBudget>(32*1024*1024,16*1024*1024);
    PolicySignatureContext context;context.metricPreference="exclusive";
    std::vector<NeutralMergedFrameValues> frames(16384);
    for(size_t i=0;i<frames.size();++i) {
        frames[i].frameIndex=i;frames[i].inclusiveNs=100+i;
        frames[i].exclusiveNs=30+i;frames[i].exact=true;
    }
    { PagedRecordWriter writer(root/"bulk-series",identity,56,options);
      WritePolicyFrameSeries(writer,context,frames);writer.Commit(); }
    uint64_t cancellationChecks=0;
    const auto restored=ReadPolicyFrameSeries(root/"bulk-series",context,32*1024*1024,
        [&]{++cancellationChecks;return false;},identity);
    Require(restored.size()==frames.size() && restored.front().valueNs==30 &&
        restored.back().frameIndex==frames.size()-1 && restored.back().valueNs==30+int64_t(frames.size()-1),
        "bulk frame-series read preserves every ordinal and value");
    Require(cancellationChecks<64,
        "sequential frame-series reading must check cancellation per chunk, not per record");
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
    {
        auto cachedOptions=options; cachedOptions.readerCacheBytes=2048;
        PagedRecordReader reader(root/"table",identity,16,cachedOptions);
        for(uint64_t i=0;i<1000;++i) {
            std::array<uint64_t,2> row{}; const auto ordinal=(i%2)*64;
            reader.Read(ordinal,row.data(),1);
            Require(row[0]==ordinal,"cached pages preserve row identity");
        }
        Require(reader.Statistics().blockLoads==2,"alternating dictionary pages must decode once per cached page");
        std::array<uint64_t,2> row{};
        for(auto ordinal:{uint64_t(128),uint64_t(64),uint64_t(0)}) {
            reader.Read(ordinal,row.data(),1);Require(row[0]==ordinal,"cache eviction preserves data");
        }
        Require(reader.Statistics().blockLoads==4,"two-page cache must evict the least recently used page");
        const auto before=reader.Statistics().blockLoads;
        reader.ValidateAll();
        Require(reader.Statistics().blockLoads-before==157,"deep validation must bypass cached pages");
    }
    Require(memory->Snapshot().currentBytes==0,"multi-page cache resources released");
    {
        auto limited=options;limited.readerCacheBytes=8192;
        limited.workspace=std::make_shared<AnalysisWorkspaceBudget>(10000,5000);
        bool rejected=false;
        try { PagedRecordReader reader(root/"table",identity,16,limited); }
        catch(const std::exception&) { rejected=limited.workspace->Snapshot().rejectedReservations==1; }
        Require(rejected && limited.workspace->Snapshot().currentBytes==0,"reader cache must reserve shared budget before growth");
    }
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
    FrameSeriesBulkRead(root);
    std::filesystem::remove_all(root);
}
#else
static void Run() { throw std::runtime_error("bounded compressed ordinal store is missing"); }
static int ZoneReadCapacity(uint64_t,bool=false) { throw std::runtime_error("bounded compressed ordinal store is missing"); }
#endif
int main(int argc,char** argv) { try {
    if(argc==3 && std::string_view(argv[1])=="--zone-read-capacity") return ZoneReadCapacity(std::stoull(argv[2]));
    if(argc==4 && std::string_view(argv[1])=="--zone-read-capacity" &&
        std::string_view(argv[3])=="incompressible") return ZoneReadCapacity(std::stoull(argv[2]),true);
    if(argc!=1) return 2;
    Run(); std::cout<<"paged record store passed\n"; return 0; }
catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; } }

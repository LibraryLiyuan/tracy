#include <iostream>
#include <stdexcept>
#include "TracyAnalysisWorkspaceBudget.hpp"
#include "TracyAnalysisCacheTable.hpp"
#include "TracyAnalysisCacheSort.hpp"
#include "TracyPolicyFrameSeries.hpp"
#include <atomic>
#include <future>
#include <thread>
#include <vector>
#include "TracyAnalysisProcessMemory.hpp"
using namespace tracy::analysis;
namespace
{
void Require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
template<class F> void Reject(F&& f,const char* reason)
{
    try { f(); } catch(const std::exception& e) {
        if(std::string(e.what()).find(reason)!=std::string::npos) return;
        throw;
    }
    throw std::runtime_error(std::string("expected rejection: ")+reason);
}
template<class Disk> void DiskSeries(std::ostream& out,PolicySignatureContext& context,
    const std::vector<NeutralMergedFrameValues>& frames,const Disk& disk)
{
    if constexpr(requires{WritePolicyFrameSeries(out,context,frames,disk);})
        WritePolicyFrameSeries(out,context,frames,disk);
    else WritePolicyFrameSeries(out,context,frames);
}
void DiskFiles(const std::filesystem::path& root)
{
    for(const auto limit:{47,97})
    {
        const auto path=root.parent_path()/("disk-table-"+std::to_string(limit));
        auto usage=std::make_shared<AnalysisDiskUsage>(std::vector<std::filesystem::path>{path});
        AnalysisDiskActivity activity(usage);
        auto disk=std::make_shared<AnalysisDiskBudget>(AnalysisDiskBudget{usage,uint64_t(limit),{}});
        AnalysisCacheTableOptions options; options.disk=disk;
        {
            AnalysisCacheTableWriter writer(path,std::string(64,'a'),"disk",options);
            if(limit==47) Reject([&]{writer.Append("k","v");},"cache_disk_budget");
            else { writer.Append("k","v"); Reject([&]{writer.Commit();},"cache_disk_budget"); }
        }
        const uint64_t admitted=limit==47?24:48;
        Require(AnalysisDiskUsage::Bytes(path)==admitted && usage->Current()==admitted,
            "index and block budget denials must happen before their bytes reach disk");
    }
    std::filesystem::create_directories(root);
    auto usage=std::make_shared<AnalysisDiskUsage>(std::vector<std::filesystem::path>{root});
    AnalysisDiskActivity activity(usage);
    auto disk=std::make_shared<AnalysisDiskBudget>(AnalysisDiskBudget{usage,128,{}});
    {
        std::ofstream out(root/"frames",std::ios::binary);
        PolicySignatureContext context; std::vector<NeutralMergedFrameValues> frames(3);
        Reject([&]{DiskSeries(out,context,frames,disk);},"cache_disk_budget");
        out.flush(); Require(std::filesystem::file_size(root/"frames")==0,"frame series reject before writing any oversized extent");
    }
    AnalysisCacheTableOptions options; options.blockBytes=1024; options.disk=disk;
    disk->maximumBytes=98; // 24-byte index header + 24-byte row + 50-byte block.
    {
        AnalysisCacheTableWriter writer(root/"manifest-denied",std::string(64,'a'),"disk",options);
        writer.Append("k","v");
        Reject([&]{writer.Commit();},"cache_disk_budget");
    }
    Require(AnalysisDiskUsage::Bytes(root)==98 && usage->Current()==98,
        "manifest publication is charged before writing, retaining only actual partial data");
    Require(!std::filesystem::exists(root/"manifest-denied"/"manifest.json"),"disk-denied manifest cannot publish");
    disk->maximumBytes=1024*1024;
    {
        AnalysisCacheSortedWriter writer(root/"sorted",std::string(64,'a'),"disk",options,1024);
        for(int i=40;i>=0;--i) writer.Append(std::to_string(100+i),std::string(48,'x'));
        writer.Commit();
    }
    Require(usage->Current()==AnalysisDiskUsage::Bytes(root),"successful sort cleanup releases all deleted intermediate bytes");
    disk->maximumBytes=1;
    {
        AnalysisCacheTableReader reader(root/"sorted",std::string(64,'a'),"disk",options);
        Require(reader.GetAt(0).key=="100","read-only records do not need disk growth capacity");
    }
}
void Reservations()
{
    auto budget=std::make_shared<AnalysisWorkspaceBudget>(256,128);
    {
        AnalysisWorkspaceReservation first(budget,128),second(budget,64);
        Require(budget->Snapshot().currentBytes==192,"components share one accounting total");
        Reject([&]{AnalysisWorkspaceReservation third(budget,65);},"workspace_budget");
        Require(budget->Snapshot().currentBytes==192,"failed acquire preserves existing reservations");
        second.Resize(128);
        Reject([&]{first.Resize(129);},"workspace_budget");
        Require(budget->Snapshot().currentBytes==256,"failed growth cannot increase the total");
        first.Resize(32);
        AnalysisWorkspaceReservation moved(std::move(second));
        second=std::move(first);
        Require(budget->Snapshot().currentBytes==160,"moves transfer ownership without double charging");
        moved.Resize(0);
        Require(budget->Snapshot().currentBytes==32,"shrinking releases capacity to other components");
        AnalysisWorkspaceReservation other(budget,200);
        second=std::move(other);
        Require(budget->Snapshot().currentBytes==200,"move assignment releases the previous owner");
        Reject([&]{second.Add(UINT64_MAX);},"workspace_budget");
    }
    const auto snapshot=budget->Snapshot();
    Require(snapshot.currentBytes==0 && snapshot.peakBytes==256,"RAII releases all live reservations and preserves peak");
    Require(snapshot.rejectedReservations>=2,"budget denial is observable");
    Reject([]{AnalysisWorkspaceBudget invalid(0,0);},"workspace_configuration");
    Reject([]{AnalysisWorkspaceBudget invalid(256,257);},"workspace_configuration");
    Reject([]{AnalysisWorkspaceBudget invalid(4ull*1024*1024*1024+1,1);},"workspace_configuration");
}
void ProcessMemoryGuard()
{
    // The native sample covers memory outside analysis reservations too.
    const auto native=ReadAnalysisProcessMemory();
    Require(native.available && native.residentBytes>0 && native.privateBytes>0,
        "native process sampling must report actual memory, not workspace reservations");
    AnalysisProcessMemoryOptions options;
    options.maximumBytes=256;
    AnalysisProcessMemorySample sample{true,128,256};
    options.sample=[&]{return sample;};
    AnalysisProcessMemoryGuard atLimit(options);
    atLimit.Check(); // Equality is allowed; either metric exceeding it is not.
    sample.privateBytes=257;
    Reject([&]{atLimit.Check();},"process_memory_budget");
    sample={true,1,1};
    Reject([&]{atLimit.Check();},"process_memory_budget");
    AnalysisProcessMemoryGuard fresh(options); fresh.Check();
    sample.residentBytes=257;
    Reject([&]{fresh.Check();},"process_memory_budget");
    sample={false,0,0};
    AnalysisProcessMemoryGuard unavailable(options);
    Reject([&]{unavailable.Check();},"process_memory_unavailable");
    options.sample=[]{throw std::runtime_error("sample failed"); return AnalysisProcessMemorySample{};};
    AnalysisProcessMemoryGuard failure(options);
    Reject([&]{failure.Check();},"process_memory_unavailable");
    // The host cap permits the explicitly authorized 32 GiB run, while the
    // guard still rejects an actual sample above that cap. No large allocation.
    AnalysisProcessMemoryOptions expanded;
    sample={true,24ull*1024*1024*1024,24ull*1024*1024*1024};
    expanded.sample=[&]{return sample;};
    AnalysisProcessMemoryGuard expandedDefault(expanded);
    expandedDefault.Check();
    sample.privateBytes=32ull*1024*1024*1024+1;
    Reject([&]{expandedDefault.Check();},"process_memory_budget");
    options.maximumBytes=32ull*1024*1024*1024+1;
    Reject([&]{AnalysisProcessMemoryGuard invalid(options);},"process_memory_configuration");

    // A scanner with no progress callbacks still receives cooperative stop.
    std::atomic<uint64_t> privateBytes=128;
    options.maximumBytes=256; options.interval=std::chrono::milliseconds(5);
    options.sample=[&]{return AnalysisProcessMemorySample{true,128,privateBytes.load()};};
    AnalysisProcessMemoryGuard asynchronous(options);
    std::stop_source stop;
    auto monitor=asynchronous.Monitor(stop);
    privateBytes=300;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(!stop.stop_requested() && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
    Require(stop.stop_requested(),"process sampling must stop a long stage independently of progress callbacks");
    Reject([&]{asynchronous.Check();},"process_memory_budget");
    const auto observed=asynchronous.Snapshot();
    Require(observed.peakPrivateBytes>=300 && observed.maximumBytes==256 && observed.samples>=1,
        "guard records measured peaks and the enforced limit");
}
template<class Guard> void PeriodicMemoryCheck(Guard& guard)
{
    guard.Check(false);
}
void ProcessMemoryCheckpoints()
{
    uint64_t calls=0;
    AnalysisProcessMemoryOptions options; options.maximumBytes=256; options.interval=std::chrono::seconds(1);
    options.sample=[&]{++calls; return AnalysisProcessMemorySample{true,128,128};};
    AnalysisProcessMemoryGuard guard(options);
    for(unsigned i=0;i<1000;++i) PeriodicMemoryCheck(guard);
    Require(calls==1,"record checkpoints must not perform an OS memory query per record");
    guard.Check(); Require(calls==2,"publication checks must force a fresh process sample");
}
void ConcurrentReservations()
{
    auto budget=std::make_shared<AnalysisWorkspaceBudget>(256,128);
    std::promise<void> release;
    const auto released=release.get_future().share();
    std::atomic<unsigned> attempted=0,accepted=0;
    std::vector<std::future<void>> workers;
    for(unsigned i=0;i<8;++i) workers.push_back(std::async(std::launch::async,[&]{
        try {
            AnalysisWorkspaceReservation reservation(budget,64);
            ++accepted; ++attempted; released.wait();
        } catch(const std::exception& e) {
            ++attempted;
            if(std::string(e.what()).find("workspace_budget")==std::string::npos) throw;
        }
    }));
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(attempted.load()!=8 && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
    const auto full=budget->Snapshot(); release.set_value();
    for(auto& worker:workers) worker.get();
    Require(attempted==8 && accepted==4 && full.currentBytes==256,
        "concurrent allocations must not each pass against the same free capacity");
    Require(budget->Snapshot().currentBytes==0 && budget->Snapshot().peakBytes==256,
        "concurrent release returns the entire shared budget");
}
void Components(const std::filesystem::path& root)
{
    auto budget=std::make_shared<AnalysisWorkspaceBudget>(1024*1024,512*1024);
    AnalysisCacheTableOptions options;
    options.blockBytes=256*1024; options.manifestBytes=4096; options.workspace=budget;
    const std::string identity(64,'a');
    {
        AnalysisWorkspaceReservation occupied(budget,1024*1024-64*1024);
        Reject([&]{AnalysisCacheTableWriter denied(root/"denied-writer",identity,"budget",options);},"workspace_budget");
        Require(!std::filesystem::exists(root/"denied-writer"/"manifest.json"),"over-budget writer must never publish");
    }
    Require(budget->Snapshot().currentBytes==0,"failed writer construction releases all reservations");
    {
        AnalysisCacheTableWriter writer(root/"source",identity,"budget",options);
        writer.Append("key","payload"); writer.Commit();
        Require(budget->Snapshot().currentBytes>=options.blockBytes,"writer's retained buffer remains charged");
    }
    Require(budget->Snapshot().currentBytes==0,"destroying writer returns shared capacity");
    {
        AnalysisWorkspaceReservation occupied(budget,1024*1024-64*1024);
        Reject([&]{AnalysisCacheTableReader denied(root/"source",identity,"budget",options); denied.GetAt(0);},"workspace_budget");
    }
    Require(budget->Snapshot().currentBytes==0,"reader growth failure releases metadata and buffers");
    {
        AnalysisCacheTableReader reader(root/"source",identity,"budget",options);
        Require(reader.GetAt(0).payload=="payload","read succeeds when shared capacity is available");
        Require(budget->Snapshot().currentBytes>=options.blockBytes,"loaded reader buffer is charged");
    }
    {
        AnalysisWorkspaceReservation occupied(budget,1024*1024-64*1024);
        Reject([&]{AnalysisCacheSortedWriter denied(root/"denied-sort",identity,"budget",options,256*1024);},"workspace_budget");
        Require(!std::filesystem::exists(root/"denied-sort"/"manifest.json"),"over-budget sort must never publish");
    }
    Require(budget->Snapshot().currentBytes==0,"all component reservations released");
}
}
int main()
{
    try {
        ProcessMemoryGuard(); ProcessMemoryCheckpoints(); Reservations(); ConcurrentReservations();
        const auto root=std::filesystem::temp_directory_path()/
            ("jn-workspace-budget-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        Components(root); DiskFiles(root/"disk"); std::filesystem::remove_all(root);
        std::cout<<"analysis workspace budget tests passed\n"; return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}

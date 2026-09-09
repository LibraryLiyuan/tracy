#include "TracyAnalysisScanManager.hpp"
#include "TracyAnalysisScanCache.hpp"
#include "TracyAnalysisProfile.hpp"
#include "TracyBoundedScanCursor.hpp"
#include "TracyExactStatistics.hpp"
#include "TracyGpuJobManagedScanner.hpp"
#include "TracyNeutralStatisticsCache.hpp"
#include "TracyPolicyFrameSeries.hpp"
#include "TracyAnalysisProcessMemory.hpp"
#include "TracyHash.hpp"
#include "TracyQueryService.hpp"
#include "FakeTraceSource.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <iostream>
#include <exception>
#include <cstdlib>
#include <tuple>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace std::chrono_literals;
using nlohmann::json;
using namespace tracy::analysis;
using namespace tracy::query;

namespace
{
std::atomic<const char*> testStage="startup";

json Profile( uint64_t topN = 10 )
{
    json result = json::object();
    result["schema_version"] = 1;
    result["profile_name"] = "scan-manager-test";
    result["frame_budget"] = { { "target_fps", 60.0 }, { "frame_ms", 16.666667 } };
    result["resource_budgets"] = {
        { "cpu_memory", { { "value", 16.0 }, { "unit", "GB" }, { "status", "provisional" } } },
        { "gpu_memory", { { "value", 6.4 }, { "unit", "GB" }, { "status", "fixed" } } } };
    result["candidate_policy"] = { { "top_n", topN }, { "cumulative_contribution", 0.8 },
        { "per_domain_limit", 50 }, { "priorities", json::array( { "P0", "P1", "P2", "P3", "P4" } ) } };
    result["limits"] = { { "query_memory_target_bytes", 8589934592ULL },
        { "query_memory_hard_bytes", 17179869184ULL }, { "cache_max_bytes", 137438953472ULL },
        { "minimum_free_disk_bytes", 68719476736ULL } };
    result["user_focus"] = json::array( { "Update" } );
    return result;
}

AnalysisScanProducts Products( const std::filesystem::path& temporary )
{
    NeutralStatisticsInput input;
    input.temporaryRoot = temporary;
    input.maximumBufferedValues = 2;
    input.runs = { { "cpu", "cpu:update", "Player.Frame", 7,
        20'000'000, 20'000'000, 0, 0, 1, true, false } };
    input.denominators = { { "cpu", "cpu:update", "Player.Frame", 1 } };
    input.domainAudit = { { "cpu", true, "complete", 1, 1, 1,
        std::string( 64, 'a' ), std::string( 64, 'a' ), true, {} } };

    AnalysisScanProducts products;
    products.aggregate = BuildNeutralStatistics( input );
    PolicySignatureContext context;
    context.domain = "cpu"; context.signatureId = "cpu:update";
    context.name = "Update"; context.path = "PlayerLoop/Update";
    context.frameScope = "Player.Frame"; context.frameRoot = true;
    context.frames = { { 7, 20'000'000, { "cpu-zone:7" }, "player-loop" } };
    products.signatureContexts.push_back( std::move( context ) );
    products.capacityFacts.push_back( { "gpu.memory", "gpu-local-peak", "gpu-capacity",
        "gpu_memory", 7'000'000'000ULL, 7, true, "DXGI Local peak" } );
    return products;
}

AnalysisScanSnapshot WaitFor( AnalysisScanManager& manager, const std::string& scanId,
    ScanState state, std::chrono::milliseconds timeout = 2s )
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    AnalysisScanSnapshot snapshot;
    do
    {
        snapshot = manager.Status( scanId );
        if( snapshot.state == state ) return snapshot;
        std::this_thread::sleep_for( 5ms );
    }
    while( std::chrono::steady_clock::now() < deadline );
    if( snapshot.state != state ) std::cerr << "Scan did not complete: " << snapshot.stage << ": " << snapshot.error << '\n';
    assert( snapshot.state == state );
    return snapshot;
}

template<class Manager,class... Args>
std::unique_ptr<Manager> ProcessGuardedManager(AnalysisProcessMemoryOptions options,Args&&... args)
{
    return std::make_unique<Manager>(std::forward<Args>(args)...,std::shared_ptr<AnalysisWorkspaceBudget>{},std::move(options));
}

template<class Manager,class... Args>
std::unique_ptr<Manager> DiskGuardedManager(std::function<uint64_t(const std::filesystem::path&)> available,Args&&... args)
{
    if constexpr(std::is_constructible_v<Manager,Args...,std::shared_ptr<AnalysisWorkspaceBudget>,
        AnalysisProcessMemoryOptions,decltype(available)>)
        return std::make_unique<Manager>(std::forward<Args>(args)...,std::shared_ptr<AnalysisWorkspaceBudget>{},
            AnalysisProcessMemoryOptions{},std::move(available));
    else return std::make_unique<Manager>(std::forward<Args>(args)...);
}

// Test-only materialization of this tiny FakeTraceSource, so all original
// field/path/audit assertions also exercise the persisted default output.
void ReadSmallFixture( AnalysisScanProducts& products )
{
    if( products.neutralCachePath.empty() || products.neutralCacheIdentity.size() != 64 )
        throw std::runtime_error( "Default scan did not return a neutral cache" );
    NeutralStatisticsCacheReader reader( products.neutralCachePath, products.neutralCacheIdentity );
    assert( reader.SignatureCount() > 0 );
    assert( json::parse( reader.SummaryJson() ).at( "materialized_result_peak" ) == "0" );
    reader.VisitSignatureContexts( [&]( const NeutralSignatureAggregate* aggregate,
        const PolicySignatureContext& context ) {
        if( aggregate ) products.aggregate.signatures.push_back( *aggregate );
        products.signatureContexts.push_back( context );
    } );
}

template<class F> void RejectCache( F&& f, const char* expected )
{
    try { f(); }
    catch( const std::exception& e )
    {
        if( std::string(e.what()).find(expected)!=std::string::npos ) return;
        throw;
    }
    throw std::runtime_error(std::string("expected cache rejection: ")+expected);
}

void RemoveTestRoot(const std::filesystem::path& root)
{
#ifdef _WIN32
    // Nested immutable generations can exceed Win32's legacy MAX_PATH.
    std::filesystem::remove_all(std::filesystem::path(L"\\\\?\\"+root.wstring()));
#else
    std::filesystem::remove_all(root);
#endif
}

void BundleMetadata( const std::filesystem::path& root )
{
    const auto generation=root/"metadata-bundle";
    std::filesystem::create_directories(generation);
    auto products=Products(generation);
    PolicyFrameTimeline timeline; timeline.frameScope="Typed.FrameSet"; timeline.name="Typed Root";
    timeline.frames={{0,12,{"frame:0"},"Root/Zero",true,100,112},
        {1,-3,{"frame:1"},"Root/Unknown",false,std::nullopt,std::nullopt},
        {2,0,{},"Root/KnownZero",true,112,112}};
    products.frameTimelines.push_back(timeline);
    const std::string identity(64,'a');
    const auto sha=PublishAnalysisNeutralBundle(generation,identity,products);
    const auto loaded=OpenAnalysisNeutralBundle(generation,identity,sha);
    assert(loaded.aggregate.signatures.empty() && loaded.signatureContexts.empty());
    assert(loaded.frameTimelines.size()==1 && loaded.capacityFacts.size()==1);
    const auto& frames=loaded.frameTimelines[0].frames;
    assert(frames.size()==3 && frames[0].beginNs==100 && frames[0].endNs==112);
    assert(!frames[1].exact && frames[1].valueNs==-3 && !frames[1].beginNs && !frames[1].endNs);
    assert(frames[2].exact && frames[2].valueNs==0 && frames[2].beginNs==112 && frames[2].endNs==112);
    assert(frames[0].eventRefs==std::vector<std::string>{"frame:0"} && frames[1].structureKey=="Root/Unknown");
    assert(loaded.capacityFacts[0].valueBytes==7'000'000'000ULL && loaded.capacityFacts[0].frameIndex==7);
    {
        AnalysisCacheTableOptions budgetOptions;
        budgetOptions.workspace=std::make_shared<AnalysisWorkspaceBudget>(1024ull*1024*1024,512ull*1024*1024);
        {
            auto charged=OpenAnalysisNeutralBundle(generation,identity,sha,budgetOptions);
            if(budgetOptions.workspace->Snapshot().currentBytes==0)
                throw std::runtime_error("Reopened Timeline metadata escaped shared workspace ownership");
            const auto bytes=budgetOptions.workspace->Snapshot().currentBytes;
            auto moved=std::move(charged);
            assert(budgetOptions.workspace->Snapshot().currentBytes==bytes && moved.frameTimelines[0].frames.size()==3);
        }
        assert(budgetOptions.workspace->Snapshot().currentBytes==0);
    }
    RejectCache([&]{OpenAnalysisNeutralBundle(generation,identity,std::string(64,'b'));},"header_identity");
    RejectCache([&]{OpenAnalysisNeutralBundle(generation,identity,sha,{},512);},"metadata_budget");
    {std::ofstream corrupt(loaded.frameSeriesPath,std::ios::binary|std::ios::app); corrupt.put('x');}
    RejectCache([&]{OpenAnalysisNeutralBundle(generation,identity,sha);},"series_identity");
    const auto partial=root/"metadata-cancelled";
    std::filesystem::create_directories(partial);
    auto cancelled=Products(partial);
    AnalysisCacheTableOptions options;
    options.cancelled=[&]{return std::filesystem::exists(partial/"metadata"/"manifest.json");};
    RejectCache([&]{PublishAnalysisNeutralBundle(partial,identity,cancelled,options);},"cancelled");
    assert(!std::filesystem::exists(partial/"bundle-header"/"manifest.json"));
}

// A valid wide current JSON record must be charged before encoding/decoding,
// independently of the table's raw block and retained metadata reservations.
void BundleJsonWorkspace(const std::filesystem::path& root)
{
    bool protectedRecords=true;
    for(const std::string mode:{"header","capacity","timeline","frame"}) {
        const auto generation=root/("bundle-json-"+mode);
        std::filesystem::create_directories(generation);
        auto products=Products(generation);
        products.frameTimelines.push_back({"Typed.FrameSet","Root",{{0,12,{"frame:0"},"Root",true,100,112}},"frame"});
        const std::string identity(64,'a');
        AnalysisCacheTableOptions options; options.blockBytes=128*1024;
        auto sha=PublishAnalysisNeutralBundle(generation,identity,products,options);
        if(mode=="header") {
            std::string payload;
            {
                AnalysisCacheTableReader header(generation/"bundle-header",identity,"analysis-neutral-bundle-v1",options);
                auto document=json::parse(header.GetAt(0).payload);
                document["extension"]=std::string(65536,'h'); payload=document.dump();
            }
            std::filesystem::remove_all(generation/"bundle-header");
            AnalysisCacheTableWriter header(generation/"bundle-header",identity,"analysis-neutral-bundle-v1",options);
            header.Append("header",payload); sha=header.Commit().contentSha256;
        } else {
            if(mode=="capacity") products.capacityFacts[0].description=std::string(65536,'c');
            if(mode=="timeline") products.frameTimelines[0].name=std::string(65536,'t');
            if(mode=="frame") products.frameTimelines[0].frames[0].eventRefs={std::string(65536,'f')};
            std::filesystem::remove_all(generation/"metadata");
            std::filesystem::remove_all(generation/"bundle-header");
        }
        options.workspace=std::make_shared<AnalysisWorkspaceBudget>((mode=="header"?2:1)*1024*1024,512*1024);
        bool rejected=false;
        try {
            if(mode=="header") OpenAnalysisNeutralBundle(generation,identity,sha,options);
            else PublishAnalysisNeutralBundle(generation,identity,products,options);
        } catch(const std::exception& e) {
            if(std::string(e.what()).find("workspace_budget")==std::string::npos) throw;
            rejected=true;
        }
        const bool unpublished=mode=="header" || !std::filesystem::exists(generation/"bundle-header"/"manifest.json");
        if(!rejected || !unpublished) std::cerr<<"bundle JSON escaped workspace: "<<mode<<'\n';
        protectedRecords&=rejected && unpublished;
        if(options.workspace->Snapshot().currentBytes!=0) throw std::runtime_error("Failed bundle operation leaked workspace");
        options.workspace.reset();
        if(mode!="header") {
            std::filesystem::remove_all(generation/"metadata");
            std::filesystem::remove_all(generation/"bundle-header");
            sha=PublishAnalysisNeutralBundle(generation,identity,products,options);
        }
        const auto recovered=OpenAnalysisNeutralBundle(generation,identity,sha,options);
        if(recovered.capacityFacts[0].description!=products.capacityFacts[0].description ||
            recovered.frameTimelines[0].name!=products.frameTimelines[0].name ||
            recovered.frameTimelines[0].frames[0].eventRefs!=products.frameTimelines[0].frames[0].eventRefs)
            throw std::runtime_error("Bundle JSON budget must preserve complete wide metadata on recovery");
    }
    if(!protectedRecords) throw std::runtime_error("Bundle header and metadata JSON require a current-record workspace reservation");
}

class SourceDegradedTrace final : public tracy::query::test::FakeTraceSource,
    public NativeBoundedTraceSource
{
public:
    bool nestedCpu = false;
    bool unknownCpu = false;
    bool wideNames = false;
    size_t wideSignatureCount = 0;
    bool compactScaleNames = false;
    int gpuFrameScopeProbe = 0;
    bool gpuProbeLongCosts = false;
    size_t wideGpuSignatureCount = 0;
    bool wideCorrelatedPayload = false;
    std::function<void( std::string_view )> readHook;
    std::vector<GfxEntityDto> ScanGfxEntities( size_t offset, size_t limit ) const override
    {
        if( readHook ) readHook( "gfx_entity" );
        return FakeTraceSource::ScanGfxEntities( offset, limit );
    }
    std::vector<MemoryEventDto> ScanMemoryEvents( const ScanRange& range ) const override
    {
        if( readHook ) readHook( "memory" );
        return FakeTraceSource::ScanMemoryEvents( range );
    }
    std::vector<GfxLinkDto> ScanGfxLinks( size_t offset, size_t limit ) const override
    {
        if( readHook ) readHook( "gfx_link" );
        return FakeTraceSource::ScanGfxLinks( offset, limit );
    }
    std::vector<IoRequestDto> ScanIoRequests( size_t offset, size_t limit ) const override
    {
        if( readHook ) readHook( "io" );
        return FakeTraceSource::ScanIoRequests( offset, limit );
    }
    std::vector<SampleDto> ScanSampleEvents( const ScanRange& range ) const override
    {
        if( readHook ) readHook( "sample" );
        return FakeTraceSource::ScanSampleEvents( range );
    }
    std::vector<CpuZoneDto> ScanCpuZones( const ScanRange& range ) const override
    {
        if( readHook ) readHook( "cpu" );
        if(wideSignatureCount)
        {
            std::vector<CpuZoneDto> values;
            const auto first=std::min(range.offset,wideSignatureCount);
            const auto last=first+std::min(range.limit,wideSignatureCount-first);
            for(auto index=first;index<last;++index)
            {
                CpuZoneDto zone;
                zone.ref=MakeEntityRef("cpu-zone",index);
                zone.threadRef=MakeEntityRef("thread",1);
                zone.sourceLocationRef="source:wide-"+std::to_string(index);
                zone.name=compactScaleNames?"ScaleCpu":"Wide"+std::string(64*1024,'x');
                zone.startNs=1; zone.endNs=2; zone.complete=true; zone.timingValid=true;
                values.push_back(std::move(zone));
            }
            return values;
        }
        if( !nestedCpu ) return FakeTraceSource::ScanCpuZones( range );
        std::vector<CpuZoneDto> values;
        const auto add = [&]( const char* name, size_t id, std::optional<size_t> parent,
            int64_t begin, int64_t end ) {
            CpuZoneDto zone;
            zone.ref = MakeEntityRef( "cpu-zone", id );
            zone.threadRef = MakeEntityRef( "thread", 1 );
            zone.sourceLocationRef = name == std::string( "Work" ) ? "source:work" : std::string( "source:" ) + name;
            zone.name = name; zone.startNs = begin; zone.endNs = end;
            if(wideNames) zone.name.append(64*1024,'x');
            zone.complete = true; zone.timingValid = true;
            if( parent ) zone.parentRef = MakeEntityRef( "cpu-zone", *parent );
            values.push_back( zone );
        };
        add( "ParentA", 10, {}, 0, 40 ); add( "Work", 11, 10, 1, 20 );
        add( "ParentB", 12, {}, 40, 80 ); add( "Work", 13, 12, 41, 60 );
        if(unknownCpu) values[1].timingValid=false;
        const auto first = std::min( range.offset, values.size() );
        const auto last = first + std::min( range.limit, values.size() - first );
        return { values.begin() + first, values.begin() + last };
    }
    std::vector<GpuZoneDto> ScanGpuZones( const ScanRange& range ) const override
    {
        if(gpuFrameScopeProbe)
        {
            std::vector<GpuZoneDto> values;
            const auto add=[&](uint64_t id,const char* name,const char* site,std::optional<uint64_t> parent,int64_t begin,int64_t end) {
                GpuZoneDto zone; zone.ref=MakeEntityRef("gpu-zone",id); zone.contextRef=MakeEntityRef("gpu-context",0);
                const int64_t scale=gpuProbeLongCosts?1000000:1;
                zone.name=name; zone.sourceLocationRef=site; zone.gpuStartNs=begin*scale; zone.gpuEndNs=end*scale; zone.complete=true;
                if(parent) zone.parentRef=MakeEntityRef("gpu-zone",*parent);
                values.push_back(std::move(zone));
            };
            add(0,"GPU.Frame.Direct","gpu-root",{},0,10);
            add(1,"Good.Pass","gpu-good",0,1,2);
            if(gpuFrameScopeProbe==8) {
                add(2,"Mid.Pass","gpu-mid",0,5,7);
                add(5,"Bad.Grandchild","gpu-bad",2,6,8);
            } else add(2,"Bad.Sibling","gpu-bad",0,9,gpuFrameScopeProbe==1?11:10);
            add(3,"GPU.Frame.Direct","gpu-root",{},20,30);
            if(gpuFrameScopeProbe!=6) add(4,"Good.Pass","gpu-good",3,21,22);
            if(gpuFrameScopeProbe==3 || gpuFrameScopeProbe==5) {
                values[0].gpuEndNs.reset(); values[0].complete=false;
            }
            if(gpuFrameScopeProbe==4) {values[2].gpuEndNs.reset(); values[2].complete=false;}
            if(gpuFrameScopeProbe==5) {values[3].gpuEndNs.reset(); values[3].complete=false;}
            const auto first=std::min(range.offset,values.size()),last=first+std::min(range.limit,values.size()-first);
            return {values.begin()+first,values.begin()+last};
        }
        if( !wideGpuSignatureCount ) return FakeTraceSource::ScanGpuZones( range );
        std::vector<GpuZoneDto> values;
        const auto first = std::min( range.offset, wideGpuSignatureCount );
        const auto last = first + std::min( range.limit, wideGpuSignatureCount - first );
        for( auto index = first; index < last; ++index )
        {
            GpuZoneDto zone;
            zone.ref = MakeEntityRef( "gpu-zone", index );
            zone.contextRef = MakeEntityRef( "gpu-context", 0 );
            zone.sourceLocationRef = "gpu-source:wide-" + std::to_string( index );
            zone.name = "WideGpu" + std::string( 64 * 1024, 'x' );
            zone.gpuStartNs = 1; zone.gpuEndNs = 2;
            zone.complete = true;
            values.push_back( std::move( zone ) );
        }
        return values;
    }
    uint32_t NativeBoundedScanVersion() const override { return NativeBoundedScanSchemaVersion; }
    TraceReadView AcquireReadView() const override
    {
        return { TraceSourceKind::Snapshot, TraceSourceState::Ready, 1, 100, true };
    }
    std::vector<FrameSetDto> GetFrameSets() const override
    {
        if( readHook ) readHook( "frame_set_metadata" );
        return {
            { MakeEntityRef( "frame-set", 0 ), 0, "Player.Frame", true, 2, 2 },
            { MakeEntityRef( "frame-set", 1 ), 1, "Render.Frame", false, 1, 1 }
        };
    }
    std::vector<FrameDto> ScanFrames( const ScanRange& range ) const override
    {
        if( readHook ) readHook( "frame" );
        const std::vector<FrameDto> values = {
            { MakeEntityRef( "frame", 0 ), MakeEntityRef( "frame-set", 0 ), 0, 0, 100, {}, true },
            // This second FrameMark proves the first continuous Player frame;
            // its own synthetic tail boundary is intentionally excluded.
            { MakeEntityRef( "frame", 1 ), MakeEntityRef( "frame-set", 0 ), 1, 100, 200, {}, true },
            { MakeEntityRef( "frame", 2 ), MakeEntityRef( "frame-set", 1 ), 0, 0, 100, {}, true }
        };
        const auto begin = std::min( range.offset, values.size() );
        const auto end = begin + std::min( range.limit, values.size() - begin );
        return { values.begin() + begin, values.begin() + end };
    }
    std::vector<JobDto> GetJobs() const override
    {
        auto values = tracy::query::test::FakeTraceSource::GetJobs();
        assert( !values.empty() );
        auto presentJob = values.back();
        presentJob.ref = MakeEntityRef( "job", 3 );
        presentJob.jobId = 3;
        presentJob.packedHandle = ( uint64_t( 2 ) << 32 ) | 7;
        presentJob.originFrameSequence = 2;
        presentJob.originFrameId = ( uint64_t( 1 ) << 48 ) | 2;
        values.emplace_back( std::move( presentJob ) );
        return values;
    }
    std::vector<CorrelatedFrameEventDto> GetCorrelatedFrameEvents() const override
    {
        if( readHook ) readHook( "correlated_frame" );
        auto values = tracy::query::test::FakeTraceSource::GetCorrelatedFrameEvents();
        const uint64_t presentFrameId = ( uint64_t( 1 ) << 48 ) | 2;
        values.push_back( { MakeEntityRef( "frame-identity-event", 4 ), presentFrameId, 2, 120,
            MakeEntityRef( "thread", 1 ), uint8_t( tracy::JnFrameDomain::Present ),
            uint8_t( tracy::JnFramePhase::Begin ), uint8_t( tracy::JnFrameFlags::Canonical ) } );
        values.push_back( { MakeEntityRef( "frame-identity-event", 5 ), presentFrameId, 2, 130,
            MakeEntityRef( "thread", 1 ), uint8_t( tracy::JnFrameDomain::Present ),
            uint8_t( tracy::JnFramePhase::End ), uint8_t( tracy::JnFrameFlags::Canonical ) } );
        if( wideCorrelatedPayload ) values.front().ref = std::string( 32 * 1024 * 1024, 'x' );
        return values;
    }
};

// Removing control-document ownership must fail cold Status and new Start
// under pressure; neither may silently report a missing scan or lose profile
// ownership once its parsing stack frame has returned.
void ManagerControlWorkspace(const std::filesystem::path& root,const std::filesystem::path& tracePath)
{
    constexpr uint64_t maximum=1024ull*1024*1024;
    auto workspace=std::make_shared<AnalysisWorkspaceBudget>(maximum,maximum/2);
    auto native=std::make_shared<SourceDegradedTrace>();
    auto profile=Profile(); profile["user_focus"]={std::string(32768,'w')};
    const auto validated=ValidateAndNormalizeAnalysisProfile(profile); assert(validated.valid);
    AnalysisScanStartRequest request;
    request.traceSessionId="control-workspace"; request.tracePath=tracePath; request.source=native;
    request.normalizedProfile=validated.normalized; request.profileIdentity=validated.profileSha256;
    std::atomic<unsigned> executions=0;
    AnalysisScanExecutor executor=[&](const auto& input,std::stop_token token,const auto& progress,auto& output,auto& error) {
        ++executions; return ExecuteDefaultAnalysisScan(input,token,progress,output,error);
    };
    bool protectedControl=true;
    {
        AnalysisWorkspaceReservation pressure(workspace,maximum-128);
        AnalysisScanManager denied(root/"control-denied",root/"control-denied-cache",std::string(64,'e'),
            AnalysisScanSourceResolver{},executor,workspace);
        bool rejected=false;
        try { denied.Start(request); }
        catch(const std::exception& e) { if(std::string(e.what()).find("workspace_budget")==std::string::npos) throw; rejected=true; }
        if(!rejected) std::cerr<<"new Start copied an uncharged wide profile\n";
        protectedControl&=rejected;
    }
    std::string scanId;
    {
        AnalysisScanManager warm(root/"control-state",root/"control-cache",std::string(64,'e'),
            AnalysisScanSourceResolver{},executor,workspace);
        scanId=warm.Start(request).scanId; WaitFor(warm,scanId,ScanState::Complete,6s);
        // Resume of an already complete entry only joins its finished worker;
        // no transient scan locals can influence this retained-owner check.
        assert(warm.Resume(scanId).completed);
        const auto held=workspace->Snapshot().currentBytes;
        if(held<65536+32768) std::cerr<<"completed entry retained an uncharged profile\n";
        protectedControl&=held>=65536+32768;
    }
    if(workspace->Snapshot().currentBytes!=0) throw std::runtime_error("Manager destruction must release its retained control documents");
    const auto beforeCold=executions.load();
    {
        AnalysisScanManager cold(root/"control-state",root/"control-cache",std::string(64,'e'),
            AnalysisScanSourceResolver{},executor,workspace);
        AnalysisWorkspaceReservation pressure(workspace,maximum-128);
        bool rejected=false;
        try { cold.Status(scanId); }
        catch(const std::exception& e) { if(std::string(e.what()).find("workspace_budget")==std::string::npos) throw; rejected=true; }
        if(!rejected) std::cerr<<"cold Status parsed control documents without workspace\n";
        protectedControl&=rejected;
        pressure.Resize(0);
        const auto recovered=cold.Status(scanId);
        const auto held=workspace->Snapshot().currentBytes;
        if(!recovered.completed || recovered.scanId!=scanId || executions!=beforeCold)
            throw std::runtime_error("Cold control reads must recover the same completed scan without source execution");
        if(held<65536+32768) std::cerr<<"cold entry released its profile reservation before its JSON owner\n";
        protectedControl&=held>=65536+32768;
    }
    if(workspace->Snapshot().currentBytes!=0) throw std::runtime_error("Cold Manager destruction leaked control workspace");
    if(!protectedControl) throw std::runtime_error("Manager control JSON must be admitted before copying/parsing and charged for its retained lifetime");
}

void ManagerColdBundleWorkspace(const std::filesystem::path& root,const std::filesystem::path& tracePath)
{
    constexpr uint64_t maximum=1024ull*1024*1024;
    const auto stateRoot=root/"cold-bundle-state",cacheRoot=root/"cold-bundle-cache";
    auto native=std::make_shared<SourceDegradedTrace>();
    const auto profile=ValidateAndNormalizeAnalysisProfile(Profile()); assert(profile.valid);
    AnalysisScanStartRequest request;
    request.traceSessionId="cold-bundle"; request.tracePath=tracePath; request.source=native;
    request.normalizedProfile=profile.normalized; request.profileIdentity=profile.profileSha256;
    std::string scanId; json expected;
    {
        AnalysisScanManager warm(stateRoot,cacheRoot,std::string(64,'e'),AnalysisScanSourceResolver{},ExecuteDefaultAnalysisScan);
        scanId=warm.Start(request).scanId; WaitFor(warm,scanId,ScanState::Complete,6s);
        expected=warm.Summary(scanId);
    }
    const auto statePath=stateRoot/"scans"/scanId/"scan-state.json";
    json state; {std::ifstream input(statePath,std::ios::binary); state=json::parse(input);}
    const auto generation=std::filesystem::path(state.at("aggregate_root").get<std::string>())/
        "generations"/state.at("neutral_generation").get<std::string>();
    const auto identity=state.at("aggregate_identity").get<std::string>();
    std::string payload;
    {
        AnalysisCacheTableReader header(generation/"bundle-header",identity,"analysis-neutral-bundle-v1");
        auto document=json::parse(header.GetAt(0).payload);
        document["extension"]=std::string(1024*1024,'h'); payload=document.dump();
    }
    std::filesystem::remove_all(generation/"bundle-header");
    {
        AnalysisCacheTableWriter header(generation/"bundle-header",identity,"analysis-neutral-bundle-v1");
        header.Append("header",payload); state["neutral_header_sha256"]=header.Commit().contentSha256;
    }
    {std::ofstream output(statePath,std::ios::binary|std::ios::trunc); output<<state.dump();}
    auto workspace=std::make_shared<AnalysisWorkspaceBudget>(maximum,maximum/2);
    AnalysisScanManager cold(stateRoot,cacheRoot,std::string(64,'e'),AnalysisScanSourceResolver{},ExecuteDefaultAnalysisScan,workspace);
    assert(cold.Status(scanId).completed);
    const auto held=workspace->Snapshot().currentBytes;
    AnalysisWorkspaceReservation pressure(workspace,maximum-held-24*1024*1024);
    bool rejected=false;
    try { cold.Summary(scanId); }
    catch(const std::exception& e) { if(std::string(e.what()).find("workspace_budget")==std::string::npos) throw; rejected=true; }
    pressure.Resize(0);
    if(cold.Summary(scanId)!=expected || !cold.Status(scanId).completed)
        throw std::runtime_error("Cold header budget must preserve the exact persisted summary after recovery");
    if(!rejected) throw std::runtime_error("Cold Manager parsed a wide bundle header without current JSON workspace");
}

// Count real process file reads on a wide, multi-block native fixture. Numeric
// merge tokens must not cause a random Context block read per signature.
void NativeContextReadLocality(const std::filesystem::path& root)
{
#ifdef _WIN32
    const auto readBytes=[] {
        IO_COUNTERS counters{};
        if(!GetProcessIoCounters(GetCurrentProcess(),&counters)) throw std::runtime_error("native I/O counter unavailable");
        return uint64_t(counters.ReadTransferCount);
    };
    auto source=std::make_shared<SourceDegradedTrace>(); source->wideSignatureCount=64;
    AnalysisScanExecutionRequest request;
    request.source=source; request.aggregateIdentity={source->GetTraceInfo().fingerprint,
        std::string(64,'e'),"1.35.0",NeutralScanAlgorithmId,NeutralAggregateSchemaVersion};
    request.temporaryRoot=root/"context-read-locality";
    const auto profile=ValidateAndNormalizeAnalysisProfile(Profile()); assert(profile.valid);
    request.normalizedProfile=profile.normalized; request.profileIdentity=profile.profileSha256;
    request.workspace=std::make_shared<AnalysisWorkspaceBudget>();
    const auto before=readBytes();
    AnalysisScanProducts products; std::string error;
    if(!ExecuteDefaultAnalysisScan(request,std::stop_token{},[](auto,auto,auto,auto){},products,error))
        throw std::runtime_error("native I/O fixture failed: "+error);
    const auto transferred=readBytes()-before;
    const auto bytes=AnalysisDiskUsage::Bytes(request.temporaryRoot);
    NeutralStatisticsCacheReader neutral(products.neutralCachePath,products.neutralCacheIdentity);
    if(neutral.SignatureCount()!=132) throw std::runtime_error("native I/O fixture must preserve both CPU FrameSets and fixed other domains");
    std::cerr<<"native context locality: read_bytes="<<transferred<<" cache_bytes="<<bytes<<'\n';
    if(transferred>128*1024*1024+8*bytes)
        throw std::runtime_error("native statistics must not reread wide Context blocks once per source signature");
#endif
}

// Diagnostic-only reproduction of the pre-existing L0 denominator rule.
// It reports both input cases; it does not weaken the audit or change policy.
void GpuFrameScopeProbe(const std::filesystem::path& root)
{
    for(int mode:{1,2}) {
        auto source=std::make_shared<SourceDegradedTrace>(); source->gpuFrameScopeProbe=mode;
        NeutralStatisticsInput numeric; numeric.temporaryRoot=root/("gpu-scope-numeric-"+std::to_string(mode));
        std::map<std::string,uint64_t> roots;
        std::set<std::pair<std::string,std::string>> present;
        GpuJobManagedScanOptions options; options.retainDetails=false; options.includeLogicalGpuSignatures=false;
        options.gpuZoneSink=[&](const GpuZoneScanFact& value) {
            if(!value.physicalTimingExact) return true;
            const auto scope="GPU.L0Segment:"+value.contextRef;
            if(value.depth==0) ++roots[scope];
            numeric.runs.push_back({"gpu",value.signatureId,scope,value.l0SegmentOrdinal,
                value.inclusiveNs,value.exclusiveNs,0,0,1,true,false});
            present.emplace(value.signatureId,scope); return true;
        };
        const auto scanned=GpuJobManagedScanner(*source).Scan(options);
        for(const auto& [signature,scope]:present) numeric.denominators.push_back({"gpu",signature,scope,roots[scope]});
        const auto result=BuildNeutralStatistics(numeric);
        json rows=json::array();
        for(const auto& row:result.signatures) rows.push_back({{"signature_id",row.signatureId},
            {"present_frames",row.presentFrameCount},{"denominator",row.completeFrameCount},
            {"inclusive_total",row.inclusive.whenPresent.total},{"exclusive_total",row.exclusive.whenPresent.total},
            {"complete_unavailable_reason",row.exclusive.perCompleteFrame.unavailableReason}});
        AnalysisScanExecutionRequest request; request.source=source;
        request.aggregateIdentity={source->GetTraceInfo().fingerprint,std::string(64,'e'),"1.35.0",NeutralScanAlgorithmId,NeutralAggregateSchemaVersion};
        request.temporaryRoot=root/("gpu-scope-default-"+std::to_string(mode));
        const auto profile=ValidateAndNormalizeAnalysisProfile(Profile());
        request.normalizedProfile=profile.normalized; request.profileIdentity=profile.profileSha256;
        AnalysisScanProducts products; std::string error;
        const auto ok=ExecuteDefaultAnalysisScan(request,std::stop_token{},[](auto,auto,auto,auto){},products,error);
        std::cout<<json{{"case",mode==1?"sibling_outside_l0":"sibling_contained"},
            {"physical_l0_count",scanned.physicalL0SegmentCount},{"accepted_roots",roots},
            {"statistics",rows},{"default_scan_succeeded",ok},{"default_error",error}}.dump()<<'\n';
    }
}

void GpuLocalQualityRecovery(const std::filesystem::path& root)
{
    struct Case {int mode; uint64_t present,complete,outside,total,zeros;};
    for(const auto test: {Case{1,2,1,1,1000000,0},Case{3,2,1,1,1000000,0},
        Case{4,2,1,1,1000000,0},Case{5,2,0,2,0,0},Case{6,1,2,0,1000000,1},Case{8,2,1,1,1000000,0}}) {
    auto source=std::make_shared<SourceDegradedTrace>();
    source->gpuFrameScopeProbe=test.mode; source->gpuProbeLongCosts=true;
    AnalysisScanExecutionRequest request; request.source=source;
    request.aggregateIdentity={source->GetTraceInfo().fingerprint,std::string(64,'e'),"1.35.0",NeutralScanAlgorithmId,NeutralAggregateSchemaVersion};
    request.temporaryRoot=root/("gpu-local-quality-"+std::to_string(test.mode));
    auto inputProfile=Profile(); inputProfile["user_focus"]=json::array();
    inputProfile["limits"]["query_memory_hard_bytes"]=34359738368ULL;
    const auto profile=ValidateAndNormalizeAnalysisProfile(inputProfile);
    request.normalizedProfile=profile.normalized; request.profileIdentity=profile.profileSha256;
    request.workspace=std::make_shared<AnalysisWorkspaceBudget>();
    AnalysisScanProducts products; std::string error;
    if(!ExecuteDefaultAnalysisScan(request,std::stop_token{},[](auto,auto,auto,auto){},products,error))
        throw std::runtime_error("A local invalid GPU L0 must not reject valid Pass observations and the other domains: "+error);
    NeutralStatisticsCacheReader reader(products.neutralCachePath,products.neutralCacheIdentity);
    bool found=false; std::string goodId;
    reader.VisitSignatureContexts([&](const NeutralSignatureAggregate* value,const PolicySignatureContext& context) {
        if(test.mode==8 && context.name=="GPU.Frame.Direct" && value && value->inclusive.whenPresent.count!=1)
            throw std::runtime_error("An invalid deeper descendant must not leave its ancestor residual labeled physically exact");
        if(context.name!="Good.Pass") return;
        found=true; goodId=context.signatureId;
        if(!value || !value->inclusive.whenPresent.exact || value->inclusive.whenPresent.count!=test.present ||
            value->inclusive.whenPresent.total!=int64_t(test.present*1000000) || value->completeFrameCount!=test.complete ||
            value->inclusive.perCompleteFrame.count!=test.complete || value->inclusive.perCompleteFrame.total!=int64_t(test.total) ||
            value->outsideCompleteFrameCount!=test.outside || value->inclusive.perCompleteFrame.zeroCount!=test.zeros ||
            value->inclusive.perCompleteFrame.exact!=(test.complete!=0))
            throw std::runtime_error("Valid Pass observations from both L0s must survive; complete-frame statistics use only the one complete L0");
        const auto rows=ReadPolicyFrameSeries(products.frameSeriesPath,context,1024*1024);
        if(rows.size()!=test.present || std::any_of(rows.begin(),rows.end(),[](const auto& row){return !row.exact || row.valueNs!=1000000;}))
            throw std::runtime_error("Local GPU cost evidence must retain both valid one-millisecond Pass observations");
    });
    if(!found || !products.aggregate.qualityComplete || products.aggregate.unreportedGapCount!=0)
        throw std::runtime_error("GPU source degradation must remain explicit without an internal transfer gap");
    if(test.mode==3 || test.mode==4 || test.mode==8) continue;
    const auto stateRoot=root/("gpu-quality-manager-"+std::to_string(test.mode));
    const auto cacheRoot=root/("gpu-quality-cache-"+std::to_string(test.mode));
    std::atomic<unsigned> executions=0;
    AnalysisScanExecutor executor=[&](const auto& r,std::stop_token token,const auto& progress,auto& p,auto& e) {
        ++executions; return ExecuteDefaultAnalysisScan(r,token,progress,p,e);
    };
    AnalysisProcessMemoryOptions memory;
    memory.sample=[]{return AnalysisProcessMemorySample{true,24ull*1024*1024*1024,24ull*1024*1024*1024};};
    std::string scanId; json summary,signatures,candidates,quality;
    {
        AnalysisScanManager manager(stateRoot,cacheRoot,std::string(64,'e'),AnalysisScanSourceResolver{},executor,{},memory);
        AnalysisScanStartRequest start;
        start.traceSessionId="gpu-local-quality"; start.tracePath=root/"fixture.tracy"; start.source=source;
        start.normalizedProfile=profile.normalized; start.profileIdentity=profile.profileSha256;
        scanId=manager.Start(start).scanId;
        const auto complete=WaitFor(manager,scanId,ScanState::Complete,8s);
        if(complete.processMemory.maximumBytes!=34359738368ULL)
            throw std::runtime_error("The explicitly expanded profile must reach the default scan and reads under the 32 GiB guard");
        summary=manager.Summary(scanId); quality=manager.Quality(scanId);
        signatures=manager.Signatures(scanId,100,"",{},json::object());
        candidates=manager.Candidates(scanId,100,"",{},json::object());
        bool selected=false;
        for(const auto& candidate:candidates.at("items")) if(candidate.at("signature_id")==goodId) {
            selected=candidate.at("selected").get<bool>();
            if(test.mode==1) {
                bool incompleteInterval=false;
                for(const auto& interval:candidate.at("representative_intervals"))
                    if(interval.at("l0_segment_ordinal")=="1")
                        incompleteInterval=interval.contains("l0_complete") && interval.at("l0_complete")==false;
                if(!incompleteInterval)
                    throw std::runtime_error("A valid Pass in an incomplete L0 must expose that interval's coverage limitation");
            }
            if(test.mode==5) {
                const auto& intervals=candidate.at("representative_intervals");
                if(intervals.size()!=2)
                    throw std::runtime_error("Missing L0 end timestamps must not erase the recorded root references needed to investigate valid child Passes");
                for(const auto& interval:intervals) {
                    const bool first=interval.at("l0_segment_ordinal")=="1";
                    if(interval.at("l0_complete")!=false || !interval.at("end_ns").is_null() ||
                        interval.at("begin_ns")!=(first?"0":"20000000") ||
                        interval.at("event_refs")!=json::array({first?"fake:gpu-zone:0":"fake:gpu-zone:3"}) ||
                        interval.at("timing_unavailable_reason")!="source_l0_timestamps_missing")
                        throw std::runtime_error("Incomplete GPU intervals must retain known endpoints and real references without inventing an end time");
                }
            }
        }
        if(!selected) throw std::runtime_error("Sparse or partially enclosed valid GPU Pass costs must remain selected without user focus");
        if(test.mode!=6 && quality.dump().find("gpu_l0_incomplete")==std::string::npos)
            throw std::runtime_error("A completed mixed-domain scan must still publish the GPU source limitation");
    }
    AnalysisScanManager cold(stateRoot,cacheRoot,std::string(64,'e'),AnalysisScanSourceResolver{},executor,{},memory);
    if(!cold.Status(scanId).completed || cold.Summary(scanId)!=summary || cold.Quality(scanId)!=quality ||
        cold.Signatures(scanId,100,"",{},json::object())!=signatures ||
        cold.Candidates(scanId,100,"",{},json::object())!=candidates || executions!=1)
        throw std::runtime_error("Cold mixed-quality reads must preserve every output and must not scan the source again");
    }
}

// Scale the native DTO -> default executor -> neutral -> policy -> Manager
// path. Only the synthetic source adapter differs from a real loaded Worker.
// One generated CPU identity has one 1 ns occurrence in each of two FrameSets.
void ScaleNativeManager(const std::filesystem::path& root,const std::filesystem::path& tracePath,uint64_t count)
{
    if(!count || count>1506514) throw std::runtime_error("native scale source count must be 1..1506514");
    const auto clockStart=std::chrono::steady_clock::now();
    const auto ms=[](auto a,auto b) {return std::chrono::duration_cast<std::chrono::milliseconds>(b-a).count();};
    auto source=std::make_shared<SourceDegradedTrace>();
    source->wideSignatureCount=size_t(count); source->compactScaleNames=true;
    auto inputProfile=Profile(); inputProfile["user_focus"]=json::array();
    const auto profile=ValidateAndNormalizeAnalysisProfile(inputProfile); assert(profile.valid);
    auto workspace=std::make_shared<AnalysisWorkspaceBudget>();
    AnalysisProcessMemoryOptions memory; memory.maximumBytes=4ull*1024*1024*1024;
    std::atomic<uint64_t> executions=0;
    AnalysisScanExecutor executor=[&](const auto& request,std::stop_token token,const auto& progress,auto& products,auto& error) {
        ++executions; std::string previous;
        return ExecuteDefaultAnalysisScan(request,token,[&](auto state,auto completed,auto total,std::string_view stage) {
            if(stage!=previous) {previous=stage; std::cerr<<"native scale "<<stage<<" "<<completed<<"/"<<total<<'\n';}
            progress(state,completed,total,stage);
        },products,error);
    };
    const auto stateRoot=root/"native-state",cacheRoot=root/"native-cache";
    std::string scanId,signatureHash,candidateHash;
    json expectedSummary,expectedQuality;
    std::vector<std::pair<uint64_t,json>> signatureSamples,candidateSamples;
    uint64_t signatureRows=0,candidateRows=0,selected=0,playerRows=0,renderRows=0;
    int64_t startMs=0,scanMs=0,warmReadMs=0,coldReadMs=0,coldStartMs=0;
    AnalysisProcessMemorySnapshot scanMemory;
    {
        AnalysisScanManager manager(stateRoot,cacheRoot,std::string(64,'e'),AnalysisScanSourceResolver{},executor,workspace,memory);
        AnalysisScanStartRequest request;
        request.traceSessionId="native-scale"; request.tracePath=tracePath; request.source=source;
        request.normalizedProfile=profile.normalized; request.profileIdentity=profile.profileSha256;
        const auto beforeStart=std::chrono::steady_clock::now();
        scanId=manager.Start(request).scanId;
        startMs=ms(beforeStart,std::chrono::steady_clock::now());
        std::string previous;
        for(;;) {
            const auto state=manager.Status(scanId);
            if(state.stage!=previous) {previous=state.stage; std::cerr<<"native manager "<<previous<<'\n';}
            if(state.completed) {scanMemory=manager.Resume(scanId).processMemory; break;}
            if(state.state==ScanState::Failed || state.state==ScanState::CancelledResumable)
                throw std::runtime_error("native scale scan did not complete: "+state.stage+":"+state.error);
            std::this_thread::sleep_for(50ms);
        }
        const auto scanned=std::chrono::steady_clock::now(); scanMs=ms(clockStart,scanned);
        expectedSummary=manager.Summary(scanId); expectedQuality=manager.Quality(scanId);
        if(!expectedSummary.at("quality").at("complete").get<bool>() ||
            expectedSummary.at("quality").at("unreported_gap_count")!="0")
            throw std::runtime_error("native scale neutral audit must remain complete");
        Sha256Builder signatures;
        std::tuple<std::string,std::string,std::string> previousKey;
        for(std::string cursor;;) {
            const auto page=manager.Signatures(scanId,1000,cursor,{},json::object());
            const auto total=std::stoull(page.at("page").at("total").get<std::string>());
            if(std::stoull(page.at("page").at("cursor").get<std::string>())!=signatureRows)
                throw std::runtime_error("native signature page must begin at the exact next row");
            for(const auto& row:page.at("items")) {
                const auto domain=row.at("domain").get<std::string>(),id=row.at("signature_id").get<std::string>(),scope=row.at("frame_scope").get<std::string>();
                const auto key=std::make_tuple(domain,id,scope);
                if(signatureRows && !(previousKey<key)) throw std::runtime_error("native signatures must be strictly ordered and unique across pages");
                previousKey=key;
                if(domain=="cpu") {
                    if(scope=="fake:frame-set:0") ++playerRows; else if(scope=="fake:frame-set:1") ++renderRows;
                    else throw std::runtime_error("native CPU scale has only the two real fixture FrameSets: "+scope);
                    if(row.at("complete_frame_count")!="1" || row.at("present_frame_count")!="1" ||
                        row.at("occurrence_count")!="1" || row.at("unknown_frame_count")!="0" || !row.at("exact").get<bool>())
                        throw std::runtime_error("native CPU signature must retain exactly one known occurrence");
                    for(const char* metric:{"inclusive","exclusive"}) {
                        const auto& distribution=row.at(metric).at("per_complete_frame");
                        if(distribution.at("count")!="1" || distribution.at("total_ns")!="1" ||
                            std::stod(distribution.at("median_ns").get<std::string>())!=1 ||
                            std::stod(distribution.at("p95_ns").get<std::string>())!=1)
                            throw std::runtime_error("native 1 ns literal distribution changed");
                    }
                }
                if(signatureRows==0 || signatureRows==total/2 || signatureRows+1==total)
                    signatureSamples.emplace_back(signatureRows,row);
                const auto payload=row.dump(); signatures.Update(payload.data(),payload.size()); ++signatureRows;
                if(signatureRows%100000==0) std::cerr<<"native verified signatures "<<signatureRows<<"/"<<total<<'\n';
            }
            if(page.at("page").at("done").get<bool>()) {
                if(signatureRows!=total) throw std::runtime_error("native signature pagination must cover the whole table");
                break;
            }
            cursor=page.at("page").at("next_cursor").get<std::string>();
        }
        if(playerRows!=count || renderRows!=count) throw std::runtime_error("native CPU stable identity count must be exact independently in both FrameSets");
        signatureHash=signatures.FinalHex(); Sha256Builder candidates;
        for(std::string cursor;;) {
            const auto page=manager.Candidates(scanId,1000,cursor,{},json::object());
            const auto total=std::stoull(page.at("page").at("total").get<std::string>());
            if(std::stoull(page.at("page").at("cursor").get<std::string>())!=candidateRows)
                throw std::runtime_error("native candidate page must begin at the exact next row");
            for(const auto& row:page.at("items")) {
                if(row.at("selected").get<bool>()) ++selected;
                if(candidateRows==0 || candidateRows==total/2 || candidateRows+1==total)
                    candidateSamples.emplace_back(candidateRows,row);
                const auto payload=row.dump(); candidates.Update(payload.data(),payload.size()); ++candidateRows;
                if(candidateRows%100000==0) std::cerr<<"native verified candidates "<<candidateRows<<"/"<<total<<'\n';
            }
            if(page.at("page").at("done").get<bool>()) {
                if(candidateRows!=total) throw std::runtime_error("native candidate pagination must cover the whole table");
                break;
            }
            cursor=page.at("page").at("next_cursor").get<std::string>();
        }
        candidateHash=candidates.FinalHex();
        if(candidateRows!=expectedSummary.at("backlog").at("total").get<uint64_t>() ||
            selected!=expectedSummary.at("backlog").at("selected").get<uint64_t>())
            throw std::runtime_error("native candidate pages must account for every selected and non-selected family");
        // The unchanged non-CPU fixture contributes four aggregates and three
        // mandatory candidates. Each scoped 1 ns CPU signature contributes one
        // global family; discretionary CPU selection has one 50-family quota.
        if(signatureRows!=count*2+4 || candidateRows!=count*2+3 || selected!=std::min<uint64_t>(50,count*2)+3)
            throw std::runtime_error("native synthetic domain/family totals changed from the literal fixture contract");
        warmReadMs=ms(scanned,std::chrono::steady_clock::now());
    }
    if(workspace->Snapshot().currentBytes) throw std::runtime_error("native warm Manager left retained analysis workspace");
    const auto coldBegin=std::chrono::steady_clock::now();
    {
        AnalysisScanManager cold(stateRoot,cacheRoot,std::string(64,'e'),AnalysisScanSourceResolver{},executor,workspace,memory);
        AnalysisScanStartRequest request;
        request.traceSessionId="native-scale"; request.tracePath=tracePath; request.source=source;
        request.normalizedProfile=profile.normalized; request.profileIdentity=profile.profileSha256;
        const auto beforeStart=std::chrono::steady_clock::now();
        if(!cold.Start(request).completed) throw std::runtime_error("native cold Start must reuse the completed generation");
        coldStartMs=ms(beforeStart,std::chrono::steady_clock::now());
        if(cold.Summary(scanId)!=expectedSummary || cold.Quality(scanId)!=expectedQuality)
            throw std::runtime_error("native cold summary and quality must remain identical");
        for(const auto& [ordinal,row]:signatureSamples)
            if(cold.Signatures(scanId,1,std::to_string(ordinal),{},json::object()).at("items").at(0)!=row)
                throw std::runtime_error("native cold first/middle/last signature changed");
        for(const auto& [ordinal,row]:candidateSamples) {
            if(cold.Candidates(scanId,1,std::to_string(ordinal),{},json::object()).at("items").at(0)!=row)
                throw std::runtime_error("native cold first/middle/last candidate page changed");
            const auto id=row.at("candidate_id").get<std::string>();
            if(cold.Candidate(scanId,id)!=row) throw std::runtime_error("native indexed candidate changed after cold reopening");
            const auto representatives=cold.RepresentativeFrames(scanId,id);
            if(representatives.at("frames")!=row.at("representative_frame_details"))
                throw std::runtime_error("native representative frame contract changed");
        }
    }
    coldReadMs=ms(coldBegin,std::chrono::steady_clock::now());
    if(executions!=1 || workspace->Snapshot().currentBytes) throw std::runtime_error("native cold reuse must neither rescan nor leak workspace");
    const auto budget=workspace->Snapshot();
    std::cout<<json{{"mode","native-manager"},{"records",count},{"cpu_source_signatures",count},
        {"cpu_statistics_rows",playerRows+renderRows},{"neutral_signature_count",signatureRows},{"candidate_records",candidateRows},
        {"selected",selected},{"start_ms",startMs},{"cold_start_ms",coldStartMs},{"scan_ms",scanMs},
        {"warm_full_paging_ms",warmReadMs},{"cold_sampled_read_ms",coldReadMs},{"source_executions",executions.load()},
        {"workspace_peak_bytes",budget.peakBytes},{"workspace_maximum_bytes",budget.maximumBytes},
        {"workspace_rejections",budget.rejectedReservations},{"process_memory",AnalysisProcessMemorySnapshotJson(scanMemory)},
        {"signature_response_sha256",signatureHash},{"candidate_response_sha256",candidateHash},
        {"logical_cache_bytes",AnalysisDiskUsage::Bytes(cacheRoot)+AnalysisDiskUsage::Bytes(stateRoot/"scans")},
        {"cold_validation","summary_quality_first_middle_last_pages_and_candidate_details"},{"verified",true}}.dump()<<'\n';
}

}

int main(int argc,char** argv)
{
    std::set_terminate([] {
        std::cerr << "Unhandled test failure in " << testStage.load() << ": ";
        try { if(const auto e=std::current_exception()) std::rethrow_exception(e); }
        catch(const std::exception& e) { std::cerr << e.what(); }
        catch(...) { std::cerr << "non-standard exception"; }
        std::cerr << '\n'; std::_Exit(3);
    });
    {
        json large = json::array();
        for( size_t index = 0; index < 100; ++index )
            large.push_back( { { "index", std::to_string( index ) },
                { "payload", std::string( 64 * 1024, char( 'a' + index % 26 ) ) } } );
        size_t observed = 0;
        std::string cursor;
        bool observedByteLimit = false;
        for( ;; )
        {
            const auto page = PaginateAnalysisScanItems( large, 1000, cursor, {}, json::object() );
            assert( page.dump().size() < 4 * 1024 * 1024 );
            observed += page.at( "items" ).size();
            observedByteLimit = observedByteLimit || page.at( "page" ).at( "byte_limited" ).get<bool>();
            if( page.at( "page" ).at( "done" ).get<bool>() ) break;
            cursor = page.at( "page" ).at( "next_cursor" ).get<std::string>();
            assert( !cursor.empty() );
        }
        assert( observed == large.size() );
        assert( observedByteLimit );
    }

    const auto root = std::filesystem::temp_directory_path() /
        ( "jn-tracy-analysis-scan-manager-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count() ) );
    const auto cache = root / "cache";
    const auto tracePath = root / "fixture.tracy";
    std::filesystem::create_directories( cache );
    { std::ofstream trace( tracePath, std::ios::binary ); trace << "fixture"; }

    const auto validation = ValidateAndNormalizeAnalysisProfile( Profile() );
    assert( validation.valid );
    if(argc==2 && std::string(argv[1])=="--gpu-frame-scope-probe") {
        testStage="GPU L0 denominator diagnostic"; GpuFrameScopeProbe(root); RemoveTestRoot(root); return 0;
    }
    if(argc==2 && std::string(argv[1])=="--gpu-local-quality-only") {
        testStage="GPU local quality recovery"; GpuLocalQualityRecovery(root); RemoveTestRoot(root); return 0;
    }
    if(argc==2 && std::string(argv[1])=="--native-io-only") {
        testStage="native Context read locality"; NativeContextReadLocality(root);
        RemoveTestRoot(root); return 0;
    }
    if(argc==3 && std::string(argv[1])=="--scale-native") {
        testStage="native Manager scale"; ScaleNativeManager(root,tracePath,std::stoull(argv[2]));
        RemoveTestRoot(root); return 0;
    }
    if(argc==1) {
        testStage="native Context read locality"; NativeContextReadLocality(root);
        testStage="GPU local quality recovery"; GpuLocalQualityRecovery(root);
    }
    testStage="manager cold bundle workspace"; ManagerColdBundleWorkspace(root,tracePath);
    if(argc==2 && std::string(argv[1])=="--cold-bundle-only") {
        RemoveTestRoot(root); return 0;
    }
    testStage="manager control workspace"; ManagerControlWorkspace(root,tracePath);
    if(argc==2 && std::string(argv[1])=="--control-json-only") {
        RemoveTestRoot(root); return 0;
    }
    testStage="bundle metadata"; BundleMetadata(root);
    testStage="bundle JSON workspace"; BundleJsonWorkspace(root);
    if(argc==2 && std::string(argv[1])=="--bundle-json-only") {
        RemoveTestRoot(root); return 0;
    }

    // The default fake trace intentionally persists a producer drop. A full
    // native scan must remain usable, preserve the invalid source domain, and
    // leave the neutral aggregate's own integrity audit complete.
    {
        testStage="default producer";
        auto degraded = std::make_shared<SourceDegradedTrace>();
        AnalysisScanExecutionRequest request;
        request.source = degraded;
        request.aggregateIdentity = { degraded->GetTraceInfo().fingerprint,
            std::string( 64, 'e' ), "1.35.0", NeutralScanAlgorithmId, NeutralAggregateSchemaVersion };
        request.temporaryRoot = root / "source-degraded-default-scan";
        request.normalizedProfile = validation.normalized;
        request.profileIdentity = validation.profileSha256;
        request.workspace=std::make_shared<AnalysisWorkspaceBudget>(1024ull*1024*1024,512ull*1024*1024);
        AnalysisScanProducts products;
        std::string scanError;
        std::stop_source stop;
        for(const auto* pressureDomain : { "io", "correlated_frame" })
        {
            testStage="default system summary budget";
            auto source=std::make_shared<SourceDegradedTrace>();
            source->wideCorrelatedPayload=std::string_view(pressureDomain)=="correlated_frame";
            auto bounded=request; bounded.source=source; bounded.temporaryRoot=root/(std::string("default-summary-budget-")+pressureDomain);
            bounded.workspace=std::make_shared<AnalysisWorkspaceBudget>(128*1024*1024,64*1024*1024);
            AnalysisWorkspaceReservation pressure(bounded.workspace);
            bool active=false; size_t laterReads=0;
            source->readHook=[&](std::string_view domain) {
                if(active) ++laterReads;
                else if(domain==pressureDomain) {
                    active=true;
                    const uint64_t allowance=source->wideCorrelatedPayload ? 64*1024*1024 : 128;
                    const auto current=bounded.workspace->Snapshot().currentBytes;
                    assert(current+allowance<=128*1024*1024);
                    pressure.Resize(128*1024*1024-current-allowance);
                }
            };
            AnalysisScanProducts rejected;
            const bool ok=ExecuteDefaultAnalysisScan(bounded,stop.get_token(),
                [](ScanState,uint64_t,uint64_t,std::string_view){},rejected,scanError);
            if(ok || !active || laterReads!=0 || scanError.find("workspace_budget")==std::string::npos)
                throw std::runtime_error(std::string("Default scan bypassed shared budget at ")+pressureDomain+": later_reads="+std::to_string(laterReads)+" error="+scanError);
            pressure.Resize(0); rejected={};
            assert(bounded.workspace->Snapshot().currentBytes==0);
        }
        {
            testStage="streamed GPU Context working set";
            auto many=std::make_shared<SourceDegradedTrace>(); many->wideGpuSignatureCount=128;
            auto bounded=request; bounded.source=many; bounded.temporaryRoot=root/"default-gpu-context-stream";
            bounded.workspace=std::make_shared<AnalysisWorkspaceBudget>(128*1024*1024,64*1024*1024);
            AnalysisScanProducts streamed;
            if(!ExecuteDefaultAnalysisScan(bounded,stop.get_token(),[](ScanState,uint64_t,uint64_t,std::string_view){},streamed,scanError))
                throw std::runtime_error("GPU Context streaming fixture must complete within 128 MiB: "+scanError);
            assert(streamed.signatureContexts.empty() && streamed.aggregate.signatures.empty());
            NeutralStatisticsCacheReader reader(streamed.neutralCachePath,streamed.neutralCacheIdentity);
            uint64_t gpuCount=0;
            reader.VisitSignatureContexts([&](const NeutralSignatureAggregate* aggregate,const PolicySignatureContext& context) {
                if(context.domain!="gpu") return;
                ++gpuCount;
                assert(aggregate && aggregate->inclusive.perCompleteFrame.total==1);
                assert(context.name=="WideGpu"+std::string(64*1024,'x') && context.path==context.name);
                assert(context.observationUnit=="l0_segment" && !context.threadOrQueue.empty());
                assert(context.frameSeriesComplete && context.seriesCount==1 && context.frames.size()==1);
                assert(context.frames[0].structureKey==context.path && context.frames[0].exact);
            });
            assert(gpuCount==128);
            assert(bounded.workspace->Snapshot().peakBytes<=128*1024*1024);
        }
        {
            auto tight=request;
            tight.temporaryRoot=root/"default-statistics-budget";
            tight.workspace=std::make_shared<AnalysisWorkspaceBudget>(1024*1024,512*1024);
            AnalysisScanProducts rejected;
            size_t stages=0;
            const auto ok=ExecuteDefaultAnalysisScan(tight,stop.get_token(),
                [&](ScanState,uint64_t,uint64_t,std::string_view){++stages;},rejected,scanError);
            if(ok || stages!=0 || scanError.find("workspace_budget")==std::string::npos)
                throw std::runtime_error("Default scan started CPU traversal without reserving statistics buffers");
            assert(tight.workspace->Snapshot().currentBytes==0);
        }
        {
            auto wide=std::make_shared<SourceDegradedTrace>(); wide->nestedCpu=true; wide->wideNames=true;
            auto bounded=request; bounded.source=wide; bounded.temporaryRoot=root/"default-context-budget";
            bounded.workspace=std::make_shared<AnalysisWorkspaceBudget>(20*1024*1024,10*1024*1024);
            AnalysisScanProducts rejected;
            bool enteredGpu=false;
            const auto ok=ExecuteDefaultAnalysisScan(bounded,stop.get_token(),
                [&](ScanState,uint64_t,uint64_t,std::string_view stage){if(stage=="gpu_job_managed_relation_scan") enteredGpu=true;},
                rejected,scanError);
            if(ok || enteredGpu || scanError.find("workspace_budget")==std::string::npos)
                throw std::runtime_error("CPU Context strings bypassed shared budget before GPU stage");
            rejected={};
            assert(bounded.workspace->Snapshot().currentBytes==0);
        }
        {
            testStage="streamed CPU Context working set";
            auto many=std::make_shared<SourceDegradedTrace>(); many->wideSignatureCount=128;
            auto bounded=request; bounded.source=many; bounded.temporaryRoot=root/"default-cpu-context-stream";
            bounded.workspace=std::make_shared<AnalysisWorkspaceBudget>(128*1024*1024,64*1024*1024);
            AnalysisScanProducts streamed;
            if(!ExecuteDefaultAnalysisScan(bounded,stop.get_token(),[](ScanState,uint64_t,uint64_t,std::string_view){},streamed,scanError))
                throw std::runtime_error("CPU Context streaming fixture must complete within 128 MiB: "+scanError);
            assert(streamed.signatureContexts.empty() && streamed.aggregate.signatures.empty());
            NeutralStatisticsCacheReader reader(streamed.neutralCachePath,streamed.neutralCacheIdentity);
            uint64_t cpuCount=0;
            reader.VisitSignatureContexts([&](const NeutralSignatureAggregate* aggregate,const PolicySignatureContext& context) {
                if(context.domain!="cpu" || context.frameRoot) return;
                ++cpuCount;
                assert(aggregate && aggregate->inclusive.perCompleteFrame.total==1);
                assert(context.name=="Wide"+std::string(64*1024,'x') && context.path==context.name);
                assert(context.frameSeriesComplete && context.seriesCount==1 && context.frames.size()==1);
                assert(context.frames[0].structureKey==context.path && context.frames[0].exact);
            });
            assert(cpuCount==256); // 128 distinct source identities in two independent FrameSets.
            assert(bounded.workspace->Snapshot().peakBytes<=128*1024*1024);
        }
        testStage="default producer";
        if( !ExecuteDefaultAnalysisScan( request, stop.get_token(),
            []( ScanState, uint64_t, uint64_t, std::string_view ) {},
            products, scanError ) )
        { std::cerr << "Default scan failed: " << scanError << '\n'; return 1; }
        assert( scanError.empty() );
        if( !products.aggregate.signatures.empty() || !products.signatureContexts.empty() )
        { std::cerr << "Default scan must return cache handles, not all signature statistics/contexts\n"; return 1; }
        if(request.workspace->Snapshot().currentBytes==0)
            throw std::runtime_error("Default producer returned Timeline metadata without workspace ownership");
        ReadSmallFixture( products );
        assert( products.aggregate.qualityComplete );
        assert( products.aggregate.unreportedGapCount == 0 );
        assert( std::count_if( products.aggregate.signatures.begin(),
            products.aggregate.signatures.end(), []( const auto& value ) {
                return value.domain == "cpu";
            } ) >= 2 );
        const auto telemetry = std::find_if( products.aggregate.domains.begin(),
            products.aggregate.domains.end(), []( const auto& value ) {
                return value.domain == "telemetry";
            } );
        assert( telemetry != products.aggregate.domains.end() );
        assert( telemetry->status == "invalid" );
        assert( telemetry->qualityComplete );
        const auto memory = std::find_if( products.aggregate.domains.begin(),
            products.aggregate.domains.end(), []( const auto& value ) {
                return value.domain == "memory";
            } );
        assert( memory != products.aggregate.domains.end() );
        assert( memory->present );
        assert( memory->status == "complete" );
        assert( memory->inputCount == 1 );
        assert( memory->consumedInputCount == 1 );
        const auto jobAggregate = std::find_if( products.aggregate.signatures.begin(),
            products.aggregate.signatures.end(), []( const auto& value ) {
                return value.domain == "job" && value.frameScope == "Player.Frame";
            } );
        assert( jobAggregate != products.aggregate.signatures.end() );
        // SourceDegradedTrace contains one proven continuous Player frame and
        // independent Render and Present frames.  The Present frame schedules
        // another exact occurrence of the same job signature. Job per-frame
        // statistics must use only Player.Frame rather than treating every
        // correlated origin identity as a Player frame.
        assert( jobAggregate->completeFrameCount == 1 );
        assert( jobAggregate->presentFrameCount == 1 );
        if( jobAggregate->criticalPath.perCompleteFrame.total != 0 )
        { std::cerr << "Job execution is not a proven critical path\n"; return 1; }
        degraded->nestedCpu = true;
        request.temporaryRoot = root / "nested-cpu-paths";
        AnalysisScanProducts nested;
        assert( ExecuteDefaultAnalysisScan( request, stop.get_token(),
            []( ScanState, uint64_t, uint64_t, std::string_view ) {}, nested, scanError ) );
        ReadSmallFixture( nested );
        size_t workPaths = 0;
        for( const auto& context : nested.signatureContexts )
            if( context.domain == "cpu" && context.name == "Work" )
            {
                ++workPaths;
                if( !context.frameSeriesComplete || context.seriesCount == 0 || context.seriesSha256.size() != 64 || context.threadOrQueue.empty() )
                { std::cerr << "Real CPU scan must persist a complete checked frame series, not representatives\n"; return 1; }
                if( context.path.find( "Parent" ) == std::string::npos ||
                    context.familyId == context.parentSignatureId )
                { std::cerr << "Candidate identity must preserve path, not merge sibling families\n"; return 1; }
            }
        if( workPaths != 4 )
        { std::cerr << "Expected two distinct Work paths in each of two FrameSets\n"; return 1; }
        if( std::none_of( nested.signatureContexts.begin(), nested.signatureContexts.end(), []( const auto& c ) {
            return c.domain == "gpu" && c.frameScope.starts_with( "GPU.L0Segment:" ) && c.frameSeriesComplete;
        } ) ) { std::cerr << "Physical GPU work must survive missing origin-frame linkage\n"; return 1; }
        degraded->unknownCpu=true;
        request.temporaryRoot=root/"unknown-cpu-representatives";
        AnalysisScanProducts unknown;
        if(!ExecuteDefaultAnalysisScan(request,stop.get_token(),[](ScanState,uint64_t,uint64_t,std::string_view){},unknown,scanError))
            throw std::runtime_error(scanError);
        NeutralStatisticsCacheReader unknownReader(unknown.neutralCachePath,unknown.neutralCacheIdentity);
        bool observedUnknown=false;
        unknownReader.VisitSignatureContexts([&](const NeutralSignatureAggregate*,const PolicySignatureContext& context) {
            if(context.domain!="cpu" || context.seriesCount==0) return;
            const auto full=ReadPolicyFrameSeries(unknown.frameSeriesPath,context,32*1024*1024);
            for(const auto& frame:full) if(!frame.exact) observedUnknown=true;
            for(const auto& representative:context.frames) {
                const auto source=std::find_if(full.begin(),full.end(),[&](const auto& f){return f.frameIndex==representative.frameIndex;});
                if(source==full.end() || source->exact!=representative.exact)
                    throw std::runtime_error("Default producer changed unknown frame evidence to exact");
            }
        });
        if(!observedUnknown) throw std::runtime_error("Unknown CPU fixture must emit an unknown frame");
    }

    // Exercise the actual default producer through manager publication and
    // policy-only reuse, not just an injected resident fixture executor.
    {
        testStage="native manager";
        auto native = std::make_shared<SourceDegradedTrace>();
        size_t nativeExecutions = 0;
        AnalysisScanExecutor nativeExecutor = [&]( const AnalysisScanExecutionRequest& request,
            std::stop_token token, const AnalysisScanProgressCallback& progress,
            AnalysisScanProducts& products, std::string& error ) {
            ++nativeExecutions;
            return ExecuteDefaultAnalysisScan( request, token, progress, products, error );
        };
        AnalysisScanManager manager( root / "native-manager", cache / "native", std::string( 64, 'e' ),
            [native]( const auto&, std::stop_token ) { return native; }, nativeExecutor );
        AnalysisScanStartRequest request;
        request.traceSessionId = "native-fixture"; request.tracePath = tracePath; request.source = native;
        request.normalizedProfile = validation.normalized; request.profileIdentity = validation.profileSha256;
        const auto started = manager.Start( request );
        WaitFor( manager, started.scanId, ScanState::Complete, 6s );
        assert( nativeExecutions == 1 );
        const auto signatures = manager.Signatures( started.scanId, 1000, "", {}, json::object() );
        assert( signatures.at( "items" ).size() >= 2 );
        const auto candidates = manager.Candidates( started.scanId, 1000, "", {}, json::object() );
        assert( !candidates.at( "items" ).empty() );
        const auto selected = manager.Candidates( started.scanId, 1000, "", {}, {{"selected",true}} );
        for( const auto& item : selected.at( "items" ) ) assert( item.at( "selected" ) == true );
        assert( manager.Summary( started.scanId ).at( "quality" ).at( "complete" ) == true );
        const auto changed = ValidateAndNormalizeAnalysisProfile( Profile( 3 ) );
        request.normalizedProfile = changed.normalized; request.profileIdentity = changed.profileSha256;
        const auto second = manager.Start( request );
        WaitFor( manager, second.scanId, ScanState::Complete, 6s );
        assert( nativeExecutions == 1 );
        assert( !manager.Candidates( second.scanId, 1, "", {}, json::object() ).at( "items" ).empty() );
        const auto cancelComplete=manager.Cancel(started.scanId);
        if(!cancelComplete.completed || cancelComplete.state!=ScanState::Complete)
            throw std::runtime_error("Cancelling a completed scan must not invalidate its completed state");
        assert(manager.Candidate(started.scanId,candidates.at("items")[0].at("candidate_id").get<std::string>())==candidates.at("items")[0]);
        const NeutralAggregateIdentity nativeIdentity{native->GetTraceInfo().fingerprint,
            std::string(64,'e'),"1.35.0",NeutralScanAlgorithmId,NeutralAggregateSchemaVersion};
        const auto pointer=cache/"native"/AnalysisScanCacheFormatId/ComputeNeutralAggregateIdentity(nativeIdentity)/"current.json";
        {
            std::ofstream oversized(pointer,std::ios::binary|std::ios::app);
            if(!oversized) throw std::runtime_error("Oversized pointer fixture could not open its file");
            for(size_t i=0;i<5*1024;++i) oversized<<std::string(1024,' ');
            oversized.flush(); if(!oversized) throw std::runtime_error("Oversized pointer fixture write failed");
        }
        const auto malformedProfile=ValidateAndNormalizeAnalysisProfile(Profile(4));
        request.normalizedProfile=malformedProfile.normalized; request.profileIdentity=malformedProfile.profileSha256;
        const auto third=manager.Start(request);
        const auto rejected=WaitFor(manager,third.scanId,ScanState::Failed,3s);
        if(rejected.error.find("small_document_budget")==std::string::npos)
            throw std::runtime_error("Oversized control documents must fail before parsing");
    }
    {
        testStage="manager shared workspace budget";
        auto native=std::make_shared<SourceDegradedTrace>();
        auto workspace=std::make_shared<AnalysisWorkspaceBudget>(1024ull*1024*1024,512ull*1024*1024);
        // Admit the control request, but not the first scan/cache workspace.
        AnalysisWorkspaceReservation occupied(workspace,1024ull*1024*1024-1024*1024);
        AnalysisScanManager manager(root/"budget-manager",cache/"budget",std::string(64,'e'),
            [native](const auto&,std::stop_token){return native;},ExecuteDefaultAnalysisScan,workspace);
        AnalysisScanStartRequest request;
        request.traceSessionId="budget-fixture"; request.tracePath=tracePath; request.source=native;
        request.normalizedProfile=validation.normalized; request.profileIdentity=validation.profileSha256;
        const auto started=manager.Start(request);
        const auto paused=WaitFor(manager,started.scanId,ScanState::CancelledResumable,6s);
        if(paused.completed || !paused.resumable || paused.error.find("workspace_budget")==std::string::npos)
            throw std::runtime_error("Exhausted shared workspace must pause the real scan with an explicit reason");
        const NeutralAggregateIdentity identity{native->GetTraceInfo().fingerprint,
            std::string(64,'e'),"1.35.0",NeutralScanAlgorithmId,NeutralAggregateSchemaVersion};
        if(std::filesystem::exists(cache/"budget"/AnalysisScanCacheFormatId/ComputeNeutralAggregateIdentity(identity)/"current.json"))
            throw std::runtime_error("Over-budget default scan must not publish a completed neutral pointer");
        occupied.Resize(0);
        manager.Resume(started.scanId);
        WaitFor(manager,started.scanId,ScanState::Complete,6s);
        if(manager.Candidates(started.scanId,1,"",{},json::object()).at("items").empty())
            throw std::runtime_error("Budget-paused scan must complete after capacity becomes available");
    }
    {
        testStage="manager response workspace";
        auto native=std::make_shared<SourceDegradedTrace>();
        auto workspace=std::make_shared<AnalysisWorkspaceBudget>(1024ull*1024*1024,512ull*1024*1024);
        std::atomic<bool> observe=false; std::atomic<uint64_t> finalSampleWorkspace=0;
        AnalysisProcessMemoryOptions options; options.interval=1s;
        options.sample=[&] {
            if(observe) finalSampleWorkspace=workspace->Snapshot().currentBytes;
            return AnalysisProcessMemorySample{true,128,128};
        };
        AnalysisScanManager manager(root/"response-workspace",cache/"response-workspace",std::string(64,'e'),
            AnalysisScanSourceResolver{},ExecuteDefaultAnalysisScan,workspace,options);
        AnalysisScanStartRequest request;
        request.traceSessionId="response-workspace"; request.tracePath=tracePath; request.source=native;
        request.normalizedProfile=validation.normalized; request.profileIdentity=validation.profileSha256;
        const auto started=manager.Start(request); WaitFor(manager,started.scanId,ScanState::Complete,6s);
        const auto expectedSummary=manager.Summary(started.scanId),expectedQuality=manager.Quality(started.scanId);
        const auto candidateId=manager.Candidates(started.scanId,1,"",{},json::object()).at("items")[0].at("candidate_id").get<std::string>();
        const auto expectedCandidate=manager.Candidate(started.scanId,candidateId);
        const auto expectedFrames=manager.RepresentativeFrames(started.scanId,candidateId);
        const auto resident=workspace->Snapshot().currentBytes;
        AnalysisWorkspaceReservation pressure(workspace,1024ull*1024*1024-resident-128);
        bool protectedResponses=true;
        for(const std::string mode:{"summary","quality"}) {
            bool rejected=false;
            try { if(mode=="summary") manager.Summary(started.scanId); else manager.Quality(started.scanId); }
            catch(const std::exception& e) { if(std::string(e.what()).find("workspace_budget")==std::string::npos) throw; rejected=true; }
            if(!rejected) std::cerr<<"response bypassed workspace: "<<mode<<'\n';
            protectedResponses&=rejected;
        }
        pressure.Resize(0);
        for(const std::string mode:{"candidate","representatives"}) {
            finalSampleWorkspace=0; observe=true;
            const auto response=mode=="candidate"?manager.Candidate(started.scanId,candidateId):manager.RepresentativeFrames(started.scanId,candidateId);
            observe=false;
            const bool charged=finalSampleWorkspace>=resident+65536;
            if(!charged) std::cerr<<"response has no live JSON workspace at final process check: "<<mode<<'\n';
            protectedResponses&=charged;
            if(response!=(mode=="candidate"?expectedCandidate:expectedFrames))
                throw std::runtime_error("Response workspace must not alter full candidate or representative evidence");
        }
        if(!protectedResponses) throw std::runtime_error("Manager responses must reserve current JSON and hold it through the final process check");
        if(manager.Summary(started.scanId)!=expectedSummary || manager.Quality(started.scanId)!=expectedQuality ||
            !manager.Status(started.scanId).completed || workspace->Snapshot().currentBytes!=resident)
            throw std::runtime_error("Response pressure must preserve completed caches and release only transient reservations");
    }
    {
        testStage="manager cumulative disk quota";
        const auto stateRoot=root/"cumulative-disk",cacheRoot=cache/"cumulative-disk";
        const auto existing=cacheRoot/AnalysisScanCacheFormatId/"older-generation.keep";
        std::filesystem::create_directories(existing.parent_path());
        constexpr uint64_t quota=32*1024*1024,oldBytes=quota-16384;
        { std::ofstream out(existing,std::ios::binary); out.put('a'); out.seekp(oldBytes-1); out.put('z'); }
        auto native=std::make_shared<SourceDegradedTrace>(); std::atomic<unsigned> executions=0;
        AnalysisScanExecutor executor=[&](const auto& request,std::stop_token token,const auto& progress,auto& products,auto& error) {
            ++executions; return ExecuteDefaultAnalysisScan(request,token,progress,products,error);
        };
        auto profile=Profile(); profile["limits"]["cache_max_bytes"]=quota;
        const auto diskProfile=ValidateAndNormalizeAnalysisProfile(profile); assert(diskProfile.valid);
        AnalysisScanStartRequest request;
        request.traceSessionId="cumulative-disk"; request.tracePath=tracePath; request.source=native;
        request.normalizedProfile=diskProfile.normalized; request.profileIdentity=diskProfile.profileSha256;
        AnalysisScanSnapshot started;
        {
            AnalysisScanManager manager(stateRoot,cacheRoot,std::string(64,'e'),{},executor);
            started=manager.Start(request);
            const auto paused=WaitFor(manager,started.scanId,ScanState::CancelledResumable,6s);
            if(paused.error!="analysis_scan_cache_disk_budget" || paused.completed || !paused.resumable)
                throw std::runtime_error("Existing cache generations must count against the same disk quota as new work");
        } // Join the paused worker and its writer-lease cleanup before walking files.
        uint64_t bytes=0;
        for(const auto& scope:{stateRoot/"scans",cacheRoot/AnalysisScanCacheFormatId})
            for(const auto& file:std::filesystem::recursive_directory_iterator(scope))
                if(file.is_regular_file()) bytes+=file.file_size();
        if(bytes>quota || std::filesystem::file_size(existing)!=oldBytes)
            throw std::runtime_error("Disk quota must reject growth before writing and must preserve older generation files");
        { std::ifstream in(existing,std::ios::binary); char first=0,last=0; in.get(first); in.seekg(oldBytes-1); in.get(last);
          if(first!='a' || last!='z') throw std::runtime_error("Older generation bytes changed under disk pressure"); }
        // This is test-owned pressure data, never a production eviction policy.
        std::filesystem::remove(existing);
        AnalysisScanManager manager(stateRoot,cacheRoot,std::string(64,'e'),
            [native](const auto&,std::stop_token){return native;},executor);
        manager.Resume(started.scanId); WaitFor(manager,started.scanId,ScanState::Complete,6s);
        if(manager.Signatures(started.scanId,1000,"",{},json::object()).at("items").empty())
            throw std::runtime_error("A quota-paused scan must resume after external space is released");
    }
    {
        testStage="manager minimum free disk";
        for(const bool failCache : {false,true})
        {
            auto native=std::make_shared<SourceDegradedTrace>();
            std::atomic<uint64_t> available=128; std::atomic<unsigned> executions=0;
            AnalysisScanExecutor executor=[&](const auto& request,std::stop_token token,const auto& progress,auto& products,auto& error) {
                ++executions; return ExecuteDefaultAnalysisScan(request,token,progress,products,error);
            };
            auto profile=Profile(); profile["limits"]["minimum_free_disk_bytes"]=1024;
            const auto diskProfile=ValidateAndNormalizeAnalysisProfile(profile); assert(diskProfile.valid);
            const auto suffix=failCache ? "cache" : "scans";
            auto guarded=DiskGuardedManager<AnalysisScanManager>([&](const std::filesystem::path& path) {
                const bool isCache=path.generic_string().find("disk-space-cache-")!=std::string::npos;
                return isCache==failCache ? available.load() : uint64_t(4096);
            },root/(std::string("disk-space-scans-")+suffix),cache/(std::string("disk-space-cache-")+suffix),
                std::string(64,'e'),AnalysisScanSourceResolver{},executor);
            auto& manager=*guarded; AnalysisScanStartRequest request;
            request.traceSessionId="disk-space"; request.tracePath=tracePath; request.source=native;
            request.normalizedProfile=diskProfile.normalized; request.profileIdentity=diskProfile.profileSha256;
            const auto started=manager.Start(request);
            const auto paused=WaitFor(manager,started.scanId,ScanState::CancelledResumable,6s);
            if(paused.error!="analysis_scan_minimum_free_disk" || executions!=0 || paused.completed || !paused.resumable)
                throw std::runtime_error("Insufficient free space on either output volume must pause before source execution");
            available=4096; manager.Resume(started.scanId);
            WaitFor(manager,started.scanId,ScanState::Complete,6s);
            if(executions!=1) throw std::runtime_error("Restored disk space must allow exactly one resumed source scan");
            const auto summary=manager.Summary(started.scanId);
            available=128;
            if(manager.Summary(started.scanId)!=summary || !manager.Status(started.scanId).completed)
                throw std::runtime_error("Read-only access to a completed cache must survive low disk space");
        }
    }
    {
        testStage="manager lower profile hard limit";
        auto native=std::make_shared<SourceDegradedTrace>();
        std::atomic<uint64_t> processBytes=2048; std::atomic<unsigned> executions=0;
        AnalysisProcessMemoryOptions options; options.maximumBytes=4096;
        options.sample=[&]{return AnalysisProcessMemorySample{true,128,processBytes.load()};};
        AnalysisScanExecutor executor=[&](const auto& request,std::stop_token token,const auto& progress,auto& products,auto& error) {
            ++executions; return ExecuteDefaultAnalysisScan(request,token,progress,products,error);
        };
        auto profile=Profile(); profile["limits"]["query_memory_target_bytes"]=512;
        profile["limits"]["query_memory_hard_bytes"]=1024;
        const auto lower=ValidateAndNormalizeAnalysisProfile(profile); assert(lower.valid);
        auto guarded=ProcessGuardedManager<AnalysisScanManager>(options,root/"lower-profile-memory",cache/"lower-profile-memory",
            std::string(64,'e'),AnalysisScanSourceResolver{},executor);
        auto& manager=*guarded; AnalysisScanStartRequest request;
        request.traceSessionId="lower-profile-memory"; request.tracePath=tracePath; request.source=native;
        request.normalizedProfile=lower.normalized; request.profileIdentity=lower.profileSha256;
        const auto started=manager.Start(request);
        const auto paused=WaitFor(manager,started.scanId,ScanState::CancelledResumable,6s);
        if(paused.error.find("process_memory_budget")==std::string::npos || paused.processMemory.maximumBytes!=1024 || executions!=0)
            throw std::runtime_error("A lower profile hard limit must pause before invoking the scan executor");
        processBytes=128; manager.Resume(started.scanId);
        const auto completed=WaitFor(manager,started.scanId,ScanState::Complete,6s);
        if(completed.processMemory.maximumBytes!=1024 || executions!=1)
            throw std::runtime_error("Resume must keep the lower configured hard limit");
        const auto expected=manager.Summary(started.scanId);
        processBytes=2048;
        RejectCache([&]{manager.Summary(started.scanId);},"process_memory_budget");
        auto reopened=ProcessGuardedManager<AnalysisScanManager>(options,root/"lower-profile-memory",cache/"lower-profile-memory",
            std::string(64,'e'),AnalysisScanSourceResolver{},executor);
        RejectCache([&]{reopened->Quality(started.scanId);},"process_memory_budget");
        processBytes=128;
        if(reopened->Summary(started.scanId)!=expected || executions!=1)
            throw std::runtime_error("Cold reads recover under the lower profile limit without re-executing the source scan");
    }
    {
        testStage="manager process memory before scan";
        auto native=std::make_shared<SourceDegradedTrace>();
        std::atomic<uint64_t> processBytes=1025;
        AnalysisProcessMemoryOptions options; options.maximumBytes=1024;
        options.sample=[&]{return AnalysisProcessMemorySample{true,128,processBytes.load()};};
        std::atomic<unsigned> executions=0;
        AnalysisScanExecutor executor=[&](const auto& request,std::stop_token token,const auto& progress,auto& products,auto& error) {
            ++executions; return ExecuteDefaultAnalysisScan(request,token,progress,products,error);
        };
        auto guarded=ProcessGuardedManager<AnalysisScanManager>(options,root/"process-memory",cache/"process-memory",
            std::string(64,'e'),AnalysisScanSourceResolver{},executor);
        auto& manager=*guarded;
        AnalysisScanStartRequest request;
        request.traceSessionId="process-memory"; request.tracePath=tracePath; request.source=native;
        request.normalizedProfile=validation.normalized; request.profileIdentity=validation.profileSha256;
        const auto started=manager.Start(request);
        const auto paused=WaitFor(manager,started.scanId,ScanState::CancelledResumable,3s);
        if(paused.completed || !paused.resumable || executions!=0 || paused.error.find("process_memory_budget")==std::string::npos)
            throw std::runtime_error("Whole-process exhaustion must pause before invoking the default producer");
        {
            std::ifstream file(root/"process-memory"/"scans"/started.scanId/"scan-state.json");
            const auto state=json::parse(file);
            if(!state.contains("process_memory") || state.at("process_memory").value("maximum_bytes",std::string())!="1024" ||
                state.at("process_memory").value("peak_private_bytes",std::string())!="1025")
                throw std::runtime_error("Process-memory pause must persist the sampled peak and enforced limit");
        }
        const NeutralAggregateIdentity identity{native->GetTraceInfo().fingerprint,
            std::string(64,'e'),"1.35.0",NeutralScanAlgorithmId,NeutralAggregateSchemaVersion};
        if(std::filesystem::exists(cache/"process-memory"/AnalysisScanCacheFormatId/ComputeNeutralAggregateIdentity(identity)/"current.json"))
            throw std::runtime_error("Process memory denial must not publish a completed neutral pointer");
        processBytes=128; manager.Resume(started.scanId);
        WaitFor(manager,started.scanId,ScanState::Complete,6s);
        if(executions!=1 || manager.Candidates(started.scanId,1,"",{},json::object()).at("items").empty())
            throw std::runtime_error("Process-memory pause must recover with the actual default producer after pressure drops");
        const auto candidateId=manager.Candidates(started.scanId,1,"",{},json::object()).at("items")[0].at("candidate_id").get<std::string>();
        processBytes=1025;
        RejectCache([&]{manager.Summary(started.scanId);},"process_memory_budget");
        RejectCache([&]{manager.Signatures(started.scanId,1,"",{},json::object());},"process_memory_budget");
        RejectCache([&]{manager.Candidates(started.scanId,1,"",{},json::object());},"process_memory_budget");
        RejectCache([&]{manager.Candidate(started.scanId,candidateId);},"process_memory_budget");
        RejectCache([&]{manager.RepresentativeFrames(started.scanId,candidateId);},"process_memory_budget");
        RejectCache([&]{manager.Quality(started.scanId);},"process_memory_budget");
        // Denied reads do not corrupt a completed generation or latch forever.
        processBytes=128;
        if(!manager.Status(started.scanId).completed || manager.Candidate(started.scanId,candidateId).at("candidate_id")!=candidateId)
            throw std::runtime_error("Completed readers must remain usable when process pressure is relieved");
        auto reopened=ProcessGuardedManager<AnalysisScanManager>(options,root/"process-memory",cache/"process-memory",
            std::string(64,'e'),AnalysisScanSourceResolver{},executor);
        const auto restored=reopened->Status(started.scanId);
        if(!restored.completed || restored.processMemory.maximumBytes!=1024 || restored.processMemory.peakPrivateBytes!=128)
            throw std::runtime_error("A reopened scan must retain this execution's measured memory evidence");
        processBytes=1025;
        RejectCache([&]{reopened->Candidates(started.scanId,1,"",{},json::object());},"process_memory_budget");
        processBytes=128;
        if(reopened->Candidate(started.scanId,candidateId).at("candidate_id")!=candidateId)
            throw std::runtime_error("Cold cache readers must recover after process pressure drops");
    }
    {
        testStage="manager process memory in long stage";
        auto native=std::make_shared<SourceDegradedTrace>();
        std::atomic<uint64_t> processBytes=128;
        std::atomic<bool> cleanup=false;
        AnalysisProcessMemoryOptions options; options.maximumBytes=1024; options.interval=5ms;
        options.sample=[&]{return AnalysisProcessMemorySample{true,128,processBytes.load()};};
        AnalysisScanExecutor executor=[&](const auto&,std::stop_token token,const auto& progress,auto&,auto& error) {
            progress(ScanState::Scanning,0,1,"long_stage_without_progress");
            while(!token.stop_requested()) std::this_thread::sleep_for(1ms);
            cleanup=true; error="cancelled"; return false;
        };
        auto manager=ProcessGuardedManager<AnalysisScanManager>(options,root/"process-memory-running",cache/"process-memory-running",
            std::string(64,'e'),AnalysisScanSourceResolver{},executor);
        AnalysisScanStartRequest request;
        request.traceSessionId="process-memory-running"; request.tracePath=tracePath; request.source=native;
        request.normalizedProfile=validation.normalized; request.profileIdentity=validation.profileSha256;
        const auto started=manager->Start(request);
        WaitFor(*manager,started.scanId,ScanState::Scanning);
        processBytes=2048;
        const auto paused=WaitFor(*manager,started.scanId,ScanState::CancelledResumable,3s);
        if(!cleanup || paused.error.find("process_memory_budget")==std::string::npos || paused.processMemory.peakPrivateBytes<2048)
            throw std::runtime_error("A long stage must stop and retain the memory reason even without another progress callback");
    }
    {
        testStage="default scanners process memory cancellation";
        for( const auto* domain : { "frame", "gfx_entity", "memory" } )
        {
            auto native=std::make_shared<SourceDegradedTrace>();
            std::atomic<uint64_t> processBytes=128;
            std::atomic<size_t> readsAfterCancel=0;
            std::atomic<bool> reached=false;
            std::stop_token executionToken;
            AnalysisProcessMemoryOptions options; options.maximumBytes=1024; options.interval=5ms;
            options.sample=[&]{return AnalysisProcessMemorySample{true,128,processBytes.load()};};
            native->readHook=[&](std::string_view current) {
                if(executionToken.stop_requested()) { ++readsAfterCancel; return; }
                if(current!=domain) return;
                reached=true; processBytes=2048;
                const auto deadline=std::chrono::steady_clock::now()+2s;
                while(!executionToken.stop_requested() && std::chrono::steady_clock::now()<deadline)
                    std::this_thread::sleep_for(1ms);
                if(!executionToken.stop_requested()) throw std::runtime_error("Process monitor did not deliver cancellation");
            };
            AnalysisScanExecutor executor=[&](const auto& request,std::stop_token token,const auto& progress,auto& products,auto& error) {
                executionToken=token;
                return ExecuteDefaultAnalysisScan(request,token,progress,products,error);
            };
            const auto fixture=std::string("process-default-")+domain;
            auto manager=ProcessGuardedManager<AnalysisScanManager>(options,root/fixture,cache/fixture,
                std::string(64,'e'),AnalysisScanSourceResolver{},executor);
            AnalysisScanStartRequest request;
            request.traceSessionId=fixture; request.tracePath=tracePath; request.source=native;
            request.normalizedProfile=validation.normalized; request.profileIdentity=validation.profileSha256;
            const auto started=manager->Start(request);
            const auto paused=WaitFor(*manager,started.scanId,ScanState::CancelledResumable,6s);
            if(!reached || readsAfterCancel!=0 || paused.completed || paused.error.find("process_memory_budget")==std::string::npos)
                throw std::runtime_error(fixture+" must stop native reads immediately after process pressure: later_reads="+std::to_string(readsAfterCancel));
        }
    }
    {
        testStage="manager shutdown";
        std::atomic<bool> cleanupFinished=false;
        {
            auto source=std::make_shared<tracy::query::test::FakeTraceSource>();
            AnalysisScanManager manager(root/"shutdown",cache/"shutdown",std::string(64,'e'),{},
                [&](const AnalysisScanExecutionRequest&,std::stop_token token,const AnalysisScanProgressCallback& progress,
                    AnalysisScanProducts&,std::string& error) {
                    progress(ScanState::Scanning,0,1,"shutdown_fixture");
                    while(!token.stop_requested()) std::this_thread::sleep_for(1ms);
                    // Model non-trivial stream/file cleanup after cancellation.
                    std::this_thread::sleep_for(30ms); cleanupFinished=true; error="cancelled"; return false;
                });
            AnalysisScanStartRequest request;
            request.traceSessionId="shutdown"; request.tracePath=tracePath; request.source=source;
            request.normalizedProfile=validation.normalized; request.profileIdentity=validation.profileSha256;
            const auto started=manager.Start(request);
            WaitFor(manager,started.scanId,ScanState::Scanning);
        }
        if(!cleanupFinished) throw std::runtime_error("Manager destruction must join cancelled workers before returning");
    }
#ifndef TRACY_DEFAULT_SCAN_TEST_ONLY
    testStage="injected manager";
    auto source = std::make_shared<tracy::query::test::FakeTraceSource>();
    std::atomic<uint32_t> executions = 0;
    std::atomic<bool> release = false;
    AnalysisScanExecutor executor = [&]( const AnalysisScanExecutionRequest& request,
        std::stop_token stopToken, const AnalysisScanProgressCallback& progress,
        AnalysisScanProducts& products, std::string& error ) {
        ++executions;
        progress( ScanState::Scanning, 1, 4, "fixture_scan" );
        while( !release.load() && !stopToken.stop_requested() ) std::this_thread::sleep_for( 2ms );
        if( stopToken.stop_requested() ) { error = "cancelled"; return false; }
        progress( ScanState::Aggregating, 3, 4, "fixture_aggregate" );
        products = Products( request.temporaryRoot );
        return true;
    };
    AnalysisScanSourceResolver resolver = [source]( const std::filesystem::path&,
        std::stop_token ) { return source; };

    std::string scanId;
    {
        AnalysisScanManager manager( root, cache, std::string( 64, 'e' ), resolver, executor );
        AnalysisScanStartRequest request;
        request.traceSessionId = "trace-session-1";
        request.tracePath = tracePath;
        request.source = source;
        request.normalizedProfile = validation.normalized;
        request.profileIdentity = validation.profileSha256;

        const auto startedAt = std::chrono::steady_clock::now();
        const auto started = manager.Start( request );
        assert( std::chrono::steady_clock::now() - startedAt < 2s );
        assert( started.state == ScanState::Queued || started.state == ScanState::Validating ||
            started.state == ScanState::Scanning );
        scanId = started.scanId;
        assert( scanId.starts_with( "scan-" ) );

        const auto duplicate = manager.Start( request );
        assert( duplicate.scanId == scanId );
        std::this_thread::sleep_for( 20ms );
        assert( executions == 1 );

        const auto cancelAt = std::chrono::steady_clock::now();
        manager.Cancel( scanId );
        const auto cancelled = WaitFor( manager, scanId, ScanState::CancelledResumable );
        assert( std::chrono::steady_clock::now() - cancelAt < 2s );
        assert( cancelled.resumable && !cancelled.completed );

        release = true;
        const auto resumed = manager.Resume( scanId );
        assert( resumed.scanId == scanId );
        const auto complete = WaitFor( manager, scanId, ScanState::Complete );
        assert( complete.completed && complete.progressCompleted == complete.progressTotal );
        assert( executions == 2 );

        const auto summary = manager.Summary( scanId );
        assert( summary.at( "quality" ).at( "complete" ) == true );
        assert( summary.at( "policy_algorithm" ) == "candidate-policy-v3" );
        assert( summary.at( "capture_quality" ).is_array() );
        const auto signatures = manager.Signatures( scanId, 1, "", {}, json::object() );
        assert( signatures.at( "items" ).size() == 1 && signatures.at( "page" ).at( "done" ) == true );
        const auto candidates = manager.Candidates( scanId, 1, "", {}, json::object() );
        assert( candidates.at( "items" ).size() == 1 );
        const auto candidateId = candidates.at( "items" ).at( 0 ).at( "candidate_id" ).get<std::string>();
        const auto candidate = manager.Candidate( scanId, candidateId );
        assert( candidate.at( "candidate_id" ) == candidateId );
        const auto representative = manager.RepresentativeFrames( scanId, candidateId );
        assert( !representative.at( "frames" ).empty() );
        const auto quality = manager.Quality( scanId );
        assert( quality.at( "complete" ) == true );
        assert( quality.at( "capture_quality" ) == summary.at( "capture_quality" ) );
        assert( manager.Close( scanId ).closed );
    }

    // A new Query process discovers completed state from disk and can query it.
    {
        testStage="manager restart";
        AnalysisScanManager restarted( root, cache, std::string( 64, 'e' ), resolver, executor );
        const auto status = restarted.Status( scanId );
        assert( status.state == ScanState::Complete && status.completed );
        assert( !restarted.Candidates( scanId, 100, "", {}, json::object() ).at( "items" ).empty() );

        // Changing only policy creates a new scan but reuses the completed neutral aggregate.
        const auto changedValidation = ValidateAndNormalizeAnalysisProfile( Profile( 3 ) );
        assert( changedValidation.valid );
        AnalysisScanStartRequest changed;
        changed.traceSessionId = "trace-session-2"; changed.tracePath = tracePath; changed.source = source;
        changed.normalizedProfile = changedValidation.normalized;
        changed.profileIdentity = changedValidation.profileSha256;
        const auto before = executions.load();
        const auto changedStart = restarted.Start( changed );
        const auto changedComplete = WaitFor( restarted, changedStart.scanId, ScanState::Complete );
        assert( changedComplete.completed && executions == before );
    }

#ifndef TRACY_SCAN_MANAGER_TEST_ONLY
    // Query dispatch remains responsive while a persistent scan runs; the scan
    // does not occupy QueryService's legacy global cache mutex.
    {
        testStage="QueryService dispatch";
        const auto analysisRoot = root / "query-analysis";
        const auto analysisCache = analysisRoot / "cache";
        std::filesystem::create_directories( analysisCache );
        std::atomic<bool> queryRelease = false;
        AnalysisScanExecutor queryExecutor = [&]( const AnalysisScanExecutionRequest& request,
            std::stop_token token, const AnalysisScanProgressCallback& progress,
            AnalysisScanProducts& output, std::string& error ) {
            progress( ScanState::Scanning, 1, 2, "query_fixture" );
            while( !queryRelease.load() && !token.stop_requested() ) std::this_thread::sleep_for( 2ms );
            if( token.stop_requested() ) { error = "cancelled"; return false; }
            output = Products( request.temporaryRoot );
            return true;
        };
        SessionManager sessions( { root }, 2,
            []( const std::filesystem::path&, SessionManager::StateCallback callback ) {
                callback( tracy::analysis::TraceSourceState::Ready );
                return std::make_unique<tracy::query::test::FakeTraceSource>();
            } );
        QueryService service( sessions, DefaultAnalysisCacheBytes, analysisRoot,
            analysisCache, queryExecutor, std::string( 64, 'd' ) );
        const auto opened = sessions.Open( tracePath );
        assert( sessions.WaitReady( opened.id, 2s ).state == tracy::analysis::TraceSourceState::Ready );
        const auto started = service.Execute( {
            { "protocol", QueryProtocol }, { "id", "scan-start" },
            { "method", "analysis.scan.start" },
            { "params", { { "trace_id", opened.id }, { "profile", Profile() } } }
        } );
        assert( started.at( "ok" ) == true );
        const auto queryScanId = started.at( "data" ).at( "scan_id" ).get<std::string>();
        const auto ordinaryAt = std::chrono::steady_clock::now();
        const auto ordinary = service.Execute( {
            { "protocol", QueryProtocol }, { "id", "ordinary" },
            { "method", "trace.info" }, { "params", { { "trace_id", opened.id } } }
        } );
        assert( ordinary.at( "ok" ) == true );
        assert( std::chrono::steady_clock::now() - ordinaryAt < 2s );
        const auto status = service.Execute( {
            { "protocol", QueryProtocol }, { "id", "scan-status" },
            { "method", "analysis.scan.status" }, { "params", { { "scan_id", queryScanId } } }
        } );
        assert( status.at( "ok" ) == true && status.at( "data" ).at( "completed" ) == false );
        queryRelease = true;
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        json completed;
        do
        {
            completed = service.Execute( {
                { "protocol", QueryProtocol }, { "id", "scan-complete" },
                { "method", "analysis.scan.status" }, { "params", { { "scan_id", queryScanId } } }
            } );
            if( completed.at( "data" ).at( "completed" ) == true ) break;
            std::this_thread::sleep_for( 5ms );
        }
        while( std::chrono::steady_clock::now() < deadline );
        assert( completed.at( "data" ).at( "completed" ) == true );
    }

#endif
#endif
    testStage="fixture cleanup";
    std::error_code ignored;
    std::filesystem::remove_all( root, ignored );
    return 0;
}

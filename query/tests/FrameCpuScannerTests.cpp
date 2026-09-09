#include "TracyFrameCpuScanner.hpp"
#include "FakeTraceSource.hpp"
#include "TracyAnalysisWorkspaceBudget.hpp"

#include <cassert>
#include <chrono>
#include <limits>
#include <map>
#include <iostream>

using namespace tracy::analysis;

namespace
{

class CpuTreeSource final : public tracy::query::test::FakeTraceSource,
    public NativeBoundedTraceSource
{
public:
    CpuTreeSource()
    {
        frames = {
            { MakeEntityRef( "frame", 0 ), MakeEntityRef( "frame-set", 0 ), 0, 0, 100, {}, true },
            { MakeEntityRef( "frame", 1 ), MakeEntityRef( "frame-set", 0 ), 1, 100, 200, {}, true },
            { MakeEntityRef( "frame", 2 ), MakeEntityRef( "frame-set", 0 ), 2, 200, {}, {}, false }
        };

        Add( "root0", "", "Root", "Main", 0, 70 );
        Add( "child0", "root0", "Child", "Main", 10, 40 );
        Add( "child1", "root0", "Child", "Main", 30, 60 );
        Add( "deep0", "", "Deep0", "Main", 70, 99 );
        for( int depth = 1; depth <= 10; ++depth )
        {
            const auto ref = "deep" + std::to_string( depth );
            const auto parent = "deep" + std::to_string( depth - 1 );
            Add( ref, parent, "Deep" + std::to_string( depth ), "Main",
                70 + depth, 99 - depth );
        }

        Add( "root1", "", "Root", "Main", 100, 200 );
        Add( "child2", "root1", "Child", "Main", 110, 120 );
        Add( "parentA", "root1", "ParentA", "Main", 120, 150 );
        Add( "workA", "parentA", "Work", "Main", 125, 130 );
        Add( "parentB", "root1", "ParentB", "Main", 150, 180 );
        Add( "workB", "parentB", "Work", "Main", 155, 160 );
        Add( "wait", "root1", "WaitForJobGroupID", "Main", 180, 190 );
        Add( "pacing", "root1", "WaitForTargetFPS", "Main", 190, 200 );

        Add( "render", "", "RenderWork", "Render Thread", 10, 20 );
        // Stable-site identity also contains the normalized thread role.  Use
        // the same SourceLocation token as the Main-thread Work site to catch
        // cache implementations that accidentally merge roles.
        zones.back().sourceLocationRef = "source:Work";
        Add( "worker", "", "WorkerWork", "Job.Worker 0", 20, 30 );
        Add( "missingEnd", "", "MissingEnd", "Render Thread", 40, std::nullopt );
        Add( "negative", "", "Negative", "Render Thread", 60, 50 );
        Add( "clockInversion", "", "ClockInversion", "Render Thread", 65, 66 );
        zones.back().timingValid = false;
        zones.back().timingInvalidReason = "clock_inversion";
        Add( "orphan", "not-present", "Orphan", "Render Thread", 67, 68 );
        Add( "unassigned", "", "OutsideFrames", "Render Thread", 300, 310 );
    }

    uint32_t NativeBoundedScanVersion() const override { return NativeBoundedScanSchemaVersion; }
    TraceReadView AcquireReadView() const override
    {
        return { TraceSourceKind::Snapshot, TraceSourceState::Ready, 9, 310, true };
    }
    std::vector<FrameSetDto> GetFrameSets() const override
    {
        return { { MakeEntityRef( "frame-set", 0 ), 0, "Player.Frame", true, 3, 2 } };
    }
    std::vector<ThreadDto> GetThreads() const override
    {
        std::vector<ThreadDto> result( 3 );
        result[0].ref = MakeEntityRef( "thread", 1 );
        result[0].nativeId = 1;
        result[0].name = "Main";
        result[1].ref = MakeEntityRef( "thread", 2 );
        result[1].nativeId = 2;
        result[1].name = "Render Thread";
        result[2].ref = MakeEntityRef( "thread", 3 );
        result[2].nativeId = 3;
        result[2].name = "Job.Worker 0";
        return result;
    }
    std::vector<FrameDto> ScanFrames( const ScanRange& range ) const override
    {
        ReadBoundary( "frame" );
        return Page( frames, range );
    }
    std::vector<CpuZoneDto> ScanCpuZones( const ScanRange& range ) const override
    {
        ReadBoundary( "cpu" );
        std::vector<CpuZoneDto> matching;
        for( const auto& zone : zones )
        {
            const auto end = zone.endNs.value_or( zone.startNs );
            if( end < range.startNs || zone.startNs > range.endNs ) continue;
            matching.emplace_back( zone );
        }
        return Page( matching, range );
    }

    std::string cancelAt;
    std::function<void( std::string_view )> readHook;
    mutable bool cancellationRequested = false;
    mutable size_t readsAfterCancel = 0;
    void ReadBoundary( std::string_view domain ) const
    {
        if( readHook ) readHook( domain );
        if( cancellationRequested ) ++readsAfterCancel;
        if( domain == cancelAt ) cancellationRequested = true;
    }
    std::vector<FrameDto> frames;
    std::vector<CpuZoneDto> zones;

private:
    template<typename T>
    static std::vector<T> Page( const std::vector<T>& values, const ScanRange& range )
    {
        const auto begin = std::min( range.offset, values.size() );
        const auto end = begin + std::min( range.limit, values.size() - begin );
        return { values.begin() + begin, values.begin() + end };
    }

    void Add( std::string ref, std::string parent, std::string name,
        std::string thread, int64_t begin, std::optional<int64_t> end )
    {
        CpuZoneDto zone;
        zone.ref = ref;
        zone.threadRef = thread == "Main" ? MakeEntityRef( "thread", 1 ) :
            thread == "Render Thread" ? MakeEntityRef( "thread", 2 ) : MakeEntityRef( "thread", 3 );
        zone.sourceLocationRef = "source:" + name;
        zone.name = std::move( name );
        zone.function = zone.name;
        zone.file = "Runtime/Test.cpp";
        // Occurrences of the same source site must keep the same identity.
        // Parent-path identity, not occurrence order, distinguishes Work below
        // ParentA from Work below ParentB.
        zone.line = 1;
        if( !parent.empty() ) zone.parentRef = parent;
        zone.startNs = begin;
        zone.endNs = end;
        zone.complete = end.has_value();
        zone.timingValid = !end || *end >= begin;
        if( !zone.timingValid ) zone.timingInvalidReason = "negative_interval";
        zones.emplace_back( std::move( zone ) );
    }
};

class LargeCpuFrameSource final : public tracy::query::test::FakeTraceSource,
    public NativeBoundedTraceSource
{
public:
    explicit LargeCpuFrameSource( size_t count )
    {
        const auto frameSet = MakeEntityRef( "frame-set", 0 );
        const auto thread = MakeEntityRef( "thread", 1 );
        frames.reserve( count );
        zones.reserve( count );
        for( size_t index = 0; index < count; ++index )
        {
            const auto begin = int64_t( index * 100 );
            frames.push_back( { MakeEntityRef( "frame", index ), frameSet,
                index, begin, begin + 100, {}, true } );
            CpuZoneDto zone;
            zone.ref = MakeEntityRef( "cpu-zone", index );
            zone.threadRef = thread;
            zone.sourceLocationRef = "source:Work";
            zone.name = "Work";
            zone.function = "Work";
            zone.file = "Runtime/Large.cpp";
            zone.line = 7;
            zone.startNs = begin + 10;
            zone.endNs = begin + 90;
            zones.emplace_back( std::move( zone ) );
        }
    }

    uint32_t NativeBoundedScanVersion() const override { return NativeBoundedScanSchemaVersion; }
    TraceReadView AcquireReadView() const override
    {
        return { TraceSourceKind::Snapshot, TraceSourceState::Ready, 9,
            int64_t( frames.size() * 100 ), true };
    }
    std::vector<FrameSetDto> GetFrameSets() const override
    {
        return { { MakeEntityRef( "frame-set", 0 ), 0, "Player.Frame", true,
            frames.size(), frames.size() } };
    }
    std::vector<ThreadDto> GetThreads() const override
    {
        ThreadDto thread;
        thread.ref = MakeEntityRef( "thread", 1 );
        thread.nativeId = 1;
        thread.name = "Main";
        return { thread };
    }
    std::vector<FrameDto> ScanFrames( const ScanRange& range ) const override
    {
        return Page( frames, range );
    }
    std::vector<CpuZoneDto> ScanCpuZones( const ScanRange& range ) const override
    {
        return Page( zones, range );
    }

private:
    template<typename T>
    static std::vector<T> Page( const std::vector<T>& values, const ScanRange& range )
    {
        const auto begin = std::min( range.offset, values.size() );
        const auto end = begin + std::min( range.limit, values.size() - begin );
        return { values.begin() + begin, values.begin() + end };
    }

    std::vector<FrameDto> frames;
    std::vector<CpuZoneDto> zones;
};

const CpuSignatureDefinition& Definition( const CpuFrameScanResult& result,
    std::string_view name, std::string_view parentName, bool logical )
{
    for( const auto& value : result.signatures )
    {
        if( value.name != name || value.logical != logical ) continue;
        if( parentName.empty() && value.parentSignatureId.empty() ) return value;
        for( const auto& parent : result.signatures )
            if( parent.signatureId == value.parentSignatureId && parent.name == parentName ) return value;
    }
    assert( false );
    return result.signatures.front();
}

const CpuSignatureFrameRun& Run( const CpuFrameScanResult& result,
    std::string_view signature, size_t frameIndex )
{
    for( const auto& value : result.runs )
        if( value.signatureId == signature && value.frameIndex == frameIndex ) return value;
    assert( false );
    return result.runs.front();
}

std::pair<int64_t, int64_t> BruteForceParentAndDirectChildUnion(
    const CpuTreeSource& source, std::string_view parentRef, int64_t begin, int64_t end )
{
    int64_t parentTicks = 0;
    int64_t childTicks = 0;
    for( auto tick = begin; tick < end; ++tick )
    {
        bool inParent = false;
        bool inDirectChild = false;
        for( const auto& zone : source.zones )
        {
            if( !zone.endNs ) continue;
            if( zone.ref == parentRef && zone.startNs <= tick && tick < *zone.endNs ) inParent = true;
            if( zone.parentRef && *zone.parentRef == parentRef && zone.startNs <= tick && tick < *zone.endNs )
                inDirectChild = true;
        }
        if( inParent ) ++parentTicks;
        if( inParent && inDirectChild ) ++childTicks;
    }
    return { parentTicks, childTicks };
}

}

template<typename Options>
void SetCpuCancellation( Options& options, std::function<bool()> cancelled )
{
    options.cancelled = std::move( cancelled );
}

int main()
{
    bool workspaceTestsPassed = true;
    for( const auto* domain : { "frame", "cpu" } )
    {
        CpuTreeSource source;
        // No definitions: their already-protected registry must not mask the
        // independent frame, invalid-zone and quality-finding allocations.
        source.zones.resize(1); source.zones.front().endNs.reset();
        auto budget=std::make_shared<AnalysisWorkspaceBudget>(2*1024*1024,1024*1024);
        AnalysisWorkspaceReservation pressure(budget);
        bool active=false; size_t laterReads=0;
        source.readHook=[&](std::string_view current) {
            if(active) ++laterReads;
            else if(current==domain) {active=true;pressure.Resize(2*1024*1024-budget->Snapshot().currentBytes-128);}
        };
        CpuFrameScanOptions options; options.workspace=budget;
        bool rejected=false;
        try { (void)ExactFrameCpuScanner(source,2).Scan({},options); }
        catch(const std::exception& e) { rejected=std::string(e.what()).find("workspace_budget")!=std::string::npos; }
        if(!rejected || laterReads!=0) {
            std::cerr<<"CPU resident state bypasses budget: "<<domain<<" rejected="<<rejected<<" later_reads="<<laterReads<<'\n';
            workspaceTestsPassed=false;
        }
        pressure.Resize(0); assert(budget->Snapshot().currentBytes==0);
    }
    {
        CpuTreeSource source; source.zones.resize(1);
        source.zones.front().ref=std::string(64*1024,'x');
        auto budget=std::make_shared<AnalysisWorkspaceBudget>(2*1024*1024,1024*1024);
        AnalysisWorkspaceReservation pressure(budget);
        CpuFrameScanOptions options; options.workspace=budget; options.includeLogicalSignatures=false;
        options.definitionSink=[&](const auto&) {
            pressure.Resize(2*1024*1024-budget->Snapshot().currentBytes-128); return true;
        };
        bool rejected=false; size_t emitted=0;
        try { (void)ExactFrameCpuScanner(source,2).ScanView([&](const auto&){++emitted;return true;},options); }
        catch(const std::exception& e) { rejected=std::string(e.what()).find("workspace_budget")!=std::string::npos; }
        if(!rejected || emitted!=0) {
            std::cerr<<"CPU active payload bypasses post-definition pressure: rejected="<<rejected<<" emitted="<<emitted<<'\n';
            workspaceTestsPassed=false;
        }
        pressure.Resize(0); assert(budget->Snapshot().currentBytes==0);
    }
    {
        CpuTreeSource source;
        auto budget=std::make_shared<AnalysisWorkspaceBudget>(4*1024*1024,2*1024*1024);
        CpuFrameScanOptions options; options.workspace=budget;
        {
            auto result=ExactFrameCpuScanner(source,2).Scan({},options);
            if(budget->Snapshot().currentBytes==0) {
                std::cerr<<"CPU returned definitions/runs/denominators release workspace too early\n";
                workspaceTestsPassed=false;
            }
            const auto bytes=budget->Snapshot().currentBytes;
            auto moved=std::move(result);
            assert(budget->Snapshot().currentBytes==bytes && !moved.denominators.empty());
        }
        assert(budget->Snapshot().currentBytes==0);
    }
    {
        CpuTreeSource source;
        const auto first=source.zones.front();
        source.zones.assign(10000,first);
        for(size_t i=0;i<source.zones.size();++i) source.zones[i].ref="repeated:"+std::to_string(i);
        auto budget=std::make_shared<AnalysisWorkspaceBudget>(2*1024*1024,1024*1024);
        CpuFrameScanOptions options; options.workspace=budget; options.includeLogicalSignatures=false;
        size_t definitions=0,runs=0; int64_t inclusive=0;
        options.definitionSink=[&](const auto& definition) {++definitions;assert(definition.path=="Root");return true;};
        {
            const auto result=ExactFrameCpuScanner(source,128).ScanView([&](const auto& run) {
                ++runs; inclusive+=run.inclusiveNs; assert(run.exact && run.directChildUnionNs==0); return true;
            },options);
            assert(definitions==1 && runs==10000 && inclusive==700000);
            assert(result.signatures.empty() && result.runs.empty() && result.denominators.size()==1);
            assert(result.denominators.front().completeFrameDenominator==2);
        }
        assert(budget->Snapshot().currentBytes==0 && budget->Snapshot().peakBytes<=2*1024*1024);
    }
    if(!workspaceTestsPassed) return 1;
    for( const auto* domain : { "pre_cancelled", "frame", "cpu" } )
    {
        CpuTreeSource source;
        source.cancelAt=domain;
        source.cancellationRequested=std::string_view(domain)=="pre_cancelled";
        CpuFrameScanOptions options;
        SetCpuCancellation(options,[&]{return source.cancellationRequested;});
        size_t emittedAfterCancel=0;
        const auto emission=[&]{if(source.cancellationRequested) ++emittedAfterCancel;};
        options.completeFrameSink=[&](const auto&,auto,auto,auto){emission();};
        options.definitionSink=[&](const auto&){emission();return true;};
        bool rejected=false;
        try { (void)ExactFrameCpuScanner(source,2).Scan([&](const auto&){emission();return true;},options); }
        catch(const std::exception& e){rejected=std::string(e.what()).find("cancelled")!=std::string::npos;}
        if(!rejected || source.readsAfterCancel!=0 || emittedAfterCancel!=0)
        {
            std::cerr<<"CPU cancellation bypass: "<<domain<<" later_reads="<<source.readsAfterCancel<<" emitted="<<emittedAfterCancel<<'\n';
            return 1;
        }
    }
    {
        CpuTreeSource source; source.zones.resize(2);
        bool cancelled=false; size_t emitted=0;
        CpuFrameScanOptions options;
        SetCpuCancellation(options,[&]{return cancelled;});
        bool rejected=false;
        try { (void)ExactFrameCpuScanner(source,100).Scan([&](const auto&){++emitted;cancelled=true;return true;},options); }
        catch(const std::exception& e){rejected=std::string(e.what()).find("cancelled")!=std::string::npos;}
        if(!rejected || emitted!=1) {std::cerr<<"CPU final drain cancellation bypass: emitted="<<emitted<<'\n';return 1;}
    }
    CpuTreeSource source;
    ExactFrameCpuScanner scanner( source, 2 );
    const auto result = scanner.Scan();

    assert( result.inputZoneCount == source.zones.size() );
    assert( result.completeFrameCount == 2 );
    assert( result.incompleteFrameCount == 1 );
    assert( result.frameSetDenominators.size() == 1 );
    assert( result.frameSetDenominators.front().name == "Player.Frame" );
    assert( result.frameSetDenominators.front().continuous );
    assert( result.frameSetDenominators.front().completeFrameCount == 2 );
    assert( result.maximumDepth >= 10 );

    const auto& root = Definition( result, "Root", "", false );
    const auto& root0 = Run( result, root.signatureId, 0 );
    const auto bruteForce = BruteForceParentAndDirectChildUnion( source, "root0", 0, 100 );
    assert( root0.inclusiveNs == bruteForce.first );
    assert( root0.directChildUnionNs == bruteForce.second );
    assert( root0.exclusiveNs == bruteForce.first - bruteForce.second );
    assert( root0.exact );

    const auto& workA = Definition( result, "Work", "ParentA", false );
    const auto& workB = Definition( result, "Work", "ParentB", false );
    assert( workA.signatureId != workB.signatureId );
    const auto& logicalWorkA = Definition( result, "Work", "ParentA", true );
    const auto& logicalWorkB = Definition( result, "Work", "ParentB", true );
    assert( logicalWorkA.signatureId != logicalWorkB.signatureId );

    // Candidate aggregation may intentionally collapse physical parent paths
    // to one stable callsite rollup. The default scanner above retains the
    // full logical tree for drill-down and compatibility.
    CpuFrameScanOptions stableSites;
    stableSites.includeExactSignatures = false;
    stableSites.includeLogicalSignatures = true;
    stableSites.logicalSignatureMode = CpuLogicalSignatureMode::StableSite;
    const auto stableSiteResult = scanner.Scan( {}, stableSites );
    size_t stableWorkDefinitions = 0;
    for( const auto& definition : stableSiteResult.signatures )
    {
        if( definition.name != "Work" ) continue;
        ++stableWorkDefinitions;
        assert( definition.logical );
        assert( definition.parentSignatureId.empty() );
    }
    assert( stableWorkDefinitions == 1 );
    const auto& stableMainWork = Definition( stableSiteResult, "Work", "", true );
    const auto& stableRenderWork = Definition( stableSiteResult, "RenderWork", "", true );
    assert( stableMainWork.signatureId != stableRenderWork.signatureId );

    const auto& wait = Definition( result, "WaitForJobGroupID", "Root", false );
    const auto& pacing = Definition( result, "WaitForTargetFPS", "Root", false );
    assert( wait.workClass == CpuWorkClass::Wait );
    assert( pacing.workClass == CpuWorkClass::IntentionalPacing );

    bool rootDenominator = false;
    for( const auto& value : result.denominators )
    {
        if( value.signatureId != root.signatureId ) continue;
        assert( value.completeFrameDenominator == 2 );
        assert( value.whenPresentDenominator == 2 );
        rootDenominator = true;
    }
    assert( rootDenominator );

    assert( result.invalidZoneCount == 3 );
    assert( result.unassignedZoneCount == 1 );
    assert( !result.qualityComplete );
    assert( !result.qualityFindings.empty() );
    bool missingParentReported = false;
    for( const auto& finding : result.qualityFindings )
        if( finding.code == "missing_parent_zone" && finding.count == 1 ) missingParentReported = true;
    assert( missingParentReported );
    assert( result.maximumBatchObserved <= 2 );

    // A bounded streaming scan must not retain per-frame runs and must use an
    // interval lookup instead of comparing every zone with every frame.
    LargeCpuFrameSource large( 4096 );
    uint64_t streamed = 0;
    const auto largeResult = ExactFrameCpuScanner( large, 64 ).Scan(
        [&]( const CpuSignatureFrameRun& ) { ++streamed; return true; } );
    // A continuous FrameSet needs the following FrameMark to prove the current
    // frame's end.  The capture-tail frame may carry a synthetic Worker/Session
    // end timestamp, but it must not enter exact AI denominators.
    assert( streamed == 8190 ); // exact + logical signature for 4095 proven frames
    assert( largeResult.completeFrameCount == 4095 );
    assert( largeResult.incompleteFrameCount == 1 );
    assert( std::any_of( largeResult.qualityFindings.begin(),
        largeResult.qualityFindings.end(), []( const auto& finding ) {
            return finding.code == "continuous_frame_tail_without_next_mark" &&
                finding.count == 1;
        } ) );
    assert( largeResult.runs.empty() );
    // AC0: event IDs and timestamps change for all 4096 occurrences, but
    // the single physical path and single logical site remain two definitions.
    // Cache partitioning must never "fix" cardinality by merging other paths.
    assert( largeResult.signatures.size() == 2 );
    assert( largeResult.frameBoundaryProbeCount < 4096 * 16 );

    // The policy scan consumes the stable logical rollup only.  Physical
    // thread/path occurrences remain available from TraceSource for targeted
    // evidence queries and must not explode the aggregate signature set.
    CpuFrameScanOptions logicalOnly;
    logicalOnly.includeExactSignatures = false;
    logicalOnly.includeLogicalSignatures = true;
    uint64_t logicalRuns = 0;
    const auto logicalResult = ExactFrameCpuScanner( large, 64 ).Scan(
        [&]( const CpuSignatureFrameRun& ) { ++logicalRuns; return true; }, logicalOnly );
    assert( logicalRuns == 4095 );
    assert( logicalResult.signatures.size() == 1 );
    assert( logicalResult.signatures.front().logical );

    // The policy path uses a non-owning view and numeric ordinals so millions
    // of occurrences do not copy or re-register the same strings.
    uint64_t viewRuns = 0;
    uint32_t stableSignatureOrdinal = std::numeric_limits<uint32_t>::max();
    const auto viewResult = ExactFrameCpuScanner( large, 64 ).ScanView(
        [&]( const CpuSignatureFrameRunView& run ) {
            assert( !run.signatureId.empty() && !run.frameSetRef.empty() );
            assert( run.frameSetOrdinal == 0 );
            if( stableSignatureOrdinal == std::numeric_limits<uint32_t>::max() )
                stableSignatureOrdinal = run.signatureOrdinal;
            assert( run.signatureOrdinal == stableSignatureOrdinal );
            ++viewRuns;
            return true;
        }, logicalOnly );
    assert( viewRuns == 4095 );
    assert( viewResult.runs.empty() && viewResult.signatures.size() == 1 );

    // Removing the definition callback, retaining every full path, changing
    // ancestry, or emitting a definition twice must fail this real scan.
    CpuFrameScanOptions streamedOptions;
    std::map<std::string,CpuSignatureDefinition> definitions;
    streamedOptions.definitionSink=[&](const CpuSignatureDefinition& definition) {
        assert(definitions.emplace(definition.signatureId,definition).second);
        return true;
    };
    uint64_t definitionStreamRuns=0;
    const auto definitionStream=scanner.ScanView([&](const CpuSignatureFrameRunView& run) {
        assert(definitions.contains(std::string(run.signatureId)));
        ++definitionStreamRuns; return true;
    },streamedOptions);
    assert(definitionStreamRuns>0 && definitionStream.signatures.empty() && definitionStream.runs.empty());
    assert(definitions.size()==result.signatures.size());
    for(const auto& expected:result.signatures)
    {
        const auto& actual=definitions.at(expected.signatureId);
        assert(std::tie(actual.parentSignatureId,actual.name,actual.sourceLocationRef,actual.function,actual.file,
            actual.line,actual.threadRef,actual.threadRole,actual.path,actual.depth,actual.workClass,actual.logical)==
            std::tie(expected.parentSignatureId,expected.name,expected.sourceLocationRef,expected.function,expected.file,
            expected.line,expected.threadRef,expected.threadRole,expected.path,expected.depth,expected.workClass,expected.logical));
    }
    assert(definitions.at(workA.signatureId).path=="Root > ParentA > Work");
    assert(definitions.at(workB.signatureId).path=="Root > ParentB > Work");
    bool stopped=false;
    streamedOptions.definitionSink=[](const CpuSignatureDefinition&) { return false; };
    try { scanner.ScanView([](const CpuSignatureFrameRunView&) { return true; },streamedOptions); }
    catch(const std::exception& e) { stopped=std::string(e.what())=="frame_cpu_scan_definition_sink_cancelled"; }
    assert(stopped);
    {
        auto budget=std::make_shared<AnalysisWorkspaceBudget>(1024*1024,512*1024);
        AnalysisWorkspaceReservation pressure(budget,1024*1024-256);
        CpuFrameScanOptions options;
        options.workspace=budget;
        uint64_t emitted=0;
        options.definitionSink=[&](const auto&) { ++emitted; return true; };
        bool rejected=false;
        try { scanner.ScanView([](const CpuSignatureFrameRunView&) { return true; },options); }
        catch(const std::exception& e) { rejected=std::string(e.what()).find("workspace_budget")!=std::string::npos; }
        if(!rejected || emitted!=0) {
            std::cerr<<"CPU dictionary and registry bypass shared budget before definition emission: rejected="<<rejected<<" emitted="<<emitted<<'\n';
            return 1;
        }
        assert(budget->Snapshot().currentBytes==1024*1024-256);
    }
    return 0;
}

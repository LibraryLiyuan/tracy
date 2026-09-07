#include "TracyFrameCpuScanner.hpp"
#include "FakeTraceSource.hpp"

#include <cassert>
#include <chrono>
#include <limits>
#include <map>

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
        return Page( frames, range );
    }
    std::vector<CpuZoneDto> ScanCpuZones( const ScanRange& range ) const override
    {
        std::vector<CpuZoneDto> matching;
        for( const auto& zone : zones )
        {
            const auto end = zone.endNs.value_or( zone.startNs );
            if( end < range.startNs || zone.startNs > range.endNs ) continue;
            matching.emplace_back( zone );
        }
        return Page( matching, range );
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

int main()
{
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
    return 0;
}

#include "TracyGpuJobManagedScanner.hpp"
#include "FakeTraceSource.hpp"
#include "TracyQueue.hpp"
#include "TracyAnalysisWorkspaceBudget.hpp"

#include <cassert>
#include <iostream>
#include <map>

using namespace tracy;
using namespace tracy::analysis;

namespace
{

template<typename T>
std::vector<T> SliceValues( const std::vector<T>& values, size_t offset, size_t limit )
{
    const auto begin = std::min( offset, values.size() );
    const auto end = begin + std::min( limit, values.size() - begin );
    return { values.begin() + begin, values.begin() + end };
}

class GpuJobManagedSource final : public tracy::query::test::FakeTraceSource,
    public NativeBoundedTraceSource
{
public:
    GpuJobManagedSource()
    {
        frames = { { MakeEntityRef( "frame", 0 ), MakeEntityRef( "frame-set", 0 ), 0, 0, 100, {}, true } };

        AddGpu( 1, 0, 10, "GPU.L0.DirectA", 0, 30 );
        AddGpu( 10, 0, 12, "Pass", 5, 25, 1 );
        AddGpu( 2, 0, 11, "GPU.L0.DirectB", 30, 60 );
        AddGpu( 12, 0, 13, "Pass", 35, 55, 2 );
        AddGpu( 3, 1, 20, "GPU.L0.Compute", 0, 40 );
        AddGpu( 11, 1, 21, "Pass", 10, 30, 3 );
        AddGpu( 4, 2, 30, "GPU.L0.Copy", 60, 80 );
        AddGpu( 5, 2, 31, "GPU.L0.CopyOutside", 200, 220 );

        JobDto waited;
        waited.ref = MakeEntityRef( "job", 1 );
        waited.jobId = 1;
        waited.packedHandle = ( uint64_t( 7 ) << 32 ) | 42;
        waited.name = "WaitedJob";
        waited.scheduleNs = 1;
        waited.scheduleThreadRef = MakeEntityRef( "thread", 1 );
        waited.readyNs = 2;
        waited.queueEnterNs = 3;
        waited.firstRunNs = 4;
        waited.completedNs = 12;
        waited.executionNs = 8;
        waited.waitNs = 4;
        waited.waitActiveHelpNs = 1;
        waited.waitSpinYieldNs = 1;
        waited.waitSleepNs = 2;
        waited.waitEndCount = 1;
        waited.continuationCount = 1;
        waited.originFrameId = 9000;
        waited.scheduleCallsiteId = 77;
        waited.scheduleStackProvenance = "SiteReused";
        waited.dependencies.push_back( { 2, 0, 0 } );
        waited.stages = {
            { 4, MakeEntityRef( "thread", 3 ), 1, 0, 0, uint8_t( JnJobStage::WorkerSliceBegin ), 0 },
            { 12, MakeEntityRef( "thread", 3 ), 1, 0, 0, uint8_t( JnJobStage::WorkerSliceEnd ), 0 },
            { 13, MakeEntityRef( "thread", 1 ), 2, 0, 0, uint8_t( JnJobStage::WaitBegin ), 0 },
            { 17, MakeEntityRef( "thread", 1 ), 2, 0, 0, uint8_t( JnJobStage::WaitEnd ), 0 }
        };

        JobDto noWait;
        noWait.ref = MakeEntityRef( "job", 2 );
        noWait.jobId = 2;
        noWait.packedHandle = ( uint64_t( 1 ) << 32 ) | 9;
        noWait.name = "AsyncNoWaiter";
        noWait.scheduleNs = 0;
        noWait.readyNs = 1;
        noWait.firstRunNs = 2;
        noWait.completedNs = 3;
        noWait.executionNs = 1;

        JobDto reused = noWait;
        reused.ref = MakeEntityRef( "job", 3 );
        reused.jobId = 3;
        reused.packedHandle = ( uint64_t( 2 ) << 32 ) | 9;
        reused.name = "ReusedSlot";
        reused.scheduleNs = 10;
        reused.readyNs = 11;
        reused.firstRunNs = 12;
        reused.completedNs = 13;
        jobs = { waited, noWait, reused };

        scriptFrames = {
            { MakeEntityRef( "script-frame", 1 ), 1, "ManagedCaller", "Runtime/Foo.cs", 10, 1, MakeEntityRef( "thread", 1 ), 1, 0 },
            { MakeEntityRef( "script-frame", 2 ), 2, "LuaCaller", "Lua/foo.lua", 20, 1, MakeEntityRef( "thread", 1 ), 2, 0 }
        };
        scriptEvents = {
            { MakeEntityRef( "script-event", 1 ), 100, 0, 1, 1, MakeEntityRef( "thread", 1 ), 1, 0, uint8_t( JnScriptRecordKind::StackHeader ), {} },
            { MakeEntityRef( "script-event", 2 ), 100, 1, 0, 2, MakeEntityRef( "thread", 1 ), 1, 0, uint8_t( JnScriptRecordKind::StackFrame ), {} },
            { MakeEntityRef( "script-event", 3 ), 100, 0, 1, 3, MakeEntityRef( "thread", 1 ), 1, 0, uint8_t( JnScriptRecordKind::Marker ), "JN.Direct/Managed" },
            { MakeEntityRef( "script-event", 4 ), 1000, 100, 1, 4, MakeEntityRef( "thread", 1 ), 1, 0, uint8_t( JnScriptRecordKind::ZoneBegin ), {} },
            { MakeEntityRef( "script-event", 5 ), 1000, 0, 0, 8, MakeEntityRef( "thread", 1 ), 0, 0, uint8_t( JnScriptRecordKind::ZoneEnd ), {} },
            { MakeEntityRef( "script-event", 6 ), 200, 0, 2, 9, MakeEntityRef( "thread", 1 ), 2, 0, uint8_t( JnScriptRecordKind::StackHeader ), {} },
            { MakeEntityRef( "script-event", 7 ), 200, 2, 0, 10, MakeEntityRef( "thread", 1 ), 2, 0, uint8_t( JnScriptRecordKind::StackFrame ), {} },
            { MakeEntityRef( "script-event", 8 ), 2000, 200, 2, 11, MakeEntityRef( "thread", 1 ), 2, 0, uint8_t( JnScriptRecordKind::ZoneBegin ), {} },
            { MakeEntityRef( "script-event", 9 ), 2000, 0, 0, 15, MakeEntityRef( "thread", 1 ), 0, 0, uint8_t( JnScriptRecordKind::ZoneEnd ), {} },
            { MakeEntityRef( "script-event", 10 ), 3000, 999, 1, 16, MakeEntityRef( "thread", 1 ), 1, 0, uint8_t( JnScriptRecordKind::ZoneBegin ), {} }
        };
    }

    uint32_t NativeBoundedScanVersion() const override { return NativeBoundedScanSchemaVersion; }
    TraceReadView AcquireReadView() const override
    {
        return { TraceSourceKind::Snapshot, TraceSourceState::Ready, 44, 220, true };
    }
    std::vector<ThreadDto> GetThreads() const override
    {
        std::vector<ThreadDto> values( 3 );
        values[0].ref = MakeEntityRef( "thread", 1 ); values[0].name = "Main";
        values[1].ref = MakeEntityRef( "thread", 2 ); values[1].name = "Render Thread";
        values[2].ref = MakeEntityRef( "thread", 3 ); values[2].name = "Job.Worker 0";
        return values;
    }
    std::vector<GpuContextDto> GetGpuContexts() const override
    {
        std::vector<GpuContextDto> values( 3 );
        values[0].ref = MakeEntityRef( "gpu-context", 0 ); values[0].index = 0; values[0].name = "D3D12 Direct"; values[0].calibrated = true;
        values[1].ref = MakeEntityRef( "gpu-context", 1 ); values[1].index = 1; values[1].name = "D3D12 Compute"; values[1].calibrated = true;
        values[2].ref = MakeEntityRef( "gpu-context", 2 ); values[2].index = 2; values[2].name = "D3D12 Copy"; values[2].calibrated = true;
        return values;
    }
    std::vector<FrameDto> ScanFrames( const ScanRange& range ) const override
    {
        ReadBoundary( "frame" );
        return SliceValues( frames, range.offset, range.limit );
    }
    std::vector<GpuZoneDto> ScanGpuZones( const ScanRange& range ) const override
    {
        ReadBoundary( "gpu" );
        return SliceValues( gpuZones, range.offset, range.limit );
    }
    std::vector<JobDto> ScanJobs( size_t offset, size_t limit ) const override
    {
        ReadBoundary( "job" );
        return SliceValues( jobs, offset, limit );
    }
    std::vector<RelationDto> ScanRelations( size_t offset, size_t limit ) const override
    {
        ReadBoundary( "relation" );
        return SliceValues( relations, offset, limit );
    }
    std::vector<GfxEntityDto> ScanGfxEntities( size_t offset, size_t limit ) const override
    {
        ReadBoundary( "gfx_entity" );
        return SliceValues( gfxEntities, offset, limit );
    }
    std::vector<GfxLinkDto> ScanGfxLinks( size_t offset, size_t limit ) const override
    {
        ReadBoundary( "gfx_link" );
        return SliceValues( gfxLinks, offset, limit );
    }
    std::vector<ScriptFrameDto> ScanScriptFrames( size_t offset, size_t limit ) const override
    {
        ReadBoundary( "script_frame" );
        return SliceValues( scriptFrames, offset, limit );
    }
    std::vector<ScriptStackEventDto> ScanScriptStackEvents( size_t offset, size_t limit ) const override
    {
        ReadBoundary( "script_stack" );
        return SliceValues( scriptEvents, offset, limit );
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
    std::vector<GpuZoneDto> gpuZones;
    std::vector<JobDto> jobs;
    std::vector<ScriptFrameDto> scriptFrames;
    std::vector<ScriptStackEventDto> scriptEvents;
    std::vector<GfxEntityDto> gfxEntities {
        { MakeEntityRef( "gfx-entity", 100 ), 100, 0, 1, {}, 10, 0, uint8_t( JnGfxEntityKind::ExplicitGpuPass ), 0 },
        { MakeEntityRef( "gfx-entity", 101 ), 101, 0, 31, {}, 11, 0, uint8_t( JnGfxEntityKind::ExplicitGpuPass ), 0 },
        { MakeEntityRef( "gfx-entity", 201 ), 201, 0, 11, {}, 21, 1, uint8_t( JnGfxEntityKind::ExplicitGpuPass ), 0 }
    };
    std::vector<GfxLinkDto> gfxLinks {
        { MakeEntityRef( "gfx-link", 1 ), 100, 9000, 1, {}, uint8_t( JnGfxRelation::BelongsToFrame ), 0 },
        { MakeEntityRef( "gfx-link", 2 ), 101, 9000, 31, {}, uint8_t( JnGfxRelation::BelongsToFrame ), 0 },
        { MakeEntityRef( "gfx-link", 3 ), 201, 9000, 11, {}, uint8_t( JnGfxRelation::BelongsToFrame ), 0 }
    };
    std::vector<RelationDto> relations {
        { MakeEntityRef( "relation", 1 ), 100, 700, 4, {}, uint8_t( JnEntityKind::GpuPass ),
            uint8_t( JnEntityKind::GpuResource ), uint8_t( JnRelationNamespace::GpuReference ),
            uint8_t( JnRelationKind::UsesResource ), 0 },
        { MakeEntityRef( "relation", 2 ), 700, 800, 5, {}, uint8_t( JnEntityKind::GpuResource ),
            uint8_t( JnEntityKind::GpuAllocation ), uint8_t( JnRelationNamespace::Generic ),
            uint8_t( JnRelationKind::OwnedBy ), 0 },
        { MakeEntityRef( "relation", 3 ), 700, 801, 6, {}, uint8_t( JnEntityKind::GpuResource ),
            uint8_t( JnEntityKind::GpuAllocation ), uint8_t( JnRelationNamespace::Generic ),
            uint8_t( JnRelationKind::OwnedBy ), 0 }
    };

private:
    void AddGpu( uint64_t id, uint8_t context, uint16_t query, std::string name,
        int64_t begin, int64_t end, std::optional<uint64_t> parent = std::nullopt )
    {
        GpuZoneDto zone;
        zone.ref = MakeEntityRef( "gpu-zone", id );
        zone.contextRef = MakeEntityRef( "gpu-context", context );
        zone.threadRef = MakeEntityRef( "thread", 2 );
        zone.sourceLocationRef = "source:" + name;
        zone.name = std::move( name );
        zone.function = zone.name;
        zone.file = "Runtime/Gpu.cpp";
        zone.line = 1;
        if( parent ) zone.parentRef = MakeEntityRef( "gpu-zone", *parent );
        zone.gpuStartNs = begin;
        zone.gpuEndNs = end;
        zone.cpuStartNs = begin;
        zone.cpuEndNs = end;
        zone.queryId = query;
        zone.queryIdAvailability.available = true;
        gpuZones.emplace_back( std::move( zone ) );
    }
};

const GpuZoneScanFact& GpuFact( const GpuJobManagedScanResult& result, uint64_t id )
{
    const auto ref = "fake:gpu-zone:" + std::to_string( id );
    for( const auto& fact : result.gpuZones ) if( fact.zoneRef == ref ) return fact;
    assert( false );
    return result.gpuZones.front();
}

const JobLifecycleFact& JobFact( const GpuJobManagedScanResult& result, uint64_t id )
{
    for( const auto& fact : result.jobs ) if( fact.jobId == id ) return fact;
    assert( false );
    return result.jobs.front();
}

}

template<typename Options>
void SetCancellation( Options& options, std::function<bool()> cancelled )
{
    options.cancelled = std::move( cancelled );
}

template<typename Options>
void SetGpuDefinitionSink( Options& options, std::function<bool( const GpuSignatureDefinition& )> sink )
{
    options.definitionSink = std::move( sink );
}

template<typename Options>
void SetGpuWorkspace( Options& options, std::shared_ptr<AnalysisWorkspaceBudget> workspace )
{
    options.workspace = std::move( workspace );
}

int main()
{
    // Missing checks at any domain boundary must neither read another batch nor
    // emit a fact after the source has requested cancellation.
    bool cancellationTestsPassed = true;
    for( const auto* domain : { "pre_cancelled", "frame", "gfx_entity", "gfx_link",
        "relation", "gpu", "job", "script_frame", "script_stack" } )
    {
        GpuJobManagedSource cancelledSource;
        cancelledSource.cancelAt = domain;
        cancelledSource.cancellationRequested = std::string_view( domain ) == "pre_cancelled";
        GpuJobManagedScanOptions options;
        SetCancellation( options, [&] { return cancelledSource.cancellationRequested; } );
        size_t emittedAfterCancel = 0;
        const auto sink = [&]( const auto& ) {
            if( cancelledSource.cancellationRequested ) ++emittedAfterCancel;
            return true;
        };
        options.gpuZoneSink = sink; options.jobSink = sink;
        options.managedZoneSink = sink; options.relationSink = sink;
        bool rejected = false;
        try { (void)GpuJobManagedScanner( cancelledSource, 2 ).Scan( options ); }
        catch( const BoundedScanError& e ) { rejected = std::string( e.what() ).find( "cancelled" ) != std::string::npos; }
        if( !rejected || cancelledSource.readsAfterCancel != 0 || emittedAfterCancel != 0 )
        {
            std::cerr << "GPU/job cancellation bypass: " << domain << " rejected=" << rejected
                << " later_reads=" << cancelledSource.readsAfterCancel << " emitted=" << emittedAfterCancel << '\n';
            cancellationTestsPassed = false;
        }
    }
    if( !cancellationTestsPassed ) return 1;
    for( const bool managed : { false, true } )
    {
        GpuJobManagedSource drainSource;
        drainSource.gpuZones.resize( 2 ); // A root and child, both emitted during final stack drain.
        bool cancelled = false;
        size_t emitted = 0;
        GpuJobManagedScanOptions options;
        options.cancelled = [&] { return cancelled; };
        const auto sink = [&]( const auto& ) { ++emitted; cancelled = true; return true; };
        if( managed ) options.managedZoneSink = sink;
        else options.gpuZoneSink = sink;
        bool rejected = false;
        try { (void)GpuJobManagedScanner( drainSource, 100 ).Scan( options ); }
        catch( const BoundedScanError& e ) { rejected = std::string( e.what() ).find( "cancelled" ) != std::string::npos; }
        if( !rejected || emitted != 1 )
        {
            std::cerr << "Post-read cancellation bypass: managed=" << managed << " emitted=" << emitted << '\n';
            cancellationTestsPassed = false;
        }
    }
    if( !cancellationTestsPassed ) return 1;
    GpuJobManagedSource source;
    GpuJobManagedScanner scanner( source, 2 );
    const auto result = scanner.Scan();

    // A missing outer timestamp is a local quality gap, not permission to
    // erase the known parent identity or turn a valid child into an orphan.
    {
        GpuJobManagedSource missingRoot;
        missingRoot.gpuZones.resize(2);
        missingRoot.gpuZones[0].gpuEndNs.reset();
        missingRoot.gpuZones[0].complete=false;
        const auto partial=GpuJobManagedScanner(missingRoot,1).Scan();
        const auto& child=GpuFact(partial,10);
        if(partial.gpuZones.size()!=2 || partial.physicalL0SegmentCount!=1 ||
            child.depth!=1 || child.signatureId!=GpuFact(result,10).signatureId ||
            !child.physicalTimingExact || child.inclusiveNs!=20)
        {
            std::cerr<<"An incomplete GPU root lost a valid child or changed its stable path/L0 identity\n";
            return 1;
        }
    }

    // A streaming consumer receives each complete definition before any fact
    // refers to it, without a second retained definition collection.
    std::map<std::string, GpuSignatureDefinition> definitions;
    GpuJobManagedScanOptions streamedDefinitions;
    streamedDefinitions.retainDetails = false;
    SetGpuDefinitionSink( streamedDefinitions, [&]( const auto& definition ) {
        assert( definitions.emplace( definition.signatureId, definition ).second );
        return true;
    } );
    // Do not rely on assert inside the fact callback for the missing-API RED.
    streamedDefinitions.gpuZoneSink = [&]( const auto& fact ) {
        if( !definitions.contains( fact.signatureId ) || !definitions.contains( fact.logicalSignatureId ) )
            return false;
        return true;
    };
    bool streamedDefinitionsPassed = true;
    try
    {
        const auto output = scanner.Scan( streamedDefinitions );
        streamedDefinitionsPassed = output.gpuSignatures.empty() &&
            definitions.size() == result.gpuSignatures.size() && output.gpuZoneCount == 8;
    }
    catch( const BoundedScanError& ) { streamedDefinitionsPassed = false; }
    if( !streamedDefinitionsPassed ) std::cerr << "GPU definitions are not streamed before facts without retained copies\n";
    for( const auto& expected : result.gpuSignatures )
    {
        if( !streamedDefinitionsPassed ) break;
        const auto& actual = definitions.at( expected.signatureId );
        assert( std::tie( actual.signatureId, actual.parentSignatureId, actual.name, actual.sourceLocationRef,
            actual.function, actual.file, actual.line, actual.contextRef, actual.queueClass, actual.path,
            actual.depth, actual.logical ) == std::tie( expected.signatureId, expected.parentSignatureId,
            expected.name, expected.sourceLocationRef, expected.function, expected.file, expected.line,
            expected.contextRef, expected.queueClass, expected.path, expected.depth, expected.logical ) );
    }
    if( streamedDefinitionsPassed )
    {
        assert( definitions.at( GpuFact( result, 10 ).signatureId ).path == "GPU.L0.DirectA > Pass" );
        assert( definitions.at( GpuFact( result, 12 ).signatureId ).path == "GPU.L0.DirectB > Pass" );
    }
    auto workspace = std::make_shared<AnalysisWorkspaceBudget>( 1024 * 1024, 1024 * 1024 );
    AnalysisWorkspaceReservation pressure( workspace, 1024 * 1024 - 256 );
    GpuJobManagedScanOptions deniedOptions;
    deniedOptions.retainDetails = false;
    SetGpuWorkspace( deniedOptions, workspace );
    size_t definitionsAfterPressure = 0;
    SetGpuDefinitionSink( deniedOptions, [&]( const auto& ) { ++definitionsAfterPressure; return true; } );
    bool budgetRejected = false;
    try { (void)scanner.Scan( deniedOptions ); }
    catch( const std::exception& e ) { budgetRejected = std::string( e.what() ).find( "analysis_workspace_budget" ) != std::string::npos; }
    if( !budgetRejected || definitionsAfterPressure != 0 )
        std::cerr << "GPU definition registry bypassed shared workspace budget\n";
    if( !streamedDefinitionsPassed || !budgetRejected || definitionsAfterPressure != 0 ) return 1;
    pressure.Resize( 0 );
    assert( workspace->Snapshot().currentBytes == 0 );
    bool residentBudgetTestsPassed = true;
    for( const auto* domain : { "frame", "gfx_entity", "gfx_link", "relation", "job", "script_frame", "script_stack" } )
    {
        GpuJobManagedSource pressured;
        pressured.gpuZones.clear(); // No signature allocations may mask another domain's missing budget.
        auto shared = std::make_shared<AnalysisWorkspaceBudget>( 1024 * 1024, 1024 * 1024 );
        AnalysisWorkspaceReservation occupied( shared );
        bool pressureActive = false;
        size_t laterReads = 0;
        pressured.readHook = [&]( std::string_view current ) {
            if( pressureActive ) ++laterReads;
            else if( current == domain )
            {
                pressureActive = true;
                occupied.Resize( 1024 * 1024 - shared->Snapshot().currentBytes - 128 );
            }
        };
        GpuJobManagedScanOptions options;
        options.workspace = shared; options.retainDetails = false;
        bool rejected = false;
        try { (void)GpuJobManagedScanner( pressured, 2 ).Scan( options ); }
        catch( const std::exception& e ) { rejected = std::string( e.what() ).find( "analysis_workspace_budget" ) != std::string::npos; }
        if( !rejected || laterReads != 0 )
        {
            std::cerr << "GPU/job resident budget bypass: " << domain << " rejected=" << rejected << " later_reads=" << laterReads << '\n';
            residentBudgetTestsPassed = false;
        }
        occupied.Resize( 0 );
        assert( shared->Snapshot().currentBytes == 0 );
    }
    {
        auto shared = std::make_shared<AnalysisWorkspaceBudget>( 2 * 1024 * 1024, 1024 * 1024 );
        GpuJobManagedScanOptions options; options.workspace = shared;
        {
            auto retained = scanner.Scan( options );
            if( shared->Snapshot().currentBytes == 0 )
            {
                std::cerr << "GPU/job returned detail vectors have no surviving workspace ownership\n";
                residentBudgetTestsPassed = false;
            }
            const auto bytes = shared->Snapshot().currentBytes;
            auto moved = std::move( retained );
            assert( shared->Snapshot().currentBytes == bytes && moved.gpuZones.size() == 8 );
        }
        assert( shared->Snapshot().currentBytes == 0 );
    }
    if( !residentBudgetTestsPassed ) return 1;
    {
        GpuJobManagedSource activeSource;
        activeSource.gpuZones.resize( 1 );
        activeSource.gpuZones.front().ref = "gpu-instance:" + std::string( 64 * 1024, 'r' );
        activeSource.jobs.clear(); activeSource.scriptFrames.clear(); activeSource.scriptEvents.clear();
        auto shared = std::make_shared<AnalysisWorkspaceBudget>( 1024 * 1024, 1024 * 1024 );
        AnalysisWorkspaceReservation occupied( shared );
        GpuJobManagedScanOptions options;
        options.workspace = shared; options.retainDetails = false; options.includeLogicalGpuSignatures = false;
        options.definitionSink = [&]( const auto& ) {
            occupied.Resize( 1024 * 1024 - shared->Snapshot().currentBytes - 128 );
            return true;
        };
        size_t emitted = 0;
        options.gpuZoneSink = [&]( const auto& ) { ++emitted; return true; };
        bool rejected = false;
        try { (void)GpuJobManagedScanner( activeSource, 2 ).Scan( options ); }
        catch( const std::exception& e ) { rejected = std::string( e.what() ).find( "analysis_workspace_budget" ) != std::string::npos; }
        if( !rejected || emitted != 0 )
        {
            std::cerr << "Active GPU zone payload bypassed workspace: rejected=" << rejected << " emitted=" << emitted << '\n';
            return 1;
        }
        occupied.Resize( 0 );
        assert( shared->Snapshot().currentBytes == 0 );
    }
    {
        GpuJobManagedSource repeated;
        repeated.gpuZones.clear(); repeated.jobs.clear(); repeated.scriptFrames.clear(); repeated.scriptEvents.clear();
        for( size_t index = 0; index < 10000; ++index )
        {
            auto zone = source.gpuZones.front();
            zone.ref = repeated.MakeEntityRef( "gpu-zone", index );
            repeated.gpuZones.push_back( std::move( zone ) );
        }
        auto shared = std::make_shared<AnalysisWorkspaceBudget>( 1024 * 1024, 1024 * 1024 );
        GpuJobManagedScanOptions options;
        options.workspace = shared; options.retainDetails = false; options.includeLogicalGpuSignatures = false;
        size_t emittedDefinitions = 0;
        options.definitionSink = [&]( const auto& ) { ++emittedDefinitions; return true; };
        {
            const auto output = GpuJobManagedScanner( repeated, 2 ).Scan( options );
            assert( emittedDefinitions == 1 && output.gpuZoneCount == 10000 );
            assert( output.physicalL0SegmentCount == 10000 && output.gpuSignatures.empty() );
        }
        assert( shared->Snapshot().currentBytes == 0 && shared->Snapshot().peakBytes <= 1024 * 1024 );
    }
    {
        GpuJobManagedScanOptions cancelledDefinition;
        size_t emitted = 0, facts = 0;
        cancelledDefinition.definitionSink = [&]( const auto& ) { ++emitted; return false; };
        cancelledDefinition.gpuZoneSink = [&]( const auto& ) { ++facts; return true; };
        bool rejected = false;
        try { (void)scanner.Scan( cancelledDefinition ); }
        catch( const BoundedScanError& e ) { rejected = std::string( e.what() ).find( "cancelled" ) != std::string::npos; }
        assert( rejected && emitted == 1 && facts == 0 );
    }
    {
        GpuJobManagedSource wide;
        wide.gpuZones.clear();
        for( size_t index = 0; index < 128; ++index )
        {
            auto zone = source.gpuZones.front();
            zone.ref = wide.MakeEntityRef( "gpu-zone", index );
            zone.sourceLocationRef = "wide-source:" + std::to_string( index );
            zone.name = "Wide" + std::string( 64 * 1024, 'x' );
            zone.function = "WideFunction";
            wide.gpuZones.push_back( std::move( zone ) );
        }
        auto compact = std::make_shared<AnalysisWorkspaceBudget>( 2 * 1024 * 1024, 1024 * 1024 );
        GpuJobManagedScanOptions options;
        options.workspace = compact; options.retainDetails = false; options.includeLogicalGpuSignatures = false;
        std::set<std::string> seen;
        options.definitionSink = [&]( const auto& definition ) {
            assert( definition.name == "Wide" + std::string( 64 * 1024, 'x' ) );
            assert( definition.path == definition.name && definition.parentSignatureId.empty() );
            assert( seen.emplace( definition.signatureId ).second );
            return true;
        };
        {
            const auto output = GpuJobManagedScanner( wide, 2 ).Scan( options );
            assert( seen.size() == 128 && output.gpuSignatures.empty() && output.gpuZoneCount == 128 );
        }
        assert( compact->Snapshot().peakBytes <= 2 * 1024 * 1024 && compact->Snapshot().currentBytes == 0 );
    }

    assert( result.maximumBatchObserved <= 2 );
    assert( result.gpuZones.size() == source.gpuZones.size() );
    assert( result.gpuTracks.size() == source.gpuZones.size() * 2 );
    assert( result.physicalL0SegmentCount == 5 );

    const auto& direct = GpuFact( result, 1 );
    assert( direct.queueClass == GpuQueueClass::Direct );
    assert( direct.inclusiveNs == 30 );
    assert( direct.directChildUnionNs == 20 );
    assert( direct.exclusiveNs == 10 );
    assert( direct.frameEvidence == GpuFrameEvidence::ExactFrameRelation );
    assert( direct.frameId && *direct.frameId == 9000 );

    const auto& directB = GpuFact( result, 2 );
    assert( directB.frameEvidence == GpuFrameEvidence::ExactFrameRelation );
    assert( direct.signatureId != directB.signatureId );
    assert( direct.frameId == directB.frameId );
    assert( GpuFact( result, 10 ).logicalSignatureId != GpuFact( result, 12 ).logicalSignatureId );

    const auto& compute = GpuFact( result, 3 );
    assert( compute.queueClass == GpuQueueClass::Compute );
    assert( compute.frameEvidence == GpuFrameEvidence::InferredFromChildPasses );
    assert( compute.frameId && *compute.frameId == 9000 );
    assert( GpuFact( result, 4 ).frameEvidence == GpuFrameEvidence::TemporalCandidate );
    assert( GpuFact( result, 5 ).frameEvidence == GpuFrameEvidence::Unassigned );

    const auto& waited = JobFact( result, 1 );
    assert( waited.hasWaiter );
    assert( waited.hasContinuation );
    assert( waited.scheduleStackAvailable );
    assert( waited.dependencyJobIds.size() == 1 && waited.dependencyJobIds.front() == 2 );
    const auto& noWait = JobFact( result, 2 );
    assert( !noWait.hasWaiter );
    assert( !noWait.hasContinuation );
    assert( noWait.handleSlot == 9 && noWait.handleGeneration == 1 );
    assert( JobFact( result, 3 ).handleGeneration == 2 );
    assert( result.handleReuseCount == 1 );

    assert( result.managedStacks.size() == 2 );
    assert( result.managedStacks[0].complete );
    assert( result.managedStacks[0].frames.size() == 1 );
    assert( result.managedStacks[0].frames.front().function == "ManagedCaller" );
    assert( !result.managedStacks[1].complete );
    assert( result.managedZones.size() == 3 );
    assert( result.managedZones[0].stackAvailable );
    assert( !result.managedZones[1].stackAvailable );
    assert( result.managedZones[1].stackUnavailableReason == "script_stack_incomplete" );
    assert( !result.managedZones[2].stackAvailable );
    assert( result.managedZones[2].stackUnavailableReason == "script_stack_missing" );
    assert( !result.managedZones[2].endNs );

    assert( result.relations.size() == 3 );
    assert( result.ambiguousRelationCount == 2 );
    assert( result.relations[0].exact );
    assert( !result.relations[1].exact && !result.relations[2].exact );
    assert( !result.qualityComplete );

    uint64_t streamedGpu = 0;
    uint64_t streamedJobs = 0;
    GpuJobManagedScanOptions bounded;
    bounded.retainDetails = false;
    bounded.gpuZoneSink = [&]( const GpuZoneScanFact& ) { ++streamedGpu; return true; };
    bounded.jobSink = [&]( const JobLifecycleFact& ) { ++streamedJobs; return true; };
    const auto streamed = scanner.Scan( bounded );
    assert( streamed.gpuZoneCount == result.gpuZones.size() );
    assert( streamed.jobCount == result.jobs.size() );
    assert( streamed.managedZoneCount == result.managedZones.size() );
    assert( streamed.relationCount == result.relations.size() );
    assert( streamedGpu == streamed.gpuZoneCount );
    assert( streamedJobs == streamed.jobCount );
    assert( streamed.gpuZones.empty() && streamed.gpuTracks.empty() );
    assert( streamed.jobs.empty() && streamed.managedZones.empty() && streamed.relations.empty() );

    GpuJobManagedScanOptions stableSites;
    stableSites.includeExactGpuSignatures = false;
    stableSites.includeLogicalGpuSignatures = true;
    stableSites.logicalSignatureMode = GpuLogicalSignatureMode::StableSite;
    const auto stable = scanner.Scan( stableSites );
    assert( GpuFact( stable, 10 ).signatureId.empty() );
    assert( GpuFact( stable, 10 ).logicalSignatureId == GpuFact( stable, 12 ).logicalSignatureId );
    const auto stablePassDefinitions = std::count_if( stable.gpuSignatures.begin(),
        stable.gpuSignatures.end(), []( const auto& value ) { return value.name == "Pass"; } );
    assert( stablePassDefinitions == 2 ); // Direct and Compute queues remain distinct.
    return 0;
}

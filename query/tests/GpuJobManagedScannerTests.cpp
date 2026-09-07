#include "TracyGpuJobManagedScanner.hpp"
#include "FakeTraceSource.hpp"
#include "TracyQueue.hpp"

#include <cassert>

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
        return SliceValues( frames, range.offset, range.limit );
    }
    std::vector<GpuZoneDto> ScanGpuZones( const ScanRange& range ) const override
    {
        return SliceValues( gpuZones, range.offset, range.limit );
    }
    std::vector<JobDto> ScanJobs( size_t offset, size_t limit ) const override
    {
        return SliceValues( jobs, offset, limit );
    }
    std::vector<RelationDto> ScanRelations( size_t offset, size_t limit ) const override
    {
        return SliceValues( relations, offset, limit );
    }
    std::vector<GfxEntityDto> ScanGfxEntities( size_t offset, size_t limit ) const override
    {
        return SliceValues( gfxEntities, offset, limit );
    }
    std::vector<GfxLinkDto> ScanGfxLinks( size_t offset, size_t limit ) const override
    {
        return SliceValues( gfxLinks, offset, limit );
    }
    std::vector<ScriptFrameDto> ScanScriptFrames( size_t offset, size_t limit ) const override
    {
        return SliceValues( scriptFrames, offset, limit );
    }
    std::vector<ScriptStackEventDto> ScanScriptStackEvents( size_t offset, size_t limit ) const override
    {
        return SliceValues( scriptEvents, offset, limit );
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

int main()
{
    GpuJobManagedSource source;
    GpuJobManagedScanner scanner( source, 2 );
    const auto result = scanner.Scan();

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

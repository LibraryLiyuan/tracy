#include "TracyMemoryIoSamplingTelemetryScanner.hpp"
#include "FakeTraceSource.hpp"
#include "TracyQueue.hpp"
#include "TracyAnalysisWorkspaceBudget.hpp"

#include <algorithm>
#include <cassert>
#include <iostream>

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

enum class CatalogFixtureMode
{
    Complete,
    Invalid,
    Disabled,
    Absent
};

class MemoryIoSamplingSource final : public tracy::query::test::FakeTraceSource,
    public NativeBoundedTraceSource, public GpuCatalogBoundedScanSource
{
public:
    explicit MemoryIoSamplingSource( CatalogFixtureMode mode = CatalogFixtureMode::Complete,
        bool samplingAvailable = true, bool contextSwitchAvailable = true,
        TraceSourceKind sourceKind = TraceSourceKind::Snapshot )
        : m_mode( mode )
        , m_samplingAvailable( samplingAvailable )
        , m_contextSwitchAvailable( contextSwitchAvailable )
        , m_sourceKind( sourceKind )
    {
        pools = {
            { MakeEntityRef( "memory-pool", 0 ), 1, "CPU.Temp", 5, 1, 400, 0, 0, false },
            { MakeEntityRef( "memory-pool", 1 ), 2, "DXGI Local Usage", 0, 0, 0, 0, 0, true }
        };
        pools[1].persistedUsageBytes = 350;

        AddMemory( 1, "A", 100, 10, 20 );
        AddMemory( 2, "A", 200, 20, 50 );
        AddMemory( 3, "B", 50, 30, 31 );
        AddMemory( 4, "C", 400, 40, {} );
        AddMemory( 5, "C", 10, 45, 60 );
        AddMemory( 6, "D", 20, 0, {} );

        IoRequestDto complete;
        complete.ref = MakeEntityRef( "io", 1 ); complete.requestId = 1; complete.queueNs = 10;
        complete.startNs = 12; complete.endNs = 20; complete.requestedBytes = 100;
        complete.transferredBytes = 100; complete.status = uint8_t( JnIoStatus::Success );
        complete.terminalCount = 1;
        complete.stages = {
            { 12, MakeEntityRef( "thread", 3 ), 0, 0, uint8_t( JnIoStage::Start ), uint8_t( JnIoStatus::Unknown ), 0 },
            { 20, MakeEntityRef( "thread", 3 ), 100, 0, uint8_t( JnIoStage::Complete ), uint8_t( JnIoStatus::Success ), 0 }
        };
        auto cancelled = complete;
        cancelled.ref = MakeEntityRef( "io", 2 ); cancelled.requestId = 2;
        cancelled.status = uint8_t( JnIoStatus::Cancelled ); cancelled.transferredBytes = 10;
        cancelled.stages.back().stage = uint8_t( JnIoStage::Cancel );
        cancelled.stages.back().status = uint8_t( JnIoStatus::Cancelled );
        auto failed = complete;
        failed.ref = MakeEntityRef( "io", 3 ); failed.requestId = 3;
        failed.status = uint8_t( JnIoStatus::Failure ); failed.transferredBytes = 0;
        failed.stages.back().stage = uint8_t( JnIoStage::Error );
        failed.stages.back().status = uint8_t( JnIoStatus::Failure );
        auto orphan = complete;
        orphan.ref = MakeEntityRef( "io", 4 ); orphan.requestId = 4; orphan.orphan = true;
        orphan.endNs.reset(); orphan.terminalCount = 0; orphan.status = uint8_t( JnIoStatus::Unknown );
        orphan.stages.resize( 1 );
        io = { complete, cancelled, failed, orphan };

        samples = {
            { MakeEntityRef( "sample", 1 ), MakeEntityRef( "thread", 1 ), 11, 10, MakeEntityRef( "callstack", 10 ), "sample" },
            { MakeEntityRef( "sample", 2 ), MakeEntityRef( "thread", 1 ), 12, 10, MakeEntityRef( "callstack", 10 ), "sample" },
            { MakeEntityRef( "sample", 3 ), MakeEntityRef( "thread", 2 ), 13, 11, MakeEntityRef( "callstack", 11 ), "kernel" },
            { MakeEntityRef( "sample", 4 ), MakeEntityRef( "thread", 2 ), 14, 0, {}, "sample" }
        };

        ContextSwitchDto mainRun;
        mainRun.ref = MakeEntityRef( "context-switch", 1 ); mainRun.threadRef = MakeEntityRef( "thread", 1 );
        mainRun.startNs = 20; mainRun.endNs = 40; mainRun.wakeupNs = 15;
        mainRun.reasonName = "wr_mutex"; mainRun.stateName = "waiting";
        ContextSwitchDto renderRun;
        renderRun.ref = MakeEntityRef( "context-switch", 2 ); renderRun.threadRef = MakeEntityRef( "thread", 2 );
        renderRun.startNs = 45; renderRun.endNs = 55; renderRun.wakeupNs = 42;
        renderRun.reasonName = "preempted"; renderRun.stateName = "ready";
        contextSwitches = { mainRun, renderRun };

        GpuAllocationAnalysisRecord a;
        a.allocationId = 100; a.sizeBytes = 100; a.residentBytes = 80; a.resources = { 1, 2 };
        GpuAllocationAnalysisRecord b;
        b.allocationId = 200; b.sizeBytes = 200; b.residentBytes = 200; b.resources = { 3 };
        allocations = { a, b };

        GpuAnalysisResourceSummary r1;
        r1.resourceId = 1; r1.allocationId = 100; r1.capacityBytes = 50;
        GpuAnalysisResourceSummary r2 = r1;
        r2.resourceId = 2; r2.capacityBytes = 70; r2.hasAliasGroup = true;
        GpuAnalysisResourceSummary r3 = r1;
        r3.resourceId = 3; r3.allocationId = 200; r3.capacityBytes = 100;
        resources = { r1, r2, r3 };

        GpuPassWorkingSet pass;
        pass.passId = 1; pass.frameId = 99; pass.directPhysicalBytes = 100;
        pass.inclusivePhysicalBytes = 300; pass.directRangeBytes = 64; pass.complete = true;
        pass.directResources = { 1, 2 }; pass.inclusiveResources = { 1, 2, 3 };
        passes = { pass };

        GpuAnalysisRangeStoreEntry range;
        range.resourceId = 1; range.record.passInstanceId = 1;
        range.record.resourceId = 1; range.record.offsetBytes = 4; range.record.lengthBytes = 64;
        ranges = { range };
    }

    uint32_t NativeBoundedScanVersion() const override { return NativeBoundedScanSchemaVersion; }
    TraceReadView AcquireReadView() const override
    {
        return { m_sourceKind, TraceSourceState::Ready, 70, 100, true };
    }
    TraceInfoDto GetTraceInfo() const override
    {
        TraceInfoDto info;
        info.fingerprint = std::string( 64, 'a' ); info.firstTimeNs = 0; info.lastTimeNs = 100;
        info.counts.frameImages = 2;
        info.appInfo = {
            "JNQ1|{\"schema_version\":1,\"snapshot_sequence\":0,\"producer\":{\"id\":1,\"key\":\"gpu.reference\",\"source_mode\":\"test\",\"requested\":true,\"compiled\":true,\"supported\":true,\"enabled\":true,\"effective\":true,\"permission_denied\":false,\"deferred\":false,\"reason\":\"\",\"counters\":{\"observed\":\"0\",\"emitted\":\"0\",\"dropped\":\"0\",\"filtered\":\"0\",\"sampled_out\":\"0\",\"overflow\":\"0\",\"mismatch\":\"0\",\"unresolved\":\"0\",\"tail_truncated\":\"0\",\"cpu_time_ns\":\"0\",\"event_bytes\":\"0\"}}}",
            "JNQ1|{\"schema_version\":1,\"snapshot_sequence\":1,\"producer\":{\"id\":1,\"key\":\"gpu.reference\",\"source_mode\":\"test\",\"requested\":true,\"compiled\":true,\"supported\":true,\"enabled\":true,\"effective\":true,\"permission_denied\":false,\"deferred\":false,\"reason\":\"\",\"counters\":{\"observed\":\"10\",\"emitted\":\"8\",\"dropped\":\"1\",\"filtered\":\"1\",\"sampled_out\":\"0\",\"overflow\":\"1\",\"mismatch\":\"0\",\"unresolved\":\"0\",\"tail_truncated\":\"0\",\"cpu_time_ns\":\"1000\",\"event_bytes\":\"256\"}}}"
        };
        if( wideMetadataAt == "info" ) info.captureName = std::string( 3 * 1024 * 1024, 'x' );
        return info;
    }
    std::vector<Capability> GetCapabilities() const override
    {
        const bool catalogPresent = m_mode == CatalogFixtureMode::Complete || m_mode == CatalogFixtureMode::Invalid;
        const bool catalogQueryable = m_mode == CatalogFixtureMode::Complete;
        std::string catalogReason = "N27 GPU Catalog schema 1 is present";
        if( m_mode == CatalogFixtureMode::Invalid ) catalogReason = "GPU Catalog Core generation is invalid";
        else if( m_mode == CatalogFixtureMode::Disabled ) catalogReason = "GPU Catalog disabled by capture profile";
        else if( m_mode == CatalogFixtureMode::Absent ) catalogReason = "trace predates N27 or contains no GPU Catalog section";
        std::vector<Capability> values = {
            { "memory", true, true, true, "available", {} },
            { "memory.gpu", true, true, true, "physical allocation facts available", {} },
            { "gpu.catalog", catalogPresent, catalogQueryable, catalogQueryable, catalogReason, {} },
            { "sample", m_samplingAvailable, m_samplingAvailable, m_samplingAvailable,
                m_samplingAvailable ? "available" : "sampling permission unavailable", {} },
            { "context_switch", m_contextSwitchAvailable, m_contextSwitchAvailable, m_contextSwitchAvailable,
                m_contextSwitchAvailable ? "available" : "context switch permission unavailable", {} },
            { "io", true, true, true, "available", {} }
        };
        if( wideMetadataAt == "capability" ) values.front().methods.push_back( std::string( 3 * 1024 * 1024, 'x' ) );
        return values;
    }
    std::vector<ThreadDto> GetThreads() const override
    {
        std::vector<ThreadDto> values( 3 );
        values[0].ref = MakeEntityRef( "thread", 1 ); values[0].name = "Main";
        values[1].ref = MakeEntityRef( "thread", 2 ); values[1].name = "Render Thread";
        values[2].ref = MakeEntityRef( "thread", 3 ); values[2].name = "Loading";
        if( wideMetadataAt == "thread" ) values.front().externalThreadName = std::string( 3 * 1024 * 1024, 'x' );
        return values;
    }
    std::vector<MemoryPoolDto> GetMemoryPools() const override
    {
        auto values = pools;
        if( wideMetadataAt == "pool" ) values.front().storedName = std::string( 3 * 1024 * 1024, 'x' );
        return values;
    }
    std::vector<MemoryEventDto> ScanMemoryEvents( const ScanRange& range ) const override
    {
        ReadBoundary( "memory" );
        if( wideMemoryCount )
        {
            std::vector<MemoryEventDto> values;
            const auto first = std::min( range.offset, wideMemoryCount );
            const auto last = first + std::min( range.limit, wideMemoryCount - first );
            for( auto index = first; index < last; ++index )
            {
                auto value = memory.front();
                value.ref = "memory:" + std::to_string( index );
                value.address = "address:" + std::to_string( index );
                value.size = 1; value.allocationNs = 10 + int64_t( index ) * 2;
                value.freeNs = value.allocationNs + 1;
                value.allocationZoneRef = std::string( 64 * 1024, 'x' );
                values.push_back( std::move( value ) );
            }
            return values;
        }
        return SliceValues( memory, range.offset, range.limit );
    }
    std::vector<IoRequestDto> ScanIoRequests( size_t offset, size_t limit ) const override
    {
        ReadBoundary( "io" );
        if( wideIoCount )
        {
            std::vector<IoRequestDto> values;
            const auto first = std::min( offset, wideIoCount );
            const auto last = first + std::min( limit, wideIoCount - first );
            for( auto index = first; index < last; ++index )
            {
                auto value = io.front();
                value.requestId = index;
                value.ref = "io:" + std::to_string( index ) + std::string( 64 * 1024, 'x' );
                values.push_back( std::move( value ) );
            }
            return values;
        }
        return SliceValues( io, offset, limit );
    }
    std::vector<SampleDto> ScanSampleEvents( const ScanRange& range ) const override
    {
        ReadBoundary( "sample" );
        return m_samplingAvailable ? SliceValues( samples, range.offset, range.limit ) : std::vector<SampleDto> {};
    }
    std::vector<ContextSwitchDto> ScanContextSwitchEvents( const ScanRange& range ) const override
    {
        ReadBoundary( "context_switch" );
        return m_contextSwitchAvailable ? SliceValues( contextSwitches, range.offset, range.limit ) : std::vector<ContextSwitchDto> {};
    }
    std::vector<CallstackFrameDto> ResolveCallstacks( const std::vector<uint32_t>& ids, size_t maxDepth ) const override
    {
        ReadBoundary( "callstack" );
        std::vector<CallstackFrameDto> result;
        for( const auto id : ids )
        {
            if( maxDepth == 0 || id == 0 ) continue;
            result.push_back( { MakeEntityRef( "callstack-frame", id ), id == 10 ? "MainLeaf" : "RenderLeaf",
                "runtime.cpp", 1, "0x1", "0x1", false, id, 0, "UnityPlayer.dll" } );
        }
        return result;
    }

    uint64_t GetGpuCatalogResourceCountBounded() const override { return resources.size(); }
    uint64_t GetGpuCatalogAllocationCountBounded() const override { return allocations.size(); }
    uint64_t GetGpuCatalogPassCountBounded() const override { return passes.size(); }
    uint64_t GetGpuCatalogRangeCountBounded() const override { return ranges.size(); }
    std::vector<GpuAnalysisResourceSummary> ScanGpuCatalogResourcesBounded( size_t offset, size_t limit ) const override
    { ReadBoundary( "gpu_resource" ); return SliceValues( resources, offset, limit ); }
    std::vector<GpuAllocationAnalysisRecord> ScanGpuCatalogAllocationsBounded( size_t offset, size_t limit ) const override
    { ReadBoundary( "gpu_allocation" ); return SliceValues( allocations, offset, limit ); }
    std::vector<GpuPassWorkingSet> ScanGpuCatalogPassesBounded( size_t offset, size_t limit ) const override
    { ReadBoundary( "gpu_pass" ); return SliceValues( passes, offset, limit ); }
    std::vector<GpuAnalysisRangeStoreEntry> ScanGpuCatalogRangesBounded( size_t offset, size_t limit ) const override
    { ReadBoundary( "gpu_range" ); return SliceValues( ranges, offset, limit ); }

    std::string cancelAt;
    size_t wideIoCount = 0;
    size_t wideMemoryCount = 0;
    std::string wideMetadataAt;
    std::function<void( std::string_view )> readHook;
    mutable bool cancellationRequested = false;
    mutable size_t readsAfterCancel = 0;
    void ReadBoundary( std::string_view domain ) const
    {
        if( readHook ) readHook( domain );
        if( cancellationRequested ) ++readsAfterCancel;
        if( domain == cancelAt ) cancellationRequested = true;
    }

private:
    void AddMemory( uint64_t id, std::string address, uint64_t size, int64_t begin,
        std::optional<int64_t> end )
    {
        MemoryEventDto event;
        event.ref = MakeEntityRef( "memory-event", id ); event.poolRef = MakeEntityRef( "memory-pool", 0 );
        event.address = std::move( address ); event.size = size; event.allocationNs = begin;
        event.freeNs = end; event.complete = end.has_value(); event.allocationThreadRef = MakeEntityRef( "thread", 1 );
        memory.emplace_back( std::move( event ) );
    }

    CatalogFixtureMode m_mode;
    bool m_samplingAvailable;
    bool m_contextSwitchAvailable;
    TraceSourceKind m_sourceKind;
    std::vector<MemoryPoolDto> pools;
    std::vector<MemoryEventDto> memory;
    std::vector<IoRequestDto> io;
    std::vector<SampleDto> samples;
    std::vector<ContextSwitchDto> contextSwitches;
    std::vector<GpuAnalysisResourceSummary> resources;
public:
    std::vector<GpuAllocationAnalysisRecord> allocations;
private:
    std::vector<GpuPassWorkingSet> passes;
    std::vector<GpuAnalysisRangeStoreEntry> ranges;
};

class LeakCandidateSource final : public tracy::query::test::FakeTraceSource,
    public NativeBoundedTraceSource
{
public:
    uint32_t NativeBoundedScanVersion() const override { return NativeBoundedScanSchemaVersion; }
    TraceReadView AcquireReadView() const override
    { return { TraceSourceKind::Snapshot, TraceSourceState::Ready, 71, 100, true }; }
    TraceInfoDto GetTraceInfo() const override
    {
        TraceInfoDto info; info.firstTimeNs = 0; info.lastTimeNs = 100; return info;
    }
    std::vector<MemoryPoolDto> GetMemoryPools() const override
    { return { { MakeEntityRef( "memory-pool", 0 ), 1, "CPU.Persistent", 1, 1, 64, 0, 0, false } }; }
    std::vector<MemoryEventDto> ScanMemoryEvents( const ScanRange& range ) const override
    {
        MemoryEventDto event; event.ref = MakeEntityRef( "memory-event", 1 );
        event.poolRef = MakeEntityRef( "memory-pool", 0 ); event.address = "0x1";
        event.size = 64; event.allocationNs = 10; event.allocationThreadRef = MakeEntityRef( "thread", 1 );
        const std::vector<MemoryEventDto> values { event };
        return SliceValues( values, range.offset, range.limit );
    }
};

const CpuMemoryPoolFact& CpuPool( const MemoryIoSamplingTelemetryScanResult& result )
{
    assert( result.cpuMemoryPools.size() == 1 );
    return result.cpuMemoryPools.front();
}

}

template<typename Scanner>
auto ScanWithCancellation( Scanner& scanner, const std::function<bool()>& cancelled )
{
    return scanner.Scan( cancelled );
}

template<typename Scanner>
auto ScanCompact( Scanner& scanner, std::shared_ptr<AnalysisWorkspaceBudget> workspace )
{
    return scanner.ScanSummary( {}, std::move( workspace ) );
}

int main()
{
    bool cancellationTestsPassed = true;
    for( const auto* domain : { "pre_cancelled", "memory", "gpu_allocation", "gpu_resource",
        "gpu_pass", "gpu_range", "io", "sample", "callstack", "context_switch" } )
    {
        MemoryIoSamplingSource cancelledSource;
        cancelledSource.cancelAt = domain;
        cancelledSource.cancellationRequested = std::string_view( domain ) == "pre_cancelled";
        MemoryIoSamplingTelemetryScanner cancelledScanner( cancelledSource, 2, 2, 20 );
        bool rejected = false;
        try { (void)ScanWithCancellation( cancelledScanner, [&] { return cancelledSource.cancellationRequested; } ); }
        catch( const BoundedScanError& e ) { rejected = std::string( e.what() ).find( "cancelled" ) != std::string::npos; }
        if( !rejected || cancelledSource.readsAfterCancel != 0 )
        {
            std::cerr << "Memory/IO cancellation bypass: " << domain << " rejected=" << rejected
                << " later_reads=" << cancelledSource.readsAfterCancel << '\n';
            cancellationTestsPassed = false;
        }
    }
    if( !cancellationTestsPassed ) return 1;
    {
        MemoryIoSamplingSource lifetimes;
        lifetimes.allocations[0].createTime = 10;
        lifetimes.allocations[0].destroyTime = 20;
        lifetimes.allocations[1].createTime = 20;
        lifetimes.allocations[1].destroyTime = 30;
        auto placed = lifetimes.allocations[0];
        placed.allocationId = 300; placed.parentAllocationId = 100; placed.sizeBytes = 50;
        lifetimes.allocations.push_back( placed );
        const auto measured = MemoryIoSamplingTelemetryScanner( lifetimes ).Scan();
        if( measured.gpuMemory.physicalBytes != 200 )
        { std::cerr << "Physical peak must be simultaneous live root allocations, not history sum\n"; return 1; }
    }
    MemoryIoSamplingSource source;
    MemoryIoSamplingTelemetryScanner scanner( source, 2, 2, 20 );
    const auto result = scanner.Scan();

    bool compactTestsPassed = true;
    for( const auto* domain : { "info", "capability", "pool", "thread" } )
    {
        MemoryIoSamplingSource wide;
        wide.wideMetadataAt = domain;
        size_t traversalReads = 0;
        wide.readHook = [&]( std::string_view ) { ++traversalReads; };
        auto shared = std::make_shared<AnalysisWorkspaceBudget>( 2 * 1024 * 1024, 1024 * 1024 );
        MemoryIoSamplingTelemetryScanner wideScanner( wide, 2, 2, 20 );
        bool rejected = false;
        try { (void)ScanCompact( wideScanner, shared ); }
        catch( const std::exception& e ) { rejected = std::string( e.what() ).find( "analysis_workspace_budget" ) != std::string::npos; }
        if( !rejected || traversalReads != 0 )
        {
            std::cerr << "System metadata payload bypasses budget: " << domain << " rejected=" << rejected << " reads=" << traversalReads << '\n';
            compactTestsPassed = false;
        }
        assert( shared->Snapshot().currentBytes == 0 );
    }
    {
        auto shared = std::make_shared<AnalysisWorkspaceBudget>( 2 * 1024 * 1024, 1024 * 1024 );
        {
            auto compact = ScanCompact( scanner, shared );
            if( !compact.ioRequests.empty() || !compact.sampling.leaves.empty() || !compact.scheduling.roles.empty() )
            {
                std::cerr << "Summary scan retains unused IO/sample/scheduling detail vectors\n";
                compactTestsPassed = false;
            }
            if( shared->Snapshot().currentBytes == 0 )
            {
                std::cerr << "Summary scan returned facts have no shared workspace ownership\n";
                compactTestsPassed = false;
            }
            assert( compact.inputMemoryEventCount == 6 && compact.inputGpuAllocationCount == 2 );
            assert( compact.inputGpuResourceCount == 3 && compact.inputGpuPassCount == 1 && compact.inputGpuRangeCount == 1 );
            assert( compact.inputIoRequestCount == 4 && compact.inputSampleCount == 4 && compact.inputContextSwitchCount == 2 );
            assert( compact.inputTelemetryRecordCount == 2 && !compact.qualityComplete );
            assert( CpuPool( compact ).peakLiveBytes == 630 && CpuPool( compact ).endLiveBytes == 420 );
            assert( compact.gpuMemory.physicalBytes == 300 && compact.gpuMemory.logicalCapacityBytes == 220 );
            assert( compact.ioSummary.completeCount == 1 && compact.ioSummary.cancelledCount == 1 &&
                compact.ioSummary.errorCount == 1 && compact.ioSummary.orphanCount == 1 );
            assert( compact.ioSummary.requestedBytes == 400 && compact.ioSummary.transferredBytes == 210 );
            assert( compact.sampling.available && compact.sampling.totalSamples == 4 && compact.sampling.unresolvedSamples == 1 );
            assert( compact.scheduling.available && compact.telemetry.totalCpuTimeNs == 1000 );
            assert( compact.qualityFindings.size() == result.qualityFindings.size() );
            for( size_t i = 0; i < result.qualityFindings.size(); ++i )
            {
                const auto& a = compact.qualityFindings[i]; const auto& b = result.qualityFindings[i];
                assert( a.code == b.code && a.message == b.message && a.count == b.count && a.representativeRefs == b.representativeRefs );
            }
            const auto bytes = shared->Snapshot().currentBytes;
            auto moved = std::move( compact );
            assert( shared->Snapshot().currentBytes == bytes && moved.cpuMemoryPools.size() == 1 );
        }
        assert( shared->Snapshot().currentBytes == 0 );
    }
    for( const auto* domain : { "memory", "gpu_allocation", "gpu_resource", "gpu_pass", "gpu_range",
        "io", "sample", "callstack", "context_switch" } )
    {
        MemoryIoSamplingSource pressured;
        auto shared = std::make_shared<AnalysisWorkspaceBudget>( 2 * 1024 * 1024, 1024 * 1024 );
        AnalysisWorkspaceReservation occupied( shared );
        bool pressureActive = false;
        size_t laterReads = 0;
        pressured.readHook = [&]( std::string_view current ) {
            if( pressureActive ) ++laterReads;
            else if( current == domain )
            {
                pressureActive = true;
                occupied.Resize( 2 * 1024 * 1024 - shared->Snapshot().currentBytes - 128 );
            }
        };
        MemoryIoSamplingTelemetryScanner pressuredScanner( pressured, 2, 2, 20 );
        bool rejected = false;
        try { (void)ScanCompact( pressuredScanner, shared ); }
        catch( const std::exception& e ) { rejected = std::string( e.what() ).find( "analysis_workspace_budget" ) != std::string::npos; }
        if( !rejected || laterReads != 0 )
        {
            std::cerr << "System summary workspace bypass: " << domain << " rejected=" << rejected << " later_reads=" << laterReads << '\n';
            compactTestsPassed = false;
        }
        occupied.Resize( 0 );
        assert( shared->Snapshot().currentBytes == 0 );
    }
    {
        MemoryIoSamplingSource wide;
        wide.wideIoCount = 512; // 32 MiB of raw strings, generated in bounded pages.
        auto shared = std::make_shared<AnalysisWorkspaceBudget>( 2 * 1024 * 1024, 1024 * 1024 );
        MemoryIoSamplingTelemetryScanner wideScanner( wide, 2, 2, 20 );
        {
            const auto compact = ScanCompact( wideScanner, shared );
            if( !compact.ioRequests.empty() ) compactTestsPassed = false;
            assert( compact.inputIoRequestCount == 512 && compact.ioSummary.completeCount == 512 );
            assert( compact.ioSummary.requestedBytes == 51200 && compact.ioSummary.transferredBytes == 51200 );
        }
        assert( shared->Snapshot().currentBytes == 0 && shared->Snapshot().peakBytes <= 2 * 1024 * 1024 );
    }
    {
        MemoryIoSamplingSource wide;
        wide.wideMemoryCount = 512; // Irrelevant 32 MiB zone-ref payload must not be retained.
        auto shared = std::make_shared<AnalysisWorkspaceBudget>( 2 * 1024 * 1024, 1024 * 1024 );
        MemoryIoSamplingTelemetryScanner wideScanner( wide, 2, 2, 20 );
        try
        {
            const auto compact = ScanCompact( wideScanner, shared );
            const auto& pool = CpuPool( compact );
            assert( compact.inputMemoryEventCount == 512 && pool.eventCount == 512 );
            assert( pool.totalAllocatedBytes == 512 && pool.totalFreedBytes == 512 );
            assert( pool.peakLiveBytes == 1 && pool.endLiveBytes == 0 && pool.accountingGapCount == 0 );
        }
        catch( const std::exception& e )
        {
            std::cerr << "Memory summary retains irrelevant per-event DTO strings: " << e.what() << '\n';
            compactTestsPassed = false;
        }
        assert( shared->Snapshot().currentBytes == 0 );
    }
    if( !compactTestsPassed ) return 1;

    assert( result.maximumBatchObserved <= 2 );
    assert( result.inputMemoryEventCount == 6 );
    assert( result.inputGpuAllocationCount == 2 );
    assert( result.inputGpuResourceCount == 3 );
    assert( result.inputGpuPassCount == 1 );
    assert( result.inputGpuRangeCount == 1 );
    assert( result.inputIoRequestCount == 4 );
    assert( result.inputSampleCount == 4 );
    assert( result.inputContextSwitchCount == 2 );
    assert( result.inputTelemetryRecordCount == 2 );
    const auto& cpu = CpuPool( result );
    assert( cpu.totalAllocatedBytes == 780 );
    assert( cpu.totalFreedBytes == 360 );
    assert( cpu.peakLiveBytes == 630 );
    assert( cpu.endLiveBytes == 420 );
    assert( cpu.openBoundaryCount == 1 );
    assert( cpu.addressReuseCount == 1 );
    assert( cpu.accountingGapCount == 1 );
    assert( cpu.transientCount == 1 );
    assert( cpu.hasGrowth && cpu.hasChurn && cpu.hasTransient && cpu.hasAccountingGap );
    assert( !cpu.hasLeakCandidate );

    assert( result.gpuMemory.catalogState == GpuCatalogState::Complete );
    assert( result.gpuMemory.physicalBytes == 300 );
    assert( result.gpuMemory.residentBytes == 280 );
    assert( result.gpuMemory.logicalCapacityBytes == 220 );
    assert( result.gpuMemory.ownedPhysicalBytes == 300 );
    assert( result.gpuMemory.aliasResourceCount == 1 );
    assert( result.gpuMemory.rangeEvidenceBytes == 64 );
    assert( result.gpuMemory.maximumDirectWorkingSetBytes == 100 );
    assert( result.gpuMemory.maximumInclusiveWorkingSetBytes == 300 );
    assert( result.gpuMemory.dxgiUsageBytes && *result.gpuMemory.dxgiUsageBytes == 350 );
    if( result.gpuMemory.untrackedBytes )
    { std::cerr << "A DXGI sample minus an unrelated lifetime peak is not exact untracked memory\n"; return 1; }

    assert( result.ioRequests.size() == 4 );
    assert( result.ioSummary.completeCount == 1 );
    assert( result.ioSummary.cancelledCount == 1 );
    assert( result.ioSummary.errorCount == 1 );
    assert( result.ioSummary.orphanCount == 1 );
    assert( result.ioRequests[0].queueLatencyNs == 2 );
    assert( result.ioRequests[0].executionNs == 8 );

    assert( result.sampling.available );
    assert( result.sampling.totalSamples == 4 );
    assert( result.sampling.unresolvedSamples == 1 );
    assert( result.sampling.leaves.size() == 3 );
    assert( result.scheduling.available );
    assert( result.scheduling.roles.size() == 2 );
    assert( result.scheduling.roles[0].runningNs + result.scheduling.roles[1].runningNs == 30 );
    assert( result.scheduling.roles[0].readyWaitNs + result.scheduling.roles[1].readyWaitNs == 8 );

    assert( result.telemetry.producers.size() == 1 );
    assert( result.telemetry.producers[0].eventCount == 8 );
    assert( result.telemetry.producers[0].cpuTimeNs == 1000 );
    assert( result.telemetry.producers[0].eventBytes == 256 );
    assert( result.telemetry.producers[0].dropped == 1 );
    assert( result.telemetry.producers[0].overflow == 1 );
    assert( !result.telemetry.producerQualityComplete );
    assert( !result.qualityComplete );
    assert( result.telemetry.frameImageCount == 2 );
    assert( result.telemetry.totalCaptureOverhead == TotalCaptureOverheadState::NotMeasuredSingleTrace );
    assert( result.telemetry.requiresAbValidation );
    assert( result.telemetry.evidenceClasses.size() >= 3 );

    MemoryIoSamplingSource workerSource( CatalogFixtureMode::Complete, true, true,
        TraceSourceKind::Snapshot );
    const auto workerResult = MemoryIoSamplingTelemetryScanner( workerSource, 2, 2, 20 ).Scan();
    assert( workerResult.gpuMemory.physicalBytes == result.gpuMemory.physicalBytes );
    assert( workerResult.cpuMemoryPools[0].peakLiveBytes == cpu.peakLiveBytes );

    LeakCandidateSource leakSource;
    const auto leakResult = MemoryIoSamplingTelemetryScanner( leakSource, 2, 2, 20 ).Scan();
    assert( leakResult.cpuMemoryPools.size() == 1 );
    assert( leakResult.cpuMemoryPools[0].hasLeakCandidate );
    assert( leakResult.cpuMemoryPools[0].leakCandidateCount == 1 );

    MemoryIoSamplingSource invalid( CatalogFixtureMode::Invalid );
    const auto invalidResult = MemoryIoSamplingTelemetryScanner( invalid, 2, 2, 20 ).Scan();
    assert( invalidResult.gpuMemory.catalogState == GpuCatalogState::Invalid );
    assert( !invalidResult.gpuMemory.resourceFactsAvailable );
    assert( invalidResult.gpuMemory.physicalBytes == 300 );
    assert( invalidResult.gpuMemory.logicalCapacityBytes == 0 );

    MemoryIoSamplingSource disabled( CatalogFixtureMode::Disabled, false, false );
    const auto disabledResult = MemoryIoSamplingTelemetryScanner( disabled, 2, 2, 20 ).Scan();
    assert( disabledResult.gpuMemory.catalogState == GpuCatalogState::Disabled );
    assert( !disabledResult.sampling.available );
    assert( disabledResult.sampling.unavailableReason == "sampling permission unavailable" );
    assert( !disabledResult.scheduling.available );
    assert( disabledResult.scheduling.unavailableReason == "context switch permission unavailable" );

    MemoryIoSamplingSource absent( CatalogFixtureMode::Absent );
    const auto absentResult = MemoryIoSamplingTelemetryScanner( absent, 2, 2, 20 ).Scan();
    assert( absentResult.gpuMemory.catalogState == GpuCatalogState::Absent );
    assert( !absentResult.gpuMemory.resourceFactsAvailable );
    return 0;
}

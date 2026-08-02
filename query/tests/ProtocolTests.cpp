#include "TracyAnalysis.hpp"
#include "TracyMemoryAnalysis.hpp"
#include "TracyQueryService.hpp"
#include "FakeTraceSource.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#ifndef TRACY_QUERY_SCHEMA_PATH
#  error TRACY_QUERY_SCHEMA_PATH must be defined
#endif

#ifndef TRACY_QUERY_COVERAGE_PATH
#  error TRACY_QUERY_COVERAGE_PATH must be defined
#endif

#ifndef TRACY_QUERY_FIELD_COVERAGE_PATH
#  error TRACY_QUERY_FIELD_COVERAGE_PATH must be defined
#endif

#ifndef TRACY_QUERY_MCP_COVERAGE_PATH
#  error TRACY_QUERY_MCP_COVERAGE_PATH must be defined
#endif

static nlohmann::json LoadJson( const char* path )
{
    std::ifstream stream( path, std::ios::binary );
    if( !stream ) throw std::runtime_error( std::string( "Cannot open " ) + path );
    return nlohmann::json::parse( stream );
}

static nlohmann::json Request( int id, std::string method, nlohmann::json params = nlohmann::json::object() )
{
    return { { "protocol", "tracy-query/1" }, { "id", id }, { "method", std::move( method ) }, { "params", std::move( params ) } };
}

static nlohmann::json ValidParams( const std::string& method, const std::string& traceId, const std::string& baselineId )
{
    using nlohmann::json;
    json params = { { "trace_id", traceId } };
    if( method == "system.describe" || method == "system.schema" || method == "trace.list" || method == "trace.open" || method == "trace.close" ) return json::object();
    if( method == "thread.get" ) params["ref"] = "fake:thread:1";
    if( method == "thread.statistics" || method == "thread.timeline" || method == "thread.migration" || method == "context_switch.thread" ) params["thread_ref"] = "fake:thread:1";
    if( method == "frame.get" ) params["ref"] = "fake:frame:0";
    if( method == "entity.related" || method == "correlation.chain" || method == "timeline.correlated_slice" ) params["ref"] = "fake:frame-identity:281474976710657";
    if( method == "frame.range_mapping" || method == "timeline.slice" ) { params["start_ns"] = "0"; params["end_ns"] = "100"; }
    if( method == "frame_image.metadata" || method == "frame_image.resource" || method == "frame_image.raw" ) params["ref"] = "fake:frame-image:0";
    if( method == "producer.get" ) params["key"] = "test.real-zero";
    if( method == "catalog.get" ) params["definition_key"] = "jn-def:v1:source:03e2e6f19c364e11";
    if( method == "zone.cpu.get" || method == "zone.cpu.tree" ) params["ref"] = "fake:cpu-zone:0";
    if( method == "zone.gpu.get" || method == "zone.gpu.tree" ) params["ref"] = "fake:gpu-zone:0";
    if( method == "gpu.pass.get" ) params["ref"] = "fake:gfx-entity:9223372036854775810";
    if( method == "memory.get" ) params["ref"] = "fake:memory-event:0";
    if( method == "memory.active_at_time" ) params["time_ns"] = "50";
    if( method == "memory.frame_snapshot" ) params["frame_index"] = 0;
    if( method == "memory.diff" ) { params["base_frame_index"] = 0; params["target_frame_index"] = 0; }
    if( method == "lock.get" ) params["ref"] = "fake:lock:1";
    if( method == "lock.timeline" ) params["lock_ref"] = "fake:lock:1";
    if( method == "plot.points" || method == "plot.range" || method == "plot.downsample" || method == "plot.statistics" ) params["plot_ref"] = "fake:plot:0";
    if( method == "message.get" ) params["ref"] = "fake:message:0";
    if( method == "job.get" || method == "job.dependencies" || method == "job.gfx_chain" ) params["ref"] = "fake:job:1";
    if( method == "callstack.resolve" || method == "callstack.batch" ) params["callstacks"] = json::array( { "1" } );
    if( method == "callstack.frames" || method == "callstack.parent" ) params["callstack"] = "1";
    if( method == "symbol.get" || method == "symbol.raw_code" || method == "symbol.disassembly" ) params["ref"] = "fake:symbol:1";
    if( method == "symbol.address" || method == "hardware_sample.address" || method == "hardware_sample.events" ) params["address"] = "0x1";
    if( method == "source.lines" || method == "source.raw" ) params["ref"] = "fake:source-file:0";
    if( method == "statistics.compute" ) params["values_ns"] = json::array( { "1", "2", "3" } );
    if( method.rfind( "compare.", 0 ) == 0 ) params["baseline_trace_id"] = baselineId;
    return params;
}

struct TemporaryTraceFiles
{
    TemporaryTraceFiles()
    {
        const auto suffix = std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() );
        root = std::filesystem::temp_directory_path() / ( "tracy-query-contract-" + suffix );
        std::filesystem::create_directories( root );
        baseline = root / "baseline.tracy";
        candidate = root / "candidate.tracy";
        n11 = root / "n11.tracy";
        duplicateIdentity = root / "duplicate-identity.tracy";
        malformedIdentity = root / "malformed-identity.tracy";
        conflictingIdentity = root / "conflicting-identity.tracy";
        mismatchedConnection = root / "mismatched-connection.tracy";
        reconnectCatalog = root / "reconnect-catalog.tracy";
        outsideRoot = std::filesystem::temp_directory_path() / ( "tracy-query-outside-" + suffix );
        std::filesystem::create_directories( outsideRoot );
        outside = outsideRoot / "outside.tracy";
        std::ofstream( baseline, std::ios::binary ).put( '\0' );
        std::ofstream( candidate, std::ios::binary ).put( '\0' );
        std::ofstream( n11, std::ios::binary ).put( '\0' );
        std::ofstream( duplicateIdentity, std::ios::binary ).put( '\0' );
        std::ofstream( malformedIdentity, std::ios::binary ).put( '\0' );
        std::ofstream( conflictingIdentity, std::ios::binary ).put( '\0' );
        std::ofstream( mismatchedConnection, std::ios::binary ).put( '\0' );
        std::ofstream( reconnectCatalog, std::ios::binary ).put( '\0' );
        std::ofstream( outside, std::ios::binary ).put( '\0' );
    }

    ~TemporaryTraceFiles()
    {
        std::error_code error;
        std::filesystem::remove_all( root, error );
        std::filesystem::remove_all( outsideRoot, error );
    }

    std::filesystem::path root;
    std::filesystem::path baseline;
    std::filesystem::path candidate;
    std::filesystem::path n11;
    std::filesystem::path duplicateIdentity;
    std::filesystem::path malformedIdentity;
    std::filesystem::path conflictingIdentity;
    std::filesystem::path mismatchedConnection;
    std::filesystem::path reconnectCatalog;
    std::filesystem::path outsideRoot;
    std::filesystem::path outside;
};

int main()
{
    const auto schema = LoadJson( TRACY_QUERY_SCHEMA_PATH );
    assert( schema.at( "$defs" ).at( "request" ).at( "properties" ).at( "protocol" ).at( "const" ) == "tracy-query/1" );
    assert( schema.at( "$defs" ).at( "success" ).at( "properties" ).at( "schema_version" ).at( "const" ) == "1.10.0" );
    assert( schema.at( "$defs" ).at( "success" ).at( "required" ).size() == 9 );
    assert( schema.at( "$defs" ).at( "page" ).at( "required" ).size() == 7 );
    assert( schema.at( "$defs" ).contains( "budget" ) );
    assert( schema.at( "$defs" ).contains( "captureIdentity" ) );
    assert( schema.at( "$defs" ).at( "errorCode" ).at( "enum" ).size() == 19 );

    const auto coverage = LoadJson( TRACY_QUERY_COVERAGE_PATH );
    assert( coverage.at( "domains" ).size() == 33 );
    assert( coverage.at( "coverage_level" ) == "domain" );
    assert( coverage.at( "domain_status" ) == "complete" );
    assert( coverage.at( "field_status" ) == "complete" );
    assert( coverage.at( "mcp_status" ) == "complete" );
    for( const auto& domain : coverage.at( "domains" ) )
    {
        assert( domain.at( "domain" ).is_string() );
        assert( !domain.at( "methods" ).empty() );
        assert( !domain.at( "worker_data" ).empty() );
        assert( domain.at( "status" ) == "complete" );
        assert( !domain.at( "tests" ).empty() );
    }

    const auto fieldCoverage = LoadJson( TRACY_QUERY_FIELD_COVERAGE_PATH );
    assert( fieldCoverage.at( "coverage_level" ) == "field" );
    assert( fieldCoverage.at( "status" ) == "complete" );
    assert( fieldCoverage.at( "entities" ).size() >= 25 );
    std::set<std::string> fieldEntities;
    size_t mappedFields = 0;
    size_t unmappedFields = 0;
    for( const auto& entity : fieldCoverage.at( "entities" ) )
    {
        assert( fieldEntities.emplace( entity.at( "entity" ).get<std::string>() ).second );
        assert( entity.at( "persisted_fields" ).is_array() );
        assert( entity.at( "query_fields" ).is_array() );
        assert( entity.at( "unmapped_fields" ).is_array() );
        assert( entity.at( "methods" ).is_array() );
        assert( entity.at( "status" ) == ( entity.at( "unmapped_fields" ).empty() ? "complete" : "partial" ) );
        mappedFields += entity.at( "query_fields" ).size();
        unmappedFields += entity.at( "unmapped_fields" ).size();
    }
    assert( mappedFields > 100 );
    assert( unmappedFields == 0 );
    assert( fieldEntities.contains( "zone.cpu" ) );
    assert( fieldEntities.contains( "gpu.context" ) );
    assert( fieldEntities.contains( "gpu.taxonomy" ) );
    assert( fieldEntities.contains( "hardware_sample" ) );
    assert( fieldEntities.contains( "job" ) );
    assert( fieldEntities.contains( "job.gfx.statistics" ) );
    assert( fieldEntities.contains( "job.gfx_chain" ) );
    assert( fieldEntities.contains( "trace.capture_identity" ) );
    assert( fieldCoverage.at( "non_persisted" ).size() >= 3 );

    const auto mcpCoverage = LoadJson( TRACY_QUERY_MCP_COVERAGE_PATH );
    assert( mcpCoverage.at( "coverage_level" ) == "mcp" );
    assert( mcpCoverage.at( "status" ) == "complete" );
    assert( mcpCoverage.at( "workflow_tools" ).size() == 12 );
    assert( mcpCoverage.at( "complete_route" ).at( "tool" ) == "tracy_inspect" );
    assert( mcpCoverage.at( "public_method_registry" ) == "tracy::query::QueryMethodRegistry" );

    using tracy::analysis::ComputeStatistics;
    const auto stats = ComputeStatistics( std::vector<int64_t>{ 1, 2, 3, 4, 100 } );
    assert( stats.count == 5 );
    assert( stats.total == 110 );
    assert( stats.min == 1 );
    assert( stats.max == 100 );
    assert( stats.median == 3 );
    assert( stats.p90 > 60 && stats.p90 < 70 );
    assert( stats.truncatedMean == 2.5 );

    using namespace tracy::analysis;
    const std::vector<MemoryEventInput> memoryEvents = {
        { { 1, 0 }, 1, 100, 0, 15, 1, 1, 0, 0 },
        { { 1, 1 }, 2, 20, 10, 20, 1, 1, 0, 0 },
        { { 1, 2 }, 3, 30, 12, 18, 1, 1, 0, 0 },
        { { 1, 3 }, 4, 50, 5, std::nullopt, 1, 0, 0, 0 }
    };
    const auto snapshot = BuildMemoryFrameSnapshot( 10, 20, { 1 }, memoryEvents );
    assert( snapshot.valid && snapshot.consistent );
    assert( snapshot.total.startBytes == 150 && snapshot.total.startCount == 2 );
    assert( snapshot.total.allocatedBytes == 50 && snapshot.total.allocatedCount == 2 );
    assert( snapshot.total.freedBytes == 130 && snapshot.total.freedCount == 2 );
    assert( snapshot.total.endBytes == 70 && snapshot.total.endCount == 2 );
    assert( snapshot.total.peakBytes == 200 && snapshot.total.peakCount == 4 );
    assert( snapshot.activeAtStart.size() == 2 && snapshot.activeAtEnd.size() == 2 );
    assert( snapshot.allocated.size() == 2 && snapshot.freed.size() == 2 && snapshot.transitions.size() == 3 );

    const std::vector<GpuMemoryCpuZoneInput> cpuMarkers = {
        { 0, GpuMemoryRequestMarker, "Upload request", "GTMEM1|SCOPE|label=7|frame=3\nGTMEM1|RESOURCE|allocation=42|physical=100|bytes=4096|offset=256|owner=7|physical_owner=7|kind=T|segment=L|flags=0|name=VSM%20Atlas", 9, 0, 100 },
        { 1, GpuMemoryPassMarker, "RenderPass", "GTMEM1|PASS|pass=11|label=7|frame=3|level=1|ordinal=2|ops=draw|commands=4|uses=1|total=1|chunks=1|untracked=0|truncated=0|dropped=0\nGTMEM1|USE|pass=11|data=42:T:3", 9, 20, 80 },
        { 2, GpuMemoryOriginMarker, GpuMemoryOriginMarker, "GTMEM2|ORIGIN|allocation=100|layer=P|connection=1|replayed=0|pre_capture=0|callstack_requested=8|callstack_emitted=1|residency=R|managed=1", 9, 10, 11 },
        { 3, GpuMemoryOriginMarker, GpuMemoryOriginMarker, "GTMEM2|ORIGIN|allocation=42|layer=L|connection=1|replayed=0|pre_capture=0|callstack_requested=8|callstack_emitted=1|residency=U|managed=0", 9, 12, 13 },
        { 4, GpuMemoryResidencyMarker, GpuMemoryResidencyMarker, "GTMEM2|RESIDENCY|allocation=100|frame=3|fence=7|bytes=16384|connection=1|state=E|reason=2|replayed=0|flags=0", 9, 60, 61 }
    };
    // The authoritative reference token must pair an explicit raw GPU zone to
    // its taxonomy memory pass even when their display names differ.
    const std::vector<GpuMemoryGpuZoneInput> gpuMarkers = { { 0, "ExplicitRawPass", 9, 30, 1000, 2000, 11 } };
    const std::vector<GpuMemoryAllocationInput> gpuAllocations = {
        { { 5, 0 }, 100, 16384, 9, 40, std::nullopt, 8, 0, "GPU D3D12 Physical Local Heap" },
        { { 6, 0 }, 42, 4096, 9, 50, std::nullopt, 9, 0, "GPU D3D12 Logical Texture" }
    };
    const auto attribution = BuildGpuMemoryAttribution( cpuMarkers, gpuMarkers, gpuAllocations );
    assert( attribution.protocolPresent && attribution.complete );
    assert( attribution.protocol2Present && attribution.origins.size() == 2 && attribution.residencyEvents.size() == 1 );
    assert( attribution.requestScopes.size() == 1 && attribution.passes.size() == 1 && attribution.allocations.size() == 2 );
    assert( attribution.passes[0].complete && attribution.passes[0].gpuPairing == GpuZonePairing::Exact );
    assert( attribution.passes[0].uses.size() == 1 && attribution.passes[0].uses[0].allocationId == 42 );
    const auto logicalAllocation = attribution.allocationById.at( 42 );
    assert( attribution.allocations[logicalAllocation].requestLabelId == 7 && attribution.allocations[logicalAllocation].passIndices == std::vector<size_t> { 0 } );
    assert( attribution.logicalResources.size() == 1 && attribution.logicalResources[0].logicalResourceId == 42 && attribution.logicalResources[0].physicalAllocationId == 100 );
    assert( attribution.logicalResources[0].primaryOwnerId == 7 && attribution.logicalResources[0].physicalOwnerId == 7 );
    assert( attribution.ownerRollups.size() == 1 && attribution.ownerRollups[0].taxonomyId == 7 );
    assert( attribution.ownerRollups[0].physicalBytes == 16384 && attribution.ownerRollups[0].physicalAllocationCount == 1 && attribution.ownerRollups[0].logicalResourceCount == 1 );
    assert( attribution.workingSets.size() == 1 && attribution.workingSets[0].frame == 3 && attribution.workingSets[0].taxonomyId == 7 );
    assert( attribution.workingSets[0].referencedPhysicalBytes == 16384 && attribution.workingSets[0].physicalAllocationCount == 1 && attribution.workingSets[0].logicalResourceCount == 1 );
    assert( attribution.fragmentation.size() == 1 && attribution.fragmentation[0].heapId == 100 );
    assert( attribution.fragmentation[0].coveredBytes == 4096 && attribution.fragmentation[0].freeBytes == 12288 );
    assert( attribution.churn.createdCount == 1 && attribution.churn.createdBytes == 16384 && attribution.churn.peakPhysicalBytes == 16384 );
    assert( attribution.residency.evictedCount == 1 && attribution.residency.evictedBytes == 16384 );
    assert( FormatGpuMemoryUsage( 3 ) == "Read/Write" );

    const std::vector<GpuMemoryCpuZoneInput> boundaryCpuMarkers = {
        { 10, GpuMemoryPassMarker, "BoundaryPass", "GTMEM1|PASS|pass=12|label=7|frame=3|level=1|ordinal=3|ops=draw|commands=1|uses=0|total=0|chunks=0|untracked=0|truncated=0|dropped=0", 9, 0, 10 }
    };
    const std::vector<GpuMemoryGpuZoneInput> boundaryGpuMarkers = {
        { 10, "BoundaryPass", 9, 20, 1000, 2000, 0 }
    };
    const auto boundaryAttribution = BuildGpuMemoryAttribution( boundaryCpuMarkers, boundaryGpuMarkers, {} );
    assert( boundaryAttribution.complete && boundaryAttribution.warnings.empty() );
    assert( boundaryAttribution.captureBoundaryPasses == 1 );
    assert( boundaryAttribution.passes.size() == 1 && boundaryAttribution.passes[0].gpuPairing == GpuZonePairing::CaptureBoundary );

    const std::vector<GpuMemoryCpuZoneInput> unsubmittedCpuMarkers = {
        { 11, GpuMemoryPassMarker, "UnsubmittedPass", "GTMEM1|PASS|pass=13|label=7|frame=3|level=1|ordinal=4|ops=draw|commands=1|uses=0|total=0|chunks=0|untracked=0|truncated=0|dropped=0|command_list=99", 9, 30, 40 }
    };
    const auto unsubmittedAttribution = BuildGpuMemoryAttribution( unsubmittedCpuMarkers, {}, {}, { 100 } );
    assert( unsubmittedAttribution.complete && unsubmittedAttribution.warnings.empty() );
    assert( unsubmittedAttribution.submissionUnobservedPasses == 1 );
    assert( unsubmittedAttribution.passes.size() == 1 && unsubmittedAttribution.passes[0].commandListId == 99 );
    assert( unsubmittedAttribution.passes[0].gpuPairing == GpuZonePairing::SubmissionUnobserved );

    const auto gpuUnavailableAttribution = BuildGpuMemoryAttribution( unsubmittedCpuMarkers, {}, {}, { 99 }, { 13 } );
    assert( gpuUnavailableAttribution.complete && gpuUnavailableAttribution.warnings.empty() );
    assert( gpuUnavailableAttribution.gpuResultUnavailablePasses == 1 );
    assert( gpuUnavailableAttribution.passes[0].gpuPairing == GpuZonePairing::GpuResultUnavailable );

    tracy::query::test::FakeTraceSource fake;
    assert( fake.AcquireReadView().sourceKind == TraceSourceKind::Snapshot );
    assert( fake.AcquireReadView().complete );
    assert( fake.GetCapabilities().size() >= 20 );
    const auto fakeCapabilities = fake.GetCapabilities();
    assert( std::find_if( fakeCapabilities.begin(), fakeCapabilities.end(), []( const auto& capability ) {
        return capability.domain == "capture" && capability.present && capability.queryable;
    } ) != fakeCapabilities.end() );
    assert( std::find_if( fakeCapabilities.begin(), fakeCapabilities.end(), []( const auto& capability ) {
        return capability.domain == "catalog" && capability.present && capability.queryable;
    } ) != fakeCapabilities.end() );
    assert( fake.GetTraceInfo().fingerprint == std::string( 64, 'f' ) );
    assert( fake.GetThreads().size() == 1 && fake.GetFrameSets().size() == 1 && fake.GetGpuContexts().size() == 1 );
    assert( fake.GetMemoryPools().size() == 1 && fake.GetPlotList().size() == 1 && fake.GetLocks().size() == 1 );
    ScanRange entire;
    assert( fake.ScanCpuZones( entire ).size() == 2 && fake.ScanGpuZones( entire ).size() == 2 && fake.ScanFrames( entire ).size() == 1 );
    assert( fake.ScanMemoryEvents( entire ).size() == 1 && fake.ScanMessages( entire ).size() == 1 && fake.ScanPlots( entire ).size() == 1 );
    assert( fake.ScanContextSwitchEvents( entire ).size() == 1 && fake.ScanCpuContextSwitchEvents( entire ).size() == 1 && fake.ScanSampleEvents( entire ).size() == 1 && fake.ScanGhostZones( entire ).size() == 1 );
    assert( fake.ScanLockEvents( entire ).size() == 1 && fake.GetHardwareSamples().size() == 1 && fake.GetSymbols().size() == 1 && fake.GetSourceLocations().size() == 1 );
    assert( fake.ResolveCallstacks( { 1 }, 1 ).size() == 1 && fake.ResolveParentCallstacks( { 1 }, 1 ).size() == 1 );
    assert( fake.GetSourceResources().size() == 1 && fake.GetSymbolResources().size() == 1 && fake.GetFrameImageResources().size() == 1 );
    assert( fake.GetMemoryFrameSnapshot( 0, 0, {}, false ).valid );
    assert( fake.GetMemoryEvent( { 1, 0 } ).has_value() && fake.GetGpuMemoryAttribution().allocations.size() == 2 );
    assert( fake.GetGpuMemoryAttribution().logicalResources.size() == 1 && fake.GetGpuMemoryAttribution().ownerRollups.size() == 1 && fake.GetGpuMemoryAttribution().workingSets.size() == 1 );
    assert( fake.GetJobs().size() == 2 && fake.GetGfxDispatches().size() == 1 && fake.GetGfxEntities().size() == 5 && fake.GetGfxLinks().size() == 11 );
    assert( fake.ReadEmbeddedSource( 0, 64 ).embedded && fake.ReadSymbolCode( 1, 64 ).bytes.size() == 1 && fake.ReadFrameImage( 0, 64 ).rgba.size() == 4 );
    assert( fake.ReadEmbeddedSourceBytes( 0, 0, 64 ).bytes.size() == 3 && fake.ReadSymbolCodeBytes( 1, 0, 64 ).bytes.size() == 1 && fake.ReadFrameImageBc1( 0, 0, 64 ).bytes.size() == 8 );
    assert( fake.GetHardwareSampleEvents( 1, "all", 0, 1 ).front().timeNs == 33 );
    assert( fake.GetSymbolAddressMappings( 0, 1 ).size() == 1 && fake.ResolveSymbolAddress( 1 ).has_value() );
    assert( fake.ParseEntityRef( fake.MakeEntityRef( "cpu-zone", 7 ), "cpu-zone" ) == 7 );
    ScanRange before; before.endNs = 10; assert( fake.ScanCpuZones( before ).empty() );
    ScanRange after; after.startNs = 60; assert( fake.ScanCpuZones( after ).empty() );
    ScanRange paged; paged.offset = 2; assert( fake.ScanCpuZones( paged ).empty() );

    TemporaryTraceFiles files;
    tracy::query::SessionManager sessions( { files.root }, 3,
        []( const std::filesystem::path& path, tracy::query::SessionManager::StateCallback callback ) -> std::unique_ptr<tracy::analysis::TraceSource> {
            callback( tracy::analysis::TraceSourceState::Indexing );
            const auto filename = path.filename().string();
            if( filename == "baseline.tracy" ) return std::make_unique<tracy::query::test::FakeTraceSource>( true, true );
            if( filename == "n11.tracy" ) return std::make_unique<tracy::query::test::FakeTraceSource>( false, false, true );
            if( filename == "duplicate-identity.tracy" )
            {
                auto records = tracy::query::test::FakeTraceSource::DefaultIdentityAppInfo();
                records.push_back( records.front() );
                return std::make_unique<tracy::query::test::FakeTraceSource>( std::move( records ) );
            }
            if( filename == "malformed-identity.tracy" )
            {
                auto records = tracy::query::test::FakeTraceSource::DefaultIdentityAppInfo();
                records.resize( 1 );
                records.emplace_back( "JNCI1|{broken" );
                records.emplace_back( "JNCI1|{\"schema_version\":2,\"kind\":\"future\",\"producer\":\"future-client\",\"identity\":{}}" );
                records.emplace_back( "JNCI2|{\"schema_version\":2,\"kind\":\"future\",\"producer\":\"future-client\",\"identity\":{}}" );
                return std::make_unique<tracy::query::test::FakeTraceSource>( std::move( records ) );
            }
            if( filename == "conflicting-identity.tracy" )
            {
                auto records = tracy::query::test::FakeTraceSource::DefaultIdentityAppInfo();
                records.emplace_back( "JNCI1|{\"schema_version\":1,\"kind\":\"core\",\"producer\":\"conflicting-client\",\"identity\":{\"protocol\":{\"jn_abi_version\":\"0x00020000\"}}}" );
                return std::make_unique<tracy::query::test::FakeTraceSource>( std::move( records ) );
            }
            if( filename == "mismatched-connection.tracy" )
            {
                auto records = tracy::query::test::FakeTraceSource::DefaultIdentityAppInfo();
                for( const size_t index : { size_t( 4 ), size_t( 5 ) } )
                {
                    auto record = records[index];
                    const auto offset = record.find( "\"connection_id\":\"1\"" );
                    assert( offset != std::string::npos );
                    record.replace( offset, sizeof( "\"connection_id\":\"1\"" ) - 1, "\"connection_id\":\"2\"" );
                    records.emplace_back( std::move( record ) );
                }
                return std::make_unique<tracy::query::test::FakeTraceSource>( std::move( records ) );
            }
            if( filename == "reconnect-catalog.tracy" )
            {
                auto records = tracy::query::test::FakeTraceSource::DefaultIdentityAppInfo();
                auto& connectionIdentity = records[2];
                auto identityOffset = connectionIdentity.find( "\"id\":\"1\"" );
                assert( identityOffset != std::string::npos );
                connectionIdentity.replace( identityOffset, sizeof( "\"id\":\"1\"" ) - 1, "\"id\":\"2\"" );
                for( auto& record : records )
                {
                    if( !record.starts_with( "JNCTX1|" ) && !record.starts_with( "JNQ1|" ) && !record.starts_with( "JNGT1|" ) )
                        continue;
                    auto offset = record.find( "\"connection_id\":\"1\"" );
                    assert( offset != std::string::npos );
                    record.replace( offset, sizeof( "\"connection_id\":\"1\"" ) - 1, "\"connection_id\":\"2\"" );
                }
                const auto definitionRecord = std::find_if( records.begin(), records.end(),
                    []( const auto& record ) { return record.starts_with( "JNCAT1|" ); } );
                const auto entityRecord = std::find_if( records.begin(), records.end(),
                    []( const auto& record ) { return record.starts_with( "JNENT1|" ); } );
                assert( definitionRecord != records.end() && entityRecord != records.end() );
                auto currentDefinition = *definitionRecord;
                auto currentEntity = *entityRecord;
                auto definitionOffset = currentDefinition.find( "\"connection_id\":\"1\"" );
                assert( definitionOffset != std::string::npos );
                currentDefinition.replace( definitionOffset, sizeof( "\"connection_id\":\"1\"" ) - 1, "\"connection_id\":\"2\"" );
                records.emplace_back( std::move( currentDefinition ) );
                auto entityConnectionOffset = currentEntity.find( "\"connection_id\":\"1\"" );
                assert( entityConnectionOffset != std::string::npos );
                currentEntity.replace( entityConnectionOffset, sizeof( "\"connection_id\":\"1\"" ) - 1, "\"connection_id\":\"2\"" );
                auto entityIdOffset = currentEntity.find( "281474976710657" );
                assert( entityIdOffset != std::string::npos );
                currentEntity.replace( entityIdOffset, sizeof( "281474976710657" ) - 1, "562949953421313" );
                auto generationOffset = currentEntity.find( "\"connection_generation\":1" );
                assert( generationOffset != std::string::npos );
                currentEntity.replace( generationOffset, sizeof( "\"connection_generation\":1" ) - 1, "\"connection_generation\":2" );
                records.emplace_back( std::move( currentEntity ) );
                return std::make_unique<tracy::query::test::FakeTraceSource>( std::move( records ) );
            }
            return std::make_unique<tracy::query::test::FakeTraceSource>( false, true );
        } );
    tracy::query::QueryService service( sessions, 1024 * 1024 );

    const auto expectPathError = [&]( const std::filesystem::path& path, tracy::query::SessionErrorCode code ) {
        try { (void)sessions.ResolveTracePath( path ); }
        catch( const tracy::query::SessionError& error ) { assert( error.code == code ); return; }
        assert( false && "path resolution unexpectedly succeeded" );
    };
    expectPathError( files.root / "missing.tracy", tracy::query::SessionErrorCode::TraceNotFound );
    const auto wrongExtension = files.root / "trace.txt"; std::ofstream( wrongExtension ).put( '\0' );
    expectPathError( wrongExtension, tracy::query::SessionErrorCode::TraceOpenFailed );
    const auto directoryTrace = files.root / "directory.tracy"; std::filesystem::create_directory( directoryTrace );
    expectPathError( directoryTrace, tracy::query::SessionErrorCode::TraceOpenFailed );
    expectPathError( files.root / ".." / files.outsideRoot.filename() / files.outside.filename(), tracy::query::SessionErrorCode::PathNotAllowed );
    std::error_code symlinkError;
    const auto escapeLink = files.root / "escape.tracy";
    std::filesystem::create_symlink( files.outside, escapeLink, symlinkError );
    if( !symlinkError ) expectPathError( escapeLink, tracy::query::SessionErrorCode::PathNotAllowed );

    const auto openBaseline = service.Execute( Request( 100, "trace.open", { { "path", files.baseline.string() } } ) );
    assert( openBaseline.at( "ok" ) );
    const auto baselineId = openBaseline.at( "data" ).at( "trace_id" ).get<std::string>();
    assert( sessions.WaitReady( baselineId, std::chrono::seconds( 5 ) ).state == TraceSourceState::Ready );
    const auto openCandidate = service.Execute( Request( 101, "trace.open", { { "path", files.candidate.string() } } ) );
    assert( openCandidate.at( "ok" ) );
    const auto candidateId = openCandidate.at( "data" ).at( "trace_id" ).get<std::string>();
    assert( sessions.WaitReady( candidateId, std::chrono::seconds( 5 ) ).state == TraceSourceState::Ready );

    const auto oldScript = service.Execute( Request( 1000, "runtime.script.summary", { { "trace_id", candidateId } } ) );
    assert( oldScript.at( "ok" ) && oldScript.at( "data" ).at( "present" ) == false && oldScript.at( "data" ).at( "complete" ) == false );
    const auto oldGc = service.Execute( Request( 1001, "memory.gc.summary", { { "trace_id", candidateId } } ) );
    assert( oldGc.at( "ok" ) && oldGc.at( "data" ).at( "present" ) == false && oldGc.at( "data" ).at( "complete" ) == false );

    const auto openN11 = service.Execute( Request( 1002, "trace.open", { { "path", files.n11.string() } } ) );
    assert( openN11.at( "ok" ) );
    const auto n11Id = openN11.at( "data" ).at( "trace_id" ).get<std::string>();
    assert( sessions.WaitReady( n11Id, std::chrono::seconds( 5 ) ).state == TraceSourceState::Ready );
    const auto scriptSummary = service.Execute( Request( 1003, "runtime.script.summary", { { "trace_id", n11Id } } ) ).at( "data" );
    assert( scriptSummary.at( "present" ) == true && scriptSummary.at( "complete" ) == true );
    assert( scriptSummary.at( "counts" ).at( "frames" ) == "2" && scriptSummary.at( "counts" ).at( "stacks" ) == "2" );
    assert( scriptSummary.at( "counts" ).at( "zones" ) == "2" && scriptSummary.at( "counts" ).at( "complete_zones" ) == "2" );
    assert( scriptSummary.at( "runtimes" ).at( "managed" ).at( "zones" ) == "1" );
    assert( scriptSummary.at( "runtimes" ).at( "lua" ).at( "zones" ) == "1" );
    const auto managedFrames = service.Execute( Request( 1004, "runtime.script.frames", { { "trace_id", n11Id }, { "runtime", "managed" } } ) ).at( "data" ).at( "frames" );
    assert( managedFrames.size() == 1 && managedFrames[0].at( "function" ) == "Fake.Managed.Caller" && managedFrames[0].at( "line" ) == 42 );
    const auto luaStacks = service.Execute( Request( 1005, "runtime.script.stacks", { { "trace_id", n11Id }, { "runtime", "lua" } } ) ).at( "data" ).at( "stacks" );
    assert( luaStacks.size() == 1 && luaStacks[0].at( "complete" ) == true && luaStacks[0].at( "frames" )[0].at( "function" ) == "FakeLuaUpdate" );
    const auto scriptZones = service.Execute( Request( 1006, "runtime.script.zones", { { "trace_id", n11Id } } ) ).at( "data" ).at( "zones" );
    assert( scriptZones.size() == 2 && scriptZones[0].at( "duration_ns" ) == "5" && scriptZones[1].at( "duration_ns" ) == "5" );
    const auto gcSummary = service.Execute( Request( 1007, "memory.gc.summary", { { "trace_id", n11Id } } ) ).at( "data" );
    assert( gcSummary.at( "present" ) == true && gcSummary.at( "complete" ) == true );
    assert( gcSummary.at( "counts" ).at( "events" ) == "4" && gcSummary.at( "counts" ).at( "paired_intervals" ) == "1" );
    assert( gcSummary.at( "latest" ).at( "managed_heap_used_bytes" ) == "1048576" );
    assert( gcSummary.at( "latest" ).at( "lua_heap_used_bytes" ) == "65536" );
    const auto luaGcEvents = service.Execute( Request( 1008, "memory.gc.events", { { "trace_id", n11Id }, { "runtime", "lua" } } ) ).at( "data" ).at( "events" );
    assert( luaGcEvents.size() == 1 && luaGcEvents[0].at( "kind_name" ) == "lua_heap_used" );
    assert( service.Execute( Request( 1009, "trace.close", { { "trace_id", n11Id } } ) ).at( "ok" ) );

    const auto described = service.Execute( Request( 102, "system.describe" ) );
    assert( described.at( "ok" ) );
    assert( described.at( "schema_version" ) == "1.10.0" );
    assert( described.at( "partial" ) == false && described.at( "omitted_count" ) == "0" );
    assert( described.at( "budget" ).at( "exhausted_by" ).empty() );
    std::set<std::string> describedMethods;
    for( const auto& method : described.at( "data" ).at( "methods" ) ) describedMethods.emplace( method.get<std::string>() );
    std::set<std::string> coveredMethods;
    for( const auto& domain : coverage.at( "domains" ) ) for( const auto& method : domain.at( "methods" ) ) coveredMethods.emplace( method.get<std::string>() );
    assert( describedMethods == coveredMethods );
    const auto& operations = described.at( "data" ).at( "operations" );
    assert( operations.size() == describedMethods.size() );
    for( const auto& operation : operations )
    {
        assert( operation.at( "schema_version" ) == "1.10.0" );
        assert( operation.at( "input_schema" ).at( "type" ) == "object" );
        assert( operation.at( "output_schema" ).at( "properties" ).at( "schema_version" ).at( "const" ) == "1.10.0" );
        assert( operation.at( "budget_parameters" ).size() == 4 );
    }
    const auto producerGetOperation = std::find_if( operations.begin(), operations.end(), []( const auto& operation ) {
        return operation.at( "method" ) == "producer.get";
    } );
    assert( producerGetOperation != operations.end() );
    const std::set<std::string> producerGetRequired(
        producerGetOperation->at( "required" ).begin(), producerGetOperation->at( "required" ).end() );
    assert( producerGetRequired == std::set<std::string>( { "trace_id", "key" } ) );

    const auto schemaResponse = service.Execute( Request( 103, "system.schema" ) );
    assert( schemaResponse.at( "ok" ) );
    assert( schemaResponse.at( "data" ).at( "coverage" ).at( "domain" ).at( "domain_status" ) == "complete" );
    assert( schemaResponse.at( "data" ).at( "coverage" ).at( "field" ).at( "status" ) == "complete" );
    assert( schemaResponse.at( "data" ).at( "coverage" ).at( "mcp" ).at( "status" ) == "complete" );
    assert( schemaResponse.at( "data" ).at( "operations" ) == operations );

    int requestId = 200;
    for( const auto& method : describedMethods )
    {
        if( method == "trace.open" || method == "trace.close" ) continue;
        const auto response = service.Execute( Request( requestId++, method, ValidParams( method, candidateId, baselineId ) ) );
        if( !response.value( "ok", false ) )
        {
            std::cerr << method << " failed: " << response.dump() << '\n';
            return 1;
        }
    }

    const auto boundedSourceCompare = service.Execute( Request( requestId++, "compare.source", {
        { "trace_id", candidateId }, { "baseline_trace_id", baselineId }, { "max_bytes", 16 }
    } ) );
    assert( boundedSourceCompare.at( "ok" ) );
    assert( boundedSourceCompare.at( "data" ).at( "changed" ).empty() );
    assert( boundedSourceCompare.at( "data" ).at( "inconclusive" ).size() == 1 );
    assert( boundedSourceCompare.at( "data" ).at( "inconclusive" )[0].at( "reason" ) == "bounded_prefix_equal" );
    assert( boundedSourceCompare.at( "data" ).at( "inconclusive" )[0].at( "compared_bytes" ) == "15" );

    const auto traceFields = service.Execute( Request( requestId++, "trace.info", { { "trace_id", candidateId } } ) ).at( "data" );
    assert( traceFields.at( "timer_multiplier" ) == 0.5 );
    assert( traceFields.at( "frame_offset" ) == "17" );
    assert( traceFields.at( "sampling_period_ns" ) == "1000" );
    assert( traceFields.at( "on_demand" ) == true );
    assert( traceFields.at( "legacy_queue_delay_ns" ).is_null() );
    assert( traceFields.at( "field_availability" ).at( "legacy_queue_delay_ns" ).at( "available" ) == false );

    const auto captureIdentity = service.Execute( Request( requestId++, "trace.identity", { { "trace_id", candidateId } } ) ).at( "data" );
    assert( captureIdentity.at( "present" ) == true );
    assert( captureIdentity.at( "complete" ) == true );
    assert( captureIdentity.at( "schema_version" ) == 1 );
    assert( captureIdentity.at( "identity" ).at( "runtime" ).at( "target_kind" ) == "editor" );
    assert( captureIdentity.at( "identity" ).at( "connection" ).at( "id" ) == "1" );
    assert( captureIdentity.at( "identity" ).at( "build" ).at( "repositories" ).at( "engine" ).at( "revision" ) == std::string( 40, '1' ) );
    assert( captureIdentity.at( "missing_required" ).empty() );
    assert( captureIdentity.at( "conflicts" ).empty() );
    assert( captureIdentity.at( "invalid_records" ).empty() );
    assert( captureIdentity.at( "records" ).at( "valid" ) == 4 );
    assert( captureIdentity.at( "canonical_fingerprint" ).get<std::string>().size() == 16 );
    const auto captureIdentityFingerprint = captureIdentity.at( "canonical_fingerprint" ).get<std::string>();

    const auto captureContext = service.Execute( Request( requestId++, "capture.context", { { "trace_id", candidateId } } ) ).at( "data" );
    assert( captureContext.at( "present" ) == true );
    assert( captureContext.at( "complete" ) == true );
    assert( captureContext.at( "generation" ) == "1" );
    assert( captureContext.at( "context" ).at( "workload" ).at( "scene" ) == "Init" );
    assert( captureContext.at( "missing_layers" ).empty() );

    const auto captureCoverage = service.Execute( Request( requestId++, "capture.coverage", { { "trace_id", candidateId } } ) ).at( "data" );
    assert( captureCoverage.at( "present" ) == true );
    assert( captureCoverage.at( "complete" ) == true );
    assert( captureCoverage.at( "evidence_kind" ) == "exact" );
    assert( captureCoverage.at( "producers" ).size() == 5 );
    const auto degradedProducer = std::find_if( captureCoverage.at( "producers" ).begin(), captureCoverage.at( "producers" ).end(),
        []( const auto& value ) { return value.at( "key" ) == "test.degraded"; } );
    assert( degradedProducer != captureCoverage.at( "producers" ).end() );
    assert( degradedProducer->at( "state" ) == "degraded" );
    assert( degradedProducer->at( "counters" ).at( "filtered" ) == "5" );
    assert( degradedProducer->at( "counters" ).at( "overflow" ) == "1" );
    const auto realZero = service.Execute( Request( requestId++, "producer.get", {
        { "trace_id", candidateId }, { "key", "test.real-zero" }
    } ) ).at( "data" );
    assert( realZero.at( "producer" ).at( "state" ) == "real_zero" );
    assert( realZero.at( "producer" ).at( "coverage_ratio" ) == 1.0 );

    const auto taxonomyTree = service.Execute( Request( requestId++, "gpu.taxonomy.tree", {
        { "trace_id", candidateId }, { "max_scan_events", 100 }
    } ) ).at( "data" );
    assert( taxonomyTree.at( "present" ) == true );
    assert( taxonomyTree.at( "schema_version" ) == 2 );
    assert( taxonomyTree.at( "nodes" ).size() == 7 );
    assert( taxonomyTree.at( "quality" ).at( "missing_part_count" ) == 0 );
    assert( taxonomyTree.at( "hierarchy_gate" ).at( "static_l0_has_multiple_l1" ) == true );
    assert( taxonomyTree.at( "hierarchy_gate" ).at( "static_l1_has_multiple_l2" ) == true );
    assert( taxonomyTree.at( "taxonomy_zone_count" ) == "2" );
    assert( taxonomyTree.at( "explicit_pass_zone_count" ) == "2" );

    const auto taxonomyCoverage = service.Execute( Request( requestId++, "gpu.taxonomy.coverage", {
        { "trace_id", candidateId }, { "max_scan_events", 100 }
    } ) ).at( "data" );
    assert( taxonomyCoverage.at( "statuses" ).at( "fallback" ).at( "available" ) == true );
    assert( taxonomyCoverage.at( "statuses" ).at( "explicit" ).at( "available" ) == true );
    assert( taxonomyCoverage.at( "statuses" ).at( "explicit" ).at( "count" ) == "2" );
    assert( taxonomyCoverage.at( "statuses" ).at( "fallback" ).at( "classified_marker_count" ) == "7" );
    assert( taxonomyCoverage.at( "statuses" ).at( "unclassified" ).at( "count" ) == "3" );
    assert( taxonomyCoverage.at( "statuses" ).at( "culled" ).at( "available" ) == false );

    const auto gpuPassSearch = service.Execute( Request( requestId++, "gpu.pass.search", {
        { "trace_id", candidateId }, { "taxonomy_id", "537067521" }, { "max_scan_events", 100 }
    } ) ).at( "data" );
    assert( gpuPassSearch.at( "present" ) == true && gpuPassSearch.at( "complete" ) == true );
    assert( gpuPassSearch.at( "effective" ) == true && gpuPassSearch.at( "producer_state" ) == "covered" );
    assert( gpuPassSearch.at( "source_mode" ) == "mixed" );
    assert( gpuPassSearch.at( "instance_count" ) == "2" && gpuPassSearch.at( "missing_gpu_zone_count" ) == "0" );
    assert( gpuPassSearch.at( "source_modes" ).at( "cpp-marker-command-list" ).at( "instance_count" ) == "1" );
    assert( gpuPassSearch.at( "source_modes" ).at( "managed-command-buffer" ).at( "instance_count" ) == "1" );
    assert( gpuPassSearch.at( "passes" ).size() == 1 );
    const auto gpuPass = gpuPassSearch.at( "passes" )[0];
    assert( gpuPass.at( "pass_source_id" ) == 77 );
    assert( gpuPass.at( "source_mode" ) == "cpp-marker-command-list" );
    assert( gpuPass.at( "taxonomy_id" ) == "537067521" );
    assert( gpuPass.at( "taxonomy_evidence" ) == "stable_id_relation" );
    assert( gpuPass.at( "name" ) == "Shadows.Draw" );
    assert( gpuPass.at( "source" ).at( "file" ) == "Runtime/Graphics/ScriptableRenderLoop/ScriptableDrawShadows.cpp" );
    assert( gpuPass.at( "source" ).at( "line" ) == 373 );
    assert( gpuPass.at( "gpu" ).at( "query_id" ) == 5 );
    assert( gpuPass.at( "reference_token" ) == "11" );
    assert( gpuPass.at( "callstack" ) == "1" );
    assert( gpuPass.at( "callstack_ref" ) == "fake:callstack:1" );
    const auto gpuPassGet = service.Execute( Request( requestId++, "gpu.pass.get", {
        { "trace_id", candidateId }, { "ref", gpuPass.at( "ref" ) }, { "max_scan_events", 100 }
    } ) ).at( "data" );
    assert( gpuPassGet.at( "pass" ).at( "gpu_zone_ref" ) == gpuPass.at( "gpu_zone_ref" ) );

    const auto managedGpuPassSearch = service.Execute( Request( requestId++, "gpu.pass.search", {
        { "trace_id", candidateId }, { "source_mode", "managed-command-buffer" }, { "max_scan_events", 100 }
    } ) ).at( "data" );
    assert( managedGpuPassSearch.at( "source_mode" ) == "managed-command-buffer" );
    assert( managedGpuPassSearch.at( "instance_count" ) == "1" );
    assert( managedGpuPassSearch.at( "page" ).at( "total" ) == 1 );
    assert( managedGpuPassSearch.at( "producer" ).at( "key" ) == "gpu.pass.managed" );
    assert( managedGpuPassSearch.at( "producers" ).size() == 1 );
    assert( managedGpuPassSearch.at( "present" ) == true && managedGpuPassSearch.at( "effective" ) == true );
    assert( managedGpuPassSearch.at( "complete" ) == true && managedGpuPassSearch.at( "producer_state" ) == "covered" );
    assert( managedGpuPassSearch.at( "missing_gpu_zone_count" ) == "0" );
    assert( managedGpuPassSearch.at( "unresolved_taxonomy_count" ) == "0" );
    assert( managedGpuPassSearch.at( "passes" ).size() == 1 );
    const auto managedGpuPass = managedGpuPassSearch.at( "passes" )[0];
    assert( managedGpuPass.at( "pass_source_id" ) == 0x81234567u );
    assert( managedGpuPass.at( "source_mode" ) == "managed-command-buffer" );
    assert( managedGpuPass.at( "taxonomy_id" ) == "537198593" );
    assert( managedGpuPass.at( "name" ) == "ScreenProbe.Execute" );
    assert( managedGpuPass.at( "source" ).at( "file" ) == "Packages/com.jngame.render-pipelines/Runtime/ScreenProbe/ScreenProbePass.cs" );
    assert( managedGpuPass.at( "source" ).at( "line" ) == 211 );

    const auto catalogKinds = service.Execute( Request( requestId++, "catalog.kinds", {
        { "trace_id", candidateId }
    } ) ).at( "data" );
    assert( catalogKinds.at( "present" ) == true && catalogKinds.at( "complete" ) == true );
    assert( catalogKinds.at( "definition_count" ) == 1 && catalogKinds.at( "entity_count" ) == 1 );
    const auto catalogDefinition = service.Execute( Request( requestId++, "catalog.get", {
        { "trace_id", candidateId }, { "definition_key", "jn-def:v1:source:03e2e6f19c364e11" }
    } ) ).at( "data" ).at( "definition" );
    assert( catalogDefinition.at( "canonical_name" ) == "Fake.Source" );
    assert( catalogDefinition.at( "source" ).at( "file_id" ) == "engine/runtime/fake.cpp" );
    const auto catalogEntities = service.Execute( Request( requestId++, "catalog.entities", {
        { "trace_id", candidateId }
    } ) ).at( "data" );
    assert( catalogEntities.at( "entities" ).size() == 1 );
    assert( catalogEntities.at( "entities" )[0].at( "connection_generation" ) == 1 );
    const auto catalogQuality = service.Execute( Request( requestId++, "catalog.quality", {
        { "trace_id", candidateId }
    } ) ).at( "data" );
    assert( catalogQuality.at( "quality" ).at( "invalid_count" ) == 0 );

    const auto openReconnectCatalog = service.Execute( Request( requestId++, "trace.open", {
        { "path", files.reconnectCatalog.string() }
    } ) );
    assert( openReconnectCatalog.at( "ok" ) );
    const auto reconnectCatalogId = openReconnectCatalog.at( "data" ).at( "trace_id" ).get<std::string>();
    assert( sessions.WaitReady( reconnectCatalogId, std::chrono::seconds( 5 ) ).state == TraceSourceState::Ready );
    const auto reconnectKinds = service.Execute( Request( requestId++, "catalog.kinds", {
        { "trace_id", reconnectCatalogId }
    } ) ).at( "data" );
    assert( reconnectKinds.at( "complete" ) == true );
    assert( reconnectKinds.at( "active_connection_id" ) == "2" );
    assert( reconnectKinds.at( "connection_ids" ).size() == 1 && reconnectKinds.at( "connection_ids" )[0] == "2" );
    assert( reconnectKinds.at( "definition_count" ) == 1 && reconnectKinds.at( "entity_count" ) == 1 );
    const auto reconnectEntities = service.Execute( Request( requestId++, "catalog.entities", {
        { "trace_id", reconnectCatalogId }
    } ) ).at( "data" );
    assert( reconnectEntities.at( "entities" ).size() == 1 );
    assert( reconnectEntities.at( "entities" )[0].at( "connection_generation" ) == 2 );
    const auto reconnectQuality = service.Execute( Request( requestId++, "catalog.quality", {
        { "trace_id", reconnectCatalogId }
    } ) ).at( "data" );
    assert( reconnectQuality.at( "quality" ).at( "invalid_count" ) == 0 );
    assert( reconnectQuality.at( "records" ).at( "stale_connection" ) == 2 );
    assert( service.Execute( Request( requestId++, "trace.close", { { "trace_id", reconnectCatalogId } } ) ).at( "ok" ) );

    const auto threadFields = service.Execute( Request( requestId++, "thread.get", {
        { "trace_id", candidateId }, { "ref", "fake:thread:1" }
    } ) ).at( "data" );
    assert( threadFields.at( "external_process_name" ) == "FakeProcess" );
    assert( threadFields.at( "external_thread_name" ) == "FakeExternalThread" );
    assert( threadFields.at( "local_name" ) == "FakeLocalThread" );
    assert( threadFields.at( "kernel_sample_count" ) == "3" );
    assert( threadFields.at( "group_hint" ) == -7 );
    assert( threadFields.at( "field_availability" ).at( "group_hint" ).at( "available" ) == true );
    assert( threadFields.at( "running_regions" ) == 4 );

    const auto cpuZoneFields = service.Execute( Request( requestId++, "zone.cpu.get", {
        { "trace_id", candidateId }, { "ref", "fake:cpu-zone:0" }
    } ) ).at( "data" );
    assert( cpuZoneFields.at( "extra_index" ) == 3 );
    assert( cpuZoneFields.at( "extra_name" ) == "Update" );
    assert( cpuZoneFields.at( "extra_text" ) == "phase=simulation" );
    assert( cpuZoneFields.at( "extra_color" ) == 0x112233 );

    const auto gpuContextFields = service.Execute( Request( requestId++, "zone.gpu.contexts", {
        { "trace_id", candidateId }
    } ) ).at( "data" ).at( "contexts" )[0];
    assert( gpuContextFields.at( "type_name" ) == "direct3d12" );
    assert( gpuContextFields.at( "custom_name" ) == "GPU" );
    assert( gpuContextFields.at( "overflow" ) == "9" );
    assert( gpuContextFields.at( "note_names" )[0].at( "time_ns" ) == "21" );
    assert( gpuContextFields.at( "notes" )[0].at( "query_id" ) == 5 );
    assert( gpuContextFields.at( "field_availability" ).at( "notes" ).at( "available" ) == true );

    const auto gpuZoneFields = service.Execute( Request( requestId++, "zone.gpu.get", {
        { "trace_id", candidateId }, { "ref", "fake:gpu-zone:0" }
    } ) ).at( "data" );
    assert( gpuZoneFields.at( "query_id" ) == 5 );
    assert( gpuZoneFields.at( "field_availability" ).at( "query_id" ).at( "available" ) == true );

    const auto contextFields = service.Execute( Request( requestId++, "context_switch.range", {
        { "trace_id", candidateId }
    } ) ).at( "data" ).at( "context_switches" )[0];
    assert( contextFields.at( "reason_name" ) == "wr_mutex" );
    assert( contextFields.at( "state_name" ) == "waiting" );
    assert( contextFields.at( "next_thread_ref" ) == "fake:thread:1" );
    assert( contextFields.at( "field_availability" ).at( "wakeup_cpu" ).at( "available" ) == true );

    const auto memoryPoolFields = service.Execute( Request( requestId++, "memory.pools", {
        { "trace_id", candidateId }
    } ) ).at( "data" ).at( "pools" )[0];
    assert( memoryPoolFields.at( "native_name_id" ) == "1" );
    assert( memoryPoolFields.at( "free_count" ) == "2" );
    assert( memoryPoolFields.at( "persisted_usage_bytes" ) == "64" );
    assert( memoryPoolFields.at( "stored_name_id" ) == "1" );
    assert( memoryPoolFields.at( "stored_name" ) == "GPU D3D12 Fake" );

    const auto lockFields = service.Execute( Request( requestId++, "lock.get", {
        { "trace_id", candidateId }, { "ref", "fake:lock:1" }
    } ) ).at( "data" );
    assert( lockFields.at( "type" ) == 1 );
    assert( lockFields.at( "type_name" ) == "shared_lockable" );
    assert( lockFields.at( "custom_name" ) == "Mutex" );

    const auto plotFields = service.Execute( Request( requestId++, "plot.list", {
        { "trace_id", candidateId }
    } ) ).at( "data" ).at( "plots" )[0];
    assert( plotFields.at( "show_steps" ) == true );
    assert( plotFields.at( "fill" ) == 2 );
    assert( plotFields.at( "color" ) == 0x123456 );

    const auto symbolFields = service.Execute( Request( requestId++, "symbol.get", {
        { "trace_id", candidateId }, { "ref", "fake:symbol:1" }
    } ) ).at( "data" );
    assert( symbolFields.at( "image_name" ) == "fake.dll" );
    assert( symbolFields.at( "call_file" ) == "caller.cpp" );
    assert( symbolFields.at( "call_line" ) == 12 );
    assert( symbolFields.at( "inline" ) == true );

    const auto callstackFields = service.Execute( Request( requestId++, "callstack.frames", {
        { "trace_id", candidateId }, { "callstack", "1" }
    } ) ).at( "data" ).at( "frames" )[0];
    assert( callstackFields.at( "image_name" ) == "fake.dll" );

    const auto sourceLocationFields = service.Execute( Request( requestId++, "source.locations", {
        { "trace_id", candidateId }
    } ) ).at( "data" ).at( "source_locations" )[0];
    assert( sourceLocationFields.at( "native_id" ) == 1 );
    assert( sourceLocationFields.at( "dynamic" ) == false );

    const auto cpuTimelineFields = service.Execute( Request( requestId++, "cpu.timeline", {
        { "trace_id", candidateId }
    } ) ).at( "data" ).at( "segments" )[0];
    assert( cpuTimelineFields.at( "raw_thread_index" ) == 1 );
    assert( cpuTimelineFields.at( "thread_ref" ) == "fake:thread:1" );

    const auto hardwareEventFields = service.Execute( Request( requestId++, "hardware_sample.events", {
        { "trace_id", candidateId }, { "address", "0x1" }, { "kind", "cycles" }
    } ) ).at( "data" ).at( "events" )[0];
    assert( hardwareEventFields.at( "event_index" ) == 0 );
    assert( hardwareEventFields.at( "time_ns" ) == "33" );

    const auto symbolAddressFields = service.Execute( Request( requestId++, "symbol.address", {
        { "trace_id", candidateId }, { "address", "0x1" }
    } ) ).at( "data" );
    assert( symbolAddressFields.at( "address_mapping_ref" ) == "fake:symbol-address:1" );
    assert( symbolAddressFields.at( "inline_mapping" ) == true );

    const auto symbolMapFields = service.Execute( Request( requestId++, "symbol.address_map", {
        { "trace_id", candidateId }
    } ) ).at( "data" ).at( "mappings" )[0];
    assert( symbolMapFields.at( "symbol_ref" ) == "fake:symbol:1" );

    const auto sourceRawFields = service.Execute( Request( requestId++, "source.raw", {
        { "trace_id", candidateId }, { "ref", "fake:source-file:0" }
    } ) ).at( "data" );
    assert( sourceRawFields.at( "data_base64url" ) == "Zm9v" );
    assert( sourceRawFields.at( "eof" ) == false );

    const auto symbolRawFields = service.Execute( Request( requestId++, "symbol.raw_code", {
        { "trace_id", candidateId }, { "ref", "fake:symbol:1" }
    } ) ).at( "data" );
    assert( symbolRawFields.at( "data_base64url" ) == "kA" );

    const auto frameRawFields = service.Execute( Request( requestId++, "frame_image.raw", {
        { "trace_id", candidateId }, { "ref", "fake:frame-image:0" }
    } ) ).at( "data" );
    assert( frameRawFields.at( "data_base64url" ) == "AQIDBAUGBwg" );
    assert( frameRawFields.at( "format" ) == "bc1_dxt1" );

    const auto frameMetadataFields = service.Execute( Request( requestId++, "frame_image.metadata", {
        { "trace_id", candidateId }, { "ref", "fake:frame-image:0" }
    } ) ).at( "data" );
    assert( frameMetadataFields.at( "raw_frame_index" ) == 0 );
    assert( frameMetadataFields.at( "frame_ref" ) == "fake:frame:0" );

    const auto lockTimelineFields = service.Execute( Request( requestId++, "lock.timeline", {
        { "trace_id", candidateId }, { "lock_ref", "fake:lock:1" }
    } ) ).at( "data" ).at( "events" )[0];
    assert( lockTimelineFields.at( "source_location_ref" ) == "fake:source:1" );

    const auto legacyTraceFields = service.Execute( Request( requestId++, "trace.info", {
        { "trace_id", baselineId }
    } ) ).at( "data" );
    assert( legacyTraceFields.at( "legacy_queue_delay_ns" ) == "42" );
    assert( legacyTraceFields.at( "field_availability" ).at( "legacy_queue_delay_ns" ).at( "available" ) == true );

    const auto legacyIdentity = service.Execute( Request( requestId++, "trace.identity", {
        { "trace_id", baselineId }
    } ) ).at( "data" );
    assert( legacyIdentity.at( "present" ) == false );
    assert( legacyIdentity.at( "complete" ) == false );
    assert( legacyIdentity.at( "identity" ).is_null() );
    assert( legacyIdentity.at( "reason" ) == "trace predates or did not emit JN Capture Identity" );

    const auto legacyContext = service.Execute( Request( requestId++, "capture.context", {
        { "trace_id", baselineId }
    } ) ).at( "data" );
    assert( legacyContext.at( "present" ) == false );
    assert( legacyContext.at( "complete" ) == false );
    const auto legacyCoverage = service.Execute( Request( requestId++, "capture.coverage", {
        { "trace_id", baselineId }
    } ) ).at( "data" );
    assert( legacyCoverage.at( "present" ) == false );
    assert( legacyCoverage.at( "complete" ) == false );
    const auto legacyCatalog = service.Execute( Request( requestId++, "catalog.kinds", {
        { "trace_id", baselineId }
    } ) ).at( "data" );
    assert( legacyCatalog.at( "present" ) == false );
    assert( legacyCatalog.at( "complete" ) == false );

    const auto legacyThreadFields = service.Execute( Request( requestId++, "thread.get", {
        { "trace_id", baselineId }, { "ref", "fake:thread:1" }
    } ) ).at( "data" );
    assert( legacyThreadFields.at( "group_hint" ).is_null() );
    assert( legacyThreadFields.at( "field_availability" ).at( "group_hint" ).at( "available" ) == false );

    const auto legacyTopologyFields = service.Execute( Request( requestId++, "cpu.topology", {
        { "trace_id", baselineId }
    } ) ).at( "data" ).at( "logical_cpus" )[0];
    assert( legacyTopologyFields.at( "die" ).is_null() );
    assert( legacyTopologyFields.at( "field_availability" ).at( "die" ).at( "available" ) == false );

    const auto legacyGpuContextFields = service.Execute( Request( requestId++, "zone.gpu.contexts", {
        { "trace_id", baselineId }
    } ) ).at( "data" ).at( "contexts" )[0];
    assert( legacyGpuContextFields.at( "notes" ).empty() );
    assert( legacyGpuContextFields.at( "field_availability" ).at( "notes" ).at( "available" ) == false );

    const auto legacyGpuZoneFields = service.Execute( Request( requestId++, "zone.gpu.get", {
        { "trace_id", baselineId }, { "ref", "fake:gpu-zone:0" }
    } ) ).at( "data" );
    assert( legacyGpuZoneFields.at( "query_id" ).is_null() );
    assert( legacyGpuZoneFields.at( "field_availability" ).at( "query_id" ).at( "available" ) == false );

    const auto legacyContextFields = service.Execute( Request( requestId++, "context_switch.range", {
        { "trace_id", baselineId }
    } ) ).at( "data" ).at( "context_switches" )[0];
    assert( legacyContextFields.at( "wakeup_cpu" ).is_null() );
    assert( legacyContextFields.at( "field_availability" ).at( "wakeup_cpu" ).at( "available" ) == false );

    const auto projected = service.Execute( Request( requestId++, "zone.cpu.search", {
        { "trace_id", candidateId }, { "fields", nlohmann::json::array( { "name" } ) }, { "filter", { { "mode", "prefix" }, { "text", "up" }, { "case_sensitive", false } } }
    } ) );
    assert( projected.at( "ok" ) );
    assert( projected.at( "data" ).at( "zones" ).size() == 1 );
    assert( projected.at( "data" ).at( "zones" )[0].size() == 2 );
    assert( projected.at( "data" ).at( "zones" )[0].contains( "ref" ) && projected.at( "data" ).at( "zones" )[0].contains( "name" ) );

    const auto firstPage = service.Execute( Request( requestId++, "zone.cpu.search", { { "trace_id", candidateId }, { "limit", 1 } } ) );
    assert( firstPage.at( "ok" ) && firstPage.at( "page" ).at( "next_cursor" ).is_string() );
    const auto secondPage = service.Execute( Request( requestId++, "zone.cpu.search", {
        { "trace_id", candidateId }, { "limit", 1 }, { "cursor", firstPage.at( "page" ).at( "next_cursor" ) }
    } ) );
    assert( secondPage.at( "ok" ) && secondPage.at( "page" ).at( "next_cursor" ).is_null() );
    assert( firstPage.at( "data" ).at( "zones" )[0].at( "ref" ) != secondPage.at( "data" ).at( "zones" )[0].at( "ref" ) );

    const auto budgetedFirstPage = service.Execute( Request( requestId++, "zone.cpu.search", {
        { "trace_id", candidateId }, { "limit", 1 }, { "max_scan_events", 1 }
    } ) );
    assert( budgetedFirstPage.at( "ok" ) && budgetedFirstPage.at( "partial" ) == true );
    assert( budgetedFirstPage.at( "omitted_count" ).is_null() );
    assert( budgetedFirstPage.at( "page" ).at( "partial" ) == true );
    assert( budgetedFirstPage.at( "page" ).at( "next_cursor" ).is_string() );
    assert( budgetedFirstPage.at( "budget" ).at( "exhausted_by" ).at( 0 ) == "max_scan_events" );
    const auto budgetedSecondPage = service.Execute( Request( requestId++, "zone.cpu.search", {
        { "trace_id", candidateId }, { "limit", 1 }, { "max_scan_events", 2 },
        { "cursor", budgetedFirstPage.at( "page" ).at( "next_cursor" ) }
    } ) );
    assert( budgetedSecondPage.at( "ok" ) && budgetedSecondPage.at( "partial" ) == false );
    assert( budgetedSecondPage.at( "page" ).at( "next_cursor" ).is_null() );
    assert( budgetedFirstPage.at( "data" ).at( "zones" )[0].at( "ref" ) != budgetedSecondPage.at( "data" ).at( "zones" )[0].at( "ref" ) );
    assert( budgetedFirstPage.at( "data" ).at( "zones" )[0].at( "ref" ) == firstPage.at( "data" ).at( "zones" )[0].at( "ref" ) );
    assert( budgetedSecondPage.at( "data" ).at( "zones" )[0].at( "ref" ) == secondPage.at( "data" ).at( "zones" )[0].at( "ref" ) );

    const auto groupBudget = service.Execute( Request( requestId++, "zone.cpu.flamegraph", {
        { "trace_id", candidateId }, { "max_groups", 1 }, { "max_scan_events", 100 }
    } ) );
    assert( groupBudget.at( "ok" ) && groupBudget.at( "partial" ) == true );
    assert( groupBudget.at( "budget" ).at( "exhausted_by" ).at( 0 ) == "max_groups" );
    assert( groupBudget.at( "data" ).at( "paths" ).size() == 1 );

    const auto nodeBudget = service.Execute( Request( requestId++, "correlation.chain", {
        { "trace_id", candidateId }, { "ref", "fake:frame-identity:281474976710657" }, { "max_nodes", 1 }
    } ) );
    assert( nodeBudget.at( "ok" ) && nodeBudget.at( "partial" ) == true );
    assert( nodeBudget.at( "budget" ).at( "exhausted_by" ).at( 0 ) == "max_nodes" );
    assert( nodeBudget.at( "data" ).at( "truncated" ) == true );

    std::stop_source cancelledSource;
    cancelledSource.request_stop();
    const auto cancelled = service.Execute( Request( requestId++, "zone.cpu.search", { { "trace_id", candidateId } } ), std::nullopt, cancelledSource.get_token() );
    assert( !cancelled.at( "ok" ) && cancelled.at( "error" ).at( "code" ) == "CANCELLED" );

    const auto invalidLimit = service.Execute( Request( requestId++, "frame.list", { { "trace_id", candidateId }, { "limit", 1001 } } ) );
    assert( !invalidLimit.at( "ok" ) && invalidLimit.at( "error" ).at( "code" ) == "INVALID_PARAMS" );
    const auto staleCursor = service.Execute( Request( requestId++, "frame.list", { { "trace_id", candidateId }, { "cursor", "not-a-valid-cursor" } } ) );
    assert( !staleCursor.at( "ok" ) && staleCursor.at( "error" ).at( "code" ) == "STALE_CURSOR" );
    const auto invalidRange = service.Execute( Request( requestId++, "timeline.slice", { { "trace_id", candidateId }, { "start_ns", "10" }, { "end_ns", "10" } } ) );
    assert( !invalidRange.at( "ok" ) && invalidRange.at( "error" ).at( "code" ) == "INVALID_PARAMS" );
    const auto extraField = service.Execute( { { "protocol", "tracy-query/1" }, { "id", requestId++ }, { "method", "trace.list" }, { "extra", true } } );
    assert( !extraField.at( "ok" ) && extraField.at( "error" ).at( "code" ) == "INVALID_REQUEST" );
    std::string invalidUtf8( 1, char( 0xFF ) );
    const auto sanitized = tracy::query::DumpProtocolJson( nlohmann::json { { "text", invalidUtf8 } } );
    assert( sanitized.find( "\xEF\xBF\xBD" ) != std::string::npos );

    assert( service.Execute( Request( requestId++, "trace.close", { { "trace_id", candidateId } } ) ).at( "ok" ) );
    assert( service.Execute( Request( requestId++, "trace.close", { { "trace_id", baselineId } } ) ).at( "ok" ) );

    const auto verifyIdentityTrace = [&]( const std::filesystem::path& path ) {
        const auto opened = service.Execute( Request( requestId++, "trace.open", { { "path", path.string() } } ) );
        assert( opened.at( "ok" ) );
        const auto traceId = opened.at( "data" ).at( "trace_id" ).get<std::string>();
        assert( sessions.WaitReady( traceId, std::chrono::seconds( 5 ) ).state == TraceSourceState::Ready );
        const auto identity = service.Execute( Request( requestId++, "trace.identity", { { "trace_id", traceId } } ) ).at( "data" );
        assert( service.Execute( Request( requestId++, "trace.close", { { "trace_id", traceId } } ) ).at( "ok" ) );
        return identity;
    };

    const auto duplicateIdentity = verifyIdentityTrace( files.duplicateIdentity );
    assert( duplicateIdentity.at( "present" ) == true );
    assert( duplicateIdentity.at( "complete" ) == true );
    assert( duplicateIdentity.at( "records" ).at( "seen" ) == 5 );
    assert( duplicateIdentity.at( "records" ).at( "valid" ) == 4 );
    assert( duplicateIdentity.at( "records" ).at( "duplicates" ) == 1 );
    assert( duplicateIdentity.at( "canonical_fingerprint" ) == captureIdentityFingerprint );

    const auto malformedIdentity = verifyIdentityTrace( files.malformedIdentity );
    assert( malformedIdentity.at( "present" ) == true );
    assert( malformedIdentity.at( "complete" ) == false );
    assert( malformedIdentity.at( "records" ).at( "seen" ) == 4 );
    assert( malformedIdentity.at( "records" ).at( "valid" ) == 1 );
    assert( malformedIdentity.at( "invalid_records" ).size() == 3 );
    assert( malformedIdentity.at( "invalid_records" )[0].at( "reason" ) == "capture identity JSON failed schema or resource-limit validation" );
    assert( malformedIdentity.at( "invalid_records" )[1].at( "reason" ) == "capture identity JSON failed schema or resource-limit validation" );
    assert( malformedIdentity.at( "invalid_records" )[2].at( "reason" ) == "unsupported capture identity envelope version" );

    const auto conflictingIdentity = verifyIdentityTrace( files.conflictingIdentity );
    assert( conflictingIdentity.at( "present" ) == true );
    assert( conflictingIdentity.at( "complete" ) == false );
    assert( conflictingIdentity.at( "records" ).at( "valid" ) == 5 );
    assert( conflictingIdentity.at( "conflicts" ).size() == 1 );
    assert( conflictingIdentity.at( "conflicts" )[0].at( "path" ) == "/protocol/jn_abi_version" );
    assert( conflictingIdentity.at( "identity" ).at( "protocol" ).at( "jn_abi_version" ) == "0x00010000" );

    const auto mismatchedOpen = service.Execute( Request( requestId++, "trace.open", {
        { "path", files.mismatchedConnection.string() }
    } ) );
    assert( mismatchedOpen.at( "ok" ) );
    const auto mismatchedId = mismatchedOpen.at( "data" ).at( "trace_id" ).get<std::string>();
    assert( sessions.WaitReady( mismatchedId, std::chrono::seconds( 5 ) ).state == TraceSourceState::Ready );
    const auto mismatchedContext = service.Execute( Request( requestId++, "capture.context", {
        { "trace_id", mismatchedId }
    } ) ).at( "data" );
    const auto mismatchedCoverage = service.Execute( Request( requestId++, "capture.coverage", {
        { "trace_id", mismatchedId }
    } ) ).at( "data" );
    assert( mismatchedContext.at( "present" ) == true && mismatchedContext.at( "complete" ) == false );
    assert( mismatchedContext.at( "invalid_records" ).size() == 1 );
    assert( mismatchedContext.at( "invalid_records" )[0].at( "reason" ) == "capture context contains multiple connection ids" );
    assert( mismatchedCoverage.at( "present" ) == true && mismatchedCoverage.at( "complete" ) == false );
    assert( mismatchedCoverage.at( "invalid_records" ).size() == 1 );
    assert( mismatchedCoverage.at( "invalid_records" )[0].at( "reason" ) == "producer quality contains multiple connection ids" );
    assert( service.Execute( Request( requestId++, "trace.close", { { "trace_id", mismatchedId } } ) ).at( "ok" ) );

    std::cout << "protocol, statistics, all query methods, fake trace source, memory snapshot, and GTMEM1 contracts passed\n";
    return 0;
}

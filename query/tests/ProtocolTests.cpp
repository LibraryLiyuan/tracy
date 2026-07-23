#include "TracyAnalysis.hpp"
#include "TracyMemoryAnalysis.hpp"
#include "TracyQueryService.hpp"
#include "FakeTraceSource.hpp"

#include <nlohmann/json.hpp>

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
    if( method == "frame.range_mapping" || method == "timeline.slice" ) { params["start_ns"] = "0"; params["end_ns"] = "100"; }
    if( method == "frame_image.metadata" || method == "frame_image.resource" ) params["ref"] = "fake:frame-image:0";
    if( method == "zone.cpu.get" || method == "zone.cpu.tree" ) params["ref"] = "fake:cpu-zone:0";
    if( method == "zone.gpu.get" || method == "zone.gpu.tree" ) params["ref"] = "fake:gpu-zone:0";
    if( method == "memory.get" ) params["ref"] = "fake:memory-event:0";
    if( method == "memory.active_at_time" ) params["time_ns"] = "50";
    if( method == "memory.frame_snapshot" ) params["frame_index"] = 0;
    if( method == "memory.diff" ) { params["base_frame_index"] = 0; params["target_frame_index"] = 0; }
    if( method == "lock.get" ) params["ref"] = "fake:lock:1";
    if( method == "lock.timeline" ) params["lock_ref"] = "fake:lock:1";
    if( method == "plot.points" || method == "plot.range" || method == "plot.downsample" || method == "plot.statistics" ) params["plot_ref"] = "fake:plot:0";
    if( method == "message.get" ) params["ref"] = "fake:message:0";
    if( method == "callstack.resolve" || method == "callstack.batch" ) params["callstacks"] = json::array( { "1" } );
    if( method == "callstack.frames" || method == "callstack.parent" ) params["callstack"] = "1";
    if( method == "symbol.get" || method == "symbol.raw_code" || method == "symbol.disassembly" ) params["ref"] = "fake:symbol:1";
    if( method == "symbol.address" || method == "hardware_sample.address" ) params["address"] = "0x1";
    if( method == "source.lines" ) params["ref"] = "fake:source-file:0";
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
        outsideRoot = std::filesystem::temp_directory_path() / ( "tracy-query-outside-" + suffix );
        std::filesystem::create_directories( outsideRoot );
        outside = outsideRoot / "outside.tracy";
        std::ofstream( baseline, std::ios::binary ).put( '\0' );
        std::ofstream( candidate, std::ios::binary ).put( '\0' );
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
    std::filesystem::path outsideRoot;
    std::filesystem::path outside;
};

int main()
{
    const auto schema = LoadJson( TRACY_QUERY_SCHEMA_PATH );
    assert( schema.at( "$defs" ).at( "request" ).at( "properties" ).at( "protocol" ).at( "const" ) == "tracy-query/1" );
    assert( schema.at( "$defs" ).at( "success" ).at( "properties" ).at( "schema_version" ).at( "const" ) == "1.0.0" );
    assert( schema.at( "$defs" ).at( "errorCode" ).at( "enum" ).size() == 19 );

    const auto coverage = LoadJson( TRACY_QUERY_COVERAGE_PATH );
    assert( coverage.at( "domains" ).size() == 23 );
    for( const auto& domain : coverage.at( "domains" ) )
    {
        assert( domain.at( "domain" ).is_string() );
        assert( !domain.at( "methods" ).empty() );
        assert( !domain.at( "worker_data" ).empty() );
        assert( domain.at( "status" ) == "complete" );
        assert( !domain.at( "tests" ).empty() );
    }

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
        { 0, GpuMemoryRequestMarker, "Upload request", "GTMEM1|SCOPE|label=7|frame=3", 9, 0, 100 },
        { 1, GpuMemoryPassMarker, "RenderPass", "GTMEM1|PASS|pass=11|label=7|frame=3|level=1|ordinal=2|ops=draw|commands=4|uses=1|total=1|chunks=1|untracked=0|truncated=0|dropped=0\nGTMEM1|USE|pass=11|data=42:T:3", 9, 20, 80 }
    };
    const std::vector<GpuMemoryGpuZoneInput> gpuMarkers = { { 0, "RenderPass", 9, 30, 1000, 2000 } };
    const std::vector<GpuMemoryAllocationInput> gpuAllocations = { { { 5, 0 }, 42, 4096, 9, 50 } };
    const auto attribution = BuildGpuMemoryAttribution( cpuMarkers, gpuMarkers, gpuAllocations );
    assert( attribution.protocolPresent && attribution.complete );
    assert( attribution.requestScopes.size() == 1 && attribution.passes.size() == 1 && attribution.allocations.size() == 1 );
    assert( attribution.passes[0].complete && attribution.passes[0].gpuPairing == GpuZonePairing::Exact );
    assert( attribution.passes[0].uses.size() == 1 && attribution.passes[0].uses[0].allocationId == 42 );
    assert( attribution.allocations[0].requestLabelId == 7 && attribution.allocations[0].passIndices == std::vector<size_t> { 0 } );
    assert( FormatGpuMemoryUsage( 3 ) == "Read/Write" );

    tracy::query::test::FakeTraceSource fake;
    assert( fake.AcquireReadView().sourceKind == TraceSourceKind::Snapshot );
    assert( fake.AcquireReadView().complete );
    assert( fake.GetCapabilities().size() >= 20 );
    assert( fake.GetTraceInfo().fingerprint == std::string( 64, 'f' ) );
    assert( fake.GetThreads().size() == 1 && fake.GetFrameSets().size() == 1 && fake.GetGpuContexts().size() == 1 );
    assert( fake.GetMemoryPools().size() == 1 && fake.GetPlotList().size() == 1 && fake.GetLocks().size() == 1 );
    ScanRange entire;
    assert( fake.ScanCpuZones( entire ).size() == 2 && fake.ScanGpuZones( entire ).size() == 1 && fake.ScanFrames( entire ).size() == 1 );
    assert( fake.ScanMemoryEvents( entire ).size() == 1 && fake.ScanMessages( entire ).size() == 1 && fake.ScanPlots( entire ).size() == 1 );
    assert( fake.ScanContextSwitchEvents( entire ).size() == 1 && fake.ScanSampleEvents( entire ).size() == 1 && fake.ScanGhostZones( entire ).size() == 1 );
    assert( fake.ScanLockEvents( entire ).size() == 1 && fake.GetHardwareSamples().size() == 1 && fake.GetSymbols().size() == 1 && fake.GetSourceLocations().size() == 1 );
    assert( fake.ResolveCallstacks( { 1 }, 1 ).size() == 1 && fake.ResolveParentCallstacks( { 1 }, 1 ).size() == 1 );
    assert( fake.GetSourceResources().size() == 1 && fake.GetSymbolResources().size() == 1 && fake.GetFrameImageResources().size() == 1 );
    assert( fake.GetMemoryFrameSnapshot( 0, 0, {}, false ).valid );
    assert( fake.GetMemoryEvent( { 1, 0 } ).has_value() && fake.GetGpuMemoryAttribution().allocations.size() == 1 );
    assert( fake.ReadEmbeddedSource( 0, 64 ).embedded && fake.ReadSymbolCode( 1, 64 ).bytes.size() == 1 && fake.ReadFrameImage( 0, 64 ).rgba.size() == 4 );
    assert( fake.ParseEntityRef( fake.MakeEntityRef( "cpu-zone", 7 ), "cpu-zone" ) == 7 );
    ScanRange before; before.endNs = 10; assert( fake.ScanCpuZones( before ).empty() );
    ScanRange after; after.startNs = 60; assert( fake.ScanCpuZones( after ).empty() );
    ScanRange paged; paged.offset = 2; assert( fake.ScanCpuZones( paged ).empty() );

    TemporaryTraceFiles files;
    tracy::query::SessionManager sessions( { files.root }, 2,
        []( const std::filesystem::path&, tracy::query::SessionManager::StateCallback callback ) -> std::unique_ptr<tracy::analysis::TraceSource> {
            callback( tracy::analysis::TraceSourceState::Indexing );
            return std::make_unique<tracy::query::test::FakeTraceSource>();
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

    const auto described = service.Execute( Request( 102, "system.describe" ) );
    assert( described.at( "ok" ) );
    std::set<std::string> describedMethods;
    for( const auto& method : described.at( "data" ).at( "methods" ) ) describedMethods.emplace( method.get<std::string>() );
    std::set<std::string> coveredMethods;
    for( const auto& domain : coverage.at( "domains" ) ) for( const auto& method : domain.at( "methods" ) ) coveredMethods.emplace( method.get<std::string>() );
    assert( describedMethods == coveredMethods );

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

    std::cout << "protocol, statistics, all query methods, fake trace source, memory snapshot, and GTMEM1 contracts passed\n";
    return 0;
}

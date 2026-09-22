#include "TracyEmbeddedData.hpp"
#include "TracyMcpServer.hpp"
#include "TracyQueryIndex.hpp"
#include "TracyQueryService.hpp"
#include "TracySessionManager.hpp"
#include "TracySegmentTraceSource.hpp"
#include "TracyWorkerTraceSource.hpp"
#include "TracyProcessCommitLimit.hpp"

#include "../../public/common/TracyVersion.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#endif

namespace
{

using nlohmann::json;
using namespace tracy::query;

struct Arguments
{
    bool version = false;
    bool schema = false;
    bool doctor = false;
    bool mcp = false;
    bool compactIndex = false;
    bool buildIndex = false;
    bool indexed = false;
    bool indexedExplicit = false;
    std::optional<std::filesystem::path> trace;
    std::optional<std::string> request;
    std::optional<std::string> batch;
    std::optional<std::filesystem::path> output;
    std::vector<std::filesystem::path> allowRoots;
    std::vector<std::filesystem::path> allowSourceRoots;
    std::optional<std::filesystem::path> analysisRoot;
    std::optional<std::filesystem::path> analysisCacheRoot;
    std::optional<std::filesystem::path> resourceProfile;
    QueryResourceContextOptions resourceOptions;
    size_t analysisCacheMiB = 512;
};

QueryResourceContextOptions LoadResourceProfile(const std::filesystem::path& path)
{
    if(!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path)>64*1024)
        throw std::runtime_error("--resource-profile must name a JSON file no larger than 64 KiB");
    std::ifstream input(path,std::ios::binary);
    const auto profile=json::parse(input);
    if(profile.at("schema")!=1 || !profile.value("working_set_is_observation_only",false) ||
        profile.value("heavy_build_concurrency",0u)!=1)
        throw std::runtime_error("unsupported Query resource profile");
    QueryResourceContextOptions options;
    options.expectedTraceBytes=profile.at("trace_bytes").get<uint64_t>();
    options.expectedTraceSha256=profile.at("trace_sha256").get<std::string>();
    std::transform(options.expectedTraceSha256.begin(),options.expectedTraceSha256.end(),
        options.expectedTraceSha256.begin(),[](unsigned char value){return char(std::tolower(value));});
    options.expectedFinalDiskLimit=profile.at("final_disk_limit").get<uint64_t>();
    options.expectedPeakDiskLimit=profile.at("peak_live_disk_limit").get<uint64_t>();
    options.privateBytesSoftTarget=profile.at("private_bytes_soft_target").get<uint64_t>();
    options.processCommitHardLimit=profile.at("process_commit_hard_limit").get<uint64_t>();
    options.sampleInterval=std::chrono::milliseconds(profile.at("sample_period_ms").get<uint64_t>());
    if(profile.contains("workspace_maximum_bytes")) options.workspaceMaximumBytes=profile.at("workspace_maximum_bytes").get<uint64_t>();
    if(profile.contains("workspace_target_bytes")) options.workspaceTargetBytes=profile.at("workspace_target_bytes").get<uint64_t>();
    if(profile.contains("diagnostic_memory_reserve_bytes")) options.diagnosticMemoryReserveBytes=profile.at("diagnostic_memory_reserve_bytes").get<uint64_t>();
    if(profile.contains("diagnostic_disk_reserve_bytes")) options.diagnosticDiskReserveBytes=profile.at("diagnostic_disk_reserve_bytes").get<uint64_t>();
    return options;
}

void Usage()
{
    std::cerr
        << "tracy-query " << tracy::Version::Major << '.' << tracy::Version::Minor << '.' << tracy::Version::Patch << "\n"
        << "Usage:\n"
        << "  tracy-query --version\n"
        << "  tracy-query --schema\n"
        << "  tracy-query --doctor [--trace file.tracy] [--compact-index] [--allow-root path]\n"
        << "  tracy-query --build-index --trace file.tracy [--resource-profile file.json] [--allow-root path] [--output file.json]\n"
        << "  tracy-query --trace file.tracy --request request.json|- [--indexed] [--allow-root path] [--output file.json]\n"
        << "  tracy-query --trace file.tracy --batch requests.ndjson|- [--indexed] [--allow-root path] [--output file.ndjson]\n"
        << "  tracy-query --mcp [--indexed|--no-indexed] [--resource-profile file.json] [--allow-root path] [--allow-source-root path] [--analysis-root path --analysis-cache-root path] [--analysis-cache-mib 512]\n";
}

Arguments ParseArguments( int argc, char** argv )
{
    Arguments result;
    for( int i = 1; i < argc; i++ )
    {
        const std::string option = argv[i];
        const auto value = [&]( const char* name ) -> std::string {
            if( i + 1 >= argc ) throw std::runtime_error( std::string( name ) + " requires a value" );
            return argv[++i];
        };
        if( option == "--version" ) result.version = true;
        else if( option == "--schema" ) result.schema = true;
        else if( option == "--doctor" ) result.doctor = true;
        else if( option == "--mcp" ) result.mcp = true;
        else if( option == "--compact-index" ) result.compactIndex = true;
        else if( option == "--build-index" ) result.buildIndex = true;
        else if( option == "--indexed" ) { result.indexed = true; result.indexedExplicit = true; }
        else if( option == "--no-indexed" ) { result.indexed = false; result.indexedExplicit = true; }
        else if( option == "--trace" ) result.trace = value( "--trace" );
        else if( option == "--request" ) result.request = value( "--request" );
        else if( option == "--batch" ) result.batch = value( "--batch" );
        else if( option == "--output" ) result.output = value( "--output" );
        else if( option == "--allow-root" ) result.allowRoots.emplace_back( value( "--allow-root" ) );
        else if( option == "--allow-source-root" ) result.allowSourceRoots.emplace_back( value( "--allow-source-root" ) );
        else if( option == "--analysis-root" ) result.analysisRoot = value( "--analysis-root" );
        else if( option == "--analysis-cache-root" ) result.analysisCacheRoot = value( "--analysis-cache-root" );
        else if( option == "--resource-profile" ) result.resourceProfile = value( "--resource-profile" );
        else if( option == "--analysis-cache-mib" )
        {
            const auto parsed = std::stoull( value( "--analysis-cache-mib" ) );
            if( parsed > 65536 ) throw std::runtime_error( "--analysis-cache-mib must be between 0 and 65536" );
            result.analysisCacheMiB = size_t( parsed );
        }
        else if( option == "--help" || option == "-h" )
        {
            Usage();
            std::exit( 0 );
        }
        else throw std::runtime_error( "unknown option: " + option );
    }
    // MCP is the formal AI-analysis entry point. Default it to the bounded,
    // memory-mapped sidecar path while retaining an explicit diagnostic escape
    // hatch for investigations that require the full in-memory Worker.
    if( result.mcp && !result.indexedExplicit ) result.indexed = true;
    if( result.analysisRoot.has_value() != result.analysisCacheRoot.has_value() )
    {
        throw std::runtime_error( "--analysis-root and --analysis-cache-root must be provided together" );
    }
    if( result.analysisRoot )
    {
        if( !result.analysisRoot->is_absolute() || !result.analysisCacheRoot->is_absolute() )
        {
            throw std::runtime_error( "--analysis-root and --analysis-cache-root must be absolute paths" );
        }
        *result.analysisRoot = result.analysisRoot->lexically_normal();
        *result.analysisCacheRoot = result.analysisCacheRoot->lexically_normal();
    }
    if(result.resourceProfile)
    {
        *result.resourceProfile=std::filesystem::absolute(*result.resourceProfile).lexically_normal();
        result.resourceOptions=LoadResourceProfile(*result.resourceProfile);
    }
    return result;
}

std::string ReadBounded( std::istream& stream )
{
    std::string result;
    std::array<char, 64 * 1024> buffer {};
    while( stream )
    {
        stream.read( buffer.data(), buffer.size() );
        const auto count = stream.gcount();
        if( count > 0 ) result.append( buffer.data(), size_t( count ) );
        if( result.size() > MaximumRequestBytes ) throw QueryError( "RESOURCE_LIMIT", "request exceeds the 1 MiB budget" );
    }
    return result;
}

std::string ReadRequest( const std::string& path )
{
    if( path == "-" ) return ReadBounded( std::cin );
    std::ifstream stream( path, std::ios::binary );
    if( !stream ) throw QueryError( "INVALID_REQUEST", "unable to open request file" );
    return ReadBounded( stream );
}

json ParseRequest( const std::string& text, QueryService& service )
{
    try
    {
        return json::parse( text );
    }
    catch( const json::parse_error& error )
    {
        return service.Failure( nullptr, "INVALID_REQUEST", std::string( "invalid JSON: " ) + error.what() );
    }
}

std::optional<std::string> OpenDefaultTrace( SessionManager& sessions, const std::optional<std::filesystem::path>& path, json& failure, QueryService& service )
{
    if( !path ) return std::nullopt;
    try
    {
        const auto opened = sessions.Open( *path );
        const auto ready = sessions.WaitReady( opened.id, std::chrono::hours( 24 ) );
        if( ready.state == tracy::analysis::TraceSourceState::Ready ) return ready.id;
        failure = service.Failure( nullptr, ready.errorCode.empty() ? "TRACE_OPEN_FAILED" : ready.errorCode, ready.errorMessage.empty() ? "trace did not become ready" : ready.errorMessage );
        return std::nullopt;
    }
    catch( const SessionError& error )
    {
        failure = service.Failure( nullptr, ToString( error.code ), error.what(), error.retryable );
        return std::nullopt;
    }
}

int RunSingle( const Arguments& args )
{
    SessionManager sessions( args.allowRoots, 2, {}, args.indexed, args.resourceOptions );
    QueryService service( sessions, args.analysisCacheMiB * 1024 * 1024,
        args.analysisRoot.value_or( std::filesystem::path {} ), args.analysisCacheRoot.value_or( std::filesystem::path {} ) );
    json failure;
    const auto trace = OpenDefaultTrace( sessions, args.trace, failure, service );
    if( args.trace && !trace )
    {
        std::cout << DumpProtocolJson( failure ) << '\n';
        return 3;
    }
    const auto parsed = ParseRequest( ReadRequest( *args.request ), service );
    const auto response = parsed.contains( "ok" ) && parsed.value( "ok", true ) == false ? parsed : service.Execute( parsed, trace );
    std::cout << DumpProtocolJson( response ) << '\n';
    std::cout.flush();
    return response.value( "ok", false ) ? 0 : 2;
}

int RunBatch( const Arguments& args )
{
    SessionManager sessions( args.allowRoots, 2, {}, args.indexed, args.resourceOptions );
    QueryService service( sessions, args.analysisCacheMiB * 1024 * 1024,
        args.analysisRoot.value_or( std::filesystem::path {} ), args.analysisCacheRoot.value_or( std::filesystem::path {} ) );
    json failure;
    const auto trace = OpenDefaultTrace( sessions, args.trace, failure, service );
    if( args.trace && !trace )
    {
        std::cout << DumpProtocolJson( failure ) << '\n';
        return 3;
    }

    std::ifstream file;
    std::istream* input = &std::cin;
    if( *args.batch != "-" )
    {
        file.open( *args.batch, std::ios::binary );
        if( !file ) throw QueryError( "INVALID_REQUEST", "unable to open batch file" );
        input = &file;
    }
    std::string line;
    while( std::getline( *input, line ) )
    {
        if( line.empty() ) continue;
        if( line.size() > MaximumRequestBytes )
        {
            std::cout << DumpProtocolJson( service.Failure( nullptr, "RESOURCE_LIMIT", "request exceeds the 1 MiB budget" ) ) << '\n';
            continue;
        }
        const auto parsed = ParseRequest( line, service );
        const auto response = parsed.contains( "ok" ) && parsed.value( "ok", true ) == false ? parsed : service.Execute( parsed, trace );
        std::cout << DumpProtocolJson( response ) << '\n';
        std::cout.flush();
    }
    return 0;
}

int RunDoctor( const Arguments& args )
{
    json result = {
        { "program", "tracy-query" },
        { "version", std::to_string( tracy::Version::Major ) + '.' + std::to_string( tracy::Version::Minor ) + '.' + std::to_string( tracy::Version::Patch ) },
        { "protocol", QueryProtocol }, { "schema_version", QuerySchemaVersion },
        { "analysis_cache_bytes", std::to_string( args.analysisCacheMiB * 1024 * 1024 ) },
        { "checks", {
            { "json_schema", json::parse( QuerySchemaJson ).is_object() ? "ok" : "failed" },
            { "coverage_manifest", json::parse( QueryCoverageJson ).is_object() ? "ok" : "failed" },
            { "domain_coverage_manifest", json::parse( QueryCoverageJson ).is_object() ? "ok" : "failed" },
            { "field_coverage_manifest", json::parse( QueryFieldCoverageJson ).is_object() ? "ok" : "failed" },
            { "mcp_coverage_manifest", json::parse( QueryMcpCoverageJson ).is_object() ? "ok" : "failed" },
            { "analysis_profile_schema", json::parse( AnalysisProfileSchemaJson ).is_object() ? "ok" : "failed" },
            { "neutral_aggregate_schema", json::parse( NeutralAggregateSchemaJson ).is_object() ? "ok" : "failed" },
            { "candidate_manifest_schema", json::parse( CandidateManifestSchemaJson ).is_object() ? "ok" : "failed" },
            { "analysis_scan_api_schema", json::parse( AnalysisScanApiSchemaJson ).is_object() ? "ok" : "failed" },
            { "stdio_framing", "single-line UTF-8 JSON/NDJSON" }, { "statistics", "enabled" }
        } }
    };
    try
    {
        SessionManager sessions( args.allowRoots, 2, {}, args.indexed, args.resourceOptions );
        json roots = json::array();
        for( const auto& root : sessions.AllowRoots() ) roots.emplace_back( root.string() );
        result["allow_roots"] = std::move( roots );
        if( args.trace )
        {
            if( args.compactIndex )
            {
                const auto path = sessions.ResolveTracePath( *args.trace );
                auto resources=QueryIndex::CreateResourceContext(path,args.resourceOptions);
                resources->SetPhase("doctor:compact-load"); resources->SetLiveWorkers(1);
                const auto source = tracy::analysis::WorkerTraceSource::Open( path, {}, {},
                    tracy::analysis::WorkerTraceLoadMode::CompactIndex,nullptr,resources->CreateFileReadHooks() );
                const auto info = source->GetTraceInfo();
                resources->ValidateTraceIdentity(std::filesystem::file_size(path),info.fingerprint);
                result["checks"]["trace_loader"] = "ok";
                result["checks"]["load_mode"] = "compact_index";
                result["trace"] = {
                    { "state", "ready" }, { "fingerprint", info.fingerprint },
                    { "counts", {
                        { "frames", std::to_string( info.counts.frames ) },
                        { "cpu_zones_declared", std::to_string( info.counts.cpuZones ) },
                        { "gpu_zones_declared", std::to_string( info.counts.gpuZones ) },
                        { "jobs", std::to_string( info.counts.jobs ) },
                        { "io_requests", std::to_string( info.counts.ioRequests ) },
                        { "relations", std::to_string( info.counts.relations ) }
                    } }
                };
            }
            else
            {
                const auto opened = sessions.Open( *args.trace );
                const auto ready = sessions.WaitReady( opened.id, std::chrono::hours( 24 ) );
                result["checks"]["trace_loader"] = ready.state == tracy::analysis::TraceSourceState::Ready ? "ok" : "failed";
                result["trace"] = {
                    { "id", ready.id }, { "state", tracy::analysis::ToString( ready.state ) }, { "fingerprint", ready.fingerprint },
                    { "error_code", ready.errorCode.empty() ? json( nullptr ) : json( ready.errorCode ) },
                    { "error_message", ready.errorMessage.empty() ? json( nullptr ) : json( ready.errorMessage ) }
                };
            }
        }
        else result["checks"]["trace_loader"] = "not_requested";
    }
    catch( const std::exception& error )
    {
        result["checks"]["allow_roots"] = "failed";
        result["error"] = error.what();
    }
    std::cout << DumpProtocolJson( result ) << '\n';
    return result.contains( "error" ) || result["checks"].value( "trace_loader", "ok" ) == "failed" ? 2 : 0;
}

int RunBuildIndex( const Arguments& args )
{
    SessionManager sessions( args.allowRoots );
    const auto path = sessions.ResolveTracePath( *args.trace );
    const auto started = std::chrono::steady_clock::now();
    auto extension = path.extension().string();
    std::transform( extension.begin(), extension.end(), extension.begin(), []( unsigned char value ) { return char( std::tolower( value ) ); } );
    const bool streamInput = extension == ".tracy-stream";
    std::filesystem::path indexedPath = path;
    uint64_t streamRevision = 0;
    std::unique_ptr<SegmentTraceSource> streamSource;
    QueryIndexManifest index;
    QueryIndexValidation validation;
    std::shared_ptr<QueryResourceContext> resources;
    if( streamInput )
    {
        streamSource = SegmentTraceSource::Open( path, {}, true );
        indexedPath = streamSource->SnapshotPath();
        const auto view = streamSource->RefreshView();
        if( view ) streamRevision = view->revision;
        resources=QueryIndex::CreateResourceContext(indexedPath,args.resourceOptions);
        validation = QueryIndex::Validate( indexedPath, true, resources );
        if( !validation.manifest ) throw std::runtime_error( "stream query index validation failed: " + validation.reason );
        index = *validation.manifest;
    }
    else
    {
        resources=QueryIndex::CreateResourceContext(indexedPath,args.resourceOptions);
        index = QueryIndex::Build( indexedPath, {}, resources );
        validation = QueryIndex::Validate( indexedPath, true, resources );
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - started ).count();
    const auto resourceSnapshot = resources->Snapshot();
    const json result = {
        { "program", "tracy-query" }, { "protocol", QueryProtocol }, { "schema_version", QuerySchemaVersion },
        { "operation", "build-index" }, { "ok", validation.manifest.has_value() },
        { "elapsed_ms", std::to_string( elapsed ) },
        { "resources", {
            { "disk_current_bytes", std::to_string( resourceSnapshot.disk.currentBytes ) },
            { "disk_peak_bytes", std::to_string( resourceSnapshot.disk.peakBytes ) },
            { "disk_final_limit_bytes", std::to_string( resourceSnapshot.disk.finalLimitBytes ) },
            { "disk_peak_limit_bytes", std::to_string( resourceSnapshot.disk.peakLimitBytes ) },
            { "workspace_peak_bytes", std::to_string( resourceSnapshot.workspace.peakBytes ) }
        } },
        { "input", {
            { "kind", streamInput ? "stream" : "snapshot" }, { "path", path.string() },
            { "indexed_snapshot", indexedPath.string() }, { "stream_revision", std::to_string( streamRevision ) }
        } },
        { "index", {
            { "schema_version", QueryIndexSchemaVersion }, { "manifest", index.manifestPath.string() },
            { "data", index.dataPath.string() }, { "source_sha256", index.sourceFingerprint },
            { "source_bytes", std::to_string( index.sourceBytes ) }, { "data_sha256", index.dataFingerprint },
            { "data_bytes", std::to_string( index.dataBytes ) }, { "validation", validation.reason },
            { "cpu_zone_index", index.cpuZoneIndex }, { "gpu_zone_index", index.gpuZoneIndex },
            { "zone_sections", {
                { "extras", { { "count", std::to_string( index.zoneExtras.count ) }, { "declared_count", std::to_string( index.zoneExtras.declaredCount ) }, { "bytes", std::to_string( index.zoneExtras.bytes ) }, { "sha256", index.zoneExtras.fingerprint } } },
                { "cpu", { { "count", std::to_string( index.cpuZones.count ) }, { "declared_count", std::to_string( index.cpuZones.declaredCount ) }, { "bytes", std::to_string( index.cpuZones.bytes ) }, { "sha256", index.cpuZones.fingerprint } } },
                { "gpu", { { "count", std::to_string( index.gpuZones.count ) }, { "declared_count", std::to_string( index.gpuZones.declaredCount ) }, { "bytes", std::to_string( index.gpuZones.bytes ) }, { "sha256", index.gpuZones.fingerprint } } }
            } }
        } }
    };
    std::cout << DumpProtocolJson( result ) << '\n';
    return validation.manifest ? 0 : 2;
}

}

int main( int argc, char** argv )
{
#ifdef _WIN32
    SetErrorMode( SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX );
#endif
    try
    {
        const auto args = ParseArguments( argc, argv );
        if( args.output && args.mcp ) throw std::runtime_error( "--output is not valid with --mcp" );
        std::ofstream output;
        if( args.output )
        {
            output.open( *args.output, std::ios::binary | std::ios::trunc );
            if( !output ) throw std::runtime_error( "unable to open --output file" );
            std::cout.rdbuf( output.rdbuf() );
        }
        if( args.compactIndex && ( !args.doctor || !args.trace ) ) throw std::runtime_error( "--compact-index requires --doctor and --trace" );
        if( args.buildIndex && !args.trace ) throw std::runtime_error( "--build-index requires --trace" );
        const int modes = int( args.version ) + int( args.schema ) + int( args.doctor ) + int( args.buildIndex ) + int( args.mcp ) + int( args.request.has_value() ) + int( args.batch.has_value() );
        if( modes != 1 )
        {
            Usage();
            return 1;
        }
        if( args.version )
        {
            std::cout << "tracy-query " << tracy::Version::Major << '.' << tracy::Version::Minor << '.' << tracy::Version::Patch << " protocol " << QueryProtocol << " schema " << QuerySchemaVersion << " trace-format JNTRACYX/1 string-index 32\n";
            return 0;
        }
        if( args.schema )
        {
            std::cout << QuerySchemaJson << '\n';
            return 0;
        }
#ifdef _WIN32
        // Installed before any input is opened, covering Worker, indexing,
        // scans, caches and direct/MCP query allocations in this process.
        tracy::analysis::ProcessCommitLimit commitLimit(32ull*1024*1024*1024);
#endif
        if( args.doctor ) return RunDoctor( args );
        if( args.buildIndex ) return RunBuildIndex( args );
        if( args.request ) return RunSingle( args );
        if( args.batch ) return RunBatch( args );
        if( args.mcp )
        {
            SessionManager sessions( args.allowRoots, 2, {}, args.indexed, args.resourceOptions );
            QueryService service( sessions, args.analysisCacheMiB * 1024 * 1024,
                args.analysisRoot.value_or( std::filesystem::path {} ), args.analysisCacheRoot.value_or( std::filesystem::path {} ) );
            McpServer server( sessions, service, args.allowSourceRoots );
            return server.Run( std::cin, std::cout );
        }
        return 1;
    }
    catch( const QueryError& error )
    {
        std::cerr << error.code << ": " << error.what() << '\n';
        return 2;
    }
    catch( const std::exception& error )
    {
        std::cerr << "INTERNAL_ERROR: " << error.what() << '\n';
        return 3;
    }
}

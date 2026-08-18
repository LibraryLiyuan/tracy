#include "TracyEmbeddedData.hpp"
#include "TracyMcpServer.hpp"
#include "TracyQueryIndex.hpp"
#include "TracyQueryService.hpp"
#include "TracySessionManager.hpp"
#include "TracyWorkerTraceSource.hpp"

#include "../../public/common/TracyVersion.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
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

void ApplyIndexedMemoryBudget()
{
#ifdef _WIN32
    constexpr SIZE_T MinimumWorkingSet = SIZE_T( 64 ) * 1024 * 1024;
    constexpr SIZE_T MaximumWorkingSet = SIZE_T( 1900 ) * 1024 * 1024;
    if( !SetProcessWorkingSetSizeEx( GetCurrentProcess(), MinimumWorkingSet, MaximumWorkingSet, QUOTA_LIMITS_HARDWS_MAX_ENABLE ) )
    {
        // PROCESS_SET_QUOTA is not available to a normal desktop token on all
        // Windows installations.  Indexed Query must remain usable without an
        // administrator prompt; its internal cache, page and response budgets
        // continue to provide the portable memory bound.  The OS hard working
        // set is therefore an additional best-effort guard, not a start-up
        // requirement.
        std::cerr << "warning: could not apply the optional 1.9 GiB indexed working-set guard (Windows error "
            << GetLastError() << "); continuing with internal query budgets\n";
    }
#endif
}

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
    std::vector<std::filesystem::path> allowRoots;
    std::vector<std::filesystem::path> allowSourceRoots;
    size_t analysisCacheMiB = 512;
};

void Usage()
{
    std::cerr
        << "tracy-query " << tracy::Version::Major << '.' << tracy::Version::Minor << '.' << tracy::Version::Patch << "\n"
        << "Usage:\n"
        << "  tracy-query --version\n"
        << "  tracy-query --schema\n"
        << "  tracy-query --doctor [--trace file.tracy] [--compact-index] [--allow-root path]\n"
        << "  tracy-query --build-index --trace file.tracy [--allow-root path]\n"
        << "  tracy-query --trace file.tracy --request request.json|- [--indexed] [--allow-root path]\n"
        << "  tracy-query --trace file.tracy --batch requests.ndjson|- [--indexed] [--allow-root path]\n"
        << "  tracy-query --mcp [--indexed|--no-indexed] [--allow-root path] [--allow-source-root path] [--analysis-cache-mib 512]\n";
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
        else if( option == "--allow-root" ) result.allowRoots.emplace_back( value( "--allow-root" ) );
        else if( option == "--allow-source-root" ) result.allowSourceRoots.emplace_back( value( "--allow-source-root" ) );
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
    SessionManager sessions( args.allowRoots, 2, {}, args.indexed );
    QueryService service( sessions, args.analysisCacheMiB * 1024 * 1024 );
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
    SessionManager sessions( args.allowRoots, 2, {}, args.indexed );
    QueryService service( sessions, args.analysisCacheMiB * 1024 * 1024 );
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
            { "stdio_framing", "single-line UTF-8 JSON/NDJSON" }, { "statistics", "enabled" }
        } }
    };
    try
    {
        SessionManager sessions( args.allowRoots, 2, {}, args.indexed );
        json roots = json::array();
        for( const auto& root : sessions.AllowRoots() ) roots.emplace_back( root.string() );
        result["allow_roots"] = std::move( roots );
        if( args.trace )
        {
            if( args.compactIndex )
            {
                const auto path = sessions.ResolveTracePath( *args.trace );
                const auto source = tracy::analysis::WorkerTraceSource::Open( path, {}, {}, tracy::analysis::WorkerTraceLoadMode::CompactIndex );
                const auto info = source->GetTraceInfo();
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
    const auto index = QueryIndex::Build( path );
    const auto validation = QueryIndex::Validate( path, true );
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - started ).count();
    const json result = {
        { "program", "tracy-query" }, { "protocol", QueryProtocol }, { "schema_version", QuerySchemaVersion },
        { "operation", "build-index" }, { "ok", validation.manifest.has_value() },
        { "elapsed_ms", std::to_string( elapsed ) },
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
        if( args.indexed || args.buildIndex ) ApplyIndexedMemoryBudget();
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
            std::cout << "tracy-query " << tracy::Version::Major << '.' << tracy::Version::Minor << '.' << tracy::Version::Patch << " protocol " << QueryProtocol << " schema " << QuerySchemaVersion << '\n';
            return 0;
        }
        if( args.schema )
        {
            std::cout << QuerySchemaJson << '\n';
            return 0;
        }
        if( args.doctor ) return RunDoctor( args );
        if( args.buildIndex ) return RunBuildIndex( args );
        if( args.request ) return RunSingle( args );
        if( args.batch ) return RunBatch( args );
        if( args.mcp )
        {
            SessionManager sessions( args.allowRoots, 2, {}, args.indexed );
            QueryService service( sessions, args.analysisCacheMiB * 1024 * 1024 );
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

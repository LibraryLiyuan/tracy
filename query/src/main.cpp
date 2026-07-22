#include "TracyEmbeddedData.hpp"
#include "TracyMcpServer.hpp"
#include "TracyQueryService.hpp"
#include "TracySessionManager.hpp"

#include "../../public/common/TracyVersion.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

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
    std::optional<std::filesystem::path> trace;
    std::optional<std::string> request;
    std::optional<std::string> batch;
    std::vector<std::filesystem::path> allowRoots;
    std::vector<std::filesystem::path> allowSourceRoots;
};

void Usage()
{
    std::cerr
        << "tracy-query " << tracy::Version::Major << '.' << tracy::Version::Minor << '.' << tracy::Version::Patch << "\n"
        << "Usage:\n"
        << "  tracy-query --version\n"
        << "  tracy-query --schema\n"
        << "  tracy-query --doctor [--trace file.tracy] [--allow-root path]\n"
        << "  tracy-query --trace file.tracy --request request.json|- [--allow-root path]\n"
        << "  tracy-query --trace file.tracy --batch requests.ndjson|- [--allow-root path]\n"
        << "  tracy-query --mcp [--allow-root path] [--allow-source-root path]\n";
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
        else if( option == "--trace" ) result.trace = value( "--trace" );
        else if( option == "--request" ) result.request = value( "--request" );
        else if( option == "--batch" ) result.batch = value( "--batch" );
        else if( option == "--allow-root" ) result.allowRoots.emplace_back( value( "--allow-root" ) );
        else if( option == "--allow-source-root" ) result.allowSourceRoots.emplace_back( value( "--allow-source-root" ) );
        else if( option == "--help" || option == "-h" )
        {
            Usage();
            std::exit( 0 );
        }
        else throw std::runtime_error( "unknown option: " + option );
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
    SessionManager sessions( args.allowRoots );
    QueryService service( sessions );
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
    return response.value( "ok", false ) ? 0 : 2;
}

int RunBatch( const Arguments& args )
{
    SessionManager sessions( args.allowRoots );
    QueryService service( sessions );
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
        { "checks", {
            { "json_schema", json::parse( QuerySchemaJson ).is_object() ? "ok" : "failed" },
            { "coverage_manifest", json::parse( QueryCoverageJson ).is_object() ? "ok" : "failed" },
            { "stdio_framing", "single-line UTF-8 JSON/NDJSON" }, { "statistics", "enabled" }
        } }
    };
    try
    {
        SessionManager sessions( args.allowRoots );
        json roots = json::array();
        for( const auto& root : sessions.AllowRoots() ) roots.emplace_back( root.string() );
        result["allow_roots"] = std::move( roots );
        if( args.trace )
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

}

int main( int argc, char** argv )
{
    try
    {
        const auto args = ParseArguments( argc, argv );
        const int modes = int( args.version ) + int( args.schema ) + int( args.doctor ) + int( args.mcp ) + int( args.request.has_value() ) + int( args.batch.has_value() );
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
        if( args.request ) return RunSingle( args );
        if( args.batch ) return RunBatch( args );
        if( args.mcp )
        {
            SessionManager sessions( args.allowRoots );
            QueryService service( sessions );
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

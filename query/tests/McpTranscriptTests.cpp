#include "TracyMcpServer.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

using nlohmann::json;

static json Request( int id, const char* method, json params = json::object() )
{
    return { { "jsonrpc", "2.0" }, { "id", id }, { "method", method }, { "params", std::move( params ) } };
}

int main()
{
    tracy::query::SessionManager sessions( { std::filesystem::current_path() } );
    tracy::query::QueryService query( sessions );
    tracy::query::McpServer server( sessions, query );

    const auto initialized = server.HandleRequest( Request( 1, "initialize", {
        { "protocolVersion", "2025-11-25" },
        { "clientInfo", { { "name", "transcript-test" }, { "version", "1" } } },
        { "capabilities", json::object() }
    } ) );
    assert( initialized["result"]["protocolVersion"] == "2025-11-25" );
    assert( initialized["result"]["capabilities"].contains( "tools" ) );
    assert( initialized["result"]["capabilities"].contains( "resources" ) );
    assert( initialized["result"]["instructions"].get<std::string>().find( "untrusted" ) != std::string::npos );

    const auto tools = server.HandleRequest( Request( 2, "tools/list" ) );
    assert( tools["result"]["tools"].size() == 12 );
    for( const auto& tool : tools["result"]["tools"] )
    {
        assert( tool.contains( "inputSchema" ) );
        assert( tool.contains( "outputSchema" ) );
        assert( tool["annotations"]["readOnlyHint"] == true );
        assert( tool["annotations"]["destructiveHint"] == false );
        assert( tool["annotations"]["openWorldHint"] == false );
    }

    const auto templates = server.HandleRequest( Request( 3, "resources/templates/list" ) );
    assert( templates["result"]["resourceTemplates"].size() == 3 );
    const auto resources = server.HandleRequest( Request( 4, "resources/list" ) );
    assert( resources["result"]["resources"].empty() );
    const auto ping = server.HandleRequest( Request( 5, "ping" ) );
    assert( ping["result"].is_object() );

    const auto invalidTool = server.HandleRequest( Request( 6, "tools/call", {
        { "name", "tracy_overview" }, { "arguments", { { "trace_id", "missing" } } }
    } ) );
    assert( invalidTool["result"]["isError"] == true );
    assert( invalidTool["result"]["structuredContent"]["error"]["code"] == "TRACE_NOT_FOUND" );

    const auto invalidToolArguments = server.HandleRequest( Request( 61, "tools/call", {
        { "name", "tracy_compare" }, { "arguments", { { "kind", "unsupported" } } }
    } ) );
    assert( !invalidToolArguments.contains( "error" ) );
    assert( invalidToolArguments["result"]["isError"] == true );
    assert( invalidToolArguments["result"]["structuredContent"]["error"]["code"] == "INVALID_PARAMS" );

    const auto unknownTool = server.HandleRequest( Request( 62, "tools/call", {
        { "name", "tracy_unknown" }, { "arguments", json::object() }
    } ) );
    assert( !unknownTool.contains( "error" ) );
    assert( unknownTool["result"]["isError"] == true );
    assert( unknownTool["result"]["structuredContent"]["error"]["code"] == "METHOD_NOT_FOUND" );

    assert( server.HandleRequest( { { "jsonrpc", "2.0" }, { "method", "notifications/cancelled" }, { "params", { { "requestId", 60 } } } } ).is_null() );
    const auto cancelled = server.HandleRequest( Request( 60, "tools/call", {
        { "name", "tracy_describe" }, { "arguments", json::object() }
    } ) );
    assert( cancelled["result"]["isError"] == true );
    assert( cancelled["result"]["structuredContent"]["error"]["code"] == "CANCELLED" );

    json deep = json::object();
    json* cursor = &deep;
    for( size_t depth = 0; depth < 70; depth++ )
    {
        ( *cursor )["child"] = json::object();
        cursor = &( *cursor )["child"];
    }
    const auto rejectedDepth = server.HandleRequest( Request( 9, "tools/call", {
        { "name", "tracy_describe" }, { "arguments", std::move( deep ) }
    } ) );
    assert( rejectedDepth["error"]["code"] == -32600 );

    std::stringstream input;
    input << R"({"jsonrpc":"2.0","id":7,"method":"initialize","params":{"protocolVersion":"2025-06-18"}})" << '\n';
    input << R"({"jsonrpc":"2.0","method":"notifications/initialized","params":{}})" << '\n';
    input << R"({"jsonrpc":"2.0","id":8,"method":"ping","params":{}})" << '\n';
    input << R"({"jsonrpc":"2.0","id":9,"method":"logging/setLevel","params":{"level":"info"}})" << '\n';
    input << R"({"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"tracy_describe","arguments":{},"_meta":{"progressToken":"progress-1"}}})" << '\n';
    input << "{malformed" << '\n';
    input << std::string( tracy::query::MaximumRequestBytes + 1, 'x' ) << '\n';
    std::stringstream output;
    assert( server.Run( input, output ) == 0 );

    std::vector<json> messages;
    std::string line;
    while( std::getline( output, line ) ) messages.emplace_back( json::parse( line ) );
    assert( messages.size() == 7 );
    assert( messages[0]["result"]["protocolVersion"] == "2025-06-18" );
    assert( messages[1]["result"].is_object() );
    assert( messages[2]["result"].is_object() );
    assert( messages[3]["method"] == "notifications/progress" );
    assert( messages[3]["params"]["progressToken"] == "progress-1" );
    assert( messages[4]["result"]["isError"] == false );
    assert( messages[5]["error"]["code"] == -32700 );
    assert( messages[6]["error"]["code"] == -32600 );
    return 0;
}

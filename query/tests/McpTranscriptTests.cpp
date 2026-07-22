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

    std::stringstream input;
    input << R"({"jsonrpc":"2.0","id":7,"method":"initialize","params":{"protocolVersion":"2025-06-18"}})" << '\n';
    input << R"({"jsonrpc":"2.0","method":"notifications/initialized","params":{}})" << '\n';
    input << R"({"jsonrpc":"2.0","id":8,"method":"ping","params":{}})" << '\n';
    input << "{malformed" << '\n';
    std::stringstream output;
    assert( server.Run( input, output ) == 0 );

    std::vector<json> messages;
    std::string line;
    while( std::getline( output, line ) ) messages.emplace_back( json::parse( line ) );
    assert( messages.size() == 3 );
    assert( messages[0]["result"]["protocolVersion"] == "2025-06-18" );
    assert( messages[1]["result"].is_object() );
    assert( messages[2]["error"]["code"] == -32700 );
    return 0;
}

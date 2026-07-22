#include "TracyMcpServer.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <string>

#ifndef TRACY_ACCEPTANCE_TRACE
#  error TRACY_ACCEPTANCE_TRACE must be defined
#endif

using nlohmann::json;

static json Request( int id, const char* method, json params = json::object() )
{
    return { { "jsonrpc", "2.0" }, { "id", id }, { "method", method }, { "params", std::move( params ) } };
}

static json ToolCall( int id, const char* name, json arguments )
{
    return Request( id, "tools/call", { { "name", name }, { "arguments", std::move( arguments ) } } );
}

int main()
{
    const std::filesystem::path tracePath = TRACY_ACCEPTANCE_TRACE;
    const auto root = tracePath.parent_path();
    tracy::query::SessionManager sessions( { root } );
    tracy::query::QueryService query( sessions );
    tracy::query::McpServer server( sessions, query, { root } );

    const auto initialize = server.HandleRequest( Request( 1, "initialize", { { "protocolVersion", "2025-11-25" } } ) );
    assert( initialize["result"]["protocolVersion"] == "2025-11-25" );

    const auto opened = server.HandleRequest( ToolCall( 2, "tracy_trace_open", { { "path", tracePath.string() } } ) );
    assert( opened["result"]["isError"] == false );
    const auto traceId = opened["result"]["structuredContent"]["data"]["trace_id"].get<std::string>();
    const auto ready = sessions.WaitReady( traceId, std::chrono::seconds( 60 ) );
    assert( ready.state == tracy::analysis::TraceSourceState::Ready );

    const auto overview = server.HandleRequest( ToolCall( 3, "tracy_overview", { { "trace_id", traceId } } ) );
    assert( overview["result"]["isError"] == false );
    assert( overview["result"]["structuredContent"]["data"]["trace"]["counts"]["cpu_zones"] != "0" );
    assert( overview["result"]["structuredContent"]["trace"]["fingerprint"] == ready.fingerprint );

    const auto resources = server.HandleRequest( Request( 4, "resources/list" ) );
    assert( !resources["result"]["resources"].empty() );
    auto source = resources["result"]["resources"].end();
    for( auto it = resources["result"]["resources"].begin(); it != resources["result"]["resources"].end(); ++it )
    {
        if( ( *it )["uri"].get<std::string>().find( "/source/" ) != std::string::npos ) { source = it; break; }
    }
    assert( source != resources["result"]["resources"].end() );
    const auto read = server.HandleRequest( Request( 5, "resources/read", { { "uri", ( *source )["uri"] } } ) );
    assert( read["result"]["contents"][0]["mimeType"] == "text/plain" );
    assert( read["result"]["contents"][0]["text"].is_string() );

    const auto closed = server.HandleRequest( ToolCall( 6, "tracy_trace_close", { { "trace_id", traceId } } ) );
    assert( closed["result"]["isError"] == false );
    return 0;
}

#include "TracyMcpServer.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <thread>

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
    const auto stage = []( const char* name ) { std::cerr << "[mcp-worker-integration] " << name << '\n'; };
    stage( "construct" );
    const std::filesystem::path tracePath = TRACY_ACCEPTANCE_TRACE;
    const auto root = tracePath.parent_path();
    tracy::query::SessionManager sessions( { root } );
    tracy::query::QueryService query( sessions );
    tracy::query::McpServer server( sessions, query, { root } );

    stage( "initialize" );
    const auto initialize = server.HandleRequest( Request( 1, "initialize", { { "protocolVersion", "2025-11-25" } } ) );
    assert( initialize["result"]["protocolVersion"] == "2025-11-25" );

    stage( "open baseline" );
    const auto opened = server.HandleRequest( ToolCall( 2, "tracy_trace_open", { { "path", tracePath.string() } } ) );
    assert( opened["result"]["isError"] == false );
    const auto traceId = opened["result"]["structuredContent"]["data"]["trace_id"].get<std::string>();
    const auto ready = sessions.WaitReady( traceId, std::chrono::seconds( 60 ) );
    assert( ready.state == tracy::analysis::TraceSourceState::Ready );

    stage( "overview" );
    const auto overview = server.HandleRequest( ToolCall( 3, "tracy_overview", { { "trace_id", traceId } } ) );
    assert( overview["result"]["isError"] == false );
    assert( overview["result"]["structuredContent"]["data"]["trace"]["counts"]["cpu_zones"] != "0" );
    assert( overview["result"]["structuredContent"]["trace"]["fingerprint"] == ready.fingerprint );

    stage( "describe" );
    const auto described = server.HandleRequest( ToolCall( 7, "tracy_describe", { { "operation", "memory.frame_snapshot" } } ) );
    assert( described["result"]["isError"] == false );
    assert( described["result"]["structuredContent"]["data"]["operations"].size() == 1 );
    assert( described["result"]["structuredContent"]["data"]["operations"][0]["method"] == "memory.frame_snapshot" );

    stage( "validation" );
    const auto validation = server.HandleRequest( ToolCall( 8, "tracy_validate", { { "trace_id", traceId } } ) );
    assert( validation["result"]["isError"] == false );
    assert( validation["result"]["structuredContent"]["data"]["checks"].size() >= 10 );

    stage( "async validation job" );
    const auto submitted = server.HandleRequest( ToolCall( 80, "tracy_validate", { { "trace_id", traceId }, { "async", true } } ) );
    assert( submitted["result"]["isError"] == false );
    const auto jobId = submitted["result"]["structuredContent"]["data"]["job_id"].get<std::string>();
    std::string jobState;
    for( size_t attempt = 0; attempt < 500; attempt++ )
    {
        const auto status = server.HandleRequest( ToolCall( 81, "tracy_job", { { "job_id", jobId }, { "operation", "status" } } ) );
        assert( status["result"]["isError"] == false );
        jobState = status["result"]["structuredContent"]["data"]["state"].get<std::string>();
        if( jobState == "completed" || jobState == "failed" || jobState == "cancelled" ) break;
        std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
    }
    assert( jobState == "completed" );
    const auto jobResult = server.HandleRequest( ToolCall( 82, "tracy_job", { { "job_id", jobId }, { "operation", "result" } } ) );
    assert( jobResult["result"]["isError"] == false );
    assert( jobResult["result"]["structuredContent"]["data"]["checks"].size() >= 10 );

    stage( "resources list" );
    const auto resources = server.HandleRequest( Request( 4, "resources/list" ) );
    assert( !resources["result"]["resources"].empty() );
    assert( resources["result"]["resources"].size() <= 100 );
    if( resources["result"].contains( "nextCursor" ) )
    {
        std::set<std::string> firstPageUris;
        for( const auto& resource : resources["result"]["resources"] ) firstPageUris.emplace( resource["uri"].get<std::string>() );
        const auto next = server.HandleRequest( Request( 41, "resources/list", { { "cursor", resources["result"]["nextCursor"] } } ) );
        assert( !next["result"]["resources"].empty() );
        assert( next["result"]["resources"].size() <= 100 );
        for( const auto& resource : next["result"]["resources"] ) assert( firstPageUris.find( resource["uri"].get<std::string>() ) == firstPageUris.end() );
    }
    auto source = resources["result"]["resources"].end();
    for( auto it = resources["result"]["resources"].begin(); it != resources["result"]["resources"].end(); ++it )
    {
        if( ( *it )["uri"].get<std::string>().find( "/source/" ) != std::string::npos ) { source = it; break; }
    }
    assert( source != resources["result"]["resources"].end() );
    stage( "resource read" );
    const auto read = server.HandleRequest( Request( 5, "resources/read", { { "uri", ( *source )["uri"] } } ) );
    assert( read["result"]["contents"][0]["mimeType"] == "text/plain" );
    assert( read["result"]["contents"][0]["text"].is_string() );

#ifdef TRACY_ACCEPTANCE_COMPARE_TRACE
    const std::filesystem::path candidatePath = TRACY_ACCEPTANCE_COMPARE_TRACE;
    stage( "open candidate" );
    const auto openedCandidate = server.HandleRequest( ToolCall( 9, "tracy_trace_open", { { "path", candidatePath.string() } } ) );
    assert( openedCandidate["result"]["isError"] == false );
    const auto candidateId = openedCandidate["result"]["structuredContent"]["data"]["trace_id"].get<std::string>();
    const auto candidateReady = sessions.WaitReady( candidateId, std::chrono::seconds( 60 ) );
    assert( candidateReady.state == tracy::analysis::TraceSourceState::Ready );

    for( const auto* kind : { "frames", "zones", "source" } )
    {
        stage( kind );
        const auto compared = server.HandleRequest( ToolCall( 10, "tracy_compare", {
            { "baseline_trace_id", traceId }, { "candidate_trace_id", candidateId }, { "kind", kind }, { "limit", 5 }
        } ) );
        assert( compared["result"]["isError"] == false );
        assert( compared["result"]["structuredContent"]["data"]["traces"]["baseline"]["fingerprint"] == ready.fingerprint );
        assert( compared["result"]["structuredContent"]["data"]["traces"]["candidate"]["fingerprint"] == candidateReady.fingerprint );
        const auto& data = compared["result"]["structuredContent"]["data"];
        if( std::string_view( kind ) == "frames" ) assert( data["frame_sets"].is_array() );
        else if( std::string_view( kind ) == "zones" ) assert( data["groups"].is_array() );
        else assert( data["changed"].is_array() && data["baseline_only"].is_array() && data["candidate_only"].is_array() );
    }
    stage( "close candidate" );
    const auto closedCandidate = server.HandleRequest( ToolCall( 11, "tracy_trace_close", { { "trace_id", candidateId } } ) );
    assert( closedCandidate["result"]["isError"] == false );
#endif

    stage( "close baseline" );
    const auto closed = server.HandleRequest( ToolCall( 12, "tracy_trace_close", { { "trace_id", traceId } } ) );
    assert( closed["result"]["isError"] == false );

    const auto verifyPair = [&]( const char* pairName, const std::filesystem::path& baselinePath, const std::filesystem::path& candidatePath, int& requestId ) {
        stage( pairName );
        const auto leftOpen = server.HandleRequest( ToolCall( requestId++, "tracy_trace_open", { { "path", baselinePath.string() } } ) );
        assert( leftOpen["result"]["isError"] == false );
        const auto leftId = leftOpen["result"]["structuredContent"]["data"]["trace_id"].get<std::string>();
        const auto leftReady = sessions.WaitReady( leftId, std::chrono::seconds( 60 ) );
        assert( leftReady.state == tracy::analysis::TraceSourceState::Ready );
        const auto rightOpen = server.HandleRequest( ToolCall( requestId++, "tracy_trace_open", { { "path", candidatePath.string() } } ) );
        assert( rightOpen["result"]["isError"] == false );
        const auto rightId = rightOpen["result"]["structuredContent"]["data"]["trace_id"].get<std::string>();
        const auto rightReady = sessions.WaitReady( rightId, std::chrono::seconds( 60 ) );
        assert( rightReady.state == tracy::analysis::TraceSourceState::Ready );
        for( const auto* kind : { "frames", "zones", "source" } )
        {
            std::cerr << "[mcp-worker-integration] " << pairName << ' ' << kind << '\n';
            const auto compared = server.HandleRequest( ToolCall( requestId++, "tracy_compare", {
                { "baseline_trace_id", leftId }, { "candidate_trace_id", rightId }, { "kind", kind }, { "limit", 5 }
            } ) );
            assert( compared["result"]["isError"] == false );
            auto result = compared["result"]["structuredContent"];
            if( result["data"].contains( "job_id" ) )
            {
                const auto jobId = result["data"]["job_id"].get<std::string>();
                std::string state;
                for( size_t attempt = 0; attempt < 6000; attempt++ )
                {
                    const auto status = server.HandleRequest( ToolCall( requestId++, "tracy_job", { { "job_id", jobId }, { "operation", "status" } } ) );
                    assert( status["result"]["isError"] == false );
                    state = status["result"]["structuredContent"]["data"]["state"].get<std::string>();
                    if( state == "completed" || state == "failed" || state == "cancelled" ) break;
                    std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
                }
                assert( state == "completed" );
                const auto completed = server.HandleRequest( ToolCall( requestId++, "tracy_job", { { "job_id", jobId }, { "operation", "result" } } ) );
                assert( completed["result"]["isError"] == false );
                result = completed["result"]["structuredContent"];
            }
            assert( result["data"]["traces"]["baseline"]["fingerprint"] == leftReady.fingerprint );
            assert( result["data"]["traces"]["candidate"]["fingerprint"] == rightReady.fingerprint );
        }
        assert( server.HandleRequest( ToolCall( requestId++, "tracy_trace_close", { { "trace_id", rightId } } ) )["result"]["isError"] == false );
        assert( server.HandleRequest( ToolCall( requestId++, "tracy_trace_close", { { "trace_id", leftId } } ) )["result"]["isError"] == false );
    };
    int pairRequestId = 200;
#if defined TRACY_ACCEPTANCE_CONNECT_BASELINE && defined TRACY_ACCEPTANCE_CONNECT_CANDIDATE
    verifyPair( "compare connect pair", TRACY_ACCEPTANCE_CONNECT_BASELINE, TRACY_ACCEPTANCE_CONNECT_CANDIDATE, pairRequestId );
#endif
#if defined TRACY_ACCEPTANCE_RECONNECT_BASELINE && defined TRACY_ACCEPTANCE_RECONNECT_CANDIDATE
    verifyPair( "compare reconnect pair", TRACY_ACCEPTANCE_RECONNECT_BASELINE, TRACY_ACCEPTANCE_RECONNECT_CANDIDATE, pairRequestId );
#endif

#ifdef TRACY_ACCEPTANCE_CORRUPT_TRACE
    stage( "corrupt trace rejection" );
    const auto corrupt = sessions.Open( std::filesystem::path( TRACY_ACCEPTANCE_CORRUPT_TRACE ) );
    const auto corruptResult = sessions.WaitReady( corrupt.id, std::chrono::seconds( 10 ) );
    assert( corruptResult.state == tracy::analysis::TraceSourceState::Failed );
    assert( corruptResult.errorCode == "CORRUPT_TRACE" );
    assert( corruptResult.errorMessage.find( "declared records" ) != std::string::npos );
#endif
    return 0;
}

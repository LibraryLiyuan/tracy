#include "TracyQueryIndex.hpp"
#include "../../analysis/TracyBoundedScanCursor.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

using namespace tracy::analysis;
using namespace tracy::query;

namespace
{
uint64_t ElapsedMs( std::chrono::steady_clock::time_point begin )
{
    return uint64_t( std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin ).count() );
}

void RequireSameFacts( const CpuZoneDto& raw, const CpuZoneDto& policy )
{
    if( raw.ref != policy.ref || raw.threadRef != policy.threadRef ||
        raw.sourceLocationRef != policy.sourceLocationRef || raw.parentRef != policy.parentRef ||
        raw.function != policy.function || raw.file != policy.file || raw.line != policy.line ||
        raw.startNs != policy.startNs || raw.endNs != policy.endNs ||
        raw.childCount != policy.childCount || raw.complete != policy.complete )
        throw std::runtime_error( "cpu_policy_projection_changed_structural_fact" );
    if( policy.extraName || policy.extraText || policy.callstack != 0 || policy.callstackRef )
        throw std::runtime_error( "cpu_policy_projection_retained_optional_payload" );
}
}

int main( int argc, char** argv )
{
    try
    {
        if( argc != 4 ) throw std::runtime_error( "expected trace path, start ordinal and count" );
        const auto tracePath = std::filesystem::path( argv[1] );
        const auto start = std::stoull( argv[2] );
        const auto count = std::stoull( argv[3] );
        if( count == 0 || count > NativeBoundedScanMaximumBatch )
            throw std::runtime_error( "probe_count_out_of_range" );
        const auto validated = QueryIndex::Validate( tracePath, false );
        if( !validated.manifest ) throw std::runtime_error( validated.reason );
        auto source = QueryIndex::Open( *validated.manifest );
        ScanRange range;
        range.offset = size_t( start );
        range.limit = size_t( count );
        auto began = std::chrono::steady_clock::now();
        const auto policy = source->ScanCpuZonesForPolicy( range );
        const auto policyMs = ElapsedMs( began );
        began = std::chrono::steady_clock::now();
        const auto raw = source->ScanCpuZones( range );
        const auto rawMs = ElapsedMs( began );
        if( policy.size() != count || raw.size() != count || policy.size() != raw.size() )
            throw std::runtime_error( "cpu_policy_projection_count" );
        uint64_t dynamicNames = 0;
        for( size_t index = 0; index < raw.size(); ++index )
        {
            RequireSameFacts( raw[index], policy[index] );
            if( raw[index].extraName ) ++dynamicNames;
        }
        if( dynamicNames == 0 ) throw std::runtime_error( "probe_window_has_no_dynamic_names" );
        std::cout << nlohmann::json {
            { "start", std::to_string( start ) }, { "count", std::to_string( count ) },
            { "dynamic_names", std::to_string( dynamicNames ) },
            { "policy_ms", std::to_string( policyMs ) }, { "raw_ms", std::to_string( rawMs ) },
            { "facts_equal", true }, { "dynamic_payload_omitted", true }
        }.dump() << '\n';
        return 0;
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

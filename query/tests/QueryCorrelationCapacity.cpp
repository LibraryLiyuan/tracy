#include "TracyQueryCorrelationStore.hpp"

#include <nlohmann/json.hpp>

#include <charconv>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace tracy;
using nlohmann::json;

namespace
{

uint64_t ParseCount( const char* text )
{
    uint64_t value = 0;
    const auto end = text + std::strlen( text );
    const auto parsed = std::from_chars( text, end, value );
    if( parsed.ec != std::errc() || parsed.ptr != end || value == 0 )
        throw std::runtime_error( "count must be a positive integer" );
    return value;
}

uint64_t Milliseconds( std::chrono::steady_clock::time_point begin )
{
    return uint64_t( std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin ).count() );
}

}

int main( int argc, char** argv )
{
    try
    {
        if( argc != 3 ) throw std::runtime_error( "usage: correlation-capacity COUNT ROOT" );
        const auto count = ParseCount( argv[1] );
        const std::filesystem::path root = argv[2];
        if( std::filesystem::exists( root ) ) throw std::runtime_error( "capacity root already exists" );

        auto usage = std::make_shared<analysis::AnalysisDiskUsage>(
            std::vector<std::filesystem::path>{ root } );
        analysis::AnalysisDiskActivity activity( usage );
        auto disk = std::make_shared<analysis::AnalysisDiskBudget>();
        disk->usage = usage;
        disk->maximumBytes = 8ull * 1024 * 1024 * 1024;
        auto workspace = std::make_shared<analysis::AnalysisWorkspaceBudget>(
            1024ull * 1024 * 1024, 512ull * 1024 * 1024 );
        analysis::PagedRecordOptions options;
        options.disk = disk;
        options.workspace = workspace;
        options.blockBytes = 4 * 1024 * 1024;

        constexpr uint64_t multiplier = 11400714819323198485ull;
        const std::string fingerprint( 64, 'b' );
        query::QueryCorrelationBuildStatistics statistics;
        const auto buildStarted = std::chrono::steady_clock::now();
        {
            query::QueryCorrelationStoreWriter writer( root, fingerprint, count, options );
            for( uint64_t ordinal = 0; ordinal < count; ordinal++ )
                writer.Append( ordinal * multiplier,
                    query::QueryCorrelationEndpointTag( ordinal %
                        uint8_t( query::QueryCorrelationEndpointTag::Count ) ), ordinal );
            statistics = writer.Commit();
        }
        const auto buildMs = Milliseconds( buildStarted );
        const auto afterBuild = usage->Snapshot();
        const auto finalBytes = analysis::AnalysisDiskUsage::Bytes( root );

        uint64_t validateMs = 0;
        uint64_t pointsMs = 0;
        uint64_t pointCandidates = 0;
        {
            query::QueryCorrelationStoreReader reader( root, fingerprint, options );
            const auto validateStarted = std::chrono::steady_clock::now();
            reader.ValidateAll();
            validateMs = Milliseconds( validateStarted );
            const auto pointsStarted = std::chrono::steady_clock::now();
            for( uint64_t sample = 0; sample < std::min<uint64_t>( count, 1000 ); sample++ )
            {
                const auto ordinal = sample * ( count / std::min<uint64_t>( count, 1000 ) );
                const auto page = reader.ReadCandidates( ordinal * multiplier, 0, 4096 );
                pointCandidates += page.values.size();
                bool found = false;
                for( const auto& value : page.values ) found |= value.ordinal == ordinal;
                if( !found ) throw std::runtime_error( "point lookup lost its source ordinal" );
            }
            pointsMs = Milliseconds( pointsStarted );
        }

        std::error_code cleanupError;
        analysis::AnalysisDiskRemove( root, disk, cleanupError, true );
        if( cleanupError || usage->Current() != 0 || workspace->Snapshot().currentBytes != 0 )
            throw std::runtime_error( "capacity cleanup or resource accounting failed" );

        const auto workspaceSnapshot = workspace->Snapshot();
        std::cout << json( {
            { "records", count },
            { "bucket_count", statistics.bucketCount },
            { "partition_count", statistics.partitionCount },
            { "temporary_logical_bytes", statistics.temporaryBytes },
            { "peak_partition_records", statistics.peakPartitionRecords },
            { "final_physical_bytes", finalBytes },
            { "disk_peak_bytes", afterBuild.peakBytes },
            { "workspace_peak_bytes", workspaceSnapshot.peakBytes },
            { "build_ms", buildMs },
            { "validate_ms", validateMs },
            { "points_1000_ms", pointsMs },
            { "point_candidates", pointCandidates },
            { "cleanup_current_bytes", usage->Current() }
        } ).dump( 2 ) << '\n';
        return 0;
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

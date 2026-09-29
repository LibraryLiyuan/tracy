#include "TracyPagedRecordStore.hpp"
#include "TracyQueryCorrelationStore.hpp"

#include <nlohmann/json.hpp>

#include <charconv>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace tracy;
using nlohmann::json;

namespace
{

#pragma pack( push, 1 )
struct PassProjection
{
    uint32_t entityOrdinal = 0;
    uint32_t segmentOrdinalAndStable = 0;
    uint64_t commandListId = 0;
    uint64_t frameId = 0;
    uint64_t cameraId = 0;
    uint64_t viewId = 0;
    uint64_t referenceToken = 0;
    uint32_t taxonomyId = 0;
};

struct GpuZoneTimeKey
{
    uint64_t queryKey = 0;
    int64_t cpuStartNs = 0;
    uint64_t zoneOrdinal = 0;
};
#pragma pack( pop )

static_assert( sizeof( PassProjection ) == 52 );
static_assert( sizeof( GpuZoneTimeKey ) == 24 );

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

uint64_t Mix( uint64_t value )
{
    value += 0x9e3779b97f4a7c15ull;
    value = ( value ^ ( value >> 30 ) ) * 0xbf58476d1ce4e5b9ull;
    value = ( value ^ ( value >> 27 ) ) * 0x94d049bb133111ebull;
    return value ^ ( value >> 31 );
}

uint64_t Bytes( const std::filesystem::path& path )
{
    return analysis::AnalysisDiskUsage::Bytes( path );
}

}

int main( int argc, char** argv )
{
    try
    {
        if( argc != 3 && argc != 4 )
            throw std::runtime_error( "usage: explicit-pass-capacity COUNT ROOT [high-entropy]" );
        const auto count = ParseCount( argv[1] );
        const bool highEntropy = argc == 4 && std::string_view( argv[3] ) == "high-entropy";
        if( argc == 4 && !highEntropy ) throw std::runtime_error( "unknown distribution" );
        const auto zoneCount = count / 20 + ( count % 20 != 0 );
        const std::filesystem::path root = argv[2];
        if( std::filesystem::exists( root ) )
            throw std::runtime_error( "capacity root already exists" );
        std::filesystem::create_directories( root );

        auto usage = std::make_shared<analysis::AnalysisDiskUsage>(
            std::vector<std::filesystem::path>{ root } );
        analysis::AnalysisDiskActivity activity( usage );
        auto disk = std::make_shared<analysis::AnalysisDiskBudget>();
        disk->usage = usage;
        disk->maximumBytes = 16ull * 1024 * 1024 * 1024;
        auto workspace = std::make_shared<analysis::AnalysisWorkspaceBudget>(
            1024ull * 1024 * 1024, 512ull * 1024 * 1024 );
        analysis::PagedRecordOptions options;
        options.disk = disk;
        options.workspace = workspace;
        options.blockBytes = 4 * 1024 * 1024;

        const std::string fingerprint( 64, 'c' );
        const std::string projectionIdentity( 64, '1' );
        const std::string modeIdentity( 64, '2' );
        const std::string zoneIdentity( 64, '4' );
        query::QueryCorrelationBuildStatistics filterStatistics;
        const auto started = std::chrono::steady_clock::now();
        {
            analysis::PagedRecordWriter projections( root / "projections", projectionIdentity,
                sizeof( PassProjection ), options );
            analysis::PagedRecordWriter modeBits( root / "mode-bits", modeIdentity,
                sizeof( uint8_t ), options );
            query::QueryCorrelationStoreWriter filters( root / "filters", fingerprint,
                count * 2, options );
            constexpr uint64_t multiplier = 11400714819323198485ull;
            uint8_t modeByte = 0;
            for( uint64_t ordinal = 0; ordinal < count; ordinal++ )
            {
                PassProjection record;
                record.entityOrdinal = uint32_t( ordinal * 5 );
                record.segmentOrdinalAndStable = ( highEntropy ?
                    uint32_t( Mix( ordinal ^ 0x13579bdf2468ace0ull ) ) : uint32_t( ordinal * 5 + 1 ) ) & 0x7fffffffu;
                record.commandListId = highEntropy ? Mix( ordinal ^ 0x2468ace013579bdfull ) : ordinal / 8 + 1;
                record.frameId = highEntropy ? Mix( ordinal ^ 0x55aa55aa33cc33ccull ) : ordinal / 100 + 1;
                record.cameraId = highEntropy ? Mix( ordinal ^ 0xaa55aa55cc33cc33ull ) : ordinal % 64 + 1;
                record.viewId = highEntropy ? Mix( ordinal ^ 0xfedcba9876543210ull ) : ordinal % 256 + 1;
                record.referenceToken = highEntropy ? Mix( ordinal ^ 0x0123456789abcdefull ) : ordinal * multiplier;
                record.taxonomyId = highEntropy ? uint32_t( Mix( ordinal ^ 0x9988776655443322ull ) ) :
                    uint32_t( ordinal % 2048 + 1 );
                if( ordinal % 3 != 0 ) record.segmentOrdinalAndStable |= 0x80000000u;
                projections.Append( &record, 1 );
                filters.Append( highEntropy ? Mix( ordinal ^ 0x1122334455667788ull ) : ordinal % 100000,
                    query::QueryCorrelationEndpointTag::LinkSource, ordinal );
                filters.Append( record.taxonomyId,
                    query::QueryCorrelationEndpointTag::LinkTarget, ordinal );
                if( ordinal % 4 == 0 ) modeByte |= uint8_t( 1u << ( ordinal & 7 ) );
                if( ( ordinal & 7 ) == 7 )
                {
                    modeBits.Append( &modeByte, 1 );
                    modeByte = 0;
                }
            }
            if( ( count & 7 ) != 0 ) modeBits.Append( &modeByte, 1 );
            projections.Commit();
            modeBits.Commit();
            filterStatistics = filters.Commit();

            analysis::PagedRecordWriter zones( root / "zones", zoneIdentity,
                sizeof( GpuZoneTimeKey ), options );
            constexpr uint64_t keyCount = 4 * 65536;
            for( uint64_t position = 0; position < zoneCount; position++ )
            {
                GpuZoneTimeKey record;
                record.queryKey = position * keyCount / zoneCount;
                record.cpuStartNs = int64_t( position ) * 1000;
                record.zoneOrdinal = position * multiplier;
                zones.Append( &record, 1 );
            }
            zones.Commit();
        }
        const auto buildMs = Milliseconds( started );
        const auto snapshot = usage->Snapshot();
        const auto finalBytes = Bytes( root );

        const auto validateStarted = std::chrono::steady_clock::now();
        {
            analysis::PagedRecordReader projections( root / "projections", projectionIdentity,
                sizeof( PassProjection ), options );
            analysis::PagedRecordReader modeBits( root / "mode-bits", modeIdentity,
                sizeof( uint8_t ), options );
            analysis::PagedRecordReader zones( root / "zones", zoneIdentity,
                sizeof( GpuZoneTimeKey ), options );
            query::QueryCorrelationStoreReader filters( root / "filters", fingerprint, options );
            projections.ValidateAll();
            modeBits.ValidateAll();
            zones.ValidateAll();
            filters.ValidateAll();
            if( projections.Count() != count || modeBits.Count() != ( count + 7 ) / 8 ||
                zones.Count() != zoneCount || filters.Count() != count * 2 )
                throw std::runtime_error( "capacity model count mismatch" );
        }
        const auto validateMs = Milliseconds( validateStarted );

        const auto workspaceSnapshot = workspace->Snapshot();
        const auto result = json {
            { "passes", count }, { "gpu_zones", zoneCount },
            { "distribution", highEntropy ? "high-entropy" : "representative" },
            { "projection_record_bytes", sizeof( PassProjection ) },
            { "projection_ordinal_bits", 32 },
            { "zone_record_bytes", sizeof( GpuZoneTimeKey ) },
            { "filter_records", filterStatistics.records },
            { "projection_bytes", Bytes( root / "projections" ) },
            { "filter_bytes", Bytes( root / "filters" ) },
            { "mode_bytes", Bytes( root / "mode-bits" ) },
            { "zone_bytes", Bytes( root / "zones" ) },
            { "final_physical_bytes", finalBytes }, { "disk_peak_bytes", snapshot.peakBytes },
            { "workspace_peak_bytes", workspaceSnapshot.peakBytes },
            { "build_ms", buildMs }, { "validate_ms", validateMs }
        };

        std::error_code cleanupError;
        analysis::AnalysisDiskRemove( root, disk, cleanupError, true );
        if( cleanupError || usage->Current() != 0 || workspace->Snapshot().currentBytes != 0 )
            throw std::runtime_error( "capacity cleanup or resource accounting failed" );
        std::cout << result.dump( 2 ) << '\n';
        return 0;
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

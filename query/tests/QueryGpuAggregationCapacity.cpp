#include "TracyAnalysisExternalSort.hpp"
#include "TracyAnalysisIoPath.hpp"
#include "TracyPagedRecordStore.hpp"
#include "TracyTraceSource.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

using namespace tracy;
using nlohmann::json;

namespace
{

struct CurrentTaxonomyZone
{
    std::string ref;
    std::string contextRef;
    std::string name;
    std::string file;
    uint32_t line = 0;
    int64_t begin = 0;
    int64_t end = 0;
};

uint64_t ParseCount( const char* text )
{
    uint64_t value = 0;
    const auto end = text + std::strlen( text );
    const auto parsed = std::from_chars( text, end, value );
    if( parsed.ec != std::errc() || parsed.ptr != end || value == 0 )
        throw std::runtime_error( "count must be a positive integer" );
    return value;
}

uint64_t Mix( uint64_t value )
{
    value += 0x9e3779b97f4a7c15ull;
    value = ( value ^ ( value >> 30 ) ) * 0xbf58476d1ce4e5b9ull;
    value = ( value ^ ( value >> 27 ) ) * 0x94d049bb133111ebull;
    return value ^ ( value >> 31 );
}

uint64_t Milliseconds( std::chrono::steady_clock::time_point begin )
{
    return uint64_t( std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin ).count() );
}

bool Less( const analysis::AnalysisKeyMetadataRecord& lhs,
    const analysis::AnalysisKeyMetadataRecord& rhs )
{
    if( lhs.keyHigh != rhs.keyHigh ) return lhs.keyHigh < rhs.keyHigh;
    if( lhs.keyLow != rhs.keyLow ) return lhs.keyLow < rhs.keyLow;
    if( lhs.kind != rhs.kind ) return lhs.kind < rhs.kind;
    if( lhs.metadata != rhs.metadata ) return lhs.metadata < rhs.metadata;
    return lhs.auxiliary < rhs.auxiliary;
}

struct ModelResult
{
    uint64_t records = 0;
    uint32_t recordBytes = 0;
    uint64_t buildMs = 0;
    uint64_t validateMs = 0;
    uint64_t finalBytes = 0;
    uint64_t peakBytes = 0;
    uint64_t workspacePeakBytes = 0;
    analysis::AnalysisExternalSortStatistics sort;
};

void WriteZeroFile( const std::filesystem::path& path, uint64_t bytes,
    const std::shared_ptr<analysis::AnalysisDiskBudget>& disk )
{
    analysis::AnalysisDiskGrow( disk, bytes );
    std::ofstream output( analysis::AnalysisIoPath( path ), std::ios::binary );
    if( !output ) throw std::runtime_error( "capacity bitset open failed" );
    std::array<char, 65536> zeros {};
    for( uint64_t written = 0; written < bytes; )
    {
        const auto count = size_t( std::min<uint64_t>( zeros.size(), bytes - written ) );
        output.write( zeros.data(), std::streamsize( count ) );
        if( !output ) throw std::runtime_error( "capacity bitset write failed" );
        written += count;
    }
    output.flush();
    if( !output ) throw std::runtime_error( "capacity bitset flush failed" );
}

ModelResult RunModel( const std::filesystem::path& root, uint64_t records,
    uint64_t bitsetBytes, bool highEntropy, bool compare )
{
    std::filesystem::create_directories( root / "runs" );
    auto usage = std::make_shared<analysis::AnalysisDiskUsage>(
        std::vector<std::filesystem::path>{ root } );
    analysis::AnalysisDiskActivity activity( usage );
    auto disk = std::make_shared<analysis::AnalysisDiskBudget>();
    disk->usage = usage;
    disk->maximumBytes = 8ull * 1024 * 1024 * 1024;
    auto workspace = std::make_shared<analysis::AnalysisWorkspaceBudget>(
        1024ull * 1024 * 1024, 512ull * 1024 * 1024 );
    const auto output = root / "sorted.bin";
    const uint32_t recordBytes = compare ? sizeof( analysis::AnalysisUInt64Pair ) :
        sizeof( analysis::AnalysisKeyMetadataRecord );
    const auto maximumBuffered = analysis::ChooseAnalysisExternalSortBufferRecords(
        records, recordBytes, workspace );
    ModelResult result;
    result.records = records;
    result.recordBytes = recordBytes;
    const auto started = std::chrono::steady_clock::now();
    {
        constexpr uint64_t sign = uint64_t( 1 ) << 63;
        if( compare )
        {
            analysis::AnalysisUInt64PairRunCollector collector( output, root / "runs",
                "compare", maximumBuffered, disk, workspace, {}, &result.sort );
            for( uint64_t ordinal = 0; ordinal < records; ordinal++ )
            {
                const auto group = highEntropy ? Mix( ordinal ^ 0x1020304050607080ull ) % 4096 : ordinal % 128;
                const auto metric = ordinal & 1;
                const int64_t value = int64_t( highEntropy ? Mix( ordinal ^ 0x8877665544332211ull ) & 0x7fffffffffffffffull : ordinal );
                collector.Emit( { group * 2 + metric, uint64_t( value ) ^ sign } );
            }
            std::string error;
            if( !collector.Finish( error ) ) throw std::runtime_error( error );
        }
        else
        {
            analysis::AnalysisKeyMetadataRunCollector collector( output, root / "runs",
                "taxonomy", maximumBuffered, disk, workspace, {}, &result.sort );
            for( uint64_t ordinal = 0; ordinal < records; ordinal++ )
            {
                analysis::AnalysisKeyMetadataRecord record;
                const auto group = highEntropy ? Mix( ordinal ^ 0x1020304050607080ull ) % 4096 : ordinal % 128;
                const int64_t value = int64_t( highEntropy ? Mix( ordinal ^ 0x8877665544332211ull ) & 0x7fffffffffffffffull : ordinal );
                record.keyHigh = group;
                record.keyLow = uint64_t( value ) ^ sign;
                record.metadata = ordinal;
                collector.Emit( record );
            }
            std::string error;
            if( !collector.Finish( error ) ) throw std::runtime_error( error );
        }
    }
    if( bitsetBytes != 0 ) WriteZeroFile( root / "matched-zones.bits", bitsetBytes, disk );
    result.buildMs = Milliseconds( started );
    result.finalBytes = analysis::AnalysisDiskUsage::Bytes( root );
    result.peakBytes = usage->Snapshot().peakBytes;
    result.workspacePeakBytes = workspace->Snapshot().peakBytes;

    const auto validationStarted = std::chrono::steady_clock::now();
    std::ifstream input( analysis::AnalysisIoPath( output ), std::ios::binary );
    if( !input ) throw std::runtime_error( "capacity sorted output open failed" );
    uint64_t read = 0;
    if( compare )
    {
        analysis::AnalysisUInt64Pair previous, current;
        while( input.read( reinterpret_cast<char*>( &current ), sizeof( current ) ) )
        {
            if( read != 0 && ( current.key < previous.key ||
                ( current.key == previous.key && current.value < previous.value ) ) )
                throw std::runtime_error( "capacity pair output order mismatch" );
            previous = current;
            read++;
        }
    }
    else
    {
        analysis::AnalysisKeyMetadataRecord previous, current;
        while( input.read( reinterpret_cast<char*>( &current ), sizeof( current ) ) )
        {
            if( read != 0 && Less( current, previous ) )
                throw std::runtime_error( "capacity metadata output order mismatch" );
            previous = current;
            read++;
        }
    }
    if( !input.eof() || read != records ) throw std::runtime_error( "capacity sorted output count mismatch" );
    result.validateMs = Milliseconds( validationStarted );

    input.close();
    std::error_code cleanupError;
    analysis::AnalysisDiskRemove( root, disk, cleanupError, true );
    if( cleanupError || usage->Current() != 0 || workspace->Snapshot().currentBytes != 0 )
        throw std::runtime_error( "capacity cleanup or resource accounting failed" );
    return result;
}

json ResultJson( const ModelResult& value )
{
    return {
        { "records", value.records },
        { "record_bytes", value.recordBytes },
        { "build_ms", value.buildMs }, { "validate_ms", value.validateMs },
        { "final_scratch_bytes", value.finalBytes }, { "disk_peak_bytes", value.peakBytes },
        { "workspace_peak_bytes", value.workspacePeakBytes },
        { "initial_runs", value.sort.initialRuns }, { "merge_passes", value.sort.mergePasses },
        { "sort_read_bytes", value.sort.sourceReadBytes + value.sort.mergeReadBytes },
        { "sort_write_bytes", value.sort.initialRunWriteBytes + value.sort.mergeWriteBytes }
    };
}

}

int main( int argc, char** argv )
{
    try
    {
        if( argc != 4 && argc != 5 )
            throw std::runtime_error( "usage: gpu-aggregation-capacity ZONES PASSES ROOT [high-entropy]" );
        const auto zones = ParseCount( argv[1] );
        const auto passes = ParseCount( argv[2] );
        const bool highEntropy = argc == 5 && std::string_view( argv[4] ) == "high-entropy";
        if( argc == 5 && !highEntropy ) throw std::runtime_error( "unknown distribution" );
        if( zones > UINT64_MAX / 2 || passes > UINT64_MAX / 2 || zones + passes > UINT64_MAX / 2 )
            throw std::overflow_error( "capacity record count overflow" );
        const std::filesystem::path root = argv[3];
        if( std::filesystem::exists( root ) ) throw std::runtime_error( "capacity root already exists" );
        std::filesystem::create_directories( root );

        const auto taxonomy = RunModel( root / "taxonomy", zones, ( zones + 7 ) / 8, highEntropy, false );
        const auto compareRecords = ( zones + passes ) * 2;
        const auto compare = RunModel( root / "compare", compareRecords, 0, highEntropy, true );

        const uint64_t currentTaxonomyFixedLowerBound =
            zones * ( sizeof( CurrentTaxonomyZone ) + 2 * sizeof( std::pair<int64_t, int64_t> ) ) +
            passes * sizeof( std::pair<int64_t, int64_t> );
        const uint64_t currentCompareFixedLowerBound =
            zones * sizeof( analysis::GpuZoneDto ) + compareRecords * sizeof( int64_t );
        const auto result = json {
            { "gpu_zones", zones }, { "explicit_passes", passes },
            { "distribution", highEntropy ? "high-entropy" : "representative" },
            { "current_taxonomy_fixed_lower_bound_bytes", currentTaxonomyFixedLowerBound },
            { "current_compare_fixed_lower_bound_bytes", currentCompareFixedLowerBound },
            { "current_lower_bounds_exclude_string_heap_and_tree_hash_nodes", true },
            { "taxonomy_interval_sort", ResultJson( taxonomy ) },
            { "taxonomy_matched_zone_bitset_bytes", ( zones + 7 ) / 8 },
            { "compare_grouped_metric_sort", ResultJson( compare ) }
        };
        std::error_code cleanupError;
        std::filesystem::remove( root, cleanupError );
        if( cleanupError ) throw std::runtime_error( "capacity root cleanup failed" );
        std::cout << result.dump( 2 ) << '\n';
        return 0;
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

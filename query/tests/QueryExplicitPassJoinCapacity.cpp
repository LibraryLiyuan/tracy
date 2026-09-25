#include "TracyAnalysisDiskBudget.hpp"
#include "TracyAnalysisIoPath.hpp"
#include "TracyPagedRecordStore.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <queue>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace tracy;
using nlohmann::json;

namespace
{

constexpr uint32_t PartitionCount = 16;

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
#pragma pack( pop )

static_assert( sizeof( PassProjection ) == 52 );

uint64_t ParseCount( const char* text )
{
    uint64_t value = 0;
    const auto end = text + std::strlen( text );
    const auto parsed = std::from_chars( text, end, value );
    if( parsed.ec != std::errc() || parsed.ptr != end || value == 0 || value > UINT32_MAX )
        throw std::runtime_error( "count must be between 1 and UINT32_MAX" );
    return value;
}

uint64_t Mix( uint64_t value )
{
    value += 0x9e3779b97f4a7c15ull;
    value = ( value ^ ( value >> 30 ) ) * 0xbf58476d1ce4e5b9ull;
    value = ( value ^ ( value >> 27 ) ) * 0x94d049bb133111ebull;
    return value ^ ( value >> 31 );
}

PassProjection Projection( uint32_t ordinal, bool highEntropy )
{
    PassProjection record;
    record.entityOrdinal = ordinal;
    record.segmentOrdinalAndStable = ( highEntropy ?
        uint32_t( Mix( ordinal ^ 0x13579bdf2468ace0ull ) ) : ordinal + 1 ) & 0x7fffffffu;
    if( ordinal % 3 != 0 ) record.segmentOrdinalAndStable |= 0x80000000u;
    record.commandListId = highEntropy ? Mix( ordinal ^ 0x2468ace013579bdfull ) : ordinal / 8 + 1;
    record.frameId = highEntropy ? Mix( ordinal ^ 0x55aa55aa33cc33ccull ) : ordinal / 100 + 1;
    record.cameraId = highEntropy ? Mix( ordinal ^ 0xaa55aa55cc33cc33ull ) : ordinal % 64 + 1;
    record.viewId = highEntropy ? Mix( ordinal ^ 0xfedcba9876543210ull ) : ordinal % 256 + 1;
    record.referenceToken = highEntropy ? Mix( ordinal ^ 0x0123456789abcdefull ) :
        uint64_t( ordinal ) * 11400714819323198485ull;
    record.taxonomyId = highEntropy ? uint32_t( Mix( ordinal ^ 0x9988776655443322ull ) ) :
        ordinal % 2048 + 1;
    return record;
}

uint64_t Milliseconds( std::chrono::steady_clock::time_point begin )
{
    return uint64_t( std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin ).count() );
}

uint64_t Share( uint64_t total, uint32_t partition )
{
    return total / PartitionCount + ( partition < total % PartitionCount ? 1 : 0 );
}

void WriteBytes( const std::filesystem::path& path, uint64_t bytes,
    const std::shared_ptr<analysis::AnalysisDiskBudget>& disk, uint64_t seed )
{
    analysis::AnalysisDiskGrow( disk, bytes );
    try
    {
        std::ofstream output( analysis::AnalysisIoPath( path ), std::ios::binary );
        if( !output ) throw std::runtime_error( "join model output open failed" );
        std::array<uint64_t, 16384> buffer;
        for( size_t index = 0; index < buffer.size(); index++ ) buffer[index] = Mix( seed + index );
        uint64_t written = 0;
        while( written < bytes )
        {
            const auto take = std::min<uint64_t>( sizeof( buffer ), bytes - written );
            output.write( reinterpret_cast<const char*>( buffer.data() ), std::streamsize( take ) );
            if( !output ) throw std::runtime_error( "join model output write failed" );
            written += take;
        }
        output.flush();
        if( !output ) throw std::runtime_error( "join model output flush failed" );
    }
    catch( ... )
    {
        std::error_code error;
        const auto actual = std::filesystem::file_size( analysis::AnalysisIoPath( path ), error );
        if( disk && disk->usage && ( error || actual < bytes ) ) disk->usage->Release( error ? bytes : bytes - actual );
        throw;
    }
}

}

int main( int argc, char** argv )
{
    try
    {
        if( argc != 3 && argc != 4 )
            throw std::runtime_error( "usage: explicit-pass-join-capacity COUNT ROOT [high-entropy]" );
        const auto count = ParseCount( argv[1] );
        const bool highEntropy = argc == 4 && std::string_view( argv[3] ) == "high-entropy";
        if( argc == 4 && !highEntropy ) throw std::runtime_error( "unknown distribution" );
        const auto relationCount = ( count * 103848473ull + 62853045ull - 1 ) / 62853045ull;
        const std::filesystem::path root = argv[2];
        if( std::filesystem::exists( root ) ) throw std::runtime_error( "capacity root already exists" );
        std::filesystem::create_directories( root / "input" );
        std::filesystem::create_directories( root / "runs" );

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

        const auto started = std::chrono::steady_clock::now();
        for( uint32_t partition = 0; partition < PartitionCount; partition++ )
        {
            WriteBytes( root / "input" / ( "entities-" + std::to_string( partition ) + ".bin" ),
                Share( count, partition ) * 16, disk, partition );
            WriteBytes( root / "input" / ( "relations-" + std::to_string( partition ) + ".bin" ),
                Share( relationCount, partition ) * 24, disk, 100 + partition );
        }
        const auto inputBytes = usage->Current();

        for( uint32_t partition = 0; partition < PartitionCount; partition++ )
        {
            const auto runPath = root / "runs" / ( std::to_string( partition ) + ".bin" );
            const auto records = Share( count, partition );
            analysis::AnalysisDiskGrow( disk, records * sizeof( PassProjection ) );
            std::ofstream output( analysis::AnalysisIoPath( runPath ), std::ios::binary );
            if( !output ) throw std::runtime_error( "projection run open failed" );
            uint64_t produced = 0;
            for( uint64_t ordinal = partition; ordinal < count; ordinal += PartitionCount )
            {
                const auto record = Projection( uint32_t( ordinal ), highEntropy );
                output.write( reinterpret_cast<const char*>( &record ), sizeof( record ) );
                if( !output ) throw std::runtime_error( "projection run write failed" );
                produced++;
            }
            output.flush();
            if( !output || produced != records ) throw std::runtime_error( "projection run count mismatch" );
            output.close();
            std::error_code error;
            analysis::AnalysisDiskRemove( root / "input" / ( "entities-" + std::to_string( partition ) + ".bin" ), disk, error );
            if( error ) throw std::runtime_error( "entity partition cleanup failed" );
            analysis::AnalysisDiskRemove( root / "input" / ( "relations-" + std::to_string( partition ) + ".bin" ), disk, error );
            if( error ) throw std::runtime_error( "relation partition cleanup failed" );
        }
        const auto runBytes = usage->Current();

        struct Head { PassProjection record; uint32_t partition = 0; };
        const auto later = []( const Head& lhs, const Head& rhs ) {
            return lhs.record.entityOrdinal > rhs.record.entityOrdinal;
        };
        std::priority_queue<Head, std::vector<Head>, decltype( later )> queue( later );
        std::array<std::ifstream, PartitionCount> inputs;
        const auto read = [&]( uint32_t partition, PassProjection& record ) {
            auto& input = inputs[partition];
            input.read( reinterpret_cast<char*>( &record ), sizeof( record ) );
            if( input.gcount() == sizeof( record ) ) return true;
            if( input.gcount() != 0 || !input.eof() ) throw std::runtime_error( "projection run read failed" );
            return false;
        };
        for( uint32_t partition = 0; partition < PartitionCount; partition++ )
        {
            inputs[partition].open( analysis::AnalysisIoPath( root / "runs" /
                ( std::to_string( partition ) + ".bin" ) ), std::ios::binary );
            if( !inputs[partition] ) throw std::runtime_error( "projection run open failed" );
            PassProjection record;
            if( read( partition, record ) ) queue.push( { record, partition } );
        }
        const std::string identity( 64, '5' );
        uint64_t merged = 0;
        {
            analysis::PagedRecordWriter projections( root / "projections", identity,
                sizeof( PassProjection ), options );
            std::vector<PassProjection> output;
            output.reserve( 4096 );
            while( !queue.empty() )
            {
                auto head = queue.top(); queue.pop();
                if( head.record.entityOrdinal != merged ) throw std::runtime_error( "projection merge order mismatch" );
                output.push_back( head.record );
                merged++;
                if( output.size() == output.capacity() )
                {
                    projections.Append( output.data(), output.size() );
                    output.clear();
                }
                PassProjection next;
                if( read( head.partition, next ) ) queue.push( { next, head.partition } );
            }
            if( !output.empty() ) projections.Append( output.data(), output.size() );
            projections.Commit();
        }
        if( merged != count ) throw std::runtime_error( "projection merge count mismatch" );
        const auto mergePeak = usage->Snapshot().peakBytes;
        for( uint32_t partition = 0; partition < PartitionCount; partition++ )
        {
            inputs[partition].close();
            std::error_code error;
            analysis::AnalysisDiskRemove( root / "runs" / ( std::to_string( partition ) + ".bin" ), disk, error );
            if( error ) throw std::runtime_error( "projection run cleanup failed" );
        }
        const auto finalProjectionBytes = analysis::AnalysisDiskUsage::Bytes( root / "projections" );
        const auto buildMs = Milliseconds( started );

        const auto validateStarted = std::chrono::steady_clock::now();
        {
            analysis::PagedRecordReader reader( root / "projections", identity,
                sizeof( PassProjection ), options );
            reader.ValidateAll();
            if( reader.Count() != count ) throw std::runtime_error( "projection count changed" );
        }
        const auto validateMs = Milliseconds( validateStarted );

        const auto workspaceSnapshot = workspace->Snapshot();
        const auto result = json {
            { "entities", count }, { "relations", relationCount },
            { "distribution", highEntropy ? "high-entropy" : "representative" },
            { "partitions", PartitionCount }, { "projection_record_bytes", sizeof( PassProjection ) },
            { "initial_input_bytes", inputBytes }, { "projection_run_bytes", runBytes },
            { "final_projection_bytes", finalProjectionBytes }, { "disk_peak_bytes", mergePeak },
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

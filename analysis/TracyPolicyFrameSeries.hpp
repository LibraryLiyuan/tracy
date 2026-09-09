#ifndef __TRACYPOLICYFRAMESERIES_HPP__
#define __TRACYPOLICYFRAMESERIES_HPP__

#include "TracyCandidatePolicy.hpp"
#include "TracyHash.hpp"
#include "TracyAnalysisDiskBudget.hpp"
#include <array>
#include <bit>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace tracy::analysis
{
// Local neutral cache, schema 1: little-endian fixed-width records. Each
// signature extent is independently checksummed, so profile-only re-evaluation
// loads and verifies one signature, never the entire event matrix.
using PolicySeriesRecord = std::array<uint64_t, 7>;
static_assert( sizeof( PolicySeriesRecord ) == 56 );
static_assert( std::endian::native == std::endian::little );

inline void WritePolicyFrameSeries( std::ostream& output, PolicySignatureContext& context,
    const std::vector<NeutralMergedFrameValues>& frames,
    const std::shared_ptr<AnalysisDiskBudget>& disk={} )
{
    if(frames.size()>UINT64_MAX/sizeof(PolicySeriesRecord)) throw std::runtime_error("analysis_scan_cache_disk_budget");
    AnalysisDiskGrow(disk,uint64_t(frames.size())*sizeof(PolicySeriesRecord));
    const auto offset = output.tellp();
    if( offset < 0 ) throw std::runtime_error( "frame_series_position_failed" );
    context.seriesOffset = uint64_t( offset );
    context.seriesCount = frames.size();
    Sha256Builder hash;
    for( const auto& frame : frames )
    {
        PolicySeriesRecord row { frame.frameIndex, uint64_t( frame.inclusiveNs ),
            uint64_t( frame.exclusiveNs ), uint64_t( frame.waitNs ), uint64_t( frame.criticalPathNs ),
            frame.occurrenceCount, frame.exact ? 1ULL : 0ULL };
        output.write( reinterpret_cast<const char*>( row.data() ), sizeof( row ) );
        hash.Update( row.data(), sizeof( row ) );
    }
    if( !output ) throw std::runtime_error( "frame_series_write_failed" );
    context.seriesSha256 = hash.FinalHex();
    context.frameSeriesComplete = true;
}

inline std::vector<PolicyFrameEvidence> ReadPolicyFrameSeries( const std::filesystem::path& path,
    const PolicySignatureContext& context, uint64_t memoryBudgetBytes,
    const std::function<bool()>& cancelled = {} )
{
    if( context.seriesCount == 0 ) return context.frames;
    constexpr uint64_t residentPerRow = sizeof( PolicyFrameEvidence ) * 4;
    if( context.seriesCount > memoryBudgetBytes / residentPerRow )
        throw std::runtime_error( "frame_series_memory_budget" );
    const auto size = std::filesystem::file_size( path );
    if( context.seriesOffset > size || context.seriesCount > ( size-context.seriesOffset ) / sizeof( PolicySeriesRecord ) )
        throw std::runtime_error( "frame_series_extent_invalid" );
    std::ifstream file( path, std::ios::binary );
    file.seekg( std::streamoff( context.seriesOffset ) );
    std::vector<PolicyFrameEvidence> result;
    result.reserve( size_t( context.seriesCount ) );
    Sha256Builder hash;
    for( uint64_t i = 0; i < context.seriesCount; ++i )
    {
        if( ( i % 4096 == 0 ) && cancelled && cancelled() ) throw std::runtime_error( "cancelled" );
        PolicySeriesRecord row;
        file.read( reinterpret_cast<char*>( row.data() ), sizeof( row ) );
        if( !file ) throw std::runtime_error( "frame_series_read_failed" );
        hash.Update( row.data(), sizeof( row ) );
        size_t field = context.metricPreference == "inclusive" ? 1 :
            context.metricPreference == "wait" ? 3 : context.metricPreference == "critical" ? 4 : 2;
        if( row[6] > 1 || ( row[6] == 1 && row[field] > uint64_t( INT64_MAX ) ) )
            throw std::runtime_error( "frame_series_value_invalid" );
        // Invalid source values stay unknown; the placeholder is never ranked
        // or included in distributions because exact remains false.
        result.push_back( { row[0], row[6] == 1 ? int64_t( row[field] ) : 0, {}, {}, row[6] == 1 } );
    }
    if( hash.FinalHex() != context.seriesSha256 ) throw std::runtime_error( "frame_series_checksum_mismatch" );
    return result;
}
}
#endif

#ifndef __TRACYPOLICYFRAMESERIES_HPP__
#define __TRACYPOLICYFRAMESERIES_HPP__

#include "TracyCandidatePolicy.hpp"
#include "TracyHash.hpp"
#include "TracyAnalysisDiskBudget.hpp"
#include <array>
#include <bit>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
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

// The rows WritePolicyFrameSeries writes for these frames. Sets the context's
// series count, hash and completeness; the caller writes the rows and sets
// the offset they were written at.
inline std::string EncodePolicyFrameSeries( PolicySignatureContext& context,
    const std::vector<NeutralMergedFrameValues>& frames )
{
    if(frames.size()>UINT64_MAX/sizeof(PolicySeriesRecord)) throw std::runtime_error("analysis_scan_cache_disk_budget");
    std::string bytes( frames.size() * sizeof( PolicySeriesRecord ), '\0' );
    for( size_t i = 0; i < frames.size(); ++i )
    {
        const auto& frame = frames[i];
        const PolicySeriesRecord row { frame.frameIndex, uint64_t( frame.inclusiveNs ),
            uint64_t( frame.exclusiveNs ), uint64_t( frame.waitNs ), uint64_t( frame.criticalPathNs ),
            frame.occurrenceCount, frame.exact ? 1ULL : 0ULL };
        std::memcpy( bytes.data() + i * sizeof( row ), row.data(), sizeof( row ) );
    }
    Sha256Builder hash;
    hash.Update( bytes.data(), bytes.size() );
    context.seriesCount = frames.size();
    context.seriesSha256 = hash.FinalHex();
    context.frameSeriesComplete = true;
    return bytes;
}

// A frame root's timeline in the same rows: the frame value fills every
// metric column, so the context's metric preference reads it back. Only the
// frame index, value and exactness are kept; the timeline's evidence carries
// no references, structure key or time range.
inline void WritePolicyFrameSeries( std::ostream& output, PolicySignatureContext& context,
    const std::vector<PolicyFrameEvidence>& frames, const std::shared_ptr<AnalysisDiskBudget>& disk = {} )
{
    if(frames.size()>UINT64_MAX/sizeof(PolicySeriesRecord)) throw std::runtime_error("analysis_scan_cache_disk_budget");
    for( const auto& frame : frames )
        if( !frame.eventRefs.empty() || !frame.structureKey.empty() || frame.beginNs || frame.endNs || frame.valueNs < 0 )
            throw std::runtime_error( "frame_series_timeline_unsupported" );
    AnalysisDiskGrow(disk,uint64_t(frames.size())*sizeof(PolicySeriesRecord));
    const auto offset = output.tellp();
    if( offset < 0 ) throw std::runtime_error( "frame_series_position_failed" );
    context.seriesOffset = uint64_t( offset );
    context.seriesCount = frames.size();
    Sha256Builder hash;
    for( const auto& frame : frames )
    {
        const auto value = uint64_t( frame.valueNs );
        PolicySeriesRecord row { frame.frameIndex, value, value, value, value, 0, frame.exact ? 1ULL : 0ULL };
        output.write( reinterpret_cast<const char*>( row.data() ), sizeof( row ) );
        hash.Update( row.data(), sizeof( row ) );
    }
    if( !output ) throw std::runtime_error( "frame_series_write_failed" );
    context.seriesSha256 = hash.FinalHex();
    context.frameSeriesComplete = true;
}

// Reads signature extents of one finished series file. The file is opened and
// sized once; each extent is read and hashed in blocks of 4096 rows. Several
// threads may read at once; block reads take turns on the file.
class PolicyFrameSeriesReader
{
public:
    explicit PolicyFrameSeriesReader( std::filesystem::path path ) : m_path( std::move( path ) ) {}

    std::vector<PolicyFrameEvidence> Read( const PolicySignatureContext& context, uint64_t memoryBudgetBytes,
        const std::function<bool()>& cancelled = {} )
    {
        if( context.seriesCount == 0 ) return context.frames;
        constexpr uint64_t residentPerRow = sizeof( PolicyFrameEvidence ) * 4;
        if( context.seriesCount > memoryBudgetBytes / residentPerRow )
            throw std::runtime_error( "frame_series_memory_budget" );
        uint64_t size = 0;
        {
            std::lock_guard lock( m_mutex );
            if( !m_file.is_open() )
            {
                m_size = std::filesystem::file_size( m_path );
                m_file.open( m_path, std::ios::binary );
            }
            size = m_size;
        }
        if( context.seriesOffset > size || context.seriesCount > ( size-context.seriesOffset ) / sizeof( PolicySeriesRecord ) )
            throw std::runtime_error( "frame_series_extent_invalid" );
        std::vector<PolicyFrameEvidence> result;
        result.reserve( size_t( context.seriesCount ) );
        const size_t field = context.metricPreference == "inclusive" ? 1 :
            context.metricPreference == "wait" ? 3 : context.metricPreference == "critical" ? 4 : 2;
        constexpr uint64_t BlockRows = 4096;
        std::vector<PolicySeriesRecord> block( size_t( std::min( context.seriesCount, BlockRows ) ) );
        Sha256Builder hash;
        for( uint64_t first = 0; first < context.seriesCount; first += BlockRows )
        {
            if( cancelled && cancelled() ) throw std::runtime_error( "cancelled" );
            const auto wanted = size_t( std::min( BlockRows, context.seriesCount - first ) );
            size_t rows = 0;
            {
                std::lock_guard lock( m_mutex );
                m_file.clear();
                m_file.seekg( std::streamoff( context.seriesOffset + first * sizeof( PolicySeriesRecord ) ) );
                m_file.read( reinterpret_cast<char*>( block.data() ), std::streamsize( wanted * sizeof( PolicySeriesRecord ) ) );
                rows = size_t( m_file.gcount() ) / sizeof( PolicySeriesRecord );
            }
            hash.Update( block.data(), rows * sizeof( PolicySeriesRecord ) );
            for( size_t i = 0; i < rows; ++i )
            {
                const auto& row = block[i];
                if( row[6] > 1 || ( row[6] == 1 && row[field] > uint64_t( INT64_MAX ) ) )
                    throw std::runtime_error( "frame_series_value_invalid" );
                // Invalid source values stay unknown; the placeholder is never ranked
                // or included in distributions because exact remains false.
                result.push_back( { row[0], row[6] == 1 ? int64_t( row[field] ) : 0, {}, {}, row[6] == 1 } );
            }
            if( rows != wanted ) throw std::runtime_error( "frame_series_read_failed" );
        }
        if( hash.FinalHex() != context.seriesSha256 ) throw std::runtime_error( "frame_series_checksum_mismatch" );
        return result;
    }

private:
    std::filesystem::path m_path;
    std::mutex m_mutex;
    std::ifstream m_file;
    uint64_t m_size = 0;
};

inline std::vector<PolicyFrameEvidence> ReadPolicyFrameSeries( const std::filesystem::path& path,
    const PolicySignatureContext& context, uint64_t memoryBudgetBytes,
    const std::function<bool()>& cancelled = {} )
{
    return PolicyFrameSeriesReader( path ).Read( context, memoryBudgetBytes, cancelled );
}
}
#endif

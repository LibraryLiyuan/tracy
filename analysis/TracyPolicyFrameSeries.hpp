#ifndef __TRACYPOLICYFRAMESERIES_HPP__
#define __TRACYPOLICYFRAMESERIES_HPP__

#include "TracyCandidatePolicy.hpp"
#include "TracyHash.hpp"
#include "TracyAnalysisDiskBudget.hpp"
#include "TracyPagedRecordStore.hpp"
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

inline void WritePolicyFrameSeries( PagedRecordWriter& output, PolicySignatureContext& context,
    const std::vector<NeutralMergedFrameValues>& frames,
    const std::shared_ptr<AnalysisDiskBudget>& = {} )
{
    if(output.Count()>UINT64_MAX/sizeof(PolicySeriesRecord)) throw std::runtime_error("frame_series_position_failed");
    context.seriesOffset=output.Count()*sizeof(PolicySeriesRecord);
    context.seriesCount=frames.size();
    Sha256Builder hash;
    for(const auto& frame:frames) {
        const PolicySeriesRecord row{frame.frameIndex,uint64_t(frame.inclusiveNs),uint64_t(frame.exclusiveNs),
            uint64_t(frame.waitNs),uint64_t(frame.criticalPathNs),frame.occurrenceCount,frame.exact?1ULL:0ULL};
        output.Append(row.data(),1); hash.Update(row.data(),sizeof(row));
    }
    context.seriesSha256=hash.FinalHex(); context.frameSeriesComplete=true;
}

// Frame-root timelines already own one complete PolicyFrameEvidence vector.
// Persist that vector directly so the root Context can reference the same
// immutable series without retaining a second in-memory copy.
inline void WritePolicyFrameSeries( PagedRecordWriter& output, PolicySignatureContext& context,
    const std::vector<PolicyFrameEvidence>& frames,
    const std::shared_ptr<AnalysisDiskBudget>& = {} )
{
    if(output.Count()>UINT64_MAX/sizeof(PolicySeriesRecord)) throw std::runtime_error("frame_series_position_failed");
    context.seriesOffset=output.Count()*sizeof(PolicySeriesRecord);
    context.seriesCount=frames.size();
    Sha256Builder hash;
    for(const auto& frame:frames) {
        if(frame.exact && frame.valueNs<0) throw std::runtime_error("frame_series_value_invalid");
        const auto value=frame.exact?uint64_t(frame.valueNs):0;
        const PolicySeriesRecord row{frame.frameIndex,value,value,0,0,1,frame.exact?1ULL:0ULL};
        output.Append(row.data(),1); hash.Update(row.data(),sizeof(row));
    }
    context.seriesSha256=hash.FinalHex(); context.frameSeriesComplete=true;
}

inline std::vector<PolicyFrameEvidence> ReadPolicyFrameSeries( const std::filesystem::path& path,
    const PolicySignatureContext& context, uint64_t memoryBudgetBytes,
    const std::function<bool()>& cancelled = {}, const std::string& identity = {} )
{
    if( context.seriesCount == 0 ) return context.frames;
    constexpr uint64_t residentPerRow = sizeof( PolicyFrameEvidence ) * 4;
    if( context.seriesCount > memoryBudgetBytes / residentPerRow )
        throw std::runtime_error( "frame_series_memory_budget" );
    std::unique_ptr<PagedRecordReader> paged;
    if(std::filesystem::is_directory(path)) {
        PagedRecordOptions options; options.cancelled=cancelled;
        options.blockBytes=uint32_t(std::min<uint64_t>(4*1024*1024,memoryBudgetBytes/4));
        paged=std::make_unique<PagedRecordReader>(path,identity,sizeof(PolicySeriesRecord),options);
        if(paged->Count()>UINT64_MAX/sizeof(PolicySeriesRecord)) throw std::runtime_error("frame_series_extent_invalid");
    }
    const auto size = paged ? paged->Count()*sizeof(PolicySeriesRecord) : std::filesystem::file_size( path );
    if(context.seriesOffset%sizeof(PolicySeriesRecord)) throw std::runtime_error("frame_series_extent_invalid");
    if( context.seriesOffset > size || context.seriesCount > ( size-context.seriesOffset ) / sizeof( PolicySeriesRecord ) )
        throw std::runtime_error( "frame_series_extent_invalid" );
    std::ifstream file;
    if(!paged) { file.open(path,std::ios::binary); file.seekg(std::streamoff(context.seriesOffset)); }
    std::vector<PolicyFrameEvidence> result;
    result.reserve( size_t( context.seriesCount ) );
    Sha256Builder hash;
    constexpr uint64_t ChunkRecords=4096;
    static_assert(residentPerRow>=sizeof(PolicyFrameEvidence)+sizeof(PolicySeriesRecord));
    // The caller reserves at least 256 B per declared row before entering;
    // this bounded chunk plus the returned vector fit that admission charge.
    std::vector<PolicySeriesRecord> chunk(size_t(std::min<uint64_t>(context.seriesCount,ChunkRecords)));
    for(uint64_t offset=0;offset<context.seriesCount;)
    {
        if(cancelled && cancelled()) throw std::runtime_error("cancelled");
        const auto count=std::min<uint64_t>(chunk.size(),context.seriesCount-offset);
        const auto bytes=size_t(count*sizeof(PolicySeriesRecord));
        if(paged) paged->Read(context.seriesOffset/sizeof(PolicySeriesRecord)+offset,chunk.data(),count);
        else {
            file.read(reinterpret_cast<char*>(chunk.data()),std::streamsize(bytes));
            if( !file ) throw std::runtime_error( "frame_series_read_failed" );
        }
        hash.Update(chunk.data(),bytes);
        for(uint64_t i=0;i<count;++i) {
            const auto& row=chunk[size_t(i)];
            const size_t field=context.metricPreference=="inclusive"?1:
                context.metricPreference=="wait"?3:context.metricPreference=="critical"?4:2;
            if(row[6]>1 || (row[6]==1 && row[field]>uint64_t(INT64_MAX)))
                throw std::runtime_error("frame_series_value_invalid");
            // Invalid source values stay unknown; absent rows are interpreted
            // only by the later FrameSet policy, never by this decoder.
            result.push_back({row[0],row[6]==1?int64_t(row[field]):0,{},{},row[6]==1});
        }
        offset+=count;
    }
    if( hash.FinalHex() != context.seriesSha256 ) throw std::runtime_error( "frame_series_checksum_mismatch" );
    return result;
}
}
#endif

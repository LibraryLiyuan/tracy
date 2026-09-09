#include "TracyAnalysisExternalSort.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <queue>
#include <vector>

namespace tracy::analysis
{
namespace
{

constexpr size_t MergeFanIn = 64;

bool PairLess( const AnalysisUInt64Pair& lhs, const AnalysisUInt64Pair& rhs )
{
    return lhs.key < rhs.key || ( lhs.key == rhs.key && lhs.value < rhs.value );
}

bool MergeRuns( const std::vector<std::filesystem::path>& inputs,
    const std::filesystem::path& output, uint64_t& written, std::string& error,
    const std::shared_ptr<AnalysisDiskBudget>& disk )
{
    struct Cursor
    {
        std::ifstream input;
        AnalysisUInt64Pair value;
    };
    struct Head
    {
        AnalysisUInt64Pair value;
        size_t run = 0;
    };
    struct Greater
    {
        bool operator()( const Head& lhs, const Head& rhs ) const
        {
            if( PairLess( rhs.value, lhs.value ) ) return true;
            if( PairLess( lhs.value, rhs.value ) ) return false;
            return lhs.run > rhs.run;
        }
    };

    std::ofstream out( output, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "analysis_pair_sort_merge_output_open_failed"; return false; }
    std::vector<Cursor> cursors( inputs.size() );
    std::priority_queue<Head, std::vector<Head>, Greater> heap;
    for( size_t index = 0; index < inputs.size(); ++index )
    {
        cursors[index].input.open( inputs[index], std::ios::binary );
        if( !cursors[index].input )
        { error = "analysis_pair_sort_run_open_failed"; return false; }
        if( cursors[index].input.read( reinterpret_cast<char*>( &cursors[index].value ),
            sizeof( AnalysisUInt64Pair ) ) )
            heap.push( { cursors[index].value, index } );
        else if( !cursors[index].input.eof() )
        { error = "analysis_pair_sort_run_read_failed"; return false; }
    }

    written = 0;
    while( !heap.empty() )
    {
        const auto head = heap.top();
        heap.pop();
        AnalysisDiskGrow(disk,sizeof(head.value));
        out.write( reinterpret_cast<const char*>( &head.value ), sizeof( head.value ) );
        if( !out ) { error = "analysis_pair_sort_merge_write_failed"; return false; }
        ++written;
        auto& cursor = cursors[head.run];
        if( cursor.input.read( reinterpret_cast<char*>( &cursor.value ), sizeof( cursor.value ) ) )
            heap.push( { cursor.value, head.run } );
        else if( !cursor.input.eof() )
        { error = "analysis_pair_sort_run_read_failed"; return false; }
    }
    out.flush();
    if( !out ) { error = "analysis_pair_sort_merge_flush_failed"; return false; }
    out.close();
    for( auto& cursor : cursors ) cursor.input.close();
    return true;
}

bool RemoveFiles( const std::vector<std::filesystem::path>& paths, std::string& error,
    const std::shared_ptr<AnalysisDiskBudget>& disk )
{
    std::error_code ec;
    for( const auto& path : paths )
    {
        ec.clear();
        const bool existed=std::filesystem::exists(path,ec);
        if(!ec) AnalysisDiskRemove(path,disk,ec);
        if( !existed || ec )
        {
            error = "analysis_pair_sort_cleanup_failed:" +
                ( ec ? ec.message() : path.filename().string() );
            return false;
        }
    }
    return true;
}

}

bool CleanupAnalysisExternalSortFiles( const std::filesystem::path& temporaryRoot,
    const std::string& prefix, std::string& error,const std::shared_ptr<AnalysisDiskBudget>& disk )
{
    error.clear();
    std::error_code ec;
    if( !std::filesystem::exists( temporaryRoot, ec ) )
        return !ec;
    for( const auto& entry : std::filesystem::directory_iterator( temporaryRoot, ec ) )
    {
        if( ec ) { error = "analysis_pair_sort_cleanup_scan_failed:" + ec.message(); return false; }
        const auto name = entry.path().filename().string();
        if( !name.starts_with( prefix ) || !name.ends_with( ".work" ) ) continue;
        ec.clear();
        AnalysisDiskRemove(entry.path(),disk,ec);
        if( ec )
        {
            error = "analysis_pair_sort_cleanup_failed:" +
                ( ec ? ec.message() : entry.path().filename().string() );
            return false;
        }
    }
    if( ec ) { error = "analysis_pair_sort_cleanup_scan_failed:" + ec.message(); return false; }
    return true;
}

bool SortAnalysisUInt64Pairs( const std::filesystem::path& source,
    const std::filesystem::path& output, const std::filesystem::path& temporaryRoot,
    const std::string& prefix, uint64_t expectedCount, uint64_t maximumBufferedPairs,
    std::string& error,const std::shared_ptr<AnalysisDiskBudget>& disk )
{
    error.clear();
    if( maximumBufferedPairs == 0 ) maximumBufferedPairs = 1;
    maximumBufferedPairs = std::min<uint64_t>( maximumBufferedPairs,
        std::numeric_limits<size_t>::max() );
    std::error_code ec;
    const auto bytes = std::filesystem::file_size( source, ec );
    if( ec ) { error = "analysis_pair_sort_source_size_failed:" + ec.message(); return false; }
    if( expectedCount > std::numeric_limits<uint64_t>::max() / sizeof( AnalysisUInt64Pair ) ||
        bytes != expectedCount * sizeof( AnalysisUInt64Pair ) )
    { error = "analysis_pair_sort_source_size_mismatch"; return false; }
    if( !CleanupAnalysisExternalSortFiles( temporaryRoot, prefix + "-run-", error,disk ) ) return false;

    std::ifstream in( source, std::ios::binary );
    if( !in ) { error = "analysis_pair_sort_source_open_failed"; return false; }
    std::vector<std::filesystem::path> runs;
    uint64_t remaining = expectedCount;
    size_t runIndex = 0;
    while( remaining != 0 )
    {
        const auto take = size_t( std::min<uint64_t>( remaining, maximumBufferedPairs ) );
        std::vector<AnalysisUInt64Pair> values( take );
        in.read( reinterpret_cast<char*>( values.data() ),
            std::streamsize( values.size() * sizeof( AnalysisUInt64Pair ) ) );
        if( !in ) { error = "analysis_pair_sort_source_truncated"; return false; }
        std::sort( values.begin(), values.end(), PairLess );
        const auto run = temporaryRoot / ( prefix + "-run-0-" +
            std::to_string( runIndex++ ) + ".work" );
        AnalysisDiskGrow(disk,uint64_t(values.size())*sizeof(AnalysisUInt64Pair));
        std::ofstream out( run, std::ios::binary | std::ios::trunc );
        if( !out ) { error = "analysis_pair_sort_run_write_open_failed"; return false; }
        out.write( reinterpret_cast<const char*>( values.data() ),
            std::streamsize( values.size() * sizeof( AnalysisUInt64Pair ) ) );
        out.flush();
        if( !out ) { error = "analysis_pair_sort_run_write_failed"; return false; }
        out.close();
        runs.emplace_back( run );
        remaining -= take;
    }
    in.close();

    if( runs.empty() )
    {
        const auto previous=disk && disk->usage?AnalysisDiskUsage::Bytes(output):0;
        std::ofstream out( output, std::ios::binary | std::ios::trunc );
        if( !out ) { error = "analysis_pair_sort_output_open_failed"; return false; }
        if(disk && disk->usage) disk->usage->Release(previous);
        return true;
    }

    size_t pass = 1;
    while( runs.size() > 1 )
    {
        std::vector<std::filesystem::path> next;
        for( size_t first = 0; first < runs.size(); first += MergeFanIn )
        {
            const auto last = std::min( first + MergeFanIn, runs.size() );
            const std::vector<std::filesystem::path> group( runs.begin() + first, runs.begin() + last );
            const auto merged = temporaryRoot / ( prefix + "-run-" +
                std::to_string( pass ) + "-" + std::to_string( next.size() ) + ".work" );
            uint64_t written = 0;
            if( !MergeRuns( group, merged, written, error,disk ) ) return false;
            uint64_t expected = 0;
            for( const auto& path : group )
            {
                const auto size = std::filesystem::file_size( path, ec );
                if( ec || size % sizeof( AnalysisUInt64Pair ) != 0 )
                { error = "analysis_pair_sort_run_size_invalid"; return false; }
                expected += size / sizeof( AnalysisUInt64Pair );
            }
            if( written != expected )
            { error = "analysis_pair_sort_merge_count_mismatch"; return false; }
            if( !RemoveFiles( group, error,disk ) ) return false;
            next.emplace_back( merged );
        }
        runs = std::move( next );
        ++pass;
    }

    AnalysisDiskRemove(output,disk,ec);
    ec.clear();
    std::filesystem::rename( runs.front(), output, ec );
    if( ec ) { error = "analysis_pair_sort_publish_failed:" + ec.message(); return false; }
    const auto outputBytes = std::filesystem::file_size( output, ec );
    if( ec || outputBytes != expectedCount * sizeof( AnalysisUInt64Pair ) )
    { error = "analysis_pair_sort_output_size_mismatch"; return false; }
    return true;
}

}

#include "TracyAnalysisExternalSort.hpp"

#include <algorithm>
#include <fstream>
#include <functional>
#include <limits>
#include <queue>
#include <string_view>
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

bool MetadataLess( const AnalysisKeyMetadataRecord& lhs, const AnalysisKeyMetadataRecord& rhs )
{
    if( lhs.keyHigh != rhs.keyHigh ) return lhs.keyHigh < rhs.keyHigh;
    if( lhs.keyLow != rhs.keyLow ) return lhs.keyLow < rhs.keyLow;
    if( lhs.kind != rhs.kind ) return lhs.kind < rhs.kind;
    if( lhs.metadata != rhs.metadata ) return lhs.metadata < rhs.metadata;
    return lhs.auxiliary < rhs.auxiliary;
}

struct FileChargeReconcile
{
    std::ofstream& stream;
    const std::filesystem::path& path;
    const std::shared_ptr<AnalysisDiskBudget>& disk;
    const uint64_t& charged;

    ~FileChargeReconcile()
    {
        stream.close();
        if( !disk || !disk->usage ) return;
        try
        {
            const auto actual = AnalysisDiskUsage::Bytes( path );
            if( charged > actual ) disk->usage->Release( charged - actual );
        }
        catch( ... ) {} // Retain a conservative charge until the next idle recount.
    }
};

template<typename Record, typename Less>
bool MergeRuns( const std::vector<std::filesystem::path>& inputs,
    const std::filesystem::path& output, uint64_t& written, std::string& error,
    const std::shared_ptr<AnalysisDiskBudget>& disk,
    const std::shared_ptr<AnalysisWorkspaceBudget>& workspace,
    Less less, std::string_view errorPrefix, const std::function<bool()>& cancelled = {} )
{
    AnalysisWorkspaceReservation mergeWorkspace(workspace,65536+uint64_t(inputs.size())*2048);
    struct Cursor
    {
        std::ifstream input;
        Record value;
    };
    struct Head
    {
        Record value;
        size_t run = 0;
    };
    struct Greater
    {
        Less less;
        bool operator()( const Head& lhs, const Head& rhs ) const
        {
            if( less( rhs.value, lhs.value ) ) return true;
            if( less( lhs.value, rhs.value ) ) return false;
            return lhs.run > rhs.run;
        }
    };

    std::ofstream out( output, std::ios::binary | std::ios::trunc );
    if( !out ) { error = std::string( errorPrefix ) + "_merge_output_open_failed"; return false; }
    uint64_t chargedBytes = 0;
    FileChargeReconcile reconcile { out, output, disk, chargedBytes };
    constexpr size_t OutputBatchRecords = 4096;
    AnalysisWorkspaceReservation outputWorkspace( workspace, OutputBatchRecords * sizeof( Record ) );
    std::vector<Record> outputBatch;
    outputBatch.reserve( OutputBatchRecords );
    const auto flushOutput = [&] {
        if( outputBatch.empty() ) return true;
        const auto bytes = outputBatch.size() * sizeof( Record );
        AnalysisDiskGrow( disk, bytes );
        chargedBytes += bytes;
        out.write( reinterpret_cast<const char*>( outputBatch.data() ), std::streamsize( bytes ) );
        if( !out ) { error = std::string( errorPrefix ) + "_merge_write_failed"; return false; }
        outputBatch.clear();
        return true;
    };
    std::vector<Cursor> cursors( inputs.size() );
    std::priority_queue<Head, std::vector<Head>, Greater> heap( Greater { less } );
    for( size_t index = 0; index < inputs.size(); ++index )
    {
        if( cancelled && cancelled() )
        { error = std::string( errorPrefix ) + "_cancelled"; return false; }
        cursors[index].input.open( inputs[index], std::ios::binary );
        if( !cursors[index].input )
        { error = std::string( errorPrefix ) + "_run_open_failed"; return false; }
        if( cursors[index].input.read( reinterpret_cast<char*>( &cursors[index].value ),
            sizeof( Record ) ) )
            heap.push( { cursors[index].value, index } );
        else if( !cursors[index].input.eof() )
        { error = std::string( errorPrefix ) + "_run_read_failed"; return false; }
    }

    written = 0;
    while( !heap.empty() )
    {
        if( ( written & 4095 ) == 0 && cancelled && cancelled() )
        { error = std::string( errorPrefix ) + "_cancelled"; return false; }
        const auto head = heap.top();
        heap.pop();
        outputBatch.push_back( head.value );
        if( outputBatch.size() == OutputBatchRecords && !flushOutput() ) return false;
        ++written;
        auto& cursor = cursors[head.run];
        if( cursor.input.read( reinterpret_cast<char*>( &cursor.value ), sizeof( Record ) ) )
            heap.push( { cursor.value, head.run } );
        else if( !cursor.input.eof() )
        { error = std::string( errorPrefix ) + "_run_read_failed"; return false; }
    }
    if( !flushOutput() ) return false;
    out.flush();
    if( !out ) { error = std::string( errorPrefix ) + "_merge_flush_failed"; return false; }
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

uint64_t ChooseAnalysisExternalSortBufferRecords( uint64_t inputRecords, uint32_t recordBytes,
    const std::shared_ptr<AnalysisWorkspaceBudget>& workspace )
{
    if( recordBytes == 0 ) throw std::runtime_error( "analysis_metadata_sort_record_width" );
    constexpr uint64_t DefaultRecords = 512 * 1024;
    constexpr uint64_t MaximumBufferBytes = 512ull << 20;
    constexpr uint64_t DesiredRuns = 64;
    const auto desired = inputRecords / DesiredRuns + ( inputRecords % DesiredRuns != 0 );
    const auto target = std::min( std::max<uint64_t>( inputRecords, 1 ),
        std::max( DefaultRecords, desired ) );
    uint64_t maximumBytes = MaximumBufferBytes;
    if( workspace )
    {
        const auto state = workspace->Snapshot();
        const auto available = state.maximumBytes > state.currentBytes ?
            state.maximumBytes - state.currentBytes : 0;
        maximumBytes = std::min( maximumBytes, available / 2 );
    }
    return std::min( target, std::max<uint64_t>( 1, maximumBytes / recordBytes ) );
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
    std::string& error,const std::shared_ptr<AnalysisDiskBudget>& disk,
    const std::shared_ptr<AnalysisWorkspaceBudget>& workspace,
    const std::function<bool()>& cancelled )
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
        if( cancelled && cancelled() )
        { error = "analysis_pair_sort_cancelled"; return false; }
        const auto take = size_t( std::min<uint64_t>( remaining, maximumBufferedPairs ) );
        AnalysisWorkspaceReservation runWorkspace(workspace,uint64_t(take)*sizeof(AnalysisUInt64Pair));
        std::vector<AnalysisUInt64Pair> values( take );
        in.read( reinterpret_cast<char*>( values.data() ),
            std::streamsize( values.size() * sizeof( AnalysisUInt64Pair ) ) );
        if( !in ) { error = "analysis_pair_sort_source_truncated"; return false; }
        std::sort( values.begin(), values.end(), PairLess );
        const auto run = temporaryRoot / ( prefix + "-run-0-" +
            std::to_string( runIndex++ ) + ".work" );
        std::ofstream out;
        const auto charged = uint64_t( values.size() ) * sizeof( AnalysisUInt64Pair );
        AnalysisDiskGrow( disk, charged );
        FileChargeReconcile reconcile { out, run, disk, charged };
        out.open( run, std::ios::binary | std::ios::trunc );
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
        if( cancelled && cancelled() )
        { error = "analysis_pair_sort_cancelled"; return false; }
        std::vector<std::filesystem::path> next;
        for( size_t first = 0; first < runs.size(); first += MergeFanIn )
        {
            const auto last = std::min( first + MergeFanIn, runs.size() );
            const std::vector<std::filesystem::path> group( runs.begin() + first, runs.begin() + last );
            const auto merged = temporaryRoot / ( prefix + "-run-" +
                std::to_string( pass ) + "-" + std::to_string( next.size() ) + ".work" );
            uint64_t written = 0;
            if( !MergeRuns<AnalysisUInt64Pair>( group, merged, written, error,disk,workspace,
                    PairLess, "analysis_pair_sort", cancelled ) ) return false;
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

    if( cancelled && cancelled() )
    { error = "analysis_pair_sort_cancelled"; return false; }
    AnalysisDiskRemove(output,disk,ec);
    ec.clear();
    std::filesystem::rename( runs.front(), output, ec );
    if( ec ) { error = "analysis_pair_sort_publish_failed:" + ec.message(); return false; }
    const auto outputBytes = std::filesystem::file_size( output, ec );
    if( ec || outputBytes != expectedCount * sizeof( AnalysisUInt64Pair ) )
    { error = "analysis_pair_sort_output_size_mismatch"; return false; }
    return true;
}

template<typename Record, typename Less>
static bool FinishGeneratedRuns( std::vector<std::filesystem::path> runs,
    const std::filesystem::path& output, const std::filesystem::path& temporaryRoot,
    const std::string& prefix, uint64_t expectedCount, std::string& error,
    const std::shared_ptr<AnalysisDiskBudget>& disk,
    const std::shared_ptr<AnalysisWorkspaceBudget>& workspace,
    const std::function<bool()>& cancelled,
    AnalysisExternalSortStatistics* statistics, Less less, std::string_view errorPrefix )
{
    std::error_code ec;
    if( runs.empty() )
    {
        const auto previous = disk && disk->usage ? AnalysisDiskUsage::Bytes( output ) : 0;
        std::ofstream out( output, std::ios::binary | std::ios::trunc );
        if( !out ) { error = std::string( errorPrefix ) + "_output_open_failed"; return false; }
        if( disk && disk->usage ) disk->usage->Release( previous );
        return true;
    }

    size_t pass = 1;
    while( runs.size() > 1 )
    {
        if( cancelled && cancelled() )
        { error = std::string( errorPrefix ) + "_cancelled"; return false; }
        if( statistics ) statistics->mergePasses++;
        std::vector<std::filesystem::path> next;
        for( size_t first = 0; first < runs.size(); first += MergeFanIn )
        {
            const auto last = std::min( first + MergeFanIn, runs.size() );
            const std::vector<std::filesystem::path> group( runs.begin() + first, runs.begin() + last );
            const auto merged = temporaryRoot / ( prefix + "-run-" +
                std::to_string( pass ) + "-" + std::to_string( next.size() ) + ".work" );
            uint64_t written = 0;
            if( !MergeRuns<Record>( group, merged, written, error, disk, workspace,
                    less, errorPrefix, cancelled ) ) return false;
            uint64_t expected = 0;
            for( const auto& path : group )
            {
                const auto size = std::filesystem::file_size( path, ec );
                if( ec || size % sizeof( Record ) != 0 )
                { error = std::string( errorPrefix ) + "_run_size_invalid"; return false; }
                expected += size / sizeof( Record );
            }
            if( written != expected )
            { error = std::string( errorPrefix ) + "_merge_count_mismatch"; return false; }
            if( statistics )
            {
                statistics->mergedGroups++;
                statistics->mergeReadBytes += expected * sizeof( Record );
                statistics->mergeWriteBytes += written * sizeof( Record );
            }
            if( !RemoveFiles( group, error,disk ) ) return false;
            next.emplace_back( merged );
        }
        runs = std::move( next );
        ++pass;
    }

    if( cancelled && cancelled() )
    { error = std::string( errorPrefix ) + "_cancelled"; return false; }
    AnalysisDiskRemove( output, disk, ec );
    ec.clear();
    std::filesystem::rename( runs.front(), output, ec );
    if( ec ) { error = std::string( errorPrefix ) + "_publish_failed:" + ec.message(); return false; }
    const auto outputBytes = std::filesystem::file_size( output, ec );
    if( ec || outputBytes != expectedCount * sizeof( Record ) )
    { error = std::string( errorPrefix ) + "_output_size_mismatch"; return false; }
    return true;
}

struct AnalysisUInt64PairRunCollector::Impl
{
    using Record = AnalysisUInt64Pair;

    Impl( std::filesystem::path result, std::filesystem::path temporary,
        std::string name, uint64_t buffered,
        std::shared_ptr<AnalysisDiskBudget> diskBudget,
        std::shared_ptr<AnalysisWorkspaceBudget> workspaceBudget,
        std::function<bool()> cancellation,
        AnalysisExternalSortStatistics* measurements )
        : output( std::move( result ) )
        , temporaryRoot( std::move( temporary ) )
        , prefix( std::move( name ) )
        , maximumBufferedRecords( buffered )
        , disk( std::move( diskBudget ) )
        , workspace( std::move( workspaceBudget ) )
        , cancelled( std::move( cancellation ) )
        , statistics( measurements )
        , bufferWorkspace( workspace )
    {
        if( !maximumBufferedRecords || maximumBufferedRecords > SIZE_MAX / sizeof( Record ) )
            throw std::runtime_error( "analysis_pair_sort_collector_configuration" );
        std::string error;
        if( !CleanupAnalysisExternalSortFiles( temporaryRoot, prefix + "-run-", error, disk ) )
            throw std::runtime_error( error );
        bufferWorkspace.Resize( maximumBufferedRecords * sizeof( Record ) );
        values.reserve( size_t( maximumBufferedRecords ) );
        if( statistics ) *statistics = {};
    }

    void Flush()
    {
        if( values.empty() ) return;
        if( cancelled && cancelled() ) throw std::runtime_error( "analysis_pair_sort_cancelled" );
        std::sort( values.begin(), values.end(), PairLess );
        const auto run = temporaryRoot / ( prefix + "-run-0-" +
            std::to_string( runs.size() ) + ".work" );
        std::ofstream out;
        const auto charged = uint64_t( values.size() ) * sizeof( Record );
        AnalysisDiskGrow( disk, charged );
        FileChargeReconcile reconcile { out, run, disk, charged };
        out.open( run, std::ios::binary | std::ios::trunc );
        if( !out ) throw std::runtime_error( "analysis_pair_sort_run_write_open_failed" );
        out.write( reinterpret_cast<const char*>( values.data() ),
            std::streamsize( values.size() * sizeof( Record ) ) );
        out.flush();
        if( !out ) throw std::runtime_error( "analysis_pair_sort_run_write_failed" );
        runs.emplace_back( run );
        values.clear();
    }

    void Emit( const Record& value )
    {
        if( finished || count >= UINT64_MAX / sizeof( Record ) )
            throw std::runtime_error( "analysis_pair_sort_collector_count" );
        if( ( count & 4095 ) == 0 && cancelled && cancelled() )
            throw std::runtime_error( "analysis_pair_sort_cancelled" );
        values.emplace_back( value );
        ++count;
        if( values.size() == maximumBufferedRecords ) Flush();
    }

    bool Finish( std::string& error )
    {
        if( finished ) throw std::runtime_error( "analysis_pair_sort_collector_finished" );
        error.clear();
        Flush();
        std::vector<Record>().swap( values );
        bufferWorkspace.Resize( 0 );
        if( statistics )
        {
            statistics->inputRecords = count;
            statistics->initialRuns = runs.size();
            statistics->initialRunWriteBytes = count * sizeof( Record );
        }
        finished = true;
        return FinishGeneratedRuns<Record>( std::move( runs ), output, temporaryRoot,
            prefix, count, error, disk, workspace, cancelled, statistics,
            PairLess, "analysis_pair_sort" );
    }

    std::filesystem::path output;
    std::filesystem::path temporaryRoot;
    std::string prefix;
    uint64_t maximumBufferedRecords;
    std::shared_ptr<AnalysisDiskBudget> disk;
    std::shared_ptr<AnalysisWorkspaceBudget> workspace;
    std::function<bool()> cancelled;
    AnalysisExternalSortStatistics* statistics;
    AnalysisWorkspaceReservation bufferWorkspace;
    std::vector<Record> values;
    std::vector<std::filesystem::path> runs;
    uint64_t count = 0;
    bool finished = false;
};

AnalysisUInt64PairRunCollector::AnalysisUInt64PairRunCollector(
    std::filesystem::path output, std::filesystem::path temporaryRoot,
    std::string prefix, uint64_t maximumBufferedPairs,
    std::shared_ptr<AnalysisDiskBudget> disk,
    std::shared_ptr<AnalysisWorkspaceBudget> workspace,
    std::function<bool()> cancelled, AnalysisExternalSortStatistics* statistics )
    : m_impl( std::make_unique<Impl>( std::move( output ), std::move( temporaryRoot ),
        std::move( prefix ), maximumBufferedPairs, std::move( disk ),
        std::move( workspace ), std::move( cancelled ), statistics ) ) {}

AnalysisUInt64PairRunCollector::~AnalysisUInt64PairRunCollector() = default;

void AnalysisUInt64PairRunCollector::Emit( const AnalysisUInt64Pair& value )
{ m_impl->Emit( value ); }

bool AnalysisUInt64PairRunCollector::Finish( std::string& error )
{ return m_impl->Finish( error ); }

struct AnalysisKeyMetadataRunCollector::Impl
{
    using Record = AnalysisKeyMetadataRecord;

    Impl( std::filesystem::path result, std::filesystem::path temporary,
        std::string name, uint64_t buffered,
        std::shared_ptr<AnalysisDiskBudget> diskBudget,
        std::shared_ptr<AnalysisWorkspaceBudget> workspaceBudget,
        std::function<bool()> cancellation,
        AnalysisExternalSortStatistics* measurements )
        : output( std::move( result ) )
        , temporaryRoot( std::move( temporary ) )
        , prefix( std::move( name ) )
        , maximumBufferedRecords( buffered )
        , disk( std::move( diskBudget ) )
        , workspace( std::move( workspaceBudget ) )
        , cancelled( std::move( cancellation ) )
        , statistics( measurements )
        , bufferWorkspace( workspace )
    {
        if( !maximumBufferedRecords || maximumBufferedRecords > SIZE_MAX / sizeof( Record ) )
            throw std::runtime_error( "analysis_metadata_sort_collector_configuration" );
        std::string error;
        if( !CleanupAnalysisExternalSortFiles( temporaryRoot, prefix + "-run-", error, disk ) )
            throw std::runtime_error( error );
        bufferWorkspace.Resize( maximumBufferedRecords * sizeof( Record ) );
        values.reserve( size_t( maximumBufferedRecords ) );
        if( statistics ) *statistics = {};
    }

    void Flush()
    {
        if( values.empty() ) return;
        if( cancelled && cancelled() ) throw std::runtime_error( "analysis_metadata_sort_cancelled" );
        std::sort( values.begin(), values.end(), MetadataLess );
        const auto run = temporaryRoot / ( prefix + "-run-0-" +
            std::to_string( runs.size() ) + ".work" );
        std::ofstream out;
        const auto charged = uint64_t( values.size() ) * sizeof( Record );
        AnalysisDiskGrow( disk, charged );
        FileChargeReconcile reconcile { out, run, disk, charged };
        out.open( run, std::ios::binary | std::ios::trunc );
        if( !out ) throw std::runtime_error( "analysis_metadata_sort_run_write_open_failed" );
        out.write( reinterpret_cast<const char*>( values.data() ),
            std::streamsize( values.size() * sizeof( Record ) ) );
        out.flush();
        if( !out ) throw std::runtime_error( "analysis_metadata_sort_run_write_failed" );
        runs.emplace_back( run );
        values.clear();
    }

    void Emit( const Record& value )
    {
        if( finished || count >= UINT64_MAX / sizeof( Record ) )
            throw std::runtime_error( "analysis_metadata_sort_collector_count" );
        if( ( count & 4095 ) == 0 && cancelled && cancelled() )
            throw std::runtime_error( "analysis_metadata_sort_cancelled" );
        values.emplace_back( value );
        ++count;
        if( values.size() == maximumBufferedRecords ) Flush();
    }

    bool Finish( std::string& error )
    {
        if( finished ) throw std::runtime_error( "analysis_metadata_sort_collector_finished" );
        error.clear();
        Flush();
        std::vector<Record>().swap( values );
        bufferWorkspace.Resize( 0 );
        if( statistics )
        {
            statistics->inputRecords = count;
            statistics->initialRuns = runs.size();
            statistics->initialRunWriteBytes = count * sizeof( Record );
        }
        finished = true;
        return FinishGeneratedRuns<Record>( std::move( runs ), output, temporaryRoot,
            prefix, count, error, disk, workspace, cancelled, statistics,
            MetadataLess, "analysis_metadata_sort" );
    }

    std::filesystem::path output;
    std::filesystem::path temporaryRoot;
    std::string prefix;
    uint64_t maximumBufferedRecords;
    std::shared_ptr<AnalysisDiskBudget> disk;
    std::shared_ptr<AnalysisWorkspaceBudget> workspace;
    std::function<bool()> cancelled;
    AnalysisExternalSortStatistics* statistics;
    AnalysisWorkspaceReservation bufferWorkspace;
    std::vector<Record> values;
    std::vector<std::filesystem::path> runs;
    uint64_t count = 0;
    bool finished = false;
};

AnalysisKeyMetadataRunCollector::AnalysisKeyMetadataRunCollector(
    std::filesystem::path output, std::filesystem::path temporaryRoot,
    std::string prefix, uint64_t maximumBufferedRecords,
    std::shared_ptr<AnalysisDiskBudget> disk,
    std::shared_ptr<AnalysisWorkspaceBudget> workspace,
    std::function<bool()> cancelled, AnalysisExternalSortStatistics* statistics )
    : m_impl( std::make_unique<Impl>( std::move( output ), std::move( temporaryRoot ),
        std::move( prefix ), maximumBufferedRecords, std::move( disk ),
        std::move( workspace ), std::move( cancelled ), statistics ) ) {}

AnalysisKeyMetadataRunCollector::~AnalysisKeyMetadataRunCollector() = default;

void AnalysisKeyMetadataRunCollector::Emit( const AnalysisKeyMetadataRecord& value )
{ m_impl->Emit( value ); }

bool AnalysisKeyMetadataRunCollector::Finish( std::string& error )
{ return m_impl->Finish( error ); }

bool SortAnalysisKeyMetadataRecords( const std::filesystem::path& source,
    const std::filesystem::path& output, const std::filesystem::path& temporaryRoot,
    const std::string& prefix, uint64_t expectedCount, uint64_t maximumBufferedRecords,
    std::string& error, const std::shared_ptr<AnalysisDiskBudget>& disk,
    const std::shared_ptr<AnalysisWorkspaceBudget>& workspace,
    const std::function<bool()>& cancelled,
    AnalysisExternalSortStatistics* statistics )
{
    using Record = AnalysisKeyMetadataRecord;
    error.clear();
    if( statistics ) *statistics = {};
    if( maximumBufferedRecords == 0 ) maximumBufferedRecords = 1;
    maximumBufferedRecords = std::min<uint64_t>( maximumBufferedRecords,
        std::numeric_limits<size_t>::max() );
    std::error_code ec;
    const auto bytes = std::filesystem::file_size( source, ec );
    if( ec ) { error = "analysis_metadata_sort_source_size_failed:" + ec.message(); return false; }
    if( expectedCount > std::numeric_limits<uint64_t>::max() / sizeof( Record ) ||
        bytes != expectedCount * sizeof( Record ) )
    { error = "analysis_metadata_sort_source_size_mismatch"; return false; }
    if( !CleanupAnalysisExternalSortFiles( temporaryRoot, prefix + "-run-", error,disk ) ) return false;

    std::ifstream in( source, std::ios::binary );
    if( !in ) { error = "analysis_metadata_sort_source_open_failed"; return false; }
    std::vector<std::filesystem::path> runs;
    uint64_t remaining = expectedCount;
    size_t runIndex = 0;
    while( remaining != 0 )
    {
        if( cancelled && cancelled() )
        { error = "analysis_metadata_sort_cancelled"; return false; }
        const auto take = size_t( std::min<uint64_t>( remaining, maximumBufferedRecords ) );
        AnalysisWorkspaceReservation runWorkspace( workspace, uint64_t( take ) * sizeof( Record ) );
        std::vector<Record> values( take );
        in.read( reinterpret_cast<char*>( values.data() ),
            std::streamsize( values.size() * sizeof( Record ) ) );
        if( !in ) { error = "analysis_metadata_sort_source_truncated"; return false; }
        std::sort( values.begin(), values.end(), MetadataLess );
        const auto run = temporaryRoot / ( prefix + "-run-0-" +
            std::to_string( runIndex++ ) + ".work" );
        std::ofstream out;
        const auto charged = uint64_t( values.size() ) * sizeof( Record );
        AnalysisDiskGrow( disk, charged );
        FileChargeReconcile reconcile { out, run, disk, charged };
        out.open( run, std::ios::binary | std::ios::trunc );
        if( !out ) { error = "analysis_metadata_sort_run_write_open_failed"; return false; }
        out.write( reinterpret_cast<const char*>( values.data() ),
            std::streamsize( values.size() * sizeof( Record ) ) );
        out.flush();
        if( !out ) { error = "analysis_metadata_sort_run_write_failed"; return false; }
        runs.emplace_back( run );
        remaining -= take;
    }
    in.close();
    if( statistics )
    {
        statistics->inputRecords = expectedCount;
        statistics->initialRuns = runs.size();
        statistics->sourceReadBytes = bytes;
        statistics->initialRunWriteBytes = expectedCount * sizeof( Record );
    }
    return FinishGeneratedRuns<Record>( std::move( runs ), output, temporaryRoot, prefix,
        expectedCount, error, disk, workspace, cancelled, statistics,
        MetadataLess, "analysis_metadata_sort" );
}

bool SortAnalysisKeyMetadataRecordsGenerated( const std::filesystem::path& output,
    const std::filesystem::path& temporaryRoot, const std::string& prefix,
    uint64_t maximumBufferedRecords,
    const std::function<void(const std::function<void(const AnalysisKeyMetadataRecord&)>& emit)>& produce,
    std::string& error, const std::shared_ptr<AnalysisDiskBudget>& disk,
    const std::shared_ptr<AnalysisWorkspaceBudget>& workspace,
    const std::function<bool()>& cancelled,
    AnalysisExternalSortStatistics* statistics )
{
    using Record = AnalysisKeyMetadataRecord;
    error.clear();
    if( statistics ) *statistics = {};
    if( !produce || !maximumBufferedRecords || maximumBufferedRecords > SIZE_MAX / sizeof( Record ) )
    { error = "analysis_metadata_sort_generated_configuration"; return false; }
    if( !CleanupAnalysisExternalSortFiles( temporaryRoot, prefix + "-run-", error, disk ) ) return false;
    AnalysisWorkspaceReservation bufferWorkspace( workspace, maximumBufferedRecords * sizeof( Record ) );
    std::vector<Record> values;
    values.reserve( size_t( maximumBufferedRecords ) );
    std::vector<std::filesystem::path> runs;
    uint64_t count = 0;
    const auto flush = [&] {
        if( values.empty() ) return;
        if( cancelled && cancelled() ) throw std::runtime_error( "analysis_metadata_sort_cancelled" );
        std::sort( values.begin(), values.end(), MetadataLess );
        const auto run = temporaryRoot / ( prefix + "-run-0-" + std::to_string( runs.size() ) + ".work" );
        std::ofstream out;
        const auto charged = uint64_t( values.size() ) * sizeof( Record );
        AnalysisDiskGrow( disk, charged );
        FileChargeReconcile reconcile { out, run, disk, charged };
        out.open( run, std::ios::binary | std::ios::trunc );
        if( !out ) throw std::runtime_error( "analysis_metadata_sort_run_write_open_failed" );
        out.write( reinterpret_cast<const char*>( values.data() ),
            std::streamsize( values.size() * sizeof( Record ) ) );
        out.flush();
        if( !out ) throw std::runtime_error( "analysis_metadata_sort_run_write_failed" );
        runs.emplace_back( run );
        values.clear();
    };
    const std::function<void(const Record&)> emit = [&]( const Record& value ) {
        if( count >= UINT64_MAX / sizeof( Record ) )
            throw std::runtime_error( "analysis_metadata_sort_generated_count" );
        if( ( count & 4095 ) == 0 && cancelled && cancelled() )
            throw std::runtime_error( "analysis_metadata_sort_cancelled" );
        values.emplace_back( value );
        ++count;
        if( values.size() == maximumBufferedRecords ) flush();
    };
    produce( emit );
    flush();
    std::vector<Record>().swap( values );
    bufferWorkspace.Resize( 0 );
    if( statistics )
    {
        statistics->inputRecords = count;
        statistics->initialRuns = runs.size();
        statistics->initialRunWriteBytes = count * sizeof( Record );
    }
    return FinishGeneratedRuns<Record>( std::move( runs ), output, temporaryRoot, prefix,
        count, error, disk, workspace, cancelled, statistics,
        MetadataLess, "analysis_metadata_sort" );
}

bool SortAnalysisUInt64PairsGenerated( const std::filesystem::path& output,
    const std::filesystem::path& temporaryRoot, const std::string& prefix,
    uint64_t maximumBufferedPairs,
    const std::function<void(const std::function<void(const AnalysisUInt64Pair&)>& emit)>& produce,
    std::string& error, const std::shared_ptr<AnalysisDiskBudget>& disk,
    const std::shared_ptr<AnalysisWorkspaceBudget>& workspace,
    const std::function<bool()>& cancelled,
    AnalysisExternalSortStatistics* statistics )
{
    using Record = AnalysisUInt64Pair;
    error.clear();
    if( statistics ) *statistics = {};
    if( !produce || !maximumBufferedPairs || maximumBufferedPairs > SIZE_MAX / sizeof( Record ) )
    { error = "analysis_pair_sort_generated_configuration"; return false; }
    if( !CleanupAnalysisExternalSortFiles( temporaryRoot, prefix + "-run-", error, disk ) ) return false;
    AnalysisWorkspaceReservation bufferWorkspace( workspace, maximumBufferedPairs * sizeof( Record ) );
    std::vector<Record> values;
    values.reserve( size_t( maximumBufferedPairs ) );
    std::vector<std::filesystem::path> runs;
    uint64_t count = 0;
    const auto flush = [&] {
        if( values.empty() ) return;
        if( cancelled && cancelled() ) throw std::runtime_error( "analysis_pair_sort_cancelled" );
        std::sort( values.begin(), values.end(), PairLess );
        const auto run = temporaryRoot / ( prefix + "-run-0-" + std::to_string( runs.size() ) + ".work" );
        std::ofstream out;
        const auto charged = uint64_t( values.size() ) * sizeof( Record );
        AnalysisDiskGrow( disk, charged );
        FileChargeReconcile reconcile { out, run, disk, charged };
        out.open( run, std::ios::binary | std::ios::trunc );
        if( !out ) throw std::runtime_error( "analysis_pair_sort_run_write_open_failed" );
        out.write( reinterpret_cast<const char*>( values.data() ),
            std::streamsize( values.size() * sizeof( Record ) ) );
        out.flush();
        if( !out ) throw std::runtime_error( "analysis_pair_sort_run_write_failed" );
        runs.emplace_back( run );
        values.clear();
    };
    const std::function<void(const Record&)> emit = [&]( const Record& value ) {
        if( count >= UINT64_MAX / sizeof( Record ) )
            throw std::runtime_error( "analysis_pair_sort_generated_count" );
        if( ( count & 4095 ) == 0 && cancelled && cancelled() )
            throw std::runtime_error( "analysis_pair_sort_cancelled" );
        values.emplace_back( value );
        ++count;
        if( values.size() == maximumBufferedPairs ) flush();
    };
    produce( emit );
    flush();
    std::vector<Record>().swap( values );
    bufferWorkspace.Resize( 0 );
    if( statistics )
    {
        statistics->inputRecords = count;
        statistics->initialRuns = runs.size();
        statistics->initialRunWriteBytes = count * sizeof( Record );
    }
    return FinishGeneratedRuns<Record>( std::move( runs ), output, temporaryRoot, prefix,
        count, error, disk, workspace, cancelled, statistics,
        PairLess, "analysis_pair_sort" );
}

}

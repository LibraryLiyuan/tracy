#include "TracyQuerySpillStatistics.hpp"
#include "TracyAnalysisExternalSort.hpp"
#include "TracyAnalysisIoPath.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace tracy::query
{
namespace
{
void WriteValues( std::ofstream& output, const int64_t* values, size_t count,
    const std::shared_ptr<analysis::AnalysisDiskBudget>& disk )
{
    analysis::AnalysisDiskGrow( disk, count * sizeof( int64_t ) );
    output.write( reinterpret_cast<const char*>( values ), std::streamsize( count * sizeof( int64_t ) ) );
    if( !output ) throw std::runtime_error( "Job statistics spill write failed" );
}

void CloseOutput( std::ofstream& output )
{
    output.flush();
    if( !output ) throw std::runtime_error( "Job statistics spill flush failed" );
    output.close();
    if( !output ) throw std::runtime_error( "Job statistics spill close failed" );
}
}

QuerySpillStatistics::QuerySpillStatistics( const std::filesystem::path& temporaryRoot,
    std::shared_ptr<analysis::AnalysisDiskBudget> disk,
    std::shared_ptr<analysis::AnalysisWorkspaceBudget> workspace,
    std::function<void()> check, size_t maximumBufferedValues )
    : m_disk( std::move( disk ) ), m_workspace( std::move( workspace ) ),
      m_check( std::move( check ) ), m_maximumBufferedValues( maximumBufferedValues )
{
    if( maximumBufferedValues == 0 || maximumBufferedValues > 65536 )
        throw std::invalid_argument( "Job statistics spill buffer must be between 1 and 65536" );
    Check();
    m_workspace.Resize( maximumBufferedValues * sizeof( int64_t ) + 131072 );
    m_values.reserve( maximumBufferedValues );
    std::filesystem::create_directories( temporaryRoot );
    static std::atomic<uint64_t> sequence { 0 };
    for( size_t attempt = 0; attempt < 100; attempt++ )
    {
        const auto candidate = temporaryRoot / ( "job-statistics-" +
            std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) + "-" +
            std::to_string( sequence.fetch_add( 1 ) ) );
        if( std::filesystem::create_directory( candidate ) ) { m_root = candidate; return; }
    }
    throw std::runtime_error( "Job statistics temporary directory collision" );
}

QuerySpillStatistics::~QuerySpillStatistics()
{
    if( m_root.empty() ) return;
    std::error_code error;
    analysis::AnalysisDiskRemove( m_root, m_disk, error, true );
}

void QuerySpillStatistics::Check() const
{
    if( m_check ) m_check();
}

std::filesystem::path QuerySpillStatistics::NewRun()
{
    return m_root / ( std::to_string( m_nextRun++ ) + ".work" );
}

void QuerySpillStatistics::Remove( const std::filesystem::path& path )
{
    std::error_code error;
    analysis::AnalysisDiskRemove( path, m_disk, error );
    if( error ) throw std::runtime_error( "Job statistics spill cleanup failed: " + error.message() );
}

std::filesystem::path QuerySpillStatistics::Merge( const std::filesystem::path& left,
    const std::filesystem::path& right )
{
    Check();
    const auto outputPath = NewRun();
    std::ifstream leftInput( left, std::ios::binary ), rightInput( right, std::ios::binary );
    std::ofstream output( outputPath, std::ios::binary );
    if( !leftInput || !rightInput || !output ) throw std::runtime_error( "Job statistics merge open failed" );
    const auto read = []( std::ifstream& input, int64_t& value ) {
        input.read( reinterpret_cast<char*>( &value ), sizeof( value ) );
        if( input.gcount() == sizeof( value ) ) return true;
        if( input.gcount() != 0 || !input.eof() ) throw std::runtime_error( "Job statistics merge read failed" );
        return false;
    };
    int64_t leftValue = 0, rightValue = 0;
    bool hasLeft = read( leftInput, leftValue ), hasRight = read( rightInput, rightValue );
    std::array<int64_t, 4096> buffer;
    size_t buffered = 0;
    while( hasLeft || hasRight )
    {
        if( hasLeft && ( !hasRight || leftValue <= rightValue ) )
        { buffer[buffered++] = leftValue; hasLeft = read( leftInput, leftValue ); }
        else { buffer[buffered++] = rightValue; hasRight = read( rightInput, rightValue ); }
        if( buffered == buffer.size() )
        {
            Check();
            WriteValues( output, buffer.data(), buffered, m_disk );
            buffered = 0;
        }
    }
    Check();
    WriteValues( output, buffer.data(), buffered, m_disk );
    CloseOutput( output );
    leftInput.close(); rightInput.close();
    Remove( left ); Remove( right );
    return outputPath;
}

void QuerySpillStatistics::Flush()
{
    if( m_values.empty() ) return;
    Check();
    size_t comparisons = 0;
    std::sort( m_values.begin(), m_values.end(), [&]( int64_t left, int64_t right ) {
        if( ++comparisons % 4096 == 0 ) Check();
        return left < right;
    } );
    auto pending = NewRun();
    std::ofstream output( pending, std::ios::binary );
    if( !output ) throw std::runtime_error( "Job statistics run open failed" );
    WriteValues( output, m_values.data(), m_values.size(), m_disk );
    CloseOutput( output );
    m_values.clear();
    for( auto& run : m_runs )
    {
        if( run.empty() ) { run = std::move( pending ); return; }
        pending = Merge( run, pending );
        run.clear();
    }
    throw std::overflow_error( "Job statistics run count overflow" );
}

void QuerySpillStatistics::Append( int64_t value )
{
    if( m_finished ) throw std::logic_error( "Job statistics already finished" );
    Check();
    if( m_count == UINT64_MAX ) throw std::overflow_error( "Job statistics count overflow" );
    m_values.push_back( value );
    m_count++;
    if( m_values.size() == m_maximumBufferedValues ) Flush();
}

analysis::Statistics QuerySpillStatistics::Finish()
{
    if( m_finished ) throw std::logic_error( "Job statistics already finished" );
    m_finished = true;
    Check();
    Flush();
    analysis::Statistics result;
    if( m_count == 0 ) return result;
    std::filesystem::path sorted;
    for( auto& run : m_runs )
    {
        if( run.empty() ) continue;
        sorted = sorted.empty() ? run : Merge( sorted, run );
        run.clear();
    }
    if( m_count > uint64_t( std::numeric_limits<std::streamoff>::max() ) / sizeof( int64_t ) ||
        std::filesystem::file_size( sorted ) != m_count * sizeof( int64_t ) )
        throw std::runtime_error( "Job statistics sorted size mismatch" );
    std::ifstream input( sorted, std::ios::binary );
    if( !input ) throw std::runtime_error( "Job statistics sorted open failed" );
    const auto at = [&]( uint64_t index ) {
        Check();
        input.clear();
        input.seekg( std::streamoff( index * sizeof( int64_t ) ) );
        int64_t value;
        input.read( reinterpret_cast<char*>( &value ), sizeof( value ) );
        if( !input ) throw std::runtime_error( "Job statistics sorted read failed" );
        return value;
    };
    const auto percentile = [&]( double fraction ) {
        const double position = fraction * double( m_count - 1 );
        const auto low = uint64_t( std::floor( position ) );
        const auto high = std::min( low + 1, m_count - 1 );
        const auto lowValue = at( low ), highValue = at( high );
        return double( lowValue ) + ( double( highValue ) - double( lowValue ) ) * ( position - double( low ) );
    };
    result.count = m_count;
    result.min = at( 0 ); result.max = at( m_count - 1 );
    result.median = result.p50 = percentile( 0.50 );
    result.p90 = percentile( 0.90 ); result.p95 = percentile( 0.95 ); result.p99 = percentile( 0.99 );
    const auto scan = [&]( const auto& visitor ) {
        input.clear(); input.seekg( 0 );
        std::array<int64_t, 4096> values;
        for( uint64_t offset = 0; offset < m_count; )
        {
            Check();
            const auto count = size_t( std::min<uint64_t>( values.size(), m_count - offset ) );
            input.read( reinterpret_cast<char*>( values.data() ), std::streamsize( count * sizeof( int64_t ) ) );
            if( !input ) throw std::runtime_error( "Job statistics sorted scan failed" );
            for( size_t index = 0; index < count; index++ ) visitor( values[index] );
            offset += count;
        }
    };
    scan( [&]( int64_t value ) {
        if( ( value > 0 && result.total > INT64_MAX - value ) ||
            ( value < 0 && result.total < INT64_MIN - value ) )
            throw std::overflow_error( "Job statistics total overflow" );
        result.total += value;
    } );
    result.mean = double( result.total ) / double( result.count );
    long double squaredDeviation = 0, truncatedTotal = 0;
    uint64_t truncatedCount = 0;
    scan( [&]( int64_t value ) {
        const long double delta = static_cast<long double>( value ) - result.mean;
        squaredDeviation += delta * delta;
        if( double( value ) <= result.p90 ) { truncatedTotal += value; truncatedCount++; }
    } );
    result.stddev = std::sqrt( double( squaredDeviation / result.count ) );
    result.truncatedMean = truncatedCount == 0 ? 0 : double( truncatedTotal / truncatedCount );
    input.close();
    Remove( sorted );
    return result;
}

QuerySpillUInt64Set::QuerySpillUInt64Set( const std::filesystem::path& temporaryRoot,
    std::shared_ptr<analysis::AnalysisDiskBudget> disk,
    std::shared_ptr<analysis::AnalysisWorkspaceBudget> workspace,
    std::function<void()> check, size_t maximumBufferedValues )
    : m_disk( std::move( disk ) ), m_workspace( std::move( workspace ) ),
      m_check( std::move( check ) ), m_maximumBufferedValues( maximumBufferedValues )
{
    if( maximumBufferedValues == 0 || maximumBufferedValues > 65536 )
        throw std::invalid_argument( "spill set buffer must be between 1 and 65536" );
    Check();
    m_workspace.Resize( maximumBufferedValues * sizeof( uint64_t ) + 131072 );
    m_values.reserve( maximumBufferedValues );
    std::filesystem::create_directories( temporaryRoot );
    static std::atomic<uint64_t> sequence { 0 };
    for( size_t attempt = 0; attempt < 100; attempt++ )
    {
        const auto candidate = temporaryRoot / ( "uint64-set-" +
            std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) + "-" +
            std::to_string( sequence.fetch_add( 1 ) ) );
        if( std::filesystem::create_directory( candidate ) ) { m_root = candidate; return; }
    }
    throw std::runtime_error( "spill set temporary directory collision" );
}

QuerySpillUInt64Set::~QuerySpillUInt64Set()
{
    if( m_root.empty() ) return;
    std::error_code error;
    analysis::AnalysisDiskRemove( m_root, m_disk, error, true );
}

void QuerySpillUInt64Set::Check() const
{
    if( m_check ) m_check();
}

std::filesystem::path QuerySpillUInt64Set::NewRun()
{
    return m_root / ( std::to_string( m_nextRun++ ) + ".work" );
}

void QuerySpillUInt64Set::Remove( const std::filesystem::path& path )
{
    std::error_code error;
    analysis::AnalysisDiskRemove( path, m_disk, error );
    if( error ) throw std::runtime_error( "spill set cleanup failed: " + error.message() );
}

std::filesystem::path QuerySpillUInt64Set::Merge( const std::filesystem::path& left,
    const std::filesystem::path& right )
{
    Check();
    const auto outputPath = NewRun();
    std::ifstream leftInput( left, std::ios::binary ), rightInput( right, std::ios::binary );
    std::ofstream output( outputPath, std::ios::binary );
    if( !leftInput || !rightInput || !output ) throw std::runtime_error( "spill set merge open failed" );
    const auto read = []( std::ifstream& input, uint64_t& value ) {
        input.read( reinterpret_cast<char*>( &value ), sizeof( value ) );
        if( input.gcount() == sizeof( value ) ) return true;
        if( input.gcount() != 0 || !input.eof() ) throw std::runtime_error( "spill set merge read failed" );
        return false;
    };
    uint64_t leftValue = 0, rightValue = 0, previous = 0;
    bool hasLeft = read( leftInput, leftValue ), hasRight = read( rightInput, rightValue ), havePrevious = false;
    std::array<uint64_t, 4096> buffer;
    size_t buffered = 0;
    while( hasLeft || hasRight )
    {
        uint64_t value;
        if( hasLeft && ( !hasRight || leftValue <= rightValue ) )
        { value = leftValue; hasLeft = read( leftInput, leftValue ); }
        else { value = rightValue; hasRight = read( rightInput, rightValue ); }
        if( havePrevious && value == previous ) continue;
        previous = value; havePrevious = true; buffer[buffered++] = value;
        if( buffered == buffer.size() )
        {
            Check();
            WriteValues( output, reinterpret_cast<const int64_t*>( buffer.data() ), buffered, m_disk );
            buffered = 0;
        }
    }
    Check();
    WriteValues( output, reinterpret_cast<const int64_t*>( buffer.data() ), buffered, m_disk );
    CloseOutput( output );
    leftInput.close(); rightInput.close();
    Remove( left ); Remove( right );
    return outputPath;
}

void QuerySpillUInt64Set::Flush()
{
    if( m_values.empty() ) return;
    Check();
    size_t comparisons = 0;
    std::sort( m_values.begin(), m_values.end(), [&]( uint64_t left, uint64_t right ) {
        if( ++comparisons % 4096 == 0 ) Check();
        return left < right;
    } );
    m_values.erase( std::unique( m_values.begin(), m_values.end() ), m_values.end() );
    auto pending = NewRun();
    std::ofstream output( pending, std::ios::binary );
    if( !output ) throw std::runtime_error( "spill set run open failed" );
    WriteValues( output, reinterpret_cast<const int64_t*>( m_values.data() ), m_values.size(), m_disk );
    CloseOutput( output );
    m_values.clear();
    for( auto& run : m_runs )
    {
        if( run.empty() ) { run = std::move( pending ); return; }
        pending = Merge( run, pending );
        run.clear();
    }
    throw std::overflow_error( "spill set run count overflow" );
}

void QuerySpillUInt64Set::Append( uint64_t value )
{
    if( m_finished ) throw std::logic_error( "spill set already finished" );
    Check();
    m_values.push_back( value );
    if( m_values.size() == m_maximumBufferedValues ) Flush();
}

void QuerySpillUInt64Set::Finish()
{
    if( m_finished ) throw std::logic_error( "spill set already finished" );
    m_finished = true;
    Check();
    Flush();
    for( auto& run : m_runs )
    {
        if( run.empty() ) continue;
        m_sorted = m_sorted.empty() ? run : Merge( m_sorted, run );
        run.clear();
    }
    if( m_sorted.empty() ) return;
    const auto bytes = std::filesystem::file_size( m_sorted );
    if( bytes % sizeof( uint64_t ) != 0 ) throw std::runtime_error( "spill set sorted size mismatch" );
    m_uniqueCount = bytes / sizeof( uint64_t );
}

uint64_t QuerySpillUInt64Set::Count() const
{
    if( !m_finished ) throw std::logic_error( "spill set is not finished" );
    return m_uniqueCount;
}

bool QuerySpillUInt64Set::Contains( uint64_t value ) const
{
    if( !m_finished ) throw std::logic_error( "spill set is not finished" );
    if( m_sorted.empty() ) return false;
    std::ifstream input( m_sorted, std::ios::binary );
    if( !input ) throw std::runtime_error( "spill set sorted open failed" );
    uint64_t begin = 0, end = m_uniqueCount;
    while( begin < end )
    {
        Check();
        const auto middle = begin + ( end - begin ) / 2;
        input.seekg( std::streamoff( middle * sizeof( uint64_t ) ) );
        uint64_t current;
        input.read( reinterpret_cast<char*>( &current ), sizeof( current ) );
        if( !input ) throw std::runtime_error( "spill set sorted read failed" );
        if( current < value ) begin = middle + 1; else end = middle;
    }
    if( begin == m_uniqueCount ) return false;
    input.clear();
    input.seekg( std::streamoff( begin * sizeof( uint64_t ) ) );
    uint64_t current;
    input.read( reinterpret_cast<char*>( &current ), sizeof( current ) );
    if( !input ) throw std::runtime_error( "spill set sorted read failed" );
    return current == value;
}

void QuerySpillUInt64Set::VisitValues( const std::function<bool( uint64_t )>& visit ) const
{
    if( !m_finished ) throw std::logic_error( "spill set is not finished" );
    if( !visit || m_sorted.empty() ) return;
    std::ifstream input( m_sorted, std::ios::binary );
    if( !input ) throw std::runtime_error( "spill set sorted open failed" );
    std::array<uint64_t, 4096> buffer;
    uint64_t visited = 0;
    while( visited < m_uniqueCount )
    {
        Check();
        const auto take = size_t( std::min<uint64_t>( buffer.size(), m_uniqueCount - visited ) );
        input.read( reinterpret_cast<char*>( buffer.data() ),
            std::streamsize( take * sizeof( uint64_t ) ) );
        if( !input ) throw std::runtime_error( "spill set sorted read failed" );
        for( size_t index = 0; index < take; index++ )
            if( !visit( buffer[index] ) ) return;
        visited += take;
    }
    if( input.peek() != std::ifstream::traits_type::eof() )
        throw std::runtime_error( "spill set sorted size mismatch" );
}

QuerySpillUInt64Graph::QuerySpillUInt64Graph( const std::filesystem::path& temporaryRoot,
    std::shared_ptr<analysis::AnalysisDiskBudget> disk,
    std::shared_ptr<analysis::AnalysisWorkspaceBudget> workspace,
    std::function<void()> check )
    : m_disk( std::move( disk ) ), m_workspace( std::move( workspace ) ), m_check( std::move( check ) )
{
    Check();
    std::filesystem::create_directories( temporaryRoot );
    static std::atomic<uint64_t> sequence { 0 };
    for( size_t attempt = 0; attempt < 100; attempt++ )
    {
        const auto candidate = temporaryRoot / ( "uint64-graph-" +
            std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) + "-" +
            std::to_string( sequence.fetch_add( 1 ) ) );
        if( std::filesystem::create_directory( candidate ) ) { m_root = candidate; break; }
    }
    if( m_root.empty() ) throw std::runtime_error( "spill graph temporary directory collision" );
    m_source = m_root / "edges.work";
    m_sorted = m_root / "edges.sorted";
    m_output = std::make_unique<std::ofstream>( analysis::AnalysisIoPath( m_source ), std::ios::binary );
    if( !*m_output ) throw std::runtime_error( "spill graph edge file open failed" );
}

QuerySpillUInt64Graph::~QuerySpillUInt64Graph()
{
    if( m_output ) m_output->close();
    if( m_root.empty() ) return;
    std::error_code error;
    analysis::AnalysisDiskRemove( m_root, m_disk, error, true );
}

void QuerySpillUInt64Graph::Check() const
{
    if( m_check ) m_check();
}

void QuerySpillUInt64Graph::AppendUndirected( uint64_t left, uint64_t right )
{
    if( m_finished ) throw std::logic_error( "spill graph already finished" );
    Check();
    if( m_edgeRecords > UINT64_MAX - 2 ) throw std::overflow_error( "spill graph edge count overflow" );
    const std::array<analysis::AnalysisUInt64Pair, 2> pairs {{{ left, right }, { right, left }}};
    analysis::AnalysisDiskGrow( m_disk, sizeof( pairs ) );
    m_output->write( reinterpret_cast<const char*>( pairs.data() ), sizeof( pairs ) );
    if( !*m_output ) throw std::runtime_error( "spill graph edge write failed" );
    m_edgeRecords += pairs.size();
}

void QuerySpillUInt64Graph::Finish()
{
    if( m_finished ) throw std::logic_error( "spill graph already finished" );
    m_finished = true;
    Check();
    m_output->flush();
    if( !*m_output ) throw std::runtime_error( "spill graph edge flush failed" );
    m_output->close();
    m_output.reset();
    if( m_edgeRecords == 0 )
    {
        m_sorted = m_source;
        return;
    }
    std::string error;
    if( !analysis::SortAnalysisUInt64Pairs( m_source, m_sorted, m_root, "gfx-chain", m_edgeRecords,
            8192, error, m_disk, m_workspace ) )
        throw std::runtime_error( "spill graph edge sort failed: " + error );
    std::error_code cleanupError;
    analysis::AnalysisDiskRemove( m_source, m_disk, cleanupError );
    if( cleanupError ) throw std::runtime_error( "spill graph source cleanup failed: " + cleanupError.message() );
    if( std::filesystem::file_size( analysis::AnalysisIoPath( m_sorted ) ) !=
        m_edgeRecords * sizeof( analysis::AnalysisUInt64Pair ) )
        throw std::runtime_error( "spill graph sorted size mismatch" );
}

uint64_t QuerySpillUInt64Graph::EdgeRecordCount() const
{
    if( !m_finished ) throw std::logic_error( "spill graph is not finished" );
    return m_edgeRecords;
}

void QuerySpillUInt64Graph::VisitNeighbors( uint64_t node,
    const std::function<bool( uint64_t )>& visit ) const
{
    if( !m_finished ) throw std::logic_error( "spill graph is not finished" );
    if( m_edgeRecords == 0 ) return;
    std::ifstream input( analysis::AnalysisIoPath( m_sorted ), std::ios::binary );
    if( !input ) throw std::runtime_error( "spill graph sorted open failed" );
    const auto read = [&]( uint64_t ordinal ) {
        Check();
        input.clear();
        input.seekg( std::streamoff( ordinal * sizeof( analysis::AnalysisUInt64Pair ) ) );
        analysis::AnalysisUInt64Pair pair;
        input.read( reinterpret_cast<char*>( &pair ), sizeof( pair ) );
        if( !input ) throw std::runtime_error( "spill graph sorted read failed" );
        return pair;
    };
    uint64_t begin = 0, end = m_edgeRecords;
    while( begin < end )
    {
        const auto middle = begin + ( end - begin ) / 2;
        if( read( middle ).key < node ) begin = middle + 1; else end = middle;
    }
    for( uint64_t ordinal = begin; ordinal < m_edgeRecords; ordinal++ )
    {
        const auto pair = read( ordinal );
        if( pair.key != node ) break;
        if( !visit( pair.value ) ) break;
    }
}

}

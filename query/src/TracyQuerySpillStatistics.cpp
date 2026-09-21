#include "TracyQuerySpillStatistics.hpp"

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

}

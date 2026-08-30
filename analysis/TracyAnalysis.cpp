#include "TracyAnalysis.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace tracy::analysis
{

const char* ToString( TraceSourceKind value )
{
    switch( value )
    {
    case TraceSourceKind::Snapshot: return "snapshot";
    case TraceSourceKind::Segment: return "segment";
    case TraceSourceKind::Session: return "session";
    }
    return "unknown";
}

const char* ToString( TraceSourceState value )
{
    switch( value )
    {
    case TraceSourceState::Queued: return "queued";
    case TraceSourceState::Loading: return "loading";
    case TraceSourceState::Indexing: return "indexing";
    case TraceSourceState::Ready: return "ready";
    case TraceSourceState::Failed: return "failed";
    case TraceSourceState::Closing: return "closing";
    case TraceSourceState::Closed: return "closed";
    }
    return "unknown";
}

double Percentile( const std::vector<int64_t>& sortedValues, double percentile )
{
    if( sortedValues.empty() ) return 0;
    if( percentile < 0 || percentile > 1 ) throw std::invalid_argument( "percentile must be between 0 and 1" );

    const double position = percentile * double( sortedValues.size() - 1 );
    const auto low = size_t( std::floor( position ) );
    const auto high = std::min( low + 1, sortedValues.size() - 1 );
    const double fraction = position - double( low );
    return double( sortedValues[low] ) + ( double( sortedValues[high] ) - double( sortedValues[low] ) ) * fraction;
}

Statistics ComputeStatistics( const std::vector<int64_t>& values, double truncatePercentile )
{
    if( truncatePercentile <= 0 || truncatePercentile > 1 ) throw std::invalid_argument( "truncate percentile must be in (0, 1]" );

    Statistics result;
    if( values.empty() ) return result;

    std::vector<int64_t> sorted = values;
    std::sort( sorted.begin(), sorted.end() );

    result.count = sorted.size();
    result.min = sorted.front();
    result.max = sorted.back();
    result.total = std::accumulate( sorted.begin(), sorted.end(), int64_t( 0 ) );
    result.mean = double( result.total ) / double( result.count );
    result.median = Percentile( sorted, 0.50 );
    result.p50 = result.median;
    result.p90 = Percentile( sorted, 0.90 );
    result.p95 = Percentile( sorted, 0.95 );
    result.p99 = Percentile( sorted, 0.99 );

    long double squaredDeviation = 0;
    for( const auto value : sorted )
    {
        const long double delta = static_cast<long double>( value ) - result.mean;
        squaredDeviation += delta * delta;
    }
    result.stddev = std::sqrt( double( squaredDeviation / result.count ) );

    const double cutoff = Percentile( sorted, truncatePercentile );
    long double truncatedTotal = 0;
    size_t truncatedCount = 0;
    for( const auto value : sorted )
    {
        if( double( value ) > cutoff ) break;
        truncatedTotal += value;
        truncatedCount++;
    }
    result.truncatedMean = truncatedCount == 0 ? 0 : double( truncatedTotal / truncatedCount );
    return result;
}

}

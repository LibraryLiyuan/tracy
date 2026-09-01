#ifndef __TRACYTRACESESSIONSTATISTICS_HPP__
#define __TRACYTRACESESSIONSTATISTICS_HPP__

#include "TracyTraceSessionExternalSort.hpp"
#include "TracyTraceSource.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace tracy::analysis
{

inline ExactStatisticsDto ReadTraceSessionExactStatistics(
    const std::filesystem::path& path, uint64_t offset, uint64_t count )
{
    ExactStatisticsDto result;
    result.count = count;
    if( count == 0 ) return result;
    std::ifstream in( path, std::ios::binary );
    if( !in ) throw std::runtime_error( "Session exact statistics are unavailable" );
    const auto valueAt = [&]( uint64_t index ) -> int64_t {
        TraceSessionUInt64Pair pair;
        in.clear();
        in.seekg( std::streamoff( offset + index * sizeof( pair ) ) );
        if( !in.read( reinterpret_cast<char*>( &pair ), sizeof( pair ) ) )
            throw std::runtime_error( "Session exact statistics are truncated" );
        return int64_t( pair.key ^ ( uint64_t( 1 ) << 63 ) );
    };
    const auto percentile = [&]( double value ) {
        const double position = value * double( count - 1 );
        const auto low = uint64_t( std::floor( position ) );
        const auto high = std::min<uint64_t>( low + 1, count - 1 );
        const auto fraction = position - double( low );
        const auto lowValue = valueAt( low );
        const auto highValue = valueAt( high );
        return double( lowValue ) +
            ( double( highValue ) - double( lowValue ) ) * fraction;
    };
    result.min = valueAt( 0 );
    result.max = valueAt( count - 1 );
    in.clear();
    in.seekg( std::streamoff( offset ) );
    for( uint64_t index = 0; index < count; ++index )
    {
        TraceSessionUInt64Pair pair;
        if( !in.read( reinterpret_cast<char*>( &pair ), sizeof( pair ) ) )
            throw std::runtime_error( "Session exact statistics are truncated" );
        result.total += int64_t( pair.key ^ ( uint64_t( 1 ) << 63 ) );
    }
    result.mean = double( result.total ) / double( count );
    result.median = percentile( 0.50 );
    result.p50 = result.median;
    result.p90 = percentile( 0.90 );
    result.p95 = percentile( 0.95 );
    result.p99 = percentile( 0.99 );
    const auto cutoff = result.p90;
    long double squaredDeviation = 0;
    long double truncatedTotal = 0;
    uint64_t truncatedCount = 0;
    in.clear();
    in.seekg( std::streamoff( offset ) );
    for( uint64_t index = 0; index < count; ++index )
    {
        TraceSessionUInt64Pair pair;
        if( !in.read( reinterpret_cast<char*>( &pair ), sizeof( pair ) ) )
            throw std::runtime_error( "Session exact statistics are truncated" );
        const auto value = int64_t( pair.key ^ ( uint64_t( 1 ) << 63 ) );
        const long double delta = static_cast<long double>( value ) - result.mean;
        squaredDeviation += delta * delta;
        if( double( value ) <= cutoff )
        {
            truncatedTotal += value;
            ++truncatedCount;
        }
    }
    result.stddev = std::sqrt( double( squaredDeviation / count ) );
    result.truncatedMean = truncatedCount == 0 ? 0 :
        double( truncatedTotal / truncatedCount );
    return result;
}

}

#endif

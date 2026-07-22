#ifndef __TRACYANALYSIS_HPP__
#define __TRACYANALYSIS_HPP__

#include "TracyTraceSource.hpp"

#include <cstdint>
#include <vector>

namespace tracy::analysis
{

struct Statistics
{
    uint64_t count = 0;
    int64_t total = 0;
    int64_t min = 0;
    int64_t max = 0;
    double mean = 0;
    double median = 0;
    double stddev = 0;
    double p50 = 0;
    double p90 = 0;
    double p95 = 0;
    double p99 = 0;
    double truncatedMean = 0;
};

Statistics ComputeStatistics( const std::vector<int64_t>& values, double truncatePercentile = 0.90 );
double Percentile( const std::vector<int64_t>& sortedValues, double percentile );

}

#endif

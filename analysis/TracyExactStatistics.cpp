#include "TracyExactStatistics.hpp"

#include "TracyHash.hpp"
#include "TracyAnalysisExternalSort.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <nlohmann/json.hpp>
#include <queue>
#include <set>
#include <sstream>
#include <tuple>
#include <type_traits>
#include <unordered_map>

namespace tracy::analysis
{
namespace
{

using nlohmann::json;

struct SignatureKey
{
    std::string domain;
    std::string signature;
    std::string frameScope;

    bool operator<( const SignatureKey& other ) const
    {
        return std::tie( domain, signature, frameScope ) <
            std::tie( other.domain, other.signature, other.frameScope );
    }
};

struct FrameKey
{
    SignatureKey signature;
    uint64_t frame = 0;

    bool operator<( const FrameKey& other ) const
    {
        if( signature < other.signature ) return true;
        if( other.signature < signature ) return false;
        return frame < other.frame;
    }
};

struct MergedRun
{
    int64_t inclusive = 0;
    int64_t exclusive = 0;
    int64_t wait = 0;
    int64_t critical = 0;
    uint64_t occurrences = 0;
    bool exact = true;
    bool logical = false;
};

struct MetricPoint
{
    uint64_t frame = 0;
    int64_t value = 0;
};

bool AddChecked( int64_t& target, int64_t value )
{
    if( value < 0 || target < 0 || value > std::numeric_limits<int64_t>::max() - target )
        return false;
    target += value;
    return true;
}

bool AddChecked( uint64_t& target, uint64_t value )
{
    if( value > std::numeric_limits<uint64_t>::max() - target ) return false;
    target += value;
    return true;
}

bool IsHexDigest( const std::string& value )
{
    return value.size() == 64 && std::all_of( value.begin(), value.end(), []( const unsigned char ch ) {
        return std::isxdigit( ch ) != 0;
    } );
}

bool IsDomainStatus( const std::string& value )
{
    return value == "complete" || value == "absent" || value == "invalid" || value == "unsupported";
}

std::string SafePrefix( const std::string& value )
{
    std::string result;
    result.reserve( std::min<size_t>( value.size(), 80 ) );
    for( const unsigned char ch : value )
    {
        if( result.size() == 80 ) break;
        result.push_back( std::isalnum( ch ) != 0 || ch == '-' || ch == '_' ? char( ch ) : '_' );
    }
    return result.empty() ? "statistics" : result;
}

void RemoveQuietly( const std::filesystem::path& path )
{
    std::error_code ignored;
    std::filesystem::remove( path, ignored );
}

bool WritePairs( const std::filesystem::path& path, const std::vector<int64_t>& values,
    std::string& error )
{
    std::ofstream output( path, std::ios::binary | std::ios::trunc );
    if( !output ) { error = "exact_statistics_source_open_failed"; return false; }
    uint64_t ordinal = 0;
    for( const auto value : values )
    {
        if( value < 0 ) { error = "negative_duration_not_supported"; return false; }
        const AnalysisUInt64Pair pair { uint64_t( value ), ordinal++ };
        output.write( reinterpret_cast<const char*>( &pair ), sizeof( pair ) );
        if( !output ) { error = "exact_statistics_source_write_failed"; return false; }
    }
    output.flush();
    if( !output ) { error = "exact_statistics_source_flush_failed"; return false; }
    return true;
}

class SortedPairs
{
public:
    SortedPairs( const std::filesystem::path& path, uint64_t count )
        : m_input( path, std::ios::binary )
        , m_count( count )
    {}

    bool Valid() const { return bool( m_input ); }
    uint64_t Count() const { return m_count; }
    void Close() { m_input.close(); }

    bool At( uint64_t index, uint64_t& value )
    {
        if( index >= m_count || index > uint64_t( std::numeric_limits<std::streamoff>::max() ) /
            sizeof( AnalysisUInt64Pair ) ) return false;
        m_input.clear();
        m_input.seekg( std::streamoff( index * sizeof( AnalysisUInt64Pair ) ), std::ios::beg );
        AnalysisUInt64Pair pair;
        m_input.read( reinterpret_cast<char*>( &pair ), sizeof( pair ) );
        if( !m_input ) return false;
        value = pair.key;
        return true;
    }

    bool LowerBound( uint64_t needle, uint64_t& index )
    {
        uint64_t first = 0;
        uint64_t last = m_count;
        while( first < last )
        {
            const auto middle = first + ( last - first ) / 2;
            uint64_t value = 0;
            if( !At( middle, value ) ) return false;
            if( value < needle ) first = middle + 1;
            else last = middle;
        }
        index = first;
        return true;
    }

private:
    std::ifstream m_input;
    uint64_t m_count = 0;
};

bool ValueWithZeroPrefix( SortedPairs& explicitValues, uint64_t implicitZeros,
    uint64_t index, uint64_t& value )
{
    if( index < implicitZeros ) { value = 0; return true; }
    return explicitValues.At( index - implicitZeros, value );
}

bool ValueWithRepeatedInsertion( SortedPairs& explicitValues, uint64_t repeatedValue,
    uint64_t repeatedCount, uint64_t lowerCount, uint64_t index, uint64_t& value )
{
    if( index < lowerCount ) return explicitValues.At( index, value );
    if( index - lowerCount < repeatedCount ) { value = repeatedValue; return true; }
    return explicitValues.At( index - repeatedCount, value );
}

template<typename Reader>
bool ExactPercentile( uint64_t count, double percentile, Reader&& reader, double& value )
{
    if( count == 0 ) { value = 0; return true; }
    const auto position = percentile * double( count - 1 );
    const auto lower = uint64_t( std::floor( position ) );
    const auto upper = uint64_t( std::ceil( position ) );
    uint64_t lowValue = 0;
    uint64_t highValue = 0;
    if( !reader( lower, lowValue ) || !reader( upper, highValue ) ) return false;
    value = double( lowValue ) + ( double( highValue ) - double( lowValue ) ) *
        ( position - double( lower ) );
    return true;
}

std::string DoubleString( double value )
{
    if( value == 0 ) return "0";
    std::ostringstream stream;
    stream.imbue( std::locale::classic() );
    stream << std::setprecision( 17 ) << value;
    return stream.str();
}

json DistributionJson( const ExactDistribution& value )
{
    return {
        { "exact", value.exact }, { "count", std::to_string( value.count ) },
        { "zero_count", std::to_string( value.zeroCount ) },
        { "total_ns", std::to_string( value.total ) }, { "min_ns", std::to_string( value.min ) },
        { "max_ns", std::to_string( value.max ) }, { "mean_ns", DoubleString( value.mean ) },
        { "median_ns", DoubleString( value.median ) }, { "mad_ns", DoubleString( value.mad ) },
        { "p50_ns", DoubleString( value.p50 ) }, { "p90_ns", DoubleString( value.p90 ) },
        { "p95_ns", DoubleString( value.p95 ) }, { "p99_ns", DoubleString( value.p99 ) },
        { "unavailable_reason", value.unavailableReason.empty() ? json( nullptr ) : json( value.unavailableReason ) }
    };
}

json MetricJson( const NeutralMetricAggregate& value )
{
    json anomalies = json::array();
    for( const auto& anomaly : value.anomalies )
    {
        anomalies.push_back( {
            { "frame_index", std::to_string( anomaly.frameIndex ) },
            { "value_ns", std::to_string( anomaly.valueNs ) },
            { "delta_from_median_ns", DoubleString( anomaly.deltaFromMedianNs ) }
        } );
    }
    return {
        { "per_complete_frame", DistributionJson( value.perCompleteFrame ) },
        { "when_present", DistributionJson( value.whenPresent ) },
        { "pattern", AnomalyPatternName( value.pattern ) },
        { "anomaly_count", std::to_string( value.anomalyCount ) },
        { "anomalies_complete", value.anomaliesComplete },
        { "anomalies", std::move( anomalies ) },
        { "longest_burst_frames", std::to_string( value.longestBurstFrames ) },
        { "longest_burst_start_frame", value.longestBurstStartFrame ?
            json( std::to_string( *value.longestBurstStartFrame ) ) : json( nullptr ) },
        { "longest_burst_end_frame", value.longestBurstEndFrame ?
            json( std::to_string( *value.longestBurstEndFrame ) ) : json( nullptr ) },
        { "longest_burst_peak_frame", value.longestBurstPeakFrame ?
            json( std::to_string( *value.longestBurstPeakFrame ) ) : json( nullptr ) },
        { "period_frames", value.periodFrames ? json( std::to_string( *value.periodFrames ) ) : json( nullptr ) }
    };
}

json RankingJson( const std::vector<NeutralRankingEntry>& values )
{
    json result = json::array();
    for( const auto& value : values )
    {
        result.push_back( {
            { "signature_id", value.signatureId }, { "frame_scope", value.frameScope },
            { "logical", value.logical }, { "total_ns", std::to_string( value.totalNs ) },
            { "wait_ns", std::to_string( value.waitNs ) },
            { "critical_path_ns", std::to_string( value.criticalPathNs ) },
            { "contribution", DoubleString( value.contribution ) },
            { "cumulative_contribution", DoubleString( value.cumulativeContribution ) }
        } );
    }
    return result;
}

std::string SerializeResult( const NeutralStatisticsResult& value, bool includeHash )
{
    json document = {
        { "schema_version", NeutralAggregateSchemaVersion },
        { "algorithm", NeutralScanAlgorithmId },
        { "quality", {
            { "complete", value.qualityComplete },
            { "unreported_gap_count", std::to_string( value.unreportedGapCount ) },
            { "findings", value.qualityFindings }
        } },
        { "maximum_buffered_values_observed", std::to_string( value.maximumBufferedValuesObserved ) },
        { "domains", json::array() }, { "signatures", json::array() }, { "rankings", json::array() }
    };
    if( includeHash ) document["content_sha256"] = value.contentSha256;

    for( const auto& domain : value.domains )
    {
        document["domains"].push_back( {
            { "domain", domain.domain }, { "present", domain.present }, { "status", domain.status },
            { "input_count", std::to_string( domain.inputCount ) },
            { "consumed_input_count", std::to_string( domain.consumedInputCount ) },
            { "output_count", std::to_string( domain.outputCount ) },
            { "actual_output_count", std::to_string( domain.actualOutputCount ) },
            { "input_checksum", domain.inputChecksum }, { "consumed_checksum", domain.consumedChecksum },
            { "quality_complete", domain.qualityComplete },
            { "unavailable_reason", domain.unavailableReason.empty() ? json( nullptr ) : json( domain.unavailableReason ) }
        } );
    }
    for( const auto& signature : value.signatures )
    {
        document["signatures"].push_back( {
            { "domain", signature.domain }, { "signature_id", signature.signatureId },
            { "frame_scope", signature.frameScope }, { "logical", signature.logical },
            { "exact", signature.exact },
            { "complete_frame_count", std::to_string( signature.completeFrameCount ) },
            { "present_frame_count", std::to_string( signature.presentFrameCount ) },
            { "occurrence_count", std::to_string( signature.occurrenceCount ) },
            { "unknown_frame_count", std::to_string( signature.unknownFrameCount ) },
            { "inclusive", MetricJson( signature.inclusive ) },
            { "exclusive", MetricJson( signature.exclusive ) },
            { "wait", MetricJson( signature.wait ) },
            { "critical_path", MetricJson( signature.criticalPath ) }
        } );
    }
    for( const auto& ranking : value.rankings )
    {
        document["rankings"].push_back( {
            { "domain", ranking.domain }, { "inclusive", RankingJson( ranking.inclusive ) },
            { "exclusive", RankingJson( ranking.exclusive ) },
            { "wait_critical", RankingJson( ranking.waitCritical ) }
        } );
    }
    return document.dump();
}

std::string Sha256Text( const std::string& value )
{
    Sha256Builder hash;
    hash.Update( value.data(), value.size() );
    return hash.FinalHex();
}

NeutralMetricAggregate BuildMetric( const std::vector<MetricPoint>& points,
    uint64_t completeFrames, const std::filesystem::path& root, const std::string& prefix,
    uint64_t maximumBufferedValues )
{
    NeutralMetricAggregate result;
    std::vector<int64_t> values;
    values.reserve( points.size() );
    for( const auto& point : points ) values.push_back( point.value );
    result.whenPresent = ComputeExactDistributionExternal( values, 0, root,
        prefix + "-present", maximumBufferedValues );
    if( completeFrames < points.size() )
    {
        result.perCompleteFrame.exact = false;
        result.perCompleteFrame.unavailableReason = "complete_frame_denominator_smaller_than_present_frames";
        return result;
    }
    result.perCompleteFrame = ComputeExactDistributionExternal( values,
        completeFrames - points.size(), root, prefix + "-complete", maximumBufferedValues );
    if( !result.perCompleteFrame.exact || !result.whenPresent.exact ) return result;

    const auto median = result.perCompleteFrame.median;
    const auto mad = result.perCompleteFrame.mad;
    const auto threshold = mad == 0 ? median : median + 6.0 * mad;
    constexpr size_t EdgeRepresentativeCount = 8;
    constexpr size_t TopRepresentativeCount = 8;
    std::vector<AnomalyInstance> provisional;
    std::vector<AnomalyInstance> first;
    std::vector<AnomalyInstance> last;
    std::vector<AnomalyInstance> top;
    provisional.reserve( NeutralMaximumRepresentativeAnomalies );
    first.reserve( EdgeRepresentativeCount );
    last.reserve( EdgeRepresentativeCount );
    top.reserve( TopRepresentativeCount + 1 );

    bool havePrevious = false;
    uint64_t previousFrame = 0;
    bool periodic = true;
    std::optional<uint64_t> period;

    bool haveBurst = false;
    uint64_t burstLength = 0;
    AnomalyInstance burstStart;
    AnomalyInstance burstEnd;
    AnomalyInstance burstPeak;
    AnomalyInstance bestStart;
    AnomalyInstance bestEnd;
    AnomalyInstance bestPeak;
    auto finishBurst = [&] {
        if( !haveBurst ) return;
        if( burstLength > result.longestBurstFrames ||
            ( burstLength == result.longestBurstFrames &&
              burstStart.frameIndex < bestStart.frameIndex ) )
        {
            result.longestBurstFrames = burstLength;
            bestStart = burstStart;
            bestEnd = burstEnd;
            bestPeak = burstPeak;
        }
    };

    for( const auto& point : points )
    {
        if( double( point.value ) <= threshold ) continue;
        const AnomalyInstance anomaly { point.frame, point.value,
            double( point.value ) - median };
        ++result.anomalyCount;
        if( provisional.size() < NeutralMaximumRepresentativeAnomalies )
            provisional.push_back( anomaly );
        if( first.size() < EdgeRepresentativeCount ) first.push_back( anomaly );
        last.push_back( anomaly );
        if( last.size() > EdgeRepresentativeCount ) last.erase( last.begin() );
        top.push_back( anomaly );
        std::sort( top.begin(), top.end(), []( const auto& left, const auto& right ) {
            if( left.valueNs != right.valueNs ) return left.valueNs > right.valueNs;
            return left.frameIndex < right.frameIndex;
        } );
        if( top.size() > TopRepresentativeCount ) top.resize( TopRepresentativeCount );

        if( havePrevious )
        {
            const auto gap = anomaly.frameIndex - previousFrame;
            if( !period ) period = gap;
            else if( *period != gap ) periodic = false;
        }
        previousFrame = anomaly.frameIndex;

        if( !haveBurst || !havePrevious || anomaly.frameIndex != burstEnd.frameIndex + 1 )
        {
            finishBurst();
            haveBurst = true;
            burstLength = 1;
            burstStart = burstEnd = burstPeak = anomaly;
        }
        else
        {
            ++burstLength;
            burstEnd = anomaly;
            if( anomaly.valueNs > burstPeak.valueNs ||
                ( anomaly.valueNs == burstPeak.valueNs && anomaly.frameIndex < burstPeak.frameIndex ) )
                burstPeak = anomaly;
        }
        havePrevious = true;
    }
    finishBurst();

    result.anomaliesComplete = result.anomalyCount <= NeutralMaximumRepresentativeAnomalies;
    if( result.anomaliesComplete )
    {
        // Do not move the provisional 32-slot reservation into every finished
        // metric, including empty wait/critical metrics. With millions of
        // signatures that retained capacity alone occupies several GiB.
        // Preserve every representative and its ordering; only discard slack.
        result.anomalies.assign( provisional.begin(), provisional.end() );
    }
    else
    {
        std::map<uint64_t, AnomalyInstance> representatives;
        const auto add = [&]( const AnomalyInstance& value ) {
            representatives.emplace( value.frameIndex, value );
        };
        for( const auto& value : first ) add( value );
        for( const auto& value : last ) add( value );
        for( const auto& value : top ) add( value );
        if( result.longestBurstFrames != 0 )
        {
            add( bestStart );
            add( bestPeak );
            add( bestEnd );
        }
        result.anomalies.reserve( representatives.size() );
        for( const auto& [frame, value] : representatives ) result.anomalies.push_back( value );
    }
    if( result.longestBurstFrames != 0 )
    {
        result.longestBurstStartFrame = bestStart.frameIndex;
        result.longestBurstEndFrame = bestEnd.frameIndex;
        result.longestBurstPeakFrame = bestPeak.frameIndex;
    }
    if( result.anomalyCount >= 3 && periodic && period && *period != 0 )
        result.periodFrames = *period;
    if( result.longestBurstFrames >= 3 ) result.pattern = AnomalyPattern::BurstWindow;
    else if( result.anomalyCount >= 2 ) result.pattern = AnomalyPattern::RecurrentSpike;
    else if( result.anomalyCount == 1 ) result.pattern = AnomalyPattern::IsolatedSpike;
    else if( result.anomalyCount == 0 && completeFrames != 0 && median > 0 &&
        double( points.size() ) / double( completeFrames ) >= 0.8 &&
        mad <= std::abs( median ) * 0.10 )
        result.pattern = AnomalyPattern::PersistentPressure;
    return result;
}

void FinalizeRanking( std::vector<NeutralRankingEntry>& values )
{
    std::sort( values.begin(), values.end(), []( const auto& left, const auto& right ) {
        if( left.totalNs != right.totalNs ) return left.totalNs > right.totalNs;
        if( left.signatureId != right.signatureId ) return left.signatureId < right.signatureId;
        return left.frameScope < right.frameScope;
    } );
    long double total = 0;
    for( const auto& value : values ) if( value.totalNs > 0 ) total += value.totalNs;
    long double cumulative = 0;
    for( auto& value : values )
    {
        value.contribution = total == 0 ? 0 : double(
            static_cast<long double>( std::max<int64_t>( value.totalNs, 0 ) ) / total );
        cumulative += value.contribution;
        value.cumulativeContribution = std::min( 1.0, double( cumulative ) );
    }
    if( total != 0 && !values.empty() ) values.back().cumulativeContribution = 1.0;
}

uint64_t AbsoluteDifference( uint64_t left, uint64_t right )
{
    return left > right ? left - right : right - left;
}

uint64_t UnixTimeNs()
{
    return uint64_t( std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch() ).count() );
}

ExactDistribution ComputeExactDistributionBounded( const std::vector<int64_t>& explicitValues,
    uint64_t implicitZeros )
{
    ExactDistribution result;
    if( explicitValues.size() > std::numeric_limits<uint64_t>::max() - implicitZeros )
    {
        result.exact = false;
        result.unavailableReason = "statistics_count_overflow";
        return result;
    }

    result.count = uint64_t( explicitValues.size() ) + implicitZeros;
    result.zeroCount = implicitZeros;
    if( result.count == 0 ) return result;

    std::vector<uint64_t> values;
    values.reserve( explicitValues.size() );
    int64_t total = 0;
    for( const auto value : explicitValues )
    {
        if( value < 0 )
        {
            result.exact = false;
            result.unavailableReason = "negative_duration_not_supported";
            return result;
        }
        if( value == 0 ) ++result.zeroCount;
        if( !AddChecked( total, value ) )
        {
            result.exact = false;
            result.unavailableReason = "statistics_total_overflow";
            return result;
        }
        values.emplace_back( uint64_t( value ) );
    }
    std::sort( values.begin(), values.end() );

    const auto reader = [&]( uint64_t index, uint64_t& value ) {
        if( index < implicitZeros )
        {
            value = 0;
            return true;
        }
        const auto explicitIndex = index - implicitZeros;
        if( explicitIndex >= values.size() ) return false;
        value = values[size_t( explicitIndex )];
        return true;
    };

    result.total = total;
    result.mean = double( total ) / double( result.count );
    uint64_t minimum = 0;
    uint64_t maximum = 0;
    if( !reader( 0, minimum ) || !reader( result.count - 1, maximum ) ||
        !ExactPercentile( result.count, 0.50, reader, result.median ) ||
        !ExactPercentile( result.count, 0.90, reader, result.p90 ) ||
        !ExactPercentile( result.count, 0.95, reader, result.p95 ) ||
        !ExactPercentile( result.count, 0.99, reader, result.p99 ) )
    {
        result.exact = false;
        result.unavailableReason = "statistics_bounded_read_failed";
        return result;
    }
    result.min = int64_t( minimum );
    result.max = int64_t( maximum );
    result.p50 = result.median;

    uint64_t medianLow = 0;
    uint64_t medianHigh = 0;
    if( !reader( ( result.count - 1 ) / 2, medianLow ) ||
        !reader( result.count / 2, medianHigh ) )
    {
        result.exact = false;
        result.unavailableReason = "statistics_bounded_median_failed";
        return result;
    }
    const uint64_t median2 = medianLow + medianHigh;
    std::vector<uint64_t> deviations;
    deviations.reserve( values.size() );
    for( const auto value : values )
    {
        const auto value2 = value * 2;
        deviations.emplace_back( value2 >= median2 ? value2 - median2 : median2 - value2 );
    }
    std::sort( deviations.begin(), deviations.end() );
    const auto insertion = uint64_t( std::lower_bound(
        deviations.begin(), deviations.end(), median2 ) - deviations.begin() );
    const auto deviationReader = [&]( uint64_t index, uint64_t& value ) {
        if( index < insertion )
        {
            value = deviations[size_t( index )];
            return true;
        }
        if( index - insertion < implicitZeros )
        {
            value = median2;
            return true;
        }
        const auto explicitIndex = index - implicitZeros;
        if( explicitIndex >= deviations.size() ) return false;
        value = deviations[size_t( explicitIndex )];
        return true;
    };
    double scaledMad = 0;
    if( !ExactPercentile( result.count, 0.50, deviationReader, scaledMad ) )
    {
        result.exact = false;
        result.unavailableReason = "statistics_bounded_mad_failed";
        return result;
    }
    result.mad = scaledMad / 2.0;
    return result;
}

}

const char* AnomalyPatternName( AnomalyPattern pattern )
{
    switch( pattern )
    {
    case AnomalyPattern::None: return "none";
    case AnomalyPattern::IsolatedSpike: return "isolated_spike";
    case AnomalyPattern::RecurrentSpike: return "recurrent_spike";
    case AnomalyPattern::BurstWindow: return "burst_window";
    case AnomalyPattern::PersistentPressure: return "persistent_pressure";
    }
    return "none";
}

ExactDistribution ComputeExactDistributionExternal( const std::vector<int64_t>& explicitValues,
    uint64_t implicitZeros, const std::filesystem::path& temporaryRoot,
    const std::string& rawPrefix, uint64_t maximumBufferedValues )
{
    if( maximumBufferedValues == 0 ) maximumBufferedValues = 1;
    if( explicitValues.size() <= maximumBufferedValues )
        return ComputeExactDistributionBounded( explicitValues, implicitZeros );

    ExactDistribution result;
    if( explicitValues.size() > std::numeric_limits<uint64_t>::max() - implicitZeros )
    {
        result.exact = false; result.unavailableReason = "statistics_count_overflow"; return result;
    }
    result.count = uint64_t( explicitValues.size() ) + implicitZeros;
    result.zeroCount = implicitZeros;
    if( result.count == 0 ) return result;
    std::error_code ec;
    std::filesystem::create_directories( temporaryRoot, ec );
    if( ec )
    {
        result.exact = false; result.unavailableReason = "statistics_temporary_directory_failed:" + ec.message();
        return result;
    }

    const auto prefix = SafePrefix( rawPrefix );
    const auto source = temporaryRoot / ( prefix + "-values-source.work" );
    const auto sorted = temporaryRoot / ( prefix + "-values-sorted.work" );
    const auto deviationSource = temporaryRoot / ( prefix + "-deviation-source.work" );
    const auto deviationSorted = temporaryRoot / ( prefix + "-deviation-sorted.work" );
    const auto cleanup = [&]() {
        RemoveQuietly( source ); RemoveQuietly( sorted );
        RemoveQuietly( deviationSource ); RemoveQuietly( deviationSorted );
        std::string ignored;
        CleanupAnalysisExternalSortFiles( temporaryRoot, prefix, ignored );
    };
    cleanup();

    int64_t total = 0;
    for( const auto value : explicitValues )
    {
        if( value < 0 )
        {
            result.exact = false; result.unavailableReason = "negative_duration_not_supported";
            cleanup(); return result;
        }
        if( value == 0 ) ++result.zeroCount;
        if( !AddChecked( total, value ) )
        {
            result.exact = false; result.unavailableReason = "statistics_total_overflow";
            cleanup(); return result;
        }
    }
    result.total = total;
    result.mean = double( total ) / double( result.count );

    std::string error;
    if( !WritePairs( source, explicitValues, error ) ||
        !SortAnalysisUInt64Pairs( source, sorted, temporaryRoot, prefix + "-values",
            explicitValues.size(), maximumBufferedValues, error ) )
    {
        result.exact = false; result.unavailableReason = error; cleanup(); return result;
    }
    SortedPairs accessor( sorted, explicitValues.size() );
    if( !accessor.Valid() )
    {
        result.exact = false; result.unavailableReason = "statistics_sorted_open_failed"; cleanup(); return result;
    }
    const auto reader = [&]( uint64_t index, uint64_t& value ) {
        return ValueWithZeroPrefix( accessor, implicitZeros, index, value );
    };
    uint64_t minimum = 0;
    uint64_t maximum = 0;
    if( !reader( 0, minimum ) || !reader( result.count - 1, maximum ) ||
        !ExactPercentile( result.count, 0.50, reader, result.median ) ||
        !ExactPercentile( result.count, 0.90, reader, result.p90 ) ||
        !ExactPercentile( result.count, 0.95, reader, result.p95 ) ||
        !ExactPercentile( result.count, 0.99, reader, result.p99 ) )
    {
        accessor.Close();
        result.exact = false; result.unavailableReason = "statistics_sorted_read_failed"; cleanup(); return result;
    }
    result.min = int64_t( minimum );
    result.max = int64_t( maximum );
    result.p50 = result.median;

    uint64_t medianLow = 0;
    uint64_t medianHigh = 0;
    if( !reader( ( result.count - 1 ) / 2, medianLow ) || !reader( result.count / 2, medianHigh ) )
    {
        accessor.Close();
        result.exact = false; result.unavailableReason = "statistics_median_read_failed"; cleanup(); return result;
    }
    const uint64_t median2 = medianLow + medianHigh;
    std::ofstream deviations( deviationSource, std::ios::binary | std::ios::trunc );
    if( !deviations )
    {
        accessor.Close();
        result.exact = false; result.unavailableReason = "statistics_deviation_open_failed"; cleanup(); return result;
    }
    for( uint64_t index = 0; index < explicitValues.size(); ++index )
    {
        const uint64_t value2 = uint64_t( explicitValues[size_t( index )] ) * 2;
        const AnalysisUInt64Pair pair {
            value2 >= median2 ? value2 - median2 : median2 - value2, index
        };
        deviations.write( reinterpret_cast<const char*>( &pair ), sizeof( pair ) );
        if( !deviations )
        {
            result.exact = false; result.unavailableReason = "statistics_deviation_write_failed";
            deviations.close(); accessor.Close(); cleanup(); return result;
        }
    }
    deviations.flush();
    deviations.close();
    if( !SortAnalysisUInt64Pairs( deviationSource, deviationSorted, temporaryRoot,
        prefix + "-deviation", explicitValues.size(), maximumBufferedValues, error ) )
    {
        accessor.Close();
        result.exact = false; result.unavailableReason = error; cleanup(); return result;
    }
    SortedPairs deviationAccessor( deviationSorted, explicitValues.size() );
    uint64_t lowerCount = 0;
    if( !deviationAccessor.Valid() || !deviationAccessor.LowerBound( median2, lowerCount ) )
    {
        accessor.Close();
        deviationAccessor.Close();
        result.exact = false; result.unavailableReason = "statistics_deviation_read_failed"; cleanup(); return result;
    }
    const auto deviationReader = [&]( uint64_t index, uint64_t& value ) {
        return ValueWithRepeatedInsertion( deviationAccessor, median2, implicitZeros,
            lowerCount, index, value );
    };
    double scaledMad = 0;
    if( !ExactPercentile( result.count, 0.50, deviationReader, scaledMad ) )
    {
        accessor.Close();
        deviationAccessor.Close();
        result.exact = false; result.unavailableReason = "statistics_mad_read_failed"; cleanup(); return result;
    }
    result.mad = scaledMad / 2.0;
    accessor.Close();
    deviationAccessor.Close();
    cleanup();
    return result;
}

namespace
{
struct PackedNeutralRun
{
    uint32_t signature = 0;
    uint32_t flags = 0;
    uint64_t frame = 0;
    int64_t inclusive = 0;
    int64_t exclusive = 0;
    int64_t wait = 0;
    int64_t critical = 0;
    uint64_t occurrences = 0;
};

static_assert( std::is_trivially_copyable_v<PackedNeutralRun> );

void FinalizeRankingsAndAudit( NeutralStatisticsResult& result,
    const std::vector<NeutralDomainAuditInput>& audits )
{
    using RankingScope = std::pair<std::string, std::string>;
    std::set<RankingScope> domainsWithPhysicalSignatures;
    for( const auto& signature : result.signatures )
        if( signature.exact && !signature.logical )
            domainsWithPhysicalSignatures.emplace( signature.domain, signature.frameScope );
    std::map<RankingScope, NeutralDomainRanking> rankings;
    for( const auto& signature : result.signatures )
    {
        if( !signature.exact ) continue;
        if( signature.logical && domainsWithPhysicalSignatures.contains( { signature.domain, signature.frameScope } ) ) continue;
        auto& ranking = rankings[{ signature.domain, signature.frameScope }];
        ranking.domain = signature.domain;
        ranking.inclusive.push_back( { signature.signatureId, signature.frameScope, signature.logical,
            signature.inclusive.perCompleteFrame.total } );
        ranking.exclusive.push_back( { signature.signatureId, signature.frameScope, signature.logical,
            signature.exclusive.perCompleteFrame.total } );
        const auto waitTotal = signature.wait.perCompleteFrame.total;
        const auto criticalTotal = signature.criticalPath.perCompleteFrame.total;
        ranking.waitCritical.push_back( { signature.signatureId, signature.frameScope, signature.logical,
            std::max( waitTotal, criticalTotal ), waitTotal, criticalTotal } );
    }
    for( auto& [domain, ranking] : rankings )
    {
        FinalizeRanking( ranking.inclusive );
        FinalizeRanking( ranking.exclusive );
        FinalizeRanking( ranking.waitCritical );
        result.rankings.push_back( std::move( ranking ) );
    }

    std::map<std::string, uint64_t> actualOutputs;
    for( const auto& signature : result.signatures ) ++actualOutputs[signature.domain];
    std::set<std::string> auditedDomains;
    for( const auto& audit : audits )
    {
        NeutralDomainAuditResult domain;
        domain.domain = audit.domain;
        domain.present = audit.present;
        domain.status = audit.status;
        domain.inputCount = audit.inputCount;
        domain.consumedInputCount = audit.consumedInputCount;
        domain.outputCount = audit.outputCount;
        domain.actualOutputCount = actualOutputs[audit.domain];
        domain.inputChecksum = audit.inputChecksum;
        domain.consumedChecksum = audit.consumedChecksum;
        domain.qualityComplete = audit.qualityComplete;
        domain.unavailableReason = audit.unavailableReason;
        const auto uniqueAudit = auditedDomains.emplace( audit.domain ).second;

        const auto inputGap = AbsoluteDifference( domain.inputCount, domain.consumedInputCount );
        const auto outputGap = AbsoluteDifference( domain.outputCount, domain.actualOutputCount );
        result.unreportedGapCount += inputGap + outputGap;
        if( domain.inputChecksum != domain.consumedChecksum ) ++result.unreportedGapCount;
        const auto contractValid = uniqueAudit && !domain.domain.empty() && IsDomainStatus( domain.status ) &&
            IsHexDigest( domain.inputChecksum ) && IsHexDigest( domain.consumedChecksum );
        if( !contractValid ) ++result.unreportedGapCount;
        if( inputGap != 0 || outputGap != 0 || domain.inputChecksum != domain.consumedChecksum ||
            !domain.qualityComplete || !contractValid )
        {
            result.qualityComplete = false;
            result.qualityFindings.push_back( "domain_audit_failed:" + domain.domain );
        }
        result.domains.push_back( std::move( domain ) );
    }
    for( const auto& [domain, count] : actualOutputs )
    {
        if( auditedDomains.contains( domain ) ) continue;
        result.qualityComplete = false;
        result.unreportedGapCount += count == 0 ? 1 : count;
        result.qualityFindings.push_back( "missing_domain_audit:" + domain );
    }
    std::sort( result.domains.begin(), result.domains.end(), []( const auto& left, const auto& right ) {
        return left.domain < right.domain;
    } );
    std::sort( result.qualityFindings.begin(), result.qualityFindings.end() );
    result.contentSha256 = Sha256Text( SerializeResult( result, false ) );
}
}

struct NeutralStatisticsStreamBuilder::Impl
{
    struct BufferedKey
    {
        uint32_t signature = 0;
        uint64_t frame = 0;
        bool operator==( const BufferedKey& other ) const
        { return signature == other.signature && frame == other.frame; }
    };
    struct BufferedKeyHash
    {
        size_t operator()( const BufferedKey& value ) const noexcept
        {
            const auto mixed = value.frame ^ ( value.frame >> 33 ) ^
                ( uint64_t( value.signature ) * 0x9E3779B185EBCA87ull );
            return size_t( mixed ^ ( mixed >> 29 ) );
        }
    };

    std::filesystem::path root;
    uint64_t maximumBufferedValues = 1;
    size_t maximumBufferedRuns = 1;
    size_t maximumBufferedRunsObserved = 0;
    uint64_t inputRunCount = 0;
    uint64_t mergeFailures = 0;
    uint64_t fileOrdinal = 0;
    std::vector<PackedNeutralRun> buffer;
    std::unordered_map<BufferedKey, size_t, BufferedKeyHash> bufferIndexes;
    std::vector<std::filesystem::path> runs;
    std::map<SignatureKey, uint32_t> signatureIndexes;
    std::vector<SignatureKey> signatures;
    std::vector<NeutralSignatureDenominatorInput> denominators;
    std::vector<NeutralDomainAuditInput> audits;
    std::string error;
    bool finished = false;

    Impl( std::filesystem::path temporaryRoot, uint64_t values, size_t rawRuns )
        : root( std::move( temporaryRoot ) )
        , maximumBufferedValues( std::max<uint64_t>( values, 1 ) )
        , maximumBufferedRuns( std::max<size_t>( rawRuns, 1 ) )
    {
        std::filesystem::create_directories( root );
        buffer.reserve( maximumBufferedRuns );
        bufferIndexes.reserve( maximumBufferedRuns );
    }

    const SignatureKey& Key( uint32_t index ) const { return signatures[index]; }
    bool Less( const PackedNeutralRun& left, const PackedNeutralRun& right ) const
    {
        // The registry index is unique for the complete (domain, signature,
        // frame-scope) key.  External runs only need a stable grouping key;
        // comparing the three strings for every sort/merge comparison made
        // large Session scans spend most of their time in lexical compares.
        if( left.signature != right.signature ) return left.signature < right.signature;
        return left.frame < right.frame;
    }
    bool SameFrame( const PackedNeutralRun& left, const PackedNeutralRun& right ) const
    {
        return left.signature == right.signature && left.frame == right.frame;
    }
    void Merge( PackedNeutralRun& target, const PackedNeutralRun& value )
    {
        const auto exact = ( target.flags & 1u ) != 0 && ( value.flags & 1u ) != 0;
        const auto logical = ( target.flags & 2u ) != 0 || ( value.flags & 2u ) != 0;
        target.flags = ( exact ? 1u : 0u ) | ( logical ? 2u : 0u );
        if( !AddChecked( target.inclusive, value.inclusive ) ||
            !AddChecked( target.exclusive, value.exclusive ) ||
            !AddChecked( target.wait, value.wait ) ||
            !AddChecked( target.critical, value.critical ) ||
            !AddChecked( target.occurrences, value.occurrences ) )
        {
            target.flags &= ~1u;
            ++mergeFailures;
        }
    }
    std::filesystem::path NextPath( std::string_view label )
    {
        return root / ( std::string( label ) + "-" + std::to_string( fileOrdinal++ ) + ".work" );
    }
    bool WriteCompacted( std::vector<PackedNeutralRun>& values, const std::filesystem::path& path )
    {
        std::sort( values.begin(), values.end(), [&]( const auto& left, const auto& right ) {
            return Less( left, right );
        } );
        std::ofstream output( path, std::ios::binary | std::ios::trunc );
        if( !output ) { error = "neutral_run_open_failed"; return false; }
        std::optional<PackedNeutralRun> pending;
        for( const auto& value : values )
        {
            if( pending && SameFrame( *pending, value ) ) Merge( *pending, value );
            else
            {
                if( pending ) output.write( reinterpret_cast<const char*>( &*pending ), sizeof( *pending ) );
                pending = value;
            }
            if( !output ) { error = "neutral_run_write_failed"; return false; }
        }
        if( pending ) output.write( reinterpret_cast<const char*>( &*pending ), sizeof( *pending ) );
        output.flush();
        if( !output ) { error = "neutral_run_flush_failed"; return false; }
        return true;
    }
    bool Flush()
    {
        if( buffer.empty() ) return true;
        maximumBufferedRunsObserved = std::max( maximumBufferedRunsObserved, buffer.size() );
        const auto path = NextPath( "neutral-run" );
        if( !WriteCompacted( buffer, path ) ) return false;
        runs.push_back( path );
        buffer.clear();
        bufferIndexes.clear();
        return true;
    }

    struct Reader
    {
        std::ifstream input;
        PackedNeutralRun value;
        bool valid = false;
        explicit Reader( const std::filesystem::path& path ) : input( path, std::ios::binary ) { Advance(); }
        void Advance()
        {
            input.read( reinterpret_cast<char*>( &value ), sizeof( value ) );
            valid = input.gcount() == sizeof( value );
            if( !valid && !input.eof() ) input.setstate( std::ios::failbit );
        }
    };

    bool MergeGroup( const std::vector<std::filesystem::path>& source,
        const std::filesystem::path& outputPath, const std::function<bool()>& cancelled )
    {
        std::vector<Reader> readers;
        readers.reserve( source.size() );
        for( const auto& path : source )
        {
            readers.emplace_back( path );
            if( !readers.back().input ) { error = "neutral_run_read_open_failed"; return false; }
        }
        const auto later = [&]( size_t left, size_t right ) {
            return Less( readers[right].value, readers[left].value );
        };
        std::priority_queue<size_t, std::vector<size_t>, decltype( later )> queue( later );
        for( size_t index = 0; index < readers.size(); ++index ) if( readers[index].valid ) queue.push( index );
        std::ofstream output( outputPath, std::ios::binary | std::ios::trunc );
        if( !output ) { error = "neutral_merge_open_failed"; return false; }
        std::optional<PackedNeutralRun> pending;
        uint64_t processed = 0;
        while( !queue.empty() )
        {
            if( ( ++processed & 0x3fff ) == 0 && cancelled && cancelled() )
            { error = "cancelled"; return false; }
            const auto index = queue.top(); queue.pop();
            const auto value = readers[index].value;
            readers[index].Advance();
            if( !readers[index].valid && !readers[index].input.eof() )
            { error = "neutral_merge_read_failed"; return false; }
            if( readers[index].valid ) queue.push( index );
            if( pending && SameFrame( *pending, value ) ) Merge( *pending, value );
            else
            {
                if( pending ) output.write( reinterpret_cast<const char*>( &*pending ), sizeof( *pending ) );
                pending = value;
            }
            if( !output ) { error = "neutral_merge_write_failed"; return false; }
        }
        if( pending ) output.write( reinterpret_cast<const char*>( &*pending ), sizeof( *pending ) );
        output.flush();
        if( !output ) { error = "neutral_merge_flush_failed"; return false; }
        return true;
    }

    bool Consolidate( const std::function<bool()>& cancelled )
    {
        constexpr size_t FanIn = 32;
        while( runs.size() > 1 )
        {
            std::vector<std::filesystem::path> next;
            for( size_t first = 0; first < runs.size(); first += FanIn )
            {
                const auto last = std::min( first + FanIn, runs.size() );
                std::vector<std::filesystem::path> group( runs.begin() + first, runs.begin() + last );
                const auto output = NextPath( "neutral-merge" );
                if( !MergeGroup( group, output, cancelled ) ) return false;
                next.push_back( output );
                for( const auto& path : group ) RemoveQuietly( path );
            }
            runs = std::move( next );
        }
        return true;
    }
};

NeutralStatisticsStreamBuilder::NeutralStatisticsStreamBuilder( std::filesystem::path temporaryRoot,
    uint64_t maximumBufferedValues, size_t maximumBufferedRuns )
    : m_impl( std::make_unique<Impl>( std::move( temporaryRoot ),
        maximumBufferedValues, maximumBufferedRuns ) )
{}

NeutralStatisticsStreamBuilder::~NeutralStatisticsStreamBuilder() = default;
NeutralStatisticsStreamBuilder::NeutralStatisticsStreamBuilder( NeutralStatisticsStreamBuilder&& ) noexcept = default;
NeutralStatisticsStreamBuilder& NeutralStatisticsStreamBuilder::operator=( NeutralStatisticsStreamBuilder&& ) noexcept = default;

bool NeutralStatisticsStreamBuilder::AddRun( const NeutralSignatureFrameInput& value )
{
    const auto signature = RegisterSignature( value.domain, value.signatureId, value.frameScope );
    if( signature == InvalidSignatureToken ) return false;
    return AddRegisteredRun( signature, value.frameIndex, value.inclusiveNs,
        value.exclusiveNs, value.waitNs, value.criticalPathNs,
        value.occurrenceCount, value.exact, value.logical );
}

NeutralStatisticsStreamBuilder::SignatureToken NeutralStatisticsStreamBuilder::RegisterSignature(
    std::string_view domain, std::string_view signatureId, std::string_view frameScope )
{
    if( !m_impl || m_impl->finished || !m_impl->error.empty() ) return InvalidSignatureToken;
    const SignatureKey key { std::string( domain ), std::string( signatureId ), std::string( frameScope ) };
    auto found = m_impl->signatureIndexes.find( key );
    if( found != m_impl->signatureIndexes.end() ) return found->second;
    if( m_impl->signatures.size() >= std::numeric_limits<uint32_t>::max() )
    { m_impl->error = "neutral_signature_limit_exceeded"; return InvalidSignatureToken; }
    const auto signature = uint32_t( m_impl->signatures.size() );
    m_impl->signatures.push_back( key );
    m_impl->signatureIndexes.emplace( key, signature );
    return signature;
}

bool NeutralStatisticsStreamBuilder::AddRegisteredRun( SignatureToken signature,
    uint64_t frameIndex, int64_t inclusiveNs, int64_t exclusiveNs,
    int64_t waitNs, int64_t criticalPathNs, uint64_t occurrenceCount,
    bool exact, bool logical )
{
    if( !m_impl || m_impl->finished || !m_impl->error.empty() ||
        signature >= m_impl->signatures.size() ) return false;
    const PackedNeutralRun value { signature,
        ( exact ? 1u : 0u ) | ( logical ? 2u : 0u ), frameIndex,
        inclusiveNs, exclusiveNs, waitNs, criticalPathNs, occurrenceCount };
    const Impl::BufferedKey key { signature, frameIndex };
    const auto found = m_impl->bufferIndexes.find( key );
    if( found != m_impl->bufferIndexes.end() ) m_impl->Merge( m_impl->buffer[found->second], value );
    else
    {
        const auto index = m_impl->buffer.size();
        m_impl->buffer.push_back( value );
        m_impl->bufferIndexes.emplace( key, index );
    }
    ++m_impl->inputRunCount;
    if( m_impl->buffer.size() >= m_impl->maximumBufferedRuns && !m_impl->Flush() ) return false;
    return true;
}

void NeutralStatisticsStreamBuilder::AddDenominator( const NeutralSignatureDenominatorInput& value )
{
    if( m_impl && !m_impl->finished ) m_impl->denominators.push_back( value );
}

void NeutralStatisticsStreamBuilder::AddDomainAudit( const NeutralDomainAuditInput& value )
{
    if( m_impl && !m_impl->finished ) m_impl->audits.push_back( value );
}

size_t NeutralStatisticsStreamBuilder::MaximumBufferedRunsObserved() const
{
    return m_impl ? std::max( m_impl->maximumBufferedRunsObserved, m_impl->buffer.size() ) : 0;
}

uint64_t NeutralStatisticsStreamBuilder::InputRunCount() const
{
    return m_impl ? m_impl->inputRunCount : 0;
}

bool NeutralStatisticsStreamBuilder::Finish( NeutralStatisticsResult& result, std::string& error,
    const NeutralSignatureFramesSink& signatureSink, const std::function<bool()>& cancelled )
{
    result = {};
    result.qualityComplete = true;
    error.clear();
    if( !m_impl || m_impl->finished ) { error = "neutral_stream_builder_already_finished"; return false; }
    m_impl->finished = true;
    if( !m_impl->error.empty() ) { error = m_impl->error; return false; }
    if( !m_impl->Flush() || !m_impl->Consolidate( cancelled ) )
    { error = m_impl->error; return false; }

    std::map<SignatureKey, uint64_t> denominators;
    for( const auto& value : m_impl->denominators )
    {
        const SignatureKey key { value.domain, value.signatureId, value.frameScope };
        const auto [found, inserted] = denominators.emplace( key, value.completeFrameCount );
        if( !inserted && found->second != value.completeFrameCount )
        {
            result.qualityComplete = false;
            ++result.unreportedGapCount;
            result.qualityFindings.push_back( "conflicting_complete_frame_denominator:" +
                value.domain + ":" + value.signatureId );
        }
    }
    if( m_impl->mergeFailures != 0 )
    {
        result.qualityComplete = false;
        result.unreportedGapCount += m_impl->mergeFailures;
        result.qualityFindings.push_back( "duration_or_occurrence_overflow_during_frame_merge" );
    }

    size_t ordinal = 0;
    auto finishSignature = [&]( uint32_t signatureIndex,
        std::vector<NeutralMergedFrameValues>& frames ) {
        if( frames.empty() ) return;
        const auto& signature = m_impl->Key( signatureIndex );
        NeutralSignatureAggregate aggregate;
        aggregate.domain = signature.domain;
        aggregate.signatureId = signature.signature;
        aggregate.frameScope = signature.frameScope;
        const auto denominator = denominators.find( signature );
        if( denominator == denominators.end() )
        {
            result.qualityComplete = false;
            ++result.unreportedGapCount;
            result.qualityFindings.push_back( "missing_complete_frame_denominator:" +
                signature.domain + ":" + signature.signature );
            aggregate.exact = false;
        }
        else aggregate.completeFrameCount = denominator->second;
        std::vector<MetricPoint> inclusive, exclusive, wait, critical;
        inclusive.reserve( frames.size() ); exclusive.reserve( frames.size() );
        wait.reserve( frames.size() ); critical.reserve( frames.size() );
        for( const auto& frame : frames )
        {
            aggregate.logical = aggregate.logical || frame.logical;
            if( !frame.exact ) { ++aggregate.unknownFrameCount; continue; }
            if( !AddChecked( aggregate.occurrenceCount, frame.occurrenceCount ) ) aggregate.exact = false;
            inclusive.push_back( { frame.frameIndex, frame.inclusiveNs } );
            exclusive.push_back( { frame.frameIndex, frame.exclusiveNs } );
            wait.push_back( { frame.frameIndex, frame.waitNs } );
            critical.push_back( { frame.frameIndex, frame.criticalPathNs } );
        }
        aggregate.presentFrameCount = inclusive.size();
        const auto knownFrameCount = aggregate.completeFrameCount >= aggregate.unknownFrameCount ?
            aggregate.completeFrameCount - aggregate.unknownFrameCount : 0;
        if( aggregate.unknownFrameCount > aggregate.completeFrameCount ) aggregate.exact = false;
        const auto itemPrefix = "signature-" + std::to_string( ordinal++ );
        aggregate.inclusive = BuildMetric( inclusive, knownFrameCount,
            m_impl->root, itemPrefix + "-inclusive", m_impl->maximumBufferedValues );
        aggregate.exclusive = BuildMetric( exclusive, knownFrameCount,
            m_impl->root, itemPrefix + "-exclusive", m_impl->maximumBufferedValues );
        aggregate.wait = BuildMetric( wait, knownFrameCount,
            m_impl->root, itemPrefix + "-wait", m_impl->maximumBufferedValues );
        aggregate.criticalPath = BuildMetric( critical, knownFrameCount,
            m_impl->root, itemPrefix + "-critical", m_impl->maximumBufferedValues );
        aggregate.exact = aggregate.exact && aggregate.inclusive.perCompleteFrame.exact &&
            aggregate.exclusive.perCompleteFrame.exact && aggregate.wait.perCompleteFrame.exact &&
            aggregate.criticalPath.perCompleteFrame.exact;
        if( !aggregate.exact )
        {
            result.qualityComplete = false;
            ++result.unreportedGapCount;
            result.qualityFindings.push_back( "non_exact_signature_statistics:" +
                signature.domain + ":" + signature.signature );
        }
        // Known observations remain useful, but a partial source cannot claim
        // an exact all-frame distribution. This is not a decoder/count gap.
        if( aggregate.unknownFrameCount != 0 )
        {
            aggregate.exact = false;
            for( auto* metric : { &aggregate.inclusive, &aggregate.exclusive,
                &aggregate.wait, &aggregate.criticalPath } )
            {
                metric->perCompleteFrame.exact = false;
                metric->perCompleteFrame.unavailableReason = "source_frames_unknown";
                metric->pattern = AnomalyPattern::None;
            }
        }
        result.maximumBufferedValuesObserved = std::max( result.maximumBufferedValuesObserved,
            std::min<uint64_t>( m_impl->maximumBufferedValues, aggregate.presentFrameCount ) );
        if( signatureSink ) signatureSink( aggregate, frames );
        result.signatures.push_back( std::move( aggregate ) );
    };

    if( !m_impl->runs.empty() )
    {
        Impl::Reader reader( m_impl->runs.front() );
        if( !reader.input ) { error = "neutral_final_run_open_failed"; return false; }
        uint32_t currentSignature = 0;
        bool haveSignature = false;
        std::vector<NeutralMergedFrameValues> frames;
        uint64_t processed = 0;
        while( reader.valid )
        {
            if( ( ++processed & 0x3fff ) == 0 && cancelled && cancelled() )
            { error = "cancelled"; return false; }
            const auto value = reader.value;
            reader.Advance();
            if( !reader.valid && !reader.input.eof() ) { error = "neutral_final_run_read_failed"; return false; }
            if( haveSignature && value.signature != currentSignature )
            {
                finishSignature( currentSignature, frames );
                frames.clear();
            }
            currentSignature = value.signature;
            haveSignature = true;
            frames.push_back( { value.frame, value.inclusive, value.exclusive,
                value.wait, value.critical, value.occurrences,
                ( value.flags & 1u ) != 0, ( value.flags & 2u ) != 0 } );
        }
        if( haveSignature ) finishSignature( currentSignature, frames );
        reader.input.close();
        RemoveQuietly( m_impl->runs.front() );
        m_impl->runs.clear();
    }

    // Numeric external sorting follows first-observation registry order.
    // Restore the public, input-order-independent lexical order before
    // ranking and hashing so persisted results remain deterministic.
    std::sort( result.signatures.begin(), result.signatures.end(), []( const auto& left, const auto& right ) {
        if( left.domain != right.domain ) return left.domain < right.domain;
        if( left.signatureId != right.signatureId ) return left.signatureId < right.signatureId;
        return left.frameScope < right.frameScope;
    } );
    FinalizeRankingsAndAudit( result, m_impl->audits );
    return true;
}

NeutralStatisticsResult BuildNeutralStatistics( const NeutralStatisticsInput& input )
{
    NeutralStatisticsStreamBuilder builder( input.temporaryRoot,
        input.maximumBufferedValues, 65536 );
    for( const auto& run : input.runs )
    {
        if( builder.AddRun( run ) ) continue;
        NeutralStatisticsResult failed;
        failed.qualityComplete = false;
        failed.unreportedGapCount = 1;
        failed.qualityFindings.push_back( "neutral_run_ingest_failed" );
        failed.contentSha256 = Sha256Text( SerializeResult( failed, false ) );
        return failed;
    }
    for( const auto& denominator : input.denominators ) builder.AddDenominator( denominator );
    for( const auto& audit : input.domainAudit ) builder.AddDomainAudit( audit );
    NeutralStatisticsResult result;
    std::string error;
    if( !builder.Finish( result, error ) )
    {
        result.qualityComplete = false;
        result.unreportedGapCount = 1;
        result.qualityFindings.push_back( error.empty() ? "neutral_statistics_stream_failed" : error );
        result.contentSha256 = Sha256Text( SerializeResult( result, false ) );
    }
    return result;
}

std::string SerializeNeutralStatisticsResult( const NeutralStatisticsResult& result )
{
    return SerializeResult( result, true );
}

bool DeserializeNeutralStatisticsResult( const std::string& payload,
    NeutralStatisticsResult& result, std::string& error )
{
    error.clear();
    result = {};
    try
    {
        const auto document = json::parse( payload );
        if( document.value( "schema_version", 0u ) != NeutralAggregateSchemaVersion ||
            document.value( "algorithm", std::string() ) != NeutralScanAlgorithmId )
        {
            error = "neutral_statistics_schema_mismatch";
            return false;
        }
        auto u64 = []( const json& value ) -> uint64_t {
            if( value.is_string() ) return std::stoull( value.get<std::string>() );
            return value.get<uint64_t>();
        };
        auto i64 = []( const json& value ) -> int64_t {
            if( value.is_string() ) return std::stoll( value.get<std::string>() );
            return value.get<int64_t>();
        };
        auto real = []( const json& value ) -> double {
            if( value.is_string() ) return std::stod( value.get<std::string>() );
            return value.get<double>();
        };
        auto distribution = [&]( const json& value, ExactDistribution& output ) {
            output.exact = value.at( "exact" ).get<bool>();
            output.count = u64( value.at( "count" ) );
            output.zeroCount = u64( value.at( "zero_count" ) );
            output.total = i64( value.at( "total_ns" ) );
            output.min = i64( value.at( "min_ns" ) );
            output.max = i64( value.at( "max_ns" ) );
            output.mean = real( value.at( "mean_ns" ) );
            output.median = real( value.at( "median_ns" ) );
            output.mad = real( value.at( "mad_ns" ) );
            output.p50 = real( value.at( "p50_ns" ) );
            output.p90 = real( value.at( "p90_ns" ) );
            output.p95 = real( value.at( "p95_ns" ) );
            output.p99 = real( value.at( "p99_ns" ) );
            if( value.contains( "unavailable_reason" ) && !value.at( "unavailable_reason" ).is_null() )
                output.unavailableReason = value.at( "unavailable_reason" ).get<std::string>();
        };
        auto metric = [&]( const json& value, NeutralMetricAggregate& output ) {
            distribution( value.at( "per_complete_frame" ), output.perCompleteFrame );
            distribution( value.at( "when_present" ), output.whenPresent );
            const auto pattern = value.at( "pattern" ).get<std::string>();
            if( pattern == "isolated_spike" ) output.pattern = AnomalyPattern::IsolatedSpike;
            else if( pattern == "recurrent_spike" ) output.pattern = AnomalyPattern::RecurrentSpike;
            else if( pattern == "burst_window" ) output.pattern = AnomalyPattern::BurstWindow;
            else if( pattern == "persistent_pressure" ) output.pattern = AnomalyPattern::PersistentPressure;
            else output.pattern = AnomalyPattern::None;
            for( const auto& item : value.at( "anomalies" ) )
                output.anomalies.push_back( { u64( item.at( "frame_index" ) ),
                    i64( item.at( "value_ns" ) ), real( item.at( "delta_from_median_ns" ) ) } );
            output.anomalyCount = value.contains( "anomaly_count" ) ?
                u64( value.at( "anomaly_count" ) ) : output.anomalies.size();
            output.anomaliesComplete = value.value( "anomalies_complete", true );
            output.longestBurstFrames = u64( value.at( "longest_burst_frames" ) );
            if( value.contains( "longest_burst_start_frame" ) &&
                !value.at( "longest_burst_start_frame" ).is_null() )
                output.longestBurstStartFrame = u64( value.at( "longest_burst_start_frame" ) );
            if( value.contains( "longest_burst_end_frame" ) &&
                !value.at( "longest_burst_end_frame" ).is_null() )
                output.longestBurstEndFrame = u64( value.at( "longest_burst_end_frame" ) );
            if( value.contains( "longest_burst_peak_frame" ) &&
                !value.at( "longest_burst_peak_frame" ).is_null() )
                output.longestBurstPeakFrame = u64( value.at( "longest_burst_peak_frame" ) );
            if( !value.at( "period_frames" ).is_null() ) output.periodFrames = u64( value.at( "period_frames" ) );
        };

        const auto& quality = document.at( "quality" );
        result.qualityComplete = quality.at( "complete" ).get<bool>();
        result.unreportedGapCount = u64( quality.at( "unreported_gap_count" ) );
        result.qualityFindings = quality.at( "findings" ).get<std::vector<std::string>>();
        result.maximumBufferedValuesObserved = u64( document.at( "maximum_buffered_values_observed" ) );
        result.contentSha256 = document.value( "content_sha256", std::string() );
        for( const auto& item : document.at( "domains" ) )
        {
            NeutralDomainAuditResult domain;
            domain.domain = item.at( "domain" ).get<std::string>();
            domain.present = item.at( "present" ).get<bool>();
            domain.status = item.at( "status" ).get<std::string>();
            domain.inputCount = u64( item.at( "input_count" ) );
            domain.consumedInputCount = u64( item.at( "consumed_input_count" ) );
            domain.outputCount = u64( item.at( "output_count" ) );
            domain.actualOutputCount = u64( item.at( "actual_output_count" ) );
            domain.inputChecksum = item.at( "input_checksum" ).get<std::string>();
            domain.consumedChecksum = item.at( "consumed_checksum" ).get<std::string>();
            domain.qualityComplete = item.at( "quality_complete" ).get<bool>();
            if( !item.at( "unavailable_reason" ).is_null() )
                domain.unavailableReason = item.at( "unavailable_reason" ).get<std::string>();
            result.domains.push_back( std::move( domain ) );
        }
        for( const auto& item : document.at( "signatures" ) )
        {
            NeutralSignatureAggregate signature;
            signature.domain = item.at( "domain" ).get<std::string>();
            signature.signatureId = item.at( "signature_id" ).get<std::string>();
            signature.frameScope = item.at( "frame_scope" ).get<std::string>();
            signature.logical = item.at( "logical" ).get<bool>();
            signature.exact = item.at( "exact" ).get<bool>();
            signature.completeFrameCount = u64( item.at( "complete_frame_count" ) );
            signature.presentFrameCount = u64( item.at( "present_frame_count" ) );
            signature.occurrenceCount = u64( item.at( "occurrence_count" ) );
            signature.unknownFrameCount = item.contains( "unknown_frame_count" ) ?
                u64( item.at( "unknown_frame_count" ) ) : 0;
            metric( item.at( "inclusive" ), signature.inclusive );
            metric( item.at( "exclusive" ), signature.exclusive );
            metric( item.at( "wait" ), signature.wait );
            metric( item.at( "critical_path" ), signature.criticalPath );
            result.signatures.push_back( std::move( signature ) );
        }
        auto rankingList = [&]( const json& values, std::vector<NeutralRankingEntry>& output ) {
            for( const auto& item : values )
            {
                NeutralRankingEntry entry;
                entry.signatureId = item.at( "signature_id" ).get<std::string>();
                entry.frameScope = item.at( "frame_scope" ).get<std::string>();
                entry.logical = item.at( "logical" ).get<bool>();
                entry.totalNs = i64( item.at( "total_ns" ) );
                entry.waitNs = i64( item.at( "wait_ns" ) );
                entry.criticalPathNs = i64( item.at( "critical_path_ns" ) );
                entry.contribution = real( item.at( "contribution" ) );
                entry.cumulativeContribution = real( item.at( "cumulative_contribution" ) );
                output.push_back( std::move( entry ) );
            }
        };
        for( const auto& item : document.at( "rankings" ) )
        {
            NeutralDomainRanking ranking;
            ranking.domain = item.at( "domain" ).get<std::string>();
            rankingList( item.at( "inclusive" ), ranking.inclusive );
            rankingList( item.at( "exclusive" ), ranking.exclusive );
            rankingList( item.at( "wait_critical" ), ranking.waitCritical );
            result.rankings.push_back( std::move( ranking ) );
        }
        const auto expected = Sha256Text( SerializeResult( result, false ) );
        if( result.contentSha256.empty() || expected != result.contentSha256 )
        {
            error = "neutral_statistics_content_hash_mismatch";
            result = {};
            return false;
        }
        return true;
    }
    catch( const std::exception& exception )
    {
        error = "neutral_statistics_parse_failed:" + std::string( exception.what() );
        result = {};
        return false;
    }
}

bool PublishNeutralStatistics( const std::filesystem::path& storeRoot,
    const NeutralAggregateIdentity& identity, const std::string& generation,
    const NeutralStatisticsResult& result, NeutralAggregateManifest& manifest,
    std::string& error )
{
    error.clear();
    manifest = {};
    if( generation.empty() ) { error = "neutral_statistics_generation_missing"; return false; }
    const auto aggregateIdentity = ComputeNeutralAggregateIdentity( identity );
    if( aggregateIdentity.empty() ) { error = "neutral_statistics_identity_invalid"; return false; }
    if( !result.qualityComplete || result.unreportedGapCount != 0 )
    {
        error = "neutral_statistics_quality_incomplete";
        return false;
    }
    if( Sha256Text( SerializeResult( result, false ) ) != result.contentSha256 )
    {
        error = "neutral_statistics_content_hash_mismatch";
        return false;
    }

    const auto payload = SerializeResult( result, true );
    NeutralAggregateRun run;
    run.runId = 1;
    run.domain = "neutral";
    run.kind = "exact-statistics-v1";
    run.relativePath = std::filesystem::path( "runs" ) /
        Sha256Text( generation ).substr( 0, 16 ) / "neutral-statistics-v1.bin";
    run.recordCount = result.signatures.size();
    if( !WriteNeutralAggregateRun( storeRoot, run, payload.data(), payload.size(), error ) ) return false;
    std::vector<uint8_t> verification;
    if( !ReadNeutralAggregateRun( storeRoot, run, verification, error ) ) return false;
    if( verification.size() != payload.size() ||
        std::memcmp( verification.data(), payload.data(), payload.size() ) != 0 )
    {
        error = "neutral_statistics_run_verification_failed";
        return false;
    }

    manifest.identity = identity;
    manifest.aggregateIdentity = aggregateIdentity;
    manifest.generation = generation;
    manifest.state = NeutralAggregateState::Complete;
    manifest.lastAccessUnixNs = UnixTimeNs();
    manifest.qualityComplete = true;
    manifest.unreportedGapCount = 0;
    manifest.qualityFindings = result.qualityFindings;
    manifest.runs.push_back( run );
    manifest.completed = true;
    for( const auto& domain : result.domains )
    {
        manifest.domains.push_back( { domain.domain, domain.present, domain.status,
            domain.inputCount, domain.actualOutputCount, domain.consumedChecksum,
            domain.unavailableReason } );
    }
    if( !SaveNeutralAggregateManifest( storeRoot, manifest, error ) ) return false;
    return VerifyNeutralAggregate( storeRoot, manifest, error );
}

}

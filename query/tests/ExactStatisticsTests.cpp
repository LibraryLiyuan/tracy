#include "TracyExactStatistics.hpp"
#include "TracyHash.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <iostream>

using namespace tracy::analysis;

namespace
{

std::filesystem::path TemporaryRoot()
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        ( "jn-tracy-exact-statistics-test-" + std::to_string( stamp ) );
}

const NeutralSignatureAggregate& Find( const NeutralStatisticsResult& result, const char* id )
{
    const auto found = std::find_if( result.signatures.begin(), result.signatures.end(),
        [&]( const auto& value ) { return value.signatureId == id; } );
    assert( found != result.signatures.end() );
    return *found;
}

NeutralSignatureFrameInput Input( const char* id, uint64_t frame, int64_t inclusive,
    int64_t exclusive, int64_t wait = 0, int64_t critical = 0 )
{
    NeutralSignatureFrameInput value;
    value.domain = "cpu"; value.signatureId = id; value.frameScope = "Player.Frame";
    value.frameIndex = frame; value.inclusiveNs = inclusive; value.exclusiveNs = exclusive;
    value.waitNs = wait; value.criticalPathNs = critical; value.occurrenceCount = 1;
    return value;
}

NeutralSignatureDenominatorInput Denominator( const char* id, uint64_t count )
{
    return { "cpu", id, "Player.Frame", count };
}

double PercentileOracle( const std::vector<double>& sorted, double percentile )
{
    if( sorted.empty() ) return 0;
    const auto position = percentile * double( sorted.size() - 1 );
    const auto lower = size_t( std::floor( position ) );
    const auto upper = size_t( std::ceil( position ) );
    return sorted[lower] + ( sorted[upper] - sorted[lower] ) * ( position - double( lower ) );
}

ExactDistribution DistributionOracle( const std::vector<int64_t>& explicitValues, uint64_t zeros )
{
    std::vector<double> values( size_t( zeros ), 0 );
    for( const auto value : explicitValues ) values.push_back( double( value ) );
    std::sort( values.begin(), values.end() );
    ExactDistribution result;
    result.count = values.size();
    result.zeroCount = std::count( values.begin(), values.end(), 0.0 );
    if( values.empty() ) return result;
    for( const auto value : values ) result.total += int64_t( value );
    result.min = int64_t( values.front() ); result.max = int64_t( values.back() );
    result.mean = double( result.total ) / double( result.count );
    result.median = result.p50 = PercentileOracle( values, 0.5 );
    result.p90 = PercentileOracle( values, 0.9 );
    result.p95 = PercentileOracle( values, 0.95 );
    result.p99 = PercentileOracle( values, 0.99 );
    std::vector<double> deviations;
    for( const auto value : values ) deviations.push_back( std::abs( value - result.median ) );
    std::sort( deviations.begin(), deviations.end() );
    result.mad = PercentileOracle( deviations, 0.5 );
    return result;
}

void AssertDistribution( const ExactDistribution& actual, const ExactDistribution& expected )
{
    assert( actual.exact && actual.count == expected.count && actual.zeroCount == expected.zeroCount );
    assert( actual.total == expected.total && actual.min == expected.min && actual.max == expected.max );
    assert( std::abs( actual.mean - expected.mean ) < 0.00001 );
    assert( std::abs( actual.median - expected.median ) < 0.00001 );
    assert( std::abs( actual.mad - expected.mad ) < 0.00001 );
    assert( std::abs( actual.p50 - expected.p50 ) < 0.00001 );
    assert( std::abs( actual.p90 - expected.p90 ) < 0.00001 );
    assert( std::abs( actual.p95 - expected.p95 ) < 0.00001 );
    assert( std::abs( actual.p99 - expected.p99 ) < 0.00001 );
}

}

int main( int argc, char** argv )
{
    const auto temporary = TemporaryRoot();
    std::filesystem::create_directories( temporary );

    {
        // Million-signature captures expose retained vector capacity, even
        // though each signature has few rows and no wait/critical anomalies.
        NeutralStatisticsInput sparseSignatures;
        sparseSignatures.temporaryRoot = temporary / "anomaly-retention";
        constexpr uint64_t Count = 4096;
        for( uint64_t index = 0; index < Count; ++index )
        {
            const auto name = "sparse-" + std::to_string( index );
            sparseSignatures.runs.push_back( Input( name.c_str(), 1,
                index % 2 == 0 ? 0 : 10, index % 2 == 0 ? 0 : 10 ) );
            sparseSignatures.denominators.push_back( Denominator( name.c_str(), 4 ) );
        }
        sparseSignatures.domainAudit = { { "cpu", true, "complete", Count, Count, Count,
            std::string( 64, 'd' ), std::string( 64, 'd' ), true, {} } };
        const auto sparseResult = BuildNeutralStatistics( sparseSignatures );
        assert( sparseResult.qualityComplete && sparseResult.signatures.size() == Count );
        uint64_t retained = 0, useful = 0;
        for( const auto& signature : sparseResult.signatures )
            for( const auto* metric : { &signature.inclusive, &signature.exclusive,
                &signature.wait, &signature.criticalPath } )
            {
                retained += metric->anomalies.capacity() * sizeof( AnomalyInstance );
                useful += metric->anomalies.size() * sizeof( AnomalyInstance );
            }
        std::cout << "ANOMALY_RETENTION signatures=" << Count << " retained_bytes=" << retained
            << " useful_bytes=" << useful << " result_sha256=" << sparseResult.contentSha256 << '\n';
        if( retained != useful )
        { std::cerr << "Finished metrics retain empty or unused provisional anomaly slots\n"; return 1; }
        if( argc > 1 && std::string( argv[1] ) == "--anomaly-retention-only" )
        {
            std::filesystem::remove_all( temporary );
            return 0;
        }
    }

    {
        NeutralStatisticsInput two;
        two.temporaryRoot = temporary / "two-spikes";
        for( uint64_t i = 0; i < 20; ++i ) two.runs.push_back( Input( "two", i,
            i == 3 || i == 15 ? 100 : 1, i == 3 || i == 15 ? 100 : 1 ) );
        two.denominators = { Denominator( "two", 20 ) };
        const auto measured = BuildNeutralStatistics( two );
        if( Find( measured, "two" ).exclusive.pattern != AnomalyPattern::RecurrentSpike )
        { std::cerr << "Two isolated anomalies cannot disappear between one and three\n"; return 1; }
    }

    {
        NeutralStatisticsInput scoped;
        scoped.temporaryRoot = temporary / "independent-frame-sets";
        scoped.runs = { Input( "player", 1, 10, 10 ), Input( "render", 1, 100, 100 ) };
        scoped.runs[1].frameScope = "Render.Frame";
        scoped.denominators = { Denominator( "player", 1 ),
            { "cpu", "render", "Render.Frame", 1 } };
        scoped.domainAudit = { { "cpu", true, "complete", 2, 2, 2,
            std::string( 64, 'a' ), std::string( 64, 'a' ), true, {} } };
        const auto scopedResult = BuildNeutralStatistics( scoped );
        if( scopedResult.rankings.size() != 2 || scopedResult.rankings[0].exclusive.size() != 1 ||
            scopedResult.rankings[1].exclusive.size() != 1 )
        { std::cerr << "FrameSets must not compete in one ranking\n"; return 1; }
    }
    {
        NeutralStatisticsInput unknown;
        unknown.temporaryRoot = temporary / "unknown-is-not-zero";
        unknown.runs = { Input( "partial", 1, 10, 10 ), Input( "partial", 2, 0, 0 ) };
        unknown.runs[1].exact = false;
        unknown.denominators = { Denominator( "partial", 3 ) };
        unknown.domainAudit = { { "cpu", true, "invalid", 2, 2, 1,
            std::string( 64, 'a' ), std::string( 64, 'a' ), true, "source_open_zone" } };
        const auto partial = BuildNeutralStatistics( unknown );
        const auto& metric = Find( partial, "partial" ).exclusive;
        if( !partial.qualityComplete || metric.perCompleteFrame.exact ||
            metric.perCompleteFrame.count != 2 || metric.perCompleteFrame.zeroCount != 1 ||
            metric.whenPresent.count != 1 )
        { std::cerr << "Unknown source frame must not count as known zero or scanner failure\n"; return 1; }
    }

    const auto even = ComputeExactDistributionExternal( { 1, 2, 3, 4 }, 0,
        temporary, "even", 2 );
    assert( even.exact && even.count == 4 && even.min == 1 && even.max == 4 );
    assert( even.total == 10 && even.mean == 2.5 && even.median == 2.5 );
    assert( even.p50 == 2.5 && std::abs( even.p90 - 3.7 ) < 0.00001 );
    assert( even.mad == 1.0 );

    const auto odd = ComputeExactDistributionExternal( { 1, 2, 3 }, 0,
        temporary, "odd", 2 );
    assert( odd.median == 2.0 && odd.mad == 1.0 );

    const auto sparse = ComputeExactDistributionExternal( { 10, 20 }, 3,
        temporary, "sparse", 1 );
    assert( sparse.count == 5 && sparse.zeroCount == 3 );
    assert( sparse.median == 0 && sparse.p90 == 16 && sparse.max == 20 );

    const auto allZero = ComputeExactDistributionExternal( {}, 7, temporary, "all-zero", 1 );
    AssertDistribution( allZero, DistributionOracle( {}, 7 ) );
    const auto invalidDuration = ComputeExactDistributionExternal( { 1, -1 }, 0,
        temporary, "negative", 1 );
    assert( !invalidDuration.exact && invalidDuration.unavailableReason == "negative_duration_not_supported" );

    // A signature that fits the configured bounded buffer must not depend on
    // filesystem-backed external sorting.  A regular file is deliberately
    // supplied where a temporary directory would otherwise be required.
    const auto blockedTemporary = temporary / "not-a-directory";
    {
        std::ofstream blocker( blockedTemporary, std::ios::binary | std::ios::trunc );
        blocker << "block";
    }
    const auto boundedInMemory = ComputeExactDistributionExternal(
        { 9, 3, 7, 3 }, 12, blockedTemporary, "bounded-in-memory", 4 );
    AssertDistribution( boundedInMemory, DistributionOracle( { 9, 3, 7, 3 }, 12 ) );

    std::mt19937_64 random( 0x515343414e38ull );
    for( size_t iteration = 0; iteration < 64; ++iteration )
    {
        std::vector<int64_t> values( size_t( random() % 34 ) );
        for( auto& value : values ) value = int64_t( random() % 101 );
        const auto zeros = uint64_t( random() % 11 );
        const auto actual = ComputeExactDistributionExternal( values, zeros, temporary,
            "oracle-" + std::to_string( iteration ), 1 + random() % 7 );
        AssertDistribution( actual, DistributionOracle( values, zeros ) );
    }
    std::vector<int64_t> externalMergeValues( 8192 );
    for( size_t index = 0; index < externalMergeValues.size(); ++index )
        externalMergeValues[index] = int64_t( ( index * 7919 ) % 100003 );
    const auto externalMerge = ComputeExactDistributionExternal( externalMergeValues, 317,
        temporary, "external-merge-corpus", 257 );
    AssertDistribution( externalMerge, DistributionOracle( externalMergeValues, 317 ) );

    NeutralStatisticsInput anomalyInput;
    anomalyInput.temporaryRoot = temporary / "bounded-anomaly-summary";
    anomalyInput.maximumBufferedValues = 4096;
    for( uint64_t frame = 0; frame < 2000; ++frame )
        anomalyInput.runs.push_back( Input( "many-anomalies", frame,
            frame < 1800 ? 1 : 100, frame < 1800 ? 1 : 100 ) );
    anomalyInput.denominators.push_back( Denominator( "many-anomalies", 2000 ) );
    const auto boundedAnomalies = BuildNeutralStatistics( anomalyInput );
    const auto& manyAnomalies = Find( boundedAnomalies, "many-anomalies" ).exclusive;
    assert( manyAnomalies.anomalyCount == 200 );
    assert( !manyAnomalies.anomaliesComplete );
    assert( manyAnomalies.anomalies.size() <= NeutralMaximumRepresentativeAnomalies );
    assert( manyAnomalies.longestBurstFrames == 200 );
    assert( manyAnomalies.longestBurstStartFrame == 1800 );
    assert( manyAnomalies.longestBurstEndFrame == 1999 );

    NeutralStatisticsInput logicalOnlyInput;
    logicalOnlyInput.temporaryRoot = temporary / "logical-only-ranking";
    logicalOnlyInput.runs = { { "cpu", "cpu-logical:site", "Player.Frame", 0,
        10, 7, 0, 0, 1, true, true } };
    logicalOnlyInput.denominators = { { "cpu", "cpu-logical:site", "Player.Frame", 1 } };
    logicalOnlyInput.domainAudit = { { "cpu", true, "complete", 1, 1, 1,
        std::string( 64, 'e' ), std::string( 64, 'e' ), true, {} } };
    const auto logicalOnlyResult = BuildNeutralStatistics( logicalOnlyInput );
    assert( logicalOnlyResult.rankings.size() == 1 );
    assert( logicalOnlyResult.rankings.front().exclusive.size() == 1 );
    assert( logicalOnlyResult.rankings.front().exclusive.front().logical );

    NeutralStatisticsInput input;
    input.temporaryRoot = temporary / "build-a";
    input.maximumBufferedValues = 2;
    input.runs = {
        Input( "sparse", 1, 10, 10 ), Input( "sparse", 3, 20, 20 ),
        Input( "sparse", 3, 5, 5 ),
        Input( "isolated", 0, 1, 1 ), Input( "isolated", 1, 1, 1 ),
        Input( "isolated", 2, 1, 1 ), Input( "isolated", 3, 20, 20 ),
        Input( "recurrent", 0, 1, 1 ), Input( "recurrent", 1, 20, 20 ),
        Input( "recurrent", 2, 1, 1 ), Input( "recurrent", 3, 20, 20 ),
        Input( "recurrent", 4, 1, 1 ), Input( "recurrent", 5, 20, 20 ),
        Input( "recurrent", 6, 1, 1 ), Input( "recurrent", 7, 1, 1 ),
        Input( "recurrent", 8, 1, 1 ), Input( "recurrent", 9, 1, 1 ),
        Input( "burst", 0, 1, 1 ), Input( "burst", 1, 1, 1 ),
        Input( "burst", 2, 1, 1 ), Input( "burst", 3, 20, 20 ),
        Input( "burst", 4, 20, 20 ), Input( "burst", 5, 20, 20 ),
        Input( "burst", 6, 1, 1 ), Input( "burst", 7, 1, 1 ),
        Input( "burst", 8, 1, 1 ), Input( "burst", 9, 1, 1 ),
        Input( "persistent", 0, 5, 5, 2 ), Input( "persistent", 1, 5, 5, 2 ),
        Input( "persistent", 2, 5, 5, 2 ), Input( "persistent", 3, 5, 5, 2 ),
        Input( "inclusive-parent", 0, 100, 0 )
    };
    input.denominators = {
        Denominator( "sparse", 5 ), Denominator( "isolated", 4 ),
        Denominator( "recurrent", 10 ), Denominator( "burst", 10 ),
        Denominator( "persistent", 4 ), Denominator( "inclusive-parent", 4 )
    };
    input.domainAudit = {
        { "cpu", true, "complete", input.runs.size(), input.runs.size(), 6,
            std::string( 64, 'a' ), std::string( 64, 'a' ), true, {} },
        { "gpu.catalog", false, "absent", 0, 0, 0,
            std::string( 64, '0' ), std::string( 64, '0' ), true, "catalog disabled for fixture" }
    };

    const auto result = BuildNeutralStatistics( input );
    assert( result.qualityComplete );
    assert( result.unreportedGapCount == 0 );
    assert( result.contentSha256.size() == 64 );
    assert( result.maximumBufferedValuesObserved <= 2 );

    const auto& sparseSignature = Find( result, "sparse" );
    assert( sparseSignature.exclusive.perCompleteFrame.count == 5 );
    assert( sparseSignature.exclusive.perCompleteFrame.zeroCount == 3 );
    assert( sparseSignature.exclusive.perCompleteFrame.median == 0 );
    assert( sparseSignature.exclusive.whenPresent.count == 2 );
    assert( sparseSignature.exclusive.whenPresent.median == 17.5 );
    assert( sparseSignature.occurrenceCount == 3 );

    const auto& isolated = Find( result, "isolated" );
    assert( isolated.exclusive.pattern == AnomalyPattern::IsolatedSpike );
    assert( isolated.exclusive.anomalies.size() == 1 );
    assert( isolated.exclusive.anomalies[0].frameIndex == 3 );
    assert( isolated.exclusive.anomalies[0].deltaFromMedianNs == 19 );

    const auto& recurrent = Find( result, "recurrent" );
    assert( recurrent.exclusive.pattern == AnomalyPattern::RecurrentSpike );
    assert( recurrent.exclusive.anomalies.size() == 3 );
    assert( recurrent.exclusive.periodFrames && *recurrent.exclusive.periodFrames == 2 );

    const auto& burst = Find( result, "burst" );
    assert( burst.exclusive.pattern == AnomalyPattern::BurstWindow );
    assert( burst.exclusive.longestBurstFrames == 3 );

    const auto& persistent = Find( result, "persistent" );
    assert( persistent.exclusive.pattern == AnomalyPattern::PersistentPressure );
    assert( persistent.exclusive.perCompleteFrame.mad == 0 );

    const auto cpuRanking = std::find_if( result.rankings.begin(), result.rankings.end(),
        []( const auto& value ) { return value.domain == "cpu"; } );
    assert( cpuRanking != result.rankings.end() );
    assert( !cpuRanking->inclusive.empty() && cpuRanking->inclusive.front().signatureId == "inclusive-parent" );
    assert( !cpuRanking->exclusive.empty() && cpuRanking->exclusive.front().signatureId == "burst" );
    assert( !cpuRanking->waitCritical.empty() && cpuRanking->waitCritical.front().signatureId == "persistent" );
    assert( std::abs( cpuRanking->exclusive.back().cumulativeContribution - 1.0 ) < 0.00001 );

    auto shuffled = input;
    shuffled.temporaryRoot = temporary / "build-b";
    std::reverse( shuffled.runs.begin(), shuffled.runs.end() );
    std::reverse( shuffled.denominators.begin(), shuffled.denominators.end() );
    const auto shuffledResult = BuildNeutralStatistics( shuffled );
    assert( shuffledResult.contentSha256 == result.contentSha256 );

    // The production path accepts arbitrarily many signature-frame inputs
    // without retaining them all in memory. The exact result must match the
    // legacy vector entry point while the raw-run buffer remains bounded.
    NeutralStatisticsStreamBuilder streamBuilder( temporary / "streaming", 127, 31 );
    NeutralStatisticsInput streamingOracle;
    streamingOracle.temporaryRoot = temporary / "streaming-oracle";
    streamingOracle.maximumBufferedValues = 127;
    constexpr uint64_t StreamingFrames = 2048;
    constexpr uint64_t StreamingSignatures = 96;
    for( uint64_t signature = 0; signature < StreamingSignatures; ++signature )
    {
        const auto name = "stream-" + std::to_string( signature );
        const auto denominator = Denominator( name.c_str(), StreamingFrames );
        const auto token = streamBuilder.RegisterSignature( "cpu", name, "Player.Frame" );
        assert( token != NeutralStatisticsStreamBuilder::InvalidSignatureToken );
        streamBuilder.AddDenominator( denominator );
        streamingOracle.denominators.push_back( denominator );
        for( uint64_t frame = 0; frame < StreamingFrames; ++frame )
        {
            const auto value = int64_t( ( signature * 17 + frame * 13 ) % 10000 );
            auto item = Input( name.c_str(), frame, value + 3, value );
            assert( streamBuilder.AddRegisteredRun( token, frame,
                item.inclusiveNs, item.exclusiveNs, item.waitNs,
                item.criticalPathNs, item.occurrenceCount,
                item.exact, item.logical ) );
            streamingOracle.runs.push_back( std::move( item ) );
        }
    }
    const auto streamingCount = StreamingFrames * StreamingSignatures;
    const auto streamingAudit = NeutralDomainAuditInput { "cpu", true, "complete",
        streamingCount, streamingCount, StreamingSignatures,
        std::string( 64, 'c' ), std::string( 64, 'c' ), true, {} };
    streamBuilder.AddDomainAudit( streamingAudit );
    streamingOracle.domainAudit.push_back( streamingAudit );
    NeutralStatisticsResult streamedResult;
    std::string streamingError;
    assert( streamBuilder.Finish( streamedResult, streamingError ) );
    const auto oracleResult = BuildNeutralStatistics( streamingOracle );
    assert( streamingError.empty() );
    assert( streamedResult.contentSha256 == oracleResult.contentSha256 );
    assert( streamBuilder.MaximumBufferedRunsObserved() <= 31 );
    assert( streamBuilder.InputRunCount() == streamingCount );

    NeutralAggregateIdentity identity;
    identity.traceStrongId = std::string( 64, '1' );
    identity.queryExecutableSha256 = std::string( 64, '2' );
    identity.querySchema = "1.35.0";
    NeutralAggregateManifest manifest;
    std::string error;
    const auto store = temporary / "store";
    assert( PublishNeutralStatistics( store, identity, "qscan-8", result, manifest, error ) );
    assert( manifest.state == NeutralAggregateState::Complete && manifest.completed );
    assert( manifest.runs.size() == 1 && manifest.runs[0].recordCount == result.signatures.size() );
    assert( VerifyNeutralAggregate( store, manifest, error ) );
    const auto loaded = LoadNeutralAggregateManifest( store, error );
    assert( loaded && loaded->qualityComplete && loaded->unreportedGapCount == 0 );

    // A source domain may be explicitly invalid while the scanner's own
    // transport/count/checksum audit is complete. This is reported evidence,
    // not an unreported scanner gap: publish the neutral aggregate so the
    // candidate policy can produce a P0 quality finding for the domain.
    auto sourceDegraded = input;
    sourceDegraded.temporaryRoot = temporary / "source-degraded";
    sourceDegraded.domainAudit[0].status = "invalid";
    sourceDegraded.domainAudit[0].qualityComplete = true;
    sourceDegraded.domainAudit[0].unavailableReason = "source_cpu_boundaries_incomplete";
    const auto sourceDegradedResult = BuildNeutralStatistics( sourceDegraded );
    assert( sourceDegradedResult.qualityComplete );
    assert( sourceDegradedResult.unreportedGapCount == 0 );
    assert( sourceDegradedResult.domains[0].status == "invalid" );
    assert( sourceDegradedResult.domains[0].qualityComplete );
    NeutralAggregateManifest sourceDegradedManifest;
    assert( PublishNeutralStatistics( temporary / "source-degraded-store", identity,
        "qscan-8-source-degraded", sourceDegradedResult, sourceDegradedManifest, error ) );

    auto invalid = input;
    invalid.temporaryRoot = temporary / "invalid";
    invalid.domainAudit[0].consumedInputCount--;
    const auto invalidResult = BuildNeutralStatistics( invalid );
    assert( !invalidResult.qualityComplete );
    assert( invalidResult.unreportedGapCount == 1 );
    NeutralAggregateManifest rejected;
    assert( !PublishNeutralStatistics( store, identity, "qscan-8-invalid", invalidResult,
        rejected, error ) );
    const auto preserved = LoadNeutralAggregateManifest( store, error );
    assert( preserved && preserved->generation == "qscan-8" );

    std::error_code ignored;
    std::filesystem::remove_all( temporary, ignored );
    return 0;
}

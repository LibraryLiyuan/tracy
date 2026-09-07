#include "TracyNeutralAggregateStore.hpp"
#include "TracyAnalysisIoPath.hpp"
#include "TracyHash.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>
#include <vector>

using namespace tracy::analysis;

namespace
{

std::filesystem::path TemporaryRoot()
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() / ( "jn-tracy-neutral-store-test-" + std::to_string( stamp ) );
}

}

int main()
{
    static_assert( NeutralAggregateStoreSchemaVersion == 1 );
    static_assert( NeutralAggregateSchemaVersion == 1 );
    static_assert( NeutralScanAlgorithmVersion == 2 );

    const auto temporary = TemporaryRoot();
    const auto cacheRoot = temporary / "cache";
    std::filesystem::create_directories( cacheRoot );

    NeutralAggregateIdentity identity;
    identity.traceStrongId = std::string( 64, 'a' );
    identity.queryExecutableSha256 = std::string( 64, 'b' );
    identity.querySchema = "1.35.0";
    identity.scanAlgorithm = NeutralScanAlgorithmId;
    identity.aggregateSchema = NeutralAggregateSchemaVersion;
    const auto aggregateIdentity = ComputeNeutralAggregateIdentity( identity );
    assert( aggregateIdentity.size() == 64 );

    auto changedAlgorithm = identity;
    changedAlgorithm.scanAlgorithm = "native-scan-v1";
    assert( ComputeNeutralAggregateIdentity( changedAlgorithm ).empty() );
    assert( ComputeNeutralAggregateIdentity( changedAlgorithm ) != aggregateIdentity );
    assert( NeutralAggregateCachePath( cacheRoot, identity ) == cacheRoot / identity.traceStrongId / NeutralScanAlgorithmId );

    const auto storeRoot = NeutralAggregateCachePath( cacheRoot, identity );
    std::string error;
    NeutralAggregateWriterLease firstLease;
    NeutralAggregateWriterLease secondLease;
    assert( AcquireNeutralAggregateWriterLease( storeRoot, firstLease, error ) );
    assert( !AcquireNeutralAggregateWriterLease( storeRoot, secondLease, error ) );
    assert( firstLease.Heartbeat( error ) );
    firstLease.Release();
    assert( AcquireNeutralAggregateWriterLease( storeRoot, secondLease, error ) );
    secondLease.Release();

    NeutralAggregateManifest manifest;
    manifest.identity = identity;
    manifest.aggregateIdentity = aggregateIdentity;
    manifest.generation = "g-test";
    manifest.state = NeutralAggregateState::Building;
    manifest.lastAccessUnixNs = 10;
    assert( SaveNeutralAggregateManifest( storeRoot, manifest, error ) );
    assert( std::filesystem::exists( storeRoot / "aggregate-manifest.json" ) );
    {
        std::ifstream input( storeRoot / "aggregate-manifest.json", std::ios::binary );
        const auto document = nlohmann::json::parse( input );
        assert( document.at( "schema_version" ) == 1 );
        assert( document.at( "trace_identity" ) == identity.traceStrongId );
        assert( document.at( "state" ) == "building" );
        assert( document.at( "domains" ).empty() );
        assert( document.at( "quality" ).at( "complete" ) == false );
    }
    assert( !CanReuseNeutralAggregate( manifest, changedAlgorithm ) );
    assert( !CanReuseNeutralAggregate( manifest, identity ) );

    const auto uncommitted = storeRoot / "domains" / "frame" / "run-uncommitted.bin.tmp";
    std::filesystem::create_directories( uncommitted.parent_path() );
    {
        std::ofstream output( uncommitted, std::ios::binary | std::ios::trunc );
        output << "not committed";
    }
    auto loaded = LoadNeutralAggregateManifest( storeRoot, error );
    assert( loaded && loaded->runs.empty() );

    constexpr std::array<uint64_t, 5> payload { 10, 20, 30, 40, 50 };
    NeutralAggregateRun run;
    run.runId = 1;
    run.domain = "frame";
    run.kind = "frame-series";
    run.relativePath = std::filesystem::path( "domains" ) / "frame" / "run-000001.bin";
    run.recordCount = payload.size();
    assert( WriteNeutralAggregateRun( storeRoot, run, payload.data(), sizeof( payload ), error ) );
    std::vector<uint8_t> readPayload;
    assert( ReadNeutralAggregateRun( storeRoot, run, readPayload, error ) );
    assert( readPayload.size() == sizeof( payload ) );
    assert( std::memcmp( readPayload.data(), payload.data(), sizeof( payload ) ) == 0 );

    const std::array<uint8_t, 6> dictionaryPayload { 'M', 'a', 'i', 'n', 0, 1 };
    NeutralAggregateRun dictionaryRun;
    dictionaryRun.runId = 2;
    dictionaryRun.domain = "dictionary";
    dictionaryRun.kind = "strings";
    dictionaryRun.relativePath = std::filesystem::path( "dictionaries" ) / "strings-000001.bin";
    dictionaryRun.recordCount = 1;
    assert( WriteNeutralAggregateRun( storeRoot, dictionaryRun,
        dictionaryPayload.data(), dictionaryPayload.size(), error ) );
    assert( ReadNeutralAggregateRun( storeRoot, dictionaryRun, readPayload, error ) );
    assert( readPayload == std::vector<uint8_t>( dictionaryPayload.begin(), dictionaryPayload.end() ) );

    // Windows weakly_canonical may remove the extended-length prefix for an
    // existing root while retaining it for a not-yet-created child.  The two
    // spellings still identify the same contained path and must not be rejected.
    for( size_t padding = 40; padding <= 180; padding += 10 )
    {
        const auto deepStoreRoot = temporary / std::string( padding, 'x' );
        std::error_code deepError;
        std::filesystem::create_directories( AnalysisIoPath( deepStoreRoot ), deepError );
        assert( !deepError );
        auto deepRun = run;
        deepRun.runId = 4 + padding;
        deepRun.relativePath = std::filesystem::path( "runs" ) /
            "0123456789abcdef" / "neutral-statistics-v1.bin";
        assert( WriteNeutralAggregateRun( deepStoreRoot, deepRun,
            payload.data(), sizeof( payload ), error ) );
        assert( ReadNeutralAggregateRun( deepStoreRoot, deepRun, readPayload, error ) );
        assert( readPayload.size() == sizeof( payload ) );
    }
    const auto explicitIoStoreRoot = AnalysisIoPath(
        temporary / "explicit-extended-root" / std::string( 80, 'z' ) );
    std::error_code explicitIoError;
    std::filesystem::create_directories( explicitIoStoreRoot, explicitIoError );
    assert( !explicitIoError );
    auto explicitIoRun = run;
    explicitIoRun.runId = 500;
    explicitIoRun.relativePath = std::filesystem::path( "runs" ) /
        "0123456789abcdef" / "neutral-statistics-v1.bin";
    assert( WriteNeutralAggregateRun( explicitIoStoreRoot, explicitIoRun,
        payload.data(), sizeof( payload ), error ) );

    auto escapedRun = run;
    escapedRun.runId = 3;
    escapedRun.relativePath = std::filesystem::path( ".." ) / "escaped.bin";
    assert( !WriteNeutralAggregateRun( storeRoot, escapedRun, payload.data(), sizeof( payload ), error ) );
    assert( error == "neutral_run_invalid_argument" );

    NeutralAggregateCheckpoint checkpoint;
    checkpoint.generation = manifest.generation;
    checkpoint.stage = "CommonScan";
    checkpoint.sourceCursor = "frame:5";
    checkpoint.sourceEvents = 5;
    checkpoint.outputRuns = 2;
    checkpoint.memoryBytes = 4096;
    checkpoint.diskBytes = run.fileBytes;
    checkpoint.progressNumerator = 5;
    checkpoint.progressDenominator = 10;
    checkpoint.resumable = true;
    assert( SaveNeutralAggregateCheckpoint( storeRoot, checkpoint, error ) );
    const auto loadedCheckpoint = LoadNeutralAggregateCheckpoint( storeRoot, error );
    assert( loadedCheckpoint );
    assert( loadedCheckpoint->sourceCursor == checkpoint.sourceCursor );
    assert( loadedCheckpoint->sourceEvents == checkpoint.sourceEvents );

    manifest.runs.push_back( run );
    manifest.runs.push_back( dictionaryRun );
    manifest.state = NeutralAggregateState::CancelledResumable;
    manifest.reason = "test_cancel";
    assert( SaveNeutralAggregateManifest( storeRoot, manifest, error ) );
    loaded = LoadNeutralAggregateManifest( storeRoot, error );
    assert( loaded && loaded->state == NeutralAggregateState::CancelledResumable );
    assert( !CanReuseNeutralAggregate( *loaded, identity ) );

    manifest.state = NeutralAggregateState::Complete;
    manifest.completed = true;
    manifest.qualityComplete = true;
    manifest.reason.clear();
    assert( SaveNeutralAggregateManifest( storeRoot, manifest, error ) );
    loaded = LoadNeutralAggregateManifest( storeRoot, error );
    assert( loaded && loaded->runs.size() == 2 );
    assert( VerifyNeutralAggregate( storeRoot, *loaded, error ) );
    assert( CanReuseNeutralAggregate( *loaded, identity ) );

    const auto protectedRoot = NeutralAggregateCachePath( cacheRoot, NeutralAggregateIdentity {
        std::string( 64, 'c' ), std::string( 64, 'b' ), "1.35.0", NeutralScanAlgorithmId, 1 } );
    NeutralAggregateManifest protectedManifest = manifest;
    protectedManifest.identity.traceStrongId = std::string( 64, 'c' );
    protectedManifest.aggregateIdentity = ComputeNeutralAggregateIdentity( protectedManifest.identity );
    protectedManifest.generation = "g-protected";
    protectedManifest.lastAccessUnixNs = 1;
    protectedManifest.pinned = true;
    protectedManifest.runs.clear();
    assert( SaveNeutralAggregateManifest( protectedRoot, protectedManifest, error ) );

    const auto referencedRoot = NeutralAggregateCachePath( cacheRoot, NeutralAggregateIdentity {
        std::string( 64, 'e' ), std::string( 64, 'b' ), "1.35.0", NeutralScanAlgorithmId, 1 } );
    NeutralAggregateManifest referencedManifest = protectedManifest;
    referencedManifest.identity.traceStrongId = std::string( 64, 'e' );
    referencedManifest.aggregateIdentity = ComputeNeutralAggregateIdentity( referencedManifest.identity );
    referencedManifest.generation = "g-referenced";
    referencedManifest.pinned = false;
    referencedManifest.reportReferenceCount = 1;
    assert( SaveNeutralAggregateManifest( referencedRoot, referencedManifest, error ) );

    const auto evictableRoot = NeutralAggregateCachePath( cacheRoot, NeutralAggregateIdentity {
        std::string( 64, 'd' ), std::string( 64, 'b' ), "1.35.0", NeutralScanAlgorithmId, 1 } );
    NeutralAggregateManifest evictableManifest = protectedManifest;
    evictableManifest.identity.traceStrongId = std::string( 64, 'd' );
    evictableManifest.aggregateIdentity = ComputeNeutralAggregateIdentity( evictableManifest.identity );
    evictableManifest.generation = "g-evictable";
    evictableManifest.pinned = false;
    evictableManifest.lastAccessUnixNs = 2;
    assert( SaveNeutralAggregateManifest( evictableRoot, evictableManifest, error ) );

    NeutralAggregatePruneResult prune;
    assert( PruneNeutralAggregateCache( cacheRoot, 1, { storeRoot }, prune, error ) );
    assert( std::filesystem::exists( storeRoot ) );
    assert( std::filesystem::exists( protectedRoot ) );
    assert( std::filesystem::exists( referencedRoot ) );
    assert( !std::filesystem::exists( evictableRoot ) );
    assert( prune.removedEntries == 1 );
    assert( prune.limitBlockedByProtectedEntries );

    const auto runPath = storeRoot / run.relativePath;
    {
        std::fstream corruption( runPath, std::ios::binary | std::ios::in | std::ios::out );
        assert( corruption );
        char value = 0;
        corruption.read( &value, 1 );
        value ^= 0x7f;
        corruption.seekp( 0 );
        corruption.write( &value, 1 );
    }
    assert( !VerifyNeutralAggregate( storeRoot, *loaded, error ) );
    assert( error == "neutral_run_sha256_mismatch" );
    auto headerCorruption = run;
    headerCorruption.sha256 = Sha256File( runPath );
    assert( !ReadNeutralAggregateRun( storeRoot, headerCorruption, readPayload, error ) );
    assert( error == "neutral_run_header_mismatch" );

    std::error_code ignored;
    std::filesystem::remove_all( temporary, ignored );
    return 0;
}

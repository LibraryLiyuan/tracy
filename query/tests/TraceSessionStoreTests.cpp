#include "TracyHash.hpp"
#include "TracyTraceSessionStore.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace tracy::analysis;

namespace
{

std::filesystem::path TemporaryRoot()
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() / ( "jn-tracy-session-store-test-" + std::to_string( stamp ) );
}

void WriteBytes( const std::filesystem::path& path, const void* data, size_t size )
{
    std::ofstream output( path, std::ios::binary | std::ios::trunc );
    assert( output );
    output.write( static_cast<const char*>( data ), std::streamsize( size ) );
    assert( output );
}

}

int main()
{
    static_assert( TraceSessionStoreSchemaVersion == 1 );
    static_assert( TraceSessionCanonicalSchemaVersion == 1 );
    static_assert( TraceSessionDerivedSchemaVersion == 1 );

    const auto root = TemporaryRoot();
    std::filesystem::create_directories( root );
    const auto source = root / "capture.tracy-stream";
    constexpr std::array<uint8_t, 8> sourceBytes { 1, 2, 3, 4, 5, 6, 7, 8 };
    WriteBytes( source, sourceBytes.data(), sourceBytes.size() );

    const auto finalPath = DefaultTraceSessionPath( source );
    assert( finalPath == root / "capture.jn-trace-session" );
    const std::string generation = "g-test";
    const auto buildingPath = BuildingTraceSessionPath( finalPath, generation );
    assert( buildingPath.filename() == "capture.jn-trace-session.building.g-test" );

    TraceSessionManifest manifest;
    manifest.sessionId = "session-test";
    manifest.generation = generation;
    manifest.state = TraceSessionState::CanonicalBuilding;
    manifest.source.sha256 = Sha256File( source );
    manifest.source.fileSize = sourceBytes.size();
    manifest.source.committedRevision = 42;
    manifest.source.protocol = 90;
    manifest.source.captureIdentity = "capture-test";
    manifest.source.captureEndState = "complete";
    manifest.source.converterSha256 = std::string( 64, 'a' );
    manifest.source.configurationHash = std::string( 64, 'b' );

    std::string error;
    assert( SaveTraceSessionManifest( buildingPath, manifest, error ) );
    assert( !IsTraceSessionQueryable( buildingPath, error ) );
    assert( error == "session_not_published" );

    constexpr std::array<uint64_t, 4> payload { 10, 20, 30, 40 };
    TraceSessionShard shard;
    shard.shardId = 7;
    shard.domain = "frame";
    shard.timeBeginNs = 100;
    shard.timeEndNs = 200;
    shard.sourceRecordBegin = 11;
    shard.sourceRecordEnd = 15;
    shard.recordCount = payload.size();
    assert( WriteTraceSessionShard( buildingPath, generation, shard, payload.data(), sizeof( payload ), error ) );
    assert( shard.relativePath == std::filesystem::path( "generations" ) / generation / "canonical" / "frame-000007.bin" );
    assert( shard.uncompressedBytes == sizeof( payload ) );
    assert( shard.sha256.size() == 64 );
    manifest.shards.push_back( shard );

    manifest.state = TraceSessionState::Complete;
    manifest.mandatoryDerivedComplete = true;
    manifest.auditComplete = true;
    assert( PublishTraceSession( buildingPath, finalPath, manifest, error ) );
    assert( !std::filesystem::exists( buildingPath ) );
    assert( IsTraceSessionQueryable( finalPath, error ) );

    const auto loaded = LoadTraceSessionManifest( finalPath, error );
    assert( loaded );
    assert( loaded->sessionId == manifest.sessionId );
    assert( loaded->generation == generation );
    assert( loaded->source.sha256 == manifest.source.sha256 );
    assert( loaded->source.committedRevision == 42 );
    assert( loaded->shards.size() == 1 );
    assert( loaded->shards.front().sha256 == shard.sha256 );
    assert( VerifyTraceSession( finalPath, *loaded, error ) );

    const auto firstGenerationShardPath = finalPath / loaded->shards.front().relativePath;
    assert( std::filesystem::exists( firstGenerationShardPath ) );

    const std::string secondGeneration = "g-test-2";
    const auto secondBuildingPath = BuildingTraceSessionPath( finalPath, secondGeneration );
    auto secondManifest = *loaded;
    secondManifest.generation = secondGeneration;
    secondManifest.state = TraceSessionState::CanonicalBuilding;
    secondManifest.mandatoryDerivedComplete = false;
    secondManifest.auditComplete = false;
    secondManifest.shards.clear();
    TraceSessionShard secondShard = shard;
    secondShard.shardId = 8;
    constexpr std::array<uint64_t, 2> secondPayload { 50, 60 };
    assert( WriteTraceSessionShard( secondBuildingPath, secondGeneration, secondShard,
        secondPayload.data(), sizeof( secondPayload ), error ) );
    secondManifest.shards.push_back( secondShard );
    secondManifest.state = TraceSessionState::Complete;
    secondManifest.mandatoryDerivedComplete = true;
    secondManifest.auditComplete = true;
    assert( PublishTraceSession( secondBuildingPath, finalPath, secondManifest, error ) );
    assert( std::filesystem::exists( firstGenerationShardPath ) );
    const auto secondLoaded = LoadTraceSessionManifest( finalPath, error );
    assert( secondLoaded );
    assert( secondLoaded->generation == secondGeneration );
    assert( secondLoaded->shards.size() == 1 );
    assert( secondLoaded->shards.front().shardId == 8 );

    assert( VerifyTraceSessionSourceIdentity( source, secondLoaded->source, error ) );
    constexpr std::array<uint8_t, 8> replacedSourceBytes { 8, 7, 6, 5, 4, 3, 2, 1 };
    WriteBytes( source, replacedSourceBytes.data(), replacedSourceBytes.size() );
    assert( !VerifyTraceSessionSourceIdentity( source, secondLoaded->source, error ) );
    assert( error == "source_sha256_mismatch" );
    assert( IsTraceSessionQueryable( finalPath, error ) );

    const auto shardPath = finalPath / secondLoaded->shards.front().relativePath;
    {
        std::fstream corruption( shardPath, std::ios::binary | std::ios::in | std::ios::out );
        assert( corruption );
        char value = 0;
        corruption.read( &value, 1 );
        assert( corruption );
        value ^= 0x5a;
        corruption.seekp( 0 );
        corruption.write( &value, 1 );
        assert( corruption );
    }
    assert( !VerifyTraceSession( finalPath, *secondLoaded, error ) );
    assert( error == "session_shard_sha256_mismatch" );

    std::error_code ignored;
    std::filesystem::remove_all( root, ignored );
    return 0;
}

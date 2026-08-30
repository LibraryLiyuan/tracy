#include "TracyTraceSessionCanonical.hpp"

#include "TracyQueue.hpp"
#include "TracyStreamJournal.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <vector>

namespace tracy::analysis
{
namespace
{

void Put32( std::vector<uint8_t>& output, uint32_t value )
{
    for( int i = 0; i < 4; i++ ) output.push_back( uint8_t( value >> ( i * 8 ) ) );
}

void Put64( std::vector<uint8_t>& output, uint64_t value )
{
    for( int i = 0; i < 8; i++ ) output.push_back( uint8_t( value >> ( i * 8 ) ) );
}

bool Get32( const std::vector<uint8_t>& input, size_t& offset, uint32_t& value )
{
    if( offset > input.size() || input.size() - offset < 4 ) return false;
    value = 0;
    for( int i = 0; i < 4; i++ ) value |= uint32_t( input[offset + i] ) << ( i * 8 );
    offset += 4;
    return true;
}

bool Get64( const std::vector<uint8_t>& input, size_t& offset, uint64_t& value )
{
    if( offset > input.size() || input.size() - offset < 8 ) return false;
    value = 0;
    for( int i = 0; i < 8; i++ ) value |= uint64_t( input[offset + i] ) << ( i * 8 );
    offset += 8;
    return true;
}

struct CanonicalBuildState
{
    const std::filesystem::path* sessionRoot = nullptr;
    const std::string* generation = nullptr;
    const TraceSessionCanonicalOptions* options = nullptr;
    TraceSessionManifest* manifest = nullptr;
    std::ifstream input;
    std::vector<uint8_t> payload;
    std::vector<uint8_t> shardPayload;
    TraceSessionProtocolDecoder decoder;
    TraceSessionProtocolInventory decodedInventory;
    tracy::stream::RecordInfo currentRecord;
    uint64_t protocolFrameOrdinal = 0;
    uint64_t protocolEventCount = 0;
    uint64_t transportRecordCount = 0;
    uint64_t shardRecordCount = 0;
    uint64_t shardSourceBegin = 0;
    uint64_t shardSourceEnd = 0;
    uint64_t shardTimeBegin = 0;
    uint64_t shardTimeEnd = 0;
    uint64_t nextShardId = 0;
    uint64_t resumeAfterSequence = 0;
    uint64_t resumeOffset = tracy::stream::FileHeaderSize;
    std::string previousCheckpointHash;
    bool cancelRequested = false;
    bool firstResumedRecord = true;
    bool failed = false;
    std::string error;
};

bool ReadPayload( CanonicalBuildState& state, const tracy::stream::RecordInfo& record )
{
    constexpr auto HeaderBytes = uint64_t( tracy::stream::RecordHeaderSize );
    const auto maxOffset = uint64_t( std::numeric_limits<std::streamoff>::max() );
    if( record.payloadSize > std::numeric_limits<size_t>::max() ||
        record.offset > maxOffset || HeaderBytes > maxOffset - record.offset ||
        record.payloadSize > uint64_t( std::numeric_limits<std::streamsize>::max() ) )
    {
        state.error = "canonical_record_exceeds_platform_limits";
        return false;
    }
    state.payload.resize( size_t( record.payloadSize ) );
    state.input.clear();
    state.input.seekg( std::streamoff( record.offset + HeaderBytes ), std::ios::beg );
    if( !state.input ) { state.error = "canonical_source_seek_failed"; return false; }
    if( state.payload.empty() ) return true;
    state.input.read( reinterpret_cast<char*>( state.payload.data() ),
        std::streamsize( state.payload.size() ) );
    if( !state.input || state.input.gcount() != std::streamsize( state.payload.size() ) )
    {
        state.error = "canonical_source_read_failed";
        return false;
    }
    return true;
}

bool AppendCanonicalRecord( CanonicalBuildState& state, uint8_t kind, uint8_t type,
    uint8_t domain, uint32_t flags, const uint8_t* payload, uint32_t payloadBytes )
{
    if( payloadBytes != 0 && !payload ) { state.error = "canonical_payload_missing"; return false; }
    if( state.shardRecordCount == 0 )
    {
        state.shardSourceBegin = state.currentRecord.sequence;
        state.shardTimeBegin = state.currentRecord.monotonicNs;
    }
    state.shardSourceEnd = state.currentRecord.sequence;
    state.shardTimeEnd = state.currentRecord.monotonicNs;
    state.shardPayload.push_back( kind );
    state.shardPayload.push_back( type );
    state.shardPayload.push_back( domain );
    state.shardPayload.push_back( 0 );
    Put32( state.shardPayload, flags );
    Put32( state.shardPayload, payloadBytes );
    Put32( state.shardPayload, 0 );
    Put64( state.shardPayload, state.currentRecord.sequence );
    Put64( state.shardPayload, state.currentRecord.monotonicNs );
    Put64( state.shardPayload, state.protocolFrameOrdinal );
    if( payloadBytes != 0 )
        state.shardPayload.insert( state.shardPayload.end(), payload, payload + payloadBytes );
    state.shardRecordCount++;
    return true;
}

bool VisitCanonicalProtocolEvent( const TraceSessionProtocolEventInfo& event,
    void* userData, std::string& error )
{
    auto& state = *static_cast<CanonicalBuildState*>( userData );
    if( !AppendCanonicalRecord( state, 1, event.queueType,
        uint8_t( ClassifyTraceProtocolEvent( event.queueType ) ), 0,
        event.encodedData, event.encodedBytes ) )
    {
        error = state.error;
        return false;
    }
    state.protocolEventCount++;
    return true;
}

bool FlushShard( CanonicalBuildState& state )
{
    if( state.shardRecordCount == 0 ) return true;
    TraceSessionShard shard;
    shard.shardId = state.nextShardId++;
    shard.domain = "protocol";
    shard.timeBeginNs = int64_t( state.shardTimeBegin );
    shard.timeEndNs = int64_t( state.shardTimeEnd );
    shard.sourceRecordBegin = state.shardSourceBegin;
    shard.sourceRecordEnd = state.shardSourceEnd;
    shard.recordCount = state.shardRecordCount;
    if( !WriteTraceSessionShard( *state.sessionRoot, *state.generation, shard,
        state.shardPayload.data(), state.shardPayload.size(), state.error ) ) return false;
    state.manifest->shards.emplace_back( shard );

    std::vector<uint8_t> checkpoint;
    static constexpr uint8_t Magic[8] = { 'J', 'N', 'C', 'H', 'K', 'P', 'T', '1' };
    checkpoint.insert( checkpoint.end(), std::begin( Magic ), std::end( Magic ) );
    Put32( checkpoint, 1 );
    Put32( checkpoint, 0 );
    Put64( checkpoint, state.currentRecord.sequence );
    Put64( checkpoint, state.currentRecord.offset + tracy::stream::RecordHeaderSize +
        state.currentRecord.payloadSize + tracy::stream::RecordTrailerSize );
    Put64( checkpoint, state.protocolFrameOrdinal );
    Put64( checkpoint, state.protocolEventCount );
    Put64( checkpoint, state.transportRecordCount );
    const auto dictionary = state.decoder.ExportDictionary();
    Put32( checkpoint, uint32_t( dictionary.size() ) );
    Put32( checkpoint, uint32_t( state.previousCheckpointHash.size() ) );
    checkpoint.insert( checkpoint.end(), dictionary.begin(), dictionary.end() );
    checkpoint.insert( checkpoint.end(), state.previousCheckpointHash.begin(), state.previousCheckpointHash.end() );

    TraceSessionShard checkpointShard;
    checkpointShard.shardId = state.nextShardId++;
    checkpointShard.domain = "checkpoint";
    checkpointShard.timeBeginNs = shard.timeEndNs;
    checkpointShard.timeEndNs = shard.timeEndNs;
    checkpointShard.sourceRecordBegin = shard.sourceRecordEnd;
    checkpointShard.sourceRecordEnd = shard.sourceRecordEnd;
    checkpointShard.recordCount = 1;
    if( !WriteTraceSessionShard( *state.sessionRoot, *state.generation, checkpointShard,
        checkpoint.data(), checkpoint.size(), state.error ) ) return false;
    state.previousCheckpointHash = checkpointShard.sha256;
    state.manifest->shards.emplace_back( checkpointShard );

    state.shardPayload.clear();
    state.shardRecordCount = 0;
    state.shardSourceBegin = 0;
    state.shardSourceEnd = 0;
    state.shardTimeBegin = 0;
    state.shardTimeEnd = 0;
    return SaveTraceSessionManifest( *state.sessionRoot, *state.manifest, state.error );
}

bool RestoreCheckpoint( CanonicalBuildState& state )
{
    const TraceSessionShard* checkpointShard = nullptr;
    const TraceSessionShard* previousCheckpointShard = nullptr;
    for( const auto& shard : state.manifest->shards )
    {
        state.nextShardId = std::max( state.nextShardId, shard.shardId + 1 );
        if( shard.domain == "checkpoint" )
        {
            previousCheckpointShard = checkpointShard;
            checkpointShard = &shard;
        }
    }
    if( !checkpointShard ) return true;
    std::vector<uint8_t> payload;
    if( !ReadTraceSessionShardPayload( *state.sessionRoot, *checkpointShard, payload, state.error ) ) return false;
    static constexpr uint8_t Magic[8] = { 'J', 'N', 'C', 'H', 'K', 'P', 'T', '1' };
    if( payload.size() < sizeof( Magic ) || !std::equal( std::begin( Magic ), std::end( Magic ), payload.begin() ) )
    {
        state.error = "canonical_checkpoint_magic_mismatch";
        return false;
    }
    size_t offset = sizeof( Magic );
    uint32_t version = 0;
    uint32_t reserved = 0;
    uint32_t dictionaryBytes = 0;
    uint32_t previousHashBytes = 0;
    if( !Get32( payload, offset, version ) || !Get32( payload, offset, reserved ) ||
        version != 1 || reserved != 0 ||
        !Get64( payload, offset, state.resumeAfterSequence ) ||
        !Get64( payload, offset, state.resumeOffset ) ||
        !Get64( payload, offset, state.protocolFrameOrdinal ) ||
        !Get64( payload, offset, state.protocolEventCount ) ||
        !Get64( payload, offset, state.transportRecordCount ) ||
        !Get32( payload, offset, dictionaryBytes ) ||
        !Get32( payload, offset, previousHashBytes ) ||
        previousHashBytes > 64 || offset > payload.size() ||
        dictionaryBytes > payload.size() - offset ||
        previousHashBytes > payload.size() - offset - dictionaryBytes ||
        offset + dictionaryBytes + previousHashBytes != payload.size() )
    {
        state.error = "canonical_checkpoint_payload_invalid";
        return false;
    }
    if( state.resumeAfterSequence != checkpointShard->sourceRecordEnd )
    {
        state.error = "canonical_checkpoint_sequence_mismatch";
        return false;
    }
    std::vector<uint8_t> dictionary( payload.begin() + offset,
        payload.begin() + offset + dictionaryBytes );
    offset += dictionaryBytes;
    const std::string storedPreviousHash( payload.begin() + offset, payload.end() );
    if( previousHashBytes != 0 && previousHashBytes != 64 )
    {
        state.error = "canonical_checkpoint_chain_hash_invalid";
        return false;
    }
    const std::string expectedPreviousHash = previousCheckpointShard ? previousCheckpointShard->sha256 : std::string {};
    if( storedPreviousHash != expectedPreviousHash )
    {
        state.error = "canonical_checkpoint_chain_hash_mismatch";
        return false;
    }
    if( !state.decoder.RestoreDictionary( dictionary, state.error ) ) return false;
    state.previousCheckpointHash = checkpointShard->sha256;
    return true;
}

bool ShouldFlush( const CanonicalBuildState& state )
{
    if( state.shardRecordCount == 0 ) return false;
    const auto bytes = state.shardPayload.size();
    const auto span = state.shardTimeEnd >= state.shardTimeBegin ?
        state.shardTimeEnd - state.shardTimeBegin : 0;
    if( bytes >= state.options->hardShardBytes ) return true;
    if( span >= state.options->maximumShardSpanNs ) return true;
    if( bytes >= state.options->softShardBytes ) return true;
    return bytes >= state.options->targetShardBytes && span >= state.options->minimumShardSpanNs;
}

void VisitCanonicalJournalRecord( const tracy::stream::RecordInfo& record, void* userData )
{
    auto& state = *static_cast<CanonicalBuildState*>( userData );
    if( state.failed ) return;
    if( record.sequence <= state.resumeAfterSequence ) return;
    if( state.firstResumedRecord )
    {
        state.firstResumedRecord = false;
        if( record.offset != state.resumeOffset )
        {
            state.error = "canonical_resume_offset_mismatch";
            state.failed = true;
            return;
        }
    }
    state.currentRecord = record;
    if( !ReadPayload( state, record ) ) { state.failed = true; return; }
    const bool compressed = record.type == tracy::stream::RecordType::ClientToServer &&
        ( record.flags & tracy::stream::RecordFlagCompressedFrame ) != 0;
    if( compressed )
    {
        if( !state.decoder.ConsumeCompressedRecord( state.payload, state.decodedInventory,
            state.error, VisitCanonicalProtocolEvent, &state ) )
        {
            state.failed = true;
            return;
        }
        state.protocolFrameOrdinal++;
    }
    else
    {
        if( record.payloadSize > std::numeric_limits<uint32_t>::max() )
        {
            state.error = "canonical_transport_payload_too_large";
            state.failed = true;
            return;
        }
        if( !AppendCanonicalRecord( state, 2, uint8_t( record.type ),
            uint8_t( TraceSessionProtocolDomain::Control ), record.flags,
            state.payload.data(), uint32_t( state.payload.size() ) ) )
        {
            state.failed = true;
            return;
        }
        state.transportRecordCount++;
    }
    if( ShouldFlush( state ) && !FlushShard( state ) ) state.failed = true;
    if( !state.failed && state.options->shouldCancel &&
        state.options->shouldCancel( state.options->cancelUserData ) ) state.cancelRequested = true;
}

bool StopCanonicalScan( void* userData )
{
    const auto& state = *static_cast<const CanonicalBuildState*>( userData );
    return state.failed || state.cancelRequested;
}

}

TraceSessionCanonicalBuildResult BuildTraceSessionCanonical( const std::filesystem::path& sourcePath,
    const std::filesystem::path& sessionRoot, const std::string& generation,
    const TraceSessionInventory& inventory, const TraceSessionCanonicalOptions& options,
    TraceSessionManifest& manifest, std::string& error )
{
    error.clear();
    if( generation.empty() || options.targetShardBytes == 0 ||
        options.softShardBytes < options.targetShardBytes ||
        options.hardShardBytes < options.softShardBytes )
    {
        error = "canonical_invalid_options";
        return TraceSessionCanonicalBuildResult::Failed;
    }
    TraceSessionSourceIdentity expectedSource;
    expectedSource.sha256 = inventory.sourceSha256;
    expectedSource.fileSize = inventory.sourceFileSize;
    if( !VerifyTraceSessionSourceIdentity( sourcePath, expectedSource, error ) ) return TraceSessionCanonicalBuildResult::Failed;
    if( inventory.recordCount < inventory.protocolInventory.frameCount )
    {
        error = "canonical_inventory_record_count_invalid";
        return TraceSessionCanonicalBuildResult::Failed;
    }
    CanonicalBuildState state;
    state.sessionRoot = &sessionRoot;
    state.generation = &generation;
    state.options = &options;
    state.manifest = &manifest;
    state.input.open( sourcePath, std::ios::binary );
    if( !state.input ) { error = "canonical_source_open_failed"; return TraceSessionCanonicalBuildResult::Failed; }

    std::error_code manifestEc;
    const bool canResume = options.resume && std::filesystem::exists( sessionRoot / "manifest", manifestEc );
    if( canResume )
    {
        const auto loaded = LoadTraceSessionManifest( sessionRoot, error );
        if( !loaded ) return TraceSessionCanonicalBuildResult::Failed;
        manifest = *loaded;
        if( manifest.generation != generation || manifest.source.sha256 != inventory.sourceSha256 ||
            manifest.source.fileSize != inventory.sourceFileSize ||
            manifest.source.committedRevision != inventory.committedRevision ||
            manifest.source.protocol != inventory.protocol ||
            manifest.source.captureIdentity != inventory.captureIdentity )
        {
            error = "canonical_resume_identity_mismatch";
            return TraceSessionCanonicalBuildResult::Failed;
        }
        if( manifest.state != TraceSessionState::CancelledResumable &&
            manifest.state != TraceSessionState::CanonicalBuilding &&
            manifest.state != TraceSessionState::CanonicalPaused )
        {
            error = "canonical_resume_state_invalid";
            return TraceSessionCanonicalBuildResult::Failed;
        }
        if( !VerifyTraceSession( sessionRoot, manifest, error ) || !RestoreCheckpoint( state ) )
            return TraceSessionCanonicalBuildResult::Failed;
    }
    else
    {
        manifest = {};
        manifest.sessionId = inventory.captureIdentity;
        manifest.generation = generation;
        manifest.source.sha256 = inventory.sourceSha256;
        manifest.source.fileSize = inventory.sourceFileSize;
        manifest.source.committedRevision = inventory.committedRevision;
        manifest.source.protocol = inventory.protocol;
        manifest.source.captureIdentity = inventory.captureIdentity;
        manifest.source.captureEndState = inventory.qualityReason;
    }
    manifest.state = TraceSessionState::CanonicalBuilding;
    manifest.reason.clear();
    if( !SaveTraceSessionManifest( sessionRoot, manifest, error ) ) return TraceSessionCanonicalBuildResult::Failed;

    tracy::stream::ScanOptions scanOptions;
    scanOptions.maxCollectedRecords = 0;
    scanOptions.recordVisitor = VisitCanonicalJournalRecord;
    scanOptions.recordVisitorUserData = &state;
    scanOptions.stopRequested = StopCanonicalScan;
    scanOptions.stopRequestedUserData = &state;
    const auto scan = tracy::stream::ScanJournal( sourcePath, scanOptions );
    if( !scan.HasRecoverablePrefix() )
    {
        error = "canonical_source_invalid:" + scan.message;
        return TraceSessionCanonicalBuildResult::Failed;
    }
    if( state.failed ) { error = state.error; return TraceSessionCanonicalBuildResult::Failed; }
    if( !FlushShard( state ) ) { error = state.error; return TraceSessionCanonicalBuildResult::Failed; }
    if( scan.code == tracy::stream::ScanCode::Stopped || state.cancelRequested )
    {
        manifest.state = TraceSessionState::CancelledResumable;
        manifest.reason = "cancelled_resumable";
        if( !SaveTraceSessionManifest( sessionRoot, manifest, error ) )
            return TraceSessionCanonicalBuildResult::Failed;
        return TraceSessionCanonicalBuildResult::CancelledResumable;
    }
    const auto expectedTransportRecords = inventory.recordCount - inventory.protocolInventory.frameCount;
    if( state.protocolEventCount != inventory.protocolInventory.eventCount ||
        state.transportRecordCount != expectedTransportRecords )
    {
        error = "canonical_inventory_count_mismatch";
        return TraceSessionCanonicalBuildResult::Failed;
    }
    if( !SaveTraceSessionManifest( sessionRoot, manifest, error ) )
        return TraceSessionCanonicalBuildResult::Failed;
    return TraceSessionCanonicalBuildResult::Complete;
}

}

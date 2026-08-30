#include "TracyTraceSessionCanonical.hpp"

#include "TracyQueue.hpp"
#include "TracyStreamJournal.hpp"

#include <algorithm>
#include <array>
#include <cstring>
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

bool Add64( uint64_t& value, uint64_t add )
{
    if( add > std::numeric_limits<uint64_t>::max() - value ) return false;
    value += add;
    return true;
}

struct CanonicalBuildState
{
    struct DomainBuffer
    {
        std::vector<uint8_t> payload;
        uint64_t recordCount = 0;
    };

    const std::filesystem::path* sessionRoot = nullptr;
    const std::string* generation = nullptr;
    const TraceSessionCanonicalOptions* options = nullptr;
    TraceSessionManifest* manifest = nullptr;
    TraceSessionWriterLease* lease = nullptr;
    std::ifstream input;
    std::vector<uint8_t> payload;
    std::array<DomainBuffer, size_t( TraceSessionProtocolDomain::Count )> domains;
    TraceSessionProtocolDecoder decoder;
    TraceSessionProtocolInventory decodedInventory;
    tracy::stream::RecordInfo currentRecord;
    uint64_t protocolFrameOrdinal = 0;
    uint64_t protocolEventCount = 0;
    uint64_t transportRecordCount = 0;
    uint32_t threadContext = 0;
    uint64_t shardRecordCount = 0;
    uint64_t shardPayloadBytes = 0;
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
    uint8_t domain, uint32_t flags, uint32_t threadContext,
    uint32_t variablePayloadBytes, uint32_t protocolFrameOffset,
    bool hasSemanticTime, int64_t semanticTime,
    const uint8_t* payload, uint32_t payloadBytes )
{
    if( payloadBytes != 0 && !payload ) { state.error = "canonical_payload_missing"; return false; }
    constexpr uint64_t RecordHeaderBytes = 56;
    const auto recordBytes = RecordHeaderBytes + uint64_t( payloadBytes );
    if( recordBytes > state.options->hardMemoryBytes ||
        state.shardPayloadBytes > state.options->hardMemoryBytes - recordBytes )
    {
        state.error = "canonical_memory_hard_limit";
        return false;
    }
    auto domainIndex = size_t( domain );
    if( domainIndex >= state.domains.size() )
        domainIndex = size_t( TraceSessionProtocolDomain::Other );
    auto& output = state.domains[domainIndex];
    if( state.shardRecordCount == 0 )
    {
        state.shardSourceBegin = state.currentRecord.sequence;
        state.shardTimeBegin = state.currentRecord.monotonicNs;
    }
    state.shardSourceEnd = state.currentRecord.sequence;
    state.shardTimeEnd = state.currentRecord.monotonicNs;
    const auto before = output.payload.size();
    output.payload.push_back( kind );
    output.payload.push_back( type );
    output.payload.push_back( uint8_t( domainIndex ) );
    output.payload.push_back( hasSemanticTime ? 1 : 0 );
    Put32( output.payload, flags );
    Put32( output.payload, payloadBytes );
    Put32( output.payload, threadContext );
    Put32( output.payload, variablePayloadBytes );
    Put32( output.payload, protocolFrameOffset );
    Put64( output.payload, state.currentRecord.sequence );
    Put64( output.payload, state.currentRecord.monotonicNs );
    Put64( output.payload, state.protocolFrameOrdinal );
    Put64( output.payload, uint64_t( semanticTime ) );
    if( payloadBytes != 0 )
        output.payload.insert( output.payload.end(), payload, payload + payloadBytes );
    state.shardPayloadBytes += output.payload.size() - before;
    output.recordCount++;
    state.shardRecordCount++;
    return true;
}

bool VisitCanonicalProtocolEvent( const TraceSessionProtocolEventInfo& event,
    void* userData, std::string& error )
{
    auto& state = *static_cast<CanonicalBuildState*>( userData );
    tracy::QueueItem item {};
    std::memcpy( &item, event.encodedData,
        std::min<size_t>( event.encodedBytes, sizeof( item ) ) );
    if( item.hdr.type == tracy::QueueType::ThreadContext )
        state.threadContext = item.threadCtx.thread;
    int64_t semanticTime = 0;
    const auto hasSemanticTime = TryGetTraceProtocolEventTime( event, semanticTime );
    if( !AppendCanonicalRecord( state, 1, event.queueType,
        uint8_t( ClassifyTraceProtocolEvent( event.queueType ) ), 0,
        state.threadContext, event.variablePayloadBytes, event.frameOffset,
        hasSemanticTime, semanticTime,
        event.encodedData, event.encodedBytes ) )
    {
        error = state.error;
        return false;
    }
    state.protocolEventCount++;
    return true;
}

bool HasDiskCapacity( CanonicalBuildState& state )
{
    uint64_t capacity = 0;
    uint64_t available = 0;
    if( state.options->diskSpaceProbe )
    {
        if( !state.options->diskSpaceProbe( *state.sessionRoot, capacity, available,
            state.options->diskSpaceUserData, state.error ) )
        {
            if( state.error.empty() ) state.error = "canonical_disk_space_query_failed";
            return false;
        }
    }
    else
    {
        std::error_code ec;
        const auto space = std::filesystem::space( *state.sessionRoot, ec );
        if( ec )
        {
            state.error = "canonical_disk_space_query_failed:" + ec.message();
            return false;
        }
        capacity = space.capacity;
        available = space.available;
    }
    const auto percent = state.options->minimumFreeReservePercent;
    const auto percentReserve = capacity / 100 * percent +
        capacity % 100 * percent / 100;
    const auto reserve = std::max( state.options->minimumFreeReserveBytes, percentReserve );
    constexpr uint64_t TransactionOverhead = 1024 * 1024;
    if( available < reserve || state.shardPayloadBytes > available - reserve ||
        TransactionOverhead > available - reserve - state.shardPayloadBytes )
    {
        state.error = "canonical_disk_pressure";
        return false;
    }
    return true;
}

bool FlushShard( CanonicalBuildState& state )
{
    if( state.shardRecordCount == 0 ) return true;
    if( !HasDiskCapacity( state ) ) return false;
    for( size_t i = 0; i < state.domains.size(); i++ )
    {
        auto& domain = state.domains[i];
        if( domain.recordCount == 0 ) continue;
        TraceSessionShard shard;
        shard.shardId = state.nextShardId++;
        shard.domain = TraceSessionProtocolDomainName( TraceSessionProtocolDomain( i ) );
        shard.timeBeginNs = int64_t( state.shardTimeBegin );
        shard.timeEndNs = int64_t( state.shardTimeEnd );
        shard.sourceRecordBegin = state.shardSourceBegin;
        shard.sourceRecordEnd = state.shardSourceEnd;
        shard.recordCount = domain.recordCount;
        if( !WriteTraceSessionShard( *state.sessionRoot, *state.generation, shard,
            domain.payload.data(), domain.payload.size(), state.error ) ) return false;
        state.manifest->shards.emplace_back( std::move( shard ) );
    }

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
    Put32( checkpoint, state.threadContext );
    Put32( checkpoint, 0 );
    const auto dictionary = state.decoder.ExportDictionary();
    Put32( checkpoint, uint32_t( dictionary.size() ) );
    Put32( checkpoint, uint32_t( state.previousCheckpointHash.size() ) );
    checkpoint.insert( checkpoint.end(), dictionary.begin(), dictionary.end() );
    checkpoint.insert( checkpoint.end(), state.previousCheckpointHash.begin(), state.previousCheckpointHash.end() );

    TraceSessionShard checkpointShard;
    checkpointShard.shardId = state.nextShardId++;
    checkpointShard.domain = "checkpoint";
    checkpointShard.timeBeginNs = int64_t( state.shardTimeEnd );
    checkpointShard.timeEndNs = int64_t( state.shardTimeEnd );
    checkpointShard.sourceRecordBegin = state.shardSourceEnd;
    checkpointShard.sourceRecordEnd = state.shardSourceEnd;
    checkpointShard.recordCount = 1;
    if( !WriteTraceSessionShard( *state.sessionRoot, *state.generation, checkpointShard,
        checkpoint.data(), checkpoint.size(), state.error ) ) return false;
    state.previousCheckpointHash = checkpointShard.sha256;
    state.manifest->shards.emplace_back( checkpointShard );

    for( auto& domain : state.domains )
    {
        std::vector<uint8_t>().swap( domain.payload );
        domain.recordCount = 0;
    }
    state.shardRecordCount = 0;
    state.shardPayloadBytes = 0;
    state.shardSourceBegin = 0;
    state.shardSourceEnd = 0;
    state.shardTimeBegin = 0;
    state.shardTimeEnd = 0;
    if( !SaveTraceSessionManifest( *state.sessionRoot, *state.manifest, state.error ) ) return false;
    return state.lease->Heartbeat( state.error );
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
    uint32_t reservedState = 0;
    if( !Get32( payload, offset, version ) || !Get32( payload, offset, reserved ) ||
        version != 1 || reserved != 0 ||
        !Get64( payload, offset, state.resumeAfterSequence ) ||
        !Get64( payload, offset, state.resumeOffset ) ||
        !Get64( payload, offset, state.protocolFrameOrdinal ) ||
        !Get64( payload, offset, state.protocolEventCount ) ||
        !Get64( payload, offset, state.transportRecordCount ) ||
        !Get32( payload, offset, state.threadContext ) ||
        !Get32( payload, offset, reservedState ) || reservedState != 0 ||
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
    const auto bytes = state.shardPayloadBytes;
    const auto span = state.shardTimeEnd >= state.shardTimeBegin ?
        state.shardTimeEnd - state.shardTimeBegin : 0;
    if( bytes >= state.options->hardShardBytes ) return true;
    if( bytes >= state.options->softMemoryBytes ) return true;
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
        if( !AppendCanonicalRecord( state,
            uint8_t( TraceSessionCanonicalRecordKind::ProtocolFrame ), 0,
            uint8_t( TraceSessionProtocolDomain::Control ), record.flags,
            state.threadContext, 0, std::numeric_limits<uint32_t>::max(),
            false, 0, nullptr, 0 ) )
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
            state.threadContext, 0, 0, false, 0,
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

struct CanonicalAuditVisitorState
{
    TraceSessionCanonicalAudit* audit = nullptr;
};

bool VisitCanonicalForAudit( const TraceSessionCanonicalRecord& record,
    void* userData, std::string& error )
{
    auto& audit = *static_cast<CanonicalAuditVisitorState*>( userData )->audit;
    if( record.kind == TraceSessionCanonicalRecordKind::TransportRecord )
    {
        if( !Add64( audit.transportRecords, 1 ) ||
            !Add64( audit.transportPayloadBytes, record.payload.size() ) )
        {
            error = "canonical_audit_counter_overflow";
            return false;
        }
        return true;
    }
    if( record.kind == TraceSessionCanonicalRecordKind::ProtocolFrame )
    {
        if( record.domain != TraceSessionProtocolDomain::Control || !record.payload.empty() ||
            record.protocolFrameOrdinal == std::numeric_limits<uint64_t>::max() ||
            !Add64( audit.protocolFrames, 1 ) )
        {
            error = "canonical_audit_protocol_frame_invalid";
            return false;
        }
        return true;
    }
    if( record.type >= audit.events.size() ||
        ClassifyTraceProtocolEvent( record.type ) != record.domain )
    {
        error = "canonical_audit_event_domain_mismatch";
        return false;
    }
    auto& event = audit.events[record.type];
    auto& domain = audit.domains[size_t( record.domain )];
    if( !Add64( event.count, 1 ) || !Add64( event.encodedBytes, record.payload.size() ) ||
        !Add64( event.variablePayloadBytes, record.variablePayloadBytes ) ||
        !Add64( domain.count, 1 ) || !Add64( domain.encodedBytes, record.payload.size() ) ||
        !Add64( domain.variablePayloadBytes, record.variablePayloadBytes ) ||
        !Add64( audit.protocolEvents, 1 ) ||
        !Add64( audit.protocolEncodedBytes, record.payload.size() ) )
    {
        error = "canonical_audit_counter_overflow";
        return false;
    }
    if( record.hasSemanticTime && !Add64( audit.semanticTimeEvents, 1 ) )
    {
        error = "canonical_audit_counter_overflow";
        return false;
    }
    return true;
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
        options.hardShardBytes < options.softShardBytes ||
        options.softMemoryBytes < options.hardShardBytes ||
        options.hardMemoryBytes < options.softMemoryBytes ||
        options.minimumFreeReservePercent > 100 )
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
    TraceSessionWriterLease lease;
    if( !AcquireTraceSessionWriterLease( sessionRoot, lease, error ) )
        return TraceSessionCanonicalBuildResult::Failed;
    CanonicalBuildState state;
    state.sessionRoot = &sessionRoot;
    state.generation = &generation;
    state.options = &options;
    state.manifest = &manifest;
    state.lease = &lease;
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
    if( !lease.Heartbeat( error ) ) return TraceSessionCanonicalBuildResult::Failed;

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
    if( state.failed )
    {
        error = state.error;
        if( error == "canonical_memory_hard_limit" )
        {
            manifest.state = TraceSessionState::InvalidCapacity;
            manifest.reason = "resource_limit";
            std::string persistError;
            if( !SaveTraceSessionManifest( sessionRoot, manifest, persistError ) )
                error += ":" + persistError;
            else if( !lease.Heartbeat( persistError ) ) error += ":" + persistError;
        }
        else if( error == "canonical_disk_pressure" )
        {
            manifest.state = TraceSessionState::InsufficientDisk;
            manifest.reason = "insufficient_disk";
            std::string persistError;
            if( !SaveTraceSessionManifest( sessionRoot, manifest, persistError ) )
                error += ":" + persistError;
            else if( !lease.Heartbeat( persistError ) ) error += ":" + persistError;
        }
        return TraceSessionCanonicalBuildResult::Failed;
    }
    if( !FlushShard( state ) ) { error = state.error; return TraceSessionCanonicalBuildResult::Failed; }
    if( scan.code == tracy::stream::ScanCode::Stopped || state.cancelRequested )
    {
        manifest.state = TraceSessionState::CancelledResumable;
        manifest.reason = "cancelled_resumable";
        if( !SaveTraceSessionManifest( sessionRoot, manifest, error ) )
            return TraceSessionCanonicalBuildResult::Failed;
        if( !lease.Heartbeat( error ) ) return TraceSessionCanonicalBuildResult::Failed;
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
    if( !lease.Heartbeat( error ) ) return TraceSessionCanonicalBuildResult::Failed;
    return TraceSessionCanonicalBuildResult::Complete;
}

bool VisitTraceSessionCanonicalShard( const std::filesystem::path& sessionRoot,
    const TraceSessionShard& shard, TraceSessionCanonicalRecordVisitor visitor,
    void* userData, std::string& error )
{
    error.clear();
    if( shard.domain == "checkpoint" )
    {
        error = "canonical_checkpoint_is_not_event_shard";
        return false;
    }
    std::vector<uint8_t> bytes;
    if( !ReadTraceSessionShardPayload( sessionRoot, shard, bytes, error ) ) return false;
    constexpr size_t RecordHeaderBytes = 56;
    size_t offset = 0;
    uint64_t recordCount = 0;
    while( offset < bytes.size() )
    {
        if( bytes.size() - offset < RecordHeaderBytes )
        {
            error = "canonical_shard_record_header_truncated";
            return false;
        }
        TraceSessionCanonicalRecord record;
        const auto kind = bytes[offset++];
        record.type = bytes[offset++];
        const auto domain = bytes[offset++];
        const auto canonicalFlags = bytes[offset++];
        uint32_t payloadBytes = 0;
        if( ( kind != uint8_t( TraceSessionCanonicalRecordKind::ProtocolEvent ) &&
              kind != uint8_t( TraceSessionCanonicalRecordKind::TransportRecord ) &&
              kind != uint8_t( TraceSessionCanonicalRecordKind::ProtocolFrame ) ) ||
            domain >= uint8_t( TraceSessionProtocolDomain::Count ) || ( canonicalFlags & ~1u ) != 0 ||
            !Get32( bytes, offset, record.flags ) || !Get32( bytes, offset, payloadBytes ) ||
            !Get32( bytes, offset, record.threadContext ) ||
            !Get32( bytes, offset, record.variablePayloadBytes ) ||
            !Get32( bytes, offset, record.protocolFrameOffset ) ||
            !Get64( bytes, offset, record.sourceSequence ) ||
            !Get64( bytes, offset, record.journalMonotonicNs ) ||
            !Get64( bytes, offset, record.protocolFrameOrdinal ) )
        {
            error = "canonical_shard_record_header_invalid";
            return false;
        }
        record.kind = TraceSessionCanonicalRecordKind( kind );
        record.domain = TraceSessionProtocolDomain( domain );
        record.hasSemanticTime = ( canonicalFlags & 1 ) != 0;
        uint64_t semanticTime = 0;
        if( !Get64( bytes, offset, semanticTime ) )
        {
            error = "canonical_shard_record_header_invalid";
            return false;
        }
        record.semanticTime = int64_t( semanticTime );
        if( shard.domain != TraceSessionProtocolDomainName( record.domain ) )
        {
            error = "canonical_shard_domain_mismatch";
            return false;
        }
        if( record.kind == TraceSessionCanonicalRecordKind::ProtocolEvent &&
            record.type >= uint8_t( tracy::QueueType::NUM_TYPES ) )
        {
            error = "canonical_shard_queue_type_invalid";
            return false;
        }
        if( record.kind == TraceSessionCanonicalRecordKind::TransportRecord &&
            ( record.domain != TraceSessionProtocolDomain::Control ||
              record.variablePayloadBytes != 0 || record.protocolFrameOffset != 0 ) )
        {
            error = "canonical_transport_domain_invalid";
            return false;
        }
        if( record.kind == TraceSessionCanonicalRecordKind::ProtocolFrame &&
            ( record.domain != TraceSessionProtocolDomain::Control || payloadBytes != 0 ||
              record.variablePayloadBytes != 0 ||
              record.protocolFrameOffset != std::numeric_limits<uint32_t>::max() ) )
        {
            error = "canonical_protocol_frame_record_invalid";
            return false;
        }
        if( record.sourceSequence < shard.sourceRecordBegin ||
            record.sourceSequence > shard.sourceRecordEnd ||
            payloadBytes > bytes.size() - offset ||
            record.variablePayloadBytes > payloadBytes )
        {
            error = "canonical_shard_record_range_invalid";
            return false;
        }
        record.payload = std::span<const uint8_t>( bytes.data() + offset, payloadBytes );
        offset += payloadBytes;
        recordCount++;
        if( visitor && !visitor( record, userData, error ) )
        {
            if( error.empty() ) error = "canonical_shard_visitor_failed";
            return false;
        }
    }
    if( recordCount != shard.recordCount )
    {
        error = "canonical_shard_record_count_mismatch";
        return false;
    }
    return true;
}

bool AuditTraceSessionCanonical( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, const TraceSessionInventory& inventory,
    TraceSessionCanonicalAudit& audit, std::string& error )
{
    error.clear();
    audit = {};
    if( !VerifyTraceSession( sessionRoot, manifest, error ) ) return false;
    CanonicalAuditVisitorState state { &audit };
    for( const auto& shard : manifest.shards )
    {
        if( shard.domain == "checkpoint" ) continue;
        if( !VisitTraceSessionCanonicalShard( sessionRoot, shard,
            VisitCanonicalForAudit, &state, error ) ) return false;
    }
    const auto expectedTransportRecords = inventory.recordCount - inventory.protocolInventory.frameCount;
    if( audit.protocolFrames != inventory.protocolInventory.frameCount ||
        audit.protocolEvents != inventory.protocolInventory.eventCount ||
        audit.protocolEncodedBytes != inventory.protocolInventory.encodedBytes ||
        audit.transportRecords != expectedTransportRecords )
    {
        error = "canonical_audit_global_count_mismatch";
        return false;
    }
    if( audit.events != inventory.protocolInventory.events )
    {
        error = "canonical_audit_event_count_mismatch";
        return false;
    }
    if( audit.domains != inventory.protocolInventory.domains )
    {
        error = "canonical_audit_domain_count_mismatch";
        return false;
    }
    return true;
}

}

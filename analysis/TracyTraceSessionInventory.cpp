#include "TracyTraceSessionInventory.hpp"

#include "TracyHash.hpp"
#include "TracyStreamJournal.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>
#include <vector>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace tracy::analysis
{
namespace
{

uint64_t SaturatingAdd( uint64_t left, uint64_t right )
{
    if( right > std::numeric_limits<uint64_t>::max() - left ) return std::numeric_limits<uint64_t>::max();
    return left + right;
}

uint64_t SaturatingScalePermille( uint64_t value, uint32_t permille )
{
    const auto whole = value / 1000;
    const auto remainder = value % 1000;
    if( whole != 0 && permille > std::numeric_limits<uint64_t>::max() / whole )
        return std::numeric_limits<uint64_t>::max();
    const auto scaledWhole = whole * permille;
    const auto scaledRemainder = ( remainder * uint64_t( permille ) + 999 ) / 1000;
    return SaturatingAdd( scaledWhole, scaledRemainder );
}

TraceSessionJournalClass Classify( tracy::stream::RecordType type )
{
    using tracy::stream::RecordType;
    switch( type )
    {
    case RecordType::SessionBegin: return TraceSessionJournalClass::SessionBegin;
    case RecordType::ClientToServer: return TraceSessionJournalClass::ClientToServer;
    case RecordType::ServerToClient: return TraceSessionJournalClass::ServerToClient;
    case RecordType::Checkpoint: return TraceSessionJournalClass::Checkpoint;
    case RecordType::SessionEnd: return TraceSessionJournalClass::SessionEnd;
    case RecordType::Diagnostic: return TraceSessionJournalClass::Diagnostic;
    }
    return TraceSessionJournalClass::Diagnostic;
}

struct InventoryVisitorState
{
    TraceSessionInventory* inventory = nullptr;
    const TraceSessionInventoryOptions* options = nullptr;
    uint64_t sourceFileSize = 0;
    bool hasTimestamp = false;
    std::ifstream payloadInput;
    std::vector<uint8_t> payload;
    TraceSessionProtocolDecoder protocolDecoder;
    bool protocolFailed = false;
    std::string protocolError;
};

bool ReadRecordPayload( InventoryVisitorState& state,
    const tracy::stream::RecordInfo& record )
{
    if( !state.payloadInput )
    {
        state.protocolError = "cannot open source for inventory payload";
        return false;
    }
    constexpr auto HeaderBytes = uint64_t( tracy::stream::RecordHeaderSize );
    const auto maxOffset = uint64_t( std::numeric_limits<std::streamoff>::max() );
    if( record.payloadSize > std::numeric_limits<size_t>::max() ||
        record.offset > maxOffset || HeaderBytes > maxOffset - record.offset )
    {
        state.protocolError = "inventory record exceeds platform limits";
        return false;
    }
    state.payload.resize( size_t( record.payloadSize ) );
    state.payloadInput.clear();
    state.payloadInput.seekg( std::streamoff( record.offset + HeaderBytes ), std::ios::beg );
    if( !state.payloadInput )
    {
        state.protocolError = "cannot seek inventory record";
        return false;
    }
    if( state.payload.empty() ) return true;
    if( state.payload.size() > size_t( std::numeric_limits<std::streamsize>::max() ) )
    {
        state.protocolError = "inventory payload exceeds stream limits";
        return false;
    }
    state.payloadInput.read( reinterpret_cast<char*>( state.payload.data() ),
        std::streamsize( state.payload.size() ) );
    if( !state.payloadInput || state.payloadInput.gcount() != std::streamsize( state.payload.size() ) )
    {
        state.protocolError = "cannot read inventory record";
        return false;
    }
    return true;
}

uint16_t Read16( const uint8_t* data )
{
    return uint16_t( data[0] ) | uint16_t( uint16_t( data[1] ) << 8 );
}

uint32_t Read32( const uint8_t* data )
{
    return uint32_t( data[0] ) | uint32_t( data[1] ) << 8 |
        uint32_t( data[2] ) << 16 | uint32_t( data[3] ) << 24;
}

uint64_t Read64( const uint8_t* data )
{
    return uint64_t( Read32( data ) ) | uint64_t( Read32( data + 4 ) ) << 32;
}

std::string CaptureEndReasonName( uint32_t reason )
{
    switch( reason )
    {
    case 1: return "local_shutdown";
    case 2: return "peer_disconnected";
    case 3: return "capture_complete";
    case 4: return "protocol_mismatch";
    case 5: return "not_available";
    case 6: return "handshake_dropped";
    case 7: return "memory_limit";
    case 8: return "instrumentation_failure";
    case 9: return "recorder_failure";
    case 10: return "transport_error";
    case 11: return "observer_destroyed";
    default: return "unknown_capture_end_reason";
    }
}

void VisitRecord( const tracy::stream::RecordInfo& record, void* userData )
{
    auto& state = *static_cast<InventoryVisitorState*>( userData );
    auto& inventory = *state.inventory;
    auto& stats = inventory.records[Classify( record.type )];
    stats.count++;
    stats.payloadBytes = SaturatingAdd( stats.payloadBytes, record.payloadSize );
    stats.committedBytes = SaturatingAdd( stats.committedBytes,
        tracy::stream::RecordHeaderSize + record.payloadSize + tracy::stream::RecordTrailerSize );
    if( !state.hasTimestamp )
    {
        inventory.firstMonotonicNs = record.monotonicNs;
        state.hasTimestamp = true;
    }
    inventory.lastMonotonicNs = record.monotonicNs;
    const bool compressedFrame = record.type == tracy::stream::RecordType::ClientToServer &&
        ( record.flags & tracy::stream::RecordFlagCompressedFrame ) != 0;
    const bool terminal = record.type == tracy::stream::RecordType::SessionEnd;
    if( !state.protocolFailed && ( compressedFrame || terminal ) )
    {
        if( !ReadRecordPayload( state, record ) )
        {
            state.protocolFailed = true;
        }
        else if( compressedFrame && !state.protocolDecoder.ConsumeCompressedRecord(
            state.payload, inventory.protocolInventory, state.protocolError ) )
        {
            state.protocolFailed = true;
            state.protocolError = "protocol record " + std::to_string( record.sequence ) + ": " + state.protocolError;
        }
        else if( terminal && state.payload.size() >= 24 )
        {
            const auto schema = Read16( state.payload.data() );
            const auto declaredSize = Read16( state.payload.data() + 2 );
            if( schema == 1 && declaredSize == state.payload.size() )
            {
                inventory.captureEndMetadataPresent = true;
                inventory.captureEndReason = Read32( state.payload.data() + 4 );
                inventory.captureEndClientBytes = Read64( state.payload.data() + 8 );
                inventory.captureEndServerBytes = Read64( state.payload.data() + 16 );
            }
        }
    }
    if( state.options->progress )
    {
        const auto completed = record.offset + tracy::stream::RecordHeaderSize +
            record.payloadSize + tracy::stream::RecordTrailerSize;
        state.options->progress( TraceSessionInventoryPhase::JournalScan,
            completed, state.sourceFileSize, state.options->progressUserData );
    }
}

bool HashSource( const std::filesystem::path& sourcePath, uint64_t sourceFileSize,
    const TraceSessionInventoryOptions& options, std::string& sha256, std::string& error )
{
    std::ifstream input( sourcePath, std::ios::binary );
    if( !input )
    {
        error = "cannot open source for SHA-256";
        return false;
    }
    Sha256Builder hash;
    std::vector<uint8_t> buffer( 4 * 1024 * 1024 );
    uint64_t completed = 0;
    if( options.progress ) options.progress( TraceSessionInventoryPhase::SourceHash,
        0, sourceFileSize, options.progressUserData );
    while( input )
    {
        input.read( reinterpret_cast<char*>( buffer.data() ), std::streamsize( buffer.size() ) );
        const auto bytes = size_t( input.gcount() );
        if( bytes == 0 ) break;
        hash.Update( buffer.data(), bytes );
        completed += bytes;
        if( options.progress ) options.progress( TraceSessionInventoryPhase::SourceHash,
            completed, sourceFileSize, options.progressUserData );
    }
    if( !input.eof() || completed != sourceFileSize )
    {
        error = "source changed or failed while computing SHA-256";
        return false;
    }
    sha256 = hash.FinalHex();
    return true;
}

std::string SessionIdentityHex( const std::array<uint8_t, 16>& identity )
{
    static constexpr char Hex[] = "0123456789abcdef";
    std::string result;
    result.resize( identity.size() * 2 );
    for( size_t i = 0; i < identity.size(); i++ )
    {
        result[i * 2] = Hex[identity[i] >> 4];
        result[i * 2 + 1] = Hex[identity[i] & 0xf];
    }
    return result;
}

std::string QualityReason( tracy::stream::ScanCode code, bool complete )
{
    using tracy::stream::ScanCode;
    switch( code )
    {
    case ScanCode::Ok: return complete ? "complete" : "missing_session_end";
    case ScanCode::TruncatedTail: return "truncated_tail";
    case ScanCode::CorruptTail: return "corrupt_tail";
    case ScanCode::TruncatedFileHeader: return "truncated_file_header";
    case ScanCode::InvalidFileHeader: return "invalid_file_header";
    case ScanCode::IoError: return "io_error";
    }
    return "unknown";
}

bool AtomicReplaceFile( const std::filesystem::path& temporary,
    const std::filesystem::path& finalPath, std::string& error )
{
#ifdef _WIN32
    if( MoveFileExW( temporary.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) )
        return true;
    error = "cannot atomically publish inventory: " + std::to_string( GetLastError() );
    return false;
#else
    std::error_code filesystemError;
    std::filesystem::rename( temporary, finalPath, filesystemError );
    if( !filesystemError ) return true;
    error = "cannot atomically publish inventory: " + filesystemError.message();
    return false;
#endif
}

}

bool BuildTraceSessionInventory( const std::filesystem::path& sourcePath,
    const TraceSessionInventoryOptions& options, TraceSessionInventory& inventory, std::string& error )
{
    error.clear();
    inventory = {};
    if( options.maxPayloadBytes == 0 )
    {
        error = "inventory maximum payload must be non-zero";
        return false;
    }

    std::error_code filesystemError;
    inventory.sourceFileSize = std::filesystem::file_size( sourcePath, filesystemError );
    if( filesystemError )
    {
        error = "cannot determine source size: " + filesystemError.message();
        return false;
    }

    InventoryVisitorState visitor;
    visitor.inventory = &inventory;
    visitor.options = &options;
    visitor.sourceFileSize = inventory.sourceFileSize;
    visitor.payloadInput.open( sourcePath, std::ios::binary );
    if( !visitor.payloadInput )
    {
        error = "cannot open source for protocol inventory";
        return false;
    }
    if( options.progress ) options.progress( TraceSessionInventoryPhase::JournalScan,
        0, inventory.sourceFileSize, options.progressUserData );
    tracy::stream::ScanOptions scanOptions;
    scanOptions.maxPayloadSize = options.maxPayloadBytes;
    scanOptions.maxCollectedRecords = 0;
    scanOptions.recordVisitor = VisitRecord;
    scanOptions.recordVisitorUserData = &visitor;
    const auto scan = tracy::stream::ScanJournal( sourcePath, scanOptions );
    if( !scan.HasRecoverablePrefix() )
    {
        error = "source journal has no recoverable committed prefix: " + scan.message;
        return false;
    }
    if( visitor.protocolFailed )
    {
        error = visitor.protocolError;
        return false;
    }
    inventory.protocolInventoryComplete = true;
    if( options.progress ) options.progress( TraceSessionInventoryPhase::JournalScan,
        inventory.sourceFileSize, inventory.sourceFileSize, options.progressUserData );
    if( !HashSource( sourcePath, inventory.sourceFileSize, options, inventory.sourceSha256, error ) ) return false;

    inventory.protocol = scan.header.protocolVersion;
    inventory.captureIdentity = SessionIdentityHex( scan.header.sessionId );
    inventory.validSize = scan.validSize;
    inventory.recordCount = scan.recordCount;
    inventory.committedRevision = scan.lastSequence;
    inventory.lastMonotonicNs = scan.lastMonotonicNs;
    inventory.prefixCrc32c = scan.prefixCrc32c;
    inventory.complete = scan.complete;
    inventory.sourceDegraded = scan.code != tracy::stream::ScanCode::Ok || !scan.complete;
    inventory.qualityReason = QualityReason( scan.code, scan.complete );
    if( scan.complete )
    {
        if( !inventory.captureEndMetadataPresent )
        {
            inventory.sourceDegraded = true;
            inventory.qualityReason = "terminal_metadata_unavailable";
        }
        else
        {
            inventory.qualityReason = CaptureEndReasonName( inventory.captureEndReason );
            if( inventory.captureEndReason != 1 && inventory.captureEndReason != 3 )
                inventory.sourceDegraded = true;
        }
    }
    inventory.retainedRecordMetadata = scan.records.size();

    inventory.estimatedCanonicalBytes = std::max( inventory.validSize,
        SaturatingScalePermille( inventory.validSize, options.canonicalEstimatePermille ) );
    inventory.estimatedDerivedBytes = SaturatingScalePermille(
        inventory.validSize, options.derivedEstimatePermille );
    inventory.estimatedTemporaryBytes = SaturatingScalePermille(
        inventory.validSize, options.temporaryEstimatePermille );
    inventory.estimatedTotalBuildBytes = SaturatingAdd( inventory.estimatedCanonicalBytes,
        SaturatingAdd( inventory.estimatedDerivedBytes, inventory.estimatedTemporaryBytes ) );
    return true;
}

bool SaveTraceSessionInventory( const std::filesystem::path& path,
    const TraceSessionInventory& inventory, std::string& error )
{
    error.clear();
    std::error_code filesystemError;
    if( path.has_parent_path() ) std::filesystem::create_directories( path.parent_path(), filesystemError );
    if( filesystemError )
    {
        error = "cannot create inventory directory: " + filesystemError.message();
        return false;
    }
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output( temporary, std::ios::binary | std::ios::trunc );
        if( !output )
        {
            error = "cannot create inventory file";
            return false;
        }
        output << "JN_TRACE_SESSION_INVENTORY\n"
            << "schema " << inventory.schema << '\n'
            << "protocol " << inventory.protocol << '\n'
            << "capture_identity " << inventory.captureIdentity << '\n'
            << "source_sha256 " << inventory.sourceSha256 << '\n'
            << "source_file_size " << inventory.sourceFileSize << '\n'
            << "valid_size " << inventory.validSize << '\n'
            << "record_count " << inventory.recordCount << '\n'
            << "committed_revision " << inventory.committedRevision << '\n'
            << "first_monotonic_ns " << inventory.firstMonotonicNs << '\n'
            << "last_monotonic_ns " << inventory.lastMonotonicNs << '\n'
            << "prefix_crc32c " << inventory.prefixCrc32c << '\n'
            << "complete " << ( inventory.complete ? 1 : 0 ) << '\n'
            << "source_degraded " << ( inventory.sourceDegraded ? 1 : 0 ) << '\n'
            << "quality_reason " << inventory.qualityReason << '\n'
            << "capture_end_metadata_present " << ( inventory.captureEndMetadataPresent ? 1 : 0 ) << '\n'
            << "capture_end_reason " << inventory.captureEndReason << '\n'
            << "capture_end_client_bytes " << inventory.captureEndClientBytes << '\n'
            << "capture_end_server_bytes " << inventory.captureEndServerBytes << '\n'
            << "protocol_inventory_complete " << ( inventory.protocolInventoryComplete ? 1 : 0 ) << '\n'
            << "protocol_frames " << inventory.protocolInventory.frameCount << '\n'
            << "protocol_events " << inventory.protocolInventory.eventCount << '\n'
            << "protocol_encoded_bytes " << inventory.protocolInventory.encodedBytes << '\n'
            << "protocol_compressed_bytes " << inventory.protocolInventory.compressedBytes << '\n'
            << "retained_record_metadata " << inventory.retainedRecordMetadata << '\n'
            << "estimated_canonical_bytes " << inventory.estimatedCanonicalBytes << '\n'
            << "estimated_derived_bytes " << inventory.estimatedDerivedBytes << '\n'
            << "estimated_temporary_bytes " << inventory.estimatedTemporaryBytes << '\n'
            << "estimated_total_build_bytes " << inventory.estimatedTotalBuildBytes << '\n';
        for( size_t i = 0; i < inventory.records.size(); i++ )
        {
            const auto& record = inventory.records[i];
            output << "record " << i << ' ' << record.count << ' '
                << record.payloadBytes << ' ' << record.committedBytes << '\n';
        }
        for( size_t i = 0; i < inventory.protocolInventory.events.size(); i++ )
        {
            const auto& event = inventory.protocolInventory.events[i];
            if( event.count == 0 ) continue;
            output << "protocol_event " << i << ' ' << event.count << ' '
                << event.encodedBytes << ' ' << event.variablePayloadBytes << '\n';
        }
        for( size_t i = 0; i < inventory.protocolInventory.domains.size(); i++ )
        {
            const auto& domain = inventory.protocolInventory.domains[i];
            output << "protocol_domain " << i << ' ' << domain.count << ' '
                << domain.encodedBytes << ' ' << domain.variablePayloadBytes << '\n';
        }
        output.flush();
        if( !output )
        {
            error = "cannot write inventory file";
            return false;
        }
    }
    if( AtomicReplaceFile( temporary, path, error ) ) return true;
    std::filesystem::remove( temporary, filesystemError );
    return false;
}

std::optional<TraceSessionInventory> LoadTraceSessionInventory(
    const std::filesystem::path& path, std::string& error )
{
    error.clear();
    std::ifstream input( path, std::ios::binary );
    if( !input )
    {
        error = "cannot open inventory file";
        return std::nullopt;
    }
    std::string magic;
    std::getline( input, magic );
    if( magic != "JN_TRACE_SESSION_INVENTORY" )
    {
        error = "invalid inventory magic";
        return std::nullopt;
    }
    TraceSessionInventory inventory;
    std::string key;
    size_t recordsRead = 0;
    size_t protocolEventsRead = 0;
    size_t protocolDomainsRead = 0;
    while( input >> key )
    {
        if( key == "schema" ) input >> inventory.schema;
        else if( key == "protocol" ) input >> inventory.protocol;
        else if( key == "capture_identity" ) input >> inventory.captureIdentity;
        else if( key == "source_sha256" ) input >> inventory.sourceSha256;
        else if( key == "source_file_size" ) input >> inventory.sourceFileSize;
        else if( key == "valid_size" ) input >> inventory.validSize;
        else if( key == "record_count" ) input >> inventory.recordCount;
        else if( key == "committed_revision" ) input >> inventory.committedRevision;
        else if( key == "first_monotonic_ns" ) input >> inventory.firstMonotonicNs;
        else if( key == "last_monotonic_ns" ) input >> inventory.lastMonotonicNs;
        else if( key == "prefix_crc32c" ) input >> inventory.prefixCrc32c;
        else if( key == "complete" ) { int value = 0; input >> value; inventory.complete = value != 0; }
        else if( key == "source_degraded" ) { int value = 0; input >> value; inventory.sourceDegraded = value != 0; }
        else if( key == "quality_reason" ) input >> inventory.qualityReason;
        else if( key == "capture_end_metadata_present" ) { int value = 0; input >> value; inventory.captureEndMetadataPresent = value != 0; }
        else if( key == "capture_end_reason" ) input >> inventory.captureEndReason;
        else if( key == "capture_end_client_bytes" ) input >> inventory.captureEndClientBytes;
        else if( key == "capture_end_server_bytes" ) input >> inventory.captureEndServerBytes;
        else if( key == "protocol_inventory_complete" ) { int value = 0; input >> value; inventory.protocolInventoryComplete = value != 0; }
        else if( key == "protocol_frames" ) input >> inventory.protocolInventory.frameCount;
        else if( key == "protocol_events" ) input >> inventory.protocolInventory.eventCount;
        else if( key == "protocol_encoded_bytes" ) input >> inventory.protocolInventory.encodedBytes;
        else if( key == "protocol_compressed_bytes" ) input >> inventory.protocolInventory.compressedBytes;
        else if( key == "retained_record_metadata" ) input >> inventory.retainedRecordMetadata;
        else if( key == "estimated_canonical_bytes" ) input >> inventory.estimatedCanonicalBytes;
        else if( key == "estimated_derived_bytes" ) input >> inventory.estimatedDerivedBytes;
        else if( key == "estimated_temporary_bytes" ) input >> inventory.estimatedTemporaryBytes;
        else if( key == "estimated_total_build_bytes" ) input >> inventory.estimatedTotalBuildBytes;
        else if( key == "record" )
        {
            size_t index = 0;
            input >> index;
            if( index >= inventory.records.size() )
            {
                error = "inventory record class is out of range";
                return std::nullopt;
            }
            auto& record = inventory.records[index];
            input >> record.count >> record.payloadBytes >> record.committedBytes;
            recordsRead++;
        }
        else if( key == "protocol_event" )
        {
            size_t index = 0;
            input >> index;
            if( index >= inventory.protocolInventory.events.size() )
            {
                error = "inventory protocol event type is out of range";
                return std::nullopt;
            }
            auto& event = inventory.protocolInventory.events[index];
            input >> event.count >> event.encodedBytes >> event.variablePayloadBytes;
            protocolEventsRead++;
        }
        else if( key == "protocol_domain" )
        {
            size_t index = 0;
            input >> index;
            if( index >= inventory.protocolInventory.domains.size() )
            {
                error = "inventory protocol domain is out of range";
                return std::nullopt;
            }
            auto& domain = inventory.protocolInventory.domains[index];
            input >> domain.count >> domain.encodedBytes >> domain.variablePayloadBytes;
            protocolDomainsRead++;
        }
        else
        {
            error = "unknown inventory field: " + key;
            return std::nullopt;
        }
        if( !input )
        {
            error = "truncated inventory field: " + key;
            return std::nullopt;
        }
    }
    if( inventory.schema != TraceSessionInventorySchemaVersion || recordsRead != inventory.records.size() )
    {
        error = "unsupported or incomplete inventory schema";
        return std::nullopt;
    }
    size_t expectedProtocolEventTypes = 0;
    uint64_t protocolEventCount = 0;
    uint64_t protocolEncodedBytes = 0;
    for( const auto& event : inventory.protocolInventory.events )
    {
        if( event.count != 0 ) expectedProtocolEventTypes++;
        if( event.count > std::numeric_limits<uint64_t>::max() - protocolEventCount ||
            event.encodedBytes > std::numeric_limits<uint64_t>::max() - protocolEncodedBytes )
        {
            error = "protocol event inventory counter overflow";
            return std::nullopt;
        }
        protocolEventCount += event.count;
        protocolEncodedBytes += event.encodedBytes;
    }
    if( protocolEventsRead != expectedProtocolEventTypes ||
        protocolEventCount != inventory.protocolInventory.eventCount ||
        protocolEncodedBytes != inventory.protocolInventory.encodedBytes )
    {
        error = "incomplete protocol event inventory";
        return std::nullopt;
    }
    uint64_t protocolDomainCount = 0;
    uint64_t protocolDomainBytes = 0;
    for( const auto& domain : inventory.protocolInventory.domains )
    {
        if( domain.count > std::numeric_limits<uint64_t>::max() - protocolDomainCount ||
            domain.encodedBytes > std::numeric_limits<uint64_t>::max() - protocolDomainBytes )
        {
            error = "protocol domain inventory counter overflow";
            return std::nullopt;
        }
        protocolDomainCount += domain.count;
        protocolDomainBytes += domain.encodedBytes;
    }
    if( protocolDomainsRead != inventory.protocolInventory.domains.size() ||
        protocolDomainCount != inventory.protocolInventory.eventCount ||
        protocolDomainBytes != inventory.protocolInventory.encodedBytes )
    {
        error = "incomplete protocol domain inventory";
        return std::nullopt;
    }
    return inventory;
}

bool EvaluateTraceSessionCapacity( const TraceSessionInventory& inventory,
    uint64_t volumeCapacityBytes, uint64_t volumeAvailableBytes,
    const TraceSessionCapacityPolicy& policy, TraceSessionCapacityResult& result )
{
    result = {};
    result.estimatedBuildBytes = inventory.estimatedTotalBuildBytes;
    if( inventory.estimatedTotalBuildBytes > policy.maxSessionStoreBytes )
    {
        result.reason = "session_store_limit";
        return false;
    }
    const auto percentReserve = SaturatingScalePermille( volumeCapacityBytes,
        policy.minimumFreeReservePercent * 10 );
    result.requiredReserveBytes = std::max( policy.minimumFreeReserveBytes, percentReserve );
    result.requiredAvailableBytes = SaturatingAdd(
        inventory.estimatedTotalBuildBytes, result.requiredReserveBytes );
    if( volumeAvailableBytes < result.requiredAvailableBytes )
    {
        result.reason = "insufficient_disk";
        return false;
    }
    result.accepted = true;
    result.reason = "accepted";
    return true;
}

}

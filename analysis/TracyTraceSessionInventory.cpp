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
};

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

    InventoryVisitorState visitor { &inventory, &options, inventory.sourceFileSize };
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

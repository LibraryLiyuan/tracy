#include "TracyNeutralAggregateStore.hpp"

#include "TracyAnalysisIoPath.hpp"
#include "TracyHash.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <thread>

#ifdef _WIN32
#  include <Windows.h>
#endif

namespace tracy::analysis
{
namespace
{

using nlohmann::json;

constexpr uint64_t NeutralManifestMagic = 0x314e414d47414e4aull; // JNAGMAN1
constexpr uint64_t NeutralRunMagic = 0x314e555247414e4aull; // JNAGRUN1
constexpr uint64_t NeutralCheckpointMagic = 0x3154504347414e4aull; // JNAGCPT1

#pragma pack( push, 1 )
struct NeutralRunHeader
{
    uint64_t magic = NeutralRunMagic;
    uint32_t storeSchema = NeutralAggregateStoreSchemaVersion;
    uint32_t aggregateSchema = NeutralAggregateSchemaVersion;
    uint64_t runId = 0;
    uint64_t recordCount = 0;
    uint64_t payloadBytes = 0;
};
#pragma pack( pop )

std::string SafeLine( std::string value )
{
    for( auto& ch : value ) if( ch == '\r' || ch == '\n' ) ch = ' ';
    return value;
}

bool IsHexDigest( const std::string& value )
{
    return value.size() == 64 && std::all_of( value.begin(), value.end(), []( const unsigned char ch ) {
        return std::isxdigit( ch ) != 0;
    } );
}

bool IsSafeComponent( const std::string& value )
{
    if( value.empty() || value == "." || value == ".." ) return false;
    return std::all_of( value.begin(), value.end(), []( const unsigned char ch ) {
        return std::isalnum( ch ) != 0 || ch == '-' || ch == '_' || ch == '.';
    } );
}

bool IsSafeRelativePath( const std::filesystem::path& value )
{
    if( value.empty() || value.is_absolute() || value.has_root_path() ) return false;
    return std::none_of( value.begin(), value.end(), []( const auto& part ) {
        return part.empty() || part == "." || part == "..";
    } );
}

bool IdentityValid( const NeutralAggregateIdentity& identity )
{
    return IsHexDigest( identity.traceStrongId ) &&
        IsHexDigest( identity.queryExecutableSha256 ) &&
        !identity.querySchema.empty() && identity.querySchema.size() <= 64 &&
        IsSafeComponent( identity.querySchema ) &&
        identity.scanAlgorithm == NeutralScanAlgorithmId &&
        identity.aggregateSchema == NeutralAggregateSchemaVersion;
}

bool DomainStatusValid( const std::string& status )
{
    return status == "complete" || status == "absent" || status == "invalid" || status == "unsupported";
}

std::optional<NeutralAggregateState> ParseState( const std::string& value )
{
    if( value == "building" ) return NeutralAggregateState::Building;
    if( value == "cancelled_resumable" ) return NeutralAggregateState::CancelledResumable;
    if( value == "complete" ) return NeutralAggregateState::Complete;
    if( value == "invalid" ) return NeutralAggregateState::Invalid;
    return std::nullopt;
}

std::string StateValue( NeutralAggregateState state )
{
    switch( state )
    {
    case NeutralAggregateState::Building: return "building";
    case NeutralAggregateState::CancelledResumable: return "cancelled_resumable";
    case NeutralAggregateState::Complete: return "complete";
    case NeutralAggregateState::Invalid: return "invalid";
    }
    return "invalid";
}

bool ParseUint64( const json& value, uint64_t& result )
{
    if( !value.is_string() ) return false;
    const auto& text = value.get_ref<const std::string&>();
    if( text.empty() || !std::all_of( text.begin(), text.end(), []( const unsigned char ch ) { return std::isdigit( ch ) != 0; } ) ) return false;
    try
    {
        size_t consumed = 0;
        result = std::stoull( text, &consumed );
        return consumed == text.size();
    }
    catch( const std::exception& )
    {
        return false;
    }
}

bool SameIdentity( const NeutralAggregateIdentity& left, const NeutralAggregateIdentity& right )
{
    return left.traceStrongId == right.traceStrongId &&
        left.queryExecutableSha256 == right.queryExecutableSha256 &&
        left.querySchema == right.querySchema &&
        left.scanAlgorithm == right.scanAlgorithm &&
        left.aggregateSchema == right.aggregateSchema;
}

void HashField( Sha256Builder& hash, const std::string& value )
{
    const uint64_t size = value.size();
    hash.Update( &size, sizeof( size ) );
    hash.Update( value.data(), value.size() );
}

bool ReplaceFileAtomically( const std::filesystem::path& temporary,
    const std::filesystem::path& target, std::string& error )
{
#ifdef _WIN32
    const auto ioTemporary = AnalysisIoPath( temporary );
    const auto ioTarget = AnalysisIoPath( target );
    constexpr auto RetryLimit = std::chrono::milliseconds( 1500 );
    const auto deadline = std::chrono::steady_clock::now() + RetryLimit;
    auto delay = std::chrono::milliseconds( 5 );
    DWORD lastError = ERROR_SUCCESS;
    for( ;; )
    {
        const auto targetExists = GetFileAttributesW( ioTarget.c_str() ) != INVALID_FILE_ATTRIBUTES;
        const auto replaced = targetExists
            ? ReplaceFileW( ioTarget.c_str(), ioTemporary.c_str(), nullptr,
                REPLACEFILE_WRITE_THROUGH | REPLACEFILE_IGNORE_ACL_ERRORS, nullptr, nullptr ) != FALSE
            : MoveFileExW( ioTemporary.c_str(), ioTarget.c_str(), MOVEFILE_WRITE_THROUGH ) != FALSE;
        if( replaced ) return true;
        lastError = GetLastError();
        const auto retryable = lastError == ERROR_ACCESS_DENIED ||
            lastError == ERROR_SHARING_VIOLATION || lastError == ERROR_LOCK_VIOLATION ||
            lastError == ERROR_ALREADY_EXISTS || lastError == ERROR_FILE_EXISTS ||
            lastError == ERROR_UNABLE_TO_REMOVE_REPLACED;
        const auto now = std::chrono::steady_clock::now();
        if( !retryable || now >= deadline ) break;
        std::this_thread::sleep_for( std::min( delay,
            std::chrono::duration_cast<std::chrono::milliseconds>( deadline - now ) ) );
        delay = std::min( delay * 2, std::chrono::milliseconds( 100 ) );
    }
    error = "neutral_atomic_replace_failed:" + std::to_string( lastError ) + ":" +
        target.filename().string();
#else
    std::error_code ec;
    std::filesystem::rename( AnalysisIoPath( temporary ), AnalysisIoPath( target ), ec );
    if( !ec ) return true;
    error = "neutral_atomic_replace_failed:" + ec.message();
#endif
    return false;
}

bool EnsureParent( const std::filesystem::path& path, std::string& error )
{
    std::error_code ec;
    std::filesystem::create_directories( AnalysisIoPath( path.parent_path() ), ec );
    if( !ec ) return true;
    error = "neutral_directory_create_failed:" + ec.message();
    return false;
}

std::string PathKey( const std::filesystem::path& path )
{
    std::error_code ec;
    auto absolute = std::filesystem::weakly_canonical( AnalysisIoPath( path ), ec );
    if( ec )
    {
        ec.clear();
        absolute = std::filesystem::absolute( AnalysisIoPath( path ), ec ).lexically_normal();
    }
    auto key = absolute.generic_string();
#ifdef _WIN32
    // weakly_canonical() may strip the Win32 extended-length namespace from
    // an existing prefix while retaining it for a not-yet-created suffix.
    // Normalize both spellings before comparing containment; otherwise an
    // ordinary child can be misclassified as an escape solely because one
    // key is `c:/...` and the other is `//?/c:/...`.
    if( key.rfind( "//?/unc/", 0 ) == 0 ) key = "//" + key.substr( 8 );
    else if( key.rfind( "//?/", 0 ) == 0 ) key.erase( 0, 4 );
    std::transform( key.begin(), key.end(), key.begin(), []( const unsigned char ch ) {
        return char( std::tolower( ch ) );
    } );
#endif
    return key;
}

bool IsContainedPath( const std::filesystem::path& root, const std::filesystem::path& child )
{
    const auto rootKey = PathKey( root );
    const auto childKey = PathKey( child );
    if( rootKey.empty() || childKey.size() <= rootKey.size() ||
        childKey.compare( 0, rootKey.size(), rootKey ) != 0 ) return false;
    return childKey[rootKey.size()] == '/';
}

uint64_t DirectoryBytes( const std::filesystem::path& root )
{
    uint64_t result = 0;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator iterator(
        AnalysisIoPath( root ), std::filesystem::directory_options::skip_permission_denied, ec );
    const std::filesystem::recursive_directory_iterator end;
    while( !ec && iterator != end )
    {
        if( iterator->is_regular_file( ec ) )
        {
            const auto bytes = iterator->file_size( ec );
            if( !ec ) result = bytes > std::numeric_limits<uint64_t>::max() - result
                ? std::numeric_limits<uint64_t>::max() : result + bytes;
        }
        ec.clear();
        iterator.increment( ec );
    }
    return result;
}

bool ReadRunHeader( std::ifstream& input, const NeutralAggregateRun& run,
    uint64_t fileBytes, NeutralRunHeader& header, std::string& error )
{
    input.read( reinterpret_cast<char*>( &header ), sizeof( header ) );
    if( !input || header.magic != NeutralRunMagic ||
        header.storeSchema != NeutralAggregateStoreSchemaVersion ||
        header.aggregateSchema != NeutralAggregateSchemaVersion ||
        header.runId != run.runId || header.recordCount != run.recordCount ||
        fileBytes < sizeof( header ) || header.payloadBytes != fileBytes - sizeof( header ) ||
        header.payloadBytes > uint64_t( std::numeric_limits<size_t>::max() ) ||
        header.payloadBytes > uint64_t( std::numeric_limits<std::streamsize>::max() ) )
    {
        error = "neutral_run_header_mismatch";
        return false;
    }
    return true;
}

}

std::string ComputeNeutralAggregateIdentity( const NeutralAggregateIdentity& identity )
{
    if( !IdentityValid( identity ) ) return {};
    Sha256Builder hash;
    HashField( hash, "JNTracyNeutralAggregateIdentity" );
    HashField( hash, identity.traceStrongId );
    HashField( hash, identity.queryExecutableSha256 );
    HashField( hash, identity.querySchema );
    HashField( hash, identity.scanAlgorithm );
    hash.Update( &identity.aggregateSchema, sizeof( identity.aggregateSchema ) );
    return hash.FinalHex();
}

std::filesystem::path NeutralAggregateCachePath( const std::filesystem::path& cacheRoot,
    const NeutralAggregateIdentity& identity )
{
    if( cacheRoot.empty() || !IdentityValid( identity ) ) return {};
    return cacheRoot / identity.traceStrongId / identity.scanAlgorithm;
}

bool AcquireNeutralAggregateWriterLease( const std::filesystem::path& storeRoot,
    NeutralAggregateWriterLease& lease, std::string& error )
{
    return AcquireAnalysisWriterLease( storeRoot, lease, error );
}

bool SaveNeutralAggregateManifest( const std::filesystem::path& storeRoot,
    const NeutralAggregateManifest& value, std::string& error )
{
    error.clear();
    if( !IdentityValid( value.identity ) || value.storeSchema != NeutralAggregateStoreSchemaVersion ||
        value.aggregateIdentity != ComputeNeutralAggregateIdentity( value.identity ) ||
        value.generation.empty() || value.completed != ( value.state == NeutralAggregateState::Complete ) ||
        value.qualityComplete != value.completed )
    {
        error = "neutral_manifest_invalid";
        return false;
    }
    for( const auto& run : value.runs )
    {
        if( run.runId == 0 || run.domain.empty() || run.kind.empty() ||
            !IsSafeRelativePath( run.relativePath ) || run.sha256.size() != 64 )
        {
            error = "neutral_manifest_invalid_run";
            return false;
        }
    }
    for( const auto& domain : value.domains )
    {
        if( domain.domain.empty() || !DomainStatusValid( domain.status ) || !IsHexDigest( domain.checksum ) )
        {
            error = "neutral_manifest_invalid_domain";
            return false;
        }
    }
    const auto target = storeRoot / "aggregate-manifest.json";
    if( !EnsureParent( target, error ) ) return false;
    auto temporary = target;
    temporary += ".tmp";
    std::ofstream output( AnalysisIoPath( temporary ), std::ios::binary | std::ios::trunc );
    if( !output ) { error = "neutral_manifest_open_failed"; return false; }
    json document = {
        { "schema_version", value.storeSchema },
        { "native_scan_schema", NativeScanApiSchemaVersion },
        { "trace_identity", value.identity.traceStrongId },
        { "aggregate_identity", value.aggregateIdentity },
        { "algorithm_identity", value.identity.scanAlgorithm },
        { "state", StateValue( value.state ) },
        { "domains", json::array() },
        { "quality", {
            { "complete", value.qualityComplete },
            { "unreported_gap_count", value.unreportedGapCount },
            { "findings", json::array() }
        } },
        { "storage", {
            { "magic", std::to_string( NeutralManifestMagic ) },
            { "query_executable_sha256", value.identity.queryExecutableSha256 },
            { "query_schema", value.identity.querySchema },
            { "aggregate_schema", value.identity.aggregateSchema },
            { "generation", value.generation },
            { "last_access_unix_ns", std::to_string( value.lastAccessUnixNs ) },
            { "pinned", value.pinned },
            { "report_reference_count", value.reportReferenceCount },
            { "completed", value.completed },
            { "reason", SafeLine( value.reason ) },
            { "runs", json::array() }
        } }
    };
    for( const auto& domain : value.domains )
    {
        json item = {
            { "domain", domain.domain }, { "present", domain.present }, { "status", domain.status },
            { "input_count", std::to_string( domain.inputCount ) },
            { "output_count", std::to_string( domain.outputCount ) }, { "checksum", domain.checksum }
        };
        item["unavailable_reason"] = domain.unavailableReason.empty() ? json( nullptr ) : json( domain.unavailableReason );
        document["domains"].push_back( std::move( item ) );
    }
    for( const auto& finding : value.qualityFindings )
        document["quality"]["findings"].push_back( { { "message", SafeLine( finding ) } } );
    for( const auto& run : value.runs )
    {
        document["storage"]["runs"].push_back( {
            { "run_id", std::to_string( run.runId ) }, { "domain", run.domain }, { "kind", run.kind },
            { "path", run.relativePath.generic_string() }, { "record_count", std::to_string( run.recordCount ) },
            { "file_bytes", std::to_string( run.fileBytes ) }, { "sha256", run.sha256 }
        } );
    }
    output << document.dump( 2 ) << '\n';
    output.flush();
    if( !output ) { error = "neutral_manifest_write_failed"; return false; }
    output.close();
    return ReplaceFileAtomically( temporary, target, error );
}

std::optional<NeutralAggregateManifest> LoadNeutralAggregateManifest(
    const std::filesystem::path& storeRoot, std::string& error )
{
    error.clear();
    std::ifstream input( AnalysisIoPath( storeRoot / "aggregate-manifest.json" ), std::ios::binary );
    if( !input ) { error = "neutral_manifest_missing"; return std::nullopt; }
    NeutralAggregateManifest result;
    try
    {
        const auto document = json::parse( input );
        if( !document.is_object() ) throw std::runtime_error( "root_not_object" );
        result.storeSchema = document.at( "schema_version" ).get<uint32_t>();
        if( document.at( "native_scan_schema" ).get<uint32_t>() != NativeScanApiSchemaVersion )
            throw std::runtime_error( "native_scan_schema" );
        result.identity.traceStrongId = document.at( "trace_identity" ).get<std::string>();
        result.aggregateIdentity = document.at( "aggregate_identity" ).get<std::string>();
        result.identity.scanAlgorithm = document.at( "algorithm_identity" ).get<std::string>();
        const auto parsedState = ParseState( document.at( "state" ).get<std::string>() );
        if( !parsedState ) throw std::runtime_error( "state" );
        result.state = *parsedState;

        const auto& quality = document.at( "quality" );
        result.qualityComplete = quality.at( "complete" ).get<bool>();
        result.unreportedGapCount = quality.at( "unreported_gap_count" ).get<uint64_t>();
        for( const auto& item : quality.value( "findings", json::array() ) )
        {
            if( item.is_object() && item.contains( "message" ) )
                result.qualityFindings.push_back( item.at( "message" ).get<std::string>() );
        }

        for( const auto& item : document.at( "domains" ) )
        {
            NeutralAggregateDomain domain;
            domain.domain = item.at( "domain" ).get<std::string>();
            domain.present = item.at( "present" ).get<bool>();
            domain.status = item.at( "status" ).get<std::string>();
            if( !ParseUint64( item.at( "input_count" ), domain.inputCount ) ||
                !ParseUint64( item.at( "output_count" ), domain.outputCount ) )
                throw std::runtime_error( "domain_count" );
            domain.checksum = item.at( "checksum" ).get<std::string>();
            if( item.contains( "unavailable_reason" ) && !item.at( "unavailable_reason" ).is_null() )
                domain.unavailableReason = item.at( "unavailable_reason" ).get<std::string>();
            result.domains.push_back( std::move( domain ) );
        }

        const auto& storage = document.at( "storage" );
        if( storage.at( "magic" ).get<std::string>() != std::to_string( NeutralManifestMagic ) )
            throw std::runtime_error( "magic" );
        result.identity.queryExecutableSha256 = storage.at( "query_executable_sha256" ).get<std::string>();
        result.identity.querySchema = storage.at( "query_schema" ).get<std::string>();
        result.identity.aggregateSchema = storage.at( "aggregate_schema" ).get<uint32_t>();
        result.generation = storage.at( "generation" ).get<std::string>();
        if( !ParseUint64( storage.at( "last_access_unix_ns" ), result.lastAccessUnixNs ) )
            throw std::runtime_error( "last_access" );
        result.pinned = storage.at( "pinned" ).get<bool>();
        result.reportReferenceCount = storage.at( "report_reference_count" ).get<uint32_t>();
        result.completed = storage.at( "completed" ).get<bool>();
        result.reason = storage.at( "reason" ).get<std::string>();
        for( const auto& item : storage.at( "runs" ) )
        {
            NeutralAggregateRun run;
            if( !ParseUint64( item.at( "run_id" ), run.runId ) ||
                !ParseUint64( item.at( "record_count" ), run.recordCount ) ||
                !ParseUint64( item.at( "file_bytes" ), run.fileBytes ) )
                throw std::runtime_error( "run_number" );
            run.domain = item.at( "domain" ).get<std::string>();
            run.kind = item.at( "kind" ).get<std::string>();
            run.relativePath = std::filesystem::path( item.at( "path" ).get<std::string>() );
            run.sha256 = item.at( "sha256" ).get<std::string>();
            result.runs.push_back( std::move( run ) );
        }
    }
    catch( const std::exception& exception )
    {
        error = "neutral_manifest_parse_failed:" + std::string( exception.what() );
        return std::nullopt;
    }
    if( result.storeSchema != NeutralAggregateStoreSchemaVersion ||
        !IdentityValid( result.identity ) || result.aggregateIdentity != ComputeNeutralAggregateIdentity( result.identity ) )
    {
        error = "unsupported_or_invalid_neutral_aggregate_schema";
        return std::nullopt;
    }
    if( result.generation.empty() || result.completed != ( result.state == NeutralAggregateState::Complete ) ||
        result.qualityComplete != result.completed )
    {
        error = "neutral_manifest_inconsistent";
        return std::nullopt;
    }
    for( const auto& run : result.runs )
    {
        if( run.runId == 0 || run.domain.empty() || run.kind.empty() ||
            !IsSafeRelativePath( run.relativePath ) || run.sha256.size() != 64 )
        {
            error = "neutral_manifest_invalid_run";
            return std::nullopt;
        }
    }
    for( const auto& domain : result.domains )
    {
        if( domain.domain.empty() || !DomainStatusValid( domain.status ) || !IsHexDigest( domain.checksum ) )
        {
            error = "neutral_manifest_invalid_domain";
            return std::nullopt;
        }
    }
    return result;
}

bool CanReuseNeutralAggregate( const NeutralAggregateManifest& manifest,
    const NeutralAggregateIdentity& identity )
{
    return manifest.storeSchema == NeutralAggregateStoreSchemaVersion &&
        manifest.state == NeutralAggregateState::Complete && manifest.completed &&
        SameIdentity( manifest.identity, identity ) &&
        manifest.aggregateIdentity == ComputeNeutralAggregateIdentity( identity );
}

bool WriteNeutralAggregateRun( const std::filesystem::path& storeRoot,
    NeutralAggregateRun& run, const void* payload, size_t payloadBytes, std::string& error )
{
    error.clear();
    if( run.runId == 0 || run.domain.empty() || run.kind.empty() ||
        !IsSafeRelativePath( run.relativePath ) || ( payloadBytes != 0 && payload == nullptr ) )
    {
        error = "neutral_run_invalid_argument";
        return false;
    }
    const auto target = storeRoot / run.relativePath;
    if( !IsContainedPath( storeRoot, target ) ) { error = "neutral_run_path_escape"; return false; }
    if( !EnsureParent( target, error ) ) return false;
    auto temporary = target;
    temporary += ".tmp";
    NeutralRunHeader header;
    header.runId = run.runId;
    header.recordCount = run.recordCount;
    header.payloadBytes = payloadBytes;
    std::ofstream output( AnalysisIoPath( temporary ), std::ios::binary | std::ios::trunc );
    if( !output ) { error = "neutral_run_open_failed"; return false; }
    output.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
    if( payloadBytes != 0 ) output.write( static_cast<const char*>( payload ), std::streamsize( payloadBytes ) );
    output.flush();
    if( !output ) { error = "neutral_run_write_failed"; return false; }
    output.close();
    run.fileBytes = sizeof( header ) + payloadBytes;
    try { run.sha256 = Sha256File( temporary ); }
    catch( const std::exception& exception )
    {
        error = "neutral_run_hash_failed:" + std::string( exception.what() );
        return false;
    }
    return ReplaceFileAtomically( temporary, target, error );
}

bool ReadNeutralAggregateRun( const std::filesystem::path& storeRoot,
    const NeutralAggregateRun& run, std::vector<uint8_t>& payload, std::string& error )
{
    error.clear();
    payload.clear();
    if( !IsSafeRelativePath( run.relativePath ) ) { error = "neutral_run_path_escape"; return false; }
    const auto path = storeRoot / run.relativePath;
    if( !IsContainedPath( storeRoot, path ) ) { error = "neutral_run_path_escape"; return false; }
    std::error_code ec;
    const auto bytes = std::filesystem::file_size( AnalysisIoPath( path ), ec );
    if( ec || bytes != run.fileBytes || bytes < sizeof( NeutralRunHeader ) )
    {
        error = "neutral_run_size_mismatch";
        return false;
    }
    try
    {
        if( Sha256File( path ) != run.sha256 ) { error = "neutral_run_sha256_mismatch"; return false; }
    }
    catch( const std::exception& exception )
    {
        error = "neutral_run_hash_failed:" + std::string( exception.what() );
        return false;
    }
    std::ifstream input( AnalysisIoPath( path ), std::ios::binary );
    NeutralRunHeader header;
    if( !ReadRunHeader( input, run, bytes, header, error ) ) return false;
    payload.resize( size_t( header.payloadBytes ) );
    if( !payload.empty() ) input.read( reinterpret_cast<char*>( payload.data() ), std::streamsize( payload.size() ) );
    if( !input && !payload.empty() )
    {
        payload.clear();
        error = "neutral_run_payload_read_failed";
        return false;
    }
    return true;
}

bool SaveNeutralAggregateCheckpoint( const std::filesystem::path& storeRoot,
    const NeutralAggregateCheckpoint& value, std::string& error )
{
    error.clear();
    if( value.storeSchema != NeutralAggregateStoreSchemaVersion || value.generation.empty() ||
        value.stage.empty() || ( value.progressDenominator != 0 && value.progressNumerator > value.progressDenominator ) )
    {
        error = "neutral_checkpoint_invalid";
        return false;
    }
    const auto target = storeRoot / "build-state" / "checkpoint";
    if( !EnsureParent( target, error ) ) return false;
    auto temporary = target;
    temporary += ".tmp";
    std::ofstream output( AnalysisIoPath( temporary ), std::ios::binary | std::ios::trunc );
    if( !output ) { error = "neutral_checkpoint_open_failed"; return false; }
    output << "magic " << NeutralCheckpointMagic << '\n'
        << "store_schema " << value.storeSchema << '\n'
        << "generation " << std::quoted( value.generation ) << '\n'
        << "stage " << std::quoted( value.stage ) << '\n'
        << "source_cursor " << std::quoted( value.sourceCursor ) << '\n'
        << "source_events " << value.sourceEvents << '\n'
        << "output_runs " << value.outputRuns << '\n'
        << "memory_bytes " << value.memoryBytes << '\n'
        << "disk_bytes " << value.diskBytes << '\n'
        << "progress_numerator " << value.progressNumerator << '\n'
        << "progress_denominator " << value.progressDenominator << '\n'
        << "resumable " << value.resumable << '\n';
    output.flush();
    if( !output ) { error = "neutral_checkpoint_write_failed"; return false; }
    output.close();
    return ReplaceFileAtomically( temporary, target, error );
}

std::optional<NeutralAggregateCheckpoint> LoadNeutralAggregateCheckpoint(
    const std::filesystem::path& storeRoot, std::string& error )
{
    error.clear();
    std::ifstream input( AnalysisIoPath( storeRoot / "build-state" / "checkpoint" ), std::ios::binary );
    if( !input ) { error = "neutral_checkpoint_missing"; return std::nullopt; }
    NeutralAggregateCheckpoint result;
    uint64_t magic = 0;
    std::string key;
    while( input >> key )
    {
        if( key == "magic" ) input >> magic;
        else if( key == "store_schema" ) input >> result.storeSchema;
        else if( key == "generation" ) input >> std::quoted( result.generation );
        else if( key == "stage" ) input >> std::quoted( result.stage );
        else if( key == "source_cursor" ) input >> std::quoted( result.sourceCursor );
        else if( key == "source_events" ) input >> result.sourceEvents;
        else if( key == "output_runs" ) input >> result.outputRuns;
        else if( key == "memory_bytes" ) input >> result.memoryBytes;
        else if( key == "disk_bytes" ) input >> result.diskBytes;
        else if( key == "progress_numerator" ) input >> result.progressNumerator;
        else if( key == "progress_denominator" ) input >> result.progressDenominator;
        else if( key == "resumable" ) input >> result.resumable;
        else { std::string ignored; std::getline( input, ignored ); }
        if( !input ) { error = "neutral_checkpoint_parse_failed"; return std::nullopt; }
    }
    if( magic != NeutralCheckpointMagic ) { error = "neutral_checkpoint_magic_mismatch"; return std::nullopt; }
    if( result.storeSchema != NeutralAggregateStoreSchemaVersion || result.generation.empty() ||
        result.stage.empty() || ( result.progressDenominator != 0 && result.progressNumerator > result.progressDenominator ) )
    {
        error = "neutral_checkpoint_invalid";
        return std::nullopt;
    }
    return result;
}

bool VerifyNeutralAggregate( const std::filesystem::path& storeRoot,
    const NeutralAggregateManifest& manifest, std::string& error )
{
    error.clear();
    if( !CanReuseNeutralAggregate( manifest, manifest.identity ) )
    {
        error = "neutral_aggregate_not_complete";
        return false;
    }
    std::set<uint64_t> runIds;
    std::set<std::string> runPaths;
    for( const auto& run : manifest.runs )
    {
        if( !runIds.emplace( run.runId ).second ||
            !runPaths.emplace( run.relativePath.generic_string() ).second )
        {
            error = "neutral_run_duplicate";
            return false;
        }
        if( !IsSafeRelativePath( run.relativePath ) ) { error = "neutral_run_path_escape"; return false; }
        const auto path = storeRoot / run.relativePath;
        if( !IsContainedPath( storeRoot, path ) ) { error = "neutral_run_path_escape"; return false; }
        std::error_code ec;
        const auto bytes = std::filesystem::file_size( AnalysisIoPath( path ), ec );
        if( ec || bytes != run.fileBytes ) { error = "neutral_run_size_mismatch"; return false; }
        try
        {
            if( Sha256File( path ) != run.sha256 ) { error = "neutral_run_sha256_mismatch"; return false; }
        }
        catch( const std::exception& exception )
        {
            error = "neutral_run_hash_failed:" + std::string( exception.what() );
            return false;
        }
        std::ifstream input( AnalysisIoPath( path ), std::ios::binary );
        NeutralRunHeader header;
        if( !ReadRunHeader( input, run, bytes, header, error ) ) return false;
    }
    return true;
}

bool PruneNeutralAggregateCache( const std::filesystem::path& cacheRoot,
    uint64_t maximumBytes, const std::vector<std::filesystem::path>& activeStoreRoots,
    NeutralAggregatePruneResult& result, std::string& error )
{
    error.clear();
    result = {};
    if( cacheRoot.empty() ) { error = "neutral_cache_root_missing"; return false; }
    std::set<std::string> active;
    for( const auto& path : activeStoreRoots ) active.emplace( PathKey( path ) );

    struct Entry
    {
        std::filesystem::path root;
        uint64_t bytes = 0;
        uint64_t lastAccess = 0;
        bool protectedEntry = true;
    };
    std::vector<Entry> entries;
    std::error_code ec;
    std::filesystem::directory_iterator traces( AnalysisIoPath( cacheRoot ),
        std::filesystem::directory_options::skip_permission_denied, ec );
    const std::filesystem::directory_iterator end;
    while( !ec && traces != end )
    {
        if( traces->is_directory( ec ) )
        {
            std::filesystem::directory_iterator algorithms( traces->path(),
                std::filesystem::directory_options::skip_permission_denied, ec );
            while( !ec && algorithms != end )
            {
                if( algorithms->is_directory( ec ) )
                {
                    const auto root = algorithms->path();
                    std::string loadError;
                    const auto manifest = LoadNeutralAggregateManifest( root, loadError );
                    Entry entry;
                    entry.root = root;
                    entry.bytes = DirectoryBytes( root );
                    entry.protectedEntry = active.find( PathKey( root ) ) != active.end() ||
                        !manifest || manifest->state != NeutralAggregateState::Complete ||
                        !manifest->completed || manifest->pinned || manifest->reportReferenceCount != 0;
                    entry.lastAccess = manifest ? manifest->lastAccessUnixNs : 0;
                    entries.push_back( std::move( entry ) );
                }
                ec.clear();
                algorithms.increment( ec );
            }
        }
        ec.clear();
        traces.increment( ec );
    }
    if( ec ) { error = "neutral_cache_enumeration_failed:" + ec.message(); return false; }

    for( const auto& entry : entries )
        result.bytesBefore = entry.bytes > std::numeric_limits<uint64_t>::max() - result.bytesBefore
            ? std::numeric_limits<uint64_t>::max() : result.bytesBefore + entry.bytes;
    result.bytesAfter = result.bytesBefore;

    std::vector<const Entry*> eligible;
    for( const auto& entry : entries ) if( !entry.protectedEntry ) eligible.push_back( &entry );
    std::sort( eligible.begin(), eligible.end(), []( const Entry* left, const Entry* right ) {
        if( left->lastAccess != right->lastAccess ) return left->lastAccess < right->lastAccess;
        return PathKey( left->root ) < PathKey( right->root );
    } );
    for( const auto* entry : eligible )
    {
        if( result.bytesAfter <= maximumBytes ) break;
        if( !IsContainedPath( cacheRoot, entry->root ) )
        {
            error = "neutral_cache_prune_path_escape";
            return false;
        }
        std::error_code removeError;
        std::filesystem::remove_all( AnalysisIoPath( entry->root ), removeError );
        if( removeError ) { error = "neutral_cache_prune_failed:" + removeError.message(); return false; }
        result.removedEntries++;
        result.removedBytes += entry->bytes;
        result.bytesAfter = entry->bytes > result.bytesAfter ? 0 : result.bytesAfter - entry->bytes;
    }
    result.limitBlockedByProtectedEntries = result.bytesAfter > maximumBytes;
    return true;
}

const char* NeutralAggregateStateName( NeutralAggregateState state )
{
    switch( state )
    {
    case NeutralAggregateState::Building: return "Building";
    case NeutralAggregateState::CancelledResumable: return "CancelledResumable";
    case NeutralAggregateState::Complete: return "Complete";
    case NeutralAggregateState::Invalid: return "Invalid";
    }
    return "Unknown";
}

}

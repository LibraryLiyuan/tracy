#include "TracyTraceSessionStore.hpp"

#include "TracyHash.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

#ifdef _WIN32
#  include <Windows.h>
#endif

namespace tracy::analysis
{
namespace
{

constexpr uint64_t SessionShardMagic = 0x314448534e4aull; // JNSHD1
constexpr uint64_t SessionManifestMagic = 0x314e414d534e4aull; // JNSMAN1

#pragma pack( push, 1 )
struct SessionShardHeader
{
    uint64_t magic = SessionShardMagic;
    uint32_t storeSchema = TraceSessionStoreSchemaVersion;
    uint32_t canonicalSchema = TraceSessionCanonicalSchemaVersion;
    uint64_t shardId = 0;
    int64_t timeBeginNs = 0;
    int64_t timeEndNs = 0;
    uint64_t sourceRecordBegin = 0;
    uint64_t sourceRecordEnd = 0;
    uint64_t recordCount = 0;
    uint64_t payloadBytes = 0;
};
#pragma pack( pop )

std::string SafeLine( std::string value )
{
    for( auto& ch : value ) if( ch == '\r' || ch == '\n' ) ch = ' ';
    return value;
}

bool IsSafeRelativePath( const std::filesystem::path& value )
{
    if( value.empty() || value.is_absolute() || value.has_root_path() ) return false;
    return std::none_of( value.begin(), value.end(), []( const auto& part ) { return part == ".."; } );
}

std::string SafeDomain( const std::string& value )
{
    std::string result;
    result.reserve( value.size() );
    for( const auto ch : value )
    {
        if( ( ch >= 'a' && ch <= 'z' ) || ( ch >= 'A' && ch <= 'Z' ) ||
            ( ch >= '0' && ch <= '9' ) || ch == '-' || ch == '_' ) result.push_back( ch );
    }
    return result;
}

bool ReplaceFileAtomically( const std::filesystem::path& temporary,
    const std::filesystem::path& target, std::string& error )
{
#ifdef _WIN32
    const auto targetExists = GetFileAttributesW( target.c_str() ) != INVALID_FILE_ATTRIBUTES;
    const auto replaced = targetExists
        ? ReplaceFileW( target.c_str(), temporary.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr ) != FALSE
        : MoveFileExW( temporary.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH ) != FALSE;
    if( replaced ) return true;
    error = "session_atomic_replace_failed:" + std::to_string( GetLastError() );
#else
    std::error_code ec;
    std::filesystem::rename( temporary, target, ec );
    if( !ec ) return true;
    error = "session_atomic_replace_failed:" + ec.message();
#endif
    return false;
}

bool RenameDirectoryAtomically( const std::filesystem::path& source,
    const std::filesystem::path& target, std::string& error )
{
#ifdef _WIN32
    if( MoveFileExW( source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH ) ) return true;
    error = "session_publish_rename_failed:" + std::to_string( GetLastError() );
#else
    std::error_code ec;
    std::filesystem::rename( source, target, ec );
    if( !ec ) return true;
    error = "session_publish_rename_failed:" + ec.message();
#endif
    return false;
}

bool WriteCurrent( const std::filesystem::path& root, const std::string& generation, std::string& error )
{
    const auto target = root / "CURRENT";
    auto temporary = target;
    temporary += ".tmp";
    std::ofstream output( temporary, std::ios::binary | std::ios::trunc );
    if( !output ) { error = "session_current_open_failed"; return false; }
    output << generation << '\n';
    output.flush();
    if( !output ) { error = "session_current_write_failed"; return false; }
    output.close();
    return ReplaceFileAtomically( temporary, target, error );
}

bool PublishedState( TraceSessionState state )
{
    return state == TraceSessionState::Complete || state == TraceSessionState::CompleteSourceDegraded;
}

bool SameSource( const TraceSessionSourceIdentity& left, const TraceSessionSourceIdentity& right )
{
    return left.sha256 == right.sha256 && left.fileSize == right.fileSize &&
        left.committedRevision == right.committedRevision && left.protocol == right.protocol &&
        left.captureIdentity == right.captureIdentity;
}

std::optional<std::string> ReadCurrentGeneration( const std::filesystem::path& root )
{
    std::ifstream input( root / "CURRENT", std::ios::binary );
    std::string generation;
    if( !input || !std::getline( input, generation ) || generation.empty() ) return std::nullopt;
    if( !generation.empty() && generation.back() == '\r' ) generation.pop_back();
    return generation.empty() ? std::nullopt : std::optional<std::string>( std::move( generation ) );
}

}

std::filesystem::path DefaultTraceSessionPath( const std::filesystem::path& streamPath )
{
    auto filename = streamPath.filename().string();
    constexpr std::string_view suffix = ".tracy-stream";
    if( filename.size() >= suffix.size() && filename.compare( filename.size() - suffix.size(), suffix.size(), suffix ) == 0 )
        filename.resize( filename.size() - suffix.size() );
    else
        filename = streamPath.stem().string();
    filename += TraceSessionSuffix;
    return streamPath.parent_path() / filename;
}

std::filesystem::path BuildingTraceSessionPath( const std::filesystem::path& finalPath, const std::string& generation )
{
    auto result = finalPath;
    result += ".building." + generation;
    return result;
}

bool SaveTraceSessionManifest( const std::filesystem::path& root,
    const TraceSessionManifest& value, std::string& error )
{
    error.clear();
    std::error_code ec;
    std::filesystem::create_directories( root, ec );
    if( ec ) { error = "session_manifest_directory_failed:" + ec.message(); return false; }
    const auto target = root / "manifest";
    auto temporary = target;
    temporary += ".tmp";
    std::ofstream output( temporary, std::ios::binary | std::ios::trunc );
    if( !output ) { error = "session_manifest_open_failed"; return false; }
    output << "magic " << SessionManifestMagic << '\n';
    output << "store_schema " << value.storeSchema << '\n';
    output << "canonical_schema " << value.canonicalSchema << '\n';
    output << "derived_schema " << value.derivedSchema << '\n';
    output << "session_id " << std::quoted( value.sessionId ) << '\n';
    output << "generation " << std::quoted( value.generation ) << '\n';
    output << "state " << unsigned( value.state ) << '\n';
    output << "source_sha256 " << std::quoted( value.source.sha256 ) << '\n';
    output << "source_size " << value.source.fileSize << '\n';
    output << "source_revision " << value.source.committedRevision << '\n';
    output << "source_protocol " << value.source.protocol << '\n';
    output << "capture_identity " << std::quoted( value.source.captureIdentity ) << '\n';
    output << "capture_end_state " << std::quoted( value.source.captureEndState ) << '\n';
    output << "converter_sha256 " << std::quoted( value.source.converterSha256 ) << '\n';
    output << "configuration_hash " << std::quoted( value.source.configurationHash ) << '\n';
    output << "mandatory_derived_complete " << value.mandatoryDerivedComplete << '\n';
    output << "audit_complete " << value.auditComplete << '\n';
    output << "reason " << std::quoted( SafeLine( value.reason ) ) << '\n';
    output << "shard_count " << value.shards.size() << '\n';
    for( const auto& shard : value.shards )
    {
        if( !IsSafeRelativePath( shard.relativePath ) ) { error = "session_manifest_unsafe_shard_path"; return false; }
        output << "shard " << shard.shardId << ' ' << std::quoted( shard.domain ) << ' '
            << shard.timeBeginNs << ' ' << shard.timeEndNs << ' '
            << shard.sourceRecordBegin << ' ' << shard.sourceRecordEnd << ' '
            << shard.recordCount << ' ' << shard.uncompressedBytes << ' ' << shard.fileBytes << ' '
            << std::quoted( shard.codec ) << ' ' << std::quoted( shard.sha256 ) << ' '
            << std::quoted( shard.relativePath.generic_string() ) << '\n';
    }
    output.flush();
    if( !output ) { error = "session_manifest_write_failed"; return false; }
    output.close();
    return ReplaceFileAtomically( temporary, target, error );
}

std::optional<TraceSessionManifest> LoadTraceSessionManifest( const std::filesystem::path& root, std::string& error )
{
    error.clear();
    auto manifestPath = root / "manifest";
    if( const auto current = ReadCurrentGeneration( root ) )
    {
        const auto generationManifest = root / "generations" / *current / "manifest";
        if( std::filesystem::exists( generationManifest ) ) manifestPath = generationManifest;
    }
    std::ifstream input( manifestPath, std::ios::binary );
    if( !input ) { error = "session_manifest_missing"; return std::nullopt; }
    TraceSessionManifest result;
    uint64_t magic = 0;
    size_t declaredShards = 0;
    std::string key;
    while( input >> key )
    {
        if( key == "magic" ) input >> magic;
        else if( key == "store_schema" ) input >> result.storeSchema;
        else if( key == "canonical_schema" ) input >> result.canonicalSchema;
        else if( key == "derived_schema" ) input >> result.derivedSchema;
        else if( key == "session_id" ) input >> std::quoted( result.sessionId );
        else if( key == "generation" ) input >> std::quoted( result.generation );
        else if( key == "state" ) { unsigned value = 0; input >> value; result.state = TraceSessionState( value ); }
        else if( key == "source_sha256" ) input >> std::quoted( result.source.sha256 );
        else if( key == "source_size" ) input >> result.source.fileSize;
        else if( key == "source_revision" ) input >> result.source.committedRevision;
        else if( key == "source_protocol" ) input >> result.source.protocol;
        else if( key == "capture_identity" ) input >> std::quoted( result.source.captureIdentity );
        else if( key == "capture_end_state" ) input >> std::quoted( result.source.captureEndState );
        else if( key == "converter_sha256" ) input >> std::quoted( result.source.converterSha256 );
        else if( key == "configuration_hash" ) input >> std::quoted( result.source.configurationHash );
        else if( key == "mandatory_derived_complete" ) input >> result.mandatoryDerivedComplete;
        else if( key == "audit_complete" ) input >> result.auditComplete;
        else if( key == "reason" ) input >> std::quoted( result.reason );
        else if( key == "shard_count" ) input >> declaredShards;
        else if( key == "shard" )
        {
            TraceSessionShard shard;
            std::string relative;
            input >> shard.shardId >> std::quoted( shard.domain )
                >> shard.timeBeginNs >> shard.timeEndNs
                >> shard.sourceRecordBegin >> shard.sourceRecordEnd
                >> shard.recordCount >> shard.uncompressedBytes >> shard.fileBytes
                >> std::quoted( shard.codec ) >> std::quoted( shard.sha256 ) >> std::quoted( relative );
            shard.relativePath = std::filesystem::path( relative );
            result.shards.push_back( std::move( shard ) );
        }
        else { std::string ignored; std::getline( input, ignored ); }
        if( !input ) { error = "session_manifest_parse_failed"; return std::nullopt; }
    }
    if( magic != SessionManifestMagic ) { error = "session_manifest_magic_mismatch"; return std::nullopt; }
    if( result.storeSchema != TraceSessionStoreSchemaVersion ||
        result.canonicalSchema != TraceSessionCanonicalSchemaVersion ||
        result.derivedSchema != TraceSessionDerivedSchemaVersion )
    {
        error = "unsupported_trace_session_schema";
        return std::nullopt;
    }
    if( result.shards.size() != declaredShards ) { error = "session_manifest_shard_count_mismatch"; return std::nullopt; }
    for( const auto& shard : result.shards )
    {
        if( !IsSafeRelativePath( shard.relativePath ) ) { error = "session_manifest_unsafe_shard_path"; return std::nullopt; }
    }
    return result;
}

bool WriteTraceSessionShard( const std::filesystem::path& root, const std::string& generation,
    TraceSessionShard& shard, const void* payload, size_t payloadBytes, std::string& error )
{
    error.clear();
    const auto domain = SafeDomain( shard.domain );
    if( domain.empty() || ( payloadBytes != 0 && payload == nullptr ) ) { error = "session_shard_invalid_argument"; return false; }
    std::ostringstream filename;
    filename << domain << '-' << std::setw( 6 ) << std::setfill( '0' ) << shard.shardId << ".bin";
    shard.relativePath = std::filesystem::path( "generations" ) / generation / "canonical" / filename.str();
    const auto target = root / shard.relativePath;
    std::error_code ec;
    std::filesystem::create_directories( target.parent_path(), ec );
    if( ec ) { error = "session_shard_directory_failed:" + ec.message(); return false; }
    auto temporary = target;
    temporary += ".tmp";
    SessionShardHeader header;
    header.shardId = shard.shardId;
    header.timeBeginNs = shard.timeBeginNs;
    header.timeEndNs = shard.timeEndNs;
    header.sourceRecordBegin = shard.sourceRecordBegin;
    header.sourceRecordEnd = shard.sourceRecordEnd;
    header.recordCount = shard.recordCount;
    header.payloadBytes = payloadBytes;
    std::ofstream output( temporary, std::ios::binary | std::ios::trunc );
    if( !output ) { error = "session_shard_open_failed"; return false; }
    output.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
    if( payloadBytes ) output.write( static_cast<const char*>( payload ), std::streamsize( payloadBytes ) );
    output.flush();
    if( !output ) { error = "session_shard_write_failed"; return false; }
    output.close();
    shard.uncompressedBytes = payloadBytes;
    shard.fileBytes = sizeof( header ) + payloadBytes;
    shard.codec = "none";
    shard.sha256 = Sha256File( temporary );
    return ReplaceFileAtomically( temporary, target, error );
}

bool VerifyTraceSession( const std::filesystem::path& root,
    const TraceSessionManifest& manifest, std::string& error )
{
    error.clear();
    if( manifest.storeSchema != TraceSessionStoreSchemaVersion ||
        manifest.canonicalSchema != TraceSessionCanonicalSchemaVersion ||
        manifest.derivedSchema != TraceSessionDerivedSchemaVersion )
    {
        error = "unsupported_trace_session_schema";
        return false;
    }
    for( const auto& shard : manifest.shards )
    {
        if( !IsSafeRelativePath( shard.relativePath ) ) { error = "session_manifest_unsafe_shard_path"; return false; }
        const auto path = root / shard.relativePath;
        std::error_code ec;
        const auto bytes = std::filesystem::file_size( path, ec );
        if( ec || bytes != shard.fileBytes ) { error = "session_shard_size_mismatch"; return false; }
        if( Sha256File( path ) != shard.sha256 ) { error = "session_shard_sha256_mismatch"; return false; }
    }
    return true;
}

bool VerifyTraceSessionSourceIdentity( const std::filesystem::path& sourcePath,
    const TraceSessionSourceIdentity& expected, std::string& error )
{
    error.clear();
    std::error_code ec;
    const auto size = std::filesystem::file_size( sourcePath, ec );
    if( ec ) { error = "source_missing"; return false; }
    if( size != expected.fileSize ) { error = "source_size_mismatch"; return false; }
    if( expected.sha256.size() != 64 ) { error = "source_strong_identity_missing"; return false; }
    if( Sha256File( sourcePath ) != expected.sha256 ) { error = "source_sha256_mismatch"; return false; }
    return true;
}

bool PublishTraceSession( const std::filesystem::path& buildingPath,
    const std::filesystem::path& finalPath, const TraceSessionManifest& manifest, std::string& error )
{
    error.clear();
    if( !PublishedState( manifest.state ) || !manifest.mandatoryDerivedComplete || !manifest.auditComplete )
    {
        error = "session_publish_incomplete";
        return false;
    }
    if( !VerifyTraceSession( buildingPath, manifest, error ) ) return false;
    if( !SaveTraceSessionManifest( buildingPath, manifest, error ) ) return false;
    const auto buildingGeneration = buildingPath / "generations" / manifest.generation;
    if( !SaveTraceSessionManifest( buildingGeneration, manifest, error ) ) return false;
    if( std::filesystem::exists( finalPath ) )
    {
        const auto current = LoadTraceSessionManifest( finalPath, error );
        if( !current ) return false;
        if( !SameSource( current->source, manifest.source ) ) { error = "identity_mismatch"; return false; }
        const auto finalGeneration = finalPath / "generations" / manifest.generation;
        if( std::filesystem::exists( finalGeneration ) ) { error = "session_generation_exists"; return false; }
        if( !RenameDirectoryAtomically( buildingGeneration, finalGeneration, error ) ) return false;
        if( !SaveTraceSessionManifest( finalPath, manifest, error ) ) return false;
        if( !WriteCurrent( finalPath, manifest.generation, error ) ) return false;
        std::error_code ignored;
        std::filesystem::remove_all( buildingPath, ignored );
        return true;
    }
    if( !WriteCurrent( buildingPath, manifest.generation, error ) ) return false;
    return RenameDirectoryAtomically( buildingPath, finalPath, error );
}

bool IsTraceSessionQueryable( const std::filesystem::path& root, std::string& error )
{
    error.clear();
    if( root.filename().string().find( ".building." ) != std::string::npos )
    {
        error = "session_not_published";
        return false;
    }
    const auto manifest = LoadTraceSessionManifest( root, error );
    if( !manifest ) return false;
    if( !PublishedState( manifest->state ) || !manifest->mandatoryDerivedComplete || !manifest->auditComplete )
    {
        error = "session_not_complete";
        return false;
    }
    std::ifstream current( root / "CURRENT", std::ios::binary );
    std::string generation;
    if( !current || !std::getline( current, generation ) || generation != manifest->generation )
    {
        error = "session_current_generation_mismatch";
        return false;
    }
    return VerifyTraceSession( root, *manifest, error );
}

const char* TraceSessionStateName( TraceSessionState state )
{
    switch( state )
    {
    case TraceSessionState::InventoryBuilding: return "InventoryBuilding";
    case TraceSessionState::InventoryFailed: return "InventoryFailed";
    case TraceSessionState::CapacityPreflight: return "CapacityPreflight";
    case TraceSessionState::CanonicalBuilding: return "CanonicalBuilding";
    case TraceSessionState::CanonicalPaused: return "CanonicalPaused";
    case TraceSessionState::CanonicalFailed: return "CanonicalFailed";
    case TraceSessionState::DerivedBuilding: return "DerivedBuilding";
    case TraceSessionState::DerivedFailed: return "DerivedFailed";
    case TraceSessionState::FinalAuditing: return "FinalAuditing";
    case TraceSessionState::CancelledResumable: return "CancelledResumable";
    case TraceSessionState::Complete: return "Complete";
    case TraceSessionState::CompleteSourceDegraded: return "CompleteSourceDegraded";
    case TraceSessionState::InvalidSource: return "InvalidSource";
    case TraceSessionState::InvalidConverterOutput: return "InvalidConverterOutput";
    case TraceSessionState::InvalidChecksum: return "InvalidChecksum";
    case TraceSessionState::InvalidCoreGap: return "InvalidCoreGap";
    case TraceSessionState::InvalidCapacity: return "InvalidCapacity";
    case TraceSessionState::InsufficientDisk: return "InsufficientDisk";
    case TraceSessionState::IdentityMismatch: return "IdentityMismatch";
    }
    return "Unknown";
}

}

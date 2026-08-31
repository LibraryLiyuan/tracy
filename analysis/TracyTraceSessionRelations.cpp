#include "TracyTraceSessionRelations.hpp"

#include "TracyHash.hpp"
#include "TracyTraceSessionCanonical.hpp"
#include "TracyTraceSessionGpuCanonical.hpp"
#include "TracyQueue.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
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

constexpr uint64_t RelationFileMagic = 0x314c4552534e4aull;     // JNSREL1
constexpr uint64_t RelationManifestMagic = 0x31464d52534e4aull; // JNSRMF1
constexpr const char* RelationFileName = "relations.bin";

#pragma pack( push, 1 )
struct RelationFileHeader
{
    uint64_t magic = RelationFileMagic;
    uint32_t schema = TraceSessionRelationIndexSchemaVersion;
    uint32_t endian = 0x01020304;
    uint64_t sourceSize = 0;
    uint64_t relationCount = 0;
    uint32_t generationBytes = 0;
    uint32_t reserved = 0;
};

struct StoredRelation
{
    int64_t timeNs = 0;
    uint64_t sourceId = 0;
    uint64_t targetId = 0;
    uint32_t thread = 0;
    uint8_t sourceKind = 0;
    uint8_t targetKind = 0;
    uint8_t relationNamespace = 0;
    uint8_t relation = 0;
    uint8_t flags = 0;
    uint8_t reserved[3] {};
};
#pragma pack( pop )

static_assert( sizeof( StoredRelation ) == 36 );

struct RelationManifest
{
    std::string sourceSha256;
    uint64_t sourceSize = 0;
    std::string generation;
    uint64_t fileBytes = 0;
    std::string fileSha256;
    TraceSessionRelationStats stats;
};

bool AtomicReplace( const std::filesystem::path& source,
    const std::filesystem::path& target, std::string& error )
{
#ifdef _WIN32
    if( MoveFileExW( source.c_str(), target.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) ) return true;
    error = "session_relation_atomic_replace_failed:" + std::to_string( GetLastError() );
    return false;
#else
    std::error_code ec;
    std::filesystem::rename( source, target, ec );
    if( !ec ) return true;
    error = "session_relation_atomic_replace_failed:" + ec.message();
    return false;
#endif
}

bool DecodeItem( const TraceSessionCanonicalRecord& record, QueueItem& item,
    std::string& error )
{
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ||
        record.type >= uint8_t( QueueType::NUM_TYPES ) ||
        record.payload.size() < QueueDataSize[record.type] )
    { error = "session_relation_protocol_record_invalid"; return false; }
    item = {};
    std::memcpy( &item, record.payload.data(),
        std::min<size_t>( record.payload.size(), sizeof( item ) ) );
    if( item.hdr.idx != record.type )
    { error = "session_relation_protocol_type_mismatch"; return false; }
    return true;
}

std::string MakeRef( const std::string& fingerprint, const char* kind, uint64_t id )
{
    std::ostringstream out;
    out << "tracy:v1:" << fingerprint.substr( 0, 16 ) << ':' << kind << ':' << std::hex << id;
    return out.str();
}

bool SaveManifest( const std::filesystem::path& root,
    const RelationManifest& manifest, std::string& error )
{
    const auto temporary = root / "manifest.tmp";
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_relation_manifest_open_failed"; return false; }
    out << "magic " << RelationManifestMagic << '\n';
    out << "schema " << TraceSessionRelationIndexSchemaVersion << '\n';
    out << "source_sha256 " << std::quoted( manifest.sourceSha256 ) << '\n';
    out << "source_size " << manifest.sourceSize << '\n';
    out << "generation " << std::quoted( manifest.generation ) << '\n';
    out << "file_bytes " << manifest.fileBytes << '\n';
    out << "file_sha256 " << std::quoted( manifest.fileSha256 ) << '\n';
    out << "relations " << manifest.stats.relations << '\n';
    out.flush();
    if( !out ) { error = "session_relation_manifest_write_failed"; return false; }
    out.close();
    return AtomicReplace( temporary, root / "manifest", error );
}

bool LoadManifest( const std::filesystem::path& root,
    RelationManifest& manifest, std::string& error )
{
    manifest = {};
    std::ifstream in( root / "manifest", std::ios::binary );
    if( !in ) { error = "session_relation_manifest_not_found"; return false; }
    uint64_t magic = 0;
    uint32_t schema = 0;
    std::string key;
    while( in >> key )
    {
        if( key == "magic" ) in >> magic;
        else if( key == "schema" ) in >> schema;
        else if( key == "source_sha256" ) in >> std::quoted( manifest.sourceSha256 );
        else if( key == "source_size" ) in >> manifest.sourceSize;
        else if( key == "generation" ) in >> std::quoted( manifest.generation );
        else if( key == "file_bytes" ) in >> manifest.fileBytes;
        else if( key == "file_sha256" ) in >> std::quoted( manifest.fileSha256 );
        else if( key == "relations" ) in >> manifest.stats.relations;
        else { std::string ignored; std::getline( in, ignored ); }
        if( !in ) { error = "session_relation_manifest_parse_failed"; return false; }
    }
    manifest.stats.fileBytes = manifest.fileBytes;
    if( magic != RelationManifestMagic || schema != TraceSessionRelationIndexSchemaVersion ||
        manifest.sourceSha256.size() != 64 || manifest.fileSha256.size() != 64 )
    { error = "session_relation_manifest_invalid"; return false; }
    return true;
}

struct BuildState
{
    std::ofstream* out = nullptr;
    TraceSessionTimeTransform transform;
    uint64_t count = 0;
};

bool VisitRelation( const TraceSessionCanonicalRecord& record, void* userData,
    std::string& error )
{
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ||
        record.type != uint8_t( QueueType::JnRelation ) ) return true;
    auto& state = *static_cast<BuildState*>( userData );
    QueueItem item {};
    if( !DecodeItem( record, item, error ) ) return false;
    if( state.count == std::numeric_limits<uint64_t>::max() )
    { error = "session_relation_count_overflow"; return false; }
    StoredRelation stored;
    stored.timeNs = state.transform.ToNanoseconds( item.jnRelation.time );
    stored.sourceId = item.jnRelation.sourceId;
    stored.targetId = item.jnRelation.targetId;
    stored.thread = record.threadContext;
    stored.sourceKind = item.jnRelation.sourceKind;
    stored.targetKind = item.jnRelation.targetKind;
    stored.relationNamespace = item.jnRelation.relationNamespace;
    stored.relation = item.jnRelation.relation;
    stored.flags = item.jnRelation.flags;
    state.out->write( reinterpret_cast<const char*>( &stored ), sizeof( stored ) );
    if( !*state.out ) { error = "session_relation_file_write_failed"; return false; }
    ++state.count;
    return true;
}

bool ValidateFileHeader( std::ifstream& in, const TraceSessionManifest& session,
    const RelationManifest& manifest, uint64_t& recordsOffset, std::string& error )
{
    RelationFileHeader header;
    in.read( reinterpret_cast<char*>( &header ), sizeof( header ) );
    if( !in || header.magic != RelationFileMagic ||
        header.schema != TraceSessionRelationIndexSchemaVersion || header.endian != 0x01020304 ||
        header.sourceSize != session.source.fileSize || header.relationCount != manifest.stats.relations ||
        header.generationBytes != session.generation.size() || header.reserved != 0 )
    { error = "session_relation_file_header_invalid"; return false; }
    std::string source( 64, '\0' );
    std::string generation( header.generationBytes, '\0' );
    in.read( source.data(), std::streamsize( source.size() ) );
    if( !generation.empty() ) in.read( generation.data(), std::streamsize( generation.size() ) );
    if( !in || source != session.source.sha256 || generation != session.generation )
    { error = "session_relation_file_identity_mismatch"; return false; }
    recordsOffset = sizeof( header ) + source.size() + generation.size();
    if( header.relationCount > ( std::numeric_limits<uint64_t>::max() - recordsOffset ) /
        sizeof( StoredRelation ) || recordsOffset + header.relationCount * sizeof( StoredRelation ) != manifest.fileBytes )
    { error = "session_relation_file_count_mismatch"; return false; }
    return true;
}

bool VerifyFiles( const std::filesystem::path& root, const TraceSessionManifest& session,
    RelationManifest& manifest, uint64_t& recordsOffset, std::string& error )
{
    if( !LoadManifest( root, manifest, error ) ) return false;
    if( manifest.sourceSha256 != session.source.sha256 ||
        manifest.sourceSize != session.source.fileSize || manifest.generation != session.generation )
    { error = "session_relation_identity_mismatch"; return false; }
    const auto path = root / RelationFileName;
    std::error_code ec;
    const auto bytes = std::filesystem::file_size( path, ec );
    if( ec || bytes != manifest.fileBytes )
    { error = "session_relation_file_size_mismatch"; return false; }
    if( Sha256File( path ) != manifest.fileSha256 )
    { error = "session_relation_file_sha256_mismatch"; return false; }
    std::ifstream in( path, std::ios::binary );
    if( !in ) { error = "session_relation_file_open_failed"; return false; }
    return ValidateFileHeader( in, session, manifest, recordsOffset, error );
}

}

std::filesystem::path TraceSessionRelationIndexRoot( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest )
{
    return sessionRoot / "generations" / manifest.generation / "derived" /
        "relation-index" / "1" / "exact";
}

bool BuildTraceSessionRelationDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& session, TraceSessionRelationStats& stats,
    std::string& error )
{
    error.clear(); stats = {};
    if( session.source.sha256.size() != 64 ||
        session.generation.size() > std::numeric_limits<uint32_t>::max() )
    { error = "session_relation_identity_invalid"; return false; }
    const auto root = TraceSessionRelationIndexRoot( sessionRoot, session );
    std::error_code ec;
    std::filesystem::create_directories( root, ec );
    if( ec ) { error = "session_relation_directory_failed:" + ec.message(); return false; }
    const auto temporary = root / ( std::string( RelationFileName ) + ".tmp" );
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_relation_file_open_failed"; return false; }
    RelationFileHeader header;
    header.sourceSize = session.source.fileSize;
    header.generationBytes = uint32_t( session.generation.size() );
    out.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
    out.write( session.source.sha256.data(), std::streamsize( session.source.sha256.size() ) );
    if( !session.generation.empty() ) out.write( session.generation.data(),
        std::streamsize( session.generation.size() ) );
    BuildState state;
    state.out = &out;
    if( !LoadTraceSessionTimeTransform( sessionRoot, session, state.transform, error ) ||
        !VisitTraceSessionCanonicalOrdered( sessionRoot, session, VisitRelation, &state, error ) ) return false;
    const uint64_t prefixBytes = sizeof( header ) + 64 + session.generation.size();
    if( state.count > ( std::numeric_limits<uint64_t>::max() - prefixBytes ) /
        sizeof( StoredRelation ) )
    { error = "session_relation_file_size_overflow"; return false; }
    header.relationCount = state.count;
    out.seekp( 0, std::ios::beg );
    out.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
    out.flush();
    if( !out ) { error = "session_relation_file_finalize_failed"; return false; }
    out.close();
    const auto target = root / RelationFileName;
    if( !AtomicReplace( temporary, target, error ) ) return false;
    RelationManifest manifest;
    manifest.sourceSha256 = session.source.sha256;
    manifest.sourceSize = session.source.fileSize;
    manifest.generation = session.generation;
    manifest.fileBytes = prefixBytes + state.count * sizeof( StoredRelation );
    manifest.fileSha256 = Sha256File( target );
    manifest.stats.relations = state.count;
    manifest.stats.fileBytes = manifest.fileBytes;
    if( !SaveManifest( root, manifest, error ) ) return false;
    stats = manifest.stats;
    return true;
}

std::shared_ptr<TraceSessionRelationReader> TraceSessionRelationReader::Open(
    const std::filesystem::path& sessionRoot, const TraceSessionManifest& session,
    std::string& error )
{
    error.clear();
    RelationManifest manifest;
    uint64_t recordsOffset = 0;
    const auto root = TraceSessionRelationIndexRoot( sessionRoot, session );
    if( !VerifyFiles( root, session, manifest, recordsOffset, error ) ) return {};
    auto reader = std::make_shared<TraceSessionRelationReader>();
    reader->m_path = root / RelationFileName;
    reader->m_recordsOffset = recordsOffset;
    reader->m_fingerprint = session.source.sha256;
    reader->m_stats = manifest.stats;
    return reader;
}

std::vector<RelationDto> TraceSessionRelationReader::Scan( size_t offset, size_t limit ) const
{
    const auto begin = std::min<uint64_t>( offset, m_stats.relations );
    const auto count = std::min<uint64_t>( limit, m_stats.relations - begin );
    if( count > std::numeric_limits<size_t>::max() ||
        count > uint64_t( std::numeric_limits<std::streamsize>::max() ) / sizeof( StoredRelation ) )
        throw std::runtime_error( "relation page exceeds platform capacity" );
    std::vector<StoredRelation> stored( static_cast<size_t>( count ) );
    std::ifstream in( m_path, std::ios::binary );
    if( !in ) throw std::runtime_error( "relation index is unavailable" );
    in.seekg( std::streamoff( m_recordsOffset + begin * sizeof( StoredRelation ) ), std::ios::beg );
    if( !stored.empty() ) in.read( reinterpret_cast<char*>( stored.data() ),
        std::streamsize( stored.size() * sizeof( StoredRelation ) ) );
    if( !in && !stored.empty() ) throw std::runtime_error( "relation index read failed" );
    std::vector<RelationDto> result;
    result.reserve( stored.size() );
    for( size_t i = 0; i < stored.size(); ++i )
    {
        const auto& value = stored[i];
        result.push_back( { MakeRef( m_fingerprint, "relation", begin + i ),
            value.sourceId, value.targetId, value.timeNs,
            MakeRef( m_fingerprint, "thread", value.thread ), value.sourceKind,
            value.targetKind, value.relationNamespace, value.relation, value.flags } );
    }
    return result;
}

bool AuditTraceSessionRelationDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& session, TraceSessionRelationStats& stats,
    std::string& error )
{
    RelationManifest manifest;
    uint64_t recordsOffset = 0;
    if( !VerifyFiles( TraceSessionRelationIndexRoot( sessionRoot, session ), session,
        manifest, recordsOffset, error ) ) return false;
    stats = manifest.stats;
    return true;
}

}

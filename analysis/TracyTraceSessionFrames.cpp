#include "TracyTraceSessionFrames.hpp"

#include "TracyHash.hpp"
#include "TracyTraceSessionCanonical.hpp"
#include "TracyTraceSessionGpuCanonical.hpp"
#include "TracyProtocol.hpp"
#include "TracyQueue.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_map>

#ifdef _WIN32
#  include <Windows.h>
#endif

namespace tracy::analysis
{
namespace
{

constexpr uint64_t FrameFileMagic = 0x314d5246534e4aull; // JNSFRM1
constexpr uint64_t FrameManifestMagic = 0x31464d46534e4aull; // JNSFMF1
constexpr const char* FrameFileName = "frames.bin";

void Put32( std::vector<uint8_t>& out, uint32_t value )
{
    for( int i = 0; i < 4; ++i ) out.push_back( uint8_t( value >> ( i * 8 ) ) );
}

void Put64( std::vector<uint8_t>& out, uint64_t value )
{
    for( int i = 0; i < 8; ++i ) out.push_back( uint8_t( value >> ( i * 8 ) ) );
}

bool Get32( const std::vector<uint8_t>& in, size_t& offset, uint32_t& value )
{
    if( offset > in.size() || in.size() - offset < 4 ) return false;
    value = 0;
    for( int i = 0; i < 4; ++i ) value |= uint32_t( in[offset + i] ) << ( i * 8 );
    offset += 4;
    return true;
}

bool Get64( const std::vector<uint8_t>& in, size_t& offset, uint64_t& value )
{
    if( offset > in.size() || in.size() - offset < 8 ) return false;
    value = 0;
    for( int i = 0; i < 8; ++i ) value |= uint64_t( in[offset + i] ) << ( i * 8 );
    offset += 8;
    return true;
}

bool AtomicReplace( const std::filesystem::path& source,
    const std::filesystem::path& target, std::string& error )
{
#ifdef _WIN32
    if( MoveFileExW( source.c_str(), target.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) ) return true;
    error = "session_frame_atomic_replace_failed:" + std::to_string( GetLastError() );
    return false;
#else
    std::error_code ec;
    std::filesystem::rename( source, target, ec );
    if( !ec ) return true;
    error = "session_frame_atomic_replace_failed:" + ec.message();
    return false;
#endif
}

struct FrameManifest
{
    std::string sourceSha256;
    uint64_t sourceSize = 0;
    std::string generation;
    uint64_t fileBytes = 0;
    std::string fileSha256;
    TraceSessionFrameStats stats;
};

bool SaveFrameManifest( const std::filesystem::path& root,
    const FrameManifest& manifest, std::string& error )
{
    const auto temporary = root / "manifest.tmp";
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_frame_manifest_open_failed"; return false; }
    out << "magic " << FrameManifestMagic << '\n';
    out << "schema " << TraceSessionFrameIndexSchemaVersion << '\n';
    out << "source_sha256 " << std::quoted( manifest.sourceSha256 ) << '\n';
    out << "source_size " << manifest.sourceSize << '\n';
    out << "generation " << std::quoted( manifest.generation ) << '\n';
    out << "file_bytes " << manifest.fileBytes << '\n';
    out << "file_sha256 " << std::quoted( manifest.fileSha256 ) << '\n';
    out << "frame_sets " << manifest.stats.frameSets << '\n';
    out << "frames " << manifest.stats.frames << '\n';
    out << "complete_frames " << manifest.stats.completeFrames << '\n';
    out.flush();
    if( !out ) { error = "session_frame_manifest_write_failed"; return false; }
    out.close();
    return AtomicReplace( temporary, root / "manifest", error );
}

bool LoadFrameManifest( const std::filesystem::path& root,
    FrameManifest& manifest, std::string& error )
{
    manifest = {};
    std::ifstream in( root / "manifest", std::ios::binary );
    if( !in ) { error = "session_frame_manifest_not_found"; return false; }
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
        else if( key == "frame_sets" ) in >> manifest.stats.frameSets;
        else if( key == "frames" ) in >> manifest.stats.frames;
        else if( key == "complete_frames" ) in >> manifest.stats.completeFrames;
        else { std::string ignored; std::getline( in, ignored ); }
        if( !in ) { error = "session_frame_manifest_parse_failed"; return false; }
    }
    manifest.stats.fileBytes = manifest.fileBytes;
    if( magic != FrameManifestMagic || schema != TraceSessionFrameIndexSchemaVersion ||
        manifest.sourceSha256.size() != 64 || manifest.fileSha256.size() != 64 )
    {
        error = "session_frame_manifest_invalid";
        return false;
    }
    return true;
}

bool DecodeItem( const TraceSessionCanonicalRecord& record, QueueItem& item,
    std::string& error )
{
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ||
        record.type >= uint8_t( QueueType::NUM_TYPES ) ||
        record.payload.size() < QueueDataSize[record.type] )
    {
        error = "session_frame_protocol_record_invalid";
        return false;
    }
    item = {};
    std::memcpy( &item, record.payload.data(),
        std::min<size_t>( record.payload.size(), sizeof( item ) ) );
    if( item.hdr.idx != record.type )
    {
        error = "session_frame_protocol_type_mismatch";
        return false;
    }
    return true;
}

struct FrameKey
{
    uint64_t value = 0;
    bool vsync = false;
    bool operator==( const FrameKey& ) const = default;
};

struct FrameKeyHash
{
    size_t operator()( const FrameKey& value ) const
    {
        return std::hash<uint64_t>()( value.value ) ^ ( value.vsync ? size_t( 0x9e3779b9 ) : 0 );
    }
};

struct BuildSet
{
    FrameKey key;
    bool continuous = false;
    std::vector<std::pair<int64_t, int64_t>> rawFrames;
};

struct BuildState
{
    std::unordered_map<uint64_t, std::string> names;
    std::unordered_map<FrameKey, size_t, FrameKeyHash> byKey;
    std::vector<BuildSet> sets;
    std::string error;
};

bool VisitFrameName( const TraceSessionCanonicalRecord& record,
    void* userData, std::string& error )
{
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ||
        record.type != uint8_t( QueueType::FrameName ) ) return true;
    QueueItem item {};
    if( !DecodeItem( record, item, error ) ) return false;
    const auto fixed = size_t( QueueDataSize[record.type] );
    if( record.payload.size() < fixed + sizeof( uint16_t ) )
    { error = "session_frame_name_payload_truncated"; return false; }
    uint16_t size = 0;
    std::memcpy( &size, record.payload.data() + fixed, sizeof( size ) );
    if( size != record.variablePayloadBytes ||
        record.payload.size() != fixed + sizeof( uint16_t ) + size )
    { error = "session_frame_name_payload_mismatch"; return false; }
    auto& state = *static_cast<BuildState*>( userData );
    const std::string value( reinterpret_cast<const char*>( record.payload.data() + fixed + sizeof( uint16_t ) ), size );
    const auto [found, inserted] = state.names.emplace( item.stringTransfer.ptr, value );
    if( !inserted && found->second != value )
    { error = "session_frame_name_conflict"; return false; }
    return true;
}

size_t GetOrCreateSet( BuildState& state, FrameKey key, bool continuous,
    std::string& error )
{
    const auto found = state.byKey.find( key );
    if( found != state.byKey.end() )
    {
        if( state.sets[found->second].continuous != continuous )
        {
            error = "session_frame_set_mode_conflict";
            return std::numeric_limits<size_t>::max();
        }
        return found->second;
    }
    const auto index = state.sets.size();
    state.sets.push_back( { key, continuous, {} } );
    state.byKey.emplace( key, index );
    return index;
}

bool VisitFrameEvent( const TraceSessionCanonicalRecord& record,
    void* userData, std::string& error )
{
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ) return true;
    const auto type = QueueType( record.type );
    if( type != QueueType::FrameMarkMsg && type != QueueType::FrameMarkMsgStart &&
        type != QueueType::FrameMarkMsgEnd && type != QueueType::FrameVsync ) return true;
    QueueItem item {};
    if( !DecodeItem( record, item, error ) ) return false;
    auto& state = *static_cast<BuildState*>( userData );
    const bool vsync = type == QueueType::FrameVsync;
    const bool continuous = vsync || type == QueueType::FrameMarkMsg;
    const uint64_t value = vsync ? item.frameVsync.id : item.frameMark.name;
    const int64_t time = vsync ? item.frameVsync.time : item.frameMark.time;
    const auto setIndex = GetOrCreateSet( state, { value, vsync }, continuous, error );
    if( setIndex == std::numeric_limits<size_t>::max() ) return false;
    auto& set = state.sets[setIndex];
    if( continuous || type == QueueType::FrameMarkMsgStart )
    {
        if( !continuous && !set.rawFrames.empty() && set.rawFrames.back().second < 0 )
        { error = "session_frame_start_before_previous_end"; return false; }
        set.rawFrames.emplace_back( time, -1 );
        return true;
    }
    if( set.rawFrames.empty() || set.rawFrames.back().second >= 0 )
    { error = "session_frame_end_without_start"; return false; }
    if( time < set.rawFrames.back().first )
    { error = "session_frame_end_before_start"; return false; }
    set.rawFrames.back().second = time;
    return true;
}

bool WriteFrameFile( const std::filesystem::path& root,
    const TraceSessionManifest& session, const BuildState& state,
    const TraceSessionTimeTransform& transform, bool semanticTimePresent,
    int64_t lastSemanticTimeRaw, FrameManifest& manifest, std::string& error )
{
    std::vector<uint8_t> bytes;
    uint64_t frameCount = 0;
    for( const auto& set : state.sets )
    {
        if( set.rawFrames.size() > std::numeric_limits<uint64_t>::max() - frameCount )
        { error = "session_frame_count_overflow"; return false; }
        frameCount += set.rawFrames.size();
    }
    if( session.source.sha256.size() != 64 || session.generation.size() > std::numeric_limits<uint32_t>::max() )
    { error = "session_frame_identity_invalid"; return false; }
    uint64_t reserveBytes = 128;
    const auto checkedAdd = [&reserveBytes]( uint64_t count, uint64_t stride ) {
        if( count != 0 && stride > ( std::numeric_limits<uint64_t>::max() - reserveBytes ) / count ) return false;
        reserveBytes += count * stride;
        return true;
    };
    if( !checkedAdd( session.generation.size(), 1 ) ||
        !checkedAdd( state.sets.size(), 48 ) || !checkedAdd( frameCount, 24 ) ||
        reserveBytes > std::numeric_limits<size_t>::max() ||
        reserveBytes > bytes.max_size() )
    { error = "session_frame_file_size_overflow"; return false; }
    bytes.reserve( size_t( reserveBytes ) );
    Put64( bytes, FrameFileMagic );
    Put32( bytes, TraceSessionFrameIndexSchemaVersion );
    Put32( bytes, 0 );
    Put64( bytes, session.source.fileSize );
    Put64( bytes, state.sets.size() );
    Put64( bytes, frameCount );
    Put32( bytes, uint32_t( session.generation.size() ) );
    Put32( bytes, 0 );
    bytes.insert( bytes.end(), session.source.sha256.begin(), session.source.sha256.end() );
    bytes.insert( bytes.end(), session.generation.begin(), session.generation.end() );
    manifest.stats = {};
    manifest.stats.frameSets = state.sets.size();
    for( const auto& set : state.sets )
    {
        std::string name;
        if( set.key.vsync ) name = "Vsync " + std::to_string( uint32_t( set.key.value ) );
        else if( set.key.value == 0 ) name = "Frames";
        else if( const auto found = state.names.find( set.key.value ); found != state.names.end() ) name = found->second;
        else name = "Unavailable frame set " + std::to_string( set.key.value );
        if( name.size() > std::numeric_limits<uint32_t>::max() )
        { error = "session_frame_name_too_large"; return false; }
        Put64( bytes, set.key.value );
        Put32( bytes, set.key.vsync ? 1u : 0u );
        Put32( bytes, set.continuous ? 1u : 0u );
        Put64( bytes, set.rawFrames.size() );
        Put32( bytes, uint32_t( name.size() ) );
        Put32( bytes, 0 );
        bytes.insert( bytes.end(), name.begin(), name.end() );
        for( size_t i = 0; i < set.rawFrames.size(); ++i )
        {
            const auto begin = transform.ToNanoseconds( set.rawFrames[i].first );
            bool complete = false;
            int64_t end = begin;
            if( set.continuous )
            {
                const auto rawEnd = i + 1 < set.rawFrames.size() ? set.rawFrames[i + 1].first :
                    ( semanticTimePresent ? lastSemanticTimeRaw : set.rawFrames[i].first );
                end = transform.ToNanoseconds( rawEnd );
                complete = semanticTimePresent && rawEnd >= set.rawFrames[i].first;
            }
            else if( set.rawFrames[i].second >= 0 )
            {
                end = transform.ToNanoseconds( set.rawFrames[i].second );
                complete = true;
            }
            if( end < begin ) { error = "session_frame_time_order_invalid"; return false; }
            Put64( bytes, uint64_t( begin ) );
            Put64( bytes, uint64_t( end ) );
            Put32( bytes, complete ? 1u : 0u );
            Put32( bytes, 0 );
            manifest.stats.frames++;
            if( complete ) manifest.stats.completeFrames++;
        }
    }
    std::error_code ec;
    std::filesystem::create_directories( root, ec );
    if( ec ) { error = "session_frame_directory_failed:" + ec.message(); return false; }
    auto temporary = root / FrameFileName;
    temporary += ".tmp";
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_frame_file_open_failed"; return false; }
    if( !bytes.empty() ) out.write( reinterpret_cast<const char*>( bytes.data() ), std::streamsize( bytes.size() ) );
    out.flush();
    if( !out ) { error = "session_frame_file_write_failed"; return false; }
    out.close();
    const auto target = root / FrameFileName;
    if( !AtomicReplace( temporary, target, error ) ) return false;
    manifest.sourceSha256 = session.source.sha256;
    manifest.sourceSize = session.source.fileSize;
    manifest.generation = session.generation;
    manifest.fileBytes = bytes.size();
    manifest.fileSha256 = Sha256File( target );
    manifest.stats.fileBytes = manifest.fileBytes;
    return SaveFrameManifest( root, manifest, error );
}

}

std::filesystem::path TraceSessionFrameIndexRoot( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest )
{
    return sessionRoot / "generations" / manifest.generation / "derived" /
        "frame-index" / "1" / "exact";
}

bool BuildTraceSessionFrameDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, bool semanticTimePresent,
    int64_t lastSemanticTimeRaw, TraceSessionFrameStats& stats, std::string& error )
{
    error.clear(); stats = {};
    TraceSessionTimeTransform transform;
    if( !LoadTraceSessionTimeTransform( sessionRoot, manifest, transform, error ) ) return false;
    BuildState state;
    for( const auto& shard : manifest.shards )
    {
        if( shard.domain == "checkpoint" ) continue;
        if( !VisitTraceSessionCanonicalShard( sessionRoot, shard, VisitFrameName, &state, error ) ) return false;
    }
    for( const auto& shard : manifest.shards )
    {
        if( shard.domain == "checkpoint" ) continue;
        if( !VisitTraceSessionCanonicalShard( sessionRoot, shard, VisitFrameEvent, &state, error ) ) return false;
    }
    FrameManifest frameManifest;
    if( !WriteFrameFile( TraceSessionFrameIndexRoot( sessionRoot, manifest ), manifest,
        state, transform, semanticTimePresent, lastSemanticTimeRaw, frameManifest, error ) ) return false;
    stats = frameManifest.stats;
    return true;
}

std::shared_ptr<TraceSessionFrameReader> TraceSessionFrameReader::Open(
    const std::filesystem::path& sessionRoot, const TraceSessionManifest& session,
    std::string& error )
{
    error.clear();
    const auto root = TraceSessionFrameIndexRoot( sessionRoot, session );
    FrameManifest manifest;
    if( !LoadFrameManifest( root, manifest, error ) ) return {};
    if( manifest.sourceSha256 != session.source.sha256 ||
        manifest.sourceSize != session.source.fileSize || manifest.generation != session.generation )
    { error = "session_frame_identity_mismatch"; return {}; }
    const auto path = root / FrameFileName;
    std::error_code ec;
    const auto fileBytes = std::filesystem::file_size( path, ec );
    if( ec || fileBytes != manifest.fileBytes )
    { error = "session_frame_file_size_mismatch"; return {}; }
    if( Sha256File( path ) != manifest.fileSha256 )
    { error = "session_frame_file_sha256_mismatch"; return {}; }
    if( fileBytes > std::numeric_limits<size_t>::max() ||
        fileBytes > uint64_t( std::numeric_limits<std::streamsize>::max() ) )
    { error = "session_frame_file_platform_limit"; return {}; }
    std::vector<uint8_t> bytes( static_cast<size_t>( fileBytes ), uint8_t{} );
    std::ifstream in( path, std::ios::binary );
    if( !in ) { error = "session_frame_file_open_failed"; return {}; }
    if( !bytes.empty() ) in.read( reinterpret_cast<char*>( bytes.data() ), std::streamsize( bytes.size() ) );
    if( !in && !bytes.empty() ) { error = "session_frame_file_read_failed"; return {}; }
    size_t offset = 0;
    uint64_t magic = 0, sourceSize = 0, setCount = 0, frameCount = 0;
    uint32_t schema = 0, reserved0 = 0, reserved1 = 0, generationBytes = 0;
    if( !Get64( bytes, offset, magic ) || !Get32( bytes, offset, schema ) ||
        !Get32( bytes, offset, reserved0 ) || !Get64( bytes, offset, sourceSize ) ||
        !Get64( bytes, offset, setCount ) || !Get64( bytes, offset, frameCount ) ||
        !Get32( bytes, offset, generationBytes ) || !Get32( bytes, offset, reserved1 ) ||
        magic != FrameFileMagic || schema != TraceSessionFrameIndexSchemaVersion ||
        reserved0 != 0 || reserved1 != 0 ||
        sourceSize != session.source.fileSize || offset > bytes.size() ||
        64 > bytes.size() - offset || generationBytes > bytes.size() - offset - 64 )
    { error = "session_frame_file_header_invalid"; return {}; }
    const std::string sourceSha( reinterpret_cast<const char*>( bytes.data() + offset ), 64 ); offset += 64;
    const std::string generation( reinterpret_cast<const char*>( bytes.data() + offset ), generationBytes ); offset += generationBytes;
    if( sourceSha != session.source.sha256 || generation != session.generation ||
        setCount > std::numeric_limits<size_t>::max() || frameCount > std::numeric_limits<size_t>::max() )
    { error = "session_frame_file_identity_invalid"; return {}; }
    auto reader = std::shared_ptr<TraceSessionFrameReader>( new TraceSessionFrameReader );
    reader->m_sets.reserve( size_t( setCount ) );
    uint64_t parsedFrames = 0;
    uint64_t completeFrames = 0;
    for( uint64_t setIndex = 0; setIndex < setCount; ++setIndex )
    {
        uint64_t key = 0, count = 0;
        uint32_t vsync = 0, continuous = 0, nameBytes = 0, reserved = 0;
        if( !Get64( bytes, offset, key ) || !Get32( bytes, offset, vsync ) ||
            !Get32( bytes, offset, continuous ) || !Get64( bytes, offset, count ) ||
            !Get32( bytes, offset, nameBytes ) || !Get32( bytes, offset, reserved ) ||
            vsync > 1 || continuous > 1 || reserved != 0 ||
            nameBytes > bytes.size() - offset || count > std::numeric_limits<size_t>::max() )
        { error = "session_frame_set_header_invalid"; return {}; }
        TraceSessionFrameSetRecord set;
        set.name.assign( reinterpret_cast<const char*>( bytes.data() + offset ), nameBytes );
        set.continuous = continuous != 0;
        offset += nameBytes;
        if( count > std::numeric_limits<uint64_t>::max() - parsedFrames )
        { error = "session_frame_parsed_count_overflow"; return {}; }
        set.frames.reserve( size_t( count ) );
        for( uint64_t frameIndex = 0; frameIndex < count; ++frameIndex )
        {
            uint64_t begin = 0, end = 0;
            uint32_t complete = 0;
            if( !Get64( bytes, offset, begin ) || !Get64( bytes, offset, end ) ||
                !Get32( bytes, offset, complete ) || !Get32( bytes, offset, reserved ) ||
                complete > 1 || reserved != 0 || ( complete != 0 && int64_t( end ) < int64_t( begin ) ) )
            { error = "session_frame_record_invalid"; return {}; }
            set.frames.push_back( { int64_t( begin ), int64_t( end ), complete != 0 } );
            parsedFrames++;
            if( complete ) completeFrames++;
        }
        reader->m_sets.emplace_back( std::move( set ) );
    }
    if( offset != bytes.size() || parsedFrames != frameCount ||
        manifest.stats.frameSets != setCount || manifest.stats.frames != frameCount ||
        manifest.stats.completeFrames != completeFrames )
    { error = "session_frame_file_count_mismatch"; return {}; }
    reader->m_stats = manifest.stats;
    return reader;
}

}

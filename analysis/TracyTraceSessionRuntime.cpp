#include "TracyTraceSessionRuntime.hpp"

#include "TracyHash.hpp"
#include "TracyTraceSessionCanonical.hpp"
#include "TracyTraceSessionGpuCanonical.hpp"
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

constexpr uint64_t RuntimeFileMagic = 0x31545253534e4aull;     // JNSSRT1
constexpr uint64_t RuntimeManifestMagic = 0x31464d54534e4aull; // JNSTMF1
constexpr const char* RuntimeFileName = "runtime-script.bin";

#pragma pack( push, 1 )
struct RuntimeFileHeader
{
    uint64_t magic = RuntimeFileMagic;
    uint32_t schema = TraceSessionRuntimeIndexSchemaVersion;
    uint32_t endian = 0x01020304;
    uint64_t sourceSize = 0;
    uint64_t domainStates = 0;
    uint64_t scriptFrames = 0;
    uint64_t scriptStacks = 0;
    uint64_t stringBytes = 0;
    uint64_t domainOffset = 0;
    uint64_t frameOffset = 0;
    uint64_t stackOffset = 0;
    uint64_t stringOffset = 0;
    uint32_t generationBytes = 0;
    uint32_t reserved = 0;
};

struct StoredDomainState
{
    int64_t timeNs = 0;
    uint64_t generation = 0;
    uint64_t requestedFrame = 0;
    uint32_t thread = 0;
    uint8_t domain = 0;
    uint8_t requestedMode = 0;
    uint8_t effectiveMode = 0;
    uint8_t reason = 0;
    uint8_t flags = 0;
    uint8_t reserved[3] {};
};

struct StoredScriptFrame
{
    uint64_t functionOffset = 0;
    uint64_t fileOffset = 0;
    uint32_t functionBytes = 0;
    uint32_t fileBytes = 0;
    uint32_t frameId = 0;
    uint32_t line = 0;
    uint32_t thread = 0;
    uint8_t runtime = 0;
    uint8_t flags = 0;
    uint8_t reserved[2] {};
};

struct StoredScriptStack
{
    int64_t timeNs = 0;
    uint64_t primaryId = 0;
    uint64_t secondaryId = 0;
    uint64_t textOffset = 0;
    uint32_t value = 0;
    uint32_t thread = 0;
    uint32_t textBytes = 0;
    uint8_t runtime = 0;
    uint8_t flags = 0;
    uint8_t kind = 0;
    uint8_t reserved = 0;
};
#pragma pack( pop )

static_assert( sizeof( StoredDomainState ) == 36 );
static_assert( sizeof( StoredScriptFrame ) == 40 );
static_assert( sizeof( StoredScriptStack ) == 48 );

struct RuntimeManifest
{
    std::string sourceSha256;
    uint64_t sourceSize = 0;
    std::string generation;
    uint64_t fileBytes = 0;
    std::string fileSha256;
    TraceSessionRuntimeStats stats;
};

bool AtomicReplace( const std::filesystem::path& source,
    const std::filesystem::path& target, std::string& error )
{
#ifdef _WIN32
    if( MoveFileExW( source.c_str(), target.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) ) return true;
    error = "session_runtime_atomic_replace_failed:" + std::to_string( GetLastError() );
    return false;
#else
    std::error_code ec;
    std::filesystem::rename( source, target, ec );
    if( !ec ) return true;
    error = "session_runtime_atomic_replace_failed:" + ec.message();
    return false;
#endif
}

bool DecodeItem( const TraceSessionCanonicalRecord& record, QueueItem& item,
    std::string& error )
{
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ||
        record.type >= uint8_t( QueueType::NUM_TYPES ) ||
        record.payload.size() < QueueDataSize[record.type] )
    { error = "session_runtime_protocol_record_invalid"; return false; }
    item = {};
    std::memcpy( &item, record.payload.data(),
        std::min<size_t>( record.payload.size(), sizeof( item ) ) );
    if( item.hdr.idx != record.type )
    { error = "session_runtime_protocol_type_mismatch"; return false; }
    return true;
}

bool GetShortPayload( const TraceSessionCanonicalRecord& record,
    const uint8_t*& data, size_t& size, std::string& error )
{
    const auto fixed = size_t( QueueDataSize[record.type] );
    if( record.payload.size() < fixed + sizeof( uint16_t ) )
    { error = "session_runtime_string_payload_truncated"; return false; }
    uint16_t bytes = 0;
    std::memcpy( &bytes, record.payload.data() + fixed, sizeof( bytes ) );
    if( bytes != record.variablePayloadBytes ||
        record.payload.size() != fixed + sizeof( bytes ) + bytes )
    { error = "session_runtime_string_payload_mismatch"; return false; }
    data = record.payload.data() + fixed + sizeof( bytes );
    size = bytes;
    return true;
}

bool CopyFile( const std::filesystem::path& path, std::ofstream& out,
    uint64_t& bytes, std::string& error )
{
    std::ifstream in( path, std::ios::binary );
    if( !in ) { error = "session_runtime_work_read_failed"; return false; }
    std::vector<char> buffer( 1024 * 1024 );
    bytes = 0;
    while( in )
    {
        in.read( buffer.data(), std::streamsize( buffer.size() ) );
        const auto count = in.gcount();
        if( count > 0 )
        {
            out.write( buffer.data(), count );
            bytes += uint64_t( count );
        }
    }
    if( !in.eof() || !out ) { error = "session_runtime_work_copy_failed"; return false; }
    return true;
}

bool SaveManifest( const std::filesystem::path& root,
    const RuntimeManifest& manifest, std::string& error )
{
    const auto temporary = root / "manifest.tmp";
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_runtime_manifest_open_failed"; return false; }
    out << "magic " << RuntimeManifestMagic << '\n';
    out << "schema " << TraceSessionRuntimeIndexSchemaVersion << '\n';
    out << "source_sha256 " << std::quoted( manifest.sourceSha256 ) << '\n';
    out << "source_size " << manifest.sourceSize << '\n';
    out << "generation " << std::quoted( manifest.generation ) << '\n';
    out << "file_bytes " << manifest.fileBytes << '\n';
    out << "file_sha256 " << std::quoted( manifest.fileSha256 ) << '\n';
    out << "domain_states " << manifest.stats.domainStates << '\n';
    out << "script_frames " << manifest.stats.scriptFrames << '\n';
    out << "script_stack_events " << manifest.stats.scriptStackEvents << '\n';
    out << "string_bytes " << manifest.stats.stringBytes << '\n';
    out.flush();
    if( !out ) { error = "session_runtime_manifest_write_failed"; return false; }
    out.close();
    return AtomicReplace( temporary, root / "manifest", error );
}

bool LoadManifest( const std::filesystem::path& root,
    RuntimeManifest& manifest, std::string& error )
{
    manifest = {};
    std::ifstream in( root / "manifest", std::ios::binary );
    if( !in ) { error = "session_runtime_manifest_not_found"; return false; }
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
        else if( key == "domain_states" ) in >> manifest.stats.domainStates;
        else if( key == "script_frames" ) in >> manifest.stats.scriptFrames;
        else if( key == "script_stack_events" ) in >> manifest.stats.scriptStackEvents;
        else if( key == "string_bytes" ) in >> manifest.stats.stringBytes;
        else { std::string ignored; std::getline( in, ignored ); }
        if( !in ) { error = "session_runtime_manifest_parse_failed"; return false; }
    }
    manifest.stats.fileBytes = manifest.fileBytes;
    if( magic != RuntimeManifestMagic || schema != TraceSessionRuntimeIndexSchemaVersion ||
        manifest.sourceSha256.size() != 64 || manifest.fileSha256.size() != 64 )
    { error = "session_runtime_manifest_invalid"; return false; }
    return true;
}

std::string MakeRef( const std::string& fingerprint, const char* kind, uint64_t id )
{
    std::ostringstream out;
    out << "tracy:v1:" << fingerprint.substr( 0, 16 ) << ':' << kind << ':' << std::hex << id;
    return out.str();
}

struct BuildState
{
    TraceSessionTimeTransform transform;
    std::ofstream domain;
    std::ofstream frame;
    std::ofstream stack;
    std::ofstream stringsFile;
    std::unordered_map<uint64_t, std::string> strings;
    std::unordered_map<std::string, std::pair<uint64_t, uint32_t>> interned;
    TraceSessionRuntimeStats stats;
};

bool WriteRecord( std::ofstream& out, const void* data, size_t bytes,
    const char* failure, std::string& error )
{
    out.write( static_cast<const char*>( data ), std::streamsize( bytes ) );
    if( out ) return true;
    error = failure;
    return false;
}

bool Intern( BuildState& state, const std::string& value,
    uint64_t& offset, uint32_t& bytes, std::string& error )
{
    if( value.size() > std::numeric_limits<uint32_t>::max() )
    { error = "session_runtime_string_too_large"; return false; }
    const auto found = state.interned.find( value );
    if( found != state.interned.end() )
    { offset = found->second.first; bytes = found->second.second; return true; }
    offset = state.stats.stringBytes;
    bytes = uint32_t( value.size() );
    if( state.stats.stringBytes > std::numeric_limits<uint64_t>::max() - value.size() )
    { error = "session_runtime_string_bytes_overflow"; return false; }
    if( !value.empty() ) state.stringsFile.write( value.data(), std::streamsize( value.size() ) );
    if( !state.stringsFile ) { error = "session_runtime_string_write_failed"; return false; }
    state.stats.stringBytes += value.size();
    state.interned.emplace( value, std::make_pair( offset, bytes ) );
    return true;
}

bool VisitRuntime( const TraceSessionCanonicalRecord& record, void* userData,
    std::string& error )
{
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ) return true;
    auto& state = *static_cast<BuildState*>( userData );
    QueueItem item {};
    if( !DecodeItem( record, item, error ) ) return false;
    switch( QueueType( record.type ) )
    {
    case QueueType::StringData:
    {
        const uint8_t* data = nullptr;
        size_t size = 0;
        if( !GetShortPayload( record, data, size, error ) ) return false;
        const std::string value( reinterpret_cast<const char*>( data ), size );
        const auto [found, inserted] = state.strings.emplace( item.stringTransfer.ptr, value );
        if( !inserted && found->second != value )
        { error = "session_runtime_string_conflict"; return false; }
        break;
    }
    case QueueType::JnRuntimeDomainState:
    {
        StoredDomainState value;
        value.timeNs = state.transform.ToNanoseconds( item.jnRuntimeDomainState.time );
        value.generation = item.jnRuntimeDomainState.generation;
        value.requestedFrame = item.jnRuntimeDomainState.requestedFrame;
        value.thread = record.threadContext;
        value.domain = item.jnRuntimeDomainState.domain;
        value.requestedMode = item.jnRuntimeDomainState.requestedMode;
        value.effectiveMode = item.jnRuntimeDomainState.effectiveMode;
        value.reason = item.jnRuntimeDomainState.reason;
        value.flags = item.jnRuntimeDomainState.flags;
        if( !WriteRecord( state.domain, &value, sizeof( value ),
            "session_runtime_domain_write_failed", error ) ) return false;
        ++state.stats.domainStates;
        break;
    }
    case QueueType::JnScriptFrame:
    {
        const auto function = state.strings.find( item.jnScriptFrame.function );
        const auto file = state.strings.find( item.jnScriptFrame.file );
        if( function == state.strings.end() || file == state.strings.end() )
        { error = "session_runtime_script_frame_string_unresolved"; return false; }
        StoredScriptFrame value;
        if( !Intern( state, function->second, value.functionOffset, value.functionBytes, error ) ||
            !Intern( state, file->second, value.fileOffset, value.fileBytes, error ) ) return false;
        value.frameId = item.jnScriptFrame.frameId;
        value.line = item.jnScriptFrame.line;
        value.thread = record.threadContext;
        value.runtime = item.jnScriptFrame.runtime;
        value.flags = item.jnScriptFrame.flags;
        if( !WriteRecord( state.frame, &value, sizeof( value ),
            "session_runtime_script_frame_write_failed", error ) ) return false;
        ++state.stats.scriptFrames;
        break;
    }
    case QueueType::JnScriptStack:
    {
        StoredScriptStack value;
        value.timeNs = state.transform.ToNanoseconds( item.jnScriptStack.time );
        value.primaryId = item.jnScriptStack.primaryId;
        value.secondaryId = item.jnScriptStack.secondaryId;
        value.value = item.jnScriptStack.value;
        value.thread = record.threadContext;
        value.runtime = item.jnScriptStack.runtime;
        value.flags = item.jnScriptStack.flags;
        value.kind = item.jnScriptStack.kind;
        if( JnScriptRecordKind( value.kind ) == JnScriptRecordKind::Marker )
        {
            const auto text = state.strings.find( value.secondaryId );
            if( text == state.strings.end() )
            { error = "session_runtime_script_marker_string_unresolved"; return false; }
            if( !Intern( state, text->second, value.textOffset, value.textBytes, error ) ) return false;
        }
        if( !WriteRecord( state.stack, &value, sizeof( value ),
            "session_runtime_script_stack_write_failed", error ) ) return false;
        ++state.stats.scriptStackEvents;
        break;
    }
    default: break;
    }
    return true;
}

bool CheckedAppend( uint64_t& value, uint64_t add, std::string& error )
{
    if( value > std::numeric_limits<uint64_t>::max() - add )
    { error = "session_runtime_file_size_overflow"; return false; }
    value += add;
    return true;
}

bool ValidateFile( std::ifstream& in, const TraceSessionManifest& session,
    const RuntimeManifest& manifest, RuntimeFileHeader& header, std::string& error )
{
    in.read( reinterpret_cast<char*>( &header ), sizeof( header ) );
    if( !in || header.magic != RuntimeFileMagic ||
        header.schema != TraceSessionRuntimeIndexSchemaVersion || header.endian != 0x01020304 ||
        header.sourceSize != session.source.fileSize || header.generationBytes != session.generation.size() ||
        header.reserved != 0 || header.domainStates != manifest.stats.domainStates ||
        header.scriptFrames != manifest.stats.scriptFrames ||
        header.scriptStacks != manifest.stats.scriptStackEvents ||
        header.stringBytes != manifest.stats.stringBytes )
    { error = "session_runtime_file_header_invalid"; return false; }
    std::string source( 64, '\0' ), generation( header.generationBytes, '\0' );
    in.read( source.data(), std::streamsize( source.size() ) );
    if( !generation.empty() ) in.read( generation.data(), std::streamsize( generation.size() ) );
    uint64_t expected = sizeof( header );
    const auto append = [&]( uint64_t count, uint64_t bytes ) {
        if( bytes != 0 && count > std::numeric_limits<uint64_t>::max() / bytes ) return false;
        const auto total = count * bytes;
        if( expected > std::numeric_limits<uint64_t>::max() - total ) return false;
        expected += total;
        return true;
    };
    bool offsetsValid = append( 1, source.size() ) && append( 1, generation.size() ) &&
        header.domainOffset == expected;
    offsetsValid = offsetsValid && append( header.domainStates, sizeof( StoredDomainState ) ) &&
        header.frameOffset == expected;
    offsetsValid = offsetsValid && append( header.scriptFrames, sizeof( StoredScriptFrame ) ) &&
        header.stackOffset == expected;
    offsetsValid = offsetsValid && append( header.scriptStacks, sizeof( StoredScriptStack ) ) &&
        header.stringOffset == expected;
    offsetsValid = offsetsValid && append( header.stringBytes, 1 ) && expected == manifest.fileBytes;
    if( !in || source != session.source.sha256 || generation != session.generation || !offsetsValid )
    { error = "session_runtime_file_identity_or_layout_mismatch"; return false; }
    return true;
}

bool VerifyFiles( const std::filesystem::path& root, const TraceSessionManifest& session,
    RuntimeManifest& manifest, RuntimeFileHeader& header, std::string& error )
{
    if( !LoadManifest( root, manifest, error ) ) return false;
    if( manifest.sourceSha256 != session.source.sha256 ||
        manifest.sourceSize != session.source.fileSize || manifest.generation != session.generation )
    { error = "session_runtime_identity_mismatch"; return false; }
    const auto path = root / RuntimeFileName;
    std::error_code ec;
    const auto bytes = std::filesystem::file_size( path, ec );
    if( ec || bytes != manifest.fileBytes )
    { error = "session_runtime_file_size_mismatch"; return false; }
    if( Sha256File( path ) != manifest.fileSha256 )
    { error = "session_runtime_file_sha256_mismatch"; return false; }
    std::ifstream in( path, std::ios::binary );
    if( !in ) { error = "session_runtime_file_open_failed"; return false; }
    return ValidateFile( in, session, manifest, header, error );
}

template<typename T>
std::vector<T> ReadFixed( const std::filesystem::path& path, uint64_t offset,
    uint64_t count, const char* failure )
{
    if( count > std::numeric_limits<size_t>::max() ||
        count > uint64_t( std::numeric_limits<std::streamsize>::max() ) / sizeof( T ) )
        throw std::runtime_error( "runtime index result exceeds platform capacity" );
    std::vector<T> values( static_cast<size_t>( count ) );
    std::ifstream in( path, std::ios::binary );
    if( !in ) throw std::runtime_error( "runtime index is unavailable" );
    in.seekg( std::streamoff( offset ), std::ios::beg );
    if( !values.empty() ) in.read( reinterpret_cast<char*>( values.data() ),
        std::streamsize( values.size() * sizeof( T ) ) );
    if( !in && !values.empty() ) throw std::runtime_error( failure );
    return values;
}

std::string ReadString( std::ifstream& in, uint64_t base, uint64_t offset, uint32_t bytes )
{
    std::string value( bytes, '\0' );
    in.clear();
    in.seekg( std::streamoff( base + offset ), std::ios::beg );
    if( !value.empty() ) in.read( value.data(), std::streamsize( value.size() ) );
    if( !in && !value.empty() ) throw std::runtime_error( "runtime string read failed" );
    return value;
}

}

std::filesystem::path TraceSessionRuntimeIndexRoot( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest )
{
    return sessionRoot / "generations" / manifest.generation / "derived" /
        "runtime-index" / "1" / "exact";
}

bool BuildTraceSessionRuntimeDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& session, TraceSessionRuntimeStats& stats,
    std::string& error )
{
    error.clear(); stats = {};
    if( session.source.sha256.size() != 64 ||
        session.generation.size() > std::numeric_limits<uint32_t>::max() )
    { error = "session_runtime_identity_invalid"; return false; }
    const auto root = TraceSessionRuntimeIndexRoot( sessionRoot, session );
    std::error_code ec;
    std::filesystem::create_directories( root, ec );
    if( ec ) { error = "session_runtime_directory_failed:" + ec.message(); return false; }
    const auto domainWork = root / "domain.work";
    const auto frameWork = root / "frame.work";
    const auto stackWork = root / "stack.work";
    const auto stringWork = root / "strings.work";
    BuildState state;
    state.domain.open( domainWork, std::ios::binary | std::ios::trunc );
    state.frame.open( frameWork, std::ios::binary | std::ios::trunc );
    state.stack.open( stackWork, std::ios::binary | std::ios::trunc );
    state.stringsFile.open( stringWork, std::ios::binary | std::ios::trunc );
    if( !state.domain || !state.frame || !state.stack || !state.stringsFile )
    { error = "session_runtime_work_open_failed"; return false; }
    if( !LoadTraceSessionTimeTransform( sessionRoot, session, state.transform, error ) ||
        !VisitTraceSessionCanonicalOrdered( sessionRoot, session, VisitRuntime, &state, error ) ) return false;
    state.domain.close(); state.frame.close(); state.stack.close(); state.stringsFile.close();

    RuntimeFileHeader header;
    header.sourceSize = session.source.fileSize;
    header.domainStates = state.stats.domainStates;
    header.scriptFrames = state.stats.scriptFrames;
    header.scriptStacks = state.stats.scriptStackEvents;
    header.stringBytes = state.stats.stringBytes;
    header.generationBytes = uint32_t( session.generation.size() );
    uint64_t next = sizeof( header );
    if( !CheckedAppend( next, 64 + session.generation.size(), error ) ) return false;
    header.domainOffset = next;
    if( state.stats.domainStates > std::numeric_limits<uint64_t>::max() / sizeof( StoredDomainState ) ||
        !CheckedAppend( next, state.stats.domainStates * sizeof( StoredDomainState ), error ) ) return false;
    header.frameOffset = next;
    if( state.stats.scriptFrames > std::numeric_limits<uint64_t>::max() / sizeof( StoredScriptFrame ) ||
        !CheckedAppend( next, state.stats.scriptFrames * sizeof( StoredScriptFrame ), error ) ) return false;
    header.stackOffset = next;
    if( state.stats.scriptStackEvents > std::numeric_limits<uint64_t>::max() / sizeof( StoredScriptStack ) ||
        !CheckedAppend( next, state.stats.scriptStackEvents * sizeof( StoredScriptStack ), error ) ) return false;
    header.stringOffset = next;
    if( !CheckedAppend( next, state.stats.stringBytes, error ) ) return false;

    const auto temporary = root / ( std::string( RuntimeFileName ) + ".tmp" );
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_runtime_file_open_failed"; return false; }
    out.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
    out.write( session.source.sha256.data(), std::streamsize( session.source.sha256.size() ) );
    if( !session.generation.empty() ) out.write( session.generation.data(),
        std::streamsize( session.generation.size() ) );
    uint64_t copied = 0;
    if( !CopyFile( domainWork, out, copied, error ) ||
        copied != state.stats.domainStates * sizeof( StoredDomainState ) ) return false;
    if( !CopyFile( frameWork, out, copied, error ) ||
        copied != state.stats.scriptFrames * sizeof( StoredScriptFrame ) ) return false;
    if( !CopyFile( stackWork, out, copied, error ) ||
        copied != state.stats.scriptStackEvents * sizeof( StoredScriptStack ) ) return false;
    if( !CopyFile( stringWork, out, copied, error ) || copied != state.stats.stringBytes ) return false;
    out.flush();
    if( !out ) { error = "session_runtime_file_finalize_failed"; return false; }
    out.close();
    const auto target = root / RuntimeFileName;
    if( !AtomicReplace( temporary, target, error ) ) return false;
    std::filesystem::remove( domainWork, ec );
    std::filesystem::remove( frameWork, ec );
    std::filesystem::remove( stackWork, ec );
    std::filesystem::remove( stringWork, ec );

    RuntimeManifest manifest;
    manifest.sourceSha256 = session.source.sha256;
    manifest.sourceSize = session.source.fileSize;
    manifest.generation = session.generation;
    manifest.fileBytes = next;
    manifest.fileSha256 = Sha256File( target );
    state.stats.fileBytes = next;
    manifest.stats = state.stats;
    if( !SaveManifest( root, manifest, error ) ) return false;
    stats = manifest.stats;
    return true;
}

std::shared_ptr<TraceSessionRuntimeReader> TraceSessionRuntimeReader::Open(
    const std::filesystem::path& sessionRoot, const TraceSessionManifest& session,
    std::string& error )
{
    error.clear();
    RuntimeManifest manifest;
    RuntimeFileHeader header;
    const auto root = TraceSessionRuntimeIndexRoot( sessionRoot, session );
    if( !VerifyFiles( root, session, manifest, header, error ) ) return {};
    auto reader = std::make_shared<TraceSessionRuntimeReader>();
    reader->m_path = root / RuntimeFileName;
    reader->m_fingerprint = session.source.sha256;
    reader->m_domainOffset = header.domainOffset;
    reader->m_frameOffset = header.frameOffset;
    reader->m_stackOffset = header.stackOffset;
    reader->m_stringOffset = header.stringOffset;
    reader->m_stats = manifest.stats;
    return reader;
}

std::vector<RuntimeDomainStateDto> TraceSessionRuntimeReader::DomainStates() const
{
    const auto stored = ReadFixed<StoredDomainState>( m_path, m_domainOffset,
        m_stats.domainStates, "runtime-domain index read failed" );
    std::vector<RuntimeDomainStateDto> result;
    result.reserve( stored.size() );
    for( size_t i = 0; i < stored.size(); ++i )
    {
        const auto& value = stored[i];
        result.push_back( { MakeRef( m_fingerprint, "runtime-domain-state", i ),
            value.generation, value.requestedFrame, value.timeNs,
            MakeRef( m_fingerprint, "thread", value.thread ), value.domain,
            value.requestedMode, value.effectiveMode, value.reason, value.flags } );
    }
    return result;
}

std::vector<ScriptFrameDto> TraceSessionRuntimeReader::ScriptFrames() const
{
    const auto stored = ReadFixed<StoredScriptFrame>( m_path, m_frameOffset,
        m_stats.scriptFrames, "script-frame index read failed" );
    std::ifstream strings( m_path, std::ios::binary );
    if( !strings ) throw std::runtime_error( "runtime string index is unavailable" );
    std::vector<ScriptFrameDto> result;
    result.reserve( stored.size() );
    for( size_t i = 0; i < stored.size(); ++i )
    {
        const auto& value = stored[i];
        result.push_back( { MakeRef( m_fingerprint, "script-frame", i ), value.frameId,
            ReadString( strings, m_stringOffset, value.functionOffset, value.functionBytes ),
            ReadString( strings, m_stringOffset, value.fileOffset, value.fileBytes ), value.line, 0,
            MakeRef( m_fingerprint, "thread", value.thread ), value.runtime, value.flags } );
    }
    return result;
}

std::vector<ScriptStackEventDto> TraceSessionRuntimeReader::ScriptStackEvents() const
{
    const auto stored = ReadFixed<StoredScriptStack>( m_path, m_stackOffset,
        m_stats.scriptStackEvents, "script-stack index read failed" );
    std::ifstream strings( m_path, std::ios::binary );
    if( !strings ) throw std::runtime_error( "runtime string index is unavailable" );
    std::vector<ScriptStackEventDto> result;
    result.reserve( stored.size() );
    for( size_t i = 0; i < stored.size(); ++i )
    {
        const auto& value = stored[i];
        result.push_back( { MakeRef( m_fingerprint, "script-stack-event", i ),
            value.primaryId, value.secondaryId, value.value, value.timeNs,
            MakeRef( m_fingerprint, "thread", value.thread ), value.runtime, value.flags,
            value.kind, value.textBytes == 0 ? std::string() :
                ReadString( strings, m_stringOffset, value.textOffset, value.textBytes ) } );
    }
    return result;
}

bool AuditTraceSessionRuntimeDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& session, TraceSessionRuntimeStats& stats,
    std::string& error )
{
    RuntimeManifest manifest;
    RuntimeFileHeader header;
    if( !VerifyFiles( TraceSessionRuntimeIndexRoot( sessionRoot, session ), session,
        manifest, header, error ) ) return false;
    stats = manifest.stats;
    return true;
}

}

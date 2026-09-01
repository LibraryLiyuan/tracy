#include "TracyTraceSessionMessages.hpp"

#include "TracyHash.hpp"
#include "TracyTraceSessionCanonical.hpp"
#include "TracyTraceSessionGpuCanonical.hpp"
#include "TracyProtocol.hpp"
#include "TracyQueue.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#  include <Windows.h>
#endif

namespace tracy::analysis
{
namespace
{

constexpr uint64_t MessageFileMagic = 0x3147534d534e4aull;     // JNSMSG1
constexpr uint64_t MessageManifestMagic = 0x3146474d534e4aull; // JNMGF1
constexpr const char* MessageFileName = "messages.bin";

#pragma pack( push, 1 )
struct MessageFileHeader
{
    uint64_t magic = MessageFileMagic;
    uint32_t schema = TraceSessionMessageIndexSchemaVersion;
    uint32_t endian = 0x01020304;
    uint64_t sourceSize = 0;
    uint64_t messageCount = 0;
    uint64_t appInfoEvents = 0;
    std::array<uint64_t, TraceSessionMessageEventKindCount> eventCounts {};
    uint64_t recordsOffset = 0;
    uint64_t literalsOffset = 0;
    uint64_t stringsOffset = 0;
    uint64_t stringBytes = 0;
    uint32_t generationBytes = 0;
    uint32_t reserved = 0;
};

enum class StoredTextKind : uint8_t { Direct = 0, LiteralPointer = 1 };

struct StoredMessage
{
    uint64_t thread = 0;
    int64_t timeNs = 0;
    uint64_t text = 0;
    uint32_t textBytes = 0;
    uint32_t color = 0xFFFFFFFF;
    uint32_t callstack = 0;
    uint8_t textKind = uint8_t( StoredTextKind::Direct );
    uint8_t reserved[3] {};
};

struct StoredLiteral
{
    uint64_t pointer = 0;
    uint64_t textOffset = 0;
    uint32_t textBytes = 0;
    uint32_t reserved = 0;
};
#pragma pack( pop )

struct MessageManifest
{
    std::string sourceSha256;
    uint64_t sourceSize = 0;
    std::string generation;
    uint64_t fileBytes = 0;
    std::string fileSha256;
    TraceSessionMessageStats stats;
};

struct BuildState
{
    TraceSessionTimeTransform transform;
    uint64_t threadContext = 0;
    std::optional<std::string> pendingSingleString;
    std::unordered_map<uint64_t, std::string> literalStrings;
    std::unordered_set<uint64_t> referencedLiteralPointers;
    std::unordered_map<std::string, uint32_t> callstackIds;
    std::vector<std::vector<uint64_t>> callstacks { {} };
    std::unordered_map<std::string, uint64_t> managedFrameIds;
    uint64_t nextManagedFrameId = 0;
    uint32_t pendingCallstack = 0;
    std::unordered_map<uint64_t, uint32_t> threadCallstacks;
    std::filesystem::path recordWorkPath;
    std::filesystem::path stringWorkPath;
    std::ofstream recordWork;
    std::ofstream stringWork;
    uint64_t stringBytes = 0;
    TraceSessionMessageStats stats;
};

bool AtomicReplace( const std::filesystem::path& source,
    const std::filesystem::path& target, std::string& error )
{
#ifdef _WIN32
    if( MoveFileExW( source.c_str(), target.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) ) return true;
    error = "session_message_atomic_replace_failed:" + std::to_string( GetLastError() );
    return false;
#else
    std::error_code ec;
    std::filesystem::rename( source, target, ec );
    if( !ec ) return true;
    error = "session_message_atomic_replace_failed:" + ec.message();
    return false;
#endif
}

bool DecodeItem( const TraceSessionCanonicalRecord& record, QueueItem& item,
    std::string& error )
{
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ||
        record.type >= uint8_t( QueueType::NUM_TYPES ) ||
        record.payload.size() < QueueDataSize[record.type] )
    { error = "session_message_protocol_record_invalid"; return false; }
    item = {};
    std::memcpy( &item, record.payload.data(),
        std::min<size_t>( record.payload.size(), sizeof( item ) ) );
    if( item.hdr.idx != record.type )
    { error = "session_message_protocol_type_mismatch"; return false; }
    return true;
}

bool GetShortPayload( const TraceSessionCanonicalRecord& record,
    const uint8_t*& data, size_t& size, std::string& error )
{
    const auto fixed = size_t( QueueDataSize[record.type] );
    if( record.payload.size() < fixed + sizeof( uint16_t ) )
    { error = "session_message_payload_truncated"; return false; }
    uint16_t bytes = 0;
    std::memcpy( &bytes, record.payload.data() + fixed, sizeof( bytes ) );
    if( bytes != record.variablePayloadBytes ||
        record.payload.size() != fixed + sizeof( bytes ) + bytes )
    { error = "session_message_payload_mismatch"; return false; }
    data = record.payload.data() + fixed + sizeof( bytes );
    size = bytes;
    return true;
}

std::optional<size_t> MessageEventKind( QueueType type )
{
    switch( type )
    {
    case QueueType::Message: return 0;
    case QueueType::MessageColor: return 1;
    case QueueType::MessageCallstack: return 2;
    case QueueType::MessageColorCallstack: return 3;
    case QueueType::MessageAppInfo: return 4;
    case QueueType::MessageLiteral: return 5;
    case QueueType::MessageLiteralColor: return 6;
    case QueueType::MessageLiteralCallstack: return 7;
    case QueueType::MessageLiteralColorCallstack: return 8;
    default: return std::nullopt;
    }
}

bool IsLiteralMessage( QueueType type )
{
    return type == QueueType::MessageLiteral ||
        type == QueueType::MessageLiteralColor ||
        type == QueueType::MessageLiteralCallstack ||
        type == QueueType::MessageLiteralColorCallstack;
}

bool IsColorMessage( QueueType type )
{
    return type == QueueType::MessageColor ||
        type == QueueType::MessageColorCallstack ||
        type == QueueType::MessageLiteralColor ||
        type == QueueType::MessageLiteralColorCallstack;
}

bool IsCallstackMessage( QueueType type )
{
    return type == QueueType::MessageCallstack ||
        type == QueueType::MessageColorCallstack ||
        type == QueueType::MessageLiteralCallstack ||
        type == QueueType::MessageLiteralColorCallstack;
}

uint32_t InternCallstack( BuildState& state, const std::vector<uint64_t>& entries,
    std::string& error )
{
    if( entries.empty() || entries.size() > 65535 )
    { error = "session_message_callstack_size_invalid"; return 0; }
    const std::string key( reinterpret_cast<const char*>( entries.data() ),
        entries.size() * sizeof( uint64_t ) );
    const auto found = state.callstackIds.find( key );
    if( found != state.callstackIds.end() ) return found->second;
    if( state.callstacks.size() > std::numeric_limits<uint32_t>::max() )
    { error = "session_message_callstack_count_overflow"; return 0; }
    const auto id = uint32_t( state.callstacks.size() );
    state.callstacks.emplace_back( entries );
    state.callstackIds.emplace( key, id );
    return id;
}

uint32_t InternNativeCallstack( BuildState& state, const uint8_t* data,
    size_t size, std::string& error )
{
    if( size == 0 || size % sizeof( uint64_t ) != 0 )
    { error = "session_message_callstack_payload_invalid"; return 0; }
    std::vector<uint64_t> entries( size / sizeof( uint64_t ) );
    std::memcpy( entries.data(), data, size );
    return InternCallstack( state, entries, error );
}

void Put32( std::string& out, uint32_t value )
{
    for( int index = 0; index < 4; ++index )
        out.push_back( char( uint8_t( value >> ( index * 8 ) ) ) );
}

void PutString( std::string& out, const std::string& value )
{
    Put32( out, uint32_t( value.size() ) );
    out.append( value );
}

bool ParseManagedCallstack( BuildState& state, const uint8_t* data,
    size_t size, std::string& error )
{
    if( size == 0 ) { error = "session_message_managed_callstack_empty"; return false; }
    size_t offset = 0;
    const auto count = data[offset++];
    if( count > 64 )
    { error = "session_message_managed_callstack_too_deep"; return false; }
    std::vector<uint64_t> entries;
    entries.reserve( count + ( state.pendingCallstack ?
        state.callstacks[state.pendingCallstack].size() : 0 ) );
    constexpr uint64_t CustomFrameMask = uint64_t( 1 ) << 62;
    for( uint8_t index = 0; index < count; ++index )
    {
        if( offset > size || size - offset < 6 )
        { error = "session_message_managed_frame_truncated"; return false; }
        uint32_t line = 0;
        std::memcpy( &line, data + offset, 4 ); offset += 4;
        uint16_t nameBytes = 0;
        std::memcpy( &nameBytes, data + offset, 2 ); offset += 2;
        if( nameBytes > size - offset )
        { error = "session_message_managed_name_truncated"; return false; }
        std::string name( reinterpret_cast<const char*>( data + offset ), nameBytes );
        offset += nameBytes;
        if( size - offset < 2 )
        { error = "session_message_managed_file_header_truncated"; return false; }
        uint16_t fileBytes = 0;
        std::memcpy( &fileBytes, data + offset, 2 ); offset += 2;
        if( fileBytes > size - offset )
        { error = "session_message_managed_file_truncated"; return false; }
        std::string file( reinterpret_cast<const char*>( data + offset ), fileBytes );
        offset += fileBytes;
        std::string key;
        Put32( key, line ); PutString( key, name ); PutString( key, file );
        auto found = state.managedFrameIds.find( key );
        if( found == state.managedFrameIds.end() )
            found = state.managedFrameIds.emplace( std::move( key ),
                CustomFrameMask | state.nextManagedFrameId++ ).first;
        entries.emplace_back( found->second );
    }
    if( offset != size )
    { error = "session_message_managed_callstack_trailing_bytes"; return false; }
    if( state.pendingCallstack != 0 )
    {
        const auto& native = state.callstacks[state.pendingCallstack];
        entries.insert( entries.end(), native.begin(), native.end() );
    }
    const auto id = InternCallstack( state, entries, error );
    if( id == 0 ) return false;
    state.pendingCallstack = id;
    return true;
}

bool AppendMessage( BuildState& state, const QueueItem& item, QueueType type,
    std::string& error )
{
    StoredMessage value;
    value.thread = state.threadContext;
    value.timeNs = state.transform.ToNanoseconds( item.message.time );
    if( IsColorMessage( type ) )
        value.color = 0xFF000000u | ( uint32_t( item.messageColor.b ) << 16 ) |
            ( uint32_t( item.messageColor.g ) << 8 ) | item.messageColor.r;
    if( IsCallstackMessage( type ) )
    {
        auto& callstack = state.threadCallstacks[state.threadContext];
        if( callstack == 0 )
        { error = "session_message_thread_callstack_missing"; return false; }
        value.callstack = callstack;
        callstack = 0;
    }
    if( IsLiteralMessage( type ) )
    {
        value.textKind = uint8_t( StoredTextKind::LiteralPointer );
        value.text = IsColorMessage( type ) ?
            item.messageColorLiteral.text : item.messageLiteral.text;
        state.referencedLiteralPointers.emplace( value.text );
    }
    else
    {
        if( !state.pendingSingleString )
        { error = "session_message_single_string_missing"; return false; }
        value.text = state.stringBytes;
        if( state.pendingSingleString->size() > std::numeric_limits<uint32_t>::max() )
        { error = "session_message_text_too_large"; return false; }
        value.textBytes = uint32_t( state.pendingSingleString->size() );
        state.stringWork.write( state.pendingSingleString->data(),
            std::streamsize( state.pendingSingleString->size() ) );
        if( !state.stringWork )
        { error = "session_message_string_work_write_failed"; return false; }
        state.stringBytes += state.pendingSingleString->size();
    }
    state.pendingSingleString.reset();
    state.recordWork.write( reinterpret_cast<const char*>( &value ), sizeof( value ) );
    if( !state.recordWork )
    { error = "session_message_record_work_write_failed"; return false; }
    ++state.stats.messages;
    return true;
}

bool VisitMessageRecord( const TraceSessionCanonicalRecord& record,
    void* userData, std::string& error )
{
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ) return true;
    auto& state = *static_cast<BuildState*>( userData );
    QueueItem item {};
    if( !DecodeItem( record, item, error ) ) return false;
    const auto type = QueueType( record.type );
    if( type == QueueType::SingleStringData )
    {
        const uint8_t* data = nullptr; size_t size = 0;
        if( !GetShortPayload( record, data, size, error ) ) return false;
        state.pendingSingleString = std::string(
            reinterpret_cast<const char*>( data ), size );
        return true;
    }
    if( type == QueueType::StringData )
    {
        const uint8_t* data = nullptr; size_t size = 0;
        if( !GetShortPayload( record, data, size, error ) ) return false;
        const std::string value( reinterpret_cast<const char*>( data ), size );
        const auto [found, inserted] = state.literalStrings.emplace(
            item.stringTransfer.ptr, value );
        if( !inserted && found->second != value )
        { error = "session_message_literal_string_conflict"; return false; }
        state.pendingSingleString.reset();
        return true;
    }
    if( type == QueueType::CallstackPayload ||
        type == QueueType::CallstackSampleDictionary )
    {
        const uint8_t* data = nullptr; size_t size = 0;
        if( !GetShortPayload( record, data, size, error ) ) return false;
        const auto id = InternNativeCallstack( state, data, size, error );
        if( id == 0 ) return false;
        if( type == QueueType::CallstackPayload )
        {
            if( state.pendingCallstack != 0 )
            { error = "session_message_callstack_sequence_invalid"; return false; }
            state.pendingCallstack = id;
        }
        state.pendingSingleString.reset();
        return true;
    }
    if( type == QueueType::CallstackAllocPayload )
    {
        const uint8_t* data = nullptr; size_t size = 0;
        if( !GetShortPayload( record, data, size, error ) ||
            !ParseManagedCallstack( state, data, size, error ) ) return false;
        state.pendingSingleString.reset();
        return true;
    }
    if( type == QueueType::Callstack || type == QueueType::CallstackAlloc )
    {
        if( state.pendingCallstack == 0 )
        { error = "session_message_callstack_consumer_without_payload"; return false; }
        state.threadCallstacks[state.threadContext] = state.pendingCallstack;
        state.pendingCallstack = 0;
        state.pendingSingleString.reset();
        return true;
    }
    if( type == QueueType::CallstackSerial || type == QueueType::CallstackSample ||
        type == QueueType::CallstackSampleContextSwitch )
    {
        if( state.pendingCallstack == 0 )
        { error = "session_message_callstack_consumer_without_payload"; return false; }
        state.pendingCallstack = 0;
        state.pendingSingleString.reset();
        return true;
    }
    const auto kind = MessageEventKind( type );
    if( !kind )
    {
        // SingleStringData is a one-consumer staging slot. Any intervening
        // protocol event proves that the string belongs to another domain.
        state.pendingSingleString.reset();
        if( type == QueueType::ThreadContext ) state.threadContext = item.threadCtx.thread;
        return true;
    }
    ++state.stats.eventCounts[*kind];
    if( type == QueueType::MessageAppInfo )
    {
        if( !state.pendingSingleString )
        { error = "session_message_app_info_string_missing"; return false; }
        state.pendingSingleString.reset();
        ++state.stats.appInfoEvents;
        return true;
    }
    return AppendMessage( state, item, type, error );
}

bool CopyFile( const std::filesystem::path& path, std::ofstream& out,
    std::string& error )
{
    std::ifstream in( path, std::ios::binary );
    if( !in ) { error = "session_message_work_read_failed"; return false; }
    // Keep large I/O buffers off the thread stack. The production converter
    // uses the default Windows 1 MiB stack and this helper is called through
    // the full Mandatory Derived pipeline, so a 1 MiB local array overflows
    // before the first message byte can be committed on real captures.
    std::vector<char> buffer( 1024 * 1024 );
    while( in )
    {
        in.read( buffer.data(), std::streamsize( buffer.size() ) );
        const auto bytes = in.gcount();
        if( bytes > 0 ) out.write( buffer.data(), bytes );
    }
    if( !in.eof() || !out )
    { error = "session_message_work_copy_failed"; return false; }
    return true;
}

bool SaveManifest( const std::filesystem::path& root,
    const MessageManifest& value, std::string& error )
{
    const auto temporary = root / "manifest.tmp";
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_message_manifest_open_failed"; return false; }
    out << std::hex << MessageManifestMagic << std::dec << '\n'
        << TraceSessionMessageIndexSchemaVersion << '\n'
        << std::quoted( value.sourceSha256 ) << '\n' << value.sourceSize << '\n'
        << std::quoted( value.generation ) << '\n' << value.fileBytes << '\n'
        << std::quoted( value.fileSha256 ) << '\n' << value.stats.messages << ' '
        << value.stats.appInfoEvents << ' ' << value.stats.literalStrings;
    for( const auto count : value.stats.eventCounts ) out << ' ' << count;
    out << '\n';
    out.flush();
    if( !out ) { error = "session_message_manifest_write_failed"; return false; }
    out.close();
    return AtomicReplace( temporary, root / "manifest", error );
}

bool LoadManifest( const std::filesystem::path& root,
    MessageManifest& value, std::string& error )
{
    std::ifstream in( root / "manifest", std::ios::binary );
    uint64_t magic = 0; uint32_t schema = 0;
    if( !( in >> std::hex >> magic >> std::dec >> schema >> std::quoted( value.sourceSha256 )
        >> value.sourceSize >> std::quoted( value.generation ) >> value.fileBytes
        >> std::quoted( value.fileSha256 ) >> value.stats.messages
        >> value.stats.appInfoEvents >> value.stats.literalStrings ) )
    { error = "session_message_manifest_parse_failed"; return false; }
    for( auto& count : value.stats.eventCounts ) if( !( in >> count ) )
    { error = "session_message_manifest_parse_failed"; return false; }
    value.stats.fileBytes = value.fileBytes;
    if( magic != MessageManifestMagic || schema != TraceSessionMessageIndexSchemaVersion ||
        value.sourceSha256.size() != 64 || value.fileSha256.size() != 64 )
    { error = "session_message_manifest_invalid"; return false; }
    return true;
}

std::string MakeRef( const std::string& fingerprint, const char* kind, uint64_t id )
{
    std::ostringstream out;
    out << "tracy:v1:" << fingerprint.substr( 0, 16 ) << ':' << kind << ':' << std::hex << id;
    return out.str();
}

}

struct TraceSessionMessageReader::Impl
{
    std::filesystem::path path;
    std::string fingerprint;
    uint64_t recordsOffset = 0;
    uint64_t messageCount = 0;
    uint64_t stringsOffset = 0;
    std::unordered_map<uint64_t, StoredLiteral> literals;
};

TraceSessionMessageReader::TraceSessionMessageReader( std::shared_ptr<Impl> impl )
    : m_impl( std::move( impl ) )
{}

std::filesystem::path TraceSessionMessageIndexRoot(
    const std::filesystem::path& sessionRoot, const TraceSessionManifest& manifest )
{
    return sessionRoot / "generations" / manifest.generation / "derived" /
        "message-index" / "1" / "exact";
}

bool BuildTraceSessionMessageDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& session, TraceSessionMessageStats& stats,
    std::string& error )
{
    error.clear(); stats = {};
    const auto root = TraceSessionMessageIndexRoot( sessionRoot, session );
    std::error_code ec; std::filesystem::create_directories( root, ec );
    if( ec ) { error = "session_message_directory_failed:" + ec.message(); return false; }
    BuildState state;
    state.recordWorkPath = root / "messages.work";
    state.stringWorkPath = root / "strings.work";
    state.recordWork.open( state.recordWorkPath, std::ios::binary | std::ios::trunc );
    state.stringWork.open( state.stringWorkPath, std::ios::binary | std::ios::trunc );
    if( !state.recordWork || !state.stringWork )
    { error = "session_message_work_open_failed"; return false; }
    if( !LoadTraceSessionTimeTransform( sessionRoot, session, state.transform, error ) ||
        !VisitTraceSessionCanonicalOrdered( sessionRoot, session,
            VisitMessageRecord, &state, error ) ) return false;
    for( const auto pointer : state.referencedLiteralPointers )
        if( !state.literalStrings.contains( pointer ) )
        { error = "session_message_literal_string_unresolved"; return false; }
    state.recordWork.flush(); state.stringWork.flush();
    if( !state.recordWork || !state.stringWork )
    { error = "session_message_work_flush_failed"; return false; }
    state.recordWork.close(); state.stringWork.close();

    std::map<uint64_t, std::string> literals(
        state.literalStrings.begin(), state.literalStrings.end() );
    std::vector<StoredLiteral> storedLiterals;
    storedLiterals.reserve( literals.size() );
    uint64_t literalStringOffset = state.stringBytes;
    for( const auto& [pointer, text] : literals )
    {
        if( text.size() > std::numeric_limits<uint32_t>::max() )
        { error = "session_message_literal_too_large"; return false; }
        storedLiterals.push_back( { pointer, literalStringOffset,
            uint32_t( text.size() ), 0 } );
        literalStringOffset += text.size();
    }
    state.stats.literalStrings = storedLiterals.size();

    MessageFileHeader header;
    header.sourceSize = session.source.fileSize;
    header.messageCount = state.stats.messages;
    header.appInfoEvents = state.stats.appInfoEvents;
    header.eventCounts = state.stats.eventCounts;
    header.generationBytes = uint32_t( session.generation.size() );
    header.recordsOffset = sizeof( header ) + session.source.sha256.size() +
        session.generation.size();
    header.literalsOffset = header.recordsOffset +
        header.messageCount * sizeof( StoredMessage );
    header.stringsOffset = header.literalsOffset +
        storedLiterals.size() * sizeof( StoredLiteral );
    header.stringBytes = literalStringOffset;

    const auto temporary = root / "messages.bin.tmp";
    const auto target = root / MessageFileName;
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_message_file_open_failed"; return false; }
    out.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
    out.write( session.source.sha256.data(), std::streamsize( session.source.sha256.size() ) );
    out.write( session.generation.data(), std::streamsize( session.generation.size() ) );
    if( !CopyFile( state.recordWorkPath, out, error ) ) return false;
    if( !storedLiterals.empty() ) out.write(
        reinterpret_cast<const char*>( storedLiterals.data() ),
        std::streamsize( storedLiterals.size() * sizeof( StoredLiteral ) ) );
    if( !CopyFile( state.stringWorkPath, out, error ) ) return false;
    for( const auto& [pointer, text] : literals )
        out.write( text.data(), std::streamsize( text.size() ) );
    out.flush();
    if( !out ) { error = "session_message_file_write_failed"; return false; }
    out.close();
    if( !AtomicReplace( temporary, target, error ) ) return false;
    std::filesystem::remove( state.recordWorkPath, ec ); ec.clear();
    std::filesystem::remove( state.stringWorkPath, ec ); ec.clear();

    MessageManifest manifest;
    manifest.sourceSha256 = session.source.sha256;
    manifest.sourceSize = session.source.fileSize;
    manifest.generation = session.generation;
    manifest.fileBytes = std::filesystem::file_size( target, ec );
    if( ec ) { error = "session_message_file_size_failed:" + ec.message(); return false; }
    manifest.fileSha256 = Sha256File( target );
    manifest.stats = state.stats;
    manifest.stats.fileBytes = manifest.fileBytes;
    if( !SaveManifest( root, manifest, error ) ) return false;
    stats = manifest.stats;
    return true;
}

bool AuditTraceSessionMessageDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& session, TraceSessionMessageStats& stats,
    std::string& error )
{
    error.clear(); stats = {};
    const auto root = TraceSessionMessageIndexRoot( sessionRoot, session );
    MessageManifest manifest;
    if( !LoadManifest( root, manifest, error ) ) return false;
    if( manifest.sourceSha256 != session.source.sha256 ||
        manifest.sourceSize != session.source.fileSize ||
        manifest.generation != session.generation )
    { error = "session_message_identity_mismatch"; return false; }
    const auto path = root / MessageFileName;
    std::error_code ec;
    if( std::filesystem::file_size( path, ec ) != manifest.fileBytes || ec )
    { error = "session_message_file_size_mismatch"; return false; }
    if( Sha256File( path ) != manifest.fileSha256 )
    { error = "session_message_file_sha256_mismatch"; return false; }
    stats = manifest.stats;
    return true;
}

std::shared_ptr<TraceSessionMessageReader> TraceSessionMessageReader::Open(
    const std::filesystem::path& sessionRoot, const TraceSessionManifest& session,
    std::string& error )
{
    TraceSessionMessageStats stats;
    if( !AuditTraceSessionMessageDerived( sessionRoot, session, stats, error ) ) return {};
    const auto path = TraceSessionMessageIndexRoot( sessionRoot, session ) / MessageFileName;
    std::ifstream in( path, std::ios::binary );
    MessageFileHeader header;
    if( !in.read( reinterpret_cast<char*>( &header ), sizeof( header ) ) ||
        header.magic != MessageFileMagic ||
        header.schema != TraceSessionMessageIndexSchemaVersion ||
        header.endian != 0x01020304 || header.sourceSize != session.source.fileSize ||
        header.messageCount != stats.messages || header.appInfoEvents != stats.appInfoEvents ||
        header.eventCounts != stats.eventCounts || header.generationBytes != session.generation.size() )
    { error = "session_message_file_header_invalid"; return {}; }
    std::string sha( 64, '\0' ), generation( header.generationBytes, '\0' );
    if( !in.read( sha.data(), std::streamsize( sha.size() ) ) ||
        !in.read( generation.data(), std::streamsize( generation.size() ) ) ||
        sha != session.source.sha256 || generation != session.generation ||
        header.recordsOffset != uint64_t( in.tellg() ) ||
        header.literalsOffset != header.recordsOffset +
            header.messageCount * sizeof( StoredMessage ) ||
        header.stringsOffset != header.literalsOffset +
            stats.literalStrings * sizeof( StoredLiteral ) ||
        header.stringsOffset > stats.fileBytes ||
        header.stringBytes != stats.fileBytes - header.stringsOffset )
    { error = "session_message_file_identity_or_bounds_invalid"; return {}; }
    in.seekg( std::streamoff( header.literalsOffset ) );
    auto impl = std::make_shared<Impl>();
    impl->path = path; impl->fingerprint = session.source.sha256;
    impl->recordsOffset = header.recordsOffset;
    impl->messageCount = header.messageCount;
    impl->stringsOffset = header.stringsOffset;
    for( uint64_t index = 0; index < stats.literalStrings; ++index )
    {
        StoredLiteral value;
        if( !in.read( reinterpret_cast<char*>( &value ), sizeof( value ) ) ||
            value.reserved != 0 || value.textOffset > header.stringBytes ||
            value.textBytes > header.stringBytes - value.textOffset ||
            !impl->literals.emplace( value.pointer, value ).second )
        { error = "session_message_literal_record_invalid"; return {}; }
    }
    auto reader = std::shared_ptr<TraceSessionMessageReader>(
        new TraceSessionMessageReader( impl ) );
    reader->m_stats = stats;
    return reader;
}

std::vector<MessageDto> TraceSessionMessageReader::Scan( const ScanRange& range ) const
{
    std::vector<MessageDto> result;
    std::ifstream records( m_impl->path, std::ios::binary );
    std::ifstream strings( m_impl->path, std::ios::binary );
    if( !records || !strings ) return result;
    records.seekg( std::streamoff( m_impl->recordsOffset ) );
    size_t skipped = 0;
    for( uint64_t index = 0; index < m_impl->messageCount; ++index )
    {
        StoredMessage value;
        if( !records.read( reinterpret_cast<char*>( &value ), sizeof( value ) ) ) return result;
        if( value.timeNs < range.startNs || value.timeNs >= range.endNs ) continue;
        if( skipped++ < range.offset ) continue;
        uint64_t textOffset = value.text;
        uint32_t textBytes = value.textBytes;
        if( StoredTextKind( value.textKind ) == StoredTextKind::LiteralPointer )
        {
            const auto found = m_impl->literals.find( value.text );
            if( found == m_impl->literals.end() ) continue;
            textOffset = found->second.textOffset;
            textBytes = found->second.textBytes;
        }
        std::string text( textBytes, '\0' );
        strings.seekg( std::streamoff( m_impl->stringsOffset + textOffset ) );
        if( textBytes != 0 && !strings.read( text.data(), std::streamsize( textBytes ) ) ) return result;
        MessageDto dto;
        dto.ref = MakeRef( m_impl->fingerprint, "message", index );
        dto.threadRef = MakeRef( m_impl->fingerprint, "thread", value.thread );
        dto.timeNs = value.timeNs; dto.text = std::move( text );
        dto.color = value.color; dto.callstack = value.callstack;
        if( value.callstack != 0 ) dto.callstackRef =
            MakeRef( m_impl->fingerprint, "callstack", value.callstack );
        result.emplace_back( std::move( dto ) );
        if( result.size() >= range.limit ) break;
    }
    return result;
}

}

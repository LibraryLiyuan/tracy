#include "TracyTraceSessionSymbols.hpp"

#include "TracyHash.hpp"
#include "TracyQueue.hpp"
#include "TracyTraceSessionCanonical.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <unordered_map>

#ifdef _WIN32
#  include <Windows.h>
#endif

namespace tracy::analysis
{
namespace
{

constexpr uint64_t MetadataMagic = 0x31594d53534e4aull; // JNSSMY1
constexpr uint64_t ManifestMagic = 0x314d5953534e4aull; // JNSSYM1
constexpr const char* MetadataFileName = "symbols.bin";
constexpr const char* CodeFileName = "symbol-code.bin";
constexpr uint64_t CustomFrameMask = uint64_t( 1 ) << 62;

struct FrameLine
{
    std::string name;
    std::string file;
    uint32_t line = 0;
    uint64_t symbol = 0;
};

struct FrameDefinition
{
    uint64_t address = 0;
    std::string image;
    std::vector<FrameLine> lines;
};

struct SymbolDefinition
{
    uint64_t address = 0;
    uint64_t codeOffset = 0;
    uint32_t codeBytes = 0;
    uint32_t size = 0;
    uint32_t line = 0;
    uint32_t callLine = 0;
    bool inlineFrame = false;
    std::string name;
    std::string file;
    std::string image;
    std::string callFile;
};

struct PendingSymbol
{
    std::string name;
    std::string image;
    std::string callFile;
    uint32_t callLine = 0;
    uint32_t size = 0;
    bool inlineFrame = false;
};

struct SymbolManifest
{
    std::string sourceSha256;
    uint64_t sourceSize = 0;
    std::string generation;
    uint64_t metadataFileBytes = 0;
    uint64_t codeFileBytes = 0;
    std::string metadataSha256;
    std::string codeSha256;
    TraceSessionSymbolStats stats;
};

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

void PutString( std::vector<uint8_t>& out, const std::string& value )
{
    Put32( out, uint32_t( value.size() ) );
    out.insert( out.end(), value.begin(), value.end() );
}

bool GetString( const std::vector<uint8_t>& in, size_t& offset,
    std::string& value, std::string& error )
{
    uint32_t bytes = 0;
    if( !Get32( in, offset, bytes ) || bytes > 16 * 1024 * 1024 ||
        offset > in.size() || bytes > in.size() - offset )
    { error = "session_symbol_string_invalid"; return false; }
    value.assign( reinterpret_cast<const char*>( in.data() + offset ), bytes );
    offset += bytes;
    return true;
}

std::string Hex( uint64_t value )
{
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

std::string MakeRef( const std::string& fingerprint, const char* kind, uint64_t id )
{
    std::ostringstream out;
    out << "tracy:v1:" << fingerprint.substr( 0, 16 ) << ':' << kind << ':' << std::hex << id;
    return out.str();
}

bool AtomicReplace( const std::filesystem::path& source,
    const std::filesystem::path& target, std::string& error )
{
#ifdef _WIN32
    if( MoveFileExW( source.c_str(), target.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) ) return true;
    error = "session_symbol_atomic_replace_failed:" + std::to_string( GetLastError() );
    return false;
#else
    std::error_code ec;
    std::filesystem::rename( source, target, ec );
    if( !ec ) return true;
    error = "session_symbol_atomic_replace_failed:" + ec.message();
    return false;
#endif
}

bool DecodeItem( const TraceSessionCanonicalRecord& record, QueueItem& item,
    std::string& error )
{
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ||
        record.type >= uint8_t( QueueType::NUM_TYPES ) ||
        record.payload.size() < QueueDataSize[record.type] )
    { error = "session_symbol_protocol_record_invalid"; return false; }
    item = {};
    std::memcpy( &item, record.payload.data(),
        std::min<size_t>( record.payload.size(), sizeof( item ) ) );
    if( item.hdr.idx != record.type )
    { error = "session_symbol_protocol_type_mismatch"; return false; }
    return true;
}

bool GetShortPayload( const TraceSessionCanonicalRecord& record,
    const uint8_t*& data, size_t& size, std::string& error )
{
    const auto fixed = size_t( QueueDataSize[record.type] );
    if( record.payload.size() < fixed + sizeof( uint16_t ) )
    { error = "session_symbol_payload_truncated"; return false; }
    uint16_t bytes = 0;
    std::memcpy( &bytes, record.payload.data() + fixed, sizeof( bytes ) );
    if( bytes != record.variablePayloadBytes ||
        record.payload.size() != fixed + sizeof( bytes ) + bytes )
    { error = "session_symbol_payload_mismatch"; return false; }
    data = record.payload.data() + fixed + sizeof( bytes );
    size = bytes;
    return true;
}

bool GetLargePayload( const TraceSessionCanonicalRecord& record,
    const uint8_t*& data, size_t& size, std::string& error )
{
    const auto fixed = size_t( QueueDataSize[record.type] );
    if( record.payload.size() < fixed + sizeof( uint32_t ) )
    { error = "session_symbol_large_payload_truncated"; return false; }
    uint32_t bytes = 0;
    std::memcpy( &bytes, record.payload.data() + fixed, sizeof( bytes ) );
    if( bytes != record.variablePayloadBytes ||
        record.payload.size() != fixed + sizeof( bytes ) + bytes )
    { error = "session_symbol_large_payload_mismatch"; return false; }
    data = record.payload.data() + fixed + sizeof( bytes );
    size = bytes;
    return true;
}

struct FrameStaging
{
    uint64_t address = 0;
    uint8_t expected = 0;
    std::string image;
    std::vector<FrameLine> lines;
};

struct BuildState
{
    std::vector<std::vector<uint64_t>> callstacks { {} };
    std::unordered_map<std::string, uint32_t> callstackIds;
    uint32_t pendingCallstack = 0;
    std::unordered_map<std::string, uint64_t> managedFrameIds;
    uint64_t nextManagedFrameId = 0;
    std::unordered_map<uint64_t, FrameDefinition> frames;
    std::optional<FrameStaging> staging;
    std::optional<std::string> singleString;
    std::optional<std::string> secondString;
    std::unordered_map<uint64_t, PendingSymbol> pendingSymbols;
    std::map<uint64_t, SymbolDefinition> symbols;
    std::map<uint64_t, std::vector<uint8_t>> code;
    std::string error;
};

uint32_t InternCallstack( BuildState& state, const std::vector<uint64_t>& entries,
    std::string& error )
{
    if( entries.empty() || entries.size() > 65535 )
    { error = "session_symbol_callstack_size_invalid"; return 0; }
    const std::string key( reinterpret_cast<const char*>( entries.data() ),
        entries.size() * sizeof( uint64_t ) );
    const auto found = state.callstackIds.find( key );
    if( found != state.callstackIds.end() ) return found->second;
    if( state.callstacks.size() > std::numeric_limits<uint32_t>::max() )
    { error = "session_symbol_callstack_count_overflow"; return 0; }
    const auto id = uint32_t( state.callstacks.size() );
    state.callstacks.emplace_back( entries );
    state.callstackIds.emplace( key, id );
    return id;
}

uint32_t InternNativeCallstack( BuildState& state, const uint8_t* data,
    size_t size, std::string& error )
{
    if( size == 0 || size % sizeof( uint64_t ) != 0 )
    { error = "session_symbol_callstack_payload_invalid"; return 0; }
    std::vector<uint64_t> entries( size / sizeof( uint64_t ) );
    std::memcpy( entries.data(), data, size );
    return InternCallstack( state, entries, error );
}

bool ParseManagedCallstack( BuildState& state, const uint8_t* data,
    size_t size, std::string& error )
{
    if( size == 0 ) { error = "session_symbol_managed_callstack_empty"; return false; }
    size_t offset = 0;
    const auto count = data[offset++];
    if( count > 64 ) { error = "session_symbol_managed_callstack_too_deep"; return false; }
    std::vector<uint64_t> entries;
    entries.reserve( count + ( state.pendingCallstack ? state.callstacks[state.pendingCallstack].size() : 0 ) );
    for( uint8_t index = 0; index < count; ++index )
    {
        if( offset > size || size - offset < 6 )
        { error = "session_symbol_managed_frame_truncated"; return false; }
        FrameLine line;
        std::memcpy( &line.line, data + offset, 4 ); offset += 4;
        uint16_t nameBytes = 0; std::memcpy( &nameBytes, data + offset, 2 ); offset += 2;
        if( nameBytes > size - offset ) { error = "session_symbol_managed_name_truncated"; return false; }
        line.name.assign( reinterpret_cast<const char*>( data + offset ), nameBytes ); offset += nameBytes;
        if( size - offset < 2 ) { error = "session_symbol_managed_file_header_truncated"; return false; }
        uint16_t fileBytes = 0; std::memcpy( &fileBytes, data + offset, 2 ); offset += 2;
        if( fileBytes > size - offset ) { error = "session_symbol_managed_file_truncated"; return false; }
        line.file.assign( reinterpret_cast<const char*>( data + offset ), fileBytes ); offset += fileBytes;
        std::vector<uint8_t> key;
        Put32( key, line.line ); PutString( key, line.name ); PutString( key, line.file );
        const std::string keyString( reinterpret_cast<const char*>( key.data() ), key.size() );
        auto found = state.managedFrameIds.find( keyString );
        if( found == state.managedFrameIds.end() )
        {
            const auto token = CustomFrameMask | state.nextManagedFrameId++;
            found = state.managedFrameIds.emplace( keyString, token ).first;
            state.frames.emplace( token, FrameDefinition { token, {}, { std::move( line ) } } );
        }
        entries.emplace_back( found->second );
    }
    if( offset != size ) { error = "session_symbol_managed_callstack_trailing_bytes"; return false; }
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

bool VisitSymbolRecord( const TraceSessionCanonicalRecord& record,
    void* userData, std::string& error )
{
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ) return true;
    auto& state = *static_cast<BuildState*>( userData );
    QueueItem item {};
    if( !DecodeItem( record, item, error ) ) return false;
    const auto type = QueueType( record.type );
    switch( type )
    {
    case QueueType::SingleStringData:
    case QueueType::SecondStringData:
    {
        const uint8_t* data = nullptr; size_t size = 0;
        if( !GetShortPayload( record, data, size, error ) ) return false;
        auto value = std::string( reinterpret_cast<const char*>( data ), size );
        if( type == QueueType::SingleStringData ) state.singleString = std::move( value );
        else state.secondString = std::move( value );
        break;
    }
    case QueueType::CallstackPayload:
    {
        const uint8_t* data = nullptr; size_t size = 0;
        if( !GetShortPayload( record, data, size, error ) || state.pendingCallstack != 0 )
        { if( error.empty() ) error = "session_symbol_pending_callstack_conflict"; return false; }
        state.pendingCallstack = InternNativeCallstack( state, data, size, error );
        if( state.pendingCallstack == 0 ) return false;
        break;
    }
    case QueueType::CallstackSampleDictionary:
    {
        const uint8_t* data = nullptr; size_t size = 0;
        if( !GetShortPayload( record, data, size, error ) ||
            InternNativeCallstack( state, data, size, error ) == 0 ) return false;
        break;
    }
    case QueueType::CallstackAllocPayload:
    {
        const uint8_t* data = nullptr; size_t size = 0;
        if( !GetShortPayload( record, data, size, error ) ||
            !ParseManagedCallstack( state, data, size, error ) ) return false;
        break;
    }
    case QueueType::Callstack:
    case QueueType::CallstackSerial:
    case QueueType::CallstackAlloc:
    case QueueType::CallstackSample:
    case QueueType::CallstackSampleContextSwitch:
        if( state.pendingCallstack == 0 )
        { error = "session_symbol_callstack_consumer_without_payload"; return false; }
        state.pendingCallstack = 0;
        break;
    case QueueType::CallstackFrameSize:
    {
        if( state.staging || item.callstackFrameSize.size == 0 )
        { error = "session_symbol_frame_size_sequence_invalid"; return false; }
        FrameStaging staging;
        staging.address = item.callstackFrameSize.ptr;
        staging.expected = item.callstackFrameSize.size;
        if( state.singleString ) staging.image = *state.singleString;
        state.staging = std::move( staging );
        break;
    }
    case QueueType::CallstackFrame:
    {
        if( !state.staging || state.staging->lines.size() >= state.staging->expected )
        { error = "session_symbol_frame_without_size"; return false; }
        FrameLine line;
        if( state.singleString ) line.name = *state.singleString;
        if( state.secondString ) line.file = *state.secondString;
        line.line = item.callstackFrame.line;
        line.symbol = item.callstackFrame.symAddr;
        const auto frameIndex = state.staging->lines.size();
        if( line.symbol != 0 && state.symbols.find( line.symbol ) == state.symbols.end() &&
            state.pendingSymbols.find( line.symbol ) == state.pendingSymbols.end() )
        {
            state.pendingSymbols.emplace( line.symbol, PendingSymbol { line.name,
                state.staging->image, line.file, line.line, item.callstackFrame.symLen,
                frameIndex + 1 != state.staging->expected } );
        }
        state.staging->lines.emplace_back( std::move( line ) );
        if( state.staging->lines.size() == state.staging->expected )
        {
            auto definition = FrameDefinition { state.staging->address,
                std::move( state.staging->image ), std::move( state.staging->lines ) };
            const auto existing = state.frames.find( definition.address );
            if( existing == state.frames.end() ) state.frames.emplace( definition.address, std::move( definition ) );
            state.staging.reset();
        }
        break;
    }
    case QueueType::SymbolInformation:
    {
        const auto pending = state.pendingSymbols.find( item.symbolInformation.symAddr );
        if( pending == state.pendingSymbols.end() )
        { error = "session_symbol_information_without_frame"; return false; }
        SymbolDefinition symbol;
        symbol.address = item.symbolInformation.symAddr;
        symbol.size = pending->second.size;
        symbol.line = item.symbolInformation.line;
        symbol.callLine = pending->second.callLine;
        symbol.inlineFrame = pending->second.inlineFrame;
        symbol.name = pending->second.name;
        symbol.file = state.singleString.value_or( std::string {} );
        symbol.image = pending->second.image;
        symbol.callFile = pending->second.callFile;
        state.symbols.emplace( symbol.address, std::move( symbol ) );
        state.pendingSymbols.erase( pending );
        break;
    }
    case QueueType::SymbolCode:
    {
        const uint8_t* data = nullptr; size_t size = 0;
        if( !GetLargePayload( record, data, size, error ) ) return false;
        auto& bytes = state.code[item.stringTransfer.ptr];
        if( !bytes.empty() ) { error = "session_symbol_code_duplicate"; return false; }
        bytes.assign( data, data + size );
        break;
    }
    default: break;
    }
    return true;
}

bool SaveManifest( const std::filesystem::path& root,
    const SymbolManifest& value, std::string& error )
{
    const auto temporary = root / "manifest.tmp";
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_symbol_manifest_open_failed"; return false; }
    out << "magic " << ManifestMagic << '\n';
    out << "schema " << TraceSessionSymbolIndexSchemaVersion << '\n';
    out << "source_sha256 " << std::quoted( value.sourceSha256 ) << '\n';
    out << "source_size " << value.sourceSize << '\n';
    out << "generation " << std::quoted( value.generation ) << '\n';
    out << "metadata_file_bytes " << value.metadataFileBytes << '\n';
    out << "code_file_bytes " << value.codeFileBytes << '\n';
    out << "metadata_sha256 " << std::quoted( value.metadataSha256 ) << '\n';
    out << "code_sha256 " << std::quoted( value.codeSha256 ) << '\n';
    out << "callstacks " << value.stats.callstacks << '\n';
    out << "callstack_entries " << value.stats.callstackEntries << '\n';
    out << "frame_addresses " << value.stats.frameAddresses << '\n';
    out << "inline_frames " << value.stats.inlineFrames << '\n';
    out << "symbols " << value.stats.symbols << '\n';
    out << "symbol_code_bytes " << value.stats.symbolCodeBytes << '\n';
    out.flush();
    if( !out ) { error = "session_symbol_manifest_write_failed"; return false; }
    out.close();
    return AtomicReplace( temporary, root / "manifest", error );
}

bool LoadManifest( const std::filesystem::path& root,
    SymbolManifest& value, std::string& error )
{
    value = {};
    std::ifstream in( root / "manifest", std::ios::binary );
    if( !in ) { error = "session_symbol_manifest_not_found"; return false; }
    uint64_t magic = 0; uint32_t schema = 0; std::string key;
    while( in >> key )
    {
        if( key == "magic" ) in >> magic;
        else if( key == "schema" ) in >> schema;
        else if( key == "source_sha256" ) in >> std::quoted( value.sourceSha256 );
        else if( key == "source_size" ) in >> value.sourceSize;
        else if( key == "generation" ) in >> std::quoted( value.generation );
        else if( key == "metadata_file_bytes" ) in >> value.metadataFileBytes;
        else if( key == "code_file_bytes" ) in >> value.codeFileBytes;
        else if( key == "metadata_sha256" ) in >> std::quoted( value.metadataSha256 );
        else if( key == "code_sha256" ) in >> std::quoted( value.codeSha256 );
        else if( key == "callstacks" ) in >> value.stats.callstacks;
        else if( key == "callstack_entries" ) in >> value.stats.callstackEntries;
        else if( key == "frame_addresses" ) in >> value.stats.frameAddresses;
        else if( key == "inline_frames" ) in >> value.stats.inlineFrames;
        else if( key == "symbols" ) in >> value.stats.symbols;
        else if( key == "symbol_code_bytes" ) in >> value.stats.symbolCodeBytes;
        else { std::string ignored; std::getline( in, ignored ); }
        if( !in ) { error = "session_symbol_manifest_parse_failed"; return false; }
    }
    value.stats.fileBytes = value.metadataFileBytes + value.codeFileBytes;
    if( magic != ManifestMagic || schema != TraceSessionSymbolIndexSchemaVersion ||
        value.sourceSha256.size() != 64 || value.metadataSha256.size() != 64 ||
        value.codeSha256.size() != 64 )
    { error = "session_symbol_manifest_invalid"; return false; }
    return true;
}

bool VerifyFiles( const std::filesystem::path& root, const TraceSessionManifest& session,
    SymbolManifest& manifest, bool checksum, std::string& error )
{
    if( !LoadManifest( root, manifest, error ) ) return false;
    if( manifest.sourceSha256 != session.source.sha256 ||
        manifest.sourceSize != session.source.fileSize || manifest.generation != session.generation )
    { error = "session_symbol_identity_mismatch"; return false; }
    std::error_code ec;
    if( std::filesystem::file_size( root / MetadataFileName, ec ) != manifest.metadataFileBytes || ec )
    { error = "session_symbol_metadata_size_mismatch"; return false; }
    ec.clear();
    if( std::filesystem::file_size( root / CodeFileName, ec ) != manifest.codeFileBytes || ec )
    { error = "session_symbol_code_size_mismatch"; return false; }
    if( checksum && Sha256File( root / MetadataFileName ) != manifest.metadataSha256 )
    { error = "session_symbol_metadata_sha256_mismatch"; return false; }
    if( checksum && Sha256File( root / CodeFileName ) != manifest.codeSha256 )
    { error = "session_symbol_code_sha256_mismatch"; return false; }
    return true;
}

}

struct TraceSessionSymbolReader::Impl
{
    std::string fingerprint;
    std::filesystem::path codePath;
    std::vector<std::vector<uint64_t>> callstacks;
    std::unordered_map<uint64_t, FrameDefinition> frames;
    std::vector<SymbolDefinition> symbols;
    std::map<uint64_t, size_t> symbolByAddress;
    std::vector<std::pair<uint64_t, uint64_t>> mappings;
};

TraceSessionSymbolReader::TraceSessionSymbolReader( std::shared_ptr<Impl> impl )
    : m_impl( std::move( impl ) )
{}

std::filesystem::path TraceSessionSymbolIndexRoot( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest )
{
    return sessionRoot / "generations" / manifest.generation / "derived" /
        "source-symbol-index" / "1" / "exact";
}

bool BuildTraceSessionSymbolDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& session, TraceSessionSymbolStats& stats,
    std::string& error )
{
    error.clear(); stats = {};
    BuildState state;
    if( !VisitTraceSessionCanonicalOrdered( sessionRoot, session,
        VisitSymbolRecord, &state, error ) ) return false;
    if( state.pendingCallstack != 0 || state.staging )
    { error = "session_symbol_pending_protocol_state"; return false; }

    const auto root = TraceSessionSymbolIndexRoot( sessionRoot, session );
    std::error_code ec; std::filesystem::create_directories( root, ec );
    if( ec ) { error = "session_symbol_directory_failed:" + ec.message(); return false; }
    const auto codeTemporary = root / ( std::string( CodeFileName ) + ".tmp" );
    std::ofstream code( codeTemporary, std::ios::binary | std::ios::trunc );
    if( !code ) { error = "session_symbol_code_open_failed"; return false; }
    uint64_t codeOffset = 0;
    for( auto& [address, symbol] : state.symbols )
    {
        const auto found = state.code.find( address );
        if( found == state.code.end() ) continue;
        symbol.codeOffset = codeOffset;
        symbol.codeBytes = uint32_t( found->second.size() );
        code.write( reinterpret_cast<const char*>( found->second.data() ),
            std::streamsize( found->second.size() ) );
        codeOffset += found->second.size();
    }
    code.flush();
    if( !code ) { error = "session_symbol_code_write_failed"; return false; }
    code.close();
    if( !AtomicReplace( codeTemporary, root / CodeFileName, error ) ) return false;

    std::vector<uint8_t> metadata;
    Put64( metadata, MetadataMagic );
    Put32( metadata, TraceSessionSymbolIndexSchemaVersion ); Put32( metadata, 0 );
    Put64( metadata, session.source.fileSize );
    Put32( metadata, uint32_t( session.generation.size() ) ); Put32( metadata, 0 );
    metadata.insert( metadata.end(), session.source.sha256.begin(), session.source.sha256.end() );
    metadata.insert( metadata.end(), session.generation.begin(), session.generation.end() );
    Put64( metadata, state.callstacks.size() - 1 );
    for( size_t id = 1; id < state.callstacks.size(); ++id )
    {
        Put32( metadata, uint32_t( state.callstacks[id].size() ) );
        for( const auto entry : state.callstacks[id] ) Put64( metadata, entry );
        stats.callstackEntries += state.callstacks[id].size();
    }
    std::vector<uint64_t> frameAddresses;
    frameAddresses.reserve( state.frames.size() );
    for( const auto& [address, frame] : state.frames ) frameAddresses.emplace_back( address );
    std::sort( frameAddresses.begin(), frameAddresses.end() );
    Put64( metadata, frameAddresses.size() );
    for( const auto address : frameAddresses )
    {
        const auto& frame = state.frames.at( address );
        Put64( metadata, address ); PutString( metadata, frame.image );
        Put32( metadata, uint32_t( frame.lines.size() ) );
        for( const auto& line : frame.lines )
        {
            PutString( metadata, line.name ); PutString( metadata, line.file );
            Put32( metadata, line.line ); Put64( metadata, line.symbol );
        }
        stats.inlineFrames += frame.lines.size();
    }
    Put64( metadata, state.symbols.size() );
    for( const auto& [address, symbol] : state.symbols )
    {
        Put64( metadata, symbol.address ); Put64( metadata, symbol.codeOffset );
        Put32( metadata, symbol.codeBytes ); Put32( metadata, symbol.size );
        Put32( metadata, symbol.line ); Put32( metadata, symbol.callLine );
        Put32( metadata, symbol.inlineFrame ? 1 : 0 );
        PutString( metadata, symbol.name ); PutString( metadata, symbol.file );
        PutString( metadata, symbol.image ); PutString( metadata, symbol.callFile );
    }
    const auto metadataTemporary = root / ( std::string( MetadataFileName ) + ".tmp" );
    std::ofstream out( metadataTemporary, std::ios::binary | std::ios::trunc );
    out.write( reinterpret_cast<const char*>( metadata.data() ), std::streamsize( metadata.size() ) );
    out.flush();
    if( !out ) { error = "session_symbol_metadata_write_failed"; return false; }
    out.close();
    if( !AtomicReplace( metadataTemporary, root / MetadataFileName, error ) ) return false;

    SymbolManifest manifest;
    manifest.sourceSha256 = session.source.sha256;
    manifest.sourceSize = session.source.fileSize;
    manifest.generation = session.generation;
    manifest.metadataFileBytes = metadata.size();
    manifest.codeFileBytes = codeOffset;
    manifest.metadataSha256 = Sha256File( root / MetadataFileName );
    manifest.codeSha256 = Sha256File( root / CodeFileName );
    stats.callstacks = state.callstacks.size() - 1;
    stats.frameAddresses = state.frames.size();
    stats.symbols = state.symbols.size();
    stats.symbolCodeBytes = codeOffset;
    stats.fileBytes = metadata.size() + codeOffset;
    manifest.stats = stats;
    return SaveManifest( root, manifest, error );
}

bool AuditTraceSessionSymbolDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& session, TraceSessionSymbolStats& stats,
    std::string& error )
{
    error.clear(); stats = {};
    SymbolManifest manifest;
    if( !VerifyFiles( TraceSessionSymbolIndexRoot( sessionRoot, session ),
        session, manifest, true, error ) ) return false;
    stats = manifest.stats;
    return true;
}

std::shared_ptr<TraceSessionSymbolReader> TraceSessionSymbolReader::Open(
    const std::filesystem::path& sessionRoot, const TraceSessionManifest& session,
    std::string& error )
{
    error.clear();
    const auto root = TraceSessionSymbolIndexRoot( sessionRoot, session );
    SymbolManifest manifest;
    if( !VerifyFiles( root, session, manifest, false, error ) ) return {};
    std::ifstream input( root / MetadataFileName, std::ios::binary );
    std::vector<uint8_t> bytes( size_t( manifest.metadataFileBytes ) );
    if( !bytes.empty() && !input.read( reinterpret_cast<char*>( bytes.data() ),
        std::streamsize( bytes.size() ) ) )
    { error = "session_symbol_metadata_read_failed"; return {}; }
    size_t offset = 0; uint64_t magic = 0, sourceSize = 0;
    uint32_t schema = 0, reserved = 0, generationBytes = 0, reserved2 = 0;
    if( !Get64( bytes, offset, magic ) || !Get32( bytes, offset, schema ) ||
        !Get32( bytes, offset, reserved ) || !Get64( bytes, offset, sourceSize ) ||
        !Get32( bytes, offset, generationBytes ) || !Get32( bytes, offset, reserved2 ) ||
        magic != MetadataMagic || schema != TraceSessionSymbolIndexSchemaVersion ||
        reserved != 0 || reserved2 != 0 || sourceSize != session.source.fileSize ||
        offset > bytes.size() || 64 > bytes.size() - offset ||
        generationBytes > bytes.size() - offset - 64 )
    { error = "session_symbol_metadata_header_invalid"; return {}; }
    const std::string sha( reinterpret_cast<const char*>( bytes.data() + offset ), 64 ); offset += 64;
    const std::string generation( reinterpret_cast<const char*>( bytes.data() + offset ), generationBytes ); offset += generationBytes;
    if( sha != session.source.sha256 || generation != session.generation )
    { error = "session_symbol_metadata_identity_invalid"; return {}; }
    auto impl = std::make_shared<Impl>();
    impl->fingerprint = session.source.sha256;
    impl->codePath = root / CodeFileName;
    uint64_t count = 0;
    if( !Get64( bytes, offset, count ) || count > std::numeric_limits<uint32_t>::max() )
    { error = "session_symbol_callstack_count_invalid"; return {}; }
    impl->callstacks.resize( size_t( count + 1 ) );
    for( uint64_t id = 1; id <= count; ++id )
    {
        uint32_t entries = 0;
        if( !Get32( bytes, offset, entries ) || entries > 65535 ||
            offset > bytes.size() || uint64_t( entries ) * 8 > bytes.size() - offset )
        { error = "session_symbol_callstack_record_invalid"; return {}; }
        auto& stack = impl->callstacks[size_t( id )]; stack.resize( entries );
        for( auto& entry : stack ) if( !Get64( bytes, offset, entry ) )
        { error = "session_symbol_callstack_record_truncated"; return {}; }
    }
    if( !Get64( bytes, offset, count ) || count > std::numeric_limits<uint32_t>::max() )
    { error = "session_symbol_frame_count_invalid"; return {}; }
    for( uint64_t i = 0; i < count; ++i )
    {
        FrameDefinition frame; uint32_t lines = 0;
        if( !Get64( bytes, offset, frame.address ) ||
            !GetString( bytes, offset, frame.image, error ) ||
            !Get32( bytes, offset, lines ) || lines == 0 || lines > 255 )
        { if( error.empty() ) error = "session_symbol_frame_record_invalid"; return {}; }
        frame.lines.resize( lines );
        for( auto& line : frame.lines )
            if( !GetString( bytes, offset, line.name, error ) ||
                !GetString( bytes, offset, line.file, error ) ||
                !Get32( bytes, offset, line.line ) || !Get64( bytes, offset, line.symbol ) ) return {};
        if( !impl->frames.emplace( frame.address, std::move( frame ) ).second )
        { error = "session_symbol_frame_address_duplicate"; return {}; }
    }
    if( !Get64( bytes, offset, count ) || count > std::numeric_limits<uint32_t>::max() )
    { error = "session_symbol_count_invalid"; return {}; }
    impl->symbols.reserve( size_t( count ) );
    for( uint64_t i = 0; i < count; ++i )
    {
        SymbolDefinition symbol; uint32_t flags = 0;
        if( !Get64( bytes, offset, symbol.address ) || !Get64( bytes, offset, symbol.codeOffset ) ||
            !Get32( bytes, offset, symbol.codeBytes ) || !Get32( bytes, offset, symbol.size ) ||
            !Get32( bytes, offset, symbol.line ) || !Get32( bytes, offset, symbol.callLine ) ||
            !Get32( bytes, offset, flags ) || !GetString( bytes, offset, symbol.name, error ) ||
            !GetString( bytes, offset, symbol.file, error ) || !GetString( bytes, offset, symbol.image, error ) ||
            !GetString( bytes, offset, symbol.callFile, error ) ||
            symbol.codeOffset > manifest.codeFileBytes || symbol.codeBytes > manifest.codeFileBytes - symbol.codeOffset )
        { if( error.empty() ) error = "session_symbol_record_invalid"; return {}; }
        symbol.inlineFrame = ( flags & 1 ) != 0;
        impl->symbolByAddress.emplace( symbol.address, impl->symbols.size() );
        impl->symbols.emplace_back( std::move( symbol ) );
    }
    if( offset != bytes.size() ) { error = "session_symbol_metadata_trailing_bytes"; return {}; }
    for( const auto& [address, frame] : impl->frames )
        if( !frame.lines.empty() && frame.lines.front().symbol != 0 )
            impl->mappings.emplace_back( address, frame.lines.front().symbol );
    std::sort( impl->mappings.begin(), impl->mappings.end() );
    auto reader = std::shared_ptr<TraceSessionSymbolReader>(
        new TraceSessionSymbolReader( std::move( impl ) ) );
    reader->m_stats = manifest.stats;
    return reader;
}

std::vector<CallstackFrameDto> TraceSessionSymbolReader::ResolveCallstacks(
    const std::vector<uint32_t>& callstacks, size_t maxDepth ) const
{
    std::vector<CallstackFrameDto> result;
    for( const auto id : callstacks )
    {
        if( id == 0 || id >= m_impl->callstacks.size() ) continue;
        size_t depth = 0;
        for( const auto address : m_impl->callstacks[id] )
        {
            if( depth >= maxDepth ) break;
            const auto found = m_impl->frames.find( address );
            if( found == m_impl->frames.end() )
            {
                result.push_back( { MakeRef( m_impl->fingerprint, "callstack-frame",
                    ( uint64_t( id ) << 32 ) | depth ), {}, {}, 0, Hex( address ),
                    "0x0", false, id, depth } );
                ++depth; continue;
            }
            const auto& frame = found->second;
            const std::optional<std::string> image = frame.image.empty() ?
                std::nullopt : std::optional<std::string>( frame.image );
            for( size_t lineIndex = 0; lineIndex < frame.lines.size() && depth < maxDepth; ++lineIndex )
            {
                const auto& line = frame.lines[lineIndex];
                result.push_back( { MakeRef( m_impl->fingerprint, "callstack-frame",
                    ( uint64_t( id ) << 32 ) | depth ), line.name, line.file, line.line,
                    Hex( address ), Hex( line.symbol ), lineIndex + 1 != frame.lines.size(),
                    id, depth, image } );
                ++depth;
            }
        }
    }
    return result;
}

std::vector<SymbolDto> TraceSessionSymbolReader::Symbols() const
{
    std::vector<SymbolDto> result;
    result.reserve( m_impl->symbols.size() );
    for( const auto& symbol : m_impl->symbols )
    {
        SymbolDto dto;
        dto.ref = MakeRef( m_impl->fingerprint, "symbol", symbol.address );
        dto.address = Hex( symbol.address ); dto.name = symbol.name; dto.file = symbol.file;
        dto.line = symbol.line; dto.size = symbol.size; dto.hasCode = symbol.codeBytes != 0;
        if( !symbol.image.empty() ) dto.imageName = symbol.image;
        if( !symbol.callFile.empty() ) dto.callFile = symbol.callFile;
        dto.callLine = symbol.callLine; dto.inlineFrame = symbol.inlineFrame;
        result.emplace_back( std::move( dto ) );
    }
    return result;
}

std::vector<SymbolAddressMappingDto> TraceSessionSymbolReader::AddressMappings(
    size_t offset, size_t limit ) const
{
    std::vector<SymbolAddressMappingDto> result;
    const auto begin = std::min( offset, m_impl->mappings.size() );
    const auto end = begin + std::min( limit, m_impl->mappings.size() - begin );
    for( size_t index = begin; index < end; ++index )
    {
        const auto [address, symbol] = m_impl->mappings[index];
        const auto found = m_impl->symbolByAddress.find( symbol );
        const bool inlined = found != m_impl->symbolByAddress.end() &&
            m_impl->symbols[found->second].inlineFrame;
        result.push_back( { MakeRef( m_impl->fingerprint, "symbol-address", address ),
            Hex( address ), MakeRef( m_impl->fingerprint, "symbol", symbol ), Hex( symbol ),
            address >= symbol ? uint32_t( std::min<uint64_t>( address - symbol,
                std::numeric_limits<uint32_t>::max() ) ) : 0, inlined } );
    }
    return result;
}

std::optional<SymbolAddressMappingDto> TraceSessionSymbolReader::ResolveAddress(
    uint64_t address ) const
{
    const auto exact = std::lower_bound( m_impl->mappings.begin(), m_impl->mappings.end(),
        std::pair<uint64_t, uint64_t> { address, 0 } );
    if( exact != m_impl->mappings.end() && exact->first == address )
        return AddressMappings( size_t( exact - m_impl->mappings.begin() ), 1 ).front();
    const auto symbol = m_impl->symbolByAddress.upper_bound( address );
    if( symbol == m_impl->symbolByAddress.begin() ) return std::nullopt;
    const auto previous = std::prev( symbol );
    const auto& data = m_impl->symbols[previous->second];
    if( data.size == 0 || address - data.address >= data.size ) return std::nullopt;
    return SymbolAddressMappingDto { MakeRef( m_impl->fingerprint, "symbol-address", address ),
        Hex( address ), MakeRef( m_impl->fingerprint, "symbol", data.address ), Hex( data.address ),
        uint32_t( address - data.address ), data.inlineFrame };
}

std::vector<SymbolResourceDto> TraceSessionSymbolReader::SymbolResources() const
{
    std::vector<SymbolResourceDto> result;
    for( const auto& symbol : m_impl->symbols ) result.push_back( { symbol.address,
        MakeRef( m_impl->fingerprint, "symbol", symbol.address ), symbol.name,
        symbol.file, symbol.line, symbol.codeBytes } );
    return result;
}

BinaryResourceChunkDto TraceSessionSymbolReader::ReadSymbolCodeBytes(
    uint64_t symbolId, size_t offset, size_t maxBytes ) const
{
    const auto found = m_impl->symbolByAddress.find( symbolId );
    if( found == m_impl->symbolByAddress.end() || m_impl->symbols[found->second].codeBytes == 0 )
        throw std::out_of_range( "symbol code resource was not found" );
    const auto& symbol = m_impl->symbols[found->second];
    const auto begin = std::min<uint64_t>( offset, symbol.codeBytes );
    const auto bytes = std::min<uint64_t>( maxBytes, symbol.codeBytes - begin );
    BinaryResourceChunkDto result;
    result.ref = MakeRef( m_impl->fingerprint, "symbol", symbolId );
    result.offset = begin; result.totalBytes = symbol.codeBytes;
    result.bytes.resize( size_t( bytes ) ); result.eof = begin + bytes == symbol.codeBytes;
    std::ifstream input( m_impl->codePath, std::ios::binary );
    input.seekg( std::streamoff( symbol.codeOffset + begin ) );
    if( bytes != 0 && !input.read( reinterpret_cast<char*>( result.bytes.data() ),
        std::streamsize( bytes ) ) ) throw std::runtime_error( "symbol code resource read failed" );
    return result;
}

SymbolCodeDto TraceSessionSymbolReader::ReadSymbolCode(
    uint64_t symbolId, size_t maxBytes ) const
{
    auto chunk = ReadSymbolCodeBytes( symbolId, 0, maxBytes );
    return { chunk.ref, Hex( symbolId ), std::move( chunk.bytes ), !chunk.eof };
}

}

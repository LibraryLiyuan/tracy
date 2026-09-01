#include "TracyTraceSessionGpuZones.hpp"

#include "TracyHash.hpp"
#include "TracyTraceSessionCanonical.hpp"
#include "TracyTraceSessionGpuCanonical.hpp"
#include "TracyProtocol.hpp"
#include "TracyQueue.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <limits>
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

constexpr uint64_t GpuZoneFileMagic = 0x315a5047534e4aull; // JNSGPZ1
constexpr uint64_t GpuZoneManifestMagic = 0x314d5a47534e4aull; // JNSGZM1
constexpr const char* GpuZoneFileName = "gpu-zones.bin";
constexpr uint64_t InvalidZoneId = std::numeric_limits<uint64_t>::max();

enum StoredZoneFlags : uint32_t
{
    ZoneCpuComplete = 1u << 0,
    ZoneGpuStartValid = 1u << 1,
    ZoneGpuEndValid = 1u << 2,
    ZoneDurationAccounted = 1u << 3,
    ZoneFullyCounted = 1u << 4
};

#pragma pack( push, 1 )
struct GpuZoneFileHeader
{
    uint64_t magic = GpuZoneFileMagic;
    uint32_t schema = TraceSessionGpuZoneIndexSchemaVersion;
    uint32_t endian = 0x01020304;
    uint64_t sourceSize = 0;
    uint64_t zoneCount = 0;
    uint64_t completeZoneCount = 0;
    uint64_t contextCount = 0;
    uint64_t sourceCount = 0;
    uint64_t zoneBlockCount = 0;
    uint64_t zonesOffset = 0;
    uint64_t zoneBlocksOffset = 0;
    uint64_t contextsOffset = 0;
    uint64_t sourcesOffset = 0;
    uint32_t generationBytes = 0;
    uint32_t reserved = 0;
};

struct StoredZone
{
    uint64_t id = 0;
    uint64_t parent = InvalidZoneId;
    uint64_t thread = 0;
    uint32_t context = 0;
    int32_t sourceNativeId = 0;
    int64_t gpuStartNs = -1;
    int64_t gpuEndNs = -1;
    int64_t cpuStartNs = 0;
    int64_t cpuEndNs = -1;
    int64_t childGpuTimeNs = 0;
    int64_t selfTimeNs = 0;
    uint32_t callstack = 0;
    uint32_t callsiteId = 0;
    uint32_t childCount = 0;
    uint32_t flags = 0;
    uint16_t queryId = 0;
    uint8_t provenance = uint8_t( JnStackProvenance::Unavailable );
    uint8_t unavailableReason = 0;
};

struct StoredContext
{
    uint32_t index = 0;
    uint32_t wireId = 0;
    uint64_t thread = 0;
    uint64_t zoneCount = 0;
    double period = 1.0;
    uint64_t overflow = 0;
    uint32_t nameBytes = 0;
    uint8_t calibrated = 0;
    uint8_t type = 0;
    uint16_t reserved = 0;
};

struct StoredSource
{
    int32_t nativeId = 0;
    uint32_t line = 0;
    uint32_t nameBytes = 0;
    uint32_t functionBytes = 0;
    uint32_t fileBytes = 0;
    uint32_t flags = 0;
};

struct StoredZoneBlock
{
    uint64_t firstZone = 0;
    uint32_t zoneCount = 0;
    uint32_t reserved = 0;
    int64_t minStartNs = 0;
    int64_t maxEndNs = 0;
    uint64_t contextBloom[4] {};
};
#pragma pack( pop )

void AddContext( StoredZoneBlock& block, uint32_t context );
bool MayContainContext( const StoredZoneBlock& block, uint32_t context );

struct GpuZoneManifest
{
    std::string sourceSha256;
    uint64_t sourceSize = 0;
    std::string generation;
    uint64_t fileBytes = 0;
    std::string fileSha256;
    TraceSessionGpuZoneStats stats;
};

struct SourceState
{
    int32_t nativeId = 0;
    uint64_t pointer = 0, namePointer = 0, functionPointer = 0, filePointer = 0;
    uint32_t line = 0;
    bool dynamic = false, definitionReceived = false;
    std::string name, function, file;
};

struct CallsiteState
{
    uint32_t callstack = 0;
    uint8_t provenance = uint8_t( JnStackProvenance::Unavailable );
    uint8_t unavailableReason = 0;
};

struct ContextState
{
    uint32_t index = 0;
    uint8_t wireId = 0;
    uint64_t thread = 0;
    double period = 1.0;
    uint8_t type = 0;
    bool hasPeriod = false, calibrated = false;
    int64_t timeDiff = 0, calibratedGpuTime = 0, calibratedCpuTime = 0;
    double calibrationMod = 1.0;
    int64_t lastGpuTime = 0;
    uint64_t overflow = 0, overflowMul = 0, zoneCount = 0;
    std::string name;
};

struct StackKey
{
    uint32_t context = 0;
    uint64_t thread = 0;
    bool operator==( const StackKey& other ) const { return context == other.context && thread == other.thread; }
};

struct StackKeyHash
{
    size_t operator()( const StackKey& value ) const
    {
        return std::hash<uint64_t>()( ( uint64_t( value.context ) << 32 ) ^ value.thread );
    }
};

struct OpenZone { StoredZone stored; };
struct PendingQuery { uint64_t zone = 0; bool start = false; };

bool AtomicReplace( const std::filesystem::path& source,
    const std::filesystem::path& target, std::string& error )
{
#ifdef _WIN32
    if( MoveFileExW( source.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) ) return true;
    error = "session_gpu_zone_atomic_replace_failed:" + std::to_string( GetLastError() );
    return false;
#else
    std::error_code ec; std::filesystem::rename( source, target, ec );
    if( !ec ) return true;
    error = "session_gpu_zone_atomic_replace_failed:" + ec.message(); return false;
#endif
}

bool DecodeItem( const TraceSessionCanonicalRecord& record, QueueItem& item, std::string& error )
{
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ||
        record.type >= uint8_t( QueueType::NUM_TYPES ) || record.payload.size() < QueueDataSize[record.type] )
    { error = "session_gpu_zone_protocol_record_invalid"; return false; }
    item = {};
    std::memcpy( &item, record.payload.data(), std::min<size_t>( record.payload.size(), sizeof( item ) ) );
    if( item.hdr.idx != record.type ) { error = "session_gpu_zone_protocol_type_mismatch"; return false; }
    return true;
}

bool GetShortPayload( const TraceSessionCanonicalRecord& record,
    const uint8_t*& data, size_t& size, std::string& error )
{
    const auto fixed = size_t( QueueDataSize[record.type] );
    if( record.payload.size() < fixed + sizeof( uint16_t ) )
    { error = "session_gpu_zone_payload_truncated"; return false; }
    uint16_t bytes = 0; std::memcpy( &bytes, record.payload.data() + fixed, sizeof( bytes ) );
    if( bytes != record.variablePayloadBytes || record.payload.size() != fixed + sizeof( bytes ) + bytes )
    { error = "session_gpu_zone_payload_mismatch"; return false; }
    data = record.payload.data() + fixed + sizeof( bytes ); size = bytes; return true;
}

class GpuZoneWriter
{
public:
    bool Open( const std::filesystem::path& root, const TraceSessionManifest& session, std::string& error )
    {
        m_root = root; m_session = &session;
        std::error_code ec; std::filesystem::create_directories( root, ec );
        if( ec ) { error = "session_gpu_zone_directory_failed:" + ec.message(); return false; }
        m_workPath = root / "zones.work";
        m_file.open( m_workPath, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc );
        if( !m_file ) { error = "session_gpu_zone_work_open_failed"; return false; }
        return true;
    }

    bool Write( const StoredZone& zone, std::string& error )
    {
        m_file.clear(); m_file.seekp( std::streamoff( zone.id * sizeof( StoredZone ) ) );
        m_file.write( reinterpret_cast<const char*>( &zone ), sizeof( zone ) );
        if( !m_file ) { error = "session_gpu_zone_record_write_failed"; return false; }
        return true;
    }

    bool Read( uint64_t id, StoredZone& zone, std::string& error )
    {
        m_file.flush(); m_file.clear(); m_file.seekg( std::streamoff( id * sizeof( StoredZone ) ) );
        if( !m_file.read( reinterpret_cast<char*>( &zone ), sizeof( zone ) ) || zone.id != id )
        { error = "session_gpu_zone_record_missing"; return false; }
        return true;
    }

    bool Patch( const StoredZone& zone, std::string& error ) { return Write( zone, error ); }

    bool Finalize( const std::vector<ContextState>& contexts, const std::vector<SourceState>& sources,
        uint64_t zones, uint64_t complete, GpuZoneManifest& manifest, std::string& error )
    {
        m_file.flush(); if( !m_file ) { error = "session_gpu_zone_work_flush_failed"; return false; } m_file.close();
        constexpr uint32_t ZonesPerBlock = 4096;
        std::vector<StoredZoneBlock> blocks;
        blocks.reserve( size_t( ( zones + ZonesPerBlock - 1 ) / ZonesPerBlock ) );
        std::ifstream zonesIn( m_workPath, std::ios::binary );
        if( !zonesIn ) { error = "session_gpu_zone_work_read_failed"; return false; }
        for( uint64_t first = 0; first < zones; first += ZonesPerBlock )
        {
            StoredZoneBlock block;
            block.firstZone = first;
            block.zoneCount = uint32_t( std::min<uint64_t>( ZonesPerBlock, zones - first ) );
            block.minStartNs = std::numeric_limits<int64_t>::max();
            block.maxEndNs = std::numeric_limits<int64_t>::min();
            for( uint32_t index = 0; index < block.zoneCount; ++index )
            {
                StoredZone zone;
                if( !zonesIn.read( reinterpret_cast<char*>( &zone ), sizeof( zone ) ) ||
                    zone.id != first + index )
                { error = "session_gpu_zone_block_source_invalid"; return false; }
                const auto start = ( zone.flags & ZoneGpuStartValid ) != 0 ? zone.gpuStartNs : zone.cpuStartNs;
                const auto end = ( zone.flags & ZoneGpuEndValid ) != 0 ? zone.gpuEndNs : start;
                block.minStartNs = std::min( block.minStartNs, std::min( start, end ) );
                block.maxEndNs = std::max( block.maxEndNs, std::max( start, end ) );
                AddContext( block, zone.context );
            }
            blocks.emplace_back( block );
        }
        if( zonesIn.peek() != std::char_traits<char>::eof() )
        { error = "session_gpu_zone_block_source_trailing_bytes"; return false; }
        zonesIn.clear(); zonesIn.seekg( 0 );
        const auto target = m_root / GpuZoneFileName; auto temporary = target; temporary += ".tmp";
        std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
        if( !out ) { error = "session_gpu_zone_file_open_failed"; return false; }
        GpuZoneFileHeader header;
        header.sourceSize = m_session->source.fileSize; header.zoneCount = zones;
        header.completeZoneCount = complete; header.contextCount = contexts.size(); header.sourceCount = sources.size();
        header.zoneBlockCount = blocks.size();
        header.generationBytes = uint32_t( m_session->generation.size() );
        header.zonesOffset = sizeof( header ) + m_session->source.sha256.size() + m_session->generation.size();
        header.zoneBlocksOffset = header.zonesOffset + zones * sizeof( StoredZone );
        header.contextsOffset = header.zoneBlocksOffset + blocks.size() * sizeof( StoredZoneBlock );
        uint64_t contextBytes = 0;
        for( const auto& context : contexts ) contextBytes += sizeof( StoredContext ) + context.name.size();
        header.sourcesOffset = header.contextsOffset + contextBytes;
        out.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
        out.write( m_session->source.sha256.data(), std::streamsize( m_session->source.sha256.size() ) );
        out.write( m_session->generation.data(), std::streamsize( m_session->generation.size() ) );
        std::vector<char> buffer( 1024 * 1024 );
        while( zonesIn ) { zonesIn.read( buffer.data(), std::streamsize( buffer.size() ) ); const auto n = zonesIn.gcount(); if( n > 0 ) out.write( buffer.data(), n ); }
        if( !zonesIn.eof() ) { error = "session_gpu_zone_copy_failed"; return false; }
        zonesIn.close();
        if( !blocks.empty() ) out.write( reinterpret_cast<const char*>( blocks.data() ),
            std::streamsize( blocks.size() * sizeof( StoredZoneBlock ) ) );
        if( !out ) { error = "session_gpu_zone_block_write_failed"; return false; }
        for( const auto& context : contexts )
        {
            StoredContext stored;
            stored.index = context.index; stored.wireId = context.wireId; stored.thread = context.thread;
            stored.zoneCount = context.zoneCount; stored.period = context.period; stored.overflow = context.overflow;
            stored.nameBytes = uint32_t( context.name.size() ); stored.calibrated = context.calibrated; stored.type = context.type;
            out.write( reinterpret_cast<const char*>( &stored ), sizeof( stored ) );
            out.write( context.name.data(), std::streamsize( context.name.size() ) );
        }
        for( const auto& source : sources )
        {
            StoredSource stored;
            stored.nativeId = source.nativeId; stored.line = source.line;
            stored.nameBytes = uint32_t( source.name.size() ); stored.functionBytes = uint32_t( source.function.size() );
            stored.fileBytes = uint32_t( source.file.size() );
            stored.flags = ( source.dynamic ? 1u : 0u ) | ( source.definitionReceived ? 2u : 0u );
            out.write( reinterpret_cast<const char*>( &stored ), sizeof( stored ) );
            out.write( source.name.data(), std::streamsize( source.name.size() ) );
            out.write( source.function.data(), std::streamsize( source.function.size() ) );
            out.write( source.file.data(), std::streamsize( source.file.size() ) );
        }
        out.flush(); if( !out ) { error = "session_gpu_zone_file_write_failed"; return false; } out.close();
        if( !AtomicReplace( temporary, target, error ) ) return false;
        std::error_code ec;
        if( !std::filesystem::remove( m_workPath, ec ) || ec )
        { error = "session_gpu_zone_work_cleanup_failed:" + ( ec ? ec.message() : m_workPath.string() ); return false; }
        manifest.sourceSha256 = m_session->source.sha256; manifest.sourceSize = m_session->source.fileSize;
        manifest.generation = m_session->generation; manifest.fileBytes = std::filesystem::file_size( target, ec );
        if( ec ) { error = "session_gpu_zone_file_size_failed:" + ec.message(); return false; }
        manifest.fileSha256 = Sha256File( target ); manifest.stats.contexts = contexts.size();
        manifest.stats.zones = zones; manifest.stats.zoneBlocks = blocks.size(); manifest.stats.completeZones = complete;
        manifest.stats.sourceLocations = sources.size(); manifest.stats.fileBytes = manifest.fileBytes;
        return true;
    }

private:
    std::filesystem::path m_root, m_workPath;
    const TraceSessionManifest* m_session = nullptr;
    std::fstream m_file;
};

struct BuildState
{
    TraceSessionTimeTransform transform;
    GpuZoneWriter writer;
    std::unordered_map<uint64_t, std::string> strings;
    std::unordered_map<uint64_t, size_t> staticSources;
    std::unordered_map<std::string, size_t> dynamicSources;
    std::deque<size_t> sourceResponseQueue;
    std::vector<SourceState> sources;
    std::unordered_map<uint32_t, CallsiteState> callsites;
    std::unordered_map<uint32_t, std::vector<uint64_t>> pendingCallsiteZones;
    std::unordered_map<std::string, uint32_t> callstackIds;
    std::unordered_map<uint64_t, uint32_t> nextCallstack;
    uint32_t pendingCallstack = 0, serialNextCallstack = 0;
    std::optional<size_t> pendingDynamicSource;
    std::optional<std::string> pendingSingleString;
    std::unordered_map<uint8_t, uint32_t> contextByWire;
    std::vector<ContextState> contexts;
    std::unordered_map<StackKey, std::vector<OpenZone>, StackKeyHash> open;
    std::unordered_map<uint64_t, PendingQuery> pendingQueries;
    uint32_t threadContext = 0;
    int64_t refTimeThread = 0, refTimeSerial = 0, refTimeGpu = 0;
    uint64_t zoneCount = 0, completeZones = 0, beginEvents = 0, endEvents = 0, gpuTimeEvents = 0;
    uint64_t calibrationEvents = 0, syncEvents = 0;
};

uint64_t QueryKey( uint8_t context, uint16_t query ) { return ( uint64_t( context ) << 16 ) | query; }
int64_t AdvanceThread( BuildState& state, int64_t delta ) { state.refTimeThread += delta; return state.transform.ToNanoseconds( state.refTimeThread ); }
int64_t AdvanceSerial( BuildState& state, int64_t delta ) { state.refTimeSerial += delta; return state.transform.ToNanoseconds( state.refTimeSerial ); }

uint32_t InternCallstack( BuildState& state, const uint8_t* data, size_t size, std::string& error )
{
    if( size % sizeof( uint64_t ) != 0 ) { error = "session_gpu_zone_callstack_payload_invalid"; return 0; }
    std::string key( reinterpret_cast<const char*>( data ), size );
    if( const auto found = state.callstackIds.find( key ); found != state.callstackIds.end() ) return found->second;
    if( state.callstackIds.size() >= std::numeric_limits<uint32_t>::max() - 1 )
    { error = "session_gpu_zone_callstack_id_overflow"; return 0; }
    const auto id = uint32_t( state.callstackIds.size() + 1 ); state.callstackIds.emplace( std::move( key ), id ); return id;
}

size_t EnsureStaticSource( BuildState& state, uint64_t pointer )
{
    if( const auto found = state.staticSources.find( pointer ); found != state.staticSources.end() ) return found->second;
    SourceState source; source.nativeId = int32_t( state.staticSources.size() ); source.pointer = pointer;
    const auto index = state.sources.size(); state.sources.emplace_back( std::move( source ) );
    state.staticSources.emplace( pointer, index ); state.sourceResponseQueue.push_back( index ); return index;
}

bool ParseDynamicSource( BuildState& state, const uint8_t* data, size_t size, std::string& error )
{
    if( size < 10 ) { error = "session_gpu_zone_dynamic_source_truncated"; return false; }
    std::string key( reinterpret_cast<const char*>( data ), size );
    if( const auto found = state.dynamicSources.find( key ); found != state.dynamicSources.end() )
    { state.pendingDynamicSource = found->second; return true; }
    uint32_t color = 0, line = 0; std::memcpy( &color, data, 4 ); std::memcpy( &line, data + 4, 4 );
    size_t offset = 8; const auto takeZ = [&]( std::string& value ) -> bool {
        const auto begin = offset; while( offset < size && data[offset] != 0 ) ++offset;
        if( offset >= size ) return false; value.assign( reinterpret_cast<const char*>( data + begin ), offset - begin ); ++offset; return true;
    };
    SourceState source; source.dynamic = true; source.nativeId = -int32_t( state.dynamicSources.size() + 1 ); source.line = line;
    if( !takeZ( source.function ) || !takeZ( source.file ) ) { error = "session_gpu_zone_dynamic_source_invalid"; return false; }
    source.name.assign( reinterpret_cast<const char*>( data + offset ), size - offset ); source.definitionReceived = true;
    const auto index = state.sources.size(); state.sources.emplace_back( std::move( source ) );
    state.dynamicSources.emplace( std::move( key ), index ); state.pendingDynamicSource = index; return true;
}

ContextState* GetContext( BuildState& state, uint8_t wire, std::string& error )
{
    const auto found = state.contextByWire.find( wire );
    if( found == state.contextByWire.end() ) { error = "session_gpu_zone_context_missing"; return nullptr; }
    return &state.contexts[found->second];
}

OpenZone* FindOpen( BuildState& state, uint64_t id )
{
    for( auto& [key, stack] : state.open )
        for( auto& zone : stack ) if( zone.stored.id == id ) return &zone;
    return nullptr;
}

bool AddChildTime( BuildState& state, uint64_t parent, int64_t duration, std::string& error )
{
    if( parent == InvalidZoneId ) return true;
    if( auto* open = FindOpen( state, parent ) ) { open->stored.childGpuTimeNs += duration; return true; }
    StoredZone stored; if( !state.writer.Read( parent, stored, error ) ) return false;
    stored.childGpuTimeNs += duration;
    if( stored.flags & ZoneGpuEndValid ) stored.selfTimeNs = std::max<int64_t>( 0, stored.gpuEndNs - stored.gpuStartNs - stored.childGpuTimeNs );
    return state.writer.Patch( stored, error );
}

bool AccountDuration( BuildState& state, StoredZone& zone, std::string& error )
{
    if( ( zone.flags & ( ZoneGpuStartValid | ZoneGpuEndValid ) ) != ( ZoneGpuStartValid | ZoneGpuEndValid ) ||
        ( zone.flags & ZoneDurationAccounted ) ) return true;
    if( zone.gpuEndNs < zone.gpuStartNs ) { error = "session_gpu_zone_gpu_end_before_begin"; return false; }
    const auto duration = zone.gpuEndNs - zone.gpuStartNs;
    zone.selfTimeNs = std::max<int64_t>( 0, duration - zone.childGpuTimeNs );
    zone.flags |= ZoneDurationAccounted;
    return AddChildTime( state, zone.parent, duration, error );
}

void MarkComplete( BuildState& state, StoredZone& zone )
{
    constexpr uint32_t Required = ZoneCpuComplete | ZoneGpuStartValid | ZoneGpuEndValid;
    if( ( zone.flags & Required ) != Required || ( zone.flags & ZoneFullyCounted ) ) return;
    zone.flags |= ZoneFullyCounted;
    state.completeZones++;
}

bool ResolvePendingCallsite( BuildState& state, uint32_t id, const CallsiteState& callsite, std::string& error )
{
    const auto pending = state.pendingCallsiteZones.find( id ); if( pending == state.pendingCallsiteZones.end() ) return true;
    for( const auto zoneId : pending->second )
    {
        if( auto* open = FindOpen( state, zoneId ) )
        { open->stored.callstack = callsite.callstack; open->stored.provenance = callsite.provenance; open->stored.unavailableReason = callsite.unavailableReason; }
        else
        {
            StoredZone zone; if( !state.writer.Read( zoneId, zone, error ) ) return false;
            zone.callstack = callsite.callstack; zone.provenance = callsite.provenance; zone.unavailableReason = callsite.unavailableReason;
            if( !state.writer.Patch( zone, error ) ) return false;
        }
    }
    state.pendingCallsiteZones.erase( pending ); return true;
}

bool BeginZone( BuildState& state, const QueueGpuZoneBeginLean& event, size_t source,
    bool serial, uint32_t callstack, uint32_t callsite, std::string& error )
{
    auto* context = GetContext( state, event.context, error ); if( !context ) return false;
    const uint64_t thread = context->thread == 0 ? event.thread : 0;
    auto& stack = state.open[{ context->index, thread }];
    OpenZone zone; zone.stored.id = state.zoneCount++; zone.stored.parent = stack.empty() ? InvalidZoneId : stack.back().stored.id;
    zone.stored.thread = thread; zone.stored.context = context->index; zone.stored.sourceNativeId = state.sources[source].nativeId;
    zone.stored.cpuStartNs = serial ? AdvanceSerial( state, event.cpuTime ) : AdvanceThread( state, event.cpuTime );
    zone.stored.queryId = event.queryId; zone.stored.callstack = callstack; zone.stored.callsiteId = callsite;
    if( callsite != 0 )
    {
        if( const auto found = state.callsites.find( callsite ); found != state.callsites.end() )
        { zone.stored.callstack = found->second.callstack; zone.stored.provenance = found->second.provenance; zone.stored.unavailableReason = found->second.unavailableReason; }
        else state.pendingCallsiteZones[callsite].push_back( zone.stored.id );
    }
    if( !stack.empty() ) stack.back().stored.childCount++;
    if( !state.pendingQueries.emplace( QueryKey( event.context, event.queryId ), PendingQuery { zone.stored.id, true } ).second )
    { error = "session_gpu_zone_query_reused_before_time"; return false; }
    stack.emplace_back( std::move( zone ) ); state.beginEvents++; return true;
}

bool EndZone( BuildState& state, const QueueGpuZoneEnd& event, bool serial, std::string& error )
{
    auto* context = GetContext( state, event.context, error ); if( !context ) return false;
    const uint64_t thread = context->thread == 0 ? event.thread : 0;
    auto& stack = state.open[{ context->index, thread }];
    if( stack.empty() ) { error = "session_gpu_zone_end_without_begin"; return false; }
    auto zone = std::move( stack.back() ); stack.pop_back();
    zone.stored.cpuEndNs = serial ? AdvanceSerial( state, event.cpuTime ) : AdvanceThread( state, event.cpuTime );
    if( zone.stored.cpuEndNs < zone.stored.cpuStartNs ) { error = "session_gpu_zone_cpu_end_before_begin"; return false; }
    zone.stored.flags |= ZoneCpuComplete;
    if( !state.pendingQueries.emplace( QueryKey( event.context, event.queryId ), PendingQuery { zone.stored.id, false } ).second )
    { error = "session_gpu_zone_query_reused_before_time"; return false; }
    MarkComplete( state, zone.stored );
    if( !state.writer.Write( zone.stored, error ) ) return false;
    state.endEvents++; return true;
}

bool ResolveGpuTime( BuildState& state, const QueueGpuTime& event, std::string& error )
{
    auto* context = GetContext( state, event.context, error ); if( !context ) return false;
    const auto query = state.pendingQueries.find( QueryKey( event.context, event.queryId ) );
    if( query == state.pendingQueries.end() ) { error = "session_gpu_zone_time_without_query"; return false; }
    state.refTimeGpu += event.gpuTime; int64_t tgpu = state.refTimeGpu;
    if( tgpu < context->lastGpuTime - ( 1u << 31 ) )
    {
        if( context->overflow == 0 )
        {
            uint64_t value = uint64_t( std::max<int64_t>( 1, context->lastGpuTime ) );
            uint32_t bits = 0; while( value != 0 ) { ++bits; value >>= 1; }
            context->overflow = uint64_t( 1 ) << bits;
        }
        context->overflowMul++;
    }
    context->lastGpuTime = tgpu; if( context->overflow != 0 ) tgpu += int64_t( context->overflow * context->overflowMul );
    int64_t gpuNs = 0;
    if( !context->hasPeriod ) gpuNs = !context->calibrated ? tgpu + context->timeDiff :
        int64_t( ( tgpu - context->calibratedGpuTime ) * context->calibrationMod + context->calibratedCpuTime );
    else gpuNs = !context->calibrated ? int64_t( context->period * double( tgpu ) ) + context->timeDiff :
        int64_t( ( context->period * double( tgpu ) - context->calibratedGpuTime ) * context->calibrationMod + context->calibratedCpuTime );
    const auto pending = query->second; state.pendingQueries.erase( query );
    if( auto* open = FindOpen( state, pending.zone ) )
    {
        if( pending.start ) { open->stored.gpuStartNs = gpuNs; open->stored.flags |= ZoneGpuStartValid; context->zoneCount++; }
        else { open->stored.gpuEndNs = gpuNs; open->stored.flags |= ZoneGpuEndValid; }
        if( !AccountDuration( state, open->stored, error ) ) return false;
        MarkComplete( state, open->stored );
    }
    else
    {
        StoredZone zone; if( !state.writer.Read( pending.zone, zone, error ) ) return false;
        if( pending.start ) { zone.gpuStartNs = gpuNs; zone.flags |= ZoneGpuStartValid; context->zoneCount++; }
        else { zone.gpuEndNs = gpuNs; zone.flags |= ZoneGpuEndValid; }
        if( !AccountDuration( state, zone, error ) ) return false;
        MarkComplete( state, zone );
        if( !state.writer.Patch( zone, error ) ) return false;
    }
    state.gpuTimeEvents++; return true;
}

bool ConsumeThreadCallstack( BuildState& state, uint32_t& value, std::string& error )
{
    auto& next = state.nextCallstack[state.threadContext];
    if( next == 0 ) { error = "session_gpu_zone_thread_callstack_missing"; return false; }
    value = next; next = 0; return true;
}

bool ConsumeSerialCallstack( BuildState& state, uint32_t& value, std::string& error )
{
    if( state.serialNextCallstack == 0 ) { error = "session_gpu_zone_serial_callstack_missing"; return false; }
    value = state.serialNextCallstack; state.serialNextCallstack = 0; return true;
}

void ConsumeSingleString( BuildState& state ) { state.pendingSingleString.reset(); }

bool VisitGpuZoneRecord( const TraceSessionCanonicalRecord& record, void* userData, std::string& error )
{
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ) return true;
    auto& state = *static_cast<BuildState*>( userData ); QueueItem item {};
    if( !DecodeItem( record, item, error ) ) return false;
    const auto type = QueueType( record.type );
    switch( type )
    {
    case QueueType::ThreadContext: state.threadContext = item.threadCtx.thread; state.refTimeThread = 0; break;
    case QueueType::StringData:
    {
        const uint8_t* data = nullptr; size_t size = 0; if( !GetShortPayload( record, data, size, error ) ) return false;
        const std::string value( reinterpret_cast<const char*>( data ), size );
        const auto [found, inserted] = state.strings.emplace( item.stringTransfer.ptr, value );
        if( !inserted && found->second != value ) { error = "session_gpu_zone_string_conflict"; return false; }
        break;
    }
    case QueueType::SingleStringData:
    {
        const uint8_t* data = nullptr; size_t size = 0;
        if( !GetShortPayload( record, data, size, error ) ) return false;
        // This protocol staging slot is shared by symbol/callstack responses
        // and GPU context names. Only the immediately following consumer owns
        // the value; unrelated domains may replace it safely.
        state.pendingSingleString = std::string( reinterpret_cast<const char*>( data ), size ); break;
    }
    case QueueType::SourceLocation:
    {
        if( state.sourceResponseQueue.empty() ) { error = "session_gpu_zone_source_response_without_request"; return false; }
        auto& source = state.sources[state.sourceResponseQueue.front()]; state.sourceResponseQueue.pop_front();
        source.namePointer = item.srcloc.name; source.functionPointer = item.srcloc.function; source.filePointer = item.srcloc.file;
        source.line = item.srcloc.line; source.definitionReceived = true; break;
    }
    case QueueType::SourceLocationPayload:
    {
        const uint8_t* data = nullptr; size_t size = 0;
        if( !GetShortPayload( record, data, size, error ) || state.pendingDynamicSource )
        { if( error.empty() ) error = "session_gpu_zone_dynamic_source_sequence_invalid"; return false; }
        if( !ParseDynamicSource( state, data, size, error ) ) return false; break;
    }
    case QueueType::CallstackPayload:
    {
        const uint8_t* data = nullptr; size_t size = 0;
        if( !GetShortPayload( record, data, size, error ) || state.pendingCallstack != 0 )
        { if( error.empty() ) error = "session_gpu_zone_callstack_sequence_invalid"; return false; }
        state.pendingCallstack = InternCallstack( state, data, size, error ); if( state.pendingCallstack == 0 ) return false; break;
    }
    case QueueType::CallstackSampleDictionary:
    {
        const uint8_t* data = nullptr; size_t size = 0;
        if( !GetShortPayload( record, data, size, error ) || InternCallstack( state, data, size, error ) == 0 ) return false; break;
    }
    case QueueType::CallstackSerial:
        if( state.pendingCallstack == 0 || state.serialNextCallstack != 0 ) { error = "session_gpu_zone_serial_stack_sequence_invalid"; return false; }
        state.serialNextCallstack = state.pendingCallstack; state.pendingCallstack = 0; break;
    case QueueType::Callstack:
    case QueueType::CallstackAlloc:
        if( state.pendingCallstack == 0 ) { error = "session_gpu_zone_thread_stack_sequence_invalid"; return false; }
        state.nextCallstack[state.threadContext] = state.pendingCallstack; state.pendingCallstack = 0; break;
    case QueueType::CallstackSample:
    case QueueType::CallstackSampleContextSwitch:
        if( state.pendingCallstack == 0 ) { error = "session_gpu_zone_sample_stack_sequence_invalid"; return false; }
        state.pendingCallstack = 0; break;
    case QueueType::JnCallsiteDefinition:
    {
        EnsureStaticSource( state, item.jnCallsiteDefinition.srcloc ); CallsiteState callsite;
        callsite.provenance = item.jnCallsiteDefinition.provenance; callsite.unavailableReason = item.jnCallsiteDefinition.unavailableReason;
        if( item.jnCallsiteDefinition.flags & uint8_t( JnCallsiteFlags::HasCallstack ) )
            if( !ConsumeSerialCallstack( state, callsite.callstack, error ) ) return false;
        if( !state.callsites.emplace( item.jnCallsiteDefinition.callsiteId, callsite ).second )
        { error = "session_gpu_zone_callsite_duplicate"; return false; }
        if( !ResolvePendingCallsite( state, item.jnCallsiteDefinition.callsiteId, callsite, error ) ) return false; break;
    }
    case QueueType::GpuNewContext:
    {
        if( state.contextByWire.contains( item.gpuNewContext.context ) || item.gpuNewContext.type == GpuContextType::Invalid )
        { error = "session_gpu_zone_context_duplicate_or_invalid"; return false; }
        ContextState context; context.index = uint32_t( state.contexts.size() ); context.wireId = item.gpuNewContext.context;
        context.thread = item.gpuNewContext.thread; context.period = item.gpuNewContext.period; context.type = uint8_t( item.gpuNewContext.type );
        context.hasPeriod = context.period != 1.f; context.calibrated = ( item.gpuNewContext.flags & GpuContextCalibration ) != 0;
        const auto gpuBase = context.hasPeriod ? int64_t( double( context.period ) * item.gpuNewContext.gpuTime ) : item.gpuNewContext.gpuTime;
        const auto cpuBase = state.transform.ToNanoseconds( item.gpuNewContext.cpuTime ); context.timeDiff = cpuBase - gpuBase;
        context.calibratedGpuTime = gpuBase; context.calibratedCpuTime = cpuBase;
        state.contextByWire.emplace( context.wireId, context.index ); state.contexts.emplace_back( std::move( context ) ); break;
    }
    case QueueType::GpuZoneBegin:
    case QueueType::GpuZoneBeginCallstack:
    {
        const auto source = EnsureStaticSource( state, item.gpuZoneBegin.srcloc ); uint32_t stack = 0;
        if( type == QueueType::GpuZoneBeginCallstack && !ConsumeThreadCallstack( state, stack, error ) ) return false;
        if( !BeginZone( state, item.gpuZoneBegin, source, false, stack, 0, error ) ) return false; break;
    }
    case QueueType::GpuZoneBeginSerial:
    case QueueType::GpuZoneBeginCallstackSerial:
    case QueueType::JnGpuZoneBeginCallsite:
    {
        const auto source = EnsureStaticSource( state, item.gpuZoneBegin.srcloc ); uint32_t stack = 0, callsite = 0;
        if( type == QueueType::GpuZoneBeginCallstackSerial && !ConsumeSerialCallstack( state, stack, error ) ) return false;
        if( type == QueueType::JnGpuZoneBeginCallsite ) callsite = item.jnGpuZoneBeginCallsite.callsiteId;
        if( !BeginZone( state, item.gpuZoneBegin, source, true, stack, callsite, error ) ) return false; break;
    }
    case QueueType::GpuZoneBeginAllocSrcLoc:
    case QueueType::GpuZoneBeginAllocSrcLocCallstack:
    case QueueType::GpuZoneBeginAllocSrcLocSerial:
    case QueueType::GpuZoneBeginAllocSrcLocCallstackSerial:
    {
        if( !state.pendingDynamicSource ) { error = "session_gpu_zone_dynamic_source_missing"; return false; }
        const auto source = *state.pendingDynamicSource; state.pendingDynamicSource.reset(); uint32_t stack = 0;
        const bool serial = type == QueueType::GpuZoneBeginAllocSrcLocSerial || type == QueueType::GpuZoneBeginAllocSrcLocCallstackSerial;
        const bool withStack = type == QueueType::GpuZoneBeginAllocSrcLocCallstack || type == QueueType::GpuZoneBeginAllocSrcLocCallstackSerial;
        if( withStack && !( serial ? ConsumeSerialCallstack( state, stack, error ) : ConsumeThreadCallstack( state, stack, error ) ) ) return false;
        if( !BeginZone( state, item.gpuZoneBeginLean, source, serial, stack, 0, error ) ) return false; break;
    }
    case QueueType::GpuZoneEnd: if( !EndZone( state, item.gpuZoneEnd, false, error ) ) return false; break;
    case QueueType::GpuZoneEndSerial: if( !EndZone( state, item.gpuZoneEnd, true, error ) ) return false; break;
    case QueueType::GpuTime: if( !ResolveGpuTime( state, item.gpuTime, error ) ) return false; break;
    case QueueType::GpuCalibration:
    {
        auto* context = GetContext( state, item.gpuCalibration.context, error ); if( !context || !context->calibrated ) return false;
        const auto gpu = context->hasPeriod ? int64_t( context->period * double( item.gpuCalibration.gpuTime ) ) : item.gpuCalibration.gpuTime;
        const auto delta = gpu - context->calibratedGpuTime; if( delta == 0 ) { error = "session_gpu_zone_calibration_zero_delta"; return false; }
        context->calibrationMod = double( item.gpuCalibration.cpuDelta ) / delta; context->calibratedGpuTime = gpu;
        context->calibratedCpuTime = state.transform.ToNanoseconds( item.gpuCalibration.cpuTime ); state.calibrationEvents++; break;
    }
    case QueueType::GpuTimeSync:
    {
        auto* context = GetContext( state, item.gpuTimeSync.context, error ); if( !context ) return false;
        const auto gpu = context->hasPeriod ? int64_t( context->period * double( item.gpuTimeSync.gpuTime ) ) : item.gpuTimeSync.gpuTime;
        context->timeDiff = state.transform.ToNanoseconds( item.gpuTimeSync.cpuTime ) - gpu;
        context->lastGpuTime = 0; context->overflow = 0; context->overflowMul = 0; state.syncEvents++; break;
    }
    case QueueType::GpuContextName:
    {
        if( !state.pendingSingleString ) { error = "session_gpu_zone_context_name_missing"; return false; }
        auto* context = GetContext( state, item.gpuContextName.context, error ); if( !context ) return false;
        context->name = *state.pendingSingleString; state.pendingSingleString.reset(); break;
    }
    case QueueType::GpuAnnotationName: ConsumeSingleString( state ); break;
    case QueueType::ZoneName: case QueueType::ZoneText: case QueueType::LockName:
    case QueueType::Message: case QueueType::MessageColor: case QueueType::MessageAppInfo:
        ConsumeSingleString( state ); break;
    case QueueType::ZoneBegin: case QueueType::ZoneBeginCallstack: case QueueType::JnZoneBeginCallsite:
        EnsureStaticSource( state, item.zoneBegin.srcloc ); AdvanceThread( state, item.zoneBegin.time );
        if( type == QueueType::ZoneBeginCallstack ) { uint32_t ignored; if( !ConsumeThreadCallstack( state, ignored, error ) ) return false; } break;
    case QueueType::ZoneBeginAllocSrcLoc: case QueueType::ZoneBeginAllocSrcLocCallstack:
        if( !state.pendingDynamicSource ) { error = "session_gpu_zone_cpu_dynamic_source_missing"; return false; }
        state.pendingDynamicSource.reset(); AdvanceThread( state, item.zoneBeginLean.time );
        if( type == QueueType::ZoneBeginAllocSrcLocCallstack ) { uint32_t ignored; if( !ConsumeThreadCallstack( state, ignored, error ) ) return false; } break;
    case QueueType::ZoneEnd: AdvanceThread( state, item.zoneEnd.time ); break;
    case QueueType::PlotDataInt: AdvanceThread( state, item.plotDataInt.time ); break;
    case QueueType::PlotDataFloat: AdvanceThread( state, item.plotDataFloat.time ); break;
    case QueueType::PlotDataDouble: AdvanceThread( state, item.plotDataDouble.time ); break;
    case QueueType::FiberEnter: AdvanceThread( state, item.fiberEnter.time ); break;
    case QueueType::FiberLeave: AdvanceThread( state, item.fiberLeave.time ); break;
    case QueueType::LockAnnounce: EnsureStaticSource( state, item.lockAnnounce.lckloc ); break;
    case QueueType::LockMark: EnsureStaticSource( state, item.lockMark.srcloc ); break;
    case QueueType::LockWait: case QueueType::LockObtain: case QueueType::LockSharedWait: case QueueType::LockSharedObtain:
        AdvanceSerial( state, item.lockWait.time ); break;
    case QueueType::LockRelease: AdvanceSerial( state, item.lockRelease.time ); break;
    case QueueType::LockSharedRelease: AdvanceSerial( state, item.lockReleaseShared.time ); break;
    case QueueType::MemAlloc: case QueueType::MemAllocNamed: case QueueType::MemAllocCallstack:
    case QueueType::MemAllocCallstackNamed: case QueueType::JnMemAllocCallsiteNamed:
        AdvanceSerial( state, item.memAlloc.time );
        if( type == QueueType::MemAllocCallstack || type == QueueType::MemAllocCallstackNamed )
        { uint32_t ignored; if( !ConsumeSerialCallstack( state, ignored, error ) ) return false; } break;
    case QueueType::MemFree: case QueueType::MemFreeNamed: case QueueType::MemFreeCallstack: case QueueType::MemFreeCallstackNamed:
        AdvanceSerial( state, item.memFree.time );
        if( type == QueueType::MemFreeCallstack || type == QueueType::MemFreeCallstackNamed )
        { uint32_t ignored; if( !ConsumeSerialCallstack( state, ignored, error ) ) return false; } break;
    case QueueType::MemDiscard: case QueueType::MemDiscardCallstack:
        AdvanceSerial( state, item.memDiscard.time );
        if( type == QueueType::MemDiscardCallstack ) { uint32_t ignored; if( !ConsumeSerialCallstack( state, ignored, error ) ) return false; } break;
    case QueueType::MessageCallstack: case QueueType::MessageColorCallstack:
    {
        ConsumeSingleString( state );
        uint32_t ignored; if( !ConsumeThreadCallstack( state, ignored, error ) ) return false; break;
    }
    case QueueType::MessageLiteralCallstack: case QueueType::MessageLiteralColorCallstack:
    {
        uint32_t ignored; if( !ConsumeThreadCallstack( state, ignored, error ) ) return false; break;
    }
    case QueueType::JnJobStage:
        if( JnJobStage( item.jnJobStage.stage ) == JnJobStage::ScheduleCallstack || JnJobStage( item.jnJobStage.stage ) == JnJobStage::WaitCallstack )
        { uint32_t ignored; if( !ConsumeSerialCallstack( state, ignored, error ) ) return false; } break;
    case QueueType::JnIoStage:
        if( JnIoStage( item.jnIoStage.stage ) == JnIoStage::RequestCallstack )
        { uint32_t ignored; if( !ConsumeSerialCallstack( state, ignored, error ) ) return false; } break;
    default: break;
    }
    return true;
}

bool SaveManifest( const std::filesystem::path& root, const GpuZoneManifest& value, std::string& error )
{
    const auto temporary = root / "manifest.tmp"; std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_gpu_zone_manifest_open_failed"; return false; }
    out << "magic " << GpuZoneManifestMagic << '\n' << "schema " << TraceSessionGpuZoneIndexSchemaVersion << '\n'
        << "source_sha256 " << std::quoted( value.sourceSha256 ) << '\n' << "source_size " << value.sourceSize << '\n'
        << "generation " << std::quoted( value.generation ) << '\n' << "file_bytes " << value.fileBytes << '\n'
        << "file_sha256 " << std::quoted( value.fileSha256 ) << '\n' << "contexts " << value.stats.contexts << '\n'
        << "zones " << value.stats.zones << '\n' << "zone_blocks " << value.stats.zoneBlocks << '\n'
        << "complete_zones " << value.stats.completeZones << '\n'
        << "source_locations " << value.stats.sourceLocations << '\n' << "begin_events " << value.stats.beginEvents << '\n'
        << "end_events " << value.stats.endEvents << '\n' << "gpu_time_events " << value.stats.gpuTimeEvents << '\n'
        << "calibration_events " << value.stats.calibrationEvents << '\n' << "sync_events " << value.stats.syncEvents << '\n';
    out.flush(); if( !out ) { error = "session_gpu_zone_manifest_write_failed"; return false; } out.close();
    return AtomicReplace( temporary, root / "manifest", error );
}

bool LoadManifest( const std::filesystem::path& root, GpuZoneManifest& value, std::string& error )
{
    value = {}; std::ifstream in( root / "manifest", std::ios::binary );
    if( !in ) { error = "session_gpu_zone_manifest_not_found"; return false; }
    uint64_t magic = 0; uint32_t schema = 0; std::string key;
    while( in >> key )
    {
        if( key == "magic" ) in >> magic; else if( key == "schema" ) in >> schema;
        else if( key == "source_sha256" ) in >> std::quoted( value.sourceSha256 ); else if( key == "source_size" ) in >> value.sourceSize;
        else if( key == "generation" ) in >> std::quoted( value.generation ); else if( key == "file_bytes" ) in >> value.fileBytes;
        else if( key == "file_sha256" ) in >> std::quoted( value.fileSha256 ); else if( key == "contexts" ) in >> value.stats.contexts;
        else if( key == "zones" ) in >> value.stats.zones; else if( key == "zone_blocks" ) in >> value.stats.zoneBlocks;
        else if( key == "complete_zones" ) in >> value.stats.completeZones;
        else if( key == "source_locations" ) in >> value.stats.sourceLocations; else if( key == "begin_events" ) in >> value.stats.beginEvents;
        else if( key == "end_events" ) in >> value.stats.endEvents; else if( key == "gpu_time_events" ) in >> value.stats.gpuTimeEvents;
        else if( key == "calibration_events" ) in >> value.stats.calibrationEvents; else if( key == "sync_events" ) in >> value.stats.syncEvents;
        else { std::string ignored; std::getline( in, ignored ); }
        if( !in ) { error = "session_gpu_zone_manifest_parse_failed"; return false; }
    }
    value.stats.fileBytes = value.fileBytes;
    if( magic != GpuZoneManifestMagic || schema != TraceSessionGpuZoneIndexSchemaVersion ||
        value.sourceSha256.size() != 64 || value.fileSha256.size() != 64 )
    { error = "session_gpu_zone_manifest_invalid"; return false; }
    return true;
}

std::string MakeRef( const std::string& fingerprint, const char* kind, uint64_t id )
{
    std::ostringstream out; out << "tracy:v1:" << fingerprint.substr( 0, 16 ) << ':' << kind << ':' << std::hex << id; return out.str();
}

std::optional<uint32_t> ParseContextRef( const std::string& fingerprint,
    std::string_view ref )
{
    const auto prefix = std::string( "tracy:v1:" ) + fingerprint.substr( 0, 16 ) + ":gpu-context:";
    if( !ref.starts_with( prefix ) ) return std::nullopt;
    uint32_t value = 0;
    const auto first = ref.data() + prefix.size();
    const auto last = ref.data() + ref.size();
    const auto parsed = std::from_chars( first, last, value, 16 );
    if( parsed.ec != std::errc {} || parsed.ptr != last ) return std::nullopt;
    return value;
}

uint64_t MixContext( uint64_t value )
{
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ull;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebull;
    return value ^ ( value >> 31 );
}

void AddContext( StoredZoneBlock& block, uint32_t context )
{
    const auto mixed = MixContext( context );
    const std::array<uint8_t, 3> bits = {
        uint8_t( mixed ), uint8_t( mixed >> 21 ), uint8_t( mixed >> 42 ) };
    for( const auto bit : bits ) block.contextBloom[bit >> 6] |= uint64_t( 1 ) << ( bit & 63 );
}

bool MayContainContext( const StoredZoneBlock& block, uint32_t context )
{
    const auto mixed = MixContext( context );
    const std::array<uint8_t, 3> bits = {
        uint8_t( mixed ), uint8_t( mixed >> 21 ), uint8_t( mixed >> 42 ) };
    for( const auto bit : bits )
    {
        if( ( block.contextBloom[bit >> 6] & ( uint64_t( 1 ) << ( bit & 63 ) ) ) == 0 ) return false;
    }
    return true;
}

const char* ContextTypeName( uint8_t value )
{
    switch( GpuContextType( value ) )
    {
    case GpuContextType::Invalid: return "invalid"; case GpuContextType::OpenGl: return "opengl";
    case GpuContextType::Vulkan: return "vulkan"; case GpuContextType::OpenCL: return "opencl";
    case GpuContextType::Direct3D12: return "direct3d12"; case GpuContextType::Direct3D11: return "direct3d11";
    case GpuContextType::Metal: return "metal"; case GpuContextType::Custom: return "custom";
    case GpuContextType::CUDA: return "cuda"; case GpuContextType::Rocprof: return "rocprof";
    }
    return "unknown";
}

const char* ProvenanceName( uint8_t value )
{
    switch( JnStackProvenance( value ) )
    {
    case JnStackProvenance::ExactSource: return "ExactSource"; case JnStackProvenance::SiteReused: return "SiteReused";
    case JnStackProvenance::PerEventExact: return "PerEventExact"; case JnStackProvenance::Unavailable: return "Unavailable";
    }
    return "Unavailable";
}

const char* UnavailableReasonName( uint8_t value )
{
    switch( value ) { case 0: return ""; case 1: return "depth_zero"; case 2: return "callstack_unsupported";
    case 3: return "admission_denied"; case 4: return "capacity"; default: return "unknown"; }
}

}

struct TraceSessionGpuZoneReader::Impl
{
    std::filesystem::path path; std::string fingerprint; uint64_t zonesOffset = 0, zoneCount = 0;
    std::vector<StoredZoneBlock> zoneBlocks;
    std::unordered_map<int32_t, SourceState> sources;
    bool ReadZone( uint64_t id, StoredZone& zone ) const
    {
        if( id >= zoneCount ) return false; std::ifstream in( path, std::ios::binary ); if( !in ) return false;
        in.seekg( std::streamoff( zonesOffset + id * sizeof( StoredZone ) ) );
        return bool( in.read( reinterpret_cast<char*>( &zone ), sizeof( zone ) ) ) && zone.id == id;
    }
    GpuZoneDto ToDto( const StoredZone& zone ) const
    {
        GpuZoneDto dto; dto.ref = MakeRef( fingerprint, "gpu-zone", zone.id );
        dto.contextRef = MakeRef( fingerprint, "gpu-context", zone.context ); dto.threadRef = MakeRef( fingerprint, "thread", zone.thread );
        dto.sourceLocationRef = MakeRef( fingerprint, "source", uint16_t( zone.sourceNativeId ) );
        if( const auto found = sources.find( zone.sourceNativeId ); found != sources.end() )
        { dto.name = found->second.name.empty() ? found->second.function : found->second.name; dto.function = found->second.function; dto.file = found->second.file; dto.line = found->second.line; }
        if( zone.parent != InvalidZoneId ) dto.parentRef = MakeRef( fingerprint, "gpu-zone", zone.parent );
        dto.gpuStartNs = zone.gpuStartNs; if( zone.flags & ZoneGpuEndValid ) { dto.gpuEndNs = zone.gpuEndNs; dto.selfTimeNs = zone.selfTimeNs; }
        dto.cpuStartNs = zone.cpuStartNs; if( zone.flags & ZoneCpuComplete ) dto.cpuEndNs = zone.cpuEndNs;
        dto.callstack = zone.callstack; if( zone.callstack != 0 ) dto.callstackRef = MakeRef( fingerprint, "callstack", zone.callstack );
        if( zone.callsiteId != 0 ) dto.callsiteId = zone.callsiteId;
        dto.stackProvenance = ProvenanceName( zone.provenance ); if( zone.callstack != 0 ) dto.stackRef = MakeRef( fingerprint, "callstack", zone.callstack );
        if( const auto* reason = UnavailableReasonName( zone.unavailableReason ); *reason != '\0' ) dto.stackUnavailableReason = reason;
        dto.childCount = zone.childCount;
        dto.complete = ( zone.flags & ( ZoneCpuComplete | ZoneGpuStartValid | ZoneGpuEndValid ) ) ==
            ( ZoneCpuComplete | ZoneGpuStartValid | ZoneGpuEndValid );
        dto.queryId = zone.queryId; dto.queryIdAvailability.available = true; return dto;
    }
};

TraceSessionGpuZoneReader::TraceSessionGpuZoneReader( std::shared_ptr<Impl> impl ) : m_impl( std::move( impl ) ) {}

std::filesystem::path TraceSessionGpuZoneIndexRoot( const std::filesystem::path& sessionRoot, const TraceSessionManifest& manifest )
{
    return sessionRoot / "generations" / manifest.generation / "derived" / "gpu-zone-index" /
        std::to_string( TraceSessionGpuZoneIndexSchemaVersion ) / "exact";
}

bool CleanupTraceSessionGpuZoneTemporaryFiles( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, std::string& error )
{
    error.clear();
    const auto path = TraceSessionGpuZoneIndexRoot( sessionRoot, manifest ) / "zones.work";
    std::error_code ec;
    const auto exists = std::filesystem::exists( path, ec );
    if( ec ) { error = "session_gpu_zone_work_cleanup_scan_failed:" + ec.message(); return false; }
    if( !exists ) return true;
    if( !std::filesystem::remove( path, ec ) || ec )
    { error = "session_gpu_zone_work_cleanup_failed:" + ( ec ? ec.message() : path.string() ); return false; }
    return true;
}

bool BuildTraceSessionGpuZoneDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionGpuZoneStats& stats, std::string& error )
{
    error.clear(); stats = {}; BuildState state;
    if( !LoadTraceSessionTimeTransform( sessionRoot, manifest, state.transform, error ) ) return false;
    const auto root = TraceSessionGpuZoneIndexRoot( sessionRoot, manifest ); if( !state.writer.Open( root, manifest, error ) ) return false;
    if( !VisitTraceSessionCanonicalOrdered( sessionRoot, manifest, VisitGpuZoneRecord, &state, error ) ) return false;
    if( state.pendingCallstack != 0 || state.serialNextCallstack != 0 || state.pendingDynamicSource )
    { error = "session_gpu_zone_pending_protocol_state"; return false; }
    // A capture may end while timestamp queries are still in flight. Preserve
    // those zones as explicitly incomplete instead of turning a truthful source
    // boundary into a converter-created failure.
    for( auto& [key, stack] : state.open ) while( !stack.empty() )
    {
        auto zone = std::move( stack.back() ); stack.pop_back();
        if( !state.writer.Write( zone.stored, error ) ) return false;
    }
    for( auto& source : state.sources ) if( !source.dynamic )
    {
        if( const auto found = state.strings.find( source.namePointer ); found != state.strings.end() ) source.name = found->second;
        if( const auto found = state.strings.find( source.functionPointer ); found != state.strings.end() ) source.function = found->second;
        if( const auto found = state.strings.find( source.filePointer ); found != state.strings.end() ) source.file = found->second;
    }
    GpuZoneManifest fileManifest;
    if( !state.writer.Finalize( state.contexts, state.sources, state.zoneCount, state.completeZones, fileManifest, error ) ) return false;
    fileManifest.stats.beginEvents = state.beginEvents; fileManifest.stats.endEvents = state.endEvents;
    fileManifest.stats.gpuTimeEvents = state.gpuTimeEvents; fileManifest.stats.calibrationEvents = state.calibrationEvents;
    fileManifest.stats.syncEvents = state.syncEvents;
    fileManifest.stats.completeZones = state.completeZones;
    if( !SaveManifest( root, fileManifest, error ) ) return false; stats = fileManifest.stats; return true;
}

bool AuditTraceSessionGpuZoneDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& session, TraceSessionGpuZoneStats& stats, std::string& error )
{
    error.clear(); stats = {}; const auto root = TraceSessionGpuZoneIndexRoot( sessionRoot, session ); GpuZoneManifest manifest;
    if( !LoadManifest( root, manifest, error ) ) return false;
    if( manifest.sourceSha256 != session.source.sha256 || manifest.sourceSize != session.source.fileSize || manifest.generation != session.generation )
    { error = "session_gpu_zone_identity_mismatch"; return false; }
    const auto path = root / GpuZoneFileName; std::error_code ec;
    if( std::filesystem::file_size( path, ec ) != manifest.fileBytes || ec ) { error = "session_gpu_zone_file_size_mismatch"; return false; }
    if( Sha256File( path ) != manifest.fileSha256 ) { error = "session_gpu_zone_file_sha256_mismatch"; return false; }
    stats = manifest.stats; return true;
}

std::shared_ptr<TraceSessionGpuZoneReader> TraceSessionGpuZoneReader::Open(
    const std::filesystem::path& sessionRoot, const TraceSessionManifest& session, std::string& error )
{
    error.clear(); const auto root = TraceSessionGpuZoneIndexRoot( sessionRoot, session ); GpuZoneManifest manifest;
    if( !LoadManifest( root, manifest, error ) ) return {};
    if( manifest.sourceSha256 != session.source.sha256 || manifest.sourceSize != session.source.fileSize || manifest.generation != session.generation )
    { error = "session_gpu_zone_identity_mismatch"; return {}; }
    const auto path = root / GpuZoneFileName; std::error_code ec;
    if( std::filesystem::file_size( path, ec ) != manifest.fileBytes || ec ) { error = "session_gpu_zone_file_size_mismatch"; return {}; }
    std::ifstream in( path, std::ios::binary ); GpuZoneFileHeader header;
    if( !in.read( reinterpret_cast<char*>( &header ), sizeof( header ) ) || header.magic != GpuZoneFileMagic ||
        header.schema != TraceSessionGpuZoneIndexSchemaVersion || header.endian != 0x01020304 ||
        header.sourceSize != session.source.fileSize || header.zoneCount != manifest.stats.zones ||
        header.completeZoneCount != manifest.stats.completeZones ||
        header.contextCount != manifest.stats.contexts || header.sourceCount != manifest.stats.sourceLocations ||
        header.zoneBlockCount != manifest.stats.zoneBlocks ||
        header.generationBytes != session.generation.size() )
    { error = "session_gpu_zone_file_header_invalid"; return {}; }
    std::string sha( 64, '\0' ), generation( header.generationBytes, '\0' );
    if( !in.read( sha.data(), std::streamsize( sha.size() ) ) || !in.read( generation.data(), std::streamsize( generation.size() ) ) ||
        sha != session.source.sha256 || generation != session.generation )
    { error = "session_gpu_zone_file_identity_invalid"; return {}; }
    const auto expectedZonesOffset = sizeof( header ) + sha.size() + generation.size();
    const auto expectedBlocksOffset = expectedZonesOffset + header.zoneCount * sizeof( StoredZone );
    const auto expectedContextsOffset = expectedBlocksOffset + header.zoneBlockCount * sizeof( StoredZoneBlock );
    if( header.zonesOffset != expectedZonesOffset || header.zoneBlocksOffset != expectedBlocksOffset ||
        header.contextsOffset != expectedContextsOffset || header.sourcesOffset < header.contextsOffset )
    { error = "session_gpu_zone_file_layout_invalid"; return {}; }
    auto impl = std::make_shared<Impl>(); impl->path = path; impl->fingerprint = session.source.sha256;
    impl->zonesOffset = header.zonesOffset; impl->zoneCount = header.zoneCount;
    impl->zoneBlocks.resize( size_t( header.zoneBlockCount ) );
    in.seekg( std::streamoff( header.zoneBlocksOffset ) );
    if( !impl->zoneBlocks.empty() && !in.read( reinterpret_cast<char*>( impl->zoneBlocks.data() ),
        std::streamsize( impl->zoneBlocks.size() * sizeof( StoredZoneBlock ) ) ) )
    { error = "session_gpu_zone_block_records_truncated"; return {}; }
    uint64_t expectedFirstZone = 0;
    for( const auto& block : impl->zoneBlocks )
    {
        if( block.firstZone != expectedFirstZone || block.zoneCount == 0 || block.zoneCount > 4096 ||
            block.firstZone + block.zoneCount > header.zoneCount || block.minStartNs > block.maxEndNs )
        { error = "session_gpu_zone_block_record_invalid"; return {}; }
        expectedFirstZone += block.zoneCount;
    }
    if( expectedFirstZone != header.zoneCount )
    { error = "session_gpu_zone_block_coverage_invalid"; return {}; }
    auto reader = std::shared_ptr<TraceSessionGpuZoneReader>( new TraceSessionGpuZoneReader( impl ) ); reader->m_stats = manifest.stats;
    in.seekg( std::streamoff( header.contextsOffset ) );
    for( uint64_t i = 0; i < header.contextCount; ++i )
    {
        StoredContext stored; if( !in.read( reinterpret_cast<char*>( &stored ), sizeof( stored ) ) || stored.index != i || stored.nameBytes > 16 * 1024 * 1024 )
        { error = "session_gpu_zone_context_record_invalid"; return {}; }
        std::string name( stored.nameBytes, '\0' ); if( !name.empty() && !in.read( name.data(), std::streamsize( name.size() ) ) )
        { error = "session_gpu_zone_context_name_truncated"; return {}; }
        GpuContextDto dto; dto.ref = MakeRef( session.source.sha256, "gpu-context", i ); dto.index = i;
        dto.name = name.empty() ? "GPU context " + std::to_string( i ) : name; dto.threadRef = MakeRef( session.source.sha256, "thread", stored.thread );
        dto.zoneCount = stored.zoneCount; dto.period = stored.period; dto.calibrated = stored.calibrated != 0;
        dto.type = stored.type; dto.typeName = ContextTypeName( stored.type ); dto.overflow = stored.overflow;
        dto.notesAvailability.available = false; dto.notesAvailability.reason = "GPU annotations are not yet materialized by the Session GPU Zone index";
        if( !name.empty() ) dto.customName = name; reader->m_contexts.emplace_back( std::move( dto ) );
    }
    in.seekg( std::streamoff( header.sourcesOffset ) );
    for( uint64_t i = 0; i < header.sourceCount; ++i )
    {
        StoredSource stored; if( !in.read( reinterpret_cast<char*>( &stored ), sizeof( stored ) ) ||
            stored.nameBytes > 16 * 1024 * 1024 || stored.functionBytes > 16 * 1024 * 1024 || stored.fileBytes > 16 * 1024 * 1024 )
        { error = "session_gpu_zone_source_record_invalid"; return {}; }
        SourceState source; source.nativeId = stored.nativeId; source.line = stored.line;
        source.dynamic = ( stored.flags & 1 ) != 0; source.definitionReceived = ( stored.flags & 2 ) != 0;
        source.name.resize( stored.nameBytes ); source.function.resize( stored.functionBytes ); source.file.resize( stored.fileBytes );
        if( ( !source.name.empty() && !in.read( source.name.data(), std::streamsize( source.name.size() ) ) ) ||
            ( !source.function.empty() && !in.read( source.function.data(), std::streamsize( source.function.size() ) ) ) ||
            ( !source.file.empty() && !in.read( source.file.data(), std::streamsize( source.file.size() ) ) ) )
        { error = "session_gpu_zone_source_record_truncated"; return {}; }
        if( !impl->sources.emplace( source.nativeId, std::move( source ) ).second )
        { error = "session_gpu_zone_source_id_duplicate"; return {}; }
    }
    if( in.peek() != std::char_traits<char>::eof() ) { error = "session_gpu_zone_file_trailing_bytes"; return {}; }
    return reader;
}

std::optional<GpuZoneDto> TraceSessionGpuZoneReader::Get( uint64_t id ) const
{
    StoredZone zone; if( !m_impl->ReadZone( id, zone ) ) return std::nullopt; return m_impl->ToDto( zone );
}

std::vector<GpuZoneDto> TraceSessionGpuZoneReader::Scan( const ScanRange& range ) const
{
    return ScanImpl( range, std::nullopt );
}

std::vector<GpuZoneDto> TraceSessionGpuZoneReader::ScanContext(
    std::string_view contextRef, const ScanRange& range ) const
{
    const auto context = ParseContextRef( m_impl->fingerprint, contextRef );
    return context ? ScanImpl( range, context ) : std::vector<GpuZoneDto> {};
}

std::vector<GpuZoneDto> TraceSessionGpuZoneReader::ScanImpl(
    const ScanRange& range, std::optional<uint32_t> context ) const
{
    std::vector<GpuZoneDto> result;
    if( range.limit == 0 ) return result;
    size_t skipped = 0; std::ifstream in( m_impl->path, std::ios::binary ); if( !in ) return result;
    for( const auto& block : m_impl->zoneBlocks )
    {
        if( block.maxEndNs < range.startNs || block.minStartNs > range.endNs ||
            ( context && !MayContainContext( block, *context ) ) ) continue;
        in.clear();
        in.seekg( std::streamoff( m_impl->zonesOffset + block.firstZone * sizeof( StoredZone ) ) );
        for( uint32_t index = 0; index < block.zoneCount; ++index )
        {
            StoredZone zone; const auto id = block.firstZone + index;
            if( !in.read( reinterpret_cast<char*>( &zone ), sizeof( zone ) ) || zone.id != id ) return result;
            if( context && zone.context != *context ) continue;
            const auto start = ( zone.flags & ZoneGpuStartValid ) ? zone.gpuStartNs : zone.cpuStartNs;
            const auto end = ( zone.flags & ZoneGpuEndValid ) ? zone.gpuEndNs : start;
            if( end < range.startNs || start > range.endNs ) continue;
            if( skipped++ < range.offset ) continue;
            result.emplace_back( m_impl->ToDto( zone ) );
            if( result.size() >= range.limit ) return result;
        }
    }
    return result;
}

std::vector<GpuZoneDto> TraceSessionGpuZoneReader::Children( uint64_t id, size_t offset, size_t limit ) const
{
    std::vector<GpuZoneDto> result; if( id >= m_impl->zoneCount ) return result; size_t skipped = 0;
    std::ifstream in( m_impl->path, std::ios::binary ); if( !in ) return result; in.seekg( std::streamoff( m_impl->zonesOffset ) );
    for( uint64_t current = 0; current < m_impl->zoneCount; ++current )
    {
        StoredZone zone; if( !in.read( reinterpret_cast<char*>( &zone ), sizeof( zone ) ) || zone.id != current ) break;
        if( zone.parent != id ) continue; if( skipped++ < offset ) continue; result.emplace_back( m_impl->ToDto( zone ) );
        if( result.size() >= limit ) break;
    }
    return result;
}

}

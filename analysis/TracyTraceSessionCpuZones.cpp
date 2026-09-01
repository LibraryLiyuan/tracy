#include "TracyTraceSessionCpuZones.hpp"

#include "TracyHash.hpp"
#include "TracyTraceSessionCanonical.hpp"
#include "TracyTraceSessionExternalSort.hpp"
#include "TracyTraceSessionGpuCanonical.hpp"
#include "TracyProtocol.hpp"
#include "TracyQueue.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <deque>
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

constexpr uint64_t CpuZoneFileMagic = 0x315a5043534e4aull; // JNSCPZ1
constexpr uint64_t CpuZoneManifestMagic = 0x314d5a43534e4aull; // JNSCZM1
constexpr const char* CpuZoneFileName = "cpu-zones.bin";
constexpr uint64_t InvalidZoneId = std::numeric_limits<uint64_t>::max();

enum StoredZoneFlags : uint32_t
{
    ZoneComplete = 1u << 0,
    ZoneNameResolved = 1u << 1,
    ZoneExtraValid = 1u << 2,
    ZoneTimingValid = 1u << 3
};

#pragma pack( push, 1 )
struct CpuZoneFileHeader
{
    uint64_t magic = CpuZoneFileMagic;
    uint32_t schema = TraceSessionCpuZoneIndexSchemaVersion;
    uint32_t endian = 0x01020304;
    uint64_t sourceSize = 0;
    uint64_t zoneCount = 0;
    uint64_t completeZoneCount = 0;
    uint64_t sourceCount = 0;
    uint64_t zoneBlockCount = 0;
    uint64_t zonesOffset = 0;
    uint64_t zoneBlocksOffset = 0;
    uint64_t extrasOffset = 0;
    uint64_t sourcesOffset = 0;
    uint64_t callsitesOffset = 0;
    uint64_t callsiteCount = 0;
    uint64_t childLinksOffset = 0;
    uint64_t childLinkCount = 0;
    uint32_t generationBytes = 0;
    uint32_t reserved = 0;
};

struct StoredZone
{
    uint64_t id = 0;
    uint64_t parent = InvalidZoneId;
    uint64_t thread = 0;
    int64_t startNs = 0;
    int64_t endNs = 0;
    int64_t selfTimeNs = 0;
    int32_t sourceNativeId = 0;
    uint32_t callstack = 0;
    uint32_t callsiteId = 0;
    uint32_t childCount = 0;
    uint32_t extraColor = 0;
    uint64_t extraNameOffset = 0;
    uint64_t extraTextOffset = 0;
    uint32_t extraNameBytes = 0;
    uint32_t extraTextBytes = 0;
    uint32_t flags = ZoneNameResolved | ZoneExtraValid | ZoneTimingValid;
    uint8_t provenance = 3;
    uint8_t unavailableReason = 0;
    uint16_t reserved = 0;
};

struct StoredSource
{
    int32_t nativeId = 0;
    uint32_t line = 0;
    uint32_t color = 0;
    uint32_t nameBytes = 0;
    uint32_t functionBytes = 0;
    uint32_t fileBytes = 0;
    uint32_t flags = 0;
};

struct StoredCallsite
{
    uint64_t thread = 0;
    uint32_t callsiteId = 0;
    int32_t sourceNativeId = 0;
    uint32_t callstack = 0;
    uint8_t domain = 0;
    uint8_t provenance = 3;
    uint8_t flags = 0;
    uint8_t unavailableReason = 0;
};

struct StoredZoneBlock
{
    uint64_t firstZone = 0;
    uint32_t zoneCount = 0;
    uint32_t reserved = 0;
    int64_t minStartNs = 0;
    int64_t maxEndNs = 0;
    uint64_t threadBloom[4] {};
};
#pragma pack( pop )

void AddThread( StoredZoneBlock& block, uint64_t thread );
bool MayContainThread( const StoredZoneBlock& block, uint64_t thread );

struct CpuZoneManifest
{
    std::string sourceSha256;
    uint64_t sourceSize = 0;
    std::string generation;
    uint64_t fileBytes = 0;
    std::string fileSha256;
    TraceSessionCpuZoneStats stats;
};

struct SourceState
{
    int32_t nativeId = 0;
    uint64_t pointer = 0;
    uint64_t namePointer = 0;
    uint64_t functionPointer = 0;
    uint64_t filePointer = 0;
    uint32_t line = 0;
    uint32_t color = 0;
    bool dynamic = false;
    bool definitionReceived = false;
    std::string name;
    std::string function;
    std::string file;
};

struct CallsiteState
{
    uint64_t thread = 0;
    uint32_t callsiteId = 0;
    int32_t sourceNativeId = 0;
    uint32_t callstack = 0;
    uint8_t domain = 0;
    uint8_t provenance = 3;
    uint8_t flags = 0;
    uint8_t unavailableReason = 0;
};

struct OpenZone
{
    StoredZone stored;
    uint32_t validationId = 0;
    int64_t childTimeNs = 0;
    std::string extraName;
    std::string extraText;
};

bool AtomicReplace( const std::filesystem::path& source,
    const std::filesystem::path& target, std::string& error )
{
#ifdef _WIN32
    if( MoveFileExW( source.c_str(), target.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) ) return true;
    error = "session_cpu_zone_atomic_replace_failed:" + std::to_string( GetLastError() );
    return false;
#else
    std::error_code ec;
    std::filesystem::rename( source, target, ec );
    if( !ec ) return true;
    error = "session_cpu_zone_atomic_replace_failed:" + ec.message();
    return false;
#endif
}

bool CopyFileBytes( std::ifstream& in, std::ofstream& out, std::string& error )
{
    std::vector<char> buffer( 1024 * 1024 );
    while( in )
    {
        in.read( buffer.data(), std::streamsize( buffer.size() ) );
        const auto count = in.gcount();
        if( count > 0 ) out.write( buffer.data(), count );
    }
    if( !in.eof() || !out ) { error = "session_cpu_zone_copy_failed"; return false; }
    return true;
}

bool DecodeItem( const TraceSessionCanonicalRecord& record, QueueItem& item,
    std::string& error )
{
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ||
        record.type >= uint8_t( QueueType::NUM_TYPES ) ||
        record.payload.size() < QueueDataSize[record.type] )
    { error = "session_cpu_zone_protocol_record_invalid"; return false; }
    item = {};
    std::memcpy( &item, record.payload.data(),
        std::min<size_t>( record.payload.size(), sizeof( item ) ) );
    if( item.hdr.idx != record.type )
    { error = "session_cpu_zone_protocol_type_mismatch"; return false; }
    return true;
}

bool GetShortPayload( const TraceSessionCanonicalRecord& record,
    const uint8_t*& data, size_t& size, std::string& error )
{
    const auto fixed = size_t( QueueDataSize[record.type] );
    if( record.payload.size() < fixed + sizeof( uint16_t ) )
    { error = "session_cpu_zone_payload_truncated"; return false; }
    uint16_t bytes = 0;
    std::memcpy( &bytes, record.payload.data() + fixed, sizeof( bytes ) );
    if( bytes != record.variablePayloadBytes || record.payload.size() != fixed + sizeof( bytes ) + bytes )
    { error = "session_cpu_zone_payload_mismatch"; return false; }
    data = record.payload.data() + fixed + sizeof( bytes );
    size = bytes;
    return true;
}

class CpuZoneWriter
{
public:
    bool Open( const std::filesystem::path& root, const TraceSessionManifest& session,
        std::string& error )
    {
        m_root = root;
        m_session = &session;
        std::error_code ec;
        std::filesystem::create_directories( root, ec );
        if( ec ) { error = "session_cpu_zone_directory_failed:" + ec.message(); return false; }
        m_zonePath = root / "zones.work";
        m_extraPath = root / "extras.work";
        m_zones.open( m_zonePath, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc );
        m_extras.open( m_extraPath, std::ios::binary | std::ios::trunc );
        if( !m_zones || !m_extras ) { error = "session_cpu_zone_work_open_failed"; return false; }
        return true;
    }

    bool WriteZone( OpenZone& zone, std::string& error )
    {
        if( !zone.extraName.empty() )
        {
            zone.stored.extraNameOffset = m_extraBytes;
            zone.stored.extraNameBytes = uint32_t( zone.extraName.size() );
            m_extras.write( zone.extraName.data(), std::streamsize( zone.extraName.size() ) );
            m_extraBytes += zone.extraName.size();
        }
        if( !zone.extraText.empty() )
        {
            zone.stored.extraTextOffset = m_extraBytes;
            zone.stored.extraTextBytes = uint32_t( zone.extraText.size() );
            m_extras.write( zone.extraText.data(), std::streamsize( zone.extraText.size() ) );
            m_extraBytes += zone.extraText.size();
        }
        if( !m_extras ) { error = "session_cpu_zone_extra_write_failed"; return false; }
        const auto offset = zone.stored.id * sizeof( StoredZone );
        if( zone.stored.id != 0 && offset / sizeof( StoredZone ) != zone.stored.id )
        { error = "session_cpu_zone_offset_overflow"; return false; }
        m_zones.seekp( std::streamoff( offset ) );
        m_zones.write( reinterpret_cast<const char*>( &zone.stored ), sizeof( zone.stored ) );
        if( !m_zones ) { error = "session_cpu_zone_record_write_failed"; return false; }
        return true;
    }

    bool PatchCallsite( uint64_t id, const CallsiteState& callsite, std::string& error )
    {
        const auto offset = id * sizeof( StoredZone );
        m_zones.flush();
        m_zones.clear();
        m_zones.seekg( std::streamoff( offset ) );
        StoredZone zone;
        if( !m_zones.read( reinterpret_cast<char*>( &zone ), sizeof( zone ) ) || zone.id != id )
        { error = "session_cpu_zone_late_callsite_record_missing"; return false; }
        zone.callstack = callsite.callstack;
        zone.provenance = callsite.provenance;
        zone.unavailableReason = callsite.unavailableReason;
        m_zones.clear();
        m_zones.seekp( std::streamoff( offset ) );
        m_zones.write( reinterpret_cast<const char*>( &zone ), sizeof( zone ) );
        if( !m_zones ) { error = "session_cpu_zone_late_callsite_write_failed"; return false; }
        return true;
    }

    bool Finalize( const std::vector<SourceState>& sources,
        const std::vector<CallsiteState>& callsites,
        uint64_t zoneCount, uint64_t completeZones, CpuZoneManifest& manifest,
        std::string& error )
    {
        m_zones.flush(); m_extras.flush();
        if( !m_zones || !m_extras ) { error = "session_cpu_zone_work_flush_failed"; return false; }
        m_zones.close(); m_extras.close();

        constexpr uint32_t ZonesPerBlock = 4096;
        std::vector<StoredZoneBlock> blocks;
        blocks.reserve( size_t( ( zoneCount + ZonesPerBlock - 1 ) / ZonesPerBlock ) );
        std::ifstream zones( m_zonePath, std::ios::binary );
        if( !zones ) { error = "session_cpu_zone_work_read_failed"; return false; }
        const auto childWorkPath = m_root / "children.work";
        const auto childSortedPath = m_root / "children-sorted.work";
        std::ofstream childWork( childWorkPath, std::ios::binary | std::ios::trunc );
        if( !childWork ) { error = "session_cpu_zone_child_work_open_failed"; return false; }
        uint64_t childLinkCount = 0;
        for( uint64_t first = 0; first < zoneCount; first += ZonesPerBlock )
        {
            StoredZoneBlock block;
            block.firstZone = first;
            block.zoneCount = uint32_t( std::min<uint64_t>( ZonesPerBlock, zoneCount - first ) );
            block.minStartNs = std::numeric_limits<int64_t>::max();
            block.maxEndNs = std::numeric_limits<int64_t>::min();
            for( uint32_t index = 0; index < block.zoneCount; ++index )
            {
                StoredZone zone;
                if( !zones.read( reinterpret_cast<char*>( &zone ), sizeof( zone ) ) ||
                    zone.id != first + index )
                { error = "session_cpu_zone_block_source_invalid"; return false; }
                const auto end = ( zone.flags & ZoneComplete ) != 0 ? zone.endNs : zone.startNs;
                block.minStartNs = std::min( block.minStartNs, std::min( zone.startNs, end ) );
                block.maxEndNs = std::max( block.maxEndNs, std::max( zone.startNs, end ) );
                AddThread( block, zone.thread );
                if( zone.parent != InvalidZoneId )
                {
                    const TraceSessionUInt64Pair link { zone.parent, zone.id };
                    childWork.write( reinterpret_cast<const char*>( &link ), sizeof( link ) );
                    ++childLinkCount;
                }
            }
            blocks.emplace_back( block );
        }
        if( zones.peek() != std::char_traits<char>::eof() )
        { error = "session_cpu_zone_block_source_trailing_bytes"; return false; }
        zones.clear();
        zones.seekg( 0 );
        childWork.flush();
        if( !childWork ) { error = "session_cpu_zone_child_work_write_failed"; return false; }
        childWork.close();
        constexpr uint64_t MaximumBufferedChildLinks = 4ull * 1024 * 1024;
        if( !SortTraceSessionUInt64Pairs( childWorkPath, childSortedPath, m_root,
            "cpu-child", childLinkCount, MaximumBufferedChildLinks, error ) ) return false;

        const auto target = m_root / CpuZoneFileName;
        auto temporary = target; temporary += ".tmp";
        std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
        if( !out ) { error = "session_cpu_zone_file_open_failed"; return false; }
        CpuZoneFileHeader header;
        header.sourceSize = m_session->source.fileSize;
        header.zoneCount = zoneCount;
        header.completeZoneCount = completeZones;
        header.sourceCount = sources.size();
        header.zoneBlockCount = blocks.size();
        header.callsiteCount = callsites.size();
        header.childLinkCount = childLinkCount;
        header.generationBytes = uint32_t( m_session->generation.size() );
        header.zonesOffset = sizeof( header ) + m_session->source.sha256.size() + m_session->generation.size();
        header.zoneBlocksOffset = header.zonesOffset + zoneCount * sizeof( StoredZone );
        header.extrasOffset = header.zoneBlocksOffset + blocks.size() * sizeof( StoredZoneBlock );
        header.sourcesOffset = header.extrasOffset + m_extraBytes;
        uint64_t sourceBytes = 0;
        for( const auto& source : sources ) sourceBytes += sizeof( StoredSource ) +
            source.name.size() + source.function.size() + source.file.size();
        header.callsitesOffset = header.sourcesOffset + sourceBytes;
        header.childLinksOffset = header.callsitesOffset +
            callsites.size() * sizeof( StoredCallsite );
        out.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
        out.write( m_session->source.sha256.data(), std::streamsize( m_session->source.sha256.size() ) );
        out.write( m_session->generation.data(), std::streamsize( m_session->generation.size() ) );
        std::ifstream extras( m_extraPath, std::ios::binary );
        if( !extras || !CopyFileBytes( zones, out, error ) ) return false;
        if( !blocks.empty() ) out.write( reinterpret_cast<const char*>( blocks.data() ),
            std::streamsize( blocks.size() * sizeof( StoredZoneBlock ) ) );
        if( !out || !CopyFileBytes( extras, out, error ) ) return false;
        zones.close();
        extras.close();
        for( const auto& source : sources )
        {
            StoredSource stored;
            stored.nativeId = source.nativeId;
            stored.line = source.line;
            stored.color = source.color;
            stored.nameBytes = uint32_t( source.name.size() );
            stored.functionBytes = uint32_t( source.function.size() );
            stored.fileBytes = uint32_t( source.file.size() );
            stored.flags = ( source.dynamic ? 1u : 0u ) | ( source.definitionReceived ? 2u : 0u );
            out.write( reinterpret_cast<const char*>( &stored ), sizeof( stored ) );
            out.write( source.name.data(), std::streamsize( source.name.size() ) );
            out.write( source.function.data(), std::streamsize( source.function.size() ) );
            out.write( source.file.data(), std::streamsize( source.file.size() ) );
        }
        for( const auto& callsite : callsites )
        {
            const StoredCallsite stored { callsite.thread, callsite.callsiteId,
                callsite.sourceNativeId, callsite.callstack, callsite.domain,
                callsite.provenance, callsite.flags, callsite.unavailableReason };
            out.write( reinterpret_cast<const char*>( &stored ), sizeof( stored ) );
        }
        std::ifstream childLinks( childSortedPath, std::ios::binary );
        if( !childLinks || !CopyFileBytes( childLinks, out, error ) ) return false;
        childLinks.close();
        out.flush();
        if( !out ) { error = "session_cpu_zone_file_write_failed"; return false; }
        out.close();
        if( !AtomicReplace( temporary, target, error ) ) return false;
        std::error_code ec;
        if( !std::filesystem::remove( m_zonePath, ec ) || ec )
        { error = "session_cpu_zone_work_cleanup_failed:" + ( ec ? ec.message() : m_zonePath.string() ); return false; }
        ec.clear();
        if( !std::filesystem::remove( m_extraPath, ec ) || ec )
        { error = "session_cpu_zone_work_cleanup_failed:" + ( ec ? ec.message() : m_extraPath.string() ); return false; }
        ec.clear();
        if( !std::filesystem::remove( childWorkPath, ec ) || ec )
        { error = "session_cpu_zone_child_work_cleanup_failed:" + ( ec ? ec.message() : childWorkPath.string() ); return false; }
        ec.clear();
        if( !std::filesystem::remove( childSortedPath, ec ) || ec )
        { error = "session_cpu_zone_child_sorted_cleanup_failed:" + ( ec ? ec.message() : childSortedPath.string() ); return false; }
        manifest.sourceSha256 = m_session->source.sha256;
        manifest.sourceSize = m_session->source.fileSize;
        manifest.generation = m_session->generation;
        manifest.fileBytes = std::filesystem::file_size( target, ec );
        if( ec ) { error = "session_cpu_zone_file_size_failed:" + ec.message(); return false; }
        manifest.fileSha256 = Sha256File( target );
        manifest.stats.zones = zoneCount;
        manifest.stats.zoneBlocks = blocks.size();
        manifest.stats.childLinks = childLinkCount;
        manifest.stats.completeZones = completeZones;
        manifest.stats.sourceLocations = sources.size();
        manifest.stats.fileBytes = manifest.fileBytes;
        return true;
    }

private:
    std::filesystem::path m_root, m_zonePath, m_extraPath;
    const TraceSessionManifest* m_session = nullptr;
    std::fstream m_zones;
    std::ofstream m_extras;
    uint64_t m_extraBytes = 0;
};

struct BuildState
{
    TraceSessionTimeTransform transform;
    CpuZoneWriter writer;
    std::unordered_map<uint64_t, std::string> strings;
    std::unordered_map<uint64_t, size_t> staticSources;
    std::unordered_map<std::string, size_t> dynamicSources;
    std::deque<size_t> sourceResponseQueue;
    std::vector<SourceState> sources;
    std::unordered_map<uint32_t, CallsiteState> callsites;
    std::vector<CallsiteState> callsiteRecords;
    std::unordered_map<uint32_t, std::vector<uint64_t>> pendingCallsiteZones;
    std::unordered_map<std::string, uint32_t> callstackIds;
    std::unordered_map<uint64_t, uint32_t> nextCallstack;
    std::unordered_map<uint64_t, std::vector<OpenZone>> open;
    std::unordered_map<uint64_t, uint32_t> nextValidation;
    std::unordered_map<uint64_t, uint64_t> fiberIds;
    std::unordered_map<uint64_t, uint64_t> activeFiber;
    uint64_t nextFiberId = uint64_t( 1 ) << 32;
    uint32_t threadContext = 0;
    int64_t refTimeThread = 0;
    uint32_t pendingCallstack = 0;
    uint32_t serialNextCallstack = 0;
    std::optional<size_t> pendingDynamicSource;
    std::optional<std::string> pendingSingleString;
    uint64_t zoneCount = 0;
    uint64_t completeZones = 0;
    uint64_t invalidTimingZones = 0;
    uint64_t beginEvents = 0;
    uint64_t endEvents = 0;
    std::string error;
};

uint64_t LogicalThread( const BuildState& state )
{
    const auto found = state.activeFiber.find( state.threadContext );
    return found == state.activeFiber.end() ? state.threadContext : found->second;
}

int64_t AdvanceThreadTime( BuildState& state, int64_t delta )
{
    state.refTimeThread += delta;
    return state.transform.ToNanoseconds( state.refTimeThread );
}

uint32_t InternCallstack( BuildState& state, const uint8_t* data,
    size_t size, std::string& error )
{
    if( size % sizeof( uint64_t ) != 0 )
    { error = "session_cpu_zone_callstack_payload_invalid"; return 0; }
    std::string key( reinterpret_cast<const char*>( data ), size );
    const auto found = state.callstackIds.find( key );
    if( found != state.callstackIds.end() ) return found->second;
    if( state.callstackIds.size() >= std::numeric_limits<uint32_t>::max() - 1 )
    { error = "session_cpu_zone_callstack_id_overflow"; return 0; }
    const auto id = uint32_t( state.callstackIds.size() + 1 );
    state.callstackIds.emplace( std::move( key ), id );
    return id;
}

size_t EnsureStaticSource( BuildState& state, uint64_t pointer )
{
    const auto found = state.staticSources.find( pointer );
    if( found != state.staticSources.end() ) return found->second;
    SourceState source;
    source.nativeId = int32_t( state.staticSources.size() );
    source.pointer = pointer;
    const auto index = state.sources.size();
    state.sources.emplace_back( std::move( source ) );
    state.staticSources.emplace( pointer, index );
    state.sourceResponseQueue.push_back( index );
    return index;
}

bool ParseDynamicSource( BuildState& state, const uint8_t* data,
    size_t size, std::string& error )
{
    if( size < 10 ) { error = "session_cpu_zone_dynamic_source_truncated"; return false; }
    std::string key( reinterpret_cast<const char*>( data ), size );
    const auto existing = state.dynamicSources.find( key );
    if( existing != state.dynamicSources.end() )
    { state.pendingDynamicSource = existing->second; return true; }
    uint32_t color = 0, line = 0;
    std::memcpy( &color, data, 4 ); std::memcpy( &line, data + 4, 4 );
    size_t offset = 8;
    const auto takeZ = [&]( std::string& value ) -> bool {
        const auto begin = offset;
        while( offset < size && data[offset] != 0 ) ++offset;
        if( offset >= size ) return false;
        value.assign( reinterpret_cast<const char*>( data + begin ), offset - begin );
        ++offset; return true;
    };
    SourceState source;
    source.dynamic = true;
    source.nativeId = -int32_t( state.dynamicSources.size() + 1 );
    source.line = line;
    source.color = ( ( color & 0x00FF0000 ) >> 16 ) | ( color & 0x0000FF00 ) | ( ( color & 0x000000FF ) << 16 );
    if( !takeZ( source.function ) || !takeZ( source.file ) )
    { error = "session_cpu_zone_dynamic_source_invalid"; return false; }
    source.name.assign( reinterpret_cast<const char*>( data + offset ), size - offset );
    source.definitionReceived = true;
    const auto index = state.sources.size();
    state.sources.emplace_back( std::move( source ) );
    state.dynamicSources.emplace( std::move( key ), index );
    state.pendingDynamicSource = index;
    return true;
}

bool WriteClosedZone( BuildState& state, OpenZone zone, int64_t endNs,
    bool complete, bool timingValid, std::string& error )
{
    if( complete )
    {
        zone.stored.endNs = endNs;
        zone.stored.selfTimeNs = timingValid ?
            std::max<int64_t>( 0, endNs - zone.stored.startNs - zone.childTimeNs ) : 0;
        zone.stored.flags |= ZoneComplete;
        if( !timingValid ) zone.stored.flags &= ~ZoneTimingValid;
    }
    else
    {
        zone.stored.endNs = zone.stored.startNs;
        zone.stored.selfTimeNs = 0;
    }
    return state.writer.WriteZone( zone, error );
}

bool BeginZone( BuildState& state, int64_t delta, size_t sourceIndex,
    uint32_t callstack, uint32_t callsiteId, std::string& error )
{
    const auto logicalThread = LogicalThread( state );
    auto& stack = state.open[logicalThread];
    OpenZone zone;
    zone.stored.id = state.zoneCount++;
    zone.stored.parent = stack.empty() ? InvalidZoneId : stack.back().stored.id;
    zone.stored.thread = logicalThread;
    zone.stored.startNs = AdvanceThreadTime( state, delta );
    zone.stored.sourceNativeId = state.sources[sourceIndex].nativeId;
    zone.stored.callstack = callstack;
    zone.stored.callsiteId = callsiteId;
    zone.validationId = state.nextValidation[logicalThread];
    state.nextValidation[logicalThread] = 0;
    if( callsiteId != 0 )
    {
        const auto found = state.callsites.find( callsiteId );
        if( found != state.callsites.end() )
        {
            zone.stored.callstack = found->second.callstack;
            zone.stored.provenance = found->second.provenance;
            zone.stored.unavailableReason = found->second.unavailableReason;
        }
        else state.pendingCallsiteZones[callsiteId].push_back( zone.stored.id );
    }
    if( !stack.empty() ) stack.back().stored.childCount++;
    stack.emplace_back( std::move( zone ) );
    state.beginEvents++;
    return true;
}

bool EndZone( BuildState& state, int64_t delta, std::string& error )
{
    const auto logicalThread = LogicalThread( state );
    auto& stack = state.open[logicalThread];
    if( stack.empty() ) { error = "session_cpu_zone_end_without_begin"; return false; }
    if( stack.back().validationId != state.nextValidation[logicalThread] )
    {
        error = "session_cpu_zone_validation_mismatch:thread=" + std::to_string( logicalThread ) +
            ":zone=" + std::to_string( stack.back().validationId ) +
            ":end=" + std::to_string( state.nextValidation[logicalThread] );
        return false;
    }
    state.nextValidation[logicalThread] = 0;
    const auto endNs = AdvanceThreadTime( state, delta );
    auto zone = std::move( stack.back() ); stack.pop_back();
    const auto duration = endNs - zone.stored.startNs;
    if( !stack.empty() ) stack.back().childTimeNs += std::max<int64_t>( 0, duration );
    const auto timingValid = endNs >= zone.stored.startNs;
    if( !WriteClosedZone( state, std::move( zone ), endNs, true, timingValid, error ) ) return false;
    state.completeZones++;
    if( !timingValid ) state.invalidTimingZones++;
    state.endEvents++;
    return true;
}

bool ConsumeThreadCallstack( BuildState& state, uint32_t& callstack,
    std::string& error )
{
    auto& next = state.nextCallstack[LogicalThread( state )];
    if( next == 0 ) { error = "session_cpu_zone_thread_callstack_missing"; return false; }
    callstack = next;
    next = 0;
    return true;
}

bool ConsumeThreadCallstack( BuildState& state, std::string& error )
{
    uint32_t ignored = 0;
    return ConsumeThreadCallstack( state, ignored, error );
}

bool ResolvePendingCallsite( BuildState& state, uint32_t callsiteId,
    const CallsiteState& callsite, std::string& error )
{
    const auto pending = state.pendingCallsiteZones.find( callsiteId );
    if( pending == state.pendingCallsiteZones.end() ) return true;
    for( const auto id : pending->second )
    {
        bool open = false;
        for( auto& [thread, stack] : state.open )
        {
            const auto found = std::find_if( stack.begin(), stack.end(),
                [id]( const auto& zone ) { return zone.stored.id == id; } );
            if( found == stack.end() ) continue;
            found->stored.callstack = callsite.callstack;
            found->stored.provenance = callsite.provenance;
            found->stored.unavailableReason = callsite.unavailableReason;
            open = true;
            break;
        }
        if( !open && !state.writer.PatchCallsite( id, callsite, error ) ) return false;
    }
    state.pendingCallsiteZones.erase( pending );
    return true;
}

bool VisitCpuZoneRecord( const TraceSessionCanonicalRecord& record,
    void* userData, std::string& error )
{
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ) return true;
    auto& state = *static_cast<BuildState*>( userData );
    QueueItem item {};
    if( !DecodeItem( record, item, error ) ) return false;
    const auto type = QueueType( record.type );
    switch( type )
    {
    case QueueType::ThreadContext:
        state.threadContext = item.threadCtx.thread;
        state.refTimeThread = 0;
        break;
    case QueueType::StringData:
    {
        const uint8_t* data = nullptr; size_t size = 0;
        if( !GetShortPayload( record, data, size, error ) ) return false;
        const std::string value( reinterpret_cast<const char*>( data ), size );
        const auto [found, inserted] = state.strings.emplace( item.stringTransfer.ptr, value );
        if( !inserted && found->second != value )
        { error = "session_cpu_zone_string_conflict"; return false; }
        break;
    }
    case QueueType::SingleStringData:
    {
        const uint8_t* data = nullptr; size_t size = 0;
        if( !GetShortPayload( record, data, size, error ) ) return false;
        // SingleStringData is a shared protocol staging slot. Callstack frame,
        // symbol, message, lock and GPU-context responses also use it. The
        // immediately following consumer determines ownership, so a later
        // string must replace an unrelated value instead of invalidating the
        // CPU-zone domain.
        state.pendingSingleString = std::string( reinterpret_cast<const char*>( data ), size );
        break;
    }
    case QueueType::SourceLocation:
    {
        if( state.sourceResponseQueue.empty() )
        { error = "session_cpu_zone_source_response_without_request"; return false; }
        auto& source = state.sources[state.sourceResponseQueue.front()];
        state.sourceResponseQueue.pop_front();
        source.namePointer = item.srcloc.name;
        source.functionPointer = item.srcloc.function;
        source.filePointer = item.srcloc.file;
        source.line = item.srcloc.line;
        source.color = ( uint32_t( item.srcloc.b ) << 16 ) |
            ( uint32_t( item.srcloc.g ) << 8 ) | item.srcloc.r;
        source.definitionReceived = true;
        break;
    }
    case QueueType::SourceLocationPayload:
    {
        const uint8_t* data = nullptr; size_t size = 0;
        if( !GetShortPayload( record, data, size, error ) || state.pendingDynamicSource )
        { if( error.empty() ) error = "session_cpu_zone_dynamic_source_sequence_invalid"; return false; }
        if( !ParseDynamicSource( state, data, size, error ) ) return false;
        break;
    }
    case QueueType::CallstackPayload:
    {
        const uint8_t* data = nullptr; size_t size = 0;
        if( !GetShortPayload( record, data, size, error ) || state.pendingCallstack != 0 )
        { if( error.empty() ) error = "session_cpu_zone_callstack_sequence_invalid"; return false; }
        state.pendingCallstack = InternCallstack( state, data, size, error );
        if( state.pendingCallstack == 0 ) return false;
        break;
    }
    case QueueType::CallstackSampleDictionary:
    {
        const uint8_t* data = nullptr; size_t size = 0;
        if( !GetShortPayload( record, data, size, error ) || InternCallstack( state, data, size, error ) == 0 ) return false;
        break;
    }
    case QueueType::CallstackSerial:
        if( state.pendingCallstack == 0 || state.serialNextCallstack != 0 )
        { error = "session_cpu_zone_serial_callstack_sequence_invalid"; return false; }
        state.serialNextCallstack = state.pendingCallstack; state.pendingCallstack = 0;
        break;
    case QueueType::Callstack:
    case QueueType::CallstackAlloc:
        if( state.pendingCallstack == 0 )
        { error = "session_cpu_zone_thread_callstack_sequence_invalid"; return false; }
        state.nextCallstack[LogicalThread( state )] = state.pendingCallstack;
        state.pendingCallstack = 0;
        break;
    case QueueType::CallstackSample:
    case QueueType::CallstackSampleContextSwitch:
        if( state.pendingCallstack == 0 )
        { error = "session_cpu_zone_sample_callstack_sequence_invalid"; return false; }
        state.pendingCallstack = 0;
        break;
    case QueueType::JnCallsiteDefinition:
    {
        const auto sourceIndex = EnsureStaticSource( state, item.jnCallsiteDefinition.srcloc );
        CallsiteState callsite;
        callsite.thread = item.jnCallsiteDefinition.thread;
        callsite.callsiteId = item.jnCallsiteDefinition.callsiteId;
        callsite.sourceNativeId = state.sources[sourceIndex].nativeId;
        callsite.domain = item.jnCallsiteDefinition.domain;
        callsite.provenance = item.jnCallsiteDefinition.provenance;
        callsite.flags = item.jnCallsiteDefinition.flags;
        callsite.unavailableReason = item.jnCallsiteDefinition.unavailableReason;
        if( item.jnCallsiteDefinition.flags & uint8_t( JnCallsiteFlags::HasCallstack ) )
        {
            if( state.serialNextCallstack == 0 )
            { error = "session_cpu_zone_callsite_stack_missing"; return false; }
            callsite.callstack = state.serialNextCallstack;
            state.serialNextCallstack = 0;
        }
        if( !state.callsites.emplace( item.jnCallsiteDefinition.callsiteId, callsite ).second )
        { error = "session_cpu_zone_callsite_duplicate"; return false; }
        state.callsiteRecords.emplace_back( callsite );
        if( !ResolvePendingCallsite( state, item.jnCallsiteDefinition.callsiteId, callsite, error ) ) return false;
        break;
    }
    case QueueType::ZoneValidation:
        state.nextValidation[LogicalThread( state )] = item.zoneValidation.id;
        break;
    case QueueType::ZoneBegin:
    case QueueType::ZoneBeginCallstack:
    case QueueType::JnZoneBeginCallsite:
    {
        const auto source = EnsureStaticSource( state, item.zoneBegin.srcloc );
        uint32_t callstack = 0, callsite = 0;
        if( type == QueueType::ZoneBeginCallstack )
        { if( !ConsumeThreadCallstack( state, callstack, error ) ) return false; }
        else if( type == QueueType::JnZoneBeginCallsite ) callsite = item.jnZoneBeginCallsite.callsiteId;
        if( !BeginZone( state, item.zoneBegin.time, source, callstack, callsite, error ) ) return false;
        break;
    }
    case QueueType::ZoneBeginAllocSrcLoc:
    case QueueType::ZoneBeginAllocSrcLocCallstack:
    {
        if( !state.pendingDynamicSource )
        { error = "session_cpu_zone_dynamic_source_missing"; return false; }
        uint32_t callstack = 0;
        if( type == QueueType::ZoneBeginAllocSrcLocCallstack )
        { if( !ConsumeThreadCallstack( state, callstack, error ) ) return false; }
        const auto source = *state.pendingDynamicSource;
        state.pendingDynamicSource.reset();
        if( !BeginZone( state, item.zoneBeginLean.time, source, callstack, 0, error ) ) return false;
        break;
    }
    case QueueType::ZoneEnd:
        if( !EndZone( state, item.zoneEnd.time, error ) ) return false;
        break;
    case QueueType::ZoneName:
    case QueueType::ZoneText:
    {
        if( !state.pendingSingleString )
        { error = "session_cpu_zone_extra_string_missing"; return false; }
        auto& stack = state.open[LogicalThread( state )];
        if( stack.empty() ) { error = "session_cpu_zone_extra_without_zone"; return false; }
        if( type == QueueType::ZoneName ) stack.back().extraName = *state.pendingSingleString;
        else
        {
            if( !stack.back().extraText.empty() ) stack.back().extraText.push_back( '\n' );
            stack.back().extraText += *state.pendingSingleString;
        }
        state.pendingSingleString.reset();
        break;
    }
    case QueueType::ZoneColor:
    {
        auto& stack = state.open[LogicalThread( state )];
        if( stack.empty() ) { error = "session_cpu_zone_color_without_zone"; return false; }
        stack.back().stored.extraColor = ( uint32_t( item.zoneColor.b ) << 16 ) |
            ( uint32_t( item.zoneColor.g ) << 8 ) | item.zoneColor.r;
        break;
    }
    case QueueType::PlotDataInt: AdvanceThreadTime( state, item.plotDataInt.time ); break;
    case QueueType::PlotDataFloat: AdvanceThreadTime( state, item.plotDataFloat.time ); break;
    case QueueType::PlotDataDouble: AdvanceThreadTime( state, item.plotDataDouble.time ); break;
    case QueueType::GpuZoneBegin:
    case QueueType::GpuZoneBeginCallstack:
    case QueueType::JnGpuZoneBeginCallsite:
        EnsureStaticSource( state, item.gpuZoneBegin.srcloc );
        AdvanceThreadTime( state, item.gpuZoneBegin.cpuTime );
        if( type == QueueType::GpuZoneBeginCallstack && !ConsumeThreadCallstack( state, error ) ) return false;
        break;
    case QueueType::GpuZoneBeginAllocSrcLoc:
    case QueueType::GpuZoneBeginAllocSrcLocCallstack:
        if( !state.pendingDynamicSource )
        { error = "session_cpu_zone_gpu_dynamic_source_missing"; return false; }
        state.pendingDynamicSource.reset();
        AdvanceThreadTime( state, item.gpuZoneBeginLean.cpuTime );
        if( type == QueueType::GpuZoneBeginAllocSrcLocCallstack && !ConsumeThreadCallstack( state, error ) ) return false;
        break;
    case QueueType::GpuZoneEnd: AdvanceThreadTime( state, item.gpuZoneEnd.cpuTime ); break;
    case QueueType::FiberEnter:
    {
        AdvanceThreadTime( state, item.fiberEnter.time );
        auto found = state.fiberIds.find( item.fiberEnter.fiber );
        if( found == state.fiberIds.end() ) found = state.fiberIds.emplace( item.fiberEnter.fiber, state.nextFiberId++ ).first;
        state.activeFiber[item.fiberEnter.thread] = found->second;
        break;
    }
    case QueueType::FiberLeave:
        AdvanceThreadTime( state, item.fiberLeave.time );
        state.activeFiber.erase( item.fiberLeave.thread );
        break;
    case QueueType::MessageCallstack:
    case QueueType::MessageLiteralCallstack:
    case QueueType::MessageColorCallstack:
    case QueueType::MessageLiteralColorCallstack:
        if( !ConsumeThreadCallstack( state, error ) ) return false;
        break;
    case QueueType::GpuZoneBeginCallstackSerial:
        EnsureStaticSource( state, item.gpuZoneBegin.srcloc );
        state.serialNextCallstack = 0;
        break;
    case QueueType::GpuZoneBeginSerial:
        EnsureStaticSource( state, item.gpuZoneBegin.srcloc );
        break;
    case QueueType::GpuZoneBeginAllocSrcLocSerial:
        if( !state.pendingDynamicSource )
        { error = "session_cpu_zone_gpu_serial_dynamic_source_missing"; return false; }
        state.pendingDynamicSource.reset();
        break;
    case QueueType::GpuZoneBeginAllocSrcLocCallstackSerial:
        if( !state.pendingDynamicSource )
        { error = "session_cpu_zone_gpu_serial_dynamic_source_missing"; return false; }
        state.pendingDynamicSource.reset();
        state.serialNextCallstack = 0;
        break;
    case QueueType::MemAllocCallstack:
    case QueueType::MemAllocCallstackNamed:
    case QueueType::MemFreeCallstack:
    case QueueType::MemFreeCallstackNamed:
    case QueueType::MemDiscardCallstack:
        state.serialNextCallstack = 0;
        break;
    case QueueType::JnJobStage:
        if( JnJobStage( item.jnJobStage.stage ) == JnJobStage::ScheduleCallstack ||
            JnJobStage( item.jnJobStage.stage ) == JnJobStage::WaitCallstack ) state.serialNextCallstack = 0;
        break;
    case QueueType::JnIoStage:
        if( JnIoStage( item.jnIoStage.stage ) == JnIoStage::RequestCallstack ) state.serialNextCallstack = 0;
        break;
    case QueueType::LockAnnounce: EnsureStaticSource( state, item.lockAnnounce.lckloc ); break;
    case QueueType::LockMark: EnsureStaticSource( state, item.lockMark.srcloc ); break;
    default: break;
    }
    return true;
}

bool SaveCpuZoneManifest( const std::filesystem::path& root,
    const CpuZoneManifest& value, std::string& error )
{
    const auto temporary = root / "manifest.tmp";
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_cpu_zone_manifest_open_failed"; return false; }
    out << "magic " << CpuZoneManifestMagic << '\n';
    out << "schema " << TraceSessionCpuZoneIndexSchemaVersion << '\n';
    out << "source_sha256 " << std::quoted( value.sourceSha256 ) << '\n';
    out << "source_size " << value.sourceSize << '\n';
    out << "generation " << std::quoted( value.generation ) << '\n';
    out << "file_bytes " << value.fileBytes << '\n';
    out << "file_sha256 " << std::quoted( value.fileSha256 ) << '\n';
    out << "zones " << value.stats.zones << '\n';
    out << "zone_blocks " << value.stats.zoneBlocks << '\n';
    out << "child_links " << value.stats.childLinks << '\n';
    out << "complete_zones " << value.stats.completeZones << '\n';
    out << "invalid_timing_zones " << value.stats.invalidTimingZones << '\n';
    out << "source_locations " << value.stats.sourceLocations << '\n';
    out << "begin_events " << value.stats.beginEvents << '\n';
    out << "end_events " << value.stats.endEvents << '\n';
    out.flush();
    if( !out ) { error = "session_cpu_zone_manifest_write_failed"; return false; }
    out.close();
    return AtomicReplace( temporary, root / "manifest", error );
}

bool LoadCpuZoneManifest( const std::filesystem::path& root,
    CpuZoneManifest& value, std::string& error )
{
    value = {};
    std::ifstream in( root / "manifest", std::ios::binary );
    if( !in ) { error = "session_cpu_zone_manifest_not_found"; return false; }
    uint64_t magic = 0; uint32_t schema = 0; std::string key;
    while( in >> key )
    {
        if( key == "magic" ) in >> magic;
        else if( key == "schema" ) in >> schema;
        else if( key == "source_sha256" ) in >> std::quoted( value.sourceSha256 );
        else if( key == "source_size" ) in >> value.sourceSize;
        else if( key == "generation" ) in >> std::quoted( value.generation );
        else if( key == "file_bytes" ) in >> value.fileBytes;
        else if( key == "file_sha256" ) in >> std::quoted( value.fileSha256 );
        else if( key == "zones" ) in >> value.stats.zones;
        else if( key == "zone_blocks" ) in >> value.stats.zoneBlocks;
        else if( key == "child_links" ) in >> value.stats.childLinks;
        else if( key == "complete_zones" ) in >> value.stats.completeZones;
        else if( key == "invalid_timing_zones" ) in >> value.stats.invalidTimingZones;
        else if( key == "source_locations" ) in >> value.stats.sourceLocations;
        else if( key == "begin_events" ) in >> value.stats.beginEvents;
        else if( key == "end_events" ) in >> value.stats.endEvents;
        else { std::string ignored; std::getline( in, ignored ); }
        if( !in ) { error = "session_cpu_zone_manifest_parse_failed"; return false; }
    }
    value.stats.fileBytes = value.fileBytes;
    if( magic != CpuZoneManifestMagic || schema != TraceSessionCpuZoneIndexSchemaVersion ||
        value.sourceSha256.size() != 64 || value.fileSha256.size() != 64 )
    { error = "session_cpu_zone_manifest_invalid"; return false; }
    return true;
}

std::string MakeRef( const std::string& fingerprint, const char* kind, uint64_t id )
{
    std::ostringstream out;
    out << "tracy:v1:" << fingerprint.substr( 0, 16 ) << ':' << kind << ':' << std::hex << id;
    return out.str();
}

std::optional<uint64_t> ParseThreadRef( const std::string& fingerprint,
    std::string_view ref )
{
    const auto prefix = std::string( "tracy:v1:" ) + fingerprint.substr( 0, 16 ) + ":thread:";
    if( !ref.starts_with( prefix ) ) return std::nullopt;
    uint64_t value = 0;
    const auto first = ref.data() + prefix.size();
    const auto last = ref.data() + ref.size();
    const auto parsed = std::from_chars( first, last, value, 16 );
    if( parsed.ec != std::errc {} || parsed.ptr != last || value == 0 ) return std::nullopt;
    return value;
}

uint64_t MixThread( uint64_t value )
{
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ull;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebull;
    return value ^ ( value >> 31 );
}

void AddThread( StoredZoneBlock& block, uint64_t thread )
{
    const auto mixed = MixThread( thread );
    const std::array<uint8_t, 3> bits = {
        uint8_t( mixed ), uint8_t( mixed >> 21 ), uint8_t( mixed >> 42 ) };
    for( const auto bit : bits ) block.threadBloom[bit >> 6] |= uint64_t( 1 ) << ( bit & 63 );
}

bool MayContainThread( const StoredZoneBlock& block, uint64_t thread )
{
    const auto mixed = MixThread( thread );
    const std::array<uint8_t, 3> bits = {
        uint8_t( mixed ), uint8_t( mixed >> 21 ), uint8_t( mixed >> 42 ) };
    for( const auto bit : bits )
    {
        if( ( block.threadBloom[bit >> 6] & ( uint64_t( 1 ) << ( bit & 63 ) ) ) == 0 ) return false;
    }
    return true;
}

const char* ProvenanceName( uint8_t value )
{
    switch( JnStackProvenance( value ) )
    {
    case JnStackProvenance::ExactSource: return "ExactSource";
    case JnStackProvenance::SiteReused: return "SiteReused";
    case JnStackProvenance::PerEventExact: return "PerEventExact";
    case JnStackProvenance::Unavailable: return "Unavailable";
    }
    return "Unavailable";
}

const char* UnavailableReasonName( uint8_t value )
{
    switch( value )
    {
    case 0: return "";
    case 1: return "depth_zero";
    case 2: return "callstack_unsupported";
    case 3: return "admission_denied";
    case 4: return "capacity";
    default: return "unknown";
    }
}

}

struct TraceSessionCpuZoneReader::Impl
{
    std::filesystem::path path;
    std::string fingerprint;
    uint64_t zonesOffset = 0;
    uint64_t extrasOffset = 0;
    uint64_t zoneCount = 0;
    uint64_t childLinksOffset = 0;
    uint64_t childLinkCount = 0;
    std::vector<StoredZoneBlock> zoneBlocks;
    std::unordered_map<int32_t, SourceState> sources;
    std::vector<StoredCallsite> callsites;

    bool ReadZone( uint64_t id, StoredZone& zone ) const
    {
        if( id >= zoneCount ) return false;
        std::ifstream in( path, std::ios::binary );
        if( !in ) return false;
        in.seekg( std::streamoff( zonesOffset + id * sizeof( StoredZone ) ) );
        return bool( in.read( reinterpret_cast<char*>( &zone ), sizeof( zone ) ) ) && zone.id == id;
    }

    std::string ReadExtra( uint64_t offset, uint32_t bytes ) const
    {
        if( bytes == 0 ) return {};
        std::ifstream in( path, std::ios::binary );
        if( !in ) return {};
        std::string value( bytes, '\0' );
        in.seekg( std::streamoff( extrasOffset + offset ) );
        if( !in.read( value.data(), std::streamsize( value.size() ) ) ) return {};
        return value;
    }

    CpuZoneDto ToDto( const StoredZone& zone ) const
    {
        CpuZoneDto dto;
        dto.ref = MakeRef( fingerprint, "cpu-zone", zone.id );
        dto.threadRef = MakeRef( fingerprint, "thread", zone.thread );
        dto.sourceLocationRef = MakeRef( fingerprint, "source", uint16_t( zone.sourceNativeId ) );
        const auto found = sources.find( zone.sourceNativeId );
        if( found != sources.end() )
        {
            const auto& source = found->second;
            dto.function = source.function;
            dto.file = source.file;
            dto.line = source.line;
            dto.name = source.name.empty() ? source.function : source.name;
            dto.nameResolved = source.definitionReceived && !source.function.empty();
        }
        else dto.nameResolved = false;
        if( zone.parent != InvalidZoneId ) dto.parentRef = MakeRef( fingerprint, "cpu-zone", zone.parent );
        dto.startNs = zone.startNs;
        dto.complete = ( zone.flags & ZoneComplete ) != 0;
        dto.timingValid = ( zone.flags & ZoneTimingValid ) != 0;
        if( !dto.timingValid ) dto.timingInvalidReason = "source_clock_inversion";
        if( dto.complete )
        {
            dto.endNs = zone.endNs;
            if( dto.timingValid ) dto.selfTimeNs = zone.selfTimeNs;
        }
        dto.childCount = zone.childCount;
        dto.callstack = zone.callstack;
        if( zone.callstack != 0 ) dto.callstackRef = MakeRef( fingerprint, "callstack", zone.callstack );
        if( zone.callsiteId != 0 ) dto.callsiteId = zone.callsiteId;
        dto.stackProvenance = ProvenanceName( zone.provenance );
        if( zone.callstack != 0 ) dto.stackRef = MakeRef( fingerprint, "callstack", zone.callstack );
        if( const auto* reason = UnavailableReasonName( zone.unavailableReason ); *reason != '\0' ) dto.stackUnavailableReason = reason;
        dto.extraColor = zone.extraColor;
        if( zone.extraNameBytes != 0 )
        {
            dto.extraName = ReadExtra( zone.extraNameOffset, zone.extraNameBytes );
            dto.name = *dto.extraName;
            dto.extraIndex = uint32_t( std::min<uint64_t>( zone.id + 1, std::numeric_limits<uint32_t>::max() ) );
        }
        if( zone.extraTextBytes != 0 )
        {
            dto.extraText = ReadExtra( zone.extraTextOffset, zone.extraTextBytes );
            if( dto.extraIndex == 0 ) dto.extraIndex = uint32_t( std::min<uint64_t>( zone.id + 1, std::numeric_limits<uint32_t>::max() ) );
        }
        dto.extraValid = ( zone.flags & ZoneExtraValid ) != 0;
        return dto;
    }
};

TraceSessionCpuZoneReader::TraceSessionCpuZoneReader( std::shared_ptr<Impl> impl )
    : m_impl( std::move( impl ) )
{}

std::filesystem::path TraceSessionCpuZoneIndexRoot( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest )
{
    return sessionRoot / "generations" / manifest.generation / "derived" /
        "cpu-zone-index" / std::to_string( TraceSessionCpuZoneIndexSchemaVersion ) / "exact";
}

bool CleanupTraceSessionCpuZoneTemporaryFiles( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, std::string& error )
{
    error.clear();
    const auto root = TraceSessionCpuZoneIndexRoot( sessionRoot, manifest );
    for( const auto* name : { "zones.work", "extras.work", "children.work", "children-sorted.work" } )
    {
        const auto path = root / name;
        std::error_code ec;
        const auto exists = std::filesystem::exists( path, ec );
        if( ec ) { error = "session_cpu_zone_work_cleanup_scan_failed:" + ec.message(); return false; }
        if( !exists ) continue;
        if( !std::filesystem::remove( path, ec ) || ec )
        { error = "session_cpu_zone_work_cleanup_failed:" + ( ec ? ec.message() : path.string() ); return false; }
    }
    if( !CleanupTraceSessionExternalSortFiles( root, "cpu-child-run-", error ) ) return false;
    return true;
}

bool BuildTraceSessionCpuZoneDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionCpuZoneStats& stats, std::string& error )
{
    error.clear(); stats = {};
    BuildState state;
    if( !LoadTraceSessionTimeTransform( sessionRoot, manifest, state.transform, error ) ) return false;
    const auto root = TraceSessionCpuZoneIndexRoot( sessionRoot, manifest );
    if( !state.writer.Open( root, manifest, error ) ) return false;
    if( !VisitTraceSessionCanonicalOrdered( sessionRoot, manifest, VisitCpuZoneRecord, &state, error ) ) return false;
    if( state.pendingCallstack != 0 || state.serialNextCallstack != 0 || state.pendingDynamicSource )
    { error = "session_cpu_zone_pending_protocol_state"; return false; }
    for( auto& [thread, stack] : state.open )
    {
        while( !stack.empty() )
        {
            auto zone = std::move( stack.back() ); stack.pop_back();
            if( !WriteClosedZone( state, std::move( zone ), 0, false, true, error ) ) return false;
        }
    }
    for( auto& source : state.sources )
    {
        if( source.dynamic ) continue;
        if( const auto found = state.strings.find( source.namePointer ); found != state.strings.end() ) source.name = found->second;
        if( const auto found = state.strings.find( source.functionPointer ); found != state.strings.end() ) source.function = found->second;
        if( const auto found = state.strings.find( source.filePointer ); found != state.strings.end() ) source.file = found->second;
    }
    CpuZoneManifest cpuManifest;
    if( !state.writer.Finalize( state.sources, state.callsiteRecords,
        state.zoneCount, state.completeZones, cpuManifest, error ) ) return false;
    cpuManifest.stats.beginEvents = state.beginEvents;
    cpuManifest.stats.endEvents = state.endEvents;
    cpuManifest.stats.invalidTimingZones = state.invalidTimingZones;
    if( !SaveCpuZoneManifest( root, cpuManifest, error ) ) return false;
    stats = cpuManifest.stats;
    return true;
}

bool AuditTraceSessionCpuZoneDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& session, TraceSessionCpuZoneStats& stats, std::string& error )
{
    error.clear(); stats = {};
    const auto root = TraceSessionCpuZoneIndexRoot( sessionRoot, session );
    CpuZoneManifest manifest;
    if( !LoadCpuZoneManifest( root, manifest, error ) ) return false;
    if( manifest.sourceSha256 != session.source.sha256 || manifest.sourceSize != session.source.fileSize ||
        manifest.generation != session.generation )
    { error = "session_cpu_zone_identity_mismatch"; return false; }
    const auto path = root / CpuZoneFileName;
    std::error_code ec;
    if( std::filesystem::file_size( path, ec ) != manifest.fileBytes || ec )
    { error = "session_cpu_zone_file_size_mismatch"; return false; }
    if( Sha256File( path ) != manifest.fileSha256 )
    { error = "session_cpu_zone_file_sha256_mismatch"; return false; }
    stats = manifest.stats;
    return true;
}

std::shared_ptr<TraceSessionCpuZoneReader> TraceSessionCpuZoneReader::Open(
    const std::filesystem::path& sessionRoot, const TraceSessionManifest& session,
    std::string& error )
{
    error.clear();
    const auto root = TraceSessionCpuZoneIndexRoot( sessionRoot, session );
    CpuZoneManifest manifest;
    if( !LoadCpuZoneManifest( root, manifest, error ) ) return {};
    if( manifest.sourceSha256 != session.source.sha256 || manifest.sourceSize != session.source.fileSize ||
        manifest.generation != session.generation )
    { error = "session_cpu_zone_identity_mismatch"; return {}; }
    const auto path = root / CpuZoneFileName;
    std::error_code ec;
    if( std::filesystem::file_size( path, ec ) != manifest.fileBytes || ec )
    { error = "session_cpu_zone_file_size_mismatch"; return {}; }
    std::ifstream in( path, std::ios::binary );
    CpuZoneFileHeader header;
    if( !in.read( reinterpret_cast<char*>( &header ), sizeof( header ) ) ||
        header.magic != CpuZoneFileMagic || header.schema != TraceSessionCpuZoneIndexSchemaVersion ||
        header.endian != 0x01020304 || header.sourceSize != session.source.fileSize ||
        header.zoneCount != manifest.stats.zones || header.completeZoneCount != manifest.stats.completeZones ||
        header.sourceCount != manifest.stats.sourceLocations ||
        header.zoneBlockCount != manifest.stats.zoneBlocks ||
        header.childLinkCount != manifest.stats.childLinks ||
        header.generationBytes != session.generation.size() )
    { error = "session_cpu_zone_file_header_invalid"; return {}; }
    std::string sha( 64, '\0' ), generation( header.generationBytes, '\0' );
    if( !in.read( sha.data(), std::streamsize( sha.size() ) ) ||
        !in.read( generation.data(), std::streamsize( generation.size() ) ) ||
        sha != session.source.sha256 || generation != session.generation )
    { error = "session_cpu_zone_file_identity_invalid"; return {}; }
    const auto expectedZonesOffset = sizeof( header ) + sha.size() + generation.size();
    const auto expectedBlocksOffset = expectedZonesOffset + header.zoneCount * sizeof( StoredZone );
    const auto expectedExtrasOffset = expectedBlocksOffset + header.zoneBlockCount * sizeof( StoredZoneBlock );
    if( header.zonesOffset != expectedZonesOffset || header.zoneBlocksOffset != expectedBlocksOffset ||
        header.extrasOffset != expectedExtrasOffset || header.sourcesOffset < header.extrasOffset ||
        header.callsitesOffset < header.sourcesOffset ||
        header.childLinksOffset != header.callsitesOffset +
            header.callsiteCount * sizeof( StoredCallsite ) ||
        header.childLinksOffset + header.childLinkCount * sizeof( TraceSessionUInt64Pair ) != manifest.fileBytes )
    { error = "session_cpu_zone_file_layout_invalid"; return {}; }
    auto impl = std::make_shared<Impl>();
    impl->path = path; impl->fingerprint = session.source.sha256;
    impl->zonesOffset = header.zonesOffset; impl->extrasOffset = header.extrasOffset;
    impl->zoneCount = header.zoneCount;
    impl->childLinksOffset = header.childLinksOffset;
    impl->childLinkCount = header.childLinkCount;
    impl->zoneBlocks.resize( size_t( header.zoneBlockCount ) );
    in.seekg( std::streamoff( header.zoneBlocksOffset ) );
    if( !impl->zoneBlocks.empty() && !in.read( reinterpret_cast<char*>( impl->zoneBlocks.data() ),
        std::streamsize( impl->zoneBlocks.size() * sizeof( StoredZoneBlock ) ) ) )
    { error = "session_cpu_zone_block_records_truncated"; return {}; }
    uint64_t expectedFirstZone = 0;
    for( const auto& block : impl->zoneBlocks )
    {
        if( block.firstZone != expectedFirstZone || block.zoneCount == 0 || block.zoneCount > 4096 ||
            block.firstZone + block.zoneCount > header.zoneCount || block.minStartNs > block.maxEndNs )
        { error = "session_cpu_zone_block_record_invalid"; return {}; }
        expectedFirstZone += block.zoneCount;
    }
    if( expectedFirstZone != header.zoneCount )
    { error = "session_cpu_zone_block_coverage_invalid"; return {}; }
    in.seekg( std::streamoff( header.sourcesOffset ) );
    for( uint64_t i = 0; i < header.sourceCount; ++i )
    {
        StoredSource stored;
        if( !in.read( reinterpret_cast<char*>( &stored ), sizeof( stored ) ) ||
            stored.nameBytes > 16 * 1024 * 1024 || stored.functionBytes > 16 * 1024 * 1024 || stored.fileBytes > 16 * 1024 * 1024 )
        { error = "session_cpu_zone_source_record_invalid"; return {}; }
        SourceState source;
        source.nativeId = stored.nativeId; source.line = stored.line; source.color = stored.color;
        source.dynamic = ( stored.flags & 1 ) != 0; source.definitionReceived = ( stored.flags & 2 ) != 0;
        source.name.resize( stored.nameBytes ); source.function.resize( stored.functionBytes ); source.file.resize( stored.fileBytes );
        if( ( !source.name.empty() && !in.read( source.name.data(), std::streamsize( source.name.size() ) ) ) ||
            ( !source.function.empty() && !in.read( source.function.data(), std::streamsize( source.function.size() ) ) ) ||
            ( !source.file.empty() && !in.read( source.file.data(), std::streamsize( source.file.size() ) ) ) )
        { error = "session_cpu_zone_source_record_truncated"; return {}; }
        if( !impl->sources.emplace( source.nativeId, std::move( source ) ).second )
        { error = "session_cpu_zone_source_id_duplicate"; return {}; }
    }
    if( uint64_t( in.tellg() ) != header.callsitesOffset )
    { error = "session_cpu_zone_callsite_offset_invalid"; return {}; }
    impl->callsites.resize( size_t( header.callsiteCount ) );
    if( !impl->callsites.empty() && !in.read( reinterpret_cast<char*>( impl->callsites.data() ),
        std::streamsize( impl->callsites.size() * sizeof( StoredCallsite ) ) ) )
    { error = "session_cpu_zone_callsite_record_truncated"; return {}; }
    if( uint64_t( in.tellg() ) != header.childLinksOffset )
    { error = "session_cpu_zone_child_link_offset_invalid"; return {}; }
    auto reader = std::shared_ptr<TraceSessionCpuZoneReader>( new TraceSessionCpuZoneReader( std::move( impl ) ) );
    reader->m_stats = manifest.stats;
    return reader;
}

std::vector<SourceLocationDto> TraceSessionCpuZoneReader::Sources() const
{
    std::vector<SourceLocationDto> result;
    result.reserve( m_impl->sources.size() );
    for( const auto& [id, source] : m_impl->sources )
    {
        result.push_back( { MakeRef( m_impl->fingerprint, "source", uint16_t( id ) ),
            source.name, source.function, source.file, source.line, source.color,
            id, source.dynamic } );
    }
    std::sort( result.begin(), result.end(), []( const auto& left, const auto& right ) {
        return left.nativeId < right.nativeId;
    } );
    return result;
}

std::vector<CallsiteDto> TraceSessionCpuZoneReader::Callsites() const
{
    std::vector<CallsiteDto> result;
    result.reserve( m_impl->callsites.size() );
    for( const auto& stored : m_impl->callsites )
    {
        CallsiteDto dto;
        dto.ref = MakeRef( m_impl->fingerprint, "callsite", stored.callsiteId );
        dto.callsiteId = stored.callsiteId;
        dto.threadRef = MakeRef( m_impl->fingerprint, "thread", stored.thread );
        dto.sourceLocationRef = MakeRef( m_impl->fingerprint, "source", uint16_t( stored.sourceNativeId ) );
        dto.callstack = stored.callstack;
        if( stored.callstack != 0 ) dto.stackRef = MakeRef( m_impl->fingerprint, "callstack", stored.callstack );
        dto.domain = stored.domain;
        dto.provenance = ProvenanceName( stored.provenance );
        dto.flags = stored.flags;
        if( const auto* reason = UnavailableReasonName( stored.unavailableReason ); *reason != '\0' )
            dto.unavailableReason = reason;
        result.emplace_back( std::move( dto ) );
    }
    return result;
}

std::optional<CpuZoneDto> TraceSessionCpuZoneReader::Get( uint64_t id ) const
{
    StoredZone zone;
    if( !m_impl->ReadZone( id, zone ) ) return std::nullopt;
    return m_impl->ToDto( zone );
}

std::vector<CpuZoneDto> TraceSessionCpuZoneReader::Scan( const ScanRange& range ) const
{
    return ScanImpl( range, std::nullopt );
}

std::vector<CpuZoneDto> TraceSessionCpuZoneReader::ScanThread(
    std::string_view threadRef, const ScanRange& range ) const
{
    const auto thread = ParseThreadRef( m_impl->fingerprint, threadRef );
    return thread ? ScanImpl( range, thread ) : std::vector<CpuZoneDto> {};
}

std::vector<CpuZoneDto> TraceSessionCpuZoneReader::ScanById(
    size_t offset, size_t limit ) const
{
    std::vector<CpuZoneDto> result;
    const auto begin = std::min<uint64_t>( offset, m_impl->zoneCount );
    const auto count = std::min<uint64_t>( limit, m_impl->zoneCount - begin );
    if( count == 0 ) return result;
    std::ifstream in( m_impl->path, std::ios::binary );
    if( !in ) throw std::runtime_error( "Session CPU Zone pages are unavailable" );
    in.seekg( std::streamoff( m_impl->zonesOffset + begin * sizeof( StoredZone ) ) );
    result.reserve( size_t( count ) );
    for( uint64_t index = 0; index < count; ++index )
    {
        StoredZone zone;
        if( !in.read( reinterpret_cast<char*>( &zone ), sizeof( zone ) ) ||
            zone.id != begin + index )
            throw std::runtime_error( "Session CPU Zone page is truncated" );
        result.emplace_back( m_impl->ToDto( zone ) );
    }
    return result;
}

std::vector<CpuZoneDto> TraceSessionCpuZoneReader::ScanImpl(
    const ScanRange& range, std::optional<uint64_t> thread ) const
{
    std::vector<CpuZoneDto> result;
    if( range.limit == 0 ) return result;
    size_t skipped = 0;
    std::ifstream in( m_impl->path, std::ios::binary );
    if( !in ) return result;
    for( const auto& block : m_impl->zoneBlocks )
    {
        if( block.maxEndNs < range.startNs || block.minStartNs > range.endNs ||
            ( thread && !MayContainThread( block, *thread ) ) ) continue;
        in.clear();
        in.seekg( std::streamoff( m_impl->zonesOffset + block.firstZone * sizeof( StoredZone ) ) );
        for( uint32_t index = 0; index < block.zoneCount; ++index )
        {
            StoredZone zone;
            const auto id = block.firstZone + index;
            if( !in.read( reinterpret_cast<char*>( &zone ), sizeof( zone ) ) || zone.id != id ) return result;
            if( thread && zone.thread != *thread ) continue;
            const auto end = ( zone.flags & ZoneComplete ) != 0 ? zone.endNs : zone.startNs;
            const auto lower = std::min( zone.startNs, end );
            const auto upper = std::max( zone.startNs, end );
            if( upper < range.startNs || lower > range.endNs ) continue;
            if( skipped++ < range.offset ) continue;
            result.emplace_back( m_impl->ToDto( zone ) );
            if( result.size() >= range.limit ) return result;
        }
    }
    return result;
}

std::vector<CpuZoneDto> TraceSessionCpuZoneReader::Children(
    uint64_t id, size_t offset, size_t limit ) const
{
    std::vector<CpuZoneDto> result;
    if( id >= m_impl->zoneCount || limit == 0 ) return result;
    std::ifstream in( m_impl->path, std::ios::binary );
    if( !in ) return result;
    const auto lowerBound = [&]( uint64_t key, bool upper )
    {
        uint64_t first = 0, last = m_impl->childLinkCount;
        while( first < last )
        {
            const auto middle = first + ( last - first ) / 2;
            TraceSessionUInt64Pair pair;
            in.clear();
            in.seekg( std::streamoff( m_impl->childLinksOffset +
                middle * sizeof( TraceSessionUInt64Pair ) ) );
            if( !in.read( reinterpret_cast<char*>( &pair ), sizeof( pair ) ) ) return m_impl->childLinkCount;
            if( pair.key < key || ( upper && pair.key == key ) ) first = middle + 1;
            else last = middle;
        }
        return first;
    };
    const auto begin = lowerBound( id, false );
    const auto end = lowerBound( id, true );
    if( begin >= end || offset >= end - begin ) return result;
    const auto count = std::min<uint64_t>( limit, end - begin - offset );
    std::vector<uint64_t> children;
    children.reserve( size_t( count ) );
    in.clear();
    in.seekg( std::streamoff( m_impl->childLinksOffset +
        ( begin + offset ) * sizeof( TraceSessionUInt64Pair ) ) );
    for( uint64_t index = 0; index < count; ++index )
    {
        TraceSessionUInt64Pair pair;
        if( !in.read( reinterpret_cast<char*>( &pair ), sizeof( pair ) ) || pair.key != id ) return {};
        children.emplace_back( pair.value );
    }
    for( const auto child : children )
    {
        StoredZone zone;
        if( !m_impl->ReadZone( child, zone ) || zone.parent != id ) return {};
        result.emplace_back( m_impl->ToDto( zone ) );
    }
    return result;
}

}

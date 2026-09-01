#include "TracyTraceSessionMemory.hpp"

#include "TracyHash.hpp"
#include "TracyMemoryAnalysis.hpp"
#include "TracyTraceSessionCanonical.hpp"
#include "TracyTraceSessionGpuCanonical.hpp"
#include "TracyProtocol.hpp"
#include "TracyQueue.hpp"

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

constexpr uint64_t MemoryFileMagic = 0x314d454d534e4aull;     // JNSMEM1
constexpr uint64_t MemoryManifestMagic = 0x31464d4d534e4aull; // JNSMMF1
constexpr const char* MemoryFileName = "memory.bin";

#pragma pack( push, 1 )
struct MemoryFileHeader
{
    uint64_t magic = MemoryFileMagic;
    uint32_t schema = TraceSessionMemoryIndexSchemaVersion;
    uint32_t endian = 0x01020304;
    uint64_t sourceSize = 0;
    uint64_t poolCount = 0;
    uint64_t eventCount = 0;
    uint64_t eventBlockCount = 0;
    uint64_t activeCount = 0;
    uint64_t poolTableOffset = 0;
    uint32_t generationBytes = 0;
    uint32_t reserved = 0;
};

struct StoredPool
{
    uint64_t nativeNameId = 0;
    uint64_t eventCount = 0;
    uint64_t activeCount = 0;
    uint64_t activeBytes = 0;
    uint64_t freeCount = 0;
    uint64_t low = 0;
    uint64_t high = 0;
    uint64_t nameOffset = 0;
    uint64_t eventsOffset = 0;
    uint64_t blocksOffset = 0;
    uint64_t blockCount = 0;
    uint32_t nameBytes = 0;
    uint32_t flags = 0;
};

struct StoredMemoryEvent
{
    uint64_t address = 0;
    uint64_t size = 0;
    int64_t allocationNs = 0;
    int64_t freeNs = -1;
    uint64_t allocationThread = 0;
    uint64_t freeThread = 0;
    uint32_t allocationCallstack = 0;
    uint32_t freeCallstack = 0;
    uint32_t allocationCallsiteId = 0;
    uint32_t reserved = 0;
};

struct StoredMemoryEventBlock
{
    uint64_t firstEvent = 0;
    uint32_t eventCount = 0;
    uint32_t reserved = 0;
    int64_t minAllocationNs = 0;
    int64_t maxFreeNs = 0;
};
#pragma pack( pop )

struct MemoryManifest
{
    std::string sourceSha256;
    uint64_t sourceSize = 0;
    std::string generation;
    uint64_t fileBytes = 0;
    std::string fileSha256;
    TraceSessionMemoryStats stats;
};

bool AtomicReplace( const std::filesystem::path& source,
    const std::filesystem::path& target, std::string& error )
{
#ifdef _WIN32
    if( MoveFileExW( source.c_str(), target.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) ) return true;
    error = "session_memory_atomic_replace_failed:" + std::to_string( GetLastError() );
    return false;
#else
    std::error_code ec;
    std::filesystem::rename( source, target, ec );
    if( !ec ) return true;
    error = "session_memory_atomic_replace_failed:" + ec.message();
    return false;
#endif
}

bool CopyFileBytes( const std::filesystem::path& source, std::ofstream& out,
    std::string& error )
{
    std::ifstream in( source, std::ios::binary );
    if( !in ) { error = "session_memory_work_read_failed"; return false; }
    std::vector<char> buffer( 1024 * 1024 );
    while( in )
    {
        in.read( buffer.data(), std::streamsize( buffer.size() ) );
        const auto count = in.gcount();
        if( count > 0 ) out.write( buffer.data(), count );
    }
    if( !in.eof() || !out ) { error = "session_memory_work_copy_failed"; return false; }
    return true;
}

bool DecodeItem( const TraceSessionCanonicalRecord& record, QueueItem& item,
    std::string& error )
{
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ||
        record.type >= uint8_t( QueueType::NUM_TYPES ) ||
        record.payload.size() < QueueDataSize[record.type] )
    { error = "session_memory_protocol_record_invalid"; return false; }
    item = {};
    std::memcpy( &item, record.payload.data(),
        std::min<size_t>( record.payload.size(), sizeof( item ) ) );
    if( item.hdr.idx != record.type )
    { error = "session_memory_protocol_type_mismatch"; return false; }
    return true;
}

bool GetShortPayload( const TraceSessionCanonicalRecord& record,
    const uint8_t*& data, size_t& size, std::string& error )
{
    const auto fixed = size_t( QueueDataSize[record.type] );
    if( record.payload.size() < fixed + sizeof( uint16_t ) )
    { error = "session_memory_payload_truncated"; return false; }
    uint16_t bytes = 0;
    std::memcpy( &bytes, record.payload.data() + fixed, sizeof( bytes ) );
    if( bytes != record.variablePayloadBytes ||
        record.payload.size() != fixed + sizeof( bytes ) + bytes )
    { error = "session_memory_payload_mismatch"; return false; }
    data = record.payload.data() + fixed + sizeof( bytes );
    size = bytes;
    return true;
}

uint64_t DecodeSize( const char size[6] )
{
    uint32_t low = 0; uint16_t high = 0;
    std::memcpy( &low, size, 4 );
    std::memcpy( &high, size + 4, 2 );
    return low | ( uint64_t( high ) << 32 );
}

std::string MakeRef( const std::string& fingerprint, const char* kind, uint64_t id )
{
    std::ostringstream out;
    out << "tracy:v1:" << fingerprint.substr( 0, 16 ) << ':' << kind << ':' << std::hex << id;
    return out.str();
}

struct ActiveAllocation
{
    uint64_t index = 0;
    uint64_t size = 0;
};

class PoolWork
{
public:
    bool Open( const std::filesystem::path& root, uint64_t nativeNameId,
        std::string& error )
    {
        m_nativeNameId = nativeNameId;
        std::ostringstream name;
        name << "pool-" << std::hex << std::setw( 16 ) << std::setfill( '0' ) << nativeNameId << ".work";
        m_path = root / name.str();
        m_file.open( m_path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc );
        if( !m_file ) { error = "session_memory_pool_work_open_failed"; return false; }
        return true;
    }

    bool Allocate( uint64_t address, uint64_t size, int64_t timeNs, uint64_t thread,
        uint32_t callstack, uint32_t callsiteId, std::string& error )
    {
        if( m_active.contains( address ) )
        { error = "session_memory_double_allocation"; return false; }
        StoredMemoryEvent event;
        event.address = address; event.size = size; event.allocationNs = timeNs;
        event.allocationThread = thread; event.allocationCallstack = callstack;
        event.allocationCallsiteId = callsiteId;
        m_file.clear();
        m_file.seekp( std::streamoff( m_eventCount * sizeof( StoredMemoryEvent ) ) );
        m_file.write( reinterpret_cast<const char*>( &event ), sizeof( event ) );
        if( !m_file ) { error = "session_memory_allocation_write_failed"; return false; }
        m_active.emplace( address, ActiveAllocation { m_eventCount, size } );
        ++m_eventCount; ++m_activeCount; m_activeBytes += size;
        m_low = std::min( m_low, address );
        m_high = std::max( m_high, address + size );
        return true;
    }

    bool Free( uint64_t address, int64_t timeNs, uint64_t thread,
        uint32_t callstack, bool& found, std::string& error )
    {
        const auto active = m_active.find( address );
        if( active == m_active.end() ) { found = false; return true; }
        found = true;
        if( !PatchFree( active->second.index, timeNs, thread, callstack, error ) ) return false;
        --m_activeCount; m_activeBytes -= active->second.size; ++m_freeCount;
        m_active.erase( active );
        return true;
    }

    bool Discard( int64_t timeNs, uint64_t thread, uint32_t callstack,
        std::string& error )
    {
        for( const auto& [address, active] : m_active )
        {
            (void)address;
            if( !PatchFree( active.index, timeNs, thread, callstack, error ) ) return false;
            ++m_freeCount;
        }
        m_active.clear(); m_activeCount = 0; m_activeBytes = 0;
        return true;
    }

    bool Close( std::string& error )
    {
        m_file.flush();
        if( !m_file ) { error = "session_memory_pool_work_flush_failed"; return false; }
        m_file.close();
        return true;
    }

    uint64_t NativeNameId() const { return m_nativeNameId; }
    bool IsActive( uint64_t address ) const { return m_active.contains( address ); }
    uint64_t EventCount() const { return m_eventCount; }
    uint64_t ActiveCount() const { return m_activeCount; }
    uint64_t ActiveBytes() const { return m_activeBytes; }
    uint64_t FreeCount() const { return m_freeCount; }
    uint64_t Low() const { return m_eventCount == 0 ? 0 : m_low; }
    uint64_t High() const { return m_high; }
    const std::filesystem::path& Path() const { return m_path; }

private:
    bool PatchFree( uint64_t index, int64_t timeNs, uint64_t thread,
        uint32_t callstack, std::string& error )
    {
        StoredMemoryEvent event;
        const auto offset = std::streamoff( index * sizeof( StoredMemoryEvent ) );
        m_file.clear(); m_file.seekg( offset );
        if( !m_file.read( reinterpret_cast<char*>( &event ), sizeof( event ) ) || event.freeNs >= 0 )
        { error = "session_memory_free_patch_read_failed"; return false; }
        event.freeNs = timeNs; event.freeThread = thread; event.freeCallstack = callstack;
        m_file.clear(); m_file.seekp( offset );
        m_file.write( reinterpret_cast<const char*>( &event ), sizeof( event ) );
        if( !m_file ) { error = "session_memory_free_patch_write_failed"; return false; }
        return true;
    }

    uint64_t m_nativeNameId = 0;
    std::filesystem::path m_path;
    std::fstream m_file;
    std::unordered_map<uint64_t, ActiveAllocation> m_active;
    uint64_t m_eventCount = 0;
    uint64_t m_activeCount = 0;
    uint64_t m_activeBytes = 0;
    uint64_t m_freeCount = 0;
    uint64_t m_low = std::numeric_limits<uint64_t>::max();
    uint64_t m_high = 0;
};

struct CallsiteState
{
    uint32_t callstack = 0;
};

struct BuildState
{
    TraceSessionTimeTransform transform;
    std::filesystem::path root;
    std::unordered_map<uint64_t, std::string> strings;
    std::unordered_map<std::string, uint32_t> callstackIds;
    std::unordered_map<uint32_t, CallsiteState> callsites;
    std::map<uint64_t, std::unique_ptr<PoolWork>> pools;
    uint64_t pendingPoolName = 0;
    bool poolNamePending = false;
    uint32_t pendingCallstack = 0;
    uint32_t serialNextCallstack = 0;
    int64_t serialTime = 0;
    TraceSessionMemoryStats stats;
};

PoolWork* EnsurePool( BuildState& state, uint64_t nativeNameId, std::string& error )
{
    const auto found = state.pools.find( nativeNameId );
    if( found != state.pools.end() ) return found->second.get();
    auto pool = std::make_unique<PoolWork>();
    if( !pool->Open( state.root, nativeNameId, error ) ) return nullptr;
    return state.pools.emplace( nativeNameId, std::move( pool ) ).first->second.get();
}

uint32_t InternCallstack( BuildState& state, const uint8_t* data,
    size_t size, std::string& error )
{
    if( size % sizeof( uint64_t ) != 0 )
    { error = "session_memory_callstack_payload_invalid"; return 0; }
    std::string key( reinterpret_cast<const char*>( data ), size );
    const auto found = state.callstackIds.find( key );
    if( found != state.callstackIds.end() ) return found->second;
    if( state.callstackIds.size() >= std::numeric_limits<uint32_t>::max() - 1 )
    { error = "session_memory_callstack_id_overflow"; return 0; }
    const auto id = uint32_t( state.callstackIds.size() + 1 );
    state.callstackIds.emplace( std::move( key ), id );
    return id;
}

int64_t AdvanceSerial( BuildState& state, int64_t delta )
{
    state.serialTime += delta;
    return state.transform.ToNanoseconds( state.serialTime );
}

bool ConsumeSerialCallstack( BuildState& state, uint32_t& callstack,
    std::string& error )
{
    if( state.serialNextCallstack == 0 )
    { error = "session_memory_serial_callstack_missing"; return false; }
    callstack = state.serialNextCallstack;
    state.serialNextCallstack = 0;
    return true;
}

bool ConsumeNamedPool( BuildState& state, uint64_t& pool, std::string& error )
{
    if( !state.poolNamePending )
    { error = "session_memory_named_pool_missing"; return false; }
    pool = state.pendingPoolName;
    state.poolNamePending = false;
    state.pendingPoolName = 0;
    return true;
}

bool VisitMemoryRecord( const TraceSessionCanonicalRecord& record,
    void* userData, std::string& error )
{
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ) return true;
    auto& state = *static_cast<BuildState*>( userData );
    QueueItem item {};
    if( !DecodeItem( record, item, error ) ) return false;
    const auto type = QueueType( record.type );
    switch( type )
    {
    case QueueType::StringData:
    {
        const uint8_t* data = nullptr; size_t size = 0;
        if( !GetShortPayload( record, data, size, error ) ) return false;
        const std::string value( reinterpret_cast<const char*>( data ), size );
        const auto [found, inserted] = state.strings.emplace( item.stringTransfer.ptr, value );
        if( !inserted && found->second != value )
        { error = "session_memory_string_conflict"; return false; }
        break;
    }
    case QueueType::CallstackPayload:
    {
        const uint8_t* data = nullptr; size_t size = 0;
        if( !GetShortPayload( record, data, size, error ) || state.pendingCallstack != 0 )
        { if( error.empty() ) error = "session_memory_callstack_sequence_invalid"; return false; }
        state.pendingCallstack = InternCallstack( state, data, size, error );
        if( state.pendingCallstack == 0 ) return false;
        break;
    }
    case QueueType::CallstackSampleDictionary:
    {
        const uint8_t* data = nullptr; size_t size = 0;
        if( !GetShortPayload( record, data, size, error ) ||
            InternCallstack( state, data, size, error ) == 0 ) return false;
        break;
    }
    case QueueType::CallstackSerial:
        if( state.pendingCallstack == 0 || state.serialNextCallstack != 0 )
        { error = "session_memory_callstack_serial_sequence_invalid"; return false; }
        state.serialNextCallstack = state.pendingCallstack; state.pendingCallstack = 0;
        break;
    case QueueType::Callstack:
    case QueueType::CallstackAlloc:
    case QueueType::CallstackSample:
    case QueueType::CallstackSampleContextSwitch:
        if( state.pendingCallstack == 0 )
        { error = "session_memory_callstack_consumer_missing"; return false; }
        state.pendingCallstack = 0;
        break;
    case QueueType::MemNamePayload:
        if( state.poolNamePending )
        { error = "session_memory_name_payload_sequence_invalid"; return false; }
        state.pendingPoolName = item.memName.name;
        state.poolNamePending = true;
        break;
    case QueueType::JnCallsiteDefinition:
    {
        CallsiteState callsite;
        if( item.jnCallsiteDefinition.flags & uint8_t( JnCallsiteFlags::HasCallstack ) )
        {
            if( !ConsumeSerialCallstack( state, callsite.callstack, error ) ) return false;
        }
        else if( state.serialNextCallstack != 0 )
        { error = "session_memory_callsite_unexpected_callstack"; return false; }
        if( !state.callsites.emplace( item.jnCallsiteDefinition.callsiteId, callsite ).second )
        { error = "session_memory_callsite_duplicate"; return false; }
        break;
    }
    case QueueType::MemAlloc:
    case QueueType::MemAllocNamed:
    case QueueType::MemAllocCallstack:
    case QueueType::MemAllocCallstackNamed:
    case QueueType::JnMemAllocCallsiteNamed:
    {
        const bool named = type == QueueType::MemAllocNamed ||
            type == QueueType::MemAllocCallstackNamed || type == QueueType::JnMemAllocCallsiteNamed;
        uint64_t poolId = 0;
        if( named && !ConsumeNamedPool( state, poolId, error ) ) return false;
        if( !named && state.poolNamePending )
        { error = "session_memory_unexpected_name_payload"; return false; }
        auto* pool = EnsurePool( state, poolId, error );
        if( !pool ) return false;
        uint32_t callstack = 0, callsite = 0;
        if( type == QueueType::MemAllocCallstack || type == QueueType::MemAllocCallstackNamed )
        {
            if( !ConsumeSerialCallstack( state, callstack, error ) ) return false;
        }
        else if( type == QueueType::JnMemAllocCallsiteNamed )
        {
            callsite = item.jnMemAllocCallsite.callsiteId;
            const auto found = state.callsites.find( callsite );
            if( found != state.callsites.end() ) callstack = found->second.callstack;
        }
        const auto& alloc = type == QueueType::JnMemAllocCallsiteNamed ?
            static_cast<const QueueMemAlloc&>( item.jnMemAllocCallsite ) : item.memAlloc;
        // Tracy rejects a double allocation before consuming the serial clock.
        if( pool->IsActive( alloc.ptr ) )
        { error = "session_memory_double_allocation"; return false; }
        const auto size = DecodeSize( alloc.size );
        const auto timeNs = AdvanceSerial( state, alloc.time );
        if( !pool->Allocate( alloc.ptr, size, timeNs, alloc.thread,
            callstack, callsite, error ) ) return false;
        ++state.stats.allocationEvents;
        break;
    }
    case QueueType::MemFree:
    case QueueType::MemFreeNamed:
    case QueueType::MemFreeCallstack:
    case QueueType::MemFreeCallstackNamed:
    {
        const bool named = type == QueueType::MemFreeNamed || type == QueueType::MemFreeCallstackNamed;
        uint64_t poolId = 0;
        if( named && !ConsumeNamedPool( state, poolId, error ) ) return false;
        if( !named && state.poolNamePending )
        { error = "session_memory_unexpected_name_payload"; return false; }
        auto* pool = EnsurePool( state, poolId, error );
        if( !pool ) return false;
        uint32_t callstack = 0;
        if( type == QueueType::MemFreeCallstack || type == QueueType::MemFreeCallstackNamed )
            if( !ConsumeSerialCallstack( state, callstack, error ) ) return false;
        const auto timeNs = AdvanceSerial( state, item.memFree.time );
        bool found = false;
        if( !pool->Free( item.memFree.ptr, timeNs, item.memFree.thread,
            callstack, found, error ) ) return false;
        if( !found && item.memFree.ptr != 0 ) ++state.stats.unknownFrees;
        ++state.stats.freeEvents;
        break;
    }
    case QueueType::MemDiscard:
    case QueueType::MemDiscardCallstack:
    {
        uint32_t callstack = 0;
        if( type == QueueType::MemDiscardCallstack &&
            !ConsumeSerialCallstack( state, callstack, error ) ) return false;
        const auto found = state.pools.find( item.memDiscard.name );
        if( found != state.pools.end() )
        {
            const auto timeNs = AdvanceSerial( state, item.memDiscard.time );
            if( !found->second->Discard( timeNs, item.memDiscard.thread, callstack, error ) ) return false;
        }
        ++state.stats.discardEvents;
        break;
    }
    case QueueType::LockWait:
    case QueueType::LockObtain:
    case QueueType::LockSharedWait:
    case QueueType::LockSharedObtain:
        (void)AdvanceSerial( state, item.lockWait.time );
        break;
    case QueueType::LockRelease:
        (void)AdvanceSerial( state, item.lockRelease.time );
        break;
    case QueueType::LockSharedRelease:
        (void)AdvanceSerial( state, item.lockReleaseShared.time );
        break;
    case QueueType::GpuZoneBeginSerial:
        (void)AdvanceSerial( state, item.gpuZoneBegin.cpuTime );
        break;
    case QueueType::GpuZoneBeginCallstackSerial:
        (void)AdvanceSerial( state, item.gpuZoneBegin.cpuTime );
        { uint32_t ignored = 0; if( !ConsumeSerialCallstack( state, ignored, error ) ) return false; }
        break;
    case QueueType::GpuZoneBeginAllocSrcLocSerial:
        (void)AdvanceSerial( state, item.gpuZoneBeginLean.cpuTime );
        break;
    case QueueType::GpuZoneBeginAllocSrcLocCallstackSerial:
        (void)AdvanceSerial( state, item.gpuZoneBeginLean.cpuTime );
        { uint32_t ignored = 0; if( !ConsumeSerialCallstack( state, ignored, error ) ) return false; }
        break;
    case QueueType::GpuZoneEndSerial:
        (void)AdvanceSerial( state, item.gpuZoneEnd.cpuTime );
        break;
    case QueueType::JnJobStage:
        if( JnJobStage( item.jnJobStage.stage ) == JnJobStage::ScheduleCallstack ||
            JnJobStage( item.jnJobStage.stage ) == JnJobStage::WaitCallstack )
        { uint32_t ignored = 0; if( !ConsumeSerialCallstack( state, ignored, error ) ) return false; }
        break;
    case QueueType::JnIoStage:
        if( JnIoStage( item.jnIoStage.stage ) == JnIoStage::RequestCallstack )
        { uint32_t ignored = 0; if( !ConsumeSerialCallstack( state, ignored, error ) ) return false; }
        break;
    default: break;
    }
    return true;
}

std::string ResolvePoolName( const BuildState& state, uint64_t id )
{
    if( id == 0 ) return "Default allocator";
    const auto found = state.strings.find( id );
    if( found != state.strings.end() ) return found->second;
    std::ostringstream out; out << "Unavailable memory pool 0x" << std::hex << id;
    return out.str();
}

bool SaveMemoryManifest( const std::filesystem::path& root,
    const MemoryManifest& value, std::string& error )
{
    const auto temporary = root / "manifest.tmp";
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_memory_manifest_open_failed"; return false; }
    out << "magic " << MemoryManifestMagic << '\n';
    out << "schema " << TraceSessionMemoryIndexSchemaVersion << '\n';
    out << "source_sha256 " << std::quoted( value.sourceSha256 ) << '\n';
    out << "source_size " << value.sourceSize << '\n';
    out << "generation " << std::quoted( value.generation ) << '\n';
    out << "file_bytes " << value.fileBytes << '\n';
    out << "file_sha256 " << std::quoted( value.fileSha256 ) << '\n';
    out << "pools " << value.stats.pools << '\n';
    out << "events " << value.stats.events << '\n';
    out << "event_blocks " << value.stats.eventBlocks << '\n';
    out << "active_events " << value.stats.activeEvents << '\n';
    out << "allocation_events " << value.stats.allocationEvents << '\n';
    out << "free_events " << value.stats.freeEvents << '\n';
    out << "discard_events " << value.stats.discardEvents << '\n';
    out << "unknown_frees " << value.stats.unknownFrees << '\n';
    out.flush();
    if( !out ) { error = "session_memory_manifest_write_failed"; return false; }
    out.close();
    return AtomicReplace( temporary, root / "manifest", error );
}

bool LoadMemoryManifest( const std::filesystem::path& root,
    MemoryManifest& value, std::string& error )
{
    value = {};
    std::ifstream in( root / "manifest", std::ios::binary );
    if( !in ) { error = "session_memory_manifest_not_found"; return false; }
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
        else if( key == "pools" ) in >> value.stats.pools;
        else if( key == "events" ) in >> value.stats.events;
        else if( key == "event_blocks" ) in >> value.stats.eventBlocks;
        else if( key == "active_events" ) in >> value.stats.activeEvents;
        else if( key == "allocation_events" ) in >> value.stats.allocationEvents;
        else if( key == "free_events" ) in >> value.stats.freeEvents;
        else if( key == "discard_events" ) in >> value.stats.discardEvents;
        else if( key == "unknown_frees" ) in >> value.stats.unknownFrees;
        else { std::string ignored; std::getline( in, ignored ); }
        if( !in ) { error = "session_memory_manifest_parse_failed"; return false; }
    }
    value.stats.fileBytes = value.fileBytes;
    if( magic != MemoryManifestMagic || schema != TraceSessionMemoryIndexSchemaVersion ||
        value.sourceSha256.size() != 64 || value.fileSha256.size() != 64 )
    { error = "session_memory_manifest_invalid"; return false; }
    return true;
}

bool FinalizeMemoryFile( BuildState& state, const TraceSessionManifest& session,
    MemoryManifest& manifest, std::string& error )
{
    struct OrderedPool
    {
        PoolWork* work;
        std::string name;
        StoredPool stored;
        std::vector<StoredMemoryEventBlock> blocks;
    };
    std::vector<OrderedPool> pools;
    pools.reserve( state.pools.size() );
    for( auto& [id, pool] : state.pools )
    {
        if( !pool->Close( error ) ) return false;
        OrderedPool entry { pool.get(), ResolvePoolName( state, id ), {}, {} };
        entry.stored.nativeNameId = id;
        entry.stored.eventCount = pool->EventCount();
        entry.stored.activeCount = pool->ActiveCount();
        entry.stored.activeBytes = pool->ActiveBytes();
        entry.stored.freeCount = pool->FreeCount();
        entry.stored.low = pool->Low(); entry.stored.high = pool->High();
        entry.stored.nameBytes = uint32_t( std::min<size_t>( entry.name.size(),
            std::numeric_limits<uint32_t>::max() ) );
        if( entry.stored.nameBytes != entry.name.size() )
        { error = "session_memory_pool_name_too_large"; return false; }
        constexpr uint32_t EventsPerBlock = 4096;
        std::ifstream events( pool->Path(), std::ios::binary );
        if( !events ) { error = "session_memory_pool_work_read_failed"; return false; }
        entry.blocks.reserve( size_t( ( entry.stored.eventCount + EventsPerBlock - 1 ) / EventsPerBlock ) );
        for( uint64_t first = 0; first < entry.stored.eventCount; first += EventsPerBlock )
        {
            StoredMemoryEventBlock block;
            block.firstEvent = first;
            block.eventCount = uint32_t( std::min<uint64_t>( EventsPerBlock,
                entry.stored.eventCount - first ) );
            block.minAllocationNs = std::numeric_limits<int64_t>::max();
            block.maxFreeNs = std::numeric_limits<int64_t>::min();
            for( uint32_t index = 0; index < block.eventCount; ++index )
            {
                StoredMemoryEvent event;
                if( !events.read( reinterpret_cast<char*>( &event ), sizeof( event ) ) )
                { error = "session_memory_block_source_truncated"; return false; }
                block.minAllocationNs = std::min( block.minAllocationNs, event.allocationNs );
                block.maxFreeNs = event.freeNs >= 0 ?
                    std::max( block.maxFreeNs, event.freeNs ) : std::numeric_limits<int64_t>::max();
            }
            entry.blocks.emplace_back( block );
        }
        if( events.peek() != std::char_traits<char>::eof() )
        { error = "session_memory_block_source_trailing_bytes"; return false; }
        entry.stored.blockCount = entry.blocks.size();
        pools.emplace_back( std::move( entry ) );
    }
    std::sort( pools.begin(), pools.end(), []( const auto& left, const auto& right ) {
        return left.name != right.name ? left.name < right.name :
            left.stored.nativeNameId < right.stored.nativeNameId;
    } );

    MemoryFileHeader header;
    header.sourceSize = session.source.fileSize;
    header.poolCount = pools.size();
    header.generationBytes = uint32_t( session.generation.size() );
    header.poolTableOffset = sizeof( header ) + session.source.sha256.size() + session.generation.size();
    uint64_t cursor = header.poolTableOffset + pools.size() * sizeof( StoredPool );
    for( auto& pool : pools ) { pool.stored.nameOffset = cursor; cursor += pool.name.size(); }
    for( auto& pool : pools )
    {
        pool.stored.eventsOffset = cursor;
        cursor += pool.stored.eventCount * sizeof( StoredMemoryEvent );
        header.eventCount += pool.stored.eventCount;
        header.activeCount += pool.stored.activeCount;
    }
    for( auto& pool : pools )
    {
        pool.stored.blocksOffset = cursor;
        cursor += pool.stored.blockCount * sizeof( StoredMemoryEventBlock );
        header.eventBlockCount += pool.stored.blockCount;
    }

    const auto temporary = state.root / "memory.bin.tmp";
    const auto target = state.root / MemoryFileName;
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_memory_file_open_failed"; return false; }
    out.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
    out.write( session.source.sha256.data(), std::streamsize( session.source.sha256.size() ) );
    out.write( session.generation.data(), std::streamsize( session.generation.size() ) );
    for( const auto& pool : pools ) out.write( reinterpret_cast<const char*>( &pool.stored ), sizeof( pool.stored ) );
    for( const auto& pool : pools ) out.write( pool.name.data(), std::streamsize( pool.name.size() ) );
    for( const auto& pool : pools ) if( !CopyFileBytes( pool.work->Path(), out, error ) ) return false;
    for( const auto& pool : pools ) if( !pool.blocks.empty() ) out.write(
        reinterpret_cast<const char*>( pool.blocks.data() ),
        std::streamsize( pool.blocks.size() * sizeof( StoredMemoryEventBlock ) ) );
    out.flush();
    if( !out ) { error = "session_memory_file_write_failed"; return false; }
    out.close();
    if( !AtomicReplace( temporary, target, error ) ) return false;
    std::error_code ec;
    for( const auto& pool : pools ) { std::filesystem::remove( pool.work->Path(), ec ); ec.clear(); }

    manifest.sourceSha256 = session.source.sha256;
    manifest.sourceSize = session.source.fileSize;
    manifest.generation = session.generation;
    manifest.fileBytes = std::filesystem::file_size( target, ec );
    if( ec ) { error = "session_memory_file_size_failed:" + ec.message(); return false; }
    manifest.fileSha256 = Sha256File( target );
    manifest.stats = state.stats;
    manifest.stats.pools = pools.size();
    manifest.stats.events = header.eventCount;
    manifest.stats.eventBlocks = header.eventBlockCount;
    manifest.stats.activeEvents = header.activeCount;
    manifest.stats.fileBytes = manifest.fileBytes;
    return true;
}

}

struct TraceSessionMemoryReader::Impl
{
    struct Pool
    {
        StoredPool stored;
        std::string name;
        size_t sortedIndex = 0;
        std::vector<StoredMemoryEventBlock> blocks;
    };

    std::filesystem::path path;
    std::string fingerprint;
    std::vector<Pool> pools;
    std::unordered_map<uint64_t, size_t> poolByNative;
    std::unordered_map<std::string, size_t> poolByRef;

    bool ReadEvent( const Pool& pool, uint64_t index, StoredMemoryEvent& event ) const
    {
        if( index >= pool.stored.eventCount ) return false;
        std::ifstream in( path, std::ios::binary );
        if( !in ) return false;
        in.seekg( std::streamoff( pool.stored.eventsOffset + index * sizeof( event ) ) );
        return bool( in.read( reinterpret_cast<char*>( &event ), sizeof( event ) ) );
    }

    MemoryEventDto ToDto( const Pool& pool, uint64_t index,
        const StoredMemoryEvent& event ) const
    {
        MemoryEventDto dto;
        dto.ref = MakeRef( fingerprint, "memory-event",
            ( uint64_t( pool.sortedIndex ) << 40 ) | index );
        dto.poolRef = MakeRef( fingerprint, "memory-pool", pool.sortedIndex );
        if( IsGpuD3D12PoolName( pool.name ) ) dto.address = std::to_string( event.address );
        else { std::ostringstream out; out << "0x" << std::hex << event.address; dto.address = out.str(); }
        dto.size = event.size; dto.allocationNs = event.allocationNs;
        if( event.freeNs >= 0 ) dto.freeNs = event.freeNs;
        dto.allocationThreadRef = MakeRef( fingerprint, "thread", event.allocationThread );
        if( event.freeNs >= 0 ) dto.freeThreadRef = MakeRef( fingerprint, "thread", event.freeThread );
        dto.allocationCallstack = event.allocationCallstack;
        dto.freeCallstack = event.freeCallstack;
        if( event.allocationCallstack != 0 ) dto.allocationCallstackRef =
            MakeRef( fingerprint, "callstack", event.allocationCallstack );
        if( event.freeCallstack != 0 ) dto.freeCallstackRef =
            MakeRef( fingerprint, "callstack", event.freeCallstack );
        dto.complete = event.freeNs >= 0;
        return dto;
    }
};

TraceSessionMemoryReader::TraceSessionMemoryReader( std::shared_ptr<Impl> impl )
    : m_impl( std::move( impl ) )
{}

std::filesystem::path TraceSessionMemoryIndexRoot( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest )
{
    return sessionRoot / "generations" / manifest.generation / "derived" /
        "memory-index" / std::to_string( TraceSessionMemoryIndexSchemaVersion ) / "exact";
}

bool BuildTraceSessionMemoryDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionMemoryStats& stats, std::string& error )
{
    error.clear(); stats = {};
    BuildState state;
    state.root = TraceSessionMemoryIndexRoot( sessionRoot, manifest );
    std::error_code ec; std::filesystem::create_directories( state.root, ec );
    if( ec ) { error = "session_memory_directory_failed:" + ec.message(); return false; }
    if( !LoadTraceSessionTimeTransform( sessionRoot, manifest, state.transform, error ) ) return false;
    if( !VisitTraceSessionCanonicalOrdered( sessionRoot, manifest,
        VisitMemoryRecord, &state, error ) ) return false;
    if( state.pendingCallstack != 0 || state.serialNextCallstack != 0 || state.poolNamePending )
    { error = "session_memory_pending_protocol_state"; return false; }
    MemoryManifest memoryManifest;
    if( !FinalizeMemoryFile( state, manifest, memoryManifest, error ) ) return false;
    if( !SaveMemoryManifest( state.root, memoryManifest, error ) ) return false;
    stats = memoryManifest.stats;
    return true;
}

bool AuditTraceSessionMemoryDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& session, TraceSessionMemoryStats& stats, std::string& error )
{
    error.clear(); stats = {};
    const auto root = TraceSessionMemoryIndexRoot( sessionRoot, session );
    MemoryManifest manifest;
    if( !LoadMemoryManifest( root, manifest, error ) ) return false;
    if( manifest.sourceSha256 != session.source.sha256 || manifest.sourceSize != session.source.fileSize ||
        manifest.generation != session.generation )
    { error = "session_memory_identity_mismatch"; return false; }
    const auto path = root / MemoryFileName;
    std::error_code ec;
    if( std::filesystem::file_size( path, ec ) != manifest.fileBytes || ec )
    { error = "session_memory_file_size_mismatch"; return false; }
    if( Sha256File( path ) != manifest.fileSha256 )
    { error = "session_memory_file_sha256_mismatch"; return false; }
    stats = manifest.stats;
    return true;
}

std::shared_ptr<TraceSessionMemoryReader> TraceSessionMemoryReader::Open(
    const std::filesystem::path& sessionRoot, const TraceSessionManifest& session,
    std::string& error )
{
    error.clear();
    const auto root = TraceSessionMemoryIndexRoot( sessionRoot, session );
    MemoryManifest manifest;
    if( !LoadMemoryManifest( root, manifest, error ) ) return {};
    if( manifest.sourceSha256 != session.source.sha256 || manifest.sourceSize != session.source.fileSize ||
        manifest.generation != session.generation )
    { error = "session_memory_identity_mismatch"; return {}; }
    const auto path = root / MemoryFileName;
    std::error_code ec;
    if( std::filesystem::file_size( path, ec ) != manifest.fileBytes || ec )
    { error = "session_memory_file_size_mismatch"; return {}; }
    std::ifstream in( path, std::ios::binary );
    MemoryFileHeader header;
    if( !in.read( reinterpret_cast<char*>( &header ), sizeof( header ) ) ||
        header.magic != MemoryFileMagic || header.schema != TraceSessionMemoryIndexSchemaVersion ||
        header.endian != 0x01020304 || header.sourceSize != session.source.fileSize ||
        header.poolCount != manifest.stats.pools || header.eventCount != manifest.stats.events ||
        header.eventBlockCount != manifest.stats.eventBlocks ||
        header.activeCount != manifest.stats.activeEvents ||
        header.generationBytes != session.generation.size() )
    { error = "session_memory_file_header_invalid"; return {}; }
    std::string sha( 64, '\0' ), generation( header.generationBytes, '\0' );
    if( !in.read( sha.data(), std::streamsize( sha.size() ) ) ||
        !in.read( generation.data(), std::streamsize( generation.size() ) ) ||
        sha != session.source.sha256 || generation != session.generation )
    { error = "session_memory_file_identity_invalid"; return {}; }
    if( header.poolTableOffset != uint64_t( in.tellg() ) ||
        header.poolCount > manifest.fileBytes / sizeof( StoredPool ) )
    { error = "session_memory_pool_table_invalid"; return {}; }
    std::vector<StoredPool> stored( size_t( header.poolCount ) );
    if( !stored.empty() && !in.read( reinterpret_cast<char*>( stored.data() ),
        std::streamsize( stored.size() * sizeof( StoredPool ) ) ) )
    { error = "session_memory_pool_table_truncated"; return {}; }

    auto impl = std::make_shared<Impl>();
    impl->path = path; impl->fingerprint = session.source.sha256;
    impl->pools.reserve( stored.size() );
    uint64_t eventTotal = 0, eventBlockTotal = 0, activeTotal = 0;
    for( size_t index = 0; index < stored.size(); ++index )
    {
        const auto& value = stored[index];
        if( value.nameOffset > manifest.fileBytes || value.nameBytes > manifest.fileBytes - value.nameOffset ||
            value.eventsOffset > manifest.fileBytes ||
            value.eventCount > ( manifest.fileBytes - value.eventsOffset ) / sizeof( StoredMemoryEvent ) ||
            value.blocksOffset > manifest.fileBytes ||
            value.blockCount > ( manifest.fileBytes - value.blocksOffset ) / sizeof( StoredMemoryEventBlock ) )
        { error = "session_memory_pool_bounds_invalid"; return {}; }
        Impl::Pool pool; pool.stored = value; pool.sortedIndex = index;
        pool.name.resize( value.nameBytes );
        in.clear(); in.seekg( std::streamoff( value.nameOffset ) );
        if( !pool.name.empty() && !in.read( pool.name.data(), std::streamsize( pool.name.size() ) ) )
        { error = "session_memory_pool_name_truncated"; return {}; }
        pool.blocks.resize( size_t( value.blockCount ) );
        in.clear(); in.seekg( std::streamoff( value.blocksOffset ) );
        if( !pool.blocks.empty() && !in.read( reinterpret_cast<char*>( pool.blocks.data() ),
            std::streamsize( pool.blocks.size() * sizeof( StoredMemoryEventBlock ) ) ) )
        { error = "session_memory_block_records_truncated"; return {}; }
        uint64_t expectedFirstEvent = 0;
        for( const auto& block : pool.blocks )
        {
            if( block.firstEvent != expectedFirstEvent || block.eventCount == 0 ||
                block.eventCount > 4096 || block.firstEvent + block.eventCount > value.eventCount ||
                block.minAllocationNs > block.maxFreeNs )
            { error = "session_memory_block_record_invalid"; return {}; }
            expectedFirstEvent += block.eventCount;
        }
        if( expectedFirstEvent != value.eventCount )
        { error = "session_memory_block_coverage_invalid"; return {}; }
        if( !impl->poolByNative.emplace( value.nativeNameId, index ).second )
        { error = "session_memory_pool_native_id_duplicate"; return {}; }
        impl->poolByRef.emplace( MakeRef( impl->fingerprint, "memory-pool", index ), index );
        eventTotal += value.eventCount; eventBlockTotal += value.blockCount; activeTotal += value.activeCount;
        impl->pools.emplace_back( std::move( pool ) );
    }
    if( eventTotal != header.eventCount || eventBlockTotal != header.eventBlockCount ||
        activeTotal != header.activeCount )
    { error = "session_memory_pool_total_mismatch"; return {}; }
    auto reader = std::shared_ptr<TraceSessionMemoryReader>(
        new TraceSessionMemoryReader( std::move( impl ) ) );
    reader->m_stats = manifest.stats;
    for( const auto& pool : reader->m_impl->pools )
    {
        MemoryPoolDto dto;
        dto.ref = MakeRef( reader->m_impl->fingerprint, "memory-pool", pool.sortedIndex );
        dto.nativeNameId = pool.stored.nativeNameId; dto.name = pool.name;
        dto.eventCount = pool.stored.eventCount; dto.activeCount = pool.stored.activeCount;
        dto.activeBytes = pool.stored.activeBytes; dto.low = pool.stored.low; dto.high = pool.stored.high;
        dto.gpuD3D12 = IsGpuD3D12PoolName( pool.name ); dto.freeCount = pool.stored.freeCount;
        dto.persistedUsageBytes = pool.stored.activeBytes; dto.storedNameId = pool.stored.nativeNameId;
        dto.storedName = pool.name;
        reader->m_pools.emplace_back( std::move( dto ) );
    }
    return reader;
}

std::vector<MemoryEventDto> TraceSessionMemoryReader::Scan( const ScanRange& range ) const
{
    return ScanImpl( range, std::nullopt );
}

std::vector<MemoryEventDto> TraceSessionMemoryReader::ScanPool(
    std::string_view poolRef, const ScanRange& range ) const
{
    const auto found = m_impl->poolByRef.find( std::string( poolRef ) );
    return found == m_impl->poolByRef.end() ? std::vector<MemoryEventDto> {} :
        ScanImpl( range, found->second );
}

std::vector<MemoryEventDto> TraceSessionMemoryReader::ScanImpl(
    const ScanRange& range, std::optional<size_t> poolIndex ) const
{
    std::vector<MemoryEventDto> result;
    if( range.limit == 0 || ( poolIndex && *poolIndex >= m_impl->pools.size() ) ) return result;
    size_t skipped = 0;
    const auto firstPool = poolIndex.value_or( 0 );
    const auto lastPool = poolIndex ? *poolIndex + 1 : m_impl->pools.size();
    std::ifstream in( m_impl->path, std::ios::binary );
    if( !in ) return result;
    for( size_t poolNumber = firstPool; poolNumber < lastPool; ++poolNumber )
    {
        const auto& pool = m_impl->pools[poolNumber];
        for( const auto& block : pool.blocks )
        {
            if( block.maxFreeNs < range.startNs || block.minAllocationNs > range.endNs ) continue;
            in.clear();
            in.seekg( std::streamoff( pool.stored.eventsOffset +
                block.firstEvent * sizeof( StoredMemoryEvent ) ) );
            for( uint32_t blockIndex = 0; blockIndex < block.eventCount; ++blockIndex )
            {
                StoredMemoryEvent event;
                if( !in.read( reinterpret_cast<char*>( &event ), sizeof( event ) ) ) return result;
                const auto end = event.freeNs >= 0 ? event.freeNs : std::numeric_limits<int64_t>::max();
                if( end < range.startNs || event.allocationNs > range.endNs ) continue;
                if( skipped++ < range.offset ) continue;
                result.emplace_back( m_impl->ToDto( pool, block.firstEvent + blockIndex, event ) );
                if( result.size() >= range.limit ) return result;
            }
        }
    }
    return result;
}

std::optional<MemoryEventDto> TraceSessionMemoryReader::Get( const MemoryEventKey& key ) const
{
    const auto found = m_impl->poolByNative.find( key.pool );
    if( found == m_impl->poolByNative.end() ) return std::nullopt;
    const auto& pool = m_impl->pools[found->second];
    StoredMemoryEvent event;
    if( !m_impl->ReadEvent( pool, key.index, event ) ) return std::nullopt;
    return m_impl->ToDto( pool, key.index, event );
}

std::optional<std::string> TraceSessionMemoryReader::PoolRef( uint64_t nativePool ) const
{
    const auto found = m_impl->poolByNative.find( nativePool );
    if( found == m_impl->poolByNative.end() ) return std::nullopt;
    return MakeRef( m_impl->fingerprint, "memory-pool", found->second );
}

MemoryFrameSnapshot TraceSessionMemoryReader::Snapshot( int64_t beginNs, int64_t endNs,
    const std::vector<std::string>& poolRefs, bool allGpuD3D12Pools ) const
{
    std::vector<const Impl::Pool*> selected;
    if( allGpuD3D12Pools )
    {
        for( const auto& pool : m_impl->pools ) if( IsGpuD3D12PoolName( pool.name ) ) selected.push_back( &pool );
    }
    else if( !poolRefs.empty() )
    {
        for( const auto& ref : poolRefs )
        {
            const auto found = m_impl->poolByRef.find( ref );
            if( found != m_impl->poolByRef.end() ) selected.push_back( &m_impl->pools[found->second] );
        }
    }
    else for( const auto& pool : m_impl->pools ) selected.push_back( &pool );

    std::vector<uint64_t> nativePools;
    std::vector<MemoryEventInput> events;
    for( const auto* pool : selected )
    {
        nativePools.push_back( pool->stored.nativeNameId );
        std::ifstream in( m_impl->path, std::ios::binary );
        if( !in ) return {};
        for( const auto& block : pool->blocks )
        {
            if( block.maxFreeNs < beginNs || block.minAllocationNs >= endNs ) continue;
            in.clear();
            in.seekg( std::streamoff( pool->stored.eventsOffset +
                block.firstEvent * sizeof( StoredMemoryEvent ) ) );
            for( uint32_t blockIndex = 0; blockIndex < block.eventCount; ++blockIndex )
            {
                StoredMemoryEvent event;
                if( !in.read( reinterpret_cast<char*>( &event ), sizeof( event ) ) ) return {};
                const auto freeNs = event.freeNs >= 0 ? event.freeNs : std::numeric_limits<int64_t>::max();
                if( event.allocationNs >= endNs || freeNs < beginNs ) continue;
                MemoryEventInput input;
                input.key = { pool->stored.nativeNameId, size_t( block.firstEvent + blockIndex ) };
                input.identifier = event.address; input.size = event.size;
                input.allocationNs = event.allocationNs;
                if( event.freeNs >= 0 ) input.freeNs = event.freeNs;
                input.allocationThread = event.allocationThread; input.freeThread = event.freeThread;
                input.allocationCallstack = event.allocationCallstack; input.freeCallstack = event.freeCallstack;
                events.emplace_back( input );
            }
        }
    }
    return BuildMemoryFrameSnapshot( beginNs, endNs, nativePools, events, false );
}

}

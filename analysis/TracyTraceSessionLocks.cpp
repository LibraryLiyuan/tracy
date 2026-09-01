#include "TracyTraceSessionLocks.hpp"

#include "TracyHash.hpp"
#include "TracyTraceSessionCanonical.hpp"
#include "TracyTraceSessionGpuCanonical.hpp"
#include "TracyProtocol.hpp"
#include "TracyQueue.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#  include <Windows.h>
#endif

namespace tracy::analysis
{
namespace
{

constexpr uint64_t LockFileMagic = 0x314b434c534e4aull;     // JNSLCK1
constexpr uint64_t LockManifestMagic = 0x31464d4c534e4aull; // JNSLMF1
constexpr const char* LockFileName = "locks.bin";
constexpr uint64_t NoThread = std::numeric_limits<uint64_t>::max();

enum class StoredLockEventType : uint8_t
{
    Wait, Obtain, Release, WaitShared, ObtainShared, ReleaseShared
};

#pragma pack( push, 1 )
struct LockFileHeader
{
    uint64_t magic = LockFileMagic;
    uint32_t schema = TraceSessionLockIndexSchemaVersion;
    uint32_t endian = 0x01020304;
    uint64_t sourceSize = 0;
    uint64_t lockCount = 0;
    uint64_t threadCount = 0;
    uint64_t eventCount = 0;
    uint64_t locksOffset = 0;
    uint64_t threadsOffset = 0;
    uint64_t eventsOffset = 0;
    uint64_t stringsOffset = 0;
    uint64_t stringBytes = 0;
    uint32_t generationBytes = 0;
    uint32_t reserved = 0;
};

struct StoredLock
{
    uint32_t id = 0;
    int32_t sourceNativeId = 0;
    int64_t announceNs = 0;
    int64_t terminateNs = 0;
    uint64_t eventCount = 0;
    uint64_t threadOffset = 0;
    uint64_t nameOffset = 0;
    uint32_t threadCount = 0;
    uint32_t nameBytes = 0;
    uint8_t type = 0;
    uint8_t flags = 0;
    uint16_t reserved = 0;
};

struct StoredLockEvent
{
    uint32_t lockId = 0;
    int32_t sourceNativeId = -1;
    int64_t timeNs = 0;
    uint64_t thread = 0;
    uint64_t ownerThread = NoThread;
    uint64_t waiterMask = 0;
    uint32_t lockCount = 0;
    uint8_t type = 0;
    uint8_t reserved[3] {};
};
#pragma pack( pop )

enum StoredLockFlags : uint8_t
{
    LockValid = 1u << 0,
    LockContended = 1u << 1,
    LockTerminated = 1u << 2
};

struct LockManifest
{
    std::string sourceSha256;
    uint64_t sourceSize = 0;
    std::string generation;
    uint64_t fileBytes = 0;
    std::string fileSha256;
    TraceSessionLockStats stats;
};

struct LockState
{
    StoredLock stored;
    std::string customName;
    std::vector<uint64_t> threads;
    std::unordered_map<uint64_t, uint8_t> threadIndices;
    std::unordered_map<uint64_t, uint64_t> lastMarkableEvent;
    uint64_t waitList = 0;
    uint64_t waitShared = 0;
    uint64_t sharedList = 0;
    uint64_t ownerThread = NoThread;
    uint32_t lockCount = 0;
};

struct BuildState
{
    TraceSessionTimeTransform transform;
    int64_t refTimeSerial = 0;
    std::optional<std::string> pendingSingleString;
    std::unordered_map<uint64_t, int32_t> staticSources;
    std::map<uint32_t, LockState> locks;
    std::filesystem::path eventWorkPath;
    std::fstream eventWork;
    TraceSessionLockStats stats;
};

bool AtomicReplace( const std::filesystem::path& source,
    const std::filesystem::path& target, std::string& error )
{
#ifdef _WIN32
    if( MoveFileExW( source.c_str(), target.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) ) return true;
    error = "session_lock_atomic_replace_failed:" + std::to_string( GetLastError() );
    return false;
#else
    std::error_code ec;
    std::filesystem::rename( source, target, ec );
    if( !ec ) return true;
    error = "session_lock_atomic_replace_failed:" + ec.message();
    return false;
#endif
}

bool DecodeItem( const TraceSessionCanonicalRecord& record, QueueItem& item,
    std::string& error )
{
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ||
        record.type >= uint8_t( QueueType::NUM_TYPES ) ||
        record.payload.size() < QueueDataSize[record.type] )
    { error = "session_lock_protocol_record_invalid"; return false; }
    item = {};
    std::memcpy( &item, record.payload.data(),
        std::min<size_t>( record.payload.size(), sizeof( item ) ) );
    if( item.hdr.idx != record.type )
    { error = "session_lock_protocol_type_mismatch"; return false; }
    return true;
}

bool GetShortPayload( const TraceSessionCanonicalRecord& record,
    const uint8_t*& data, size_t& size, std::string& error )
{
    const auto fixed = size_t( QueueDataSize[record.type] );
    if( record.payload.size() < fixed + sizeof( uint16_t ) )
    { error = "session_lock_payload_truncated"; return false; }
    uint16_t bytes = 0;
    std::memcpy( &bytes, record.payload.data() + fixed, sizeof( bytes ) );
    if( bytes != record.variablePayloadBytes ||
        record.payload.size() != fixed + sizeof( bytes ) + bytes )
    { error = "session_lock_payload_mismatch"; return false; }
    data = record.payload.data() + fixed + sizeof( bytes );
    size = bytes;
    return true;
}

int32_t EnsureStaticSource( BuildState& state, uint64_t pointer )
{
    const auto found = state.staticSources.find( pointer );
    if( found != state.staticSources.end() ) return found->second;
    const auto id = int32_t( state.staticSources.size() );
    state.staticSources.emplace( pointer, id );
    return id;
}

int64_t AdvanceSerial( BuildState& state, int64_t delta )
{
    state.refTimeSerial += delta;
    return state.transform.ToNanoseconds( state.refTimeSerial );
}

bool ThreadBit( LockState& lock, uint64_t thread, uint8_t& index,
    std::string& error )
{
    const auto found = lock.threadIndices.find( thread );
    if( found != lock.threadIndices.end() ) { index = found->second; return true; }
    if( lock.threads.size() >= 64 )
    { error = "session_lock_thread_capacity_exceeded"; return false; }
    index = uint8_t( lock.threads.size() );
    lock.threads.emplace_back( thread );
    lock.threadIndices.emplace( thread, index );
    return true;
}

bool WriteEvent( BuildState& state, LockState& lock, StoredLockEvent event,
    bool markable, std::string& error )
{
    const auto index = state.stats.events;
    state.eventWork.seekp( std::streamoff( index * sizeof( StoredLockEvent ) ) );
    state.eventWork.write( reinterpret_cast<const char*>( &event ), sizeof( event ) );
    if( !state.eventWork )
    { error = "session_lock_event_work_write_failed"; return false; }
    if( markable ) lock.lastMarkableEvent[event.thread] = index;
    ++state.stats.events;
    ++lock.stored.eventCount;
    return true;
}

bool AppendLockEvent( BuildState& state, QueueType type, uint32_t lockId,
    uint64_t thread, int64_t delta, std::string& error )
{
    const auto found = state.locks.find( lockId );
    if( found == state.locks.end() )
    { error = "session_lock_event_without_announce"; return false; }
    auto& lock = found->second;
    uint8_t threadIndex = 0;
    if( !ThreadBit( lock, thread, threadIndex, error ) ) return false;
    const auto bit = uint64_t( 1 ) << threadIndex;
    StoredLockEvent event;
    event.lockId = lockId;
    event.timeNs = AdvanceSerial( state, delta );
    event.thread = thread;
    event.type = uint8_t( StoredLockEventType::Wait );
    bool markable = false;
    switch( type )
    {
    case QueueType::LockWait:
        lock.waitList |= bit; event.type = uint8_t( StoredLockEventType::Wait ); markable = true;
        ++state.stats.waitEvents; break;
    case QueueType::LockObtain:
        if( ( lock.waitList & bit ) == 0 || lock.lockCount == std::numeric_limits<uint32_t>::max() )
        { error = "session_lock_obtain_state_invalid"; return false; }
        lock.waitList &= ~bit; lock.ownerThread = thread; ++lock.lockCount;
        event.type = uint8_t( StoredLockEventType::Obtain ); markable = true;
        ++state.stats.obtainEvents; break;
    case QueueType::LockRelease:
        if( lock.lockCount == 0 )
        { error = "session_lock_release_state_invalid"; return false; }
        --lock.lockCount; event.type = uint8_t( StoredLockEventType::Release );
        ++state.stats.releaseEvents; break;
    case QueueType::LockSharedWait:
        if( lock.stored.type != uint8_t( LockType::SharedLockable ) )
        { error = "session_lock_shared_event_on_exclusive_lock"; return false; }
        lock.waitShared |= bit; event.type = uint8_t( StoredLockEventType::WaitShared ); markable = true;
        ++state.stats.sharedWaitEvents; break;
    case QueueType::LockSharedObtain:
        if( lock.stored.type != uint8_t( LockType::SharedLockable ) ||
            ( lock.waitShared & bit ) == 0 || ( lock.sharedList & bit ) != 0 )
        { error = "session_lock_shared_obtain_state_invalid"; return false; }
        lock.waitShared &= ~bit; lock.sharedList |= bit;
        event.type = uint8_t( StoredLockEventType::ObtainShared ); markable = true;
        ++state.stats.sharedObtainEvents; break;
    case QueueType::LockSharedRelease:
        if( lock.stored.type != uint8_t( LockType::SharedLockable ) ||
            ( lock.sharedList & bit ) == 0 )
        { error = "session_lock_shared_release_state_invalid"; return false; }
        lock.sharedList &= ~bit; event.type = uint8_t( StoredLockEventType::ReleaseShared );
        ++state.stats.sharedReleaseEvents; break;
    default: error = "session_lock_event_type_invalid"; return false;
    }
    event.lockCount = lock.lockCount;
    event.ownerThread = lock.lockCount == 0 ? NoThread : lock.ownerThread;
    event.waiterMask = lock.waitList | lock.waitShared;
    if( ( lock.lockCount != 0 && event.waiterMask != 0 ) ||
        ( lock.sharedList != 0 && lock.waitList != 0 ) )
        lock.stored.flags |= LockContended;
    return WriteEvent( state, lock, event, markable, error );
}

bool AdvanceOtherSerial( BuildState& state, const QueueItem& item, QueueType type )
{
    switch( type )
    {
    case QueueType::MemAlloc: case QueueType::MemAllocCallstack:
    case QueueType::MemAllocNamed: case QueueType::MemAllocCallstackNamed:
    case QueueType::JnMemAllocCallsiteNamed:
        AdvanceSerial( state, type == QueueType::JnMemAllocCallsiteNamed ?
            item.jnMemAllocCallsite.time : item.memAlloc.time ); return true;
    case QueueType::MemFree: case QueueType::MemFreeNamed:
    case QueueType::MemFreeCallstack: case QueueType::MemFreeCallstackNamed:
        AdvanceSerial( state, item.memFree.time ); return true;
    case QueueType::MemDiscard: case QueueType::MemDiscardCallstack:
        AdvanceSerial( state, item.memDiscard.time ); return true;
    case QueueType::GpuZoneBeginSerial: case QueueType::GpuZoneBeginCallstackSerial:
        AdvanceSerial( state, item.gpuZoneBegin.cpuTime ); return true;
    case QueueType::GpuZoneBeginAllocSrcLocSerial:
    case QueueType::GpuZoneBeginAllocSrcLocCallstackSerial:
        AdvanceSerial( state, item.gpuZoneBeginLean.cpuTime ); return true;
    case QueueType::GpuZoneEndSerial:
        AdvanceSerial( state, item.gpuZoneEnd.cpuTime ); return true;
    default: return false;
    }
}

bool VisitLockRecord( const TraceSessionCanonicalRecord& record,
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
    switch( type )
    {
    case QueueType::JnCallsiteDefinition:
        EnsureStaticSource( state, item.jnCallsiteDefinition.srcloc ); break;
    case QueueType::ZoneBegin: case QueueType::ZoneBeginCallstack:
    case QueueType::JnZoneBeginCallsite:
        EnsureStaticSource( state, item.zoneBegin.srcloc ); break;
    case QueueType::GpuZoneBegin: case QueueType::GpuZoneBeginCallstack:
    case QueueType::JnGpuZoneBeginCallsite: case QueueType::GpuZoneBeginSerial:
    case QueueType::GpuZoneBeginCallstackSerial:
        EnsureStaticSource( state, item.gpuZoneBegin.srcloc ); break;
    case QueueType::LockAnnounce:
    {
        if( state.locks.contains( item.lockAnnounce.id ) )
        { error = "session_lock_duplicate_announce"; return false; }
        LockState lock;
        lock.stored.id = item.lockAnnounce.id;
        lock.stored.sourceNativeId = EnsureStaticSource( state, item.lockAnnounce.lckloc );
        lock.stored.announceNs = state.transform.ToNanoseconds( item.lockAnnounce.time );
        lock.stored.type = uint8_t( item.lockAnnounce.type );
        lock.stored.flags = LockValid;
        state.locks.emplace( item.lockAnnounce.id, std::move( lock ) );
        ++state.stats.announceEvents;
        break;
    }
    case QueueType::LockTerminate:
    {
        const auto found = state.locks.find( item.lockTerminate.id );
        if( found == state.locks.end() || ( found->second.stored.flags & LockTerminated ) != 0 )
        { error = "session_lock_terminate_state_invalid"; return false; }
        found->second.stored.terminateNs =
            state.transform.ToNanoseconds( item.lockTerminate.time );
        found->second.stored.flags |= LockTerminated;
        ++state.stats.terminateEvents;
        break;
    }
    case QueueType::LockName:
    {
        const auto found = state.locks.find( item.lockName.id );
        if( found == state.locks.end() || !state.pendingSingleString )
        { error = "session_lock_name_state_invalid"; return false; }
        found->second.customName = std::move( *state.pendingSingleString );
        state.pendingSingleString.reset();
        ++state.stats.nameEvents;
        return true;
    }
    case QueueType::LockWait: case QueueType::LockObtain:
    case QueueType::LockSharedWait: case QueueType::LockSharedObtain:
        if( !AppendLockEvent( state, type, item.lockWait.id,
            item.lockWait.thread, item.lockWait.time, error ) ) return false;
        break;
    case QueueType::LockRelease:
    {
        const auto found = state.locks.find( item.lockRelease.id );
        if( found == state.locks.end() || found->second.ownerThread == NoThread )
        { error = "session_lock_release_without_owner"; return false; }
        if( !AppendLockEvent( state, type, item.lockRelease.id,
            found->second.ownerThread, item.lockRelease.time, error ) ) return false;
        break;
    }
    case QueueType::LockSharedRelease:
        if( !AppendLockEvent( state, type, item.lockReleaseShared.id,
            item.lockReleaseShared.thread, item.lockReleaseShared.time, error ) ) return false;
        break;
    case QueueType::LockMark:
    {
        const auto found = state.locks.find( item.lockMark.id );
        if( found == state.locks.end() )
        { error = "session_lock_mark_without_announce"; return false; }
        const auto event = found->second.lastMarkableEvent.find( item.lockMark.thread );
        if( event == found->second.lastMarkableEvent.end() )
        { error = "session_lock_mark_without_event"; return false; }
        const auto source = EnsureStaticSource( state, item.lockMark.srcloc );
        state.eventWork.seekp( std::streamoff(
            event->second * sizeof( StoredLockEvent ) +
            offsetof( StoredLockEvent, sourceNativeId ) ) );
        state.eventWork.write( reinterpret_cast<const char*>( &source ), sizeof( source ) );
        if( !state.eventWork )
        { error = "session_lock_mark_work_write_failed"; return false; }
        ++state.stats.markEvents;
        break;
    }
    default:
        AdvanceOtherSerial( state, item, type );
        break;
    }
    if( type != QueueType::LockName ) state.pendingSingleString.reset();
    return true;
}

bool CopyFile( const std::filesystem::path& path, std::ofstream& out,
    std::string& error )
{
    std::ifstream in( path, std::ios::binary );
    if( !in ) { error = "session_lock_work_read_failed"; return false; }
    // The converter uses the default Windows 1 MiB stack. Keep bulk-copy
    // storage on the heap so the lock domain cannot overflow that stack when
    // reached through the complete Mandatory Derived call chain.
    std::vector<char> buffer( 1024 * 1024 );
    while( in )
    {
        in.read( buffer.data(), std::streamsize( buffer.size() ) );
        const auto bytes = in.gcount();
        if( bytes > 0 ) out.write( buffer.data(), bytes );
    }
    if( !in.eof() || !out )
    { error = "session_lock_work_copy_failed"; return false; }
    return true;
}

bool SaveManifest( const std::filesystem::path& root,
    const LockManifest& value, std::string& error )
{
    const auto temporary = root / "manifest.tmp";
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_lock_manifest_open_failed"; return false; }
    const auto& s = value.stats;
    out << "magic " << LockManifestMagic << '\n' << "schema " << TraceSessionLockIndexSchemaVersion << '\n'
        << "source_sha256 " << value.sourceSha256 << '\n' << "source_size " << value.sourceSize << '\n'
        << "generation " << std::quoted( value.generation ) << '\n' << "file_bytes " << value.fileBytes << '\n'
        << "file_sha256 " << value.fileSha256 << '\n' << "stats "
        << s.locks << ' ' << s.events << ' ' << s.announceEvents << ' ' << s.terminateEvents << ' '
        << s.waitEvents << ' ' << s.obtainEvents << ' ' << s.releaseEvents << ' '
        << s.sharedWaitEvents << ' ' << s.sharedObtainEvents << ' ' << s.sharedReleaseEvents << ' '
        << s.nameEvents << ' ' << s.markEvents << ' ' << s.fileBytes << '\n';
    out.flush();
    if( !out ) { error = "session_lock_manifest_write_failed"; return false; }
    out.close();
    return AtomicReplace( temporary, root / "manifest", error );
}

bool LoadManifest( const std::filesystem::path& root,
    LockManifest& value, std::string& error )
{
    std::ifstream in( root / "manifest", std::ios::binary );
    std::string key; uint64_t magic = 0; uint32_t schema = 0;
    if( !( in >> key >> magic ) || key != "magic" || magic != LockManifestMagic ||
        !( in >> key >> schema ) || key != "schema" || schema != TraceSessionLockIndexSchemaVersion ||
        !( in >> key >> value.sourceSha256 ) || key != "source_sha256" ||
        !( in >> key >> value.sourceSize ) || key != "source_size" ||
        !( in >> key >> std::quoted( value.generation ) ) || key != "generation" ||
        !( in >> key >> value.fileBytes ) || key != "file_bytes" ||
        !( in >> key >> value.fileSha256 ) || key != "file_sha256" ||
        !( in >> key ) || key != "stats" )
    { error = "session_lock_manifest_parse_failed"; return false; }
    auto& s = value.stats;
    if( !( in >> s.locks >> s.events >> s.announceEvents >> s.terminateEvents >>
        s.waitEvents >> s.obtainEvents >> s.releaseEvents >> s.sharedWaitEvents >>
        s.sharedObtainEvents >> s.sharedReleaseEvents >> s.nameEvents >>
        s.markEvents >> s.fileBytes ) )
    { error = "session_lock_manifest_parse_failed"; return false; }
    return true;
}

const char* LockTypeName( uint8_t type )
{
    return type == uint8_t( LockType::SharedLockable ) ? "shared_lockable" : "lockable";
}

const char* EventTypeName( uint8_t type )
{
    switch( StoredLockEventType( type ) )
    {
    case StoredLockEventType::Wait: return "wait";
    case StoredLockEventType::Obtain: return "obtain";
    case StoredLockEventType::Release: return "release";
    case StoredLockEventType::WaitShared: return "wait_shared";
    case StoredLockEventType::ObtainShared: return "obtain_shared";
    case StoredLockEventType::ReleaseShared: return "release_shared";
    }
    return "unknown";
}

std::string MakeRef( const std::string& fingerprint, const char* kind,
    uint64_t id )
{
    std::ostringstream out;
    out << "tracy:v1:" << fingerprint.substr( 0, 16 ) << ':' << kind << ':'
        << std::hex << id;
    return out.str();
}

}

struct TraceSessionLockReader::Impl
{
    std::filesystem::path path;
    std::string fingerprint;
    uint64_t eventsOffset = 0;
    uint64_t eventCount = 0;
    std::vector<StoredLock> locks;
    std::vector<std::vector<uint64_t>> threads;
    std::vector<std::string> names;
    std::unordered_map<uint32_t, size_t> lockIndices;
};

TraceSessionLockReader::TraceSessionLockReader( std::shared_ptr<Impl> impl )
    : m_impl( std::move( impl ) ) {}

std::filesystem::path TraceSessionLockIndexRoot(
    const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest )
{
    return sessionRoot / "generations" / manifest.generation / "derived" /
        "lock-index" / "1" / "exact";
}

bool BuildTraceSessionLockDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& session, TraceSessionLockStats& stats,
    std::string& error )
{
    error.clear(); stats = {};
    const auto root = TraceSessionLockIndexRoot( sessionRoot, session );
    std::error_code ec; std::filesystem::create_directories( root, ec );
    if( ec ) { error = "session_lock_directory_failed:" + ec.message(); return false; }
    BuildState state;
    state.eventWorkPath = root / "events.work";
    state.eventWork.open( state.eventWorkPath,
        std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc );
    if( !state.eventWork ) { error = "session_lock_work_open_failed"; return false; }
    if( !LoadTraceSessionTimeTransform( sessionRoot, session, state.transform, error ) ||
        !VisitTraceSessionCanonicalOrdered( sessionRoot, session,
            VisitLockRecord, &state, error ) ) return false;
    state.eventWork.flush(); state.eventWork.close();
    if( !state.eventWork ) { error = "session_lock_work_flush_failed"; return false; }

    state.stats.locks = state.locks.size();
    std::vector<StoredLock> locks;
    std::vector<uint64_t> threads;
    std::string names;
    locks.reserve( state.locks.size() );
    for( auto& [id, lock] : state.locks )
    {
        lock.stored.threadOffset = threads.size();
        lock.stored.threadCount = uint32_t( lock.threads.size() );
        threads.insert( threads.end(), lock.threads.begin(), lock.threads.end() );
        lock.stored.nameOffset = names.size();
        if( lock.customName.size() > std::numeric_limits<uint32_t>::max() )
        { error = "session_lock_name_too_large"; return false; }
        lock.stored.nameBytes = uint32_t( lock.customName.size() );
        names += lock.customName;
        locks.emplace_back( lock.stored );
    }

    LockFileHeader header;
    header.sourceSize = session.source.fileSize;
    header.lockCount = locks.size(); header.threadCount = threads.size();
    header.eventCount = state.stats.events;
    header.generationBytes = uint32_t( session.generation.size() );
    header.locksOffset = sizeof( header ) + session.source.sha256.size() +
        session.generation.size();
    header.threadsOffset = header.locksOffset + locks.size() * sizeof( StoredLock );
    header.eventsOffset = header.threadsOffset + threads.size() * sizeof( uint64_t );
    header.stringsOffset = header.eventsOffset + state.stats.events * sizeof( StoredLockEvent );
    header.stringBytes = names.size();
    const auto temporary = root / "locks.bin.tmp";
    const auto target = root / LockFileName;
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_lock_file_open_failed"; return false; }
    out.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
    out.write( session.source.sha256.data(), std::streamsize( session.source.sha256.size() ) );
    out.write( session.generation.data(), std::streamsize( session.generation.size() ) );
    if( !locks.empty() ) out.write( reinterpret_cast<const char*>( locks.data() ),
        std::streamsize( locks.size() * sizeof( StoredLock ) ) );
    if( !threads.empty() ) out.write( reinterpret_cast<const char*>( threads.data() ),
        std::streamsize( threads.size() * sizeof( uint64_t ) ) );
    if( !CopyFile( state.eventWorkPath, out, error ) ) return false;
    out.write( names.data(), std::streamsize( names.size() ) );
    out.flush();
    if( !out ) { error = "session_lock_file_write_failed"; return false; }
    out.close();
    if( !AtomicReplace( temporary, target, error ) ) return false;
    std::filesystem::remove( state.eventWorkPath, ec );

    LockManifest manifest;
    manifest.sourceSha256 = session.source.sha256;
    manifest.sourceSize = session.source.fileSize;
    manifest.generation = session.generation;
    manifest.fileBytes = std::filesystem::file_size( target, ec );
    if( ec ) { error = "session_lock_file_size_failed:" + ec.message(); return false; }
    manifest.fileSha256 = Sha256File( target );
    state.stats.fileBytes = manifest.fileBytes;
    manifest.stats = state.stats;
    if( !SaveManifest( root, manifest, error ) ) return false;
    stats = state.stats;
    return true;
}

bool AuditTraceSessionLockDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& session, TraceSessionLockStats& stats,
    std::string& error )
{
    error.clear(); stats = {};
    const auto root = TraceSessionLockIndexRoot( sessionRoot, session );
    LockManifest manifest;
    if( !LoadManifest( root, manifest, error ) ) return false;
    if( manifest.sourceSha256 != session.source.sha256 ||
        manifest.sourceSize != session.source.fileSize ||
        manifest.generation != session.generation )
    { error = "session_lock_identity_mismatch"; return false; }
    const auto path = root / LockFileName;
    std::error_code ec;
    if( std::filesystem::file_size( path, ec ) != manifest.fileBytes || ec )
    { error = "session_lock_file_size_mismatch"; return false; }
    if( Sha256File( path ) != manifest.fileSha256 )
    { error = "session_lock_file_sha256_mismatch"; return false; }
    stats = manifest.stats;
    return true;
}

std::shared_ptr<TraceSessionLockReader> TraceSessionLockReader::Open(
    const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& session, std::string& error )
{
    TraceSessionLockStats stats;
    if( !AuditTraceSessionLockDerived( sessionRoot, session, stats, error ) ) return {};
    const auto path = TraceSessionLockIndexRoot( sessionRoot, session ) / LockFileName;
    std::ifstream in( path, std::ios::binary );
    LockFileHeader header;
    if( !in.read( reinterpret_cast<char*>( &header ), sizeof( header ) ) ||
        header.magic != LockFileMagic || header.schema != TraceSessionLockIndexSchemaVersion ||
        header.endian != 0x01020304 || header.sourceSize != session.source.fileSize ||
        header.lockCount != stats.locks || header.eventCount != stats.events ||
        header.generationBytes != session.generation.size() )
    { error = "session_lock_file_header_invalid"; return {}; }
    std::string sha( 64, '\0' ), generation( header.generationBytes, '\0' );
    if( !in.read( sha.data(), std::streamsize( sha.size() ) ) ||
        !in.read( generation.data(), std::streamsize( generation.size() ) ) ||
        sha != session.source.sha256 || generation != session.generation ||
        uint64_t( in.tellg() ) != header.locksOffset ||
        header.threadsOffset != header.locksOffset + header.lockCount * sizeof( StoredLock ) ||
        header.eventsOffset != header.threadsOffset + header.threadCount * sizeof( uint64_t ) ||
        header.stringsOffset != header.eventsOffset + header.eventCount * sizeof( StoredLockEvent ) ||
        header.stringBytes != stats.fileBytes - header.stringsOffset )
    { error = "session_lock_file_identity_or_bounds_invalid"; return {}; }
    auto impl = std::make_shared<Impl>();
    impl->path = path; impl->fingerprint = session.source.sha256;
    impl->eventsOffset = header.eventsOffset; impl->eventCount = header.eventCount;
    impl->locks.resize( size_t( header.lockCount ) );
    if( !impl->locks.empty() && !in.read( reinterpret_cast<char*>( impl->locks.data() ),
        std::streamsize( impl->locks.size() * sizeof( StoredLock ) ) ) )
    { error = "session_lock_definition_truncated"; return {}; }
    std::vector<uint64_t> allThreads( size_t( header.threadCount ) );
    if( !allThreads.empty() && !in.read( reinterpret_cast<char*>( allThreads.data() ),
        std::streamsize( allThreads.size() * sizeof( uint64_t ) ) ) )
    { error = "session_lock_thread_table_truncated"; return {}; }
    impl->threads.resize( impl->locks.size() ); impl->names.resize( impl->locks.size() );
    for( size_t index = 0; index < impl->locks.size(); ++index )
    {
        const auto& lock = impl->locks[index];
        if( lock.threadOffset > allThreads.size() ||
            lock.threadCount > allThreads.size() - lock.threadOffset ||
            lock.nameOffset > header.stringBytes ||
            lock.nameBytes > header.stringBytes - lock.nameOffset ||
            !impl->lockIndices.emplace( lock.id, index ).second )
        { error = "session_lock_definition_invalid"; return {}; }
        impl->threads[index].assign( allThreads.begin() + lock.threadOffset,
            allThreads.begin() + lock.threadOffset + lock.threadCount );
        if( lock.nameBytes != 0 )
        {
            impl->names[index].resize( lock.nameBytes );
            in.seekg( std::streamoff( header.stringsOffset + lock.nameOffset ) );
            if( !in.read( impl->names[index].data(), std::streamsize( lock.nameBytes ) ) )
            { error = "session_lock_name_truncated"; return {}; }
        }
    }
    auto reader = std::shared_ptr<TraceSessionLockReader>(
        new TraceSessionLockReader( std::move( impl ) ) );
    reader->m_stats = stats;
    return reader;
}

std::vector<LockDto> TraceSessionLockReader::Locks() const
{
    std::vector<LockDto> result;
    result.reserve( m_impl->locks.size() );
    for( size_t index = 0; index < m_impl->locks.size(); ++index )
    {
        const auto& value = m_impl->locks[index];
        LockDto dto;
        dto.ref = MakeRef( m_impl->fingerprint, "lock", value.id );
        dto.nativeId = value.id;
        dto.name = m_impl->names[index].empty() ?
            "Lock " + std::to_string( value.id ) : m_impl->names[index];
        dto.sourceLocationRef = MakeRef( m_impl->fingerprint, "source",
            uint16_t( value.sourceNativeId ) );
        dto.eventCount = value.eventCount;
        dto.threadCount = value.threadCount;
        dto.valid = ( value.flags & LockValid ) != 0;
        dto.contended = ( value.flags & LockContended ) != 0;
        dto.announceNs = value.announceNs;
        if( value.flags & LockTerminated ) dto.terminateNs = value.terminateNs;
        dto.type = value.type; dto.typeName = LockTypeName( value.type );
        if( !m_impl->names[index].empty() ) dto.customName = m_impl->names[index];
        result.emplace_back( std::move( dto ) );
    }
    return result;
}

std::vector<LockEventDto> TraceSessionLockReader::Scan(
    const ScanRange& range ) const
{
    std::vector<LockEventDto> result;
    std::ifstream in( m_impl->path, std::ios::binary );
    if( !in ) return result;
    in.seekg( std::streamoff( m_impl->eventsOffset ) );
    size_t skipped = 0;
    for( uint64_t index = 0; index < m_impl->eventCount; ++index )
    {
        StoredLockEvent value;
        if( !in.read( reinterpret_cast<char*>( &value ), sizeof( value ) ) ) return result;
        if( value.timeNs < range.startNs || value.timeNs >= range.endNs ) continue;
        if( skipped++ < range.offset ) continue;
        const auto lockFound = m_impl->lockIndices.find( value.lockId );
        if( lockFound == m_impl->lockIndices.end() ) return {};
        const auto& threads = m_impl->threads[lockFound->second];
        LockEventDto dto;
        dto.ref = MakeRef( m_impl->fingerprint, "lock-event", index );
        dto.lockRef = MakeRef( m_impl->fingerprint, "lock", value.lockId );
        dto.timeNs = value.timeNs;
        dto.threadRef = MakeRef( m_impl->fingerprint, "thread", value.thread );
        dto.type = EventTypeName( value.type ); dto.lockCount = value.lockCount;
        if( value.ownerThread != NoThread ) dto.ownerThreadRef =
            MakeRef( m_impl->fingerprint, "thread", value.ownerThread );
        for( size_t bit = 0; bit < threads.size() && bit < 64; ++bit )
            if( value.waiterMask & ( uint64_t( 1 ) << bit ) )
                dto.waiterThreadRefs.emplace_back(
                    MakeRef( m_impl->fingerprint, "thread", threads[bit] ) );
        if( value.sourceNativeId >= 0 ) dto.sourceLocationRef =
            MakeRef( m_impl->fingerprint, "source", uint16_t( value.sourceNativeId ) );
        result.emplace_back( std::move( dto ) );
        if( result.size() >= range.limit ) break;
    }
    return result;
}

}

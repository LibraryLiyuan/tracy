#include "TracyTraceSessionScheduling.hpp"

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
#include <map>
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

constexpr uint64_t SchedulingFileMagic = 0x31484353534e4aull;     // JNSSCH1
constexpr uint64_t SchedulingManifestMagic = 0x31464d43534e4aull; // JNSCMF1
constexpr const char* SchedulingFileName = "scheduling.bin";
constexpr int8_t WakeupReason = -2;

#pragma pack( push, 1 )
struct SchedulingFileHeader
{
    uint64_t magic = SchedulingFileMagic;
    uint32_t schema = TraceSessionSchedulingIndexSchemaVersion;
    uint32_t endian = 0x01020304;
    uint64_t sourceSize = 0;
    uint64_t contextSwitchRecords = 0;
    uint64_t wakeupRecords = 0;
    uint64_t threadEventCount = 0;
    uint64_t completeThreadEventCount = 0;
    uint64_t cpuEventCount = 0;
    uint64_t completeCpuEventCount = 0;
    uint64_t sourceGapEventCount = 0;
    uint64_t threadRecordsOffset = 0;
    uint64_t cpuRecordsOffset = 0;
    uint32_t generationBytes = 0;
    uint32_t reserved = 0;
};

struct StoredThreadEvent
{
    int64_t startNs = 0;
    int64_t endNs = -1;
    int64_t wakeupNs = -1;
    uint64_t thread = 0;
    uint8_t cpu = 0;
    uint8_t wakeupCpu = 0;
    int8_t reason = -1;
    int8_t state = -1;
    uint16_t relatedThreadIndex = 0;
    uint16_t reserved = 0;
};

struct StoredCpuEvent
{
    int64_t startNs = 0;
    int64_t endNs = -1;
    uint64_t thread = 0;
    uint32_t cpu = 0;
    uint16_t rawThreadIndex = 0;
    uint16_t reserved = 0;
};
#pragma pack( pop )

struct SchedulingManifest
{
    std::string sourceSha256;
    uint64_t sourceSize = 0;
    std::string generation;
    uint64_t fileBytes = 0;
    std::string fileSha256;
    TraceSessionSchedulingStats stats;
};

bool AtomicReplace( const std::filesystem::path& source,
    const std::filesystem::path& target, std::string& error )
{
#ifdef _WIN32
    if( MoveFileExW( source.c_str(), target.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) ) return true;
    error = "session_scheduling_atomic_replace_failed:" + std::to_string( GetLastError() );
    return false;
#else
    std::error_code ec;
    std::filesystem::rename( source, target, ec );
    if( !ec ) return true;
    error = "session_scheduling_atomic_replace_failed:" + ec.message();
    return false;
#endif
}

bool DecodeItem( const TraceSessionCanonicalRecord& record, QueueItem& item,
    std::string& error )
{
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ||
        record.type >= uint8_t( QueueType::NUM_TYPES ) ||
        record.payload.size() < QueueDataSize[record.type] )
    { error = "session_scheduling_protocol_record_invalid"; return false; }
    item = {};
    std::memcpy( &item, record.payload.data(),
        std::min<size_t>( record.payload.size(), sizeof( item ) ) );
    if( item.hdr.idx != record.type )
    { error = "session_scheduling_protocol_type_mismatch"; return false; }
    return true;
}

std::string MakeRef( const std::string& fingerprint, const char* kind, uint64_t id )
{
    std::ostringstream out;
    out << "tracy:v1:" << fingerprint.substr( 0, 16 ) << ':' << kind << ':' << std::hex << id;
    return out.str();
}

bool CopyFileBytes( const std::filesystem::path& source, std::ofstream& out,
    std::string& error )
{
    std::ifstream in( source, std::ios::binary );
    if( !in ) { error = "session_scheduling_work_read_failed"; return false; }
    std::vector<char> buffer( 1024 * 1024 );
    while( in )
    {
        in.read( buffer.data(), std::streamsize( buffer.size() ) );
        const auto count = in.gcount();
        if( count > 0 ) out.write( buffer.data(), count );
    }
    if( !in.eof() || !out ) { error = "session_scheduling_work_copy_failed"; return false; }
    return true;
}

template<typename T>
class WorkFile
{
public:
    bool Open( const std::filesystem::path& path, std::string& error )
    {
        m_path = path;
        m_file.open( path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc );
        if( !m_file ) { error = "session_scheduling_work_open_failed"; return false; }
        return true;
    }

    bool Append( const T& value, uint64_t& index, std::string& error )
    {
        index = m_count;
        m_file.clear();
        m_file.seekp( std::streamoff( m_count * sizeof( T ) ) );
        m_file.write( reinterpret_cast<const char*>( &value ), sizeof( value ) );
        if( !m_file ) { error = "session_scheduling_work_append_failed"; return false; }
        ++m_count;
        return true;
    }

    bool Patch( uint64_t index, const T& value, std::string& error )
    {
        if( index >= m_count ) { error = "session_scheduling_work_patch_index_invalid"; return false; }
        m_file.clear();
        m_file.seekp( std::streamoff( index * sizeof( T ) ) );
        m_file.write( reinterpret_cast<const char*>( &value ), sizeof( value ) );
        if( !m_file ) { error = "session_scheduling_work_patch_failed"; return false; }
        return true;
    }

    bool Close( std::string& error )
    {
        m_file.flush();
        if( !m_file ) { error = "session_scheduling_work_flush_failed"; return false; }
        m_file.close();
        return true;
    }

    uint64_t Count() const { return m_count; }
    const std::filesystem::path& Path() const { return m_path; }

private:
    std::filesystem::path m_path;
    std::fstream m_file;
    uint64_t m_count = 0;
};

struct ThreadRuntime
{
    StoredThreadEvent last;
    uint64_t lastIndex = 0;
    bool hasLast = false;
    int64_t pendingWakeupNs = 0;
    uint8_t pendingWakeupCpu = 0;
};

struct CpuRuntime
{
    StoredCpuEvent last;
    uint64_t lastIndex = 0;
    bool active = false;
};

struct BuildState
{
    TraceSessionTimeTransform transform;
    std::filesystem::path root;
    WorkFile<StoredThreadEvent> threadWork;
    WorkFile<StoredCpuEvent> cpuWork;
    std::map<uint64_t, std::unique_ptr<ThreadRuntime>> threads;
    std::map<uint32_t, std::unique_ptr<CpuRuntime>> cpus;
    std::unordered_map<uint64_t, uint16_t> compressedThreads;
    int64_t contextTime = 0;
    TraceSessionSchedulingStats stats;
};

ThreadRuntime* EnsureThread( BuildState& state, uint64_t thread, std::string& error )
{
    const auto found = state.threads.find( thread );
    if( found != state.threads.end() ) return found->second.get();
    auto runtime = std::make_unique<ThreadRuntime>();
    return state.threads.emplace( thread, std::move( runtime ) ).first->second.get();
}

CpuRuntime* EnsureCpu( BuildState& state, uint32_t cpu, std::string& error )
{
    const auto found = state.cpus.find( cpu );
    if( found != state.cpus.end() ) return found->second.get();
    auto runtime = std::make_unique<CpuRuntime>();
    return state.cpus.emplace( cpu, std::move( runtime ) ).first->second.get();
}

bool AppendThreadEvent( BuildState& state, ThreadRuntime& runtime,
    const StoredThreadEvent& value, std::string& error )
{
    if( !state.threadWork.Append( value, runtime.lastIndex, error ) ) return false;
    runtime.last = value; runtime.hasLast = true; ++state.stats.threadEvents;
    return true;
}

bool PatchThreadEvent( BuildState& state, ThreadRuntime& runtime,
    const StoredThreadEvent& value, std::string& error )
{
    if( !runtime.hasLast || !state.threadWork.Patch( runtime.lastIndex, value, error ) ) return false;
    runtime.last = value;
    return true;
}

uint16_t CompressExternalThread( BuildState& state, uint64_t thread,
    std::string& error )
{
    const auto found = state.compressedThreads.find( thread );
    if( found != state.compressedThreads.end() ) return found->second;
    if( state.compressedThreads.size() > std::numeric_limits<uint16_t>::max() )
    { error = "session_scheduling_thread_compression_overflow"; return 0; }
    const auto index = uint16_t( state.compressedThreads.size() );
    state.compressedThreads.emplace( thread, index );
    return index;
}

int64_t AdvanceContext( BuildState& state, int64_t delta )
{
    state.contextTime += delta;
    return state.transform.ToNanoseconds( state.contextTime );
}

bool ProcessWakeup( BuildState& state, const QueueThreadWakeup& event,
    int64_t timeNs, std::string& error )
{
    auto* runtime = EnsureThread( state, event.thread, error );
    if( !runtime ) return false;
    if( runtime->hasLast && runtime->last.endNs < 0 )
    {
        runtime->pendingWakeupNs = timeNs;
        runtime->pendingWakeupCpu = event.cpu;
        return true;
    }
    StoredThreadEvent value;
    value.startNs = timeNs; value.wakeupNs = timeNs; value.thread = event.thread;
    value.wakeupCpu = event.cpu; value.reason = WakeupReason;
    return AppendThreadEvent( state, *runtime, value, error );
}

bool ProcessSwitchOut( BuildState& state, const QueueContextSwitch& event,
    int64_t timeNs, std::string& error )
{
    if( event.oldThread != 0 )
    {
        const auto found = state.threads.find( event.oldThread );
        if( found != state.threads.end() )
        {
            auto& runtime = *found->second;
            if( !runtime.hasLast || runtime.last.endNs >= 0 || runtime.last.startNs > timeNs )
            { ++state.stats.sourceGapEvents; }
            else
            {
                auto value = runtime.last;
                value.endNs = timeNs;
                value.reason = int8_t( event.oldThreadWaitReason );
                value.state = int8_t( event.oldThreadState );
                if( !PatchThreadEvent( state, runtime, value, error ) ) return false;
                ++state.stats.completeThreadEvents;
            }
        }
        auto* cpu = EnsureCpu( state, event.cpu, error );
        if( !cpu ) return false;
        if( cpu->active )
        {
            if( cpu->last.thread != event.oldThread || cpu->last.startNs > timeNs )
            { ++state.stats.sourceGapEvents; }
            else
            {
                auto value = cpu->last; value.endNs = timeNs;
                if( !state.cpuWork.Patch( cpu->lastIndex, value, error ) ) return false;
                cpu->last = value; cpu->active = false; ++state.stats.completeCpuEvents;
            }
        }
        else ++state.stats.sourceGapEvents;
    }
    return true;
}

bool ProcessSwitchIn( BuildState& state, const QueueContextSwitch& event,
    int64_t timeNs, std::string& error )
{
    if( event.newThread == 0 ) return true;
    auto* runtime = EnsureThread( state, event.newThread, error );
    if( !runtime ) return false;
    StoredThreadEvent value;
    bool reuseWakeup = runtime->hasLast && runtime->last.reason == WakeupReason;
    if( reuseWakeup )
    {
        if( runtime->last.endNs >= 0 )
        { error = "session_scheduling_wakeup_event_already_closed"; return false; }
        value = runtime->last;
    }
    else
    {
        if( runtime->hasLast && runtime->last.endNs < 0 )
            ++state.stats.sourceGapEvents;
        value.startNs = timeNs; value.wakeupNs = timeNs; value.thread = event.newThread;
        value.wakeupCpu = event.cpu;
        if( runtime->pendingWakeupNs != 0 && runtime->hasLast &&
            runtime->last.wakeupNs <= runtime->pendingWakeupNs && runtime->pendingWakeupNs <= timeNs )
        {
            value.wakeupNs = runtime->pendingWakeupNs;
            value.wakeupCpu = runtime->pendingWakeupCpu;
            runtime->pendingWakeupNs = 0;
        }
        if( !AppendThreadEvent( state, *runtime, value, error ) ) return false;
    }
    value.startNs = timeNs; value.endNs = -1; value.cpu = event.cpu;
    value.reason = -1; value.state = -1; value.relatedThreadIndex = 0;
    if( !PatchThreadEvent( state, *runtime, value, error ) ) return false;

    auto* cpu = EnsureCpu( state, event.cpu, error );
    if( !cpu ) return false;
    if( cpu->active )
        ++state.stats.sourceGapEvents;
    StoredCpuEvent cpuValue;
    cpuValue.startNs = timeNs; cpuValue.thread = event.newThread; cpuValue.cpu = event.cpu;
    cpuValue.rawThreadIndex = CompressExternalThread( state, event.newThread, error );
    if( !error.empty() ) return false;
    if( !state.cpuWork.Append( cpuValue, cpu->lastIndex, error ) ) return false;
    cpu->last = cpuValue; cpu->active = true; ++state.stats.cpuEvents;
    return true;
}

bool VisitSchedulingRecord( const TraceSessionCanonicalRecord& record,
    void* userData, std::string& error )
{
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ) return true;
    auto& state = *static_cast<BuildState*>( userData );
    QueueItem item {};
    if( !DecodeItem( record, item, error ) ) return false;
    switch( QueueType( record.type ) )
    {
    case QueueType::CallstackSample:
    case QueueType::CallstackSampleContextSwitch:
        (void)AdvanceContext( state, item.callstackSample.time );
        break;
    case QueueType::CallstackSampleRef:
    case QueueType::CallstackSampleContextSwitchRef:
        (void)AdvanceContext( state, item.callstackSampleRef.time );
        break;
    case QueueType::ThreadWakeup:
    {
        const auto timeNs = AdvanceContext( state, item.threadWakeup.time );
        ++state.stats.wakeupRecords;
        if( !ProcessWakeup( state, item.threadWakeup, timeNs, error ) ) return false;
        break;
    }
    case QueueType::ContextSwitch:
    {
        const auto timeNs = AdvanceContext( state, item.contextSwitch.time );
        ++state.stats.contextSwitchRecords;
        if( !ProcessSwitchOut( state, item.contextSwitch, timeNs, error ) ||
            !ProcessSwitchIn( state, item.contextSwitch, timeNs, error ) ) return false;
        break;
    }
    default: break;
    }
    return true;
}

const char* ReasonName( int8_t value )
{
    if( value == -2 ) return "wakeup";
    if( value == 99 ) return "fiber";
    if( value == 100 ) return "no_state";
    static constexpr const char* names[] = {
        "executive", "free_page", "page_in", "pool_allocation", "delay_execution",
        "suspended", "user_request", "wr_executive", "wr_free_page", "wr_page_in",
        "wr_pool_allocation", "wr_delay_execution", "wr_suspended", "wr_user_request",
        "wr_event_pair", "wr_queue", "wr_lpc_receive", "wr_lpc_reply", "wr_virtual_memory",
        "wr_page_out", "wr_rendezvous", "wr_keyed_event", "wr_terminated",
        "wr_process_in_swap", "wr_cpu_rate_control", "wr_callout_stack", "wr_kernel",
        "wr_resource", "wr_push_lock", "wr_mutex", "wr_quantum_end",
        "wr_dispatch_interrupt", "wr_preempted", "wr_yield_execution", "wr_fast_mutex",
        "wr_guarded_mutex", "wr_rundown", "wr_alert_by_thread_id", "wr_deferred_preempt",
        "wr_physical_fault", "wr_io_ring", "wr_mdl_cache", "wr_rcu"
    };
    return value >= 0 && size_t( value ) < std::size( names ) ? names[value] : "unknown";
}

const char* StateName( int8_t value )
{
    switch( value )
    {
    case 0: return "initialized"; case 1: return "ready"; case 2: return "running";
    case 3: return "standby"; case 4: return "terminated"; case 5: return "waiting";
    case 6: return "transition"; case 7: return "deferred_ready";
    case 101: return "disk_sleep"; case 102: return "idle"; case 103: return "run_queue";
    case 104: return "sleeping"; case 105: return "stopped"; case 106: return "tracing_stop";
    case 107: return "paging"; case 108: return "dead"; case 109: return "zombie";
    case 110: return "parked"; default: return "unknown";
    }
}

bool Intersects( int64_t begin, int64_t end, const ScanRange& range )
{
    return begin < range.endNs && end > range.startNs;
}

bool SaveSchedulingManifest( const std::filesystem::path& root,
    const SchedulingManifest& value, std::string& error )
{
    const auto temporary = root / "manifest.tmp";
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_scheduling_manifest_open_failed"; return false; }
    out << "magic " << SchedulingManifestMagic << '\n';
    out << "schema " << TraceSessionSchedulingIndexSchemaVersion << '\n';
    out << "source_sha256 " << std::quoted( value.sourceSha256 ) << '\n';
    out << "source_size " << value.sourceSize << '\n';
    out << "generation " << std::quoted( value.generation ) << '\n';
    out << "file_bytes " << value.fileBytes << '\n';
    out << "file_sha256 " << std::quoted( value.fileSha256 ) << '\n';
    out << "context_switch_records " << value.stats.contextSwitchRecords << '\n';
    out << "wakeup_records " << value.stats.wakeupRecords << '\n';
    out << "thread_events " << value.stats.threadEvents << '\n';
    out << "complete_thread_events " << value.stats.completeThreadEvents << '\n';
    out << "cpu_events " << value.stats.cpuEvents << '\n';
    out << "complete_cpu_events " << value.stats.completeCpuEvents << '\n';
    out << "source_gap_events " << value.stats.sourceGapEvents << '\n';
    out.flush();
    if( !out ) { error = "session_scheduling_manifest_write_failed"; return false; }
    out.close();
    return AtomicReplace( temporary, root / "manifest", error );
}

bool LoadSchedulingManifest( const std::filesystem::path& root,
    SchedulingManifest& value, std::string& error )
{
    value = {};
    std::ifstream in( root / "manifest", std::ios::binary );
    if( !in ) { error = "session_scheduling_manifest_not_found"; return false; }
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
        else if( key == "context_switch_records" ) in >> value.stats.contextSwitchRecords;
        else if( key == "wakeup_records" ) in >> value.stats.wakeupRecords;
        else if( key == "thread_events" ) in >> value.stats.threadEvents;
        else if( key == "complete_thread_events" ) in >> value.stats.completeThreadEvents;
        else if( key == "cpu_events" ) in >> value.stats.cpuEvents;
        else if( key == "complete_cpu_events" ) in >> value.stats.completeCpuEvents;
        else if( key == "source_gap_events" ) in >> value.stats.sourceGapEvents;
        else { std::string ignored; std::getline( in, ignored ); }
        if( !in ) { error = "session_scheduling_manifest_parse_failed"; return false; }
    }
    value.stats.fileBytes = value.fileBytes;
    if( magic != SchedulingManifestMagic || schema != TraceSessionSchedulingIndexSchemaVersion ||
        value.sourceSha256.size() != 64 || value.fileSha256.size() != 64 ||
        value.stats.completeThreadEvents > value.stats.threadEvents ||
        value.stats.completeCpuEvents > value.stats.cpuEvents )
    { error = "session_scheduling_manifest_invalid"; return false; }
    return true;
}

bool FinalizeSchedulingFile( BuildState& state, const TraceSessionManifest& session,
    SchedulingManifest& manifest, std::string& error )
{
    if( !state.threadWork.Close( error ) || !state.cpuWork.Close( error ) ) return false;
    SchedulingFileHeader header;
    header.sourceSize = session.source.fileSize;
    header.contextSwitchRecords = state.stats.contextSwitchRecords;
    header.wakeupRecords = state.stats.wakeupRecords;
    header.threadEventCount = state.stats.threadEvents;
    header.completeThreadEventCount = state.stats.completeThreadEvents;
    header.cpuEventCount = state.stats.cpuEvents;
    header.completeCpuEventCount = state.stats.completeCpuEvents;
    header.sourceGapEventCount = state.stats.sourceGapEvents;
    header.generationBytes = uint32_t( session.generation.size() );
    header.threadRecordsOffset = sizeof( header ) + session.source.sha256.size() + session.generation.size();
    header.cpuRecordsOffset = header.threadRecordsOffset + header.threadEventCount * sizeof( StoredThreadEvent );
    const auto temporary = state.root / "scheduling.bin.tmp";
    const auto target = state.root / SchedulingFileName;
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_scheduling_file_open_failed"; return false; }
    out.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
    out.write( session.source.sha256.data(), std::streamsize( session.source.sha256.size() ) );
    out.write( session.generation.data(), std::streamsize( session.generation.size() ) );
    if( !CopyFileBytes( state.threadWork.Path(), out, error ) ||
        !CopyFileBytes( state.cpuWork.Path(), out, error ) ) return false;
    out.flush();
    if( !out ) { error = "session_scheduling_file_write_failed"; return false; }
    out.close();
    if( !AtomicReplace( temporary, target, error ) ) return false;
    std::error_code ec;
    const std::array<std::filesystem::path, 2> workFiles = {
        state.threadWork.Path(), state.cpuWork.Path() };
    for( const auto& path : workFiles )
    {
        if( !std::filesystem::remove( path, ec ) || ec )
        { error = "session_scheduling_work_cleanup_failed:" + ( ec ? ec.message() : path.string() ); return false; }
        ec.clear();
    }
    manifest.sourceSha256 = session.source.sha256;
    manifest.sourceSize = session.source.fileSize;
    manifest.generation = session.generation;
    manifest.fileBytes = std::filesystem::file_size( target, ec );
    if( ec ) { error = "session_scheduling_file_size_failed:" + ec.message(); return false; }
    manifest.fileSha256 = Sha256File( target );
    manifest.stats = state.stats; manifest.stats.fileBytes = manifest.fileBytes;
    return true;
}

}

struct TraceSessionSchedulingReader::Impl
{
    std::filesystem::path path;
    std::string fingerprint;
    uint64_t threadRecordsOffset = 0;
    uint64_t threadEventCount = 0;
    uint64_t cpuRecordsOffset = 0;
    uint64_t cpuEventCount = 0;
};

TraceSessionSchedulingReader::TraceSessionSchedulingReader( std::shared_ptr<Impl> impl )
    : m_impl( std::move( impl ) )
{}

std::filesystem::path TraceSessionSchedulingIndexRoot( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest )
{
    return sessionRoot / "generations" / manifest.generation / "derived" /
        "scheduling-index" / std::to_string( TraceSessionSchedulingIndexSchemaVersion ) / "exact";
}

bool BuildTraceSessionSchedulingDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionSchedulingStats& stats, std::string& error )
{
    error.clear(); stats = {};
    BuildState state;
    state.root = TraceSessionSchedulingIndexRoot( sessionRoot, manifest );
    std::error_code ec; std::filesystem::create_directories( state.root, ec );
    if( ec ) { error = "session_scheduling_directory_failed:" + ec.message(); return false; }
    for( std::filesystem::directory_iterator it( state.root, ec ), end; it != end; it.increment( ec ) )
    {
        if( ec ) { error = "session_scheduling_work_cleanup_scan_failed:" + ec.message(); return false; }
        if( !it->is_regular_file( ec ) )
        { if( ec ) { error = "session_scheduling_work_cleanup_scan_failed:" + ec.message(); return false; } continue; }
        if( it->path().extension() != ".work" ) continue;
        ec.clear();
        if( !std::filesystem::remove( it->path(), ec ) || ec )
        { error = "session_scheduling_work_cleanup_failed:" + ( ec ? ec.message() : it->path().string() ); return false; }
    }
    if( ec ) { error = "session_scheduling_work_cleanup_scan_failed:" + ec.message(); return false; }
    if( !state.threadWork.Open( state.root / "thread-events.work", error ) ||
        !state.cpuWork.Open( state.root / "cpu-events.work", error ) ) return false;
    if( !LoadTraceSessionTimeTransform( sessionRoot, manifest, state.transform, error ) ) return false;
    if( !VisitTraceSessionCanonicalOrdered( sessionRoot, manifest,
        VisitSchedulingRecord, &state, error ) ) return false;
    SchedulingManifest schedulingManifest;
    if( !FinalizeSchedulingFile( state, manifest, schedulingManifest, error ) ) return false;
    if( !SaveSchedulingManifest( state.root, schedulingManifest, error ) ) return false;
    stats = schedulingManifest.stats;
    return true;
}

bool AuditTraceSessionSchedulingDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& session, TraceSessionSchedulingStats& stats, std::string& error )
{
    error.clear(); stats = {};
    const auto root = TraceSessionSchedulingIndexRoot( sessionRoot, session );
    SchedulingManifest manifest;
    if( !LoadSchedulingManifest( root, manifest, error ) ) return false;
    if( manifest.sourceSha256 != session.source.sha256 || manifest.sourceSize != session.source.fileSize ||
        manifest.generation != session.generation )
    { error = "session_scheduling_identity_mismatch"; return false; }
    const auto path = root / SchedulingFileName;
    std::error_code ec;
    if( std::filesystem::file_size( path, ec ) != manifest.fileBytes || ec )
    { error = "session_scheduling_file_size_mismatch"; return false; }
    if( Sha256File( path ) != manifest.fileSha256 )
    { error = "session_scheduling_file_sha256_mismatch"; return false; }
    stats = manifest.stats;
    return true;
}

std::shared_ptr<TraceSessionSchedulingReader> TraceSessionSchedulingReader::Open(
    const std::filesystem::path& sessionRoot, const TraceSessionManifest& session,
    std::string& error )
{
    error.clear();
    const auto root = TraceSessionSchedulingIndexRoot( sessionRoot, session );
    SchedulingManifest manifest;
    if( !LoadSchedulingManifest( root, manifest, error ) ) return {};
    if( manifest.sourceSha256 != session.source.sha256 || manifest.sourceSize != session.source.fileSize ||
        manifest.generation != session.generation )
    { error = "session_scheduling_identity_mismatch"; return {}; }
    const auto path = root / SchedulingFileName;
    std::error_code ec;
    if( std::filesystem::file_size( path, ec ) != manifest.fileBytes || ec )
    { error = "session_scheduling_file_size_mismatch"; return {}; }
    std::ifstream in( path, std::ios::binary );
    SchedulingFileHeader header;
    if( !in.read( reinterpret_cast<char*>( &header ), sizeof( header ) ) ||
        header.magic != SchedulingFileMagic || header.schema != TraceSessionSchedulingIndexSchemaVersion ||
        header.endian != 0x01020304 || header.sourceSize != session.source.fileSize ||
        header.contextSwitchRecords != manifest.stats.contextSwitchRecords ||
        header.wakeupRecords != manifest.stats.wakeupRecords ||
        header.threadEventCount != manifest.stats.threadEvents ||
        header.completeThreadEventCount != manifest.stats.completeThreadEvents ||
        header.cpuEventCount != manifest.stats.cpuEvents ||
        header.completeCpuEventCount != manifest.stats.completeCpuEvents ||
        header.sourceGapEventCount != manifest.stats.sourceGapEvents ||
        header.generationBytes != session.generation.size() )
    { error = "session_scheduling_file_header_invalid"; return {}; }
    std::string sha( 64, '\0' ), generation( header.generationBytes, '\0' );
    if( !in.read( sha.data(), std::streamsize( sha.size() ) ) ||
        !in.read( generation.data(), std::streamsize( generation.size() ) ) ||
        sha != session.source.sha256 || generation != session.generation ||
        header.threadRecordsOffset != uint64_t( in.tellg() ) ||
        header.cpuRecordsOffset != header.threadRecordsOffset +
            header.threadEventCount * sizeof( StoredThreadEvent ) ||
        header.cpuRecordsOffset + header.cpuEventCount * sizeof( StoredCpuEvent ) != manifest.fileBytes )
    { error = "session_scheduling_file_identity_or_bounds_invalid"; return {}; }
    auto impl = std::make_shared<Impl>();
    impl->path = path; impl->fingerprint = session.source.sha256;
    impl->threadRecordsOffset = header.threadRecordsOffset;
    impl->threadEventCount = header.threadEventCount;
    impl->cpuRecordsOffset = header.cpuRecordsOffset;
    impl->cpuEventCount = header.cpuEventCount;
    auto reader = std::shared_ptr<TraceSessionSchedulingReader>(
        new TraceSessionSchedulingReader( std::move( impl ) ) );
    reader->m_stats = manifest.stats;
    return reader;
}

std::vector<ContextSwitchDto> TraceSessionSchedulingReader::ScanThreads(
    const ScanRange& range ) const
{
    std::vector<ContextSwitchDto> result;
    std::ifstream in( m_impl->path, std::ios::binary );
    if( !in ) return result;
    in.seekg( std::streamoff( m_impl->threadRecordsOffset ) );
    size_t skipped = 0;
    for( uint64_t ordinal = 0; ordinal < m_impl->threadEventCount; ++ordinal )
    {
        StoredThreadEvent value;
        if( !in.read( reinterpret_cast<char*>( &value ), sizeof( value ) ) ) return result;
        const auto end = value.endNs >= 0 ? value.endNs : value.startNs;
        if( !Intersects( value.startNs, end, range ) ) continue;
        if( skipped++ < range.offset ) continue;
        ContextSwitchDto dto;
        dto.ref = MakeRef( m_impl->fingerprint, "context-switch", ordinal );
        dto.threadRef = MakeRef( m_impl->fingerprint, "thread", value.thread );
        dto.startNs = value.startNs;
        if( value.endNs >= 0 ) dto.endNs = value.endNs;
        if( value.wakeupNs >= 0 ) dto.wakeupNs = value.wakeupNs;
        dto.cpu = value.cpu; dto.wakeupCpu = value.wakeupCpu;
        dto.reason = value.reason; dto.state = value.state;
        dto.complete = value.endNs >= 0;
        dto.reasonName = ReasonName( value.reason ); dto.stateName = StateName( value.state );
        dto.relatedThreadIndex = value.relatedThreadIndex;
        dto.wakeupCpuAvailability.available = true;
        result.emplace_back( std::move( dto ) );
        if( result.size() >= range.limit ) break;
    }
    return result;
}

std::vector<CpuContextSwitchDto> TraceSessionSchedulingReader::ScanCpus(
    const ScanRange& range ) const
{
    std::vector<CpuContextSwitchDto> result;
    std::ifstream in( m_impl->path, std::ios::binary );
    if( !in ) return result;
    in.seekg( std::streamoff( m_impl->cpuRecordsOffset ) );
    size_t skipped = 0;
    for( uint64_t ordinal = 0; ordinal < m_impl->cpuEventCount; ++ordinal )
    {
        StoredCpuEvent value;
        if( !in.read( reinterpret_cast<char*>( &value ), sizeof( value ) ) ) return result;
        const auto complete = value.endNs >= 0;
        const auto end = complete ? value.endNs : value.startNs;
        const auto intersects = complete ? Intersects( value.startNs, end, range ) :
            value.startNs >= range.startNs && value.startNs < range.endNs;
        if( !intersects ) continue;
        if( skipped++ < range.offset ) continue;
        CpuContextSwitchDto dto;
        dto.ref = MakeRef( m_impl->fingerprint, "cpu-context-switch", ordinal );
        dto.cpu = value.cpu; dto.startNs = value.startNs;
        if( complete ) dto.endNs = value.endNs;
        dto.rawThreadIndex = value.rawThreadIndex;
        dto.threadRef = MakeRef( m_impl->fingerprint, "thread", value.thread );
        dto.complete = complete;
        result.emplace_back( std::move( dto ) );
        if( result.size() >= range.limit ) break;
    }
    return result;
}

}

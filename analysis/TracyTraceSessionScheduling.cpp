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
#include <queue>
#include <sstream>
#include <tuple>
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
    uint64_t topologyRecordCount = 0;
    uint64_t topologyCpuCount = 0;
    uint64_t threadSummaryCount = 0;
    uint64_t threadStringBytes = 0;
    uint64_t threadNameRecordCount = 0;
    uint64_t tidToPidRecordCount = 0;
    uint64_t groupHintRecordCount = 0;
    uint64_t externalNameMetadataRecordCount = 0;
    uint64_t externalNameRecordCount = 0;
    uint64_t externalThreadNameRecordCount = 0;
    uint64_t fiberNameRecordCount = 0;
    uint64_t fiberEnterRecordCount = 0;
    uint64_t fiberLeaveRecordCount = 0;
    uint64_t cpuUsagePointCount = 0;
    uint64_t threadRecordsOffset = 0;
    uint64_t cpuRecordsOffset = 0;
    uint64_t topologyRecordsOffset = 0;
    uint64_t threadSummariesOffset = 0;
    uint64_t cpuUsageRecordsOffset = 0;
    uint64_t threadStringsOffset = 0;
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

struct StoredCpuTopology
{
    uint32_t cpu = 0;
    uint32_t package = 0;
    uint32_t die = 0;
    uint32_t core = 0;
};

enum StoredThreadFlags : uint32_t
{
    StoredThreadFiber = 1u << 0,
    StoredThreadLocalRecord = 1u << 1,
    StoredThreadLocalName = 1u << 2,
    StoredThreadExternalProcessName = 1u << 3,
    StoredThreadExternalThreadName = 1u << 4,
    StoredThreadGroupHint = 1u << 5
};

struct StoredThreadSummary
{
    uint64_t nativeId = 0;
    uint64_t processId = 0;
    uint64_t zoneCount = 0;
    uint64_t messageCount = 0;
    uint64_t sampleCount = 0;
    uint64_t contextSwitchCount = 0;
    int64_t runningTimeNs = 0;
    uint32_t migrations = 0;
    uint32_t runningRegions = 0;
    int32_t groupHint = 0;
    uint32_t flags = 0;
    uint64_t localNameOffset = 0;
    uint64_t externalProcessNameOffset = 0;
    uint64_t externalThreadNameOffset = 0;
    uint32_t localNameBytes = 0;
    uint32_t externalProcessNameBytes = 0;
    uint32_t externalThreadNameBytes = 0;
    uint32_t reserved = 0;
};

struct StoredCpuUsage
{
    int64_t timeNs = 0;
    uint8_t own = 0;
    uint8_t other = 0;
    uint8_t reserved[6] {};
};

struct StoredCpuUsageTransition
{
    int64_t timeNs = 0;
    uint64_t sourceOrdinal = 0;
    int8_t ownDelta = 0;
    int8_t otherDelta = 0;
    uint8_t reserved[6] {};
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

bool GetShortPayload( const TraceSessionCanonicalRecord& record,
    const uint8_t*& data, size_t& size, std::string& error )
{
    const auto fixed = size_t( QueueDataSize[record.type] );
    if( record.payload.size() < fixed + sizeof( uint16_t ) )
    { error = "session_scheduling_payload_truncated"; return false; }
    uint16_t bytes = 0;
    std::memcpy( &bytes, record.payload.data() + fixed, sizeof( bytes ) );
    if( bytes != record.variablePayloadBytes ||
        record.payload.size() != fixed + sizeof( bytes ) + bytes )
    { error = "session_scheduling_payload_mismatch"; return false; }
    data = record.payload.data() + fixed + sizeof( bytes );
    size = bytes;
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

struct ThreadSummaryState
{
    StoredThreadSummary stored;
    std::string localName;
    std::string externalProcessName;
    std::string externalThreadName;
    bool localNameDefined = false;
    bool externalProcessNameDefined = false;
    bool externalThreadNameDefined = false;
    bool haveLastCpu = false;
    uint8_t lastCpu = 0;
};

struct ExternalNameState
{
    uint64_t thread = 0;
    uint64_t processNamePointer = 0;
    uint64_t threadNamePointer = 0;
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
    std::map<uint32_t, StoredCpuTopology> topology;
    std::map<uint64_t, ThreadSummaryState> threadSummaries;
    std::unordered_map<uint64_t, std::string> externalProcessNames;
    std::unordered_map<uint64_t, std::string> externalThreadNames;
    std::vector<ExternalNameState> externalNameMappings;
    std::unordered_map<uint64_t, uint64_t> fiberToThread;
    std::unordered_map<uint32_t, uint64_t> activeFiber;
    std::unordered_map<uint64_t, std::string> fiberNames;
    int64_t contextTime = 0;
    TraceSessionSchedulingStats stats;
};

ThreadSummaryState& EnsureThreadSummary( BuildState& state, uint64_t thread,
    bool localRecord = false )
{
    auto [found, inserted] = state.threadSummaries.try_emplace( thread );
    if( inserted ) found->second.stored.nativeId = thread;
    if( localRecord ) found->second.stored.flags |= StoredThreadLocalRecord;
    return found->second;
}

uint64_t LogicalThread( const BuildState& state, uint32_t nativeThread )
{
    const auto fiber = state.activeFiber.find( nativeThread );
    return fiber == state.activeFiber.end() ? nativeThread : fiber->second;
}

ThreadRuntime* EnsureThread( BuildState& state, uint64_t thread, std::string& error )
{
    EnsureThreadSummary( state, thread );
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
    ++EnsureThreadSummary( state, value.thread ).stored.contextSwitchCount;
    return true;
}

bool PatchThreadEvent( BuildState& state, ThreadRuntime& runtime,
    const StoredThreadEvent& value, std::string& error )
{
    if( !runtime.hasLast || !state.threadWork.Patch( runtime.lastIndex, value, error ) ) return false;
    if( runtime.last.endNs < 0 && value.endNs >= value.startNs )
    {
        auto& summary = EnsureThreadSummary( state, value.thread ).stored;
        summary.runningTimeNs += value.endNs - value.startNs;
        ++summary.runningRegions;
    }
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
    auto& threadSummary = EnsureThreadSummary( state, event.newThread );
    if( threadSummary.haveLastCpu && threadSummary.lastCpu != event.cpu )
        ++threadSummary.stored.migrations;
    threadSummary.lastCpu = event.cpu;
    threadSummary.haveLastCpu = true;

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

bool IsCpuZoneBegin( QueueType type )
{
    switch( type )
    {
    case QueueType::ZoneBegin:
    case QueueType::ZoneBeginCallstack:
    case QueueType::ZoneBeginAllocSrcLoc:
    case QueueType::ZoneBeginAllocSrcLocCallstack:
    case QueueType::JnZoneBeginCallsite:
        return true;
    default: return false;
    }
}

bool IsThreadMessage( QueueType type )
{
    switch( type )
    {
    case QueueType::Message:
    case QueueType::MessageColor:
    case QueueType::MessageCallstack:
    case QueueType::MessageColorCallstack:
    case QueueType::MessageLiteral:
    case QueueType::MessageLiteralColor:
    case QueueType::MessageLiteralCallstack:
    case QueueType::MessageLiteralColorCallstack:
        return true;
    default: return false;
    }
}

bool CollectThreadFact( BuildState& state,
    const TraceSessionCanonicalRecord& record, const QueueItem& item,
    QueueType type, std::string& error )
{
    if( record.threadContext != 0 )
        EnsureThreadSummary( state, record.threadContext, true );
    if( IsCpuZoneBegin( type ) )
        ++EnsureThreadSummary( state,
            LogicalThread( state, record.threadContext ), true ).stored.zoneCount;
    if( IsThreadMessage( type ) )
        ++EnsureThreadSummary( state,
            LogicalThread( state, record.threadContext ), true ).stored.messageCount;
    if( type == QueueType::CallstackSample || type == QueueType::CallstackSampleRef )
    {
        const auto thread = type == QueueType::CallstackSample ?
            item.callstackSample.thread : item.callstackSampleRef.thread;
        ++EnsureThreadSummary( state, thread, true ).stored.sampleCount;
    }

    switch( type )
    {
    case QueueType::ThreadName:
    case QueueType::FiberName:
    case QueueType::ExternalName:
    case QueueType::ExternalThreadName:
    {
        const uint8_t* data = nullptr; size_t size = 0;
        if( !GetShortPayload( record, data, size, error ) ) return false;
        std::string text( reinterpret_cast<const char*>( data ), size );
        if( type == QueueType::ThreadName )
        {
            ++state.stats.threadNameRecords;
            auto& summary = EnsureThreadSummary( state, item.stringTransfer.ptr, true );
            summary.localName = std::move( text ); summary.localNameDefined = true;
        }
        else if( type == QueueType::FiberName )
        {
            ++state.stats.fiberNameRecords;
            state.fiberNames[item.stringTransfer.ptr] = text;
            const auto fiber = state.fiberToThread.find( item.stringTransfer.ptr );
            if( fiber != state.fiberToThread.end() )
            {
                auto& summary = EnsureThreadSummary( state, fiber->second, true );
                summary.localName = std::move( text ); summary.localNameDefined = true;
            }
        }
        else if( type == QueueType::ExternalName )
        {
            ++state.stats.externalNameRecords;
            state.externalProcessNames[item.stringTransfer.ptr] = std::move( text );
        }
        else
        {
            ++state.stats.externalThreadNameRecords;
            state.externalThreadNames[item.stringTransfer.ptr] = std::move( text );
        }
        break;
    }
    case QueueType::ExternalNameMetadata:
        ++state.stats.externalNameMetadataRecords;
        state.externalNameMappings.push_back( { item.externalNameMetadata.thread,
            item.externalNameMetadata.name, item.externalNameMetadata.threadName } );
        EnsureThreadSummary( state, item.externalNameMetadata.thread );
        break;
    case QueueType::TidToPid:
        ++state.stats.tidToPidRecords;
        EnsureThreadSummary( state, item.tidToPid.tid ).stored.processId = item.tidToPid.pid;
        break;
    case QueueType::ThreadGroupHint:
    {
        ++state.stats.groupHintRecords;
        auto& summary = EnsureThreadSummary( state, item.threadGroupHint.thread, true );
        summary.stored.groupHint = item.threadGroupHint.groupHint;
        summary.stored.flags |= StoredThreadGroupHint;
        break;
    }
    case QueueType::FiberEnter:
    {
        ++state.stats.fiberEnterRecords;
        auto fiber = state.fiberToThread.find( item.fiberEnter.fiber );
        if( fiber == state.fiberToThread.end() )
        {
            const auto id = ( uint64_t( 1 ) << 32 ) | state.fiberToThread.size();
            fiber = state.fiberToThread.emplace( item.fiberEnter.fiber, id ).first;
            auto& summary = EnsureThreadSummary( state, id, true );
            summary.stored.flags |= StoredThreadFiber;
            summary.stored.groupHint = item.fiberEnter.groupHint;
            summary.stored.flags |= StoredThreadGroupHint;
            const auto name = state.fiberNames.find( item.fiberEnter.fiber );
            if( name != state.fiberNames.end() )
            { summary.localName = name->second; summary.localNameDefined = true; }
        }
        EnsureThreadSummary( state, item.fiberEnter.thread, true );
        state.activeFiber[item.fiberEnter.thread] = fiber->second;
        break;
    }
    case QueueType::FiberLeave:
        ++state.stats.fiberLeaveRecords;
        EnsureThreadSummary( state, item.fiberLeave.thread, true );
        state.activeFiber.erase( item.fiberLeave.thread );
        break;
    default: break;
    }
    return true;
}

bool VisitSchedulingRecord( const TraceSessionCanonicalRecord& record,
    void* userData, std::string& error )
{
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ) return true;
    auto& state = *static_cast<BuildState*>( userData );
    QueueItem item {};
    if( !DecodeItem( record, item, error ) ) return false;
    const auto type = QueueType( record.type );
    if( !CollectThreadFact( state, record, item, type, error ) ) return false;
    switch( type )
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
    case QueueType::CpuTopology:
    {
        ++state.stats.topologyRecords;
        const StoredCpuTopology value { item.cpuTopology.thread,
            item.cpuTopology.package, item.cpuTopology.die, item.cpuTopology.core };
        if( !state.topology.emplace( value.cpu, value ).second )
        { error = "session_scheduling_cpu_topology_duplicate"; return false; }
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
    out << "topology_records " << value.stats.topologyRecords << '\n';
    out << "topology_cpus " << value.stats.topologyCpus << '\n';
    out << "thread_summaries " << value.stats.threadSummaries << '\n';
    out << "thread_name_records " << value.stats.threadNameRecords << '\n';
    out << "tid_to_pid_records " << value.stats.tidToPidRecords << '\n';
    out << "group_hint_records " << value.stats.groupHintRecords << '\n';
    out << "external_name_metadata_records " << value.stats.externalNameMetadataRecords << '\n';
    out << "external_name_records " << value.stats.externalNameRecords << '\n';
    out << "external_thread_name_records " << value.stats.externalThreadNameRecords << '\n';
    out << "fiber_name_records " << value.stats.fiberNameRecords << '\n';
    out << "fiber_enter_records " << value.stats.fiberEnterRecords << '\n';
    out << "fiber_leave_records " << value.stats.fiberLeaveRecords << '\n';
    out << "cpu_usage_points " << value.stats.cpuUsagePoints << '\n';
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
        else if( key == "topology_records" ) in >> value.stats.topologyRecords;
        else if( key == "topology_cpus" ) in >> value.stats.topologyCpus;
        else if( key == "thread_summaries" ) in >> value.stats.threadSummaries;
        else if( key == "thread_name_records" ) in >> value.stats.threadNameRecords;
        else if( key == "tid_to_pid_records" ) in >> value.stats.tidToPidRecords;
        else if( key == "group_hint_records" ) in >> value.stats.groupHintRecords;
        else if( key == "external_name_metadata_records" ) in >> value.stats.externalNameMetadataRecords;
        else if( key == "external_name_records" ) in >> value.stats.externalNameRecords;
        else if( key == "external_thread_name_records" ) in >> value.stats.externalThreadNameRecords;
        else if( key == "fiber_name_records" ) in >> value.stats.fiberNameRecords;
        else if( key == "fiber_enter_records" ) in >> value.stats.fiberEnterRecords;
        else if( key == "fiber_leave_records" ) in >> value.stats.fiberLeaveRecords;
        else if( key == "cpu_usage_points" ) in >> value.stats.cpuUsagePoints;
        else { std::string ignored; std::getline( in, ignored ); }
        if( !in ) { error = "session_scheduling_manifest_parse_failed"; return false; }
    }
    value.stats.fileBytes = value.fileBytes;
    if( magic != SchedulingManifestMagic || schema != TraceSessionSchedulingIndexSchemaVersion ||
        value.sourceSha256.size() != 64 || value.fileSha256.size() != 64 ||
        value.stats.completeThreadEvents > value.stats.threadEvents ||
        value.stats.completeCpuEvents > value.stats.cpuEvents ||
        value.stats.topologyRecords != value.stats.topologyCpus )
    { error = "session_scheduling_manifest_invalid"; return false; }
    return true;
}

bool CpuUsageTransitionLess( const StoredCpuUsageTransition& lhs,
    const StoredCpuUsageTransition& rhs )
{
    return std::tie( lhs.timeNs, lhs.sourceOrdinal ) <
        std::tie( rhs.timeNs, rhs.sourceOrdinal );
}

bool BuildCpuUsageFile( BuildState& state, std::filesystem::path& outputPath,
    uint64_t& pointCount, std::string& error )
{
    outputPath = state.root / "cpu-usage.work";
    pointCount = 0;
    std::ofstream empty( outputPath, std::ios::binary | std::ios::trunc );
    if( !empty ) { error = "session_scheduling_cpu_usage_open_failed"; return false; }
    empty.close();
    if( state.stats.cpuEvents == 0 ) return true;

    const auto transitionPath = state.root / "cpu-usage-transitions.work";
    std::ofstream transitions( transitionPath, std::ios::binary | std::ios::trunc );
    std::ifstream cpuEvents( state.cpuWork.Path(), std::ios::binary );
    if( !transitions || !cpuEvents )
    { error = "session_scheduling_cpu_usage_source_open_failed"; return false; }
    uint64_t transitionOrdinal = 0;
    uint64_t transitionCount = 0;
    for( uint64_t index = 0; index < state.stats.cpuEvents; ++index )
    {
        StoredCpuEvent event;
        if( !cpuEvents.read( reinterpret_cast<char*>( &event ), sizeof( event ) ) )
        { error = "session_scheduling_cpu_usage_source_truncated"; return false; }
        const auto thread = state.threadSummaries.find( event.thread );
        const auto isLocal = thread != state.threadSummaries.end() &&
            ( thread->second.stored.zoneCount != 0 || thread->second.stored.sampleCount != 0 );
        const auto sameProcess = thread != state.threadSummaries.end() &&
            state.transform.processId != 0 &&
            thread->second.stored.processId == state.transform.processId;
        const auto own = isLocal || sameProcess;
        StoredCpuUsageTransition begin;
        begin.timeNs = event.startNs; begin.sourceOrdinal = transitionOrdinal++;
        begin.ownDelta = own ? 1 : 0; begin.otherDelta = own ? 0 : 1;
        transitions.write( reinterpret_cast<const char*>( &begin ), sizeof( begin ) );
        ++transitionCount;
        if( event.endNs >= event.startNs )
        {
            auto end = begin; end.timeNs = event.endNs; end.sourceOrdinal = transitionOrdinal++;
            end.ownDelta = -begin.ownDelta; end.otherDelta = -begin.otherDelta;
            transitions.write( reinterpret_cast<const char*>( &end ), sizeof( end ) );
            ++transitionCount;
        }
    }
    transitions.flush();
    if( !transitions )
    { error = "session_scheduling_cpu_usage_transition_write_failed"; return false; }
    transitions.close(); cpuEvents.close();

    constexpr size_t SortChunkRecords = 1024 * 1024;
    std::ifstream transitionInput( transitionPath, std::ios::binary );
    std::vector<StoredCpuUsageTransition> chunk( SortChunkRecords );
    std::vector<std::filesystem::path> runs;
    uint64_t read = 0;
    while( read < transitionCount )
    {
        const auto wanted = size_t( std::min<uint64_t>(
            SortChunkRecords, transitionCount - read ) );
        transitionInput.read( reinterpret_cast<char*>( chunk.data() ),
            std::streamsize( wanted * sizeof( StoredCpuUsageTransition ) ) );
        if( size_t( transitionInput.gcount() ) !=
            wanted * sizeof( StoredCpuUsageTransition ) )
        { error = "session_scheduling_cpu_usage_transition_truncated"; return false; }
        std::sort( chunk.begin(), chunk.begin() + wanted, CpuUsageTransitionLess );
        const auto path = state.root /
            ( "cpu-usage-run-" + std::to_string( runs.size() ) + ".work" );
        std::ofstream run( path, std::ios::binary | std::ios::trunc );
        run.write( reinterpret_cast<const char*>( chunk.data() ),
            std::streamsize( wanted * sizeof( StoredCpuUsageTransition ) ) );
        run.flush();
        if( !run ) { error = "session_scheduling_cpu_usage_run_write_failed"; return false; }
        runs.emplace_back( path ); read += wanted;
    }
    transitionInput.close();

    struct RunState { std::ifstream stream; StoredCpuUsageTransition value; };
    struct HeapEntry { StoredCpuUsageTransition value; size_t run = 0; };
    const auto later = []( const HeapEntry& lhs, const HeapEntry& rhs ) {
        return CpuUsageTransitionLess( rhs.value, lhs.value );
    };
    std::vector<RunState> readers( runs.size() );
    std::priority_queue<HeapEntry, std::vector<HeapEntry>, decltype( later )> heap( later );
    for( size_t index = 0; index < runs.size(); ++index )
    {
        readers[index].stream.open( runs[index], std::ios::binary );
        if( !readers[index].stream || !readers[index].stream.read(
            reinterpret_cast<char*>( &readers[index].value ), sizeof( StoredCpuUsageTransition ) ) )
        { error = "session_scheduling_cpu_usage_run_read_failed"; return false; }
        heap.push( { readers[index].value, index } );
    }
    std::ofstream output( outputPath, std::ios::binary | std::ios::trunc );
    StoredCpuUsage current;
    output.write( reinterpret_cast<const char*>( &current ), sizeof( current ) );
    pointCount = 1;
    int32_t own = 0, other = 0;
    while( !heap.empty() )
    {
        const auto time = heap.top().value.timeNs;
        int32_t ownDelta = 0, otherDelta = 0;
        while( !heap.empty() && heap.top().value.timeNs == time )
        {
            const auto entry = heap.top(); heap.pop();
            ownDelta += entry.value.ownDelta; otherDelta += entry.value.otherDelta;
            auto& reader = readers[entry.run];
            if( reader.stream.read( reinterpret_cast<char*>( &reader.value ),
                sizeof( StoredCpuUsageTransition ) ) )
                heap.push( { reader.value, entry.run } );
            else if( !reader.stream.eof() )
            { error = "session_scheduling_cpu_usage_run_read_failed"; return false; }
        }
        own += ownDelta; other += otherDelta;
        if( own < 0 || other < 0 || own > 255 || other > 255 )
        { error = "session_scheduling_cpu_usage_count_invalid"; return false; }
        if( current.own == own && current.other == other ) continue;
        current.timeNs = time; current.own = uint8_t( own ); current.other = uint8_t( other );
        output.write( reinterpret_cast<const char*>( &current ), sizeof( current ) );
        ++pointCount;
    }
    output.flush();
    if( !output ) { error = "session_scheduling_cpu_usage_write_failed"; return false; }
    output.close();
    for( auto& reader : readers ) reader.stream.close();
    std::error_code ec;
    std::filesystem::remove( transitionPath, ec ); ec.clear();
    for( const auto& run : runs ) { std::filesystem::remove( run, ec ); ec.clear(); }
    return true;
}

bool FinalizeSchedulingFile( BuildState& state, const TraceSessionManifest& session,
    SchedulingManifest& manifest, std::string& error )
{
    if( !state.threadWork.Close( error ) || !state.cpuWork.Close( error ) ) return false;
    for( const auto& mapping : state.externalNameMappings )
    {
        auto& summary = EnsureThreadSummary( state, mapping.thread );
        const auto processName = state.externalProcessNames.find( mapping.processNamePointer );
        const auto threadName = state.externalThreadNames.find( mapping.threadNamePointer );
        if( processName == state.externalProcessNames.end() ||
            threadName == state.externalThreadNames.end() )
        { error = "session_scheduling_external_name_unresolved"; return false; }
        if( ( !summary.externalProcessName.empty() &&
                summary.externalProcessName != processName->second ) ||
            ( !summary.externalThreadName.empty() &&
                summary.externalThreadName != threadName->second ) )
        { error = "session_scheduling_external_name_conflict"; return false; }
        summary.externalProcessName = processName->second;
        summary.externalThreadName = threadName->second;
        summary.externalProcessNameDefined = true;
        summary.externalThreadNameDefined = true;
    }
    std::string threadStrings;
    std::vector<StoredThreadSummary> threadSummaries;
    threadSummaries.reserve( state.threadSummaries.size() );
    const auto appendString = [&]( const std::string& text, bool defined,
        uint64_t& offset, uint32_t& bytes, uint32_t flag,
        StoredThreadSummary& stored ) -> bool {
        if( !defined ) return true;
        stored.flags |= flag;
        if( text.empty() ) return true;
        if( text.size() > std::numeric_limits<uint32_t>::max() ) return false;
        offset = threadStrings.size(); bytes = uint32_t( text.size() );
        threadStrings.append( text ); return true;
    };
    for( auto& [_, value] : state.threadSummaries )
    {
        if( value.stored.flags & StoredThreadLocalRecord )
            value.stored.flags |= StoredThreadGroupHint;
        if( !appendString( value.localName, value.localNameDefined,
                value.stored.localNameOffset,
                value.stored.localNameBytes, StoredThreadLocalName, value.stored ) ||
            !appendString( value.externalProcessName, value.externalProcessNameDefined,
                value.stored.externalProcessNameOffset,
                value.stored.externalProcessNameBytes,
                StoredThreadExternalProcessName, value.stored ) ||
            !appendString( value.externalThreadName, value.externalThreadNameDefined,
                value.stored.externalThreadNameOffset,
                value.stored.externalThreadNameBytes,
                StoredThreadExternalThreadName, value.stored ) )
        { error = "session_scheduling_thread_name_too_large"; return false; }
        threadSummaries.emplace_back( value.stored );
    }
    std::filesystem::path cpuUsagePath;
    uint64_t cpuUsagePointCount = 0;
    if( !BuildCpuUsageFile( state, cpuUsagePath, cpuUsagePointCount, error ) ) return false;
    state.stats.cpuUsagePoints = cpuUsagePointCount;
    SchedulingFileHeader header;
    header.sourceSize = session.source.fileSize;
    header.contextSwitchRecords = state.stats.contextSwitchRecords;
    header.wakeupRecords = state.stats.wakeupRecords;
    header.threadEventCount = state.stats.threadEvents;
    header.completeThreadEventCount = state.stats.completeThreadEvents;
    header.cpuEventCount = state.stats.cpuEvents;
    header.completeCpuEventCount = state.stats.completeCpuEvents;
    header.sourceGapEventCount = state.stats.sourceGapEvents;
    state.stats.topologyCpus = state.topology.size();
    header.topologyRecordCount = state.stats.topologyRecords;
    header.topologyCpuCount = state.stats.topologyCpus;
    state.stats.threadSummaries = threadSummaries.size();
    header.threadSummaryCount = state.stats.threadSummaries;
    header.threadStringBytes = threadStrings.size();
    header.threadNameRecordCount = state.stats.threadNameRecords;
    header.tidToPidRecordCount = state.stats.tidToPidRecords;
    header.groupHintRecordCount = state.stats.groupHintRecords;
    header.externalNameMetadataRecordCount = state.stats.externalNameMetadataRecords;
    header.externalNameRecordCount = state.stats.externalNameRecords;
    header.externalThreadNameRecordCount = state.stats.externalThreadNameRecords;
    header.fiberNameRecordCount = state.stats.fiberNameRecords;
    header.fiberEnterRecordCount = state.stats.fiberEnterRecords;
    header.fiberLeaveRecordCount = state.stats.fiberLeaveRecords;
    header.cpuUsagePointCount = state.stats.cpuUsagePoints;
    header.generationBytes = uint32_t( session.generation.size() );
    header.threadRecordsOffset = sizeof( header ) + session.source.sha256.size() + session.generation.size();
    header.cpuRecordsOffset = header.threadRecordsOffset + header.threadEventCount * sizeof( StoredThreadEvent );
    header.topologyRecordsOffset = header.cpuRecordsOffset +
        header.cpuEventCount * sizeof( StoredCpuEvent );
    header.threadSummariesOffset = header.topologyRecordsOffset +
        header.topologyCpuCount * sizeof( StoredCpuTopology );
    header.cpuUsageRecordsOffset = header.threadSummariesOffset +
        header.threadSummaryCount * sizeof( StoredThreadSummary );
    header.threadStringsOffset = header.cpuUsageRecordsOffset +
        header.cpuUsagePointCount * sizeof( StoredCpuUsage );
    const auto temporary = state.root / "scheduling.bin.tmp";
    const auto target = state.root / SchedulingFileName;
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_scheduling_file_open_failed"; return false; }
    out.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
    out.write( session.source.sha256.data(), std::streamsize( session.source.sha256.size() ) );
    out.write( session.generation.data(), std::streamsize( session.generation.size() ) );
    if( !CopyFileBytes( state.threadWork.Path(), out, error ) ||
        !CopyFileBytes( state.cpuWork.Path(), out, error ) ) return false;
    for( const auto& [_, topology] : state.topology )
        out.write( reinterpret_cast<const char*>( &topology ), sizeof( topology ) );
    if( !threadSummaries.empty() ) out.write(
        reinterpret_cast<const char*>( threadSummaries.data() ),
        std::streamsize( threadSummaries.size() * sizeof( StoredThreadSummary ) ) );
    if( !CopyFileBytes( cpuUsagePath, out, error ) ) return false;
    if( !threadStrings.empty() ) out.write(
        threadStrings.data(), std::streamsize( threadStrings.size() ) );
    out.flush();
    if( !out ) { error = "session_scheduling_file_write_failed"; return false; }
    out.close();
    if( !AtomicReplace( temporary, target, error ) ) return false;
    std::error_code ec;
    const std::array<std::filesystem::path, 3> workFiles = {
        state.threadWork.Path(), state.cpuWork.Path(), cpuUsagePath };
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
    uint64_t cpuUsageRecordsOffset = 0;
    uint64_t cpuUsagePointCount = 0;
    std::vector<CpuTopologyDto> topology;
    std::vector<ThreadDto> threads;
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
        header.topologyRecordCount != manifest.stats.topologyRecords ||
        header.topologyCpuCount != manifest.stats.topologyCpus ||
        header.threadSummaryCount != manifest.stats.threadSummaries ||
        header.threadNameRecordCount != manifest.stats.threadNameRecords ||
        header.tidToPidRecordCount != manifest.stats.tidToPidRecords ||
        header.groupHintRecordCount != manifest.stats.groupHintRecords ||
        header.externalNameMetadataRecordCount != manifest.stats.externalNameMetadataRecords ||
        header.externalNameRecordCount != manifest.stats.externalNameRecords ||
        header.externalThreadNameRecordCount != manifest.stats.externalThreadNameRecords ||
        header.fiberNameRecordCount != manifest.stats.fiberNameRecords ||
        header.fiberEnterRecordCount != manifest.stats.fiberEnterRecords ||
        header.fiberLeaveRecordCount != manifest.stats.fiberLeaveRecords ||
        header.cpuUsagePointCount != manifest.stats.cpuUsagePoints ||
        header.generationBytes != session.generation.size() )
    { error = "session_scheduling_file_header_invalid"; return {}; }
    std::string sha( 64, '\0' ), generation( header.generationBytes, '\0' );
    if( !in.read( sha.data(), std::streamsize( sha.size() ) ) ||
        !in.read( generation.data(), std::streamsize( generation.size() ) ) ||
        sha != session.source.sha256 || generation != session.generation ||
        header.threadRecordsOffset != uint64_t( in.tellg() ) ||
        header.cpuRecordsOffset != header.threadRecordsOffset +
            header.threadEventCount * sizeof( StoredThreadEvent ) ||
        header.topologyRecordsOffset != header.cpuRecordsOffset +
            header.cpuEventCount * sizeof( StoredCpuEvent ) ||
        header.threadSummariesOffset != header.topologyRecordsOffset +
            header.topologyCpuCount * sizeof( StoredCpuTopology ) ||
        header.cpuUsageRecordsOffset != header.threadSummariesOffset +
            header.threadSummaryCount * sizeof( StoredThreadSummary ) ||
        header.threadStringsOffset != header.cpuUsageRecordsOffset +
            header.cpuUsagePointCount * sizeof( StoredCpuUsage ) ||
        header.threadStringsOffset + header.threadStringBytes != manifest.fileBytes )
    { error = "session_scheduling_file_identity_or_bounds_invalid"; return {}; }
    auto impl = std::make_shared<Impl>();
    impl->path = path; impl->fingerprint = session.source.sha256;
    impl->threadRecordsOffset = header.threadRecordsOffset;
    impl->threadEventCount = header.threadEventCount;
    impl->cpuRecordsOffset = header.cpuRecordsOffset;
    impl->cpuEventCount = header.cpuEventCount;
    impl->cpuUsageRecordsOffset = header.cpuUsageRecordsOffset;
    impl->cpuUsagePointCount = header.cpuUsagePointCount;
    impl->topology.reserve( size_t( header.topologyCpuCount ) );
    in.seekg( std::streamoff( header.topologyRecordsOffset ) );
    for( uint64_t index = 0; index < header.topologyCpuCount; ++index )
    {
        StoredCpuTopology value;
        if( !in.read( reinterpret_cast<char*>( &value ), sizeof( value ) ) ||
            ( index != 0 && impl->topology.back().cpu >= value.cpu ) )
        { error = "session_scheduling_topology_invalid"; return {}; }
        CpuTopologyDto dto { value.cpu, value.package, value.die, value.core };
        dto.dieAvailability.available = true;
        impl->topology.emplace_back( std::move( dto ) );
    }
    std::vector<StoredThreadSummary> storedThreads(
        size_t( header.threadSummaryCount ) );
    in.seekg( std::streamoff( header.threadSummariesOffset ) );
    if( header.threadSummaryCount != 0 && !in.read(
        reinterpret_cast<char*>( storedThreads.data() ),
        std::streamsize( header.threadSummaryCount * sizeof( StoredThreadSummary ) ) ) )
    { error = "session_scheduling_thread_summaries_truncated"; return {}; }
    const auto readThreadString = [&]( uint64_t offset, uint32_t bytes,
        std::string& value ) -> bool {
        if( bytes == 0 ) { value.clear(); return true; }
        if( offset > header.threadStringBytes ||
            bytes > header.threadStringBytes - offset ) return false;
        value.resize( bytes );
        in.clear();
        in.seekg( std::streamoff( header.threadStringsOffset + offset ) );
        return bool( in.read( value.data(), std::streamsize( bytes ) ) );
    };
    impl->threads.reserve( storedThreads.size() );
    for( size_t index = 0; index < storedThreads.size(); ++index )
    {
        const auto& stored = storedThreads[index];
        if( stored.nativeId == 0 ||
            ( index != 0 && storedThreads[index-1].nativeId >= stored.nativeId ) )
        { error = "session_scheduling_thread_summary_order_invalid"; return {}; }
        std::string localName, externalProcessName, externalThreadName;
        if( !readThreadString( stored.localNameOffset, stored.localNameBytes, localName ) ||
            !readThreadString( stored.externalProcessNameOffset,
                stored.externalProcessNameBytes, externalProcessName ) ||
            !readThreadString( stored.externalThreadNameOffset,
                stored.externalThreadNameBytes, externalThreadName ) )
        { error = "session_scheduling_thread_string_bounds_invalid"; return {}; }
        ThreadDto dto;
        dto.ref = MakeRef( session.source.sha256, "thread", stored.nativeId );
        dto.nativeId = stored.nativeId;
        dto.processId = stored.processId;
        dto.fiber = ( stored.flags & StoredThreadFiber ) != 0;
        dto.zoneCount = stored.zoneCount;
        dto.messageCount = stored.messageCount;
        dto.sampleCount = stored.sampleCount;
        dto.contextSwitchCount = stored.contextSwitchCount;
        dto.runningTimeNs = stored.runningTimeNs;
        dto.migrations = stored.migrations;
        dto.runningRegions = stored.runningRegions;
        if( stored.flags & StoredThreadLocalName ) dto.localName = localName;
        if( stored.flags & StoredThreadExternalProcessName )
            dto.externalProcessName = externalProcessName;
        if( stored.flags & StoredThreadExternalThreadName )
            dto.externalThreadName = externalThreadName;
        dto.name = localName.empty() ?
            ( externalThreadName.empty() ? "???" : externalThreadName ) : localName;
        if( localName == std::to_string( stored.nativeId ) &&
            !externalThreadName.empty() && externalThreadName != "???" &&
            externalThreadName != "ntdll.dll" ) dto.name = externalThreadName;
        dto.groupHintAvailability.available =
            ( stored.flags & StoredThreadLocalRecord ) != 0;
        if( dto.groupHintAvailability.available ) dto.groupHint = stored.groupHint;
        else dto.groupHintAvailability.reason =
            "this native thread has no persisted Tracy thread record";
        impl->threads.emplace_back( std::move( dto ) );
    }
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

const std::vector<CpuTopologyDto>& TraceSessionSchedulingReader::CpuTopology() const
{
    return m_impl->topology;
}

const std::vector<ThreadDto>& TraceSessionSchedulingReader::Threads() const
{
    return m_impl->threads;
}

std::vector<CpuUsagePointDto> TraceSessionSchedulingReader::ScanCpuUsage(
    size_t offset, size_t limit ) const
{
    std::vector<CpuUsagePointDto> result;
    if( limit == 0 || offset >= m_impl->cpuUsagePointCount ) return result;
    const auto count = std::min<uint64_t>( limit,
        m_impl->cpuUsagePointCount - uint64_t( offset ) );
    std::ifstream in( m_impl->path, std::ios::binary );
    if( !in ) return result;
    in.seekg( std::streamoff( m_impl->cpuUsageRecordsOffset +
        uint64_t( offset ) * sizeof( StoredCpuUsage ) ) );
    result.reserve( size_t( count ) );
    for( uint64_t index = 0; index < count; ++index )
    {
        StoredCpuUsage value;
        if( !in.read( reinterpret_cast<char*>( &value ), sizeof( value ) ) ) break;
        CpuUsagePointDto dto;
        dto.ref = MakeRef( m_impl->fingerprint, "cpu-usage", uint64_t( offset ) + index );
        dto.timeNs = value.timeNs;
        dto.own = value.own;
        dto.other = value.other;
        result.emplace_back( std::move( dto ) );
    }
    return result;
}

}

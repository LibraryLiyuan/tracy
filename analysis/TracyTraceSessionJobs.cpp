#include "TracyTraceSessionJobs.hpp"

#include "TracyHash.hpp"
#include "TracyTraceSessionCanonical.hpp"
#include "TracyTraceSessionGpuCanonical.hpp"
#include "TracyProtocol.hpp"
#include "TracyQueue.hpp"
#include "../server/TracyJnData.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
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

constexpr uint64_t JobFileMagic = 0x31424f4a534e4aull;
constexpr uint64_t JobManifestMagic = 0x31464d4a534e4aull;
constexpr const char* JobFileName = "jobs.bin";

#pragma pack( push, 1 )
struct JobFileHeader
{
    uint64_t magic = JobFileMagic;
    uint32_t schema = TraceSessionJobIndexSchemaVersion;
    uint32_t reserved = 0;
    uint64_t sourceSize = 0;
    uint64_t typeCount = 0;
    uint64_t scheduleCount = 0;
    uint64_t configCount = 0;
    uint64_t dependencyCount = 0;
    uint64_t stageCount = 0;
    uint64_t frameCount = 0;
    uint64_t callsiteCount = 0;
    uint32_t generationBytes = 0;
    uint32_t endian = 0x01020304;
};

struct StoredJobType
{
    uint32_t typeId = 0;
    uint32_t nameBytes = 0;
    uint8_t kind = 0;
    uint8_t flags = 0;
    uint16_t reserved = 0;
};

struct StoredCallsite
{
    uint32_t callsiteId = 0;
    uint32_t callstack = 0;
    uint8_t provenance = uint8_t( JnStackProvenance::Unavailable );
    uint8_t flags = 0;
    uint8_t unavailableReason = 0;
    uint8_t reserved = 0;
};
#pragma pack( pop )

struct JobManifest
{
    std::string sourceSha256;
    uint64_t sourceSize = 0;
    std::string generation;
    uint64_t fileBytes = 0;
    std::string fileSha256;
    TraceSessionJobStats stats;
};

struct RawJobStore
{
    std::vector<std::pair<JnJobTypeData, std::string>> types;
    std::vector<JnJobScheduleData> schedules;
    std::vector<JnJobConfigData> configs;
    std::vector<JnJobDependencyData> dependencies;
    std::vector<JnJobStageData> stages;
    std::vector<JnFrameData> frames;
    std::vector<StoredCallsite> callsites;
};

bool AtomicReplace( const std::filesystem::path& source,
    const std::filesystem::path& target, std::string& error )
{
#ifdef _WIN32
    if( MoveFileExW( source.c_str(), target.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) ) return true;
    error = "session_job_atomic_replace_failed:" + std::to_string( GetLastError() );
    return false;
#else
    std::error_code ec;
    std::filesystem::rename( source, target, ec );
    if( !ec ) return true;
    error = "session_job_atomic_replace_failed:" + ec.message();
    return false;
#endif
}

bool SaveJobManifest( const std::filesystem::path& root,
    const JobManifest& value, std::string& error )
{
    const auto temporary = root / "manifest.tmp";
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_job_manifest_open_failed"; return false; }
    out << "magic " << JobManifestMagic << '\n';
    out << "schema " << TraceSessionJobIndexSchemaVersion << '\n';
    out << "source_sha256 " << std::quoted( value.sourceSha256 ) << '\n';
    out << "source_size " << value.sourceSize << '\n';
    out << "generation " << std::quoted( value.generation ) << '\n';
    out << "file_bytes " << value.fileBytes << '\n';
    out << "file_sha256 " << std::quoted( value.fileSha256 ) << '\n';
    out << "job_types " << value.stats.jobTypes << '\n';
    out << "jobs " << value.stats.jobs << '\n';
    out << "schedules " << value.stats.schedules << '\n';
    out << "configs " << value.stats.configs << '\n';
    out << "dependencies " << value.stats.dependencies << '\n';
    out << "stages " << value.stats.stages << '\n';
    out.flush();
    if( !out ) { error = "session_job_manifest_write_failed"; return false; }
    out.close();
    return AtomicReplace( temporary, root / "manifest", error );
}

bool LoadJobManifest( const std::filesystem::path& root,
    JobManifest& value, std::string& error )
{
    value = {};
    std::ifstream in( root / "manifest", std::ios::binary );
    if( !in ) { error = "session_job_manifest_not_found"; return false; }
    uint64_t magic = 0;
    uint32_t schema = 0;
    std::string key;
    while( in >> key )
    {
        if( key == "magic" ) in >> magic;
        else if( key == "schema" ) in >> schema;
        else if( key == "source_sha256" ) in >> std::quoted( value.sourceSha256 );
        else if( key == "source_size" ) in >> value.sourceSize;
        else if( key == "generation" ) in >> std::quoted( value.generation );
        else if( key == "file_bytes" ) in >> value.fileBytes;
        else if( key == "file_sha256" ) in >> std::quoted( value.fileSha256 );
        else if( key == "job_types" ) in >> value.stats.jobTypes;
        else if( key == "jobs" ) in >> value.stats.jobs;
        else if( key == "schedules" ) in >> value.stats.schedules;
        else if( key == "configs" ) in >> value.stats.configs;
        else if( key == "dependencies" ) in >> value.stats.dependencies;
        else if( key == "stages" ) in >> value.stats.stages;
        else { std::string ignored; std::getline( in, ignored ); }
        if( !in ) { error = "session_job_manifest_parse_failed"; return false; }
    }
    value.stats.fileBytes = value.fileBytes;
    if( magic != JobManifestMagic || schema != TraceSessionJobIndexSchemaVersion ||
        value.sourceSha256.size() != 64 || value.fileSha256.size() != 64 )
    { error = "session_job_manifest_invalid"; return false; }
    return true;
}

bool DecodeItem( const TraceSessionCanonicalRecord& record, QueueItem& item,
    std::string& error )
{
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ||
        record.type >= uint8_t( QueueType::NUM_TYPES ) ||
        record.payload.size() < QueueDataSize[record.type] )
    { error = "session_job_protocol_record_invalid"; return false; }
    item = {};
    std::memcpy( &item, record.payload.data(),
        std::min<size_t>( record.payload.size(), sizeof( item ) ) );
    if( item.hdr.idx != record.type )
    { error = "session_job_protocol_type_mismatch"; return false; }
    return true;
}

bool GetShortPayload( const TraceSessionCanonicalRecord& record,
    const uint8_t*& data, size_t& size, std::string& error )
{
    const auto fixed = size_t( QueueDataSize[record.type] );
    if( record.payload.size() < fixed + sizeof( uint16_t ) )
    { error = "session_job_variable_payload_truncated"; return false; }
    uint16_t encoded = 0;
    std::memcpy( &encoded, record.payload.data() + fixed, sizeof( encoded ) );
    if( encoded != record.variablePayloadBytes ||
        record.payload.size() != fixed + sizeof( encoded ) + encoded )
    { error = "session_job_variable_payload_mismatch"; return false; }
    data = record.payload.data() + fixed + sizeof( encoded );
    size = encoded;
    return true;
}

struct BuildState
{
    RawJobStore raw;
    TraceSessionTimeTransform transform;
    std::unordered_map<uint64_t, std::string> strings;
    std::vector<JnJobTypeData> unresolvedTypes;
    std::unordered_map<std::string, uint32_t> callstackIds;
    std::unordered_map<uint32_t, StoredCallsite> callsites;
    uint32_t pendingCallstack = 0;
    uint32_t serialNextCallstack = 0;
};

bool AddCount( size_t size, std::string& error )
{
    if( size < JnTraceMaxRecordsPerDomain ) return true;
    error = "session_job_record_limit_exceeded";
    return false;
}

uint32_t InternCallstack( BuildState& state, const uint8_t* data,
    size_t size, std::string& error )
{
    if( size % sizeof( uint64_t ) != 0 )
    { error = "session_job_callstack_payload_invalid"; return 0; }
    std::string key( reinterpret_cast<const char*>( data ), size );
    const auto found = state.callstackIds.find( key );
    if( found != state.callstackIds.end() ) return found->second;
    if( state.callstackIds.size() >= std::numeric_limits<uint32_t>::max() - 1 )
    { error = "session_job_callstack_id_overflow"; return 0; }
    const auto id = uint32_t( state.callstackIds.size() + 1 );
    state.callstackIds.emplace( std::move( key ), id );
    return id;
}

bool VisitJobRecord( const TraceSessionCanonicalRecord& record,
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
        { error = "session_job_string_conflict"; return false; }
        break;
    }
    case QueueType::CallstackPayload:
    {
        const uint8_t* data = nullptr; size_t size = 0;
        if( !GetShortPayload( record, data, size, error ) || state.pendingCallstack != 0 )
        { if( error.empty() ) error = "session_job_callstack_payload_invalid"; return false; }
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
        { error = "session_job_callstack_serial_sequence_invalid"; return false; }
        state.serialNextCallstack = state.pendingCallstack;
        state.pendingCallstack = 0;
        break;
    case QueueType::Callstack:
    case QueueType::CallstackAlloc:
    case QueueType::CallstackSample:
    case QueueType::CallstackSampleContextSwitch:
        if( state.pendingCallstack == 0 )
        { error = "session_job_callstack_sequence_invalid"; return false; }
        state.pendingCallstack = 0;
        break;
    case QueueType::JnCallsiteDefinition:
    {
        const auto& value = item.jnCallsiteDefinition;
        if( value.callsiteId == 0 || state.callsites.contains( value.callsiteId ) )
        { error = "session_job_callsite_duplicate"; return false; }
        StoredCallsite callsite;
        callsite.callsiteId = value.callsiteId;
        callsite.provenance = value.provenance;
        callsite.flags = value.flags;
        callsite.unavailableReason = value.unavailableReason;
        if( ( value.flags & uint8_t( JnCallsiteFlags::HasCallstack ) ) != 0 )
        {
            if( state.serialNextCallstack == 0 )
            { error = "session_job_callsite_callstack_missing"; return false; }
            callsite.callstack = state.serialNextCallstack;
            state.serialNextCallstack = 0;
        }
        else if( state.serialNextCallstack != 0 )
        { error = "session_job_callsite_unexpected_callstack"; return false; }
        state.callsites.emplace( value.callsiteId, callsite );
        break;
    }
    case QueueType::JnJobType:
        if( !AddCount( state.unresolvedTypes.size(), error ) ) return false;
        state.unresolvedTypes.push_back( { item.jnJobType.name, item.jnJobType.typeId,
            item.jnJobType.kind, item.jnJobType.flags } );
        break;
    case QueueType::JnJobSchedule:
        if( !AddCount( state.raw.schedules.size(), error ) ) return false;
        state.raw.schedules.push_back( { state.transform.ToNanoseconds( item.jnJobSchedule.time ),
            item.jnJobSchedule.jobId, item.jnJobSchedule.packedHandle, record.threadContext,
            item.jnJobSchedule.dependencyCount, item.jnJobSchedule.kind, item.jnJobSchedule.flags } );
        break;
    case QueueType::JnJobConfig:
        if( !AddCount( state.raw.configs.size(), error ) ) return false;
        state.raw.configs.push_back( { item.jnJobConfig.jobId, item.jnJobConfig.typeId,
            item.jnJobConfig.count, item.jnJobConfig.grainSize, item.jnJobConfig.unityFlowId,
            item.jnJobConfig.originFrameSequence, item.jnJobConfig.kind, item.jnJobConfig.flags } );
        break;
    case QueueType::JnJobDependency:
        if( !AddCount( state.raw.dependencies.size(), error ) ) return false;
        state.raw.dependencies.push_back( { item.jnJobDependency.jobId,
            item.jnJobDependency.prerequisiteJobId, item.jnJobDependency.prerequisiteHandle,
            item.jnJobDependency.flags } );
        break;
    case QueueType::JnJobStage:
    {
        if( !AddCount( state.raw.stages.size(), error ) ) return false;
        uint32_t spanId = item.jnJobStage.spanId;
        uint64_t thread = record.threadContext;
        const auto stage = JnJobStage( item.jnJobStage.stage );
        if( stage == JnJobStage::ScheduleCallstack || stage == JnJobStage::WaitCallstack )
        {
            if( state.serialNextCallstack == 0 )
            { error = "session_job_stage_callstack_missing"; return false; }
            spanId = state.serialNextCallstack;
            thread = item.jnJobStage.arg0;
            state.serialNextCallstack = 0;
        }
        state.raw.stages.push_back( { state.transform.ToNanoseconds( item.jnJobStage.time ),
            item.jnJobStage.jobId, thread, spanId,
            item.jnJobStage.arg0, item.jnJobStage.arg1, item.jnJobStage.stage,
            item.jnJobStage.flags } );
        break;
    }
    case QueueType::JnIoStage:
        if( JnIoStage( item.jnIoStage.stage ) == JnIoStage::RequestCallstack )
        {
            if( state.serialNextCallstack == 0 )
            { error = "session_job_io_callstack_missing"; return false; }
            state.serialNextCallstack = 0;
        }
        break;
    case QueueType::GpuZoneBeginCallstackSerial:
    case QueueType::GpuZoneBeginAllocSrcLocCallstackSerial:
    case QueueType::MemAllocCallstack:
    case QueueType::MemAllocCallstackNamed:
    case QueueType::MemFreeCallstack:
    case QueueType::MemFreeCallstackNamed:
    case QueueType::MemDiscardCallstack:
        if( state.serialNextCallstack == 0 )
        { error = "session_job_serial_callstack_missing"; return false; }
        state.serialNextCallstack = 0;
        break;
    case QueueType::JnFrame:
        if( !AddCount( state.raw.frames.size(), error ) ) return false;
        state.raw.frames.push_back( { state.transform.ToNanoseconds( item.jnFrame.time ),
            item.jnFrame.frameId, item.jnFrame.domainIndex, record.threadContext,
            item.jnFrame.domain, item.jnFrame.phase, item.jnFrame.flags } );
        break;
    default: break;
    }
    return true;
}

std::string MakeRef( const std::string& fingerprint, const char* kind, uint64_t id )
{
    std::ostringstream out;
    out << "tracy:v1:" << fingerprint.substr( 0, 16 ) << ':' << kind << ':' << std::hex << id;
    return out.str();
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

std::vector<JobDto> BuildJobs( const RawJobStore& raw, const std::string& fingerprint )
{
    std::unordered_map<uint32_t, std::string> typeNames;
    for( const auto& type : raw.types ) typeNames[type.first.typeId] = type.second;
    std::unordered_map<uint32_t, const StoredCallsite*> callsites;
    for( const auto& callsite : raw.callsites ) callsites[callsite.callsiteId] = &callsite;
    std::unordered_map<uint32_t, uint64_t> frameIdsBySequence;
    for( const auto& frame : raw.frames )
        if( frame.phase == uint8_t( JnFramePhase::Begin ) &&
            ( frame.flags & uint8_t( JnFrameFlags::Canonical ) ) != 0 )
            frameIdsBySequence[uint32_t( frame.frameId )] = frame.frameId;

    std::map<uint64_t, JobDto> jobs;
    const auto ensure = [&]( uint64_t id ) -> JobDto& {
        auto [it, inserted] = jobs.try_emplace( id );
        if( inserted ) { it->second.ref = MakeRef( fingerprint, "job", id ); it->second.jobId = id; it->second.orphan = true; }
        return it->second;
    };
    for( const auto& value : raw.schedules )
    {
        auto& job = ensure( value.jobId );
        job.packedHandle = value.packedHandle; job.kind = value.kind; job.flags = value.flags;
        if( value.flags & uint8_t( 1 << 6 ) ) job.jobSchemaVersion = 3;
        job.captureBoundary = ( value.flags & uint8_t( 1 << 7 ) ) != 0;
        job.scheduleNs = value.time; job.scheduleThreadRef = MakeRef( fingerprint, "thread", value.thread );
        job.expectedDependencyCount = value.dependencyCount; job.orphan = job.captureBoundary;
    }
    for( const auto& value : raw.configs )
    {
        auto& job = ensure( value.jobId );
        if( value.typeId != 0 ) job.typeId = value.typeId;
        if( value.count != 0 || job.count == 0 ) job.count = value.count;
        if( value.grainSize != 0 || job.grainSize == 0 ) job.grainSize = value.grainSize;
        if( value.unityFlowId != 0 || job.unityFlowId == 0 ) job.unityFlowId = value.unityFlowId;
        if( value.originFrameSequence != 0 )
        {
            job.originFrameSequence = value.originFrameSequence;
            const auto frame = frameIdsBySequence.find( value.originFrameSequence );
            if( frame != frameIdsBySequence.end() ) job.originFrameId = frame->second;
        }
        job.kind = value.kind; job.flags |= value.flags;
        if( value.flags & uint8_t( 1 << 6 ) ) job.jobSchemaVersion = 3;
        job.captureBoundary = job.captureBoundary || ( value.flags & uint8_t( 1 << 7 ) ) != 0;
        if( job.captureBoundary ) job.orphan = true;
    }
    for( const auto& value : raw.dependencies )
        ensure( value.jobId ).dependencies.push_back( { value.prerequisiteJobId,
            value.prerequisiteHandle, value.flags } );

    using SpanStarts = std::unordered_map<uint32_t, int64_t>;
    std::unordered_map<uint64_t, SpanStarts> sliceStarts, activeHelpStarts, spinStarts, sleepStarts, waitStarts;
    const auto closeSpan = []( auto& starts, uint64_t jobId, uint32_t spanId, int64_t time, int64_t& total ) {
        const auto jobsIt = starts.find( jobId ); if( jobsIt == starts.end() ) return;
        const auto spanIt = jobsIt->second.find( spanId ); if( spanIt == jobsIt->second.end() ) return;
        if( time >= spanIt->second ) total += time - spanIt->second;
        jobsIt->second.erase( spanIt );
    };
    for( const auto& value : raw.stages )
    {
        auto& job = ensure( value.jobId );
        job.stages.push_back( { value.time, MakeRef( fingerprint, "thread", value.thread ),
            value.spanId, value.arg0, value.arg1, value.stage, value.flags } );
        auto& stage = job.stages.back();
        switch( JnJobStage( value.stage ) )
        {
        case JnJobStage::WorkerSliceBegin:
            if( !job.firstRunNs || value.time < *job.firstRunNs ) job.firstRunNs = value.time;
            if( value.flags & uint8_t( 1 << 7 ) ) job.rangeStealSliceCount++;
            sliceStarts[value.jobId][value.spanId] = value.time; break;
        case JnJobStage::WorkerSliceEnd: closeSpan( sliceStarts, value.jobId, value.spanId, value.time, job.executionNs ); break;
        case JnJobStage::Completed: job.completedNs = value.time; break;
        case JnJobStage::WaitBegin: waitStarts[value.jobId][value.spanId] = value.time; break;
        case JnJobStage::WaitEnd: closeSpan( waitStarts, value.jobId, value.spanId, value.time, job.waitNs ); job.waitEndCount++; break;
        case JnJobStage::WaitActiveHelpBegin: activeHelpStarts[value.jobId][value.spanId] = value.time; break;
        case JnJobStage::WaitActiveHelpEnd: closeSpan( activeHelpStarts, value.jobId, value.spanId, value.time, job.waitActiveHelpNs ); break;
        case JnJobStage::WaitSpinYieldBegin: spinStarts[value.jobId][value.spanId] = value.time; break;
        case JnJobStage::WaitSpinYieldEnd: closeSpan( spinStarts, value.jobId, value.spanId, value.time, job.waitSpinYieldNs ); break;
        case JnJobStage::WaitSleepBegin: sleepStarts[value.jobId][value.spanId] = value.time; break;
        case JnJobStage::WaitSleepEnd: closeSpan( sleepStarts, value.jobId, value.spanId, value.time, job.waitSleepNs ); break;
        case JnJobStage::ScheduleCallstack:
            job.scheduleCallstack = value.spanId; job.scheduleStackProvenance = "PerEventExact";
            stage.callstack = value.spanId; stage.stackProvenance = "PerEventExact"; break;
        case JnJobStage::ScheduleCallsite:
        {
            job.jobSchemaVersion = 3; job.scheduleCallsiteId = value.spanId; stage.callsiteId = value.spanId;
            const auto found = callsites.find( value.spanId );
            if( found != callsites.end() )
            {
                job.scheduleCallstack = found->second->callstack;
                job.scheduleStackProvenance = ProvenanceName( found->second->provenance );
                const auto reason = UnavailableReasonName( found->second->unavailableReason );
                if( *reason ) job.scheduleStackUnavailableReason = reason;
                stage.callstack = job.scheduleCallstack; stage.stackProvenance = job.scheduleStackProvenance;
                stage.stackUnavailableReason = job.scheduleStackUnavailableReason;
            }
            break;
        }
        case JnJobStage::Ready:
            job.jobSchemaVersion = std::max<uint16_t>( job.jobSchemaVersion, 2 );
            if( !job.readyNs || value.time < *job.readyNs )
            { job.readyNs = value.time; job.readyLane = value.arg0; job.readyFlags = value.flags; }
            break;
        case JnJobStage::QueueEnter:
            job.jobSchemaVersion = std::max<uint16_t>( job.jobSchemaVersion, 2 );
            if( !job.queueEnterNs || value.time < *job.queueEnterNs )
            { job.queueEnterNs = value.time; job.queueLane = value.arg0; }
            if( value.flags & uint8_t( 1 << 6 ) ) job.queueRetryCount++;
            break;
        case JnJobStage::Dispatch:
            job.jobSchemaVersion = std::max<uint16_t>( job.jobSchemaVersion, 2 ); job.dispatchCount++;
            if( value.flags & uint8_t( 1 << 1 ) ) job.activeHelpDispatchCount++;
            if( std::find( job.executionLanes.begin(), job.executionLanes.end(), value.arg0 ) == job.executionLanes.end() )
                job.executionLanes.push_back( value.arg0 );
            break;
        case JnJobStage::Steal: job.jobSchemaVersion = std::max<uint16_t>( job.jobSchemaVersion, 2 ); job.schedulerStealCount++; break;
        case JnJobStage::WaitCallstack:
            job.jobSchemaVersion = std::max<uint16_t>( job.jobSchemaVersion, 2 ); stage.callstack = value.spanId;
            stage.stackProvenance = "PerEventExact";
            job.waitCallstacks.push_back( { value.time, MakeRef( fingerprint, "thread", value.thread ),
                value.arg1, value.spanId, 0, "PerEventExact", std::nullopt } ); break;
        case JnJobStage::WaitCallsite:
        {
            job.jobSchemaVersion = 3; stage.callsiteId = value.spanId;
            JobWaitCallstackDto wait { value.time, MakeRef( fingerprint, "thread", value.thread ),
                value.arg1, 0, value.spanId, {}, std::nullopt };
            const auto found = callsites.find( value.spanId );
            if( found != callsites.end() )
            {
                wait.callstack = found->second->callstack; wait.stackProvenance = ProvenanceName( found->second->provenance );
                const auto reason = UnavailableReasonName( found->second->unavailableReason );
                if( *reason ) wait.stackUnavailableReason = reason;
                stage.callstack = wait.callstack; stage.stackProvenance = wait.stackProvenance;
                stage.stackUnavailableReason = wait.stackUnavailableReason;
            }
            job.waitCallstacks.emplace_back( std::move( wait ) ); break;
        }
        case JnJobStage::Continuation: job.jobSchemaVersion = 3; job.continuationCount++; break;
        case JnJobStage::Cancelled: job.cancelled = true; break;
        case JnJobStage::Incomplete: job.incomplete = true; break;
        default: break;
        }
    }
    for( auto& [id, job] : jobs )
    {
        if( !job.readyNs || job.dependencies.empty() ) continue;
        std::optional<int64_t> latest; bool complete = true;
        for( const auto& dependency : job.dependencies )
        {
            const auto prerequisite = jobs.find( dependency.prerequisiteJobId );
            if( prerequisite == jobs.end() || !prerequisite->second.completedNs ) { complete = false; break; }
            if( !latest || *prerequisite->second.completedNs > *latest ) latest = prerequisite->second.completedNs;
        }
        if( complete && latest && *job.readyNs >= *latest ) job.dependencyReadyLatencyNs = *job.readyNs - *latest;
    }
    std::vector<JobDto> result;
    result.reserve( jobs.size() );
    for( auto& [id, job] : jobs )
    {
        const auto type = typeNames.find( job.typeId );
        job.name = type == typeNames.end() ? "<unknown job type>" : type->second;
        job.truncated = !job.completedNs && !job.cancelled && !job.incomplete;
        std::sort( job.executionLanes.begin(), job.executionLanes.end() );
        std::sort( job.stages.begin(), job.stages.end(), []( const auto& lhs, const auto& rhs ) { return lhs.timeNs < rhs.timeNs; } );
        result.emplace_back( std::move( job ) );
    }
    return result;
}

template<typename T>
bool WriteVector( std::ofstream& out, const std::vector<T>& values )
{
    if( values.empty() ) return true;
    if( values.size() > uint64_t( std::numeric_limits<std::streamsize>::max() ) / sizeof( T ) ) return false;
    out.write( reinterpret_cast<const char*>( values.data() ),
        std::streamsize( values.size() * sizeof( T ) ) );
    return bool( out );
}

template<typename T>
bool ReadVector( std::ifstream& in, uint64_t count, std::vector<T>& values )
{
    if( count > JnTraceMaxRecordsPerDomain || count > std::numeric_limits<size_t>::max() ||
        count > uint64_t( std::numeric_limits<std::streamsize>::max() ) / sizeof( T ) ) return false;
    values.resize( size_t( count ) );
    if( values.empty() ) return true;
    in.read( reinterpret_cast<char*>( values.data() ), std::streamsize( values.size() * sizeof( T ) ) );
    return bool( in );
}

bool WriteRawStore( const std::filesystem::path& root, const TraceSessionManifest& session,
    const RawJobStore& raw, JobManifest& manifest, std::string& error )
{
    std::error_code ec;
    std::filesystem::create_directories( root, ec );
    if( ec ) { error = "session_job_directory_failed:" + ec.message(); return false; }
    if( session.source.sha256.size() != 64 || session.generation.size() > std::numeric_limits<uint32_t>::max() )
    { error = "session_job_identity_invalid"; return false; }
    JobFileHeader header;
    header.sourceSize = session.source.fileSize;
    header.typeCount = raw.types.size(); header.scheduleCount = raw.schedules.size();
    header.configCount = raw.configs.size(); header.dependencyCount = raw.dependencies.size();
    header.stageCount = raw.stages.size(); header.frameCount = raw.frames.size();
    header.callsiteCount = raw.callsites.size(); header.generationBytes = uint32_t( session.generation.size() );
    auto temporary = root / JobFileName; temporary += ".tmp";
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_job_file_open_failed"; return false; }
    out.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
    out.write( session.source.sha256.data(), 64 );
    out.write( session.generation.data(), std::streamsize( session.generation.size() ) );
    for( const auto& type : raw.types )
    {
        if( type.second.size() > std::numeric_limits<uint32_t>::max() )
        { error = "session_job_type_name_too_large"; return false; }
        StoredJobType stored { type.first.typeId, uint32_t( type.second.size() ),
            type.first.kind, type.first.flags, 0 };
        out.write( reinterpret_cast<const char*>( &stored ), sizeof( stored ) );
        out.write( type.second.data(), std::streamsize( type.second.size() ) );
    }
    if( !WriteVector( out, raw.schedules ) || !WriteVector( out, raw.configs ) ||
        !WriteVector( out, raw.dependencies ) || !WriteVector( out, raw.stages ) ||
        !WriteVector( out, raw.frames ) || !WriteVector( out, raw.callsites ) )
    { error = "session_job_file_write_failed"; return false; }
    out.flush();
    if( !out ) { error = "session_job_file_write_failed"; return false; }
    out.close();
    const auto target = root / JobFileName;
    if( !AtomicReplace( temporary, target, error ) ) return false;
    manifest.sourceSha256 = session.source.sha256; manifest.sourceSize = session.source.fileSize;
    manifest.generation = session.generation; manifest.fileBytes = std::filesystem::file_size( target, ec );
    if( ec ) { error = "session_job_file_size_failed:" + ec.message(); return false; }
    manifest.fileSha256 = Sha256File( target );
    manifest.stats = { uint64_t( raw.types.size() ), 0, uint64_t( raw.schedules.size() ),
        uint64_t( raw.configs.size() ), uint64_t( raw.dependencies.size() ),
        uint64_t( raw.stages.size() ), manifest.fileBytes };
    std::unordered_set<uint64_t> jobs;
    for( const auto& value : raw.schedules ) jobs.emplace( value.jobId );
    for( const auto& value : raw.configs ) jobs.emplace( value.jobId );
    for( const auto& value : raw.dependencies ) jobs.emplace( value.jobId );
    for( const auto& value : raw.stages ) jobs.emplace( value.jobId );
    manifest.stats.jobs = jobs.size();
    return SaveJobManifest( root, manifest, error );
}

bool ReadRawStore( const std::filesystem::path& root, const TraceSessionManifest& session,
    const JobManifest& manifest, RawJobStore& raw, std::string& error )
{
    const auto path = root / JobFileName;
    std::error_code ec;
    const auto bytes = std::filesystem::file_size( path, ec );
    if( ec || bytes != manifest.fileBytes ) { error = "session_job_file_size_mismatch"; return false; }
    if( Sha256File( path ) != manifest.fileSha256 ) { error = "session_job_file_sha256_mismatch"; return false; }
    std::ifstream in( path, std::ios::binary );
    JobFileHeader header;
    if( !in.read( reinterpret_cast<char*>( &header ), sizeof( header ) ) ||
        header.magic != JobFileMagic || header.schema != TraceSessionJobIndexSchemaVersion ||
        header.reserved != 0 || header.endian != 0x01020304 ||
        header.sourceSize != session.source.fileSize || header.generationBytes > 1024 * 1024 )
    { error = "session_job_file_header_invalid"; return false; }
    std::string sourceSha( 64, '\0' ), generation( header.generationBytes, '\0' );
    if( !in.read( sourceSha.data(), 64 ) ||
        ( !generation.empty() && !in.read( generation.data(), std::streamsize( generation.size() ) ) ) ||
        sourceSha != session.source.sha256 || generation != session.generation )
    { error = "session_job_file_identity_invalid"; return false; }
    if( header.typeCount > JnTraceMaxRecordsPerDomain ||
        header.typeCount > std::numeric_limits<size_t>::max() )
    { error = "session_job_type_count_invalid"; return false; }
    raw.types.reserve( size_t( header.typeCount ) );
    for( uint64_t i = 0; i < header.typeCount; ++i )
    {
        StoredJobType stored;
        if( !in.read( reinterpret_cast<char*>( &stored ), sizeof( stored ) ) || stored.reserved != 0 ||
            stored.nameBytes > 1024 * 1024 )
        { error = "session_job_type_record_invalid"; return false; }
        std::string name( stored.nameBytes, '\0' );
        if( !name.empty() && !in.read( name.data(), std::streamsize( name.size() ) ) )
        { error = "session_job_type_name_truncated"; return false; }
        raw.types.push_back( { JnJobTypeData { 0, stored.typeId, stored.kind, stored.flags }, std::move( name ) } );
    }
    if( !ReadVector( in, header.scheduleCount, raw.schedules ) ||
        !ReadVector( in, header.configCount, raw.configs ) ||
        !ReadVector( in, header.dependencyCount, raw.dependencies ) ||
        !ReadVector( in, header.stageCount, raw.stages ) ||
        !ReadVector( in, header.frameCount, raw.frames ) ||
        !ReadVector( in, header.callsiteCount, raw.callsites ) )
    { error = "session_job_file_records_truncated"; return false; }
    if( in.peek() != std::char_traits<char>::eof() )
    { error = "session_job_file_trailing_bytes"; return false; }
    if( raw.types.size() != manifest.stats.jobTypes ||
        raw.schedules.size() != manifest.stats.schedules ||
        raw.configs.size() != manifest.stats.configs ||
        raw.dependencies.size() != manifest.stats.dependencies ||
        raw.stages.size() != manifest.stats.stages )
    { error = "session_job_manifest_count_mismatch"; return false; }
    return true;
}

}

std::filesystem::path TraceSessionJobIndexRoot( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest )
{
    return sessionRoot / "generations" / manifest.generation / "derived" /
        "job-index" / "1" / "exact";
}

bool BuildTraceSessionJobDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionJobStats& stats, std::string& error )
{
    error.clear(); stats = {};
    BuildState state;
    if( !LoadTraceSessionTimeTransform( sessionRoot, manifest, state.transform, error ) ) return false;
    if( !VisitTraceSessionCanonicalOrdered( sessionRoot, manifest, VisitJobRecord, &state, error ) ) return false;
    if( state.pendingCallstack != 0 || state.serialNextCallstack != 0 )
    { error = "session_job_callstack_payload_unconsumed"; return false; }
    state.raw.callsites.reserve( state.callsites.size() );
    for( const auto& [id, callsite] : state.callsites ) state.raw.callsites.push_back( callsite );
    std::sort( state.raw.callsites.begin(), state.raw.callsites.end(),
        []( const auto& lhs, const auto& rhs ) { return lhs.callsiteId < rhs.callsiteId; } );
    state.raw.types.reserve( state.unresolvedTypes.size() );
    for( const auto& type : state.unresolvedTypes )
    {
        const auto found = state.strings.find( type.name );
        if( type.name != 0 && found == state.strings.end() )
        { error = "session_job_type_name_unresolved"; return false; }
        state.raw.types.push_back( { type, found == state.strings.end() ? std::string() : found->second } );
    }
    JobManifest jobManifest;
    if( !WriteRawStore( TraceSessionJobIndexRoot( sessionRoot, manifest ), manifest,
        state.raw, jobManifest, error ) ) return false;
    stats = jobManifest.stats;
    return true;
}

std::shared_ptr<TraceSessionJobReader> TraceSessionJobReader::Open(
    const std::filesystem::path& sessionRoot, const TraceSessionManifest& session,
    std::string& error )
{
    error.clear();
    const auto root = TraceSessionJobIndexRoot( sessionRoot, session );
    JobManifest manifest;
    if( !LoadJobManifest( root, manifest, error ) ) return {};
    if( manifest.sourceSha256 != session.source.sha256 || manifest.sourceSize != session.source.fileSize ||
        manifest.generation != session.generation )
    { error = "session_job_identity_mismatch"; return {}; }
    RawJobStore raw;
    if( !ReadRawStore( root, session, manifest, raw, error ) ) return {};
    auto reader = std::shared_ptr<TraceSessionJobReader>( new TraceSessionJobReader );
    reader->m_jobs = BuildJobs( raw, session.source.sha256 );
    reader->m_stats = manifest.stats;
    if( reader->m_jobs.size() != manifest.stats.jobs )
    { error = "session_job_count_mismatch"; return {}; }
    return reader;
}

}

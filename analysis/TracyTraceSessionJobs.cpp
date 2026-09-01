#include "TracyTraceSessionJobs.hpp"

#include "TracyHash.hpp"
#include "TracyTraceSessionCanonical.hpp"
#include "TracyTraceSessionExternalSort.hpp"
#include "TracyTraceSessionGpuCanonical.hpp"
#include "TracyTraceSessionStatistics.hpp"
#include "TracyProtocol.hpp"
#include "TracyQueue.hpp"
#include "../server/TracyJnData.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <queue>
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
constexpr uint64_t JobPageFileMagic = 0x3147504a534e4aull;
constexpr uint64_t JobPageManifestMagic = 0x314d504a534e4aull;
constexpr uint32_t JobPageSchemaVersion = 4;
constexpr const char* JobPageFileName = "job-pages.bin";
constexpr uint64_t JobPostingFileMagic = 0x3154504a534e4aull;
constexpr uint32_t JobPostingSchemaVersion = 3;
constexpr const char* JobPostingFileName = "job-postings.bin";

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

struct JobPageFileHeader
{
    uint64_t magic = JobPageFileMagic;
    uint32_t schema = JobPageSchemaVersion;
    uint32_t reserved = 0;
    uint64_t sourceSize = 0;
    uint64_t jobCount = 0;
    uint64_t typeCount = 0;
    uint64_t frameCount = 0;
    uint64_t callsiteCount = 0;
    uint64_t scheduleCount = 0;
    uint64_t configCount = 0;
    uint64_t dependencyCount = 0;
    uint64_t stageCount = 0;
    uint32_t generationBytes = 0;
    uint32_t endian = 0x01020304;
};

struct JobPostingFileHeader
{
    uint64_t magic = JobPostingFileMagic;
    uint32_t schema = JobPostingSchemaVersion;
    uint32_t reserved = 0;
    uint64_t sourceSize = 0;
    uint64_t reverseDependencyCount = 0;
    uint64_t frameJobCount = 0;
    uint64_t packedHandleJobCount = 0;
    uint64_t handleSlotJobCount = 0;
    uint64_t scheduleToReadyCount = 0;
    uint64_t readyToQueueCount = 0;
    uint64_t queueToFirstRunCount = 0;
    uint64_t dependencyReadyCount = 0;
    uint64_t executionCount = 0;
    uint64_t waitCount = 0;
    uint64_t reverseDependenciesOffset = 0;
    uint64_t frameJobsOffset = 0;
    uint64_t packedHandleJobsOffset = 0;
    uint64_t handleSlotJobsOffset = 0;
    uint64_t scheduleToReadyOffset = 0;
    uint64_t readyToQueueOffset = 0;
    uint64_t queueToFirstRunOffset = 0;
    uint64_t dependencyReadyOffset = 0;
    uint64_t executionOffset = 0;
    uint64_t waitOffset = 0;
    uint32_t generationBytes = 0;
    uint32_t endian = 0x01020304;
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

struct JobPageManifest
{
    std::string sourceSha256;
    uint64_t sourceSize = 0;
    std::string generation;
    uint64_t fileBytes = 0;
    std::string fileSha256;
    uint64_t jobs = 0;
    uint64_t postingFileBytes = 0;
    std::string postingFileSha256;
    uint64_t reverseDependencies = 0;
    uint64_t frameJobs = 0;
    uint64_t packedHandleJobs = 0;
    uint64_t handleSlotJobs = 0;
    uint64_t scheduleToReady = 0;
    uint64_t readyToQueue = 0;
    uint64_t queueToFirstRun = 0;
    uint64_t dependencyReady = 0;
    uint64_t execution = 0;
    uint64_t wait = 0;
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
    out << "peak_buffered_records " << value.stats.peakBufferedRecords << '\n';
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
        else if( key == "peak_buffered_records" ) in >> value.stats.peakBufferedRecords;
        else { std::string ignored; std::getline( in, ignored ); }
        if( !in ) { error = "session_job_manifest_parse_failed"; return false; }
    }
    value.stats.fileBytes = value.fileBytes;
    if( magic != JobManifestMagic || schema != TraceSessionJobIndexSchemaVersion ||
        value.sourceSha256.size() != 64 || value.fileSha256.size() != 64 )
    { error = "session_job_manifest_invalid"; return false; }
    return true;
}

bool SaveJobPageManifest( const std::filesystem::path& root,
    const JobPageManifest& value, std::string& error )
{
    const auto temporary = root / "pages-manifest.tmp";
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_job_page_manifest_open_failed"; return false; }
    out << "magic " << JobPageManifestMagic << '\n';
    out << "schema " << JobPageSchemaVersion << '\n';
    out << "source_sha256 " << std::quoted( value.sourceSha256 ) << '\n';
    out << "source_size " << value.sourceSize << '\n';
    out << "generation " << std::quoted( value.generation ) << '\n';
    out << "file_bytes " << value.fileBytes << '\n';
    out << "file_sha256 " << std::quoted( value.fileSha256 ) << '\n';
    out << "jobs " << value.jobs << '\n';
    out << "posting_file_bytes " << value.postingFileBytes << '\n';
    out << "posting_file_sha256 " << std::quoted( value.postingFileSha256 ) << '\n';
    out << "reverse_dependencies " << value.reverseDependencies << '\n';
    out << "frame_jobs " << value.frameJobs << '\n';
    out << "packed_handle_jobs " << value.packedHandleJobs << '\n';
    out << "handle_slot_jobs " << value.handleSlotJobs << '\n';
    out << "schedule_to_ready " << value.scheduleToReady << '\n';
    out << "ready_to_queue " << value.readyToQueue << '\n';
    out << "queue_to_first_run " << value.queueToFirstRun << '\n';
    out << "dependency_ready " << value.dependencyReady << '\n';
    out << "execution " << value.execution << '\n';
    out << "wait " << value.wait << '\n';
    out.flush();
    if( !out ) { error = "session_job_page_manifest_write_failed"; return false; }
    out.close();
    return AtomicReplace( temporary, root / "pages-manifest", error );
}

bool LoadJobPageManifest( const std::filesystem::path& root,
    JobPageManifest& value, std::string& error )
{
    value = {};
    std::ifstream in( root / "pages-manifest", std::ios::binary );
    if( !in ) { error = "session_job_page_manifest_not_found"; return false; }
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
        else if( key == "jobs" ) in >> value.jobs;
        else if( key == "posting_file_bytes" ) in >> value.postingFileBytes;
        else if( key == "posting_file_sha256" ) in >> std::quoted( value.postingFileSha256 );
        else if( key == "reverse_dependencies" ) in >> value.reverseDependencies;
        else if( key == "frame_jobs" ) in >> value.frameJobs;
        else if( key == "packed_handle_jobs" ) in >> value.packedHandleJobs;
        else if( key == "handle_slot_jobs" ) in >> value.handleSlotJobs;
        else if( key == "schedule_to_ready" ) in >> value.scheduleToReady;
        else if( key == "ready_to_queue" ) in >> value.readyToQueue;
        else if( key == "queue_to_first_run" ) in >> value.queueToFirstRun;
        else if( key == "dependency_ready" ) in >> value.dependencyReady;
        else if( key == "execution" ) in >> value.execution;
        else if( key == "wait" ) in >> value.wait;
        else { std::string ignored; std::getline( in, ignored ); }
        if( !in ) { error = "session_job_page_manifest_parse_failed"; return false; }
    }
    if( magic != JobPageManifestMagic || schema != JobPageSchemaVersion ||
        value.sourceSha256.size() != 64 || value.fileSha256.size() != 64 ||
        value.postingFileSha256.size() != 64 )
    { error = "session_job_page_manifest_invalid"; return false; }
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
    TraceSessionTimeTransform transform;
    std::unordered_map<uint64_t, std::string> strings;
    std::vector<JnJobTypeData> unresolvedTypes;
    std::unordered_map<std::string, uint32_t> callstackIds;
    std::unordered_map<uint32_t, StoredCallsite> callsites;
    uint32_t pendingCallstack = 0;
    uint32_t serialNextCallstack = 0;
    std::ofstream schedules;
    std::ofstream configs;
    std::ofstream dependencies;
    std::ofstream stages;
    std::ofstream frames;
    std::ofstream jobIds;
    TraceSessionJobStats stats;
    uint64_t frameCount = 0;
};

template<typename T>
bool WriteRecord( std::ofstream& out, const T& value, const char* failure,
    std::string& error )
{
    out.write( reinterpret_cast<const char*>( &value ), sizeof( value ) );
    if( out ) return true;
    error = failure;
    return false;
}

bool WriteJobId( BuildState& state, uint64_t jobId, std::string& error )
{
    return WriteRecord( state.jobIds, jobId, "session_job_id_work_write_failed", error );
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
        state.unresolvedTypes.push_back( { item.jnJobType.name, item.jnJobType.typeId,
            item.jnJobType.kind, item.jnJobType.flags } );
        break;
    case QueueType::JnJobSchedule:
    {
        const JnJobScheduleData value { state.transform.ToNanoseconds( item.jnJobSchedule.time ),
            item.jnJobSchedule.jobId, item.jnJobSchedule.packedHandle, record.threadContext,
            item.jnJobSchedule.dependencyCount, item.jnJobSchedule.kind, item.jnJobSchedule.flags };
        if( !WriteRecord( state.schedules, value, "session_job_schedule_work_write_failed", error ) ||
            !WriteJobId( state, value.jobId, error ) ) return false;
        ++state.stats.schedules;
        break;
    }
    case QueueType::JnJobConfig:
    {
        const JnJobConfigData value { item.jnJobConfig.jobId, item.jnJobConfig.typeId,
            item.jnJobConfig.count, item.jnJobConfig.grainSize, item.jnJobConfig.unityFlowId,
            item.jnJobConfig.originFrameSequence, item.jnJobConfig.kind, item.jnJobConfig.flags };
        if( !WriteRecord( state.configs, value, "session_job_config_work_write_failed", error ) ||
            !WriteJobId( state, value.jobId, error ) ) return false;
        ++state.stats.configs;
        break;
    }
    case QueueType::JnJobDependency:
    {
        const JnJobDependencyData value { item.jnJobDependency.jobId,
            item.jnJobDependency.prerequisiteJobId, item.jnJobDependency.prerequisiteHandle,
            item.jnJobDependency.flags };
        if( !WriteRecord( state.dependencies, value, "session_job_dependency_work_write_failed", error ) ||
            !WriteJobId( state, value.jobId, error ) ) return false;
        ++state.stats.dependencies;
        break;
    }
    case QueueType::JnJobStage:
    {
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
        const JnJobStageData value { state.transform.ToNanoseconds( item.jnJobStage.time ),
            item.jnJobStage.jobId, thread, spanId,
            item.jnJobStage.arg0, item.jnJobStage.arg1, item.jnJobStage.stage,
            item.jnJobStage.flags };
        if( !WriteRecord( state.stages, value, "session_job_stage_work_write_failed", error ) ||
            !WriteJobId( state, value.jobId, error ) ) return false;
        ++state.stats.stages;
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
    {
        const JnFrameData value { state.transform.ToNanoseconds( item.jnFrame.time ),
            item.jnFrame.frameId, item.jnFrame.domainIndex, record.threadContext,
            item.jnFrame.domain, item.jnFrame.phase, item.jnFrame.flags };
        if( !WriteRecord( state.frames, value, "session_job_frame_work_write_failed", error ) ) return false;
        ++state.frameCount;
        break;
    }
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

bool CopyFileBytes( const std::filesystem::path& source, std::ofstream& out,
    uint64_t& copied, std::string& error )
{
    std::ifstream in( source, std::ios::binary );
    if( !in ) { error = "session_job_work_read_failed"; return false; }
    std::vector<char> buffer( 1024 * 1024 );
    copied = 0;
    while( in )
    {
        in.read( buffer.data(), std::streamsize( buffer.size() ) );
        const auto count = in.gcount();
        if( count > 0 )
        {
            out.write( buffer.data(), count );
            copied += uint64_t( count );
        }
    }
    if( !in.eof() || !out ) { error = "session_job_work_copy_failed"; return false; }
    return true;
}

bool CountDistinctJobIds( const std::filesystem::path& source,
    const std::filesystem::path& root, uint64_t maximumBufferedRecords,
    uint64_t& distinct, uint64_t& peakBufferedRecords, std::string& error )
{
    distinct = 0; peakBufferedRecords = 0;
    maximumBufferedRecords = std::max<uint64_t>( maximumBufferedRecords, 1 );
    if( maximumBufferedRecords > std::numeric_limits<size_t>::max() )
        maximumBufferedRecords = std::numeric_limits<size_t>::max();
    std::ifstream in( source, std::ios::binary );
    if( !in ) { error = "session_job_id_work_read_failed"; return false; }
    std::vector<std::filesystem::path> runs;
    std::vector<uint64_t> values;
    values.resize( size_t( maximumBufferedRecords ) );
    while( in )
    {
        in.read( reinterpret_cast<char*>( values.data() ),
            std::streamsize( values.size() * sizeof( uint64_t ) ) );
        const auto bytes = in.gcount();
        if( bytes == 0 ) break;
        if( bytes % sizeof( uint64_t ) != 0 )
        { error = "session_job_id_work_truncated"; return false; }
        const auto count = size_t( bytes / sizeof( uint64_t ) );
        peakBufferedRecords = std::max<uint64_t>( peakBufferedRecords, count );
        values.resize( count );
        std::sort( values.begin(), values.end() );
        values.erase( std::unique( values.begin(), values.end() ), values.end() );
        const auto path = root / ( "job-id-run-" + std::to_string( runs.size() ) + ".work" );
        std::ofstream out( path, std::ios::binary | std::ios::trunc );
        if( !out || !WriteVector( out, values ) )
        { error = "session_job_id_run_write_failed"; return false; }
        out.close();
        runs.push_back( path );
        values.resize( size_t( maximumBufferedRecords ) );
    }
    if( !in.eof() ) { error = "session_job_id_work_read_failed"; return false; }

    struct Cursor { std::ifstream input; uint64_t value = 0; bool valid = false; };
    struct Head { uint64_t value; size_t run; };
    struct Greater { bool operator()( const Head& lhs, const Head& rhs ) const {
        return lhs.value > rhs.value || ( lhs.value == rhs.value && lhs.run > rhs.run ); } };
    std::vector<Cursor> cursors( runs.size() );
    std::priority_queue<Head, std::vector<Head>, Greater> heap;
    for( size_t i = 0; i < runs.size(); ++i )
    {
        cursors[i].input.open( runs[i], std::ios::binary );
        if( !cursors[i].input ) { error = "session_job_id_run_read_failed"; return false; }
        cursors[i].valid = bool( cursors[i].input.read(
            reinterpret_cast<char*>( &cursors[i].value ), sizeof( uint64_t ) ) );
        if( cursors[i].valid ) heap.push( { cursors[i].value, i } );
    }
    uint64_t previous = 0; bool havePrevious = false;
    while( !heap.empty() )
    {
        const auto head = heap.top(); heap.pop();
        if( !havePrevious || head.value != previous )
        { previous = head.value; havePrevious = true; ++distinct; }
        auto& cursor = cursors[head.run];
        if( cursor.input.read( reinterpret_cast<char*>( &cursor.value ), sizeof( uint64_t ) ) )
            heap.push( { cursor.value, head.run } );
        else if( !cursor.input.eof() )
        { error = "session_job_id_run_read_failed"; return false; }
    }
    // Windows does not permit removing a file while these merge cursors still
    // own read handles.  Letting the vector destruct at function return is too
    // late because the cleanup below runs first.
    for( auto& cursor : cursors ) cursor.input.close();
    std::error_code ec;
    for( const auto& run : runs )
    {
        ec.clear();
        if( !std::filesystem::remove( run, ec ) || ec )
        {
            error = "session_job_id_run_cleanup_failed:" +
                ( ec ? ec.message() : run.filename().string() );
            return false;
        }
    }
    return true;
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

bool WriteStreamedStore( const std::filesystem::path& root, const TraceSessionManifest& session,
    const std::vector<std::pair<JnJobTypeData, std::string>>& types,
    const std::vector<StoredCallsite>& callsites,
    const std::array<std::filesystem::path, 5>& work,
    uint64_t frameCount, const TraceSessionJobStats& inputStats,
    JobManifest& manifest, std::string& error )
{
    std::error_code ec;
    std::filesystem::create_directories( root, ec );
    if( ec ) { error = "session_job_directory_failed:" + ec.message(); return false; }
    if( session.source.sha256.size() != 64 || session.generation.size() > std::numeric_limits<uint32_t>::max() )
    { error = "session_job_identity_invalid"; return false; }
    JobFileHeader header;
    header.sourceSize = session.source.fileSize;
    header.typeCount = types.size(); header.scheduleCount = inputStats.schedules;
    header.configCount = inputStats.configs; header.dependencyCount = inputStats.dependencies;
    header.stageCount = inputStats.stages; header.frameCount = frameCount;
    header.callsiteCount = callsites.size(); header.generationBytes = uint32_t( session.generation.size() );
    auto temporary = root / JobFileName; temporary += ".tmp";
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_job_file_open_failed"; return false; }
    out.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
    out.write( session.source.sha256.data(), 64 );
    out.write( session.generation.data(), std::streamsize( session.generation.size() ) );
    for( const auto& type : types )
    {
        if( type.second.size() > std::numeric_limits<uint32_t>::max() )
        { error = "session_job_type_name_too_large"; return false; }
        StoredJobType stored { type.first.typeId, uint32_t( type.second.size() ),
            type.first.kind, type.first.flags, 0 };
        out.write( reinterpret_cast<const char*>( &stored ), sizeof( stored ) );
        out.write( type.second.data(), std::streamsize( type.second.size() ) );
    }
    const std::array<uint64_t, 5> expected = {
        inputStats.schedules * sizeof( JnJobScheduleData ),
        inputStats.configs * sizeof( JnJobConfigData ),
        inputStats.dependencies * sizeof( JnJobDependencyData ),
        inputStats.stages * sizeof( JnJobStageData ),
        header.frameCount * sizeof( JnFrameData ) };
    uint64_t copied = 0;
    for( size_t i = 0; i < work.size(); ++i )
        if( !CopyFileBytes( work[i], out, copied, error ) || copied != expected[i] )
        { if( error.empty() ) error = "session_job_work_size_mismatch"; return false; }
    if( !WriteVector( out, callsites ) )
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
    manifest.stats = inputStats;
    manifest.stats.jobTypes = types.size();
    manifest.stats.fileBytes = manifest.fileBytes;
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

bool ValidateRawStore( const std::filesystem::path& root,
    const TraceSessionManifest& session, const JobManifest& manifest,
    JobFileHeader& header, std::string& error )
{
    const auto path = root / JobFileName;
    std::error_code ec;
    const auto bytes = std::filesystem::file_size( path, ec );
    if( ec || bytes != manifest.fileBytes )
    { error = "session_job_file_size_mismatch"; return false; }
    if( Sha256File( path ) != manifest.fileSha256 )
    { error = "session_job_file_sha256_mismatch"; return false; }
    std::ifstream in( path, std::ios::binary );
    if( !in.read( reinterpret_cast<char*>( &header ), sizeof( header ) ) ||
        header.magic != JobFileMagic || header.schema != TraceSessionJobIndexSchemaVersion ||
        header.reserved != 0 || header.endian != 0x01020304 ||
        header.sourceSize != session.source.fileSize ||
        header.generationBytes > 1024 * 1024 ||
        header.typeCount != manifest.stats.jobTypes ||
        header.scheduleCount != manifest.stats.schedules ||
        header.configCount != manifest.stats.configs ||
        header.dependencyCount != manifest.stats.dependencies ||
        header.stageCount != manifest.stats.stages )
    { error = "session_job_file_header_invalid"; return false; }
    std::string sourceSha( 64, '\0' ), generation( header.generationBytes, '\0' );
    if( !in.read( sourceSha.data(), 64 ) ||
        ( !generation.empty() && !in.read( generation.data(), std::streamsize( generation.size() ) ) ) ||
        sourceSha != session.source.sha256 || generation != session.generation )
    { error = "session_job_file_identity_invalid"; return false; }
    for( uint64_t i = 0; i < header.typeCount; ++i )
    {
        StoredJobType stored;
        if( !in.read( reinterpret_cast<char*>( &stored ), sizeof( stored ) ) ||
            stored.reserved != 0 || stored.nameBytes > 1024 * 1024 )
        { error = "session_job_type_record_invalid"; return false; }
        in.seekg( stored.nameBytes, std::ios::cur );
        if( !in ) { error = "session_job_type_name_truncated"; return false; }
    }
    const auto fixedOffset = in.tellg();
    if( fixedOffset < 0 ) { error = "session_job_file_layout_invalid"; return false; }
    uint64_t expected = uint64_t( fixedOffset );
    const auto append = [&]( uint64_t count, uint64_t itemBytes ) {
        if( itemBytes != 0 && count > std::numeric_limits<uint64_t>::max() / itemBytes ) return false;
        const auto add = count * itemBytes;
        if( expected > std::numeric_limits<uint64_t>::max() - add ) return false;
        expected += add; return true;
    };
    if( !append( header.scheduleCount, sizeof( JnJobScheduleData ) ) ||
        !append( header.configCount, sizeof( JnJobConfigData ) ) ||
        !append( header.dependencyCount, sizeof( JnJobDependencyData ) ) ||
        !append( header.stageCount, sizeof( JnJobStageData ) ) ||
        !append( header.frameCount, sizeof( JnFrameData ) ) ||
        !append( header.callsiteCount, sizeof( StoredCallsite ) ) ||
        expected != manifest.fileBytes )
    { error = "session_job_file_layout_invalid"; return false; }
    return true;
}

struct JobRawLayout
{
    JobFileHeader header;
    uint64_t typesOffset = 0;
    uint64_t typesBytes = 0;
    uint64_t schedulesOffset = 0;
    uint64_t configsOffset = 0;
    uint64_t dependenciesOffset = 0;
    uint64_t stagesOffset = 0;
    uint64_t framesOffset = 0;
    uint64_t callsitesOffset = 0;
};

bool ReadJobRawLayout( const std::filesystem::path& root,
    const TraceSessionManifest& session, const JobManifest& manifest,
    JobRawLayout& layout, std::string& error )
{
    if( !ValidateRawStore( root, session, manifest, layout.header, error ) ) return false;
    std::ifstream in( root / JobFileName, std::ios::binary );
    if( !in ) { error = "session_job_file_open_failed"; return false; }
    in.seekg( sizeof( JobFileHeader ) + 64 + layout.header.generationBytes );
    if( !in ) { error = "session_job_file_layout_invalid"; return false; }
    layout.typesOffset = uint64_t( in.tellg() );
    for( uint64_t i = 0; i < layout.header.typeCount; ++i )
    {
        StoredJobType stored;
        if( !in.read( reinterpret_cast<char*>( &stored ), sizeof( stored ) ) ||
            stored.reserved != 0 || stored.nameBytes > 1024 * 1024 )
        { error = "session_job_type_record_invalid"; return false; }
        in.seekg( stored.nameBytes, std::ios::cur );
        if( !in ) { error = "session_job_type_name_truncated"; return false; }
    }
    layout.schedulesOffset = uint64_t( in.tellg() );
    layout.typesBytes = layout.schedulesOffset - layout.typesOffset;
    layout.configsOffset = layout.schedulesOffset + layout.header.scheduleCount * sizeof( JnJobScheduleData );
    layout.dependenciesOffset = layout.configsOffset + layout.header.configCount * sizeof( JnJobConfigData );
    layout.stagesOffset = layout.dependenciesOffset + layout.header.dependencyCount * sizeof( JnJobDependencyData );
    layout.framesOffset = layout.stagesOffset + layout.header.stageCount * sizeof( JnJobStageData );
    layout.callsitesOffset = layout.framesOffset + layout.header.frameCount * sizeof( JnFrameData );
    return true;
}

template<typename T>
bool BuildSortedJobSection( const std::filesystem::path& source, uint64_t sourceOffset,
    uint64_t count, const std::filesystem::path& root, const char* label,
    uint64_t maximumBufferedRecords, std::filesystem::path& output,
    std::string& error )
{
    maximumBufferedRecords = std::max<uint64_t>( maximumBufferedRecords, 1 );
    maximumBufferedRecords = std::min<uint64_t>( maximumBufferedRecords,
        std::numeric_limits<size_t>::max() );
    std::ifstream in( source, std::ios::binary );
    if( !in ) { error = "session_job_page_source_open_failed"; return false; }
    in.seekg( std::streamoff( sourceOffset ) );
    if( !in ) { error = "session_job_page_source_seek_failed"; return false; }
    std::vector<std::filesystem::path> runs;
    uint64_t remaining = count;
    while( remaining != 0 )
    {
        const auto take = size_t( std::min<uint64_t>( remaining, maximumBufferedRecords ) );
        std::vector<T> values( take );
        in.read( reinterpret_cast<char*>( values.data() ),
            std::streamsize( values.size() * sizeof( T ) ) );
        if( !in ) { error = "session_job_page_source_truncated"; return false; }
        std::stable_sort( values.begin(), values.end(),
            []( const auto& lhs, const auto& rhs ) { return lhs.jobId < rhs.jobId; } );
        const auto run = root / ( std::string( "page-" ) + label + "-run-" +
            std::to_string( runs.size() ) + ".work" );
        std::ofstream out( run, std::ios::binary | std::ios::trunc );
        if( !out || !WriteVector( out, values ) )
        { error = "session_job_page_run_write_failed"; return false; }
        out.close(); runs.emplace_back( run ); remaining -= take;
    }
    in.close();
    output = root / ( std::string( "page-" ) + label + ".work" );
    std::ofstream out( output, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_job_page_sorted_open_failed"; return false; }
    struct Cursor { std::ifstream in; T value {}; bool valid = false; };
    struct Head { uint64_t jobId = 0; size_t run = 0; };
    struct Greater { bool operator()( const Head& lhs, const Head& rhs ) const {
        return lhs.jobId > rhs.jobId || ( lhs.jobId == rhs.jobId && lhs.run > rhs.run ); } };
    std::vector<Cursor> cursors( runs.size() );
    std::priority_queue<Head, std::vector<Head>, Greater> heap;
    for( size_t i = 0; i < runs.size(); ++i )
    {
        cursors[i].in.open( runs[i], std::ios::binary );
        if( !cursors[i].in ) { error = "session_job_page_run_read_failed"; return false; }
        cursors[i].valid = bool( cursors[i].in.read(
            reinterpret_cast<char*>( &cursors[i].value ), sizeof( T ) ) );
        if( cursors[i].valid ) heap.push( { cursors[i].value.jobId, i } );
    }
    uint64_t written = 0;
    while( !heap.empty() )
    {
        const auto head = heap.top(); heap.pop();
        auto& cursor = cursors[head.run];
        out.write( reinterpret_cast<const char*>( &cursor.value ), sizeof( T ) );
        if( !out ) { error = "session_job_page_sorted_write_failed"; return false; }
        ++written;
        if( cursor.in.read( reinterpret_cast<char*>( &cursor.value ), sizeof( T ) ) )
            heap.push( { cursor.value.jobId, head.run } );
        else if( !cursor.in.eof() )
        { error = "session_job_page_run_read_failed"; return false; }
    }
    out.flush();
    if( !out || written != count )
    { error = "session_job_page_sorted_count_mismatch"; return false; }
    out.close();
    for( auto& cursor : cursors ) cursor.in.close();
    std::error_code ec;
    for( const auto& run : runs )
    {
        ec.clear();
        if( !std::filesystem::remove( run, ec ) || ec )
        { error = "session_job_page_run_cleanup_failed:" + ( ec ? ec.message() : run.string() ); return false; }
    }
    return true;
}

template<typename T>
struct JobIdCursor
{
    std::ifstream in;
    T value {};
    bool valid = false;
};

bool BuildDistinctPagedJobIds( const std::array<std::filesystem::path, 4>& sections,
    const std::filesystem::path& output, uint64_t& count, std::string& error )
{
    JobIdCursor<JnJobScheduleData> schedules;
    JobIdCursor<JnJobConfigData> configs;
    JobIdCursor<JnJobDependencyData> dependencies;
    JobIdCursor<JnJobStageData> stages;
    schedules.in.open( sections[0], std::ios::binary );
    configs.in.open( sections[1], std::ios::binary );
    dependencies.in.open( sections[2], std::ios::binary );
    stages.in.open( sections[3], std::ios::binary );
    if( !schedules.in || !configs.in || !dependencies.in || !stages.in )
    { error = "session_job_page_section_open_failed"; return false; }
    const auto advance = []( auto& cursor ) {
        cursor.valid = bool( cursor.in.read( reinterpret_cast<char*>( &cursor.value ), sizeof( cursor.value ) ) );
        return cursor.valid || cursor.in.eof();
    };
    if( !advance( schedules ) || !advance( configs ) ||
        !advance( dependencies ) || !advance( stages ) )
    { error = "session_job_page_section_read_failed"; return false; }
    std::ofstream out( output, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_job_page_ids_open_failed"; return false; }
    count = 0; uint64_t previous = 0; bool havePrevious = false;
    while( schedules.valid || configs.valid || dependencies.valid || stages.valid )
    {
        uint64_t next = std::numeric_limits<uint64_t>::max();
        if( schedules.valid ) next = std::min( next, schedules.value.jobId );
        if( configs.valid ) next = std::min( next, configs.value.jobId );
        if( dependencies.valid ) next = std::min( next, dependencies.value.jobId );
        if( stages.valid ) next = std::min( next, stages.value.jobId );
        if( !havePrevious || previous != next )
        {
            out.write( reinterpret_cast<const char*>( &next ), sizeof( next ) );
            if( !out ) { error = "session_job_page_ids_write_failed"; return false; }
            previous = next; havePrevious = true; ++count;
        }
        while( schedules.valid && schedules.value.jobId == next ) if( !advance( schedules ) ) { error = "session_job_page_section_read_failed"; return false; }
        while( configs.valid && configs.value.jobId == next ) if( !advance( configs ) ) { error = "session_job_page_section_read_failed"; return false; }
        while( dependencies.valid && dependencies.value.jobId == next ) if( !advance( dependencies ) ) { error = "session_job_page_section_read_failed"; return false; }
        while( stages.valid && stages.value.jobId == next ) if( !advance( stages ) ) { error = "session_job_page_section_read_failed"; return false; }
    }
    out.flush();
    if( !out ) { error = "session_job_page_ids_write_failed"; return false; }
    return true;
}

bool CopyRange( std::ifstream& in, uint64_t offset, uint64_t bytes,
    std::ofstream& out, std::string& error )
{
    in.clear(); in.seekg( std::streamoff( offset ) );
    if( !in ) { error = "session_job_page_copy_seek_failed"; return false; }
    std::vector<char> buffer( 1024 * 1024 );
    while( bytes != 0 )
    {
        const auto take = size_t( std::min<uint64_t>( bytes, buffer.size() ) );
        in.read( buffer.data(), std::streamsize( take ) );
        if( !in ) { error = "session_job_page_copy_read_failed"; return false; }
        out.write( buffer.data(), std::streamsize( take ) );
        if( !out ) { error = "session_job_page_copy_write_failed"; return false; }
        bytes -= take;
    }
    return true;
}

bool BuildJobPostingWork( const std::filesystem::path& source,
    const JobRawLayout& layout, const std::filesystem::path& root,
    uint64_t maximumBufferedRecords, std::filesystem::path& reverseSorted,
    uint64_t& reverseCount, std::filesystem::path& frameSorted,
    uint64_t& frameCount, std::filesystem::path& handleSorted,
    uint64_t& handleCount, std::filesystem::path& slotSorted,
    uint64_t& slotCount, std::string& error )
{
    const auto reverseSource = root / "posting-reverse-source.work";
    const auto frameSource = root / "posting-frame-source.work";
    const auto handleSource = root / "posting-handle-source.work";
    const auto slotSource = root / "posting-slot-source.work";
    reverseSorted = root / "posting-reverse-sorted.work";
    frameSorted = root / "posting-frame-sorted.work";
    handleSorted = root / "posting-handle-sorted.work";
    slotSorted = root / "posting-slot-sorted.work";
    std::ofstream reverseOut( reverseSource, std::ios::binary | std::ios::trunc );
    std::ofstream frameOut( frameSource, std::ios::binary | std::ios::trunc );
    std::ofstream handleOut( handleSource, std::ios::binary | std::ios::trunc );
    std::ofstream slotOut( slotSource, std::ios::binary | std::ios::trunc );
    std::ifstream in( source, std::ios::binary );
    if( !reverseOut || !frameOut || !handleOut || !slotOut || !in )
    { error = "session_job_posting_work_open_failed"; return false; }

    handleCount = 0;
    slotCount = 0;
    in.seekg( std::streamoff( layout.schedulesOffset ) );
    for( uint64_t index = 0; index < layout.header.scheduleCount; ++index )
    {
        JnJobScheduleData schedule;
        if( !in.read( reinterpret_cast<char*>( &schedule ), sizeof( schedule ) ) )
        { error = "session_job_posting_schedule_truncated"; return false; }
        if( schedule.packedHandle == 0 ) continue;
        const TraceSessionUInt64Pair handlePair { schedule.packedHandle, schedule.jobId };
        const TraceSessionUInt64Pair slotPair { uint32_t( schedule.packedHandle ), schedule.jobId };
        handleOut.write( reinterpret_cast<const char*>( &handlePair ), sizeof( handlePair ) );
        slotOut.write( reinterpret_cast<const char*>( &slotPair ), sizeof( slotPair ) );
        ++handleCount;
        ++slotCount;
    }

    reverseCount = 0;
    in.clear();
    in.seekg( std::streamoff( layout.dependenciesOffset ) );
    for( uint64_t index = 0; index < layout.header.dependencyCount; ++index )
    {
        JnJobDependencyData dependency;
        if( !in.read( reinterpret_cast<char*>( &dependency ), sizeof( dependency ) ) )
        { error = "session_job_posting_dependency_truncated"; return false; }
        if( dependency.prerequisiteJobId == 0 ) continue;
        const TraceSessionUInt64Pair pair { dependency.prerequisiteJobId, dependency.jobId };
        reverseOut.write( reinterpret_cast<const char*>( &pair ), sizeof( pair ) );
        ++reverseCount;
    }

    std::unordered_map<uint32_t, uint64_t> frameIdsBySequence;
    in.clear(); in.seekg( std::streamoff( layout.framesOffset ) );
    for( uint64_t index = 0; index < layout.header.frameCount; ++index )
    {
        JnFrameData frame;
        if( !in.read( reinterpret_cast<char*>( &frame ), sizeof( frame ) ) )
        { error = "session_job_posting_frame_truncated"; return false; }
        if( frame.phase == uint8_t( JnFramePhase::Begin ) &&
            ( frame.flags & uint8_t( JnFrameFlags::Canonical ) ) != 0 )
            frameIdsBySequence[uint32_t( frame.frameId )] = frame.frameId;
    }

    frameCount = 0;
    in.clear(); in.seekg( std::streamoff( layout.configsOffset ) );
    for( uint64_t index = 0; index < layout.header.configCount; ++index )
    {
        JnJobConfigData config;
        if( !in.read( reinterpret_cast<char*>( &config ), sizeof( config ) ) )
        { error = "session_job_posting_config_truncated"; return false; }
        if( config.originFrameSequence == 0 ) continue;
        const auto frame = frameIdsBySequence.find( config.originFrameSequence );
        if( frame == frameIdsBySequence.end() ) continue;
        const TraceSessionUInt64Pair pair { frame->second, config.jobId };
        frameOut.write( reinterpret_cast<const char*>( &pair ), sizeof( pair ) );
        ++frameCount;
    }
    reverseOut.flush(); frameOut.flush(); handleOut.flush(); slotOut.flush();
    if( !reverseOut || !frameOut || !handleOut || !slotOut )
    { error = "session_job_posting_work_write_failed"; return false; }
    reverseOut.close(); frameOut.close(); handleOut.close(); slotOut.close(); in.close();

    if( !SortTraceSessionUInt64Pairs( reverseSource, reverseSorted, root,
            "job-reverse-posting", reverseCount, maximumBufferedRecords, error ) ||
        !SortTraceSessionUInt64Pairs( frameSource, frameSorted, root,
            "job-frame-posting", frameCount, maximumBufferedRecords, error ) ||
        !SortTraceSessionUInt64Pairs( handleSource, handleSorted, root,
            "job-handle-posting", handleCount, maximumBufferedRecords, error ) ||
        !SortTraceSessionUInt64Pairs( slotSource, slotSorted, root,
            "job-slot-posting", slotCount, maximumBufferedRecords, error ) ) return false;
    std::error_code ec;
    for( const auto& path : { reverseSource, frameSource, handleSource, slotSource } )
    {
        ec.clear();
        if( !std::filesystem::remove( path, ec ) || ec )
        { error = "session_job_posting_source_cleanup_failed:" + ( ec ? ec.message() : path.string() ); return false; }
    }
    return true;
}

bool WriteJobPostingFile( const std::filesystem::path& temporary,
    const TraceSessionManifest& session, const std::filesystem::path& reverse,
    uint64_t reverseCount, const std::filesystem::path& frames, uint64_t frameCount,
    const std::filesystem::path& handles, uint64_t handleCount,
    const std::filesystem::path& slots, uint64_t slotCount,
    const std::array<std::filesystem::path, 6>& latencies,
    const std::array<uint64_t, 6>& latencyCounts,
    std::string& error )
{
    JobPostingFileHeader header;
    header.sourceSize = session.source.fileSize;
    header.reverseDependencyCount = reverseCount;
    header.frameJobCount = frameCount;
    header.packedHandleJobCount = handleCount;
    header.handleSlotJobCount = slotCount;
    header.scheduleToReadyCount = latencyCounts[0];
    header.readyToQueueCount = latencyCounts[1];
    header.queueToFirstRunCount = latencyCounts[2];
    header.dependencyReadyCount = latencyCounts[3];
    header.executionCount = latencyCounts[4];
    header.waitCount = latencyCounts[5];
    header.generationBytes = uint32_t( session.generation.size() );
    header.reverseDependenciesOffset = sizeof( header ) + 64 + header.generationBytes;
    header.frameJobsOffset = header.reverseDependenciesOffset +
        reverseCount * sizeof( TraceSessionUInt64Pair );
    header.packedHandleJobsOffset = header.frameJobsOffset +
        frameCount * sizeof( TraceSessionUInt64Pair );
    header.handleSlotJobsOffset = header.packedHandleJobsOffset +
        handleCount * sizeof( TraceSessionUInt64Pair );
    header.scheduleToReadyOffset = header.handleSlotJobsOffset +
        slotCount * sizeof( TraceSessionUInt64Pair );
    header.readyToQueueOffset = header.scheduleToReadyOffset +
        latencyCounts[0] * sizeof( TraceSessionUInt64Pair );
    header.queueToFirstRunOffset = header.readyToQueueOffset +
        latencyCounts[1] * sizeof( TraceSessionUInt64Pair );
    header.dependencyReadyOffset = header.queueToFirstRunOffset +
        latencyCounts[2] * sizeof( TraceSessionUInt64Pair );
    header.executionOffset = header.dependencyReadyOffset +
        latencyCounts[3] * sizeof( TraceSessionUInt64Pair );
    header.waitOffset = header.executionOffset +
        latencyCounts[4] * sizeof( TraceSessionUInt64Pair );
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_job_posting_file_open_failed"; return false; }
    out.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
    out.write( session.source.sha256.data(), 64 );
    out.write( session.generation.data(), std::streamsize( session.generation.size() ) );
    uint64_t copied = 0;
    if( !out || !CopyFileBytes( reverse, out, copied, error ) ||
        copied != reverseCount * sizeof( TraceSessionUInt64Pair ) ||
        !CopyFileBytes( frames, out, copied, error ) ||
        copied != frameCount * sizeof( TraceSessionUInt64Pair ) ||
        !CopyFileBytes( handles, out, copied, error ) ||
        copied != handleCount * sizeof( TraceSessionUInt64Pair ) ||
        !CopyFileBytes( slots, out, copied, error ) ||
        copied != slotCount * sizeof( TraceSessionUInt64Pair ) )
    { if( error.empty() ) error = "session_job_posting_file_size_mismatch"; return false; }
    for( size_t i = 0; i < latencies.size(); ++i )
        if( !CopyFileBytes( latencies[i], out, copied, error ) ||
            copied != latencyCounts[i] * sizeof( TraceSessionUInt64Pair ) )
        { if( error.empty() ) error = "session_job_latency_file_size_mismatch"; return false; }
    out.flush();
    if( !out ) { error = "session_job_posting_file_write_failed"; return false; }
    return true;
}

}

struct TraceSessionJobReader::PageState
{
    std::filesystem::path path;
    std::filesystem::path postingPath;
    JobPageFileHeader header;
    JobPostingFileHeader postingHeader;
    uint64_t jobIdsOffset = 0;
    uint64_t schedulesOffset = 0;
    uint64_t configsOffset = 0;
    uint64_t dependenciesOffset = 0;
    uint64_t stagesOffset = 0;
    RawJobStore globals;
};

std::vector<JobDto> LoadPagedJobs( const TraceSessionJobReader::PageState& page,
    const std::string& fingerprint, const std::vector<uint64_t>& ids );

bool LoadJobPageState( const std::filesystem::path& path,
    TraceSessionJobReader::PageState& page, std::string& error )
{
    page = {};
    page.path = path;
    std::ifstream in( path, std::ios::binary );
    if( !in.read( reinterpret_cast<char*>( &page.header ), sizeof( page.header ) ) )
    { error = "session_job_page_file_header_truncated"; return false; }
    in.seekg( 64 + page.header.generationBytes, std::ios::cur );
    if( !in ) { error = "session_job_page_file_identity_truncated"; return false; }
    page.globals.types.reserve( size_t( page.header.typeCount ) );
    for( uint64_t i = 0; i < page.header.typeCount; ++i )
    {
        StoredJobType stored;
        if( !in.read( reinterpret_cast<char*>( &stored ), sizeof( stored ) ) )
        { error = "session_job_page_type_truncated"; return false; }
        std::string name( stored.nameBytes, '\0' );
        if( !name.empty() && !in.read( name.data(), std::streamsize( name.size() ) ) )
        { error = "session_job_page_type_name_truncated"; return false; }
        page.globals.types.push_back( { JnJobTypeData { 0, stored.typeId,
            stored.kind, stored.flags }, std::move( name ) } );
    }
    if( !ReadVector( in, page.header.frameCount, page.globals.frames ) ||
        !ReadVector( in, page.header.callsiteCount, page.globals.callsites ) )
    { error = "session_job_page_globals_truncated"; return false; }
    page.jobIdsOffset = uint64_t( in.tellg() );
    page.schedulesOffset = page.jobIdsOffset + page.header.jobCount * sizeof( uint64_t );
    page.configsOffset = page.schedulesOffset + page.header.scheduleCount * sizeof( JnJobScheduleData );
    page.dependenciesOffset = page.configsOffset + page.header.configCount * sizeof( JnJobConfigData );
    page.stagesOffset = page.dependenciesOffset + page.header.dependencyCount * sizeof( JnJobDependencyData );
    return true;
}

bool BuildJobLatencyWork( const TraceSessionJobReader::PageState& page,
    const std::string& fingerprint, const std::filesystem::path& ids,
    const std::filesystem::path& root, uint64_t maximumBufferedRecords,
    std::array<std::filesystem::path, 6>& sorted,
    std::array<uint64_t, 6>& counts, std::string& error )
{
    const std::array<const char*, 6> names = { "schedule-ready", "ready-queue",
        "queue-run", "dependency-ready", "execution", "wait" };
    std::array<std::filesystem::path, 6> source;
    std::array<std::ofstream, 6> out;
    for( size_t i = 0; i < source.size(); ++i )
    {
        source[i] = root / ( std::string( "posting-job-latency-" ) + names[i] + "-source.work" );
        sorted[i] = root / ( std::string( "posting-job-latency-" ) + names[i] + "-sorted.work" );
        out[i].open( source[i], std::ios::binary | std::ios::trunc );
        if( !out[i] ) { error = "session_job_latency_work_open_failed"; return false; }
    }
    const auto emit = [&]( size_t index, int64_t value, uint64_t jobId ) {
        const TraceSessionUInt64Pair pair { uint64_t( value ) ^ ( uint64_t( 1 ) << 63 ), jobId };
        out[index].write( reinterpret_cast<const char*>( &pair ), sizeof( pair ) );
        ++counts[index];
        return bool( out[index] );
    };
    std::ifstream idInput( ids, std::ios::binary );
    if( !idInput ) { error = "session_job_latency_ids_open_failed"; return false; }
    const auto chunkSize = size_t( std::max<uint64_t>( 1,
        std::min<uint64_t>( maximumBufferedRecords, 4096 ) ) );
    std::vector<uint64_t> chunk( chunkSize );
    uint64_t remaining = page.header.jobCount;
    while( remaining != 0 )
    {
        const auto count = size_t( std::min<uint64_t>( remaining, chunk.size() ) );
        if( !idInput.read( reinterpret_cast<char*>( chunk.data() ),
            std::streamsize( count * sizeof( uint64_t ) ) ) )
        { error = "session_job_latency_ids_truncated"; return false; }
        std::vector<uint64_t> current( chunk.begin(), chunk.begin() + count );
        const auto jobs = LoadPagedJobs( page, fingerprint, current );
        if( jobs.size() != count )
        { error = "session_job_latency_page_count_mismatch"; return false; }
        for( const auto& job : jobs )
        {
            const bool captureBoundary = job.orphan || job.truncated;
            if( !captureBoundary && job.readyNs && *job.readyNs >= job.scheduleNs &&
                !emit( 0, *job.readyNs - job.scheduleNs, job.jobId ) ) return false;
            if( !captureBoundary && job.readyNs && job.queueEnterNs &&
                *job.queueEnterNs >= *job.readyNs &&
                !emit( 1, *job.queueEnterNs - *job.readyNs, job.jobId ) ) return false;
            if( !captureBoundary && job.queueEnterNs && job.firstRunNs &&
                *job.firstRunNs >= *job.queueEnterNs &&
                !emit( 2, *job.firstRunNs - *job.queueEnterNs, job.jobId ) ) return false;
            if( !captureBoundary && job.dependencyReadyLatencyNs &&
                !emit( 3, *job.dependencyReadyLatencyNs, job.jobId ) ) return false;
            if( job.executionNs > 0 && !emit( 4, job.executionNs, job.jobId ) ) return false;
            if( job.waitNs > 0 && !emit( 5, job.waitNs, job.jobId ) ) return false;
        }
        remaining -= count;
    }
    for( auto& stream : out ) { stream.flush(); if( !stream ) return false; stream.close(); }
    for( size_t i = 0; i < source.size(); ++i )
        if( !SortTraceSessionUInt64Pairs( source[i], sorted[i], root,
            std::string( "job-latency-" ) + names[i], counts[i],
            maximumBufferedRecords, error ) ) return false;
    std::error_code ec;
    for( const auto& path : source )
    {
        ec.clear();
        if( !std::filesystem::remove( path, ec ) || ec )
        { error = "session_job_latency_source_cleanup_failed:" + ( ec ? ec.message() : path.string() ); return false; }
    }
    return true;
}

std::filesystem::path TraceSessionJobIndexRoot( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest )
{
    return sessionRoot / "generations" / manifest.generation / "derived" /
        "job-index" / std::to_string( TraceSessionJobIndexSchemaVersion ) / "exact";
}

bool CleanupTraceSessionJobTemporaryRuns( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, std::string& error )
{
    error.clear();
    const auto root = TraceSessionJobIndexRoot( sessionRoot, manifest );
    std::error_code ec;
    if( !std::filesystem::exists( root, ec ) ) return !ec;
    if( ec ) { error = "session_job_id_run_cleanup_scan_failed:" + ec.message(); return false; }
    for( std::filesystem::directory_iterator it( root, ec ), end; it != end; it.increment( ec ) )
    {
        if( ec ) { error = "session_job_id_run_cleanup_scan_failed:" + ec.message(); return false; }
        if( !it->is_regular_file( ec ) )
        {
            if( ec ) { error = "session_job_id_run_cleanup_scan_failed:" + ec.message(); return false; }
            continue;
        }
        const auto name = it->path().filename().string();
        const bool knownTemporary = name.starts_with( "job-id-run-" ) ||
            name.starts_with( "page-" ) || name.starts_with( "posting-" ) ||
            name.starts_with( "job-reverse-posting-run-" ) ||
            name.starts_with( "job-frame-posting-run-" );
        if( !knownTemporary || !name.ends_with( ".work" ) ) continue;
        ec.clear();
        if( !std::filesystem::remove( it->path(), ec ) || ec )
        {
            error = "session_job_id_run_cleanup_failed:" +
                ( ec ? ec.message() : name );
            return false;
        }
    }
    if( ec ) { error = "session_job_id_run_cleanup_scan_failed:" + ec.message(); return false; }
    return true;
}

bool BuildTraceSessionJobDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionJobStats& stats, std::string& error,
    const TraceSessionJobBuildOptions& options )
{
    error.clear(); stats = {};
    if( manifest.source.sha256.size() != 64 ||
        manifest.generation.size() > std::numeric_limits<uint32_t>::max() )
    { error = "session_job_identity_invalid"; return false; }
    const auto root = TraceSessionJobIndexRoot( sessionRoot, manifest );
    std::error_code ec;
    std::filesystem::create_directories( root, ec );
    if( ec ) { error = "session_job_directory_failed:" + ec.message(); return false; }
    const std::array<std::filesystem::path, 6> allWork = {
        root / "schedule.work", root / "config.work", root / "dependency.work",
        root / "stage.work", root / "frame.work", root / "job-id.work" };
    BuildState state;
    state.schedules.open( allWork[0], std::ios::binary | std::ios::trunc );
    state.configs.open( allWork[1], std::ios::binary | std::ios::trunc );
    state.dependencies.open( allWork[2], std::ios::binary | std::ios::trunc );
    state.stages.open( allWork[3], std::ios::binary | std::ios::trunc );
    state.frames.open( allWork[4], std::ios::binary | std::ios::trunc );
    state.jobIds.open( allWork[5], std::ios::binary | std::ios::trunc );
    if( !state.schedules || !state.configs || !state.dependencies || !state.stages ||
        !state.frames || !state.jobIds )
    { error = "session_job_work_open_failed"; return false; }
    if( !LoadTraceSessionTimeTransform( sessionRoot, manifest, state.transform, error ) ||
        !VisitTraceSessionCanonicalOrdered( sessionRoot, manifest, VisitJobRecord, &state, error ) ) return false;
    if( state.pendingCallstack != 0 || state.serialNextCallstack != 0 )
    { error = "session_job_callstack_payload_unconsumed"; return false; }
    state.schedules.close(); state.configs.close(); state.dependencies.close();
    state.stages.close(); state.frames.close(); state.jobIds.close();
    if( !state.schedules || !state.configs || !state.dependencies || !state.stages ||
        !state.frames || !state.jobIds )
    { error = "session_job_work_flush_failed"; return false; }
    std::vector<StoredCallsite> callsites;
    callsites.reserve( state.callsites.size() );
    for( const auto& [id, callsite] : state.callsites ) callsites.push_back( callsite );
    std::sort( callsites.begin(), callsites.end(),
        []( const auto& lhs, const auto& rhs ) { return lhs.callsiteId < rhs.callsiteId; } );
    std::vector<std::pair<JnJobTypeData, std::string>> types;
    types.reserve( state.unresolvedTypes.size() );
    for( const auto& type : state.unresolvedTypes )
    {
        const auto found = state.strings.find( type.name );
        if( type.name != 0 && found == state.strings.end() )
        { error = "session_job_type_name_unresolved"; return false; }
        types.push_back( { type, found == state.strings.end() ? std::string() : found->second } );
    }
    if( !CountDistinctJobIds( allWork[5], root, options.maximumBufferedRecords,
        state.stats.jobs, state.stats.peakBufferedRecords, error ) ) return false;
    const std::array<std::filesystem::path, 5> dataWork = {
        allWork[0], allWork[1], allWork[2], allWork[3], allWork[4] };
    JobManifest jobManifest;
    if( !WriteStreamedStore( root, manifest, types, callsites, dataWork,
        state.frameCount, state.stats, jobManifest, error ) ) return false;
    for( const auto& path : allWork ) { ec.clear(); std::filesystem::remove( path, ec ); }
    stats = jobManifest.stats;
    return true;
}

bool BuildTraceSessionJobPagingDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& session, std::string& error,
    const TraceSessionJobBuildOptions& options )
{
    error.clear();
    const auto root = TraceSessionJobIndexRoot( sessionRoot, session );
    JobManifest sourceManifest;
    if( !LoadJobManifest( root, sourceManifest, error ) ) return false;
    JobRawLayout layout;
    if( !ReadJobRawLayout( root, session, sourceManifest, layout, error ) ) return false;
    std::error_code ec;
    for( std::filesystem::directory_iterator it( root, ec ), end; it != end; it.increment( ec ) )
    {
        if( ec ) { error = "session_job_page_cleanup_scan_failed:" + ec.message(); return false; }
        if( !it->is_regular_file( ec ) ) continue;
        const auto name = it->path().filename().string();
        if( name.starts_with( "page-" ) && name.ends_with( ".work" ) )
        {
            ec.clear();
            if( !std::filesystem::remove( it->path(), ec ) || ec )
            { error = "session_job_page_cleanup_failed:" + ( ec ? ec.message() : name ); return false; }
        }
    }
    if( ec ) { error = "session_job_page_cleanup_scan_failed:" + ec.message(); return false; }

    const auto source = root / JobFileName;
    std::array<std::filesystem::path, 4> sorted;
    if( !BuildSortedJobSection<JnJobScheduleData>( source, layout.schedulesOffset,
            layout.header.scheduleCount, root, "schedule", options.maximumBufferedRecords, sorted[0], error ) ||
        !BuildSortedJobSection<JnJobConfigData>( source, layout.configsOffset,
            layout.header.configCount, root, "config", options.maximumBufferedRecords, sorted[1], error ) ||
        !BuildSortedJobSection<JnJobDependencyData>( source, layout.dependenciesOffset,
            layout.header.dependencyCount, root, "dependency", options.maximumBufferedRecords, sorted[2], error ) ||
        !BuildSortedJobSection<JnJobStageData>( source, layout.stagesOffset,
            layout.header.stageCount, root, "stage", options.maximumBufferedRecords, sorted[3], error ) ) return false;
    const auto ids = root / "page-job-ids.work";
    uint64_t jobCount = 0;
    if( !BuildDistinctPagedJobIds( sorted, ids, jobCount, error ) ) return false;
    if( jobCount != sourceManifest.stats.jobs )
    { error = "session_job_page_distinct_count_mismatch"; return false; }
    std::filesystem::path reversePostings, framePostings, handlePostings, slotPostings;
    uint64_t reversePostingCount = 0, framePostingCount = 0;
    uint64_t handlePostingCount = 0, slotPostingCount = 0;
    if( !BuildJobPostingWork( source, layout, root, options.maximumBufferedRecords,
        reversePostings, reversePostingCount, framePostings, framePostingCount,
        handlePostings, handlePostingCount, slotPostings, slotPostingCount, error ) ) return false;

    JobPageFileHeader header;
    header.sourceSize = session.source.fileSize; header.jobCount = jobCount;
    header.typeCount = layout.header.typeCount; header.frameCount = layout.header.frameCount;
    header.callsiteCount = layout.header.callsiteCount; header.scheduleCount = layout.header.scheduleCount;
    header.configCount = layout.header.configCount; header.dependencyCount = layout.header.dependencyCount;
    header.stageCount = layout.header.stageCount; header.generationBytes = uint32_t( session.generation.size() );
    auto temporary = root / JobPageFileName; temporary += ".tmp";
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    std::ifstream raw( source, std::ios::binary );
    if( !out || !raw ) { error = "session_job_page_file_open_failed"; return false; }
    out.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
    out.write( session.source.sha256.data(), std::streamsize( session.source.sha256.size() ) );
    out.write( session.generation.data(), std::streamsize( session.generation.size() ) );
    if( !out ||
        !CopyRange( raw, layout.typesOffset, layout.typesBytes, out, error ) ||
        !CopyRange( raw, layout.framesOffset, layout.header.frameCount * sizeof( JnFrameData ), out, error ) ||
        !CopyRange( raw, layout.callsitesOffset, layout.header.callsiteCount * sizeof( StoredCallsite ), out, error ) ) return false;
    uint64_t copied = 0;
    if( !CopyFileBytes( ids, out, copied, error ) || copied != jobCount * sizeof( uint64_t ) )
    { if( error.empty() ) error = "session_job_page_ids_size_mismatch"; return false; }
    const std::array<uint64_t, 4> expected = {
        header.scheduleCount * sizeof( JnJobScheduleData ),
        header.configCount * sizeof( JnJobConfigData ),
        header.dependencyCount * sizeof( JnJobDependencyData ),
        header.stageCount * sizeof( JnJobStageData ) };
    for( size_t i = 0; i < sorted.size(); ++i )
        if( !CopyFileBytes( sorted[i], out, copied, error ) || copied != expected[i] )
        { if( error.empty() ) error = "session_job_page_section_size_mismatch"; return false; }
    out.flush();
    if( !out ) { error = "session_job_page_file_write_failed"; return false; }
    out.close(); raw.close();
    TraceSessionJobReader::PageState temporaryPage;
    if( !LoadJobPageState( temporary, temporaryPage, error ) ) return false;
    std::array<std::filesystem::path, 6> latencyPostings;
    std::array<uint64_t, 6> latencyPostingCounts {};
    if( !BuildJobLatencyWork( temporaryPage, session.source.sha256, ids, root,
        options.maximumBufferedRecords, latencyPostings, latencyPostingCounts, error ) ) return false;
    const auto target = root / JobPageFileName;
    const auto postingTarget = root / JobPostingFileName;
    auto postingTemporary = postingTarget; postingTemporary += ".tmp";
    if( !WriteJobPostingFile( postingTemporary, session, reversePostings,
        reversePostingCount, framePostings, framePostingCount,
        handlePostings, handlePostingCount, slotPostings, slotPostingCount,
        latencyPostings, latencyPostingCounts, error ) ) return false;
    if( !AtomicReplace( temporary, target, error ) ) return false;
    if( !AtomicReplace( postingTemporary, postingTarget, error ) ) return false;
    JobPageManifest pageManifest;
    pageManifest.sourceSha256 = session.source.sha256; pageManifest.sourceSize = session.source.fileSize;
    pageManifest.generation = session.generation; pageManifest.jobs = jobCount;
    pageManifest.fileBytes = std::filesystem::file_size( target, ec );
    if( ec ) { error = "session_job_page_file_size_failed:" + ec.message(); return false; }
    pageManifest.fileSha256 = Sha256File( target );
    pageManifest.postingFileBytes = std::filesystem::file_size( postingTarget, ec );
    if( ec ) { error = "session_job_posting_file_size_failed:" + ec.message(); return false; }
    pageManifest.postingFileSha256 = Sha256File( postingTarget );
    pageManifest.reverseDependencies = reversePostingCount;
    pageManifest.frameJobs = framePostingCount;
    pageManifest.packedHandleJobs = handlePostingCount;
    pageManifest.handleSlotJobs = slotPostingCount;
    pageManifest.scheduleToReady = latencyPostingCounts[0];
    pageManifest.readyToQueue = latencyPostingCounts[1];
    pageManifest.queueToFirstRun = latencyPostingCounts[2];
    pageManifest.dependencyReady = latencyPostingCounts[3];
    pageManifest.execution = latencyPostingCounts[4];
    pageManifest.wait = latencyPostingCounts[5];
    if( !SaveJobPageManifest( root, pageManifest, error ) ) return false;
    for( const auto& path : sorted )
    {
        ec.clear();
        if( !std::filesystem::remove( path, ec ) || ec )
        { error = "session_job_page_sorted_cleanup_failed:" + ( ec ? ec.message() : path.string() ); return false; }
    }
    ec.clear();
    if( !std::filesystem::remove( ids, ec ) || ec )
    { error = "session_job_page_ids_cleanup_failed:" + ( ec ? ec.message() : ids.string() ); return false; }
    for( const auto& path : { reversePostings, framePostings, handlePostings, slotPostings } )
    {
        ec.clear();
        if( !std::filesystem::remove( path, ec ) || ec )
        { error = "session_job_posting_sorted_cleanup_failed:" + ( ec ? ec.message() : path.string() ); return false; }
    }
    for( const auto& path : latencyPostings )
    {
        ec.clear();
        if( !std::filesystem::remove( path, ec ) || ec )
        { error = "session_job_latency_sorted_cleanup_failed:" + ( ec ? ec.message() : path.string() ); return false; }
    }
    return true;
}

bool AuditTraceSessionJobDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& session, TraceSessionJobStats& stats,
    std::string& error )
{
    error.clear(); stats = {};
    const auto root = TraceSessionJobIndexRoot( sessionRoot, session );
    JobManifest manifest;
    if( !LoadJobManifest( root, manifest, error ) ) return false;
    if( manifest.sourceSha256 != session.source.sha256 ||
        manifest.sourceSize != session.source.fileSize || manifest.generation != session.generation )
    { error = "session_job_identity_mismatch"; return false; }
    JobFileHeader header;
    if( !ValidateRawStore( root, session, manifest, header, error ) ) return false;
    stats = manifest.stats;
    return true;
}

bool AuditTraceSessionJobPagingDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& session, std::string& error )
{
    error.clear();
    const auto root = TraceSessionJobIndexRoot( sessionRoot, session );
    JobPageManifest manifest;
    if( !LoadJobPageManifest( root, manifest, error ) ) return false;
    const auto path = root / JobPageFileName;
    std::error_code ec;
    if( manifest.sourceSha256 != session.source.sha256 || manifest.sourceSize != session.source.fileSize ||
        manifest.generation != session.generation || std::filesystem::file_size( path, ec ) != manifest.fileBytes || ec )
    { error = "session_job_page_identity_mismatch"; return false; }
    if( Sha256File( path ) != manifest.fileSha256 )
    { error = "session_job_page_file_sha256_mismatch"; return false; }
    std::ifstream in( path, std::ios::binary ); JobPageFileHeader header;
    if( !in.read( reinterpret_cast<char*>( &header ), sizeof( header ) ) ||
        header.magic != JobPageFileMagic || header.schema != JobPageSchemaVersion ||
        header.reserved != 0 || header.endian != 0x01020304 ||
        header.sourceSize != session.source.fileSize || header.jobCount != manifest.jobs ||
        header.generationBytes != session.generation.size() )
    { error = "session_job_page_file_header_invalid"; return false; }
    std::string sourceSha( 64, '\0' ), generation( header.generationBytes, '\0' );
    if( !in.read( sourceSha.data(), 64 ) ||
        ( !generation.empty() && !in.read( generation.data(), std::streamsize( generation.size() ) ) ) ||
        sourceSha != session.source.sha256 || generation != session.generation )
    { error = "session_job_page_file_identity_invalid"; return false; }
    uint64_t expected = sizeof( header ) + 64 + header.generationBytes;
    for( uint64_t i = 0; i < header.typeCount; ++i )
    {
        StoredJobType type;
        if( !in.read( reinterpret_cast<char*>( &type ), sizeof( type ) ) ||
            type.reserved != 0 || type.nameBytes > 1024 * 1024 )
        { error = "session_job_page_type_invalid"; return false; }
        in.seekg( type.nameBytes, std::ios::cur );
        if( !in ) { error = "session_job_page_type_truncated"; return false; }
        expected += sizeof( type ) + type.nameBytes;
    }
    const auto globalsEnd = expected;
    const auto add = [&]( uint64_t count, uint64_t bytes ) {
        if( bytes != 0 && count > std::numeric_limits<uint64_t>::max() / bytes ) return false;
        const auto value = count * bytes;
        if( expected > std::numeric_limits<uint64_t>::max() - value ) return false;
        expected += value; return true;
    };
    if( !add( header.frameCount, sizeof( JnFrameData ) ) ||
        !add( header.callsiteCount, sizeof( StoredCallsite ) ) ||
        !add( header.jobCount, sizeof( uint64_t ) ) ||
        !add( header.scheduleCount, sizeof( JnJobScheduleData ) ) ||
        !add( header.configCount, sizeof( JnJobConfigData ) ) ||
        !add( header.dependencyCount, sizeof( JnJobDependencyData ) ) ||
        !add( header.stageCount, sizeof( JnJobStageData ) ) || expected != manifest.fileBytes )
    { error = "session_job_page_file_layout_invalid"; return false; }
    const auto postingPath = root / JobPostingFileName;
    ec.clear();
    if( std::filesystem::file_size( postingPath, ec ) != manifest.postingFileBytes || ec )
    { error = "session_job_posting_file_size_mismatch"; return false; }
    if( Sha256File( postingPath ) != manifest.postingFileSha256 )
    { error = "session_job_posting_file_sha256_mismatch"; return false; }
    std::ifstream postings( postingPath, std::ios::binary );
    JobPostingFileHeader postingHeader;
    if( !postings.read( reinterpret_cast<char*>( &postingHeader ), sizeof( postingHeader ) ) ||
        postingHeader.magic != JobPostingFileMagic || postingHeader.schema != JobPostingSchemaVersion ||
        postingHeader.reserved != 0 || postingHeader.endian != 0x01020304 ||
        postingHeader.sourceSize != session.source.fileSize ||
        postingHeader.generationBytes != session.generation.size() ||
        postingHeader.reverseDependencyCount != manifest.reverseDependencies ||
        postingHeader.frameJobCount != manifest.frameJobs ||
        postingHeader.packedHandleJobCount != manifest.packedHandleJobs ||
        postingHeader.handleSlotJobCount != manifest.handleSlotJobs ||
        postingHeader.scheduleToReadyCount != manifest.scheduleToReady ||
        postingHeader.readyToQueueCount != manifest.readyToQueue ||
        postingHeader.queueToFirstRunCount != manifest.queueToFirstRun ||
        postingHeader.dependencyReadyCount != manifest.dependencyReady ||
        postingHeader.executionCount != manifest.execution ||
        postingHeader.waitCount != manifest.wait )
    { error = "session_job_posting_file_header_invalid"; return false; }
    std::string postingSha( 64, '\0' ), postingGeneration( postingHeader.generationBytes, '\0' );
    if( !postings.read( postingSha.data(), 64 ) ||
        ( !postingGeneration.empty() && !postings.read( postingGeneration.data(),
            std::streamsize( postingGeneration.size() ) ) ) ||
        postingSha != session.source.sha256 || postingGeneration != session.generation )
    { error = "session_job_posting_file_identity_invalid"; return false; }
    const auto expectedReverseOffset = sizeof( postingHeader ) + 64 + postingHeader.generationBytes;
    const auto expectedFrameOffset = expectedReverseOffset +
        postingHeader.reverseDependencyCount * sizeof( TraceSessionUInt64Pair );
    const auto expectedHandleOffset = expectedFrameOffset +
        postingHeader.frameJobCount * sizeof( TraceSessionUInt64Pair );
    const auto expectedSlotOffset = expectedHandleOffset +
        postingHeader.packedHandleJobCount * sizeof( TraceSessionUInt64Pair );
    const auto expectedScheduleReadyOffset = expectedSlotOffset +
        postingHeader.handleSlotJobCount * sizeof( TraceSessionUInt64Pair );
    const auto expectedReadyQueueOffset = expectedScheduleReadyOffset +
        postingHeader.scheduleToReadyCount * sizeof( TraceSessionUInt64Pair );
    const auto expectedQueueRunOffset = expectedReadyQueueOffset +
        postingHeader.readyToQueueCount * sizeof( TraceSessionUInt64Pair );
    const auto expectedDependencyReadyOffset = expectedQueueRunOffset +
        postingHeader.queueToFirstRunCount * sizeof( TraceSessionUInt64Pair );
    const auto expectedExecutionOffset = expectedDependencyReadyOffset +
        postingHeader.dependencyReadyCount * sizeof( TraceSessionUInt64Pair );
    const auto expectedWaitOffset = expectedExecutionOffset +
        postingHeader.executionCount * sizeof( TraceSessionUInt64Pair );
    const auto expectedPostingBytes = expectedWaitOffset +
        postingHeader.waitCount * sizeof( TraceSessionUInt64Pair );
    if( postingHeader.reverseDependenciesOffset != expectedReverseOffset ||
        postingHeader.frameJobsOffset != expectedFrameOffset ||
        postingHeader.packedHandleJobsOffset != expectedHandleOffset ||
        postingHeader.handleSlotJobsOffset != expectedSlotOffset ||
        postingHeader.scheduleToReadyOffset != expectedScheduleReadyOffset ||
        postingHeader.readyToQueueOffset != expectedReadyQueueOffset ||
        postingHeader.queueToFirstRunOffset != expectedQueueRunOffset ||
        postingHeader.dependencyReadyOffset != expectedDependencyReadyOffset ||
        postingHeader.executionOffset != expectedExecutionOffset ||
        postingHeader.waitOffset != expectedWaitOffset ||
        expectedPostingBytes != manifest.postingFileBytes )
    { error = "session_job_posting_file_layout_invalid"; return false; }
    const auto schedulesOffset = globalsEnd +
        header.frameCount * sizeof( JnFrameData ) +
        header.callsiteCount * sizeof( StoredCallsite ) +
        header.jobCount * sizeof( uint64_t );
    std::ifstream page( path, std::ios::binary );
    const auto scheduleMatches = [&]( uint64_t jobId, uint64_t key, bool slot ) {
        uint64_t first = 0, last = header.scheduleCount;
        while( first < last )
        {
            const auto middle = first + ( last - first ) / 2;
            JnJobScheduleData value;
            page.clear(); page.seekg( std::streamoff( schedulesOffset + middle * sizeof( value ) ) );
            if( !page.read( reinterpret_cast<char*>( &value ), sizeof( value ) ) ) return false;
            if( value.jobId < jobId ) first = middle + 1; else last = middle;
        }
        for( auto index = first; index < header.scheduleCount; ++index )
        {
            JnJobScheduleData value;
            page.clear(); page.seekg( std::streamoff( schedulesOffset + index * sizeof( value ) ) );
            if( !page.read( reinterpret_cast<char*>( &value ), sizeof( value ) ) ) return false;
            if( value.jobId != jobId ) return false;
            const auto actual = slot ? uint64_t( uint32_t( value.packedHandle ) ) : value.packedHandle;
            if( actual == key ) return true;
        }
        return false;
    };
    const auto auditHandleSection = [&]( uint64_t offset, uint64_t count, bool slot ) {
        postings.clear(); postings.seekg( std::streamoff( offset ) );
        TraceSessionUInt64Pair previous {};
        bool havePrevious = false;
        for( uint64_t index = 0; index < count; ++index )
        {
            TraceSessionUInt64Pair pair;
            if( !postings.read( reinterpret_cast<char*>( &pair ), sizeof( pair ) ) ) return false;
            if( havePrevious && ( pair.key < previous.key ||
                ( pair.key == previous.key && pair.value < previous.value ) ) ) return false;
            if( !scheduleMatches( pair.value, pair.key, slot ) ) return false;
            previous = pair; havePrevious = true;
        }
        return true;
    };
    if( !auditHandleSection( postingHeader.packedHandleJobsOffset,
            postingHeader.packedHandleJobCount, false ) ||
        !auditHandleSection( postingHeader.handleSlotJobsOffset,
            postingHeader.handleSlotJobCount, true ) )
    { error = "session_job_handle_posting_invalid"; return false; }
    const auto jobExists = [&]( uint64_t jobId ) {
        uint64_t first = 0, last = header.jobCount;
        const auto idsOffset = globalsEnd + header.frameCount * sizeof( JnFrameData ) +
            header.callsiteCount * sizeof( StoredCallsite );
        while( first < last )
        {
            const auto middle = first + ( last - first ) / 2;
            uint64_t value = 0;
            page.clear(); page.seekg( std::streamoff( idsOffset + middle * sizeof( value ) ) );
            if( !page.read( reinterpret_cast<char*>( &value ), sizeof( value ) ) ) return false;
            if( value < jobId ) first = middle + 1; else last = middle;
        }
        if( first >= header.jobCount ) return false;
        uint64_t value = 0;
        page.clear(); page.seekg( std::streamoff( idsOffset + first * sizeof( value ) ) );
        return bool( page.read( reinterpret_cast<char*>( &value ), sizeof( value ) ) ) && value == jobId;
    };
    const auto auditLatencySection = [&]( uint64_t offset, uint64_t count ) {
        postings.clear(); postings.seekg( std::streamoff( offset ) );
        TraceSessionUInt64Pair previous {};
        bool havePrevious = false;
        for( uint64_t index = 0; index < count; ++index )
        {
            TraceSessionUInt64Pair pair;
            if( !postings.read( reinterpret_cast<char*>( &pair ), sizeof( pair ) ) ) return false;
            if( havePrevious && ( pair.key < previous.key ||
                ( pair.key == previous.key && pair.value < previous.value ) ) ) return false;
            if( !jobExists( pair.value ) ) return false;
            previous = pair; havePrevious = true;
        }
        return true;
    };
    if( !auditLatencySection( postingHeader.scheduleToReadyOffset,
            postingHeader.scheduleToReadyCount ) ||
        !auditLatencySection( postingHeader.readyToQueueOffset,
            postingHeader.readyToQueueCount ) ||
        !auditLatencySection( postingHeader.queueToFirstRunOffset,
            postingHeader.queueToFirstRunCount ) ||
        !auditLatencySection( postingHeader.dependencyReadyOffset,
            postingHeader.dependencyReadyCount ) ||
        !auditLatencySection( postingHeader.executionOffset,
            postingHeader.executionCount ) ||
        !auditLatencySection( postingHeader.waitOffset, postingHeader.waitCount ) )
    { error = "session_job_latency_posting_invalid"; return false; }
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
    JobFileHeader header;
    if( !ValidateRawStore( root, session, manifest, header, error ) ) return {};
    if( !AuditTraceSessionJobPagingDerived( sessionRoot, session, error ) ) return {};
    auto reader = std::shared_ptr<TraceSessionJobReader>( new TraceSessionJobReader );
    reader->m_root = root;
    reader->m_session = session;
    reader->m_stats = manifest.stats;
    auto page = std::make_shared<PageState>();
    page->path = root / JobPageFileName;
    std::ifstream in( page->path, std::ios::binary );
    if( !in.read( reinterpret_cast<char*>( &page->header ), sizeof( page->header ) ) )
    { error = "session_job_page_file_header_truncated"; return {}; }
    in.seekg( 64 + page->header.generationBytes, std::ios::cur );
    if( !in ) { error = "session_job_page_file_identity_truncated"; return {}; }
    page->globals.types.reserve( size_t( page->header.typeCount ) );
    for( uint64_t i = 0; i < page->header.typeCount; ++i )
    {
        StoredJobType stored;
        if( !in.read( reinterpret_cast<char*>( &stored ), sizeof( stored ) ) )
        { error = "session_job_page_type_truncated"; return {}; }
        std::string name( stored.nameBytes, '\0' );
        if( !name.empty() && !in.read( name.data(), std::streamsize( name.size() ) ) )
        { error = "session_job_page_type_name_truncated"; return {}; }
        page->globals.types.push_back( { JnJobTypeData { 0, stored.typeId, stored.kind, stored.flags }, std::move( name ) } );
    }
    if( !ReadVector( in, page->header.frameCount, page->globals.frames ) ||
        !ReadVector( in, page->header.callsiteCount, page->globals.callsites ) )
    { error = "session_job_page_globals_truncated"; return {}; }
    page->jobIdsOffset = uint64_t( in.tellg() );
    page->schedulesOffset = page->jobIdsOffset + page->header.jobCount * sizeof( uint64_t );
    page->configsOffset = page->schedulesOffset + page->header.scheduleCount * sizeof( JnJobScheduleData );
    page->dependenciesOffset = page->configsOffset + page->header.configCount * sizeof( JnJobConfigData );
    page->stagesOffset = page->dependenciesOffset + page->header.dependencyCount * sizeof( JnJobDependencyData );
    page->postingPath = root / JobPostingFileName;
    std::ifstream postings( page->postingPath, std::ios::binary );
    if( !postings.read( reinterpret_cast<char*>( &page->postingHeader ), sizeof( page->postingHeader ) ) )
    { error = "session_job_posting_file_header_truncated"; return {}; }
    reader->m_pageState = std::move( page );
    return reader;
}

template<typename T>
bool ReadPagedJobSection( const std::filesystem::path& path, uint64_t offset,
    uint64_t count, const std::vector<uint64_t>& ids, std::vector<T>& output,
    std::string& error )
{
    if( ids.empty() || count == 0 ) return true;
    std::ifstream in( path, std::ios::binary );
    if( !in ) { error = "session_job_page_read_open_failed"; return false; }
    const auto readAt = [&]( uint64_t index, T& value ) {
        in.clear();
        in.seekg( std::streamoff( offset + index * sizeof( T ) ) );
        return bool( in.read( reinterpret_cast<char*>( &value ), sizeof( value ) ) );
    };
    const auto lower = [&]( uint64_t id, bool upper ) -> std::optional<uint64_t> {
        uint64_t first = 0, last = count;
        while( first < last )
        {
            const auto middle = first + ( last - first ) / 2;
            T value;
            if( !readAt( middle, value ) ) return std::nullopt;
            if( value.jobId < id || ( upper && value.jobId == id ) ) first = middle + 1;
            else last = middle;
        }
        return first;
    };
    const auto begin = lower( ids.front(), false );
    const auto end = lower( ids.back(), true );
    if( !begin || !end || *end < *begin )
    { error = "session_job_page_binary_search_failed"; return false; }
    in.clear(); in.seekg( std::streamoff( offset + *begin * sizeof( T ) ) );
    if( !in ) { error = "session_job_page_read_seek_failed"; return false; }
    constexpr size_t Chunk = 64 * 1024;
    std::vector<T> buffer( Chunk );
    uint64_t remaining = *end - *begin;
    while( remaining != 0 )
    {
        const auto take = size_t( std::min<uint64_t>( remaining, buffer.size() ) );
        in.read( reinterpret_cast<char*>( buffer.data() ), std::streamsize( take * sizeof( T ) ) );
        if( !in ) { error = "session_job_page_read_truncated"; return false; }
        for( size_t i = 0; i < take; ++i )
            if( std::binary_search( ids.begin(), ids.end(), buffer[i].jobId ) ) output.emplace_back( buffer[i] );
        remaining -= take;
    }
    return true;
}

std::vector<JobDto> LoadPagedJobs( const TraceSessionJobReader::PageState& page,
    const std::string& fingerprint, const std::vector<uint64_t>& ids )
{
    RawJobStore raw = page.globals;
    std::string error;
    if( !ReadPagedJobSection( page.path, page.schedulesOffset, page.header.scheduleCount, ids, raw.schedules, error ) ||
        !ReadPagedJobSection( page.path, page.configsOffset, page.header.configCount, ids, raw.configs, error ) ||
        !ReadPagedJobSection( page.path, page.dependenciesOffset, page.header.dependencyCount, ids, raw.dependencies, error ) ||
        !ReadPagedJobSection( page.path, page.stagesOffset, page.header.stageCount, ids, raw.stages, error ) )
        throw std::runtime_error( error );
    // Dependency-ready latency is an exact field. Pull only the direct
    // prerequisite summaries needed by this page instead of materializing
    // every Job in the Session.
    std::vector<uint64_t> prerequisites;
    for( const auto& dependency : raw.dependencies )
        if( dependency.prerequisiteJobId != 0 &&
            !std::binary_search( ids.begin(), ids.end(), dependency.prerequisiteJobId ) )
            prerequisites.emplace_back( dependency.prerequisiteJobId );
    std::sort( prerequisites.begin(), prerequisites.end() );
    prerequisites.erase( std::unique( prerequisites.begin(), prerequisites.end() ), prerequisites.end() );
    if( !prerequisites.empty() &&
        ( !ReadPagedJobSection( page.path, page.schedulesOffset, page.header.scheduleCount,
              prerequisites, raw.schedules, error ) ||
          !ReadPagedJobSection( page.path, page.configsOffset, page.header.configCount,
              prerequisites, raw.configs, error ) ||
          !ReadPagedJobSection( page.path, page.stagesOffset, page.header.stageCount,
              prerequisites, raw.stages, error ) ) )
        throw std::runtime_error( error );
    auto values = BuildJobs( raw, fingerprint );
    values.erase( std::remove_if( values.begin(), values.end(), [&]( const auto& value ) {
        return !std::binary_search( ids.begin(), ids.end(), value.jobId ); } ), values.end() );
    return values;
}

std::vector<uint64_t> ReadJobPostingIds( const std::filesystem::path& path,
    uint64_t sectionOffset, uint64_t recordCount, uint64_t key,
    size_t offset, size_t limit )
{
    std::vector<uint64_t> result;
    if( recordCount == 0 || limit == 0 ) return result;
    std::ifstream in( path, std::ios::binary );
    if( !in ) throw std::runtime_error( "session_job_posting_read_open_failed" );
    const auto lowerBound = [&]( bool upper )
    {
        uint64_t first = 0, last = recordCount;
        while( first < last )
        {
            const auto middle = first + ( last - first ) / 2;
            TraceSessionUInt64Pair pair;
            in.clear();
            in.seekg( std::streamoff( sectionOffset + middle * sizeof( pair ) ) );
            if( !in.read( reinterpret_cast<char*>( &pair ), sizeof( pair ) ) )
                throw std::runtime_error( "session_job_posting_binary_search_failed" );
            if( pair.key < key || ( upper && pair.key == key ) ) first = middle + 1;
            else last = middle;
        }
        return first;
    };
    const auto begin = lowerBound( false );
    const auto end = lowerBound( true );
    if( begin >= end ) return result;
    result.reserve( std::min<uint64_t>( limit, end - begin ) );
    in.clear();
    in.seekg( std::streamoff( sectionOffset + begin * sizeof( TraceSessionUInt64Pair ) ) );
    uint64_t skipped = 0, previous = 0;
    bool havePrevious = false;
    for( uint64_t index = begin; index < end; ++index )
    {
        TraceSessionUInt64Pair pair;
        if( !in.read( reinterpret_cast<char*>( &pair ), sizeof( pair ) ) || pair.key != key )
            throw std::runtime_error( "session_job_posting_read_truncated" );
        if( havePrevious && pair.value == previous ) continue;
        previous = pair.value; havePrevious = true;
        if( skipped++ < offset ) continue;
        result.emplace_back( pair.value );
        if( result.size() >= limit ) break;
    }
    return result;
}

std::vector<uint64_t> ReadJobPostingIdsNear( const std::filesystem::path& path,
    uint64_t sectionOffset, uint64_t recordCount, uint64_t key,
    uint64_t targetId, size_t limit )
{
    std::vector<uint64_t> result;
    if( recordCount == 0 || limit == 0 ) return result;
    std::ifstream in( path, std::ios::binary );
    if( !in ) throw std::runtime_error( "session_job_posting_read_open_failed" );
    const auto readAt = [&]( uint64_t index ) {
        TraceSessionUInt64Pair pair;
        in.clear();
        in.seekg( std::streamoff( sectionOffset + index * sizeof( pair ) ) );
        if( !in.read( reinterpret_cast<char*>( &pair ), sizeof( pair ) ) )
            throw std::runtime_error( "session_job_posting_binary_search_failed" );
        return pair;
    };
    const auto lowerKey = [&]( bool upper ) {
        uint64_t first = 0, last = recordCount;
        while( first < last )
        {
            const auto middle = first + ( last - first ) / 2;
            const auto pair = readAt( middle );
            if( pair.key < key || ( upper && pair.key == key ) ) first = middle + 1;
            else last = middle;
        }
        return first;
    };
    const auto begin = lowerKey( false );
    const auto end = lowerKey( true );
    if( begin >= end ) return result;
    uint64_t first = begin, last = end;
    while( first < last )
    {
        const auto middle = first + ( last - first ) / 2;
        if( readAt( middle ).value < targetId ) first = middle + 1;
        else last = middle;
    }
    const auto pivot = first;
    const auto radius = uint64_t( limit );
    const auto windowBegin = pivot > begin + radius ? pivot - radius : begin;
    const auto windowEnd = std::min<uint64_t>( end, pivot + radius );
    result.reserve( size_t( windowEnd - windowBegin ) );
    for( auto index = windowBegin; index < windowEnd; ++index )
        result.emplace_back( readAt( index ).value );
    std::sort( result.begin(), result.end(), [targetId]( uint64_t lhs, uint64_t rhs ) {
        const auto lhsDistance = lhs > targetId ? lhs - targetId : targetId - lhs;
        const auto rhsDistance = rhs > targetId ? rhs - targetId : targetId - rhs;
        return lhsDistance != rhsDistance ? lhsDistance < rhsDistance : lhs < rhs;
    } );
    result.erase( std::unique( result.begin(), result.end() ), result.end() );
    if( result.size() > limit ) result.resize( limit );
    return result;
}

const std::vector<JobDto>& TraceSessionJobReader::Jobs() const
{
    if( m_jobsLoaded ) return m_jobs;
    JobManifest manifest;
    std::string error;
    if( !LoadJobManifest( m_root, manifest, error ) )
        throw std::runtime_error( error );
    RawJobStore raw;
    if( !ReadRawStore( m_root, m_session, manifest, raw, error ) )
        throw std::runtime_error( error );
    m_jobs = BuildJobs( raw, m_session.source.sha256 );
    if( m_jobs.size() != manifest.stats.jobs )
        throw std::runtime_error( "session_job_count_mismatch" );
    m_jobsLoaded = true;
    return m_jobs;
}

std::vector<JobDto> TraceSessionJobReader::Scan( size_t offset, size_t limit ) const
{
    if( !m_pageState ) return {};
    const auto begin = std::min<uint64_t>( offset, m_pageState->header.jobCount );
    const auto count = std::min<uint64_t>( limit, m_pageState->header.jobCount - begin );
    std::vector<uint64_t> ids;
    ids.resize( size_t( count ) );
    if( ids.empty() ) return {};
    std::ifstream in( m_pageState->path, std::ios::binary );
    in.seekg( std::streamoff( m_pageState->jobIdsOffset + begin * sizeof( uint64_t ) ) );
    if( !in.read( reinterpret_cast<char*>( ids.data()), std::streamsize( ids.size() * sizeof( uint64_t ) ) ) )
        throw std::runtime_error( "session_job_page_ids_truncated" );
    return LoadPagedJobs( *m_pageState, m_session.source.sha256, ids );
}

std::optional<JobDto> TraceSessionJobReader::Get( uint64_t jobId ) const
{
    if( !m_pageState ) return std::nullopt;
    std::ifstream in( m_pageState->path, std::ios::binary );
    uint64_t first = 0, last = m_pageState->header.jobCount;
    while( first < last )
    {
        const auto middle = first + ( last - first ) / 2;
        uint64_t value = 0;
        in.clear(); in.seekg( std::streamoff( m_pageState->jobIdsOffset + middle * sizeof( value ) ) );
        if( !in.read( reinterpret_cast<char*>( &value ), sizeof( value ) ) )
            throw std::runtime_error( "session_job_page_ids_truncated" );
        if( value < jobId ) first = middle + 1; else last = middle;
    }
    if( first >= m_pageState->header.jobCount ) return std::nullopt;
    uint64_t value = 0;
    in.clear(); in.seekg( std::streamoff( m_pageState->jobIdsOffset + first * sizeof( value ) ) );
    if( !in.read( reinterpret_cast<char*>( &value ), sizeof( value ) ) )
        throw std::runtime_error( "session_job_page_ids_truncated" );
    if( value != jobId ) return std::nullopt;
    auto jobs = LoadPagedJobs( *m_pageState, m_session.source.sha256, { jobId } );
    return jobs.empty() ? std::nullopt : std::optional<JobDto>( std::move( jobs.front() ) );
}

std::vector<JobDto> TraceSessionJobReader::Dependents(
    uint64_t jobId, size_t offset, size_t limit ) const
{
    if( !m_pageState ) return {};
    const auto ids = ReadJobPostingIds( m_pageState->postingPath,
        m_pageState->postingHeader.reverseDependenciesOffset,
        m_pageState->postingHeader.reverseDependencyCount, jobId, offset, limit );
    return LoadPagedJobs( *m_pageState, m_session.source.sha256, ids );
}

std::vector<JobDto> TraceSessionJobReader::FrameJobs(
    uint64_t frameId, size_t offset, size_t limit ) const
{
    if( !m_pageState ) return {};
    const auto ids = ReadJobPostingIds( m_pageState->postingPath,
        m_pageState->postingHeader.frameJobsOffset,
        m_pageState->postingHeader.frameJobCount, frameId, offset, limit );
    return LoadPagedJobs( *m_pageState, m_session.source.sha256, ids );
}

std::vector<JobDto> TraceSessionJobReader::HandleJobs(
    uint64_t packedHandle, size_t offset, size_t limit ) const
{
    if( !m_pageState ) return {};
    const auto ids = ReadJobPostingIds( m_pageState->postingPath,
        m_pageState->postingHeader.packedHandleJobsOffset,
        m_pageState->postingHeader.packedHandleJobCount, packedHandle, offset, limit );
    return LoadPagedJobs( *m_pageState, m_session.source.sha256, ids );
}

std::vector<JobDto> TraceSessionJobReader::SlotJobsNear(
    uint32_t slotIndex, uint64_t jobId, size_t limit ) const
{
    if( !m_pageState ) return {};
    const auto ids = ReadJobPostingIdsNear( m_pageState->postingPath,
        m_pageState->postingHeader.handleSlotJobsOffset,
        m_pageState->postingHeader.handleSlotJobCount, slotIndex, jobId, limit );
    return LoadPagedJobs( *m_pageState, m_session.source.sha256, ids );
}

JobLatencyStatisticsDto TraceSessionJobReader::LatencyStatistics() const
{
    if( !m_pageState ) return {};
    const auto& header = m_pageState->postingHeader;
    return {
        ReadTraceSessionExactStatistics( m_pageState->postingPath,
            header.scheduleToReadyOffset, header.scheduleToReadyCount ),
        ReadTraceSessionExactStatistics( m_pageState->postingPath,
            header.readyToQueueOffset, header.readyToQueueCount ),
        ReadTraceSessionExactStatistics( m_pageState->postingPath,
            header.queueToFirstRunOffset, header.queueToFirstRunCount ),
        ReadTraceSessionExactStatistics( m_pageState->postingPath,
            header.dependencyReadyOffset, header.dependencyReadyCount ),
        ReadTraceSessionExactStatistics( m_pageState->postingPath,
            header.executionOffset, header.executionCount ),
        ReadTraceSessionExactStatistics( m_pageState->postingPath,
            header.waitOffset, header.waitCount )
    };
}

}

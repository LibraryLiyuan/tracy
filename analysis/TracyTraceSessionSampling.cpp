#include "TracyTraceSessionSampling.hpp"

#include "TracyHash.hpp"
#include "TracyTraceSessionCanonical.hpp"
#include "TracyTraceSessionGpuCanonical.hpp"
#include "TracyProtocol.hpp"
#include "TracyQueue.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <queue>
#include <sstream>
#include <tuple>
#include <unordered_map>

#ifdef _WIN32
#  include <Windows.h>
#endif

namespace tracy::analysis
{
namespace
{

constexpr uint64_t SamplingFileMagic = 0x31504d53534e4aull;     // JNSSMP1
constexpr uint64_t SamplingManifestMagic = 0x31464d53534e4aull; // JNSSMF1
constexpr const char* SamplingFileName = "samples.bin";

#pragma pack( push, 1 )
struct SamplingFileHeader
{
    uint64_t magic = SamplingFileMagic;
    uint32_t schema = TraceSessionSamplingIndexSchemaVersion;
    uint32_t endian = 0x01020304;
    uint64_t sourceSize = 0;
    uint64_t eventCount = 0;
    uint64_t sampleCount = 0;
    uint64_t contextSwitchSampleCount = 0;
    uint64_t dictionaryEntries = 0;
    uint64_t callstackPayloads = 0;
    uint64_t recordsOffset = 0;
    uint64_t hardwareSummaryCount = 0;
    uint64_t hardwareEventCount = 0;
    uint64_t sampleBlockCount = 0;
    uint64_t sampleBlocksOffset = 0;
    uint64_t hardwareSummariesOffset = 0;
    uint64_t hardwareEventsOffset = 0;
    uint32_t generationBytes = 0;
    uint32_t reserved = 0;
};

struct StoredSample
{
    int64_t timeNs = 0;
    uint64_t thread = 0;
    uint32_t callstack = 0;
    uint8_t kind = 0;
    uint8_t reserved[3] {};
};

struct StoredHardwareSample
{
    uint64_t address = 0;
    int64_t timeNs = 0;
    uint64_t sourceOrdinal = 0;
    uint8_t kind = 0;
    uint8_t reserved[7] {};
};

struct StoredHardwareSummary
{
    uint64_t address = 0;
    uint64_t counts[6] {};
    uint64_t eventOffsets[6] {};
};

struct StoredSampleBlock
{
    uint64_t firstRecord = 0;
    uint32_t recordCount = 0;
    uint32_t reserved = 0;
    int64_t minTimeNs = 0;
    int64_t maxTimeNs = 0;
    uint64_t threadBloom[4] {};
};
#pragma pack( pop )

struct SamplingManifest
{
    std::string sourceSha256;
    uint64_t sourceSize = 0;
    std::string generation;
    uint64_t fileBytes = 0;
    std::string fileSha256;
    TraceSessionSamplingStats stats;
};

bool AtomicReplace( const std::filesystem::path& source,
    const std::filesystem::path& target, std::string& error )
{
#ifdef _WIN32
    if( MoveFileExW( source.c_str(), target.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) ) return true;
    error = "session_sampling_atomic_replace_failed:" + std::to_string( GetLastError() );
    return false;
#else
    std::error_code ec;
    std::filesystem::rename( source, target, ec );
    if( !ec ) return true;
    error = "session_sampling_atomic_replace_failed:" + ec.message();
    return false;
#endif
}

bool DecodeItem( const TraceSessionCanonicalRecord& record, QueueItem& item,
    std::string& error )
{
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ||
        record.type >= uint8_t( QueueType::NUM_TYPES ) ||
        record.payload.size() < QueueDataSize[record.type] )
    { error = "session_sampling_protocol_record_invalid"; return false; }
    item = {};
    std::memcpy( &item, record.payload.data(),
        std::min<size_t>( record.payload.size(), sizeof( item ) ) );
    if( item.hdr.idx != record.type )
    { error = "session_sampling_protocol_type_mismatch"; return false; }
    return true;
}

bool GetShortPayload( const TraceSessionCanonicalRecord& record,
    const uint8_t*& data, size_t& size, std::string& error )
{
    const auto fixed = size_t( QueueDataSize[record.type] );
    if( record.payload.size() < fixed + sizeof( uint16_t ) )
    { error = "session_sampling_payload_truncated"; return false; }
    uint16_t bytes = 0;
    std::memcpy( &bytes, record.payload.data() + fixed, sizeof( bytes ) );
    if( bytes != record.variablePayloadBytes ||
        record.payload.size() != fixed + sizeof( bytes ) + bytes )
    { error = "session_sampling_payload_mismatch"; return false; }
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

void AddThread( StoredSampleBlock& block, uint64_t thread )
{
    const auto mixed = MixThread( thread );
    const std::array<uint8_t, 3> bits = {
        uint8_t( mixed ), uint8_t( mixed >> 21 ), uint8_t( mixed >> 42 ) };
    for( const auto bit : bits ) block.threadBloom[bit >> 6] |= uint64_t( 1 ) << ( bit & 63 );
}

bool MayContainThread( const StoredSampleBlock& block, uint64_t thread )
{
    const auto mixed = MixThread( thread );
    const std::array<uint8_t, 3> bits = {
        uint8_t( mixed ), uint8_t( mixed >> 21 ), uint8_t( mixed >> 42 ) };
    for( const auto bit : bits )
        if( ( block.threadBloom[bit >> 6] & ( uint64_t( 1 ) << ( bit & 63 ) ) ) == 0 )
            return false;
    return true;
}

bool CopyFileBytes( const std::filesystem::path& source, std::ofstream& out,
    std::string& error )
{
    std::ifstream in( source, std::ios::binary );
    if( !in ) { error = "session_sampling_work_read_failed"; return false; }
    std::vector<char> buffer( 1024 * 1024 );
    while( in )
    {
        in.read( buffer.data(), std::streamsize( buffer.size() ) );
        const auto count = in.gcount();
        if( count > 0 ) out.write( buffer.data(), count );
    }
    if( !in.eof() || !out ) { error = "session_sampling_work_copy_failed"; return false; }
    return true;
}

class ThreadWork
{
public:
    bool Open( const std::filesystem::path& root, uint64_t thread, std::string& error )
    {
        m_thread = thread;
        std::ostringstream prefix;
        prefix << "thread-" << std::hex << std::setw( 16 ) << std::setfill( '0' ) << thread;
        m_samplePath = root / ( prefix.str() + "-sample.work" );
        m_contextPath = root / ( prefix.str() + "-context.work" );
        m_samples.open( m_samplePath, std::ios::binary | std::ios::trunc );
        m_context.open( m_contextPath, std::ios::binary | std::ios::trunc );
        if( !m_samples || !m_context )
        { error = "session_sampling_thread_work_open_failed"; return false; }
        return true;
    }

    bool Append( int64_t timeNs, uint32_t callstack, bool contextSwitch,
        std::string& error )
    {
        StoredSample value;
        value.timeNs = timeNs; value.thread = m_thread; value.callstack = callstack;
        value.kind = contextSwitch ? 1 : 0;
        auto& out = contextSwitch ? m_context : m_samples;
        out.write( reinterpret_cast<const char*>( &value ), sizeof( value ) );
        if( !out ) { error = "session_sampling_thread_work_write_failed"; return false; }
        if( contextSwitch ) ++m_contextCount; else ++m_sampleCount;
        return true;
    }

    bool Close( std::string& error )
    {
        m_samples.flush(); m_context.flush();
        if( !m_samples || !m_context )
        { error = "session_sampling_thread_work_flush_failed"; return false; }
        m_samples.close(); m_context.close();
        return true;
    }

    uint64_t SampleCount() const { return m_sampleCount; }
    uint64_t ContextCount() const { return m_contextCount; }
    const std::filesystem::path& SamplePath() const { return m_samplePath; }
    const std::filesystem::path& ContextPath() const { return m_contextPath; }

private:
    uint64_t m_thread = 0;
    uint64_t m_sampleCount = 0;
    uint64_t m_contextCount = 0;
    std::filesystem::path m_samplePath;
    std::filesystem::path m_contextPath;
    std::ofstream m_samples;
    std::ofstream m_context;
};

struct BuildState
{
    TraceSessionTimeTransform transform;
    std::filesystem::path root;
    std::unordered_map<std::string, uint32_t> callstackIds;
    std::unordered_map<uint32_t, uint32_t> sampleDictionary;
    std::map<uint64_t, std::unique_ptr<ThreadWork>> threads;
    std::filesystem::path hardwareWorkPath;
    std::ofstream hardwareWork;
    uint64_t hardwareOrdinal = 0;
    uint32_t pendingCallstack = 0;
    uint32_t serialNextCallstack = 0;
    int64_t contextTime = 0;
    TraceSessionSamplingStats stats;
};

uint32_t InternCallstack( BuildState& state, const uint8_t* data,
    size_t size, std::string& error )
{
    if( size % sizeof( uint64_t ) != 0 )
    { error = "session_sampling_callstack_payload_invalid"; return 0; }
    std::string key( reinterpret_cast<const char*>( data ), size );
    const auto found = state.callstackIds.find( key );
    if( found != state.callstackIds.end() ) return found->second;
    if( state.callstackIds.size() >= std::numeric_limits<uint32_t>::max() - 1 )
    { error = "session_sampling_callstack_id_overflow"; return 0; }
    const auto id = uint32_t( state.callstackIds.size() + 1 );
    state.callstackIds.emplace( std::move( key ), id );
    return id;
}

ThreadWork* EnsureThread( BuildState& state, uint64_t thread, std::string& error )
{
    const auto found = state.threads.find( thread );
    if( found != state.threads.end() ) return found->second.get();
    auto work = std::make_unique<ThreadWork>();
    if( !work->Open( state.root, thread, error ) ) return nullptr;
    return state.threads.emplace( thread, std::move( work ) ).first->second.get();
}

bool ConsumeSerialCallstack( BuildState& state, std::string& error )
{
    if( state.serialNextCallstack == 0 )
    { error = "session_sampling_serial_callstack_missing"; return false; }
    state.serialNextCallstack = 0;
    return true;
}

bool ConsumePendingCallstack( BuildState& state, uint32_t& callstack,
    std::string& error )
{
    if( state.pendingCallstack == 0 )
    { error = "session_sampling_pending_callstack_missing"; return false; }
    callstack = state.pendingCallstack;
    state.pendingCallstack = 0;
    return true;
}

int64_t AdvanceContext( BuildState& state, int64_t delta )
{
    state.contextTime += delta;
    return state.transform.ToNanoseconds( state.contextTime );
}

bool AppendSample( BuildState& state, const QueueCallstackSample& sample,
    uint32_t callstack, bool contextSwitch, std::string& error )
{
    auto* thread = EnsureThread( state, sample.thread, error );
    if( !thread ) return false;
    if( !thread->Append( AdvanceContext( state, sample.time ), callstack,
        contextSwitch, error ) ) return false;
    ++state.stats.events;
    if( contextSwitch ) ++state.stats.contextSwitchSamples;
    else ++state.stats.samples;
    return true;
}

int HardwareKind( QueueType type )
{
    switch( type )
    {
    case QueueType::HwSampleCpuCycle: return 0;
    case QueueType::HwSampleInstructionRetired: return 1;
    case QueueType::HwSampleCacheReference: return 2;
    case QueueType::HwSampleCacheMiss: return 3;
    case QueueType::HwSampleBranchRetired: return 4;
    case QueueType::HwSampleBranchMiss: return 5;
    default: return -1;
    }
}

const char* HardwareKindName( uint8_t kind )
{
    static constexpr const char* Names[] = {
        "cycles", "retired", "cache_references", "cache_misses",
        "branch_retired", "branch_misses"
    };
    return kind < std::size( Names ) ? Names[kind] : "unknown";
}

bool AppendHardwareSample( BuildState& state, const QueueHwSample& sample,
    uint8_t kind, std::string& error )
{
    if( !state.hardwareWork.is_open() )
    {
        state.hardwareWorkPath = state.root / "hardware-samples.work";
        state.hardwareWork.open( state.hardwareWorkPath,
            std::ios::binary | std::ios::trunc );
        if( !state.hardwareWork )
        { error = "session_sampling_hardware_work_open_failed"; return false; }
    }
    StoredHardwareSample value;
    value.address = sample.ip;
    value.timeNs = sample.time == 0 ? 0 : state.transform.ToNanoseconds( sample.time );
    value.sourceOrdinal = state.hardwareOrdinal++;
    value.kind = kind;
    state.hardwareWork.write( reinterpret_cast<const char*>( &value ), sizeof( value ) );
    if( !state.hardwareWork )
    { error = "session_sampling_hardware_work_write_failed"; return false; }
    ++state.stats.hardwareEvents;
    return true;
}

bool VisitSamplingRecord( const TraceSessionCanonicalRecord& record,
    void* userData, std::string& error )
{
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ) return true;
    auto& state = *static_cast<BuildState*>( userData );
    QueueItem item {};
    if( !DecodeItem( record, item, error ) ) return false;
    const auto type = QueueType( record.type );
    const auto hardwareKind = HardwareKind( type );
    if( hardwareKind >= 0 ) return AppendHardwareSample(
        state, item.hwSample, uint8_t( hardwareKind ), error );
    switch( type )
    {
    case QueueType::CallstackPayload:
    {
        const uint8_t* data = nullptr; size_t size = 0;
        if( !GetShortPayload( record, data, size, error ) || state.pendingCallstack != 0 )
        { if( error.empty() ) error = "session_sampling_callstack_sequence_invalid"; return false; }
        state.pendingCallstack = InternCallstack( state, data, size, error );
        if( state.pendingCallstack == 0 ) return false;
        ++state.stats.callstackPayloads;
        break;
    }
    case QueueType::CallstackSampleDictionary:
    {
        const uint8_t* data = nullptr; size_t size = 0;
        if( !GetShortPayload( record, data, size, error ) ) return false;
        const auto callstack = InternCallstack( state, data, size, error );
        if( callstack == 0 || item.stringTransfer.ptr == 0 ||
            !state.sampleDictionary.emplace( uint32_t( item.stringTransfer.ptr ), callstack ).second )
        { if( error.empty() ) error = "session_sampling_dictionary_invalid"; return false; }
        ++state.stats.dictionaryEntries;
        break;
    }
    case QueueType::CallstackSerial:
        if( state.pendingCallstack == 0 || state.serialNextCallstack != 0 )
        { error = "session_sampling_callstack_serial_sequence_invalid"; return false; }
        state.serialNextCallstack = state.pendingCallstack;
        state.pendingCallstack = 0;
        break;
    case QueueType::Callstack:
    case QueueType::CallstackAlloc:
    {
        uint32_t ignored = 0;
        if( !ConsumePendingCallstack( state, ignored, error ) ) return false;
        break;
    }
    case QueueType::CallstackSample:
    case QueueType::CallstackSampleContextSwitch:
    {
        uint32_t callstack = 0;
        if( !ConsumePendingCallstack( state, callstack, error ) ) return false;
        if( !AppendSample( state, item.callstackSample, callstack,
            type == QueueType::CallstackSampleContextSwitch, error ) ) return false;
        break;
    }
    case QueueType::CallstackSampleRef:
    case QueueType::CallstackSampleContextSwitchRef:
    {
        if( state.pendingCallstack != 0 )
        { error = "session_sampling_ref_with_pending_callstack"; return false; }
        const auto found = state.sampleDictionary.find( item.callstackSampleRef.stackId );
        if( found == state.sampleDictionary.end() || found->second == 0 )
        { error = "session_sampling_dictionary_reference_missing"; return false; }
        if( !AppendSample( state, item.callstackSampleRef, found->second,
            type == QueueType::CallstackSampleContextSwitchRef, error ) ) return false;
        break;
    }
    case QueueType::ContextSwitch:
        (void)AdvanceContext( state, item.contextSwitch.time );
        break;
    case QueueType::ThreadWakeup:
        (void)AdvanceContext( state, item.threadWakeup.time );
        break;
    case QueueType::GpuZoneBeginCallstackSerial:
    case QueueType::GpuZoneBeginAllocSrcLocCallstackSerial:
    case QueueType::MemAllocCallstack:
    case QueueType::MemAllocCallstackNamed:
    case QueueType::MemFreeCallstack:
    case QueueType::MemFreeCallstackNamed:
    case QueueType::MemDiscardCallstack:
        if( !ConsumeSerialCallstack( state, error ) ) return false;
        break;
    case QueueType::JnCallsiteDefinition:
        if( item.jnCallsiteDefinition.flags & uint8_t( JnCallsiteFlags::HasCallstack ) )
        {
            if( !ConsumeSerialCallstack( state, error ) ) return false;
        }
        else if( state.serialNextCallstack != 0 )
        { error = "session_sampling_callsite_unexpected_callstack"; return false; }
        break;
    case QueueType::JnJobStage:
        if( JnJobStage( item.jnJobStage.stage ) == JnJobStage::ScheduleCallstack ||
            JnJobStage( item.jnJobStage.stage ) == JnJobStage::WaitCallstack )
            if( !ConsumeSerialCallstack( state, error ) ) return false;
        break;
    case QueueType::JnIoStage:
        if( JnIoStage( item.jnIoStage.stage ) == JnIoStage::RequestCallstack )
            if( !ConsumeSerialCallstack( state, error ) ) return false;
        break;
    default: break;
    }
    return true;
}

bool SaveSamplingManifest( const std::filesystem::path& root,
    const SamplingManifest& value, std::string& error )
{
    const auto temporary = root / "manifest.tmp";
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_sampling_manifest_open_failed"; return false; }
    out << "magic " << SamplingManifestMagic << '\n';
    out << "schema " << TraceSessionSamplingIndexSchemaVersion << '\n';
    out << "source_sha256 " << std::quoted( value.sourceSha256 ) << '\n';
    out << "source_size " << value.sourceSize << '\n';
    out << "generation " << std::quoted( value.generation ) << '\n';
    out << "file_bytes " << value.fileBytes << '\n';
    out << "file_sha256 " << std::quoted( value.fileSha256 ) << '\n';
    out << "events " << value.stats.events << '\n';
    out << "samples " << value.stats.samples << '\n';
    out << "context_switch_samples " << value.stats.contextSwitchSamples << '\n';
    out << "dictionary_entries " << value.stats.dictionaryEntries << '\n';
    out << "callstack_payloads " << value.stats.callstackPayloads << '\n';
    out << "hardware_events " << value.stats.hardwareEvents << '\n';
    out << "hardware_addresses " << value.stats.hardwareAddresses << '\n';
    out << "sample_blocks " << value.stats.sampleBlocks << '\n';
    out.flush();
    if( !out ) { error = "session_sampling_manifest_write_failed"; return false; }
    out.close();
    return AtomicReplace( temporary, root / "manifest", error );
}

bool LoadSamplingManifest( const std::filesystem::path& root,
    SamplingManifest& value, std::string& error )
{
    value = {};
    std::ifstream in( root / "manifest", std::ios::binary );
    if( !in ) { error = "session_sampling_manifest_not_found"; return false; }
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
        else if( key == "events" ) in >> value.stats.events;
        else if( key == "samples" ) in >> value.stats.samples;
        else if( key == "context_switch_samples" ) in >> value.stats.contextSwitchSamples;
        else if( key == "dictionary_entries" ) in >> value.stats.dictionaryEntries;
        else if( key == "callstack_payloads" ) in >> value.stats.callstackPayloads;
        else if( key == "hardware_events" ) in >> value.stats.hardwareEvents;
        else if( key == "hardware_addresses" ) in >> value.stats.hardwareAddresses;
        else if( key == "sample_blocks" ) in >> value.stats.sampleBlocks;
        else { std::string ignored; std::getline( in, ignored ); }
        if( !in ) { error = "session_sampling_manifest_parse_failed"; return false; }
    }
    value.stats.fileBytes = value.fileBytes;
    if( magic != SamplingManifestMagic || schema != TraceSessionSamplingIndexSchemaVersion ||
        value.sourceSha256.size() != 64 || value.fileSha256.size() != 64 ||
        value.stats.events != value.stats.samples + value.stats.contextSwitchSamples )
    { error = "session_sampling_manifest_invalid"; return false; }
    return true;
}

bool HardwareSampleLess( const StoredHardwareSample& lhs,
    const StoredHardwareSample& rhs )
{
    return std::tie( lhs.address, lhs.kind, lhs.sourceOrdinal ) <
        std::tie( rhs.address, rhs.kind, rhs.sourceOrdinal );
}

bool BuildHardwareFiles( BuildState& state,
    std::filesystem::path& summaryPath, std::filesystem::path& eventPath,
    uint64_t& summaryCount, std::string& error )
{
    summaryCount = 0;
    summaryPath = state.root / "hardware-summaries.work";
    eventPath = state.root / "hardware-events.work";
    if( state.hardwareWork.is_open() )
    {
        state.hardwareWork.flush();
        if( !state.hardwareWork )
        { error = "session_sampling_hardware_work_flush_failed"; return false; }
        state.hardwareWork.close();
    }

    std::ofstream emptySummary( summaryPath, std::ios::binary | std::ios::trunc );
    std::ofstream emptyEvents( eventPath, std::ios::binary | std::ios::trunc );
    if( !emptySummary || !emptyEvents )
    { error = "session_sampling_hardware_output_open_failed"; return false; }
    emptySummary.close(); emptyEvents.close();
    if( state.stats.hardwareEvents == 0 ) return true;

    constexpr size_t SortChunkRecords = 1024 * 1024;
    std::ifstream input( state.hardwareWorkPath, std::ios::binary );
    if( !input ) { error = "session_sampling_hardware_work_read_failed"; return false; }
    std::vector<std::filesystem::path> runs;
    std::vector<StoredHardwareSample> chunk( SortChunkRecords );
    uint64_t readRecords = 0;
    while( readRecords < state.stats.hardwareEvents )
    {
        const auto wanted = size_t( std::min<uint64_t>(
            SortChunkRecords, state.stats.hardwareEvents - readRecords ) );
        input.read( reinterpret_cast<char*>( chunk.data() ),
            std::streamsize( wanted * sizeof( StoredHardwareSample ) ) );
        if( size_t( input.gcount() ) != wanted * sizeof( StoredHardwareSample ) )
        { error = "session_sampling_hardware_work_truncated"; return false; }
        std::sort( chunk.begin(), chunk.begin() + wanted, HardwareSampleLess );
        const auto runPath = state.root /
            ( "hardware-run-" + std::to_string( runs.size() ) + ".work" );
        std::ofstream run( runPath, std::ios::binary | std::ios::trunc );
        if( !run ) { error = "session_sampling_hardware_run_open_failed"; return false; }
        run.write( reinterpret_cast<const char*>( chunk.data() ),
            std::streamsize( wanted * sizeof( StoredHardwareSample ) ) );
        run.flush();
        if( !run ) { error = "session_sampling_hardware_run_write_failed"; return false; }
        runs.emplace_back( runPath );
        readRecords += wanted;
    }
    input.close();

    struct RunState
    {
        std::ifstream stream;
        StoredHardwareSample value;
        bool valid = false;
    };
    struct HeapEntry
    {
        StoredHardwareSample value;
        size_t run = 0;
    };
    const auto later = []( const HeapEntry& lhs, const HeapEntry& rhs ) {
        return HardwareSampleLess( rhs.value, lhs.value );
    };
    constexpr size_t MaxOpenRuns = 64;
    size_t mergePass = 0;
    while( runs.size() > MaxOpenRuns )
    {
        std::vector<std::filesystem::path> mergedRuns;
        for( size_t first = 0; first < runs.size(); first += MaxOpenRuns )
        {
            const auto groupCount = std::min( MaxOpenRuns, runs.size() - first );
            std::vector<RunState> group( groupCount );
            std::priority_queue<HeapEntry, std::vector<HeapEntry>, decltype( later )>
                groupHeap( later );
            for( size_t index = 0; index < groupCount; ++index )
            {
                group[index].stream.open( runs[first + index], std::ios::binary );
                if( !group[index].stream || !group[index].stream.read(
                    reinterpret_cast<char*>( &group[index].value ),
                    sizeof( StoredHardwareSample ) ) )
                { error = "session_sampling_hardware_run_read_failed"; return false; }
                groupHeap.push( { group[index].value, index } );
            }
            const auto mergedPath = state.root /
                ( "hardware-merge-" + std::to_string( mergePass ) + "-" +
                    std::to_string( mergedRuns.size() ) + ".work" );
            std::ofstream merged( mergedPath, std::ios::binary | std::ios::trunc );
            if( !merged )
            { error = "session_sampling_hardware_merge_open_failed"; return false; }
            while( !groupHeap.empty() )
            {
                const auto entry = groupHeap.top(); groupHeap.pop();
                merged.write( reinterpret_cast<const char*>( &entry.value ),
                    sizeof( StoredHardwareSample ) );
                auto& reader = group[entry.run];
                if( reader.stream.read( reinterpret_cast<char*>( &reader.value ),
                    sizeof( StoredHardwareSample ) ) )
                    groupHeap.push( { reader.value, entry.run } );
                else if( !reader.stream.eof() )
                { error = "session_sampling_hardware_run_read_failed"; return false; }
            }
            merged.flush();
            if( !merged )
            { error = "session_sampling_hardware_merge_write_failed"; return false; }
            merged.close();
            for( auto& reader : group ) reader.stream.close();
            std::error_code removeError;
            for( size_t index = 0; index < groupCount; ++index )
            {
                if( !std::filesystem::remove( runs[first + index], removeError ) || removeError )
                {
                    error = "session_sampling_hardware_merge_cleanup_failed:" +
                        ( removeError ? removeError.message() : runs[first + index].string() );
                    return false;
                }
                removeError.clear();
            }
            mergedRuns.emplace_back( mergedPath );
        }
        runs = std::move( mergedRuns );
        ++mergePass;
    }
    std::vector<RunState> readers( runs.size() );
    std::priority_queue<HeapEntry, std::vector<HeapEntry>, decltype( later )> heap( later );
    for( size_t i = 0; i < runs.size(); ++i )
    {
        readers[i].stream.open( runs[i], std::ios::binary );
        if( !readers[i].stream || !readers[i].stream.read(
            reinterpret_cast<char*>( &readers[i].value ), sizeof( StoredHardwareSample ) ) )
        { error = "session_sampling_hardware_run_read_failed"; return false; }
        readers[i].valid = true;
        heap.push( { readers[i].value, i } );
    }

    std::ofstream summaries( summaryPath, std::ios::binary | std::ios::trunc );
    std::ofstream events( eventPath, std::ios::binary | std::ios::trunc );
    if( !summaries || !events )
    { error = "session_sampling_hardware_output_open_failed"; return false; }
    StoredHardwareSummary summary;
    bool haveSummary = false;
    uint64_t eventOrdinal = 0;
    while( !heap.empty() )
    {
        const auto entry = heap.top(); heap.pop();
        const auto& value = entry.value;
        if( !haveSummary || summary.address != value.address )
        {
            if( haveSummary )
            {
                summaries.write( reinterpret_cast<const char*>( &summary ), sizeof( summary ) );
                if( !summaries )
                { error = "session_sampling_hardware_summary_write_failed"; return false; }
                ++summaryCount;
            }
            summary = {}; summary.address = value.address; haveSummary = true;
        }
        if( value.kind >= 6 )
        { error = "session_sampling_hardware_kind_invalid"; return false; }
        if( summary.counts[value.kind] == 0 ) summary.eventOffsets[value.kind] = eventOrdinal;
        ++summary.counts[value.kind];
        events.write( reinterpret_cast<const char*>( &value ), sizeof( value ) );
        if( !events )
        { error = "session_sampling_hardware_event_write_failed"; return false; }
        ++eventOrdinal;

        auto& reader = readers[entry.run];
        if( reader.stream.read( reinterpret_cast<char*>( &reader.value ),
            sizeof( StoredHardwareSample ) ) )
        {
            heap.push( { reader.value, entry.run } );
        }
        else if( !reader.stream.eof() )
        { error = "session_sampling_hardware_run_read_failed"; return false; }
    }
    if( haveSummary )
    {
        summaries.write( reinterpret_cast<const char*>( &summary ), sizeof( summary ) );
        ++summaryCount;
    }
    summaries.flush(); events.flush();
    if( !summaries || !events || eventOrdinal != state.stats.hardwareEvents )
    { error = "session_sampling_hardware_merge_failed"; return false; }
    summaries.close(); events.close();
    for( auto& reader : readers ) reader.stream.close();

    std::error_code ec;
    std::filesystem::remove( state.hardwareWorkPath, ec ); ec.clear();
    for( const auto& run : runs ) { std::filesystem::remove( run, ec ); ec.clear(); }
    return true;
}

bool BuildSampleBlocks( const std::filesystem::path& recordsPath,
    uint64_t recordCount, std::vector<StoredSampleBlock>& blocks,
    std::string& error )
{
    constexpr uint32_t RecordsPerBlock = 4096;
    blocks.clear();
    if( recordCount == 0 ) return true;
    std::ifstream in( recordsPath, std::ios::binary );
    if( !in ) { error = "session_sampling_block_source_open_failed"; return false; }
    blocks.reserve( size_t( ( recordCount + RecordsPerBlock - 1 ) / RecordsPerBlock ) );
    uint64_t first = 0;
    while( first < recordCount )
    {
        StoredSampleBlock block;
        block.firstRecord = first;
        block.recordCount = uint32_t( std::min<uint64_t>(
            RecordsPerBlock, recordCount - first ) );
        block.minTimeNs = std::numeric_limits<int64_t>::max();
        block.maxTimeNs = std::numeric_limits<int64_t>::min();
        for( uint32_t index = 0; index < block.recordCount; ++index )
        {
            StoredSample value;
            if( !in.read( reinterpret_cast<char*>( &value ), sizeof( value ) ) )
            { error = "session_sampling_block_source_truncated"; return false; }
            block.minTimeNs = std::min( block.minTimeNs, value.timeNs );
            block.maxTimeNs = std::max( block.maxTimeNs, value.timeNs );
            AddThread( block, value.thread );
        }
        blocks.emplace_back( block );
        first += block.recordCount;
    }
    char trailing = 0;
    if( in.read( &trailing, 1 ) )
    { error = "session_sampling_block_source_trailing_bytes"; return false; }
    return true;
}

bool FinalizeSamplingFile( BuildState& state, const TraceSessionManifest& session,
    SamplingManifest& manifest, std::string& error )
{
    for( auto& [_, thread] : state.threads ) if( !thread->Close( error ) ) return false;
    std::filesystem::path hardwareSummaryPath, hardwareEventPath;
    uint64_t hardwareSummaryCount = 0;
    if( !BuildHardwareFiles( state, hardwareSummaryPath, hardwareEventPath,
        hardwareSummaryCount, error ) ) return false;
    state.stats.hardwareAddresses = hardwareSummaryCount;
    const auto recordsPath = state.root / "sample-records.work";
    {
        std::ofstream records( recordsPath, std::ios::binary | std::ios::trunc );
        if( !records ) { error = "session_sampling_records_open_failed"; return false; }
        for( const auto& [_, thread] : state.threads )
        {
            if( !CopyFileBytes( thread->SamplePath(), records, error ) ||
                !CopyFileBytes( thread->ContextPath(), records, error ) ) return false;
        }
        records.flush();
        if( !records ) { error = "session_sampling_records_write_failed"; return false; }
    }
    std::vector<StoredSampleBlock> sampleBlocks;
    if( !BuildSampleBlocks( recordsPath, state.stats.events, sampleBlocks, error ) ) return false;
    state.stats.sampleBlocks = sampleBlocks.size();
    SamplingFileHeader header;
    header.sourceSize = session.source.fileSize;
    header.eventCount = state.stats.events;
    header.sampleCount = state.stats.samples;
    header.contextSwitchSampleCount = state.stats.contextSwitchSamples;
    header.dictionaryEntries = state.stats.dictionaryEntries;
    header.callstackPayloads = state.stats.callstackPayloads;
    header.hardwareSummaryCount = state.stats.hardwareAddresses;
    header.hardwareEventCount = state.stats.hardwareEvents;
    header.sampleBlockCount = state.stats.sampleBlocks;
    header.generationBytes = uint32_t( session.generation.size() );
    header.recordsOffset = sizeof( header ) + session.source.sha256.size() + session.generation.size();
    header.sampleBlocksOffset = header.recordsOffset +
        header.eventCount * sizeof( StoredSample );
    header.hardwareSummariesOffset = header.sampleBlocksOffset +
        header.sampleBlockCount * sizeof( StoredSampleBlock );
    header.hardwareEventsOffset = header.hardwareSummariesOffset +
        header.hardwareSummaryCount * sizeof( StoredHardwareSummary );

    const auto temporary = state.root / "samples.bin.tmp";
    const auto target = state.root / SamplingFileName;
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_sampling_file_open_failed"; return false; }
    out.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
    out.write( session.source.sha256.data(), std::streamsize( session.source.sha256.size() ) );
    out.write( session.generation.data(), std::streamsize( session.generation.size() ) );
    // The records work file already matches WorkerTraceSource ordering exactly:
    // threads by native id, then regular samples followed by context-switch samples.
    if( !CopyFileBytes( recordsPath, out, error ) ) return false;
    if( !sampleBlocks.empty() ) out.write(
        reinterpret_cast<const char*>( sampleBlocks.data() ),
        std::streamsize( sampleBlocks.size() * sizeof( StoredSampleBlock ) ) );
    if( !CopyFileBytes( hardwareSummaryPath, out, error ) ||
        !CopyFileBytes( hardwareEventPath, out, error ) ) return false;
    out.flush();
    if( !out ) { error = "session_sampling_file_write_failed"; return false; }
    out.close();
    if( !AtomicReplace( temporary, target, error ) ) return false;
    std::error_code ec;
    for( const auto& [_, thread] : state.threads )
    {
        std::filesystem::remove( thread->SamplePath(), ec ); ec.clear();
        std::filesystem::remove( thread->ContextPath(), ec ); ec.clear();
    }
    std::filesystem::remove( hardwareSummaryPath, ec ); ec.clear();
    std::filesystem::remove( hardwareEventPath, ec ); ec.clear();
    std::filesystem::remove( recordsPath, ec ); ec.clear();

    manifest.sourceSha256 = session.source.sha256;
    manifest.sourceSize = session.source.fileSize;
    manifest.generation = session.generation;
    manifest.fileBytes = std::filesystem::file_size( target, ec );
    if( ec ) { error = "session_sampling_file_size_failed:" + ec.message(); return false; }
    manifest.fileSha256 = Sha256File( target );
    manifest.stats = state.stats;
    manifest.stats.fileBytes = manifest.fileBytes;
    return true;
}

}

struct TraceSessionSamplingReader::Impl
{
    std::filesystem::path path;
    std::string fingerprint;
    uint64_t recordsOffset = 0;
    uint64_t eventCount = 0;
    std::vector<StoredSampleBlock> sampleBlocks;
    uint64_t hardwareEventsOffset = 0;
    std::vector<StoredHardwareSummary> hardwareSummaries;
};

TraceSessionSamplingReader::TraceSessionSamplingReader( std::shared_ptr<Impl> impl )
    : m_impl( std::move( impl ) )
{}

std::filesystem::path TraceSessionSamplingIndexRoot( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest )
{
    return sessionRoot / "generations" / manifest.generation / "derived" /
        "sampling-index" / std::to_string( TraceSessionSamplingIndexSchemaVersion ) / "exact";
}

bool BuildTraceSessionSamplingDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionSamplingStats& stats, std::string& error )
{
    error.clear(); stats = {};
    BuildState state;
    state.root = TraceSessionSamplingIndexRoot( sessionRoot, manifest );
    std::error_code ec; std::filesystem::create_directories( state.root, ec );
    if( ec ) { error = "session_sampling_directory_failed:" + ec.message(); return false; }
    if( !LoadTraceSessionTimeTransform( sessionRoot, manifest, state.transform, error ) ) return false;
    if( !VisitTraceSessionCanonicalOrdered( sessionRoot, manifest,
        VisitSamplingRecord, &state, error ) ) return false;
    if( state.pendingCallstack != 0 || state.serialNextCallstack != 0 )
    { error = "session_sampling_pending_protocol_state"; return false; }
    SamplingManifest samplingManifest;
    if( !FinalizeSamplingFile( state, manifest, samplingManifest, error ) ) return false;
    if( !SaveSamplingManifest( state.root, samplingManifest, error ) ) return false;
    stats = samplingManifest.stats;
    return true;
}

bool AuditTraceSessionSamplingDerived( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& session, TraceSessionSamplingStats& stats, std::string& error )
{
    error.clear(); stats = {};
    const auto root = TraceSessionSamplingIndexRoot( sessionRoot, session );
    SamplingManifest manifest;
    if( !LoadSamplingManifest( root, manifest, error ) ) return false;
    if( manifest.sourceSha256 != session.source.sha256 || manifest.sourceSize != session.source.fileSize ||
        manifest.generation != session.generation )
    { error = "session_sampling_identity_mismatch"; return false; }
    const auto path = root / SamplingFileName;
    std::error_code ec;
    if( std::filesystem::file_size( path, ec ) != manifest.fileBytes || ec )
    { error = "session_sampling_file_size_mismatch"; return false; }
    if( Sha256File( path ) != manifest.fileSha256 )
    { error = "session_sampling_file_sha256_mismatch"; return false; }
    stats = manifest.stats;
    return true;
}

std::shared_ptr<TraceSessionSamplingReader> TraceSessionSamplingReader::Open(
    const std::filesystem::path& sessionRoot, const TraceSessionManifest& session,
    std::string& error )
{
    error.clear();
    const auto root = TraceSessionSamplingIndexRoot( sessionRoot, session );
    SamplingManifest manifest;
    if( !LoadSamplingManifest( root, manifest, error ) ) return {};
    if( manifest.sourceSha256 != session.source.sha256 || manifest.sourceSize != session.source.fileSize ||
        manifest.generation != session.generation )
    { error = "session_sampling_identity_mismatch"; return {}; }
    const auto path = root / SamplingFileName;
    std::error_code ec;
    if( std::filesystem::file_size( path, ec ) != manifest.fileBytes || ec )
    { error = "session_sampling_file_size_mismatch"; return {}; }
    std::ifstream in( path, std::ios::binary );
    SamplingFileHeader header;
    if( !in.read( reinterpret_cast<char*>( &header ), sizeof( header ) ) ||
        header.magic != SamplingFileMagic || header.schema != TraceSessionSamplingIndexSchemaVersion ||
        header.endian != 0x01020304 || header.sourceSize != session.source.fileSize ||
        header.eventCount != manifest.stats.events || header.sampleCount != manifest.stats.samples ||
        header.contextSwitchSampleCount != manifest.stats.contextSwitchSamples ||
        header.dictionaryEntries != manifest.stats.dictionaryEntries ||
        header.callstackPayloads != manifest.stats.callstackPayloads ||
        header.hardwareEventCount != manifest.stats.hardwareEvents ||
        header.hardwareSummaryCount != manifest.stats.hardwareAddresses ||
        header.sampleBlockCount != manifest.stats.sampleBlocks ||
        header.generationBytes != session.generation.size() )
    { error = "session_sampling_file_header_invalid"; return {}; }
    std::string sha( 64, '\0' ), generation( header.generationBytes, '\0' );
    if( !in.read( sha.data(), std::streamsize( sha.size() ) ) ||
        !in.read( generation.data(), std::streamsize( generation.size() ) ) ||
        sha != session.source.sha256 || generation != session.generation ||
        header.recordsOffset != uint64_t( in.tellg() ) ||
        header.eventCount > ( manifest.fileBytes - header.recordsOffset ) / sizeof( StoredSample ) ||
        header.sampleBlocksOffset != header.recordsOffset +
            header.eventCount * sizeof( StoredSample ) ||
        header.hardwareSummariesOffset != header.sampleBlocksOffset +
            header.sampleBlockCount * sizeof( StoredSampleBlock ) ||
        header.hardwareEventsOffset != header.hardwareSummariesOffset +
            header.hardwareSummaryCount * sizeof( StoredHardwareSummary ) ||
        header.hardwareEventsOffset + header.hardwareEventCount *
            sizeof( StoredHardwareSample ) != manifest.fileBytes )
    { error = "session_sampling_file_identity_or_bounds_invalid"; return {}; }
    auto impl = std::make_shared<Impl>();
    impl->path = path; impl->fingerprint = session.source.sha256;
    impl->recordsOffset = header.recordsOffset; impl->eventCount = header.eventCount;
    impl->hardwareEventsOffset = header.hardwareEventsOffset;
    impl->sampleBlocks.resize( size_t( header.sampleBlockCount ) );
    in.seekg( std::streamoff( header.sampleBlocksOffset ) );
    if( header.sampleBlockCount != 0 && !in.read(
        reinterpret_cast<char*>( impl->sampleBlocks.data() ),
        std::streamsize( header.sampleBlockCount * sizeof( StoredSampleBlock ) ) ) )
    { error = "session_sampling_blocks_truncated"; return {}; }
    uint64_t expectedSampleRecord = 0;
    for( const auto& block : impl->sampleBlocks )
    {
        if( block.firstRecord != expectedSampleRecord || block.recordCount == 0 ||
            block.minTimeNs > block.maxTimeNs )
        { error = "session_sampling_block_index_invalid"; return {}; }
        expectedSampleRecord += block.recordCount;
    }
    if( expectedSampleRecord != header.eventCount )
    { error = "session_sampling_block_index_invalid"; return {}; }
    impl->hardwareSummaries.resize( size_t( header.hardwareSummaryCount ) );
    if( header.hardwareSummaryCount != 0 )
    {
        in.seekg( std::streamoff( header.hardwareSummariesOffset ) );
        if( !in.read( reinterpret_cast<char*>( impl->hardwareSummaries.data() ),
            std::streamsize( header.hardwareSummaryCount * sizeof( StoredHardwareSummary ) ) ) )
        { error = "session_sampling_hardware_summaries_truncated"; return {}; }
        for( size_t i = 0; i < impl->hardwareSummaries.size(); ++i )
        {
            const auto& summary = impl->hardwareSummaries[i];
            if( i != 0 && impl->hardwareSummaries[i-1].address >= summary.address )
            { error = "session_sampling_hardware_summary_order_invalid"; return {}; }
            uint64_t count = 0;
            for( size_t kind = 0; kind < 6; ++kind )
            {
                if( summary.counts[kind] != 0 &&
                    summary.eventOffsets[kind] + summary.counts[kind] > header.hardwareEventCount )
                { error = "session_sampling_hardware_summary_bounds_invalid"; return {}; }
                count += summary.counts[kind];
            }
            if( count == 0 )
            { error = "session_sampling_hardware_summary_empty"; return {}; }
        }
    }
    auto reader = std::shared_ptr<TraceSessionSamplingReader>(
        new TraceSessionSamplingReader( std::move( impl ) ) );
    reader->m_stats = manifest.stats;
    return reader;
}

std::vector<SampleDto> TraceSessionSamplingReader::ScanImpl( const ScanRange& range,
    std::optional<uint64_t> thread ) const
{
    std::vector<SampleDto> result;
    std::ifstream in( m_impl->path, std::ios::binary );
    if( !in ) return result;
    size_t skipped = 0;
    for( const auto& block : m_impl->sampleBlocks )
    {
        if( block.minTimeNs >= range.endNs || block.maxTimeNs < range.startNs ||
            ( thread && !MayContainThread( block, *thread ) ) ) continue;
        in.clear();
        in.seekg( std::streamoff( m_impl->recordsOffset +
            block.firstRecord * sizeof( StoredSample ) ) );
        for( uint32_t index = 0; index < block.recordCount; ++index )
        {
            StoredSample value;
            if( !in.read( reinterpret_cast<char*>( &value ), sizeof( value ) ) ) return result;
            if( thread && value.thread != *thread ) continue;
            if( value.timeNs < range.startNs || value.timeNs >= range.endNs ) continue;
            if( skipped++ < range.offset ) continue;
            const auto ordinal = block.firstRecord + index;
            SampleDto dto;
            dto.ref = MakeRef( m_impl->fingerprint,
                value.kind == 0 ? "sample" : "context-switch-sample", ordinal );
            dto.threadRef = MakeRef( m_impl->fingerprint, "thread", value.thread );
            dto.timeNs = value.timeNs; dto.callstack = value.callstack;
            if( dto.callstack != 0 ) dto.callstackRef =
                MakeRef( m_impl->fingerprint, "callstack", dto.callstack );
            dto.kind = value.kind == 0 ? "sample" : "context_switch";
            result.emplace_back( std::move( dto ) );
            if( result.size() >= range.limit ) return result;
        }
    }
    return result;
}

std::vector<SampleDto> TraceSessionSamplingReader::Scan( const ScanRange& range ) const
{
    return ScanImpl( range, std::nullopt );
}

std::vector<SampleDto> TraceSessionSamplingReader::ScanThread(
    std::string_view threadRef, const ScanRange& range ) const
{
    const auto thread = ParseThreadRef( m_impl->fingerprint, threadRef );
    return thread ? ScanImpl( range, thread ) : std::vector<SampleDto> {};
}

std::vector<HardwareSampleDto> TraceSessionSamplingReader::HardwareSamples() const
{
    std::vector<HardwareSampleDto> result;
    result.reserve( m_impl->hardwareSummaries.size() );
    for( const auto& value : m_impl->hardwareSummaries )
    {
        std::ostringstream address;
        address << "0x" << std::hex << value.address;
        HardwareSampleDto dto;
        dto.ref = MakeRef( m_impl->fingerprint, "hardware-sample", value.address );
        dto.address = address.str();
        dto.cycles = value.counts[0];
        dto.retired = value.counts[1];
        dto.cacheReferences = value.counts[2];
        dto.cacheMisses = value.counts[3];
        dto.branchRetired = value.counts[4];
        dto.branchMisses = value.counts[5];
        result.emplace_back( std::move( dto ) );
    }
    return result;
}

std::vector<HardwareSampleEventDto> TraceSessionSamplingReader::HardwareSampleEvents(
    uint64_t addressValue, std::string_view requestedKind,
    size_t offset, size_t limit ) const
{
    std::vector<HardwareSampleEventDto> result;
    if( limit == 0 ) return result;
    const auto summary = std::lower_bound( m_impl->hardwareSummaries.begin(),
        m_impl->hardwareSummaries.end(), addressValue,
        []( const auto& value, uint64_t address ) { return value.address < address; } );
    if( summary == m_impl->hardwareSummaries.end() || summary->address != addressValue )
        return result;

    int onlyKind = -1;
    if( requestedKind != "all" )
    {
        for( int kind = 0; kind < 6; ++kind )
            if( requestedKind == HardwareKindName( uint8_t( kind ) ) ) onlyKind = kind;
        if( onlyKind < 0 ) return result;
    }
    std::ostringstream address;
    address << "0x" << std::hex << addressValue;
    const auto addressText = address.str();
    std::ifstream in( m_impl->path, std::ios::binary );
    if( !in ) return result;
    size_t skipped = 0;
    for( uint8_t kind = 0; kind < 6; ++kind )
    {
        if( onlyKind >= 0 && kind != onlyKind ) continue;
        const auto count = summary->counts[kind];
        for( uint64_t index = 0; index < count; ++index )
        {
            if( skipped++ < offset ) continue;
            if( result.size() >= limit ) return result;
            const auto eventOrdinal = summary->eventOffsets[kind] + index;
            in.seekg( std::streamoff( m_impl->hardwareEventsOffset +
                eventOrdinal * sizeof( StoredHardwareSample ) ) );
            StoredHardwareSample value;
            if( !in.read( reinterpret_cast<char*>( &value ), sizeof( value ) ) ||
                value.address != addressValue || value.kind != kind ) return result;
            HardwareSampleEventDto dto;
            dto.ref = MakeRef( m_impl->fingerprint, "hardware-sample", addressValue ) +
                ':' + HardwareKindName( kind ) + ':' + std::to_string( index );
            dto.address = addressText;
            dto.kind = HardwareKindName( kind );
            dto.eventIndex = index;
            dto.timeNs = value.timeNs;
            result.emplace_back( std::move( dto ) );
        }
    }
    return result;
}

}

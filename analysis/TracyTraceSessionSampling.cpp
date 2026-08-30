#include "TracyTraceSessionSampling.hpp"

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

bool VisitSamplingRecord( const TraceSessionCanonicalRecord& record,
    void* userData, std::string& error )
{
    if( record.kind != TraceSessionCanonicalRecordKind::ProtocolEvent ) return true;
    auto& state = *static_cast<BuildState*>( userData );
    QueueItem item {};
    if( !DecodeItem( record, item, error ) ) return false;
    const auto type = QueueType( record.type );
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

bool FinalizeSamplingFile( BuildState& state, const TraceSessionManifest& session,
    SamplingManifest& manifest, std::string& error )
{
    for( auto& [_, thread] : state.threads ) if( !thread->Close( error ) ) return false;
    SamplingFileHeader header;
    header.sourceSize = session.source.fileSize;
    header.eventCount = state.stats.events;
    header.sampleCount = state.stats.samples;
    header.contextSwitchSampleCount = state.stats.contextSwitchSamples;
    header.dictionaryEntries = state.stats.dictionaryEntries;
    header.callstackPayloads = state.stats.callstackPayloads;
    header.generationBytes = uint32_t( session.generation.size() );
    header.recordsOffset = sizeof( header ) + session.source.sha256.size() + session.generation.size();

    const auto temporary = state.root / "samples.bin.tmp";
    const auto target = state.root / SamplingFileName;
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_sampling_file_open_failed"; return false; }
    out.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
    out.write( session.source.sha256.data(), std::streamsize( session.source.sha256.size() ) );
    out.write( session.generation.data(), std::streamsize( session.generation.size() ) );
    // Match WorkerTraceSource ordering exactly: threads by native id, then
    // regular samples followed by context-switch samples for each thread.
    for( const auto& [_, thread] : state.threads )
    {
        if( !CopyFileBytes( thread->SamplePath(), out, error ) ) return false;
        if( !CopyFileBytes( thread->ContextPath(), out, error ) ) return false;
    }
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
};

TraceSessionSamplingReader::TraceSessionSamplingReader( std::shared_ptr<Impl> impl )
    : m_impl( std::move( impl ) )
{}

std::filesystem::path TraceSessionSamplingIndexRoot( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest )
{
    return sessionRoot / "generations" / manifest.generation / "derived" /
        "sampling-index" / "1" / "exact";
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
        header.generationBytes != session.generation.size() )
    { error = "session_sampling_file_header_invalid"; return {}; }
    std::string sha( 64, '\0' ), generation( header.generationBytes, '\0' );
    if( !in.read( sha.data(), std::streamsize( sha.size() ) ) ||
        !in.read( generation.data(), std::streamsize( generation.size() ) ) ||
        sha != session.source.sha256 || generation != session.generation ||
        header.recordsOffset != uint64_t( in.tellg() ) ||
        header.eventCount > ( manifest.fileBytes - header.recordsOffset ) / sizeof( StoredSample ) ||
        header.recordsOffset + header.eventCount * sizeof( StoredSample ) != manifest.fileBytes )
    { error = "session_sampling_file_identity_or_bounds_invalid"; return {}; }
    auto impl = std::make_shared<Impl>();
    impl->path = path; impl->fingerprint = session.source.sha256;
    impl->recordsOffset = header.recordsOffset; impl->eventCount = header.eventCount;
    auto reader = std::shared_ptr<TraceSessionSamplingReader>(
        new TraceSessionSamplingReader( std::move( impl ) ) );
    reader->m_stats = manifest.stats;
    return reader;
}

std::vector<SampleDto> TraceSessionSamplingReader::Scan( const ScanRange& range ) const
{
    std::vector<SampleDto> result;
    std::ifstream in( m_impl->path, std::ios::binary );
    if( !in ) return result;
    in.seekg( std::streamoff( m_impl->recordsOffset ) );
    size_t skipped = 0;
    for( uint64_t ordinal = 0; ordinal < m_impl->eventCount; ++ordinal )
    {
        StoredSample value;
        if( !in.read( reinterpret_cast<char*>( &value ), sizeof( value ) ) ) return result;
        if( value.timeNs < range.startNs || value.timeNs >= range.endNs ) continue;
        if( skipped++ < range.offset ) continue;
        SampleDto dto;
        dto.ref = MakeRef( m_impl->fingerprint,
            value.kind == 0 ? "sample" : "context-switch-sample", ordinal );
        dto.threadRef = MakeRef( m_impl->fingerprint, "thread", value.thread );
        dto.timeNs = value.timeNs; dto.callstack = value.callstack;
        if( dto.callstack != 0 ) dto.callstackRef =
            MakeRef( m_impl->fingerprint, "callstack", dto.callstack );
        dto.kind = value.kind == 0 ? "sample" : "context_switch";
        result.emplace_back( std::move( dto ) );
        if( result.size() >= range.limit ) break;
    }
    return result;
}

}

#include "TracyTraceSessionDerived.hpp"

#include "TracyGpuAnalysisStore.hpp"
#include "TracyHash.hpp"
#include "TracyTraceSessionFrames.hpp"
#include "TracyTraceSessionFrameImages.hpp"
#include "TracyTraceSessionSymbols.hpp"
#include "TracyTraceSessionGpuCanonical.hpp"
#include "TracyTraceSessionJobs.hpp"
#include "TracyTraceSessionCpuZones.hpp"
#include "TracyTraceSessionGpuZones.hpp"
#include "TracyTraceSessionMemory.hpp"
#include "TracyTraceSessionSampling.hpp"
#include "TracyTraceSessionScheduling.hpp"
#include "TracyTraceSessionPlots.hpp"
#include "TracyTraceSessionMessages.hpp"
#include "TracyTraceSessionLocks.hpp"
#include "TracyTraceSessionRelations.hpp"
#include "TracyTraceSessionRuntime.hpp"
#include "TracyTraceSessionIoGfx.hpp"
#include "TracyQueue.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <vector>

#ifdef _WIN32
#  include <Windows.h>
#endif

namespace tracy::analysis
{
namespace
{

constexpr uint64_t DomainIndexMagic = 0x31584449534e4aull; // JNSIDX1
constexpr uint64_t DomainManifestMagic = 0x314d4449534e4aull; // JNSIDM1

#pragma pack( push, 1 )
struct DomainIndexHeader
{
    uint64_t magic = DomainIndexMagic;
    uint32_t schema = TraceSessionDomainIndexSchemaVersion;
    uint32_t domain = 0;
    uint64_t shardId = 0;
    uint64_t recordCount = 0;
    uint64_t protocolEvents = 0;
    uint64_t protocolFrames = 0;
    uint64_t transportRecords = 0;
};

#pragma pack( pop )

struct IndexFile
{
    uint32_t domain = 0;
    uint64_t shardId = 0;
    uint64_t recordCount = 0;
    uint64_t fileBytes = 0;
    std::string sha256;
    std::filesystem::path relativePath;
};

struct IndexManifest
{
    std::string sourceSha256;
    uint64_t sourceSize = 0;
    std::string generation;
    TraceSessionDerivedStats stats;
    std::vector<IndexFile> files;
};

bool AtomicReplace( const std::filesystem::path& source,
    const std::filesystem::path& target, std::string& error )
{
#ifdef _WIN32
    if( MoveFileExW( source.c_str(), target.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) ) return true;
    error = "session_index_atomic_replace_failed:" + std::to_string( GetLastError() );
    return false;
#else
    std::error_code ec;
    std::filesystem::rename( source, target, ec );
    if( !ec ) return true;
    error = "session_index_atomic_replace_failed:" + ec.message();
    return false;
#endif
}

std::string SafeDomain( std::string value )
{
    for( auto& ch : value )
        if( !( ch >= 'a' && ch <= 'z' ) && !( ch >= '0' && ch <= '9' ) && ch != '_' ) ch = '_';
    return value;
}

TraceSessionProtocolDomain DomainFromName( std::string_view name )
{
    for( size_t i = 0; i < size_t( TraceSessionProtocolDomain::Count ); ++i )
        if( name == TraceSessionProtocolDomainName( TraceSessionProtocolDomain( i ) ) )
            return TraceSessionProtocolDomain( i );
    return TraceSessionProtocolDomain::Other;
}

struct IndexVisitorState
{
    DomainIndexHeader* header = nullptr;
    TraceSessionDerivedStats* stats = nullptr;
};

bool IndexRecord( const TraceSessionCanonicalRecord& record, void* userData, std::string& )
{
    auto& state = *static_cast<IndexVisitorState*>( userData );
    state.header->recordCount++;
    switch( record.kind )
    {
    case TraceSessionCanonicalRecordKind::ProtocolEvent: state.header->protocolEvents++; break;
    case TraceSessionCanonicalRecordKind::ProtocolFrame: state.header->protocolFrames++; break;
    case TraceSessionCanonicalRecordKind::TransportRecord: state.header->transportRecords++; break;
    }
    state.stats->domains[size_t( record.domain )]++;
    if( record.hasSemanticTime )
    {
        if( !state.stats->semanticTimePresent )
        {
            state.stats->firstSemanticTimeRaw = record.semanticTime;
            state.stats->lastSemanticTimeRaw = record.semanticTime;
            state.stats->semanticTimePresent = true;
        }
        else
        {
            state.stats->firstSemanticTimeRaw = std::min( state.stats->firstSemanticTimeRaw, record.semanticTime );
            state.stats->lastSemanticTimeRaw = std::max( state.stats->lastSemanticTimeRaw, record.semanticTime );
        }
        state.stats->semanticTimeRecords++;
    }
    return true;
}

bool SaveIndexManifest( const std::filesystem::path& root,
    const IndexManifest& value, std::string& error )
{
    const auto temporary = root / "manifest.tmp";
    const auto target = root / "manifest";
    std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
    if( !out ) { error = "session_index_manifest_open_failed"; return false; }
    out << "magic " << DomainManifestMagic << '\n';
    out << "schema " << TraceSessionDomainIndexSchemaVersion << '\n';
    out << "source_sha256 " << std::quoted( value.sourceSha256 ) << '\n';
    out << "source_size " << value.sourceSize << '\n';
    out << "generation " << std::quoted( value.generation ) << '\n';
    out << "indexed_records " << value.stats.indexedRecords << '\n';
    out << "protocol_events " << value.stats.indexedProtocolEvents << '\n';
    out << "protocol_frames " << value.stats.indexedProtocolFrames << '\n';
    out << "transport_records " << value.stats.indexedTransportRecords << '\n';
    out << "semantic_time_records " << value.stats.semanticTimeRecords << '\n';
    out << "first_semantic_time_raw " << value.stats.firstSemanticTimeRaw << '\n';
    out << "last_semantic_time_raw " << value.stats.lastSemanticTimeRaw << '\n';
    out << "semantic_time_present " << ( value.stats.semanticTimePresent ? 1 : 0 ) << '\n';
    out << "index_bytes " << value.stats.indexBytes << '\n';
    out << "index_files " << value.stats.indexFiles << '\n';
    out << "gpu_resources " << value.stats.gpuResources << '\n';
    out << "gpu_allocations " << value.stats.gpuAllocations << '\n';
    out << "gpu_passes " << value.stats.gpuPasses << '\n';
    out << "gpu_source_gap_resources " << value.stats.gpuSourceGapResources << '\n';
    out << "gpu_source_gap_references " << value.stats.gpuSourceGapReferences << '\n';
    out << "frame_sets " << value.stats.frameSets << '\n';
    out << "frames " << value.stats.frames << '\n';
    out << "complete_frames " << value.stats.completeFrames << '\n';
    out << "frame_images " << value.stats.frameImages << '\n';
    out << "frame_image_data_events " << value.stats.frameImageDataEvents << '\n';
    out << "frame_image_events " << value.stats.frameImageEvents << '\n';
    out << "frame_image_bc1_bytes " << value.stats.frameImageBc1Bytes << '\n';
    out << "job_types " << value.stats.jobTypes << '\n';
    out << "jobs " << value.stats.jobs << '\n';
    out << "job_schedules " << value.stats.jobSchedules << '\n';
    out << "job_configs " << value.stats.jobConfigs << '\n';
    out << "job_dependencies " << value.stats.jobDependencies << '\n';
    out << "job_stages " << value.stats.jobStages << '\n';
    out << "cpu_zones " << value.stats.cpuZones << '\n';
    out << "cpu_zone_blocks " << value.stats.cpuZoneBlocks << '\n';
    out << "complete_cpu_zones " << value.stats.completeCpuZones << '\n';
    out << "invalid_cpu_zone_timings " << value.stats.invalidCpuZoneTimings << '\n';
    out << "cpu_zone_sources " << value.stats.cpuZoneSources << '\n';
    out << "cpu_zone_begins " << value.stats.cpuZoneBegins << '\n';
    out << "cpu_zone_ends " << value.stats.cpuZoneEnds << '\n';
    out << "callsites " << value.stats.callsites << '\n';
    out << "resolved_callstacks " << value.stats.resolvedCallstacks << '\n';
    out << "callstack_entries " << value.stats.callstackEntries << '\n';
    out << "callstack_frame_addresses " << value.stats.callstackFrameAddresses << '\n';
    out << "callstack_inline_frames " << value.stats.callstackInlineFrames << '\n';
    out << "symbols " << value.stats.symbols << '\n';
    out << "symbol_code_bytes " << value.stats.symbolCodeBytes << '\n';
    out << "gpu_contexts " << value.stats.gpuContexts << '\n';
    out << "gpu_zones " << value.stats.gpuZones << '\n';
    out << "complete_gpu_zones " << value.stats.completeGpuZones << '\n';
    out << "gpu_zone_sources " << value.stats.gpuZoneSources << '\n';
    out << "gpu_zone_begins " << value.stats.gpuZoneBegins << '\n';
    out << "gpu_zone_ends " << value.stats.gpuZoneEnds << '\n';
    out << "gpu_time_events " << value.stats.gpuTimeEvents << '\n';
    out << "gpu_calibration_events " << value.stats.gpuCalibrationEvents << '\n';
    out << "gpu_sync_events " << value.stats.gpuSyncEvents << '\n';
    out << "memory_pools " << value.stats.memoryPools << '\n';
    out << "memory_events " << value.stats.memoryEvents << '\n';
    out << "active_memory_events " << value.stats.activeMemoryEvents << '\n';
    out << "memory_allocations " << value.stats.memoryAllocations << '\n';
    out << "memory_frees " << value.stats.memoryFrees << '\n';
    out << "memory_discards " << value.stats.memoryDiscards << '\n';
    out << "memory_unknown_frees " << value.stats.memoryUnknownFrees << '\n';
    out << "sample_events " << value.stats.sampleEvents << '\n';
    out << "context_switch_sample_events " << value.stats.contextSwitchSampleEvents << '\n';
    out << "sample_dictionary_entries " << value.stats.sampleDictionaryEntries << '\n';
    out << "callstack_payloads " << value.stats.callstackPayloads << '\n';
    out << "hardware_sample_events " << value.stats.hardwareSampleEvents << '\n';
    out << "hardware_sample_addresses " << value.stats.hardwareSampleAddresses << '\n';
    out << "sample_blocks " << value.stats.sampleBlocks << '\n';
    out << "context_switch_records " << value.stats.contextSwitchRecords << '\n';
    out << "thread_wakeup_records " << value.stats.threadWakeupRecords << '\n';
    out << "context_switch_events " << value.stats.contextSwitchEvents << '\n';
    out << "complete_context_switch_events " << value.stats.completeContextSwitchEvents << '\n';
    out << "cpu_context_switch_events " << value.stats.cpuContextSwitchEvents << '\n';
    out << "complete_cpu_context_switch_events " << value.stats.completeCpuContextSwitchEvents << '\n';
    out << "scheduling_source_gaps " << value.stats.schedulingSourceGaps << '\n';
    out << "cpu_topology_records " << value.stats.cpuTopologyRecords << '\n';
    out << "cpu_topology_cpus " << value.stats.cpuTopologyCpus << '\n';
    out << "thread_summaries " << value.stats.threadSummaries << '\n';
    out << "cpu_usage_points " << value.stats.cpuUsagePoints << '\n';
    out << "scheduling_thread_blocks " << value.stats.schedulingThreadBlocks << '\n';
    out << "scheduling_cpu_blocks " << value.stats.schedulingCpuBlocks << '\n';
    out << "relations " << value.stats.relations << '\n';
    out << "runtime_domain_states " << value.stats.runtimeDomainStates << '\n';
    out << "script_frames " << value.stats.scriptFrames << '\n';
    out << "script_stack_events " << value.stats.scriptStackEvents << '\n';
    out << "io_requests " << value.stats.ioRequests << '\n';
    out << "io_configs " << value.stats.ioConfigs << '\n';
    out << "io_stages " << value.stats.ioStages << '\n';
    out << "gfx_dispatches " << value.stats.gfxDispatches << '\n';
    out << "gfx_entities " << value.stats.gfxEntities << '\n';
    out << "gfx_links " << value.stats.gfxLinks << '\n';
    out << "correlated_frames " << value.stats.correlatedFrames << '\n';
    for( size_t i = 0; i < value.stats.domains.size(); ++i )
        out << "domain " << i << ' ' << value.stats.domains[i] << '\n';
    out << "file_count " << value.files.size() << '\n';
    for( const auto& file : value.files )
        out << "file " << file.domain << ' ' << file.shardId << ' ' << file.recordCount << ' '
            << file.fileBytes << ' ' << std::quoted( file.sha256 ) << ' '
            << std::quoted( file.relativePath.generic_string() ) << '\n';
    out.flush();
    if( !out ) { error = "session_index_manifest_write_failed"; return false; }
    out.close();
    return AtomicReplace( temporary, target, error );
}

bool LoadIndexManifest( const std::filesystem::path& root,
    IndexManifest& value, std::string& error )
{
    value = {};
    std::ifstream in( root / "manifest", std::ios::binary );
    if( !in ) { error = "session_index_manifest_not_found"; return false; }
    uint64_t magic = 0; uint32_t schema = 0; size_t fileCount = 0; std::string key;
    while( in >> key )
    {
        if( key == "magic" ) in >> magic;
        else if( key == "schema" ) in >> schema;
        else if( key == "source_sha256" ) in >> std::quoted( value.sourceSha256 );
        else if( key == "source_size" ) in >> value.sourceSize;
        else if( key == "generation" ) in >> std::quoted( value.generation );
        else if( key == "indexed_records" ) in >> value.stats.indexedRecords;
        else if( key == "protocol_events" ) in >> value.stats.indexedProtocolEvents;
        else if( key == "protocol_frames" ) in >> value.stats.indexedProtocolFrames;
        else if( key == "transport_records" ) in >> value.stats.indexedTransportRecords;
        else if( key == "semantic_time_records" ) in >> value.stats.semanticTimeRecords;
        else if( key == "first_semantic_time_raw" ) in >> value.stats.firstSemanticTimeRaw;
        else if( key == "last_semantic_time_raw" ) in >> value.stats.lastSemanticTimeRaw;
        else if( key == "semantic_time_present" )
        {
            uint32_t present = 0;
            in >> present;
            if( present > 1 ) { error = "session_index_semantic_time_flag_invalid"; return false; }
            value.stats.semanticTimePresent = present != 0;
        }
        else if( key == "index_bytes" ) in >> value.stats.indexBytes;
        else if( key == "index_files" ) in >> value.stats.indexFiles;
        else if( key == "gpu_resources" ) in >> value.stats.gpuResources;
        else if( key == "gpu_allocations" ) in >> value.stats.gpuAllocations;
        else if( key == "gpu_passes" ) in >> value.stats.gpuPasses;
        else if( key == "gpu_source_gap_resources" ) in >> value.stats.gpuSourceGapResources;
        else if( key == "gpu_source_gap_references" ) in >> value.stats.gpuSourceGapReferences;
        else if( key == "frame_sets" ) in >> value.stats.frameSets;
        else if( key == "frames" ) in >> value.stats.frames;
        else if( key == "complete_frames" ) in >> value.stats.completeFrames;
        else if( key == "frame_images" ) in >> value.stats.frameImages;
        else if( key == "frame_image_data_events" ) in >> value.stats.frameImageDataEvents;
        else if( key == "frame_image_events" ) in >> value.stats.frameImageEvents;
        else if( key == "frame_image_bc1_bytes" ) in >> value.stats.frameImageBc1Bytes;
        else if( key == "job_types" ) in >> value.stats.jobTypes;
        else if( key == "jobs" ) in >> value.stats.jobs;
        else if( key == "job_schedules" ) in >> value.stats.jobSchedules;
        else if( key == "job_configs" ) in >> value.stats.jobConfigs;
        else if( key == "job_dependencies" ) in >> value.stats.jobDependencies;
        else if( key == "job_stages" ) in >> value.stats.jobStages;
        else if( key == "cpu_zones" ) in >> value.stats.cpuZones;
        else if( key == "cpu_zone_blocks" ) in >> value.stats.cpuZoneBlocks;
        else if( key == "complete_cpu_zones" ) in >> value.stats.completeCpuZones;
        else if( key == "invalid_cpu_zone_timings" ) in >> value.stats.invalidCpuZoneTimings;
        else if( key == "cpu_zone_sources" ) in >> value.stats.cpuZoneSources;
        else if( key == "cpu_zone_begins" ) in >> value.stats.cpuZoneBegins;
        else if( key == "cpu_zone_ends" ) in >> value.stats.cpuZoneEnds;
        else if( key == "callsites" ) in >> value.stats.callsites;
        else if( key == "resolved_callstacks" ) in >> value.stats.resolvedCallstacks;
        else if( key == "callstack_entries" ) in >> value.stats.callstackEntries;
        else if( key == "callstack_frame_addresses" ) in >> value.stats.callstackFrameAddresses;
        else if( key == "callstack_inline_frames" ) in >> value.stats.callstackInlineFrames;
        else if( key == "symbols" ) in >> value.stats.symbols;
        else if( key == "symbol_code_bytes" ) in >> value.stats.symbolCodeBytes;
        else if( key == "gpu_contexts" ) in >> value.stats.gpuContexts;
        else if( key == "gpu_zones" ) in >> value.stats.gpuZones;
        else if( key == "complete_gpu_zones" ) in >> value.stats.completeGpuZones;
        else if( key == "gpu_zone_sources" ) in >> value.stats.gpuZoneSources;
        else if( key == "gpu_zone_begins" ) in >> value.stats.gpuZoneBegins;
        else if( key == "gpu_zone_ends" ) in >> value.stats.gpuZoneEnds;
        else if( key == "gpu_time_events" ) in >> value.stats.gpuTimeEvents;
        else if( key == "gpu_calibration_events" ) in >> value.stats.gpuCalibrationEvents;
        else if( key == "gpu_sync_events" ) in >> value.stats.gpuSyncEvents;
        else if( key == "memory_pools" ) in >> value.stats.memoryPools;
        else if( key == "memory_events" ) in >> value.stats.memoryEvents;
        else if( key == "active_memory_events" ) in >> value.stats.activeMemoryEvents;
        else if( key == "memory_allocations" ) in >> value.stats.memoryAllocations;
        else if( key == "memory_frees" ) in >> value.stats.memoryFrees;
        else if( key == "memory_discards" ) in >> value.stats.memoryDiscards;
        else if( key == "memory_unknown_frees" ) in >> value.stats.memoryUnknownFrees;
        else if( key == "sample_events" ) in >> value.stats.sampleEvents;
        else if( key == "context_switch_sample_events" ) in >> value.stats.contextSwitchSampleEvents;
        else if( key == "sample_dictionary_entries" ) in >> value.stats.sampleDictionaryEntries;
        else if( key == "callstack_payloads" ) in >> value.stats.callstackPayloads;
        else if( key == "hardware_sample_events" ) in >> value.stats.hardwareSampleEvents;
        else if( key == "hardware_sample_addresses" ) in >> value.stats.hardwareSampleAddresses;
        else if( key == "sample_blocks" ) in >> value.stats.sampleBlocks;
        else if( key == "context_switch_records" ) in >> value.stats.contextSwitchRecords;
        else if( key == "thread_wakeup_records" ) in >> value.stats.threadWakeupRecords;
        else if( key == "context_switch_events" ) in >> value.stats.contextSwitchEvents;
        else if( key == "complete_context_switch_events" ) in >> value.stats.completeContextSwitchEvents;
        else if( key == "cpu_context_switch_events" ) in >> value.stats.cpuContextSwitchEvents;
        else if( key == "complete_cpu_context_switch_events" ) in >> value.stats.completeCpuContextSwitchEvents;
        else if( key == "scheduling_source_gaps" ) in >> value.stats.schedulingSourceGaps;
        else if( key == "cpu_topology_records" ) in >> value.stats.cpuTopologyRecords;
        else if( key == "cpu_topology_cpus" ) in >> value.stats.cpuTopologyCpus;
        else if( key == "thread_summaries" ) in >> value.stats.threadSummaries;
        else if( key == "cpu_usage_points" ) in >> value.stats.cpuUsagePoints;
        else if( key == "scheduling_thread_blocks" ) in >> value.stats.schedulingThreadBlocks;
        else if( key == "scheduling_cpu_blocks" ) in >> value.stats.schedulingCpuBlocks;
        else if( key == "relations" ) in >> value.stats.relations;
        else if( key == "runtime_domain_states" ) in >> value.stats.runtimeDomainStates;
        else if( key == "script_frames" ) in >> value.stats.scriptFrames;
        else if( key == "script_stack_events" ) in >> value.stats.scriptStackEvents;
        else if( key == "io_requests" ) in >> value.stats.ioRequests;
        else if( key == "io_configs" ) in >> value.stats.ioConfigs;
        else if( key == "io_stages" ) in >> value.stats.ioStages;
        else if( key == "gfx_dispatches" ) in >> value.stats.gfxDispatches;
        else if( key == "gfx_entities" ) in >> value.stats.gfxEntities;
        else if( key == "gfx_links" ) in >> value.stats.gfxLinks;
        else if( key == "correlated_frames" ) in >> value.stats.correlatedFrames;
        else if( key == "domain" )
        {
            size_t index = 0; uint64_t count = 0; in >> index >> count;
            if( index >= value.stats.domains.size() ) { error = "session_index_domain_invalid"; return false; }
            value.stats.domains[index] = count;
        }
        else if( key == "file_count" ) in >> fileCount;
        else if( key == "file" )
        {
            IndexFile file; std::string relative;
            in >> file.domain >> file.shardId >> file.recordCount >> file.fileBytes
                >> std::quoted( file.sha256 ) >> std::quoted( relative );
            file.relativePath = std::filesystem::path( relative );
            value.files.push_back( std::move( file ) );
        }
        else { std::string ignored; std::getline( in, ignored ); }
        if( !in ) { error = "session_index_manifest_parse_failed"; return false; }
    }
    if( magic != DomainManifestMagic || schema != TraceSessionDomainIndexSchemaVersion ||
        value.files.size() != fileCount || value.stats.indexFiles != fileCount )
    { error = "session_index_manifest_invalid"; return false; }
    return true;
}

bool VerifyIndexManifest( const std::filesystem::path& root,
    const TraceSessionManifest& session, IndexManifest& index, std::string& error )
{
    if( !LoadIndexManifest( root, index, error ) ) return false;
    if( index.sourceSha256 != session.source.sha256 || index.sourceSize != session.source.fileSize ||
        index.generation != session.generation )
    { error = "session_index_identity_mismatch"; return false; }
    uint64_t records = 0; uint64_t bytes = 0;
    for( const auto& file : index.files )
    {
        if( file.relativePath.is_absolute() || file.relativePath.string().find( ".." ) != std::string::npos )
        { error = "session_index_unsafe_path"; return false; }
        const auto path = root / file.relativePath;
        std::error_code ec; const auto size = std::filesystem::file_size( path, ec );
        if( ec || size != file.fileBytes ) { error = "session_index_size_mismatch"; return false; }
        if( Sha256File( path ) != file.sha256 ) { error = "session_index_checksum_mismatch"; return false; }
        std::ifstream in( path, std::ios::binary ); DomainIndexHeader header;
        in.read( reinterpret_cast<char*>( &header ), sizeof( header ) );
        if( !in || header.magic != DomainIndexMagic || header.schema != TraceSessionDomainIndexSchemaVersion ||
            header.domain != file.domain || header.shardId != file.shardId ||
            header.recordCount != file.recordCount ||
            file.fileBytes != sizeof( header ) )
        { error = "session_index_header_mismatch"; return false; }
        records += header.recordCount; bytes += file.fileBytes;
    }
    if( records != index.stats.indexedRecords || bytes != index.stats.indexBytes )
    { error = "session_index_total_mismatch"; return false; }
    return true;
}

bool IndexCoversCanonicalShards( const TraceSessionManifest& session,
    const IndexManifest& index )
{
    size_t fileIndex = 0;
    for( const auto& shard : session.shards )
    {
        if( shard.domain == "checkpoint" ) continue;
        if( fileIndex >= index.files.size() ) return false;
        const auto& file = index.files[fileIndex++];
        auto domain = uint32_t( DomainFromName( shard.domain ) );
        if( domain >= uint32_t( TraceSessionProtocolDomain::Count ) )
            domain = uint32_t( TraceSessionProtocolDomain::Other );
        if( file.shardId != shard.shardId || file.domain != domain ||
            file.recordCount != shard.recordCount ) return false;
    }
    return fileIndex == index.files.size();
}

}

std::filesystem::path TraceSessionDomainIndexRoot( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest )
{
    return sessionRoot / "generations" / manifest.generation / "derived" / "session-index";
}

bool LoadTraceSessionDerivedStats( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, TraceSessionDerivedStats& stats,
    std::string& error )
{
    error.clear();
    stats = {};
    IndexManifest index;
    // Final Audit already verified every immutable index file before the
    // generation was published. Opening a Session must remain O(manifest), so
    // payload checksums are verified lazily by the page that is actually read.
    if( !LoadIndexManifest( TraceSessionDomainIndexRoot( sessionRoot, manifest ),
        index, error ) ) return false;
    if( index.sourceSha256 != manifest.source.sha256 ||
        index.sourceSize != manifest.source.fileSize ||
        index.generation != manifest.generation )
    {
        error = "session_index_identity_mismatch";
        return false;
    }
    stats = index.stats;
    return true;
}

bool BuildTraceSessionMandatoryDerived( const std::filesystem::path& sessionRoot,
    TraceSessionManifest& manifest, const TraceSessionInventory& inventory,
    const TraceSessionDerivedControl& control, TraceSessionDerivedStats& stats,
    std::string& error )
{
    error.clear(); stats = {};
    const auto root = TraceSessionDomainIndexRoot( sessionRoot, manifest );
    std::error_code ec; std::filesystem::create_directories( root, ec );
    if( ec ) { error = "session_index_directory_failed:" + ec.message(); return false; }
    const auto eventShardCount = std::count_if( manifest.shards.begin(), manifest.shards.end(),
        []( const auto& shard ) { return shard.domain != "checkpoint"; } );
    IndexManifest index;
    std::string reuseError;
    const bool reuseDomainIndex = VerifyIndexManifest( root, manifest, index, reuseError ) &&
        IndexCoversCanonicalShards( manifest, index );
    if( reuseDomainIndex )
    {
        if( control.progress ) control.progress( 1.f, "session-domain-index-reused" );
    }
    else
    {
        index = {};
        index.sourceSha256 = manifest.source.sha256;
        index.sourceSize = manifest.source.fileSize;
        index.generation = manifest.generation;
        size_t completedShards = 0;
        for( const auto& shard : manifest.shards )
        {
            if( shard.domain == "checkpoint" ) continue;
            if( control.stopToken.stop_requested() ) { error = "cancelled_resumable"; return false; }
            DomainIndexHeader header;
            header.domain = uint32_t( DomainFromName( shard.domain ) );
            if( header.domain >= uint32_t( TraceSessionProtocolDomain::Count ) ) header.domain = uint32_t( TraceSessionProtocolDomain::Other );
            header.shardId = shard.shardId;
            IndexVisitorState state { &header, &index.stats };
            if( !VisitTraceSessionCanonicalShard( sessionRoot, shard, IndexRecord, &state, error ) ) return false;
            if( header.recordCount != shard.recordCount )
            { error = "session_index_record_count_mismatch"; return false; }
            std::ostringstream name;
            name << SafeDomain( shard.domain ) << '-' << std::setw( 6 ) << std::setfill( '0' ) << shard.shardId << ".idx";
            const auto relative = std::filesystem::path( "shards" ) / name.str();
            const auto target = root / relative;
            std::filesystem::create_directories( target.parent_path(), ec );
            if( ec ) { error = "session_index_shard_directory_failed:" + ec.message(); return false; }
            auto temporary = target; temporary += ".tmp";
            std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
            if( !out ) { error = "session_index_open_failed"; return false; }
            out.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );
            out.flush();
            if( !out ) { error = "session_index_write_failed"; return false; }
            out.close();
            if( !AtomicReplace( temporary, target, error ) ) return false;
            IndexFile file;
            file.domain = header.domain; file.shardId = shard.shardId; file.recordCount = header.recordCount;
            file.fileBytes = sizeof( header );
            file.sha256 = Sha256File( target ); file.relativePath = relative;
            index.files.push_back( file );
            index.stats.indexedRecords += header.recordCount;
            index.stats.indexedProtocolEvents += header.protocolEvents;
            index.stats.indexedProtocolFrames += header.protocolFrames;
            index.stats.indexedTransportRecords += header.transportRecords;
            index.stats.indexBytes += file.fileBytes;
            index.stats.indexFiles++;
            if( control.progress ) control.progress( eventShardCount == 0 ? 1.f :
                float( ++completedShards ) / float( eventShardCount ), "session-domain-index" );
        }
        // Publish the exact generic index as soon as it is complete. Later
        // domain failures may then resume without rescanning every Canonical
        // shard. The same manifest is atomically replaced with final domain
        // statistics after all mandatory builders finish.
        if( !SaveIndexManifest( root, index, error ) ) return false;
    }

    TraceSessionTimeTransform timeTransform;
    if( control.progress ) control.progress( 0.f, "time-transform" );
    reuseError.clear();
    if( AuditTraceSessionTimeTransformDerived( sessionRoot, manifest, timeTransform, reuseError ) )
    {
        if( control.progress ) control.progress( 1.f, "time-transform-reused" );
    }
    else if( !BuildTraceSessionTimeTransformDerived( sessionRoot, manifest, timeTransform, error ) ) return false;

    TraceSessionFrameStats frameStats;
    if( control.progress ) control.progress( 0.f, "frames" );
    reuseError.clear();
    if( const auto reader = TraceSessionFrameReader::Open( sessionRoot, manifest, reuseError ) )
    {
        frameStats = reader->Stats();
        if( control.progress ) control.progress( 1.f, "frames-reused" );
    }
    else if( !BuildTraceSessionFrameDerived( sessionRoot, manifest,
        index.stats.semanticTimePresent, index.stats.lastSemanticTimeRaw,
        frameStats, error ) ) return false;
    index.stats.frameSets = frameStats.frameSets;
    index.stats.frames = frameStats.frames;
    index.stats.completeFrames = frameStats.completeFrames;

    TraceSessionFrameImageStats frameImageStats;
    if( control.progress ) control.progress( 0.f, "frame-images" );
    reuseError.clear();
    if( AuditTraceSessionFrameImageDerived( sessionRoot, manifest,
        frameImageStats, reuseError ) )
    {
        if( control.progress ) control.progress( 1.f, "frame-images-reused" );
    }
    else if( !BuildTraceSessionFrameImageDerived( sessionRoot, manifest,
        frameImageStats, error ) ) return false;
    index.stats.frameImages = frameImageStats.images;
    index.stats.frameImageDataEvents = frameImageStats.imageDataEvents;
    index.stats.frameImageEvents = frameImageStats.imageEvents;
    index.stats.frameImageBc1Bytes = frameImageStats.bc1Bytes;

    TraceSessionJobStats jobStats;
    if( control.progress ) control.progress( 0.f, "jobs" );
    reuseError.clear();
    if( AuditTraceSessionJobDerived( sessionRoot, manifest, jobStats, reuseError ) )
    {
        if( !CleanupTraceSessionJobTemporaryRuns( sessionRoot, manifest, error ) ) return false;
        if( control.progress ) control.progress( 1.f, "jobs-reused" );
    }
    else if( !BuildTraceSessionJobDerived( sessionRoot, manifest, jobStats, error ) ) return false;
    if( control.progress ) control.progress( 0.f, "job-pages" );
    reuseError.clear();
    if( AuditTraceSessionJobPagingDerived( sessionRoot, manifest, reuseError ) )
    {
        if( control.progress ) control.progress( 1.f, "job-pages-reused" );
    }
    else if( !BuildTraceSessionJobPagingDerived( sessionRoot, manifest, error ) ) return false;
    index.stats.jobTypes = jobStats.jobTypes;
    index.stats.jobs = jobStats.jobs;
    index.stats.jobSchedules = jobStats.schedules;
    index.stats.jobConfigs = jobStats.configs;
    index.stats.jobDependencies = jobStats.dependencies;
    index.stats.jobStages = jobStats.stages;

    TraceSessionCpuZoneStats cpuZoneStats;
    if( control.progress ) control.progress( 0.f, "cpu-zones" );
    reuseError.clear();
    if( AuditTraceSessionCpuZoneDerived( sessionRoot, manifest, cpuZoneStats, reuseError ) )
    {
        if( !CleanupTraceSessionCpuZoneTemporaryFiles( sessionRoot, manifest, error ) ) return false;
        if( control.progress ) control.progress( 1.f, "cpu-zones-reused" );
    }
    else if( !BuildTraceSessionCpuZoneDerived( sessionRoot, manifest, cpuZoneStats, error ) ) return false;
    index.stats.cpuZones = cpuZoneStats.zones;
    index.stats.cpuZoneBlocks = cpuZoneStats.zoneBlocks;
    index.stats.completeCpuZones = cpuZoneStats.completeZones;
    index.stats.invalidCpuZoneTimings = cpuZoneStats.invalidTimingZones;
    index.stats.cpuZoneSources = cpuZoneStats.sourceLocations;
    index.stats.cpuZoneBegins = cpuZoneStats.beginEvents;
    index.stats.cpuZoneEnds = cpuZoneStats.endEvents;
    const auto cpuZoneReader = TraceSessionCpuZoneReader::Open( sessionRoot, manifest, error );
    if( !cpuZoneReader ) return false;
    index.stats.callsites = cpuZoneReader->Callsites().size();

    TraceSessionSymbolStats symbolStats;
    if( control.progress ) control.progress( 0.f, "symbols-callstacks" );
    reuseError.clear();
    if( AuditTraceSessionSymbolDerived( sessionRoot, manifest, symbolStats, reuseError ) )
    {
        if( control.progress ) control.progress( 1.f, "symbols-callstacks-reused" );
    }
    else if( !BuildTraceSessionSymbolDerived( sessionRoot, manifest, symbolStats, error ) ) return false;
    index.stats.resolvedCallstacks = symbolStats.callstacks;
    index.stats.callstackEntries = symbolStats.callstackEntries;
    index.stats.callstackFrameAddresses = symbolStats.frameAddresses;
    index.stats.callstackInlineFrames = symbolStats.inlineFrames;
    index.stats.symbols = symbolStats.symbols;
    index.stats.symbolCodeBytes = symbolStats.symbolCodeBytes;

    TraceSessionGpuZoneStats gpuZoneStats;
    if( control.progress ) control.progress( 0.f, "gpu-zones" );
    reuseError.clear();
    if( AuditTraceSessionGpuZoneDerived( sessionRoot, manifest, gpuZoneStats, reuseError ) )
    {
        if( !CleanupTraceSessionGpuZoneTemporaryFiles( sessionRoot, manifest, error ) ) return false;
        if( control.progress ) control.progress( 1.f, "gpu-zones-reused" );
    }
    else if( !BuildTraceSessionGpuZoneDerived( sessionRoot, manifest, gpuZoneStats, error ) ) return false;
    index.stats.gpuContexts = gpuZoneStats.contexts;
    index.stats.gpuZones = gpuZoneStats.zones;
    index.stats.completeGpuZones = gpuZoneStats.completeZones;
    index.stats.gpuZoneSources = gpuZoneStats.sourceLocations;
    index.stats.gpuZoneBegins = gpuZoneStats.beginEvents;
    index.stats.gpuZoneEnds = gpuZoneStats.endEvents;
    index.stats.gpuTimeEvents = gpuZoneStats.gpuTimeEvents;
    index.stats.gpuCalibrationEvents = gpuZoneStats.calibrationEvents;
    index.stats.gpuSyncEvents = gpuZoneStats.syncEvents;

    TraceSessionMemoryStats memoryStats;
    if( control.progress ) control.progress( 0.f, "memory" );
    reuseError.clear();
    if( AuditTraceSessionMemoryDerived( sessionRoot, manifest, memoryStats, reuseError ) )
    {
        if( control.progress ) control.progress( 1.f, "memory-reused" );
    }
    else if( !BuildTraceSessionMemoryDerived( sessionRoot, manifest, memoryStats, error ) ) return false;
    index.stats.memoryPools = memoryStats.pools;
    index.stats.memoryEvents = memoryStats.events;
    index.stats.activeMemoryEvents = memoryStats.activeEvents;
    index.stats.memoryAllocations = memoryStats.allocationEvents;
    index.stats.memoryFrees = memoryStats.freeEvents;
    index.stats.memoryDiscards = memoryStats.discardEvents;
    index.stats.memoryUnknownFrees = memoryStats.unknownFrees;

    TraceSessionSamplingStats samplingStats;
    if( control.progress ) control.progress( 0.f, "sampling" );
    reuseError.clear();
    if( AuditTraceSessionSamplingDerived( sessionRoot, manifest, samplingStats, reuseError ) )
    {
        if( control.progress ) control.progress( 1.f, "sampling-reused" );
    }
    else if( !BuildTraceSessionSamplingDerived( sessionRoot, manifest, samplingStats, error ) ) return false;
    index.stats.sampleEvents = samplingStats.samples;
    index.stats.contextSwitchSampleEvents = samplingStats.contextSwitchSamples;
    index.stats.sampleDictionaryEntries = samplingStats.dictionaryEntries;
    index.stats.callstackPayloads = samplingStats.callstackPayloads;
    index.stats.hardwareSampleEvents = samplingStats.hardwareEvents;
    index.stats.hardwareSampleAddresses = samplingStats.hardwareAddresses;
    index.stats.sampleBlocks = samplingStats.sampleBlocks;

    TraceSessionSchedulingStats schedulingStats;
    if( control.progress ) control.progress( 0.f, "scheduling" );
    reuseError.clear();
    if( AuditTraceSessionSchedulingDerived( sessionRoot, manifest, schedulingStats, reuseError ) )
    {
        if( control.progress ) control.progress( 1.f, "scheduling-reused" );
    }
    else if( !BuildTraceSessionSchedulingDerived( sessionRoot, manifest, schedulingStats, error ) ) return false;
    index.stats.contextSwitchRecords = schedulingStats.contextSwitchRecords;
    index.stats.threadWakeupRecords = schedulingStats.wakeupRecords;
    index.stats.contextSwitchEvents = schedulingStats.threadEvents;
    index.stats.completeContextSwitchEvents = schedulingStats.completeThreadEvents;
    index.stats.cpuContextSwitchEvents = schedulingStats.cpuEvents;
    index.stats.completeCpuContextSwitchEvents = schedulingStats.completeCpuEvents;
    index.stats.schedulingSourceGaps = schedulingStats.sourceGapEvents;
    index.stats.cpuTopologyRecords = schedulingStats.topologyRecords;
    index.stats.cpuTopologyCpus = schedulingStats.topologyCpus;
    index.stats.threadSummaries = schedulingStats.threadSummaries;
    index.stats.cpuUsagePoints = schedulingStats.cpuUsagePoints;
    index.stats.schedulingThreadBlocks = schedulingStats.threadBlocks;
    index.stats.schedulingCpuBlocks = schedulingStats.cpuBlocks;

    TraceSessionPlotStats plotStats;
    if( control.progress ) control.progress( 0.f, "plots" );
    reuseError.clear();
    if( AuditTraceSessionPlotDerived( sessionRoot, manifest, plotStats, reuseError ) )
    {
        if( control.progress ) control.progress( 1.f, "plots-reused" );
    }
    else if( !BuildTraceSessionPlotDerived( sessionRoot, manifest, plotStats, error ) ) return false;

    TraceSessionMessageStats messageStats;
    if( control.progress ) control.progress( 0.f, "messages" );
    reuseError.clear();
    if( AuditTraceSessionMessageDerived( sessionRoot, manifest, messageStats, reuseError ) )
    {
        if( control.progress ) control.progress( 1.f, "messages-reused" );
    }
    else if( !BuildTraceSessionMessageDerived( sessionRoot, manifest, messageStats, error ) ) return false;

    TraceSessionLockStats lockStats;
    if( control.progress ) control.progress( 0.f, "locks" );
    reuseError.clear();
    if( AuditTraceSessionLockDerived( sessionRoot, manifest, lockStats, reuseError ) )
    {
        if( control.progress ) control.progress( 1.f, "locks-reused" );
    }
    else if( !BuildTraceSessionLockDerived( sessionRoot, manifest, lockStats, error ) ) return false;

    TraceSessionRelationStats relationStats;
    if( control.progress ) control.progress( 0.f, "relations" );
    reuseError.clear();
    if( AuditTraceSessionRelationDerived( sessionRoot, manifest, relationStats, reuseError ) )
    {
        if( control.progress ) control.progress( 1.f, "relations-reused" );
    }
    else if( !BuildTraceSessionRelationDerived( sessionRoot, manifest, relationStats, error ) ) return false;
    index.stats.relations = relationStats.relations;

    TraceSessionRuntimeStats runtimeStats;
    if( control.progress ) control.progress( 0.f, "runtime-script" );
    reuseError.clear();
    if( AuditTraceSessionRuntimeDerived( sessionRoot, manifest, runtimeStats, reuseError ) )
    {
        if( control.progress ) control.progress( 1.f, "runtime-script-reused" );
    }
    else if( !BuildTraceSessionRuntimeDerived( sessionRoot, manifest, runtimeStats, error ) ) return false;
    index.stats.runtimeDomainStates = runtimeStats.domainStates;
    index.stats.scriptFrames = runtimeStats.scriptFrames;
    index.stats.scriptStackEvents = runtimeStats.scriptStackEvents;

    TraceSessionIoGfxStats ioGfxStats;
    if( control.progress ) control.progress( 0.f, "io-gfx" );
    reuseError.clear();
    if( AuditTraceSessionIoGfxDerived( sessionRoot, manifest, ioGfxStats, reuseError ) )
    {
        if( control.progress ) control.progress( 1.f, "io-gfx-reused" );
    }
    else if( !BuildTraceSessionIoGfxDerived( sessionRoot, manifest, ioGfxStats, error ) ) return false;
    index.stats.ioRequests = ioGfxStats.ioRequests;
    index.stats.ioConfigs = ioGfxStats.ioConfigs;
    index.stats.ioStages = ioGfxStats.ioStages;
    index.stats.gfxDispatches = ioGfxStats.gfxDispatches;
    index.stats.gfxEntities = ioGfxStats.gfxEntities;
    index.stats.gfxLinks = ioGfxStats.gfxLinks;
    index.stats.correlatedFrames = ioGfxStats.correlatedFrames;

    const auto gpuCatalogEvents = inventory.protocolInventory.domains[
        size_t( TraceSessionProtocolDomain::GpuCatalog )].count;
    if( gpuCatalogEvents != 0 )
    {
        if( control.progress ) control.progress( 0.f, "gpu-resource-analysis" );
        GpuAnalysisSidecarControl gpuControl;
        gpuControl.stopToken = control.stopToken;
        gpuControl.minimumFreeBytes = control.minimumFreeBytes;
        gpuControl.progress = control.progress;
        TraceSessionGpuDerivedStats gpuStats;
        if( !BuildTraceSessionGpuAnalysisDerived( sessionRoot, manifest, gpuControl, gpuStats, error ) ) return false;
        index.stats.gpuResources = gpuStats.resourceCount;
        index.stats.gpuAllocations = gpuStats.allocationCount;
        index.stats.gpuPasses = gpuStats.passCount;
        index.stats.gpuSourceGapResources = gpuStats.sourceGapResourceCount;
        index.stats.gpuSourceGapReferences = gpuStats.sourceGapReferenceCount;
    }
    if( !SaveIndexManifest( root, index, error ) ) return false;
    stats = index.stats;
    manifest.mandatoryDerivedComplete = true;
    manifest.state = TraceSessionState::FinalAuditing;
    return SaveTraceSessionManifest( sessionRoot, manifest, error );
}

bool AuditTraceSessionFinal( const std::filesystem::path& sessionRoot,
    const TraceSessionManifest& manifest, const TraceSessionInventory& inventory,
    TraceSessionDerivedStats& stats, std::string& error )
{
    error.clear(); stats = {};
    TraceSessionCanonicalAudit canonical;
    if( !AuditTraceSessionCanonical( sessionRoot, manifest, inventory, canonical, error ) ) return false;
    IndexManifest index;
    const auto indexRoot = TraceSessionDomainIndexRoot( sessionRoot, manifest );
    if( !VerifyIndexManifest( indexRoot, manifest, index, error ) ) return false;
    const auto expected = canonical.protocolEvents + canonical.protocolFrames + canonical.transportRecords;
    if( index.stats.indexedRecords != expected ||
        index.stats.indexedProtocolEvents != canonical.protocolEvents ||
        index.stats.indexedProtocolFrames != canonical.protocolFrames ||
        index.stats.indexedTransportRecords != canonical.transportRecords ||
        index.stats.semanticTimeRecords != canonical.semanticTimeEvents )
    { error = "session_final_audit_index_count_mismatch"; return false; }
    const auto gpuCatalogEvents = inventory.protocolInventory.domains[
        size_t( TraceSessionProtocolDomain::GpuCatalog )].count;
    if( gpuCatalogEvents != 0 )
    {
        const auto gpuRoot = TraceSessionGpuAnalysisRoot( sessionRoot, manifest );
        std::ifstream current( gpuRoot / "current", std::ios::binary );
        std::string generation;
        if( !current || !std::getline( current, generation ) || generation.empty() )
        { error = "session_gpu_derived_current_missing"; return false; }
        const auto gpu = LoadGpuAnalysisStoreManifest( gpuRoot / generation, error );
        if( !gpu || !gpu->complete || gpu->traceSha256 != manifest.source.sha256 ||
            gpu->traceSize != manifest.source.fileSize ||
            gpu->resourceCount != index.stats.gpuResources ||
            gpu->allocationCount != index.stats.gpuAllocations ||
            gpu->passCount != index.stats.gpuPasses ||
            gpu->sourceGapResourceCount != index.stats.gpuSourceGapResources ||
            gpu->sourceGapReferenceCount != index.stats.gpuSourceGapReferences )
        { if( error.empty() ) error = "session_gpu_derived_audit_mismatch"; return false; }
    }
    TraceSessionTimeTransform timeTransform;
    if( !AuditTraceSessionTimeTransformDerived( sessionRoot, manifest, timeTransform, error ) ) return false;
    const auto frameReader = TraceSessionFrameReader::Open( sessionRoot, manifest, error );
    if( !frameReader || frameReader->Stats().frameSets != index.stats.frameSets ||
        frameReader->Stats().frames != index.stats.frames ||
        frameReader->Stats().completeFrames != index.stats.completeFrames )
    { if( error.empty() ) error = "session_frame_derived_audit_mismatch"; return false; }
    TraceSessionFrameImageStats frameImageStats;
    if( !AuditTraceSessionFrameImageDerived( sessionRoot, manifest, frameImageStats, error ) ||
        frameImageStats.images != index.stats.frameImages ||
        frameImageStats.imageDataEvents != index.stats.frameImageDataEvents ||
        frameImageStats.imageEvents != index.stats.frameImageEvents ||
        frameImageStats.bc1Bytes != index.stats.frameImageBc1Bytes )
    { if( error.empty() ) error = "session_frame_image_derived_audit_mismatch"; return false; }
    const auto jobReader = TraceSessionJobReader::Open( sessionRoot, manifest, error );
    if( !jobReader || jobReader->Stats().jobTypes != index.stats.jobTypes ||
        jobReader->Stats().jobs != index.stats.jobs ||
        jobReader->Stats().schedules != index.stats.jobSchedules ||
        jobReader->Stats().configs != index.stats.jobConfigs ||
        jobReader->Stats().dependencies != index.stats.jobDependencies ||
        jobReader->Stats().stages != index.stats.jobStages )
    { if( error.empty() ) error = "session_job_derived_audit_mismatch"; return false; }
    TraceSessionCpuZoneStats cpuZoneStats;
    if( !AuditTraceSessionCpuZoneDerived( sessionRoot, manifest, cpuZoneStats, error ) ||
        cpuZoneStats.zones != index.stats.cpuZones ||
        cpuZoneStats.zoneBlocks != index.stats.cpuZoneBlocks ||
        cpuZoneStats.completeZones != index.stats.completeCpuZones ||
        cpuZoneStats.invalidTimingZones != index.stats.invalidCpuZoneTimings ||
        cpuZoneStats.sourceLocations != index.stats.cpuZoneSources ||
        cpuZoneStats.beginEvents != index.stats.cpuZoneBegins ||
        cpuZoneStats.endEvents != index.stats.cpuZoneEnds )
    { if( error.empty() ) error = "session_cpu_zone_derived_audit_mismatch"; return false; }
    const auto cpuZoneReader = TraceSessionCpuZoneReader::Open( sessionRoot, manifest, error );
    if( !cpuZoneReader || cpuZoneReader->Callsites().size() != index.stats.callsites )
    { if( error.empty() ) error = "session_callsite_derived_audit_mismatch"; return false; }
    TraceSessionSymbolStats symbolStats;
    if( !AuditTraceSessionSymbolDerived( sessionRoot, manifest, symbolStats, error ) ||
        symbolStats.callstacks != index.stats.resolvedCallstacks ||
        symbolStats.callstackEntries != index.stats.callstackEntries ||
        symbolStats.frameAddresses != index.stats.callstackFrameAddresses ||
        symbolStats.inlineFrames != index.stats.callstackInlineFrames ||
        symbolStats.symbols != index.stats.symbols ||
        symbolStats.symbolCodeBytes != index.stats.symbolCodeBytes )
    { if( error.empty() ) error = "session_symbol_derived_audit_mismatch"; return false; }
    TraceSessionGpuZoneStats gpuZoneStats;
    if( !AuditTraceSessionGpuZoneDerived( sessionRoot, manifest, gpuZoneStats, error ) ||
        gpuZoneStats.contexts != index.stats.gpuContexts ||
        gpuZoneStats.zones != index.stats.gpuZones ||
        gpuZoneStats.completeZones != index.stats.completeGpuZones ||
        gpuZoneStats.sourceLocations != index.stats.gpuZoneSources ||
        gpuZoneStats.beginEvents != index.stats.gpuZoneBegins ||
        gpuZoneStats.endEvents != index.stats.gpuZoneEnds ||
        gpuZoneStats.gpuTimeEvents != index.stats.gpuTimeEvents ||
        gpuZoneStats.calibrationEvents != index.stats.gpuCalibrationEvents ||
        gpuZoneStats.syncEvents != index.stats.gpuSyncEvents )
    { if( error.empty() ) error = "session_gpu_zone_derived_audit_mismatch"; return false; }
    TraceSessionMemoryStats memoryStats;
    if( !AuditTraceSessionMemoryDerived( sessionRoot, manifest, memoryStats, error ) ||
        memoryStats.pools != index.stats.memoryPools ||
        memoryStats.events != index.stats.memoryEvents ||
        memoryStats.activeEvents != index.stats.activeMemoryEvents ||
        memoryStats.allocationEvents != index.stats.memoryAllocations ||
        memoryStats.freeEvents != index.stats.memoryFrees ||
        memoryStats.discardEvents != index.stats.memoryDiscards ||
        memoryStats.unknownFrees != index.stats.memoryUnknownFrees )
    { if( error.empty() ) error = "session_memory_derived_audit_mismatch"; return false; }
    TraceSessionSamplingStats samplingStats;
    if( !AuditTraceSessionSamplingDerived( sessionRoot, manifest, samplingStats, error ) ||
        samplingStats.samples != index.stats.sampleEvents ||
        samplingStats.contextSwitchSamples != index.stats.contextSwitchSampleEvents ||
        samplingStats.dictionaryEntries != index.stats.sampleDictionaryEntries ||
        samplingStats.callstackPayloads != index.stats.callstackPayloads ||
        samplingStats.hardwareEvents != index.stats.hardwareSampleEvents ||
        samplingStats.hardwareAddresses != index.stats.hardwareSampleAddresses ||
        samplingStats.sampleBlocks != index.stats.sampleBlocks )
    { if( error.empty() ) error = "session_sampling_derived_audit_mismatch"; return false; }
    TraceSessionSchedulingStats schedulingStats;
    if( !AuditTraceSessionSchedulingDerived( sessionRoot, manifest, schedulingStats, error ) ||
        schedulingStats.contextSwitchRecords != index.stats.contextSwitchRecords ||
        schedulingStats.wakeupRecords != index.stats.threadWakeupRecords ||
        schedulingStats.threadEvents != index.stats.contextSwitchEvents ||
        schedulingStats.completeThreadEvents != index.stats.completeContextSwitchEvents ||
        schedulingStats.cpuEvents != index.stats.cpuContextSwitchEvents ||
        schedulingStats.completeCpuEvents != index.stats.completeCpuContextSwitchEvents ||
        schedulingStats.sourceGapEvents != index.stats.schedulingSourceGaps ||
        schedulingStats.topologyRecords != index.stats.cpuTopologyRecords ||
        schedulingStats.topologyCpus != index.stats.cpuTopologyCpus ||
        schedulingStats.threadSummaries != index.stats.threadSummaries ||
        schedulingStats.cpuUsagePoints != index.stats.cpuUsagePoints ||
        schedulingStats.threadBlocks != index.stats.schedulingThreadBlocks ||
        schedulingStats.cpuBlocks != index.stats.schedulingCpuBlocks )
    { if( error.empty() ) error = "session_scheduling_derived_audit_mismatch"; return false; }
    TraceSessionPlotStats plotStats;
    if( !AuditTraceSessionPlotDerived( sessionRoot, manifest, plotStats, error ) )
    { if( error.empty() ) error = "session_plot_derived_audit_mismatch"; return false; }
    TraceSessionMessageStats messageStats;
    if( !AuditTraceSessionMessageDerived( sessionRoot, manifest, messageStats, error ) )
    { if( error.empty() ) error = "session_message_derived_audit_mismatch"; return false; }
    TraceSessionLockStats lockStats;
    if( !AuditTraceSessionLockDerived( sessionRoot, manifest, lockStats, error ) )
    { if( error.empty() ) error = "session_lock_derived_audit_mismatch"; return false; }
    TraceSessionRelationStats relationStats;
    if( !AuditTraceSessionRelationDerived( sessionRoot, manifest, relationStats, error ) ||
        relationStats.relations != index.stats.relations )
    { if( error.empty() ) error = "session_relation_derived_audit_mismatch"; return false; }
    TraceSessionRuntimeStats runtimeStats;
    if( !AuditTraceSessionRuntimeDerived( sessionRoot, manifest, runtimeStats, error ) ||
        runtimeStats.domainStates != index.stats.runtimeDomainStates ||
        runtimeStats.scriptFrames != index.stats.scriptFrames ||
        runtimeStats.scriptStackEvents != index.stats.scriptStackEvents )
    { if( error.empty() ) error = "session_runtime_derived_audit_mismatch"; return false; }
    TraceSessionIoGfxStats ioGfxStats;
    if( !AuditTraceSessionIoGfxDerived( sessionRoot, manifest, ioGfxStats, error ) ||
        ioGfxStats.ioRequests != index.stats.ioRequests ||
        ioGfxStats.ioConfigs != index.stats.ioConfigs || ioGfxStats.ioStages != index.stats.ioStages ||
        ioGfxStats.gfxDispatches != index.stats.gfxDispatches ||
        ioGfxStats.gfxEntities != index.stats.gfxEntities || ioGfxStats.gfxLinks != index.stats.gfxLinks ||
        ioGfxStats.correlatedFrames != index.stats.correlatedFrames )
    { if( error.empty() ) error = "session_io_gfx_derived_audit_mismatch"; return false; }
    const auto& queueCounts = inventory.protocolInventory.events;
    static constexpr std::array MessageTypes {
        QueueType::Message, QueueType::MessageColor,
        QueueType::MessageCallstack, QueueType::MessageColorCallstack,
        QueueType::MessageAppInfo, QueueType::MessageLiteral,
        QueueType::MessageLiteralColor, QueueType::MessageLiteralCallstack,
        QueueType::MessageLiteralColorCallstack
    };
    for( size_t index = 0; index < MessageTypes.size(); ++index )
    {
        if( queueCounts[size_t( MessageTypes[index] )].count !=
            messageStats.eventCounts[index] )
        { error = "session_message_source_count_mismatch"; return false; }
    }
    if( queueCounts[size_t( QueueType::LockAnnounce )].count != lockStats.announceEvents ||
        queueCounts[size_t( QueueType::LockTerminate )].count != lockStats.terminateEvents ||
        queueCounts[size_t( QueueType::LockWait )].count != lockStats.waitEvents ||
        queueCounts[size_t( QueueType::LockObtain )].count != lockStats.obtainEvents ||
        queueCounts[size_t( QueueType::LockRelease )].count != lockStats.releaseEvents ||
        queueCounts[size_t( QueueType::LockSharedWait )].count != lockStats.sharedWaitEvents ||
        queueCounts[size_t( QueueType::LockSharedObtain )].count != lockStats.sharedObtainEvents ||
        queueCounts[size_t( QueueType::LockSharedRelease )].count != lockStats.sharedReleaseEvents ||
        queueCounts[size_t( QueueType::LockName )].count != lockStats.nameEvents ||
        queueCounts[size_t( QueueType::LockMark )].count != lockStats.markEvents )
    { error = "session_lock_source_count_mismatch"; return false; }
    if( queueCounts[size_t( QueueType::JnJobType )].count != index.stats.jobTypes ||
        queueCounts[size_t( QueueType::JnJobSchedule )].count != index.stats.jobSchedules ||
        queueCounts[size_t( QueueType::JnJobConfig )].count != index.stats.jobConfigs ||
        queueCounts[size_t( QueueType::JnJobDependency )].count != index.stats.jobDependencies ||
        queueCounts[size_t( QueueType::JnJobStage )].count != index.stats.jobStages )
    { error = "session_job_source_count_mismatch"; return false; }
    const auto cpuBegins = queueCounts[size_t( QueueType::ZoneBegin )].count +
        queueCounts[size_t( QueueType::ZoneBeginCallstack )].count +
        queueCounts[size_t( QueueType::ZoneBeginAllocSrcLoc )].count +
        queueCounts[size_t( QueueType::ZoneBeginAllocSrcLocCallstack )].count +
        queueCounts[size_t( QueueType::JnZoneBeginCallsite )].count;
    if( cpuBegins != index.stats.cpuZoneBegins ||
        queueCounts[size_t( QueueType::ZoneEnd )].count != index.stats.cpuZoneEnds ||
        index.stats.cpuZones != index.stats.cpuZoneBegins ||
        index.stats.completeCpuZones != index.stats.cpuZoneEnds )
    { error = "session_cpu_zone_source_count_mismatch"; return false; }
    const auto gpuBegins = queueCounts[size_t( QueueType::GpuZoneBegin )].count +
        queueCounts[size_t( QueueType::GpuZoneBeginCallstack )].count +
        queueCounts[size_t( QueueType::GpuZoneBeginAllocSrcLoc )].count +
        queueCounts[size_t( QueueType::GpuZoneBeginAllocSrcLocCallstack )].count +
        queueCounts[size_t( QueueType::GpuZoneBeginSerial )].count +
        queueCounts[size_t( QueueType::GpuZoneBeginCallstackSerial )].count +
        queueCounts[size_t( QueueType::GpuZoneBeginAllocSrcLocSerial )].count +
        queueCounts[size_t( QueueType::GpuZoneBeginAllocSrcLocCallstackSerial )].count +
        queueCounts[size_t( QueueType::JnGpuZoneBeginCallsite )].count;
    const auto gpuEnds = queueCounts[size_t( QueueType::GpuZoneEnd )].count +
        queueCounts[size_t( QueueType::GpuZoneEndSerial )].count;
    if( queueCounts[size_t( QueueType::GpuNewContext )].count != index.stats.gpuContexts ||
        gpuBegins != index.stats.gpuZoneBegins || gpuEnds != index.stats.gpuZoneEnds ||
        queueCounts[size_t( QueueType::GpuTime )].count != index.stats.gpuTimeEvents ||
        queueCounts[size_t( QueueType::GpuCalibration )].count != index.stats.gpuCalibrationEvents ||
        queueCounts[size_t( QueueType::GpuTimeSync )].count != index.stats.gpuSyncEvents ||
        index.stats.gpuZones != index.stats.gpuZoneBegins )
    { error = "session_gpu_zone_source_count_mismatch"; return false; }
    const auto memoryAllocations = queueCounts[size_t( QueueType::MemAlloc )].count +
        queueCounts[size_t( QueueType::MemAllocNamed )].count +
        queueCounts[size_t( QueueType::MemAllocCallstack )].count +
        queueCounts[size_t( QueueType::MemAllocCallstackNamed )].count +
        queueCounts[size_t( QueueType::JnMemAllocCallsiteNamed )].count;
    const auto memoryFrees = queueCounts[size_t( QueueType::MemFree )].count +
        queueCounts[size_t( QueueType::MemFreeNamed )].count +
        queueCounts[size_t( QueueType::MemFreeCallstack )].count +
        queueCounts[size_t( QueueType::MemFreeCallstackNamed )].count;
    const auto memoryDiscards = queueCounts[size_t( QueueType::MemDiscard )].count +
        queueCounts[size_t( QueueType::MemDiscardCallstack )].count;
    if( memoryAllocations != index.stats.memoryAllocations ||
        memoryFrees != index.stats.memoryFrees ||
        memoryDiscards != index.stats.memoryDiscards ||
        index.stats.memoryEvents != index.stats.memoryAllocations )
    { error = "session_memory_source_count_mismatch"; return false; }
    const auto samples = queueCounts[size_t( QueueType::CallstackSample )].count +
        queueCounts[size_t( QueueType::CallstackSampleRef )].count;
    const auto contextSwitchSamples =
        queueCounts[size_t( QueueType::CallstackSampleContextSwitch )].count +
        queueCounts[size_t( QueueType::CallstackSampleContextSwitchRef )].count;
    const auto hardwareSamples =
        queueCounts[size_t( QueueType::HwSampleCpuCycle )].count +
        queueCounts[size_t( QueueType::HwSampleInstructionRetired )].count +
        queueCounts[size_t( QueueType::HwSampleCacheReference )].count +
        queueCounts[size_t( QueueType::HwSampleCacheMiss )].count +
        queueCounts[size_t( QueueType::HwSampleBranchRetired )].count +
        queueCounts[size_t( QueueType::HwSampleBranchMiss )].count;
    if( samples != index.stats.sampleEvents ||
        contextSwitchSamples != index.stats.contextSwitchSampleEvents ||
        queueCounts[size_t( QueueType::CallstackSampleDictionary )].count != index.stats.sampleDictionaryEntries ||
        queueCounts[size_t( QueueType::CallstackPayload )].count != index.stats.callstackPayloads ||
        hardwareSamples != index.stats.hardwareSampleEvents )
    { error = "session_sampling_source_count_mismatch"; return false; }
    if( queueCounts[size_t( QueueType::ContextSwitch )].count != index.stats.contextSwitchRecords ||
        queueCounts[size_t( QueueType::ThreadWakeup )].count != index.stats.threadWakeupRecords ||
        queueCounts[size_t( QueueType::CpuTopology )].count != index.stats.cpuTopologyRecords )
    { error = "session_scheduling_source_count_mismatch"; return false; }
    if( queueCounts[size_t( QueueType::ThreadName )].count != schedulingStats.threadNameRecords ||
        queueCounts[size_t( QueueType::TidToPid )].count != schedulingStats.tidToPidRecords ||
        queueCounts[size_t( QueueType::ThreadGroupHint )].count != schedulingStats.groupHintRecords ||
        queueCounts[size_t( QueueType::ExternalNameMetadata )].count != schedulingStats.externalNameMetadataRecords ||
        queueCounts[size_t( QueueType::ExternalName )].count != schedulingStats.externalNameRecords ||
        queueCounts[size_t( QueueType::ExternalThreadName )].count != schedulingStats.externalThreadNameRecords ||
        queueCounts[size_t( QueueType::FiberName )].count != schedulingStats.fiberNameRecords ||
        queueCounts[size_t( QueueType::FiberEnter )].count != schedulingStats.fiberEnterRecords ||
        queueCounts[size_t( QueueType::FiberLeave )].count != schedulingStats.fiberLeaveRecords )
    { error = "session_thread_source_count_mismatch"; return false; }
    const auto plotDataEvents = queueCounts[size_t( QueueType::PlotDataInt )].count +
        queueCounts[size_t( QueueType::PlotDataFloat )].count +
        queueCounts[size_t( QueueType::PlotDataDouble )].count;
    if( plotDataEvents != plotStats.dataEvents || plotStats.points > plotStats.dataEvents ||
        queueCounts[size_t( QueueType::PlotConfig )].count != plotStats.configEvents ||
        queueCounts[size_t( QueueType::PlotName )].count != plotStats.nameEvents )
    { error = "session_plot_source_count_mismatch"; return false; }
    if( queueCounts[size_t( QueueType::JnRelation )].count != index.stats.relations )
    { error = "session_relation_source_count_mismatch"; return false; }
    if( queueCounts[size_t( QueueType::JnRuntimeDomainState )].count != index.stats.runtimeDomainStates ||
        queueCounts[size_t( QueueType::JnScriptFrame )].count != index.stats.scriptFrames ||
        queueCounts[size_t( QueueType::JnScriptStack )].count != index.stats.scriptStackEvents )
    { error = "session_runtime_source_count_mismatch"; return false; }
    if( queueCounts[size_t( QueueType::JnIoRequest )].count != index.stats.ioRequests ||
        queueCounts[size_t( QueueType::JnIoConfig )].count != index.stats.ioConfigs ||
        queueCounts[size_t( QueueType::JnIoStage )].count != index.stats.ioStages ||
        queueCounts[size_t( QueueType::JnGfxDispatch )].count != index.stats.gfxDispatches ||
        queueCounts[size_t( QueueType::JnGfxEntity )].count != index.stats.gfxEntities ||
        queueCounts[size_t( QueueType::JnGfxLink )].count != index.stats.gfxLinks ||
        queueCounts[size_t( QueueType::JnFrame )].count != index.stats.correlatedFrames )
    { error = "session_io_gfx_source_count_mismatch"; return false; }
    if( queueCounts[size_t( QueueType::FrameImageData )].count != index.stats.frameImageDataEvents ||
        queueCounts[size_t( QueueType::FrameImage )].count != index.stats.frameImageEvents )
    { error = "session_frame_image_source_count_mismatch"; return false; }
    stats = index.stats;
    return true;
}

}

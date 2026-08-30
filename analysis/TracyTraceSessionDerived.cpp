#include "TracyTraceSessionDerived.hpp"

#include "TracyGpuAnalysisStore.hpp"
#include "TracyHash.hpp"
#include "TracyTraceSessionFrames.hpp"
#include "TracyTraceSessionGpuCanonical.hpp"
#include "TracyTraceSessionJobs.hpp"
#include "TracyTraceSessionCpuZones.hpp"
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

struct DomainIndexEntry
{
    uint64_t sourceSequence = 0;
    uint64_t journalMonotonicNs = 0;
    uint64_t protocolFrameOrdinal = 0;
    int64_t semanticTime = 0;
    uint32_t protocolFrameOffset = 0;
    uint32_t threadContext = 0;
    uint16_t type = 0;
    uint8_t kind = 0;
    uint8_t hasSemanticTime = 0;
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
    std::vector<DomainIndexEntry>* entries = nullptr;
    DomainIndexHeader* header = nullptr;
    TraceSessionDerivedStats* stats = nullptr;
};

bool IndexRecord( const TraceSessionCanonicalRecord& record, void* userData, std::string& )
{
    auto& state = *static_cast<IndexVisitorState*>( userData );
    state.entries->push_back( { record.sourceSequence, record.journalMonotonicNs,
        record.protocolFrameOrdinal, record.semanticTime, record.protocolFrameOffset,
        record.threadContext, record.type, uint8_t( record.kind ),
        uint8_t( record.hasSemanticTime ? 1 : 0 ) } );
    switch( record.kind )
    {
    case TraceSessionCanonicalRecordKind::ProtocolEvent: state.header->protocolEvents++; break;
    case TraceSessionCanonicalRecordKind::ProtocolFrame: state.header->protocolFrames++; break;
    case TraceSessionCanonicalRecordKind::TransportRecord: state.header->transportRecords++; break;
    }
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
    out << "frame_sets " << value.stats.frameSets << '\n';
    out << "frames " << value.stats.frames << '\n';
    out << "complete_frames " << value.stats.completeFrames << '\n';
    out << "job_types " << value.stats.jobTypes << '\n';
    out << "jobs " << value.stats.jobs << '\n';
    out << "job_schedules " << value.stats.jobSchedules << '\n';
    out << "job_configs " << value.stats.jobConfigs << '\n';
    out << "job_dependencies " << value.stats.jobDependencies << '\n';
    out << "job_stages " << value.stats.jobStages << '\n';
    out << "cpu_zones " << value.stats.cpuZones << '\n';
    out << "complete_cpu_zones " << value.stats.completeCpuZones << '\n';
    out << "cpu_zone_sources " << value.stats.cpuZoneSources << '\n';
    out << "cpu_zone_begins " << value.stats.cpuZoneBegins << '\n';
    out << "cpu_zone_ends " << value.stats.cpuZoneEnds << '\n';
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
        else if( key == "frame_sets" ) in >> value.stats.frameSets;
        else if( key == "frames" ) in >> value.stats.frames;
        else if( key == "complete_frames" ) in >> value.stats.completeFrames;
        else if( key == "job_types" ) in >> value.stats.jobTypes;
        else if( key == "jobs" ) in >> value.stats.jobs;
        else if( key == "job_schedules" ) in >> value.stats.jobSchedules;
        else if( key == "job_configs" ) in >> value.stats.jobConfigs;
        else if( key == "job_dependencies" ) in >> value.stats.jobDependencies;
        else if( key == "job_stages" ) in >> value.stats.jobStages;
        else if( key == "cpu_zones" ) in >> value.stats.cpuZones;
        else if( key == "complete_cpu_zones" ) in >> value.stats.completeCpuZones;
        else if( key == "cpu_zone_sources" ) in >> value.stats.cpuZoneSources;
        else if( key == "cpu_zone_begins" ) in >> value.stats.cpuZoneBegins;
        else if( key == "cpu_zone_ends" ) in >> value.stats.cpuZoneEnds;
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
            file.fileBytes != sizeof( header ) + header.recordCount * sizeof( DomainIndexEntry ) )
        { error = "session_index_header_mismatch"; return false; }
        records += header.recordCount; bytes += file.fileBytes;
    }
    if( records != index.stats.indexedRecords || bytes != index.stats.indexBytes )
    { error = "session_index_total_mismatch"; return false; }
    return true;
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
    IndexManifest index;
    index.sourceSha256 = manifest.source.sha256;
    index.sourceSize = manifest.source.fileSize;
    index.generation = manifest.generation;
    const auto eventShardCount = std::count_if( manifest.shards.begin(), manifest.shards.end(),
        []( const auto& shard ) { return shard.domain != "checkpoint"; } );
    size_t completedShards = 0;
    for( const auto& shard : manifest.shards )
    {
        if( shard.domain == "checkpoint" ) continue;
        if( control.stopToken.stop_requested() ) { error = "cancelled_resumable"; return false; }
        DomainIndexHeader header;
        header.domain = uint32_t( DomainFromName( shard.domain ) );
        if( header.domain >= uint32_t( TraceSessionProtocolDomain::Count ) ) header.domain = uint32_t( TraceSessionProtocolDomain::Other );
        header.shardId = shard.shardId;
        std::vector<DomainIndexEntry> entries;
        if( shard.recordCount > uint64_t( std::numeric_limits<size_t>::max() ) )
        { error = "session_index_record_limit"; return false; }
        entries.reserve( size_t( shard.recordCount ) );
        IndexVisitorState state { &entries, &header, &index.stats };
        if( !VisitTraceSessionCanonicalShard( sessionRoot, shard, IndexRecord, &state, error ) ) return false;
        header.recordCount = entries.size();
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
        if( !entries.empty() ) out.write( reinterpret_cast<const char*>( entries.data() ),
            std::streamsize( entries.size() * sizeof( entries.front() ) ) );
        out.flush();
        if( !out ) { error = "session_index_write_failed"; return false; }
        out.close();
        if( !AtomicReplace( temporary, target, error ) ) return false;
        IndexFile file;
        file.domain = header.domain; file.shardId = shard.shardId; file.recordCount = header.recordCount;
        file.fileBytes = sizeof( header ) + entries.size() * sizeof( DomainIndexEntry );
        file.sha256 = Sha256File( target ); file.relativePath = relative;
        index.files.push_back( file );
        index.stats.indexedRecords += header.recordCount;
        index.stats.indexedProtocolEvents += header.protocolEvents;
        index.stats.indexedProtocolFrames += header.protocolFrames;
        index.stats.indexedTransportRecords += header.transportRecords;
        index.stats.indexBytes += file.fileBytes;
        index.stats.indexFiles++;
        index.stats.domains[header.domain] += header.recordCount;
        if( control.progress ) control.progress( eventShardCount == 0 ? 1.f :
            float( ++completedShards ) / float( eventShardCount ), "session-domain-index" );
    }

    TraceSessionFrameStats frameStats;
    if( !BuildTraceSessionFrameDerived( sessionRoot, manifest,
        index.stats.semanticTimePresent, index.stats.lastSemanticTimeRaw,
        frameStats, error ) ) return false;
    index.stats.frameSets = frameStats.frameSets;
    index.stats.frames = frameStats.frames;
    index.stats.completeFrames = frameStats.completeFrames;

    TraceSessionJobStats jobStats;
    if( !BuildTraceSessionJobDerived( sessionRoot, manifest, jobStats, error ) ) return false;
    index.stats.jobTypes = jobStats.jobTypes;
    index.stats.jobs = jobStats.jobs;
    index.stats.jobSchedules = jobStats.schedules;
    index.stats.jobConfigs = jobStats.configs;
    index.stats.jobDependencies = jobStats.dependencies;
    index.stats.jobStages = jobStats.stages;

    TraceSessionCpuZoneStats cpuZoneStats;
    if( !BuildTraceSessionCpuZoneDerived( sessionRoot, manifest, cpuZoneStats, error ) ) return false;
    index.stats.cpuZones = cpuZoneStats.zones;
    index.stats.completeCpuZones = cpuZoneStats.completeZones;
    index.stats.cpuZoneSources = cpuZoneStats.sourceLocations;
    index.stats.cpuZoneBegins = cpuZoneStats.beginEvents;
    index.stats.cpuZoneEnds = cpuZoneStats.endEvents;

    const auto gpuCatalogEvents = inventory.protocolInventory.domains[
        size_t( TraceSessionProtocolDomain::GpuCatalog )].count;
    if( gpuCatalogEvents != 0 )
    {
        GpuAnalysisSidecarControl gpuControl;
        gpuControl.stopToken = control.stopToken;
        gpuControl.minimumFreeBytes = control.minimumFreeBytes;
        gpuControl.progress = control.progress;
        TraceSessionGpuDerivedStats gpuStats;
        if( !BuildTraceSessionGpuAnalysisDerived( sessionRoot, manifest, gpuControl, gpuStats, error ) ) return false;
        index.stats.gpuResources = gpuStats.resourceCount;
        index.stats.gpuAllocations = gpuStats.allocationCount;
        index.stats.gpuPasses = gpuStats.passCount;
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
            gpu->passCount != index.stats.gpuPasses )
        { if( error.empty() ) error = "session_gpu_derived_audit_mismatch"; return false; }
    }
    const auto frameReader = TraceSessionFrameReader::Open( sessionRoot, manifest, error );
    if( !frameReader || frameReader->Stats().frameSets != index.stats.frameSets ||
        frameReader->Stats().frames != index.stats.frames ||
        frameReader->Stats().completeFrames != index.stats.completeFrames )
    { if( error.empty() ) error = "session_frame_derived_audit_mismatch"; return false; }
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
        cpuZoneStats.completeZones != index.stats.completeCpuZones ||
        cpuZoneStats.sourceLocations != index.stats.cpuZoneSources ||
        cpuZoneStats.beginEvents != index.stats.cpuZoneBegins ||
        cpuZoneStats.endEvents != index.stats.cpuZoneEnds )
    { if( error.empty() ) error = "session_cpu_zone_derived_audit_mismatch"; return false; }
    const auto& queueCounts = inventory.protocolInventory.events;
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
    stats = index.stats;
    return true;
}

}

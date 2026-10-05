#include "TracyEventStoreTraceSource.hpp"
#include "TracyCpuZoneScanSource.hpp"
#include "TracyWindowedTraceSource.hpp"

#include "TracyBoundedScanCursor.hpp"
#include "TracyEventStore.hpp"
#include "TracyFileHeader.hpp"
#include "TracyQueue.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <deque>
#include <fstream>
#include <limits>
#include <mutex>
#include <map>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace tracy::query
{
namespace
{

namespace store = tracy::stream;
using namespace tracy::analysis;

constexpr uint32_t SkeletonFormat = 1;
// Format of the validation summary cached beside the skeleton: findings,
// referenced callstacks, GPU memory markers and the root positions of each
// thread's CPU zones. A summary of another format is rebuilt.
constexpr uint32_t ValidationFormat = 4;
// Recorded root positions are at least this many zones apart.
constexpr uint64_t RootSpacing = 4096;
// Whole-domain reads above this many records are refused; the paged methods
// read any size.
constexpr uint64_t UnboundedReadLimit = 8'000'000;

std::filesystem::path WithSuffix( const std::filesystem::path& path, const char* suffix )
{
    auto result = path;
    result += suffix;
    return result;
}

std::string ReadText( const std::filesystem::path& path )
{
    std::ifstream input( path, std::ios::binary );
    if( !input ) return {};
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

// The skeleton snapshot is valid for the store whose data hash and schema it
// records. Its manifest is written after the snapshot is published.
std::string SkeletonManifest( const store::StoreInfo& info, uint64_t bytes )
{
    return "format=" + std::to_string( SkeletonFormat ) + "\nschema=" + std::to_string( info.schema ) + "\nstore=" + info.dataSha256 +
        "\nblocks=" + info.blockSha256 + "\nbytes=" + std::to_string( bytes ) + "\n";
}

std::filesystem::path EnsureSkeleton( const std::filesystem::path& journal, const store::EventStore& events,
    const WorkerTraceSource::StateCallback& callback )
{
    const auto skeleton = EventStoreTraceSource::SkeletonPath( journal );
    const auto manifest = WithSuffix( journal, ".query-skeleton.manifest" );
    std::error_code error;
    if( std::filesystem::is_regular_file( skeleton, error ) )
    {
        const auto bytes = std::filesystem::file_size( skeleton, error );
        if( !error && ReadText( manifest ) == SkeletonManifest( events.Info(), bytes ) ) return skeleton;
    }
    if( callback ) callback( TraceSourceState::Indexing );
    std::filesystem::remove( manifest, error );
    store::StoreExportOptions options;
    options.skeleton = true;
    options.overwrite = true;
    events.ExportSnapshot( skeleton, options );
    auto temporary = WithSuffix( manifest, ".tmp" );
    {
        std::ofstream output( temporary, std::ios::binary | std::ios::trunc );
        output << SkeletonManifest( events.Info(), std::filesystem::file_size( skeleton ) );
        if( !output ) throw std::runtime_error( "cannot write the skeleton snapshot manifest" );
    }
    std::filesystem::rename( temporary, manifest );
    return skeleton;
}

struct SourceKey
{
    uint8_t kind = 0;
    uint64_t key = 0;
    bool operator==( const SourceKey& ) const = default;
};
struct SourceKeyHash
{
    size_t operator()( const SourceKey& k ) const
    {
        uint64_t x = k.key ^ ( uint64_t( k.kind ) << 56 );
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdull;
        x ^= x >> 33;
        return size_t( x );
    }
};

struct Ancestor
{
    uint64_t id = 0, ordinal = 0;
};

// Ordinal of a zone's parent within its thread, from the open ancestors when
// they hold it, otherwise from the store; nothing when the zone has no parent
// or the store does not have it.
std::optional<uint64_t> ParentOrdinal( const store::EventStore& events, uint64_t thread, std::vector<Ancestor>& ancestors,
    const store::StoreZoneRecord& zone, uint64_t ordinal )
{
    std::optional<uint64_t> parent;
    if( zone.parent )
    {
        if( zone.depth > 0 && ancestors.size() >= zone.depth && ancestors[zone.depth - 1].id == zone.parent ) parent = ancestors[zone.depth - 1].ordinal;
        else
        {
            parent = events.ZoneOrdinal( thread, zone.parent );
            ancestors.assign( zone.depth, Ancestor{} );
            if( zone.depth > 0 && parent ) ancestors[zone.depth - 1] = { zone.parent, *parent };
        }
    }
    if( ancestors.size() < zone.depth ) ancestors.resize( zone.depth );
    ancestors.resize( zone.depth + 1 );
    ancestors[zone.depth] = { zone.id, ordinal };
    return parent;
}

class StoreTraceSource final : public TraceSource, public NativeBoundedTraceSource, public GpuCatalogBoundedScanSource, public CpuZoneScanSource,
    public FrameTriageSource, public WindowedDomainSource
{
public:
    StoreTraceSource( std::unique_ptr<store::EventStore> events, std::unique_ptr<WorkerTraceSource> worker, std::filesystem::path journal )
        : m_events( std::move( events ) )
        , m_worker( std::move( worker ) )
        , m_fingerprint( m_events->Info().sourceSha256 )
        , m_journal( std::move( journal ) )
    {
        m_refPrefix = "tracy:v1:" + m_fingerprint.substr( 0, 16 ) + ':';
        m_threads = m_events->ZoneThreads();
        m_threadBase.reserve( m_threads.size() + 1 );
        uint64_t total = 0;
        for( const auto& thread : m_threads )
        {
            m_threadBase.push_back( total );
            m_threadIndex.emplace( thread.thread, m_threadBase.size() - 1 );
            total += thread.zones;
        }
        m_threadBase.push_back( total );
        m_scanCursor = m_events->ZoneCursor();
        m_lookupCursor = m_events->ZoneCursor();
        for( auto& location : m_worker->GetSourceLocations() ) m_locations.emplace( location.nativeId, std::move( location ) );
        for( const auto& value : m_events->SourceNumbers() ) m_sourceIds.emplace( SourceKey{ value.kind, value.key }, value.number );
        for( auto& callsite : m_worker->GetCallsites() )
        {
            const auto source = m_worker->ParseEntityRef( callsite.sourceLocationRef, "source" );
            if( !source ) continue;
            m_callsitesByZone.try_emplace( ( *source << 32 ) | callsite.callstack, std::move( callsite ) );
        }
        for( const auto& value : m_events->CallsiteStacks() ) m_callsiteStacks.emplace( value.first, value.second );
        m_stackIds = m_events->StackIds();
        m_gpuQueryIds = m_worker->GetTraceInfo().traceVersion >= FileVersion( 0, 12, 4 );
        const auto contexts = m_events->GpuContextIds();
        for( size_t n = 0; n < contexts.size(); ++n ) m_contextIndex.emplace( contexts[n], n );
        m_gpuGroups = m_events->GpuZoneGroups();
        m_gpuBase.reserve( m_gpuGroups.size() + 1 );
        total = 0;
        for( const auto& group : m_gpuGroups )
        {
            m_gpuBase.push_back( total );
            total += group.zones;
            const auto index = m_contextIndex.find( group.context );
            if( index != m_contextIndex.end() ) m_gpuCompletedByContext[index->second] += group.completed;
        }
        m_gpuBase.push_back( total );
        m_gpuScanCursor = m_events->GpuZoneCursor();
        m_gpuLookupCursor = m_events->GpuZoneCursor();
        const auto capture = m_events->CaptureMetadata();
        m_initBegin = capture.initBegin;
        m_timerMultiplier = capture.timerMultiplier;
        for( auto& [type, name] : m_events->JobTypeNames() ) m_jobTypeNames[type] = name ? std::move( *name ) : std::string( "???" );
        for( const auto& frame : m_worker->GetCorrelatedFrameEvents() )
            if( frame.phase == uint8_t( JnFramePhase::Begin ) && ( frame.flags & uint8_t( JnFrameFlags::Canonical ) ) != 0 )
                m_frameIdsBySequence[uint32_t( frame.frameId )] = frame.frameId;
        for( auto& callsite : m_worker->GetCallsites() ) m_callsitesById.try_emplace( callsite.callsiteId, std::move( callsite ) );
        m_zoneValidation = LoadOrBuildZoneValidation();
        BuildZoneScanTables();
    }

    uint32_t NativeBoundedScanVersion() const override { return NativeBoundedScanSchemaVersion; }

    // Compact CPU zone records: the zones of ScanCpuZones, read by readers
    // with their own store connection, without zone text.
    const std::vector<CpuZoneScanThread>& CpuZoneScanThreads() const override { return m_scanThreads; }
    const std::vector<CpuZoneScanLocation>& CpuZoneScanLocations() const override { return m_scanLocations; }
    void CpuZoneScanRef( uint64_t ordinal, std::string& ref ) const override
    {
        char digits[16];
        const auto end = std::to_chars( digits, digits + sizeof( digits ), ordinal, 16 ).ptr;
        ref.assign( m_refPrefix ).append( "cpu-zone:" ).append( digits, end );
    }
    std::unique_ptr<CpuZoneScanReader> OpenCpuZoneScanReader() const override
    {
        store::StoreOptions options;
        options.verifyContent = false;
        options.threadConfined = true;
        options.cacheBytes = 8ull << 20;
        options.resultBytes = 1ull << 20;
        return std::make_unique<ZoneScanReader>( *this, store::EventStore::Open( m_journal, options ) );
    }

    std::vector<Capability> GetCapabilities() const override
    {
        auto result = m_worker->GetCapabilities();
        const auto zones = m_threadBase.back();
        for( auto& item : result )
        {
            if( item.domain == "zone.cpu" )
            {
                item.present = zones != 0;
                item.queryable = item.present;
                item.indexed = item.present;
                item.reason = item.present ? "read from the event store's CPU zone blocks" : "data is absent from the event store";
            }
            else if( item.domain == "zone.gpu" )
            {
                item.present = m_gpuBase.back() != 0;
                item.queryable = item.present;
                item.indexed = item.present;
                item.reason = item.present ? "read from the event store's GPU zones" : "data is absent from the event store";
            }
            else if( item.domain == "timeline" )
            {
                item.present = item.present || zones != 0 || m_gpuBase.back() != 0;
                item.queryable = item.present;
                item.indexed = item.present;
            }
        }
        return result;
    }

    TraceReadView AcquireReadView() const override { return m_worker->AcquireReadView(); }

    TraceInfoDto GetTraceInfo() const override
    {
        auto result = m_worker->GetTraceInfo();
        result.counts.cpuZones = m_threadBase.back();
        result.counts.gpuZones = m_gpuBase.back();
        // Record counts of the domains the skeleton omits, as the Worker counts
        // a full snapshot's records.
        result.counts.jobs = PacketCount( QueueType::JnJobSchedule );
        result.counts.jobDependencies = PacketCount( QueueType::JnJobDependency );
        result.counts.jobStages = PacketCount( QueueType::JnJobStage );
        result.counts.gfxDispatches = PacketCount( QueueType::JnGfxDispatch );
        result.counts.gfxEntities = PacketCount( QueueType::JnGfxEntity );
        result.counts.gfxLinks = PacketCount( QueueType::JnGfxLink );
        result.counts.relations = PacketCount( QueueType::JnRelation );
        return result;
    }

    std::vector<ThreadDto> GetThreads() const override
    {
        auto result = m_worker->GetThreads();
        for( auto& thread : result )
        {
            const auto found = m_threadIndex.find( thread.nativeId );
            thread.zoneCount = found == m_threadIndex.end() ? 0 : m_threads[found->second].zones;
        }
        return result;
    }

    std::vector<CpuZoneDto> ScanCpuZones( const ScanRange& range ) const override
    {
        std::lock_guard lock( m_mutex );
        std::vector<CpuZoneDto> result;
        if( range.limit == 0 ) return result;
        const bool full = range.startNs == std::numeric_limits<int64_t>::min() && range.endNs == std::numeric_limits<int64_t>::max();
        auto& scan = m_zoneScan;
        if( !scan.valid || scan.startNs != range.startNs || scan.endNs != range.endNs || scan.nextOffset != range.offset )
        {
            scan.valid = true;
            scan.startNs = range.startNs;
            scan.endNs = range.endNs;
            scan.nextOffset = 0;
            scan.matched = 0;
            if( full )
            {
                const auto total = m_threadBase.back();
                const auto index = std::min<uint64_t>( range.offset, total );
                StartThread( scan, size_t( std::upper_bound( m_threadBase.begin(), m_threadBase.end(), index ) - m_threadBase.begin() ) - 1, index );
                scan.matched = index;
                scan.nextOffset = range.offset;
            }
            else StartThread( scan, 0, 0 );
        }
        store::StoreZoneRecord zone;
        while( result.size() < range.limit )
        {
            if( scan.thread >= m_threads.size() ) break;
            if( !m_scanCursor->Next( zone ) )
            {
                StartThread( scan, scan.thread + 1, scan.thread + 1 < m_threads.size() ? m_threadBase[scan.thread + 1] : m_threadBase.back() );
                continue;
            }
            const auto ordinal = m_scanCursor->Ordinal() - 1;
            const auto parent = ParentOrdinal( scan.ancestors, scan.thread, zone, ordinal );
            if( !full )
            {
                const auto end = zone.open ? zone.start : zone.end;
                if( !( zone.start < range.endNs && end > range.startNs ) ) continue;
                if( scan.matched++ < range.offset ) continue;
            }
            result.emplace_back( CpuDto( zone, scan.thread, ordinal, parent ) );
        }
        scan.nextOffset = range.offset + result.size();
        return result;
    }

    std::optional<CpuZoneDto> GetCpuZone( std::string_view ref ) const override
    {
        std::lock_guard lock( m_mutex );
        const auto position = Position( ref );
        if( !position ) return std::nullopt;
        m_lookupCursor->Seek( m_threads[position->first].thread, position->second );
        store::StoreZoneRecord zone;
        if( !m_lookupCursor->Next( zone ) ) return std::nullopt;
        std::optional<uint64_t> parent;
        if( zone.parent ) parent = m_events->ZoneOrdinal( m_threads[position->first].thread, zone.parent );
        return CpuDto( zone, position->first, position->second, parent );
    }

    std::vector<CpuZoneDto> GetCpuZoneChildren( std::string_view ref, size_t offset, size_t limit ) const override
    {
        std::lock_guard lock( m_mutex );
        std::vector<CpuZoneDto> result;
        const auto position = Position( ref );
        if( !position || limit == 0 ) return result;
        const auto thread = m_threads[position->first].thread;
        m_lookupCursor->Seek( thread, position->second );
        store::StoreZoneRecord zone;
        if( !m_lookupCursor->Next( zone ) || zone.children == 0 ) return result;
        const auto depth = zone.depth, wanted = zone.children;
        size_t found = 0;
        while( found < wanted && result.size() < limit && m_lookupCursor->Next( zone ) && zone.depth > depth )
        {
            if( zone.depth != depth + 1 ) continue;
            if( found++ >= offset ) result.emplace_back( CpuDto( zone, position->first, m_lookupCursor->Ordinal() - 1, position->second ) );
        }
        return result;
    }

    std::optional<std::string> GetCpuZoneRef( uint64_t index ) const override
    {
        return index < m_threadBase.back() ? std::optional( Ref( "cpu-zone", index ) ) : std::nullopt;
    }

    std::vector<GpuContextDto> GetGpuContexts() const override
    {
        auto result = m_worker->GetGpuContexts();
        for( auto& context : result )
        {
            const auto found = m_gpuCompletedByContext.find( context.index );
            context.zoneCount = found == m_gpuCompletedByContext.end() ? 0 : found->second;
        }
        return result;
    }

    std::vector<GpuZoneDto> ScanGpuZones( const ScanRange& range ) const override
    {
        std::lock_guard lock( m_mutex );
        std::vector<GpuZoneDto> result;
        if( range.limit == 0 ) return result;
        const bool full = range.startNs == std::numeric_limits<int64_t>::min() && range.endNs == std::numeric_limits<int64_t>::max();
        auto& scan = m_gpuScan;
        if( !scan.valid || scan.startNs != range.startNs || scan.endNs != range.endNs || scan.nextOffset != range.offset )
        {
            scan.valid = true;
            scan.startNs = range.startNs;
            scan.endNs = range.endNs;
            scan.matched = 0;
            if( full )
            {
                const auto index = std::min<uint64_t>( range.offset, m_gpuBase.back() );
                StartGroup( scan, size_t( std::upper_bound( m_gpuBase.begin(), m_gpuBase.end(), index ) - m_gpuBase.begin() ) - 1, index );
            }
            else StartGroup( scan, 0, 0 );
        }
        store::StoreGpuZoneRecord zone;
        while( result.size() < range.limit )
        {
            if( scan.thread >= m_gpuGroups.size() ) break;
            if( !m_gpuScanCursor->Next( zone ) )
            {
                StartGroup( scan, scan.thread + 1, scan.thread + 1 < m_gpuGroups.size() ? m_gpuBase[scan.thread + 1] : m_gpuBase.back() );
                continue;
            }
            const auto ordinal = m_gpuScanCursor->Ordinal() - 1;
            const auto parent = GpuParentOrdinal( scan.ancestors, scan.thread, zone, ordinal );
            if( !full )
            {
                const auto end = zone.gpuEnd >= 0 ? zone.gpuEnd : zone.gpuStart;
                if( !( zone.gpuStart < range.endNs && end > range.startNs ) ) continue;
                if( scan.matched++ < range.offset ) continue;
            }
            result.emplace_back( GpuDto( zone, scan.thread, ordinal, parent ) );
        }
        scan.nextOffset = range.offset + result.size();
        return result;
    }

    std::optional<GpuZoneDto> GetGpuZone( std::string_view ref ) const override
    {
        std::lock_guard lock( m_mutex );
        const auto position = GpuPosition( ref );
        if( !position ) return std::nullopt;
        m_gpuLookupCursor->Seek( position->first, position->second );
        store::StoreGpuZoneRecord zone;
        if( !m_gpuLookupCursor->Next( zone ) ) return std::nullopt;
        std::optional<uint64_t> parent;
        if( zone.parent ) parent = m_events->GpuZoneOrdinal( position->first, zone.parent );
        return GpuDto( zone, position->first, position->second, parent );
    }

    std::vector<GpuZoneDto> GetGpuZoneChildren( std::string_view ref, size_t offset, size_t limit ) const override
    {
        std::lock_guard lock( m_mutex );
        std::vector<GpuZoneDto> result;
        const auto position = GpuPosition( ref );
        if( !position || limit == 0 ) return result;
        m_gpuLookupCursor->Seek( position->first, position->second );
        store::StoreGpuZoneRecord zone;
        if( !m_gpuLookupCursor->Next( zone ) || zone.children == 0 ) return result;
        const auto depth = zone.depth, wanted = zone.children;
        const auto parentId = zone.id;
        size_t found = 0;
        while( found < wanted && result.size() < limit && m_gpuLookupCursor->Next( zone ) && zone.depth > depth )
        {
            if( zone.parent != parentId ) continue;
            if( found++ >= offset ) result.emplace_back( GpuDto( zone, position->first, m_gpuLookupCursor->Ordinal() - 1, position->second ) );
        }
        return result;
    }

    std::optional<std::string> GetGpuZoneRef( uint64_t index ) const override
    {
        return index < m_gpuBase.back() ? std::optional( Ref( "gpu-zone", index ) ) : std::nullopt;
    }

    // Memory events come from the skeleton; the CPU zones running where they
    // were allocated and freed come from the store.
    std::vector<MemoryEventDto> ScanMemoryEvents( const ScanRange& range ) const override
    {
        auto result = m_worker->ScanMemoryEvents( range );
        for( auto& event : result ) AttachZones( event );
        return result;
    }
    std::optional<MemoryEventDto> GetMemoryEvent( const MemoryEventKey& key ) const override
    {
        auto result = m_worker->GetMemoryEvent( key );
        if( result ) AttachZones( *result );
        return result;
    }

    // WorkerTraceSource::GetGpuMemoryAttribution with the zones, graphics
    // records and relations read from the store.
    GpuMemoryAttribution GetGpuMemoryAttribution() const override
    {
        std::vector<GpuMemoryCpuZoneInput> cpuInputs;
        {
            std::lock_guard lock( m_mutex );
            store::StoreZoneRecord zone;
            for( const auto& [thread, ordinal] : m_gpuMemoryMarkers )
            {
                m_lookupCursor->Seek( m_threads[thread].thread, ordinal );
                if( !m_lookupCursor->Next( zone ) || zone.open || !zone.hasText ) continue;
                const auto sourceId = m_sourceIds.find( SourceKey{ zone.skind, zone.skey } );
                if( sourceId == m_sourceIds.end() ) continue;
                const auto location = m_locations.find( sourceId->second );
                if( location == m_locations.end() ) continue;
                const auto marker = location->second.name.empty() ? location->second.function : location->second.name;
                cpuInputs.push_back( { m_threadBase[thread] + ordinal, marker, zone.hasName ? zone.name : marker, zone.text,
                    m_threads[thread].thread, zone.start, zone.end } );
            }
        }
        struct Entity
        {
            uint8_t kind = 0, gpuContext = 0;
            uint32_t gpuQueryId = 0;
            int64_t time = 0;
        };
        std::unordered_map<uint64_t, uint64_t> segmentByPass, referenceByPass, referenceBySegment;
        std::unordered_set<uint64_t> submittedCommandLists, gpuSegmentReferenceTokens, wantedEntities;
        Packets( QueueType::JnGfxLink, 0, SIZE_MAX, [&]( const QueueItem& e, const store::StoreExportPacket&, uint64_t ) {
            const auto& link = e.jnGfxLink;
            if( link.relation == 5 ) segmentByPass[link.sourceId] = link.targetId;
            else if( link.relation == 11 ) referenceByPass[link.sourceId] = link.targetId;
            else if( link.relation == 4 ) submittedCommandLists.emplace( link.sourceId );
            else if( link.relation == 13 ) referenceBySegment[link.sourceId] = link.targetId;
        } );
        for( const auto& [pass, segment] : segmentByPass ) wantedEntities.emplace( segment );
        for( const auto& [segment, token] : referenceBySegment ) wantedEntities.emplace( segment );
        // The first entity of each wanted id, as the Worker's id map keeps it.
        std::unordered_map<uint64_t, Entity> entityById;
        Packets( QueueType::JnGfxEntity, 0, SIZE_MAX, [&]( const QueueItem& e, const store::StoreExportPacket&, uint64_t ) {
            const auto& entity = e.jnGfxEntity;
            if( wantedEntities.contains( entity.entityId ) ) entityById.try_emplace( entity.entityId, Entity{ entity.kind, entity.gpuContext, entity.gpuQueryId, Time( entity.time ) } );
        } );
        struct ReferenceCandidate { uint64_t token; int64_t time; };
        std::unordered_map<uint64_t, std::vector<ReferenceCandidate>> referenceByQuery;
        for( const auto& [passId, referenceToken] : referenceByPass )
        {
            const auto segmentLink = segmentByPass.find( passId );
            if( segmentLink == segmentByPass.end() ) continue;
            const auto segment = entityById.find( segmentLink->second );
            if( segment == entityById.end() || segment->second.kind != 4 ) continue;
            gpuSegmentReferenceTokens.emplace( referenceToken );
            referenceByQuery[( uint64_t( segment->second.gpuContext ) << 32 ) | segment->second.gpuQueryId].push_back( { referenceToken, segment->second.time } );
        }
        for( const auto& [segmentId, referenceToken] : referenceBySegment )
        {
            gpuSegmentReferenceTokens.emplace( referenceToken );
            const auto segment = entityById.find( segmentId );
            if( segment == entityById.end() || segment->second.kind != 4 ) continue;
            referenceByQuery[( uint64_t( segment->second.gpuContext ) << 32 ) | segment->second.gpuQueryId].push_back( { referenceToken, segment->second.time } );
        }
        std::vector<GpuMemoryGpuZoneInput> gpuInputs;
        std::unordered_map<uint64_t, std::vector<size_t>> zonePositionsByQuery;
        {
            std::lock_guard lock( m_mutex );
            auto cursor = m_events->GpuZoneCursor();
            store::StoreGpuZoneRecord zone;
            for( size_t group = 0; group < m_gpuGroups.size(); ++group )
            {
                const auto context = m_contextIndex.find( m_gpuGroups[group].context );
                const uint64_t contextIndex = context == m_contextIndex.end() ? 0 : context->second;
                cursor->Seek( group, 0 );
                for( uint64_t ordinal = 0; cursor->Next( zone ); ++ordinal )
                {
                    if( zone.gpuEnd < 0 ) continue;
                    std::string name;
                    const auto sourceId = m_sourceIds.find( SourceKey{ zone.skind, zone.skey } );
                    if( sourceId != m_sourceIds.end() )
                    {
                        const auto location = m_locations.find( sourceId->second );
                        if( location != m_locations.end() ) name = location->second.name.empty() ? location->second.function : location->second.name;
                    }
                    zonePositionsByQuery[( contextIndex << 32 ) | zone.query].emplace_back( gpuInputs.size() );
                    gpuInputs.push_back( { m_gpuBase[group] + ordinal, std::move( name ), m_gpuGroups[group].thread, zone.cpuStart, zone.gpuStart, zone.gpuEnd, 0 } );
                }
            }
        }
        std::vector<bool> zoneAssigned( gpuInputs.size(), false );
        for( const auto& [key, candidates] : referenceByQuery )
        {
            const auto positions = zonePositionsByQuery.find( key );
            if( positions == zonePositionsByQuery.end() ) continue;
            auto sortedCandidates = candidates;
            std::sort( sortedCandidates.begin(), sortedCandidates.end(), []( const auto& lhs, const auto& rhs ) { return lhs.time < rhs.time; } );
            for( const auto& candidate : sortedCandidates )
            {
                size_t bestPosition = ~size_t( 0 );
                uint64_t bestDistance = ~uint64_t( 0 );
                for( const auto position : positions->second )
                {
                    if( zoneAssigned[position] ) continue;
                    const auto zoneStart = gpuInputs[position].cpuStartNs;
                    const uint64_t distance = zoneStart >= candidate.time ? uint64_t( zoneStart - candidate.time ) : uint64_t( candidate.time - zoneStart );
                    if( distance < bestDistance ) { bestDistance = distance; bestPosition = position; }
                }
                if( bestPosition != ~size_t( 0 ) )
                {
                    gpuInputs[bestPosition].referenceToken = candidate.token;
                    zoneAssigned[bestPosition] = true;
                }
            }
        }
        std::vector<GpuMemoryReferencePassInput> structuredReferencePasses;
        std::unordered_map<uint64_t, size_t> structuredReferenceById;
        constexpr size_t Page = 4096; // the snapshot source's page limit
        for( size_t offset = 0;; offset += Page )
        {
            const auto values = m_worker->ScanGpuReferencePasses( offset, Page );
            for( const auto& value : values )
            {
                if( value.passId == 0 || structuredReferenceById.contains( value.passId ) ) continue;
                GpuMemoryReferencePassInput input;
                input.passId = value.passId;
                input.frame = value.frameIndex;
                input.thread = value.thread;
                input.start = value.time;
                input.end = value.time;
                input.taxonomyId = value.taxonomyId;
                input.taxonomyLevel = value.taxonomyLevel;
                input.flags = value.flags;
                structuredReferenceById.emplace( value.passId, structuredReferencePasses.size() );
                structuredReferencePasses.emplace_back( std::move( input ) );
            }
            if( values.size() < Page ) break;
        }
        for( size_t offset = 0;; offset += Page )
        {
            const auto values = m_worker->ScanGpuReferenceUses( offset, Page );
            for( const auto& value : values )
            {
                const auto pass = structuredReferenceById.find( value.passId );
                if( pass == structuredReferenceById.end() || value.resourceId == 0 ) continue;
                structuredReferencePasses[pass->second].uses.push_back( { value.resourceId, value.usageMask, 'U', value.resourceSetId, value.encoding } );
            }
            if( values.size() < Page ) break;
        }
        for( size_t offset = 0;; offset += Page )
        {
            const auto values = m_worker->ScanGpuReferenceEnds( offset, Page );
            for( const auto& value : values )
            {
                const auto pass = structuredReferenceById.find( value.passId );
                if( pass == structuredReferenceById.end() ) continue;
                auto& input = structuredReferencePasses[pass->second];
                input.end = value.time;
                input.commandListId = value.commandListId;
                input.totalUseCount = value.totalReferenceCount;
                input.droppedUses = value.droppedReferenceCount;
                input.flags |= value.flags;
                input.ended = true;
            }
            if( values.size() < Page ) break;
        }
        if( !structuredReferenceById.empty() ) Packets( QueueType::JnRelation, 0, SIZE_MAX, [&]( const QueueItem& e, const store::StoreExportPacket&, uint64_t ) {
            const auto& value = e.jnRelation;
            if( value.relationNamespace != uint8_t( JnRelationNamespace::GpuReference ) || value.relation != 1 ) return;
            const auto child = structuredReferenceById.find( value.sourceId );
            if( child != structuredReferenceById.end() ) structuredReferencePasses[child->second].parentPassId = value.targetId;
        } );
        return BuildGpuMemoryAttribution( cpuInputs, gpuInputs, m_worker->GetGpuMemoryAllocationInputs(), submittedCommandLists,
            gpuSegmentReferenceTokens, structuredReferencePasses, m_worker->GetTraceInfo().lastTimeNs );
    }

    // Jobs are assembled from their raw schedule, config, dependency and stage
    // packets exactly as the Worker assembles a snapshot's Job records.
    uint64_t GetJobCount() const override
    {
        std::lock_guard lock( m_mutex );
        return m_events->JobCount();
    }
    std::vector<JobDto> ScanJobs( size_t offset, size_t limit ) const override
    {
        if( limit == 0 ) return {};
        std::vector<store::StoreJobKey> keys;
        {
            std::lock_guard lock( m_mutex );
            keys = m_events->JobKeys( offset, limit );
        }
        if( keys.empty() ) return {};
        std::unordered_set<uint64_t> selected;
        uint64_t first = UINT64_MAX, last = 0;
        for( const auto& key : keys )
        {
            selected.emplace( key.id );
            first = std::min( first, key.first );
            last = std::max( last, key.last );
        }
        return BuildJobs( std::nullopt, &selected, first, last, {} );
    }
    // A windowed view reads the Jobs and GPU zones near its windows through
    // the store's time indexes, in ScanJobs and ScanGpuZones order.
    std::vector<JobDto> ScanJobsNear( const std::vector<AnalysisTimeWindow>& windows, size_t offset, size_t limit ) const override
    {
        std::vector<store::StoreJobKey> keys;
        {
            std::lock_guard lock( m_mutex );
            const auto key = AnalysisWindowsText( windows );
            if( m_nearJobsKey != key )
            {
                std::vector<store::StoreJobKey> found;
                for( const auto& window : windows )
                {
                    auto part = m_events->JobKeysMeeting( window.startNs, window.endNs );
                    found.insert( found.end(), part.begin(), part.end() );
                }
                std::sort( found.begin(), found.end(), []( const auto& a, const auto& b ) { return a.id < b.id; } );
                found.erase( std::unique( found.begin(), found.end(), []( const auto& a, const auto& b ) { return a.id == b.id; } ), found.end() );
                m_nearJobs = std::move( found );
                m_nearJobsKey = key;
            }
            if( limit == 0 || offset >= m_nearJobs.size() ) return {};
            keys.assign( m_nearJobs.begin() + offset, m_nearJobs.begin() + std::min( m_nearJobs.size(), offset + limit ) );
        }
        std::unordered_set<uint64_t> selected;
        uint64_t first = UINT64_MAX, last = 0;
        for( const auto& key : keys )
        {
            selected.emplace( key.id );
            first = std::min( first, key.first );
            last = std::max( last, key.last );
        }
        return BuildJobs( std::nullopt, &selected, first, last, {} );
    }
    std::vector<GpuZoneDto> ScanGpuZonesNear( const std::vector<AnalysisTimeWindow>& windows, size_t offset, size_t limit ) const override
    {
        std::lock_guard lock( m_mutex );
        auto& s = m_nearGpu;
        const auto key = AnalysisWindowsText( windows );
        if( s.key != key )
        {
            s = NearGpu {};
            s.key = key;
            std::map<std::pair<uint64_t, uint64_t>, size_t> groups;
            for( size_t n = 0; n < m_gpuGroups.size(); ++n ) groups.emplace( std::make_pair( m_gpuGroups[n].context, m_gpuGroups[n].thread ), n );
            for( const auto& window : windows )
                for( const auto& [id, context, thread] : m_events->GpuZonesMeeting( window.startNs, window.endNs ) )
                {
                    const auto group = groups.find( { context, thread } );
                    if( group != groups.end() ) s.candidates.emplace_back( group->second, id );
                }
            std::sort( s.candidates.begin(), s.candidates.end() );
            s.candidates.erase( std::unique( s.candidates.begin(), s.candidates.end() ), s.candidates.end() );
            s.cursor = m_events->GpuZoneCursor();
        }
        std::vector<GpuZoneDto> result;
        // Zones between two candidates close together are read and skipped;
        // a farther candidate is reached by seeking.
        constexpr uint64_t SeekGap = 1ull << 20;
        if( offset != s.next ) s.group = SIZE_MAX;
        auto n = offset;
        for( ; n < s.candidates.size() && result.size() < limit; ++n )
        {
            const auto [group, id] = s.candidates[n];
            if( s.group != group || id <= s.lastId || id - s.lastId > SeekGap )
            {
                const auto ordinal = m_events->GpuZoneOrdinal( group, id );
                if( !ordinal ) throw std::runtime_error( "gpu_zone_window_candidate_missing" );
                s.cursor->Seek( group, *ordinal );
                s.group = group;
                s.ordinal = *ordinal;
                s.ancestors.clear();
                s.lastId = 0;
            }
            store::StoreGpuZoneRecord zone;
            for( ;; )
            {
                if( !s.cursor->Next( zone ) ) throw std::runtime_error( "gpu_zone_window_candidate_missing" );
                const auto ordinal = s.ordinal++;
                const auto parent = GpuParentOrdinal( s.ancestors, group, zone, ordinal );
                s.lastId = zone.id;
                if( zone.id == id )
                {
                    result.push_back( GpuDto( zone, group, ordinal, parent ) );
                    break;
                }
                if( zone.id > id ) throw std::runtime_error( "gpu_zone_window_candidate_missing" );
            }
        }
        s.next = n;
        return result;
    }

    std::vector<JobDto> GetJobs() const override
    {
        if( GetJobCount() > UnboundedReadLimit ) throw std::runtime_error( "domain_too_large_for_unbounded_read: use the paged method" );
        return BuildJobs( std::nullopt, nullptr, 0, UINT64_MAX, {} );
    }
    std::vector<JobDto> GetEvidenceJobs( uint64_t frameId ) const override { return GetEvidenceJobs( frameId, {} ); }
    std::vector<JobDto> GetEvidenceJobs( uint64_t frameId, const std::function<void()>& check ) const override
    {
        // The Jobs of the frame's sequence and, transitively, their
        // prerequisites.
        if( check ) check();
        std::unordered_set<uint64_t> selected;
        const auto sequence = uint32_t( frameId );
        Visit( QueueType::JnJobConfig, 0, UINT64_MAX, check, [&]( const QueueItem& e, const store::StoreExportPacket& ) {
            if( e.jnJobConfig.originFrameSequence == sequence ) selected.emplace( e.jnJobConfig.jobId );
        } );
        for( std::unordered_set<uint64_t> frontier = selected; !frontier.empty(); )
        {
            std::unordered_set<uint64_t> next;
            Visit( QueueType::JnJobDependency, 0, UINT64_MAX, check, [&]( const QueueItem& e, const store::StoreExportPacket& ) {
                const auto& p = e.jnJobDependency;
                if( p.prerequisiteJobId != 0 && frontier.contains( p.jobId ) && selected.emplace( p.prerequisiteJobId ).second ) next.emplace( p.prerequisiteJobId );
            } );
            frontier = std::move( next );
        }
        if( selected.empty() ) return {};
        const auto [first, last] = Span( selected );
        return BuildJobs( frameId, &selected, first, last, check );
    }
    std::vector<JobDto> GetDirectedJobs( uint64_t jobId, bool includeNeighbors, const std::function<void()>& check ) const override
    {
        std::unordered_set<uint64_t> selected{ jobId };
        if( check ) check();
        if( includeNeighbors ) Visit( QueueType::JnJobDependency, 0, UINT64_MAX, check, [&]( const QueueItem& e, const store::StoreExportPacket& ) {
            const auto& p = e.jnJobDependency;
            if( p.jobId == jobId && p.prerequisiteJobId != 0 ) selected.emplace( p.prerequisiteJobId );
            if( p.prerequisiteJobId == jobId ) selected.emplace( p.jobId );
        } );
        const auto [first, last] = Span( selected );
        return BuildJobs( std::nullopt, &selected, first, last, check );
    }

    // Graphics records and relations are the raw JN packets of their type in
    // id order, as a standard snapshot stores them.
    uint64_t GetGfxDispatchCount() const override { return PacketCount( QueueType::JnGfxDispatch ); }
    std::vector<GfxDispatchDto> ScanGfxDispatches( size_t offset, size_t limit ) const override
    {
        std::vector<GfxDispatchDto> result;
        Packets( QueueType::JnGfxDispatch, offset, limit, [&]( const QueueItem& e, const store::StoreExportPacket& packet, uint64_t ) {
            const auto& p = e.jnGfxDispatch;
            result.push_back( { Ref( "gfx-dispatch", p.dispatchId ), p.dispatchId, p.frameIndex, Time( p.time ),
                Ref( "thread", packet.thread ), p.expectedJobs, p.threadingMode, p.flags } );
        } );
        return result;
    }
    std::vector<GfxDispatchDto> GetGfxDispatches() const override { return ScanGfxDispatches( 0, size_t( Unbounded( QueueType::JnGfxDispatch ) ) ); }
    uint64_t GetGfxEntityCount() const override { return PacketCount( QueueType::JnGfxEntity ); }
    std::vector<GfxEntityDto> ScanGfxEntities( size_t offset, size_t limit ) const override
    {
        std::vector<GfxEntityDto> result;
        Packets( QueueType::JnGfxEntity, offset, limit, [&]( const QueueItem& e, const store::StoreExportPacket& packet, uint64_t ) {
            result.push_back( Entity( e, packet ) );
        } );
        return result;
    }
    std::vector<GfxEntityDto> GetGfxEntities() const override { return ScanGfxEntities( 0, size_t( Unbounded( QueueType::JnGfxEntity ) ) ); }
    uint64_t GetGfxLinkCount() const override { return PacketCount( QueueType::JnGfxLink ); }
    std::vector<GfxLinkDto> ScanGfxLinks( size_t offset, size_t limit ) const override
    {
        std::vector<GfxLinkDto> result;
        Packets( QueueType::JnGfxLink, offset, limit, [&]( const QueueItem& e, const store::StoreExportPacket& packet, uint64_t ordinal ) {
            result.push_back( Link( e, packet, ordinal ) );
        } );
        return result;
    }
    std::vector<GfxLinkDto> GetGfxLinks() const override { return ScanGfxLinks( 0, size_t( Unbounded( QueueType::JnGfxLink ) ) ); }
    uint64_t GetRelationCount() const override { return PacketCount( QueueType::JnRelation ); }
    std::vector<RelationDto> ScanRelations( size_t offset, size_t limit ) const override
    {
        std::vector<RelationDto> result;
        Packets( QueueType::JnRelation, offset, limit, [&]( const QueueItem& e, const store::StoreExportPacket& packet, uint64_t ordinal ) {
            const auto& p = e.jnRelation;
            result.push_back( { Ref( "relation", ordinal ), p.sourceId, p.targetId, Time( p.time ), Ref( "thread", packet.thread ),
                p.sourceKind, p.targetKind, p.relationNamespace, p.relation, p.flags } );
        } );
        return result;
    }
    std::vector<RelationDto> GetRelations() const override { return ScanRelations( 0, size_t( Unbounded( QueueType::JnRelation ) ) ); }

    // The entities and links reachable from the frame's dispatches, the
    // seeds and the entities linked to the frame, to a fixed point, as the
    // default does over whole-domain reads; here each pass pages the store.
    GfxEvidenceSlice GetEvidenceGfx( uint64_t frameId, const std::vector<uint64_t>& seedIds ) const override
    {
        GfxEvidenceSlice result;
        std::unordered_set<uint64_t> reachable( seedIds.begin(), seedIds.end() );
        Packets( QueueType::JnGfxDispatch, 0, SIZE_MAX, [&]( const QueueItem& e, const store::StoreExportPacket& packet, uint64_t ) {
            const auto& p = e.jnGfxDispatch;
            if( p.frameIndex != frameId ) return;
            result.dispatches.push_back( { Ref( "gfx-dispatch", p.dispatchId ), p.dispatchId, p.frameIndex, Time( p.time ),
                Ref( "thread", packet.thread ), p.expectedJobs, p.threadingMode, p.flags } );
            reachable.emplace( p.dispatchId );
        } );
        Packets( QueueType::JnGfxLink, 0, SIZE_MAX, [&]( const QueueItem& e, const store::StoreExportPacket&, uint64_t ) {
            if( e.jnGfxLink.relation == 8 && e.jnGfxLink.targetId == frameId ) reachable.emplace( e.jnGfxLink.sourceId );
        } );
        bool changed = true;
        while( changed )
        {
            changed = false;
            Packets( QueueType::JnGfxEntity, 0, SIZE_MAX, [&]( const QueueItem& e, const store::StoreExportPacket&, uint64_t ) {
                if( e.jnGfxEntity.parentId != 0 && reachable.contains( e.jnGfxEntity.parentId ) ) changed |= reachable.emplace( e.jnGfxEntity.entityId ).second;
            } );
            Packets( QueueType::JnGfxLink, 0, SIZE_MAX, [&]( const QueueItem& e, const store::StoreExportPacket&, uint64_t ) {
                if( reachable.contains( e.jnGfxLink.sourceId ) ) changed |= reachable.emplace( e.jnGfxLink.targetId ).second;
            } );
        }
        Packets( QueueType::JnGfxEntity, 0, SIZE_MAX, [&]( const QueueItem& e, const store::StoreExportPacket& packet, uint64_t ) {
            if( reachable.contains( e.jnGfxEntity.entityId ) ) result.entities.push_back( Entity( e, packet ) );
        } );
        Packets( QueueType::JnGfxLink, 0, SIZE_MAX, [&]( const QueueItem& e, const store::StoreExportPacket& packet, uint64_t ordinal ) {
            if( reachable.contains( e.jnGfxLink.sourceId ) ) result.links.push_back( Link( e, packet, ordinal ) );
        } );
        return result;
    }

    // Domains still served by the skeleton snapshot.
#define TRACY_STORE_FORWARD0( Return, Name ) Return Name() const override { return m_worker->Name(); }
#define TRACY_STORE_FORWARD1( Return, Name, T1, A1 ) Return Name( T1 A1 ) const override { return m_worker->Name( A1 ); }
#define TRACY_STORE_FORWARD2( Return, Name, T1, A1, T2, A2 ) Return Name( T1 A1, T2 A2 ) const override { return m_worker->Name( A1, A2 ); }
#define TRACY_STORE_FORWARD3( Return, Name, T1, A1, T2, A2, T3, A3 ) Return Name( T1 A1, T2 A2, T3 A3 ) const override { return m_worker->Name( A1, A2, A3 ); }
#define TRACY_STORE_FORWARD4( Return, Name, T1, A1, T2, A2, T3, A3, T4, A4 ) Return Name( T1 A1, T2 A2, T3 A3, T4 A4 ) const override { return m_worker->Name( A1, A2, A3, A4 ); }
    TRACY_STORE_FORWARD0( std::vector<FrameSetDto>, GetFrameSets )
    TRACY_STORE_FORWARD0( std::vector<MemoryPoolDto>, GetMemoryPools )
    TRACY_STORE_FORWARD0( std::vector<PlotDto>, GetPlotList )
    TRACY_STORE_FORWARD0( std::vector<LockDto>, GetLocks )
    TRACY_STORE_FORWARD1( std::vector<FrameDto>, ScanFrames, const ScanRange&, range )
    TRACY_STORE_FORWARD1( std::vector<MessageDto>, ScanMessages, const ScanRange&, range )
    TRACY_STORE_FORWARD1( std::vector<PlotPointDto>, ScanPlots, const ScanRange&, range )

    // Frame triage: top-level GPU zones read through their own store handle,
    // and plot points from the skeleton.
    void VisitTopLevelGpuZones( const std::function<void( size_t, int64_t, int64_t )>& visit ) const override
    {
        store::StoreOptions options;
        options.verifyContent = false;
        options.threadConfined = true;
        options.cacheBytes = 8ull << 20;
        options.resultBytes = 1ull << 20;
        const auto events = store::EventStore::Open( m_journal, options );
        events->VisitRootGpuZones( [&]( uint64_t stored, int64_t start, int64_t end ) {
            const auto context = m_contextIndex.find( stored );
            if( context != m_contextIndex.end() && end >= start ) visit( context->second, start, end );
        } );
    }
    void VisitPlotPoints( std::string_view plotRef, const std::function<void( int64_t, double )>& visit ) const override
    {
        m_worker->VisitPlotPoints( plotRef, visit );
    }
    TRACY_STORE_FORWARD1( std::vector<std::string>, ScanLocks, const ScanRange&, range )
    TRACY_STORE_FORWARD1( std::vector<std::string>, ScanContextSwitches, const ScanRange&, range )
    TRACY_STORE_FORWARD1( std::vector<std::string>, ScanSamples, const ScanRange&, range )
    TRACY_STORE_FORWARD0( std::vector<IoRequestDto>, GetIoRequests )
    TRACY_STORE_FORWARD0( uint64_t, GetIoRequestCount )
    TRACY_STORE_FORWARD2( std::vector<IoRequestDto>, ScanIoRequests, size_t, offset, size_t, limit )
    TRACY_STORE_FORWARD0( std::vector<CorrelatedFrameEventDto>, GetCorrelatedFrameEvents )
    TRACY_STORE_FORWARD0( std::optional<bool>, HasGpuMemoryProtocol2 )
    std::optional<ZoneValidationSummaryDto> ValidateSystemTrace( const std::function<size_t( size_t )>& allowance ) const override { return m_worker->ValidateSystemTrace( allowance ); }
    // Computed over every zone when the store was first opened.
    std::optional<ZoneValidationSummaryDto> ValidateZoneIndex( const std::function<size_t( size_t )>& allowance ) const override
    {
        auto result = m_zoneValidation;
        if( allowance( 1 ) == 0 ) result.complete = false;
        return result;
    }
    TRACY_STORE_FORWARD0( std::vector<RuntimeDomainStateDto>, GetRuntimeDomainStates )
    TRACY_STORE_FORWARD0( uint64_t, GetRuntimeDomainStateCount )
    TRACY_STORE_FORWARD2( std::vector<RuntimeDomainStateDto>, ScanRuntimeDomainStates, size_t, offset, size_t, limit )
    TRACY_STORE_FORWARD0( std::vector<ScriptFrameDto>, GetScriptFrames )
    TRACY_STORE_FORWARD0( uint64_t, GetScriptFrameCount )
    TRACY_STORE_FORWARD2( std::vector<ScriptFrameDto>, ScanScriptFrames, size_t, offset, size_t, limit )
    TRACY_STORE_FORWARD0( std::vector<ScriptStackEventDto>, GetScriptStackEvents )
    TRACY_STORE_FORWARD0( uint64_t, GetScriptStackEventCount )
    TRACY_STORE_FORWARD2( std::vector<ScriptStackEventDto>, ScanScriptStackEvents, size_t, offset, size_t, limit )
    TRACY_STORE_FORWARD2( std::vector<CpuUsagePointDto>, ScanCpuUsage, size_t, offset, size_t, limit )
    TRACY_STORE_FORWARD0( std::vector<CallsiteDto>, GetCallsites )
    TRACY_STORE_FORWARD0( uint64_t, GetCallsiteCount )
    TRACY_STORE_FORWARD2( std::vector<CallsiteDto>, ScanCallsites, size_t, offset, size_t, limit )
    TRACY_STORE_FORWARD0( std::shared_ptr<const tracy::JnTraceData>, GetGpuCatalogData )
    TRACY_STORE_FORWARD0( uint64_t, GetGpuCatalogResourceCountBounded )
    TRACY_STORE_FORWARD0( uint64_t, GetGpuCatalogAllocationCountBounded )
    TRACY_STORE_FORWARD0( uint64_t, GetGpuCatalogPassCountBounded )
    TRACY_STORE_FORWARD0( uint64_t, GetGpuCatalogRangeCountBounded )
    TRACY_STORE_FORWARD2( std::vector<GpuAnalysisResourceSummary>, ScanGpuCatalogResourcesBounded, size_t, offset, size_t, limit )
    TRACY_STORE_FORWARD2( std::vector<GpuAllocationAnalysisRecord>, ScanGpuCatalogAllocationsBounded, size_t, offset, size_t, limit )
    TRACY_STORE_FORWARD2( std::vector<GpuPassWorkingSet>, ScanGpuCatalogPassesBounded, size_t, offset, size_t, limit )
    TRACY_STORE_FORWARD2( std::vector<GpuAnalysisRangeStoreEntry>, ScanGpuCatalogRangesBounded, size_t, offset, size_t, limit )
    TRACY_STORE_FORWARD0( CrashDto, GetCrash )
    TRACY_STORE_FORWARD0( std::vector<CpuTopologyDto>, GetCpuTopology )
    TRACY_STORE_FORWARD0( std::vector<CpuUsagePointDto>, GetCpuUsage )
    TRACY_STORE_FORWARD1( std::vector<ContextSwitchDto>, ScanContextSwitchEvents, const ScanRange&, range )
    TRACY_STORE_FORWARD1( std::vector<CpuContextSwitchDto>, ScanCpuContextSwitchEvents, const ScanRange&, range )
    TRACY_STORE_FORWARD1( std::vector<SampleDto>, ScanSampleEvents, const ScanRange&, range )
    TRACY_STORE_FORWARD1( std::vector<GhostZoneDto>, ScanGhostZones, const ScanRange&, range )
    TRACY_STORE_FORWARD0( std::vector<HardwareSampleDto>, GetHardwareSamples )
    TRACY_STORE_FORWARD4( std::vector<HardwareSampleEventDto>, GetHardwareSampleEvents, uint64_t, address, std::string_view, kind, size_t, offset, size_t, limit )
    TRACY_STORE_FORWARD1( std::vector<LockEventDto>, ScanLockEvents, const ScanRange&, range )
    TRACY_STORE_FORWARD0( std::vector<SymbolDto>, GetSymbols )
    TRACY_STORE_FORWARD2( std::vector<SymbolAddressMappingDto>, GetSymbolAddressMappings, size_t, offset, size_t, limit )
    TRACY_STORE_FORWARD1( std::optional<SymbolAddressMappingDto>, ResolveSymbolAddress, uint64_t, address )
    TRACY_STORE_FORWARD0( std::vector<SourceLocationDto>, GetSourceLocations )
    TRACY_STORE_FORWARD2( std::vector<CallstackFrameDto>, ResolveCallstacks, const std::vector<uint32_t>&, callstacks, size_t, maxDepth )
    TRACY_STORE_FORWARD2( std::vector<CallstackFrameDto>, ResolveParentCallstacks, const std::vector<uint32_t>&, callstacks, size_t, maxDepth )
    TRACY_STORE_FORWARD2( std::vector<SourceTextDto>, ResolveSources, const std::vector<std::string>&, refs, size_t, maxBytes )
    TRACY_STORE_FORWARD2( std::vector<SymbolCodeDto>, ResolveSymbols, const std::vector<std::string>&, refs, size_t, maxBytes )
    TRACY_STORE_FORWARD2( std::vector<FrameImageDto>, ResolveFrameImages, const std::vector<std::string>&, refs, size_t, maxBytes )
    TRACY_STORE_FORWARD3( std::vector<FrameDto>, GetFramesForSet, size_t, frameSetIndex, size_t, offset, size_t, limit )
    TRACY_STORE_FORWARD1( std::vector<int64_t>, GetFrameDurations, size_t, frameSetIndex )
    TRACY_STORE_FORWARD0( std::vector<SourceResourceDto>, GetSourceResources )
    TRACY_STORE_FORWARD0( std::vector<SymbolResourceDto>, GetSymbolResources )
    TRACY_STORE_FORWARD0( std::vector<FrameImageMetadataDto>, GetFrameImageResources )
    TRACY_STORE_FORWARD4( MemoryFrameSnapshot, GetMemoryFrameSnapshot, size_t, frameSetIndex, size_t, frameIndex, const std::vector<std::string>&, poolRefs, bool, allGpuD3D12Pools )
    TRACY_STORE_FORWARD1( std::optional<std::string>, GetMemoryPoolRef, uint64_t, internalPoolKey )
    TRACY_STORE_FORWARD2( std::string, MakeEntityRef, std::string_view, kind, uint64_t, id )
    TRACY_STORE_FORWARD2( std::optional<uint64_t>, ParseEntityRef, std::string_view, ref, std::string_view, kind )
    TRACY_STORE_FORWARD2( SourceTextDto, ReadEmbeddedSource, size_t, sourceId, size_t, maxBytes )
    TRACY_STORE_FORWARD3( BinaryResourceChunkDto, ReadEmbeddedSourceBytes, size_t, sourceId, size_t, offset, size_t, maxBytes )
    TRACY_STORE_FORWARD2( SymbolCodeDto, ReadSymbolCode, uint64_t, symbolId, size_t, maxBytes )
    TRACY_STORE_FORWARD3( BinaryResourceChunkDto, ReadSymbolCodeBytes, uint64_t, symbolId, size_t, offset, size_t, maxBytes )
    TRACY_STORE_FORWARD3( std::vector<DisassemblyInstructionDto>, DisassembleSymbol, std::string_view, symbolRef, size_t, maxBytes, size_t, maxInstructions )
    TRACY_STORE_FORWARD2( FrameImageDto, ReadFrameImage, size_t, imageId, size_t, maxBytes )
    TRACY_STORE_FORWARD3( BinaryResourceChunkDto, ReadFrameImageBc1, size_t, imageId, size_t, offset, size_t, maxBytes )
#undef TRACY_STORE_FORWARD0
#undef TRACY_STORE_FORWARD1
#undef TRACY_STORE_FORWARD2
#undef TRACY_STORE_FORWARD3
#undef TRACY_STORE_FORWARD4

private:
    // Zones of one thread read in preorder, with the open ancestors' ordinals
    // by depth so that a zone's parent resolves without a lookup.
    class ZoneScanReader final : public CpuZoneScanReader
    {
    public:
        ZoneScanReader( const StoreTraceSource& source, std::unique_ptr<store::EventStore> events )
            : m_source( source )
            , m_events( std::move( events ) )
            , m_cursor( m_events->ZoneCursor( false ) )
        {
        }
        void Seek( size_t thread, uint64_t ordinal ) override
        {
            m_thread = thread;
            m_ancestors.clear();
            m_cursor->Seek( m_source.m_threads[thread].thread, ordinal - m_source.m_threadBase[thread] );
        }
        void Read( std::vector<CpuZoneScanRecord>& records, size_t limit ) override
        {
            records.clear();
            m_names.clear();
            const auto threadId = m_source.m_threads[m_thread].thread;
            const auto base = m_source.m_threadBase[m_thread];
            while( records.size() < limit && m_cursor->Next( m_zone ) )
            {
                const auto ordinal = m_cursor->Ordinal() - 1;
                const auto parent = ::tracy::query::ParentOrdinal( *m_events, threadId, m_ancestors, m_zone, ordinal );
                auto& record = records.emplace_back();
                record.ordinal = base + ordinal;
                if( parent ) record.parent = base + *parent;
                record.startNs = m_zone.start;
                record.hasEnd = record.complete = !m_zone.open;
                if( !m_zone.open ) record.endNs = m_zone.end;
                if( m_zone.skind != m_lastKind || m_zone.skey != m_lastKey || m_lastLocation == UINT32_MAX )
                {
                    const auto found = m_source.m_scanLocationOf.find( SourceKey{ m_zone.skind, m_zone.skey } );
                    m_lastLocation = found == m_source.m_scanLocationOf.end() ? m_source.m_scanMissingLocation : found->second;
                    m_lastKind = m_zone.skind;
                    m_lastKey = m_zone.skey;
                }
                record.location = m_lastLocation;
                if( m_zone.hasName )
                {
                    record.hasName = true;
                    record.name = m_names.emplace_back( m_zone.name );
                }
            }
        }

    private:
        const StoreTraceSource& m_source;
        std::unique_ptr<store::EventStore> m_events;
        std::unique_ptr<store::StoreZoneCursor> m_cursor;
        size_t m_thread = 0;
        std::vector<Ancestor> m_ancestors;
        store::StoreZoneRecord m_zone;
        // Names of the records of the last Read; a deque keeps them in place.
        std::deque<std::string> m_names;
        uint8_t m_lastKind = 0;
        uint64_t m_lastKey = 0;
        uint32_t m_lastLocation = UINT32_MAX;
    };

    void BuildZoneScanTables()
    {
        // One location per stored source number, as CpuDto resolves it, and
        // one for zones whose source the store does not number.
        std::unordered_map<int16_t, uint32_t> byNumber;
        const auto location = [&]( std::optional<int16_t> number ) {
            CpuZoneScanLocation value;
            const auto found = number ? m_locations.find( *number ) : m_locations.end();
            if( found != m_locations.end() )
            {
                const auto& source = found->second;
                value.ref = source.ref;
                value.name = source.name.empty() ? source.function : source.name;
                value.function = source.function;
                value.file = source.file;
                value.line = source.line;
            }
            else value.ref = Ref( "source", uint16_t( number.value_or( 0 ) ) );
            m_scanLocations.push_back( std::move( value ) );
            return uint32_t( m_scanLocations.size() - 1 );
        };
        for( const auto& [key, number] : m_sourceIds )
        {
            auto found = byNumber.find( number );
            if( found == byNumber.end() ) found = byNumber.emplace( number, location( number ) ).first;
            m_scanLocationOf.emplace( key, found->second );
        }
        m_scanMissingLocation = location( std::nullopt );
        m_scanThreads.resize( m_threads.size() );
        for( size_t thread = 0; thread < m_threads.size(); ++thread )
        {
            auto& value = m_scanThreads[thread];
            value.ref = Ref( "thread", m_threads[thread].thread );
            value.first = m_threadBase[thread];
            value.count = m_threads[thread].zones;
            if( thread < m_zoneRoots.size() )
                for( const auto root : m_zoneRoots[thread] ) value.roots.push_back( value.first + root );
            if( thread < m_zoneRootStarts.size() && thread < m_zoneRootEnds.size() &&
                value.roots.size() == m_zoneRootStarts[thread].size() && value.roots.size() == m_zoneRootEnds[thread].size() )
            {
                value.rootStarts = m_zoneRootStarts[thread];
                value.rootEnds = m_zoneRootEnds[thread];
            }
        }
    }

    struct ZoneScan
    {
        bool valid = false;
        int64_t startNs = 0, endNs = 0;
        size_t nextOffset = 0, thread = 0;
        uint64_t matched = 0;
        std::vector<Ancestor> ancestors;
    };

    std::string Ref( const char* kind, uint64_t id ) const
    {
        char digits[16];
        const auto end = std::to_chars( digits, digits + sizeof( digits ), id, 16 ).ptr;
        std::string out;
        out.reserve( m_refPrefix.size() + std::strlen( kind ) + 1 + size_t( end - digits ) );
        out.append( m_refPrefix ).append( kind ).append( 1, ':' ).append( digits, end );
        return out;
    }

    // Thread index and ordinal of a CPU zone ref.
    std::optional<std::pair<size_t, uint64_t>> Position( std::string_view ref ) const
    {
        const auto index = m_worker->ParseEntityRef( ref, "cpu-zone" );
        if( !index || *index >= m_threadBase.back() ) return std::nullopt;
        const auto thread = size_t( std::upper_bound( m_threadBase.begin(), m_threadBase.end(), *index ) - m_threadBase.begin() ) - 1;
        return std::make_pair( thread, *index - m_threadBase[thread] );
    }

    // A JN record time as a standard snapshot stores it.
    int64_t Time( int64_t ticks ) const
    {
        return int64_t( double( ticks - m_initBegin ) * m_timerMultiplier );
    }

    uint64_t PacketCount( QueueType type ) const
    {
        std::lock_guard lock( m_mutex );
        return m_events->PacketCount( uint8_t( type ) );
    }

    uint64_t Unbounded( QueueType type ) const
    {
        const auto count = PacketCount( type );
        if( count > UnboundedReadLimit ) throw std::runtime_error( "domain_too_large_for_unbounded_read: use the paged method" );
        return count;
    }

    // Visits packets [offset, offset+limit) of a type in pages.
    template<class F>
    void Packets( QueueType type, size_t offset, size_t limit, F&& visit ) const
    {
        constexpr size_t Page = 65536;
        uint64_t ordinal = offset;
        size_t remaining = limit;
        while( remaining )
        {
            std::vector<store::StoreExportPacket> packets;
            {
                std::lock_guard lock( m_mutex );
                packets = m_events->PacketsAt( uint8_t( type ), ordinal, std::min( remaining, Page ) );
            }
            if( packets.empty() ) break;
            for( const auto& packet : packets )
            {
                QueueItem event{};
                memcpy( reinterpret_cast<uint8_t*>( &event ) + 1, packet.payload.data(), packet.size );
                visit( event, packet, ordinal++ );
            }
            remaining -= packets.size();
        }
    }

    std::optional<std::string> ZoneRefAt( const std::string& threadRef, int64_t time ) const
    {
        const auto thread = m_worker->ParseEntityRef( threadRef, "thread" );
        if( !thread ) return std::nullopt;
        const auto found = m_threadIndex.find( *thread );
        if( found == m_threadIndex.end() ) return std::nullopt;
        std::optional<uint64_t> ordinal;
        {
            std::lock_guard lock( m_mutex );
            ordinal = m_events->ZoneAt( *thread, time );
        }
        if( !ordinal ) return std::nullopt;
        return Ref( "cpu-zone", m_threadBase[found->second] + *ordinal );
    }

    void AttachZones( MemoryEventDto& event ) const
    {
        event.allocationZoneRef = ZoneRefAt( event.allocationThreadRef, event.allocationNs );
        event.freeZoneRef.reset();
        if( event.freeNs && event.freeThreadRef ) event.freeZoneRef = ZoneRefAt( *event.freeThreadRef, *event.freeNs );
    }

    // The zone validation of Query's sidecar source over the store's zones,
    // cached beside the skeleton snapshot for the store's data hash.
    ZoneValidationSummaryDto LoadOrBuildZoneValidation()
    {
        using json = nlohmann::json;
        const auto path = WithSuffix( m_journal, ".query-skeleton.validation" );
        const auto& info = m_events->Info();
        try
        {
            const auto text = ReadText( path );
            if( !text.empty() )
            {
                const auto value = json::parse( text );
                if( value.at( "store" ).get<std::string>() == info.dataSha256 && value.at( "format" ).get<uint32_t>() == ValidationFormat )
                {
                    ZoneValidationSummaryDto result;
                    result.complete = value.at( "complete" ).get<bool>();
                    result.scanned = value.at( "scanned" ).get<uint64_t>();
                    for( const auto& finding : value.at( "findings" ) )
                        result.findings.push_back( { finding.at( "severity" ).get<std::string>(), finding.at( "code" ).get<std::string>(),
                            finding.at( "message" ).get<std::string>(), finding.at( "count" ).get<uint64_t>(), finding.at( "refs" ).get<std::vector<std::string>>() } );
                    result.referencedCallstacks = value.at( "callstacks" ).get<std::vector<uint32_t>>();
                    m_gpuMemoryMarkers = value.at( "gpu_memory_markers" ).get<std::vector<std::pair<size_t, uint64_t>>>();
                    m_zoneRoots = value.at( "zone_roots" ).get<std::vector<std::vector<uint64_t>>>();
                    m_zoneRootStarts = value.at( "zone_root_starts" ).get<std::vector<std::vector<int64_t>>>();
                    m_zoneRootEnds = value.at( "zone_root_ends" ).get<std::vector<std::vector<int64_t>>>();
                    return result;
                }
            }
        }
        catch( const std::exception& )
        {
        }
        auto result = BuildZoneValidation();
        json findings = json::array();
        for( const auto& finding : result.findings )
            findings.push_back( { { "severity", finding.severity }, { "code", finding.code }, { "message", finding.message }, { "count", finding.count }, { "refs", finding.refs } } );
        const json value = { { "format", ValidationFormat }, { "store", info.dataSha256 }, { "complete", result.complete }, { "scanned", result.scanned },
            { "findings", findings }, { "callstacks", result.referencedCallstacks }, { "gpu_memory_markers", m_gpuMemoryMarkers },
            { "zone_roots", m_zoneRoots }, { "zone_root_starts", m_zoneRootStarts }, { "zone_root_ends", m_zoneRootEnds } };
        auto temporary = WithSuffix( path, ".tmp" );
        {
            std::ofstream output( temporary, std::ios::binary | std::ios::trunc );
            output << value.dump();
            if( !output ) return result;
        }
        std::error_code error;
        std::filesystem::rename( temporary, path, error );
        return result;
    }

    ZoneValidationSummaryDto BuildZoneValidation()
    {
        ZoneValidationSummaryDto result;
        std::unordered_map<std::string, size_t> issueByCode;
        const auto note = [&]( const char* severity, const char* code, const char* message, const std::string& ref ) {
            auto found = issueByCode.find( code );
            if( found == issueByCode.end() )
            {
                found = issueByCode.emplace( code, result.findings.size() ).first;
                result.findings.push_back( { severity, code, message, 0, {} } );
            }
            auto& issue = result.findings[found->second];
            issue.count++;
            if( issue.refs.size() < 20 ) issue.refs.emplace_back( ref );
        };
        std::unordered_set<uint64_t> threads;
        for( const auto& thread : m_worker->GetThreads() ) threads.emplace( thread.nativeId );
        std::unordered_set<size_t> contexts;
        for( const auto& context : m_worker->GetGpuContexts() ) contexts.emplace( context.index );
        std::unordered_set<int16_t> mainLoop, unityLoop, gpuMemoryMarkers;
        m_gpuMemoryMarkers.clear();
        // 0 = missing, 1 = present without a name, 2 = present and named.
        std::array<uint8_t, 1u << 16> sourceState {};
        for( const auto& [id, source] : m_locations )
        {
            const auto& name = source.name.empty() ? source.function : source.name;
            if( name == "Main.PlayerLoop" ) mainLoop.emplace( int16_t( id ) );
            else if( name == "PlayerLoop" ) unityLoop.emplace( int16_t( id ) );
            else if( name == GpuMemoryRequestMarker || name == GpuMemoryPassMarker || name == GpuMemoryOriginMarker || name == GpuMemoryResidencyMarker ) gpuMemoryMarkers.emplace( int16_t( id ) );
            sourceState[uint16_t( int16_t( id ) )] = source.name.empty() && source.function.empty() ? 1 : 2;
        }
        const auto sourceOf = [&]( uint8_t kind, uint64_t key ) {
            const auto found = m_sourceIds.find( SourceKey{ kind, key } );
            return found == m_sourceIds.end() ? std::optional<int16_t>() : std::optional<int16_t>( found->second );
        };
        std::vector<uint32_t> callstacks;
        struct Root
        {
            uint64_t index = 0;
            int64_t start = 0, end = 0;
        };
        std::vector<Root> roots;
        // The checks read no zone text.
        auto cursor = m_events->ZoneCursor( false );
        store::StoreZoneRecord zone;
        m_zoneRoots.assign( m_threads.size(), {} );
        m_zoneRootStarts.assign( m_threads.size(), {} );
        m_zoneRootEnds.assign( m_threads.size(), {} );
        for( size_t thread = 0; thread < m_threads.size(); ++thread )
        {
            const auto base = m_threadBase[thread];
            const bool knownThread = threads.contains( m_threads[thread].thread );
            // Depth of each open ancestor that is a Main.PlayerLoop zone.
            std::vector<bool> loopAncestors;
            roots.clear();
            cursor->Seek( m_threads[thread].thread, 0 );
            auto& zoneRoots = m_zoneRoots[thread];
            auto& zoneRootStarts = m_zoneRootStarts[thread];
            auto& zoneRootEnds = m_zoneRootEnds[thread];
            for( uint64_t ordinal = 0; cursor->Next( zone ); ++ordinal )
            {
                const auto index = base + ordinal;
                const auto ref = [&] { return Ref( "cpu-zone", index ); };
                if( zone.parent == 0 && ( zoneRoots.empty() || ordinal >= zoneRoots.back() + RootSpacing ) )
                {
                    zoneRoots.push_back( ordinal );
                    zoneRootStarts.push_back( zone.start );
                    zoneRootEnds.push_back( zone.start );
                }
                if( !zoneRoots.empty() )
                {
                    zoneRootStarts.back() = std::min( zoneRootStarts.back(), zone.start );
                    zoneRootEnds.back() = std::max( zoneRootEnds.back(), zone.open ? std::numeric_limits<int64_t>::max() : std::max( zone.end, zone.start ) );
                }
                if( zone.open ) note( "warning", "INCOMPLETE_CPU_ZONES", "CPU zones have no persisted end event", ref() );
                else if( zone.end < zone.start ) note( "error", "INVALID_CPU_ZONE_TIMING", "CPU zones end before they begin", ref() );
                if( !knownThread ) note( "warning", "UNRESOLVED_THREAD_REFERENCE", "persisted entity reference cannot be resolved in this trace", ref() );
                const auto source = sourceOf( zone.skind, zone.skey );
                const auto state = source ? sourceState[uint16_t( *source )] : uint8_t( 0 );
                if( state == 0 ) note( "warning", "UNRESOLVED_SOURCE_LOCATION_REFERENCE", "persisted entity reference cannot be resolved in this trace", ref() );
                const bool nameResolved = zone.hasName || state == 2;
                uint64_t rawStack = zone.stack;
                if( !rawStack && zone.callsite )
                {
                    const auto found = m_callsiteStacks.find( zone.callsite );
                    if( found != m_callsiteStacks.end() ) rawStack = found->second;
                }
                if( const auto callstack = Callstack( rawStack ) ) callstacks.emplace_back( callstack );
                if( !nameResolved ) note( "warning", "UNRESOLVED_CPU_ZONE_NAME", "CPU zones reference dynamic names that are absent from the persisted string table; source-location names were used as fallback", ref() );
                if( source && gpuMemoryMarkers.contains( *source ) ) m_gpuMemoryMarkers.emplace_back( thread, ordinal );
                loopAncestors.resize( zone.depth );
                const bool mainRoot = source && mainLoop.contains( *source );
                if( source && unityLoop.contains( *source ) )
                    note( "error", "DUPLICATE_AUTHORITATIVE_PLAYERLOOP_ROOT", "Unity's cross-frame PlayerLoop marker was mirrored beside the authoritative Main.PlayerLoop root", ref() );
                if( mainRoot )
                {
                    if( std::find( loopAncestors.begin(), loopAncestors.end(), true ) != loopAncestors.end() )
                        note( "error", "CPU_FRAME_ROOT_NESTED_SAME_SOURCE", "Main.PlayerLoop is nested below another Main.PlayerLoop root", ref() );
                    if( !zone.open ) roots.push_back( { index, zone.start, zone.end } );
                }
                loopAncestors.push_back( mainRoot );
            }
            result.scanned += m_threads[thread].zones;
            std::sort( roots.begin(), roots.end(), []( const Root& a, const Root& b ) { return a.start != b.start ? a.start < b.start : a.index < b.index; } );
            if( roots.empty() ) continue;
            auto longest = roots.front();
            for( size_t n = 1; n < roots.size(); ++n )
            {
                if( roots[n].start < longest.end ) note( "error", "CPU_FRAME_ROOT_CROSSING", "Main.PlayerLoop overlaps the next authoritative frame root", Ref( "cpu-zone", roots[n].index ) );
                if( roots[n].end > longest.end ) longest = roots[n];
            }
        }
        auto gpu = m_events->GpuZoneCursor();
        store::StoreGpuZoneRecord gpuZone;
        for( size_t group = 0; group < m_gpuGroups.size(); ++group )
        {
            const auto base = m_gpuBase[group];
            const auto context = m_contextIndex.find( m_gpuGroups[group].context );
            const bool knownContext = context != m_contextIndex.end() && contexts.contains( context->second );
            const bool knownThread = threads.contains( m_gpuGroups[group].thread );
            gpu->Seek( group, 0 );
            for( uint64_t ordinal = 0; gpu->Next( gpuZone ); ++ordinal )
            {
                const auto ref = [&] { return Ref( "gpu-zone", base + ordinal ); };
                if( !( gpuZone.gpuEnd >= 0 && gpuZone.cpuEnd >= 0 ) ) note( "warning", "INCOMPLETE_GPU_ZONES", "GPU zones have incomplete CPU or GPU timing", ref() );
                else if( gpuZone.gpuEnd < gpuZone.gpuStart || gpuZone.cpuEnd < gpuZone.cpuStart ) note( "error", "INVALID_GPU_ZONE_TIMING", "GPU zones contain reversed CPU or GPU timing", ref() );
                if( !knownThread ) note( "warning", "UNRESOLVED_THREAD_REFERENCE", "persisted entity reference cannot be resolved in this trace", ref() );
                if( !knownContext ) note( "warning", "UNRESOLVED_GPU_CONTEXT_REFERENCE", "persisted entity reference cannot be resolved in this trace", ref() );
                const auto source = sourceOf( gpuZone.skind, gpuZone.skey );
                if( !source || sourceState[uint16_t( *source )] == 0 ) note( "warning", "UNRESOLVED_SOURCE_LOCATION_REFERENCE", "persisted entity reference cannot be resolved in this trace", ref() );
                uint64_t rawStack = gpuZone.stack;
                if( !rawStack && gpuZone.callsite )
                {
                    const auto found = m_callsiteStacks.find( gpuZone.callsite );
                    if( found != m_callsiteStacks.end() ) rawStack = found->second;
                }
                if( const auto callstack = Callstack( rawStack ) ) callstacks.emplace_back( callstack );
            }
            result.scanned += m_gpuGroups[group].zones;
        }
        std::sort( callstacks.begin(), callstacks.end() );
        callstacks.erase( std::unique( callstacks.begin(), callstacks.end() ), callstacks.end() );
        result.referencedCallstacks = std::move( callstacks );
        return result;
    }

    // Visits the packets of a type with ids in [from,to].
    template<class F>
    void Visit( QueueType type, uint64_t from, uint64_t to, const std::function<void()>& check, F&& visit ) const
    {
        std::lock_guard lock( m_mutex );
        size_t records = 0;
        m_events->VisitPackets( uint8_t( type ), from, to, [&]( const store::StoreExportPacket& packet ) {
            if( check && ( ++records & 1023 ) == 0 ) check();
            QueueItem event{};
            memcpy( reinterpret_cast<uint8_t*>( &event ) + 1, packet.payload.data(), packet.size );
            visit( event, packet );
            return true;
        } );
    }

    // Packet id range holding every record of the Jobs.
    std::pair<uint64_t, uint64_t> Span( const std::unordered_set<uint64_t>& jobs ) const
    {
        uint64_t first = UINT64_MAX, last = 0;
        std::lock_guard lock( m_mutex );
        for( const auto job : jobs )
            if( const auto key = m_events->JobKey( job ) )
            {
                first = std::min( first, key->first );
                last = std::max( last, key->last );
            }
        return { first, last };
    }

    // WorkerTraceSource::BuildJobs over the store's packets: selected Jobs
    // (all when `explicitJobIds` is null) from the records in [first,last].
    std::vector<JobDto> BuildJobs( std::optional<uint64_t> evidenceFrameId, const std::unordered_set<uint64_t>* explicitJobIds,
        uint64_t first, uint64_t last, const std::function<void()>& check ) const
    {
        if( check ) check();
        const auto includeJob = [&]( uint64_t jobId ) { return !explicitJobIds || explicitJobIds->contains( jobId ); };
        std::map<uint64_t, JobDto> jobs;
        const auto ensureJob = [&]( uint64_t jobId ) -> JobDto& {
            auto [it, inserted] = jobs.try_emplace( jobId );
            if( inserted )
            {
                it->second.ref = Ref( "job", jobId );
                it->second.jobId = jobId;
                it->second.orphan = true;
            }
            return it->second;
        };
        Visit( QueueType::JnJobSchedule, first, last, check, [&]( const QueueItem& e, const store::StoreExportPacket& packet ) {
            const auto& p = e.jnJobSchedule;
            if( !includeJob( p.jobId ) ) return;
            auto& job = ensureJob( p.jobId );
            job.packedHandle = p.packedHandle;
            job.kind = p.kind;
            job.flags = p.flags;
            if( ( p.flags & uint8_t( 1 << 6 ) ) != 0 ) job.jobSchemaVersion = 3;
            job.captureBoundary = ( p.flags & uint8_t( 1 << 7 ) ) != 0;
            job.scheduleNs = Time( p.time );
            job.scheduleThreadRef = Ref( "thread", packet.thread );
            job.expectedDependencyCount = p.dependencyCount;
            job.orphan = job.captureBoundary;
        } );
        Visit( QueueType::JnJobConfig, first, last, check, [&]( const QueueItem& e, const store::StoreExportPacket& ) {
            const auto& config = e.jnJobConfig;
            if( !includeJob( config.jobId ) ) return;
            auto& job = ensureJob( config.jobId );
            if( config.typeId != 0 ) job.typeId = config.typeId;
            if( config.count != 0 || job.count == 0 ) job.count = config.count;
            if( config.grainSize != 0 || job.grainSize == 0 ) job.grainSize = config.grainSize;
            if( config.unityFlowId != 0 || job.unityFlowId == 0 ) job.unityFlowId = config.unityFlowId;
            if( config.originFrameSequence != 0 )
            {
                job.originFrameSequence = config.originFrameSequence;
                const auto frame = m_frameIdsBySequence.find( config.originFrameSequence );
                if( frame != m_frameIdsBySequence.end() ) job.originFrameId = frame->second;
            }
            job.kind = config.kind;
            job.flags |= config.flags;
            if( ( config.flags & uint8_t( 1 << 6 ) ) != 0 ) job.jobSchemaVersion = 3;
            job.captureBoundary = job.captureBoundary || ( config.flags & uint8_t( 1 << 7 ) ) != 0;
            if( job.captureBoundary ) job.orphan = true;
        } );
        Visit( QueueType::JnJobDependency, first, last, check, [&]( const QueueItem& e, const store::StoreExportPacket& ) {
            const auto& dependency = e.jnJobDependency;
            if( !includeJob( dependency.jobId ) ) return;
            ensureJob( dependency.jobId ).dependencies.push_back( { dependency.prerequisiteJobId, dependency.prerequisiteHandle, dependency.flags } );
        } );

        using SpanStarts = std::unordered_map<uint32_t, int64_t>;
        std::unordered_map<uint64_t, SpanStarts> sliceStarts, activeHelpStarts, spinStarts, sleepStarts, waitStarts;
        std::unordered_map<uint64_t, int64_t> completedByJob;
        if( explicitJobIds && !evidenceFrameId )
        {
            // Completion times of prerequisites outside the selection, for
            // the dependency latency.
            std::unordered_set<uint64_t> prerequisiteIds;
            for( const auto& [id, job] : jobs )
                for( const auto& dependency : job.dependencies )
                    if( !explicitJobIds->contains( dependency.prerequisiteJobId ) ) prerequisiteIds.emplace( dependency.prerequisiteJobId );
            for( const auto prerequisite : prerequisiteIds )
            {
                std::optional<store::StoreJobKey> key;
                {
                    std::lock_guard lock( m_mutex );
                    key = m_events->JobKey( prerequisite );
                }
                if( !key ) continue;
                Visit( QueueType::JnJobStage, key->first, key->last, check, [&]( const QueueItem& e, const store::StoreExportPacket& ) {
                    if( e.jnJobStage.jobId == prerequisite && JnJobStage( e.jnJobStage.stage ) == JnJobStage::Completed ) completedByJob[prerequisite] = Time( e.jnJobStage.time );
                } );
            }
        }
        const auto closeSpan = []( auto& starts, uint64_t jobId, uint32_t spanId, int64_t time, int64_t& total ) {
            const auto jobsIt = starts.find( jobId );
            if( jobsIt == starts.end() ) return;
            const auto spanIt = jobsIt->second.find( spanId );
            if( spanIt == jobsIt->second.end() ) return;
            if( time >= spanIt->second ) total += time - spanIt->second;
            jobsIt->second.erase( spanIt );
        };
        Visit( QueueType::JnJobStage, first, last, check, [&]( const QueueItem& e, const store::StoreExportPacket& packet ) {
            const auto& p = e.jnJobStage;
            if( !includeJob( p.jobId ) ) return;
            // The record a standard snapshot stores for the packet.
            const bool callstackStage = p.stage == uint8_t( JnJobStage::ScheduleCallstack ) || p.stage == uint8_t( JnJobStage::WaitCallstack );
            const auto time = Time( p.time );
            const uint64_t thread = callstackStage ? p.arg0 : packet.thread;
            const uint32_t spanId = callstackStage ? Callstack( packet.stack ) : p.spanId;
            auto& job = ensureJob( p.jobId );
            job.stages.push_back( { time, Ref( "thread", thread ), spanId, p.arg0, p.arg1, p.stage, p.flags } );
            switch( JnJobStage( p.stage ) )
            {
            case JnJobStage::WorkerSliceBegin:
                if( !job.firstRunNs || time < *job.firstRunNs ) job.firstRunNs = time;
                if( ( p.flags & uint8_t( 1 << 7 ) ) != 0 ) job.rangeStealSliceCount++;
                sliceStarts[p.jobId][spanId] = time;
                break;
            case JnJobStage::WorkerSliceEnd: closeSpan( sliceStarts, p.jobId, spanId, time, job.executionNs ); break;
            case JnJobStage::Completed: job.completedNs = time; break;
            case JnJobStage::WaitBegin: waitStarts[p.jobId][spanId] = time; break;
            case JnJobStage::WaitEnd:
                closeSpan( waitStarts, p.jobId, spanId, time, job.waitNs );
                job.waitEndCount++;
                break;
            case JnJobStage::WaitActiveHelpBegin: activeHelpStarts[p.jobId][spanId] = time; break;
            case JnJobStage::WaitActiveHelpEnd: closeSpan( activeHelpStarts, p.jobId, spanId, time, job.waitActiveHelpNs ); break;
            case JnJobStage::WaitSpinYieldBegin: spinStarts[p.jobId][spanId] = time; break;
            case JnJobStage::WaitSpinYieldEnd: closeSpan( spinStarts, p.jobId, spanId, time, job.waitSpinYieldNs ); break;
            case JnJobStage::WaitSleepBegin: sleepStarts[p.jobId][spanId] = time; break;
            case JnJobStage::WaitSleepEnd: closeSpan( sleepStarts, p.jobId, spanId, time, job.waitSleepNs ); break;
            case JnJobStage::ScheduleCallstack:
                job.scheduleCallstack = spanId;
                job.scheduleStackProvenance = "PerEventExact";
                job.stages.back().callstack = spanId;
                job.stages.back().stackProvenance = "PerEventExact";
                break;
            case JnJobStage::ScheduleCallsite:
            {
                job.jobSchemaVersion = 3;
                job.scheduleCallsiteId = spanId;
                auto& stageDto = job.stages.back();
                stageDto.callsiteId = spanId;
                const auto callsite = m_callsitesById.find( spanId );
                if( callsite != m_callsitesById.end() )
                {
                    job.scheduleCallstack = callsite->second.callstack;
                    job.scheduleStackProvenance = callsite->second.provenance;
                    job.scheduleStackUnavailableReason = callsite->second.unavailableReason;
                    stageDto.callstack = callsite->second.callstack;
                    stageDto.stackProvenance = job.scheduleStackProvenance;
                    stageDto.stackUnavailableReason = job.scheduleStackUnavailableReason;
                }
                break;
            }
            case JnJobStage::Ready:
                job.jobSchemaVersion = std::max<uint16_t>( job.jobSchemaVersion, 2 );
                if( !job.readyNs || time < *job.readyNs )
                {
                    job.readyNs = time;
                    job.readyLane = p.arg0;
                    job.readyFlags = p.flags;
                }
                break;
            case JnJobStage::QueueEnter:
                job.jobSchemaVersion = std::max<uint16_t>( job.jobSchemaVersion, 2 );
                if( !job.queueEnterNs || time < *job.queueEnterNs )
                {
                    job.queueEnterNs = time;
                    job.queueLane = p.arg0;
                }
                if( ( p.flags & uint8_t( 1 << 6 ) ) != 0 ) job.queueRetryCount++;
                break;
            case JnJobStage::Dispatch:
                job.jobSchemaVersion = std::max<uint16_t>( job.jobSchemaVersion, 2 );
                job.dispatchCount++;
                if( ( p.flags & uint8_t( 1 << 1 ) ) != 0 ) job.activeHelpDispatchCount++;
                if( std::find( job.executionLanes.begin(), job.executionLanes.end(), p.arg0 ) == job.executionLanes.end() ) job.executionLanes.push_back( p.arg0 );
                break;
            case JnJobStage::Steal:
                job.jobSchemaVersion = std::max<uint16_t>( job.jobSchemaVersion, 2 );
                job.schedulerStealCount++;
                break;
            case JnJobStage::WaitCallstack:
                job.jobSchemaVersion = std::max<uint16_t>( job.jobSchemaVersion, 2 );
                job.stages.back().callstack = spanId;
                job.stages.back().stackProvenance = "PerEventExact";
                job.waitCallstacks.push_back( { time, Ref( "thread", thread ), p.arg1, spanId, 0, "PerEventExact", std::nullopt } );
                break;
            case JnJobStage::WaitCallsite:
            {
                job.jobSchemaVersion = 3;
                auto& stageDto = job.stages.back();
                stageDto.callsiteId = spanId;
                JobWaitCallstackDto wait { time, Ref( "thread", thread ), p.arg1, 0, spanId, {}, std::nullopt };
                const auto callsite = m_callsitesById.find( spanId );
                if( callsite != m_callsitesById.end() )
                {
                    wait.callstack = callsite->second.callstack;
                    wait.stackProvenance = callsite->second.provenance;
                    wait.stackUnavailableReason = callsite->second.unavailableReason;
                    stageDto.callstack = wait.callstack;
                    stageDto.stackProvenance = wait.stackProvenance;
                    stageDto.stackUnavailableReason = wait.stackUnavailableReason;
                }
                job.waitCallstacks.emplace_back( std::move( wait ) );
                break;
            }
            case JnJobStage::Continuation:
                job.jobSchemaVersion = 3;
                job.continuationCount++;
                break;
            case JnJobStage::Cancelled: job.cancelled = true; break;
            case JnJobStage::Incomplete: job.incomplete = true; break;
            default: break;
            }
        } );

        for( auto& [jobId, job] : jobs )
        {
            if( !job.readyNs || job.dependencies.empty() ) continue;
            std::optional<int64_t> maximumPrerequisiteCompleted;
            bool complete = true;
            for( const auto& dependency : job.dependencies )
            {
                const auto prerequisite = jobs.find( dependency.prerequisiteJobId );
                const auto externalCompletion = completedByJob.find( dependency.prerequisiteJobId );
                const auto completed = prerequisite != jobs.end() && prerequisite->second.completedNs ?
                    prerequisite->second.completedNs :
                    externalCompletion != completedByJob.end() ? std::optional<int64_t>( externalCompletion->second ) : std::nullopt;
                if( !completed )
                {
                    complete = false;
                    break;
                }
                if( !maximumPrerequisiteCompleted || *completed > *maximumPrerequisiteCompleted ) maximumPrerequisiteCompleted = *completed;
            }
            if( complete && maximumPrerequisiteCompleted && *job.readyNs >= *maximumPrerequisiteCompleted )
                job.dependencyReadyLatencyNs = *job.readyNs - *maximumPrerequisiteCompleted;
        }

        std::vector<JobDto> result;
        result.reserve( jobs.size() );
        for( auto& [jobId, job] : jobs )
        {
            const auto type = m_jobTypeNames.find( job.typeId );
            job.name = type == m_jobTypeNames.end() ? "<unknown job type>" : type->second;
            job.truncated = !job.completedNs && !job.cancelled && !job.incomplete;
            std::sort( job.executionLanes.begin(), job.executionLanes.end() );
            std::sort( job.stages.begin(), job.stages.end(), []( const auto& lhs, const auto& rhs ) { return lhs.timeNs < rhs.timeNs; } );
            result.emplace_back( std::move( job ) );
        }
        if( check ) check();
        return result;
    }

    GfxEntityDto Entity( const QueueItem& e, const store::StoreExportPacket& packet ) const
    {
        const auto& p = e.jnGfxEntity;
        return { Ref( "gfx-entity", p.entityId ), p.entityId, p.parentId, Time( p.time ), Ref( "thread", packet.thread ),
            p.gpuQueryId, p.gpuContext, p.kind, p.flags };
    }

    GfxLinkDto Link( const QueueItem& e, const store::StoreExportPacket& packet, uint64_t ordinal ) const
    {
        const auto& p = e.jnGfxLink;
        return { Ref( "gfx-link", ordinal ), p.sourceId, p.targetId, Time( p.time ), Ref( "thread", packet.thread ), p.relation, p.flags };
    }

    // Timeline index and ordinal of a GPU zone ref.
    std::optional<std::pair<size_t, uint64_t>> GpuPosition( std::string_view ref ) const
    {
        const auto index = m_worker->ParseEntityRef( ref, "gpu-zone" );
        if( !index || *index >= m_gpuBase.back() ) return std::nullopt;
        const auto group = size_t( std::upper_bound( m_gpuBase.begin(), m_gpuBase.end(), *index ) - m_gpuBase.begin() ) - 1;
        return std::make_pair( group, *index - m_gpuBase[group] );
    }

    void StartGroup( ZoneScan& scan, size_t group, uint64_t globalIndex ) const
    {
        scan.thread = group;
        scan.ancestors.clear();
        if( group < m_gpuGroups.size() ) m_gpuScanCursor->Seek( group, globalIndex - m_gpuBase[group] );
    }

    std::optional<uint64_t> GpuParentOrdinal( std::vector<Ancestor>& ancestors, size_t group, const store::StoreGpuZoneRecord& zone, uint64_t ordinal ) const
    {
        std::optional<uint64_t> parent;
        if( zone.parent )
        {
            if( zone.depth > 0 && ancestors.size() >= zone.depth && ancestors[zone.depth - 1].id == zone.parent ) parent = ancestors[zone.depth - 1].ordinal;
            else
            {
                parent = m_events->GpuZoneOrdinal( group, zone.parent );
                ancestors.assign( zone.depth, Ancestor{} );
                if( zone.depth > 0 && parent ) ancestors[zone.depth - 1] = { zone.parent, *parent };
            }
        }
        if( ancestors.size() < zone.depth ) ancestors.resize( zone.depth );
        ancestors.resize( zone.depth + 1 );
        ancestors[zone.depth] = { zone.id, ordinal };
        return parent;
    }

    GpuZoneDto GpuDto( const store::StoreGpuZoneRecord& zone, size_t group, uint64_t ordinal, std::optional<uint64_t> parentOrdinal ) const
    {
        const auto base = m_gpuBase[group];
        const auto& timeline = m_gpuGroups[group];
        GpuZoneDto dto;
        dto.ref = Ref( "gpu-zone", base + ordinal );
        const auto context = m_contextIndex.find( timeline.context );
        dto.contextRef = Ref( "gpu-context", context == m_contextIndex.end() ? 0 : context->second );
        dto.threadRef = Ref( "thread", timeline.thread );
        const auto sourceId = m_sourceIds.find( SourceKey{ zone.skind, zone.skey } );
        const SourceLocationDto* source = nullptr;
        int16_t srcloc = 0;
        if( sourceId != m_sourceIds.end() )
        {
            srcloc = sourceId->second;
            const auto found = m_locations.find( srcloc );
            if( found != m_locations.end() ) source = &found->second;
        }
        dto.sourceLocationRef = source ? source->ref : Ref( "source", uint16_t( srcloc ) );
        if( source )
        {
            dto.name = source->name.empty() ? source->function : source->name;
            dto.function = source->function;
            dto.file = source->file;
            dto.line = source->line;
        }
        if( parentOrdinal ) dto.parentRef = Ref( "gpu-zone", base + *parentOrdinal );
        dto.gpuStartNs = zone.gpuStart;
        if( zone.gpuEnd >= 0 )
        {
            dto.gpuEndNs = zone.gpuEnd;
            dto.selfTimeNs = std::max<int64_t>( 0, zone.gpuEnd - zone.gpuStart - zone.childTime );
        }
        dto.cpuStartNs = zone.cpuStart;
        if( zone.cpuEnd >= 0 ) dto.cpuEndNs = zone.cpuEnd;
        dto.childCount = zone.children;
        uint64_t rawStack = zone.stack;
        if( !rawStack && zone.callsite )
        {
            const auto found = m_callsiteStacks.find( zone.callsite );
            if( found != m_callsiteStacks.end() ) rawStack = found->second;
        }
        dto.callstack = Callstack( rawStack );
        if( dto.callstack != 0 ) dto.callstackRef = Ref( "callstack", dto.callstack );
        const auto callsite = m_callsitesByZone.find( ( uint64_t( uint16_t( srcloc ) ) << 32 ) | dto.callstack );
        if( callsite != m_callsitesByZone.end() )
        {
            dto.callsiteId = callsite->second.callsiteId;
            dto.stackProvenance = callsite->second.provenance;
            if( dto.callstack != 0 ) dto.stackRef = Ref( "callstack", dto.callstack );
            dto.stackUnavailableReason = callsite->second.unavailableReason;
        }
        dto.complete = zone.gpuEnd >= 0 && zone.cpuEnd >= 0;
        dto.queryId = zone.query;
        dto.queryIdAvailability.available = m_gpuQueryIds;
        if( !m_gpuQueryIds ) dto.queryIdAvailability.reason = "gpu query IDs were not persisted before Tracy 0.12.4";
        return dto;
    }

    void StartThread( ZoneScan& scan, size_t thread, uint64_t globalIndex ) const
    {
        scan.thread = thread;
        scan.ancestors.clear();
        if( thread < m_threads.size() ) m_scanCursor->Seek( m_threads[thread].thread, globalIndex - m_threadBase[thread] );
    }

    std::optional<uint64_t> ParentOrdinal( std::vector<Ancestor>& ancestors, size_t thread, const store::StoreZoneRecord& zone, uint64_t ordinal ) const
    {
        return ::tracy::query::ParentOrdinal( *m_events, m_threads[thread].thread, ancestors, zone, ordinal );
    }

    uint32_t Callstack( uint64_t stack ) const
    {
        if( !stack ) return 0;
        const auto found = std::lower_bound( m_stackIds.begin(), m_stackIds.end(), stack );
        if( found == m_stackIds.end() || *found != stack ) return 0;
        return uint32_t( found - m_stackIds.begin() + 1 );
    }

    CpuZoneDto CpuDto( const store::StoreZoneRecord& zone, size_t thread, uint64_t ordinal, std::optional<uint64_t> parentOrdinal ) const
    {
        const auto base = m_threadBase[thread];
        CpuZoneDto dto;
        dto.ref = Ref( "cpu-zone", base + ordinal );
        dto.threadRef = Ref( "thread", m_threads[thread].thread );
        const auto sourceId = m_sourceIds.find( SourceKey{ zone.skind, zone.skey } );
        const SourceLocationDto* source = nullptr;
        int16_t srcloc = 0;
        if( sourceId != m_sourceIds.end() )
        {
            srcloc = sourceId->second;
            const auto found = m_locations.find( srcloc );
            if( found != m_locations.end() ) source = &found->second;
        }
        dto.sourceLocationRef = source ? source->ref : Ref( "source", uint16_t( srcloc ) );
        uint64_t rawStack = zone.stack;
        if( !rawStack && zone.callsite )
        {
            const auto found = m_callsiteStacks.find( zone.callsite );
            if( found != m_callsiteStacks.end() ) rawStack = found->second;
        }
        dto.callstack = Callstack( rawStack );
        // A standard snapshot keeps a zone extra for a stack, text, name or
        // color; its slot number is not reproduced here.
        const bool extra = dto.callstack != 0 || zone.hasText || zone.hasName || zone.hasColor;
        dto.extraIndex = extra ? 1 : 0;
        if( extra )
        {
            dto.extraColor = zone.hasColor ? zone.color : 0;
            if( zone.hasName ) dto.extraName = zone.name;
            if( zone.hasText ) dto.extraText = zone.text;
        }
        if( zone.hasName ) dto.name = zone.name;
        else if( source ) dto.name = source->name.empty() ? source->function : source->name;
        else dto.nameResolved = false;
        if( source )
        {
            dto.function = source->function;
            dto.file = source->file;
            dto.line = source->line;
        }
        if( parentOrdinal ) dto.parentRef = Ref( "cpu-zone", base + *parentOrdinal );
        dto.startNs = zone.start;
        dto.complete = !zone.open;
        if( dto.complete )
        {
            dto.endNs = zone.end;
            dto.selfTimeNs = std::max<int64_t>( 0, zone.end - zone.start - zone.childTime );
        }
        dto.childCount = zone.children;
        if( dto.callstack != 0 ) dto.callstackRef = Ref( "callstack", dto.callstack );
        const auto callsite = m_callsitesByZone.find( ( uint64_t( uint16_t( srcloc ) ) << 32 ) | dto.callstack );
        if( callsite != m_callsitesByZone.end() )
        {
            dto.callsiteId = callsite->second.callsiteId;
            dto.stackProvenance = callsite->second.provenance;
            if( dto.callstack != 0 ) dto.stackRef = Ref( "callstack", dto.callstack );
            dto.stackUnavailableReason = callsite->second.unavailableReason;
        }
        return dto;
    }

    mutable std::mutex m_mutex;
    std::unique_ptr<store::EventStore> m_events;
    std::unique_ptr<WorkerTraceSource> m_worker;
    std::string m_fingerprint, m_refPrefix;
    std::vector<store::StoreZoneThread> m_threads;
    std::vector<uint64_t> m_threadBase;
    std::unordered_map<uint64_t, size_t> m_threadIndex;
    std::unique_ptr<store::StoreZoneCursor> m_scanCursor, m_lookupCursor;
    mutable ZoneScan m_zoneScan, m_gpuScan;
    std::vector<store::StoreGpuZoneGroup> m_gpuGroups;
    std::vector<uint64_t> m_gpuBase;
    std::unordered_map<uint64_t, size_t> m_contextIndex;
    std::unordered_map<size_t, uint64_t> m_gpuCompletedByContext;
    std::unique_ptr<store::StoreGpuZoneCursor> m_gpuScanCursor, m_gpuLookupCursor;
    bool m_gpuQueryIds = true;
    int64_t m_initBegin = 0;
    double m_timerMultiplier = 1;
    std::unordered_map<uint32_t, std::string> m_jobTypeNames;
    std::unordered_map<uint32_t, uint64_t> m_frameIdsBySequence;
    std::unordered_map<uint32_t, CallsiteDto> m_callsitesById;
    std::filesystem::path m_journal;
    ZoneValidationSummaryDto m_zoneValidation;
    // GTMEM marker zones (thread index, ordinal) in global zone order.
    std::vector<std::pair<size_t, uint64_t>> m_gpuMemoryMarkers;
    // Per thread, ordinals of zones without a parent, RootSpacing apart.
    std::vector<std::vector<uint64_t>> m_zoneRoots;
    // For the zones from each of those zones up to the next: earliest start
    // and latest end.
    std::vector<std::vector<int64_t>> m_zoneRootStarts;
    std::vector<std::vector<int64_t>> m_zoneRootEnds;
    // The Jobs and GPU zones near the windows of the last windowed read, and
    // the position of the sequential GPU zone read.
    mutable std::string m_nearJobsKey;
    mutable std::vector<store::StoreJobKey> m_nearJobs;
    struct NearGpu
    {
        std::string key;
        std::vector<std::pair<size_t, uint64_t>> candidates;   // (group, zone id)
        size_t next = SIZE_MAX;
        size_t group = SIZE_MAX;
        uint64_t lastId = 0;
        uint64_t ordinal = 0;
        std::vector<Ancestor> ancestors;
        std::unique_ptr<store::StoreGpuZoneCursor> cursor;
    };
    mutable NearGpu m_nearGpu;
    std::vector<CpuZoneScanThread> m_scanThreads;
    std::vector<CpuZoneScanLocation> m_scanLocations;
    std::unordered_map<SourceKey, uint32_t, SourceKeyHash> m_scanLocationOf;
    uint32_t m_scanMissingLocation = 0;
    std::unordered_map<SourceKey, int16_t, SourceKeyHash> m_sourceIds;
    std::unordered_map<int32_t, SourceLocationDto> m_locations;
    std::unordered_map<uint64_t, CallsiteDto> m_callsitesByZone;
    std::unordered_map<uint64_t, uint64_t> m_callsiteStacks;
    std::vector<uint64_t> m_stackIds;
};

}

std::filesystem::path EventStoreTraceSource::SkeletonPath( const std::filesystem::path& journal )
{
    return WithSuffix( journal, ".query-skeleton.tracy" );
}

bool EventStoreTraceSource::Available( const std::filesystem::path& journal )
{
    std::error_code error;
    return std::filesystem::is_directory( store::EventStore::Directory( journal ), error ) &&
        store::EventStore::StoredSchema( journal ) == store::EventStoreSchema;
}

std::unique_ptr<TraceSource> EventStoreTraceSource::Open( const std::filesystem::path& journal,
    WorkerTraceSource::StateCallback stateCallback )
{
    if( stateCallback ) stateCallback( TraceSourceState::Loading );
    store::StoreOptions options;
    // The manifest records each file's size, write time and id; an unchanged
    // store opens without hashing its files.
    options.verifyContent = false;
    options.cacheBytes = 256ull << 20;
    options.resultBytes = 64ull << 20;
    auto events = store::EventStore::Open( journal, options );
    if( !events ) throw TraceLoadError( TraceLoadErrorCode::OpenFailed, "the event store cannot be opened" );
    const auto skeleton = EnsureSkeleton( journal, *events, stateCallback );
    auto worker = WorkerTraceSource::Open( skeleton, {}, events->Info().sourceSha256, WorkerTraceLoadMode::Full );
    auto source = std::make_unique<StoreTraceSource>( std::move( events ), std::move( worker ), journal );
    if( stateCallback ) stateCallback( TraceSourceState::Ready );
    return source;
}

}

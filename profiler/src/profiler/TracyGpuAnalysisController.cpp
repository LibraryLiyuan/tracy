#include "TracyGpuAnalysisController.hpp"

#include "TracyGpuAnalysisCache.hpp"
#include "TracyHash.hpp"
#include "TracyStorage.hpp"
#include "TracyWorker.hpp"

#include <chrono>
#include <algorithm>
#include <functional>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#  include <windows.h>
#endif

namespace tracy
{
namespace
{

constexpr const char* GuiBuildIdentity = "N27-GUI-preview-a1-c2";

analysis::GpuMemoryAttribution BuildWorkerGpuMemoryAttribution( Worker& worker )
{
    using namespace analysis;

    std::vector<GpuMemoryCpuZoneInput> cpuInputs;
    for( const auto& sourceEntry : worker.GetSourceLocationZones() )
    {
        const auto& sourceLocation = worker.GetSourceLocation( sourceEntry.first );
        const auto markerNamePtr = worker.GetZoneName( sourceLocation );
        const std::string markerName = markerNamePtr ? markerNamePtr : "";
        if( markerName != GpuMemoryRequestMarker && markerName != GpuMemoryPassMarker &&
            markerName != GpuMemoryOriginMarker && markerName != GpuMemoryResidencyMarker ) continue;
        for( const auto& zoneThread : sourceEntry.second.zones )
        {
            const auto zone = zoneThread.Zone();
            if( !zone || zone->End() < 0 || !worker.HasZoneExtra( *zone ) ) continue;
            const auto& extra = worker.GetZoneExtra( *zone );
            if( !extra.text.Active() ) continue;
            const auto text = worker.GetString( extra.text );
            if( !text ) continue;
            const auto zoneName = worker.GetZoneName( *zone );
            cpuInputs.push_back( {
                cpuInputs.size(), markerName, zoneName ? zoneName : markerName, text,
                worker.DecompressThread( zoneThread.Thread() ), zone->Start(), worker.GetZoneEnd( *zone )
            } );
        }
    }

    const auto& jn = worker.GetJnTraceData();
    struct ReferenceCandidate { uint64_t token; int64_t time; };
    std::unordered_map<uint64_t, const JnGfxEntityData*> entityById;
    std::unordered_map<uint64_t, uint64_t> segmentByPass;
    std::unordered_map<uint64_t, uint64_t> referenceByPass;
    std::unordered_map<uint64_t, uint64_t> referenceBySegment;
    std::unordered_map<uint64_t, std::vector<ReferenceCandidate>> referenceByQuery;
    std::unordered_map<uint64_t, std::vector<size_t>> zonePositionsByQuery;
    std::unordered_map<const GpuEvent*, uint16_t> contextByZone;
    std::unordered_set<uint64_t> submittedCommandLists;
    std::unordered_set<uint64_t> gpuSegmentReferenceTokens;
    entityById.reserve( jn.gfxEntities.size() );
    for( const auto& entity : jn.gfxEntities ) entityById.emplace( entity.entityId, &entity );
    for( const auto& link : jn.gfxLinks )
    {
        if( link.relation == 5 ) segmentByPass[link.sourceId] = link.targetId;
        else if( link.relation == 11 ) referenceByPass[link.sourceId] = link.targetId;
        else if( link.relation == 4 ) submittedCommandLists.emplace( link.sourceId );
        else if( link.relation == 13 ) referenceBySegment[link.sourceId] = link.targetId;
    }
    for( const auto& [passId, referenceToken] : referenceByPass )
    {
        const auto segmentLink = segmentByPass.find( passId );
        if( segmentLink == segmentByPass.end() ) continue;
        const auto segment = entityById.find( segmentLink->second );
        if( segment == entityById.end() || segment->second->kind != 4 ) continue;
        gpuSegmentReferenceTokens.emplace( referenceToken );
        const uint64_t key = ( uint64_t( segment->second->gpuContext ) << 32 ) | segment->second->gpuQueryId;
        referenceByQuery[key].push_back( { referenceToken, segment->second->time } );
    }
    for( const auto& [segmentId, referenceToken] : referenceBySegment )
    {
        gpuSegmentReferenceTokens.emplace( referenceToken );
        const auto segment = entityById.find( segmentId );
        if( segment == entityById.end() || segment->second->kind != 4 ) continue;
        const uint64_t key = ( uint64_t( segment->second->gpuContext ) << 32 ) | segment->second->gpuQueryId;
        referenceByQuery[key].push_back( { referenceToken, segment->second->time } );
    }

    const auto& gpuContexts = worker.GetGpuData();
    for( uint16_t contextIndex = 0; contextIndex < gpuContexts.size(); ++contextIndex )
    {
        const auto* context = gpuContexts[contextIndex];
        std::function<void( const Vector<short_ptr<GpuEvent>>& )> visit;
        visit = [&]( const Vector<short_ptr<GpuEvent>>& timeline )
        {
            if( timeline.is_magic() )
            {
                const auto& values = *reinterpret_cast<const Vector<GpuEvent>*>( &timeline );
                for( const auto& zone : values )
                {
                    contextByZone.emplace( &zone, contextIndex );
                    if( zone.Child() >= 0 ) visit( worker.GetGpuChildren( zone.Child() ) );
                }
            }
            else
            {
                for( const auto& zonePointer : timeline )
                {
                    const GpuEvent* zone = zonePointer;
                    contextByZone.emplace( zone, contextIndex );
                    if( zone->Child() >= 0 ) visit( worker.GetGpuChildren( zone->Child() ) );
                }
            }
        };
        for( const auto& [thread, threadData] : context->threadData ) visit( threadData.timeline );
    }

    std::vector<GpuMemoryGpuZoneInput> gpuInputs;
    if( worker.AreGpuSourceLocationZonesReady() )
    {
        for( const auto& sourceEntry : worker.GetGpuSourceLocationZones() )
        {
            for( const auto& zoneThread : sourceEntry.second.zones )
            {
                const auto zone = zoneThread.Zone();
                if( !zone || zone->GpuEnd() < 0 ) continue;
                const auto name = worker.GetZoneName( *zone );
                if( !name ) continue;
                const auto thread = zone->Thread() != 0 ? worker.DecompressThread( zone->Thread() ) : worker.DecompressThread( zoneThread.Thread() );
                const auto context = contextByZone.find( zone );
                if( context == contextByZone.end() ) continue;
                const uint64_t key = ( uint64_t( context->second ) << 32 ) | zone->query_id;
                zonePositionsByQuery[key].emplace_back( gpuInputs.size() );
                gpuInputs.push_back( { gpuInputs.size(), name, thread, zone->CpuStart(), zone->GpuStart(), zone->GpuEnd(), 0 } );
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

    std::vector<GpuMemoryAllocationInput> allocations;
    for( const auto& memoryEntry : worker.GetMemNameMap() )
    {
        const auto pool = memoryEntry.first;
        const auto poolName = pool == 0 ? std::string( "Default allocator" ) : std::string( worker.GetString( pool ) ? worker.GetString( pool ) : "" );
        if( !IsGpuD3D12PoolName( poolName ) ) continue;
        const auto& memory = *memoryEntry.second;
        for( size_t index = 0; index < memory.data.size(); ++index )
        {
            const auto& event = memory.data[index];
            if( event.Ptr() == 0 ) continue;
            allocations.push_back( { { pool, index }, event.Ptr(), event.Size(), worker.DecompressThread( event.ThreadAlloc() ),
                event.TimeAlloc(), event.TimeFree() >= 0 ? std::optional<int64_t>( event.TimeFree() ) : std::nullopt,
                event.CsAlloc(), event.csFree.Val(), poolName } );
        }
    }

    std::vector<GpuMemoryReferencePassInput> structuredReferencePasses;
    std::unordered_map<uint64_t, size_t> structuredReferenceById;
    structuredReferencePasses.reserve( jn.gpuReferencePasses.size() );
    structuredReferenceById.reserve( jn.gpuReferencePasses.size() );
    for( const auto& value : jn.gpuReferencePasses )
    {
        if( value.passId == 0 || structuredReferenceById.contains( value.passId ) ) continue;
        GpuMemoryReferencePassInput input;
        input.passId = value.passId; input.frame = value.frameIndex; input.thread = value.thread;
        input.start = value.time; input.end = value.time; input.taxonomyId = value.taxonomyId;
        input.taxonomyLevel = value.taxonomyLevel; input.flags = value.flags;
        structuredReferenceById.emplace( value.passId, structuredReferencePasses.size() );
        structuredReferencePasses.emplace_back( std::move( input ) );
    }
    for( const auto& value : jn.gpuReferenceUses )
    {
        const auto pass = structuredReferenceById.find( value.passId );
        if( pass == structuredReferenceById.end() || value.resourceId == 0 ) continue;
        structuredReferencePasses[pass->second].uses.push_back( { value.resourceId, value.usageMask, 'U', value.resourceSetId, value.encoding } );
    }
    for( const auto& value : jn.gpuReferenceEnds )
    {
        const auto pass = structuredReferenceById.find( value.passId );
        if( pass == structuredReferenceById.end() ) continue;
        auto& input = structuredReferencePasses[pass->second];
        input.end = value.time; input.commandListId = value.commandListId;
        input.totalUseCount = value.totalReferenceCount; input.droppedUses = value.droppedReferenceCount;
        input.flags |= value.flags; input.ended = true;
    }
    for( const auto& value : jn.relations )
    {
        if( value.relationNamespace != uint8_t( JnRelationNamespace::GpuReference ) ||
            value.relation != uint8_t( JnRelationKind::LogicalParent ) ) continue;
        const auto child = structuredReferenceById.find( value.sourceId );
        if( child != structuredReferenceById.end() ) structuredReferencePasses[child->second].parentPassId = value.targetId;
    }
    auto result = BuildGpuMemoryAttribution( cpuInputs, gpuInputs, allocations, submittedCommandLists,
        gpuSegmentReferenceTokens, structuredReferencePasses, worker.GetLastTime() );
    for( auto& pass : result.passes )
    {
        if( pass.gpuPairing != GpuZonePairing::Exact || !pass.gpuZoneIndex || *pass.gpuZoneIndex >= gpuInputs.size() ) continue;
        const auto& zone = gpuInputs[*pass.gpuZoneIndex];
        pass.start = zone.gpuStartNs;
        pass.end = zone.gpuEndNs;
    }
    return result;
}

uint64_t EstimateSnapshotBytes( const analysis::GpuAnalysisSnapshot& value )
{
    uint64_t bytes = sizeof( value ) + value.resources.capacity() * sizeof( analysis::GpuResourceAnalysisRecord ) +
        value.allocations.capacity() * sizeof( analysis::GpuAllocationAnalysisRecord ) + value.passes.capacity() * sizeof( analysis::GpuPassWorkingSet ) +
        value.residency.capacity() * sizeof( analysis::GpuResidencyInterval ) + value.churnCandidates.capacity() * sizeof( analysis::GpuChurnCandidate );
    for( const auto& resource : value.resources ) bytes += resource.name.capacity() +
        ( resource.history.capacity() + resource.views.capacity() + resource.parts.capacity() + resource.relations.capacity() + resource.ranges.capacity() ) * sizeof( size_t );
    for( const auto& allocation : value.allocations ) bytes += allocation.history.capacity() * sizeof( size_t ) + allocation.resources.capacity() * sizeof( uint64_t );
    for( const auto& pass : value.passes ) bytes += pass.name.capacity() +
        ( pass.directResources.capacity() + pass.inclusiveResources.capacity() ) * sizeof( uint64_t );
    for( const auto& candidate : value.churnCandidates ) bytes += candidate.reason.capacity();
    return bytes;
}

}

GpuAnalysisController::GpuAnalysisController( Worker& worker, std::filesystem::path tracePath )
    : m_worker( worker )
    , m_tracePath( std::move( tracePath ) )
{}

GpuAnalysisController::~GpuAnalysisController()
{
    Cancel();
}

void GpuAnalysisController::Start()
{
    if( m_worker.IsConnected() )
    {
        PublishStatus( GpuAnalysisControllerState::Idle, 0, "waiting-for-capture-end", "GPU deep analysis is offline-first" );
        return;
    }
    {
        std::lock_guard lock( m_mutex );
        if( m_status.state == GpuAnalysisControllerState::Building || m_status.state == GpuAnalysisControllerState::Ready ||
            m_status.state == GpuAnalysisControllerState::Partial || m_status.state == GpuAnalysisControllerState::NotPresent ) return;
        m_status = {}; m_status.state = GpuAnalysisControllerState::Building; m_status.stage = "queued";
        m_snapshot.reset();
    }
    if( m_thread.joinable() ) { m_thread.request_stop(); m_thread.join(); }
    m_thread = std::jthread( [this]( std::stop_token token ) { Run( token ); } );
}

void GpuAnalysisController::Cancel()
{
    if( m_thread.joinable() )
    {
        m_thread.request_stop();
        m_thread.join();
    }
    std::lock_guard lock( m_mutex );
    if( m_status.state == GpuAnalysisControllerState::Building )
    {
        m_status.state = GpuAnalysisControllerState::Cancelled;
        m_status.stage = "cancelled";
    }
}

void GpuAnalysisController::Retry()
{
    Cancel();
    { std::lock_guard lock( m_mutex ); m_status = {}; m_snapshot.reset(); }
    Start();
}

bool GpuAnalysisController::ClearCache( std::string& error )
{
    error.clear();
    std::filesystem::path target;
    { std::lock_guard lock( m_mutex ); target = m_cachePath.parent_path(); }
    if( target.empty() ) { error = "cache_identity_not_available"; return false; }
    const auto root = std::filesystem::weakly_canonical( std::filesystem::path( GetSavePath( "ProfilerCache" ) ) );
    const auto resolved = std::filesystem::weakly_canonical( target );
    const auto rootString = root.generic_string(); const auto targetString = resolved.generic_string();
    if( targetString.size() <= rootString.size() || targetString.rfind( rootString + '/', 0 ) != 0 )
    { error = "cache_path_outside_profiler_cache"; return false; }
    std::error_code ec; std::filesystem::remove_all( resolved, ec );
    if( ec ) { error = ec.message(); return false; }
    return true;
}

GpuAnalysisControllerStatus GpuAnalysisController::Status() const
{
    std::lock_guard lock( m_mutex ); return m_status;
}

std::shared_ptr<const analysis::GpuAnalysisSnapshot> GpuAnalysisController::Snapshot() const
{
    std::lock_guard lock( m_mutex ); return m_snapshot;
}

void GpuAnalysisController::PublishStatus( GpuAnalysisControllerState state, float progress, std::string stage, std::string error )
{
    std::lock_guard lock( m_mutex );
    m_status.state = state; m_status.progress = progress; m_status.stage = std::move( stage ); m_status.error = std::move( error );
}

void GpuAnalysisController::Run( std::stop_token stopToken )
{
#ifdef _WIN32
    SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL );
#endif
    try
    {
        analysis::GpuAnalysisCacheIdentity identity;
        if( !m_tracePath.empty() && std::filesystem::is_regular_file( m_tracePath ) )
        {
            PublishStatus( GpuAnalysisControllerState::Building, 0.01f, "trace-fingerprint" );
            identity.traceSize = std::filesystem::file_size( m_tracePath );
            identity.traceSha256 = analysis::Sha256File( m_tracePath );
            identity.guiBuild = GuiBuildIdentity;
            const auto cacheRoot = std::filesystem::path( GetSavePath( "ProfilerCache" ) );
            const auto directory = identity.traceSha256 + "-a1-c2-" + GuiBuildIdentity;
            { std::lock_guard lock( m_mutex ); m_cachePath = cacheRoot / directory / "snapshot.bin"; }
            std::string cacheError;
            if( auto cached = analysis::LoadGpuAnalysisCache( m_cachePath, identity, cacheError ) )
            {
                auto value = std::make_shared<analysis::GpuAnalysisSnapshot>( std::move( *cached ) );
                std::lock_guard lock( m_mutex );
                m_snapshot = std::move( value ); m_status.state = GpuAnalysisControllerState::Ready; m_status.progress = 1;
                m_status.stage = "ready-cache"; m_status.cacheHit = true; m_status.estimatedBytes = EstimateSnapshotBytes( *m_snapshot ); m_status.error.clear();
                return;
            }
        }
        if( stopToken.stop_requested() ) { PublishStatus( GpuAnalysisControllerState::Cancelled, 0, "cancelled" ); return; }
        analysis::GpuAnalysisBuildControl control;
        control.stopToken = stopToken;
        control.progress = [this]( float value, const char* stage ) { PublishStatus( GpuAnalysisControllerState::Building, value, stage ); };
        PublishStatus( GpuAnalysisControllerState::Building, 0.03f, "gpu-attribution" );
        const auto attribution = BuildWorkerGpuMemoryAttribution( m_worker );
        auto built = analysis::BuildGpuAnalysisSnapshot( m_worker.GetJnTraceData(), &attribution, {}, control );
        if( built.manifest.state == analysis::GpuAnalysisState::Cancelled || stopToken.stop_requested() )
        { PublishStatus( GpuAnalysisControllerState::Cancelled, 0, "cancelled" ); return; }
        auto value = std::make_shared<analysis::GpuAnalysisSnapshot>( std::move( built ) );
        const auto finalState = value->manifest.state == analysis::GpuAnalysisState::NotPresent ? GpuAnalysisControllerState::NotPresent :
            value->manifest.state == analysis::GpuAnalysisState::Complete ? GpuAnalysisControllerState::Ready :
            value->manifest.state == analysis::GpuAnalysisState::Partial || value->manifest.state == analysis::GpuAnalysisState::ResourceLimit ?
            GpuAnalysisControllerState::Partial : GpuAnalysisControllerState::Failed;
        if( !m_cachePath.empty() && finalState != GpuAnalysisControllerState::Failed )
        {
            std::string cacheError; analysis::SaveGpuAnalysisCache( m_cachePath, identity, *value, cacheError );
            if( !cacheError.empty() ) { std::lock_guard lock( m_mutex ); m_status.error = "cache:" + cacheError; }
        }
        std::lock_guard lock( m_mutex );
        m_snapshot = std::move( value ); m_status.state = finalState; m_status.progress = 1;
        m_status.stage = finalState == GpuAnalysisControllerState::Ready ? "ready" : finalState == GpuAnalysisControllerState::NotPresent ? "not-present" : "partial";
        m_status.estimatedBytes = EstimateSnapshotBytes( *m_snapshot );
        if( finalState == GpuAnalysisControllerState::Failed || finalState == GpuAnalysisControllerState::NotPresent ) m_status.error = m_snapshot->manifest.reason;
    }
    catch( const std::exception& e ) { PublishStatus( GpuAnalysisControllerState::Failed, 0, "failed", e.what() ); }
}

const char* GpuAnalysisControllerStateName( GpuAnalysisControllerState value )
{
    switch( value )
    {
    case GpuAnalysisControllerState::Idle: return "idle"; case GpuAnalysisControllerState::Building: return "building";
    case GpuAnalysisControllerState::NotPresent: return "not-present";
    case GpuAnalysisControllerState::Ready: return "ready"; case GpuAnalysisControllerState::Partial: return "partial";
    case GpuAnalysisControllerState::Cancelled: return "cancelled"; case GpuAnalysisControllerState::Failed: return "failed";
    }
    return "unknown";
}

}

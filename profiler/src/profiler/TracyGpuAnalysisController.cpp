#include "TracyGpuAnalysisController.hpp"

#include "TracyGpuAnalysisSidecar.hpp"
#include "TracyGpuAnalysisStore.hpp"
#include "TracyWorker.hpp"

#include <chrono>
#include <algorithm>
#include <fstream>
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

std::filesystem::path BuilderExecutablePath()
{
#ifdef _WIN32
    std::vector<wchar_t> buffer( 32768 );
    const auto length = GetModuleFileNameW( nullptr, buffer.data(), DWORD( buffer.size() ) );
    if( length == 0 || length >= buffer.size() ) return {};
    return std::filesystem::path( std::wstring( buffer.data(), length ) ).parent_path() / "tracy-gpu-analysis-build.exe";
#else
    return "tracy-gpu-analysis-build";
#endif
}

bool BuilderLeaseActive( const std::filesystem::path& sidecar )
{
    std::error_code ec; const auto heartbeat = sidecar / ".writer-lease" / "heartbeat";
    const auto write = std::filesystem::last_write_time( heartbeat, ec );
    if( ec ) return false;
    return std::chrono::duration_cast<std::chrono::seconds>( std::filesystem::file_time_type::clock::now() - write ).count() <= 30;
}

#ifdef _WIN32
std::wstring QuoteArgument( const std::filesystem::path& value )
{
    std::wstring out = L"\"";
    for( const auto ch : value.wstring() ) { if( ch == L'\"' ) out += L'\\'; out += ch; }
    out += L'\"'; return out;
}

bool RunExternalBuilder( const std::filesystem::path& executable, const std::filesystem::path& trace,
    const std::filesystem::path& progress, const std::filesystem::path& cancel, std::stop_token stopToken,
    const std::atomic<bool>& cancelOnStop, const std::function<void()>& poll, DWORD& exitCode, bool& detached, std::string& error )
{
    detached = false;
    std::error_code ignored; std::filesystem::remove( cancel, ignored );
    auto command = QuoteArgument( executable ) + L" --trace " + QuoteArgument( trace ) +
        L" --progress-json " + QuoteArgument( progress ) + L" --cancel-file " + QuoteArgument( cancel );
    STARTUPINFOW startup {}; startup.cb = sizeof( startup ); PROCESS_INFORMATION process {};
    std::vector<wchar_t> mutableCommand( command.begin(), command.end() ); mutableCommand.push_back( 0 );
    if( !CreateProcessW( executable.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, executable.parent_path().c_str(), &startup, &process ) )
    { error = "builder_launch_failed:" + std::to_string( GetLastError() ); return false; }
    CloseHandle( process.hThread );
    for( ;; )
    {
        const auto wait = WaitForSingleObject( process.hProcess, 200 ); poll();
        if( stopToken.stop_requested() )
        {
            if( cancelOnStop.load( std::memory_order_acquire ) )
            { std::ofstream request( cancel, std::ios::binary | std::ios::trunc ); request << "cancel\n"; }
            else
            {
                CloseHandle( process.hProcess ); detached = true; return true;
            }
        }
        if( wait == WAIT_OBJECT_0 ) break;
        if( wait == WAIT_FAILED ) { error = "builder_wait_failed:" + std::to_string( GetLastError() ); CloseHandle( process.hProcess ); return false; }
    }
    GetExitCodeProcess( process.hProcess, &exitCode ); CloseHandle( process.hProcess );
    std::filesystem::remove( cancel, ignored ); return true;
}
#endif

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
    for( const auto& resource : value.resources )
    {
        bytes += resource.name.capacity() + resource.history.capacity() * sizeof( size_t ) +
            resource.views.capacity() * sizeof( analysis::GpuViewAnalysisRecord ) +
            resource.parts.capacity() * sizeof( analysis::GpuPartAnalysisRecord ) +
            resource.relations.capacity() * sizeof( analysis::GpuRelationAnalysisRecord ) +
            resource.ranges.capacity() * sizeof( analysis::GpuRangeAnalysisRecord ) +
            resource.virtualGeometry.capacity() * sizeof( analysis::GpuVgAnalysisRecord );
        for( const auto& logical : resource.logicals ) bytes += sizeof( logical ) + logical.name.capacity();
    }
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
    // The builder is a standalone checkpointed process. Closing a trace or
    // the Profiler detaches from it; only the explicit Cancel action creates
    // a cancellation request.
    m_cancelBuilderOnStop.store( false, std::memory_order_release );
    StopPageLoad();
    if( m_thread.joinable() ) { m_thread.request_stop(); m_thread.join(); }
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
        m_reader.reset(); m_snapshot.reset(); m_resourcePageData.clear(); m_allocationPageData.clear(); m_passPageData.clear();
    }
    m_cancelBuilderOnStop.store( false, std::memory_order_release );
    StopPageLoad();
    if( m_thread.joinable() ) { m_thread.request_stop(); m_thread.join(); }
    m_thread = std::jthread( [this]( std::stop_token token ) { Run( token ); } );
}

void GpuAnalysisController::Cancel()
{
    m_cancelBuilderOnStop.store( true, std::memory_order_release );
    StopPageLoad();
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
    { std::lock_guard lock( m_mutex ); m_status = {}; m_reader.reset(); m_snapshot.reset();
      m_resourcePageData.clear(); m_allocationPageData.clear(); m_passPageData.clear(); }
    Start();
}

bool GpuAnalysisController::ClearCache( std::string& error )
{
    error.clear();
    StopPageLoad();
    std::filesystem::path sidecar;
    {
        std::lock_guard lock( m_mutex );
        m_reader.reset(); m_snapshot.reset(); m_resourcePageData.clear(); m_allocationPageData.clear(); m_passPageData.clear();
        sidecar = analysis::GpuAnalysisSidecarPath( m_tracePath );
    }
    if( sidecar.empty() || !std::filesystem::exists( sidecar / "manifest" ) ) { error = "gpu_analysis_sidecar_not_found"; return false; }
    const auto target = analysis::GpuAnalysisDerivedPath( sidecar );
    const auto resolvedSidecar = std::filesystem::weakly_canonical( sidecar );
    const auto resolvedTarget = std::filesystem::weakly_canonical( target );
    if( resolvedTarget.generic_string().rfind( resolvedSidecar.generic_string() + '/', 0 ) != 0 )
    { error = "gpu_analysis_cache_path_invalid"; return false; }
    auto retired = target; retired += ".retired-" + std::to_string( std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch() ).count() );
    std::error_code ec; std::filesystem::rename( target, retired, ec );
    if( ec ) { error = ec.message(); return false; }
    std::filesystem::remove_all( retired, ec );
    if( ec ) { error = "cache_retired_pending:" + ec.message(); return false; }
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

size_t GpuAnalysisController::ResourcePageCount() const { std::lock_guard lock( m_mutex ); return m_reader ? m_reader->ResourcePageCount() : 0; }
size_t GpuAnalysisController::AllocationPageCount() const { std::lock_guard lock( m_mutex ); return m_reader ? m_reader->AllocationPageCount() : 0; }
size_t GpuAnalysisController::PassPageCount() const { std::lock_guard lock( m_mutex ); return m_reader ? m_reader->PassPageCount() : 0; }
uint64_t GpuAnalysisController::ResourceCount() const { std::lock_guard lock( m_mutex ); return m_reader ? m_reader->Manifest().resourceCount : 0; }
uint64_t GpuAnalysisController::AllocationCount() const { std::lock_guard lock( m_mutex ); return m_reader ? m_reader->Manifest().allocationCount : 0; }
uint64_t GpuAnalysisController::PassCount() const { std::lock_guard lock( m_mutex ); return m_reader ? m_reader->Manifest().passCount : 0; }
uint64_t GpuAnalysisController::ResidencyCount() const { std::lock_guard lock( m_mutex ); return m_reader ? m_reader->Manifest().residencyCount : 0; }
size_t GpuAnalysisController::ResourcePage() const { std::lock_guard lock( m_mutex ); return m_resourcePage; }
size_t GpuAnalysisController::AllocationPage() const { std::lock_guard lock( m_mutex ); return m_allocationPage; }
size_t GpuAnalysisController::PassPage() const { std::lock_guard lock( m_mutex ); return m_passPage; }

void GpuAnalysisController::RebuildPageSnapshotLocked()
{
    if( !m_reader ) { m_snapshot.reset(); return; }
    auto value = std::make_shared<analysis::GpuAnalysisSnapshot>( m_reader->Overview() );
    value->resources = m_resourcePageData; value->allocations = m_allocationPageData; value->passes = m_passPageData;
    for( size_t i = 0; i < value->resources.size(); ++i ) value->resourceById[value->resources[i].resourceId] = i;
    for( size_t i = 0; i < value->allocations.size(); ++i ) value->allocationById[value->allocations[i].allocationId] = i;
    for( size_t i = 0; i < value->passes.size(); ++i ) value->passById[value->passes[i].passId] = i;
    m_snapshot = std::move( value );
}

bool GpuAnalysisController::PublishReader( std::shared_ptr<analysis::GpuAnalysisStoreReader> reader, bool cacheHit, std::string& error )
{
    std::vector<analysis::GpuResourceAnalysisRecord> resources; std::vector<analysis::GpuAllocationAnalysisRecord> allocations;
    std::vector<analysis::GpuPassWorkingSet> passes;
    if( reader->ResourcePageCount() && !reader->LoadResourcePage( 0, resources, error ) ) return false;
    if( reader->AllocationPageCount() && !reader->LoadAllocationPage( 0, allocations, error ) ) return false;
    if( reader->PassPageCount() && !reader->LoadPassPage( 0, passes, error ) ) return false;
    std::lock_guard lock( m_mutex ); m_reader = std::move( reader ); m_resourcePageData = std::move( resources );
    m_allocationPageData = std::move( allocations ); m_passPageData = std::move( passes );
    m_resourcePage = m_allocationPage = m_passPage = 0; RebuildPageSnapshotLocked();
    m_status.state = m_snapshot->manifest.state == analysis::GpuAnalysisState::Complete ? GpuAnalysisControllerState::Ready : GpuAnalysisControllerState::Partial;
    m_status.progress = 1; m_status.stage = "ready-sidecar-paged"; m_status.cacheHit = cacheHit;
    m_status.estimatedBytes = EstimateSnapshotBytes( *m_snapshot ); m_status.error.clear(); return true;
}

bool GpuAnalysisController::LoadResourcePage( size_t page, std::string& error )
{
    std::shared_ptr<analysis::GpuAnalysisStoreReader> reader;
    {
        std::lock_guard lock( m_mutex ); reader = m_reader;
        if( !reader ) { error = "gpu_analysis_reader_not_ready"; return false; }
        if( page >= reader->ResourcePageCount() ) { error = "gpu_analysis_page_out_of_range"; return false; }
        if( m_pageLoading ) { error = "gpu_analysis_page_load_in_progress"; return false; }
        m_pageLoading = true; m_status.stage = "loading-resource-page"; m_status.error.clear();
    }
    if( m_pageThread.joinable() ) m_pageThread.join();
    m_pageThread = std::jthread( [this, reader = std::move( reader ), page]( std::stop_token token ) {
        std::string loadError; std::vector<analysis::GpuResourceAnalysisRecord> values;
        const bool loaded = !token.stop_requested() && reader->LoadResourcePage( page, values, loadError );
        std::lock_guard lock( m_mutex );
        if( loaded && !token.stop_requested() && reader == m_reader )
        { m_resourcePageData = std::move( values ); m_resourcePage = page; RebuildPageSnapshotLocked(); m_status.stage = "ready-sidecar-paged"; }
        else if( !token.stop_requested() ) m_status.error = loadError.empty() ? "gpu_analysis_generation_changed" : std::move( loadError );
        m_pageLoading = false;
    } );
    return true;
}

bool GpuAnalysisController::LoadAllocationPage( size_t page, std::string& error )
{
    std::shared_ptr<analysis::GpuAnalysisStoreReader> reader;
    {
        std::lock_guard lock( m_mutex ); reader = m_reader;
        if( !reader ) { error = "gpu_analysis_reader_not_ready"; return false; }
        if( page >= reader->AllocationPageCount() ) { error = "gpu_analysis_page_out_of_range"; return false; }
        if( m_pageLoading ) { error = "gpu_analysis_page_load_in_progress"; return false; }
        m_pageLoading = true; m_status.stage = "loading-allocation-page"; m_status.error.clear();
    }
    if( m_pageThread.joinable() ) m_pageThread.join();
    m_pageThread = std::jthread( [this, reader = std::move( reader ), page]( std::stop_token token ) {
        std::string loadError; std::vector<analysis::GpuAllocationAnalysisRecord> values;
        const bool loaded = !token.stop_requested() && reader->LoadAllocationPage( page, values, loadError );
        std::lock_guard lock( m_mutex );
        if( loaded && !token.stop_requested() && reader == m_reader )
        { m_allocationPageData = std::move( values ); m_allocationPage = page; RebuildPageSnapshotLocked(); m_status.stage = "ready-sidecar-paged"; }
        else if( !token.stop_requested() ) m_status.error = loadError.empty() ? "gpu_analysis_generation_changed" : std::move( loadError );
        m_pageLoading = false;
    } );
    return true;
}

bool GpuAnalysisController::LoadPassPage( size_t page, std::string& error )
{
    std::shared_ptr<analysis::GpuAnalysisStoreReader> reader;
    {
        std::lock_guard lock( m_mutex ); reader = m_reader;
        if( !reader ) { error = "gpu_analysis_reader_not_ready"; return false; }
        if( page >= reader->PassPageCount() ) { error = "gpu_analysis_page_out_of_range"; return false; }
        if( m_pageLoading ) { error = "gpu_analysis_page_load_in_progress"; return false; }
        m_pageLoading = true; m_status.stage = "loading-pass-page"; m_status.error.clear();
    }
    if( m_pageThread.joinable() ) m_pageThread.join();
    m_pageThread = std::jthread( [this, reader = std::move( reader ), page]( std::stop_token token ) {
        std::string loadError; std::vector<analysis::GpuPassWorkingSet> values;
        const bool loaded = !token.stop_requested() && reader->LoadPassPage( page, values, loadError );
        std::lock_guard lock( m_mutex );
        if( loaded && !token.stop_requested() && reader == m_reader )
        { m_passPageData = std::move( values ); m_passPage = page; m_passFrameView = false; RebuildPageSnapshotLocked(); m_status.stage = "ready-sidecar-paged"; }
        else if( !token.stop_requested() ) m_status.error = loadError.empty() ? "gpu_analysis_generation_changed" : std::move( loadError );
        m_pageLoading = false;
    } );
    return true;
}

bool GpuAnalysisController::LoadPassFrame( uint64_t frame, std::string& error )
{
    std::shared_ptr<analysis::GpuAnalysisStoreReader> reader;
    {
        std::lock_guard lock( m_mutex ); reader = m_reader;
        if( !reader ) { error = "gpu_analysis_reader_not_ready"; return false; }
        if( m_pageLoading ) { error = "gpu_analysis_page_load_in_progress"; return false; }
        if( m_passFrameView && m_passFrame == frame ) return true;
        m_pageLoading = true; m_status.stage = "loading-frame-pass-index"; m_status.error.clear();
    }
    if( m_pageThread.joinable() ) m_pageThread.join();
    m_pageThread = std::jthread( [this, reader = std::move( reader ), frame]( std::stop_token token ) {
        std::string loadError; std::vector<analysis::GpuPassWorkingSet> values; bool hasMore = false;
        const bool loaded = !token.stop_requested() && reader->PassesForFrame( frame, 0, 100000, values, hasMore, loadError );
        if( loaded && hasMore ) loadError = "gpu_analysis_frame_pass_limit";
        std::lock_guard lock( m_mutex );
        if( loaded && !hasMore && !token.stop_requested() && reader == m_reader )
        {
            m_passPageData = std::move( values ); m_passFrame = frame; m_passFrameView = true;
            RebuildPageSnapshotLocked(); m_status.stage = "ready-frame-priority";
        }
        else if( !token.stop_requested() ) m_status.error = loadError.empty() ? "gpu_analysis_generation_changed" : std::move( loadError );
        m_pageLoading = false;
    } );
    return true;
}

void GpuAnalysisController::StopPageLoad()
{
    if( m_pageThread.joinable() ) { m_pageThread.request_stop(); m_pageThread.join(); }
    std::lock_guard lock( m_mutex ); m_pageLoading = false;
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
        if( m_tracePath.empty() || !std::filesystem::is_regular_file( m_tracePath ) )
        {
            PublishStatus( GpuAnalysisControllerState::NotPresent, 0, "sidecar-unavailable", "saved trace path is required" );
            return;
        }
        const auto sidecar = analysis::GpuAnalysisSidecarPath( m_tracePath );
        const auto derived = analysis::GpuAnalysisDerivedPath( sidecar );
        { std::lock_guard lock( m_mutex ); m_cachePath = derived; }
        std::string sidecarError;
        analysis::GpuAnalysisSidecarManifest sidecarManifest;
        if( auto reader = analysis::GpuAnalysisStoreReader::Open( m_tracePath, false, &sidecarManifest, sidecarError ) )
        {
            if( PublishReader( std::move( reader ), true, sidecarError ) ) return;
            PublishStatus( GpuAnalysisControllerState::Failed, 0, "sidecar-page-load-failed", sidecarError ); return;
        }
        if( stopToken.stop_requested() ) { PublishStatus( GpuAnalysisControllerState::Cancelled, 0, "cancelled" ); return; }
        if( sidecarManifest.state == analysis::GpuAnalysisSidecarState::DerivedBuilding && BuilderLeaseActive( sidecar ) )
        {
            PublishStatus( GpuAnalysisControllerState::Building, .4f, "attached-to-existing-builder", sidecarManifest.reason );
            while( !stopToken.stop_requested() && BuilderLeaseActive( sidecar ) )
            {
                std::this_thread::sleep_for( std::chrono::milliseconds( 250 ) );
                std::string manifestError;
                if( const auto current = analysis::LoadGpuAnalysisSidecarManifest( sidecar, manifestError ) )
                {
                    sidecarManifest = *current;
                    if( current->state == analysis::GpuAnalysisSidecarState::Ready && current->derivedComplete )
                    {
                        auto reader = analysis::GpuAnalysisStoreReader::Open( m_tracePath, false, &sidecarManifest, sidecarError );
                        if( reader && PublishReader( std::move( reader ), true, sidecarError ) ) return;
                        break;
                    }
                    if( current->state == analysis::GpuAnalysisSidecarState::Failed || current->state == analysis::GpuAnalysisSidecarState::Cancelled ) break;
                    PublishStatus( GpuAnalysisControllerState::Building, .4f, "attached-to-existing-builder", current->reason );
                }
            }
            if( stopToken.stop_requested() )
            {
                if( m_cancelBuilderOnStop.load( std::memory_order_acquire ) )
                { std::ofstream request( sidecar / "builder-cancel.request", std::ios::binary | std::ios::trunc ); request << "cancel\n"; }
                PublishStatus( m_cancelBuilderOnStop.load( std::memory_order_acquire ) ? GpuAnalysisControllerState::Cancelled : GpuAnalysisControllerState::Idle,
                    0, m_cancelBuilderOnStop.load( std::memory_order_acquire ) ? "cancelled" : "builder-continues-external" );
                return;
            }
        }
        PublishStatus( GpuAnalysisControllerState::Building, 0.02f, "starting-external-builder", sidecarError );
#ifdef _WIN32
        const auto builder = BuilderExecutablePath();
        if( !std::filesystem::is_regular_file( builder ) )
        { PublishStatus( GpuAnalysisControllerState::Failed, 0, "builder-missing", builder.string() ); return; }
        DWORD exitCode = 0; bool detached = false;
        const auto poll = [&]() {
            std::string manifestError;
            if( auto current = analysis::LoadGpuAnalysisSidecarManifest( sidecar, manifestError ) )
            {
                const auto stage = std::string( analysis::GpuAnalysisSidecarStateName( current->state ) );
                const float progress = current->derivedComplete ? 1.f : current->rawComplete ? .4f : .1f;
                PublishStatus( GpuAnalysisControllerState::Building, progress, stage, current->reason );
            }
        };
        if( !RunExternalBuilder( builder, m_tracePath, sidecar / "builder-progress.json", sidecar / "builder-cancel.request",
            stopToken, m_cancelBuilderOnStop, poll, exitCode, detached, sidecarError ) )
        { PublishStatus( GpuAnalysisControllerState::Failed, 0, "builder-launch-failed", sidecarError ); return; }
        if( detached ) { PublishStatus( GpuAnalysisControllerState::Idle, 0, "builder-continues-external" ); return; }
        if( stopToken.stop_requested() || exitCode == 130 ) { PublishStatus( GpuAnalysisControllerState::Cancelled, 0, "cancelled" ); return; }
        if( exitCode != 0 ) { PublishStatus( GpuAnalysisControllerState::Failed, 0, "builder-failed", "exit=" + std::to_string( exitCode ) ); return; }
#else
        analysis::GpuAnalysisSidecarControl control; control.stopToken = stopToken;
        control.progress = [this]( float value, const char* stage ) { PublishStatus( GpuAnalysisControllerState::Building, value, stage ); };
        if( !analysis::BuildGpuAnalysisDerived( m_tracePath, control, sidecarError ) )
        { PublishStatus( GpuAnalysisControllerState::Failed, 0, "builder-failed", sidecarError ); return; }
#endif
        auto reader = analysis::GpuAnalysisStoreReader::Open( m_tracePath, false, &sidecarManifest, sidecarError );
        if( !reader || !PublishReader( std::move( reader ), false, sidecarError ) )
        { PublishStatus( GpuAnalysisControllerState::Failed, 0, "sidecar-load-failed", sidecarError ); return; }
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

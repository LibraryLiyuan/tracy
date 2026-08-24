#include "TracyGpuAnalysisController.hpp"

#include "TracyGpuAnalysisCache.hpp"
#include "TracyHash.hpp"
#include "TracyStorage.hpp"
#include "TracyWorker.hpp"

#include <chrono>
#include <system_error>

#ifdef _WIN32
#  include <windows.h>
#endif

namespace tracy
{
namespace
{

constexpr const char* GuiBuildIdentity = "N27-GUI-preview-a1-c1";

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
            m_status.state == GpuAnalysisControllerState::Partial ) return;
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
            const auto directory = identity.traceSha256 + "-a1-c1-" + GuiBuildIdentity;
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
        auto built = analysis::BuildGpuAnalysisSnapshot( m_worker.GetJnTraceData(), nullptr, {}, control );
        if( built.manifest.state == analysis::GpuAnalysisState::Cancelled || stopToken.stop_requested() )
        { PublishStatus( GpuAnalysisControllerState::Cancelled, 0, "cancelled" ); return; }
        auto value = std::make_shared<analysis::GpuAnalysisSnapshot>( std::move( built ) );
        const auto finalState = value->manifest.state == analysis::GpuAnalysisState::Complete ? GpuAnalysisControllerState::Ready :
            value->manifest.state == analysis::GpuAnalysisState::Partial || value->manifest.state == analysis::GpuAnalysisState::ResourceLimit ?
            GpuAnalysisControllerState::Partial : GpuAnalysisControllerState::Failed;
        if( !m_cachePath.empty() && finalState != GpuAnalysisControllerState::Failed )
        {
            std::string cacheError; analysis::SaveGpuAnalysisCache( m_cachePath, identity, *value, cacheError );
            if( !cacheError.empty() ) { std::lock_guard lock( m_mutex ); m_status.error = "cache:" + cacheError; }
        }
        std::lock_guard lock( m_mutex );
        m_snapshot = std::move( value ); m_status.state = finalState; m_status.progress = 1; m_status.stage = finalState == GpuAnalysisControllerState::Ready ? "ready" : "partial";
        m_status.estimatedBytes = EstimateSnapshotBytes( *m_snapshot );
        if( finalState == GpuAnalysisControllerState::Failed ) m_status.error = m_snapshot->manifest.reason;
    }
    catch( const std::exception& e ) { PublishStatus( GpuAnalysisControllerState::Failed, 0, "failed", e.what() ); }
}

const char* GpuAnalysisControllerStateName( GpuAnalysisControllerState value )
{
    switch( value )
    {
    case GpuAnalysisControllerState::Idle: return "idle"; case GpuAnalysisControllerState::Building: return "building";
    case GpuAnalysisControllerState::Ready: return "ready"; case GpuAnalysisControllerState::Partial: return "partial";
    case GpuAnalysisControllerState::Cancelled: return "cancelled"; case GpuAnalysisControllerState::Failed: return "failed";
    }
    return "unknown";
}

}

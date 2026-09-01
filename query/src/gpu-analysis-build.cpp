#include "TracyGpuAnalysisSidecar.hpp"
#include "TracyGpuAnalysisStore.hpp"
#include "TracyHash.hpp"
#include "TracyTraceSessionGpuCanonical.hpp"
#include "TracyTraceSessionStore.hpp"
#include "TracyWorkerTraceSource.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>

#ifdef _WIN32
#  include <Windows.h>
#endif

namespace
{

std::atomic<bool> Cancelled { false };

#ifdef _WIN32
BOOL WINAPI ControlHandler( DWORD type )
{
    if( type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT && type != CTRL_CLOSE_EVENT ) return FALSE;
    Cancelled.store( true, std::memory_order_release ); return TRUE;
}
#endif

bool AtomicProgress( const std::filesystem::path& path, float progress, const char* stage, const char* state )
{
    if( path.empty() ) return true;
    auto temporary = path; temporary += ".tmp";
    {
        std::ofstream out( temporary, std::ios::binary | std::ios::trunc );
        if( !out ) return false;
        out << "{\"schema\":1,\"state\":\"" << state << "\",\"stage\":\"" << stage
            << "\",\"progress\":" << progress << "}\n";
    }
#ifdef _WIN32
    for( unsigned attempt = 0; attempt < 8; ++attempt )
    {
        if( MoveFileExW( temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) != 0 ) return true;
        const auto error = GetLastError();
        if( error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION &&
            error != ERROR_UNABLE_TO_REMOVE_REPLACED && error != ERROR_UNABLE_TO_MOVE_REPLACEMENT &&
            error != ERROR_UNABLE_TO_MOVE_REPLACEMENT_2 ) return false;
        Sleep( 1u << attempt );
    }
    return false;
#else
    std::error_code ec; std::filesystem::rename( temporary, path, ec ); return !ec;
#endif
}

void WaitForSystemPressure( const std::filesystem::path& trace, const std::filesystem::path& pausePath,
    const std::filesystem::path& progressPath, uint64_t minimumFreeBytes, std::stop_source& stop )
{
    while( !Cancelled.load( std::memory_order_acquire ) )
    {
        bool memoryPressure = false;
#ifdef _WIN32
        MEMORYSTATUSEX status {}; status.dwLength = sizeof( status );
        memoryPressure = GlobalMemoryStatusEx( &status ) &&
            ( status.dwMemoryLoad >= 90 || status.ullAvailPhys < 2ull * 1024 * 1024 * 1024 );
#endif
        std::error_code ec; const auto space = std::filesystem::space( trace.parent_path(), ec );
        const bool diskPressure = !ec && space.available < minimumFreeBytes;
        const bool manuallyPaused = !pausePath.empty() && std::filesystem::exists( pausePath );
        if( !memoryPressure && !diskPressure && !manuallyPaused ) return;
        AtomicProgress( progressPath, 0, manuallyPaused ? "paused-requested" :
            memoryPressure ? "paused-memory-pressure" : "paused-disk-pressure", "paused" );
        std::this_thread::sleep_for( std::chrono::seconds( 1 ) );
    }
    stop.request_stop();
}

void Usage()
{
    std::fprintf( stderr,
        "Usage:\n"
        "  tracy-gpu-analysis-build --trace file.tracy [--progress-json path] [--cancel-file path] [--pause-file path] [--strong-identity]\n"
        "  tracy-gpu-analysis-build --session capture.jn-trace-session --resource-summaries [--progress-json path] [--cancel-file path] [--pause-file path]\n" );
}

}

int main( int argc, char** argv )
{
    std::filesystem::path trace;
    std::filesystem::path session;
    std::filesystem::path progressPath;
    std::filesystem::path cancelPath;
    std::filesystem::path pausePath;
    bool strongIdentity = false;
    bool resourceSummaries = false;
    for( int i = 1; i < argc; ++i )
    {
        const std::string_view arg = argv[i];
        if( arg == "--trace" && i + 1 < argc ) trace = std::filesystem::u8path( argv[++i] );
        else if( arg == "--session" && i + 1 < argc ) session = std::filesystem::u8path( argv[++i] );
        else if( arg == "--progress-json" && i + 1 < argc ) progressPath = std::filesystem::u8path( argv[++i] );
        else if( arg == "--cancel-file" && i + 1 < argc ) cancelPath = std::filesystem::u8path( argv[++i] );
        else if( arg == "--pause-file" && i + 1 < argc ) pausePath = std::filesystem::u8path( argv[++i] );
        else if( arg == "--strong-identity" ) strongIdentity = true;
        else if( arg == "--resource-summaries" ) resourceSummaries = true;
        else { Usage(); return 1; }
    }
    if( ( trace.empty() == session.empty() ) || ( !session.empty() && !resourceSummaries ) )
    { Usage(); return 1; }
#ifdef _WIN32
    SetConsoleCtrlHandler( ControlHandler, TRUE );
    SetPriorityClass( GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS );
    SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL );
#endif
    std::stop_source stop;
    std::jthread cancellation( [&]( std::stop_token token ) {
        while( !token.stop_requested() && !Cancelled.load( std::memory_order_acquire ) &&
            ( cancelPath.empty() || !std::filesystem::exists( cancelPath ) ) ) std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
        if( Cancelled.load( std::memory_order_acquire ) || ( !cancelPath.empty() && std::filesystem::exists( cancelPath ) ) ) stop.request_stop();
    } );

    try
    {
        if( !session.empty() )
        {
            std::string error;
            auto manifest = tracy::analysis::LoadTraceSessionManifest( session, error );
            if( !manifest || !tracy::analysis::IsTraceSessionQueryable( session, error ) )
            { std::fprintf( stderr, "Trace Session is not queryable: %s\n", error.c_str() ); return 7; }
            tracy::analysis::GpuAnalysisSidecarControl control;
            control.stopToken = stop.get_token();
            control.maximumSidecarBytes = 256ull * 1024 * 1024 * 1024;
            std::error_code spaceError;
            const auto space = std::filesystem::space( session.parent_path(), spaceError );
            if( spaceError || space.available < control.minimumFreeBytes )
            {
                std::fprintf( stderr, "GPU summary generation requires at least %llu free bytes; available=%llu.\n",
                    static_cast<unsigned long long>( control.minimumFreeBytes ),
                    static_cast<unsigned long long>( spaceError ? 0 : space.available ) );
                return 8;
            }
            control.progress = [&]( float value, const char* stage ) {
                WaitForSystemPressure( session, pausePath, progressPath,
                    control.minimumFreeBytes, stop );
                AtomicProgress( progressPath, value, stage, "building" );
            };
            std::string generation; uint64_t logicalBytes = 0;
            if( !tracy::analysis::BuildGpuAnalysisResourceSummariesAt(
                tracy::analysis::TraceSessionGpuAnalysisRoot( session, *manifest ),
                manifest->source.sha256, manifest->source.fileSize, control,
                generation, logicalBytes, error ) )
            {
                AtomicProgress( progressPath, 0, error.c_str(),
                    error == "cancelled" ? "cancelled" : "failed" );
                std::fprintf( stderr, "GPU Resource Summary build failed: %s\n", error.c_str() );
                return error == "cancelled" ? 130 : 9;
            }
            AtomicProgress( progressPath, 1, "complete", "ready" );
            std::printf( "GPU Resource Summary generation is ready: %s (%llu logical bytes)\n",
                generation.c_str(), static_cast<unsigned long long>( logicalBytes ) );
            return 0;
        }
        auto sidecar = tracy::analysis::GpuAnalysisSidecarPath( trace );
        std::string error;
        auto manifest = tracy::analysis::LoadGpuAnalysisSidecarManifest( sidecar, error );
        if( !manifest )
        {
            AtomicProgress( progressPath, 0, "backfill-trace", "building" );
            auto source = tracy::analysis::WorkerTraceSource::Open( trace );
            const auto data = source->GetGpuCatalogData();
            if( !data ) { std::fprintf( stderr, "GPU Catalog is not present in this trace.\n" ); return 2; }
            auto identity = tracy::analysis::ComputeGpuAnalysisQuickIdentity( trace );
            identity.sha256 = tracy::analysis::Sha256File( trace );
            auto staging = sidecar; staging += ".building";
            std::error_code ec; std::filesystem::remove_all( staging, ec );
            tracy::analysis::GpuAnalysisSidecarControl rawControl;
            rawControl.stopToken = stop.get_token();
            rawControl.progress = [&]( float value, const char* stage ) { AtomicProgress( progressPath, value * .35f, stage, "building" ); };
            if( !tracy::analysis::WriteGpuAnalysisRawSidecar( staging, identity, *data, rawControl, error ) ||
                !tracy::analysis::PublishGpuAnalysisSidecar( staging, sidecar, true, error ) )
            { std::fprintf( stderr, "GPU sidecar backfill failed: %s\n", error.c_str() ); return error == "cancelled" ? 130 : 3; }
        }
        if( strongIdentity )
        {
            manifest = tracy::analysis::LoadGpuAnalysisSidecarManifest( sidecar, error );
            std::string reason;
            if( !manifest || tracy::analysis::VerifyGpuAnalysisIdentity( trace, manifest->identity, true, reason ) == tracy::analysis::GpuAnalysisIdentityState::Mismatch )
            { std::fprintf( stderr, "GPU sidecar identity failed: %s\n", reason.c_str() ); return 4; }
        }
        tracy::analysis::GpuAnalysisSidecarControl control;
        control.stopToken = stop.get_token();
        WaitForSystemPressure( trace, pausePath, progressPath, control.minimumFreeBytes, stop );
        control.progress = [&]( float value, const char* stage ) {
            WaitForSystemPressure( trace, pausePath, progressPath, control.minimumFreeBytes, stop );
            AtomicProgress( progressPath, .35f + value * .65f, stage, "building" );
        };
        if( !tracy::analysis::BuildGpuAnalysisDerived( trace, control, error ) )
        {
            AtomicProgress( progressPath, 0, error.c_str(), error == "cancelled" ? "cancelled" : "failed" );
            std::fprintf( stderr, "GPU analysis build failed: %s\n", error.c_str() ); return error == "cancelled" ? 130 : 5;
        }
        AtomicProgress( progressPath, 1, "complete", "ready" );
        std::printf( "GPU Resource Analysis sidecar is ready: %s\n", sidecar.string().c_str() );
        return 0;
    }
    catch( const std::exception& e )
    {
        AtomicProgress( progressPath, 0, e.what(), "failed" ); std::fprintf( stderr, "%s\n", e.what() ); return 6;
    }
}

#include "TracyTraceSessionCanonical.hpp"
#include "TracyTraceSessionDerived.hpp"
#include "TracyHash.hpp"
#include "TracyTraceSessionInventory.hpp"
#include "TracyTraceSessionStore.hpp"
#include "TracyProtocol.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  include <Windows.h>
#else
#  include <unistd.h>
#endif

namespace
{

struct Options
{
    std::filesystem::path input;
    std::filesystem::path output;
    bool resume = true;
    bool restartGeneration = false;
    bool status = false;
};

std::atomic<uint32_t> CancelRequests { 0 };

#ifdef _WIN32
BOOL WINAPI ConsoleHandler( DWORD type )
{
    if( type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT && type != CTRL_CLOSE_EVENT ) return FALSE;
    CancelRequests.fetch_add( 1, std::memory_order_relaxed );
    return TRUE;
}
#endif

bool Cancelled( void* )
{
    return CancelRequests.load( std::memory_order_relaxed ) != 0;
}

void Usage()
{
    std::fprintf( stderr,
        "Usage: tracy-stream-convert -i <capture.tracy-stream> [-o <capture.jn-trace-session>]\n"
        "       [--resume] [--restart-generation] [--status]\n"
        "The default and only production output is a Trace Session Store.\n" );
}

bool ParseArguments( int argc, char** argv, Options& options )
{
    for( int i = 1; i < argc; ++i )
    {
        const std::string argument = argv[i];
        if( argument == "-i" && i + 1 < argc ) options.input = std::filesystem::u8path( argv[++i] );
        else if( argument == "-o" && i + 1 < argc ) options.output = std::filesystem::u8path( argv[++i] );
        else if( argument == "--resume" ) options.resume = true;
        else if( argument == "--restart-generation" ) { options.restartGeneration = true; options.resume = false; }
        else if( argument == "--status" ) options.status = true;
        else if( argument == "-h" || argument == "--help" ) { Usage(); std::exit( 0 ); }
        else return false;
    }
    return !options.input.empty();
}

std::string GenerationName()
{
    const auto ticks = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch() ).count();
#ifdef _WIN32
    const auto pid = GetCurrentProcessId();
#else
    const auto pid = getpid();
#endif
    return "n30-" + std::to_string( ticks ) + "-" + std::to_string( pid );
}

void PrintInventoryProgress( tracy::analysis::TraceSessionInventoryPhase phase,
    uint64_t completed, uint64_t total, void* )
{
    static auto last = std::chrono::steady_clock::now() - std::chrono::seconds( 10 );
    const auto now = std::chrono::steady_clock::now();
    if( completed != total && now - last < std::chrono::seconds( 5 ) ) return;
    last = now;
    const auto percent = total == 0 ? 100.0 : 100.0 * double( completed ) / double( total );
    std::fprintf( stderr, "[Inventory.%s] %.1f%% (%llu/%llu bytes)\n",
        phase == tracy::analysis::TraceSessionInventoryPhase::JournalScan ? "Scan" : "Hash",
        percent, static_cast<unsigned long long>( completed ),
        static_cast<unsigned long long>( total ) );
    std::fflush( stderr );
}

void PrintDerivedProgress( float value, const char* stage )
{
    static auto last = std::chrono::steady_clock::now() - std::chrono::seconds( 10 );
    static std::string lastStage;
    const auto now = std::chrono::steady_clock::now();
    const std::string currentStage = stage ? stage : "Build";
    if( currentStage == lastStage && value < 1.f && now - last < std::chrono::seconds( 5 ) ) return;
    last = now;
    lastStage = currentStage;
    std::fprintf( stderr, "[Derived.%s] %.1f%%\n", stage ? stage : "Build", value * 100.f );
    std::fflush( stderr );
}

void PrintCanonicalProgress( uint64_t completed, uint64_t total,
    uint64_t records, uint64_t shards, void* )
{
    const auto percent = total == 0 ? 100.0 : 100.0 * double( completed ) / double( total );
    std::fprintf( stderr, "[Canonical] %.1f%% (%llu/%llu bytes, record=%llu, shards=%llu)\n",
        percent, static_cast<unsigned long long>( completed ),
        static_cast<unsigned long long>( total ), static_cast<unsigned long long>( records ),
        static_cast<unsigned long long>( shards ) );
    std::fflush( stderr );
}

std::optional<std::filesystem::path> FindBuilding( const std::filesystem::path& finalPath,
    const std::optional<std::filesystem::path>& sourcePath = std::nullopt )
{
    const auto parent = finalPath.has_parent_path() ? finalPath.parent_path() : std::filesystem::current_path();
    const auto prefix = finalPath.filename().string() + ".building.";
    struct Candidate
    {
        std::filesystem::path path;
        tracy::analysis::TraceSessionManifest manifest;
        uint64_t progress = 0;
        std::filesystem::file_time_type writeTime {};
    };
    std::vector<Candidate> candidates;
    std::error_code ec;
    for( const auto& entry : std::filesystem::directory_iterator( parent, ec ) )
    {
        if( !entry.is_directory() || !entry.path().filename().string().starts_with( prefix ) ) continue;
        std::string manifestError;
        const auto manifest = tracy::analysis::LoadTraceSessionManifest( entry.path(), manifestError );
        if( !manifest ) continue;
        uint64_t progress = 0;
        for( const auto& shard : manifest->shards )
            progress = std::max( progress, shard.sourceRecordEnd );
        candidates.push_back( { entry.path(), *manifest, progress,
            std::filesystem::last_write_time( entry.path() / "manifest", ec ) } );
        ec.clear();
    }
    if( candidates.empty() ) return std::nullopt;
    if( sourcePath )
    {
        const auto sourceBytes = std::filesystem::file_size( *sourcePath, ec );
        if( ec ) return std::nullopt;
        std::erase_if( candidates, [&]( const auto& candidate ) {
            return candidate.manifest.source.fileSize != sourceBytes;
        } );
        if( candidates.empty() ) return std::nullopt;
        if( candidates.size() > 1 )
        {
            const auto sourceSha256 = tracy::analysis::Sha256File( *sourcePath );
            std::erase_if( candidates, [&]( const auto& candidate ) {
                return candidate.manifest.source.sha256 != sourceSha256;
            } );
            if( candidates.empty() ) return std::nullopt;
        }
    }
    std::erase_if( candidates, []( const auto& candidate ) {
        std::string compatibilityError;
        return !tracy::analysis::CanResumeTraceSessionCanonical(
            candidate.path, candidate.manifest, compatibilityError );
    } );
    if( candidates.empty() ) return std::nullopt;
    std::sort( candidates.begin(), candidates.end(), []( const auto& left, const auto& right ) {
        if( left.progress != right.progress ) return left.progress > right.progress;
        if( left.writeTime != right.writeTime ) return left.writeTime > right.writeTime;
        return left.path.filename().string() > right.path.filename().string();
    } );
    return candidates.front().path;
}

int PrintStatus( const std::filesystem::path& finalPath )
{
    std::string error;
    auto root = finalPath;
    if( !std::filesystem::exists( root ) )
    {
        const auto building = FindBuilding( finalPath );
        if( !building ) { std::fprintf( stderr, "Session not found.\n" ); return 2; }
        root = *building;
    }
    const auto manifest = tracy::analysis::LoadTraceSessionManifest( root, error );
    if( !manifest ) { std::fprintf( stderr, "Status failed: %s\n", error.c_str() ); return 2; }
    uint64_t sourceRecord = 0;
    int64_t timeEndNs = 0;
    for( const auto& shard : manifest->shards )
    {
        sourceRecord = std::max( sourceRecord, shard.sourceRecordEnd );
        timeEndNs = std::max( timeEndNs, shard.timeEndNs );
    }
    std::printf( "state=%s generation=%s shards=%zu source_record=%llu time_end_ns=%lld mandatory_derived=%s audit=%s reason=%s root=%s\n",
        tracy::analysis::TraceSessionStateName( manifest->state ), manifest->generation.c_str(),
        manifest->shards.size(), static_cast<unsigned long long>( sourceRecord ),
        static_cast<long long>( timeEndNs ), manifest->mandatoryDerivedComplete ? "true" : "false",
        manifest->auditComplete ? "true" : "false", manifest->reason.c_str(), root.string().c_str() );
    return 0;
}

bool MoveInventoryRuns( const std::filesystem::path& source,
    const std::filesystem::path& target, std::string& error )
{
    std::error_code ec;
    std::filesystem::create_directories( target.parent_path(), ec );
    if( ec ) { error = "inventory_directory_failed:" + ec.message(); return false; }
    std::filesystem::rename( source, target, ec );
    if( !ec ) return true;
    error = "inventory_run_publish_failed:" + ec.message();
    return false;
}

}

int main( int argc, char** argv )
{
    Options options;
    if( !ParseArguments( argc, argv, options ) ) { Usage(); return 1; }
    if( options.output.empty() ) options.output = tracy::analysis::DefaultTraceSessionPath( options.input );
#ifdef _WIN32
    SetConsoleCtrlHandler( ConsoleHandler, TRUE );
    SetPriorityClass( GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS );
#endif
    if( options.status ) return PrintStatus( options.output );
    if( options.output.extension() == ".tracy" )
    {
        std::fprintf( stderr, "-o selects a Session directory, not a .tracy file. Use tracy-session-export after conversion.\n" );
        return 1;
    }

    std::string error;
    tracy::analysis::TraceSessionInventory inventory;
    std::filesystem::path building;
    std::string generation;
    if( options.resume && !options.restartGeneration )
    {
        const auto candidate = FindBuilding( options.output, options.input );
        if( candidate )
        {
            const auto saved = tracy::analysis::LoadTraceSessionManifest( *candidate, error );
            if( saved ) { building = *candidate; generation = saved->generation; }
        }
    }

    if( !building.empty() )
    {
        const auto loaded = tracy::analysis::LoadTraceSessionInventory( building / "inventory" / "manifest", error );
        if( !loaded || !tracy::analysis::VerifyTraceSessionInventoryRuns(
            building / "inventory" / "runs", *loaded, error ) )
        { std::fprintf( stderr, "Cannot resume Inventory: %s\n", error.c_str() ); return 2; }
        inventory = *loaded;
        if( !tracy::analysis::VerifyTraceSessionSourceIdentity( options.input,
            { inventory.sourceSha256, inventory.sourceFileSize }, error ) )
        { std::fprintf( stderr, "Cannot resume source: %s\n", error.c_str() ); return 2; }
        std::fprintf( stderr, "Resuming generation %s.\n", generation.c_str() );
    }
    else
    {
        auto temporaryRuns = options.output; temporaryRuns += ".inventory-building";
        std::error_code cleanupError; std::filesystem::remove_all( temporaryRuns, cleanupError );
        tracy::analysis::TraceSessionInventoryOptions inventoryOptions;
        inventoryOptions.runDirectory = temporaryRuns;
        inventoryOptions.progress = PrintInventoryProgress;
        inventoryOptions.shouldCancel = Cancelled;
        if( !tracy::analysis::BuildTraceSessionInventory( options.input, inventoryOptions, inventory, error ) )
        {
            if( error == "cancelled_safe_restart" )
            {
                std::fprintf( stderr, "Inventory cancelled safely at a committed input boundary; rerun to restart Inventory.\n" );
                return 130;
            }
            std::fprintf( stderr, "Inventory failed: %s\n", error.c_str() ); return 2;
        }
        if( inventory.protocol != tracy::ProtocolVersion )
        { std::fprintf( stderr, "Protocol mismatch: source=%u converter=%u.\n", inventory.protocol, tracy::ProtocolVersion ); return 2; }
        generation = GenerationName();
        building = tracy::analysis::BuildingTraceSessionPath( options.output, generation );
        if( std::filesystem::exists( building ) )
        { std::fprintf( stderr, "Generated building path already exists.\n" ); return 2; }
        if( !MoveInventoryRuns( temporaryRuns, building / "inventory" / "runs", error ) ||
            !tracy::analysis::SaveTraceSessionInventory( building / "inventory" / "manifest", inventory, error ) )
        { std::fprintf( stderr, "Inventory publish failed: %s\n", error.c_str() ); return 2; }
    }

    std::error_code spaceError;
    const auto volume = std::filesystem::space(
        options.output.has_parent_path() ? options.output.parent_path() : std::filesystem::current_path(), spaceError );
    tracy::analysis::TraceSessionCapacityResult capacity;
    if( spaceError || !tracy::analysis::EvaluateTraceSessionCapacity( inventory,
        volume.capacity, volume.available, {}, capacity ) )
    {
        std::fprintf( stderr, "Capacity preflight failed: %s (required=%llu available=%llu).\n",
            spaceError ? spaceError.message().c_str() : capacity.reason.c_str(),
            static_cast<unsigned long long>( capacity.requiredAvailableBytes ),
            static_cast<unsigned long long>( spaceError ? 0 : volume.available ) );
        return 3;
    }

    tracy::analysis::TraceSessionManifest manifest;
    bool reusedCanonical = false;
    if( options.resume )
    {
        const auto saved = tracy::analysis::LoadTraceSessionManifest( building, error );
        if( saved )
        {
            std::string reuseError;
            if( tracy::analysis::CanReuseCompletedTraceSessionCanonical(
                building, *saved, inventory, reuseError ) )
            {
                manifest = *saved;
                reusedCanonical = true;
                std::fprintf( stderr, "Canonical generation is complete and verified; continuing with Derived.\n" );
            }
            else
            {
                const bool derivedState = saved->state == tracy::analysis::TraceSessionState::DerivedBuilding ||
                    saved->state == tracy::analysis::TraceSessionState::DerivedFailed ||
                    saved->state == tracy::analysis::TraceSessionState::FinalAuditing ||
                    saved->state == tracy::analysis::TraceSessionState::InvalidConverterOutput;
                if( derivedState )
                {
                    std::fprintf( stderr, "Completed Canonical reuse failed: %s\n", reuseError.c_str() );
                    return 4;
                }
            }
        }
    }
    tracy::analysis::TraceSessionCanonicalBuildResult canonical =
        tracy::analysis::TraceSessionCanonicalBuildResult::Complete;
    if( !reusedCanonical )
    {
        tracy::analysis::TraceSessionCanonicalOptions canonicalOptions;
        canonicalOptions.resume = options.resume;
        canonicalOptions.shouldCancel = Cancelled;
        canonicalOptions.progress = PrintCanonicalProgress;
        canonical = tracy::analysis::BuildTraceSessionCanonical( options.input,
            building, generation, inventory, canonicalOptions, manifest, error );
    }
    if( canonical == tracy::analysis::TraceSessionCanonicalBuildResult::CancelledResumable )
    {
        std::fprintf( stderr, "Conversion cancelled safely; rerun the same command to resume.\n" );
        return 130;
    }
    if( canonical != tracy::analysis::TraceSessionCanonicalBuildResult::Complete )
    { std::fprintf( stderr, "Canonical build failed: %s\n", error.c_str() ); return 4; }
    if( CancelRequests.load( std::memory_order_relaxed ) != 0 )
    { std::fprintf( stderr, "Cancellation will be checkpointed before Derived.\n" ); return 130; }

    manifest.state = tracy::analysis::TraceSessionState::DerivedBuilding;
    manifest.reason.clear();
    if( !tracy::analysis::SaveTraceSessionManifest( building, manifest, error ) )
    { std::fprintf( stderr, "Derived state save failed: %s\n", error.c_str() ); return 5; }
    tracy::analysis::TraceSessionDerivedControl derivedControl;
    std::stop_source derivedStop;
    std::atomic<bool> derivedFinished { false };
    std::jthread derivedCancelMonitor( [&]( std::stop_token stop ) {
        auto nextHeartbeat = std::chrono::steady_clock::now() + std::chrono::seconds( 30 );
        while( !stop.stop_requested() && !derivedFinished.load( std::memory_order_acquire ) )
        {
            if( CancelRequests.load( std::memory_order_relaxed ) != 0 )
            {
                derivedStop.request_stop();
                return;
            }
            if( std::chrono::steady_clock::now() >= nextHeartbeat )
            {
                std::fprintf( stderr, "[Derived] active; use --status for the committed Session state.\n" );
                std::fflush( stderr );
                nextHeartbeat = std::chrono::steady_clock::now() + std::chrono::seconds( 30 );
            }
            std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
        }
    } );
    derivedControl.stopToken = derivedStop.get_token();
    derivedControl.progress = [&]( float value, const char* stage ) {
        if( CancelRequests.load( std::memory_order_relaxed ) != 0 ) derivedStop.request_stop();
        PrintDerivedProgress( value, stage );
    };
    tracy::analysis::TraceSessionDerivedStats derivedStats;
    const auto derivedOk = tracy::analysis::BuildTraceSessionMandatoryDerived(
        building, manifest, inventory, derivedControl, derivedStats, error );
    derivedFinished.store( true, std::memory_order_release );
    derivedCancelMonitor.request_stop();
    if( !derivedOk )
    {
        manifest.state = error == "cancelled_resumable" ?
            tracy::analysis::TraceSessionState::CancelledResumable : tracy::analysis::TraceSessionState::DerivedFailed;
        manifest.reason = error;
        std::string ignored; tracy::analysis::SaveTraceSessionManifest( building, manifest, ignored );
        std::fprintf( stderr, "Mandatory Derived failed: %s\n", error.c_str() );
        return error == "cancelled_resumable" ? 130 : 5;
    }

    tracy::analysis::TraceSessionDerivedStats audited;
    if( !tracy::analysis::AuditTraceSessionFinal( building, manifest, inventory, audited, error ) )
    {
        manifest.state = tracy::analysis::TraceSessionState::InvalidConverterOutput;
        manifest.reason = error;
        std::string ignored; tracy::analysis::SaveTraceSessionManifest( building, manifest, ignored );
        std::fprintf( stderr, "Final Audit failed: %s\n", error.c_str() );
        return 6;
    }
    manifest.auditComplete = true;
    manifest.mandatoryDerivedComplete = true;
    const auto derivedSourceDegraded = audited.invalidCpuZoneTimings != 0 ||
        audited.schedulingSourceGaps != 0 || audited.gpuSourceGapResources != 0;
    manifest.state = inventory.sourceDegraded || derivedSourceDegraded ?
        tracy::analysis::TraceSessionState::CompleteSourceDegraded :
        tracy::analysis::TraceSessionState::Complete;
    if( inventory.sourceDegraded || derivedSourceDegraded )
    {
        std::string reason = inventory.sourceDegraded ? inventory.qualityReason : std::string();
        if( audited.invalidCpuZoneTimings != 0 )
        {
            if( !reason.empty() ) reason += ';';
            reason += "source_cpu_zone_clock_inversion:" + std::to_string( audited.invalidCpuZoneTimings );
        }
        if( audited.schedulingSourceGaps != 0 )
        {
            if( !reason.empty() ) reason += ';';
            reason += "source_scheduling_gap:" + std::to_string( audited.schedulingSourceGaps );
        }
        if( audited.gpuSourceGapResources != 0 )
        {
            if( !reason.empty() ) reason += ';';
            reason += "source_gpu_resource_identity_gap:" +
                std::to_string( audited.gpuSourceGapResources );
        }
        manifest.reason = std::move( reason );
    }
    else manifest.reason = "complete";
    if( !tracy::analysis::PublishTraceSession( building, options.output, manifest, error ) )
    { std::fprintf( stderr, "Session publish failed: %s\n", error.c_str() ); return 7; }
    std::printf( "Session complete: %s\n", options.output.string().c_str() );
    std::printf( "generation=%s canonical_shards=%zu indexed_records=%llu index_bytes=%llu gpu_resources=%llu gpu_passes=%llu\n",
        generation.c_str(), manifest.shards.size(),
        static_cast<unsigned long long>( audited.indexedRecords ),
        static_cast<unsigned long long>( audited.indexBytes ),
        static_cast<unsigned long long>( audited.gpuResources ),
        static_cast<unsigned long long>( audited.gpuPasses ) );
    return 0;
}

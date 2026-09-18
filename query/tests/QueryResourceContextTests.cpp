#include "TracyQueryResourceContext.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>

using namespace tracy::analysis;
using namespace tracy::query;

namespace
{
void Require( bool value, const char* message )
{
    if( !value ) throw std::runtime_error( message );
}

template<typename Function>
void Reject( Function&& function, const char* reason )
{
    try { function(); }
    catch( const std::exception& error )
    {
        if( std::string( error.what() ).find( reason ) != std::string::npos ) return;
        throw;
    }
    throw std::runtime_error( std::string( "expected rejection: " ) + reason );
}

void SharedBudgetsAndDiagnostics( const std::filesystem::path& root )
{
    std::filesystem::create_directories( root );
    std::ofstream( root / "existing.bin", std::ios::binary ).write( "01234567890123456789", 20 );

    AnalysisProcessMemorySample process { true, 200, 70 };
    uint64_t pressureCallbacks = 0;
    QueryResourceContextOptions options;
    options.traceBytes = 10;
    options.derivedRoots = { root };
    options.privateBytesSoftTarget = 80;
    options.processCommitHardLimit = 100;
    options.workspaceMaximumBytes = 64;
    options.workspaceTargetBytes = 32;
    options.diagnosticDiskReserveBytes = 8;
    options.memorySample = [&] { return process; };
    options.relieveMemoryPressure = [&] { ++pressureCallbacks; };

    QueryResourceContext context( options );
    context.SetPhase( "catalog-runs" );
    context.SetProgress( 7, 11 );
    context.SetLiveWorkers( 2 );

    // A large mapped working set remains telemetry when Private Bytes is below
    // the soft target.
    context.CheckMemory();
    Require( pressureCallbacks == 0, "working set incorrectly triggered private-memory pressure" );

    process = { true, 300, 81 };
    context.CheckMemory();
    context.CheckMemory();
    Require( pressureCallbacks == 1, "one soft-target crossing must trigger one pressure response" );
    process.privateBytes = 70;
    context.CheckMemory();
    process.privateBytes = 90;
    context.CheckMemory();
    Require( pressureCallbacks == 2, "falling below the target must re-arm the pressure response" );

    {
        AnalysisWorkspaceReservation first( context.Workspace(), 40 );
        Reject( [&] { AnalysisWorkspaceReservation denied( context.Workspace(), 25 ); }, "workspace_budget" );
    }
    Require( context.Workspace()->Snapshot().currentBytes == 0, "workspace reservations leaked" );

    context.Disk()->Grow( 131 ); // 20 existing + 131 live + 8 diagnostic reserve = 159 of 160.
    Reject( [&] { context.Disk()->Grow( 2 ); }, "disk_budget" );

    auto snapshot = context.Snapshot();
    Require( snapshot.phase == "catalog-runs" && snapshot.progressCurrent == 7 && snapshot.progressTotal == 11,
        "phase progress was not preserved" );
    Require( snapshot.liveWorkers == 2, "live Worker count was not preserved" );
    Require( snapshot.memory.peakResidentBytes == 300 && snapshot.memory.peakPrivateBytes == 90,
        "memory telemetry lost a peak" );
    Require( snapshot.disk.currentBytes == 151 && snapshot.disk.peakBytes == 151,
        "disk ledger did not include existing and admitted live bytes" );
    Require( snapshot.disk.rejectedGrowths == 1 && snapshot.failureReason == "disk_peak_limit" &&
        snapshot.requestedBytesKnown && snapshot.requestedBytes == 2,
        "write-before-growth denial did not leave an actionable diagnostic" );


    const auto finalRoot = root / "final";
    std::filesystem::create_directories( finalRoot );
    std::ofstream( finalRoot / "existing.bin", std::ios::binary ).write(
        "0123456789012345678901234567890123456789012345678901234567890123456789", 70 );
    options.derivedRoots = { finalRoot };
    options.diagnosticDiskReserveBytes = 0;
    QueryResourceContext finalContext( options );
    finalContext.Disk()->Grow( 10 );
    finalContext.CheckFinalDisk();
    finalContext.Disk()->Grow( 1 );
    Reject( [&] { finalContext.CheckFinalDisk(); }, "final_disk_ratio" );
}

void AllocationFailureMapping( const std::filesystem::path& root )
{
    AnalysisProcessMemorySample process { true, 1, 1 };
    QueryResourceContextOptions options;
    options.traceBytes = 1024;
    options.derivedRoots = { root };
    options.privateBytesSoftTarget = 80;
    options.processCommitHardLimit = 100;
    options.workspaceMaximumBytes = 64;
    options.workspaceTargetBytes = 32;
    options.diagnosticDiskReserveBytes = 0;
    options.memorySample = [&] { return process; };
    QueryResourceContext context( options );
    context.SetPhase( "compact-open" );

    bool mapped = false;
    try
    {
        context.RunResourceBoundary( [] { throw std::bad_alloc(); } );
    }
    catch( const QueryResourceError& error )
    {
        mapped = error.Code() == "RESOURCE_LIMIT" && error.Reason() == "allocation_failed" &&
            !error.RequestedBytes().has_value();
    }
    Require( mapped, "bad_alloc was not converted to a resource error with unknown request size" );
    const auto snapshot = context.Snapshot();
    Require( snapshot.phase == "compact-open" && snapshot.failureReason == "allocation_failed" &&
        !snapshot.requestedBytesKnown, "allocation diagnostic invented a requested byte count" );

    QueryResourceContext hardLimit( options );
    hardLimit.SetPhase( "sample-read" );
    process = { true, 1, 101 };
    Reject( [&] { hardLimit.CheckMemory(); }, "query_resource_memory" );
    Require( hardLimit.Snapshot().failureReason == "process_private_hard_limit",
        "hard private-memory rejection did not preserve its cause" );
}
}

int main( int argc, char** argv )
{
    try
    {
        const auto base = argc > 1 ? std::filesystem::path( argv[1] ) :
            std::filesystem::current_path() / "query-resource-context-test";
        std::error_code ignored;
        std::filesystem::remove_all( base, ignored );
        SharedBudgetsAndDiagnostics( base / "shared" );
        AllocationFailureMapping( base / "allocation" );
        std::filesystem::remove_all( base, ignored );
        std::cout << "query resource context tests passed\n";
        return 0;
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

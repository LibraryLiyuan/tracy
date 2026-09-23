#include "TracyQueryResourceContext.hpp"
#include "TracyQueryIndex.hpp"
#include "TracyFileWrite.hpp"

#include <nlohmann/json.hpp>

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

    AnalysisProcessMemorySample process { true, 200, 70, true, 1000, 600, 400 };
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

    process = { true, 300, 81, true, 1000, 650, 350 };
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
    Require( snapshot.memory.systemCommitAvailable && snapshot.memory.systemCommitLimitBytes == 1000 &&
        snapshot.memory.peakSystemCommitBytes == 650 && snapshot.memory.minimumSystemCommitHeadroomBytes == 350,
        "system commit telemetry was not preserved independently of process memory" );
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

void FileWriteAccounting( const std::filesystem::path& root )
{
    QueryResourceContextOptions options;
    options.traceBytes = 1024;
    options.derivedRoots = { root };
    options.privateBytesSoftTarget = 80;
    options.processCommitHardLimit = 100;
    options.workspaceMaximumBytes = 64 * 1024 * 1024;
    options.workspaceTargetBytes = 32 * 1024 * 1024;
    options.diagnosticDiskReserveBytes = 0;
    options.memorySample = [] { return AnalysisProcessMemorySample { true, 1, 1 }; };
    QueryResourceContext context( options );
    context.SetPhase( "compact-write" );
    std::filesystem::create_directories( root );
    const auto outputPath = root / "candidate.tracy";
    std::vector<uint8_t> bytes( 64 * 1024 );
    uint32_t value = 0x12345678;
    for( auto& byte : bytes ) { value ^= value << 13; value ^= value >> 17; value ^= value << 5; byte = uint8_t( value ); }

    bool rejected = false;
    try
    {
        auto output = std::unique_ptr<tracy::FileWrite>( tracy::FileWrite::Open(
            outputPath, tracy::FileCompression::Fast, 1, 1, context.CreateFileWriteHooks( outputPath ) ) );
        Require( output != nullptr, "test FileWrite did not open" );
        output->Write( bytes.data(), bytes.size() );
        output->Finish();
    }
    catch( const QueryResourceError& error )
    {
        rejected = error.Code() == "RESOURCE_LIMIT" && error.Reason() == "disk_peak_limit";
    }
    Require( rejected, "compressed snapshot bypassed cumulative write accounting" );
    const auto snapshot = context.Snapshot();
    Require( snapshot.failureReason == "disk_peak_limit" && snapshot.disk.currentBytes <= snapshot.disk.peakLimitBytes,
        "FileWrite rejection lost its disk-limit diagnostic" );
}

void ReservedFailureDiagnostic( const std::filesystem::path& root )
{
    QueryResourceContextOptions options;
    options.traceBytes = 1024;
    options.derivedRoots = { root };
    options.privateBytesSoftTarget = 80;
    options.processCommitHardLimit = 100;
    options.workspaceMaximumBytes = 1024 * 1024;
    options.workspaceTargetBytes = 512 * 1024;
    options.diagnosticMemoryReserveBytes = 4096;
    options.diagnosticDiskReserveBytes = 4096;
    options.memorySample = [] { return AnalysisProcessMemorySample { true, 1, 1 }; };
    QueryResourceContext context( options );
    context.SetPhase( "catalog-validate" );
    context.SetProgress( 3, 9 );
    context.SetContainer( "validation_callstacks", 5, 8 );
    try { context.RunResourceBoundary( [] { throw std::bad_alloc(); } ); }
    catch( const QueryResourceError& ) {}

    std::filesystem::create_directories( root );
    const auto report = root / "failure.json";
    context.WriteDiagnostic( report );
    std::ifstream input( report, std::ios::binary );
    const auto parsed = nlohmann::json::parse( input );
    Require( parsed.at( "phase" ) == "catalog-validate" && parsed.at( "progress_current" ) == "3" &&
        parsed.at( "failure_reason" ) == "allocation_failed" && parsed.at( "requested_bytes" ).is_null(),
        "reserved diagnostic did not preserve the failure context" );
    Require( parsed.at( "containers" ).size() == 1 &&
        parsed.at( "containers" )[0].at( "name" ) == "validation_callstacks" &&
        parsed.at( "containers" )[0].at( "capacity" ) == "8",
        "reserved diagnostic lost container size and capacity" );
    Require( context.Snapshot().workspace.currentBytes >= 4096,
        "diagnostic workspace was not reserved before the failure" );
}

void ReusedDiskRoots( const std::filesystem::path& root )
{
    QueryResourceContextOptions options;
    options.traceBytes = 10;
    options.derivedRoots = { root / "initial" };
    options.privateBytesSoftTarget = 80;
    options.processCommitHardLimit = 100;
    options.workspaceMaximumBytes = 64;
    options.workspaceTargetBytes = 32;
    options.diagnosticDiskReserveBytes = 0;
    options.memorySample = [] { return AnalysisProcessMemorySample { true, 1, 1 }; };
    QueryResourceContext context( options );
    const auto reused = root / "reused";
    std::filesystem::create_directories( reused );
    std::ofstream( reused / "cache.bin", std::ios::binary ).write( std::string( 120, 'x' ).data(), 120 );
    context.IncludeDiskRoots( { reused } );
    context.IncludeDiskRoots( { reused } );
    Require( context.Snapshot().disk.currentBytes == 120,
        "reused Query roots were omitted or counted twice" );
    const auto overflow = root / "overflow";
    std::filesystem::create_directories( overflow );
    std::ofstream( overflow / "cache.bin", std::ios::binary ).write( std::string( 60, 'x' ).data(), 60 );
    Reject( [&] { context.IncludeDiskRoots( { overflow } ); }, "query_resource_disk" );
    Require( context.Snapshot().disk.currentBytes == 120,
        "rejected reused roots changed the admitted disk ledger" );
}

void ReducedProfileDiskLimits( const std::filesystem::path& root )
{
    QueryResourceContextOptions options;
    options.traceBytes = 10;
    options.expectedTraceBytes = 10;
    options.expectedFinalDiskLimit = 40;
    options.expectedPeakDiskLimit = 80;
    options.derivedRoots = { root };
    options.diagnosticDiskReserveBytes = 0;
    options.memorySample = [] { return AnalysisProcessMemorySample { true, 1, 1 }; };

    QueryResourceContext context( options );
    context.ValidateTraceIdentity( 10, {} );
    const auto limits = context.Snapshot().disk;
    Require( limits.finalLimitBytes == 40 && limits.peakLimitBytes == 80,
        "reduced Profile limits were not applied to the shared disk ledger" );
    context.Disk()->Grow( 40 );
    context.CheckFinalDisk();
    context.Disk()->Grow( 1 );
    Reject( [&] { context.CheckFinalDisk(); }, "final_disk_ratio" );
    context.Disk()->Grow( 39 );
    Reject( [&] { context.Disk()->Grow( 1 ); }, "disk_budget" );

    QueryResourceContextOptions invalid = options;
    invalid.expectedPeakDiskLimit = 0;
    Reject( [&] { QueryResourceContext ignored( invalid ); }, "query_resource_configuration" );
    invalid = options;
    invalid.expectedFinalDiskLimit = 81;
    Reject( [&] { QueryResourceContext ignored( invalid ); }, "query_resource_configuration" );

    const auto existing = root / "existing-over-limit";
    std::filesystem::create_directories( existing );
    std::ofstream( existing / "index.bin", std::ios::binary ).write( std::string( 81, 'x' ).data(), 81 );
    invalid = options;
    invalid.derivedRoots = { existing };
    Reject( [&] { QueryResourceContext ignored( invalid ); }, "disk_peak_limit" );
}

void ReducedProfileAcceptedAtTraceEntry( const std::filesystem::path& root )
{
    std::filesystem::create_directories( root );
    const auto trace = root / "small.tracy";
    std::ofstream( trace, std::ios::binary ).write( "0123456789", 10 );
    QueryResourceContextOptions options;
    options.expectedTraceBytes = 10;
    options.expectedFinalDiskLimit = 40;
    options.expectedPeakDiskLimit = 80;
    options.diagnosticDiskReserveBytes = 0;
    options.memorySample = [] { return AnalysisProcessMemorySample { true, 1, 1 }; };

    const auto context = QueryIndex::CreateResourceContext( trace, options );
    const auto disk = context->Snapshot().disk;
    Require( disk.finalLimitBytes == 40 && disk.peakLimitBytes == 80,
        "trace entry did not retain the lower Profile limits" );
    options.expectedTraceBytes = 11;
    Reject( [&] { QueryIndex::CreateResourceContext( trace, options ); },
        "query_resource_profile_identity_mismatch" );
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
        FileWriteAccounting( base / "file-write" );
        ReservedFailureDiagnostic( base / "diagnostic" );
        ReusedDiskRoots( base / "reused-roots" );
        ReducedProfileDiskLimits( base / "reduced-profile" );
        ReducedProfileAcceptedAtTraceEntry( base / "trace-entry" );
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

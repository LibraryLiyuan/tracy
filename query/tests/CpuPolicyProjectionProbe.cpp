#include "TracyQueryIndex.hpp"
#include "../../analysis/TracyBoundedScanCursor.hpp"
#include "../../analysis/TracyGpuJobManagedScanner.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

using namespace tracy::analysis;
using namespace tracy::query;

namespace
{
uint64_t ElapsedMs( std::chrono::steady_clock::time_point begin )
{
    return uint64_t( std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin ).count() );
}

void RequireSameFacts( const CpuZoneDto& raw, const CpuZoneDto& policy )
{
    if( raw.ref != policy.ref || raw.threadRef != policy.threadRef ||
        raw.sourceLocationRef != policy.sourceLocationRef || raw.parentRef != policy.parentRef ||
        raw.function != policy.function || raw.file != policy.file || raw.line != policy.line ||
        raw.startNs != policy.startNs || raw.endNs != policy.endNs ||
        raw.childCount != policy.childCount || raw.complete != policy.complete )
        throw std::runtime_error( "cpu_policy_projection_changed_structural_fact" );
    if( policy.extraName || policy.extraText || policy.callstack != 0 || policy.callstackRef )
        throw std::runtime_error( "cpu_policy_projection_retained_optional_payload" );
}
}

int main( int argc, char** argv )
{
    try
    {
        if( argc == 4 && ( std::string_view( argv[1] ) == "--gpu" ||
            std::string_view( argv[1] ) == "--job" ) )
        {
            const auto mode = std::string_view( argv[1] );
            const auto tracePath = std::filesystem::path( argv[2] );
            const auto temporaryRoot = std::filesystem::path( argv[3] );
            auto resources = QueryIndex::CreateResourceContext( tracePath );
            resources->IncludeDiskRoots( { temporaryRoot } );
            const auto validated = QueryIndex::Validate( tracePath, false, resources );
            if( !validated.manifest ) throw std::runtime_error( validated.reason );
            auto source = QueryIndex::Open( *validated.manifest, {}, {}, resources );
            uint64_t definitions = 0, gpuFacts = 0, jobs = 0;
            const auto began = std::chrono::steady_clock::now();
            std::filesystem::create_directories( temporaryRoot );
            std::ofstream progressLedger( temporaryRoot / "gpu-probe-progress.ndjson",
                std::ios::binary | std::ios::app );
            if( !progressLedger ) throw std::runtime_error( "gpu_probe_progress_open_failed" );
            const auto writeProgress = [&]( std::string_view stage, uint64_t completed, uint64_t total ) {
                progressLedger << nlohmann::json {
                    { "elapsed_ms", std::to_string( ElapsedMs( began ) ) },
                    { "stage", stage }, { "completed", std::to_string( completed ) },
                    { "total", std::to_string( total ) }
                }.dump() << '\n';
                progressLedger.flush();
                if( !progressLedger ) throw std::runtime_error( "gpu_probe_progress_write_failed" );
            };
            writeProgress( "starting", 0, 0 );
            if( mode == "--job" )
            {
                const auto total = source->GetJobCount();
                uint64_t completed = 0, checksum = 0;
                auto reportedAt = std::chrono::steady_clock::now();
                writeProgress( "job_policy_projection", 0, total );
                while( completed < total )
                {
                    const auto take = size_t( std::min<uint64_t>( NativeBoundedScanMaximumBatch, total - completed ) );
                    const auto page = source->ScanJobsForPolicy( size_t( completed ), take );
                    if( page.size() != take ) throw std::runtime_error( "job_policy_projection_count" );
                    for( const auto& job : page )
                    {
                        if( !job.dependencies.empty() || !job.stages.empty() ||
                            !job.waitCallstacks.empty() || !job.executionLanes.empty() )
                            throw std::runtime_error( "job_policy_projection_retained_details" );
                        checksum ^= job.jobId + job.packedHandle + uint64_t( job.scheduleNs ) +
                            uint64_t( job.executionNs ) + uint64_t( job.waitNs ) + job.originFrameId +
                            uint64_t( job.typeId ) + uint64_t( job.name.size() );
                    }
                    completed += page.size();
                    const auto now = std::chrono::steady_clock::now();
                    if( completed == total || now - reportedAt >= std::chrono::seconds( 5 ) )
                    {
                        reportedAt = now;
                        writeProgress( "job_policy_projection", completed, total );
                        std::cerr << "job-probe completed=" << completed << " total=" << total << '\n';
                    }
                }
                const auto snapshot = resources->Snapshot();
                writeProgress( "complete", completed, total );
                std::cout << nlohmann::json {
                    { "mode", "job-policy" }, { "elapsed_ms", std::to_string( ElapsedMs( began ) ) },
                    { "jobs", std::to_string( completed ) }, { "checksum", std::to_string( checksum ) },
                    { "workspace_current", std::to_string( snapshot.workspace.currentBytes ) },
                    { "workspace_peak", std::to_string( snapshot.workspace.peakBytes ) },
                    { "disk_current", std::to_string( snapshot.disk.currentBytes ) },
                    { "disk_peak", std::to_string( snapshot.disk.peakBytes ) }
                }.dump() << '\n';
                return 0;
            }
            GpuJobManagedScanOptions options;
            options.workspace = resources->Workspace(); options.disk = resources->Disk();
            options.temporaryRoot = temporaryRoot; options.retainDetails = false;
            options.includeGpuFrameEvidence = false; options.includeJobDetails = false;
            options.includeLogicalGpuSignatures = false;
            auto reportedAt=std::chrono::steady_clock::now(); std::string reportedStage;
            options.progress=[&](std::string_view stage,uint64_t completed,uint64_t total) {
                const auto now=std::chrono::steady_clock::now();
                if(stage==reportedStage && completed!=total && now-reportedAt<std::chrono::seconds(5)) return;
                reportedStage=stage; reportedAt=now;
                writeProgress( stage, completed, total );
                std::cerr<<"gpu-probe stage="<<stage<<" completed="<<completed<<" total="<<total<<'\n';
            };
            options.definitionSink = [&]( const auto& ) { ++definitions; return true; };
            options.gpuZoneSink = [&]( const auto& ) { ++gpuFacts; return true; };
            options.jobSink = [&]( const auto& ) { ++jobs; return true; };
            const auto result = GpuJobManagedScanner( *source ).Scan( options );
            const auto snapshot = resources->Snapshot();
            writeProgress( "complete", result.gpuZoneCount + result.jobCount + result.relationCount,
                result.gpuZoneCount + result.jobCount + result.relationCount );
            std::cout << nlohmann::json {
                { "mode", "gpu-policy" }, { "elapsed_ms", std::to_string( ElapsedMs( began ) ) },
                { "definitions", std::to_string( definitions ) }, { "gpu_facts", std::to_string( gpuFacts ) },
                { "jobs", std::to_string( jobs ) }, { "relations", std::to_string( result.relationCount ) },
                { "ambiguous_relations", std::to_string( result.ambiguousRelationCount ) },
                { "managed_zones", std::to_string( result.managedZoneCount ) },
                { "workspace_current", std::to_string( snapshot.workspace.currentBytes ) },
                { "workspace_peak", std::to_string( snapshot.workspace.peakBytes ) },
                { "disk_current", std::to_string( snapshot.disk.currentBytes ) },
                { "disk_peak", std::to_string( snapshot.disk.peakBytes ) }
            }.dump() << '\n';
            return 0;
        }
        if( argc != 4 ) throw std::runtime_error( "expected trace path, start ordinal and count" );
        const auto tracePath = std::filesystem::path( argv[1] );
        const auto start = std::stoull( argv[2] );
        const auto count = std::stoull( argv[3] );
        if( count == 0 || count > NativeBoundedScanMaximumBatch )
            throw std::runtime_error( "probe_count_out_of_range" );
        auto resources = QueryIndex::CreateResourceContext( tracePath );
        const auto validated = QueryIndex::Validate( tracePath, false, resources );
        if( !validated.manifest ) throw std::runtime_error( validated.reason );
        auto source = QueryIndex::Open( *validated.manifest, {}, {}, resources );
        const auto openedWorkspace = resources->Snapshot().workspace;
        ScanRange range;
        range.offset = size_t( start );
        range.limit = size_t( count );
        auto began = std::chrono::steady_clock::now();
        const auto policy = source->ScanCpuZonesForPolicy( range );
        const auto policyMs = ElapsedMs( began );
        const auto policyWorkspace = resources->Snapshot().workspace;
        began = std::chrono::steady_clock::now();
        const auto raw = source->ScanCpuZones( range );
        const auto rawMs = ElapsedMs( began );
        const auto rawWorkspace = resources->Snapshot().workspace;
        if( policy.size() != count || raw.size() != count || policy.size() != raw.size() )
            throw std::runtime_error( "cpu_policy_projection_count" );
        uint64_t dynamicNames = 0;
        for( size_t index = 0; index < raw.size(); ++index )
        {
            RequireSameFacts( raw[index], policy[index] );
            if( raw[index].extraName ) ++dynamicNames;
        }
        if( dynamicNames == 0 ) throw std::runtime_error( "probe_window_has_no_dynamic_names" );
        std::cout << nlohmann::json {
            { "start", std::to_string( start ) }, { "count", std::to_string( count ) },
            { "dynamic_names", std::to_string( dynamicNames ) },
            { "policy_ms", std::to_string( policyMs ) }, { "raw_ms", std::to_string( rawMs ) },
            { "workspace_open_current", std::to_string( openedWorkspace.currentBytes ) },
            { "workspace_policy_current", std::to_string( policyWorkspace.currentBytes ) },
            { "workspace_raw_current", std::to_string( rawWorkspace.currentBytes ) },
            { "workspace_peak", std::to_string( rawWorkspace.peakBytes ) },
            { "facts_equal", true }, { "dynamic_payload_omitted", true }
        }.dump() << '\n';
        return 0;
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

#include "TracyQueryIndex.hpp"
#include "../../analysis/TracyBoundedScanCursor.hpp"
#include "../../analysis/TracyGpuJobManagedScanner.hpp"
#include "../../analysis/TracyFrameCpuScanner.hpp"

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
        if( argc == 4 && std::string_view( argv[1] ) == "--cpu-prefix" )
        {
            const auto target = std::stoull( argv[3] );
            if( target == 0 || target > 20'000'000 ) throw std::runtime_error( "cpu_prefix_limit" );
            const auto tracePath = std::filesystem::path( argv[2] );
            QueryResourceContextOptions options;
            const auto bytes = std::filesystem::file_size( tracePath );
            options.expectedFinalDiskLimit = bytes * 4; options.expectedPeakDiskLimit = bytes * 8;
            auto resources = QueryIndex::CreateResourceContext( tracePath, options );
            const auto validated = QueryIndex::Validate( tracePath, false, resources );
            if( !validated.manifest ) throw std::runtime_error( validated.reason );
            auto source = QueryIndex::Open( *validated.manifest, {}, {}, resources );
            const auto started = std::chrono::steady_clock::now();
            uint64_t observed = 0, emitted = 0;
            CpuFrameScanOptions scan;
            scan.workspace = resources->Workspace();
            scan.includeExactSignatures = false; scan.includeLogicalSignatures = true;
            scan.logicalSignatureMode = CpuLogicalSignatureMode::FullPath;
            scan.canonicalizeLogicalDynamicNames = true;
            scan.includeLogicalThreadRole = true;
            scan.progress = [&]( uint64_t count, uint64_t runs ) { observed = count; emitted = runs; };
            scan.cancelled = [&] { return observed >= target || ElapsedMs( started ) > 120000; };
            scan.definitionSink = []( const CpuSignatureDefinition& ) { return true; };
            bool stopped = false;
            try {
                ExactFrameCpuScanner( *source ).ScanView(
                    []( const CpuSignatureFrameRunView& ) { return true; }, scan );
            }
            catch( const BoundedScanError& error ) {
                if( std::string( error.what() ).find( "cancelled" ) == std::string::npos ) throw;
                stopped = true;
            }
            const auto snapshot = resources->Snapshot();
            std::cout << nlohmann::json {
                { "mode", "cpu_prefix_bounded" }, { "zones", std::to_string( observed ) },
                { "target", std::to_string( target ) }, { "emitted_runs", std::to_string( emitted ) },
                { "scan_elapsed_ms", std::to_string( ElapsedMs( started ) ) },
                { "stopped", stopped }, { "workspace_peak", std::to_string( snapshot.workspace.peakBytes ) },
                { "disk_current", std::to_string( snapshot.disk.currentBytes ) }
            }.dump() << '\n';
            return stopped && observed >= target ? 0 : 2;
        }
        if( argc == 3 && std::string_view( argv[1] ) == "--memory-summary" )
        {
            const std::filesystem::path tracePath = argv[2];
            QueryResourceContextOptions options;
            const auto bytes=std::filesystem::file_size(tracePath);
            if(bytes>UINT64_MAX/8) throw std::runtime_error("probe_trace_size");
            options.expectedFinalDiskLimit=bytes*4; options.expectedPeakDiskLimit=bytes*8;
            auto resources=QueryIndex::CreateResourceContext(tracePath,options);
            const auto validated=QueryIndex::Validate(tracePath,false,resources);
            if(!validated.manifest)throw std::runtime_error(validated.reason);
            auto source=QueryIndex::Open(*validated.manifest,{},{},resources);
            const auto total=source->GetTraceInfo().counts.memoryEvents;
            if(total<32)throw std::runtime_error("memory_probe_needs_32_events");
            for(uint64_t offset:{uint64_t(0),total/2,total-32})
            {
                ScanRange range;range.offset=size_t(offset);range.limit=32;
                const auto full=source->ScanMemoryEvents(range);
                const auto at=std::chrono::steady_clock::now();
                const auto small=source->ScanMemoryEventsForSummary(size_t(offset),32);
                if(full.size()!=32 || small.size()!=32)throw std::runtime_error("memory_projection_count");
                for(size_t i=0;i<small.size();++i)
                {
                    const auto& a=full[i];const auto& b=small[i];
                    if(a.ref!=b.ref || a.poolRef!=b.poolRef || a.address!=b.address || a.size!=b.size ||
                        a.allocationNs!=b.allocationNs || a.freeNs!=b.freeNs || a.complete!=b.complete)
                        throw std::runtime_error("memory_projection_changed_lifetime_fact");
                    if(!b.allocationThreadRef.empty() || b.freeThreadRef || b.allocationCallstack || b.freeCallstack ||
                        b.allocationCallstackRef || b.freeCallstackRef || b.allocationZoneRef || b.freeZoneRef)
                        throw std::runtime_error("memory_summary_retained_optional_details");
                }
                std::cout<<nlohmann::json{{"offset",offset},{"rows",small.size()},
                    {"elapsed_ms",ElapsedMs(at)},{"facts_equal",true}}.dump()<<std::endl;
            }
            bool rejected=false;
            try{(void)source->ScanMemoryEventsForSummary(0,32,[]{throw std::runtime_error("probe_cancelled");});}
            catch(const std::runtime_error& e){rejected=std::string_view(e.what())=="probe_cancelled";}
            if(!rejected)throw std::runtime_error("memory_summary_ignored_cancellation");
            return 0;
        }
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

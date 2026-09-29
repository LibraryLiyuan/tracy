#include "TracyQueryIndex.hpp"
#include "../../analysis/TracyBoundedScanCursor.hpp"
#include "../../analysis/TracyGpuJobManagedScanner.hpp"
#include "../../analysis/TracyFrameCpuScanner.hpp"
#include "../../analysis/TracyExactStatistics.hpp"
#include "../../analysis/TracyFrameWindowPolicy.hpp"
#include "TracyAnalysisScanCache.hpp"
#include "TracyAnalysisScanManager.hpp"
#include "TracyNeutralStatisticsCache.hpp"
#include "TracyPolicyFrameSeries.hpp"
#include "TracyAnalysisCacheSort.hpp"
#include "TracyAnalysisCacheKey.hpp"
#include "TracyCandidatePolicyTopRecord.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

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
        const bool binaryPolicyTops=argc==7 && std::string_view(argv[1])=="--policy-context-write-binary";
        const bool writePolicyTops=argc==7 && (std::string_view(argv[1])=="--policy-context-write" ||
            binaryPolicyTops);
        const bool sharedPolicyRead=argc==6 && std::string_view(argv[1])=="--policy-context-shared";
        if((argc==6 && std::string_view(argv[1])=="--policy-context") || writePolicyTops || sharedPolicyRead)
        {
            const std::filesystem::path generation=argv[2],pointerPath=argv[3],profilePath=argv[4];
            const auto ordinal=std::stoull(argv[5]);
            nlohmann::json pointer,profile;
            {std::ifstream input(pointerPath); input>>pointer;}
            {std::ifstream input(profilePath); input>>profile;}
            const auto identity=pointer.at("identity").get<std::string>();
            const auto started=std::chrono::steady_clock::now();
            const auto cancelled=[&]{return ElapsedMs(started)>120000;};
            auto workspace=std::make_shared<AnalysisWorkspaceBudget>(4ull<<30,2ull<<30);
            AnalysisCacheTableOptions table; table.workspace=workspace; table.cancelled=cancelled;
            std::filesystem::path scratch;
            std::shared_ptr<AnalysisDiskUsage> scratchUsage;
            std::unique_ptr<AnalysisCacheSortedWriter> topWriter;
            if(writePolicyTops) {
                scratch=argv[6];
                if(std::filesystem::exists(scratch))throw std::runtime_error("policy_probe_scratch_exists");
                std::filesystem::create_directories(scratch);
                scratchUsage=std::make_shared<AnalysisDiskUsage>(std::vector<std::filesystem::path>{scratch});
                table.disk=std::make_shared<AnalysisDiskBudget>(AnalysisDiskBudget{scratchUsage,1ull<<30,{}});
                topWriter=std::make_unique<AnalysisCacheSortedWriter>(scratch/"top-input",identity,
                    "candidate-policy-cache-v1-local-top-input",table);
            }
            auto bundle=OpenAnalysisNeutralBundle(generation,identity,
                pointer.at("header_sha256").get<std::string>(),table);
            CandidatePolicyInput policy; policy.aggregateIdentity=identity;
            policy.normalizedProfile=std::move(profile);
            policy.frameTimelines=std::move(bundle.frameTimelines);
            policy.cancelled=cancelled;
            AnalysisWorkspaceReservation timelineWorkspace(workspace);
            uint64_t timelineRows=0;
            for(const auto& timeline:policy.frameTimelines) timelineRows+=timeline.frames.size();
            if(timelineRows>UINT64_MAX/512)throw std::runtime_error("policy_probe_timeline_budget");
            timelineWorkspace.Add(timelineRows*512);
            const auto state=frame_window::Prepare(policy,[](const auto&){},&timelineWorkspace,workspace);
            std::unique_ptr<PagedRecordReader> sharedSeries;
            if(sharedPolicyRead) {
                PagedRecordOptions readerOptions; readerOptions.workspace=workspace;
                readerOptions.cancelled=cancelled;
                sharedSeries=std::make_unique<PagedRecordReader>(bundle.frameSeriesPath,identity,
                    sizeof(PolicySeriesRecord),readerOptions);
            }
            NeutralStatisticsCacheOptions neutralOptions; neutralOptions.table=table;
            NeutralStatisticsCacheReader neutral(bundle.neutralCachePath,identity,neutralOptions);
            const auto page=neutral.Contexts(ordinal,32,1<<20);
            nlohmann::json samples=nlohmann::json::array();
            for(const auto& row:page.records)
            {
                if(cancelled())throw std::runtime_error("policy_probe_time_limit");
                PolicySignatureContext context; std::string error;
                if(!DeserializePolicySignatureContextRecord(row.payload,context,error))
                    throw std::runtime_error(error);
                if(!context.sourceOrdinal || !context.frameSeriesComplete || context.seriesCount>100000 ||
                    !state.timelines.contains(context.frameScope))continue;
                AnalysisWorkspaceReservation seriesWorkspace(workspace,65536+context.seriesCount*512);
                uint64_t readMs=0,topCount=0;
                policy.readFrameSeries=[&](const PolicySignatureContext& current) {
                    const auto began=std::chrono::steady_clock::now();
                    auto values=sharedSeries ? ReadPolicyFrameSeries(*sharedSeries,current,
                        workspace->Snapshot().maximumBytes,cancelled) :
                        ReadPolicyFrameSeries(bundle.frameSeriesPath,current,workspace->Snapshot().maximumBytes,
                            cancelled,identity);
                    readMs+=ElapsedMs(began); return values;
                };
                const auto began=std::chrono::steady_clock::now();
                const auto sourceKey=cache_key::Signature(context.domain,context.signatureId,context.frameScope);
                const auto signals=frame_window::ScanContext(policy,size_t(*context.sourceOrdinal),
                    context,state,[&](const frame_window::TopKey& group,frame_window::TopItem item){
                        ++topCount;
                        if(!topWriter)return;
                        item.sourceKey=sourceKey;
                        const auto groupKey=cache_key::Field(std::get<0>(group))+
                            cache_key::Field(std::get<1>(group))+cache_key::Field(std::get<2>(group))+
                            cache_key::Field(std::get<3>(group));
                        if(binaryPolicyTops)
                            topWriter->Append(groupKey+cache_key::Unsigned(item.context),
                                candidate_policy::top_record::Encode(item));
                        else {
                            nlohmann::json payload={{"context",item.context},{"frame",item.frame},
                                {"cost",std::to_string(item.cost)},{"manifestation",item.manifestation},
                                {"evidence",item.evidence},{"signature",item.signature},
                                {"source_key",cache_key::Hex(item.sourceKey)},
                                {"group",cache_key::Hex(groupKey)}};
                            topWriter->Append(groupKey+cache_key::Unsigned(item.context),payload.dump());
                        }
                    });
                samples.push_back({{"source_ordinal",std::to_string(*context.sourceOrdinal)},
                    {"frame_scope",context.frameScope},{"series_rows",std::to_string(context.seriesCount)},
                    {"scan_ms",ElapsedMs(began)},{"series_read_ms",readMs},
                    {"top_facts",topCount},{"signals",signals.size()}});
            }
            uint64_t commitMs=0;
            if(topWriter){const auto began=std::chrono::steady_clock::now(); topWriter->Commit();commitMs=ElapsedMs(began);}
            std::cout<<nlohmann::json{{"mode",binaryPolicyTops?"policy_context_binary_scratch":
                writePolicyTops?"policy_context_scratch":
                sharedPolicyRead?"policy_context_shared_reader":"policy_context_read_only"},
                {"start_ordinal",ordinal},{"top_commit_ms",commitMs},
                {"scratch_bytes",scratchUsage?scratchUsage->Snapshot().currentBytes:0},
                {"elapsed_ms",ElapsedMs(started)},{"timeline_rows",timelineRows},
                {"page_records",page.records.size()},{"samples",samples},
                {"workspace_peak",workspace->Snapshot().peakBytes}}.dump()<<'\n';
            return 0;
        }
        const bool neutralPrefix=argc==5 && std::string_view(argv[1])=="--cpu-prefix-neutral";
        if( ( argc == 4 && ( std::string_view( argv[1] ) == "--cpu-prefix" ||
            std::string_view( argv[1] ) == "--cpu-prefix-binary" ) ) || neutralPrefix )
        {
            const auto target = std::stoull( argv[3] );
            if( target == 0 || target > 20'000'000 ) throw std::runtime_error( "cpu_prefix_limit" );
            const auto tracePath = std::filesystem::path( argv[2] );
            QueryResourceContextOptions options;
            const auto bytes = std::filesystem::file_size( tracePath );
            options.expectedFinalDiskLimit = bytes * 4; options.expectedPeakDiskLimit = bytes * 8;
            std::filesystem::path scratch;
            if(neutralPrefix) {
                scratch=argv[4];
                if(std::filesystem::exists(scratch)) throw std::runtime_error("cpu_prefix_scratch_exists");
                std::filesystem::create_directories(scratch);
                options.accountingRoots.push_back(scratch);
            }
            auto resources = QueryIndex::CreateResourceContext( tracePath, options );
            const auto validated = QueryIndex::Validate( tracePath, false, resources );
            if( !validated.manifest ) throw std::runtime_error( validated.reason );
            auto source = QueryIndex::Open( *validated.manifest, {}, {}, resources );
            const auto started = std::chrono::steady_clock::now();
            uint64_t observed = 0, emitted = 0;
            CpuFrameScanOptions scan;
            scan.workspace = resources->Workspace();
            scan.useFrameCursor=std::string_view(argv[1])!="--cpu-prefix-binary";
            CpuFrameScanTimings latestTimings;
            scan.timingProgress=[&](const CpuFrameScanTimings& value) { latestTimings=value; };
            scan.includeExactSignatures = false; scan.includeLogicalSignatures = true;
            scan.logicalSignatureMode = CpuLogicalSignatureMode::FullPath;
            scan.canonicalizeLogicalDynamicNames = true;
            scan.includeLogicalThreadRole = true;
            scan.progress = [&]( uint64_t count, uint64_t runs ) { observed = count; emitted = runs; };
            scan.cancelled = [&] { return observed >= target || ElapsedMs( started ) > 120000; };
            scan.definitionSink = []( const CpuSignatureDefinition& ) { return true; };
            std::unique_ptr<NeutralStatisticsStreamBuilder> statistics;
            AnalysisWorkspaceReservation registryWorkspace(resources->Workspace());
            std::unordered_map<uint64_t,NeutralStatisticsStreamBuilder::SignatureToken> tokens;
            if(neutralPrefix) statistics=std::make_unique<NeutralStatisticsStreamBuilder>(
                scratch/"neutral",131072,65536,resources->Workspace(),resources->Disk());
            bool stopped = false;
            try {
                ExactFrameCpuScanner( *source ).ScanView(
                    [&]( const CpuSignatureFrameRunView& run ) {
                        if(!statistics) return true;
                        const auto key=(uint64_t(run.signatureOrdinal)<<32)|run.frameSetOrdinal;
                        auto found=tokens.find(key);
                        if(found==tokens.end()) {
                            const auto token=statistics->RegisterSignature("cpu",run.signatureId,run.frameSetRef);
                            if(token==NeutralStatisticsStreamBuilder::InvalidSignatureToken)
                                throw std::runtime_error("cpu_prefix_neutral_register");
                            registryWorkspace.Add(128);
                            found=tokens.emplace(key,token).first;
                        }
                        const bool wait=run.workClass==CpuWorkClass::Wait ||
                            run.workClass==CpuWorkClass::IntentionalPacing;
                        if(!statistics->AddRegisteredRun(found->second,uint64_t(run.frameIndex),
                            run.inclusiveNs,run.exclusiveNs,wait?run.inclusiveNs:0,0,
                            run.occurrenceCount,run.exact,true))
                            throw std::runtime_error("cpu_prefix_neutral_ingest");
                        return true;
                    }, scan );
            }
            catch( const BoundedScanError& error ) {
                if( std::string( error.what() ).find( "cancelled" ) == std::string::npos ) throw;
                stopped = true;
            }
            const auto snapshot = resources->Snapshot();
            std::cout << nlohmann::json {
                { "mode", scan.useFrameCursor ? "cpu_prefix_cursor" : "cpu_prefix_binary" },
                { "zones", std::to_string( observed ) },
                { "target", std::to_string( target ) }, { "emitted_runs", std::to_string( emitted ) },
                { "scan_elapsed_ms", std::to_string( ElapsedMs( started ) ) },
                { "frame_read_ms", std::to_string( latestTimings.frameReadNs / 1'000'000 ) },
                { "zone_read_decode_ms", std::to_string( latestTimings.zoneReadNs / 1'000'000 ) },
                { "zone_process_ms", std::to_string( latestTimings.zoneProcessNs / 1'000'000 ) },
                { "frame_pages", std::to_string( latestTimings.frameReadCalls ) },
                { "zone_pages", std::to_string( latestTimings.zoneReadCalls ) },
                { "frame_boundary_probes", std::to_string( latestTimings.frameBoundaryProbes ) },
                { "logical_signature_hashes", std::to_string( latestTimings.logicalSignatureHashes ) },
                { "sink_samples", std::to_string( latestTimings.sinkSamples ) },
                { "sink_sample_ns", std::to_string( latestTimings.sinkSampleNs ) },
                { "sink_mode", neutralPrefix ? "neutral" : "empty" },
                { "scratch_bytes", std::to_string(neutralPrefix ? AnalysisDiskUsage::Bytes(scratch) : 0) },
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

// Diagnostic-only native probe. Reads one approved Trace through the real
// Worker/scanner; does not run CPU analysis, CandidatePolicy, or publish a cache.
#include "TracySessionManager.hpp"
#include "TracyGpuJobManagedScanner.hpp"
#include "TracyExactStatistics.hpp"
#include "TracyAnalysisProcessMemory.hpp"
#include "TracyHash.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

using namespace tracy::analysis;
using namespace tracy::query;
using nlohmann::json;

int main(int argc,char** argv)
{
    if(argc!=5) { std::cerr<<"trace output-directory expected-sha256 target-signature required\n"; return 2; }
    try {
        const auto trace=std::filesystem::canonical(argv[1]);
        if(Sha256File(trace)!=argv[3]) throw std::runtime_error("trace_identity_mismatch");
        const auto output=std::filesystem::absolute(argv[2]);
        if(std::filesystem::exists(output)) throw std::runtime_error("fresh_output_required");
        std::filesystem::create_directories(output);
        std::ofstream facts(output/"gpu-facts.ndjson",std::ios::binary);
        if(!facts) throw std::runtime_error("probe_output_open_failed");
        const auto emit=[&](const json& value) {
            facts<<value.dump()<<'\n';
            if(!facts) throw std::runtime_error("probe_output_write_failed");
        };
        AnalysisProcessMemoryOptions memory; memory.maximumBytes=32ull*1024*1024*1024;
        AnalysisProcessMemoryGuard guard(memory);
        std::stop_source stop; auto monitor=guard.Monitor(stop);
        const auto started=std::chrono::steady_clock::now();
        const auto check=[&] {
            guard.Check(false);
            if(stop.stop_requested() || std::chrono::steady_clock::now()-started>std::chrono::minutes(15))
                throw std::runtime_error("probe_cancelled_or_timeout");
        };
        SessionManager sessions({trace.parent_path()},1,{},false);
        const auto opened=sessions.Open(trace);
        TraceSessionSnapshot ready;
        do {
            check();
            ready=sessions.WaitReady(opened.id,std::chrono::milliseconds(100));
            if(ready.state==TraceSourceState::Failed) throw std::runtime_error(ready.errorMessage);
        } while(ready.state!=TraceSourceState::Ready);
        std::cerr<<"[gpu-scope-probe] Worker ready\n";
        auto source=sessions.GetReadySource(opened.id);
        const auto info=source->GetTraceInfo();
        emit({{"kind","trace"},{"fingerprint",info.fingerprint},
            {"first_ns",info.firstTimeNs},{"last_ns",info.lastTimeNs}});
        const std::string target=argv[4];
        auto workspace=std::make_shared<AnalysisWorkspaceBudget>();
        AnalysisWorkspaceReservation stateBudget(workspace);
        NeutralStatisticsStreamBuilder numeric(output/"target-numeric",65536,4096,workspace);
        std::map<std::string,uint64_t> roots,invalidRoots;
        std::string targetScope;
        uint64_t badFacts=0,targetFacts=0,acceptedTarget=0,reportedBadFacts=0;
        GpuJobManagedScanOptions options;
        options.workspace=workspace; options.retainDetails=false;
        options.includeLogicalGpuSignatures=false;
        options.cancelled=[&] {check(); return false;};
        options.definitionSink=[&](const GpuSignatureDefinition& value) {
            if(value.signatureId==target) emit({{"kind","target_definition"},
                {"signature_id",value.signatureId},{"parent_signature_id",value.parentSignatureId},
                {"name",value.name},{"path",value.path},{"depth",value.depth}});
            return true;
        };
        options.gpuZoneSink=[&](const GpuZoneScanFact& value) {
            check();
            const auto scope="GPU.L0Segment:"+value.contextRef;
            if(value.depth==0) {
                if(!roots.contains(scope)) {stateBudget.Add(2048+16*scope.size()); roots[scope]=0; invalidRoots[scope]=0;}
                if(value.physicalTimingExact) ++roots[scope]; else ++invalidRoots[scope];
            }
            if(!value.physicalTimingExact) ++badFacts;
            const bool isTarget=value.signatureId==target;
            if(isTarget) {
                ++targetFacts; targetScope=scope;
                if(value.physicalTimingExact) {
                    ++acceptedTarget;
                    if(!numeric.AddRun({"gpu",target,scope,value.l0SegmentOrdinal,
                        value.inclusiveNs,value.exclusiveNs,0,0,1,true,false}))
                        throw std::runtime_error("probe_target_ingest_failed");
                }
            }
            if(value.depth==0 || (isTarget && targetFacts<=10000) ||
                (!value.physicalTimingExact && reportedBadFacts++<128))
                emit({{"kind","gpu_fact"},{"ref",value.zoneRef},{"parent_ref",value.parentZoneRef},
                    {"signature_id",value.signatureId},{"scope",scope},{"l0",value.l0SegmentOrdinal},
                    {"depth",value.depth},{"begin_ns",value.beginNs},{"end_ns",value.endNs},
                    {"inclusive_ns",value.inclusiveNs},{"exclusive_ns",value.exclusiveNs},
                    {"physical_exact",value.physicalTimingExact},{"target",isTarget}});
            return true;
        };
        const auto scanned=GpuJobManagedScanner(*source).Scan(options);
        std::cerr<<"[gpu-scope-probe] Native GPU/Job scanner complete\n";
        if(targetScope.empty()) throw std::runtime_error("target_not_found");
        numeric.AddDenominator({"gpu",target,targetScope,roots[targetScope]});
        NeutralStatisticsStreamSummary summary; std::string error;
        const auto finished=numeric.FinishToSink(summary,error,[&](const NeutralSignatureAggregate& value,const auto&) {
            emit({{"kind","target_statistics"},{"record",json::parse(SerializeNeutralSignatureRecord(value))}});
        },options.cancelled);
        json quality=json::array();
        for(const auto& finding:scanned.qualityFindings)
            quality.push_back({{"code",finding.code},{"count",finding.count},{"representative_refs",finding.representativeRefs}});
        guard.Check(); const auto memoryResult=guard.Snapshot();
        const json result={{"kind","probe_summary"},{"diagnostic_only",true},
            {"physical_l0_count",scanned.physicalL0SegmentCount},{"accepted_roots",roots},{"invalid_roots",invalidRoots},
            {"gpu_fact_count",scanned.gpuZoneCount},{"invalid_source_zones",scanned.invalidGpuZoneCount},
            {"nonexact_facts",badFacts},{"target_facts",targetFacts},{"accepted_target_facts",acceptedTarget},
            {"statistics_finished",finished},{"statistics_error",error},{"statistics_quality_complete",summary.qualityComplete},
            {"quality_findings",quality},{"peak_private_bytes",memoryResult.peakPrivateBytes},
            {"peak_resident_bytes",memoryResult.peakResidentBytes},{"memory_limit_bytes",memoryResult.maximumBytes},
            {"elapsed_ms",std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count()}};
        emit(result); facts.close();
        source.reset(); sessions.Close(opened.id);
        if(Sha256File(trace)!=argv[3]) throw std::runtime_error("trace_changed");
        std::cout<<result.dump()<<'\n';
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n'; return 1;}
}

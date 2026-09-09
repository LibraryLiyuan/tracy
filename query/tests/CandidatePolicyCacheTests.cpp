#include "TracyCandidatePolicyCache.hpp"
#include "TracyHash.hpp"
#include <chrono>
#include <iostream>
#include <fstream>
#include <stdexcept>
using namespace tracy::analysis;
using nlohmann::json;
namespace
{
const std::string Identity(64,'a');
void Require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
template<class Action> void Reject(Action action,const char* expected)
{
    try { action(); } catch(const std::exception& e) {
        Require(std::string(e.what()).find(expected)!=std::string::npos,e.what()); return; }
    throw std::runtime_error(std::string("expected rejection: ")+expected);
}
std::string Sha(const json& j) { const auto s=j.dump(); Sha256Builder h; h.Update(s.data(),s.size()); return h.FinalHex(); }
CandidatePolicyInput Fixture(const std::filesystem::path& root, bool local=false)
{
    CandidatePolicyInput input; input.aggregateIdentity=Identity;
    input.normalizedProfile={
        {"frame_budget",{{"frame_ms",16.666667}}},
        {"module_budgets",json::array({{{"id","module"},{"scope","per_complete_frame"},
            {"budget_ms",0.0005},{"status","fixed"},{"marker_rules",{"exact:Budget"}}}})},
        {"resource_budgets",{{"memory_bytes",100},{"memory_status","fixed"}}},
        {"user_focus",{"Focus","Common"}},
        {"candidate_policy",{{"top_n",1},{"cumulative_contribution",0.60},
            {"per_domain_limit",1},{"priorities",{"P2","P3","P4"}}}}};
    input.profileIdentity=Sha(input.normalizedProfile);
    NeutralStatisticsInput raw; raw.temporaryRoot=root/"oracle";
    constexpr int Count=12;
    for(int i=0;i<Count;++i)
    {
        const auto id="signature-"+std::to_string(i);
        const auto scope=i%3==0?"Render":"Player";
        raw.denominators.push_back({"cpu",id,scope,5});
        for(uint64_t f=0;f<3;++f)
        {
            const int64_t v=i==11?0:((i%5+1)*1000*(f+1));
            raw.runs.push_back({"cpu",id,scope,f,v,v/2,v/10,v/5,1,true,false});
        }
        PolicySignatureContext c; c.domain="cpu"; c.signatureId=id; c.frameScope=scope;
        c.familyId=i<3?"shared-family":"family-"+std::to_string(i);
        c.name=i==4?"Budget":i<3?"Focus Common":i==5?"Common":"Regular";
        c.path="Thread/FullPath/"+id; c.threadOrQueue="thread-"+std::to_string(i);
        c.frames={{0,100,{},"first"},{1,1000,{"event:"+id},"next"},{2,200,{},""}};
        c.provenCriticalPath=i==7;
        if(local)
        {
            c.frameSeriesComplete=true; c.threadOrQueue="shared-thread";
            c.frames.clear();
            for(uint64_t f=0;f<60;++f)
                c.frames.push_back({f, i==2 && f==17?12'000'000:int64_t((i+1)*40000),
                    {"event:"+id+":"+std::to_string(f)},"",!(i==11 && f==15)});
        }
        input.signatures.push_back(c);
    }
    // Original traversal is deliberately the reverse of the cache's key order.
    std::reverse(input.signatures.begin(),input.signatures.end());
    for(size_t i=0;i<input.signatures.size();++i) input.signatures[i].sourceOrdinal=i;
    if(local)
    {
        input.normalizedProfile["candidate_policy"]["local_top_n"]=1;
        input.normalizedProfile["candidate_policy"]["absolute_frame_cost_ms"]=10.0;
        input.profileIdentity=Sha(input.normalizedProfile);
        PolicySignatureContext wall; wall.domain="cpu"; wall.signatureId="frame-root"; wall.frameScope="Player";
        wall.name="Player"; wall.frameRoot=true; wall.frameSeriesComplete=true; wall.sourceOrdinal=input.signatures.size();
        for(uint64_t f=0;f<60;++f) wall.frames.push_back({f,f<10?12'000'000:24'000'000,{"wall:"+std::to_string(f)},"",true});
        input.frameTimelines.push_back({"Player","Player",wall.frames,"frame"});
        input.signatures.push_back(std::move(wall));
        PolicyFrameTimeline gpuTimeline; gpuTimeline.frameScope="GPU.L0"; gpuTimeline.name="GPU queue"; gpuTimeline.observationUnit="l0_segment";
        for(uint64_t f=0;f<3;++f) gpuTimeline.frames.push_back({f,2'000'000,{"gpu-event:"+std::to_string(f)},"",true,int64_t(100+f*1000),int64_t(1000+f*1000)});
        input.frameTimelines.push_back(gpuTimeline);
        for(int i=0;i<2;++i)
        {
            PolicySignatureContext c; c.domain="gpu"; c.signatureId="gpu-"+std::to_string(i); c.frameScope="GPU.L0";
            c.name="GPU work"; c.path="Queue/Root/"+c.signatureId; c.observationUnit="l0_segment"; c.threadOrQueue="queue:1";
            c.sourceOrdinal=input.signatures.size(); c.frameSeriesComplete=true;
            raw.denominators.push_back({c.domain,c.signatureId,c.frameScope,3});
            for(uint64_t f=0;f<3;++f) {
                const int64_t cost=300000+i*100000;
                c.frames.push_back({f,cost,{"gpu-work:"+std::to_string(i)+":"+std::to_string(f)},"",true,int64_t(100+f*1000),int64_t(1000+f*1000)});
                raw.runs.push_back({c.domain,c.signatureId,c.frameScope,f,cost,cost,0,0,1,true,false}); }
            input.signatures.push_back(std::move(c));
        }
        raw.domainAudit.push_back({"gpu",true,"complete",6,6,2,Identity,Identity,true,{}});
    }
    raw.domainAudit.push_back({"cpu",true,"complete",36,36,Count,Identity,Identity,true,{}});
    input.aggregate=BuildNeutralStatistics(raw);
    input.aggregate.signatures[5].exclusive.pattern=AnomalyPattern::IsolatedSpike;
    input.aggregate.signatures[5].exclusive.anomalyCount=1;
    input.aggregate.signatures[5].exclusive.anomalies={{1,4000,2000}};
    input.capacityFacts.push_back({"cpu","capacity","shared-family","memory",200,2,true,"capacity fixture"});
    return input;
}
void WriteNeutral(const std::filesystem::path& root,const CandidatePolicyInput& input,NeutralStatisticsCacheOptions options)
{
    NeutralStatisticsCacheWriter w(root,Identity,options);
    for(const auto& s:input.aggregate.signatures) w.AppendStatistics(s);
    for(const auto& c:input.signatures) w.AppendContext(SerializePolicySignatureContextRecord(c));
    NeutralStatisticsStreamSummary summary; summary.qualityComplete=true;
    summary.signatureCount=input.aggregate.signatures.size(); summary.domains=input.aggregate.domains; w.Commit(summary);
}
void CachedParity(const std::filesystem::path& root, bool local=false, bool fallback=false)
{
    auto input=Fixture(root,local);
    if(fallback)
    {
        input.frameTimelines.erase(std::remove_if(input.frameTimelines.begin(),input.frameTimelines.end(),[](const auto& t){return t.frameScope=="Player";}),input.frameTimelines.end());
        const auto original=std::find_if(input.signatures.begin(),input.signatures.end(),[](const auto& c){return c.signatureId=="frame-root";});
        auto later=*original; later.signatureId="a-root-sorts-earlier"; later.sourceOrdinal=input.signatures.size();
        for(auto& f:later.frames) f.valueNs=1'000'000;
        input.signatures.push_back(std::move(later));
    }
    NeutralStatisticsCacheOptions neutralOptions; neutralOptions.table.blockBytes=16384; neutralOptions.sortBufferBytes=32768;
    WriteNeutral(root/"neutral",input,neutralOptions);
    const auto oracle=EvaluateCandidatePolicy(input);
    Require(oracle.valid,oracle.error.c_str());
    const auto expected=json::parse(SerializeCandidatePolicyResult(oracle));
    const auto shared=std::find_if(oracle.candidates.begin(),oracle.candidates.end(),[](const auto& c){return c.familyId=="shared-family";});
    Require(shared!=oracle.candidates.end() && shared->selected,"mandatory merged family must survive disabled P1");
    Require(shared->structuralSignature=="Thread/FullPath/signature-0","last original context wins family display fields");
    Require(shared->memberSignatures.size()==4,"capacity and three signatures share a family");
    Require(std::any_of(oracle.candidates.begin(),oracle.candidates.end(),[](const auto& c){return c.notSelectedReason=="per_domain_limit";}),"fixture exercises shared global domain quota");
    if(local)
    {
        const auto localWinner=std::find_if(oracle.candidates.begin(),oracle.candidates.end(),[](const auto& c){
            return c.signatureId=="signature-11" && c.manifestation=="frame_cost";});
        Require(localWinner!=oracle.candidates.end() && localWinner->selected,"cross-signature local Top survives zero global total");
        Require(std::none_of(oracle.candidates.begin(),oracle.candidates.end(),[](const auto& c){
            return c.signatureId=="signature-1" && c.manifestation=="frame_cost";}),"local Top is competitive, not independently applied to each signature");
        const auto l0=std::find_if(expected.at("candidates").begin(),expected.at("candidates").end(),[](const auto& c){
            return c.at("signature_id")=="gpu-1" && c.at("manifestation")=="frame_cost";});
        Require(l0!=expected.at("candidates").end() && l0->at("representative_frames").empty() &&
            !l0->at("representative_intervals").empty(),"GPU L0 evidence remains intervals, never invented Player frames");
    }
    auto metadata=input; metadata.aggregate.signatures.clear(); metadata.aggregate.rankings.clear(); metadata.signatures.clear();
    NeutralStatisticsCacheReader neutral(root/"neutral",Identity,neutralOptions);
    CandidatePolicyCacheOptions options; options.table.blockBytes=16384; options.sortBufferBytes=32768;
    const auto policyId=BuildCandidatePolicyCache(root/"policy",neutral,metadata,options);
    Require(policyId==oracle.policyIdentity,"policy identity unchanged");
    CandidatePolicyCacheReader cache(root/"policy",policyId,options);
    json candidates=json::array(),rankings=json::array();
    for(uint64_t ordinal=0;;) { auto page=cache.Candidates(ordinal,2,65536);
        for(const auto& row:page.records) { candidates.push_back(json::parse(row.payload));
            auto lookup=cache.Candidate(candidates.back().at("candidate_id").get<std::string>());
            Require(lookup && lookup->payload==row.payload,"candidate indexed lookup parity"); }
        ordinal=page.nextOrdinal; if(page.done) break; }
    for(uint64_t ordinal=0;;) { auto page=cache.RankedSignatures(ordinal,3,65536);
        for(const auto& row:page.records) rankings.push_back(json::parse(row.payload));
        ordinal=page.nextOrdinal; if(page.done) break; }
    Require(candidates==expected.at("candidates"),"all candidate fields and order match legacy oracle");
    Require(rankings==expected.at("ranked_signatures"),"all ranks including zero/outside-policy match legacy oracle");
    const auto summary=json::parse(cache.SummaryJson());
    Require(summary.at("backlog")==expected.at("backlog"),"global backlog parity");
    Require(summary.at("neutral_cache_content_sha256")==neutral.ContentSha256(),"policy binds the exact neutral cache contents");
    Reject([&]{cache.Candidates(0,1,1);},"response_budget");
    Require(!cache.Candidate("absent"),"missing candidate lookup");
    Reject([&]{BuildCandidatePolicyCache(root/"policy",neutral,metadata,options);},"already_exists");
}
void Boundaries(const std::filesystem::path& root)
{
    auto input=Fixture(root);
    NeutralStatisticsCacheOptions no; no.table.blockBytes=16384; no.sortBufferBytes=32768;
    WriteNeutral(root/"neutral",input,no);
    NeutralStatisticsCacheReader neutral(root/"neutral",Identity,no);
    auto metadata=input; metadata.signatures.clear(); metadata.aggregate.signatures.clear(); metadata.aggregate.rankings.clear();
    CandidatePolicyCacheOptions options; options.table.blockBytes=16384; options.sortBufferBytes=32768;
    Reject([&]{BuildCandidatePolicyCache(root/"materialized",neutral,input,options);},"materialized_input");
    auto wrong=metadata; wrong.aggregateIdentity=std::string(64,'b');
    Reject([&]{BuildCandidatePolicyCache(root/"wrong",neutral,wrong,options);},"neutral_identity_mismatch");
    auto budget=options; budget.familyBytes=2048;
    Reject([&]{BuildCandidatePolicyCache(root/"budget",neutral,metadata,budget);},"family_budget");
    Require(!std::filesystem::exists(root/"budget"/"header"/"manifest.json"),"resource failure does not publish partial candidates");
    auto cancel=options; const auto cancelledRoot=root/"cancel";
    cancel.table.cancelled=[&]{return std::filesystem::exists(cancelledRoot/"candidates"/"manifest.json");};
    Reject([&]{BuildCandidatePolicyCache(cancelledRoot,neutral,metadata,cancel);},"cancelled");
    Require(!std::filesystem::exists(cancelledRoot/"header"/"manifest.json"),"late cancellation leaves no queryable header");
    Reject([&]{CandidatePolicyCacheReader r(cancelledRoot,std::string(64,'a'),options);},"incomplete");
    for(const bool missing:{false,true})
    {
        auto badInput=input;
        if(missing) badInput.signatures[0].sourceOrdinal.reset();
        else badInput.signatures[0].sourceOrdinal=badInput.signatures.size()+1;
        const auto path=root/(missing?"missing-order":"gapped-order");
        WriteNeutral(path/"neutral",badInput,no); NeutralStatisticsCacheReader bad(path/"neutral",Identity,no);
        Reject([&]{BuildCandidatePolicyCache(path/"policy",bad,metadata,options);},missing?"context_order_missing":"context_order_gap");
        Require(!std::filesystem::exists(path/"policy"/"header"/"manifest.json"),"invalid traversal order cannot publish");
    }
    auto full=Fixture(root/"series",true); WriteNeutral(root/"series"/"neutral",full,no);
    NeutralStatisticsCacheReader seriesNeutral(root/"series"/"neutral",Identity,no);
    full.signatures.clear(); full.aggregate.signatures.clear(); full.aggregate.rankings.clear();
    full.readFrameSeries=[](const auto&) {
        std::vector<PolicyFrameEvidence> frames;
        for(uint64_t i=0;i<1000;++i) frames.push_back({i,0,{},"",true});
        return frames;
    };
    auto seriesOptions=options; seriesOptions.frameWorkspaceBytes=256*1024;
    Reject([&]{BuildCandidatePolicyCache(root/"series"/"policy",seriesNeutral,full,seriesOptions);},"frame_workspace_budget");
    Require(!std::filesystem::exists(root/"series"/"policy"/"header"/"manifest.json"),"oversized loaded series cannot publish");
    const auto policyId=BuildCandidatePolicyCache(root/"corrupt",neutral,metadata,options);
    const auto blocks=root/"corrupt"/"candidates"/"blocks";
    const auto first=std::filesystem::directory_iterator(blocks)->path();
    { std::ofstream truncated(first,std::ios::binary|std::ios::trunc); truncated<<'x'; }
    { CandidatePolicyCacheReader broken(root/"corrupt",policyId,options);
      Reject([&]{broken.Candidates(0,1,65536);},"analysis_cache_"); }
    NeutralStatisticsStreamSummary empty; empty.qualityComplete=true;
    { NeutralStatisticsCacheWriter writer(root/"empty-neutral",Identity,no); writer.Commit(empty); }
    NeutralStatisticsCacheReader emptyNeutral(root/"empty-neutral",Identity,no);
    auto emptyMetadata=metadata; emptyMetadata.capacityFacts.clear(); emptyMetadata.aggregate.domains.clear();
    const auto emptyId=BuildCandidatePolicyCache(root/"empty-policy",emptyNeutral,emptyMetadata,options);
    CandidatePolicyCacheReader emptyPolicy(root/"empty-policy",emptyId,options);
    Require(emptyPolicy.Candidates(0,10,65536).done && emptyPolicy.Candidates(0,10,65536).records.empty(),"empty candidate cache is a valid complete result");
    Require(json::parse(emptyPolicy.SummaryJson()).at("retained_final_candidate_peak")==0,"empty cache does not claim a retained candidate");
}
void SharedTimelineWorkspace(const std::filesystem::path& root)
{
    auto input=Fixture(root);
    NeutralStatisticsCacheOptions neutralOptions; neutralOptions.table.blockBytes=16384; neutralOptions.sortBufferBytes=32768;
    WriteNeutral(root/"neutral",input,neutralOptions);
    NeutralStatisticsCacheReader neutral(root/"neutral",Identity,neutralOptions);
    input.signatures.clear(); input.aggregate.signatures.clear(); input.aggregate.rankings.clear();
    PolicyFrameTimeline timeline; timeline.frameScope="Player"; timeline.name="Player";
    // Prepare must copy this full 16 MiB timeline. It cannot fit in an 8 MiB
    // shared workspace even though the independent frame-workspace limit can.
    for(uint64_t i=0;i<512;++i) timeline.frames.push_back({i,1,{},std::string(32*1024,'x'),true});
    input.frameTimelines.push_back(std::move(timeline));
    CandidatePolicyCacheOptions options;
    options.table.blockBytes=16384; options.table.manifestBytes=65536; options.sortBufferBytes=32768;
    options.frameWorkspaceBytes=512*1024*1024;
    options.table.workspace=std::make_shared<AnalysisWorkspaceBudget>(8*1024*1024,4*1024*1024);
    Reject([&]{BuildCandidatePolicyCache(root/"denied",neutral,input,options);},"workspace_budget");
    Require(!std::filesystem::exists(root/"denied"/"header"/"manifest.json"),"unaffordable Timeline must not publish a partial policy");
    Require(options.table.workspace->Snapshot().currentBytes==0,"Timeline budget denial releases all policy-owned reservations");
    input.frameTimelines[0].frames.resize(1);
    options.table.workspace=std::make_shared<AnalysisWorkspaceBudget>(64*1024*1024,32*1024*1024);
    const auto id=BuildCandidatePolicyCache(root/"small",neutral,input,options);
    Require(options.table.workspace->Snapshot().currentBytes==0,"finished Timeline state returns its workspace");
    CandidatePolicyCacheReader result(root/"small",id,options);
    Require(!result.Candidates(0,1,65536).records.empty(),"bounded Timeline preserves candidate availability");
}
void SharedContextDecodeWorkspace(const std::filesystem::path& root)
{
    const auto input=Fixture(root);
    NeutralStatisticsCacheOptions options;
    options.table.blockBytes=65536; options.table.manifestBytes=65536; options.sortBufferBytes=32768;
    WriteNeutral(root/"neutral",input,options);
    auto budget=std::make_shared<AnalysisWorkspaceBudget>(4*1024*1024,2*1024*1024);
    options.table.workspace=budget;
    {
        NeutralStatisticsCacheReader reader(root/"neutral",Identity,options);
        reader.SignatureAt(0); reader.Contexts(0,1,65536); // Warm both source blocks.
        const auto resident=budget->Snapshot().currentBytes;
        AnalysisWorkspaceReservation pressure(budget,4*1024*1024-resident-128);
        bool decoded=false,rejected=false;
        try { reader.VisitSignatureContexts([&](const auto*,const auto&){decoded=true;}); }
        catch(const std::exception& e) { if(std::string(e.what()).find("workspace_budget")==std::string::npos) throw; rejected=true; }
        Require(rejected && !decoded,"a warmed Context/statistics join must reserve decoded objects before invoking its consumer");
        pressure.Resize(0);
        uint64_t visited=0;
        reader.VisitSignatureContexts([&](const auto* statistic,const auto& context) {
            Require(statistic!=nullptr && !context.path.empty(),"recovered join retains complete statistics and paths"); ++visited;
        });
        Require(visited==12,"failed temporary decode reservation must not advance or poison subsequent joins");
    }
    Require(budget->Snapshot().currentBytes==0,"decoded records and readers release all shared workspace");
}
void SharedPolicyDecodeWorkspace(const std::filesystem::path& root)
{
    auto input=Fixture(root);
    input.signatures[0].path=std::string(1024*1024,'p');
    NeutralStatisticsCacheOptions no; no.table.blockBytes=2*1024*1024; no.sortBufferBytes=4*1024*1024;
    WriteNeutral(root/"neutral",input,no); NeutralStatisticsCacheReader neutral(root/"neutral",Identity,no);
    input.signatures.clear(); input.aggregate.signatures.clear(); input.aggregate.rankings.clear();
    CandidatePolicyCacheOptions options; options.table.blockBytes=2*1024*1024;
    options.table.manifestBytes=65536; options.sortBufferBytes=32768;
    options.table.workspace=std::make_shared<AnalysisWorkspaceBudget>(8*1024*1024,4*1024*1024);
    std::string error;
    try { BuildCandidatePolicyCache(root/"policy",neutral,input,options); }
    catch(const std::exception& e) { error=e.what(); }
    const auto advanced=std::filesystem::exists(root/"policy"/"context-order"/"manifest.json");
    if(error.find("workspace_budget")==std::string::npos || advanced)
        std::cerr<<"policy context decode error="<<error<<" context_pass_completed="<<advanced<<'\n';
    Require(error.find("workspace_budget")!=std::string::npos && !advanced,
        "policy must reserve the current full Context JSON/DTO before completing the first traversal");
    Require(options.table.workspace->Snapshot().currentBytes==0,"early Context decode denial returns all reservations");
}
void SharedSeriesWorkspace(const std::filesystem::path& root)
{
    for(const bool declaredCount:{false,true})
    {
        const auto path=root/(declaredCount?"declared":"loaded");
        auto input=Fixture(path);
        for(auto& c:input.signatures) {
            c.frameSeriesComplete=true; c.frames.clear();
            c.seriesCount=declaredCount?100000:0;
        }
        NeutralStatisticsCacheOptions no; no.table.blockBytes=65536; no.sortBufferBytes=32768;
        WriteNeutral(path/"neutral",input,no);
        NeutralStatisticsCacheReader neutral(path/"neutral",Identity,no);
        input.signatures.clear(); input.aggregate.signatures.clear(); input.aggregate.rankings.clear();
        uint64_t reads=0;
        input.readFrameSeries=[&](const auto&) {
            ++reads; std::vector<PolicyFrameEvidence> series;
            if(!declaredCount) for(uint64_t i=0;i<1024;++i)
                series.push_back({i,0,{},std::string(8192,'x'),true});
            return series;
        };
        CandidatePolicyCacheOptions options; options.table.blockBytes=65536;
        options.table.manifestBytes=65536; options.sortBufferBytes=32768;
        options.table.workspace=std::make_shared<AnalysisWorkspaceBudget>(4*1024*1024,2*1024*1024);
        bool rejected=false;
        try { BuildCandidatePolicyCache(path/"policy",neutral,input,options); }
        catch(const std::exception& e) { if(std::string(e.what()).find("workspace_budget")==std::string::npos) throw; rejected=true; }
        if(!rejected || reads!=(declaredCount?0:1))
            std::cerr<<"unbudgeted policy series: declared="<<declaredCount<<" reads="<<reads<<" rejected="<<rejected<<'\n';
        Require(rejected && reads==(declaredCount?0:1),
            "policy must reserve declared series before loading, or a returned unknown series before any copies and later reads");
        Require(!std::filesystem::exists(path/"policy"/"header"/"manifest.json"),"unaffordable series cannot publish");
        Require(options.table.workspace->Snapshot().currentBytes==0,"failed series workspace returns all policy reservations");
    }
}
void LocalSignalDecodeWorkspace(const std::filesystem::path& root)
{
    auto input=Fixture(root,true);
    for(auto& c:input.signatures) if(c.signatureId=="signature-2")
        c.frames[17].eventRefs={std::string(16384,'r')};
    NeutralStatisticsCacheOptions no; no.table.blockBytes=65536; no.sortBufferBytes=131072;
    WriteNeutral(root/"neutral",input,no); NeutralStatisticsCacheReader neutral(root/"neutral",Identity,no);
    input.signatures.clear(); input.aggregate.signatures.clear(); input.aggregate.rankings.clear();
    CandidatePolicyCacheOptions options; options.table.blockBytes=65536; options.sortBufferBytes=131072;
    auto budget=std::make_shared<AnalysisWorkspaceBudget>(32*1024*1024,16*1024*1024); options.table.workspace=budget;
    AnalysisWorkspaceReservation pressure(budget); bool armed=false;
    options.table.cancelled=[&] {
        if(!armed && std::filesystem::exists(root/"policy"/"local-signals.sorting")) {
            armed=true; const auto current=budget->Snapshot().currentBytes;
            Require(current+1024*1024<=32*1024*1024,"local signal fixture has sufficient setup headroom");
            pressure.Resize(32*1024*1024-current-1024*1024);
        }
        return false;
    };
    std::string error;
    try { BuildCandidatePolicyCache(root/"policy",neutral,input,options); }
    catch(const std::exception& e) { error=e.what(); }
    const auto published=std::filesystem::exists(root/"policy"/"local-signals"/"manifest.json");
    if(error.find("workspace_budget")==std::string::npos || published)
        std::cerr<<"local signal decode error="<<error<<" local_signals_published="<<published<<'\n';
    Require(armed && error.find("workspace_budget")!=std::string::npos && !published,
        "local observation reduction must reserve full current Signal JSON and retained evidence before publication");
    pressure.Resize(0); Require(budget->Snapshot().currentBytes==0,"local Signal decode denial releases every reservation");
}
void RankingDecodeWorkspace(const std::filesystem::path& root)
{
    auto input=Fixture(root);
    const auto id=input.signatures[0].signatureId;
    const auto scope=std::string(32768,'w'); input.signatures[0].frameScope=scope;
    for(auto& stat:input.aggregate.signatures) if(stat.signatureId==id) stat.frameScope=scope;
    NeutralStatisticsCacheOptions no; no.table.blockBytes=1048576; no.sortBufferBytes=2097152;
    WriteNeutral(root/"neutral",input,no); NeutralStatisticsCacheReader neutral(root/"neutral",Identity,no);
    input.signatures.clear(); input.aggregate.signatures.clear(); input.aggregate.rankings.clear();
    CandidatePolicyCacheOptions options; options.table.blockBytes=1048576; options.sortBufferBytes=2097152;
    auto budget=std::make_shared<AnalysisWorkspaceBudget>(64*1024*1024,32*1024*1024); options.table.workspace=budget;
    AnalysisWorkspaceReservation pressure(budget); bool armed=false;
    options.table.cancelled=[&] {
        if(!armed && std::filesystem::exists(root/"policy"/"local-signals"/"manifest.json")) {
            armed=true; const auto current=budget->Snapshot().currentBytes;
            Require(current+3*1024*1024<=64*1024*1024,"policy ranking fixture has sufficient setup headroom");
            pressure.Resize(64*1024*1024-current-3*1024*1024);
        }
        return false;
    };
    std::string error;
    try { BuildCandidatePolicyCache(root/"policy",neutral,input,options); }
    catch(const std::exception& e) { error=e.what(); }
    const auto emitted=std::filesystem::exists(root/"policy"/"ranking-input.sorting"/"run-0"/"manifest.json") ||
        std::filesystem::exists(root/"policy"/"ranking-input"/"manifest.json");
    if(error.find("workspace_budget")==std::string::npos || emitted)
        std::cerr<<"policy ranking decode error="<<error<<" emitted_ranking_run="<<emitted<<'\n';
    Require(armed && error.find("workspace_budget")!=std::string::npos && !emitted,
        "policy ranking preparation must reserve current wide-scope JSON before spilling ranking input");
    pressure.Resize(0); Require(budget->Snapshot().currentBytes==0,"policy ranking denial releases its complete workspace");
}
void CandidateJoinWorkspace(const std::filesystem::path& root)
{
    bool allRejected=true;
    for(const std::string mode:{"modules","context","local","rank","capacity"})
    {
        const auto path=root/mode; auto input=Fixture(path,mode=="local");
        std::erase_if(input.signatures,[](const auto& c){return c.signatureId!="signature-0";});
        std::erase_if(input.aggregate.signatures,[](const auto& s){return s.signatureId!="signature-0";});
        auto& context=input.signatures[0]; context.sourceOrdinal=0; context.familyId="family"; context.name="Focus";
        if(mode=="context") context.path=std::string(65536,'p');
        if(mode=="local") { context.frames[17].valueNs=12'000'000; context.frames[17].eventRefs={std::string(16384,'l')}; }
        if(mode=="rank") { context.frameScope=std::string(32768,'s'); input.aggregate.signatures[0].frameScope=context.frameScope; }
        if(mode=="modules") {
            for(auto& frame:context.frames) frame.eventRefs={std::string(512,'m')};
            auto& modules=input.normalizedProfile["module_budgets"]; modules=json::array();
            for(unsigned i=0;i<32;++i) modules.push_back({{"id","module-"+std::to_string(i)},
                {"scope","per_complete_frame"},{"budget_ms",0.00001},{"status","fixed"},{"marker_rules",{"exact:Focus"}}});
            input.profileIdentity=Sha(input.normalizedProfile);
        }
        if(mode=="capacity") {
            input.signatures.clear(); input.aggregate.signatures.clear();
            input.capacityFacts[0].description=std::string(65536,'c');
        } else input.capacityFacts.clear();
        for(auto& d:input.aggregate.domains) {
            d.present=mode!="capacity" && d.domain=="cpu"; d.status=d.present?"complete":"absent";
            d.inputCount=d.consumedInputCount=d.present?3:0;
            d.outputCount=d.actualOutputCount=d.present?1:0;
        }
        NeutralStatisticsCacheOptions no; no.table.blockBytes=(mode=="rank" || mode=="modules")?1048576:131072; no.sortBufferBytes=2*no.table.blockBytes;
        WriteNeutral(path/"neutral",input,no); NeutralStatisticsCacheReader neutral(path/"neutral",Identity,no);
        input.signatures.clear(); input.aggregate.signatures.clear(); input.aggregate.rankings.clear();
        CandidatePolicyCacheOptions options; options.table.blockBytes=no.table.blockBytes; options.sortBufferBytes=no.sortBufferBytes;
        auto budget=std::make_shared<AnalysisWorkspaceBudget>(64*1024*1024,32*1024*1024); options.table.workspace=budget;
        AnalysisWorkspaceReservation pressure(budget); bool armed=false;
        options.table.cancelled=[&] {
            if(!armed && std::filesystem::exists(path/"policy"/"ranking-input"/"manifest.json")) {
                armed=true; const auto current=budget->Snapshot().currentBytes;
                const uint64_t allowance=((mode=="rank" || mode=="modules")?4:mode=="local"?3:2)*1024*1024;
                Require(current+allowance<64*1024*1024,"join fixture has sufficient setup headroom");
                pressure.Resize(64*1024*1024-current-allowance);
            }
            return false;
        };
        std::string error;
        try { BuildCandidatePolicyCache(path/"policy",neutral,input,options); }
        catch(const std::exception& e) { error=e.what(); }
        const auto emitted=std::filesystem::exists(path/"policy"/"facts.sorting"/"run-0"/"manifest.json") ||
            std::filesystem::exists(path/"policy"/"facts"/"manifest.json");
        const bool rejected=armed && error.find("workspace_budget")!=std::string::npos && !emitted;
        if(!rejected) std::cerr<<"join mode="<<mode<<" error="<<error<<" emitted_facts="<<emitted<<'\n';
        allRejected&=rejected; pressure.Resize(0);
        Require(budget->Snapshot().currentBytes==0,"join denial releases current families and record workspace");
        if(mode=="modules") {
            options.table.cancelled={};
            const auto id=BuildCandidatePolicyCache(path/"recovered",neutral,input,options);
            CandidatePolicyCacheReader reader(path/"recovered",id,options);
            Require(json::parse(reader.SummaryJson()).at("backlog").at("total")==1,"module recovery preserves one merged family");
            const auto candidate=json::parse(reader.CandidateAt(0).payload); unsigned moduleReasons=0;
            for(const auto& evidence:candidate.at("trigger_evidence"))
                moduleReasons+=evidence.at("reason").get<std::string>().starts_with("p95_exceeds_module_budget:");
            Require(moduleReasons==32 && candidate.at("selected")==true && candidate.at("representative_frame_details").size()==3,
                "module recovery must preserve every budget hit and all three representative frames");
        }
    }
    Require(allRejected,"policy join must reserve current context/Signal/ranking/capacity expansion before emitting facts");
}
void CandidatePublicationWorkspace(const std::filesystem::path& root)
{
    bool allRejected=true;
    for(const std::string mode:{"merged","interval","quality"})
    {
        const auto path=root/mode;
        auto input=Fixture(path,mode=="interval");
        if(mode=="merged") for(auto& c:input.signatures) c.path=std::string(65536,'p');
        if(mode=="interval") for(auto& t:input.frameTimelines) if(t.observationUnit=="l0_segment")
            t.frames[0].eventRefs={std::string(65536,'i')};
        if(mode=="quality") { input.aggregate.domains[0].status="invalid";
            input.aggregate.domains[0].unavailableReason=std::string(65536,'q'); }
        NeutralStatisticsCacheOptions no; no.table.blockBytes=131072; no.sortBufferBytes=262144;
        WriteNeutral(path/"neutral",input,no); NeutralStatisticsCacheReader neutral(path/"neutral",Identity,no);
        input.signatures.clear(); input.aggregate.signatures.clear(); input.aggregate.rankings.clear();
        CandidatePolicyCacheOptions options; options.table.blockBytes=131072; options.sortBufferBytes=262144;
        auto budget=std::make_shared<AnalysisWorkspaceBudget>(64*1024*1024,32*1024*1024); options.table.workspace=budget;
        AnalysisWorkspaceReservation pressure(budget); bool armed=false;
        options.table.cancelled=[&] {
            const auto trigger=mode=="quality"?path/"policy"/"candidate-index"/"manifest.json":path/"policy"/"candidate-index.sorting";
            if(!armed && std::filesystem::exists(trigger)) {
                armed=true; const auto current=budget->Snapshot().currentBytes;
                const uint64_t allowance=mode=="quality"?1024*1024:2*1024*1024;
                Require(current+allowance<64*1024*1024,"publication fixture has sufficient setup headroom");
                pressure.Resize(64*1024*1024-current-allowance);
            }
            return false;
        };
        std::string error;
        try { BuildCandidatePolicyCache(path/"policy",neutral,input,options); }
        catch(const std::exception& e) { error=e.what(); }
        const auto published=std::filesystem::exists(path/"policy"/(mode=="quality"?"header":"candidates")/"manifest.json");
        const bool rejected=armed && error.find("workspace_budget")!=std::string::npos && !published;
        if(!rejected) std::cerr<<"publication mode="<<mode<<" error="<<error<<" published="<<published<<'\n';
        allRejected&=rejected;
        pressure.Resize(0); Require(budget->Snapshot().currentBytes==0,"publication denial releases complete current-candidate and quality workspace");
    }
    Require(allRejected,"candidate publication must reserve decoded family, copied L0 interval evidence and quality JSON before publishing");
}
void CandidateValidationWorkspace(const std::filesystem::path& root)
{
    auto input=Fixture(root);
    for(auto& c:input.signatures) c.path=std::string(65536,'p');
    NeutralStatisticsCacheOptions no; no.table.blockBytes=131072; no.sortBufferBytes=262144;
    WriteNeutral(root/"neutral",input,no); NeutralStatisticsCacheReader neutral(root/"neutral",Identity,no);
    input.signatures.clear(); input.aggregate.signatures.clear(); input.aggregate.rankings.clear();
    CandidatePolicyCacheOptions options; options.table.blockBytes=131072; options.sortBufferBytes=262144;
    const auto id=BuildCandidatePolicyCache(root/"policy",neutral,input,options);
    auto budget=std::make_shared<AnalysisWorkspaceBudget>(16*1024*1024,8*1024*1024); options.table.workspace=budget;
    {
        CandidatePolicyCacheReader reader(root/"policy",id,options);
        std::string candidateId; uint64_t candidateOrdinal=0,payloadBytes=0;
        const auto count=json::parse(reader.SummaryJson()).at("backlog").at("total").get<uint64_t>();
        for(uint64_t i=0;i<count;++i) {
            auto record=reader.CandidateAt(i);
            if(record.payload.size()>payloadBytes) {
                payloadBytes=record.payload.size(); candidateOrdinal=i;
                candidateId=json::parse(record.payload).at("candidate_id").get<std::string>();
            }
        }
        Require(payloadBytes>65536,"candidate validation fixture preserves the complete wide path");
        { auto warm=reader.Candidate(candidateId); Require(warm.has_value(),"candidate lookup warms both required blocks"); }
        const auto resident=budget->Snapshot().currentBytes;
        const auto allowance=65536+4*payloadBytes;
        Require(resident+allowance<16*1024*1024,"candidate validation fixture leaves record-copy headroom");
        AnalysisWorkspaceReservation pressure(budget,16*1024*1024-resident-allowance);
        { auto raw=reader.CandidateAt(candidateOrdinal); Require(raw.payload.size()==payloadBytes,"plain budgeted record remains affordable"); }
        Reject([&]{reader.Candidate(candidateId);},"workspace_budget");
        pressure.Resize(0);
        Require(reader.Candidate(candidateId).has_value(),"candidate lookup recovers after decode pressure is removed");
    }
    Require(budget->Snapshot().currentBytes==0,"candidate validation and reader destruction release all reservations");
}
void HeaderDecodeWorkspace(const std::filesystem::path& root)
{
    auto input=Fixture(root); input.aggregate.domains[0].status="invalid";
    input.aggregate.domains[0].unavailableReason=std::string(65536,'q');
    NeutralStatisticsCacheOptions no; no.table.blockBytes=131072; no.sortBufferBytes=262144;
    WriteNeutral(root/"neutral",input,no); NeutralStatisticsCacheReader neutral(root/"neutral",Identity,no);
    input.signatures.clear(); input.aggregate.signatures.clear(); input.aggregate.rankings.clear();
    CandidatePolicyCacheOptions options; options.table.blockBytes=131072; options.sortBufferBytes=262144;
    const auto id=BuildCandidatePolicyCache(root/"policy",neutral,input,options);
    options.table.workspace=std::make_shared<AnalysisWorkspaceBudget>(2*1024*1024,1024*1024);
    Reject([&]{CandidatePolicyCacheReader reader(root/"policy",id,options);},"workspace_budget");
    Require(options.table.workspace->Snapshot().currentBytes==0,"candidate header denial releases every reader reservation");
    options.table.workspace=std::make_shared<AnalysisWorkspaceBudget>(8*1024*1024,4*1024*1024);
    { CandidatePolicyCacheReader reader(root/"policy",id,options);
      Require(json::parse(reader.SummaryJson()).at("capture_quality").at(0).at("reason")==std::string(65536,'q'),
          "quality remains complete and separate from candidate selection after budgeted reopen"); }
    Require(options.table.workspace->Snapshot().currentBytes==0,"candidate cached summary releases its retained budget");
}
void RetainedFamilyWorkspace(const std::filesystem::path& root)
{
    auto input=Fixture(root);
    for(auto& c:input.signatures) {
        c.familyId="shared-family"; c.name="Focus";
        for(auto& frame:c.frames) frame.eventRefs={c.signatureId+":"+std::to_string(frame.frameIndex)+std::string(8192,'e')};
    }
    NeutralStatisticsCacheOptions no; no.table.blockBytes=1024*1024; no.sortBufferBytes=1024*1024;
    WriteNeutral(root/"neutral",input,no); NeutralStatisticsCacheReader neutral(root/"neutral",Identity,no);
    input.signatures.clear(); input.aggregate.signatures.clear(); input.aggregate.rankings.clear();
    CandidatePolicyCacheOptions options; options.table.blockBytes=1024*1024; options.sortBufferBytes=1024*1024;
    options.table.manifestBytes=65536; options.familyBytes=8*1024*1024;
    options.table.workspace=std::make_shared<AnalysisWorkspaceBudget>(64*1024*1024,32*1024*1024);
    std::string error;
    try { BuildCandidatePolicyCache(root/"policy",neutral,input,options); }
    catch(const std::exception& e) { error=e.what(); }
    const auto facts=std::filesystem::exists(root/"policy"/"facts"/"manifest.json");
    const auto priority=std::filesystem::exists(root/"policy"/"priority"/"manifest.json");
    if(error.find("workspace_budget")==std::string::npos || !facts || priority)
        std::cerr<<"family workspace error="<<error<<" facts="<<facts<<" priority="<<priority<<'\n';
    Require(error.find("workspace_budget")!=std::string::npos && facts && !priority,
        "current family must reserve merged full representative evidence before priority output is published");
    Require(options.table.workspace->Snapshot().currentBytes==0,"family merge denial releases all retained input fragments");
}
void RetainedTopWorkspace(const std::filesystem::path& root)
{
    CandidatePolicyInput input; input.aggregateIdentity=Identity;
    input.normalizedProfile={{"frame_budget",{{"frame_ms",16.666667}}},{"module_budgets",json::array()},
        {"resource_budgets",json::object()},{"user_focus",json::array()},
        {"candidate_policy",{{"top_n",1},{"local_top_n",256},{"cumulative_contribution",0.8},{"per_domain_limit",10},
            {"absolute_frame_cost_ms",10.0},{"priorities",{"P0","P1","P2","P3","P4"}}}}};
    input.profileIdentity=Sha(input.normalizedProfile);
    NeutralStatisticsInput raw; raw.temporaryRoot=root/"oracle";
    for(uint64_t i=0;i<256;++i) {
        PolicySignatureContext c; c.domain="cpu"; c.signatureId=std::to_string(i)+std::string(8192,'s');
        c.frameScope="Player"; c.threadOrQueue="shared"; c.frames={{0,300000,{},"",true}};
        c.sourceOrdinal=i; c.frameSeriesComplete=true;
        raw.runs.push_back({"cpu",c.signatureId,"Player",0,300000,300000,0,0,1,true,false});
        raw.denominators.push_back({"cpu",c.signatureId,"Player",1}); input.signatures.push_back(std::move(c));
    }
    raw.domainAudit.push_back({"cpu",true,"complete",256,256,256,Identity,Identity,true,{}});
    input.aggregate=BuildNeutralStatistics(raw); input.frameTimelines.push_back({"Player","Player",{{0,20000000,{},"",true}},"frame"});
    NeutralStatisticsCacheOptions no; no.table.blockBytes=65536; no.table.manifestBytes=1024*1024; no.sortBufferBytes=262144;
    WriteNeutral(root/"neutral",input,no); NeutralStatisticsCacheReader neutral(root/"neutral",Identity,no);
    input.signatures.clear(); input.aggregate.signatures.clear(); input.aggregate.rankings.clear();
    CandidatePolicyCacheOptions options; options.table.blockBytes=65536; options.table.manifestBytes=1024*1024;
    options.sortBufferBytes=262144; options.table.workspace=std::make_shared<AnalysisWorkspaceBudget>(8*1024*1024,4*1024*1024);
    bool rejected=false;
    try { BuildCandidatePolicyCache(root/"policy",neutral,input,options); }
    catch(const std::exception& e) { if(std::string(e.what()).find("workspace_budget")==std::string::npos) throw; rejected=true; }
    Require(rejected && std::filesystem::exists(root/"policy"/"local-top-input"/"manifest.json") &&
        !std::filesystem::exists(root/"policy"/"local-observations"/"manifest.json"),
        "cross-signature Top reservoir must reserve retained full identity/evidence before observations publish");
    Require(options.table.workspace->Snapshot().currentBytes==0,"Top reservoir denial releases retained and transient entries");
}
void RepeatedLocalTops(const std::filesystem::path& root)
{
    CandidatePolicyInput input; input.aggregateIdentity=Identity;
    input.normalizedProfile={{"frame_budget",{{"frame_ms",16.666667}}},{"module_budgets",json::array()},
        {"resource_budgets",json::object()},{"user_focus",json::array()},
        {"candidate_policy",{{"top_n",1},{"cumulative_contribution",0.8},{"per_domain_limit",10},
            {"absolute_frame_cost_ms",10.0},{"priorities",{"P0","P1","P2","P3","P4"}}}}};
    input.profileIdentity=Sha(input.normalizedProfile);
    NeutralStatisticsInput raw; raw.temporaryRoot=root/"oracle";
    PolicySignatureContext c; c.domain="cpu"; c.signatureId="constant-work"; c.frameScope="Player";
    c.frameSeriesComplete=true; c.sourceOrdinal=0; c.threadOrQueue="thread";
    PolicyFrameTimeline timeline; timeline.frameScope="Player";
    for(uint64_t f=0;f<400;++f) {
        c.frames.push_back({f,300000,{},"",true}); timeline.frames.push_back({f,20000000,{},"",true});
        raw.runs.push_back({"cpu",c.signatureId,"Player",f,300000,300000,0,0,1,true,false}); }
    raw.denominators.push_back({"cpu",c.signatureId,"Player",400});
    raw.domainAudit.push_back({"cpu",true,"complete",400,400,1,Identity,Identity,true,{}});
    input.aggregate=BuildNeutralStatistics(raw); input.signatures.push_back(c); input.frameTimelines.push_back(timeline);
    const auto expected=json::parse(SerializeCandidatePolicyResult(EvaluateCandidatePolicy(input)));
    NeutralStatisticsCacheOptions no; no.table.blockBytes=131072; no.sortBufferBytes=262144;
    WriteNeutral(root/"neutral",input,no); NeutralStatisticsCacheReader neutral(root/"neutral",Identity,no);
    input.signatures.clear(); input.aggregate.signatures.clear(); input.aggregate.rankings.clear();
    CandidatePolicyCacheOptions options; options.table.blockBytes=131072; options.sortBufferBytes=262144; options.familyBytes=16384;
    const auto identity=BuildCandidatePolicyCache(root/"policy",neutral,input,options);
    CandidatePolicyCacheReader cache(root/"policy",identity,options); json actual=json::array();
    for(const auto& row:cache.Candidates(0,10,131072).records) actual.push_back(json::parse(row.payload));
    Require(actual==expected.at("candidates"),"many equal local Tops keep bounded state and exact representatives, not cumulative input bytes");
}
std::string ScaleId(uint64_t i) { return "scale-"+std::to_string(10000000000000000ull+i); }
uint64_t ScaleIndex(const std::string& id,uint64_t count)
{
    Require(id.starts_with("scale-"),"scale ID prefix");
    const auto i=std::stoull(id.substr(6))-10000000000000000ull;
    Require(i<count && ScaleId(i)==id,"scale ID range and canonical representation"); return i;
}
uint64_t DirectoryBytes(const std::filesystem::path& root)
{ uint64_t bytes=0; for(const auto& f:std::filesystem::recursive_directory_iterator(root)) if(f.is_regular_file()) bytes+=f.file_size(); return bytes; }
void Scale(const std::filesystem::path& root,uint64_t count)
{
    Require(count>0 && count<=2000000,"scale count range");
    const auto start=std::chrono::steady_clock::now(); NeutralStatisticsStreamSummary summary;
    {
        NeutralStatisticsStreamBuilder source(root/"runs"); NeutralStatisticsCacheWriter cache(root/"neutral",Identity);
        for(uint64_t i=0;i<count;++i)
        {
            const auto id=ScaleId(i); const int64_t value=1'000'000+(i%97)*10000;
            Require(source.AddRun({"cpu",id,"Player",0,value,value/2,value/5,value/3,1,true,false}),"scale raw input");
            source.AddDenominator({"cpu",id,"Player",5});
        }
        source.AddDomainAudit({"cpu",true,"complete",count,count,count,Identity,Identity,true,{}});
        std::string error;
        const auto ok=source.FinishToSink(summary,error,[&](const auto& stat,const auto& frames) {
            cache.AppendStatistics(stat); Require(frames.size()==1,"one exact sparse scale frame");
            PolicySignatureContext c; c.domain=stat.domain; c.signatureId=stat.signatureId; c.frameScope=stat.frameScope;
            c.name="Scale.Work"; c.path="Player/Thread/"+c.signatureId; c.threadOrQueue="thread:1";
            c.metricPreference="inclusive"; c.sourceOrdinal=ScaleIndex(c.signatureId,count); c.frameSeriesComplete=true;
            c.frames={{0,frames[0].inclusiveNs,{"event:"+c.signatureId},"structure",true}};
            cache.AppendContext(SerializePolicySignatureContextRecord(c));
        });
        Require(ok,error.c_str()); Require(summary.materializedResultPeak==0,"scale statistics streamed without retaining result vector");
        std::cerr<<"scale statistics emitted; building neutral rankings\n"; cache.Commit(summary);
    }
    const auto neutralBuilt=std::chrono::steady_clock::now();
    CandidatePolicyInput metadata; metadata.aggregateIdentity=Identity; metadata.aggregate.qualityComplete=true;
    metadata.aggregate.domains=summary.domains; metadata.normalizedProfile={
        {"frame_budget",{{"frame_ms",16.666667}}},{"module_budgets",json::array()},
        {"resource_budgets",json::object()},{"user_focus",json::array()},
        {"candidate_policy",{{"top_n",5},{"cumulative_contribution",0.8},{"per_domain_limit",50},
            {"priorities",{"P0","P1","P2","P3","P4"}}}}};
    metadata.profileIdentity=Sha(metadata.normalizedProfile);
    PolicyFrameTimeline timeline; timeline.frameScope="Player"; timeline.name="Player";
    for(uint64_t f=0;f<5;++f) timeline.frames.push_back({f,f==0?20'000'000:12'000'000,{"wall:"+std::to_string(f)},"",true});
    metadata.frameTimelines.push_back(timeline);
    std::string identity;
    {
        NeutralStatisticsCacheReader neutral(root/"neutral",Identity);
        metadata.aggregate.contentSha256=neutral.ContentSha256();
        std::cerr<<"scale neutral cache ready; building global/local candidate cache\n";
        identity=BuildCandidatePolicyCache(root/"policy",neutral,metadata);
    }
    const auto policyBuilt=std::chrono::steady_clock::now();
    std::cerr<<"scale candidate cache published; reopening and verifying every candidate/ranking\n";
    uint64_t candidateCount=0,rankedCount=0,globalSelected=0,localSelected=0;
    json cacheSummary;
    {
        CandidatePolicyCacheReader cache(root/"policy",identity);
        cacheSummary=json::parse(cache.SummaryJson());
        // Verification only: <=2 MB even at the maximum synthetic count.
        std::vector<uint8_t> seen(size_t(count),0);
        for(uint64_t ordinal=0;;)
        {
            auto page=cache.Candidates(ordinal,128,4*1024*1024);
            for(const auto& record:page.records)
            {
                const auto c=json::parse(record.payload); const auto id=c.at("signature_id").get<std::string>();
                const auto i=ScaleIndex(id,count); const bool local=c.at("manifestation")=="frame_cost";
                const auto bit=uint8_t(local?1:2); Require(!(seen[size_t(i)]&bit),"scale candidate unique per global/local family"); seen[size_t(i)]|=bit;
                const auto family=local?"cpu:"+id+":Player:thread:1:inclusive:frame_cost":"cpu:"+id;
                Require(c.at("family_id")==family && c.at("structural_signature")=="Player/Thread/"+id,"scale exact family and full structural path");
                Require(c.at("member_signatures")==json::array({"cpu:"+id}),"scale candidate retains exact membership");
                Require(c.at("representative_frames")==(local?json::array({"0","1"}):json::array({"0"})),
                    "scale representative links input frame and known-zero lower-load reference");
                Require(c.at("observation_unit")=="frame" && c.at("representative_intervals").empty(),"scale frame evidence unit unchanged");
                if(local) { Require(c.at("selected").get<bool>(),"local candidates are mandatory outside global quota"); ++localSelected; }
                else globalSelected+=c.at("selected").get<bool>();
                ++candidateCount;
            }
            ordinal=page.nextOrdinal; Require(ordinal==candidateCount,"scale candidate page continuity"); if(page.done) break;
        }
        Require(candidateCount==count*2 && localSelected==count,"all global and local candidates preserved");
        Require(globalSelected==std::min<uint64_t>(50,count),"global discretionary quota applied across all signatures");
        Require(std::all_of(seen.begin(),seen.end(),[](auto bits){return bits==3;}),"scale no omitted signature families");
        std::string previousMetric,previousId; int64_t previousTotal=INT64_MAX; uint64_t rank=0; double previousCumulative=0;
        for(uint64_t ordinal=0;;)
        {
            auto page=cache.RankedSignatures(ordinal,128,4*1024*1024);
            for(const auto& record:page.records)
            {
                const auto row=json::parse(record.payload); const auto metric=row.at("ranking").get<std::string>();
                if(metric!=previousMetric) {
                    if(!previousMetric.empty()) Require(rank==count && previousCumulative==1,"previous ranking group complete");
                    previousMetric=metric; previousId.clear(); previousTotal=INT64_MAX; rank=0; previousCumulative=0;
                }
                const auto id=row.at("signature_id").get<std::string>(); const auto i=ScaleIndex(id,count);
                const int64_t value=1'000'000+(i%97)*10000;
                const auto expected=metric=="inclusive"?value:metric=="exclusive"?value/2:value/3;
                const auto total=std::stoll(row.at("total_ns").get<std::string>());
                Require(total==expected && (total<previousTotal || (total==previousTotal && (previousId.empty() || previousId<id))),"scale ranking total and stable tie order");
                Require(row.at("rank").get<uint64_t>()==++rank,"scale ranking ordinal");
                const auto cumulative=row.at("cumulative_contribution").get<double>();
                Require(cumulative>=previousCumulative && cumulative<=1,"scale cumulative monotonicity");
                previousCumulative=cumulative; previousTotal=total; previousId=id; ++rankedCount;
            }
            ordinal=page.nextOrdinal; Require(ordinal==rankedCount,"scale ranked page continuity"); if(page.done) break;
        }
        Require(rank==count && previousCumulative==1 && rankedCount==count*3,"scale all three complete ranking groups");
        for(const auto i:{uint64_t(0),count/2,count-1})
        {
            const auto family="cpu:"+ScaleId(i); const auto text=identity+"\n"+family;
            Sha256Builder hash; hash.Update(text.data(),text.size());
            const auto lookup=cache.Candidate("candidate:"+hash.FinalHex());
            Require(lookup && json::parse(lookup->payload).at("family_id")==family,"scale indexed lookup after full pagination");
        }
        Require(cacheSummary.at("backlog").at("total").get<uint64_t>()==candidateCount &&
            cacheSummary.at("backlog").at("selected").get<uint64_t>()==globalSelected+localSelected,"scale small backlog verified");
    }
    const auto end=std::chrono::steady_clock::now();
    const auto ms=[](auto a,auto b){return std::chrono::duration_cast<std::chrono::milliseconds>(b-a).count();};
    std::cout<<json{{"mode","candidate-cache"},{"records",count},{"candidate_records",candidateCount},{"ranking_records",rankedCount},
        {"neutral_build_ms",ms(start,neutralBuilt)},{"policy_build_ms",ms(neutralBuilt,policyBuilt)},{"reopen_verify_ms",ms(policyBuilt,end)},
        {"neutral_cache_bytes",DirectoryBytes(root/"neutral")},{"policy_cache_bytes",DirectoryBytes(root/"policy")},
        {"materialized_statistics_peak",summary.materializedResultPeak},{"retained_final_candidate_peak",cacheSummary.at("retained_final_candidate_peak")},
        {"global_selected",globalSelected},{"local_selected",localSelected},{"verified",true}}.dump()<<'\n';
}
}
int main(int argc,char** argv)
{
    const auto root=std::filesystem::temp_directory_path()/("jn-policy-cache-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        if(argc==3 && std::string(argv[1])=="--scale-policy") { Scale(root,std::stoull(argv[2])); std::filesystem::remove_all(root); return 0; }
        CandidateValidationWorkspace(root/"candidate-validation");
        CandidatePublicationWorkspace(root/"candidate-publication");
        CandidateJoinWorkspace(root/"candidate-join");
        LocalSignalDecodeWorkspace(root/"signal-workspace"); RankingDecodeWorkspace(root/"ranking-workspace"); HeaderDecodeWorkspace(root/"header-workspace"); RetainedFamilyWorkspace(root/"family-workspace"); RetainedTopWorkspace(root/"top-workspace"); SharedPolicyDecodeWorkspace(root/"policy-decode"); SharedSeriesWorkspace(root/"shared-series"); SharedContextDecodeWorkspace(root/"shared-decode"); SharedTimelineWorkspace(root/"shared-timeline"); CachedParity(root/"global"); CachedParity(root/"local",true); CachedParity(root/"fallback",true,true); Boundaries(root/"boundaries"); RepeatedLocalTops(root/"repeated-tops"); std::filesystem::remove_all(root); std::cout<<"candidate policy cache tests passed\n"; return 0; }
    catch(const std::exception& e) { std::cerr<<e.what()<<"\nfixture: "<<root<<'\n'; return 1; }
}

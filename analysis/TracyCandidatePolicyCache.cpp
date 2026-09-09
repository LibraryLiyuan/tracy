#include "TracyCandidatePolicyCache.hpp"
#include "TracyCandidatePolicyInternal.hpp"
#include "TracyFrameWindowPolicy.hpp"
#include "TracyAnalysisCacheSort.hpp"
#include "TracyAnalysisCacheKey.hpp"
#include <algorithm>
#include <map>
#include <stdexcept>
#include <tuple>
namespace tracy::analysis
{
namespace
{
using nlohmann::json;
using candidate_policy::FamilyWork;
using candidate_policy::Families;
using cache_key::Field;
using cache_key::Unsigned;
using cache_key::Hex;
using cache_key::Unhex;
constexpr uint32_t Schema=1;
[[noreturn]] void Fail(const char* reason) { throw std::runtime_error(std::string("candidate_policy_cache_")+reason); }
void Check(const CandidatePolicyCacheOptions& o)
{ if(o.table.cancelled && o.table.cancelled()) Fail("cancelled"); }
std::string Kind(const char* name) { return std::string("candidate-policy-cache-v1-")+name; }
json Description(const AnalysisCacheTableDescriptor& d)
{ return {{"records",d.records},{"blocks",d.blocks},{"content_sha256",d.contentSha256}}; }
uint64_t U64(const json& j) { return std::stoull(j.get<std::string>()); }
std::string SignatureKey(const PolicySignatureContext& c)
{ return cache_key::Signature(c.domain,c.signatureId,c.frameScope); }
std::string SignatureKey(const json& j)
{ return cache_key::Signature(j.at("domain").get<std::string>(),j.at("signature_id").get<std::string>(),j.at("frame_scope").get<std::string>()); }
uint64_t MetricOrdinal(const std::string& metric)
{
    if(metric=="inclusive") return 0;
    if(metric=="exclusive") return 1;
    if(metric=="wait_critical") return 2;
    Fail("ranking_metric");
}
NeutralRankingMetric Metric(const std::string& metric)
{
    switch(MetricOrdinal(metric)) {
    case 0:return NeutralRankingMetric::Inclusive;
    case 1:return NeutralRankingMetric::Exclusive;
    default:return NeutralRankingMetric::WaitCritical;
    }
}
json FramesJson(const std::vector<CandidateRepresentativeFrame>& frames)
{
    json result=json::array();
    for(const auto& f:frames) result.push_back({{"frame",std::to_string(f.frameIndex)},{"reasons",f.reasons},{"event_refs",f.eventRefs}});
    return result;
}
std::vector<CandidateRepresentativeFrame> ParseFrames(const json& rows)
{
    std::vector<CandidateRepresentativeFrame> result;
    for(const auto& f:rows) result.push_back({U64(f.at("frame")),f.at("reasons").get<std::vector<std::string>>(),f.at("event_refs").get<std::vector<std::string>>()});
    return result;
}
CandidateTrigger Trigger(const std::string& name)
{
    for(unsigned i=0;i<=unsigned(CandidateTrigger::WindowLocal);++i)
        if(name==CandidateTriggerName(CandidateTrigger(i))) return CandidateTrigger(i);
    Fail("trigger_record");
}
CandidateFamily ParseCandidate(const json& j)
{
    CandidateFamily c;
    c.candidateId=j.at("candidate_id").get<std::string>(); c.familyId=j.at("family_id").get<std::string>();
    const auto p=j.at("priority").get<std::string>();
    if(p.size()!=2 || p[0]!='P' || p[1]<'0' || p[1]>'4') Fail("priority_record");
    c.priority=CandidatePriority(p[1]-'0');
    c.domain=j.at("domain").get<std::string>(); c.signatureId=j.at("signature_id").get<std::string>();
    c.structuralSignature=j.at("structural_signature").get<std::string>();
    c.memberSignatures=j.at("member_signatures").get<std::vector<std::string>>();
    for(const auto& t:j.at("triggers")) c.triggers.push_back(Trigger(t.get<std::string>()));
    for(const auto& e:j.at("trigger_evidence"))
    {
        CandidateTriggerEvidence value; value.trigger=Trigger(e.at("trigger").get<std::string>());
        value.metric=e.at("metric").get<std::string>(); value.scope=e.at("scope").get<std::string>();
        value.reason=e.at("reason").get<std::string>(); value.observedExact=e.at("observed_value").get<std::string>();
        value.observedValue=std::stod(value.observedExact);
        if(!e.at("threshold_value").is_null()) {
            value.thresholdExact=e.at("threshold_value").get<std::string>();
            value.thresholdValue=std::stod(*value.thresholdExact);
        }
        value.unit=e.at("unit").get<std::string>(); value.authority=e.at("authority").get<std::string>();
        value.eligible=e.at("eligible").get<bool>(); c.triggerEvidence.push_back(std::move(value));
    }
    c.selected=j.at("selected").get<bool>();
    if(!j.at("not_selected_reason").is_null()) c.notSelectedReason=j.at("not_selected_reason").get<std::string>();
    c.qualityStatus=j.at("quality_status").get<std::string>(); c.frameScope=j.at("frame_scope").get<std::string>();
    c.threadOrQueue=j.at("thread_or_queue").get<std::string>(); c.metric=j.at("metric").get<std::string>();
    c.manifestation=j.at("manifestation").get<std::string>(); c.localEvidence=j.at("local_evidence");
    c.observationUnit=j.at("observation_unit").get<std::string>();
    c.representativeIntervals=j.at("representative_intervals"); c.frameRoot=j.at("frame_root").get<bool>();
    return c;
}
std::string Encode(const FamilyWork& work)
{
    std::vector<CandidateRepresentativeFrame> pending;
    for(const auto& [_,f]:work.representatives) pending.push_back(f);
    return json{{"candidate",candidate_policy::CandidateJson(work.candidate)},
        {"eligible",work.policyEligible},{"mandatory",work.mandatory},{"best_rank",std::to_string(work.bestTopRank)},
        {"context_written",work.contextWritten},{"local_written",work.localWritten},
        {"pending_frames",FramesJson(pending)},{"final_frames",FramesJson(work.candidate.representativeFrames)}}.dump();
}
FamilyWork Decode(const std::string& payload)
{
    const auto j=json::parse(payload); FamilyWork w; w.candidate=ParseCandidate(j.at("candidate"));
    w.policyEligible=j.at("eligible").get<bool>(); w.mandatory=j.at("mandatory").get<bool>();
    w.bestTopRank=U64(j.at("best_rank")); w.contextWritten=j.at("context_written").get<bool>();
    w.localWritten=j.at("local_written").get<bool>();
    for(auto& f:ParseFrames(j.at("pending_frames"))) w.representatives.emplace(f.frameIndex,std::move(f));
    w.candidate.representativeFrames=ParseFrames(j.at("final_frames")); return w;
}
void Merge(FamilyWork& target,FamilyWork source)
{
    auto& a=target.candidate; auto& b=source.candidate;
    if(a.familyId.empty()) { target=std::move(source); return; }
    if(a.familyId!=b.familyId) Fail("family_join");
    if(b.priority<a.priority || (b.priority==a.priority && std::tie(b.domain,b.signatureId)<std::tie(a.domain,a.signatureId)))
    {
        a.priority=b.priority; a.domain=b.domain; a.signatureId=b.signatureId;
        // AddSignal resets this on a winning identity even without a Context.
        a.structuralSignature=b.signatureId;
    }
    a.memberSignatures.insert(a.memberSignatures.end(),b.memberSignatures.begin(),b.memberSignatures.end());
    a.triggers.insert(a.triggers.end(),b.triggers.begin(),b.triggers.end());
    a.triggerEvidence.insert(a.triggerEvidence.end(),std::make_move_iterator(b.triggerEvidence.begin()),std::make_move_iterator(b.triggerEvidence.end()));
    target.policyEligible|=source.policyEligible; target.mandatory|=source.mandatory;
    target.bestTopRank=std::min(target.bestTopRank,source.bestTopRank);
    if(source.contextWritten || source.localWritten)
    {
        a.structuralSignature=b.structuralSignature; a.frameScope=b.frameScope;
        a.threadOrQueue=b.threadOrQueue; a.observationUnit=b.observationUnit; a.metric=b.metric;
    }
    if(source.contextWritten) a.frameRoot=b.frameRoot;
    if(source.localWritten) { a.manifestation=b.manifestation; a.localEvidence=std::move(b.localEvidence); }
    target.contextWritten|=source.contextWritten; target.localWritten|=source.localWritten;
    for(auto& [frame,f]:source.representatives)
    {
        auto& out=target.representatives[frame]; out.frameIndex=frame;
        out.reasons.insert(out.reasons.end(),f.reasons.begin(),f.reasons.end());
        out.eventRefs.insert(out.eventRefs.end(),f.eventRefs.begin(),f.eventRefs.end());
    }
}
std::string PriorityKey(const FamilyWork& w)
{ return Unsigned(uint64_t(w.candidate.priority))+Unsigned(w.policyEligible?0:1)+Unsigned(w.bestTopRank)+Field(w.candidate.familyId); }
NeutralRankingEntry Entry(const json& j)
{
    NeutralRankingEntry e; e.signatureId=j.at("signature_id").get<std::string>(); e.frameScope=j.at("frame_scope").get<std::string>();
    e.logical=j.at("logical").get<bool>(); e.totalNs=std::stoll(j.at("total_ns").get<std::string>());
    e.contribution=std::stod(j.at("contribution").get<std::string>());
    e.cumulativeContribution=std::stod(j.at("cumulative_contribution").get<std::string>());
    return e;
}
uint64_t FrameBytes(const std::vector<PolicyFrameEvidence>& frames)
{
    uint64_t bytes=frames.size()*256;
    for(const auto& f:frames) { bytes+=f.structureKey.size()*2; for(const auto& ref:f.eventRefs) bytes+=64+ref.size()*2; }
    return bytes;
}
uint64_t ContextBytes(const PolicySignatureContext& context)
{
    uint64_t bytes=2048+FrameBytes(context.frames);
    for(const auto* value:{&context.domain,&context.signatureId,&context.familyId,&context.parentSignatureId,
        &context.name,&context.path,&context.frameScope,&context.metricPreference,&context.moduleBudgetId,
        &context.budgetScope,&context.threadOrQueue,&context.seriesSha256,&context.observationUnit}) bytes+=2*value->size();
    return bytes;
}
json SignalJson(const frame_window::Signal& s)
{
    PolicySignatureContext carrier; carrier.frames=s.representatives;
    return {{"context",s.context},{"manifestation",s.manifestation},{"reason",s.reason},
        {"value_ns",std::to_string(s.valueNs)},{"evidence",s.evidence},
        {"frames",json::parse(SerializePolicySignatureContextRecord(carrier)).at("frames")}};
}
frame_window::Signal ParseSignal(const json& j)
{
    PolicySignatureContext carrier; auto context=json::parse(SerializePolicySignatureContextRecord(carrier));
    context["frames"]=j.at("frames"); std::string error;
    if(!DeserializePolicySignatureContextRecord(context.dump(),carrier,error)) throw std::runtime_error(error);
    frame_window::Signal s; s.context=j.at("context").get<size_t>(); s.manifestation=j.at("manifestation").get<std::string>();
    s.reason=j.at("reason").get<std::string>(); s.valueNs=std::stoll(j.at("value_ns").get<std::string>());
    s.evidence=j.at("evidence"); s.representatives=std::move(carrier.frames); return s;
}
json TopJson(const frame_window::TopItem& t)
{
    return {{"context",t.context},{"frame",t.frame},{"cost",std::to_string(t.cost)},
        {"manifestation",t.manifestation},{"evidence",t.evidence},{"signature",t.signature},{"source_key",Hex(t.sourceKey)}};
}
frame_window::TopItem ParseTop(const json& j)
{
    return {std::stoll(j.at("cost").get<std::string>()),j.at("context").get<size_t>(),j.at("frame").get<uint64_t>(),
        j.at("manifestation").get<std::string>(),j.at("evidence"),j.at("signature").get<std::string>(),Unhex(j.at("source_key").get<std::string>())};
}
struct Builder
{
    const std::filesystem::path& root;
    NeutralStatisticsCacheReader& neutral;
    const CandidatePolicyInput& input;
    CandidatePolicyCacheOptions options;
    std::string identity;
    std::unique_ptr<AnalysisCacheSortedWriter> facts,rankInput,ranked,contextOrder;
    uint64_t fragments=0;
    Builder(const std::filesystem::path& path,NeutralStatisticsCacheReader& source,const CandidatePolicyInput& metadata,CandidatePolicyCacheOptions config)
        :root(path),neutral(source),input(metadata),options(std::move(config))
    {
        const auto callerCancel=options.table.cancelled;
        options.table.cancelled=[&,callerCancel] { return (callerCancel && callerCancel()) || (input.cancelled && input.cancelled()); };
        Check(options);
        if(!input.signatures.empty() || !input.aggregate.signatures.empty() || !input.aggregate.rankings.empty()) Fail("materialized_input");
        if(options.familyBytes<1024 || options.familyBytes>64*1024*1024) Fail("family_budget");
        if(options.frameWorkspaceBytes<4096 || options.frameWorkspaceBytes>512*1024*1024) Fail("frame_workspace_budget");
        if(input.normalizedProfile.dump().size()>options.table.manifestBytes/4) Fail("profile_budget");
        std::string error; if(!candidate_policy::ValidateInput(input,error)) throw std::runtime_error(error);
        if(input.aggregateIdentity!=neutral.Identity()) Fail("neutral_identity_mismatch");
        identity=candidate_policy::Sha256Text(std::string(CandidatePolicyAlgorithmId)+"\n"+input.aggregateIdentity+"\n"+input.profileIdentity);
        if(root.filename().empty() || std::filesystem::exists(root)) Fail("already_exists");
        std::filesystem::create_directories(root.parent_path());
        if(!std::filesystem::create_directory(root)) Fail("already_exists");
        facts=Sorter("facts"); rankInput=Sorter("ranking-input"); ranked=Sorter("ranked-signatures"); contextOrder=Sorter("context-order");
    }
    std::unique_ptr<AnalysisCacheSortedWriter> Sorter(const char* name)
    { return std::make_unique<AnalysisCacheSortedWriter>(root/name,identity,Kind(name),options.table,options.sortBufferBytes); }
    AnalysisCacheTableReader Reader(const char* name)
    { return AnalysisCacheTableReader(root/name,identity,Kind(name),options.table); }
    void Emit(Families& families,const std::string& order)
    {
        for(const auto& [family,work]:families)
        {
            Check(options); const auto payload=Encode(work);
            if(payload.size()>options.familyBytes) Fail("family_budget");
            facts->Append(Field(family)+order,payload); ++fragments;
        }
        families.clear();
    }
    template<class Visitor> uint64_t VisitContexts(Visitor visit)
    {
        uint64_t visited=0;
        for(uint64_t ordinal=0;;)
        {
            auto page=neutral.Contexts(ordinal,128,options.table.blockBytes);
            for(const auto& row:page.records)
            {
                Check(options);
                AnalysisWorkspaceReservation decodedWorkspace(options.table.workspace,
                    65536+32ull*(row.key.size()+row.payload.size()));
                PolicySignatureContext c; std::string error;
                if(!DeserializePolicySignatureContextRecord(row.payload,c,error)) throw std::runtime_error(error);
                if(row.key!=SignatureKey(c)) Fail("context_key");
                if(!c.sourceOrdinal) Fail("context_order_missing");
                visit(c); ++visited;
            }
            ordinal=page.nextOrdinal; if(page.done) break;
        }
        return visited;
    }
    void PrepareLocals()
    {
        AnalysisWorkspaceReservation timelineWorkspace(options.table.workspace),rootsWorkspace(options.table.workspace);
        uint64_t timelineBytes=0;
        std::set<std::string> supplied;
        for(const auto& t:input.frameTimelines) {
            timelineWorkspace.Add(2048+8ull*(t.frameScope.size()+t.name.size()+t.observationUnit.size()));
            supplied.insert(t.frameScope); timelineBytes+=FrameBytes(t.frames);
        }
        if(timelineBytes>options.frameWorkspaceBytes/4) Fail("frame_workspace_budget");
        std::map<std::string,PolicySignatureContext> roots;
        const auto count=VisitContexts([&](const PolicySignatureContext& c) {
            contextOrder->Append(Unsigned(*c.sourceOrdinal),SignatureKey(c));
            if(!c.frameRoot || !c.frameSeriesComplete || supplied.contains(c.frameScope)) return;
            auto it=roots.find(c.frameScope);
            if(it!=roots.end() && *it->second.sourceOrdinal<*c.sourceOrdinal) return;
            const auto bytes=FrameBytes(c.frames);
            if(it!=roots.end()) timelineBytes-=FrameBytes(it->second.frames);
            if(bytes>options.frameWorkspaceBytes/4-timelineBytes) Fail("frame_workspace_budget");
            const auto previous=it==roots.end()?0:ContextBytes(it->second);
            const auto next=ContextBytes(c);
            rootsWorkspace.Add(next); // Old and replacement coexist during the copy.
            PolicySignatureContext replacement(c);
            timelineBytes+=bytes; roots[c.frameScope]=std::move(replacement);
            rootsWorkspace.Resize(rootsWorkspace.Bytes()-previous);
        });
        contextOrder->Commit(); contextOrder.reset();
        { auto order=Reader("context-order");
          if(order.RecordCount()!=count) Fail("context_order_count");
          for(uint64_t i=0;i<count;++i) if(order.GetAt(i).key!=Unsigned(i)) Fail("context_order_gap"); }
        const auto& config=input.normalizedProfile.at("candidate_policy");
        const auto topN=config.value("local_top_n",uint64_t(5));
        if(topN>1000) Fail("local_top_budget");
        const auto scales=config.value("window_sizes",std::vector<uint64_t>{30,60,120});
        if(scales.size()>64 || timelineBytes>options.frameWorkspaceBytes/(4+scales.size())) Fail("frame_workspace_budget");
        // Covers the copied timelines, window vectors/set nodes, their growth
        // peaks, and median/MAD arrays. Charge before Prepare creates them.
        for(const auto& [_,c]:roots)
            timelineWorkspace.Add(2048+8ull*(c.frameScope.size()+c.name.size()+c.observationUnit.size()));
        timelineWorkspace.Add(2*timelineBytes*(4+scales.size()));
        const auto state=frame_window::Prepare(input,[&](const auto& visit){for(const auto& [_,c]:roots) visit(c);});
        roots.clear(); rootsWorkspace.Resize(0);
        auto topInput=Sorter("local-top-input"),observations=Sorter("local-observations");
        VisitContexts([&](const PolicySignatureContext& c) {
            if(FrameBytes(c.frames)>options.frameWorkspaceBytes/4) Fail("frame_workspace_budget");
            const auto key=SignatureKey(c);
            AnalysisWorkspaceReservation seriesWorkspace(options.table.workspace);
            if(c.frameSeriesComplete) {
                // The native extent has a known row count, so reject before
                // ReadPolicyFrameSeries allocates. Opaque callbacks are checked
                // immediately on return, before the policy makes any copies.
                if(input.readFrameSeries && c.seriesCount) {
                    if(c.seriesCount>UINT64_MAX/256) Fail("frame_workspace_budget");
                    seriesWorkspace.Add(c.seriesCount*256);
                } else seriesWorkspace.Add(FrameBytes(c.frames));
            }
            auto signals=frame_window::ScanContext(input,size_t(*c.sourceOrdinal),c,state,
                [&](const frame_window::TopKey& group,frame_window::TopItem item) {
                    Check(options); item.sourceKey=key;
                    const auto groupKey=Field(std::get<0>(group))+Field(std::get<1>(group))+Field(std::get<2>(group))+Field(std::get<3>(group));
                    auto payload=TopJson(item); payload["group"]=Hex(groupKey);
                    topInput->Append(groupKey+Unsigned(item.context),payload.dump());
                },[&](const std::vector<PolicyFrameEvidence>& series) {
                    const auto bytes=FrameBytes(series);
                    if(bytes>options.frameWorkspaceBytes/4) Fail("frame_workspace_budget");
                    uint64_t largest=256;
                    for(const auto& frame:series) {
                        uint64_t current=256+2ull*frame.structureKey.size();
                        for(const auto& ref:frame.eventRefs) current+=64+2ull*ref.size();
                        largest=std::max(largest,current);
                    }
                    // Qualified/typical/window copies, ranges/quantile arrays,
                    // and returned Signal JSON stay covered through spooling.
                    seriesWorkspace.Resize(8192+32*bytes+16ull*key.size());
                    const auto timeline=state.timelines.find(c.frameScope);
                    if(timeline!=state.timelines.end()) {
                        seriesWorkspace.Add(4*FrameBytes(timeline->second.frames));
                        const auto windows=state.windows.at(c.frameScope).size();
                        const auto perWindow=8192+64*largest;
                        if(windows>UINT64_MAX/perWindow) Fail("frame_workspace_budget");
                        seriesWorkspace.Add(windows*perWindow);
                    }
                });
            for(const auto& signal:signals)
            {
                Check(options); const auto mergeKey=Unsigned(signal.context)+Field(signal.manifestation);
                observations->Append(mergeKey+Unsigned(0),json{{"merge_key",Hex(mergeKey)},{"source_key",Hex(key)},{"signal",SignalJson(signal)}}.dump());
            }
        });
        topInput->Commit(); topInput.reset();
        {
            auto inputTop=Reader("local-top-input"); std::string group;
            const auto slotBytes=2ull*sizeof(frame_window::TopItem)*(topN+1);
            AnalysisWorkspaceReservation entriesWorkspace(options.table.workspace,slotBytes);
            std::vector<frame_window::TopItem> entries;
            entries.reserve(size_t(topN+1));
            const auto flush=[&] {
                uint64_t rank=0;
                for(const auto& item:entries)
                {
                    AnalysisWorkspaceReservation encodingWorkspace(options.table.workspace,65536+16*item.workspaceBytes);
                    const auto mergeKey=Unsigned(item.context)+Field(item.manifestation);
                    observations->Append(mergeKey+Unsigned(1)+group+Unsigned(rank++),
                        json{{"merge_key",Hex(mergeKey)},{"source_key",Hex(item.sourceKey)},{"top",TopJson(item)}}.dump());
                }
                entries.clear(); entriesWorkspace.Resize(slotBytes);
            };
            for(uint64_t i=0;i<inputTop.RecordCount();++i)
            {
                Check(options); const auto record=inputTop.GetAt(i);
                AnalysisWorkspaceReservation decodingWorkspace(options.table.workspace,65536+32ull*record.payload.size());
                const auto row=json::parse(record.payload);
                const auto nextGroup=Unhex(row.at("group").get<std::string>());
                if(nextGroup!=group) { flush(); group=nextGroup; }
                // Replay original context order through the very same bounded
                // reservoir, including equivalent-comparator insertion behavior.
                auto item=ParseTop(row);
                item.workspaceBytes=4096+4ull*(item.manifestation.size()+item.signature.size()+item.sourceKey.size())+
                    32ull*item.evidence.dump().size();
                entriesWorkspace.Add(item.workspaceBytes);
                frame_window::InsertTop(entries,std::move(item),topN);
                uint64_t retained=slotBytes;
                for(const auto& kept:entries) retained+=kept.workspaceBytes;
                entriesWorkspace.Resize(retained); // Dropped entries no longer retain strings or JSON.
            }
            flush();
        }
        observations->Commit(); observations.reset();
        {
            auto observationsReader=Reader("local-observations"); auto output=Sorter("local-signals");
            AnalysisWorkspaceReservation keysWorkspace(options.table.workspace),signalWorkspace(options.table.workspace);
            std::string mergeKey,sourceKey; frame_window::SignalMap current; uint64_t payloadBytes=0;
            const auto flush=[&] {
                for(const auto& [_,s]:current) output->Append(sourceKey+Field(s.manifestation),SignalJson(s).dump());
                current.clear(); payloadBytes=0; signalWorkspace.Resize(0);
            };
            for(uint64_t i=0;i<observationsReader.RecordCount();++i)
            {
                Check(options); const auto record=observationsReader.GetAt(i);
                AnalysisWorkspaceReservation decodingWorkspace(options.table.workspace,65536+32ull*record.payload.size());
                const auto row=json::parse(record.payload);
                auto nextKey=Unhex(row.at("merge_key").get<std::string>());
                if(nextKey!=mergeKey) {
                    flush(); auto nextSource=Unhex(row.at("source_key").get<std::string>());
                    keysWorkspace.Resize(std::max(keysWorkspace.Bytes(),1024+8ull*(nextKey.size()+nextSource.size())));
                    mergeKey=std::move(nextKey); sourceKey=std::move(nextSource);
                }
                if(sourceKey!=Unhex(row.at("source_key").get<std::string>())) Fail("local_context_join");
                if(row.contains("signal")) {
                    if(record.payload.size()>options.familyBytes) Fail("family_budget");
                    signalWorkspace.Resize(65536+32ull*record.payload.size());
                    auto s=ParseSignal(row.at("signal"));
                    if(!current.emplace(std::make_pair(s.context,s.manifestation),std::move(s)).second) Fail("duplicate_local_signal");
                    payloadBytes=record.payload.size();
                } else {
                    const bool wasEmpty=current.empty();
                    const auto previous=wasEmpty?0:current.begin()->second.representatives.size();
                    auto top=ParseTop(row.at("top"));
                    if(wasEmpty) {
                        if(record.payload.size()>options.familyBytes) Fail("family_budget");
                        signalWorkspace.Resize(65536+32ull*record.payload.size());
                    } else if(top.cost>current.begin()->second.valueNs) signalWorkspace.Add(8192);
                    frame_window::ApplyTop(current,top);
                    if(wasEmpty) payloadBytes=record.payload.size();
                    else {
                        // ApplyTop only appends a lightweight representative on
                        // a new maximum; equal/lower observations retain no data.
                        const auto added=current.begin()->second.representatives.size()-previous;
                        if(added>(options.familyBytes-std::min(payloadBytes,options.familyBytes))/256) Fail("family_budget");
                        payloadBytes+=added*256;
                    }
                }
                if(payloadBytes>options.familyBytes) Fail("family_budget");
                signalWorkspace.Resize(65536+32*payloadBytes);
            }
            flush(); output->Commit();
        }
    }
    void PrepareRankings()
    {
        for(uint64_t groupOrdinal=0;;)
        {
            auto groups=neutral.RankingGroups(groupOrdinal,64,options.table.blockBytes);
            for(const auto& record:groups.records)
            {
                AnalysisWorkspaceReservation groupWorkspace(options.table.workspace,65536+32ull*record.payload.size());
                const auto group=json::parse(record.payload);
                const auto domain=group.at("domain").get<std::string>(),scope=group.at("frame_scope").get<std::string>();
                const auto metric=group.at("metric").get<std::string>(); double previous=0;
                for(uint64_t ordinal=0;;)
                {
                    auto page=neutral.Ranking(domain,scope,Metric(metric),ordinal,128,options.table.blockBytes);
                    for(const auto& row:page.records)
                    {
                        Check(options);
                        // Augmented ranking JSON adds a full encoded scope
                        // ordering key while the source/group remain alive.
                        AnalysisWorkspaceReservation decodingWorkspace(options.table.workspace,
                            65536+32ull*(row.payload.size()+domain.size()+scope.size()));
                        auto j=json::parse(row.payload);
                        if(j.at("frame_scope")!=scope) Fail("ranking_scope");
                        j["domain"]=domain;
                        j["metric"]=metric; j["index"]=ordinal; j["previous"]=previous;
                        j["order"]=Hex(Field(domain)+Field(scope)+Unsigned(MetricOrdinal(metric))+Unsigned(ordinal));
                        rankInput->Append(SignatureKey(j)+Unsigned(MetricOrdinal(metric)),j.dump());
                        previous=std::stod(j.at("cumulative_contribution").get<std::string>()); ++ordinal;
                    }
                    if(ordinal!=page.nextOrdinal) Fail("ranking_ordinal");
                    if(page.done) break;
                }
            }
            groupOrdinal=groups.nextOrdinal; if(groups.done) break;
        }
        rankInput->Commit(); rankInput.reset();
    }
    void Join()
    {
        auto ranks=Reader("ranking-input"); uint64_t ordinal=0;
        auto locals=Reader("local-signals"); uint64_t localOrdinal=0;
        std::optional<AnalysisCacheRecord> nextLocal;
        const auto advanceLocal=[&] { nextLocal=localOrdinal<locals.RecordCount()?std::optional(locals.GetAt(localOrdinal++)):std::nullopt; };
        advanceLocal();
        std::optional<AnalysisCacheRecord> next;
        const auto advance=[&] { next=ordinal<ranks.RecordCount()?std::optional(ranks.GetAt(ordinal++)):std::nullopt; };
        advance();
        const auto visited=neutral.VisitSignatureContexts([&](const NeutralSignatureAggregate* stat,const PolicySignatureContext& context)
        {
            Check(options);
            // The reader owns its input DTOs; this lease owns the additional
            // representative copies and current candidate-fact serialization.
            AnalysisWorkspaceReservation contextWorkspace(options.table.workspace,65536+32*ContextBytes(context));
            if(stat) for(const auto* metric:{&stat->inclusive,&stat->exclusive,&stat->wait,&stat->criticalPath})
                contextWorkspace.Add(2048ull*metric->anomalies.size()+32ull*(metric->perCompleteFrame.unavailableReason.size()+
                    metric->whenPresent.unavailableReason.size()));
            Families small;
            if(!context.sourceOrdinal) Fail("context_order_missing");
            const auto key=SignatureKey(context);
            while(nextLocal && nextLocal->key.starts_with(key))
            {
                AnalysisWorkspaceReservation signalWorkspace(options.table.workspace,65536+32ull*nextLocal->payload.size());
                const auto signal=ParseSignal(json::parse(nextLocal->payload));
                Families local;
                if(signal.context!=*context.sourceOrdinal) Fail("local_context_join");
                candidate_policy::Local(local,context,signal);
                Emit(local,Unsigned(0)+Unsigned(*context.sourceOrdinal)+Field(signal.manifestation)); advanceLocal();
            }
            if(nextLocal && nextLocal->key<key) Fail("local_context_missing");
            {
                AnalysisWorkspaceReservation budgetWorkspace(options.table.workspace);
                Families budget;
                const auto contextBytes=ContextBytes(context);
                candidate_policy::Budget(budget,input,stat,context,[&](uint64_t profileBytes) {
                    budgetWorkspace.Add(65536+32*(contextBytes+profileBytes));
                });
                Emit(budget,Unsigned(1)+Unsigned(*context.sourceOrdinal));
            }
            while(next && next->key.starts_with(key))
            {
                AnalysisWorkspaceReservation rankingWorkspace(options.table.workspace,65536+32ull*next->payload.size());
                const auto j=json::parse(next->payload);
                Families ranking;
                if(SignatureKey(j)!=key || !stat) Fail("ranking_join");
                const auto domain=j.at("domain").get<std::string>(),metric=j.at("metric").get<std::string>();
                const auto entry=Entry(j);
                const auto output=candidate_policy::Top(ranking,input,domain,metric.c_str(),entry,j.at("index").get<uint64_t>(),
                    j.at("previous").get<double>(),&context,stat);
                ranked->Append(Field(output.domain)+Field(output.frameScope)+Field(output.ranking)+Unsigned(output.rank)+Field(output.signatureId),
                    candidate_policy::RankedJson(output).dump());
                Emit(ranking,Unsigned(2)+Unhex(j.at("order").get<std::string>())); advance();
            }
            if(next && next->key<key) Fail("ranking_context_missing");
            if(stat) { candidate_policy::Anomalies(small,*stat,&context); Emit(small,Unsigned(3)+key); }
            uint64_t focus=0;
            for(const auto& value:input.normalizedProfile.at("user_focus")) {
                candidate_policy::Focus(small,value,context,stat);
                Emit(small,Unsigned(5)+Unsigned(focus++)+Unsigned(*context.sourceOrdinal));
            }
        });
        if(next) Fail("ranking_context_missing");
        if(nextLocal) Fail("local_context_missing");
        { auto order=Reader("context-order"); if(order.RecordCount()!=visited) Fail("context_order_count"); }
        for(uint64_t i=0;i<input.capacityFacts.size();++i)
        {
            Check(options); const auto& fact=input.capacityFacts[i];
            AnalysisWorkspaceReservation capacityWorkspace(options.table.workspace,
                65536+32ull*(fact.domain.size()+fact.signatureId.size()+fact.familyId.size()+fact.metric.size()+fact.description.size()));
            Families small; candidate_policy::Capacity(small,input,fact); Emit(small,Unsigned(4)+Unsigned(i));
        }
        facts->Commit(); facts.reset(); ranked->Commit(); ranked.reset();
    }
    void Reduce()
    {
        auto priority=Sorter("priority"); auto source=Reader("facts");
        AnalysisWorkspaceReservation familyWorkspace(options.table.workspace);
        FamilyWork current; uint64_t familyPayload=0;
        const auto flush=[&] {
            if(current.candidate.familyId.empty()) return;
            candidate_policy::Finalize(current,current.candidate.familyId,identity);
            current.representatives.clear();
            priority->Append(PriorityKey(current),Encode(current)); current={}; familyPayload=0;
            familyWorkspace.Resize(0);
        };
        for(uint64_t ordinal=0;ordinal<source.RecordCount();++ordinal)
        {
            Check(options); auto record=source.GetAt(ordinal);
            AnalysisWorkspaceReservation incomingWorkspace(options.table.workspace,65536+32ull*record.payload.size());
            auto incoming=Decode(record.payload);
            if(!current.candidate.familyId.empty() && incoming.candidate.familyId!=current.candidate.familyId) flush();
            if(record.payload.size()>options.familyBytes-familyPayload) Fail("family_budget");
            familyPayload+=record.payload.size();
            // Cumulative encoded fragments conservatively cover retained
            // members/evidence, merge growth, Finalize and re-encoding. The
            // incoming parsed fragment coexists until Merge consumes it.
            familyWorkspace.Add(32ull*record.payload.size());
            Merge(current,std::move(incoming));
        }
        flush(); priority->Commit();
    }
    void Publish()
    {
        AnalysisWorkspaceReservation selectedWorkspace(options.table.workspace);
        CandidateBacklog backlog; std::map<std::string,uint64_t> selected;
        AnalysisCacheTableWriter candidates(root/"candidates",identity,Kind("candidates"),options.table);
        auto lookup=Sorter("candidate-index"); auto priority=Reader("priority");
        for(uint64_t ordinal=0;ordinal<priority.RecordCount();++ordinal)
        {
            Check(options); const auto record=priority.GetAt(ordinal);
            AnalysisWorkspaceReservation currentWorkspace(options.table.workspace,65536+32ull*record.payload.size());
            auto work=Decode(record.payload);
            // Select copies the complete L0 event references from the input
            // timeline, which are not present in the serialized family.
            if(work.candidate.observationUnit=="l0_segment")
                for(const auto& timeline:input.frameTimelines) if(timeline.frameScope==work.candidate.frameScope)
                    for(const auto& frame:work.candidate.representativeFrames)
                    {
                        const auto segment=std::find_if(timeline.frames.begin(),timeline.frames.end(),
                            [&](const auto& value){return value.frameIndex==frame.frameIndex;});
                        if(segment==timeline.frames.end()) continue;
                        currentWorkspace.Add(2048);
                        for(const auto& ref:segment->eventRefs) currentWorkspace.Add(32ull*(64+ref.size()));
                    }
            const uint64_t newDomainBytes=selected.contains(work.candidate.domain)?0:256+4ull*work.candidate.domain.size();
            selectedWorkspace.Add(newDomainBytes);
            candidate_policy::Select(work,input,selected);
            if(!selected.contains(work.candidate.domain)) selectedWorkspace.Resize(selectedWorkspace.Bytes()-newDomainBytes);
            if(selected.size()>options.table.manifestBytes/512) Fail("domain_metadata_budget");
            candidates.Append(Unsigned(ordinal),candidate_policy::CandidateJson(work.candidate).dump());
            lookup->Append(work.candidate.candidateId,json{{"ordinal",ordinal}}.dump());
            ++backlog.total; backlog.selected+=work.candidate.selected;
        }
        backlog.notSelected=backlog.total-backlog.selected;
        const auto candidateDescriptor=candidates.Commit(),indexDescriptor=lookup->Commit();
        const auto rankedReader=Reader("ranked-signatures");
        AnalysisWorkspaceReservation qualityWorkspace(options.table.workspace,65536+32ull*neutral.SummaryBytes());
        json quality=json::array();
        const auto neutralSummary=json::parse(neutral.SummaryJson());
        for(const auto& d:neutralSummary.at("domains"))
        {
            if(d.at("status")!="invalid" && d.at("quality_complete").get<bool>()) continue;
            const auto reason=d.at("unavailable_reason");
            quality.push_back({{"domain",d.at("domain")},{"status",d.at("status")},
                {"reason",reason.is_null() || reason==""?json("domain_invalid"):reason},
                {"input_count",d.at("input_count")},{"consumed_input_count",d.at("consumed_input_count")},
                {"audit_complete",d.at("quality_complete")}});
        }
        std::sort(quality.begin(),quality.end(),[](const json& a,const json& b){return a.dump()<b.dump();});
        json summary={{"schema",Schema},{"aggregate_identity",input.aggregateIdentity},
            {"aggregate_content_sha256",input.aggregate.contentSha256},{"neutral_cache_content_sha256",neutral.ContentSha256()},
            {"policy_identity",identity},{"profile_identity",input.profileIdentity},{"policy_algorithm",CandidatePolicyAlgorithmId},
            {"capture_quality",quality},{"backlog",{{"total",backlog.total},{"selected",backlog.selected},{"not_selected",backlog.notSelected}}},
            {"ranked_signature_count",rankedReader.RecordCount()},{"fact_count",fragments},{"retained_final_candidate_peak",backlog.total==0?0:1}};
        json tables={{"candidates",Description(candidateDescriptor)},{"candidate-index",Description(indexDescriptor)},
            {"ranked-signatures",Description(rankedReader.Descriptor())}};
        Check(options);
        AnalysisCacheTableWriter header(root/"header",identity,Kind("header"),options.table);
        header.Append("header",json{{"schema",Schema},{"summary",summary},{"tables",tables}}.dump()); header.Commit();
    }
};
}
std::string BuildCandidatePolicyCache(const std::filesystem::path& root,NeutralStatisticsCacheReader& neutral,
    const CandidatePolicyInput& metadata,CandidatePolicyCacheOptions options)
{
    Builder builder(root,neutral,metadata,std::move(options));
    builder.PrepareLocals(); builder.PrepareRankings(); builder.Join(); builder.Reduce(); builder.Publish(); return builder.identity;
}
struct CandidatePolicyCacheReader::Impl
{
    CandidatePolicyCacheOptions options;
    AnalysisWorkspaceReservation summaryWorkspace;
    std::string summary,contentSha256;
    std::unique_ptr<AnalysisCacheTableReader> candidates,index,ranked;
    Impl(const std::filesystem::path& root,const std::string& identity,CandidatePolicyCacheOptions config)
        :options(std::move(config)),summaryWorkspace(options.table.workspace)
    {
        Check(options); if(!std::filesystem::exists(root/"header"/"manifest.json")) Fail("incomplete");
        AnalysisCacheTableReader header(root/"header",identity,Kind("header"),options.table);
        contentSha256=header.Descriptor().contentSha256;
        if(header.RecordCount()!=1) Fail("header_count");
        const auto record=header.GetAt(0);
        AnalysisWorkspaceReservation decodingWorkspace(options.table.workspace,65536+32ull*record.payload.size());
        const auto j=json::parse(record.payload);
        if(record.key!="header" || j.at("schema").get<uint32_t>()!=Schema) Fail("header_schema");
        const auto open=[&](const char* name) {
            auto r=std::make_unique<AnalysisCacheTableReader>(root/name,identity,Kind(name),options.table);
            if(Description(r->Descriptor())!=j.at("tables").at(name)) Fail("table_identity_mismatch");
            return r;
        };
        candidates=open("candidates"); index=open("candidate-index"); ranked=open("ranked-signatures");
        const auto& small=j.at("summary");
        if(small.at("policy_identity")!=identity || small.at("policy_algorithm")!=CandidatePolicyAlgorithmId) Fail("policy_identity_mismatch");
        const auto& backlog=small.at("backlog");
        const auto total=backlog.at("total").get<uint64_t>(),selected=backlog.at("selected").get<uint64_t>();
        if(candidates->RecordCount()!=total || index->RecordCount()!=total || selected>total ||
            backlog.at("not_selected").get<uint64_t>()!=total-selected ||
            small.at("ranked_signature_count").get<uint64_t>()!=ranked->RecordCount()) Fail("count_mismatch");
        summaryWorkspace.Resize(512+2ull*record.payload.size());
        summary=small.dump();
    }
};
CandidatePolicyCacheReader::CandidatePolicyCacheReader(std::filesystem::path root,std::string identity,CandidatePolicyCacheOptions options)
    :m_impl(std::make_unique<Impl>(root,identity,std::move(options))) {}
CandidatePolicyCacheReader::~CandidatePolicyCacheReader()=default;
std::string CandidatePolicyCacheReader::SummaryJson() const { Check(m_impl->options); return m_impl->summary; }
uint64_t CandidatePolicyCacheReader::SummaryBytes() const { Check(m_impl->options); return m_impl->summary.size(); }
std::string CandidatePolicyCacheReader::ContentSha256() const { Check(m_impl->options); return m_impl->contentSha256; }
AnalysisCacheRecord CandidatePolicyCacheReader::CandidateAt(uint64_t ordinal) { return m_impl->candidates->GetAt(ordinal); }
AnalysisCachePage CandidatePolicyCacheReader::Candidates(uint64_t ordinal,size_t limit,uint64_t bytes)
{ return m_impl->candidates->ReadPage(ordinal,limit,bytes); }
AnalysisCachePage CandidatePolicyCacheReader::RankedSignatures(uint64_t ordinal,size_t limit,uint64_t bytes)
{ return m_impl->ranked->ReadPage(ordinal,limit,bytes); }
std::optional<AnalysisCacheRecord> CandidatePolicyCacheReader::Candidate(std::string_view id)
{
    auto index=m_impl->index->Find(id); if(!index) return {};
    AnalysisWorkspaceReservation decodingWorkspace(m_impl->options.table.workspace,65536+32ull*index->payload.size());
    auto record=m_impl->candidates->GetAt(json::parse(index->payload).at("ordinal").get<uint64_t>());
    decodingWorkspace.Resize(65536+32ull*record.payload.size());
    if(json::parse(record.payload).at("candidate_id").get<std::string>()!=id) Fail("candidate_index_mismatch");
    return record;
}
}

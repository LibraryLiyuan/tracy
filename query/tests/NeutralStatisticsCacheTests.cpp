#include "TracyNeutralStatisticsCache.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace tracy::analysis;
using nlohmann::json;
namespace
{
const std::string Identity(64,'a');
void Require(bool value, const char* message) { if(!value) throw std::runtime_error(message); }
template<class F> void Reject(F action, const char* expected)
{
    try { action(); } catch(const std::exception& e)
    { Require(std::string(e.what()).find(expected) != std::string::npos, e.what()); return; }
    throw std::runtime_error(std::string("expected rejection: ")+expected);
}
NeutralSignatureAggregate Stat(std::string id, std::string scope, int64_t inclusive,
    int64_t exclusive = 0, bool logical = false, bool exact = true)
{
    NeutralSignatureAggregate s;
    s.domain="cpu"; s.signatureId=std::move(id); s.frameScope=std::move(scope);
    s.logical=logical; s.exact=exact; s.completeFrameCount=1; s.presentFrameCount=1;
    s.inclusive.perCompleteFrame.total=inclusive; s.exclusive.perCompleteFrame.total=exclusive;
    return s;
}
std::string ContextJson(const NeutralSignatureAggregate& s, bool root = false)
{
    return json{{"domain",s.domain},{"signature_id",s.signatureId},{"frame_scope",s.frameScope},
        {"frame_root",root},{"path",json::array({"Player","Thread/Root",s.signatureId})},
        {"series_offset","12345"},{"series_count","5309"},{"name","Same display name"}}.dump();
}
NeutralStatisticsStreamSummary Summary(uint64_t count)
{
    NeutralStatisticsStreamSummary s; s.qualityComplete=true; s.signatureCount=count;
    NeutralDomainAuditResult d; d.domain="cpu"; d.present=true; d.status="complete";
    d.inputCount=d.consumedInputCount=d.outputCount=d.actualOutputCount=count;
    d.inputChecksum=d.consumedChecksum=std::string(64,'a'); d.qualityComplete=true;
    s.domains.push_back(d); return s;
}
NeutralStatisticsCacheOptions Options()
{
    NeutralStatisticsCacheOptions o; o.table.blockBytes=16384; o.sortBufferBytes=32768; return o;
}
void Matrix(const std::filesystem::path& root)
{
    auto a=Stat("A","Player",30,20); a.wait.perCompleteFrame.total=5; a.criticalPath.perCompleteFrame.total=7;
    auto b=Stat("B","Player",30,10); b.wait.perCompleteFrame.total=2;
    auto logical=Stat("L","Player",500,500,true);
    auto render=Stat("L","Render",12,3,true);
    auto unknown=Stat("D","Player",999,999,false,false); unknown.unknownFrameCount=1;
    std::vector<NeutralSignatureAggregate> stats={unknown,render,logical,b,a};
    {
        NeutralStatisticsCacheWriter w(root,Identity,Options());
        for(const auto& s:stats) { w.AppendStatistics(s); w.AppendContext(ContextJson(s)); }
        w.AppendContext(ContextJson(Stat("root","Player",0),true));
        Reject([&]{ NeutralStatisticsCacheReader r(root,Identity,Options()); },"incomplete");
        w.Commit(Summary(stats.size()));
    }
    NeutralStatisticsCacheReader r(root,Identity,Options());
    Require(r.SignatureCount()==5,"all exact and unknown signatures retained");
    Require(json::parse(r.SummaryJson()).at("signature_count")=="5","small summary count");
    for(const auto& s:stats)
    {
        Require(r.Signature(s.domain,s.signatureId,s.frameScope)->payload==SerializeNeutralSignatureRecord(s),"all statistical fields identical");
        Require(r.Context(s.domain,s.signatureId,s.frameScope)->payload==ContextJson(s),"full Context preserved");
    }
    Require(!r.Signature("cpu","root","Player"),"frame root has no artificial aggregate");
    Require(r.Context("cpu","root","Player").has_value(),"root-only context retained");
    const auto contexts=r.Contexts(0,20,65536);
    Require(contexts.done && contexts.nextOrdinal==6 && contexts.records.size()==6,"sequential contexts include frame roots");
    const auto groups=r.RankingGroups(0,20,65536);
    Require(groups.done && groups.records.size()==6,"enumerate both scopes and all three ranking metrics");
    for(const auto& group:groups.records)
    {
        const auto g=json::parse(group.payload);
        Require(g.at("domain")=="cpu" && (g.at("frame_scope")=="Player" || g.at("frame_scope")=="Render"),"ranking groups expose full scope");
        Require(g.contains("metric") && g.contains("count"),"ranking groups can drive sequential consumers");
    }
    auto page=r.Ranking("cpu","Player",NeutralRankingMetric::Inclusive,0,20,65536);
    Require(page.records.size()==2 && page.done && page.nextOrdinal==2,"physical rankings exclude logical and unknown");
    const auto first=json::parse(page.records[0].payload), second=json::parse(page.records[1].payload);
    Require(first.at("signature_id")=="A" && second.at("signature_id")=="B","stable tie order");
    Require(std::stod(first.at("contribution").get<std::string>())==0.5,"exact contribution");
    Require(std::stod(second.at("cumulative_contribution").get<std::string>())==1,"final cumulative exactly one");
    page=r.Ranking("cpu","Player",NeutralRankingMetric::WaitCritical,0,1,65536);
    auto wait=json::parse(page.records.at(0).payload);
    Require(wait.at("total_ns")=="7" && wait.at("wait_ns")=="5" && wait.at("critical_path_ns")=="7","wait critical max and components");
    Require(!page.done && page.nextOrdinal==1,"ranking pagination");
    page=r.Ranking("cpu","Render",NeutralRankingMetric::Inclusive,0,20,65536);
    Require(page.records.size()==1 && json::parse(page.records[0].payload).at("logical")==true,"logical-only scope ranked independently");
    page=r.Ranking("cpu","Player",NeutralRankingMetric::Inclusive,2,20,65536);
    Require(page.records.empty() && page.done,"page cannot spill into next scope");
    page=r.Ranking("absent","Player",NeutralRankingMetric::Inclusive,0,20,65536);
    Require(page.records.empty() && page.done,"absent ranking group");
    Reject([&]{r.Signatures(0,20,1);},"response_budget");
    Reject([&]{r.Ranking("cpu","Player",NeutralRankingMetric::Inclusive,0,20,1);},"response_budget");
    Reject([&]{NeutralStatisticsCacheReader wrong(root,"wrong",Options());},"identity");
}
void RankingDecodeWorkspace(const std::filesystem::path& root)
{
    auto options=Options(); options.table.blockBytes=262144; options.sortBufferBytes=1048576;
    options.table.manifestBytes=65536;
    auto budget=std::make_shared<AnalysisWorkspaceBudget>(16*1024*1024,8*1024*1024); options.table.workspace=budget;
    AnalysisWorkspaceReservation pressure(budget); bool armed=false;
    options.table.cancelled=[&] {
        if(!armed && std::filesystem::exists(root/"ranking-input.sorting")) {
            armed=true; const auto current=budget->Snapshot().currentBytes;
            Require(current+3*1024*1024<=16*1024*1024,"ranking pressure has sufficient setup headroom");
            pressure.Resize(16*1024*1024-current-3*1024*1024);
        }
        return false;
    };
    std::string error;
    {
        NeutralStatisticsCacheWriter writer(root,Identity,options);
        auto stat=Stat("one","Player",1); stat.inclusive.perCompleteFrame.unavailableReason=std::string(131072,'r');
        writer.AppendStatistics(stat); writer.AppendContext(ContextJson(stat));
        try { writer.Commit(Summary(1)); } catch(const std::exception& e) { error=e.what(); }
    }
    const auto published=std::filesystem::exists(root/"ranking-input"/"manifest.json");
    if(error.find("workspace_budget")==std::string::npos || published)
        std::cerr<<"ranking decode error="<<error<<" ranking_input_published="<<published<<'\n';
    Require(armed && error.find("workspace_budget")!=std::string::npos && !published,
        "ranking preparation must reserve the current statistics JSON before emitting ranking records");
    pressure.Resize(0); Require(budget->Snapshot().currentBytes==0,"ranking decode cancellation frees all source and writer budgets");
}
void HeaderDecodeWorkspace(const std::filesystem::path& root)
{
    auto options=Options(); options.table.blockBytes=131072; options.sortBufferBytes=262144;
    auto summary=Summary(1); summary.qualityFindings.push_back(std::string(65536,'q'));
    { NeutralStatisticsCacheWriter writer(root,Identity,options); const auto stat=Stat("one","Player",1);
      writer.AppendStatistics(stat); writer.AppendContext(ContextJson(stat)); writer.Commit(summary); }
    options.table.workspace=std::make_shared<AnalysisWorkspaceBudget>(2*1024*1024,1024*1024);
    Reject([&]{ NeutralStatisticsCacheReader reader(root,Identity,options); },"workspace_budget");
    Require(options.table.workspace->Snapshot().currentBytes==0,"failed header decode releases the open source block and metadata");
    options.table.workspace=std::make_shared<AnalysisWorkspaceBudget>(8*1024*1024,4*1024*1024);
    { NeutralStatisticsCacheReader reader(root,Identity,options);
      Require(reader.SummaryJson().find(std::string(65536,'q'))!=std::string::npos,"bounded reopen retains the full quality summary"); }
    Require(options.table.workspace->Snapshot().currentBytes==0,"cached summary releases its retained workspace with the reader");
}
void CurrentWriterWorkspace(const std::filesystem::path& root)
{
    unsigned bypasses=0;
    for(const unsigned operation:{0,1,2}) {
        auto options=Options(); options.table.manifestBytes=65536;
        auto budget=std::make_shared<AnalysisWorkspaceBudget>(2*1024*1024,1024*1024); options.table.workspace=budget;
        {
            NeutralStatisticsCacheWriter writer(root/std::to_string(operation),Identity,options);
            if(operation==1) writer.AppendStatistics(Stat("warm","Player",1));
            const auto before=budget->Snapshot().currentBytes;
            AnalysisWorkspaceReservation pressure(budget,2*1024*1024-before-128);
            bool rejected=false;
            try { if(operation==2) writer.AppendContext(ContextJson(Stat("next","Player",2)));
                  else writer.AppendStatistics(Stat("next","Player",2)); }
            catch(const std::exception& e) { if(std::string(e.what()).find("workspace_budget")==std::string::npos) throw; rejected=true; }
            bypasses+=!rejected;
            if(rejected) Require(!std::filesystem::exists(root/std::to_string(operation)/"header"/"manifest.json"),"writer workspace denial cannot publish");
        }
        Require(budget->Snapshot().currentBytes==0,"failed current writer objects release shared reservations");
    }
    if(bypasses) std::cerr<<"unbudgeted neutral writer operations="<<bypasses<<'\n';
    Require(bypasses==0,"new scope metadata, existing-scope statistics encoding and Context JSON decoding must share the workspace");
    auto options=Options(); options.table.workspace=std::make_shared<AnalysisWorkspaceBudget>(2*1024*1024,1024*1024);
    {
        NeutralStatisticsCacheWriter writer(root/"retained",Identity,options);
        const auto empty=options.table.workspace->Snapshot().currentBytes;
        writer.AppendStatistics(Stat("one","Player",1));
        Require(options.table.workspace->Snapshot().currentBytes>empty,"domain/scope metadata stays budgeted after the encoding temporary dies");
    }
    Require(options.table.workspace->Snapshot().currentBytes==0,"domain/scope metadata returns its shared budget with the writer");
}
void KeyIdentity(const std::filesystem::path& root)
{
    std::vector<NeutralSignatureAggregate> stats={Stat("a","bc",1),Stat("ab","c",2),
        Stat(std::string("a\0b",3),"c",3),Stat("a",std::string("b\0c",3),4)};
    { NeutralStatisticsCacheWriter w(root,Identity,Options());
      for(const auto& s:stats) { w.AppendStatistics(s); w.AppendContext(ContextJson(s)); }
      w.Commit(Summary(stats.size())); }
    NeutralStatisticsCacheReader r(root,Identity,Options());
    for(const auto& s:stats) Require(r.Signature("cpu",s.signatureId,s.frameScope)->payload==SerializeNeutralSignatureRecord(s),"tuple fields and embedded NUL cannot collide");
}
void Publication(const std::filesystem::path& root)
{
    auto s=Stat("A","Player",1);
    { NeutralStatisticsCacheWriter w(root/"missing",Identity,Options()); w.AppendStatistics(s);
      Reject([&]{w.Commit(Summary(1));},"context_missing"); }
    Reject([&]{NeutralStatisticsCacheReader r(root/"missing",Identity,Options());},"incomplete");
    { NeutralStatisticsCacheWriter w(root/"orphan",Identity,Options()); w.AppendContext(ContextJson(s));
      Reject([&]{w.Commit(Summary(0));},"context_orphan"); }
    { NeutralStatisticsCacheWriter w(root/"count",Identity,Options()); w.AppendStatistics(s); w.AppendContext(ContextJson(s));
      Reject([&]{w.Commit(Summary(2));},"count_mismatch"); }
    { NeutralStatisticsCacheWriter w(root/"quality",Identity,Options()); auto summary=Summary(0); summary.qualityComplete=false;
      Reject([&]{w.Commit(summary);},"quality_incomplete"); }
    { NeutralStatisticsCacheWriter w(root/"duplicate",Identity,Options()); w.AppendStatistics(s); w.AppendStatistics(s); w.AppendContext(ContextJson(s));
      Reject([&]{w.Commit(Summary(2));},"key_order"); }
    { NeutralStatisticsCacheWriter w(root/"empty",Identity,Options()); w.Commit(Summary(0)); }
    { NeutralStatisticsCacheReader r(root/"empty",Identity,Options()); Require(r.SignatureCount()==0,"empty cache round trip"); }
    Reject([&]{NeutralStatisticsCacheWriter w(root/"empty",Identity,Options());},"already_exists");
}
void AuditRejectsInvalidContract(const std::filesystem::path& root)
{
    for(const bool checksum:{false,true})
    {
        const auto path=root/(checksum?"checksum":"status");
        NeutralStatisticsCacheWriter w(path,Identity,Options());
        auto summary=Summary(0);
        if(checksum) summary.domains[0].inputChecksum=summary.domains[0].consumedChecksum="not-a-digest";
        else summary.domains[0].status="typo";
        Reject([&]{w.Commit(summary);},"domain_audit");
        Require(!std::filesystem::exists(path/"header"/"manifest.json"),"invalid audit cannot publish");
    }
}
void MatchesLegacyOracle(const std::filesystem::path& root)
{
    NeutralStatisticsInput input; input.temporaryRoot=root/"oracle";
    constexpr int Count=80;
    for(int i=0;i<Count;++i)
    {
        const auto id="signature-"+std::to_string(i);
        const auto scope=i<10?"Zero":("scope-"+std::to_string(i%4));
        input.denominators.push_back({"cpu",id,scope,5});
        for(int frame=0;frame<3;++frame)
        {
            const int64_t value=i<10?0:(i%7)*(frame+1)*97;
            input.runs.push_back({"cpu",id,scope,uint64_t(frame),value,value/2,value/5,value/3,2,true,i%4==0 || i%3==0});
        }
    }
    input.domainAudit.push_back({"cpu",true,"complete",Count*3,Count*3,Count,Identity,Identity,true,{}});
    const auto oracle=BuildNeutralStatistics(input);
    Require(oracle.qualityComplete,"oracle input is valid");
    const auto legacy=json::parse(SerializeNeutralStatisticsResult(oracle));
    const auto cache=root/"cache";
    { NeutralStatisticsCacheWriter writer(cache,Identity,Options());
      for(auto i=oracle.signatures.rbegin();i!=oracle.signatures.rend();++i)
      { writer.AppendStatistics(*i); writer.AppendContext(ContextJson(*i)); }
      auto summary=Summary(Count); summary.domains=oracle.domains; writer.Commit(summary); }
    NeutralStatisticsCacheReader reader(cache,Identity,Options());
    for(const auto& row:legacy.at("signatures"))
        Require(json::parse(reader.Signature(row.at("domain"),row.at("signature_id"),row.at("frame_scope"))->payload)==row,"legacy statistics all-field parity");
    for(const auto& ranking:legacy.at("rankings"))
    {
        for(const auto& [metric,name]:std::vector<std::pair<NeutralRankingMetric,std::string>>{
            {NeutralRankingMetric::Inclusive,"inclusive"},{NeutralRankingMetric::Exclusive,"exclusive"},{NeutralRankingMetric::WaitCritical,"wait_critical"}})
        {
            const auto& expected=ranking.at(name); Require(!expected.empty(),"fixture has ranking rows");
            uint64_t ordinal=0;
            do {
                const auto page=reader.Ranking(ranking.at("domain"),expected[0].at("frame_scope"),metric,ordinal,7,4096);
                for(const auto& row:page.records) Require(json::parse(row.payload)==expected.at(ordinal++),"legacy ranking all-field parity including floating precision");
                Require(page.nextOrdinal==ordinal,"relative ranking cursor");
                if(page.done) break;
            } while(true);
            Require(ordinal==expected.size(),"legacy ranking no missing rows");
        }
    }
}
void CancellationAndCorruption(const std::filesystem::path& root)
{
    bool cancelled=false; auto options=Options(); options.table.cancelled=[&]{return cancelled;};
    const auto stat=Stat("A","Player",1);
    { NeutralStatisticsCacheWriter writer(root/"cancel",Identity,options);
      writer.AppendStatistics(stat); writer.AppendContext(ContextJson(stat)); cancelled=true;
      Reject([&]{writer.Commit(Summary(1));},"cancelled"); }
    Require(!std::filesystem::exists(root/"cancel"/"header"/"manifest.json"),"cancel cannot publish");
    cancelled=false;
    { NeutralStatisticsCacheWriter writer(root/"valid",Identity,options);
      writer.AppendStatistics(stat); writer.AppendContext(ContextJson(stat)); writer.Commit(Summary(1)); }
    { NeutralStatisticsCacheReader reader(root/"valid",Identity,options); cancelled=true;
      Reject([&]{reader.Signature("cpu","A","Player");},"cancelled");
      Reject([&]{reader.Ranking("cpu","Player",NeutralRankingMetric::Inclusive,0,10,1000);},"cancelled"); }
    cancelled=false;
    const auto block=*std::filesystem::directory_iterator(root/"valid"/"statistics"/"blocks");
    { std::fstream file(block.path(),std::ios::binary|std::ios::in|std::ios::out);
      file.seekg(-1,std::ios::end); char value=0; file.read(&value,1); value^=1;
      file.seekp(-1,std::ios::end); file.write(&value,1); }
    NeutralStatisticsCacheReader reader(root/"valid",Identity,options);
    Reject([&]{reader.Signature("cpu","A","Player");},"checksum");
}
void ResourceAndPublicationBoundaries(const std::filesystem::path& root)
{
    const auto stat=Stat("A","Player",1);
    { auto options=Options(); options.scopeMetadataBytes=1024;
      NeutralStatisticsCacheWriter writer(root/"metadata",Identity,options);
      Reject([&]{for(int i=0;i<10;++i) writer.AppendStatistics(Stat("A","scope-"+std::to_string(i),1));},"scope_metadata_budget");
      Reject([&]{writer.AppendContext(ContextJson(stat));},"writer_failed"); }
    { NeutralStatisticsCacheWriter writer(root/"oversize",Identity,Options());
      Reject([&]{writer.AppendContext(std::string(Options().table.blockBytes+1,'x'));},"context_record_budget");
      Reject([&]{writer.Commit(Summary(0));},"writer_failed"); }
    { NeutralStatisticsCacheWriter writer(root/"duplicate-context",Identity,Options());
      writer.AppendContext(ContextJson(stat,true)); writer.AppendContext(ContextJson(stat,true));
      Reject([&]{writer.Commit(Summary(0));},"key_order"); }
    const auto partial=root/"partial-publication";
    { auto options=Options(); options.table.cancelled=[&]{return std::filesystem::exists(partial/"rankings"/"manifest.json");};
      NeutralStatisticsCacheWriter writer(partial,Identity,options);
      writer.AppendStatistics(stat); writer.AppendContext(ContextJson(stat));
      Reject([&]{writer.Commit(Summary(1));},"cancelled"); }
    Require(std::filesystem::exists(partial/"rankings"/"manifest.json"),"cancellation occurred after a child table published");
    Reject([&]{NeutralStatisticsCacheReader reader(partial,Identity,Options());},"incomplete");
    for(const auto name:{"left","right"})
    { NeutralStatisticsCacheWriter writer(root/name,Identity,Options());
      auto value=stat; value.inclusive.perCompleteFrame.total=std::string(name)=="left"?1:2;
      writer.AppendStatistics(value); writer.AppendContext(ContextJson(value)); writer.Commit(Summary(1)); }
    // Replace only this fixture's owned child table with a valid but different
    // table having the SAME identity/count. Parent content binding must catch it.
    std::filesystem::remove_all(root/"left"/"statistics");
    std::filesystem::copy(root/"right"/"statistics",root/"left"/"statistics",std::filesystem::copy_options::recursive);
    Reject([&]{NeutralStatisticsCacheReader reader(root/"left",Identity,Options());},"table_identity_mismatch");
}
std::string ScaleId(uint64_t index) { return "scale-"+std::to_string(10000000000000000ull+index); }
void Scale(const std::filesystem::path& root,uint64_t count)
{
    Require(count>0 && count<=2000000,"scale count range");
    const auto start=std::chrono::steady_clock::now();
    NeutralStatisticsStreamSummary summary;
    {
        NeutralStatisticsStreamBuilder source(root/"runs");
        NeutralStatisticsCacheWriter cache(root/"cache",Identity);
        for(uint64_t i=0;i<count;++i)
        {
            const auto id=ScaleId(i); const int64_t value=100+(i%97);
            Require(source.AddRun({"cpu",id,"Player",i%5,value,value/2,value/5,value/3,1,true,false}),"scale run input");
            source.AddDenominator({"cpu",id,"Player",5});
        }
        source.AddDomainAudit({"cpu",true,"complete",count,count,count,Identity,Identity,true,{}});
        std::string error;
        const bool ok=source.FinishToSink(summary,error,[&](const auto& stat,const auto& frames){
            cache.AppendStatistics(stat);
            Require(frames.size()==1,"scale complete sparse evidence");
            cache.AppendContext(json{{"domain","cpu"},{"signature_id",stat.signatureId},{"frame_scope","Player"},
                {"path","Player/Thread/"+stat.signatureId},{"frame_root",false},{"frame_series_complete",true},
                {"frames",json::array({{{"frame_index",std::to_string(frames[0].frameIndex)},
                    {"value_ns",std::to_string(frames[0].inclusiveNs)}, {"event_refs",json::array({stat.signatureId})}}})}}.dump());
        });
        Require(ok,error.c_str()); Require(summary.materializedResultPeak==0,"scale sink retains no aggregate result vector");
        std::cerr<<"statistics emitted; building cache rankings\n";
        cache.Commit(summary);
    }
    const auto built=std::chrono::steady_clock::now();
    uint64_t rankingRecords=0;
    {
        NeutralStatisticsCacheReader cache(root/"cache",Identity);
        Require(cache.SignatureCount()==count,"scale re-open signature count");
        uint64_t ordinal=0;
        do {
            const auto page=cache.Signatures(ordinal,1000,1024*1024);
            for(const auto& record:page.records)
            {
                const auto row=json::parse(record.payload); const int64_t value=100+(ordinal%97);
                Require(row.at("signature_id")==ScaleId(ordinal),"scale sorted identity no omissions");
                Require(row.at("inclusive").at("per_complete_frame").at("total_ns")==std::to_string(value),"scale exact total");
                Require(row.at("inclusive").at("per_complete_frame").at("count")=="5","scale complete denominator");
                Require(row.at("inclusive").at("per_complete_frame").at("zero_count")=="4","scale implicit zeros");
                Require(row.at("unknown_frame_count")=="0","scale unknown count"); ++ordinal;
            }
            Require(page.nextOrdinal==ordinal,"scale signature cursor"); if(page.done) break;
        } while(true);
        Require(ordinal==count,"scale all signatures read"); ordinal=0;
        do {
            const auto page=cache.Contexts(ordinal,1000,1024*1024);
            for(const auto& record:page.records)
            {
                const auto row=json::parse(record.payload); const auto id=ScaleId(ordinal);
                Require(row.at("signature_id")==id && row.at("path")=="Player/Thread/"+id,"scale context identity and full path");
                Require(row.at("frames").at(0).at("frame_index")==std::to_string(ordinal%5),"scale representative frame link"); ++ordinal;
            }
            Require(page.nextOrdinal==ordinal,"scale context cursor"); if(page.done) break;
        } while(true);
        Require(ordinal==count,"scale all contexts read");
        for(const auto metric:{NeutralRankingMetric::Inclusive,NeutralRankingMetric::Exclusive,NeutralRankingMetric::WaitCritical})
        {
            ordinal=0; int64_t previous=INT64_MAX; std::string previousId; double cumulative=0;
            do {
                const auto page=cache.Ranking("cpu","Player",metric,ordinal,1000,1024*1024);
                for(const auto& record:page.records)
                {
                    const auto row=json::parse(record.payload); const auto id=row.at("signature_id").get<std::string>();
                    const int64_t total=std::stoll(row.at("total_ns").get<std::string>());
                    Require(total<previous || (total==previous && (previousId.empty() || previousId<id)),"scale exact rank order and unique identity");
                    previous=total; previousId=id;
                    const auto index=std::stoull(id.substr(6))-10000000000000000ull;
                    Require(index<count,"scale rank ID within input"); const int64_t value=100+(index%97);
                    const int64_t expected=metric==NeutralRankingMetric::Inclusive?value:(metric==NeutralRankingMetric::Exclusive?value/2:value/3);
                    Require(total==expected,"scale rank metric total");
                    const auto current=std::stod(row.at("cumulative_contribution").get<std::string>());
                    Require(current>=cumulative && current<=1,"scale monotonic contribution"); cumulative=current; ++ordinal;
                }
                Require(page.nextOrdinal==ordinal,"scale ranking cursor"); if(page.done) break;
            } while(true);
            Require(ordinal==count && cumulative==1,"scale complete ranking and final contribution"); rankingRecords+=ordinal;
        }
        for(const auto index:{uint64_t(0),count/2,count-1})
            Require(json::parse(cache.Signature("cpu",ScaleId(index),"Player")->payload).at("signature_id")==ScaleId(index),"scale individual lookup after paging");
    }
    const auto verified=std::chrono::steady_clock::now(); uint64_t diskBytes=0;
    for(const auto& entry:std::filesystem::recursive_directory_iterator(root/"cache"))
        if(entry.is_regular_file()) diskBytes+=entry.file_size();
    std::cout<<json{{"verified",true},{"records",count},{"ranking_records",rankingRecords},
        {"materialized_result_peak",summary.materializedResultPeak},{"cache_disk_bytes",diskBytes},
        {"build_ms",std::chrono::duration_cast<std::chrono::milliseconds>(built-start).count()},
        {"reopen_and_verify_ms",std::chrono::duration_cast<std::chrono::milliseconds>(verified-built).count()},
        {"scope","synthetic exact statistics + contexts + all rankings; excludes Worker, policy and MCP"}}.dump()<<'\n';
}
}
int main(int argc,char** argv)
{
    const auto root=std::filesystem::temp_directory_path()/("jn-neutral-cache-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        if(argc==3 && std::string(argv[1])=="--scale-cache") { Scale(root,std::stoull(argv[2])); std::filesystem::remove_all(root); return 0; }
        HeaderDecodeWorkspace(root/"header-workspace"); RankingDecodeWorkspace(root/"ranking-workspace"); CurrentWriterWorkspace(root/"writer-workspace"); Matrix(root/"matrix"); KeyIdentity(root/"keys"); Publication(root/"publication");
        AuditRejectsInvalidContract(root/"audit"); MatchesLegacyOracle(root/"parity"); CancellationAndCorruption(root/"failure");
        ResourceAndPublicationBoundaries(root/"boundaries");
        std::filesystem::remove_all(root); std::cout<<"neutral statistics cache tests passed\n"; return 0; }
    catch(const std::exception& e) { std::cerr<<e.what()<<"\nfixture: "<<root<<'\n'; return 1; }
}

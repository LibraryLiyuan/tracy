#include "TracyCandidatePolicy.hpp"
#include "TracyNeutralStatisticsCache.hpp"
#include "TracyAnalysisCacheKey.hpp"
#include <iostream>
#include <chrono>
#include <stdexcept>
using namespace tracy::analysis;
using nlohmann::json;
namespace
{
void Require(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
void BinaryKeyJson()
{
    std::string bytes; for(int i=0;i<256;++i) bytes.push_back(char(i));
    const auto payload=json{{"key",cache_key::Hex(bytes)}}.dump();
    Require(cache_key::Unhex(json::parse(payload).at("key").get<std::string>())==bytes,
        "all binary key bytes survive a JSON storage boundary");
    Require(cache_key::Unhex(cache_key::Hex(cache_key::Unsigned(128)))==cache_key::Unsigned(128),
        "ordinal 128 is not treated as UTF-8");
}
void SignatureRecord()
{
    NeutralSignatureAggregate input; input.domain="cpu"; input.signatureId="thread:7/full/path"; input.frameScope="Player";
    input.logical=true; input.exact=false; input.completeFrameCount=17; input.presentFrameCount=4;
    input.occurrenceCount=9; input.unknownFrameCount=2;
    auto& metric=input.exclusive;
    metric.perCompleteFrame={false,17,11,123,0,99,7.235294117647059,0,1.5,0,10,20,80,"unknown_frame"};
    metric.whenPresent={true,4,0,123,3,99,30.75,10.5,4.5,10.5,73.2,86.1,96.42,{}};
    metric.pattern=AnomalyPattern::BurstWindow; metric.anomalies={{8,99,88.5},{9,12,1.5}};
    metric.anomalyCount=8; metric.anomaliesComplete=false; metric.longestBurstFrames=7;
    metric.longestBurstStartFrame=4; metric.longestBurstEndFrame=10; metric.longestBurstPeakFrame=8; metric.periodFrames=3;
    input.inclusive=metric; input.wait=metric; input.criticalPath=metric;
    const auto payload=SerializeNeutralSignatureRecord(input);
    NeutralSignatureAggregate output; std::string error;
    const auto parsed=DeserializeNeutralSignatureRecord(payload,output,error);
    Require(parsed,error.c_str());
    Require(output.domain=="cpu" && output.signatureId=="thread:7/full/path" && output.frameScope=="Player","full signature identity");
    Require(!output.exact && output.logical && output.unknownFrameCount==2,"unknown and logical survive");
    Require(output.exclusive.perCompleteFrame.p95==20 && output.wait.anomalies.size()==2,"metrics and anomaly representatives survive");
    Require(output.inclusive.longestBurstEndFrame==10 && output.criticalPath.periodFrames==3,"burst summaries survive");
    Require(SerializeNeutralSignatureRecord(output)==payload,"single-statistic complete field roundtrip");
    Require(!DeserializeNeutralSignatureRecord("{}",output,error) && output.signatureId.empty(),"invalid statistics return no partial record");
}
void ContextRecord()
{
    PolicySignatureContext input;
    input.domain="gpu"; input.signatureId="signature/full"; input.familyId="family"; input.parentSignatureId="parent";
    input.name="Same.Name"; input.path="Queue/Root/Same.Name"; input.frameScope="l0:queue:5";
    input.metricPreference="inclusive"; input.moduleBudgetId="module"; input.budgetScope="when_present";
    input.frameRoot=true; input.provenCriticalPath=true; input.frameSeriesComplete=true; input.threadOrQueue="queue:5";
    input.seriesOffset=9007199254740993ull; input.seriesCount=5309; input.seriesSha256=std::string(64,'a');
    input.observationUnit="l0_segment";
    input.sourceOrdinal=123;
    input.frames={{7,99,{"event:7"},"structure:A",false,100,200}, {8,0,{},"",true,std::nullopt,std::nullopt}};
    const auto payload=SerializePolicySignatureContextRecord(input);
    Require(!payload.empty(),"context serializer must produce a record");
    const auto document=json::parse(payload);
    Require(document.at("path")=="Queue/Root/Same.Name" && document.at("parent_signature_id")=="parent","context identity not display name only");
    Require(document.at("series_offset")=="9007199254740993","series offsets retain uint64 precision");
    Require(document.at("frames").at(0).at("exact")==false,"unknown frame is not silently restored as exact");
    Require(document.at("frames").at(0).at("begin_ns")=="100" && document.at("frames").at(0).at("end_ns")=="200","L0 interval boundaries survive");
    PolicySignatureContext output; std::string error;
    const auto parsed=DeserializePolicySignatureContextRecord(payload,output,error);
    Require(parsed,error.c_str());
    Require(output.frames.size()==2 && !output.frames[0].exact && output.frames[1].exact,"unknown differs from known zero");
    Require(output.frames[0].beginNs==100 && !output.frames[1].beginNs,"missing interval remains unavailable");
    Require(output.seriesOffset==9007199254740993ull && output.observationUnit=="l0_segment","L0 is not inferred as PlayerFrame");
    Require(output.sourceOrdinal==123,"context ingestion order survives key sorting");
    Require(SerializePolicySignatureContextRecord(output)==payload,"context complete field roundtrip");
    Require(!DeserializePolicySignatureContextRecord("{}",output,error) && output.signatureId.empty(),"invalid context returns no partial record");
}
void JoinedCacheInput()
{
    const auto root=std::filesystem::temp_directory_path()/("jn-policy-cache-input-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const std::string identity(64,'a');
    constexpr uint64_t Count=40;
    auto context=[](uint64_t i) {
        PolicySignatureContext c; c.domain="cpu"; c.signatureId="same"; c.name="Same.Display.Name";
        c.frameScope="scope-"+std::to_string(i); c.path="Thread/FullPath/"+c.frameScope; c.parentSignatureId="parent:"+c.frameScope;
        c.frames={{7,int64_t(100+i),{"event:"+std::to_string(i)},{},i!=0,100,200}};
        c.frameSeriesComplete=true; c.sourceOrdinal=i; return c;
    };
    NeutralStatisticsCacheOptions options; options.table.blockBytes=16384; options.sortBufferBytes=32768;
    {
        NeutralStatisticsCacheWriter writer(root,identity,options);
        for(uint64_t i=Count;i-->0;)
        {
            NeutralSignatureAggregate s; s.domain="cpu"; s.signatureId="same"; s.frameScope="scope-"+std::to_string(i);
            s.inclusive.perCompleteFrame.total=100+i; s.exact=i!=0; s.unknownFrameCount=i==0?1:0;
            writer.AppendStatistics(s);
        }
        for(uint64_t i=0;i<Count;++i) writer.AppendContext(SerializePolicySignatureContextRecord(context(i)));
        auto wall=context(0); wall.signatureId="root"; wall.frameRoot=true; wall.sourceOrdinal=Count;
        writer.AppendContext(SerializePolicySignatureContextRecord(wall));
        NeutralStatisticsStreamSummary summary; summary.signatureCount=Count; summary.qualityComplete=true;
        NeutralDomainAuditResult audit; audit.domain="cpu"; audit.present=true; audit.status="complete";
        audit.inputCount=audit.consumedInputCount=audit.outputCount=audit.actualOutputCount=Count;
        audit.inputChecksum=audit.consumedChecksum=identity; audit.qualityComplete=true; summary.domains.push_back(audit);
        writer.Commit(summary);
    }
    {
        NeutralStatisticsCacheReader reader(root,identity,options);
        uint64_t seen=0,roots=0;
        const auto visited=reader.VisitSignatureContexts([&](const NeutralSignatureAggregate* stat,const PolicySignatureContext& c){
            if(c.frameRoot) { Require(!stat && c.sourceOrdinal==Count,"root context must not acquire another signature's statistics or order"); ++roots; return; }
            Require(stat && stat->signatureId=="same" && stat->frameScope==c.frameScope,"merge join uses full key");
            const auto index=std::stoull(c.frameScope.substr(6)); Require(index<Count,"joined scope range");
            Require(c.sourceOrdinal==index,"source order survives sorted cache traversal");
            Require(stat->inclusive.perCompleteFrame.total==int64_t(100+index),"joined metric belongs to this scope");
            Require(c.path=="Thread/FullPath/"+c.frameScope && c.parentSignatureId=="parent:"+c.frameScope,"joined structural context intact");
            Require(c.frames[0].exact==(index!=0) && stat->exact==(index!=0),"joined unknown state intact");
            Require(c.frames[0].beginNs==100 && c.frames[0].endNs==200,"joined interval intact"); ++seen;
        });
        Require(visited==Count+1 && seen==Count && roots==1,"visit every joined pair and root exactly once");
    }
    {
        bool cancel=false; options.table.cancelled=[&]{return cancel;};
        NeutralStatisticsCacheReader reader(root,identity,options); uint64_t seen=0;
        bool rejected=false;
        try { reader.VisitSignatureContexts([&](const auto*,const auto&){++seen; cancel=true;}); }
        catch(const std::exception& e) { rejected=std::string(e.what()).find("cancelled")!=std::string::npos; }
        Require(rejected && seen==1,"join cancellation stops before further callbacks");
    }
    std::filesystem::remove_all(root);
}
}
int main()
{
    try { BinaryKeyJson(); SignatureRecord(); ContextRecord(); JoinedCacheInput(); std::cout<<"policy cache input tests passed\n"; return 0; }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}

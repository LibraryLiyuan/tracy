#include "TracyNeutralStatisticsCache.hpp"
#include "TracyAnalysisCacheSort.hpp"
#include "TracyAnalysisCacheKey.hpp"
#include "TracyCandidatePolicy.hpp"
#include <algorithm>
#include <iomanip>
#include <locale>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <stdexcept>
namespace tracy::analysis
{
namespace
{
using nlohmann::json;
constexpr uint32_t CacheSchema = 1;
[[noreturn]] void Fail(const char* reason) { throw std::runtime_error(std::string("neutral_statistics_cache_")+reason); }
void CheckCancel(const NeutralStatisticsCacheOptions& options)
{ if(options.table.cancelled && options.table.cancelled()) Fail("cancelled"); }
// Order-preserving tuple encoding. Escaped NUL and terminators are distinct,
// including for prefix strings and embedded NULs. No name/hash-only identity.
std::string Field(std::string_view value)
{ return cache_key::Field(value); }
std::string Key(std::string_view domain, std::string_view signature, std::string_view scope)
{ return Field(domain)+Field(signature)+Field(scope); }
std::string Key(const json& row)
{ return Key(row.at("domain").get<std::string>(),row.at("signature_id").get<std::string>(),row.at("frame_scope").get<std::string>()); }
std::string Scope(const json& row)
{ return Field(row.at("domain").get<std::string>())+Field(row.at("frame_scope").get<std::string>()); }
const char* Metric(NeutralRankingMetric metric)
{
    switch(metric) {
    case NeutralRankingMetric::Inclusive:return "inclusive";
    case NeutralRankingMetric::Exclusive:return "exclusive";
    case NeutralRankingMetric::WaitCritical:return "wait_critical";
    } Fail("ranking_metric");
}
std::string Descending(int64_t value)
{
    const auto ordered=~(uint64_t(value) ^ (uint64_t(1)<<63));
    std::string result;
    for(int shift=56;shift>=0;shift-=8) result.push_back(char(ordered>>shift));
    return result;
}
std::string DoubleString(double value)
{
    if(value==0) return "0";
    std::ostringstream out; out.imbue(std::locale::classic()); out<<std::setprecision(17)<<value; return out.str();
}
std::string Kind(const char* name) { return std::string("neutral-cache-v1-")+name; }
json Description(const AnalysisCacheTableDescriptor& d)
{ return {{"records",d.records},{"blocks",d.blocks},{"content_sha256",d.contentSha256}}; }
void CheckDescription(const AnalysisCacheTableReader& r, const json& expected)
{ if(Description(r.Descriptor())!=expected) Fail("table_identity_mismatch"); }
int64_t Total(const json& row, const char* metric)
{ return std::stoll(row.at(metric).at("per_complete_frame").at("total_ns").get<std::string>()); }
bool Digest(std::string_view value)
{ return value.size()==64 && std::all_of(value.begin(),value.end(),[](char c){return (c>='0' && c<='9') || (c>='a' && c<='f') || (c>='A' && c<='F');}); }
}
struct NeutralStatisticsCacheWriter::Impl
{
    std::filesystem::path root;
    std::string identity;
    NeutralStatisticsCacheOptions options;
    AnalysisWorkspaceReservation metadataWorkspace;
    std::unique_ptr<AnalysisCacheSortedWriter> statistics, contexts;
    // Only domain/scope metadata is resident; no signature/ranking vector.
    std::map<std::string,bool> physicalScopes;
    std::map<std::string,uint64_t> domainCounts;
    uint64_t metadataBytes=0, count=0;
    bool failed=false, committed=false;
    Impl(std::filesystem::path path,std::string id,NeutralStatisticsCacheOptions config)
        : root(std::move(path)),identity(std::move(id)),options(std::move(config)),metadataWorkspace(options.table.workspace)
    {
        if(options.scopeMetadataBytes<1024 || options.scopeMetadataBytes>64*1024*1024) Fail("scope_metadata_budget");
        Check();
        if(root.filename().empty() || std::filesystem::exists(root)) Fail("already_exists");
        std::filesystem::create_directories(root.parent_path());
        if(!std::filesystem::create_directory(root)) Fail("already_exists");
        statistics=Sorter("statistics"); contexts=Sorter("contexts");
    }
    void Check()
    { if(failed) Fail("writer_failed"); if(committed) Fail("writer_closed"); CheckCancel(options); }
    void ReserveMetadata(uint64_t bytes)
    {
        if(bytes>options.scopeMetadataBytes-metadataBytes) Fail("scope_metadata_budget");
        metadataWorkspace.Add(16*bytes); // Maps, audit copies and their small JSON summary coexist.
        metadataBytes+=bytes;
    }
    std::unique_ptr<AnalysisCacheSortedWriter> Sorter(const char* name)
    { return std::make_unique<AnalysisCacheSortedWriter>(root/name,identity,Kind(name),options.table,options.sortBufferBytes); }
    AnalysisCacheTableReader Reader(const char* name)
    { return AnalysisCacheTableReader(root/name,identity,Kind(name),options.table); }
    json Summary(const NeutralStatisticsStreamSummary& summary)
    {
        if(!summary.qualityComplete || summary.unreportedGapCount) Fail("quality_incomplete");
        if(summary.signatureCount!=count) Fail("count_mismatch");
        std::set<std::string> audited;
        for(const auto& d:summary.domains)
        {
            ReserveMetadata(512+2*(d.domain.size()+d.status.size()+d.inputChecksum.size()+d.consumedChecksum.size()+d.unavailableReason.size()));
            const bool status=d.status=="complete" || d.status=="absent" || d.status=="invalid" || d.status=="unsupported";
            if(d.domain.empty() || !status || !Digest(d.inputChecksum) || !Digest(d.consumedChecksum) ||
                !audited.insert(d.domain).second) Fail("domain_audit");
            const auto found=domainCounts.find(d.domain);
            const auto actual=found==domainCounts.end()?0:found->second;
            if(d.actualOutputCount!=actual || d.outputCount!=actual || !d.qualityComplete ||
                d.inputCount!=d.consumedInputCount || d.inputChecksum!=d.consumedChecksum) Fail("domain_audit");
        }
        for(const auto& [domain,n]:domainCounts) if(!audited.contains(domain)) Fail("domain_audit");
        for(const auto& finding:summary.qualityFindings) ReserveMetadata(64+2*finding.size());
        // Reuse the legacy audit encoding only for this bounded, signature-free
        // object. Never serialize a materialized result on this path.
        NeutralStatisticsResult small;
        small.qualityComplete=summary.qualityComplete; small.unreportedGapCount=summary.unreportedGapCount;
        small.qualityFindings=summary.qualityFindings; small.domains=summary.domains;
        const auto audit=json::parse(SerializeNeutralStatisticsResult(small));
        return {{"signature_count",std::to_string(count)}, {"quality",audit.at("quality")},
            {"domains",audit.at("domains")}, {"materialized_result_peak",std::to_string(summary.materializedResultPeak)}};
    }
    void ValidateContexts()
    {
        auto stats=Reader("statistics"), ctx=Reader("contexts");
        uint64_t si=0,ci=0;
        std::optional<AnalysisCacheRecord> s,c;
        if(si<stats.RecordCount()) s=stats.GetAt(si);
        if(ci<ctx.RecordCount()) c=ctx.GetAt(ci);
        // Merge-join once. Root FrameSet contexts legitimately have no stats.
        while(s || c)
        {
            Check();
            if(s && (!c || s->key<c->key)) Fail("context_missing");
            if(c && (!s || c->key<s->key))
            {
                AnalysisWorkspaceReservation decodingWorkspace(options.table.workspace,65536+32ull*c->payload.size());
                if(!json::parse(c->payload).value("frame_root",false)) Fail("context_orphan");
            }
            else { ++si; s=si<stats.RecordCount()?std::optional(stats.GetAt(si)):std::nullopt; }
            ++ci; c=ci<ctx.RecordCount()?std::optional(ctx.GetAt(ci)):std::nullopt;
        }
    }
    void BuildRankings(json& descriptions)
    {
        {
            auto output=Sorter("ranking-input"); auto stats=Reader("statistics");
            for(uint64_t i=0;i<stats.RecordCount();++i)
            {
                Check(); const auto record=stats.GetAt(i);
                AnalysisWorkspaceReservation decodingWorkspace(options.table.workspace,65536+32ull*record.payload.size());
                const auto row=json::parse(record.payload);
                if(!row.at("exact").get<bool>()) continue;
                const auto scope=Scope(row);
                const bool logical=row.at("logical").get<bool>();
                if(logical && physicalScopes.at(scope)) continue;
                const auto signature=row.at("signature_id").get<std::string>();
                const auto wait=Total(row,"wait"), critical=Total(row,"critical_path");
                for(const auto metric:{NeutralRankingMetric::Inclusive,NeutralRankingMetric::Exclusive,NeutralRankingMetric::WaitCritical})
                {
                    const bool wc=metric==NeutralRankingMetric::WaitCritical;
                    const auto total=wc?std::max(wait,critical):Total(row,Metric(metric));
                    const auto group=scope+Field(Metric(metric));
                    json rank={{"signature_id",signature},{"frame_scope",row.at("frame_scope")},{"logical",logical},
                        {"total_ns",std::to_string(total)},{"wait_ns",std::to_string(wc?wait:0)},
                        {"critical_path_ns",std::to_string(wc?critical:0)}};
                    output->Append(group+Descending(total)+Field(signature),
                        json{{"domain",row.at("domain")},{"metric",Metric(metric)},{"entry",std::move(rank)}}.dump());
                }
            }
            output->Commit();
        }
        auto input=Reader("ranking-input");
        AnalysisCacheTableWriter output(root/"rankings",identity,Kind("rankings"),options.table);
        AnalysisCacheTableWriter groups(root/"ranking-groups",identity,Kind("ranking-groups"),options.table);
        const auto groupKey=[](const json& row) {
            return Field(row.at("domain").get<std::string>())+
                Field(row.at("entry").at("frame_scope").get<std::string>())+Field(row.at("metric").get<std::string>());
        };
        // Two sequential passes per group preserve the legacy summation order,
        // including double rounding and the final cumulative=1 rule.
        uint64_t begin=0;
        while(begin<input.RecordCount())
        {
            Check(); const auto firstRecord=input.GetAt(begin);
            AnalysisWorkspaceReservation groupWorkspace(options.table.workspace,65536+32ull*firstRecord.payload.size());
            const auto first=json::parse(firstRecord.payload); const auto group=groupKey(first);
            uint64_t end=begin; long double total=0;
            while(end<input.RecordCount())
            {
                const auto record=input.GetAt(end);
                AnalysisWorkspaceReservation decodingWorkspace(options.table.workspace,65536+32ull*record.payload.size());
                const auto row=json::parse(record.payload);
                if(groupKey(row)!=group) break;
                const auto value=std::stoll(row.at("entry").at("total_ns").get<std::string>());
                if(value>0) total+=value; ++end;
            }
            long double cumulative=0;
            for(uint64_t i=begin;i<end;++i)
            {
                Check(); auto record=input.GetAt(i);
                AnalysisWorkspaceReservation decodingWorkspace(options.table.workspace,65536+32ull*record.payload.size());
                auto entry=json::parse(record.payload).at("entry");
                const auto value=std::stoll(entry.at("total_ns").get<std::string>());
                const double contribution=total==0?0:double(static_cast<long double>(std::max<int64_t>(value,0))/total);
                cumulative+=contribution;
                const double running=(i+1==end && total!=0)?1.0:std::min(1.0,double(cumulative));
                entry["contribution"]=DoubleString(contribution); entry["cumulative_contribution"]=DoubleString(running);
                output.Append(record.key,entry.dump());
            }
            groups.Append(group,json{{"domain",first.at("domain")},{"frame_scope",first.at("entry").at("frame_scope")},
                {"metric",first.at("metric")},{"begin",begin},{"count",end-begin}}.dump()); begin=end;
        }
        descriptions["rankings"]=Description(output.Commit());
        descriptions["ranking-groups"]=Description(groups.Commit());
        // Verify the newly written final tables with bounded reads before the
        // parent header can become visible. Indexes/blocks enforce checksums.
        for(const auto name:{"rankings","ranking-groups"})
        {
            auto reader=Reader(name); CheckDescription(reader,descriptions.at(name));
            for(uint64_t i=0;i<reader.RecordCount();++i) reader.GetAt(i);
        }
    }
};
NeutralStatisticsCacheWriter::NeutralStatisticsCacheWriter(std::filesystem::path root,std::string identity,NeutralStatisticsCacheOptions options)
    :m_impl(std::make_unique<Impl>(std::move(root),std::move(identity),std::move(options))) {}
NeutralStatisticsCacheWriter::~NeutralStatisticsCacheWriter()=default;
void NeutralStatisticsCacheWriter::AppendStatistics(const NeutralSignatureAggregate& signature)
{
    auto& s=*m_impl;
    try {
        s.Check();
        AnalysisWorkspaceReservation encodingWorkspace(s.options.table.workspace,
            65536+32ull*(signature.domain.size()+signature.signatureId.size()+signature.frameScope.size()));
        for(const auto* metric:{&signature.inclusive,&signature.exclusive,&signature.wait,&signature.criticalPath})
            encodingWorkspace.Add(2048ull*metric->anomalies.size()+32ull*
                (metric->perCompleteFrame.unavailableReason.size()+metric->whenPresent.unavailableReason.size()));
        const auto scope=Field(signature.domain)+Field(signature.frameScope);
        if(!s.physicalScopes.contains(scope)) { s.ReserveMetadata(256+2*scope.size()); s.physicalScopes.emplace(scope,false); }
        if(signature.exact && !signature.logical) s.physicalScopes.at(scope)=true;
        if(!s.domainCounts.contains(signature.domain)) { s.ReserveMetadata(256+2*signature.domain.size()); s.domainCounts.emplace(signature.domain,0); }
        s.statistics->Append(Key(signature.domain,signature.signatureId,signature.frameScope),SerializeNeutralSignatureRecord(signature));
        ++s.domainCounts.at(signature.domain); ++s.count;
    } catch(...) { s.failed=true; throw; }
}
void NeutralStatisticsCacheWriter::AppendContext(std::string_view payload)
{
    auto& s=*m_impl;
    try { s.Check(); if(payload.size()>s.options.table.blockBytes) Fail("context_record_budget");
        AnalysisWorkspaceReservation decodingWorkspace(s.options.table.workspace,65536+32ull*payload.size());
        s.contexts->Append(Key(json::parse(payload)),payload); }
    catch(...) { s.failed=true; throw; }
}
void NeutralStatisticsCacheWriter::Commit(const NeutralStatisticsStreamSummary& summary)
{
    auto& s=*m_impl;
    try {
        s.Check(); const auto small=s.Summary(summary);
        json descriptions;
        descriptions["statistics"]=Description(s.statistics->Commit());
        descriptions["contexts"]=Description(s.contexts->Commit());
        s.statistics.reset(); s.contexts.reset();
        s.ValidateContexts(); s.BuildRankings(descriptions); s.Check();
        AnalysisCacheTableWriter header(s.root/"header",s.identity,Kind("header"),s.options.table);
        header.Append("header",json{{"schema",CacheSchema},{"summary",small},{"tables",descriptions}}.dump());
        header.Commit(); s.committed=true;
    } catch(...) { s.failed=true; throw; }
}
struct NeutralStatisticsCacheReader::Impl
{
    NeutralStatisticsCacheOptions options;
    AnalysisWorkspaceReservation summaryWorkspace;
    std::string summary;
    std::string cacheIdentity, contentSha256;
    std::unique_ptr<AnalysisCacheTableReader> statistics,contexts,rankings,groups;
    Impl(const std::filesystem::path& root,const std::string& identity,NeutralStatisticsCacheOptions config)
        :options(std::move(config)),summaryWorkspace(options.table.workspace)
    {
        CheckCancel(options);
        if(!std::filesystem::exists(root/"header"/"manifest.json")) Fail("incomplete");
        AnalysisCacheTableReader header(root/"header",identity,Kind("header"),options.table);
        cacheIdentity=identity; contentSha256=header.Descriptor().contentSha256;
        if(header.RecordCount()!=1) Fail("header_count");
        auto record=header.GetAt(0);
        AnalysisWorkspaceReservation decodingWorkspace(options.table.workspace,65536+32ull*record.payload.size());
        const auto document=json::parse(record.payload);
        if(record.key!="header" || document.at("schema").get<uint32_t>()!=CacheSchema) Fail("header_schema");
        const auto& tables=document.at("tables");
        const auto open=[&](const char* name) {
            auto r=std::make_unique<AnalysisCacheTableReader>(root/name,identity,Kind(name),options.table);
            CheckDescription(*r,tables.at(name)); return r;
        };
        statistics=open("statistics"); contexts=open("contexts"); rankings=open("rankings"); groups=open("ranking-groups");
        const auto& small=document.at("summary");
        if(!small.at("quality").at("complete").get<bool>() || small.at("quality").at("unreported_gap_count")!="0") Fail("quality_incomplete");
        if(small.at("signature_count")!=std::to_string(statistics->RecordCount())) Fail("count_mismatch");
        summaryWorkspace.Resize(512+2ull*record.payload.size());
        summary=small.dump();
    }
};
NeutralStatisticsCacheReader::NeutralStatisticsCacheReader(std::filesystem::path root,std::string identity,NeutralStatisticsCacheOptions options)
    :m_impl(std::make_unique<Impl>(root,identity,std::move(options))) {}
NeutralStatisticsCacheReader::~NeutralStatisticsCacheReader()=default;
uint64_t NeutralStatisticsCacheReader::SignatureCount() const { return m_impl->statistics->RecordCount(); }
std::string NeutralStatisticsCacheReader::Identity() const { return m_impl->cacheIdentity; }
std::string NeutralStatisticsCacheReader::ContentSha256() const { return m_impl->contentSha256; }
std::string NeutralStatisticsCacheReader::SummaryJson() const { CheckCancel(m_impl->options); return m_impl->summary; }
uint64_t NeutralStatisticsCacheReader::SummaryBytes() const { CheckCancel(m_impl->options); return m_impl->summary.size(); }
std::optional<AnalysisCacheRecord> NeutralStatisticsCacheReader::Signature(std::string_view domain,std::string_view signature,std::string_view scope)
{ return m_impl->statistics->Find(Key(domain,signature,scope)); }
std::optional<AnalysisCacheRecord> NeutralStatisticsCacheReader::Context(std::string_view domain,std::string_view signature,std::string_view scope)
{ return m_impl->contexts->Find(Key(domain,signature,scope)); }
AnalysisCachePage NeutralStatisticsCacheReader::Signatures(uint64_t ordinal,size_t limit,uint64_t bytes)
{ return m_impl->statistics->ReadPage(ordinal,limit,bytes); }
AnalysisCacheRecord NeutralStatisticsCacheReader::SignatureAt(uint64_t ordinal)
{ return m_impl->statistics->GetAt(ordinal); }
AnalysisCachePage NeutralStatisticsCacheReader::Contexts(uint64_t ordinal,size_t limit,uint64_t bytes)
{ return m_impl->contexts->ReadPage(ordinal,limit,bytes); }
AnalysisCachePage NeutralStatisticsCacheReader::RankingGroups(uint64_t ordinal,size_t limit,uint64_t bytes)
{ return m_impl->groups->ReadPage(ordinal,limit,bytes); }
uint64_t NeutralStatisticsCacheReader::VisitSignatureContexts(const std::function<void(
    const NeutralSignatureAggregate*,const PolicySignatureContext&)>& sink)
{
    if(!sink) Fail("signature_context_sink_missing");
    auto& state=*m_impl;
    CheckCancel(state.options);
    uint64_t si=0,visited=0;
    std::optional<AnalysisCacheRecord> stat;
    if(si<state.statistics->RecordCount()) stat=state.statistics->GetAt(si);
    for(uint64_t ci=0;ci<state.contexts->RecordCount();++ci)
    {
        CheckCancel(state.options);
        const auto record=state.contexts->GetAt(ci);
        const auto decodedBytes=record.payload.size()+
            (stat && stat->key==record.key ? stat->payload.size() : 0);
        // Parsed DTOs and the temporary JSON DOM outlive the loaded block's
        // byte buffer. Reserve them separately, including through the sink.
        AnalysisWorkspaceReservation decodedWorkspace(state.options.table.workspace,65536+32ull*decodedBytes);
        PolicySignatureContext context; std::string error;
        if(!DeserializePolicySignatureContextRecord(record.payload,context,error)) throw std::runtime_error(error);
        if(Key(context.domain,context.signatureId,context.frameScope)!=record.key) Fail("context_key_mismatch");
        if(stat && stat->key<record.key) Fail("context_missing");
        if(stat && stat->key==record.key)
        {
            NeutralSignatureAggregate value;
            if(!DeserializeNeutralSignatureRecord(stat->payload,value,error)) throw std::runtime_error(error);
            if(Key(value.domain,value.signatureId,value.frameScope)!=stat->key) Fail("statistics_key_mismatch");
            sink(&value,context);
            ++si;
            stat=si<state.statistics->RecordCount()?std::optional(state.statistics->GetAt(si)):std::nullopt;
        }
        else
        {
            if(!context.frameRoot) Fail("context_orphan");
            sink(nullptr,context);
        }
        ++visited;
    }
    CheckCancel(state.options);
    if(stat) Fail("context_missing");
    return visited;
}
AnalysisCachePage NeutralStatisticsCacheReader::Ranking(std::string_view domain,std::string_view scope,NeutralRankingMetric metric,uint64_t ordinal,size_t limit,uint64_t bytes)
{
    if(limit==0 || limit>1000 || bytes==0 || bytes>16*1024*1024) Fail("page_budget");
    const auto group=m_impl->groups->Find(Field(domain)+Field(scope)+Field(Metric(metric)));
    if(!group) return {{},ordinal,true};
    AnalysisWorkspaceReservation decodingWorkspace(m_impl->options.table.workspace,65536+32ull*group->payload.size());
    const auto position=json::parse(group->payload);
    const auto begin=position.at("begin").get<uint64_t>(), count=position.at("count").get<uint64_t>();
    if(begin>m_impl->rankings->RecordCount() || count>m_impl->rankings->RecordCount()-begin) Fail("ranking_range");
    if(ordinal>count) Fail("ordinal_out_of_range");
    if(ordinal==count) return {{},ordinal,true};
    auto page=m_impl->rankings->ReadPage(begin+ordinal,size_t(std::min<uint64_t>(limit,count-ordinal)),bytes);
    page.nextOrdinal-=begin; page.done=page.nextOrdinal==count; return page;
}
}

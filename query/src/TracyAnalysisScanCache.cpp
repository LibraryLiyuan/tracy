#include "TracyAnalysisScanCache.hpp"
#include "TracyAnalysisScanManager.hpp"
#include "TracyAnalysisCacheKey.hpp"
#include "TracyNeutralStatisticsCache.hpp"
#include "TracyHash.hpp"
#include <array>
#include <fstream>
#include <stdexcept>
namespace tracy::query
{
namespace
{
using namespace tracy::analysis;
using nlohmann::json;
[[noreturn]] void Fail(const char* reason) { throw std::runtime_error(std::string("analysis_scan_bundle_")+reason); }
void Check(const AnalysisCacheTableOptions& options) { if(options.cancelled && options.cancelled()) Fail("cancelled"); }
json Descriptor(const AnalysisCacheTableDescriptor& d)
{ return {{"records",d.records},{"blocks",d.blocks},{"content_sha256",d.contentSha256}}; }
json FileIdentity(const std::filesystem::path& path,const AnalysisCacheTableOptions& options)
{
    std::ifstream input(path,std::ios::binary); if(!input) Fail("series_missing");
    std::array<char,65536> buffer; Sha256Builder hash; uint64_t bytes=0;
    while(input) {
        Check(options); input.read(buffer.data(),buffer.size()); const auto count=input.gcount();
        if(count>0) { hash.Update(buffer.data(),size_t(count)); bytes+=uint64_t(count); }
    }
    if(!input.eof()) Fail("series_read");
    Check(options); return {{"bytes",bytes},{"sha256",hash.FinalHex()}};
}
json Frame(const PolicyFrameEvidence& f)
{
    return json::array({std::to_string(f.frameIndex),std::to_string(f.valueNs),f.exact,f.eventRefs,f.structureKey,
        f.beginNs?json(std::to_string(*f.beginNs)):json(nullptr),f.endNs?json(std::to_string(*f.endNs)):json(nullptr)});
}
PolicyFrameEvidence ParseFrame(const json& j)
{
    if(!j.is_array() || j.size()!=7) Fail("frame_schema");
    PolicyFrameEvidence f{std::stoull(j[0].get<std::string>()),std::stoll(j[1].get<std::string>()),
        j[3].get<std::vector<std::string>>(),j[4].get<std::string>(),j[2].get<bool>()};
    if(!j[5].is_null()) f.beginNs=std::stoll(j[5].get<std::string>());
    if(!j[6].is_null()) f.endNs=std::stoll(j[6].get<std::string>());
    return f;
}
// Compatibility for deliberately injected small executors. The default
// executor has already streamed its entire output and never takes this branch.
void PersistInjected(const std::filesystem::path& generation,const std::string& identity,
    AnalysisScanProducts& p,const AnalysisCacheTableOptions& options)
{
    if(p.aggregate.signatures.size()>4096 || p.signatureContexts.size()>4096) Fail("injected_product_budget");
    NeutralStatisticsCacheOptions config; config.table=options;
    NeutralStatisticsCacheWriter writer(generation/"neutral-statistics-v1",identity,config);
    NeutralStatisticsStreamSummary summary;
    summary.signatureCount=p.aggregate.signatures.size(); summary.materializedResultPeak=p.aggregate.signatures.size();
    summary.qualityComplete=p.aggregate.qualityComplete; summary.unreportedGapCount=p.aggregate.unreportedGapCount;
    summary.qualityFindings=p.aggregate.qualityFindings; summary.domains=p.aggregate.domains;
    for(const auto& s:p.aggregate.signatures) writer.AppendStatistics(s);
    for(size_t i=0;i<p.signatureContexts.size();++i) {
        p.signatureContexts[i].sourceOrdinal=i;
        writer.AppendContext(SerializePolicySignatureContextRecord(p.signatureContexts[i]));
    }
    writer.Commit(summary);
    p.neutralCachePath=generation/"neutral-statistics-v1"; p.neutralCacheIdentity=identity;
    p.aggregate.signatures.clear(); p.aggregate.rankings.clear();
    std::vector<PolicySignatureContext>().swap(p.signatureContexts);
}
}
std::string PublishAnalysisNeutralBundle(const std::filesystem::path& generation,
    const std::string& identity,AnalysisScanProducts& products,AnalysisCacheTableOptions options)
{
    Check(options);
    AnalysisWorkspaceReservation documentWorkspace(options.workspace,65536+32ull*identity.size());
    if(products.neutralCachePath.empty()) PersistInjected(generation,identity,products,options);
    if(products.neutralCachePath!=generation/"neutral-statistics-v1" || products.neutralCacheIdentity!=identity)
        Fail("neutral_location_or_identity");
    NeutralStatisticsCacheOptions neutralOptions; neutralOptions.table=options;
    NeutralStatisticsCacheReader neutral(products.neutralCachePath,identity,neutralOptions);
    const auto series=generation/"frame-series-v1.bin";
    if(products.frameSeriesPath.empty()) {
        if(std::filesystem::exists(series)) Fail("series_already_exists");
        std::ofstream output(series,std::ios::binary); if(!output) Fail("series_create");
    }
    else if(products.frameSeriesPath!=series) {
        AnalysisDiskGrow(options.disk,std::filesystem::file_size(products.frameSeriesPath));
        std::filesystem::copy_file(products.frameSeriesPath,series);
    }
    products.frameSeriesPath=series;
    const auto seriesIdentity=FileIdentity(series,options);
    AnalysisCacheTableWriter metadata(generation/"metadata",identity,"analysis-neutral-metadata-v1",options);
    for(size_t i=0;i<products.capacityFacts.size();++i) {
        Check(options); const auto& v=products.capacityFacts[i];
        AnalysisWorkspaceReservation rowWorkspace(options.workspace,65536+32ull*
            (v.domain.size()+v.signatureId.size()+v.familyId.size()+v.metric.size()+v.description.size()));
        metadata.Append("capacity"+cache_key::Unsigned(i),json{{"type","capacity"},{"ordinal",i},
            {"domain",v.domain},{"signature_id",v.signatureId},{"family_id",v.familyId},{"metric",v.metric},
            {"value_bytes",std::to_string(v.valueBytes)},{"frame_index",v.frameIndex?json(std::to_string(*v.frameIndex)):json(nullptr)},
            {"exact",v.exact},{"description",v.description}}.dump());
    }
    for(size_t i=0;i<products.frameTimelines.size();++i) {
        Check(options); const auto& t=products.frameTimelines[i]; const auto key="timeline"+cache_key::Unsigned(i);
        {
            AnalysisWorkspaceReservation rowWorkspace(options.workspace,65536+32ull*
                (t.frameScope.size()+t.name.size()+t.observationUnit.size()));
            metadata.Append(key+"0",json{{"type","timeline"},{"ordinal",i},{"frame_scope",t.frameScope},
                {"name",t.name},{"observation_unit",t.observationUnit},{"frame_count",t.frames.size()}}.dump());
        }
        for(size_t j=0;j<t.frames.size();++j) {
            Check(options); const auto& frame=t.frames[j];
            AnalysisWorkspaceReservation rowWorkspace(options.workspace,65536+32ull*frame.structureKey.size());
            for(const auto& ref:frame.eventRefs) rowWorkspace.Add(2048+32ull*ref.size());
            metadata.Append(key+"1"+cache_key::Unsigned(j),
                json{{"type","frame"},{"timeline",i},{"ordinal",j},{"frame",Frame(frame)}}.dump());
        }
    }
    const auto md=metadata.Commit();
    const json document={{"schema",1},{"format",AnalysisScanCacheFormatId},{"identity",identity},
        {"neutral_content_sha256",neutral.ContentSha256()},{"metadata",Descriptor(md)},{"frame_series",seriesIdentity}};
    Check(options);
    AnalysisCacheTableWriter header(generation/"bundle-header",identity,"analysis-neutral-bundle-v1",options);
    header.Append("header",document.dump()); return header.Commit().contentSha256;
}
AnalysisScanProducts OpenAnalysisNeutralBundle(const std::filesystem::path& generation,
    const std::string& identity,const std::string& headerSha,AnalysisCacheTableOptions options,uint64_t metadataBytes)
{
    Check(options);
    AnalysisCacheTableReader header(generation/"bundle-header",identity,"analysis-neutral-bundle-v1",options);
    if(header.RecordCount()!=1 || header.Descriptor().contentSha256!=headerSha) Fail("header_identity");
    const auto record=header.GetAt(0);
    AnalysisWorkspaceReservation headerWorkspace(options.workspace,65536+32ull*record.payload.size());
    const auto j=json::parse(record.payload);
    if(record.key!="header" || j.at("schema")!=1 || j.at("format")!=AnalysisScanCacheFormatId || j.at("identity")!=identity) Fail("header_schema");
    AnalysisScanProducts products;
    products.metadataWorkspace=AnalysisWorkspaceReservation(options.workspace);
    products.neutralCachePath=generation/"neutral-statistics-v1"; products.neutralCacheIdentity=identity;
    NeutralStatisticsCacheOptions neutralOptions; neutralOptions.table=options;
    NeutralStatisticsCacheReader neutral(products.neutralCachePath,identity,neutralOptions);
    if(neutral.ContentSha256()!=j.at("neutral_content_sha256").get<std::string>()) Fail("neutral_content_identity");
    products.frameSeriesPath=generation/"frame-series-v1.bin";
    if(FileIdentity(products.frameSeriesPath,options)!=j.at("frame_series")) Fail("series_identity");
    AnalysisCacheTableReader metadata(generation/"metadata",identity,"analysis-neutral-metadata-v1",options);
    if(Descriptor(metadata.Descriptor())!=j.at("metadata")) Fail("metadata_identity");
    uint64_t retained=0,expectedFrames=0;
    const auto finishTimeline=[&] {
        if(!products.frameTimelines.empty() && products.frameTimelines.back().frames.size()!=expectedFrames) Fail("frame_count");
    };
    for(uint64_t i=0;i<metadata.RecordCount();++i) {
        Check(options); const auto row=metadata.GetAt(i);
        const auto bytes=uint64_t(row.payload.size())*4+512;
        if(retained>metadataBytes || bytes>metadataBytes-retained) Fail("metadata_budget");
        products.metadataWorkspace.Add(bytes);
        AnalysisWorkspaceReservation parseWorkspace(options.workspace,65536+32ull*row.payload.size());
        retained+=bytes; const auto v=json::parse(row.payload); const auto type=v.at("type").get<std::string>();
        if(type=="capacity") {
            const auto index=products.capacityFacts.size();
            if(v.at("ordinal")!=index || row.key!="capacity"+cache_key::Unsigned(index)) Fail("capacity_order");
            PolicyCapacityFact f; f.domain=v.at("domain").get<std::string>(); f.signatureId=v.at("signature_id").get<std::string>();
            f.familyId=v.at("family_id").get<std::string>(); f.metric=v.at("metric").get<std::string>();
            f.valueBytes=std::stoull(v.at("value_bytes").get<std::string>());
            if(!v.at("frame_index").is_null()) f.frameIndex=std::stoull(v.at("frame_index").get<std::string>());
            f.exact=v.at("exact").get<bool>(); f.description=v.at("description").get<std::string>();
            products.capacityFacts.push_back(std::move(f));
        } else if(type=="timeline") {
            finishTimeline(); const auto index=products.frameTimelines.size();
            if(v.at("ordinal")!=index || row.key!="timeline"+cache_key::Unsigned(index)+"0") Fail("timeline_order");
            expectedFrames=v.at("frame_count").get<uint64_t>();
            if(expectedFrames>metadataBytes/(4*sizeof(PolicyFrameEvidence))) Fail("metadata_budget");
            products.frameTimelines.push_back({v.at("frame_scope").get<std::string>(),v.at("name").get<std::string>(),{},
                v.at("observation_unit").get<std::string>()});
        } else if(type=="frame") {
            if(products.frameTimelines.empty()) Fail("frame_timeline_missing");
            const auto ti=products.frameTimelines.size()-1, fi=products.frameTimelines.back().frames.size();
            if(v.at("timeline")!=ti || v.at("ordinal")!=fi || fi>=expectedFrames ||
                row.key!="timeline"+cache_key::Unsigned(ti)+"1"+cache_key::Unsigned(fi)) Fail("frame_order");
            products.frameTimelines.back().frames.push_back(ParseFrame(v.at("frame")));
        } else Fail("metadata_type");
    }
    finishTimeline(); Check(options); return products;
}
}

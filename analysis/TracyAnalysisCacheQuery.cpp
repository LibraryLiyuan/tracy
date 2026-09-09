#include "TracyAnalysisCacheQuery.hpp"
#include "TracyAnalysisCacheKey.hpp"
#include "TracyAnalysisIoPath.hpp"
#include "TracyHash.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
#ifdef _WIN32
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
namespace tracy::analysis
{
namespace
{
using nlohmann::json;
[[noreturn]] void Fail(const char* reason) { throw std::runtime_error(std::string("analysis_scan_")+reason); }
uint64_t Number(const std::string& value)
{
    if(value.empty() || !std::all_of(value.begin(),value.end(),[](char c){return c>='0' && c<='9';})) Fail("invalid_cursor");
    uint64_t result=0;
    for(const char c:value) {
        const auto digit=uint64_t(c-'0');
        if(result>(std::numeric_limits<uint64_t>::max()-digit)/10) Fail("invalid_cursor");
        result=result*10+digit;
    }
    return result;
}
std::string Hash(std::string_view value)
{ Sha256Builder h; h.Update(value.data(),value.size()); return h.FinalHex(); }
void Publish(const std::filesystem::path& temporary,const std::filesystem::path& target)
{
    // A completed table is nonempty and immutable. A concurrent builder may
    // have published the same source/filter first; never replace its table.
    if(std::filesystem::exists(target)) return;
#ifdef _WIN32
    if(!MoveFileExW(temporary.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH) &&
        !std::filesystem::exists(target/"manifest.json")) Fail("filter_publish_failed");
#else
    std::error_code ec;
    std::filesystem::rename(temporary,target,ec);
    if(ec && !std::filesystem::exists(target/"manifest.json")) Fail("filter_publish_failed");
    const auto directory=open(target.parent_path().c_str(),O_RDONLY | O_DIRECTORY);
    if(directory<0) Fail("filter_directory_flush_failed");
    const auto flushed=fsync(directory); close(directory);
    if(flushed!=0) Fail("filter_directory_flush_failed");
#endif
}
}
struct AnalysisCacheQuery::Impl
{
    std::filesystem::path root;
    std::string identity,currentFilter;
    uint64_t records,responseBytes;
    std::function<AnalysisCacheRecord(uint64_t)> read;
    AnalysisCacheTableOptions options;
    std::unique_ptr<AnalysisCacheTableReader> index;
    Impl(std::filesystem::path path,std::string id,uint64_t count,
        std::function<AnalysisCacheRecord(uint64_t)> getter,AnalysisCacheTableOptions config,uint64_t bytes)
        :root(AnalysisIoPath(path)),identity(std::move(id)),records(count),responseBytes(bytes),
        read(std::move(getter)),options(std::move(config))
    {
        if(root.empty() || identity.size()!=64 || !read || bytes<2 || bytes>16*1024*1024) Fail("invalid_query_configuration");
        Check();
    }
    void Check() const { if(options.cancelled && options.cancelled()) Fail("cancelled"); }
    std::filesystem::path NewGeneration(const std::filesystem::path& parent)
    {
        const auto generations=parent/"generations";
        std::filesystem::create_directories(generations);
        for(uint64_t generation=0;generation!=UINT64_MAX;++generation)
        {
            Check();
            const auto path=generations/("gen-"+std::to_string(generation));
            // Reserve the namespace atomically, even across Query processes.
            // Failed/cancelled generations remain untouched and are not read.
            if(std::filesystem::create_directory(path)) return path/"table";
        }
        Fail("filter_generations_exhausted");
    }
    void Filter(const json& filter)
    {
        Check();
        const auto encoded=filter.dump();
        if(encoded.size()>64*1024) Fail("filter_budget");
        const auto id=Hash("analysis-query-filter-v2\n"+identity+"\n"+std::to_string(records)+"\n"+encoded);
        if(index && currentFilter==id) return;
        index.reset(); currentFilter.clear();
        const auto path=root/id/"complete";
        if(!std::filesystem::exists(path))
        {
            AnalysisDiskActivity diskActivity(options.disk?options.disk->usage:nullptr);
            const auto temporary=NewGeneration(root/id);
            uint64_t matches=0;
            {
                AnalysisCacheTableWriter writer(temporary,id,"analysis-query-filter-v2",options);
                for(uint64_t i=0;i<records;++i)
                {
                    Check(); const auto item=json::parse(read(i).payload);
                    bool match=true;
                    for(const auto& [key,expected]:filter.items())
                        if(!item.contains(key) || item.at(key)!=expected) { match=false; break; }
                    if(match) writer.Append(cache_key::Unsigned(matches++),std::to_string(i));
                }
                Check(); writer.Commit();
            }
            {
                AnalysisCacheTableReader verify(temporary,id,"analysis-query-filter-v2",options);
                if(verify.RecordCount()!=matches) Fail("filter_index_count");
            }
            Check(); Publish(temporary,path);
        }
        index=std::make_unique<AnalysisCacheTableReader>(path,id,"analysis-query-filter-v2",options);
        if(index->RecordCount()>records) Fail("filter_index_count");
        currentFilter=id;
    }
    json Page(size_t limit,const std::string& cursor,const std::vector<std::string>& fields,const json& filter)
    {
        Check(); if(limit==0 || limit>1000) Fail("invalid_page");
        const auto offset=cursor.empty()?0:Number(cursor);
        if(options.blockBytes<64 || options.blockBytes>64*1024*1024) Fail("invalid_query_configuration");
        // JSON can be far larger in memory than its wire encoding. Reserve
        // conservative DOM/copy headroom before decoding a source record or
        // constructing this one bounded response. Retention by the protocol
        // caller after return is covered by its lifecycle/process guard.
        AnalysisWorkspaceReservation pageMemory(options.workspace,
            64*1024+32*(options.blockBytes+responseBytes));
        const bool filtered=filter.is_object() && !filter.empty();
        if(filtered) Filter(filter);
        const auto total=filtered?index->RecordCount():records;
        json items=json::array(); uint64_t payloadBytes=2; bool byteLimited=false;
        for(uint64_t i=offset;i<total && items.size()<limit;++i)
        {
            Check(); uint64_t source=i;
            if(filtered) {
                const auto entry=index->GetAt(i);
                if(entry.key!=cache_key::Unsigned(i)) Fail("filter_index_key");
                source=Number(entry.payload);
                if(source>=records) Fail("filter_index_ordinal");
            }
            const auto item=json::parse(read(source).payload);
            json projected;
            if(fields.empty()) projected=item;
            else { projected=json::object(); for(const auto& field:fields) if(item.contains(field)) projected[field]=item[field]; }
            const auto itemBytes=projected.dump().size()+(items.empty()?0:1);
            if(items.empty() && itemBytes>responseBytes-2) Fail("page_item_too_large");
            if(itemBytes>responseBytes-payloadBytes) { byteLimited=true; break; }
            payloadBytes+=itemBytes; items.push_back(std::move(projected));
        }
        const auto end=offset>total?total:offset+items.size(); Check();
        return {{"items",std::move(items)},{"page",{{"cursor",std::to_string(offset)},
            {"next_cursor",end<total?json(std::to_string(end)):json(nullptr)},{"done",end>=total},
            {"total",std::to_string(total)},{"byte_limited",byteLimited},{"payload_bytes",std::to_string(payloadBytes)}}}};
    }
};
AnalysisCacheQuery::AnalysisCacheQuery(std::filesystem::path root,std::string identity,uint64_t records,
    std::function<AnalysisCacheRecord(uint64_t)> read,AnalysisCacheTableOptions options,uint64_t bytes)
    :m_impl(std::make_unique<Impl>(std::move(root),std::move(identity),records,std::move(read),std::move(options),bytes)) {}
AnalysisCacheQuery::~AnalysisCacheQuery()=default;
nlohmann::json AnalysisCacheQuery::Page(size_t limit,const std::string& cursor,
    const std::vector<std::string>& fields,const nlohmann::json& filter)
{ return m_impl->Page(limit,cursor,fields,filter); }
}

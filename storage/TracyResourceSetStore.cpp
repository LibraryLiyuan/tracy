#include "TracyResourceSetStore.hpp"
#include "../analysis/TracyPagedRecordStore.hpp"
#include "../analysis/TracyAnalysisIoPath.hpp"
#include "../analysis/TracyHash.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <map>
namespace tracy::storage
{
namespace
{
constexpr uint64_t PageBytes=16384;
constexpr uint32_t PageMagic=0x31544252;
constexpr size_t Capacity=1018;
#pragma pack(push,1)
struct Item { uint32_t key=0,count=0;uint64_t value=0; };
struct Page {
    uint32_t magic=PageMagic;uint16_t level=0,count=0;uint64_t id=0;
    char checksum[64]{};uint64_t reserved[2]{};Item items[Capacity]{};
};
#pragma pack(pop)
static_assert(sizeof(Page)==PageBytes);
[[noreturn]] void Fail(const char* code){throw std::runtime_error(code);}
std::string Digest(const Page& page)
{ auto copy=page;std::memset(copy.checksum,0,64);analysis::Sha256Builder h;h.Update(&copy,sizeof(copy));return h.FinalHex(); }
analysis::PagedRecordOptions Options(ConversionResources& resources,uint64_t cache)
{
    analysis::PagedRecordOptions o;o.blockBytes=128*1024;o.activeReadCacheBytes=cache;o.workspace=resources.Workspace();
    o.cancelled=[&resources]{resources.CheckFast();return false;};
    o.beforeFileGrowth=[&resources](const auto& p,uint64_t n){resources.BeforeFileGrowth(p,n);};
    o.renameFile=[&resources](const auto& a,const auto& b){resources.RenameOwned(a,b);};return o;
}
}
struct ResourceSetStore::Impl
{
    struct CachedPage {
        analysis::AnalysisWorkspaceReservation lease;
        Page page;uint64_t touched=0;bool dirty=false;
        explicit CachedPage(ConversionResources& r):lease(r.Workspace(),PageBytes+256){}
    };
    struct Result { uint32_t minimum=0;std::optional<Item> right; };
    std::filesystem::path root,indexPath;
    std::string identity;
    ConversionResources& resources;
    analysis::AnalysisWorkspaceReservation traversal;
    std::unique_ptr<analysis::PagedRecordWriter> members;
    std::fstream index;
    std::map<uint64_t,std::unique_ptr<CachedPage>> cache;
    uint64_t cacheLimit,clock=0,nextPage=1,rootPage=0,definitions=0;
    bool sealed=false;
    Impl(std::filesystem::path path,std::string hash,ConversionResources& r,uint64_t indexBytes,uint64_t dataBytes)
        :root(std::move(path)),indexPath(root/"index.bin"),identity(std::move(hash)),resources(r),
         traversal(r.Workspace(),1ull<<20),cacheLimit(indexBytes)
    {
        if(identity.size()!=64 || indexBytes<2*(PageBytes+256) || indexBytes>32ull<<20 || dataBytes>64ull<<20)
            Fail("resource_set_configuration");
        if(std::filesystem::exists(analysis::AnalysisIoPath(root)))Fail("resource_set_already_exists");
        resources.BeforeFileGrowth(root,0);std::filesystem::create_directories(analysis::AnalysisIoPath(root));
        resources.BeforeFileGrowth(indexPath,PageBytes);
        index.open(analysis::AnalysisIoPath(indexPath),std::ios::in|std::ios::out|std::ios::binary|std::ios::trunc);
        if(!index)Fail("resource_set_index_open");
        std::array<char,PageBytes> zero{};index.write(zero.data(),zero.size());if(!index)Fail("resource_set_index_write");
        Page p;rootPage=Allocate(p);
        members=std::make_unique<analysis::PagedRecordWriter>(root/"members",identity,uint32_t(sizeof(ResourceSetMember)),Options(resources,dataBytes));
    }
    void RawWrite(Page p)
    {
        resources.CheckFast();
        const auto digest=Digest(p);std::memcpy(p.checksum,digest.data(),64);
        if(p.id>uint64_t(INT64_MAX)/PageBytes)Fail("resource_set_index_offset");
        index.clear();index.seekp(std::streamoff(p.id*PageBytes));index.write(reinterpret_cast<const char*>(&p),sizeof(p));
        if(!index)Fail("resource_set_index_write");
    }
    uint64_t Allocate(Page& p)
    {
        if(nextPage>=uint64_t(INT64_MAX)/PageBytes)Fail("resource_set_index_offset");
        resources.BeforeFileGrowth(indexPath,(nextPage+1)*PageBytes);
        p.id=nextPage++;RawWrite(p);return p.id;
    }
    void Evict()
    {
        if(cache.empty())return;
        auto victim=std::min_element(cache.begin(),cache.end(),[](const auto& a,const auto& b){return a.second->touched<b.second->touched;});
        if(victim->second->dirty)RawWrite(victim->second->page);
        cache.erase(victim);
    }
    CachedPage& Slot(uint64_t id)
    {
        if(id==0||id>=nextPage)Fail("resource_set_index_page_id");
        auto found=cache.find(id);
        if(found==cache.end()) {
            while((cache.size()+1)*(PageBytes+256)>cacheLimit)Evict();
            auto value=std::make_unique<CachedPage>(resources);
            index.flush();index.clear();index.seekg(std::streamoff(id*PageBytes));
            index.read(reinterpret_cast<char*>(&value->page),sizeof(Page));
            const auto& p=value->page;
            if(!index || p.id!=id || p.magic!=PageMagic || p.level>=16 || p.count>Capacity ||
                (p.level&&p.count==0) || Digest(p)!=std::string(p.checksum,64))Fail("resource_set_index_checksum");
            for(size_t i=1;i<p.count;++i)if(p.items[i-1].key>=p.items[i].key)Fail("resource_set_index_order");
            found=cache.emplace(id,std::move(value)).first;
        }
        found->second->touched=++clock;return *found->second;
    }
    Page Get(uint64_t id){return Slot(id).page;}
    void Put(const Page& page){auto& slot=Slot(page.id);slot.page=page;slot.dirty=true;}
    std::optional<ResourceSetDefinition> Lookup(uint32_t key)
    {
        resources.CheckFast();uint64_t id=rootPage;uint16_t prior=16;
        for(unsigned depth=0;depth<16;++depth) {
            const auto p=Get(id);if(p.level>=prior)Fail("resource_set_index_depth");prior=p.level;
            const auto begin=p.items,end=begin+p.count;
            if(!p.level) {
                const auto at=std::lower_bound(begin,end,key,[](const Item& a,uint32_t b){return a.key<b;});
                if(at==end||at->key!=key)return {};
                if(!at->count || at->count>65535)Fail("resource_set_index_member_count");
                return ResourceSetDefinition{at->value,at->count};
            }
            auto at=std::upper_bound(begin,end,key,[](uint32_t a,const Item& b){return a<b.key;});
            if(at!=begin)--at;id=at->value;
        }
        Fail("resource_set_index_depth");
    }
    Result Insert(uint64_t id,Item value,unsigned depth=0)
    {
        if(depth>=16)Fail("resource_set_index_depth");
        auto p=Get(id);std::array<Item,Capacity+1> items;
        std::copy_n(p.items,p.count,items.begin());size_t count=p.count;
        if(!p.level) {
            auto at=std::lower_bound(items.begin(),items.begin()+count,value.key,[](const Item& a,uint32_t b){return a.key<b;});
            if(at!=items.begin()+count && at->key==value.key)Fail("resource_set_duplicate");
            const auto position=size_t(at-items.begin());std::move_backward(at,items.begin()+count,items.begin()+count+1);
            items[position]=value;++count;
        }else {
            auto at=std::upper_bound(items.begin(),items.begin()+count,value.key,[](uint32_t a,const Item& b){return a<b.key;});
            if(at!=items.begin())--at;const auto position=size_t(at-items.begin());
            const auto child=Insert(at->value,value,depth+1);items[position].key=child.minimum;
            if(child.right) {
                std::move_backward(items.begin()+position+1,items.begin()+count,items.begin()+count+1);
                items[position+1]=*child.right;++count;
            }
        }
        if(count<=Capacity) {p.count=uint16_t(count);std::copy_n(items.begin(),count,p.items);Put(p);return {p.items[0].key,{}};}
        const auto middle=count/2;p.count=uint16_t(middle);std::copy_n(items.begin(),middle,p.items);Put(p);
        Page right;right.level=p.level;right.count=uint16_t(count-middle);std::copy(items.begin()+middle,items.begin()+count,right.items);
        Allocate(right);return {p.items[0].key,Item{right.items[0].key,0,right.id}};
    }
    void Flush()
    {
        for(auto& item:cache)if(item.second->dirty){RawWrite(item.second->page);item.second->dirty=false;}
        std::array<char,PageBytes> header{};std::memcpy(header.data(),"JNSETBT1",8);std::memcpy(header.data()+8,identity.data(),64);
        std::memcpy(header.data()+72,&rootPage,8);std::memcpy(header.data()+80,&nextPage,8);
        std::memcpy(header.data()+88,&definitions,8);
        index.clear();index.seekp(0);index.write(header.data(),header.size());index.flush();if(!index)Fail("resource_set_index_flush");
    }
};
ResourceSetStore::ResourceSetStore(std::filesystem::path path,std::string identity,ConversionResources& resources,uint64_t indexBytes,uint64_t dataBytes)
    :m_impl(std::make_unique<Impl>(std::move(path),std::move(identity),resources,indexBytes,dataBytes)){}
ResourceSetStore::~ResourceSetStore()=default;
bool ResourceSetStore::Define(uint32_t id,std::span<const ResourceSetMember> data)
{
    auto& s=*m_impl;s.resources.CheckFast();if(s.sealed)Fail("resource_set_closed");
    if(!id||data.empty()||data.size()>65535||s.Lookup(id))return false;
    for(const auto& member:data)if(!member.resourceId)return false;
    const auto first=s.members->Count();s.members->Append(data.data(),data.size());
    const auto result=s.Insert(s.rootPage,{id,uint32_t(data.size()),first});
    if(result.right) {
        Page root;root.level=uint16_t(s.Get(s.rootPage).level+1);root.count=2;
        root.items[0]={result.minimum,0,s.rootPage};root.items[1]=*result.right;
        s.rootPage=s.Allocate(root);
    }
    ++s.definitions;return true;
}
std::optional<ResourceSetDefinition> ResourceSetStore::Lookup(uint32_t id){return m_impl->Lookup(id);}
void ResourceSetStore::Read(ResourceSetDefinition d,uint32_t offset,std::span<ResourceSetMember> output)
{
    auto& s=*m_impl;
    if(offset>d.count||output.size()>d.count-offset||d.first>s.members->Count()||d.count>s.members->Count()-d.first)
        Fail("resource_set_read_bounds");
    s.members->Read(d.first+offset,output.data(),output.size());
}
void ResourceSetStore::ClearCaches(){auto& s=*m_impl;s.Flush();s.cache.clear();s.members->ClearReadCache();}
void ResourceSetStore::Seal(){auto& s=*m_impl;if(s.sealed)Fail("resource_set_closed");s.Flush();s.members->Commit();s.sealed=true;}
uint64_t ResourceSetStore::Count() const{return m_impl->definitions;}
}

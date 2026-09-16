#ifndef TRACY_DISK_INDEX64_HPP
#define TRACY_DISK_INDEX64_HPP
#include "TracyConversionResources.hpp"
#include "../analysis/TracyAnalysisIoPath.hpp"
#include "../analysis/TracyHash.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <list>
#include <map>
#include <optional>
namespace tracy::storage {
// Task-local B+ tree. Pages can be evicted; references never escape the cache.
// Interrupted trees are discarded rather than resumed.
class DiskIndex64 {
    static constexpr uint64_t PageBytes=16384,HeaderBytes=4096,PageCharge=PageBytes+256;
    static constexpr size_t Capacity=1018;
    struct Item {uint64_t key=0,value=0;};
    struct Page {
        uint32_t magic=0x31583649;uint16_t level=0,count=0;uint64_t id=0;
        char checksum[64]{};uint64_t reserved[2]{};std::array<Item,Capacity> items{};
    };
    static_assert(sizeof(Page)==PageBytes);
    struct Slot {
        analysis::AnalysisWorkspaceReservation lease;Page page;std::list<uint64_t>::iterator lru;bool dirty=false;
        explicit Slot(ConversionResources& r):lease(r.Workspace(),PageCharge){}
    };
    struct Split {uint64_t minimum;std::optional<Item> right;};
public:
    struct Stats {uint64_t finds=0,upserts=0,pageLoads=0,pageWrites=0,evictions=0,evictionCandidates=0,pageCopies=0,parentUpdates=0,sameValueUpserts=0;};
private:
    std::filesystem::path m_path;std::string m_identity;ConversionResources& m_resources;
    analysis::AnalysisWorkspaceReservation m_traversal;
    std::fstream m_file;std::map<uint64_t,std::unique_ptr<Slot>> m_cache;std::list<uint64_t> m_lru;
    uint64_t m_cacheBytes,m_next=1,m_root=0,m_count=0;Stats m_stats{};bool m_sealed=false,m_failed=false;
    static void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
    static uint64_t Offset(uint64_t id){Require(id>0&&id<(uint64_t(INT64_MAX)-HeaderBytes)/PageBytes,"disk_index64_offset");return HeaderBytes+(id-1)*PageBytes;}
    static std::string Digest(Page page){std::memset(page.checksum,0,64);analysis::Sha256Builder hash;hash.Update(&page,sizeof(page));return hash.FinalHex();}
    void Check(){m_resources.CheckFast();Require(!m_failed,"disk_index64_failed");}
    void Write(Page page){
        Check();const auto hash=Digest(page);std::memcpy(page.checksum,hash.data(),64);
        m_file.clear();m_file.seekp(std::streamoff(Offset(page.id)));m_file.write(reinterpret_cast<const char*>(&page),sizeof(page));Require(bool(m_file),"disk_index64_write");++m_stats.pageWrites;
    }
    uint64_t Allocate(Page& page){
        Require(m_next<(uint64_t(INT64_MAX)-HeaderBytes)/PageBytes,"disk_index64_offset");m_resources.BeforeFileGrowth(m_path,HeaderBytes+m_next*PageBytes);
        page.id=m_next++;Write(page);return page.id;
    }
    void Evict(){
        Require(!m_lru.empty(),"disk_index64_cache");++m_stats.evictions;++m_stats.evictionCandidates;const auto id=m_lru.front();m_lru.pop_front();auto found=m_cache.find(id);Require(found!=m_cache.end(),"disk_index64_lru");
        if(found->second->dirty)Write(found->second->page);m_cache.erase(found);
    }
    Slot& Cached(uint64_t id){
        Check();Require(id>0&&id<m_next,"disk_index64_page_id");auto found=m_cache.find(id);
        if(found==m_cache.end()){
            while((m_cache.size()+1)*PageCharge>m_cacheBytes)Evict();auto slot=std::make_unique<Slot>(m_resources);
            m_file.flush();Require(bool(m_file),"disk_index64_flush");m_file.clear();m_file.seekg(std::streamoff(Offset(id)));m_file.read(reinterpret_cast<char*>(&slot->page),sizeof(Page));
            const auto& page=slot->page;Require(bool(m_file)&&page.magic==0x31583649&&page.id==id&&page.level<16&&page.count<=Capacity&&(!page.level||page.count),"disk_index64_header");
            Require(Digest(page)==std::string(page.checksum,64),"disk_index64_checksum");
            for(size_t i=1;i<page.count;++i)Require(page.items[i-1].key<page.items[i].key,"disk_index64_order");
            m_lru.push_back(id);slot->lru=std::prev(m_lru.end());found=m_cache.emplace(id,std::move(slot)).first;++m_stats.pageLoads;
        }else{
            m_lru.splice(m_lru.end(),m_lru,found->second->lru);found->second->lru=std::prev(m_lru.end());
        }
        return *found->second;
    }
    Split Insert(uint64_t id,Item value,bool& added,unsigned depth=0){
        Require(depth<16,"disk_index64_depth");
        auto& initial=Cached(id).page;
        if(!initial.level){
            auto& slot=Cached(id);auto& page=slot.page;auto begin=page.items.begin(),end=begin+page.count;
            auto at=std::lower_bound(begin,end,value.key,[](const Item& a,uint64_t b){return a.key<b;});
            if(at!=end&&at->key==value.key){if(at->value==value.value)++m_stats.sameValueUpserts;else{at->value=value.value;slot.dirty=true;}return {page.items[0].key,{}};}
            const auto position=size_t(at-begin);added=true;
            if(page.count<Capacity){std::move_backward(begin+position,end,end+1);page.items[position]=value;++page.count;slot.dirty=true;return {page.items[0].key,{}};}
            std::array<Item,Capacity+1> items{};std::copy_n(begin,page.count,items.begin());std::move_backward(items.begin()+position,items.begin()+page.count,items.begin()+page.count+1);items[position]=value;
            const auto middle=(Capacity+1)/2;page.count=uint16_t(middle);std::copy_n(items.begin(),middle,page.items.begin());slot.dirty=true;
            Page right;right.level=page.level;right.count=uint16_t(Capacity+1-middle);std::copy(items.begin()+middle,items.end(),right.items.begin());Allocate(right);return {page.items[0].key,Item{right.items[0].key,right.id}};
        }
        const auto begin=initial.items.begin(),end=begin+initial.count;auto at=std::upper_bound(begin,end,value.key,[](uint64_t a,const Item& b){return a<b.key;});if(at!=begin)--at;
        const auto position=size_t(at-begin),childId=at->value,oldMinimum=at->key;
        const auto child=Insert(childId,value,added,depth+1);
        auto& slot=Cached(id);auto& page=slot.page;Require(position<page.count&&page.items[position].value==childId,"disk_index64_parent_changed");
        bool changed=false;if(child.minimum!=oldMinimum){page.items[position].key=child.minimum;changed=true;++m_stats.parentUpdates;}
        if(!child.right){if(changed)slot.dirty=true;return {page.items[0].key,{}};}
        ++m_stats.parentUpdates;
        if(page.count<Capacity){std::move_backward(page.items.begin()+position+1,page.items.begin()+page.count,page.items.begin()+page.count+1);page.items[position+1]=*child.right;++page.count;slot.dirty=true;return {page.items[0].key,{}};}
        std::array<Item,Capacity+1> items{};std::copy_n(page.items.begin(),page.count,items.begin());std::move_backward(items.begin()+position+1,items.begin()+page.count,items.begin()+page.count+1);items[position+1]=*child.right;
        const auto middle=(Capacity+1)/2;page.count=uint16_t(middle);std::copy_n(items.begin(),middle,page.items.begin());slot.dirty=true;
        Page right;right.level=page.level;right.count=uint16_t(Capacity+1-middle);std::copy(items.begin()+middle,items.end(),right.items.begin());Allocate(right);return {page.items[0].key,Item{right.items[0].key,right.id}};
    }
    template<class Visitor> void VisitPage(uint64_t id,uint64_t first,uint64_t last,Visitor& visitor,unsigned depth=0){
        Require(depth<16,"disk_index64_depth");const auto level=Cached(id).page.level,count=Cached(id).page.count;
        if(!level){const auto& page=Cached(id).page;for(uint16_t i=0;i<count;++i){const auto item=page.items[i];if(item.key<first)continue;if(item.key>last)break;visitor(item.key,item.value);}return;}
        for(uint16_t i=0;i<count;++i){const auto& page=Cached(id).page;const auto childFirst=page.items[i].key;const auto childLast=i+1<count?page.items[i+1].key-1:UINT64_MAX;const auto childId=page.items[i].value;if(childLast<first||childFirst>last)continue;VisitPage(childId,first,last,visitor,depth+1);}
    }
public:
    DiskIndex64(std::filesystem::path path,std::string identity,ConversionResources& resources,uint64_t cacheBytes=8ull<<20)
        :m_path(std::move(path)),m_identity(std::move(identity)),m_resources(resources),m_traversal(resources.Workspace(),1ull<<20),m_cacheBytes(cacheBytes){
        Require(m_identity.size()==64&&cacheBytes>=2*PageCharge&&cacheBytes<=32ull<<20,"disk_index64_configuration");Require(!std::filesystem::exists(analysis::AnalysisIoPath(m_path)),"disk_index64_exists");
        m_resources.BeforeFileGrowth(m_path,HeaderBytes);m_file.open(analysis::AnalysisIoPath(m_path),std::ios::in|std::ios::out|std::ios::binary|std::ios::trunc);Require(bool(m_file),"disk_index64_open");
        std::array<char,HeaderBytes> header{};m_file.write(header.data(),header.size());Require(bool(m_file),"disk_index64_write");Page root;m_root=Allocate(root);
    }
    ~DiskIndex64()=default;
    std::optional<uint64_t> Find(uint64_t key){
        Check();++m_stats.finds;uint64_t id=m_root;uint16_t prior=16;
        for(unsigned depth=0;depth<16;++depth){const auto& page=Cached(id).page;Require(page.level<prior,"disk_index64_depth");prior=page.level;auto begin=page.items.begin(),end=begin+page.count;
            if(!page.level){auto at=std::lower_bound(begin,end,key,[](const Item& a,uint64_t b){return a.key<b;});if(at==end||at->key!=key)return {};return at->value;}
            auto at=std::upper_bound(begin,end,key,[](uint64_t a,const Item& b){return a<b.key;});if(at!=begin)--at;id=at->value;
        }throw std::runtime_error("disk_index64_depth");
    }
    void Upsert(uint64_t key,uint64_t value){
        Check();Require(!m_sealed,"disk_index64_sealed");++m_stats.upserts;try{bool added=false;auto result=Insert(m_root,{key,value},added);
            if(result.right){const auto oldLevel=Cached(m_root).page.level;Require(oldLevel<15,"disk_index64_depth");Page root;root.level=oldLevel+1;root.count=2;root.items[0]={result.minimum,m_root};root.items[1]=*result.right;m_root=Allocate(root);}
            if(added)++m_count;
        }catch(...){m_failed=true;m_resources.Fail("disk_index64_upsert");throw;}
    }
    template<class Visitor> void VisitRange(uint64_t first,uint64_t last,Visitor&& visitor){Check();Require(first<=last,"disk_index64_range");VisitPage(m_root,first,last,visitor);}
    void ClearCache(){Check();for(auto& item:m_cache)if(item.second->dirty)Write(item.second->page);m_file.flush();Require(bool(m_file),"disk_index64_flush");m_cache.clear();m_lru.clear();}
    void ReduceCache(){m_cacheBytes=std::min<uint64_t>(m_cacheBytes,4ull<<20);while(m_cache.size()*PageCharge>m_cacheBytes)Evict();}
    void Seal(){Check();Require(!m_sealed,"disk_index64_sealed");ClearCache();std::array<char,HeaderBytes> header{};std::memcpy(header.data(),"JNIDX642",8);std::memcpy(header.data()+8,m_identity.data(),64);std::memcpy(header.data()+72,&m_root,8);std::memcpy(header.data()+80,&m_next,8);std::memcpy(header.data()+88,&m_count,8);m_file.seekp(0);m_file.write(header.data(),header.size());m_file.flush();Require(bool(m_file),"disk_index64_seal");m_sealed=true;}
    uint64_t Count()const{return m_count;}
    uint64_t LogicalBytes()const{return HeaderBytes+(m_next-1)*PageBytes;}
    Stats Statistics()const{return m_stats;}
};
}
#endif

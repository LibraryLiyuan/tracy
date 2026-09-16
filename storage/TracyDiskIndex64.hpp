#ifndef TRACY_DISK_INDEX64_HPP
#define TRACY_DISK_INDEX64_HPP
#include "TracyConversionResources.hpp"
#include "../analysis/TracyAnalysisIoPath.hpp"
#include "../analysis/TracyHash.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
namespace tracy::storage {
// Task-local B+ tree. Pages can be evicted; references never escape the cache.
// Interrupted trees are discarded rather than resumed.
class DiskIndex64 {
    static constexpr uint64_t PageBytes=16384,PageCharge=PageBytes+256;
    static constexpr size_t Capacity=1018;
    struct Item {uint64_t key=0,value=0;};
    struct Page {
        uint32_t magic=0x31583649;uint16_t level=0,count=0;uint64_t id=0;
        char checksum[64]{};uint64_t reserved[2]{};std::array<Item,Capacity> items{};
    };
    static_assert(sizeof(Page)==PageBytes);
    struct Slot {
        analysis::AnalysisWorkspaceReservation lease;Page page;uint64_t touched=0;bool dirty=false;
        explicit Slot(ConversionResources& r):lease(r.Workspace(),PageCharge){}
    };
    struct Split {uint64_t minimum;std::optional<Item> right;};
    std::filesystem::path m_path;std::string m_identity;ConversionResources& m_resources;
    analysis::AnalysisWorkspaceReservation m_traversal;
    std::fstream m_file;std::map<uint64_t,std::unique_ptr<Slot>> m_cache;
    uint64_t m_cacheBytes,m_clock=0,m_next=1,m_root=0,m_count=0;bool m_sealed=false,m_failed=false;
    static void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
    static std::string Digest(Page page){std::memset(page.checksum,0,64);analysis::Sha256Builder hash;hash.Update(&page,sizeof(page));return hash.FinalHex();}
    void Check(){m_resources.CheckFast();Require(!m_failed,"disk_index64_failed");}
    void Write(Page page){
        Check();const auto hash=Digest(page);std::memcpy(page.checksum,hash.data(),64);
        Require(page.id<uint64_t(INT64_MAX)/PageBytes,"disk_index64_offset");
        m_file.clear();m_file.seekp(std::streamoff(page.id*PageBytes));m_file.write(reinterpret_cast<const char*>(&page),sizeof(page));Require(bool(m_file),"disk_index64_write");
    }
    uint64_t Allocate(Page& page){
        Require(m_next<uint64_t(INT64_MAX)/PageBytes,"disk_index64_offset");m_resources.BeforeFileGrowth(m_path,(m_next+1)*PageBytes);
        page.id=m_next++;Write(page);return page.id;
    }
    void Evict(){
        Require(!m_cache.empty(),"disk_index64_cache");auto found=std::min_element(m_cache.begin(),m_cache.end(),[](const auto& a,const auto& b){return a.second->touched<b.second->touched;});
        if(found->second->dirty)Write(found->second->page);m_cache.erase(found);
    }
    Slot& Cached(uint64_t id){
        Check();Require(id>0&&id<m_next,"disk_index64_page_id");auto found=m_cache.find(id);
        if(found==m_cache.end()){
            while((m_cache.size()+1)*PageCharge>m_cacheBytes)Evict();auto slot=std::make_unique<Slot>(m_resources);
            m_file.flush();Require(bool(m_file),"disk_index64_flush");m_file.clear();m_file.seekg(std::streamoff(id*PageBytes));m_file.read(reinterpret_cast<char*>(&slot->page),sizeof(Page));
            const auto& page=slot->page;Require(bool(m_file)&&page.magic==0x31583649&&page.id==id&&page.level<16&&page.count<=Capacity&&(!page.level||page.count),"disk_index64_header");
            Require(Digest(page)==std::string(page.checksum,64),"disk_index64_checksum");
            for(size_t i=1;i<page.count;++i)Require(page.items[i-1].key<page.items[i].key,"disk_index64_order");
            found=m_cache.emplace(id,std::move(slot)).first;
        }
        found->second->touched=++m_clock;return *found->second;
    }
    Page Get(uint64_t id){return Cached(id).page;}
    void Put(const Page& page){auto& slot=Cached(page.id);slot.page=page;slot.dirty=true;}
    Split Insert(uint64_t id,Item value,bool& added,unsigned depth=0){
        Require(depth<16,"disk_index64_depth");auto page=Get(id);std::array<Item,Capacity+1> items{};std::copy_n(page.items.begin(),page.count,items.begin());size_t count=page.count;
        if(!page.level){
            auto at=std::lower_bound(items.begin(),items.begin()+count,value.key,[](const Item& a,uint64_t b){return a.key<b;});
            if(at!=items.begin()+count&&at->key==value.key)*at=value;
            else{auto position=size_t(at-items.begin());std::move_backward(at,items.begin()+count,items.begin()+count+1);items[position]=value;++count;added=true;}
        }else{
            auto at=std::upper_bound(items.begin(),items.begin()+count,value.key,[](uint64_t a,const Item& b){return a<b.key;});if(at!=items.begin())--at;auto position=size_t(at-items.begin());
            const auto child=Insert(at->value,value,added,depth+1);items[position].key=child.minimum;
            if(child.right){std::move_backward(items.begin()+position+1,items.begin()+count,items.begin()+count+1);items[position+1]=*child.right;++count;}
        }
        if(count<=Capacity){page.count=uint16_t(count);std::copy_n(items.begin(),count,page.items.begin());Put(page);return {page.items[0].key,{}};}
        const auto middle=count/2;page.count=uint16_t(middle);std::copy_n(items.begin(),middle,page.items.begin());Put(page);
        Page right;right.level=page.level;right.count=uint16_t(count-middle);std::copy(items.begin()+middle,items.begin()+count,right.items.begin());Allocate(right);return {page.items[0].key,Item{right.items[0].key,right.id}};
    }
public:
    DiskIndex64(std::filesystem::path path,std::string identity,ConversionResources& resources,uint64_t cacheBytes=8ull<<20)
        :m_path(std::move(path)),m_identity(std::move(identity)),m_resources(resources),m_traversal(resources.Workspace(),1ull<<20),m_cacheBytes(cacheBytes){
        Require(m_identity.size()==64&&cacheBytes>=2*PageCharge&&cacheBytes<=32ull<<20,"disk_index64_configuration");Require(!std::filesystem::exists(analysis::AnalysisIoPath(m_path)),"disk_index64_exists");
        m_resources.BeforeFileGrowth(m_path,PageBytes);m_file.open(analysis::AnalysisIoPath(m_path),std::ios::in|std::ios::out|std::ios::binary|std::ios::trunc);Require(bool(m_file),"disk_index64_open");
        std::array<char,PageBytes> header{};m_file.write(header.data(),header.size());Require(bool(m_file),"disk_index64_write");Page root;m_root=Allocate(root);
    }
    ~DiskIndex64()=default;
    std::optional<uint64_t> Find(uint64_t key){
        Check();uint64_t id=m_root;uint16_t prior=16;
        for(unsigned depth=0;depth<16;++depth){auto page=Get(id);Require(page.level<prior,"disk_index64_depth");prior=page.level;auto begin=page.items.begin(),end=begin+page.count;
            if(!page.level){auto at=std::lower_bound(begin,end,key,[](const Item& a,uint64_t b){return a.key<b;});if(at==end||at->key!=key)return {};return at->value;}
            auto at=std::upper_bound(begin,end,key,[](uint64_t a,const Item& b){return a<b.key;});if(at!=begin)--at;id=at->value;
        }throw std::runtime_error("disk_index64_depth");
    }
    void Upsert(uint64_t key,uint64_t value){
        Check();Require(!m_sealed,"disk_index64_sealed");try{bool added=false;auto result=Insert(m_root,{key,value},added);
            if(result.right){auto old=Get(m_root);Require(old.level<15,"disk_index64_depth");Page root;root.level=old.level+1;root.count=2;root.items[0]={result.minimum,m_root};root.items[1]=*result.right;m_root=Allocate(root);}
            if(added)++m_count;
        }catch(...){m_failed=true;m_resources.Fail("disk_index64_upsert");throw;}
    }
    void ClearCache(){Check();for(auto& item:m_cache)if(item.second->dirty)Write(item.second->page);m_file.flush();Require(bool(m_file),"disk_index64_flush");m_cache.clear();}
    void ReduceCache(){m_cacheBytes=std::min<uint64_t>(m_cacheBytes,4ull<<20);while(m_cache.size()*PageCharge>m_cacheBytes)Evict();}
    void Seal(){Check();Require(!m_sealed,"disk_index64_sealed");ClearCache();std::array<char,PageBytes> header{};std::memcpy(header.data(),"JNIDX641",8);std::memcpy(header.data()+8,m_identity.data(),64);std::memcpy(header.data()+72,&m_root,8);std::memcpy(header.data()+80,&m_next,8);std::memcpy(header.data()+88,&m_count,8);m_file.seekp(0);m_file.write(header.data(),header.size());m_file.flush();Require(bool(m_file),"disk_index64_seal");m_sealed=true;}
    uint64_t Count()const{return m_count;}
};
}
#endif

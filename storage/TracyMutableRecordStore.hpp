#ifndef TRACY_MUTABLE_RECORD_STORE_HPP
#define TRACY_MUTABLE_RECORD_STORE_HPP
#include "TracyConversionResources.hpp"
#include "../analysis/TracyAnalysisIoPath.hpp"
#include <array>
#include <fstream>
#include <map>
#include <memory>
#include <algorithm>
#include <cstring>
#include <stdexcept>
namespace tracy::storage
{
// Conversion-local, fixed slots. Unfinished files are never reopened for recovery.
class MutableRecordStore
{
public:
    using Record=std::array<uint32_t,4>;
    static constexpr uint64_t Rows=4096;
private:
    struct Page {
        uint64_t magic=0x325058455a,id=0,checksum=0;
        uint32_t schema=2,recordBytes=16;
        std::array<char,4096-32> reserved{};
        std::array<Record,Rows> rows{};
    };
    static_assert(sizeof(Page)==69632);
    struct Slot {
        analysis::AnalysisWorkspaceReservation lease;
        Page page;uint64_t touched=0;bool dirty=false;
        Slot(ConversionResources& r):lease(r.Workspace(),sizeof(Slot)+256){}
    };
    std::filesystem::path m_path;
    ConversionResources& m_resources;
    std::fstream m_file;
    std::map<uint64_t,std::unique_ptr<Slot>> m_cache;
    uint64_t m_count=0,m_pages=0,m_clock=0,m_limit;
    Slot* m_last=nullptr;
    bool m_sealed=false;
    static uint64_t Digest(const Page& p) {
        uint64_t h=14695981039346656037ull;
        auto bytes=reinterpret_cast<const unsigned char*>(&p);
        for(size_t i=0;i<sizeof(p);++i){h^=(i>=16&&i<24)?0:bytes[i];h*=1099511628211ull;}return h;
    }
    static void Require(bool yes,const char* why){if(!yes)throw std::runtime_error(why);}
    void Flush(Slot& slot){
        if(!slot.dirty)return;m_resources.CheckFast();
        slot.page.checksum=Digest(slot.page);
        m_file.seekp(std::streamoff(slot.page.id*sizeof(Page)));
        m_file.write(reinterpret_cast<const char*>(&slot.page),sizeof(Page));
        Require(bool(m_file),"zone_extra_page_write");slot.dirty=false;
    }
    void Evict(){
        auto v=std::min_element(m_cache.begin(),m_cache.end(),[](const auto& a,const auto& b){return a.second->touched<b.second->touched;});
        Require(v!=m_cache.end(),"zone_extra_cache_empty");Flush(*v->second);m_last=nullptr;m_cache.erase(v);
    }
    Slot& Get(uint64_t id,bool create=false){
        m_resources.CheckFast();
        if(m_last&&m_last->page.id==id){m_last->touched=++m_clock;return *m_last;}
        auto found=m_cache.find(id);
        if(found==m_cache.end()){
            while(m_cache.size()>=m_limit)Evict();
            m_resources.Check(sizeof(Slot)+256);
            auto slot=std::make_unique<Slot>(m_resources);
            if(create){
                Require(id==m_pages&&id<uint64_t(INT64_MAX)/sizeof(Page),"zone_extra_page_offset");
                m_resources.BeforeFileGrowth(m_path,(id+1)*sizeof(Page));
                slot->page.id=id;slot->dirty=true;Flush(*slot);++m_pages;
            }else{
                Require(id<m_pages,"zone_extra_page_missing");
                m_file.flush();Require(bool(m_file),"zone_extra_page_flush");
                m_file.seekg(std::streamoff(id*sizeof(Page)));
                m_file.read(reinterpret_cast<char*>(&slot->page),sizeof(Page));
                Require(bool(m_file)&&slot->page.magic==0x325058455a&&slot->page.schema==2&&slot->page.recordBytes==16&&slot->page.id==id&&slot->page.checksum==Digest(slot->page),"zone_extra_page_integrity");
            }
            found=m_cache.emplace(id,std::move(slot)).first;
        }
        m_last=found->second.get();m_last->touched=++m_clock;return *m_last;
    }
public:
    MutableRecordStore(std::filesystem::path path,ConversionResources& r,uint64_t cachedPages=0):m_path(std::move(path)),m_resources(r),m_limit(cachedPages?cachedPages:(32ull<<20)/(sizeof(Slot)+256)){
        Require(m_limit>=1&&m_limit<=(32ull<<20)/(sizeof(Slot)+256),"zone_extra_cache_limit");
        Require(!std::filesystem::exists(analysis::AnalysisIoPath(m_path)),"zone_extra_file_exists");
        r.BeforeFileGrowth(m_path,0);
        m_file.open(analysis::AnalysisIoPath(m_path),std::ios::in|std::ios::out|std::ios::binary|std::ios::trunc);
        Require(bool(m_file),"zone_extra_file_open");
    }
    ~MutableRecordStore() noexcept=default;
    uint64_t Count()const{return m_count;}
    uint32_t Append(const Record& value){
        Require(!m_sealed&&m_count<=UINT32_MAX,"zone_extra_append_limit");
        auto& slot=Get(m_count/Rows,m_count/Rows==m_pages);
        slot.page.rows[m_count%Rows]=value;slot.dirty=true;return uint32_t(m_count++);
    }
    Record Read(uint32_t id){Require(id<m_count,"zone_extra_index");return Get(id/Rows).page.rows[id%Rows];}
    void Put(uint32_t id,const Record& value){Require(!m_sealed&&id<m_count,"zone_extra_update_state");auto& slot=Get(id/Rows);slot.page.rows[id%Rows]=value;slot.dirty=true;}
    void ReduceCache(){m_limit=std::min<uint64_t>(m_limit,(4ull<<20)/(sizeof(Slot)+256));while(m_cache.size()>m_limit)Evict();}
    void Seal(){
        Require(!m_sealed,"zone_extra_already_sealed");for(auto& s:m_cache)Flush(*s.second);
        m_file.flush();Require(bool(m_file),"zone_extra_seal_flush");
        m_file.close();Require(!m_file.fail(),"zone_extra_close");
        m_cache.clear();m_last=nullptr;
        m_file.open(analysis::AnalysisIoPath(m_path),std::ios::in|std::ios::binary);
        Require(bool(m_file),"zone_extra_reopen");m_sealed=true;
    }
    template<class F> void Visit(F f){
        Require(m_sealed,"zone_extra_not_sealed");
        for(uint64_t base=0;base<m_count;base+=Rows){auto& s=Get(base/Rows);f(s.page.rows.data(),std::min(Rows,m_count-base));}
    }
};
}
#endif

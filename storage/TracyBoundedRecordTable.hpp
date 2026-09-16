#ifndef TRACY_BOUNDED_RECORD_TABLE_HPP
#define TRACY_BOUNDED_RECORD_TABLE_HPP
#include "TracyMutableRows.hpp"
#include "../analysis/TracyPagedRecordStore.hpp"
#include <vector>
namespace tracy::storage {
class BoundedRecordTable {
    static constexpr uint64_t SmallBytes=256*1024;
    std::filesystem::path m_path;std::string m_identity;uint32_t m_width;bool m_mutable;
    ConversionResources& m_resources;analysis::AnalysisWorkspaceReservation m_fixed,m_smallLease;
    std::vector<unsigned char> m_small;std::unique_ptr<MutableRows> m_rows;
    std::unique_ptr<analysis::PagedRecordWriter> m_writer;std::unique_ptr<analysis::PagedRecordReader> m_reader;
    uint64_t m_count=0;bool m_frozen=false,m_sealed=false,m_failed=false;
    static void Require(bool value,const char* error){if(!value)throw std::runtime_error(error);}
    void Check(){m_resources.CheckFast();Require(!m_failed,"bounded_table_failed");}
    analysis::PagedRecordOptions Options(){
        analysis::PagedRecordOptions options;options.blockBytes=1024*1024;options.workspace=m_resources.Workspace();options.activeReadCacheBytes=2ull<<20;
        options.cancelled=[this]{m_resources.CheckFast();return false;};options.beforeMemoryAllocation=[this](uint64_t n){m_resources.Check(n);};
        options.beforeFileGrowth=[this](const auto& path,uint64_t n){m_resources.BeforeFileGrowth(path,n);};options.renameFile=[this](const auto& a,const auto& b){m_resources.RenameOwned(a,b);};return options;
    }
    template<class F> decltype(auto) Guard(F&& action){try{return action();}catch(const std::exception& e){m_failed=true;m_resources.Fail(e.what());throw;}catch(...){m_failed=true;m_resources.Fail("bounded_table_failure");throw;}}
public:
    BoundedRecordTable(std::filesystem::path path,std::string identity,uint32_t width,bool mutableRows,ConversionResources& resources)
        :m_path(std::move(path)),m_identity(std::move(identity)),m_width(width),m_mutable(mutableRows),m_resources(resources),m_fixed(resources.Workspace(),16384),m_smallLease(resources.Workspace()){
        Require(width>0&&width<=4096&&m_identity.size()==64,"bounded_table_configuration");Require(!std::filesystem::exists(analysis::AnalysisIoPath(m_path)),"bounded_table_exists");
        if(m_mutable)m_rows=std::make_unique<MutableRows>(m_path,width,resources);
    }
    uint64_t Append(const void* row){
        Check();Require(row&&!m_frozen&&!m_sealed,"bounded_table_append_state");return Guard([&]{Require(m_count<UINT64_MAX/m_width,"bounded_table_count");const auto id=m_count;
            if(m_mutable){Require(m_rows->Append(row)==id,"bounded_table_mutable_count");}
            else if(!m_writer&&m_small.size()+m_width<=SmallBytes){
                if(!m_small.capacity()){m_smallLease.Resize(SmallBytes+256);m_small.reserve(SmallBytes);}
                const auto* bytes=static_cast<const unsigned char*>(row);m_small.insert(m_small.end(),bytes,bytes+m_width);
            }else{
                if(!m_writer){m_writer=std::make_unique<analysis::PagedRecordWriter>(m_path,m_identity,m_width,Options());if(m_count)m_writer->Append(m_small.data(),m_count);std::vector<unsigned char>().swap(m_small);m_smallLease.Resize(0);}
                m_writer->Append(row,1);
            }
            ++m_count;return id;
        });
    }
    void Read(uint64_t id,void* row){Check();Require(row&&id<m_count,"bounded_table_index");Guard([&]{if(m_rows)m_rows->Read(id,row);else if(m_reader)m_reader->Read(id,row,1);else if(m_writer)m_writer->Read(id,row,1);else std::memcpy(row,m_small.data()+id*m_width,m_width);});}
    void Put(uint64_t id,const void* row){Check();Require(m_mutable&&!m_sealed&&row&&id<m_count,"bounded_table_update_state");Guard([&]{m_rows->Put(id,row);});}
    void FreezeAppend(){Check();if(m_frozen)return;Guard([&]{if(m_rows)m_rows->FreezeAppend();if(m_writer){m_writer->Commit();m_writer.reset();m_reader=std::make_unique<analysis::PagedRecordReader>(m_path,m_identity,m_width,Options());Require(m_reader->Count()==m_count,"bounded_table_count");}m_frozen=true;});}
    void Seal(){Check();Require(!m_sealed,"bounded_table_already_sealed");FreezeAppend();if(m_rows)m_rows->Seal();m_sealed=true;}
    template<class F> void Visit(F&& visitor){
        Check();Require(m_sealed,"bounded_table_not_sealed");Guard([&]{
            if(m_rows){m_rows->Visit(std::forward<F>(visitor));return;}
            if(!m_reader){if(m_count)visitor(m_small.data(),m_count);return;}
            const uint64_t capacity=std::max<uint64_t>(1,65536/m_width);analysis::AnalysisWorkspaceReservation lease(m_resources.Workspace(),capacity*m_width+256);std::vector<unsigned char> buffer(size_t(capacity*m_width));
            for(uint64_t first=0;first<m_count;){Check();const auto n=std::min(capacity,m_count-first);m_reader->Read(first,buffer.data(),n);visitor(buffer.data(),n);first+=n;}
        });
    }
    void ReduceCache(){Check();if(m_rows)m_rows->ReduceCache();if(m_writer)m_writer->LimitReadCache(1024*1024);}
    uint64_t Count()const{return m_count;}
};
}
#endif

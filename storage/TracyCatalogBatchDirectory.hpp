#ifndef TRACY_CATALOG_BATCH_DIRECTORY_HPP
#define TRACY_CATALOG_BATCH_DIRECTORY_HPP
#include "TracyDiskIndex64.hpp"
#include "TracyMutableRows.hpp"
#include "../public/common/TracyQueue.hpp"
#include <array>
namespace tracy::storage {
class CatalogBatchDirectory {
    static constexpr uint8_t KindCount=10;
    struct Entry {uint64_t head=0,tail=0,count=0;};
    struct Reference {uint64_t batch=0,next=0;};
    static_assert(sizeof(Entry)==24&&sizeof(Reference)==16);
public:
    struct Stats {uint64_t generations=0,batches=0,nonEmptyChains=0,visited=0,logicalBytes=0;};
private:
    std::filesystem::path m_root,m_generationPath,m_directoryPath,m_referencePath;
    ConversionResources& m_resources;
    std::unique_ptr<DiskIndex64> m_generations;
    std::unique_ptr<MutableRows> m_directory,m_references;
    Stats m_stats{};bool m_sealed=false,m_closed=false;
    static void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
public:
    CatalogBatchDirectory(std::filesystem::path root,std::string identity,ConversionResources& resources)
        :m_root(std::move(root)),m_generationPath(m_root/"generation.index"),m_directoryPath(m_root/"directory.rows"),m_referencePath(m_root/"reference.rows"),m_resources(resources){
        static_assert(uint8_t(JnGpuCatalogBatchKind::String)+1==KindCount);
        Require(!std::filesystem::exists(analysis::AnalysisIoPath(m_root)),"catalog_batch_directory_exists");
        resources.BeforeFileGrowth(m_root,0);std::filesystem::create_directories(analysis::AnalysisIoPath(m_root));
        m_generations=std::make_unique<DiskIndex64>(m_generationPath,std::move(identity),resources,2ull<<20);
        m_directory=std::make_unique<MutableRows>(m_directoryPath,sizeof(Entry),resources,2ull<<20);
        m_references=std::make_unique<MutableRows>(m_referencePath,sizeof(Reference),resources,2ull<<20);
    }
    ~CatalogBatchDirectory(){CloseAndRemove();}
    void AddGeneration(uint64_t generation){
        m_resources.CheckFast();Require(!m_closed&&!m_sealed,"catalog_batch_directory_state");Require(!m_generations->Find(generation),"catalog_batch_directory_duplicate_generation");
        Require(m_stats.generations<UINT64_MAX/KindCount,"catalog_batch_directory_generation_range");const auto ordinal=m_stats.generations++;
        m_generations->Upsert(generation,ordinal+1);Entry empty;for(uint8_t kind=0;kind<KindCount;++kind)Require(m_directory->Append(&empty)==ordinal*KindCount+kind,"catalog_batch_directory_entry_order");
    }
    void AddBatch(uint64_t batch,uint64_t generation,uint8_t kind){
        m_resources.CheckFast();Require(!m_closed&&!m_sealed,"catalog_batch_directory_state");Require(kind<KindCount,"catalog_batch_directory_kind");const auto encoded=m_generations->Find(generation);Require(encoded&&*encoded>0&&*encoded<=m_stats.generations,"catalog_batch_directory_generation");
        const auto entryId=(*encoded-1)*KindCount+kind;Entry entry;m_directory->Read(entryId,&entry);Reference reference{batch,0};const auto referenceId=m_references->Append(&reference)+1;
        if(entry.tail){Reference tail;m_references->Read(entry.tail-1,&tail);Require(!tail.next,"catalog_batch_directory_tail");tail.next=referenceId;m_references->Put(entry.tail-1,&tail);}else{Require(!entry.head&&!entry.count,"catalog_batch_directory_empty");entry.head=referenceId;++m_stats.nonEmptyChains;}
        entry.tail=referenceId;++entry.count;++m_stats.batches;m_directory->Put(entryId,&entry);
    }
    void Seal(){
        m_resources.CheckFast();Require(!m_closed&&!m_sealed,"catalog_batch_directory_state");m_generations->Seal();m_directory->Seal();m_references->Seal();m_stats.logicalBytes=m_generations->LogicalBytes()+m_directory->Count()*sizeof(Entry)+m_references->Count()*sizeof(Reference);m_sealed=true;
    }
    template<class Visitor> uint64_t Visit(uint64_t generation,uint8_t kind,Visitor&& visitor){
        m_resources.CheckFast();Require(!m_closed&&m_sealed,"catalog_batch_directory_visit_state");Require(kind<KindCount,"catalog_batch_directory_kind");const auto encoded=m_generations->Find(generation);if(!encoded)return 0;Require(*encoded>0&&*encoded<=m_stats.generations,"catalog_batch_directory_generation");
        Entry entry;m_directory->Read((*encoded-1)*KindCount+kind,&entry);Require((entry.count==0)==(entry.head==0&&entry.tail==0),"catalog_batch_directory_entry");uint64_t next=entry.head,visited=0;
        while(next){Require(next<=m_references->Count()&&visited<entry.count,"catalog_batch_directory_chain");Reference reference;m_references->Read(next-1,&reference);visitor(reference.batch);next=reference.next;++visited;}
        Require(visited==entry.count,"catalog_batch_directory_count");m_stats.visited+=visited;return visited;
    }
    Stats Statistics()const{return m_stats;}
    bool CloseAndRemove() noexcept {
        if(m_closed)return true;const bool directoryOwned=m_directory&&m_directory->HasDiskFile(),referenceOwned=m_references&&m_references->HasDiskFile();m_generations.reset();m_directory.reset();m_references.reset();bool ok=true;
        if(!m_resources.RemoveOwned(m_generationPath))ok=false;if(directoryOwned&&!m_resources.RemoveOwned(m_directoryPath))ok=false;if(referenceOwned&&!m_resources.RemoveOwned(m_referencePath))ok=false;if(!m_resources.RemoveOwned(m_root))ok=false;m_closed=true;if(!ok)m_resources.Fail("catalog_batch_directory_cleanup_failed");return ok;
    }
};
}
#endif

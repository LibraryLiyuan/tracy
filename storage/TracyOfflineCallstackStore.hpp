#ifndef TRACY_OFFLINE_CALLSTACK_STORE_HPP
#define TRACY_OFFLINE_CALLSTACK_STORE_HPP

#include "TracyMappedAppendArena.hpp"
#include "TracyMappedHashIndex64.hpp"
#include "TracyMutableRows.hpp"
#include "../analysis/TracyHash.hpp"
#include <functional>

namespace tracy::storage
{

class OfflineCallstackStore
{
#pragma pack(push,1)
    struct Entry
    {
        uint64_t hash = 0;
        uint64_t next = 0;
        uint64_t offset = 0;
        uint32_t count = 0;
        uint32_t chunk = 0;
    };
#pragma pack(pop)
    static_assert( sizeof( Entry ) == 32 );
public:
    struct Value { const uint64_t* frames; uint32_t count; uint32_t id; };
    using HashFunction = std::function<uint64_t( const void*, size_t )>;
    OfflineCallstackStore( std::filesystem::path root, std::string identity, ConversionResources& resources, HashFunction hash = {} )
        : m_root( std::move( root ) ), m_resources( resources ), m_hashFunction( hash ? std::move( hash ) : HashFunction( DefaultHash ) ),
          m_smallLease( resources.Workspace(), SmallBytes + SmallEntries * sizeof( void* ) + 256 )
    {
        if( identity.size() != 64 ) throw std::runtime_error( "offline_callstack_identity" );
        if( std::filesystem::exists( analysis::AnalysisIoPath( m_root ) ) ) throw std::runtime_error( "offline_callstack_exists" );
        m_resources.BeforeFileGrowth( m_root, 0 );std::filesystem::create_directories( analysis::AnalysisIoPath( m_root ) );
        m_small.reserve( SmallEntries );
        m_entries = std::make_unique<MutableRows>( m_root / "entries.bin", sizeof( Entry ), m_resources );
        m_index = std::make_unique<MappedHashIndex64>( m_root / "hash.index", m_resources, 64 );
    }
    Value Intern( const uint64_t* frames, uint32_t count )
    {
        Check();Require( !m_sealed && frames && count, "offline_callstack_append_state" );
        try
        {
            const auto bytes = uint64_t( count ) * sizeof( uint64_t );const auto hash=m_hashFunction(frames,size_t(bytes));uint64_t link=m_index->Find(hash).value_or(0),traversed=0;
            while(link){const auto id=link-1;Require(id<m_entries->Count()&&++traversed<=m_entries->Count(),"offline_callstack_chain");Entry entry;m_entries->Read(id,&entry);Require(entry.hash==hash,"offline_callstack_hash_chain");const auto* value=Pointer(entry);if(entry.count==count&&std::memcmp(value,frames,size_t(bytes))==0)return{value,count,uint32_t(id+1)};link=entry.next;}
            Require(m_entries->Count()<0xFFFFFEull,"callstack_table_24bit_overflow");const auto location=AppendFrames(frames,count);const Entry entry{hash,m_index->Find(hash).value_or(0),location.offset,count,location.chunk};const auto ordinal=m_entries->Append(&entry);const auto id=ordinal+1;Require(id<=0xFFFFFEull,"callstack_table_24bit_overflow");m_index->Upsert(hash,id);
            m_contentHash.Update(&count,sizeof(count));m_contentHash.Update(frames,size_t(bytes));return{location.data,count,uint32_t(id)};
        }catch(const std::exception& e){m_failed=true;m_resources.Fail(e.what());throw;}
    }
    Value Get( uint32_t id )
    {
        Check();Require(id>0&&id<=m_entries->Count(),"offline_callstack_index");Entry entry;m_entries->Read(id-1,&entry);return{Pointer(entry),entry.count,id};
    }
    void Seal(){Check();Require(!m_sealed,"offline_callstack_sealed");m_entries->Seal();m_index->Seal();m_index->Validate();if(m_arena)m_arena->Flush();m_hash=m_contentHash.FinalHex();m_sealed=true;}
    template<class Visitor>void Visit(Visitor&& visitor){Check();Require(m_sealed,"offline_callstack_not_sealed");uint64_t id=1;m_entries->Visit([&](const void* data,uint64_t count){const auto* rows=static_cast<const Entry*>(data);for(uint64_t i=0;i<count;++i,++id)visitor(uint32_t(id),Pointer(rows[i]),rows[i].count);});Require(id==m_entries->Count()+1,"offline_callstack_visit_count");}
    uint64_t Count()const{return m_entries?m_entries->Count()+1:1;}
    uint64_t UsedBytes()const{return m_smallBytes+(m_arena?m_arena->UsedBytes():0);}
    uint64_t ReservedBytes()const{return m_arena?m_arena->ReservedBytes():0;}
    void ReduceCache(){Check();if(m_entries)m_entries->ReduceCache();}
    const std::string& Hash()const{if(!m_sealed)throw std::runtime_error("offline_callstack_not_sealed");return m_hash;}
private:
    static constexpr uint64_t SmallBytes=256ull<<10,SmallEntries=4096;
    struct Location{uint32_t chunk;uint64_t offset;const uint64_t* data;};
    Location AppendFrames(const uint64_t* frames,uint32_t count){const auto bytes=uint64_t(count)*sizeof(uint64_t);if(m_small.size()<SmallEntries&&bytes<=SmallBytes-m_smallBytes){auto value=std::make_unique<uint64_t[]>(count);std::memcpy(value.get(),frames,size_t(bytes));auto* result=value.get();const auto index=uint64_t(m_small.size());m_small.emplace_back(std::move(value));m_smallBytes+=bytes;return{UINT32_MAX,index,result};}if(!m_arena)m_arena=std::make_unique<MappedAppendArena>(m_root/"bytes",m_resources);const auto location=m_arena->Allocate(bytes,alignof(uint64_t));std::memcpy(location.data,frames,size_t(bytes));return{location.chunk,location.offset,reinterpret_cast<const uint64_t*>(location.data)};}
    const uint64_t* Pointer(const Entry& entry)const{if(entry.chunk==UINT32_MAX){Require(entry.offset<m_small.size(),"offline_callstack_small_index");return m_small[size_t(entry.offset)].get();}Require(m_arena!=nullptr,"offline_callstack_arena_missing");return reinterpret_cast<const uint64_t*>(m_arena->GetRaw(entry.chunk,entry.offset,uint64_t(entry.count)*sizeof(uint64_t)));}
    static uint64_t DefaultHash(const void* data,size_t bytes){const auto* value=static_cast<const unsigned char*>(data);uint64_t hash=1469598103934665603ull;for(size_t i=0;i<bytes;++i){hash^=value[i];hash*=1099511628211ull;}hash^=uint64_t(bytes);hash*=1099511628211ull;return hash;}
    static void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}void Check()const{m_resources.CheckFast();Require(!m_failed,"offline_callstack_failed");}
    std::filesystem::path m_root;ConversionResources& m_resources;HashFunction m_hashFunction;analysis::AnalysisWorkspaceReservation m_smallLease;std::vector<std::unique_ptr<uint64_t[]>>m_small;uint64_t m_smallBytes=0;std::unique_ptr<MappedAppendArena>m_arena;std::unique_ptr<MutableRows>m_entries;std::unique_ptr<MappedHashIndex64>m_index;analysis::Sha256Builder m_contentHash;std::string m_hash;bool m_sealed=false,m_failed=false;
};

}
#endif

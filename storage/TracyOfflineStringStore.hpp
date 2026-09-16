#ifndef TRACY_OFFLINE_STRING_STORE_HPP
#define TRACY_OFFLINE_STRING_STORE_HPP

#include "TracyMappedAppendArena.hpp"
#include "TracyMappedHashIndex64.hpp"
#include "TracyMutableRows.hpp"
#include "../analysis/TracyHash.hpp"
#include <functional>

namespace tracy::storage
{

class OfflineStringStore
{
#pragma pack(push,1)
    struct Entry
    {
        uint64_t hash = 0;
        uint64_t next = 0;
        uint64_t offset = 0;
        uint64_t length = 0;
        uint32_t chunk = 0;
        uint32_t reserved = 0;
    };
#pragma pack(pop)
    static_assert( sizeof( Entry ) == 40 );
public:
    struct Value { const char* data; uint32_t id; };
    using HashFunction = std::function<uint64_t( const void*, size_t )>;
    OfflineStringStore( std::filesystem::path root, std::string identity, ConversionResources& resources, HashFunction hash = {} )
        : m_root( std::move( root ) )
        , m_resources( resources )
        , m_hashFunction( hash ? std::move( hash ) : HashFunction( DefaultHash ) )
        , m_smallLease( resources.Workspace(), SmallBytes + SmallEntries * sizeof( void* ) + 256 )
    {
        if( identity.size() != 64 ) throw std::runtime_error( "offline_string_identity" );
        if( std::filesystem::exists( analysis::AnalysisIoPath( m_root ) ) ) throw std::runtime_error( "offline_string_exists" );
        m_resources.BeforeFileGrowth( m_root, 0 );
        std::filesystem::create_directories( analysis::AnalysisIoPath( m_root ) );
        m_small.reserve( SmallEntries );
        m_entries = std::make_unique<MutableRows>( m_root / "entries.bin", sizeof( Entry ), m_resources );
        m_index = std::make_unique<MappedHashIndex64>( m_root / "hash.index", m_resources, 64 );
    }
    Value Intern( const void* data, size_t bytes )
    {
        Check();
        Require( !m_sealed && ( bytes == 0 || data ), "offline_string_append_state" );
        try
        {
            const auto hash = m_hashFunction( data, bytes );
            uint64_t link = m_index->Find( hash ).value_or( 0 );
            uint64_t traversed = 0;
            while( link )
            {
                const auto id = link - 1;
                Require( id < m_entries->Count() && ++traversed <= m_entries->Count(), "offline_string_chain" );
                Entry entry;
                m_entries->Read( id, &entry );
                Require( entry.hash == hash, "offline_string_hash_chain" );
                const auto* value = Pointer( entry );
                if( entry.length == bytes && ( bytes == 0 || std::memcmp( value, data, bytes ) == 0 ) ) return { value, uint32_t( id ) };
                link = entry.next;
            }
            const auto count = m_entries->Count();
            Require( count <= 0xFFFFFFFEull, "string_table_32bit_overflow" );
            const auto location = AppendBytes( data, bytes );
            const Entry entry { hash, m_index->Find( hash ).value_or( 0 ), location.offset, bytes, location.chunk, 0 };
            const auto id = m_entries->Append( &entry );
            Require( id == count && id <= 0xFFFFFFFEull, "offline_string_id" );
            m_index->Upsert( hash, id + 1 );
            const uint64_t length = bytes;
            m_contentHash.Update( &length, sizeof( length ) );
            if( bytes ) m_contentHash.Update( data, bytes );
            return { location.data, uint32_t( id ) };
        }
        catch( const std::exception& e )
        {
            m_failed = true;
            m_resources.Fail( e.what() );
            throw;
        }
    }
    std::optional<Value> Find( const void* data, size_t bytes )
    {
        Check();
        Require( bytes == 0 || data, "offline_string_find" );
        const auto hash = m_hashFunction( data, bytes );
        uint64_t link = m_index->Find( hash ).value_or( 0 ), traversed = 0;
        while( link )
        {
            const auto id = link - 1;
            Require( id < m_entries->Count() && ++traversed <= m_entries->Count(), "offline_string_chain" );
            Entry entry; m_entries->Read( id, &entry );
            const auto* value = Pointer( entry );
            if( entry.length == bytes && ( bytes == 0 || std::memcmp( value, data, bytes ) == 0 ) ) return Value { value, uint32_t( id ) };
            link = entry.next;
        }
        return {};
    }
    const char* Get( uint32_t id )
    {
        Check();
        Require( id < m_entries->Count(), "offline_string_index" );
        Entry entry; m_entries->Read( id, &entry );
        return Pointer( entry );
    }
    void Seal()
    {
        Check();
        Require( !m_sealed, "offline_string_sealed" );
        m_entries->Seal();
        m_index->Seal();
        m_index->Validate();
        if( m_arena ) m_arena->Flush();
        m_hash = m_contentHash.FinalHex();
        m_sealed = true;
    }
    template<class Visitor> void Visit( Visitor&& visitor )
    {
        Check();
        Require( m_sealed, "offline_string_not_sealed" );
        uint64_t id = 0;
        m_entries->Visit( [&]( const void* data, uint64_t count ) {
            const auto* rows = static_cast<const Entry*>( data );
            for( uint64_t i = 0; i < count; ++i, ++id ) visitor( uint32_t( id ), Pointer( rows[i] ), rows[i].length );
        } );
        Require( id == m_entries->Count(), "offline_string_visit_count" );
    }
    uint64_t Count() const { return m_entries ? m_entries->Count() : 0; }
    uint64_t UsedBytes() const { return m_smallBytes + (m_arena ? m_arena->UsedBytes() : 0); }
    uint64_t ReservedBytes() const { return m_arena ? m_arena->ReservedBytes() : 0; }
    void ReduceCache() { Check(); if(m_entries)m_entries->ReduceCache(); }
    const std::string& Hash() const { if( !m_sealed ) throw std::runtime_error( "offline_string_not_sealed" ); return m_hash; }
private:
    static constexpr uint64_t SmallBytes = 256ull << 10;
    static constexpr uint64_t SmallEntries = 4096;
    struct StoredLocation { uint32_t chunk; uint64_t offset; char* data; };
    StoredLocation AppendBytes( const void* data, size_t bytes )
    {
        Require( bytes < SIZE_MAX, "offline_string_size" );
        if( m_small.size() < SmallEntries && bytes + 1 <= SmallBytes - m_smallBytes )
        {
            auto value = std::make_unique<char[]>( bytes + 1 );
            if( bytes ) std::memcpy( value.get(), data, bytes );
            value[bytes] = 0;
            auto* result = value.get();
            const auto index = uint64_t( m_small.size() );
            m_small.emplace_back( std::move( value ) );
            m_smallBytes += bytes + 1;
            return { UINT32_MAX, index, result };
        }
        if( !m_arena ) m_arena = std::make_unique<MappedAppendArena>( m_root / "bytes", m_resources );
        const auto location = m_arena->Append( data, bytes );
        return { location.chunk, location.offset, location.data };
    }
    const char* Pointer( const Entry& entry ) const
    {
        if( entry.chunk == UINT32_MAX )
        {
            Require( entry.offset < m_small.size(), "offline_string_small_index" );
            const auto* value = m_small[size_t(entry.offset)].get();
            Require( value[entry.length] == 0, "offline_string_small_length" );
            return value;
        }
        Require( m_arena != nullptr, "offline_string_arena_missing" );
        return m_arena->Get( entry.chunk, entry.offset, entry.length );
    }
    static uint64_t DefaultHash( const void* data, size_t bytes )
    {
        const auto* value = static_cast<const unsigned char*>( data );
        uint64_t hash = 1469598103934665603ull;
        for( size_t i = 0; i < bytes; ++i ) { hash ^= value[i]; hash *= 1099511628211ull; }
        hash ^= uint64_t( bytes ); hash *= 1099511628211ull;
        return hash;
    }
    static void Require( bool value, const char* message ) { if( !value ) throw std::runtime_error( message ); }
    void Check() const { m_resources.CheckFast(); Require( !m_failed, "offline_string_failed" ); }
    std::filesystem::path m_root;
    ConversionResources& m_resources;
    HashFunction m_hashFunction;
    analysis::AnalysisWorkspaceReservation m_smallLease;
    std::vector<std::unique_ptr<char[]>> m_small;
    uint64_t m_smallBytes = 0;
    std::unique_ptr<MappedAppendArena> m_arena;
    std::unique_ptr<MutableRows> m_entries;
    std::unique_ptr<MappedHashIndex64> m_index;
    analysis::Sha256Builder m_contentHash;
    std::string m_hash;
    bool m_sealed = false;
    bool m_failed = false;
};

}
#endif

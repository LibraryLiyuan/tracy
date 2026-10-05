#pragma once
#include "TracyEventStore.hpp"
#include <list>
#include <stdexcept>
#include <string_view>
#include <type_traits>
namespace tracy::stream
{
template<class T>
struct CacheVector : std::false_type
{
};
template<class T, class A>
struct CacheVector<std::vector<T, A>> : std::true_type
{
};
template<class T>
struct CacheOptional : std::false_type
{
};
template<class T>
struct CacheOptional<std::optional<T>> : std::true_type
{
};
template<class T>
struct CacheVariant : std::false_type
{
};
template<class... T>
struct CacheVariant<std::variant<T...>> : std::true_type
{
};
template<class T>
uint64_t CacheDynamicBytes( const T& value )
{
    if constexpr( std::is_same_v<T, std::string> ) return value.capacity() + 1 + 32;
    else if constexpr( CacheVector<T>::value )
    {
        uint64_t bytes = value.capacity() * sizeof( typename T::value_type ) + 32;
        if constexpr( !std::is_trivially_copyable_v<typename T::value_type> )
            for( const auto& v : value ) bytes += CacheDynamicBytes( v );
        return bytes;
    }
    else if constexpr( CacheOptional<T>::value ) return value ? CacheDynamicBytes( *value ) : 0;
    else if constexpr( CacheVariant<T>::value ) return std::visit( []( const auto& v ) {
        return CacheDynamicBytes( v );
    },
                                                                   value );
    else
    {
        uint64_t bytes = 0;
#define TRACY_CACHE_FIELD( field ) \
    if constexpr( requires { value.field; } ) bytes += CacheDynamicBytes( value.field )
        TRACY_CACHE_FIELD( name );
        TRACY_CACHE_FIELD( function );
        TRACY_CACHE_FIELD( file );
        TRACY_CACHE_FIELD( text );
        TRACY_CACHE_FIELD( image );
        TRACY_CACHE_FIELD( callFile );
        TRACY_CACHE_FIELD( path );
        TRACY_CACHE_FIELD( program );
        TRACY_CACHE_FIELD( host );
        TRACY_CACHE_FIELD( session );
        TRACY_CACHE_FIELD( sourceSha256 );
        TRACY_CACHE_FIELD( dataSha256 );
        TRACY_CACHE_FIELD( data );
        TRACY_CACHE_FIELD( descriptor );
        TRACY_CACHE_FIELD( waiters );
        TRACY_CACHE_FIELD( sharedWaiters );
        TRACY_CACHE_FIELD( sharedOwners );
        TRACY_CACHE_FIELD( values );
        TRACY_CACHE_FIELD( boxes );
        TRACY_CACHE_FIELD( lanes );
        TRACY_CACHE_FIELD( cells );
        TRACY_CACHE_FIELD( row );
#undef TRACY_CACHE_FIELD
        return bytes;
    }
}
using StreamCacheValue = std::variant<StorePage<StoreZone>, StorePage<StoreGpuZone>, std::vector<StoreOverview>,StoreTimelineView>;
struct StreamCacheStats
{
    uint64_t bytes = 0, hits = 0, misses = 0, evictions = 0, rejections = 0;
    size_t entries = 0;
};
class StreamCache
{
public:
    explicit StreamCache( uint64_t budget, size_t entries = 256 )
        : m_budget( budget )
        , m_limit( entries )
    {
        if( !budget || !entries ) throw std::invalid_argument( "Invalid stream cache budget" );
    }
    static uint64_t Cost( const std::string& key, const StreamCacheValue& value )
    {
        return sizeof( value ) + CacheDynamicBytes( value ) + key.capacity() + 1 + 128;
    }
    template<class T>
    std::shared_ptr<const T> Get( std::string_view key )
    {
        for( auto it = m_entries.begin(); it != m_entries.end(); ++it )
            if( it->active && it->key == key )
            {
                const auto* value = std::get_if<T>( it->value.get() );
                if( !value ) throw std::logic_error( "Stream cache key type mismatch" );
                auto lease = std::shared_ptr<const T>( it->value, value );
                m_entries.splice( m_entries.begin(), m_entries, it );
                ++m_stats.hits;
                return lease;
            }
        ++m_stats.misses;
        return {};
    }
    template<class T>
    bool Put( std::string key, T value )
    {
        return PutValue( std::move( key ), std::make_shared<const StreamCacheValue>( std::move( value ) ) );
    }
    bool PutValue( std::string key, std::shared_ptr<const StreamCacheValue> payload )
    {
        if( !payload ) throw std::invalid_argument( "Missing stream cache payload" );
        // One cache entry per ownership block: otherwise cache-owned aliases
        // look like external leases and can remain permanently pinned.
        for( auto it = m_entries.begin(); it != m_entries.end(); ++it )
            if( !it->value.owner_before( payload ) && !payload.owner_before( it->value ) )
            {
                if( it->key == key && it->value.get() == payload.get() )
                {
                    it->active = true;
                    m_entries.splice( m_entries.begin(), m_entries, it );
                    return true;
                }
                ++m_stats.rejections;
                return false;
            }
        const auto cost = Cost( key, *payload );
        if( cost > m_budget )
        {
            ++m_stats.rejections;
            return false;
        }
        uint64_t reclaim = 0;
        size_t removable = 0;
        for( const auto& entry : m_entries )
            if( entry.value.use_count() == 1 )
            {
                reclaim += entry.cost;
                ++removable;
            }
        if( cost > m_budget - m_stats.bytes + reclaim || m_entries.size() - removable >= m_limit )
        {
            ++m_stats.rejections;
            return false;
        }
        // Admission is checked before changing the preceding successful entry.
        for( auto& entry : m_entries )
            if( entry.active && entry.key == key ) entry.active = false;
        for( auto it = m_entries.begin(); it != m_entries.end(); )
            if( !it->active && it->value.use_count() == 1 )
            {
                it = Erase( it );
            }
            else ++it;
        while( m_stats.bytes + cost > m_budget || m_entries.size() >= m_limit )
        {
            auto victim = m_entries.end();
            for( auto it = m_entries.end(); it != m_entries.begin(); )
            {
                --it;
                if( it->value.use_count() == 1 )
                {
                    victim = it;
                    break;
                }
            }
            if( victim == m_entries.end() ) throw std::logic_error( "Pinned stream cache admission changed" );
            Erase( victim );
        }
        m_entries.push_front( { std::move( key ), std::move( payload ), cost, true } );
        m_stats.bytes += cost;
        return true;
    }
    void Clear()
    {
        for( auto it = m_entries.begin(); it != m_entries.end(); )
            if( it->value.use_count() == 1 )
            {
                it = Erase( it );
            }
            else
            {
                it->active = false;
                ++it;
            }
    }
    StreamCacheStats Stats() const
    {
        auto result = m_stats;
        result.entries = m_entries.size();
        return result;
    }

private:
    struct Entry
    {
        std::string key;
        std::shared_ptr<const StreamCacheValue> value;
        uint64_t cost;
        bool active;
    };
    std::list<Entry> m_entries;
    std::list<Entry>::iterator Erase( std::list<Entry>::iterator it )
    {
        m_stats.bytes -= it->cost;
        ++m_stats.evictions;
        return m_entries.erase( it );
    }
    uint64_t m_budget;
    size_t m_limit;
    StreamCacheStats m_stats;
};
}

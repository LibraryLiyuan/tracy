#ifndef __TRACYTHREADLAYOUT_HPP__
#define __TRACYTHREADLAYOUT_HPP__

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tracy
{

struct ThreadLayoutEntry
{
    uint64_t id;
    bool visible;
};

struct ThreadLayoutState
{
    bool customOrder = false;
    std::vector<ThreadLayoutEntry> threads;
};

inline bool ThreadNameStartsWith( std::string_view name, std::string_view prefix )
{
    if( name.size() < prefix.size() ) return false;
    for( size_t i = 0; i < prefix.size(); ++i )
    {
        const auto ch = name[i] >= 'A' && name[i] <= 'Z' ? name[i] - 'A' + 'a' : name[i];
        if( ch != prefix[i] ) return false;
    }
    return true;
}

inline int DefaultThreadPriority( std::string_view name )
{
    if( ThreadNameStartsWith( name, "main thread" ) || ThreadNameStartsWith( name, "unitymain" ) ) return 0;
    if( ThreadNameStartsWith( name, "render thread" ) || ThreadNameStartsWith( name, "renderthread" ) ||
        ThreadNameStartsWith( name, "gfxdeviceworker" ) ) return 1;
    if( ThreadNameStartsWith( name, "job.worker" ) || ThreadNameStartsWith( name, "job worker" ) ||
        ThreadNameStartsWith( name, "jobworker" ) ) return 2;
    return 3;
}

template<class Iterator, class GetName, class GetId, class GetHint>
void SortThreadLayout( Iterator first, Iterator last, const ThreadLayoutState& state,
    GetName getName, GetId getId, GetHint getHint )
{
    std::unordered_map<uint64_t, size_t> savedOrder;
    if( state.customOrder )
    {
        savedOrder.reserve( state.threads.size() );
        for( size_t i = 0; i < state.threads.size(); ++i ) savedOrder.emplace( state.threads[i].id, i );
    }
    const auto unsaved = (std::numeric_limits<size_t>::max)();
    const auto less = [&]( const auto& lhs, const auto& rhs ) {
        if( state.customOrder )
        {
            const auto lhsIt = savedOrder.find( getId( lhs ) );
            const auto rhsIt = savedOrder.find( getId( rhs ) );
            const auto lhsOrder = lhsIt == savedOrder.end() ? unsaved : lhsIt->second;
            const auto rhsOrder = rhsIt == savedOrder.end() ? unsaved : rhsIt->second;
            if( lhsOrder != rhsOrder ) return lhsOrder < rhsOrder;
        }
        const auto lhsPriority = DefaultThreadPriority( getName( lhs ) );
        const auto rhsPriority = DefaultThreadPriority( getName( rhs ) );
        if( lhsPriority != rhsPriority ) return lhsPriority < rhsPriority;
        return getHint( lhs ) < getHint( rhs );
    };
    if( !std::is_sorted( first, last, less ) ) std::stable_sort( first, last, less );
}

inline bool WriteThreadLayout( FILE* file, const ThreadLayoutState& state )
{
    if( !file || state.threads.size() > 100000 ) return false;
    const uint32_t version = 1;
    const uint8_t customOrder = state.customOrder ? 1 : 0;
    const uint32_t count = uint32_t( state.threads.size() );
    if( fwrite( &version, sizeof( version ), 1, file ) != 1 ||
        fwrite( &customOrder, sizeof( customOrder ), 1, file ) != 1 ||
        fwrite( &count, sizeof( count ), 1, file ) != 1 ) return false;
    for( const auto& thread : state.threads )
    {
        const uint8_t visible = thread.visible ? 1 : 0;
        if( fwrite( &thread.id, sizeof( thread.id ), 1, file ) != 1 ||
            fwrite( &visible, sizeof( visible ), 1, file ) != 1 ) return false;
    }
    return true;
}

inline bool ReadThreadLayout( FILE* file, ThreadLayoutState& state )
{
    if( !file ) return false;
    uint32_t version;
    uint8_t customOrder;
    uint32_t count;
    if( fread( &version, sizeof( version ), 1, file ) != 1 || version != 1 ||
        fread( &customOrder, sizeof( customOrder ), 1, file ) != 1 || customOrder > 1 ||
        fread( &count, sizeof( count ), 1, file ) != 1 || count > 100000 ) return false;
    ThreadLayoutState loaded;
    loaded.customOrder = customOrder != 0;
    loaded.threads.reserve( count );
    for( uint32_t i = 0; i < count; ++i )
    {
        uint64_t id;
        uint8_t visible;
        if( fread( &id, sizeof( id ), 1, file ) != 1 ||
            fread( &visible, sizeof( visible ), 1, file ) != 1 || visible > 1 ) return false;
        loaded.threads.push_back( { id, visible != 0 } );
    }
    state = std::move( loaded );
    return true;
}

}

#endif

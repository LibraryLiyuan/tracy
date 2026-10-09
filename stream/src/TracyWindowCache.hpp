#pragma once
#include "TracyStreamCache.hpp"
#include <algorithm>
namespace tracy::stream
{
inline std::string WindowCacheKey( const StoreInfo& info, char domain, int64_t begin = 0, int64_t end = 0, uint64_t after = 0, std::optional<uint64_t> context = {} )
{
    std::string key = info.session;
    key.push_back( domain );
    for( const auto value : { info.revision, uint64_t( begin ), uint64_t( end ), after, uint64_t( context.has_value() ), context.value_or( 0 ) } ) key.append( reinterpret_cast<const char*>( &value ), sizeof( value ) );
    return key;
}
template<class T, class F>
std::shared_ptr<const T> CachedWindowPage( StreamCache& cache, const std::string& key, F fetch )
{
    if( auto existing = cache.Get<T>( key ) ) return existing;
    auto value = std::make_shared<const StreamCacheValue>( fetch() );
    cache.PutValue( key, value );
    return std::shared_ptr<const T>( value, &std::get<T>( *value ) );
}
inline std::shared_ptr<const StorePage<StoreZone>> CachedCpuWindow( StreamCache& cache, EventStore& store, int64_t begin, int64_t end, uint64_t after = 0 )
{
    return CachedWindowPage<StorePage<StoreZone>>( cache, WindowCacheKey( store.Info(), 'C', begin, end, after ), [&] {
        return store.Zones( begin, end, {}, after, 512 );
    } );
}
inline std::shared_ptr<const StorePage<StoreGpuZone>> CachedGpuWindow( StreamCache& cache, EventStore& store, int64_t begin, int64_t end, std::optional<uint64_t> context = {}, uint64_t after = 0 )
{
    return CachedWindowPage<StorePage<StoreGpuZone>>( cache, WindowCacheKey( store.Info(), 'G', begin, end, after, context ), [&] {
        return store.GpuZones( begin, end, context, after, 256 );
    } );
}
inline std::shared_ptr<const std::vector<StoreOverview>> CachedFrameOverview( StreamCache& cache, EventStore& store )
{
    return CachedWindowPage<std::vector<StoreOverview>>( cache, WindowCacheKey( store.Info(), 'O' ), [&] {
        return store.Overview( 0, std::max<int64_t>( 1, store.Info().lastNs ), 512 );
    } );
}
inline std::shared_ptr<const StoreTimelineView> CachedTimelineOverview(StreamCache& cache,EventStore& store,const StoreTimelineOptions& options)
{
    auto key=WindowCacheKey(store.Info(),options.gpu?'U':'T',options.begin,options.end,options.afterLane,options.context);
    for(const uint64_t value:{uint64_t(options.bins),uint64_t(options.depths),uint64_t(options.maximumLanes)})key.append(reinterpret_cast<const char*>(&value),sizeof(value));
    return CachedWindowPage<StoreTimelineView>(cache,key,[&]{return store.TimelineOverview(options);});
}
inline std::shared_ptr<const StorePage<StoreZone>> EmptyCpuWindow()
{
    static const auto value = std::make_shared<const StorePage<StoreZone>>();
    return value;
}
inline std::shared_ptr<const StorePage<StoreGpuZone>> EmptyGpuWindow()
{
    static const auto value = std::make_shared<const StorePage<StoreGpuZone>>();
    return value;
}
inline std::shared_ptr<const std::vector<StoreOverview>> EmptyFrameOverview()
{
    static const auto value = std::make_shared<const std::vector<StoreOverview>>();
    return value;
}
}

#include "../src/TracyStreamCache.hpp"
#include "../src/TracyWindowCache.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <tuple>
using namespace tracy::stream;
void Check( bool value, const char* message )
{
    if( !value ) throw std::runtime_error( message );
}
StorePage<StoreZone> Page( uint64_t id )
{
    StorePage<StoreZone> page;
    StoreZone zone;
    zone.id = id;
    zone.start = id;
    zone.end = id + 1;
    zone.name = std::string( 4096, 'x' );
    zone.function = "Function";
    zone.file = "cache.cpp";
    page.values.push_back( std::move( zone ) );
    page.next = id;
    return page;
}
bool Same( const StoreZone& a, const StoreZone& b )
{
    return std::tie( a.id, a.thread, a.parent, a.stack, a.callsite, a.start, a.end, a.depth, a.line, a.color, a.wireCallsite, a.provenance, a.unavailableReason, a.name, a.function, a.file, a.text ) == std::tie( b.id, b.thread, b.parent, b.stack, b.callsite, b.start, b.end, b.depth, b.line, b.color, b.wireCallsite, b.provenance, b.unavailableReason, b.name, b.function, b.file, b.text );
}
bool Same( const StoreGpuZone& a, const StoreGpuZone& b )
{
    return std::tie( a.id, a.context, a.thread, a.parent, a.stack, a.callsite, a.cpuStart, a.cpuEnd, a.gpuStart, a.gpuEnd, a.depth, a.line, a.color, a.query, a.provenance, a.unavailableReason, a.wireCallsite, a.name, a.function, a.file ) == std::tie( b.id, b.context, b.thread, b.parent, b.stack, b.callsite, b.cpuStart, b.cpuEnd, b.gpuStart, b.gpuEnd, b.depth, b.line, b.color, b.query, b.provenance, b.unavailableReason, b.wireCallsite, b.name, b.function, b.file );
}
template<class T>
void SamePage( const StorePage<T>& a, const StorePage<T>& b )
{
    Check( a.next == b.next && a.more == b.more && a.values.size() == b.values.size(), "page shape survives cache retirement" );
    for( size_t n = 0; n < a.values.size(); ++n ) Check( Same( a.values[n], b.values[n] ), "event fields survive cache retirement and reload" );
}
int main( int argc, char** argv )
{
    try
    {
        if( argc == 3 && std::string_view( argv[1] ) == "--capture" )
        {
            StoreOptions options;
            options.cacheBytes = 16ull << 20;
            options.resultBytes = 1ull << 20;
            auto store = EventStore::Open( std::filesystem::u8path( argv[2] ), options );
            StreamCache cache( 64ull << 20 );
            for( int minute : { 1, 10, 20, 29 } )
            {
                const int64_t begin = store->Info().lastNs >= 1800000000000ll ? int64_t( minute ) * 60000000000ll : store->Info().lastNs * minute / 30;
                const int64_t end = std::min<int64_t>( store->Info().lastNs + 1, begin + 5000000000ll );
                const auto started = std::chrono::steady_clock::now();
                auto cpu = CachedCpuWindow( cache, *store, begin, end );
                auto gpu = CachedGpuWindow( cache, *store, begin, end );
                auto overview = CachedFrameOverview( cache, *store );
                StoreTimelineOptions denseOptions;denseOptions.begin=begin;denseOptions.end=end;auto density=CachedTimelineOverview(cache,*store,denseOptions);
                Check(density->events>=cpu->values.size() || density->more,"CPU aggregate includes every detailed event when lanes are complete");
                denseOptions.gpu=true;auto gpuDensity=CachedTimelineOverview(cache,*store,denseOptions);Check(gpuDensity->events>=gpu->values.size() || gpuDensity->more,"GPU aggregate includes all placed and unplaced events");
                Check( CachedCpuWindow( cache, *store, begin, end ).get() == cpu.get() && CachedGpuWindow( cache, *store, begin, end ).get() == gpu.get(), "repeat read reuses immutable page handles" );
                cache.Clear();
                auto freshCpu = CachedCpuWindow( cache, *store, begin, end );
                auto freshGpu = CachedGpuWindow( cache, *store, begin, end );
                auto freshDensity=CachedTimelineOverview(cache,*store,denseOptions);Check(freshDensity.get()!=gpuDensity.get() && freshDensity->events==gpuDensity->events && freshDensity->cells.size()==gpuDensity->cells.size(),"aggregate cache retirement preserves counts and shape");
                Check( freshCpu.get() != cpu.get() && freshGpu.get() != gpu.get(), "retired pinned pages are reloaded" );
                SamePage( *cpu, *freshCpu );
                SamePage( *gpu, *freshGpu );
                Check( cache.Stats().bytes <= ( 64ull << 20 ) && CachedFrameOverview( cache, *store )->size() == overview->size(), "all window types share byte budget" );
                std::cout << "minute=" << minute << " cpu=" << cpu->values.size() << " gpu=" << gpu->values.size() << " bytes=" << cache.Stats().bytes << " ms=" << std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - started ).count() << '\n';
            }
            auto info = store->Info();
            const auto original = WindowCacheKey( info, 'C', 0, 1 );
            ++info.revision;
            Check( WindowCacheKey( info, 'C', 0, 1 ) != original, "revision is part of immutable cache identity" );
            Check( WindowCacheKey( info, 'G', 0, 1, 0, {} ) != WindowCacheKey( info, 'G', 0, 1, 0, 0 ), "unfiltered and context-zero windows have different keys" );
            return 0;
        }
        const uint64_t cost = StreamCache::Cost( "a", StreamCacheValue( Page( 1 ) ) );
        {
            StreamCache shared( cost * 3, 3 );
            auto value=std::make_shared<const StreamCacheValue>(Page(11));
            Check(shared.PutValue("one",value),"shared payload admission");
            const auto bytes=shared.Stats().bytes;
            Check(shared.PutValue("one",value) && shared.Stats().entries==1 && shared.Stats().bytes==bytes,"re-admitting the same immutable payload is idempotent");
            Check(!shared.PutValue("two",value),"cache must not pin a payload with multiple internal owners");
            value.reset();shared.Clear();
            Check(shared.Stats().bytes==0 && shared.Stats().entries==0,"cache-owned references do not prevent reclamation");
        }
        StreamCache cache( cost * 2, 2 );
        Check( cache.Put( "a", Page( 1 ) ), "first window page must enter byte cache" );
        auto pinned = cache.Get<StorePage<StoreZone>>( "a" );
        Check( pinned && pinned->values[0].id == 1, "immutable page handle" );
        Check( cache.Put( "b", Page( 2 ) ) && cache.Put( "c", Page( 3 ) ), "unpinned least-recent page is evicted" );
        Check( !cache.Get<StorePage<StoreZone>>( "b" ) && pinned->values[0].name.size() == 4096, "eviction leaves pinned pages intact" );
        auto other = cache.Get<StorePage<StoreZone>>( "c" );
        Check( !cache.Put( "d", Page( 4 ) ), "all pinned pages prevent budget overflow" );
        Check( cache.Stats().bytes <= cost * 2 && cache.Stats().rejections > 0, "cache includes pinned allocations" );
        cache.Clear();
        Check( !cache.Get<StorePage<StoreZone>>( "a" ) && pinned->values[0].id == 1 && cache.Stats().bytes > 0, "clear retires pinned pages without invalidating handles" );
        pinned.reset();
        other.reset();
        Check( cache.Put( "d", Page( 4 ) ) && cache.Stats().bytes <= cost * 2, "released retired pages are reclaimed" );
        cache.Clear();
        Check( cache.Stats().bytes == 0 && cache.Stats().entries == 0, "unpinned clear releases cache memory" );
        StreamCache tiny( cost - 1 );
        Check( !tiny.Put( "x", Page( 5 ) ) && tiny.Stats().bytes == 0, "oversized page does not evict or exceed budget" );
        StorePage<StoreGpuZone> gpu;
        gpu.values.emplace_back();
        Check( cache.Put( "gpu", std::move( gpu ) ) && cache.Get<StorePage<StoreGpuZone>>( "gpu" ), "GPU pages share the same cache budget" );
        bool wrong = false;
        try
        {
            cache.Get<StorePage<StoreZone>>( "gpu" );
        }
        catch( const std::exception& )
        {
            wrong = true;
        }
        Check( wrong, "typed key mismatch is rejected" );
        std::cout << "Stream cache tests passed.\n";
        return 0;
    }
    catch( const std::exception& e )
    {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}

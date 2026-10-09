#include "../src/TracyQueryVram.hpp"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

using namespace tracy;
using namespace tracy::query;
using json = nlohmann::json;

namespace
{

constexpr double MiB = 1024.0 * 1024.0;
constexpr int64_t Second = 1'000'000'000;

void Check( bool condition, const char* what )
{
    if( condition ) return;
    std::fprintf( stderr, "VRAM summary check failed: %s\n", what );
    std::exit( 1 );
}

uint64_t Value( const json& value )
{
    return std::stoull( value.get<std::string>() );
}

VramSeries& CategorySeries( VramSeriesSet& series, JnVram::Segment segment, JnVram::Ledger ledger, JnVram::Category category )
{
    return series.categories[size_t( segment )][size_t( ledger )][size_t( category )];
}

// Totals and DXGI every 250 ms over ten seconds: usage climbs from 4 to
// 7 GiB at the 6 s mark and comes back down; the budget is 7.5 GiB.
VramSeriesSet TotalsOnly()
{
    VramSeriesSet series;
    for( int64_t t = 0; t <= 10 * Second; t += Second / 4 )
    {
        const double usage = t >= 6 * Second && t < 8 * Second ? 7.0 * 1024 * MiB : 4.0 * 1024 * MiB;
        series.usage[0].push_back( { t, usage } );
        series.budget[0].push_back( { t, 7.5 * 1024 * MiB } );
        series.physical[0].push_back( { t, usage - 512 * MiB } );
        series.logical[0].push_back( { t, usage - 1024 * MiB } );
        series.usage[1].push_back( { t, 300 * MiB } );
        series.budget[1].push_back( { t, 16.0 * 1024 * MiB } );
    }
    return series;
}

}

int main()
{
    // No GPU memory plots at all.
    {
        const auto result = VramSummary( VramSeriesSet {}, VramRequest {} );
        Check( !result.at( "present" ).get<bool>(), "a trace without plots is not present" );
    }
    // DXGI and totals without categories, as captures before JNTracy ABI 1.8.
    {
        const auto result = VramSummary( TotalsOnly(), VramRequest {} );
        Check( result.at( "present" ).get<bool>() && !result.at( "categories_present" ).get<bool>(), "totals without categories" );
        Check( result.contains( "categories_reason" ), "the missing categories are explained" );
        const auto& local = result.at( "segments" ).at( "local" );
        Check( Value( local.at( "dxgi" ).at( "peak_usage_bytes" ) ) == uint64_t( 7.0 * 1024 * MiB ), "local peak usage" );
        Check( Value( local.at( "dxgi" ).at( "peak_time_ns" ) ) == uint64_t( 6 * Second ), "local peak time" );
        Check( local.at( "dxgi" ).at( "peak_budget_percent" ).get<double>() == 93.3, "peak share of the budget" );
        // 7 of 7.5 GiB is above 90 % but below 95 %, for 2 s.
        Check( Value( local.at( "dxgi" ).at( "time_at_or_above_90_percent_ns" ) ) == uint64_t( 2 * Second ), "time above 90 % of budget" );
        Check( Value( local.at( "dxgi" ).at( "time_at_or_above_95_percent_ns" ) ) == 0, "never above 95 % of budget" );
        Check( local.at( "at_peak" ).at( "untracked_bytes" ).get<std::string>() == std::to_string( int64_t( 512 * MiB ) ), "untracked at the peak" );
        Check( !local.at( "at_peak" ).contains( "categories" ), "no category breakdown without category plots" );
        Check( local.at( "series" ).at( "dxgi_usage_max_mib" ).size() == 60, "default 60 buckets" );
        Check( result.at( "segments" ).contains( "non_local" ), "both segments by default" );
    }
    // One segment only, a clipped range and no series.
    {
        VramRequest request;
        request.segments = { true, false };
        request.beginNs = 0;
        request.endNs = 5 * Second;
        request.buckets = 0;
        const auto result = VramSummary( TotalsOnly(), request );
        const auto& segments = result.at( "segments" );
        Check( segments.contains( "local" ) && !segments.contains( "non_local" ), "segment selection" );
        Check( Value( segments.at( "local" ).at( "dxgi" ).at( "peak_usage_bytes" ) ) == uint64_t( 4.0 * 1024 * MiB ), "range clips the peak" );
        Check( !segments.at( "local" ).contains( "series" ), "buckets 0 returns no series" );
    }
    // Categories: textures grow, a render target appears, a heap holds placed
    // resources; eviction for one second. The texture plot starts a moment
    // after the totals, as the engine sends its categories after them.
    {
        auto series = TotalsOnly();
        using JnVram::Category;
        using JnVram::Ledger;
        using JnVram::Segment;
        CategorySeries( series, Segment::Local, Ledger::Physical, Category::Texture ) = { { 1000, 1024 * MiB }, { 6 * Second, 2048 * MiB } };
        CategorySeries( series, Segment::Local, Ledger::Physical, Category::RenderTarget ) = { { 0, 0 }, { 6 * Second, 1536 * MiB }, { 8 * Second, 0 } };
        CategorySeries( series, Segment::Local, Ledger::Physical, Category::Heap ) = { { 0, 2560 * MiB } };
        CategorySeries( series, Segment::Local, Ledger::Logical, Category::Texture ) = { { 0, 900 * MiB }, { 6 * Second, 1900 * MiB } };
        CategorySeries( series, Segment::Local, Ledger::Logical, Category::Buffer ) = { { 0, 1200 * MiB } };
        CategorySeries( series, Segment::Local, Ledger::Resources, Category::Texture ) = { { 0, 400 }, { 6 * Second, 700 } };
        CategorySeries( series, Segment::Local, Ledger::Resources, Category::Heap ) = { { 0, 40 } };
        series.evicted = { { 0, 0 }, { 7 * Second, 256 * MiB }, { 8 * Second, 0 } };
        VramRequest request;
        request.atNs = 1 * Second;
        request.buckets = 10;
        const auto result = VramSummary( series, request );
        Check( result.at( "categories_present" ).get<bool>(), "categories present" );
        const auto& local = result.at( "segments" ).at( "local" );
        const auto& peakCategories = local.at( "at_peak" ).at( "categories" );
        Check( peakCategories.size() == 4, "nonzero categories at the peak" );
        Check( peakCategories[0].at( "category" ) == "Heap" && peakCategories[1].at( "category" ) == "Texture" &&
            peakCategories[2].at( "category" ) == "RenderTarget" && peakCategories[3].at( "category" ) == "Buffer", "categories by physical bytes" );
        Check( Value( peakCategories[2].at( "physical_bytes" ) ) == uint64_t( 1536 * MiB ), "render target at the peak" );
        Check( peakCategories[1].at( "resources" ).get<uint64_t>() == 700, "resources at the peak" );
        Check( peakCategories[0].at( "share_of_physical_percent" ).get<double>() == 41.7, "share of physical" );
        const auto& endCategories = local.at( "at_end" ).at( "categories" );
        Check( endCategories.size() == 3, "the render target is gone at the end" );
        const auto& early = local.at( "at_time" ).at( "categories" );
        Check( Value( early[1].at( "physical_bytes" ) ) == uint64_t( 1024 * MiB ), "textures at the requested time" );
        const auto& growth = local.at( "category_growth" );
        Check( growth[0].at( "category" ) == "Texture" && growth[0].at( "delta_bytes" ).get<std::string>() == std::to_string( int64_t( 1024 * MiB ) ),
            "texture growth leads" );
        bool renderTarget = false;
        for( const auto& row : growth )
            if( row.at( "category" ) == "RenderTarget" )
            {
                renderTarget = true;
                Check( row.at( "delta_bytes" ) == "0" && Value( row.at( "peak_bytes" ) ) == uint64_t( 1536 * MiB ), "a transient render target peaks without growth" );
            }
        Check( renderTarget, "the transient render target is reported" );
        const auto& buckets = local.at( "series" );
        Check( buckets.at( "dxgi_usage_max_mib" ).size() == 10 && buckets.at( "physical_max_mib" ).contains( "RenderTarget" ), "category series" );
        Check( buckets.at( "physical_max_mib" ).at( "RenderTarget" )[6].get<double>() == 1536.0, "render target bucket" );
        Check( Value( result.at( "eviction" ).at( "peak_evicted_bytes" ) ) == uint64_t( 256 * MiB ), "eviction peak" );
        Check( Value( result.at( "eviction" ).at( "time_with_evictions_ns" ) ) == uint64_t( Second ), "time with evictions" );
    }
    // The first collect after a connection can find the registry empty, before
    // the engine's D3D12 memory snapshot replays: growth starts at the next one.
    {
        using JnVram::Category;
        using JnVram::Ledger;
        using JnVram::Segment;
        VramSeriesSet series;
        series.physical[0] = { { 0, 0 }, { Second / 4, 3072 * MiB }, { 5 * Second, 4096 * MiB } };
        CategorySeries( series, Segment::Local, Ledger::Physical, Category::Texture ) = { { 10, 0 }, { Second / 4 + 10, 2048 * MiB }, { 5 * Second + 10, 3072 * MiB } };
        CategorySeries( series, Segment::Local, Ledger::Physical, Category::Buffer ) = { { 10, 0 }, { Second / 4 + 20, 1024 * MiB } };
        const auto result = VramSummary( series, VramRequest {} );
        const auto& growth = result.at( "segments" ).at( "local" ).at( "category_growth" );
        Check( growth.size() == 2 && growth[0].at( "category" ) == "Texture", "both categories grow from the registry's start" );
        Check( Value( growth[0].at( "physical_begin_bytes" ) ) == uint64_t( 2048 * MiB ) &&
            growth[0].at( "delta_bytes" ).get<std::string>() == std::to_string( int64_t( 1024 * MiB ) ), "the empty first sample is not growth" );
        Check( growth[1].at( "delta_bytes" ) == "0", "a category unchanged since the registry filled did not grow" );
    }
    // DXGI sends a sample's budget just after its usage: a peak at the first
    // sample still has its budget.
    {
        VramSeriesSet series;
        series.usage[0] = { { 0, 4096 * MiB }, { Second / 4, 2048 * MiB } };
        series.budget[0] = { { 10, 8192 * MiB }, { Second / 4 + 10, 8192 * MiB } };
        const auto result = VramSummary( series, VramRequest {} );
        const auto& dxgi = result.at( "segments" ).at( "local" ).at( "dxgi" );
        Check( Value( dxgi.at( "peak_time_ns" ) ) == 0 && dxgi.at( "peak_budget_percent" ).get<double>() == 50.0, "the first sample's budget is found" );
        Check( result.at( "segments" ).at( "local" ).at( "at_peak" ).at( "budget_percent" ).get<double>() == 50.0, "the peak's breakdown has its budget" );
    }
    // The driver demoted engine memory: DXGI below the engine's ledger.
    {
        VramSeriesSet series;
        series.usage[0] = { { 0, 1000 * MiB } };
        series.physical[0] = { { 0, 1200 * MiB } };
        const auto result = VramSummary( series, VramRequest {} );
        Check( result.at( "segments" ).at( "local" ).at( "at_end" ).at( "untracked_bytes" ).get<std::string>() == std::to_string( int64_t( -200 * MiB ) ),
            "untracked is negative under demotion" );
    }
    // Evicted video memory: the engine's physical bytes still count it, DXGI
    // usage no longer does.
    {
        VramSeriesSet series;
        series.usage[0] = { { 0, 1000 * MiB } };
        series.physical[0] = { { 0, 1200 * MiB } };
        series.evicted = { { 10, 300 * MiB } };
        series.usage[1] = { { 0, 100 * MiB } };
        series.physical[1] = { { 0, 80 * MiB } };
        const auto result = VramSummary( series, VramRequest {} );
        const auto& local = result.at( "segments" ).at( "local" ).at( "at_end" );
        Check( Value( local.at( "evicted_bytes" ) ) == uint64_t( 300 * MiB ), "evicted video memory" );
        Check( local.at( "untracked_bytes" ).get<std::string>() == std::to_string( int64_t( 100 * MiB ) ), "untracked counts the engine's memory still in video memory" );
        const auto& nonLocal = result.at( "segments" ).at( "non_local" ).at( "at_end" );
        Check( !nonLocal.contains( "evicted_bytes" ) && nonLocal.at( "untracked_bytes" ).get<std::string>() == std::to_string( int64_t( 20 * MiB ) ),
            "evictions leave system memory alone" );
    }
    // The engine records a connection's resources after their memory: the
    // registry holds resources from its first sample with logical bytes.
    {
        VramSeriesSet series;
        series.physical[0] = { { 0, 0 }, { Second, 2048 * MiB }, { 2 * Second, 3072 * MiB } };
        series.logical[0] = { { 0, 0 }, { Second, 0 }, { 2 * Second, 3000 * MiB } };
        Check( VramRegistryStart( series ) == 2 * Second, "resources fill the registry after their memory" );
        series.logical[0].clear();
        Check( VramRegistryStart( series ) == Second, "without logical totals, the registry starts with its memory" );
    }
    // The engine's resources: live at a time by size and by name, and their
    // growth and churn over a range.
    {
        std::vector<VramResourceInput> resources;
        const auto add = [&]( const char* name, char kind, char segment, double mib, int64_t created, std::optional<int64_t> freed, bool preCapture = false ) {
            VramResourceInput resource;
            resource.name = name;
            resource.kind = kind;
            resource.segment = segment;
            resource.sizeBytes = uint64_t( mib * MiB );
            resource.allocationId = resources.size() + 1;
            resource.createdNs = created;
            resource.freedNs = freed;
            resource.preCapture = preCapture;
            resources.push_back( resource );
        };
        // Created before the capture: its record comes when it connects, at 1 s.
        add( "Atlas", 'T', 'L', 256, Second, std::nullopt, true );
        add( "ShadowMap", 'T', 'L', 64, 2 * Second, 8 * Second );
        add( "Fog", 'T', 'L', 4, 3 * Second, 4 * Second + Second / 2 );
        add( "Fog", 'T', 'L', 4, 4 * Second, 5 * Second );
        add( "Fog", 'T', 'L', 4, 5 * Second, std::nullopt );
        add( "Clusters", 'B', 'L', 128, 6 * Second, std::nullopt );
        add( "Staging", 'U', 'N', 32, 2 * Second, std::nullopt );
        add( "", 'B', 'L', 1, 2 * Second, std::nullopt );

        const auto at = VramResourcesAt( resources, 0, 4 * Second + Second / 4, {}, 0, 2 );
        Check( at.at( "live_resources" ).get<uint64_t>() == 5, "video memory resources live at 4.25 s" );
        Check( Value( at.at( "live_bytes" ) ) == uint64_t( ( 256 + 64 + 4 + 4 + 1 ) * MiB ) && Value( at.at( "named_bytes" ) ) == uint64_t( ( 256 + 64 + 4 + 4 ) * MiB ), "their bytes" );
        const auto& largest = at.at( "resources" );
        Check( largest.size() == 2 && largest[0].at( "name" ) == "Atlas" && largest[1].at( "name" ) == "ShadowMap" && at.at( "resources_truncated" ).get<bool>(), "the largest first, to the limit" );
        Check( largest[0].at( "pre_capture" ).get<bool>() && largest[0].at( "kind" ) == "texture" && largest[1].at( "freed_ns" ) == std::to_string( 8 * Second ), "a resource's details" );
        const auto& byName = at.at( "by_name" );
        Check( byName.size() == 2 && at.at( "by_name_truncated" ).get<bool>(), "names to the limit" );
        const auto all = VramResourcesAt( resources, 0, 4 * Second + Second / 4, {}, 0, 10 );
        bool fog = false;
        for( const auto& row : all.at( "by_name" ) )
            if( row.at( "name" ) == "Fog" ) fog = row.at( "count" ).get<uint64_t>() == 2 && Value( row.at( "bytes" ) ) == uint64_t( 8 * MiB );
        Check( fog, "the resources of one name are grouped" );
        const auto early = VramResourcesAt( resources, 0, Second / 2, {}, 0, 10 );
        Check( early.at( "live_resources" ).get<uint64_t>() == 1 && early.at( "resources" )[0].at( "name" ) == "Atlas", "a pre-capture resource lived before its record" );
        const auto system = VramResourcesAt( resources, 1, 3 * Second, {}, 0, 10 );
        Check( system.at( "live_resources" ).get<uint64_t>() == 1 && system.at( "resources" )[0].at( "kind" ) == "upload", "system memory lists the upload buffers" );

        const auto growth = VramResourceGrowth( resources, 0, Second, 9 * Second, {}, 10 );
        Check( Value( growth.at( "at_begin" ).at( "bytes" ) ) == uint64_t( 256 * MiB ), "bytes at the begin" );
        Check( Value( growth.at( "at_end" ).at( "bytes" ) ) == uint64_t( ( 256 + 4 + 128 + 1 ) * MiB ), "bytes at the end" );
        Check( growth.at( "delta_bytes" ).get<std::string>() == std::to_string( int64_t( 133 * MiB ) ), "the growth" );
        const auto& grew = growth.at( "grew" );
        Check( grew.size() == 3 && grew[0].at( "name" ) == "Clusters" && grew[1].at( "name" ) == "Fog" &&
            grew[0].at( "delta_bytes" ).get<std::string>() == std::to_string( int64_t( 128 * MiB ) ), "the largest growth first" );
        const auto& created = growth.at( "most_created" );
        Check( created[0].at( "name" ) == "Fog" && created[0].at( "created" ).get<uint64_t>() == 3 && created[0].at( "freed" ).get<uint64_t>() == 2, "churn by name" );
        for( const auto& row : created ) Check( row.at( "name" ) != "Atlas", "a pre-capture resource was not created in the range" );
        Check( growth.at( "shrank" ).empty(), "nothing shrank" );
        const auto later = VramResourceGrowth( resources, 0, 7 * Second, 9 * Second, {}, 10 );
        Check( later.at( "shrank" ).size() == 1 && later.at( "shrank" )[0].at( "name" ) == "ShadowMap" &&
            later.at( "shrank" )[0].at( "delta_bytes" ).get<std::string>() == std::to_string( -int64_t( 64 * MiB ) ), "a freed resource shrinks its name" );
    }
    // Types from the engine's categories, the filters, and pages of the list.
    {
        std::vector<VramResourceInput> resources;
        const auto add = [&]( const char* name, char kind, const char* category, double mib ) {
            VramResourceInput resource;
            resource.name = name;
            resource.category = category;
            resource.kind = kind;
            resource.segment = 'L';
            resource.sizeBytes = uint64_t( mib * MiB );
            resource.allocationId = resources.size() + 1;
            resource.createdNs = Second / 2;
            resources.push_back( resource );
        };
        add( "GBufferA", 'T', "RenderTarget", 32 );
        add( "SceneDepth", 'T', "DepthStencil", 16 );
        add( "Albedo_rock", 'T', "Texture", 8 );
        add( "albedo_tree", 'T', "Texture", 4 );
        add( "TLAS", 'B', "RayTracing", 2 );
        // A record that predates categories goes by its kind.
        add( "Old", 'T', "", 1 );
        Check( IsVramResourceType( "render_target" ) && !IsVramResourceType( "RenderTarget" ), "type keys" );
        const auto all = VramResourcesAt( resources, 0, Second, {}, 0, 100 );
        Check( all.at( "categories_recorded" ).get<bool>() && all.at( "matched_resources" ).get<uint64_t>() == 6, "every resource" );
        const auto& first = all.at( "resources" )[0];
        Check( first.at( "type" ) == "render_target" && first.at( "category" ) == "RenderTarget" && first.at( "kind" ) == "texture", "a render target's type" );
        Check( all.at( "resources" )[5].at( "type" ) == "texture" && all.at( "resources" )[5].at( "category" ).is_null(), "a record without a category" );
        bool textures = false;
        for( const auto& row : all.at( "by_type" ) )
            if( row.at( "type" ) == "texture" ) textures = row.at( "resources" ).get<uint64_t>() == 3 && Value( row.at( "bytes" ) ) == uint64_t( 13 * MiB );
        Check( textures, "bytes by type" );
        const auto targets = VramResourcesAt( resources, 0, Second, { "render_target", "" }, 0, 100 );
        Check( targets.at( "matched_resources" ).get<uint64_t>() == 1 && targets.at( "resources" )[0].at( "name" ) == "GBufferA" &&
            targets.at( "by_type" ).size() == 4, "a type filter; by_type before it" );
        const auto albedo = VramResourcesAt( resources, 0, Second, { "", "ALBEDO" }, 0, 100 );
        Check( albedo.at( "matched_resources" ).get<uint64_t>() == 2 && Value( albedo.at( "matched_bytes" ) ) == uint64_t( 12 * MiB ), "a name filter in any case" );
        const auto page1 = VramResourcesAt( resources, 0, Second, {}, 0, 2 );
        const auto page2 = VramResourcesAt( resources, 0, Second, {}, 2, 2 );
        const auto page3 = VramResourcesAt( resources, 0, Second, {}, 4, 2 );
        Check( page1.at( "resources" )[1].at( "name" ) == "SceneDepth" && page1.at( "resources_truncated" ).get<bool>() &&
            page2.at( "resources" )[0].at( "name" ) == "Albedo_rock" && page2.at( "resources_truncated" ).get<bool>() &&
            page3.at( "resources" ).size() == 2 && page3.at( "resources" )[1].at( "name" ) == "Old" && !page3.at( "resources_truncated" ).get<bool>(), "pages" );
        const auto growth = VramResourceGrowth( resources, 0, 0, 2 * Second, { "texture", "" }, 10 );
        Check( growth.at( "grew" ).size() == 3 && growth.at( "grew" )[0].at( "name" ) == "Albedo_rock" && growth.at( "grew" )[0].at( "type" ) == "texture",
            "growth of one type" );
    }
    // The resources a frame's command lists referenced, resolved to the
    // generation of a reused pointer live when they were referenced.
    {
        std::vector<VramResourceInput> resources;
        const auto add = [&]( const char* name, uint64_t id, char kind, const char* category, char segment, double mib, int64_t created, std::optional<int64_t> freed ) {
            VramResourceInput resource;
            resource.name = name;
            resource.category = category;
            resource.kind = kind;
            resource.segment = segment;
            resource.sizeBytes = uint64_t( mib * MiB );
            resource.allocationId = id;
            resource.createdNs = created;
            resource.freedNs = freed;
            resources.push_back( resource );
        };
        add( "GBufferA", 0x100, 'T', "RenderTarget", 'L', 32, Second / 2, std::nullopt );
        add( "SceneDepth", 0x200, 'T', "DepthStencil", 'L', 16, Second / 2, std::nullopt );
        // The pointer 0x300 is reused: an old texture, then a buffer.
        add( "OldAtlas", 0x300, 'T', "Texture", 'L', 64, Second / 2, 2 * Second );
        add( "Clusters", 0x300, 'B', "Buffer", 'L', 8, 2 * Second, std::nullopt );
        add( "Staging", 0x400, 'U', "", 'N', 4, Second / 2, std::nullopt );
        add( "Unused", 0x500, 'T', "Texture", 'L', 128, Second / 2, std::nullopt );

        VramFrameReferences frame;
        frame.beginNs = 3 * Second;
        frame.endNs = 3 * Second + Second / 60;
        frame.passes = 3;
        frame.commandListSets = 3;
        // 0x600 is not among the engine's resources, as a swap chain buffer.
        frame.resources = { { 0x100, frame.beginNs, 2, 1u << 14 }, { 0x200, frame.beginNs, 1, 1u << 14 }, { 0x300, frame.beginNs + 10, 3, 1u << 14 },
            { 0x400, frame.beginNs, 1, 1u << 14 }, { 0x600, frame.beginNs, 1, 1u << 14 } };
        const auto used = VramFrameResources( resources, frame, {}, 0, 100 );
        Check( used.at( "granularity" ) == "frame" && used.at( "command_list_sets" ).get<uint64_t>() == 3, "frame granularity" );
        Check( used.at( "referenced_resources" ).get<uint64_t>() == 5 && used.at( "resolved_resources" ).get<uint64_t>() == 4 &&
            used.at( "unresolved_resources" ).get<uint64_t>() == 1, "a pointer outside the registry is unresolved" );
        Check( Value( used.at( "resolved_bytes" ) ) == uint64_t( ( 32 + 16 + 8 + 4 ) * MiB ), "the frame's bytes, without the unused texture" );
        const auto& rows = used.at( "resources" );
        Check( rows.size() == 4 && rows[0].at( "name" ) == "GBufferA" && rows[0].at( "passes" ).get<uint64_t>() == 2 && rows[0].at( "usage" ).is_null(),
            "largest first, with its command lists; no usage at frame granularity" );
        bool clusters = false;
        for( const auto& row : rows ) if( row.at( "name" ) == "Clusters" ) clusters = row.at( "type" ) == "buffer";
        Check( clusters, "a reused pointer resolves to the generation live when referenced" );
        for( const auto& row : rows ) Check( row.at( "name" ) != "OldAtlas" && row.at( "name" ) != "Unused", "resources the frame did not reference are left out" );
        bool video = false, system = false;
        for( const auto& row : used.at( "by_segment" ) )
        {
            if( row.at( "segment" ) == "local" ) video = row.at( "resources" ).get<uint64_t>() == 3;
            if( row.at( "segment" ) == "non_local" ) system = Value( row.at( "bytes" ) ) == uint64_t( 4 * MiB );
        }
        Check( video && system, "bytes by segment" );
        const auto targets = VramFrameResources( resources, frame, { "render_target", "" }, 0, 100 );
        Check( targets.at( "matched_resources" ).get<uint64_t>() == 1 && targets.at( "by_type" ).size() == 4, "a type filter; by_type before it" );

        // Pass granularity records how each pass used a resource.
        frame.commandListSets = 0;
        frame.resources = { { 0x100, frame.beginNs, 4, ( 1u << 1 ) | ( 1u << 11 ) } };
        const auto passes = VramFrameResources( resources, frame, {}, 0, 100 );
        Check( passes.at( "granularity" ) == "pass" && passes.at( "resources" )[0].at( "usage" ) == nlohmann::json::array( { "write", "color_attachment" } ),
            "pass granularity usage" );
        frame.passes = 0;
        frame.resources.clear();
        const auto empty = VramFrameResources( resources, frame, {}, 0, 100 );
        Check( empty.at( "granularity" ).is_null() && empty.at( "referenced_resources" ).get<uint64_t>() == 0, "a range without references" );
    }
    std::puts( "VRAM summary tests passed." );
    return 0;
}

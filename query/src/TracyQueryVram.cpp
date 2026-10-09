#include "TracyQueryVram.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>

namespace tracy::query
{
namespace
{

using json = nlohmann::json;
using namespace tracy::JnVram;

constexpr const char* SegmentKeys[Segments] = { "local", "non_local" };
// The engine sends the plots of one sample within microseconds of each other
// (a budget just after its usage, categories just after their totals), and
// samples 250 ms apart: a value of the same sample is read this much later.
constexpr int64_t SampleSlack = 1'000'000;

std::string Bytes( double value )
{
    return std::to_string( uint64_t( std::llround( std::max( 0.0, value ) ) ) );
}

std::string SignedBytes( double value )
{
    return std::to_string( int64_t( std::llround( value ) ) );
}

double Mib( double value )
{
    return std::round( value / ( 1024.0 * 1024.0 ) * 10 ) / 10;
}

// The value in effect at time t: the last point at or before it.
std::optional<double> At( const VramSeries& series, int64_t t )
{
    const auto next = std::upper_bound( series.begin(), series.end(), t, []( int64_t time, const VramPoint& point ) { return time < point.timeNs; } );
    if( next == series.begin() ) return std::nullopt;
    return std::prev( next )->value;
}

// The value in effect at the start of [begin, end), or the first value of a
// series that starts inside it: the engine sends its categories just after
// the totals and DXGI plots that start the range.
std::optional<double> First( const VramSeries& series, int64_t begin, int64_t end )
{
    if( const auto value = At( series, begin ) ) return value;
    const auto point = std::lower_bound( series.begin(), series.end(), begin, []( const VramPoint& p, int64_t time ) { return p.timeNs < time; } );
    if( point != series.end() && point->timeNs < end ) return point->value;
    return std::nullopt;
}

// The largest value in effect during [begin, end), and when it took effect.
std::optional<VramPoint> Peak( const VramSeries& series, int64_t begin, int64_t end )
{
    std::optional<VramPoint> peak;
    if( const auto value = At( series, begin ) ) peak = VramPoint { begin, *value };
    for( auto point = std::upper_bound( series.begin(), series.end(), begin, []( int64_t time, const VramPoint& p ) { return time < p.timeNs; } );
         point != series.end() && point->timeNs < end; ++point )
    {
        if( !peak || point->value > peak->value ) peak = *point;
    }
    return peak;
}

// Every time in (begin, end) at which one of the series changes, with both ends.
std::vector<int64_t> ChangeTimes( std::initializer_list<const VramSeries*> series, int64_t begin, int64_t end )
{
    std::vector<int64_t> times { begin, end };
    for( const auto* values : series )
        for( const auto& point : *values )
            if( point.timeNs > begin && point.timeNs < end ) times.push_back( point.timeNs );
    std::sort( times.begin(), times.end() );
    times.erase( std::unique( times.begin(), times.end() ), times.end() );
    return times;
}

double TimeWeightedMean( const VramSeries& series, int64_t begin, int64_t end )
{
    const auto times = ChangeTimes( { &series }, begin, end );
    double sum = 0;
    int64_t covered = 0;
    for( size_t n = 0; n + 1 < times.size(); ++n )
    {
        if( const auto value = At( series, times[n] ) )
        {
            sum += *value * double( times[n + 1] - times[n] );
            covered += times[n + 1] - times[n];
        }
    }
    return covered > 0 ? sum / double( covered ) : 0;
}

// How long during [begin, end) usage was at least `fraction` of the budget.
int64_t TimeAtOrAbove( const VramSeries& usage, const VramSeries& budget, double fraction, int64_t begin, int64_t end )
{
    const auto times = ChangeTimes( { &usage, &budget }, begin, end );
    int64_t total = 0;
    for( size_t n = 0; n + 1 < times.size(); ++n )
    {
        const auto used = At( usage, times[n] );
        const auto limit = At( budget, times[n] );
        if( used && limit && *limit > 0 && *used >= fraction * *limit ) total += times[n + 1] - times[n];
    }
    return total;
}

bool HasCategories( const VramSeriesSet& series, size_t segment )
{
    for( const auto& ledger : series.categories[segment] )
        for( const auto& category : ledger )
            if( !category.empty() ) return true;
    return false;
}

// When the engine's registry first held memory. Its first sample after a
// connection can be empty, taken before the D3D12 memory snapshot replayed
// into it.
std::optional<int64_t> RegistryStart( const VramSeriesSet& series, size_t segment )
{
    const auto firstHeld = []( const VramSeries& values ) -> std::optional<int64_t> {
        for( const auto& point : values )
            if( point.value > 0 ) return point.timeNs;
        return std::nullopt;
    };
    if( !series.physical[segment].empty() ) return firstHeld( series.physical[segment] );
    std::optional<int64_t> start;
    for( const auto& values : series.categories[segment][size_t( Ledger::Physical )] )
        if( const auto t = firstHeld( values ); t && ( !start || *t < *start ) ) start = t;
    return start;
}

json Breakdown( const VramSeriesSet& series, size_t segment, int64_t t )
{
    const auto usage = At( series.usage[segment], t );
    const auto budget = At( series.budget[segment], t + SampleSlack );
    // Before the registry first held memory the engine's values are not
    // known, rather than zero.
    const auto start = RegistryStart( series, segment );
    const bool known = start && t + SampleSlack >= *start;
    const auto physical = known ? At( series.physical[segment], t + SampleSlack ) : std::nullopt;
    const auto logical = known ? At( series.logical[segment], t + SampleSlack ) : std::nullopt;
    // The residency manager evicts video memory only. DXGI usage stops
    // counting what it evicts; the engine's physical bytes still count it.
    const auto evicted = known && segment == 0 ? At( series.evicted, t + SampleSlack ) : std::nullopt;
    json result = { { "time_ns", std::to_string( t ) } };
    result["dxgi_usage_bytes"] = usage ? json( Bytes( *usage ) ) : json( nullptr );
    result["dxgi_budget_bytes"] = budget ? json( Bytes( *budget ) ) : json( nullptr );
    result["budget_percent"] = usage && budget && *budget > 0 ? json( std::round( *usage / *budget * 1000 ) / 10 ) : json( nullptr );
    result["engine_physical_bytes"] = physical ? json( Bytes( *physical ) ) : json( nullptr );
    result["engine_logical_bytes"] = logical ? json( Bytes( *logical ) ) : json( nullptr );
    if( segment == 0 ) result["evicted_bytes"] = evicted ? json( Bytes( *evicted ) ) : json( nullptr );
    result["untracked_bytes"] = usage && physical ? json( SignedBytes( *usage - ( *physical - evicted.value_or( 0 ) ) ) ) : json( nullptr );
    if( !HasCategories( series, segment ) || !known ) return result;
    struct Row { size_t category; double physical, logical, resources; };
    std::vector<Row> rows;
    double physicalSum = 0;
    const auto sample = t + SampleSlack;
    for( size_t category = 0; category < Categories; ++category )
    {
        const auto& ledgers = series.categories[segment];
        Row row { category, At( ledgers[size_t( Ledger::Physical )][category], sample ).value_or( 0 ),
            At( ledgers[size_t( Ledger::Logical )][category], sample ).value_or( 0 ), At( ledgers[size_t( Ledger::Resources )][category], sample ).value_or( 0 ) };
        physicalSum += row.physical;
        if( row.physical != 0 || row.logical != 0 || row.resources != 0 ) rows.push_back( row );
    }
    std::sort( rows.begin(), rows.end(), []( const Row& a, const Row& b ) { return a.physical != b.physical ? a.physical > b.physical : a.logical > b.logical; } );
    json categories = json::array();
    for( const auto& row : rows )
    {
        categories.push_back( { { "category", CategoryNames[row.category] }, { "physical_bytes", Bytes( row.physical ) },
            { "logical_bytes", Bytes( row.logical ) }, { "resources", uint64_t( std::llround( std::max( 0.0, row.resources ) ) ) },
            { "share_of_physical_percent", physicalSum > 0 ? std::round( row.physical / physicalSum * 1000 ) / 10 : 0.0 } } );
    }
    result["categories"] = std::move( categories );
    return result;
}

json Growth( const VramSeriesSet& series, size_t segment, int64_t begin, int64_t end )
{
    // Growth is measured from the registry's first sample that held memory:
    // before it the registry is empty, not small.
    if( const auto start = RegistryStart( series, segment ); start && *start + SampleSlack > begin && *start < end )
        begin = std::min( *start + SampleSlack, end - 1 );
    struct Row { size_t category; double first, last; VramPoint peak; };
    std::vector<Row> rows;
    for( size_t category = 0; category < Categories; ++category )
    {
        const auto& values = series.categories[segment][size_t( Ledger::Physical )][category];
        if( values.empty() ) continue;
        const auto peak = Peak( values, begin, end );
        if( !peak ) continue;
        rows.push_back( { category, First( values, begin, end ).value_or( 0 ), At( values, end - 1 ).value_or( 0 ), *peak } );
    }
    std::sort( rows.begin(), rows.end(), []( const Row& a, const Row& b ) { return a.last - a.first > b.last - b.first; } );
    json growth = json::array();
    for( const auto& row : rows )
    {
        if( row.first == 0 && row.last == 0 && row.peak.value == 0 ) continue;
        growth.push_back( { { "category", CategoryNames[row.category] }, { "physical_begin_bytes", Bytes( row.first ) },
            { "physical_end_bytes", Bytes( row.last ) }, { "delta_bytes", SignedBytes( row.last - row.first ) },
            { "peak_bytes", Bytes( row.peak.value ) }, { "peak_time_ns", std::to_string( row.peak.timeNs ) } } );
    }
    return growth;
}

const char* KindKey( char kind )
{
    switch( kind )
    {
    case 'T': return "texture";
    case 'B': return "buffer";
    case 'U': return "upload";
    case 'R': return "readback";
    default: return "unknown";
    }
}

constexpr const char* TypeKeys[] = { "texture", "render_target", "depth_stencil", "buffer", "ray_tracing", "other", "upload", "readback" };

// A resource's type: its category, or its kind in records that predate
// categories; upload and readback buffers by their kind.
const char* TypeKey( const VramResourceInput& resource )
{
    if( resource.kind == 'U' ) return "upload";
    if( resource.kind == 'R' ) return "readback";
    if( resource.category.empty() ) return resource.kind == 'T' ? "texture" : resource.kind == 'B' ? "buffer" : "other";
    if( resource.category == "Texture" ) return "texture";
    if( resource.category == "RenderTarget" ) return "render_target";
    if( resource.category == "DepthStencil" ) return "depth_stencil";
    if( resource.category == "Buffer" ) return "buffer";
    if( resource.category == "RayTracing" ) return "ray_tracing";
    return "other";
}

std::string Lower( std::string_view text )
{
    std::string lower( text );
    for( auto& c : lower ) c = char( std::tolower( static_cast<unsigned char>( c ) ) );
    return lower;
}

// Whether a resource of the segment passes the filter, whose name is lower case.
bool Passes( const VramResourceInput& resource, char segment, const VramResourceFilter& filter, const std::string& name )
{
    if( resource.segment != segment ) return false;
    if( !filter.type.empty() && filter.type != TypeKey( resource ) ) return false;
    return name.empty() || Lower( resource.name ).find( name ) != std::string::npos;
}

// A resource created before the capture lived before its record, which the
// engine sends when the capture connects.
bool LiveAt( const VramResourceInput& resource, int64_t t )
{
    return ( resource.preCapture || resource.createdNs <= t ) && ( !resource.freedNs || *resource.freedNs > t );
}

double Percent( uint64_t part, uint64_t whole )
{
    return whole > 0 ? std::round( double( part ) / double( whole ) * 1000 ) / 10 : 0.0;
}

json FilterJson( const VramResourceFilter& filter )
{
    return { { "type", filter.type.empty() ? json( nullptr ) : json( filter.type ) }, { "name", filter.name.empty() ? json( nullptr ) : json( filter.name ) } };
}

json ResourceSemantics( bool categories )
{
    auto semantics = json::array( {
        "resources are the engine's named D3D12 resources (GTMEM logical records) at their logical size; placed resources share their heap, so their sum can exceed engine_physical",
        "local lists the textures and buffers in video memory, non_local the upload and readback buffers in system memory",
        "type is the engine's category (texture, render_target, depth_stencil, buffer, ray_tracing, other); upload and readback buffers go by their kind",
        "a resource created before the capture connected has pre_capture true: it counts as live from before the capture, and its created_ns is when the engine sent its record",
        "names repeat: by_name and the growth lists group every resource of one name; an empty name is a resource the engine did not name" } );
    if( !categories ) semantics.push_back( "categories_recorded is false: the engine predates per-resource categories, so render targets and depth buffers are typed texture" );
    return semantics;
}

// The largest value in effect in each of `buckets` equal parts of the range,
// in MiB.
json Buckets( const VramSeries& series, int64_t begin, int64_t width, size_t buckets )
{
    json values = json::array();
    for( size_t n = 0; n < buckets; ++n )
    {
        const auto peak = Peak( series, begin + int64_t( n ) * width, begin + int64_t( n + 1 ) * width );
        values.push_back( peak ? json( Mib( peak->value ) ) : json( nullptr ) );
    }
    return values;
}

json Segment( const VramSeriesSet& series, size_t segment, const VramRequest& request, int64_t begin, int64_t end )
{
    const auto& usage = series.usage[segment];
    const auto& budget = series.budget[segment];
    json result = {
        { "segment", SegmentKeys[segment] },
        { "label", segment == 0 ? "video memory (D3D12 LOCAL)" : "system memory the GPU uses (D3D12 NON_LOCAL)" },
        { "series_present", {
            { "dxgi_usage", !usage.empty() }, { "dxgi_budget", !budget.empty() },
            { "engine_physical", !series.physical[segment].empty() }, { "engine_logical", !series.logical[segment].empty() },
            { "categories", HasCategories( series, segment ) } } }
    };
    if( const auto peak = Peak( usage, begin, end ) )
    {
        const auto limit = At( budget, peak->timeNs + SampleSlack );
        result["dxgi"] = {
            { "peak_usage_bytes", Bytes( peak->value ) }, { "peak_time_ns", std::to_string( peak->timeNs ) },
            { "mean_usage_bytes", Bytes( TimeWeightedMean( usage, begin, end ) ) },
            { "end_usage_bytes", At( usage, end - 1 ) ? json( Bytes( *At( usage, end - 1 ) ) ) : json( nullptr ) },
            { "budget_at_peak_bytes", limit ? json( Bytes( *limit ) ) : json( nullptr ) },
            { "peak_budget_percent", limit && *limit > 0 ? json( std::round( peak->value / *limit * 1000 ) / 10 ) : json( nullptr ) },
            { "time_at_or_above_90_percent_ns", std::to_string( TimeAtOrAbove( usage, budget, 0.9, begin, end ) ) },
            { "time_at_or_above_95_percent_ns", std::to_string( TimeAtOrAbove( usage, budget, 0.95, begin, end ) ) },
            { "time_at_or_above_budget_ns", std::to_string( TimeAtOrAbove( usage, budget, 1.0, begin, end ) ) } };
        result["at_peak"] = Breakdown( series, segment, peak->timeNs );
    }
    if( const auto peak = Peak( series.physical[segment], begin, end ) )
        result["engine_physical_peak"] = { { "bytes", Bytes( peak->value ) }, { "time_ns", std::to_string( peak->timeNs ) } };
    result["at_end"] = Breakdown( series, segment, end - 1 );
    if( request.atNs ) result["at_time"] = Breakdown( series, segment, *request.atNs );
    if( HasCategories( series, segment ) ) result["category_growth"] = Growth( series, segment, begin, end );
    if( request.buckets > 0 && end > begin )
    {
        const int64_t width = std::max<int64_t>( 1, ( end - begin + int64_t( request.buckets ) - 1 ) / int64_t( request.buckets ) );
        json values = {
            { "bucket_ns", std::to_string( width ) }, { "begin_ns", std::to_string( begin ) },
            { "dxgi_usage_max_mib", Buckets( usage, begin, width, request.buckets ) },
            { "dxgi_budget_mib", Buckets( budget, begin, width, request.buckets ) },
            { "engine_physical_max_mib", Buckets( series.physical[segment], begin, width, request.buckets ) } };
        if( HasCategories( series, segment ) )
        {
            json physical = json::object();
            for( size_t category = 0; category < Categories; ++category )
            {
                const auto& points = series.categories[segment][size_t( Ledger::Physical )][category];
                if( !points.empty() && Peak( points, begin, end ).value_or( VramPoint {} ).value != 0 )
                    physical[CategoryNames[category]] = Buckets( points, begin, width, request.buckets );
            }
            values["physical_max_mib"] = std::move( physical );
        }
        result["series"] = std::move( values );
    }
    return result;
}

}

json VramSummary( const VramSeriesSet& series, const VramRequest& request )
{
    int64_t first = std::numeric_limits<int64_t>::max(), last = std::numeric_limits<int64_t>::min();
    const auto extend = [&]( const VramSeries& values ) {
        if( values.empty() ) return;
        first = std::min( first, values.front().timeNs );
        last = std::max( last, values.back().timeNs );
    };
    // The range spans every plot: a collect sends its categories just after
    // its totals.
    bool categories = false;
    for( size_t segment = 0; segment < Segments; ++segment )
    {
        extend( series.usage[segment] );
        extend( series.budget[segment] );
        extend( series.physical[segment] );
        extend( series.logical[segment] );
        for( const auto& ledger : series.categories[segment] )
            for( const auto& values : ledger ) extend( values );
        categories |= HasCategories( series, segment );
    }
    if( first > last )
        return { { "present", false }, { "reason", "the trace has no DXGI or engine GPU memory plots (GPU.DXGI.*, GPU.VRAM.*)" } };
    const int64_t begin = request.beginNs.value_or( first );
    const int64_t end = std::max( begin + 1, request.endNs.value_or( last + 1 ) );
    json segments = json::object();
    for( size_t segment = 0; segment < Segments; ++segment )
        if( request.segments[segment] ) segments[SegmentKeys[segment]] = Segment( series, segment, request, begin, end );
    json result = {
        { "present", true }, { "categories_present", categories },
        { "range", { { "begin_ns", std::to_string( begin ) }, { "end_ns", std::to_string( end ) } } },
        { "segments", std::move( segments ) },
        { "semantics", json::array( {
            "local is video memory and non_local system memory; DXGI budgets them separately, so never add the two",
            "dxgi_usage is what the driver reports for the process; engine_physical is what the engine allocated from D3D12; engine_logical is the resources inside, so aliased placed resources can exceed it",
            "untracked = dxgi_usage - (engine_physical - evicted_bytes) on local and dxgi_usage - engine_physical on non_local: swap chains, descriptor heaps, PSOs, driver memory",
            "evicted_bytes is engine memory the residency manager evicted from video memory: engine_physical still counts it, dxgi_usage no longer does",
            "categories split engine_physical and engine_logical: Heap is heaps holding placed resources, Other is allocations without a resource",
            "a plot's value holds until its next point; category plots are sent when they change, totals and DXGI every 250 ms" } ) }
    };
    if( !categories )
        result["categories_reason"] = "the capture predates per-category GPU memory plots (JNTracy ABI 1.8) or GPU memory tracking was off";
    if( !series.evicted.empty() )
    {
        const auto peak = Peak( series.evicted, begin, end );
        result["eviction"] = {
            { "peak_evicted_bytes", Bytes( peak ? peak->value : 0 ) },
            { "peak_time_ns", peak ? json( std::to_string( peak->timeNs ) ) : json( nullptr ) },
            { "time_with_evictions_ns", std::to_string( TimeAtOrAbove( series.evicted, VramSeries { { std::numeric_limits<int64_t>::min(), 1.0 } }, 1.0, begin, end ) ) } };
    }
    return result;
}

std::optional<int64_t> VramRegistryStart( const VramSeriesSet& series )
{
    // The engine records a connection's resources after their memory, so the
    // logical totals are the last to fill.
    std::optional<int64_t> start;
    for( size_t segment = 0; segment < Segments; ++segment )
        for( const auto& point : series.logical[segment] )
        {
            if( point.value <= 0 ) continue;
            if( !start || point.timeNs < *start ) start = point.timeNs;
            break;
        }
    if( start ) return start;
    for( size_t segment = 0; segment < Segments; ++segment )
        if( const auto t = RegistryStart( series, segment ); t && ( !start || *t < *start ) ) start = t;
    return start;
}

std::optional<int64_t> VramUsagePeak( const VramSeriesSet& series, size_t segment )
{
    const auto& usage = series.usage[segment];
    if( usage.empty() ) return std::nullopt;
    const auto peak = Peak( usage, usage.front().timeNs, std::numeric_limits<int64_t>::max() );
    return peak ? std::optional<int64_t>( peak->timeNs ) : std::nullopt;
}

bool IsVramResourceType( const std::string& type )
{
    return std::find_if( std::begin( TypeKeys ), std::end( TypeKeys ), [&]( const char* key ) { return type == key; } ) != std::end( TypeKeys );
}

json VramResourcesAt( const std::vector<VramResourceInput>& resources, size_t segment, int64_t timeNs,
    const VramResourceFilter& filter, size_t offset, size_t limit )
{
    const char code = segment == 0 ? 'L' : 'N';
    const auto name = Lower( filter.name );
    std::vector<const VramResourceInput*> matched;
    uint64_t live = 0, liveBytes = 0, matchedBytes = 0, named = 0;
    bool categories = false;
    std::unordered_map<std::string_view, std::pair<uint64_t, uint64_t>> byName, byType;
    for( const auto& resource : resources )
    {
        if( resource.segment != code || !LiveAt( resource, timeNs ) ) continue;
        live++;
        liveBytes += resource.sizeBytes;
        categories |= !resource.category.empty();
        auto& type = byType[TypeKey( resource )];
        type.first++;
        type.second += resource.sizeBytes;
        if( !Passes( resource, code, filter, name ) ) continue;
        matched.push_back( &resource );
        matchedBytes += resource.sizeBytes;
        if( !resource.name.empty() ) named += resource.sizeBytes;
        auto& group = byName[resource.name];
        group.first++;
        group.second += resource.sizeBytes;
    }
    std::stable_sort( matched.begin(), matched.end(), []( const VramResourceInput* a, const VramResourceInput* b ) {
        return a->sizeBytes != b->sizeBytes ? a->sizeBytes > b->sizeBytes : a->createdNs < b->createdNs; } );
    const auto begin = std::min( offset, matched.size() );
    const auto end = std::min( begin + limit, matched.size() );
    json page = json::array();
    for( size_t n = begin; n < end; ++n )
    {
        const auto& resource = *matched[n];
        page.push_back( { { "name", resource.name }, { "type", TypeKey( resource ) },
            { "category", resource.category.empty() ? json( nullptr ) : json( resource.category ) }, { "kind", KindKey( resource.kind ) },
            { "size_bytes", Bytes( double( resource.sizeBytes ) ) }, { "share_percent", Percent( resource.sizeBytes, liveBytes ) },
            { "created_ns", std::to_string( resource.createdNs ) }, { "freed_ns", resource.freedNs ? json( std::to_string( *resource.freedNs ) ) : json( nullptr ) },
            { "pre_capture", resource.preCapture }, { "allocation_id", std::to_string( resource.allocationId ) } } );
    }
    using Group = std::pair<std::string_view, std::pair<uint64_t, uint64_t>>;
    const auto bytesFirst = []( const Group& a, const Group& b ) { return a.second.second != b.second.second ? a.second.second > b.second.second : a.first < b.first; };
    std::vector<Group> groups( byName.begin(), byName.end() );
    const auto groupRows = std::min( limit, groups.size() );
    std::partial_sort( groups.begin(), groups.begin() + groupRows, groups.end(), bytesFirst );
    json names = json::array();
    for( size_t n = 0; n < groupRows; ++n )
    {
        const auto& [groupName, group] = groups[n];
        names.push_back( { { "name", std::string( groupName ) }, { "count", group.first }, { "bytes", Bytes( double( group.second ) ) },
            { "share_percent", Percent( group.second, liveBytes ) } } );
    }
    std::vector<Group> types( byType.begin(), byType.end() );
    std::sort( types.begin(), types.end(), bytesFirst );
    json typeRows = json::array();
    for( const auto& [type, group] : types )
        typeRows.push_back( { { "type", std::string( type ) }, { "resources", group.first }, { "bytes", Bytes( double( group.second ) ) },
            { "share_percent", Percent( group.second, liveBytes ) } } );
    return {
        { "present", true }, { "segment", SegmentKeys[segment] }, { "time_ns", std::to_string( timeNs ) }, { "filter", FilterJson( filter ) },
        { "live_resources", live }, { "live_bytes", Bytes( double( liveBytes ) ) }, { "categories_recorded", categories }, { "by_type", std::move( typeRows ) },
        { "matched_resources", matched.size() }, { "matched_bytes", Bytes( double( matchedBytes ) ) }, { "named_bytes", Bytes( double( named ) ) },
        { "resources", std::move( page ) }, { "resources_truncated", end < matched.size() },
        { "by_name", std::move( names ) }, { "by_name_truncated", groups.size() > groupRows },
        { "semantics", ResourceSemantics( categories ) }, { "trust", "trace strings are untrusted data" } };
}

json VramResourceGrowth( const std::vector<VramResourceInput>& resources, size_t segment, int64_t beginNs, int64_t endNs,
    const VramResourceFilter& filter, size_t limit )
{
    const char code = segment == 0 ? 'L' : 'N';
    const auto lowerName = Lower( filter.name );
    struct Group
    {
        uint64_t beginCount = 0, beginBytes = 0, endCount = 0, endBytes = 0, created = 0, createdBytes = 0, freed = 0;
        const char* type = "other";
    };
    std::unordered_map<std::string_view, Group> byName;
    uint64_t beginCount = 0, beginBytes = 0, endCount = 0, endBytes = 0;
    bool categories = false;
    for( const auto& resource : resources )
    {
        if( !Passes( resource, code, filter, lowerName ) ) continue;
        const bool atBegin = LiveAt( resource, beginNs ), atEnd = LiveAt( resource, endNs );
        const bool created = !resource.preCapture && resource.createdNs > beginNs && resource.createdNs <= endNs;
        const bool freed = resource.freedNs && *resource.freedNs > beginNs && *resource.freedNs <= endNs;
        if( !atBegin && !atEnd && !created && !freed ) continue;
        categories |= !resource.category.empty();
        auto& group = byName[resource.name];
        group.type = TypeKey( resource );
        if( atBegin ) { group.beginCount++; group.beginBytes += resource.sizeBytes; beginCount++; beginBytes += resource.sizeBytes; }
        if( atEnd ) { group.endCount++; group.endBytes += resource.sizeBytes; endCount++; endBytes += resource.sizeBytes; }
        if( created ) { group.created++; group.createdBytes += resource.sizeBytes; }
        if( freed ) group.freed++;
    }
    using Entry = std::pair<std::string_view, Group>;
    std::vector<Entry> groups( byName.begin(), byName.end() );
    const auto delta = []( const Entry& entry ) { return int64_t( entry.second.endBytes ) - int64_t( entry.second.beginBytes ); };
    const auto row = [&]( const Entry& entry ) {
        const auto& group = entry.second;
        return json { { "name", std::string( entry.first ) }, { "type", group.type },
            { "bytes_at_begin", Bytes( double( group.beginBytes ) ) }, { "bytes_at_end", Bytes( double( group.endBytes ) ) },
            { "delta_bytes", SignedBytes( double( delta( entry ) ) ) }, { "count_at_begin", group.beginCount }, { "count_at_end", group.endCount },
            { "created", group.created }, { "created_bytes", Bytes( double( group.createdBytes ) ) }, { "freed", group.freed } };
    };
    // The first `limit` entries passing `keep` in `order`.
    const auto list = [&]( auto keep, auto order ) {
        std::vector<const Entry*> kept;
        for( const auto& entry : groups ) if( keep( entry ) ) kept.push_back( &entry );
        const auto rows = std::min( limit, kept.size() );
        std::partial_sort( kept.begin(), kept.begin() + rows, kept.end(), [&]( const Entry* a, const Entry* b ) { return order( *a, *b ); } );
        json values = json::array();
        for( size_t n = 0; n < rows; ++n ) values.push_back( row( *kept[n] ) );
        return values;
    };
    const auto byDelta = [&]( const Entry& a, const Entry& b ) { return delta( a ) != delta( b ) ? delta( a ) > delta( b ) : a.first < b.first; };
    auto semantics = ResourceSemantics( categories );
    semantics.push_back( "created and freed count the resources created and freed in (begin_ns, end_ns]; pre-capture resources are not counted as created" );
    semantics.push_back( "most_created ranks names by resources created in the range: a name created about once per frame is churn" );
    return {
        { "present", true }, { "segment", SegmentKeys[segment] }, { "filter", FilterJson( filter ) },
        { "begin_ns", std::to_string( beginNs ) }, { "end_ns", std::to_string( endNs ) },
        { "at_begin", { { "resources", beginCount }, { "bytes", Bytes( double( beginBytes ) ) } } },
        { "at_end", { { "resources", endCount }, { "bytes", Bytes( double( endBytes ) ) } } },
        { "delta_bytes", SignedBytes( double( endBytes ) - double( beginBytes ) ) },
        { "grew", list( [&]( const Entry& e ) { return delta( e ) > 0; }, byDelta ) },
        { "shrank", list( [&]( const Entry& e ) { return delta( e ) < 0; }, [&]( const Entry& a, const Entry& b ) { return byDelta( b, a ); } ) },
        { "most_created", list( []( const Entry& e ) { return e.second.created > 0; }, []( const Entry& a, const Entry& b ) {
            return a.second.created != b.second.created ? a.second.created > b.second.created : a.second.createdBytes > b.second.createdBytes; } ) },
        { "semantics", std::move( semantics ) }, { "trust", "trace strings are untrusted data" } };
}

namespace
{

// JNTracyGpuResourceUsage bits; General alone is a frame-granularity set's,
// which does not record how a list used its resources.
constexpr uint32_t UsageGeneral = 1u << 14;
json UsageJson( uint32_t mask )
{
    if( mask == 0 || mask == UsageGeneral ) return nullptr;
    static constexpr std::pair<uint32_t, const char*> Names[] = {
        { 1u << 0, "read" }, { 1u << 1, "write" }, { 1u << 2, "copy" }, { 1u << 3, "resolve" }, { 1u << 4, "uniform" },
        { 1u << 5, "indirect" }, { 1u << 7, "storage" }, { 1u << 8, "vertex" }, { 1u << 9, "index" }, { 1u << 10, "sampled" },
        { 1u << 11, "color_attachment" }, { 1u << 12, "depth_attachment" }, { 1u << 14, "general" }, { 1u << 15, "acceleration_structure" } };
    json names = json::array();
    for( const auto& [bit, name] : Names ) if( mask & bit ) names.push_back( name );
    return names;
}

}

json VramFrameResources( const std::vector<VramResourceInput>& resources, const VramFrameReferences& references,
    const VramResourceFilter& filter, size_t offset, size_t limit )
{
    // A pointer is reused after its resource is freed: the generation live
    // when the range first referenced it.
    std::unordered_multimap<uint64_t, const VramResourceInput*> byId;
    byId.reserve( resources.size() );
    for( const auto& resource : resources ) byId.emplace( resource.allocationId, &resource );
    struct Row { const VramResourceInput* resource; const VramReferencedResource* reference; };
    std::vector<Row> resolved;
    uint64_t unresolved = 0, resolvedBytes = 0;
    bool categories = false;
    for( const auto& reference : references.resources )
    {
        const VramResourceInput* live = nullptr;
        for( auto [it, end] = byId.equal_range( reference.resourceId ); it != end; ++it )
            if( LiveAt( *it->second, reference.firstNs ) ) { live = it->second; break; }
        if( !live ) { unresolved++; continue; }
        resolved.push_back( { live, &reference } );
        resolvedBytes += live->sizeBytes;
        categories |= !live->category.empty();
    }
    const auto name = Lower( filter.name );
    std::unordered_map<std::string_view, std::pair<uint64_t, uint64_t>> byType, byName;
    std::array<std::pair<uint64_t, uint64_t>, Segments> bySegment {};
    std::vector<const Row*> matched;
    uint64_t matchedBytes = 0;
    for( const auto& row : resolved )
    {
        const auto& resource = *row.resource;
        auto& type = byType[TypeKey( resource )];
        type.first++;
        type.second += resource.sizeBytes;
        auto& segment = bySegment[resource.segment == 'L' ? 0 : 1];
        segment.first++;
        segment.second += resource.sizeBytes;
        if( !Passes( resource, resource.segment, filter, name ) ) continue;
        matched.push_back( &row );
        matchedBytes += resource.sizeBytes;
        auto& group = byName[resource.name];
        group.first++;
        group.second += resource.sizeBytes;
    }
    std::stable_sort( matched.begin(), matched.end(), []( const Row* a, const Row* b ) {
        return a->resource->sizeBytes != b->resource->sizeBytes ? a->resource->sizeBytes > b->resource->sizeBytes : a->resource->createdNs < b->resource->createdNs; } );
    const auto first = std::min( offset, matched.size() );
    const auto last = std::min( first + limit, matched.size() );
    json page = json::array();
    for( size_t n = first; n < last; ++n )
    {
        const auto& resource = *matched[n]->resource;
        const auto& reference = *matched[n]->reference;
        page.push_back( { { "name", resource.name }, { "type", TypeKey( resource ) },
            { "category", resource.category.empty() ? json( nullptr ) : json( resource.category ) }, { "kind", KindKey( resource.kind ) },
            { "segment", SegmentKeys[resource.segment == 'L' ? 0 : 1] }, { "size_bytes", Bytes( double( resource.sizeBytes ) ) },
            { "share_percent", Percent( resource.sizeBytes, resolvedBytes ) }, { "passes", reference.passes }, { "usage", UsageJson( reference.usageMask ) },
            { "created_ns", std::to_string( resource.createdNs ) }, { "freed_ns", resource.freedNs ? json( std::to_string( *resource.freedNs ) ) : json( nullptr ) },
            { "pre_capture", resource.preCapture }, { "allocation_id", std::to_string( resource.allocationId ) } } );
    }
    using Group = std::pair<std::string_view, std::pair<uint64_t, uint64_t>>;
    const auto bytesFirst = []( const Group& a, const Group& b ) { return a.second.second != b.second.second ? a.second.second > b.second.second : a.first < b.first; };
    std::vector<Group> types( byType.begin(), byType.end() );
    std::sort( types.begin(), types.end(), bytesFirst );
    json typeRows = json::array();
    for( const auto& [type, group] : types )
        typeRows.push_back( { { "type", std::string( type ) }, { "resources", group.first }, { "bytes", Bytes( double( group.second ) ) },
            { "share_percent", Percent( group.second, resolvedBytes ) } } );
    json segmentRows = json::array();
    for( size_t segment = 0; segment < Segments; ++segment )
        if( bySegment[segment].first ) segmentRows.push_back( { { "segment", SegmentKeys[segment] }, { "resources", bySegment[segment].first },
            { "bytes", Bytes( double( bySegment[segment].second ) ) } } );
    std::vector<Group> groups( byName.begin(), byName.end() );
    const auto groupRows = std::min( limit, groups.size() );
    std::partial_sort( groups.begin(), groups.begin() + groupRows, groups.end(), bytesFirst );
    json names = json::array();
    for( size_t n = 0; n < groupRows; ++n )
    {
        const auto& [groupName, group] = groups[n];
        names.push_back( { { "name", std::string( groupName ) }, { "count", group.first }, { "bytes", Bytes( double( group.second ) ) },
            { "share_percent", Percent( group.second, resolvedBytes ) } } );
    }
    const auto perPass = references.passes - references.commandListSets;
    auto semantics = json::array( {
        "the resources are those the GPU reference passes beginning in [begin_ns, end_ns) referenced, each once, resolved to the engine's GTMEM resources live when first referenced",
        "with frame granularity ([GpuReference] Granularity=Frame) each command list sends, when it is closed, every resource it accessed: a frame's resources are those of the command lists closed during it, which the render thread can close a frame after the main thread prepared them",
        "passes counts the reference passes that referenced a resource: command lists at frame granularity, GPU passes at pass granularity",
        "usage is null at frame granularity, which does not record how a list used a resource",
        "unresolved_resources were referenced but are not among the engine's named resources then: swap-chain buffers and resources created outside the D3D12 memory manager",
        "share_percent is of the resolved bytes; placed resources share their heap, so the bytes can exceed the memory the frame touched" } );
    if( !categories ) semantics.push_back( "categories_recorded is false: the engine predates per-resource categories, so render targets and depth buffers are typed texture" );
    return {
        { "present", true }, { "range", { { "begin_ns", std::to_string( references.beginNs ) }, { "end_ns", std::to_string( references.endNs ) } } },
        { "granularity", references.passes == 0 ? json( nullptr ) : json( perPass == 0 ? "frame" : references.commandListSets == 0 ? "pass" : "mixed" ) },
        { "reference_passes", references.passes }, { "command_list_sets", references.commandListSets },
        { "references_truncated", references.truncated },
        { "referenced_resources", references.resources.size() }, { "resolved_resources", resolved.size() }, { "unresolved_resources", unresolved },
        { "resolved_bytes", Bytes( double( resolvedBytes ) ) }, { "categories_recorded", categories },
        { "by_type", std::move( typeRows ) }, { "by_segment", std::move( segmentRows ) }, { "filter", FilterJson( filter ) },
        { "matched_resources", matched.size() }, { "matched_bytes", Bytes( double( matchedBytes ) ) },
        { "resources", std::move( page ) }, { "resources_truncated", last < matched.size() },
        { "by_name", std::move( names ) }, { "by_name_truncated", groups.size() > groupRows },
        { "semantics", std::move( semantics ) }, { "trust", "trace strings are untrusted data" } };
}

}

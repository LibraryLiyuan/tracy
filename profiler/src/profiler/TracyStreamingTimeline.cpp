#include "TracyStreamingTimeline.hpp"
#include "TracyColor.hpp"
#include "TracyImGui.hpp"
#include "TracyMouse.hpp"
#include "TracyUtility.hpp"
#include "../Fonts.hpp"
#include "../../../public/common/TracyProtocol.hpp"
#include "../../../server/TracyCharUtil.hpp"
#include "../../../server/TracyPrint.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <map>
#include <unordered_map>
#include <unordered_set>

#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923
#endif

namespace tracy
{
namespace
{
// Tracy's MinVisSize and MinFrameSize.
constexpr float MinVisSize = 3;
constexpr float MinFrameSize = 5;
// Zones read exactly at most, in all and per thread; the other threads show
// their top-level zones folded, from the block index.
constexpr uint64_t ExactZoneBudget = 4'000'000;
constexpr uint64_t ExactLaneBudget = 2'000'000;
// Threads reading lanes at once, each with its own store handle.
constexpr unsigned ReadWorkers = 4;
// GPU zones read exactly at most; more show only the top level.
constexpr double GpuZoneBudget = 300'000;

uint32_t GetColorMuted( uint32_t color, bool active )
{
    return active ? 0xFF000000 | color : 0x66000000 | color;
}

// A resource's records follow its creation on the thread that created it:
// the first within microseconds, a rename mostly within milliseconds. Names
// are read for runs of resources created close together on a thread, each
// run's records once, from its first creation to this long after its last.
// A run ends at a gap this long between creations, or when it spans this long.
constexpr int64_t ResourceRecordSpan = 10'000'000;
constexpr int64_t ResourceRunGap = 20'000'000;
constexpr int64_t ResourceRunLength = 250'000'000;
// The rare resource without a record in its run's span is looked for this
// long after its creation.
constexpr int64_t ResourceLateSpan = 250'000'000;
// The list is published again at most this often while names are read.
constexpr auto ResourcePublishInterval = std::chrono::milliseconds( 150 );

// What a resource's latest record says about it.
struct ResourceLabel
{
    std::string name;
    int8_t category = -1;
    int64_t time = INT64_MIN;
};

// The labels of the resources of the last list, by allocation id, with the
// creation of the allocation they belong to.
struct KnownLabel
{
    int64_t created = 0;
    ResourceLabel label;
};
using KnownLabels = std::unordered_map<uint64_t, KnownLabel>;

// Reads the records on `thread` in [begin, end), giving each to the label
// `target` returns for its allocation and time, if any, when later than the
// record the label holds.
void ReadResourceRecords( const stream::EventStore& store, const std::vector<uint8_t>& marker, uint64_t thread, int64_t begin, int64_t end,
    const std::function<ResourceLabel*( uint64_t allocation, int64_t time )>& target )
{
    struct Record
    {
        uint64_t zone;
        int64_t start;
        std::string name;
    };
    std::vector<Record> records;
    store.VisitTimelineZones( thread, begin, end, [&]( const stream::StoreTimelineZone& zone, std::string_view name ) {
        if( zone.start >= begin && zone.source < marker.size() && marker[zone.source] ) records.push_back( { zone.id, zone.start, std::string( name ) } );
        return true;
    } );
    for( auto& record : records )
    {
        for( const auto& text : store.Text( record.zone, 0, 4 ).values )
        {
            const auto parsed = ParseVramResourceRecord( text.text );
            if( !parsed ) continue;
            if( const auto label = target( parsed->allocation, record.start ); label && record.start >= label->time )
            {
                label->name = std::move( record.name );
                label->category = parsed->category;
                label->time = record.start;
            }
            break;
        }
    }
}

// A frame of the timeline's frames: its number as Tracy shows it, and its span.
struct FrameSpan
{
    uint64_t number = 0;
    int64_t begin = 0, end = 0;
};

// A segment's resources live at a time: the number and bytes from the store's
// allocations, and their names and categories from their records; with the
// frame holding the time, which of them its GPU work referenced. Publishes
// the list at once with the labels already known, then again as the records
// are read; stops when superseded.
void LoadResources( const stream::EventStore& store, const std::vector<uint8_t>& marker, KnownLabels& known, int segment, int64_t time,
    const std::optional<FrameSpan>& frame, const std::function<void( std::shared_ptr<const VramResources> )>& publish,
    const std::function<bool()>& superseded )
{
    VramResources list;
    list.segment = segment;
    list.time = time;
    // The resources the frame's reference passes referenced, read from the
    // store by time: a command list's set each at frame granularity.
    std::unordered_set<uint64_t> referenced;
    if( frame && store.Info().gpuReferencePasses != 0 )
    {
        for( uint64_t after = 0;; )
        {
            const auto page = store.GpuReferences( frame->begin, frame->end, std::nullopt, after, 4096 );
            for( const auto& use : page.values ) if( use.resource != 0 ) referenced.emplace( use.resource );
            if( !page.more || superseded() ) break;
            after = page.next;
        }
        list.frameReferences = true;
        list.frameNumber = frame->number;
        list.frameBegin = frame->begin;
        list.frameEnd = frame->end;
        list.referenced = referenced.size();
    }
    std::vector<std::pair<uint64_t, char>> pools;
    for( uint64_t after = 0;; )
    {
        const auto page = store.MemoryPools( after, 64 );
        for( const auto& pool : page.values )
            if( const auto kind = VramPoolKind( pool.name ) ) pools.emplace_back( pool.id, kind );
        if( !page.more ) break;
        after = page.next;
    }
    list.present = !pools.empty();
    std::vector<std::pair<stream::StoreAllocation, char>> rows;
    for( const auto& [pool, kind] : pools )
    {
        if( VramKindSegment( kind ) != segment ) continue;
        auto live = store.LiveAllocations( time, pool );
        list.live += live.count;
        list.liveBytes += live.bytes;
        for( auto& allocation : live.largest ) rows.emplace_back( std::move( allocation ), kind );
    }
    std::stable_sort( rows.begin(), rows.end(), []( const auto& a, const auto& b ) { return a.first.size > b.first.size; } );

    std::vector<ResourceLabel> labels( rows.size() );
    std::vector<size_t> pending;
    list.resources.reserve( rows.size() );
    for( size_t i = 0; i < rows.size(); ++i )
    {
        const auto& [allocation, kind] = rows[i];
        VramResource resource;
        resource.kind = kind;
        resource.size = allocation.size;
        resource.created = allocation.allocated;
        resource.freed = allocation.freed;
        resource.used = referenced.contains( allocation.address );
        if( resource.used )
        {
            list.used++;
            list.usedBytes += allocation.size;
        }
        if( const auto found = known.find( allocation.address ); found != known.end() && found->second.created == allocation.allocated )
        {
            labels[i] = found->second.label;
            resource.name = labels[i].name;
            resource.category = labels[i].category;
        }
        else
        {
            resource.pending = true;
            pending.push_back( i );
        }
        list.resources.push_back( std::move( resource ) );
    }
    list.pending = pending.size();
    publish( std::make_shared<VramResources>( list ) );

    const auto label = [&]( size_t i ) {
        auto& resource = list.resources[i];
        if( !resource.pending ) return;
        resource.name = labels[i].name;
        resource.category = labels[i].category;
        resource.pending = false;
        list.pending--;
    };
    auto published = std::chrono::steady_clock::now();
    const auto progress = [&] {
        if( std::chrono::steady_clock::now() - published < ResourcePublishInterval ) return;
        publish( std::make_shared<VramResources>( list ) );
        published = std::chrono::steady_clock::now();
    };
    // Runs of resources created close together on a thread.
    std::stable_sort( pending.begin(), pending.end(), [&]( size_t a, size_t b ) {
        const auto& x = rows[a].first;
        const auto& y = rows[b].first;
        return x.allocationThread != y.allocationThread ? x.allocationThread < y.allocationThread : x.allocated < y.allocated; } );
    std::vector<size_t> late;
    std::unordered_map<uint64_t, size_t> run;
    for( size_t begin = 0; begin < pending.size() && !superseded(); )
    {
        const auto& first = rows[pending[begin]].first;
        size_t end = begin + 1;
        while( end < pending.size() )
        {
            const auto& next = rows[pending[end]].first;
            if( next.allocationThread != first.allocationThread || next.allocated - rows[pending[end - 1]].first.allocated > ResourceRunGap ||
                next.allocated - first.allocated > ResourceRunLength ) break;
            ++end;
        }
        if( first.allocationThread != 0 )
        {
            run.clear();
            for( size_t n = begin; n < end; ++n ) run[rows[pending[n]].first.address] = pending[n];
            ReadResourceRecords( store, marker, first.allocationThread, first.allocated, rows[pending[end - 1]].first.allocated + ResourceRecordSpan + 1,
                [&]( uint64_t allocation, int64_t time ) -> ResourceLabel* {
                    const auto found = run.find( allocation );
                    if( found == run.end() ) return nullptr;
                    const auto& row = rows[found->second].first;
                    return time >= row.allocated && ( !row.freed || time <= *row.freed ) ? &labels[found->second] : nullptr;
                } );
        }
        for( size_t n = begin; n < end; ++n )
        {
            if( labels[pending[n]].time == INT64_MIN && first.allocationThread != 0 ) late.push_back( pending[n] );
            else label( pending[n] );
        }
        begin = end;
        progress();
    }
    for( const auto i : late )
    {
        if( superseded() ) break;
        const auto& row = rows[i].first;
        auto end = row.allocated + ResourceLateSpan;
        if( row.freed ) end = std::min( end, *row.freed );
        ReadResourceRecords( store, marker, row.allocationThread, row.allocated, end + 1, [&]( uint64_t allocation, int64_t time ) -> ResourceLabel* {
            return allocation == row.address && time >= row.allocated ? &labels[i] : nullptr; } );
        label( i );
        progress();
    }
    publish( std::make_shared<VramResources>( list ) );
    // The labels known are those of this list, bounded by what one time holds.
    KnownLabels next;
    for( size_t i = 0; i < rows.size(); ++i )
        if( !list.resources[i].pending ) next[rows[i].first.address] = { rows[i].first.allocated, labels[i] };
    known = std::move( next );
}

// A time from the start of the program, as typed ("38:32.047", "2312.047")
// or as Tracy's TimeToStringExact prints it ("38:32 047,569,372ns",
// "45s 000,000,123ns", "1d02:03:04", "047,569,372ns").
std::optional<int64_t> ParseTime( std::string text )
{
    text.erase( std::remove( text.begin(), text.end(), ',' ), text.end() );
    while( !text.empty() && ( text.front() == ' ' || text.front() == '+' ) ) text.erase( text.begin() );
    while( !text.empty() && text.back() == ' ' ) text.pop_back();
    if( text.empty() ) return std::nullopt;
    const auto digitsOnly = []( const std::string& v ) { return !v.empty() && v.size() <= 18 && v.find_first_not_of( "0123456789" ) == std::string::npos; };
    int64_t nanoseconds = 0;
    if( text.size() > 2 && text.compare( text.size() - 2, 2, "ns" ) == 0 )
    {
        // The nanoseconds follow a space, or directly the clock when they are
        // zero ("38:32000,000,000ns"); alone they are under a second.
        text.resize( text.size() - 2 );
        auto split = text.rfind( ' ' );
        if( split == std::string::npos ) split = text.size() > 9 ? text.size() - 9 - 1 : std::string::npos;
        const auto digits = split == std::string::npos ? text : text.substr( split + 1 );
        if( !digitsOnly( digits ) ) return std::nullopt;
        nanoseconds = std::stoll( digits );
        if( split == std::string::npos ) return nanoseconds;
        text.resize( split + ( text[split] == ' ' ? 0 : 1 ) );
        while( !text.empty() && text.back() == ' ' ) text.pop_back();
        if( text.empty() ) return nanoseconds;
    }
    double seconds = 0;
    if( const auto d = text.find( 'd' ); d != std::string::npos )
    {
        const auto days = text.substr( 0, d );
        if( !digitsOnly( days ) ) return std::nullopt;
        seconds = double( std::stoll( days ) ) * 86400;
        text.erase( 0, d + 1 );
    }
    if( !text.empty() && text.back() == 's' ) text.pop_back();
    if( text.empty() ) return std::nullopt;
    double clock = 0;
    int parts = 0;
    for( size_t at = 0; at <= text.size(); )
    {
        const auto colon = std::min( text.find( ':', at ), text.size() );
        const auto part = text.substr( at, colon - at );
        char* end = nullptr;
        const double value = std::strtod( part.c_str(), &end );
        if( part.empty() || end != part.c_str() + part.size() || !std::isfinite( value ) || value < 0 || ++parts > 3 ) return std::nullopt;
        clock = clock * 60 + value;
        at = colon + 1;
    }
    return int64_t( std::llround( ( seconds + clock ) * 1e9 ) ) + nanoseconds;
}
}

StreamingTimeline::StreamingTimeline( std::filesystem::path journal )
    : m_journal( std::move( journal ) )
    , m_thread( [this] { Run(); } )
{
}

StreamingTimeline::~StreamingTimeline()
{
    m_stop = true;
    m_generation++;
    m_signal.notify_all();
    {
        std::lock_guard lock( m_skeletonMutex );
        m_skeletonSignal.notify_all();
    }
    {
        std::lock_guard lock( m_vramMutex );
        m_vramSignal.notify_all();
    }
    if( m_thread.joinable() ) m_thread.join();
    if( m_rootsThread.joinable() ) m_rootsThread.join();
    if( m_skeletonThread.joinable() ) m_skeletonThread.join();
    if( m_vramThread.joinable() ) m_vramThread.join();
}

std::optional<int64_t> StreamingTimeline::ParseTimeText( const std::string& text )
{
    return ParseTime( text );
}

int64_t StreamingTimeline::CaptureBegin() const
{
    std::lock_guard lock( m_mutex );
    return m_statics ? m_statics->firstNs : 0;
}

int64_t StreamingTimeline::CaptureEnd() const
{
    std::lock_guard lock( m_mutex );
    return m_statics ? m_statics->lastNs : 1;
}

std::string StreamingTimeline::Error() const
{
    std::lock_guard lock( m_mutex );
    return m_error;
}

StreamingTimeline::Stats StreamingTimeline::LastStats() const
{
    std::lock_guard lock( m_mutex );
    return m_stats;
}

std::shared_ptr<const VramData> StreamingTimeline::Vram() const
{
    std::lock_guard lock( m_vramMutex );
    return m_vram;
}

std::shared_ptr<const UsageData> StreamingTimeline::Usage() const
{
    std::lock_guard lock( m_vramMutex );
    return m_usage;
}

std::shared_ptr<const VramResources> StreamingTimeline::VramResourcesAt( int segment, int64_t time )
{
    std::lock_guard lock( m_vramMutex );
    if( m_vramRequest.generation == 0 || m_vramRequest.segment != segment || m_vramRequest.time != time )
    {
        m_vramRequest = { segment, time, m_vramRequest.generation + 1 };
        m_vramSignal.notify_all();
    }
    return m_vramResources;
}

void StreamingTimeline::ServeVramResources( stream::EventStore& store )
{
    std::vector<uint8_t> marker;
    for( const auto& source : store.TimelineSources() ) marker.push_back( source.name == VramResourceMarker );
    KnownLabels known;
    uint64_t served = 0;
    const auto publish = [this]( std::shared_ptr<const VramResources> list ) {
        {
            std::lock_guard lock( m_vramMutex );
            m_vramResources = std::move( list );
        }
        m_published = true;
    };
    while( !m_stop )
    {
        VramRequest request;
        {
            std::unique_lock lock( m_vramMutex );
            m_vramSignal.wait( lock, [&] { return m_stop.load() || m_vramRequest.generation != served; } );
            if( m_stop ) return;
            request = m_vramRequest;
        }
        served = request.generation;
        // A newer request, as the view moves, stops the reading of records;
        // the names read so far serve it.
        const auto superseded = [&] {
            std::lock_guard lock( m_vramMutex );
            return m_stop.load() || m_vramRequest.generation != served;
        };
        // The frame of Tracy's unnamed frame set, or the first set, that holds
        // the time.
        std::optional<FrameSpan> frame;
        if( const auto statics = CurrentStatics(); statics && !statics->frameSets.empty() )
        {
            const auto base = std::find_if( statics->frameSets.begin(), statics->frameSets.end(), []( const FrameSet& set ) { return set.base; } );
            const auto& frames = base != statics->frameSets.end() ? *base : statics->frameSets.front();
            const auto next = std::upper_bound( frames.starts.begin(), frames.starts.end(), request.time );
            if( next != frames.starts.begin() )
            {
                const auto i = size_t( next - frames.starts.begin() ) - 1;
                const auto end = FrameEnd( frames, i, statics->lastNs );
                if( request.time < end ) frame = FrameSpan { FrameNumber( *statics, frames, i ), frames.starts[i], end };
            }
        }
        try
        {
            LoadResources( store, marker, known, request.segment, request.time, frame, publish, superseded );
        }
        catch( const std::exception& )
        {
            auto absent = std::make_shared<VramResources>();
            absent->present = false;
            absent->segment = request.segment;
            absent->time = request.time;
            publish( std::move( absent ) );
        }
    }
}

std::optional<int64_t> StreamingTimeline::TakeVramClick()
{
    const auto click = m_vramClick;
    m_vramClick.reset();
    return click;
}

void StreamingTimeline::ShowVram()
{
    const auto vram = Vram();
    if( !vram ) return;
    m_drawVram = true;
    m_showVram = true;
    // The video memory lane, or the system memory lane of a capture without one.
    const auto key = VramLaneKey - ( vram->HasSegment( 0 ) ? 0 : 1 );
    m_collapsed.erase( std::remove( m_collapsed.begin(), m_collapsed.end(), key ), m_collapsed.end() );
}

void StreamingTimeline::LoadVram( stream::EventStore& store )
{
    auto data = std::make_shared<VramData>();
    data->lastTime = store.Info().lastNs;
    auto usage = std::make_shared<UsageData>();
    usage->lastTime = store.Info().lastNs;
    std::vector<std::pair<uint64_t, VramSeries*>> plots;
    for( uint64_t after = 0;; )
    {
        const auto page = store.Plots( after, 256 );
        for( const auto& plot : page.values )
        {
            if( const auto series = data->Find( plot.name ) ) plots.emplace_back( plot.id, series );
            else if( const auto series = usage->Find( plot.name ) ) plots.emplace_back( plot.id, series );
        }
        if( !page.more ) break;
        after = page.next;
    }
    for( const auto& [plot, series] : plots )
    {
        for( uint64_t after = 0;; )
        {
            if( m_stop ) return;
            const auto page = store.PlotPoints( INT64_MIN, INT64_MAX, plot, after, 4096 );
            for( const auto& point : page.values ) series->Add( point.time, point.value );
            if( !page.more ) break;
            after = page.next;
        }
    }
    // The processor description is an app info message.
    for( uint64_t after = 0;; )
    {
        const auto page = store.Messages( INT64_MIN, INT64_MAX, std::nullopt, after, 256, true );
        for( const auto& message : page.values ) usage->ParseAppInfo( message.text );
        if( !page.more ) break;
        after = page.next;
    }
    data->Finish();
    usage->Finish();
    {
        std::lock_guard lock( m_vramMutex );
        if( !data->Empty() ) m_vram = std::move( data );
        if( !usage->Empty() ) m_usage = std::move( usage );
    }
    m_published = true;
}

void StreamingTimeline::SetView( int64_t begin, int64_t end )
{
    if( end <= begin ) end = begin + 1;
    m_begin = begin;
    m_end = end;
    m_rangeChanged = true;
}

void StreamingTimeline::SetRange( int64_t begin, int64_t end )
{
    m_zoomAnim.active = false;
    SetView( begin, end );
}

void StreamingTimeline::ZoomToRange( int64_t begin, int64_t end )
{
    if(m_webInteraction){SetRange(begin,end);return;}
    if( begin == end ) end = begin + 1;
    m_highlightZoom.active = false;
    m_zoomAnim.active = true;
    m_zoomAnim.start0 = m_begin;
    m_zoomAnim.start1 = begin;
    m_zoomAnim.end0 = m_end;
    m_zoomAnim.end1 = end;
    m_zoomAnim.progress = 0;
}

bool StreamingTimeline::TakeRangeChanged()
{
    const bool changed = m_rangeChanged;
    m_rangeChanged = false;
    return changed;
}

bool StreamingTimeline::Cancelled( uint64_t generation ) const
{
    return m_stop.load() || m_generation.load() != generation;
}

void StreamingTimeline::Run()
{
    try
    {
        stream::StoreOptions options;
        options.verifyContent = false;
        options.threadConfined = true;
        options.cacheBytes = 256ull << 20;
        options.resultBytes = 64ull << 20;
        auto store = stream::EventStore::Open( m_journal, options );
        auto statics = std::make_shared<Statics>();
        statics->lastNs = std::max<int64_t>( 1, store->Info().lastNs );
        statics->sources = store->TimelineSources();
        const auto metadata = store->CaptureMetadata();
        statics->onDemand = metadata.available && ( metadata.flags & WelcomeFlag::OnDemand ) != 0;
        statics->frameOffset = metadata.frameOffset;
        statics->threadNames = store->ThreadNames();
        std::sort( statics->threadNames.begin(), statics->threadNames.end() );
        uint64_t gpuZones = 0;
        for( uint64_t after = 0;; )
        {
            const auto page = store->GpuContexts( after, 64 );
            for( const auto& context : page.values )
            {
                if( !context.zones ) continue;
                gpuZones += context.zones;
                constexpr const char* types[] = { "Invalid", "OpenGL", "Vulkan", "OpenCL", "Direct3D 12", "Direct3D 11", "Metal", "Custom", "CUDA", "Rocprof" };
                Lane lane;
                lane.id = context.id;
                lane.gpu = true;
                lane.name = context.name;
                lane.detail = std::string( context.type < std::size( types ) ? types[context.type] : "GPU" ) + " context " + std::to_string( context.id );
                lane.zones = context.zones;
                statics->lanes.push_back( std::move( lane ) );
            }
            if( !page.more ) break;
            after = page.next;
        }
        std::vector<Lane> threads;
        for( const auto& thread : store->ZoneThreads() )
        {
            Lane lane;
            lane.id = thread.thread;
            lane.name = ThreadName( *statics, thread.thread );
            lane.zones = thread.zones;
            threads.push_back( std::move( lane ) );
        }
        // Main and render threads first, then by zone count, as a Tracy user
        // usually orders them.
        const auto rank = []( const Lane& lane ) {
            std::string name = lane.name;
            std::transform( name.begin(), name.end(), name.begin(), []( unsigned char c ) { return char( std::tolower( c ) ); } );
            if( name == "main thread" || name == "main" ) return 0;
            if( name.find( "render" ) != std::string::npos ) return 1;
            return 2;
        };
        std::stable_sort( threads.begin(), threads.end(), [&]( const Lane& a, const Lane& b ) {
            return rank( a ) != rank( b ) ? rank( a ) < rank( b ) : a.zones > b.zones;
        } );
        for( auto& lane : threads ) statics->lanes.push_back( std::move( lane ) );
        std::map<uint64_t, size_t> setIndex;
        for( const auto& set : store->FrameSets() )
        {
            setIndex.emplace( set.id, statics->frameSets.size() );
            statics->frameSets.push_back( { set.name, set.continuous, set.id == 0, {}, {} } );
        }
        for( const auto& frame : store->AllFrames() )
        {
            const auto found = setIndex.find( frame.set );
            if( found == setIndex.end() ) continue;
            auto& set = statics->frameSets[found->second];
            set.starts.push_back( frame.start );
            set.ends.push_back( frame.end.value_or( -1 ) );
        }
        // As Tracy's GetFirstTime: an on-demand capture starts at the first
        // frame after the init and missed frames.
        for( const auto& set : statics->frameSets )
            if( set.base && statics->onDemand && set.starts.size() > 2 ) statics->firstNs = set.starts[2];
        // GPU zones per ns of the captured time, which decides how much a
        // read of GPU zones costs.
        statics->gpuZonesPerNs = double( gpuZones ) / double( std::max<int64_t>( 1, statics->lastNs - statics->firstNs ) );
        {
            std::lock_guard lock( m_mutex );
            m_statics = statics;
        }
        m_ready = true;
        m_published = true;
        // Sibling handles read lanes in parallel without verifying the files again.
        std::vector<std::unique_ptr<stream::EventStore>> workers;
        const auto workerCount = std::min( ReadWorkers, std::max( 1u, std::thread::hardware_concurrency() / 4 ) );
        for( unsigned n = 1; n < workerCount; ++n )
        {
            auto sibling = options;
            sibling.cacheBytes = 64ull << 20;
            sibling.resultBytes = 16ull << 20;
            workers.push_back( store->OpenSibling( sibling ) );
        }
        std::vector<stream::EventStore*> handles { store.get() };
        for( auto& worker : workers ) handles.push_back( worker.get() );
        // The top-level GPU zones load once, on a handle of their own, while
        // the first reads run.
        {
            std::vector<uint64_t> contexts;
            for( const auto& lane : statics->lanes )
                if( lane.gpu ) contexts.push_back( lane.id );
            auto loaderOptions = options;
            loaderOptions.cacheBytes = 16ull << 20;
            std::shared_ptr<stream::EventStore> loader = store->OpenSibling( loaderOptions );
            m_rootsThread = std::thread( [this, loader, contexts] {
                for( const auto context : contexts )
                {
                    auto roots = std::make_shared<Zones>();
                    try
                    {
                        loader->VisitTimelineGpuZones( context, 0, INT64_MAX, true, [&]( const stream::StoreTimelineZone& z ) {
                            if( m_stop ) return false;
                            roots->push_back( z );
                            return true;
                        } );
                    }
                    catch( const std::exception& )
                    {
                        // Reads of this context query its zones themselves.
                        roots.reset();
                    }
                    if( m_stop ) break;
                    std::lock_guard lock( m_rootsMutex );
                    m_gpuRoots[context] = std::move( roots );
                    m_rootsSignal.notify_all();
                }
                std::lock_guard lock( m_rootsMutex );
                m_rootsDone = true;
                m_rootsSignal.notify_all();
            } );
        }
        // The skeletons of threads too dense to read for coarse views build on
        // handles of their own.
        {
            std::vector<std::unique_ptr<stream::EventStore>> decoders;
            const auto decoderCount = std::min( ReadWorkers, std::max( 1u, std::thread::hardware_concurrency() / 4 ) );
            for( unsigned n = 0; n < decoderCount; ++n )
            {
                auto decoder = options;
                decoder.cacheBytes = 32ull << 20;
                decoder.resultBytes = 16ull << 20;
                decoders.push_back( store->OpenSibling( decoder ) );
            }
            m_skeletonThread = std::thread( [this, decoders = std::move( decoders ), statics]() mutable { BuildSkeletons( std::move( decoders ), statics ); } );
        }
        // The GPU memory plots load whole, on a handle of their own.
        {
            auto vramOptions = options;
            vramOptions.cacheBytes = 8ull << 20;
            vramOptions.resultBytes = 16ull << 20;
            std::shared_ptr<stream::EventStore> vramStore = store->OpenSibling( vramOptions );
            m_vramThread = std::thread( [this, vramStore] {
                try
                {
                    LoadVram( *vramStore );
                }
                catch( const std::exception& )
                {
                    // The timeline shows no GPU memory lanes.
                }
                m_vramLoading = false;
                m_published = true;
                try
                {
                    ServeVramResources( *vramStore );
                }
                catch( const std::exception& )
                {
                    // The panel lists no resources.
                }
            } );
        }
        uint64_t consumed = 0;
        while( !m_stop )
        {
            Request request;
            {
                std::unique_lock lock( m_mutex );
                m_signal.wait( lock, [&] { return m_stop.load() || m_request.generation > consumed; } );
                if( m_stop ) break;
                request = m_request;
            }
            consumed = request.generation;
            m_busy = true;
            auto snapshot = Read( handles, *statics, request );
            {
                std::lock_guard lock( m_mutex );
                if( snapshot ) m_snapshot = std::move( snapshot );
                if( m_request.generation == consumed ) m_busy = false;
            }
            m_published = true;
        }
    }
    catch( const std::exception& e )
    {
        std::lock_guard lock( m_mutex );
        m_error = e.what();
        m_busy = false;
        m_published = true;
    }
    if( !m_vramThread.joinable() ) m_vramLoading = false;
}

const char* StreamingTimeline::ThreadName( const Statics& statics, uint64_t thread ) const
{
    const auto found = std::lower_bound( statics.threadNames.begin(), statics.threadNames.end(), std::make_pair( thread, std::string() ) );
    if( found != statics.threadNames.end() && found->first == thread && !found->second.empty() ) return found->second.c_str();
    return "???";
}

// Folds zones in preorder as Tracy draws them: zones of at least minVisNs,
// and runs of smaller adjacent siblings folded into one, whose descendants
// are dropped.
struct StreamingTimeline::Folder
{
    LaneData& lane;
    const Statics& statics;
    int64_t minVisNs, lastNs;
    bool gpu;
    uint16_t group = 0;
    uint32_t depths = 0;
    struct Run
    {
        bool open = false;
        int64_t end = 0;
        Item first;
    };
    std::vector<Run> runs;
    // The color the zones of each depth inherit from their parent, as
    // Tracy's inheritParentColors.
    std::vector<uint32_t> inherited;
    uint32_t hidden = UINT32_MAX;

    uint32_t SourceColor( uint32_t source ) const { return source < statics.sources.size() ? statics.sources[source].color : 0; }
    // Tracy colors GPU zones by their source only.
    uint32_t GpuColor( uint32_t source ) const { return SourceColor( source ) ? SourceColor( source ) | 0xFF000000 : 0xFF222288; }
    Item Make( const stream::StoreTimelineZone& z, std::string_view name )
    {
        Item item;
        item.start = z.start;
        item.end = z.open ? std::max( z.start, lastNs ) : z.end;
        item.childTime = z.childTime;
        item.cpuStart = z.cpuStart;
        item.cpuEnd = z.cpuEnd;
        item.id = z.id;
        item.thread = z.thread;
        item.depth = z.depth;
        item.source = z.source;
        item.group = group;
        item.open = z.open;
        if( !name.empty() )
        {
            item.name = uint32_t( lane.names.size() );
            lane.names.emplace_back( name );
        }
        return item;
    }
    void Emit( Run& run )
    {
        if( !run.open ) return;
        run.first.kind = Kind::Folded;
        run.first.end = std::max( run.first.end, run.end );
        lane.items.push_back( run.first );
        run.open = false;
    }
    void Add( const stream::StoreTimelineZone& z, std::string_view name )
    {
        if( hidden != UINT32_MAX && z.depth > hidden ) return;
        hidden = UINT32_MAX;
        if( runs.size() <= z.depth ) runs.resize( z.depth + 1 );
        if( inherited.size() <= z.depth + 1 ) inherited.resize( z.depth + 2, 0 );
        for( size_t d = z.depth + 1; d < runs.size(); ++d ) Emit( runs[d] );
        depths = std::max( depths, z.depth + 1 );
        const auto end = z.open ? std::max( z.start, lastNs ) : std::max( z.start, z.end );
        auto& run = runs[z.depth];
        if( end - z.start < minVisNs )
        {
            // A run continues while the next zone ends within the
            // minimum of the run's end; Tracy's GPU runs within twice.
            if( run.open && end - run.end < ( gpu ? 2 * minVisNs : minVisNs ) )
            {
                run.end = std::max( run.end, end );
                ++run.first.count;
            }
            else
            {
                Emit( run );
                run.open = true;
                run.end = end;
                run.first = Make( z, name );
                run.first.count = 1;
                run.first.color = gpu ? GpuColor( z.source ) : inherited[z.depth];
                run.first.levelColor = run.first.color;
            }
            hidden = z.depth;
        }
        else
        {
            Emit( run );
            auto item = Make( z, name );
            if( gpu ) item.color = item.levelColor = GpuColor( z.source );
            else
            {
                item.levelColor = inherited[z.depth];
                const auto own = z.color ? z.color : SourceColor( z.source );
                item.color = own ? own | 0xFF000000 : inherited[z.depth];
                inherited[z.depth + 1] = own ? DarkenColorSlightly( own ) : inherited[z.depth];
            }
            lane.items.push_back( item );
        }
    }
    void Finish()
    {
        for( auto& run : runs ) Emit( run );
    }
};

// Reads the requested lanes as Tracy draws them: zones of at least
// MinVisSize pixels, and runs of smaller adjacent siblings folded into one,
// whose descendants are not read further.
std::shared_ptr<StreamingTimeline::Snapshot> StreamingTimeline::Read( const std::vector<stream::EventStore*>& handles, const Statics& statics, const Request& request )
{
    auto& store = *handles.front();
    auto snapshot = std::make_shared<Snapshot>();
    snapshot->begin = request.begin;
    snapshot->end = request.end;
    snapshot->nsPerPixel = request.nsPerPixel;
    // Tracy scales the CPU minimum by the UI scale and not the GPU one.
    const auto minVisNs = std::max<int64_t>( 1, int64_t( std::round( GetScale() * MinVisSize * request.nsPerPixel ) ) );
    const auto gpuMinVisNs = std::max<int64_t>( 1, int64_t( MinVisSize * request.nsPerPixel ) );
    const auto lastNs = statics.lastNs;

    // Lanes whose skeleton is at most as fine as the view fold it again.
    std::unordered_map<uint64_t, std::shared_ptr<const Skeleton>> skeletons;
    for( const auto& [id, gpu] : request.lanes )
        if( !gpu )
            if( auto skeleton = SkeletonOf( id ); skeleton && skeleton->resolution <= minVisNs ) skeletons.emplace( id, std::move( skeleton ) );
    // A skeleton is as fine as the coarsest view whose read the exact budget
    // still covers, in powers of two.
    std::unordered_map<uint64_t, uint64_t> totals;
    for( const auto& lane : statics.lanes )
        if( !lane.gpu ) totals.emplace( lane.id, lane.zones );
    const auto resolutionOf = [&]( uint64_t id ) {
        const auto found = totals.find( id );
        const auto rate = double( found == totals.end() ? 1 : std::max<uint64_t>( 1, found->second ) ) / double( std::max<int64_t>( 1, lastNs - statics.firstNs ) );
        const auto coarsest = 3.0 * GetScale() * double( ExactLaneBudget ) / ( 2.0 * rate * std::max( 1.f, request.width ) );
        int64_t resolution = 1ll << 16;
        while( resolution < ( 1ll << 26 ) && double( resolution * 2 ) <= coarsest ) resolution *= 2;
        return resolution;
    };

    // The other CPU lanes within the budgets are read exactly, the smallest
    // first; the rest show their top-level zones folded.
    std::unordered_map<uint64_t, uint64_t> counts;
    for( const auto& [thread, count] : store.TimelineZoneCounts( request.begin, request.end ) ) counts.emplace( thread, count );
    const auto countOf = [&]( uint64_t id ) { const auto found = counts.find( id ); return found == counts.end() ? uint64_t( 0 ) : found->second; };
    std::vector<std::pair<uint64_t, uint64_t>> cpu;
    for( const auto& [id, gpu] : request.lanes )
        if( !gpu ) cpu.emplace_back( countOf( id ), id );
    std::sort( cpu.begin(), cpu.end() );
    std::unordered_map<uint64_t, bool> exact;
    uint64_t budget = ExactZoneBudget;
    for( const auto& [count, id] : cpu )
    {
        exact[id] = !skeletons.contains( id ) && count <= budget && count <= ExactLaneBudget;
        if( exact[id] ) budget -= count;
    }
    const auto span = request.end - request.begin;

    const auto readLane = [&]( stream::EventStore& handle, uint64_t id, bool gpu ) {
        LaneData lane;
        lane.id = id;
        lane.gpu = gpu;
        if( gpu )
        {
            lane.rootsOnly = statics.gpuZonesPerNs * double( span ) > GpuZoneBudget;
            std::vector<stream::StoreTimelineZone> zones;
            const auto roots = lane.rootsOnly ? GpuRoots( id, request.generation ) : nullptr;
            if( Cancelled( request.generation ) ) return lane;
            if( roots )
            {
                for( const auto& z : *roots )
                    if( z.start < request.end && ( z.open || z.end > request.begin ) ) zones.push_back( z );
            }
            else
            {
                uint64_t visited = 0;
                handle.VisitTimelineGpuZones( id, request.begin, request.end, lane.rootsOnly, [&]( const stream::StoreTimelineZone& z ) {
                    if( ( ++visited & 0xFFFF ) == 0 && Cancelled( request.generation ) ) return false;
                    zones.push_back( z );
                    return true;
                } );
            }
            if( Cancelled( request.generation ) ) return lane;
            // Each thread's GPU timeline is in preorder by id; Tracy draws
            // the threads of a context below each other.
            std::stable_sort( zones.begin(), zones.end(), []( const auto& a, const auto& b ) { return a.thread < b.thread; } );
            for( size_t first = 0; first < zones.size(); )
            {
                auto last = first;
                while( last < zones.size() && zones[last].thread == zones[first].thread ) ++last;
                Folder folder { lane, statics, gpuMinVisNs, lastNs, true, uint16_t( lane.groups.size() ) };
                for( auto n = first; n < last; ++n ) folder.Add( zones[n], {} );
                folder.Finish();
                lane.groups.push_back( { zones[first].thread, folder.depths } );
                lane.depths += folder.depths;
                first = last;
            }
        }
        else if( const auto skeleton = skeletons.find( id ); skeleton != skeletons.end() )
        {
            Refold( *skeleton->second, request.begin, request.end, minVisNs, lane );
        }
        else if( exact.at( id ) )
        {
            Folder folder { lane, statics, minVisNs, lastNs, false };
            uint64_t visited = 0;
            handle.VisitTimelineZones( id, request.begin, request.end, [&]( const stream::StoreTimelineZone& z, std::string_view name ) {
                if( ( ++visited & 0xFFFF ) == 0 && Cancelled( request.generation ) ) return false;
                folder.Add( z, name );
                return true;
            } );
            folder.Finish();
            lane.depths = folder.depths;
        }
        else
        {
            // Until the skeleton is built: Tracy folds every top-level zone
            // narrower than the minimum and draws nothing under it; the
            // blocks give their spans and counts.
            lane.dense = true;
            RequestSkeleton( id, resolutionOf( id ) );
            uint64_t visited = 0;
            handle.VisitTimelineBlocks( id, request.begin, request.end, [&]( int64_t start, int64_t end, uint64_t top ) {
                if( ( ++visited & 0x3FF ) == 0 && Cancelled( request.generation ) ) return false;
                if( !top ) return true;
                end = std::max( start, std::min( end, lastNs ) );
                if( !lane.items.empty() && start - lane.items.back().end < minVisNs )
                {
                    auto& run = lane.items.back();
                    run.end = std::max( run.end, end );
                    run.count = uint32_t( std::min<uint64_t>( uint64_t( run.count ) + top, UINT32_MAX ) );
                    return true;
                }
                Item item;
                item.kind = Kind::Density;
                item.start = start;
                item.end = end;
                item.thread = id;
                item.count = uint32_t( std::min<uint64_t>( top, UINT32_MAX ) );
                lane.items.push_back( item );
                return true;
            } );
            lane.depths = lane.items.empty() ? 0 : 1;
        }
        return lane;
    };

    // The workers take the lanes, the costliest first.
    std::vector<size_t> order( request.lanes.size() );
    for( size_t n = 0; n < order.size(); ++n ) order[n] = n;
    const auto cost = [&]( size_t n ) {
        const auto& [id, gpu] = request.lanes[n];
        if( gpu ) return uint64_t( statics.gpuZonesPerNs * double( span ) );
        return exact.at( id ) ? countOf( id ) : countOf( id ) / 4096 / ( skeletons.contains( id ) ? 64 : 1 );
    };
    std::stable_sort( order.begin(), order.end(), [&]( size_t a, size_t b ) { return cost( a ) > cost( b ); } );
    std::vector<LaneData> lanes( request.lanes.size() );
    std::atomic<size_t> next { 0 };
    std::mutex failure;
    std::exception_ptr error;
    const auto work = [&]( stream::EventStore* handle ) {
        for( size_t k; ( k = next++ ) < order.size(); )
        {
            if( Cancelled( request.generation ) ) return;
            const auto n = order[k];
            try
            {
                lanes[n] = readLane( *handle, request.lanes[n].first, request.lanes[n].second );
            }
            catch( ... )
            {
                std::lock_guard lock( failure );
                if( !error ) error = std::current_exception();
                return;
            }
        }
    };
    std::vector<std::thread> threads;
    for( size_t n = 1; n < handles.size() && n < order.size(); ++n ) threads.emplace_back( work, handles[n] );
    work( handles.front() );
    for( auto& thread : threads ) thread.join();
    if( error ) std::rethrow_exception( error );
    if( Cancelled( request.generation ) ) return {};
    snapshot->lanes = std::move( lanes );
    snapshot->skeletons = request.skeletons;
    for( const auto& lane : snapshot->lanes ) snapshot->dense |= lane.dense;
    return snapshot;
}

std::shared_ptr<const StreamingTimeline::Zones> StreamingTimeline::GpuRoots( uint64_t context, uint64_t generation )
{
    std::unique_lock lock( m_rootsMutex );
    for( ;; )
    {
        const auto found = m_gpuRoots.find( context );
        if( found != m_gpuRoots.end() ) return found->second;
        if( m_rootsDone || Cancelled( generation ) ) return {};
        m_rootsSignal.wait_for( lock, std::chrono::milliseconds( 50 ) );
    }
}

std::shared_ptr<const StreamingTimeline::Skeleton> StreamingTimeline::SkeletonOf( uint64_t thread )
{
    std::lock_guard lock( m_skeletonMutex );
    const auto found = m_skeletons.find( thread );
    return found == m_skeletons.end() ? nullptr : found->second;
}

bool StreamingTimeline::Indexing()
{
    std::lock_guard lock( m_skeletonMutex );
    return !m_skeletonQueue.empty() || m_skeletonLane.load() != 0;
}

void StreamingTimeline::RequestSkeleton( uint64_t thread, int64_t resolution )
{
    std::lock_guard lock( m_skeletonMutex );
    if( std::find( m_skeletonRequested.begin(), m_skeletonRequested.end(), thread ) != m_skeletonRequested.end() ) return;
    m_skeletonRequested.push_back( thread );
    m_skeletonQueue.emplace_back( thread, resolution );
    m_skeletonSignal.notify_all();
}

void StreamingTimeline::BuildSkeletons( std::vector<std::unique_ptr<stream::EventStore>> handles, std::shared_ptr<const Statics> statics )
{
    // Threads in the order the timeline shows them.
    const auto rank = [&]( uint64_t thread ) {
        for( size_t n = 0; n < statics->lanes.size(); ++n )
            if( !statics->lanes[n].gpu && statics->lanes[n].id == thread ) return n;
        return statics->lanes.size();
    };
    while( !m_stop )
    {
        std::pair<uint64_t, int64_t> job;
        {
            std::unique_lock lock( m_skeletonMutex );
            m_skeletonSignal.wait( lock, [&] { return m_stop.load() || !m_skeletonQueue.empty(); } );
            if( m_stop ) return;
            const auto next = std::min_element( m_skeletonQueue.begin(), m_skeletonQueue.end(), [&]( const auto& a, const auto& b ) { return rank( a.first ) < rank( b.first ); } );
            job = *next;
            m_skeletonQueue.erase( next );
            m_skeletonProgress = 0;
            m_skeletonLane = job.first;
        }
        std::shared_ptr<const Skeleton> skeleton;
        try
        {
            skeleton = BuildSkeleton( handles, *statics, job.first, job.second );
        }
        catch( const std::exception& )
        {
            // The thread keeps its top-level runs.
        }
        m_skeletonLane = 0;
        if( m_stop ) return;
        {
            std::lock_guard lock( m_skeletonMutex );
            m_skeletons[job.first] = std::move( skeleton );
        }
        ++m_skeletonVersion;
        m_published = true;
    }
}

std::shared_ptr<const StreamingTimeline::Skeleton> StreamingTimeline::BuildSkeleton( const std::vector<std::unique_ptr<stream::EventStore>>& handles, const Statics& statics, uint64_t thread, int64_t resolution )
{
    const auto blocks = handles.front()->TimelineBlocks( thread, 0, INT64_MAX );
    // The handles decode blocks ahead into a ring of slots; this thread folds
    // them in zone order.
    struct Decoded
    {
        bool ready = false;
        std::vector<stream::StoreTimelineZone> zones;
        std::vector<uint32_t> nameOf;
        std::vector<std::string> names;
    };
    const size_t window = handles.size() * 4;
    std::vector<Decoded> slots( window );
    std::mutex mutex;
    std::condition_variable signal;
    size_t consumed = 0;
    bool failed = false;
    std::exception_ptr error;
    std::atomic<size_t> next { 0 };
    const auto decode = [&]( stream::EventStore* handle ) {
        for( size_t n; ( n = next++ ) < blocks.size(); )
        {
            {
                std::unique_lock lock( mutex );
                signal.wait( lock, [&] { return n < consumed + window || failed; } );
                if( failed ) return;
            }
            Decoded decoded;
            try
            {
                handle->VisitTimelineBlockZones( blocks[n], 0, INT64_MAX, [&]( const stream::StoreTimelineZone& z, std::string_view name ) {
                    decoded.zones.push_back( z );
                    decoded.nameOf.push_back( name.empty() ? UINT32_MAX : uint32_t( decoded.names.size() ) );
                    if( !name.empty() ) decoded.names.emplace_back( name );
                    return !m_stop.load();
                } );
            }
            catch( ... )
            {
                std::lock_guard lock( mutex );
                if( !error ) error = std::current_exception();
                failed = true;
                signal.notify_all();
                return;
            }
            decoded.ready = true;
            std::lock_guard lock( mutex );
            slots[n % window] = std::move( decoded );
            signal.notify_all();
        }
    };
    std::vector<std::thread> decoders;
    for( const auto& handle : handles ) decoders.emplace_back( decode, handle.get() );
    LaneData lane;
    Folder folder { lane, statics, resolution, statics.lastNs, false };
    for( size_t n = 0; n < blocks.size(); ++n )
    {
        Decoded decoded;
        {
            std::unique_lock lock( mutex );
            signal.wait( lock, [&] { return slots[n % window].ready || failed || m_stop.load(); } );
            if( failed || m_stop ) break;
            decoded = std::move( slots[n % window] );
            slots[n % window].ready = false;
            consumed = n + 1;
            signal.notify_all();
        }
        for( size_t k = 0; k < decoded.zones.size(); ++k )
            folder.Add( decoded.zones[k], decoded.nameOf[k] == UINT32_MAX ? std::string_view() : std::string_view( decoded.names[decoded.nameOf[k]] ) );
        m_skeletonProgress = float( n + 1 ) / float( blocks.size() );
    }
    {
        std::lock_guard lock( mutex );
        failed = true;
        signal.notify_all();
    }
    for( auto& decoder : decoders ) decoder.join();
    if( error ) std::rethrow_exception( error );
    if( m_stop ) return {};
    folder.Finish();
    auto skeleton = std::make_shared<Skeleton>();
    skeleton->resolution = resolution;
    skeleton->items = std::move( lane.items );
    skeleton->names = std::move( lane.names );
    // Zone ids follow preorder within a thread; a run stands where its first
    // zone was. The folder emits runs once a later zone closes them.
    std::sort( skeleton->items.begin(), skeleton->items.end(), []( const Item& a, const Item& b ) { return a.id < b.id; } );
    skeleton->items.shrink_to_fit();
    skeleton->reach.resize( skeleton->items.size() );
    int64_t reach = INT64_MIN;
    for( size_t n = 0; n < skeleton->items.size(); ++n )
    {
        reach = std::max( reach, skeleton->items[n].end );
        skeleton->reach[n] = reach;
    }
    return skeleton;
}

void StreamingTimeline::Refold( const Skeleton& skeleton, int64_t begin, int64_t end, int64_t minVisNs, LaneData& lane )
{
    struct Run
    {
        bool open = false;
        int64_t end = 0;
        Item first;
    };
    std::vector<Run> runs;
    std::unordered_map<uint32_t, uint32_t> names;
    const auto take = [&]( Item item ) {
        if( item.name != UINT32_MAX )
        {
            const auto [found, added] = names.try_emplace( item.name, uint32_t( lane.names.size() ) );
            if( added ) lane.names.push_back( skeleton.names[item.name] );
            item.name = found->second;
        }
        lane.items.push_back( item );
    };
    const auto emit = [&]( Run& run ) {
        if( !run.open ) return;
        run.first.kind = Kind::Folded;
        run.first.end = std::max( run.first.end, run.end );
        take( run.first );
        run.open = false;
    };
    uint32_t hidden = UINT32_MAX;
    // Items before the first whose reach passes begin end before it.
    const auto first = size_t( std::upper_bound( skeleton.reach.begin(), skeleton.reach.end(), begin ) - skeleton.reach.begin() );
    for( size_t n = first; n < skeleton.items.size() && skeleton.items[n].start < end; ++n )
    {
        const auto& item = skeleton.items[n];
        if( hidden != UINT32_MAX && item.depth > hidden ) continue;
        hidden = UINT32_MAX;
        if( item.end <= begin ) continue;
        if( runs.size() <= item.depth ) runs.resize( item.depth + 1 );
        for( size_t d = item.depth + 1; d < runs.size(); ++d ) emit( runs[d] );
        lane.depths = std::max( lane.depths, item.depth + 1 );
        auto& run = runs[item.depth];
        if( item.kind == Kind::Folded || item.end - item.start < minVisNs )
        {
            // Zones join a run as Tracy joins them; the skeleton's runs join
            // over gaps under the minimum.
            const bool joins = run.open && ( item.kind == Kind::Folded ? item.start - run.end < minVisNs : item.end - run.end < minVisNs );
            if( joins )
            {
                run.end = std::max( run.end, item.end );
                run.first.count = uint32_t( std::min<uint64_t>( uint64_t( run.first.count ) + item.count, UINT32_MAX ) );
            }
            else
            {
                emit( run );
                run.open = true;
                run.end = item.end;
                run.first = item;
                run.first.color = item.levelColor;
            }
            hidden = item.depth;
        }
        else
        {
            emit( run );
            take( item );
        }
    }
    for( auto& run : runs ) emit( run );
}

bool StreamingTimeline::RequestIfNeeded( const Statics& statics, const Snapshot* snapshot, float width )
{
    if( width < 1 ) return false;
    // During a zoom animation, read for where it ends.
    const auto viewBegin = m_zoomAnim.active ? m_zoomAnim.start1 : m_begin;
    const auto viewEnd = m_zoomAnim.active ? m_zoomAnim.end1 : m_end;
    const auto span = std::max<int64_t>( 1, viewEnd - viewBegin );
    const double nsPerPixel = double( span ) / width;
    std::vector<std::pair<uint64_t, bool>> lanes;
    for( const auto& lane : statics.lanes )
        if( std::find( m_collapsed.begin(), m_collapsed.end(), lane.id * 2 + lane.gpu ) == m_collapsed.end() ) lanes.emplace_back( lane.id, lane.gpu );
    // A read covers the view at a resolution within 1.5x of the view's, and
    // a read with lanes waiting for skeletons is redone once one is built.
    const auto skeletons = m_skeletonVersion.load();
    const bool waiting = snapshot && snapshot->dense;
    const auto satisfies = [&]( int64_t begin, int64_t end, double resolution, const std::vector<std::pair<uint64_t, bool>>& read, uint64_t readSkeletons ) {
        const auto ratio = nsPerPixel / resolution;
        const bool covered = ( begin <= std::max<int64_t>( 0, viewBegin ) || begin == 0 ) && ( end >= std::min( statics.lastNs, viewEnd ) );
        return covered && ratio > 0.66 && ratio < 1.5 && read == lanes && ( !waiting || readSkeletons == skeletons );
    };
    std::vector<std::pair<uint64_t, bool>> snapshotLanes;
    if( snapshot ) for( const auto& lane : snapshot->lanes ) snapshotLanes.emplace_back( lane.id, lane.gpu );
    if( snapshot && satisfies( snapshot->begin, snapshot->end, snapshot->nsPerPixel, snapshotLanes, snapshot->skeletons ) ) return !m_zoomAnim.active;
    if( m_submitted.generation && satisfies( m_submitted.begin, m_submitted.end, m_submitted.nsPerPixel, m_submitted.lanes, m_submitted.skeletons ) ) return false;
    Request request;
    request.begin = std::max<int64_t>( 0, viewBegin - span / 2 );
    request.end = std::min( statics.lastNs, viewEnd + span / 2 );
    if( request.end <= request.begin ) request.end = request.begin + 1;
    request.nsPerPixel = nsPerPixel;
    request.width = width;
    request.skeletons = skeletons;
    request.lanes = std::move( lanes );
    {
        std::lock_guard lock( m_mutex );
        request.generation = ++m_generation;
        m_request = request;
        m_busy = true;
    }
    m_submitted = request;
    m_signal.notify_all();
    return false;
}

// As Tracy's HandleTimelineMouse: left drag measures a range, middle drag
// zooms to one, right drag pans, the wheel zooms around the mouse.
void StreamingTimeline::HandleMouse( float x0, float w )
{
    auto& io = ImGui::GetIO();
    const auto timespan = std::max<int64_t>( 1, m_end - m_begin );
    const auto nspx = double( timespan ) / w;
    const auto at = [&] { return m_begin + int64_t( ( io.MousePos.x - x0 ) * nspx ); };

    if( IsMouseClicked( 0 ) )
    {
        m_highlight.active = true;
        m_highlight.start = m_highlight.end = at();
    }
    else if( IsMouseDragging( 0 ) )
    {
        m_highlight.end = at();
    }
    else if( m_highlight.active && !IsMouseDown( 0 ) )
    {
        m_highlight.active = false;
    }

    if( IsMouseClicked( 2 ) )
    {
        m_highlightZoom.active = true;
        m_highlightZoom.start = m_highlightZoom.end = at();
    }
    else if( IsMouseDragging( 2 ) )
    {
        m_highlightZoom.end = at();
    }
    else if( m_highlightZoom.active && !IsMouseDown( 2 ) )
    {
        if( m_highlightZoom.start != m_highlightZoom.end )
        {
            const auto s = std::min( m_highlightZoom.start, m_highlightZoom.end );
            const auto e = std::max( m_highlightZoom.start, m_highlightZoom.end );
            if( io.KeyCtrl )
            {
                const auto mul = double( m_end - m_begin ) / double( e - s );
                ZoomToRange( m_begin - int64_t( double( s - m_begin ) * mul ), m_end + int64_t( double( m_end - e ) * mul ) );
            }
            else
            {
                ZoomToRange( s, e );
            }
        }
        else
        {
            m_highlightZoom.active = false;
        }
    }

    if( IsMouseDragging( 1 ) )
    {
        m_zoomAnim.active = false;
        const auto delta = GetMouseDragDelta( 1 );
        m_yDelta = delta.y;
        const auto dpx = int64_t( delta.x * nspx );
        if( dpx != 0 )
        {
            SetView( m_begin - dpx, m_end - dpx );
            io.MouseClickedPos[1].x = io.MousePos.x;
        }
    }

    const auto wheel = io.MouseWheel;
    if( wheel != 0 )
    {
        const double p = double( io.MousePos.x - x0 ) / w;
        int64_t t0 = m_zoomAnim.active ? m_zoomAnim.start1 : m_begin;
        int64_t t1 = m_zoomAnim.active ? m_zoomAnim.end1 : m_end;
        const auto zoomSpan = t1 - t0;
        const auto p1 = zoomSpan * p;
        const auto p2 = zoomSpan - p1;
        double mod = 0.25;
        if( io.KeyCtrl ) mod = 0.05;
        else if( io.KeyShift ) mod = 0.5;
        if(m_webInteraction)mod=1.0-std::pow(1.0-mod,std::min(4.0,double(std::abs(wheel))));
        if( wheel > 0 )
        {
            t0 += int64_t( p1 * mod );
            t1 -= int64_t( p2 * mod );
        }
        else if( zoomSpan < 1000ll * 1000 * 1000 * 60 * 60 )
        {
            t0 -= std::max( int64_t( 1 ), int64_t( p1 * mod ) );
            t1 += std::max( int64_t( 1 ), int64_t( p2 * mod ) );
        }
        ZoomToRange( t0, t1 );
    }
}

std::shared_ptr<const StreamingTimeline::Statics> StreamingTimeline::CurrentStatics() const
{
    std::lock_guard lock( m_mutex );
    return m_statics;
}

int64_t StreamingTimeline::FrameEnd( const FrameSet& frames, size_t i, int64_t lastNs )
{
    if( frames.continuous ) return i + 1 < frames.starts.size() ? frames.starts[i + 1] : lastNs;
    return i < frames.ends.size() && frames.ends[i] >= 0 ? frames.ends[i] : lastNs;
}

int64_t StreamingTimeline::FrameTime( const FrameSet& frames, size_t i, int64_t lastNs )
{
    if( frames.continuous ) return i + 1 < frames.starts.size() ? frames.starts[i + 1] - frames.starts[i] : lastNs - frames.starts.back();
    return frames.ends[i] >= 0 ? frames.ends[i] - frames.starts[i] : lastNs - frames.starts.back();
}

uint64_t StreamingTimeline::FrameNumber( const Statics& statics, const FrameSet& frames, size_t i )
{
    if( !frames.base ) return i + 1;
    return statics.frameOffset == 0 ? uint64_t( i ) : uint64_t( i ) + statics.frameOffset - 1;
}

std::pair<size_t, size_t> StreamingTimeline::FrameRange( const FrameSet& frames, int64_t from, int64_t to )
{
    auto first = size_t( std::lower_bound( frames.starts.begin(), frames.starts.end(), from ) - frames.starts.begin() );
    if( first == frames.starts.size() ) first--;
    const auto last = size_t( std::lower_bound( frames.starts.begin() + first, frames.starts.end(), to ) - frames.starts.begin() );
    if( first > 0 && frames.starts[first] != from ) --first;
    return { first, last };
}

void StreamingTimeline::ZoomToPrevFrame( const Statics& statics )
{
    const auto& frames = statics.frameSets[m_activeFrameSet];
    if( frames.starts.empty() || m_begin < frames.starts.front() ) return;
    const auto range = FrameRange( frames, m_begin, m_begin );
    auto frame = frames.continuous ? range.first : range.second;
    if( frame == 0 ) return;
    frame--;
    ZoomToRange( frames.starts[frame], FrameEnd( frames, frame, statics.lastNs ) );
}

void StreamingTimeline::ZoomToNextFrame( const Statics& statics )
{
    const auto& frames = statics.frameSets[m_activeFrameSet];
    if( frames.starts.empty() ) return;
    const auto start = m_zoomAnim.active ? m_zoomAnim.start1 : m_begin;
    const size_t frame = start < frames.starts.front() ? 0 : FrameRange( frames, start, start ).first + 1;
    if( frame >= frames.starts.size() ) return;
    ZoomToRange( frames.starts[frame], FrameEnd( frames, frame, statics.lastNs ) );
}

void StreamingTimeline::HandleKeyboard( const Statics& statics, float x0, float w )
{
    constexpr std::array<ImGuiKey, 4> keys { ImGuiKey_A, ImGuiKey_D, ImGuiKey_W, ImGuiKey_S };
    constexpr std::array<int, 4> startMod { -1, 1, 1, -1 }, endMod { -1, 1, -1, 1 };
    auto& io = ImGui::GetIO();
    int64_t start = m_zoomAnim.active ? m_zoomAnim.start1 : m_begin;
    int64_t end = m_zoomAnim.active ? m_zoomAnim.end1 : m_end;
    const auto bias = ( io.MousePos.x - x0 ) / w;
    const auto span = end - start;
    // A tenth of the view per second, at least 500 ns.
    const auto movement = double( std::max<int64_t>( span / 10, 500 ) ) * std::max( std::min( io.DeltaTime, 0.25f ), 0.016f );
    bool moved = false;
    for( int direction = 0; direction < 4; direction++ )
    {
        auto& inertia = m_inertia[direction];
        if( ImGui::IsKeyDown( keys[direction] ) )
        {
            const auto x = inertia / 10.0f;
            const auto mult = 1 + std::max( 0.0, 0.7 * std::pow( x, 1.6 ) - 0.8 * std::pow( x, 1.4 ) );
            if( direction > 1 )
            {
                start += int64_t( movement * startMod[direction] * mult * 2 * bias );
                end += int64_t( movement * endMod[direction] * mult * 2 * ( 1 - bias ) );
            }
            else
            {
                start += int64_t( movement * startMod[direction] * mult );
                end += int64_t( movement * endMod[direction] * mult );
            }
            inertia = std::min( 150.0f, inertia + 1 );
            moved = true;
        }
        else
        {
            inertia = std::max( 0.0f, inertia - 1 );
        }
    }
    if( !moved || start > end ) return;
    start = std::max<int64_t>( std::min( start, statics.lastNs - 50 ), statics.firstNs );
    end = std::max<int64_t>( std::min( end, statics.lastNs ), statics.firstNs + 1 );
    if( end - start <= 50 ) return;
    ZoomToRange( start, end );
}

void StreamingTimeline::DrawOptions()
{
    ImGui::Checkbox( "Draw frame targets", &m_drawFrameTargets );
    ImGui::Indent();
    ImGui::SetNextItemWidth( 90 * GetScale() );
    if( ImGui::InputInt( "Target FPS", &m_frameTarget ) ) m_frameTarget = std::clamp( m_frameTarget, 1, 10000 );
    ImGui::TextDisabled( "The frame graph's colors, guide lines and FPS line, and the timeline's frame shading, follow the target." );
    ImGui::Unindent();
    ImGui::BeginDisabled( !Vram() );
    ImGui::Checkbox( ICON_FA_MICROCHIP " Draw GPU memory", &m_drawVram );
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled( Vram() ? "(video and system memory lanes, with the other plot lanes at the top)" : "(the capture has no GPU memory plots)" );
    ImGui::BeginDisabled( !Usage() );
    ImGui::Checkbox( ICON_FA_GAUGE_HIGH " Draw CPU, memory and GPU usage", &m_drawUsage );
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled( Usage() ? "(CPU, memory and GPU usage lanes at the top)" : "(the capture has no usage plots)" );
    ImGui::TextDisabled( "The plot lanes zoom with the view: zoomed out, each pixel averages the samples it covers; zoomed in, lines join the samples. The dashed line marks the view's middle." );
}

void StreamingTimeline::DrawFrameNavigation()
{
    const auto statics = CurrentStatics();
    if( !statics ) return;
    if( !statics->frameSets.empty() )
    {
        m_activeFrameSet = std::min( m_activeFrameSet, statics->frameSets.size() - 1 );
        const auto& frames = statics->frameSets[m_activeFrameSet];
        if( ImGui::SmallButton( " " ICON_FA_CARET_LEFT " " ) ) ZoomToPrevFrame( *statics );
        ImGui::SameLine();
        ImGui::Text( "%s: %s", frames.base ? "Frames" : frames.name.c_str(), RealToString( frames.starts.size() ) );
        TooltipIfHovered( "Click to go to a frame or a time" );
        if( ImGui::IsItemClicked() )
        {
            ImGui::OpenPopup( "GoToFramePopup" );
            // The popup starts at the frame and the time of the view.
            const auto first = FrameRange( frames, m_begin, m_begin ).first;
            m_goToFrame = int( std::min<uint64_t>( FrameNumber( *statics, frames, first ), INT_MAX ) );
            snprintf( m_goToTime.data(), m_goToTime.size(), "%s", TimeToStringExact( ( m_begin + m_end ) / 2 ) );
        }
        ImGui::SameLine();
        if( ImGui::SmallButton( " " ICON_FA_CARET_RIGHT " " ) ) ZoomToNextFrame( *statics );
        ImGui::SameLine();
        if( ImGui::BeginCombo( "##frameCombo", nullptr, ImGuiComboFlags_NoPreview ) )
        {
            for( size_t n = 0; n < statics->frameSets.size(); ++n )
            {
                const auto& set = statics->frameSets[n];
                if( ImGui::Selectable( set.base ? "Frames" : set.name.c_str(), n == m_activeFrameSet ) ) m_activeFrameSet = n;
                if( n == m_activeFrameSet ) ImGui::SetItemDefaultFocus();
                ImGui::SameLine();
                ImGui::TextDisabled( "(%s)", RealToString( set.starts.size() ) );
            }
            ImGui::EndCombo();
        }
        if( ImGui::BeginPopup( "GoToFramePopup" ) )
        {
            // Frames are numbered as their labels: from 1 in a named set,
            // and from the client's frame count in an on-demand capture.
            const int64_t base = frames.base ? ( statics->frameOffset == 0 ? 0 : int64_t( statics->frameOffset ) - 1 ) : 1;
            const auto lastNumber = base + int64_t( frames.starts.size() ) - 1;
            ImGui::SetNextItemWidth( 140 * GetScale() );
            const bool frameEntered = ImGui::InputInt( "Frame", &m_goToFrame, 1, 100, ImGuiInputTextFlags_EnterReturnsTrue );
            m_goToFrame = int( std::clamp<int64_t>( m_goToFrame, base, std::max( base, lastNumber ) ) );
            if( frameEntered )
            {
                const auto index = size_t( m_goToFrame - base );
                if( index < frames.starts.size() ) ZoomToRange( frames.starts[index], FrameEnd( frames, index, statics->lastNs ) );
            }
            // Not in Tracy's popup: a time, centered at the current zoom.
            ImGui::SetNextItemWidth( 140 * GetScale() );
            if( ImGui::InputTextWithHint( "Time", "mm:ss.sss", m_goToTime.data(), m_goToTime.size(), ImGuiInputTextFlags_EnterReturnsTrue ) )
            {
                if( const auto time = ParseTime( m_goToTime.data() ) )
                {
                    const auto half = std::max<int64_t>( 1, ( m_end - m_begin ) / 2 );
                    ZoomToRange( *time - half, *time + half );
                }
            }
            ImGui::TextDisabled( "Time from start of program, as frame tooltips show it." );
            ImGui::EndPopup();
        }
    }
    ImGui::SameLine();
    ImGui::Spacing();
    ImGui::SameLine();
    // View and capture spans, as Tracy's toolbar shows them.
    auto targetLabelSize = ImGui::CalcTextSize( ICON_FA_EYE " 12345.67 ms" ).x;
    auto cx = ImGui::GetCursorPosX();
    ImGui::Text( ICON_FA_EYE " %s", TimeToString( m_end - m_begin ) );
    TooltipIfHovered( "View span" );
    ImGui::SameLine();
    auto dx = ImGui::GetCursorPosX() - cx;
    if( dx < targetLabelSize ) ImGui::SameLine( cx + targetLabelSize );
    ImGui::Text( ICON_FA_DATABASE " %s", TimeToString( statics->lastNs - statics->firstNs ) );
    if( ImGui::IsItemHovered() )
    {
        ImGui::BeginTooltip();
        if( statics->firstNs == 0 ) ImGui::Text( "Time span" );
        else TextFocused( "Total time span:", TimeToString( statics->lastNs ) );
        ImGui::EndTooltip();
        if( ImGui::IsItemClicked( 2 ) ) ZoomToRange( statics->firstNs, statics->lastNs );
    }
}

// The frame graph of the active frame set: a bar per frame, or per group of
// frames once scaled down, as tall as its frame time (a group's slowest), and
// a line of the average FPS over the last second (over the group when that
// is longer), on one scale with guide lines labelled in FPS and frame times.
// Above it, the frames it shows with their average FPS, 1% low and frames
// over twice the target. Zoomed out, it stops at the first scale at which
// every frame fits and stretches the whole capture across its width; zoomed
// in, the frame under the mouse stays put and the graph never scrolls before
// the first frame or past the last. It keeps the view's frames in sight when
// the view moves elsewhere; a click selects a frame, a drag a range of them.
void StreamingTimeline::DrawFrameGraph()
{
    const auto statics = CurrentStatics();
    if( !statics || statics->frameSets.empty() ) return;
    m_activeFrameSet = std::min( m_activeFrameSet, statics->frameSets.size() - 1 );
    const auto& frames = statics->frameSets[m_activeFrameSet];
    const int total = int( frames.starts.size() );
    if( total == 0 ) return;
    const auto lastNs = statics->lastNs;
    const auto frameWidth = []( int frameScale ) { return frameScale == 0 ? 4 : ( frameScale < 0 ? 6 : 1 ); };
    const auto frameGroup = []( int frameScale ) { return frameScale < 2 ? 1 : ( 1 << ( frameScale - 1 ) ); };

    const auto scale = GetScale();
    const auto Height = 70 * scale;
    const auto ty = ImGui::GetTextLineHeight();
    const int64_t frameTarget = 1000 * 1000 * 1000 / std::max( 1, m_frameTarget );
    // 50 ms, or more so that twice the target fits.
    const int64_t MaxFrameTime = std::max<int64_t>( 50 * 1000 * 1000, frameTarget * 5 / 2 );
    const auto frameColor = [frameTarget]( int64_t time ) { return time > frameTarget * 2 ? 0xFF2222DD : time > frameTarget ? 0xFF22DDDD : 0xFF22DD22; };
    auto& io = ImGui::GetIO();
    auto draw = ImGui::GetWindowDrawList();
    const auto w = ImGui::GetContentRegionAvail().x;
    // The summary row, written once the graph's frames are known.
    const auto rowPos = ImGui::GetCursorScreenPos();
    ImGui::Dummy( ImVec2( w, ty ) );
    const auto wpos = ImGui::GetCursorScreenPos();
    const auto dpos = wpos + ImVec2( 0.5f, 0.5f );
    // The guide lines' labels, in a column right of the bars.
    char guides[2][64];
    const auto guideText = []( char* out, size_t size, double fps, int64_t time ) {
        snprintf( out, size, fps == std::floor( fps ) ? "%.0f FPS \xc2\xb7 %.1f ms" : "%.1f FPS \xc2\xb7 %.1f ms", fps, time / 1e6 );
    };
    guideText( guides[0], sizeof( guides[0] ), double( m_frameTarget ), frameTarget );
    guideText( guides[1], sizeof( guides[1] ), m_frameTarget / 2.0, frameTarget * 2 );
    const auto labelWidth = std::max( ImGui::CalcTextSize( guides[0] ).x, ImGui::CalcTextSize( guides[1] ).x ) + ty;
    const int columns = std::max( 1, int( w - labelWidth - 2 ) );

    // The widest view: the first scale at which every frame fits. It shows
    // the whole capture in bars of whole frames and whole pixels across the
    // graph, rather than squeezed into its left part.
    int fitScale = -1;
    while( fitScale < 10 && columns / frameWidth( fitScale ) * frameGroup( fitScale ) < total ) fitScale++;
    const int bars = std::min( total, columns );
    m_frameScale = std::min( m_frameScale, fitScale );
    // The first frame and the frame count of the column at offset mo.
    const auto columnFrames = [&]( float mo, int frameScale, int start ) {
        if( frameScale >= fitScale )
        {
            const auto bar = std::clamp( int( mo * bars / columns ), 0, bars - 1 );
            const auto first = int( int64_t( bar ) * total / bars );
            return std::make_pair( first, int( int64_t( bar + 1 ) * total / bars ) - first );
        }
        const auto first = start + int( mo * frameGroup( frameScale ) / frameWidth( frameScale ) );
        return std::make_pair( first, std::min( frameGroup( frameScale ), total - first ) );
    };
    // Tracy's initialization and missed frames, left out of the averages.
    const int skip = frames.base ? ( statics->onDemand ? 2 : 1 ) : 0;
    // Frames per second over the frames first to last, taken back to cover a
    // second when they are shorter, and the time that covers.
    const auto averageFps = [&]( int first, int last ) {
        const auto end = FrameEnd( frames, last, lastNs );
        auto from = std::min( first, int( std::lower_bound( frames.starts.begin(), frames.starts.end(), end - 1000 * 1000 * 1000 ) - frames.starts.begin() ) );
        from = std::max( from, std::min( skip, first ) );
        const auto span = end - frames.starts[from];
        return std::make_pair( span > 0 ? 1e9 * ( last - from + 1 ) / double( span ) : 0.0, span );
    };
    const auto selectFrames = [&]( int first, int last ) {
        SetRange( frames.starts[first], FrameEnd( frames, last, lastNs ) );
        if( m_begin == m_end ) m_begin--;
    };

    ImGui::InvisibleButton( "##frames", ImVec2( w, Height ) );
    const bool hover = ImGui::IsItemHovered();
    if( hover ) ImGui::SetItemKeyOwner( ImGuiKey_MouseWheelY );
    draw->AddRectFilled( wpos, wpos + ImVec2( columns + 2, Height ), 0x33FFFFFF );
    const auto wheel = io.MouseWheel;
    const auto prevScale = m_frameScale;
    if( hover )
    {
        if( wheel > 0 )
        {
            if( m_frameScale >= 0 ) m_frameScale--;
        }
        else if( wheel < 0 )
        {
            if( m_frameScale < fitScale ) m_frameScale++;
        }
    }
    const bool fit = m_frameScale >= fitScale;
    const int fwidth = frameWidth( m_frameScale );
    const int group = frameGroup( m_frameScale );
    const int onScreen = columns / fwidth;
    const auto clampStart = [&] { m_frameStart = fit ? 0 : std::clamp( m_frameStart, 0, std::max( 0, total - onScreen * group ) ); };
    if( !ImGui::IsMouseDown( 0 ) ) m_frameDragFirst = -1;
    // When the view moves elsewhere, the graph scrolls to show its frames.
    if( ( m_begin != m_frameGraphBegin || m_end != m_frameGraphEnd ) && m_frameDragFirst < 0 && !( hover && IsMouseDragging( 1 ) ) )
    {
        m_frameGraphBegin = m_begin;
        m_frameGraphEnd = m_end;
        if( !fit )
        {
            const auto view = FrameRange( frames, m_begin, m_end );
            const int first = int( view.first ), last = std::max( first + 1, int( view.second ) );
            const int shown = onScreen * group;
            if( first < m_frameStart || last > m_frameStart + shown ) m_frameStart = last - first <= shown ? ( first + last ) / 2 - shown / 2 : first;
        }
    }
    float crosshair = -1;
    if( hover )
    {
        if( !fit && IsMouseDragging( 1 ) )
        {
            const auto delta = GetMouseDragDelta( 1 ).x;
            if( std::abs( delta ) >= fwidth )
            {
                const auto d = int( delta ) / fwidth;
                m_frameStart -= d * group;
                io.MouseClickedPos[1].x = io.MousePos.x + d * fwidth - delta;
            }
        }
        const auto mx = io.MousePos.x;
        if( mx > wpos.x && mx < wpos.x + columns + 1 )
        {
            const auto mo = mx - ( wpos.x + 1 );
            if( m_frameScale != prevScale ) m_frameStart = columnFrames( mo, prevScale, m_frameStart ).first - int( mo * group / fwidth );
            clampStart();
            const auto [sel, count] = columnFrames( mo, m_frameScale, m_frameStart );
            if( sel < total )
            {
                const auto last = sel + count - 1;
                crosshair = fit ? float( int64_t( std::clamp( int( mo * bars / columns ), 0, bars - 1 ) ) * columns / bars ) + 0.5f * columns / bars
                                : float( ( sel - m_frameStart ) / group * fwidth ) + 0.5f * fwidth;
                const auto [fps, span] = averageFps( sel, last );
                ImGui::BeginTooltip();
                if( count > 1 )
                {
                    int64_t slowest = 0;
                    int over = 0;
                    for( int j = sel; j <= last; j++ )
                    {
                        const auto time = FrameTime( frames, j, lastNs );
                        slowest = std::max( slowest, time );
                        if( time > frameTarget ) over++;
                    }
                    TextDisabledUnformatted( "Frames:" );
                    ImGui::SameLine();
                    ImGui::Text( "%s - %s (%s)", RealToString( FrameNumber( *statics, frames, sel ) ), RealToString( FrameNumber( *statics, frames, last ) ), RealToString( count ) );
                    ImGui::Separator();
                    TextFocused( "Slowest frame:", TimeToString( slowest ) );
                    ImGui::SameLine();
                    ImGui::TextDisabled( "(%.1f FPS)", 1000000000.0 / slowest );
                    TextFocused( "Over the target:", RealToString( over ) );
                    ImGui::SameLine();
                    ImGui::TextDisabled( "of %s frames", RealToString( count ) );
                }
                else
                {
                    const auto fnum = FrameNumber( *statics, frames, sel );
                    const auto frameTime = FrameTime( frames, sel, lastNs );
                    if( frames.base && sel == 0 )
                    {
                        ImGui::TextUnformatted( "Tracy initialization" );
                        ImGui::Separator();
                        TextFocused( "Time:", TimeToString( frameTime ) );
                    }
                    else if( frames.base && statics->onDemand && sel == 1 )
                    {
                        ImGui::TextUnformatted( "Missed frames" );
                        ImGui::Separator();
                        TextFocused( "Time:", TimeToString( frameTime ) );
                    }
                    else
                    {
                        if( frames.base ) TextDisabledUnformatted( "Frame:" );
                        else ImGui::TextDisabled( "%s:", frames.name.c_str() );
                        ImGui::SameLine();
                        ImGui::TextUnformatted( RealToString( fnum ) );
                        ImGui::Separator();
                        TextFocused( "Frame time:", TimeToString( frameTime ) );
                        ImGui::SameLine();
                        ImGui::TextDisabled( "(%.1f FPS)", 1000000000.0 / frameTime );
                    }
                }
                char average[32];
                snprintf( average, sizeof( average ), "%.1f FPS", fps );
                TextFocused( "Average:", average );
                ImGui::SameLine();
                ImGui::TextDisabled( "(over %s)", TimeToString( span ) );
                TextFocused( "Time from start of program:", TimeToStringExact( frames.starts[sel] ) );
                ImGui::EndTooltip();
                if( IsMouseClicked( 0 ) )
                {
                    m_frameDragFirst = sel;
                    m_frameDragLast = last;
                    selectFrames( sel, last );
                }
                else if( m_frameDragFirst >= 0 && IsMouseDragging( 0 ) )
                {
                    selectFrames( std::min( m_frameDragFirst, sel ), std::max( m_frameDragLast, last ) );
                }
            }
        }
    }
    clampStart();

    // The view's frames: a band behind the bars with its edges above them,
    // or a mark above them when it is narrow.
    const auto frameX = [&]( size_t frame ) {
        return fit ? int( int64_t( frame ) * columns / total ) : int( ( int64_t( frame ) - m_frameStart ) * fwidth / group );
    };
    const auto zrange = FrameRange( frames, m_begin, m_end );
    const bool viewShown = int( zrange.second ) > ( fit ? 0 : m_frameStart ) && int( zrange.first ) < ( fit ? total : m_frameStart + onScreen * group );
    auto x1 = std::min( fit ? columns : onScreen * fwidth, frameX( zrange.second ) );
    const auto x0 = std::max( 0, frameX( zrange.first ) );
    if( x0 == x1 ) x1 = x0 + 1;
    if( viewShown && x1 - x0 >= 3 ) draw->AddRectFilled( wpos + ImVec2( 2 + x0, 0 ), wpos + ImVec2( x1, Height ), 0x55DD22DD );

    // The bars, each frame's time or a group's slowest, with a cap where it
    // runs off the scale; the average FPS line through their middles.
    std::vector<ImVec2> line;
    const auto barY = [&]( int64_t time ) { return float( std::min( MaxFrameTime, std::max<int64_t>( 0, time ) ) ) / MaxFrameTime * ( Height - 2 ); };
    const auto drawBar = [&]( int x, int width, int first, int count ) {
        auto f = FrameTime( frames, first, lastNs );
        for( int j = 1; j < count; j++ ) f = std::max( f, FrameTime( frames, first + j, lastNs ) );
        const auto h = std::max( 1.f, barY( f ) );
        if( width != 1 )
            draw->AddRectFilled( wpos + ImVec2( 1 + x, Height - 1 - h ), wpos + ImVec2( x + width, Height - 1 ), frameColor( f ) );
        else
            DrawLine( draw, dpos + ImVec2( 1 + x, Height - 2 - h ), dpos + ImVec2( 1 + x, Height - 2 ), frameColor( f ) );
        const auto cx = wpos.x + 1 + x + 0.5f * width;
        if( f > MaxFrameTime ) draw->AddTriangleFilled( ImVec2( cx - 3 * scale, wpos.y + 1 + 5 * scale ), ImVec2( cx + 3 * scale, wpos.y + 1 + 5 * scale ), ImVec2( cx, wpos.y + 1 ), 0xFF8888FF );
        const auto fps = averageFps( first, first + count - 1 ).first;
        line.emplace_back( cx, wpos.y + Height - 1 - barY( fps > 0 ? int64_t( 1e9 / fps ) : MaxFrameTime ) );
    };
    if( fit )
    {
        line.reserve( bars );
        for( int bar = 0; bar < bars; bar++ )
        {
            const auto first = int( int64_t( bar ) * total / bars );
            const auto x = int( int64_t( bar ) * columns / bars );
            drawBar( x, int( int64_t( bar + 1 ) * columns / bars ) - x, first, int( int64_t( bar + 1 ) * total / bars ) - first );
        }
    }
    else
    {
        line.reserve( onScreen );
        for( int i = 0, idx = m_frameStart; i < onScreen && idx < total; i++, idx += group ) drawBar( i * fwidth, fwidth, idx, std::min( group, total - idx ) );
    }

    if( viewShown )
    {
        if( x1 - x0 >= 3 )
        {
            DrawLine( draw, dpos + ImVec2( 1 + x0, -1 ), dpos + ImVec2( 1 + x0, Height - 1 ), 0x55FF55FF );
            DrawLine( draw, dpos + ImVec2( x1, -1 ), dpos + ImVec2( x1, Height - 1 ), 0x55FF55FF );
        }
        else
        {
            draw->AddRectFilled( wpos + ImVec2( 1 + x0, 0 ), wpos + ImVec2( 1 + x1, Height ), 0x55FF55FF );
        }
    }

    // The target and twice the target, labelled in FPS and frame times.
    const auto guide = [&]( int64_t time, uint32_t color, const char* label ) {
        if( time > MaxFrameTime ) return;
        const auto y = round( Height - Height * double( time ) / MaxFrameTime );
        DrawLine( draw, dpos + ImVec2( 0, y ), dpos + ImVec2( columns + 1, y ), ( color & 0x00FFFFFF ) | 0x66000000 );
        draw->AddText( wpos + ImVec2( columns + 2 + ty * 0.5f, std::clamp( float( y ) - ty * 0.5f, 0.f, Height - ty ) ), color, label );
    };
    guide( frameTarget * 2, 0xFF2222DD, guides[1] );
    guide( frameTarget, 0xFF22DDDD, guides[0] );
    if( line.size() > 1 ) draw->AddPolyline( line.data(), int( line.size() ), 0xDDFFFFFF, ImDrawFlags_None, 1.5f * scale );
    if( crosshair >= 0 ) DrawLine( draw, dpos + ImVec2( 1 + crosshair, 0 ), dpos + ImVec2( 1 + crosshair, Height - 1 ), 0x88FFFFFF );

    // The summary row: the frames shown, their average FPS, 1% low and frames
    // over twice the target, kept while they stay the same.
    const int shownFirst = fit ? 0 : m_frameStart;
    const int shownLast = fit ? total - 1 : std::min( total, m_frameStart + onScreen * group ) - 1;
    auto& summary = m_frameSummary;
    if( summary.set != m_activeFrameSet || summary.first != shownFirst || summary.last != shownLast || summary.total != total || summary.target != m_frameTarget || summary.lastNs != lastNs )
    {
        summary = FrameSummary { m_activeFrameSet, shownFirst, shownLast, total, m_frameTarget, lastNs };
        const int first = std::max( shownFirst, std::min( skip, shownLast ) );
        std::vector<int64_t> times;
        times.reserve( shownLast - first + 1 );
        for( int i = first; i <= shownLast; i++ ) times.push_back( FrameTime( frames, i, lastNs ) );
        summary.slow = int( std::count_if( times.begin(), times.end(), [&]( int64_t t ) { return t > frameTarget * 2; } ) );
        const auto span = FrameEnd( frames, shownLast, lastNs ) - frames.starts[first];
        summary.averageFps = span > 0 ? 1e9 * times.size() / double( span ) : 0;
        const auto k = times.begin() + ( times.size() - 1 ) * 99 / 100;
        std::nth_element( times.begin(), k, times.end() );
        summary.lowFps = *k > 0 ? 1e9 / double( *k ) : 0;
    }
    char text[160];
    snprintf( text, sizeof( text ), "Frames %s - %s%s%s", RealToString( FrameNumber( *statics, frames, shownFirst ) ), RealToString( FrameNumber( *statics, frames, shownLast ) ),
              fit ? " \xc2\xb7 the whole capture" : group > 1 ? " \xc2\xb7 " : "", fit || group == 1 ? "" : ( std::to_string( group ) + " per bar" ).c_str() );
    draw->AddText( rowPos, ImGui::GetColorU32( ImGuiCol_TextDisabled ), text );
    snprintf( text, sizeof( text ), "Average %.1f FPS \xc2\xb7 1%% low %.1f FPS \xc2\xb7 %s over %.1f ms", summary.averageFps, summary.lowFps, RealToString( summary.slow ), frameTarget * 2 / 1e6 );
    draw->AddText( ImVec2( rowPos.x + w - ImGui::CalcTextSize( text ).x, rowPos.y ), ImGui::GetColorU32( ImGuiCol_Text ), text );
}

// As Tracy's DrawTimelineFramesHeader.
void StreamingTimeline::DrawRuler( float w )
{
    const auto wpos = ImGui::GetCursorScreenPos();
    const auto dpos = wpos + ImVec2( 0.5f, 0.5f );
    auto draw = ImGui::GetWindowDrawList();
    const auto ty = ImGui::GetTextLineHeight();
    const auto ty025 = round( ty * 0.25f );
    const auto ty0375 = round( ty * 0.375f );
    const auto ty05 = round( ty * 0.5f );

    const auto timespan = std::max<int64_t>( 1, m_end - m_begin );
    const auto pxns = w / double( timespan );
    const auto nspx = 1.0 / pxns;
    const auto scale = std::max( 0.0, round( log10( nspx ) + 2 ) );
    const auto step = pow( 10, scale );

    ImGui::InvisibleButton( "##zoneFrames", ImVec2( w, ty * 1.5f ) );
    TooltipIfHovered( TimeToStringExact( m_begin + int64_t( ( ImGui::GetIO().MousePos.x - wpos.x ) * nspx ) ) );

    const auto dx = step * pxns;
    double x = 0;
    int tw = 0;
    int tx = 0;
    int64_t tt = 0;
    while( x < w )
    {
        DrawLine( draw, dpos + ImVec2( x, 0 ), dpos + ImVec2( x, ty05 ), 0x66FFFFFF );
        if( tw == 0 )
        {
            char buf[128];
            auto txt = TimeToStringExact( m_begin );
            if( m_begin >= 0 )
            {
                snprintf( buf, sizeof( buf ), "+%s", txt );
                txt = buf;
            }
            draw->AddText( wpos + ImVec2( x, ty05 ), 0x66FFFFFF, txt );
            tw = ImGui::CalcTextSize( txt ).x;
        }
        else if( x > tx + tw + ty * 2 )
        {
            tx = x;
            auto txt = TimeToString( tt );
            draw->AddText( wpos + ImVec2( x, ty05 ), 0x66FFFFFF, txt );
            tw = ImGui::CalcTextSize( txt ).x;
        }

        if( scale != 0 )
        {
            for( int i = 1; i < 5; i++ )
                DrawLine( draw, dpos + ImVec2( x + i * dx / 10, 0 ), dpos + ImVec2( x + i * dx / 10, ty025 ), 0x33FFFFFF );
            DrawLine( draw, dpos + ImVec2( x + 5 * dx / 10, 0 ), dpos + ImVec2( x + 5 * dx / 10, ty0375 ), 0x33FFFFFF );
            for( int i = 6; i < 10; i++ )
                DrawLine( draw, dpos + ImVec2( x + i * dx / 10, 0 ), dpos + ImVec2( x + i * dx / 10, ty025 ), 0x33FFFFFF );
        }

        x += dx;
        tt += int64_t( step );
    }
}

// As Tracy's DrawTimelineFrames, for one frame set; returns the frames labelled.
size_t StreamingTimeline::DrawFrames( const Statics& statics, const FrameSet& frames, size_t index, float w )
{
    const auto lastNs = statics.lastNs;
    const auto frameEnd = [&]( size_t i ) { return FrameEnd( frames, i, lastNs ); };
    const auto frameTime = [&]( size_t i ) { return FrameTime( frames, i, lastNs ); };
    const auto frameText = [&]( size_t i, int64_t ftime ) {
        static char buf[1024];
        if( frames.base )
        {
            const auto number = FrameNumber( statics, frames, i );
            if( i == 0 ) snprintf( buf, sizeof( buf ), "Tracy init (%s)", TimeToString( ftime ) );
            else if( i != 1 || !statics.onDemand ) snprintf( buf, sizeof( buf ), "Frame %s (%s)", RealToString( number ), TimeToString( ftime ) );
            else snprintf( buf, sizeof( buf ), "Missed frames (%s)", TimeToString( ftime ) );
        }
        else snprintf( buf, sizeof( buf ), "%s %s (%s)", frames.name.c_str(), RealToString( i + 1 ), TimeToString( ftime ) );
        return buf;
    };

    const auto wpos = ImGui::GetCursorScreenPos();
    const auto dpos = wpos + ImVec2( 0.5f, 0.5f );
    const auto wh = ImGui::GetContentRegionAvail().y;
    auto draw = ImGui::GetWindowDrawList();
    const auto ty = ImGui::GetTextLineHeight();
    const auto ty025 = ty * 0.25f;
    const auto ty05 = round( ty * 0.5f );

    ImGui::PushID( int( index ) );
    ImGui::InvisibleButton( "##zoneFrames", ImVec2( w, ty ) );
    ImGui::PopID();
    const bool hover = ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenBlockedByActiveItem );
    const auto activeFrameSet = index == m_activeFrameSet;
    if( activeFrameSet )
    {
        m_frameLines.clear();
        m_frameTargetSpans.clear();
    }
    const int64_t frameTarget = activeFrameSet && m_drawFrameTargets ? 1000000000ll / std::max( 1, m_frameTarget ) : INT64_MAX;
    if( frames.starts.empty() ) return 0;

    // As Tracy's GetFrameRange.
    auto first = size_t( std::lower_bound( frames.starts.begin(), frames.starts.end(), m_begin ) - frames.starts.begin() );
    if( first == frames.starts.size() ) first--;
    const auto last = size_t( std::lower_bound( frames.starts.begin() + first, frames.starts.end(), m_end ) - frames.starts.begin() );
    if( first > 0 && frames.starts[first] != m_begin ) --first;
    if( frames.starts[first] > m_end || frameEnd( last ) < m_begin ) return 0;

    const auto timespan = std::max<int64_t>( 1, m_end - m_begin );
    const auto pxns = w / double( timespan );
    const auto nspx = 1.0 / pxns;

    int64_t prev = -1;
    int64_t prevEnd = -1;
    int64_t endPos = -1;
    bool tooltipDisplayed = false;
    size_t labelled = 0;

    const auto inactiveColor = GetColorMuted( 0x888888, activeFrameSet );
    const auto activeColor = GetColorMuted( 0xFFFFFF, activeFrameSet );
    const auto redColor = GetColorMuted( 0x4444FF, activeFrameSet );

    auto i = first;
    auto x1 = ( frames.starts[i] - m_begin ) * pxns;
    while( i < last )
    {
        const auto ftime = frameTime( i );
        const auto fbegin = frames.starts[i];
        const auto fend = frameEnd( i );
        const auto fsz = pxns * ftime;

        if( hover )
        {
            const auto x0 = frames.continuous ? x1 : ( fbegin - m_begin ) * pxns;
            x1 = ( fend - m_begin ) * pxns;
            if( ImGui::IsMouseHoveringRect( wpos + ImVec2( x0, 0 ), wpos + ImVec2( x1, ty ) ) )
            {
                tooltipDisplayed = true;
                ImGui::BeginTooltip();
                ImGui::TextUnformatted( frameText( i, ftime ) );
                ImGui::SameLine();
                ImGui::TextDisabled( "(%.1f FPS)", 1000000000.0 / ftime );
                TextFocused( "Time from start of program:", TimeToStringExact( fbegin ) );
                ImGui::EndTooltip();
                if( IsMouseClicked( 2 ) ) ZoomToRange( fbegin, fend );
            }
        }

        if( fsz < MinFrameSize )
        {
            if( !frames.continuous && prev != -1 )
            {
                if( ( fbegin - prevEnd ) * pxns >= MinFrameSize )
                {
                    DrawZigZag( draw, wpos + ImVec2( 0, ty05 ), ( prev - m_begin ) * pxns, ( prevEnd - m_begin ) * pxns, ty025, inactiveColor );
                    prev = -1;
                }
                else
                {
                    prevEnd = std::max<int64_t>( fend, fbegin + int64_t( MinFrameSize * nspx ) );
                }
            }
            if( prev == -1 )
            {
                prev = fbegin;
                prevEnd = std::max<int64_t>( fend, fbegin + int64_t( MinFrameSize * nspx ) );
            }

            // Skip the frames that end within the minimum of this one.
            const auto key = int64_t( fbegin + MinVisSize * nspx );
            size_t lo = i, hi = last;
            while( lo < hi )
            {
                const auto mid = ( lo + hi ) / 2;
                if( frameEnd( mid ) < key ) lo = mid + 1;
                else hi = mid;
            }
            i = lo == i ? i + 1 : lo;
            continue;
        }

        if( prev != -1 )
        {
            if( frames.continuous )
                DrawZigZag( draw, wpos + ImVec2( 0, ty05 ), ( prev - m_begin ) * pxns, ( fbegin - m_begin ) * pxns, ty025, inactiveColor );
            else
                DrawZigZag( draw, wpos + ImVec2( 0, ty05 ), ( prev - m_begin ) * pxns, ( prevEnd - m_begin ) * pxns, ty025, inactiveColor );
            prev = -1;
        }

        if( activeFrameSet )
        {
            if( fend - fbegin > frameTarget )
            {
                const auto x0 = float( ( fbegin + frameTarget - m_begin ) * pxns ), x1 = float( ( fend - m_begin ) * pxns );
                draw->AddRectFilled( wpos + ImVec2( x0, 0 ), wpos + ImVec2( x1, wh ), 0x224444FF );
                m_frameTargetSpans.emplace_back( x0, x1 );
            }
            if( fbegin >= m_begin && endPos != fbegin )
            {
                const auto x = float( ( fbegin - m_begin ) * pxns );
                DrawLine( draw, dpos + ImVec2( x, 0 ), dpos + ImVec2( x, wh ), 0x22FFFFFF );
                m_frameLines.push_back( x );
            }
            if( fend <= m_end )
            {
                const auto x = float( ( fend - m_begin ) * pxns );
                DrawLine( draw, dpos + ImVec2( x, 0 ), dpos + ImVec2( x, wh ), 0x22FFFFFF );
                m_frameLines.push_back( x );
            }
            endPos = fend;
        }

        const char* buf = frameText( i, ftime );
        auto tx = ImGui::CalcTextSize( buf ).x;
        const uint32_t color = ( frames.base && i == 0 ) ? redColor : activeColor;

        if( fsz - 7 <= tx )
        {
            static char tmp[256];
            snprintf( tmp, sizeof( tmp ), "%s (%s)", RealToString( i ), TimeToString( ftime ) );
            buf = tmp;
            tx = ImGui::CalcTextSize( buf ).x;
        }
        if( fsz - 7 <= tx )
        {
            buf = TimeToString( ftime );
            tx = ImGui::CalcTextSize( buf ).x;
        }

        if( fbegin >= m_begin )
            DrawLine( draw, dpos + ImVec2( ( fbegin - m_begin ) * pxns + 2, 1 ), dpos + ImVec2( ( fbegin - m_begin ) * pxns + 2, ty - 1 ), color );
        if( fend <= m_end )
            DrawLine( draw, dpos + ImVec2( ( fend - m_begin ) * pxns - 2, 1 ), dpos + ImVec2( ( fend - m_begin ) * pxns - 2, ty - 1 ), color );
        if( fsz - 7 > tx )
        {
            const auto f0 = ( fbegin - m_begin ) * pxns + 2;
            const auto f1 = ( fend - m_begin ) * pxns - 2;
            const auto x0 = f0 + 1;
            const auto fx1 = f1 - 1;
            const auto te = fx1 - tx;

            auto tpos = ( x0 + te ) / 2;
            if( tpos < 0 )
                tpos = std::min( std::min( 0., te - tpos ), te );
            else if( tpos > w - tx )
                tpos = std::max( double( w - tx ), x0 );
            tpos = round( tpos );

            DrawLine( draw, dpos + ImVec2( std::max( -10.0, f0 ), ty05 ), dpos + ImVec2( tpos, ty05 ), color );
            DrawLine( draw, dpos + ImVec2( std::max( -10.0, tpos + tx + 1 ), ty05 ), dpos + ImVec2( std::min( w + 20.0, f1 ), ty05 ), color );
            draw->AddText( wpos + ImVec2( tpos, 0 ), color, buf );
            labelled++;
        }
        else
        {
            DrawLine( draw, dpos + ImVec2( std::max( -10.0, ( fbegin - m_begin ) * pxns + 2 ), ty05 ), dpos + ImVec2( std::min( w + 20.0, ( fend - m_begin ) * pxns - 2 ), ty05 ), color );
        }

        i++;
    }

    if( prev != -1 )
    {
        if( frames.continuous )
        {
            DrawZigZag( draw, wpos + ImVec2( 0, ty05 ), ( prev - m_begin ) * pxns, ( frames.starts[last - 1] - m_begin ) * pxns, ty025, inactiveColor );
        }
        else
        {
            const auto begin = ( prev - m_begin ) * pxns;
            const auto end = ( frames.starts[last - 1] - m_begin ) * pxns;
            DrawZigZag( draw, wpos + ImVec2( 0, ty05 ), begin, std::max( begin + MinFrameSize, end ), ty025, inactiveColor );
        }
    }

    if( hover )
    {
        if( !tooltipDisplayed )
        {
            ImGui::BeginTooltip();
            TextDisabledUnformatted( "Frame set:" );
            ImGui::SameLine();
            ImGui::TextUnformatted( frames.base ? "Frames" : frames.name.c_str() );
            ImGui::EndTooltip();
        }
        if( IsMouseClicked( 0 ) ) m_activeFrameSet = index;
    }
    return labelled;
}

// The measured and the zoom ranges and the mouse line, as Tracy draws them
// over its timeline.
void StreamingTimeline::DrawRanges( ImDrawList* draw, ImVec2 wpos, float top, float bottom, double pxns, bool mouseOver, bool tooltip )
{
    if( m_highlight.active && m_highlight.start != m_highlight.end )
    {
        const auto s = std::min( m_highlight.start, m_highlight.end );
        const auto e = std::max( m_highlight.start, m_highlight.end );
        draw->AddRectFilled( ImVec2( wpos.x + float( ( s - m_begin ) * pxns ), top ), ImVec2( wpos.x + float( ( e - m_begin ) * pxns ), bottom ), 0x22DD8888 );
        draw->AddRect( ImVec2( wpos.x + float( ( s - m_begin ) * pxns ), top ), ImVec2( wpos.x + float( ( e - m_begin ) * pxns ), bottom ), 0x44DD8888 );
        if( tooltip )
        {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted( TimeToString( e - s ) );
            ImGui::EndTooltip();
        }
    }
    else if( mouseOver )
    {
        auto& io = ImGui::GetIO();
        DrawLine( draw, ImVec2( io.MousePos.x + 0.5f, top + 0.5f ), ImVec2( io.MousePos.x + 0.5f, bottom + 0.5f ), 0x33FFFFFF );
    }
    if( m_highlightZoom.active && m_highlightZoom.start != m_highlightZoom.end )
    {
        const auto s = std::min( m_highlightZoom.start, m_highlightZoom.end );
        const auto e = std::max( m_highlightZoom.start, m_highlightZoom.end );
        draw->AddRectFilled( ImVec2( wpos.x + float( ( s - m_begin ) * pxns ), top ), ImVec2( wpos.x + float( ( e - m_begin ) * pxns ), bottom ), 0x1688DD88 );
        draw->AddRect( ImVec2( wpos.x + float( ( s - m_begin ) * pxns ), top ), ImVec2( wpos.x + float( ( e - m_begin ) * pxns ), bottom ), 0x2C88DD88 );
    }
}

// As Tracy's ZoneTooltip, with what the store keeps of a zone.
void StreamingTimeline::ZoneTooltip( const Statics& statics, const LaneData& lane, const Item& item ) const
{
    ImGui::BeginTooltip();
    if( item.kind != Kind::Zone && ( item.kind == Kind::Density || item.count > 1 ) )
    {
        TextFocused( "Zones too small to display:", RealToString( item.count ) );
        ImGui::Separator();
        TextFocused( "Execution time:", TimeToString( item.end - item.start ) );
        if( item.kind == Kind::Density ) ImGui::TextDisabled( "Zoom in to see the zones." );
        ImGui::EndTooltip();
        return;
    }
    const auto& source = item.source < statics.sources.size() ? statics.sources[item.source] : statics.sources.front();
    if( item.name != UINT32_MAX ) ImGui::TextUnformatted( lane.names[item.name].c_str() );
    if( !source.name.empty() && source.name != source.function ) ImGui::TextUnformatted( source.name.c_str() );
    ImGui::TextUnformatted( source.function.empty() ? source.name.c_str() : source.function.c_str() );
    ImGui::Separator();
    const auto sourceColor = source.color ? source.color | 0xFF000000 : GetHsvColor( source.function.empty() ? 1 : charutil::hash( source.function.c_str() ), 0 );
    SmallColorBox( sourceColor );
    ImGui::SameLine();
    ImGui::TextUnformatted( LocationToString( source.file.c_str(), source.line ) );
    if( lane.gpu )
    {
        if( item.thread )
        {
            SmallColorBox( GetThreadColor( item.thread, 0, true ) );
            ImGui::SameLine();
            TextFocused( "Thread:", ThreadName( statics, item.thread ) );
            ImGui::SameLine();
            ImGui::TextDisabled( "(%s)", RealToString( item.thread ) );
        }
        ImGui::Separator();
        TextFocused( "GPU execution time:", item.open ? "unfinished" : TimeToString( item.end - item.start ) );
        // As Tracy's: how long the CPU recorded the commands, and how long the
        // GPU took to start them after that recording began.
        if( item.cpuStart )
        {
            if( item.cpuEnd >= item.cpuStart ) TextFocused( "CPU command setup time:", TimeToString( item.cpuEnd - item.cpuStart ) );
            TextFocused( "Delay to execution:", TimeToString( item.start - item.cpuStart ) );
        }
    }
    else
    {
        SmallColorBox( GetThreadColor( item.thread, 0, true ) );
        ImGui::SameLine();
        TextFocused( "Thread:", ThreadName( statics, item.thread ) );
        ImGui::SameLine();
        ImGui::TextDisabled( "(%s)", RealToString( item.thread ) );
        ImGui::Separator();
        if( item.open ) TextFocused( "Execution time:", "unfinished" );
        else
        {
            const auto ztime = item.end - item.start;
            const auto selftime = std::max<int64_t>( 0, ztime - item.childTime );
            TextFocused( "Execution time:", TimeToString( ztime ) );
            TextFocused( "Self time:", TimeToString( selftime ) );
            if( ztime != 0 )
            {
                char buf[64];
                PrintStringPercent( buf, 100.f * selftime / ztime );
                ImGui::SameLine();
                TextDisabledUnformatted( buf );
            }
        }
    }
    ImGui::EndTooltip();
}

void StreamingTimeline::LaneTooltip( const Statics& statics, const Lane& lane ) const
{
    ImGui::BeginTooltip();
    if( lane.gpu )
    {
        if( !lane.name.empty() ) TextFocused( "Name:", lane.name.c_str() );
        ImGui::TextUnformatted( lane.detail.c_str() );
        ImGui::Separator();
        TextFocused( "Zone count:", RealToString( lane.zones ) );
    }
    else
    {
        SmallColorBox( GetThreadColor( lane.id, 0, true ) );
        ImGui::SameLine();
        ImGui::TextUnformatted( lane.name.c_str() );
        ImGui::SameLine();
        ImGui::TextDisabled( "(%s)", RealToString( lane.id ) );
        ImGui::Separator();
        TextFocused( "Zone count:", RealToString( lane.zones ) );
    }
    ImGui::EndTooltip();
}

std::optional<StreamingTimeline::Selection> StreamingTimeline::Draw( float height, uint64_t selected, bool selectedGpu )
{
    std::shared_ptr<const Statics> statics;
    std::shared_ptr<const Snapshot> snapshot;
    {
        std::lock_guard lock( m_mutex );
        statics = m_statics;
        snapshot = m_snapshot;
        if( !m_error.empty() )
        {
            ImGui::TextColored( ImVec4( 1.f, 0.3f, 0.3f, 1.f ), "Timeline: %s", m_error.c_str() );
            return {};
        }
    }
    if( !statics )
    {
        ImGui::TextDisabled( "Loading the timeline..." );
        return {};
    }
    if( m_end <= m_begin ) SetView( statics->firstNs, statics->lastNs );
    auto& io = ImGui::GetIO();
    // Tracy's zoom animation.
    if( m_zoomAnim.active )
    {
        m_zoomAnim.progress += io.DeltaTime * 3.33f;
        if( m_zoomAnim.progress >= 1.f )
        {
            m_zoomAnim.active = false;
            SetView( m_zoomAnim.start1, m_zoomAnim.end1 );
        }
        else
        {
            const auto v = sqrt( sin( M_PI_2 * m_zoomAnim.progress ) );
            SetView( int64_t( m_zoomAnim.start0 + ( m_zoomAnim.start1 - m_zoomAnim.start0 ) * v ), int64_t( m_zoomAnim.end0 + ( m_zoomAnim.end1 - m_zoomAnim.end0 ) * v ) );
        }
    }

    std::optional<Selection> clicked;
    const auto ty = ImGui::GetTextLineHeight();
    const auto ostep = ty + 1;
    const auto spacing = ImGui::GetStyle().ItemSpacing.y;
    ImGui::PushFont( g_fonts.normal, FontSmall );
    const auto sty = ImGui::GetTextLineHeight();
    const auto sstep = sty + 1;
    ImGui::PopFont();

    // The mouse works on the whole timeline, header and lanes, as on Tracy's.
    const auto headerHeight = ty * 1.5f + float( statics->frameSets.size() ) * ty + float( statics->frameSets.size() + 1 ) * spacing;
    const auto lanesHeight = std::max( ty * 4, height - headerHeight - spacing );
    const auto origin = ImGui::GetCursorScreenPos();
    const auto fullWidth = ImGui::GetContentRegionAvail().x;
    const auto w = std::max( 100.f, fullWidth - ImGui::GetStyle().ScrollbarSize );
    const bool mouseOver = ImGui::IsWindowHovered( ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem ) &&
        ImGui::IsMouseHoveringRect( origin, origin + ImVec2( fullWidth, headerHeight + spacing + lanesHeight ) );
    if( mouseOver ) HandleMouse( origin.x, w );
    if( ImGui::IsWindowFocused( ImGuiFocusedFlags_RootAndChildWindows ) && !ImGui::GetIO().WantTextInput ) HandleKeyboard( *statics, origin.x, w );

    const auto timespan = std::max<int64_t>( 1, m_end - m_begin );
    const double pxns = double( w ) / double( timespan );

    ImGui::BeginChild( "##timeline_header", ImVec2( 0, headerHeight ), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar );
    const auto hpos = ImGui::GetCursorScreenPos();
    DrawRuler( w );
    size_t framesDrawn = 0;
    for( size_t n = 0; n < statics->frameSets.size(); ++n ) framesDrawn += DrawFrames( *statics, statics->frameSets[n], n, w );
    DrawRanges( ImGui::GetWindowDrawList(), hpos, ImGui::GetWindowPos().y, ImGui::GetWindowPos().y + ImGui::GetWindowHeight(), pxns, mouseOver, false );
    ImGui::EndChild();

    ImGui::BeginChild( "##timeline_lanes", ImVec2( 0, lanesHeight ), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_AlwaysVerticalScrollbar );
    const auto wpos = ImGui::GetCursorScreenPos();
    const auto dpos = wpos + ImVec2( 0.5f, 0.5f );
    // One item under the lanes keeps the parent window from moving on drags
    // and sizes the scroll range.
    ImGui::InvisibleButton( "##lanes", ImVec2( w, std::max( m_lanesHeight, ImGui::GetContentRegionAvail().y ) ), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle );
    // The wheel zooms here; it must not also scroll the windows around.
    ImGui::SetItemKeyOwner( ImGuiKey_MouseWheelY );
    auto draw = ImGui::GetWindowDrawList();
    const bool hover = ImGui::IsWindowHovered( ImGuiHoveredFlags_AllowWhenBlockedByActiveItem ) && !( m_highlight.active && m_highlight.start != m_highlight.end );
    const auto yMin = ImGui::GetWindowPos().y, yMax = yMin + ImGui::GetWindowHeight();
    for( const auto& [x0, x1] : m_frameTargetSpans ) draw->AddRectFilled( ImVec2( wpos.x + x0, yMin ), ImVec2( wpos.x + x1, yMax ), 0x224444FF );
    for( const auto x : m_frameLines ) DrawLine( draw, ImVec2( dpos.x + x, yMin ), ImVec2( dpos.x + x, yMax ), 0x22FFFFFF );

    std::map<std::pair<uint64_t, bool>, const LaneData*> loaded;
    if( snapshot ) for( const auto& lane : snapshot->lanes ) loaded.emplace( std::make_pair( lane.id, lane.gpu ), &lane );
    Stats stats;
    stats.frames = framesDrawn;
    uint32_t sourceHover = 0;
    float y = 0;
    // Tracy's plot height and header colors, for the GPU memory and usage
    // lanes, which draw as Tracy draws plots.
    const auto plotHeight = 100 * GetScale();
    constexpr uint32_t plotColor = 0xFF44DDDD, plotColorInactive = 0xFF226E6E;
    // A plot lane's header at yBegin: its fold arrow, label and tooltip; a
    // click folds or unfolds the lane.
    // The plot lanes show the view, as the threads do.
    stats.plotBegin = m_begin;
    stats.plotEnd = m_end;
    const auto plotLaneHeader = [&]( float yBegin, uint64_t key, bool expanded, const char* label, const std::function<void()>& tooltip ) {
        const auto hy = wpos.y + yBegin;
        if( hy + ty < yMin || hy > yMax ) return;
        DrawTextContrast( draw, ImVec2( wpos.x, hy ), expanded ? plotColor : plotColorInactive, expanded ? ICON_FA_CARET_DOWN : ICON_FA_CARET_RIGHT );
        const auto labelWidth = ImGui::CalcTextSize( label ).x;
        DrawTextContrast( draw, ImVec2( wpos.x + ty, hy ), expanded ? plotColor : plotColorInactive, label );
        if( expanded ) DrawLine( draw, dpos + ImVec2( 0, yBegin + ty - 1 ), dpos + ImVec2( w, yBegin + ty - 1 ), 0x8844DDDD );
        if( hover && ImGui::IsMouseHoveringRect( ImVec2( wpos.x, hy ), ImVec2( wpos.x + ty + labelWidth, hy + ty ) ) )
        {
            tooltip();
            if( IsMouseClicked( 0 ) )
            {
                if( expanded ) m_collapsed.push_back( key );
                else m_collapsed.erase( std::find( m_collapsed.begin(), m_collapsed.end(), key ) );
            }
        }
    };
    // A plot lane's area at laneTop, over the view, on an opaque background,
    // with the current frame marked. Returns the time under the mouse.
    const auto drawOverview = [&]( float laneTop, float height, const char* label, const std::function<std::optional<int64_t>( const VramPlotArea& )>& drawLane ) {
        auto area = OverviewArea( ImVec2( wpos.x, laneTop ), w, height, m_begin, m_end );
        area.hover = hover;
        area.legendMinX = wpos.x + ty * 2 + ImGui::CalcTextSize( label ).x;
        // Opaque from the header row to the gap after the lane: the frame
        // lines and target spans drawn under the lanes are in the view's time.
        auto background = area;
        background.pos.y -= ostep;
        background.height += 1.2f * ostep;
        return DrawPlotLane( draw, area, background, drawLane );
    };
    // The usage lanes: CPU and memory above all others, GPU usage above the
    // GPU memory lanes.
    const auto usage = m_drawUsage ? Usage() : nullptr;
    const auto drawUsageLane = [&]( UsageLane which ) {
        if( !usage || !UsageLanePresent( *usage, which ) ) return;
        const auto key = UsageLaneKey - uint64_t( which );
        const bool expanded = std::find( m_collapsed.begin(), m_collapsed.end(), key ) == m_collapsed.end();
        const auto yBegin = y;
        const auto label = UsageLaneLabel( which );
        y += ostep;
        if( expanded )
        {
            const auto height = UsageLaneHeight( *usage, which, plotHeight );
            const auto laneTop = wpos.y + y;
            if( laneTop < yMax && laneTop + height > yMin )
            {
                stats.usageLanes++;
                drawOverview( laneTop, height, label, [&]( const VramPlotArea& area ) { return DrawUsageLane( draw, area, *usage, which, plotHeight ); } );
            }
            y += height;
        }
        plotLaneHeader( yBegin, key, expanded, label, [&] { UsageLaneTooltip( *usage, which ); } );
        y += 0.2f * ostep;
    };
    // A GPU memory lane: video (Local) or system (NonLocal) memory.
    const auto vram = m_drawVram ? Vram() : nullptr;
    const auto drawVramLane = [&]( int segment ) {
        if( !vram || !vram->HasSegment( segment ) ) return;
        const auto key = VramLaneKey - uint64_t( segment );
        const bool expanded = std::find( m_collapsed.begin(), m_collapsed.end(), key ) == m_collapsed.end();
        const auto yBegin = y;
        if( m_showVram && segment == ( vram->HasSegment( 0 ) ? 0 : 1 ) )
        {
            // Mouse input on the lanes hands the scroll back to the user.
            const auto& mouse = ImGui::GetIO();
            if( hover && ( mouse.MouseWheel != 0 || ImGui::IsAnyMouseDown() ) ) m_showVram = false;
            else ImGui::SetScrollY( yBegin );
        }
        y += ostep;
        if( expanded )
        {
            const auto laneTop = wpos.y + y;
            if( laneTop < yMax && laneTop + plotHeight > yMin )
            {
                stats.vramLanes++;
                const auto time = drawOverview( laneTop, plotHeight, VramLaneLabel( segment ), [&]( const VramPlotArea& area ) { return DrawVramPlot( draw, area, *vram, segment ); } );
                if( time && IsMouseClickReleased( ImGuiMouseButton_Left ) ) m_vramClick = *time;
            }
            y += plotHeight;
        }
        plotLaneHeader( yBegin, key, expanded, VramLaneLabel( segment ), [&] { VramLaneTooltip( *vram, segment ); } );
        y += 0.2f * ostep;
    };
    // A GPU context's or a thread's lane.
    const auto drawTimelineLane = [&]( const Lane& lane ) {
        const auto key = lane.id * 2 + lane.gpu;
        const auto collapsed = std::find( m_collapsed.begin(), m_collapsed.end(), key );
        const bool expanded = collapsed == m_collapsed.end();
        const auto yBegin = y;
        y += ostep;
        const auto found = expanded ? loaded.find( std::make_pair( lane.id, lane.gpu ) ) : loaded.end();
        const LaneData* data = found == loaded.end() ? nullptr : found->second;
        if( key == m_reportFocusKey && m_reportScrolling && data )
        {
            ImGui::SetScrollY( std::max( 0.f, y + m_reportDepth * ostep - lanesHeight * 0.30f ) );
            if( !Busy() && ++m_reportStable >= 4 ) m_reportScrolling = false;
        }
        if( expanded )
        {
            // Row tops: the groups of a GPU context are below each other,
            // each under its thread's name when there are several.
            std::vector<float> groupTop;
            float contentHeight = ostep;
            if( data )
            {
                const bool labels = data->gpu && data->groups.size() > 1;
                float top = 0;
                if( data->gpu )
                {
                    for( const auto& group : data->groups )
                    {
                        if( labels ) top += sstep;
                        groupTop.push_back( top );
                        top += group.depths * ostep;
                    }
                }
                else
                {
                    groupTop.push_back( 0 );
                    top = data->depths * ostep;
                }
                contentHeight = std::max( top, ostep );
                const auto laneTop = wpos.y + y;
                if( laneTop < yMax && laneTop + contentHeight > yMin )
                {
                    if( key == m_reportFocusKey ) stats.focusLaneVisible = true;
                    stats.lanes++;
                    ImGui::PushClipRect( ImVec2( wpos.x, std::max( laneTop, yMin ) ), ImVec2( wpos.x + w, std::min( laneTop + contentHeight, yMax ) ), true );
                    if( labels )
                    {
                        ImGui::PushFont( g_fonts.normal, FontSmall );
                        for( size_t g = 0; g < data->groups.size(); ++g )
                        {
                            const auto offset = laneTop + groupTop[g];
                            DrawTextContrast( draw, ImVec2( wpos.x + ty, offset - 1 - sstep ), 0xFFFFAAAA, ThreadName( *statics, data->groups[g].thread ) );
                            DrawLine( draw, ImVec2( dpos.x, offset + sty - sstep + 0.5f ), ImVec2( dpos.x + w, offset + sty - sstep + 0.5f ), 0x22FFAAAA );
                        }
                        ImGui::PopFont();
                    }
                    for( const auto& item : data->items )
                    {
                        const auto top = laneTop + groupTop[std::min<size_t>( item.group, groupTop.size() - 1 )] + item.depth * ostep;
                        if( top > yMax || top + ostep < yMin ) continue;
                        const auto pr0 = double( item.start - m_begin ) * pxns, pr1 = double( item.end - m_begin ) * pxns;
                        if( pr1 < -10 || pr0 > w + 10 ) continue;
                        if( item.kind == Kind::Zone && item.id == selected && lane.gpu == selectedGpu ) stats.selectedVisible = true;
                        const auto& source = item.source < statics->sources.size() ? statics->sources[item.source] : statics->sources.front();
                        ImVec2 a, b;
                        switch( item.kind )
                        {
                        case Kind::Density:
                        case Kind::Folded:
                        {
                            const auto color = item.color ? item.color : GetThreadColor( item.thread, int( item.depth ), true );
                            const auto px0 = std::max( pr0, -10.0 );
                            const auto px1 = std::min( std::max( pr1, px0 + MinVisSize ), double( w + 10 ) );
                            a = ImVec2( wpos.x + float( px0 ), top );
                            b = ImVec2( wpos.x + float( px1 ), top + ty );
                            draw->AddRectFilled( a, b, color );
                            DrawZigZag( draw, ImVec2( wpos.x, top + ty / 2 ), px0, px1, ty / 4, DarkenColor( color ) );
                            if( item.count > 1 )
                            {
                                const auto text = RealToString( item.count );
                                const auto tsz = ImGui::CalcTextSize( text );
                                const auto tpx0 = std::max( pr0, 0.0 );
                                if( tsz.x < px1 - tpx0 ) DrawTextContrast( draw, ImVec2( wpos.x + float( tpx0 + ( px1 - tpx0 - tsz.x ) / 2 ), top ), 0xFF4488DD, text );
                            }
                            if( item.kind == Kind::Density ) stats.density++;
                            else stats.folded++;
                            break;
                        }
                        case Kind::Zone:
                        {
                            const auto color = item.color ? item.color : GetHsvColor( item.thread, int( item.depth ) );
                            const auto px0 = std::max( pr0, -10.0 );
                            const auto px1 = std::max( { std::min( pr1, double( w + 10 ) ), px0 + pxns * 0.5, px0 + MinVisSize } );
                            a = ImVec2( wpos.x + float( px0 ), top );
                            b = ImVec2( wpos.x + float( px1 ), top + ty );
                            draw->AddRectFilled( a, b, color );
                            // Tracy's outlines: the zone of the zone info window,
                            // the zones of the hovered zone's source, or a bevel.
                            if( item.id == selected && lane.gpu == selectedGpu )
                                draw->AddRect( a + ImVec2( 1, 1 ), b - ImVec2( 1, 1 ), 0xFF44DD44, 0.f, -1, 3.f );
                            else if( !lane.gpu && m_sourceHighlight == item.source + 1 )
                                draw->AddRect( a, b, 0xFFEEEEEE, 0.f, -1, 1.f );
                            else
                            {
                                const auto ox = dpos.x, oy = top + 0.5f;
                                const auto x0 = ox + float( px0 ), x1 = ox + float( px1 ) - 1;
                                DrawLine( draw, ImVec2( x0, oy + ty ), ImVec2( x0, oy ), ImVec2( x1, oy ), HighlightColor( color ) );
                                DrawLine( draw, ImVec2( x0, oy + ty ), ImVec2( x1, oy + ty ), ImVec2( x1, oy ), DarkenColor( color ) );
                            }
                            const char* name = item.name != UINT32_MAX ? data->names[item.name].c_str() : source.name.c_str();
                            auto tsz = ImGui::CalcTextSize( name );
                            const auto zsz = std::max( pr1 - pr0, pxns * 0.5 );
                            if( tsz.x > zsz ) name = ShortenZoneName( ShortenName::NoSpaceAndNormalize, name, tsz, float( zsz ) );
                            const auto tpx0 = std::max( px0, 0.0 );
                            if( tsz.x < zsz )
                            {
                                const auto x = pr0 + ( pr1 - pr0 - tsz.x ) / 2;
                                if( x < 0 || x > w - tsz.x )
                                {
                                    ImGui::PushClipRect( ImVec2( wpos.x + float( tpx0 ), top ), ImVec2( wpos.x + float( px1 ), top + tsz.y * 2 ), true );
                                    DrawTextContrast( draw, ImVec2( wpos.x + float( std::max( tpx0, std::min( double( w - tsz.x ), x ) ) ), top ), 0xFFFFFFFF, name );
                                    ImGui::PopClipRect();
                                }
                                else if( pr1 == pr0 )
                                {
                                    DrawTextContrast( draw, ImVec2( wpos.x + float( px0 + ( px1 - px0 - tsz.x ) * 0.5 ), top ), 0xFFFFFFFF, name );
                                }
                                else
                                {
                                    DrawTextContrast( draw, ImVec2( wpos.x + float( x ), top ), 0xFFFFFFFF, name );
                                }
                            }
                            else
                            {
                                ImGui::PushClipRect( ImVec2( wpos.x + float( tpx0 ), top ), ImVec2( wpos.x + float( px1 ), top + tsz.y * 2 ), true );
                                DrawTextContrast( draw, ImVec2( wpos.x + float( tpx0 ), top ), 0xFFFFFFFF, name );
                                ImGui::PopClipRect();
                            }
                            stats.zones++;
                            break;
                        }
                        }
                        if( hover && ImGui::IsMouseHoveringRect( a, ImVec2( b.x, b.y + 1 ) ) )
                        {
                            ZoneTooltip( *statics, *data, item );
                            const bool single = item.kind == Kind::Zone || ( item.kind == Kind::Folded && item.count == 1 );
                            if( IsMouseClicked( 2 ) && item.end > item.start ) ZoomToRange( item.start, item.end );
                            if( single && IsMouseClicked( 0 ) ) clicked = Selection { item.id, lane.gpu };
                            if( single && !lane.gpu ) sourceHover = item.source + 1;
                        }
                    }
                    ImGui::PopClipRect();
                }
            }
            else if( wpos.y + y < yMax )
            {
                draw->AddText( ImVec2( wpos.x + ty, wpos.y + y ), 0xFF666666, "..." );
            }
            y += contentHeight;
        }
        // Tracy's item header: caret, label, line, and the context of a named GPU context.
        const auto hy = wpos.y + yBegin;
        if( hy + ty >= yMin && hy <= yMax )
        {
            const auto color = lane.gpu ? 0xFFFFAAAA : 0xFFFFFFFF;
            const auto colorInactive = lane.gpu ? 0xFF886666 : 0xFF888888;
            DrawTextContrast( draw, ImVec2( wpos.x, hy ), expanded ? color : colorInactive, expanded ? ICON_FA_CARET_DOWN : ICON_FA_CARET_RIGHT );
            const char* label = lane.gpu && lane.name.empty() ? lane.detail.c_str() : lane.name.c_str();
            const auto labelWidth = ImGui::CalcTextSize( label ).x;
            DrawTextContrast( draw, ImVec2( wpos.x + ty, hy ), expanded ? color : colorInactive, label );
            if( expanded )
            {
                DrawLine( draw, dpos + ImVec2( 0, yBegin + ty - 1 ), dpos + ImVec2( w, yBegin + ty - 1 ), 0x33FFFFFF );
                if( lane.gpu && !lane.name.empty() ) draw->AddText( ImVec2( wpos.x + ty * 1.5f + labelWidth, hy ), colorInactive, lane.detail.c_str() );
            }
            if( hover && ImGui::IsMouseHoveringRect( ImVec2( wpos.x, hy ), ImVec2( wpos.x + ty + labelWidth, hy + ty ) ) )
            {
                LaneTooltip( *statics, lane );
                if( IsMouseClicked( 0 ) )
                {
                    if( expanded ) m_collapsed.push_back( key );
                    else m_collapsed.erase( collapsed );
                }
            }
        }
        y += 0.2f * ostep;
    };
    // The order: video memory, the direct GPU queue, the main and render
    // threads and the job workers first; then CPU, memory and GPU usage,
    // system memory, the other GPU contexts and the other threads. FPS is in
    // the frame graph.
    std::vector<size_t> front, rest;
    {
        const auto lower = []( std::string text ) {
            std::transform( text.begin(), text.end(), text.begin(), []( unsigned char c ) { return char( std::tolower( c ) ); } );
            return text;
        };
        size_t direct = SIZE_MAX, mainThread = SIZE_MAX, renderThread = SIZE_MAX;
        std::vector<std::pair<long, size_t>> workers;
        for( size_t i = 0; i < statics->lanes.size(); ++i )
        {
            const auto& lane = statics->lanes[i];
            const auto name = lower( lane.name.empty() ? lane.detail : lane.name );
            if( lane.gpu )
            {
                if( direct == SIZE_MAX && name.find( "direct" ) != std::string::npos ) direct = i;
            }
            else if( mainThread == SIZE_MAX && ( name == "main thread" || name == "main" ) ) mainThread = i;
            else if( renderThread == SIZE_MAX && name == "render thread" ) renderThread = i;
            else if( name.rfind( "job.worker ", 0 ) == 0 ) workers.emplace_back( strtol( name.c_str() + 11, nullptr, 10 ), i );
        }
        std::stable_sort( workers.begin(), workers.end() );
        for( const auto i : { direct, mainThread, renderThread } )
            if( i != SIZE_MAX ) front.push_back( i );
        for( const auto& worker : workers ) front.push_back( worker.second );
        for( size_t i = 0; i < statics->lanes.size(); ++i )
            if( std::find( front.begin(), front.end(), i ) == front.end() ) rest.push_back( i );
    }
    drawVramLane( 0 );
    for( const auto i : front ) drawTimelineLane( statics->lanes[i] );
    drawUsageLane( UsageLane::Cpu );
    drawUsageLane( UsageLane::Memory );
    drawUsageLane( UsageLane::Gpu );
    drawVramLane( 1 );
    for( const auto i : rest ) drawTimelineLane( statics->lanes[i] );
    m_lanesHeight = y;
    m_sourceHighlight = sourceHover;
    DrawRanges( draw, wpos, yMin, yMax, pxns, mouseOver, mouseOver );
    if( m_yDelta != 0 )
    {
        ImGui::SetScrollY( ImGui::GetScrollY() - m_yDelta );
        io.MouseClickedPos[1].y = io.MousePos.y;
        m_yDelta = 0;
    }
    ImGui::EndChild();
    if( Busy() || m_skeletonLane.load() )
    {
        char text[256];
        if( const auto building = m_skeletonLane.load() )
            snprintf( text, sizeof( text ), "Indexing %s %d%%", ThreadName( *statics, building ), int( m_skeletonProgress.load() * 100 ) );
        else
            snprintf( text, sizeof( text ), "Reading..." );
        ImGui::GetWindowDrawList()->AddText( hpos + ImVec2( w - ImGui::CalcTextSize( text ).x - 4, 0 ), 0xFF88CCFF, text );
    }
    stats.drawn = snapshot != nullptr;
    stats.current = RequestIfNeeded( *statics, snapshot.get(), w );
    if( m_showVram && stats.vramLanes > 0 && stats.current && !Busy() ) m_showVram = false;
    stats.draws = ++m_draws;
    {
        std::lock_guard lock( m_mutex );
        m_stats = stats;
    }
    return clicked;
}
}
#include "TracyStreamingReportTimeline.inl"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "TracyImGui.hpp"
#include "TracyMouse.hpp"
#include "TracyPrint.hpp"
#include "TracyTimelineContext.hpp"
#include "TracyTimelineItemUsage.hpp"
#include "TracyView.hpp"

namespace tracy
{

std::optional<int64_t> DrawOverviewLane( View& view, const Worker& worker, const TimelineContext& ctx, float top, float height, const char* label,
    const std::function<std::optional<int64_t>( const VramPlotArea& )>& drawLane )
{
    auto area = OverviewArea( ImVec2( ctx.wpos.x, top ), ctx.w, height, ctx.vStart, ctx.vEnd );
    area.hover = ctx.hover;
    area.legendMinX = ctx.wpos.x + ctx.ty * 2 + ImGui::CalcTextSize( label ).x;
    // Opaque from the header row, which the item draws over it afterwards.
    auto background = area;
    background.pos.y -= ctx.ty + 1;
    background.height += ctx.ty + 1;
    return DrawPlotLane( ImGui::GetWindowDrawList(), area, background, drawLane );
}

TimelineItemUsage::TimelineItemUsage( View& view, Worker& worker, UsageLane* lane )
    : TimelineItem( view, worker, lane, false )
    , m_lane( *lane )
{
}

const char* TimelineItemUsage::HeaderLabel() const
{
    return UsageLaneLabel( m_lane );
}

namespace
{
// A series that spans the lane's data.
const VramSeries* RangeSeries( const UsageData* usage, UsageLane lane )
{
    if( !usage ) return nullptr;
    const auto first = []( std::initializer_list<const VramSeries*> list ) -> const VramSeries* {
        for( const auto series : list ) if( !series->Empty() ) return series;
        return nullptr;
    };
    switch( lane )
    {
    case UsageLane::Cpu:
        if( const auto series = first( { &usage->cpuSystem, &usage->cpuProcess, &usage->tracyCpu } ) ) return series;
        for( const auto& series : usage->cores ) if( !series.Empty() ) return &series;
        return nullptr;
    case UsageLane::Memory: return first( { &usage->processCommitted, &usage->processWorkingSet, &usage->systemUsed } );
    case UsageLane::Gpu:
        for( const auto& series : usage->engines ) if( !series.Empty() ) return &series;
        return nullptr;
    case UsageLane::Display: return first( { &usage->displayRefreshes } );
    default: return nullptr;
    }
}

// The GPU context named for the direct queue, else the first Direct3D 12 one.
const GpuCtxData* DirectQueue( const Worker& worker )
{
    const GpuCtxData* direct = nullptr;
    for( const auto ctx : worker.GetGpuData() )
    {
        if( !ctx || ctx->threadData.empty() ) continue;
        std::string name = ctx->name.Active() ? worker.GetString( ctx->name ) : "";
        std::transform( name.begin(), name.end(), name.begin(), []( unsigned char c ) { return char( std::tolower( c ) ); } );
        if( name.find( "direct" ) != std::string::npos ) return ctx;
        if( !direct && ctx->type == GpuContextType::Direct3D12 ) direct = ctx;
    }
    return direct;
}

// The time the direct queue's top-level zones cover in [begin, end).
std::optional<GpuBusy> DirectQueueBusy( const Worker& worker, int64_t begin, int64_t end )
{
    const auto direct = DirectQueue( worker );
    if( !direct ) return std::nullopt;
    std::vector<std::pair<int64_t, int64_t>> spans;
    const auto add = [&]( const GpuEvent& ev ) {
        const auto start = ev.GpuStart();
        const auto zoneEnd = Worker::GetZoneEndDirect( ev );
        if( start >= 0 && zoneEnd > begin && start < end ) spans.emplace_back( std::max( start, begin ), std::min( zoneEnd, end ) );
        return start < end;
    };
    for( const auto& td : direct->threadData )
    {
        // Top-level zones do not overlap: from the one starting before the
        // range on.
        const auto& tl = td.second.timeline;
        if( tl.is_magic() )
        {
            const auto& vec = *(const Vector<GpuEvent>*)&tl;
            auto it = std::lower_bound( vec.begin(), vec.end(), begin, []( const GpuEvent& ev, int64_t t ) { return ev.GpuStart() < t; } );
            if( it != vec.begin() ) --it;
            for( ; it != vec.end() && add( *it ); ++it ) {}
        }
        else
        {
            auto it = std::lower_bound( tl.begin(), tl.end(), begin, []( const short_ptr<GpuEvent>& ev, int64_t t ) { return ev->GpuStart() < t; } );
            if( it != tl.begin() ) --it;
            for( ; it != tl.end() && add( **it ); ++it ) {}
        }
    }
    std::sort( spans.begin(), spans.end() );
    GpuBusy busy;
    int64_t covered = INT64_MIN;
    for( const auto& [start, stop] : spans )
    {
        const auto from = std::max( start, covered );
        if( stop > from ) busy.busy += stop - from;
        covered = std::max( covered, stop );
    }
    return busy;
}
}

int64_t TimelineItemUsage::RangeBegin() const
{
    const auto series = RangeSeries( m_view.GetUsage(), m_lane );
    return series ? series->time.front() : 0;
}

int64_t TimelineItemUsage::RangeEnd() const
{
    const auto series = RangeSeries( m_view.GetUsage(), m_lane );
    return series ? series->time.back() : 0;
}

void TimelineItemUsage::HeaderTooltip( const char* ) const
{
    if( const auto usage = m_view.GetUsage() ) UsageLaneTooltip( *usage, m_lane );
}

bool TimelineItemUsage::DrawContents( const TimelineContext& ctx, int& offset )
{
    const auto usage = m_view.GetUsage();
    if( !usage ) return false;
    const auto plotHeight = float( m_view.GetViewData().plotHeight ) * GetScale();
    const auto height = UsageLaneHeight( *usage, m_lane, plotHeight );
    const auto top = ctx.wpos.y + offset;
    if( top + height >= ctx.yMin && top <= ctx.yMax )
    {
        GpuBusyFn gpuBusy;
        if( m_lane == UsageLane::Display && DirectQueue( m_worker ) )
            gpuBusy = [this]( int64_t begin, int64_t end ) { return DirectQueueBusy( m_worker, begin, end ); };
        DrawOverviewLane( m_view, m_worker, ctx, top, height, HeaderLabel(), [&]( const VramPlotArea& area ) {
            return DrawUsageLane( ImGui::GetWindowDrawList(), area, *usage, m_lane, plotHeight, gpuBusy ); } );
    }
    offset += int( height );
    return true;
}

bool TimelineItemUsage::IsEmpty() const
{
    const auto usage = m_view.GetUsage();
    return !usage || !UsageLanePresent( *usage, m_lane );
}

TimelineItemFps::TimelineItemFps( View& view, Worker& worker, int* key )
    : TimelineItem( view, worker, key, false )
{
}

const char* TimelineItemFps::HeaderLabel() const
{
    return FpsLaneLabel();
}

int64_t TimelineItemFps::RangeBegin() const
{
    const auto frames = m_view.GetFpsFrames();
    return frames.Complete() > 0 ? frames.Start( 0 ) : 0;
}

int64_t TimelineItemFps::RangeEnd() const
{
    const auto frames = m_view.GetFpsFrames();
    return frames.Complete() > 0 ? frames.End( frames.Complete() - 1 ) : 0;
}

void TimelineItemFps::HeaderTooltip( const char* ) const
{
    FpsLaneTooltip( m_view.GetFpsFrames(), int( m_view.GetViewData().frameTarget ) );
}

bool TimelineItemFps::DrawContents( const TimelineContext& ctx, int& offset )
{
    const auto frames = m_view.GetFpsFrames();
    if( frames.Complete() == 0 ) return false;
    const auto plotHeight = float( m_view.GetViewData().plotHeight ) * GetScale();
    const auto height = FpsLaneHeight( plotHeight );
    const auto top = ctx.wpos.y + offset;
    if( top + height >= ctx.yMin && top <= ctx.yMax )
    {
        DrawOverviewLane( m_view, m_worker, ctx, top, height, HeaderLabel(), [&]( const VramPlotArea& area ) {
            return DrawFpsLane( ImGui::GetWindowDrawList(), area, frames, int( m_view.GetViewData().frameTarget ) ); } );
    }
    offset += int( height );
    return true;
}

bool TimelineItemFps::IsEmpty() const
{
    return m_view.GetFpsFrames().Complete() == 0;
}

}

#include <string>

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
    default: return nullptr;
    }
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
        DrawOverviewLane( m_view, m_worker, ctx, top, height, HeaderLabel(), [&]( const VramPlotArea& area ) {
            return DrawUsageLane( ImGui::GetWindowDrawList(), area, *usage, m_lane, plotHeight ); } );
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

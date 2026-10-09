#include "TracyImGui.hpp"
#include "TracyMouse.hpp"
#include "TracyTimelineContext.hpp"
#include "TracyTimelineItemUsage.hpp"
#include "TracyTimelineItemVram.hpp"
#include "TracyView.hpp"
#include "TracyUsage.hpp"
#include "TracyVram.hpp"

namespace tracy
{

TimelineItemVram::TimelineItemVram( View& view, Worker& worker, int* segment )
    : TimelineItem( view, worker, segment, false )
    , m_segment( *segment )
{
    // The system memory lane starts collapsed.
    SetShowFull( m_segment == 0 );
}

const char* TimelineItemVram::HeaderLabel() const
{
    return VramLaneLabel( m_segment );
}

namespace
{
// DXGI usage, or the engine's physical total of a capture without it.
const VramSeries* RangeSeries( const VramData* vram, int segment )
{
    if( !vram ) return nullptr;
    if( !vram->usage[segment].Empty() ) return &vram->usage[segment];
    if( !vram->physical[segment].Empty() ) return &vram->physical[segment];
    return nullptr;
}
}

int64_t TimelineItemVram::RangeBegin() const
{
    const auto series = RangeSeries( m_view.GetVram(), m_segment );
    return series ? series->time.front() : 0;
}

int64_t TimelineItemVram::RangeEnd() const
{
    const auto series = RangeSeries( m_view.GetVram(), m_segment );
    return series ? series->time.back() : 0;
}

void TimelineItemVram::HeaderTooltip( const char* ) const
{
    if( const auto vram = m_view.GetVram() ) VramLaneTooltip( *vram, m_segment );
}

bool TimelineItemVram::DrawContents( const TimelineContext& ctx, int& offset )
{
    const auto vram = m_view.GetVram();
    if( !vram ) return false;
    const auto height = float( m_view.GetViewData().plotHeight ) * GetScale();
    const auto top = ctx.wpos.y + offset;
    if( top + height >= ctx.yMin && top <= ctx.yMax )
    {
        const auto time = DrawOverviewLane( m_view, m_worker, ctx, top, height, HeaderLabel(), [&]( const VramPlotArea& area ) {
            return DrawVramPlot( ImGui::GetWindowDrawList(), area, *vram, m_segment ); } );
        if( time && IsMouseClickReleased( ImGuiMouseButton_Left ) ) m_view.ShowVramAt( *time );
    }
    offset += int( height );
    return true;
}

bool TimelineItemVram::IsEmpty() const
{
    const auto vram = m_view.GetVram();
    return !vram || !vram->HasSegment( m_segment );
}

}

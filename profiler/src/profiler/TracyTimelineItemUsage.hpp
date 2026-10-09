#ifndef __TRACYTIMELINEITEMUSAGE_HPP__
#define __TRACYTIMELINEITEMUSAGE_HPP__

#include <functional>
#include <optional>

#include "TracyTimelineItem.hpp"
#include "TracyUsage.hpp"

namespace tracy
{

struct TimelineContext;

// Draws a plot lane of the normal view over the view, with the current
// frame marked. Returns the time under the mouse.
std::optional<int64_t> DrawOverviewLane( View& view, const Worker& worker, const TimelineContext& ctx, float top, float height, const char* label,
    const std::function<std::optional<int64_t>( const VramPlotArea& )>& drawLane );

// A usage lane: CPU busy share with a row per logical processor, memory, or
// GPU engine use, in place of a plot lane per usage plot.
class TimelineItemUsage final : public TimelineItem
{
public:
    // `lane` is owned by the view.
    TimelineItemUsage( View& view, Worker& worker, UsageLane* lane );

protected:
    uint32_t HeaderColor() const override { return 0xFF44DDDD; }
    uint32_t HeaderColorInactive() const override { return 0xFF226E6E; }
    uint32_t HeaderLineColor() const override { return 0x8844DDDD; }
    const char* HeaderLabel() const override;

    int64_t RangeBegin() const override;
    int64_t RangeEnd() const override;

    void HeaderTooltip( const char* label ) const override;

    bool DrawContents( const TimelineContext& ctx, int& offset ) override;

    bool IsEmpty() const override;

private:
    UsageLane m_lane;
};

// The FPS lane of the base frame set, against the view's target FPS.
class TimelineItemFps final : public TimelineItem
{
public:
    // `key` is owned by the view.
    TimelineItemFps( View& view, Worker& worker, int* key );

protected:
    uint32_t HeaderColor() const override { return 0xFF44DDDD; }
    uint32_t HeaderColorInactive() const override { return 0xFF226E6E; }
    uint32_t HeaderLineColor() const override { return 0x8844DDDD; }
    const char* HeaderLabel() const override;

    int64_t RangeBegin() const override;
    int64_t RangeEnd() const override;

    void HeaderTooltip( const char* label ) const override;

    bool DrawContents( const TimelineContext& ctx, int& offset ) override;

    bool IsEmpty() const override;
};

}

#endif

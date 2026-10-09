#ifndef __TRACYTIMELINEITEMVRAM_HPP__
#define __TRACYTIMELINEITEMVRAM_HPP__

#include "TracyTimelineItem.hpp"

namespace tracy
{

// A GPU memory segment's lane: the engine's per-category plots stacked, with
// DXGI usage and budget, in place of a plot lane per category.
class TimelineItemVram final : public TimelineItem
{
public:
    // `segment` is a TracyJnVram.hpp segment, owned by the view.
    TimelineItemVram( View& view, Worker& worker, int* segment );

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
    int m_segment;
};

}

#endif

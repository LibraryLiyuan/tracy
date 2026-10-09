#ifndef __TRACYVRAM_HPP__
#define __TRACYVRAM_HPP__

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <stdint.h>
#include <string>
#include <string_view>
#include <vector>

#include "imgui.h"
#include "../../../public/common/TracyJnVram.hpp"

namespace tracy
{

// One GPU memory plot of TracyJnVram.hpp in time order. A value holds until
// the next point.
struct VramSeries
{
    std::vector<int64_t> time;
    std::vector<double> value;

    bool Empty() const { return time.empty(); }
    void Add( int64_t t, double v ) { time.push_back( t ); value.push_back( v ); }
    // The value holding at t, none before the first point.
    std::optional<double> At( int64_t t ) const;
};

// The GPU memory plots of a capture. The engine sends a plot at most every
// 250 ms, so a 30 minute capture holds a few hundred thousand points at most
// and they are loaded whole.
struct VramData
{
    std::array<VramSeries, JnVram::Segments> usage, budget, physical, logical;
    std::array<std::array<std::array<VramSeries, JnVram::Categories>, JnVram::Ledgers>, JnVram::Segments> categories;
    VramSeries evicted;
    // The adapter over all processes (TracyJnUsage.hpp): bytes resident in
    // its video memory segments (Local) and in system memory segments
    // (NonLocal), and the size of its video memory.
    std::array<VramSeries, JnVram::Segments> adapter;
    VramSeries adapterTotal;
    // The end of the capture, where the last values stop holding.
    int64_t lastTime = 0;

    // The series a plot of this name loads into, null for other plots.
    VramSeries* Find( std::string_view name );
    // Orders the series by time once loaded.
    void Finish();
    bool Empty() const;
    bool HasSegment( int segment ) const;
    bool HasCategories( int segment ) const;
};

// A GPU resource the engine registered, live at a time.
struct VramResource
{
    std::string name;
    // 'T' texture, 'B' buffer, 'U' upload, 'R' readback.
    char kind = 'U';
    // Its JnVram::Category, -1 in records that predate categories.
    int8_t category = -1;
    // Its name is still being looked up.
    bool pending = false;
    // The GPU work of the list's frame referenced it.
    bool used = false;
    uint64_t size = 0;
    int64_t created = 0;
    std::optional<int64_t> freed;
};

// The resources of a segment live at a time: textures and buffers in video
// memory, upload and readback buffers in system memory.
struct VramResources
{
    // False when the capture has no resource records.
    bool present = true;
    int segment = 0;
    int64_t time = 0;
    uint64_t live = 0, liveBytes = 0;
    // All of them, largest first, and how many names are still looked up.
    std::vector<VramResource> resources;
    size_t pending = 0;
    // The frame holding `time`, from a capture with GPU resource references:
    // the resources its command lists referenced, each once, how many of
    // them this list holds, and their bytes.
    bool frameReferences = false;
    uint64_t frameNumber = 0;
    int64_t frameBegin = 0, frameEnd = 0;
    uint64_t referenced = 0, used = 0, usedBytes = 0;
};

// The engine's resource records: for each resource, zones of this marker
// named after it, whose text holds a record line; one when it is created, one
// more each time it is renamed or changes category.
constexpr const char* VramResourceMarker = "GTMEM Request Scope";
struct VramResourceRecord
{
    uint64_t allocation = 0;
    char kind = 'U', segment = 'L';
    int8_t category = -1;
};
std::optional<VramResourceRecord> ParseVramResourceRecord( std::string_view text );
// The kind of the resources of a memory pool, 0 when it holds none.
char VramPoolKind( std::string_view pool );
int VramKindSegment( char kind );
// What the resource list shows as a resource's type: its category, or its
// kind in records that predate categories; upload and readback buffers by
// their kind.
enum class VramResourceType : uint8_t { Texture, RenderTarget, DepthStencil, Buffer, RayTracing, Other, Upload, Readback };
constexpr int VramResourceTypes = 8;
constexpr const char* VramResourceTypeNames[VramResourceTypes] = { "Texture", "RenderTarget", "DepthStencil", "Buffer", "RayTracing", "Other", "Upload", "Readback" };
VramResourceType VramResourceTypeOf( char kind, int category );

// The per-category plots, which the GPU memory lanes draw stacked instead of
// a plot lane each.
bool IsVramCategoryPlot( std::string_view name );
// A segment's lane label: "GPU memory: video (Local)".
const char* VramLaneLabel( int segment );

struct VramPlotArea
{
    // The top left of the plot, at time `begin`.
    ImVec2 pos;
    float width = 0, height = 0;
    int64_t begin = 0;
    double pxns = 1;
    bool hover = false;
    // The legend goes on the lane's header row, above the plot, right of this.
    float legendMinX = 0;
};

// Plot drawing, shared with the usage lanes (TracyUsage.hpp). Per pixel
// column of an area, the largest value holding in it: NaN before the first
// point and past `end`.
void PlotPeakColumns( const VramSeries& series, const VramPlotArea& area, int64_t end, int count, std::vector<double>& out );
// Fills the columns between `low` and `high`, offsets from the top of the
// area, merging neighbours of the same extent.
void PlotFillColumns( ImDrawList* draw, const VramPlotArea& area, const std::vector<float>& low, const std::vector<float>& high, uint32_t color );
// A step line through the columns' values.
void PlotLineColumns( ImDrawList* draw, const VramPlotArea& area, const std::vector<double>& values, double scale, uint32_t color, float thickness );
// Draws a legend entry ending at x, unless it would cross minX; returns
// whether it did.
bool PlotLegendEntry( ImDrawList* draw, float& x, float minX, float y, uint32_t color, const char* label, bool line );

// The plot lanes (GPU memory, usage, FPS): the area spans [first, last] over
// its width, on an opaque background that keeps the timeline's frame lines
// out, and a marker has a dashed line at the view's middle, and shades the
// view when it is narrower than the area.
VramPlotArea OverviewArea( ImVec2 pos, float width, float height, int64_t first, int64_t last );
void PlotOverviewBackground( ImDrawList* draw, const VramPlotArea& area );
void PlotViewMarker( ImDrawList* draw, const VramPlotArea& area, int64_t viewBegin, int64_t viewEnd );

// Draws a segment's GPU memory as a plot lane: the physical bytes of its
// categories stacked, the engine's physical total when the capture has no
// categories, and DXGI usage, budget and the adapter's use by all processes
// as lines, with a legend of what the view shows on the header row. Returns the time under the mouse, after
// showing its tooltip.
std::optional<int64_t> DrawVramPlot( ImDrawList* draw, const VramPlotArea& area, const VramData& data, int segment );
// A segment at a time: DXGI usage against budget, the engine's ledgers and
// the categories.
void VramTooltip( const VramData& data, int segment, int64_t time );
// The header tooltip of a segment's lane: its peaks over the capture.
void VramLaneTooltip( const VramData& data, int segment );

struct VramPanelState
{
    int segment = 0;
    // Follow the middle of the view, or show `time`.
    bool follow = true;
    int64_t time = 0;
    // The peaks of the capture, computed once per data.
    const VramData* summarized = nullptr;
    struct Peak
    {
        bool present = false;
        double value = 0;
        int64_t time = 0;
    };
    struct Growth
    {
        int category = 0;
        double first = 0, last = 0;
        Peak peak;
    };
    std::array<Peak, JnVram::Segments> usagePeak, budgetSharePeak, physicalPeak;
    std::array<int64_t, JnVram::Segments> above90 {}, aboveBudget {};
    std::array<std::vector<Growth>, JnVram::Segments> growth;
    Peak evictedPeak;
    // The resource list: its filters, and the rows they leave of the list
    // shown, in the table's order. Grouped, a row is a name.
    int resourceType = -1;
    ImGuiTextFilter resourceFilter;
    bool resourceGroups = false;
    // Only the resources the frame's GPU work referenced.
    bool usedOnly = false;
    struct ResourceGroup
    {
        uint32_t first = 0, count = 0;
        uint64_t size = 0;
    };
    const VramResources* listed = nullptr;
    int listedType = -1;
    std::string listedFilter;
    bool listedGroups = false, listedUsedOnly = false;
    std::vector<uint32_t> rows;
    std::vector<ResourceGroup> groups;
    uint64_t rowsSize = 0;
};

struct VramPanelActions
{
    // Zooms the timeline to a range.
    std::function<void( int64_t, int64_t )> zoomTo;
    // Scrolls the timeline to the GPU memory lanes; empty where it cannot.
    std::function<void()> showLanes;
    // The resources of a segment live at a time; empty where the view cannot
    // list them. Returns the last list loaded, which can be for another time
    // while this one loads, or null before the first.
    std::function<std::shared_ptr<const VramResources>( int segment, int64_t time )> resources;
};

// The GPU memory panel: at the view's middle or a picked time, a segment's
// usage against budget, its ledgers, categories and largest resources; then
// the capture's peaks and the growth of each category, with buttons that zoom
// the timeline to them.
void DrawVramPanel( const VramData& data, VramPanelState& state, int64_t viewCenter, const VramPanelActions& actions );

}

#endif

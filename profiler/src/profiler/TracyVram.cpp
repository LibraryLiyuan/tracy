#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdio.h>

#include "TracyColor.hpp"
#include "TracyImGui.hpp"
#include "TracyVram.hpp"
#include "../../../public/common/TracyJnUsage.hpp"
#include "../../../server/TracyPrint.hpp"

namespace tracy
{

namespace
{

constexpr double Nan = std::numeric_limits<double>::quiet_NaN();
constexpr uint32_t UsageColor = 0xFFEEEEEE;
constexpr uint32_t BudgetColor = 0xFF5555EE;
constexpr uint32_t PhysicalColor = 0xFF44DDDD;
constexpr uint32_t AdapterColor = 0xFFDD88CC;
// A zoom to a point of the capture shows a second on each side.
constexpr int64_t ZoomHalfWidth = 1'000'000'000;
// The engine sends one collect's plots within microseconds of each other.
constexpr int64_t CollectSlack = 1'000'000;

// TracyJnVram.hpp's colors are 0xRRGGBB, ImGui's 0xAABBGGRR.
uint32_t CategoryColor( int category, uint32_t alpha = 0xFF )
{
    const auto rgb = JnVram::CategoryColors[category];
    return ( alpha << 24 ) | ( ( rgb & 0xFF ) << 16 ) | ( rgb & 0xFF00 ) | ( rgb >> 16 );
}

const char* Bytes( double value )
{
    return MemSizeToString( int64_t( std::llround( value ) ) );
}

const char* SegmentLabel( int segment )
{
    return segment == 0 ? "Video memory (Local)" : "System memory (NonLocal)";
}

int64_t LastPoint( const VramData& data )
{
    int64_t last = data.lastTime;
    const auto visit = [&]( const VramSeries& series ) { if( !series.Empty() ) last = std::max( last, series.time.back() ); };
    for( int s = 0; s < JnVram::Segments; ++s )
    {
        visit( data.usage[s] );
        visit( data.budget[s] );
        visit( data.physical[s] );
        visit( data.logical[s] );
        visit( data.adapter[s] );
    }
    return last;
}

// When the engine's registry first held memory. Its first sample after a
// connection can be empty, taken before the D3D12 memory snapshot replayed
// into it.
std::optional<int64_t> RegistryStart( const VramData& data, int segment )
{
    const auto firstHeld = []( const VramSeries& series ) -> std::optional<int64_t> {
        for( size_t i = 0; i < series.time.size(); ++i )
            if( series.value[i] > 0 ) return series.time[i];
        return std::nullopt;
    };
    if( !data.physical[segment].Empty() ) return firstHeld( data.physical[segment] );
    std::optional<int64_t> start;
    for( const auto& series : data.categories[segment][int( JnVram::Ledger::Physical )] )
        if( const auto t = firstHeld( series ); t && ( !start || *t < *start ) ) start = t;
    return start;
}

// The engine's physical bytes of a segment: its total, or the sum of its
// categories when only those were recorded.
// Whether the registry held memory by this time: before that its values are
// not known, rather than zero.
bool RegistryKnown( const VramData& data, int segment, int64_t time )
{
    const auto start = RegistryStart( data, segment );
    return start && time + CollectSlack >= *start;
}

std::optional<double> PhysicalAt( const VramData& data, int segment, int64_t time )
{
    if( !RegistryKnown( data, segment, time ) ) return std::nullopt;
    time += CollectSlack;
    if( const auto total = data.physical[segment].At( time ) ) return total;
    if( !data.HasCategories( segment ) ) return std::nullopt;
    double sum = 0;
    for( const auto& series : data.categories[segment][int( JnVram::Ledger::Physical )] ) sum += series.At( time ).value_or( 0 );
    return sum;
}

}

// Per pixel column of the area, the largest value holding in it: NaN before
// the first point and past `end`.
void PlotPeakColumns( const VramSeries& series, const VramPlotArea& area, int64_t end, int count, std::vector<double>& out )
{
    out.assign( count, Nan );
    if( series.Empty() ) return;
    const auto nsPerPixel = 1.0 / area.pxns;
    size_t i = size_t( std::upper_bound( series.time.begin(), series.time.end(), area.begin ) - series.time.begin() );
    double current = i > 0 ? series.value[i - 1] : Nan;
    for( int c = 0; c < count; ++c )
    {
        if( area.begin + int64_t( c * nsPerPixel ) >= end ) break;
        const auto t1 = area.begin + int64_t( ( c + 1 ) * nsPerPixel );
        double peak = current;
        while( i < series.time.size() && series.time[i] < t1 )
        {
            current = series.value[i++];
            peak = std::isnan( peak ) ? current : std::max( peak, current );
        }
        out[c] = peak;
    }
}

// Fills the columns between `low` and `high`, offsets from the top of the
// area, merging neighbours of the same extent.
void PlotFillColumns( ImDrawList* draw, const VramPlotArea& area, const std::vector<float>& low, const std::vector<float>& high, uint32_t color )
{
    const int count = int( high.size() );
    for( int c = 0; c < count; )
    {
        if( !( high[c] < low[c] ) )
        {
            c++;
            continue;
        }
        int e = c + 1;
        while( e < count && high[e] == high[c] && low[e] == low[c] ) e++;
        draw->AddRectFilled( ImVec2( area.pos.x + float( c ), area.pos.y + high[c] ), ImVec2( area.pos.x + float( e ), area.pos.y + low[c] ), color );
        c = e;
    }
}

// A step line through the columns' values.
void PlotLineColumns( ImDrawList* draw, const VramPlotArea& area, const std::vector<double>& values, double scale, uint32_t color, float thickness )
{
    std::vector<ImVec2> points;
    float lastX = 0, lastY = 0;
    const auto flush = [&] {
        if( !points.empty() )
        {
            points.emplace_back( lastX, lastY );
            draw->AddPolyline( points.data(), int( points.size() ), color, ImDrawFlags_None, thickness );
        }
        points.clear();
    };
    for( int c = 0; c < int( values.size() ); ++c )
    {
        if( std::isnan( values[c] ) )
        {
            flush();
            continue;
        }
        const auto x = area.pos.x + float( c );
        const auto y = area.pos.y + area.height - float( values[c] * scale );
        if( points.empty() ) points.emplace_back( x, y );
        else if( y != lastY )
        {
            points.emplace_back( x, lastY );
            points.emplace_back( x, y );
        }
        lastX = x + 1;
        lastY = y;
    }
    flush();
}

// Draws a legend entry ending at x, unless it would cross minX; returns
// whether it did.
bool PlotLegendEntry( ImDrawList* draw, float& x, float minX, float y, uint32_t color, const char* label, bool line )
{
    const auto ty = ImGui::GetTextLineHeight();
    const auto width = ImGui::CalcTextSize( label ).x;
    if( x - width - ty * 1.8f < minX ) return false;
    x -= width;
    DrawTextContrast( draw, ImVec2( x, y ), 0xFFCCCCCC, label );
    x -= ty * 0.3f + ty * 0.7f;
    if( line ) draw->AddRectFilled( ImVec2( x, y + ty * 0.45f ), ImVec2( x + ty * 0.7f, y + ty * 0.6f ), color );
    else draw->AddRectFilled( ImVec2( x, y + ty * 0.2f ), ImVec2( x + ty * 0.7f, y + ty * 0.85f ), color );
    x -= ty * 0.8f;
    return true;
}

VramPlotArea OverviewArea( ImVec2 pos, float width, float height, int64_t first, int64_t last )
{
    VramPlotArea area;
    area.pos = pos;
    area.width = width;
    area.height = height;
    area.begin = first;
    area.pxns = double( width ) / double( std::max<int64_t>( 1, last - first ) );
    return area;
}

void PlotOverviewBackground( ImDrawList* draw, const VramPlotArea& area )
{
    auto color = ImGui::GetStyleColorVec4( ImGuiCol_WindowBg );
    color.w = 1;
    draw->AddRectFilled( area.pos, area.pos + ImVec2( area.width, area.height ), ImGui::ColorConvertFloat4ToU32( color ) );
}

void PlotViewMarker( ImDrawList* draw, const VramPlotArea& area, int64_t viewBegin, int64_t viewEnd )
{
    const auto bottom = area.pos.y + area.height;
    const auto x0 = area.pos.x + float( double( viewBegin - area.begin ) * area.pxns );
    const auto x1 = area.pos.x + float( double( viewEnd - area.begin ) * area.pxns );
    draw->PushClipRect( area.pos, ImVec2( area.pos.x + area.width, bottom ), true );
    if( x1 - x0 >= 2 && ( x0 > area.pos.x + 0.5f || x1 < area.pos.x + area.width - 0.5f ) ) draw->AddRectFilled( ImVec2( x0, area.pos.y ), ImVec2( x1, bottom ), 0x1CFFFFFF );
    const auto x = std::round( ( x0 + x1 ) * 0.5f ) + 0.5f;
    const auto dash = 4 * GetScale(), gap = 3 * GetScale();
    for( auto y = area.pos.y; y < bottom; y += dash + gap ) draw->AddLine( ImVec2( x, y ), ImVec2( x, std::min( y + dash, bottom ) ), 0xDDFFFFFF );
    draw->PopClipRect();
}

namespace
{

void ZoomButton( const char* id, int64_t time, const std::function<void( int64_t, int64_t )>& zoomTo )
{
    ImGui::SameLine();
    ImGui::PushID( id );
    if( ImGui::SmallButton( ICON_FA_MAGNIFYING_GLASS ) ) zoomTo( time - ZoomHalfWidth, time + ZoomHalfWidth );
    ImGui::PopID();
    TooltipIfHovered( "Zoom the timeline to this time" );
}

// DXGI's usage and budget of a segment, the engine's ledgers, and what the
// engine does not see.
void DrawLedgers( const VramData& data, int segment, int64_t time, bool bar )
{
    const auto usage = data.usage[segment].At( time );
    const auto budget = data.budget[segment].At( time + CollectSlack );
    const auto physical = PhysicalAt( data, segment, time );
    const auto logical = RegistryKnown( data, segment, time ) ? data.logical[segment].At( time + CollectSlack ) : std::nullopt;
    char buf[256];
    if( usage )
    {
        if( budget && *budget > 0 )
        {
            const auto share = *usage / *budget;
            snprintf( buf, sizeof( buf ), "%s of %s budget (%.1f%%)", Bytes( *usage ), Bytes( *budget ), share * 100 );
            TextFocused( "DXGI usage:", buf );
            if( bar )
            {
                ImGui::PushStyleColor( ImGuiCol_PlotHistogram, share >= 0.95 ? 0xFF4444DD : share >= 0.9 ? 0xFF33AADD : 0xFF44AA44 );
                ImGui::ProgressBar( float( std::min( share, 1.0 ) ), ImVec2( -1, 0 ), share > 1 ? "Over budget" : "" );
                ImGui::PopStyleColor();
            }
        }
        else TextFocused( "DXGI usage:", Bytes( *usage ) );
    }
    else ImGui::TextDisabled( "DXGI usage: not recorded at this time" );
    if( const auto adapter = data.adapter[segment].At( time ) )
    {
        const auto total = segment == 0 ? data.adapterTotal.At( time ) : std::nullopt;
        if( total && *total > 0 ) snprintf( buf, sizeof( buf ), "%s of %s", Bytes( *adapter ), Bytes( *total ) );
        else snprintf( buf, sizeof( buf ), "%s", Bytes( *adapter ) );
        TextFocused( "All processes:", buf );
        TooltipIfHovered( segment == 0 ?
            "What every process holds resident in the adapter's video memory, as Task Manager's dedicated GPU memory shows it." :
            "What every process holds resident in system memory for the adapter, as Task Manager's shared GPU memory shows it." );
    }
    if( physical ) TextFocused( "Engine physical:", Bytes( *physical ) );
    else if( data.HasSegment( segment ) && !data.physical[segment].Empty() ) ImGui::TextDisabled( "Engine physical: not recorded yet" );
    if( logical )
    {
        if( physical ) ImGui::SameLine();
        TextFocused( "Engine logical:", Bytes( *logical ) );
    }
    // The residency manager evicts video memory only. DXGI usage stops
    // counting what it evicts; the engine's physical bytes still count it.
    const auto evicted = segment == 0 && RegistryKnown( data, segment, time ) ? data.evicted.At( time + CollectSlack ) : std::nullopt;
    if( evicted && *evicted > 0 )
    {
        TextFocused( "Evicted:", Bytes( *evicted ) );
        TooltipIfHovered( "Engine memory the residency manager evicted from video memory. It still counts in the engine's physical bytes, but no longer in DXGI usage." );
    }
    if( usage && physical )
    {
        TextFocused( "Not seen by the engine:", Bytes( *usage - ( *physical - evicted.value_or( 0 ) ) ) );
        TooltipIfHovered( segment == 0 ?
            "DXGI usage minus the engine's physical bytes still in video memory (physical minus evicted): swap chains, descriptor heaps, pipeline states and driver memory." :
            "DXGI usage minus the engine's physical bytes: swap chains, descriptor heaps, pipeline states and driver memory." );
    }
}

// The categories of a segment at a time, by physical bytes.
void DrawCategoryTable( const VramData& data, int segment, int64_t time, const char* id )
{
    if( !data.HasCategories( segment ) )
    {
        ImGui::TextDisabled( "No categories: the capture predates the JN engine's per-category plots." );
        return;
    }
    struct Row
    {
        int category;
        double physical, logical, resources;
    };
    if( !RegistryKnown( data, segment, time ) )
    {
        ImGui::TextDisabled( "The engine's registry had not been sampled yet at this time." );
        return;
    }
    std::vector<Row> rows;
    double total = 0;
    const auto& ledgers = data.categories[segment];
    const auto sample = time + CollectSlack;
    for( int c = 0; c < JnVram::Categories; ++c )
    {
        Row row { c,
            ledgers[int( JnVram::Ledger::Physical )][c].At( sample ).value_or( 0 ),
            ledgers[int( JnVram::Ledger::Logical )][c].At( sample ).value_or( 0 ),
            ledgers[int( JnVram::Ledger::Resources )][c].At( sample ).value_or( 0 ) };
        total += row.physical;
        if( row.physical != 0 || row.logical != 0 || row.resources != 0 ) rows.push_back( row );
    }
    std::stable_sort( rows.begin(), rows.end(), []( const Row& a, const Row& b ) { return a.physical > b.physical; } );
    if( rows.empty() )
    {
        ImGui::TextDisabled( "No GPU memory registered at this time." );
        return;
    }
    if( !ImGui::BeginTable( id, 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit ) ) return;
    ImGui::TableSetupColumn( "Category" );
    ImGui::TableSetupColumn( "Physical" );
    ImGui::TableSetupColumn( "Share" );
    ImGui::TableSetupColumn( "Logical" );
    ImGui::TableSetupColumn( "Resources" );
    ImGui::TableHeadersRow();
    for( const auto& row : rows )
    {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        SmallColorBox( CategoryColor( row.category ) );
        ImGui::SameLine();
        ImGui::TextUnformatted( JnVram::CategoryNames[row.category] );
        ImGui::TableNextColumn();
        ImGui::TextUnformatted( Bytes( row.physical ) );
        ImGui::TableNextColumn();
        if( total > 0 ) ImGui::Text( "%.1f%%", row.physical / total * 100 );
        ImGui::TableNextColumn();
        if( row.category == int( JnVram::Category::Heap ) ) ImGui::TextDisabled( "-" );
        else ImGui::TextUnformatted( Bytes( row.logical ) );
        ImGui::TableNextColumn();
        ImGui::TextUnformatted( RealToString( int64_t( row.resources ) ) );
    }
    ImGui::EndTable();
}

uint32_t TypeColor( VramResourceType type )
{
    switch( type )
    {
    case VramResourceType::Texture: return CategoryColor( int( JnVram::Category::Texture ) );
    case VramResourceType::RenderTarget: return CategoryColor( int( JnVram::Category::RenderTarget ) );
    case VramResourceType::DepthStencil: return CategoryColor( int( JnVram::Category::DepthStencil ) );
    case VramResourceType::Buffer: return CategoryColor( int( JnVram::Category::Buffer ) );
    case VramResourceType::RayTracing: return CategoryColor( int( JnVram::Category::RayTracing ) );
    case VramResourceType::Other: return CategoryColor( int( JnVram::Category::Other ) );
    default: return 0xFF888888;
    }
}

VramResourceType TypeOf( const VramResource& resource )
{
    return VramResourceTypeOf( resource.kind, resource.category );
}

const char* ResourceName( const VramResource& resource )
{
    return resource.pending ? "..." : resource.name.empty() ? "(no name)" : resource.name.c_str();
}

// The rows of the list the filters leave, grouped by name when asked: the
// rows of a name are then together, a group per name.
void FilterResources( const VramResources& list, VramPanelState& state )
{
    state.listed = &list;
    state.listedType = state.resourceType;
    state.listedFilter = state.resourceFilter.InputBuf;
    state.listedGroups = state.resourceGroups;
    state.listedUsedOnly = state.usedOnly && list.frameReferences;
    state.rows.clear();
    state.groups.clear();
    state.rowsSize = 0;
    for( uint32_t i = 0; i < uint32_t( list.resources.size() ); ++i )
    {
        const auto& resource = list.resources[i];
        if( state.listedUsedOnly && !resource.used ) continue;
        if( state.resourceType >= 0 && int( TypeOf( resource ) ) != state.resourceType ) continue;
        if( state.resourceFilter.IsActive() && ( resource.pending || !state.resourceFilter.PassFilter( resource.name.c_str() ) ) ) continue;
        state.rows.push_back( i );
        state.rowsSize += resource.size;
    }
    if( !state.resourceGroups ) return;
    std::stable_sort( state.rows.begin(), state.rows.end(), [&]( uint32_t a, uint32_t b ) { return list.resources[a].name < list.resources[b].name; } );
    for( uint32_t i = 0; i < uint32_t( state.rows.size() ); ++i )
    {
        const auto& resource = list.resources[state.rows[i]];
        if( state.groups.empty() || list.resources[state.rows[state.groups.back().first]].name != resource.name ) state.groups.push_back( { i, 0, 0 } );
        state.groups.back().count++;
        state.groups.back().size += resource.size;
    }
}

// Column ids of the resource list.
enum ResourceColumn : ImGuiID { NameColumn, TypeColumn, CountColumn, SizeColumn, ShareColumn, CreatedColumn };

// Orders the rows, or the groups, as the table's sort specs say.
void SortResources( const VramResources& list, VramPanelState& state, const ImGuiTableSortSpecs& specs )
{
    if( specs.SpecsCount == 0 ) return;
    const auto column = specs.Specs[0].ColumnUserID;
    const bool ascending = specs.Specs[0].SortDirection == ImGuiSortDirection_Ascending;
    const auto order = [&]( auto less ) { return [&, less]( const auto& a, const auto& b ) { return ascending ? less( a, b ) : less( b, a ); }; };
    if( state.listedGroups )
    {
        const auto firstOf = [&]( const VramPanelState::ResourceGroup& group ) -> const VramResource& { return list.resources[state.rows[group.first]]; };
        auto& groups = state.groups;
        if( column == NameColumn ) std::stable_sort( groups.begin(), groups.end(), order( [&]( const auto& a, const auto& b ) { return firstOf( a ).name < firstOf( b ).name; } ) );
        else if( column == TypeColumn ) std::stable_sort( groups.begin(), groups.end(), order( [&]( const auto& a, const auto& b ) { return TypeOf( firstOf( a ) ) < TypeOf( firstOf( b ) ); } ) );
        else if( column == CountColumn ) std::stable_sort( groups.begin(), groups.end(), order( []( const auto& a, const auto& b ) { return a.count < b.count; } ) );
        else std::stable_sort( groups.begin(), groups.end(), order( []( const auto& a, const auto& b ) { return a.size < b.size; } ) );
        return;
    }
    const auto at = [&]( uint32_t index ) -> const VramResource& { return list.resources[index]; };
    auto& rows = state.rows;
    if( column == NameColumn ) std::stable_sort( rows.begin(), rows.end(), order( [&]( uint32_t a, uint32_t b ) { return at( a ).name < at( b ).name; } ) );
    else if( column == TypeColumn ) std::stable_sort( rows.begin(), rows.end(), order( [&]( uint32_t a, uint32_t b ) { return TypeOf( at( a ) ) < TypeOf( at( b ) ); } ) );
    else if( column == CreatedColumn ) std::stable_sort( rows.begin(), rows.end(), order( [&]( uint32_t a, uint32_t b ) { return at( a ).created < at( b ).created; } ) );
    else std::stable_sort( rows.begin(), rows.end(), order( [&]( uint32_t a, uint32_t b ) { return at( a ).size < at( b ).size; } ) );
}

void ResourceTooltip( const VramResource& resource, const VramResources& list )
{
    ImGui::BeginTooltip();
    ImGui::TextUnformatted( ResourceName( resource ) );
    ImGui::Separator();
    TextFocused( "Type:", VramResourceTypeNames[int( TypeOf( resource ) )] );
    if( list.frameReferences )
    {
        char buf[64];
        snprintf( buf, sizeof( buf ), resource.used ? "used in frame %s" : "not used in frame %s", RealToString( int64_t( list.frameNumber ) ) );
        TextFocused( "GPU work:", buf );
    }
    if( resource.category < 0 ) TextDisabledUnformatted( "The engine did not record categories yet: render targets and depth buffers show as Texture." );
    TextFocused( "Size:", Bytes( double( resource.size ) ) );
    TextFocused( "Created:", TimeToString( resource.created ) );
    if( resource.freed ) TextFocused( "Freed:", TimeToString( *resource.freed ) );
    else TextDisabledUnformatted( "Not freed during the capture" );
    ImGui::EndTooltip();
}

// A segment's resources at a time, from the view's last list, which is
// another time's while this one loads: every resource or every name, by
// type and name.
void DrawResourceTable( const VramResources* list, VramPanelState& state, int segment, int64_t time, const std::function<void( int64_t, int64_t )>& zoomTo )
{
    if( !list || list->segment != segment )
    {
        ImGui::TextDisabled( "Loading the resources..." );
        return;
    }
    if( !list->present )
    {
        ImGui::TextDisabled( "No resource records: the capture has no GTMEM resource names." );
        return;
    }
    char buf[256];
    snprintf( buf, sizeof( buf ), "%s, %s", RealToString( int64_t( list->live ) ), Bytes( double( list->liveBytes ) ) );
    TextFocused( "Live resources:", buf );
    if( list->time != time )
    {
        ImGui::SameLine();
        ImGui::TextDisabled( "(at %s, updating)", TimeToString( list->time ) );
    }
    if( list->pending )
    {
        ImGui::SameLine();
        ImGui::TextDisabled( "looking up %s names", RealToString( int64_t( list->pending ) ) );
    }
    if( list->frameReferences )
    {
        char label[64];
        snprintf( label, sizeof( label ), "Used in frame %s:", RealToString( int64_t( list->frameNumber ) ) );
        snprintf( buf, sizeof( buf ), "%s, %s", RealToString( int64_t( list->used ) ), Bytes( double( list->usedBytes ) ) );
        TextFocused( label, buf );
        ImGui::SameLine();
        ImGui::TextDisabled( "(%s resources referenced in the frame)", RealToString( int64_t( list->referenced ) ) );
        if( ImGui::IsItemHovered() )
        {
            ImGui::BeginTooltip();
            TextFocused( "Frame:", TimeToString( list->frameBegin ) );
            ImGui::SameLine();
            ImGui::TextUnformatted( "-" );
            ImGui::SameLine();
            ImGui::TextUnformatted( TimeToString( list->frameEnd ) );
            ImGui::Separator();
            TextDisabledUnformatted( "The resources the frame's command lists referenced: each command list sends the set of resources" );
            TextDisabledUnformatted( "it accessed when it is closed ([GpuReference] Granularity=Frame). The render thread can close a" );
            TextDisabledUnformatted( "frame's lists during the next frame. References not in this list are in the other memory segment" );
            TextDisabledUnformatted( "or are not named resources, such as swap chain buffers." );
            ImGui::EndTooltip();
        }
    }
    if( list->resources.empty() ) return;

    // The filters: a type, with what the list holds of each, and names.
    std::array<uint64_t, VramResourceTypes> count {}, bytes {};
    for( const auto& resource : list->resources )
    {
        const auto type = int( TypeOf( resource ) );
        count[type]++;
        bytes[type] += resource.size;
    }
    const auto typeLabel = [&]( int type ) {
        static char label[128];
        if( type < 0 ) snprintf( label, sizeof( label ), "All types (%s)", RealToString( int64_t( list->resources.size() ) ) );
        else snprintf( label, sizeof( label ), "%s (%s, %s)", VramResourceTypeNames[type], RealToString( int64_t( count[type] ) ), Bytes( double( bytes[type] ) ) );
        return label;
    };
    if( state.resourceType >= 0 && count[state.resourceType] == 0 ) state.resourceType = -1;
    ImGui::SetNextItemWidth( ImGui::CalcTextSize( "RenderTarget (99,999, 9999.99 MB)" ).x + ImGui::GetFrameHeight() * 2 );
    if( ImGui::BeginCombo( "##vramResourceType", typeLabel( state.resourceType ) ) )
    {
        if( ImGui::Selectable( typeLabel( -1 ), state.resourceType < 0 ) ) state.resourceType = -1;
        for( int type = 0; type < VramResourceTypes; ++type )
        {
            if( count[type] == 0 ) continue;
            SmallColorBox( TypeColor( VramResourceType( type ) ) );
            ImGui::SameLine();
            if( ImGui::Selectable( typeLabel( type ), state.resourceType == type ) ) state.resourceType = type;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    state.resourceFilter.Draw( ICON_FA_FILTER " Filter names", 200 );
    if( state.resourceFilter.IsActive() )
    {
        ImGui::SameLine();
        if( ImGui::Button( ICON_FA_DELETE_LEFT " Clear" ) ) state.resourceFilter.Clear();
    }
    ImGui::SameLine();
    ImGui::Checkbox( "Group by name", &state.resourceGroups );
    if( list->frameReferences )
    {
        ImGui::SameLine();
        ImGui::Checkbox( "Used in this frame", &state.usedOnly );
        TooltipIfHovered( "Only the resources the GPU work of the frame at this time referenced." );
    }

    const bool rebuilt = state.listed != list || state.listedType != state.resourceType || state.listedFilter != state.resourceFilter.InputBuf ||
        state.listedGroups != state.resourceGroups || state.listedUsedOnly != ( state.usedOnly && list->frameReferences );
    if( rebuilt ) FilterResources( *list, state );
    const bool grouped = state.listedGroups;
    if( grouped ) snprintf( buf, sizeof( buf ), "%s names, %s resources, %s (%.1f%%)", RealToString( int64_t( state.groups.size() ) ),
        RealToString( int64_t( state.rows.size() ) ), Bytes( double( state.rowsSize ) ), list->liveBytes > 0 ? double( state.rowsSize ) / double( list->liveBytes ) * 100 : 0.0 );
    else snprintf( buf, sizeof( buf ), "%s resources, %s (%.1f%%)", RealToString( int64_t( state.rows.size() ) ), Bytes( double( state.rowsSize ) ),
        list->liveBytes > 0 ? double( state.rowsSize ) / double( list->liveBytes ) * 100 : 0.0 );
    TextFocused( "Shown:", buf );
    const auto rows = grouped ? state.groups.size() : state.rows.size();
    if( rows == 0 ) return;

    // About 15 rows, scrolled: rows with a button are a frame high.
    const auto rowHeight = grouped ? ImGui::GetTextLineHeightWithSpacing() : ImGui::GetFrameHeightWithSpacing();
    const ImVec2 size( 0, rowHeight * ( float( std::min<size_t>( rows, 15 ) ) + 1.5f ) );
    const auto flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable;
    if( !ImGui::BeginTable( grouped ? "##vramResourceNames" : "##vramResources", 5, flags, size ) ) return;
    ImGui::TableSetupScrollFreeze( 0, 1 );
    const auto ty = ImGui::GetTextLineHeight();
    ImGui::TableSetupColumn( "Name", ImGuiTableColumnFlags_WidthStretch, 0, NameColumn );
    ImGui::TableSetupColumn( "Type", ImGuiTableColumnFlags_WidthFixed,
        std::max( ImGui::CalcTextSize( "RenderTarget" ).x, ImGui::CalcTextSize( "DepthStencil" ).x ) + ty + ImGui::GetStyle().ItemSpacing.x * 2, TypeColumn );
    if( grouped ) ImGui::TableSetupColumn( "Count", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, std::max( ImGui::CalcTextSize( "99,999" ).x, ImGui::CalcTextSize( "Count" ).x + ty ), CountColumn );
    ImGui::TableSetupColumn( "Size", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending,
        ImGui::CalcTextSize( "1023.99 MB" ).x, SizeColumn );
    ImGui::TableSetupColumn( "Share", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, ImGui::CalcTextSize( "100.0%" ).x, ShareColumn );
    if( !grouped ) ImGui::TableSetupColumn( "Created", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize( "00:00:00.000" ).x + ty * 2, CreatedColumn );
    ImGui::TableHeadersRow();
    if( auto specs = ImGui::TableGetSortSpecs(); specs && ( specs->SpecsDirty || rebuilt ) )
    {
        SortResources( *list, state, *specs );
        specs->SpecsDirty = false;
    }
    const auto share = [&]( uint64_t value ) { if( list->liveBytes > 0 ) ImGui::Text( "%.1f%%", double( value ) / double( list->liveBytes ) * 100 ); };
    ImGuiListClipper clipper;
    clipper.Begin( int( rows ) );
    while( clipper.Step() )
    {
        for( int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row )
        {
            ImGui::PushID( row );
            ImGui::TableNextRow();
            if( grouped )
            {
                const auto& group = state.groups[row];
                const auto& first = list->resources[state.rows[group.first]];
                ImGui::TableNextColumn();
                if( first.pending || first.name.empty() ) ImGui::TextDisabled( "%s", ResourceName( first ) );
                else ImGui::TextUnformatted( first.name.c_str() );
                if( ImGui::IsItemHovered() )
                {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted( ResourceName( first ) );
                    ImGui::Separator();
                    snprintf( buf, sizeof( buf ), "%s, %s", RealToString( int64_t( group.count ) ), Bytes( double( group.size ) ) );
                    TextFocused( "Resources:", buf );
                    ImGui::EndTooltip();
                }
                ImGui::TableNextColumn();
                SmallColorBox( TypeColor( TypeOf( first ) ) );
                ImGui::SameLine();
                ImGui::TextUnformatted( VramResourceTypeNames[int( TypeOf( first ) )] );
                ImGui::TableNextColumn();
                ImGui::TextUnformatted( RealToString( int64_t( group.count ) ) );
                ImGui::TableNextColumn();
                ImGui::TextUnformatted( Bytes( double( group.size ) ) );
                ImGui::TableNextColumn();
                share( group.size );
            }
            else
            {
                const auto& resource = list->resources[state.rows[row]];
                ImGui::TableNextColumn();
                if( resource.pending || resource.name.empty() ) ImGui::TextDisabled( "%s", ResourceName( resource ) );
                else ImGui::TextUnformatted( resource.name.c_str() );
                if( ImGui::IsItemHovered() ) ResourceTooltip( resource, *list );
                ImGui::TableNextColumn();
                SmallColorBox( TypeColor( TypeOf( resource ) ) );
                ImGui::SameLine();
                ImGui::TextUnformatted( VramResourceTypeNames[int( TypeOf( resource ) )] );
                ImGui::TableNextColumn();
                ImGui::TextUnformatted( Bytes( double( resource.size ) ) );
                ImGui::TableNextColumn();
                share( resource.size );
                ImGui::TableNextColumn();
                ImGui::TextUnformatted( TimeToString( resource.created ) );
                ZoomButton( "created", resource.created, zoomTo );
            }
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

VramPanelState::Peak SeriesPeak( const VramSeries& series )
{
    VramPanelState::Peak peak;
    for( size_t i = 0; i < series.time.size(); ++i )
    {
        if( !peak.present || series.value[i] > peak.value )
        {
            peak.present = true;
            peak.value = series.value[i];
            peak.time = series.time[i];
        }
    }
    return peak;
}

// Walks the changes of usage and budget up to `end`, calling visit( from,
// to, usage, budget ) for each span both hold a value over.
template<class Visit>
void WalkShare( const VramSeries& usage, const VramSeries& budget, int64_t end, Visit visit )
{
    if( usage.Empty() || budget.Empty() ) return;
    size_t i = 0, j = 0;
    double u = Nan, b = Nan;
    for( int64_t t = std::min( usage.time[0], budget.time[0] ); t < end; )
    {
        while( i < usage.time.size() && usage.time[i] <= t ) u = usage.value[i++];
        while( j < budget.time.size() && budget.time[j] <= t ) b = budget.value[j++];
        int64_t next = end;
        if( i < usage.time.size() ) next = std::min( next, usage.time[i] );
        if( j < budget.time.size() ) next = std::min( next, budget.time[j] );
        if( !std::isnan( u ) && !std::isnan( b ) && b > 0 ) visit( t, next, u, b );
        t = next;
    }
}

void Summarize( const VramData& data, VramPanelState& state )
{
    state.summarized = &data;
    const auto end = LastPoint( data ) + 1;
    for( int s = 0; s < JnVram::Segments; ++s )
    {
        state.usagePeak[s] = SeriesPeak( data.usage[s] );
        state.physicalPeak[s] = SeriesPeak( data.physical[s] );
        state.budgetSharePeak[s] = {};
        state.above90[s] = state.aboveBudget[s] = 0;
        WalkShare( data.usage[s], data.budget[s], end, [&]( int64_t from, int64_t to, double u, double b ) {
            const auto share = u / b;
            auto& peak = state.budgetSharePeak[s];
            if( !peak.present || share > peak.value ) peak = { true, share, from };
            if( share >= 0.9 ) state.above90[s] += to - from;
            if( share >= 1 ) state.aboveBudget[s] += to - from;
        } );
        // Growth from the registry's first sample that held memory; a collect
        // sends its plots within microseconds, 250 ms apart.
        state.growth[s].clear();
        const auto start = RegistryStart( data, s );
        for( int c = 0; c < JnVram::Categories; ++c )
        {
            const auto& series = data.categories[s][int( JnVram::Ledger::Physical )][c];
            const auto peak = SeriesPeak( series );
            if( !peak.present || peak.value == 0 ) continue;
            const auto first = start ? series.At( *start + CollectSlack ).value_or( series.value.front() ) : series.value.front();
            state.growth[s].push_back( { c, first, series.value.back(), peak } );
        }
        std::stable_sort( state.growth[s].begin(), state.growth[s].end(), []( const auto& a, const auto& b ) { return a.last - a.first > b.last - b.first; } );
    }
    state.evictedPeak = SeriesPeak( data.evicted );
}

}

std::optional<VramResourceRecord> ParseVramResourceRecord( std::string_view text )
{
    const auto line = text.find( "GTMEM1|RESOURCE|" );
    if( line == std::string_view::npos ) return std::nullopt;
    text.remove_prefix( line );
    text = text.substr( 0, text.find( '\n' ) );
    const auto field = [&]( std::string_view key ) {
        const auto at = text.find( key );
        if( at == std::string_view::npos ) return std::string_view();
        const auto value = text.substr( at + key.size() );
        return value.substr( 0, value.find( '|' ) );
    };
    VramResourceRecord record;
    const auto allocation = field( "|allocation=" );
    if( allocation.empty() || std::from_chars( allocation.data(), allocation.data() + allocation.size(), record.allocation ).ec != std::errc() ) return std::nullopt;
    if( const auto kind = field( "|kind=" ); kind.size() == 1 ) record.kind = kind[0];
    if( const auto segment = field( "|segment=" ); segment.size() == 1 ) record.segment = segment[0];
    if( const auto category = field( "|category=" ); !category.empty() )
        for( int c = 0; c < JnVram::Categories; ++c )
            if( category == JnVram::CategoryNames[c] ) record.category = int8_t( c );
    return record;
}

char VramPoolKind( std::string_view pool )
{
    constexpr std::string_view Prefix = "GPU D3D12 Logical ";
    if( pool.substr( 0, Prefix.size() ) != Prefix ) return 0;
    pool.remove_prefix( Prefix.size() );
    if( pool == "Texture" ) return 'T';
    if( pool == "Buffer" ) return 'B';
    if( pool == "Upload" ) return 'U';
    if( pool == "Readback" ) return 'R';
    return 0;
}

int VramKindSegment( char kind )
{
    return kind == 'U' || kind == 'R' ? 1 : 0;
}

VramResourceType VramResourceTypeOf( char kind, int category )
{
    if( kind == 'U' ) return VramResourceType::Upload;
    if( kind == 'R' ) return VramResourceType::Readback;
    switch( category )
    {
    case int( JnVram::Category::Texture ): return VramResourceType::Texture;
    case int( JnVram::Category::RenderTarget ): return VramResourceType::RenderTarget;
    case int( JnVram::Category::DepthStencil ): return VramResourceType::DepthStencil;
    case int( JnVram::Category::Buffer ): return VramResourceType::Buffer;
    case int( JnVram::Category::RayTracing ): return VramResourceType::RayTracing;
    case int( JnVram::Category::Heap ):
    case int( JnVram::Category::Other ): return VramResourceType::Other;
    default: return kind == 'T' ? VramResourceType::Texture : kind == 'B' ? VramResourceType::Buffer : VramResourceType::Other;
    }
}

std::optional<double> VramSeries::At( int64_t t ) const
{
    const auto found = std::upper_bound( time.begin(), time.end(), t );
    if( found == time.begin() ) return std::nullopt;
    return value[size_t( found - time.begin() ) - 1];
}

VramSeries* VramData::Find( std::string_view name )
{
    if( name.substr( 0, 4 ) != "GPU." ) return nullptr;
    if( name == JnVram::EvictedBytesPlot ) return &evicted;
    if( name == JnUsage::GpuDedicatedUsedPlot ) return &adapter[0];
    if( name == JnUsage::GpuSharedUsedPlot ) return &adapter[1];
    if( name == JnUsage::GpuDedicatedTotalPlot ) return &adapterTotal;
    for( int s = 0; s < JnVram::Segments; ++s )
    {
        if( name == JnVram::DxgiUsagePlots[s] ) return &usage[s];
        if( name == JnVram::DxgiBudgetPlots[s] ) return &budget[s];
        if( name == JnVram::PhysicalTotalPlots[s] ) return &physical[s];
        if( name == JnVram::LogicalTotalPlots[s] ) return &logical[s];
        for( int l = 0; l < JnVram::Ledgers; ++l )
            for( int c = 0; c < JnVram::Categories; ++c )
                if( name == JnVram::PlotNames[s][l][c] ) return &categories[s][l][c];
    }
    return nullptr;
}

void VramData::Finish()
{
    const auto order = []( VramSeries& series ) {
        if( std::is_sorted( series.time.begin(), series.time.end() ) ) return;
        std::vector<size_t> index( series.time.size() );
        for( size_t i = 0; i < index.size(); ++i ) index[i] = i;
        std::stable_sort( index.begin(), index.end(), [&]( size_t a, size_t b ) { return series.time[a] < series.time[b]; } );
        VramSeries sorted;
        for( const auto i : index ) sorted.Add( series.time[i], series.value[i] );
        series = std::move( sorted );
    };
    for( int s = 0; s < JnVram::Segments; ++s )
    {
        order( usage[s] );
        order( budget[s] );
        order( physical[s] );
        order( logical[s] );
        order( adapter[s] );
        for( auto& ledger : categories[s] )
            for( auto& series : ledger ) order( series );
    }
    order( evicted );
    order( adapterTotal );
}

bool VramData::Empty() const
{
    for( int s = 0; s < JnVram::Segments; ++s ) if( HasSegment( s ) ) return false;
    return true;
}

bool VramData::HasSegment( int segment ) const
{
    return !usage[segment].Empty() || !physical[segment].Empty() || HasCategories( segment );
}

bool VramData::HasCategories( int segment ) const
{
    for( const auto& series : categories[segment][int( JnVram::Ledger::Physical )] )
        if( !series.Empty() ) return true;
    return false;
}

bool IsVramCategoryPlot( std::string_view name )
{
    if( name.substr( 0, 9 ) != "GPU.VRAM." ) return false;
    for( int s = 0; s < JnVram::Segments; ++s )
        for( int l = 0; l < JnVram::Ledgers; ++l )
            for( int c = 0; c < JnVram::Categories; ++c )
                if( name == JnVram::PlotNames[s][l][c] ) return true;
    return false;
}

const char* VramLaneLabel( int segment )
{
    return segment == 0 ? ICON_FA_MICROCHIP " GPU memory: video (Local)" : ICON_FA_MICROCHIP " GPU memory: system (NonLocal)";
}

std::optional<int64_t> DrawVramPlot( ImDrawList* draw, const VramPlotArea& area, const VramData& data, int segment )
{
    const int count = std::max( 1, int( std::ceil( area.width ) ) );
    const auto end = LastPoint( data );
    const bool categories = data.HasCategories( segment );
    const auto& physicalLedger = data.categories[segment][int( JnVram::Ledger::Physical )];
    std::vector<double> usage, budget, total, adapter;
    PlotPeakColumns( data.usage[segment], area, end, count, usage );
    PlotPeakColumns( data.budget[segment], area, end, count, budget );
    PlotPeakColumns( data.adapter[segment], area, end, count, adapter );
    std::array<std::vector<double>, JnVram::Categories> stack;
    std::array<bool, JnVram::Categories> shown {};
    if( categories )
    {
        total.assign( count, Nan );
        for( int c = 0; c < JnVram::Categories; ++c )
        {
            PlotPeakColumns( physicalLedger[c], area, end, count, stack[c] );
            for( int x = 0; x < count; ++x )
            {
                if( std::isnan( stack[c][x] ) ) continue;
                total[x] = ( std::isnan( total[x] ) ? 0 : total[x] ) + stack[c][x];
                shown[c] = shown[c] || stack[c][x] > 0;
            }
        }
    }
    else PlotPeakColumns( data.physical[segment], area, end, count, total );

    // From zero to the view's largest value; the budget counts once usage
    // comes within a quarter of it, so a distant budget does not flatten the
    // plot.
    double peak = 0, peakBudget = 0;
    for( int x = 0; x < count; ++x )
    {
        if( !std::isnan( usage[x] ) ) peak = std::max( peak, usage[x] );
        if( !std::isnan( total[x] ) ) peak = std::max( peak, total[x] );
        if( !std::isnan( adapter[x] ) ) peak = std::max( peak, adapter[x] );
        if( !std::isnan( budget[x] ) ) peakBudget = std::max( peakBudget, budget[x] );
    }
    if( peak >= peakBudget / 4 ) peak = std::max( peak, peakBudget );
    const auto top = peak > 0 ? peak * 1.05 : 1.0;
    const auto scale = double( area.height ) / top;

    const auto bottom = area.pos + ImVec2( area.width, area.height );
    draw->PushClipRect( area.pos, bottom, true );
    draw->AddRectFilled( area.pos, bottom, 0x22000000 | ( DarkenColorMore( PhysicalColor ) & 0xFFFFFF ) );
    std::vector<float> low( count, area.height ), high( count );
    if( categories )
    {
        for( int c = 0; c < JnVram::Categories; ++c )
        {
            if( !shown[c] ) continue;
            for( int x = 0; x < count; ++x )
            {
                const auto value = std::isnan( stack[c][x] ) ? 0 : stack[c][x];
                high[x] = low[x] - float( value * scale );
            }
            PlotFillColumns( draw, area, low, high, CategoryColor( c, 0xCC ) );
            low.swap( high );
        }
    }
    else
    {
        for( int x = 0; x < count; ++x ) high[x] = std::isnan( total[x] ) ? area.height : area.height - float( total[x] * scale );
        PlotFillColumns( draw, area, low, high, 0x66000000 | ( PhysicalColor & 0xFFFFFF ) );
        PlotLineColumns( draw, area, total, scale, PhysicalColor, 1.f );
    }
    PlotLineColumns( draw, area, adapter, scale, AdapterColor, 1.f );
    PlotLineColumns( draw, area, budget, scale, BudgetColor, 1.5f );
    PlotLineColumns( draw, area, usage, scale, UsageColor, 1.5f );

    // Tracy's plot labels: the range on the left.
    const auto ty = ImGui::GetTextLineHeight();
    DrawTextSuperContrast( draw, area.pos, PhysicalColor, Bytes( top ) );
    DrawTextSuperContrast( draw, ImVec2( area.pos.x, bottom.y - ty ), PhysicalColor, "0" );
    draw->PopClipRect();

    // The legend, right-aligned on the header row; categories in stack order
    // from the left, as far as the row has room.
    const auto legendY = area.pos.y - ty - 1;
    float x = bottom.x - ty * 0.5f;
    const bool adapterShown = std::any_of( adapter.begin(), adapter.end(), []( double v ) { return !std::isnan( v ); } );
    if( ( !adapterShown || PlotLegendEntry( draw, x, area.legendMinX, legendY, AdapterColor, "All processes", true ) ) &&
        PlotLegendEntry( draw, x, area.legendMinX, legendY, BudgetColor, "Budget", true ) &&
        PlotLegendEntry( draw, x, area.legendMinX, legendY, UsageColor, "DXGI usage", true ) )
    {
        if( categories )
        {
            for( int c = JnVram::Categories - 1; c >= 0; --c )
                if( shown[c] && !PlotLegendEntry( draw, x, area.legendMinX, legendY, CategoryColor( c ), JnVram::CategoryNames[c], false ) ) break;
        }
        else PlotLegendEntry( draw, x, area.legendMinX, legendY, PhysicalColor, "Engine physical", false );
    }

    if( !area.hover || !ImGui::IsMouseHoveringRect( area.pos, bottom ) ) return std::nullopt;
    const auto mouseX = ImGui::GetMousePos().x;
    const auto time = area.begin + int64_t( double( mouseX - area.pos.x ) / area.pxns );
    draw->AddLine( ImVec2( mouseX, area.pos.y ), ImVec2( mouseX, bottom.y ), 0x99FFFFFF );
    VramTooltip( data, segment, time );
    return time;
}

void VramTooltip( const VramData& data, int segment, int64_t time )
{
    ImGui::BeginTooltip();
    ImGui::TextUnformatted( VramLaneLabel( segment ) );
    ImGui::Separator();
    TextFocused( "Time:", TimeToString( time ) );
    DrawLedgers( data, segment, time, false );
    DrawCategoryTable( data, segment, time, "##vramTooltip" );
    ImGui::Separator();
    TextDisabledUnformatted( "Click to show this time in the GPU memory window" );
    ImGui::EndTooltip();
}

void VramLaneTooltip( const VramData& data, int segment )
{
    ImGui::BeginTooltip();
    ImGui::TextUnformatted( VramLaneLabel( segment ) );
    ImGui::Separator();
    const auto usage = SeriesPeak( data.usage[segment] );
    if( usage.present )
    {
        TextFocused( "Peak DXGI usage:", Bytes( usage.value ) );
        ImGui::SameLine();
        ImGui::TextDisabled( "at %s", TimeToString( usage.time ) );
        if( const auto budget = data.budget[segment].At( usage.time + CollectSlack ); budget && *budget > 0 )
        {
            char buf[64];
            snprintf( buf, sizeof( buf ), "%.1f%%", usage.value / *budget * 100 );
            TextFocused( "Of the budget:", buf );
        }
    }
    const auto physical = SeriesPeak( data.physical[segment] );
    if( physical.present ) TextFocused( "Peak engine physical:", Bytes( physical.value ) );
    ImGui::Separator();
    TextDisabledUnformatted( "Stacked: the engine's physical bytes by category." );
    TextDisabledUnformatted( "Lines: what DXGI reports for the process, its budget, and the adapter's use by all processes." );
    TextDisabledUnformatted( "The engine sends the values every 250 ms while they change." );
    ImGui::EndTooltip();
}

void DrawVramPanel( const VramData& data, VramPanelState& state, int64_t viewCenter, const VramPanelActions& actions )
{
    const auto& zoomTo = actions.zoomTo;
    if( state.summarized != &data ) Summarize( data, state );
    for( int s = 0; s < JnVram::Segments; ++s )
    {
        if( s ) ImGui::SameLine();
        ImGui::BeginDisabled( !data.HasSegment( s ) );
        if( ImGui::RadioButton( SegmentLabel( s ), state.segment == s ) ) state.segment = s;
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    ImGui::Checkbox( "Follow timeline", &state.follow );
    TooltipIfHovered( "Show the middle of the timeline's view; a click on a GPU memory lane picks a time instead." );
    if( actions.showLanes )
    {
        ImGui::SameLine();
        if( ImGui::SmallButton( ICON_FA_ARROW_DOWN " Show in timeline" ) ) actions.showLanes();
        TooltipIfHovered( "Scroll the timeline to its GPU memory lanes, under the GPU lanes." );
    }
    const auto segment = state.segment;
    const auto time = state.follow ? viewCenter : state.time;

    ImGui::SeparatorText( "At this time" );
    TextFocused( "Time:", TimeToString( time ) );
    if( !state.follow ) ZoomButton( "time", time, zoomTo );
    DrawLedgers( data, segment, time, true );
    DrawCategoryTable( data, segment, time, "##vramPanel" );
    if( actions.resources ) DrawResourceTable( actions.resources( segment, time ).get(), state, segment, time, zoomTo );

    ImGui::SeparatorText( "Over the capture" );
    char buf[256];
    if( const auto& peak = state.usagePeak[segment]; peak.present )
    {
        snprintf( buf, sizeof( buf ), "%s at %s", Bytes( peak.value ), TimeToString( peak.time ) );
        TextFocused( "Peak DXGI usage:", buf );
        ZoomButton( "usage", peak.time, zoomTo );
    }
    if( const auto& peak = state.budgetSharePeak[segment]; peak.present )
    {
        snprintf( buf, sizeof( buf ), "%.1f%% at %s", peak.value * 100, TimeToString( peak.time ) );
        TextFocused( "Peak share of the budget:", buf );
        ZoomButton( "share", peak.time, zoomTo );
        TextFocused( "At or above 90% of the budget:", TimeToString( state.above90[segment] ) );
        ImGui::SameLine();
        TextFocused( "over the budget:", TimeToString( state.aboveBudget[segment] ) );
    }
    if( const auto& peak = state.physicalPeak[segment]; peak.present )
    {
        snprintf( buf, sizeof( buf ), "%s at %s", Bytes( peak.value ), TimeToString( peak.time ) );
        TextFocused( "Peak engine physical:", buf );
        ZoomButton( "physical", peak.time, zoomTo );
    }
    if( segment == 0 && state.evictedPeak.present && state.evictedPeak.value > 0 )
    {
        snprintf( buf, sizeof( buf ), "%s at %s", Bytes( state.evictedPeak.value ), TimeToString( state.evictedPeak.time ) );
        TextFocused( "Peak evicted:", buf );
        ZoomButton( "evicted", state.evictedPeak.time, zoomTo );
    }
    const auto& growth = state.growth[segment];
    if( !growth.empty() && ImGui::BeginTable( "##vramGrowth", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit ) )
    {
        ImGui::TableSetupColumn( "Category" );
        ImGui::TableSetupColumn( "Physical at start" );
        ImGui::TableSetupColumn( "At end" );
        ImGui::TableSetupColumn( "Change" );
        ImGui::TableSetupColumn( "Peak" );
        ImGui::TableSetupColumn( "Peak time" );
        ImGui::TableHeadersRow();
        for( const auto& row : growth )
        {
            ImGui::PushID( row.category );
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            SmallColorBox( CategoryColor( row.category ) );
            ImGui::SameLine();
            ImGui::TextUnformatted( JnVram::CategoryNames[row.category] );
            ImGui::TableNextColumn();
            ImGui::TextUnformatted( Bytes( row.first ) );
            ImGui::TableNextColumn();
            ImGui::TextUnformatted( Bytes( row.last ) );
            ImGui::TableNextColumn();
            const auto change = row.last - row.first;
            if( change > 0 ) ImGui::TextColored( ImVec4( 1.f, .6f, .4f, 1.f ), "+%s", Bytes( change ) );
            else ImGui::TextUnformatted( Bytes( change ) );
            ImGui::TableNextColumn();
            ImGui::TextUnformatted( Bytes( row.peak.value ) );
            ImGui::TableNextColumn();
            ImGui::TextUnformatted( TimeToString( row.peak.time ) );
            ZoomButton( "peak", row.peak.time, zoomTo );
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if( ImGui::TreeNode( "How to read these values" ) )
    {
        TextDisabledUnformatted( "Local is video memory; NonLocal is system memory the GPU reaches. DXGI budgets them separately, so never add the two." );
        TextDisabledUnformatted( "DXGI usage is what the driver reports for the process. Engine physical is what the engine allocated from D3D12;" );
        TextDisabledUnformatted( "engine logical is the resources inside, so aliased placed resources can exceed it." );
        TextDisabledUnformatted( "Heap is heaps holding placed resources; their resources count under their own category in the logical bytes." );
        TextDisabledUnformatted( "Other is memory without a resource. The engine sends each value every 250 ms while it changes." );
        TextDisabledUnformatted( "Evicted is engine memory the residency manager moved out of video memory: DXGI usage no longer counts it." );
        TextDisabledUnformatted( "The resources are the engine's named textures and buffers at their logical size; placed resources share their heap." );
        TextDisabledUnformatted( "A resource created before the capture started shows when the capture connected as its creation time." );
        ImGui::TreePop();
    }
}

}

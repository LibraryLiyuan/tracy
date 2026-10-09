#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdio.h>
#include <string.h>
#include <string>

#include "TracyColor.hpp"
#include "TracyImGui.hpp"
#include "TracyMouse.hpp"
#include "TracyUsage.hpp"
#include "../../../server/TracyPrint.hpp"

namespace tracy
{

namespace
{

constexpr double Nan = std::numeric_limits<double>::quiet_NaN();
constexpr uint32_t MachineColor = 0xFFAAAAAA;
constexpr uint32_t GameColor = 0xFF44AAFF;
constexpr uint32_t CoresColor = 0xFFDDAA55;
constexpr uint32_t WorkingSetColor = 0xFFEEEEEE;
constexpr uint32_t InstalledColor = 0xFF5555EE;
// A point further than this many sampling intervals from the previous one
// follows a gap in the samples, such as a reconnect: it covers one interval.
constexpr int64_t GapIntervals = 5;
// FPS against the target: met, below, below half; the slowest frame's FPS;
// the target.
constexpr uint32_t FpsMetColor = 0xFF22DD22;
constexpr uint32_t FpsBelowColor = 0xFF22DDDD;
constexpr uint32_t FpsPoorColor = 0xFF2222DD;
constexpr uint32_t FpsSlowestColor = 0xFF4488FF;
constexpr uint32_t FpsLineColor = 0xFFE8E8E8;
constexpr uint32_t FpsTargetColor = 0xDDFFFFFF;

uint32_t WithAlpha( uint32_t color, uint32_t alpha )
{
    return ( color & 0xFFFFFF ) | ( alpha << 24 );
}

// TracyJnUsage.hpp's colors are 0xRRGGBB, ImGui's 0xAABBGGRR.
uint32_t EngineColor( int engine, uint32_t alpha = 0xFF )
{
    const auto rgb = JnUsage::EngineColors[engine];
    return ( alpha << 24 ) | ( ( rgb & 0xFF ) << 16 ) | ( rgb & 0xFF00 ) | ( rgb >> 16 );
}

const char* Bytes( double value )
{
    return MemSizeToString( int64_t( std::llround( value ) ) );
}

const char* Percent( double value )
{
    static char buf[4][32];
    static int next = 0;
    auto& out = buf[next++ % 4];
    snprintf( out, sizeof( out ), "%.1f%%", value );
    return out;
}

bool StartsWith( std::string_view text, std::string_view prefix )
{
    return text.substr( 0, prefix.size() ) == prefix;
}

bool EndsWith( std::string_view text, std::string_view suffix )
{
    return text.size() >= suffix.size() && text.substr( text.size() - suffix.size() ) == suffix;
}

void Order( VramSeries& series )
{
    if( std::is_sorted( series.time.begin(), series.time.end() ) ) return;
    std::vector<size_t> index( series.time.size() );
    for( size_t i = 0; i < index.size(); ++i ) index[i] = i;
    std::stable_sort( index.begin(), index.end(), [&]( size_t a, size_t b ) { return series.time[a] < series.time[b]; } );
    VramSeries sorted;
    for( const auto i : index ) sorted.Add( series.time[i], series.value[i] );
    series = std::move( sorted );
}

// Where the interval a percentage point averages starts.
int64_t IntervalStart( const VramSeries& series, size_t i, int64_t interval )
{
    const auto end = series.time[i];
    if( i == 0 ) return end - interval;
    return std::max( series.time[i - 1], end - GapIntervals * interval );
}

// The percentage holding at t: the point whose interval holds t.
std::optional<double> AverageAt( const VramSeries& series, int64_t interval, int64_t t )
{
    const auto i = size_t( std::lower_bound( series.time.begin(), series.time.end(), t ) - series.time.begin() );
    if( i == series.time.size() || t <= IntervalStart( series, i, interval ) ) return std::nullopt;
    return series.value[i];
}

// The value at t on the line through the middles of a point's interval and
// of its neighbour's on t's side, or the point's own value without an
// adjacent neighbour there.
double Interpolated( const VramSeries& series, size_t i, int64_t interval, int64_t t )
{
    const auto start = IntervalStart( series, i, interval );
    const auto middle = start + ( series.time[i] - start ) / 2;
    size_t j = i;
    if( t < middle && i > 0 && start == series.time[i - 1] ) j = i - 1;
    else if( t > middle && i + 1 < series.time.size() && IntervalStart( series, i + 1, interval ) == series.time[i] ) j = i + 1;
    if( j == i ) return series.value[i];
    const auto otherStart = IntervalStart( series, j, interval );
    const auto otherMiddle = otherStart + ( series.time[j] - otherStart ) / 2;
    const auto f = double( t - middle ) / double( otherMiddle - middle );
    return series.value[i] + ( series.value[j] - series.value[i] ) * f;
}

// Per pixel column of the area, the time-weighted average of a percentage
// series over the column; zoomed in past the samples, the line between them.
// NaN where no interval covers the column.
void AverageColumns( const VramSeries& series, const VramPlotArea& area, int64_t interval, int count, std::vector<double>& out )
{
    out.assign( count, Nan );
    if( series.Empty() ) return;
    const auto nsPerPixel = 1.0 / area.pxns;
    // The first point whose interval ends after the area's begin.
    size_t i = size_t( std::upper_bound( series.time.begin(), series.time.end(), area.begin ) - series.time.begin() );
    for( int c = 0; c < count && i < series.time.size(); ++c )
    {
        const auto c0 = area.begin + int64_t( c * nsPerPixel );
        const auto c1 = area.begin + int64_t( ( c + 1 ) * nsPerPixel );
        double sum = 0, covered = 0;
        int intervals = 0;
        size_t only = i;
        while( i < series.time.size() )
        {
            const auto start = IntervalStart( series, i, interval );
            const auto end = series.time[i];
            if( start >= c1 ) break;
            const auto overlap = double( std::min( end, c1 ) - std::max( start, c0 ) );
            if( overlap > 0 )
            {
                sum += series.value[i] * overlap;
                covered += overlap;
                intervals++;
                only = i;
            }
            if( end > c1 ) break;
            ++i;
        }
        if( covered <= 0 ) continue;
        out[c] = intervals == 1 ? Interpolated( series, only, interval, c0 + ( c1 - c0 ) / 2 ) : sum / covered;
    }
}

// AverageColumns of a series of the data, kept in the data.
const std::vector<double>& Averages( const UsageData& data, const VramSeries& series, const VramPlotArea& area, int count )
{
    auto& cached = data.columns[&series];
    if( cached.begin != area.begin || cached.pxns != area.pxns || cached.values.size() != size_t( count ) )
    {
        cached.begin = area.begin;
        cached.pxns = area.pxns;
        AverageColumns( series, area, data.interval, count, cached.values );
    }
    return cached.values;
}

// Fills the columns from the bottom of the area up to their values.
void FillFromBottom( ImDrawList* draw, const VramPlotArea& area, const std::vector<double>& values, double scale, uint32_t color )
{
    std::vector<float> low( values.size(), area.height ), high( values.size() );
    for( size_t x = 0; x < values.size(); ++x )
        high[x] = std::isnan( values[x] ) ? area.height : area.height - float( std::min( values[x] * scale, double( area.height ) ) );
    PlotFillColumns( draw, area, low, high, color );
}

VramPlotArea SubArea( const VramPlotArea& area, float top, float height )
{
    auto sub = area;
    sub.pos.y += top;
    sub.height = height;
    return sub;
}

void RangeLabels( ImDrawList* draw, const VramPlotArea& area, uint32_t color, const char* top )
{
    const auto ty = ImGui::GetTextLineHeight();
    DrawTextSuperContrast( draw, area.pos, color, top );
    if( area.height >= ty * 2 ) DrawTextSuperContrast( draw, ImVec2( area.pos.x, area.pos.y + area.height - ty ), color, "0" );
}

void Background( ImDrawList* draw, const VramPlotArea& area, uint32_t color )
{
    draw->AddRectFilled( area.pos, area.pos + ImVec2( area.width, area.height ), 0x22000000 | ( DarkenColorMore( color ) & 0xFFFFFF ) );
}

// The largest value of the series in the view, from the columns.
double ColumnPeak( const std::vector<double>& values )
{
    double peak = 0;
    for( const auto v : values ) if( !std::isnan( v ) ) peak = std::max( peak, v );
    return peak;
}

// A logical processor's busy share as a colour: dark when idle, then green,
// yellow and red at full load.
uint32_t HeatColor( double busy )
{
    struct Stop
    {
        double at;
        int r, g, b;
    };
    static constexpr Stop stops[] = { { 0, 34, 52, 44 }, { 25, 40, 130, 70 }, { 50, 110, 195, 60 }, { 75, 235, 200, 50 }, { 100, 235, 70, 50 } };
    busy = std::clamp( busy, 0.0, 100.0 );
    int i = 0;
    while( i < 3 && busy > stops[i + 1].at ) i++;
    const auto& a = stops[i];
    const auto& b = stops[i + 1];
    const auto f = ( busy - a.at ) / ( b.at - a.at );
    const auto mix = [f]( int from, int to ) { return uint32_t( std::lround( from + ( to - from ) * f ) ); };
    return 0xFF000000 | ( mix( a.b, b.b ) << 16 ) | ( mix( a.g, b.g ) << 8 ) | mix( a.r, b.r );
}

// Fills each column of the row with its value's heat colour, in runs of one
// colour (twenty steps), leaving columns without a value empty.
void HeatColumns( ImDrawList* draw, const VramPlotArea& row, const std::vector<double>& values )
{
    const int count = int( values.size() );
    const auto step = []( double v ) { return std::isnan( v ) ? -1 : int( std::lround( std::clamp( v, 0.0, 100.0 ) / 5 ) ); };
    for( int c = 0; c < count; )
    {
        const auto level = step( values[c] );
        int e = c + 1;
        while( e < count && step( values[e] ) == level ) e++;
        if( level >= 0 ) draw->AddRectFilled( ImVec2( row.pos.x + float( c ), row.pos.y ), ImVec2( row.pos.x + float( e ), row.pos.y + row.height ), HeatColor( level * 5.0 ) );
        c = e;
    }
}

// The heat map's legend entry, ending at x: a colour ramp from idle to full,
// labelled; false when it would cross minX.
bool HeatLegend( ImDrawList* draw, float& x, float minX, float y )
{
    const auto ty = ImGui::GetTextLineHeight();
    const char* label = "Cores 0-100%";
    const auto width = ImGui::CalcTextSize( label ).x;
    const auto ramp = ty * 2.5f;
    if( x - width - ramp - ty * 0.6f < minX ) return false;
    x -= width;
    DrawTextContrast( draw, ImVec2( x, y ), 0xFFCCCCCC, label );
    x -= ty * 0.3f + ramp;
    constexpr int Steps = 10;
    for( int i = 0; i < Steps; ++i )
        draw->AddRectFilled( ImVec2( x + ramp * i / Steps, y + ty * 0.2f ), ImVec2( x + ramp * ( i + 1 ) / Steps, y + ty * 0.85f ), HeatColor( 100.0 * ( i + 0.5 ) / Steps ) );
    x -= ty * 0.8f;
    return true;
}

struct CpuLayout
{
    float usage = 0, gap = 0, row = 0;
    int rows = 0;
};

CpuLayout LayoutCpu( const UsageData& data, float plotHeight )
{
    CpuLayout layout;
    const bool usage = !data.cpuSystem.Empty() || !data.cpuProcess.Empty() || !data.tracyCpu.Empty();
    if( usage ) layout.usage = plotHeight * 0.5f;
    if( data.HasCores() )
    {
        layout.rows = int( data.cores.size() );
        layout.row = std::clamp( plotHeight * 1.8f / float( layout.rows ), plotHeight * 0.06f, plotHeight * 0.12f );
        if( usage ) layout.gap = plotHeight * 0.05f;
    }
    return layout;
}

struct MemoryLayout
{
    float game = 0, gap = 0, machine = 0;
};

MemoryLayout LayoutMemory( const UsageData& data, float plotHeight )
{
    MemoryLayout layout;
    const bool game = !data.processCommitted.Empty() || !data.processWorkingSet.Empty();
    const bool machine = !data.systemUsed.Empty();
    if( game ) layout.game = plotHeight * ( machine ? 0.45f : 0.6f );
    if( machine ) layout.machine = plotHeight * ( game ? 0.3f : 0.6f );
    if( game && machine ) layout.gap = plotHeight * 0.05f;
    return layout;
}

const VramSeries& MachineCpu( const UsageData& data )
{
    return data.cpuSystem.Empty() ? data.tracyCpu : data.cpuSystem;
}

struct Peak
{
    bool present = false;
    double value = 0;
    int64_t time = 0;
};

Peak SeriesPeak( const VramSeries& series )
{
    Peak peak;
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

double SeriesMean( const VramSeries& series )
{
    if( series.Empty() ) return 0;
    double sum = 0;
    for( const auto v : series.value ) sum += v;
    return sum / double( series.value.size() );
}

void PeakLine( const char* label, const Peak& peak, bool bytes )
{
    if( !peak.present ) return;
    TextFocused( label, bytes ? Bytes( peak.value ) : Percent( peak.value ) );
    ImGui::SameLine();
    ImGui::TextDisabled( "at %s", TimeToString( peak.time ) );
}

int64_t TimeUnderMouse( const VramPlotArea& area )
{
    return area.begin + int64_t( double( ImGui::GetMousePos().x - area.pos.x ) / area.pxns );
}

std::optional<int64_t> DrawCpu( ImDrawList* draw, const VramPlotArea& area, const UsageData& data, float plotHeight )
{
    const int count = std::max( 1, int( std::ceil( area.width ) ) );
    const auto layout = LayoutCpu( data, plotHeight );
    const auto& machine = MachineCpu( data );
    const bool hybrid = data.Hybrid();
    const auto bottom = area.pos + ImVec2( area.width, area.height );
    draw->PushClipRect( area.pos, bottom, true );
    if( layout.usage > 0 )
    {
        const auto sub = SubArea( area, 0, layout.usage );
        Background( draw, sub, GameColor );
        // A grid line at half load.
        const auto half = std::round( sub.pos.y + sub.height * 0.5f ) + 0.5f;
        draw->AddLine( ImVec2( sub.pos.x, half ), ImVec2( sub.pos.x + sub.width, half ), 0x20FFFFFF );
        const auto& machineColumns = Averages( data, machine, sub, count );
        const auto& gameColumns = Averages( data, data.cpuProcess, sub, count );
        const auto scale = double( sub.height ) / 100.0;
        FillFromBottom( draw, sub, machineColumns, scale, WithAlpha( MachineColor, 0x50 ) );
        FillFromBottom( draw, sub, gameColumns, scale, WithAlpha( GameColor, 0x90 ) );
        PlotLineColumns( draw, sub, machineColumns, scale, MachineColor, 1.f );
        PlotLineColumns( draw, sub, gameColumns, scale, GameColor, 1.5f );
        RangeLabels( draw, sub, MachineColor, "100%" );
        if( sub.height >= ImGui::GetTextLineHeight() * 3 ) DrawTextSuperContrast( draw, ImVec2( sub.pos.x, half - ImGui::GetTextLineHeight() * 0.5f ), 0x80FFFFFF, "50%" );
    }
    if( layout.rows > 0 )
    {
        // The logical processors as a heat map: a row each, coloured by how
        // busy it was, labelled P or E on a hybrid CPU, with a line between
        // the two kinds.
        const auto top = layout.usage + layout.gap;
        Background( draw, SubArea( area, top, layout.row * float( layout.rows ) ), CoresColor );
        const auto labelSize = std::min( layout.row, ImGui::GetTextLineHeight() * 0.85f );
        const bool labels = labelSize >= ImGui::GetTextLineHeight() * 0.5f;
        char label[16];
        for( int core = 0; core < layout.rows; ++core )
        {
            const auto row = SubArea( area, top + layout.row * float( core ), std::max( 1.f, layout.row - 1 ) );
            if( row.pos.y > bottom.y ) break;
            const auto& columns = Averages( data, data.cores[core], row, count );
            HeatColumns( draw, row, columns );
            const bool efficiency = data.EfficiencyCore( core );
            if( hybrid && core > 0 && efficiency != data.EfficiencyCore( core - 1 ) )
                draw->AddLine( ImVec2( row.pos.x, row.pos.y - 1 ), ImVec2( row.pos.x + row.width, row.pos.y - 1 ), 0xA0FFFFFF );
            if( labels )
            {
                snprintf( label, sizeof( label ), hybrid ? ( efficiency ? "E%d" : "P%d" ) : "%d", core );
                const auto pos = row.pos + ImVec2( 2, ( row.height - labelSize ) * 0.5f );
                draw->AddText( ImGui::GetFont(), labelSize, pos + ImVec2( 1, 1 ), 0xC0000000, label );
                draw->AddText( ImGui::GetFont(), labelSize, pos, 0xE0FFFFFF, label );
            }
        }
    }
    draw->PopClipRect();

    // The legend, right-aligned on the header row: the game's and the
    // machine's load at the dashed line, then the heat map's colours.
    const auto ty = ImGui::GetTextLineHeight();
    const auto legendY = area.pos.y - ty - 1;
    float x = bottom.x - ty * 0.5f;
    bool room = true;
    const auto middle = area.begin + int64_t( double( area.width ) * 0.5 / area.pxns );
    char text[64];
    if( !data.cpuProcess.Empty() )
    {
        const auto value = AverageAt( data.cpuProcess, data.interval, middle );
        snprintf( text, sizeof( text ), value ? "Game %.0f%%" : "Game", value.value_or( 0 ) );
        room = PlotLegendEntry( draw, x, area.legendMinX, legendY, GameColor, text, false );
    }
    if( room && !machine.Empty() )
    {
        const auto value = AverageAt( machine, data.interval, middle );
        snprintf( text, sizeof( text ), value ? "Machine %.0f%%" : "Machine", value.value_or( 0 ) );
        room = PlotLegendEntry( draw, x, area.legendMinX, legendY, MachineColor, text, false );
    }
    if( room && layout.rows > 0 ) HeatLegend( draw, x, area.legendMinX, legendY );

    if( !area.hover || !ImGui::IsMouseHoveringRect( area.pos, bottom ) ) return std::nullopt;
    const auto mouse = ImGui::GetMousePos();
    const auto time = TimeUnderMouse( area );
    draw->AddLine( ImVec2( mouse.x, area.pos.y ), ImVec2( mouse.x, bottom.y ), 0x99FFFFFF );
    // The core row under the mouse, if any.
    int hovered = -1;
    if( layout.rows > 0 )
    {
        const auto rowY = mouse.y - area.pos.y - layout.usage - layout.gap;
        if( rowY >= 0 ) hovered = std::min( layout.rows - 1, int( rowY / layout.row ) );
    }
    ImGui::BeginTooltip();
    ImGui::TextUnformatted( UsageLaneLabel( UsageLane::Cpu ) );
    ImGui::Separator();
    TextFocused( "Time:", TimeToString( time ) );
    if( hovered >= 0 )
    {
        char label[64];
        snprintf( label, sizeof( label ), "CPU %d%s:", hovered, !hybrid ? "" : data.EfficiencyCore( hovered ) ? " (efficiency core)" : " (performance core)" );
        if( const auto value = AverageAt( data.cores[hovered], data.interval, time ) ) TextFocused( label, Percent( *value ) );
        else
        {
            ImGui::TextDisabled( "%s", label );
            ImGui::SameLine();
            ImGui::TextDisabled( "not recorded at this time" );
        }
    }
    if( const auto value = AverageAt( machine, data.interval, time ) ) TextFocused( "Machine:", Percent( *value ) );
    if( const auto value = AverageAt( data.cpuProcess, data.interval, time ) )
    {
        TextFocused( "Game:", Percent( *value ) );
        if( !data.cores.empty() )
        {
            ImGui::SameLine();
            ImGui::TextDisabled( "(%.1f of %zu logical processors)", *value / 100.0 * double( data.cores.size() ), data.cores.size() );
        }
    }
    if( layout.rows > 0 )
    {
        // The busiest cores, and each core type's average.
        std::vector<std::pair<double, int>> busy;
        double sums[2] = {};
        int counts[2] = {};
        for( int core = 0; core < layout.rows; ++core )
        {
            const auto value = AverageAt( data.cores[core], data.interval, time );
            if( !value ) continue;
            busy.emplace_back( *value, core );
            const int type = data.EfficiencyCore( core ) ? 1 : 0;
            sums[type] += *value;
            counts[type]++;
        }
        std::sort( busy.begin(), busy.end(), []( const auto& a, const auto& b ) { return a.first > b.first; } );
        if( !busy.empty() )
        {
            std::string text;
            char item[32];
            for( size_t i = 0; i < std::min<size_t>( busy.size(), 4 ); ++i )
            {
                snprintf( item, sizeof( item ), "%sCPU %d %.0f%%", i ? ", " : "", busy[i].second, busy[i].first );
                text += item;
            }
            TextFocused( "Busiest:", text.c_str() );
        }
        if( hybrid )
        {
            if( counts[0] ) TextFocused( "P-cores average:", Percent( sums[0] / counts[0] ) );
            if( counts[1] )
            {
                if( counts[0] ) ImGui::SameLine();
                TextFocused( "E-cores average:", Percent( sums[1] / counts[1] ) );
            }
        }
    }
    ImGui::EndTooltip();
    return time;
}

std::optional<int64_t> DrawMemory( ImDrawList* draw, const VramPlotArea& area, const UsageData& data, float plotHeight )
{
    const int count = std::max( 1, int( std::ceil( area.width ) ) );
    const auto layout = LayoutMemory( data, plotHeight );
    const auto end = data.lastTime;
    const auto bottom = area.pos + ImVec2( area.width, area.height );
    draw->PushClipRect( area.pos, bottom, true );
    if( layout.game > 0 )
    {
        const auto sub = SubArea( area, 0, layout.game );
        Background( draw, sub, GameColor );
        std::vector<double> committed, workingSet;
        PlotPeakColumns( data.processCommitted, sub, end, count, committed );
        PlotPeakColumns( data.processWorkingSet, sub, end, count, workingSet );
        const auto peak = std::max( ColumnPeak( committed ), ColumnPeak( workingSet ) );
        const auto top = peak > 0 ? peak * 1.05 : 1.0;
        const auto scale = double( sub.height ) / top;
        FillFromBottom( draw, sub, committed, scale, WithAlpha( GameColor, 0x70 ) );
        PlotLineColumns( draw, sub, committed, scale, GameColor, 1.5f );
        PlotLineColumns( draw, sub, workingSet, scale, WorkingSetColor, 1.f );
        RangeLabels( draw, sub, GameColor, Bytes( top ) );
    }
    if( layout.machine > 0 )
    {
        const auto sub = SubArea( area, layout.game + layout.gap, layout.machine );
        Background( draw, sub, MachineColor );
        std::vector<double> used, installed;
        PlotPeakColumns( data.systemUsed, sub, end, count, used );
        PlotPeakColumns( data.systemTotal, sub, end, count, installed );
        const auto peak = std::max( ColumnPeak( used ), ColumnPeak( installed ) );
        const auto top = peak > 0 ? peak * 1.05 : 1.0;
        const auto scale = double( sub.height ) / top;
        FillFromBottom( draw, sub, used, scale, WithAlpha( MachineColor, 0x50 ) );
        PlotLineColumns( draw, sub, used, scale, MachineColor, 1.f );
        PlotLineColumns( draw, sub, installed, scale, InstalledColor, 1.5f );
        RangeLabels( draw, sub, MachineColor, Bytes( top ) );
    }
    draw->PopClipRect();

    const auto ty = ImGui::GetTextLineHeight();
    const auto legendY = area.pos.y - ty - 1;
    float x = bottom.x - ty * 0.5f;
    bool room = true;
    if( layout.machine > 0 )
    {
        room = PlotLegendEntry( draw, x, area.legendMinX, legendY, InstalledColor, "Installed", true ) &&
            PlotLegendEntry( draw, x, area.legendMinX, legendY, MachineColor, "Machine in use", false );
    }
    if( room && !data.processWorkingSet.Empty() ) room = PlotLegendEntry( draw, x, area.legendMinX, legendY, WorkingSetColor, "Game working set", true );
    if( room && !data.processCommitted.Empty() ) PlotLegendEntry( draw, x, area.legendMinX, legendY, GameColor, "Game committed", false );

    if( !area.hover || !ImGui::IsMouseHoveringRect( area.pos, bottom ) ) return std::nullopt;
    const auto mouse = ImGui::GetMousePos();
    const auto time = TimeUnderMouse( area );
    draw->AddLine( ImVec2( mouse.x, area.pos.y ), ImVec2( mouse.x, bottom.y ), 0x99FFFFFF );
    ImGui::BeginTooltip();
    ImGui::TextUnformatted( UsageLaneLabel( UsageLane::Memory ) );
    ImGui::Separator();
    TextFocused( "Time:", TimeToString( time ) );
    if( const auto value = data.processCommitted.At( time ) )
    {
        TextFocused( "Game committed:", Bytes( *value ) );
        TooltipIfHovered( "The game's private memory: what it allocated for itself, in RAM or the page file." );
    }
    if( const auto value = data.processWorkingSet.At( time ) )
    {
        TextFocused( "Game working set:", Bytes( *value ) );
        TooltipIfHovered( "The game's memory resident in RAM, including shared pages." );
    }
    const auto used = data.systemUsed.At( time );
    const auto installed = data.systemTotal.At( time );
    if( used )
    {
        char buf[128];
        if( installed && *installed > 0 ) snprintf( buf, sizeof( buf ), "%s of %s (%.1f%%)", Bytes( *used ), Bytes( *installed ), *used / *installed * 100 );
        else snprintf( buf, sizeof( buf ), "%s", Bytes( *used ) );
        TextFocused( "Machine in use:", buf );
    }
    ImGui::EndTooltip();
    return time;
}

std::optional<int64_t> DrawGpu( ImDrawList* draw, const VramPlotArea& area, const UsageData& data )
{
    const int count = std::max( 1, int( std::ceil( area.width ) ) );
    const auto bottom = area.pos + ImVec2( area.width, area.height );
    const auto scale = double( area.height ) / 100.0;
    draw->PushClipRect( area.pos, bottom, true );
    Background( draw, area, EngineColor( 0 ) );
    std::array<const std::vector<double>*, JnUsage::Engines> columns;
    for( int e = 0; e < JnUsage::Engines; ++e ) columns[e] = &Averages( data, data.engines[e], area, count );
    FillFromBottom( draw, area, *columns[0], scale, EngineColor( 0, 0x70 ) );
    for( int e = JnUsage::Engines - 1; e >= 0; --e )
        if( data.engineShown[e] ) PlotLineColumns( draw, area, *columns[e], scale, EngineColor( e ), e == 0 ? 1.5f : 1.f );
    RangeLabels( draw, area, EngineColor( 0 ), "100%" );
    draw->PopClipRect();

    const auto ty = ImGui::GetTextLineHeight();
    const auto legendY = area.pos.y - ty - 1;
    float x = bottom.x - ty * 0.5f;
    for( int e = JnUsage::Engines - 1; e >= 0; --e )
        if( data.engineShown[e] && !PlotLegendEntry( draw, x, area.legendMinX, legendY, EngineColor( e ), JnUsage::EngineNames[e], e != 0 ) ) break;

    if( !area.hover || !ImGui::IsMouseHoveringRect( area.pos, bottom ) ) return std::nullopt;
    const auto mouse = ImGui::GetMousePos();
    const auto time = TimeUnderMouse( area );
    draw->AddLine( ImVec2( mouse.x, area.pos.y ), ImVec2( mouse.x, bottom.y ), 0x99FFFFFF );
    ImGui::BeginTooltip();
    ImGui::TextUnformatted( UsageLaneLabel( UsageLane::Gpu ) );
    ImGui::Separator();
    TextFocused( "Time:", TimeToString( time ) );
    for( int e = 0; e < JnUsage::Engines; ++e )
    {
        if( !data.engineShown[e] ) continue;
        char label[64];
        snprintf( label, sizeof( label ), "%s:", JnUsage::EngineNames[e] );
        if( const auto value = AverageAt( data.engines[e], data.interval, time ) ) TextFocused( label, Percent( *value ) );
        else ImGui::TextDisabled( "%s not recorded at this time", label );
    }
    ImGui::Separator();
    TextDisabledUnformatted( "The share of time each engine ran the game's work." );
    ImGui::EndTooltip();
    return time;
}

uint32_t FpsColor( double fps, int target )
{
    if( fps >= target * 0.95 ) return FpsMetColor;
    if( fps >= target * 0.5 ) return FpsBelowColor;
    return FpsPoorColor;
}

// A pixel column of the FPS lane: the time-weighted average of the FPS of
// the frames it covers (frames per second over a column wider than a frame,
// the frame's own FPS within one), and its slowest frame.
struct FpsColumn
{
    double average = Nan, lowest = Nan;
    size_t frames = 0, slowest = 0;
};

void FpsColumns( const FrameTimes& frames, const VramPlotArea& area, int count, std::vector<FpsColumn>& out )
{
    out.assign( count, FpsColumn() );
    const auto n = frames.Complete();
    if( n == 0 ) return;
    const auto nsPerPixel = 1.0 / area.pxns;
    // The frame holding the area's begin.
    size_t i = size_t( std::upper_bound( frames.starts->begin(), frames.starts->begin() + n, area.begin ) - frames.starts->begin() );
    if( i > 0 ) --i;
    for( int c = 0; c < count && i < n; ++c )
    {
        const auto c0 = area.begin + int64_t( c * nsPerPixel );
        const auto c1 = area.begin + int64_t( ( c + 1 ) * nsPerPixel );
        auto& column = out[c];
        double covered = 0, rate = 0;
        int64_t longest = 0;
        while( i < n )
        {
            const auto start = frames.Start( i );
            if( start >= c1 ) break;
            const auto end = frames.End( i );
            if( end > start )
            {
                const auto overlap = double( std::min( end, c1 ) - std::max( start, c0 ) );
                if( overlap > 0 )
                {
                    covered += overlap;
                    rate += overlap / double( end - start );
                    column.frames++;
                    if( end - start > longest )
                    {
                        longest = end - start;
                        column.slowest = i;
                    }
                }
                // A frame running past the column continues in the next.
                if( end > c1 ) break;
            }
            ++i;
        }
        if( covered <= 0 ) continue;
        if( column.frames == 1 )
        {
            // Zoomed in past the frames: the line between the FPS of this
            // frame and of the neighbouring frame, through their middles.
            const auto f = column.slowest;
            const auto start = frames.Start( f ), end = frames.End( f );
            const auto middle = start + ( end - start ) / 2;
            const auto t = c0 + ( c1 - c0 ) / 2;
            auto fps = 1e9 / double( end - start );
            size_t g = f;
            if( t < middle && f > 0 && frames.End( f - 1 ) == start ) g = f - 1;
            else if( t > middle && f + 1 < n && frames.Start( f + 1 ) == end && frames.End( f + 1 ) > end ) g = f + 1;
            if( g != f )
            {
                const auto otherMiddle = frames.Start( g ) + ( frames.End( g ) - frames.Start( g ) ) / 2;
                const auto otherFps = 1e9 / double( frames.End( g ) - frames.Start( g ) );
                fps += ( otherFps - fps ) * double( t - middle ) / double( otherMiddle - middle );
            }
            column.average = fps;
        }
        else
        {
            column.average = 1e9 * rate / covered;
            column.lowest = 1e9 / double( longest );
        }
    }
}

// The frame holding t, or Complete() when none does.
size_t FrameAt( const FrameTimes& frames, int64_t t )
{
    const auto n = frames.Complete();
    auto i = size_t( std::upper_bound( frames.starts->begin(), frames.starts->begin() + n, t ) - frames.starts->begin() );
    if( i == 0 ) return n;
    --i;
    return frames.End( i ) > t ? i : n;
}

const char* Fps( double fps, int64_t frameTime )
{
    static char buf[4][64];
    static int next = 0;
    auto& out = buf[next++ % 4];
    snprintf( out, sizeof( out ), "%.1f FPS (%s)", fps, TimeToString( frameTime ) );
    return out;
}

}

VramSeries* UsageData::Find( std::string_view name )
{
    if( name == JnUsage::CpuSystemPlot ) return &cpuSystem;
    if( name == JnUsage::CpuProcessPlot ) return &cpuProcess;
    if( name == "CPU usage" ) return &tracyCpu;
    if( StartsWith( name, JnUsage::CpuCorePrefix ) && EndsWith( name, JnUsage::CpuCoreSuffix ) )
    {
        const auto digits = name.substr( strlen( JnUsage::CpuCorePrefix ), name.size() - strlen( JnUsage::CpuCorePrefix ) - strlen( JnUsage::CpuCoreSuffix ) );
        unsigned index = 0;
        const auto parsed = std::from_chars( digits.data(), digits.data() + digits.size(), index );
        if( parsed.ec != std::errc() || parsed.ptr != digits.data() + digits.size() || index >= unsigned( JnUsage::MaxCores ) ) return nullptr;
        // Reserved whole, so that series handed out stay where they are.
        if( cores.capacity() < size_t( JnUsage::MaxCores ) ) cores.reserve( JnUsage::MaxCores );
        if( cores.size() <= index ) cores.resize( index + 1 );
        return &cores[index];
    }
    if( name == JnUsage::MemoryProcessCommittedPlot ) return &processCommitted;
    if( name == JnUsage::MemoryProcessWorkingSetPlot ) return &processWorkingSet;
    if( name == JnUsage::MemorySystemUsedPlot ) return &systemUsed;
    if( name == JnUsage::MemorySystemTotalPlot ) return &systemTotal;
    for( int e = 0; e < JnUsage::Engines; ++e )
        if( name == JnUsage::GpuEnginePlots[e] ) return &engines[e];
    if( StartsWith( name, "UnityCounter/" ) )
    {
        if( EndsWith( name, "/App Committed Memory" ) ) return &unityCommitted;
        if( EndsWith( name, "/App Resident Memory" ) ) return &unityResident;
    }
    return nullptr;
}

void UsageData::ParseAppInfo( std::string_view text )
{
    if( !StartsWith( text, JnUsage::AppInfoPrefix ) ) return;
    text.remove_prefix( strlen( JnUsage::AppInfoPrefix ) );
    while( !text.empty() )
    {
        const auto space = text.find( ' ' );
        const auto field = text.substr( 0, space );
        text = space == std::string_view::npos ? std::string_view() : text.substr( space + 1 );
        const auto equals = field.find( '=' );
        if( equals == std::string_view::npos ) continue;
        const auto key = field.substr( 0, equals );
        auto value = field.substr( equals + 1 );
        if( key == "classes" )
        {
            coreClasses.clear();
            while( !value.empty() && coreClasses.size() < size_t( JnUsage::MaxCores ) )
            {
                const auto comma = value.find( ',' );
                const auto item = value.substr( 0, comma );
                int cls = -1;
                std::from_chars( item.data(), item.data() + item.size(), cls );
                coreClasses.push_back( cls );
                value = comma == std::string_view::npos ? std::string_view() : value.substr( comma + 1 );
            }
        }
        else if( key == "interval_ms" )
        {
            unsigned ms = 0;
            const auto parsed = std::from_chars( value.data(), value.data() + value.size(), ms );
            if( parsed.ec == std::errc() && ms > 0 ) interval = int64_t( ms ) * 1'000'000;
        }
    }
}

void UsageData::Finish()
{
    for( auto* series : { &cpuSystem, &cpuProcess, &tracyCpu, &processCommitted, &processWorkingSet, &systemUsed, &systemTotal, &unityCommitted, &unityResident } ) Order( *series );
    for( auto& series : cores ) Order( series );
    for( auto& series : engines ) Order( series );
    if( processCommitted.Empty() ) std::swap( processCommitted, unityCommitted );
    if( processWorkingSet.Empty() ) std::swap( processWorkingSet, unityResident );
    // Captures without the processor description: the plots' median spacing.
    if( coreClasses.empty() )
    {
        const auto& series = !cpuSystem.Empty() ? cpuSystem : tracyCpu;
        if( series.time.size() > 2 )
        {
            std::vector<int64_t> gaps;
            for( size_t i = 1; i < series.time.size() && gaps.size() < 1000; ++i ) gaps.push_back( series.time[i] - series.time[i - 1] );
            std::nth_element( gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end() );
            if( gaps[gaps.size() / 2] > 0 ) interval = gaps[gaps.size() / 2];
        }
    }
    for( const auto* series : { &cpuSystem, &tracyCpu, &processCommitted, &systemUsed } )
        if( !series->Empty() ) lastTime = std::max( lastTime, series->time.back() );
    for( int e = 0; e < JnUsage::Engines; ++e )
    {
        const auto& values = engines[e].value;
        engineShown[e] = !values.empty() && ( e == int( JnUsage::Engine::ThreeD ) || std::any_of( values.begin(), values.end(), []( double v ) { return v > 0; } ) );
    }
}

bool UsageData::HasCpu() const
{
    return !cpuSystem.Empty() || !cpuProcess.Empty() || !tracyCpu.Empty() || HasCores();
}

bool UsageData::HasCores() const
{
    for( const auto& series : cores ) if( !series.Empty() ) return true;
    return false;
}

bool UsageData::HasMemory() const
{
    return !processCommitted.Empty() || !processWorkingSet.Empty() || !systemUsed.Empty();
}

bool UsageData::HasGpu() const
{
    for( const auto& series : engines ) if( !series.Empty() ) return true;
    return false;
}

bool UsageData::Hybrid() const
{
    int lowest = INT32_MAX, highest = -1;
    for( const auto cls : coreClasses )
    {
        if( cls < 0 ) continue;
        lowest = std::min( lowest, cls );
        highest = std::max( highest, cls );
    }
    return highest > lowest && highest >= 0;
}

bool UsageData::EfficiencyCore( size_t core ) const
{
    if( core >= coreClasses.size() || coreClasses[core] < 0 || !Hybrid() ) return false;
    int lowest = INT32_MAX;
    for( const auto cls : coreClasses ) if( cls >= 0 ) lowest = std::min( lowest, cls );
    return coreClasses[core] == lowest;
}

bool IsUsagePlot( std::string_view name )
{
    if( name == JnUsage::CpuSystemPlot || name == JnUsage::CpuProcessPlot ) return true;
    if( StartsWith( name, JnUsage::CpuCorePrefix ) && EndsWith( name, JnUsage::CpuCoreSuffix ) ) return true;
    if( StartsWith( name, "Memory.Process." ) || StartsWith( name, "Memory.System." ) ) return true;
    if( StartsWith( name, "GPU.Process." ) || StartsWith( name, "GPU.Adapter." ) ) return true;
    return false;
}

const char* UsageLaneLabel( UsageLane lane )
{
    switch( lane )
    {
    case UsageLane::Cpu: return ICON_FA_GAUGE_HIGH " CPU usage";
    case UsageLane::Memory: return ICON_FA_MEMORY " Memory usage";
    case UsageLane::Gpu: return ICON_FA_GAUGE_HIGH " GPU usage";
    default: return "";
    }
}

bool UsageLanePresent( const UsageData& data, UsageLane lane )
{
    switch( lane )
    {
    case UsageLane::Cpu: return data.HasCpu();
    case UsageLane::Memory: return data.HasMemory();
    case UsageLane::Gpu: return data.HasGpu();
    default: return false;
    }
}

float UsageLaneHeight( const UsageData& data, UsageLane lane, float plotHeight )
{
    switch( lane )
    {
    case UsageLane::Cpu:
    {
        const auto layout = LayoutCpu( data, plotHeight );
        return layout.usage + layout.gap + layout.row * float( layout.rows );
    }
    case UsageLane::Memory:
    {
        const auto layout = LayoutMemory( data, plotHeight );
        return layout.game + layout.gap + layout.machine;
    }
    case UsageLane::Gpu: return plotHeight * 0.5f;
    default: return 0;
    }
}

std::optional<int64_t> DrawUsageLane( ImDrawList* draw, const VramPlotArea& area, const UsageData& data, UsageLane lane, float plotHeight )
{
    switch( lane )
    {
    case UsageLane::Cpu: return DrawCpu( draw, area, data, plotHeight );
    case UsageLane::Memory: return DrawMemory( draw, area, data, plotHeight );
    case UsageLane::Gpu: return DrawGpu( draw, area, data );
    default: return std::nullopt;
    }
}

void UsageLaneTooltip( const UsageData& data, UsageLane lane )
{
    ImGui::BeginTooltip();
    ImGui::TextUnformatted( UsageLaneLabel( lane ) );
    ImGui::Separator();
    char buf[128];
    switch( lane )
    {
    case UsageLane::Cpu:
    {
        const auto& machine = MachineCpu( data );
        if( !machine.Empty() )
        {
            TextFocused( "Machine average:", Percent( SeriesMean( machine ) ) );
            PeakLine( "Machine peak:", SeriesPeak( machine ), false );
        }
        if( !data.cpuProcess.Empty() )
        {
            TextFocused( "Game average:", Percent( SeriesMean( data.cpuProcess ) ) );
            PeakLine( "Game peak:", SeriesPeak( data.cpuProcess ), false );
        }
        if( !data.cores.empty() )
        {
            int efficiency = 0;
            for( size_t core = 0; core < data.cores.size(); ++core ) efficiency += data.EfficiencyCore( core ) ? 1 : 0;
            if( efficiency > 0 ) snprintf( buf, sizeof( buf ), "%zu (%zu performance, %d efficiency)", data.cores.size(), data.cores.size() - efficiency, efficiency );
            else snprintf( buf, sizeof( buf ), "%zu", data.cores.size() );
            TextFocused( "Logical processors:", buf );
        }
        ImGui::Separator();
        if( data.cpuSystem.Empty() ) TextDisabledUnformatted( "Tracy's CPU usage of the machine; the capture predates the engine's usage plots." );
        else
        {
            TextDisabledUnformatted( "Busy time from the processors' idle cycle counters, averaged over each sample." );
            TextDisabledUnformatted( "Top: the whole machine (grey) and the game (orange); the legend gives both at the dashed line." );
            TextDisabledUnformatted( "Rows: each logical processor (P performance, E efficiency core), dark when idle, then green, yellow, red at 100%." );
        }
        break;
    }
    case UsageLane::Memory:
        PeakLine( "Game committed peak:", SeriesPeak( data.processCommitted ), true );
        if( data.processCommitted.time.size() > 1 )
        {
            snprintf( buf, sizeof( buf ), "%s to %s", Bytes( data.processCommitted.value.front() ), Bytes( data.processCommitted.value.back() ) );
            TextFocused( "Game committed:", buf );
        }
        PeakLine( "Game working set peak:", SeriesPeak( data.processWorkingSet ), true );
        PeakLine( "Machine in use peak:", SeriesPeak( data.systemUsed ), true );
        if( !data.systemTotal.Empty() ) TextFocused( "Installed:", Bytes( data.systemTotal.value.back() ) );
        if( data.systemUsed.Empty() )
        {
            ImGui::Separator();
            TextDisabledUnformatted( "Unity's memory counters; the capture predates the engine's usage plots." );
        }
        break;
    case UsageLane::Gpu:
        for( int e = 0; e < JnUsage::Engines; ++e )
        {
            if( !data.engineShown[e] ) continue;
            snprintf( buf, sizeof( buf ), "%s average:", JnUsage::EngineNames[e] );
            TextFocused( buf, Percent( SeriesMean( data.engines[e] ) ) );
            snprintf( buf, sizeof( buf ), "%s peak:", JnUsage::EngineNames[e] );
            PeakLine( buf, SeriesPeak( data.engines[e] ), false );
        }
        ImGui::Separator();
        TextDisabledUnformatted( "The share of time each GPU engine ran the game's work, averaged over each sample." );
        break;
    default:
        break;
    }
    ImGui::EndTooltip();
}

size_t FrameTimes::Complete() const
{
    if( !starts || starts->empty() ) return 0;
    return ends ? starts->size() : starts->size() - 1;
}

int64_t FrameTimes::End( size_t i ) const
{
    if( ends ) return ( *ends )[i];
    return i + 1 < starts->size() ? ( *starts )[i + 1] : -1;
}

const char* FpsLaneLabel()
{
    return ICON_FA_GAUGE " FPS";
}

float FpsLaneHeight( float plotHeight )
{
    return plotHeight * 0.6f;
}

std::optional<int64_t> DrawFpsLane( ImDrawList* draw, const VramPlotArea& area, const FrameTimes& frames, int target )
{
    target = std::max( 1, target );
    const int count = std::max( 1, int( std::ceil( area.width ) ) );
    static struct
    {
        const int64_t* starts = nullptr;
        size_t size = 0;
        const std::vector<int64_t>* ends = nullptr;
        int64_t begin = 0;
        double pxns = 0;
        std::vector<FpsColumn> values;
    } cached;
    if( cached.starts != frames.starts->data() || cached.size != frames.starts->size() || cached.ends != frames.ends ||
        cached.begin != area.begin || cached.pxns != area.pxns || cached.values.size() != size_t( count ) )
    {
        cached.starts = frames.starts->data();
        cached.size = frames.starts->size();
        cached.ends = frames.ends;
        cached.begin = area.begin;
        cached.pxns = area.pxns;
        FpsColumns( frames, area, count, cached.values );
    }
    const auto& columns = cached.values;
    // From zero to the view's highest average, at least a quarter above the
    // target and at most four times it: loading screens at hundreds of FPS
    // are clipped rather than flattening the rest.
    double peak = 0;
    for( const auto& column : columns ) if( !std::isnan( column.average ) ) peak = std::max( peak, column.average );
    const auto top = std::clamp( peak * 1.1, target * 1.25, target * 4.0 );
    const auto scale = double( area.height ) / top;
    const auto bottom = area.pos + ImVec2( area.width, area.height );
    draw->PushClipRect( area.pos, bottom, true );
    Background( draw, area, FpsMetColor );
    // The average, filled in its color against the target, in runs of one
    // color and height.
    const auto heightOf = [&]( double fps ) { return float( std::min( fps * scale, double( area.height ) ) ); };
    for( int c = 0; c < count; )
    {
        const auto value = columns[c].average;
        if( std::isnan( value ) )
        {
            c++;
            continue;
        }
        const auto height = heightOf( value );
        const auto color = FpsColor( value, target );
        int e = c + 1;
        while( e < count && !std::isnan( columns[e].average ) && FpsColor( columns[e].average, target ) == color && heightOf( columns[e].average ) == height ) e++;
        draw->AddRectFilled( ImVec2( area.pos.x + float( c ), bottom.y - height ), ImVec2( area.pos.x + float( e ), bottom.y ), WithAlpha( color, 0x60 ) );
        c = e;
    }
    // The average as a line over its fill, then the slowest frame's.
    std::vector<double> average( count ), lowest( count );
    for( int c = 0; c < count; ++c )
    {
        average[c] = std::min( columns[c].average, top );
        lowest[c] = std::min( columns[c].lowest, top );
    }
    PlotLineColumns( draw, area, average, scale, FpsLineColor, 1.5f );
    PlotLineColumns( draw, area, lowest, scale, FpsSlowestColor, 1.f );
    const auto ty = ImGui::GetTextLineHeight();
    const auto targetY = std::round( bottom.y - float( target * scale ) ) + 0.5f;
    draw->AddLine( ImVec2( area.pos.x, targetY ), ImVec2( bottom.x, targetY ), FpsTargetColor );
    // The target's label at the right end of its line, above it where
    // there is room.
    char label[32];
    snprintf( label, sizeof( label ), "%d FPS", target );
    const auto labelX = bottom.x - ImGui::CalcTextSize( label ).x - ty * 0.5f;
    DrawTextSuperContrast( draw, ImVec2( labelX, targetY - ty >= area.pos.y ? targetY - ty : targetY + 1 ), FpsTargetColor, label );
    snprintf( label, sizeof( label ), "%.0f FPS", top );
    RangeLabels( draw, area, FpsMetColor, label );
    draw->PopClipRect();

    const auto legendY = area.pos.y - ty - 1;
    float x = bottom.x - ty * 0.5f;
    if( PlotLegendEntry( draw, x, area.legendMinX, legendY, FpsTargetColor, "Target", true ) &&
        PlotLegendEntry( draw, x, area.legendMinX, legendY, FpsSlowestColor, "Slowest frame", true ) &&
        PlotLegendEntry( draw, x, area.legendMinX, legendY, FpsBelowColor, "Below target", false ) )
        PlotLegendEntry( draw, x, area.legendMinX, legendY, FpsMetColor, "Average FPS", false );

    if( !area.hover || !ImGui::IsMouseHoveringRect( area.pos, bottom ) ) return std::nullopt;
    const auto mouse = ImGui::GetMousePos();
    const auto time = TimeUnderMouse( area );
    draw->AddLine( ImVec2( mouse.x, area.pos.y ), ImVec2( mouse.x, bottom.y ), 0x99FFFFFF );
    const auto& column = columns[std::clamp( int( mouse.x - area.pos.x ), 0, count - 1 )];
    char buf[64];
    ImGui::BeginTooltip();
    ImGui::TextUnformatted( FpsLaneLabel() );
    ImGui::SameLine();
    ImGui::TextDisabled( "%s", frames.name );
    ImGui::Separator();
    TextFocused( "Time:", TimeToString( time ) );
    if( const auto i = FrameAt( frames, time ); i < frames.Complete() )
    {
        const auto frameTime = frames.End( i ) - frames.Start( i );
        snprintf( buf, sizeof( buf ), "%llu", (unsigned long long)( frames.firstNumber + i ) );
        TextFocused( "Frame:", buf );
        TextFocused( "This frame:", Fps( 1e9 / double( frameTime ), frameTime ) );
    }
    if( column.frames > 1 )
    {
        TextFocused( "Average here:", Fps( column.average, int64_t( 1e9 / column.average ) ) );
        ImGui::SameLine();
        ImGui::TextDisabled( "(%zu frames in this pixel)", column.frames );
        const auto slowest = frames.End( column.slowest ) - frames.Start( column.slowest );
        TextFocused( "Slowest here:", Fps( 1e9 / double( slowest ), slowest ) );
        ImGui::SameLine();
        ImGui::TextDisabled( "frame %llu", (unsigned long long)( frames.firstNumber + column.slowest ) );
    }
    snprintf( buf, sizeof( buf ), "%d FPS (%s)", target, TimeToString( 1'000'000'000ll / target ) );
    TextFocused( "Target:", buf );
    ImGui::EndTooltip();
    return time;
}

void FpsLaneTooltip( const FrameTimes& frames, int target )
{
    target = std::max( 1, target );
    // The capture's frame times, summarized once per frame set, frame count
    // and target.
    static struct
    {
        const std::vector<int64_t>* starts = nullptr;
        size_t count = 0;
        int target = 0;
        size_t frames = 0, slow = 0;
        int64_t total = 0, median = 0, p99 = 0;
    } summary;
    const auto count = frames.starts ? frames.starts->size() : 0;
    if( summary.starts != frames.starts || summary.count != count || summary.target != target )
    {
        summary = {};
        summary.starts = frames.starts;
        summary.count = count;
        summary.target = target;
        std::vector<int64_t> times;
        const auto n = frames.Complete();
        times.reserve( n );
        for( size_t i = 0; i < n; ++i )
        {
            const auto end = frames.End( i );
            if( end <= frames.Start( i ) ) continue;
            const auto frameTime = end - frames.Start( i );
            times.push_back( frameTime );
            summary.total += frameTime;
            if( frameTime > 1'000'000'000ll / target ) summary.slow++;
        }
        summary.frames = times.size();
        if( !times.empty() )
        {
            std::nth_element( times.begin(), times.begin() + times.size() / 2, times.end() );
            summary.median = times[times.size() / 2];
            const auto p99 = std::min( times.size() - 1, size_t( double( times.size() ) * 0.99 ) );
            std::nth_element( times.begin(), times.begin() + p99, times.end() );
            summary.p99 = times[p99];
        }
    }
    ImGui::BeginTooltip();
    ImGui::TextUnformatted( FpsLaneLabel() );
    ImGui::SameLine();
    ImGui::TextDisabled( "%s", frames.name );
    ImGui::Separator();
    if( summary.frames > 0 && summary.total > 0 )
    {
        char buf[128];
        snprintf( buf, sizeof( buf ), "%zu over %s", summary.frames, TimeToString( summary.total ) );
        TextFocused( "Frames:", buf );
        const auto average = double( summary.frames ) * 1e9 / double( summary.total );
        TextFocused( "Average:", Fps( average, int64_t( 1e9 / average ) ) );
        TextFocused( "Median frame:", Fps( 1e9 / double( summary.median ), summary.median ) );
        TextFocused( "99th percentile frame:", Fps( 1e9 / double( summary.p99 ), summary.p99 ) );
        snprintf( buf, sizeof( buf ), "%zu (%.1f%%)", summary.slow, 100.0 * double( summary.slow ) / double( summary.frames ) );
        TextFocused( "Slower than the target:", buf );
    }
    else ImGui::TextDisabled( "No complete frames." );
    ImGui::Separator();
    TextDisabledUnformatted( "Per pixel: the average FPS of the frames it covers, and its slowest frame." );
    TextDisabledUnformatted( "The frame set and the target are the frame graph's." );
    ImGui::EndTooltip();
}

std::optional<int64_t> DrawPlotLane( ImDrawList* draw, const VramPlotArea& area, const VramPlotArea& background,
    const std::function<std::optional<int64_t>( const VramPlotArea& )>& drawLane )
{
    PlotOverviewBackground( draw, background );
    const auto time = drawLane( area );
    PlotViewMarker( draw, area, area.begin, area.begin + int64_t( double( area.width ) / area.pxns ) );
    return time;
}

}

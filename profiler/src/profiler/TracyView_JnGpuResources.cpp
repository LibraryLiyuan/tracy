#include "TracyView.hpp"

#include "TracyGpuAnalysisController.hpp"
#include "TracyImGui.hpp"
#include "TracyPrint.hpp"

#include "imgui.h"
#include "IconsFontAwesome6.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace tracy
{
namespace
{

std::string ByteText( uint64_t bytes, bool decimal )
{
    static constexpr const char* binaryUnits[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    static constexpr const char* decimalUnits[] = { "B", "KB", "MB", "GB", "TB" };
    const double divisor = decimal ? 1000.0 : 1024.0;
    double value = double( bytes ); int unit = 0;
    while( value >= divisor && unit < 4 ) { value /= divisor; ++unit; }
    char buffer[64]; std::snprintf( buffer, sizeof( buffer ), value >= 100 ? "%.0f %s" : value >= 10 ? "%.1f %s" : "%.2f %s",
        value, decimal ? decimalUnits[unit] : binaryUnits[unit] );
    return buffer;
}

bool ContainsInsensitive( const std::string& text, const char* pattern )
{
    if( !pattern || !*pattern ) return true;
    const std::string needle( pattern );
    return std::search( text.begin(), text.end(), needle.begin(), needle.end(), []( char a, char b )
    { return std::tolower( static_cast<unsigned char>( a ) ) == std::tolower( static_cast<unsigned char>( b ) ); } ) != text.end();
}

bool ResourceMatches( const analysis::GpuResourceAnalysisRecord& item, const char* search )
{
    if( !search || !*search ) return true;
    if( ContainsInsensitive( item.name, search ) || ContainsInsensitive( analysis::GpuPrimaryKindName( item.primaryKind ), search ) ) return true;
    char id[32]; std::snprintf( id, sizeof( id ), "%llu", static_cast<unsigned long long>( item.resourceId ) );
    return ContainsInsensitive( id, search );
}

ImGuiTableColumnSortSpecs SortSpecOrDefault( ImGuiTableSortSpecs* specs, ImGuiID column, ImGuiSortDirection direction )
{
    if( specs && specs->SpecsCount ) return specs->Specs[0];
    ImGuiTableColumnSortSpecs result;
    result.ColumnUserID = column;
    result.ColumnIndex = -1;
    result.SortOrder = 0;
    result.SortDirection = direction;
    return result;
}

void Metric( const char* name, uint64_t bytes, bool decimal, const char* quality )
{
    ImGui::BeginGroup(); ImGui::TextDisabled( "%s", name );
    const auto text = ByteText( bytes, decimal ); ImGui::Text( "%s", text.c_str() );
    if( ImGui::IsItemHovered() ) ImGui::SetTooltip( "%llu bytes\n%s", static_cast<unsigned long long>( bytes ), quality );
    ImGui::EndGroup();
}

void DrawResourceInspector( const analysis::GpuAnalysisSnapshot& snapshot, const analysis::GpuResourceAnalysisRecord* resource, bool decimal )
{
    if( !resource ) { ImGui::TextDisabled( "Select a resource to inspect evidence." ); return; }
    ImGui::Text( "%s", resource->name.empty() ? "Unnamed GPU resource" : resource->name.c_str() );
    ImGui::TextDisabled( "Resource %llu / Family %llu", static_cast<unsigned long long>( resource->resourceId ), static_cast<unsigned long long>( resource->familyId ) );
    ImGui::SeparatorText( "Identity" );
    ImGui::Text( "Kind: %s", analysis::GpuPrimaryKindName( resource->primaryKind ) );
    ImGui::Text( "Generation: %llu    Definition revision: %u", static_cast<unsigned long long>( resource->generation ), resource->definitionRevision );
    ImGui::Text( "Name provenance: %u    Classification provenance: %u", resource->nameProvenance, resource->classificationProvenance );
    ImGui::SeparatorText( "Memory" );
    const auto capacity = ByteText( resource->capacityBytes, decimal ); ImGui::Text( "Logical capacity: %s", capacity.c_str() );
    if( const auto allocation = snapshot.FindAllocation( resource->allocationId ) )
    {
        const auto physical = ByteText( allocation->sizeBytes, decimal ); const auto resident = ByteText( allocation->residentBytes, decimal );
        ImGui::Text( "Allocation %llu: %s", static_cast<unsigned long long>( allocation->allocationId ), physical.c_str() );
        ImGui::Text( "Heap %llu / offset %llu / resident %s", static_cast<unsigned long long>( allocation->heapId ),
            static_cast<unsigned long long>( allocation->offsetBytes ), resident.c_str() );
        if( allocation->resources.size() > 1 ) ImGui::TextColored( ImVec4( 1.f, .75f, .2f, 1.f ), "Shared / alias participants: %zu", allocation->resources.size() );
    }
    else ImGui::TextDisabled( "Physical allocation: unavailable" );
    ImGui::SeparatorText( "Lifetime" );
    ImGui::Text( "Create: %llu ns", static_cast<unsigned long long>( resource->createTime ) );
    ImGui::Text( "Destroy: %s", resource->aliveAtEnd ? "Alive at capture end" : std::to_string( resource->destroyTime ).c_str() );
    if( resource->openBoundary ) ImGui::TextColored( ImVec4( 1.f, .75f, .2f, 1.f ), "Open create boundary (pre-capture)" );
    ImGui::SeparatorText( "Usage & views" );
    ImGui::Text( "Views %zu / Parts %zu / Ranges %zu / Relations %zu", resource->views.size(), resource->parts.size(), resource->ranges.size(), resource->relations.size() );
    ImGui::Text( "Declared 0x%08x / Observed 0x%08x", resource->declaredUsageMask, resource->observedUsageMask );
    if( resource->width || resource->height ) ImGui::Text( "Extent %llu x %u x %u / mip %u / format %u", static_cast<unsigned long long>( resource->width ), resource->height,
        resource->depthOrArraySize, resource->mipLevels, resource->format );
    ImGui::SeparatorText( "Source & quality" );
    ImGui::Text( "Callsite %u / stack provenance %u", resource->createCallsiteId, resource->stackProvenance );
    ImGui::Text( "Exactness: %s", analysis::GpuExactnessName( resource->exactness ) );
    if( resource->invalid ) ImGui::TextColored( ImVec4( 1.f, .25f, .25f, 1.f ), "InvalidRecord" );
}

void DrawResourceTable( const analysis::GpuAnalysisSnapshot& snapshot, auto& ui )
{
    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable;
    // The table identity intentionally changes with the sortable N27 layout.  Reusing the
    // pre-sort identity would restore an old ID ordering from imgui.ini and defeat the new
    // capacity-descending default on an existing workstation.
    if( !ImGui::BeginTable( "gpuResourcesN27Sortable", 7, flags ) ) return;
    ImGui::TableSetupColumn( "ID", ImGuiTableColumnFlags_WidthFixed, 75, 0 ); ImGui::TableSetupColumn( "Name", ImGuiTableColumnFlags_WidthStretch, 0, 1 );
    ImGui::TableSetupColumn( "Kind", ImGuiTableColumnFlags_WidthFixed, 120, 2 );
    ImGui::TableSetupColumn( "Capacity", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending, 95, 3 );
    ImGui::TableSetupColumn( "Physical", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, 95, 4 );
    ImGui::TableSetupColumn( "Allocation", ImGuiTableColumnFlags_WidthFixed, 85, 5 ); ImGui::TableSetupColumn( "Quality", ImGuiTableColumnFlags_WidthFixed, 90, 6 );
    ImGui::TableSetupScrollFreeze( 0, 1 ); ImGui::TableHeadersRow();
    auto* sort = ImGui::TableGetSortSpecs();
    if( ui.resourceOrder.size() != snapshot.resources.size() || ( sort && sort->SpecsDirty ) )
    {
        ui.resourceOrder.resize( snapshot.resources.size() );
        for( size_t i = 0; i < ui.resourceOrder.size(); ++i ) ui.resourceOrder[i] = i;
        const auto spec = SortSpecOrDefault( sort, 3, ImGuiSortDirection_Descending );
        std::stable_sort( ui.resourceOrder.begin(), ui.resourceOrder.end(), [&]( size_t lhsIndex, size_t rhsIndex ) {
            const auto& lhs = snapshot.resources[lhsIndex]; const auto& rhs = snapshot.resources[rhsIndex];
            int cmp = 0;
            switch( spec.ColumnUserID )
            {
            case 0: cmp = lhs.resourceId < rhs.resourceId ? -1 : lhs.resourceId > rhs.resourceId ? 1 : 0; break;
            case 1: cmp = lhs.name.compare( rhs.name ); break;
            case 2: cmp = int( lhs.primaryKind ) - int( rhs.primaryKind ); break;
            case 4:
            {
                const auto* la = snapshot.FindAllocation( lhs.allocationId ); const auto* ra = snapshot.FindAllocation( rhs.allocationId );
                const uint64_t lv = la ? la->sizeBytes : 0, rv = ra ? ra->sizeBytes : 0;
                cmp = lv < rv ? -1 : lv > rv ? 1 : 0; break;
            }
            case 5: cmp = lhs.allocationId < rhs.allocationId ? -1 : lhs.allocationId > rhs.allocationId ? 1 : 0; break;
            case 6: cmp = lhs.invalid != rhs.invalid ? ( lhs.invalid ? 1 : -1 ) : int( lhs.exactness ) - int( rhs.exactness ); break;
            default: cmp = lhs.capacityBytes < rhs.capacityBytes ? -1 : lhs.capacityBytes > rhs.capacityBytes ? 1 : 0; break;
            }
            if( cmp == 0 ) cmp = lhs.resourceId < rhs.resourceId ? -1 : lhs.resourceId > rhs.resourceId ? 1 : 0;
            return spec.SortDirection == ImGuiSortDirection_Ascending ? cmp < 0 : cmp > 0;
        } );
        if( sort ) sort->SpecsDirty = false;
    }
    const bool filtered = ui.search[0] != '\0';
    auto row = [&]( const analysis::GpuResourceAnalysisRecord& item )
    {
        ImGui::TableNextRow(); ImGui::TableNextColumn();
        const bool selected = ui.selectedResource == item.resourceId;
        char label[64]; std::snprintf( label, sizeof( label ), "%llu##gpuRes", static_cast<unsigned long long>( item.resourceId ) );
        if( ImGui::Selectable( label, selected, ImGuiSelectableFlags_SpanAllColumns ) ) { ui.selectedResource = item.resourceId; ui.selectedAllocation = item.allocationId; }
        ImGui::TableNextColumn(); ImGui::TextUnformatted( item.name.empty() ? "<unnamed>" : item.name.c_str() );
        ImGui::TableNextColumn(); ImGui::TextUnformatted( analysis::GpuPrimaryKindName( item.primaryKind ) );
        ImGui::TableNextColumn(); const auto capacity = ByteText( item.capacityBytes, ui.decimalUnits ); ImGui::TextUnformatted( capacity.c_str() );
        ImGui::TableNextColumn(); const auto* allocation = snapshot.FindAllocation( item.allocationId );
        ImGui::TextUnformatted( ByteText( allocation ? allocation->sizeBytes : 0, ui.decimalUnits ).c_str() );
        ImGui::TableNextColumn(); ImGui::Text( "%llu", static_cast<unsigned long long>( item.allocationId ) );
        ImGui::TableNextColumn(); ImGui::TextUnformatted( item.invalid ? "Invalid" : analysis::GpuExactnessName( item.exactness ) );
    };
    if( filtered ) { for( const auto index : ui.resourceOrder ) { const auto& item = snapshot.resources[index]; if( ResourceMatches( item, ui.search ) ) row( item ); } }
    else
    {
        ImGuiListClipper clipper; clipper.Begin( int( ui.resourceOrder.size() ) );
        while( clipper.Step() ) for( int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i ) row( snapshot.resources[ui.resourceOrder[size_t( i )]] );
    }
    ImGui::EndTable();
}

void DrawAllocationTable( const analysis::GpuAnalysisSnapshot& snapshot, auto& ui )
{
    if( !ImGui::BeginTable( "gpuAllocationsN27Sortable", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable ) ) return;
    ImGui::TableSetupColumn( "Allocation", 0, 0, 0 ); ImGui::TableSetupColumn( "Heap", 0, 0, 1 ); ImGui::TableSetupColumn( "Offset", 0, 0, 2 );
    ImGui::TableSetupColumn( "Size", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending, 0, 3 );
    ImGui::TableSetupColumn( "Resident", ImGuiTableColumnFlags_PreferSortDescending, 0, 4 ); ImGui::TableSetupColumn( "Resources", 0, 0, 5 ); ImGui::TableSetupColumn( "State", 0, 0, 6 );
    ImGui::TableSetupScrollFreeze( 0, 1 ); ImGui::TableHeadersRow();
    auto* sort = ImGui::TableGetSortSpecs();
    if( ui.allocationOrder.size() != snapshot.allocations.size() || ( sort && sort->SpecsDirty ) )
    {
        ui.allocationOrder.resize( snapshot.allocations.size() );
        for( size_t i = 0; i < ui.allocationOrder.size(); ++i ) ui.allocationOrder[i] = i;
        const auto spec = SortSpecOrDefault( sort, 3, ImGuiSortDirection_Descending );
        std::stable_sort( ui.allocationOrder.begin(), ui.allocationOrder.end(), [&]( size_t lhsIndex, size_t rhsIndex ) {
            const auto& lhs = snapshot.allocations[lhsIndex]; const auto& rhs = snapshot.allocations[rhsIndex]; int cmp = 0;
            const auto compare = [&]( uint64_t lv, uint64_t rv ) { return lv < rv ? -1 : lv > rv ? 1 : 0; };
            switch( spec.ColumnUserID )
            {
            case 0: cmp = compare( lhs.allocationId, rhs.allocationId ); break; case 1: cmp = compare( lhs.heapId, rhs.heapId ); break;
            case 2: cmp = compare( lhs.offsetBytes, rhs.offsetBytes ); break; case 4: cmp = compare( lhs.residentBytes, rhs.residentBytes ); break;
            case 5: cmp = compare( lhs.resources.size(), rhs.resources.size() ); break;
            case 6: cmp = lhs.invalid != rhs.invalid ? ( lhs.invalid ? 1 : -1 ) : lhs.aliveAtEnd != rhs.aliveAtEnd ? ( lhs.aliveAtEnd ? -1 : 1 ) : 0; break;
            default: cmp = compare( lhs.sizeBytes, rhs.sizeBytes ); break;
            }
            if( cmp == 0 ) cmp = compare( lhs.allocationId, rhs.allocationId );
            return spec.SortDirection == ImGuiSortDirection_Ascending ? cmp < 0 : cmp > 0;
        } );
        if( sort ) sort->SpecsDirty = false;
    }
    ImGuiListClipper clipper; clipper.Begin( int( ui.allocationOrder.size() ) );
    while( clipper.Step() ) for( int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i )
    {
        const auto& item = snapshot.allocations[ui.allocationOrder[size_t( i )]]; ImGui::TableNextRow(); ImGui::TableNextColumn();
        char id[64]; std::snprintf( id, sizeof( id ), "%llu##gpuAlloc", static_cast<unsigned long long>( item.allocationId ) );
        if( ImGui::Selectable( id, ui.selectedAllocation == item.allocationId, ImGuiSelectableFlags_SpanAllColumns ) )
        { ui.selectedAllocation = item.allocationId; ui.selectedHeap = item.heapId; if( !item.resources.empty() ) ui.selectedResource = item.resources.front(); }
        ImGui::TableNextColumn(); ImGui::Text( "%llu", static_cast<unsigned long long>( item.heapId ) );
        ImGui::TableNextColumn(); ImGui::Text( "%llu", static_cast<unsigned long long>( item.offsetBytes ) );
        ImGui::TableNextColumn(); const auto size = ByteText( item.sizeBytes, ui.decimalUnits ); ImGui::TextUnformatted( size.c_str() );
        ImGui::TableNextColumn(); const auto resident = ByteText( item.residentBytes, ui.decimalUnits ); ImGui::TextUnformatted( resident.c_str() );
        ImGui::TableNextColumn(); ImGui::Text( "%zu", item.resources.size() );
        ImGui::TableNextColumn(); ImGui::TextUnformatted( item.invalid ? "Invalid" : item.aliveAtEnd ? "Alive" : "Destroyed" );
    }
    ImGui::EndTable();
}

void DrawHeapMap( const analysis::GpuAnalysisSnapshot& snapshot, auto& ui )
{
    if( ui.selectedHeap == 0 && !snapshot.allocations.empty() ) ui.selectedHeap = snapshot.allocations.front().heapId;
    uint64_t extent = 0; size_t count = 0;
    for( const auto& item : snapshot.allocations ) if( item.heapId == ui.selectedHeap ) { extent = std::max( extent, item.offsetBytes + item.sizeBytes ); ++count; }
    ImGui::Text( "Heap %llu / exact ranges %zu / extent %s", static_cast<unsigned long long>( ui.selectedHeap ), count, ByteText( extent, ui.decimalUnits ).c_str() );
    const ImVec2 pos = ImGui::GetCursorScreenPos(); const ImVec2 size( ImGui::GetContentRegionAvail().x, 100 * ImGui::GetIO().FontGlobalScale );
    ImGui::InvisibleButton( "heapMap", size ); const auto draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled( pos, ImVec2( pos.x + size.x, pos.y + size.y ), IM_COL32( 45, 45, 50, 255 ) );
    if( extent == 0 ) { draw->AddText( ImVec2( pos.x + 8, pos.y + 8 ), IM_COL32( 160, 160, 160, 255 ), "No exact heap ranges" ); return; }
    for( const auto& item : snapshot.allocations ) if( item.heapId == ui.selectedHeap && item.sizeBytes != 0 )
    {
        const float x0 = pos.x + size.x * float( double( item.offsetBytes ) / double( extent ) );
        const float x1 = pos.x + size.x * float( double( item.offsetBytes + item.sizeBytes ) / double( extent ) );
        const bool selected = ui.selectedAllocation == item.allocationId;
        const auto color = selected ? IM_COL32( 255, 190, 45, 255 ) : item.resources.size() > 1 ? IM_COL32( 180, 85, 210, 255 ) : IM_COL32( 45, 145, 225, 255 );
        draw->AddRectFilled( ImVec2( x0, pos.y + 4 ), ImVec2( std::max( x0 + 1, x1 ), pos.y + size.y - 4 ), color );
    }
}

void DrawPassTable( const analysis::GpuAnalysisSnapshot& snapshot, auto& ui, bool frameValid, int64_t frameBegin, int64_t frameEnd )
{
    const bool globalScope = ui.passScope == 1;
    ImGui::Checkbox( "Show zero-byte / unresolved passes", &ui.showZeroBytePasses );
    ImGui::SameLine(); ImGui::TextDisabled( "%zu hidden; still available in Quality", ui.hiddenZeroBytePasses );
    if( !ImGui::BeginTable( "gpuPassesN27Sortable", globalScope ? 8 : 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable ) ) return;
    ImGui::TableSetupColumn( "Pass", 0, 0, 0 ); if( globalScope ) ImGui::TableSetupColumn( "Producer frame", 0, 0, 1 );
    ImGui::TableSetupColumn( "Direct #", 0, 0, 2 ); ImGui::TableSetupColumn( "Direct WS", ImGuiTableColumnFlags_PreferSortDescending, 0, 3 );
    ImGui::TableSetupColumn( "Inclusive #", 0, 0, 4 );
    ImGui::TableSetupColumn( "Inclusive WS", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending, 0, 5 );
    ImGui::TableSetupColumn( "Range", ImGuiTableColumnFlags_PreferSortDescending, 0, 6 ); ImGui::TableSetupColumn( "Quality", 0, 0, 7 );
    ImGui::TableSetupScrollFreeze( 0, 1 ); ImGui::TableHeadersRow();
    auto* sort = ImGui::TableGetSortSpecs();
    const bool contextChanged = ui.passOrder.size() > snapshot.passes.size() || ui.passOrderScope != ui.passScope ||
        ui.passOrderFrameValid != frameValid || ui.passOrderBegin != frameBegin || ui.passOrderEnd != frameEnd ||
        ui.passOrderShowZero != ui.showZeroBytePasses;
    if( contextChanged || ( sort && sort->SpecsDirty ) || ui.passOrder.empty() )
    {
        ui.passOrder.clear(); ui.hiddenZeroBytePasses = 0;
        for( size_t i = 0; i < snapshot.passes.size(); ++i )
        {
            const auto& pass = snapshot.passes[i];
            if( !globalScope && ( !frameValid || pass.endNs < frameBegin || pass.startNs > frameEnd ) ) continue;
            const bool zeroPlaceholder = pass.directPhysicalBytes == 0 && pass.inclusivePhysicalBytes == 0 && pass.directResources.empty() && pass.inclusiveResources.empty();
            if( zeroPlaceholder && !ui.showZeroBytePasses ) { ++ui.hiddenZeroBytePasses; continue; }
            ui.passOrder.emplace_back( i );
        }
        const auto spec = SortSpecOrDefault( sort, 5, ImGuiSortDirection_Descending );
        std::stable_sort( ui.passOrder.begin(), ui.passOrder.end(), [&]( size_t lhsIndex, size_t rhsIndex ) {
            const auto& lhs = snapshot.passes[lhsIndex]; const auto& rhs = snapshot.passes[rhsIndex]; int cmp = 0;
            const auto compare = [&]( uint64_t lv, uint64_t rv ) { return lv < rv ? -1 : lv > rv ? 1 : 0; };
            switch( spec.ColumnUserID )
            {
            case 0: cmp = lhs.name.compare( rhs.name ); break; case 1: cmp = compare( lhs.frameId, rhs.frameId ); break;
            case 2: cmp = compare( lhs.directResources.size(), rhs.directResources.size() ); break; case 3: cmp = compare( lhs.directPhysicalBytes, rhs.directPhysicalBytes ); break;
            case 4: cmp = compare( lhs.inclusiveResources.size(), rhs.inclusiveResources.size() ); break; case 6: cmp = compare( lhs.directRangeBytes, rhs.directRangeBytes ); break;
            case 7: cmp = lhs.truncated != rhs.truncated ? ( lhs.truncated ? 1 : -1 ) : lhs.complete != rhs.complete ? ( lhs.complete ? -1 : 1 ) : 0; break;
            default: cmp = compare( lhs.inclusivePhysicalBytes, rhs.inclusivePhysicalBytes ); break;
            }
            if( cmp == 0 ) cmp = compare( lhs.passId, rhs.passId );
            return spec.SortDirection == ImGuiSortDirection_Ascending ? cmp < 0 : cmp > 0;
        } );
        ui.passOrderScope = ui.passScope; ui.passOrderFrameValid = frameValid; ui.passOrderBegin = frameBegin; ui.passOrderEnd = frameEnd;
        ui.passOrderShowZero = ui.showZeroBytePasses; if( sort ) sort->SpecsDirty = false;
    }
    const auto row = [&]( const analysis::GpuPassWorkingSet& pass )
    {
        ImGui::TableNextRow(); ImGui::TableNextColumn();
        std::string label = pass.name.empty() ? "Pass " + std::to_string( pass.passId ) : pass.name; label += "##pass" + std::to_string( pass.passId );
        if( ImGui::Selectable( label.c_str(), ui.selectedPass == pass.passId, ImGuiSelectableFlags_SpanAllColumns ) ) ui.selectedPass = pass.passId;
        if( globalScope ) { ImGui::TableNextColumn(); ImGui::Text( "%llu", static_cast<unsigned long long>( pass.frameId ) ); }
        ImGui::TableNextColumn(); ImGui::Text( "%zu", pass.directResources.size() );
        ImGui::TableNextColumn(); ImGui::TextUnformatted( ByteText( pass.directPhysicalBytes, ui.decimalUnits ).c_str() );
        ImGui::TableNextColumn(); ImGui::Text( "%zu", pass.inclusiveResources.size() );
        ImGui::TableNextColumn(); ImGui::TextUnformatted( ByteText( pass.inclusivePhysicalBytes, ui.decimalUnits ).c_str() );
        ImGui::TableNextColumn(); ImGui::TextUnformatted( ByteText( pass.directRangeBytes, ui.decimalUnits ).c_str() );
        ImGui::TableNextColumn(); ImGui::TextUnformatted( pass.truncated ? "Truncated" : pass.complete ? "Exact" : "Partial" );
    };
    ImGuiListClipper clipper; clipper.Begin( int( ui.passOrder.size() ) );
    while( clipper.Step() ) for( int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i ) row( snapshot.passes[ui.passOrder[size_t( i )]] );
    ImGui::EndTable();
}

void DrawPassResourceTable( const analysis::GpuAnalysisSnapshot& snapshot, const analysis::GpuPassWorkingSet& pass, auto& ui )
{
    if( !ImGui::BeginTable( "gpuPassResources", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY ) ) return;
    ImGui::TableSetupColumn( "Resource" ); ImGui::TableSetupColumn( "Name" ); ImGui::TableSetupColumn( "Kind" );
    ImGui::TableSetupColumn( "Capacity" ); ImGui::TableSetupColumn( "Physical" ); ImGui::TableSetupColumn( "Resident" );
    ImGui::TableSetupColumn( "Allocation" ); ImGui::TableSetupColumn( "Range evidence" );
    ImGui::TableSetupScrollFreeze( 0, 1 ); ImGui::TableHeadersRow();
    ImGuiListClipper clipper; clipper.Begin( int( pass.directResources.size() ) );
    while( clipper.Step() ) for( int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i )
    {
        const auto id = pass.directResources[size_t( i )];
        const auto* resource = snapshot.FindResource( id );
        const auto* allocation = resource ? snapshot.FindAllocation( resource->allocationId ) : nullptr;
        ImGui::TableNextRow(); ImGui::TableNextColumn();
        const auto label = std::to_string( id ) + "##passResource" + std::to_string( pass.passId );
        if( ImGui::Selectable( label.c_str(), ui.selectedResource == id, ImGuiSelectableFlags_SpanAllColumns ) )
        { ui.selectedResource = id; ui.selectedAllocation = resource ? resource->allocationId : 0; }
        ImGui::TableNextColumn(); ImGui::TextUnformatted( !resource ? "<unresolved>" : resource->name.empty() ? "<unnamed>" : resource->name.c_str() );
        ImGui::TableNextColumn(); ImGui::TextUnformatted( resource ? analysis::GpuPrimaryKindName( resource->primaryKind ) : "Unavailable" );
        ImGui::TableNextColumn(); ImGui::TextUnformatted( ByteText( resource ? resource->capacityBytes : 0, ui.decimalUnits ).c_str() );
        ImGui::TableNextColumn(); ImGui::TextUnformatted( ByteText( allocation ? allocation->sizeBytes : 0, ui.decimalUnits ).c_str() );
        ImGui::TableNextColumn(); ImGui::TextUnformatted( ByteText( allocation ? allocation->residentBytes : 0, ui.decimalUnits ).c_str() );
        ImGui::TableNextColumn(); if( allocation ) ImGui::Text( "%llu", static_cast<unsigned long long>( allocation->allocationId ) ); else ImGui::TextDisabled( "Unavailable" );
        ImGui::TableNextColumn();
        if( resource && !resource->ranges.empty() ) ImGui::Text( "%zu RangeSet record(s)", resource->ranges.size() );
        else ImGui::TextDisabled( "Unknown / whole-resource evidence" );
    }
    ImGui::EndTable();
}

analysis::GpuFrameComparison CompareGpuFrameIntervals( const analysis::GpuAnalysisSnapshot& snapshot,
    uint64_t frameA, int64_t beginA, int64_t endA, uint64_t frameB, int64_t beginB, int64_t endB )
{
    const auto collect = [&]( int64_t begin, int64_t end )
    {
        std::vector<uint64_t> resources;
        for( const auto& pass : snapshot.passes )
        {
            if( pass.endNs < begin || pass.startNs > end ) continue;
            resources.insert( resources.end(), pass.directResources.begin(), pass.directResources.end() );
        }
        std::sort( resources.begin(), resources.end() );
        resources.erase( std::unique( resources.begin(), resources.end() ), resources.end() );
        return resources;
    };
    const auto physical = [&]( const std::vector<uint64_t>& resources )
    {
        std::vector<uint64_t> allocations;
        uint64_t bytes = 0;
        for( const auto resourceId : resources )
        {
            const auto* resource = snapshot.FindResource( resourceId );
            if( resource && resource->allocationId != 0 ) allocations.emplace_back( resource->allocationId );
        }
        std::sort( allocations.begin(), allocations.end() );
        allocations.erase( std::unique( allocations.begin(), allocations.end() ), allocations.end() );
        for( const auto allocationId : allocations )
            if( const auto* allocation = snapshot.FindAllocation( allocationId ) ) bytes += allocation->sizeBytes;
        return bytes;
    };
    analysis::GpuFrameComparison result;
    result.frameA = frameA; result.frameB = frameB;
    const auto resourcesA = collect( beginA, endA );
    const auto resourcesB = collect( beginB, endB );
    if( resourcesA.empty() && resourcesB.empty() )
    {
        result.unavailableReason = "No GPU pass/resource evidence overlaps either selected Trace frame";
        return result;
    }
    std::set_difference( resourcesB.begin(), resourcesB.end(), resourcesA.begin(), resourcesA.end(), std::back_inserter( result.addedResources ) );
    std::set_difference( resourcesA.begin(), resourcesA.end(), resourcesB.begin(), resourcesB.end(), std::back_inserter( result.removedResources ) );
    result.referencedPhysicalDelta = int64_t( physical( resourcesB ) ) - int64_t( physical( resourcesA ) );
    result.valid = true;
    return result;
}

void DrawChurnTable( const analysis::GpuAnalysisSnapshot& snapshot, auto& ui )
{
    if( !ImGui::BeginTable( "gpuChurn", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY ) ) return;
    ImGui::TableSetupColumn( "Candidate" ); ImGui::TableSetupColumn( "Resource" ); ImGui::TableSetupColumn( "Allocation" ); ImGui::TableSetupColumn( "Bytes" );
    ImGui::TableSetupColumn( "Events" ); ImGui::TableSetupColumn( "Evidence / condition" ); ImGui::TableSetupScrollFreeze( 0, 1 ); ImGui::TableHeadersRow();
    ImGuiListClipper clipper; clipper.Begin( int( snapshot.churnCandidates.size() ) );
    while( clipper.Step() ) for( int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i )
    {
        const auto& item = snapshot.churnCandidates[size_t( i )]; ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted( analysis::GpuChurnCandidateKindName( item.kind ) );
        ImGui::TableNextColumn();
        char label[64]; std::snprintf( label, sizeof( label ), "%llu##churn", static_cast<unsigned long long>( item.resourceId ) );
        if( ImGui::Selectable( label, false, ImGuiSelectableFlags_SpanAllColumns ) ) { ui.selectedResource = item.resourceId; ui.selectedAllocation = item.allocationId; }
        ImGui::TableNextColumn(); ImGui::Text( "%llu", static_cast<unsigned long long>( item.allocationId ) );
        ImGui::TableNextColumn(); ImGui::TextUnformatted( ByteText( item.bytes, ui.decimalUnits ).c_str() );
        ImGui::TableNextColumn(); ImGui::Text( "%llu", static_cast<unsigned long long>( item.eventCount ) );
        ImGui::TableNextColumn(); ImGui::TextUnformatted( item.reason.c_str() );
    }
    ImGui::EndTable();
}

nlohmann::json EvidenceJson( const analysis::GpuAnalysisSnapshot& snapshot, uint64_t resourceId, uint64_t passId )
{
    nlohmann::json out = {
        { "analysis_schema", analysis::GpuAnalysisSchemaVersion }, { "catalog_schema", snapshot.manifest.catalogSchema },
        { "state", analysis::GpuAnalysisStateName( snapshot.manifest.state ) }, { "quality", {
            { "complete", snapshot.manifest.complete }, { "transport_valid", snapshot.manifest.transportValid },
            { "unresolved", snapshot.manifest.unresolvedCount }, { "invalid_records", snapshot.manifest.invalidRecordCount },
            { "reason", snapshot.manifest.reason } } }
    };
    if( const auto resource = snapshot.FindResource( resourceId ) ) out["resource"] = {
        { "id", resource->resourceId }, { "name", resource->name }, { "kind", analysis::GpuPrimaryKindName( resource->primaryKind ) },
        { "capacity_bytes", resource->capacityBytes }, { "allocation_id", resource->allocationId }, { "generation", resource->generation },
        { "definition_revision", resource->definitionRevision }, { "exactness", analysis::GpuExactnessName( resource->exactness ) },
        { "open_boundary", resource->openBoundary }, { "alive_at_end", resource->aliveAtEnd }, { "callsite_id", resource->createCallsiteId } };
    if( const auto pass = snapshot.FindPass( passId ) ) out["pass"] = {
        { "id", pass->passId }, { "name", pass->name }, { "frame", pass->frameId }, { "parent", pass->parentPassId },
        { "direct_resource_count", pass->directResources.size() }, { "inclusive_resource_count", pass->inclusiveResources.size() },
        { "direct_physical_bytes", pass->directPhysicalBytes }, { "inclusive_physical_bytes", pass->inclusivePhysicalBytes },
        { "direct_range_bytes", pass->directRangeBytes }, { "complete", pass->complete }, { "truncated", pass->truncated } };
    return out;
}

bool ExportEvidencePage( const std::filesystem::path& cacheFile, const analysis::GpuAnalysisSnapshot& snapshot, std::string& result )
{
    if( cacheFile.empty() ) { result = "Export unavailable for unsaved live capture"; return false; }
    try
    {
        const auto directory = cacheFile.parent_path() / "exports"; std::filesystem::create_directories( directory );
        const auto jsonPath = directory / "gpu-evidence.json"; const auto csvPath = directory / "gpu-resources-page-000.csv";
        nlohmann::json summary = EvidenceJson( snapshot, 0, 0 ); summary["counts"] = {
            { "resources", snapshot.resources.size() }, { "allocations", snapshot.allocations.size() }, { "passes", snapshot.passes.size() } };
        std::ofstream json( jsonPath, std::ios::binary | std::ios::trunc ); json << summary.dump( 2 ); json.close();
        std::ofstream csv( csvPath, std::ios::binary | std::ios::trunc ); csv << "resource_id,name,kind,capacity_bytes,allocation_id,exactness\n";
        const size_t count = std::min<size_t>( snapshot.resources.size(), 10000 );
        for( size_t i = 0; i < count; ++i )
        {
            const auto& item = snapshot.resources[i]; std::string name = item.name; std::replace( name.begin(), name.end(), '"', '\'' );
            csv << item.resourceId << ",\"" << name << "\"," << analysis::GpuPrimaryKindName( item.primaryKind ) << ',' << item.capacityBytes << ',' << item.allocationId << ',' << analysis::GpuExactnessName( item.exactness ) << '\n';
        }
        csv.close(); result = directory.string() + ( snapshot.resources.size() > count ? " (first 10000 resources; paged export)" : "" ); return bool( json ) && bool( csv );
    }
    catch( const std::exception& e ) { result = e.what(); return false; }
}

}

void View::DrawJnGpuResources()
{
    ImGui::SetNextWindowSize( ImVec2( 1200 * GetScale(), 760 * GetScale() ), ImGuiCond_FirstUseEver );
    if( !ImGui::Begin( ICON_FA_MICROCHIP " GPU Memory & Resources", &m_showJnGpuResources ) ) { ImGui::End(); return; }
    if( m_gpuAnalysis )
    {
        const auto status = m_gpuAnalysis->Status();
        if( status.state == GpuAnalysisControllerState::Idle ) m_gpuAnalysis->Start();
        const auto current = m_gpuAnalysis->Status();
        ImGui::Text( "FrameSet: %s", m_frames ? GetFrameSetName( *m_frames ) : "Global / Lifetime" ); ImGui::SameLine();
        ImGui::TextDisabled( "| analysis %s / %s", GpuAnalysisControllerStateName( current.state ), current.stage.c_str() );
        ImGui::SameLine(); ImGui::Checkbox( "Advanced", &m_jnGpuUi.advanced ); ImGui::SameLine();
        ImGui::Checkbox( "GB/MB", &m_jnGpuUi.decimalUnits );
        if( current.state == GpuAnalysisControllerState::Building )
        {
            ImGui::ProgressBar( current.progress, ImVec2( -90 * GetScale(), 0 ) ); ImGui::SameLine(); if( ImGui::Button( "Cancel" ) ) m_gpuAnalysis->Cancel();
            ImGui::End(); return;
        }
        if( current.state == GpuAnalysisControllerState::NotPresent )
        {
            ImGui::TextColored( ImVec4( 1.f, .75f, .2f, 1.f ), "%s", current.error.empty() ? "N27 GPU Catalog not present" : current.error.c_str() );
            ImGui::TextDisabled( "Timeline, CPU Memory, Jobs and FrameImage remain available." );
            ImGui::End(); return;
        }
        if( current.state == GpuAnalysisControllerState::Failed || current.state == GpuAnalysisControllerState::Cancelled )
        {
            ImGui::TextColored( ImVec4( 1, .25f, .25f, 1 ), "%s", current.error.c_str() );
            if( ImGui::Button( "Retry" ) ) m_gpuAnalysis->Retry(); ImGui::End(); return;
        }
    }
    const auto snapshot = m_gpuAnalysis ? m_gpuAnalysis->Snapshot() : nullptr;
    if( !snapshot ) { ImGui::TextDisabled( "GPU analysis is not ready. Live sessions become analyzable after disconnect/freeze." ); ImGui::End(); return; }
    bool selectedFrameValid = false;
    int64_t selectedFrameBegin = 0;
    int64_t selectedFrameEnd = 0;
    if( m_frames && !m_frames->frames.empty() )
    {
        const int64_t center = m_vd.zvStart + ( m_vd.zvEnd - m_vd.zvStart ) / 2;
        if( m_jnGpuUi.followTimelineFrame || m_jnGpuUi.currentFrameIndex < 0 ||
            m_jnGpuUi.currentFrameIndex >= int( m_frames->frames.size() ) )
        {
            int frameIndex = -1;
            if( FindMemoryFrameAtTime( *m_frames, center, frameIndex ) == MemoryFrameMapping::Valid )
            {
                m_jnGpuUi.currentFrameIndex = frameIndex;
                m_jnGpuUi.currentFrame = GetFrameNumber( *m_frames, frameIndex );
            }
        }
        if( m_jnGpuUi.currentFrameIndex >= 0 && m_jnGpuUi.currentFrameIndex < int( m_frames->frames.size() ) )
        {
            selectedFrameValid = true;
            selectedFrameBegin = m_worker.GetFrameBegin( *m_frames, m_jnGpuUi.currentFrameIndex );
            selectedFrameEnd = m_worker.GetFrameEnd( *m_frames, m_jnGpuUi.currentFrameIndex );
        }
    }
    ImGui::Separator();
    ImGui::SetNextItemWidth( 155 * GetScale() );
    ImGui::Combo( "Scope", &m_jnGpuUi.passScope, "Current Frame\0Global / Lifetime\0" ); ImGui::SameLine();
    if( m_jnGpuUi.passScope == 0 )
    {
        ImGui::SetNextItemWidth( 135 * GetScale() );
        if( ImGui::InputScalar( "Frame", ImGuiDataType_U64, &m_jnGpuUi.currentFrame ) && m_frames )
        {
            m_jnGpuUi.followTimelineFrame = false;
            m_jnGpuUi.currentFrameIndex = -1;
            for( size_t index = 0; index < m_frames->frames.size(); ++index )
                if( GetFrameNumber( *m_frames, int( index ) ) == m_jnGpuUi.currentFrame )
                { m_jnGpuUi.currentFrameIndex = int( index ); break; }
            if( m_jnGpuUi.currentFrameIndex >= 0 )
            {
                selectedFrameValid = true;
                selectedFrameBegin = m_worker.GetFrameBegin( *m_frames, m_jnGpuUi.currentFrameIndex );
                selectedFrameEnd = m_worker.GetFrameEnd( *m_frames, m_jnGpuUi.currentFrameIndex );
            }
            else selectedFrameValid = false;
        }
        ImGui::SameLine();
        ImGui::Checkbox( "Follow timeline", &m_jnGpuUi.followTimelineFrame ); ImGui::SameLine();
    }
    if( m_jnGpuUi.passScope == 0 )
        ImGui::TextDisabled( "FrameSet: %s | Trace frame %llu | interval %s - %s%s",
            m_frames ? GetFrameSetName( *m_frames ) : "No frame set", static_cast<unsigned long long>( m_jnGpuUi.currentFrame ),
            selectedFrameValid ? TimeToStringExact( selectedFrameBegin ) : "Unavailable",
            selectedFrameValid ? TimeToStringExact( selectedFrameEnd ) : "Unavailable",
            selectedFrameValid ? "" : " (no matching frame)" );
    else ImGui::TextDisabled( "FrameSet: %s | scope=global / lifetime | per-pass column is producer frame identity",
        m_frames ? GetFrameSetName( *m_frames ) : "No frame set" );
    if( m_jnGpuUi.passScope == 0 && m_gpuAnalysis && m_jnGpuUi.currentFrame != m_jnGpuUi.requestedGpuPassFrame )
    {
        std::string frameLoadError;
        if( m_gpuAnalysis->LoadPassFrame( m_jnGpuUi.currentFrame, frameLoadError ) ) m_jnGpuUi.requestedGpuPassFrame = m_jnGpuUi.currentFrame;
        else if( !frameLoadError.empty() && frameLoadError != "gpu_analysis_page_load_in_progress" ) m_jnGpuUi.exportStatus = frameLoadError;
    }
    const char* tabs[] = { "Overview", "Resources", "Physical", "Passes", "Lifetime & Churn", "Quality" };
    if( ImGui::BeginTabBar( "gpuResourceTabs" ) )
    {
        for( int i = 0; i < 6; ++i ) if( ImGui::BeginTabItem( tabs[i] ) ) { m_jnGpuUi.tab = i; ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    if( m_jnGpuUi.tab == 0 )
    {
        Metric( "Engine Known Physical", snapshot->engineKnownPhysicalBytes, m_jnGpuUi.decimalUnits, "event-level exact; alias-safe" ); ImGui::SameLine( 220 * GetScale() );
        Metric( "Peak Physical", snapshot->engineKnownPhysicalPeakBytes, m_jnGpuUi.decimalUnits, "event-level peak" ); ImGui::SameLine( 430 * GetScale() );
        Metric( "Resident Physical", snapshot->residentPhysicalBytes, m_jnGpuUi.decimalUnits, "residency evidence only" ); ImGui::SameLine( 640 * GetScale() );
        Metric( "Logical Capacity", snapshot->logicalCapacityBytes, m_jnGpuUi.decimalUnits, "resource capacity; not additive with physical" );
        const auto projectBudget = uint64_t( m_jnGpuUi.projectBudgetGb * 1000.0 * 1000.0 * 1000.0 );
        ImGui::SetNextItemWidth( 160 * GetScale() ); ImGui::InputDouble( "Project GPU budget (GB)", &m_jnGpuUi.projectBudgetGb, .1, 1, "%.2f" );
        const double ratio = projectBudget ? double( snapshot->engineKnownPhysicalBytes ) / double( projectBudget ) : 0;
        ImGui::ProgressBar( float( std::min( ratio, 1.0 ) ), ImVec2( -1, 0 ), ratio > 1 ? "OverProjectBudget" : "Project budget" );
        ImGui::SeparatorText( "Capture summary" );
        ImGui::Text( "Resources %llu / Allocations %llu / Passes %llu / Residency intervals %llu",
            static_cast<unsigned long long>( m_gpuAnalysis->ResourceCount() ),
            static_cast<unsigned long long>( m_gpuAnalysis->AllocationCount() ),
            static_cast<unsigned long long>( m_gpuAnalysis->PassCount() ),
            static_cast<unsigned long long>( m_gpuAnalysis->ResidencyCount() ) );
        ImGui::Text( "DXGI Local Usage/Budget: Unavailable in this snapshot" );
        ImGui::Text( "Untracked DXGI Delta: Unavailable (not coerced to zero)" );
        ImGui::TextDisabled( "Peak time: %lld ns", static_cast<long long>( snapshot->engineKnownPhysicalPeakTimeNs ) );
    }
    else if( m_jnGpuUi.tab == 1 )
    {
        const auto pageCount = m_gpuAnalysis->ResourcePageCount(); const auto page = m_gpuAnalysis->ResourcePage(); std::string pageError;
        if( ImGui::Button( "< Resource page" ) && page > 0 )
        { if( m_gpuAnalysis->LoadResourcePage( page - 1, pageError ) ) { m_jnGpuUi.resourceOrder.clear(); m_jnGpuUi.selectedResource = 0; } }
        ImGui::SameLine(); ImGui::Text( "Page %zu / %zu (total records %llu)", pageCount ? page + 1 : 0, pageCount,
            static_cast<unsigned long long>( m_gpuAnalysis->ResourceCount() ) ); ImGui::SameLine();
        if( ImGui::Button( "Resource page >" ) && page + 1 < pageCount )
        { if( m_gpuAnalysis->LoadResourcePage( page + 1, pageError ) ) { m_jnGpuUi.resourceOrder.clear(); m_jnGpuUi.selectedResource = 0; } }
        if( !pageError.empty() ) ImGui::TextColored( ImVec4( 1.f, .3f, .3f, 1.f ), "%s", pageError.c_str() );
        ImGui::SetNextItemWidth( -1 ); ImGui::InputTextWithHint( "##gpuSearch", "Search name, ID or type", m_jnGpuUi.search, sizeof( m_jnGpuUi.search ) );
        ImGui::BeginChild( "gpuResourceList", ImVec2( ImGui::GetContentRegionAvail().x * .60f, 0 ), ImGuiChildFlags_Borders ); DrawResourceTable( *snapshot, m_jnGpuUi ); ImGui::EndChild();
        ImGui::SameLine(); ImGui::BeginChild( "gpuResourceInspector", ImVec2( 0, 0 ), ImGuiChildFlags_Borders );
        DrawResourceInspector( *snapshot, snapshot->FindResource( m_jnGpuUi.selectedResource ), m_jnGpuUi.decimalUnits ); ImGui::EndChild();
    }
    else if( m_jnGpuUi.tab == 2 )
    {
        const auto pageCount = m_gpuAnalysis->AllocationPageCount(); const auto page = m_gpuAnalysis->AllocationPage(); std::string pageError;
        if( ImGui::Button( "< Allocation page" ) && page > 0 )
        { if( m_gpuAnalysis->LoadAllocationPage( page - 1, pageError ) ) { m_jnGpuUi.allocationOrder.clear(); m_jnGpuUi.selectedAllocation = 0; } }
        ImGui::SameLine(); ImGui::Text( "Page %zu / %zu (total records %llu)", pageCount ? page + 1 : 0, pageCount,
            static_cast<unsigned long long>( m_gpuAnalysis->AllocationCount() ) ); ImGui::SameLine();
        if( ImGui::Button( "Allocation page >" ) && page + 1 < pageCount )
        { if( m_gpuAnalysis->LoadAllocationPage( page + 1, pageError ) ) { m_jnGpuUi.allocationOrder.clear(); m_jnGpuUi.selectedAllocation = 0; } }
        if( !pageError.empty() ) ImGui::TextColored( ImVec4( 1.f, .3f, .3f, 1.f ), "%s", pageError.c_str() );
        DrawHeapMap( *snapshot, m_jnGpuUi ); ImGui::BeginChild( "gpuAllocationList", ImVec2( ImGui::GetContentRegionAvail().x * .60f, 0 ), ImGuiChildFlags_Borders );
        DrawAllocationTable( *snapshot, m_jnGpuUi ); ImGui::EndChild(); ImGui::SameLine(); ImGui::BeginChild( "gpuAllocationInspector", ImVec2( 0, 0 ), ImGuiChildFlags_Borders );
        const auto allocation = snapshot->FindAllocation( m_jnGpuUi.selectedAllocation );
        if( allocation )
        {
            ImGui::Text( "Allocation %llu", static_cast<unsigned long long>( allocation->allocationId ) );
            ImGui::Text( "Heap %llu / parent %llu", static_cast<unsigned long long>( allocation->heapId ), static_cast<unsigned long long>( allocation->parentAllocationId ) );
            ImGui::Text( "Size %s / resident %s", ByteText( allocation->sizeBytes, m_jnGpuUi.decimalUnits ).c_str(), ByteText( allocation->residentBytes, m_jnGpuUi.decimalUnits ).c_str() );
            ImGui::Text( "Resources %zu / exactness %s", allocation->resources.size(), analysis::GpuExactnessName( allocation->exactness ) );
        }
        else ImGui::TextDisabled( "Select an allocation." ); ImGui::EndChild();
    }
    else if( m_jnGpuUi.tab == 3 )
    {
        const auto pageCount = m_gpuAnalysis->PassPageCount(); const auto page = m_gpuAnalysis->PassPage(); std::string pageError;
        if( ImGui::Button( "< Pass page" ) && page > 0 )
        { if( m_gpuAnalysis->LoadPassPage( page - 1, pageError ) ) { m_jnGpuUi.requestedGpuPassFrame = ~uint64_t( 0 ); m_jnGpuUi.passOrder.clear(); m_jnGpuUi.selectedPass = 0; } }
        ImGui::SameLine(); ImGui::Text( "Page %zu / %zu", pageCount ? page + 1 : 0, pageCount ); ImGui::SameLine();
        if( ImGui::Button( "Pass page >" ) && page + 1 < pageCount )
        { if( m_gpuAnalysis->LoadPassPage( page + 1, pageError ) ) { m_jnGpuUi.requestedGpuPassFrame = ~uint64_t( 0 ); m_jnGpuUi.passOrder.clear(); m_jnGpuUi.selectedPass = 0; } }
        if( !pageError.empty() ) ImGui::TextColored( ImVec4( 1.f, .3f, .3f, 1.f ), "%s", pageError.c_str() );
        ImGui::SetNextItemWidth( 130 * GetScale() ); ImGui::InputScalar( "Frame A", ImGuiDataType_U64, &m_jnGpuUi.frameA ); ImGui::SameLine();
        ImGui::SetNextItemWidth( 130 * GetScale() ); ImGui::InputScalar( "Frame B", ImGuiDataType_U64, &m_jnGpuUi.frameB );
        if( m_jnGpuUi.frameA && m_jnGpuUi.frameB )
        {
            const auto frameInterval = [&]( uint64_t frameNumber, int64_t& begin, int64_t& end )
            {
                if( !m_frames ) return false;
                for( size_t index = 0; index < m_frames->frames.size(); ++index )
                {
                    if( GetFrameNumber( *m_frames, int( index ) ) != frameNumber ) continue;
                    begin = m_worker.GetFrameBegin( *m_frames, index ); end = m_worker.GetFrameEnd( *m_frames, index ); return true;
                }
                return false;
            };
            int64_t beginA = 0, endA = 0, beginB = 0, endB = 0;
            ImGui::SameLine();
            if( frameInterval( m_jnGpuUi.frameA, beginA, endA ) && frameInterval( m_jnGpuUi.frameB, beginB, endB ) )
            {
                const auto comparison = CompareGpuFrameIntervals( *snapshot, m_jnGpuUi.frameA, beginA, endA,
                    m_jnGpuUi.frameB, beginB, endB );
                if( comparison.valid ) ImGui::Text( "B-A referenced physical: %+.3f MiB / +%zu -%zu resources",
                    double( comparison.referencedPhysicalDelta ) / ( 1024.0 * 1024.0 ), comparison.addedResources.size(), comparison.removedResources.size() );
                else ImGui::TextColored( ImVec4( 1.f, .75f, .2f, 1.f ), "%s", comparison.unavailableReason.c_str() );
            }
            else ImGui::TextColored( ImVec4( 1.f, .75f, .2f, 1.f ), "Frame A/B is not present in the selected FrameSet" );
        }
        ImGui::BeginChild( "gpuPassList", ImVec2( ImGui::GetContentRegionAvail().x * .60f, 0 ), ImGuiChildFlags_Borders );
        DrawPassTable( *snapshot, m_jnGpuUi, selectedFrameValid, selectedFrameBegin, selectedFrameEnd ); ImGui::EndChild();
        ImGui::SameLine(); ImGui::BeginChild( "gpuPassInspector", ImVec2( 0, 0 ), ImGuiChildFlags_Borders );
        if( const auto pass = snapshot->FindPass( m_jnGpuUi.selectedPass ) )
        {
            int passFrameIndex = -1;
            const bool hasTraceFrame = m_frames && FindMemoryFrameAtTime( *m_frames, pass->startNs, passFrameIndex ) == MemoryFrameMapping::Valid;
            ImGui::Text( "%s", pass->name.empty() ? "Unnamed pass" : pass->name.c_str() );
            ImGui::TextDisabled( "Pass %llu / Trace frame %s / Producer frame %llu / CommandList %llu",
                static_cast<unsigned long long>( pass->passId ), hasTraceFrame ? RealToString( GetFrameNumber( *m_frames, passFrameIndex ) ) : "Unavailable",
                static_cast<unsigned long long>( pass->frameId ), static_cast<unsigned long long>( pass->commandListId ) );
            if( ImGui::Button( "Focus timeline" ) && pass->endNs > pass->startNs ) { PushEvidenceNavigation(); ZoomToRange( pass->startNs, pass->endNs ); }
            ImGui::SameLine(); if( ImGui::Button( "Set A" ) && hasTraceFrame ) m_jnGpuUi.frameA = GetFrameNumber( *m_frames, passFrameIndex );
            ImGui::SameLine(); if( ImGui::Button( "Set B" ) && hasTraceFrame ) m_jnGpuUi.frameB = GetFrameNumber( *m_frames, passFrameIndex );
            ImGui::SeparatorText( "Working set" );
            ImGui::Text( "Direct %zu / %s", pass->directResources.size(), ByteText( pass->directPhysicalBytes, m_jnGpuUi.decimalUnits ).c_str() );
            ImGui::Text( "Inclusive %zu / %s", pass->inclusiveResources.size(), ByteText( pass->inclusivePhysicalBytes, m_jnGpuUi.decimalUnits ).c_str() );
            ImGui::Text( "Direct range %s / unknown range resources %u", ByteText( pass->directRangeBytes, m_jnGpuUi.decimalUnits ).c_str(), pass->unknownRangeResourceCount );
            ImGui::Text( "Quality: %s%s", pass->complete ? "Exact" : "Partial", pass->truncated ? " / Truncated" : "" );
            ImGui::SeparatorText( "Direct resources (select a row, then open Resources tab for full evidence)" );
            DrawPassResourceTable( *snapshot, *pass, m_jnGpuUi );
        }
        else ImGui::TextDisabled( "Select a pass." ); ImGui::EndChild();
    }
    else if( m_jnGpuUi.tab == 4 )
    {
        ImGui::TextWrapped( "These are evidence-backed candidates, not leak declarations. P95/P99 ranking and capture boundary quality must be considered." );
        DrawChurnTable( *snapshot, m_jnGpuUi );
    }
    else if( m_jnGpuUi.tab == 5 )
    {
        const auto& manifest = snapshot->manifest;
        ImGui::Text( "Analysis: %s", analysis::GpuAnalysisStateName( manifest.state ) ); ImGui::SameLine();
        ImGui::TextColored( manifest.complete ? ImVec4( .25f, .85f, .35f, 1 ) : ImVec4( 1.f, .75f, .2f, 1 ), "%s", manifest.complete ? "Complete" : "Partial / Invalid" );
        ImGui::Text( "Catalog Schema %u / Evidence Schema %u / generations %llu", manifest.catalogSchema, manifest.evidenceSchema, static_cast<unsigned long long>( manifest.generationCount ) );
        if( ImGui::BeginTable( "gpuQualityCounts", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg ) )
        {
            const auto qualityRow = []( const char* name, uint64_t value, const char* quality )
            { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted( name ); ImGui::TableNextColumn(); ImGui::Text( "%llu", static_cast<unsigned long long>( value ) ); ImGui::TableNextColumn(); ImGui::TextUnformatted( quality ); };
            qualityRow( "Resource records", manifest.resourceRecordCount, "Catalog Core" ); qualityRow( "Allocation records", manifest.allocationRecordCount, "Catalog Core" );
            qualityRow( "View records", manifest.viewRecordCount, "Enrichment" ); qualityRow( "Part records", manifest.partRecordCount, "Enrichment" );
            qualityRow( "Relation records", manifest.relationRecordCount, "Typed relations" ); qualityRow( "Range records", manifest.rangeRecordCount, "GpuRangeSetV1" );
            qualityRow( "VG records", manifest.vgRecordCount, "VG adapter" ); qualityRow( "Detailed evidence", manifest.evidenceRecordCount, "Periodic / Manual" );
            qualityRow( "Unresolved", manifest.unresolvedCount, manifest.unresolvedCount ? "Unavailable / partial" : "Complete" );
            qualityRow( "Invalid records", manifest.invalidRecordCount, manifest.invalidRecordCount ? "InvalidRecord" : "Valid" ); qualityRow( "Payload bytes", manifest.payloadBytes, "binary transport" );
            ImGui::EndTable();
        }
        if( !manifest.reason.empty() ) ImGui::TextWrapped( "Reason: %s", manifest.reason.c_str() );
        ImGui::SeparatorText( "Evidence export" );
        const auto evidence = EvidenceJson( *snapshot, m_jnGpuUi.selectedResource, m_jnGpuUi.selectedPass );
        if( ImGui::Button( "Copy Evidence Summary" ) )
        {
            std::ostringstream summary; summary << "GPU Catalog " << analysis::GpuAnalysisStateName( manifest.state ) << "; resources=" << snapshot->resources.size()
                << "; allocations=" << snapshot->allocations.size() << "; passes=" << snapshot->passes.size() << "; unresolved=" << manifest.unresolvedCount;
            ImGui::SetClipboardText( summary.str().c_str() );
        }
        ImGui::SameLine(); if( ImGui::Button( "Copy Query JSON" ) ) { const auto text = evidence.dump( 2 ); if( text.size() <= 16 * 1024 * 1024 ) ImGui::SetClipboardText( text.c_str() ); }
        ImGui::SameLine(); if( ImGui::Button( "Export CSV/JSON + Diagnostics" ) ) ExportEvidencePage( m_gpuAnalysis->CachePath(), *snapshot, m_jnGpuUi.exportStatus );
        if( !m_jnGpuUi.exportStatus.empty() ) ImGui::TextWrapped( "%s", m_jnGpuUi.exportStatus.c_str() );
        std::string cacheError;
        if( ImGui::Button( "Clear GPU cache" ) ) { if( m_gpuAnalysis->ClearCache( cacheError ) ) m_jnGpuUi.exportStatus = "Cache cleared; Retry to rebuild"; else m_jnGpuUi.exportStatus = cacheError; }
        ImGui::SeparatorText( "FrameImage" );
        ImGui::Text( "Images: %u", m_worker.GetFrameImageCount() ); ImGui::SameLine();
        if( ButtonDisablable( "Open FrameImage playback", m_worker.GetFrameImageCount() == 0 ) ) m_showPlayback = true;
        ImGui::TextDisabled( "NotSampled intervals and absent DXGI evidence remain unavailable; they are never rendered as zero." );
    }
    ImGui::End();
}

}

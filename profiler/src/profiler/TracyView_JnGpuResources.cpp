#include "TracyView.hpp"

#include "TracyGpuAnalysisController.hpp"
#include "TracyImGui.hpp"

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
    if( !ImGui::BeginTable( "gpuResources", 6, flags ) ) return;
    ImGui::TableSetupColumn( "ID", ImGuiTableColumnFlags_WidthFixed, 75 ); ImGui::TableSetupColumn( "Name", ImGuiTableColumnFlags_WidthStretch );
    ImGui::TableSetupColumn( "Kind", ImGuiTableColumnFlags_WidthFixed, 120 ); ImGui::TableSetupColumn( "Capacity", ImGuiTableColumnFlags_WidthFixed, 95 );
    ImGui::TableSetupColumn( "Allocation", ImGuiTableColumnFlags_WidthFixed, 85 ); ImGui::TableSetupColumn( "Quality", ImGuiTableColumnFlags_WidthFixed, 90 );
    ImGui::TableSetupScrollFreeze( 0, 1 ); ImGui::TableHeadersRow();
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
        ImGui::TableNextColumn(); ImGui::Text( "%llu", static_cast<unsigned long long>( item.allocationId ) );
        ImGui::TableNextColumn(); ImGui::TextUnformatted( item.invalid ? "Invalid" : analysis::GpuExactnessName( item.exactness ) );
    };
    if( filtered ) { for( const auto& item : snapshot.resources ) if( ResourceMatches( item, ui.search ) ) row( item ); }
    else
    {
        ImGuiListClipper clipper; clipper.Begin( int( snapshot.resources.size() ) );
        while( clipper.Step() ) for( int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i ) row( snapshot.resources[size_t( i )] );
    }
    ImGui::EndTable();
}

void DrawAllocationTable( const analysis::GpuAnalysisSnapshot& snapshot, auto& ui )
{
    if( !ImGui::BeginTable( "gpuAllocations", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY ) ) return;
    ImGui::TableSetupColumn( "Allocation" ); ImGui::TableSetupColumn( "Heap" ); ImGui::TableSetupColumn( "Offset" ); ImGui::TableSetupColumn( "Size" );
    ImGui::TableSetupColumn( "Resident" ); ImGui::TableSetupColumn( "Resources" ); ImGui::TableSetupColumn( "State" ); ImGui::TableSetupScrollFreeze( 0, 1 ); ImGui::TableHeadersRow();
    ImGuiListClipper clipper; clipper.Begin( int( snapshot.allocations.size() ) );
    while( clipper.Step() ) for( int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i )
    {
        const auto& item = snapshot.allocations[size_t( i )]; ImGui::TableNextRow(); ImGui::TableNextColumn();
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

void DrawPassTable( const analysis::GpuAnalysisSnapshot& snapshot, auto& ui )
{
    if( !ImGui::BeginTable( "gpuPasses", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY ) ) return;
    ImGui::TableSetupColumn( "Pass" ); ImGui::TableSetupColumn( "Frame" ); ImGui::TableSetupColumn( "Direct #" ); ImGui::TableSetupColumn( "Direct WS" );
    ImGui::TableSetupColumn( "Inclusive #" ); ImGui::TableSetupColumn( "Inclusive WS" ); ImGui::TableSetupColumn( "Range" ); ImGui::TableSetupColumn( "Quality" );
    ImGui::TableSetupScrollFreeze( 0, 1 ); ImGui::TableHeadersRow();
    ImGuiListClipper clipper; clipper.Begin( int( snapshot.passes.size() ) );
    while( clipper.Step() ) for( int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i )
    {
        const auto& pass = snapshot.passes[size_t( i )]; ImGui::TableNextRow(); ImGui::TableNextColumn();
        std::string label = pass.name.empty() ? "Pass " + std::to_string( pass.passId ) : pass.name; label += "##pass" + std::to_string( pass.passId );
        if( ImGui::Selectable( label.c_str(), ui.selectedPass == pass.passId, ImGuiSelectableFlags_SpanAllColumns ) ) ui.selectedPass = pass.passId;
        ImGui::TableNextColumn(); ImGui::Text( "%llu", static_cast<unsigned long long>( pass.frameId ) );
        ImGui::TableNextColumn(); ImGui::Text( "%zu", pass.directResources.size() );
        ImGui::TableNextColumn(); ImGui::TextUnformatted( ByteText( pass.directPhysicalBytes, ui.decimalUnits ).c_str() );
        ImGui::TableNextColumn(); ImGui::Text( "%zu", pass.inclusiveResources.size() );
        ImGui::TableNextColumn(); ImGui::TextUnformatted( ByteText( pass.inclusivePhysicalBytes, ui.decimalUnits ).c_str() );
        ImGui::TableNextColumn(); ImGui::TextUnformatted( ByteText( pass.directRangeBytes, ui.decimalUnits ).c_str() );
        ImGui::TableNextColumn(); ImGui::TextUnformatted( pass.truncated ? "Truncated" : pass.complete ? "Exact" : "Partial" );
    }
    ImGui::EndTable();
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
        if( current.state == GpuAnalysisControllerState::Failed || current.state == GpuAnalysisControllerState::Cancelled )
        {
            ImGui::TextColored( ImVec4( 1, .25f, .25f, 1 ), "%s", current.error.c_str() );
            if( ImGui::Button( "Retry" ) ) m_gpuAnalysis->Retry(); ImGui::End(); return;
        }
    }
    const auto snapshot = m_gpuAnalysis ? m_gpuAnalysis->Snapshot() : nullptr;
    if( !snapshot ) { ImGui::TextDisabled( "GPU analysis is not ready. Live sessions become analyzable after disconnect/freeze." ); ImGui::End(); return; }
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
        ImGui::Text( "Resources %zu / Allocations %zu / Passes %zu / Residency intervals %zu", snapshot->resources.size(), snapshot->allocations.size(), snapshot->passes.size(), snapshot->residency.size() );
        ImGui::Text( "DXGI Local Usage/Budget: Unavailable in this snapshot" );
        ImGui::Text( "Untracked DXGI Delta: Unavailable (not coerced to zero)" );
        ImGui::TextDisabled( "Peak time: %lld ns", static_cast<long long>( snapshot->engineKnownPhysicalPeakTimeNs ) );
    }
    else if( m_jnGpuUi.tab == 1 )
    {
        ImGui::SetNextItemWidth( -1 ); ImGui::InputTextWithHint( "##gpuSearch", "Search name, ID or type", m_jnGpuUi.search, sizeof( m_jnGpuUi.search ) );
        ImGui::BeginChild( "gpuResourceList", ImVec2( ImGui::GetContentRegionAvail().x * .60f, 0 ), ImGuiChildFlags_Borders ); DrawResourceTable( *snapshot, m_jnGpuUi ); ImGui::EndChild();
        ImGui::SameLine(); ImGui::BeginChild( "gpuResourceInspector", ImVec2( 0, 0 ), ImGuiChildFlags_Borders );
        DrawResourceInspector( *snapshot, snapshot->FindResource( m_jnGpuUi.selectedResource ), m_jnGpuUi.decimalUnits ); ImGui::EndChild();
    }
    else if( m_jnGpuUi.tab == 2 )
    {
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
        ImGui::SetNextItemWidth( 130 * GetScale() ); ImGui::InputScalar( "Frame A", ImGuiDataType_U64, &m_jnGpuUi.frameA ); ImGui::SameLine();
        ImGui::SetNextItemWidth( 130 * GetScale() ); ImGui::InputScalar( "Frame B", ImGuiDataType_U64, &m_jnGpuUi.frameB );
        if( m_jnGpuUi.frameA && m_jnGpuUi.frameB )
        {
            const auto comparison = analysis::CompareGpuFrames( *snapshot, m_jnGpuUi.frameA, m_jnGpuUi.frameB ); ImGui::SameLine();
            if( comparison.valid ) ImGui::Text( "B-A referenced physical: %+.3f MiB / +%zu -%zu resources",
                double( comparison.referencedPhysicalDelta ) / ( 1024.0 * 1024.0 ), comparison.addedResources.size(), comparison.removedResources.size() );
            else ImGui::TextColored( ImVec4( 1.f, .75f, .2f, 1.f ), "%s", comparison.unavailableReason.c_str() );
        }
        ImGui::BeginChild( "gpuPassList", ImVec2( ImGui::GetContentRegionAvail().x * .60f, 0 ), ImGuiChildFlags_Borders ); DrawPassTable( *snapshot, m_jnGpuUi ); ImGui::EndChild();
        ImGui::SameLine(); ImGui::BeginChild( "gpuPassInspector", ImVec2( 0, 0 ), ImGuiChildFlags_Borders );
        if( const auto pass = snapshot->FindPass( m_jnGpuUi.selectedPass ) )
        {
            ImGui::Text( "%s", pass->name.empty() ? "Unnamed pass" : pass->name.c_str() ); ImGui::TextDisabled( "Pass %llu / Frame %llu / CommandList %llu",
                static_cast<unsigned long long>( pass->passId ), static_cast<unsigned long long>( pass->frameId ), static_cast<unsigned long long>( pass->commandListId ) );
            if( ImGui::Button( "Focus timeline" ) && pass->endNs > pass->startNs ) { PushEvidenceNavigation(); ZoomToRange( pass->startNs, pass->endNs ); }
            ImGui::SameLine(); if( ImGui::Button( "Set A" ) ) m_jnGpuUi.frameA = pass->frameId; ImGui::SameLine(); if( ImGui::Button( "Set B" ) ) m_jnGpuUi.frameB = pass->frameId;
            ImGui::SeparatorText( "Working set" );
            ImGui::Text( "Direct %zu / %s", pass->directResources.size(), ByteText( pass->directPhysicalBytes, m_jnGpuUi.decimalUnits ).c_str() );
            ImGui::Text( "Inclusive %zu / %s", pass->inclusiveResources.size(), ByteText( pass->inclusivePhysicalBytes, m_jnGpuUi.decimalUnits ).c_str() );
            ImGui::Text( "Direct range %s / unknown range resources %u", ByteText( pass->directRangeBytes, m_jnGpuUi.decimalUnits ).c_str(), pass->unknownRangeResourceCount );
            ImGui::Text( "Quality: %s%s", pass->complete ? "Exact" : "Partial", pass->truncated ? " / Truncated" : "" );
            ImGui::SeparatorText( "Direct resources" );
            const size_t limit = std::min<size_t>( pass->directResources.size(), 256 );
            for( size_t i = 0; i < limit; ++i )
            {
                const auto id = pass->directResources[i]; const auto resource = snapshot->FindResource( id );
                std::string label = resource && !resource->name.empty() ? resource->name : "Resource " + std::to_string( id ); label += "##passResource" + std::to_string( id );
                if( ImGui::Selectable( label.c_str() ) ) { m_jnGpuUi.selectedResource = id; m_jnGpuUi.tab = 1; }
            }
            if( pass->directResources.size() > limit ) ImGui::TextDisabled( "%zu more; use paged Query/export", pass->directResources.size() - limit );
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

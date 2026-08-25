#include "TracyView.hpp"

#include "TracyGpuAnalysisController.hpp"
#include "TracyImGui.hpp"
#include "TracyProtocol.hpp"

#include "imgui.h"
#include "IconsFontAwesome6.h"

#include <cstring>

namespace tracy
{
namespace
{

enum class DomainState : uint8_t { Complete, Partial, Invalid, NotCaptured, Unsupported };

const char* DomainStateName( DomainState state )
{
    switch( state )
    {
    case DomainState::Complete: return "Complete";
    case DomainState::Partial: return "Partial";
    case DomainState::Invalid: return "Invalid";
    case DomainState::NotCaptured: return "NotCaptured";
    case DomainState::Unsupported: return "Unsupported";
    }
    return "Unknown";
}

ImVec4 DomainStateColor( DomainState state )
{
    switch( state )
    {
    case DomainState::Complete: return ImVec4( 0.25f, 0.85f, 0.35f, 1.f );
    case DomainState::Partial: return ImVec4( 0.95f, 0.75f, 0.2f, 1.f );
    case DomainState::Invalid: return ImVec4( 1.f, 0.25f, 0.25f, 1.f );
    case DomainState::NotCaptured:
    case DomainState::Unsupported: return ImVec4( 0.55f, 0.55f, 0.55f, 1.f );
    }
    return ImVec4( 1, 1, 1, 1 );
}

void DomainRow( const char* name, DomainState state, uint64_t records, const char* reason )
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn(); ImGui::TextUnformatted( name );
    ImGui::TableNextColumn(); ImGui::TextColored( DomainStateColor( state ), "%s", DomainStateName( state ) );
    ImGui::TableNextColumn(); ImGui::Text( "%llu", static_cast<unsigned long long>( records ) );
    ImGui::TableNextColumn(); ImGui::TextWrapped( "%s", reason && *reason ? reason : "-" );
}

}

void View::DrawJnCaptureOverview()
{
    ImGui::SetNextWindowSize( ImVec2( 900 * GetScale(), 650 * GetScale() ), ImGuiCond_FirstUseEver );
    if( !ImGui::Begin( ICON_FA_GAUGE_HIGH " JN Unity Capture Overview", &m_showJnCaptureOverview ) )
    {
        ImGui::End();
        return;
    }

    ImGui::TextColored( ImVec4( 0.3f, 0.8f, 1.f, 1.f ), "JN Unity GUI Preview" );
    ImGui::SameLine();
    ImGui::TextDisabled( "Analysis Schema 1 / Cache Schema 1" );

    if( ImGui::BeginTable( "jnIdentity", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp ) )
    {
        const auto identity = [&]( const char* label, const char* value )
        {
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled( "%s", label );
            ImGui::TableNextColumn(); ImGui::TextUnformatted( value && *value ? value : "Unavailable" );
        };
        identity( "Capture", m_worker.GetCaptureName().c_str() );
        identity( "Program", m_worker.GetCaptureProgram().c_str() );
        identity( "Host", m_worker.GetHostInfo().c_str() );
        identity( "Trace path", m_traceFilename.empty() ? "Live / unsaved" : m_traceFilename.c_str() );
        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled( "Protocol" );
        ImGui::TableNextColumn(); ImGui::Text( "%u", ProtocolVersion );
        const auto& jn = m_worker.GetJnTraceData();
        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled( "JN trace section" );
        ImGui::TableNextColumn(); ImGui::Text( "%u", unsigned( jn.schemaVersion ) );
        ImGui::EndTable();
    }

    ImGui::SeparatorText( "Capture domains" );
    const auto& jn = m_worker.GetJnTraceData();
    uint64_t cpuMemoryEventCount = 0;
    for( const auto& pool : m_worker.GetMemNameMap() )
        if( !IsGpuD3D12MemoryPool( pool.first ) ) cpuMemoryEventCount += pool.second->data.size();
    uint64_t cpuMemoryAggregatePoints = 0;
    uint64_t cpuMemoryAggregateLabels = 0;
    for( const auto* plot : m_worker.GetPlots() )
    {
        const auto* name = plot->name == 0 ? nullptr : m_worker.GetString( plot->name );
        if( !name || strncmp( name, "JN.CPU.Memory.", 14 ) != 0 ) continue;
        const auto length = strlen( name );
        static constexpr const char* suffix = ".ActiveBytes";
        static constexpr size_t suffixLength = 12;
        if( length <= suffixLength || strcmp( name + length - suffixLength, suffix ) != 0 ) continue;
        cpuMemoryAggregateLabels++;
        cpuMemoryAggregatePoints += plot->data.size();
    }
    if( ImGui::BeginTable( "jnDomains", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
        ImVec2( 0, 240 * GetScale() ) ) )
    {
        ImGui::TableSetupColumn( "Domain", ImGuiTableColumnFlags_WidthFixed, 180 * GetScale() );
        ImGui::TableSetupColumn( "Status", ImGuiTableColumnFlags_WidthFixed, 120 * GetScale() );
        ImGui::TableSetupColumn( "Records", ImGuiTableColumnFlags_WidthFixed, 110 * GetScale() );
        ImGui::TableSetupColumn( "Quality / reason", ImGuiTableColumnFlags_WidthStretch );
        ImGui::TableSetupScrollFreeze( 0, 1 ); ImGui::TableHeadersRow();
        DomainRow( "CPU Zones", m_worker.GetZoneCount() ? DomainState::Complete : DomainState::NotCaptured, m_worker.GetZoneCount(), "timeline" );
        DomainRow( "Unity Jobs", !jn.present ? DomainState::NotCaptured : ( jn.jobSchedules.empty() ? DomainState::Partial : DomainState::Complete ),
            jn.jobSchedules.size(), !jn.present ? "JN section not present" : ( jn.jobSchedules.empty() ? "no schedules" : "schedule/dependency/stage" ) );
        DomainRow( "GPU Zones", m_worker.GetGpuZoneCount() ? DomainState::Complete : DomainState::NotCaptured, m_worker.GetGpuZoneCount(), "physical queue timelines" );
        const auto catalogState = !jn.gpuCatalogPresent ? DomainState::NotCaptured : ( jn.gpuCatalogValid ? DomainState::Complete : DomainState::Invalid );
        DomainRow( "GPU Resource Catalog", catalogState, jn.gpuCatalogResources.size(),
            !jn.gpuCatalogPresent ? "N27 GPU Catalog not present" : ( jn.gpuCatalogValid ? "Protocol 90 / Catalog Schema 1" : "invalid generation or transport" ) );
        DomainRow( "GPU Physical Memory", jn.gpuCatalogAllocations.empty() ? DomainState::NotCaptured : catalogState,
            jn.gpuCatalogAllocations.size(), jn.gpuCatalogAllocations.empty() ? "no allocation records" : "allocation/heap records" );
        DomainRow( "GPU Pass References", jn.gpuReferenceUses.empty() ? DomainState::NotCaptured : DomainState::Complete,
            jn.gpuReferenceUses.size(), jn.gpuReferenceUses.empty() ? "not captured" : "ResourceSetV2" );
        const auto cpuMemoryState = cpuMemoryEventCount == 0 && cpuMemoryAggregateLabels == 0 ? DomainState::NotCaptured : DomainState::Partial;
        DomainRow( "CPU Memory", cpuMemoryState, cpuMemoryEventCount + cpuMemoryAggregatePoints,
            cpuMemoryAggregateLabels == 0 ? "filtered allocation window only; no aggregate baseline" :
            cpuMemoryEventCount == 0 ? "AggregateBaseline; individual allocation events not captured" :
            "AggregateBaseline + FilteredAllocationWindow" );
        DomainRow( "Sampling", m_worker.GetCallstackSampleCount() ? DomainState::Complete : DomainState::NotCaptured,
            m_worker.GetCallstackSampleCount(), m_worker.GetCallstackSampleCount() ? "native samples" : "not captured" );
        DomainRow( "Context Switch", m_worker.GetContextSwitchSampleCount() ? DomainState::Complete : DomainState::NotCaptured,
            m_worker.GetContextSwitchSampleCount(), m_worker.GetContextSwitchSampleCount() ? "process-related samples" : "not captured" );
        DomainRow( "FrameImage", m_worker.GetFrameImageCount() ? DomainState::Complete : DomainState::NotCaptured,
            m_worker.GetFrameImageCount(), m_worker.GetFrameImageCount() ? "captured images" : "not captured" );
        ImGui::EndTable();
    }

    ImGui::SeparatorText( "GPU analysis" );
    const auto status = m_gpuAnalysis ? m_gpuAnalysis->Status() : GpuAnalysisControllerStatus{};
    ImGui::Text( "State: %s", GpuAnalysisControllerStateName( status.state ) );
    ImGui::SameLine(); ImGui::TextDisabled( "stage=%s", status.stage.empty() ? "idle" : status.stage.c_str() );
    if( status.state == GpuAnalysisControllerState::Building )
    {
        ImGui::ProgressBar( status.progress, ImVec2( -1, 0 ) );
        if( ImGui::Button( "Cancel" ) ) m_gpuAnalysis->Cancel();
    }
    else if( status.state == GpuAnalysisControllerState::Failed || status.state == GpuAnalysisControllerState::Cancelled )
    {
        if( ImGui::Button( "Retry" ) ) m_gpuAnalysis->Retry();
    }
    if( !status.error.empty() ) ImGui::TextColored( DomainStateColor( DomainState::Invalid ), "%s", status.error.c_str() );
    ImGui::TextDisabled( "The complete GPU index starts only when GPU Memory & Resources is opened after capture data is frozen." );

    const auto& appInfo = m_worker.GetAppInfo();
    if( ImGui::CollapsingHeader( "Application identity / capture configuration" ) )
    {
        ImGui::BeginChild( "jnAppInfo", ImVec2( 0, 140 * GetScale() ), ImGuiChildFlags_Borders );
        for( const auto& item : appInfo ) ImGui::TextUnformatted( m_worker.GetString( item ) );
        if( appInfo.empty() ) ImGui::TextDisabled( "No AppInfo records." );
        ImGui::EndChild();
    }
    ImGui::End();
}

}

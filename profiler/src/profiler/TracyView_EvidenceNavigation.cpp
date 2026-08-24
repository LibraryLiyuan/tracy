#include "TracyView.hpp"

namespace tracy
{

View::EvidenceNavigationState View::CaptureEvidenceNavigation() const
{
    EvidenceNavigationState state;
    state.frameSet = m_frames; state.viewStart = m_vd.zvStart; state.viewEnd = m_vd.zvEnd; state.selectedThread = m_selectedThread;
    state.selectedJob = m_selectedJnJob; state.selectedResource = m_jnGpuUi.selectedResource; state.selectedAllocation = m_jnGpuUi.selectedAllocation;
    state.selectedPass = m_jnGpuUi.selectedPass; state.gpuTab = m_jnGpuUi.tab; state.timelineScrollY = m_jnTimelineScrollY;
    state.showJobs = m_showJnJobs; state.showGpu = m_showJnGpuResources; state.showCpuMemory = m_memInfo.show;
    return state;
}

void View::ApplyEvidenceNavigation( const EvidenceNavigationState& state )
{
    if( state.frameSet ) m_frames = state.frameSet;
    m_selectedJnJob = state.selectedJob; m_jnGpuUi.selectedResource = state.selectedResource; m_jnGpuUi.selectedAllocation = state.selectedAllocation;
    m_jnGpuUi.selectedPass = state.selectedPass; m_jnGpuUi.tab = state.gpuTab; m_jnPendingTimelineScrollY = state.timelineScrollY;
    m_showJnJobs = state.showJobs; m_showJnGpuResources = state.showGpu; m_memInfo.show = state.showCpuMemory;
    SelectThread( state.selectedThread ); ZoomToRange( state.viewStart, state.viewEnd );
}

void View::PushEvidenceNavigation()
{
    m_evidenceBack.emplace_back( CaptureEvidenceNavigation() );
    if( m_evidenceBack.size() > 256 ) m_evidenceBack.erase( m_evidenceBack.begin() );
    m_evidenceForward.clear();
}

void View::EvidenceNavigateBack()
{
    if( m_evidenceBack.empty() ) return;
    m_evidenceForward.emplace_back( CaptureEvidenceNavigation() );
    const auto state = m_evidenceBack.back(); m_evidenceBack.pop_back(); ApplyEvidenceNavigation( state );
}

void View::EvidenceNavigateForward()
{
    if( m_evidenceForward.empty() ) return;
    m_evidenceBack.emplace_back( CaptureEvidenceNavigation() );
    const auto state = m_evidenceForward.back(); m_evidenceForward.pop_back(); ApplyEvidenceNavigation( state );
}

}

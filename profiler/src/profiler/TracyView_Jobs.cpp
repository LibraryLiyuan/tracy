#include <algorithm>
#include <inttypes.h>
#include <unordered_map>

#include "TracyImGui.hpp"
#include "TracyMouse.hpp"
#include "TracyPrint.hpp"
#include "TracyView.hpp"

namespace tracy
{

namespace
{

const char* JobKindName( uint8_t kind )
{
    static constexpr const char* Names[] = { "Native", "Managed", "Burst", "Gfx" };
    return kind < std::size( Names ) ? Names[kind] : "Unknown";
}

const char* JobStageName( uint8_t stage )
{
    static constexpr const char* Names[] = {
        "PreExecute Begin", "PreExecute End", "Worker Slice Begin", "Worker Slice End",
        "PostExecute Begin", "PostExecute End", "Completed", "Wait Begin",
        "Wait ActiveHelp Begin", "Wait ActiveHelp End", "Wait Spin/Yield Begin", "Wait Spin/Yield End",
        "Wait Sleep Begin", "Wait Sleep End", "Wait End", "Flow Begin", "Flow Next",
        "Flow ParallelNext", "Flow End", "Cancelled", "Incomplete", "Schedule Callstack"
    };
    return stage < std::size( Names ) ? Names[stage] : "Unknown";
}

bool IsSpanBegin( JnJobStage stage )
{
    return stage == JnJobStage::PreExecuteBegin || stage == JnJobStage::WorkerSliceBegin ||
        stage == JnJobStage::PostExecuteBegin || stage == JnJobStage::WaitActiveHelpBegin ||
        stage == JnJobStage::WaitSpinYieldBegin || stage == JnJobStage::WaitSleepBegin;
}

bool IsSpanEnd( JnJobStage stage )
{
    return stage == JnJobStage::PreExecuteEnd || stage == JnJobStage::WorkerSliceEnd ||
        stage == JnJobStage::PostExecuteEnd || stage == JnJobStage::WaitActiveHelpEnd ||
        stage == JnJobStage::WaitSpinYieldEnd || stage == JnJobStage::WaitSleepEnd;
}

JnJobStage MatchingBegin( JnJobStage end )
{
    return JnJobStage( uint8_t( end ) - 1 );
}

uint32_t StageColor( JnJobStage stage )
{
    switch( stage )
    {
    case JnJobStage::WorkerSliceBegin: return 0xFFDD9955;
    case JnJobStage::PreExecuteBegin:
    case JnJobStage::PostExecuteBegin: return 0xFFCC66CC;
    case JnJobStage::WaitActiveHelpBegin: return 0xFF66CC66;
    case JnJobStage::WaitSpinYieldBegin: return 0xFF44AAEE;
    case JnJobStage::WaitSleepBegin: return 0xFF5577DD;
    default: return 0xFFDDDDDD;
    }
}

}

void View::RebuildJnJobView()
{
    const auto& data = m_worker.GetJnTraceData();
    if( m_jnJobTypeCount == data.jobTypes.size() &&
        m_jnJobScheduleCount == data.jobSchedules.size() &&
        m_jnJobConfigCount == data.jobConfigs.size() &&
        m_jnJobDependencyCount == data.jobDependencies.size() &&
        m_jnJobStageCount == data.jobStages.size() ) return;

    m_jnJobTypeCount = data.jobTypes.size();
    m_jnJobScheduleCount = data.jobSchedules.size();
    m_jnJobConfigCount = data.jobConfigs.size();
    m_jnJobDependencyCount = data.jobDependencies.size();
    m_jnJobStageCount = data.jobStages.size();
    m_jnJobs.clear();
    m_jnJobById.clear();
    m_jnJobTypeNames.clear();

    for( const auto& type : data.jobTypes ) m_jnJobTypeNames[type.typeId] = type.name;

    const auto ensureJob = [&]( uint64_t jobId ) -> JnJobViewData& {
        auto it = m_jnJobById.find( jobId );
        if( it != m_jnJobById.end() ) return m_jnJobs[it->second];
        const auto index = m_jnJobs.size();
        m_jnJobs.emplace_back();
        m_jnJobs.back().jobId = jobId;
        m_jnJobById[jobId] = index;
        return m_jnJobs.back();
    };

    for( const auto& schedule : data.jobSchedules )
    {
        auto& job = ensureJob( schedule.jobId );
        job.packedHandle = schedule.packedHandle;
        job.scheduleThread = schedule.thread;
        job.scheduleTime = schedule.time;
        job.expectedDependencyCount = schedule.dependencyCount;
        job.kind = schedule.kind;
        job.flags = schedule.flags;
        job.orphan = false;
    }
    for( const auto& config : data.jobConfigs )
    {
        auto& job = ensureJob( config.jobId );
        if( config.typeId != 0 ) job.typeId = config.typeId;
        if( config.count != 0 || job.count == 0 ) job.count = config.count;
        if( config.grainSize != 0 || job.grainSize == 0 ) job.grainSize = config.grainSize;
        if( config.unityFlowId != 0 || job.unityFlowId == 0 ) job.unityFlowId = config.unityFlowId;
        job.kind = config.kind;
        job.flags |= config.flags;
    }
    for( size_t i=0; i<data.jobDependencies.size(); i++ )
    {
        ensureJob( data.jobDependencies[i].jobId ).dependencies.emplace_back( i );
    }
    for( size_t i=0; i<data.jobStages.size(); i++ )
    {
        ensureJob( data.jobStages[i].jobId ).stages.emplace_back( i );
    }

    for( auto& job : m_jnJobs )
    {
        std::sort( job.stages.begin(), job.stages.end(), [&]( size_t lhs, size_t rhs ) {
            return data.jobStages[lhs].time < data.jobStages[rhs].time;
        } );
        std::unordered_map<uint32_t, int64_t> workerStarts;
        std::unordered_map<uint32_t, int64_t> activeHelpStarts;
        std::unordered_map<uint32_t, int64_t> spinStarts;
        std::unordered_map<uint32_t, int64_t> sleepStarts;
        const auto closeSpan = []( auto& starts, uint32_t id, int64_t time, int64_t& total ) {
            const auto it = starts.find( id );
            if( it == starts.end() ) return;
            if( time >= it->second ) total += time - it->second;
            starts.erase( it );
        };
        for( const auto stageIndex : job.stages )
        {
            const auto& stage = data.jobStages[stageIndex];
            switch( JnJobStage( stage.stage ) )
            {
            case JnJobStage::WorkerSliceBegin:
                if( job.firstRunTime < 0 || stage.time < job.firstRunTime ) job.firstRunTime = stage.time;
                workerStarts[stage.spanId] = stage.time;
                break;
            case JnJobStage::WorkerSliceEnd: closeSpan( workerStarts, stage.spanId, stage.time, job.executionTime ); break;
            case JnJobStage::Completed: job.completedTime = stage.time; break;
            case JnJobStage::WaitActiveHelpBegin: activeHelpStarts[stage.spanId] = stage.time; break;
            case JnJobStage::WaitActiveHelpEnd: closeSpan( activeHelpStarts, stage.spanId, stage.time, job.waitActiveHelpTime ); break;
            case JnJobStage::WaitSpinYieldBegin: spinStarts[stage.spanId] = stage.time; break;
            case JnJobStage::WaitSpinYieldEnd: closeSpan( spinStarts, stage.spanId, stage.time, job.waitSpinYieldTime ); break;
            case JnJobStage::WaitSleepBegin: sleepStarts[stage.spanId] = stage.time; break;
            case JnJobStage::WaitSleepEnd: closeSpan( sleepStarts, stage.spanId, stage.time, job.waitSleepTime ); break;
            case JnJobStage::ScheduleCallstack: job.scheduleCallstack = stage.spanId; break;
            case JnJobStage::Cancelled: job.cancelled = true; break;
            case JnJobStage::Incomplete: job.incomplete = true; break;
            default: break;
            }
        }
    }

    std::sort( m_jnJobs.begin(), m_jnJobs.end(), []( const auto& lhs, const auto& rhs ) {
        if( lhs.orphan != rhs.orphan ) return !lhs.orphan;
        if( lhs.scheduleTime != rhs.scheduleTime ) return lhs.scheduleTime < rhs.scheduleTime;
        return lhs.jobId < rhs.jobId;
    } );
    m_jnJobById.clear();
    for( size_t i=0; i<m_jnJobs.size(); i++ ) m_jnJobById[m_jnJobs[i].jobId] = i;

    if( m_selectedJnJob != 0 && m_jnJobById.find( m_selectedJnJob ) == m_jnJobById.end() ) m_selectedJnJob = 0;
}

const char* View::GetJnJobName( const JnJobViewData& job ) const
{
    const auto it = m_jnJobTypeNames.find( job.typeId );
    return it == m_jnJobTypeNames.end() ? "<unknown job type>" : m_worker.GetString( it->second );
}

void View::NavigateToJnJobTime( uint64_t jobId, int64_t time, uint64_t thread, int64_t rangeStart, int64_t rangeEnd )
{
    m_jnJobNavigation.push_back( JnJobNavigationState { m_selectedJnJob, m_selectedThread, m_vd.zvStart, m_vd.zvEnd, m_jnTimelineScrollY } );
    m_selectedJnJob = jobId;
    m_showJnJobs = true;
    if( thread != 0 ) SelectThread( thread );
    if( rangeEnd < rangeStart ) std::swap( rangeStart, rangeEnd );
    if( rangeEnd == rangeStart ) rangeStart = rangeEnd = time;
    const auto span = std::max<int64_t>( rangeEnd - rangeStart, 1000 );
    const auto margin = std::max<int64_t>( span / 2, 50000 );
    ZoomToRange( rangeStart - margin, rangeEnd + margin );
}

void View::RestoreJnJobNavigation()
{
    if( m_jnJobNavigation.empty() ) return;
    const auto state = m_jnJobNavigation.back();
    m_jnJobNavigation.pop_back();
    m_selectedJnJob = state.jobId;
    SelectThread( state.selectedThread );
    m_jnPendingTimelineScrollY = state.scrollY;
    ZoomToRange( state.viewStart, state.viewEnd );
}

void View::DrawJnJobWindow()
{
    RebuildJnJobView();
    ImGui::SetNextWindowSize( ImVec2( 980, 720 ), ImGuiCond_FirstUseEver );
    ImGui::Begin( "Unity Jobs", &m_showJnJobs );
    if( !m_worker.HasJnTraceData() )
    {
        ImGui::TextDisabled( "This trace does not contain the JN Job schema." );
        ImGui::End();
        return;
    }

    ImGui::Text( "Jobs: %s", RealToString( m_jnJobs.size() ) );
    ImGui::SameLine();
    if( ButtonDisablable( "Go Back", m_jnJobNavigation.empty() ) ) RestoreJnJobNavigation();
    ImGui::SameLine();
    m_jnJobFilter.Draw( "Filter", 260 );

    if( ImGui::BeginTable( "jn-job-list", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2( 0, 245 ) ) )
    {
        ImGui::TableSetupScrollFreeze( 0, 1 );
        ImGui::TableSetupColumn( "Job ID", ImGuiTableColumnFlags_WidthFixed, 82 );
        ImGui::TableSetupColumn( "Name", ImGuiTableColumnFlags_WidthStretch );
        ImGui::TableSetupColumn( "Kind", ImGuiTableColumnFlags_WidthFixed, 68 );
        ImGui::TableSetupColumn( "Schedule thread", ImGuiTableColumnFlags_WidthStretch );
        ImGui::TableSetupColumn( "Start latency", ImGuiTableColumnFlags_WidthFixed, 90 );
        ImGui::TableSetupColumn( "Execution", ImGuiTableColumnFlags_WidthFixed, 90 );
        ImGui::TableSetupColumn( "State", ImGuiTableColumnFlags_WidthFixed, 78 );
        ImGui::TableHeadersRow();
        for( const auto& job : m_jnJobs )
        {
            const auto name = GetJnJobName( job );
            if( !m_jnJobFilter.PassFilter( name ) ) continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex( 0 );
            char id[64];
            sprintf( id, "%" PRIu64 "##job-%" PRIu64, job.jobId, job.jobId );
            if( ImGui::Selectable( id, m_selectedJnJob == job.jobId, ImGuiSelectableFlags_SpanAllColumns ) ) m_selectedJnJob = job.jobId;
            ImGui::TableSetColumnIndex( 1 ); ImGui::TextUnformatted( name );
            ImGui::TableSetColumnIndex( 2 ); ImGui::TextUnformatted( JobKindName( job.kind ) );
            ImGui::TableSetColumnIndex( 3 ); ImGui::TextUnformatted( job.scheduleThread == 0 ? "<unknown>" : m_worker.GetThreadName( job.scheduleThread ) );
            ImGui::TableSetColumnIndex( 4 ); ImGui::TextUnformatted( job.firstRunTime < 0 || job.orphan ? "-" : TimeToString( job.firstRunTime - job.scheduleTime ) );
            ImGui::TableSetColumnIndex( 5 ); ImGui::TextUnformatted( TimeToString( job.executionTime ) );
            ImGui::TableSetColumnIndex( 6 );
            ImGui::TextUnformatted( job.cancelled ? "cancelled" : job.incomplete ? "incomplete" : job.completedTime >= 0 ? "completed" : "truncated" );
        }
        ImGui::EndTable();
    }

    const auto selectedIt = m_jnJobById.find( m_selectedJnJob );
    if( selectedIt == m_jnJobById.end() )
    {
        ImGui::TextDisabled( "Select a Job to inspect Schedule, Worker slices, dependencies, waits, and completion." );
        ImGui::End();
        return;
    }

    const auto& data = m_worker.GetJnTraceData();
    const auto& job = m_jnJobs[selectedIt->second];
    ImGui::Separator();
    ImGui::Text( "Job #%" PRIu64 "  %s", job.jobId, GetJnJobName( job ) );
    ImGui::SameLine();
    ImGui::TextDisabled( "[%s] handle=%" PRIu32 ":%" PRIu32, JobKindName( job.kind ), uint32_t( job.packedHandle >> 32 ), uint32_t( job.packedHandle ) );

    if( ImGui::Button( "Focus chain" ) )
    {
        auto end = job.completedTime >= 0 ? job.completedTime : job.firstRunTime >= 0 ? job.firstRunTime : job.scheduleTime;
        NavigateToJnJobTime( job.jobId, job.scheduleTime, job.scheduleThread, job.scheduleTime, end );
    }
    ImGui::SameLine();
    if( ImGui::Button( "Schedule" ) ) NavigateToJnJobTime( job.jobId, job.scheduleTime, job.scheduleThread, job.scheduleTime, job.scheduleTime );
    if( job.firstRunTime >= 0 )
    {
        ImGui::SameLine();
        if( ImGui::Button( "First run" ) )
        {
            uint64_t thread = 0;
            for( const auto index : job.stages ) if( data.jobStages[index].time == job.firstRunTime ) { thread = data.jobStages[index].thread; break; }
            NavigateToJnJobTime( job.jobId, job.firstRunTime, thread, job.firstRunTime, job.firstRunTime );
        }
    }
    if( job.completedTime >= 0 )
    {
        ImGui::SameLine();
        if( ImGui::Button( "Complete" ) )
        {
            uint64_t thread = 0;
            for( const auto index : job.stages ) if( data.jobStages[index].time == job.completedTime ) { thread = data.jobStages[index].thread; break; }
            NavigateToJnJobTime( job.jobId, job.completedTime, thread, job.completedTime, job.completedTime );
        }
    }
    if( job.scheduleCallstack != 0 )
    {
        ImGui::SameLine();
        if( ImGui::Button( "Schedule call stack" ) ) m_callstackInfoWindow = job.scheduleCallstack;
    }

    ImGui::Columns( 2, "jn-job-summary", false );
    TextFocused( "Schedule:", TimeToStringExact( job.scheduleTime ) );
    TextFocused( "Schedule thread:", job.scheduleThread == 0 ? "<unknown>" : m_worker.GetThreadName( job.scheduleThread ) );
    TextFocused( "Count:", RealToString( job.count ) );
    TextFocused( "Grain:", RealToString( job.grainSize ) );
    TextFocused( "Dependencies:", RealToString( job.dependencies.size() ) );
    TextFocused( "Unity flow:", RealToString( job.unityFlowId ) );
    ImGui::NextColumn();
    TextFocused( "Schedule to first run:", job.firstRunTime < 0 || job.orphan ? "-" : TimeToString( job.firstRunTime - job.scheduleTime ) );
    TextFocused( "Execution:", TimeToString( job.executionTime ) );
    TextFocused( "Schedule to complete:", job.completedTime < 0 || job.orphan ? "-" : TimeToString( job.completedTime - job.scheduleTime ) );
    TextFocused( "Wait ActiveHelp:", TimeToString( job.waitActiveHelpTime ) );
    TextFocused( "Wait Spin/Yield:", TimeToString( job.waitSpinYieldTime ) );
    TextFocused( "Wait Sleep:", TimeToString( job.waitSleepTime ) );
    ImGui::Columns( 1 );

    if( ImGui::BeginTabBar( "jn-job-details" ) )
    {
        if( ImGui::BeginTabItem( "Stages" ) )
        {
            if( ImGui::BeginTable( "jn-job-stages", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2( 0, 210 ) ) )
            {
                ImGui::TableSetupColumn( "Stage" ); ImGui::TableSetupColumn( "Time" ); ImGui::TableSetupColumn( "Thread" );
                ImGui::TableSetupColumn( "Span" ); ImGui::TableSetupColumn( "Range/arg" ); ImGui::TableSetupColumn( "Navigate" );
                ImGui::TableHeadersRow();
                for( const auto stageIndex : job.stages )
                {
                    const auto& stage = data.jobStages[stageIndex];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex( 0 ); ImGui::TextUnformatted( JobStageName( stage.stage ) );
                    ImGui::TableSetColumnIndex( 1 ); ImGui::TextUnformatted( TimeToStringExact( stage.time ) );
                    ImGui::TableSetColumnIndex( 2 ); ImGui::TextUnformatted( stage.thread == 0 ? "<unknown>" : m_worker.GetThreadName( stage.thread ) );
                    ImGui::TableSetColumnIndex( 3 ); ImGui::Text( "%" PRIu32, stage.spanId );
                    ImGui::TableSetColumnIndex( 4 ); ImGui::Text( "%" PRIu32 "..%" PRIu32, stage.arg0, stage.arg1 );
                    ImGui::TableSetColumnIndex( 5 );
                    ImGui::PushID( int( stageIndex ) );
                    if( JnJobStage( stage.stage ) == JnJobStage::ScheduleCallstack )
                    {
                        if( ImGui::SmallButton( "Call stack" ) ) m_callstackInfoWindow = stage.spanId;
                    }
                    else if( ImGui::SmallButton( "Focus" ) )
                    {
                        NavigateToJnJobTime( job.jobId, stage.time, stage.thread, stage.time, stage.time );
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if( ImGui::BeginTabItem( "Dependencies" ) )
        {
            ImGui::TextUnformatted( "Upstream" );
            for( const auto dependencyIndex : job.dependencies )
            {
                const auto& dependency = data.jobDependencies[dependencyIndex];
                const auto upstream = m_jnJobById.find( dependency.prerequisiteJobId );
                if( upstream == m_jnJobById.end() )
                {
                    ImGui::TextDisabled( "Missing Job #%" PRIu64 " (handle=%" PRIu64 ")", dependency.prerequisiteJobId, dependency.prerequisiteHandle );
                    continue;
                }
                const auto& target = m_jnJobs[upstream->second];
                char label[256];
                sprintf( label, "Job #%" PRIu64 "  %s", target.jobId, GetJnJobName( target ) );
                if( ImGui::Selectable( label, false ) ) m_selectedJnJob = target.jobId;
            }
            ImGui::Separator();
            ImGui::TextUnformatted( "Downstream" );
            for( const auto& candidate : m_jnJobs )
            {
                bool dependent = false;
                for( const auto dependencyIndex : candidate.dependencies )
                {
                    if( data.jobDependencies[dependencyIndex].prerequisiteJobId == job.jobId ) { dependent = true; break; }
                }
                if( !dependent ) continue;
                char label[256];
                sprintf( label, "Job #%" PRIu64 "  %s", candidate.jobId, GetJnJobName( candidate ) );
                if( ImGui::Selectable( label, false ) ) m_selectedJnJob = candidate.jobId;
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void View::DrawJnJobTimelineOverlay( const ImVec2& timelinePos, double pxns, bool hover )
{
    if( !m_worker.HasJnTraceData() || m_jnJobThreadBounds.empty() ) return;
    RebuildJnJobView();
    const auto& data = m_worker.GetJnTraceData();
    auto draw = ImGui::GetWindowDrawList();

    const auto pointAt = [&]( int64_t time, uint64_t thread, ImVec2& point ) {
        const auto bounds = m_jnJobThreadBounds.find( thread );
        if( bounds == m_jnJobThreadBounds.end() ) return false;
        point = ImVec2( timelinePos.x + float( ( time - m_vd.zvStart ) * pxns ), bounds->second.upperLeft.y + 5.f );
        return true;
    };
    const auto selectMarker = [&]( const JnJobViewData& job, const ImVec2& point, int64_t time, uint64_t thread, const char* stage ) {
        if( !hover || !ImGui::IsMouseHoveringRect( point - ImVec2( 5, 5 ), point + ImVec2( 5, 5 ) ) ) return false;
        ImGui::BeginTooltip();
        ImGui::Text( "Job #%" PRIu64 "  %s", job.jobId, GetJnJobName( job ) );
        TextFocused( "Event:", stage );
        TextFocused( "Time:", TimeToStringExact( time ) );
        TextFocused( "Thread:", thread == 0 ? "<unknown>" : m_worker.GetThreadName( thread ) );
        ImGui::TextDisabled( "Click: inspect, double-click: focus" );
        ImGui::EndTooltip();
        if( IsMouseClicked( 0 ) )
        {
            m_selectedJnJob = job.jobId;
            m_showJnJobs = true;
            if( ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left ) ) NavigateToJnJobTime( job.jobId, time, thread, time, time );
        }
        return true;
    };

    for( const auto& job : m_jnJobs )
    {
        if( job.orphan || job.scheduleTime < m_vd.zvStart || job.scheduleTime > m_vd.zvEnd ) continue;
        ImVec2 point;
        if( !pointAt( job.scheduleTime, job.scheduleThread, point ) ) continue;
        const auto selected = job.jobId == m_selectedJnJob;
        const auto radius = selected ? 5.f : 3.f;
        const auto color = selected ? 0xFF55EE88 : 0xCC55AA77;
        draw->AddQuadFilled( point + ImVec2( 0, -radius ), point + ImVec2( radius, 0 ), point + ImVec2( 0, radius ), point + ImVec2( -radius, 0 ), color );
        selectMarker( job, point, job.scheduleTime, job.scheduleThread, "Schedule" );
    }

    const auto selectedIt = m_jnJobById.find( m_selectedJnJob );
    if( selectedIt == m_jnJobById.end() ) return;
    const auto& job = m_jnJobs[selectedIt->second];

    std::unordered_map<uint64_t, size_t> openSpans;
    ImVec2 firstSlicePoint, lastSlicePoint, completedPoint, waitEndPoint;
    bool hasFirstSlice = false, hasLastSlice = false, hasCompleted = false, hasWaitEnd = false;
    const auto spanKey = []( JnJobStage stage, uint32_t id ) { return ( uint64_t( uint8_t( stage ) ) << 32 ) | id; };
    for( const auto stageIndex : job.stages )
    {
        const auto& stage = data.jobStages[stageIndex];
        const auto stageType = JnJobStage( stage.stage );
        ImVec2 point;
        if( stageType == JnJobStage::WorkerSliceBegin && pointAt( stage.time, stage.thread, point ) && !hasFirstSlice )
        {
            firstSlicePoint = point;
            hasFirstSlice = true;
        }
        if( stageType == JnJobStage::WorkerSliceEnd && pointAt( stage.time, stage.thread, point ) )
        {
            lastSlicePoint = point;
            hasLastSlice = true;
        }
        if( stageType == JnJobStage::Completed && pointAt( stage.time, stage.thread, point ) )
        {
            completedPoint = point;
            hasCompleted = true;
        }
        if( stageType == JnJobStage::WaitEnd && job.completedTime >= 0 && stage.time >= job.completedTime && pointAt( stage.time, stage.thread, point ) )
        {
            waitEndPoint = point;
            hasWaitEnd = true;
        }

        if( IsSpanBegin( stageType ) )
        {
            openSpans[spanKey( stageType, stage.spanId )] = stageIndex;
        }
        else if( IsSpanEnd( stageType ) )
        {
            const auto beginType = MatchingBegin( stageType );
            const auto beginIt = openSpans.find( spanKey( beginType, stage.spanId ) );
            if( beginIt == openSpans.end() ) continue;
            const auto& begin = data.jobStages[beginIt->second];
            const auto bounds = m_jnJobThreadBounds.find( begin.thread );
            if( bounds == m_jnJobThreadBounds.end() ) continue;
            const auto x0 = timelinePos.x + float( ( begin.time - m_vd.zvStart ) * pxns );
            const auto x1 = timelinePos.x + float( ( stage.time - m_vd.zvStart ) * pxns );
            const auto y0 = bounds->second.upperLeft.y + 2.f;
            const auto y1 = y0 + 7.f;
            const ImVec2 ul( std::min( x0, x1 ), y0 );
            const ImVec2 dr( std::max( x0 + 2.f, x1 ), y1 );
            const auto color = StageColor( beginType );
            draw->AddRectFilled( ul, dr, color );
            draw->AddRect( ul, dr, 0xFFFFFFFF );
            if( hover && ImGui::IsMouseHoveringRect( ul, dr ) )
            {
                ImGui::BeginTooltip();
                ImGui::Text( "Job #%" PRIu64 "  %s", job.jobId, GetJnJobName( job ) );
                TextFocused( "Stage:", JobStageName( begin.stage ) );
                TextFocused( "Duration:", TimeToString( stage.time - begin.time ) );
                TextFocused( "Thread:", m_worker.GetThreadName( begin.thread ) );
                if( beginType == JnJobStage::WorkerSliceBegin ) ImGui::Text( "Range: %" PRIu32 "..%" PRIu32, begin.arg0, begin.arg1 );
                ImGui::EndTooltip();
                if( ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left ) ) NavigateToJnJobTime( job.jobId, begin.time, begin.thread, begin.time, stage.time );
            }
            openSpans.erase( beginIt );
        }
        else if( stageType != JnJobStage::ScheduleCallstack && pointAt( stage.time, stage.thread, point ) )
        {
            draw->AddCircleFilled( point, 3.f, stageType == JnJobStage::Completed ? 0xFF55EE55 : 0xFFEEEEEE );
            selectMarker( job, point, stage.time, stage.thread, JobStageName( stage.stage ) );
        }
    }

    ImVec2 schedulePoint;
    const auto hasSchedule = pointAt( job.scheduleTime, job.scheduleThread, schedulePoint );
    const auto drawArrow = [&]( const ImVec2& from, const ImVec2& to, uint32_t color ) {
        const auto mid = ( from.x + to.x ) * 0.5f;
        draw->AddBezierCubic( from, ImVec2( mid, from.y ), ImVec2( mid, to.y ), to, color, 1.5f );
        const auto direction = to.x >= from.x ? 1.f : -1.f;
        draw->AddTriangleFilled( to, to + ImVec2( -direction * 6.f, -3.f ), to + ImVec2( -direction * 6.f, 3.f ), color );
    };
    if( hasSchedule && hasFirstSlice ) drawArrow( schedulePoint, firstSlicePoint, 0xCC55DD99 );
    if( hasLastSlice && hasCompleted ) drawArrow( lastSlicePoint, completedPoint, 0xCC99DD55 );
    if( hasCompleted && hasWaitEnd ) drawArrow( completedPoint, waitEndPoint, 0xCCDD9955 );
}

}

#include <charconv>
#include <limits>
#include "TracyView.hpp"
#include "TracyTimelineItem.hpp"

namespace tracy
{
namespace
{
bool ReadUnsigned( const nlohmann::json& object, const char* key, uint64_t& value )
{
    if( !object.contains( key ) || !object[key].is_string() ) return false;
    const auto& text = object[key].get_ref<const std::string&>();
    if( text.empty() ) return false;
    const auto result = std::from_chars( text.data(), text.data() + text.size(), value );
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() && value <= uint64_t( std::numeric_limits<int64_t>::max() );
}
}

nlohmann::json View::ReportNavigate( const nlohmann::json& request )
{
    auto fail = []( const char* reason ) { return nlohmann::json{ { "ok", false }, { "error", reason } }; };
    if( !m_worker.HasData() || !m_worker.IsBackgroundDone() ) return fail( "not_ready" );
    if( !request.is_object() || !request.contains("frame_set") || !request["frame_set"].is_string() || !request.contains("frame_index") || !request["frame_index"].is_number_integer() ) return fail( "invalid_request" );
    const auto index = request["frame_index"].get<int64_t>();
    if( index < 0 || index > std::numeric_limits<int>::max() ) return fail( "invalid_frame_index" );
    uint64_t begin, end;
    if( !ReadUnsigned( request, "begin_ns", begin ) || !ReadUnsigned( request, "end_ns", end ) || end <= begin ) return fail( "invalid_frame_range" );
    const auto name = request["frame_set"].get<std::string>();
    const FrameData* frames = nullptr;
    for( const auto* item : m_worker.GetFrames() )
    {
        if( name != GetFrameSetName( *item ) ) continue;
        if( frames ) return fail( "ambiguous_frame_set" );
        frames = item;
    }
    if( !frames ) return fail( "frame_set_not_found" );
    if( uint64_t(index) >= m_worker.GetFrameCount( *frames ) ) return fail( "frame_out_of_range" );
    const auto actualBegin = m_worker.GetFrameBegin( *frames, size_t(index) );
    const auto actualEnd = m_worker.GetFrameEnd( *frames, size_t(index) );
    if( actualBegin != int64_t(begin) || actualEnd != int64_t(end) ) return fail( "frame_range_mismatch" );

    std::vector<const ThreadData*> threads;
    if( request.contains("threads") )
    {
        if( !request["threads"].is_array() ) return fail( "invalid_threads" );
        for( const auto& thread : request["threads"] )
        {
            uint64_t tid;
            if( !ReadUnsigned( thread, "id", tid ) || !thread.contains("name") || !thread["name"].is_string() ) return fail( "invalid_thread_identity" );
            const auto* data = m_worker.GetThreadData( tid );
            if( !data || thread["name"].get<std::string>() != m_worker.GetThreadName( tid ) ) return fail( "thread_identity_mismatch" );
            threads.push_back( data );
        }
    }

    const ZoneEvent* event = nullptr;
    nlohmann::json eventResult = nullptr;
    if( request.contains("event") && !request["event"].is_null() )
    {
        const auto& target = request["event"];
        uint64_t tid, start, finish;
        if( !ReadUnsigned(target,"thread_id",tid) || !ReadUnsigned(target,"begin_ns",start) || !ReadUnsigned(target,"end_ns",finish) || finish <= start || !target.contains("name") || !target["name"].is_string() ) return fail("invalid_event_identity");
        if( finish <= begin || start >= end ) return fail("event_outside_frame");
        event = FindZoneAtTime( tid, int64_t(start + (finish-start)/2) );
        for( int depth=0; event && depth<1024; ++depth )
        {
            if( event->Start() == int64_t(start) && event->IsEndValid() && event->End() == int64_t(finish) && target["name"].get<std::string>() == m_worker.GetZoneName(*event) ) break;
            event = GetZoneParent(*event,tid);
        }
        if( !event || event->Start()!=int64_t(start) || !event->IsEndValid() || event->End()!=int64_t(finish) || target["name"].get<std::string>()!=m_worker.GetZoneName(*event) ) return fail("event_not_found");
        const auto* thread=m_worker.GetThreadData(tid);
        if( !thread ) return fail("thread_not_found");
        if( threads.empty() ) threads.push_back(thread);
        eventResult={{"name",m_worker.GetZoneName(*event)},{"thread_id",std::to_string(tid)},{"begin_ns",std::to_string(start)},{"end_ns",std::to_string(finish)}};
    }

    PushEvidenceNavigation();
    m_frames=frames;
    ZoomToRange(actualBegin,actualEnd);
    m_zoomAnim.active=false;
    m_vd.zvStart=actualBegin;m_vd.zvEnd=actualEnd;
    m_vd.drawZones=true;
    m_reportEvent=event;m_zoneInfoWindow=nullptr;
    m_reportThreads=threads;m_reportFocusThread=threads.empty()?nullptr:threads.front();
    if( m_reportFocusThread ) SelectThread(m_reportFocusThread->id);
    m_showJnCaptureOverview=false;
    m_reportLayoutFrames=8;
    nlohmann::json threadResult=nlohmann::json::array();
    for( const auto* thread:threads ) threadResult.push_back({{"id",std::to_string(thread->id)},{"name",m_worker.GetThreadName(thread->id)}});
    m_reportNavigation={{"ok",true},{"revision",++m_reportRevision},{"frame_set",name},{"frame_index",index},{"display_frame_number",GetFrameNumber(*frames,int(index))},{"begin_ns",std::to_string(actualBegin)},{"end_ns",std::to_string(actualEnd)},{"threads",threadResult},{"event",eventResult}};
    return m_reportNavigation;
}

void View::ReportViewTick()
{
    ++m_reportDrawCount;
    if( m_reportLayoutFrames<=0 ) return;
    --m_reportLayoutFrames;
    for( const auto* thread:m_reportThreads )
    {
        const auto& map=m_tc.GetItemMap();auto it=map.find(thread);
        if(it!=map.end()){it->second->SetVisible(true);it->second->SetShowFull(true);}
    }
    if( m_reportFocusThread ) m_tc.ScrollToItem(m_reportFocusThread);
}

nlohmann::json View::ReportState() const
{
    nlohmann::json sets=nlohmann::json::array();
    if( m_worker.HasData() ) for(const auto* frame:m_worker.GetFrames()) sets.push_back({{"name",GetFrameSetName(*frame)},{"count",m_worker.GetFrameCount(*frame)}});
    return {{"api_version","0.1.0"},{"background_done",m_worker.IsBackgroundDone()},{"has_data",m_worker.HasData()},{"draws",m_reportDrawCount},{"layout_ready",m_reportRevision>0 && m_reportLayoutFrames==0 && (!m_reportFocusThread || m_tc.IsItemHeaderVisible(m_reportFocusThread))},{"begin_ns",std::to_string(m_vd.zvStart)},{"end_ns",std::to_string(m_vd.zvEnd)},{"scroll_y",m_jnTimelineScrollY},{"zone_selected",m_zoneInfoWindow!=nullptr},{"navigation",m_reportNavigation},{"frame_sets",sets}};
}

bool View::ReportSelectEvent()
{
    if(!m_reportEvent)return false;
    m_zoneInfoWindow=m_reportEvent;m_reportLayoutFrames=2;
    return true;
}
}

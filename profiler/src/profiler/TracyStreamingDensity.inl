bool StreamingView::DrawDenseTimeline( const Result& r, bool gpu )
{
    const auto& pointer = gpu ? r.gpuDensity : r.cpuDensity;
    if( !pointer ) return false;
    const auto& view = *pointer;
    bool request = false;
    const char* name = gpu ? "GPU density" : "CPU density";
    ImGui::Text( "Aggregated %s: %llu events in %llu lanes", gpu ? "GPU" : "CPU", (unsigned long long)view.events, (unsigned long long)view.lanes.size() );
    ImGui::TextDisabled( "Each cell counts overlapping events. The last depth row includes deeper levels. Click a cell to zoom." );
    if( m_smokeActive && ( ( !gpu && m_smokeStep < 6 ) || ( gpu && m_smokeStep >= 6 && m_smokeStep < 10 ) ) ) ImGui::SetScrollHereY( 0.f );
    if( ImGui::BeginChild( name, ImVec2( 0, gpu ? 150.f : 200.f ), true ) )
    {
        const auto origin = ImGui::GetCursorScreenPos();
        auto* draw = ImGui::GetWindowDrawList();
        const float width = std::max( 100.f, ImGui::GetContentRegionAvail().x );
        const float row = 19, laneHeight = row * float( view.depths + 1 );
        const double span = std::max( 1., double( r.end ) - double( r.begin ) );
        for( size_t n = 0; n < view.lanes.size(); ++n )
        {
            const auto& lane = view.lanes[n];
            char label[160];
            snprintf( label, sizeof( label ), "%s %llu | %llu events | max depth %u | %llu timestamps pending", gpu ? "Context" : "Thread", (unsigned long long)lane.id, (unsigned long long)lane.events, lane.maximumDepth, (unsigned long long)lane.unplaced );
            draw->AddText( ImVec2( origin.x, origin.y + float( n ) * laneHeight ), IM_COL32_WHITE, label );
        }
        const auto mouse = ImGui::GetIO().MousePos;
        for( const auto& cell : view.cells )
        {
            const auto found = std::lower_bound( view.lanes.begin(), view.lanes.end(), cell.lane, []( const auto& a, uint64_t b ) {
                return a.id < b;
            } );
            if( found == view.lanes.end() ) continue;
            const float y = origin.y + float( found - view.lanes.begin() ) * laneHeight + row * float( cell.depth + 1 );
            const float x0 = origin.x + float( ( double( cell.begin ) - double( r.begin ) ) / span ) * width, x1 = origin.x + float( ( double( cell.end ) - double( r.begin ) ) / span ) * width;
            const int heat = std::min( 180, int( std::log2( double( cell.events ) + 1 ) * 25 ) );
            const auto color = cell.invalid ? IM_COL32( 220, 70, 70, 255 ) : cell.unfinished ? IM_COL32( 195, 140, 45, 255 )
                                                                         : gpu               ? IM_COL32( 70, 70 + heat / 2, 90 + heat, 255 )
                                                                                             : IM_COL32( 45, 70 + heat, 70 + heat / 2, 255 );
            const ImVec2 a( x0, y ), b( std::max( x0 + 1, x1 ), y + row - 2 );
            draw->AddRectFilled( a, b, color );
            if( ImGui::IsRectVisible( a, b ) )
            {
                if( gpu ) m_smokeGpuDensityVerified = true;
                else m_smokeCpuDensityVerified = true;
            }
            if( ImGui::IsWindowHovered() && mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y && mouse.y < b.y )
            {
                ImGui::SetTooltip( "%llu overlaps | %llu closed | %llu unfinished | %llu invalid\nDepth %u%s", (unsigned long long)cell.events, (unsigned long long)cell.closed, (unsigned long long)cell.unfinished, (unsigned long long)cell.invalid, cell.depth, cell.depth + 1 == view.depths ? " and deeper" : "" );
                if( ImGui::IsMouseClicked( 0 ) && ( m_begin != cell.begin || m_end != cell.end ) )
                {
                    m_begin = cell.begin;
                    m_end = cell.end;
                    m_cpuLaneAfter = m_gpuLaneAfter = 0;
                    m_followLive = false;
                    request = true;
                }
            }
        }
        ImGui::Dummy( ImVec2( width, std::max( row, laneHeight * float( view.lanes.size() ) ) ) );
    }
    ImGui::EndChild();
    auto& cursor = gpu ? m_gpuLaneAfter : m_cpuLaneAfter;
    if( view.more )
    {
        ImGui::TextUnformatted( "More lanes are available." );
        ImGui::SameLine();
        if( ImGui::Button( gpu ? "Next GPU lanes" : "Next CPU lanes" ) )
        {
            cursor = view.nextLane;
            request = true;
        }
    }
    if( cursor && ImGui::Button( gpu ? "First GPU lanes" : "First CPU lanes" ) )
    {
        cursor = 0;
        request = true;
    }
    return request;
}

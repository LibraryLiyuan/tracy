void StreamingView::SelectAnalysisMatch( const stream::StoreAnalysisMatch& match, bool gpu )
{
    SelectEvent( match.id, gpu );
    if( gpu ) m_gpuContext = match.context;
    if( match.start )
    {
        m_begin = *match.start;
        m_end = match.end.value_or( m_begin < INT64_MAX - 1000000 ? m_begin + 1000000 : INT64_MAX );
        if( m_end <= m_begin && m_begin < INT64_MAX ) m_end = m_begin + 1;
    }
}
// The whole-capture analysis behind the Statistics, Find and Flame windows:
// its scope, run and progress.
bool StreamingView::DrawAnalysisRun( const Result& r )
{
    bool request = false;
    ImGui::Checkbox( "GPU zones", &m_analysisGpu );
    ImGui::SameLine();
    ImGui::Checkbox( "Use selected time range", &m_analysisRange );
    if( ImGui::Button( "Run analysis" ) )
    {
        m_analysisOptions = {};
        m_analysisOptions.gpu = m_analysisGpu;
        if( m_analysisRange )
        {
            m_analysisOptions.begin = m_begin;
            m_analysisOptions.end = m_end;
        }
        m_analysisTask = ++m_analysisNext;
        m_analysisStatsAfter = m_analysisFlameParent = m_analysisFlameAfter = m_analysisSearchAfter = 0;
        m_analysisSource.reset();
        request = true;
    }
    if( m_analysisBusy )
    {
        ImGui::SameLine();
        if( ImGui::Button( "Cancel analysis" ) )
        {
            m_analysisTask = 0;
            request = true;
        }
        const auto total = m_analysisTotal.load();
        if( total ) ImGui::ProgressBar( float( double( m_analysisProgress.load() ) / double( total ) ) );
        else ImGui::TextUnformatted( "Preparing analysis..." );
    }
    if( !r.analysisError.empty() ) ImGui::TextWrapped( "Analysis: %s", r.analysisError.c_str() );
    if( !r.analysis )
    {
        ImGui::TextDisabled( "Search, statistics and flame paths run across the selected capture range." );
        return request;
    }
    const auto& info = *r.analysis;
    if( info.begin == INT64_MIN && info.end == INT64_MAX ) ImGui::TextUnformatted( "Scope: full capture" );
    else ImGui::Text( "Scope: %.6f to %.6f seconds", double( info.begin ) / 1e9, double( info.end ) / 1e9 );
    ImGui::Text( "%s | revision %llu | %llu events | %llu closed | %llu unfinished | %llu invalid", info.gpu ? "GPU" : "CPU", (unsigned long long)info.revision, (unsigned long long)info.events, (unsigned long long)info.closed, (unsigned long long)info.unfinished, (unsigned long long)info.invalid );
    ImGui::TextDisabled( "Durations use completed events. Self time excludes the union of direct children; %llu events have unknown self time.", (unsigned long long)info.selfUnknown );
    if( info.gpu ) ImGui::TextDisabled( "Events with unknown GPU timestamps remain in the coverage counts." );
    if( m_analysisTask && r.analysisTask != m_analysisTask ) ImGui::TextUnformatted( "Showing the previous successful analysis." );
    return request;
}
// IDs belong to one analysis result. Rows are disabled while a replacement
// result is pending; a failed/cancelled replacement remains usable.
bool StreamingView::AnalysisPending( const Result& r ) const
{
    return m_analysisBusy || ( Busy() && m_analysisTask && r.analysisTask != m_analysisTask );
}
bool StreamingView::DrawStatistics( const Result& r, bool run )
{
    bool request = run && DrawAnalysisRun( r );
    if( !r.analysis ) return request;
    ImGui::BeginDisabled( AnalysisPending( r ) );
    if( ImGui::BeginTable( "Global statistics", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2( 0, std::max( 180.f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() ) ) ) )
    {
        for( const char* label : { "Source", "Events / unfinished", "Inclusive (ms)", "Known self (ms)", "Min / max (ms)" } ) ImGui::TableSetupColumn( label );
        ImGui::TableHeadersRow();
        for( const auto& row : r.statistics.values )
        {
            ImGui::PushID( int( row.id ) );
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if( ImGui::Selectable( ( row.name.empty() ? "(unnamed)" : row.name.substr( 0, 180 ) ).c_str(), m_analysisSource == row.source ) )
            {
                m_analysisSource = row.source;
                m_analysisSearchAfter = 0;
                m_analysisText[0] = 0;
                request = true;
            }
            if( ImGui::IsItemHovered() ) ImGui::SetTooltip( "%s\n%s:%u\n%llu events with unknown self time", row.function.c_str(), row.file.c_str(), row.line, (unsigned long long)row.selfUnknown );
            ImGui::TableNextColumn();
            ImGui::Text( "%llu / %llu", (unsigned long long)row.events, (unsigned long long)row.unfinished );
            ImGui::TableNextColumn();
            ImGui::Text( "%.6f", double( row.inclusive ) / 1e6 );
            ImGui::TableNextColumn();
            ImGui::Text( "%.6f", double( row.self ) / 1e6 );
            ImGui::TableNextColumn();
            ImGui::Text( "%.6f / %.6f", double( row.minimum ) / 1e6, double( row.maximum ) / 1e6 );
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if( r.statistics.more && ImGui::Button( "Next statistics page" ) )
    {
        m_analysisStatsAfter = r.statistics.next;
        request = true;
    }
    ImGui::SameLine();
    if( m_analysisStatsAfter && ImGui::Button( "First statistics page" ) )
    {
        m_analysisStatsAfter = 0;
        request = true;
    }
    ImGui::EndDisabled();
    return request;
}
bool StreamingView::DrawFlame( const Result& r, bool run )
{
    bool request = run && DrawAnalysisRun( r );
    if( !r.analysis ) return request;
    ImGui::BeginDisabled( AnalysisPending( r ) );
    ImGui::TextUnformatted( "Flame graph — click a frame to zoom into its path." );
    if( ImGui::SliderInt( "Visible levels", &m_flameDepth, 2, 32 ) ) request = true;
    ImGui::SameLine();
    if( ImGui::Checkbox( "Event-count widths", &m_flameCounts ) ) request = true;
    const uint32_t pixels = uint32_t( std::clamp( std::ceil( ImGui::GetContentRegionAvail().x / 128.f ) * 128.f, 128.f, 16384.f ) );
    if( pixels != m_flamePixels && !m_analysisBusy && !( Busy() && m_analysisTask && r.analysisTask != m_analysisTask ) )
    {
        m_flamePixels = pixels;
        request = true;
    }
    if( m_analysisFlameParent )
    {
        if( ImGui::Button( "Parent path" ) )
        {
            m_analysisFlameParent = r.analysisParentParent;
            m_analysisFlameAfter = 0;
            request = true;
        }
        ImGui::SameLine();
        if( ImGui::Button( "Root paths" ) )
        {
            m_analysisFlameParent = 0;
            m_analysisFlameAfter = 0;
            request = true;
        }
    }
    ImGui::Text( "Showing %llu of %llu path nodes", (unsigned long long)r.flameGraph.boxes.size(), (unsigned long long)r.flameGraph.totalNodes );
    if( r.flameGraph.omittedNodes ) ImGui::TextDisabled( "Zoom or increase levels to reveal more paths. All paths remain available in the browser below." );
    if( r.flameGraph.eventCounts ) ImGui::TextDisabled( "Width counts events in each path and its descendants." );
    else ImGui::TextDisabled( "Width stacks known self and descendant costs. Tooltips show measured inclusive time." );
    if( m_smokeStep >= 47 && m_smokeStep <= 50 ) ImGui::SetScrollHereY( 0.f );
    m_flameDrawn = 0;
    const float rowHeight = ImGui::GetTextLineHeight() + 6;
    const float graphHeight = rowHeight * float( std::max<uint32_t>( 1, r.flameGraph.depth ) );
    if( ImGui::BeginChild( "Flame graph canvas", ImVec2( 0, std::clamp( graphHeight + 8, 100.f, 350.f ) ), true ) )
    {
        const auto start = ImGui::GetCursorScreenPos();
        const float width = std::max( 100.f, ImGui::GetContentRegionAvail().x );
        ImGui::InvisibleButton( "flame canvas", ImVec2( width, graphHeight ) );
        const bool hovered = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked();
        const auto mouse = ImGui::GetIO().MousePos;
        auto* draw = ImGui::GetWindowDrawList();
        for( const auto& box : r.flameGraph.boxes )
        {
            const auto& row = box.row;
            const uint32_t hash = uint32_t( row.source ) * 2654435761u;
            const ImVec2 a( start.x + float( box.begin ) * width, start.y + float( r.flameGraph.depth - 1 - box.depth ) * rowHeight ), b( start.x + float( box.end ) * width, a.y + rowHeight - 2 );
            const auto color = row.closed ? IM_COL32( 170 + ( hash & 63 ), 70 + ( ( hash >> 8 ) & 95 ), 50 + ( ( hash >> 16 ) & 63 ), 255 ) : IM_COL32( 110, 110, 110, 255 );
            draw->AddRectFilled( a, b, color );
            draw->AddRect( a, b, IM_COL32( 35, 35, 35, 255 ) );
            if( ImGui::IsRectVisible( a, b ) ) ++m_flameDrawn;
            if( b.x - a.x > 24 )
            {
                const auto label = row.name.empty() ? std::string( "(unnamed)" ) : row.name.substr( 0, 160 );
                draw->PushClipRect( a, b, true );
                draw->AddText( ImVec2( a.x + 3, a.y + 2 ), IM_COL32_WHITE, label.c_str() );
                draw->PopClipRect();
            }
            if( hovered && mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y && mouse.y < b.y )
            {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted( row.name.c_str() );
                ImGui::Text( "%.6f ms measured inclusive | %.6f ms known self", double( row.inclusive ) / 1e6, double( row.self ) / 1e6 );
                ImGui::Text( "%llu events | %llu unfinished | thread %llu | context %llu", (unsigned long long)row.events, (unsigned long long)row.unfinished, (unsigned long long)row.thread, (unsigned long long)row.context );
                if( r.flameGraph.eventCounts ) ImGui::Text( "Width: %.0f events including descendants", box.weight );
                else if( box.expandedWeight ) ImGui::Text( "Width: %.6f ms aggregate cost including descendants", box.weight / 1e6 );
                ImGui::EndTooltip();
                if( clicked && row.id != m_analysisFlameParent )
                {
                    m_analysisFlameParent = row.id;
                    m_analysisFlameAfter = 0;
                    request = true;
                }
            }
        }
        if( r.flameGraph.boxes.empty() ) ImGui::TextUnformatted( "No paths visible at this resolution." );
    }
    ImGui::EndChild();
    if( ImGui::CollapsingHeader( "Browse flame paths" ) )
    {
        if( ImGui::BeginChild( "Flame path list", ImVec2( 0, 120 ), true ) )
            for( const auto& row : r.flame.values )
            {
                ImGui::PushID( int( row.id ) );
                if( ImGui::Selectable( ( row.name.empty() ? "(unnamed)" : row.name.substr( 0, 180 ) ).c_str() ) )
                {
                    m_analysisFlameParent = row.id;
                    m_analysisFlameAfter = 0;
                    request = true;
                }
                ImGui::PopID();
            }
        ImGui::EndChild();
        if( r.flame.more && ImGui::Button( "Next flame paths" ) )
        {
            m_analysisFlameAfter = r.flame.next;
            request = true;
        }
        if( m_analysisFlameAfter && ImGui::Button( "First flame paths" ) )
        {
            m_analysisFlameAfter = 0;
            request = true;
        }
    }
    ImGui::EndDisabled();
    return request;
}
bool StreamingView::DrawFind( const Result& r, bool run )
{
    bool request = run && DrawAnalysisRun( r );
    if( !r.analysis ) return request;
    const auto& info = *r.analysis;
    ImGui::BeginDisabled( AnalysisPending( r ) );
    if( ImGui::InputText( "Find event", m_analysisText.data(), m_analysisText.size(), ImGuiInputTextFlags_EnterReturnsTrue ) )
    {
        m_analysisSearchAfter = 0;
        request = true;
    }
    ImGui::SameLine();
    if( ImGui::Button( "Search" ) )
    {
        m_analysisSearchAfter = 0;
        request = true;
    }
    if( ImGui::Checkbox( "Case sensitive", &m_analysisCase ) )
    {
        m_analysisSearchAfter = 0;
        request = true;
    }
    if( m_analysisSource )
    {
        ImGui::SameLine();
        if( ImGui::Button( "Clear source filter" ) )
        {
            m_analysisSource.reset();
            m_analysisSearchAfter = 0;
            request = true;
        }
    }
    if( ImGui::BeginChild( "Search results", ImVec2( 0, std::max( 150.f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() ) ), true ) )
    {
        for( const auto& hit : r.search.values )
        {
            const auto label = std::to_string( hit.id ) + " | " + ( hit.name.empty() ? "(unnamed)" : hit.name.substr( 0, 180 ) );
            if( ImGui::Selectable( label.c_str(), m_selected == hit.id && m_gpuSelected == info.gpu ) )
            {
                SelectAnalysisMatch( hit, info.gpu );
                request = true;
            }
            if( ImGui::IsItemHovered() ) ImGui::SetTooltip( "%s\n%s%s", hit.function.c_str(), hit.file.c_str(), hit.textMatched ? "\nMatched Zone text" : "" );
        }
    }
    ImGui::EndChild();
    if( r.search.more && ImGui::Button( "Next matches" ) )
    {
        m_analysisSearchAfter = r.search.next;
        request = true;
    }
    ImGui::SameLine();
    if( m_analysisSearchAfter && ImGui::Button( "First matches" ) )
    {
        m_analysisSearchAfter = 0;
        request = true;
    }
    ImGui::EndDisabled();
    return request;
}
// Every part in one section, for the live view.
bool StreamingView::DrawAnalysis( const Result& r )
{
    if( !ImGui::CollapsingHeader( "Global analysis", ImGuiTreeNodeFlags_DefaultOpen ) ) return false;
    bool request = DrawStatistics( r );
    if( !r.analysis ) return request;
    ImGui::Separator();
    ImGui::PushID( "flame" );
    request |= DrawFlame( r, false );
    ImGui::PopID();
    ImGui::Separator();
    ImGui::PushID( "find" );
    request |= DrawFind( r, false );
    ImGui::PopID();
    return request;
}

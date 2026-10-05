// CPU lanes come from the zone event blocks. A block without open zones whose
// whole interval lies inside the window and inside one bin is counted from its
// per-depth summary; other blocks are decoded and binned zone by zone.
StoreTimelineView EventStore::CpuTimelineOverview( const StoreTimelineOptions& options, StoreTimelineView& view, size_t laneLimit ) const
{
    auto& d = m_impl->db;
    const I span = Sub( options.end, options.begin );
    std::vector<I> edges( view.bins + 1 );
    const I step = span / view.bins, remainder = span % view.bins;
    for( uint32_t n = 0; n <= view.bins; ++n ) edges[n] = options.begin + step * n + ( remainder * n ) / view.bins;
    struct Diff
    {
        I events = 0, closed = 0, unfinished = 0, invalid = 0;
    };
    const size_t laneCells = size_t( view.depths ) * ( view.bins + 1 );
    std::vector<Diff> delta;
    const auto binOf = [&]( I t ) { return size_t( std::upper_bound( edges.begin(), edges.end(), t ) - edges.begin() - 1 ); };
    auto q = d.Prep( std::string( "SELECT b.id,b.key,b.tmin,b.tmax,b.unfinished,b.summary FROM blocks b WHERE b.stream=1 AND b.id IN(SELECT id FROM block_range WHERE mint<=?1 AND maxt>=?2) AND b.tmin<?3 AND b.tmax>=?4 AND b.key>=?5" ) +
        ( options.context ? " AND b.key=?6" : "" ) + " ORDER BY b.key,b.first" );
    Int( q, 1, uint64_t( Bucket( options.end - 1 ) ) );
    Int( q, 2, uint64_t( Bucket( options.begin ) ) );
    Int( q, 3, options.end );
    Int( q, 4, options.begin );
    Int( q, 5, options.afterLane );
    if( options.context ) Int( q, 6, *options.context );
    struct Candidate
    {
        uint64_t block, lane;
        I tmin, tmax;
        uint64_t unfinished;
        std::vector<uint8_t> summary;
    };
    std::vector<Candidate> candidates;
    while( d.Step( q ) )
    {
        Candidate c{ uint64_t( sqlite3_column_int64( q, 0 ) ), uint64_t( sqlite3_column_int64( q, 1 ) ), sqlite3_column_int64( q, 2 ), sqlite3_column_int64( q, 3 ), uint64_t( sqlite3_column_int64( q, 4 ) ) };
        const auto blob = static_cast<const uint8_t*>( sqlite3_column_blob( q, 5 ) );
        c.summary.assign( blob, blob + sqlite3_column_bytes( q, 5 ) );
        candidates.push_back( std::move( c ) );
    }
    sqlite3_reset( q );
    StoreTimelineLane* lane = nullptr;
    bool probing = false;
    for( const auto& c : candidates )
    {
        if( !lane || lane->id != c.lane )
        {
            if( lane && !lane->events )
            {
                view.lanes.pop_back();
                delta.resize( delta.size() - laneCells );
            }
            else if( lane && probing )
            {
                view.events -= lane->events;
                view.lanes.pop_back();
                delta.resize( delta.size() - laneCells );
                view.more = true;
                lane = nullptr;
                break;
            }
            if( view.lanes.size() == laneLimit ) probing = true;
            Require( c.lane < UINT64_MAX, "Timeline lane cursor overflow" );
            view.lanes.push_back( { c.lane } );
            delta.resize( delta.size() + laneCells );
            lane = &view.lanes.back();
        }
        if( probing && lane->events ) continue;
        const size_t base = ( view.lanes.size() - 1 ) * laneCells;
        const auto add = [&]( uint32_t depth, size_t beginBin, size_t endBin, I events, I closed, I unfinished ) {
            const size_t cell = base + size_t( std::min( depth, view.depths - 1 ) ) * ( view.bins + 1 );
            auto& a = delta[cell + beginBin];
            auto& b = delta[cell + endBin + 1];
            a.events += events;
            a.closed += closed;
            a.unfinished += unfinished;
            b.events -= events;
            b.closed -= closed;
            b.unfinished -= unfinished;
        };
        if( !c.unfinished && c.tmin >= options.begin && c.tmax < options.end && binOf( c.tmin ) == binOf( c.tmax ) && !c.summary.empty() )
        {
            const auto summary = DecodeSummary( c.summary );
            const auto bin = binOf( c.tmin );
            for( uint32_t depth = 0; depth < SummaryDepths; ++depth )
                if( summary.depth[depth] )
                {
                    add( depth, bin, bin, summary.depth[depth], summary.depth[depth], 0 );
                    lane->events += summary.depth[depth];
                    view.events += summary.depth[depth];
                }
            lane->maximumDepth = std::max( lane->maximumDepth, summary.maximumDepth );
            continue;
        }
        for( const auto& r : LoadZoneBlock( d, m_impl->blocks, m_impl->zoneCache, c.block )->rows )
        {
            if( !ZoneOverlaps( r, options.begin, options.end ) ) continue;
            lane->maximumDepth = std::max( lane->maximumDepth, r.depth );
            ++lane->events;
            ++view.events;
            if( r.open ) ++lane->unfinished;
            const I first = r.start, last = r.open ? options.end : r.end;
            const I lo = std::max( options.begin, std::min( first, last ) ), hi = std::min( options.end, std::max( first, last ) );
            if( lo >= options.end || hi < options.begin ) continue;
            const auto beginBin = binOf( lo );
            const auto endBin = first == last ? beginBin : size_t( std::lower_bound( edges.begin(), edges.end(), hi ) - edges.begin() - 1 );
            Require( beginBin < view.bins && endBin < view.bins && beginBin <= endBin, "Invalid timeline bin interval" );
            add( r.depth, beginBin, endBin, 1, r.open ? 0 : 1, r.open ? 1 : 0 );
        }
    }
    if( lane && ( !lane->events || probing ) )
    {
        if( probing && lane->events ) view.more = true;
        view.events -= lane->events;
        view.lanes.pop_back();
        delta.resize( delta.size() - laneCells );
    }
    if( !view.lanes.empty() ) view.nextLane = view.lanes.back().id + 1;
    for( size_t laneIndex = 0; laneIndex < view.lanes.size(); ++laneIndex )
        for( uint32_t depth = 0; depth < view.depths; ++depth )
        {
            Diff total;
            const size_t base = laneIndex * laneCells + size_t( depth ) * ( view.bins + 1 );
            for( uint32_t bin = 0; bin < view.bins; ++bin )
            {
                const auto& change = delta[base + bin];
                total.events += change.events;
                total.closed += change.closed;
                total.unfinished += change.unfinished;
                total.invalid += change.invalid;
                Require( total.events >= 0 && total.closed >= 0 && total.unfinished >= 0 && total.invalid >= 0, "Timeline difference counts underflow" );
                if( total.events ) view.cells.push_back( { view.lanes[laneIndex].id, uint64_t( total.events ), uint64_t( total.closed ), uint64_t( total.unfinished ), uint64_t( total.invalid ), edges[bin], edges[bin + 1], depth } );
            }
        }
    return view;
}
StoreTimelineView EventStore::TimelineOverview( const StoreTimelineOptions& options ) const
{
    Require( options.begin < options.end && options.bins > 0 && options.bins <= 2048 && options.depths > 0 && options.depths <= 16 && options.maximumLanes > 0 && options.maximumLanes <= 32, "Invalid timeline overview budget" );
    const I span = Sub( options.end, options.begin );
    auto& d = m_impl->db;
    StoreTimelineView view;
    view.bins = uint32_t( std::min<uint64_t>( options.bins, uint64_t( span ) ) );
    view.depths = options.depths;
    const uint64_t perLane = sizeof( StoreTimelineLane ) + uint64_t( view.bins ) * view.depths * sizeof( StoreTimelineBin );
    const size_t laneLimit = std::min<uint64_t>( options.maximumLanes, ( d.options.resultBytes - sizeof( view ) ) / perLane );
    Require( laneLimit > 0, "Timeline overview result budget is too small" );
    view.lanes.reserve( laneLimit );
    view.cells.reserve( laneLimit * view.bins * view.depths );
    if( !options.gpu ) return CpuTimelineOverview( options, view, laneLimit );
    const std::string lane = options.gpu ? "z.context" : "z.thread", start = options.gpu ? "z.gpu_start" : "z.start", end = options.gpu ? "z.gpu_end" : "z.end", table = options.gpu ? "gpu_zones z" : "zones z";
    const std::string range = options.gpu ? "z.id IN(SELECT id FROM gpu_range WHERE mint<=?1 AND maxt>=?2 UNION ALL SELECT id FROM gpu_zones WHERE gpu_start IS NULL) AND (z.gpu_start IS NULL OR (z.gpu_start<?3 AND (z.gpu_end IS NULL OR z.gpu_end>?4 OR (z.gpu_end=z.gpu_start AND z.gpu_start>=?4))) OR (z.gpu_end<z.gpu_start AND z.gpu_end<?3 AND z.gpu_start>?4))" : "z.id IN(SELECT id FROM zone_range WHERE mint<=?1 AND maxt>=?2) AND z.start<?3 AND (z.end IS NULL OR z.end>?4 OR (z.end=z.start AND z.start>=?4))";
    const std::string filter = options.gpu && options.context ? " AND z.context=?6" : "";
    const auto bind = [&]( sqlite3_stmt* q ) {
        Int( q, 1, uint64_t( Bucket( options.end - 1 ) ) );
        Int( q, 2, uint64_t( Bucket( options.begin ) ) );
        Int( q, 3, options.end );
        Int( q, 4, options.begin );
        if( options.gpu && options.context ) Int( q, 6, *options.context );
    };
    auto q = d.Prep( "SELECT DISTINCT " + lane + " FROM " + table + " WHERE " + range + filter + " AND " + lane + ">=?5 ORDER BY " + lane + " LIMIT ?7" );
    bind( q );
    Int( q, 5, options.afterLane );
    Int( q, 7, laneLimit + 1 );
    try
    {
        while( d.Step( q ) )
        {
            if( view.lanes.size() == laneLimit )
            {
                view.more = true;
                break;
            }
            StoreTimelineLane row;
            row.id = sqlite3_column_int64( q, 0 );
            Require( row.id < UINT64_MAX, "Timeline lane cursor overflow" );
            view.nextLane = row.id + 1;
            view.lanes.push_back( row );
        }
    }
    catch( ... )
    {
        sqlite3_reset( q );
        throw;
    }
    sqlite3_reset( q );
    if( view.lanes.empty() ) return view;
    std::vector<I> edges( view.bins + 1 );
    const I step = span / view.bins, remainder = span % view.bins;
    for( uint32_t n = 0; n <= view.bins; ++n ) edges[n] = options.begin + step * n + ( remainder * n ) / view.bins;
    struct Diff
    {
        I events = 0, closed = 0, unfinished = 0, invalid = 0;
    };
    std::vector<Diff> delta( view.lanes.size() * view.depths * ( view.bins + 1 ) );
    std::string lanes;
    for( size_t n = 0; n < view.lanes.size(); ++n )
    {
        if( n ) lanes += ',';
        lanes += '?' + std::to_string( 8 + n );
    }
    q = d.Prep( "SELECT " + lane + ",z.depth," + start + "," + end + " FROM " + table + " WHERE " + range + filter + " AND " + lane + " IN(" + lanes + ")" );
    bind( q );
    for( size_t n = 0; n < view.lanes.size(); ++n ) Int( q, 8 + int( n ), view.lanes[n].id );
    try
    {
        while( d.Step( q ) )
        {
            const uint64_t id = sqlite3_column_int64( q, 0 );
            auto found = std::lower_bound( view.lanes.begin(), view.lanes.end(), id, []( const auto& a, uint64_t b ) {
                return a.id < b;
            } );
            Require( found != view.lanes.end() && found->id == id, "Timeline lane mapping mismatch" );
            const size_t index = found - view.lanes.begin();
            const uint32_t depth = uint32_t( sqlite3_column_int64( q, 1 ) );
            found->maximumDepth = std::max( found->maximumDepth, depth );
            ++found->events;
            ++view.events;
            const bool missingStart = sqlite3_column_type( q, 2 ) == SQLITE_NULL, open = sqlite3_column_type( q, 3 ) == SQLITE_NULL;
            const I first = sqlite3_column_int64( q, 2 ), last = open ? options.end : sqlite3_column_int64( q, 3 );
            const bool invalid = !open && !missingStart && last < first;
            if( open ) ++found->unfinished;
            if( invalid ) ++found->invalid;
            if( missingStart )
            {
                ++found->unplaced;
                continue;
            }
            const I lo = std::max( options.begin, std::min( first, last ) ), hi = std::min( options.end, std::max( first, last ) );
            if( lo >= options.end || hi < options.begin ) continue;
            const auto beginBin = size_t( std::upper_bound( edges.begin(), edges.end(), lo ) - edges.begin() - 1 );
            const auto endBin = first == last ? beginBin : size_t( std::lower_bound( edges.begin(), edges.end(), hi ) - edges.begin() - 1 );
            Require( beginBin < view.bins && endBin < view.bins && beginBin <= endBin, "Invalid timeline bin interval" );
            const size_t base = ( index * view.depths + std::min( depth, view.depths - 1 ) ) * ( view.bins + 1 );
            const auto change = [&]( Diff& a, I sign ) {
                a.events += sign;
                a.closed += !open && !invalid ? sign : 0;
                a.unfinished += open ? sign : 0;
                a.invalid += invalid ? sign : 0;
            };
            change( delta[base + beginBin], 1 );
            change( delta[base + endBin + 1], -1 );
        }
    }
    catch( ... )
    {
        sqlite3_reset( q );
        throw;
    }
    sqlite3_reset( q );
    for( size_t laneIndex = 0; laneIndex < view.lanes.size(); ++laneIndex )
        for( uint32_t depth = 0; depth < view.depths; ++depth )
        {
            Diff total;
            const size_t base = ( laneIndex * view.depths + depth ) * ( view.bins + 1 );
            for( uint32_t bin = 0; bin < view.bins; ++bin )
            {
                const auto& change = delta[base + bin];
                total.events += change.events;
                total.closed += change.closed;
                total.unfinished += change.unfinished;
                total.invalid += change.invalid;
                Require( total.events >= 0 && total.closed >= 0 && total.unfinished >= 0 && total.invalid >= 0, "Timeline difference counts underflow" );
                if( total.events ) view.cells.push_back( { view.lanes[laneIndex].id, uint64_t( total.events ), uint64_t( total.closed ), uint64_t( total.unfinished ), uint64_t( total.invalid ), edges[bin], edges[bin + 1], depth } );
            }
        }
    return view;
}
std::vector<StoreTimelineSource> EventStore::TimelineSources() const
{
    auto& d = m_impl->db;
    // Names as ZoneColumns resolves them for a zone without its own name.
    auto q = d.Prep( "SELECT s.kind,s.key,CASE WHEN s.kind=1 THEN COALESCE(NULLIF(s.name,''),s.function,'') WHEN s.nkey!=0 THEN COALESCE(n.value,'???') WHEN s.fkey=0 THEN '' ELSE COALESCE(f.value,'???') END,"
        "COALESCE(s.function,f.value,''),COALESCE(s.file,p.value,''),COALESCE(s.line,0),COALESCE(s.color,0) FROM sources s "
        "LEFT JOIN strings n ON n.kind=?1 AND n.key=s.nkey LEFT JOIN strings f ON f.kind=?1 AND f.key=s.fkey LEFT JOIN strings p ON p.kind=?1 AND p.key=s.pkey ORDER BY s.kind,s.key" );
    Int( q, 1, uint8_t( QueueType::StringData ) );
    std::vector<StoreTimelineSource> result( 1 );
    result.front().name = "???";
    auto& index = m_impl->timelineSources.emplace();
    while( d.Step( q ) )
    {
        index.emplace( std::make_pair( uint8_t( sqlite3_column_int( q, 0 ) ), uint64_t( sqlite3_column_int64( q, 1 ) ) ), uint32_t( result.size() ) );
        result.push_back( { Col( q, 2 ), Col( q, 3 ), Col( q, 4 ), uint32_t( sqlite3_column_int64( q, 5 ) ), uint32_t( sqlite3_column_int64( q, 6 ) ) } );
    }
    sqlite3_reset( q );
    return result;
}
std::vector<std::pair<uint64_t, uint64_t>> EventStore::TimelineZoneCounts( I begin, I end ) const
{
    std::vector<std::pair<uint64_t, uint64_t>> result;
    if( begin >= end ) return result;
    auto& d = m_impl->db;
    // Over much of the capture one pass over the block table beats looking
    // up each block the time index finds.
    const bool wide = double( end - begin ) * 8 > double( std::max<I>( 1, m_impl->info.lastNs ) );
    auto q = d.Prep( wide ? "SELECT b.key,SUM(b.count) FROM blocks b NOT INDEXED WHERE b.stream=1 AND ?1=?1 AND ?2=?2 AND b.tmin<?3 AND b.tmax>=?4 GROUP BY b.key ORDER BY b.key"
                          : "SELECT b.key,SUM(b.count) FROM blocks b WHERE b.stream=1 AND b.id IN(SELECT id FROM block_range WHERE mint<=?1 AND maxt>=?2) AND b.tmin<?3 AND b.tmax>=?4 GROUP BY b.key ORDER BY b.key" );
    Int( q, 1, uint64_t( Bucket( end - 1 ) ) );
    Int( q, 2, uint64_t( Bucket( begin ) ) );
    Int( q, 3, uint64_t( end ) );
    Int( q, 4, uint64_t( begin ) );
    while( d.Step( q ) ) result.emplace_back( uint64_t( sqlite3_column_int64( q, 0 ) ), uint64_t( sqlite3_column_int64( q, 1 ) ) );
    sqlite3_reset( q );
    return result;
}
std::vector<uint64_t> EventStore::TimelineBlocks( uint64_t thread, I begin, I end ) const
{
    std::vector<uint64_t> blocks;
    if( begin >= end ) return blocks;
    auto& d = m_impl->db;
    // Over much of the capture the thread's own blocks, through the key
    // index, are fewer than every thread's blocks the time index finds.
    if( double( end - begin ) * 8 > double( std::max<I>( 1, m_impl->info.lastNs ) ) )
    {
        auto q = d.Prep( "SELECT id FROM blocks INDEXED BY block_key WHERE stream=1 AND key=?1 AND tmin<?2 AND tmax>=?3 ORDER BY first" );
        Int( q, 1, thread );
        Int( q, 2, uint64_t( end ) );
        Int( q, 3, uint64_t( begin ) );
        while( d.Step( q ) ) blocks.push_back( uint64_t( sqlite3_column_int64( q, 0 ) ) );
        sqlite3_reset( q );
    }
    else blocks = ZoneBlocks( d, begin, end, thread, 0 );
    return blocks;
}
bool EventStore::VisitTimelineBlockZones( uint64_t block, I begin, I end, const std::function<bool( const StoreTimelineZone&, std::string_view )>& visit ) const
{
    auto& d = m_impl->db;
    if( !m_impl->timelineSources ) (void)TimelineSources();
    const auto& sources = *m_impl->timelineSources;
    const auto zones = LoadZoneBlock( d, m_impl->blocks, m_impl->zoneCache, block );
    StoreTimelineZone zone;
    zone.thread = zones->thread;
    for( const auto& r : zones->rows )
    {
        if( !ZoneOverlaps( r, begin, end ) ) continue;
        zone.id = r.id;
        zone.start = r.start;
        zone.open = r.open;
        zone.end = r.open ? INT64_MAX : r.end;
        zone.childTime = r.childTime;
        zone.depth = r.depth;
        zone.color = r.hasColor ? r.color : 0;
        const auto found = sources.find( { r.skind, r.skey } );
        zone.source = found == sources.end() ? 0 : found->second;
        if( !visit( zone, r.hasName ? std::string_view( r.name ) : std::string_view() ) ) return false;
    }
    return true;
}
void EventStore::VisitTimelineZones( uint64_t thread, I begin, I end, const std::function<bool( const StoreTimelineZone&, std::string_view )>& visit ) const
{
    for( const auto block : TimelineBlocks( thread, begin, end ) )
        if( !VisitTimelineBlockZones( block, begin, end, visit ) ) return;
}
void EventStore::VisitTimelineGpuZones( uint64_t context, I begin, I end, bool rootsOnly, const std::function<bool( const StoreTimelineZone& )>& visit ) const
{
    if( begin >= end ) return;
    auto& d = m_impl->db;
    if( !m_impl->timelineSources ) (void)TimelineSources();
    const auto& sources = *m_impl->timelineSources;
    const std::string overlap = "z.gpu_start IS NOT NULL AND z.gpu_start<?3 AND (z.gpu_end IS NULL OR z.gpu_end>?4 OR (z.gpu_end=z.gpu_start AND z.gpu_start>=?4))";
    // Without statistics SQLite would walk every zone of the context by its
    // context index; the parent index finds the top-level ones.
    auto q = d.Prep( "SELECT z.id,z.thread,z.depth,z.gpu_start,z.gpu_end,z.skind,z.skey,z.cpu_start,z.cpu_end FROM gpu_zones z " +
        std::string( rootsOnly ? "INDEXED BY gpu_zone_parent WHERE z.parent=0 AND ?1=?1 AND ?2=?2" : "WHERE z.id IN(SELECT id FROM gpu_range WHERE mint<=?1 AND maxt>=?2)" ) +
        " AND z.context=?5 AND " + overlap + " ORDER BY z.id" );
    Int( q, 1, uint64_t( Bucket( end - 1 ) ) );
    Int( q, 2, uint64_t( Bucket( begin ) ) );
    Int( q, 3, uint64_t( end ) );
    Int( q, 4, uint64_t( begin ) );
    Int( q, 5, context );
    StoreTimelineZone zone;
    try
    {
        while( d.Step( q ) )
        {
            zone.id = uint64_t( sqlite3_column_int64( q, 0 ) );
            zone.thread = uint64_t( sqlite3_column_int64( q, 1 ) );
            zone.depth = uint32_t( sqlite3_column_int64( q, 2 ) );
            zone.start = sqlite3_column_int64( q, 3 );
            zone.open = sqlite3_column_type( q, 4 ) == SQLITE_NULL;
            zone.end = zone.open ? INT64_MAX : sqlite3_column_int64( q, 4 );
            const auto found = sources.find( { uint8_t( sqlite3_column_int( q, 5 ) ), uint64_t( sqlite3_column_int64( q, 6 ) ) } );
            zone.source = found == sources.end() ? 0 : found->second;
            zone.cpuStart = sqlite3_column_int64( q, 7 );
            zone.cpuEnd = sqlite3_column_int64( q, 8 );
            if( !visit( zone ) ) break;
        }
    }
    catch( ... )
    {
        sqlite3_reset( q );
        throw;
    }
    sqlite3_reset( q );
}
void EventStore::VisitTimelineBlocks( uint64_t thread, I begin, I end, const std::function<bool( I, I, uint64_t )>& visit ) const
{
    if( begin >= end ) return;
    auto& d = m_impl->db;
    // One thread's blocks through the key index: the time index would visit
    // the blocks of every thread in the range.
    auto q = d.Prep( "SELECT tmin,tmax,count,summary FROM blocks INDEXED BY block_key WHERE stream=1 AND key=?1 AND tmin<?2 AND tmax>=?3 ORDER BY first" );
    Int( q, 1, thread );
    Int( q, 2, uint64_t( end ) );
    Int( q, 3, uint64_t( begin ) );
    try
    {
        while( d.Step( q ) )
        {
            const auto blob = static_cast<const uint8_t*>( sqlite3_column_blob( q, 3 ) );
            const auto bytes = size_t( sqlite3_column_bytes( q, 3 ) );
            // A block without a summary counts all its zones.
            const uint64_t top = bytes ? DecodeSummary( std::span<const uint8_t>( blob, bytes ) ).depth[0] : uint64_t( sqlite3_column_int64( q, 2 ) );
            if( !visit( sqlite3_column_int64( q, 0 ), sqlite3_column_int64( q, 1 ), top ) ) break;
        }
    }
    catch( ... )
    {
        sqlite3_reset( q );
        throw;
    }
    sqlite3_reset( q );
}
std::vector<std::pair<uint64_t, std::string>> EventStore::ThreadNames() const
{
    auto& d = m_impl->db;
    auto q = d.Prep( "SELECT key,value FROM strings WHERE kind=?1 ORDER BY key" );
    Int( q, 1, uint8_t( QueueType::ThreadName ) );
    std::vector<std::pair<uint64_t, std::string>> result;
    while( d.Step( q ) ) result.emplace_back( uint64_t( sqlite3_column_int64( q, 0 ) ), Col( q, 1 ) );
    sqlite3_reset( q );
    return result;
}
std::vector<StoreFrameSet> EventStore::FrameSets() const
{
    auto& d = m_impl->db;
    std::vector<uint64_t> ids;
    auto q = d.Prep( "SELECT setid FROM frame_state ORDER BY setid" );
    while( d.Step( q ) ) ids.push_back( uint64_t( sqlite3_column_int64( q, 0 ) ) );
    sqlite3_reset( q );
    std::vector<StoreFrameSet> result;
    for( const auto id : ids ) if( auto set = FrameSet( id ) ) result.push_back( std::move( *set ) );
    return result;
}
std::vector<StoreFrame> EventStore::AllFrames() const
{
    auto& d = m_impl->db;
    auto q = d.Prep( "SELECT id,setid,start,end,continuous FROM frames ORDER BY setid,start,id" );
    std::vector<StoreFrame> result;
    while( d.Step( q ) )
    {
        StoreFrame f;
        f.id = uint64_t( sqlite3_column_int64( q, 0 ) );
        f.set = uint64_t( sqlite3_column_int64( q, 1 ) );
        f.start = sqlite3_column_int64( q, 2 );
        if( sqlite3_column_type( q, 3 ) != SQLITE_NULL ) f.end = sqlite3_column_int64( q, 3 );
        f.continuous = sqlite3_column_int( q, 4 ) != 0;
        result.push_back( f );
    }
    sqlite3_reset( q );
    return result;
}

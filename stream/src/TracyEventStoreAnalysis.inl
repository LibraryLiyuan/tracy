namespace
{
void AnalysisBudget( Db& db, uint64_t limit )
{
    const auto pages = uint64_t( db.Scalar( "PRAGMA page_count" ) ) + uint64_t( db.Scalar( "PRAGMA temp.page_count" ) );
    Require( pages * uint64_t( db.Scalar( "PRAGMA page_size" ) ) <= limit, "Analysis temporary disk budget exceeded" );
}
template<class F>
void AnalysisEach( Db& db, const std::string& sql, F callback )
{
    auto q = db.Prep( sql );
    try
    {
        while( db.Step( q ) ) callback( q );
    }
    catch( ... )
    {
        sqlite3_reset( q );
        throw;
    }
    sqlite3_reset( q );
}
StoreAnalysisRow AnalysisRow( sqlite3_stmt* q )
{
    StoreAnalysisRow r;
    r.id = sqlite3_column_int64( q, 1 );
    r.source = sqlite3_column_int64( q, 2 );
    r.parent = sqlite3_column_int64( q, 3 );
    r.thread = sqlite3_column_int64( q, 4 );
    r.context = sqlite3_column_int64( q, 5 );
    r.example = sqlite3_column_int64( q, 6 );
    r.events = sqlite3_column_int64( q, 7 );
    r.closed = sqlite3_column_int64( q, 8 );
    r.unfinished = sqlite3_column_int64( q, 9 );
    r.invalid = sqlite3_column_int64( q, 10 );
    r.selfUnknown = sqlite3_column_int64( q, 11 );
    r.subtreeEvents = sqlite3_column_int64( q, 12 );
    r.inclusive = sqlite3_column_int64( q, 13 );
    r.self = sqlite3_column_int64( q, 14 );
    r.minimum = sqlite3_column_int64( q, 15 );
    r.maximum = sqlite3_column_int64( q, 16 );
    r.name = Col( q, 17 );
    r.function = Col( q, 18 );
    r.file = Col( q, 19 );
    r.line = sqlite3_column_int( q, 20 );
    return r;
}
StorePage<StoreAnalysisRow> AnalysisRows( Db& db, sqlite3_stmt* q, size_t limit )
{
    StorePage<StoreAnalysisRow> page;
    const auto capacity = std::min<uint64_t>( limit, ( db.options.resultBytes - sizeof( page ) ) / ( 2 * sizeof( StoreAnalysisRow ) ) );
    page.values.reserve( size_t( capacity ) );
    size_t bytes = sizeof( page ) + page.values.capacity() * sizeof( StoreAnalysisRow );
    try
    {
        while( db.Step( q ) )
        {
            if( page.values.size() == capacity )
            {
                page.more = true;
                break;
            }
            auto row = AnalysisRow( q );
            const auto extra = row.name.capacity() + row.function.capacity() + row.file.capacity() + 3;
            if( extra > db.options.resultBytes - bytes )
            {
                Require( !page.values.empty(), "Analysis row exceeds result budget" );
                page.more = true;
                break;
            }
            bytes += extra;
            page.next = sqlite3_column_int64( q, 0 );
            page.values.push_back( std::move( row ) );
        }
    }
    catch( ... )
    {
        sqlite3_reset( q );
        throw;
    }
    sqlite3_reset( q );
    return page;
}
}
std::unique_ptr<StoreAnalysis> EventStore::Analyze( const StoreAnalysisOptions& options ) const
{
    Require( options.begin < options.end, "Invalid analysis interval" );
    auto state = std::make_unique<StoreAnalysis::Impl>( m_impl->db.options );
    auto& d = *state->db;
    auto& source = m_impl->db;
    auto& summary = state->info;
    summary.revision = m_impl->info.revision;
    summary.gpu = options.gpu;
    summary.begin = options.begin;
    summary.end = options.end;
    d.Exec( "PRAGMA max_page_count=" + std::to_string( std::max<uint64_t>( 1, options.temporaryBytes / 4096 ) ) );
    d.Exec( R"SQL(
BEGIN;
CREATE TABLE a_sources(id INTEGER PRIMARY KEY,kind INTEGER,skey INTEGER,name TEXT,function TEXT,file TEXT,line INTEGER,color INTEGER,identity_name TEXT,UNIQUE(kind,skey,identity_name,function,file,line,color));
CREATE TABLE a_source_map(kind INTEGER,skey INTEGER,source INTEGER,PRIMARY KEY(kind,skey)) WITHOUT ROWID;
CREATE TABLE a_events(id INTEGER PRIMARY KEY,parent INTEGER,thread INTEGER,context INTEGER,source INTEGER NOT NULL,begin INTEGER,finish INTEGER,name TEXT,lo INTEGER,hi INTEGER,included INTEGER,closed INTEGER,unfinished INTEGER,invalid INTEGER,self_unknown INTEGER,duration INTEGER CHECK(typeof(duration)='integer' AND duration>=0),self INTEGER);
CREATE INDEX a_event_parent ON a_events(parent,id);
CREATE INDEX a_event_source ON a_events(source,id);
CREATE TABLE a_text(zone INTEGER,event INTEGER,text TEXT,PRIMARY KEY(zone,event)) WITHOUT ROWID;
CREATE TABLE a_coverage(parent INTEGER PRIMARY KEY,covered INTEGER);
CREATE TABLE a_nodes(id INTEGER PRIMARY KEY,parent INTEGER,thread INTEGER,context INTEGER,source INTEGER,example INTEGER,events INTEGER DEFAULT 0,closed INTEGER DEFAULT 0,unfinished INTEGER DEFAULT 0,invalid INTEGER DEFAULT 0,self_unknown INTEGER DEFAULT 0,subtree INTEGER DEFAULT 0,inclusive INTEGER DEFAULT 0 CHECK(typeof(inclusive)='integer'),self INTEGER DEFAULT 0 CHECK(typeof(self)='integer'),minimum INTEGER,maximum INTEGER,child_time REAL DEFAULT 0,time_weight REAL DEFAULT 0,desc_nodes INTEGER DEFAULT 0,UNIQUE(parent,thread,context,source));
CREATE TABLE a_paths(id INTEGER PRIMARY KEY,node INTEGER);
CREATE TABLE a_order(parent INTEGER,rank INTEGER,node INTEGER,PRIMARY KEY(parent,rank)) WITHOUT ROWID;
)SQL" );
    AnalysisBudget( d, options.temporaryBytes );
    std::string where = " WHERE 1=1";
    if( options.thread ) where += " AND thread=" + std::to_string( I( *options.thread ) );
    if( options.context && options.gpu ) where += " AND context=" + std::to_string( I( *options.context ) );
    // CPU zones come from the zone event blocks of the scope. A bounded window
    // reads only the blocks whose time range overlaps it: a block that holds
    // an included zone also places that zone's ancestors in overlapping
    // blocks, since an ancestor's interval contains the zone's.
    const std::string blockScope = options.thread ? " AND key=" + std::to_string( I( *options.thread ) ) : "";
    const bool whole = options.begin == INT64_MIN && options.end == INT64_MAX;
    // The whole capture of a finished store is aggregated from the call tree
    // that ingestion collected: an interval that starts at or before the
    // first zone and ends after the last timestamp includes every zone whole.
    bool tree = false;
    if( !options.gpu && !options.events && !m_impl->snapshot && m_impl->info.zoneTree )
        tree = options.begin <= source.Scalar( "SELECT COALESCE(MIN(tmin),0) FROM blocks WHERE stream=1" ) && options.end > m_impl->info.lastNs;
    const std::string treeScope = options.thread ? " WHERE thread=" + std::to_string( I( *options.thread ) ) : "";
    summary.callTree = tree;
    struct ZoneBlockRange
    {
        uint64_t id, thread, first, last;
    };
    std::vector<uint64_t> zoneBlocks;
    std::vector<ZoneBlockRange> zoneRanges;
    uint64_t total = 0;
    if( tree )
    {
        source.Exec( "CREATE TEMP TABLE IF NOT EXISTS analysis_sources(skind INTEGER,skey INTEGER,PRIMARY KEY(skind,skey)) WITHOUT ROWID;DELETE FROM temp.analysis_sources;INSERT INTO temp.analysis_sources SELECT DISTINCT skind,skey FROM zone_tree" + treeScope );
        total = uint64_t( source.Scalar( "SELECT COALESCE(SUM(events),0) FROM zone_tree" + treeScope ) );
    }
    else if( !options.gpu )
    {
        auto q = source.Prep( whole ? "SELECT b.id,b.key,b.first,b.last,b.count FROM blocks b WHERE b.stream=1" + blockScope + " ORDER BY b.first"
                                    : "SELECT b.id,b.key,b.first,b.last,b.count FROM blocks b WHERE b.stream=1 AND b.id IN(SELECT id FROM block_range WHERE mint<=?1 AND maxt>=?2) AND b.tmin<?3 AND b.tmax>=?4" + blockScope + " ORDER BY b.first" );
        if( !whole )
        {
            Int( q, 1, uint64_t( Bucket( options.end - 1 ) ) );
            Int( q, 2, uint64_t( Bucket( options.begin ) ) );
            Int( q, 3, uint64_t( options.end ) );
            Int( q, 4, uint64_t( options.begin ) );
        }
        while( source.Step( q ) )
        {
            zoneBlocks.push_back( sqlite3_column_int64( q, 0 ) );
            zoneRanges.push_back( { uint64_t( sqlite3_column_int64( q, 0 ) ), uint64_t( sqlite3_column_int64( q, 1 ) ), uint64_t( sqlite3_column_int64( q, 2 ) ), uint64_t( sqlite3_column_int64( q, 3 ) ) } );
            total += uint64_t( sqlite3_column_int64( q, 4 ) );
        }
        sqlite3_reset( q );
        std::set<std::pair<uint8_t, uint64_t>> kinds;
        for( const auto block : zoneBlocks )
            for( const auto& r : LoadZoneBlock( source, m_impl->blocks, m_impl->zoneCache, block )->rows ) kinds.insert( { r.skind, r.skey } );
        source.Exec( "CREATE TEMP TABLE IF NOT EXISTS analysis_sources(skind INTEGER,skey INTEGER,PRIMARY KEY(skind,skey)) WITHOUT ROWID;DELETE FROM temp.analysis_sources;" );
        for( const auto& [kind, key] : kinds )
        {
            auto put = source.Prep( "INSERT INTO temp.analysis_sources VALUES(?,?)" );
            Int( put, 1, kind );
            Int( put, 2, key );
            source.Run( put );
        }
    }
    if( options.gpu ) total = uint64_t( source.Scalar( "SELECT COUNT(*) FROM gpu_zones" + where ) );
    uint64_t done = 0;
    if( options.progress ) options.progress( 0, total * 2 );
    const auto kind = std::to_string( uint8_t( QueueType::StringData ) );
    AnalysisEach( source, "SELECT k.kind,k.skey,CASE WHEN s.key IS NULL THEN '???' WHEN s.kind=1 THEN COALESCE(NULLIF(s.name,''),s.function,'') WHEN s.nkey!=0 THEN COALESCE(n.value,'???') WHEN s.fkey=0 THEN '' ELSE COALESCE(f.value,'???') END,COALESCE(s.function,f.value,''),COALESCE(s.file,p.value,''),COALESCE(s.line,0),COALESCE(s.color,0),COALESCE(s.name,''),s.key IS NOT NULL FROM (SELECT DISTINCT skind kind,skey FROM " + ( options.gpu ? "gpu_zones" + where : std::string( "temp.analysis_sources" ) ) + ") k LEFT JOIN sources s ON s.kind=k.kind AND s.key=k.skey LEFT JOIN strings n ON n.kind=" + kind + " AND n.key=s.nkey LEFT JOIN strings f ON f.kind=" + kind + " AND f.key=s.fkey LEFT JOIN strings p ON p.kind=" + kind + " AND p.key=s.pkey ORDER BY k.kind,k.skey", [&]( auto row ) {
        const auto sourceKind = sqlite3_column_int64( row, 0 ), rawKey = sqlite3_column_int64( row, 1 );
        const auto canonicalKey = sourceKind == 1 && sqlite3_column_int( row, 8 ) ? 0 : rawKey;
        auto q = d.Prep( "INSERT OR IGNORE INTO a_sources(kind,skey,name,function,file,line,color,identity_name) VALUES(?,?,?,?,?,?,?,?)" );
        Int( q, 1, sqlite3_column_int64( row, 0 ) );
        Int( q, 2, canonicalKey );
        for( int n = 2; n < 5; ++n ) ::tracy::stream::Text( q, n + 1, Col( row, n ) );
        Int( q, 6, sqlite3_column_int64( row, 5 ) );
        Int( q, 7, sqlite3_column_int64( row, 6 ) );
        ::tracy::stream::Text( q, 8, Col( row, 7 ) );
        d.Run( q );
        q = d.Prep( "SELECT id FROM a_sources WHERE kind=?1 AND skey=?2 AND identity_name=?3 AND function=?4 AND file=?5 AND line=?6 AND color=?7" );
        Int( q, 1, sourceKind );
        Int( q, 2, canonicalKey );
        ::tracy::stream::Text( q, 3, Col( row, 7 ) );
        ::tracy::stream::Text( q, 4, Col( row, 3 ) );
        ::tracy::stream::Text( q, 5, Col( row, 4 ) );
        Int( q, 6, sqlite3_column_int64( row, 5 ) );
        Int( q, 7, sqlite3_column_int64( row, 6 ) );
        Require( d.Step( q ), "Missing canonical analysis source" );
        const uint64_t identity = sqlite3_column_int64( q, 0 );
        sqlite3_reset( q );
        q = d.Prep( "INSERT INTO a_source_map VALUES(?,?,?)" );
        Int( q, 1, sourceKind );
        Int( q, 2, rawKey );
        Int( q, 3, identity );
        d.Run( q );
    } );
    const auto progress = [&] {
        if( ( ++done & 1023 ) == 0 )
        {
            AnalysisBudget( d, options.temporaryBytes );
            if( options.progress ) options.progress( done, total * 2 );
        }
    };
    if( tree ) {}
    else if( options.gpu ) AnalysisEach( source, "SELECT id,parent,thread,context,skind,skey,gpu_start,gpu_end,NULL FROM gpu_zones" + where + " ORDER BY id", [&]( auto row ) {
        auto q = d.Prep( "INSERT INTO a_events(id,parent,thread,context,source,begin,finish,name,duration) VALUES(?1,?2,?3,?4,(SELECT source FROM a_source_map WHERE kind=?5 AND skey=?6),?7,?8,?9,0)" );
        for( int n = 0; n < 8; ++n )
            if( sqlite3_column_type( row, n ) != SQLITE_NULL ) Int( q, n + 1, sqlite3_column_int64( row, n ) );
        d.Run( q );
        progress();
    } );
    else
    {
        for( const auto block : zoneBlocks )
        {
            const auto zones = LoadZoneBlock( source, m_impl->blocks, m_impl->zoneCache, block );
            for( const auto& r : zones->rows )
            {
                auto q = d.Prep( "INSERT INTO a_events(id,parent,thread,context,source,begin,finish,name,duration) VALUES(?1,?2,?3,0,(SELECT source FROM a_source_map WHERE kind=?5 AND skey=?6),?7,?8,?9,0)" );
                Int( q, 1, r.id );
                Int( q, 2, r.parent );
                Int( q, 3, zones->thread );
                Int( q, 5, r.skind );
                Int( q, 6, r.skey );
                Int( q, 7, uint64_t( r.start ) );
                if( !r.open ) Int( q, 8, uint64_t( r.end ) );
                if( r.hasName ) ::tracy::stream::Text( q, 9, r.name );
                d.Run( q );
                progress();
            }
        }
        // Texts of the read zones: a thread's text blocks are in zone order, so
        // the blocks overlapping each zone block's id range hold them. Rows of
        // other zones in those blocks never match an analysis event.
        std::vector<uint64_t> textBlocks;
        if( whole )
        {
            auto q = source.Prep( "SELECT id FROM blocks WHERE stream=2" + blockScope );
            while( source.Step( q ) ) textBlocks.push_back( sqlite3_column_int64( q, 0 ) );
            sqlite3_reset( q );
        }
        else
        {
            std::set<uint64_t> unique;
            for( const auto& range : zoneRanges )
            {
                auto q = source.Prep( "SELECT id FROM blocks WHERE stream=2 AND key=?1 AND first<=?3 AND last>=?2" );
                Int( q, 1, range.thread );
                Int( q, 2, range.first );
                Int( q, 3, range.last );
                while( source.Step( q ) ) unique.insert( uint64_t( sqlite3_column_int64( q, 0 ) ) );
                sqlite3_reset( q );
            }
            textBlocks.assign( unique.begin(), unique.end() );
        }
        const auto putText = [&]( uint64_t zone, uint64_t event, const std::string& text ) {
            auto put = d.Prep( "INSERT INTO a_text VALUES(?,?,?)" );
            Int( put, 1, zone );
            Int( put, 2, event );
            ::tracy::stream::Text( put, 3, text );
            d.Run( put );
        };
        for( const auto block : textBlocks )
            for( const auto& t : LoadTextBlock( source, m_impl->blocks, m_impl->zoneCache, block )->rows ) putText( t.zone, t.event, t.text );
        AnalysisEach( source, "SELECT zone,event,text FROM zone_late_text" + std::string( options.thread ? " WHERE thread=" + std::to_string( I( *options.thread ) ) : "" ), [&]( auto row ) { putText( sqlite3_column_int64( row, 0 ), sqlite3_column_int64( row, 1 ), Col( row, 2 ) ); } );
    }
    if( tree )
    {
        // Call tree nodes in creation order, which is the order of their first
        // zones; nodes whose sources share one definition merge.
        std::unordered_map<uint64_t, uint64_t> merged;
        AnalysisEach( source, "SELECT id,parent,thread,skind,skey,example,events,closed,inclusive,self,minimum,maximum FROM zone_tree" + treeScope + " ORDER BY id", [&]( auto row ) {
            const uint64_t raw = sqlite3_column_int64( row, 0 ), rawParent = sqlite3_column_int64( row, 1 ), thread = sqlite3_column_int64( row, 2 );
            uint64_t parent = 0;
            if( rawParent )
            {
                const auto found = merged.find( rawParent );
                Require( found != merged.end(), "Analysis call tree parent is missing" );
                parent = found->second;
            }
            auto map = d.Prep( "SELECT source FROM a_source_map WHERE kind=? AND skey=?" );
            Int( map, 1, sqlite3_column_int64( row, 3 ) );
            Int( map, 2, sqlite3_column_int64( row, 4 ) );
            Require( d.Step( map ), "Missing canonical analysis source" );
            const uint64_t sourceId = sqlite3_column_int64( map, 0 );
            sqlite3_reset( map );
            auto put = d.Prep( "INSERT OR IGNORE INTO a_nodes(parent,thread,context,source,example) VALUES(?,?,0,?,?)" );
            Int( put, 1, parent );
            Int( put, 2, thread );
            Int( put, 3, sourceId );
            Int( put, 4, sqlite3_column_int64( row, 5 ) );
            d.Run( put );
            auto nodeQuery = d.Prep( "SELECT id FROM a_nodes WHERE parent=? AND thread=? AND context=0 AND source=?" );
            Int( nodeQuery, 1, parent );
            Int( nodeQuery, 2, thread );
            Int( nodeQuery, 3, sourceId );
            Require( d.Step( nodeQuery ), "Missing analysis flame node" );
            const uint64_t node = sqlite3_column_int64( nodeQuery, 0 );
            sqlite3_reset( nodeQuery );
            merged.emplace( raw, node );
            const uint64_t events = sqlite3_column_int64( row, 6 ), closed = sqlite3_column_int64( row, 7 );
            Require( closed <= events, "Invalid call tree node" );
            // Open zones are the unfinished ones; their self time is unknown.
            put = d.Prep( "UPDATE a_nodes SET events=events+?2,closed=closed+?3,unfinished=unfinished+?4,self_unknown=self_unknown+?4,inclusive=inclusive+?5,self=self+?6,subtree=subtree+?2,minimum=CASE WHEN ?3 THEN MIN(COALESCE(minimum,?7),?7) ELSE minimum END,maximum=CASE WHEN ?3 THEN MAX(COALESCE(maximum,?8),?8) ELSE maximum END WHERE id=?1" );
            Int( put, 1, node );
            Int( put, 2, events );
            Int( put, 3, closed );
            Int( put, 4, events - closed );
            Int( put, 5, sqlite3_column_int64( row, 8 ) );
            Int( put, 6, sqlite3_column_int64( row, 9 ) );
            if( closed )
            {
                Int( put, 7, sqlite3_column_int64( row, 10 ) );
                Int( put, 8, sqlite3_column_int64( row, 11 ) );
            }
            d.Run( put );
            done += events;
            if( options.progress ) options.progress( done, total * 2 );
        } );
        AnalysisBudget( d, options.temporaryBytes );
    }
    else
    {
        auto q = d.Prep( "UPDATE a_events SET lo=MAX(begin,?1),hi=MIN(finish,?2),included=CASE WHEN begin IS NULL THEN 1 WHEN finish IS NULL THEN begin<?2 WHEN finish<begin THEN finish<?2 AND begin>=?1 ELSE begin<?2 AND (finish>?1 OR (finish=begin AND begin>=?1)) END" );
        Int( q, 1, options.begin );
        Int( q, 2, options.end );
        d.Run( q );
        d.Exec( "UPDATE a_events SET closed=included AND begin IS NOT NULL AND finish IS NOT NULL AND finish>=begin,unfinished=included AND (begin IS NULL OR finish IS NULL),invalid=included AND finish<begin,duration=CASE WHEN included AND finish>=begin THEN MAX(0,hi-lo) ELSE 0 END" );
        // Union clipped direct-child intervals; overlapping GPU siblings must not
        // be subtracted twice from parent self time.
        d.Exec( R"SQL(
    INSERT INTO a_coverage
    WITH intervals AS (SELECT p.id parent,MAX(c.lo,p.lo) lo,MIN(c.hi,p.hi) hi FROM a_events c JOIN a_events p ON c.parent=p.id WHERE p.closed AND c.closed AND c.hi>p.lo AND c.lo<p.hi),
    ordered AS (SELECT parent,lo,hi,MAX(hi) OVER(PARTITION BY parent ORDER BY lo,hi ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING) prior FROM intervals)
    SELECT parent,SUM(MAX(0,hi-MAX(lo,COALESCE(prior,lo)))) FROM ordered GROUP BY parent;
    UPDATE a_events SET self_unknown=included AND (NOT closed OR EXISTS(SELECT 1 FROM a_events c WHERE c.parent=a_events.id AND c.included AND NOT c.closed));
    UPDATE a_events SET self=CASE WHEN included AND NOT self_unknown THEN MAX(0,duration-COALESCE((SELECT covered FROM a_coverage c WHERE c.parent=a_events.id),0)) ELSE 0 END;
    )SQL" );
        AnalysisEach( d, "SELECT id,parent,thread,context,source,included,closed,unfinished,COALESCE(invalid,0),self_unknown,duration,self FROM a_events ORDER BY id", [&]( auto row ) {
            const uint64_t event = sqlite3_column_int64( row, 0 ), rawParent = sqlite3_column_int64( row, 1 );
            uint64_t parent = 0;
            if( rawParent )
            {
                auto p = d.Prep( "SELECT node FROM a_paths WHERE id=?" );
                Int( p, 1, rawParent );
                const bool found = d.Step( p );
                if( found ) parent = sqlite3_column_int64( p, 0 );
                sqlite3_reset( p );
                // A zone read from a window block whose parent was not read lies
                // outside the window, and so does its subtree.
                if( !found )
                {
                    Require( !whole && !options.gpu && !sqlite3_column_int( row, 5 ), "Analysis parent is missing or outside the requested thread/context" );
                    return;
                }
            }
            auto put = d.Prep( "INSERT OR IGNORE INTO a_nodes(parent,thread,context,source,example) VALUES(?,?,?,?,?)" );
            Int( put, 1, parent );
            for( int n = 2; n <= 4; ++n ) Int( put, n, sqlite3_column_int64( row, n ) );
            Int( put, 5, event );
            d.Run( put );
            auto nodeQuery = d.Prep( "SELECT id FROM a_nodes WHERE parent=? AND thread=? AND context=? AND source=?" );
            Int( nodeQuery, 1, parent );
            for( int n = 2; n <= 4; ++n ) Int( nodeQuery, n, sqlite3_column_int64( row, n ) );
            Require( d.Step( nodeQuery ), "Missing analysis flame node" );
            const uint64_t node = sqlite3_column_int64( nodeQuery, 0 );
            sqlite3_reset( nodeQuery );
            put = d.Prep( "INSERT INTO a_paths VALUES(?,?)" );
            Int( put, 1, event );
            Int( put, 2, node );
            d.Run( put );
            put = d.Prep( "UPDATE a_nodes SET events=events+?2,closed=closed+?3,unfinished=unfinished+?4,invalid=invalid+?5,self_unknown=self_unknown+?6,inclusive=inclusive+?7,self=self+?8,subtree=subtree+?2,minimum=CASE WHEN ?3 THEN MIN(COALESCE(minimum,?7),?7) ELSE minimum END,maximum=CASE WHEN ?3 THEN MAX(COALESCE(maximum,?7),?7) ELSE maximum END WHERE id=?1" );
            Int( put, 1, node );
            for( int n = 5; n <= 11; ++n ) Int( put, n - 3, sqlite3_column_int64( row, n ) );
            d.Run( put );
            if( ( ++done & 1023 ) == 0 )
            {
                AnalysisBudget( d, options.temporaryBytes );
                if( options.progress ) options.progress( done, total * 2 );
            }
        } );
    }
    if( options.progress ) options.progress( done, total * 2 );
    AnalysisBudget( d, options.temporaryBytes );
    AnalysisEach( d, "SELECT id,parent,subtree,inclusive,self,child_time,desc_nodes FROM a_nodes ORDER BY id DESC", [&]( auto row ) {
        const auto weight = std::max( double( sqlite3_column_int64( row, 3 ) ), double( sqlite3_column_int64( row, 4 ) ) + sqlite3_column_double( row, 5 ) );
        Require( std::isfinite( weight ) && weight >= 0, "Flame aggregate cost overflow" );
        const uint64_t descendants = sqlite3_column_int64( row, 6 ) + ( sqlite3_column_int64( row, 2 ) > 0 ? 1 : 0 );
        auto update = d.Prep( "UPDATE a_nodes SET time_weight=?2,desc_nodes=?3 WHERE id=?1" );
        Int( update, 1, sqlite3_column_int64( row, 0 ) );
        Require( sqlite3_bind_double( update, 2, weight ) == SQLITE_OK, "Flame weight binding failed" );
        Int( update, 3, descendants );
        d.Run( update );
        const uint64_t parent = sqlite3_column_int64( row, 1 );
        if( parent )
        {
            auto put = d.Prep( "UPDATE a_nodes SET subtree=subtree+?2,child_time=child_time+?3,desc_nodes=desc_nodes+?4 WHERE id=?1" );
            Int( put, 1, parent );
            Int( put, 2, sqlite3_column_int64( row, 2 ) );
            Require( sqlite3_bind_double( put, 3, weight ) == SQLITE_OK, "Child flame weight binding failed" );
            Int( put, 4, descendants );
            d.Run( put );
        }
    } );
    d.Exec( R"SQL(
INSERT INTO a_order SELECT parent,ROW_NUMBER() OVER(PARTITION BY parent ORDER BY inclusive DESC,id),id FROM a_nodes WHERE subtree>0;
CREATE TABLE a_layout AS SELECT o.parent,o.rank,o.node,
COALESCE(SUM(n.time_weight) OVER(PARTITION BY o.parent ORDER BY o.rank ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING),0) time_left,
SUM(n.time_weight) OVER(PARTITION BY o.parent ORDER BY o.rank ROWS UNBOUNDED PRECEDING) time_right,
COALESCE(SUM(CAST(n.subtree AS REAL)) OVER(PARTITION BY o.parent ORDER BY o.rank ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING),0) count_left,
SUM(CAST(n.subtree AS REAL)) OVER(PARTITION BY o.parent ORDER BY o.rank ROWS UNBOUNDED PRECEDING) count_right,
n.time_weight time_width,CAST(n.subtree AS REAL) count_width FROM a_order o JOIN a_nodes n ON n.id=o.node;
CREATE UNIQUE INDEX a_layout_rank ON a_layout(parent,rank);
CREATE UNIQUE INDEX a_layout_node ON a_layout(node);
CREATE INDEX a_layout_time ON a_layout(parent,time_width);
CREATE INDEX a_layout_count ON a_layout(parent,count_width);
)SQL" );
    // Per-source totals: over the included zones, or over the call tree
    // nodes, which hold every zone once.
    d.Exec( tree ? "CREATE TABLE a_stats AS SELECT ROW_NUMBER() OVER(ORDER BY SUM(inclusive) DESC,source) rank,source,MIN(example) example,SUM(events) events,SUM(closed) closed,SUM(unfinished) unfinished,SUM(invalid) invalid,SUM(self_unknown) self_unknown,SUM(inclusive) inclusive,SUM(self) self,COALESCE(MIN(minimum),0) minimum,COALESCE(MAX(maximum),0) maximum FROM a_nodes WHERE events>0 GROUP BY source;CREATE UNIQUE INDEX a_stats_rank ON a_stats(rank);"
                 : "CREATE TABLE a_stats AS SELECT ROW_NUMBER() OVER(ORDER BY SUM(duration) DESC,source) rank,source,MIN(id) example,SUM(included) events,SUM(closed) closed,SUM(unfinished) unfinished,SUM(COALESCE(invalid,0)) invalid,SUM(self_unknown) self_unknown,SUM(duration) inclusive,SUM(self) self,COALESCE(MIN(CASE WHEN closed THEN duration END),0) minimum,COALESCE(MAX(CASE WHEN closed THEN duration END),0) maximum FROM a_events WHERE included GROUP BY source;CREATE UNIQUE INDEX a_stats_rank ON a_stats(rank);" );
    auto q = d.Prep( "SELECT COALESCE(SUM(events),0),COALESCE(SUM(closed),0),COALESCE(SUM(unfinished),0),COALESCE(SUM(invalid),0),COALESCE(SUM(self_unknown),0),COUNT(*) FROM a_stats" );
    Require( d.Step( q ), "Missing analysis summary" );
    summary.events = sqlite3_column_int64( q, 0 );
    summary.closed = sqlite3_column_int64( q, 1 );
    summary.unfinished = sqlite3_column_int64( q, 2 );
    summary.invalid = sqlite3_column_int64( q, 3 );
    summary.selfUnknown = sqlite3_column_int64( q, 4 );
    summary.sources = sqlite3_column_int64( q, 5 );
    sqlite3_reset( q );
    summary.nodes = d.Scalar( "SELECT COUNT(*) FROM a_nodes WHERE subtree>0" );
    AnalysisBudget( d, options.temporaryBytes );
    d.Exec( "COMMIT" );
    summary.workspaceBytes = uint64_t( d.Scalar( "PRAGMA page_count" ) ) * uint64_t( d.Scalar( "PRAGMA page_size" ) );
    if( tree )
    {
        auto open = source.options;
        open.verifyContent = false;
        open.cancelled = {};
        open.progress = {};
        open.stage = {};
        state->zones = EventStore::Open( m_impl->source, open );
        state->thread = options.thread;
    }
    return std::unique_ptr<StoreAnalysis>( new StoreAnalysis( std::move( state ) ) );
}
StorePage<StoreAnalysisRow> StoreAnalysis::Statistics( uint64_t after, size_t limit ) const
{
    Require( limit > 0 && limit <= 4096, "Analysis page limit exceeded" );
    auto& d = *m_impl->db;
    auto q = d.Prep( "SELECT a.rank,a.rank,a.source,0,0,0,a.example,a.events,a.closed,a.unfinished,a.invalid,a.self_unknown,a.events,a.inclusive,a.self,a.minimum,a.maximum,substr(s.name,1,2048),substr(s.function,1,2048),substr(s.file,1,2048),s.line FROM a_stats a JOIN a_sources s ON s.id=a.source WHERE a.rank>? ORDER BY a.rank LIMIT ?" );
    Int( q, 1, after );
    Int( q, 2, limit + 1 );
    return AnalysisRows( d, q, limit );
}
StorePage<StoreAnalysisRow> StoreAnalysis::Flame( uint64_t parent, uint64_t after, size_t limit ) const
{
    Require( limit > 0 && limit <= 4096, "Flame page limit exceeded" );
    auto& d = *m_impl->db;
    auto q = d.Prep( "SELECT o.rank,n.id,n.source,n.parent,n.thread,n.context,n.example,n.events,n.closed,n.unfinished,n.invalid,n.self_unknown,n.subtree,n.inclusive,n.self,COALESCE(n.minimum,0),COALESCE(n.maximum,0),substr(s.name,1,2048),substr(s.function,1,2048),substr(s.file,1,2048),s.line FROM a_order o JOIN a_nodes n ON n.id=o.node JOIN a_sources s ON s.id=n.source WHERE o.parent=? AND o.rank>? ORDER BY o.rank LIMIT ?" );
    Int( q, 1, parent );
    Int( q, 2, after );
    Int( q, 3, limit + 1 );
    return AnalysisRows( d, q, limit );
}
uint64_t StoreAnalysis::FlameParent( uint64_t node ) const
{
    if( !node ) return 0;
    auto& d = *m_impl->db;
    auto q = d.Prep( "SELECT parent FROM a_nodes WHERE id=?" );
    Int( q, 1, node );
    Require( d.Step( q ), "Unknown flame node" );
    const uint64_t parent = sqlite3_column_int64( q, 0 );
    sqlite3_reset( q );
    return parent;
}
StoreFlameView StoreAnalysis::FlameView( const StoreFlameOptions& options ) const
{
    Require( options.depth > 0 && options.depth <= 64 && options.pixels > 0 && options.pixels <= 16384 && options.maximumNodes > 0 && options.maximumNodes <= 4096, "Invalid flame view budget" );
    auto& d = *m_impl->db;
    StoreFlameView view;
    const size_t capacity = std::min<uint64_t>( options.maximumNodes, ( d.options.resultBytes - sizeof( view ) ) / ( 2 * sizeof( StoreFlameBox ) ) );
    Require( capacity > 0, "Flame view result budget too small" );
    view.boxes.reserve( capacity );
    size_t bytes = sizeof( view ) + view.boxes.capacity() * sizeof( StoreFlameBox );
    const std::string columns = "SELECT l.rank,n.id,n.source,n.parent,n.thread,n.context,n.example,n.events,n.closed,n.unfinished,n.invalid,n.self_unknown,n.subtree,n.inclusive,n.self,COALESCE(n.minimum,0),COALESCE(n.maximum,0),substr(s.name,1,2048),substr(s.function,1,2048),substr(s.file,1,2048),s.line";
    struct Pending
    {
        uint64_t parent;
        double begin, end, weight;
        uint32_t depth;
    };
    std::vector<Pending> pending;
    pending.reserve( capacity + 1 );
    double countWeight = 0, timeWeight = 0;
    if( options.focus )
    {
        auto q = d.Prep( columns + ",n.time_weight,n.subtree,n.desc_nodes FROM a_nodes n JOIN a_sources s ON s.id=n.source LEFT JOIN a_layout l ON l.node=n.id WHERE n.id=?" );
        Int( q, 1, options.focus );
        Require( d.Step( q ), "Unknown flame focus" );
        StoreFlameBox box;
        box.row = AnalysisRow( q );
        box.end = 1;
        timeWeight = sqlite3_column_double( q, 21 );
        countWeight = sqlite3_column_double( q, 22 );
        view.totalNodes = sqlite3_column_int64( q, 23 );
        sqlite3_reset( q );
        view.eventCounts = options.eventCounts || timeWeight <= 0;
        view.totalWeight = view.eventCounts ? countWeight : timeWeight;
        box.weight = view.totalWeight;
        box.expandedWeight = !view.eventCounts && timeWeight > double( box.row.inclusive ) + 0.5;
        if( view.totalNodes )
        {
            bytes += box.row.name.capacity() + box.row.function.capacity() + box.row.file.capacity() + 3;
            Require( bytes <= d.options.resultBytes, "Flame focus exceeds result budget" );
            view.boxes.push_back( std::move( box ) );
            view.depth = 1;
            if( options.depth > 1 ) pending.push_back( { options.focus, 0, 1, view.totalWeight, 1 } );
        }
    }
    else
    {
        auto q = d.Prep( "SELECT COALESCE(SUM(time_weight),0),COALESCE(SUM(CAST(subtree AS REAL)),0) FROM a_nodes WHERE parent=0" );
        Require( d.Step( q ), "Missing flame totals" );
        timeWeight = sqlite3_column_double( q, 0 );
        countWeight = sqlite3_column_double( q, 1 );
        sqlite3_reset( q );
        view.totalNodes = m_impl->info.nodes;
        view.eventCounts = options.eventCounts || timeWeight <= 0;
        view.totalWeight = view.eventCounts ? countWeight : timeWeight;
        if( view.totalNodes ) pending.push_back( { 0, 0, 1, view.totalWeight, 0 } );
    }
    Require( std::isfinite( view.totalWeight ) && view.totalWeight >= 0, "Invalid flame weight" );
    const std::string mode = view.eventCounts ? "count" : "time";
    bool full = false;
    for( size_t next = 0; next < pending.size() && !full; ++next )
    {
        const auto job = pending[next];
        if( job.weight <= 0 || job.depth >= options.depth ) continue;
        const double cutoff = job.weight / ( double( options.pixels ) * ( job.end - job.begin ) );
        auto q = d.Prep( columns + ",l." + mode + "_left,l." + mode + "_right," + ( view.eventCounts ? "CAST(n.subtree AS REAL)" : "n.time_weight" ) + " FROM a_layout l JOIN a_nodes n ON n.id=l.node JOIN a_sources s ON s.id=n.source WHERE l.parent=?1 AND l." + mode + "_width>=?2 ORDER BY l.rank LIMIT ?3" );
        Int( q, 1, job.parent );
        Require( sqlite3_bind_double( q, 2, cutoff ) == SQLITE_OK, "Flame cutoff binding failed" );
        Int( q, 3, capacity - view.boxes.size() + 1 );
        try
        {
            while( d.Step( q ) )
            {
                if( view.boxes.size() == capacity )
                {
                    full = true;
                    break;
                }
                StoreFlameBox box;
                box.row = AnalysisRow( q );
                box.depth = job.depth;
                box.weight = sqlite3_column_double( q, 23 );
                box.begin = std::clamp( job.begin + ( job.end - job.begin ) * sqlite3_column_double( q, 21 ) / job.weight, job.begin, job.end );
                box.end = std::clamp( job.begin + ( job.end - job.begin ) * sqlite3_column_double( q, 22 ) / job.weight, box.begin, job.end );
                box.expandedWeight = !view.eventCounts && box.weight > double( box.row.inclusive ) + 0.5;
                const auto extra = box.row.name.capacity() + box.row.function.capacity() + box.row.file.capacity() + 3;
                if( extra > d.options.resultBytes - bytes )
                {
                    Require( !view.boxes.empty(), "Flame node exceeds result budget" );
                    full = true;
                    break;
                }
                bytes += extra;
                view.depth = std::max( view.depth, box.depth + 1 );
                if( box.depth + 1 < options.depth && box.end > box.begin ) pending.push_back( { box.row.id, box.begin, box.end, box.weight, box.depth + 1 } );
                view.boxes.push_back( std::move( box ) );
            }
        }
        catch( ... )
        {
            sqlite3_reset( q );
            throw;
        }
        sqlite3_reset( q );
    }
    Require( view.boxes.size() <= view.totalNodes, "Flame node accounting mismatch" );
    view.omittedNodes = view.totalNodes - view.boxes.size();
    return view;
}
// Search of a call tree analysis: the zones are read from the store in id
// order with the same matches as the event table query. SQLite's lower() and
// instr() fold only ASCII letters and compare bytes, as these do.
StorePage<StoreAnalysisMatch> StoreAnalysis::ZoneSearch( const std::string& text, bool caseSensitive, uint64_t after, size_t limit, std::optional<uint64_t> source ) const
{
    auto& d = *m_impl->db;
    auto& z = *m_impl->zones->m_impl;
    const auto fold = [&]( std::string_view value ) {
        std::string out( value );
        if( !caseSensitive )
            for( auto& ch : out )
                if( ch >= 'A' && ch <= 'Z' ) ch = char( ch - 'A' + 'a' );
        return out;
    };
    const auto needle = fold( text );
    const auto contains = [&]( std::string_view value ) { return fold( value ).find( needle ) != std::string::npos; };
    // Canonical sources and the ones whose own name, function or file match.
    struct Source
    {
        std::string name, function, file;
        bool matched = false;
    };
    std::unordered_map<uint64_t, Source> sources;
    // Bounded previews as substr(value,1,2048) returns them, with capacity to match.
    const auto clip = []( const std::string& value ) {
        if( value.size() <= 2048 ) return value;
        size_t size = 2048;
        while( size > 0 && ( uint8_t( value[size] ) & 0xC0 ) == 0x80 ) --size;
        return std::string( value.data(), size );
    };
    AnalysisEach( d, "SELECT id,name,function,file FROM a_sources", [&]( auto row ) {
        Source s{ Col( row, 1 ), Col( row, 2 ), Col( row, 3 ) };
        s.matched = contains( s.name ) || contains( s.function ) || contains( s.file );
        sources.emplace( uint64_t( sqlite3_column_int64( row, 0 ) ), std::move( s ) );
    } );
    std::unordered_map<std::pair<uint8_t, uint64_t>, uint64_t, SourceKeyHash> canonical;
    AnalysisEach( d, "SELECT kind,skey,source FROM a_source_map", [&]( auto row ) { canonical.emplace( std::make_pair( uint8_t( sqlite3_column_int( row, 0 ) ), uint64_t( sqlite3_column_int64( row, 1 ) ) ), uint64_t( sqlite3_column_int64( row, 2 ) ) ); } );
    std::vector<std::pair<uint64_t, uint64_t>> blocks;
    {
        auto q = z.db.Prep( std::string( "SELECT id,first FROM blocks WHERE stream=1 AND last>?1" ) + ( m_impl->thread ? " AND key=?2" : "" ) + " ORDER BY first" );
        Int( q, 1, after );
        if( m_impl->thread ) Int( q, 2, *m_impl->thread );
        while( z.db.Step( q ) ) blocks.emplace_back( sqlite3_column_int64( q, 0 ), sqlite3_column_int64( q, 1 ) );
        sqlite3_reset( q );
    }
    const auto byId = []( const StoreAnalysisMatch& a, const StoreAnalysisMatch& b ) { return a.id < b.id; };
    std::vector<StoreAnalysisMatch> found;
    for( const auto& [block, first] : blocks )
    {
        if( found.size() > limit )
        {
            std::nth_element( found.begin(), found.begin() + limit, found.end(), byId );
            found.resize( limit + 1 );
            if( first > std::max_element( found.begin(), found.end(), byId )->id ) break;
        }
        const auto zones = LoadZoneBlock( z.db, z.blocks, z.zoneCache, block );
        if( zones->rows.empty() ) continue;
        const uint64_t lo = zones->rows.front().id, hi = zones->rows.back().id;
        // Zones of this block with a matching text fragment.
        std::unordered_set<uint64_t> textMatched;
        auto tq = z.db.Prep( "SELECT id FROM blocks WHERE stream=2 AND key=?1 AND first<=?3 AND last>=?2" );
        Int( tq, 1, zones->thread );
        Int( tq, 2, lo );
        Int( tq, 3, hi );
        std::vector<uint64_t> textBlocks;
        while( z.db.Step( tq ) ) textBlocks.push_back( sqlite3_column_int64( tq, 0 ) );
        sqlite3_reset( tq );
        for( const auto textBlock : textBlocks )
            for( const auto& t : LoadTextBlock( z.db, z.blocks, z.zoneCache, textBlock )->rows )
                if( t.zone >= lo && t.zone <= hi && !textMatched.count( t.zone ) && contains( t.text ) ) textMatched.insert( t.zone );
        tq = z.db.Prep( "SELECT zone,text FROM zone_late_text WHERE thread=?1 AND zone BETWEEN ?2 AND ?3" );
        Int( tq, 1, zones->thread );
        Int( tq, 2, lo );
        Int( tq, 3, hi );
        while( z.db.Step( tq ) )
            if( contains( Col( tq, 1 ) ) ) textMatched.insert( uint64_t( sqlite3_column_int64( tq, 0 ) ) );
        sqlite3_reset( tq );
        for( const auto& r : zones->rows )
        {
            if( r.id <= after ) continue;
            const auto c = canonical.find( { r.skind, r.skey } );
            Require( c != canonical.end(), "Missing canonical analysis source" );
            if( source && c->second != *source ) continue;
            const auto& s = sources.at( c->second );
            const bool textMatch = textMatched.count( r.id ) != 0;
            if( !( textMatch || s.matched || ( r.hasName && contains( r.name ) ) ) ) continue;
            StoreAnalysisMatch m;
            m.id = r.id;
            m.thread = zones->thread;
            m.source = c->second;
            m.start = r.start;
            if( !r.open ) m.end = r.end;
            m.name = clip( r.hasName ? r.name : s.name );
            m.function = clip( s.function );
            m.file = clip( s.file );
            m.textMatched = textMatch;
            found.push_back( std::move( m ) );
        }
    }
    std::sort( found.begin(), found.end(), byId );
    StorePage<StoreAnalysisMatch> page;
    const auto capacity = std::min<uint64_t>( limit, ( d.options.resultBytes - sizeof( page ) ) / ( 2 * sizeof( StoreAnalysisMatch ) ) );
    page.values.reserve( size_t( capacity ) );
    size_t bytes = sizeof( page ) + page.values.capacity() * sizeof( StoreAnalysisMatch );
    for( auto& row : found )
    {
        if( page.values.size() == capacity )
        {
            page.more = true;
            break;
        }
        const auto extraBytes = row.name.capacity() + row.function.capacity() + row.file.capacity() + 3;
        if( extraBytes > d.options.resultBytes - bytes )
        {
            Require( !page.values.empty(), "Search row exceeds result budget" );
            page.more = true;
            break;
        }
        bytes += extraBytes;
        page.next = row.id;
        page.values.push_back( std::move( row ) );
    }
    return page;
}
StorePage<StoreAnalysisMatch> StoreAnalysis::Search( const std::string& text, bool caseSensitive, uint64_t after, size_t limit, std::optional<uint64_t> source ) const
{
    Require( text.size() <= 4096 && limit > 0 && limit <= 4096, "Search input/result limit exceeded" );
    auto& d = *m_impl->db;
    if( m_impl->zones ) return ZoneSearch( text, caseSensitive, after, limit, source );
    const auto match = [&]( const std::string& value ) {
        return caseSensitive ? "instr(" + value + ",?1)>0" : "instr(lower(" + value + "),lower(?1))>0";
    };
    const std::string extra = "EXISTS(SELECT 1 FROM a_text t WHERE t.zone=a.id AND " + match( "t.text" ) + ")";
    const std::string name = "COALESCE(a.name,s.name)";
    const auto sql = "SELECT a.id,a.thread,a.context,a.source,a.begin,a.finish,substr(" + name + ",1,2048),substr(s.function,1,2048),substr(s.file,1,2048)," + extra + " FROM a_events a JOIN a_sources s ON s.id=a.source WHERE a.included AND a.id>?2 " + ( source ? "AND a.source=?4 " : "" ) + "AND (" + match( name ) + " OR " + match( "s.name" ) + " OR " + match( "s.function" ) + " OR " + match( "s.file" ) + " OR " + extra + ") ORDER BY a.id LIMIT ?3";
    auto q = d.Prep( sql );
    ::tracy::stream::Text( q, 1, text );
    Int( q, 2, after );
    Int( q, 3, limit + 1 );
    if( source ) Int( q, 4, *source );
    StorePage<StoreAnalysisMatch> page;
    const auto capacity = std::min<uint64_t>( limit, ( d.options.resultBytes - sizeof( page ) ) / ( 2 * sizeof( StoreAnalysisMatch ) ) );
    page.values.reserve( size_t( capacity ) );
    size_t bytes = sizeof( page ) + page.values.capacity() * sizeof( StoreAnalysisMatch );
    try
    {
        while( d.Step( q ) )
        {
            if( page.values.size() == capacity )
            {
                page.more = true;
                break;
            }
            StoreAnalysisMatch row;
            row.id = sqlite3_column_int64( q, 0 );
            row.thread = sqlite3_column_int64( q, 1 );
            row.context = sqlite3_column_int64( q, 2 );
            row.source = sqlite3_column_int64( q, 3 );
            if( sqlite3_column_type( q, 4 ) != SQLITE_NULL ) row.start = sqlite3_column_int64( q, 4 );
            if( sqlite3_column_type( q, 5 ) != SQLITE_NULL ) row.end = sqlite3_column_int64( q, 5 );
            row.name = Col( q, 6 );
            row.function = Col( q, 7 );
            row.file = Col( q, 8 );
            row.textMatched = sqlite3_column_int( q, 9 ) != 0;
            const auto extraBytes = row.name.capacity() + row.function.capacity() + row.file.capacity() + 3;
            if( extraBytes > d.options.resultBytes - bytes )
            {
                Require( !page.values.empty(), "Search row exceeds result budget" );
                page.more = true;
                break;
            }
            bytes += extraBytes;
            page.next = row.id;
            page.values.push_back( std::move( row ) );
        }
    }
    catch( ... )
    {
        sqlite3_reset( q );
        throw;
    }
    sqlite3_reset( q );
    return page;
}

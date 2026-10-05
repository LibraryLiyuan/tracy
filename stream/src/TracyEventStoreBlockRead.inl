// Read side of the event blocks. Decoded blocks of one connection are kept in
// a byte-budgeted LRU; late values from `zone_late` are merged when a block is
// loaded.
struct ZoneBlock
{
    uint64_t id = 0, thread = 0;
    std::vector<ZoneRow> rows;
};
struct TextBlock
{
    uint64_t id = 0, thread = 0;
    std::vector<TextRow> rows;
};
struct PacketBlock
{
    uint64_t id = 0;
    uint8_t type = 0;
    size_t size = 0;
    std::vector<PacketRow> rows;
    std::vector<uint8_t> payloads;
    std::span<const uint8_t> Payload( size_t n ) const { return { payloads.data() + n * size, size }; }
};
template<class T>
struct BlockLru
{
    struct Entry
    {
        uint64_t block = 0, used = 0, bytes = 0;
        std::shared_ptr<const T> value;
    };
    std::vector<Entry> entries;
    uint64_t clock = 0, bytes = 0, budget = 8ull << 20, hits = 0, misses = 0;
    std::shared_ptr<const T> Find( uint64_t block )
    {
        for( auto& e : entries )
            if( e.block == block )
            {
                e.used = ++clock;
                ++hits;
                return e.value;
            }
        ++misses;
        return {};
    }
    bool Holds( uint64_t block ) const
    {
        return std::any_of( entries.begin(), entries.end(), [block]( const Entry& e ) { return e.block == block; } );
    }
    // At least two blocks stay, so a walk alternating between two blocks does
    // not reload them; the others fit the byte budget.
    void Put( uint64_t block, std::shared_ptr<const T> value, uint64_t size )
    {
        while( entries.size() >= 2 && bytes + size > budget )
        {
            const auto victim = std::min_element( entries.begin(), entries.end(), []( const Entry& a, const Entry& b ) { return a.used < b.used; } );
            bytes -= victim->bytes;
            if( victim != entries.end() - 1 ) *victim = std::move( entries.back() );
            entries.pop_back();
        }
        entries.push_back( { block, ++clock, size, std::move( value ) } );
        bytes += size;
    }
};
struct ZoneBlockCache
{
    BlockLru<ZoneBlock> zones;
    BlockLru<TextBlock> texts;
    BlockLru<PacketBlock> packets;
    // Logical threads owning zone blocks, loaded by the first id lookup.
    std::optional<std::vector<uint64_t>> threads;
};
BlockLocation LocateBlock( Db& d, uint64_t block, BlockStream stream, uint64_t& key, uint64_t& first, uint64_t& last )
{
    auto q = d.Prep( "SELECT key,first,last,offset,size,raw,checksum FROM blocks WHERE id=? AND stream=?" );
    Int( q, 1, block );
    Int( q, 2, uint64_t( stream ) );
    Require( d.Step( q ), "Missing event block" );
    key = sqlite3_column_int64( q, 0 );
    first = sqlite3_column_int64( q, 1 );
    last = sqlite3_column_int64( q, 2 );
    BlockLocation b{ block, uint64_t( sqlite3_column_int64( q, 3 ) ), uint64_t( sqlite3_column_int64( q, 4 ) ), uint64_t( sqlite3_column_int64( q, 5 ) ), uint64_t( sqlite3_column_int64( q, 6 ) ) };
    sqlite3_reset( q );
    return b;
}
std::shared_ptr<const ZoneBlock> LoadZoneBlock( Db& d, const BlockFile& file, ZoneBlockCache& cache, uint64_t block )
{
    if( auto found = cache.zones.Find( block ) ) return found;
    auto result = std::make_shared<ZoneBlock>();
    uint64_t first, last;
    const auto location = LocateBlock( d, block, BlockStream::CpuZones, result->thread, first, last );
    result->id = block;
    DecodeZones( ReadBlock( file, location ), result->rows );
    Require( !result->rows.empty() && result->rows.front().id == first && result->rows.back().id == last, "Event block range mismatch" );
    auto q = d.Prep( "SELECT id,end,children,name,color,child_time FROM zone_late WHERE id BETWEEN ?1 AND ?2 AND thread=?3" );
    Int( q, 1, first );
    Int( q, 2, last );
    Int( q, 3, result->thread );
    while( d.Step( q ) )
    {
        const uint64_t id = sqlite3_column_int64( q, 0 );
        auto row = std::lower_bound( result->rows.begin(), result->rows.end(), id, []( const ZoneRow& r, uint64_t v ) { return r.id < v; } );
        if( row == result->rows.end() || row->id != id ) continue;
        Require( row->open, "Late values for a closed CPU zone" );
        if( sqlite3_column_type( q, 1 ) != SQLITE_NULL )
        {
            row->open = false;
            row->end = sqlite3_column_int64( q, 1 );
        }
        row->children = uint32_t( sqlite3_column_int64( q, 2 ) );
        row->childTime = sqlite3_column_int64( q, 5 );
        if( sqlite3_column_type( q, 3 ) != SQLITE_NULL )
        {
            row->hasName = true;
            row->name = Col( q, 3 );
        }
        if( sqlite3_column_type( q, 4 ) != SQLITE_NULL )
        {
            row->hasColor = true;
            row->color = uint32_t( sqlite3_column_int64( q, 4 ) );
        }
    }
    sqlite3_reset( q );
    uint64_t size = sizeof( ZoneBlock ) + result->rows.capacity() * sizeof( ZoneRow );
    for( const auto& r : result->rows ) size += r.name.size();
    std::shared_ptr<const ZoneBlock> value = std::move( result );
    cache.zones.Put( block, value, size );
    return value;
}
std::shared_ptr<const TextBlock> LoadTextBlock( Db& d, const BlockFile& file, ZoneBlockCache& cache, uint64_t block )
{
    if( auto found = cache.texts.Find( block ) ) return found;
    auto result = std::make_shared<TextBlock>();
    uint64_t first, last;
    const auto location = LocateBlock( d, block, BlockStream::ZoneText, result->thread, first, last );
    result->id = block;
    DecodeTexts( ReadBlock( file, location ), result->rows );
    uint64_t size = sizeof( TextBlock ) + result->rows.capacity() * sizeof( TextRow );
    for( const auto& r : result->rows ) size += r.text.size();
    std::shared_ptr<const TextBlock> value = std::move( result );
    cache.texts.Put( block, value, size );
    return value;
}
std::shared_ptr<const PacketBlock> LoadPacketBlock( Db& d, const BlockFile& file, ZoneBlockCache& cache, uint64_t block )
{
    if( auto found = cache.packets.Find( block ) ) return found;
    auto result = std::make_shared<PacketBlock>();
    uint64_t key, first, last;
    const auto location = LocateBlock( d, block, BlockStream::Packets, key, first, last );
    result->id = block;
    result->type = uint8_t( key );
    DecodePackets( ReadBlock( file, location ), result->rows, result->payloads, result->size );
    Require( !result->rows.empty() && result->rows.front().id == first && result->rows.back().id == last && key < 256 && result->size == size_t( QueueDataSize[key] - 1 ), "Packet block range mismatch" );
    const uint64_t size = sizeof( PacketBlock ) + result->rows.capacity() * sizeof( PacketRow ) + result->payloads.capacity();
    std::shared_ptr<const PacketBlock> value = std::move( result );
    cache.packets.Put( block, value, size );
    return value;
}
// Visits the packets of one type with ids in [from,to] in id order until the
// visitor returns false. The blocks of one type hold disjoint, increasing id
// ranges, so the walk starts at the block holding `from`.
template<class F>
void ForPackets( Db& d, const BlockFile& file, ZoneBlockCache& cache, uint8_t type, uint64_t from, I to, F&& visit )
{
    auto q = d.Prep( "SELECT id FROM blocks WHERE stream=3 AND key=?1 AND first>=COALESCE((SELECT first FROM blocks WHERE stream=3 AND key=?1 AND first<=?2 ORDER BY first DESC LIMIT 1),0) AND first<=?3 ORDER BY first" );
    Int( q, 1, type );
    Int( q, 2, from );
    Int( q, 3, uint64_t( to ) );
    try
    {
        while( d.Step( q ) )
        {
            const auto packets = LoadPacketBlock( d, file, cache, sqlite3_column_int64( q, 0 ) );
            for( size_t n = 0; n < packets->rows.size(); ++n )
            {
                const auto id = packets->rows[n].id;
                if( id < from ) continue;
                if( I( id ) > to || !visit( *packets, n ) )
                {
                    sqlite3_reset( q );
                    return;
                }
            }
        }
    }
    catch( ... )
    {
        sqlite3_reset( q );
        throw;
    }
    sqlite3_reset( q );
}
struct PacketRef
{
    std::shared_ptr<const PacketBlock> block;
    size_t index = 0;
};
std::optional<PacketRef> FindPacket( Db& d, const BlockFile& file, ZoneBlockCache& cache, uint8_t type, uint64_t id )
{
    auto q = d.Prep( "SELECT id FROM blocks WHERE stream=3 AND key=? AND first<=? ORDER BY first DESC LIMIT 1" );
    Int( q, 1, type );
    Int( q, 2, id );
    uint64_t block = 0;
    if( d.Step( q ) ) block = sqlite3_column_int64( q, 0 );
    sqlite3_reset( q );
    if( !block ) return {};
    auto packets = LoadPacketBlock( d, file, cache, block );
    const auto row = std::lower_bound( packets->rows.begin(), packets->rows.end(), id, []( const PacketRow& r, uint64_t v ) { return r.id < v; } );
    if( row == packets->rows.end() || row->id != id ) return {};
    const auto index = size_t( row - packets->rows.begin() );
    return PacketRef{ std::move( packets ), index };
}
// Whether a zone event block holds `id`, decoding only its id column.
bool ProbeZoneBlock( const BlockFile& file, const BlockLocation& location, uint64_t id )
{
    const auto raw = ReadBlock( file, location );
    const uint8_t* p = raw.data();
    const uint8_t* e = p + raw.size();
    const auto count = GetVar( p, e );
    const auto sources = GetVar( p, e );
    for( uint64_t n = 0; n < sources; ++n )
    {
        Require( p < e, "Corrupt zone block sources" );
        ++p;
        GetVar( p, e );
    }
    uint64_t current = 0;
    for( uint64_t n = 0; n < count; ++n )
    {
        current += GetVar( p, e );
        if( current >= id ) return current == id;
    }
    return false;
}
// Zone overlap rule shared by the window APIs: open zones extend to the end
// of the capture, and zero-length zones count at their start.
bool ZoneOverlaps( const ZoneRow& r, I begin, I end )
{
    return r.start < end && ( r.open || r.end > begin || ( r.end == r.start && r.start >= begin ) );
}
// Zone event blocks overlapping [begin,end), optionally of one thread and
// holding ids above `after`, in ascending order of their first id.
std::vector<uint64_t> ZoneBlocks( Db& d, I begin, I end, std::optional<uint64_t> thread, uint64_t after )
{
    auto q = d.Prep( std::string( "SELECT b.id FROM blocks b WHERE b.stream=1 AND b.id IN(SELECT id FROM block_range WHERE mint<=?1 AND maxt>=?2) AND b.tmin<?3 AND b.tmax>=?4 AND b.last>?5" ) + ( thread ? " AND b.key=?6" : "" ) + " ORDER BY b.first" );
    Int( q, 1, uint64_t( Bucket( end - 1 ) ) );
    Int( q, 2, uint64_t( Bucket( begin ) ) );
    Int( q, 3, uint64_t( end ) );
    Int( q, 4, uint64_t( begin ) );
    Int( q, 5, after );
    if( thread ) Int( q, 6, *thread );
    std::vector<uint64_t> blocks;
    while( d.Step( q ) ) blocks.push_back( sqlite3_column_int64( q, 0 ) );
    sqlite3_reset( q );
    return blocks;
}
// One CPU zone with its logical thread.
struct ZoneRef
{
    uint64_t thread = 0;
    ZoneRow row;
};
// Finds one CPU zone by id. Decoded blocks are searched first. Otherwise the
// block of each thread whose id range covers `id` is probed through its id
// column, and only the block holding the zone is decoded. The blocks of one
// thread hold disjoint id ranges, so each thread has at most one candidate.
std::optional<ZoneRef> FindZone( Db& d, const BlockFile& file, ZoneBlockCache& cache, uint64_t id )
{
    const auto search = [id]( const ZoneBlock& zones ) -> const ZoneRow* {
        if( zones.rows.empty() || zones.rows.front().id > id || zones.rows.back().id < id ) return nullptr;
        const auto row = std::lower_bound( zones.rows.begin(), zones.rows.end(), id, []( const ZoneRow& r, uint64_t v ) { return r.id < v; } );
        return row != zones.rows.end() && row->id == id ? &*row : nullptr;
    };
    for( auto& e : cache.zones.entries )
        if( const auto row = search( *e.value ) )
        {
            e.used = ++cache.zones.clock;
            ++cache.zones.hits;
            return ZoneRef{ e.value->thread, *row };
        }
    if( !cache.threads )
    {
        std::vector<uint64_t> threads;
        auto q = d.Prep( "SELECT DISTINCT key FROM blocks WHERE stream=1" );
        while( d.Step( q ) ) threads.push_back( sqlite3_column_int64( q, 0 ) );
        sqlite3_reset( q );
        cache.threads = std::move( threads );
    }
    for( const auto thread : *cache.threads )
    {
        auto q = d.Prep( "SELECT id,last FROM blocks WHERE stream=1 AND key=?1 AND first<=?2 ORDER BY first DESC LIMIT 1" );
        Int( q, 1, thread );
        Int( q, 2, id );
        uint64_t block = 0;
        if( d.Step( q ) && uint64_t( sqlite3_column_int64( q, 1 ) ) >= id ) block = sqlite3_column_int64( q, 0 );
        sqlite3_reset( q );
        if( !block || cache.zones.Holds( block ) ) continue;
        uint64_t key, first, last;
        ++cache.zones.misses;
        if( !ProbeZoneBlock( file, LocateBlock( d, block, BlockStream::CpuZones, key, first, last ), id ) ) continue;
        const auto zones = LoadZoneBlock( d, file, cache, block );
        if( const auto row = search( *zones ) ) return ZoneRef{ zones->thread, *row };
    }
    return {};
}
// Text fragments of one zone in event order, from its thread's text blocks and
// the fragments that arrived out of order. A thread's text blocks are in zone
// order, and consecutive blocks share at most their boundary zone.
std::vector<StoreText> ZoneTextRows( Db& d, const BlockFile& file, ZoneBlockCache& cache, uint64_t thread, uint64_t zone, uint64_t after, size_t limit )
{
    std::vector<StoreText> rows;
    std::vector<uint64_t> blocks;
    auto q = d.Prep( "SELECT id,last FROM blocks WHERE stream=2 AND key=?1 AND first<=?2 ORDER BY first DESC" );
    Int( q, 1, thread );
    Int( q, 2, zone );
    while( d.Step( q ) && uint64_t( sqlite3_column_int64( q, 1 ) ) >= zone ) blocks.push_back( sqlite3_column_int64( q, 0 ) );
    sqlite3_reset( q );
    for( auto block = blocks.rbegin(); block != blocks.rend(); ++block )
    {
        const auto texts = LoadTextBlock( d, file, cache, *block );
        auto row = std::lower_bound( texts->rows.begin(), texts->rows.end(), zone, []( const TextRow& r, uint64_t v ) { return r.zone < v; } );
        for( ; row != texts->rows.end() && row->zone == zone; ++row )
            if( row->event > after ) rows.push_back( { row->event, row->text } );
    }
    q = d.Prep( "SELECT event,text FROM zone_late_text WHERE zone=? AND event>? ORDER BY event LIMIT ?" );
    Int( q, 1, zone );
    Int( q, 2, after );
    Int( q, 3, limit );
    while( d.Step( q ) ) rows.push_back( { uint64_t( sqlite3_column_int64( q, 0 ) ), Col( q, 1 ) } );
    sqlite3_reset( q );
    std::sort( rows.begin(), rows.end(), []( const StoreText& a, const StoreText& b ) { return a.event < b.event; } );
    if( rows.size() > limit ) rows.resize( limit );
    return rows;
}
// Copies zones into temp.zone_page, whose columns match the former `zones`
// table, so the shared column SQL resolves names, sources, colors and
// callsites exactly as for row-stored zones.
void StageZones( Db& d, const std::vector<ZoneRef>& zones )
{
    d.Exec( "CREATE TEMP TABLE IF NOT EXISTS zone_page(id INTEGER PRIMARY KEY,thread INTEGER,parent INTEGER,depth INTEGER,start INTEGER,end INTEGER,skind INTEGER,skey INTEGER,stack INTEGER,callsite INTEGER,validation INTEGER,name TEXT,color INTEGER,child_time INTEGER);DELETE FROM temp.zone_page;" );
    for( const auto& z : zones )
    {
        auto q = d.Prep( "INSERT INTO temp.zone_page VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)" );
        const auto& r = z.row;
        Int( q, 1, r.id );
        Int( q, 2, z.thread );
        Int( q, 3, r.parent );
        Int( q, 4, r.depth );
        Int( q, 5, uint64_t( r.start ) );
        if( !r.open ) Int( q, 6, uint64_t( r.end ) );
        Int( q, 7, r.skind );
        Int( q, 8, r.skey );
        Int( q, 9, r.stack );
        Int( q, 10, r.callsite );
        Int( q, 11, r.validation );
        if( r.hasName ) Text( q, 12, r.name );
        if( r.hasColor ) Int( q, 13, r.color );
        Int( q, 14, uint64_t( r.childTime ) );
        d.Run( q );
    }
}
// Zone and text blocks of every logical thread, ordered by first id. Built
// once per store from the block index; it is bounded by the block count.
struct ZoneThreadIndex
{
    struct Block
    {
        uint64_t id = 0, first = 0, last = 0, count = 0, before = 0;
        // Zone blocks: earliest start, latest closed end, and whether a zone
        // of the block never closed.
        int64_t tmin = 0, tmax = 0;
        bool unfinished = false;
    };
    std::vector<uint64_t> threads;
    std::vector<std::vector<Block>> zones, texts;
    std::vector<uint64_t> totals;
    size_t Find( uint64_t thread ) const
    {
        const auto it = std::lower_bound( threads.begin(), threads.end(), thread );
        return it != threads.end() && *it == thread ? size_t( it - threads.begin() ) : SIZE_MAX;
    }
};
ZoneThreadIndex BuildZoneThreadIndex( Db& d )
{
    ZoneThreadIndex index;
    std::unordered_map<uint64_t, size_t> position;
    auto q = d.Prep( "SELECT key,id,first,last,count,tmin,tmax,unfinished FROM blocks WHERE stream=1 ORDER BY key,first" );
    while( d.Step( q ) )
    {
        const uint64_t thread = sqlite3_column_int64( q, 0 );
        if( index.threads.empty() || index.threads.back() != thread )
        {
            position.emplace( thread, index.threads.size() );
            index.threads.push_back( thread );
            index.zones.emplace_back();
            index.texts.emplace_back();
            index.totals.push_back( 0 );
        }
        auto& total = index.totals.back();
        const uint64_t count = sqlite3_column_int64( q, 4 );
        index.zones.back().push_back( { uint64_t( sqlite3_column_int64( q, 1 ) ), uint64_t( sqlite3_column_int64( q, 2 ) ), uint64_t( sqlite3_column_int64( q, 3 ) ), count, total,
            sqlite3_column_int64( q, 5 ), sqlite3_column_int64( q, 6 ), sqlite3_column_int64( q, 7 ) > 0 } );
        total += count;
    }
    sqlite3_reset( q );
    q = d.Prep( "SELECT key,id,first,last FROM blocks WHERE stream=2 ORDER BY key,first" );
    while( d.Step( q ) )
    {
        const auto found = position.find( uint64_t( sqlite3_column_int64( q, 0 ) ) );
        if( found == position.end() ) continue;
        index.texts[found->second].push_back( { uint64_t( sqlite3_column_int64( q, 1 ) ), uint64_t( sqlite3_column_int64( q, 2 ) ), uint64_t( sqlite3_column_int64( q, 3 ) ), 0, 0 } );
    }
    sqlite3_reset( q );
    return index;
}
// GPU zone timelines in snapshot order (context sequence, then thread), with
// the id of every SampleStride-th zone of each for ordinal seeks.
struct GpuGroupIndex
{
    static constexpr uint64_t SampleStride = 4096;
    struct Group
    {
        uint64_t context = 0, thread = 0, zones = 0, completed = 0;
        std::vector<uint64_t> samples;
    };
    std::vector<Group> groups;
};
GpuGroupIndex BuildGpuGroupIndex( Db& d )
{
    std::map<std::pair<uint64_t, uint64_t>, GpuGroupIndex::Group> groups;
    auto q = d.Prep( "SELECT context,thread,id,gpu_end IS NOT NULL FROM gpu_zones ORDER BY id" );
    while( d.Step( q ) )
    {
        const uint64_t context = sqlite3_column_int64( q, 0 ), thread = sqlite3_column_int64( q, 1 );
        auto& group = groups[{ context, thread }];
        if( group.zones % GpuGroupIndex::SampleStride == 0 ) group.samples.push_back( uint64_t( sqlite3_column_int64( q, 2 ) ) );
        ++group.zones;
        if( sqlite3_column_int( q, 3 ) ) ++group.completed;
    }
    sqlite3_reset( q );
    std::map<uint64_t, int64_t> sequence;
    q = d.Prep( "SELECT id,sequence FROM gpu_contexts" );
    while( d.Step( q ) ) sequence.emplace( uint64_t( sqlite3_column_int64( q, 0 ) ), sqlite3_column_int64( q, 1 ) );
    sqlite3_reset( q );
    GpuGroupIndex index;
    for( auto& [key, group] : groups )
    {
        group.context = key.first;
        group.thread = key.second;
        index.groups.push_back( std::move( group ) );
    }
    std::stable_sort( index.groups.begin(), index.groups.end(), [&]( const auto& a, const auto& b ) {
        const auto sa = sequence.find( a.context ), sb = sequence.find( b.context );
        const auto ka = sa == sequence.end() ? INT64_MAX : sa->second, kb = sb == sequence.end() ? INT64_MAX : sb->second;
        return ka != kb ? ka < kb : a.thread < b.thread;
    } );
    return index;
}
// Packet blocks of one export type in id order with the number of packets in
// the blocks before each, for ordinal seeks.
struct PacketTypeIndex
{
    struct Block
    {
        uint64_t id = 0, count = 0, before = 0;
    };
    std::vector<Block> blocks;
    uint64_t total = 0;
};
PacketTypeIndex BuildPacketTypeIndex( Db& d, uint8_t type )
{
    PacketTypeIndex index;
    auto q = d.Prep( "SELECT id,count FROM blocks WHERE stream=3 AND key=? ORDER BY first" );
    Int( q, 1, type );
    while( d.Step( q ) )
    {
        const uint64_t count = sqlite3_column_int64( q, 1 );
        index.blocks.push_back( { uint64_t( sqlite3_column_int64( q, 0 ) ), count, index.total } );
        index.total += count;
    }
    sqlite3_reset( q );
    return index;
}

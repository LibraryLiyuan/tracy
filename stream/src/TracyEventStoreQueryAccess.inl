// Sequential read access for analysis tools that number events the way Tracy's
// Worker numbers a standard snapshot of this store: CPU zones by logical
// thread in ascending id order, each thread in preorder.
namespace
{
// Appends one text fragment the way a standard snapshot stores zone text:
// fragments in event order joined by newlines, each cut at its first NUL.
void AppendZoneText( std::string& text, bool& any, std::string_view fragment )
{
    if( any ) text += '\n';
    any = true;
    text.append( fragment.substr( 0, fragment.find( '\0' ) ) );
}
}
struct StoreZoneCursor::Impl
{
    Db& d;
    const BlockFile& file;
    ZoneBlockCache& cache;
    const ZoneThreadIndex& index;
    size_t thread = SIZE_MAX, block = 0, row = 0;
    uint64_t ordinal = 0;
    std::shared_ptr<const ZoneBlock> zones;
    // Text blocks of the thread: the current one and the next row in it.
    size_t textBlock = 0, textRow = 0;
    std::shared_ptr<const TextBlock> texts;
    // Fragments of the current zone block that arrived out of order.
    struct Late
    {
        uint64_t zone = 0, event = 0;
        std::string text;
    };
    std::vector<Late> late;
    size_t lateRow = 0;
    std::vector<std::pair<uint64_t, std::string_view>> parts;
    bool withTexts = true;
    Impl( Db& db, const BlockFile& f, ZoneBlockCache& c, const ZoneThreadIndex& i, bool texts )
        : d( db )
        , file( f )
        , cache( c )
        , index( i )
        , withTexts( texts )
    {
    }
    void LoadBlock()
    {
        const auto& b = index.zones[thread][block];
        zones = LoadZoneBlock( d, file, cache, b.id );
        Require( zones->rows.size() == b.count, "Zone block count mismatch" );
        late.clear();
        lateRow = 0;
        if( !withTexts ) return;
        auto q = d.Prep( "SELECT zone,event,text FROM zone_late_text WHERE zone BETWEEN ?1 AND ?2 AND thread=?3 ORDER BY zone,event" );
        Int( q, 1, b.first );
        Int( q, 2, b.last );
        Int( q, 3, index.threads[thread] );
        while( d.Step( q ) )
        {
            const auto bytes = sqlite3_column_bytes( q, 2 );
            late.push_back( { uint64_t( sqlite3_column_int64( q, 0 ) ), uint64_t( sqlite3_column_int64( q, 1 ) ), std::string( reinterpret_cast<const char*>( sqlite3_column_blob( q, 2 ) ), size_t( bytes ) ) } );
        }
        sqlite3_reset( q );
    }
    // Positions the text cursor at the first fragment of a zone >= `zone`.
    void SeekTexts( uint64_t zone )
    {
        const auto& blocks = index.texts[thread];
        textBlock = size_t( std::lower_bound( blocks.begin(), blocks.end(), zone, []( const ZoneThreadIndex::Block& b, uint64_t v ) { return b.last < v; } ) - blocks.begin() );
        texts.reset();
        textRow = 0;
        if( textBlock == blocks.size() ) return;
        texts = LoadTextBlock( d, file, cache, blocks[textBlock].id );
        textRow = size_t( std::lower_bound( texts->rows.begin(), texts->rows.end(), zone, []( const TextRow& r, uint64_t v ) { return r.zone < v; } ) - texts->rows.begin() );
    }
    // The text fragment at the cursor, crossing into later text blocks.
    const TextRow* Text()
    {
        const auto& blocks = index.texts[thread];
        while( texts && textRow == texts->rows.size() )
        {
            texts.reset();
            textRow = 0;
            if( ++textBlock < blocks.size() ) texts = LoadTextBlock( d, file, cache, blocks[textBlock].id );
        }
        return texts ? &texts->rows[textRow] : nullptr;
    }
    void Seek( uint64_t threadId, uint64_t position )
    {
        thread = index.Find( threadId );
        Require( thread != SIZE_MAX, "Thread has no CPU zones" );
        Require( position <= index.totals[thread], "CPU zone ordinal out of range" );
        ordinal = position;
        zones.reset();
        const auto& blocks = index.zones[thread];
        block = size_t( std::upper_bound( blocks.begin(), blocks.end(), position, []( uint64_t v, const ZoneThreadIndex::Block& b ) { return v < b.before; } ) - blocks.begin() ) - 1;
        if( position == index.totals[thread] )
        {
            block = blocks.size();
            return;
        }
        LoadBlock();
        row = size_t( position - blocks[block].before );
        if( !withTexts ) return;
        SeekTexts( zones->rows[row].id );
        while( lateRow < late.size() && late[lateRow].zone < zones->rows[row].id ) ++lateRow;
    }
    bool Next( StoreZoneRecord& z )
    {
        if( thread == SIZE_MAX ) return false;
        const auto& blocks = index.zones[thread];
        if( !zones || row == zones->rows.size() )
        {
            if( zones ) ++block;
            if( block >= blocks.size() ) return false;
            LoadBlock();
            row = 0;
            if( withTexts && !texts ) SeekTexts( zones->rows[0].id );
        }
        const auto& r = zones->rows[row++];
        ++ordinal;
        z.id = r.id;
        z.parent = r.parent;
        z.stack = r.stack;
        z.callsite = r.callsite;
        z.validation = r.validation;
        z.skind = r.skind;
        z.skey = r.skey;
        z.start = r.start;
        z.end = r.end;
        z.open = r.open;
        z.childTime = r.childTime;
        z.depth = r.depth;
        z.children = r.children;
        z.hasColor = r.hasColor;
        z.color = r.color;
        z.hasName = r.hasName;
        if( r.hasName ) z.name = r.name;
        else z.name.clear();
        z.text.clear();
        z.hasText = false;
        if( !withTexts ) return true;
        // Fragments from the text blocks and the out-of-order table, in
        // event order.
        parts.clear();
        for( auto t = Text(); t && t->zone <= r.id; t = Text() )
        {
            if( t->zone == r.id ) parts.emplace_back( t->event, t->text );
            ++textRow;
        }
        while( lateRow < late.size() && late[lateRow].zone < r.id ) ++lateRow;
        bool merged = false;
        for( ; lateRow < late.size() && late[lateRow].zone == r.id; ++lateRow )
        {
            parts.emplace_back( late[lateRow].event, late[lateRow].text );
            merged = true;
        }
        if( merged ) std::sort( parts.begin(), parts.end(), []( const auto& a, const auto& b ) { return a.first < b.first; } );
        for( const auto& [event, fragment] : parts ) AppendZoneText( z.text, z.hasText, fragment );
        return true;
    }
};
StoreZoneCursor::StoreZoneCursor( std::unique_ptr<Impl> impl )
    : m_impl( std::move( impl ) )
{
}
StoreZoneCursor::~StoreZoneCursor() = default;
void StoreZoneCursor::Seek( uint64_t thread, uint64_t ordinal )
{
    m_impl->Seek( thread, ordinal );
}
bool StoreZoneCursor::Next( StoreZoneRecord& zone )
{
    return m_impl->Next( zone );
}
uint64_t StoreZoneCursor::Ordinal() const
{
    return m_impl->ordinal;
}
const ZoneThreadIndex& EventStoreZoneIndex( EventStore::Impl& impl )
{
    if( !impl.zoneIndex ) impl.zoneIndex = BuildZoneThreadIndex( impl.db );
    return *impl.zoneIndex;
}
std::vector<StoreZoneThread> EventStore::ZoneThreads() const
{
    const auto& index = EventStoreZoneIndex( *m_impl );
    std::vector<StoreZoneThread> threads( index.threads.size() );
    for( size_t n = 0; n < threads.size(); ++n ) threads[n] = { index.threads[n], index.totals[n] };
    return threads;
}
std::unique_ptr<StoreZoneCursor> EventStore::ZoneCursor( bool texts ) const
{
    return std::unique_ptr<StoreZoneCursor>( new StoreZoneCursor( std::make_unique<StoreZoneCursor::Impl>( m_impl->db, m_impl->blocks, m_impl->zoneCache, EventStoreZoneIndex( *m_impl ), texts ) ) );
}
std::optional<uint64_t> EventStore::ZoneOrdinal( uint64_t thread, uint64_t id ) const
{
    const auto& index = EventStoreZoneIndex( *m_impl );
    const auto position = index.Find( thread );
    if( position == SIZE_MAX ) return {};
    const auto& blocks = index.zones[position];
    const auto block = std::lower_bound( blocks.begin(), blocks.end(), id, []( const ZoneThreadIndex::Block& b, uint64_t v ) { return b.last < v; } );
    if( block == blocks.end() || block->first > id ) return {};
    const auto rows = LoadZoneBlock( m_impl->db, m_impl->blocks, m_impl->zoneCache, block->id );
    const auto row = std::lower_bound( rows->rows.begin(), rows->rows.end(), id, []( const ZoneRow& r, uint64_t v ) { return r.id < v; } );
    if( row == rows->rows.end() || row->id != id ) return {};
    return block->before + uint64_t( row - rows->rows.begin() );
}
// The exporter numbers static sources 1..N in key order and gives each
// distinct dynamic source (name, function, file, line, color) the next
// negative number, strings compared up to their first NUL as the snapshot
// dictionary stores them.
std::vector<StoreSourceNumber> EventStore::SourceNumbers() const
{
    auto& d = m_impl->db;
    std::vector<StoreSourceNumber> result;
    struct DynamicKey
    {
        bool named = false;
        std::string name, function, file;
        int64_t line = 0, color = 0;
        bool operator==( const DynamicKey& ) const = default;
    };
    struct DynamicKeyHash
    {
        size_t operator()( const DynamicKey& k ) const
        {
            const std::hash<std::string> h;
            return h( k.name ) ^ ( h( k.function ) * 31 ) ^ ( h( k.file ) * 131 ) ^ size_t( k.line * 1009 ) ^ size_t( k.color * 9176 ) ^ ( k.named ? 1 : 0 );
        }
    };
    std::unordered_map<DynamicKey, int16_t, DynamicKeyHash> dynamic;
    int64_t staticSources = 0, dynamicSources = 0;
    const auto cut = []( std::string value ) {
        const auto nul = value.find( '\0' );
        if( nul != std::string::npos ) value.resize( nul );
        return value;
    };
    auto q = d.Prep( "SELECT kind,key,line,color,name,function,file FROM sources ORDER BY kind,key" );
    while( d.Step( q ) )
    {
        StoreSourceNumber number;
        number.kind = uint8_t( sqlite3_column_int( q, 0 ) );
        number.key = sqlite3_column_int64( q, 1 );
        if( number.kind == 0 )
        {
            Require( staticSources < INT16_MAX, "Source location count exceeds standard snapshot limits" );
            number.number = int16_t( ++staticSources );
        }
        else
        {
            DynamicKey key;
            const auto name = Col( q, 4 );
            key.named = !name.empty();
            key.name = cut( name );
            key.function = cut( Col( q, 5 ) );
            key.file = cut( Col( q, 6 ) );
            key.line = sqlite3_column_int64( q, 2 );
            key.color = sqlite3_column_int64( q, 3 );
            auto found = dynamic.find( key );
            if( found == dynamic.end() )
            {
                Require( dynamicSources < INT16_MAX, "Source location count exceeds standard snapshot limits" );
                found = dynamic.emplace( std::move( key ), int16_t( -dynamicSources - 1 ) ).first;
                ++dynamicSources;
            }
            number.number = found->second;
        }
        result.push_back( number );
    }
    sqlite3_reset( q );
    return result;
}
std::vector<std::pair<uint64_t, uint64_t>> EventStore::CallsiteStacks() const
{
    auto& d = m_impl->db;
    std::vector<std::pair<uint64_t, uint64_t>> result;
    auto q = d.Prep( "SELECT id,stack FROM callsites WHERE stack!=0 ORDER BY id" );
    while( d.Step( q ) ) result.emplace_back( uint64_t( sqlite3_column_int64( q, 0 ) ), uint64_t( sqlite3_column_int64( q, 1 ) ) );
    sqlite3_reset( q );
    return result;
}
std::vector<uint64_t> EventStore::StackIds() const
{
    auto& d = m_impl->db;
    std::vector<uint64_t> result;
    auto q = d.Prep( "SELECT id FROM stacks ORDER BY id" );
    while( d.Step( q ) ) result.push_back( uint64_t( sqlite3_column_int64( q, 0 ) ) );
    sqlite3_reset( q );
    return result;
}
const GpuGroupIndex& EventStoreGpuIndex( EventStore::Impl& impl )
{
    if( !impl.gpuIndex ) impl.gpuIndex = BuildGpuGroupIndex( impl.db );
    return *impl.gpuIndex;
}
struct StoreGpuZoneCursor::Impl
{
    Db& d;
    const GpuGroupIndex& index;
    int64_t lastTime = 0;
    size_t group = SIZE_MAX, row = 0;
    uint64_t ordinal = 0, after = 0;
    bool exhausted = true;
    std::vector<StoreGpuZoneRecord> rows;
    Impl( Db& db, const GpuGroupIndex& i, int64_t last )
        : d( db )
        , index( i )
        , lastTime( last )
    {
    }
    static constexpr const char* Columns = "id,parent,depth,cpu_start,cpu_end,gpu_start,gpu_end,skind,skey,stack,callsite,query";
    static void Read( sqlite3_stmt* q, StoreGpuZoneRecord& z )
    {
        const auto value = [&]( int n ) { return sqlite3_column_type( q, n ) == SQLITE_NULL ? int64_t( -1 ) : int64_t( sqlite3_column_int64( q, n ) ); };
        z = StoreGpuZoneRecord{};
        z.id = sqlite3_column_int64( q, 0 );
        z.parent = sqlite3_column_int64( q, 1 );
        z.depth = uint32_t( sqlite3_column_int64( q, 2 ) );
        z.cpuStart = sqlite3_column_int64( q, 3 );
        z.cpuEnd = value( 4 );
        z.gpuStart = value( 5 );
        z.gpuEnd = value( 6 );
        z.skind = uint8_t( sqlite3_column_int( q, 7 ) );
        z.skey = sqlite3_column_int64( q, 8 );
        z.stack = sqlite3_column_int64( q, 9 );
        z.callsite = sqlite3_column_int64( q, 10 );
        z.query = uint16_t( sqlite3_column_int( q, 11 ) );
    }
    // The GPU end the Worker infers for a zone without one: the GPU end of
    // its last descendant that has one, else that descendant's GPU start or
    // the end of the capture.
    int64_t InferredEnd( StoreGpuZoneRecord zone )
    {
        for( ;; )
        {
            auto q = d.Prep( std::string( "SELECT " ) + Columns + " FROM gpu_zones WHERE parent=? ORDER BY id DESC LIMIT 1" );
            Int( q, 1, zone.id );
            const bool child = d.Step( q );
            if( child ) Read( q, zone );
            sqlite3_reset( q );
            if( !child ) return zone.gpuStart >= 0 ? zone.gpuStart : lastTime;
            if( zone.gpuEnd >= 0 ) return zone.gpuEnd;
        }
    }
    // Reads the next zones of the group with their child counts and times.
    void Fill( size_t skip )
    {
        rows.clear();
        row = 0;
        const auto& g = index.groups[group];
        auto q = d.Prep( std::string( "SELECT " ) + Columns + " FROM gpu_zones WHERE context=?1 AND thread=?2 AND id>?3 ORDER BY id LIMIT ?4" );
        Int( q, 1, g.context );
        Int( q, 2, g.thread );
        Int( q, 3, after );
        Int( q, 4, uint64_t( skip + 4096 ) );
        StoreGpuZoneRecord z;
        size_t skipped = 0;
        while( d.Step( q ) )
        {
            Read( q, z );
            after = z.id;
            if( skipped < skip )
            {
                ++skipped;
                continue;
            }
            rows.push_back( z );
        }
        sqlite3_reset( q );
        exhausted = rows.size() < 4096;
        if( rows.empty() ) return;
        std::unordered_map<uint64_t, size_t> position;
        position.reserve( rows.size() );
        for( size_t n = 0; n < rows.size(); ++n ) position.emplace( rows[n].id, n );
        // Zone IDs interleave contexts. A sparse Copy queue batch can span
        // millions of Direct/Compute zones. Probe only the actual parents in
        // this batch instead of decoding every other queue's children in the
        // numeric ID range. The existing parent and context indexes suffice.
        q = d.Prep( std::string( "SELECT " ) + Columns +
            " FROM gpu_zones WHERE parent IN(SELECT id FROM gpu_zones WHERE context=?3 AND thread=?4 AND id>=?1 AND id<=?2) ORDER BY parent,id" );
        Int( q, 1, rows.front().id );
        Int( q, 2, rows.back().id );
        Int( q, 3, g.context );
        Int( q, 4, g.thread );
        while( d.Step( q ) )
        {
            Read( q, z );
            const auto found = position.find( z.parent );
            if( found == position.end() ) continue;
            auto& parent = rows[found->second];
            ++parent.children;
            const auto end = z.gpuEnd >= 0 ? z.gpuEnd : InferredEnd( z );
            if( end >= z.gpuStart ) parent.childTime += end - z.gpuStart;
        }
        sqlite3_reset( q );
    }
    void Seek( size_t g, uint64_t position )
    {
        Require( g < index.groups.size() && position <= index.groups[g].zones, "GPU zone position out of range" );
        group = g;
        ordinal = position;
        rows.clear();
        row = 0;
        const auto& samples = index.groups[g].samples;
        const auto sample = size_t( position / GpuGroupIndex::SampleStride );
        if( position == index.groups[g].zones )
        {
            exhausted = true;
            return;
        }
        after = samples[sample] - 1;
        Fill( size_t( position % GpuGroupIndex::SampleStride ) );
    }
    bool Next( StoreGpuZoneRecord& zone )
    {
        if( group == SIZE_MAX ) return false;
        if( row == rows.size() )
        {
            if( exhausted ) return false;
            Fill( 0 );
            if( rows.empty() ) return false;
        }
        zone = rows[row++];
        ++ordinal;
        return true;
    }
};
StoreGpuZoneCursor::StoreGpuZoneCursor( std::unique_ptr<Impl> impl )
    : m_impl( std::move( impl ) )
{
}
StoreGpuZoneCursor::~StoreGpuZoneCursor() = default;
void StoreGpuZoneCursor::Seek( size_t group, uint64_t ordinal )
{
    m_impl->Seek( group, ordinal );
}
bool StoreGpuZoneCursor::Next( StoreGpuZoneRecord& zone )
{
    return m_impl->Next( zone );
}
uint64_t StoreGpuZoneCursor::Ordinal() const
{
    return m_impl->ordinal;
}
std::vector<StoreGpuZoneGroup> EventStore::GpuZoneGroups() const
{
    const auto& index = EventStoreGpuIndex( *m_impl );
    std::vector<StoreGpuZoneGroup> groups;
    for( const auto& g : index.groups ) groups.push_back( { g.context, g.thread, g.zones, g.completed } );
    return groups;
}
std::unique_ptr<StoreGpuZoneCursor> EventStore::GpuZoneCursor() const
{
    return std::unique_ptr<StoreGpuZoneCursor>( new StoreGpuZoneCursor( std::make_unique<StoreGpuZoneCursor::Impl>( m_impl->db, EventStoreGpuIndex( *m_impl ), m_impl->info.lastNs ) ) );
}
std::vector<std::array<uint64_t, 3>> EventStore::GpuZonesMeeting( int64_t start, int64_t end ) const
{
    std::vector<std::array<uint64_t, 3>> result;
    if( end < start ) return result;
    auto& d = m_impl->db;
    auto q = d.Prep( "SELECT id,context,thread FROM gpu_zones WHERE id IN(SELECT id FROM gpu_range WHERE mint<=?1 AND maxt>=?2) AND gpu_start IS NOT NULL "
        "AND min(gpu_start,COALESCE(gpu_end,gpu_start))<=?3 AND max(gpu_start,COALESCE(gpu_end,gpu_start))>=?4 ORDER BY id" );
    Int( q, 1, uint64_t( Bucket( end ) ) );
    Int( q, 2, uint64_t( Bucket( start ) ) );
    Int( q, 3, uint64_t( end ) );
    Int( q, 4, uint64_t( start ) );
    while( d.Step( q ) ) result.push_back( { uint64_t( sqlite3_column_int64( q, 0 ) ), uint64_t( sqlite3_column_int64( q, 1 ) ), uint64_t( sqlite3_column_int64( q, 2 ) ) } );
    sqlite3_reset( q );
    return result;
}
void EventStore::VisitRootGpuZones( const std::function<void( uint64_t, int64_t, int64_t )>& visit ) const
{
    auto& d = m_impl->db;
    auto q = d.Prep( "SELECT context,gpu_start,gpu_end FROM gpu_zones WHERE parent=0 AND gpu_start IS NOT NULL AND gpu_end IS NOT NULL ORDER BY id" );
    while( d.Step( q ) ) visit( uint64_t( sqlite3_column_int64( q, 0 ) ), sqlite3_column_int64( q, 1 ), sqlite3_column_int64( q, 2 ) );
    sqlite3_reset( q );
}
std::optional<uint64_t> EventStore::GpuZoneOrdinal( size_t group, uint64_t id ) const
{
    const auto& index = EventStoreGpuIndex( *m_impl );
    if( group >= index.groups.size() ) return {};
    const auto& g = index.groups[group];
    const auto sample = std::upper_bound( g.samples.begin(), g.samples.end(), id );
    if( sample == g.samples.begin() ) return {};
    const auto first = *( sample - 1 );
    auto& d = m_impl->db;
    auto q = d.Prep( "SELECT COUNT(*),MAX(id=?4) FROM gpu_zones WHERE context=?1 AND thread=?2 AND id>=?3 AND id<=?4" );
    Int( q, 1, g.context );
    Int( q, 2, g.thread );
    Int( q, 3, first );
    Int( q, 4, id );
    Require( d.Step( q ), "Missing SQL scalar" );
    const uint64_t count = sqlite3_column_int64( q, 0 );
    const bool found = sqlite3_column_int64( q, 1 ) != 0;
    sqlite3_reset( q );
    if( !found ) return {};
    return uint64_t( sample - 1 - g.samples.begin() ) * GpuGroupIndex::SampleStride + count - 1;
}
std::vector<uint64_t> EventStore::GpuContextIds() const
{
    auto& d = m_impl->db;
    std::vector<uint64_t> result;
    auto q = d.Prep( "SELECT id FROM gpu_contexts ORDER BY sequence" );
    while( d.Step( q ) ) result.push_back( uint64_t( sqlite3_column_int64( q, 0 ) ) );
    sqlite3_reset( q );
    return result;
}
const PacketTypeIndex& EventStorePacketIndex( EventStore::Impl& impl, uint8_t type )
{
    auto found = impl.packetIndex.find( type );
    if( found == impl.packetIndex.end() ) found = impl.packetIndex.emplace( type, BuildPacketTypeIndex( impl.db, type ) ).first;
    return found->second;
}
uint64_t EventStore::PacketCount( uint8_t type ) const
{
    return EventStorePacketIndex( *m_impl, type ).total;
}
std::vector<StoreExportPacket> EventStore::PacketsAt( uint8_t type, uint64_t ordinal, size_t limit ) const
{
    Require( type >= uint8_t( QueueType::JnJobType ) && type <= uint8_t( QueueType::JnGpuCatalogBatch ), "Unknown export packet type" );
    const auto& index = EventStorePacketIndex( *m_impl, type );
    std::vector<StoreExportPacket> result;
    if( ordinal >= index.total || limit == 0 ) return result;
    result.reserve( size_t( std::min<uint64_t>( limit, index.total - ordinal ) ) );
    auto block = size_t( std::upper_bound( index.blocks.begin(), index.blocks.end(), ordinal, []( uint64_t v, const PacketTypeIndex::Block& b ) { return v < b.before; } ) - index.blocks.begin() ) - 1;
    auto row = size_t( ordinal - index.blocks[block].before );
    for( ; block < index.blocks.size() && result.size() < limit; ++block, row = 0 )
    {
        const auto packets = LoadPacketBlock( m_impl->db, m_impl->blocks, m_impl->zoneCache, index.blocks[block].id );
        Require( packets->rows.size() == index.blocks[block].count && packets->size <= 31, "Packet block count mismatch" );
        for( ; row < packets->rows.size() && result.size() < limit; ++row )
        {
            const auto& r = packets->rows[row];
            StoreExportPacket packet;
            packet.event = r.id;
            packet.thread = r.thread;
            packet.stack = r.stack;
            packet.type = type;
            packet.size = uint8_t( packets->size );
            memcpy( packet.payload.data(), packets->Payload( row ).data(), packets->size );
            result.push_back( packet );
        }
    }
    return result;
}
void EventStore::VisitPackets( uint8_t type, uint64_t from, uint64_t to, const std::function<bool( const StoreExportPacket& )>& visit ) const
{
    Require( type >= uint8_t( QueueType::JnJobType ) && type <= uint8_t( QueueType::JnGpuCatalogBatch ), "Unknown export packet type" );
    if( from > to ) return;
    StoreExportPacket packet;
    packet.type = type;
    ForPackets( m_impl->db, m_impl->blocks, m_impl->zoneCache, type, from, I( std::min<uint64_t>( to, uint64_t( INT64_MAX ) ) ), [&]( const PacketBlock& packets, size_t n ) {
        const auto& r = packets.rows[n];
        Require( packets.size <= 31, "Invalid export packet length" );
        packet.event = r.id;
        packet.thread = r.thread;
        packet.stack = r.stack;
        packet.size = uint8_t( packets.size );
        memcpy( packet.payload.data(), packets.Payload( n ).data(), packets.size );
        return visit( packet );
    } );
}
constexpr uint64_t JobSampleStride = 4096;
const std::pair<std::vector<uint64_t>, uint64_t>& EventStoreJobIndex( EventStore::Impl& impl )
{
    if( !impl.jobIndex )
    {
        std::vector<uint64_t> samples;
        uint64_t count = 0;
        auto q = impl.db.Prep( "SELECT id FROM jobs ORDER BY id" );
        while( impl.db.Step( q ) )
        {
            if( count % JobSampleStride == 0 ) samples.push_back( uint64_t( sqlite3_column_int64( q, 0 ) ) );
            ++count;
        }
        sqlite3_reset( q );
        impl.jobIndex.emplace( std::move( samples ), count );
    }
    return *impl.jobIndex;
}
uint64_t EventStore::JobCount() const
{
    return EventStoreJobIndex( *m_impl ).second;
}
std::vector<StoreJobKey> EventStore::JobKeys( uint64_t ordinal, size_t limit ) const
{
    const auto& [samples, count] = EventStoreJobIndex( *m_impl );
    std::vector<StoreJobKey> result;
    if( ordinal >= count || limit == 0 ) return result;
    const auto skip = ordinal % JobSampleStride;
    auto& d = m_impl->db;
    auto q = d.Prep( "SELECT id,event,last_event FROM jobs WHERE id>=? ORDER BY id LIMIT ?" );
    Int( q, 1, samples[size_t( ordinal / JobSampleStride )] );
    Int( q, 2, uint64_t( skip + std::min<uint64_t>( limit, count - ordinal ) ) );
    uint64_t row = 0;
    while( d.Step( q ) )
    {
        if( row++ < skip ) continue;
        result.push_back( { uint64_t( sqlite3_column_int64( q, 0 ) ), uint64_t( sqlite3_column_int64( q, 1 ) ), uint64_t( sqlite3_column_int64( q, 2 ) ) } );
    }
    sqlite3_reset( q );
    return result;
}
std::vector<StoreJobKey> EventStore::JobKeysMeeting( int64_t start, int64_t end ) const
{
    std::vector<StoreJobKey> result;
    if( end < start ) return result;
    auto& d = m_impl->db;
    auto q = d.Prep( "SELECT id,event,last_event FROM jobs WHERE id IN(SELECT id FROM job_range WHERE mint<=?1 AND maxt>=?2) "
        "AND COALESCE(first_time,schedule,0)<=?3 AND (completed IS NULL OR completed>=?4) ORDER BY id" );
    Int( q, 1, uint64_t( Bucket( end ) ) );
    Int( q, 2, uint64_t( Bucket( start ) ) );
    Int( q, 3, uint64_t( end ) );
    Int( q, 4, uint64_t( start ) );
    while( d.Step( q ) ) result.push_back( { uint64_t( sqlite3_column_int64( q, 0 ) ), uint64_t( sqlite3_column_int64( q, 1 ) ), uint64_t( sqlite3_column_int64( q, 2 ) ) } );
    sqlite3_reset( q );
    return result;
}
std::optional<StoreJobKey> EventStore::JobKey( uint64_t id ) const
{
    auto& d = m_impl->db;
    auto q = d.Prep( "SELECT event,last_event FROM jobs WHERE id=?" );
    Int( q, 1, id );
    std::optional<StoreJobKey> result;
    if( d.Step( q ) ) result = StoreJobKey{ id, uint64_t( sqlite3_column_int64( q, 0 ) ), uint64_t( sqlite3_column_int64( q, 1 ) ) };
    sqlite3_reset( q );
    return result;
}
std::vector<std::pair<uint32_t, std::optional<std::string>>> EventStore::JobTypeNames() const
{
    auto& d = m_impl->db;
    std::vector<std::pair<uint32_t, std::optional<std::string>>> result;
    // A snapshot maps raw strings of these kinds, the first kind winning.
    const auto kinds = std::to_string( uint8_t( QueueType::StringData ) ) + "," + std::to_string( uint8_t( QueueType::PlotName ) ) + "," + std::to_string( uint8_t( QueueType::FrameName ) );
    auto q = d.Prep( "SELECT t.id,(SELECT s.value FROM strings s WHERE s.key=t.name_key AND s.kind IN(" + kinds + ") ORDER BY s.kind LIMIT 1) FROM job_types t ORDER BY t.event" );
    while( d.Step( q ) )
    {
        std::optional<std::string> name;
        if( sqlite3_column_type( q, 1 ) != SQLITE_NULL ) name = Col( q, 1 );
        result.emplace_back( uint32_t( sqlite3_column_int64( q, 0 ) ), std::move( name ) );
    }
    sqlite3_reset( q );
    return result;
}
std::optional<uint64_t> EventStore::ZoneAt( uint64_t thread, int64_t time ) const
{
    const auto& index = EventStoreZoneIndex( *m_impl );
    const auto position = index.Find( thread );
    if( position == SIZE_MAX ) return {};
    auto& d = m_impl->db;
    std::optional<std::pair<int64_t, uint64_t>> best;
    // The end of a zone that never closed: the end of its last descendant
    // that closed, else that descendant's start.
    const auto openEnd = [&]( uint64_t id ) {
        auto q = d.Prep( "SELECT end FROM zone_display_end WHERE id=?" );
        Int( q, 1, id );
        const auto end = d.Step( q ) && sqlite3_column_type( q, 0 ) != SQLITE_NULL ? std::optional<I>( sqlite3_column_int64( q, 0 ) ) : std::nullopt;
        sqlite3_reset( q );
        return end;
    };
    for( const auto& block : index.zones[position] )
    {
        if( block.tmin > time || ( !block.unfinished && block.tmax < time ) ) continue;
        const auto rows = LoadZoneBlock( d, m_impl->blocks, m_impl->zoneCache, block.id );
        for( size_t n = 0; n < rows->rows.size(); ++n )
        {
            const auto& r = rows->rows[n];
            if( r.start > time ) break;
            I end = r.end;
            if( r.open ) end = openEnd( r.id ).value_or( r.start );
            if( end < time ) continue;
            const std::pair<int64_t, uint64_t> candidate{ r.start, block.before + n };
            if( !best || candidate > *best ) best = candidate;
        }
    }
    if( !best ) return {};
    return best->second;
}

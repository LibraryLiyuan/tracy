// Streams the zones of one logical thread in id order, decoding one row at a
// time from the current block. Only the decompressed block is held, so a
// merge over every thread stays small.
class ZoneStream
{
    Db& d;
    const BlockFile& file;
    std::vector<uint64_t> blocks;
    size_t nextBlock = 0, remaining = 0;
    std::vector<uint8_t> raw;
    const uint8_t* e = nullptr;
    const uint8_t *pId = nullptr, *pParent = nullptr, *pStart = nullptr, *pDuration = nullptr, *pDepth = nullptr, *pChildren = nullptr, *pSource = nullptr, *pFlags = nullptr;
    const uint8_t *pStack = nullptr, *pCallsite = nullptr, *pValidation = nullptr, *pColor = nullptr, *pChildTime = nullptr, *pName = nullptr;
    std::vector<std::pair<uint8_t, uint64_t>> sources;
    uint64_t lastId = 0, lastValidation = 0;
    int64_t lastStart = 0;
    struct Late
    {
        uint64_t id = 0, children = 0, color = 0;
        bool hasEnd = false, hasName = false, hasColor = false;
        int64_t end = 0, childTime = 0;
        std::string name;
    };
    std::vector<Late> late;
    size_t lateIndex = 0;
    static void Skip( const uint8_t*& p, const uint8_t* end, size_t n )
    {
        for( size_t i = 0; i < n; ++i ) GetVar( p, end );
    }
    bool Load()
    {
        if( nextBlock == blocks.size() ) return false;
        uint64_t key, first, last;
        const auto location = LocateBlock( d, blocks[nextBlock++], BlockStream::CpuZones, key, first, last );
        raw = ReadBlock( file, location );
        const uint8_t* p = raw.data();
        e = p + raw.size();
        remaining = size_t( GetVar( p, e ) );
        sources.resize( size_t( GetVar( p, e ) ) );
        for( auto& s : sources )
        {
            Require( p < e, "Corrupt zone block sources" );
            s.first = *p++;
            s.second = GetVar( p, e );
        }
        pId = p;
        Skip( p, e, remaining );
        pParent = p;
        Skip( p, e, remaining );
        pStart = p;
        Skip( p, e, remaining );
        pDuration = p;
        Skip( p, e, remaining );
        pDepth = p;
        Skip( p, e, remaining );
        pChildren = p;
        Skip( p, e, remaining );
        pSource = p;
        Skip( p, e, remaining );
        pFlags = p;
        Require( size_t( e - p ) >= remaining, "Corrupt zone flags" );
        size_t counts[6] = {};
        for( size_t n = 0; n < remaining; ++n )
            for( int bit = 0; bit < 6; ++bit ) counts[bit] += ( p[n] >> bit ) & 1;
        p += remaining;
        pStack = p;
        Skip( p, e, counts[0] );
        pCallsite = p;
        Skip( p, e, counts[1] );
        pValidation = p;
        Skip( p, e, counts[2] );
        pColor = p;
        Skip( p, e, counts[3] );
        pChildTime = p;
        Skip( p, e, counts[5] );
        pName = p;
        lastId = lastValidation = 0;
        lastStart = 0;
        late.clear();
        lateIndex = 0;
        auto q = d.Prep( "SELECT id,end,children,name,color,child_time FROM zone_late WHERE id BETWEEN ?1 AND ?2 AND thread=?3 ORDER BY id" );
        Int( q, 1, first );
        Int( q, 2, last );
        Int( q, 3, thread );
        while( d.Step( q ) )
        {
            Late l;
            l.id = sqlite3_column_int64( q, 0 );
            l.hasEnd = sqlite3_column_type( q, 1 ) != SQLITE_NULL;
            l.end = sqlite3_column_int64( q, 1 );
            l.children = sqlite3_column_int64( q, 2 );
            l.hasName = sqlite3_column_type( q, 3 ) != SQLITE_NULL;
            if( l.hasName ) l.name = Col( q, 3 );
            l.hasColor = sqlite3_column_type( q, 4 ) != SQLITE_NULL;
            l.color = sqlite3_column_int64( q, 4 );
            l.childTime = sqlite3_column_int64( q, 5 );
            late.push_back( std::move( l ) );
        }
        sqlite3_reset( q );
        return remaining > 0;
    }

public:
    uint64_t thread = 0;
    ZoneRow row;
    ZoneStream( Db& db, const BlockFile& f, uint64_t tid, std::vector<uint64_t> ids )
        : d( db )
        , file( f )
        , blocks( std::move( ids ) )
        , thread( tid )
    {
    }
    bool Next()
    {
        while( !remaining )
            if( !Load() ) return false;
        --remaining;
        auto& r = row;
        r = ZoneRow{};
        r.id = lastId += GetVar( pId, e );
        const auto distance = GetVar( pParent, e );
        Require( distance <= r.id, "Corrupt zone parent" );
        r.parent = distance ? r.id - distance : 0;
        r.start = lastStart += Unzig( GetVar( pStart, e ) );
        const auto duration = GetVar( pDuration, e );
        r.open = duration == 0;
        if( !r.open ) r.end = r.start + int64_t( duration - 1 );
        r.depth = uint32_t( GetVar( pDepth, e ) );
        r.children = uint32_t( GetVar( pChildren, e ) );
        const auto source = GetVar( pSource, e );
        Require( source < sources.size(), "Corrupt zone source index" );
        r.skind = sources[size_t( source )].first;
        r.skey = sources[size_t( source )].second;
        const auto flags = *pFlags++;
        if( flags & 1 ) r.stack = GetVar( pStack, e );
        if( flags & 2 ) r.callsite = GetVar( pCallsite, e );
        if( flags & 4 ) r.validation = lastValidation += uint64_t( Unzig( GetVar( pValidation, e ) ) );
        if( flags & 8 )
        {
            r.hasColor = true;
            r.color = uint32_t( GetVar( pColor, e ) );
        }
        if( flags & 32 ) r.childTime = int64_t( GetVar( pChildTime, e ) );
        if( flags & 16 )
        {
            const auto size = GetVar( pName, e );
            Require( size <= uint64_t( e - pName ), "Corrupt zone name" );
            r.hasName = true;
            r.name.assign( reinterpret_cast<const char*>( pName ), size_t( size ) );
            pName += size;
        }
        while( lateIndex < late.size() && late[lateIndex].id < r.id ) ++lateIndex;
        if( r.open && lateIndex < late.size() && late[lateIndex].id == r.id )
        {
            const auto& l = late[lateIndex];
            if( l.hasEnd )
            {
                r.open = false;
                r.end = l.end;
            }
            r.children = uint32_t( l.children );
            r.childTime = l.childTime;
            if( l.hasName )
            {
                r.hasName = true;
                r.name = l.name;
            }
            if( l.hasColor )
            {
                r.hasColor = true;
                r.color = uint32_t( l.color );
            }
        }
        return true;
    }
};
// Streams the zone texts of one logical thread in (zone,event) order.
class TextStream
{
    Db& d;
    const BlockFile& file;
    std::vector<uint64_t> blocks;
    size_t nextBlock = 0, remaining = 0;
    std::vector<uint8_t> raw;
    const uint8_t *p = nullptr, *e = nullptr, *bytes = nullptr;
    bool Load()
    {
        if( nextBlock == blocks.size() ) return false;
        uint64_t key, first, last;
        const auto location = LocateBlock( d, blocks[nextBlock++], BlockStream::ZoneText, key, first, last );
        raw = ReadBlock( file, location );
        p = raw.data();
        e = p + raw.size();
        remaining = size_t( GetVar( p, e ) );
        const uint8_t* q = p;
        for( size_t n = 0; n < remaining * 3; ++n ) GetVar( q, e );
        bytes = q;
        zone = event = 0;
        return remaining > 0;
    }

public:
    uint64_t zone = 0, event = 0;
    std::string_view text;
    bool valid = false;
    TextStream( Db& db, const BlockFile& f, std::vector<uint64_t> ids )
        : d( db )
        , file( f )
        , blocks( std::move( ids ) )
    {
        valid = Next();
    }
    bool Next()
    {
        while( !remaining )
            if( !Load() ) return valid = false;
        --remaining;
        zone += GetVar( p, e );
        event += GetVar( p, e );
        const auto size = GetVar( p, e );
        Require( size <= uint64_t( e - bytes ), "Corrupt zone text" );
        text = std::string_view( reinterpret_cast<const char*>( bytes ), size_t( size ) );
        bytes += size;
        return valid = true;
    }
};

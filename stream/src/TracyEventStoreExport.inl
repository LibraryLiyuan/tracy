namespace
{
class ExportStringHash
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;

public:
    ExportStringHash()
    {
        Require( BCryptOpenAlgorithmProvider( &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0 ) == 0, "String hash initialization failed" );
        if( BCryptCreateHash( algorithm, &hash, nullptr, 0, nullptr, 0, 0 ) != 0 )
        {
            BCryptCloseAlgorithmProvider( algorithm, 0 );
            algorithm = nullptr;
            throw std::runtime_error( "String hash creation failed" );
        }
    }
    ~ExportStringHash()
    {
        if( hash ) BCryptDestroyHash( hash );
        if( algorithm ) BCryptCloseAlgorithmProvider( algorithm, 0 );
    }
    void Add( std::span<const uint8_t> bytes )
    {
        Require( BCryptHashData( hash, const_cast<PUCHAR>( bytes.data() ), ULONG( bytes.size() ), 0 ) == 0, "String hash update failed" );
    }
    std::array<uint8_t, 32> Finish()
    {
        std::array<uint8_t, 32> digest{};
        Require( BCryptFinishHash( hash, digest.data(), 32, 0 ) == 0, "String hash finish failed" );
        return digest;
    }
};
class SnapshotExport
{
    Db& d;
    const BlockFile& blocks;
    const StoreInfo& info;
    StoreCaptureMetadata capture;
    StoreExportOptions options;
    std::function<void( std::span<const uint8_t> )> output;
    uint64_t bytes = 0, operations = 0;
    bool dictionaryFrozen = false;
    static constexpr uint64_t StringLimit = ( 1ull << 24 ) - 1;
    static constexpr const char* Names[] = { "e_strings", "e_chunks", "e_raw", "e_threads", "e_local", "e_external", "e_sources", "e_source_map", "e_extra", "e_cpu", "e_gpu", "e_plots", "e_names", "e_stackids", "e_verify", "e_open" };
    // Serialized bytes are staged in a fixed buffer. Call boundaries never
    // change the stream; Emit() flushes the remainder.
    std::vector<uint8_t> staged = std::vector<uint8_t>( 1 << 16 );
    size_t stagedSize = 0;
    // Fixed-size caches of mappings that are immutable once Prepare() built
    // them. Keys are compared in full; a miss reads the temp mapping table.
    struct Mapping
    {
        uint64_t a = 0, b = 0, value = 0;
        bool valid = false;
    };
    static constexpr size_t MappingSlots = 4096;
    std::vector<Mapping> stackMap = std::vector<Mapping>( MappingSlots ), threadMap = std::vector<Mapping>( MappingSlots ), sourceMap = std::vector<Mapping>( MappingSlots );
    static Mapping& Slot( std::vector<Mapping>& map, uint64_t a, uint64_t b )
    {
        return map[( ( a * 0x9E3779B97F4A7C15ull ) ^ ( b * 0xC2B2AE3D27D4EB4Full ) ) >> 52];
    }
    // Zone counts gathered while staging zones. Source counts are indexed by
    // the int16 snapshot source plus 32768, i.e. in ascending source order.
    std::vector<uint64_t> cpuSourceZones = std::vector<uint64_t>( 65536 ), gpuSourceZones = std::vector<uint64_t>( 65536 );
    uint64_t cpuParents = 0, gpuParents = 0, extraRows = 1;
    std::vector<uint8_t> textBuffer;
    // Open ancestors of the timeline being serialized, innermost last. The
    // newest OpenWindow entries stay in memory; older ones spill to
    // temp.e_open so memory does not grow with nesting depth.
    struct OpenNode
    {
        uint64_t id = 0, remaining = 0;
        I end = 0, gpuEnd = 0;
        uint64_t query = 0;
    };
    static constexpr size_t OpenWindow = 65536;
    std::vector<OpenNode> openNodes;
    uint64_t openSpilled = 0;
    void PushOpen( const OpenNode& node )
    {
        if( openNodes.size() == OpenWindow )
        {
            for( size_t n = 0; n < OpenWindow / 2; ++n )
            {
                const auto& spilled = openNodes[n];
                auto q = d.Prep( "INSERT INTO temp.e_open VALUES(?,?,?,?,?,?)" );
                Int( q, 1, openSpilled + n );
                Int( q, 2, spilled.id );
                Int( q, 3, spilled.remaining );
                Int( q, 4, uint64_t( spilled.end ) );
                Int( q, 5, uint64_t( spilled.gpuEnd ) );
                Int( q, 6, spilled.query );
                d.Run( q );
            }
            openSpilled += OpenWindow / 2;
            openNodes.erase( openNodes.begin(), openNodes.begin() + OpenWindow / 2 );
        }
        openNodes.push_back( node );
    }
    bool HasOpen() const { return !openNodes.empty() || openSpilled; }
    OpenNode& InnerOpen()
    {
        if( openNodes.empty() )
        {
            Require( openSpilled != 0, "Timeline depth stack is empty" );
            --openSpilled;
            auto q = d.Prep( "SELECT id,remaining,end,gpu_end,query FROM temp.e_open WHERE depth=?" );
            Int( q, 1, openSpilled );
            Require( d.Step( q ), "Timeline depth stack missing" );
            openNodes.push_back( { uint64_t( Val( q, 0 ) ), uint64_t( Val( q, 1 ) ), Val( q, 2 ), Val( q, 3 ), uint64_t( Val( q, 4 ) ) } );
            sqlite3_reset( q );
            q = d.Prep( "DELETE FROM temp.e_open WHERE depth=?" );
            Int( q, 1, openSpilled );
            d.Run( q );
        }
        return openNodes.back();
    }
    OpenNode PopOpen()
    {
        const auto node = InnerOpen();
        openNodes.pop_back();
        return node;
    }
    void Poll()
    {
        if( options.cancelled && options.cancelled() ) throw std::runtime_error( "Cancelled" );
        if( ( ++operations & 4095 ) == 0 ) Require( uint64_t( d.Scalar( "PRAGMA temp.page_count" ) ) * uint64_t( d.Scalar( "PRAGMA temp.page_size" ) ) <= options.temporaryBytes, "Export temporary disk budget exceeded" );
    }
    void Stage( const char* name )
    {
        Poll();
        if( options.progress ) options.progress( name, bytes );
    }
    template<class F>
    void Each( const std::string& sql, F callback )
    {
        auto q = d.Prep( sql );
        try
        {
            while( d.Step( q ) )
            {
                Poll();
                callback( q );
            }
        }
        catch( ... )
        {
            sqlite3_reset( q );
            throw;
        }
        sqlite3_reset( q );
    }
    template<class T>
    void Put( const T& value )
    {
        Raw( { reinterpret_cast<const uint8_t*>( &value ), sizeof( value ) } );
    }
    void Raw( std::span<const uint8_t> data )
    {
        Require( data.size() <= UINT64_MAX - bytes, "Snapshot length overflow" );
        bytes += data.size();
        if( data.size() > staged.size() - stagedSize )
        {
            Flush();
            if( data.size() >= staged.size() )
            {
                output( data );
                return;
            }
        }
        memcpy( staged.data() + stagedSize, data.data(), data.size() );
        stagedSize += data.size();
    }
    void Flush()
    {
        Poll();
        if( !stagedSize ) return;
        output( { staged.data(), stagedSize } );
        stagedSize = 0;
    }
    void U8( uint8_t n )
    {
        Put( n );
    }
    void U16( uint16_t n )
    {
        Put( n );
    }
    void U32( uint32_t n )
    {
        Put( n );
    }
    void U64( uint64_t n )
    {
        Put( n );
    }
    void I64( I n )
    {
        Put( n );
    }
    void U24( uint64_t n )
    {
        Require( n < ( 1ull << 24 ), "Snapshot 24-bit field overflow" );
        uint8_t b[3] = { uint8_t( n ), uint8_t( n >> 8 ), uint8_t( n >> 16 ) };
        Raw( b );
    }
    void Delta( I& reference, I value )
    {
        I64( Sub( value, reference ) );
        reference = value;
    }
    static I Val( sqlite3_stmt* q, int n )
    {
        return sqlite3_column_int64( q, n );
    }
    static bool Null( sqlite3_stmt* q, int n )
    {
        return sqlite3_column_type( q, n ) == SQLITE_NULL;
    }
    uint64_t Count( const std::string& table )
    {
        return uint64_t( d.Scalar( "SELECT COUNT(*) FROM " + table ) );
    }
    void String64( const std::string& text )
    {
        U64( text.size() );
        Raw( { reinterpret_cast<const uint8_t*>( text.data() ), text.size() } );
    }
    void BindBytes( sqlite3_stmt* q, int column, std::span<const uint8_t> value )
    {
        if( value.empty() ) Require( sqlite3_bind_zeroblob( q, column, 0 ) == SQLITE_OK, "Empty export blob binding failed" );
        else Blob( q, column, value );
    }
    // Dictionary strings of at most 65535 bytes keep their bytes in
    // e_strings.value and are found through a 64-bit hash index; equality is
    // always decided by comparing the stored bytes. Longer zone texts keep
    // their bytes only in e_chunks (value NULL) and are matched by digest.
    static uint64_t StringHash( std::span<const uint8_t> value )
    {
        return XXH3_64bits( value.data(), value.size() );
    }
    uint64_t FindString( uint64_t hash, std::span<const uint8_t> value )
    {
        auto q = d.Prep( "SELECT id,value FROM temp.e_strings WHERE hash=?" );
        Int( q, 1, hash );
        while( d.Step( q ) )
        {
            const auto data = static_cast<const uint8_t*>( sqlite3_column_blob( q, 1 ) );
            const auto size = size_t( sqlite3_column_bytes( q, 1 ) );
            if( size == value.size() && ( !size || memcmp( data, value.data(), size ) == 0 ) )
            {
                const uint64_t id = Val( q, 0 );
                sqlite3_reset( q );
                return id;
            }
        }
        sqlite3_reset( q );
        return 0;
    }
    // Repeated zone texts are usually close together, so a small exact cache
    // of recent strings resolves most of them without touching the hash
    // index. Bounded: RecentSlots strings of at most RecentBytes each, and
    // RecentBudget bytes of string capacity in total.
    struct RecentString
    {
        uint64_t hash = 0, id = 0;
        std::string text;
    };
    static constexpr size_t RecentSlots = 8192, RecentBytes = 4096, RecentBudget = 8 << 20;
    std::vector<RecentString> recent = std::vector<RecentString>( RecentSlots );
    // Sum of the slots' string capacities.
    size_t recentBytes = RecentSlots * std::string().capacity();
    void Remember( RecentString& slot, uint64_t hash, uint64_t id, std::string_view text )
    {
        const auto before = slot.text.capacity();
        // A slot keeps its largest capacity across assignments; release it
        // when it far exceeds the new text.
        if( before > text.size() * 2 + 64 ) std::string().swap( slot.text );
        if( recentBytes - before + std::max( slot.text.capacity(), text.size() ) <= RecentBudget )
        {
            slot.text.assign( text );
            slot.hash = hash;
            slot.id = id;
        }
        else
        {
            std::string().swap( slot.text );
            slot.id = 0;
        }
        recentBytes = recentBytes - before + slot.text.capacity();
    }
    uint64_t Intern( std::string_view text )
    {
        text = text.substr( 0, text.find( '\0' ) );
        Require( text.size() <= 65535, "Use paged string construction for long text" );
        const std::span<const uint8_t> value( reinterpret_cast<const uint8_t*>( text.data() ), text.size() );
        const auto hash = StringHash( value );
        auto& cached = recent[hash & ( RecentSlots - 1 )];
        if( cached.id && cached.hash == hash && cached.text == text ) return cached.id;
        uint64_t id = FindString( hash, value );
        if( !id )
        {
            Require( !dictionaryFrozen, "Snapshot string dictionary changed during serialization" );
            auto q = d.Prep( "INSERT INTO temp.e_strings(value,total,hash) VALUES(?,?,?)" );
            BindBytes( q, 1, value );
            Int( q, 2, value.size() );
            Int( q, 3, hash );
            d.Run( q );
            id = uint64_t( sqlite3_last_insert_rowid( d.handle ) );
        }
        Require( id <= StringLimit, "Snapshot string index limit exceeded" );
        if( text.size() <= RecentBytes ) Remember( cached, hash, id, text );
        return id;
    }
    uint64_t RawString( uint64_t key )
    {
        auto q = d.Prep( "SELECT sid FROM temp.e_raw WHERE key=?" );
        Int( q, 1, key );
        const auto id = d.Step( q ) ? uint64_t( Val( q, 0 ) ) : 0;
        sqlite3_reset( q );
        return id;
    }
    void RawMapping( uint64_t key, uint64_t sid )
    {
        auto q = d.Prep( "INSERT OR IGNORE INTO temp.e_raw VALUES(?,?)" );
        Int( q, 1, key );
        Int( q, 2, sid );
        d.Run( q );
    }
    uint64_t Synthetic( uint64_t sid )
    {
        uint64_t key = ( 1ull << 63 ) | sid;
        while( RawString( key ) && RawString( key ) != sid ) ++key;
        RawMapping( key, sid );
        return key;
    }
    uint64_t Stack( uint64_t id )
    {
        if( !id ) return 0;
        auto& cached = Slot( stackMap, id, 0 );
        if( cached.valid && cached.a == id ) return cached.value;
        auto q = d.Prep( "SELECT n FROM temp.e_stackids WHERE id=?" );
        Int( q, 1, id );
        Require( d.Step( q ), "Unmapped snapshot callstack" );
        const uint64_t value = Val( q, 0 );
        sqlite3_reset( q );
        cached = { id, 0, value, true };
        return value;
    }
    uint16_t Thread( uint64_t id, bool external = false )
    {
        auto& cached = Slot( threadMap, id, external );
        if( cached.valid && cached.a == id && cached.b == uint64_t( external ) ) return uint16_t( cached.value );
        auto q = d.PrepStatic( external ? "SELECT n FROM temp.e_external WHERE id=?" : "SELECT n FROM temp.e_local WHERE id=?" );
        Int( q, 1, id );
        Require( d.Step( q ), "Unmapped snapshot thread" );
        const uint64_t value = Val( q, 0 );
        sqlite3_reset( q );
        Require( value <= 65535, "Snapshot thread index limit exceeded" );
        cached = { id, uint64_t( external ), value, true };
        return uint16_t( value );
    }
    int16_t Source( int kind, uint64_t key )
    {
        if( kind == 0 && key == 0 ) return 0;
        auto& cached = Slot( sourceMap, key, uint64_t( kind ) );
        if( cached.valid && cached.a == key && cached.b == uint64_t( kind ) ) return int16_t( I( cached.value ) );
        auto q = d.Prep( "SELECT sid FROM temp.e_source_map WHERE kind=? AND key=?" );
        Int( q, 1, kind );
        Int( q, 2, key );
        Require( d.Step( q ), "Unmapped snapshot source location" );
        const auto value = Val( q, 0 );
        sqlite3_reset( q );
        Require( value >= INT16_MIN + 1 && value <= INT16_MAX, "Snapshot source index overflow" );
        cached = { key, uint64_t( kind ), uint64_t( value ), true };
        return int16_t( value );
    }
    void Ref( uint64_t value, bool indexed, bool active = true )
    {
        U64( value );
        U8( active ? ( indexed ? 3 : 2 ) : 0 );
    }
    void SqlBlob( sqlite3_stmt* q, int column )
    {
        const auto n = sqlite3_column_bytes( q, column );
        Raw( { static_cast<const uint8_t*>( sqlite3_column_blob( q, column ) ), size_t( n ) } );
    }
    uint64_t ZoneText( const std::vector<std::pair<uint64_t, std::string>>& parts, size_t count );
    void Prepare();
    void Header();
    void Strings();
    void Sources();
    void Locks();
    void Messages();
    void Cpu();
    void Gpu();
    void Plots();
    void Memory();
    void Stacks();
    void Images();
    void Scheduling();
    void Code();
    void Jn();
    void Timeline( uint64_t owner, uint64_t lane, bool gpu, uint64_t roots );
    void CountVector( const std::string& query, const std::function<void( sqlite3_stmt* )>& row )
    {
        U64( uint64_t( d.Scalar( "SELECT COUNT(*) FROM (" + query + ")" ) ) );
        Each( query, row );
    }

public:
    SnapshotExport( Db& db, const BlockFile& file, const StoreInfo& data, StoreCaptureMetadata metadata, const StoreExportOptions& opts )
        : d( db )
        , blocks( file )
        , info( data )
        , capture( std::move( metadata ) )
        , options( opts )
    {
    }
    ~SnapshotExport()
    {
        for( auto name : Names )
        {
            try
            {
                d.Exec( std::string( "DROP TABLE IF EXISTS temp." ) + name );
            }
            catch( ... )
            {
            }
        }
    }
    void Setup()
    {
        Prepare();
        dictionaryFrozen = true;
    }
    uint64_t Emit( std::function<void( std::span<const uint8_t> )> sink )
    {
        output = std::move( sink );
        bytes = 0;
        stagedSize = 0;
        Header();
        Strings();
        Sources();
        Locks();
        Messages();
        Cpu();
        Gpu();
        Plots();
        Memory();
        Stacks();
        Images();
        Scheduling();
        Code();
        Jn();
        Flush();
        return bytes;
    }
};
uint64_t SnapshotExport::ZoneText( const std::vector<std::pair<uint64_t, std::string>>& parts, size_t count )
{
    // Fragment concatenation is streamed into chunks. No group_concat or
    // capture-sized text buffer is used, including for multi-megabyte zones.
    // `parts` holds the first `count` fragments of one zone in event order.
    auto& buffer = textBuffer;
    buffer.clear();
    uint64_t id = 0, total = 0, part = 0;
    bool first = true;
    std::unique_ptr<ExportStringHash> hash;
    const auto save = [&] {
        hash->Add( buffer );
        auto q = d.Prep( "INSERT INTO temp.e_chunks VALUES(?,?,?)" );
        Int( q, 1, id );
        Int( q, 2, part++ );
        BindBytes( q, 3, buffer );
        d.Run( q );
        buffer.clear();
    };
    const auto add = [&]( std::span<const uint8_t> value ) {
        while( !value.empty() )
        {
            const auto n = std::min<size_t>( 65535 - buffer.size(), value.size() );
            buffer.insert( buffer.end(), value.begin(), value.begin() + n );
            value = value.subspan( n );
            total += n;
            if( buffer.size() == 65535 )
            {
                if( !id )
                {
                    auto q = d.Prep( "INSERT INTO temp.e_strings(total) VALUES(0)" );
                    d.Run( q );
                    id = uint64_t( sqlite3_last_insert_rowid( d.handle ) );
                    hash = std::make_unique<ExportStringHash>();
                }
                save();
            }
        }
    };
    for( size_t n = 0; n < count; ++n )
    {
        const auto text = reinterpret_cast<const unsigned char*>( parts[n].second.data() );
        const auto size = parts[n].second.size();
        const auto end = size ? static_cast<const unsigned char*>( memchr( text, 0, size ) ) : nullptr;
        if( !first )
        {
            const uint8_t newline = '\n';
            add( { &newline, 1 } );
        }
        first = false;
        add( { text, end ? size_t( end - text ) : size } );
    }
    if( first ) return 0;
    if( !id ) return Intern( { reinterpret_cast<const char*>( buffer.data() ), buffer.size() } );
    if( !buffer.empty() ) save();
    Require( id <= StringLimit && total <= uint64_t( INT64_MAX ), "Snapshot text index/length limit exceeded" );
    const auto digest = hash->Finish();
    uint64_t existing = 0, shortHash = 0;
    if( total <= 65535 )
    {
        // A text of exactly 65535 bytes may equal a dictionary string.
        auto q = d.Prep( "SELECT data FROM temp.e_chunks WHERE id=? AND part=0" );
        Int( q, 1, id );
        Require( d.Step( q ), "Missing zone text chunk" );
        const std::span<const uint8_t> value( static_cast<const uint8_t*>( sqlite3_column_blob( q, 0 ) ), size_t( sqlite3_column_bytes( q, 0 ) ) );
        shortHash = StringHash( value );
        existing = FindString( shortHash, value );
        sqlite3_reset( q );
    }
    if( !existing )
    {
        auto q = d.Prep( "SELECT id FROM temp.e_strings WHERE total=? AND digest=? AND id!=?" );
        Int( q, 1, total );
        Blob( q, 2, digest );
        Int( q, 3, id );
        while( d.Step( q ) )
        {
            const uint64_t candidate = Val( q, 0 );
            auto equal = d.Prep( "SELECT NOT EXISTS(SELECT 1 FROM temp.e_chunks a LEFT JOIN temp.e_chunks b ON b.id=?2 AND b.part=a.part WHERE a.id=?1 AND (b.data IS NULL OR a.data!=b.data))" );
            Int( equal, 1, id );
            Int( equal, 2, candidate );
            Require( d.Step( equal ), "Missing string comparison" );
            const bool same = Val( equal, 0 ) != 0;
            sqlite3_reset( equal );
            if( same )
            {
                existing = candidate;
                break;
            }
        }
        sqlite3_reset( q );
    }
    if( existing )
    {
        auto q = d.Prep( "DELETE FROM temp.e_chunks WHERE id=?" );
        Int( q, 1, id );
        d.Run( q );
        q = d.Prep( "DELETE FROM temp.e_strings WHERE id=?" );
        Int( q, 1, id );
        d.Run( q );
        return existing;
    }
    auto q = d.Prep( "UPDATE temp.e_strings SET total=?1,digest=?2,value=CASE WHEN ?1<=65535 THEN (SELECT data FROM temp.e_chunks WHERE id=?3 AND part=0) ELSE NULL END,hash=CASE WHEN ?1<=65535 THEN ?4 ELSE NULL END WHERE id=?3" );
    Int( q, 1, total );
    Blob( q, 2, digest );
    Int( q, 3, id );
    Int( q, 4, shortHash );
    d.Run( q );
    return id;
}
void SnapshotExport::Prepare()
{
    Stage( "Prepare mappings" );
    Require( capture.available, "Capture header unavailable" );
    Require( info.unresolvedDefinitions == 0, "Unresolved definitions prevent exact export" );
    const auto staging = options.cacheBytes ? options.cacheBytes : d.options.cacheBytes;
    d.Exec( "PRAGMA temp.cache_size=-" + std::to_string( std::max<uint64_t>( staging / 1024, 4096 ) ) );
    for( auto name : Names ) d.Exec( std::string( "DROP TABLE IF EXISTS temp." ) + name );
    // e_cpu and e_gpu are clustered by timeline owner, so each timeline is one
    // ordered range scan. Child counts are stored with every node.
    d.Exec( R"SQL(
CREATE TEMP TABLE e_strings(id INTEGER PRIMARY KEY,value BLOB,total INTEGER,digest BLOB,hash INTEGER);
CREATE INDEX temp.e_string_hash ON e_strings(hash);
CREATE INDEX temp.e_string_digest ON e_strings(total,digest);
CREATE TEMP TABLE e_chunks(id INTEGER,part INTEGER,data BLOB,PRIMARY KEY(id,part)) WITHOUT ROWID;
CREATE TEMP TABLE e_raw(key INTEGER PRIMARY KEY,sid INTEGER);
CREATE TEMP TABLE e_names(kind INTEGER,key INTEGER,sid INTEGER,PRIMARY KEY(kind,key)) WITHOUT ROWID;
CREATE TEMP TABLE e_threads(id INTEGER PRIMARY KEY,fiber INTEGER DEFAULT 0,hint INTEGER DEFAULT 0,kernel INTEGER DEFAULT 0,zones INTEGER DEFAULT 0,roots INTEGER DEFAULT 0);
CREATE TEMP TABLE e_local(id INTEGER PRIMARY KEY,n INTEGER UNIQUE);
CREATE TEMP TABLE e_external(id INTEGER PRIMARY KEY,n INTEGER UNIQUE);
CREATE TEMP TABLE e_sources(kind INTEGER,n INTEGER,key INTEGER,name INTEGER,function INTEGER,file INTEGER,line INTEGER,color INTEGER,PRIMARY KEY(kind,n),UNIQUE(kind,name,function,file,line,color,key));
CREATE TEMP TABLE e_source_map(kind INTEGER,key INTEGER,sid INTEGER,PRIMARY KEY(kind,key)) WITHOUT ROWID;
CREATE TEMP TABLE e_extra(n INTEGER PRIMARY KEY,stack INTEGER,text INTEGER,name INTEGER,color INTEGER);
INSERT INTO e_extra VALUES(0,0,0,0,0);
CREATE TEMP TABLE e_cpu(thread INTEGER,id INTEGER,parent INTEGER,start INTEGER,end INTEGER,source INTEGER,extra INTEGER,children INTEGER,PRIMARY KEY(thread,id)) WITHOUT ROWID;
CREATE TEMP TABLE e_gpu(context INTEGER,thread INTEGER,id INTEGER,parent INTEGER,cpu_start INTEGER,cpu_end INTEGER,gpu_start INTEGER,gpu_end INTEGER,source INTEGER,stack INTEGER,query INTEGER,children INTEGER,PRIMARY KEY(context,thread,id)) WITHOUT ROWID;
CREATE TEMP TABLE e_plots(id INTEGER PRIMARY KEY,key INTEGER);
CREATE TEMP TABLE e_stackids(id INTEGER PRIMARY KEY,n INTEGER UNIQUE);
CREATE TEMP TABLE e_open(depth INTEGER PRIMARY KEY,id INTEGER,remaining INTEGER,end INTEGER,gpu_end INTEGER,query INTEGER);
)SQL" );
    Intern( "???" );
    Intern( "" );
    Each( "SELECT kind,key,value FROM strings ORDER BY kind,key", [&]( auto q ) {
        const auto sid = Intern( Col( q, 2 ) );
        auto put = d.Prep( "INSERT INTO temp.e_names VALUES(?,?,?)" );
        Int( put, 1, Val( q, 0 ) );
        Int( put, 2, Val( q, 1 ) );
        Int( put, 3, sid );
        d.Run( put );
        const auto kind = QueueType( sqlite3_column_int( q, 0 ) );
        if( kind == QueueType::StringData || kind == QueueType::PlotName || kind == QueueType::FrameName ) RawMapping( Val( q, 1 ), sid );
    } );
    uint64_t ordinal = 1;
    Each( "SELECT id FROM stacks ORDER BY id", [&]( auto q ) {
        auto put = d.Prep( "INSERT INTO temp.e_stackids VALUES(?,?)" );
        Int( put, 1, Val( q, 0 ) );
        Int( put, 2, ordinal++ );
        d.Run( put );
    } );
    Require( ordinal <= StringLimit, "Too many snapshot callstacks" );
    uint64_t staticSource = 0, dynamicSource = 0;
    Each( "SELECT kind,key,nkey,fkey,pkey,line,color,name,function,file FROM sources ORDER BY kind,key", [&]( auto q ) {
        const int kind = int( Val( q, 0 ) );
        const uint64_t key = Val( q, 1 );
        uint64_t n, fn, file;
        I number;
        if( kind == 0 )
        {
            n = Val( q, 2 );
            fn = Val( q, 3 );
            file = Val( q, 4 );
            number = I( ++staticSource );
        }
        else
        {
            const auto name = Col( q, 7 );
            n = name.empty() ? 0 : Intern( name );
            fn = Intern( Col( q, 8 ) );
            file = Intern( Col( q, 9 ) );
            auto old = d.Prep( "SELECT n FROM temp.e_sources WHERE kind=1 AND name=? AND function=? AND file=? AND line=? AND color=?" );
            Int( old, 1, n );
            Int( old, 2, fn );
            Int( old, 3, file );
            Int( old, 4, Val( q, 5 ) );
            Int( old, 5, Val( q, 6 ) );
            if( d.Step( old ) ) number = Val( old, 0 );
            else number = I( dynamicSource++ );
            sqlite3_reset( old );
        }
        Require( staticSource <= INT16_MAX && dynamicSource <= INT16_MAX, "Source location count exceeds standard snapshot limits" );
        auto put = d.Prep( "INSERT OR IGNORE INTO temp.e_sources(kind,n,key,name,function,file,line,color) VALUES(?,?,?,?,?,?,?,?)" );
        Int( put, 1, kind );
        Int( put, 2, uint64_t( number ) );
        Int( put, 3, kind == 0 ? key : 0 );
        Int( put, 4, n );
        Int( put, 5, fn );
        Int( put, 6, file );
        Int( put, 7, Val( q, 5 ) );
        Int( put, 8, Val( q, 6 ) );
        d.Run( put );
        put = d.Prep( "INSERT INTO temp.e_source_map VALUES(?,?,?)" );
        Int( put, 1, kind );
        Int( put, 2, key );
        Int( put, 3, uint64_t( kind == 0 ? number : -number - 1 ) );
        d.Run( put );
    } );
    // One ordered pass over zones. Text fragments (zone,event order) and child
    // counts (zone_parent index, parent order) are merged by zone id instead
    // of being looked up per zone. Per-thread totals are bounded by the
    // snapshot's 16-bit local thread space. A skeleton keeps the threads with
    // empty timelines.
    std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> zoneThreads;
    if( options.skeleton ) Each( "SELECT DISTINCT key FROM blocks WHERE stream=1", [&]( auto q ) { zoneThreads.emplace( uint64_t( Val( q, 0 ) ), std::pair<uint64_t, uint64_t>{} ); } );
    else
    {
        // Zones in id order across the per-thread block streams; the texts of a
        // zone come from its thread's text stream and the out-of-order table.
        std::vector<std::unique_ptr<ZoneStream>> streams;
        std::unordered_map<uint64_t, std::unique_ptr<TextStream>> texts;
        for( const auto stream : { BlockStream::CpuZones, BlockStream::ZoneText } )
        {
            auto q = d.Prep( "SELECT key,id FROM blocks WHERE stream=? ORDER BY key,first" );
            Int( q, 1, uint64_t( stream ) );
            uint64_t key = 0;
            std::vector<uint64_t> ids;
            const auto flush = [&] {
                if( ids.empty() ) return;
                if( stream == BlockStream::CpuZones ) streams.push_back( std::make_unique<ZoneStream>( d, blocks, key, std::move( ids ) ) );
                else texts.emplace( key, std::make_unique<TextStream>( d, blocks, std::move( ids ) ) );
                ids.clear();
            };
            while( d.Step( q ) )
            {
                const uint64_t k = sqlite3_column_int64( q, 0 );
                if( k != key ) flush();
                key = k;
                ids.push_back( sqlite3_column_int64( q, 1 ) );
            }
            sqlite3_reset( q );
            flush();
        }
        struct Head
        {
            uint64_t id;
            size_t stream;
            bool operator>( const Head& o ) const { return id > o.id; }
        };
        std::priority_queue<Head, std::vector<Head>, std::greater<Head>> heap;
        for( size_t n = 0; n < streams.size(); ++n )
            if( streams[n]->Next() ) heap.push( { streams[n]->row.id, n } );
        auto late = d.Prep( "SELECT zone,event,text FROM zone_late_text ORDER BY zone,event" );
        std::unordered_map<uint64_t, uint64_t> callsiteStacks;
        std::vector<std::pair<uint64_t, std::string>> parts;
        try
        {
            bool moreLate = d.Step( late );
            uint64_t nextExtra = 1;
            while( !heap.empty() )
            {
                const auto head = heap.top();
                heap.pop();
                auto& zs = *streams[head.stream];
                const auto& r = zs.row;
                Poll();
                const uint64_t id = r.id, thread = zs.thread, parent = r.parent;
                uint64_t rawStack = r.stack;
                if( !rawStack && r.callsite )
                {
                    auto found = callsiteStacks.find( r.callsite );
                    if( found == callsiteStacks.end() )
                    {
                        auto q = d.Prep( "SELECT stack FROM callsites WHERE id=?" );
                        Int( q, 1, r.callsite );
                        const uint64_t value = d.Step( q ) ? uint64_t( Val( q, 0 ) ) : 0;
                        sqlite3_reset( q );
                        found = callsiteStacks.emplace( r.callsite, value ).first;
                    }
                    rawStack = found->second;
                }
                const auto stack = Stack( rawStack );
                size_t count = 0;
                const auto part = [&]( uint64_t event, std::string_view value ) {
                    if( parts.size() == count ) parts.emplace_back();
                    parts[count].first = event;
                    parts[count].second.assign( value );
                    ++count;
                };
                if( auto found = texts.find( thread ); found != texts.end() )
                {
                    auto& ts = *found->second;
                    while( ts.valid && ts.zone < id ) ts.Next();
                    while( ts.valid && ts.zone == id )
                    {
                        part( ts.event, ts.text );
                        ts.Next();
                    }
                }
                while( moreLate && uint64_t( Val( late, 0 ) ) < id ) moreLate = d.Step( late );
                bool merged = false;
                while( moreLate && uint64_t( Val( late, 0 ) ) == id )
                {
                    const auto bytes = sqlite3_column_bytes( late, 2 );
                    part( uint64_t( Val( late, 1 ) ), std::string_view( reinterpret_cast<const char*>( sqlite3_column_blob( late, 2 ) ), size_t( bytes ) ) );
                    merged = true;
                    moreLate = d.Step( late );
                }
                if( merged ) std::sort( parts.begin(), parts.begin() + count, []( const auto& a, const auto& b ) { return a.first < b.first; } );
                const auto text = ZoneText( parts, count );
                const auto name = r.hasName ? Intern( r.name ) : 0;
                const auto color = r.hasColor ? uint64_t( r.color ) : 0;
                uint64_t extra = 0;
                if( stack || text || name || r.hasColor )
                {
                    extra = nextExtra++;
                    Require( extra <= UINT32_MAX, "Zone extra limit exceeded" );
                    auto put = d.Prep( "INSERT INTO temp.e_extra VALUES(?,?,?,?,?)" );
                    Int( put, 1, extra );
                    Int( put, 2, stack );
                    Int( put, 3, text );
                    Int( put, 4, name );
                    Int( put, 5, color );
                    d.Run( put );
                }
                const uint64_t children = r.children;
                const auto source = Source( int( r.skind ), r.skey );
                auto put = d.Prep( "INSERT INTO temp.e_cpu VALUES(?,?,?,?,?,?,?,?)" );
                Int( put, 1, thread );
                Int( put, 2, id );
                Int( put, 3, parent );
                Int( put, 4, uint64_t( r.start ) );
                Int( put, 5, r.open ? uint64_t( -1 ) : uint64_t( r.end ) );
                Int( put, 6, uint64_t( source ) );
                Int( put, 7, extra );
                Int( put, 8, children );
                d.Run( put );
                ++cpuSourceZones[size_t( int( source ) + 32768 )];
                if( children ) ++cpuParents;
                auto& totals = zoneThreads[thread];
                Require( zoneThreads.size() <= 65536, "Too many local thread identities for snapshot" );
                ++totals.first;
                if( !parent ) ++totals.second;
                if( zs.Next() ) heap.push( { zs.row.id, head.stream } );
            }
            extraRows = nextExtra;
        }
        catch( ... )
        {
            sqlite3_reset( late );
            throw;
        }
        sqlite3_reset( late );
    }
    for( const auto& [thread, totals] : zoneThreads )
    {
        auto put = d.Prep( "INSERT INTO temp.e_threads(id,zones,roots) VALUES(?,?,?)" );
        Int( put, 1, thread );
        Int( put, 2, totals.first );
        Int( put, 3, totals.second );
        d.Run( put );
    }
    d.Exec( "INSERT OR IGNORE INTO temp.e_threads(id) SELECT thread FROM messages WHERE app=0 UNION SELECT thread FROM samples UNION SELECT thread FROM sample_pending UNION SELECT allocation_thread FROM allocations UNION SELECT free_thread FROM allocations WHERE freed IS NOT NULL UNION SELECT id FROM fibers UNION SELECT related FROM context_regions WHERE reason=99" );
    d.Exec( "UPDATE temp.e_threads SET fiber=EXISTS(SELECT 1 FROM fibers f WHERE f.id=e_threads.id),hint=COALESCE((SELECT group_hint FROM fibers f WHERE f.id=e_threads.id),(SELECT hint FROM thread_hints h WHERE h.thread=e_threads.id),0)" );
    Each( "SELECT s.thread,k.data FROM samples s JOIN stacks k ON k.id=s.stack", [&]( auto q ) {
        Require( sqlite3_column_bytes( q, 1 ) >= 8, "Empty sampling stack" );
        uint64_t ip;
        memcpy( &ip, sqlite3_column_blob( q, 1 ), 8 );
        if( ip >> 63 )
        {
            auto put = d.Prep( "UPDATE temp.e_threads SET kernel=kernel+1 WHERE id=?" );
            Int( put, 1, Val( q, 0 ) );
            d.Run( put );
        }
    } );
    d.Exec( "INSERT INTO temp.e_local VALUES(0,0)" );
    ordinal = 1;
    Each( "SELECT id FROM e_threads UNION SELECT thread FROM lock_threads UNION SELECT thread FROM gpu_zones WHERE thread!=0 ORDER BY 1", [&]( auto q ) {
        const uint64_t id = Val( q, 0 );
        if( !id ) return;
        auto put = d.Prep( "INSERT OR IGNORE INTO temp.e_local VALUES(?,?)" );
        Int( put, 1, id );
        Int( put, 2, ordinal );
        d.Run( put );
        if( sqlite3_changes( d.handle ) ) ++ordinal;
    } );
    Require( ordinal <= 65536, "Too many local thread identities for snapshot" );
    ordinal = 0;
    Each( "SELECT DISTINCT thread FROM cpu_regions ORDER BY thread", [&]( auto q ) {
        auto put = d.Prep( "INSERT INTO temp.e_external VALUES(?,?)" );
        Int( put, 1, Val( q, 0 ) );
        Int( put, 2, ordinal++ );
        d.Run( put );
    } );
    Require( ordinal <= 65536, "Too many external thread identities for snapshot" );
    if( !options.skeleton )
    {
        auto children = d.Prep( "SELECT parent,COUNT(*) FROM gpu_zones WHERE parent>0 GROUP BY parent ORDER BY parent" );
        try
        {
            bool moreChildren = d.Step( children );
            Each( "SELECT z.id,z.context,z.thread,z.parent,z.cpu_start,z.cpu_end,z.gpu_start,z.gpu_end,z.skind,z.skey,CASE WHEN z.stack!=0 THEN z.stack ELSE COALESCE(c.stack,0) END,z.query FROM gpu_zones z LEFT JOIN callsites c ON c.id=z.callsite ORDER BY z.id", [&]( auto q ) {
                const uint64_t id = Val( q, 0 );
                while( moreChildren && uint64_t( Val( children, 0 ) ) < id ) moreChildren = d.Step( children );
                uint64_t count = 0;
                if( moreChildren && uint64_t( Val( children, 0 ) ) == id )
                {
                    count = uint64_t( Val( children, 1 ) );
                    moreChildren = d.Step( children );
                }
                const auto field = [&]( int n ) { return Null( q, n ) ? uint64_t( -1 ) : uint64_t( Val( q, n ) ); };
                const auto source = Source( int( Val( q, 8 ) ), Val( q, 9 ) );
                auto put = d.Prep( "INSERT INTO temp.e_gpu VALUES(?,?,?,?,?,?,?,?,?,?,?,?)" );
                Int( put, 1, field( 1 ) );
                Int( put, 2, field( 2 ) );
                Int( put, 3, field( 0 ) );
                Int( put, 4, field( 3 ) );
                Int( put, 5, field( 4 ) );
                Int( put, 6, field( 5 ) );
                Int( put, 7, field( 6 ) );
                Int( put, 8, field( 7 ) );
                Int( put, 9, uint64_t( source ) );
                Int( put, 10, Stack( Val( q, 10 ) ) );
                Int( put, 11, Val( q, 11 ) );
                Int( put, 12, count );
                d.Run( put );
                ++gpuSourceZones[size_t( int( source ) + 32768 )];
                if( count ) ++gpuParents;
            } );
        }
        catch( ... )
        {
            sqlite3_reset( children );
            throw;
        }
        sqlite3_reset( children );
    }
    // Intern every field used by later sections before the dictionary is written.
    for( const auto& sql : { "SELECT name FROM locks WHERE name IS NOT NULL", "SELECT text FROM messages WHERE text IS NOT NULL", "SELECT name FROM gpu_contexts", "SELECT name FROM gpu_notes", "SELECT name FROM plots", "SELECT name FROM stackframes", "SELECT file FROM stackframes", "SELECT image FROM stackframes", "SELECT name FROM symbol_resources", "SELECT file FROM symbol_resources", "SELECT image FROM symbol_resources", "SELECT call_file FROM symbol_resources" } ) Each( sql, [&]( auto q ) {
        Intern( Col( q, 0 ) );
    } );
    Each("SELECT name_key FROM parameters",[&](auto q){if(!RawString(Val(q,0)))Synthetic(Intern("???"));});
    Each( "SELECT id,name,kind FROM plots WHERE kind!=1", [&]( auto q ) {
        const auto key = Val(q,2)==2?0:Synthetic( Intern( Col( q, 1 ) ) );
        auto put = d.Prep( "INSERT INTO temp.e_plots VALUES(?,?)" );
        Int( put, 1, Val( q, 0 ) );
        Int( put, 2, key );
        d.Run( put );
    } );
    Require( uint64_t( d.Scalar( "PRAGMA temp.page_count" ) ) * uint64_t( d.Scalar( "PRAGMA temp.page_size" ) ) <= options.temporaryBytes, "Export staging exceeds disk budget" );
}
void SnapshotExport::Header()
{
    Stage( "Capture and frames" );
    const uint8_t header[] = { 't', 'r', 'a', 'c', 'y', Version::Major, Version::Minor, Version::Patch };
    Raw( header );
    const double resolution = double( capture.resolution ) * capture.timerMultiplier;
    Require( std::isfinite( resolution ) && resolution >= 0 && resolution < double( INT64_MAX ), "Resolution overflow" );
    I64( I( resolution ) );
    Put( capture.timerMultiplier );
    I64( info.lastNs );
    U64( capture.frameOffset );
    U64( capture.pid );
    I64( capture.samplingPeriod );
    U8( capture.architecture );
    U32( capture.cpuId );
    Raw( { reinterpret_cast<const uint8_t*>( capture.cpuManufacturer.data() ), 12 } );
    U8( bool( capture.flags & WelcomeFlag::OnDemand ) );
    char date[64]{};
    time_t epoch = time_t( capture.epoch );
    tm local{};
    Require( localtime_s( &local, &epoch ) == 0, "Capture date cannot be represented" );
    strftime( date, sizeof( date ), "%Y-%m-%d %H:%M:%S", &local );
    String64( capture.program + " @ " + date );
    String64( capture.program );
    U64( capture.epoch );
    U64( capture.executableTime );
    String64( capture.host );
    U64( uint64_t( d.Scalar( "SELECT COUNT(DISTINCT package) FROM cpu_topology" ) ) );
    Each( "SELECT DISTINCT package FROM cpu_topology ORDER BY package", [&]( auto p ) {
        const uint32_t package = uint32_t( Val( p, 0 ) );
        U32( package );
        U64( uint64_t( d.Scalar( "SELECT COUNT(DISTINCT die) FROM cpu_topology WHERE package=" + std::to_string( package ) ) ) );
        Each( "SELECT DISTINCT die FROM cpu_topology WHERE package=" + std::to_string( package ) + " ORDER BY die", [&]( auto di ) {
            const uint32_t die = uint32_t( Val( di, 0 ) );
            U32( die );
            const auto where = " WHERE package=" + std::to_string( package ) + " AND die=" + std::to_string( die );
            U64( uint64_t( d.Scalar( "SELECT COUNT(DISTINCT core) FROM cpu_topology" + where ) ) );
            Each( "SELECT DISTINCT core FROM cpu_topology" + where + " ORDER BY core", [&]( auto co ) {
                const uint32_t core = uint32_t( Val( co, 0 ) );
                U32( core );
                CountVector( "SELECT thread FROM cpu_topology" + where + " AND core=" + std::to_string( core ) + " ORDER BY thread", [&]( auto th ) {
                    U32( uint32_t( Val( th, 0 ) ) );
                } );
            } );
        } );
    } );
    auto crash = d.Prep( "SELECT thread,time,message,stack FROM crash" );
    if( d.Step( crash ) )
    {
        U64( Val( crash, 0 ) );
        I64( Val( crash, 1 ) );
        U64( Val( crash, 2 ) );
        U32( uint32_t( Stack( Val( crash, 3 ) ) ) );
    }
    else
    {
        U64( 0 );
        I64( 0 );
        U64( 0 );
        U32( 0 );
    }
    sqlite3_reset( crash );
    U64( Count( "frame_state" ) );
    Each( "SELECT setid,continuous FROM frame_state ORDER BY CASE WHEN setid=0 THEN 0 ELSE 1 END,setid", [&]( auto q ) {
        const uint64_t set = Val( q, 0 );
        const bool continuous = Val( q, 1 ) != 0;
        U64( set );
        U8( continuous );
        I reference = 0;
        CountVector( "SELECT f.start,f.end,(SELECT i.ordinal FROM base_frames b JOIN frame_images i ON i.frame_index=b.ordinal WHERE b.frame=f.id) FROM frames f WHERE setid=" + std::to_string( I( set ) ) + " ORDER BY id", [&]( auto row ) {
            Delta( reference, Val( row, 0 ) );
            if( !continuous ) Delta( reference, Null( row, 1 ) ? -1 : Val( row, 1 ) );
            Put( int32_t( Null( row, 2 ) ? -1 : Val( row, 2 ) ) );
        } );
    } );
}
void SnapshotExport::Strings()
{
    Stage( "Strings and thread mappings" );
    U64( Count( "temp.e_strings" ) );
    // Strings of at most 65535 bytes are stored whole in e_strings.value;
    // only longer zone texts are read back from their e_chunks parts.
    Each( "SELECT id,total,value FROM temp.e_strings ORDER BY id", [&]( auto q ) {
        const uint64_t id = Val( q, 0 );
        U64( id );
        U64( Val( q, 1 ) );
        if( !Null( q, 2 ) )
        {
            SqlBlob( q, 2 );
            return;
        }
        auto parts = d.Prep( "SELECT data FROM temp.e_chunks WHERE id=? ORDER BY part" );
        Int( parts, 1, id );
        try
        {
            while( d.Step( parts ) )
            {
                Poll();
                SqlBlob( parts, 0 );
            }
        }
        catch( ... )
        {
            sqlite3_reset( parts );
            throw;
        }
        sqlite3_reset( parts );
    } );
    CountVector( "SELECT key,sid FROM temp.e_raw ORDER BY key", [&]( auto q ) {
        U64( Val( q, 0 ) );
        U64( Val( q, 1 ) );
    } );
    const auto threadKind = std::to_string( uint8_t( QueueType::ThreadName ) ), fiberKind = std::to_string( uint8_t( QueueType::FiberName ) );
    CountVector( "SELECT key,sid FROM temp.e_names WHERE kind=" + threadKind + " UNION ALL SELECT f.id,n.sid FROM fibers f JOIN temp.e_names n ON n.kind=" + fiberKind + " AND n.key=f.key", [&]( auto q ) {
        U64( Val( q, 0 ) );
        U64( Val( q, 1 ) );
    } );
    const auto external = std::to_string( uint8_t( QueueType::ExternalName ) ), externalThread = std::to_string( uint8_t( QueueType::ExternalThreadName ) );
    CountVector( "SELECT a.key,a.sid,COALESCE(b.sid,1) FROM temp.e_names a LEFT JOIN temp.e_names b ON b.key=a.key AND b.kind=" + externalThread + " WHERE a.kind=" + external, [&]( auto q ) {
        U64( Val( q, 0 ) );
        U64( Val( q, 1 ) );
        U64( Val( q, 2 ) );
    } );
    CountVector( "SELECT id FROM temp.e_local ORDER BY n", [&]( auto q ) {
        U64( Val( q, 0 ) );
    } );
    CountVector( "SELECT id FROM temp.e_external ORDER BY n", [&]( auto q ) {
        U64( Val( q, 0 ) );
    } );
}
void SnapshotExport::Sources()
{
    Stage( "Source locations" );
    const auto source = [&]( sqlite3_stmt* q, bool allocated ) {
        const uint64_t name = Val( q, 0 ), function = Val( q, 1 ), file = Val( q, 2 );
        Ref( allocated ? ( name ? name - 1 : 0 ) : name, allocated, name != 0 );
        Ref( allocated ? function - 1 : function, allocated );
        Ref( allocated ? file - 1 : file, allocated );
        U32( uint32_t( Val( q, 3 ) ) );
        U32( uint32_t( Val( q, 4 ) ) );
    };
    U64( uint64_t( d.Scalar( "SELECT COUNT(*) FROM temp.e_sources WHERE kind=0" ) ) );
    Each( "SELECT key,name,function,file,line,color FROM temp.e_sources WHERE kind=0 ORDER BY n", [&]( auto q ) {
        U64( Val( q, 0 ) );
        const auto name = uint64_t( Val( q, 1 ) );
        Ref( name, false, name != 0 );
        Ref( Val( q, 2 ), false );
        Ref( Val( q, 3 ), false );
        U32( uint32_t( Val( q, 4 ) ) );
        U32( uint32_t( Val( q, 5 ) ) );
    } );
    U64( uint64_t( d.Scalar( "SELECT COUNT(*) FROM temp.e_sources WHERE kind=0" ) ) + 1 );
    U64( 0 );
    Each( "SELECT key FROM temp.e_sources WHERE kind=0 ORDER BY n", [&]( auto q ) {
        U64( Val( q, 0 ) );
    } );
    U64( uint64_t( d.Scalar( "SELECT COUNT(*) FROM temp.e_sources WHERE kind=1" ) ) );
    Each( "SELECT name,function,file,line,color FROM temp.e_sources WHERE kind=1 ORDER BY n", [&]( auto q ) {
        source( q, true );
    } );
    // Zone counts per source in ascending source order, as GROUP BY source
    // returned them; the counts were gathered while staging the zones.
    for( const auto* zones : { &cpuSourceZones, &gpuSourceZones } )
    {
        U64( uint64_t( std::count_if( zones->begin(), zones->end(), []( uint64_t n ) { return n != 0; } ) ) );
        for( size_t n = 0; n < zones->size(); ++n )
        {
            if( !( *zones )[n] ) continue;
            Put( int16_t( int( n ) - 32768 ) );
            U64( ( *zones )[n] );
        }
    }
}
void SnapshotExport::Locks()
{
    Stage( "Locks" );
    U64( Count( "locks" ) );
    Each( "SELECT id,name,source,type,announce,terminate FROM locks ORDER BY id", [&]( auto q ) {
        const uint64_t id = Val( q, 0 );
        U32( uint32_t( id ) );
        U24( Null( q, 1 ) ? 0 : Intern( Col( q, 1 ) ) );
        Put( Source( 0, Val( q, 2 ) ) );
        U8( uint8_t( Val( q, 3 ) ) );
        U8( 1 );
        const I announce = Val( q, 4 );
        I64( announce );
        I64( Null( q, 5 ) ? 0 : Val( q, 5 ) );
        CountVector( "SELECT thread FROM lock_threads WHERE lock=" + std::to_string( id ) + " ORDER BY ordinal", [&]( auto row ) {
            U64( Val( row, 0 ) );
        } );
        I time = announce;
        CountVector( "SELECT e.time,e.source,t.ordinal,e.type FROM lock_events e JOIN lock_threads t ON t.lock=e.lock AND t.thread=e.thread WHERE e.lock=" + std::to_string( id ) + " ORDER BY e.id", [&]( auto row ) {
            Delta( time, Val( row, 0 ) );
            Put( Source( 0, Val( row, 1 ) ) );
            U8( uint8_t( Val( row, 2 ) ) );
            U8( uint8_t( Val( row, 3 ) ) );
        } );
    } );
}
void SnapshotExport::Messages()
{
    Stage( "Messages and zone extras" );
    I time = 0;
    CountVector( "SELECT id,time,text,string_key,color,stack FROM messages WHERE app=0 ORDER BY time,id", [&]( auto q ) {
        U64( Val( q, 0 ) );
        Delta( time, Val( q, 1 ) );
        if( Null( q, 2 ) ) Ref( Val( q, 3 ), false );
        else Ref( Intern( Col( q, 2 ) ) - 1, true );
        U32( uint32_t( Val( q, 4 ) ) );
        U24( Stack( Val( q, 5 ) ) );
    } );
    U64( extraRows );
    Each( "SELECT stack,text,name,color FROM temp.e_extra ORDER BY n", [&]( auto q ) {
        for( int n = 0; n < 4; ++n ) U24( Val( q, n ) );
    } );
}
void SnapshotExport::Timeline( uint64_t owner, uint64_t lane, bool gpu, uint64_t roots )
{
    // A timeline is serialized in preorder: node header, child count, the
    // children, then the node's end. Zone ids follow begin order, so within
    // one owner they are already a preorder; one ordered range scan visits
    // every node and the open ancestors are closed between rows.
    if( gpu ) U64( roots );
    else
    {
        Require( roots <= UINT32_MAX, "CPU timeline root count overflow" );
        U32( uint32_t( roots ) );
    }
    auto q = gpu ? d.Prep( "SELECT id,parent,children,source,cpu_start,gpu_start,stack,cpu_end,gpu_end,query FROM temp.e_gpu WHERE context=? AND thread=? ORDER BY id" ) : d.Prep( "SELECT id,parent,children,source,start,extra,end FROM temp.e_cpu WHERE thread=? ORDER BY id" );
    Int( q, 1, owner );
    if( gpu ) Int( q, 2, lane );
    I cpuTime = 0, gpuTime = 0;
    uint64_t visited = 0, seenRoots = 0;
    std::optional<uint16_t> laneThread;
    const auto tail = [&]( I end, I gpuEnd, uint64_t query ) {
        Delta( cpuTime, end );
        if( gpu )
        {
            Delta( gpuTime, gpuEnd );
            U16( uint16_t( query ) );
        }
    };
    const auto close = [&] {
        const auto node = PopOpen();
        Require( node.remaining == 0, "Timeline child count mismatch" );
        tail( node.end, node.gpuEnd, node.query );
    };
    openNodes.clear();
    Require( openSpilled == 0, "Timeline depth stack is not empty" );
    try
    {
        while( d.Step( q ) )
        {
            Poll();
            const uint64_t id = Val( q, 0 ), parent = Val( q, 1 ), children = Val( q, 2 );
            while( HasOpen() && InnerOpen().id != parent ) close();
            if( parent )
            {
                Require( HasOpen(), "Timeline parent order mismatch" );
                auto& open = InnerOpen();
                Require( open.remaining != 0, "Timeline child count mismatch" );
                --open.remaining;
            }
            else ++seenRoots;
            ++visited;
            Require( visited <= uint64_t( gpu ? info.gpuZones : info.cpuZones ), "Timeline cycle detected" );
            I end, gpuEnd = 0;
            uint64_t query = 0;
            if( gpu )
            {
                Delta( cpuTime, Val( q, 4 ) );
                Delta( gpuTime, Val( q, 5 ) );
                Put( int16_t( Val( q, 3 ) ) );
                U24( Val( q, 6 ) );
                // Every node of a GPU lane carries that lane's thread.
                if( !laneThread ) laneThread = Thread( lane );
                U16( *laneThread );
                U64( children );
                end = Val( q, 7 );
                gpuEnd = Val( q, 8 );
                query = Val( q, 9 );
            }
            else
            {
                Put( int16_t( Val( q, 3 ) ) );
                Delta( cpuTime, Val( q, 4 ) );
                U32( uint32_t( Val( q, 5 ) ) );
                Require( children <= UINT32_MAX, "CPU child count overflow" );
                U32( uint32_t( children ) );
                end = Val( q, 6 );
            }
            if( children ) PushOpen( { id, children, end, gpuEnd, query } );
            else tail( end, gpuEnd, query );
        }
        while( HasOpen() ) close();
    }
    catch( ... )
    {
        sqlite3_reset( q );
        throw;
    }
    sqlite3_reset( q );
    Require( seenRoots == roots, "Timeline root count mismatch" );
}
void SnapshotExport::Cpu()
{
    Stage( "CPU timelines" );
    U64( options.skeleton ? 0 : info.cpuZones );
    U64( cpuParents );
    U64( Count( "temp.e_threads" ) );
    Each( "SELECT id,fiber,hint,kernel,zones,roots FROM temp.e_threads ORDER BY id", [&]( auto q ) {
        const uint64_t thread = Val( q, 0 );
        const auto where = " WHERE thread=" + std::to_string( I( thread ) );
        U64( thread );
        U64( Val( q, 4 ) );
        U64( Val( q, 3 ) );
        U8( uint8_t( Val( q, 1 ) ) );
        Put( int32_t( Val( q, 2 ) ) );
        Timeline( thread, 0, false, uint64_t( Val( q, 5 ) ) );
        CountVector( "SELECT id FROM messages" + where + " AND app=0 ORDER BY time,id", [&]( auto row ) {
            U64( Val( row, 0 ) );
        } );
        I reference = 0;
        CountVector( "SELECT time,stack FROM samples" + where + " AND (explicit_context=1 OR EXISTS(SELECT 1 FROM context_regions c WHERE c.thread=samples.thread AND c.start=samples.time AND c.reason!=-2)) ORDER BY time,id", [&]( auto row ) {
            Delta( reference, Val( row, 0 ) );
            U24( Stack( Val( row, 1 ) ) );
        } );
        reference = 0;
        CountVector( "SELECT time,stack FROM samples" + where + " ORDER BY time,id", [&]( auto row ) {
            Delta( reference, Val( row, 0 ) );
            U24( Stack( Val( row, 1 ) ) );
        } );
    } );
}
void SnapshotExport::Gpu()
{
    Stage( "GPU timelines" );
    U64( options.skeleton ? 0 : info.gpuZones );
    U64( gpuParents );
    U64( Count( "gpu_contexts" ) );
    Each( "SELECT id,thread,calibrated,period,type,name,overflow FROM gpu_contexts ORDER BY sequence", [&]( auto q ) {
        const uint64_t context = Val( q, 0 );
        U64( Val( q, 1 ) );
        U8( uint8_t( Val( q, 2 ) ) );
        U64( uint64_t( d.Scalar( "SELECT COUNT(*) FROM temp.e_gpu WHERE context=" + std::to_string( context ) + " AND gpu_end!=-1" ) ) );
        Put( float( sqlite3_column_double( q, 3 ) ) );
        U8( uint8_t( Val( q, 4 ) ) );
        const auto name = Col( q, 5 );
        U24( name.empty() ? 0 : Intern( name ) );
        U64( Val( q, 6 ) );
        CountVector( "SELECT note,name FROM gpu_notes WHERE context=" + std::to_string( context ) + " ORDER BY id", [&]( auto row ) {
            I64( Val( row, 0 ) );
            U24( Intern( Col( row, 1 ) ) );
        } );
        CountVector( "SELECT DISTINCT thread FROM temp.e_gpu WHERE context=" + std::to_string( context ) + " ORDER BY thread", [&]( auto row ) {
            const uint64_t thread = Val( row, 0 );
            U64( thread );
            auto roots = d.Prep( "SELECT COUNT(*) FROM temp.e_gpu WHERE context=? AND thread=? AND parent=0" );
            Int( roots, 1, context );
            Int( roots, 2, thread );
            Require( d.Step( roots ), "Missing SQL scalar" );
            const uint64_t count = Val( roots, 0 );
            sqlite3_reset( roots );
            Timeline( context, thread, true, count );
        } );
        CountVector( "SELECT DISTINCT query FROM gpu_annotations WHERE context=" + std::to_string( context ) + " ORDER BY query", [&]( auto row ) {
            const auto query = uint16_t( Val( row, 0 ) );
            U16( query );
            CountVector( "SELECT note,value FROM gpu_annotations WHERE context=" + std::to_string( context ) + " AND query=" + std::to_string( query ) + " ORDER BY id", [&]( auto annotation ) {
                I64( Val( annotation, 0 ) );
                Require( sqlite3_column_bytes( annotation, 1 ) == 8, "Invalid GPU annotation" );
                SqlBlob( annotation, 1 );
            } );
        } );
    } );
}
void SnapshotExport::Plots()
{
    Stage( "Plots" );
    U64( Count( "temp.e_plots" ) );
    Each( "SELECT p.id,p.kind,p.format,p.steps,p.fill,p.color,k.key,p.min,p.max,p.sum FROM plots p JOIN temp.e_plots k ON k.id=p.id ORDER BY p.id", [&]( auto q ) {
        const uint64_t id = Val( q, 0 );
        U8( uint8_t( Val( q, 1 ) ) );
        U8( uint8_t( Val( q, 2 ) ) );
        U8( uint8_t( Val( q, 3 ) ) );
        U8( uint8_t( Val( q, 4 ) ) );
        U32( uint32_t( Val( q, 5 ) ) );
        U64( Val( q, 6 ) );
        Put( sqlite3_column_type(q,7)==SQLITE_BLOB?StoredDouble(q,7):sqlite3_column_double(q,7) );
        Put( sqlite3_column_type(q,8)==SQLITE_BLOB?StoredDouble(q,8):sqlite3_column_double(q,8) );
        Require( sqlite3_column_bytes( q, 9 ) == 8, "Invalid plot sum" );
        SqlBlob( q, 9 );
        I reference = 0;
        CountVector( "SELECT time,value FROM plot_points WHERE plot=" + std::to_string( id ) + " ORDER BY time,id", [&]( auto row ) {
            Delta( reference, Val( row, 0 ) );
            Require( sqlite3_column_bytes( row, 1 ) == 8, "Invalid plot point" );
            SqlBlob( row, 1 );
        } );
    } );
}
void SnapshotExport::Memory()
{
    Stage( "Memory" );
    U64( Count( "memory_pools" ) );
    U64( info.allocations );
    Each( "SELECT id,active,frees,high,low,usage FROM memory_pools ORDER BY event,id", [&]( auto q ) {
        const uint64_t pool = Val( q, 0 );
        U64( pool );
        U64( uint64_t( d.Scalar( "SELECT COUNT(*) FROM allocations WHERE pool=" + std::to_string( I( pool ) ) ) ) );
        U64( Val( q, 1 ) );
        U64( Val( q, 2 ) );
        I reference = 0;
        Each( "SELECT address,size,allocation_stack,free_stack,allocated,freed,allocation_thread,free_thread FROM allocations WHERE pool=" + std::to_string( I( pool ) ) + " ORDER BY id", [&]( auto row ) {
            U64( Val( row, 0 ) );
            U64( Val( row, 1 ) );
            U24( Stack( Val( row, 2 ) ) );
            U24( Stack( Val( row, 3 ) ) );
            const I allocated = Val( row, 4 ), freed = Null( row, 5 ) ? -1 : Val( row, 5 );
            Delta( reference, allocated );
            I64( freed < 0 ? freed : Sub( freed, allocated ) );
            U16( Thread( Val( row, 6 ) ) );
            U16( Thread( Val( row, 7 ) ) );
        } );
        const auto high = Col( q, 3 ), low = Col( q, 4 );
        U64( high.empty() ? 0 : std::stoull( high, nullptr, 16 ) );
        U64( low.empty() ? UINT64_MAX : std::stoull( low, nullptr, 16 ) );
        Require( sqlite3_column_bytes( q, 5 ) == 8, "Invalid allocator usage" );
        SqlBlob( q, 5 );
        U64( pool );
    } );
}
void SnapshotExport::Stacks()
{
    Stage( "Callstacks" );
    U64( Count( "stacks" ) );
    Each( "SELECT s.data FROM stacks s JOIN temp.e_stackids m ON m.id=s.id ORDER BY m.n", [&]( auto q ) {
        const auto n = sqlite3_column_bytes( q, 0 );
        Require( n % 8 == 0 && n / 8 <= 65535, "Snapshot callstack size limit exceeded" );
        U16( uint16_t( n / 8 ) );
        const auto data = static_cast<const uint8_t*>( sqlite3_column_blob( q, 0 ) );
        for( int offset = 0; offset < n; offset += 8 )
        {
            uint64_t address;
            memcpy( &address, data + offset, 8 );
            const bool managed = ( address & ( 1ull << 62 ) ) && !( address & ( 1ull << 63 ) );
            U64( managed ? address : address & 0x3fffffffffffffffull );
        }
    } );
    CountVector( "SELECT key,COUNT(*),MIN(image) FROM stackframes GROUP BY key ORDER BY key", [&]( auto q ) {
        const uint64_t key = Val( q, 0 );
        const bool managed = ( key & ( 1ull << 62 ) ) && !( key & ( 1ull << 63 ) );
        U64( managed ? key : key & 0x3fffffffffffffffull );
        Require( Val( q, 1 ) <= 255, "Snapshot inline frame count exceeded" );
        U8( uint8_t( Val( q, 1 ) ) );
        U24( managed ? 0 : Intern( Col( q, 2 ) ) );
        Each( "SELECT name,file,line,symbol FROM stackframes WHERE key=" + std::to_string( I( key ) ) + " ORDER BY ordinal", [&]( auto row ) {
            U24( Intern( Col( row, 0 ) ) );
            U24( Intern( Col( row, 1 ) ) );
            U32( uint32_t( Val( row, 2 ) ) );
            U64( Val( row, 3 ) );
        } );
    } );
    CountVector( "SELECT text FROM messages WHERE app=1 ORDER BY id", [&]( auto q ) {
        Ref( Intern( Col( q, 0 ) ) - 1, true );
    } );
}
void SnapshotExport::Images()
{
    Stage( "Frame images" );
    U32( 0 );
    U64( Count( "frame_images" ) );
    Each( "SELECT width,height,flipped,data FROM frame_images ORDER BY ordinal", [&]( auto q ) {
        U16( uint16_t( Val( q, 0 ) ) );
        U16( uint16_t( Val( q, 1 ) ) );
        U8( uint8_t( Val( q, 2 ) ) );
        Require( uint64_t( sqlite3_column_bytes( q, 3 ) ) == uint64_t( Val( q, 0 ) ) * uint64_t( Val( q, 1 ) ) / 2, "Frame image dimensions differ from data" );
        SqlBlob( q, 3 );
    } );
}
void SnapshotExport::Scheduling()
{
    Stage( "Scheduling" );
    CountVector( "SELECT DISTINCT r.thread FROM context_regions r JOIN temp.e_threads t ON t.id=r.thread WHERE EXISTS(SELECT 1 FROM blocks b WHERE b.stream=1 AND b.key=t.id) OR EXISTS(SELECT 1 FROM samples s WHERE s.thread=t.id) ORDER BY r.thread", [&]( auto q ) {
        const uint64_t thread = Val( q, 0 );
        U64( thread );
        I reference = 0;
        CountVector( "SELECT wakeup,start,end,cpu,reason,state,related,wake_cpu FROM context_regions WHERE thread=" + std::to_string( I( thread ) ) + " ORDER BY id", [&]( auto row ) {
            Delta( reference, Null( row, 0 ) ? 0 : Val( row, 0 ) );
            Delta( reference, Val( row, 1 ) );
            Delta( reference, Null( row, 2 ) ? -1 : Val( row, 2 ) );
            U8( uint8_t( Val( row, 3 ) ) );
            Put( int8_t( Val( row, 4 ) ) );
            Put( int8_t( Val( row, 5 ) ) );
            U64( Val( row, 6 ) );
            U8( uint8_t( Val( row, 7 ) ) );
        } );
    } );
    U64( Count( "cpu_regions" ) );
    for( int cpu = 0; cpu < 256; ++cpu )
    {
        I reference = 0;
        CountVector( "SELECT start,end,thread FROM cpu_regions WHERE cpu=" + std::to_string( cpu ) + " ORDER BY id", [&]( auto q ) {
            Delta( reference, Val( q, 0 ) );
            Delta( reference, Null( q, 1 ) ? -1 : Val( q, 1 ) );
            U16( Thread( Val( q, 2 ), true ) );
        } );
    }
    CountVector( "SELECT thread,pid FROM thread_process ORDER BY thread", [&]( auto q ) {
        U64( Val( q, 0 ) );
        U64( Val( q, 1 ) );
    } );
    CountVector( "SELECT thread,running,migrations,(SELECT COUNT(*) FROM context_regions c WHERE c.thread=t.thread AND c.reason!=99 AND c.end IS NOT NULL) FROM context_threads t WHERE EXISTS(SELECT 1 FROM context_regions c WHERE c.thread=t.thread AND c.reason!=99 AND c.end IS NOT NULL) OR migrations>0 ORDER BY thread", [&]( auto q ) {
        U64( Val( q, 0 ) );
        I64( Val( q, 1 ) );
        U32( uint32_t( Val( q, 3 ) ) );
        U32( uint32_t( Val( q, 2 ) ) );
    } );
}
void SnapshotExport::Code()
{
    Stage( "Symbols and code" );
    U64( uint64_t( d.Scalar( "SELECT COUNT(*) FROM symbol_resources WHERE inlined=0" ) ) );
    U64( uint64_t( d.Scalar( "SELECT COUNT(*) FROM symbol_resources WHERE inlined!=0" ) ) );
    U64( Count( "symbol_resources" ) );
    Each( "SELECT address,name,file,line,image,call_file,call_line,inlined,size FROM symbol_resources ORDER BY id", [&]( auto q ) {
        U64( Val( q, 0 ) );
        U24( Intern( Col( q, 1 ) ) );
        U24( Intern( Col( q, 2 ) ) );
        U32( uint32_t( Val( q, 3 ) ) );
        U24( Intern( Col( q, 4 ) ) );
        U24( Intern( Col( q, 5 ) ) );
        U32( uint32_t( Val( q, 6 ) ) );
        U8( uint8_t( Val( q, 7 ) ) );
        U24( Val( q, 8 ) );
    } );
    CountVector( "SELECT address,data FROM symbol_resources WHERE status=2 ORDER BY id", [&]( auto q ) {
        U64( Val( q, 0 ) );
        U32( uint32_t( sqlite3_column_bytes( q, 1 ) ) );
        SqlBlob( q, 1 );
    } );
    CountVector( "SELECT key,symbol FROM stackframes WHERE ordinal=0 AND key NOT IN(SELECT (4611686018427387904 | id) FROM custom_frames) ORDER BY key", [&]( auto q ) {
        U64( Val( q, 0 ) );
        U64( Val( q, 1 ) );
    } );
    U64( Count( "hardware_addresses" ) );
    Each( "SELECT address FROM hardware_addresses ORDER BY id", [&]( auto q ) {
        const uint64_t address = Val( q, 0 );
        U64( address );
        for( int kind = 0; kind < 6; ++kind )
        {
            I reference = 0;
            CountVector( "SELECT time FROM hardware_samples WHERE address=" + std::to_string( I( address ) ) + " AND kind=" + std::to_string( kind ) + " ORDER BY time,id", [&]( auto row ) {
                Delta( reference, Val( row, 0 ) );
            } );
        }
    } );
    CountVector( "SELECT path,data FROM source_resources WHERE status=2 ORDER BY id", [&]( auto q ) {
        U32( uint32_t( sqlite3_column_bytes( q, 0 ) ) );
        SqlBlob( q, 0 );
        U32( uint32_t( sqlite3_column_bytes( q, 1 ) ) );
        SqlBlob( q, 1 );
    } );
}
void SnapshotExport::Jn()
{
    Stage( "JN records" );
    U32( JnTraceSectionMagic );
    U16( JnTraceSchemaVersion );
    U8( d.Scalar( "SELECT EXISTS(SELECT 1 FROM blocks WHERE stream=3)" ) != 0 || info.catalogPresent );
    const auto time = [&]( I ticks ) {
        const auto value = double( Sub( ticks, capture.initBegin ) ) * capture.timerMultiplier;
        Require( std::isfinite( value ) && value >= double( INT64_MIN ) && value < double( INT64_MAX ), "JN time conversion overflow" );
        return I( value );
    };
    // Each type's packets are its packet blocks in id order, read once per
    // block without the shared block cache.
    const auto packets = [&]( QueueType type, const auto& emit ) {
        const auto key = std::to_string( uint8_t( type ) );
        const bool omitted = options.skeleton && ( type == QueueType::JnJobSchedule || type == QueueType::JnJobConfig || type == QueueType::JnJobDependency ||
            type == QueueType::JnJobStage || type == QueueType::JnGfxDispatch || type == QueueType::JnGfxEntity || type == QueueType::JnGfxLink || type == QueueType::JnRelation );
        if( omitted )
        {
            U64( 0 );
            return;
        }
        U64( uint64_t( d.Scalar( "SELECT COALESCE(SUM(count),0) FROM blocks WHERE stream=3 AND key=" + key ) ) );
        std::vector<uint64_t> ids;
        Each( "SELECT id FROM blocks WHERE stream=3 AND key=" + key + " ORDER BY first", [&]( auto q ) { ids.push_back( uint64_t( Val( q, 0 ) ) ); } );
        std::vector<PacketRow> rows;
        std::vector<uint8_t> payloads;
        for( const auto block : ids )
        {
            uint64_t blockKey, first, last;
            size_t size = 0;
            DecodePackets( ReadBlock( blocks, LocateBlock( d, block, BlockStream::Packets, blockKey, first, last ) ), rows, payloads, size );
            Require( size == size_t( QueueDataSize[uint8_t( type )] - 1 ) && !rows.empty() && rows.front().id == first && rows.back().id == last, "Invalid stored JN packet" );
            for( size_t n = 0; n < rows.size(); ++n )
            {
                Poll();
                QueueItem event{};
                memcpy( reinterpret_cast<uint8_t*>( &event ) + 1, payloads.data() + n * size, size );
                emit( event, rows[n].id, rows[n].thread, rows[n].stack );
            }
        }
    };
    packets( QueueType::JnJobType, [&]( const QueueItem& e, uint64_t, uint64_t, uint64_t ) {
        const auto& p = e.jnJobType;
        Put( JnJobTypeData{ p.name, p.typeId, p.kind, p.flags } );
    } );
    packets( QueueType::JnJobSchedule, [&]( const QueueItem& e, uint64_t, uint64_t thread, uint64_t ) {
        const auto& p = e.jnJobSchedule;
        Put( JnJobScheduleData{ time( p.time ), p.jobId, p.packedHandle, thread, p.dependencyCount, p.kind, p.flags } );
    } );
    packets( QueueType::JnJobConfig, [&]( const QueueItem& e, uint64_t, uint64_t, uint64_t ) {
        const auto& p = e.jnJobConfig;
        Put( JnJobConfigData{ p.jobId, p.typeId, p.count, p.grainSize, p.unityFlowId, p.originFrameSequence, p.kind, p.flags } );
    } );
    packets( QueueType::JnJobDependency, [&]( const QueueItem& e, uint64_t, uint64_t, uint64_t ) {
        const auto& p = e.jnJobDependency;
        Put( JnJobDependencyData{ p.jobId, p.prerequisiteJobId, p.prerequisiteHandle, p.flags } );
    } );
    packets( QueueType::JnJobStage, [&]( const QueueItem& e, uint64_t, uint64_t thread, uint64_t stack ) {
        const auto& p = e.jnJobStage;
        const bool cs = p.stage == uint8_t( JnJobStage::ScheduleCallstack ) || p.stage == uint8_t( JnJobStage::WaitCallstack );
        Put( JnJobStageData{ time( p.time ), p.jobId, cs ? p.arg0 : thread, cs ? uint32_t( Stack( stack ) ) : p.spanId, p.arg0, p.arg1, p.stage, p.flags } );
    } );
    packets( QueueType::JnGfxDispatch, [&]( const QueueItem& e, uint64_t, uint64_t thread, uint64_t ) {
        const auto& p = e.jnGfxDispatch;
        Put( JnGfxDispatchData{ time( p.time ), p.dispatchId, p.frameIndex, thread, p.expectedJobs, p.threadingMode, p.flags } );
    } );
    packets( QueueType::JnGfxEntity, [&]( const QueueItem& e, uint64_t, uint64_t thread, uint64_t ) {
        const auto& p = e.jnGfxEntity;
        Put( JnGfxEntityData{ time( p.time ), p.entityId, p.parentId, thread, p.gpuQueryId, p.gpuContext, p.kind, p.flags } );
    } );
    packets( QueueType::JnGfxLink, [&]( const QueueItem& e, uint64_t, uint64_t thread, uint64_t ) {
        const auto& p = e.jnGfxLink;
        Put( JnGfxLinkData{ time( p.time ), p.sourceId, p.targetId, thread, p.relation, p.flags } );
    } );
    packets( QueueType::JnFrame, [&]( const QueueItem& e, uint64_t, uint64_t thread, uint64_t ) {
        const auto& p = e.jnFrame;
        Put( JnFrameData{ time( p.time ), p.frameId, p.domainIndex, thread, p.domain, p.phase, p.flags } );
    } );
    packets( QueueType::JnIoRequest, [&]( const QueueItem& e, uint64_t, uint64_t thread, uint64_t ) {
        const auto& p = e.jnIoRequest;
        Put( JnIoRequestData{ time( p.time ), p.requestId, p.resourceId, thread, p.operation, p.source, p.priority, p.subsystem, p.flags } );
    } );
    packets( QueueType::JnIoConfig, [&]( const QueueItem& e, uint64_t, uint64_t, uint64_t ) {
        const auto& p = e.jnIoConfig;
        Put( JnIoConfigData{ p.requestId, p.parentId, p.requestedBytes, p.originFrameSequence, p.parentKind, p.flags } );
    } );
    packets( QueueType::JnIoStage, [&]( const QueueItem& e, uint64_t, uint64_t thread, uint64_t stack ) {
        const auto& p = e.jnIoStage;
        const bool cs = p.stage == uint8_t( JnIoStage::RequestCallstack );
        Put( JnIoStageData{ time( p.time ), p.requestId, p.bytes, cs ? p.detail : thread, cs ? uint32_t( Stack( stack ) ) : p.detail, p.stage, p.status, p.flags } );
    } );
    packets( QueueType::JnRelation, [&]( const QueueItem& e, uint64_t, uint64_t thread, uint64_t ) {
        const auto& p = e.jnRelation;
        Put( JnRelationData{ time( p.time ), p.sourceId, p.targetId, thread, p.sourceKind, p.targetKind, p.relationNamespace, p.relation, p.flags } );
    } );
    packets( QueueType::JnRuntimeDomainState, [&]( const QueueItem& e, uint64_t, uint64_t thread, uint64_t ) {
        const auto& p = e.jnRuntimeDomainState;
        Put( JnRuntimeDomainStateData{ time( p.time ), p.generation, p.requestedFrame, thread, p.domain, p.requestedMode, p.effectiveMode, p.reason, p.flags } );
    } );
    CountVector( "SELECT time,pass,frame,thread,taxonomy,level,flags FROM gpu_ref_passes ORDER BY id", [&]( auto q ) {
        Put( JnGpuReferencePassData{ Val( q, 0 ), uint64_t( Val( q, 1 ) ), uint64_t( Val( q, 2 ) ), uint64_t( Val( q, 3 ) ), uint32_t( Val( q, 4 ) ), uint8_t( Val( q, 5 ) ), uint8_t( Val( q, 6 ) ) } );
    } );
    // A skeleton leaves out references the snapshot cannot hold expanded:
    // its readers find them in the store by time.
    if( options.skeleton && info.gpuReferenceUses > JnTraceMaxRecordsPerDomain ) U64( 0 );
    else
    {
        Require( info.gpuReferenceUses <= JnTraceMaxRecordsPerDomain, "Expanded references exceed snapshot domain limit" );
        U64( info.gpuReferenceUses );
        Each( "SELECT pass,time,thread,setid,resource,mask,count,flags,encoding FROM gpu_ref_packets ORDER BY id", [&]( auto q ) {
            const uint64_t pass = Val( q, 0 ), thread = Val( q, 2 );
            const I time = Val( q, 1 );
            const uint32_t set = uint32_t( Val( q, 3 ) );
            const auto flags = uint8_t( Val( q, 7 ) ), encoding = uint8_t( Val( q, 8 ) );
            if( encoding == 1 ) Put( JnGpuReferenceUseData{ time, pass, uint64_t( Val( q, 4 ) ), thread, uint32_t( Val( q, 5 ) ), 0, flags, encoding } );
            else Each( "SELECT resource,mask FROM gpu_ref_entries WHERE setid=" + std::to_string( set ) + " ORDER BY ordinal", [&]( auto entry ) {
                Put( JnGpuReferenceUseData{ time, pass, uint64_t( Val( entry, 0 ) ), thread, uint32_t( Val( entry, 1 ) ), set, flags, encoding } );
            } );
        } );
    }
    CountVector( "SELECT time,pass,command_list,thread,total,dropped,flags FROM gpu_ref_ends ORDER BY id", [&]( auto q ) {
        Put( JnGpuReferenceEndData{ Val( q, 0 ), uint64_t( Val( q, 1 ) ), uint64_t( Val( q, 2 ) ), uint64_t( Val( q, 3 ) ), uint32_t( Val( q, 4 ) ), uint16_t( Val( q, 5 ) ), uint8_t( Val( q, 6 ) ) } );
    } );
    packets( QueueType::JnScriptFrame, [&]( const QueueItem& e, uint64_t, uint64_t thread, uint64_t ) {
        const auto& p = e.jnScriptFrame;
        Put( JnScriptFrameData{ p.function, p.file, thread, p.frameId, p.line, p.runtime, p.flags } );
    } );
    packets( QueueType::JnScriptStack, [&]( const QueueItem& e, uint64_t, uint64_t thread, uint64_t ) {
        const auto& p = e.jnScriptStack;
        Put( JnScriptStackData{ time( p.time ), p.primaryId, p.secondaryId, thread, p.value, p.runtime, p.flags, p.kind } );
    } );
    CountVector( "SELECT thread,id,stack,source,domain,provenance,flags,reason FROM callsites ORDER BY event", [&]( auto q ) {
        Put( JnCallsiteData{ uint64_t( Val( q, 0 ) ), uint32_t( Val( q, 1 ) ), uint32_t( Stack( Val( q, 2 ) ) ), Source( 0, Val( q, 3 ) ), uint8_t( Val( q, 4 ) ), uint8_t( Val( q, 5 ) ), uint8_t( Val( q, 6 ) ), uint8_t( Val( q, 7 ) ) } );
    } );
    U8( info.catalogPresent );
    U8( info.catalogValid );
    U16( info.catalogPresent ? JnGpuCatalogSchemaVersion : 0 );
    U16( info.catalogPresent ? JnGpuDetailedEvidenceSchemaVersion : 0 );
    CountVector( "SELECT time,generation,value,thread,sequence,kind,state,flags FROM catalog_controls ORDER BY id", [&]( auto q ) {
        Put( JnGpuCatalogControlData{ Val( q, 0 ), uint64_t( Val( q, 1 ) ), uint64_t( Val( q, 2 ) ), uint64_t( Val( q, 3 ) ), uint32_t( Val( q, 4 ) ), uint8_t( Val( q, 5 ) ), uint8_t( Val( q, 6 ) ), uint8_t( Val( q, 7 ) ) } );
    } );
    CountVector( "SELECT generation,transport,stored,first_record,sequence,records,bytes,kind,encoding,flags,valid FROM catalog_batches ORDER BY id", [&]( auto q ) {
        Put( JnGpuCatalogBatchData{ uint64_t( Val( q, 0 ) ), uint64_t( Val( q, 1 ) ), uint64_t( Val( q, 2 ) ), uint64_t( Val( q, 3 ) ), uint32_t( Val( q, 4 ) ), uint32_t( Val( q, 5 ) ), uint32_t( Val( q, 6 ) ), uint8_t( Val( q, 7 ) ), uint8_t( Val( q, 8 ) ), uint8_t( Val( q, 9 ) ), uint8_t( Val( q, 10 ) ) } );
    } );
    CountVector( "SELECT id,begin_value,end_value,begin,end,sequence,batches,records,bytes,unresolved,state,flags,ended,valid FROM catalog_generations ORDER BY event", [&]( auto q ) {
        Put( JnGpuCatalogGenerationData{ uint64_t( Val( q, 0 ) ), uint64_t( Val( q, 1 ) ), uint64_t( Val( q, 2 ) ), Val( q, 3 ), Val( q, 4 ), uint32_t( Val( q, 5 ) ), uint32_t( Val( q, 6 ) ), uint64_t( Val( q, 7 ) ), uint64_t( Val( q, 8 ) ), uint64_t( Val( q, 9 ) ), uint8_t( Val( q, 10 ) ), uint8_t( Val( q, 11 ) ), 1, uint8_t( Val( q, 12 ) ), uint8_t( Val( q, 13 ) ) } );
    } );
    CountVector( "SELECT generation,data FROM catalog_records WHERE kind=9 ORDER BY id", [&]( auto q ) {
        U64( Val( q, 0 ) );
        SqlBlob( q, 1 );
    } );
    for( int kind = 0; kind < 9; ++kind ) CountVector( "SELECT data FROM catalog_records WHERE kind=" + std::to_string( kind ) + " ORDER BY id", [&]( auto q ) {
        SqlBlob( q, 0 );
    } );
    if(Count("parameters"))
    {
        U32(JnParametersMagic);U16(JnParametersVersion);
        CountVector("SELECT idx,name_key,is_bool,value FROM parameters ORDER BY event",[&](auto q){U32(uint32_t(Val(q,0)));const auto key=Val(q,1);U64(RawString(key)?uint64_t(key):Synthetic(Intern("???")));U8(2);U8(uint8_t(Val(q,2)));Put(int32_t(Val(q,3)));});
    }
    const std::string unassociated = "SELECT i.ordinal,i.frame_index FROM frame_images i WHERE NOT EXISTS(SELECT 1 FROM base_frames f WHERE f.ordinal=i.frame_index) ORDER BY i.ordinal";
    if( d.Scalar( "SELECT COUNT(*) FROM (" + unassociated + ")" ) )
    {
        U32( JnFrameImageRefsMagic );
        U16( JnFrameImageRefsVersion );
        CountVector( unassociated, [&]( auto q ) {
            U64( Val( q, 0 ) );
            Require( Val( q, 1 ) >= 0 && uint64_t( Val( q, 1 ) ) <= UINT32_MAX, "Frame Image index exceeds snapshot limits" );
            U32( uint32_t( Val( q, 1 ) ) );
        } );
    }
}
#include "TracySnapshotValidate.inl"
}
StoreExportResult EventStore::ExportSnapshot( const std::filesystem::path& destination, const StoreExportOptions& options ) const
{
    Require( options.cacheBytes == 0 || ( options.cacheBytes >= ( 4ull << 20 ) && options.cacheBytes <= ( 256ull << 20 ) ), "Invalid export staging cache budget" );
    Require( !destination.empty(), "Missing snapshot destination" );
    Require( options.overwrite || !std::filesystem::exists( destination ), "Snapshot destination exists" );
    const auto protectSource = [&] {
        const auto target = std::filesystem::weakly_canonical( std::filesystem::absolute( destination ) );
        const auto same = []( const std::filesystem::path& a, const std::filesystem::path& b ) {
            if( CompareStringOrdinal( a.c_str(), -1, b.c_str(), -1, TRUE ) == CSTR_EQUAL ) return true;
            return std::filesystem::exists( a ) && std::filesystem::exists( b ) && std::filesystem::equivalent( a, b );
        };
        Require( !same( target, m_impl->source ), "Snapshot destination would replace the source journal" );
        for( auto parent = target; !parent.empty(); )
        {
            Require( !same( parent, m_impl->directory ), "Snapshot destination is inside the source event store" );
            const auto next = parent.parent_path();
            if( next == parent ) break;
            parent = next;
        }
    };
    protectSource();
    auto temporary = destination;
    temporary += ".partial-" + Hex( MakeFileHeader( ProtocolVersion ).sessionId );
    auto& db = m_impl->db;
    SnapshotExport exporter( db, m_impl->blocks, m_impl->info, CaptureMetadata(), options );
    struct RestoreCancel
    {
        Db& db;
        std::function<bool()> prior;
        ~RestoreCancel()
        {
            db.options.cancelled = prior;
        }
    } restore{ db, db.options.cancelled };
    db.options.cancelled = [prior = restore.prior, &options] {
        return ( prior && prior() ) || ( options.cancelled && options.cancelled() );
    };
    struct Temporary
    {
        std::filesystem::path path;
        bool owned = false;
        ~Temporary()
        {
            if( owned )
            {
                std::error_code error;
                std::filesystem::remove( path, error );
            }
        }
    } cleanup{ temporary };
    struct Transaction
    {
        Db& db;
        bool owned;
        bool done = false;
        ~Transaction()
        {
            if( owned && !done ) sqlite3_exec( db.handle, "ROLLBACK", nullptr, nullptr, nullptr );
        }
    } transaction{ db, sqlite3_get_autocommit( db.handle ) != 0 };
    if( transaction.owned ) db.Exec( "BEGIN" );
    exporter.Setup();
    SnapshotStreamOptions streamOptions;
    streamOptions.streams = options.compressionStreams;
    streamOptions.maxFileBytes = options.fileBytes;
    streamOptions.cancelled = options.cancelled;
    SnapshotStreamStats written;
    {
        SnapshotStreamWriter writer( temporary, streamOptions );
        cleanup.owned = true;
        exporter.Emit( [&]( std::span<const uint8_t> bytes ) {
            writer.Write( bytes );
        } );
        written = writer.Finish();
    }
    // Compare the decoded output with a second bounded traversal of the pinned
    // source mappings. This covers every emitted field without validationWorker.
    streamOptions.maxDecodedBytes = written.decodedBytes;
    {
        SnapshotStreamReader reader( temporary, streamOptions );
        std::array<uint8_t, 65536> buffer;
        exporter.Emit( [&]( std::span<const uint8_t> bytes ) {
            while( !bytes.empty() )
            {
                const auto size = std::min( bytes.size(), buffer.size() );
                reader.ReadExact( { buffer.data(), size } );
                Require( memcmp( buffer.data(), bytes.data(), size ) == 0, "Snapshot read-back differs from source projection" );
                bytes = bytes.subspan( size );
            }
        } );
        std::array<uint8_t, 1> extra;
        Require( reader.Read( extra ) == 0, "Snapshot contains trailing data" );
        const auto verified = reader.Finish();
        Require( verified.sha256 == written.sha256 && verified.decodedBytes == written.decodedBytes, "Snapshot verification digest mismatch" );
    }
    if( options.progress ) options.progress( "Verify structure", written.decodedBytes );
    {
        SnapshotStructure verifier( db, temporary, written.decodedBytes, streamOptions );
        const auto verified = verifier.Run();
        Require( verified.sha256 == written.sha256 && verified.fileBytes == written.fileBytes, "Snapshot changed during final validation" );
    }
    if( options.cancelled && options.cancelled() ) throw std::runtime_error( "Cancelled" );
    protectSource();
    if( transaction.owned )
    {
        db.Exec( "COMMIT" );
        transaction.done = true;
    }
    Require( MoveFileExW( temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH | ( options.overwrite ? MOVEFILE_REPLACE_EXISTING : 0 ) ) != 0, "Atomic snapshot publication failed" );
    cleanup.owned = false;
    return { written.decodedBytes, written.fileBytes, m_impl->info.revision, Hex( written.sha256 ) };
}

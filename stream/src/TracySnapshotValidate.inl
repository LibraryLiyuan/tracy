// Independent format walk: validate lengths, format limits, tree counts and
// dictionary references using fixed buffers and a disk-backed depth stack.
class SnapshotStructure
{
    Db& db;
    SnapshotStreamReader input;
    uint64_t position = 0, length = 0, strings = 0, localThreads = 0, externalThreads = 0, staticSources = 0, dynamicSources = 0, extras = 0, stacks = 0, maxStack = 0, maxImage = 0;
    bool hasImage = false;
    // Decoded bytes are taken from one block at a time instead of one stream
    // call per field.
    std::array<uint8_t, 65536> buffer{};
    size_t bufferOffset = 0, bufferSize = 0;
    // Remaining sibling counts of the open tree levels, innermost last. The
    // newest OpenWindow entries stay in memory; older ones spill to
    // temp.e_verify, so memory does not grow with nesting depth.
    static constexpr size_t OpenWindow = 65536;
    std::vector<uint64_t> open;
    uint64_t spilled = 0;
    void Next()
    {
        if( bufferOffset != bufferSize ) return;
        bufferSize = input.Read( buffer );
        bufferOffset = 0;
        Require( bufferSize > 0, "Truncated snapshot payload" );
    }
    template<class T>
    T Take()
    {
        T value{};
        auto out = std::span<uint8_t>( reinterpret_cast<uint8_t*>( &value ), sizeof( value ) );
        while( !out.empty() )
        {
            Next();
            const auto n = std::min( out.size(), bufferSize - bufferOffset );
            memcpy( out.data(), buffer.data() + bufferOffset, n );
            bufferOffset += n;
            out = out.subspan( n );
        }
        position += sizeof( value );
        Require( position <= length, "Snapshot structure exceeds payload" );
        return value;
    }
    void Skip( uint64_t size )
    {
        Require( position <= length && size <= length - position, "Snapshot section length exceeds payload" );
        while( size )
        {
            Next();
            const auto n = size_t( std::min<uint64_t>( size, bufferSize - bufferOffset ) );
            bufferOffset += n;
            size -= n;
            position += n;
        }
    }
    void Push( uint64_t remaining )
    {
        if( open.size() == OpenWindow )
        {
            for( size_t n = 0; n < OpenWindow / 2; ++n )
            {
                auto q = db.Prep( "INSERT INTO temp.e_verify VALUES(?,?)" );
                Int( q, 1, spilled + n );
                Int( q, 2, open[n] );
                db.Run( q );
            }
            spilled += OpenWindow / 2;
            open.erase( open.begin(), open.begin() + OpenWindow / 2 );
        }
        open.push_back( remaining );
    }
    uint64_t Pop()
    {
        if( !open.empty() )
        {
            const auto remaining = open.back();
            open.pop_back();
            return remaining;
        }
        Require( spilled != 0, "Snapshot verifier depth stack missing" );
        --spilled;
        auto q = db.Prep( "SELECT remaining FROM temp.e_verify WHERE depth=?" );
        Int( q, 1, spilled );
        Require( db.Step( q ), "Snapshot verifier depth stack missing" );
        const auto remaining = uint64_t( sqlite3_column_int64( q, 0 ) );
        sqlite3_reset( q );
        q = db.Prep( "DELETE FROM temp.e_verify WHERE depth=?" );
        Int( q, 1, spilled );
        db.Run( q );
        return remaining;
    }
    uint64_t Count( uint64_t width = 0, uint64_t maximum = UINT64_MAX )
    {
        const auto n = Take<uint64_t>();
        Require( n <= maximum && ( !width || n <= ( length - position ) / width ), "Invalid snapshot section count" );
        return n;
    }
    uint32_t U24()
    {
        uint32_t value = 0;
        for( unsigned n = 0; n < 3; ++n ) value |= uint32_t( Take<uint8_t>() ) << ( n * 8 );
        return value;
    }
    void Index()
    {
        Require( U24() <= strings, "Snapshot string index is out of range" );
    }
    void Stack()
    {
        maxStack = std::max<uint64_t>( maxStack, U24() );
    }
    void Source()
    {
        const auto value = Take<int16_t>();
        Require( value >= 0 ? uint64_t( value ) <= staticSources : uint64_t( -int64_t( value ) ) <= dynamicSources, "Snapshot source index is out of range" );
    }
    void Ref()
    {
        const auto id = Take<uint64_t>();
        const auto flags = Take<uint8_t>();
        Require( flags <= 3, "Invalid string reference flags" );
        if( ( flags & 3 ) == 3 ) Require( id < strings, "Indexed string reference is out of range" );
    }
    void SourceRecord()
    {
        Ref();
        Ref();
        Ref();
        Skip( 8 );
    }
    uint64_t Tree( bool gpu, uint64_t& childVectors )
    {
        uint64_t remaining = gpu ? Count() : Take<uint32_t>(), nodes = 0, depth = 0;
        while( remaining || depth )
        {
            if( !remaining )
            {
                remaining = Pop();
                --depth;
                Skip( gpu ? 18 : 8 );
                continue;
            }
            --remaining;
            ++nodes;
            if( gpu )
            {
                Skip( 16 );
                Source();
                Stack();
                Require( Take<uint16_t>() < localThreads, "GPU thread reference out of range" );
            }
            else
            {
                Source();
                Skip( 8 );
                Require( Take<uint32_t>() < extras, "CPU extra index out of range" );
            }
            const uint64_t children = gpu ? Count() : Take<uint32_t>();
            if( children )
            {
                ++childVectors;
                Push( remaining );
                ++depth;
                remaining = children;
            }
            else Skip( gpu ? 18 : 8 );
        }
        return nodes;
    }
    template<class T>
    void Vector()
    {
        const auto n = Count( sizeof( T ), JnTraceMaxRecordsPerDomain );
        Skip( n * sizeof( T ) );
    }

public:
    SnapshotStructure( Db& database, const std::filesystem::path& path, uint64_t bytes, const SnapshotStreamOptions& options )
        : db( database )
        , input( path, options )
        , length( bytes )
    {
    }
    SnapshotStreamStats Run()
    {
        db.Exec( "CREATE TEMP TABLE IF NOT EXISTS e_verify(depth INTEGER PRIMARY KEY,remaining INTEGER);DELETE FROM temp.e_verify" );
        const uint8_t signature[] = { 't', 'r', 'a', 'c', 'y', Version::Major, Version::Minor, Version::Patch };
        for( auto byte : signature ) Require( Take<uint8_t>() == byte, "Snapshot inner version mismatch" );
        Skip( 66 );
        for( int n = 0; n < 2; ++n ) Skip( Count( 1, 1023 ) );
        Skip( 16 );
        Skip( Count( 1, 1023 ) );
        for( uint64_t p = Count( 12 ); p; --p )
        {
            Skip( 4 );
            for( uint64_t d = Count( 12 ); d; --d )
            {
                Skip( 4 );
                for( uint64_t c = Count( 12 ); c; --c )
                {
                    Skip( 4 );
                    Skip( Count( 4 ) * 4 );
                }
            }
        }
        Skip( 24 );
        maxStack = Take<uint32_t>();
        uint64_t baseFrames = 0;
        for( uint64_t f = Count( 17 ); f; --f )
        {
            const auto frameSet = Take<uint64_t>();
            const auto continuous = Take<uint8_t>();
            Require( continuous <= 1, "Invalid Frame mode" );
            const auto frameCount = Count( continuous ? 12 : 20 );
            if( frameSet == 0 ) baseFrames = frameCount;
            for( uint64_t n = frameCount; n; --n )
            {
                Skip( continuous ? 8 : 16 );
                const auto image = Take<int32_t>();
                Require( image >= -1, "Invalid Frame Image index" );
                if( image >= 0 )
                {
                    hasImage = true;
                    maxImage = std::max( maxImage, uint64_t( image ) );
                }
            }
        }
        strings = Count( 16, ( 1ull << 24 ) - 1 );
        for( uint64_t n = 0; n < strings; ++n )
        {
            Require( Take<uint64_t>() == n + 1, "Invalid export string identity" );
            Skip( Count( 1 ) );
        }
        for( int map = 0; map < 2; ++map )
            for( uint64_t n = Count( 16 ); n; --n )
            {
                Skip( 8 );
                const auto id = Take<uint64_t>();
                Require( id > 0 && id <= strings, "String pointer map out of range" );
            }
        for( uint64_t n = Count( 24 ); n; --n )
        {
            Skip( 8 );
            for( int k = 0; k < 2; ++k )
            {
                const auto id = Take<uint64_t>();
                Require( id > 0 && id <= strings, "External name map out of range" );
            }
        }
        localThreads = Count( 8, 65536 );
        Require( localThreads > 0, "Local thread map has no zero entry" );
        Require( Take<uint64_t>() == 0, "Local thread zero entry invalid" );
        Skip( ( localThreads - 1 ) * 8 );
        externalThreads = Count( 8, 65536 );
        Skip( externalThreads * 8 );
        staticSources = Count( 43, 32767 );
        for( uint64_t n = staticSources; n; --n )
        {
            Skip( 8 );
            SourceRecord();
        }
        const auto expanded = Count( 8, 32768 );
        Require( expanded == staticSources + 1, "Source expansion count mismatch" );
        Skip( expanded * 8 );
        dynamicSources = Count( 35, 32767 );
        for( uint64_t n = dynamicSources; n; --n ) SourceRecord();
        for( int domain = 0; domain < 2; ++domain )
            for( uint64_t n = Count( 10 ); n; --n )
            {
                Source();
                Take<uint64_t>();
            }
        for( uint64_t n = Count( 27 ); n; --n )
        {
            Skip( 4 );
            Index();
            Source();
            Require( Take<uint8_t>() <= 1, "Unknown lock type" );
            Require( Take<uint8_t>() <= 1, "Invalid lock validity" );
            Skip( 16 );
            const auto threads = Count( 8, 64 );
            Skip( threads * 8 );
            for( uint64_t e = Count( 12 ); e; --e )
            {
                Skip( 8 );
                Source();
                Require( Take<uint8_t>() < threads, "Lock thread index invalid" );
                Require( Take<uint8_t>() <= 5, "Unknown lock event type" );
            }
        }
        for( uint64_t n = Count( 32 ); n; --n )
        {
            Skip( 16 );
            Ref();
            Skip( 4 );
            Stack();
        }
        extras = Count( 12, UINT32_MAX );
        Require( extras > 0, "Missing zero ZoneExtra" );
        for( uint64_t n = extras; n; --n )
        {
            Stack();
            Index();
            Index();
            Skip( 3 );
        }
        const auto zones = Count(), children = Count(), threadCount = Count( 29 );
        uint64_t readZones = 0, readChildren = 0;
        for( uint64_t n = threadCount; n; --n )
        {
            Skip( 8 );
            const auto threadZones = Take<uint64_t>();
            Skip( 13 );
            const auto observed = Tree( false, readChildren );
            Require( observed == threadZones, "CPU per-thread zone count mismatch" );
            readZones += observed;
            Skip( Count( 8 ) * 8 );
            for( int k = 0; k < 2; ++k )
                for( uint64_t sample = Count( 11 ); sample; --sample )
                {
                    Skip( 8 );
                    Stack();
                }
        }
        Require( readZones == zones && readChildren == children, "CPU timeline totals mismatch" );
        const auto gpuZones = Count(), gpuChildren = Count(), contexts = Count( 33 );
        uint64_t readGpu = 0, readGpuChildren = 0;
        for( uint64_t n = contexts; n; --n )
        {
            Skip( 8 );
            Require( Take<uint8_t>() <= 1, "Invalid GPU calibration flag" );
            const auto completed = Take<uint64_t>();
            Skip( 5 );
            Index();
            Skip( 8 );
            for( uint64_t note = Count( 11 ); note; --note )
            {
                Skip( 8 );
                Index();
            }
            uint64_t contextZones = 0;
            for( uint64_t lane = Count( 16 ); lane; --lane )
            {
                Skip( 8 );
                contextZones += Tree( true, readGpuChildren );
            }
            Require( completed <= contextZones, "GPU completed count exceeds timeline" );
            readGpu += contextZones;
            for( uint64_t notes = Count( 10 ); notes; --notes )
            {
                Skip( 2 );
                Skip( Count( 16 ) * 16 );
            }
        }
        Require( readGpu == gpuZones && readGpuChildren == gpuChildren, "GPU timeline totals mismatch" );
        for( uint64_t n = Count( 48 ); n; --n )
        {
            Skip( 40 );
            Skip( Count( 16 ) * 16 );
        }
        const auto pools = Count(), allocations = Count();
        uint64_t readAllocations = 0;
        for( uint64_t n = pools; n; --n )
        {
            Skip( 8 );
            const auto events = Count( 42 ), active = Count(), freed = Count();
            Require( active <= events && freed == events - active, "Allocator lifecycle counts mismatch" );
            readAllocations += events;
            for( uint64_t e = events; e; --e )
            {
                Skip( 16 );
                Stack();
                Stack();
                Skip( 16 );
                Require( Take<uint16_t>() < localThreads && Take<uint16_t>() < localThreads, "Memory thread reference invalid" );
            }
            Skip( 32 );
        }
        Require( readAllocations == allocations, "Allocation total mismatch" );
        stacks = Count( 2, ( 1ull << 24 ) - 1 );
        Require( maxStack <= stacks, "Callstack reference out of range" );
        for( uint64_t n = stacks; n; --n ) Skip( uint64_t( Take<uint16_t>() ) * 8 );
        for( uint64_t n = Count( 12 ); n; --n )
        {
            Skip( 8 );
            const auto frames = Take<uint8_t>();
            Index();
            for( uint64_t f = frames; f; --f )
            {
                Index();
                Index();
                Skip( 12 );
            }
        }
        for( uint64_t n = Count( 9 ); n; --n ) Ref();
        const auto dictionary = Take<uint32_t>();
        Skip( dictionary );
        const auto images = Count( 5 );
        Require( !hasImage || maxImage < images, "Frame Image reference out of range" );
        for( uint64_t n = images; n; --n )
        {
            const auto w = Take<uint16_t>(), h = Take<uint16_t>();
            Require( w && h && w % 4 == 0 && h % 4 == 0 && Take<uint8_t>() <= 1, "Invalid Frame Image dimensions" );
            Skip( uint64_t( w ) * h / 2 );
        }
        for( uint64_t n = Count( 16 ); n; --n )
        {
            Skip( 8 );
            Skip( Count( 36 ) * 36 );
        }
        const auto cpuRegions = Count();
        uint64_t readCpuRegions = 0;
        for( int cpu = 0; cpu < 256; ++cpu )
        {
            const auto n = Count( 18 );
            readCpuRegions += n;
            for( uint64_t i = n; i; --i )
            {
                Skip( 16 );
                Require( Take<uint16_t>() < externalThreads, "CPU external thread index invalid" );
            }
        }
        Require( cpuRegions == readCpuRegions, "CPU region count mismatch" );
        Skip( Count( 16 ) * 16 );
        Skip( Count( 24 ) * 24 );
        const auto normalSymbols = Count(), inlineSymbols = Count(), symbols = Count( 32 );
        Require( normalSymbols + inlineSymbols == symbols, "Symbol class counts mismatch" );
        for( uint64_t n = symbols; n; --n )
        {
            Skip( 8 );
            Index();
            Index();
            Skip( 4 );
            Index();
            Index();
            Skip( 8 );
        }
        for( uint64_t n = Count( 12 ); n; --n )
        {
            Skip( 8 );
            Skip( Take<uint32_t>() );
        }
        Skip( Count( 16 ) * 16 );
        for( uint64_t n = Count( 56 ); n; --n )
        {
            Skip( 8 );
            for( int kind = 0; kind < 6; ++kind ) Skip( Count( 8 ) * 8 );
        }
        for( uint64_t n = Count( 8 ); n; --n )
        {
            Skip( Take<uint32_t>() );
            Skip( Take<uint32_t>() );
        }
        Require( Take<uint32_t>() == JnTraceSectionMagic && Take<uint16_t>() == JnTraceSchemaVersion, "Invalid JN section header" );
        Require( Take<uint8_t>() <= 1, "Invalid JN presence flag" );
        Vector<JnJobTypeData>();
        Vector<JnJobScheduleData>();
        Vector<JnJobConfigData>();
        Vector<JnJobDependencyData>();
        Vector<JnJobStageData>();
        Vector<JnGfxDispatchData>();
        Vector<JnGfxEntityData>();
        Vector<JnGfxLinkData>();
        Vector<JnFrameData>();
        Vector<JnIoRequestData>();
        Vector<JnIoConfigData>();
        Vector<JnIoStageData>();
        Vector<JnRelationData>();
        Vector<JnRuntimeDomainStateData>();
        Vector<JnGpuReferencePassData>();
        Vector<JnGpuReferenceUseData>();
        Vector<JnGpuReferenceEndData>();
        Vector<JnScriptFrameData>();
        Vector<JnScriptStackData>();
        for( uint64_t n = Count( sizeof( JnCallsiteData ), JnTraceMaxRecordsPerDomain ); n; --n )
        {
            const auto site = Take<JnCallsiteData>();
            Require( site.callstack <= stacks, "JN callsite stack invalid" );
            Require( site.sourceLocation >= 0 ? uint64_t( site.sourceLocation ) <= staticSources : uint64_t( -int64_t( site.sourceLocation ) ) <= dynamicSources, "JN callsite source invalid" );
        }
        Require( Take<uint8_t>() <= 1 && Take<uint8_t>() <= 1, "Invalid catalog flags" );
        Skip( 4 );
        Vector<JnGpuCatalogControlData>();
        Vector<JnGpuCatalogBatchData>();
        Vector<JnGpuCatalogGenerationData>();
        for( uint64_t n = Count( 8 + sizeof( JnGpuCatalogStringRecordHeaderV1 ), JnTraceMaxRecordsPerDomain ); n; --n )
        {
            Require( Take<uint64_t>() != 0, "Invalid catalog string generation" );
            const auto string = Take<JnGpuCatalogStringRecordHeaderV1>();
            Require( string.byteLength <= 256 && string.stringId != 0, "Invalid catalog string metadata" );
            Skip( string.byteLength );
        }
        Vector<JnGpuCatalogResourceRecordV1>();
        Vector<JnGpuCatalogAllocationRecordV1>();
        Vector<JnGpuCatalogViewRecordV1>();
        Vector<JnGpuCatalogLogicalRecordV1>();
        Vector<JnGpuCatalogPartRecordV1>();
        Vector<JnGpuCatalogRelationRecordV1>();
        Vector<JnGpuCatalogVgRecordV1>();
        Vector<JnGpuRangeSetRecordV1>();
        Vector<JnGpuDetailedEvidenceRecordV1>();
        bool seenImages=false,seenParameters=false;
        while( position < length )
        {
            const auto magic=Take<uint32_t>();const auto version=Take<uint16_t>();
            if(magic==JnParametersMagic)
            {
                Require(!seenParameters && version==JnParametersVersion,"Invalid parameter footer");seenParameters=true;
                for(uint64_t n=Count(18,JnTraceMaxRecordsPerDomain);n;--n){Take<uint32_t>();Ref();Require(Take<uint8_t>()<=1,"Invalid parameter type");Take<int32_t>();}
                continue;
            }
            Require( !seenImages && magic == JnFrameImageRefsMagic && version == JnFrameImageRefsVersion, "Invalid Frame Image reference footer" );
            seenImages=true;
            uint64_t previous = 0;
            const auto count = Count( 12, images );
            for( uint64_t n = 0; n < count; ++n )
            {
                const auto image = Take<uint64_t>();
                const auto frame = Take<uint32_t>();
                Require( image < images && ( !n || image > previous ) && frame >= baseFrames, "Invalid unassociated Frame Image reference" );
                previous = image;
            }
        }
        Require( position == length, "Snapshot structure has trailing or missing bytes" );
        std::array<uint8_t, 1> extra;
        Require( bufferOffset == bufferSize && input.Read( extra ) == 0, "Trailing snapshot payload" );
        const auto verified = input.Finish();
        db.Exec( "DROP TABLE temp.e_verify" );
        return verified;
    }
};

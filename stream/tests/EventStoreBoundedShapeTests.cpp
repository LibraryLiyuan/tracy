// Shapes beyond the fixed in-memory windows of ingestion and export: more
// interleaved open thread stacks than the thread cache holds, nesting deeper
// than the zone, timeline and verifier windows, and Zone texts around the
// 65535-byte dictionary boundary. Every path must keep exact results.
#include "../../public/common/TracyProtocol.hpp"
#include "../../public/common/TracyQueue.hpp"
#include "../../public/common/tracy_lz4.hpp"
#include "../src/TracyEventStore.hpp"
#include "../src/TracyStreamJournal.hpp"
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
using namespace tracy;
using namespace tracy::stream;
namespace
{
void Check( bool v, const char* m )
{
    if( !v ) throw std::runtime_error( m );
}
template<class T>
void Bytes( std::vector<uint8_t>& b, const T& v )
{
    const auto p = reinterpret_cast<const uint8_t*>( &v );
    b.insert( b.end(), p, p + sizeof( v ) );
}
template<class T>
void Event( std::vector<uint8_t>& b, QueueType type, const T& v )
{
    b.push_back( uint8_t( type ) );
    Bytes( b, v );
}
constexpr uint32_t Threads = 1500;  // Ingest thread cache holds 1024 rows.
constexpr uint32_t Depth = 70000;   // Export and verifier keep 65536 levels in memory.
constexpr uint32_t DeepThread = 9000, TextThread = 9001, OpenThread = 9002;
constexpr uint32_t DistinctTexts = 20000; // Export recent-string cache has 8192 slots.
struct Fixture
{
    JournalWriter& writer;
    std::string error;
    std::vector<uint8_t> b;
    uint64_t sequence = 0;
    int64_t now = 2000, reference = 0;
    uint32_t validation = 0;
    uint64_t zones = 0;
    void Append( RecordType t, uint32_t flags, std::span<const uint8_t> data ) { Check( writer.Append( t, flags, data, ++sequence, error ), error.c_str() ); }
    void Flush()
    {
        if( b.empty() ) return;
        std::vector<uint8_t> out( LZ4Size + 4 );
        const int n = LZ4_compress_default( reinterpret_cast<char*>( b.data() ), reinterpret_cast<char*>( out.data() + 4 ), int( b.size() ), LZ4Size );
        Check( n > 0, "compress fixture frame" );
        memcpy( out.data(), &n, 4 );
        out.resize( n + 4 );
        Append( RecordType::ClientToServer, RecordFlagCompressedFrame, out );
        b.clear();
    }
    void Room() { if( b.size() > 300000 ) Flush(); }
    // Zone times are deltas from the last event since ThreadContext.
    int64_t Delta()
    {
        now += 3;
        const auto delta = now - reference;
        reference = now;
        return delta;
    }
    void Context( uint32_t thread )
    {
        Room();
        Event( b, QueueType::ThreadContext, QueueThreadContext{ thread } );
        reference = 0;
    }
    uint32_t Begin()
    {
        Room();
        const auto v = ++validation;
        Event( b, QueueType::ZoneValidation, QueueZoneValidation{ v } );
        Event( b, QueueType::ZoneBegin, QueueZoneBegin{ { Delta() }, 0x123 } );
        ++zones;
        return v;
    }
    void Text( uint32_t v, std::string_view text )
    {
        Room();
        Event( b, QueueType::ZoneValidation, QueueZoneValidation{ v } );
        b.push_back( uint8_t( QueueType::SingleStringData ) );
        Bytes( b, uint16_t( text.size() ) );
        b.insert( b.end(), text.begin(), text.end() );
        b.push_back( uint8_t( QueueType::ZoneText ) );
    }
    void End( uint32_t v )
    {
        Room();
        Event( b, QueueType::ZoneValidation, QueueZoneValidation{ v } );
        Event( b, QueueType::ZoneEnd, QueueZoneEnd{ Delta() } );
    }
};
uint64_t Write( const std::filesystem::path& path, uint32_t depth = Depth, bool embeddedNul = true )
{
    std::string error;
    auto writer = JournalWriter::CreateFileJournal( path, MakeFileHeader( ProtocolVersion ), false, {}, error );
    Check( bool( writer ), error.c_str() );
    Fixture f{ *writer };
    std::vector<uint8_t> header( 24 );
    header[0] = 2;
    header[2] = 24;
    const uint32_t protocol = ProtocolVersion;
    memcpy( header.data() + 4, &protocol, 4 );
    f.Append( RecordType::SessionBegin, 0, header );
    const uint8_t handshake = HandshakeWelcome;
    f.Append( RecordType::ClientToServer, RecordFlagHandshake, { &handshake, 1 } );
    WelcomeMessage welcome{};
    welcome.initBegin = 1000;
    welcome.initEnd = 1010;
    welcome.timerMul = 1;
    strcpy_s( welcome.programName, "Bounded shape fixture" );
    f.Append( RecordType::ClientToServer, RecordFlagHandshake, { reinterpret_cast<const uint8_t*>( &welcome ), sizeof( welcome ) } );
    ServerQueryPacket query{ ServerQuerySourceLocation, 0x123, 0 };
    f.Append( RecordType::ServerToClient, RecordFlagServerQuery, { reinterpret_cast<const uint8_t*>( &query ), sizeof( query ) } );
    Event( f.b, QueueType::SourceLocation, QueueSourceLocation{ 0, 0, 0, 1, 0, 0, 0 } );
    // Every thread opens two zones before any of them closes, so most open
    // stacks are written back and reloaded while they still hold zones.
    std::vector<std::pair<uint32_t, uint32_t>> open( Threads );
    for( uint32_t t = 0; t < Threads; ++t )
    {
        f.Context( 100 + t );
        open[t].first = f.Begin();
        open[t].second = f.Begin();
    }
    for( uint32_t t = Threads; t-- > 0; )
    {
        f.Context( 100 + t );
        f.Text( open[t].second, "thread " + std::to_string( 100 + t ) );
        f.End( open[t].second );
        f.End( open[t].first );
    }
    // A single nesting chain deeper than every in-memory window.
    f.Context( DeepThread );
    std::vector<uint32_t> chain;
    chain.reserve( depth );
    for( uint32_t n = 0; n < depth; ++n )
    {
        chain.push_back( f.Begin() );
        if( n % 997 == 0 ) f.Text( chain.back(), "deep " + std::to_string( n ) );
    }
    for( uint32_t n = depth; n-- > 0; ) f.End( chain[n] );
    // Texts at the dictionary boundary: 65535 bytes take the chunked path
    // and must still match an equal text; longer texts span chunks; repeats
    // of an early text return after the recent-string cache was refilled.
    f.Context( TextThread );
    const std::string limit( 65535, 'L' ), below( 65534, 'B' ), half( 40000, 'H' );
    std::string withNul = "before";
    withNul.push_back( '\0' );
    withNul += "after";
    for( int repeat = 0; repeat < 2; ++repeat )
    {
        for( const auto* text : { &limit, &below } )
        {
            const auto v = f.Begin();
            f.Text( v, *text );
            f.End( v );
        }
        const auto v = f.Begin();
        f.Text( v, half );
        f.Text( v, half );
        f.End( v );
    }
    // Snapshots keep each fragment up to its first NUL, as the native Worker
    // does; StoreZone::text keeps the raw fragment bytes.
    if( embeddedNul )
    {
        const auto v = f.Begin();
        f.Text( v, withNul );
        f.Text( v, "second fragment" );
        f.End( v );
    }
    for( uint32_t n = 0; n < DistinctTexts; ++n )
    {
        const auto v = f.Begin();
        f.Text( v, "distinct text " + std::to_string( n % ( DistinctTexts / 2 ) ) + std::string( n % 7, '.' ) );
        f.End( v );
    }
    // Zones still open when the capture ends.
    f.Context( OpenThread );
    f.Begin();
    f.Begin();
    f.Flush();
    std::vector<uint8_t> end( 32 );
    end[0] = 1;
    end[2] = 32;
    end[4] = 3;
    f.Append( RecordType::SessionEnd, RecordFlagTerminal, end );
    return f.zones;
}
std::vector<StoreZone> ThreadZones( const EventStore& store, uint64_t thread )
{
    std::vector<StoreZone> result;
    uint64_t after = 0;
    for( ;; )
    {
        auto page = store.Zones( INT64_MIN, INT64_MAX, thread, after, 256 );
        for( auto& zone : page.values ) result.push_back( std::move( zone ) );
        if( !page.more ) return result;
        after = page.next;
    }
}
// The zone cursor reads every thread in preorder, from the start and from
// any ordinal, with the same values as the paged zone API.
void CheckCursor( const EventStore& store )
{
    uint64_t total = 0;
    auto cursor = store.ZoneCursor();
    StoreZoneRecord r;
    for( const auto& t : store.ZoneThreads() )
    {
        const auto list = ThreadZones( store, t.thread );
        Check( list.size() == t.zones && !list.empty(), "cursor thread zone count" );
        cursor->Seek( t.thread, 0 );
        for( const auto& z : list )
        {
            Check( cursor->Next( r ), "cursor reads every zone" );
            Check( r.id == z.id && r.parent == z.parent && r.depth == z.depth && r.start == z.start && r.open == !z.end && ( r.open || r.end == *z.end ) && r.childTime == z.childTime && r.stack == z.stack, "cursor zone fields" );
            if( z.text.find( '\0' ) == std::string::npos && !z.textMore ) Check( r.text == z.text && r.hasText == !z.text.empty(), "cursor zone text" );
            if( r.hasName ) Check( r.name == z.name, "cursor zone name" );
        }
        Check( !cursor->Next( r ) && cursor->Ordinal() == t.zones, "cursor stops at the thread end" );
        const auto middle = list.size() / 2;
        cursor->Seek( t.thread, middle );
        Check( cursor->Next( r ) && r.id == list[middle].id && cursor->Ordinal() == middle + 1, "cursor seeks to an ordinal" );
        const auto ordinal = store.ZoneOrdinal( t.thread, list.back().id );
        Check( ordinal && *ordinal == list.size() - 1, "zone ordinal" );
        total += t.zones;
    }
    Check( total == store.Info().cpuZones, "cursor threads cover every zone" );
}
}
int main( int argc, char** argv )
{
    const auto root = std::filesystem::temp_directory_path() / ( "tracy-bounded-shapes-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    try
    {
        std::filesystem::create_directory( root );
        const auto journal = root / "shapes.tracy-stream";
        // --write-only [depth [--no-nul]] writes the fixture for external
        // differential checks; Tracy's recursive Worker loader needs a
        // shallower chain.
        if( argc > 1 && std::string_view( argv[1] ) == "--write-only" )
        {
            Write( journal, argc > 2 ? uint32_t( std::stoul( argv[2] ) ) : Depth, !( argc > 3 && std::string_view( argv[3] ) == "--no-nul" ) );
            std::cout << "Fixture journal: " << journal.string() << '\n';
            return 0;
        }
        const auto zones = Write( journal );
        StoreOptions read;
        read.cacheBytes = 4ull << 20;
        read.resultBytes = 65536;
        const auto info = EventStore::Build( journal, read );
        Check( info.cpuZones == zones && info.openZones == 2 && info.unresolvedDefinitions == 0, "zone totals" );
        auto store = EventStore::Open( journal, read );
        for( const uint64_t thread : { 100ull, 100ull + Threads / 2, 100ull + Threads - 1 } )
        {
            const auto list = ThreadZones( *store, thread );
            Check( list.size() == 2, "each interleaved thread keeps its two zones" );
            Check( list[0].parent == 0 && list[0].depth == 0 && list[0].end && list[1].parent == list[0].id && list[1].depth == 1 && list[1].end, "evicted thread stacks keep parents and ends" );
            Check( *list[1].end < *list[0].end && list[1].text == "thread " + std::to_string( thread ), "inner zone closes first and keeps its text" );
            Check( list[0].childTime == *list[1].end - list[1].start && list[1].childTime == 0, "evicted thread stacks keep the child time" );
        }
        auto first = store->Zones( INT64_MIN, INT64_MAX, DeepThread, 0, 1 );
        Check( first.values.size() == 1 && first.values[0].depth == 0 && first.values[0].parent == 0, "deep chain root" );
        auto zone = first.values[0];
        for( uint32_t n = 1; n < Depth; ++n )
        {
            const auto children = store->Children( zone.id );
            Check( children.values.size() == 1 && !children.more, "deep chain has one child per level" );
            const auto& child = children.values[0];
            Check( child.depth == n && child.parent == zone.id && child.thread == DeepThread && child.end && zone.end, "deep chain parent and depth" );
            Check( child.start >= zone.start && *child.end <= *zone.end, "deep chain nesting" );
            Check( zone.childTime == *child.end - child.start, "deep chain child time survives spilled frames" );
            Check( ( n % 997 == 0 ) == !child.text.empty(), "deep chain texts stay on their zones" );
            zone = child;
        }
        Check( store->Children( zone.id ).values.empty() && zone.childTime == 0, "deep chain leaf" );
        CheckCursor( *store );
        const auto openZones = ThreadZones( *store, OpenThread );
        Check( openZones.size() == 2 && !openZones[0].end && !openZones[1].end && openZones[1].parent == openZones[0].id, "open zones stay open" );
        StoreTimelineOptions overview;
        overview.begin = 0;
        overview.end = store->Info().lastNs + 1;
        overview.bins = 16;
        overview.afterLane = DeepThread;
        overview.maximumLanes = 1;
        const auto deepView = store->TimelineOverview( overview );
        Check( deepView.lanes.size() == 1 && deepView.lanes[0].id == DeepThread && deepView.lanes[0].events == Depth && deepView.lanes[0].maximumDepth == Depth - 1, "range index returns the whole deep chain" );
        const auto output = root / "shapes.tracy";
        const auto result = store->ExportSnapshot( output );
        Check( result.fileBytes == std::filesystem::file_size( output ) && result.decodedBytes > 0, "export with spilled timeline and verifier stacks" );
        // The live projection decodes the same journal in small batches; every
        // batch flushes the write-back thread and task state before commit.
        StoreOptions liveOptions;
        liveOptions.cacheBytes = 4ull << 20;
        auto live = LiveEventStore::Create( journal, liveOptions );
        while( live->Update( 3 ) ) {}
        const auto snapshot = live->Snapshot( read );
        Check( snapshot->Info().cpuZones == info.cpuZones && snapshot->Info().openZones == info.openZones && snapshot->Info().cleanEnd, "live projection totals" );
        for( const uint64_t thread : { 100ull, 100ull + Threads - 1, uint64_t( OpenThread ) } )
        {
            const auto a = ThreadZones( *store, thread ), b = ThreadZones( *snapshot, thread );
            Check( a.size() == b.size(), "live projection zone count per thread" );
            for( size_t n = 0; n < a.size(); ++n ) Check( a[n].id == b[n].id && a[n].parent == b[n].parent && a[n].depth == b[n].depth && a[n].end == b[n].end && a[n].childTime == b[n].childTime && a[n].text == b[n].text, "live projection zones" );
        }
        std::cout << "Bounded shapes passed: zones=" << zones << " payload SHA256=" << result.sha256 << " evidence=" << root.string() << '\n';
        return 0;
    }
    catch( const std::exception& e )
    {
        std::cerr << "Bounded shapes failed: " << e.what() << " evidence=" << root.string() << '\n';
        return 1;
    }
}

#include "../../public/common/TracyProtocol.hpp"
#include "../../public/common/TracyQueue.hpp"
#include "../../public/common/tracy_lz4.hpp"
#include "../src/TracyEventStore.hpp"
#include "../src/TracyStreamJournal.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
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
    auto p = reinterpret_cast<const uint8_t*>( &v );
    b.insert( b.end(), p, p + sizeof( v ) );
}
template<class T>
void Event( std::vector<uint8_t>& b, QueueType t, const T& v )
{
    b.push_back( uint8_t( t ) );
    Bytes( b, v );
}
void String( std::vector<uint8_t>& b, QueueType type, uint64_t key, const char* value )
{
    Event( b, type, QueueStringTransfer{ key } );
    const uint16_t length = uint16_t( strlen( value ) );
    Bytes( b, length );
    b.insert( b.end(), value, value + length );
}
// Thread 1: zone A at depth 0 holding 10000 children, so that the middle
// block holds nested zones only, then three more top-level zones. Thread 2:
// 3000 top-level zones after A. Times are those of the journal; the store counts
// them from initBegin.
constexpr uint64_t Children = 10000, Siblings = 3, Others = 3000;
constexpr int64_t InitBegin = 1000, AStart = 2000, AEnd = AStart + 10 + 20 * int64_t( Children - 1 ) + 10 + 10;
void Fixture( const std::filesystem::path& p )
{
    std::string error;
    auto writer = JournalWriter::CreateFileJournal( p, MakeFileHeader( ProtocolVersion ), false, {}, error );
    Check( bool( writer ), error.c_str() );
    uint64_t now = 0;
    auto append = [&]( RecordType type, uint32_t flags, std::span<const uint8_t> data ) { Check( writer->Append( type, flags, data, ++now, error ), error.c_str() ); };
    const uint32_t version = ProtocolVersion;
    std::vector<uint8_t> data( 24 );
    data[0] = 2;
    data[2] = 24;
    memcpy( data.data() + 4, &version, 4 );
    append( RecordType::SessionBegin, 0, data );
    append( RecordType::ServerToClient, RecordFlagHandshake, { reinterpret_cast<const uint8_t*>( HandshakeShibboleth ), 8 } );
    append( RecordType::ServerToClient, RecordFlagHandshake, { reinterpret_cast<const uint8_t*>( &version ), 4 } );
    const uint8_t welcomeStatus = HandshakeWelcome;
    append( RecordType::ClientToServer, RecordFlagHandshake, { &welcomeStatus, 1 } );
    WelcomeMessage welcome{};
    welcome.timerMul = 1;
    welcome.initBegin = InitBegin;
    welcome.initEnd = InitBegin + 10;
    strcpy_s( welcome.programName, "TimelineFixture" );
    append( RecordType::ClientToServer, RecordFlagHandshake, { reinterpret_cast<const uint8_t*>( &welcome ), sizeof( welcome ) } );
    for( auto [type, key] : std::array<std::pair<ServerQuery, uint64_t>, 4>{ { { ServerQuerySourceLocation, 0x123 }, { ServerQueryString, 0x100 }, { ServerQueryString, 0x101 }, { ServerQueryString, 0x102 } } } )
    {
        ServerQueryPacket query{ type, key, 0 };
        append( RecordType::ServerToClient, RecordFlagServerQuery, { reinterpret_cast<const uint8_t*>( &query ), sizeof( query ) } );
    }
    data.clear();
    auto flush = [&] {
        if( data.empty() ) return;
        std::vector<uint8_t> compressed( LZ4Size + 4 );
        int size = LZ4_compress_default( reinterpret_cast<const char*>( data.data() ), reinterpret_cast<char*>( compressed.data() + 4 ), int( data.size() ), LZ4Size );
        Check( size > 0, "compress fixture" );
        memcpy( compressed.data(), &size, 4 );
        compressed.resize( size + 4 );
        append( RecordType::ClientToServer, RecordFlagCompressedFrame, compressed );
        data.clear();
    };
    const auto maybeFlush = [&] {
        if( data.size() > 32 * 1024 ) flush();
    };
    Event( data, QueueType::ThreadContext, QueueThreadContext{ 1 } );
    Event( data, QueueType::ZoneBegin, QueueZoneBegin{ { AStart }, 0x123 } );
    for( uint64_t i = 0; i < Children; ++i )
    {
        Event( data, QueueType::ZoneBegin, QueueZoneBegin{ { 10 }, 0x123 } );
        Event( data, QueueType::ZoneEnd, QueueZoneEnd{ 10 } );
        maybeFlush();
    }
    Event( data, QueueType::ZoneEnd, QueueZoneEnd{ 10 } );
    for( uint64_t i = 0; i < Siblings; ++i )
    {
        Event( data, QueueType::ZoneBegin, QueueZoneBegin{ { 1000 }, 0x123 } );
        Event( data, QueueType::ZoneEnd, QueueZoneEnd{ 100 } );
    }
    flush();
    Event( data, QueueType::ThreadContext, QueueThreadContext{ 2 } );
    Event( data, QueueType::ZoneBegin, QueueZoneBegin{ { AEnd + 100000 }, 0x123 } );
    Event( data, QueueType::ZoneEnd, QueueZoneEnd{ 50 } );
    for( uint64_t i = 1; i < Others; ++i )
    {
        Event( data, QueueType::ZoneBegin, QueueZoneBegin{ { 50 }, 0x123 } );
        Event( data, QueueType::ZoneEnd, QueueZoneEnd{ 50 } );
        maybeFlush();
    }
    Event( data, QueueType::SourceLocation, QueueSourceLocation{ 0x100, 0x101, 0x102, 42, 0, 0, 255 } );
    String( data, QueueType::StringData, 0x100, "Timeline zone" );
    String( data, QueueType::StringData, 0x101, "TimelineFn" );
    String( data, QueueType::StringData, 0x102, "timeline.cpp" );
    flush();
    data.assign( 32, 0 );
    data[0] = 1;
    data[2] = 32;
    data[4] = 3;
    append( RecordType::SessionEnd, RecordFlagTerminal, data );
}
struct Zone
{
    uint64_t id;
    int64_t start, end;
    uint32_t depth, source;
    bool operator==( const Zone& ) const = default;
};
std::vector<Zone> Zones( const EventStore& store, uint64_t thread, int64_t begin, int64_t end )
{
    std::vector<Zone> zones;
    store.VisitTimelineZones( thread, begin, end, [&]( const StoreTimelineZone& z, std::string_view ) {
        zones.push_back( { z.id, z.start, z.end, z.depth, z.source } );
        return true;
    } );
    return zones;
}
}
int main()
{
    auto root = std::filesystem::temp_directory_path() / std::filesystem::path( "tracy-store-timeline-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    try
    {
        std::filesystem::create_directory( root );
        auto p = root / "timeline.tracy-stream";
        Fixture( p );
        const auto info = EventStore::Build( p );
        Check( info.cpuZones == 1 + Children + Siblings + Others, "fixture zone count" );
        auto store = EventStore::Open( p );
        const auto last = store->Info().lastNs;
        const int64_t a0 = AStart - InitBegin, a1 = AEnd - InitBegin;

        // Block spans and top-level counts from the block index.
        std::map<uint64_t, uint64_t> top;
        std::vector<std::pair<int64_t, int64_t>> spans;
        bool nestedOnly = false;
        for( uint64_t thread : { 1, 2 } )
        {
            int64_t previous = INT64_MIN;
            store->VisitTimelineBlocks( thread, 0, last + 1, [&]( int64_t start, int64_t end, uint64_t count ) {
                Check( start >= previous && end >= start, "blocks in zone order with valid spans" );
                previous = start;
                top[thread] += count;
                if( thread == 1 && !count ) nestedOnly = true;
                if( thread == 1 && count ) spans.emplace_back( start, end );
                return true;
            } );
        }
        Check( top[1] == 1 + Siblings && top[2] == Others, "top-level zones per thread from block summaries" );
        Check( nestedOnly, "a block of nested zones only reports no top-level zones" );
        Check( !spans.empty() && spans.front().first <= a0 && spans.front().second >= a1, "the block of the long top-level zone spans its children" );
        uint64_t visited = 0;
        store->VisitTimelineBlocks( 1, 0, last + 1, [&]( int64_t, int64_t, uint64_t ) { return ++visited < 1; } );
        Check( visited == 1, "block visit stops when asked" );
        uint64_t inside = 0;
        store->VisitTimelineBlocks( 2, 0, a1, [&]( int64_t, int64_t, uint64_t count ) { inside += count; return true; } );
        Check( inside == 0, "blocks outside the range are not visited" );

        // A wide range reads the thread's blocks by key, a narrow one through
        // the time index; both return the zones overlapping the range.
        const auto all = Zones( *store, 1, 0, last + 1 );
        Check( all.size() == 1 + Children + Siblings, "every zone of thread 1 over a wide range" );
        Check( std::is_sorted( all.begin(), all.end(), []( const Zone& x, const Zone& y ) { return x.id < y.id; } ), "zones in preorder" );
        Check( all.front().depth == 0 && all.front().start == a0 && all.front().end == a1 && all[1].depth == 1, "top-level zone before its children" );
        const int64_t n0 = a0 + ( a1 - a0 ) / 2, n1 = n0 + 1000;
        Check( double( n1 - n0 ) * 8 < double( last ), "a narrow range" );
        const auto narrow = Zones( *store, 1, n0, n1 );
        std::vector<Zone> expected;
        for( const auto& z : all )
            if( z.start < n1 && ( z.end > n0 || ( z.end == z.start && z.start >= n0 ) ) ) expected.push_back( z );
        Check( !narrow.empty() && narrow == expected, "the time index returns the overlapping zones of the wide read" );

        // Zone counts per thread, from one pass over the blocks or the time index.
        std::map<uint64_t, uint64_t> wide, few;
        for( const auto& [thread, count] : store->TimelineZoneCounts( 0, last + 1 ) ) wide[thread] = count;
        Check( wide[1] == 1 + Children + Siblings && wide[2] == Others, "zone counts over the whole capture" );
        for( const auto& [thread, count] : store->TimelineZoneCounts( n0, n1 ) ) few[thread] = count;
        Check( few[1] >= narrow.size() && few[1] < wide[1] && !few.contains( 2 ), "zone counts of a narrow range are a bound from its blocks" );

        // A sibling handle reads the same store without opening it again.
        auto sibling = store->OpenSibling();
        Check( sibling->Info().cpuZones == store->Info().cpuZones && sibling->Info().sourceSha256 == store->Info().sourceSha256 && sibling->Info().dataSha256 == store->Info().dataSha256, "sibling reports the same store" );
        Check( Zones( *sibling, 1, n0, n1 ) == narrow, "sibling reads the same zones" );
        store.reset();
        Check( Zones( *sibling, 2, 0, last + 1 ).size() == Others, "sibling outlives the handle it came from" );
        std::cout << "Timeline store tests passed. Evidence: " << root.string() << '\n';
        sibling.reset();
        std::filesystem::remove_all( root );
        return 0;
    }
    catch( const std::exception& e )
    {
        std::cerr << "FAIL: " << e.what() << "\nEvidence: " << root.string() << '\n';
        return 1;
    }
}

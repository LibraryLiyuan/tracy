#include "../../public/common/TracyProtocol.hpp"
#include "../../public/common/TracyQueue.hpp"
#include "../../public/common/tracy_lz4.hpp"
#include "../src/TracyEventStore.hpp"
#include "../src/TracyStreamJournal.hpp"
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace tracy;
using namespace tracy::stream;
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
void Fixture( const std::filesystem::path& path, int invalid = 0, uint32_t count = 0, bool longName = false )
{
    std::string error;
    auto writer = JournalWriter::CreateFileJournal( path, MakeFileHeader( ProtocolVersion ), false, {}, error );
    Check( bool( writer ), error.c_str() );
    uint64_t sequence = 0;
    const auto append = [&]( RecordType t, uint32_t flags, std::span<const uint8_t> b ) { Check( writer->Append( t, flags, b, ++sequence, error ), error.c_str() ); };
    std::vector<uint8_t> b( 24 );
    b[0] = 2;
    b[2] = 24;
    const uint32_t protocol = ProtocolVersion;
    memcpy( b.data() + 4, &protocol, 4 );
    append( RecordType::SessionBegin, 0, b );
    const uint8_t handshake = HandshakeWelcome;
    append( RecordType::ClientToServer, RecordFlagHandshake, { &handshake, 1 } );
    WelcomeMessage welcome{};
    welcome.initBegin = 1000;
    welcome.initEnd = 1010;
    welcome.timerMul = 1;
    strcpy_s( welcome.programName, "Fiber fixture" );
    append( RecordType::ClientToServer, RecordFlagHandshake, { reinterpret_cast<const uint8_t*>( &welcome ), sizeof( welcome ) } );
    const auto flush = [&] { std::vector<uint8_t> out( LZ4Size + 4 ); const int n = LZ4_compress_default( reinterpret_cast<char*>( b.data() ), reinterpret_cast<char*>( out.data() + 4 ), int( b.size() ), LZ4Size ); Check( n > 0, "compress references" ); memcpy( out.data(), &n, 4 ); out.resize( n + 4 ); append( RecordType::ClientToServer, RecordFlagCompressedFrame, out ); b.clear(); };
    b.clear();
    if( count )
    {
        ServerQueryPacket source{ ServerQuerySourceLocation, 0x123, 0 };
        append( RecordType::ServerToClient, RecordFlagServerQuery, { reinterpret_cast<const uint8_t*>( &source ), sizeof( source ) } );
        Event( b, QueueType::SourceLocation, QueueSourceLocation{ 0, 0, 0, 1, 0, 0, 0 } );
        for( uint32_t n = 0; n < count; ++n )
        {
            const uint64_t key = 0x10000ull + n;
            ServerQueryPacket name{ ServerQueryFiberName, key, 0 };
            append( RecordType::ServerToClient, RecordFlagServerQuery, { reinterpret_cast<const uint8_t*>( &name ), sizeof( name ) } );
            Event( b, QueueType::ThreadContext, QueueThreadContext{ 7 } );
            Event( b, QueueType::FiberEnter, QueueFiberEnter{ 1000 + int64_t( n ) * 1800000000000ll / count, key, 7, int32_t( n % 8 ) } );
            Event( b, QueueType::ZoneBegin, QueueZoneBegin{ { 1000000 }, 0x123 } );
            Event( b, QueueType::ZoneEnd, QueueZoneEnd{ 2000000 } );
            Event( b, QueueType::FiberLeave, QueueFiberLeave{ 1000000, 7 } );
            const auto label = longName ? std::string( 6000, 'F' ) : "Fiber_" + std::to_string( n );
            Event( b, QueueType::FiberName, QueueStringTransfer{ key } );
            Bytes( b, uint16_t( label.size() ) );
            b.insert( b.end(), label.begin(), label.end() );
            flush();
        }
        b.assign( 32, 0 );
        b[0] = 1;
        b[2] = 32;
        b[4] = 3;
        append( RecordType::SessionEnd, RecordFlagTerminal, b );
        return;
    }
    Event( b, QueueType::ThreadContext, QueueThreadContext{ 7 } );
    Event( b, QueueType::ZoneBegin, QueueZoneBegin{ { 1050 }, 0x124 } );
    Event( b, QueueType::FiberEnter, QueueFiberEnter{ 50, 7, 7, 3 } );
    Event( b, QueueType::JnZoneBeginCallsite, QueueJnZoneBeginCallsite{ { { 10 }, 0x123 }, 33 } );
    b.push_back( uint8_t( QueueType::SingleStringData ) );
    Bytes( b, uint16_t( 13 ) );
    const char message[] = "Fiber message";
    b.insert( b.end(), message, message + 13 );
    Event( b, QueueType::Message, QueueMessage{ 1115 } );
    Event( b, QueueType::CallstackPayload, QueueStringTransfer{ 1 } );
    Bytes( b, uint16_t( 8 ) );
    Bytes( b, uint64_t( 0x1001 ) );
    b.push_back( uint8_t( QueueType::Callstack ) );
    flush();
    Event( b, QueueType::FiberLeave, QueueFiberLeave{ 10, 7 } );
    Event( b, QueueType::ZoneEnd, QueueZoneEnd{ 10 } );
    Event( b, QueueType::ThreadContext, QueueThreadContext{ 8 } );
    Event( b, QueueType::FiberEnter, QueueFiberEnter{ 1200, 7, 8, 99 } );
    Event( b, QueueType::ZoneBeginCallstack, QueueZoneBegin{ { 10 }, 0x124 } );
    Event( b, QueueType::ZoneEnd, QueueZoneEnd{ 10 } );
    Event( b, QueueType::FiberEnter, QueueFiberEnter{ 10, 9, 8, -5 } );
    Event( b, QueueType::ZoneBegin, QueueZoneBegin{ { 10 }, 0x124 } );
    Event( b, QueueType::ZoneEnd, QueueZoneEnd{ 10 } );
    Event( b, QueueType::FiberLeave, QueueFiberLeave{ 10, 8 } );
    Event( b, QueueType::FiberEnter, QueueFiberEnter{ 10, 7, 8, 99 } );
    Event( b, QueueType::ZoneEnd, QueueZoneEnd{ 10 } );
    Event( b, QueueType::FiberLeave, QueueFiberLeave{ 10, 8 } );
    if( invalid ) Event( b, QueueType::FiberLeave, QueueFiberLeave{ 10, 8 } );
    flush();
    for( uint64_t key : { 7ull, 9ull } )
    {
        ServerQueryPacket q{ ServerQueryFiberName, key, 0 };
        append( RecordType::ServerToClient, RecordFlagServerQuery, { reinterpret_cast<const uint8_t*>( &q ), sizeof( q ) } );
    }
    for( auto pair : { std::pair{ 7ull, "Fiber A" }, std::pair{ 9ull, "Fiber B" } } )
    {
        Event( b, QueueType::FiberName, QueueStringTransfer{ pair.first } );
        Bytes( b, uint16_t( strlen( pair.second ) ) );
        b.insert( b.end(), pair.second, pair.second + strlen( pair.second ) );
    }
    Event( b, QueueType::JnCallsiteDefinition, QueueJnCallsiteDefinition{ 0x123, 31, 7, 0, 3, 64, 4 } );
    Event( b, QueueType::JnCallsiteDefinition, QueueJnCallsiteDefinition{ 0x123, 33, 8, 0, 3, 2, 2 } );
    flush();
    b.assign( 32, 0 );
    b[0] = 1;
    b[2] = 32;
    b[4] = 3;
    append( RecordType::SessionEnd, RecordFlagTerminal, b );
}
int main( int argc, char** )
{
    const auto root = std::filesystem::temp_directory_path() / ( "tracy-fiber-store-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    try
    {
        std::filesystem::create_directory( root );
        if( argc > 1 )
        {
            const auto path = root / "fiber-scale.tracy-stream";
            Fixture( path, 0, 20000 );
            StoreOptions options;
            options.cacheBytes = 4ull << 20;
            options.resultBytes = 65536;
            const auto start = std::chrono::steady_clock::now();
            EventStore::Build( path, options );
            auto store = EventStore::Open( path, options );
            Check( store->Info().fibers == 20000 && store->Info().cpuZones == 20000 && store->Info().contextRegions == 20000, "high-cardinality fibers and scheduling regions" );
            for( int minute : { 1, 10, 20, 29 } )
            {
                const auto page = store->Zones( int64_t( minute ) * 60000000000ll, int64_t( minute ) * 60000000000ll + 1000000000ll, {} );
                Check( page.values.size() >= 11 && page.values.size() <= 12, "30-minute Fiber zone windows" );
            }
            auto tail = store->Fibers( 0x100000000ull + 19998, 1 ).values[0];
            Check( tail.id == 0x100000000ull + 19999 && tail.key == 0x10000ull + 19999 && tail.name == "Fiber_19999" && tail.running == 4000000, "late Fiber metadata and active duration" );
            store.reset();
            store = EventStore::Open( path, options );
            Check( store->Fibers( tail.id - 1, 1 ).values[0].key == tail.key, "Fiber identity survives eviction and reopen" );
            std::cout << "Fiber scale passed: 20000 logical threads; 1800-second span; seconds=" << std::chrono::duration<double>( std::chrono::steady_clock::now() - start ).count() << "; store_bytes=" << std::filesystem::file_size( EventStore::Directory( path ) / "events.sqlite" ) << "; evidence=" << root.string() << '\n';
            return 0;
        }
        const auto path = root / "fibers.tracy-stream";
        Fixture( path );
        EventStore::Build( path );
        auto store = EventStore::Open( path );
        const auto zones = store->Zones( 0, 1000, {} );
        Check( zones.values.size() == 4, "four physical/logical zones retained" );
        Check( zones.values[0].thread == 7 && zones.values[0].parent == 0 && zones.values[0].end == 130, "physical zone resumes outside the fiber" );
        Check( zones.values[1].thread == 0x100000000ull && zones.values[1].parent == 0 && zones.values[1].depth == 0 && zones.values[1].end == 280, "fiber has a native logical identity independent of its raw pointer and physical thread" );
        Check( zones.values[2].parent == zones.values[1].id && zones.values[2].thread == zones.values[1].thread && zones.values[2].depth == 1, "fiber hierarchy survives migration" );
        Check( store->Messages( 0, 1000 ).values[0].thread == 0x100000000ull && store->Messages( 0, 1000 ).values[0].text == "Fiber message", "messages use the logical Fiber identity" );
        Check( zones.values[2].stack != 0 && store->Stack( zones.values[2].stack ) == std::vector<uint64_t>{ 0x1001 }, "staged stack follows the Fiber across physical threads" );
        Check( zones.values[3].thread == 0x100000001ull && zones.values[3].parent == 0, "distinct logical fiber identity" );
        Check( zones.values[1].wireCallsite == 33 && zones.values[1].callsite == 31 && zones.values[1].provenance == 3 && zones.values[1].unavailableReason == 4, "wire callsite retained separately from snapshot's first source/stack match" );
        const auto fibers = store->Fibers();
        Check( fibers.values.size() == 2 && store->Info().fibers == 2, "Fiber count and paging" );
        Check( fibers.values[0].id == 0x100000000ull && fibers.values[0].key == 7 && fibers.values[0].name == "Fiber A" && fibers.values[0].groupHint == 3 && fibers.values[0].running == 70 && fibers.values[0].regions == 3, "first Fiber hint persists and closed intervals match snapshot running time" );
        Check( fibers.values[1].key == 9 && fibers.values[1].name == "Fiber B" && fibers.values[1].groupHint == -5 && fibers.values[1].running == 30, "second Fiber metadata" );
        const auto regions = store->ContextRegions( 0, 1000 );
        Check( regions.values.size() == 4 && store->Info().cpuRegions == 0, "Fiber scheduling stays distinct from OS CPU schedules" );
        Check( regions.values[0].thread == 0x100000000ull && regions.values[0].relatedThread == 7 && regions.values[0].start == 100 && regions.values[0].end == 120 && regions.values[0].wakeup == 100 && regions.values[0].reason == 99 && regions.values[0].state == -1, "physical/logical Fiber scheduling fields" );
        Check( regions.values[1].relatedThread == 8 && regions.values[1].start == 200 && regions.values[1].end == 230, "implicit Fiber switch closes prior region" );
        auto sites = store->Callsites( 0, 1 );
        Check( sites.more && sites.values[0].id == 31 && sites.values[0].source == 0x123 && sites.values[0].thread == 7 && sites.values[0].domain == 0 && sites.values[0].flags == 64 && sites.values[0].reason == 4, "all callsite definition fields and protocol ordering" );
        Check( store->Callsites( sites.next, 1 ).values[0].id == 33, "callsite stable event cursor" );
        auto live = LiveEventStore::Create( path );
        Check( live->Update( 4 ), "partial Fiber revision" );
        auto first = live->Snapshot();
        Check( first->Fibers().values[0].running == 0 && !first->Zones( 0, 1000, {} ).values[1].end, "open Fiber state" );
        while( live->Update( 2 ) )
        {
        }
        Check( live->Snapshot()->Fibers().values[0].running == 70 && first->Fibers().values[0].running == 0, "migrating Fiber does not mutate prior revisions" );
        const auto invalid = root / "invalid-leave.tracy-stream";
        Fixture( invalid, 1 );
        bool rejected = false;
        try
        {
            EventStore::Build( invalid );
        }
        catch( const std::exception& )
        {
            rejected = true;
        }
        Check( rejected, "leave without an active Fiber is rejected" );
        const auto longPath = root / "long-name.tracy-stream";
        Fixture( longPath, 0, 1, true );
        EventStore::Build( longPath );
        auto names = EventStore::Open( longPath );
        const auto name = names->Fibers().values[0];
        Check( name.nameMore && name.nameBytes == 6000 && name.name.size() == 4096, "long Fiber name preview is explicit" );
        Check( names->FiberNameBytes( name.id, 4096, 4096 ).data == std::vector<uint8_t>( 1904, 'F' ), "full Fiber name remains accessible" );
        bool cancel = false;
        StoreOptions options;
        options.cancelled = [&] { return cancel; };
        store = EventStore::Open( path, options );
        cancel = true;
        bool cancelled = false;
        try
        {
            store->Fibers();
        }
        catch( const std::exception& )
        {
            cancelled = true;
        }
        Check( cancelled, "Fiber metadata query cancellation" );
        std::cout << "Fiber tests passed. Evidence: " << root.string() << '\n';
        return 0;
    }
    catch( const std::exception& e )
    {
        std::cerr << "FAIL: " << e.what() << "\nEvidence: " << root.string() << '\n';
        return 1;
    }
}

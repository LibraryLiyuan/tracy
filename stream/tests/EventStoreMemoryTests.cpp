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
void Check( bool value, const char* message )
{
    if( !value ) throw std::runtime_error( message );
}
template<class T>
void Bytes( std::vector<uint8_t>& b, const T& v )
{
    const auto p = reinterpret_cast<const uint8_t*>( &v );
    b.insert( b.end(), p, p + sizeof( v ) );
}
template<class T>
void Event( std::vector<uint8_t>& b, QueueType t, const T& v )
{
    b.push_back( uint8_t( t ) );
    Bytes( b, v );
}
void Alloc( std::vector<uint8_t>& b, QueueType type, int64_t time, uint32_t thread, uint64_t address, uint64_t size )
{
    QueueMemAlloc event{};
    event.time = time;
    event.thread = thread;
    event.ptr = address;
    for( int i = 0; i < 6; ++i ) event.size[i] = char( size >> ( i * 8 ) );
    Event( b, type, event );
}
void CombinedStack( std::vector<uint8_t>& b, bool native, uint16_t nativeFrames = 1, uint8_t managedFrames = 1 )
{
    if( native )
    {
        Event( b, QueueType::CallstackPayload, QueueStringTransfer{ 2 } );
        Bytes( b, uint16_t( nativeFrames * 8 ) );
        for( uint16_t n = 0; n < nativeFrames; ++n ) Bytes( b, uint64_t( 0x12345 ) );
    }
    std::vector<uint8_t> frames;
    Bytes( frames, managedFrames );
    const char name[] = "ManagedAllocate", file[] = "Fixture.cs";
    for( uint8_t n = 0; n < managedFrames; ++n )
    {
        Bytes( frames, uint32_t( 42 ) );
        Bytes( frames, uint16_t( sizeof( name ) - 1 ) );
        frames.insert( frames.end(), name, name + sizeof( name ) - 1 );
        Bytes( frames, uint16_t( sizeof( file ) - 1 ) );
        frames.insert( frames.end(), file, file + sizeof( file ) - 1 );
    }
    Event( b, QueueType::CallstackAllocPayload, QueueStringTransfer{ 3 } );
    Bytes( b, uint16_t( frames.size() ) );
    b.insert( b.end(), frames.begin(), frames.end() );
}
void Fixture( const std::filesystem::path& path, int fault = 0, bool largeStack = false )
{
    std::string error;
    auto writer = JournalWriter::CreateFileJournal( path, MakeFileHeader( ProtocolVersion ), false, {}, error );
    Check( bool( writer ), error.c_str() );
    uint64_t sequence = 0;
    auto append = [&]( RecordType type, uint32_t flags, std::span<const uint8_t> b ) { Check( writer->Append( type, flags, b, ++sequence, error ), error.c_str() ); };
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
    if( fault == 2 ) welcome.flags |= WelcomeFlag::IgnoreMemFaults;
    strcpy_s( welcome.programName, "Memory fixture" );
    append( RecordType::ClientToServer, RecordFlagHandshake, { reinterpret_cast<const uint8_t*>( &welcome ), sizeof( welcome ) } );
    ServerQueryPacket query{ ServerQueryString, 1, 0 };
    append( RecordType::ServerToClient, RecordFlagServerQuery, { reinterpret_cast<const uint8_t*>( &query ), sizeof( query ) } );
    query = { ServerQuerySourceLocation, 99, 0 };
    append( RecordType::ServerToClient, RecordFlagServerQuery, { reinterpret_cast<const uint8_t*>( &query ), sizeof( query ) } );
    b.clear();
    auto flush = [&] {
        std::vector<uint8_t> out( LZ4Size + 4 );
        const int size = LZ4_compress_default( reinterpret_cast<char*>( b.data() ), reinterpret_cast<char*>( out.data() + 4 ), int( b.size() ), LZ4Size );
        Check( size > 0, "compress memory fixture" );
        memcpy( out.data(), &size, 4 );
        out.resize( size + 4 );
        append( RecordType::ClientToServer, RecordFlagCompressedFrame, out );
        b.clear();
    };
    Event( b, QueueType::ThreadContext, QueueThreadContext{ 7 } );
    Event( b, QueueType::SourceLocation, QueueSourceLocation{ 0, 0, 0, 42, 0, 0, 0 } );
    if( fault == 1 || fault == 2 || fault == 4 ) Event( b, QueueType::MemFree, QueueMemFree{ 50, 7, uint64_t( fault == 4 ? 0 : 0xBAD ) } );
    Alloc( b, QueueType::MemAlloc, fault == 1 || fault == 2 || fault == 4 ? 1050 : 1100, 7, 0xf000000000000010ull, 1ull << 40 );
    if( fault == 3 ) Alloc( b, QueueType::MemAlloc, 0, 7, 0xf000000000000010ull, 1 );
    flush();
    Event( b, QueueType::MemFree, QueueMemFree{ 50, 8, 0xf000000000000010ull } );
    Event( b, QueueType::MemNamePayload, QueueMemNamePayload{ 1 } );
    Alloc( b, QueueType::MemAllocNamed, 10, 7, 0x100, 2048 );
    flush();
    CombinedStack( b, true, largeStack ? 8191 : 1, largeStack ? 64 : 1 );
    b.push_back( uint8_t( QueueType::CallstackSerial ) );
    Alloc( b, QueueType::MemAllocCallstack, 5, 7, 0x200, 123 );
    CombinedStack( b, false );
    b.push_back( uint8_t( QueueType::CallstackSerial ) );
    Event( b, QueueType::MemFreeCallstack, QueueMemFree{ 5, 8, 0x200 } );
    // Existing Client versions emit a serial stack followed by plain discard;
    // the Worker leaves that stack unused and allows the next one to replace it.
    CombinedStack( b, false );
    b.push_back( uint8_t( QueueType::CallstackSerial ) );
    Event( b, QueueType::MemDiscard, QueueMemDiscard{ 30, 9, 1 } );
    CombinedStack( b, false );
    b.push_back( uint8_t( QueueType::CallstackSerial ) );
    Alloc( b, QueueType::MemAllocCallstack, 10, 8, 0xf000000000000010ull, 999 );
    Event( b, QueueType::StringData, QueueStringTransfer{ 1 } );
    const char name[] = "Named allocator";
    Bytes( b, uint16_t( sizeof( name ) - 1 ) );
    b.insert( b.end(), name, name + sizeof( name ) - 1 );
    CombinedStack( b, false );
    b.push_back( uint8_t( QueueType::CallstackAlloc ) );
    Event( b, QueueType::ZoneBeginCallstack, QueueZoneBegin{ { 1180 }, 99 } );
    Event( b, QueueType::ZoneEnd, QueueZoneEnd{ { 10 } } );
    flush();
    b.assign( 32, 0 );
    b[0] = 1;
    b[2] = 32;
    b[4] = 3;
    append( RecordType::SessionEnd, RecordFlagTerminal, b );
}
int main()
{
    const auto root = std::filesystem::temp_directory_path() / ( "tracy-store-memory-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    try
    {
        std::filesystem::create_directory( root );
        const auto path = root / "memory.tracy-stream";
        Fixture( path );
        EventStore::Build( path );
        auto store = EventStore::Open( path );
        auto all = store->Allocations( 0, 1000 );
        Check( all.values.size() == 4, "allocation lifetimes are indexed" );
        const auto& first = all.values[0];
        Check( first.address == 0xf000000000000010ull && first.size == ( 1ull << 40 ) && first.allocated == 100 && first.freed == 150 && first.allocationThread == 7 && first.freeThread == 8, "cross-record 64-bit allocation and release identity" );
        Check( store->Allocations( 135, 145 ).values.size() == 1 && store->Allocations( 150, 160 ).values.empty(), "allocation half-open overlap" );
        auto named = store->Allocations( 195, 205, 1 );
        Check( named.values.size() == 1 && named.values[0].freed == 200 && named.values[0].freeThread == 9, "named pool discard closes active allocations" );
        auto open = store->Allocations( 290, 300 );
        Check( open.values.size() == 1 && open.values[0].size == 999 && !open.values[0].freed, "address reuse keeps distinct stable lifetimes and explicit open end" );
        auto pools = store->MemoryPools();
        Check( pools.values.size() == 2, "default and named pools are paged" );
        Check( pools.values[0].id == 0 && pools.values[0].active == 1 && pools.values[0].activeBytes == 999 && pools.values[0].allocations == 3 && pools.values[0].frees == 2, "default pool final counters" );
        Check( pools.values[1].name == "Named allocator" && pools.values[1].active == 0 && pools.values[1].frees == 1, "delayed pool name and discard counters" );
        auto mixed = store->StackFrames( all.values[2].allocationStack );
        Check( mixed.values.size() == 2 && mixed.values[0].address == 0 && mixed.values[0].name == "ManagedAllocate" && mixed.values[0].file == "Fixture.cs" && mixed.values[0].line == 42 && mixed.values[1].address == 0x12345, "managed frames precede native allocation frames" );
        Check( store->StackFrames( all.values[2].freeStack ).values[0].name == "ManagedAllocate", "managed free stack retains definition" );
        auto cpu = store->Zones( 175, 195, {} );
        Check( cpu.values.size() == 1 && store->StackFrames( cpu.values[0].stack ).values[0].name == "ManagedAllocate", "combined CPU stack reuses the same paged definitions" );
        auto page = store->Allocations( 0, 1000, {}, 0, 1 );
        Check( page.more && page.values[0].id == first.id && page.next == first.id, "allocation stable cursor" );
        store.reset();
        store = EventStore::Open( path );
        Check( store->Allocation( first.id )->freed == 150, "allocation identity survives reopen" );
        bool cancelled = false;
        StoreOptions options;
        options.cancelled = [&] { return cancelled; };
        store = EventStore::Open( path, options );
        cancelled = true;
        bool rejected = false;
        try
        {
            store->Allocations( 0, 1000 );
        }
        catch( const std::exception& )
        {
            rejected = true;
        }
        Check( rejected, "short memory queries honor cancellation" );
        cancelled = false;
        Check( store->Allocation( first.id )->size == ( 1ull << 40 ), "memory reader remains usable after cancellation" );
        for( int fault : { 1, 2, 3, 4 } )
        {
            const auto input = root / ( "fault-" + std::to_string( fault ) + ".tracy-stream" );
            Fixture( input, fault );
            bool failed = false;
            try
            {
                EventStore::Build( input );
            }
            catch( const std::exception& )
            {
                failed = true;
            }
            const bool invalid = fault == 1 || fault == 3;
            Check( failed == invalid, "allocation faults respect welcome policy and null free semantics" );
            if( invalid ) Check( std::filesystem::exists( input ) && !std::filesystem::exists( EventStore::Directory( input ) ), "invalid memory input preserves the journal without publishing a store" );
            else Check( EventStore::Open( input )->Allocations( 0, 1000 ).values[0].allocated == 100, "ignored frees still advance the serial clock" );
        }
        const auto large = root / "large-stack.tracy-stream";
        Fixture( large, 0, true );
        EventStore::Build( large );
        StoreOptions smallPages;
        smallPages.resultBytes = 65536;
        auto symbols = EventStore::Open( large, smallPages );
        const auto stack = symbols->Allocations( 0, 1000 ).values[2].allocationStack;
        uint64_t cursor = 0, count = 0;
        for( ;; )
        {
            const auto page = symbols->StackFrames( stack, cursor, 4096 );
            for( const auto& frame : page.values )
            {
                Check( count < 64 ? frame.name == "ManagedAllocate" : frame.address == 0x12345, "large mixed stack retains complete frame order" );
                ++count;
            }
            if( !page.more ) break;
            Check( page.next > cursor, "large symbol page cursor progresses" );
            cursor = page.next;
        }
        Check( count == 8255, "combined stack is paged even when raw addresses exceed the result budget" );
        std::cout << "Memory tests passed. Evidence: " << root.string() << '\n';
        return 0;
    }
    catch( const std::exception& error )
    {
        std::cerr << "FAIL: " << error.what() << "\nEvidence: " << root.string() << '\n';
        return 1;
    }
}

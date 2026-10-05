#include "../../capture/src/ProcessMemory.hpp"
#include "../../public/common/TracyProtocol.hpp"
#include "../../public/common/TracyQueue.hpp"
#include "../../public/common/tracy_lz4.hpp"
#include "../src/TracyEventStore.hpp"
#include "../src/TracyStreamJournal.hpp"
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
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
void Event( std::vector<uint8_t>& b, QueueType t, const T& v )
{
    b.push_back( uint8_t( t ) );
    Bytes( b, v );
}
constexpr uint32_t Count = 40000;
constexpr uint64_t Address = 0xf000000000000000ull, Size = ( 1ull << 48 ) - 1;
void Fixture( const std::filesystem::path& path, bool discard )
{
    std::string error;
    auto writer = JournalWriter::CreateFileJournal( path, MakeFileHeader( ProtocolVersion ), false, {}, error );
    Check( bool( writer ), error.c_str() );
    uint64_t sequence = 0;
    auto append = [&]( RecordType t, uint32_t flags, std::span<const uint8_t> b ) { Check( writer->Append( t, flags, b, ++sequence, error ), error.c_str() ); };
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
    strcpy_s( welcome.programName, "Memory scale" );
    append( RecordType::ClientToServer, RecordFlagHandshake, { reinterpret_cast<const uint8_t*>( &welcome ), sizeof( welcome ) } );
    if( discard )
    {
        const ServerQueryPacket q{ ServerQueryString, 1, 0 };
        append( RecordType::ServerToClient, RecordFlagServerQuery, { reinterpret_cast<const uint8_t*>( &q ), sizeof( q ) } );
    }
    b.clear();
    auto flush = [&] {
        if( b.empty() ) return;
        std::vector<uint8_t> out( LZ4Size + 4 );
        const int n = LZ4_compress_default( reinterpret_cast<char*>( b.data() ), reinterpret_cast<char*>( out.data() + 4 ), int( b.size() ), LZ4Size );
        Check( n > 0, "compress memory scale" );
        memcpy( out.data(), &n, 4 );
        out.resize( n + 4 );
        append( RecordType::ClientToServer, RecordFlagCompressedFrame, out );
        b.clear();
    };
    int64_t serial = 0;
    for( uint32_t n = 0; n < Count; ++n )
    {
        const int64_t time = 1100 + int64_t( n ) * 1800000000000ll / Count;
        if( discard ) Event( b, QueueType::MemNamePayload, QueueMemNamePayload{ 1 } );
        QueueMemAlloc allocation{};
        allocation.time = time - serial;
        serial = time;
        allocation.ptr = Address + n * 16;
        allocation.thread = 7;
        for( int byte = 0; byte < 6; ++byte ) allocation.size[byte] = char( Size >> ( byte * 8 ) );
        Event( b, discard ? QueueType::MemAllocNamed : QueueType::MemAlloc, allocation );
        if( b.size() > 100000 ) flush();
    }
    if( discard )
    {
        Event( b, QueueType::MemDiscard, QueueMemDiscard{ 1800000001100ll - serial, 8, 1 } );
        Event( b, QueueType::StringData, QueueStringTransfer{ 1 } );
        const char name[] = "Scale named pool";
        Bytes( b, uint16_t( sizeof( name ) - 1 ) );
        b.insert( b.end(), name, name + sizeof( name ) - 1 );
    }
    Event( b, QueueType::JnFrame, QueueJnFrame{ 1800000001100ll, 0x100000001ull, 1, 1, 2, 0 } );
    flush();
    b.assign( 32, 0 );
    b[0] = 1;
    b[2] = 32;
    b[4] = 3;
    append( RecordType::SessionEnd, RecordFlagTerminal, b );
}
int main()
{
    const auto root = std::filesystem::temp_directory_path() / ( "tracy-store-memory-scale-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    try
    {
        std::filesystem::create_directory( root );
        std::atomic<uint64_t> peakCommit{ 0 }, peakWorkingSet{ 0 };
        std::jthread monitor( [&]( std::stop_token stop ) { while( !stop.stop_requested() ) { const auto m = GetProcessMemoryState(); peakCommit = std::max( peakCommit.load(), m.commit ); peakWorkingSet = std::max( peakWorkingSet.load(), m.workingSet ); std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) ); } } );
        StoreOptions options;
        options.cacheBytes = 4ull << 20;
        options.resultBytes = 65536;
        const auto start = std::chrono::steady_clock::now();
        for( bool discard : { false, true } )
        {
            const auto path = root / ( discard ? "discard.tracy-stream" : "active.tracy-stream" );
            Fixture( path, discard );
            const auto info = EventStore::Build( path, options );
            auto store = EventStore::Open( path, options );
            Check( info.allocations == Count && info.activeAllocations == ( discard ? 0 : Count ), "large active-set counts" );
            const auto pools = store->MemoryPools();
            const auto& pool = pools.values[discard ? 1 : 0];
            Check( pool.activeBytes == ( discard ? 0 : Size * Count ) && pool.frees == ( discard ? Count : 0 ), "unsigned usage exceeds signed 64-bit range without precision loss" );
            Check( pool.low == Address && pool.high == Address + ( Count - 1 ) * 16 + Size, "unsigned address bounds" );
            uint64_t cursor = 0, seen = 0, first = 0;
            for( ;; )
            {
                const auto page = store->Allocations( INT64_MIN, INT64_MAX, {}, cursor, 4096 );
                Check( page.values.size() * sizeof( StoreAllocation ) <= options.resultBytes, "allocation page byte budget" );
                for( const auto& a : page.values )
                {
                    if( !first ) first = a.id;
                    Check( a.size == Size && a.freed.has_value() == discard && ( !discard || a.freeThread == 8 ), "all active/discarded lifetimes remain exact" );
                }
                seen += page.values.size();
                if( !page.more ) break;
                Check( page.next > cursor, "allocation cursor progresses" );
                cursor = page.next;
            }
            Check( seen == Count, "every allocation remains accessible" );
            for( int minute : { 1, 10, 20, 29 } )
            {
                const int64_t time = int64_t( minute ) * 60000000000ll;
                Check( !store->Allocations( time, time + 1000000000ll ).values.empty(), "30-minute random lifetime windows" );
            }
            store.reset();
            store = EventStore::Open( path, options );
            Check( store->Allocation( first )->size == Size, "stable allocation survives eviction and reopen" );
        }
        monitor.request_stop();
        monitor.join();
        Check( peakCommit.load() < 128ull << 20, "memory domain process experiment budget" );
        std::cout << "Memory scale passed; allocations_per_case=" << Count << " seconds=" << std::chrono::duration<double>( std::chrono::steady_clock::now() - start ).count() << " sampled_peak_commit=" << peakCommit.load() << " sampled_peak_working_set=" << peakWorkingSet.load() << " evidence=" << root.string() << '\n';
        return 0;
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << "\nEvidence: " << root.string() << '\n';
        return 1;
    }
}

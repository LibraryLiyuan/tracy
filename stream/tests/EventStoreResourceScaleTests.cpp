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
void Check( bool value, const char* message )
{
    if( !value ) throw std::runtime_error( message );
}
template<class T>
void Bytes( std::vector<uint8_t>& b, const T& value )
{
    const auto p = reinterpret_cast<const uint8_t*>( &value );
    b.insert( b.end(), p, p + sizeof( value ) );
}
template<class T>
void Event( std::vector<uint8_t>& b, QueueType type, const T& value )
{
    b.push_back( uint8_t( type ) );
    Bytes( b, value );
}
void Fixture( const std::filesystem::path& path, bool unique )
{
    std::string error;
    auto writer = JournalWriter::CreateFileJournal( path, MakeFileHeader( ProtocolVersion ), false, {}, error );
    Check( bool( writer ), error.c_str() );
    uint64_t sequence = 0;
    const auto append = [&]( RecordType type, uint32_t flags, std::span<const uint8_t> b ) { Check( writer->Append( type, flags, b, ++sequence, error ), error.c_str() ); };
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
    strcpy_s( welcome.programName, "Resource expansion scale" );
    append( RecordType::ClientToServer, RecordFlagHandshake, { reinterpret_cast<const uint8_t*>( &welcome ), sizeof( welcome ) } );
    const auto flush = [&] { if( b.empty() ) return; std::vector<uint8_t> out( LZ4Size + 4 ); const int n = LZ4_compress_default( reinterpret_cast<char*>( b.data() ), reinterpret_cast<char*>( out.data() + 4 ), int( b.size() ), LZ4Size ); Check( n > 0, "compress set scale" ); memcpy( out.data(), &n, 4 ); out.resize( n + 4 ); append( RecordType::ClientToServer, RecordFlagCompressedFrame, out ); b.clear(); };
    b.clear();
    Event( b, QueueType::ThreadContext, QueueThreadContext{ 7 } );
    const uint32_t count = unique ? 10000 : 20000;
    const uint16_t entries = unique ? 32 : 4096;
    for( uint32_t n = 0; n < count; ++n )
    {
        const uint32_t set = unique ? n + 1 : 1;
        if( unique || n == 0 )
        {
            Event( b, QueueType::JnGpuReferenceSetDefinition, QueueStringTransfer{ set } );
            Bytes( b, uint32_t( entries * sizeof( JnGpuReferenceSetEntry ) ) );
            for( uint32_t entry = 0; entry < entries; ++entry ) Bytes( b, JnGpuReferenceSetEntry{ 0xf000000000000000ull + entry, entry % 16 } );
        }
        const auto time = int64_t( 1100 ) + int64_t( n ) * 1800000000000ll / count;
        Event( b, QueueType::JnGpuReferencePass, QueueJnGpuReferencePass{ time, n + 1, 0x100000000ull + n, n, 2, 0 } );
        Event( b, QueueType::JnGpuReferenceSetUse, QueueJnGpuReferenceSetUse{ n + 1, set, entries, 9, 2 } );
        Event( b, QueueType::JnGpuReferenceEnd, QueueJnGpuReferenceEnd{ time + 100, n + 1, 0x200000000ull + n, entries, 0, 0 } );
        if( b.size() > 100000 ) flush();
    }
    Event( b, QueueType::JnFrame, QueueJnFrame{ 1800000001000ll, 0x100000001ull, count, 1, 2, 0 } );
    flush();
    b.assign( 32, 0 );
    b[0] = 1;
    b[2] = 32;
    b[4] = 3;
    append( RecordType::SessionEnd, RecordFlagTerminal, b );
}
int main()
{
    const auto root = std::filesystem::temp_directory_path() / ( "tracy-resource-scale-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    try
    {
        std::filesystem::create_directory( root );
        std::atomic<uint64_t> peak{ 0 };
        std::jthread monitor( [&]( std::stop_token stop ) { while( !stop.stop_requested() ) { peak = std::max( peak.load(), GetProcessMemoryState().commit ); std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) ); } } );
        StoreOptions options;
        options.cacheBytes = 4ull << 20;
        options.resultBytes = 65536;
        const auto start = std::chrono::steady_clock::now();
        for( bool unique : { false, true } )
        {
            const auto path = root / ( unique ? "definitions.tracy-stream" : "reuse.tracy-stream" );
            Fixture( path, unique );
            const auto info = EventStore::Build( path, options );
            auto store = EventStore::Open( path, options );
            const uint64_t passes = unique ? 10000 : 20000, entries = unique ? 32 : 4096;
            Check( info.gpuReferencePasses == passes && info.gpuReferenceEnds == passes && info.gpuReferenceUses == passes * entries, "logical expansion count" );
            const auto bytes = std::filesystem::file_size( EventStore::Directory( path ) / "events.sqlite" );
            Check( bytes < ( 64ull << 20 ), "reused sets are not materialized as expanded event rows" );
            uint64_t cursor = 0, seen = 0;
            for( ;; )
            {
                const auto page = store->GpuReferences( 0, INT64_MAX, 1, cursor, 4096 );
                Check( page.values.size() * sizeof( StoreGpuReference ) <= options.resultBytes, "expanded page byte budget" );
                for( const auto& use : page.values )
                {
                    Check( use.ordinal == seen && use.resource == 0xf000000000000000ull + seen && use.mask == seen % 16, "full set order after byte-limited paging" );
                    ++seen;
                }
                if( !page.more ) break;
                Check( page.next > cursor, "expanded page makes progress" );
                cursor = page.next;
            }
            Check( seen == entries, "all entries remain individually accessible" );
            for( int minute : { 1, 10, 20, 29 } )
            {
                const auto time = int64_t( minute ) * 60000000000ll;
                const auto page = store->GpuReferences( time, time + 1000000000ll );
                Check( !page.values.empty() && page.values[0].time >= time && page.values[0].time < time + 1000000000ll, "long reference windows" );
            }
            store.reset();
            store = EventStore::Open( path, options );
            Check( store->GpuReferences( 0, INT64_MAX, passes, 0, 1 ).values[0].ordinal == 0, "last set remains addressable after reopen" );
            std::cout << "case=" << ( unique ? "definitions" : "reuse" ) << " expanded=" << info.gpuReferenceUses << " store_bytes=" << bytes << '\n';
        }
        monitor.request_stop();
        monitor.join();
        Check( peak.load() < ( 128ull << 20 ), "resource process experiment budget" );
        std::cout << "Resource scale passed; seconds=" << std::chrono::duration<double>( std::chrono::steady_clock::now() - start ).count() << " sampled_peak_commit=" << peak.load() << " evidence=" << root.string() << '\n';
        return 0;
    }
    catch( const std::exception& e )
    {
        std::cerr << "FAIL: " << e.what() << "\nEvidence: " << root.string() << '\n';
        return 1;
    }
}

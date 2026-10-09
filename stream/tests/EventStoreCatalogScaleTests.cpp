#include "../../capture/src/ProcessMemory.hpp"
#include "../../public/common/TracyJnGpuCatalog.hpp"
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
int main()
{
    const auto root = std::filesystem::temp_directory_path() / ( "tracy-catalog-scale-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    try
    {
        std::filesystem::create_directory( root );
        const auto path = root / "catalog.tracy-stream";
        std::string error;
        auto writer = JournalWriter::CreateFileJournal( path, MakeFileHeader( ProtocolVersion ), false, {}, error );
        Check( bool( writer ), error.c_str() );
        uint64_t sequence = 0;
        const auto append = [&]( RecordType type, uint32_t flags, std::span<const uint8_t> bytes ) { Check( writer->Append( type, flags, bytes, ++sequence, error ), error.c_str() ); };
        std::vector<uint8_t> b( 24 );
        b[0] = 2;
        b[2] = 24;
        const uint32_t protocol = ProtocolVersion;
        memcpy( b.data() + 4, &protocol, 4 );
        append( RecordType::SessionBegin, 0, b );
        const uint8_t flag = HandshakeWelcome;
        append( RecordType::ClientToServer, RecordFlagHandshake, { &flag, 1 } );
        WelcomeMessage welcome{};
        welcome.initBegin = 1000;
        welcome.initEnd = 1010;
        welcome.timerMul = 1;
        strcpy_s( welcome.programName, "Catalog scale" );
        append( RecordType::ClientToServer, RecordFlagHandshake, { reinterpret_cast<const uint8_t*>( &welcome ), sizeof( welcome ) } );
        const auto flush = [&] { if( b.empty() ) return; std::vector<uint8_t> out( LZ4Size + 4 ); const int size = LZ4_compress_default( reinterpret_cast<char*>( b.data() ), reinterpret_cast<char*>( out.data() + 4 ), int( b.size() ), LZ4Size ); Check( size > 0, "compress Catalog scale" ); memcpy( out.data(), &size, 4 ); out.resize( size + 4 ); append( RecordType::ClientToServer, RecordFlagCompressedFrame, out ); b.clear(); };
        constexpr uint64_t Generation = 0xf000000000000001ull;
        constexpr uint32_t Count = 20000, Updates = 2000;
        uint32_t batchSequence = 1;
        b.clear();
        Event( b, QueueType::ThreadContext, QueueThreadContext{ 7 } );
        Event( b, QueueType::JnGpuCatalogControl, QueueJnGpuCatalogControl{ 1050, Generation, Count, batchSequence++, 0, 0, 4 } );
        const auto batch = [&]( uint8_t kind, auto& records ) {
            using T = typename std::decay_t<decltype( records )>::value_type;
            const uint32_t payloadBytes = uint32_t( records.size() * sizeof( T ) );
            const JnGpuCatalogBatchEnvelopeV1 envelope{ JnGpuCatalogBatchMagic, JnGpuCatalogSchemaVersion, JnGpuDetailedEvidenceSchemaVersion, sizeof( T ), 0, uint32_t( records.size() ), payloadBytes, JnGpuCatalogChecksum64( records.data(), payloadBytes ) };
            Event( b, QueueType::JnGpuCatalogBatchData, QueueStringTransfer{ batchSequence } );
            Bytes( b, uint32_t( sizeof( envelope ) + payloadBytes ) );
            Bytes( b, envelope );
            const auto p = reinterpret_cast<const uint8_t*>( records.data() );
            b.insert( b.end(), p, p + payloadBytes );
            Event( b, QueueType::JnGpuCatalogBatch, QueueJnGpuCatalogBatch{ Generation, batchSequence, batchSequence, uint32_t( records.size() ), uint32_t( sizeof( envelope ) + payloadBytes ), kind, 0, 0 } );
            ++batchSequence;
            flush();
        };
        std::vector<JnGpuCatalogResourceRecordV1> resources;
        std::vector<JnGpuCatalogViewRecordV1> views;
        resources.reserve( 250 );
        views.reserve( 250 );
        for( uint32_t n = 0; n < Count; ++n )
        {
            const auto time = int64_t( 1100 ) + int64_t( n ) * 1800000000000ll / Count;
            JnGpuCatalogResourceRecordV1 resource{};
            resource.time = time;
            resource.resourceId = 0xe000000000000000ull + n;
            resource.pointerToken = 0xf000000000000000ull + n;
            resource.capacityBytes = 4096 + n;
            resource.width = resource.capacityBytes;
            resource.definitionRevision = 1;
            resources.push_back( resource );
            if( resources.size() == 250 )
            {
                batch( 0, resources );
                resources.clear();
            }
        }
        for( uint32_t n = 0; n < Count; ++n )
        {
            JnGpuCatalogViewRecordV1 view{};
            view.time = 1101 + int64_t( n ) * 1800000000000ll / Count;
            view.viewId = n + 1;
            view.pointerToken = 0xf000000000000000ull + n;
            view.bufferLengthBytes = 128;
            views.push_back( view );
            if( views.size() == 250 )
            {
                batch( 2, views );
                views.clear();
            }
        }
        Event( b, QueueType::JnGpuCatalogControl, QueueJnGpuCatalogControl{ 1800000002000ll, Generation, Count, batchSequence++, 2, 1, 0 } );
        flush();
        for( uint32_t n = 0; n < Updates; ++n )
        {
            JnGpuCatalogResourceRecordV1 resource{};
            resource.time = 1800000002001ll + n;
            resource.resourceId = 0xe000000000000000ull + n;
            resource.pointerToken = 0xf000000000000000ull + n;
            resource.operation = 1;
            resource.capacityBytes = 8192 + n;
            resource.definitionRevision = 2;
            resources.push_back( resource );
            batch( 0, resources );
            resources.clear();
        }
        b.assign( 32, 0 );
        b[0] = 1;
        b[2] = 32;
        b[4] = 3;
        append( RecordType::SessionEnd, RecordFlagTerminal, b );
        writer.reset();
        std::atomic<uint64_t> peak{ 0 };
        std::jthread monitor( [&]( std::stop_token stop ) { while( !stop.stop_requested() ) { peak = std::max( peak.load(), GetProcessMemoryState().commit ); std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) ); } } );
        const auto start = std::chrono::steady_clock::now();
        StoreOptions options;
        options.cacheBytes = 4ull << 20;
        options.resultBytes = 65536;
        const auto info = EventStore::Build( path, options );
        auto store = EventStore::Open( path, options );
        Check( info.catalogValid && info.catalogGenerations == 1 && info.catalogRecords == Count * 2 + Updates, "Catalog scale counts and quality" );
        uint64_t cursor = 0, seen = 0;
        for( ;; )
        {
            auto page = store->CatalogRecords( 2, Generation, cursor, 4096 );
            for( const auto& record : page.values )
            {
                JnGpuCatalogViewRecordV1 view;
                memcpy( &view, record.data.data(), sizeof( view ) );
                Check( view.resourceId == 0xe000000000000000ull + seen && view.pointerToken == 0, "all pointer relations resolve with bounded state" );
                ++seen;
            }
            if( !page.more ) break;
            Check( page.next > cursor, "Catalog cursor progresses" );
            cursor = page.next;
        }
        Check( seen == Count, "all view records remain accessible" );
        for( int minute : { 1, 10, 20, 29 } )
        {
            const auto time = int64_t( minute ) * 60000000000ll;
            auto page = store->CatalogResources( time, time + 1000000000ll, Generation );
            Check( !page.values.empty() && page.values[0].valid, "long resource lifetime windows" );
        }
        store.reset();
        store = EventStore::Open( path, options );
        Check( store->CatalogGenerations().values[0].unresolved == 0, "resolved generation survives reopen" );
        monitor.request_stop();
        monitor.join();
        Check( peak.load() < ( 128ull << 20 ), "Catalog process experiment budget" );
        std::cout << "Catalog scale passed; resources=" << Count << " views=" << Count << " post_end_batches=" << Updates << " seconds=" << std::chrono::duration<double>( std::chrono::steady_clock::now() - start ).count() << " sampled_peak_commit=" << peak.load() << " evidence=" << root.string() << '\n';
        return 0;
    }
    catch( const std::exception& e )
    {
        std::cerr << "FAIL: " << e.what() << "\nEvidence: " << root.string() << '\n';
        return 1;
    }
}

#include "../../public/common/TracyJnGpuCatalog.hpp"
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
constexpr uint64_t Generation = 0xf000000000000001ull;
template<class T>
void Batch( std::vector<uint8_t>& b, uint32_t sequence, uint8_t kind, const T& record, bool corrupt = false )
{
    JnGpuCatalogBatchEnvelopeV1 envelope{ JnGpuCatalogBatchMagic, JnGpuCatalogSchemaVersion, JnGpuDetailedEvidenceSchemaVersion, sizeof( T ), 0, 1, sizeof( T ), JnGpuCatalogChecksum64( &record, sizeof( record ) ) };
    if( corrupt ) ++envelope.checksum;
    Event( b, QueueType::JnGpuCatalogBatchData, QueueStringTransfer{ sequence } );
    Bytes( b, uint32_t( sizeof( envelope ) + sizeof( record ) ) );
    Bytes( b, envelope );
    Bytes( b, record );
    Event( b, QueueType::JnGpuCatalogBatch, QueueJnGpuCatalogBatch{ Generation, sequence, sequence, 1, uint32_t( sizeof( envelope ) + sizeof( record ) ), kind, uint8_t( kind == 7 ? 2 : kind == 8 ? 3
                                                                                                                                                                                                   : 0 ),
                                                                    0 } );
}
void Fixture( const std::filesystem::path& path, int mode = 0 )
{
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
    const uint8_t welcomeFlag = HandshakeWelcome;
    append( RecordType::ClientToServer, RecordFlagHandshake, { &welcomeFlag, 1 } );
    WelcomeMessage welcome{};
    welcome.initBegin = 1000;
    welcome.initEnd = 1010;
    welcome.timerMul = 1;
    strcpy_s( welcome.programName, "Catalog fixture" );
    append( RecordType::ClientToServer, RecordFlagHandshake, { reinterpret_cast<const uint8_t*>( &welcome ), sizeof( welcome ) } );
    const auto flush = [&] { std::vector<uint8_t> out( LZ4Size + 4 ); const int n = LZ4_compress_default( reinterpret_cast<char*>( b.data() ), reinterpret_cast<char*>( out.data() + 4 ), int( b.size() ), LZ4Size ); Check( n > 0, "compress Catalog" ); memcpy( out.data(), &n, 4 ); out.resize( n + 4 ); append( RecordType::ClientToServer, RecordFlagCompressedFrame, out ); b.clear(); };
    b.clear();
    Event( b, QueueType::ThreadContext, QueueThreadContext{ 7 } );
    Event( b, QueueType::JnGpuCatalogControl, QueueJnGpuCatalogControl{ 1050, Generation, 11, 1, 0, 0, 4 } );
    if( mode == 2 ) Event( b, QueueType::JnGpuCatalogControl, QueueJnGpuCatalogControl{ 1051, Generation, 11, 1, 0, 0, 4 } );
    JnGpuCatalogResourceRecordV1 resource{};
    resource.time = 1100;
    resource.resourceId = 42;
    resource.pointerToken = 0x100;
    resource.capacityBytes = 4096;
    resource.width = 4096;
    resource.operation = 0;
    resource.resourceClass = 1;
    resource.definitionRevision = 1;
    Batch( b, 2, 0, resource, mode == 1 );
    JnGpuCatalogViewRecordV1 view{};
    view.time = 1110;
    view.viewId = 51;
    view.pointerToken = 0x100;
    view.bufferLengthBytes = 1024;
    Batch( b, 3, 2, view );
    if( mode == 3 )
    {
        flush();
        b.assign( 32, 0 );
        b[0] = 1;
        b[2] = 32;
        b[4] = 3;
        append( RecordType::SessionEnd, RecordFlagTerminal, b );
        return;
    }
    Event( b, QueueType::JnGpuCatalogControl, QueueJnGpuCatalogControl{ 1150, Generation, 22, 4, 2, 1, 0 } );
    flush();
    resource.time = 1200;
    resource.operation = mode == 4 ? 1 : 2;
    resource.definitionRevision = 2;
    Batch( b, 5, 0, resource );
    view.time = 1210;
    view.viewId = 52;
    Batch( b, 6, 2, view );
    resource.time = 1220;
    resource.resourceId = 43;
    resource.operation = 0;
    resource.definitionRevision = 1;
    Batch( b, 7, 0, resource );
    view.time = 1225;
    view.viewId = 53;
    Batch( b, 8, 2, view );
    if( mode == 5 )
    {
        JnGpuCatalogRelationRecordV1 relation{};
        relation.time = 1226;
        relation.sourceId = 0xbad;
        relation.targetId = 42;
        relation.flags = 1;
        Batch( b, 9, 5, relation );
    }
    flush();
    b.assign( 32, 0 );
    b[0] = 1;
    b[2] = 32;
    b[4] = 3;
    append( RecordType::SessionEnd, RecordFlagTerminal, b );
}
template<class T>
T Decode( const StoreCatalogRecord& record )
{
    Check( record.data.size() == sizeof( T ), "Catalog record width" );
    T value;
    memcpy( &value, record.data.data(), sizeof( value ) );
    return value;
}
int main()
{
    const auto root = std::filesystem::temp_directory_path() / ( "tracy-catalog-store-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    try
    {
        std::filesystem::create_directory( root );
        const auto path = root / "catalog.tracy-stream";
        Fixture( path );
        EventStore::Build( path );
        auto store = EventStore::Open( path );
        auto generations = store->CatalogGenerations();
        Check( generations.values.size() == 1, "Catalog generation is retained" );
        const auto& generation = generations.values[0];
        Check( generation.id == Generation && generation.valid && generation.ended && generation.sequence == 8 && generation.records == 6 && generation.batches == 6 && generation.unresolved == 1, "post-end generation accounting and non-Core unresolved reference" );
        const auto resources = store->CatalogRecords( 0 );
        Check( resources.values.size() == 3 && Decode<JnGpuCatalogResourceRecordV1>( resources.values[0] ).time == 100, "resource lifecycle records use normalized time" );
        auto views = store->CatalogRecords( 2 );
        Check( views.values.size() == 3, "view records preserved" );
        const auto first = Decode<JnGpuCatalogViewRecordV1>( views.values[0] ), gap = Decode<JnGpuCatalogViewRecordV1>( views.values[1] ), reused = Decode<JnGpuCatalogViewRecordV1>( views.values[2] );
        Check( first.resourceId == 42 && first.pointerToken == 0 && first.time == 110, "view pointer resolves in first lifetime" );
        Check( gap.resourceId == 0 && gap.pointerToken == 0x100 && gap.exactness == 2, "pointer gap remains explicitly partial" );
        Check( reused.resourceId == 43 && reused.pointerToken == 0, "pointer reuse resolves to the second resource" );
        const auto firstLifetime = store->CatalogResources( 120, 130 );
        Check( firstLifetime.values.size() == 1 && firstLifetime.values[0].resource == 42 && firstLifetime.values[0].begin == 100 && firstLifetime.values[0].end == 200, "resource lifetime overlaps a window with no create event" );
        Check( store->CatalogResources( 200, 220 ).values.empty(), "destroy-to-recreate gap is not a live resource" );
        const auto secondLifetime = store->CatalogResources( 225, 226 );
        Check( secondLifetime.values.size() == 1 && secondLifetime.values[0].resource == 43 && !secondLifetime.values[0].end, "second resource stays active after pointer reuse" );
        const auto batches = store->CatalogBatches();
        Check( batches.values.size() == 6 && batches.values[0].transportChecksum != batches.values[0].storedChecksum, "transport and canonical checksums are distinct" );
        auto one = store->CatalogRecords( 2, Generation, 0, 1 );
        Check( one.more && store->CatalogRecords( 2, Generation, one.next, 1 ).values[0].id == views.values[1].id, "Catalog pages preserve stable record IDs" );
        store.reset();
        store = EventStore::Open( path );
        Check( Decode<JnGpuCatalogViewRecordV1>( store->CatalogRecords( 2 ).values[0] ).resourceId == 42, "canonical resolution survives reopen" );
        const auto bad = root / "corrupt.tracy-stream";
        Fixture( bad, true );
        EventStore::Build( bad );
        Check( !EventStore::Open( bad )->CatalogGenerations().values[0].valid, "bad Catalog checksum invalidates Core evidence" );
        for( int mode : { 2, 3, 4, 5 } )
        {
            const auto input = root / ( "quality-" + std::to_string( mode ) + ".tracy-stream" );
            Fixture( input, mode );
            EventStore::Build( input );
            auto quality = EventStore::Open( input );
            Check( quality->Info().catalogPresent && quality->Info().catalogValid == ( mode == 4 ), "duplicate generation, incomplete generation and unresolved Core retain distinct validity" );
            if( mode == 4 )
            {
                const auto last = Decode<JnGpuCatalogViewRecordV1>( quality->CatalogRecords( 2 ).values.back() );
                Check( last.resourceId == 0 && last.pointerToken == 0x100 && last.exactness == 2, "ambiguous overlapping pointer lifetimes remain unresolved" );
            }
        }
        auto live = LiveEventStore::Create( path );
        std::unique_ptr<EventStore> earlier;
        while( live->Update( 1 ) )
        {
            auto view = live->Snapshot();
            if( !earlier && view->Info().catalogRecords == 2 ) earlier = std::move( view );
        }
        Check( earlier && earlier->CatalogRecords( 2 ).values.size() == 1 && live->Snapshot()->CatalogRecords( 2 ).values.size() == 3, "Catalog records are immutable across live revisions" );
        bool cancel = false;
        StoreOptions options;
        options.cancelled = [&] { return cancel; };
        auto reader = EventStore::Open( path, options );
        cancel = true;
        bool rejected = false;
        try
        {
            reader->CatalogRecords( 0 );
        }
        catch( const std::exception& )
        {
            rejected = true;
        }
        Check( rejected, "Catalog queries honor cancellation" );
        cancel = false;
        Check( reader->CatalogGenerations().values[0].valid, "Catalog reader recovers after cancellation" );
        std::cout << "Catalog tests passed. Evidence: " << root.string() << '\n';
        return 0;
    }
    catch( const std::exception& e )
    {
        std::cerr << "FAIL: " << e.what() << "\nEvidence: " << root.string() << '\n';
        return 1;
    }
}

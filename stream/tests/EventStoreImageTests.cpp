#include "../../public/common/TracyProtocol.hpp"
#include "../../public/common/TracyQueue.hpp"
#include "../../public/common/tracy_lz4.hpp"
#include "../src/TracyEventStore.hpp"
#include "../src/TracyStreamJournal.hpp"
#include "sqlite3.h"
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
void Fixture( const std::filesystem::path& path, bool demand = false, int invalid = 0, uint32_t imageCount = 0 )
{
    std::string error;
    auto writer = JournalWriter::CreateFileJournal( path, MakeFileHeader( ProtocolVersion ), false, {}, error );
    Check( bool( writer ), error.c_str() );
    uint64_t seq = 0;
    const auto append = [&]( RecordType t, uint32_t flags, std::span<const uint8_t> b ) { Check( writer->Append( t, flags, b, ++seq, error ), error.c_str() ); };
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
    welcome.flags = demand ? WelcomeFlag::OnDemand : 0;
    append( RecordType::ClientToServer, RecordFlagHandshake, { reinterpret_cast<const uint8_t*>( &welcome ), sizeof( welcome ) } );
    if( demand )
    {
        OnDemandPayloadMessage od{};
        od.frames = 100;
        od.currentTime = 1020;
        append( RecordType::ClientToServer, RecordFlagHandshake, { reinterpret_cast<const uint8_t*>( &od ), sizeof( od ) } );
    }
    const auto flush = [&] { std::vector<uint8_t> out( LZ4Size + 4 ); int n = LZ4_compress_default( reinterpret_cast<char*>( b.data() ), reinterpret_cast<char*>( out.data() + 4 ), int( b.size() ), LZ4Size ); Check( n > 0, "compress images" ); memcpy( out.data(), &n, 4 ); out.resize( n + 4 ); append( RecordType::ClientToServer, RecordFlagCompressedFrame, out ); b.clear(); };
    const auto payload = [&] { Event( b, QueueType::FrameImageData, QueueStringTransfer{ 99 } ); Bytes( b, uint32_t( 8 ) ); Bytes( b, uint64_t( 0xe4e4e4e40000ffffull ) ); };
    const auto image = [&]( uint32_t frame, bool flip ) { Event( b, QueueType::FrameImage, QueueFrameImage{ uint32_t( frame + ( demand ? 100 : 0 ) ), uint16_t( invalid == 4 ? 8 : 4 ), 4, uint8_t( flip ) } ); };
    if( imageCount )
    {
        b.clear();
        for( uint32_t n = 0; n < imageCount; ++n )
        {
            Event( b, QueueType::FrameMarkMsg, QueueFrameMark{ 1000 + int64_t( n + 1 ) * 1800000000000ll / imageCount, 0 } );
            Event( b, QueueType::FrameImageData, QueueStringTransfer{ 99 } );
            Bytes( b, uint32_t( 1024 ) );
            for( int block = 0; block < 128; ++block ) Bytes( b, uint64_t( 0xe4e4e4e40000ffffull ) );
            Event( b, QueueType::FrameImage, QueueFrameImage{ n + 1, 64, 32, uint8_t( n % 2 ) } );
            if( n % 32 == 31 || n + 1 == imageCount ) flush();
        }
        b.assign( 32, 0 );
        b[0] = 1;
        b[2] = 32;
        b[4] = 3;
        append( RecordType::SessionEnd, RecordFlagTerminal, b );
        return;
    }
    b.clear();
    payload();
    flush(); // Pixel data and metadata may cross a live revision.
    if( invalid == 1 ) payload();
    image( 0, false );
    payload();
    image( invalid == 2 ? 0 : 2, true );
    flush(); // Image for base frame 3 precedes that frame.
    Event( b, QueueType::FrameMarkMsg, QueueFrameMark{ 1100, 77 } );
    Event( b, QueueType::FrameMarkMsg, QueueFrameMark{ 1100, 0 } );
    Event( b, QueueType::FrameMarkMsg, QueueFrameMark{ 1200, 0 } );
    payload();
    image( 10, false ); // Retained even when the matching frame never arrives.
    if( invalid == 3 ) payload();
    flush();
    b.assign( 32, 0 );
    b[0] = 1;
    b[2] = 32;
    b[4] = 3;
    append( RecordType::SessionEnd, RecordFlagTerminal, b );
}
int main( int argc, char** )
{
    const auto root = std::filesystem::temp_directory_path() / ( "tracy-image-store-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    try
    {
        std::filesystem::create_directory( root );
        if( argc > 1 )
        {
            const auto path = root / "image-scale.tracy-stream";
            constexpr uint32_t count = 50000;
            Fixture( path, false, 0, count );
            StoreOptions options;
            options.cacheBytes = 4ull << 20;
            options.resultBytes = 65536;
            const auto start = std::chrono::steady_clock::now();
            EventStore::Build( path, options );
            auto store = EventStore::Open( path, options );
            Check( store->Info().frameImages == count, "scale image count" );
            const auto capped = store->FrameImages(0,4096);
            Check(capped.more && capped.values.capacity()*sizeof(StoreFrameImage)+sizeof(capped)<=options.resultBytes,"image metadata page allocation stays within the result budget");
            for( int minute : { 1, 10, 20, 29 } )
            {
                const auto page = store->FrameImagesInRange( int64_t( minute ) * 60000000000ll, int64_t( minute ) * 60000000000ll + 1000000000ll );
                Check( page.values.size() >= 27 && page.values.size() <= 29, "30-minute image window overlap" );
                for( const auto& image : page.values ) Check( store->FrameImageBytes( image.id, 0, 2048 ).data.size() == 1024, "large image store reads stay bounded" );
            }
            const auto first = store->FrameImages( 0, 1 ).values.front();
            store.reset();
            store = EventStore::Open( path, options );
            Check( store->FrameImage( first.id )->frame == first.frame && store->FrameImageBytes( first.id, 0, 2048 ).data[4] == 0x2d, "evicted image data and IDs survive reopen" );
            std::cout << "Image scale passed: " << count << " images, 1800-second span; seconds=" << std::chrono::duration<double>( std::chrono::steady_clock::now() - start ).count() << "; store_bytes=" << std::filesystem::file_size( EventStore::Directory( path ) / "events.sqlite" ) << "; evidence=" << root.string() << '\n';
            return 0;
        }
        const auto path = root / "images.tracy-stream";
        Fixture( path );
        EventStore::Build( path );
        sqlite3* db = nullptr;
        Check( sqlite3_open( ( EventStore::Directory( path ) / "events.sqlite" ).string().c_str(), &db ) == SQLITE_OK, "open image store" );
        sqlite3_stmt* statement = nullptr;
        const auto rc = sqlite3_prepare_v2( db, "SELECT COUNT(*) FROM frame_images", -1, &statement, nullptr );
        const bool indexed = rc == SQLITE_OK && sqlite3_step( statement ) == SQLITE_ROW && sqlite3_column_int64( statement, 0 ) == 3;
        sqlite3_finalize( statement );
        sqlite3_close( db );
        Check( indexed, "all three Frame Images must be indexed, including an image awaiting its frame" );
        StoreOptions options;
        options.cacheBytes = 4ull << 20;
        options.resultBytes = 65536;
        auto store = EventStore::Open( path, options );
        auto first = store->FrameImages( 0, 1 );
        Check( first.more && first.values.size() == 1 && first.values[0].index == 0 && first.values[0].frameIndex == 1 && first.values[0].frame == 2 && first.values[0].start == 10 && first.values[0].end == 100, "first image fields and base frame association" );
        auto second = store->FrameImages( first.next, 1 );
        Check( second.more && second.values[0].index == 1 && second.values[0].frame == 5 && second.values[0].frameIndex == 3 && second.values[0].flipped && second.values[0].start == 200 && !second.values[0].end, "staged image binds to base frame, ignoring named frames" );
        auto third = store->FrameImages( second.next, 1 );
        Check( !third.more && !third.values[0].frame && !third.values[0].start && third.values[0].frameIndex == 11, "unmatched image stays available without invented timing" );
        const std::vector<uint8_t> expected{ 255, 255, 0, 0, 0x2d, 0x2d, 0x2d, 0x2d };
        auto chunk = store->FrameImageBytes( first.next, 0, 3 );
        Check( chunk.more && chunk.totalBytes == 8 && chunk.data == std::vector<uint8_t>( expected.begin(), expected.begin() + 3 ), "canonical BC1 byte paging" );
        chunk = store->FrameImageBytes( first.next, 3, 64 );
        Check( !chunk.more && chunk.data == std::vector<uint8_t>( expected.begin() + 3, expected.end() ), "canonical selector order survives chunk split" );
        Check( store->FrameImageBytes( first.next, 8, 64 ).data.empty(), "byte EOF is an empty final chunk" );
        bool overBudget = false;
        try { store->FrameImageBytes( first.next, 0, 65536 ); }
        catch( const std::exception& ) { overBudget = true; }
        Check( overBudget, "image result budget includes chunk metadata" );
        Check( store->FrameImagesInRange( 99, 100 ).values.size() == 1 && store->FrameImagesInRange( 100, 200 ).values.empty() && store->FrameImagesInRange( 200, 201 ).values.size() == 1, "image windows use exact half-open frame intervals" );
        auto frames = store->Frames( 0, 201 );
        Check( frames.values[1].image == first.next && frames.values[4].image == second.next && !frames.values[2].image, "frame rows expose stable image IDs" );
        auto live = LiveEventStore::Create( path, options );
        Check( live->Update( 4 ), "live image payload revision" );
        auto before = live->Snapshot();
        Check( before->Info().frameImages == 0, "payload alone is not an image" );
        Check( live->Update( 1 ), "live metadata revision" );
        auto staged = live->Snapshot();
        auto stagedImages = staged->FrameImages();
        Check( stagedImages.values.size() == 2 && !stagedImages.values[1].frame, "live unresolved frame association" );
        Check( live->Update( 1 ), "live matching frame revision" );
        auto resolved = live->Snapshot();
        Check( resolved->FrameImage( second.next )->frame == 5 && !staged->FrameImage( second.next )->frame && before->FrameImages().values.empty(), "published image revisions remain immutable" );
        Check( live->Update( 1 ) && live->Snapshot()->Info().cleanEnd, "live image clean final revision" );
        const auto demandPath = root / "on-demand.tracy-stream";
        Fixture( demandPath, true );
        EventStore::Build( demandPath );
        auto demand = EventStore::Open( demandPath );
        auto demandImages = demand->FrameImages();
        Check( demandImages.values.size() == 2 && demandImages.values[0].index == 0 && demandImages.values[0].frameIndex == 3 && demandImages.values[0].frame == 5, "on-demand pre-capture image skipped and offset normalized" );
        for( int invalid = 1; invalid <= 4; ++invalid )
        {
            const auto input = root / ( "invalid-" + std::to_string( invalid ) + ".tracy-stream" );
            Fixture( input, false, invalid );
            bool rejected = false;
            try
            {
                EventStore::Build( input );
            }
            catch( const std::exception& )
            {
                rejected = true;
            }
            Check( rejected && !std::filesystem::exists( EventStore::Directory( input ) ), "invalid Frame Image never publishes" );
        }
        bool cancel = false;
        options.cancelled = [&] { return cancel; };
        store = EventStore::Open( path, options );
        cancel = true;
        bool rejected = false;
        try
        {
            store->FrameImageBytes( first.next, 0, 64 );
        }
        catch( const std::exception& )
        {
            rejected = true;
        }
        Check( rejected, "image reads honor cancellation" );
        cancel = false;
        Check( store->FrameImageBytes( first.next, 0, 64 ).data == expected, "image ID and data survive reopen/cancel" );
        std::cout << "Image tests passed. Evidence: " << root.string() << '\n';
        return 0;
    }
    catch( const std::exception& e )
    {
        std::cerr << "FAIL: " << e.what() << "\nEvidence: " << root.string() << '\n';
        return 1;
    }
}

#include "../../public/common/TracyProtocol.hpp"
#include "../../public/common/TracyQueue.hpp"
#include "../../public/common/tracy_lz4.hpp"
#include "../../server/TracyTraceFormat.hpp"
#include "../src/TracyEventStore.hpp"
#include "../src/TracySnapshotStream.hpp"
#include "../src/TracyStreamJournal.hpp"
#include <chrono>
#include <cstring>
#include <fstream>
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
void Fixture( const std::filesystem::path& path, uint32_t depth = 1, bool orphanImage = false )
{
    std::string error;
    auto writer = JournalWriter::CreateFileJournal( path, MakeFileHeader( ProtocolVersion ), false, {}, error );
    Check( bool( writer ), error.c_str() );
    uint64_t sequence = 0;
    const auto append = [&]( RecordType t, uint32_t flags, std::span<const uint8_t> b ) {
        Check( writer->Append( t, flags, b, ++sequence, error ), error.c_str() );
    };
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
    strcpy_s( welcome.programName, "Snapshot export fixture" );
    append( RecordType::ClientToServer, RecordFlagHandshake, { reinterpret_cast<const uint8_t*>( &welcome ), sizeof( welcome ) } );
    const auto flush = [&] {
        std::vector<uint8_t> out( LZ4Size + 4 );
        const int n = LZ4_compress_default( reinterpret_cast<char*>( b.data() ), reinterpret_cast<char*>( out.data() + 4 ), int( b.size() ), LZ4Size );
        Check( n > 0, "compress references" );
        memcpy( out.data(), &n, 4 );
        out.resize( n + 4 );
        append( RecordType::ClientToServer, RecordFlagCompressedFrame, out );
        b.clear();
    };
    b.clear();
    ServerQueryPacket query{ ServerQuerySourceLocation, 0x123, 0 };
    append( RecordType::ServerToClient, RecordFlagServerQuery, { reinterpret_cast<const uint8_t*>( &query ), sizeof( query ) } );
    Event( b, QueueType::SourceLocation, QueueSourceLocation{ 0, 0, 0, 42, 0, 0, 0 } );
    Event( b, QueueType::ThreadContext, QueueThreadContext{ 7 } );
    for( uint32_t n = 0; n < depth; ++n )
    {
        Event( b, QueueType::ZoneBegin, QueueZoneBegin{ { n ? 1 : 1100 }, 0x123 } );
        if( b.size() > 400000 ) flush();
    }
    for( uint32_t n = 0; n < depth; ++n )
    {
        Event( b, QueueType::ZoneEnd, QueueZoneEnd{ 1 } );
        if( b.size() > 400000 ) flush();
    }
    flush();
    const std::string fragment( 60000, 'x' );
    for( int zone = 0; zone < 2; ++zone )
    {
        Event( b, QueueType::ZoneBegin, QueueZoneBegin{ { 10 }, 0x123 } );
        for( int n = 0; n < 40; ++n )
        {
            b.push_back( uint8_t( QueueType::SingleStringData ) );
            Bytes( b, uint16_t( fragment.size() ) );
            b.insert( b.end(), fragment.begin(), fragment.end() );
            b.push_back( uint8_t( QueueType::ZoneText ) );
            flush();
        }
        Event( b, QueueType::ZoneEnd, QueueZoneEnd{ 10 } );
        flush();
    }
    if( orphanImage )
    {
        Event( b, QueueType::FrameImageData, QueueStringTransfer{ 99 } );
        Bytes( b, uint32_t( 8 ) );
        Bytes( b, uint64_t( 0xe4e4e4e40000ffffull ) );
        Event( b, QueueType::FrameImage, QueueFrameImage{ 99, 4, 4, 0 } );
        flush();
    }
    b.assign( 32, 0 );
    b[0] = 1;
    b[2] = 32;
    b[4] = 3;
    append( RecordType::SessionEnd, RecordFlagTerminal, b );
}
int main( int argc, char** argv )
{
    const auto root = std::filesystem::temp_directory_path() / ( "tracy-snapshot-export-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    try
    {
        std::filesystem::create_directory( root );
        const auto journal = root / "deep.tracy-stream";
        Fixture( journal, argc > 1 ? uint32_t( std::stoul( argv[1] ) ) : 10000 );
        StoreOptions read;
        read.cacheBytes = 4ull << 20;
        read.resultBytes = 65536;
        EventStore::Build( journal, read );
        auto store = EventStore::Open( journal, read );
        StoreExportOptions forced;
        forced.overwrite = true;
        const auto protectsSource = [&]( const std::filesystem::path& path ) {
            bool refused = false;
            try
            {
                store->ExportSnapshot( path, forced );
            }
            catch( const std::exception& e )
            {
                refused = std::string_view( e.what() ).find( "source" ) != std::string_view::npos;
            }
            Check( refused, "forced export must reject source journal and store artifacts before writing" );
        };
        protectsSource( journal );
        const auto alias = root / "journal-alias.tracy";
        std::filesystem::create_hard_link( journal, alias );
        protectsSource( alias );
        const auto storeDirectory = EventStore::Directory( journal );
        protectsSource( storeDirectory / "events.sqlite" );
        protectsSource( storeDirectory / "manifest.json" );
        std::filesystem::create_directory( storeDirectory / "nested" );
        protectsSource( storeDirectory / "nested" / "new.tracy" );
        const auto output = root / "complete.tracy";
        const auto result = store->ExportSnapshot( output );
        {
            SnapshotStreamReader reader( output );
            std::array<uint8_t, 24> header{};
            Check( reader.Read( header ) == header.size(), "exported native trace header is complete" );
            // Exercise the same format contract used by Worker, independently
            // of the exporter's own structural validator.
            DecodeTraceHeader( header );
        }
        Check( result.decodedBytes > 2400039 && result.decodedBytes < 4800000, "long identical Zone texts are streamed and deduplicated" );
        Check( result.fileBytes == std::filesystem::file_size( output ) && result.sha256.size() == 64, "published output identity" );
        bool rejected = false;
        try
        {
            store->ExportSnapshot( output );
        }
        catch( const std::exception& )
        {
            rejected = true;
        }
        Check( rejected, "existing output is protected" );
        const auto protectedFile = root / "protected.tracy";
        {
            std::ofstream file( protectedFile );
            file << "preserve existing data";
        }
        bool cancel = false;
        StoreExportOptions options;
        options.overwrite = true;
        options.cancelled = [&] {
            return cancel;
        };
        options.progress = [&]( const char* stage, uint64_t ) {
            if( std::string_view( stage ) == "CPU timelines" ) cancel = true;
        };
        rejected = false;
        try
        {
            store->ExportSnapshot( protectedFile, options );
        }
        catch( const std::exception& )
        {
            rejected = true;
        }
        Check( rejected, "export cancellation interrupts writing" );
        const auto preserved = [&] {
            std::ifstream file( protectedFile );
            return std::string( std::istreambuf_iterator<char>( file ), {} );
        };
        Check( preserved() == "preserve existing data", "cancel never replaces the destination" );
        cancel = false;
        options.progress = {};
        options.fileBytes = 6;
        rejected = false;
        try
        {
            store->ExportSnapshot( protectedFile, options );
        }
        catch( const std::exception& )
        {
            rejected = true;
        }
        Check( rejected && preserved() == "preserve existing data", "output budget never replaces the destination" );
        options.fileBytes = 64ull << 30;
        options.temporaryBytes = 1;
        rejected = false;
        try
        {
            store->ExportSnapshot( protectedFile, options );
        }
        catch( const std::exception& )
        {
            rejected = true;
        }
        Check( rejected && preserved() == "preserve existing data", "staging disk budget never replaces the destination" );
        options.temporaryBytes = 64ull << 30;
        options.progress = [&]( const char* stage, uint64_t ) {
            if( std::string_view( stage ) == "Verify structure" )
                for( const auto& entry : std::filesystem::directory_iterator( root ) )
                    if( entry.path().filename().string().find( "protected.tracy.partial-" ) == 0 )
                    {
                        std::fstream file( entry.path(), std::ios::in | std::ios::out | std::ios::binary );
                        file.seekp( 6 );
                        const uint32_t invalid = 0xffffffffu;
                        file.write( reinterpret_cast<const char*>( &invalid ), 4 );
                    }
        };
        rejected = false;
        try
        {
            store->ExportSnapshot( protectedFile, options );
        }
        catch( const std::exception& )
        {
            rejected = true;
        }
        Check( rejected && preserved() == "preserve existing data", "independent structure validation rejects damaged output" );
        options.progress = [&]( const char* stage, uint64_t ) {
            if( std::string_view( stage ) != "Verify structure" ) return;
            for( const auto& entry : std::filesystem::directory_iterator( root ) )
            {
                if( entry.path().filename().string().find( "protected.tracy.partial-" ) != 0 ) continue;
                auto changed = entry.path();
                changed += ".changed";
                {
                    SnapshotStreamReader reader( entry.path() );
                    SnapshotStreamWriter writer( changed );
                    std::array<uint8_t, 4096> buffer;
                    bool first = true;
                    for( ;; )
                    {
                        const auto n = reader.Read( buffer );
                        if( !n ) break;
                        if( first )
                        {
                            Check( n > 8, "tamper header available" );
                            buffer[8] ^= 1;
                            first = false;
                        }
                        writer.Write( { buffer.data(), n } );
                    }
                    writer.Finish();
                }
                std::filesystem::remove( entry.path() );
                std::filesystem::rename( changed, entry.path() );
                break;
            }
        };
        rejected = false;
        try
        {
            store->ExportSnapshot( protectedFile, options );
        }
        catch( const std::exception& )
        {
            rejected = true;
        }
        Check( rejected && preserved() == "preserve existing data", "structure pass must detect valid-layout content changes after semantic readback" );
        for( const auto& entry : std::filesystem::directory_iterator( root ) ) Check( entry.path().filename().string().find( ".partial-" ) == std::string::npos, "failed export temporary files are cleaned" );
        options.progress = {};
        const auto replaced = store->ExportSnapshot( protectedFile, options );
        Check( replaced.sha256 == result.sha256 && replaced.fileBytes == result.fileBytes, "explicit overwrite publishes the fully validated artifact" );
        auto live = LiveEventStore::Create( journal, read );
        while( live->Update( 32 ) )
        {
        }
        auto snapshot = live->Snapshot( read );
        const auto liveZone = snapshot->Zones( INT64_MIN, INT64_MAX, {}, 0, 1 ).values.front();
        const auto liveResult = snapshot->ExportSnapshot( root / "live.tracy" );
        Check( liveResult.sha256 == result.sha256 && liveResult.revision == snapshot->Info().revision, "pinned live revision exports the same payload as offline indexing" );
        const auto observedZone = snapshot->Zone( liveZone.id );
        Check( observedZone && observedZone->start == liveZone.start, "export leaves the live read transaction usable" );
        rejected = false;
        try
        {
            snapshot->ExportSnapshot( journal, forced );
        }
        catch( const std::exception& e )
        {
            rejected = std::string_view( e.what() ).find( "source" ) != std::string_view::npos;
        }
        Check( rejected, "live snapshots protect the source journal" );
        const auto orphan = root / "orphan.tracy-stream";
        Fixture( orphan, 1, true );
        EventStore::Build( orphan, read );
        auto orphanStore = EventStore::Open( orphan, read );
        const auto orphanExport = orphanStore->ExportSnapshot( root / "orphan.tracy" );
        // Keep corrupt fixtures for the independent native-reader acceptance test.
        for( bool truncate : { false, true } )
        {
            SnapshotStreamReader reader( root / "orphan.tracy" );
            SnapshotStreamWriter writer( root / ( truncate ? "orphan-truncated.tracy" : "orphan-invalid.tracy" ) );
            std::array<uint8_t, 4096> buffer;
            uint64_t offset = 0;
            for( ;; )
            {
                const auto n = reader.Read( buffer );
                if( !n ) break;
                for( size_t i = 0; i < n; ++i ) if( offset + i >= orphanExport.decodedBytes - 4 ) buffer[i] = 0;
                const auto count = truncate ? size_t( std::min<uint64_t>( n, orphanExport.decodedBytes - 2 - offset ) ) : n;
                writer.Write( { buffer.data(), count } );
                offset += n;
                if( offset >= orphanExport.decodedBytes - 2 && truncate ) break;
            }
            writer.Finish();
        }
        std::cout << "Snapshot export tests passed. Evidence: " << root.string() << '\n';
        return 0;
    }
    catch( const std::exception& e )
    {
        std::cerr << "FAIL: " << e.what() << "\nEvidence: " << root.string() << '\n';
        return 1;
    }
}

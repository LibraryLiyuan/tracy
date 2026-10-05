#include "../../public/common/TracyProtocol.hpp"
#include "../../public/common/TracyQueue.hpp"
#include "../../public/common/tracy_lz4.hpp"
#include "../src/TracyEventStore.hpp"
#include "../src/TracyStreamJournal.hpp"
#include <chrono>
#include <cstring>
#include <fstream>
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
void String( std::vector<uint8_t>& b, QueueType t, uint64_t key, const char* value )
{
    Event( b, t, QueueStringTransfer{ key } );
    Bytes( b, uint16_t( strlen( value ) ) );
    b.insert( b.end(), value, value + strlen( value ) );
}
void Fixture( const std::filesystem::path& path, int fault, size_t definitions = 0 )
{
    std::string error;
    auto writer = JournalWriter::CreateFileJournal( path, MakeFileHeader( ProtocolVersion ), false, {}, error );
    Check( bool( writer ), error.c_str() );
    uint64_t tick = 0;
    auto append = [&]( RecordType type, uint32_t flags, std::span<const uint8_t> bytes ) { Check( writer->Append( type, flags, bytes, ++tick, error ), error.c_str() ); };
    std::vector<uint8_t> b( 24 );
    b[0] = 2;
    b[2] = 24;
    const uint32_t version = ProtocolVersion, flags = SessionBeginFlagDeferredSymbolExpansion;
    memcpy( b.data() + 4, &version, 4 );
    memcpy( b.data() + 16, &flags, 4 );
    append( RecordType::SessionBegin, RecordFlagHandshake, b );
    std::array<uint8_t, 8> hello;
    memcpy( hello.data(), HandshakeShibboleth, 8 );
    if( fault == 4 ) hello[0] ^= 1;
    append( RecordType::ServerToClient, RecordFlagHandshake, hello );
    append( RecordType::ServerToClient, RecordFlagHandshake, { reinterpret_cast<const uint8_t*>( &version ), 4 } );
    const uint8_t status = HandshakeWelcome;
    append( RecordType::ClientToServer, RecordFlagHandshake, { &status, 1 } );
    WelcomeMessage welcome{};
    welcome.timerMul = 1;
    welcome.initBegin = 1000;
    welcome.initEnd = 1010;
    append( RecordType::ClientToServer, RecordFlagHandshake, { reinterpret_cast<const uint8_t*>( &welcome ), sizeof( welcome ) } );
    auto frame = [&] {
        std::vector<uint8_t> out( LZ4Size + 4 );
        int size = LZ4_compress_default( reinterpret_cast<const char*>( b.data() ), reinterpret_cast<char*>( out.data() + 4 ), int( b.size() ), LZ4Size );
        Check( size > 0, "fixture compression" );
        memcpy( out.data(), &size, 4 );
        out.resize( size + 4 );
        append( RecordType::ClientToServer, RecordFlagCompressedFrame, out );
        b.clear();
    };
    b.clear();
    Event( b, QueueType::ThreadContext, QueueThreadContext{ 7 } );
    Event( b, QueueType::ZoneBegin, QueueZoneBegin{ { 1100 }, 99 } );
    Event( b, QueueType::ZoneEnd, QueueZoneEnd{ 10 } );
    Event( b, QueueType::FrameMarkMsg, QueueFrameMark{ 1100, fault == 9 ? 0ull : 42ull } );
    frame();
    std::vector<ServerQueryPacket> queries{ { ServerQuerySourceLocation, 99, 0 }, { ServerQueryThreadString, 7, 0 }, { ServerQueryFrameName, 42, 0 }, { ServerQueryString, 1, 0 }, { ServerQueryString, 2, 0 }, { ServerQueryString, 3, 0 } };
    if( fault == 1 ) queries.push_back( { ServerQueryString, 444, 0 } );
    if( fault == 2 ) queries.push_back( queries.back() );
    if( fault == 3 ) queries[0].extra = 1;
    if( fault == 6 ) queries[1].ptr = 8;
    if( fault == 8 || fault == 9 ) queries.erase( queries.begin() + 2 );
    auto send = [&] { for( const auto& q : queries ) append( RecordType::ServerToClient, RecordFlagServerQuery, { reinterpret_cast<const uint8_t*>( &q ), sizeof( q ) } ); };
    if( fault != 7 ) send();
    Event( b, QueueType::SourceLocation, QueueSourceLocation{ 1, 2, 3, 42, 0, 0, 0 } );
    String( b, QueueType::ThreadName, fault == 6 ? 8 : 7, "Thread" );
    if( fault != 8 && fault != 9 ) String( b, QueueType::FrameName, 42, "Frame" );
    String( b, QueueType::StringData, 1, "Zone" );
    String( b, QueueType::StringData, 2, "Function" );
    String( b, QueueType::StringData, 3, "file.cpp" );
    frame();
    if( fault == 7 ) send(); // The response observer may publish before the send observer.
    for( size_t n = 0; n < definitions; ++n )
    {
        const uint64_t key = 1000 + n;
        Event( b, QueueType::MessageLiteral, QueueMessageLiteral{ 1, key } );
        const ServerQueryPacket q{ ServerQueryString, key, 0 };
        append( RecordType::ServerToClient, RecordFlagServerQuery, { reinterpret_cast<const uint8_t*>( &q ), sizeof( q ) } );
        String( b, QueueType::StringData, key, "Independent definition" );
        if( b.size() > 100000 ) frame();
    }
    if( !b.empty() ) frame();
    const ServerQueryPacket end{ ServerQueryTerminate, fault == 5 ? 1ull : 0ull, 0 };
    append( RecordType::ServerToClient, RecordFlagServerQuery, { reinterpret_cast<const uint8_t*>( &end ), sizeof( end ) } );
    b.push_back( uint8_t( QueueType::Terminate ) );
    frame();
    b.assign( 32, 0 );
    b[0] = 1;
    b[2] = 32;
    b[4] = 3;
    append( RecordType::SessionEnd, RecordFlagTerminal, b );
}
int main( int argc, char** argv )
{
    const auto root = std::filesystem::temp_directory_path() / ( "tracy-transcript-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    try
    {
        std::filesystem::create_directory( root );
        if( argc == 2 && std::string_view( argv[1] ) == "--scale" )
        {
            const auto path = root / "scale.tracy-stream";
            Fixture( path, 0, 100000 );
            StoreOptions options;
            options.cacheBytes = 4ull << 20;
            const auto info = EventStore::Build( path, options );
            Check( info.transcriptVerified && info.messages == 100000, "high cardinality transcript is independently verified with bounded cache" );
            Check( EventStore::Open( path, options )->Info().transcriptVerified, "verification proof survives reopening" );
            std::cout << "Transcript scale passed. Evidence: " << root.string() << '\n';
            return 0;
        }
        for( int fault = 0; fault <= 9; ++fault )
        {
            const auto path = root / ( std::to_string( fault ) + ".tracy-stream" );
            Fixture( path, fault );
            bool rejected = false;
            try
            {
                EventStore::Build( path );
            }
            catch( const std::exception& e )
            {
                rejected = true;
                std::cout << fault << ": " << e.what() << '\n';
            }
            Check( rejected == ( fault >= 1 && fault != 7 && fault != 9 ), "strict transcript rejects missing, extra, duplicate, altered queries, handshake and controls" );
            if( fault == 7 )
            {
                auto live = LiveEventStore::Create( path );
                for( int n = 0; n < 32 && !live->Snapshot()->Info().cleanEnd; ++n ) live->Update( 2 );
                Check( live->Snapshot()->Info().transcriptVerified, "live decoder accepts a response observed before its recorded request across batches" );
                const auto scan = ScanJournal( path );
                const auto firstQuery = std::find_if( scan.records.begin(), scan.records.end(), []( const RecordInfo& r ) { return r.type == RecordType::ServerToClient && r.flags == RecordFlagServerQuery; } );
                Check( firstQuery != scan.records.end(), "query boundary in observer-order fixture" );
                std::vector<uint8_t> bytes( size_t( std::filesystem::file_size( path ) ) );
                std::ifstream source( path, std::ios::binary );
                source.read( reinterpret_cast<char*>( bytes.data() ), bytes.size() );
                Check( bool( source ), "read observer fixture" );
                const auto growing = root / "growing.tracy-stream";
                {
                    std::ofstream output( growing, std::ios::binary );
                    output.write( reinterpret_cast<const char*>( bytes.data() ), firstQuery->offset );
                }
                auto waiting = LiveEventStore::Create( growing );
                for( int n = 0; n < 16; ++n ) waiting->Update( 2 );
                auto old = waiting->Snapshot();
                const auto oldRevision = old->Info().revision;
                Check( old->Info().cpuZones == 1 && !old->Info().cleanEnd && !old->Info().transcriptVerified, "published prefix remains readable while response waits" );
                Check( !waiting->Update( 2 ) && waiting->Snapshot()->Info().revision == oldRevision, "temporary EOF with deferred responses keeps the last revision" );
                {
                    std::ofstream output( growing, std::ios::binary | std::ios::app );
                    output.write( reinterpret_cast<const char*>( bytes.data() + firstQuery->offset ), bytes.size() - firstQuery->offset );
                }
                for( int n = 0; n < 32 && !waiting->Snapshot()->Info().cleanEnd; ++n ) waiting->Update( 2 );
                Check( waiting->Snapshot()->Info().transcriptVerified && old->Info().revision == oldRevision, "late request completes verification without changing pinned revision" );
            }
        }
        std::cout << "Transcript acceptance passed. Evidence: " << root.string() << '\n';
        return 0;
    }
    catch( const std::exception& e )
    {
        std::cerr << e.what() << "\nEvidence: " << root.string() << '\n';
        return 1;
    }
}

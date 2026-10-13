#include "TracyEventStoreTraceSource.hpp"
#include "TracyEventStore.hpp"
#include "TracyStreamJournal.hpp"
#include "../../public/common/TracyProtocol.hpp"
#include "../../public/common/TracyQueue.hpp"
#include "../../public/common/tracy_lz4.hpp"

#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace tracy;

static void Check( bool value, const char* message )
{
    if( !value ) throw std::runtime_error( message );
}

template<class T> static void Bytes( std::vector<uint8_t>& data, const T& value )
{
    const auto p = reinterpret_cast<const uint8_t*>( &value );
    data.insert( data.end(), p, p + sizeof( value ) );
}

template<class T> static void Event( std::vector<uint8_t>& data, QueueType type, const T& value )
{
    data.push_back( uint8_t( type ) );
    Bytes( data, value );
}

static void Fixture( const std::filesystem::path& path )
{
    std::string error;
    auto writer = stream::JournalWriter::CreateFileJournal( path, stream::MakeFileHeader( ProtocolVersion ), false, {}, error );
    Check( bool( writer ), "create fixture journal" );
    uint64_t sequence = 0;
    const auto append = [&]( stream::RecordType type, uint32_t flags, std::span<const uint8_t> data ) {
        Check( writer->Append( type, flags, data, ++sequence, error ), "append fixture record" );
    };
    const uint32_t protocol = ProtocolVersion;
    std::vector<uint8_t> data( 24 );
    data[0] = 2; data[2] = 24;
    std::memcpy( data.data() + 4, &protocol, 4 );
    append( stream::RecordType::SessionBegin, 0, data );
    const uint8_t status = HandshakeWelcome;
    append( stream::RecordType::ClientToServer, stream::RecordFlagHandshake, { &status, 1 } );
    WelcomeMessage welcome{};
    welcome.timerMul = 1; welcome.initBegin = 1000; welcome.initEnd = 1010;
    std::strcpy( welcome.programName, "Window scan fixture" );
    append( stream::RecordType::ClientToServer, stream::RecordFlagHandshake,
        { reinterpret_cast<const uint8_t*>( &welcome ), sizeof( welcome ) } );
    for( const auto [type, key] : { std::pair{ ServerQuerySourceLocation, uint64_t( 0x123 ) },
        std::pair{ ServerQueryString, uint64_t( 0x100 ) }, std::pair{ ServerQueryString, uint64_t( 0x101 ) },
        std::pair{ ServerQueryString, uint64_t( 0x102 ) } } )
    {
        ServerQueryPacket query{ type, key, 0 };
        append( stream::RecordType::ServerToClient, stream::RecordFlagServerQuery,
            { reinterpret_cast<const uint8_t*>( &query ), sizeof( query ) } );
    }
    const auto flush = [&] {
        if( data.empty() ) return;
        std::vector<uint8_t> compressed( LZ4Size + 4 );
        const int size = LZ4_compress_default( reinterpret_cast<const char*>( data.data() ),
            reinterpret_cast<char*>( compressed.data() + 4 ), int( data.size() ), LZ4Size );
        Check( size > 0, "compress fixture" );
        std::memcpy( compressed.data(), &size, 4 );
        compressed.resize( size + 4 );
        append( stream::RecordType::ClientToServer, stream::RecordFlagCompressedFrame, compressed );
        data.clear();
    };
    data.clear();
    Event( data, QueueType::SourceLocation, QueueSourceLocation{ 0x100, 0x101, 0x102, 1, 0, 0, 255 } );
    for( const auto [key, value] : { std::pair{ uint64_t( 0x100 ), "Window zone" },
        std::pair{ uint64_t( 0x101 ), "FixtureFunction" }, std::pair{ uint64_t( 0x102 ), "fixture.cpp" } } )
    {
        Event( data, QueueType::StringData, QueueStringTransfer{ key } );
        Bytes( data, uint16_t( std::strlen( value ) ) );
        data.insert( data.end(), value, value + std::strlen( value ) );
    }
    Event( data, QueueType::FrameMarkMsg, QueueFrameMark{ 1100, 0 } );
    for( uint32_t thread : { 1u, 2u } )
    {
        Event( data, QueueType::ThreadContext, QueueThreadContext{ thread } );
        int64_t previous = 0;
        const auto begin = [&]( int64_t time ) {
            Event( data, QueueType::ZoneBegin, QueueZoneBegin{ { time - previous }, 0x123 } ); previous = time;
        };
        const auto end = [&]( int64_t time ) {
            Event( data, QueueType::ZoneEnd, QueueZoneEnd{ time - previous } ); previous = time;
        };
        for( int64_t i = 0; i < 5000; ++i )
        {
            const auto time = 2000 + i * 100 + int64_t( thread ) * 7;
            begin( time ); begin( time + 10 ); end( time + 20 );
            begin( time + 30 ); end( time + 40 ); end( time + 50 );
            if( data.size() > 32000 ) flush();
        }
        // An open root exercises conservative bucket bounds without inventing
        // a completed event at the capture watermark.
        begin( 510000 + int64_t( thread ) );
    }
    Event( data, QueueType::FrameMarkMsg, QueueFrameMark{ 520000, 0 } );
    flush();
    data.assign( 32, 0 ); data[0] = 1; data[2] = 32; data[4] = 3;
    append( stream::RecordType::SessionEnd, stream::RecordFlagTerminal, data );
}

static bool Same( const analysis::CpuZoneDto& a, const analysis::CpuZoneDto& b )
{
    return a.ref == b.ref && a.parentRef == b.parentRef && a.threadRef == b.threadRef && a.name == b.name &&
        a.startNs == b.startNs && a.endNs == b.endNs && a.selfTimeNs == b.selfTimeNs &&
        a.childCount == b.childCount && a.complete == b.complete;
}

int main()
{
    const auto root = std::filesystem::temp_directory_path() / ( "tracy-window-scan-" +
        std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    std::filesystem::create_directories( root );
    try
    {
        const auto path = root / "input.tracy-stream";
        Fixture( path );
        stream::EventStore::Build( path );
        const auto source = query::EventStoreTraceSource::Open( path );
        const auto readView = source->AcquireReadView();
        Check( readView.sourceKind == analysis::TraceSourceKind::Segment && readView.revision > 0,
            "event-store read view preserves original segment identity" );
        const auto store = stream::EventStore::Open( path );
        Check( readView.complete == store->Info().cleanEnd && readView.watermarkNs == store->Info().lastNs,
            "event-store read view reflects journal completeness and watermark" );
        analysis::ScanRange full;
        full.limit = 100000;
        const auto all = source->ScanCpuZones( full );
        Check( all.size() == 30002, "all fixture zones retained" );
        for( const auto [begin, end] : { std::pair<int64_t, int64_t>{ -100, 0 }, { 1000, 1100 },
            { 137000, 137100 }, { 273000, 274000 }, { 409000, 410000 }, { 509000, 520000 }, { 0, 520000 } } )
        {
            std::vector<analysis::CpuZoneDto> expected, actual;
            for( const auto& zone : all )
                if( zone.startNs < end && zone.endNs.value_or( zone.startNs ) > begin ) expected.push_back( zone );
            analysis::ScanRange range{ begin, end, 0, 7 };
            for( ;; )
            {
                const auto page = source->ScanCpuZones( range );
                actual.insert( actual.end(), page.begin(), page.end() );
                range.offset += page.size();
                if( page.size() < range.limit ) break;
            }
            Check( expected.size() == actual.size(), "window pagination preserves count" );
            for( size_t i = 0; i < actual.size(); ++i ) Check( Same( expected[i], actual[i] ), "window preserves order, ancestry and timing" );
            range.offset = expected.size() / 2; range.limit = 3;
            const auto randomPage = source->ScanCpuZones( range );
            for( size_t i = 0; i < randomPage.size(); ++i ) Check( Same( expected[range.offset + i], randomPage[i] ), "random offset after exhausted cursor" );
        }
    }
    catch( const std::exception& error )
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::filesystem::remove_all( root );
    return 0;
}

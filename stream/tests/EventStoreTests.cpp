#include "../../public/common/TracyProtocol.hpp"
#include "../../public/common/TracyQueue.hpp"
#include "../../public/common/tracy_lz4.hpp"
#include "../src/TracyEventStore.hpp"
#include "../src/TracyStreamJournal.hpp"
#include <algorithm>
#include <array>
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
    auto p = reinterpret_cast<const uint8_t*>( &v );
    b.insert( b.end(), p, p + sizeof( v ) );
}
template<class T>
void Event( std::vector<uint8_t>& b, QueueType t, const T& v )
{
    b.push_back( uint8_t( t ) );
    Bytes( b, v );
}
void String( std::vector<uint8_t>& b, QueueType t, uint64_t id, const char* v )
{
    Event( b, t, QueueStringTransfer{ id } );
    uint16_t n = uint16_t( strlen( v ) );
    Bytes( b, n );
    b.insert( b.end(), v, v + n );
}
void InlineString( std::vector<uint8_t>& b, QueueType t, const char* value )
{
    b.push_back( uint8_t( t ) );
    const uint16_t length = uint16_t( strlen( value ) );
    Bytes( b, length );
    b.insert( b.end(), value, value + length );
}
void Fixture( const std::filesystem::path& p, size_t extraFrames = 0, bool empty = false )
{
    std::string error;
    auto writer = JournalWriter::CreateFileJournal( p, MakeFileHeader( ProtocolVersion ), false, {}, error );
    Check( bool( writer ), error.c_str() );
    uint64_t now = 0;
    auto append = [&]( RecordType type, uint32_t flags, std::span<const uint8_t> data ) { Check( writer->Append( type, flags, data, ++now, error ), error.c_str() ); };
    const uint32_t version = ProtocolVersion;
    std::vector<uint8_t> data( 24 );
    data[0] = 2;
    data[2] = 24;
    memcpy( data.data() + 4, &version, 4 );
    append( RecordType::SessionBegin, 0, data );
    append( RecordType::ServerToClient, RecordFlagHandshake, { reinterpret_cast<const uint8_t*>( HandshakeShibboleth ), 8 } );
    append( RecordType::ServerToClient, RecordFlagHandshake, { reinterpret_cast<const uint8_t*>( &version ), 4 } );
    const uint8_t welcomeStatus = HandshakeWelcome;
    append( RecordType::ClientToServer, RecordFlagHandshake, { &welcomeStatus, 1 } );
    WelcomeMessage welcome{};
    welcome.timerMul = 1;
    welcome.initBegin = 1000;
    welcome.initEnd = 1010;
    strcpy_s( welcome.programName, "StoreFixture" );
    append( RecordType::ClientToServer, RecordFlagHandshake, { reinterpret_cast<const uint8_t*>( &welcome ), sizeof( welcome ) } );
    if( empty )
    {
        data.assign( 32, 0 );
        data[0] = 1;
        data[2] = 32;
        data[4] = 3;
        append( RecordType::SessionEnd, RecordFlagTerminal, data );
        return;
    }
    for( auto [type, key] : std::array<std::pair<ServerQuery, uint64_t>, 5>{ { { ServerQuerySourceLocation, 0x123 }, { ServerQueryString, 0x100 }, { ServerQueryString, 0x101 }, { ServerQueryString, 0x102 }, { ServerQueryCallstackFrame, 0x12345 } } } )
    {
        ServerQueryPacket query{ type, key, 0 };
        append( RecordType::ServerToClient, RecordFlagServerQuery, { reinterpret_cast<const uint8_t*>( &query ), sizeof( query ) } );
    }
    auto frame = [&]( std::vector<uint8_t>& events ) {
        std::vector<uint8_t> compressed( LZ4Size + 4 );
        int size = LZ4_compress_default( reinterpret_cast<const char*>( events.data() ), reinterpret_cast<char*>( compressed.data() + 4 ), int( events.size() ), LZ4Size );
        Check( size > 0, "compress fixture" );
        memcpy( compressed.data(), &size, 4 );
        compressed.resize( size + 4 );
        append( RecordType::ClientToServer, RecordFlagCompressedFrame, compressed );
    };
    data.clear();
    Event( data, QueueType::ThreadContext, QueueThreadContext{ 1 } );
    Event( data, QueueType::CallstackPayload, QueueStringTransfer{ 1 } );
    Bytes( data, uint16_t( 16 ) );
    Bytes( data, uint64_t( 0x12345 ) );
    Bytes( data, uint64_t( 0x67890 ) );
    data.push_back( uint8_t( QueueType::Callstack ) );
    Event( data, QueueType::ZoneBeginCallstack, QueueZoneBegin{ { 1100 }, 0x123 } );
    Event( data, QueueType::ZoneBegin, QueueZoneBegin{ { 10 }, 0x123 } );
    data.push_back( uint8_t( QueueType::SingleStringData ) );
    uint16_t n = 10;
    Bytes( data, n );
    const char text[] = "child note";
    data.insert( data.end(), text, text + 10 );
    data.push_back( uint8_t( QueueType::ZoneText ) );
    Event( data, QueueType::FrameMarkMsg, QueueFrameMark{ 1100, 0 } );
    frame( data );
    data.clear();
    Event( data, QueueType::ZoneEnd, QueueZoneEnd{ 10 } );
    Event( data, QueueType::ZoneEnd, QueueZoneEnd{ 80 } );
    Event( data, QueueType::ThreadContext, QueueThreadContext{ 2 } );
    Event( data, QueueType::ZoneBegin, QueueZoneBegin{ { 1200 }, 0x123 } );
    Event( data, QueueType::ZoneEnd, QueueZoneEnd{ 0 } );
    Event( data, QueueType::ThreadContext, QueueThreadContext{ 3 } );
    Event( data, QueueType::ZoneBegin, QueueZoneBegin{ { 1210 }, 0x123 } );
    Event( data, QueueType::FrameMarkMsg, QueueFrameMark{ 1200, 0 } );
    Event( data, QueueType::SourceLocation, QueueSourceLocation{ 0x100, 0x101, 0x102, 42, 0, 0, 255 } );
    String( data, QueueType::StringData, 0x100, "Delayed zone" );
    String( data, QueueType::StringData, 0x101, "FixtureFn" );
    String( data, QueueType::StringData, 0x102, "fixture.cpp" );
    InlineString( data, QueueType::SingleStringData, "fixture.exe" );
    Event( data, QueueType::CallstackFrameSize, QueueCallstackFrameSize{ 0x12345, 2 } );
    InlineString( data, QueueType::SingleStringData, "InlinedFn" );
    InlineString( data, QueueType::SecondStringData, "inline.cpp" );
    Event( data, QueueType::CallstackFrame, QueueCallstackFrame{ 10, 0x12000, 32 } );
    InlineString( data, QueueType::SingleStringData, "OuterFn" );
    InlineString( data, QueueType::SecondStringData, "outer.cpp" );
    Event( data, QueueType::CallstackFrame, QueueCallstackFrame{ 20, 0x12100, 64 } );
    frame( data );
    data.clear();
    for( size_t i = 0; i < extraFrames; ++i ) Event( data, QueueType::FrameMarkMsg, QueueFrameMark{ int64_t( 1300 + i ), 0 } );
    if( !data.empty() ) frame( data );
    data.assign( 32, 0 );
    data[0] = 1;
    data[2] = 32;
    data[4] = 3;
    append( RecordType::SessionEnd, RecordFlagTerminal, data );
}
// A zone text, name or color carries the validation of the zone it is for.
// One written for an outer zone while an inner zone is open cannot be attached
// to the zone it names, so it is dropped and counted; a zone end for an outer
// zone while an inner zone is open breaks the stack and is rejected.
void ZoneEventFixture( const std::filesystem::path& p, bool misnestedEnd )
{
    std::string error;
    auto writer = JournalWriter::CreateFileJournal( p, MakeFileHeader( ProtocolVersion ), false, {}, error );
    Check( bool( writer ), error.c_str() );
    uint64_t now = 0;
    auto append = [&]( RecordType type, uint32_t flags, std::span<const uint8_t> data ) { Check( writer->Append( type, flags, data, ++now, error ), error.c_str() ); };
    const uint32_t version = ProtocolVersion;
    std::vector<uint8_t> data( 24 );
    data[0] = 2;
    data[2] = 24;
    memcpy( data.data() + 4, &version, 4 );
    append( RecordType::SessionBegin, 0, data );
    const uint8_t welcomeStatus = HandshakeWelcome;
    append( RecordType::ClientToServer, RecordFlagHandshake, { &welcomeStatus, 1 } );
    WelcomeMessage welcome{};
    welcome.timerMul = 1;
    welcome.initBegin = 1000;
    welcome.initEnd = 1010;
    strcpy_s( welcome.programName, "ZoneEventFixture" );
    append( RecordType::ClientToServer, RecordFlagHandshake, { reinterpret_cast<const uint8_t*>( &welcome ), sizeof( welcome ) } );
    ServerQueryPacket query{ ServerQuerySourceLocation, 0x123, 0 };
    append( RecordType::ServerToClient, RecordFlagServerQuery, { reinterpret_cast<const uint8_t*>( &query ), sizeof( query ) } );
    data.clear();
    Event( data, QueueType::SourceLocation, QueueSourceLocation{ 0, 0, 0, 1, 0, 0, 0 } );
    Event( data, QueueType::ThreadContext, QueueThreadContext{ 5 } );
    const auto text = [&]( uint32_t validation, const char* value ) {
        Event( data, QueueType::ZoneValidation, QueueZoneValidation{ validation } );
        InlineString( data, QueueType::SingleStringData, value );
        data.push_back( uint8_t( QueueType::ZoneText ) );
    };
    Event( data, QueueType::ZoneValidation, QueueZoneValidation{ 7 } );
    Event( data, QueueType::ZoneBegin, QueueZoneBegin{ { 1100 }, 0x123 } );
    Event( data, QueueType::ZoneValidation, QueueZoneValidation{ 8 } );
    Event( data, QueueType::ZoneBegin, QueueZoneBegin{ { 10 }, 0x123 } );
    if( misnestedEnd )
    {
        Event( data, QueueType::ZoneValidation, QueueZoneValidation{ 7 } );
        Event( data, QueueType::ZoneEnd, QueueZoneEnd{ 10 } );
    }
    text( 7, "outer late" );
    Event( data, QueueType::ZoneValidation, QueueZoneValidation{ 7 } );
    Event( data, QueueType::ZoneColor, QueueZoneColor{ 1, 2, 3 } );
    text( 8, "inner note" );
    Event( data, QueueType::ZoneValidation, QueueZoneValidation{ 8 } );
    Event( data, QueueType::ZoneEnd, QueueZoneEnd{ 10 } );
    text( 7, "outer note" );
    Event( data, QueueType::ZoneValidation, QueueZoneValidation{ 7 } );
    Event( data, QueueType::ZoneEnd, QueueZoneEnd{ 10 } );
    std::vector<uint8_t> compressed( LZ4Size + 4 );
    int size = LZ4_compress_default( reinterpret_cast<const char*>( data.data() ), reinterpret_cast<char*>( compressed.data() + 4 ), int( data.size() ), LZ4Size );
    Check( size > 0, "compress zone event fixture" );
    memcpy( compressed.data(), &size, 4 );
    compressed.resize( size + 4 );
    append( RecordType::ClientToServer, RecordFlagCompressedFrame, compressed );
    data.assign( 32, 0 );
    data[0] = 1;
    data[2] = 32;
    data[4] = 3;
    append( RecordType::SessionEnd, RecordFlagTerminal, data );
}
int main()
{
    auto root = std::filesystem::temp_directory_path() / std::filesystem::path( "tracy-store-test-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
    try
    {
        std::filesystem::create_directory( root );
        auto p = root / "test.tracy-stream";
        Fixture( p );
        auto info = EventStore::Build( p );
        Check( info.cpuZones == 4, "four CPU zones indexed directly from journal" );
        const auto following = root / "following.tracy-stream";
        std::filesystem::copy_file( p, following );
        const auto followed = EventStore::BuildFollowing( following, [] { return true; } );
        Check( followed.sourceSha256 == info.sourceSha256 && followed.events == info.events && followed.cpuZones == info.cpuZones && followed.frames == info.frames && followed.transcriptVerified == info.transcriptVerified, "incremental follower retains the offline store's identity, counts and transcript checks" );
        const auto changedFollowing = root / "changed-following.tracy-stream";
        std::filesystem::copy_file( p, changedFollowing );
        StoreOptions changedOptions;
        bool changedOnce = false;
        changedOptions.progress = [&]( uint64_t, uint64_t ) {
            if( changedOnce ) return;
            changedOnce = true;
            std::fstream changedInput( changedFollowing, std::ios::binary | std::ios::in | std::ios::out );
            changedInput.seekg( FileHeaderSize + RecordHeaderSize );
            char byte = char( changedInput.get() ) ^ 1;
            changedInput.seekp( FileHeaderSize + RecordHeaderSize );
            changedInput.put( byte );
            Check( bool( changedInput ), "rewrite followed source" );
        };
        bool followedRejected = false;
        try { EventStore::BuildFollowing( changedFollowing, [] { return true; }, changedOptions ); }
        catch( const std::exception& ) { followedRejected = true; }
        Check( changedOnce && followedRejected && !std::filesystem::exists( EventStore::Directory( changedFollowing ) ), "a rewritten followed prefix cannot publish its event store" );
        auto store = EventStore::Open( p );
        Check( bool( store ), "reopen store" );
        auto page = store->Zones( 115, 125, 1 );
        Check( page.values.size() == 2, "long parent starting before window is included" );
        Check( page.values[1].parent == page.values[0].id, "parent ID preserved across frames" );
        Check( page.values[1].text == "child note" && page.values[1].name == "Delayed zone", "extras and delayed definitions resolved" );
        auto addresses = store->Stack( page.values[0].stack );
        Check( addresses == std::vector<uint64_t>{ 0x12345, 0x67890 }, "native stack address order retained" );
        auto symbols = store->StackFrames( page.values[0].stack, 0, 1 );
        Check( symbols.more && symbols.values[0].name == "InlinedFn" && symbols.values[0].inlineFrame && symbols.values[0].image == "fixture.exe", "inline symbols are resolved on demand" );
        symbols = store->StackFrames( page.values[0].stack, symbols.next, 1 );
        Check( symbols.more && symbols.values[0].name == "OuterFn" && !symbols.values[0].inlineFrame && symbols.values[0].symbolLength == 64, "symbol pagination preserves inline order and metadata" );
        symbols = store->StackFrames( page.values[0].stack, symbols.next, 1 );
        Check( !symbols.more && symbols.values[0].address == 0x67890 && symbols.values[0].name.empty(), "unresolved address is explicit rather than dropped" );
        Check( store->Zones( 120, 121, 1 ).values.size() == 1, "half-open window excludes ending child" );
        Check( store->Zones( 200, 201, 2 ).values.size() == 1, "zero-duration event remains accessible" );
        Check( store->Zones( 215, 216, 3 ).values.size() == 1, "open zone is explicit and intersects future window" );
        auto first = store->Zones( 115, 125, 1, 0, 1 );
        Check( first.more, "dense window is paginated" );
        auto next = store->Zones( 115, 125, 1, first.next, 1 );
        Check( next.values.size() == 1 && next.values[0].id != first.values[0].id, "cursor has no duplicates" );
        Check( store->Frames( 100, 200 ).values.size() == 1, "frame overlap is half-open" );
        auto frameSet = store->FrameSet( 0 );
        Check( frameSet && frameSet->name == "Frames" && frameSet->frames == 4 && frameSet->completeFrames == 3 && frameSet->continuous, "frame-set identity and completeness" );
        store.reset();
        store = EventStore::Open( p );
        Check( store->Zone( page.values[1].id )->text == "child note", "stable ID on reopen" );
        auto children = store->Children( page.values[0].id );
        Check( children.values.size() == 1 && children.values[0].id == page.values[1].id, "indexed parent-to-child navigation" );
        Check( page.values[0].childTime == *children.values[0].end - children.values[0].start && children.values[0].childTime == 0, "child time sums the closed direct children" );
        auto overview = store->Overview( 0, 300 );
        Check( !overview.empty() && overview[0].frames == 3, "frame overview is persisted independently of frame DTOs" );
        store.reset();
        {
            // Unchanged files open by identity; content checks run on request.
            StoreOptions fast;
            fast.verifyContent = false;
            Check( bool( EventStore::Open( p, fast ) ), "unchanged store opens by file identity" );
            EventStore::VerifyContent( p );
            // A rewritten journal of the same size loses its identity, so the
            // same open hashes it and rejects the changed bytes.
            std::vector<char> bytes( size_t( std::filesystem::file_size( p ) ) );
            {
                std::ifstream in( p, std::ios::binary );
                in.read( bytes.data(), std::streamsize( bytes.size() ) );
            }
            const auto saved = bytes;
            bytes[bytes.size() / 2] ^= 1;
            {
                std::ofstream out( p, std::ios::binary | std::ios::trunc );
                out.write( bytes.data(), std::streamsize( bytes.size() ) );
            }
            bool rejected = false;
            try
            {
                EventStore::Open( p, fast );
            }
            catch( const std::exception& )
            {
                rejected = true;
            }
            Check( rejected, "changed journal is hashed and rejected" );
            rejected = false;
            try
            {
                EventStore::VerifyContent( p );
            }
            catch( const std::exception& )
            {
                rejected = true;
            }
            Check( rejected, "content verification detects a changed journal" );
            {
                std::ofstream out( p, std::ios::binary | std::ios::trunc );
                out.write( saved.data(), std::streamsize( saved.size() ) );
            }
            Check( bool( EventStore::Open( p, fast ) ), "restored journal is accepted after hashing" );
        }
        {
            auto manifest = EventStore::Directory( p ) / "manifest.json";
            std::ifstream f( manifest, std::ios::binary );
            std::string original( ( std::istreambuf_iterator<char>( f ) ), {} );
            f.close();
            const auto schemaField = "\"schema\":" + std::to_string( EventStoreSchema );
            auto schema = original.find( schemaField );
            Check( schema != std::string::npos, "fixture manifest schema" );
            std::vector<std::string> invalid;
            auto changed = original;
            changed.insert( schema + schemaField.size(), "0" );
            invalid.push_back( changed );
            invalid.push_back( original.substr( 0, original.size() - 1 ) );
            changed = original;
            changed.insert( changed.size() - 1, "," + schemaField );
            invalid.push_back( changed );
            invalid.push_back( "[" + original + "]" );
            changed = original;
            auto source = changed.find( "\"source_sha256\":\"" ) + std::string( "\"source_sha256\":\"" ).size();
            changed[source] = changed[source] == '0' ? '1' : '0';
            invalid.push_back( changed );
            for( const auto& contents : invalid )
            {
                {
                    std::ofstream out( manifest, std::ios::binary | std::ios::trunc );
                    out << contents;
                }
                bool rejected = false;
                try
                {
                    EventStore::Open( p );
                }
                catch( const std::exception& )
                {
                    rejected = true;
                }
                {
                    std::ofstream out( manifest, std::ios::binary | std::ios::trunc );
                    out << original;
                }
                Check( rejected, "invalid, duplicated, wrong-schema or wrong-source manifest must be rejected" );
            }
        }
        {
            const auto dbPath = EventStore::Directory( p ) / "events.sqlite";
            auto hidden = dbPath;
            hidden += ".missing-test";
            std::filesystem::rename( dbPath, hidden );
            bool rejected = false;
            try
            {
                EventStore::Open( p );
            }
            catch( const std::exception& )
            {
                rejected = true;
            }
            std::filesystem::rename( hidden, dbPath );
            Check( rejected, "missing data file rejects the store" );
            char first = 0;
            {
                std::ifstream in( dbPath, std::ios::binary );
                in.get( first );
            }
            {
                std::fstream out( dbPath, std::ios::binary | std::ios::in | std::ios::out );
                out.put( char( first ^ 1 ) );
            }
            rejected = false;
            try
            {
                EventStore::Open( p );
            }
            catch( const std::exception& )
            {
                rejected = true;
            }
            {
                std::fstream out( dbPath, std::ios::binary | std::ios::in | std::ios::out );
                out.put( first );
            }
            Check( rejected, "corrupt data file fails identity validation" );
        }
        bool cancelQuery = false;
        StoreOptions queryOptions;
        queryOptions.cancelled = [&] { return cancelQuery; };
        store = EventStore::Open( p, queryOptions );
        cancelQuery = true;
        bool queryRejected = false;
        try
        {
            store->Zones( 0, 300, {} );
        }
        catch( const std::exception& )
        {
            queryRejected = true;
        }
        cancelQuery = false;
        Check( queryRejected, "cancelled small query must stop without reaching SQLite progress interval" );
        Check( store->Zones( 115, 125, 1 ).values.size() == 2, "query remains usable after cancellation" );
        store.reset();
        auto dense = root / "dense.tracy-stream";
        Fixture( dense, 3000 );
        EventStore::Build( dense );
        StoreOptions bounded;
        bounded.resultBytes = 65536;
        store = EventStore::Open( dense, bounded );
        auto densePage = store->Frames( 0, 10000, 0, 4096 );
        Check( densePage.more && densePage.values.size() * sizeof( StoreFrame ) <= bounded.resultBytes, "frame page respects byte budget and exposes remaining rows" );
        uint64_t denseCount = densePage.values.size();
        while( densePage.more )
        {
            densePage = store->Frames( 0, 10000, densePage.next, 4096 );
            denseCount += densePage.values.size();
        }
        Check( denseCount == store->Info().frames, "byte-limited frame paging preserves every frame" );
        store.reset();
        auto empty = root / "empty.tracy-stream";
        StoreOptions rebuild;
        rebuild.rebuild = true;
        rebuild.cancelled = [] { return true; };
        bool rebuildRejected = false;
        try
        {
            EventStore::Build( p, rebuild );
        }
        catch( const std::exception& )
        {
            rebuildRejected = true;
        }
        Check( rebuildRejected && EventStore::Open( p )->Info().cpuZones == 4, "cancelled rebuild preserves the published store" );
        rebuild.cancelled = {};
        auto rebuilt = EventStore::Build( p, rebuild );
        Check( rebuilt.cpuZones == 4 && EventStore::Open( p )->Zone( page.values[0].id )->end == 200, "successful rebuild preserves logical event identities" );
        Fixture( empty, 0, true );
        auto emptyInfo = EventStore::Build( empty );
        store = EventStore::Open( empty );
        Check( emptyInfo.cpuZones == 0 && emptyInfo.events == 0 && emptyInfo.unresolvedDefinitions == 0, "empty protocol event tables remain valid" );
        Check( store->Zones( INT64_MIN, INT64_MAX, {} ).values.empty() && !store->Zone( 1 ) && store->Children( 1 ).values.empty() && store->StackFrames( 1 ).values.empty() && store->Text( 1 ).values.empty(), "empty queries return no fabricated events" );
        Check( store->FrameSet( 0 )->frames == 2 && store->Frames( INT64_MIN, INT64_MAX ).values.size() == 2, "empty capture retains welcome frame boundaries" );
        store.reset();
        auto partial = root / "partial.tracy-stream";
        std::filesystem::copy_file( p, partial );
        std::filesystem::resize_file( partial, std::filesystem::file_size( partial ) - 7 );
        Check( !EventStore::Build( partial ).cleanEnd, "truncated tail is explicitly incomplete" );
        auto corrupt = root / "corrupt.tracy-stream";
        std::filesystem::copy_file( p, corrupt );
        {
            std::fstream input( corrupt, std::ios::in | std::ios::out | std::ios::binary );
            input.seekp( FileHeaderSize + RecordHeaderSize );
            input.put( '\x77' );
        }
        bool corruptRejected = false;
        try
        {
            EventStore::Build( corrupt );
        }
        catch( const std::exception& )
        {
            corruptRejected = true;
        }
        Check( corruptRejected && !std::filesystem::exists( EventStore::Directory( corrupt ) ), "committed record corruption is rejected without publication" );
        auto cancel = root / "cancel.tracy-stream";
        std::filesystem::copy_file( p, cancel );
        StoreOptions cancelled;
        cancelled.cancelled = [] { return true; };
        bool rejected = false;
        try
        {
            EventStore::Build( cancel, cancelled );
        }
        catch( const std::exception& )
        {
            rejected = true;
        }
        Check( rejected && !std::filesystem::exists( EventStore::Directory( cancel ) ), "cancel does not publish a store" );
        auto disk = root / "disk.tracy-stream";
        std::filesystem::copy_file( p, disk );
        StoreOptions small;
        small.diskBytes = 1;
        rejected = false;
        try
        {
            EventStore::Build( disk, small );
        }
        catch( const std::exception& )
        {
            rejected = true;
        }
        Check( rejected && !std::filesystem::exists( EventStore::Directory( disk ) ), "disk budget failure does not publish a store" );
        {
            std::fstream input( p, std::ios::in | std::ios::out | std::ios::binary );
            input.seekp( 40 );
            input.put( '\x55' );
        }
        rejected = false;
        try
        {
            EventStore::Open( p );
        }
        catch( const std::exception& )
        {
            rejected = true;
        }
        Check( rejected, "changed source identity rejects the old store" );
        const auto misplaced = root / "misplaced.tracy-stream";
        ZoneEventFixture( misplaced, false );
        const auto misplacedInfo = EventStore::Build( misplaced );
        Check( misplacedInfo.cpuZones == 2 && misplacedInfo.openZones == 0 && misplacedInfo.misplacedZoneEvents == 2, "a text and a color for a zone that is not innermost are dropped and counted" );
        auto misplacedStore = EventStore::Open( misplaced );
        Check( misplacedStore->Info().misplacedZoneEvents == 2, "the dropped zone events stay counted in the store" );
        const auto misplacedZones = misplacedStore->Zones( INT64_MIN, INT64_MAX, 5 ).values;
        Check( misplacedZones.size() == 2 && misplacedZones[1].parent == misplacedZones[0].id, "the zones keep their nesting" );
        Check( misplacedZones[0].text == "outer note" && misplacedZones[1].text == "inner note", "a misplaced text is attached to neither zone" );
        misplacedStore.reset();
        const auto misnested = root / "misnested.tracy-stream";
        ZoneEventFixture( misnested, true );
        std::string misnestedError;
        try { EventStore::Build( misnested ); }
        catch( const std::exception& e ) { misnestedError = e.what(); }
        Check( misnestedError.find( "zone end for validation 7" ) != std::string::npos && misnestedError.find( "1 below the top" ) != std::string::npos, "an end for a zone that is not innermost is rejected with its place in the stack" );
        std::cout << "Event store behavior tests passed.\n";
        return 0;
    }
    catch( const std::exception& e )
    {
        std::cerr << "FAIL: " << e.what() << "\nEvidence: " << root.string() << '\n';
        return 1;
    }
}
